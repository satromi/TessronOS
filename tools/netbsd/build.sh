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
# (vers.c, ioconf.c and the machine header links) are made here.
#
# Usage: build.sh [fetch|gen|compile|link|notices|all|clean]
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
# the set NetBSD publishes for it (the SHA512 file of the release)
NB_VER=10.1
NB_URL=https://cdn.netbsd.org/pub/NetBSD/NetBSD-$NB_VER/source/sets/syssrc.tgz
NB_SHA512=766ac21f33cfe0e701dfedb894fa07f36d811da1a12e979181e8fca7af4e627852680ce42a7b29e97dd3e2e402ddf9ae7bfba60c8d7dc6b8a3354d8ce8c06926
NB=${NBSRC:-$HOME/netbsd/netbsd-$NB_VER}
OUT=${OUT:-$HOME/tessronos-netbsd}
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
EXTRACT_DIRS="common/include/ppath common/include/prop
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
 sys/miscfs/deadfs sys/net sys/net/agr sys/net80211 sys/netatalk sys/netinet
 sys/netinet6 sys/netipsec sys/netmpls sys/rump sys/rump/include/machine
 sys/rump/include/opt sys/rump/include/rump sys/rump/include/rump-sys
 sys/rump/include/sys sys/rump/librump/rumpkern
 sys/rump/librump/rumpkern/arch/generic sys/rump/librump/rumpnet
 sys/rump/net/lib/libnet sys/rump/net/lib/libnetinet
 sys/rump/net/lib/libnetinet6 sys/secmodel sys/secmodel/extensions
 sys/secmodel/suser sys/sys sys/ufs/ufs sys/uvm"
EXTRACT_TREES="sys/arch/aarch64/include sys/arch/arm/include sys/arch/evbarm/include"

stage_fetch() {
	if [ -d "$S/rump" ]; then
		echo "fetch: $NB is there"
		return
	fi
	mkdir -p "$(dirname "$NB")"
	local tgz
	tgz=$(dirname "$NB")/syssrc-$NB_VER.tgz
	if [ ! -f "$tgz" ]; then
		curl -fsSL -o "$tgz.part" "$NB_URL"
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
	rm -f "$tgz"
	echo "fetch: NetBSD $NB_VER sources in $NB"
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
 subr_thmap.c subr_vmem.c subr_workqueue.c subr_xcall.c sys_descrip.c
 sys_generic.c sys_getrandom.c sys_module.c sys_pipe.c sys_select.c
 syscalls.c uipc_sem.c
 uvm_aobj.c uvm_readahead.c uvm_object.c uvm_swapstub.c
 uvm_page_array.c uvm_page_status.c
 secmodel.c secmodel_suser.c secmodel_extensions.c
 vnode_if.c clock_subr.c
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
 prop_dictionary.c prop_dictionary_util.c prop_ingest.c
 prop_kern.c prop_number.c prop_object.c prop_stack.c prop_string.c
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
NETA_SRCS="if_tsneta.c tsnet_conf.c novfs_stub.c"

# ---------------------------------------------------------------- flags

# As the kernel is built (common.mk), without the floating point
# registers, which kernel tasks do not have; with the rump kernel's own
# options (opt_rumpkernel.h)
CFLAGS="-march=armv8.2-a -mgeneral-regs-only -mno-outline-atomics -O2 -g
 -ffreestanding -fno-strict-aliasing -fno-delete-null-pointer-checks
 -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -ffunction-sections -fdata-sections -std=gnu99
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
ASFLAGS="-march=armv8.2-a -D_LOCORE -D_KERNEL"

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
	add_jobs libkern "$LIBKERN_PATH" "$LIBKERN_SRCS"
	# net_stub.c holds tentative definitions of what if.c defines
	add_jobs rumpnet "$RUMPNET_PATH" "$RUMPNET_SRCS" "-fcommon"
	add_jobs net "$NET_PATH" "$NET_SRCS" "-I$GEN/net"
	add_jobs neta "$NETA_PATH" "$NETA_SRCS"
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
	for o in $(find "$OBJ" -name '*.o' | sort); do
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
	$LD -r -d -T "$OUT/linkset.ld" -o "$OUT/rumpnet_all.o" "$OUT"/ren/*.o
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
	find "$OBJ" -name '*.o.d' -exec cat {} + | tr ' ' '\n' \
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
fetch)		stage_fetch ;;
gen)		stage_gen ;;
compile)	stage_compile ;;
link)		stage_link ;;
notices)	stage_notices ;;
all)		stage_fetch; stage_gen; stage_compile; stage_link; stage_notices ;;
clean)		rm -rf "$OBJ" "$GEN" "$OUT/rumpnet.o" "$OUT/NOTICE-netbsd.txt" ;;
*)		echo "usage: $0 [fetch|gen|compile|link|notices|all|clean]" >&2; exit 2 ;;
esac
