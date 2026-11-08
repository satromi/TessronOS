#!/usr/bin/env python3
"""The マイクロカード objects of etc/xtad, written as the program keeps them.

  python tools/mkmcard.py [ETC_XTAD]

A マイクロカード実身 keeps its database in the records of type 31 that
マイクロカード実身データ構造説明書 (1999/05/20) describes, every word
little-endian (design 11.18):

  subtype 1  the format: TC id ＣＲＤ, UH version 0x100, UH ncards, UH
             datalen, UH reserved[9], UH nitems, and for each item TC
             name[8], UH offset, UH kind (1 number, 2 string, 3 link),
             UH nchars, UH tchars
  subtype 2  the cards: UH 0xA5B6, UH datalen, then each item -- a DOUBLE,
             TC string[nchars + 1], or UH vobjnumber and 23 words
  subtype 4  the links: UH vobjnumber, UH linknumber, UH attr[5] and a
             仮身セグメント (RECT, height, CHSIZE, four COLORs, dlen)

Record 0 is the program's xmlTAD: TessronOS's settings in <microcard>,
what the object holds, and after <mclinks/> one <link> for each object
the links point at, in the order of their link numbers.

This writes the template カード用紙 (no fields, one card, the first record
of each kind it needs), and the sample 住所録 with its layout cabinet and
two layouts (a figure of labels and a text of a letter). The letters are
TRON characters of plane 1 as the system writes them: ASCII in row 3, the
rest by JIS X 0208.
"""

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vobjw  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XTAD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "etc", "xtad")
ICON = "019a0f91-066f-706f-93da-ffe8150d6207.ico"   # 画用紙's

TEMPLATE = "01a0d8c4-5334-7e95-8a06-b1c2d3e4f534"
SAMPLE = "01a0d8c4-5336-7a17-8c28-d3e4f5a6b736"
CABINET = "01a0d8c4-5338-7c39-8e4a-f5a6b7c8d938"
LABEL = "01a0d8c4-5339-7d4a-9f5b-a6b7c8d9ea39"
LETTER = "01a0d8c4-533a-7e5b-8a6c-b7c8d9eafb3a"

K_NUM, K_STR, K_LINK = 1, 2, 3


def tron(text):
    """A text as TRON characters of plane 1."""
    out = []
    for ch in text:
        c = ord(ch)
        if 0x20 <= c <= 0x7E:
            out.append(0x2300 | c)
            continue
        b = ch.encode("euc_jis_2004")
        if len(b) != 2 or b[0] < 0xA1 or b[1] < 0xA1:
            raise SystemExit("not a letter of JIS X 0208: %r" % ch)
        out.append(((b[0] & 0x7F) << 8) | (b[1] & 0x7F))
    return out


def tc_string(text, words):
    t = tron(text)[:words - 1]
    t += [0] * (words - len(t))
    return struct.pack("<%dH" % words, *t)


def item_size(kind, nchars):
    return 8 if kind == K_NUM else 48 if kind == K_LINK else 2 * (nchars + 1)


def format_rec(fields, ncards):
    datalen = 4 + sum(item_size(k, n) for _, k, n, _ in fields)
    b = struct.pack("<3H", 0x2343, 0x2352, 0x2344) + struct.pack("<HHH", 0x100, ncards, datalen)
    b += b"\0" * 18 + struct.pack("<H", len(fields))
    off = 4
    for name, kind, nchars, width in fields:
        t = tron(name)[:8]
        b += struct.pack("<8H", *(t + [0] * (8 - len(t)))) + struct.pack("<4H", off, kind, nchars, width)
        off += item_size(kind, nchars)
    return b, datalen


def cards_rec(fields, cards, datalen):
    b = b""
    vno = 5
    for card in cards:
        c = struct.pack("<HH", 0xA5B6, datalen)
        for (name, kind, nchars, _), v in zip(fields, card):
            if kind == K_NUM:
                c += struct.pack("<d", float(v)) if v != "" else struct.pack("<Q", 0x7FF8000000000000)
            elif kind == K_LINK:
                c += struct.pack("<H", 0) + b"\0" * 46
            else:
                c += tc_string(v, nchars + 1)
        assert len(c) == datalen
        b += c
    return b


def vdata(vobjno, linkno, left, top, right, bottom):
    attr = 0x0002 | 0x0004 | 0x0008        # no relation, type or time shown
    b = struct.pack("<HHH", vobjno, linkno, attr) + b"\0" * 8
    b += struct.pack("<4hhH", left, top, right, bottom, bottom - top, 0x8000 | (14 * 20))
    b += struct.pack("<4I", 0x10000000, 0x10000000, 0x10FFFFFF, 0x10FFFFFF) + struct.pack("<H", 0)
    return b


def closed_box(name, left=0, top=0):
    """Where a closed link as wide as its name stands, its corner at left and top"""
    return (left, top, left + vobjw.closed_w(name), top + vobjw.closed_h())


