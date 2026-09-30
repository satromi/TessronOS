#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkpict.py -- 画像を TessronOS の絵の記録に直す(設計書 16.2.3)

    mkpict.py <出力.pic> <入力画像> [幅 高さ]

出力は絵の記録:

    UH width, UH height, UH depth, UH next     (8 バイト)
    画素 width*height 個                        (depth 32 なら各 4 バイト)

画素は 0x00rrggbb のリトルエンディアン。next は次の寸法の頭までのバイト数で、
最後の寸法は 0。幅と高さを与えると、その大きさに縮めてから書く。

Pillow が要る。無ければ何もせずに終える(壁紙が無くても系は動く)。
"""
import struct
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out, src = sys.argv[1], sys.argv[2]
    size = None
    if len(sys.argv) >= 5:
        size = (int(sys.argv[3]), int(sys.argv[4]))

    try:
        from PIL import Image
    except ImportError:
        sys.stderr.write("mkpict: Pillow が無いので %s は作らない\n" % out)
        return 0

    im = Image.open(src).convert("RGB")
    if size is not None and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    w, h = im.size

    body = bytearray()
    raw = im.tobytes()
    for i in range(0, len(raw), 3):
        body += struct.pack("<I", (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2])

    with open(out, "wb") as f:
        f.write(struct.pack("<HHHH", w, h, 32, 0))
        f.write(bytes(body))
    print("  %s -> %s (%dx%d, %d bytes)" % (src, out, w, h, 8 + len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
