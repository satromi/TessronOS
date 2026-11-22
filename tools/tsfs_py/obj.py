"""The object block and the parts an object has (design 11.6.6, 7.3).

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
    288  place of the icon
    320  version 5: the spare place, runs the object owns that no record
         and no management part does (F_SPARE while it is not empty)
    512  the inline area, 56 units of 64 bytes

The reference count is the number of link table entries of the volume
that point at the object and are not external, plus the holds.

A place (32 bytes): form (8 bits at 0: none, inline, run, tree, tree of
two levels, run tree, runs to give back), bytes (64 at 8), then for
inline the offset in the block and the units (16 bits each at 16 and
18), for a run the first block and its length (64 each at 16 and 24),
for a tree the leaf (or index, or root node) block and the number of
runs, for the runs to give back the rid whose runs past `bytes` go.

Management parts (metadata, icon, the three tables) are inline or a run
of blocks, each with the common head ("TFTB") and 4032 bytes of
payload; on a volume of version 5 also a tree (F_TREE, F_TREEN) whose
runs are such blocks, the logical block being the number of the block
within the part. Data parts (records, resources) are inline when 2048
bytes or less and there is room, else plain blocks as one run, else an
extent tree: one "TFTB" leaf with the count at 64 and from 128 up to 165
entries of (first block within the part, block on the volume, blocks),
64 each; a tree of two levels (F_TREE2) has an index of such leaves; a
run tree (F_TREEN, version 5) has nodes of any level (at 68, 0 at the
leaves), an index entry being (first block of the child, the child's
node, the blocks below it).

Placement table entry (48): rid, kind (0 xtad, 1 bin), bytes, place.
Resource table entry (64): owner (rid; 0xFFFFFFFF for the icon; the top
bit and a position for one written before its record), number, file
name extension (16, NUL padded; on version 5, 0xFF then the offset and
length of an extension of 16 bytes or more in the string area behind
the entries), bytes, place.
Link table entry (48): vobjid (all zero when the link has none), target
UUID, rid of the record holding the link, flags (bit 0: the target was
not in the object index when the table was made, and is not counted),
8 reserved. The entries are in the order of their first 40 bytes taken
as unsigned bytes, the rid and the flags little endian as they lie.
"""

import os
import struct
import uuid

from . import head
from .error import TsfsError
from .layout import (BLOCK_SIZE, HDR_SIZE, MAGIC_OBJ, MAGIC_TBL, owner_of,
                     V4_EXT_MAX, V4_MAX_LINK, V4_MAX_REC, V4_MAX_RES,
                     V4_META_MAX)

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
OB_SPARE = 320                          # version 5: the spare place
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
F_TRASH = 0x1000          # a cabinet of 隠れ屑実身 the disk repair made
F_SPARE = 0x2000          # version 5: the spare place holds runs; on the orphan tree
F_DEFAULT = F_EDITABLE | F_DELETABLE | F_READABLE

FORM_NONE = 0
FORM_INLINE = 1
FORM_RUN = 2
FORM_TREE = 3
FORM_TREE2 = 4              # an index of leaves, each a leaf as FORM_TREE has
FORM_TREEN = 5              # version 5: a run tree of any depth
FORM_TRIM = 6               # version 5, the spare place only: record a's runs past bytes
FORM_NAMES = {FORM_NONE: "none", FORM_INLINE: "inline", FORM_RUN: "run",
              FORM_TREE: "tree", FORM_TREE2: "tree of two levels",
              FORM_TREEN: "run tree", FORM_TRIM: "runs to give back"}
TREE_FORMS = (FORM_TREE, FORM_TREE2, FORM_TREEN)
RUN_FORMS = (FORM_RUN,) + TREE_FORMS

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

MB_PAY = BLOCK_SIZE - HDR_SIZE          # payload of a management block
DATA_INLINE_MAX = 2048
MAX_EXTENTS = 165
XL_N = 64
XL_ENT = 128
XL_ESZ = 24
XN_LEVEL = 68
XN_MAX_LEVEL = 8                        # deeper than this a run tree is broken

RS_EXT = 8
RSX_MARK = 0xFF
RSX_OFF = 4
RSX_LEN = 8