def link_el(target, vobjid, box):
    l, t, r, b = box
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="%d" vobjtop="%d" vobjright="%d" vobjbottom="%d" '
            'height="%d" chsz="14" frcol="#000000" chcol="#000000" tbcol="#ffffff" bgcol="#ffffff" dlen="0" '
            'pictdisp="true" namedisp="true" framedisp="true"/>' % (target, vobjid, l, t, r, b, b - t))


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def record0(name, fields, ncards, links=(), faces=None, prints=()):
    x = ['<tad version="1.0" encoding="UTF-8" filename="%s">' % esc(name), "<document>",
         '<microcard version="1" cardsize="10" cardprop="true" cardgray="false" listsize="10" '
         'listprop="true" listgray="false" names="true">']
    for k, (fname, kind, _, _) in enumerate(fields):
        if kind == K_STR:
            x.append('<field n="%d" face="%s"/>' % (k, (faces or {}).get(fname, "mincho")))
    for p in prints:
        x.append(p)
    x.append("</microcard>")
    x.append("<p>マイクロカード「%s」　カード%d枚、項目%d個</p>" % (esc(name), ncards, len(fields)))
    x.append("<p>項目：%s</p>" % "、".join(esc(f[0]) for f in fields))
    if links:
        x.append("<p><mclinks/>" + "".join(links) + "</p>")
    x += ["</document>", "</tad>", ""]
    return "\n".join(x)


def meta(name, applist, records, window=None, maker="TessronOS"):
    m = {"name": name, "relationship": [], "linktype": False,
         "makeDate": "2026-10-03T00:00:00Z", "updateDate": "2026-10-03T00:00:00Z",
         "accessDate": "2026-10-03T00:00:00Z", "periodDate": None, "refCount": 1,
         "recordCount": len(records), "editable": True, "deletable": True, "readable": True,
         "maker": maker}
    if window:
        m["window"] = window
    m["applist"] = applist
    m["records"] = records
    return m


