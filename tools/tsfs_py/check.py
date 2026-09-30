"""Checking a volume: everything the kernel relies on, and the counts.

The checker only reads. It looks at the two superblocks and the spares,
the journal, every group head and free map, the three trees, every
object block with all its parts (those of the index, and those deleted
while open that only the orphan tree holds), the reference counts
against the link tables and the holds, the garbage list and the orphan
tree against the marks of the objects. Every block something points at
is claimed once; a block claimed twice, a claimed block the map calls
free, and a taken block nothing claims (a leak) are reported.

What it finds is a list of errors (the kernel would refuse the volume,
read something wrong, or lose data), warnings (the volume works but is
not as the kernel leaves it) and notes.

The reference count of an object is the number of link table entries of
the volume that point at it and are not external, plus its holds. An
object deleted while open, or one whose link table has to be made again,
is what a cut in the power leaves; the kernel finishes both when it
mounts the volume, so they are warnings, not errors, and the link table
of the second may not match its records.

A volume written by a kernel before stages T4 and T5 of design 11
carries the structural keys in its metadata, no link table and no hold.
When no object of the volume has a link table or a hold, the reference
counts cannot be compared with anything, so a difference is only a
note, and so are structural keys in the metadata.
"""

import struct

from . import head
from .btree import (INTERNAL, LEAF, TREE_GC, TREE_OBJ, TREE_ORPHAN,
                    TREE_NAMES, Node)
from .error import TsfsError
from .groups import fresh_map
from .layout import (BLOCK_SIZE, JRNL_AREA_BLOCKS, KNOWN_INCOMPAT,
                     KNOWN_ROCOMPAT, LABEL_MAX, MAGIC_NODE, ERR_NAMES,
                     NIL_UUID, ST_CLEAN, ST_ERROR, Superblock, layout)
from . import meta as metamod
from .obj import ICON_MAX, OB_ICON, is_icon
from .obj import (DATA_INLINE_MAX, F_DELETABLE, F_EDITABLE, F_GARBAGE,
                  F_ORPHAN, F_READABLE, F_RELINK, FORM_INLINE, FORM_NONE,
                  FORM_RUN, FORM_TREE, INL_MASK, INL_UNITS, INLINE_UNIT,
                  LK_EXTERNAL, LK_SIZE, LinkEntry, MAX_EXTENTS, MAX_LINK,
                  MAX_REC, MAX_RES, MB_PAY, OB_INLINE, OB_LINK, OB_META,
                  OB_NLINK, OB_NPLACE, OB_NRES, OB_PLACE, OB_RES, ObjectBlock,
                  PE_SIZE, PART_NAMES, PlaceEntry, REC_BIN, REC_XTAD, RID_ICON,
                  RID_PLACE, RS_SIZE, ResEntry, blks_for, unpack_table)

KNOWN_FLAGS = (F_EDITABLE | F_DELETABLE | F_READABLE | F_GARBAGE | F_ORPHAN
               | F_RELINK)


class Report(object):
    """What a check found."""

    def __init__(self):
        self.errors = []
        self.warnings = []
        self.notes = []
        self.stats = {}
        self.actions = []

    def error(self, where, what):
        self.errors.append((where, what))

    def warn(self, where, what):
        self.warnings.append((where, what))

    def note(self, where, what):
        self.notes.append((where, what))

    @property
    def ok(self):
        return not self.errors

    def to_dict(self):
        def items(lst):
            return [{"where": w, "what": t} for (w, t) in lst]
        return {"errors": items(self.errors), "warnings": items(self.warnings),
                "notes": items(self.notes), "stats": self.stats,
                "actions": list(self.actions)}


class ObjInfo(object):
    """What the check learnt about one object."""

    def __init__(self, obj_uuid, blk, orphan=False):
        self.uuid = obj_uuid
        self.blk = blk
        self.orphan = orphan            # held by the orphan tree only
        self.ok = False
        self.refcnt = 0
        self.pins = 0
        self.flags = 0
        self.links = []
        self.has_links = False
        self.struct_keys = []


