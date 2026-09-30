"""Allocation groups: their heads, their free maps, and handing out blocks.

A group head is a management block ("TFGH", owner = the group number):

     64  group number              72  first block
     80  blocks                    88  free blocks
     96  first map block          104  map blocks (32)
    108  1 when it keeps a spare superblock (32)
    128  CRC32C of each map block (32 each)

The map blocks carry no head: one bit per block of the group, set when
the block is taken. The blocks the group uses itself (head, spare, map)
and the bits past the end of a short group are set from the start.
"""

import struct

from . import head
from .crc import crc32c
from .error import TsfsError
from .layout import BITS_PER_MAP, BLOCK_SIZE, MAGIC_AGH

AG_NO = 64
AG_FIRST = 72
AG_LEN = 80
AG_FREE = 88
AG_MAPFIRST = 96
AG_MAPBLOCKS = 104
AG_SPARE = 108
AG_MAPCRC = 128


def pack_head(sb, shape, free, crcs, gen=None):
    """The head block of a group, sealed."""
    buf = bytearray(BLOCK_SIZE)
    struct.pack_into("<QQQQQII", buf, AG_NO, shape.n, shape.first, shape.len,
                     free, shape.map_first, shape.map_blocks,
                     1 if shape.spare else 0)
    for k in range(shape.map_blocks):
        struct.pack_into("<I", buf, AG_MAPCRC + 4 * k, crcs[k])
    return head.seal(buf, sb.vol_uuid, sb.journal_seq if gen is None else gen,
                     MAGIC_AGH, shape.first, shape.n)


def head_problems(buf, sb, shape):
    """What is wrong with a group head, as a list of text; empty when the
    kernel's ag_unpack would take it."""
    why = head.problem(buf, sb.vol_uuid, MAGIC_AGH, shape.first)
    if why is not None:
        return [why]
    out = []
    no, first, length, free, mfirst = struct.unpack_from("<QQQQQ", buf, AG_NO)
    mblocks, spare = struct.unpack_from("<II", buf, AG_MAPBLOCKS)
    if no != shape.n:
        out.append("group number %d" % no)
    if first != shape.first:
        out.append("first block %d, not %d" % (first, shape.first))
    if length != shape.len:
        out.append("%d blocks, not %d" % (length, shape.len))
    if mfirst != shape.map_first:
        out.append("map at %d, not %d" % (mfirst, shape.map_first))
    if mblocks != shape.map_blocks:
        out.append("%d map blocks, not %d" % (mblocks, shape.map_blocks))
    if free > shape.len:
        out.append("%d free of %d" % (free, shape.len))
    if spare != (1 if shape.spare else 0):
        out.append("the spare flag is %d" % spare)
    if head.owner(buf) != shape.n:
        out.append("owner %d" % head.owner(buf))
    return out


def head_fields(buf, shape):
    """(free, [map CRCs]) from a head block."""
    free = struct.unpack_from("<Q", buf, AG_FREE)[0]
    crcs = [struct.unpack_from("<I", buf, AG_MAPCRC + 4 * k)[0]
            for k in range(shape.map_blocks)]
    return free, crcs


def fresh_map(shape):
    """The map of a group as the volume is made: its own blocks and the
    bits past its end taken, everything else free. One bytearray over all
    of the group's map blocks."""
    m = bytearray(shape.map_blocks * BLOCK_SIZE)
    for off in range(shape.meta):
        m[off >> 3] |= 1 << (off & 7)
    for off in range(shape.len, shape.map_blocks * BITS_PER_MAP):
        m[off >> 3] |= 1 << (off & 7)
    return m


def map_crcs(m, shape):
    return [crc32c(bytes(m[k * BLOCK_SIZE:(k + 1) * BLOCK_SIZE]))
            for k in range(shape.map_blocks)]


def count_free(m, shape):
    """Zero bits among the group's own blocks."""
    used = 0
    whole = shape.len >> 3
    tbl = _POPCOUNT
    for b in bytes(m[:whole]):
        used += tbl[b]
    for off in range(whole << 3, shape.len):
        if m[off >> 3] & (1 << (off & 7)):
            used += 1
    return shape.len - used


_POPCOUNT = tuple(bin(i).count("1") for i in range(256))


