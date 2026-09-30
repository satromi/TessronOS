"""Putting a volume right after a check.

fix: the free maps made again from what the check found in use (only
adding to them when something could not be read whole, so that nothing
of a damaged object is handed out again), every group head written with
its counts and map CRCs, the node counts and free count of the
superblock, both superblocks and the spares, the error mark cleared.
The journal has been replayed by the time the volume is open.

tidy: what the kernel finishes when it mounts a volume (ts_obj_tidy),
walking the orphan tree in key order: an object deleted while open is
taken away, its links given up first; an object whose link table has to
be made again gets it, and the counts of what it points at move by the
difference; an entry for an object with neither mark is dropped.

relink: every object's link table made again from its xmlTAD records,
and every reference count from the link tables and the holds. An object
whose count falls to zero goes on the garbage list, one that is referred
to again comes off it. Objects deleted while open keep their tables,
which still count until they are taken away.

The link tables are made as the kernel's lk_build makes them, and the
counts move as its ref_move moves them: a count never falls below the
holds, and one that reaches zero puts the object on the garbage list
unless it is an orphan.
"""

from .btree import TREE_GC, TREE_OBJ, TREE_ORPHAN
from .error import TsfsError
from .groups import count_free
from .layout import ST_CLEAN, ST_ERROR, now_tron
from . import meta as metamod
from .obj import (F_GARBAGE, F_ORPHAN, F_RELINK, LK_EXTERNAL, MAX_LINK,
                  OB_LINK, OB_META, OB_NLINK, OB_PLACE, OB_RES, LinkEntry,
                  ObjectBlock, sort_links)


def fix(vol, checker):
    """Rebuild the maps and heads from a finished check. Answers a list
    of what was done."""
    done = []
    maps = vol.load_maps()
    want = checker.claimed_maps()
    changed = 0
    for s in maps.shapes:
        new = want[s.n]
        if checker.broken:
            old = maps.maps[s.n]
            new = bytearray(a | b for a, b in zip(new, old))
        if new != maps.maps[s.n]:
            changed += 1
        maps.maps[s.n] = new
        maps.free[s.n] = count_free(new, s)
    maps.dirty = set(range(len(maps.shapes)))
    if changed:
        done.append("the free maps of %d group(s) were made again" % changed)
    if vol.group_problems or vol.map_problems:
        done.append("group heads were written again")
    if checker.broken:
        done.append("something could not be read whole: blocks were only "
                    "marked taken, none given back")

    sb = vol.sb
    nodes = checker.tree_nodes
    if checker.index is not None and sb.objtbl_blocks != nodes[TREE_OBJ]:
        sb.objtbl_blocks = nodes[TREE_OBJ]
        done.append("the index counts %d nodes" % nodes[TREE_OBJ])
    if checker.gc is not None and sb.gclist_blocks != nodes[TREE_GC]:
        sb.gclist_blocks = nodes[TREE_GC]
        done.append("the garbage list counts %d nodes" % nodes[TREE_GC])
    if checker.orphan_tree is not None and \
            sb.orphan_blocks != nodes[TREE_ORPHAN]:
        sb.orphan_blocks = nodes[TREE_ORPHAN]
        done.append("the orphan tree counts %d nodes" % nodes[TREE_ORPHAN])
    if sb.state & ST_ERROR:
        sb.state &= ~ST_ERROR
        sb.err_kind = 0
        sb.err_blk = 0
        done.append("the error mark was cleared")
    sb.state |= ST_CLEAN
    sb.fsck_time = now_tron()
    j = vol.journal
    if j is not None and j.head_state == "damaged":
        j.write_head(sb.journal_seq, 0)
        done.append("the journal head was written afresh")
    vol.sb_dirty = True
    vol.flush(both=True, spares=True)
    done.append("both superblocks and the spares were written")
    return done


# ---------------------------------------------------------------- link tables

def links_of(o, raw, places, here):
    """The link table an object's records call for, as the kernel's
    lk_build makes it: the records of type 1 in their order, the links
    each holds, a vobjid met before in the object left out; a target not
    in `here` (the object index) external; the whole in the order of the
    entries' first 40 bytes."""
    order, _named = metamod.order_of(raw, places)
    found = []
    for e, rt, _sub in order:
        if rt != metamod.RT_XTAD or e.nbytes == 0:
            continue
        if not metamod.scan_links(o.read_data(e.place), e.rid, found, MAX_LINK):
            raise TsfsError("more than %d links" % MAX_LINK)
    return sort_links([LinkEntry(v, t, rid, 0 if t in here else LK_EXTERNAL)
                       for (v, t, rid) in found])


