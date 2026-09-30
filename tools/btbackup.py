#!/usr/bin/env python3
"""BTRON backup archives on the host side (design 11.19).

A backup archive is the one TAD record a backup volume is: a TS_INFO
segment, the volume head in a 0xFFFD segment, then one 0xFFFD large
segment for each object (or piece of an object) with its name and
F_STATE, and its records compressed with LZSS. Everything is
little-endian, the TC strings too.

  btbackup.py list ARCHIVE...
      The volume heads, and every object with its records. Several
      volumes of one set are given in order; pieces of an object that
      runs on into the next volume are joined by objid.

  btbackup.py extract ARCHIVE... -o DIR
      volume.json, and a directory for each object holding obj.json,
      meta.bin (its 20 TC name and 96 byte F_STATE as the archive has
      them) and one file for each record: rNNNN.tT.sS.bin for data,
      rNNNN.tT.sS.link for the 146 byte link descriptor.

  btbackup.py recompress --check ARCHIVE [--jobs N]
      Every object stream expanded and compressed again; the result must
      be the bytes of the archive.

  btbackup.py pack DIR -o OUT [--capacity BYTES] [--bsize N] [--memo TEXT]
      The objects of an extracted directory written as a backup. With a
      capacity, volumes are cut where the writer of the format cuts them
      and OUT, OUT.2, OUT.3 ... are written.

The codec and the writer follow the design exactly, so an archive that
is extracted and packed again comes back byte for byte.
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------------
# LZSS

RING = 0x2000                   # encoder ring
RMASK = RING - 1
WINDOW = 0xFFE                  # distance limit of the encoder
MAXMATCH = 0x100
DRING = 0x1000                  # decoder ring


class Encoder:
    """The LZSS encoder. put() takes input in any pieces; the output
    handed to the sink does not depend on how it was cut, but when it is
    handed does: blocks go out as the ring fills."""

    def __init__(self, sink):
        self.sink = sink
        self.ring = bytearray(RING)
        self.oldest = 0
        self.cur = WINDOW
        self.wp = WINDOW
        self.outbuf = bytearray(0x24)
        self.outn = 0
        self.delay = WINDOW
        self.head = [0] * 0x400         # oldest position + 1 of each chain
        self.tail = [0] * 0x400         # newest position + 1
        self.chain = [0] * RING         # next newer position + 1

    def _hash(self, p):
        r = self.ring
        return (r[p] + 2 * r[(p + 1) & RMASK] + 2 * r[(p + 2) & RMASK]) & 0x3FF

    def put(self, buf):
        if len(buf) == 0:
            self._encode(True)
            return
        i = 0
        n_all = len(buf)
        while i < n_all:
            if self.oldest == self.wp:
                self._encode(False)
                continue
            if self.oldest >= self.wp:
                n = self.oldest - self.wp
            else:
                n = RING - self.wp
            n = min(n, n_all - i)
            self.ring[self.wp:self.wp + n] = buf[i:i + n]
            i += n
            self.wp = (self.wp + n) & RMASK

    def flush(self):
        self._encode(True)

    def _encode(self, flush):
        ring = self.ring
        head, tail, chain = self.head, self.tail, self.chain
        out = self.outbuf
        cur_s = self.cur
        if cur_s == self.wp:
            return
        while True:
            avail = (self.wp - cur_s) & RMASK
            if flush:
                maxlen = avail
            else:
                maxlen = avail - 3
                if maxlen <= MAXMATCH:
                    return
            best = 0
            dist = 0
            if maxlen > 3:
                if maxlen > MAXMATCH:
                    maxlen = MAXMATCH
                p = head[self._hash(cur_s)] - 1
                while p >= 0:
                    if ring[(cur_s + best) & RMASK] == ring[(p + best) & RMASK]:
                        n = 0
                        if ring[cur_s] == ring[p]:
                            while True:
                                n += 1
                                if n >= maxlen:
                                    break
                                if ring[(cur_s + n) & RMASK] != ring[(p + n) & RMASK]:
                                    break
                        if n > best:
                            dist = cur_s - p
                            best = n
                            if n > 0xFF:
                                break
                    p = chain[p] - 1
            ln = best - 1
            if ln <= 1:
                self.outn += 1
                out[self.outn] = ring[self.cur]
                if self.outn > 0x1F:
                    out[0] = self.outn - 1
                    self.sink(bytes(out[:self.outn + 1]))
                    self.outn = 0
                ln = 0
            else:
                if self.outn > 0:
                    out[0] = self.outn - 1
                else:
                    self.outn = -1
                d = dist
                if d < 0:
                    d += RING
                if ln > 0xE:
                    a = self.outn
                    self.outn += 1
                    out[a + 1] = 0xF0 | (ln >> 4)
                d |= (ln & 0xF) << 12
                a = self.outn
                out[a + 1] = (d >> 8) & 0xFF
                out[a + 2] = d & 0xFF
                self.sink(bytes(out[:a + 3]))
                self.outn = 0
            while True:
                o = self.oldest
                self.oldest += 1
                if self.delay > 0:
                    self.delay -= 1
                else:
                    h = self._hash(o)
                    if head[h] == self.oldest:
                        v = chain[o]
                        head[h] = v
                        if v == 0:
                            tail[h] = 0
                        chain[o] = 0
                self.oldest &= RMASK
                c = self.cur
                self.cur += 1
                h2 = self._hash(c)
                chain[c] = 0
                t = tail[h2]
                if t != 0:
                    chain[t - 1] = self.cur
                else:
                    head[h2] = self.cur
                tail[h2] = self.cur
                self.cur &= RMASK
                ln -= 1
                if ln < 0:
                    break
            cur_s = self.cur
            if cur_s == self.wp:
                if not flush or self.outn <= 0:
                    return
                out[0] = self.outn - 1
                self.sink(bytes(out[:self.outn + 1]))
                self.outn = 0
                return


def compress(data, piece=None):
    """The compressed form of data, put in pieces of `piece` bytes."""
    out = bytearray()
    enc = Encoder(out.extend)
    if piece is None:
        enc.put(data)
    else:
        for i in range(0, len(data), piece):
            enc.put(data[i:i + piece])
    enc.flush()
    return bytes(out)


class LzssError(Exception):
    pass


def decompress(data, outlen=None):
    """Expand a whole stream. With outlen, stop there and fail when the
    stream ends first; without, expand until the input is used up."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n and (outlen is None or len(out) < outlen):
        b0 = data[i]
        i += 1
        if b0 & 0xE0 == 0:
            k = b0 + 1
            if i + k > n:
                out += data[i:n]
                i = n
                break
            out += data[i:i + k]
            i += k
            continue
        if b0 <= 0xEF:
            if i >= n:
                break
            w = (b0 << 8) | data[i]
            i += 1
            ln = (w >> 12) & 0xF
        else:
            if i + 1 >= n:
                break
            w = (data[i] << 8) | data[i + 1]
            i += 2
            ln = ((b0 & 0xF) << 4) + ((w >> 12) & 0xF)
        dist = w & 0xFFF
        pos = len(out)
        src = pos - dist if dist else pos - DRING
        for _ in range(ln + 1):
            out.append(out[src] if src >= 0 else 0)
            src += 1
    if outlen is not None:
        if len(out) < outlen:
            raise LzssError("stream ends at %d of %d bytes" % (len(out), outlen))
        return bytes(out[:outlen])
    return bytes(out)