class Maps(object):
    """The free maps of every group, held in memory, with the allocator
    the kernel uses (ts_alloc_ext): a run of up to `want` blocks from the
    first free block found, starting where the last search left off,
    through the groups in turn, wrapping once."""

    def __init__(self, sb, maps, free=None):
        self.sb = sb
        self.shapes = sb.groups()
        self.maps = maps                        # bytearray per group
        self.free = (list(free) if free is not None else
                     [count_free(m, s) for m, s in zip(maps, self.shapes)])
        self.dirty = set()
        self.hint = sb.ag_first

    @classmethod
    def fresh(cls, sb):
        shapes = sb.groups()
        return cls(sb, [fresh_map(s) for s in shapes],
                   [s.len - s.meta for s in shapes])

    # ------------------------------------------------------------ bits

    def _loc(self, blk):
        n = self.sb.group_of(blk)
        if n is None:
            raise TsfsError("block %d is in no group" % blk)
        return n, blk - self.shapes[n].first

    def get(self, blk):
        n, off = self._loc(blk)
        return bool(self.maps[n][off >> 3] & (1 << (off & 7)))

    def put(self, blk, used):
        n, off = self._loc(blk)
        m = self.maps[n]
        bit = 1 << (off & 7)
        was = bool(m[off >> 3] & bit)
        if was == used:
            return False
        if used:
            m[off >> 3] |= bit
            self.free[n] -= 1
        else:
            m[off >> 3] &= ~bit & 0xFF
            self.free[n] += 1
        self.dirty.add(n)
        return True

    def total_free(self):
        return sum(self.free)

    # ------------------------------------------------------------ runs

    def _run_find(self, n, frm, to, want):
        m = self.maps[n]
        first = self.shapes[n].first
        off = frm - first
        end = to - first
        while off < end:
            if (off & 7) == 0 and m[off >> 3] == 0xFF:
                off += 8
                continue
            if m[off >> 3] & (1 << (off & 7)):
                off += 1
                continue
            k = 1
            while k < want and off + k < end:
                o = off + k
                if m[o >> 3] & (1 << (o & 7)):
                    break
                k += 1
            return first + off, k
        return None

    def alloc_ext(self, want):
        """(start, count) of a run of 1..want blocks, taken."""
        if want <= 0:
            raise TsfsError("a run is at least one block")
        agn = self.sb.ag_count
        h = self.sb.group_of(self.hint)
        if h is None:
            h = 0
            self.hint = self.shapes[0].first
        found = None
        for i in range(agn + 1):
            n = (h + i) % agn
            s = self.shapes[n]
            lo, hi = s.data_first, s.end
            if self.free[n] == 0:
                continue
            if i == 0:
                frm = max(self.hint, lo)
            elif i == agn:
                hi = max(self.hint, lo)
                frm = lo
            else:
                frm = lo
            found = self._run_find(n, frm, hi, want)
            if found is not None:
                break
        if found is None:
            raise TsfsError("the volume is full")
        start, count = found
        for b in range(start, start + count):
            self.put(b, True)
        self.hint = start + count
        return start, count

    def alloc_blk(self):
        return self.alloc_ext(1)[0]

    def free_ext(self, start, count):
        """Give a run back: data blocks of one group, every one taken."""
        n = self.sb.group_of(start)
        if n is None:
            raise TsfsError("block %d is in no group" % start)
        s = self.shapes[n]
        if start < s.data_first or start + count > s.end or count <= 0:
            raise TsfsError("blocks %d+%d are not data blocks of group %d"
                           % (start, count, n))
        for b in range(start, start + count):
            if not self.get(b):
                raise TsfsError("block %d is already free" % b)
        for b in range(start, start + count):
            self.put(b, False)
        if start < self.hint:
            self.hint = start

    # ------------------------------------------------------------ out

    def write(self, vol, groups=None, gen=None):
        """The map blocks and heads of the dirty groups (or those named)."""
        todo = sorted(self.dirty if groups is None else groups)
        for n in todo:
            s = self.shapes[n]
            m = self.maps[n]
            for k in range(s.map_blocks):
                vol.write_block(s.map_first + k,
                                bytes(m[k * BLOCK_SIZE:(k + 1) * BLOCK_SIZE]))
            vol.write_block(s.first, pack_head(self.sb, s, self.free[n],
                                               map_crcs(m, s), gen))
        self.dirty.difference_update(todo)
