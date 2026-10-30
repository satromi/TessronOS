#!/bin/bash
#
# tools/rpi5/wifi/fw.sh — the files of the Raspberry Pi 5's Wi-Fi chip (CYW43455 on SDIO,
# docs/private/rpi5_wifi_study.md 2.4): the firmware and the regulatory data (CLM) that
# NetBSD's bwfm(4) loads into the chip, the settings of the Pi's board (NVRAM, a text), and
# the licences they are distributed under, taken from linux-firmware at a fixed tag into a
# directory of their own (~/tessronos-rpi5-wifi-fw), each checked against the SHA-256 and the
# size written here.
#
#   tools/rpi5/wifi/fw.sh [fetch|clean]
#
# The firmware and the CLM are Cypress's (cypress/, linked from brcm/ under the names bwfm asks
# for), redistributable in object code under LICENCE.cypress. The NVRAM text is the Raspberry
# Pi 4 Model B's, which linux-firmware's WHENCE also gives the Pi 5 (raspberrypi,5-model-b)
# and the Compute Module 5; it is under GPL-2.0 (or later, its own SPDX line). The same bytes
# were had from git.kernel.org and from the project's mirror on gitlab.com when the sums were
# written; FW_URL may name another place with the same tree (FW_URL/<path>).
#
set -e

STAGE=${1:-fetch}
OUT=${OUT:-$HOME/tessronos-rpi5-wifi-fw}
TAG=20260810
URLS="${FW_URL:+$FW_URL }https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
 https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$TAG"

# path in linux-firmware, its SHA-256, its size
FILES="cypress/cyfmac43455-sdio.bin d408faa9d0d5b1a2f9912dcea53ab0be48217288e398406d117f0edafe7c3edd 643651
cypress/cyfmac43455-sdio.clm_blob 15f50a27020b263d1bea215c8f68d0550d912932d1d9ef19ffd59f18d82dd460 4733
brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt edb6f4e4fb19e18940004124feb4ffe160d72fc607243a07a4480338a28b2748 1883
LICENSES/LICENCE.cypress ae0db6cc4db33941148df0f67de53e76a77b1b5a46b3165edb7040aa2750015f 8914
LICENSES/GPL-2.0 edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6 17984"

ok() {
	[ -f "$1" ] && [ "$(stat -c %s "$1")" = "$3" ] && echo "$2  $1" | sha256sum -c --status
}

fetch() {
	local path f sum size u got tmp
	mkdir -p "$OUT"
	echo "$FILES" | while read -r path sum size; do
		f=$(basename "$path")
		ok "$OUT/$f" "$sum" "$size" && continue
		# each file is fetched into a new empty directory and moved in only once its sum is right
		tmp=$(mktemp -d "$OUT/.dl.XXXXXX")
		got=""
		for u in $URLS; do
			case $u in
			*git.kernel.org*)	u="$u/$path?h=$TAG" ;;
			*)			u="$u/$path" ;;
			esac
			if curl -fsSL -m 300 -o "$tmp/$f" "$u" && ok "$tmp/$f" "$sum" "$size"; then
				got=$u
				break
			fi
			rm -f "$tmp/$f"
		done
		if [ -z "$got" ]; then
			rm -rf "$tmp"
			echo "rpi5-wifi-fw: $path (linux-firmware $TAG) could not be had with the right SHA-256" >&2
			exit 1
		fi
		mv "$tmp/$f" "$OUT/$f"
		rm -rf "$tmp"
		echo "rpi5-wifi-fw: $f from $got"
	done
	echo "rpi5-wifi-fw: $OUT"
}

case $STAGE in
fetch)	fetch ;;
clean)	rm -rf "$OUT" ;;
*)	echo "usage: $0 [fetch|clean]" >&2; exit 2 ;;
esac
