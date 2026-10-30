#!/usr/bin/env bash
#
# TessronOS x64 用ツールチェーンの構築(WSL2 Ubuntu / Linux x86_64)
#
#   x86_64-elf          : binutils 2.45.1 + GCC 15.2.0(C, C++) + newlib 4.5.0 + GDB 16.3
#   x86_64-w64-mingw32  : binutils 2.45.1 + GCC 15.2.0(C) + mingw-w64 v13.0.0(UEFI のローダ用)
#   nasm                : NASM 3.01(ブラウザの x64 の dav1d、libvpx、libjpeg-turbo、FFmpeg のアセンブリ用)
#
# 置き場は aarch64 の xPack と同じく $PREFIX(既定 ~/.local/xPacks)の下に
# バージョン付きのディレクトリを作り、バージョンなしの名前のリンクを張る。
#   $PREFIX/tessron-x86_64-elf-gcc-15.2.0-1         -> リンク x86_64-elf-gcc
#   $PREFIX/tessron-x86_64-w64-mingw32-gcc-15.2.0-1 -> リンク x86_64-w64-mingw32-gcc
#   $PREFIX/tessron-nasm-3.01-1                     -> リンク nasm
# root 権限は不要。ホストには gcc、g++、make、bison、flex、m4、curl、gpg、xz、bzip2 が要る。
#
#   tools/x64/build_toolchain.sh             # 全部(fetch hostlibs elf gdb mingw test clean)
#   tools/x64/build_toolchain.sh elf test    # 指定した段だけ
#   tools/x64/build_toolchain.sh nasm        # NASM だけ(全部の段には入れない)
#
# 環境変数
#   TS_XPACK_PREFIX   置き場(既定 ~/.local/xPacks)
#   TS_X64_SRC_CACHE  ダウンロードした tarball の置き場(既定 ~/.cache/tessronos-x64-src、残す)
#   TS_X64_WORK       展開と build の作業域(既定 ~/tessronos-x64-work、clean で消す)
#   JOBS              並列度(既定 8)
#
set -euo pipefail

BINUTILS_VER=2.45.1
GCC_VER=15.2.0
NEWLIB_VER=4.5.0.20241231
GDB_VER=16.3
MINGW_VER=13.0.0
NCURSES_VER=6.5
# NASM は署名もハッシュの表も公開していない。リリースの git.id が Chromium の DEPS が固定する
# NASM(third_party/nasm の README.chromium の 3.01 / 19fd1bb4…)と同じであり、tarball の
# ソースがそのチェックアウトの同じファイルと一致する(違うのは Chromium の手直しと生成した表
# だけ)ことを確かめた上で、tarball の SHA-256 をここに固定する
NASM_VER=3.01
NASM_GITID=19fd1bb434f2fed19b0eb82b1b42c5608b601134
NASM_SHA256=b7324cbe86e767b65f26f467ed8b12ad80e124e3ccb89076855c98e43a9eddd4
PKG_REV=1

PREFIX="${TS_XPACK_PREFIX:-$HOME/.local/xPacks}"
CACHE="${TS_X64_SRC_CACHE:-$HOME/.cache/tessronos-x64-src}"
WORK="${TS_X64_WORK:-$HOME/tessronos-x64-work}"
JOBS="${JOBS:-8}"

ELF_TARGET=x86_64-elf
MGW_TARGET=x86_64-w64-mingw32
ELF_DIR="$PREFIX/tessron-${ELF_TARGET}-gcc-${GCC_VER}-${PKG_REV}"
MGW_DIR="$PREFIX/tessron-${MGW_TARGET}-gcc-${GCC_VER}-${PKG_REV}"
HOST="$WORK/host"
SRC="$WORK/src"
BLD="$WORK/build"
LOG="$WORK/log"

GNU=https://ftp.gnu.org/gnu
GCC_INFRA=https://gcc.gnu.org/pub/gcc/infrastructure
NEWLIB_URL=https://sourceware.org/pub/newlib
MINGW_URL=https://downloads.sourceforge.net/project/mingw-w64/mingw-w64/mingw-w64-release
NASM_URL=https://www.nasm.us/pub/nasm/releasebuilds
NASM_DIR="$PREFIX/tessron-nasm-${NASM_VER}-${PKG_REV}"

PKGVERSION="TessronOS x86_64 GCC ${GCC_VER}-${PKG_REV}"
# x86_64-elf の対象のライブラリ(libgcc、newlib、libstdc++)を作るときの旗
TFLAGS="-g -O2 -fPIC -ffunction-sections -fdata-sections"

