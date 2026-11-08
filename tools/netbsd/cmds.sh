#!/bin/bash
#
# cmds.sh -- the standard commands of NetBSD 11.0 for TessronOS
#
# Copyright (C) 2026 satromi
# This software is distributed under the T-License 2.2.
#
# Fetches the source sets of a pinned NetBSD release, keeps the parts the
# commands are made of (bin, usr.bin, the libraries they use and a few of
# external/), and builds each command as a program of its own for the
# POSIX layer of TessronOS (lib/libposix, include/posix, design 5.8.7):
#
#	$OUT/bin/<name>		one ELF a command, debugging data taken out
#	$OUT/libnbcompat.a	the parts of NetBSD's libc and libutil they share
#	$OUT/NOTICE-netbsd-cmds.txt
#
# The NetBSD sources are not changed and are not kept in this tree. What
# newlib lacks is given by lib/libposix and by NetBSD's own files
# compiled into libnbcompat.a. A command with several names (gunzip and
# gzip, [ and test) is linked once for each name, with the name it
# answers to when it is started with no argv[0] of its own.
#
# Usage: cmds.sh [fetch|compat|cmds|notices|all|clean] [name ...]
#   make passes ARCH, MARCH, PSX_LIB, PSX_CRT, USER_LD and PSX_INC; OUT, NBCSRC
#   and JOBS may be set. ARCH is aarch64 (the default) or x86_64.
#
set -e

STAGE=${1:-all}
shift || true
ONLY="$*"
TOP=$(cd "$(dirname "$0")/../.." && pwd)
ARCH=${ARCH:-aarch64}
case $ARCH in
x86_64)
	XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	CROSS=$XB/x86_64-elf-
	MARCH=${MARCH:-x86-64-v2}
	OUT=${OUT:-$HOME/tessronos-netbsd-cmds-x64}
	# char as it is on AArch64: the regular expressions of NetBSD's libc built with BCS_ONLY take
	# bytes from char, and with a signed one no byte above 0x7f matches. newlib's stdio asks
	# through the names libposix gives
	ARCH_CF=-funsigned-char
	ARCH_LD="-Wl,-u,_fstat_r -Wl,-u,_stat_r"
	;;
aarch64)
	XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	CROSS=$XB/aarch64-none-elf-
	MARCH=${MARCH:-armv8.2-a}
	ARCH_CF=-mstrict-align
	ARCH_LD=
	;;
*)
	echo "cmds: ARCH is aarch64 or x86_64, not $ARCH" >&2
	exit 2
	;;
esac
CC=${CROSS}gcc
AR=${CROSS}ar
OBJCOPY=${CROSS}objcopy
JOBS=${JOBS:-8}

NB_VER=11.0
NB_BASE=https://cdn.netbsd.org/pub/NetBSD/NetBSD-$NB_VER/source/sets
NB_SRC_SHA512=99a8f96202290ce203d98396305f3ae1e38b49663b1210447cb82d05ee1717969adb6101dab5f06050e9ef981bd6efb798ae1b2e8e7f0079ba5486da4ca82fcc
NB_SYS_SHA512=318d451ecc83749607d5448198c02663ea5f0b7f4cc74fb565747f86eead7b66b5809a37deee219dc6ddb2e5ee332d1cbb94c1a4e9db63620c3abd7586fb3057
NB=${NBCSRC:-$HOME/netbsd/src-$NB_VER}
DL=${NBDL:-$HOME/netbsd/dl}
OUT=${OUT:-$HOME/tessronos-netbsd-cmds}

# diff is not in NetBSD's base (its diff is GNU's): OpenBSD's is taken
OB_VER=7.9
OB_BASE=https://cdn.openbsd.org/pub/OpenBSD/$OB_VER
OB_SRC_SHA256=fb305c553059b48e8ee64539f392b783cb38a67865823f6f6a94f3b220a1268b
OB=${OBSRC:-$HOME/openbsd/src-$OB_VER}
OBDL=${OBDL:-$HOME/openbsd/dl}
OB_DIRS="usr.bin/diff"

UOBJ=${UOBJ:-$TOP/build_make/obj}
PSX_LIB=${PSX_LIB:-$UOBJ/psx/libposix.a}
PSX_CRT=${PSX_CRT:-$UOBJ/psx/psx_crt.o}
USER_LD=${USER_LD:-$TOP/etc/linker/$([ "$ARCH" = x86_64 ] && echo x64 || echo user)/program.ld}

# What is kept of the two sets
SRC_DIRS="usr/src/bin usr/src/usr.bin usr/src/include usr/src/lib/libc usr/src/lib/libutil
 usr/src/lib/libterminfo usr/src/lib/libcurses usr/src/lib/libedit usr/src/lib/libz
 usr/src/lib/libbz2 usr/src/external/bsd/less usr/src/external/bsd/nvi
 usr/src/external/bsd/file usr/src/external/bsd/bzip2 usr/src/external/historical/nawk
 usr/src/external/bsd/byacc"
