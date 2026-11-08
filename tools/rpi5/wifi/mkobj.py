#!/usr/bin/env python3
"""The Wi-Fi's objects of the Raspberry Pi 5's system volume (docs/tessronos/09-network.md 9.6.8).

  mkobj.py --fw DIR --box SYSBOX_XTAD --out DIR

Writes into --out, in the form tools/mktsfs takes (--over):

  - the firmware of the CYW43455 (SYSDEF_FW_BWFM43455): a plain object
    whose record 1 is cyfmac43455-sdio.bin as linux-firmware publishes it
    (DIR, tools/rpi5/wifi/fw.sh) and whose record 0 says what it is, with
    Cypress's licence for it; owned by the system, read by everyone;
  - the regulatory data (CLM) of the chip (SYSDEF_FW_BWFM43455_CLM), the
    same way;
  - the settings of the board (NVRAM, SYSDEF_FW_BWFM43455_NV): a document
    whose record 0 has notes on where it is from and under what licence
    (paragraphs that start with NOTE), then each line of the text as one
    paragraph; record 1 is the GPL-2.0 the text is under. The system and
    the administrators may edit it; the Wi-Fi reads it as nvram.py does;
  - the Wi-Fi's settings (SYSDEF_CONF_WIFI, Ｗｉ－Ｆｉの設定), as on the
    x64 PC (tools/x64/wifi/mkobj.py): the networks saved and their
    keys, read and written only by the system and the administrators
    (rw-------); record 1 starts with "$$WIFI on";
  - the patch of the same chip's Bluetooth (SYSDEF_FW_BCM4345C0,
    BCM4345C0.hcd, design 6.8): a plain object like the firmware's,
    record 1 the file as Raspberry Pi's bluez-firmware has it, given by
    device/bt/bt_bcm.c at every power on. Only for the Raspberry Pi 5,
    whose built-in Bluetooth the system runs (--no-bt leaves it out,
    --bt puts it in for another board);
  - the system box (SYSDEF_SYSTEM_BOX) as etc/def has it, with a link to
    each of them after its others.

What is written depends only on the inputs.
"""

import argparse
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvram  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import vobjw  # noqa: E402

FW_UUID = "01a0d8c4-5484-7e95-8a06-3def40515484"
CLM_UUID = "01a0d8c4-5485-7fa6-9b17-4ef051625485"
NV_UUID = "01a0d8c4-5486-70b7-ac28-5f0162735486"
FW_LINK = "01a0d8c4-5487-71c8-bd39-601273845487"
CLM_LINK = "01a0d8c4-5488-72d9-8e4a-712384955488"
NV_LINK = "01a0d8c4-5489-73ea-9f5b-823495a65489"
CONF_UUID = "01a0d8c4-5481-7b92-9db3-0abc1d2e5481"
CONF_LINK = "01a0d8c4-5483-7d94-9fd5-2cde3f405483"
BT_UUID = "01a0d8c4-5610-7d01-8e10-01a0c3d45610"
BT_LINK = "01a0d8c4-5611-7d02-8e11-02a1c3d45611"
BT_FILE = "BCM4345C0.hcd"
BT_COMMIT = "cdf61dc691a49ff01a124752bd04194907f0f9cd"
SYSBOX = "01a0d8c4-5211-7c52-9e0a-3b61d27f4c85"
TAG = "20260810"
FW_FILE = "cyfmac43455-sdio.bin"
CLM_FILE = "cyfmac43455-sdio.clm_blob"
NV_FILE = "brcmfmac43455-sdio.raspberrypi,4-model-b.txt"
LIC_CYPRESS = "LICENCE.cypress"
LIC_GPL = "GPL-2.0"
STAMP = "2026-10-20T00:00:00Z"
SYSTEM = "01a0d680-3aa0-75b3-bd7d-075e14217cd2"
ADMINS = "01a0d680-3aa1-7922-ae79-ba5cbbaa2037"


def xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def meta(name, mode, records, licence, source, file=None, editable=False):
    m = {
        "name": name,
        "relationship": [],
        "linktype": False,
        "makeDate": STAMP,
        "updateDate": STAMP,
        "accessDate": STAMP,
        "periodDate": None,
        "refCount": 1,
        "recordCount": len(records),
        "editable": editable,
        "deletable": False,
        "readable": True,
        "maker": "TessronOS",
        "applist": {},
        "tessronos": {
            "records": [{"n": n, "rt": rt} for n, rt in records],
            "firmware": {"license": licence, "sources": [source]},
            "access": {"owner": SYSTEM, "group": ADMINS, "mode": mode,
                       "attr": ["perm"], "acl": [], "records": []},
        },
    }
    if file is not None:
        m["tessronos"]["file"] = file
    return m


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def document(name, paras):
    return ('<tad version="1.0" encoding="UTF-8" filename="%s">\n<document>\n%s</document>\n</tad>\n'
            % (xml(name), "".join("<p>%s</p>\n" % xml(x) for x in paras)))


