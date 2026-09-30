"""Objects between a native volume and TADjs files (design 11.3).

The files of one object:

    {uuid}.json             the metadata
    {uuid}_N.xtad           the record at position N, xmlTAD
    {uuid}_N.bin            the record at position N, bytes
    {uuid}.ico              the icon
    {uuid}_N_M.{ext}        resource M of the record at position N

Reading them in: the records are made in position order with rids 1, 2,
3 ...; the metadata is kept as the kernel keeps a text it is given and
then records added to: without its structural keys, and with "records"
first; a text that is JSON but not an object is kept as it is. Each
xmlTAD record's links make the link table as the kernel makes it; a
link to an object that is not among those put on is external. Each root
the superblock names is a hold on its object, and the reference count
of an object is its holds and the links of the volume that point at it.
An object nothing refers to is not put on the garbage list.

Writing them out: the metadata as ts_obj_get_meta gives it (the
structural keys put in first), each record to the file its position
names, each resource to its own.
"tsfs-volume.json" beside them keeps the label, the volume UUID and the
roots, so that a volume can be made again from the directory.
"""

import json
import os
import uuid

from .error import TsfsError
from .layout import NIL_UUID, now_ns
from . import meta as metamod
from .obj import ICON_MAX, OB_ICON, is_icon
from .obj import (F_DELETABLE, F_EDITABLE, F_READABLE, ICON_REC, LK_EXTERNAL,
                  MAX_LINK, MAX_REC, MAX_RES, META_MAX, OB_META, OB_NLINK,
                  OB_NPLACE, OB_NRES, OB_LINK, OB_PLACE, OB_RES, RID_ICON,
                  RID_PLACE, LinkEntry, ObjectBlock, PlaceEntry, ResEntry,
                  sort_links)

VOLUME_FILE = "tsfs-volume.json"
VOLUME_FORMAT = "tsfs-volume-1"
ROOT_FIELDS = ("root", "sysbox", "domain")


class ObjectSet(object):
    """The files of one object, found in one or more directories."""

    def __init__(self, obj_uuid):
        self.uuid = obj_uuid
        self.json = None
        self.recs = {}                  # position -> (kind, path)
        self.res = {}                   # (position, number) -> (ext, path)
        self.problem = None
        # filled in by prepare()
        self.stored = None
        self.kinds = []
        self.records = []
        self.links = []                 # (vobjid, target, rid) as found
        self.flags = 0
        self.dates = (0, 0, 0)
        self.refcnt = 0
        self.pins = 0