SYS_DIRS="usr/src/common/dist/zlib usr/src/common/lib/libc usr/src/common/include usr/src/sys/sys"

# yacc of NetBSD (byacc) made for the building machine: the grammars of
# NetBSD use its own forms, which bison does not take
YACC=$OUT/host/yacc

# The terminals libterminfo knows of itself (tools/netbsd/port/terminfo.src)
TERMS="tessron vt100 xterm xterm-256color dumb"

CFLAGS_ALL="-march=$MARCH $ARCH_CF -fno-stack-protector -O2 -g -std=gnu11
 -ffunction-sections -fdata-sections -D_GNU_SOURCE -D_POSIX_TIMERS=1
 -D_POSIX_MONOTONIC_CLOCK=200112L -DNO_NLS=1
 -Wno-implicit-fallthrough -Wno-unused-result -Wno-format-truncation"
INC="-I$OUT/include -I$TOP/include/posix"
KAPI="-DCHK_TKERNEL_CONST -DUSE_STDINC_STDINT=1 -include stdint.h -I$TOP/include/posix -I$TOP/include -I$TOP/config"
LDFLAGS_ALL="-march=$MARCH -nostdlib -nostartfiles -static -T $USER_LD -Wl,--build-id=none -Wl,--gc-sections $ARCH_LD"

fetch_set() {
	local name=$1 sum=$2
	mkdir -p "$DL"
	if [ ! -f "$DL/$name" ]; then
		echo "fetch: $NB_BASE/$name"
		curl -fsSL -C - -o "$DL/$name.part" "$NB_BASE/$name"
		mv "$DL/$name.part" "$DL/$name"
	fi
	echo "$sum  $DL/$name" | sha512sum -c -
}

stage_fetch() {
	if [ ! -d "$OB/usr.bin/diff" ]; then
		mkdir -p "$OBDL" "$OB"
		if [ ! -f "$OBDL/src.tar.gz" ]; then
			echo "fetch: $OB_BASE/src.tar.gz"
			curl -fsSL -o "$OBDL/src.tar.gz.part" "$OB_BASE/src.tar.gz"
			mv "$OBDL/src.tar.gz.part" "$OBDL/src.tar.gz"
		fi
		echo "$OB_SRC_SHA256  $OBDL/src.tar.gz" | sha256sum -c -
		tar xzf "$OBDL/src.tar.gz" -C "$OB" --exclude=CVS $OB_DIRS
		echo "fetch: OpenBSD $OB_VER sources of diff in $OB"
	fi
	if [ -d "$NB/bin/sh" ] && [ -d "$NB/common/dist/zlib" ] && [ -d "$NB/external/bsd/byacc" ]; then
		echo "fetch: $NB is there"
		return
	fi
	fetch_set src.tgz $NB_SRC_SHA512
	fetch_set syssrc.tgz $NB_SYS_SHA512
	mkdir -p "$NB"
	tar xzf "$DL/src.tgz" -C "$NB" --strip-components=2 --exclude=CVS $SRC_DIRS
	# the system set links its files to the CVS records of others: those are left out
	tar xzf "$DL/syssrc.tgz" -C "$NB" --strip-components=2 --exclude=CVS $SYS_DIRS
	echo "fetch: NetBSD $NB_VER sources of the commands in $NB"
}

# ---------------------------------------------------------------- the building machine's tools

