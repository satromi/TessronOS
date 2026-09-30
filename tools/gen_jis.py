#!/usr/bin/env python3
"""The character tables of マイクロスクリプト's sconv and srconv, and the
TRON code tables of lib/libbpk.

  tools/gen_jis.py > application/microscript/ms_jis.c
  tools/gen_jis.py bpk > lib/libbpk/bpk_jis.c

Every table comes from the codecs of Python, so that it can be made
again the same:

  - JIS X 0208 (row and cell 1..94): euc_jp, and for the places it has
    nothing, the Windows code page by the Shift_JIS bytes of the same
    place (the NEC special characters of row 13 and the like). This is
    マイクロスクリプト's table, for Shift_JIS and EUC text.
  - The first plane of TRON code, zone A: JIS X 0208, and in its gaps the
    first plane of JIS X 0213 (euc_jis_2004). A place may hold a
    character past the BMP or two characters (a letter and a combining
    mark); such a place holds 0xD800 + n, n indexing bpk_tron_multi.
  - Zone B, first byte 87..A0: the second plane of JIS X 0213, its rows
    1, 3-5, 8, 12-15 and 78-94 in turn (euc_jis_2004, SS3).
  - Zone B, first byte A1..ED: JIS X 0212 補助漢字, rows 1..77 (euc_jp, SS3).
  - Zone C: GB 2312, its row and cell laid 126 to a first byte (gb2312).
  - Zone D from first byte B7: KS X 1001 the same way (euc_kr).

Both tables of JIS X 0208 read back the forms of the Windows code page
as well (FF5E for 〜 and FF0D for −, and so on): the characters come out
in the JIS form (301C, 2212) and either form goes back to the same place.
"""

import sys

HEAD = ("/*\n *----------------------------------------------------------------------\n"
        " *    TessronOS\n *\n *    Copyright (C) 2026 satromi\n *    This software is distributed under the T-License 2.2.\n"
        " *----------------------------------------------------------------------\n */\n\n")

X0213_P2_ROWS = [1, 3, 4, 5, 8, 12, 13, 14, 15] + list(range(78, 95))


def dec(b, codec):
    try:
        return bytes(b).decode(codec)
    except UnicodeDecodeError:
        return None


def cp932_of(row, cell):
    """The character the Windows code page has at a row and cell"""
    j1, j2 = row + 0x20, cell + 0x20
    if j1 % 2:
        s1 = (j1 + 1) // 2 + 0x70
        s2 = j2 + 0x1F + (1 if j2 >= 0x60 else 0)
    else:
        s1 = j1 // 2 + 0x70
        s2 = j2 + 0x7E
    if s1 >= 0xA0:
        s1 += 0x40
    s = dec([s1, s2], "cp932")
    return ord(s) if s is not None and len(s) == 1 else 0


def x0208_of(row, cell):
    s = dec([row + 0xA0, cell + 0xA0], "euc_jp")
    return ord(s) if s is not None and len(s) == 1 else 0


def table(out, ctype, name, vals, fmt="0x%04X", per=12):
    size = "94 * 94" if len(vals) == 94 * 94 else "%d" % len(vals)
    out.write("const %s %s[%s] = {\n" % (ctype, name, size))
    for i in range(0, len(vals), per):
        out.write("\t" + ", ".join(fmt % v for v in vals[i:i + per]) + ",\n")
    out.write("};\n\n")


def ms_main(out):
    tbl, back = [], {}
    for row in range(1, 95):
        for cell in range(1, 95):
            u = x0208_of(row, cell) or cp932_of(row, cell)
            tbl.append(u)
            if u and u > 0x7F and u not in back:
                back[u] = (row << 8) | cell
    add_alternates(back)
    out.write(HEAD)
    out.write("/*\n *\tms_jis.c\n *\tJIS X 0208 and the characters it holds, made by tools/gen_jis.py\n */\n\n")
    out.write("#include \"ms.h\"\n\n")
    out.write("/* the character of row r and cell c (1..94) at [(r - 1) * 94 + c - 1]; 0 for none */\n")
    table(out, "UH", "ms_jis_char", tbl)
    pairs = sorted(back.items())
    out.write("/* the characters in order, each with its row and cell (row << 8 | cell) */\n")
    table(out, "UW", "ms_jis_back", ["%04X%04X" % p for p in pairs], fmt="0x%s", per=8)
    out.write("const INT ms_jis_nback = %d;\n" % len(pairs))


def add_alternates(back):
    """The Windows code page's forms of the characters of JIS X 0208"""
    for row in range(1, 95):
        for cell in range(1, 95):
            a, w = x0208_of(row, cell), cp932_of(row, cell)
            if a and w and a != w:
                back[w] = (row << 8) | cell