def link(uuid, vobj, name, y):
    """A closed link as wide as its name, its top at y"""
    h = vobjw.closed_h()
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="%d" vobjtop="%d" vobjright="%d" '
            'vobjbottom="%d" height="%d" chsz="%d" frcol="#000000" chcol="#000000" '
            'tbcol="#ffffff" bgcol="#ffffff" pictdisp="true" namedisp="true" framedisp="true"/>\n'
            % (uuid, vobj, vobjw.PAD, y, vobjw.PAD + vobjw.closed_w(name), y + h, h, vobjw.CHSZ))


def links_after(box, items):
    """The links to items, (uuid, link id, name), one below another after the box's lowest"""
    bottoms = [int(x) for x in re.findall(r'vobjbottom="(\d+)"', box)]
    y = (max(bottoms) + vobjw.GAP) if bottoms else vobjw.PAD
    out = ""
    for u, v, n in items:
        out += link(u, v, n, y)
        y += vobjw.closed_h() + vobjw.GAP
    return out


def binary(out, uuid, fw_dir, fname, what, lic_paras, source=None, origin=None):
    path = os.path.join(fw_dir, fname)
    size = os.path.getsize(path)
    if source is None:
        source = "linux-firmware %s cypress/%s" % (TAG, fname)
    if origin is None:
        origin = "linux-firmwareのタグ%sの%s" % (TAG, "cypress/" + fname)
    write(os.path.join(out, uuid + ".json"),
          json.dumps(meta(fname, "r--r--r--", [(0, 1), (1, 15)], "LicenseRef-Cypress", source,
                          file={"name": fname, "mediatype": "application/octet-stream",
                                "size": size, "source": "system"}),
                     ensure_ascii=False, indent=2) + "\n")
    paras = [fname, what + "レコード1が%sそのもの(SHA-256を確かめたもの)。"
             "Cypressの次の許諾に従って、改変せずに再配布する。" % origin]
    paras += lic_paras
    write(os.path.join(out, uuid + "_0.xtad"), document(fname, paras))
    shutil.copyfile(path, os.path.join(out, uuid + "_1.bin"))
    return size


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--board", default="Raspberry Pi 5")
    p.add_argument("--fw", required=True)
    p.add_argument("--box", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--bt", dest="bt", action="store_true", default=None)
    p.add_argument("--no-bt", dest="bt", action="store_false")
    a = p.parse_args()
    if a.bt is None:
        a.bt = a.board == "Raspberry Pi 5"

    for f in (FW_FILE, CLM_FILE, NV_FILE, LIC_CYPRESS, LIC_GPL) + ((BT_FILE,) if a.bt else ()):
        if not os.path.isfile(os.path.join(a.fw, f)):
            sys.exit("rpi5-wifi-obj: %s is not in %s (tools/rpi5/wifi/fw.sh)" % (f, a.fw))
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)

    board = a.board
    # the machine's name in full-width letters, as the object's name has it
    wide = board.translate({c: c + 0xfee0 for c in range(0x21, 0x7f)}).replace(" ", "　")
    lic = open(os.path.join(a.fw, LIC_CYPRESS), encoding="utf-8").read()
    lic_paras = [" ".join(x.split()) for x in lic.split("\n\n") if x.strip()]
    fw = binary(a.out, FW_UUID, a.fw, FW_FILE,
                board + "のWi-Fiのチップ(CYW43455、SDIO)のファームウェア。Wi-Fiのドライバ"
                "(NetBSDのbwfm)がチップを起こすときに読み込む。", lic_paras)
    bt = 0
    if a.bt:
        bt = binary(a.out, BT_UUID, a.fw, BT_FILE,
                    board + "の内蔵Bluetooth(CYW43455のBluetooth、BCM4345C0、UART)のパッチ。Bluetoothの"
                    "ドライバ(device/bt/bt_bcm.c)が電源を入れるたびにコントローラへ送る。Raspberry Piはこのファイルを"
                    "linux-firmwareのLICENCE.cypressと同じ許諾で配っている(bluez-firmwareのdebian/copyright)。", lic_paras,
                    source="bluez-firmware %s debian/firmware/broadcom/%s" % (BT_COMMIT, BT_FILE),
                    origin="Raspberry Piのbluez-firmware(github.com/RPi-Distro/bluez-firmware)のコミット%sの"
                           "debian/firmware/broadcom/%s" % (BT_COMMIT, BT_FILE))
    clm = binary(a.out, CLM_UUID, a.fw, CLM_FILE,
                 board + "のWi-Fiのチップ(CYW43455)の、国と地域ごとの電波の規則(CLM)。"
                 "ファームウェアの後にチップに読み込む。", lic_paras)

    # the settings of the board: notes, then the text a line to a paragraph
    name = "Ｗｉ－Ｆｉの基板の設定（%s）" % wide
    text = open(os.path.join(a.fw, NV_FILE), encoding="ascii").read()
    notes = [
        nvram.NOTE + board + "のWi-Fiのチップ(CYW43455)の基板の設定(NVRAM)。Wi-Fiの"
        "初期化が、この注記の段落、「#」で始まる行、空の行を除いた段落を「名前=値」の行として読み、"
        "チップに渡す。全角の英数字と記号は半角として読む。",
        nvram.NOTE + "出典: linux-firmwareのタグ%sの brcm/%s(SHA-256を確かめたもの)。"
        "linux-firmwareでは、Raspberry Pi 4、Compute Module 4、Pi 5、Pi 500、Compute Module 5の名前が"
        "このファイルかその別名になっている。"
        % (TAG, NV_FILE),
        nvram.NOTE + "ライセンス: GPL-2.0以降(下の最初の行のSPDX-License-Identifier: GPL-2.0+)。"
        "著作権はRaspberry Pi (Trading) Ltd.にある。この文章そのものがこの設定のソースである。"
        "GPL-2.0の全文はこの実身のレコード1にある。",
    ]
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    write(os.path.join(a.out, NV_UUID + ".json"),
          json.dumps(meta(name, "rw-rw-r--", [(0, 1), (1, 15)], "GPL-2.0-or-later",
                          "linux-firmware %s brcm/%s" % (TAG, NV_FILE), editable=True),
                     ensure_ascii=False, indent=2) + "\n")
    doc = document(name, notes + lines)
    write(os.path.join(a.out, NV_UUID + "_0.xtad"), doc)
    shutil.copyfile(os.path.join(a.fw, LIC_GPL), os.path.join(a.out, NV_UUID + "_1.bin"))

    # what the Wi-Fi will read of it is the text without its comments and blank lines
    got = nvram.extract(doc)
    want = nvram.plain(text)
    if got != want:
        sys.exit("rpi5-wifi-obj: the settings read back from the document differ from %s" % NV_FILE)

    # the Wi-Fi's settings, as on the x64 PC
    cname = "Ｗｉ－Ｆｉの設定"
    cm = meta(cname, "rw-------", [(0, 1), (1, 15)], None, None, editable=True)
    del cm["tessronos"]["firmware"]
    write(os.path.join(a.out, CONF_UUID + ".json"), json.dumps(cm, ensure_ascii=False, indent=2) + "\n")
    write(os.path.join(a.out, CONF_UUID + "_0.xtad"),
          '<tad version="1.0" encoding="UTF-8" filename="%s">\n<document>\n<p>%s</p>\n<p>%s</p>\n</document>\n</tad>\n'
          % (xml(cname), xml(cname),
             xml("このマシンが接続するWi-Fiのネットワーク(SSID、セキュリティの種類、自動で接続するか、"
                 "保存した利用者、鍵)。ネットワーク設定の小物の〈Ｗｉ－Ｆｉ〉の依頼で、"
                 "システムのWi-Fiの管理だけが書く。鍵はパスフレーズとSSIDから作ったPMKで、パスフレーズ"
                 "そのものは持たない。鍵を含むので、システムと管理者だけが読み書きできる。レコード1が"
                 "設定の行。起動時と書き換えられたときにシステムが読む。")))
    write(os.path.join(a.out, CONF_UUID + "_1.bin"), "$$WIFI\ton\n")

    # the system box, with the four linked after the rest
    box = open(a.box, encoding="utf-8").read()
    if "</figure>" not in box:
        sys.exit("rpi5-wifi-obj: the system box %s is not laid out as expected" % a.box)
    links = links_after(box, [(FW_UUID, FW_LINK, FW_FILE), (CLM_UUID, CLM_LINK, CLM_FILE),
                              (NV_UUID, NV_LINK, name), (CONF_UUID, CONF_LINK, cname)]
                        + ([(BT_UUID, BT_LINK, BT_FILE)] if a.bt else []))
    box = box.replace("</figure>", links + "</figure>", 1)
    write(os.path.join(a.out, SYSBOX + "_0.xtad"), box)
    print("rpi5-wifi-obj: %s (firmware %d bytes, CLM %d bytes, settings %d lines, the Wi-Fi's settings, %sthe system box)"
          % (a.out, fw, clm, want.count("\n"), ("the Bluetooth's patch %d bytes, " % bt) if a.bt else ""))


if __name__ == "__main__":
    main()