def sb_refs(sb):
    """How many fields of the superblock name each object."""
    out = {}
    for _name, u in sb.roots():
        out[u] = out.get(u, 0) + 1
    return out


class Objects(object):
    """Objects of a volume being changed together: loaded once, the
    counts moved as the kernel moves them, stored at the end."""

    def __init__(self, vol, checker):
        self.vol = vol
        self.index = dict(checker.index)
        self.orphans = dict(checker.orphan_tree or {})
        self.gc = dict(checker.gc or {})
        self.gc_changed = False
        self.or_changed = False
        self.cache = {}
        self.dirty = set()

    def load(self, u):
        """An object as the kernel's obj_load finds it: the index first,
        then the orphan tree. None when it is in neither."""
        if u in self.cache:
            return self.cache[u]
        blk = self.index.get(u)
        if blk is None:
            blk = self.orphans.get(u)
        if blk is None:
            return None
        o = ObjectBlock.load(self.vol, blk, u)
        self.cache[u] = o
        return o

    def ref_move(self, u, dlinks, dpins=0):
        """The count of u moved by links and holds (ref_move)."""
        try:
            o = self.load(u)
        except TsfsError:
            return
        if o is None:
            return                      # one gone meanwhile has nothing to count
        pins = o.pins + dpins
        cur = o.refcnt_signed + dlinks + dpins
        if pins < 0:
            return
        if cur < pins:
            cur = pins
        flags = o.flags
        if cur == 0 and not flags & (F_GARBAGE | F_ORPHAN):
            self.gc[u] = o.blk
            self.gc_changed = True
            flags |= F_GARBAGE
        elif cur > 0 and flags & F_GARBAGE:
            self.gc.pop(u, None)
            self.gc_changed = True
            flags &= ~F_GARBAGE
        o.refcnt = cur
        o.pins = pins
        o.flags = flags
        self.dirty.add(u)

    def reap(self, u):
        """An orphan taken away: its links given up, then everything it
        holds given back and its keys taken out of the trees."""
        o = self.cache[u]
        for e in reversed(o.links()):
            if e.counted:
                self.ref_move(e.target, -1)
        for e in o.placements():
            o.dseg_free(e.place)
        for e in o.resources():
            o.dseg_free(e.place)
        for off in (OB_PLACE, OB_RES, OB_LINK, OB_META):
            o.mseg_free(off)
        if o.flags & F_GARBAGE and self.gc.pop(u, None) is not None:
            self.gc_changed = True
        if self.orphans.pop(u, None) is not None:
            self.or_changed = True
        if not o.flags & F_ORPHAN:
            self.index.pop(u, None)
        self.vol.free_ext(o.blk, 1)
        del self.cache[u]
        self.dirty.discard(u)

    def relink(self, u):
        """The link table of u made again and the counts moved by the
        difference; the mark taken off."""
        o = self.cache[u]
        now = links_of(o, o.meta(), o.placements(), self.index)
        old = o.links()
        nxt = []
        deltas = []
        a = b = 0
        while a < len(old) or b < len(now):
            if a >= len(old):
                c = 1
            elif b >= len(now):
                c = -1
            else:
                ka, kb = old[a].key(), now[b].key()
                c = 0 if ka == kb else (-1 if ka < kb else 1)
            if c == 0:
                nxt.append(old[a])
                a += 1
                b += 1
            elif c < 0:
                if old[a].counted:
                    deltas.append((old[a].target, -1))
                a += 1
            else:
                nxt.append(now[b])
                if now[b].counted:
                    deltas.append((now[b].target, 1))
                b += 1
        o.set_table(OB_LINK, OB_NLINK, nxt)
        o.flags = o.flags & ~F_RELINK
        if self.orphans.pop(u, None) is not None:
            self.or_changed = True
        self.dirty.add(u)
        for t, d in deltas:
            self.ref_move(t, d)
        return len(deltas)

    def finish(self, done):
        """Every changed object stored, and the garbage list and the
        orphan tree made again when they changed."""
        for u in sorted(self.dirty, key=lambda x: x.bytes):
            if u in self.cache:
                self.cache[u].store()
        if self.gc_changed:
            self.vol.gctree.rebuild(sorted(self.gc.items(),
                                           key=lambda kv: kv[0].bytes))
            done.append("the garbage list holds %d object(s)" % len(self.gc))
        if self.or_changed:
            self.vol.ortree.rebuild(sorted(self.orphans.items(),
                                           key=lambda kv: kv[0].bytes))
            done.append("the orphan tree holds %d object(s)" % len(self.orphans))
        self.vol.flush()


def tidy_needed(checker):
    """Whether the orphan tree holds anything, or an object is marked for
    its link table to be made again."""
    return bool(checker.orphan_tree) or any(
        i.flags & F_RELINK for i in checker.objs.values())