say() { printf '== [%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# 段ごとの所要時間を記録する
timed() {
	local name="$1"; shift
	local t0=$SECONDS
	say "$name: start"
	local rc=0
	"$@" || rc=$?
	[ "$rc" -eq 0 ] || { say "$name: failed (rc=$rc)"; return "$rc"; }
	local dt=$((SECONDS - t0))
	say "$name: done (${dt}s)"
	if [ -d "$LOG" ]; then echo "$name ${dt}s" >> "$LOG/times.txt"; fi
}

# C: の空き(WSL の仮想ディスクは C: にある)を確かめ、5GB を切ったら止める
check_space() {
	local avail
	if [ -d /mnt/c ]; then
		avail=$(df -Pk /mnt/c | awk 'NR==2{print $4}')
		[ "$avail" -ge $((5 * 1024 * 1024)) ] || die "C: free space below 5GB ($((avail / 1024))MB)"
	fi
	avail=$(df -Pk "$HOME" | awk 'NR==2{print $4}')
	[ "$avail" -ge $((10 * 1024 * 1024)) ] || die "WSL free space below 10GB"
}

fetch() {
	# $1 = URL, $2 = 保存名(省略時は URL の末尾)
	local url="$1" name="${2:-${1##*/}}"
	if [ ! -s "$CACHE/$name" ]; then
		say "download $url"
		curl -fsSL --retry 3 -o "$CACHE/$name.part" "$url"
		mv "$CACHE/$name.part" "$CACHE/$name"
	fi
}

gpg_verify() {
	# $1 = tarball, $2 = 署名, $3 = 鍵束
	gpgv --keyring "$3" "$CACHE/$2" "$CACHE/$1" 2>&1 | sed 's/^/   /'
	gpgv --keyring "$3" "$CACHE/$2" "$CACHE/$1" >/dev/null 2>&1 || die "signature check failed: $1"
}

# ---------------------------------------------------------------- fetch
step_fetch() {
	mkdir -p "$CACHE"
	fetch "$GNU/gnu-keyring.gpg"
	local f
	for f in "binutils/binutils-$BINUTILS_VER.tar.xz" "gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" \
	         "gdb/gdb-$GDB_VER.tar.xz" "ncurses/ncurses-$NCURSES_VER.tar.gz"; do
		fetch "$GNU/$f"
		fetch "$GNU/$f.sig"
	done
	fetch "$NEWLIB_URL/newlib-$NEWLIB_VER.tar.gz"
	fetch "$NEWLIB_URL/sha512.sum" "newlib-sha512.sum"
	fetch "$MINGW_URL/mingw-w64-v$MINGW_VER.tar.bz2"
	fetch "$MINGW_URL/mingw-w64-v$MINGW_VER.tar.bz2.sig"

	say "verify GNU signatures"
	gpgv --version >/dev/null
	for f in "binutils-$BINUTILS_VER.tar.xz" "gcc-$GCC_VER.tar.xz" "gdb-$GDB_VER.tar.xz" "ncurses-$NCURSES_VER.tar.gz"; do
		gpg_verify "$f" "$f.sig" "$CACHE/gnu-keyring.gpg"
	done

	say "verify newlib sha512"
	(cd "$CACHE" && grep " newlib-$NEWLIB_VER.tar.gz\$" newlib-sha512.sum | sha512sum -c -) || die "newlib sha512 mismatch"

	say "verify mingw-w64 signature"
	# リリースに署名している鍵(Jonathan Yong)を鍵サーバから取り、指紋を固定して照合する
	local mingw_fpr=CAF5641F74F7DFBA88AE205693BDB53CD4EBC740
	if [ ! -s "$CACHE/mingw-w64-signer.gpg" ]; then
		curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&options=mr&search=0x$mingw_fpr" -o "$CACHE/mingw-w64-signer.asc"
		local kh; kh=$(mktemp -d)
		gpg --homedir "$kh" --batch --quiet --import "$CACHE/mingw-w64-signer.asc"
		gpg --homedir "$kh" --batch --with-colons --fingerprint | grep -q "^fpr:::::::::$mingw_fpr:" || { rm -rf "$kh"; die "mingw-w64 signer key fingerprint mismatch"; }
		gpg --homedir "$kh" --batch --export "$mingw_fpr" > "$CACHE/mingw-w64-signer.gpg"
		rm -rf "$kh"
	fi
	gpg_verify "mingw-w64-v$MINGW_VER.tar.bz2" "mingw-w64-v$MINGW_VER.tar.bz2.sig" "$CACHE/mingw-w64-signer.gpg"

	# GCC の前提ライブラリは、署名を確かめた GCC のソースにある sha512 の表で照合する
	local kd; kd=$(mktemp -d)
	tar -xJf "$CACHE/gcc-$GCC_VER.tar.xz" -C "$kd" "gcc-$GCC_VER/contrib/download_prerequisites" "gcc-$GCC_VER/contrib/prerequisites.sha512"
	local dp="$kd/gcc-$GCC_VER/contrib/download_prerequisites"
	local gmp mpfr mpc isl
	gmp=$(sed -n 's/^gmp=.\(gmp-[0-9.]*\.tar\.[a-z0-9]*\).*/\1/p' "$dp")
	mpfr=$(sed -n 's/^mpfr=.\(mpfr-[0-9.]*\.tar\.[a-z0-9]*\).*/\1/p' "$dp")
	mpc=$(sed -n 's/^mpc=.\(mpc-[0-9.]*\.tar\.[a-z0-9]*\).*/\1/p' "$dp")
	isl=$(sed -n 's/^isl=.\(isl-[0-9.]*\.tar\.[a-z0-9]*\).*/\1/p' "$dp")
	[ -n "$gmp" ] && [ -n "$mpfr" ] && [ -n "$mpc" ] && [ -n "$isl" ] || { rm -rf "$kd"; die "cannot parse download_prerequisites"; }
	for f in "$gmp" "$mpfr" "$mpc" "$isl"; do fetch "$GCC_INFRA/$f"; done
	(cd "$CACHE" && grep -E " ($gmp|$mpfr|$mpc|$isl)\$" "$kd/gcc-$GCC_VER/contrib/prerequisites.sha512" | sha512sum -c -) || { rm -rf "$kd"; die "prerequisite sha512 mismatch"; }
	printf '%s\n' "$gmp" "$mpfr" "$mpc" "$isl" > "$CACHE/prerequisites.list"
	rm -rf "$kd"
}

# tarball を新しい空のディレクトリに展開する。$1 = tarball, $2 = 展開先の名前
unpack() {
	local tb="$1" name="$2"
	rm -rf "${SRC:?}/$name"
	mkdir -p "$SRC/$name"
	tar -xf "$CACHE/$tb" -C "$SRC/$name" --strip-components=1 --no-same-owner
}

fresh_build() { rm -rf "${BLD:?}/$1"; mkdir -p "$BLD/$1"; }

# ---------------------------------------------------------------- hostlibs
# gmp、mpfr、mpc、isl、ncurses を静的ライブラリとして作業域に作る(GCC と GDB が使う)
build_hostlib() {
	local name="$1" tb="$2"; shift 2
	unpack "$tb" "$name"
	fresh_build "$name"
	(cd "$BLD/$name" && "$SRC/$name/configure" --prefix="$HOST" --disable-shared --enable-static "$@" > "$LOG/$name-configure.log" 2>&1 \
		&& make -j"$JOBS" > "$LOG/$name-make.log" 2>&1 && make install > "$LOG/$name-install.log" 2>&1) \
		|| die "$name build failed (see $LOG)"
}

step_hostlibs() {
	mkdir -p "$LOG" "$HOST"
	local gmp mpfr mpc isl
	{ read -r gmp; read -r mpfr; read -r mpc; read -r isl; } < "$CACHE/prerequisites.list"
	build_hostlib gmp  "$gmp"  --enable-cxx
	build_hostlib mpfr "$mpfr" --with-gmp="$HOST"
	build_hostlib mpc  "$mpc"  --with-gmp="$HOST" --with-mpfr="$HOST"
	build_hostlib isl  "$isl"  --with-gmp-prefix="$HOST"
	build_hostlib ncurses "ncurses-$NCURSES_VER.tar.gz" --enable-widec --without-cxx --without-cxx-binding \
		--without-ada --without-progs --without-tests --without-manpages --disable-db-install \
		--with-default-terminfo-dir=/usr/share/terminfo \
		--with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo
}

# ---------------------------------------------------------------- elf
patch_gcc_multilib() {
	# x86_64-elf に -mno-red-zone の libgcc、newlib、libstdc++ の組を足す(既定は利用者のプログラム用)。
	# 対象のライブラリはすべて -fPIC で作るので、-mcmodel=kernel(最上位の 2GB)や
	# -mcmodel=large のカーネルにも、位置を問わずに結べる。
	local g="$SRC/gcc"
	cat > "$g/gcc/config/i386/t-x86_64-elf" <<'EOF2'
MULTILIB_OPTIONS  = mno-red-zone
MULTILIB_DIRNAMES = no-red-zone
EOF2
	# x86_64-*-fuchsia* も同じ名前を使うので、x86_64-*-elf* の節の直後の行で確かめる
	if ! grep -A1 '^x86_64-\*-elf\*)$' "$g/gcc/config.gcc" | grep -q 'i386/t-x86_64-elf'; then
		sed -i '/^x86_64-\*-elf\*)$/a\	tmake_file="${tmake_file} i386/t-x86_64-elf"' "$g/gcc/config.gcc"
	fi
	grep -A1 '^x86_64-\*-elf\*)$' "$g/gcc/config.gcc" | grep -q 'i386/t-x86_64-elf' || die "failed to patch config.gcc"
}

step_elf() {
	mkdir -p "$LOG"
	: > "$LOG/times.txt"
	check_space
	rm -rf "$ELF_DIR"
	mkdir -p "$ELF_DIR"
	export PATH="$ELF_DIR/bin:$PATH"

	unpack "binutils-$BINUTILS_VER.tar.xz" binutils
	fresh_build elf-binutils
	timed elf-binutils bash -c "cd '$BLD/elf-binutils' && '$SRC/binutils/configure' \
		--prefix='$ELF_DIR' --target=$ELF_TARGET --with-sysroot='$ELF_DIR/$ELF_TARGET' \
		--with-pkgversion='$PKGVERSION' --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
		--disable-sim --disable-libdecnumber --disable-readline --with-system-zlib \
		--enable-deterministic-archives --enable-targets=x86_64-elf,i386-elf,x86_64-pep \
		> '$LOG/elf-binutils-configure.log' 2>&1 \
		&& make -j$JOBS MAKEINFO=true > '$LOG/elf-binutils-make.log' 2>&1 \
		&& make install MAKEINFO=true > '$LOG/elf-binutils-install.log' 2>&1" || die "elf-binutils failed"
	check_space

	unpack "gcc-$GCC_VER.tar.xz" gcc
	patch_gcc_multilib
	unpack "newlib-$NEWLIB_VER.tar.gz" newlib

	local gcc_conf=(
		--prefix="$ELF_DIR" --target=$ELF_TARGET
		--with-sysroot="$ELF_DIR/$ELF_TARGET" --with-native-system-header-dir=/include
		--with-pkgversion="$PKGVERSION"
		--enable-languages=c,c++ --enable-multilib
		--with-newlib --with-headers=yes --with-gnu-as --with-gnu-ld
		--disable-nls --disable-shared --disable-threads --disable-tls
		--enable-checking=release --with-system-zlib
		--disable-libatomic --disable-libgomp --disable-libitm --disable-libquadmath
		--disable-libsanitizer --disable-libvtv --disable-libssp
		--with-gmp="$HOST" --with-mpfr="$HOST" --with-mpc="$HOST" --with-isl="$HOST"
	)
	# 1 段目: newlib のない状態でコンパイラ本体と libgcc を作る
	mkdir -p "$ELF_DIR/$ELF_TARGET/include"
	cp -a "$SRC/newlib/newlib/libc/include/." "$ELF_DIR/$ELF_TARGET/include/"
	fresh_build elf-gcc
	timed elf-gcc1 bash -c "cd '$BLD/elf-gcc' && \
		CFLAGS_FOR_TARGET='$TFLAGS' CXXFLAGS_FOR_TARGET='$TFLAGS' \
		'$SRC/gcc/configure' $(printf "'%s' " "${gcc_conf[@]}") \
		> '$LOG/elf-gcc-configure.log' 2>&1 \
		&& make -j$JOBS all-gcc all-target-libgcc MAKEINFO=true > '$LOG/elf-gcc1-make.log' 2>&1 \
		&& make install-gcc install-target-libgcc MAKEINFO=true > '$LOG/elf-gcc1-install.log' 2>&1" || die "elf-gcc stage 1 failed"
	[ "$("$ELF_DIR/bin/$ELF_TARGET-gcc" -print-multi-lib | wc -l)" -eq 2 ] || die "multilib set is not as expected"
	check_space

	# newlib: aarch64-none-elf の xPack と同じ機能の組(newlib.h の定義が一致するもの)
	rm -rf "$ELF_DIR/$ELF_TARGET/include"
	fresh_build elf-newlib
	timed elf-newlib bash -c "cd '$BLD/elf-newlib' && \
		CFLAGS_FOR_TARGET='$TFLAGS' \
		'$SRC/newlib/configure' --prefix='$ELF_DIR' --target=$ELF_TARGET \
		--enable-newlib-io-long-long --enable-newlib-io-c99-formats --enable-newlib-register-fini \
		--enable-newlib-retargetable-locking --enable-newlib-mb --disable-newlib-supplied-syscalls --disable-nls \
		> '$LOG/elf-newlib-configure.log' 2>&1 \
		&& make -j$JOBS MAKEINFO=true > '$LOG/elf-newlib-make.log' 2>&1 \
		&& make install MAKEINFO=true > '$LOG/elf-newlib-install.log' 2>&1" || die "elf-newlib failed"
	check_space

	# 2 段目: newlib を使って libstdc++ まで作る
	timed elf-gcc2 bash -c "cd '$BLD/elf-gcc' && \
		make -j$JOBS MAKEINFO=true > '$LOG/elf-gcc2-make.log' 2>&1 \
		&& make install MAKEINFO=true > '$LOG/elf-gcc2-install.log' 2>&1" || die "elf-gcc stage 2 failed"

	strip_host "$ELF_DIR"
	ln -sfn "$ELF_DIR" "$PREFIX/$ELF_TARGET-gcc"
	write_info "$ELF_DIR" "$ELF_TARGET" "${gcc_conf[*]}"
	echo "target library flags: $TFLAGS" >> "$ELF_DIR/BUILD-INFO.txt"
}

# ---------------------------------------------------------------- gdb
step_gdb() {
	mkdir -p "$LOG"
	check_space
	unpack "gdb-$GDB_VER.tar.xz" gdb
	fresh_build elf-gdb
	local py=--without-python
	command -v python3-config >/dev/null 2>&1 && python3-config --ldflags --embed >/dev/null 2>&1 && py=--with-python=/usr/bin/python3
	timed elf-gdb bash -c "cd '$BLD/elf-gdb' && \
		CFLAGS='-g -O2 -I$HOST/include -I$HOST/include/ncursesw' CXXFLAGS='-g -O2 -I$HOST/include -I$HOST/include/ncursesw' LDFLAGS='-L$HOST/lib' \
		'$SRC/gdb/configure' --prefix='$ELF_DIR' --target=$ELF_TARGET --with-pkgversion='$PKGVERSION' \
		--disable-nls --disable-werror --disable-sim --disable-binutils --disable-ld --disable-gas \
		--disable-gprof --disable-gprofng --with-system-zlib --with-expat \
		--with-gmp='$HOST' --with-mpfr='$HOST' --enable-tui $py \
		> '$LOG/elf-gdb-configure.log' 2>&1 \
		&& make -j$JOBS MAKEINFO=true all-gdb > '$LOG/elf-gdb-make.log' 2>&1 \
		&& make MAKEINFO=true install-gdb > '$LOG/elf-gdb-install.log' 2>&1" || die "elf-gdb failed"
	strip_host "$ELF_DIR"
	echo "gdb: $GDB_VER ($py)" >> "$ELF_DIR/BUILD-INFO.txt"
}

# ---------------------------------------------------------------- mingw
step_mingw() {
	mkdir -p "$LOG"
	: > "$LOG/times.txt"
	check_space
	rm -rf "$MGW_DIR"
	mkdir -p "$MGW_DIR"
	export PATH="$MGW_DIR/bin:$PATH"

	[ -d "$SRC/binutils" ] || unpack "binutils-$BINUTILS_VER.tar.xz" binutils
	fresh_build mgw-binutils
	timed mgw-binutils bash -c "cd '$BLD/mgw-binutils' && '$SRC/binutils/configure' \
		--prefix='$MGW_DIR' --target=$MGW_TARGET --with-sysroot='$MGW_DIR' \
		--with-pkgversion='$PKGVERSION' --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
		--disable-sim --disable-libdecnumber --disable-readline --disable-multilib --with-system-zlib \
		--enable-deterministic-archives \
		> '$LOG/mgw-binutils-configure.log' 2>&1 \
		&& make -j$JOBS MAKEINFO=true > '$LOG/mgw-binutils-make.log' 2>&1 \
		&& make install MAKEINFO=true > '$LOG/mgw-binutils-install.log' 2>&1" || die "mgw-binutils failed"

	unpack "mingw-w64-v$MINGW_VER.tar.bz2" mingw-w64
	fresh_build mgw-headers
	timed mgw-headers bash -c "cd '$BLD/mgw-headers' && '$SRC/mingw-w64/mingw-w64-headers/configure' \
		--prefix='$MGW_DIR/$MGW_TARGET' --host=$MGW_TARGET > '$LOG/mgw-headers.log' 2>&1 \
		&& make install >> '$LOG/mgw-headers.log' 2>&1" || die "mgw-headers failed"
	ln -sfn "$MGW_TARGET" "$MGW_DIR/mingw"

	[ -d "$SRC/gcc" ] || { unpack "gcc-$GCC_VER.tar.xz" gcc; }
	local gcc_conf=(
		--prefix="$MGW_DIR" --target=$MGW_TARGET --with-sysroot="$MGW_DIR"
		--with-pkgversion="$PKGVERSION"
		--enable-languages=c --disable-multilib --disable-nls --disable-shared
		--disable-threads --disable-libgomp --disable-libssp --disable-libquadmath
		--disable-libatomic --disable-libvtv --disable-win32-registry
		--enable-checking=release --with-system-zlib
		--with-gmp="$HOST" --with-mpfr="$HOST" --with-mpc="$HOST" --with-isl="$HOST"
	)
	fresh_build mgw-gcc
	timed mgw-gcc1 bash -c "cd '$BLD/mgw-gcc' && '$SRC/gcc/configure' $(printf "'%s' " "${gcc_conf[@]}") \
		> '$LOG/mgw-gcc-configure.log' 2>&1 \
		&& make -j$JOBS all-gcc MAKEINFO=true > '$LOG/mgw-gcc1-make.log' 2>&1 \
		&& make install-gcc MAKEINFO=true > '$LOG/mgw-gcc1-install.log' 2>&1" || die "mgw-gcc stage 1 failed"

	fresh_build mgw-crt
	timed mgw-crt bash -c "cd '$BLD/mgw-crt' && '$SRC/mingw-w64/mingw-w64-crt/configure' \
		--prefix='$MGW_DIR/$MGW_TARGET' --host=$MGW_TARGET --with-sysroot='$MGW_DIR' \
		--enable-lib64 --disable-lib32 > '$LOG/mgw-crt-configure.log' 2>&1 \
		&& make -j$JOBS > '$LOG/mgw-crt-make.log' 2>&1 \
		&& make install > '$LOG/mgw-crt-install.log' 2>&1" || die "mgw-crt failed"

	timed mgw-gcc2 bash -c "cd '$BLD/mgw-gcc' && make -j$JOBS MAKEINFO=true > '$LOG/mgw-gcc2-make.log' 2>&1 \
		&& make install MAKEINFO=true > '$LOG/mgw-gcc2-install.log' 2>&1" || die "mgw-gcc stage 2 failed"

	strip_host "$MGW_DIR"
	ln -sfn "$MGW_DIR" "$PREFIX/$MGW_TARGET-gcc"
	write_info "$MGW_DIR" "$MGW_TARGET" "${gcc_conf[*]}"
	echo "mingw-w64: $MINGW_VER" >> "$MGW_DIR/BUILD-INFO.txt"
}

# ホストで動く実行ファイルだけからデバッグ情報を除く(対象の .a や .o には触れない)
strip_host() {
	local dir="$1" f
	while IFS= read -r -d '' f; do
		case "$(file -b "$f")" in
			ELF\ 64-bit\ LSB\ *x86-64*) strip --strip-unneeded "$f" 2>/dev/null || true ;;
		esac
	done < <(find "$dir/bin" "$dir/libexec" "$dir/$ELF_TARGET/bin" "$dir/$MGW_TARGET/bin" -type f -perm -u+x -print0 2>/dev/null)
}

