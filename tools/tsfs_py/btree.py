"""The three B+trees of a volume: the object index, the garbage list and
the orphan tree.

A node is one block with the common head ("TFBT", owner = the tree
number), then:

     64  type (0 leaf, 1 internal)     65  level (0 at the leaves)
     66  number of keys (16)           72  next leaf, 0 at the last
     80  leftmost child (internal)     88  the tree (32)
     96  entries: a UUID (16) and a value (64), 166 of them at most

In an internal node child0 takes every key below the first separator
and entry i's value takes the keys from separator i up to the next one.
Keys live in the leaves, which are chained left to right. The object
index maps a UUID to its object block; the garbage list holds the
objects whose reference count has fallen to zero, and the orphan tree the
objects that were deleted while open (out of the index) or whose link
table has to be made again (still in the index), with the same value.

A root added over a split one is a level above it. Kernels before stage
T5 gave every new root level 1, so a volume they grew past two levels has
internal nodes whose level is below their height.

The kernel does not merge nodes: a leaf may be empty and an internal
node may hold no separator, only child0.
"""

import struct
import uuid

from . import head
from .error import TsfsError
from .layout import BLOCK_SIZE, MAGIC_NODE

HDR_SIZE = 96
ENT_SIZE = 24
MAX_KEYS = (BLOCK_SIZE - HDR_SIZE) // ENT_SIZE

LEAF = 0
INTERNAL = 1

TREE_OBJ = 0
TREE_GC = 1
TREE_ORPHAN = 2
TREE_NAMES = {TREE_OBJ: "object index", TREE_GC: "garbage list",
              TREE_ORPHAN: "orphan tree"}

OFF_TYPE = 64
OFF_LEVEL = 65
OFF_NKEYS = 66
OFF_NEXT = 72
OFF_CHILD0 = 80
OFF_TREE = 88

MAX_DEPTH = 32


class Node(object):
    """One node, as Python values."""

    def __init__(self, kind=LEAF, level=0, tree=TREE_OBJ):
        self.kind = kind
        self.level = level
        self.tree = tree
        self.next = 0
        self.child0 = 0
        self.keys = []                  # UUIDs
        self.values = []                # block numbers

    @property
    def is_leaf(self):
        return self.kind == LEAF

    def children(self):
        return [self.child0] + list(self.values)

    def pack(self, vol_uuid, gen, blk):
        if len(self.keys) > MAX_KEYS:
            raise TsfsError("%d keys do not fit a node" % len(self.keys))
        buf = bytearray(BLOCK_SIZE)
        buf[OFF_TYPE] = self.kind
        buf[OFF_LEVEL] = self.level
        struct.pack_into("<H", buf, OFF_NKEYS, len(self.keys))
        struct.pack_into("<QQI", buf, OFF_NEXT, self.next, self.child0,
                         self.tree)
        for i, (k, v) in enumerate(zip(self.keys, self.values)):
            off = HDR_SIZE + i * ENT_SIZE
            buf[off:off + 16] = k.bytes
            struct.pack_into("<Q", buf, off + 16, v)
        return head.seal(buf, vol_uuid, gen, MAGIC_NODE, blk, self.tree)

    @classmethod
    def unpack(cls, buf):
        n = cls()
        n.kind = buf[OFF_TYPE]
        n.level = buf[OFF_LEVEL]
        nkeys = struct.unpack_from("<H", buf, OFF_NKEYS)[0]
        n.next, n.child0, n.tree = struct.unpack_from("<QQI", buf, OFF_NEXT)
        if nkeys > MAX_KEYS:
            raise TsfsError("%d keys, more than a node holds" % nkeys)
        for i in range(nkeys):
            off = HDR_SIZE + i * ENT_SIZE
            n.keys.append(uuid.UUID(bytes=bytes(buf[off:off + 16])))
            n.values.append(struct.unpack_from("<Q", buf, off + 16)[0])
        return n