def tidy(vol, checker):
    """Finish what the orphan tree holds, as the kernel does at mount.
    Answers a list of what was done."""
    if checker.index is None or checker.orphan_tree is None:
        raise TsfsError("the index or the orphan tree cannot be walked; the "
                       "orphans are left alone")
    done = []
    objs = Objects(vol, checker)
    reaped = relinked = dropped = 0
    for u in sorted(checker.orphan_tree, key=lambda x: x.bytes):
        if u not in objs.orphans:
            continue
        try:
            o = objs.load(u)
            flags = o.flags if o is not None else 0
            if flags & F_ORPHAN:
                objs.reap(u)
                reaped += 1
            elif flags & F_RELINK:
                objs.relink(u)
                relinked += 1
            else:
                objs.orphans.pop(u, None)
                objs.or_changed = True
                dropped += 1
        except TsfsError as e:
            done.append("object %s on the orphan tree left alone: %s" % (u, e))
    # marked but not on the tree: the kernel would wait for the next close
    for u, info in sorted(checker.objs.items(), key=lambda kv: kv[0].bytes):
        if info.orphan or u in checker.orphan_tree:
            continue
        if info.flags & F_RELINK and not info.flags & F_ORPHAN:
            try:
                objs.load(u)
                objs.relink(u)
                relinked += 1
            except TsfsError as e:
                done.append("object %s left alone: %s" % (u, e))
    if reaped:
        done.append("%d object(s) deleted while open were taken away" % reaped)
    if relinked:
        done.append("%d link table(s) marked to be made again were made"
                    % relinked)
    if dropped:
        done.append("%d entr(ies) of the orphan tree with nothing to finish "
                    "were dropped" % dropped)
    objs.finish(done)
    return done


def relink(vol, checker):
    """Link tables and reference counts made again. Answers a list of
    what was done."""
    if checker.index is None:
        raise TsfsError("the index cannot be walked; the links are left alone")
    done = []
    here = dict(checker.index)
    objs = Objects(vol, checker)
    tables = 0
    loaded = []
    for u in sorted(here, key=lambda x: x.bytes):
        try:
            o = objs.load(u)
            new = links_of(o, o.meta(), o.placements(), here)
            old = o.links()
        except TsfsError as e:
            done.append("object %s left alone: %s" % (u, e))
            continue
        if [x.pack() for x in new] != [x.pack() for x in old] \
                or o.flags & F_RELINK:
            o.set_table(OB_LINK, OB_NLINK, new)
            o.flags = o.flags & ~F_RELINK
            objs.dirty.add(u)
            tables += 1
        if objs.orphans.pop(u, None) is not None:
            objs.or_changed = True
        loaded.append(u)
    # objects deleted while open keep their tables; they still count
    for u in sorted(checker.orphan_objs, key=lambda x: x.bytes):
        try:
            objs.load(u).links()
        except TsfsError as e:
            done.append("object %s left alone: %s" % (u, e))
            continue
        loaded.append(u)
    if tables:
        done.append("%d link table(s) were made again" % tables)

    refs = sb_refs(vol.sb)
    counts = {}
    pinned = 0
    for u in loaded:
        o = objs.cache[u]
        if o.pins < refs.get(u, 0):
            o.pins = refs[u]
            objs.dirty.add(u)
            pinned += 1
        counts[u] = o.pins
    for u in loaded:
        for e in objs.cache[u].links():
            if e.counted and e.target in counts:
                counts[e.target] += 1
    if pinned:
        done.append("%d object(s) the superblock names were given the "
                    "hold it takes" % pinned)

    fixed = 0
    for u in loaded:
        o = objs.cache[u]
        cur = counts[u]
        flags = o.flags
        if o.refcnt_signed != cur:
            if cur == 0 and o.refcnt_signed > 0 and not flags & F_ORPHAN:
                flags |= F_GARBAGE
            o.refcnt = cur
            objs.dirty.add(u)
            fixed += 1
        if flags & F_GARBAGE and (cur > 0 or flags & F_ORPHAN):
            flags &= ~F_GARBAGE
        if flags != o.flags:
            o.flags = flags
            objs.dirty.add(u)
        on = u in objs.gc
        if bool(flags & F_GARBAGE) != on:
            if on:
                objs.gc.pop(u)
            else:
                objs.gc[u] = o.blk
            objs.gc_changed = True
    if fixed:
        done.append("%d reference count(s) were set" % fixed)
    for u in list(objs.gc):
        if u not in here:
            objs.gc.pop(u)              # only objects of the index belong there
            objs.gc_changed = True
    objs.finish(done)
    return done
