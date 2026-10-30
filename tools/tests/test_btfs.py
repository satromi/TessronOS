#!/usr/bin/env python3
"""Tests of lib/libbtfs built for the host (design 11.9).

    python3 tools/tests/test_btfs.py

btfs_host (tools/tests/btfs_host.c) and bpk_host are built with the
host's C compiler. A BTRON CD-ROM image made by mkbtcd.py -- in the
semi-standard and the standard form, with logical blocks of 1024, 2048
and 4096 bytes -- is read: the file system, each file's records at index
levels 0, 1 and 2, the runs a connection index adds, the link files.
Then a file and what it links to are written as one archive, and
bpk_host takes that archive out as 書庫管理 would.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unicodedata
import unittest
import xml.etree.ElementTree as ET

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = os.path.dirname(TOOLS)
HERE = os.path.join(TOOLS, "tests")


def host_cc():
    for cc in ("cc", "gcc", "clang"):
        if shutil.which(cc):
            return cc
    return None


@unittest.skipIf(host_cc() is None, "no C compiler on this host")
class TestBtfs(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        bpk = os.path.join(TOP, "lib", "libbpk")
        libs = [os.path.join(bpk, f) for f in sorted(os.listdir(bpk)) if f.endswith(".c")]
        inc = ["-I", os.path.join(TOP, "include")]
        cls.btfs = os.path.join(cls.tmp, "btfs_host")
        cls.bpk = os.path.join(cls.tmp, "bpk_host")
        subprocess.check_call([host_cc(), "-O2"] + inc + ["-o", cls.btfs, os.path.join(HERE, "btfs_host.c"),
                                                          os.path.join(TOP, "lib", "libbtfs", "btfs.c")] + libs)
        subprocess.check_call([host_cc(), "-O2"] + inc + ["-o", cls.bpk, os.path.join(HERE, "bpk_host.c")] + libs)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def make(self, name, *opts):
        out = os.path.join(self.tmp, name)
        subprocess.check_call(["python3", os.path.join(HERE, "mkbtcd.py"), out, "--part", out + ".part", "--json"] + list(opts))
        with open(out + ".json", encoding="utf-8") as f:
            return out, json.load(f)

    def run_exe(self, *args):
        r = subprocess.run(list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode, r.stdout.decode("utf-8", "replace")

    def files(self, text):
        """Each file the list printed: its line and its records' lines"""
        out, cur = {}, None
        for l in text.splitlines():
            m = re.match(r"file (\d+) (.*)", l)
            if m:
                cur = int(m.group(1))
                out[cur] = {"line": m.group(2), "recs": []}
            elif l.startswith(" rec ") and cur is not None:
                out[cur]["recs"].append(l.strip())
        return out

    def check_list(self, path, want, bsize, le):
        code, text = self.run_exe(self.btfs, "list", path)
        self.assertEqual(code, 0, text)
        self.assertIn("fs 試験CD", text)
        self.assertIn("bsize %d le %d format 6402" % (bsize, 1 if le else 0), text)
        fl = self.files(text)
        self.assertEqual(sorted(fl), list(range(9)))
        for fid, w in want["files"].items():
            line = fl[int(fid)]["line"]
            if w.get("link_file"):
                self.assertTrue(line.startswith("link " + unicodedata.normalize("NFKC", w["name"])), line)
                continue
            self.assertIn("name " + unicodedata.normalize("NFKC", w["name"]) + " ", line)
            self.assertIn("level %d" % w["level"], line)
            self.assertIn("records %d" % w["records"], line)
        # a link file elsewhere goes nowhere, one to this file system goes on to its target
        self.assertIn("target -1 fs ほかの区画", fl[7]["line"])
        self.assertIn("target 2 fs 試験CD", fl[8]["line"])
        self.assertIn("rec 4 link 7 to -1", fl[3]["recs"])
        self.assertIn("rec 5 link 8 to 2", fl[3]["recs"])
        # the long text: its runs, joined, are the record (its CRC checked against the one made)
        big = fl[4]["recs"][0]
        self.assertRegex(big, r"type 1 sub 0 size 18932 off 0 runs (\d+)")
        self.assertGreater(int(re.search(r"runs (\d+)", big).group(1)), 2)
        self.assertEqual(sum(1 for r in fl[4]["recs"] if "type 5 sub 7 size 18" in r and r.endswith("er 0")), 130)
        for f in fl.values():
            for r in f["recs"]:
                if " type " in r:
                    self.assertTrue(r.endswith("er 0"), r)
        return text

    def test_forms(self):
        texts = []
        for name, opts, bsize, le in (("le2048.iso", [], 2048, True), ("be1024.iso", ["--be", "--block", "1024"], 1024, False),
                                      ("le4096.iso", ["--block", "4096"], 4096, True)):
            path, want = self.make(name, *opts)
            texts.append(self.check_list(path, want, bsize, le))
            # the partition alone, with no ISO 9660 round it, is read too
            code, text = self.run_exe(self.btfs, "list", path + ".part")
            self.assertEqual(code, 0, text)
            self.assertNotIn("cd partition", text)
            self.assertIn("fs 試験CD", text)
        # whatever the size of a block, the records' bytes are the same; the
        # standard form writes its TAD high byte first, so there only the
        # records that are bytes alone (types 5 and 31) are the same
        crcs = [re.findall(r"crc ([0-9a-f]{8})", t) for t in texts]
        self.assertEqual(crcs[0], crcs[2])
        raw = [re.findall(r"type (?:5|31) sub \d+ size \d+ off \d+ runs \d+ crc ([0-9a-f]{8})", t) for t in texts]
        self.assertEqual(len(raw[0]), 133)
        self.assertEqual(raw[0], raw[1])

    def take_out(self, iso, tag):
        """The root and what it links to as an archive, taken out: each file made, and what was said"""
        bpk = os.path.join(self.tmp, tag + ".bpk")
        code, book = self.run_exe(self.btfs, "book", iso, "0", bpk)
        self.assertEqual(code, 0, book)
        out = os.path.join(self.tmp, tag)
        os.mkdir(out)
        code, text = self.run_exe(self.bpk, "archive", bpk, out)
        self.assertEqual(code, 0, text)
        files = {}
        for f in sorted(os.listdir(out)):
            with open(os.path.join(out, f), "rb") as fp:
                files[f] = fp.read()
        return files, book, text

    def test_standard_form(self):
        """The standard form, its TAD high byte first, becomes the very xmlTAD the semi-standard form does"""
        le, _ = self.make("same_le.iso")
        be, _ = self.make("same_be.iso", "--be")
        a, abook, atext = self.take_out(le, "same_le")
        b, bbook, btext = self.take_out(be, "same_be")
        self.assertEqual(sorted(a), sorted(b))
        self.assertGreaterEqual(len(a), 6)
        # a segment kept as it was (the 仮身 of 資料 whose link goes nowhere)
        # keeps its bytes in its own order, marked so
        kept = re.compile(rb'(<tadseg id="[0-9a-f]+")( byteorder="big")? data="[0-9a-f]*"')
        for name in a:
            self.assertNotIn(b'byteorder="big"', a[name], name)
            self.assertEqual(len(kept.findall(a[name])), b[name].count(b'byteorder="big"'), name)
            self.assertEqual(kept.sub(rb"\1", a[name]), kept.sub(rb"\1", b[name]), name)
        self.assertEqual(b["f0003-0000-0000-0000-000000000000_0.xtad"].count(b'byteorder="big"'), 1)
        self.assertIn(b"<figure>", a["f0000-0000-0000-0000-000000000000_0.xtad"])
        self.assertIn("BTRONのCD−ROMの試験です。".encode("utf-8"), a["f0001-0000-0000-0000-000000000000_0.xtad"])
        self.assertIn(b"<rect ", a["f0002-0000-0000-0000-000000000000_0.xtad"])
        self.assertIn(b"bgcol=\"#ffffff\"", a["f0002-0000-0000-0000-000000000000_0.xtad"])
        self.assertEqual(atext, btext)
        # the 実行機能付箋 read the same: 基本図形編集 and 仮身一覧, and their windows
        ex = lambda t: [l for l in t.splitlines() if l.startswith("exec ")]
        self.assertEqual(ex(abook), ["exec 2 8000-0002-8000 win 30 40 630 440",
                                     "exec 3 8000-0001-8000 win 60 60 560 460"])
        self.assertEqual(ex(abook), ex(bbook))

    def test_book(self):
        path, want = self.make("book.iso")
        bpk = os.path.join(self.tmp, "root.bpk")
        code, text = self.run_exe(self.btfs, "book", path, "0", bpk)
        self.assertEqual(code, 0, text)
        self.assertIn("book er 0 files 7", text)
        out = os.path.join(self.tmp, "root")
        os.mkdir(out)
        code, text = self.run_exe(self.bpk, "archive", bpk, out)
        self.assertEqual(code, 0, text)
        names = re.findall(r"object (\d+) (\S+)", text)
        self.assertEqual([n for _, n in names], ["試験CD", "はじめに", "図1", "資料", "大きな実身", "多段の実身", "住所カード"])

        def xml(i):
            with open(os.path.join(out, "f%04d-0000-0000-0000-000000000000_0.xtad" % i), encoding="utf-8") as f:
                return ET.fromstring(f.read())
        # the root: a figure of six 仮身, each to the object of its link record
        links = xml(0).find("figure").findall("link")
        self.assertEqual([l.get("id")[:5] for l in links], ["f0001", "f0002", "f0003", "f0004", "f0005", "f0006"])
        # the text and its 仮身 to the figure
        doc = xml(1).find("document")
        self.assertIn("BTRONのCD−ROMの試験です。", "".join(doc.itertext()))
        self.assertEqual(doc.find(".//link").get("id")[:5], "f0002")
        # the figure: two rectangles and a 仮身 back to the text
        fig = xml(2).find("figure")
        self.assertEqual(len(fig.findall("rect")), 2)
        self.assertEqual(fig.find("link").get("id")[:5], "f0001")
        # the long text whole
        text4 = "".join(xml(4).find("document").itertext())
        self.assertEqual(text4.count("あいうえおかきくけこさしすせそたちつてと"), 450)
        # the text read through index level 2
        self.assertIn("段を重ねた索引の実身。", "".join(xml(5).find("document").itertext()))

    def test_iso_side(self):
        """The ISO 9660 side beside the partition: Joliet's names, a directory, the files' bytes"""
        path, _ = self.make("side.iso")
        code, text = self.run_exe(self.btfs, "iso", path)
        self.assertEqual(code, 0, text)
        lines = [l for l in text.splitlines() if not l.startswith("iso ")]
        self.assertEqual(sorted(l.split()[0] + " " + l.split()[1] for l in lines),
                         ["dir 文書", "file HDA2.IMG", "file はじめにお読みください.txt"])
        self.assertTrue(all(l.endswith(" joliet") for l in lines), text)
        code, text = self.run_exe(self.btfs, "iso", path, "文書")
        self.assertEqual(code, 0, text)
        self.assertIn("file メモ.txt 27 ", text)
        code, text = self.run_exe(self.btfs, "iso", path, "文書/メモ.txt")
        self.assertEqual(code, 0, text)
        self.assertEqual(text, "ISO 9660 の側のメモです。\n")
        code, text = self.run_exe(self.btfs, "iso", path, "はじめにお読みください.TXT")
        self.assertEqual(code, 0, text)
        self.assertIn("BTRON partition", text)
        code, text = self.run_exe(self.btfs, "iso", path, "ない")
        self.assertEqual(code, 1, text)

    def test_book_many(self):
        """More objects than a 書籍 holds (650) taken in at once: the import's archive is not one"""
        n = 700
        path, want = self.make("many.iso", "--many", str(n))
        bpk = os.path.join(self.tmp, "many.bpk")
        code, text = self.run_exe(self.btfs, "book", path, "0", bpk)
        self.assertEqual(code, 0, text)
        self.assertIn("files %d" % (n + 2), text)
        out = os.path.join(self.tmp, "many")
        os.mkdir(out)
        code, text = self.run_exe(self.bpk, "archive", bpk, out)
        self.assertEqual(code, 0, text)
        names = re.findall(r"object (\d+) (\S+)", text)
        self.assertEqual(len(names), n + 2)
        self.assertEqual(names[1][1], "多数の実身")
        self.assertEqual(sorted(nm for _, nm in names[2:]), sorted("文章%d" % (i + 1) for i in range(n)))
        with open(os.path.join(out, "f0001-0000-0000-0000-000000000000_0.xtad"), encoding="utf-8") as f:
            x = f.read()
        self.assertEqual(len(re.findall(r'<link id="f', x)), n)
        last = "f%04d-0000-0000-0000-000000000000_0.xtad" % (n + 1)
        with open(os.path.join(out, last), encoding="utf-8") as f:
            self.assertIn("番の中身。", f.read())

    def test_book_links_elsewhere(self):
        """資料: a link file to another file system goes nowhere, one to 図１ goes to it"""
        path, _ = self.make("docs.iso")
        bpk = os.path.join(self.tmp, "docs.bpk")
        code, text = self.run_exe(self.btfs, "book", path, "3", bpk)
        self.assertEqual(code, 0, text)
        self.assertIn("files 5", text)
        out = os.path.join(self.tmp, "docs")
        os.mkdir(out)
        code, text = self.run_exe(self.bpk, "archive", bpk, out)
        self.assertEqual(code, 0, text)
        with open(os.path.join(out, "f0000-0000-0000-0000-000000000000_0.xtad"), encoding="utf-8") as f:
            x = f.read()
        ids = re.findall(r'<link id="([^"]*)"', x)
        self.assertEqual(len(ids), 4, x)          # the one that goes nowhere is left out
        self.assertEqual([i[:5] for i in ids], ["f0001", "f0002", "f0003", "f0004"])


if __name__ == "__main__":
    unittest.main()
