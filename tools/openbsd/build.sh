#!/bin/bash
#
# build.sh -- OpenBSD's IEEE 802.11 layer, iwm(4), iwx(4), mwx(4) and TessronOS's athx for the x64 PC's Wi-Fi
#
# Copyright (C) 2026 satromi
# This software is distributed under the T-License 2.2.
#
# The Wi-Fi of docs/tessronos/09-network.md 9.6.9 (make WIFI_NET80211=obsd,
# build_make/x64.mk): OpenBSD's net80211 (802.11n and ac, protected
# management frames, background scans and roaming) and its iwm(4) (Intel's
# 7260 to 8265) and iwx(4) (the Wi-Fi 6 AX200 and later; the layer chooses
# between them by the card's PCI ID), with the parts of OpenBSD's kernel
# they use (mbufs, the media of an interface, red-black trees, the ciphers
# and hashes of net80211, fls), compiled with
# OpenBSD's own headers and the bare x86_64 toolchain as the kernel is
# (the kernel's code model, no red zone, no SIMD registers), together with
# the layer that gives them the kernel services they call
# (device/net/wifi/obsd), into one relocatable object:
#
#	$OUT/obsdwifi.o (~/tessronos-obsd-x64)
#
# The sources are OpenBSD's sys.tar.gz of the release OB_VER, checked
# against the SHA-256 the release publishes (OB_SHA256), kept in
# ~/openbsd/dl and unpacked (only the directories below) into
# ~/openbsd/src-$OB_VER. They are not changed. A patch of
# tools/openbsd/patches, when there is one for a file, is applied to a
# copy of the file, and the copy compiled in its place (patched_copy,
# 09-network.md 9.6.2); a header's patched copy is found before
# OpenBSD's (HDR_PATCHED, -I$GEN/patched/sys): only a plain error, or the
# least a feature of the standard needs that the release does not have,
# each with its reason at its head. Five functions of net80211 are wrapped at the link (--wrap)
# by the layer's: ieee80211_end_scan (each scan's end counted and told),
# ieee80211_eapol_key_input (the EAPOL frames to TessronOS's handshake,
# not OpenBSD's), ieee80211_inputm (the frames the driver hands up, counted
# before the layer's checks), ieee80211_encap (the first data frames
# after a join said, with their power management bit) and
# ieee80211_node_trigger_addba_req (a block-ack agreement for what the
# station sends asked for only once the port is open and the TID carries
# traffic, as iwlwifi starts one).
#
# Every symbol is then put into the obsdns_ name space and made local, so
# that the kernel sees only the entry points rump_tswifi_*; what the
# object needs from outside is the hypercalls (obsd_hc_*, obsd_user.c, and
# rumpcomp_*, peripheral_kernel/network/netbsd/wifi_user.c) and the
# compiler's run-time library.
#
# With ARCH=aarch64 (the Raspberry Pi's, make WIFI_NET80211=obsd, build_make/wifi_arm.mk) the same
# layer is built by the bare aarch64 toolchain with arm64's headers, MARCH for the machine
# (armv8-a+crc for the Pi 4 family, ~/tessronos-obsd-arm-v80), and the drivers are in the
# place of the PCI cards: sdhc(4), sdmmc(4) and bwfm(4) with its SDIO attachment.
#
# Usage: build.sh [fetch|compile|link|notices|all|clean]
#
set -e

STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
ARCH=${ARCH:-x86_64}
if [ "$ARCH" = aarch64 ]; then
	XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	TP=aarch64-none-elf
	OB_MACHINE=arm64
else
	XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	TP=x86_64-elf
	OB_MACHINE=amd64
fi
CC=$XB/$TP-gcc
LD=$XB/$TP-ld
OBJCOPY=$XB/$TP-objcopy
NM=$XB/$TP-nm
JOBS=${JOBS:-12}

# The release, where it is fetched from, and the SHA-256 of its sys.tar.gz
# (the release's SHA256 file; the same bytes were had from both places)
OB_VER=8.0
OB_URLS="${OB_URL:+$OB_URL }https://cdn.openbsd.org/pub/OpenBSD/$OB_VER/sys.tar.gz
 https://ftp.openbsd.org/pub/OpenBSD/$OB_VER/sys.tar.gz"
