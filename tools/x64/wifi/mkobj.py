#!/usr/bin/env python3
"""The Wi-Fi's objects of the x64 system volume (docs/tessronos/09-network.md 9.6).

  mkobj.py --fw DIR --box SYSBOX_XTAD --out DIR

Writes into --out, in the form tools/mktsfs takes (--over):

  - the firmware of Intel's 8265 (SYSDEF_FW_IWM8265_36) and of the Wi-Fi
    6 AX200 (SYSDEF_FW_IWXCC_77): each a plain object whose record 1 is
    the file (iwlwifi-8265-36.ucode, iwlwifi-cc-a0-77.ucode) as Intel
    publishes it (DIR, tools/x64/wifi/fw.sh), the version OpenBSD's iwm
    or iwx asks for (design 9.6.9), and whose record 0 says what it is
    with Intel's licence for it; owned by the system, read by everyone;
  - the Wi-Fi's settings (SYSDEF_CONF_WIFI, Ｗｉ－Ｆｉの設定): the
    networks saved and their keys (rw-------: the system's alone, and
    the administrators', who may do anything); record 1 starts with
    "$$WIFI on";
  - the system box (SYSDEF_SYSTEM_BOX) as etc/def has it, with a link to
    each of them after its others, so that none stands alone on the
    volume.

The objects of the x64 build only; the Arm machines' volumes do not
have them. What is written depends only on the inputs.
"""

import argparse
import json
import os
import re
import shutil
import sys

CONF_UUID = "01a0d8c4-5481-7b92-9db3-0abc1d2e5481"
CONF_LINK = "01a0d8c4-5483-7d94-9fd5-2cde3f405483"
SYSBOX = "01a0d8c4-5211-7c52-9e0a-3b61d27f4c85"
# the version OpenBSD's iwm asks for (API 36, 09-network.md 9.6.9)
FW_UUID = "01a0d8c4-548a-7fab-8c2d-5e6f7081548a"
FW_LINK = "01a0d8c4-548b-70bc-9d3e-6f708192548b"
FW_FILE = "iwlwifi-8265-36.ucode"
# the AX200's, the version OpenBSD's iwx asks for (API 77, 09-network.md 9.6.9)
FWX_UUID = "01a0d8c4-548c-71cd-ae4f-708192a3548c"
FWX_LINK = "01a0d8c4-548d-72de-bf50-8192a3b4548d"
FWX_FILE = "iwlwifi-cc-a0-77.ucode"
LICENCE = "LICENCE.iwlwifi_firmware"
STAMP = "2026-10-19T00:00:00Z"
SYSTEM = "01a0d680-3aa0-75b3-bd7d-075e14217cd2"
ADMINS = "01a0d680-3aa1-7922-ae79-ba5cbbaa2037"


def xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def meta(name, mode, size=None, editable=False, file=FW_FILE):
    m = {
        "name": name,
        "relationship": [],
        "linktype": False,
        "makeDate": STAMP,
        "updateDate": STAMP,
        "accessDate": STAMP,
        "periodDate": None,
        "refCount": 1,
        "recordCount": 2,
        "editable": editable,
        "deletable": False,
        "readable": True,
        "maker": "TessronOS",
        "applist": {},
        "tessronos": {
            "records": [{"n": 0, "rt": 1}, {"n": 1, "rt": 15}],
            "access": {"owner": SYSTEM, "group": ADMINS, "mode": mode,
                       "attr": ["perm"], "acl": [], "records": []},
        },
    }
    if size is not None:
        m["tessronos"]["file"] = {"name": file, "mediatype": "application/octet-stream",
                                  "size": size, "source": "system"}
    return m


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def link(uuid, vobj, y):
    return ('<link id="%s_0.xtad" vobjid="%s" vobjleft="8" vobjtop="%d" vobjright="248" '
            'vobjbottom="%d" height="25" chsz="14" frcol="#000000" chcol="#000000" '
            'tbcol="#ffffff" bgcol="#ffffff" pictdisp="true" namedisp="true" framedisp="true"/>\n'
            % (uuid, vobj, y, y + 25))