# ---------------------------------------------------------------------
# The archive

SEG_INFO = 0xFFE0
SEG_BACKUP = 0xFFFD
KIND_VOL = 0xF0
KIND_VOL2 = 0xF2                # the totals passed 0x60000000
KIND_OBJ = 0xF1
VOL_MORE = 0x8000               # vol / seg: continued in the next volume
OBJ_META = 0x90                 # object segment after its length: 8 + 40 + 96
REC_HEAD = 16
LINK_DESC = 0x92                # atr[5], objid, F_LINK
HEAD_FIXED = 42                 # TS_INFO 10 + segment head 4 + 28
MEMO_BYTES = 160
BIG_TOTAL = 0x60000000
TAD_VERSION = 0x0121

ER_FULL = "full"


class ArchiveError(Exception):
    pass


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def tc_list(b):
    out = []
    for i in range(0, len(b) - 1, 2):
        c = u16(b, i)
        if c == 0:
            break
        out.append(c)
    return out


def tc_to_str(b):
    s = []
    for c in tc_list(b):
        if 0x2321 <= c <= 0x237E:
            s.append(chr(c - 0x2300))
        elif 0x2121 <= c <= 0x7E7E:
            try:
                s.append(bytes([(c >> 8) | 0x80, (c & 0xFF) | 0x80]).decode("euc_jp"))
            except UnicodeDecodeError:
                s.append("<%04X>" % c)
        elif 0x20 <= c < 0x7F:
            s.append(chr(c))
        else:
            s.append("<%04X>" % c)
    return "".join(s)


