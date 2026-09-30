#!/usr/bin/env python3
"""Tests of tools/btbackup.py, and of lib/libbtbk built for the host.

    python3 tools/tests/test_btbackup.py
    python3 -m pytest tools/tests/test_btbackup.py

The sample volume is tests/ktest/data/backup.TAD: four cabinets that
refer to one another. When a C compiler is there, the C library is
built too (tools/tests/btbk_host.c) and must agree with the Python one
byte for byte; without one those tests are skipped.

A large archive can be checked by hand, which is not done here for its
size:

    python3 tools/btbackup.py recompress --check --jobs 16 ARCHIVE
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import btbackup as bb                                   # noqa: E402

SAMPLE = os.path.join(TOP, "tests", "ktest", "data", "backup.TAD")
TFBK = os.path.join(TOP, "tests", "ktest", "data", "tfbk.TAD")     # written by TessronOS's バックアップ
XTAD = os.path.join(TOP, "etc", "xtad")


def sample():
    with open(SAMPLE, "rb") as f:
        return f.read()


def big_object(items):
    """A fifth object: a 実行機能付箋 and 3000 bytes that do not compress."""
    seed = 12345
    data = bytearray()
    for _ in range(3000):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        data.append((seed >> 16) & 0xFF)
    meta = items[3][1]
    st = bb.fstate_parse(meta[40:])
    st.update(f_nrec=2, f_nlink=0, f_size=128 + 3000)
    return (0x12340001, meta[:40] + bb.fstate_build(st),
            [(8, 0, items[3][2][0][2]), (1, 0, bytes(data))])


class TestLzss(unittest.TestCase):

    def test_tokens(self):
        cases = [
            (b"", ""),
            (b"A", "0041"),
            (bytes(range(32)), "1f" + bytes(range(32)).hex()),
            (bytes(range(33)), "1f" + bytes(range(32)).hex() + "0020"),
            (b"AAA", "02414141"),
            (b"A" * 16, "0041e001"),
            (b"A" * 17, "0041f0f001"),
            (b"A" * 257, "0041fff001"),
            (b"A" * 258, "0041fff0010041"),
            (b"A" * 300, "0041fff001f2a101"),
            (b"abcdXabcd", "0461626364583005"),
        ]
        for data, want in cases:
            c = bb.compress(data)
            self.assertEqual(c.hex(), want, data[:8])
            self.assertEqual(bb.decompress(c), data)
            self.assertEqual(bb.decompress(c, len(data)), data)

    def tokens_ok(self, s):
        i = 0
        while i < len(s):
            b0 = s[i]
            if b0 & 0xE0 == 0:
                i += b0 + 2
                continue
            if b0 <= 0xEF:
                w = (b0 << 8) | s[i + 1]
                ln = (w >> 12) & 0xF
                i += 2
            else:
                w = (s[i + 1] << 8) | s[i + 2]
                ln = ((b0 & 0xF) << 4) + ((w >> 12) & 0xF)
                self.assertGreaterEqual(ln, 15)
                i += 3
            self.assertTrue(1 <= (w & 0xFFF) <= 4094)
            self.assertGreaterEqual(ln, 2)
        self.assertEqual(i, len(s))

    def test_sizes(self):
        rnd = random.Random(1)
        kinds = [
            lambda n: bytes(n),
            lambda n: bytes(rnd.getrandbits(8) for _ in range(n)),
            lambda n: (b"abcab" * (n // 5 + 1))[:n],
            lambda n: bytes(rnd.choice(b"xy") for _ in range(n)),
        ]
        for n in (0, 1, 2, 3, 4, 5, 15, 31, 32, 33, 255, 256, 257, 258,
                  4093, 4094, 4095, 4096, 4097, 8191, 8192, 8193, 12000):
            for kind in kinds:
                data = kind(n)
                c = bb.compress(data)
                self.assertEqual(bb.decompress(c), data)
                self.tokens_ok(c)
                # the pieces the input comes in do not matter
                self.assertEqual(bb.compress(data, piece=7), c)

    def test_window(self):
        # a match 4094 bytes back is taken, one 4095 back is not
        rnd = random.Random(2)
        head = bytes(rnd.getrandbits(8) for _ in range(16))
        for gap, near in ((4094 - 16, True), (4095 - 16, False)):
            filler = bytes(rnd.getrandbits(8) for _ in range(gap))
            data = head + filler + head
            c = bb.compress(data)
            self.assertEqual(bb.decompress(c), data)
            found = False
            i = 0
            while i < len(c):
                b0 = c[i]
                if b0 & 0xE0 == 0:
                    i += b0 + 2
                    continue
                w = ((b0 << 8) | c[i + 1]) if b0 <= 0xEF else ((c[i + 1] << 8) | c[i + 2])
                found |= (w & 0xFFF) == 4094
                i += 2 if b0 <= 0xEF else 3
            self.assertEqual(found, near)

    def test_cut_input(self):
        c = bb.compress(b"hello hello hello hello")
        with self.assertRaises(bb.LzssError):
            bb.decompress(c[:-1], 23)


class TestArchive(unittest.TestCase):

    def test_parse(self):
        v = bb.Volume(sample())
        h = v.head
        self.assertEqual((h["kind"], h["vol"], h["more"]), (0xF0, 0, False))
        self.assertEqual((h["total"], h["nobj"], h["src"]), (3012, 4, 262144))
        self.assertEqual(bb.tc_to_str(v.memo), "1018キャビネットデモ")
        want = [(0x0CA, 412, 0x18B40001, "1018キャビネットデモ", 5, 3, 368, 268, 886),
                (0x26E, 363, 0x18C80001, "キャビネット3", 4, 2, 302, 219, 658),
                (0x3E1, 331, 0x18C70001, "キャビネット2", 3, 1, 236, 187, 430),
                (0x534, 331, 0x18C60001, "キャビネット", 3, 1, 236, 187, 430)]
        self.assertEqual(len(v.pieces), 4)
        for p, (pos, llen, objid, name, nrec, nlink, size, clen, plen) in zip(v.pieces, want):
            self.assertEqual((p.pos, p.llen, p.objid, p.name), (pos, llen, objid, name))
            self.assertEqual((p.cmp, p.kind, p.seg), (1, 0xF1, 0))
            self.assertEqual((p.st["f_nrec"], p.st["f_nlink"], p.st["f_size"]), (nrec, nlink, size))
            self.assertEqual((p.st["f_type"], p.st["f_atype"], p.st["f_pubacc"]), (0x1000, 6, 0x0FFF))
            self.assertEqual(p.st["f_ltime"], 0xFFFFFFFF)
            self.assertEqual(len(p.stream), clen)
            self.assertEqual(sum(16 + len(r.data) for r in p.records), plen)
            self.assertEqual(len(p.records), nrec)
            self.assertEqual(sum(1 for r in p.records if r.type == 0), nlink)
            self.assertEqual(sum(len(r.data) for r in p.records if r.type != 0), size)
            self.assertEqual(p.records[0].type, 8)
            self.assertEqual(p.records[-1].type, 1)
        lk = bb.link_parse(v.pieces[0].records[1].data)
        self.assertEqual(lk["atr"], [0x0008, 0, 0x8000, 0x0001, 0x8000])
        self.assertEqual((lk["objid"], lk["f_atype"], lk["f_name"]), (0x18C60001, 6, "キャビネット"))
        self.assertEqual(v.pieces[0].st["f_mtime"], 0x4CB01A1C)

    def test_recompress(self):
        for p in bb.Volume(sample()).pieces:
            self.assertEqual(bb.compress(bb.decompress(p.stream)), p.stream)

    def test_estimate(self):
        total = src = 0
        for p in bb.Volume(sample()).pieces:
            t, s = bb.estimate(p.st, 32768)
            total += t
            src += s
        self.assertEqual((total, src), (3012, 262144))

    def test_rewrite(self):
        data = sample()
        vols = [bb.Volume(data)]
        items, h = bb.rewrite(vols)
        out = bb.Writer(items, h["total"], h["nobj"], h["src"], vols[0].memo).volumes()
        self.assertEqual(out, [data])

    def test_volumes(self):
        vols = [bb.Volume(sample())]
        items, h = bb.rewrite(vols)
        items = items + [big_object(items)]
        out = bb.Writer(items, h["total"], h["nobj"] + 1, h["src"], vols[0].memo, 1600).volumes()
        self.assertEqual(len(out), 4)
        back = [bb.Volume(x) for x in out]
        for i, v in enumerate(back):
            self.assertEqual(v.head["vol"], i)
            self.assertEqual(v.head["more"], i + 1 < len(back))
        segs = [p.seg for v in back for p in v.pieces if p.objid == 0x12340001]
        self.assertEqual(segs, [0x8000, 1])
        objs = bb.join(back)
        self.assertEqual(len(objs), 5)
        for o, (objid, meta, recs) in zip(objs, items):
            self.assertTrue(o.complete)
            self.assertEqual(o.objid, objid)
            self.assertEqual([(t, s, bytes(d)) for t, s, d in o.records], list(recs))
        # the volumes out of order, or one missing, are not a set
        with self.assertRaises(bb.ArchiveError):
            bb.join(back[1:])
        with self.assertRaises(bb.ArchiveError):
            bb.join([back[0], back[2]])
        self.assertEqual(len(bb.join(back[1:], loose=True)), 4)

    def test_bad(self):
        data = bytearray(sample())
        data[0x0CA + 9] = 0xF0                          # an object of the wrong kind
        with self.assertRaises(bb.ArchiveError):
            bb.Volume(bytes(data))
        with self.assertRaises(bb.ArchiveError):
            bb.Volume(sample()[:-1]).pieces[-1].records
        data = bytearray(sample())
        data[0x0F] = 0xF3                               # a volume of the wrong kind
        with self.assertRaises(bb.ArchiveError):
            bb.Volume(bytes(data))

    def test_tessronos(self):
        # a volume TessronOS wrote: its own record first, then what BTRON reads
        with open(TFBK, "rb") as f:
            data = f.read()
        v = bb.Volume(data)
        self.assertEqual((v.head["kind"], v.head["nobj"], v.head["more"]), (0xF0, 2, False))
        self.assertEqual(bb.tc_to_str(v.memo), "TFBK")
        self.assertEqual([p.name for p in v.pieces], ["TFBK見本", "KT保存修飾"])
        objs = bb.join([v])
        self.assertEqual(len(objs), 2)
        for o, p in zip(objs, v.pieces):
            self.assertTrue(o.complete)
            t, s, d = o.records[0]
            self.assertEqual((t, s, bytes(d[:4])), (28, 0x5446, b"TFBK"))
            self.assertEqual(o.records[1][0], 8)                # 実行機能付箋
            self.assertEqual(sum(1 for r in o.records if r[0] == 1), 1)
            self.assertEqual(p.st["f_nrec"], len(o.records))
            self.assertEqual(p.st["f_size"], sum(len(r[2]) for r in o.records if r[0] != 0))
            self.assertEqual(bb.compress(bb.decompress(p.stream)), p.stream)
        lk = bb.link_parse(objs[0].records[2][2])
        self.assertEqual((lk["objid"], lk["f_name"]), (objs[1].objid, "KT保存修飾"))
        self.assertEqual(list(objs[1].records[-1][:2]), [15, 3])
        # written again from what was read, byte for byte
        items, h = bb.rewrite([v])
        self.assertEqual(bb.Writer(items, h["total"], h["nobj"], h["src"], v.memo).volumes(), [data])
        # and listed by the tool without a complaint
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "btbackup.py"), "list", "-r", TFBK],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn(b"2 objects, 2 complete", r.stdout)

    def test_extract_pack(self):
        tmp = tempfile.mkdtemp()
        try:
            out = os.path.join(tmp, "x")
            self.assertEqual(bb.main(["extract", SAMPLE, "-o", out]), 0)
            names = sorted(os.listdir(out))
            self.assertEqual(len(names), 5)             # volume.json and 4 objects
            self.assertIn("r0001.t0.s0.link", os.listdir(os.path.join(out, names[0])))
            packed = os.path.join(tmp, "p.TAD")
            self.assertEqual(bb.main(["pack", out, "-o", packed]), 0)
            with open(packed, "rb") as f:
                self.assertEqual(f.read(), sample())
            # the totals worked out from the objects are the same
            self.assertEqual(bb.main(["pack", out, "-o", packed, "--recompute"]), 0)
            with open(packed, "rb") as f:
                self.assertEqual(f.read(), sample())
            self.assertEqual(bb.main(["recompress", "--check", SAMPLE]), 0)
        finally:
            shutil.rmtree(tmp)


def host_cc():
    for cc in ("cc", "gcc", "clang"):
        if shutil.which(cc):
            return cc
    return None


@unittest.skipIf(host_cc() is None, "no C compiler on this host")
class TestHostC(unittest.TestCase):
    """lib/libbtbk built for the host must agree with the Python codec."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.tmp, "btbk_host")
        srcs = [os.path.join(TOOLS, "tests", "btbk_host.c")]
        for d, pre in (("libbtbk", "btbk_"), ("libbpk", "bpk_")):
            dd = os.path.join(TOP, "lib", d)
            srcs += [os.path.join(dd, f) for f in sorted(os.listdir(dd))
                     if f.startswith(pre) and f.endswith(".c")]
        subprocess.check_call([host_cc(), "-O2", "-I", os.path.join(TOP, "include"),
                               "-o", cls.exe] + srcs)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def run_exe(self, *args):
        return subprocess.run([self.exe] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)

    def test_check(self):
        r = self.run_exe("check", SAMPLE)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn(b"objects 4 pieces 4 records 15", r.stdout)
        self.assertIn(b"recompress-differ 0 rewrite-differ 0", r.stdout)

    def test_round_trip(self):
        # TAD to xmlTAD (lib/libbpk), back to TAD (lib/libbtbk), to xmlTAD again: the same
        bpk = os.path.join(XTAD, "01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_1.bin")
        for args in (("rtarc", SAMPLE), ("rtarc", TFBK), ("rtbpk", bpk)):
            r = self.run_exe(*args)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn(b" differ 0", r.stdout.splitlines()[-1], r.stdout[-2000:])

    def test_check_tessronos(self):
        r = self.run_exe("check", TFBK)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn(b"recompress-differ 0 rewrite-differ 0", r.stdout)

    def test_codec(self):
        rnd = random.Random(3)
        for n in (0, 1, 3, 4, 4094, 4095, 4097, 9000, 70000):
            data = bytes(rnd.choice(b"abcdefg\0\0\0") for _ in range(n))
            i = os.path.join(self.tmp, "in")
            o = os.path.join(self.tmp, "out")
            with open(i, "wb") as f:
                f.write(data)
            self.assertEqual(self.run_exe("compress", i, o).returncode, 0)
            with open(o, "rb") as f:
                self.assertEqual(f.read(), bb.compress(data))
            self.assertEqual(self.run_exe("decompress", o, i).returncode, 0)
            with open(i, "rb") as f:
                self.assertEqual(f.read(), data)

    def test_tad_overlay_link(self):
        # a figure's overlay is written once, from its words, not again from
        # the shapes inside it; a link's chsz in points comes back the same
        words = "786,65456,18,0,1,1,1,0,10,20,50,40"
        xml = ('<tad version="1.0" encoding="UTF-8"><figure>'
               '<figView left="0" top="0" right="200" bottom="100"/>'
               '<figDraw left="0" top="0" right="200" bottom="100"/>'
               '<figScale hunit="-120" vunit="-120"/>'
               '<figoverlay number="2" even="false" odd="true" overlayData="%s">'
               '<rect round="0" lineType="0" lineWidth="1" l_pat="1" f_pat="1" angle="0"'
               ' left="10" top="20" right="50" bottom="40" zIndex="1" /></figoverlay>'
               '<figoverlay active="2" />'
               '<link id="01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_0.xtad" vobjleft="5" vobjtop="48"'
               ' vobjright="164" vobjbottom="80" height="32" chsz="14"/>'
               '</figure></tad>' % words)
        path = os.path.join(self.tmp, "ov.xtad")
        with open(path, "w", encoding="utf-8") as f:
            f.write(xml)
        r = self.run_exe("tad", path)
        self.assertEqual(r.returncode, 0, r.stdout)
        out = r.stdout.decode("utf-8")
        self.assertIn(" check 0 ", out)
        self.assertEqual(out.count("<rect"), 1, out)
        self.assertEqual(out.count('overlayData="%s"' % words), 1, out)
        self.assertIn('chsz="14"', out)

    def test_tad(self):
        # every record of etc/xtad is written as TAD that walks clean
        names = [f for f in os.listdir(XTAD) if f.endswith(".xtad")]
        self.assertGreater(len(names), 20)
        for name in names:
            r = self.run_exe("tad", os.path.join(XTAD, name))
            self.assertEqual(r.returncode, 0, name)
            self.assertIn(b" check 0 ", r.stdout, name)


if __name__ == "__main__":
    unittest.main()