write_info() {
	local dir="$1" tgt="$2" conf="$3"
	{
		echo "TessronOS $tgt toolchain, built $(date '+%Y-%m-%d %H:%M') on $(uname -sr)"
		echo "host gcc: $(gcc --version | head -1)"
		echo "binutils: $BINUTILS_VER"
		echo "gcc: $GCC_VER"
		[ "$tgt" = "$ELF_TARGET" ] && echo "newlib: $NEWLIB_VER"
		echo "gcc configure: $conf"
	} > "$dir/BUILD-INFO.txt"
	[ -f "$LOG/times.txt" ] && cp "$LOG/times.txt" "$dir/BUILD-TIMES.txt"
	return 0
}

# ---------------------------------------------------------------- nasm
# 取得と照合(固定した SHA-256 と git.id)、新しい空のディレクトリへの展開、構築
step_nasm() {
	check_space
	mkdir -p "$CACHE" "$LOG"
	fetch "$NASM_URL/$NASM_VER/nasm-$NASM_VER.tar.xz"
	fetch "$NASM_URL/$NASM_VER/git.id" "nasm-$NASM_VER.git.id"
	say "verify nasm sha256 and git id"
	echo "$NASM_SHA256  $CACHE/nasm-$NASM_VER.tar.xz" | sha256sum -c - || die "nasm sha256 mismatch"
	[ "$(tr -d ' \n' < "$CACHE/nasm-$NASM_VER.git.id")" = "$NASM_GITID" ] || die "nasm git id mismatch"
	unpack "nasm-$NASM_VER.tar.xz" nasm
	fresh_build nasm
	(cd "$BLD/nasm" && "$SRC/nasm/configure" --prefix="$NASM_DIR" > "$LOG/nasm-configure.log" 2>&1 \
		&& make -j"$JOBS" > "$LOG/nasm-make.log" 2>&1 && make install > "$LOG/nasm-install.log" 2>&1) \
		|| die "nasm build failed (see $LOG)"
	strip --strip-unneeded "$NASM_DIR/bin/nasm" "$NASM_DIR/bin/ndisasm" 2>/dev/null || true
	{
		echo "TessronOS nasm, built $(date '+%Y-%m-%d %H:%M') on $(uname -sr)"
		echo "host gcc: $(gcc --version | head -1)"
		echo "nasm: $NASM_VER (git $NASM_GITID, sha256 $NASM_SHA256)"
	} > "$NASM_DIR/BUILD-INFO.txt"
	ln -sfn "$NASM_DIR" "$PREFIX/nasm"
	"$PREFIX/nasm/bin/nasm" -v | sed 's/^/   /'
	rm -rf "${SRC:?}/nasm" "${BLD:?}/nasm"
}

