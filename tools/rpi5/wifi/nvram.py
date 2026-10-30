#!/usr/bin/env python3
"""The board's settings of the Raspberry Pi 5's Wi-Fi, read out of their document.

The same rules as the kernel's (peripheral_kernel/network/netbsd/wifi_nvram.c),
for tools/rpi5/wifi/mkobj.py to check what it wrote and for test_nvram.py:

  - each paragraph (<p>) of the document is read for its text; a line
    break in it (<br/>) and the end of the paragraph both end a line;
  - a paragraph that starts with NOTE is a note, not a setting;
  - full-width letters, digits and signs (U+FF01 to U+FF5E) and the
    full-width space are taken as their ASCII ones;
  - a line is trimmed; an empty one and one that starts with '#' are
    skipped; the blanks around its first '=' are dropped;
  - a line with anything else than printable ASCII, or without '=', is
    not a setting either (the kernel says so on the console).

The result is the lines "name=value", each ended by a newline: the text
NetBSD's bwfm(4) is given for the board's NVRAM file.
"""

import sys
import xml.etree.ElementTree as ET

NOTE = "【注記】"


def _half(s):
    out = []
    for c in s:
        o = ord(c)
        if 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        elif o == 0x3000 or c == "\t":
            out.append(" ")
        else:
            out.append(c)
    return "".join(out)


def _line(s):
    """A line as the setting it is, or None when it is none (and why)."""
    s = _half(s).strip(" ")
    if s == "" or s.startswith("#"):
        return None, None
    if any(not (0x20 <= ord(c) <= 0x7E) for c in s):
        return None, "not ASCII"
    if "=" not in s:
        return None, "no '='"
    k, v = s.split("=", 1)
    return k.strip(" ") + "=" + v.strip(" "), None


def _texts(el, out):
    """The text of an element as lines: <br/> starts a new one."""
    if el.text:
        out[-1] += el.text
    for c in el:
        if c.tag == "br":
            out.append("")
        else:
            _texts(c, out)
        if c.tail:
            out[-1] += c.tail


def lines_of(doc):
    """(paragraph, the lines of its text) for each paragraph of the document."""
    root = ET.fromstring(doc)
    body = root.find("document")
    if body is None:
        return []
    paras = []
    for p in body.iter("p"):
        out = [""]
        _texts(p, out)
        lines = []
        for x in out:
            lines += x.replace("\r", "\n").split("\n")
        paras.append(lines)
    return paras


def extract(doc, why=None):
    """The settings of a document (xmlTAD text), as the kernel reads them."""
    res = []
    for lines in lines_of(doc):
        if "".join(lines).lstrip().startswith(NOTE):
            continue
        for x in lines:
            s, bad = _line(x)
            if s is not None:
                res.append(s + "\n")
            elif bad is not None and why is not None:
                why.append((x, bad))
    return "".join(res)


def plain(text):
    """The settings of the original text: without its comments and blank lines."""
    res = []
    for x in text.split("\n"):
        s, bad = _line(x)
        if s is not None:
            res.append(s + "\n")
    return "".join(res)


if __name__ == "__main__":
    sys.stdout.write(extract(open(sys.argv[1], encoding="utf-8").read()))