def str_to_tc(s, n):
    """A text as n TC, 0 filled; JIS X 0208 where the character is there."""
    out = []
    for ch in s:
        try:
            e = ch.encode("euc_jp")
        except UnicodeEncodeError:
            e = b""
        if len(e) == 2 and e[0] >= 0xA1 and e[1] >= 0xA1:
            out.append(((e[0] & 0x7F) << 8) | (e[1] & 0x7F))
        elif len(e) == 1 and 0x21 <= e[0] <= 0x7E:
            out.append(0x2300 + e[0])
        elif ch == " ":
            out.append(0x2121)
        else:
            out.append(0x2222)
    out = out[:n] + [0] * (n - min(n, len(out)))
    return struct.pack("<%dH" % n, *out)


FSTATE_FIELDS = ("f_type", "f_atype", "f_grpacc", "f_pubacc", "f_nlink",
                 "f_index", "f_size", "f_nblk", "f_nrec", "f_ltime",
                 "f_atime", "f_mtime", "f_ctime")


def fstate_parse(b):
    st = {"f_type": u16(b, 0), "f_atype": u16(b, 2),
          "f_owner": b[4:32].hex(), "f_group": b[32:60].hex()}
    st["f_grpacc"], st["f_pubacc"], st["f_nlink"], st["f_index"] = \
        struct.unpack_from("<HHhh", b, 0x3C)
    (st["f_size"], st["f_nblk"], st["f_nrec"], st["f_ltime"],
     st["f_atime"], st["f_mtime"], st["f_ctime"]) = struct.unpack_from("<7I", b, 0x44)
    return st


def fstate_build(st):
    b = bytearray(96)
    struct.pack_into("<HH", b, 0, st.get("f_type", 0x1000), st.get("f_atype", 0))
    b[4:32] = bytes.fromhex(st.get("f_owner", "00" * 28))
    b[32:60] = bytes.fromhex(st.get("f_group", "00" * 28))
    struct.pack_into("<HHhh", b, 0x3C, st.get("f_grpacc", 0), st.get("f_pubacc", 0x0FFF),
                     st.get("f_nlink", 0), st.get("f_index", 0))
    struct.pack_into("<7I", b, 0x44, st.get("f_size", 0), st.get("f_nblk", 0),
                     st.get("f_nrec", 0), st.get("f_ltime", 0xFFFFFFFF),
                     st.get("f_atime", 0), st.get("f_mtime", 0), st.get("f_ctime", 0))
    return bytes(b)


def link_parse(b):
    atr = struct.unpack_from("<5H", b, 0)
    ref = b[14:]
    return {"atr": list(atr), "objid": u32(b, 10),
            "f_ctime": u32(ref, 0), "f_atype": u16(ref, 4),
            "f_name": tc_to_str(ref[6:46]), "f_id": u16(ref, 0x2E),
            "rf_ctime": u32(ref, 0x30), "fs_name": tc_to_str(ref[0x34:0x5C]),
            "fs_locat": tc_to_str(ref[0x5C:0x84])}


