#!/usr/bin/env python3
"""Tests of lib/libbpk built for the host (design 17.16).

    python3 tools/tests/test_bpk.py

bpk_host (tools/tests/bpk_host.c) is built with the host's C compiler
from lib/libbpk. The tests give it records of binary TAD made here, the
sample archive of etc/xtad, and TRON code, and read what comes back.
"""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = os.path.dirname(TOOLS)
SAMPLE = os.path.join(TOP, "etc", "xtad", "01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_1.bin")

# ---------------------------------------------------------------- a picture, compressed

W, H = 77, 41


def picture():
    """1 = black: a ring, a diagonal, a filled box and a dotted last row"""
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            dx, dy = x - 30, y - 20
            d = dx * dx * 4 + dy * dy * 9
            b = 2000 <= d <= 2900
            b |= abs(x * 40 - y * 76) < 40
            b |= 50 <= x < 75 and 5 <= y < 21
            b |= y == H - 1 and x % 3 == 0
            row.append(1 if b else 0)
        rows.append(row)
    return rows


# The picture as CCITT T.4 MH and MR (EOL before each line)
MH = bytes.fromhex(
    "001355b200047f670018f73001cf50605c007fa01813001a7819b8061c00518e26a2401870012052e28e061c004e3480"
    "d2e003427d2066b800ccb1240cc7001f2fc9865b800f11f56194e003c61ea00c3800e1d583030e0038c66200c3800d99"
    "7d38187001b309d4061c006d93f4030e0036cc640187001b60690061c006d8d643100036c4b3b100036627f988001b31"
    "5f0c4000e317e197001c328197001e328306003c42c40c007c85087c4000cc848ad0c001a10d17758009c69074f50012"
    "052e28786003a09a895e8001a8334bf40014e0615fe0020c181af000773e000767c7001354e9d3a74e9d3a74e9d3a74e"
    "9d3a74e9d3a74e9d3a74f8"
)
MR = bytes.fromhex(
    "0019aad9000267d9c0071ee60020cfa90605c007fd00c09800833e06700c40068c71351200c38009882061f0019c6901"
    "a5c004822f87800e658920663800926727800f88fab0ca7001506751800f0eac18187001506711800eccbe9c0c3800b0"
    "cf56003b64fd00c3800b0ce46003b60690061c005b390588001db12cec4000ac33fe003b315f0c40009c33c2800f0ca0"
    "65c004db5001f10b10300136911f800e642456860009b499eb0019c69074f5001060c10419c3001e826a257a00041884"
    "19e80034e0615fe00220c180cf8003dcf800106a003354e9d3a74e9d3a74e9d3a74e9d3a74e9d3a74e9d3a74f8"
)

# ---------------------------------------------------------------- binary TAD made here

TS_TEXT, TS_TEXTEND, TS_FIG, TS_FIGEND, TS_IMAGE = 0xFFE1, 0xFFE2, 0xFFE3, 0xFFE4, 0xFFE5
TS_DFUSEN, TS_TPAGE, TS_TRULER, TS_TFONT, TS_TATTR = 0xFFE7, 0xFFA0, 0xFFA1, 0xFFA2, 0xFFA4
TS_TVAR, TS_TAPPL, TS_FDEF, TS_FAPPL, TS_FFUSEN = 0xFFAD, 0xFFAF, 0xFFB1, 0xFFBF, 0xFFE8
TS_FPRIM, TS_FPAGE, TS_VOBJ = 0xFFB0, 0xFFB5, 0xFFE6


def words(*ws):
    return b"".join(struct.pack("<H", w & 0xFFFF) for w in ws)


def seg(sid, body):
    if len(body) >= 0xFFFF:
        return struct.pack("<HHI", sid, 0xFFFF, len(body)) + body
    return struct.pack("<HH", sid, len(body)) + body


def fusen(sid, sub, attr, body=b""):
    return seg(sid, words((sub << 8) | attr) + body)


