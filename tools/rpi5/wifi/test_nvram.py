#!/usr/bin/env python3
"""Tests of the reading of the Wi-Fi's board settings (nvram.py, mkobj.py).

  test_nvram.py [FW_DIR]

With FW_DIR (tools/rpi5/wifi/fw.sh), the real text is made into its
document by mkobj.py and read back: the settings must be the text's lines
without its comments and blank lines. The cases below hold the same
documents as the kernel's test (tests/ktest/ktest_wifinv.c).
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvram  # noqa: E402

HEAD = '<tad version="1.0" encoding="UTF-8" filename="t">\n<document>\n'
TAIL = '</document>\n</tad>\n'

CASES = [
    # notes, a comment, a blank line and two settings
    ("<p>【注記】出典とライセンス</p>\n<p># SPDX-License-Identifier: GPL-2.0+</p>\n<p></p>\n"
     "<p>aa2g=1</p>\n<p>boardflags=0x00480201</p>\n",
     "aa2g=1\nboardflags=0x00480201\n"),
    # full-width letters, digits and signs, and the blanks around '='
    ("<p>ａａ５ｇ　＝　１</p>\n<p>  devid = 0x43ab  </p>\n",
     "aa5g=1\ndevid=0x43ab\n"),
    # a line break inside a paragraph, and an inline element's text
    ("<p>ldo1=4<br/>macaddr=b8:27:eb:74:f2:6c</p>\n<p><font size=\"12\">nocrc=1</font></p>\n",
     "ldo1=4\nmacaddr=b8:27:eb:74:f2:6c\nnocrc=1\n"),
    # what is not a setting: Japanese left in a line, no '='
    ("<p>名前=1</p>\n<p>sromrev</p>\n<p>sromrev=11</p>\n",
     "sromrev=11\n"),
    # references: the characters they stand for
    ("<p>x=a&amp;b</p>\n<p>y=&lt;1&gt;</p>\n",
     "x=a&b\ny=<1>\n"),
    # a note that runs over two lines is a note as a whole
    ("<p>【注記】一行目<br/>key=ignored</p>\n<p>k=v</p>\n",
     "k=v\n"),
]


def main():
    n = bad = 0
    for body, want in CASES:
        n += 1
        got = nvram.extract(HEAD + body + TAIL)
        if got != want:
            bad += 1
            print("FAIL case %d: %r, want %r" % (n, got, want))
    if len(sys.argv) > 1:
        fw = sys.argv[1]
        with tempfile.TemporaryDirectory() as out:
            box = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                               "etc", "def", "01a0d8c4-5211-7c52-9e0a-3b61d27f4c85_0.xtad")
            subprocess.check_call([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkobj.py"),
                                   "--fw", fw, "--box", box, "--out", out])
            doc = open(os.path.join(out, "01a0d8c4-5486-70b7-ac28-5f0162735486_0.xtad"), encoding="utf-8").read()
            text = open(os.path.join(fw, "brcmfmac43455-sdio.raspberrypi,4-model-b.txt"), encoding="ascii").read()
            got = nvram.extract(doc)
            want = nvram.plain(text)
            n += 1
            if got != want:
                bad += 1
                print("FAIL the real text: %d lines read, %d in the text" % (got.count("\n"), want.count("\n")))
            else:
                print("the real text: %d settings read back, the same as the text's" % want.count("\n"))
    print("test_nvram: %d/%d" % (n - bad, n))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
