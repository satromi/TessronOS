#!/bin/bash
#
# tools/x64/mesa/build.sh — tools/mesa/build.sh の x64 の PC 向け(docs/tessronos/03-platform.md 3.10):
# 同じ Mesa のリリースとパッチ、シム、移植層(lib/libgl)を、x86_64-elf のプロセス用の
# コンパイラ(x86-64-v2)で建て、ドライバは softpipe と iris(Intel の第9世代以降。V3D は Raspberry Pi のもの)にする。
# 作業物は WORK(既定 ~/tsmesa-x64)。段階の意味は tools/mesa/build.sh と同じ
#
#   tools/x64/mesa/build.sh [fetch|tree|setup|gen|compile|port|lib|all|clean]
#
# fetch    Mesa のリリース(MESA_VER)を MESA_HOME に取ってきて展開する(SHA-256 を検査する)
# tree     展開したソースの写しを WORK/src に作り(ファイルはハードリンク)、
#          tools/mesa/patches のパッチを当て、tools/mesa/src のファイル(EGL の
#          プラットフォーム tessronos)を足す。パッチが当たるファイルだけは写しを
#          別のファイルにしてから書き換えるので、展開したソースは変わらない
# setup    ホスト(Linux)で meson を本番と同じオプションで実行する。コンパイラは TessronOS の
#          プロセス用のもの(aarch64-none-elf-gcc と include/tsposix・include/tscxx)にし、
#          libdrm は DRM シムの宣言(tools/mesa/shim)を指す pkg-config の記述で見せる
# gen      meson が書いた生成のコマンド(CUSTOM_COMMAND)をすべて ninja で走らせ、
#          生成物(ヘッダ、表、GLSL の字句・構文解析器など)を作る
# compile  compile_commands.json のうち、libEGL・libGLESv2・Gallium のライブラリに入る
#          ものを TessronOS のプロセス用のコマンドに書き換えてコンパイルする
#          (tools/mesa/translate.py)
# port     移植層(lib/libgl: OS の層と DRM シム)をコンパイルする
# lib      全部を一つの静的ライブラリ OUT/libtsgl.a にまとめ、アプリが読むヘッダを
#          OUT/include に置く
# all      tree から lib まで(既定)。変わったものだけ作り直す
# clean    WORK を消す(Mesa の展開物は残す)
#
# 作業物はすべて WORK(既定 ~/tsmesa)に置く。C: の空きが MINFREE_GB を切ったら止まる。
# ドライバは GALLIUM_DRIVERS(既定 softpipe,iris)。

set -e

TOP=$(cd "$(dirname "$0")/../../.." && pwd)
HERE=$TOP/tools/mesa
XHERE=$TOP/tools/x64/mesa
MESA_VER=${MESA_VER:-26.2.3}
MESA_SHA256=${MESA_SHA256:-1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f}
MESA_HOME=${MESA_HOME:-$HOME/mesa}
MESA=${MESA:-$MESA_HOME/mesa-$MESA_VER}
WORK=${WORK:-$HOME/tsmesa-x64}
SRC=$WORK/src
BUILD=$WORK/build
OBJ=$WORK/obj
OUT=${OUT:-$WORK}
XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
CROSS=$XB/x86_64-elf-
JOBS=${JOBS:-12}
MINFREE_GB=${MINFREE_GB:-3}
GALLIUM_DRIVERS=${GALLIUM_DRIVERS:-softpipe,iris}
# The options of the build (13.6.1): EGL and OpenGL ES 2 and 3 only, no window
# system of Linux's, no LLVM, no shader cache on disk, driconf's defaults only
MESON_OPTS="-Dbuildtype=plain -Doptimization=2 -Ddebug=false -Db_ndebug=true \
 -Dplatforms= -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
 -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dvulkan-drivers= \
 -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled -Dzlib=disabled \
 -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
 -Dbuild-tests=false -Dtools= -Dgallium-va=disabled"
