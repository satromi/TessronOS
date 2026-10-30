#!/bin/bash
#
# tools/mesa/shaders.sh — GPU でウインドウを合成するシェーダを、ホストで V3D 7.1 の
# QPU のコードに翻訳し、カーネルが読む C の配列にする(第13章 13.10)
#
#   tools/mesa/shaders.sh [fetch|build|gen|test|all|clean]
#
# fetch  Mesa のリリース(MESA_VER)を MESA_HOME に取ってきて展開する(SHA-256 を検査する)
# build  Mesa の NIR、V3D のシェーダコンパイラ、QPU、src/util をホスト向けに建てて
#        静的ライブラリにし、tools/mesa/gen_cp_shaders.c とリンクする
# gen    できたプログラムを走らせ、device/gpu/v3d/mesa/v3d_cp_shaders.h を書く
# test   シェーダを NIR のままホストで解釈して走らせ、ホスト向けに建てた compose.c と
#        乱数の場面で突き合わせる(tools/mesa/test_cp_shaders.c、NSCENE 個、既定 200)
# all    build、gen、test(既定)
# clean  WORK を消す(Mesa の展開物は残す)
#
# Mesa はカーネルにリンクしない。カーネルに入るのは生成したヘッダだけで、
# 先頭に MIT の許諾文が付く。Mesa の生成スクリプトは Python の mako を使う。

set -e

TOP=$(cd "$(dirname "$0")/../.." && pwd)
MESA_VER=${MESA_VER:-26.2.3}
MESA_SHA256=${MESA_SHA256:-1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f}
MESA_HOME=${MESA_HOME:-$HOME/mesa}
MESA=$MESA_HOME/mesa-$MESA_VER
WORK=${WORK:-$MESA_HOME/cpshaders}
GEN=$WORK/gen
JOBS=${JOBS:-12}
OUT=$TOP/device/gpu/v3d/mesa/v3d_cp_shaders.h

HOST_CFLAGS="-O1 -g0 -std=gnu11 -w -fPIC -DHAVE_PTHREAD=1 -DHAVE_ENDIAN_H=1 \
 -DHAVE_STRUCT_TIMESPEC=1 -DHAVE_TIMESPEC_GET=1 -D_GNU_SOURCE -DHAVE_DLFCN_H=1 \
 -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_SECURE_GETENV=1 -DHAVE_SYSCONF=1 \
 -DHAVE_GNU_QSORT_R=1 -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
 -DPACKAGE_VERSION=\"$MESA_VER\" -DPACKAGE_BUGREPORT=\"\" -DV3D_VERSION=71"

MESA_INC="-I$GEN/src -I$GEN/src/compiler -I$GEN/src/compiler/nir -I$GEN/src/util/format \
 -I$GEN -I$GEN/broadcom -I$MESA/include -I$MESA/src -I$MESA/src/compiler -I$MESA/src/compiler/nir \
 -I$MESA/src/util -I$MESA/src/util/format -I$MESA/src/broadcom -I$MESA/src/broadcom/compiler \
 -I$MESA/src/broadcom/common -I$MESA/src/gallium/include -I$MESA/src/gallium/auxiliary \
 -I$WORK/hshim"

log() { echo "shaders: $*"; }

# libdrm's declarations from the shim (tools/mesa/shim), alone: the rest of the shim stands in for
# newlib's headers, which the host's C library has of its own
hshim() {
	mkdir -p "$WORK/hshim"
	cp "$TOP/tools/mesa/shim/xf86drm.h" "$TOP/tools/mesa/shim/xf86drmMode.h" "$TOP/tools/mesa/shim/libsync.h" \
		"$WORK/hshim/"
}

fetch() {
	mkdir -p "$MESA_HOME"
	if [ ! -d "$MESA" ]; then
		log "fetching Mesa $MESA_VER"
		(cd "$MESA_HOME" && curl -sSLO "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz" \
			&& echo "$MESA_SHA256  mesa-$MESA_VER.tar.xz" | sha256sum -c - \
			&& tar xf "mesa-$MESA_VER.tar.xz" && rm -f "mesa-$MESA_VER.tar.xz")
	fi
	python3 -c 'import mako' 2>/dev/null || { log "python3 needs mako (pip3 install --user mako)"; exit 1; }
}

generate() {
	local N=$MESA/src/compiler/nir O=$GEN/src/compiler/nir
	mkdir -p "$O" "$GEN/src/util/format" "$GEN/src/compiler" "$GEN/broadcom/cle" \
		"$GEN/cle" "$GEN/broadcom/compiler"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" --enums \
		> "$GEN/src/util/format/u_format_gen.h"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" --header \
		> "$GEN/src/util/format/u_format_pack.h"
	python3 "$MESA/src/util/format/u_format_table.py" "$MESA/src/util/format/u_format.yaml" \
		> "$GEN/src/util/format/u_format_table.c"
	python3 "$MESA/src/util/format_srgb.py" > "$GEN/src/util/format_srgb.c"
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
	for v in 42 71; do
		python3 "$MESA/src/broadcom/cle/gen_pack_header.py" "$MESA/src/broadcom/cle/v3d_packet.xml" $v \
			> "$GEN/cle/v3d_packet_v${v}_pack.h"
		cp "$GEN/cle/v3d_packet_v${v}_pack.h" "$GEN/broadcom/cle/"
	done
	python3 "$MESA/src/broadcom/compiler/v3d_nir_lower_algebraic.py" -p "$N" \
		> "$GEN/broadcom/compiler/v3d_nir_lower_algebraic.c"
}