OB_SHA256=964b1aed7839b86f4e010a0fdd03dcd409e6f569e69fe85f95e5fceb740c2aaf
OB_DL=${OB_DL:-$HOME/openbsd/dl/$OB_VER/sys.tar.gz}
OB=${OBSRC:-$HOME/openbsd/src-$OB_VER}
S=$OB/sys
if [ "$ARCH" = aarch64 ]; then
	OUT=${OUT:-$HOME/tessronos-obsd-arm}
	MARCH=${MARCH:-armv8.2-a}
else
	OUT=${OUT:-$HOME/tessronos-obsd-x64}
	MARCH=${MARCH:-x86-64-v2}
fi
COMP=$TOP/device/net/wifi/obsd
PATCHES=$TOP/tools/openbsd/patches
GEN=$OUT/gen
OBJ=$OUT/obj

# The directories of sys.tar.gz that are unpacked (their files, not what is below them),
# and the machine's headers whole
EXTRACT_DIRS="sys/net80211 sys/sys sys/net sys/netinet sys/netinet6 sys/crypto
 sys/kern sys/uvm sys/lib/libkern sys/dev/pci sys/dev/ic sys/ddb sys/dev/sdmmc sys/scsi sys/dev/ofw"
EXTRACT_TREES="sys/arch/amd64/include sys/arch/x86/include sys/arch/arm64/include"

# The sources: net80211 whole, iwm, iwx and mwx, and of OpenBSD's kernel what they use
NET80211_SRCS="ieee80211.c ieee80211_amrr.c ieee80211_crypto.c ieee80211_crypto_bip.c
 ieee80211_crypto_ccmp.c ieee80211_crypto_tkip.c ieee80211_crypto_wep.c
 ieee80211_input.c ieee80211_ioctl.c ieee80211_node.c ieee80211_output.c
 ieee80211_pae_input.c ieee80211_pae_output.c ieee80211_proto.c ieee80211_ra.c
 ieee80211_ra_vht.c ieee80211_regdomain.c ieee80211_rssadapt.c"
DRV_SRCS="if_iwm.c if_iwx.c if_mwx.c"
KERN_SRCS="kern/uipc_mbuf.c kern/uipc_mbuf2.c kern/subr_tree.c net/if_media.c
 crypto/aes.c crypto/cmac.c crypto/hmac.c crypto/md5.c crypto/sha1.c crypto/sha2.c
 crypto/key_wrap.c crypto/michael.c crypto/arc4.c lib/libkern/fls.c"
COMP_SRCS="obsd_kern.c obsd_net.c obsd_pci.c tswifi_obsd.c tsw_iwm.c tsw_iwx.c tsw_mwx.c obsd_test.c
 tsw_athx.c athx.c athx_pci.c athx_fw.c athx_wmi.c athx_htt.c"
# OpenBSD's headers that have patches (tools/openbsd/patches/<name>-*.patch)
HDR_PATCHED="dev/pci/if_iwxvar.h"
# The Raspberry Pi: no PCI card, the chip on its SDIO host (obsd_bus_arm.c, tsw_bwfm.c)
SDIO_SRCS=""
if [ "$ARCH" = aarch64 ]; then
	DRV_SRCS=""
	SDIO_SRCS="dev/sdmmc/sdhc.c dev/sdmmc/sdmmc.c dev/sdmmc/sdmmc_cis.c dev/sdmmc/sdmmc_io.c
	 dev/sdmmc/sdmmc_mem.c dev/sdmmc/if_bwfm_sdio.c dev/ic/bwfm.c"
	COMP_SRCS="obsd_kern.c obsd_net.c obsd_bus_arm.c tswifi_obsd.c tsw_bwfm.c obsd_test.c"
fi
# WIFI_SIM=1: the Wi-Fi simulator's two radios (device/net/wifi/sim/obsd_sim.c, docs/tessronos/
# 09-network.md 9.6.12) in the place of the drivers and their PCI bus, into another place (OUT)
WIFI_SIM=${WIFI_SIM:-0}
if [ "$WIFI_SIM" = 1 ]; then
	DRV_SRCS=""
	SDIO_SRCS=""
	COMP_SRCS="obsd_kern.c obsd_net.c tswifi_obsd.c"
	[ "$ARCH" = aarch64 ] && COMP_SRCS="obsd_kern.c obsd_net.c tswifi_obsd.c"
fi