# iris (Intel's 9th generation and later): Gfx9 only (no ELK compiler for Gfx8), no ray
# tracing, no virtio; the internal shaders of the drivers (OpenCL C) are not built, so no
# LLVM and clang are needed on the host: mesa_clc and vtn_bindgen2 are stand-ins
# (tools/x64/mesa/clcstub) and iris never uses what they stand for (patches/iris-no-generation.patch)
case ",$GALLIUM_DRIVERS," in
*,iris,*)
	MESON_OPTS="$MESON_OPTS -Dmesa-clc=system -Dprecomp-compiler=system -Dintel-elk=false -Dintel-rt=disabled"
	;;
esac
. "$TOP/tools/env.sh"
export PATH=$XHERE/clcstub:$HOME/.local/bin:$PATH

log() { echo "mesa: $*"; }

disk_guard() {
	local free_gb
	free_gb=$(df -BG /mnt/c 2>/dev/null | awk 'NR==2 { sub("G", "", $4); print $4 }')
	if [ -n "$free_gb" ] && [ "$free_gb" -lt "$MINFREE_GB" ]; then
		log "only ${free_gb}GB left on C:, stopping"
		exit 1
	fi
}

fetch() {
	mkdir -p "$MESA_HOME"
	if [ ! -d "$MESA" ]; then
		log "fetching Mesa $MESA_VER"
		(cd "$MESA_HOME" && curl -sSLO "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz" \
			&& echo "$MESA_SHA256  mesa-$MESA_VER.tar.xz" | sha256sum -c - \
			&& tar xf "mesa-$MESA_VER.tar.xz" && rm -f "mesa-$MESA_VER.tar.xz")
	fi
	python3 -c 'import mako, yaml' 2>/dev/null \
		|| { log "python3 needs mako and yaml (pip3 install --user mako pyyaml)"; exit 1; }
	command -v meson >/dev/null && command -v ninja >/dev/null \
		|| { log "meson and ninja are needed (pip3 install --user 'meson>=1.4' ninja)"; exit 1; }
}

