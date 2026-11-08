#!/bin/bash
#
# tools/x64/wifi/fw.sh — the firmware of the Wi-Fi cards the x64 kernel drives (docs/tessronos/
# 09-network.md 9.6, 9.6.9): Intel's microcode for the Dual Band Wireless-AC 8265 (8086:24FD), the
# version OpenBSD's iwm(4) asks for (iwlwifi-8265-36.ucode, API 36), for the Wi-Fi 6 AX200
# (8086:2723, the Surface Go 2's), the version OpenBSD's iwx(4) asks for (iwlwifi-cc-a0-77.ucode,
# API 77), for the AX201 and AX210, for MediaTek's MT7921 and MT7922 (OpenBSD's mwx(4)) and for
# Qualcomm's QCA6174 hw3.x (168c:003e, the first Surface Go's; TessronOS's athx), and the
# makers' licences for them, taken from linux-firmware at fixed tags into a directory
# of their own (~/tessronos-wifi-fw), each checked against the SHA-256 and the size written here.
# The build makes the microcode objects of the system volume (tools/x64/wifi/mkobj.py,
# build_make/x64.mk).
#
#   tools/x64/wifi/fw.sh [fetch|clean]
#
# Each file is taken at the tag whose bytes are those OpenBSD 8.0's own firmware packages have,
# which its drivers read:
#
# - iwlwifi-8265-36.ucode at 20230117: iwm-firmware-20240410's iwm-8265-36. Later tags have a
#   newer file of the same name, whose TLVs 66 and 67 OpenBSD's iwm does not know and refuses
#   (firmware parse error 22, section type 66);
# - iwlwifi-cc-a0-77.ucode at 20251021 (by then in intel/iwlwifi/ of the tree):
#   iwx-firmware-20251021p0's iwx-cc-a0-77. The tag before (20250917) has other bytes of the
#   same size;
# - the AX201's (Qu-b0, Qu-c0, QuZ-a0 with the hr-b0 radio) and the AX210's (ty-a0 and so-a0 with the
#   gf-a0 radio, each with its PNVM) at 20251021: the same package's files of those names;
# - the others iwx(4) may ask for on the same PCI IDs as the AX211 (8086:51F0 and its kin), by the
#   radio the card reports (iwx_dev_info_table): so-a0 with gf4-a0 (with its PNVM), hr-b0 and jf-b0,
#   at 20251021: the same package's iwx-so-a0-*;
# - the AX201's PCI IDs with the jf radio (9462, 9560: Qu-b0, Qu-c0, QuZ-a0 with jf-b0) at 20251021:
#   the same package's iwx-*-jf-b0-77;
# - the other cards iwm(4) drives, by the name it asks for: iwm-3160-17, iwm-3168-29, iwm-7260-17,
#   iwm-7265-17, iwm-7265D-29 and iwm-8000C-36 at 20230117, iwm-9000-46 and iwm-9260-46 (in the tree
#   iwlwifi-9000-pu-b0-jf-b0-46 and iwlwifi-9260-th-b0-jf-b0-46) at 20240312: iwm-firmware-20240410's.
#   20230117 has other bytes for the last two;
# - MediaTek's and LICENCE.mediatek at 20260309: mwx-firmware-20260309p0's mwx-mt7961_* and
#   mwx-mt7922_*, and its mwx-license;
# - the Bluetooth's: Intel's ibt-* for the 8265 and the AX200, and LICENCE.ibt_firmware, at 20251021
#   (design 6.8; device/bt/bt_intel.c names the same files);
# - Qualcomm's for the QCA6174 at 20261019: ath10k/QCA6174/hw3.0/firmware-6.bin (WLAN.RM.4.4.1-00309),
#   board-2.bin (the boards' calibration, of which athx takes the one named by the card's PCI IDs and
#   subsystem), notice_ath10k_firmware-6.txt (the notices of what the firmware contains) and
#   LICENSES/LICENSE.QualcommAtheros_ath10k. Each is the same file in hw3.0 for hw3.0 and hw3.2;
# - LICENCE.iwlwifi_firmware: the same text at both tags, and in both packages (iwm-license,
#   iwx-license).
#
# The same bytes were had from git.kernel.org and from the project's mirror on gitlab.com when
# the sums were written; FW_URL may name another place with the same tree (FW_URL/<dir><file>).
#
set -e

STAGE=${1:-fetch}
OUT=${OUT:-$HOME/tessronos-wifi-fw}
URLS="${FW_URL:+$FW_URL }https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
 https://gitlab.com/kernel-firmware/linux-firmware/-/raw"