class Record:
    """One piece of a record as the stream has it."""

    def __init__(self, recno, rtype, subtype, offset, data):
        self.recno = recno
        self.type = rtype
        self.subtype = subtype
        self.offset = offset
        self.data = data


class Piece:
    """One object segment: an object, or the part of one a volume holds."""

    def __init__(self, pos, llen, cmp, kind, seg, objid, meta, stream):
        self.pos = pos
        self.llen = llen
        self.cmp = cmp
        self.kind = kind
        self.seg = seg
        self.objid = objid
        self.meta = meta                # name 40 + F_STATE 96
        self.name = tc_to_str(meta[:40])
        self.st = fstate_parse(meta[40:])
        self.stream = stream            # the stored bytes (compressed if cmp)
        self._records = None

    @property
    def records(self):
        """The record pieces, expanded when first asked for."""
        if self._records is None:
            plain = decompress(self.stream) if self.cmp else self.stream
            self._records = parse_records(plain)
        return self._records


class Volume:
    def __init__(self, data, lazy=False):
        self.data = data
        self.head = None
        self.memo = b""
        self.pieces = []
        self.parse(lazy)

    def parse(self, lazy):
        d = self.data
        pos = 0
        while True:
            if pos + 4 > len(d):
                raise ArchiveError("no volume head")
            sid, ln = u16(d, pos), u16(d, pos + 2)
            if sid == SEG_INFO and ln != 0xFFFF:
                pos += 4 + ln
                continue
            break
        if sid != SEG_BACKUP or ln == 0xFFFF or ln < 0x1C or pos + 4 + ln > len(d):
            raise ArchiveError("bad volume head")
        h = d[pos + 4:pos + 4 + 0x1C]
        if h[1] not in (KIND_VOL, KIND_VOL2):
            raise ArchiveError("volume kind 0x%02X" % h[1])
        self.head_raw = d[:pos + 4 + 0x1C]
        vol = u16(h, 2)
        total_lo, nobj, src_lo, total_hi, src_hi = struct.unpack_from("<IIIHH", h, 6)
        self.head = {"cmp": h[0], "kind": h[1], "vol": vol & 0x7FFF,
                     "more": bool(vol & VOL_MORE), "rsv": u16(h, 4),
                     "total": total_lo | ((total_hi << 32) if h[1] != KIND_VOL else 0),
                     "nobj": nobj,
                     "src": src_lo | ((src_hi << 32) if h[1] != KIND_VOL else 0),
                     "total_lo": total_lo, "total_hi": total_hi,
                     "src_lo": src_lo, "src_hi": src_hi, "pad": h[22:28].hex()}
        self.memo = d[pos + 4 + 0x1C:pos + 4 + ln]
        pos += 4 + ln
        while pos < len(d):
            if pos + 4 > len(d):
                raise ArchiveError("cut segment at 0x%X" % pos)
            sid, ln = u16(d, pos), u16(d, pos + 2)
            if sid != SEG_BACKUP:
                raise ArchiveError("segment 0x%04X at 0x%X" % (sid, pos))
            hl = 4
            if ln == 0xFFFF:
                ln = u32(d, pos + 4)
                hl = 8
            if ln < OBJ_META or pos + hl + ln > len(d):
                raise ArchiveError("object segment at 0x%X: length %d" % (pos, ln))
            h = d[pos + hl:pos + hl + OBJ_META]
            cmp, kind, seg, objid = struct.unpack_from("<BBHI", h, 0)
            if kind != KIND_OBJ or cmp > 1:
                raise ArchiveError("object at 0x%X: cmp %d kind 0x%02X" % (pos, cmp, kind))
            stream = d[pos + hl + OBJ_META:pos + hl + ln]
            p = Piece(pos, ln, cmp, kind, seg, objid, h[8:], stream)
            if not lazy:
                p.records
            self.pieces.append(p)
            pos += hl + ln