# What the patched tree is made from: the release, the patches, and the
# names of the added files (their text is copied in by tree, and only the
# objects made from them are made again)
tree_key() {
	{
		echo "$MESA_VER"
		cat "$HERE"/patches/*.patch "$XHERE"/patches/*.patch 2>/dev/null
		(cd "$HERE/src" 2>/dev/null && find . -type f | sort)
	} | sha1sum | cut -d' ' -f1
}

# The tree made again when the patches changed. A patched file that comes
# out as it was keeps its time, so that only the objects of the files a
# patch changed are made again. The added files (tools/mesa/src, laid out
# as Mesa's src) are copied in when their text differs.
tree() {
	fetch
	local key p f
	key=$(tree_key)
	if [ ! -f "$SRC/.tskey" ] || [ "$(cat "$SRC/.tskey")" != "$key" ]; then
		log "making the patched tree"
		mkdir -p "$WORK"
		rm -rf "$SRC.new"
		cp -al "$MESA" "$SRC.new"
		for p in "$HERE"/patches/*.patch "$XHERE"/patches/*.patch; do
			[ -f "$p" ] || continue
			# a file the patch changes becomes a file of its own first (not a link)
			for f in $(sed -n 's/^+++ b\/\([^\t]*\).*/\1/p' "$p"); do
				if [ -f "$SRC.new/$f" ]; then
					cp "$SRC.new/$f" "$SRC.new/$f.tscopy" && mv "$SRC.new/$f.tscopy" "$SRC.new/$f"
				fi
			done
			patch -s -p1 -d "$SRC.new" -i "$p"
			for f in $(sed -n 's/^+++ b\/\([^\t]*\).*/\1/p' "$p"); do
				if [ -f "$SRC/$f" ] && cmp -s "$SRC/$f" "$SRC.new/$f"; then
					touch -r "$SRC/$f" "$SRC.new/$f"
				fi
			done
		done
		if [ -d "$HERE/src" ]; then
			(cd "$HERE/src" && find . -type f) | while read -r f; do
				mkdir -p "$(dirname "$SRC.new/src/$f")"
				if [ -f "$SRC/src/$f" ]; then
					cp -p "$SRC/src/$f" "$SRC.new/src/$f"
				fi
			done
		fi
		rm -rf "$SRC"
		mv "$SRC.new" "$SRC"
		echo "$key" > "$SRC/.tskey"
	fi
	if [ -d "$HERE/src" ]; then
		(cd "$HERE/src" && find . -type f) | while read -r f; do
			if ! cmp -s "$HERE/src/$f" "$SRC/src/$f"; then
				mkdir -p "$(dirname "$SRC/src/$f")"
				cp "$HERE/src/$f" "$SRC/src/$f"
			fi
		done
	fi
}

# The C flags of a TessronOS program (build_make/cxxrt.mk TSPOSIX_FLAGS), as meson's list
ts_args() {
	local a
	for a in -march=x86-64-v2 -fno-stack-protector \
		"-DCHK_TKERNEL_CONST=(1)" -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L \
		-D_POSIX_PRIORITY_SCHEDULING=1 -D__TESSRONOS__=1 -DEGL_NO_PLATFORM_SPECIFIC_TYPES \
		-I$TOP/tools/mesa/shim -I$TOP/include/tscxx -I$TOP/include/tsposix; do
		printf "'%s', " "$a"
	done
}

# meson only links its probes: x86_64-elf newlib has no crt0.o, so a probe links as a program
# does, with _start of libcxxrt, its crti and crtn (_init, _fini) and the program layout
# (which places __bss_end and the TLS sizes)
ts_link_args() {
	local a L=$TOP/build_make/obj/x64/lib
	for a in -nostartfiles -specs=nosys.specs -T$TOP/etc/linker/x64/program_cxx.ld $L/cxxrt/crti.o $L/cxxrt/crtn.o -L$WORK/stublib -Wl,--start-group $L/libcxxrt.a $L/libpthread.a \
		-lc -Wl,--end-group; do
		printf "'%s', " "$a"
	done
}

setup() {
	tree
	disk_guard
	mkdir -p "$WORK/pkgconfig" "$WORK/stublib"
	# the C runtime and the threads of a TessronOS program, which meson's
	# checks link with, so that a function is found where a program has it
	( . "$TOP/tools/x64/env.sh" && make -s -C "$TOP/build_make" TARGET=_X64_ obj/x64/lib/libcxxrt.a obj/x64/lib/libpthread.a obj/x64/lib/cxxrt/crti.o obj/x64/lib/cxxrt/crtn.o >/dev/null )
	# libdl and librt, which meson looks for when the C library lacks dlopen
	# and clock_gettime: empty, so that it finds them and the functions stay absent
	for l in dl rt; do
		rm -f "$WORK/stublib/lib$l.a"
		${CROSS}ar rcs "$WORK/stublib/lib$l.a"
	done
	# libdrm as far as meson asks: its version, and the shim's declarations
	cat > "$WORK/pkgconfig/libdrm.pc" <<-EOF
	Name: libdrm
	Description: the DRM calls of TessronOS (lib/libgl/gl_drm.c)
	Version: 2.4.125
	Cflags: -I$TOP/tools/mesa/shim
	Libs:
	EOF
	cat > "$WORK/cross.ini" <<-EOF
	[binaries]
	c = '${CROSS}gcc'
	cpp = '${CROSS}g++'
	ar = '${CROSS}ar'
	strip = '${CROSS}strip'
	pkg-config = 'pkg-config'

	[properties]
	pkg_config_libdir = '$WORK/pkgconfig'
	needs_exe_wrapper = true

	[built-in options]
	c_args = [$(ts_args)]
	cpp_args = [$(ts_args)]
	c_link_args = [$(ts_link_args)]
	cpp_link_args = [$(ts_link_args)]

	[host_machine]
	system = 'linux'
	cpu_family = 'x86_64'
	cpu = 'x86_64'
	endian = 'little'
	EOF
	# the description made again keeps the generated files that come out the
	# same at their old times (gen), so that the objects made from them stay
	rm -rf "$BUILD.old"
	[ -d "$BUILD" ] && mv "$BUILD" "$BUILD.old"
	meson setup "$BUILD" "$SRC" --cross-file "$WORK/cross.ini" $MESON_OPTS \
		-Dgallium-drivers="$GALLIUM_DRIVERS" >"$WORK/meson-setup.log" \
		|| { tail -30 "$WORK/meson-setup.log"; exit 1; }
	cfg_key > "$BUILD/.tskey"
}

# What meson's description depends on besides the tree: its options, the
# drivers, and the shim's headers its checks look at
cfg_key() {
	echo "$(tree_key) $GALLIUM_DRIVERS $( (echo "$MESON_OPTS"; cat "$HERE"/shim/*.h "$HERE"/shim/*/*.h) \
		| sha1sum | cut -c1-12)"
}

# Every output of meson's generating commands; none of them runs a program
# built with the cross compiler
gen() {
	tree
	if [ ! -f "$BUILD/.tskey" ] || [ "$(cat "$BUILD/.tskey")" != "$(cfg_key)" ]; then
		setup
	fi
	local outs
	outs=$(ninja -C "$BUILD" -t targets rule CUSTOM_COMMAND | grep -v '^meson-')
	log "generating $(echo "$outs" | wc -l) files"
	(cd "$BUILD" && echo "$outs" | xargs ninja -j "$JOBS" >/dev/null)
	if [ -d "$BUILD.old" ]; then
		(cd "$BUILD" && echo "$outs" | while read -r f; do
			if [ -f "$BUILD.old/$f" ] && cmp -s "$f" "$BUILD.old/$f"; then
				touch -r "$BUILD.old/$f" "$f"
			fi
		done)
		rm -rf "$BUILD.old"
	fi
}

compile() {
	gen
	disk_guard
	python3 "$HERE/translate.py" --build "$BUILD" --obj "$OBJ" --jobs "$JOBS" \
		--objlist "$WORK/mesa_objs.txt" ${MESA_DEBUG:+--debug}
}

# The port's own sources (lib/libgl), with the flags of a TessronOS program
PORT_SRCS="gl_os.c gl_drm.c gl_win.c"
port() {
	mkdir -p "$OBJ/port"
	local f o
	: > "$WORK/port_objs.txt"
	for f in $PORT_SRCS; do
		o=$OBJ/port/${f%.c}.o
		if [ ! -f "$o" ] || [ "$TOP/lib/libgl/$f" -nt "$o" ] \
		   || [ -n "$(find "$TOP/lib/libgl" "$TOP/tools/mesa/shim" "$TOP/include/ts" -name '*.h' -newer "$o" 2>/dev/null | head -1)" ] 		   || [ -n "$(find "$TOP/lib/libgl" -name 'gl_drm_*.c' -newer "$o" 2>/dev/null | head -1)" ]; then
			${CROSS}gcc -march=x86-64-v2 -O2 -g0 \
				-fno-stack-protector -ffunction-sections -fdata-sections -std=gnu11 -Wall \
				'-DCHK_TKERNEL_CONST=(1)' -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L \
				-D_POSIX_PRIORITY_SCHEDULING=1 -D__TESSRONOS__=1 \
				-I"$TOP/tools/mesa/shim" -I"$SRC/include" -I"$TOP/include/tscxx" \
				-I"$TOP/include/tsposix" -I"$TOP/include" -I"$TOP/config" \
				-c "$TOP/lib/libgl/$f" -o "$o"
		fi
		echo "$o" >> "$WORK/port_objs.txt"
	done
}

lib() {
	mkdir -p "$OUT"
	rm -f "$OUT/libtsgl.a"
	cat "$WORK/mesa_objs.txt" "$WORK/port_objs.txt" > "$WORK/all_objs.txt"
	${CROSS}ar rcs "$OUT/libtsgl.a" @"$WORK/all_objs.txt"
	# the headers a program includes: Khronos' EGL, GLES2, GLES3 and KHR
	rm -rf "$OUT/include"
	mkdir -p "$OUT/include"
	cp -r "$SRC/include/EGL" "$SRC/include/GLES2" "$SRC/include/GLES3" "$SRC/include/KHR" "$OUT/include/"
	log "$OUT/libtsgl.a: $(du -h "$OUT/libtsgl.a" | cut -f1), $(wc -l < "$WORK/all_objs.txt") objects"
}

case "${1:-all}" in
fetch)		fetch ;;
tree)		tree ;;
setup)		setup ;;
gen)		gen ;;
compile)	compile ;;
port)		port ;;
lib)		lib ;;
all)		compile; port; lib ;;
clean)		rm -rf "$SRC" "$BUILD" "$OBJ" "$OUT/libtsgl.a" "$OUT/include" ;;
*)		echo "usage: $0 [fetch|tree|setup|gen|compile|port|lib|all|clean]"; exit 2 ;;
esac