# The limits of a volume of version 4 (Limits answers a volume's own)
MAX_REC = V4_MAX_REC
MAX_RES = V4_MAX_RES
MAX_LINK = V4_MAX_LINK
EXT_LEN = V4_EXT_MAX + 1
META_MAX = V4_META_MAX
ICON_MAX = 65536
# the object layer's bound on one metadata text (OB_META_LIMIT): what is
# held in memory at once, on any volume
META_LIMIT = 64 * 1024 * 1024


def xn_fanout():
    """Entries a node of a run tree written here holds: 165, or fewer
    when TSFS_PY_XN_FANOUT asks (for trees of several levels in tests)."""
    v = os.environ.get("TSFS_PY_XN_FANOUT")
    if v:
        try:
            n = int(v)
        except ValueError:
            n = 0
        if 2 <= n <= MAX_EXTENTS:
            return n
    return MAX_EXTENTS


def is_icon(data):
    """An icon is an ICO or a PNG."""
    return (data[:4] == b"\0\0\1\0" and len(data) >= 6) or data[:8] == b"\x89PNG\r\n\x1a\n"


PART_NAMES = {OB_META: "metadata", OB_PLACE: "placement table",
              OB_RES: "resource table", OB_LINK: "link table",
              OB_ICON: "icon", OB_SPARE: "spare place"}


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
        elif self.form != FORM_NONE:
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

    def is_empty(self):
        return self.form == FORM_NONE and self.nbytes == 0 and not self.a and not self.b

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
    """A resource table entry. ext is the extension as text; ext_raw the
    16 bytes of the field as read; ext_long its (offset, length) in the
    string area when the field is of the long form."""

    __slots__ = ("owner", "resno", "ext", "nbytes", "place", "ext_raw",
                 "ext_long", "ext_bytes")

    def __init__(self, owner, resno, ext, nbytes=0, place=None):
        self.owner = owner
        self.resno = resno              # signed, as the kernel's INT
        self.ext = ext                  # text
        self.nbytes = nbytes
        self.place = place or Place()
        self.ext_raw = None
        self.ext_long = None
        self.ext_bytes = None

    def ext_utf8(self):
        if self.ext_bytes is not None:
            return self.ext_bytes
        return self.ext.encode("utf-8")

    def pack(self, field=None):
        """The entry; `field` the 16 bytes of the extension field, else
        the short form of the extension (cut to 15 bytes)."""
        if field is None:
            raw = self.ext_utf8()[:EXT_LEN - 1]
            field = raw + b"\0" * (EXT_LEN - len(raw))
        return (struct.pack("<Ii", self.owner, self.resno) + field
                + struct.pack("<Q", self.nbytes) + self.place.pack())

    @classmethod
    def unpack(cls, buf, off, area=None):
        """An entry; `area`, the string area of a volume of version 5,
        or None on version 4, where the field is always of the short
        form. Raises for a long form that points out of the area."""
        owner, resno = struct.unpack_from("<Ii", buf, off)
        raw = bytes(buf[off + RS_EXT:off + RS_EXT + EXT_LEN])
        nbytes = struct.unpack_from("<Q", buf, off + 24)[0]
        long_ = None
        if area is not None and raw[0] == RSX_MARK:
            xo, xl = struct.unpack_from("<II", raw, RSX_OFF)
            if xl == 0 or xo + xl > len(area):
                raise TsfsError("an extension at %d+%d of a string area of %d "
                                "bytes" % (xo, xl, len(area)))
            eb = bytes(area[xo:xo + xl])
            long_ = (xo, xl)
        else:
            eb = raw.split(b"\0", 1)[0]
        e = cls(owner, resno, eb.decode("utf-8", "replace"), nbytes,
                Place.unpack(buf, off + 32))
        e.ext_raw = raw
        e.ext_long = long_
        e.ext_bytes = eb
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


