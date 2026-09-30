#!/bin/bash
#
# tools/mesa/probe.sh — Mesa の一部を TessronOS のツールチェーンで建ててみる(第13章 13.6.4)
#
#   tools/mesa/probe.sh [fetch|cle|nir|v3dcc|all|clean]
#
# fetch  Mesa のリリース(MESA_VER)を WORK に取ってきて展開する
# cle    V3D 7.1 の制御リストのパケットの詰め込みを生成し、cle_probe.c を
#        ホスト向けに建てて走らせ、TessronOS のプロセス向けに建ててリンクする
# nir    NIR のソースを生成し、src/compiler/nir の .c をすべて aarch64 向けに
#        コンパイルして、通った数と通らなかった理由を数える(リンクはしない)
# v3dcc  V3D のシェーダコンパイラと QPU、src/util、Gallium の v3d ドライバを
#        同じようにコンパイルする。DRM のライブラリの宣言は tools/mesa/shim が補う
# all    cle、nir、v3dcc の順に全部
# pack   V3D 7.1 のパケットの詰め込みを生成し、MIT の許諾文を付けて
#        device/gpu/v3d/mesa/v3d_packet_v71_pack.h に書く(カーネルの試験が使う)
# clean  WORK を消す
#
# Mesa の生成スクリプトは Python の mako を使う(pip3 install --user mako)。
# 成果物と記録は WORK(既定は /tmp/tsmesa)にだけ置き、リポジトリには pack だけが書く。

set -e

TOP=$(cd "$(dirname "$0")/../.." && pwd)
MESA_VER=${MESA_VER:-26.2.3}
MESA_SHA256=${MESA_SHA256:-1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f}
WORK=${WORK:-/tmp/tsmesa}
MESA=$WORK/mesa-$MESA_VER
GEN=$WORK/gen
CROSS=${CROSS:-aarch64-none-elf-}
JOBS=${JOBS:-8}

# TessronOS のプロセス向けの C の旗(build_make/cxxrt.mk の TSPOSIX_CFLAGS と同じ並び)
TS_CFLAGS="-march=armv8.2-a -O2 -g -fno-stack-protector -std=gnu11 \
 -DCHK_TKERNEL_CONST=(1) -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L \
 -D_POSIX_PRIORITY_SCHEDULING=1 \
 -I$TOP/include/tscxx -I$TOP/include/tsposix -I$TOP/include -I$TOP/config"

# Mesa を TessronOS 向けに建てるときの構成の定義。meson が Linux で渡すものの
# うち、newlib と TessronOS にあるものだけを残す
MESA_DEFS="-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_PTHREAD=1 \
 -DHAVE_STRUCT_TIMESPEC=1 -DHAVE_TIMESPEC_GET=1 -D_GNU_SOURCE \
 -DPACKAGE_VERSION=\"$MESA_VER\" -DPACKAGE_BUGREPORT=\"\" -DNDEBUG"

MESA_INC="-I$GEN/src -I$GEN/src/compiler -I$GEN/src/compiler/nir -I$GEN/src/util/format \
 -I$MESA/include -I$MESA/src -I$MESA/src/compiler -I$MESA/src/compiler/nir \
 -I$MESA/src/util -I$MESA/src/util/format -I$MESA/src/broadcom -I$MESA/src/broadcom/compiler \
 -I$MESA/src/gallium/include -I$MESA/src/gallium/auxiliary -I$GEN"

log() { echo "probe: $*"; }

fetch() {
	mkdir -p "$WORK"
	if [ ! -d "$MESA" ]; then
		log "fetching Mesa $MESA_VER"
		(cd "$WORK" && curl -sSLO "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz" \
			&& echo "$MESA_SHA256  mesa-$MESA_VER.tar.xz" | sha256sum -c - \
			&& tar xf "mesa-$MESA_VER.tar.xz")
	fi
	python3 -c 'import mako' 2>/dev/null || { log "python3 needs mako (pip3 install --user mako)"; exit 1; }
}

