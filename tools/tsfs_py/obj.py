"""The object block and the parts an object has (design 11.6.6).

The object block, 4096 bytes, with the common head ("TFOB", owner = the
low 64 bits of the object's UUID):

     64  UUID                        80  flags (32)
     84  reference count (32)        88  placement entries (32)
     92  resource entries (32)       96  link entries (32)
    100  the next rid (32)
    104  made, 112 updated, 120 read: 64 bit ns since 1985-01-01 UTC
    128  place of the metadata text  160  place of the placement table
    192  place of the resource table 224  place of the link table
    256  which 64 byte units of the inline area are taken (56 bits)
    264  holds (32): the part of the reference count no link table of
         the volume makes (the superblock's roots, links from elsewhere)
    512  the inline area, 56 units of 64 bytes

The reference count is the number of link table entries of the volume
that point at the object and are not external, plus the holds.

A place (32 bytes): form (8 bits at 0: none, inline, run, tree), bytes
(64 at 8), then for inline the offset in the block and the units (16 bits
each at 16 and 18), for a run the first block and its length (64 each at
16 and 24), for a tree the leaf block and the number of runs in it.

Management parts (metadata, the three tables) are inline or a run of
blocks, each with the common head ("TFTB") and 4032 bytes of payload.
Data parts (records, resources) are inline when 2048 bytes or less and
there is room, else plain blocks as one run, else an extent tree: one
"TFTB" leaf with the count at 64 and from 128 up to 165 entries of
(first block within the part, block on the volume, blocks), 64 each.

Placement table entry (48): rid, kind (0 xtad, 1 bin), bytes, place.
Resource table entry (64): owner (rid; 0xFFFFFFFF for the icon; the top
bit and a position for one written before its record), number, file
name extension (16, NUL padded), bytes, place.
Link table entry (48): vobjid (all zero when the link has none), target
UUID, rid of the record holding the link, flags (bit 0: the target was
not in the object index when the table was made, and is not counted),
8 reserved. The entries are in the order of their first 40 bytes taken
as unsigned bytes, the rid and the flags little endian as they lie.
"""

import struct
import uuid

from . import head
from .error import TsfsError
from .layout import (BLOCK_SIZE, HDR_SIZE, MAGIC_OBJ, MAGIC_TBL, owner_of)

OB_UUID = 64
OB_FLAGS = 80
OB_REFCNT = 84
OB_NPLACE = 88
OB_NRES = 92
OB_NLINK = 96
OB_NEXTRID = 100
OB_MADE = 104
OB_UPDATED = 112
OB_READ = 120
OB_META = 128
OB_PLACE = 160
OB_RES = 192
OB_LINK = 224
OB_INLMAP = 256
OB_PINS = 264
OB_ICON = 288                           # the icon, a management part like the metadata
OB_INLINE = 512

HEAD_SIZE = 512
INLINE_BYTES = 3584
INLINE_UNIT = 64
INL_UNITS = INLINE_BYTES // INLINE_UNIT
INL_MASK = (1 << INL_UNITS) - 1

F_EDITABLE = 0x0001
F_DELETABLE = 0x0002
F_READABLE = 0x0004
F_GARBAGE = 0x0100
F_ORPHAN = 0x0200
F_RELINK = 0x0400
F_DEFAULT = F_EDITABLE | F_DELETABLE | F_READABLE

FORM_NONE = 0
FORM_INLINE = 1
FORM_RUN = 2
FORM_TREE = 3
FORM_NAMES = {FORM_NONE: "none", FORM_INLINE: "inline", FORM_RUN: "run",
              FORM_TREE: "tree"}

PL_SIZE = 32
PE_SIZE = 48
RS_SIZE = 64
LK_SIZE = 48

REC_XTAD = 0
REC_BIN = 1

RID_ICON = 0xFFFFFFFF
RID_PLACE = 0x80000000
ICON_REC = -1

LK_EXTERNAL = 0x0001
LK_KEY = 40                             # the bytes the entries are ordered by
MAX_LINK = 1024

MB_PAY = BLOCK_SIZE - HDR_SIZE          # payload of a management block
DATA_INLINE_MAX = 2048
MAX_EXTENTS = 165
XL_N = 64
XL_ENT = 128
XL_ESZ = 24

MAX_REC = 64
MAX_RES = 64
EXT_LEN = 16
META_MAX = 65536
ICON_MAX = 65536