def scan(dirs, problems):
    """Every object set in the directories, {UUID: ObjectSet}. A file
    that two directories both have is taken from the first."""
    sets = {}
    for d in dirs:
        if not os.path.isdir(d):
            raise TsfsError("%s is not a directory" % d)
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if not os.path.isfile(path):
                continue
            c = metamod.classify(name)
            if c is None:
                continue
            kind, u, det = c
            s = sets.get(u)
            if s is None:
                s = sets[u] = ObjectSet(u)
            if kind == "bad":
                problems.append("%s: %s" % (path, det))
                continue
            if kind == "json":
                if s.json is None:
                    s.json = path
                else:
                    problems.append("%s: %s.json is taken from %s"
                                    % (path, u, s.json))
            elif kind == "rec":
                pos, rk = det
                if pos in s.recs:
                    if s.recs[pos][0] != rk:
                        s.problem = "record %d is both .xtad and .bin" % pos
                    else:
                        problems.append("%s: record %d is taken from %s"
                                        % (path, pos, s.recs[pos][1]))
                    continue
                s.recs[pos] = (rk, path)
            else:
                pos, resno, ext = det
                if (pos, resno) in s.res:
                    problems.append("%s: resource (%d, %d) is taken from %s"
                                    % (path, pos, resno, s.res[(pos, resno)][1]))
                    continue
                s.res[(pos, resno)] = (ext, path)
    return sets


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def prepare(s):
    """Check one set and work out its metadata, records and links; sets
    s.problem when it cannot be taken in."""
    if s.problem:
        return
    if s.json is None:
        s.problem = "there is no %s.json" % s.uuid
        return
    raw = _read(s.json)
    try:
        d = metamod.loads(raw)
    except UnicodeDecodeError as e:
        s.problem = "the metadata is not UTF-8: %s" % e
        return
    except ValueError as e:
        if not metamod.is_json(raw):
            s.problem = "the metadata is not JSON: %s" % e
            return
        d = None                        # JSON, but not an object: kept as it is
    n = len(s.recs)
    if sorted(s.recs) != list(range(n)):
        s.problem = "the records are at positions %s, not 0..%d" % (
            sorted(s.recs), n - 1)
        return
    if n > MAX_REC:
        s.problem = "%d records, more than %d" % (n, MAX_REC)
        return
    # the icon is a part of its own, beside the metadata, not a resource
    s.icon = None
    for key in [k for k in s.res if k[0] == ICON_REC]:
        ext, path = s.res.pop(key)
        if key[1] != 0 or ext != "ico":
            s.problem = "%s: only {uuid}.ico is the icon" % path
            return
        data = _read(path)
        if len(data) > ICON_MAX:
            s.problem = "the icon is %d bytes, more than %d" % (len(data), ICON_MAX)
            return
        if not is_icon(data):
            s.problem = "%s is neither an ICO nor a PNG" % path
            return
        s.icon = data
    if len(s.res) > MAX_RES:
        s.problem = "%d resources, more than %d" % (len(s.res), MAX_RES)
        return
    s.kinds = [s.recs[p][0] for p in range(n)]
    s.records = metamod.records_for(d, s.kinds)
    s.stored = metamod.stored_text(raw, s.records)
    if len(s.stored) > META_MAX:
        s.problem = "the metadata is %d bytes, more than %d" % (len(s.stored),
                                                                META_MAX)
        return
    now = now_ns()
    dates = []
    for (k, _f) in metamod.DATE_KEYS:
        t = metamod.parse_date(d.get(k)) if d is not None else None
        dates.append(now if t is None else t)
    s.dates = tuple(dates)
    flags = 0
    for key, bit in (("editable", F_EDITABLE), ("deletable", F_DELETABLE),
                     ("readable", F_READABLE)):
        if d is None or d.get(key, True) is not False:
            flags |= bit
    s.flags = flags
    # as the kernel makes the table: the records of type 1 in their order,
    # a vobjid met before in the object left out
    links = []
    for pos, r in enumerate(s.records):
        if r["rt"] != metamod.RT_XTAD:
            continue
        if not metamod.scan_links(_read(s.recs[pos][1]), r["rid"], links,
                                  MAX_LINK):
            s.problem = "more than %d links" % MAX_LINK
            return
    s.links = links


def build(vol, dirs, roots=None, problems=None):
    """Put every object set of the directories on a volume that has just
    been made. `roots` maps "root", "sysbox", "domain" to UUIDs; each
    one the superblock names is a hold on its object. Answers (objects
    written, [(UUID, why) skipped])."""
    if problems is None:
        problems = []
    sets = scan(dirs, problems)
    skipped = []
    for u in sorted(sets, key=lambda x: x.bytes):
        prepare(sets[u])
        if sets[u].problem:
            skipped.append((u, sets[u].problem))
    good = dict((u, s) for u, s in sets.items() if not s.problem)

    roots = roots or {}
    for name, u in roots.items():
        if u is not None and u != NIL_UUID and u not in good:
            raise TsfsError("the %s object %s is not among the objects" % (name, u))

    pins = dict((u, 0) for u in good)
    for name in ROOT_FIELDS:
        u = roots.get(name)
        if u is not None and u != NIL_UUID:
            pins[u] += 1
    counts = dict(pins)
    for s in good.values():
        for _vobjid, target, _rid in s.links:
            if target in counts:
                counts[target] += 1

    index = []
    for u in sorted(good, key=lambda x: x.bytes):
        s = good[u]
        s.pins = pins[u]
        s.refcnt = counts[u]
        index.append((u, write_object(vol, s, good)))

    vol.btree.build(index)
    sb = vol.sb
    sb.root_uuid = roots.get("root") or NIL_UUID
    sb.sysbox_uuid = roots.get("sysbox") or NIL_UUID
    sb.domain_uuid = roots.get("domain") or NIL_UUID
    vol.sb_dirty = True
    return len(index), skipped