cle() {
	fetch
	mkdir -p "$GEN/cle"
	python3 "$MESA/src/broadcom/cle/gen_pack_header.py" "$MESA/src/broadcom/cle/v3d_packet.xml" 71 \
		> "$GEN/cle/v3d_packet_v71_pack.h"
	log "generated $(wc -l < "$GEN/cle/v3d_packet_v71_pack.h") lines of V3D 7.1 packet packing"

	gcc -std=gnu11 -O1 -DHAVE_ENDIAN_H $MESA_INC -o "$WORK/cle_probe_host" "$TOP/tools/mesa/cle_probe.c"
	log "host run:"
	"$WORK/cle_probe_host"

	make -s -C "$TOP/build_make" obj/lib/libcxxrt.a obj/lib/libpthread.a >/dev/null
	${CROSS}gcc $TS_CFLAGS $MESA_DEFS $MESA_INC -c -o "$WORK/cle_probe.o" "$TOP/tools/mesa/cle_probe.c"
	L=$TOP/build_make/obj/lib
	${CROSS}g++ -march=armv8.2-a -nostdlib -nostartfiles -static -T "$TOP/etc/linker/user/program_cxx.ld" \
		-Wl,--build-id=none -Wl,--gc-sections -o "$WORK/cle_probe.elf" \
		"$(${CROSS}gcc -print-file-name=crti.o)" "$(${CROSS}gcc -print-file-name=crtbegin.o)" \
		"$WORK/cle_probe.o" -Wl,--whole-archive "$L/libcxxrt.a" "$L/libpthread.a" -Wl,--no-whole-archive \
		-Wl,--start-group -lstdc++ -lm -lc -lgcc -Wl,--end-group \
		"$(${CROSS}gcc -print-file-name=crtend.o)" "$(${CROSS}gcc -print-file-name=crtn.o)"
	log "TessronOS process: $(${CROSS}size "$WORK/cle_probe.elf" | tail -1)"
}

gen_nir() {
	local N=$MESA/src/compiler/nir O=$GEN/src/compiler/nir
	mkdir -p "$O" "$GEN/src/util/format" "$GEN/src/compiler"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" --enums \
		> "$GEN/src/util/format/u_format_gen.h"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" --header \
		> "$GEN/src/util/format/u_format_pack.h"
	python3 "$MESA/src/compiler/builtin_types_h.py" "$GEN/src/compiler/builtin_types.h"
	python3 "$MESA/src/compiler/builtin_types_c.py" "$GEN/src/compiler/builtin_types.c"
	python3 "$N/nir_builder_opcodes_h.py" > "$O/nir_builder_opcodes.h"
	python3 "$N/nir_constant_expressions.py" > "$O/nir_constant_expressions.c"
	python3 "$N/nir_opcodes_h.py" > "$O/nir_opcodes.h"
	python3 "$N/nir_opcodes_c.py" > "$O/nir_opcodes.c"
	python3 "$N/nir_opt_algebraic.py" --out "$O/nir_opt_algebraic.c"
	python3 "$N/nir_intrinsics_h.py" --out "$O/nir_intrinsics.h"
	python3 "$N/nir_intrinsics_indices_h.py" --out "$O/nir_intrinsics_indices.h"
	python3 "$N/nir_intrinsics_c.py" --out "$O/nir_intrinsics.c"
}

compile_one() {
	local f=$1 b
	b=$(basename "$f" .c)
	if ${CROSS}gcc $TS_CFLAGS $MESA_DEFS $MESA_INC -w -c -o "$OBJ/$b.o" "$f" 2> "$LOGD/$b.log"; then
		rm -f "$LOGD/$b.log"
	fi
}