class Checker(object):

    def __init__(self, vol, report=None):
        self.vol = vol
        self.sb = vol.sb
        self.rep = report or Report()
        self.claims = {}                # block -> what uses it
        self.broken = False             # something could not be read whole
        self.index = {}                 # UUID -> object block
        self.gc = {}
        self.orphan_tree = {}           # what the orphan tree holds
        self.orphan_objs = {}           # of it, the objects not in the index
        self.index_ok = False
        self.objs = {}
        self.tree_nodes = {TREE_OBJ: 0, TREE_GC: 0, TREE_ORPHAN: 0}
        self.leaked = []

    def where(self, info):
        return ("orphan %s" if info.orphan else "object %s") % info.uuid

    # ------------------------------------------------------------ claims

    def claim(self, blk, what, where):
        """A block something points at: it has to be a data block of a
        group, and nothing else may point at it."""
        sb = self.sb
        n = sb.group_of(blk) if blk < sb.total_blocks else None
        if n is None:
            self.rep.error(where, "%s points at block %d, which is in no group"
                           % (what, blk))
            self.broken = True
            return False
        s = self.vol.maps.shapes[n]
        if blk < s.data_first:
            self.rep.error(where, "%s points at block %d, which group %d uses "
                           "itself" % (what, blk, n))
            self.broken = True
            return False
        if blk in self.claims:
            self.rep.error(where, "%s takes block %d, which %s takes too"
                           % (what, blk, self.claims[blk]))
            return False
        self.claims[blk] = "%s (%s)" % (what, where)
        return True

    def claim_run(self, start, count, what, where):
        ok = True
        for b in range(start, start + count):
            ok = self.claim(b, what, where) and ok
        return ok

    # ------------------------------------------------------------ run

    def run(self):
        self.check_superblocks()
        self.check_journal()
        self.check_groups()
        self.index = self.check_tree(TREE_OBJ)
        self.index_ok = self.index is not None
        self.gc = self.check_tree(TREE_GC)
        self.orphan_tree = self.check_tree(TREE_ORPHAN)
        self.check_objects()
        self.check_orphans()
        self.check_counts()
        self.check_claims()
        return self.rep

    # ------------------------------------------------------------ superblock

    def check_superblocks(self):
        rep, sb, vol = self.rep, self.sb, self.vol
        for c in vol.sb_copies:
            name = "superblock %s" % "AB"[c.blk]
            if not c.ok:
                rep.warn(name, "cannot be used: %s" % c.error)
            elif c.sb.vol_uuid != sb.vol_uuid:
                rep.error(name, "belongs to volume %s" % c.sb.vol_uuid)
            else:
                rep.note(name, "generation %d" % c.sb.generation)
                if (c.sb.generation & 1) != c.blk:
                    rep.warn(name, "generation %d does not belong in block %d"
                             % (c.sb.generation, c.blk))
        where = "superblock"
        if sb.journal_start != 2:
            rep.error(where, "the journal starts at %d, not 2" % sb.journal_start)
        if sb.journal_blocks < JRNL_AREA_BLOCKS:
            rep.error(where, "the journal area is %d blocks, less than %d"
                      % (sb.journal_blocks, JRNL_AREA_BLOCKS))
        try:
            want = layout(sb.total_blocks)
            if want != (sb.journal_blocks, sb.ag_blocks, sb.ag_count):
                rep.warn(where, "the geometry (journal %d, %d groups of %d) is "
                         "not what a volume of %d blocks is made with (%d, %d "
                         "of %d)" % (sb.journal_blocks, sb.ag_count,
                                     sb.ag_blocks, sb.total_blocks, want[0],
                                     want[2], want[1]))
        except TsfsError as e:
            rep.warn(where, str(e))
        if sb.ag_first + (sb.ag_count - 1) * sb.ag_blocks >= sb.total_blocks:
            rep.error(where, "the last group starts past the end")
        if sb.total_blocks < vol.dev.nblk:
            rep.note(where, "%d blocks of the medium are past the volume"
                     % (vol.dev.nblk - sb.total_blocks))
        if b"\0" not in sb.label_raw:
            rep.warn(where, "the label fills its %d bytes with no NUL" % LABEL_MAX)
        if sb.incompat & ~KNOWN_INCOMPAT:
            rep.error(where, "incompatible features 0x%x: the kernel will not "
                      "open it" % sb.incompat)
        if sb.ro_compat & ~KNOWN_ROCOMPAT:
            rep.warn(where, "read-only features 0x%x: the kernel opens it read "
                     "only" % sb.ro_compat)
        if sb.state & ST_ERROR:
            rep.warn(where, "an error is recorded: %s at block %d; the kernel "
                     "opens it read only until it is checked"
                     % (ERR_NAMES.get(sb.err_kind, sb.err_kind), sb.err_blk))
        if not sb.state & ST_CLEAN:
            rep.warn(where, "it was not taken down properly")

        # the spares
        for s in sb.groups():
            if not s.spare or s.n == 0:
                continue
            w = "spare superblock in group %d" % s.n
            try:
                sp = Superblock.unpack(vol.read_block(s.first + 1))
            except TsfsError as e:
                rep.warn(w, "cannot be used: %s" % e)
                continue
            if sp.vol_uuid != sb.vol_uuid:
                rep.warn(w, "belongs to volume %s" % sp.vol_uuid)
            elif (sp.total_blocks, sp.ag_blocks, sp.ag_count, sp.journal_start,
                  sp.journal_blocks) != (sb.total_blocks, sb.ag_blocks,
                                         sb.ag_count, sb.journal_start,
                                         sb.journal_blocks):
                rep.warn(w, "describes another geometry")

    # ------------------------------------------------------------ journal

    def check_journal(self):
        rep = self.rep
        j = self.vol.journal
        where = "journal"
        if j is None or j.head_state == "none":
            rep.error(where, "there is no journal area")
            return
        if j.head_state == "empty":
            rep.note(where, "the head block has not been written yet (the "
                     "kernel writes it when the volume is first opened)")
        elif j.head_state == "damaged":
            rep.warn(where, "the head block cannot be used; the kernel starts "
                     "an empty ring at transaction %d" % j.seq_sb)
        else:
            rep.note(where, "head at %d, transaction %d" % (j.head, j.head_seq))
        if j.txns:
            rep.warn(where, "%d committed transaction(s) (%d to %d) are not in "
                     "place; the kernel puts them back when it opens the "
                     "volume, fsck.tsfs --fix does it now; what follows is "
                     "checked as they will leave it"
                     % (len(j.txns), j.txns[0].seq, j.txns[-1].seq))
        if j.stop_reason and j.txns:
            rep.note(where, "the ring ends: %s" % j.stop_reason)

    # ------------------------------------------------------------ groups

    def check_groups(self):
        rep, sb = self.rep, self.sb
        maps = self.vol.load_maps()
        head_sum = 0
        for s in maps.shapes:
            where = "group %d" % s.n
            for p in self.vol.group_problems.get(s.n, []):
                rep.error(where, "head: %s" % p)
                self.broken = True
            for k in self.vol.map_problems.get(s.n, []):
                rep.error(where, "map block %d (block %d) does not match the "
                          "CRC its head keeps" % (k, s.map_first + k))
            m = maps.maps[s.n]
            for off in range(s.meta):
                if not m[off >> 3] & (1 << (off & 7)):
                    rep.error(where, "block %d, which the group uses itself, is "
                              "free in its map" % (s.first + off))
            for off in range(s.len, s.map_blocks * 8 * BLOCK_SIZE):
                if not m[off >> 3] & (1 << (off & 7)):
                    rep.warn(where, "bits past the end of the group are clear")
                    break
            hf = maps.head_free.get(s.n)
            if hf is not None:
                head_sum += hf
                if hf != maps.free[s.n]:
                    rep.error(where, "the head says %d free, the map %d"
                              % (hf, maps.free[s.n]))
        if not self.vol.group_problems and sb.free_blocks != head_sum:
            rep.warn("superblock", "says %d blocks are free, the groups %d "
                     "(the kernel counts again when it opens the volume)"
                     % (sb.free_blocks, head_sum))

    # ------------------------------------------------------------ trees

    def check_tree(self, tree):
        """The items of a tree as {UUID: value}, or None when the index
        or the orphan tree cannot be walked.

        An internal node's level is its height above the leaves. One
        below its height is what a kernel before stage T5 left (it gave
        every new root level 1): a warning. Any other level is an
        error."""
        rep, vol = self.rep, self.vol
        tname = TREE_NAMES[tree]
        root = vol.tree_root(tree)
        nodes_sb = vol.tree_nodes(tree)
        items = {}
        if root == 0:
            if nodes_sb != 0:
                rep.warn(tname, "has no root but the superblock counts %d nodes"
                         % nodes_sb)
            return items
        leaves = []                     # (depth, block, node) in order
        seen = set()
        ok = [True]
        old_levels = []

        def visit(blk, lo, hi, depth):
            """The height of the node, or None when it cannot be read."""
            where = "%s node %d" % (tname, blk)
            if depth > 32:
                rep.error(where, "the tree is deeper than 32 levels")
                ok[0] = False
                return None
            if blk in seen:
                rep.error(where, "is reached twice")
                ok[0] = False
                return None
            seen.add(blk)
            if blk >= self.sb.total_blocks:
                rep.error(where, "is past the end of the volume")
                ok[0] = False
                return None
            self.claim(blk, "a node", tname)
            buf = vol.read_block(blk)
            why = head.problem(buf, self.sb.vol_uuid, MAGIC_NODE, blk)
            if why is not None:
                rep.error(where, why)
                ok[0] = False
                return None
            try:
                node = Node.unpack(buf)
            except TsfsError as e:
                rep.error(where, str(e))
                ok[0] = False
                return None
            if head.owner(buf) != tree:
                rep.error(where, "owner %d, not %d" % (head.owner(buf), tree))
            if node.tree != tree:
                rep.error(where, "says it is in tree %d" % node.tree)
            if node.kind not in (LEAF, INTERNAL):
                rep.error(where, "type %d" % node.kind)
                ok[0] = False
                return None
            keys = [k.bytes for k in node.keys]
            for i in range(1, len(keys)):
                if keys[i] <= keys[i - 1]:
                    rep.error(where, "key %d (%s) is not above the one before"
                              % (i, node.keys[i]))
                    ok[0] = False
            for i, k in enumerate(keys):
                if (lo is not None and k < lo) or (hi is not None and k >= hi):
                    rep.error(where, "key %s is outside what its parent sends "
                              "here" % node.keys[i])
                    ok[0] = False
            if node.kind == LEAF:
                if node.level != 0:
                    rep.warn(where, "a leaf at level %d" % node.level)
                leaves.append((depth, blk, node))
                return 0
            if node.child0 == 0:
                rep.error(where, "has no leftmost child")
                ok[0] = False
                return None
            bounds = [lo] + keys + [hi]
            height = None
            for i, child in enumerate(node.children()):
                if child == 0:
                    rep.error(where, "entry %d points nowhere" % (i - 1))
                    ok[0] = False
                    continue
                h = visit(child, bounds[i], bounds[i + 1], depth + 1)
                if h is not None and (height is None or h + 1 > height):
                    height = h + 1
            if height is not None and node.level != height:
                if 1 <= node.level < height:
                    old_levels.append(blk)
                else:
                    rep.error(where, "an internal node at level %d, %d above "
                              "its leaves" % (node.level, height))
            return height

        visit(root, None, None, 0)
        self.tree_nodes[tree] = len(seen)
        if old_levels:
            rep.warn(tname, "internal node(s) %s are at a level below their "
                     "height, as a kernel before stage T5 made new roots"
                     % ", ".join(str(b) for b in old_levels[:8])
                     + (" ..." if len(old_levels) > 8 else ""))
        if leaves:
            depths = set(d for (d, _b, _n) in leaves)
            if len(depths) > 1:
                rep.warn(tname, "leaves lie at depths %s"
                         % sorted(depths))
            for i, (_d, blk, node) in enumerate(leaves):
                want = leaves[i + 1][1] if i + 1 < len(leaves) else 0
                if node.next != want:
                    rep.error("%s node %d" % (tname, blk),
                              "the next leaf is %d, not %d" % (node.next, want))
                    ok[0] = False
                for k, v in zip(node.keys, node.values):
                    items[k] = v
            empty = sum(1 for (_d, _b, n) in leaves if not n.keys)
            if empty:
                rep.note(tname, "%d empty leaves" % empty)
        if len(seen) != nodes_sb:
            rep.warn(tname, "has %d nodes, the superblock counts %d"
                     % (len(seen), nodes_sb))
        rep.note(tname, "%d keys in %d nodes" % (len(items), len(seen)))
        if not ok[0]:
            self.broken = True
            if tree in (TREE_OBJ, TREE_ORPHAN):
                return None
        return items

    # ------------------------------------------------------------ objects

    def check_place_data(self, o, pl, what, where, inl):
        """A data part's place: its blocks claimed, its inline units noted."""
        rep = self.rep
        if pl.form == FORM_NONE:
            if pl.nbytes != 0:
                rep.error(where, "%s is nowhere but %d bytes" % (what, pl.nbytes))
            return
        if pl.form == FORM_INLINE:
            self.check_inline(pl, what, where, inl)
            if pl.nbytes > DATA_INLINE_MAX:
                rep.warn(where, "%s holds %d bytes inline, more than %d"
                         % (what, pl.nbytes, DATA_INLINE_MAX))
            return
        if pl.form not in (FORM_RUN, FORM_TREE):
            rep.error(where, "%s has form %d" % (what, pl.form))
            self.broken = True
            return
        if pl.form == FORM_TREE:
            if not self.claim(pl.a, "the extent leaf of %s" % what, where):
                return
        problems = []
        try:
            runs = o.extents(pl, check=lambda b, why: problems.append((b, why)))
        except TsfsError as e:
            rep.error(where, "%s: %s" % (what, e))
            self.broken = True
            return
        for b, why in problems:
            rep.error(where, "the extent leaf %d of %s: %s" % (b, what, why))
            self.broken = True
        if pl.form == FORM_TREE and pl.b != len(runs):
            rep.warn(where, "%s says %d runs, its leaf %d" % (what, pl.b, len(runs)))
        if len(runs) > MAX_EXTENTS:
            rep.error(where, "%s lies in %d runs" % (what, len(runs)))
        have = 0
        for (l, s, c) in runs:
            if l != have:
                rep.error(where, "%s: a run starts at block %d of the part, not "
                          "%d" % (what, l, have))
                self.broken = True
            if c == 0:
                rep.warn(where, "%s: a run of no blocks" % what)
            self.claim_run(s, c, what, where)
            have = l + c
        need = blks_for(pl.nbytes, BLOCK_SIZE)
        if have < need:
            rep.error(where, "%s is %d bytes in %d blocks" % (what, pl.nbytes, have))
        elif have > need:
            rep.warn(where, "%s is %d bytes and keeps %d blocks"
                     % (what, pl.nbytes, have))

    def check_inline(self, pl, what, where, inl):
        rep = self.rep
        off, units = pl.a, pl.b
        if off < OB_INLINE or (off - OB_INLINE) % INLINE_UNIT:
            rep.error(where, "%s sits at %d, not on a unit of the inline area"
                      % (what, off))
            self.broken = True
            return
        u = (off - OB_INLINE) // INLINE_UNIT
        if units == 0 or u + units > INL_UNITS:
            rep.error(where, "%s takes units %d+%d of the inline area"
                      % (what, u, units))
            self.broken = True
            return
        if pl.nbytes > units * INLINE_UNIT:
            rep.error(where, "%s is %d bytes in %d units" % (what, pl.nbytes, units))
            self.broken = True
        for k in range(u, u + units):
            if k in inl:
                rep.error(where, "%s and %s share unit %d of the inline area"
                          % (what, inl[k], k))
            else:
                inl[k] = what

    def check_mgmt(self, o, off, where, inl):
        """A management part: its place, its blocks, its bytes (or None)."""
        rep = self.rep
        pl = o.place(off)
        what = PART_NAMES[off]
        if pl.form == FORM_NONE:
            if pl.nbytes:
                rep.error(where, "the %s is nowhere but %d bytes" % (what, pl.nbytes))
            return b""
        if pl.form == FORM_INLINE:
            self.check_inline(pl, "the " + what, where, inl)
        elif pl.form == FORM_RUN:
            want = blks_for(pl.nbytes, MB_PAY)
            if pl.b != want:
                rep.error(where, "the %s of %d bytes has a run of %d blocks"
                          % (what, pl.nbytes, pl.b))
                self.broken = True
                if pl.b < want:
                    return None
            self.claim_run(pl.a, pl.b, "the " + what, where)
        else:
            rep.error(where, "the %s has form %d" % (what, pl.form))
            self.broken = True
            return None
        problems = []
        try:
            raw = o.read_mgmt(pl, check=lambda b, why: problems.append((b, why)))
        except TsfsError as e:
            rep.error(where, "the %s: %s" % (what, e))
            self.broken = True
            return None
        for b, why in problems:
            rep.error(where, "block %d of the %s: %s" % (b, what, why))
            self.broken = True
        return None if problems else raw

    def object_list(self):
        """(UUID, block, held by the orphan tree only) of every object."""
        out = [(u, b, False) for u, b in self.index.items()]
        for u, b in (self.orphan_tree or {}).items():
            if u not in self.index:
                out.append((u, b, True))
        return sorted(out, key=lambda x: x[0].bytes)

    def check_objects(self):
        rep = self.rep
        if self.index is None:
            rep.error("object index", "cannot be walked: the objects are not "
                      "looked at")
            return
        nrec = nres = nlink = 0
        for obj_uuid, blk, orphan in self.object_list():
            info = ObjInfo(obj_uuid, blk, orphan)
            self.objs[obj_uuid] = info
            where = self.where(info)
            if blk == 0 or blk >= self.sb.total_blocks:
                rep.error(where, "the %s points at block %d"
                          % ("orphan tree" if orphan else "index", blk))
                self.broken = True
                continue
            if not self.claim(blk, "the object block", where):
                continue
            try:
                o = ObjectBlock.load(self.vol, blk, obj_uuid)
            except TsfsError as e:
                rep.error(where, str(e))
                self.broken = True
                continue
            if orphan:
                self.orphan_objs[obj_uuid] = blk
            info.flags = o.flags
            info.refcnt = o.refcnt_signed
            info.pins = o.pins
            if o.flags & ~KNOWN_FLAGS:
                rep.warn(where, "unknown flags 0x%x" % (o.flags & ~KNOWN_FLAGS))
            if o.flags & F_ORPHAN:
                if not orphan:
                    rep.error(where, "is marked as deleted while open but is "
                              "in the index")
                else:
                    rep.warn(where, "was deleted while open; the kernel takes "
                             "it away when it mounts the volume, fsck.tsfs "
                             "--fix does it now")
                if o.flags & F_GARBAGE:
                    rep.warn(where, "is marked both as deleted while open and "
                             "as garbage")
            elif orphan:
                rep.error(where, "the orphan tree holds it, but it is neither "
                          "in the index nor marked as deleted while open")
            if o.flags & F_RELINK and not o.flags & F_ORPHAN:
                rep.warn(where, "its link table is marked to be made again; "
                         "the kernel does it when it mounts the volume, "
                         "fsck.tsfs --fix does it now")
            if o.nplace > MAX_REC:
                rep.error(where, "%d records, more than %d" % (o.nplace, MAX_REC))
            if o.nres > MAX_RES:
                rep.error(where, "%d resources, more than %d" % (o.nres, MAX_RES))
            if o.inlmap & ~INL_MASK:
                rep.error(where, "the inline map has bits past unit %d" % INL_UNITS)
            inl = {}

            raw_meta = self.check_mgmt(o, OB_META, where, inl)
            raw_icon = self.check_mgmt(o, OB_ICON, where, inl)
            if raw_icon:
                if len(raw_icon) > ICON_MAX:
                    rep.error(where, "the icon is %d bytes, more than %d"
                              % (len(raw_icon), ICON_MAX))
                if not is_icon(raw_icon):
                    rep.error(where, "the icon is neither an ICO nor a PNG")
            parts = {}
            for off, cnt_off, size, cls in ((OB_PLACE, OB_NPLACE, PE_SIZE, PlaceEntry),
                                            (OB_RES, OB_NRES, RS_SIZE, ResEntry),
                                            (OB_LINK, OB_NLINK, LK_SIZE, LinkEntry)):
                raw = self.check_mgmt(o, off, where, inl)
                cnt = struct.unpack_from("<I", o.hdr, cnt_off)[0]
                if raw is None:
                    parts[off] = None
                    continue
                if len(raw) != cnt * size:
                    rep.error(where, "the %s is %d bytes for %d entries"
                              % (PART_NAMES[off], len(raw), cnt))
                    self.broken = True
                    parts[off] = None
                    continue
                parts[off] = unpack_table(raw, cls, size)
            places, ress, links = parts[OB_PLACE], parts[OB_RES], parts[OB_LINK]

            # the metadata
            d = None
            if raw_meta is not None:
                if metamod.is_object(raw_meta):
                    try:
                        d = metamod.loads(raw_meta)
                    except ValueError as e:
                        rep.warn(where, "the metadata is not JSON after its "
                                 "outer object: %s" % e)
                        d = metamod.first_value(raw_meta)
                elif metamod.is_json(raw_meta):
                    rep.note(where, "the metadata is JSON but not an object; "
                             "it is kept as it is and the records are in rid "
                             "order")
                else:
                    rep.warn(where, "the metadata is not JSON")
            recs = metamod.order_parse(raw_meta) if raw_meta is not None else None
            if d is not None and "records" in d and recs is None:
                rep.warn(where, "\"records\" is not a list the kernel reads "
                         "(of {rid, rt, sub}, rid not 0, at most %d); the "
                         "records are taken in rid order" % MAX_REC)
            if d is not None:
                info.struct_keys = metamod.structural(d)

            # records
            rids = []
            if places is not None:
                nrec += len(places)
                last = 0
                for i, e in enumerate(places):
                    w = "record rid %d" % e.rid
                    if e.rid == 0 or e.rid <= last:
                        rep.error(where, "the placement table is not in rid order "
                                  "at entry %d (rid %d)" % (i, e.rid))
                    if e.rid >= o.nextrid:
                        rep.error(where, "rid %d is not below the next rid %d"
                                  % (e.rid, o.nextrid))
                    if e.kind not in (REC_XTAD, REC_BIN):
                        rep.error(where, "%s is of kind %d" % (w, e.kind))
                    if e.nbytes != e.place.nbytes:
                        rep.error(where, "%s says %d bytes, its place %d"
                                  % (w, e.nbytes, e.place.nbytes))
                    self.check_place_data(o, e.place, w, where, inl)
                    last = max(last, e.rid)
                    rids.append(e.rid)
            if recs is not None and places is not None:
                rrids = [r[0] for r in recs]
                if len(rrids) == len(rids) and all(r in rids for r in rrids):
                    if len(set(rrids)) != len(rrids):
                        rep.error(where, "\"records\" names a rid twice, and "
                                  "leaves out rid(s) %s, which cannot be reached"
                                  % sorted(set(rids) - set(rrids)))
                else:
                    rep.warn(where, "\"records\" names rids %s, the placement "
                             "table %s; the kernel takes the records in rid "
                             "order" % (rrids, rids))
                for rid, rt, sub in recs:
                    if not 0 <= rt <= 31 or not 0 <= sub <= 0xFFFF:
                        rep.warn(where, "record rid %d has type %d subtype %d"
                                 % (rid, rt, sub))

            # resources
            if ress is not None:
                nres += len(ress)
                seen = set()
                for e in ress:
                    w = "resource (%s, %d)" % (
                        "icon" if e.owner == RID_ICON else
                        "position %d" % (e.owner & ~RID_PLACE)
                        if e.owner & RID_PLACE else "rid %d" % e.owner, e.resno)
                    if (e.owner, e.resno) in seen:
                        rep.error(where, "%s is there twice" % w)
                    if e.owner == RID_ICON and raw_icon:
                        rep.warn(where, "the icon is kept both as a part and as "
                                 "a resource; the part is the one read")
                    seen.add((e.owner, e.resno))
                    if (e.owner != RID_ICON and not e.owner & RID_PLACE
                            and e.owner not in rids and places is not None):
                        rep.warn(where, "%s belongs to no record" % w)
                    if e.ext_raw is not None and b"\0" not in e.ext_raw:
                        rep.warn(where, "%s: the extension has no NUL" % w)
                    if e.nbytes != e.place.nbytes:
                        rep.error(where, "%s says %d bytes, its place %d"
                                  % (w, e.nbytes, e.place.nbytes))
                    self.check_place_data(o, e.place, w, where, inl)

            # links
            if links is not None:
                nlink += len(links)
                info.links = links
                info.has_links = bool(links)
                if len(links) > MAX_LINK:
                    rep.warn(where, "%d links, more than the kernel makes (%d)"
                             % (len(links), MAX_LINK))
                for i in range(1, len(links)):
                    if links[i].key() < links[i - 1].key():
                        rep.error(where, "the link table is not in order at "
                                  "entry %d" % i)
                vseen = set()
                for e in links:
                    if e.vobjid != NIL_UUID:
                        if e.vobjid in vseen:
                            rep.warn(where, "vobjid %s is in the link table "
                                     "twice" % e.vobjid)
                        vseen.add(e.vobjid)
                    if e.reserved:
                        rep.warn(where, "link %s has its reserved field set"
                                 % e.vobjid)
                    if e.flags & ~LK_EXTERNAL:
                        rep.warn(where, "link %s has flags 0x%x" % (e.vobjid, e.flags))
                    if places is not None and e.rid not in rids:
                        rep.warn(where, "link %s is in rid %d, which is no record"
                                 % (e.vobjid, e.rid))

            # the inline area
            used = 0
            for k in inl:
                used |= 1 << k
            m = o.inlmap & INL_MASK
            if m & ~used:
                rep.warn(where, "inline units %s are taken but nothing is there"
                         % [k for k in range(INL_UNITS) if (m & ~used) >> k & 1])
            if used & ~m:
                rep.error(where, "inline units %s are used but free in the map"
                          % [k for k in range(INL_UNITS) if (used & ~m) >> k & 1])
            info.ok = raw_meta is not None and None not in (places, ress, links)
            if not info.ok:
                self.broken = True
        self.rep.stats.update({"objects": len(self.index), "records": nrec,
                               "resources": nres, "links": nlink,
                               "orphans": len(self.orphan_objs)})

    # ------------------------------------------------------------ the orphan tree

    def check_orphans(self):
        """The orphan tree against the marks: each entry an object deleted
        while open (out of the index) or one whose link table has to be
        made again (in the index, at the same block), and every object
        so marked on it."""
        rep = self.rep
        tree = self.orphan_tree
        if tree is None:
            rep.error("orphan tree", "cannot be walked: the objects deleted "
                      "while open are not looked at")
            return
        if self.index is None:
            return
        w = "orphan tree"
        for u, v in sorted(tree.items(), key=lambda kv: kv[0].bytes):
            info = self.objs.get(u)
            if info is None or info.orphan:
                continue                # looked at with the objects
            if v != info.blk:
                rep.error(w, "%s points at block %d, the index at %d"
                          % (u, v, info.blk))
            if (info.ok or info.flags) and not info.flags & (F_RELINK | F_ORPHAN):
                rep.warn(w, "holds %s, which has nothing to be finished; the "
                         "kernel drops the entry when it mounts the volume, "
                         "fsck.tsfs --fix does it now" % u)
        for u, info in sorted(self.objs.items(), key=lambda kv: kv[0].bytes):
            if info.flags & F_RELINK and not info.flags & F_ORPHAN \
                    and u not in tree:
                rep.warn("object %s" % u, "is marked for its link table to be "
                         "made again but is not on the orphan tree: the "
                         "kernel does it only when the object is next closed "
                         "after a write; fsck.tsfs --fix does it now")
        self.rep.stats["orphan_tree"] = len(tree)

    # ------------------------------------------------------------ counts

    def legacy_volume(self):
        """True when no object has a link table or a hold: the kernel that
        wrote the volume keeps neither yet, and its counts come from
        elsewhere."""
        return not any(i.has_links or i.pins for i in self.objs.values())

    def computed_counts(self):
        """The count each object should have: its holds and the link
        table entries of the volume that point at it and are counted."""
        counts = {}
        for u, info in self.objs.items():
            counts[u] = info.pins
        for info in self.objs.values():
            for e in info.links:
                if e.counted and e.target in counts:
                    counts[e.target] += 1
        return counts

    def check_counts(self):
        rep = self.rep
        if not self.index_ok:
            return
        refs = {}
        for name, u in self.sb.roots():
            refs[u] = refs.get(u, 0) + 1
            if u not in self.index:
                rep.error("superblock", "the %s object %s is not on the volume"
                          % (name, u))
        legacy = self.legacy_volume()
        for info in self.objs.values():
            for e in info.links:
                here = e.target in self.objs
                if e.flags & LK_EXTERNAL and e.target in self.index:
                    rep.note(self.where(info), "link %s is external but %s is "
                             "on this volume now (it is counted when the table "
                             "is next made)" % (e.vobjid, e.target))
                elif not e.flags & LK_EXTERNAL and not here:
                    rep.warn(self.where(info), "link %s points at %s, which "
                             "is not on this volume" % (e.vobjid, e.target))
            if info.struct_keys:
                msg = ("the metadata keeps %s, which the object block holds"
                       % ", ".join(info.struct_keys))
                if legacy:
                    rep.note(self.where(info), msg)
                else:
                    rep.warn(self.where(info), msg)
        if legacy and self.objs:
            rep.note("reference counts", "no object has a link table or a "
                     "hold (a volume written before stages T4/T5, or one "
                     "nothing links in): the counts are not held against them")
        counts = self.computed_counts()
        bad = 0
        for u, info in sorted(self.objs.items(), key=lambda kv: kv[0].bytes):
            if not info.ok and info.refcnt == 0 and not info.flags:
                continue
            where = self.where(info)
            if info.pins > info.refcnt:
                rep.error(where, "holds %d, more than its reference count %d"
                          % (info.pins, info.refcnt))
            if counts.get(u, 0) != info.refcnt:
                bad += 1
                msg = ("the reference count is %d, the link tables and %d "
                       "hold(s) make it %d" % (info.refcnt, info.pins,
                                               counts.get(u, 0)))
                if legacy:
                    rep.note(where, msg)
                else:
                    rep.error(where, msg)
            if u in refs and info.pins < refs[u] and not legacy:
                rep.warn(where, "the superblock names it %d time(s) but it "
                         "has %d hold(s); fsck.tsfs --relink gives it them"
                         % (refs[u], info.pins))
        self.rep.stats["count_mismatches"] = bad

        # the garbage list
        if self.gc is None:
            return
        for u, v in self.gc.items():
            w = "garbage list"
            info = self.objs.get(u)
            if info is None or info.orphan:
                rep.error(w, "%s is not in the index" % u)
                continue
            if v != info.blk:
                rep.error(w, "%s points at block %d, the index at %d"
                          % (u, v, info.blk))
            if info.ok or info.flags:
                if not info.flags & F_GARBAGE:
                    rep.error(w, "%s is on it but not marked as garbage" % u)
                if info.refcnt != 0:
                    rep.error(w, "%s is on it with a reference count of %d"
                              % (u, info.refcnt))
        for u, info in self.objs.items():
            if info.flags & F_GARBAGE and not info.orphan and u not in self.gc:
                rep.error("object %s" % u, "is marked as garbage but is not on "
                          "the garbage list")
        self.rep.stats["garbage"] = len(self.gc)

    # ------------------------------------------------------------ maps

    def claimed_maps(self):
        """The maps as the claims make them: each group's own blocks and
        the bits past its end taken, and every claimed block."""
        out = []
        for s in self.vol.maps.shapes:
            out.append(fresh_map(s))
        for blk in self.claims:
            n = self.sb.group_of(blk)
            off = blk - self.vol.maps.shapes[n].first
            out[n][off >> 3] |= 1 << (off & 7)
        return out

    def check_claims(self):
        rep = self.rep
        maps = self.vol.maps
        want = self.claimed_maps()
        leaked = []
        lost = 0
        for s in maps.shapes:
            have = maps.maps[s.n]
            if have == want[s.n]:
                continue
            w, h = want[s.n], have
            for i in range(s.meta >> 3, (s.len + 7) >> 3):
                if w[i] == h[i]:
                    continue
                for bit in range(8):
                    off = i * 8 + bit
                    if off < s.meta or off >= s.len:
                        continue
                    wb = w[i] >> bit & 1
                    hb = h[i] >> bit & 1
                    if wb and not hb:
                        lost += 1
                        rep.error("group %d" % s.n, "block %d is used by %s but "
                                  "free in the map" % (s.first + off,
                                                       self.claims[s.first + off]))
                    elif hb and not wb:
                        leaked.append(s.first + off)
        if leaked:
            show = ", ".join(str(b) for b in leaked[:12])
            rep.warn("free maps", "%d block(s) are taken but nothing uses them "
                     "(%s%s)%s" % (len(leaked), show,
                                   " ..." if len(leaked) > 12 else "",
                                   "" if self.broken else
                                   "; fsck.tsfs --fix gives them back"))
        used = sum(s.len for s in maps.shapes) - maps.total_free()
        self.rep.stats.update({"blocks": self.sb.total_blocks,
                               "free": maps.total_free(), "used": used,
                               "leaked": len(leaked), "claimed_free": lost})
        self.leaked = leaked