# As the kernel is built (common.mk), without the floating point registers;
# a uniprocessor kernel's headers (no MULTIPROCESSOR), with the steps of
# the drivers and net80211 said while the manager wants them (rump_tswifi_debug)
if [ "$ARCH" = aarch64 ]; then
	ARCH_CFLAGS="-march=$MARCH -mgeneral-regs-only -mno-outline-atomics"
else
	ARCH_CFLAGS="-march=$MARCH -mgeneral-regs-only -mno-red-zone -mcmodel=kernel"
fi
CFLAGS="$ARCH_CFLAGS -O2 -g
 -ffreestanding -fno-builtin -fno-strict-aliasing -fno-delete-null-pointer-checks
 -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -ffunction-sections -fdata-sections -std=gnu11
 -Wall -Wno-unused -Wno-sign-compare -Wno-pointer-sign -Wno-format
 -Wno-address-of-packed-member -Wno-array-bounds -Wno-stringop-overflow
 -Wno-missing-braces -Wno-maybe-uninitialized -Wno-attributes
 -Wno-builtin-declaration-mismatch -Wno-zero-length-bounds -Wno-dangling-pointer
 -Werror=implicit-function-declaration -Werror=int-conversion
 -Werror=incompatible-pointer-types"
CPPFLAGS="-nostdinc -D_KERNEL -DIEEE80211_DEBUG -DIWM_DEBUG -DIWX_DEBUG
 -I$GEN/include -I$GEN/patched/sys -I$S"

stage_fetch() {
	if [ -f "$S/net80211/ieee80211.c" ] && [ -f "$S/dev/pci/if_iwm.c" ] \
	   && [ -f "$S/dev/pci/if_iwx.c" ] && [ -f "$S/lib/libkern/fls.c" ] \
	   && [ -d "$S/arch/$OB_MACHINE/include" ] && [ -f "$S/dev/sdmmc/sdhc.c" ] \
	   && [ -f "$S/dev/ic/bwfm.c" ] && [ -d "$S/scsi" ]; then
		echo "fetch: $OB is there"
		return
	fi
	if [ ! -f "$OB_DL" ] || ! echo "$OB_SHA256  $OB_DL" | sha256sum -c --status -; then
		local u got="" d
		mkdir -p "$(dirname "$OB_DL")"
		# fetched into a new empty directory, moved in once its sum is right
		d=$(mktemp -d "$(dirname "$OB_DL")/.dl.XXXXXX")
		for u in $OB_URLS; do
			echo "fetch: $u"
			if curl -fsSL -m 1800 -o "$d/sys.tar.gz" "$u" &&
			   echo "$OB_SHA256  $d/sys.tar.gz" | sha256sum -c --status -; then
				got=$u
				break
			fi
			rm -f "$d/sys.tar.gz"
		done
		if [ -z "$got" ]; then
			rm -rf "$d"
			echo "fetch: OpenBSD $OB_VER's sys.tar.gz could not be had with the right SHA-256" \
			     "(set OB_URL to a place that has it, or OB_DL to where it is)" >&2
			exit 1
		fi
		mv "$d/sys.tar.gz" "$OB_DL"
		rmdir "$d"
	fi
	echo "$OB_SHA256  $OB_DL" | sha256sum -c -
	mkdir -p "$OB"
	local pats="" p
	for p in $EXTRACT_DIRS; do
		pats="$pats $p/*"
	done
	# shellcheck disable=SC2086
	tar xzf "$OB_DL" -C "$OB" --exclude='*/CVS' \
		--wildcards --no-wildcards-match-slash --no-recursion $pats
	# shellcheck disable=SC2086
	tar xzf "$OB_DL" -C "$OB" --exclude='*/CVS' $EXTRACT_TREES
	echo "fetch: OpenBSD $OB_VER sources in $OB"
}

# The headers a kernel build would generate: the counts of the pseudo-devices,
# none of which is here; <machine/...> is the machine's
stage_gen() {
	local n
	mkdir -p "$GEN/include"
	ln -sfn "$S/arch/$OB_MACHINE/include" "$GEN/include/machine"
	for n in bpfilter bridge vlan carp pf pfsync trunk bwfm_sdio sdmmc; do
		echo "#define N$(echo $n | tr a-z A-Z) 0" > "$GEN/include/$n.h.tmp"
		cmp -s "$GEN/include/$n.h.tmp" "$GEN/include/$n.h" && rm -f "$GEN/include/$n.h.tmp" \
			|| mv "$GEN/include/$n.h.tmp" "$GEN/include/$n.h"
	done
	for h in $HDR_PATCHED; do
		# shellcheck disable=SC2046
		patched_copy "sys/$h" $(ls "$PATCHES"/"$(basename "$h" .h)"-*.patch) > /dev/null || exit 1
	done
}