nir() {
	fetch
	gen_nir
	local OBJ=$WORK/obj/nir LOGD=$WORK/log/nir
	rm -rf "$OBJ" "$LOGD"
	mkdir -p "$OBJ" "$LOGD"
	local SRCS
	SRCS=$(ls "$MESA"/src/compiler/nir/*.c "$GEN"/src/compiler/nir/*.c "$GEN"/src/compiler/builtin_types.c \
		"$MESA"/src/compiler/glsl_types.c "$MESA"/src/compiler/shader_enums.c)
	local start=$(date +%s)
	export CROSS TS_CFLAGS MESA_DEFS MESA_INC OBJ LOGD
	export -f compile_one
	echo "$SRCS" | xargs -P "$JOBS" -n 1 bash -c 'compile_one "$1"' _
	local end=$(date +%s)
	local total ok
	total=$(echo "$SRCS" | wc -l)
	ok=$(ls "$OBJ" | wc -l)
	log "NIR: $ok of $total files compiled for aarch64-none-elf in $((end - start)) s ($JOBS jobs)"
	if [ "$ok" -gt 0 ]; then
		log "code in the objects: $(${CROSS}size -t "$OBJ"/*.o | tail -1)"
	fi
	if [ "$ok" -lt "$total" ]; then
		log "the first error of each file that did not compile, counted:"
		for f in "$LOGD"/*.log; do grep -m1 "error" "$f" | sed 's/^[^ ]* //'; done | sort | uniq -c | sort -rn | head -20
	fi
}

# Compile a list of sources into $1 (a name under obj/ and log/) and report
compile_set() {
	local name=$1 srcs=$2 extra=$3
	OBJ=$WORK/obj/$name
	LOGD=$WORK/log/$name
	rm -rf "$OBJ" "$LOGD"
	mkdir -p "$OBJ" "$LOGD"
	local start=$(date +%s)
	local SAVE_DEFS=$MESA_DEFS
	MESA_DEFS="$MESA_DEFS $extra"
	export CROSS TS_CFLAGS MESA_DEFS MESA_INC OBJ LOGD
	export -f compile_one
	echo "$srcs" | xargs -P "$JOBS" -n 1 bash -c 'compile_one "$1"' _
	MESA_DEFS=$SAVE_DEFS
	local end=$(date +%s) total ok
	total=$(echo "$srcs" | wc -l)
	ok=$(ls "$OBJ" | wc -l)
	log "$name: $ok of $total files compiled in $((end - start)) s ($JOBS jobs)"
	if [ "$ok" -gt 0 ]; then
		log "code in the objects: $(${CROSS}size -t "$OBJ"/*.o | tail -1)"
	fi
	if [ "$ok" -lt "$total" ]; then
		for f in "$LOGD"/*.log; do echo "  $(basename "$f" .log): $(grep -m1 'error' "$f" | sed 's/^[^ ]* //')"; done | head -20
	fi
}

# The V3D shader compiler (NIR to QPU code) and the utilities under it
v3dcc() {
	fetch
	[ -f "$GEN/src/compiler/nir/nir_opcodes.h" ] || gen_nir
	mkdir -p "$GEN/cle" "$GEN/broadcom/cle"
	for v in 42 71; do
		python3 "$MESA/src/broadcom/cle/gen_pack_header.py" "$MESA/src/broadcom/cle/v3d_packet.xml" $v \
			> "$GEN/cle/v3d_packet_v${v}_pack.h"
		cp "$GEN/cle/v3d_packet_v${v}_pack.h" "$GEN/broadcom/cle/"
	done
	mkdir -p "$GEN/broadcom/compiler"
	python3 "$MESA/src/broadcom/compiler/v3d_nir_lower_algebraic.py" -p "$MESA/src/compiler/nir" \
		> "$GEN/broadcom/compiler/v3d_nir_lower_algebraic.c"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" \
		> "$GEN/src/util/format/u_format_table.c"
	compile_set v3dcc "$(ls "$MESA"/src/broadcom/compiler/*.c "$MESA"/src/broadcom/qpu/*.c \
		"$GEN"/broadcom/compiler/*.c)" "-DV3D_VERSION=71"
	compile_set util "$(ls "$MESA"/src/util/*.c "$MESA"/src/util/format/*.c "$GEN"/src/util/format/*.c \
		| grep -v -e cache_ops_x86 -e streaming-load-memcpy)"
	# the Gallium driver, with the DRM library's calls declared by tools/mesa/shim
	# (the v3dx_ files once for each hardware version, here 7.1 only)
	compile_set gallium_v3d "$(ls "$MESA"/src/gallium/drivers/v3d/*.c | grep -v /v3dx_)" \
		"-I$TOP/tools/mesa/shim -I$MESA/src/gallium/drivers/v3d"
	compile_set gallium_v3dx "$(ls "$MESA"/src/gallium/drivers/v3d/v3dx_*.c)" \
		"-DV3D_VERSION=71 -I$TOP/tools/mesa/shim -I$MESA/src/gallium/drivers/v3d"
}

pack() {
	fetch
	local out=$TOP/device/gpu/v3d/mesa/v3d_packet_v71_pack.h
	mkdir -p "$(dirname "$out")"
	{
		sed -e "s/@VER@/$MESA_VER/" "$TOP/tools/mesa/pack_notice.txt"
		python3 "$MESA/src/broadcom/cle/gen_pack_header.py" "$MESA/src/broadcom/cle/v3d_packet.xml" 71
	} > "$out"
	log "wrote $out ($(wc -l < "$out") lines)"
}

case "${1:-all}" in
fetch)	fetch ;;
cle)	cle ;;
nir)	nir ;;
v3dcc)	v3dcc ;;
all)	cle; nir; v3dcc ;;
pack)	pack ;;
clean)	rm -rf "$WORK" ;;
*)	echo "usage: $0 [fetch|cle|nir|v3dcc|all|pack|clean]"; exit 2 ;;
esac
