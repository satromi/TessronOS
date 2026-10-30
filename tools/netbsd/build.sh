#!/bin/bash
#
# build.sh -- the NetBSD TCP/IP stack for TessronOS, as rump kernel components
#
# Copyright (C) 2026 satromi
# This software is distributed under the T-License 2.2.
#
# Fetches the kernel sources of a pinned NetBSD release, keeps only the
# parts the network stack is made of, and compiles them with the bare
# aarch64 toolchain into one relocatable object the kernel links:
#
#	$OUT/rumpnet.o
#
# The object holds the rump kernel base (librump), its networking faction
# (librumpnet), interfaces and routing (librumpnet_net, which carries IPv4,
# IPv6 and MPLS as well), and TessronOS's interface component over the
# device "neta" (peripheral_kernel/network/netbsd/rumpcomp). Every symbol
# of NetBSD's is first put into the rumpns_ name space and then made
# local, so that the kernel sees only the rump_ entry points; what the
# object needs from outside is the hypercall interface (rumpuser_*,
# rumpcomp_*), which peripheral_kernel/network/netbsd supplies.
#
# The sources are not changed. The few files a kernel build generates
# (vers.c, ioconf.c and the machine header links) are made here. One
# function of theirs, ip_savecontrol, is wrapped at the link by one of
# the interface component's. if_arp.c (the probing of an IPv4 address
# before it is used, 9.3.1) and, with the Wi-Fi, bwfm.c are compiled
# from a copy with the patches of tools/netbsd/patches applied
# (patched_copy below; docs/tessronos/09-network.md 9.6).
#
# With WIFI=1 (the Raspberry Pi 5) the object also holds the Wi-Fi: the
# device faction of the rump kernel (librumpdev), NetBSD's SD/SDIO layer
# (sdmmc) and SD Host Controller driver (sdhc) for the Wi-Fi's own SDIO
# host, Broadcom's FullMAC driver bwfm(4) over them, the IEEE 802.11
# layer (net80211) with the AES of its CCMP, and TessronOS's component
# that attaches the host (peripheral_kernel/network/netbsd/rumpcomp/wifi).
# Its sources are kept apart (~/netbsd/netbsd-$NB_VER-wifi) from the
# other targets', and so is the object (~/tessronos-netbsd-wifi).
#
# Usage: [WIFI=1] build.sh [fetch|gen|compile|link|notices|all|clean]
#
set -e

STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
CC=$XB/aarch64-none-elf-gcc
LD=$XB/aarch64-none-elf-ld
OBJCOPY=$XB/aarch64-none-elf-objcopy
NM=$XB/aarch64-none-elf-nm
JOBS=${JOBS:-12}

# The release, where its sources are fetched from, and the checksum of
# the set NetBSD publishes for it (the SHA512 file of the release). A
# release a newer one has replaced is taken off the main mirror and kept
# on the archive, so each place is tried in turn (NB_URL first when given).
NB_VER=11.0
NB_URLS="${NB_URL:+$NB_URL }https://cdn.netbsd.org/pub/NetBSD/NetBSD-$NB_VER/source/sets/syssrc.tgz
 https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-$NB_VER/source/sets/syssrc.tgz
 https://ftp.netbsd.org/pub/NetBSD/NetBSD-$NB_VER/source/sets/syssrc.tgz"
NB_SHA512=318d451ecc83749607d5448198c02663ea5f0b7f4cc74fb565747f86eead7b66b5809a37deee219dc6ddb2e5ee332d1cbb94c1a4e9db63620c3abd7586fb3057
# When the set cannot be had whole from any of them (a server may hand
# out only its first megabytes), the part of it that is kept below -- the same
# files, as they are in the set, packed in name order with no owner or
# time of packing -- is taken instead: from this tree if it is here, else
# from the TessronOS release that carries it (NB_SUB_URL first when given)
NB_SUB=netbsd-$NB_VER-rumpsrc.tgz
NB_SUB_URLS="${NB_SUB_URL:+$NB_SUB_URL }https://github.com/satromi/TessronOS/releases/download/netbsd-$NB_VER-src/$NB_SUB"
NB_SUB_SHA512=69df51784bc88696219af3a6793c780f1edc051a5c98bc8edabc88efa88766be63559dbc9eb20c8c7c9947f381530c188e8dc8835ac50ed43af2784517295ba4
WIFI=${WIFI:-0}
# WIFI_SIM=1 (with WIFI=1): the Wi-Fi simulator's two radios (rumpcomp/wifisim/tswifi_sim.c,
# docs/tessronos/09-network.md 9.6.12) in the place of the card's driver and its bus: net80211,
# the AES of its CCMP and TessronOS's component of the card (tswifi_if.c), with no driver
WIFI_SIM=${WIFI_SIM:-0}
# The whole set, kept where it was fetched to once (tools/x64/netbsd too)
NB_DL=${NB_DL:-$HOME/netbsd/dl/syssrc.tgz}
if [ "$WIFI" = 1 ]; then
	NB=${NBSRC:-$HOME/netbsd/netbsd-$NB_VER-wifi}
	OUT=${OUT:-$HOME/tessronos-netbsd-wifi}
else
	NB=${NBSRC:-$HOME/netbsd/netbsd-$NB_VER}
	OUT=${OUT:-$HOME/tessronos-netbsd}
fi
# The instruction set the object is built for: the kernel's (common.mk).
# The Raspberry Pi 4 family takes ARMv8.0-A with CRC32 (MARCH=armv8-a+crc,
# OUT=~/tessronos-netbsd-v80), which has no LSE atomics
MARCH=${MARCH:-armv8.2-a}
S=$NB/sys
C=$NB/common
R=$S/rump
COMP=$TOP/peripheral_kernel/network/netbsd/rumpcomp
GEN=$OUT/gen
OBJ=$OUT/obj