def is_icon(data):
    """An icon is an ICO or a PNG."""
    return (data[:4] == b"\0\0\1\0" and len(data) >= 6) or data[:8] == b"\x89PNG\r\n\x1a\n"


PART_NAMES = {OB_META: "metadata", OB_PLACE: "placement table",
              OB_RES: "resource table", OB_LINK: "link table",
              OB_ICON: "icon"}


def blks_for(n, per):
    return (n + per - 1) // per


# ---------------------------------------------------------------- places

class Place(object):
    """Where the bytes of a part are."""

    __slots__ = ("form", "nbytes", "a", "b")

    def __init__(self, form=FORM_NONE, nbytes=0, a=0, b=0):
        self.form = form
        self.nbytes = nbytes
        self.a = a
        self.b = b

    def pack(self):
        buf = bytearray(PL_SIZE)
        buf[0] = self.form
        struct.pack_into("<Q", buf, 8, self.nbytes)
        if self.form == FORM_INLINE:
            struct.pack_into("<HH", buf, 16, self.a, self.b)
        elif self.form in (FORM_RUN, FORM_TREE):
            struct.pack_into("<QQ", buf, 16, self.a, self.b)
        return bytes(buf)

    @classmethod
    def unpack(cls, buf, off=0):
        form = buf[off]
        nbytes = struct.unpack_from("<Q", buf, off + 8)[0]
        if form == FORM_INLINE:
            a, b = struct.unpack_from("<HH", buf, off + 16)
        else:
            a, b = struct.unpack_from("<QQ", buf, off + 16)
        return cls(form, nbytes, a, b)

    def units(self):
        """(first unit, units) of an inline place."""
        return (self.a - OB_INLINE) // INLINE_UNIT, self.b

    def __repr__(self):
        return "<%s %d bytes %d/%d>" % (FORM_NAMES.get(self.form, self.form),
                                        self.nbytes, self.a, self.b)


class PlaceEntry(object):
    __slots__ = ("rid", "kind", "nbytes", "place")

    def __init__(self, rid, kind, nbytes=0, place=None):
        self.rid = rid
        self.kind = kind
        self.nbytes = nbytes
        self.place = place or Place()

    def pack(self):
        return (struct.pack("<IIQ", self.rid, self.kind, self.nbytes)
                + self.place.pack())

    @classmethod
    def unpack(cls, buf, off):
        rid, kind, nbytes = struct.unpack_from("<IIQ", buf, off)
        return cls(rid, kind, nbytes, Place.unpack(buf, off + 16))


class ResEntry(object):
    __slots__ = ("owner", "resno", "ext", "nbytes", "place", "ext_raw")

    def __init__(self, owner, resno, ext, nbytes=0, place=None):
        self.owner = owner
        self.resno = resno              # signed, as the kernel's INT
        self.ext = ext                  # text
        self.nbytes = nbytes
        self.place = place or Place()
        self.ext_raw = None

    def pack(self):
        raw = self.ext.encode("utf-8")[:EXT_LEN - 1]
        return (struct.pack("<Ii", self.owner, self.resno)
                + raw + b"\0" * (EXT_LEN - len(raw))
                + struct.pack("<Q", self.nbytes) + self.place.pack())

    @classmethod
    def unpack(cls, buf, off):
        owner, resno = struct.unpack_from("<Ii", buf, off)
        raw = bytes(buf[off + 8:off + 8 + EXT_LEN])
        ext = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        nbytes = struct.unpack_from("<Q", buf, off + 24)[0]
        e = cls(owner, resno, ext, nbytes, Place.unpack(buf, off + 32))
        e.ext_raw = raw
        return e


class LinkEntry(object):
    __slots__ = ("vobjid", "target", "rid", "flags", "reserved")

    def __init__(self, vobjid, target, rid, flags=0, reserved=0):
        self.vobjid = vobjid
        self.target = target
        self.rid = rid
        self.flags = flags
        self.reserved = reserved

    def pack(self):
        return (self.vobjid.bytes + self.target.bytes
                + struct.pack("<IIQ", self.rid, self.flags, self.reserved))

    @classmethod
    def unpack(cls, buf, off):
        v = uuid.UUID(bytes=bytes(buf[off:off + 16]))
        t = uuid.UUID(bytes=bytes(buf[off + 16:off + 32]))
        rid, flags, res = struct.unpack_from("<IIQ", buf, off + 32)
        return cls(v, t, rid, flags, res)

    def key(self):
        """The bytes the table is ordered by."""
        return self.pack()[:LK_KEY]

    @property
    def counted(self):
        return not self.flags & LK_EXTERNAL

    def __repr__(self):
        return "<link %s -> %s rid %d flags %x>" % (self.vobjid, self.target,
                                                    self.rid, self.flags)