def put(uuid, m, recs, icon=True):
    """recs: [(rt, sub, bytes or text)] in record order"""
    for n, (rt, sub, data) in enumerate(recs):
        if rt == 1:
            open(os.path.join(XTAD, "%s_%d.xtad" % (uuid, n)), "w", encoding="utf-8", newline="\n").write(data)
        else:
            open(os.path.join(XTAD, "%s_%d.bin" % (uuid, n)), "wb").write(data)
    with open(os.path.join(XTAD, uuid + ".json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, ensure_ascii=False, indent=2)
        f.write("\n")
    if icon:
        with open(os.path.join(XTAD, ICON), "rb") as s, open(os.path.join(XTAD, uuid + ".ico"), "wb") as d:
            d.write(s.read())


MCARD_APPS = {"tessronos-microcard": {"name": "マイクロカード", "defaultOpen": True},
              "basic-text-editor": {"name": "基本文章編集", "defaultOpen": False}}
WINDOW = {"pos": {"x": 120, "y": 90}, "width": 560, "height": 440, "minWidth": 360, "minHeight": 240,
          "resizable": True, "scrollable": True, "backgroundColor": "#ffffff"}


def database(uuid, name, fields, cards, links=(), vd=b"", faces=None, prints=(), icon=True):
    fmt, datalen = format_rec(fields, len(cards))
    recs = [(1, 0, record0(name, fields, len(cards), links, faces, prints)), (31, 1, fmt)]
    if cards:
        recs.append((31, 2, cards_rec(fields, cards, datalen)))
    if vd:
        recs.append((31, 4, vd))
    records = [{"rid": n + 1, "rt": rt, "sub": sub} for n, (rt, sub, _) in enumerate(recs)]
    put(uuid, meta(name, MCARD_APPS, records, WINDOW, "TRON User"), recs, icon)


def main():
    # カード用紙: no fields, one empty card
    database(TEMPLATE, "カード用紙", [], [[]])

    fields = [("氏名", K_STR, 20, 8), ("よみ", K_STR, 20, 10), ("郵便番号", K_STR, 8, 8),
              ("住所", K_STR, 60, 20), ("電話", K_STR, 15, 12), ("会員番号", K_NUM, 6, 6),
              ("入会日", K_STR, 10, 10), ("写真", K_LINK, 20, 0)]
    cards = [
        ["岡田　勇作", "おかだ　ゆうさく", "146-0082", "東京都大田区池上9-5-12", "03-3199-1110", "4", "1995/04/01", ""],
        ["鈴木　亨", "すずき　とおる", "160-0005", "東京都新宿区愛住町5-1-30", "03-3355-2201", "1034", "1998/10/15", ""],
        ["高橋　良子", "たかはし　りょうこ", "231-0023", "神奈川県横浜市中区山下町1-2", "045-201-3300", "1205", "2001/06/20", ""],
        ["齋藤　健一", "さいとう　けんいち", "330-0063", "埼玉県さいたま市浦和区高砂3-1", "048-822-4410", "1311", "2003/01/09", ""],
        ["渡邊　真理", "わたなべ　まり", "260-0013", "千葉県千葉市中央区中央4-5-1", "043-224-5520", "1420", "2005/08/30", ""],
        ["小林　大輔", "こばやし　だいすけ", "146-0082", "東京都大田区池上2-3-4", "03-3752-6630", "1502", "2008/02/14", ""],
        ["小川　さくら", "おがわ　さくら", "060-0001", "北海道札幌市中央区北一条西2", "011-231-7740", "1617", "2010/11/03", ""],
        ["伊藤　浩", "いとう　ひろし", "460-0008", "愛知県名古屋市中区栄3-6-1", "052-261-8850", "1733", "2012/07/07", ""],
        ["斎藤　美咲", "さいとう　みさき", "530-0001", "大阪府大阪市北区梅田1-1-3", "06-6345-9960", "1840", "2015/03/25", ""],
        ["近藤　誠", "こんどう　まこと", "810-0001", "福岡県福岡市中央区天神2-8-1", "092-711-1070", "1952", "2018/09/12", ""],
        ["高橋　一郎", "たかはし　いちろう", "980-0811", "宮城県仙台市青葉区一番町3-7", "022-225-1180", "2001", "2020/12/01", ""],
        ["タナカ　ミホ", "たなか　みほ", "700-0901", "岡山県岡山市北区本町6-36", "086-231-1290", "2108", "2023/05/18", ""],
    ]
    prints = ['<print layout="宛名ラベル" across="2" down="5" gapx="900" gapy="500" order="across" paper="a4" '
              'turn="false" top="15" bottom="15" left="15" right="15" head="" foot=""/>',
              '<print layout="案内状" across="1" down="1" gapx="0" gapy="0" order="across" paper="a4" '
              'turn="false" top="25" bottom="25" left="25" right="25" head="" foot="- %P / %T -"/>']
    database(SAMPLE, "住所録", fields, cards,
             links=[link_el(CABINET, "01a0d8c4-5338-7d40-8f51-a6b7c8d9e040", closed_box("住所録の印刷書式"))],
             vd=vdata(1, 0, *closed_box("住所録の印刷書式")), faces={"氏名": "gothic"}, prints=prints)

    # the layout cabinet: a figure of the layouts' links
    cab = ['<tad version="1.0" encoding="UTF-8" filename="住所録の印刷書式">', "<figure>",
           link_el(LABEL, "01a0d8c4-5339-7e51-8a62-b7c8d9eaf151",
                   closed_box("宛名ラベル", vobjw.PAD, vobjw.PAD)),
           link_el(LETTER, "01a0d8c4-533a-7f62-9b73-c8d9eafb0262",
                   closed_box("案内状", vobjw.PAD, vobjw.PAD + vobjw.closed_h() + vobjw.GAP)),
           "</figure>", "</tad>", ""]
    put(CABINET, meta("住所録の印刷書式", {"virtual-object-list": {"name": "仮身一覧", "defaultOpen": True}},
                      [{"rid": 1, "rt": 1, "sub": 0}], None, "TRON User"), [(1, 0, "\n".join(cab))], False)

    def seg(l, t, r, b, size, text, bg=False):
        return ('<document%s>\n<docView viewleft="%d" viewtop="%d" viewright="%d" viewbottom="%d"/>\n'
                '<docDraw drawleft="%d" drawtop="%d" drawright="%d" drawbottom="%d"/>\n'
                '<docScale hunit="-72" vunit="-72"/>\n<text lang="0" bpat="0"/>\n<font size="%d"/>\n'
                '<font color="#000000"/>\n%s\n</document>' % (' background="true"' if bg else "",
                                                             l, t, r, b, l, t, r, b, size, text))
    label = ['<tad version="1.0" encoding="UTF-8" filename="宛名ラベル">', "<figure>",
             seg(0, 0, 640, 24, 14, "住所録　宛名ラベル　%y年%m月%d日", True),
             seg(10, 40, 300, 60, 13, "〒%(郵便番号)"),
             seg(10, 64, 330, 104, 13, "%(住所)"),
             seg(30, 110, 330, 140, 18, "%(氏名)　様"),
             seg(220, 150, 330, 170, 11, "No.%-6C"),
             "</figure>", "</tad>", ""]
    put(LABEL, meta("宛名ラベル", {"basic-figure-editor": {"name": "基本図形編集", "defaultOpen": True}},
                    [{"rid": 1, "rt": 1, "sub": 0}], None, "TRON User"), [(1, 0, "\n".join(label))], False)
    letter = ['<tad version="1.0" encoding="UTF-8" filename="案内状">', "<document>",
              "<p>〒%(郵便番号)</p>", "<p>%(住所)</p>", "<p>%(氏名)　様</p>", "<p></p>",
              "<p>拝啓　会員番号%(会員番号)番の%(氏名)様に、例会のご案内を申し上げます。</p>",
              "<p>入会日（%(入会日)）以来のご協力に感謝いたします。</p>", "<p></p>",
              "<p>%y年%M月%D日　%N係</p>", "</document>", "</tad>", ""]
    put(LETTER, meta("案内状", {"basic-text-editor": {"name": "基本文章編集", "defaultOpen": True}},
                     [{"rid": 1, "rt": 1, "sub": 0}], None, "TRON User"), [(1, 0, "\n".join(letter))], False)


if __name__ == "__main__":
    main()