def parse_records(plain):
    recs = []
    i = 0
    while i < len(plain):
        if i + REC_HEAD > len(plain):
            raise ArchiveError("record head cut at %d" % i)
        recno, rtype, sub, off, n = struct.unpack_from("<iHHii", plain, i)
        i += REC_HEAD
        if n < 0 or i + n > len(plain):
            raise ArchiveError("record %d: length %d" % (recno, n))
        if rtype == 0 and (n != LINK_DESC or off != 0):
            raise ArchiveError("record %d: link of %d bytes" % (recno, n))
        recs.append(Record(recno, rtype, sub, off, plain[i:i + n]))
        i += n
    return recs


class Object:
    """An object made whole from its pieces in the volumes of a set."""

    def __init__(self, piece):
        self.objid = piece.objid
        self.meta = piece.meta
        self.name = piece.name
        self.st = piece.st
        self.pieces = []
        self.records = []               # [type, subtype, bytearray | link bytes]
        self.nrec_done = 0
        self.recoff = 0
        self.complete = False


def join(volumes, loose=False):
    """The objects of a set of volumes given in order, checking what the
    restore checks: the volume numbers, and the record numbers and
    offsets of the pieces of each object. With loose, the first volume
    may be a later one of its set, and a piece whose earlier pieces are
    not there is taken as it stands."""
    objs = []
    byid = {}
    first = volumes[0].head["vol"] if loose and volumes else 0
    for vi, v in enumerate(volumes):
        if v.head["vol"] != first + vi:
            raise ArchiveError("volume %d says it is volume %d" % (vi + 1, v.head["vol"] + 1))
        if vi + 1 < len(volumes) and not v.head["more"]:
            raise ArchiveError("volume %d is the last of its set" % (vi + 1))
        for p in v.pieces:
            adopt = False
            if p.seg & 0x7FFF == 0:
                o = Object(p)
                objs.append(o)
                byid[p.objid] = o
            else:
                o = byid.get(p.objid)
                if o is None or o.complete:
                    if not loose:
                        raise ArchiveError("piece %d of 0x%08X has nothing before it"
                                           % (p.seg & 0x7FFF, p.objid))
                    o = Object(p)
                    objs.append(o)
                    byid[p.objid] = o
                    adopt = True
            o.pieces.append(p)
            nrec = o.st["f_nrec"]
            for r in p.records:
                if adopt:
                    adopt = False
                    o.nrec_done = r.recno + 1 if r.offset else r.recno
                    o.recoff = r.offset
                    if r.offset:
                        o.records.append([r.type, r.subtype, bytearray()])
                if o.nrec_done == r.recno:
                    if r.offset != 0:
                        raise ArchiveError("0x%08X record %d starts at %d"
                                           % (o.objid, r.recno, r.offset))
                    o.recoff = 0
                    o.nrec_done = r.recno + 1
                    o.records.append([r.type, r.subtype, bytearray()])
                elif (o.nrec_done == r.recno + 1 and r.offset != 0
                      and r.offset == o.recoff):
                    pass
                else:
                    raise ArchiveError("0x%08X record %d at %d out of order"
                                       % (o.objid, r.recno, r.offset))
                if o.nrec_done > nrec:
                    raise ArchiveError("0x%08X has more than %d records" % (o.objid, nrec))
                o.records[-1][2] += r.data
                o.recoff += len(r.data)
            if not p.seg & VOL_MORE:
                if o.nrec_done != nrec:
                    raise ArchiveError("0x%08X ends after %d of %d records"
                                       % (o.objid, o.nrec_done, nrec))
                o.complete = True
    return objs


def load(paths, lazy=False):
    vols = []
    for p in paths:
        with open(p, "rb") as f:
            vols.append(Volume(f.read(), lazy))
    return vols


# ---------------------------------------------------------------------
# The writer

def estimate(st, bsize):
    """What one object adds to the totals of the volume head."""
    return (st["f_size"] + 16 * st["f_nrec"] + 0x98 + 146 * st["f_nlink"],
            bsize * st["f_nblk"])


