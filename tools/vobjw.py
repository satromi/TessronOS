#!/usr/bin/env python3
"""The size of a closed virtual object, for the tools that write links
into records when the system is built.

The rules are those of include/ts/vobjsz.h and tad_closed_w
(outer_kernel/tad/tad_link.c): with a frame and the picture, at letters
of c pixels,

  closed height   c + c/4 + 2                    (19 at 14)
  closed width    the name's width + (c/2)*3 + c  (35 at 14), at least 2c

The name's width is measured with the system's face
(etc/font/NotoSansJP-Regular.otf) by the advances of its letters at c
pixels. The desktop sums the advances in 64ths of a pixel and rounds
the sum; here each letter's advance is rounded up before it is added,
so that the box is never a pixel short of the name.

The advances are read with fontTools, or, where it is not to be had (the
build runs some tools with python3 -I, which leaves out the user's
packages), straight from the face's cmap and hmtx tables; both give the
same numbers. When the face is not there, every letter is taken to be as
wide as the widest of its kind in the face (a full letter c, a letter of
ASCII 0.95c), which is never too narrow, and a warning is printed once.

New links are laid out as the system adds them to a box
(om_obj_link_add): from PAD on the left and at the top, each one GAP
below the bottom of the one before.
"""

import math
import os
import struct
import sys

TOP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
FACE = os.path.join(TOP, "etc", "font", "NotoSansJP-Regular.otf")

CHSZ = 14               # VOBJ_CHSZ_DEF
OPEN_SLACK = 8          # VOBJ_OPEN_SLACK
PAD = 8                 # left and top of the first link a box is given
GAP = 8                 # between one link and the next below it

_face = None            # (units per em, {code point: advance}) or False


def _read_face(path):
    """(units per em, {code point: advance}) from the face's head, hhea, hmtx and cmap tables"""
    data = open(path, "rb").read()
    n = struct.unpack_from(">H", data, 4)[0]
    tab = {}
    for i in range(n):
        tag, _, off, _ = struct.unpack_from(">4sIII", data, 12 + i * 16)
        tab[tag] = off
    upem = struct.unpack_from(">H", data, tab[b"head"] + 18)[0]
    nhm = struct.unpack_from(">H", data, tab[b"hhea"] + 34)[0]
    hm = tab[b"hmtx"]
    advs = [struct.unpack_from(">H", data, hm + 4 * g)[0] for g in range(nhm)]

    def adv(g):
        return advs[g] if g < nhm else advs[-1]

    cm = tab[b"cmap"]
    subs = {}
    for i in range(struct.unpack_from(">H", data, cm + 2)[0]):
        pid, eid, off = struct.unpack_from(">HHI", data, cm + 4 + i * 8)
        subs[(pid, eid)] = cm + off
    out = {}
    if (3, 10) in subs:                         # format 12: groups of code points
        st = subs[(3, 10)]
        for i in range(struct.unpack_from(">I", data, st + 12)[0]):
            a, b, g = struct.unpack_from(">III", data, st + 16 + i * 12)
            for c in range(a, b + 1):
                out[c] = adv(g + c - a)
    elif (3, 1) in subs:                        # format 4: segments of the BMP
        st = subs[(3, 1)]
        seg = struct.unpack_from(">H", data, st + 6)[0] // 2
        ends, starts = st + 14, st + 16 + seg * 2
        deltas, ranges = starts + seg * 2, starts + seg * 4
        for s in range(seg):
            e, a = struct.unpack_from(">H", data, ends + s * 2)[0], struct.unpack_from(">H", data, starts + s * 2)[0]
            d = struct.unpack_from(">h", data, deltas + s * 2)[0]
            r = struct.unpack_from(">H", data, ranges + s * 2)[0]
            for c in range(a, min(e, 0xFFFE) + 1):
                if r == 0:
                    g = (c + d) & 0xFFFF
                else:
                    g = struct.unpack_from(">H", data, ranges + s * 2 + r + (c - a) * 2)[0]
                    g = (g + d) & 0xFFFF if g else 0
                if g:
                    out[c] = adv(g)
    return upem, out


def _load():
    global _face
    if _face is not None:
        return _face
    try:
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            # run with -I the user's packages are not seen: the tables are read here
            _face = _read_face(FACE)
            return _face
        f = TTFont(FACE, lazy=True)
        cmap = f.getBestCmap()
        hmtx = f["hmtx"]
        _face = (f["head"].unitsPerEm, {c: hmtx[g][0] for c, g in cmap.items()})
    except (OSError, KeyError, struct.error) as e:
        sys.stderr.write("vobjw: %s (%s): the names' widths are estimated\n" % (FACE, e))
        _face = False
    return _face


def ch(c):
    return c if c > 0 else CHSZ


def closed_h(c=CHSZ, frame=True, pict=True):
    c = ch(c)
    return c + c // 4 + 2 if frame else c + c // 4 if pict else c


def least_w(c=CHSZ, frame=True, pict=True):
    c = ch(c)
    return c * 2 if frame else c + c // 4 if pict else c


def edge_w(c=CHSZ):
    c = ch(c)
    return (c // 2) * 3 + c


def name_w(name, c=CHSZ):
    """The width of a name at letters of c pixels, each letter rounded up"""
    c = ch(c)
    face = _load()
    w = 0
    for s in name:
        cp = ord(s)
        if face:
            upem, adv = face
            a = adv.get(cp)
            if a is None:
                w += c          # from another face: a full letter
            else:
                w += -((-a * c) // upem)
        else:
            w += math.ceil(c * 0.95) if cp < 0x80 else c
    return w


def closed_w(name, c=CHSZ, frame=True, pict=True):
    """How wide a new closed link is made to hold its name (tad_closed_w)"""
    c = ch(c)
    w = name_w(name, c)
    if frame:
        w += edge_w(c) if pict else (c // 2) * 2
    elif pict:
        w += c + c // 4
    return max(w, least_w(c, frame, pict))


if __name__ == "__main__":
    for a in sys.argv[1:]:
        print("%d %s" % (closed_w(a), a))