def chars(*codes):
    return words(*codes)


def text_start():
    return seg(TS_TEXT, words(0, 0, 0, 0, 0, 0, 0, 0, -120, -120, 0x21, 0))


def fig_start():
    return seg(TS_FIG, words(0, 0, 200, 100, 0, 0, 200, 100, -120, -120, 0, 0))


def tron(s):
    """A string of JIS X 0208 characters as TRON code"""
    out = []
    for ch in s:
        b = ch.encode("euc_jp")
        out.append(((b[0] & 0x7F) << 8) | (b[1] & 0x7F))
    return chars(*out)


def calc_cell(row, col, size, deco, rule, colour):
    data = words(0, row, col, 0, (deco << 8) | size, (colour << 8) | rule)
    name = words(0x493D, 0x4955, 0x6435) + b"\0" * 26
    body = words(*([0] * 12)) + words(0x8000, 0x0009, 0x8000) + name + struct.pack("<I", len(data)) + data
    return seg(TS_DFUSEN, body)


def image_seg(data, compac):
    head = words(0, 0, W, H, 0, 0, W, H, -120, -120, 0, 0, 0, 0, 0, 0)
    head += struct.pack("<III", 0, 0, 0)
    head += words(compac, 1, 0x0101, (W + 15) // 16 * 2, 0, 0, W, H)
    head += struct.pack("<I", 0x44)
    assert len(head) == 0x40
    return seg(TS_IMAGE, head + data)


def png_pixels(path):
    """The RGB of each pixel of a PNG of bpk_png_encode (filter 0 on every row)"""
    b = open(path, "rb").read()
    assert b[:8] == b"\x89PNG\r\n\x1a\n"
    at, idat, w = 8, b"", 0
    while at < len(b):
        n, kind = struct.unpack(">I4s", b[at:at + 8])
        body = b[at + 8:at + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        at += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 3
    rows = []
    for y in range(h):
        r = raw[y * (1 + w * bpp) + 1:(y + 1) * (1 + w * bpp)]
        rows.append([r[x * bpp:x * bpp + 3] for x in range(w)])
    return rows


def host_cc():
    for cc in ("cc", "gcc", "clang"):
        if shutil.which(cc):
            return cc
    return None


@unittest.skipIf(host_cc() is None, "no C compiler on this host")
class TestBpk(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.tmp, "bpk_host")
        d = os.path.join(TOP, "lib", "libbpk")
        srcs = [os.path.join(TOOLS, "tests", "bpk_host.c")]
        srcs += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".c")]
        subprocess.check_call([host_cc(), "-O2", "-I", os.path.join(TOP, "include"), "-o", cls.exe] + srcs)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def run_exe(self, *args):
        r = subprocess.run([self.exe] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return r.returncode, r.stdout

    def path(self, name):
        return os.path.join(self.tmp, name)

    def convert(self, rec):
        with open(self.path("rec"), "wb") as f:
            f.write(rec)
        code, out = self.run_exe("tad", self.path("rec"), self.path("out.xtad"))
        self.assertEqual(code, 0, out)
        with open(self.path("out.xtad"), encoding="utf-8") as f:
            xml = f.read()
        return xml, ET.fromstring(xml), out

    # ------------------------------------------------------------ the characters

    def to_tron(self, s):
        with open(self.path("u8"), "w", encoding="utf-8") as f:
            f.write(s)
        code, out = self.run_exe("totron", self.path("u8"))
        self.assertEqual(code, 0)
        return [int(w, 16) for w in out.split()]

    def from_tron(self, ws):
        with open(self.path("tc"), "wb") as f:
            f.write(words(*ws))
        code, out = self.run_exe("fromtron", self.path("tc"))
        self.assertEqual(code, 0)
        return out.decode("utf-8")

    def test_jis_forms(self):
        # 〜 and − come out in the JIS form and go back from either form
        self.assertEqual(self.from_tron([0x2141, 0x215D]), "\u301c\u2212")
        self.assertEqual(self.to_tron("\u301c\uff5e\u2212\uff0d"), [0x2141, 0x2141, 0x215D, 0x215D])
        self.assertEqual(self.to_tron("\u00ac\uffe2\u2016\u2225"), [0x224C, 0x224C, 0x2142, 0x2142])

    def test_other_sets(self):
        cases = []
        # JIS X 0212 in zone B from A1
        b = "丂".encode("euc_jp")
        cases.append(("丂", (b[1] << 8) | (b[2] - 0x80), True))
        # JIS X 0213's first plane in zone A's gaps, a character past the BMP
        b = "\U0002000b".encode("euc_jis_2004")
        cases.append(("\U0002000b", ((b[0] & 0x7F) << 8) | (b[1] & 0x7F), True))
        # a letter with a mark that has a place of its own
        b = "か゚".encode("euc_jis_2004")
        cases.append(("か゚", ((b[0] & 0x7F) << 8) | (b[1] & 0x7F), True))
        # GB 2312 in zone C, KS X 1001 in zone D: read, and written in plane 16
        for ch, codec, base in (("们", "gb2312", 0x21), ("한", "euc_kr", 0xB7)):
            b = ch.encode(codec)
            p = (b[0] - 0xA1) * 94 + (b[1] - 0xA1)
            cases.append((ch, ((base + p // 126) << 8) | (0x80 + p % 126), False))
        for ch, code, back in cases:
            self.assertEqual(self.from_tron([code]), ch, hex(code))
            if back:
                self.assertEqual(self.to_tron(ch), [code], ch)
            else:
                ws = self.to_tron(ch)
                self.assertIn(ws[0], (0xFE30, 0xFE31), ch)
                self.assertEqual(self.from_tron(ws), ch)

    def test_x0213_plane2(self):
        # the first character of JIS X 0213's second plane, row 1 cell 1
        ch = bytes([0x8F, 0xA1, 0xA1]).decode("euc_jis_2004")
        self.assertEqual(self.from_tron([0x8721]), ch)
        self.assertEqual(self.from_tron(self.to_tron(ch)), ch)

    def test_no_unicode(self):
        # a character of plane 9 (大漢和): 〓, its plane and code kept
        rec = text_start() + chars(0x2341, 0xFE29, 0x9830, 0x9831, 0xFE21, 0x2342) + seg(TS_TEXTEND, b"")
        xml, root, _ = self.convert(rec)
        t = root.find(".//tchar")
        self.assertIsNotNone(t)
        self.assertEqual(t.get("plane"), "9")
        self.assertEqual(t.get("code"), "9830 9831")
        self.assertEqual(t.text, "\u3013\u3013")
        self.assertIn("A<tchar", xml)
        self.assertIn("</tchar>B", xml)

    # ------------------------------------------------------------ a text

    def test_text(self):
        overlay = (fusen(TS_TPAGE, 8, 0) + fusen(TS_TRULER, 1, 1) + chars(0x215D)
                   + fusen(TS_TVAR, 0, 0, words(200)) + chars(0x215D))
        rec = (text_start()
               + fusen(TS_TPAGE, 0, 0, words(1403, 992, 94, 70, 108, 85))
               + fusen(TS_TPAGE, 3, 0x20, overlay)
               + fusen(TS_TPAGE, 4, 0, words(0x8000))
               + fusen(TS_TATTR, 8, 0x11, tron("、。"))
               + fusen(TS_TATTR, 9, 0x11, tron("（「"))
               + tron("本文") + chars(0x000A)
               + fusen(TS_TVAR, 0, 0, words(121))
               + fusen(TS_TFONT, 5, 0, words(90)) + fusen(TS_TFONT, 7, 1, words(0x0102))
               + fusen(TS_TAPPL, 2, 0, words(0x8000, 0x0003, 0x8000))
               + fusen(TS_FFUSEN, 0, 0, words(1, 2, 3))
               + calc_cell(3, 2, 5, 0x01, 0x11, 2) + tron("表") + fusen(TS_TRULER, 5, 0)
               + seg(TS_TEXTEND, b""))
        xml, root, out = self.convert(rec)
        doc = root.find("document")
        # the page's settings stand before the first paragraph
        kids = [c.tag for c in doc]
        first_p = kids.index("p")
        for tag in ("paper", "paper-overlay-define", "docoverlay", "line-head-kinsoku", "line-tail-kinsoku"):
            self.assertLess(kids.index(tag), first_p, tag)
        ov = doc.find("paper-overlay-define")
        self.assertEqual((ov.get("N"), ov.get("P")), ("0", "2"))
        p = ov.find("p")
        self.assertIsNotNone(p.find("fill-line"))
        self.assertEqual(p.find("page-number").get("num"), "1")
        self.assertEqual(doc.find("docoverlay").get("active"), "0")
        k = doc.find("line-head-kinsoku")
        self.assertEqual((k.get("kind"), k.get("ch")), ("0x11", "、。"))
        self.assertEqual(doc.find("line-tail-kinsoku").get("ch"), "（「")
        self.assertEqual(doc.find(".//variable").get("id"), "121")
        self.assertEqual(doc.find(".//font[@rotation]").get("rotation"), "90")
        self.assertEqual(doc.find(".//font[@baseshift]").get("baseshift"), "0.5")
        a = doc.find(".//docappl")
        self.assertEqual(a.get("appl"), "8000-0003-8000")
        self.assertEqual(a.get("data"), "0002008003000080")
        s = doc.find(".//tadseg")
        self.assertEqual((s.get("id"), s.get("data")), ("ffe8", "0000010002000300"))
        self.assertIn(b"tadseg 1 ", out, xml)
        self.assertEqual(doc.find(".//calcPos").get("cell"), "B3")
        self.assertIn('<calcPos cell="B3"/><font size="36"/><bold><calcCell', xml)
        self.assertIn("表</bold><tab/>", xml)

    # ------------------------------------------------------------ a figure

    def test_figure(self):
        for name, data, compac in (("MH", MH, 1), ("MR2", MR, 2), ("MR4", MR, 3)):
            rec = (fig_start()
                   + fusen(TS_FDEF, 3, 0, words(9, 2) + bytes([0xF0, 0xCC]))
                   + fusen(TS_FAPPL, 0, 0, words(0x8000, 0, 0x8000))
                   + image_seg(data, compac)
                   + seg(TS_FIGEND, b""))
            xml, root, out = self.convert(rec)
            self.assertIn(b"pictures 1 tadseg 0", out, name)
            fig = root.find("figure")
            lt = fig.find("lineTypeDefine")
            self.assertEqual((lt.get("id"), lt.get("nb"), lt.get("mask")), ("9", "2", "f0cc"))
            self.assertEqual(fig.find("figappl").get("appl"), "8000-0000-8000")
            self.assertTrue(fig.find("image").get("href").endswith("_0_0.png"))
            px = png_pixels(self.path("out.xtad") + ".0.png")
            want = picture()
            for y in range(H):
                for x in range(W):
                    black = px[y][x] == b"\0\0\0"
                    self.assertEqual(black, bool(want[y][x]), "%s %d,%d" % (name, x, y))

    def test_figure_overlay(self):
        # an overlay of a figure: a rectangle, on the odd pages, put on
        rect = fusen(TS_FPRIM, 0, 0, words(0x0001, 1, 1, 0, 10, 20, 50, 40))
        rec = (fig_start()
               + fusen(TS_FPAGE, 3, 0x12, rect)
               + fusen(TS_FPAGE, 4, 0, words(0x2000))
               + fusen(TS_FPRIM, 0, 0, words(0x0001, 1, 1, 0, 60, 60, 90, 90))
               + seg(TS_FIGEND, b""))
        xml, root, out = self.convert(rec)
        fig = root.find("figure")
        ovs = fig.findall("figoverlay")
        self.assertEqual(len(ovs), 2, xml)
        d = ovs[0]
        self.assertEqual((d.get("number"), d.get("even"), d.get("odd")), ("2", "false", "true"))
        self.assertTrue(d.get("overlayData").startswith("786,"))
        r = d.find("rect")
        self.assertIsNotNone(r, xml)
        self.assertEqual((r.get("left"), r.get("top"), r.get("right"), r.get("bottom")), ("10", "20", "50", "40"))
        self.assertEqual(ovs[1].get("active"), "2")
        # the figure's own rectangle is the figure's, numbered on its own
        rs = fig.findall("rect")
        self.assertEqual(len(rs), 1)
        self.assertEqual((rs[0].get("left"), rs[0].get("zIndex")), ("60", "1"))

    def test_vobj_sizes(self):
        def vobj(l, t, r, b, chsz):
            return seg(TS_VOBJ, words(l, t, r, b, 204, chsz) + words(0, 0x1000) * 4 + words(0))
        # in a text of 120 units to the inch: a width and height in points,
        # and the name's size in points whatever the CHSIZE was written in
        rec = (text_start() + vobj(5, 48, 164, 80, 24) + vobj(0, 0, 120, 40, 0x8000 | 288)
               + fusen(TS_TFONT, 2, 0, words(0x000C)) + tron("字") + seg(TS_TEXTEND, b""))
        xml, root, _ = self.convert(rec)
        links = root.findall(".//link")
        self.assertEqual(len(links), 2, xml)
        a, b = links
        self.assertEqual((a.get("vobjleft"), a.get("vobjright")), ("5", "164"))
        self.assertEqual((a.get("width"), a.get("heightpx"), a.get("chsz")), ("95", "19", "14"))
        self.assertEqual((b.get("width"), b.get("heightpx"), b.get("chsz")), ("72", "24", "14"))
        self.assertIn('<font size="7.2"/>', xml)
        # in a figure: the rectangle alone
        rec = fig_start() + vobj(5, 48, 164, 80, 24) + seg(TS_FIGEND, b"")
        xml, root, _ = self.convert(rec)
        link = root.find(".//link")
        self.assertIsNone(link.get("width"))
        self.assertEqual((link.get("vobjright"), link.get("chsz")), ("164", "14"))

    def test_fax_direct(self):
        for mode, data in (("MH", MH), ("MR", MR)):
            with open(self.path("fax"), "wb") as f:
                f.write(data)
            code, out = self.run_exe("fax", mode, self.path("fax"), str(W), str(H), self.path("plane"))
            self.assertEqual(code, 0, out)
            with open(self.path("plane"), "rb") as f:
                plane = f.read()
            rb = (W + 7) // 8
            for y, row in enumerate(picture()):
                for x, v in enumerate(row):
                    self.assertEqual((plane[y * rb + x // 8] >> (7 - x % 8)) & 1, v, "%s %d,%d" % (mode, x, y))

    # ------------------------------------------------------------ the sample archive

    def test_sample(self):
        out = self.path("sample")
        os.mkdir(out)
        code, text = self.run_exe("archive", SAMPLE, out)
        self.assertEqual(code, 0, text)
        self.assertIn(b"objects 33 records 33 kept 0 pictures 1 tadseg 0", text)
        names = [f for f in os.listdir(out) if f.endswith(".xtad")]
        self.assertEqual(len(names), 33)
        n_over = 0
        for f in names:
            with open(os.path.join(out, f), encoding="utf-8") as fp:
                root = ET.fromstring(fp.read())
            doc = root.find("document")
            if doc is None:
                continue
            ov = doc.find("paper-overlay-define")
            if ov is not None:
                n_over += 1
                self.assertIsNotNone(ov.find(".//page-number"))
                self.assertEqual(doc.find("docoverlay").get("active"), "0")
        self.assertEqual(n_over, 33)


if __name__ == "__main__":
    unittest.main()
