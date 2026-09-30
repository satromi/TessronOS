"""A whole volume: the superblock, the journal, the groups and the trees.

Opening reads both superblocks and takes the valid one of the higher
generation, as the kernel does. A journal holding transactions that were
committed but never put in place is either replayed onto the medium or,
for a look that must not write, laid over the blocks in memory, so that
everything read afterwards is what the kernel would see once it has
replayed it.

The tools write straight to the blocks, not through the journal: they
work on a volume nothing else has open.
"""

import os
import struct

from .btree import BTree, TREE_GC, TREE_OBJ, TREE_ORPHAN
from .error import TsfsError
from .groups import Maps, head_fields, head_problems, map_crcs
from .journal import Journal
from .layout import (BLOCK_SIZE, LABEL_MAX, ST_CLEAN, Superblock, gen_uuid7,
                     layout, now_tron)
from .obj import ObjectBlock


class SbCopy(object):
    """What one of the two superblock blocks holds."""

    def __init__(self, blk, sb=None, error=None):
        self.blk = blk
        self.sb = sb
        self.error = error

    @property
    def ok(self):
        return self.sb is not None


def read_sb_copies(read):
    out = []
    for blk in (0, 1):
        try:
            out.append(SbCopy(blk, Superblock.unpack(read(blk))))
        except TsfsError as e:
            out.append(SbCopy(blk, error=str(e)))
    return out


def newest(copies):
    """The valid copy of the higher generation, A on a tie."""
    good = [c for c in copies if c.ok]
    if not good:
        return None
    best = good[0]
    for c in good[1:]:
        if c.sb.generation > best.sb.generation:
            best = c
    return best