# The parts of the source set that are kept: the files of these
# directories (not what is below them) -- what the stack's sources and
# the headers they include come from, as the compiler lists them -- and
# the machine's headers whole
EXTRACT_DIRS="common/include/libc common/include/ppath common/include/prop
 common/lib/libc/arch/aarch64/atomic common/lib/libc/arch/aarch64/gen
 common/lib/libc/arch/aarch64/string common/lib/libc/atomic common/lib/libc/cdb
 common/lib/libc/gen common/lib/libc/hash/murmurhash common/lib/libc/hash/rmd160
 common/lib/libc/hash/sha1 common/lib/libc/hash/sha2 common/lib/libc/hash/sha3
 common/lib/libc/inet common/lib/libc/md common/lib/libc/stdlib
 common/lib/libc/string common/lib/libc/sys common/lib/libppath
 common/lib/libprop common/lib/libutil
 sys/altq sys/compat/common sys/compat/net sys/compat/netinet6 sys/compat/sys
 sys/conf sys/crypto/blake2 sys/crypto/chacha sys/crypto/cprng_fast
 sys/crypto/nist_hash_drbg sys/dev sys/dev/mii sys/kern sys/lib/libkern
 sys/net sys/net/agr sys/net80211 sys/netatalk sys/netinet
 sys/netinet6 sys/netipsec sys/netmpls sys/rump/include/machine
 sys/rump/include/opt sys/rump/include/rump sys/rump/include/rump-sys
 sys/rump/include/sys sys/rump/librump/rumpkern
 sys/rump/librump/rumpkern/arch/generic sys/rump/librump/rumpnet
 sys/rump/net/lib/libnet sys/rump/net/lib/libnetinet
 sys/rump/net/lib/libnetinet6 sys/secmodel sys/secmodel/extensions
 sys/secmodel/suser sys/sys sys/ufs/ufs sys/uvm"
EXTRACT_TREES="sys/arch/aarch64/include sys/arch/arm/include sys/arch/evbarm/include"

# What the Wi-Fi takes besides (WIFI=1): the files of these directories,
# and these files
WIFI_COMP=$TOP/peripheral_kernel/network/netbsd/rumpcomp/wifi
WIFI_EXTRACT_DIRS="sys/dev/sdmmc sys/crypto/aes sys/rump/librump/rumpdev"
WIFI_EXTRACT_FILES="sys/dev/ic/bwfm.c sys/dev/ic/bwfmreg.h sys/dev/ic/bwfmvar.h
 sys/dev/pcmcia/pcmciareg.h sys/dev/ic/wi_ieee.h"

# The kept part of the set (NB_SUB): from this tree, else from where it is published
fetch_sub() {
	local sub u got=""
	sub=$(dirname "$NB")/$NB_SUB
	if [ -f "$TOP/tools/netbsd/$NB_SUB" ]; then
		cp "$TOP/tools/netbsd/$NB_SUB" "$sub"
		got=$TOP/tools/netbsd/$NB_SUB
	else
		for u in $NB_SUB_URLS; do
			echo "fetch: $u"
			if curl -fsSL -o "$sub.part" "$u"; then
				mv "$sub.part" "$sub"
				got=$u
				break
			fi
			rm -f "$sub.part"
		done
	fi
	if [ -z "$got" ]; then
		echo "fetch: neither NetBSD $NB_VER's syssrc.tgz nor its kept part $NB_SUB could be had" \
		     "(set NB_URL to a place that has the set, or NB_SUB_URL to one that has the part)" >&2
		exit 1
	fi
	echo "$NB_SUB_SHA512  $sub" | sha512sum -c -
	mkdir -p "$NB"
	tar xzf "$sub" -C "$NB" --strip-components=1
	rm -f "$sub"
	echo "fetch: NetBSD $NB_VER sources in $NB (the kept part, from $got)"
}

stage_fetch() {
	if [ -d "$S/rump" ]; then
		echo "fetch: $NB is there"
		return
	fi
	mkdir -p "$(dirname "$NB")"
	local tgz keep=0
	tgz=$(dirname "$NB")/syssrc-$NB_VER.tgz
	# the Wi-Fi's tree is made from the set kept in NB_DL when it is there
	if [ "$WIFI" = 1 ] && [ -f "$NB_DL" ]; then
		tgz=$NB_DL
		keep=1
	fi
	if [ ! -f "$tgz" ]; then
		local u got="" rc n
		for u in $NB_URLS; do
			echo "fetch: $u"
			rm -f "$tgz.part"
			# a server that closes the connection part way (the archive
			# gives a megabyte at a time) is asked again from where it
			# stopped: curl says 18 for a transfer cut short
			n=0
			while :; do
				rc=0
				curl -fsSL -C - -o "$tgz.part" "$u" || rc=$?
				[ "$rc" -eq 18 ] && [ "$n" -lt 400 ] || break
				n=$((n + 1))
			done
			if [ "$rc" -eq 0 ]; then
				got=$u
				break
			fi
			rm -f "$tgz.part"
		done
		if [ -z "$got" ]; then
			echo "fetch: NetBSD $NB_VER's syssrc.tgz could not be had whole; its kept part instead"
			fetch_sub
			return
		fi
		mv "$tgz.part" "$tgz"
	fi
	echo "$NB_SHA512  $tgz" | sha512sum -c -
	mkdir -p "$NB"
	local pats="" p
	for p in $EXTRACT_DIRS; do
		pats="$pats usr/src/$p/*"
	done
	# shellcheck disable=SC2086
	tar xzf "$tgz" -C "$NB" --strip-components=2 --exclude='*/CVS' \
		--wildcards --no-wildcards-match-slash --no-recursion $pats
	pats=""
	for p in $EXTRACT_TREES; do
		pats="$pats usr/src/$p"
	done
	# shellcheck disable=SC2086
	tar xzf "$tgz" -C "$NB" --strip-components=2 --exclude='*/CVS' $pats
	[ "$keep" = 1 ] || rm -f "$tgz"
	echo "fetch: NetBSD $NB_VER sources in $NB"
}