# ---------------------------------------------------------------- test
step_test() {
	local t; t=$(mktemp -d)
	# shellcheck disable=SC2064
	trap "rm -rf '$t'" EXIT
	local E="$PREFIX/$ELF_TARGET-gcc/bin/$ELF_TARGET-" M="$PREFIX/$MGW_TARGET-gcc/bin/$MGW_TARGET-"
	say "multilib:"; "${E}gcc" -print-multi-lib | sed 's/^/   /'
	for o in "" "-mno-red-zone" "-mno-red-zone -mcmodel=kernel"; do
		# shellcheck disable=SC2086
		printf '   %-32s %s\n' "[$o]" "$("${E}gcc" $o -print-libgcc-file-name)"
	done

	# -mcmodel=kernel で最上位の 2GB に置いたカーネルに、no-red-zone の libgcc と newlib の関数を結ぶ
	cat > "$t/k.c" <<'EOF'
#include <string.h>
static char buf[64];
unsigned long long kdiv(unsigned __int128 a, unsigned long long b) { return (unsigned long long)(a / b); }
void _start(void)
{
	volatile unsigned long long r = kdiv(12345678901234567890ULL, 7) + __builtin_popcountll(255);
	memcpy(buf, "tessron", 8);
	r += strlen(buf) + (unsigned long long)memchr(buf, 's', sizeof buf);
	(void)r;
	for (;;) __asm__ volatile("hlt");
}
EOF
	cat > "$t/k.ld" <<'EOF'
ENTRY(_start)
SECTIONS { . = 0xffffffff80100000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .got : { *(.got*) } .bss : { *(.bss*) *(COMMON) } }
EOF
	local kopt=(-ffreestanding -O2 -mno-red-zone -mgeneral-regs-only -nostdlib -static -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
	"${E}gcc" "${kopt[@]}" -mcmodel=kernel -T "$t/k.ld" -o "$t/k.elf" "$t/k.c" -lc -lgcc || die "kernel model link failed"
	say "kernel model link: entry $("${E}readelf" -h "$t/k.elf" | awk '/Entry/{print $4}'), libc $("${E}gcc" -mno-red-zone -print-file-name=libc.a)"
	sed -i 's/0xffffffff80100000/0xffff800000100000/' "$t/k.ld"
	"${E}gcc" "${kopt[@]}" -mcmodel=large -T "$t/k.ld" -o "$t/kl.elf" "$t/k.c" -lc -lgcc || die "large model link failed"
	say "large model link: entry $("${E}readelf" -h "$t/kl.elf" | awk '/Entry/{print $4}')"

	cat > "$t/u.cc" <<'EOF'
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
int main() {
	std::vector<std::string> v{"x86_64", "newlib"};
	try { throw std::runtime_error("ok"); } catch (const std::exception &e) { v.push_back(e.what()); }
	std::printf("%s %s %s %lld %.3f\n", v[0].c_str(), v[1].c_str(), v[2].c_str(), 1LL << 40, 3.14159);
	return 0;
}
EOF
	# newlib の x86_64-elf には crt0 が無い(TessronOS が自前の crt を持つ)ので、試験用の入口を足す
	cat > "$t/start.c" <<'EOF'
#include <stdlib.h>
extern int main(void);
void _init(void) {}
void _fini(void) {}
void _start(void) { exit(main()); }
EOF
	"${E}g++" -O2 -c -o "$t/u.o" "$t/u.cc" || die "C++ compile failed"
	"${E}gcc" -O2 -c -o "$t/start.o" "$t/start.c" || die "C compile failed"
	"${E}g++" -O2 -nostartfiles -static -o "$t/u.elf" "$t/start.o" "$t/u.o" --specs=nosys.specs 		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Ttext=0x400000 || die "user link failed"
	say "user link (C++ exceptions, libstdc++, newlib printf, nosys.specs): $("${E}size" "$t/u.elf" | tail -1)"

	if [ -x "${M}gcc" ]; then
		cat > "$t/hello.c" <<'EOF'
typedef unsigned long long UINTN;
typedef unsigned short CHAR16;
typedef UINTN EFI_STATUS;
typedef struct OUT_PROTO {
	void *Reset;
	EFI_STATUS (*OutputString)(struct OUT_PROTO *self, const CHAR16 *s);
} OUT_PROTO;
typedef struct {
	char Hdr[24];
	void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
	void *GetVariable, *GetNextVariableName, *SetVariable, *GetNextHighMonotonicCount;
	void (*ResetSystem)(int type, EFI_STATUS status, UINTN size, void *data);
} RUNTIME;
typedef struct {
	char Hdr[24];
	CHAR16 *FirmwareVendor;
	unsigned int FirmwareRevision;
	void *ConsoleInHandle, *ConIn, *ConsoleOutHandle;
	OUT_PROTO *ConOut;
	void *StandardErrorHandle, *StdErr;
	RUNTIME *RuntimeServices;
} SYSTEM_TABLE;
EFI_STATUS efi_main(void *image, SYSTEM_TABLE *st)
{
	(void)image;
	st->ConOut->OutputString(st->ConOut, u"Hello from TessronOS x64 EFI\r\n");
	st->ConOut->OutputString(st->ConOut, st->FirmwareVendor);
	st->ConOut->OutputString(st->ConOut, u"\r\n");
	st->RuntimeServices->ResetSystem(2, 0, 0, 0);
	return 0;
}
EOF
		"${M}gcc" -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone -O2 \
			-nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -o "$t/BOOTX64.EFI" "$t/hello.c" || die "EFI link failed"
		say "EFI app: $("${M}objdump" -p "$t/BOOTX64.EFI" | grep -E 'Magic|Subsystem' | tr -s ' \t' ' ' | paste -sd ';')"
		mkdir -p "$WORK/efi-test/EFI/BOOT"
		cp "$t/BOOTX64.EFI" "$WORK/efi-test/EFI/BOOT/BOOTX64.EFI"
		say "EFI test tree: $WORK/efi-test"
	fi
	rm -rf "$t"
	trap - EXIT
}

# ---------------------------------------------------------------- clean
step_clean() {
	rm -rf "${WORK:?}"
	say "removed $WORK"
}

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(fetch hostlibs elf gdb mingw test clean)
mkdir -p "$WORK" "$LOG"
for s in "${steps[@]}"; do
	case "$s" in
		fetch|hostlibs|elf|gdb|mingw|nasm|test|clean) timed "$s" "step_$s" ;;
		*) die "unknown step: $s" ;;
	esac
done
