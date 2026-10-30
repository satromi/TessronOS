#!/bin/bash
#
# tools/x64/acpica/build.sh — the ACPI interpreter of the x64 kernel (docs/tessronos/03-platform.md
# 3.10.6): ACPICA's release taken from its project, built as the kernel is built (x86_64-elf,
# no floating point, no red zone, the kernel's code model) with the OS layer and the kernel's use
# of it (kernel/sysdepend/acpi/acpica/), into one object ~/tessronos-acpica-x64/acpica.o that the
# kernel links (build_make/x64.mk, ACPICA_OBJ). Only the functions the kernel calls
# (knl_acpica_*) and the OS layer's (AcpiOs*) stay global; the interpreter's own, its small C
# library among them, are made local so that they meet none of the kernel's.
#
#   tools/x64/acpica/build.sh [fetch|compile|link|all|iasl|clean]
#
# The sources are not changed but for one line of a copy of include/platform/acenv.h, which
# chooses kernel/sysdepend/acpi/acpica/actessronos.h for __TESSRONOS_KERNEL__. The release is kept
# in ~/acpica (ACPICA_HOME); its licence is ACPICA's dual BSD-3-Clause / GPL-2.0 one, under the
# BSD terms here (LICENSE.BSD-3-Clause of the release, written beside the object).
#
set -e

STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
CC=$XB/x86_64-elf-gcc
LD=$XB/x86_64-elf-ld
OBJCOPY=$XB/x86_64-elf-objcopy
NM=$XB/x86_64-elf-nm
JOBS=${JOBS:-12}

VER=20260930
URL=${ACPICA_URL:-https://github.com/open-acpica/acpica/releases/download/$VER/acpica-unix-$VER.tar.gz}
SHA256=aa18901b92e30749be0edc3081c8d550c61fce4fa37546fc6a65d367a4ae71a5
AH=${ACPICA_HOME:-$HOME/acpica}
SRC=$AH/acpica-unix-$VER/source
OUT=${OUT:-$HOME/tessronos-acpica-x64}
WORK=$OUT/work
GLUE=$TOP/kernel/sysdepend/acpi/acpica

# the parts of the interpreter the kernel runs: no debugger, disassembler or compiler
DIRS="dispatcher events executer hardware namespace parser resources tables utilities"
# files only a debugging build of the interpreter has use for
SKIP="/(rsdump)\.c$"

CFLAGS="-march=x86-64-v2 -mgeneral-regs-only -mno-red-zone -mcmodel=kernel -O2 -g
 -ffreestanding -fno-builtin -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -fno-strict-aliasing -std=gnu11 -Wall -Wno-unused -Wno-format -Wno-format-security
 -Wno-unused-parameter -Werror=implicit-function-declaration -Werror=int-conversion
 -D__TESSRONOS_KERNEL__ -DACPI_USE_BUILTIN_STDARG
 -I$WORK/include -I$GLUE"

fetch() {
	mkdir -p "$AH/dl"
	local f=$AH/dl/acpica-unix-$VER.tar.gz
	if [ ! -f "$f" ] || ! echo "$SHA256  $f" | sha256sum -c --status; then
		curl -sSL -m 600 -o "$f.part" "$URL"
		echo "$SHA256  $f.part" | sha256sum -c --status || { echo "acpica: the checksum does not match: $URL" >&2; exit 1; }
		mv "$f.part" "$f"
	fi
	if [ ! -d "$SRC" ]; then
		tar xzf "$f" -C "$AH"
	fi
}

compile() {
	# the headers copied, the one change made in the copy: TessronOS's configuration chosen
	# first (acpi.h includes "platform/acenv.h" from beside itself, so the copy is used whole)
	rm -rf "$WORK/include"
	mkdir -p "$WORK/obj"
	cp -r "$SRC/include" "$WORK/include"
	sed 's|^#if defined(_LINUX) \|\| defined(__linux__)$|#if defined(__TESSRONOS_KERNEL__)\n#include "actessronos.h"\n\n#elif defined(_LINUX) \|\| defined(__linux__)|' \
		"$WORK/include/platform/acenv.h" > "$WORK/include/platform/acenv.h.new"
	grep -q actessronos "$WORK/include/platform/acenv.h.new" || { echo "acpica: acenv.h is not laid out as expected" >&2; exit 1; }
	mv "$WORK/include/platform/acenv.h.new" "$WORK/include/platform/acenv.h"
	local list=$WORK/sources.txt
	: > "$list"
	for d in $DIRS; do
		ls "$SRC/components/$d"/*.c | grep -v -E "$SKIP" >> "$list"
	done
	ls "$GLUE"/*.c >> "$list"
	xargs -P "$JOBS" -n 1 -a "$list" sh -c '
		src=$1; o='"$WORK"'/obj/$(basename "$(dirname "$src")")_$(basename "$src" .c).o
		if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ '"$GLUE"'/actessronos.h -nt "$o" ] || [ '"$GLUE"'/tsacpi_os.h -nt "$o" ]; then
			'"$CC"' '"$(echo $CFLAGS)"' -c "$src" -o "$o" || exit 255
		fi' _
	echo "acpica: $(wc -l < "$list") sources compiled"
}

link() {
	local o=$OUT/acpica.o
	$LD -r -o "$o.all" "$WORK"/obj/*.o
	# global: what the kernel calls and what the OS layer and the kernel give each other
	$NM -g --defined-only "$o.all" | awk '{print $3}' | grep -E '^(knl_acpica_|AcpiOs)' > "$WORK/keep.txt"
	$OBJCOPY --keep-global-symbols="$WORK/keep.txt" "$o.all" "$o"
	rm -f "$o.all"
	cp "$SRC/../LICENSE.BSD-3-Clause" "$OUT/LICENSE-acpica.txt" 2>/dev/null || true
	local undef
	undef=$($NM -u "$o" | awk '{print $2}' | sort -u | tr '\n' ' ')
	echo "acpica: $o ($(du -k "$o" | cut -f1) KB); it needs of the kernel: $undef"
}

# iasl for the machine the build runs on: the test tables (tools/x64/tests/*.asl) compiled, a
# machine's DSDT and SSDTs (its \EFI\TESSRON\ACPI) taken apart. Built in the release's own tree.
iasl() {
	( cd "$AH/acpica-unix-$VER" && make -s iasl >/dev/null )
	mkdir -p "$OUT"
	cp "$AH/acpica-unix-$VER/generate/unix/bin/iasl" "$OUT/iasl"
	echo "acpica: $OUT/iasl"
}

case $STAGE in
fetch)		fetch ;;
iasl)		fetch; iasl ;;
compile)	fetch; compile ;;
link)		link ;;
all)		fetch; compile; link ;;
clean)		rm -rf "$OUT" ;;
*)		echo "usage: $0 [fetch|compile|link|all|iasl|clean]" >&2; exit 2 ;;
esac
