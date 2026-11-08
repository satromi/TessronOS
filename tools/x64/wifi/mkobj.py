#!/usr/bin/env python3
"""The Wi-Fi's objects of the x64 system volume (docs/tessronos/09-network.md 9.6).

  mkobj.py --fw DIR --box SYSBOX_XTAD --out DIR

Writes into --out, in the form tools/mktsfs takes (--over):

  - the firmware of Intel's 8265 (SYSDEF_FW_IWM8265_36), of the Wi-Fi 6
    AX200 (SYSDEF_FW_IWXCC_77), and of the other cards of iwm and iwx and
    MediaTek's MT7921 and MT7922 (MORE below): each a plain object whose record 1 is the
    file as its maker publishes it (DIR, tools/x64/wifi/fw.sh), the
    version OpenBSD's iwm, iwx or mwx asks for (design 9.6.9), and whose
    record 0 says what it is with the maker's licence for it; owned by
    the system, read by everyone;
  - the firmware and the boards' data of Qualcomm's QCA6174 (ATHX below,
    SYSDEF_FW_ATHX6174_*), which TessronOS's athx reads: the same kind of
    object, under a name that says the chip, whose record 0 has Qualcomm's
    licence and the notices of what the firmware contains;
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import vobjw  # noqa: E402

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
LICENCE_MT = "LICENCE.mediatek"
# The other cards' (include/ts/sysdef.h SYSDEF_FW_*): the object, the link to it, the file, what it is for
IWX_WHAT = ("Wi-Fiのドライバ(OpenBSDのiwx)がカードを動かすときに読む。レコード1がIntelの配布するファイルそのもの"
            "(linux-firmwareのタグ20251021のintel/iwlwifi/から取得し、SHA-256を確かめたもの。"
            "OpenBSD 8.0のiwx-firmware-20251021p0と同じバイト列)。Intelの次の許諾に従って再配布する。")
IWM_WHAT = ("Wi-Fiのドライバ(OpenBSDのiwm)がカードを動かすときに読む。レコード1がIntelの配布するファイルそのもの"
            "(linux-firmwareのタグ%s。SHA-256を確かめたもの。"
            "OpenBSD 8.0のiwm-firmware-20240410と同じバイト列)。Intelの次の許諾に従って再配布する。")
MWX_WHAT = ("Wi-Fiのドライバ(OpenBSDのmwx)がカードを動かすときに読む。レコード1がMediaTekの配布するファイルそのもの"
            "(linux-firmwareのタグ20260309のmediatek/から取得し、SHA-256を確かめたもの。"
            "OpenBSD 8.0のmwx-firmware-20260309p0と同じバイト列)。MediaTekの次の許諾に従って再配布する。")
BT_WHAT = ("。Bluetoothのドライバ(device/bt/bt_intel.c)が起動のたびにコントローラへ送る。レコード1がIntelの配布するファイルそのもの"
           "(linux-firmwareのタグ20251021のintel/から取得し、SHA-256を確かめたもの)。Intelの次の許諾に従って再配布する。")
LICENCE_IBT = "LICENCE.ibt_firmware"
MORE = [
    ("01a0d8c4-54f0-7b01-8c10-01f0a2b354f0", "01a0d8c4-54f1-7b02-8c11-02f1a2b354f1", "ibt-12-16.sfi", LICENCE_IBT,
     "Intelの8265のBluetoothのファームウェア" + BT_WHAT),
    ("01a0d8c4-54f2-7b03-8c12-03f2a2b354f2", "01a0d8c4-54f3-7b04-8c13-04f3a2b354f3", "ibt-12-16.ddc", LICENCE_IBT,
     "Intelの8265のBluetoothの設定" + BT_WHAT),
    ("01a0d8c4-54f4-7b05-8c14-05f4a2b354f4", "01a0d8c4-54f5-7b06-8c15-06f5a2b354f5", "ibt-20-0-3.sfi", LICENCE_IBT,
     "IntelのAX200のBluetoothのファームウェア" + BT_WHAT),
    ("01a0d8c4-54f6-7b07-8c16-07f6a2b354f6", "01a0d8c4-54f7-7b08-8c17-08f7a2b354f7", "ibt-20-0-3.ddc", LICENCE_IBT,
     "IntelのAX200のBluetoothの設定" + BT_WHAT),
    ("01a0d8c4-54f8-7b09-8c18-09f8a2b354f8", "01a0d8c4-54f9-7b0a-8c19-0af9a2b354f9", "ibt-20-1-3.sfi", LICENCE_IBT,
     "IntelのAX200のBluetoothのファームウェア" + BT_WHAT),
    ("01a0d8c4-54fa-7b0b-8c1a-0bfaa2b354fa", "01a0d8c4-54fb-7b0c-8c1b-0cfba2b354fb", "ibt-20-1-3.ddc", LICENCE_IBT,
     "IntelのAX200のBluetoothの設定" + BT_WHAT),
    ("01a0d8c4-54fc-7b0d-8c1c-0dfca2b354fc", "01a0d8c4-54fd-7b0e-8c1d-0efda2b354fd", "ibt-20-1-4.sfi", LICENCE_IBT,
     "IntelのAX200のBluetoothのファームウェア" + BT_WHAT),
    ("01a0d8c4-54fe-7b0f-8c1e-0ffea2b354fe", "01a0d8c4-54ff-7b10-8c1f-10ffa2b354ff", "ibt-20-1-4.ddc", LICENCE_IBT,
     "IntelのAX200のBluetoothの設定" + BT_WHAT),
    ("01a0d8c4-5490-7a01-8b10-0190a2b35490", "01a0d8c4-5491-7a02-8b11-0291a2b35491", "iwlwifi-Qu-b0-hr-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201(CNVi。チップのQu-b0)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-5492-7a03-8b12-0392a2b35492", "01a0d8c4-5493-7a04-8b13-0493a2b35493", "iwlwifi-Qu-c0-hr-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201(CNVi。チップのQu-c0)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-5494-7a05-8b14-0594a2b35494", "01a0d8c4-5495-7a06-8b15-0695a2b35495", "iwlwifi-QuZ-a0-hr-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201(CNVi。チップのQuZ-a0)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-5496-7a07-8b16-0796a2b35496", "01a0d8c4-5497-7a08-8b17-0797a2b35497", "iwlwifi-ty-a0-gf-a0-77.ucode", LICENCE,
     "IntelのWi-Fi 6E AX210のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-5498-7a09-8b18-0998a2b35498", "01a0d8c4-5499-7a0a-8b19-0a99a2b35499", "iwlwifi-ty-a0-gf-a0.pnvm", LICENCE,
     "IntelのWi-Fi 6E AX210のPNVM(電波の規制などのデータ)。" + IWX_WHAT),
    ("01a0d8c4-549a-7a0b-8b1a-0b9aa2b3549a", "01a0d8c4-549b-7a0c-8b1b-0c9ba2b3549b", "iwlwifi-so-a0-gf-a0-77.ucode", LICENCE,
     "IntelのWi-Fi 6E AX210の別のサブシステムとAX211のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-549c-7a0d-8b1c-0d9ca2b3549c", "01a0d8c4-549d-7a0e-8b1d-0e9da2b3549d", "iwlwifi-so-a0-gf-a0.pnvm", LICENCE,
     "IntelのWi-Fi 6E AX210の別のサブシステムとAX211のPNVM。" + IWX_WHAT),
    ("01a0d8c4-54e0-7a1b-8b2a-1bc0a2b354e0", "01a0d8c4-54e1-7a1c-8b2b-1cc1a2b354e1", "iwlwifi-so-a0-gf4-a0-77.ucode", LICENCE,
     "AX211と同じPCIのIDで、無線部がgf4のカード(AX411など)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54e2-7a1d-8b2c-1dc2a2b354e2", "01a0d8c4-54e3-7a1e-8b2d-1ec3a2b354e3", "iwlwifi-so-a0-gf4-a0.pnvm", LICENCE,
     "AX211と同じPCIのIDで、無線部がgf4のカードのPNVM。" + IWX_WHAT),
    ("01a0d8c4-54e4-7a1f-8b2e-1fc4a2b354e4", "01a0d8c4-54e5-7a20-8b2f-20c5a2b354e5", "iwlwifi-so-a0-hr-b0-77.ucode", LICENCE,
     "AX211と同じPCIのIDで、無線部がhrのカード(AX201、AX203、AX101)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54e6-7a21-8b30-21c6a2b354e6", "01a0d8c4-54e7-7a22-8b31-22c7a2b354e7", "iwlwifi-so-a0-jf-b0-77.ucode", LICENCE,
     "AX211と同じPCIのIDで、無線部がjfのカード(9560、9462、9461)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54e8-7a23-8b32-23c8a2b354e8", "01a0d8c4-54e9-7a24-8b33-24c9a2b354e9", "iwlwifi-Qu-b0-jf-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201のPCIのIDを持つ9462、9560(チップのQu-b0、無線部のjf)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54ea-7a25-8b34-25caa2b354ea", "01a0d8c4-54eb-7a26-8b35-26cba2b354eb", "iwlwifi-Qu-c0-jf-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201のPCIのIDを持つ9462、9560(チップのQu-c0、無線部のjf)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54ec-7a27-8b36-27cca2b354ec", "01a0d8c4-54ed-7a28-8b37-28cda2b354ed", "iwlwifi-QuZ-a0-jf-b0-77.ucode", LICENCE,
     "IntelのWi-Fi 6 AX201のPCIのIDを持つ9462、9560(チップのQuZ-a0、無線部のjf)のファームウェア(APIのバージョン77)。" + IWX_WHAT),
    ("01a0d8c4-54ee-7a29-8b38-29cea2b354ee", "01a0d8c4-54ef-7a2a-8b39-2acfa2b354ef", "iwlwifi-3160-17.ucode", LICENCE,
     "IntelのWireless 3160のファームウェア(APIのバージョン17)。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54f0-7a2b-8b3a-2bc0a2b354f0", "01a0d8c4-54f1-7a2c-8b3b-2cc1a2b354f1", "iwlwifi-3168-29.ucode", LICENCE,
     "IntelのWireless-AC 3168のファームウェア(APIのバージョン29)。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54f2-7a2d-8b3c-2dc2a2b354f2", "01a0d8c4-54f3-7a2e-8b3d-2ec3a2b354f3", "iwlwifi-7260-17.ucode", LICENCE,
     "IntelのWireless 7260のファームウェア(APIのバージョン17)。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54f4-7a2f-8b3e-2fc4a2b354f4", "01a0d8c4-54f5-7a30-8b3f-30c5a2b354f5", "iwlwifi-7265-17.ucode", LICENCE,
     "IntelのWireless 7265のファームウェア(APIのバージョン17)。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54f6-7a31-8b40-31c6a2b354f6", "01a0d8c4-54f7-7a32-8b41-32c7a2b354f7", "iwlwifi-7265D-29.ucode", LICENCE,
     "IntelのWireless-AC 3165のファームウェア(APIのバージョン29)。OpenBSDのiwmはPCIのIDが3165のカードと、7265のIDでリビジョンがDのカードにこれを求める。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54f8-7a33-8b42-33c8a2b354f8", "01a0d8c4-54f9-7a34-8b43-34c9a2b354f9", "iwlwifi-8000C-36.ucode", LICENCE,
     "IntelのWireless 8260のファームウェア(APIのバージョン36)。" + IWM_WHAT % "20230117"),
    ("01a0d8c4-54fa-7a35-8b44-35caa2b354fa", "01a0d8c4-54fb-7a36-8b45-36cba2b354fb", "iwlwifi-9000-pu-b0-jf-b0-46.ucode", LICENCE,
     "IntelのWireless-AC 9560(iwmが扱うPCIのIDのもの)のファームウェア(APIのバージョン46)。" + IWM_WHAT % "20240312"),
    ("01a0d8c4-54fc-7a37-8b46-37cca2b354fc", "01a0d8c4-54fd-7a38-8b47-38cda2b354fd", "iwlwifi-9260-th-b0-jf-b0-46.ucode", LICENCE,
     "IntelのWireless-AC 9260のファームウェア(APIのバージョン46)。" + IWM_WHAT % "20240312"),
    ("01a0d8c4-549e-7a0f-8b1e-0f9ea2b3549e", "01a0d8c4-549f-7a10-8b1f-109fa2b3549f", "WIFI_MT7961_patch_mcu_1_2_hdr.bin", LICENCE_MT,
     "MediaTekのMT7921のROMパッチ。" + MWX_WHAT),
    ("01a0d8c4-54a0-7a11-8b20-11a0a2b354a0", "01a0d8c4-54a1-7a12-8b21-12a1a2b354a1", "WIFI_RAM_CODE_MT7961_1.bin", LICENCE_MT,
     "MediaTekのMT7921のファームウェア。" + MWX_WHAT),
    ("01a0d8c4-54a2-7a13-8b22-13a2a2b354a2", "01a0d8c4-54a3-7a14-8b23-14a3a2b354a3", "WIFI_MT7961_patch_mcu_1a_2_hdr.bin", LICENCE_MT,
     "MediaTekのMT7920のROMパッチ。" + MWX_WHAT),
    ("01a0d8c4-54a4-7a15-8b24-15a4a2b354a4", "01a0d8c4-54a5-7a16-8b25-16a5a2b354a5", "WIFI_RAM_CODE_MT7961_1a.bin", LICENCE_MT,
     "MediaTekのMT7920のファームウェア。" + MWX_WHAT),
    ("01a0d8c4-54a6-7a17-8b26-17a6a2b354a6", "01a0d8c4-54a7-7a18-8b27-18a7a2b354a7", "WIFI_MT7922_patch_mcu_1_1_hdr.bin", LICENCE_MT,
     "MediaTekのMT7922のROMパッチ。" + MWX_WHAT),
    ("01a0d8c4-54a8-7a19-8b28-19a8a2b354a8", "01a0d8c4-54a9-7a1a-8b29-1aa9a2b354a9", "WIFI_RAM_CODE_MT7922_1.bin", LICENCE_MT,
     "MediaTekのMT7922のファームウェア。" + MWX_WHAT),
]
# Qualcomm's QCA6174 hw3.x (168c:003e), read by TessronOS's athx (include/ts/sysdef.h
# SYSDEF_FW_ATHX6174_*): the object, the link to it, its name, the file in DIR, what it is
ATHX_LIC = ["LICENSE.QualcommAtheros_ath10k", "notice_ath10k_firmware-6.txt"]
ATHX_WHAT = ("Wi-Fiのドライバ(TessronOSのathx)がカードを動かすときに読む。レコード1がQualcommの配布するファイルそのもの"
             "(linux-firmwareのタグ20261019のath10k/QCA6174/hw3.0/から取得し、SHA-256を確かめたもの)。"
             "Qualcommの次の許諾に従って再配布する。許諾の後に、ファームウェアが含むソフトウェアの告知"
             "(notice_ath10k_firmware-6.txt)を続ける。")
ATHX = [
    ("01a0d8c4-55a0-7c01-8d10-01a0b2c355a0", "01a0d8c4-55a1-7c02-8d11-02a1b2c355a1",
     "QCA6174-hw3.0-firmware-6.bin", "firmware-6.bin",
     "QualcommのQCA6174(PCI 168c:003e。チップのhw3.0とhw3.2。初代Surface GoのWi-Fi)のファームウェア"
     "(WLAN.RM.4.4.1-00309、APIの6)。" + ATHX_WHAT),
    ("01a0d8c4-55a2-7c03-8d12-03a2b2c355a2", "01a0d8c4-55a3-7c04-8d13-04a3b2c355a3",
     "QCA6174-hw3.0-board-2.bin", "board-2.bin",
     "QualcommのQCA6174の基板ごとの較正のデータ(board-2.bin)。athxは、カードのPCIのIDとサブシステムのIDから作る"
     "名前(bus=pci,vendor=168c,device=003e,subsystem-vendor=…,subsystem-device=…)の一つを選んでカードへ送る。" + ATHX_WHAT),
]
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


def write_fw(out, uuid, name, path, lic, what):
    """A firmware file as a plain object: record 1 the file, record 0 what it is and the maker's licence (one file or several)"""
    write(os.path.join(out, uuid + ".json"),
          json.dumps(meta(name, "r--r--r--", os.path.getsize(path), file=name),
                     ensure_ascii=False, indent=2) + "\n")
    paras = [name, what]
    for l in ([lic] if isinstance(lic, str) else lic):
        paras += [x for x in open(l, encoding="utf-8").read().split("\n\n") if x.strip()]
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
    for _, _, name, licence, _ in MORE:
        for f in (name, licence):
            if not os.path.isfile(os.path.join(a.fw, f)):
                sys.exit("wifi-obj: %s is not in %s (tools/x64/wifi/fw.sh)" % (f, a.fw))
    for _, _, _, f, _ in ATHX:
        for g in [f] + ATHX_LIC:
            if not os.path.isfile(os.path.join(a.fw, g)):
                sys.exit("wifi-obj: %s is not in %s (tools/x64/wifi/fw.sh)" % (g, a.fw))
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
    for uuid, _, name, licence, what in MORE:
        write_fw(a.out, uuid, name, os.path.join(a.fw, name), os.path.join(a.fw, licence), what)
    for uuid, _, name, f, what in ATHX:
        write_fw(a.out, uuid, name, os.path.join(a.fw, f), [os.path.join(a.fw, l) for l in ATHX_LIC], what)

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
    if "</figure>" not in box:
        sys.exit("wifi-obj: the system box %s is not laid out as expected" % a.box)
    links = links_after(box, [(FW_UUID, FW_LINK, FW_FILE), (CONF_UUID, CONF_LINK, name),
                              (FWX_UUID, FWX_LINK, FWX_FILE)]
                        + [(m[0], m[1], m[2]) for m in MORE] + [(m[0], m[1], m[2]) for m in ATHX])
    box = box.replace("</figure>", links + "</figure>", 1)
    write(os.path.join(a.out, SYSBOX + "_0.xtad"), box)
    print("wifi-obj: %s (firmware %d and %d bytes and %d others, settings, the system box)"
          % (a.out, os.path.getsize(fw), os.path.getsize(fwx), len(MORE) + len(ATHX)))


if __name__ == "__main__":
    main()