def write_object(vol, s, here):
    """One object on the volume. Answers its object block."""
    blk = vol.alloc_blk()
    o = ObjectBlock.new(vol, blk, s.uuid, flags=s.flags)
    o.made, o.updated, o.read = s.dates
    o.refcnt = s.refcnt
    o.pins = s.pins
    o.nextrid = len(s.records) + 1

    links = sort_links([LinkEntry(v, t, rid, 0 if t in here else LK_EXTERNAL)
                        for (v, t, rid) in s.links])

    # the management parts first, so that they are the ones kept inline;
    # the tables are laid out at their final size and filled in below
    o.mseg_write(OB_META, s.stored)
    if getattr(s, "icon", None):
        o.mseg_write(OB_ICON, s.icon)
    o.set_table(OB_PLACE, OB_NPLACE, [PlaceEntry(0, 0)] * len(s.records))
    o.set_table(OB_RES, OB_NRES, [ResEntry(0, 0, "")] * len(s.res))
    o.set_table(OB_LINK, OB_NLINK, links)

    places = []
    for pos, r in enumerate(s.records):
        kind, path = s.recs[pos]
        pl = o.dseg_new(_read(path))
        places.append(PlaceEntry(r["rid"], kind, pl.nbytes, pl))
    ress = []
    for (pos, resno) in sorted(s.res, key=lambda k: (k[0] != ICON_REC, k)):
        ext, path = s.res[(pos, resno)]
        if pos == ICON_REC:
            owner = RID_ICON
        elif pos < len(s.records):
            owner = s.records[pos]["rid"]
        else:
            owner = RID_PLACE | pos
        pl = o.dseg_new(_read(path))
        ress.append(ResEntry(owner, resno, ext, pl.nbytes, pl))
    o.set_table(OB_PLACE, OB_NPLACE, places)
    o.set_table(OB_RES, OB_NRES, ress)
    o.store()
    return blk


# ---------------------------------------------------------------- out

def export_object(vol, obj_uuid, blk, outdir, problems):
    """Write one object's files. Answers (records, resources) written."""
    o = ObjectBlock.load(vol, blk, obj_uuid)
    raw = o.meta()
    if not metamod.is_object(raw):
        problems.append("%s: the metadata is not a JSON object; written as it "
                        "is" % obj_uuid)
    order, _named = metamod.order_of(raw, o.placements())
    pos_of = {}
    for pos, (e, _rt, _sub) in enumerate(order):
        pos_of.setdefault(e.rid, pos)
        _write(outdir, metamod.rec_name(obj_uuid, pos, e.kind),
               o.read_data(e.place))
    nres = 0
    icon = o.icon()
    if icon is not None:
        _write(outdir, "%s.ico" % obj_uuid, icon)
        nres += 1
    for e in o.resources():
        if e.owner == RID_ICON:
            continue                    # written above, wherever it was kept
        elif e.owner & RID_PLACE:
            pos = e.owner & ~RID_PLACE
        elif e.owner in pos_of:
            pos = pos_of[e.owner]
        else:
            problems.append("%s: resource %d belongs to rid %d, which is no "
                            "record; left out" % (obj_uuid, e.resno, e.owner))
            continue
        _write(outdir, metamod.res_name(obj_uuid, pos, e.resno, e.ext),
               o.read_data(e.place))
        nres += 1
    _write(outdir, "%s.json" % obj_uuid,
           metamod.synth(raw, o.refcnt_signed, o.nplace, o.made, o.updated,
                         o.read))
    return len(order), nres


def _write(outdir, name, data):
    with open(os.path.join(outdir, name), "wb") as f:
        f.write(data)


def export(vol, outdir, problems=None):
    """Every object of a volume into outdir. Answers (objects, records,
    resources, [(UUID, why) not written])."""
    if problems is None:
        problems = []
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    nobj = nrec = nres = 0
    failed = []
    for u, blk in vol.objects():
        try:
            r, s = export_object(vol, u, blk, outdir, problems)
        except TsfsError as e:
            failed.append((u, str(e)))
            continue
        nobj += 1
        nrec += r
        nres += s
    sb = vol.sb
    info = {
        "format": VOLUME_FORMAT,
        "label": sb.label,
        "uuid": str(sb.vol_uuid),
        "root": str(sb.root_uuid) if sb.root_uuid != NIL_UUID else None,
        "sysbox": str(sb.sysbox_uuid) if sb.sysbox_uuid != NIL_UUID else None,
        "domain": str(sb.domain_uuid) if sb.domain_uuid != NIL_UUID else None,
    }
    _write(outdir, VOLUME_FILE,
           (json.dumps(info, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return nobj, nrec, nres, failed


def read_volume_file(d):
    """What tsfs-volume.json in a directory says, or {}."""
    path = os.path.join(d, VOLUME_FILE)
    if not os.path.exists(path):
        return {}
    with open(path, "rb") as f:
        info = json.loads(f.read().decode("utf-8"))
    if not isinstance(info, dict):
        raise TsfsError("%s is not a JSON object" % path)
    out = {}
    for k in ("label", "uuid", "root", "sysbox", "domain"):
        v = info.get(k)
        if v is None:
            continue
        if k == "label":
            out[k] = v
        else:
            try:
                out[k] = uuid.UUID(v)
            except ValueError:
                raise TsfsError("%s: %s is not a UUID" % (path, k))
    return out