# The Wi-Fi's sources (WIFI=1), from the set NB_DL or fetched into it
stage_fetch_wifi() {
	[ "$WIFI" = 1 ] || return 0
	if [ -f "$S/dev/sdmmc/sdhc.c" ] && [ -f "$S/dev/ic/bwfm.c" ] \
	   && [ -f "$S/rump/librump/rumpdev/rump_dev.c" ] && [ -f "$S/dev/ic/wi_ieee.h" ]; then
		return 0
	fi
	local tgz=$NB_DL pats="" p u rc n
	if [ ! -f "$tgz" ]; then
		mkdir -p "$(dirname "$tgz")"
		for u in $NB_URLS; do
			echo "fetch: $u"
			rm -f "$tgz.part"
			n=0
			while :; do
				rc=0
				curl -fsSL -C - -o "$tgz.part" "$u" || rc=$?
				[ "$rc" -eq 18 ] && [ "$n" -lt 400 ] || break
				n=$((n + 1))
			done
			[ "$rc" -eq 0 ] && { mv "$tgz.part" "$tgz"; break; }
			rm -f "$tgz.part"
		done
		if [ ! -f "$tgz" ]; then
			echo "fetch: the Wi-Fi's sources need NetBSD $NB_VER's syssrc.tgz in $NB_DL, which could" \
			     "not be had (set NB_URL to a place that has it, or NB_DL to where it is)" >&2
			exit 1
		fi
	fi
	echo "$NB_SHA512  $tgz" | sha512sum -c - || { echo "fetch: $tgz is not NetBSD $NB_VER's syssrc.tgz" >&2; exit 1; }
	for p in $WIFI_EXTRACT_DIRS; do
		pats="$pats usr/src/$p/*"
	done
	for p in $WIFI_EXTRACT_FILES; do
		pats="$pats usr/src/$p"
	done
	# shellcheck disable=SC2086
	tar xzf "$tgz" -C "$NB" --strip-components=2 --exclude='*/CVS' \
		--wildcards --no-wildcards-match-slash --no-recursion $pats
	echo "fetch: the Wi-Fi's sources in $NB"
}

# The Wi-Fi's autoconfiguration (WIFI=1), as config(1) would make it of
# "tssdhc* at mainbus?" (TessronOS's host, rumpcomp/wifi/tswifi_sdio.c),
# "sdmmc* at sdmmcbus?" and "bwfm* at sdmmc?" (dev/sdmmc/files.sdmmc):
# the root bus gets the interface attribute the host's device attaches
# with, and the component includes the table of the three
gen_wifi() {
	local f=$GEN/rumpkern/ioconf.c
	sed -i 's|^static const struct cfiattrdata \* const mainbus_attrs\[\] = {$|static const struct cfiattrdata tssdhcbuscf_iattrdata = {\n\t"tssdhcbus", 0, {\n\t\t{ NULL, NULL, 0 },\n\t}\n};\n\n&|' "$f"
	sed -i 's|^\t&mainbuscf_iattrdata, NULL$|\t\&mainbuscf_iattrdata, \&tssdhcbuscf_iattrdata, NULL|' "$f"
	grep -q tssdhcbuscf_iattrdata, "$f" || { echo "gen: the root bus's table did not take the SDIO host" >&2; exit 1; }
	mkdir -p "$GEN/wifi/dev/ofw"
	cat > "$GEN/wifi/ioconf.c" <<'EOF'
/* The autoconfiguration data of the Wi-Fi's SDIO host, the SD/SDIO bus and the Wi-Fi under the root bus. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/mount.h>

static const struct cfiattrdata sdmmcbuscf_iattrdata = {
	"sdmmcbus", 0, {
		{ NULL, NULL, 0 },
	}
};
static const struct cfiattrdata sdmmccf_iattrdata = {
	"sdmmc", 0, {
		{ NULL, NULL, 0 },
	}
};

static const struct cfiattrdata * const tssdhc_attrs[] = {
	&sdmmcbuscf_iattrdata, NULL
};
static const struct cfiattrdata * const sdmmc_attrs[] = {
	&sdmmccf_iattrdata, NULL
};
CFDRIVER_DECL(tssdhc, DV_DULL, tssdhc_attrs);
CFDRIVER_DECL(sdmmc, DV_DULL, sdmmc_attrs);
CFDRIVER_DECL(bwfm, DV_IFNET, NULL);

static struct cfdriver * const cfdriver_ioconf_tssdhc[] = {
	&tssdhc_cd,
	&sdmmc_cd,
	&bwfm_cd,
	NULL
};

extern struct cfattach tssdhc_ca;
extern struct cfattach sdmmc_ca;
extern struct cfattach bwfm_sdio_ca;

static struct cfattach * const tssdhc_cfattachinit[] = {
	&tssdhc_ca, NULL
};
static struct cfattach * const sdmmc_cfattachinit[] = {
	&sdmmc_ca, NULL
};
static struct cfattach * const bwfm_cfattachinit[] = {
	&bwfm_sdio_ca, NULL
};

static const struct cfattachinit cfattach_ioconf_tssdhc[] = {
	{ "tssdhc", tssdhc_cfattachinit },
	{ "sdmmc", sdmmc_cfattachinit },
	{ "bwfm", bwfm_cfattachinit },
	{ NULL, NULL }
};

static const struct cfparent pspec_tssdhc = { "tssdhcbus", "mainbus", DVUNIT_ANY };
static const struct cfparent pspec_sdmmc = { "sdmmcbus", "tssdhc", DVUNIT_ANY };
static const struct cfparent pspec_bwfm = { "sdmmc", "sdmmc", DVUNIT_ANY };

#define NORM FSTATE_NOTFOUND
#define STAR FSTATE_STAR

static struct cfdata cfdata_ioconf_tssdhc[] = {
	/* driver	attachment	unit state	loc	flags	pspec */
	{ "tssdhc",	"tssdhc",	0, STAR,	NULL,	0,	&pspec_tssdhc },
	{ "sdmmc",	"sdmmc",	0, STAR,	NULL,	0,	&pspec_sdmmc },
	{ "bwfm",	"bwfm_sdio",	0, STAR,	NULL,	0,	&pspec_bwfm },
	{ NULL,		NULL,		0, 0,		NULL,	0,	NULL }
};
EOF
	# the options of sdmmc and of the device tree, none of them on: the
	# board's model is not looked up (bwfm then asks for the firmware by
	# the chip's names alone)
	: > "$GEN/wifi/opt_sdmmc.h"
	: > "$GEN/wifi/opt_fdt.h"
	# what if_bwfm_sdio.c includes of Open Firmware whether or not the
	# device tree is there: nothing of it is used without FDT
	cat > "$GEN/wifi/dev/ofw/openfirm.h" <<'EOF'
