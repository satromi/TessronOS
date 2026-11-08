#!/bin/bash
# The fimps of exFAT and NTFS run on the host against images, and the
# results checked by the standard tools: exfatprogs (kept unpacked under
# ~/.local/exfatprogs) and the ntfs-3g utilities. Only tools here use them;
# none of their code goes into the system. Usage: hostfs.sh exfat|ntfs
set -u
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
work=$(mktemp -d /tmp/hostfs.XXXXXX)
trap 'rm -rf "$work"' EXIT
EX=$HOME/.local/exfatprogs/usr/sbin
export LD_LIBRARY_PATH=$HOME/.local/exfatprogs/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}
INC="-I$here/shim -I$top/include -I$top/peripheral_kernel/fs"
CF="-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-function"
case "${1:-exfat}" in
exfat)
	gcc $CF $INC -o "$work/t" "$here/exfat_host.c" "$top/peripheral_kernel/fs/exfatfs.c" "$top/peripheral_kernel/fs/fsname.c" || exit 1
	rc=0
	for spec in "64M 4K" "64M 512" "300M 128K"; do
		set -- $spec
		truncate -s "$1" "$work/i.img"
		"$EX/mkfs.exfat" -c "$2" "$work/i.img" >/dev/null || exit 1
		echo "== $1 cluster $2"
		"$work/t" "$work/i.img" || rc=1
		"$EX/fsck.exfat" -n "$work/i.img" 2>&1 | tail -4 || true
		"$EX/fsck.exfat" -n "$work/i.img" >/dev/null 2>&1 || { echo "fsck.exfat FAILED"; rc=1; }
		rm -f "$work/i.img"
	done
	echo "RESULT rc=$rc"
	exit $rc
	;;
ntfs)
	bash "$here/ntfs_host.sh" "$work"
	;;
esac
