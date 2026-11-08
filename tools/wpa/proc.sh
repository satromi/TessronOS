#!/bin/bash
#
# tools/wpa/proc.sh — wpa_supplicant as a process of its own (docs/tessronos/09-network.md
# 9.6.11, WPA_MODE=proc): the same handshakes and SAE as the kernel's (tools/wpa/build.sh), with
# their cryptography from OpenSSL's libcrypto through wpa_supplicant's own crypto_openssl.c,
# built as a program is built, for the process "wpasupp" (application/wpasupp/):
#
#   ARCH=x86_64 (the default)	~/tessronos-wpaproc-x64/wpasupp_lib.o
#   ARCH=aarch64			~/tessronos-wpaproc-arm/wpasupp_lib.o
#
#   [ARCH=aarch64] [UFLAGS=...] tools/wpa/proc.sh [fetch|openssl|compile|all|test|modtest|clean]
#
# OpenSSL (3.5, the release of long term support) is fetched from its release on GitHub, its
# SHA-256 checked, and unpacked into ~/openssl; it is configured outside its tree, without
# assembly, threads, sockets, files of configuration, engines, loadable modules or the
# legacy algorithms, its seed from getentropy() (the process gives it from the system's random
# source). Its sources are not changed. The two files of libcrypto that need the links of
# files and a system log are left out (neither is reached); its sleeping is the busy loop
# OpenSSL has for a system without one, and its reading of directories is built against the
# declarations of application/wpasupp/include/dirent.h (the process finds none).
#
# wpasupp_lib.o is the glue (device/net/wifi/wpa/wpa_glue.c), wpa_supplicant's sources, its
# crypto_openssl.c, the process's answering of the kernel's messages (wps_serve.c) and the
# parts of libcrypto they reach (--gc-sections from wps_serve), in one object of which
# wps_serve alone stays global (the glue's own strtol, strdup and abort do not meet newlib's). The program's main
# (application/wpasupp/wps_main.c) is linked with it by build_make/makefile. UFLAGS are the
# flags of a program that uses floating point (ARCHFLAGS and UARCH_FP of build_make/common.mk).
#
# "test" and "modtest" run tools/wpa/build.sh's tests on the host with OpenSSL: test_wpa.c's
# handshakes with every call of knl_wpa_* made into a message by the kernel's side
# (device/net/wifi/wpaproc/wpaproc_call.c), answered by the process's (wps_serve.c) and its
# callbacks carried back (tools/wpa/proc_loop.c); the upstream module tests with the same
# sources and crypto_openssl.c.
#
set -e

STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
ARCH=${ARCH:-x86_64}
case $ARCH in
x86_64)
	XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	TP=$XB/x86_64-elf-
	OUT=${OUT:-$HOME/tessronos-wpaproc-x64}
	UFLAGS=${UFLAGS:--march=x86-64-v2}
	;;
aarch64)
	XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	TP=$XB/aarch64-none-elf-
	OUT=${OUT:-$HOME/tessronos-wpaproc-arm}
	UFLAGS=${UFLAGS:--march=${MARCH:-armv8.2-a} -mstrict-align}
	;;
*)
	echo "wpaproc: ARCH is x86_64 or aarch64, not $ARCH" >&2
	exit 2
	;;
esac
CC=${TP}gcc
LD=${TP}ld
AR=${TP}ar
OBJCOPY=${TP}objcopy
NM=${TP}nm
JOBS=${JOBS:-12}

WSRC=${WPASRC:-$HOME/netbsd/wpa-11.0}
D=$WSRC/dist/src
GLUE=$TOP/device/net/wifi/wpa
WPP=$TOP/device/net/wifi/wpaproc
APP=$TOP/application/wpasupp
WORK=$OUT/work