class BTree(object):
    """One of the three trees of a volume."""

    def __init__(self, vol, tree):
        self.vol = vol
        self.tree = tree

    @property
    def name(self):
        return TREE_NAMES.get(self.tree, "tree %d" % self.tree)

    def root(self):
        return self.vol.tree_root(self.tree)

    def nodes(self):
        return self.vol.tree_nodes(self.tree)

    def read_node(self, blk):
        """A node, checked as the kernel's node_read checks it."""
        buf = self.vol.read_block(blk)
        why = head.problem(buf, self.vol.sb.vol_uuid, MAGIC_NODE, blk)
        if why is not None:
            raise TsfsError("node %d of the %s: %s" % (blk, self.name, why))
        return Node.unpack(buf)

    def write_node(self, blk, node):
        self.vol.write_block(blk, node.pack(self.vol.sb.vol_uuid,
                                            self.vol.sb.journal_seq, blk))

    # ------------------------------------------------------------ reading

    @staticmethod
    def _child(node, key):
        """The child of an internal node a key belongs to."""
        lo, hi = 0, len(node.keys)
        kb = key.bytes
        while lo < hi:
            mid = (lo + hi) // 2
            c = node.keys[mid].bytes
            if c == kb:
                return node.values[mid]
            if c < kb:
                lo = mid + 1
            else:
                hi = mid
        return node.child0 if lo == 0 else node.values[lo - 1]

    def lookup(self, key):
        """The value of a key, or None."""
        blk = self.root()
        if blk == 0:
            return None
        for _ in range(MAX_DEPTH):
            node = self.read_node(blk)
            if node.is_leaf:
                for k, v in zip(node.keys, node.values):
                    if k == key:
                        return v
                return None
            blk = self._child(node, key)
            if blk == 0:
                raise TsfsError("a separator of the %s points nowhere" % self.name)
        raise TsfsError("the %s is deeper than any real tree" % self.name)

    def leftmost(self):
        blk = self.root()
        if blk == 0:
            return 0
        for _ in range(MAX_DEPTH):
            node = self.read_node(blk)
            if node.is_leaf:
                return blk
            blk = node.child0
            if blk == 0:
                raise TsfsError("the %s has an internal node with no child"
                               % self.name)
        raise TsfsError("the %s is deeper than any real tree" % self.name)

    def items(self):
        """Every (key, value) in order, along the chain of leaves."""
        blk = self.leftmost()
        seen = set()
        while blk != 0:
            if blk in seen:
                raise TsfsError("the leaves of the %s go round in a circle"
                               % self.name)
            seen.add(blk)
            node = self.read_node(blk)
            for k, v in zip(node.keys, node.values):
                yield k, v
            blk = node.next

    def node_blocks(self):
        """Every node of the tree, walked from the root."""
        out = []
        blk = self.root()
        if blk == 0:
            return out
        todo = [blk]
        seen = set()
        while todo:
            b = todo.pop()
            if b in seen:
                continue
            seen.add(b)
            out.append(b)
            node = self.read_node(b)
            if not node.is_leaf:
                todo.extend(c for c in node.children() if c != 0)
        return out

    # ------------------------------------------------------------ building

    def drop(self):
        """Give every node back and leave the tree empty."""
        try:
            blocks = self.node_blocks()
        except TsfsError:
            blocks = []
        for b in blocks:
            self.vol.free_ext(b, 1)
        self.vol.set_tree_root(self.tree, 0, 0)

    def build(self, items):
        """Make the tree afresh from (key, value) pairs, bottom up: full
        leaves chained left to right, then as many levels of internal
        nodes as it takes. The tree must be empty."""
        items = sorted(items, key=lambda kv: kv[0].bytes)
        for i in range(1, len(items)):
            if items[i][0] == items[i - 1][0]:
                raise TsfsError("key %s is there twice" % items[i][0])
        for _k, v in items:
            if v == 0:
                raise TsfsError("a value of 0 cannot go in a tree")
        if self.root() != 0:
            raise TsfsError("the %s is not empty" % self.name)
        if not items:
            self.vol.set_tree_root(self.tree, 0, 0)
            return 0

        # the leaves
        level = []                      # (first key, block, node)
        for i in range(0, len(items), MAX_KEYS):
            node = Node(LEAF, 0, self.tree)
            chunk = items[i:i + MAX_KEYS]
            node.keys = [k for k, _v in chunk]
            node.values = [v for _k, v in chunk]
            level.append([chunk[0][0], self.vol.alloc_blk(), node])
        for i in range(len(level) - 1):
            level[i][2].next = level[i + 1][1]
        count = len(level)
        pending = list(level)

        # the levels above, each node taking up to MAX_KEYS + 1 children
        depth = 0
        while len(level) > 1:
            depth += 1
            upper = []
            for i in range(0, len(level), MAX_KEYS + 1):
                group = level[i:i + MAX_KEYS + 1]
                node = Node(INTERNAL, depth, self.tree)
                node.child0 = group[0][1]
                node.keys = [g[0] for g in group[1:]]
                node.values = [g[1] for g in group[1:]]
                upper.append([group[0][0], self.vol.alloc_blk(), node])
            count += len(upper)
            pending.extend(upper)
            level = upper

        for _k, blk, node in pending:
            self.write_node(blk, node)
        self.vol.set_tree_root(self.tree, level[0][1], count)
        return count

    def rebuild(self, items):
        self.drop()
        return self.build(items)