class Writer:
    """Volumes written as the format's writer writes them.

    `objs` is a list of (objid, meta 136 bytes, records), each record a
    tuple (type, subtype, bytes); a link record's bytes are its 146 byte
    descriptor. `capacity` is what one volume may hold (None: no limit);
    the writer stops an object where the next piece would not fit, in
    the same place the format's writer does, so that the volumes are
    byte for byte the same.
    """

    def __init__(self, objs, total, nobj, src, memo, capacity=None):
        self.objs = objs
        self.total = total
        self.nobj = nobj
        self.src = src
        self.memo = memo[:MEMO_BYTES].ljust(MEMO_BYTES, b"\0")
        self.capacity = capacity
        self.cur = 0
        self.resume_rec = 0
        self.resume_off = 0
        self.segno = 0

    def volumes(self):
        vols = []
        while self.cur < len(self.objs) or not vols:
            at = (self.cur, self.resume_rec, self.resume_off)
            vols.append(self.volume(len(vols)))
            if len(vols) > 1 and at == (self.cur, self.resume_rec, self.resume_off):
                raise ArchiveError("a volume of %d bytes holds nothing" % self.capacity)
        return vols

    def head(self, vol, more):
        h = bytearray(44)
        struct.pack_into("<5H", h, 0, SEG_INFO, 6, 0, 2, TAD_VERSION)
        struct.pack_into("<HH", h, 10, SEG_BACKUP, 4 + 0x1C + MEMO_BYTES - 4)
        h[14] = 0
        h[15] = KIND_VOL2 if self.total > BIG_TOTAL else KIND_VOL
        struct.pack_into("<H", h, 16, vol | (VOL_MORE if more else 0))
        struct.pack_into("<IIIHH", h, 20, self.total & 0xFFFFFFFF, self.nobj,
                         self.src & 0xFFFFFFFF, (self.total >> 32) & 0xFFFF,
                         (self.src >> 32) & 0xFFFF)
        return bytes(h[:HEAD_FIXED])

    def _raw(self, b):
        self.out += b
        self.free -= len(b)
        self.objbytes += len(b)

    def volume(self, vol):
        self.out = bytearray()
        self.free = self.capacity if self.capacity is not None else 1 << 62
        self.objbytes = 0
        self._raw(self.head(vol, False))
        self._raw(self.memo)
        while self.cur < len(self.objs):
            r = self.object()
            if r == ER_FULL:
                self.out[0:HEAD_FIXED] = self.head(vol, True)
                break
            self.cur += 1
        return bytes(self.out)

    def _room(self, need):
        f = self.free
        return min(need, max(f - f // 32, 0))

    def object(self):
        if self.free <= 0x3FF:
            return ER_FULL
        objid, meta, recs = self.objs[self.cur]
        hdrpos = len(self.out)
        oh = bytearray(16)
        struct.pack_into("<HHIBBHI", oh, 0, SEG_BACKUP, 0xFFFF, 0, 1, KIND_OBJ, 0, objid)
        self._raw(oh)
        self._raw(meta)
        enc = Encoder(self._raw)
        self.objbytes = OBJ_META
        split = False
        while self.resume_rec < len(recs):
            if self.record(enc, recs[self.resume_rec]) == ER_FULL:
                split = True
                break
            self.resume_rec += 1
        enc.flush()
        llen = self.objbytes
        if split:
            seg = self.segno | VOL_MORE
            self.segno += 1
        else:
            seg = self.segno
            self.segno = 0
            self.resume_rec = 0
            self.resume_off = 0
        struct.pack_into("<IBBH", oh, 4, llen, 1, KIND_OBJ, seg)
        self.out[hdrpos:hdrpos + 16] = oh
        return ER_FULL if split else None

    def record(self, enc, rec):
        rtype, sub, data = rec
        if rtype == 0:
            if self._room(REC_HEAD + LINK_DESC) < REC_HEAD + LINK_DESC:
                return ER_FULL
            enc.put(struct.pack("<iHHii", self.resume_rec, 0, sub, 0, LINK_DESC))
            enc.put(data[:LINK_DESC])
            return None
        rest = len(data) - self.resume_off
        while True:
            n = self._room(rest + REC_HEAD) - REC_HEAD
            if n < rest and n <= 0x3FF:
                return ER_FULL
            enc.put(struct.pack("<iHHii", self.resume_rec, rtype, sub, self.resume_off, n))
            at = self.resume_off
            k = 0
            while k < n:
                m = min(0x800, n - k)
                enc.put(data[at + k:at + k + m])
                k += m
            rest -= n
            if rest > 0:
                self.resume_off += n
            else:
                break
        self.resume_off = 0
        return None


def rewrite(volumes):
    """Every volume of a set written again from what was read out of it."""
    objs = join(volumes)
    h = volumes[0].head
    items = []
    for o in objs:
        recs = [(t, s, bytes(d)) for t, s, d in o.records]
        items.append((o.objid, o.meta, recs))
    return items, h


# ---------------------------------------------------------------------
# The commands

def cmd_list(args):
    vols = load(args.archive, lazy=not args.records)
    for vi, v in enumerate(vols):
        h = v.head
        print("volume %d%s  kind 0x%02X  objects %d  total %d  src %d  memo '%s'"
              % (h["vol"] + 1, " (continues)" if h["more"] else "", h["kind"],
                 h["nobj"], h["total"], h["src"], tc_to_str(v.memo)))
        for p in v.pieces:
            print("  0x%08X seg %d%s @0x%X llen %d  '%s'  atype 0x%04X nrec %d nlink %d "
                  "size %d  stream %d"
                  % (p.objid, p.seg & 0x7FFF, "+" if p.seg & VOL_MORE else "",
                     p.pos, p.llen, p.name, p.st["f_atype"], p.st["f_nrec"],
                     p.st["f_nlink"], p.st["f_size"], len(p.stream)))
            if args.records:
                for r in p.records:
                    extra = ""
                    if r.type == 0:
                        lk = link_parse(r.data)
                        extra = " -> 0x%08X '%s'" % (lk["objid"], lk["f_name"])
                    print("      rec %d type %d sub %d off %d len %d%s"
                          % (r.recno, r.type, r.subtype, r.offset, len(r.data), extra))
    if args.records or args.loose:
        objs = join(vols, args.loose)
        print("%d objects, %d complete" % (len(objs), sum(o.complete for o in objs)))
    return 0


def cmd_extract(args):
    vols = load(args.archive, lazy=True)
    objs = join(vols, args.loose)
    os.makedirs(args.output, exist_ok=True)
    h = dict(vols[0].head)
    h["memo"] = vols[0].memo.hex()
    h["memo_text"] = tc_to_str(vols[0].memo)
    h["volumes"] = len(vols)
    with open(os.path.join(args.output, "volume.json"), "w", encoding="utf-8") as f:
        json.dump(h, f, ensure_ascii=False, indent=1)
    for i, o in enumerate(objs):
        d = os.path.join(args.output, "%04d_%08x" % (i, o.objid))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "meta.bin"), "wb") as f:
            f.write(o.meta)
        info = {"objid": "0x%08X" % o.objid, "name": o.name, "complete": o.complete}
        info.update(o.st)
        with open(os.path.join(d, "obj.json"), "w", encoding="utf-8") as f:
            json.dump(info, f, ensure_ascii=False, indent=1)
        for n, (t, s, data) in enumerate(o.records):
            ext = "link" if t == 0 else "bin"
            with open(os.path.join(d, "r%04d.t%d.s%d.%s" % (n, t, s, ext)), "wb") as f:
                f.write(data)
    print("%d objects in %s" % (len(objs), args.output))
    return 0