# file, its SHA-256, its size, the tag, and the directory of the tree it is in (- the top)
FILES="iwlwifi-8265-36.ucode d7d84c9b6a3b1976acb97a2bec7102e24a6b5d794110c0dcbd107ab5d9877e5f 2436632 20230117 -
iwlwifi-cc-a0-77.ucode 64287d37a0f92722de7aa90e9cb41d31a3b4b4fd0966b3ba5fa48cfa426f5a09 1368096 20251021 intel/iwlwifi/
iwlwifi-Qu-b0-hr-b0-77.ucode faa81d3c46bfa7e82700452a1590e15bc3b5bd2f954628f8820c12f4e3e01fe3 1406572 20251021 intel/iwlwifi/
iwlwifi-Qu-c0-hr-b0-77.ucode de48c1a6fc671231e85495508d5ff05c3a2edb82610d79b0433fa5e560a0495f 1406588 20251021 intel/iwlwifi/
iwlwifi-QuZ-a0-hr-b0-77.ucode ad2f6202b0920db7cc5942d13253db322e0cab4adb135f26bcf4a984735a3b7c 1406716 20251021 intel/iwlwifi/
iwlwifi-ty-a0-gf-a0-77.ucode fd8977dbfacfe50b8a6601479aa9ffd21d01e274e4a9efd13935dd9d667fd45b 1594276 20251021 intel/iwlwifi/
iwlwifi-ty-a0-gf-a0.pnvm 7056112e90244dc94cc43b8c7a84bd6b99f5e495d66194008ece5770e7412db0 55052 20251021 intel/iwlwifi/
iwlwifi-so-a0-gf-a0-77.ucode 87590ea9535a8939fddb296bc7622f3a900f28f47b45cb4e435a2a1d5f496ccd 1641260 20251021 intel/iwlwifi/
iwlwifi-so-a0-gf-a0.pnvm b37f038b3467ed568d107a9c1dfd916fa7e8cbd58b2f9bec0c2c53297ccfcab7 55208 20251021 intel/iwlwifi/
iwlwifi-so-a0-gf4-a0-77.ucode d945bce539ee78033a85c288da9b988b2651a1623c5f6081b7db510d3967df76 1669164 20251021 intel/iwlwifi/
iwlwifi-so-a0-gf4-a0.pnvm fb77cc355ade54a2345665f16329579fde6a9193d73cfb8f5af736a57c0e4f39 27876 20251021 intel/iwlwifi/
iwlwifi-so-a0-hr-b0-77.ucode bed4958236366c3c451b3ec994a56b2ad83c08d70e0a1eae42fb00f2eaabbcc8 1500532 20251021 intel/iwlwifi/
iwlwifi-so-a0-jf-b0-77.ucode 9dccba0a8ba98483cb8385398d013b693b5b37cbff9402763ba215cc4921386c 1446036 20251021 intel/iwlwifi/
iwlwifi-Qu-b0-jf-b0-77.ucode 6d0e41d309db34db23055d58a1389c875635fb1402f73a6a9ec72879761e6f9b 1323292 20251021 intel/iwlwifi/
iwlwifi-Qu-c0-jf-b0-77.ucode 53f75e001ea83935d949f56a11c473f35bde5fc1800c902bc09b87a879507982 1323308 20251021 intel/iwlwifi/
iwlwifi-QuZ-a0-jf-b0-77.ucode 0efd65924235b7593e3403f623719d377e55eab7462db26bfdd95224628b3170 1323376 20251021 intel/iwlwifi/
iwlwifi-3160-17.ucode 0e893c1ed74c3186047508e93442b2d71a07dd5fac00d9dbdb68f34407d7be14 918268 20230117 -
iwlwifi-3168-29.ucode 3afae7bf4bb5fbb83a895ec7ad097b9fd20b7d8794f405c2823cbea5284e65c3 1036288 20230117 -
iwlwifi-7260-17.ucode 5d81a6003df0228a497ad27f916ba2c979614b4c439b0f45a5f2873dc0607fe8 1049340 20230117 -
iwlwifi-7265-17.ucode 91f699308f102a9ed90217d813bfdb355464ece958a4f8ac96bba5e9d77fe870 1180412 20230117 -
iwlwifi-7265D-29.ucode f6776a50107cba4f68362b5cf8455f5a0db6a4e8d1e34f4ede6f26803458e25e 1036772 20230117 -
iwlwifi-8000C-36.ucode a830767818a47b5c51ae3a763fa7a65b03703ce38d3ef5918d8cd7ba82859c64 2428004 20230117 -
iwlwifi-9000-pu-b0-jf-b0-46.ucode 9a0c5d938646942f9798e6212579c25771e4bb44301cd409b82abe361ca2c3e0 1519568 20240312 -
iwlwifi-9260-th-b0-jf-b0-46.ucode 05f4c12514fddfd56fea660b3d2eb6b6e6f4a87a324d5d40464050312ddca911 1490508 20240312 -
WIFI_MT7961_patch_mcu_1_2_hdr.bin 9eaf4d2b07e08b5b41631cc7bb04ebc60263011d8412cd05c4786609ffbcebd1 92192 20260309 mediatek/
WIFI_RAM_CODE_MT7961_1.bin 0a99736f35d4d4318d8d2fdaac550ecfee88d8b9acfef68d40bff79e22166a00 791908 20260309 mediatek/
WIFI_MT7961_patch_mcu_1a_2_hdr.bin ca3acc30064567a05a8dd3e62db94a3abcf2cd1ab1e0c659812b74dea76b3fce 100384 20260309 mediatek/
WIFI_RAM_CODE_MT7961_1a.bin ff9f27c15bf078c8df02b40a8cda72740d2de52d7d9660f4ca861fc2442d34e6 802744 20260309 mediatek/
WIFI_MT7922_patch_mcu_1_1_hdr.bin 6d04988f5f44fc41e9404a492291a7a519b38e6aea6369b2f939cd0c70765f5a 137632 20260309 mediatek/
WIFI_RAM_CODE_MT7922_1.bin 1226f5b30531b2f027897a4a499fb77c31f4a39025c98e5a9896769aaa781fda 1003092 20260309 mediatek/
ibt-12-16.sfi 75b8b8cc10abd616a96066bce9ce62be80ebc293d6a4e9a0a11c3ecc5d3ca373 584092 20251021 intel/
ibt-12-16.ddc 3ca252ad20e8f76d6d906536bf9e5abe6654c657183584a48e90a0dcf40ac4d5 14 20251021 intel/
ibt-20-0-3.sfi fd51628c307a1b03fbd7156f0d2a7f5af67b7202d4f358f5d4d807911982c3cc 801016 20251021 intel/
ibt-20-0-3.ddc fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 9 20251021 intel/
ibt-20-1-3.sfi fd51628c307a1b03fbd7156f0d2a7f5af67b7202d4f358f5d4d807911982c3cc 801016 20251021 intel/
ibt-20-1-3.ddc fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 9 20251021 intel/
ibt-20-1-4.sfi 00e29ce24fc8f312309f9df0282345c8c1e6d0745245ed3a8dfe1de0a4b20289 799640 20251021 intel/
ibt-20-1-4.ddc fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077 9 20251021 intel/
LICENCE.ibt_firmware 5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4 2040 20251021 -
LICENCE.mediatek a90d3f66704d85889945fec5525ea77622549da83aced1aac99828383f8f1805 561 20260309 -
LICENCE.iwlwifi_firmware 16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5 2046 20230117 -
firmware-6.bin 04d3bad5efa3f9fbe3ba53fd3e25fa9b0585ed227eea8111303b4e08861f979d 706360 20261019 ath10k/QCA6174/hw3.0/
board-2.bin 66e83dde1c9af535df1fcd17c72971a96a263357300e921b358d35a353227d60 740076 20261019 ath10k/QCA6174/hw3.0/
notice_ath10k_firmware-6.txt 8ce5c6ea0542bf4aac31fc3ae16a39792ad22d0eae4543063fac56fb3380f021 53425 20261019 ath10k/QCA6174/hw3.0/
LICENSE.QualcommAtheros_ath10k 337a55102138d7baa143ee4a4c6c91693e0113fece35d380b2a12109e8c23b3f 2713 20261019 LICENSES/"