/* Open Firmware, which the rump kernel of the Wi-Fi has none of. */
EOF
}

stage_gen() {
	mkdir -p "$GEN/include" "$GEN/rumpkern"
	# <machine/...> and the architecture's own directories
	ln -sfn "$S/arch/evbarm/include" "$GEN/include/machine"
	ln -sfn "$S/arch/evbarm/include" "$GEN/include/evbarm"
	ln -sfn "$S/arch/aarch64/include" "$GEN/include/aarch64"
	ln -sfn "$S/arch/arm/include" "$GEN/include/arm"

	# The release's name and the copyright notice of the sources
	{
		echo '#include <sys/cdefs.h>'
		echo '#include <sys/types.h>'
		echo "const char ostype[] = \"NetBSD\";"
		echo "const char osrelease[] = \"$NB_VER\";"
		echo "const char sccs[] = \"@(#)NetBSD $NB_VER (TessronOS rump)\";"
		echo "const char version[] = \"NetBSD $NB_VER (TessronOS rump)\\n\";"
		echo 'const char buildinfo[] = "";'
		echo 'const char kernel_ident[] = "TESSRONOS";'
		printf 'const char copyright[] =\n'
		sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$/\\n"/' "$S/conf/copyright"
		echo '"\n";'
	} > "$GEN/rumpkern/vers.c"

	# The autoconfiguration table of the rump kernel's one bus,
	# "mainbus0 at root", which rump_autoconf.c includes
	cat > "$GEN/rumpkern/ioconf.c" <<'EOF'
/* The autoconfiguration data of the rump kernel's root bus. */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/mount.h>

static const struct cfiattrdata mainbuscf_iattrdata = {
	"mainbus", 0, {
		{ NULL, NULL, 0 },
	}
};

static const struct cfiattrdata * const mainbus_attrs[] = {
	&mainbuscf_iattrdata, NULL
};
CFDRIVER_DECL(mainbus, DV_DULL, mainbus_attrs);

static struct cfdriver * const cfdriver_ioconf_mainbus[] = {
	&mainbus_cd,
	NULL
};

extern struct cfattach mainbus_ca;

static struct cfattach * const mainbus_cfattachinit[] = {
	&mainbus_ca, NULL
};

static const struct cfattachinit cfattach_ioconf_mainbus[] = {
	{ "mainbus", mainbus_cfattachinit },
	{ NULL, NULL }
};

#define NORM FSTATE_NOTFOUND
#define STAR FSTATE_STAR

static struct cfdata cfdata_ioconf_mainbus[] = {
	/* driver	attachment	unit state	loc	flags	pspec */
	{ "mainbus",	"mainbus",	0, NORM,	NULL,	0,	NULL },
	{ NULL,		NULL,		0, 0,		NULL,	0,	NULL }
};
EOF
	# The attach functions of the pseudo-devices of librumpnet_net
	mkdir -p "$GEN/net"
	cat > "$GEN/net/ioconf.h" <<'EOF'
/* The attach functions of the network's pseudo-devices. */
void	carpattach(int);
void	mplsattach(int);
EOF
	[ "$WIFI" = 1 ] && [ "$WIFI_SIM" != 1 ] && gen_wifi
	echo "gen: done"
}

# ---------------------------------------------------------------- sources
#
# Each list names the sources of one rump kernel library of NetBSD
# $NB_VER, built with the options below, and the directories they are in.
# A name is looked for in those directories in order; an assembler version
# in the machine's directory is taken before the C one.

# librump: locks for several processors, no KTRACE, no compatibility
# with earlier NetBSD releases
RUMPKERN_PATH="$R/librump/rumpkern $R/librump/rumpkern/arch/generic $S/kern
 $S/uvm $S/conf $S/dev $S/crypto/blake2 $S/crypto/chacha $S/crypto/cprng_fast
 $S/crypto/nist_hash_drbg $S/secmodel $S/secmodel/extensions
 $S/secmodel/suser $S/compat/common $GEN/rumpkern"