def _check_one(stream):
    plain = decompress(stream)
    return compress(plain) == stream, len(plain)


def cmd_recompress(args):
    vols = load(args.archive, lazy=True)
    streams = [(p.objid, p.stream) for v in vols for p in v.pieces if p.cmp]
    bad = 0
    total = 0
    if args.jobs > 1:
        import multiprocessing
        with multiprocessing.Pool(args.jobs) as pool:
            res = pool.map(_check_one, [s for _, s in streams], chunksize=4)
    else:
        res = [_check_one(s) for _, s in streams]
    for (objid, _), (ok, n) in zip(streams, res):
        total += n
        if not ok:
            bad += 1
            print("0x%08X differs" % objid)
    print("%d streams, %d bytes expanded, %d differ" % (len(streams), total, bad))
    return 1 if bad else 0


def read_dir(path, bsize):
    """The objects of an extracted directory, and the totals of a head."""
    objs = []
    total = src = 0
    for name in sorted(os.listdir(path)):
        d = os.path.join(path, name)
        if not os.path.isdir(d):
            continue
        with open(os.path.join(d, "obj.json"), encoding="utf-8") as f:
            info = json.load(f)
        recs = []
        for fn in sorted(os.listdir(d)):
            if not fn.startswith("r") or not (fn.endswith(".bin") or fn.endswith(".link")):
                continue
            parts = fn.split(".")
            t = int(parts[1][1:])
            s = int(parts[2][1:])
            with open(os.path.join(d, fn), "rb") as f:
                recs.append((t, s, f.read()))
        mp = os.path.join(d, "meta.bin")
        if os.path.exists(mp):
            with open(mp, "rb") as f:
                meta = f.read()
        else:
            st = dict(info)
            st["f_nrec"] = len(recs)
            st["f_nlink"] = sum(1 for r in recs if r[0] == 0)
            st["f_size"] = sum(len(r[2]) for r in recs if r[0] != 0)
            st.setdefault("f_nblk", 1 + (st["f_size"] + bsize - 1) // bsize)
            meta = str_to_tc(info.get("name", ""), 20) + fstate_build(st)
        st = fstate_parse(meta[40:])
        t, s = estimate(st, bsize)
        total += t
        src += s
        objs.append((int(info["objid"], 0), meta, recs))
    return objs, total, src


def cmd_pack(args):
    objs, total, src = read_dir(args.dir, args.bsize)
    memo = b""
    vp = os.path.join(args.dir, "volume.json")
    nobj = len(objs)
    if os.path.exists(vp):
        with open(vp, encoding="utf-8") as f:
            h = json.load(f)
        memo = bytes.fromhex(h.get("memo", ""))
        if not args.recompute:
            total, nobj, src = h["total"], h["nobj"], h["src"]
    if args.memo is not None:
        memo = str_to_tc(args.memo, 80)
    w = Writer(objs, total, nobj, src, memo, args.capacity)
    vols = w.volumes()
    for i, v in enumerate(vols):
        out = args.output if i == 0 else "%s.%d" % (args.output, i + 1)
        with open(out, "wb") as f:
            f.write(v)
    print("%d objects, %d volume(s)" % (len(objs), len(vols)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="BTRON backup archives")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("archive", nargs="+")
    p.add_argument("-r", "--records", action="store_true")
    p.add_argument("--loose", action="store_true",
                   help="a later volume of a set without the ones before it")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("extract")
    p.add_argument("archive", nargs="+")
    p.add_argument("--loose", action="store_true")
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(fn=cmd_extract)
    p = sub.add_parser("recompress")
    p.add_argument("archive", nargs="+")
    p.add_argument("--check", action="store_true", required=True)
    p.add_argument("--jobs", type=int, default=1)
    p.set_defaults(fn=cmd_recompress)
    p = sub.add_parser("pack")
    p.add_argument("dir")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--capacity", type=int, default=None)
    p.add_argument("--bsize", type=int, default=32768)
    p.add_argument("--memo", default=None)
    p.add_argument("--recompute", action="store_true",
                   help="totals from the objects, not from volume.json")
    p.set_defaults(fn=cmd_pack)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (ArchiveError, LzssError) as e:
        print("btbackup: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