def pack_res_table(entries, v5):
    """A resource table: the entries, and on a volume of version 5 the
    string area behind them, made afresh in the order of the entries,
    for the extensions of 16 bytes or more and those that start with
    0xFF. Raises on version 4 for an
    extension longer than the field takes."""
    area = bytearray()
    out = bytearray()
    for e in entries:
        eb = e.ext_utf8()
        if len(eb) <= EXT_LEN - 1 and not (v5 and eb[:1] == bytes([RSX_MARK])):
            out += e.pack()
            continue
        if not v5:
            raise TsfsError("the extension %r is longer than %d bytes"
                            % (e.ext, EXT_LEN - 1))
        field = bytearray(EXT_LEN)
        field[0] = RSX_MARK
        struct.pack_into("<II", field, RSX_OFF, len(area), len(eb))
        area += eb
        out += e.pack(bytes(field))
    return bytes(out + area)


def unpack_res_table(raw, cnt, v5):
    """The entries of a resource table of cnt entries and its string area
    (b"" on version 4). Raises when the bytes do not fit the count."""
    if v5:
        if len(raw) < cnt * RS_SIZE:
            raise TsfsError("the resource table is %d bytes for %d entries"
                            % (len(raw), cnt))
    elif len(raw) != cnt * RS_SIZE:
        raise TsfsError("the resource table is %d bytes for %d entries"
                        % (len(raw), cnt))
    area = raw[cnt * RS_SIZE:] if v5 else None
    ents = [ResEntry.unpack(raw, i * RS_SIZE, area) for i in range(cnt)]
    return ents, (area or b"")


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
        self.v5 = vol.sb.v5

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

    def spare(self):
        """The spare place (all 0 on a volume of version 4)."""
        return self.place(OB_SPARE)

    def spare_raw_empty(self):
        return self.hdr[OB_SPARE:OB_SPARE + PL_SIZE] == bytes(PL_SIZE)

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

    # ------------------------------------------------------------ trees

    def _node(self, blk, what, check):
        """A block of a tree (kind "TFTB", the object's): (count, level,
        entries)."""
        buf = self.vol.read_block(blk)
        why = head.problem(buf, self.vol.sb.vol_uuid, MAGIC_TBL, blk,
                           self.owner)
        if why is not None:
            if check is None:
                raise TsfsError("extent %s %d of %s: %s"
                               % (what, blk, self.uuid, why))
            check(blk, why)
        n = struct.unpack_from("<I", buf, XL_N)[0]
        level = struct.unpack_from("<I", buf, XN_LEVEL)[0]
        if n > MAX_EXTENTS:
            raise TsfsError("extent %s %d names %d entries" % (what, blk, n))
        return n, level, [struct.unpack_from("<QQQ", buf, XL_ENT + i * XL_ESZ)
                          for i in range(n)]

    def _xt_table(self, blk, what, check):
        """A leaf or an index of the older forms: its entries."""
        return self._node(blk, what, check)[2]

    def leaves(self, pl, check=None):
        """The leaves of a tree of two levels: (first block, leaf, runs)."""
        if pl.form != FORM_TREE2:
            return []
        return self._xt_table(pl.a, "index", check)

    def _treen(self, blk, want_level, nodes, check, depth):
        """The runs below a node of a run tree, its first logical block
        and the blocks below it checked against what its parent says."""
        if depth > XN_MAX_LEVEL:
            raise TsfsError("the run tree of %s is deeper than %d levels"
                           % (self.uuid, XN_MAX_LEVEL))
        if nodes is not None:
            nodes.append(blk)
        n, level, ents = self._node(blk, "node", check)
        if want_level is not None and level != want_level:
            raise TsfsError("run tree node %d of %s is at level %d, not %d"
                           % (blk, self.uuid, level, want_level))
        if level > XN_MAX_LEVEL:
            raise TsfsError("run tree node %d of %s is at level %d"
                           % (blk, self.uuid, level))
        if n == 0:
            raise TsfsError("run tree node %d of %s is empty" % (blk, self.uuid))
        if level == 0:
            return list(ents)
        runs = []
        for (first, child, blocks) in ents:
            got = self._treen(child, level - 1, nodes, check, depth + 1)
            if got and got[0][0] != first:
                raise TsfsError("run tree node %d of %s: a child %d starts at "
                               "block %d, the index says %d"
                               % (blk, self.uuid, child, got[0][0], first))
            below = sum(c for (_l, _s, c) in got)
            if below != blocks:
                raise TsfsError("run tree node %d of %s: the child %d holds %d "
                               "blocks, the index says %d"
                               % (blk, self.uuid, child, below, blocks))
            runs += got
        return runs

    def tree_depth(self, pl):
        """The level of the root of a run tree (0 for the other forms)."""
        if pl.form != FORM_TREEN:
            return 0
        return self._node(pl.a, "node", lambda b, w: None)[1]

    def extents(self, pl, check=None, nodes=None):
        """The runs (first block within the part, block, blocks) of a part
        kept in runs, in order. `nodes`, when given, gets the blocks of
        the tree."""
        if pl.form == FORM_RUN:
            return [(0, pl.a, pl.b)] if pl.b > 0 else []
        if pl.form == FORM_TREE:
            if nodes is not None:
                nodes.append(pl.a)
            return self._xt_table(pl.a, "leaf", check)
        if pl.form == FORM_TREEN:
            return self._treen(pl.a, None, nodes, check, 0)
        if pl.form != FORM_TREE2:
            return []
        if nodes is not None:
            nodes.append(pl.a)
        runs = []
        for (first, leaf, cnt) in self.leaves(pl, check):
            if nodes is not None:
                nodes.append(leaf)
            got = self._xt_table(leaf, "leaf", check)
            if len(got) != cnt:
                raise TsfsError("extent leaf %d of %s holds %d runs, its index "
                                "says %d" % (leaf, self.uuid, len(got), cnt))
            runs += got
        return runs

    @staticmethod
    def block_map(runs, need):
        """The block of each logical block 0..need-1, or 0 where there is
        none."""
        out = [0] * need
        for (l, s, n) in runs:
            for i in range(n):
                if 0 <= l + i < need:
                    out[l + i] = s + i
        return out

    # ------------------------------------------------------------ reading

    def read_mgmt(self, off_or_place, check=None):
        """A management part whole. `check`, when given, is called with
        (block, problem) for each block that does not pass, and the part
        is read on; otherwise such a block raises."""
        pl = (off_or_place if isinstance(off_or_place, Place)
              else self.place(off_or_place))
        if pl.form == FORM_NONE:
            return b""
        if pl.form == FORM_INLINE:
            return bytes(self.hdr[pl.a:pl.a + pl.nbytes])
        need = blks_for(pl.nbytes, MB_PAY)
        if pl.form == FORM_RUN:
            blocks = [pl.a + i for i in range(need)]
        elif pl.form in (FORM_TREE, FORM_TREEN) and self.v5:
            blocks = self.block_map(self.extents(pl, check), need)
        else:
            raise TsfsError("a management part in form %d" % pl.form)
        out = bytearray()
        for i, blk in enumerate(blocks):
            if blk == 0:
                raise TsfsError("block %d of a part of %s is nowhere"
                               % (i, self.uuid))
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

    def read_data(self, pl):
        """The bytes of a data part."""
        if pl.form == FORM_NONE:
            return b""
        if pl.form == FORM_INLINE:
            return bytes(self.hdr[pl.a:pl.a + pl.nbytes])
        need = blks_for(pl.nbytes, BLOCK_SIZE)
        blocks = self.block_map(self.extents(pl), need)
        out = bytearray()
        for lb, p in enumerate(blocks):
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
        try:
            return unpack_res_table(self.read_mgmt(OB_RES), self.nres,
                                    self.v5)[0]
        except TsfsError as e:
            raise TsfsError("%s: %s" % (self.uuid, e))

    def links(self):
        return self._table(OB_LINK, self.nlink, LinkEntry, LK_SIZE)

    # ------------------------------------------------------------ writing

    def _alloc_runs(self, need, most=None):
        """Runs of `need` blocks in all, as (logical, block, blocks), the
        free runs taken first fit; at most `most` of them (None: any)."""
        runs = []
        have = 0
        try:
            while have < need:
                start, got = self.vol.alloc_ext(need - have)
                if runs and runs[-1][1] + runs[-1][2] == start:
                    runs[-1] = (runs[-1][0], runs[-1][1], runs[-1][2] + got)
                else:
                    if most is not None and len(runs) >= most:
                        self.vol.free_ext(start, got)
                        raise TsfsError("the bytes would lie in more than %d "
                                       "runs" % most)
                    runs.append((have, start, got))
                have += got
        except TsfsError:
            for (_l, s, c) in runs:
                self.vol.free_ext(s, c)
            raise
        return runs

    def _seal_node(self, blk, n, level, ents):
        buf = bytearray(BLOCK_SIZE)
        struct.pack_into("<I", buf, XL_N, n)
        if level:
            struct.pack_into("<I", buf, XN_LEVEL, level)
        for i, (l, s, c) in enumerate(ents):
            struct.pack_into("<QQQ", buf, XL_ENT + i * XL_ESZ, l, s, c)
        self.vol.write_block(blk, head.seal(
            buf, self.vol.sb.vol_uuid, self.vol.sb.journal_seq, MAGIC_TBL,
            blk, self.owner))

    def build_tree(self, runs):
        """A run tree for the runs, written: leaves of up to the fan-out,
        index levels above them until one node is left. Answers the root
        block."""
        fan = xn_fanout()
        level = 0
        ents = list(runs)
        made = []
        try:
            while True:
                nodes = []
                for i in range(0, len(ents), fan):
                    chunk = ents[i:i + fan]
                    blk = self.vol.alloc_blk()
                    made.append(blk)
                    self._seal_node(blk, len(chunk), level, chunk)
                    nodes.append((chunk[0][0], blk, sum(
                        c for (_l, _s, c) in chunk)))
                if len(nodes) == 1:
                    return nodes[0][1]
                ents = nodes
                level += 1
        except TsfsError:
            for b in made:
                self.vol.free_ext(b, 1)
            raise

    def put_runs(self, runs, nbytes, mgmt=False):
        """The place for a part laid out in these runs: one run as it is,
        up to a leaf's worth in one leaf (F_TREE), more in a run tree on a
        volume of version 5. The tree's blocks are taken and written."""
        if not runs:
            return Place()
        if len(runs) == 1:
            return Place(FORM_RUN, nbytes, runs[0][1], runs[0][2])
        fan = xn_fanout()
        if len(runs) <= fan or (not self.v5 and len(runs) <= MAX_EXTENTS):
            if mgmt and not self.v5:
                raise TsfsError("a management part in %d runs on a volume of "
                                "version 4" % len(runs))
            leaf = self.vol.alloc_blk()
            self._seal_node(leaf, len(runs), 0, runs)
            return Place(FORM_TREE, nbytes, leaf, len(runs))
        if not self.v5:
            raise TsfsError("the bytes would lie in %d runs, more than %d"
                            % (len(runs), MAX_EXTENTS))
        return Place(FORM_TREEN, nbytes, self.build_tree(runs), len(runs))

    def free_runs_of(self, pl):
        """Give back the runs of a place and the blocks of its tree."""
        nodes = []
        for (_l, s, c) in self.extents(pl, nodes=nodes):
            if c > 0:
                self.vol.free_ext(s, c)
        for b in nodes:
            self.vol.free_ext(b, 1)

    def mseg_free(self, off):
        pl = self.place(off)
        if pl.form == FORM_INLINE:
            self.inl_give(pl.a, pl.b)
        elif pl.form in RUN_FORMS:
            self.free_runs_of(pl)
        self.set_place(off, Place())

    def mseg_write(self, off, data):
        """A management part written whole: inline when it fits, else a
        run of sealed blocks; a run of the same length is used again. On
        a volume of version 5 a part that finds no free run long enough
        lies in several, named by a tree."""
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
            runs = [(0, pl.a, want)]
            newpl = Place(FORM_RUN, n, pl.a, want)
        else:
            self.mseg_free(off)
            runs = None
            # the first free run may be too short while a longer one is
            # further on: the short ones are held aside until one is long
            # enough, then given back
            held = []
            try:
                while True:
                    start, got = self.vol.alloc_ext(want)
                    if got >= want:
                        runs = [(0, start, want)]
                        break
                    held.append((start, got))
            except TsfsError:
                if not self.v5:
                    raise TsfsError("no run of %d blocks is free" % want)
            finally:
                for hs, hg in held:
                    self.vol.free_ext(hs, hg)
            if runs is None:
                runs = self._alloc_runs(want)
            newpl = self.put_runs(runs, n, mgmt=True)
        for (l, s, c) in runs:
            for i in range(c):
                k = l + i
                buf = bytearray(BLOCK_SIZE)
                chunk = data[k * MB_PAY:(k + 1) * MB_PAY]
                buf[HDR_SIZE:HDR_SIZE + len(chunk)] = chunk
                self.vol.write_block(s + i, head.seal(
                    buf, self.vol.sb.vol_uuid, self.vol.sb.journal_seq,
                    MAGIC_TBL, s + i, self.owner))
        self.set_place(off, newpl)

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
        runs = self._alloc_runs(need, None if self.v5 else MAX_EXTENTS)
        try:
            pl = self.put_runs(runs, n)
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
        return pl

    def dseg_free(self, pl):
        if pl.form == FORM_INLINE:
            self.inl_give(pl.a, pl.b)
            return
        if pl.form in RUN_FORMS:
            self.free_runs_of(pl)

    def set_table(self, off, cnt_off, entries):
        struct.pack_into("<I", self.hdr, cnt_off, len(entries))
        if off == OB_RES:
            self.mseg_write(off, pack_res_table(entries, self.v5))
        else:
            self.mseg_write(off, pack_table(entries))

    # ------------------------------------------------------------ the spare place

    def spare_runs(self, nodes=None):
        """The runs the spare place holds, and the blocks of its tree in
        `nodes`; for the runs to give back (F_TRIM), the runs of the
        record past its kept bytes. None when the spare is empty."""
        pl = self.spare()
        if pl.form == FORM_NONE:
            return None
        if pl.form in RUN_FORMS:
            return self.extents(pl, nodes=nodes)
        if pl.form == FORM_TRIM:
            return self.trim_tail(pl)[1]
        raise TsfsError("the spare place of %s has form %d" % (self.uuid, pl.form))

    def trim_tail(self, spl):
        """For a spare place of F_TRIM: (the record's entry, its runs past
        the kept bytes, its runs that stay). Raises when the record is
        not there."""
        rid = spl.a & 0xFFFFFFFF
        for e in self.placements():
            if e.rid == rid:
                break
        else:
            raise TsfsError("the spare place of %s gives back runs of rid %d, "
                           "which is no record" % (self.uuid, rid))
        keep = blks_for(spl.nbytes, BLOCK_SIZE)
        if e.place.form not in RUN_FORMS:
            return e, [], []
        tail, stay = [], []
        for (l, s, c) in self.extents(e.place):
            if l >= keep:
                tail.append((l, s, c))
            elif l + c > keep:
                k = keep - l
                stay.append((l, s, k))
                tail.append((keep, s + k, c - k))
            else:
                stay.append((l, s, c))
        return e, tail, stay

    def empty_spare(self):
        """Give back what the spare place holds, as the kernel's tidy at
        mount does, and take the mark off. Answers the blocks given back.
        The object block is not stored."""
        pl = self.spare()
        freed = 0
        if pl.form in RUN_FORMS:
            nodes = []
            for (_l, s, c) in self.extents(pl, nodes=nodes):
                if c > 0:
                    self.vol.free_ext(s, c)
                    freed += c
            for b in nodes:
                self.vol.free_ext(b, 1)
                freed += 1
        elif pl.form == FORM_TRIM:
            e, tail, stay = self.trim_tail(pl)
            if tail:
                for (_l, s, c) in tail:
                    self.vol.free_ext(s, c)
                    freed += c
                # the record's own tree written again for the runs it keeps
                nodes = []
                self.extents(e.place, nodes=nodes)
                for b in nodes:
                    self.vol.free_ext(b, 1)
                newpl = self.put_runs(stay, e.place.nbytes)
                ents = self.placements()
                for x in ents:
                    if x.rid == e.rid:
                        x.place = newpl
                        x.place.nbytes = x.nbytes
                self.set_table(OB_PLACE, OB_NPLACE, ents)
        elif pl.form != FORM_NONE:
            raise TsfsError("the spare place of %s has form %d"
                            % (self.uuid, pl.form))
        self.hdr[OB_SPARE:OB_SPARE + PL_SIZE] = bytes(PL_SIZE)
        self.flags = self.flags & ~F_SPARE
        return freed
