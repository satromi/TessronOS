#!/bin/bash
#
# tools/x64/wifi/fw.sh — the firmware of the Wi-Fi cards the x64 kernel drives (docs/tessronos/
# 09-network.md 9.6, 9.6.9): Intel's microcode for the Dual Band Wireless-AC 8265 (8086:24FD), the
# version OpenBSD's iwm(4) asks for (iwlwifi-8265-36.ucode, API 36), for the Wi-Fi 6 AX200
# (8086:2723, the Surface Go 2's), the version OpenBSD's iwx(4) asks for (iwlwifi-cc-a0-77.ucode,
# API 77), and Intel's licence for them, taken from linux-firmware at fixed tags into a directory
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
LICENCE.iwlwifi_firmware 16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5 2046 20230117 -"

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