OSSL_VER=3.5.9
OSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
OSSL_URL=${OSSL_URL:-https://github.com/openssl/openssl/releases/download/openssl-$OSSL_VER/openssl-$OSSL_VER.tar.gz}
OSSL_DL=${OSSLDL:-$HOME/openssl/dl}
OSRC=${OSSLSRC:-$HOME/openssl/openssl-$OSSL_VER}
OSSL_OPTS="no-asm no-pic no-shared no-module no-dso no-engine no-threads no-sock no-dgram no-http
 no-autoload-config no-async no-ui-console no-tests no-apps no-docs no-ktls no-afalgeng
 no-devcryptoeng no-padlockeng no-capieng no-ocsp no-cms no-ts no-ct no-srp no-quic no-comp
 no-zlib no-pinshared no-atexit no-ssl-trace no-filenames --with-rand-seed=getrandom"
# what needs a C library that has the links of files and a system log (not reached)
OSSL_SKIP="crypto/x509/by_dir.c crypto/bio/bss_log.c"

# The same sources of wpa_supplicant as the kernel's, the cryptography under crypto.h OpenSSL's
SRCS="rsn_supp/wpa.c rsn_supp/wpa_ie.c rsn_supp/pmksa_cache.c
 common/wpa_common.c common/ieee802_11_common.c common/sae.c common/dragonfly.c
 utils/common.c utils/wpabuf.c crypto/dh_groups.c crypto/crypto_openssl.c
 crypto/sha1-prf.c crypto/sha256-prf.c crypto/sha384-prf.c crypto/sha512-prf.c
 crypto/sha256-kdf.c crypto/sha384-kdf.c crypto/sha512-kdf.c"
# 802.1X: the EAPOL supplicant, EAP's peer with PEAP-MSCHAPv2 and EAP-TLS, and TLS by libssl
EAPSRCS="eapol_supp/eapol_supp_sm.c eap_peer/eap.c eap_peer/eap_methods.c eap_peer/eap_tls.c
 eap_peer/eap_tls_common.c eap_peer/eap_peap.c eap_peer/eap_mschapv2.c eap_peer/mschapv2.c
 eap_common/eap_common.c eap_common/eap_peap_common.c eap_common/chap.c
 crypto/ms_funcs.c crypto/tls_openssl.c utils/base64.c"
EAPDEFS="-include $TOP/device/net/wifi/wpa/ts_eap_defs.h -DIEEE8021X_EAPOL -DEAP_TLS -DEAP_PEAP -DEAP_MSCHAPv2 -DEAP_TLS_FUNCS"
DEFS="-DCONFIG_NO_RANDOM_POOL
 -DCONFIG_NO_VLAN -DCONFIG_SHA256 -DCONFIG_SHA384 -DCONFIG_SHA512 -DCONFIG_NO_RADIUS
 -DCONFIG_HMAC_SHA256_KDF -DCONFIG_HMAC_SHA384_KDF -DCONFIG_HMAC_SHA512_KDF
 -DCONFIG_SAE -DCONFIG_ECC -DCONFIG_NO_ACCOUNTING -DCONFIG_NO_RC4
 -D__BYTE_ORDER=1234 -D__LITTLE_ENDIAN=1234 -D__BIG_ENDIAN=4321
 -Dbswap_16=__builtin_bswap16 -Dbswap_32=__builtin_bswap32 -Dbswap_64=__builtin_bswap64"

fetch_openssl() {
	[ -f "$OSRC/Configure" ] && return 0
	local tgz=$OSSL_DL/openssl-$OSSL_VER.tar.gz
	if [ ! -f "$tgz" ]; then
		mkdir -p "$OSSL_DL"
		curl -fsSL -m 1800 -o "$tgz.part" "$OSSL_URL" || { rm -f "$tgz.part"; echo "wpaproc: $OSSL_URL could not be had (set OSSL_URL)" >&2; exit 1; }
		mv "$tgz.part" "$tgz"
	fi
	echo "$OSSL_SHA256  $tgz" | sha256sum -c - >/dev/null || { echo "wpaproc: $tgz is not OpenSSL $OSSL_VER's" >&2; exit 1; }
	mkdir -p "$(dirname "$OSRC")"
	tar xzf "$tgz" -C "$(dirname "$OSRC")"
	echo "wpaproc: OpenSSL $OSSL_VER in $OSRC"
}

fetch() {
	bash "$TOP/tools/wpa/build.sh" fetch >/dev/null
	fetch_openssl
}

# OpenSSL configured in $1 for the compiler $2 and its flags $3 (once: the configuration is kept)
configure_openssl() {
	local b=$1 cc=$2 cflags=$3 target=$4 stamp
	stamp="$OSSL_VER|$target|$cc|$cflags|$(echo $OSSL_OPTS)"
	if [ -f "$b/.tessronos" ] && [ "$(cat "$b/.tessronos")" = "$stamp" ]; then
		return 0
	fi
	rm -rf "$b"
	mkdir -p "$b"
	(cd "$b" && perl "$OSRC/Configure" "$target" --prefix=/nonexistent --openssldir=/nonexistent \
		CC="$cc" CFLAGS="$cflags" CPPFLAGS= LDFLAGS= $OSSL_OPTS > configure.log 2>&1) \
		|| { tail -20 "$b/configure.log"; echo "wpaproc: OpenSSL could not be configured" >&2; exit 1; }
	(cd "$b" && make build_generated > generated.log 2>&1) \
		|| { tail -20 "$b/generated.log"; exit 1; }
	echo "$stamp" > "$b/.tessronos"
}

# libcrypto.a of the configuration in $1: its objects, those of OSSL_SKIP left out
libcrypto() {
	local b=$1 ar=$2 skips=$3 objs
	[ -f "$b/libcrypto-ts.a" ] && [ "$b/libcrypto-ts.a" -nt "$b/.tessronos" ] && return 0
	# the objects the library is made of, as the configuration's makefile lists them
	objs=$(awk '/^libcrypto\.a:/ {f=1} f {print} f && !/\\$/ {exit}' "$b/Makefile" \
		| sed 's/^libcrypto\.a://; s/\\$//' | tr ' ' '\n' | grep '\.o$')
	local skip s o list=""
	for o in $objs; do
		skip=0
		for s in $skips; do
			case $o in "$(dirname "$s")"/*-"$(basename "$s" .c)".o) skip=1 ;; esac
		done
		[ $skip = 1 ] || list="$list $o"
	done
	(cd "$b" && make -j"$JOBS" $list > build.log 2>&1) \
		|| { grep -E "error" "$b/build.log" | head -20; echo "wpaproc: libcrypto could not be built" >&2; exit 1; }
	(cd "$b" && rm -f libcrypto-ts.a && "$ar" rcs libcrypto-ts.a $list)
	echo "wpaproc: libcrypto of $(echo $list | wc -w) objects in $b"
}

# libssl.a of the configuration in $1
libssl() {
	local b=$1 ar=$2 objs
	[ -f "$b/libssl-ts.a" ] && [ "$b/libssl-ts.a" -nt "$b/.tessronos" ] && return 0
	objs=$(awk '/^libssl\.a:/ {f=1} f {print} f && !/\\$/ {exit}' "$b/Makefile" \
		| sed 's/^libssl\.a://; s/\\$//' | tr ' ' '\n' | grep '\.o$')
	(cd "$b" && make -j"$JOBS" $objs > build-ssl.log 2>&1) \
		|| { grep -E "error" "$b/build-ssl.log" | head -20; echo "wpaproc: libssl could not be built" >&2; exit 1; }
	(cd "$b" && rm -f libssl-ts.a && "$ar" rcs libssl-ts.a $objs)
	echo "wpaproc: libssl of $(echo $objs | wc -w) objects in $b"
}

openssl() {
	configure_openssl "$WORK/openssl" "$CC" "$UFLAGS -O2 -g -fno-stack-protector -ffunction-sections -fdata-sections \
		-DOPENSSL_USE_SLEEP_BUSYLOOP -I$APP/include" linux-generic64
	libcrypto "$WORK/openssl" "$AR" "$OSSL_SKIP"
	libssl "$WORK/openssl" "$AR"
}

compile() {
	local O=$WORK/obj s o objs=""
	local OB=$WORK/openssl
	mkdir -p "$O"
	local CFLAGS="$UFLAGS -O2 -g -fno-stack-protector -ffunction-sections -fdata-sections -std=gnu11 -w $DEFS $EAPDEFS
		-I$GLUE -I$WPP -I$D -I$D/utils -I$D/crypto -I$OB/include -I$OSRC/include
		-idirafter $GLUE/include"
	for s in $SRCS $EAPSRCS; do echo "$D/$s"; done > "$WORK/sources.txt"
	echo "$GLUE/wpa_glue.c" >> "$WORK/sources.txt"
	echo "$APP/wps_eap.c" >> "$WORK/sources.txt"
	echo "$APP/wps_serve.c" >> "$WORK/sources.txt"
	xargs -P "$JOBS" -n 1 -a "$WORK/sources.txt" sh -c '
		src=$1; o='"$O"'/$(basename "$(dirname "$src")")_$(basename "$src" .c).o
		if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find '"$GLUE"' '"$WPP"' -maxdepth 1 -newer "$o" -name "*.h" | head -1)" ]; then
			'"$CC"' '"$(echo $CFLAGS)"' -c "$src" -o "$o" || exit 255
		fi' _
	while read -r s; do
		objs="$objs $O/$(basename "$(dirname "$s")")_$(basename "$s" .c).o"
	done < "$WORK/sources.txt"
	local o=$OUT/wpasupp_lib.o
	$LD -r --gc-sections -u wps_serve -o "$o.all" $objs "$OB/libssl-ts.a" "$OB/libcrypto-ts.a"
	$OBJCOPY --keep-global-symbol=wps_serve "$o.all" "$o"
	rm -f "$o.all"
	cp "$WSRC/dist/COPYING" "$OUT/COPYING-wpa_supplicant.txt"
	cp "$OSRC/LICENSE.txt" "$OUT/LICENSE-openssl.txt"
	${TP}size "$o" | tail -1 | awk '{printf "wpaproc: text %d, data %d, bss %d bytes\n", $1, $2, $3}'
	echo "wpaproc: $o; it needs: $($NM -u "$o" | awk '{print $2}' | sort -u | tr '\n' ' ')"
}

# ---------------------------------------------------------------- the host's tests

HOST=$OUT/host
HDEFS="-DCONFIG_NO_RANDOM_POOL -DCONFIG_SHA256 -DCONFIG_SHA384 -DCONFIG_SHA512
	-DCONFIG_HMAC_SHA256_KDF -DCONFIG_HMAC_SHA384_KDF -DCONFIG_HMAC_SHA512_KDF -DCONFIG_SAE -DCONFIG_ECC
	-DCONFIG_NO_RADIUS -DCONFIG_NO_ACCOUNTING -DCONFIG_NO_VLAN -DCONFIG_NO_CONFIG_BLOBS -DCONFIG_NO_RC4
	-DTSWPA_HOST_TEST"
# The process's side of the test under other names (the kernel's side has knl_wpa_*, and the
# test's callbacks knl_wifi_*)
SIDE=""
for f in classify psk start choose start2 sae_pt sae_start sae_rx sae_resend sae_pmk sae_abort \
	 assoc rx state disassoc tick eap_set eap_assoc eap_rx eap_result eap_end; do SIDE="$SIDE -Dknl_wpa_$f=wps_$f"; done
for f in eapol_tx setkey delkey deauth say; do SIDE="$SIDE -Dknl_wifi_$f=wps_cb_$f"; done

host_openssl() {
	configure_openssl "$HOST/openssl" gcc "-O1 -g" linux-x86_64
	libcrypto "$HOST/openssl" ar ""
}

host_flags() {
	echo "-O1 -g -std=gnu11 -w $HDEFS -I$GLUE -I$WPP -I$D -I$D/utils -I$D/crypto
		-I$HOST/openssl/include -I$OSRC/include"
}

test_host() {
	host_openssl
	local F srcs="" s
	F=$(host_flags)
	for s in $SRCS; do srcs="$srcs $D/$s"; done
	mkdir -p "$HOST/obj"
	gcc $F $SIDE -c -o "$HOST/obj/glue.o" "$GLUE/wpa_glue.c" 2> "$HOST/cc.log" || { tail -20 "$HOST/cc.log"; exit 1; }
	gcc $F $SIDE -c -o "$HOST/obj/serve.o" "$APP/wps_serve.c" 2>> "$HOST/cc.log" || { tail -20 "$HOST/cc.log"; exit 1; }
	gcc $F -o "$HOST/test_wpa" "$TOP/tools/wpa/test_wpa.c" "$WPP/wpaproc_call.c" "$TOP/tools/wpa/proc_loop.c" \
		"$HOST/obj/glue.o" "$HOST/obj/serve.o" $srcs \
		"$D/ap/wpa_auth.c" "$D/ap/wpa_auth_ie.c" "$D/ap/pmksa_cache_auth.c" \
		"$D/utils/os_unix.c" "$D/utils/wpa_debug.c" "$HOST/openssl/libcrypto-ts.a" 2>> "$HOST/cc.log" \
		|| { tail -30 "$HOST/cc.log"; exit 1; }
	"$HOST/test_wpa"
}

# What the module tests need besides: GAS's frames, the event loop's timers, MS-CHAPv2's
# functions and the algorithms of their other cases, after OpenSSL's in the link (the first
# definition is taken); MD4 and single DES, which the configuration of OpenSSL leaves out with
# the legacy algorithms, before it (MS-CHAPv2 is not in the process)
MODSRCS="$D/common/gas.c $D/utils/eloop.c $D/crypto/ms_funcs.c
	$D/crypto/fips_prf_internal.c $D/crypto/sha1-internal.c"
MODPRE="$D/crypto/md4-internal.c $D/crypto/des-internal.c"

# 802.1X on the host: the peer of wpa_eap.c against the release's EAP server (tools/wpa/test_eap.c)
eap_certs() {
	local c=$1
	mkdir -p "$c"
	[ -f "$c/client.key" ] && return 0
	(cd "$c"
	 mkca() { command openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout "$1.key" -out "$1.pem" -subj "/CN=$1" 2>/dev/null; }
	 leaf() {	# name CN signer
		command openssl req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$2" 2>/dev/null
		printf 'subjectAltName=DNS:%s\n' "$2" > "$1.ext"
		command openssl x509 -req -in "$1.csr" -CA "$3.pem" -CAkey "$3.key" -CAcreateserial -days 3650 -extfile "$1.ext" -out "$1.pem" 2>/dev/null
	 }
	 mkca ca; mkca other_ca
	 leaf server radius.example ca
	 leaf client user.example ca
	 leaf bad user.example other_ca)
}

eap_test() {
	host_openssl
	libssl "$HOST/openssl" ar
	local F srcs="" s
	F="-O1 -g -std=gnu11 -w $(echo "$HDEFS" | sed 's/-DCONFIG_NO_CONFIG_BLOBS//') $EAPDEFS -DEAP_SERVER -DEAP_SERVER_IDENTITY
		-DEAP_SERVER_PEAP -DEAP_SERVER_TLS -DEAP_SERVER_MSCHAPV2 -I$GLUE -I$WPP -I$D -I$D/utils -I$D/crypto
		-I$HOST/openssl/include -I$OSRC/include"
	for s in $EAPSRCS; do srcs="$srcs $D/$s"; done
	for s in eap_server.c eap_server_methods.c eap_server_identity.c eap_server_peap.c eap_server_tls.c \
		 eap_server_tls_common.c eap_server_mschapv2.c; do srcs="$srcs $D/eap_server/$s"; done
	eap_certs "$HOST/certs"
	gcc $F -o "$HOST/test_eap" "$TOP/tools/wpa/test_eap.c" "$APP/wps_eap.c" $srcs \
		"$D/utils/common.c" "$D/utils/wpabuf.c" "$D/utils/eloop.c" "$D/utils/os_unix.c" "$D/utils/wpa_debug.c" \
		"$D/crypto/crypto_openssl.c" "$D/crypto/sha1-prf.c" "$D/crypto/sha1-tlsprf.c" "$D/crypto/sha256-prf.c" \
		"$HOST/openssl/libssl-ts.a" "$HOST/openssl/libcrypto-ts.a" -lpthread 2> "$HOST/cc-eap.log" \
		|| { grep -E "error|undefined" "$HOST/cc-eap.log" | sort -u | head -40; exit 1; }
	"$HOST/test_eap" "$HOST/certs"
}

mod_test() {
	host_openssl
	local F srcs="" s
	F=$(host_flags)
	for s in $SRCS; do srcs="$srcs $D/$s"; done
	mkdir -p "$HOST"
	gcc $F -DCONFIG_MODULE_TESTS -o "$HOST/modtest" "$TOP/tools/wpa/modtest.c" $MODPRE $srcs \
		"$D/common/common_module_tests.c" "$D/crypto/crypto_module_tests.c" $MODSRCS \
		"$D/utils/os_unix.c" "$D/utils/wpa_debug.c" "$HOST/openssl/libcrypto-ts.a" \
		-Wl,--allow-multiple-definition 2> "$HOST/cc-mod.log" || { tail -30 "$HOST/cc-mod.log"; exit 1; }
	"$HOST/modtest"
}

case $STAGE in
fetch)		fetch ;;
openssl)	fetch; openssl ;;
compile)	fetch; openssl; compile ;;
all)		fetch; openssl; compile ;;
test)		fetch; test_host ;;
modtest)	fetch; mod_test ;;
eaptest)	fetch; eap_test ;;
clean)		rm -rf "$OUT" ;;
*)		echo "usage: $0 [fetch|openssl|compile|all|test|modtest|eaptest|clean]" >&2; exit 2 ;;
esac
