#!/bin/bash
# The desktop under QEMU in a window of its own, to be tried by hand with
# the mouse and the keyboard. Run from WSL:
#
#   tools/desk_run.sh            build what changed, then start
#   RESET=1 tools/desk_run.sh    start again from a fresh disk
#
# The disk is the test disk (the system volume with the sample objects,
# /boot with the fonts and the programs), copied once to
# build_make/obj/desk/disk.img and kept between runs, so what is saved
# stays. A USB memory with a FAT32 partition is plugged in as well.
#
# The window is the Windows QEMU's (GTK): the one in WSL has no window of
# its own worth using. Its user network reaches the host as 10.0.2.2.
# The environment may change:
#   QEMU_WIN  the Windows QEMU          (default C:\Program Files\qemu)
#   MOZC      1 to build with kana-kanji conversion when its parts are
#             there, 0 without         (default: 1 when they are there)
#   SMP       processors               (default 4)
#   SERIAL    vc: the console in the window's menu (View), stdio: here
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP/build_make"
# shellcheck disable=SC1091
source "$TOP/tools/env.sh" >/dev/null

QEMU_WIN=${QEMU_WIN:-"/mnt/c/Program Files/qemu/qemu-system-aarch64.exe"}
SMP=${SMP:-4}
SERIAL=${SERIAL:-vc}
if [ -z "${MOZC:-}" ]; then
	if [ -f "$HOME/tessronos-mozc/tsmozc.o" ] && [ -f "$HOME/mozc/src/bazel-bin/data_manager/oss/mozc.data" ]; then
		MOZC=1
	else
		MOZC=0
	fi
fi
[ -x "$QEMU_WIN" ] || { echo "desk_run: no QEMU at $QEMU_WIN (set QEMU_WIN)"; exit 2; }

# the disks (made by the test build) and the desktop kernel
DISKDIR=obj/_QEMU_VIRT__ktestdeskdisk
make TARGET=_QEMU_VIRT_ KTEST=1 BUILD_ID=deskdisk MOZC="$MOZC" \
	"$DISKDIR/test_disk.img" "$DISKDIR/usb_disk.img"
make TARGET=_QEMU_VIRT_ DESKTOP=1 BUILD_ID=desk MOZC="$MOZC"

# the disks the run writes to, kept between runs
mkdir -p obj/desk
if [ "${RESET:-0}" = 1 ] || [ ! -f obj/desk/disk.img ]; then
	cp "$DISKDIR/test_disk.img" obj/desk/disk.img
	cp "$DISKDIR/usb_disk.img" obj/desk/usb.img
	echo "desk_run: a fresh disk"
fi

win() { wslpath -w "$1"; }
exec "$QEMU_WIN" -M virt,gic-version=2 -cpu cortex-a76 -smp "$SMP" -m 2G \
	-display gtk,zoom-to-fit=off -name TessronOS \
	-global virtio-mmio.force-legacy=false \
	-drive file="$(win obj/desk/disk.img)",if=none,format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0 \
	-netdev user,id=n0 -device virtio-net-device,netdev=n0 \
	-device qemu-xhci,id=xhci \
	-device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
	-drive file="$(win obj/desk/usb.img)",if=none,format=raw,id=usb0 \
	-device usb-storage,bus=xhci.0,drive=usb0,removable=on \
	-audiodev dsound,id=snd0 -device usb-audio,bus=xhci.0,audiodev=snd0 \
	-device bochs-display \
	-serial "$SERIAL" \
	-kernel "$(win tessronosdesk.elf)"