stage_host() {
	local d=$NB/external/bsd/byacc/dist h=$OUT/host
	mkdir -p "$h/inc/sys"
	# what NetBSD's sys/cdefs.h and libc give that the building machine may lack
	cat > "$h/host.h" <<'EOT'
#include <stddef.h>
#include <stdint.h>
#define __RCSID(s)
#define __COPYRIGHT(s)
#define __dead __attribute__((noreturn))
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#define __scanflike(a, b)
#define __unused __attribute__((unused))
#define __UNCONST(a) ((void *)(unsigned long)(const void *)(a))
#define __arraycount(a) (sizeof(a) / sizeof((a)[0]))
#define __predict_false(e) __builtin_expect((e) != 0, 0)
#define __predict_true(e) __builtin_expect((e) != 0, 1)
#define _DIAGASSERT(e) ((void)0)
#define hdestroy1(a, b) hdestroy()
#define strlcpy host_strlcpy
static inline size_t host_strlcpy(char *d, const char *s, size_t n)
{
	size_t k = 0, i, c;

	while (s[k] != 0) k++;
	if (n != 0) {
		c = (k < n - 1) ? k : n - 1;
		for (i = 0; i < c; i++) d[i] = s[i];
		d[c] = 0;
	}
	return k;
}
/* the same numbers every time: what is made from them is the same every time */
#define arc4random host_arc4random
static inline uint32_t host_arc4random(void)
{
	static uint64_t s = 0x2545F4914F6CDD1DULL;

	s = s * 6364136223846793005ULL + 1442695040888963407ULL;
	return (uint32_t)(s >> 32);
}
extern char *program_invocation_short_name;
#define getprogname() program_invocation_short_name
EOT
	cat > "$h/inc/sys/endian.h" <<'EOT'
#include <endian.h>
#include <stdint.h>
static inline uint16_t le16dec(const void *p) { const uint8_t *b = p; return (uint16_t)(b[0] | (b[1] << 8)); }
static inline uint32_t le32dec(const void *p) { const uint8_t *b = p; return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static inline void le16enc(void *p, uint16_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static inline void le32enc(void *p, uint32_t v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static inline uint32_t be32dec(const void *p) { const uint8_t *b = p; return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
static inline void be32enc(void *p, uint32_t v) { uint8_t *b = p; b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
EOT
	: > "$h/inc/sys/bitops.h"
	: > "$h/inc/nbtool_config.h"
	echo '#define _DIAGASSERT(e) ((void)0)' > "$h/inc/namespace.h"
	cat > "$h/inc/util.h" <<'EOT'
#include <err.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline void *emalloc(size_t n) { void *p = malloc(n); if (p == NULL) err(1, NULL); return p; }
static inline void *ecalloc(size_t n, size_t s) { void *p = calloc(n, s); if (p == NULL) err(1, NULL); return p; }
static inline void *erealloc(void *q, size_t n) { void *p = realloc(q, n); if (p == NULL) err(1, NULL); return p; }
static inline char *estrdup(const char *s) { char *p = strdup(s); if (p == NULL) err(1, NULL); return p; }
static inline char *estrndup(const char *s, size_t n) { char *p = strndup(s, n); if (p == NULL) err(1, NULL); return p; }
static inline int easprintf(char **r, const char *f, ...)
{
	va_list ap;
	int n;

	va_start(ap, f);
	n = vasprintf(r, f, ap);
	va_end(ap);
	if (n < 0) err(1, NULL);
	return n;
}
EOT
	cp -p "$NB/include/cdbw.h" "$h/inc/"
	HCC="cc -O2 -w -D_GNU_SOURCE -include $h/host.h"
	if [ ! -x "$YACC" ] || [ "$d/reader.c" -nt "$YACC" ]; then
		echo "HOSTCC yacc"
		(cd "$d" && $HCC -DHAVE_CONFIG_H -I../include -DMAXTABLE=INT_MAX -o "$YACC" \
		    closure.c error.c lalr.c lr0.c main.c mkpar.c output.c reader.c symtab.c \
		    verbose.c warshall.c graph.c mstring.c btyaccpar.c)
	fi
	# nbperf and tic: the tables of names of libterminfo and the descriptions of terminals in it
	if [ ! -x "$h/nbperf" ]; then
		echo "HOSTCC nbperf"
		(cd "$NB/usr.bin/nbperf" && $HCC -I"$h/inc" -o "$h/nbperf" nbperf.c nbperf-bdz.c \
		    nbperf-chm.c nbperf-chm3.c graph2.c graph3.c "$NB/common/lib/libc/stdlib/mi_vector_hash.c")
	fi
	if [ ! -f "$h/hash.c" ] || [ ! -f "$h/termcap_hash.c" ]; then
		(cd "$NB/lib/libterminfo" && TOOL_NBPERF=$h/nbperf sh genhash term.h > "$h/hash.c" \
		    && TOOL_NBPERF=$h/nbperf sh genthash termcap_map.c > "$h/termcap_hash.c") >/dev/null
	fi
	if [ ! -x "$h/tic" ]; then
		echo "HOSTCC tic"
		(cd "$NB/usr.bin/tic" && $HCC -DHAVE_NBTOOL_CONFIG_H=1 -include "$h/inc/sys/endian.h" \
		    -I"$h/inc" -I"$NB/lib/libterminfo" -DTERMINFO_COMPILE -DTERMINFO_DB -DTERMINFO_COMPAT \
		    -o "$h/tic" tic.c "$NB/lib/libterminfo/compile.c" "$h/hash.c" "$NB/lib/libc/cdb/cdbw.c" \
		    "$NB/common/lib/libc/stdlib/mi_vector_hash.c")
	fi
	# the terminals libterminfo knows without a database: TessronOS's console and the like
	if [ ! -f "$h/compiled_terms.c" ] || [ "$TOP/tools/netbsd/port/terminfo.src" -nt "$h/compiled_terms.c" ]; then
		{
			echo "/* made by tools/netbsd/cmds.sh from tools/netbsd/port/terminfo.src */"
			"$h/tic" -Sx "$TOP/tools/netbsd/port/terminfo.src" $TERMS
		} > "$h/compiled_terms.c"
	fi
}

# ---------------------------------------------------------------- the shared parts

# NetBSD's headers the commands include, copied as they are
HDRS="include/fts.h include/vis.h include/regex.h include/db.h include/mpool.h include/stringlist.h
 include/sysexits.h include/tzfile.h include/md2.h sys/sys/md4.h sys/sys/md5.h sys/sys/rmd160.h
 sys/sys/sha1.h sys/sys/sha2.h include/fnmatch.h sys/sys/cdbr.h
 sys/sys/ttydefaults.h"

# The files of libc and libutil the commands share (paths from $NB)
COMPAT="lib/libc/gen/fts.c lib/libc/gen/setmode.c lib/libc/gen/vis.c lib/libc/gen/unvis.c
 lib/libc/gen/basename.c lib/libc/gen/dirname.c lib/libc/gen/signalname.c
 lib/libc/gen/signalnext.c lib/libc/gen/fnmatch.c
 lib/libc/gen/humanize_number.c lib/libc/gen/dehumanize_number.c lib/libc/gen/getbsize.c
 lib/libc/gen/stringlist.c lib/libc/string/strmode.c lib/libc/stdlib/radixsort.c
 lib/libc/stdlib/merge.c lib/libc/stdlib/strtonum.c lib/libc/stdlib/strsuftoll.c
 common/lib/libc/stdlib/heapsort.c common/lib/libc/stdlib/mi_vector_hash.c common/lib/libc/stdlib/strtoi.c common/lib/libc/stdlib/strtou.c
 lib/libc/stdio/fparseln.c lib/libc/regex/regcomp.c lib/libc/regex/regerror.c
 lib/libc/regex/regexec.c lib/libc/regex/regfree.c lib/libutil/efun.c
 lib/libutil/raise_default_signal.c lib/libutil/strpct.c lib/libutil/stat_flags.c
 @lib/libutil/parsedate.y
 lib/libc/db/db/db.c lib/libc/db/db/dbfile.c lib/libc/db/mpool/mpool.c
 lib/libc/db/btree/bt_close.c lib/libc/db/btree/bt_conv.c lib/libc/db/btree/bt_debug.c
 lib/libc/db/btree/bt_delete.c lib/libc/db/btree/bt_get.c lib/libc/db/btree/bt_open.c
 lib/libc/db/btree/bt_overflow.c lib/libc/db/btree/bt_page.c lib/libc/db/btree/bt_put.c
 lib/libc/db/btree/bt_search.c lib/libc/db/btree/bt_seq.c lib/libc/db/btree/bt_split.c
 lib/libc/db/btree/bt_utils.c lib/libc/db/hash/hash.c lib/libc/db/hash/hash_bigkey.c
 lib/libc/db/hash/hash_buf.c lib/libc/db/hash/hash_func.c lib/libc/db/hash/hash_log2.c
 lib/libc/db/hash/hash_page.c lib/libc/db/recno/rec_close.c lib/libc/db/recno/rec_delete.c
 lib/libc/db/recno/rec_get.c lib/libc/db/recno/rec_open.c lib/libc/db/recno/rec_put.c
 lib/libc/db/recno/rec_search.c lib/libc/db/recno/rec_seq.c lib/libc/db/recno/rec_utils.c"

# zlib and libbz2, for grep -Z, gzip and bzip2
ZLIB="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees
 trees uncompr zutil"
BZLIB="blocksort huffman crctable randtable compress decompress bzlib"

# One library of the shared parts: name, the sources (a grammar marked with @), flags
build_lib() {
	local lib=$1 files=$2 flags=$3 f s o y objs=""
	mkdir -p "$OUT/obj/$lib"
	for f in $files; do
		case $f in
		@*.y)	# a grammar: made into C by yacc first
			f=${f#@}
			y=$OUT/obj/$lib/$(basename "${f%.y}").c
			if [ ! -f "$y" ] || [ "$NB/$f" -nt "$y" ]; then
				"$YACC" -o "$y" "$NB/$f" >/dev/null
			fi
			s=$y
			;;
		/*)	s=$f ;;		# made here
		*)	s=$NB/$f ;;
		esac
		o=$OUT/obj/$lib/$(basename "${s%.c}").o
		objs="$objs $o"
		if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then
			echo "CC  $lib/$(basename $s)"
			$CC $CFLAGS_ALL $INC -I"$NB/$(dirname $f)" -I"$NB/lib/libc/include" $flags \
			    -MMD -c -o "$o" "$s"
		fi
	done
	# made again only when an object is newer: the commands are made again after it
	for o in $objs; do
		if [ ! -f "$OUT/$lib.a" ] || [ "$o" -nt "$OUT/$lib.a" ]; then
			rm -f "$OUT/$lib.a"
			$AR rcs "$OUT/$lib.a" $objs
			break
		fi
	done
	echo "compat: $OUT/$lib.a"
}

stage_compat() {
	local f z="" b=""
	mkdir -p "$OUT/include/sys" "$OUT/obj/compat"
	for f in $HDRS; do
		case $f in
		sys/sys/*) cp -p "$NB/$f" "$OUT/include/sys/$(basename $f)" ;;
		*)         cp -p "$NB/$f" "$OUT/include/$(basename $f)" ;;
		esac
	done
	# libterminfo includes it by its short name
	cp -p "$NB/sys/sys/cdbr.h" "$OUT/include/cdbr.h"
	cp -p "$NB/common/dist/zlib/zlib.h" "$NB/common/dist/zlib/zconf.h" "$OUT/include/"
	cp -p "$NB/external/bsd/bzip2/dist/bzlib.h" "$OUT/include/"
	# what NetBSD's libc includes of its own: its checks of arguments are not made
	cat > "$OUT/include/namespace.h" <<'EOT'
#ifndef _DIAGASSERT
#define _DIAGASSERT(e)	((void)0)
#endif
#define FLOCKFILE(fp)	((void)0)
#define FUNLOCKFILE(fp)	((void)0)
EOT
	cp "$OUT/include/namespace.h" "$OUT/include/reentrant.h"
	: > "$OUT/include/nbtool_config.h"
	build_lib libnbcompat "$COMPAT" "-DBCS_ONLY -D__DBINTERFACE_PRIVATE"
	for f in $ZLIB; do z="$z common/dist/zlib/$f.c"; done
	build_lib libz "$z" "-DHAVE_UNISTD_H -DHAVE_STDARG_H"
	for f in $BZLIB; do b="$b external/bsd/bzip2/dist/$f.c"; done
	build_lib libbz2 "$b" ""
	# libterminfo, with the descriptions of stage_host built in (no database of terminals)
	cp -p "$NB/lib/libterminfo/term.h" "$NB/lib/libterminfo/termcap.h" "$NB/lib/libcurses/curses.h" 	    "$OUT/include/"
	build_lib libterminfo "lib/libterminfo/term.c lib/libterminfo/ti.c lib/libterminfo/setupterm.c
	    lib/libterminfo/curterm.c lib/libterminfo/tparm.c lib/libterminfo/tputs.c
	    lib/libterminfo/termcap.c $OUT/host/hash.c" "-I$NB/lib/libterminfo -I$OUT/host -include string.h"
	# libedit, the line editing of tsh: its tables of functions made by its makelist first
	f=$OUT/obj/libedit/gen
	mkdir -p "$f"
	(cd "$NB/lib/libedit" &&
	 sh makelist -h vi.c > "$f/vi.h" && sh makelist -h emacs.c > "$f/emacs.h" &&
	 sh makelist -h common.c > "$f/common.h" &&
	 cd "$f" && sh "$NB/lib/libedit/makelist" -fh vi.h emacs.h common.h > fcns.h &&
	 sh "$NB/lib/libedit/makelist" -fc vi.h emacs.h common.h > func.h &&
	 cd "$NB/lib/libedit" && sh makelist -bh vi.c emacs.c common.c > "$f/help.h")
	cp -p "$NB/lib/libedit/histedit.h" "$OUT/include/"
	z=""
	for b in chared chartype common el eln emacs filecomplete hist history historyn keymacro \
	    literal map parse prompt read refresh search sig terminal tokenizer tokenizern \
	    tty vi; do
		z="$z lib/libedit/$b.c"
	done
	build_lib libedit "$z" "-I$f -I$NB/lib/libedit -D__STDC_ISO_10646__=201103L"
}

# ---------------------------------------------------------------- the commands

# name|directory|sources|cflags|libraries|other names
#   sources: empty for the SRCS of the command's Makefile (or <directory name>.c);
#	a name with @ in front is a grammar for yacc, with % a file of tools/netbsd/port
#   cflags: @NB stands for the top of the NetBSD sources, @KAPI for the flags
#	a program needs that calls the system's own API (include/ts)
#   libraries: -lz, -lbz2 and the like are the ones built by the compat stage
CMDS="
cat|bin/cat||||
chmod|bin/chmod||||
cp|bin/cp||-DSMALL||
date|bin/date|date.c %date_net.c|||
df|bin/df||||
echo|bin/echo||||
expr|bin/expr|@expr.y|||
hostname|bin/hostname||||
kill|bin/kill||||
ln|bin/ln||||
ls|bin/ls||-DSMALL||
mkdir|bin/mkdir||||
mv|bin/mv||||
pwd|bin/pwd||||
rm|bin/rm||||
rmdir|bin/rmdir||||
sleep|bin/sleep||||
test|bin/test||||[
basename|usr.bin/basename||||
cmp|usr.bin/cmp||||
comm|usr.bin/comm||||
cut|usr.bin/cut||||
dirname|usr.bin/dirname||||
du|usr.bin/du||||
env|usr.bin/env||||
find|usr.bin/find||||
fmt|usr.bin/fmt|fmt.c ../mail/head.c|-DFMT_PROG||
fold|usr.bin/fold||||
grep|usr.bin/grep|||-lz -lbz2|egrep fgrep zgrep
head|usr.bin/head||||
hexdump|usr.bin/hexdump||||od
id|usr.bin/id||||whoami groups
join|usr.bin/join||||
mktemp|usr.bin/mktemp||||
nice|usr.bin/nice||||
nl|usr.bin/nl||||
nohup|usr.bin/nohup||||
paste|usr.bin/paste||||
printenv|usr.bin/printenv||||
printf|usr.bin/printf||||
realpath|usr.bin/realpath||||
rev|usr.bin/rev||||
sed|usr.bin/sed||||
seq|usr.bin/seq||||
split|usr.bin/split||||
stat|usr.bin/stat||||readlink
tail|usr.bin/tail||||
tee|usr.bin/tee||||
time|usr.bin/time||-DNOT_CSH -I@NB/bin||
touch|usr.bin/touch||||
tr|usr.bin/tr||||
tsort|usr.bin/tsort||||
tty|usr.bin/tty||||
uname|usr.bin/uname||||
uniq|usr.bin/uniq||||
wc|usr.bin/wc||||
whereis|usr.bin/whereis||||which
xargs|usr.bin/xargs||||
yes|usr.bin/yes||||
logname|usr.bin/logname||||
timeout|usr.bin/timeout||||
expand|usr.bin/expand||||
unexpand|usr.bin/unexpand||||
column|usr.bin/column||||
jot|usr.bin/jot||||
getopt|usr.bin/getopt||||
pathchk|usr.bin/pathchk||||
tsh|@TSH||||sh
true|bin|%true.c|||false
ps|bin|%ps.c|@KAPI||
diff|@OB/usr.bin/diff|diff.c diffdir.c diffreg.c xmalloc.c|||
sort|usr.bin/sort||||
gzip|usr.bin/gzip||-DNO_XZ_SUPPORT -DNO_LZ_SUPPORT|-lz -lbz2|gunzip gzcat zcat
bzip2|external/bsd/bzip2/dist|bzip2.c||-lbz2|bunzip2 bzcat
pax|bin/pax|ar_io.c ar_subs.c buf_subs.c file_subs.c ftree.c gen_subs.c getoldopt.c options.c pat_rep.c pax.c sel_subs.c tables.c tar.c tty_subs.c|-DSMALL -DNO_CPIO||tar
awk|external/historical/nawk/dist|awkgram.y b.c lex.c lib.c main.c parse.c proctab.c run.c tran.c|-DHAS_ISBLANK||
"

# The sources a Makefile of NetBSD names in SRCS, those spelt with variables left out
make_srcs() {
	[ -f "$1" ] || return 0
	awk '
	/^SRCS[+]?=/ { on = 1; sub(/^SRCS[+]?=/, "") }
	on { more = sub(/\\$/, ""); printf "%s ", $0; if (!more) on = 0 }
	' "$1" | tr ' \t' '\n\n' | grep -v '^$' | grep -v '\$' | tr '\n' ' '
}

# tsh (application/tsh): NetBSD's sh made TessronOS's shell (design 5.8.8). Its tables are
# made by its own scripts first; the builtins test, printf and kill are NetBSD's commands
# compiled for the shell. It is linked as tsh and as sh.
TSH_SRCS="alias.c arith_token.c arithmetic.c cd.c echo.c error.c eval.c exec.c expand.c histedit.c
 input.c jobs.c mail.c main.c memalloc.c miscbltin.c mystring.c options.c parser.c redir.c show.c
 trap.c output.c var.c syntax.c tspawn.c bashext.c basharr.c"
TSH_SRCS_EXTRA="bin/test/test.c usr.bin/printf/printf.c bin/kill/kill.c"

build_tsh() {
	local src=$TOP/application/tsh od=$OUT/obj/tsh g s o n objs="" shsrcs=""
	g=$od/gen
	mkdir -p "$g" "$OUT/bin"
	if up_to_date "tsh sh" "$src"/* "$src"/bltin/* "$OUT/libedit.a" "$OUT/libterminfo.a" \
	    "$OUT/libnbcompat.a"; then
		echo "LD  tsh (as it was)"
		return 0
	fi
	for s in $TSH_SRCS; do
		case $s in
		echo.c)	shsrcs="$shsrcs $src/bltin/echo.c" ;;
		*)	shsrcs="$shsrcs $src/$s" ;;
		esac
	done
	(cd "$g" && sh "$src/mktokens" &&
	 sh "$src/mkbuiltins" "$src/shell.h" "$src/builtins.def" "$g" &&
	 sh "$src/mknodes.sh" "$src/nodetypes" "$src/nodes.c.pat" "$g" &&
	 sh "$src/mknodenames.sh" "$g/nodes.h" > "$g/nodenames.h" &&
	 sh "$src/mkoptions.sh" "$src/option.list" optinit.h "$g" &&
	 sh "$src/mkinit.sh" $shsrcs) || return 1
	for s in $shsrcs "$g/builtins.c" "$g/init.c" "$g/nodes.c"; do
		o=$od/$(basename "${s%.c}").o
		objs="$objs $o"
		rm -f "$o"
		$CC $CFLAGS_ALL -Wall -DSHELL -D__TessronOS__ -DUSE_LRAND48 -DREJECT_NULS -I"$g" -I"$src" -I"$NB/lib/libedit" $INC \
		    -MMD -c -o "$o" "$s" || return 1
	done
	for s in $TSH_SRCS_EXTRA; do
		o=$od/$(basename "${s%.c}").o
		objs="$objs $o"
		rm -f "$o"
		$CC $CFLAGS_ALL -DSHELL -I"$g" -I"$src" -I"$src/bltin" -I"$NB/$(dirname $s)" $INC \
		    -MMD -c -o "$o" "$NB/$s" || return 1
	done
	: > "$od/origin"
	for n in tsh sh; do
		printf '%s\t%s\n' "$n" "TessronOS のシェル tsh(NetBSD $NB_VER の sh から。T-License 2.2、BSD の表示を保つ)" >> "$od/origin"
		printf 'const char __psx_default_name[] = "%s";\n' "$n" > "$od/name_$n.c"
		$CC $CFLAGS_ALL -c -o "$od/name_$n.o" "$od/name_$n.c" || return 1
		$CC $LDFLAGS_ALL -o "$od/$n.elf" "$PSX_CRT" $objs "$od/name_$n.o" "$OUT/libedit.a" \
		    "$OUT/libterminfo.a" "$OUT/libnbcompat.a" \
		    -Wl,--start-group "$PSX_LIB" -lm -lc -lgcc -Wl,--end-group || return 1
		$OBJCOPY --strip-debug "$od/$n.elf" "$OUT/bin/$n" || return 1
		echo "LD  $n"
	done
}

# Whether the programs of these names are there and newer than what they are made of:
# the files given, the POSIX layer, the headers and this script
up_to_date() {
	local names=$1 n f
	shift
	for n in $names; do
		[ -f "$OUT/bin/$n" ] || return 1
		for f in "$@" "$PSX_LIB" "$PSX_CRT" "$0" "$NEWEST_HDR"; do
			[ -n "$f" ] && [ "$f" -nt "$OUT/bin/$n" ] && return 1
		done
	done
	return 0
}

# One command: compiled once, linked once for each of its names
build_cmd() {
	local name=$1 dir=$2 srcs=$3 cflags=$4 libs=$5 more=$6
	local od=$OUT/obj/$name objs="" s o y n l libs2="" sd
	mkdir -p "$od" "$OUT/bin"
	case $dir in
	@OB/*)	sd=$OB/${dir#@OB/} ;;	# a command of OpenBSD
	*)	sd=$NB/$dir ;;
	esac
	if [ -z "$srcs" ]; then
		srcs=$(make_srcs "$sd/Makefile")
		[ -z "$srcs" ] && srcs=$(basename "$dir").c
	fi
	cflags=${cflags//@NB/$NB}
	cflags=${cflags//@KAPI/$KAPI}
	for l in $libs; do
		case $l in
		-l*)	if [ -f "$OUT/lib${l#-l}.a" ]; then l=$OUT/lib${l#-l}.a; fi ;;
		esac
		libs2="$libs2 $l"
	done
	# made already, and nothing it is made of newer: left as it is
	l=""
	for s in $srcs; do
		case $s in
		@*)	l="$l $sd/${s#@}" ;;
		%*)	l="$l $TOP/tools/netbsd/port/${s#%}" ;;
		*)	l="$l $sd/$s" ;;
		esac
	done
	if up_to_date "$name $more" $l $libs2 "$OUT/libnbcompat.a"; then
		echo "LD  $name (as it was)"
		return 0
	fi
	for s in $srcs; do
		case $s in
		@*.y)	# a grammar: made into C by yacc first
			y=${s#@}
			"$YACC" -d -o "$od/${y%.y}.c" "$sd/$y" >/dev/null || return 1
			s=$od/${y%.y}.c
			;;
		%*)	s=$TOP/tools/netbsd/port/${s#%} ;;
		*.y)	y=$s
			"$YACC" -d -o "$od/${y%.y}.c" "$sd/$y" >/dev/null || return 1
			s=$od/${y%.y}.c
			;;
		*)	s=$sd/$s ;;
		esac
		o=$od/$(basename "${s%.c}").o
		objs="$objs $o"
		rm -f "$o"
		$CC $CFLAGS_ALL $cflags -I"$sd" -I"$od" $INC -MMD -c -o "$o" "$s" || return 1
	done
	case $dir in
	@OB/*)	o="OpenBSD $OB_VER の ${dir#@OB/}(ISC・BSD ライセンス)" ;;
	*)	case $srcs in
		%*)	o="TessronOS のコマンド(T-License 2.2)" ;;
		*)	o="NetBSD $NB_VER の $dir(BSD ライセンス)" ;;
		esac ;;
	esac
	: > "$od/origin"
	for n in $name $more; do
		printf '%s\t%s\n' "$n" "$o" >> "$od/origin"
		printf 'const char __psx_default_name[] = "%s";\n' "$n" > "$od/name_$n.c"
		$CC $CFLAGS_ALL -c -o "$od/name_$n.o" "$od/name_$n.c" || return 1
		$CC $LDFLAGS_ALL -o "$od/$n.elf" "$PSX_CRT" $objs "$od/name_$n.o" "$OUT/libnbcompat.a" \
		    $libs2 -Wl,--start-group "$PSX_LIB" -lm -lc -lgcc -Wl,--end-group || return 1
		$OBJCOPY --strip-debug "$od/$n.elf" "$OUT/bin/$n" || return 1
		echo "LD  $n"
	done
}

stage_cmds() {
	local line name dir srcs cflags libs more failed=""
	NEWEST_HDR=$(find "$TOP/include/posix" "$OUT/include" -type f -printf '%T@ %p\n' 2>/dev/null \
	    | sort -n | tail -1 | cut -d' ' -f2-)
	while IFS='|' read -r name dir srcs cflags libs more; do
		[ -z "$name" ] && continue
		if [ -n "$ONLY" ] && ! echo " $ONLY " | grep -q " $name "; then
			continue
		fi
		if [ "$dir" = "@TSH" ]; then
			if ! build_tsh > "$OUT/obj/$name.log" 2>&1; then
				failed="$failed $name"
				echo "FAIL $name (see $OUT/obj/$name.log)"
			else
				grep '^LD' "$OUT/obj/$name.log" || true
			fi
			continue
		fi
		if ! build_cmd "$name" "$dir" "$srcs" "$cflags" "$libs" "$more" > "$OUT/obj/$name.log" 2>&1; then
			failed="$failed $name"
			echo "FAIL $name (see $OUT/obj/$name.log)"
		else
			grep '^LD' "$OUT/obj/$name.log" || true
		fi
	done <<< "$CMDS"
	# where each command comes from, for the text of its object (tools/netbsd/mkcmdobj.py)
	cat "$OUT"/obj/*/origin 2>/dev/null | sort > "$OUT/origin.txt"
	if [ -n "$failed" ]; then
		echo "cmds: not built:$failed"
		return 1
	fi
}

# ---------------------------------------------------------------- notices

stage_notices() {
	local f
	: > "$OUT/NOTICE-netbsd-cmds.txt"
	for f in $(find "$OUT/obj" -name '*.d' -exec cat {} + 2>/dev/null | tr ' \\' '\n\n' | grep "^$NB/" | sort -u); do
		echo "==> ${f#$NB/}" >> "$OUT/NOTICE-netbsd-cmds.txt"
		sed -n '1,/^\s*\*\//p' "$f" | head -60 >> "$OUT/NOTICE-netbsd-cmds.txt"
	done
	echo "notices: $OUT/NOTICE-netbsd-cmds.txt"
}

case $STAGE in
fetch)		stage_fetch ;;
compat)		stage_host; stage_compat ;;
cmds)		mkdir -p "$OUT/obj"; stage_cmds ;;
notices)	stage_notices ;;
all)		stage_fetch; stage_host; stage_compat; mkdir -p "$OUT/obj"; stage_cmds; stage_notices ;;
clean)		rm -rf "$OUT" ;;
*)		echo "usage: $0 [fetch|compat|cmds|notices|all|clean] [name ...]" >&2; exit 2 ;;
esac