ok() {
	[ -f "$1" ] && [ "$(stat -c %s "$1")" = "$3" ] && echo "$2  $1" | sha256sum -c --status
}

fetch() {
	local f sum size tag dir u got tmp
	mkdir -p "$OUT"
	echo "$FILES" | while read -r f sum size tag dir; do
		[ "$dir" = "-" ] && dir=""
		ok "$OUT/$f" "$sum" "$size" && continue
		# each file is fetched into a new empty directory and moved in only once its sum is right
		tmp=$(mktemp -d "$OUT/.dl.XXXXXX")
		got=""
		for u in $URLS; do
			case $u in
			*git.kernel.org*)	u="$u/$dir$f?h=$tag" ;;
			*gitlab.com*)		u="$u/$tag/$dir$f" ;;
			*)			u="$u/$dir$f" ;;
			esac
			if curl -fsSL -m 300 -o "$tmp/$f" "$u" && ok "$tmp/$f" "$sum" "$size"; then
				got=$u
				break
			fi
			rm -f "$tmp/$f"
		done
		if [ -z "$got" ]; then
			rm -rf "$tmp"
			echo "wifi-fw: $f (linux-firmware $tag) could not be had with the right SHA-256" >&2
			exit 1
		fi
		mv "$tmp/$f" "$OUT/$f"
		rm -rf "$tmp"
		echo "wifi-fw: $f from $got"
	done
	echo "wifi-fw: $OUT"
}

case $STAGE in
fetch)	fetch ;;
clean)	rm -rf "$OUT" ;;
*)	echo "usage: $0 [fetch|clean]" >&2; exit 2 ;;
esac