RUMPKERN_SRCS="rump.c rumpcopy.c cons.c emul.c etfs_wrap.c intr.c
 lwproc.c klock.c kobj_rename.c ltsleep.c scheduler.c
 signals.c sleepq.c threads.c vm.c hyperentropy.c accessors.c
 rump_autoconf.c rumpkern_syscalls.c locks.c vers.c
 rumpkern_if_wrappers.c devsw.c
 init_sysctl_base.c compat_stub.c kern_auth.c kern_cfglock.c kern_clock.c
 kern_descrip.c kern_entropy.c kern_event.c kern_hook.c kern_ksyms.c
 kern_malloc.c kern_module.c kern_module_hook.c kern_mutex_obj.c
 kern_ntptime.c kern_proc.c kern_prot.c kern_rate.c kern_reboot.c
 kern_resource.c kern_rwlock_obj.c kern_scdebug.c kern_stub.c kern_ssp.c
 kern_syscall.c kern_sysctl.c kern_tc.c kern_threadpool.c kern_time.c
 kern_timeout.c kern_uidinfo.c param.c subr_autoconf.c subr_callback.c
 subr_copy.c subr_cprng.c subr_cpu.c subr_device.c subr_devsw.c
 subr_evcnt.c subr_extent.c subr_hash.c subr_humanize.c subr_iostat.c
 subr_kcpuset.c subr_kmem.c subr_kobj.c subr_localcount.c subr_log.c
 subr_lwp_specificdata.c subr_once.c subr_pcq.c subr_percpu.c subr_pool.c
 subr_prf.c subr_pserialize.c subr_psref.c subr_specificdata.c subr_time.c
 subr_time_arith.c subr_thmap.c subr_vmem.c subr_workqueue.c subr_xcall.c
 sys_descrip.c sys_generic.c sys_getrandom.c sys_module.c sys_pipe.c sys_select.c
 syscalls.c uipc_sem.c
 uvm_aobj.c uvm_readahead.c uvm_object.c uvm_swapstub.c
 uvm_page_array.c uvm_page_status.c
 secmodel.c secmodel_suser.c secmodel_extensions.c secmodel_extensions_rump.c
 clock_subr.c
 nist_hash_drbg.c cprng_fast.c chacha_impl.c chacha_ref.c chacha_selftest.c
 blake2s.c
 rump_generic_cpu.c rump_generic_kobj.c rump_generic_pmap.c
 rump_generic_abi.c rump_generic_directmap.c"
# rump_syscalls.c is generated into the rump name space already
RUMPKERN_NORENAME="rump_syscalls.c"

# libkern and the common library, for aarch64
LIBKERN_PATH="$C/lib/libc/arch/aarch64/string $C/lib/libc/arch/aarch64/gen
 $C/lib/libc/arch/aarch64/atomic $S/lib/libkern $C/lib/libc/atomic
 $C/lib/libc/gen $C/lib/libc/inet $C/lib/libc/md $C/lib/libc/misc
 $C/lib/libc/net $C/lib/libc/stdlib $C/lib/libc/string $C/lib/libc/sys
 $C/lib/libc/hash/sha1 $C/lib/libc/hash/sha2 $C/lib/libc/hash/sha3
 $C/lib/libc/hash/rmd160 $C/lib/libc/hash/murmurhash $C/lib/libc/cdb
 $C/lib/libutil $C/lib/libprop $C/lib/libppath"
LIBKERN_SRCS="byte_swap_2.S byte_swap_4.S byte_swap_8.S memcmp.S memcpy.S
 memmove.S memset.S
 atomic_add_8.S atomic_add_16.S atomic_add_32.S atomic_add_64.S
 atomic_and_8.S atomic_and_16.S atomic_and_32.S atomic_and_64.S
 atomic_cas_8.S atomic_cas_16.S atomic_cas_32.S atomic_cas_64.S
 atomic_nand_8.S atomic_nand_16.S atomic_nand_32.S atomic_nand_64.S
 atomic_or_8.S atomic_or_16.S atomic_or_32.S atomic_or_64.S
 atomic_sub_8.S atomic_sub_16.S atomic_sub_32.S atomic_sub_64.S
 atomic_swap_8.S atomic_swap_16.S atomic_swap_32.S atomic_swap_64.S
 atomic_xor_8.S atomic_xor_16.S atomic_xor_32.S atomic_xor_64.S
 atomic_dec_32.S atomic_dec_64.S atomic_inc_32.S atomic_inc_64.S
 membar_ops.S atomic_init_cas.c
 snprintb.c proc_compare.c getfstypename.c
 prop_array.c prop_array_util.c prop_bool.c prop_data.c
 prop_dictionary.c prop_dictionary_util.c prop_extern.c prop_ingest.c
 prop_intern.c prop_kern.c prop_number.c prop_object.c prop_stack.c prop_string.c
 ppath.c ppath_extant.c
 kern_assert.c __main.c cpuset.c inet_addr.c intoa.c
 md4c.c md5c.c rmd160.c sha1.c sha2.c sha3.c keccak.c murmurhash.c
 pmatch.c crc32.c strlist.c ppath_kmem_alloc.c copystr.c
 strsep.c strstr.c strlcpy.c strlcat.c
 imax.c imin.c lmax.c lmin.c uimax.c uimin.c ulmax.c ulmin.c
 strchr.c strrchr.c memmem.c
 popcount32.c popcount64.c
 strtoul.c strtoll.c strtoull.c strtoimax.c strtoumax.c strtoi.c strtou.c
 strnvisx.c scanc.c skpc.c random.c rngtest.c memchr.c
 strcat.c strcmp.c strcpy.c strcspn.c strlen.c strnlen.c
 strncat.c strncmp.c strncpy.c strpbrk.c strspn.c
 strcasecmp.c strncasecmp.c xlat_mbr_fstype.c
 heapsort.c ptree.c radixtree.c rb.c rpst.c hexdump.c
 explicit_memset.c consttime_memequal.c entpool.c dkcksum.c
 disklabel_swap.c cdbr.c mi_vector_hash.c"

# librumpnet: sockets, mbufs and packet queues
RUMPNET_PATH="$R/librump/rumpnet $S/kern $S/net $S/netatalk $S/netinet
 $S/netinet6 $S/netipsec $S/compat/common"
RUMPNET_SRCS="net_stub.c rump_net.c rumpnet_if_wrappers.c rumpnet_syscalls.c
 sys_socket.c uipc_accf.c uipc_domain.c uipc_mbuf.c uipc_socket.c
 uipc_socket2.c uipc_syscalls.c
 pktqueue.c pfil.c rss_config.c toeplitz.c
 at_print.c dl_print.c in_print.c in6_print.c
 radix.c rtbl.c bpf_stub.c wqinput.c"