class Volume(object):
    """A volume on a block image. Use it as a context manager, or close
    it, so that what changed reaches the medium."""

    def __init__(self, dev, sb, writable=False):
        self.dev = dev
        self.sb = sb
        self.writable = writable
        self.overlay = {}
        self.sb_copies = []
        self.sb_dirty = False
        self.maps = None
        self.group_problems = {}
        self.map_problems = {}
        self.journal = None
        self.replayed = (0, 0)          # transactions, blocks put in place
        self.btree = BTree(self, TREE_OBJ)
        self.gctree = BTree(self, TREE_GC)
        self.ortree = BTree(self, TREE_ORPHAN)

    # ------------------------------------------------------------ opening

    @classmethod
    def open(cls, dev, replay=False):
        """Open a volume. With replay the journal's pending transactions
        are put in place first (the device must be writable); without,
        they are laid over the blocks in memory."""
        copies = read_sb_copies(dev.read_block)
        best = newest(copies)
        if best is None:
            raise TsfsError("neither superblock can be used: A: %s; B: %s"
                           % (copies[0].error, copies[1].error))
        if best.sb.total_blocks > dev.nblk:
            raise TsfsError("the superblock says %d blocks, the medium holds %d"
                           % (best.sb.total_blocks, dev.nblk))
        vol = cls(dev, best.sb.copy(), writable=dev.writable)
        vol.sb_copies = copies
        vol.journal = Journal(vol)
        vol.journal.scan()
        if vol.journal.pending():
            if replay:
                if not dev.writable:
                    raise TsfsError("the journal has to be replayed, and the "
                                   "image is read only")
                ntx = vol.journal.pending()
                vol.replayed = (ntx, vol.journal.replay())
            else:
                vol.overlay = vol.journal.overlay()
            vol.reload_sb()
        return vol

    def reload_sb(self):
        """The superblock again, as the blocks now read."""
        copies = read_sb_copies(self.read_block)
        best = newest(copies)
        if best is None:
            raise TsfsError("the journal leaves no superblock that can be used")
        self.sb_copies = copies
        jr = self.journal
        self.sb = best.sb.copy()
        self.journal = Journal(self)
        if jr is not None:
            self.journal.head_state = jr.head_state
            self.journal.head = jr.head
            self.journal.head_seq = jr.head_seq
            self.journal.txns = jr.txns
            self.journal.good_end = jr.good_end
            self.journal.next_seq = jr.next_seq
            self.journal.stop_reason = jr.stop_reason
        self.maps = None

    @classmethod
    def format(cls, dev, label="", vol_uuid=None, mkfs_time=None,
               journal_seq=None):
        """Make a volume as ts_format_blk does: the journal head cleared,
        each group's map and head, superblock B (generation 1), A
        (generation 2), and a copy of A in each spare."""
        nblk = dev.nblk
        jblocks, agb, agn = layout(nblk)
        if len((label or "").encode("utf-8")) > LABEL_MAX - 1:
            raise TsfsError("the label is longer than %d bytes" % (LABEL_MAX - 1))
        sb = Superblock()
        sb.total_blocks = nblk
        sb.ag_blocks = agb
        sb.ag_count = agn
        sb.journal_start = 2
        sb.journal_blocks = jblocks
        if journal_seq is None:
            r = struct.unpack("<I", os.urandom(4))[0]
            journal_seq = (r << 8) + 1
        sb.journal_seq = journal_seq
        sb.state = ST_CLEAN
        sb.mkfs_time = now_tron() if mkfs_time is None else mkfs_time
        sb.vol_uuid = vol_uuid if vol_uuid is not None else gen_uuid7()
        sb.set_label(label)

        vol = cls(dev, sb, writable=True)
        vol.raw_write(sb.journal_start, bytes(BLOCK_SIZE))
        vol.maps = Maps.fresh(sb)
        vol.maps.write(vol, groups=range(agn))
        sb.free_blocks = vol.maps.total_free()
        sb.generation = 0
        vol.sb_dirty = True
        vol.write_sb(both=True, spares=True)
        vol.journal = Journal(vol)
        vol.journal.scan()
        return vol

    # ------------------------------------------------------------ blocks

    def raw_read(self, blk):
        return self.dev.read_block(blk)

    def raw_write(self, blk, buf):
        self.dev.write_block(blk, buf)
        self.overlay.pop(blk, None)

    def read_block(self, blk):
        if blk >= self.sb.total_blocks:
            raise TsfsError("block %d is past the end of the volume (%d)"
                           % (blk, self.sb.total_blocks))
        got = self.overlay.get(blk)
        if got is not None:
            return got
        return self.dev.read_block(blk)

    def write_block(self, blk, buf):
        if blk >= self.sb.total_blocks:
            raise TsfsError("block %d is past the end of the volume (%d)"
                           % (blk, self.sb.total_blocks))
        if self.overlay:
            raise TsfsError("the journal has to be replayed before the volume "
                           "is written")
        self.dev.write_block(blk, buf)

    # ------------------------------------------------------------ groups

    def load_maps(self):
        """Every group head and free map into memory. What does not pass
        its check is noted in group_problems / map_problems; the maps are
        taken as they are either way."""
        if self.maps is not None:
            return self.maps
        maps, free = [], []
        self.group_problems = {}
        self.map_problems = {}
        for s in self.sb.groups():
            hb = self.read_block(s.first)
            probs = head_problems(hb, self.sb, s)
            m = bytearray()
            for k in range(s.map_blocks):
                m += self.read_block(s.map_first + k)
            if probs:
                self.group_problems[s.n] = probs
                crcs = None
            else:
                f, crcs = head_fields(hb, s)
                free.append(f)
            want = map_crcs(m, s)
            if crcs is not None:
                bad = [k for k in range(s.map_blocks) if crcs[k] != want[k]]
                if bad:
                    self.map_problems[s.n] = bad
            maps.append(m)
        self.maps = Maps(self.sb, maps)
        self.maps.head_free = {}
        for s in self.sb.groups():
            if s.n not in self.group_problems:
                self.maps.head_free[s.n] = head_fields(
                    self.read_block(s.first), s)[0]
        return self.maps

    def alloc_ext(self, want):
        return self.load_maps().alloc_ext(want)

    def alloc_blk(self):
        return self.load_maps().alloc_blk()

    def free_ext(self, start, count):
        self.load_maps().free_ext(start, count)

    # ------------------------------------------------------------ superblock

    def tree_root(self, tree=TREE_OBJ):
        if tree == TREE_GC:
            return self.sb.gclist_start
        if tree == TREE_ORPHAN:
            return self.sb.orphan_start
        return self.sb.objtbl_start

    def tree_nodes(self, tree=TREE_OBJ):
        if tree == TREE_GC:
            return self.sb.gclist_blocks
        if tree == TREE_ORPHAN:
            return self.sb.orphan_blocks
        return self.sb.objtbl_blocks

    def set_tree_root(self, tree, root, nodes):
        if root != 0 and root >= self.sb.total_blocks:
            raise TsfsError("block %d is past the end of the volume" % root)
        if tree == TREE_GC:
            self.sb.gclist_start, self.sb.gclist_blocks = root, nodes
        elif tree == TREE_ORPHAN:
            self.sb.orphan_start, self.sb.orphan_blocks = root, nodes
        else:
            self.sb.objtbl_start, self.sb.objtbl_blocks = root, nodes
        self.sb_dirty = True

    def write_sb(self, both=False, spares=False):
        """The superblock, one generation up, to the copy the generation
        names (block generation & 1), as the kernel writes it. With both,
        the other copy too, a generation higher; with spares, the last one
        written goes to every spare as well."""
        if self.maps is not None:
            self.sb.free_blocks = self.maps.total_free()
        buf = None
        for _ in range(2 if both else 1):
            self.sb.generation += 1
            buf = self.sb.pack()
            self.raw_write(self.sb.generation & 1, buf)
        if spares:
            for s in self.sb.groups():
                if s.spare and s.n != 0:
                    self.raw_write(s.first + 1, buf)
        self.sb_dirty = False

    def flush(self, both=False, spares=False):
        """What changed, to the medium: maps and group heads, then the
        superblock."""
        if self.maps is not None and self.maps.dirty:
            self.maps.write(self)
            self.sb_dirty = True
        if self.sb_dirty or both or spares:
            self.write_sb(both=both, spares=spares)
        self.dev.flush()

    def close(self):
        if self.writable and not self.overlay:
            self.flush()
        self.dev.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0] is not None:
            self.dev.close()
            return False
        self.close()
        return False

    # ------------------------------------------------------------ objects

    def lookup(self, obj_uuid):
        return self.btree.lookup(obj_uuid)

    def object(self, obj_uuid, blk=None):
        if blk is None:
            blk = self.lookup(obj_uuid)
            if blk is None:
                raise TsfsError("no object %s" % obj_uuid)
        return ObjectBlock.load(self, blk, obj_uuid)

    def objects(self):
        """(UUID, block) of every object, in UUID order."""
        return list(self.btree.items())

    def garbage(self):
        return list(self.gctree.items())

    def orphans(self):
        """(UUID, block) of every object on the orphan tree."""
        return list(self.ortree.items())

    def any_object(self, obj_uuid):
        """An object from the index, or from the orphan tree when it was
        deleted while open, as the kernel's obj_load finds it."""
        blk = self.lookup(obj_uuid)
        if blk is None:
            blk = self.ortree.lookup(obj_uuid)
            if blk is None:
                raise TsfsError("no object %s" % obj_uuid)
        return ObjectBlock.load(self, blk, obj_uuid)