# A source with its patches applied, as a copy under $GEN/patched: its directory
patched_copy() {
	local rel=$1 dst tmp p
	shift
	dst=$GEN/patched/$(dirname "$rel")
	mkdir -p "$dst"
	tmp=$(mktemp -d)
	mkdir -p "$tmp/$(dirname "$rel")"
	cp "$OB/$rel" "$tmp/$rel"
	for p in "$@"; do
		if ! patch -s -p1 -d "$tmp" --no-backup-if-mismatch < "$p" >&2; then
			echo "patch does not apply: $p" >&2
			rm -rf "$tmp"
			return 1
		fi
	done
	cmp -s "$tmp/$rel" "$dst/$(basename "$rel")" || cp "$tmp/$rel" "$dst/"
	rm -rf "$tmp"
	echo "$dst/$(basename "$rel")"
}

# The file to compile for sys/<rel>: the patched copy when there are patches for it
src_of() {
	local rel=$1 base pats
	base=$(basename "$rel" .c)
	pats=$(ls "$PATCHES"/"$base"-*.patch 2>/dev/null || true)
	if [ -n "$pats" ]; then
		# shellcheck disable=SC2086
		patched_copy "sys/$rel" $pats
	else
		echo "$S/$rel"
	fi
}

write_cc1() {
	cat > "$OUT/cc1.sh" <<'EOS'
#!/bin/bash
src=$1; obj=$2; shift 2
[ -f "$obj" ] && [ "$obj" -nt "$src" ] && [ ! "$obj" -ot "$OUT/cc1.sh" ] && exit 0
mkdir -p "$(dirname "$obj")"
cmd="$CC $CFLAGS $CPPFLAGS $* -MD -MF $obj.d -c $src -o $obj"
if ! $cmd 2> "$obj.err"; then
	{ echo "=== $src"; grep -m8 -E "error" "$obj.err"; } > "$obj.fail"
	rm -f "$obj"
else
	[ -s "$obj.err" ] || rm -f "$obj.err"
	rm -f "$obj.fail"
fi
EOS
	chmod +x "$OUT/cc1.sh"
}

stage_compile() {
	local f src n fail
	stage_gen
	write_cc1
	export CC CFLAGS CPPFLAGS OUT
	: > "$OUT/jobs.list"
	for f in $NET80211_SRCS; do
		src=$(src_of "net80211/$f") || exit 1
		echo "$src $OBJ/net80211/${f%.c}.o" >> "$OUT/jobs.list"
	done
	for f in $DRV_SRCS; do
		src=$(src_of "dev/pci/$f") || exit 1
		echo "$src $OBJ/drv/${f%.c}.o" >> "$OUT/jobs.list"
	done
	for f in $SDIO_SRCS; do
		src=$(src_of "$f") || exit 1
		echo "$src $OBJ/drv/$(basename "${f%.c}").o" >> "$OUT/jobs.list"
	done
	for f in $KERN_SRCS; do
		src=$(src_of "$f") || exit 1
		echo "$src $OBJ/kern/$(echo "${f%.c}" | tr / _).o" >> "$OUT/jobs.list"
	done
	for f in $COMP_SRCS; do
		echo "$COMP/$f $OBJ/comp/${f%.c}.o -I$COMP${WIFI_SIM:+ -DTSWIFI_SIM=$WIFI_SIM}" >> "$OUT/jobs.list"
	done
	if [ "$WIFI_SIM" = 1 ]; then
		echo "$TOP/device/net/wifi/sim/obsd_sim.c $OBJ/comp/obsd_sim.o -I$COMP -DTSWIFI_SIM=1" >> "$OUT/jobs.list"
	fi
	find "$OBJ" -name '*.fail' -delete 2>/dev/null || true
	xargs -P "$JOBS" -L 1 "$OUT/cc1.sh" < "$OUT/jobs.list"
	n=$(wc -l < "$OUT/jobs.list")
	fail=$(find "$OBJ" -name '*.fail' | wc -l)
	echo "compile: $n sources, $fail failed"
	if [ "$fail" -ne 0 ]; then
		find "$OBJ" -name '*.fail' -exec cat {} + | head -80
		exit 1
	fi
}