# librumpnet_net: interfaces, routing, and the IPv4, IPv6 and MPLS
# protocols, with the component files of netinet and netinet6
NET_PATH="$R/net/lib/libnet $R/net/lib/libnetinet $R/net/lib/libnetinet6
 $S/net $S/netinet $S/netinet6 $S/netmpls $S/compat/common"
NET_SRCS="if.c if_loop.c if_stats.c route.c rtsock.c raw_usrreq.c
 raw_cb.c if_media.c link_proto.c net_stats.c if_ethersubr.c
 if_spppsubr.c if_43.c if_llatbl.c nd.c net_component.c ether_sw_offload.c
 in_proto.c igmp.c in.c in_offload.c in_pcb.c ip_carp.c ip_icmp.c
 ip_flow.c ip_input.c ip_reass.c ip_output.c raw_ip.c
 in_cksum.c cpu_in_cksum.c in4_cksum.c ip_encap.c portalgo.c
 if_arp.c tcp_congctl.c tcp_input.c tcp_output.c tcp_sack.c tcp_subr.c
 tcp_syncache.c tcp_timer.c tcp_usrreq.c tcp_vtw.c udp_usrreq.c ip_ecn.c
 dest6.c frag6.c icmp6.c in6.c in6_cksum.c in6_ifattach.c
 in6_offload.c in6_pcb.c in6_proto.c in6_src.c ip6_flow.c
 ip6_forward.c ip6_input.c ip6_mroute.c ip6_output.c
 mld6.c nd6.c nd6_nbr.c nd6_rtr.c raw_ip6.c route6.c scope6.c
 udp6_usrreq.c
 mpls_ttl.c if_mpls.c mpls_proto.c
 netinet_component.c netinet6_component.c"

# TessronOS's interface component over the device "neta"
NETA_PATH="$COMP"
NETA_SRCS="if_tsneta.c tsnet_conf.c"

# The Wi-Fi (WIFI=1). librumpdev: the device faction, which brings the
# root bus up and attaches what is under it, and the power management
# hooks drivers register with
RUMPDEV_PATH="$R/librump/rumpdev $S/kern"
RUMPDEV_SRCS="rump_dev.c kern_pmf.c"

# The SD/SDIO layer and the SD Host Controller driver (dev/sdmmc/files.sdmmc)
SDMMC_PATH="$S/dev/sdmmc"
SDMMC_SRCS="sdmmc.c sdmmc_cis.c sdmmc_io.c sdmmc_mem.c sdhc.c"

# net80211 as librumpnet_net80211 builds it (rump/net/lib/libnet80211),
# with the AES its CCMP uses
WLAN_PATH="$S/net80211 $S/crypto/aes"
WLAN_SRCS="ieee80211.c ieee80211_amrr.c ieee80211_crypto.c
 ieee80211_crypto_ccmp.c ieee80211_crypto_none.c ieee80211_crypto_tkip.c
 ieee80211_crypto_wep.c ieee80211_input.c ieee80211_ioctl.c
 ieee80211_netbsd.c ieee80211_node.c ieee80211_output.c ieee80211_proto.c
 ieee80211_rssadapt.c ieee80211_xauth.c
 aes_impl.c aes_bear.c aes_ct.c aes_ct_dec.c aes_ct_enc.c aes_ccm.c
 aes_ccm_mbuf.c aes_selftest.c"

# Broadcom's FullMAC chips and their SDIO attachment
BWFM_PATH="$S/dev/ic $S/dev/sdmmc"
BWFM_SRCS="bwfm.c if_bwfm_sdio.c"

# TessronOS's component over them: the host's device, its bus_space and
# bus_dma, its interrupt and the firmware (tswifi_sdio.c), and the
# Wi-Fi's interface to the manager (tswifi_if.c, as on the x64 PC)
TSWIFI_PATH="$WIFI_COMP"
TSWIFI_SRCS="tswifi_sdio.c tswifi_if.c"

# ---------------------------------------------------------------- flags

# As the kernel is built (common.mk), without the floating point
# registers, which kernel tasks do not have; with the rump kernel's own
# options (opt_rumpkernel.h)
CFLAGS="-march=$MARCH -mgeneral-regs-only -mno-outline-atomics -O2 -g
 -ffreestanding -fno-strict-aliasing -fno-delete-null-pointer-checks
 -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -ffunction-sections -fdata-sections -std=gnu11
 -Wall -Wno-unused -Wno-sign-compare -Wno-pointer-sign
 -Wno-format-zero-length -Wno-address-of-packed-member -Wno-cast-function-type
 -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread
 -Wno-missing-braces -Wno-maybe-uninitialized -Wno-dangling-pointer
 -Wno-zero-length-bounds -Wno-attributes -Wno-address -Wno-enum-int-mismatch
 -Wno-use-after-free -Wno-int-in-bool-context -Wno-builtin-declaration-mismatch
 -Werror=implicit-function-declaration -Werror=int-conversion
 -Werror=incompatible-pointer-types"
CPPFLAGS="-nostdinc -D_RUMPKERNEL -DDIAGNOSTIC
 -DINET -DINET6 -DPORTALGO_INET4_DEFAULT=PORTALGO_RANDOM_START
 -DPORTALGO_INET6_DEFAULT=PORTALGO_RANDOM_START
 -imacros $R/include/opt/opt_rumpkernel.h
 -I$R/include -I$R/librump/rumpkern -I$R/librump/rumpnet
 -I$GEN/include -I$C/include -I$R/include/opt -I$S/arch -I$S
 -I$S/lib/libkern -I$C/lib/libc/quad -I$C/lib/libc/string
 -I$C/lib/libc/arch/aarch64/string -I$C/lib/libc/arch/aarch64/atomic
 -I$C/lib/libc/hash/sha3 -I$C/lib/libprop"
ASFLAGS="-march=$MARCH -D_LOCORE -D_KERNEL"