compile_one() {
	local f=$1 b
	b=$(echo "$f" | sed -e "s|^$MESA/src/||" -e "s|^$GEN/||" -e 's|/|_|g' -e 's|\.c$||')
	if [ "$f" -nt "$OBJ/$b.o" ] || [ ! -f "$OBJ/$b.o" ]; then
		gcc $HOST_CFLAGS $MESA_INC -c -o "$OBJ/$b.o" "$f" 2> "$OBJ/$b.log" || true
		[ -s "$OBJ/$b.log" ] || rm -f "$OBJ/$b.log"
	fi
}

# Compile a list of sources and put what compiled into lib$1.a
compile_lib() {
	local name=$1 srcs=$2
	OBJ=$WORK/obj/$name
	mkdir -p "$OBJ"
	export HOST_CFLAGS MESA_INC OBJ MESA GEN
	export -f compile_one
	echo "$srcs" | xargs -P "$JOBS" -n 1 bash -c 'compile_one "$1"' _
	local total ok
	total=$(echo "$srcs" | wc -l)
	ok=$(ls "$OBJ"/*.o 2>/dev/null | wc -l)
	log "$name: $ok of $total files compiled"
	if [ "$ok" -lt "$total" ]; then
		for f in "$OBJ"/*.log; do
			[ -f "$f" ] && echo "  $(basename "$f" .log): $(grep -m1 'error' "$f" | sed 's/^[^ ]* //')"
		done | head -20
	fi
	rm -f "$WORK/lib$name.a"
	ar rcs "$WORK/lib$name.a" "$OBJ"/*.o
}

build() {
	fetch
	mkdir -p "$WORK"
	hshim
	[ -f "$GEN/broadcom/compiler/v3d_nir_lower_algebraic.c" ] || generate
	compile_lib nir "$(ls "$MESA"/src/compiler/nir/*.c "$GEN"/src/compiler/nir/*.c \
		"$GEN"/src/compiler/builtin_types.c "$MESA"/src/compiler/glsl_types.c \
		"$MESA"/src/compiler/shader_enums.c)"
	compile_lib v3dcc "$(ls "$MESA"/src/broadcom/compiler/*.c "$MESA"/src/broadcom/qpu/qpu_*.c \
		"$GEN"/broadcom/compiler/*.c "$MESA"/src/broadcom/common/v3d_util.c \
		"$MESA"/src/broadcom/common/v3d_debug.c)"
	compile_lib util "$(ls "$MESA"/src/util/*.c "$MESA"/src/util/format/*.c "$GEN"/src/util/format/*.c \
		"$GEN"/src/util/format_srgb.c \
		"$MESA"/src/util/blake3/blake3.c "$MESA"/src/util/blake3/blake3_dispatch.c \
		"$MESA"/src/util/blake3/blake3_portable.c \
		| grep -v -e cache_ops_x86 -e cache_ops_aarch64 -e streaming-load-memcpy \
			-e xmlconfig -e u_sync_provider)"
	gcc $HOST_CFLAGS -Wall -Wno-unused-function $MESA_INC -o "$WORK/gen_cp_shaders" \
		"$TOP/tools/mesa/gen_cp_shaders.c" \
		-Wl,--start-group "$WORK/libv3dcc.a" "$WORK/libnir.a" "$WORK/libutil.a" -Wl,--end-group \
		-lpthread -lm -ldl
	log "built $WORK/gen_cp_shaders"
}

test() {
	[ -f "$WORK/libnir.a" ] || build
	hshim
	gcc -O1 -g0 -std=gnu11 -Wall -Wno-unused-function -I"$TOP/tools/mesa/hostshim" \
		-c -o "$WORK/compose_host.o" "$TOP/outer_kernel/wm/compose.c"
	gcc -O1 -g0 -std=gnu11 -Wall -Wno-unused-function -I"$TOP/tools/mesa/hostshim" \
		-c -o "$WORK/host_compose.o" "$TOP/tools/mesa/hostshim/host_compose.c"
	gcc $HOST_CFLAGS -I"$TOP/tools/mesa" $MESA_INC -I"$TOP/tools/mesa/hostshim" \
		-o "$WORK/test_cp_shaders" "$TOP/tools/mesa/test_cp_shaders.c" \
		"$WORK/compose_host.o" "$WORK/host_compose.o" \
		-Wl,--start-group "$WORK/libv3dcc.a" "$WORK/libnir.a" "$WORK/libutil.a" -Wl,--end-group \
		-lpthread -lm -ldl
	"$WORK/test_cp_shaders" "${NSCENE:-200}"
}

gen() {
	[ -x "$WORK/gen_cp_shaders" ] || build
	(cd "$WORK" && ./gen_cp_shaders "$OUT.tmp" "$MESA_VER")
	mv "$OUT.tmp" "$OUT"
	log "wrote $OUT ($(wc -l < "$OUT") lines); the disassembly is in $WORK/v3d_cp_shaders.dis"
}

case "${1:-all}" in
fetch)	fetch ;;
build)	build ;;
gen)	gen ;;
test)	test ;;
all)	build; gen; test ;;
clean)	rm -rf "$WORK" ;;
*)	echo "usage: $0 [fetch|build|gen|test|all|clean]"; exit 2 ;;
esac
