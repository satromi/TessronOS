#!/usr/bin/env python3
"""Tests of the host side TSFS tools on volumes of version 5 (design 7.3),
and of the conversion from version 4.

    python3 tools/tests/test_tsfs_v5.py [-v] [TestCase ...]

Everything runs on image files in a temporary directory (TMPDIR, best on
a disk of the host's own: the images are up to 256 MB), so nothing here
touches a device. With R1410=1 in the environment the tools of R1.410
(git f2adede) are written out to the temporary directory and used to
make a volume of version 4 and to look at one of version 5; that needs
git and the repository.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.environ.get("TSFS_REPO") or os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

from tsfs_py import BLOCK_SIZE, BlockImage, TsfsError, Volume   # noqa: E402
from tsfs_py import obj as objmod                                # noqa: E402
from tsfs_py import repair, transfer                             # noqa: E402
from tsfs_py.check import Checker                                # noqa: E402
from tsfs_py.journal import Journal                              # noqa: E402
from tsfs_py.layout import (COMPAT_CONVREQ, INCOMPAT_FMT5, VERSION5,  # noqa: E402
                            V4_META_MAX)
from tsfs_py.obj import (F_ORPHAN, F_SPARE, FORM_RUN, FORM_TREE,  # noqa: E402
                         FORM_TREEN, FORM_TRIM, OB_META, OB_NPLACE, OB_NRES,
                         OB_PLACE, OB_RES, ObjectBlock, Place)

PY = sys.executable


def make_format():
    """The version mktsfs makes under make (MAKE_FORMAT in tools/mktsfs)."""
    with open(os.path.join(TOOLS, "mktsfs"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("MAKE_FORMAT = "):
                return int(line.split("=", 1)[1])
    raise AssertionError("tools/mktsfs has no MAKE_FORMAT")


MAKE_FORMAT = make_format()


def run(tool, *args, env=None):
    e = dict(os.environ)
    e.pop("MAKELEVEL", None)
    e.pop("TSFS_FORMAT", None)
    if env:
        e.update(env)
    r = subprocess.run([PY, os.path.join(TOOLS, tool)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, env=e)
    return r.returncode, r.stdout


def meta(name, nrec, **more):
    d = {"name": name, "relationship": [], "linktype": False,
         "makeDate": "2026-11-21T00:00:00Z", "updateDate": "2026-11-21T00:00:00Z",
         "accessDate": "2026-11-21T00:00:00Z", "refCount": 0,
         "recordCount": nrec, "editable": True, "deletable": True,
         "readable": True, "maker": "TessronOS", "applist": {}}
    d.update(more)
    return d


def link_text(targets):
    out = ["<document><p>"]
    for t in targets:
        out.append('<link id="%s_0.xtad" vobjid="%s" vobjleft="8" vobjtop="8" '
                   'vobjright="248" vobjbottom="33"/>' % (t, uuid.uuid4()))
    out.append("</p></document>")
    return "".join(out).encode("utf-8")


def put(d, name, data):
    with open(os.path.join(d, name), "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def make_set(d, big=False):
    """A directory of TADjs objects; with big, one past every limit of
    version 4 as well. Answers {what: UUID}."""
    os.makedirs(d, exist_ok=True)
    u = {k: uuid.uuid4() for k in ("box", "doc", "pic", "rec", "res", "lnk",
                                   "ext", "meta")}
    put(d, "%s.json" % u["box"], json.dumps(meta("箱", 1)))
    put(d, "%s_0.xtad" % u["box"], link_text([u["doc"], u["pic"]]))
    put(d, "%s.json" % u["doc"], json.dumps(meta("文章", 2)))
    put(d, "%s_0.xtad" % u["doc"], "<document><p>本文</p></document>" * 300)
    put(d, "%s_1.bin" % u["doc"], os.urandom(50000))
    put(d, "%s.json" % u["pic"], json.dumps(meta("図", 1)))
    put(d, "%s_0.xtad" % u["pic"], "<figure/>")
    put(d, "%s_0_1.png" % u["pic"], os.urandom(9000))
    put(d, "%s_0_2.longextension16b" % u["pic"], os.urandom(100))
    if not big:
        for k in ("rec", "res", "lnk", "ext", "meta"):
            del u[k]
        return u
    # 300 records
    put(d, "%s.json" % u["rec"], json.dumps(meta("レコード300", 300)))
    for i in range(300):
        put(d, "%s_%d.%s" % (u["rec"], i, "xtad" if i % 2 == 0 else "bin"),
            ("<document><p>%d</p></document>" % i) if i % 2 == 0
            else os.urandom(100 + i * 37))
    # 100 resources
    put(d, "%s.json" % u["res"], json.dumps(meta("資源100", 1)))
    put(d, "%s_0.xtad" % u["res"], "<document/>")
    for i in range(100):
        put(d, "%s_0_%d.dat" % (u["res"], i + 1), os.urandom(10 + i * 50))
    # 2000 links, to objects not on the volume (external)
    put(d, "%s.json" % u["lnk"], json.dumps(meta("リンク2000", 1)))
    put(d, "%s_0.xtad" % u["lnk"], link_text([uuid.uuid4() for _ in range(2000)]))
    # an extension of 200 bytes
    put(d, "%s.json" % u["ext"], json.dumps(meta("長い拡張子", 1)))
    put(d, "%s_0.xtad" % u["ext"], "<document/>")
    put(d, "%s_0_1.%s" % (u["ext"], "e" * 200), b"ext200")
    put(d, "%s_0_2.short" % u["ext"], b"short")
    # metadata of 1 MB
    put(d, "%s.json" % u["meta"], json.dumps(meta("大きなメタデータ", 1,
                                                   blob="m" * (1 << 20))))
    put(d, "%s_0.xtad" % u["meta"], "<document/>")
    return u


def export_files(img, out):
    code, text = run("tsfs-export", "-o", out, img)
    if code != 0:
        raise AssertionError(text)
    return out


def same_dirs(a, b, skip=("tsfs-volume.json",)):
    na = sorted(n for n in os.listdir(a) if n not in skip)
    nb = sorted(n for n in os.listdir(b) if n not in skip)
    if na != nb:
        return "names differ: %s" % sorted(set(na) ^ set(nb))[:6]
    for n in na:
        with open(os.path.join(a, n), "rb") as f1, open(os.path.join(b, n), "rb") as f2:
            if f1.read() != f2.read():
                return "%s differs" % n
    return None


def fsck(img, *opts):
    code, text = run("fsck.tsfs", "--json", *(list(opts) + [img]))
    try:
        rep = json.loads(text[text.index("{"):])
    except ValueError:
        rep = {"errors": [{"what": text}], "warnings": []}
    return code, rep


class Base(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="tsfd-v5-")
        self.addCleanup(shutil.rmtree, self.dir, True)

    def p(self, *names):
        return os.path.join(self.dir, *names)

    def assertClean(self, img, *opts):
        code, rep = fsck(img, *opts)
        self.assertEqual(code, 0, rep.get("errors"))
        return rep

    def mk(self, img, dirs, size="64M", fmt=None, env=None):
        args = ["-q", "--size", size]
        if fmt:
            args += ["--format", str(fmt)]
        for d in dirs:
            args += ["--from", d]
        return run("mktsfs", *(args + [img]), env=env)


class TestMake(Base):

    def test_default_versions(self):
        src = self.p("src")
        make_set(src)
        other = 9 - MAKE_FORMAT
        for fmt, env, want in ((None, None, 5), (4, None, 4), (5, None, 5),
                               (None, {"MAKELEVEL": "1"}, MAKE_FORMAT),
                               (None, {"MAKELEVEL": "1", "TSFS_FORMAT": str(other)}, other),
                               (None, {"TSFS_FORMAT": "4"}, 4)):
            img = self.p("v.img")
            code, text = self.mk(img, [src], fmt=fmt, env=env)
            self.assertEqual(code, 0, text)
            with Volume.open(BlockImage(img)) as vol:
                self.assertEqual(vol.sb.version, want, (fmt, env))
                self.assertEqual(bool(vol.sb.incompat & INCOMPAT_FMT5), want == 5)
            self.assertClean(img)
            os.remove(img)

    def test_round_trip(self):
        src = self.p("src")
        make_set(src)
        outs = {}
        for fmt in (4, 5):
            img = self.p("v%d.img" % fmt)
            code, text = self.mk(img, [src], fmt=fmt)
            self.assertEqual(code, 0, text)
            self.assertClean(img)
            e1 = export_files(img, self.p("e%d" % fmt))
            img2 = self.p("w%d.img" % fmt)
            code, text = run("tsfs-import", "--format", fmt, "--size", "64M", e1, img2)
            self.assertEqual(code, 0, text)
            self.assertClean(img2)
            e2 = export_files(img2, self.p("f%d" % fmt))
            self.assertIsNone(same_dirs(e1, e2))
            outs[fmt] = e1
        # a long extension does not fit version 4: that resource is left out
        self.assertTrue(any(n.endswith(".longextension16b") for n in os.listdir(outs[5])))
        self.assertFalse(any(n.endswith(".longextension16b") for n in os.listdir(outs[4])))
        for n in os.listdir(src):
            if n.endswith(".longextension16b") or n.endswith(".json"):
                continue
            with open(os.path.join(src, n), "rb") as f:
                want = f.read()
            with open(os.path.join(outs[5], n), "rb") as f:
                self.assertEqual(f.read(), want, n)

    def test_past_version4(self):
        src = self.p("src")
        u = make_set(src, big=True)
        img4 = self.p("v4.img")
        code, text = self.mk(img4, [src], fmt=4, size="128M")
        self.assertEqual(code, 1, text)
        for k in ("rec", "res", "lnk", "meta"):
            self.assertIn("%s left out" % u[k], text)
        img = self.p("v5.img")
        code, text = self.mk(img, [src], fmt=5, size="128M")
        self.assertEqual(code, 0, text)
        rep = self.assertClean(img)
        self.assertGreaterEqual(rep["stats"].get("long_extensions", 0), 1)
        out = export_files(img, self.p("out"))
        for n in os.listdir(src):
            if n.endswith(".json"):
                continue
            with open(os.path.join(src, n), "rb") as f1, \
                    open(os.path.join(out, n), "rb") as f2:
                self.assertEqual(f1.read(), f2.read(), n)
        with Volume.open(BlockImage(img)) as vol:
            o = vol.object(u["rec"])
            self.assertEqual(o.nplace, 300)
            self.assertEqual(vol.object(u["res"]).nres, 100)
            self.assertEqual(vol.object(u["lnk"]).nlink, 2000)
            self.assertGreater(vol.object(u["meta"]).place(OB_META).nbytes, V4_META_MAX)
            exts = [e.ext for e in vol.object(u["ext"]).resources()]
            self.assertIn("e" * 200, exts)
            self.assertIn("short", exts)

    def test_ext767_and_big_meta(self):
        """An extension of 767 bytes (no file name holds it) and 8 MB of
        metadata, put on with the library."""
        src = self.p("src")
        u = make_set(src)
        img = self.p("v5.img")
        code, text = self.mk(img, [src], fmt=5, size="128M")
        self.assertEqual(code, 0, text)
        with Volume.open(BlockImage(img, writable=True)) as vol:
            o = vol.object(u["pic"])
            ents = o.resources()
            ents[0].ext = "x" * 767
            ents[0].ext_bytes = None
            o.set_table(OB_RES, OB_NRES, ents)
            raw = o.meta()
            o.mseg_write(OB_META, raw[:-1] + b', "blob": "' + b"z" * (8 << 20) + b'"}')
            o.store()
        self.assertClean(img)
        with Volume.open(BlockImage(img)) as vol:
            o = vol.object(u["pic"])
            self.assertEqual(o.resources()[0].ext, "x" * 767)
            self.assertEqual(json.loads(o.meta().decode())["blob"], "z" * (8 << 20))


def fragment(vol, keep_every=2):
    """Every free block taken, then every other one given back, so that
    no free run is longer than one block. Answers the blocks still held,
    to give back afterwards."""
    held = []
    maps = vol.load_maps()
    while True:
        try:
            s, n = maps.alloc_ext(1 << 16)
        except TsfsError:
            break
        held.append((s, n))
    keep = []
    for s, n in held:
        for b in range(s, s + n):
            if (b % keep_every) == 0:
                maps.free_ext(b, 1)
            else:
                keep.append(b)
    return keep


class TestTrees(Base):

    def build_frag(self, fan):
        src = self.p("src")
        u = make_set(src)
        big = os.urandom(3 * 1024 * 1024)
        put(src, "%s_1.bin" % u["doc"], big)
        img = self.p("t.img")
        dev = BlockImage(self.tmp_image(img, 64), writable=True)
        old = os.environ.get("TSFS_PY_XN_FANOUT")
        if fan:
            os.environ["TSFS_PY_XN_FANOUT"] = str(fan)
        try:
            with Volume.format(dev, version=5) as vol:
                held = fragment(vol)
                transfer.build(vol, [src])
                for b in held:
                    vol.free_ext(b, 1)
                vol.flush(both=True, spares=True)
        finally:
            if old is None:
                os.environ.pop("TSFS_PY_XN_FANOUT", None)
            else:
                os.environ["TSFS_PY_XN_FANOUT"] = old
        return img, u, big

    def tmp_image(self, path, mb):
        with open(path, "wb") as f:
            f.truncate(mb << 20)
        return path

    def test_run_tree_levels(self):
        for fan, depth in ((4, 3), (0, 1)):
            img, u, big = self.build_frag(fan)
            rep = self.assertClean(img)
            self.assertGreaterEqual(rep["stats"].get("run_tree_depth", 0), depth)
            with Volume.open(BlockImage(img)) as vol:
                o = vol.object(u["doc"])
                e = [x for x in o.placements() if x.rid == 2][0]
                self.assertEqual(e.place.form, FORM_TREEN)
                self.assertEqual(e.place.b, len(o.extents(e.place)))
                self.assertGreater(e.place.b, 165)
                self.assertEqual(o.read_data(e.place), big)
            os.remove(img)

    def test_mgmt_tree(self):
        """Metadata that finds no free run long enough lies in a tree."""
        src = self.p("src")
        u = make_set(src)
        img = self.tmp_image(self.p("m.img"), 64)
        text = (json.dumps(meta("m", 1, blob="q" * 400000))).encode()
        with Volume.format(BlockImage(img, writable=True), version=5) as vol:
            transfer.build(vol, [src])
            held = fragment(vol)
            o = vol.object(u["box"])
            o.mseg_write(OB_META, text)
            o.store()
            self.assertIn(o.place(OB_META).form, (FORM_TREE, FORM_TREEN))
            for b in held:
                vol.free_ext(b, 1)
            vol.flush(both=True, spares=True)
        self.assertClean(img)
        with Volume.open(BlockImage(img)) as vol:
            self.assertEqual(vol.object(u["box"]).meta(), text)

    def test_v4_refuses_trees(self):
        src = self.p("src")
        make_set(src)
        img = self.tmp_image(self.p("m.img"), 64)
        with Volume.format(BlockImage(img, writable=True), version=4) as vol:
            vol.btree.build([])
            fragment(vol)
            o = ObjectBlock.new(vol, vol.alloc_blk(), uuid.uuid4())
            self.assertRaises(TsfsError, o.mseg_write, OB_META, b"x" * 100000)
            self.assertRaises(TsfsError, o.dseg_new, b"y" * (200 * 4096))


class TestJournal(Base):

    def vol(self, version=5, mb=16):
        img = self.p("j.img")
        with open(img, "wb") as f:
            f.truncate(mb << 20)
        dev = BlockImage(img, writable=True)
        vol = Volume.format(dev, version=version)
        self.addCleanup(dev.close)
        return vol

    def test_chain(self):
        vol = self.vol(mb=64)
        # a ring of 2800 blocks over the groups: only the journal is looked at
        vol.sb.journal_blocks = 2801
        j = Journal(vol)
        self.assertEqual(j.budget, 700)
        vol.journal = j
        base = vol.sb.data_end - 700
        blocks = [(base + i, bytes([i & 0xFF]) * BLOCK_SIZE) for i in range(600)]
        revokes = list(range(1000, 2500))
        foot = j.commit(blocks, revokes)
        self.assertEqual(foot, 3 + 600 + 3 + 1)
        j2 = Journal(vol)
        txns = j2.scan()
        self.assertEqual(len(txns), 1)
        self.assertEqual((txns[0].ndesc, txns[0].nrevoke), (3, 3))
        self.assertEqual(len(txns[0].homes), 600)
        self.assertEqual(len(txns[0].revokes), 1500)
        over = j2.overlay()
        self.assertEqual(over[base + 300], bytes([300 & 0xFF]) * BLOCK_SIZE)
        # without its commit it never happened
        j.commit(blocks[:10], write_commit=False)
        self.assertEqual(len(Journal(vol).scan()), 1)
        # a descriptor number skipped stops the scan at that transaction
        j3 = Journal(vol)
        j3.scan()
        foot = j3.commit(blocks[:300])
        pos = j3.txns[-1].pos + 1 + 254
        buf = bytearray(vol.raw_read(j3.ring_blk(pos)))
        import struct
        struct.pack_into("<I", buf, 28, 7)
        struct.pack_into("<I", buf, 24, j3.crc_at(buf, 24))
        vol.raw_write(j3.ring_blk(pos), bytes(buf))
        self.assertEqual(len(Journal(vol).scan()), 1)
        self.assertEqual(Journal(vol).replay(), 600)

    def test_v4_one_descriptor(self):
        vol = self.vol(version=4)
        j = vol.journal
        self.assertEqual(j.budget, 254)
        self.assertRaises(TsfsError, j.commit,
                          [(vol.sb.data_end - 1 - i, bytes(BLOCK_SIZE)) for i in range(255)])
        j.commit([(vol.sb.data_end - 1, b"\1" * BLOCK_SIZE)], [5])
        t = Journal(vol).scan()
        self.assertEqual((t[0].ndesc, t[0].nrevoke), (1, 1))


class TestSpare(Base):

    def setup_vol(self):
        src = self.p("src")
        u = make_set(src)
        img = self.p("s.img")
        code, text = self.mk(img, [src], fmt=5)
        self.assertEqual(code, 0, text)
        return img, u

    def orphan_add(self, vol, u, blk):
        items = list(vol.ortree.items()) + [(u, blk)]
        vol.ortree.rebuild(sorted(items, key=lambda kv: kv[0].bytes))

    def test_runs(self):
        img, u = self.setup_vol()
        with Volume.open(BlockImage(img)) as vol:
            free0 = vol.load_maps().total_free()
        with Volume.open(BlockImage(img, writable=True)) as vol:
            o = vol.object(u["pic"])
            runs = o._alloc_runs(40)
            o.set_place(objmod.OB_SPARE, o.put_runs(runs, 40 * BLOCK_SIZE))
            o.flags = o.flags | F_SPARE
            o.store()
            self.orphan_add(vol, u["pic"], o.blk)
        code, rep = fsck(img)
        self.assertEqual(code, 0, rep["errors"])
        self.assertTrue(any("spare place" in w["what"] for w in rep["warnings"]))
        code, rep = fsck(img, "--fix")
        self.assertEqual(code, 0, rep["errors"])
        rep = self.assertClean(img)
        self.assertEqual(rep["warnings"], [])
        with Volume.open(BlockImage(img)) as vol:
            o = vol.object(u["pic"])
            self.assertTrue(o.spare_raw_empty())
            self.assertFalse(o.flags & F_SPARE)
            self.assertEqual(vol.load_maps().total_free(), free0)
            self.assertEqual(vol.orphans(), [])

    def test_trim(self):
        img, u = self.setup_vol()
        with Volume.open(BlockImage(img)) as vol:
            o = vol.object(u["doc"])
            whole = o.read_data([e for e in o.placements() if e.rid == 2][0].place)
            free0 = vol.load_maps().total_free()
        keep = 5000
        with Volume.open(BlockImage(img, writable=True)) as vol:
            o = vol.object(u["doc"])
            ents = o.placements()
            for e in ents:
                if e.rid == 2:
                    e.nbytes = keep
                    e.place.nbytes = keep
            o.set_table(OB_PLACE, OB_NPLACE, ents)
            o.set_place(objmod.OB_SPARE, Place(FORM_TRIM, keep, 2, 0))
            o.flags = o.flags | F_SPARE
            o.store()
            self.orphan_add(vol, u["doc"], o.blk)
        code, rep = fsck(img)
        self.assertEqual(code, 0, rep["errors"])
        code, rep = fsck(img, "--fix")
        self.assertEqual(code, 0, rep["errors"])
        self.assertEqual(self.assertClean(img)["warnings"], [])
        with Volume.open(BlockImage(img)) as vol:
            o = vol.object(u["doc"])
            e = [x for x in o.placements() if x.rid == 2][0]
            self.assertEqual(o.read_data(e.place), whole[:keep])
            self.assertEqual(vol.load_maps().total_free(),
                             free0 + (len(whole) + 4095) // 4096 - 2)

    def test_unmarked_is_error(self):
        img, u = self.setup_vol()
        with Volume.open(BlockImage(img, writable=True)) as vol:
            o = vol.object(u["pic"])
            runs = o._alloc_runs(3)
            o.set_place(objmod.OB_SPARE, o.put_runs(runs, 0))
            o.store()
        code, rep = fsck(img)
        self.assertEqual(code, 1)


class TestConvert(Base):

    def v4(self, maker=None):
        src = self.p("src")
        make_set(src)
        img = self.p("c.img")
        if maker is None:
            code, text = self.mk(img, [src], fmt=4)
        else:
            code, text = maker(img, src)
        self.assertEqual(code, 0, text)
        before = export_files(img, self.p("before"))
        return img, before

    def converted(self, img, before):
        rep = self.assertClean(img)
        self.assertEqual(rep["warnings"], [])
        with Volume.open(BlockImage(img)) as vol:
            self.assertEqual(vol.sb.version, VERSION5)
            self.assertTrue(vol.sb.incompat & INCOMPAT_FMT5)
            self.assertFalse(vol.sb.compat & COMPAT_CONVREQ)
            self.assertEqual(vol.journal.head_version, 5)
        after = export_files(img, self.p("after"))
        self.assertIsNone(same_dirs(before, after))
        shutil.rmtree(after)

    def test_convert(self):
        img, before = self.v4()
        with Volume.open(BlockImage(img, writable=True)) as vol:
            vol.sb.compat |= COMPAT_CONVREQ
            vol.write_sb(both=True, spares=True)
        code, text = run("tsfs-convert", "-n", img)
        self.assertEqual(code, 0, text)
        with Volume.open(BlockImage(img)) as vol:
            self.assertEqual(vol.sb.version, 4)
        code, text = run("tsfs-convert", img)
        self.assertEqual(code, 0, text)
        self.converted(img, before)
        code, text = run("tsfs-convert", img)
        self.assertEqual(code, 0, text)
        self.assertIn("version 5 already", text)

    def test_steps(self):
        for step, v5 in (("2", False), ("3", False), ("4", False), ("5a", True),
                         ("5", True)):
            img, before = self.v4()
            code, text = run("tsfs-convert", "--stop-after", step, img)
            self.assertEqual(code, 1, text)
            with Volume.open(BlockImage(img)) as vol:
                self.assertEqual(vol.sb.v5, v5, step)
                if v5:
                    self.assertEqual(vol.journal.head_state, "old", step)
            self.assertClean(img)
            code, text = run("tsfs-convert", img)
            self.assertEqual(code, 0, text)
            self.converted(img, before)
            os.remove(img)
            shutil.rmtree(before)

    def test_fix_finishes_head(self):
        img, before = self.v4()
        code, text = run("tsfs-convert", "--stop-after", "5", img)
        code, rep = fsck(img, "--fix")
        self.assertEqual(code, 0, rep)
        self.converted(img, before)

    def test_damaged(self):
        img, _before = self.v4()
        with Volume.open(BlockImage(img, writable=True)) as vol:
            u, blk = vol.objects()[0]
            o = vol.object(u, blk)
            o.refcnt = o.refcnt + 3
            o.store()
        with open(img, "rb") as f:
            want = f.read()
        code, text = run("tsfs-convert", img)
        self.assertEqual(code, 1, text)
        with open(img, "rb") as f:
            self.assertEqual(f.read(), want)

    @unittest.skipUnless(os.environ.get("R1410"), "R1410=1 uses git f2adede")
    def test_r1410(self):
        old = self.p("r1410")
        os.makedirs(old)
        arch = subprocess.run(["git", "-C", ROOT, "archive", "f2adede", "tools/mktsfs",
                               "tools/fsck.tsfs", "tools/tsfs_py"],
                              stdout=subprocess.PIPE, check=True).stdout
        subprocess.run(["tar", "-x", "-C", old], input=arch, check=True)
        otools = os.path.join(old, "tools")

        def oldmk(img, src):
            r = subprocess.run([PY, os.path.join(otools, "mktsfs"), "-q", "--size", "64M",
                                "--from", src, img], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, universal_newlines=True)
            return r.returncode, r.stdout
        img, before = self.v4(oldmk)
        code, text = run("tsfs-convert", img)
        self.assertEqual(code, 0, text)
        self.converted(img, before)
        with open(img, "rb") as f:
            want = f.read()
        for opts in ([], ["--fix"], ["--relink"], ["--fix", "--relink"]):
            r = subprocess.run([PY, os.path.join(otools, "fsck.tsfs")] + opts + [img],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               universal_newlines=True)
            self.assertEqual(r.returncode, 2, (opts, r.stdout))
            with open(img, "rb") as f:
                self.assertEqual(f.read(), want, opts)


if __name__ == "__main__":
    unittest.main()
