#!/usr/bin/env python3
"""The tables of lib/libtxc: JIS X 0208 with the Windows extensions.

  tools/gen_txc.py > lib/libtxc/txc_tab.c

A double byte of Shift_JIS, a pair of EUC-JP and a pair of ISO-2022-JP
all name one place of the same grid: rows of 94 cells, a pointer being
(row - 1) * 94 + (cell - 1). Rows 1 to 94 are JIS X 0208 with the NEC
special characters of row 13 and the NEC selected IBM extensions of rows
89 to 92; rows 95 to 114 are the user's own (the private use area, made
by arithmetic and not kept here); rows 115 to 120 are the IBM
extensions, reached only from Shift_JIS (lead bytes 0xFA to 0xFC).

Each place's character is what the Windows code page 932 decoder of
Python gives for the Shift_JIS bytes of that place. The way back is
every (character, pointer) pair ordered by character and then pointer,
so that an encoder finds all the places a character has and chooses.
"""

import sys


def sjis_of(ptr):
    lead = ptr // 188
    trail = ptr % 188
    b1 = lead + (0x81 if lead < 0x1F else 0xC1)
    b2 = trail + (0x40 if trail < 0x3F else 0x41)
    return bytes([b1, b2])


def char_of(ptr):
    try:
        s = sjis_of(ptr).decode("cp932")
    except UnicodeDecodeError:
        return 0
    if len(s) != 1:
        return 0
    return ord(s)


MAIN = 94 * 94                  # rows 1 to 94
IBM_FROM = 114 * 94             # row 115
IBM_TO = 120 * 94               # past row 120


def main():
    main_tbl = [char_of(p) for p in range(MAIN)]
    ibm_tbl = [char_of(p) for p in range(IBM_FROM, IBM_TO)]
    pairs = []
    for p, u in enumerate(main_tbl):
        if u > 0x7F:
            pairs.append((u, p))
    for i, u in enumerate(ibm_tbl):
        if u > 0x7F:
            pairs.append((u, IBM_FROM + i))
    pairs.sort()

    out = open(sys.stdout.fileno(), "w", encoding="utf-8", newline="\n", closefd=False)
    out.write("/*\n *----------------------------------------------------------------------\n"
              " *    TessronOS\n *\n *    Copyright (C) 2026 satromi\n *    This software is distributed under the T-License 2.2.\n"
              " *----------------------------------------------------------------------\n */\n\n")
    out.write("/*\n *\ttxc_tab.c\n *\tJIS X 0208 with the Windows extensions, "
              "made by tools/gen_txc.py\n */\n\n")
    out.write("#include <tk/typedef.h>\n#include \"txc_tab.h\"\n\n")
    out.write("/* the character at pointer (row - 1) * 94 + (cell - 1), rows 1 to 94; 0 for none */\n")
    out.write("EXPORT CONST UH txc_main[TXC_MAIN_N] = {\n")
    for i in range(0, len(main_tbl), 12):
        out.write("\t" + ", ".join("0x%04X" % u for u in main_tbl[i:i + 12]) + ",\n")
    out.write("};\n\n")
    out.write("/* the same for rows 115 to 120, from pointer TXC_IBM_FROM */\n")
    out.write("EXPORT CONST UH txc_ibm[TXC_IBM_N] = {\n")
    for i in range(0, len(ibm_tbl), 12):
        out.write("\t" + ", ".join("0x%04X" % u for u in ibm_tbl[i:i + 12]) + ",\n")
    out.write("};\n\n")
    out.write("/* every (character, pointer) pair, by character and then pointer */\n")
    out.write("EXPORT CONST UH txc_back[TXC_BACK_N][2] = {\n")
    for i in range(0, len(pairs), 6):
        out.write("\t" + ", ".join("{ 0x%04X, %5d }" % (u, p) for u, p in pairs[i:i + 6]) + ",\n")
    out.write("};\n")

    # the sizes lib/libtxc/txc_tab.h declares
    sys.stderr.write("main %d, ibm %d, back %d\n" % (MAIN, IBM_TO - IBM_FROM, len(pairs)))


if __name__ == "__main__":
    main()