def bpk_main(out):
    multi = []          # (first, second) of the places that hold no single BMP character

    def cell_value(s):
        if s is None:
            return 0
        if len(s) == 1 and ord(s) <= 0xFFFF and not (0xD800 <= ord(s) <= 0xDFFF):
            return ord(s)
        pair = (ord(s[0]), ord(s[1]) if len(s) > 1 else 0)
        if pair not in multi:
            multi.append(pair)
        return 0xD800 + multi.index(pair)

    # zone A of plane 1
    za, back = [], {}
    for row in range(1, 95):
        for cell in range(1, 95):
            s = dec([row + 0xA0, cell + 0xA0], "euc_jp")
            if s is None:
                s = dec([row + 0xA0, cell + 0xA0], "euc_jis_2004")
            v = cell_value(s)
            za.append(v)
            if 0x7F < v < 0xD800 or v > 0xDFFF:
                back.setdefault(v, (row << 8) | cell)
    add_alternates(back)

    # zone B: JIS X 0213's second plane at 87..A0, JIS X 0212 at A1..ED
    zb = []
    for hi in range(0x87, 0xEE):
        for cell in range(1, 95):
            if hi <= 0xA0:
                s = dec([0x8F, X0213_P2_ROWS[hi - 0x87] + 0xA0, cell + 0xA0], "euc_jis_2004")
            else:
                s = dec([0x8F, hi, cell + 0xA0], "euc_jp")
            zb.append(cell_value(s))

    # zones C and D: 94 by 94 laid 126 to a first byte
    def packed(codec):
        v = []
        for p in range(94 * 94):
            s = dec([0xA1 + p // 94, 0xA1 + p % 94], codec)
            v.append(cell_value(s) if s is not None and len(s) == 1 else 0)
        return v
    zc = packed("gb2312")
    zd = packed("euc_kr")

    # the way back for what zone A does not hold: the Japanese sets of zone B
    rev = {}
    for code_hi in list(range(0xA1, 0xEE)) + list(range(0x87, 0xA1)):
        for cell in range(1, 95):
            v = zb[(code_hi - 0x87) * 94 + cell - 1]
            if v == 0:
                continue
            if 0xD800 <= v <= 0xDFFF:
                first, second = multi[v - 0xD800]
                if second != 0:
                    continue
                v = first
            if v in back or v in rev:
                continue
            rev[v] = (code_hi << 8) | (cell + 0x20)
    # and zone A's places past the BMP
    for i, v in enumerate(za):
        if 0xD800 <= v <= 0xDFFF:
            first, second = multi[v - 0xD800]
            if second == 0 and first not in rev:
                rev[first] = ((i // 94 + 0x21) << 8) | (i % 94 + 0x21)

    # the two-character places, each with its code, for the way back
    seqs = []
    for i, v in enumerate(za):
        if 0xD800 <= v <= 0xDFFF and multi[v - 0xD800][1] != 0:
            seqs.append((multi[v - 0xD800], ((i // 94 + 0x21) << 8) | (i % 94 + 0x21)))

    out.write(HEAD)
    out.write("/*\n *\tbpk_jis.c\n *\tThe character sets of TRON code's first plane, made by tools/gen_jis.py bpk\n */\n\n")
    out.write("#include <ts/bpk.h>\n\n")
    out.write("/*\n * Zone A: JIS X 0208 and, in its gaps, JIS X 0213's first plane; the\n"
              " * character of row r and cell c (1..94) at [(r - 1) * 94 + c - 1], 0 for\n"
              " * none, 0xD800 + n for bpk_tron_multi[n]\n */\n")
    table(out, "UH", "bpk_jis_char", za)
    pairs = sorted(back.items())
    out.write("/* zone A's characters of the BMP in order, each with its row and cell (row << 8 | cell) */\n")
    table(out, "UINT", "bpk_jis_rev", ["%04X%04X" % p for p in pairs], fmt="0x%s", per=8)
    out.write("const INT bpk_jis_nrev = %d;\n\n" % len(pairs))
    out.write("/* zone B, first byte 87..ED, 94 cells each: JIS X 0213's second plane, then JIS X 0212 */\n")
    table(out, "UH", "bpk_tron_zb", zb)
    out.write("/* zone C: GB 2312, at (first - 21) * 126 + second - 80 */\n")
    table(out, "UH", "bpk_tron_zc", zc)
    out.write("/* zone D: KS X 1001, at (first - B7) * 126 + second - 80 */\n")
    table(out, "UH", "bpk_tron_zd", zd)
    out.write("/* the places that hold a character past the BMP (second 0) or two */\n")
    out.write("const UINT bpk_tron_multi[%d][2] = {\n" % len(multi))
    for i in range(0, len(multi), 6):
        out.write("\t" + " ".join("{ 0x%05X, 0x%04X }," % m for m in multi[i:i + 6]) + "\n")
    out.write("};\n\n")
    out.write("const INT bpk_tron_nmulti = %d;\n\n" % len(multi))
    rp = sorted(rev.items())
    out.write("/* what zone A does not hold, in order: the character and its code in plane 1 */\n")
    table(out, "UINT", "bpk_tron_rev_cp", [p[0] for p in rp], fmt="0x%05X", per=10)
    table(out, "UH", "bpk_tron_rev_code", [p[1] for p in rp])
    out.write("const INT bpk_tron_nrev = %d;\n\n" % len(rp))
    out.write("/* the two-character places of zone A: the letter, the mark and the code */\n")
    out.write("const UINT bpk_tron_seq[%d][3] = {\n" % len(seqs))
    for (a, b), c in seqs:
        out.write("\t{ 0x%04X, 0x%04X, 0x%04X },\n" % (a, b, c))
    out.write("};\n\n")
    out.write("const INT bpk_tron_nseq = %d;\n" % len(seqs))


def main():
    # LF line ends whatever system writes it
    out = open(sys.stdout.fileno(), 'w', encoding='utf-8', newline='\n', closefd=False)
    if len(sys.argv) > 1 and sys.argv[1] == "bpk":
        bpk_main(out)
    else:
        ms_main(out)


if __name__ == "__main__":
    main()