objects() {
	awk '{ print $2 }' "$OUT/jobs.list" | sort
}

# The names kept as they are: the entry points, the hypercalls, the
# compiler's run-time library
KEEP='^(rump_tswifi_|rump_tswifisim_|obsd_hc_|rumpcomp_|__)'

stage_link() {
	local tab
	# shellcheck disable=SC2046
	$LD -r -o "$OUT/obsd_all.o" \
		--wrap=ieee80211_end_scan --wrap=ieee80211_eapol_key_input --wrap=ieee80211_inputm --wrap=ieee80211_encap \
		--wrap=ieee80211_node_trigger_addba_req $(objects)
	# the wrapped names first back to plain ones, then everything into obsdns_
	tab=$OUT/rename.tab
	$NM -o "$OUT/obsd_all.o" | awk '{ print $NF }' | sort -u \
		| awk -v keep="$KEEP" '$0 !~ keep { print $0, "obsdns_" $0 }' > "$tab"
	$OBJCOPY --redefine-syms "$tab" "$OUT/obsd_all.o"
	rm -f "$tab"
	$NM -g --defined-only "$OUT/obsd_all.o" | awk '{ print $NF }' \
		| grep -E '^rump_tswifi(sim)?_' | sort -u > "$OUT/keep.syms"
	$OBJCOPY --keep-global-symbols="$OUT/keep.syms" "$OUT/obsd_all.o" "$OUT/obsdwifi.o"
	rm -f "$OUT/obsd_all.o"
	$NM -u "$OUT/obsdwifi.o" | awk '{ print $NF }' | sort -u > "$OUT/undef.syms"
	echo "link: $OUT/obsdwifi.o, $(wc -l < "$OUT/keep.syms") entry points," \
	     "$(wc -l < "$OUT/undef.syms") names from outside ($OUT/undef.syms)"
	if grep -vE '^(obsd_hc_|rumpcomp_|__)' "$OUT/undef.syms"; then
		echo "link: the names above are not given by anything" >&2
		exit 1
	fi
}

# The copyright notices and licence terms of every OpenBSD file that went
# into the object, the sources and the headers they include, each once
# (THIRD_PARTY_NOTICES.md)
stage_notices() {
	local list=$OUT/notice.files out=$OUT/NOTICE-openbsd.txt
	objects | grep -v '/comp/' | sed 's/$/.d/' | xargs cat | tr ' ' '\n' \
		| sed 's/^[[:space:]]*//' | grep -E "^($S|$GEN/patched)/" | grep -v ':$' \
		| sed "s|^$GEN/patched/sys/|$S/|" | sort -u > "$list"
	{
		echo "The OpenBSD $OB_VER sources compiled into the TessronOS Wi-Fi (WIFI_NET80211=obsd):"
		echo "their copyright notices and licence terms."
		echo
		# shellcheck disable=SC2046
		awk -v top="$OB/" '
			FNR == 1 { inb = 0; name = FILENAME; sub(top, "", name) }
			FNR > 200 { next }
			/^[ \t]*\/\*/ && !inb { inb = 1; blk = "" }
			inb { blk = blk $0 "\n" }
			inb && /\*\// {
				inb = 0
				if (blk ~ /[Cc]opyright|Redistribution|[Pp]ermission to use|[Pp]ublic domain/) {
					if (!(blk in seen)) {
						seen[blk] = 1
						printf "==== %s\n%s\n", name, blk
					}
				}
			}
		' $(cat "$list")
	} > "$out"
	echo "notices: $out, $(wc -l < "$list") files"
}

mkdir -p "$OUT"
case $STAGE in
fetch)		stage_fetch ;;
compile)	stage_compile ;;
link)		stage_link ;;
notices)	stage_notices ;;
all)		stage_fetch; stage_compile; stage_link; stage_notices ;;
clean)		rm -rf "$OBJ" "$GEN" "$OUT/obsdwifi.o" "$OUT/NOTICE-openbsd.txt" "$OUT/jobs.list" ;;
*)		echo "usage: $0 [fetch|compile|link|notices|all|clean]" >&2; exit 2 ;;
esac