def sort_links(entries):
    """A link table in its order; entries with the same key keep the
    order they came in."""
    return sorted(entries, key=lambda e: e.key())


def pack_table(entries):
    return b"".join(e.pack() for e in entries)


def unpack_table(raw, cls, size):
    return [cls.unpack(raw, i * size) for i in range(len(raw) // size)]


# ---------------------------------------------------------------- the block

class ObjectBlock(object):
    """An object block held in memory, with the kernel's rules for the
    inline area and the parts. Reading never changes the volume; the
    writing calls take blocks through vol.alloc_ext / vol.free_ext and
    write the blocks of the parts at once, the object block itself only
    on store()."""

    def __init__(self, vol, blk, obj_uuid, hdr=None):
        self.vol = vol
        self.blk = blk
        self.uuid = obj_uuid
        self.owner = owner_of(obj_uuid)
        self.hdr = bytearray(hdr) if hdr is not None else bytearray(BLOCK_SIZE)

    @classmethod
    def new(cls, vol, blk, obj_uuid, when=0, flags=F_DEFAULT):
        o = cls(vol, blk, obj_uuid)
        o.hdr[OB_UUID:OB_UUID + 16] = obj_uuid.bytes
        o.flags = flags
        o.nextrid = 1
        o.made = o.updated = o.read = when
        return o

    @classmethod
    def load(cls, vol, blk, obj_uuid=None):
        """The block, checked as obj_load checks it (and its owner)."""
        buf = vol.read_block(blk)
        u = uuid.UUID(bytes=bytes(buf[OB_UUID:OB_UUID + 16]))
        why = head.problem(buf, vol.sb.vol_uuid, MAGIC_OBJ, blk, owner_of(u))
        if why is not None:
            raise TsfsError("object block %d: %s" % (blk, why))
        if obj_uuid is not None and u != obj_uuid:
            raise TsfsError("object block %d holds %s, not %s"
                           % (blk, u, obj_uuid))
        return cls(vol, blk, u, buf)

    def store(self):
        buf = head.seal(self.hdr, self.vol.sb.vol_uuid, self.vol.sb.journal_seq,
                        MAGIC_OBJ, self.blk, self.owner)
        self.vol.write_block(self.blk, buf)

    # ------------------------------------------------------------ fields

    def _u32(off):
        return property(lambda self: struct.unpack_from("<I", self.hdr, off)[0],
                        lambda self, v: struct.pack_into("<I", self.hdr, off,
                                                         v & 0xFFFFFFFF))

    def _u64(off):
        return property(lambda self: struct.unpack_from("<Q", self.hdr, off)[0],
                        lambda self, v: struct.pack_into("<Q", self.hdr, off, v))

    flags = _u32(OB_FLAGS)
    refcnt = _u32(OB_REFCNT)
    nplace = _u32(OB_NPLACE)
    nres = _u32(OB_NRES)
    nlink = _u32(OB_NLINK)
    nextrid = _u32(OB_NEXTRID)
    made = _u64(OB_MADE)
    updated = _u64(OB_UPDATED)
    read = _u64(OB_READ)
    inlmap = _u64(OB_INLMAP)
    pins = _u32(OB_PINS)
    del _u32, _u64

    @property
    def refcnt_signed(self):
        """The count as the kernel reads it, a signed 32 bit value."""
        v = self.refcnt
        return v - (1 << 32) if v & 0x80000000 else v

    def place(self, off):
        return Place.unpack(self.hdr, off)

    def set_place(self, off, pl):
        self.hdr[off:off + PL_SIZE] = pl.pack()

    # ------------------------------------------------------------ inline area

    def inl_take(self, nbytes):
        """Room for nbytes in the inline area, first fit: its offset in the
        block, or -1."""
        units = blks_for(nbytes, INLINE_UNIT)
        m = self.inlmap
        if nbytes <= 0 or units > INL_UNITS:
            return -1
        u = 0
        while u + units <= INL_UNITS:
            k = 0
            while k < units and not (m & (1 << (u + k))):
                k += 1
            if k == units:
                self.inlmap = m | (((1 << units) - 1) << u)
                return OB_INLINE + u * INLINE_UNIT
            u += k + 1
        return -1

    def inl_give(self, off, units):
        u = (off - OB_INLINE) // INLINE_UNIT
        m = self.inlmap
        for k in range(units):
            m &= ~(1 << (u + k))
        self.inlmap = m

    # ------------------------------------------------------------ reading

    def read_mgmt(self, off_or_place, check=None):
        """A management part whole. `check`, when given, is called with
        (block, problem) for each block of a run that does not pass, and
        the part is read on; otherwise such a block raises."""
        pl = (off_or_place if isinstance(off_or_place, Place)
              else self.place(off_or_place))
        if pl.form == FORM_NONE:
            return b""
        if pl.form == FORM_INLINE:
            return bytes(self.hdr[pl.a:pl.a + pl.nbytes])
        if pl.form != FORM_RUN:
            raise TsfsError("a management part in form %d" % pl.form)
        out = bytearray()
        for i in range(blks_for(pl.nbytes, MB_PAY)):
            blk = pl.a + i
            buf = self.vol.read_block(blk)
            why = head.problem(buf, self.vol.sb.vol_uuid, MAGIC_TBL, blk,
                               self.owner)
            if why is not None:
                if check is None:
                    raise TsfsError("block %d of a part of %s: %s"
                                   % (blk, self.uuid, why))
                check(blk, why)
            out += buf[HDR_SIZE:]
        return bytes(out[:pl.nbytes])

    def extents(self, pl, check=None):
        """The runs (first block within the part, block, blocks) of a data
        part, in order."""
        if pl.form == FORM_RUN:
            return [(0, pl.a, pl.b)] if pl.b > 0 else []
        if pl.form != FORM_TREE:
            return []
        buf = self.vol.read_block(pl.a)
        why = head.problem(buf, self.vol.sb.vol_uuid, MAGIC_TBL, pl.a,
                           self.owner)
        if why is not None:
            if check is None:
                raise TsfsError("extent leaf %d of %s: %s"
                               % (pl.a, self.uuid, why))
            check(pl.a, why)
        n = struct.unpack_from("<I", buf, XL_N)[0]
        if n > MAX_EXTENTS:
            raise TsfsError("extent leaf %d names %d runs" % (pl.a, n))
        return [struct.unpack_from("<QQQ", buf, XL_ENT + i * XL_ESZ)
                for i in range(n)]

    def read_data(self, pl):
        """The bytes of a data part."""
        if pl.form == FORM_NONE:
            return b""
        if pl.form == FORM_INLINE:
            return bytes(self.hdr[pl.a:pl.a + pl.nbytes])
        out = bytearray()
        need = blks_for(pl.nbytes, BLOCK_SIZE)
        runs = self.extents(pl)
        for lb in range(need):
            p = 0
            for (l, s, n) in runs:
                if l <= lb < l + n:
                    p = s + (lb - l)
                    break
            if p == 0:
                raise TsfsError("block %d of a part of %s is nowhere"
                               % (lb, self.uuid))
            out += self.vol.read_block(p)
        return bytes(out[:pl.nbytes])

    def meta(self):
        return self.read_mgmt(OB_META)

    def icon(self):
        """The icon: its management part, or the resource an older volume
        kept it in; None when there is none."""
        if self.place(OB_ICON).form != FORM_NONE:
            return self.read_mgmt(OB_ICON)
        for e in self.resources():
            if e.owner == RID_ICON:
                return self.read_data(e.place)
        return None

    def _table(self, off, cnt, cls, size):
        raw = self.read_mgmt(off)
        if len(raw) != cnt * size:
            raise TsfsError("the %s of %s is %d bytes for %d entries"
                           % (PART_NAMES[off], self.uuid, len(raw), cnt))
        return unpack_table(raw, cls, size)

    def placements(self):
        return self._table(OB_PLACE, self.nplace, PlaceEntry, PE_SIZE)

    def resources(self):
        return self._table(OB_RES, self.nres, ResEntry, RS_SIZE)

    def links(self):
        return self._table(OB_LINK, self.nlink, LinkEntry, LK_SIZE)

    # ------------------------------------------------------------ writing

    def mseg_free(self, off):
        pl = self.place(off)
        if pl.form == FORM_INLINE:
            self.inl_give(pl.a, pl.b)
        elif pl.form == FORM_RUN and pl.b > 0:
            self.vol.free_ext(pl.a, pl.b)
        self.set_place(off, Place())

    def mseg_write(self, off, data):
        """A management part written whole: inline when it fits, else a
        run of sealed blocks; a run of the same length is used again."""
        pl = self.place(off)
        if pl.form == FORM_INLINE:
            self.inl_give(pl.a, pl.b)
            pl.form = FORM_NONE
            self.set_place(off, pl)
        n = len(data)
        if n <= 0:
            self.mseg_free(off)
            return
        at = self.inl_take(n) if n <= INLINE_BYTES else -1
        if at >= 0:
            self.mseg_free(off)
            self.hdr[at:at + n] = data
            self.set_place(off, Place(FORM_INLINE, n, at,
                                      blks_for(n, INLINE_UNIT)))
            return
        want = blks_for(n, MB_PAY)
        if pl.form == FORM_RUN and pl.b == want:
            start = pl.a
        else:
            self.mseg_free(off)
            # the first free run may be too short while a longer one is
            # further on: the short ones are held aside until one is long
            # enough, then given back
            held = []
            try:
                while True:
                    start, got = self.vol.alloc_ext(want)
                    if got >= want:
                        break
                    held.append((start, got))
            except TsfsError:
                raise TsfsError("no run of %d blocks is free" % want)
            finally:
                for hs, hg in held:
                    self.vol.free_ext(hs, hg)
        for i in range(want):
            buf = bytearray(BLOCK_SIZE)
            chunk = data[i * MB_PAY:(i + 1) * MB_PAY]
            buf[HDR_SIZE:HDR_SIZE + len(chunk)] = chunk
            self.vol.write_block(start + i, head.seal(
                buf, self.vol.sb.vol_uuid, self.vol.sb.journal_seq, MAGIC_TBL,
                start + i, self.owner))
        self.set_place(off, Place(FORM_RUN, n, start, want))

    def dseg_new(self, data):
        """A data part for these bytes, written: inline when small enough
        and there is room, else blocks as one run or an extent tree.
        Answers its place."""
        n = len(data)
        if n == 0:
            return Place()
        if n <= DATA_INLINE_MAX:
            at = self.inl_take(n)
            if at >= 0:
                self.hdr[at:at + n] = data
                return Place(FORM_INLINE, n, at, blks_for(n, INLINE_UNIT))
        need = blks_for(n, BLOCK_SIZE)
        runs = []
        have = 0
        try:
            while have < need:
                start, got = self.vol.alloc_ext(need - have)
                if runs and runs[-1][1] + runs[-1][2] == start:
                    runs[-1] = (runs[-1][0], runs[-1][1], runs[-1][2] + got)
                else:
                    if len(runs) >= MAX_EXTENTS:
                        self.vol.free_ext(start, got)
                        raise TsfsError("the bytes would lie in more than %d "
                                       "runs" % MAX_EXTENTS)
                    runs.append((have, start, got))
                have += got
        except TsfsError:
            for (_l, s, c) in runs:
                self.vol.free_ext(s, c)
            raise
        for (l, s, c) in runs:
            for i in range(c):
                chunk = data[(l + i) * BLOCK_SIZE:(l + i + 1) * BLOCK_SIZE]
                if len(chunk) < BLOCK_SIZE:
                    chunk = chunk + b"\0" * (BLOCK_SIZE - len(chunk))
                self.vol.write_block(s + i, chunk)
        if len(runs) == 1:
            return Place(FORM_RUN, n, runs[0][1], runs[0][2])
        leaf = self.vol.alloc_blk()
        buf = bytearray(BLOCK_SIZE)
        struct.pack_into("<I", buf, XL_N, len(runs))
        for i, (l, s, c) in enumerate(runs):
            struct.pack_into("<QQQ", buf, XL_ENT + i * XL_ESZ, l, s, c)
        self.vol.write_block(leaf, head.seal(
            buf, self.vol.sb.vol_uuid, self.vol.sb.journal_seq, MAGIC_TBL,
            leaf, self.owner))
        return Place(FORM_TREE, n, leaf, len(runs))

    def dseg_free(self, pl):
        if pl.form == FORM_INLINE:
            self.inl_give(pl.a, pl.b)
            return
        for (_l, s, c) in self.extents(pl):
            if c > 0:
                self.vol.free_ext(s, c)
        if pl.form == FORM_TREE:
            self.vol.free_ext(pl.a, 1)

    def set_table(self, off, cnt_off, entries):
        struct.pack_into("<I", self.hdr, cnt_off, len(entries))
        self.mseg_write(off, pack_table(entries))