def write_fw(out, uuid, name, path, lic, what):
    """A firmware file as a plain object: record 1 the file, record 0 what it is and Intel's licence"""
    write(os.path.join(out, uuid + ".json"),
          json.dumps(meta(name, "r--r--r--", os.path.getsize(path), file=name),
                     ensure_ascii=False, indent=2) + "\n")
    paras = [name, what]
    paras += [l for l in open(lic, encoding="utf-8").read().split("\n\n") if l.strip()]
    write(os.path.join(out, uuid + "_0.xtad"),
          '<tad version="1.0" encoding="UTF-8" filename="%s">\n<document>\n%s</document>\n</tad>\n'
          % (xml(name), "".join("<p>%s</p>\n" % xml(" ".join(x.split())) for x in paras)))
    shutil.copyfile(path, os.path.join(out, uuid + "_1.bin"))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--fw", required=True)
    p.add_argument("--box", required=True)
    p.add_argument("--out", required=True)
    a = p.parse_args()

    fw = os.path.join(a.fw, FW_FILE)
    fwx = os.path.join(a.fw, FWX_FILE)
    lic = os.path.join(a.fw, LICENCE)
    if not os.path.isfile(fw) or not os.path.isfile(fwx) or not os.path.isfile(lic):
        sys.exit("wifi-obj: %s, %s and %s are not in %s (tools/x64/wifi/fw.sh)"
                 % (FW_FILE, FWX_FILE, LICENCE, a.fw))
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)

    # the firmware
    write_fw(a.out, FW_UUID, FW_FILE, fw, lic,
             "IntelのDual Band Wireless-AC 8265(PCI 8086:24FD)のファームウェア(APIのバージョン36)。"
             "Wi-Fiのドライバ(OpenBSDのiwm)がカードを動かすときに読む。レコード1がIntelの配布する"
             "ファイルそのもの(linux-firmwareのタグ20230117から取得し、SHA-256を確かめたもの。OpenBSD 8.0のiwm-firmware-20240410のiwm-8265-36と同じバイト列)。"
             "Intelの次の許諾に従って再配布する。")
    write_fw(a.out, FWX_UUID, FWX_FILE, fwx, lic,
             "IntelのWi-Fi 6 AX200(PCI 8086:2723。Surface Go 2のWi-Fi)のファームウェア(APIのバージョン77)。"
             "Wi-Fiのドライバ(OpenBSDのiwx)がカードを動かすときに読む。レコード1がIntelの配布する"
             "ファイルそのもの(linux-firmwareのタグ20251021のintel/iwlwifi/から取得し、SHA-256を確かめたもの。"
             "OpenBSD 8.0のiwx-firmware-20251021p0のiwx-cc-a0-77と同じバイト列)。"
             "Intelの次の許諾に従って再配布する。")

    # the settings
    name = "Ｗｉ－Ｆｉの設定"
    write(os.path.join(a.out, CONF_UUID + ".json"),
          json.dumps(meta(name, "rw-------", editable=True), ensure_ascii=False, indent=2) + "\n")
    write(os.path.join(a.out, CONF_UUID + "_0.xtad"),
          '<tad version="1.0" encoding="UTF-8" filename="%s">\n<document>\n<p>%s</p>\n<p>%s</p>\n</document>\n</tad>\n'
          % (xml(name), xml(name),
             xml("このマシンが接続するWi-Fiのネットワーク(SSID、セキュリティの種類、自動で接続するか、"
                 "保存した利用者、鍵)。ネットワーク設定の小物の〈Ｗｉ－Ｆｉ〉の依頼で、"
                 "システムのWi-Fiの管理だけが書く。鍵はパスフレーズとSSIDから作ったPMKで、パスフレーズ"
                 "そのものは持たない。鍵を含むので、システムと管理者だけが読み書きできる。レコード1が"
                 "設定の行。起動時と書き換えられたときにシステムが読む。")))
    write(os.path.join(a.out, CONF_UUID + "_1.bin"), "$$WIFI\ton\n")

    # the system box, with the two linked after the rest
    box = open(a.box, encoding="utf-8").read()
    tops = [int(x) for x in re.findall(r'vobjtop="(\d+)"', box)]
    y = (max(tops) + 32) if tops else 8
    if "</figure>" not in box:
        sys.exit("wifi-obj: the system box %s is not laid out as expected" % a.box)
    links = (link(FW_UUID, FW_LINK, y) + link(CONF_UUID, CONF_LINK, y + 32)
             + link(FWX_UUID, FWX_LINK, y + 64))
    box = box.replace("</figure>", links + "</figure>", 1)
    write(os.path.join(a.out, SYSBOX + "_0.xtad"), box)
    print("wifi-obj: %s (firmware %d and %d bytes, settings, the system box)"
          % (a.out, os.path.getsize(fw), os.path.getsize(fwx)))


if __name__ == "__main__":
    main()
