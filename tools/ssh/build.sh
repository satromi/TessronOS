#!/bin/bash
#
# tools/ssh/build.sh — the SSH client's library (docs/tessronos/11-desktop.md): libssh2 on the
# repository's Mbed TLS (lib/mbedtls, as lib/libtls configures it), built as a program is built.
#
#   ARCH=x86_64 (the default)	~/tessronos-ssh-x64/libssh2.a
#   ARCH=aarch64		~/tessronos-ssh-arm/libssh2.a
#   ARCH=host			~/tessronos-ssh-host/libssh2.a   (the tests on the host)
#
#   [ARCH=...] [UFLAGS=...] tools/ssh/build.sh [fetch|compile|all]
#
# libssh2 is fetched from its release, its SHA-256 checked, and unpacked into ~/libssh2. Its
# sources are not changed and are not kept in the repository. The configuration it would take
# from configure is tools/ssh/libssh2_config.h: no sockets of the system, for every byte goes
# through the send and receive callbacks the caller sets (lib/libsftp/sftp.c).
#
set -e
STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
ARCH=${ARCH:-x86_64}
VER=1.11.1
SHA=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
SRC=$HOME/libssh2/libssh2-$VER
case $ARCH in
x86_64)
	XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	CC=$XB/x86_64-elf-gcc; AR=$XB/x86_64-elf-ar
	OUT=${OUT:-$HOME/tessronos-ssh-x64}
	UFLAGS=${UFLAGS:--march=x86-64-v2}
	;;
aarch64)
	XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	CC=$XB/aarch64-none-elf-gcc; AR=$XB/aarch64-none-elf-ar
	OUT=${OUT:-$HOME/tessronos-ssh-arm}
	UFLAGS=${UFLAGS:--march=${MARCH:-armv8.2-a} -mstrict-align}
	;;
host)
	CC=gcc; AR=ar
	OUT=${OUT:-$HOME/tessronos-ssh-host}
	UFLAGS=${UFLAGS:--g -fsanitize=address,undefined}
	;;
*)
	echo "ssh: ARCH is x86_64, aarch64 or host, not $ARCH" >&2
	exit 2
	;;
esac

fetch() {
	[ -f "$SRC/src/sftp.c" ] && return 0
	mkdir -p "$HOME/libssh2"
	curl -fsSL -o "$HOME/libssh2/l.tgz" "https://libssh2.org/download/libssh2-$VER.tar.gz"
	echo "$SHA  $HOME/libssh2/l.tgz" | sha256sum -c -
	tar xzf "$HOME/libssh2/l.tgz" -C "$HOME/libssh2"
	rm -f "$HOME/libssh2/l.tgz"
}

compile() {
	mkdir -p "$OUT/o"
	local cfg=$TOP/lib/libtls/ts_tls_config.h
	local fl="$UFLAGS -O2 -fno-stack-protector -ffunction-sections -fdata-sections -include $TOP/tools/ssh/ts_ssh_shim.h -DHAVE_CONFIG_H -DLIBSSH2_MBEDTLS -I$TOP/tools/ssh -I$TOP/tools/ssh/include -I$SRC/include -I$SRC/src -I$TOP/lib/libtls -I$TOP/lib/mbedtls/include -DMBEDTLS_CONFIG_FILE=\"ts_tls_config.h\" -DCHK_TKERNEL_CONST"
	[ "$ARCH" = host ] && fl="${fl/-I$TOP\/tools\/ssh\/include /} -DTS_SSH_HOST"
	local objs=
	for f in agent bcrypt_pbkdf channel chacha cipher-chachapoly comp crypt crypto global hostkey keepalive kex knownhost mac misc packet pem poly1305 publickey scp session sftp transport userauth userauth_kbd_packet version; do
		[ -f "$SRC/src/$f.c" ] || continue
		if [ ! -f "$OUT/o/$f.o" ] || [ "$SRC/src/$f.c" -nt "$OUT/o/$f.o" ] || [ "$TOP/tools/ssh/libssh2_config.h" -nt "$OUT/o/$f.o" ] || [ "$TOP/tools/ssh/ts_ssh_shim.h" -nt "$OUT/o/$f.o" ]; then
			echo "CC $f"
			$CC $fl -c -o "$OUT/o/$f.o" "$SRC/src/$f.c"
		fi
		objs="$objs $OUT/o/$f.o"
	done
	rm -f "$OUT/libssh2.a"
	$AR rcs "$OUT/libssh2.a" $objs
	cp "$SRC/COPYING" "$OUT/COPYING-libssh2.txt"
	echo "built $OUT/libssh2.a"
}

case $STAGE in
fetch) fetch ;;
compile) compile ;;
all) fetch; compile ;;
*) echo "ssh: fetch, compile or all" >&2; exit 2 ;;
esac