export CC CFLAGS CPPFLAGS ASFLAGS

# The file of a name, from the first directory of the list that has it
find_src() {
	local name=$1 dirs=$2 d base
	base=${name%.*}
	case $name in
	*.c)
		for d in $dirs; do
			[ -f "$d/$base.S" ] && case $d in */arch/*) echo "$d/$base.S"; return ;; esac
		done ;;
	esac
	for d in $dirs; do
		[ -f "$d/$name" ] && { echo "$d/$name"; return; }
	done
	return 1
}

# A source with the patches of $PATCHES for it applied, as a copy under
# $GEN/patched (the same path below it): "<path under $NB> <patches...>"
# says the copy's directory. The copy is written again only when what it
# holds changes, so that its object is not made again for nothing. A
# patch that does not apply stops the build.
PATCHES=$TOP/tools/netbsd/patches

patched_copy() {
	local rel=$1 dst tmp p
	shift
	dst=$GEN/patched/$(dirname "$rel")
	mkdir -p "$dst"
	tmp=$(mktemp -d)
	mkdir -p "$tmp/$(dirname "$rel")"
	cp "$NB/$rel" "$tmp/$rel"
	for p in "$@"; do
		if ! patch -s -p1 -d "$tmp" --no-backup-if-mismatch < "$p" >&2; then
			echo "patch does not apply: $p" >&2
			rm -rf "$tmp"
			return 1
		fi
	done
	cmp -s "$tmp/$rel" "$dst/$(basename "$rel")" || cp "$tmp/$rel" "$dst/"
	rm -rf "$tmp"
	echo "$dst"
}

# One compile job: "<source> <object> <extra flags...>"
write_cc1() {
	cat > "$OUT/cc1.sh" <<'EOS'
#!/bin/bash
src=$1; obj=$2; shift 2
[ -f "$obj" ] && [ "$obj" -nt "$src" ] && [ ! "$obj" -ot "$OUT/cc1.sh" ] && exit 0
mkdir -p "$(dirname "$obj")"
case $src in
*.S)	cmd="$CC $ASFLAGS $CPPFLAGS $* -MD -MF $obj.d -c $src -o $obj" ;;
*)	cmd="$CC $CFLAGS $CPPFLAGS $* -MD -MF $obj.d -c $src -o $obj" ;;
esac
if ! $cmd 2> "$obj.err"; then
	{ echo "=== $src"; grep -m5 -E "error" "$obj.err"; } > "$obj.fail"
	rm -f "$obj"
else
	[ -s "$obj.err" ] || rm -f "$obj.err"
	rm -f "$obj.fail"
fi
EOS
	chmod +x "$OUT/cc1.sh"
}

# Jobs of one component: its objects go to $OBJ/<name>/
add_jobs() {
	local name=$1 path=$2 srcs=$3 flags=$4 f src
	mkdir -p "$OBJ/$name"
	for f in $srcs; do
		if ! src=$(find_src "$f" "$path"); then
			echo "missing source: $f ($name)" >&2
			MISSING=1
			continue
		fi
		echo "$src $OBJ/$name/${f%.*}.o${flags:+ $flags}" >> "$OUT/jobs.list"
	done
}

stage_compile() {
	write_cc1
	export OUT
	: > "$OUT/jobs.list"
	MISSING=0
	add_jobs rumpkern "$RUMPKERN_PATH" "$RUMPKERN_SRCS $RUMPKERN_NORENAME" "-I$GEN/rumpkern"
	# the C library's internal declarations, which libkern's sources share
	add_jobs libkern "$LIBKERN_PATH" "$LIBKERN_SRCS" "-I$C/include/libc"
	# net_stub.c holds tentative definitions of what if.c defines
	add_jobs rumpnet "$RUMPNET_PATH" "$RUMPNET_SRCS" "-fcommon"
	# if_arp.c from its patched copy, the rest where it is
	local arp_path
	arp_path=$(patched_copy sys/netinet/if_arp.c "$PATCHES"/if_arp-*.patch) || exit 1
	add_jobs net "$arp_path $NET_PATH" "$NET_SRCS" "-I$GEN/net"
	add_jobs neta "$NETA_PATH" "$NETA_SRCS"
	if [ "$WIFI" = 1 ] && [ "$WIFI_SIM" = 1 ]; then
		# the station is bwfm0, as the chip (tswifi_if.c)
		add_jobs wlan "$WLAN_PATH" "$WLAN_SRCS" "-DIEEE80211_DEBUG"
		add_jobs tswifi "$TSWIFI_PATH" "tswifi_if.c" "-DIEEE80211_DEBUG"
		add_jobs wifisim "$COMP/wifisim" "tswifi_sim.c" "-DIEEE80211_DEBUG"
	elif [ "$WIFI" = 1 ]; then
		add_jobs rumpdev "$RUMPDEV_PATH" "$RUMPDEV_SRCS"
		add_jobs sdmmc "$SDMMC_PATH" "$SDMMC_SRCS" "-I$GEN/wifi"
		# what net80211 and bwfm say of each step goes to the console
		# while the manager wants it (rump_tswifi_debug)
		add_jobs wlan "$WLAN_PATH" "$WLAN_SRCS" "-DIEEE80211_DEBUG"
		# bwfm.c from its patched copy, the rest of bwfm where it is
		local bwfm_path
		bwfm_path=$(patched_copy sys/dev/ic/bwfm.c "$PATCHES"/bwfm-*.patch) || exit 1
		add_jobs bwfm "$bwfm_path $BWFM_PATH" "$BWFM_SRCS" "-I$GEN/wifi -DIEEE80211_DEBUG"
		add_jobs tswifi "$TSWIFI_PATH" "$TSWIFI_SRCS" "-I$GEN/wifi -DIEEE80211_DEBUG"
	fi
	[ "$MISSING" = 0 ] || exit 1
	find "$OBJ" -name '*.fail' -delete
	xargs -P "$JOBS" -L 1 "$OUT/cc1.sh" < "$OUT/jobs.list"
	local n fail
	n=$(wc -l < "$OUT/jobs.list")
	fail=$(find "$OBJ" -name '*.fail' | wc -l)
	echo "compile: $n sources, $fail failed"
	if [ "$fail" -ne 0 ]; then
		find "$OBJ" -name '*.fail' -exec cat {} + | head -60
		exit 1
	fi
}

# The objects of the sources listed above, and no others: an object an
# earlier release's source left in $OBJ is not taken
objects() {
	awk '{ print $2 }' "$OUT/jobs.list" | sort
}

# The names a rump kernel keeps: its own entry points, the link set
# bounds, and what the compiler's run-time library provides
RENAME_KEEP='^(rump|RUMP|__|_GLOBAL_OFFSET_TABLE)'

stage_link() {
	local o tab
	rm -rf "$OUT/ren"
	mkdir -p "$OUT/ren"
	# Every object's names into the rumpns_ name space, so that none of
	# them can meet a name of the kernel's or of the C library's.
	# rump_syscalls.o is generated into it already; only the calls the
	# compiler makes of its own (memset for a structure cleared) are
	# turned to the rump kernel's copies.
	for o in $(objects); do
		local r keep=$RENAME_KEEP
		r=$OUT/ren/$(echo "${o#$OBJ/}" | tr / _)
		cp "$o" "$r"
		case $o in */rump_syscalls.o) keep='^$' ;; esac
		tab=$r.tab
		$NM -go "$r" | awk -v keep="$keep" -v sys="${o##*/}" \
			'sys == "rump_syscalls.o" && $NF !~ /^(memset|memcpy|memmove|memcmp)$/ { next }
			 $NF !~ keep { print $NF, "rumpns_" $NF }' | sort -u > "$tab"
		[ -s "$tab" ] && $OBJCOPY --redefine-syms "$tab" "$r"
		rm -f "$tab"
	done
	# The link sets (the domains, the components, the sysctl nodes ...)
	# are bounded here and put in one read-only section whose name the
	# kernel's linker script already places with the rest of .rodata
	local sets s
	sets=$(for o in "$OUT"/ren/*.o; do $NM -u "$o"; done | awk '{ print $NF }' \
		| sed -n 's/^__start_link_set_//p' | sort -u)
	{
		echo "SECTIONS {"
		echo "	.rodata.rump_link_sets : {"
		for s in $sets; do
			printf '\t\t. = ALIGN(8);\n\t\t__start_link_set_%s = .;\n\t\tKEEP(*(link_set_%s))\n\t\t__stop_link_set_%s = .;\n' "$s" "$s" "$s"
		done
		echo "	}"
		echo "}"
	} > "$OUT/linkset.ld"
	# The stack's calls of ip_savecontrol go to the interface component's
	# own (rumpcomp/tsnet_conf.c), which adds the type of service of a
	# datagram received to its ancillary data (ECN)
	$LD -r -d -T "$OUT/linkset.ld" --wrap=rumpns_ip_savecontrol -o "$OUT/rumpnet_all.o" "$OUT"/ren/*.o
	# Only the rump kernel's entry points stay global; the rest of its
	# names cannot meet the kernel's
	$NM -g --defined-only "$OUT/rumpnet_all.o" | awk '{ print $NF }' \
		| grep -E '^rump_' | sort -u > "$OUT/keep.syms"
	$OBJCOPY --keep-global-symbols="$OUT/keep.syms" "$OUT/rumpnet_all.o" "$OUT/rumpnet.o"
	rm -f "$OUT/rumpnet_all.o"
	rm -rf "$OUT/ren"
	echo "link: $OUT/rumpnet.o, $(wc -l < "$OUT/keep.syms") entry points"
	$NM -u "$OUT/rumpnet.o" | awk '{ print $NF }' | sort -u > "$OUT/undef.syms"
	echo "link: $(wc -l < "$OUT/undef.syms") names from outside ($OUT/undef.syms)"
}

# The copyright notices and licence terms of every NetBSD file that went
# into the object -- the sources and the headers they include, as the
# compiler listed them -- each once, for whoever distributes an image
# with the stack in it (THIRD_PARTY_NOTICES.md)
stage_notices() {
	local list=$OUT/notice.files out=$OUT/NOTICE-netbsd.txt
	objects | sed 's/$/.d/' | xargs cat | tr ' ' '\n' \
		| sed 's/^[[:space:]]*//' | grep -E "^$NB/" | grep -v ':$' | sort -u > "$list"
	{
		echo "The NetBSD $NB_VER sources compiled into the TessronOS network stack:"
		echo "their copyright notices and licence terms."
		echo
		cat "$S/conf/copyright"
		echo
		# shellcheck disable=SC2046
		awk -v top="$NB/" '
			FNR == 1 { inb = 0; name = FILENAME; sub(top, "", name) }
			FNR > 200 { next }
			/^[ \t]*\/\*/ && !inb { inb = 1; blk = "" }
			inb { blk = blk $0 "\n" }
			inb && /\*\// {
				inb = 0
				if (blk ~ /[Cc]opyright|Redistribution|[Pp]ublic domain/) {
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

case $STAGE in
fetch)		stage_fetch; stage_fetch_wifi ;;
gen)		stage_gen ;;
compile)	stage_compile ;;
link)		stage_link ;;
notices)	stage_notices ;;
all)		stage_fetch; stage_fetch_wifi; stage_gen; stage_compile; stage_link; stage_notices ;;
clean)		rm -rf "$OBJ" "$GEN" "$OUT/rumpnet.o" "$OUT/NOTICE-netbsd.txt" ;;
*)		echo "usage: $0 [fetch|gen|compile|link|notices|all|clean]" >&2; exit 2 ;;
esac
