#!/bin/bash
#
# tools/x64/mesa/igt_shaders.sh — GPU でウインドウを合成するシェーダを、ホストで Intel の第9世代の
# レンダーのエンジンのコード(SIMD16 の計算シェーダ)に翻訳し、カーネルが読む C の配列にする
# (第13章 13.11.14)
#
#   tools/x64/mesa/igt_shaders.sh [build|gen|test|all|clean]
#
# build  x64 の Mesa の建て(tools/x64/mesa/build.sh の WORK/build、既定 ~/tsmesa-x64/build)の
#        compile_commands.json から、NIR、src/util、Intel のコンパイラ(brw)、装置の情報、isl を
#        ホストのコンパイラで翻訳し直して静的ライブラリにし(tools/x64/mesa/hostcc.py)、
#        gen_igt_shaders.c とリンクする。シェーダの仕事は V3D と同じ tools/mesa/cp_stages.h
# gen    できたプログラムを走らせ、device/gpu/intel/mesa/igt_cp_shaders.h を書く。逆アセンブルは
#        WORK/igt_cp_shaders.dis に置く
# test   シェーダを計算シェーダにした NIR のままホストで解釈して走らせ、カーネルと同じ並べ方
#        (device/gpu/intel/igt_rplan.c)で乱数の場面を組み立て、ホスト向けに建てた compose.c と
#        全画素を突き合わせる(test_igt_shaders.c、NSCENE 個、既定 200)
# all    build、gen、test(既定)
# clean  WORK を消す
#
# MESA_DEBUG=1 なら Mesa の検査(assert とシェーダの検証)を入れて -O0 で建てる(WORK を分けること)。
# 作業物は WORK(既定 ~/mesa/igtshaders)に置く。Mesa はカーネルにリンクしない。カーネルに
# 入るのは生成したヘッダだけで、先頭に MIT の許諾文が付く。

set -e

TOP=$(cd "$(dirname "$0")/../../.." && pwd)
XHERE=$TOP/tools/x64/mesa
MESA_VER=${MESA_VER:-26.2.3}
MBUILD=${MBUILD:-$HOME/tsmesa-x64/build}
WORK=${WORK:-$HOME/mesa/igtshaders}
JOBS=${JOBS:-8}
OUT=$TOP/device/gpu/intel/mesa/igt_cp_shaders.h

log() { echo "igt_shaders: $*"; }

[ -f "$MBUILD/compile_commands.json" ] || { log "no x64 Mesa build at $MBUILD (tools/x64/mesa/build.sh setup gen)"; exit 1; }

hostcc() {
	python3 "$XHERE/hostcc.py" --build "$MBUILD" --jobs "$JOBS" --hostshim "$WORK/hshim" ${MESA_DEBUG:+--debug} "$@"
}

lib() {
	local name=$1; shift
	hostcc --obj "$WORK/obj/$name" --list "$WORK/$name.txt" "$@" | sed 's/^/  /'
	rm -f "$WORK/lib$name.a"
	ar rcs "$WORK/lib$name.a" $(cat "$WORK/$name.txt")
}

# The flags of a program of ours, as Mesa's compiler is built with, for C
flags() {
	hostcc --obj /tmp --list /dev/null --flags-of intel/compiler/brw/brw_compile_cs.cpp | sed 's/-std=c++17/-std=gnu11/'
}

build() {
	mkdir -p "$WORK/hshim"
	# libdrm's declarations alone: the rest of the shim stands for newlib, the host has its own
	cp "$TOP/tools/mesa/shim/xf86drm.h" "$TOP/tools/mesa/shim/xf86drmMode.h" "$TOP/tools/mesa/shim/libsync.h" \
		"$WORK/hshim/"
	log "Mesa for the host"
	lib intel --exclude /jay/ src/intel/compiler/ src/intel/dev/ src/intel/mda/
	lib isl src/intel/isl/
	lib base src/compiler/nir/ src/compiler/glsl_types src/compiler/shader_enums src/compiler/builtin_types \
		src/util/ src/c11/
	local fl
	fl=$(flags)
	eval gcc $fl -O1 -g0 ${MESA_DEBUG:+-O0 -g} -w -I"$TOP/tools/mesa" -c -o "$WORK/gen_igt_shaders.o" \
		"$XHERE/gen_igt_shaders.c"
	gcc -O1 -c -o "$WORK/hoststub.o" "$XHERE/hoststub.c"
	g++ -o "$WORK/gen_igt_shaders" "$WORK/gen_igt_shaders.o" "$WORK/hoststub.o" \
		-Wl,--start-group "$WORK/libintel.a" "$WORK/libisl.a" "$WORK/libbase.a" -Wl,--end-group \
		-lpthread -lm -ldl -lz
	log "built $WORK/gen_igt_shaders"
}

gen() {
	[ -x "$WORK/gen_igt_shaders" ] || build
	mkdir -p "$(dirname "$OUT")"
	(cd "$WORK" && INTEL_DEBUG=cs ./gen_igt_shaders "$OUT.tmp" "$MESA_VER" 2> "$WORK/igt_cp_shaders.dis") \
		|| { tail -5 "$WORK/igt_cp_shaders.dis"; exit 1; }
	grep -E '^[a-z0-9]+: [0-9]+ bytes' "$WORK/igt_cp_shaders.dis" | sed 's/^/  /' || true
	# the compiler's own check of the code it made (its disassembler marks what the hardware does not take)
	if grep -q -E 'ERROR|Validation failed' "$WORK/igt_cp_shaders.dis"; then
		grep -n -E 'ERROR|Validation failed' "$WORK/igt_cp_shaders.dis" | head -5
		rm -f "$OUT.tmp"
		log "the code did not pass the compiler's check: $OUT left as it was"
		exit 1
	fi
	mv "$OUT.tmp" "$OUT"
	log "wrote $OUT ($(wc -l < "$OUT") lines); the disassembly is in $WORK/igt_cp_shaders.dis"
}

test() {
	[ -f "$WORK/libintel.a" ] || build
	local fl
	fl=$(flags)
	# compose.c and the kernel's plan of the rectangles, built for the host with the kernel's types
	gcc -O1 -g0 -std=gnu11 -w -I"$TOP/tools/mesa/hostshim" -c -o "$WORK/compose_host.o" "$TOP/outer_kernel/wm/compose.c"
	gcc -O1 -g0 -std=gnu11 -w -I"$TOP/tools/mesa/hostshim" -c -o "$WORK/host_compose.o" \
		"$TOP/tools/mesa/hostshim/host_compose.c"
	gcc -O1 -g0 -std=gnu11 -w -DIGT_RPLAN_HOST -I"$XHERE/hostshim" -I"$TOP/tools/mesa/hostshim" \
		-c -o "$WORK/rplan_host.o" "$TOP/device/gpu/intel/igt_rplan.c"
	gcc -O1 -g0 -std=gnu11 -w -I"$XHERE/hostshim" -I"$TOP/tools/mesa/hostshim" \
		-c -o "$WORK/host_rplan.o" "$XHERE/hostshim/host_rplan.c"
	eval gcc $fl -O1 -g0 -w -I"$TOP/tools/mesa" -I"$XHERE" -I"$TOP/tools/mesa/hostshim" -I"$XHERE/hostshim" \
		-c -o "$WORK/test_igt_shaders.o" "$XHERE/test_igt_shaders.c"
	g++ -o "$WORK/test_igt_shaders" "$WORK/test_igt_shaders.o" "$WORK/compose_host.o" "$WORK/host_compose.o" \
		"$WORK/rplan_host.o" "$WORK/host_rplan.o" "$WORK/hoststub.o" \
		-Wl,--start-group "$WORK/libintel.a" "$WORK/libisl.a" "$WORK/libbase.a" -Wl,--end-group \
		-lpthread -lm -ldl -lz
	"$WORK/test_igt_shaders" "${NSCENE:-200}"
}

case "${1:-all}" in
build)	build ;;
gen)	gen ;;
test)	test ;;
all)	build; gen; test ;;
clean)	rm -rf "$WORK" ;;
*)	echo "usage: $0 [build|gen|test|all|clean]"; exit 2 ;;
esac
