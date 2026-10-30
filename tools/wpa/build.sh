#!/bin/bash
#
# tools/wpa/build.sh — the WPA2 and WPA3 handshakes of the kernel's Wi-Fi (docs/tessronos/
# 09-network.md 9.6, 9.6.9): the supplicant's state machine of the 4-way and group key
# handshakes, SAE (src/common/sae.c, dragonfly.c) and the protection of management frames,
# taken from wpa_supplicant 2.11 as NetBSD 11.0 carries it (src.tgz, external/bsd/wpa/dist),
# with its cryptography from Mbed TLS (lib/mbedtls, the configuration of device/net/wifi/wpa/
# ts_wpa_mbedtls_config.h) through wpa_supplicant's layer for Mbed TLS (crypto_mbedtls.c,
# which wpa_supplicant 2.11 does not carry: taken from OpenWrt's patch of hostapd, below),
# built as the kernel is built with TessronOS's glue (device/net/wifi/wpa/), into one object
# the kernel links:
#
#   ARCH=x86_64 (the default)	~/tessronos-wpa-x64/wpasupp.o (x86_64-elf, no floating point,
#				no red zone, the kernel's code model; build_make/x64.mk)
#   ARCH=aarch64			~/tessronos-wpa-arm/wpasupp.o (aarch64-none-elf, no floating
#				point registers, MARCH as the kernel's; build_make/wifi_arm.mk)
#
#   [ARCH=aarch64] tools/wpa/build.sh [fetch|compile|link|all|test|clean]
#
# The rest of wpa_supplicant is not used: its layer for the BSDs talks to net80211 through
# routing sockets and BPF, its event loop through select, its configuration through files.
# The Wi-Fi manager (device/net/wifi/wifi.c) does what those did, through the rump
# kernel's entry points (rumpcomp/wifi/tswifi_if.c), and the glue gives the state machine its
# callbacks, its timers, the time, random bytes and a place to print. The sources are not
# changed. Only the glue's functions (knl_wpa_*) stay global; the rest is made local, so that
# none of wpa_supplicant's or Mbed TLS's names meets the kernel's (or the programs' libtls).
# The licences -- wpa_supplicant's BSD one of COPYING, crypto_mbedtls.c's BSD-3-Clause and
# Mbed TLS's Apache-2.0 -- are written beside the object.
#
# "test" builds the same sources with the host's compiler, with the authenticator of hostapd
# (src/ap) in place of an access point, and runs the 4-way and group key handshakes between
# the two, SAE with the access point's side of sae.c, and the known answers of SAE (IEEE
# 802.11-2020 Annex J.10) (tools/wpa/test_wpa.c): the keys each side ends with must agree.
#
set -e

STAGE=${1:-all}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
ARCH=${ARCH:-x86_64}
case $ARCH in
x86_64)
	XB=${TS_X64_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	TP=$XB/x86_64-elf-
	OUT=${OUT:-$HOME/tessronos-wpa-x64}
	ARCH_CFLAGS="-march=x86-64-v2 -mgeneral-regs-only -mno-red-zone -mcmodel=kernel"
	;;
aarch64)
	XB=${TS_XPACK_BIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	TP=$XB/aarch64-none-elf-
	OUT=${OUT:-$HOME/tessronos-wpa-arm}
	ARCH_CFLAGS="-march=${MARCH:-armv8.2-a} -mgeneral-regs-only -mstrict-align -mno-outline-atomics"
	;;
*)
	echo "wpa: ARCH is x86_64 or aarch64, not $ARCH" >&2
	exit 2
	;;
esac
CC=${TP}gcc
LD=${TP}ld
OBJCOPY=${TP}objcopy
NM=${TP}nm
JOBS=${JOBS:-12}

NB_VER=11.0
NB_SRC_SHA512=99a8f96202290ce203d98396305f3ae1e38b49663b1210447cb82d05ee1717969adb6101dab5f06050e9ef981bd6efb798ae1b2e8e7f0079ba5486da4ca82fcc
NB_URL=${NB_URL:-https://cdn.netbsd.org/pub/NetBSD/NetBSD-$NB_VER/source/sets/src.tgz}
DL=${NBDL:-$HOME/netbsd/dl}
WSRC=${WPASRC:-$HOME/netbsd/wpa-$NB_VER}
D=$WSRC/dist/src
GLUE=$TOP/device/net/wifi/wpa
WORK=$OUT/work
MBED=$TOP/lib/mbedtls

# wpa_supplicant's layer for Mbed TLS, as OpenWrt carries it for its hostapd (the first of its
# patches, which adds the file whole; the rest of the patch is not taken)
OW_TAG=v24.10.0
OW_PATCH=110-mbedtls-TLS-crypto-option-initial-port.patch
OW_PATCH_SHA256=93d3f88cdc5f72be4be74e0f0dbff412e803a1427abbcdf4d323645dcc29883a
OW_FILE_SHA256=3ad41315f6502721fb520d42885338d637c368dcab3fcc20d3d9201007e4287d
OW_URL=${OW_URL:-https://raw.githubusercontent.com/openwrt/openwrt/$OW_TAG/package/network/services/hostapd/patches/$OW_PATCH}
CM=$WSRC/crypto_mbedtls.c

# What of Mbed TLS is built: the modules the configuration has (ts_wpa_mbedtls_config.h)
MSRCS="aes.c asn1parse.c asn1write.c bignum.c bignum_core.c bignum_mod.c bignum_mod_raw.c
 block_cipher.c cipher.c cipher_wrap.c cmac.c constant_time.c ctr_drbg.c dhm.c ecdh.c ecdsa.c
 ecp.c ecp_curves.c ecp_curves_new.c entropy.c entropy_poll.c hkdf.c md.c md5.c nist_kw.c oid.c
 pk.c pk_ecc.c pk_wrap.c pkcs5.c pkparse.c pkwrite.c platform.c platform_util.c
 sha1.c sha256.c sha512.c"

# What is built of it: the supplicant's handshakes, the elements they read and write, SAE,
# and the finite field groups' table SAE looks its groups up in; the hashes, ciphers, big
# numbers and curves are Mbed TLS's, through crypto_mbedtls.c
SRCS="rsn_supp/wpa.c rsn_supp/wpa_ie.c rsn_supp/pmksa_cache.c
 common/wpa_common.c common/ieee802_11_common.c common/sae.c common/dragonfly.c
 utils/common.c utils/wpabuf.c crypto/dh_groups.c"

DEFS="-DCONFIG_NO_RANDOM_POOL -DCONFIG_NO_CONFIG_BLOBS
 -DCONFIG_NO_VLAN -DCONFIG_SHA256 -DCONFIG_SHA384 -DCONFIG_SHA512 -DCONFIG_NO_RADIUS
 -DCONFIG_HMAC_SHA256_KDF -DCONFIG_HMAC_SHA384_KDF -DCONFIG_HMAC_SHA512_KDF
 -DCONFIG_SAE -DCONFIG_ECC -DCONFIG_NO_ACCOUNTING -D__TESSRONOS_KERNEL__
 -D__BYTE_ORDER=1234 -D__LITTLE_ENDIAN=1234 -D__BIG_ENDIAN=4321
 -Dbswap_16=__builtin_bswap16 -Dbswap_32=__builtin_bswap32 -Dbswap_64=__builtin_bswap64"

GCCINC=$("$CC" -print-file-name=include)
CFLAGS="$ARCH_CFLAGS -O2 -g
 -ffreestanding -fno-builtin -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -fno-strict-aliasing -std=gnu11 -Wall -Wno-unused -Wno-unused-parameter
 -Werror=implicit-function-declaration -Werror=int-conversion
 -nostdinc -isystem $GCCINC -I$GLUE/include -I$GLUE -I$D -I$D/utils $DEFS"
MBEDDEFS="-DMBEDTLS_CONFIG_FILE=\\\"ts_wpa_mbedtls_config.h\\\""
MCFLAGS="$ARCH_CFLAGS -O2 -g
 -ffreestanding -fno-builtin -fno-common -fno-pic -fno-stack-protector -fno-omit-frame-pointer
 -fno-strict-aliasing -std=gnu11 -Wno-unused
 -Werror=implicit-function-declaration -Werror=int-conversion
 -nostdinc -isystem $GCCINC -I$GLUE/include -I$GLUE -I$MBED/include $MBEDDEFS"

# crypto_mbedtls.c out of OpenWrt's patch: the lines the patch adds to the new file, checked
fetch_layer() {
	[ -f "$CM" ] && return 0
	local pf=$DL/openwrt-$OW_TAG-$OW_PATCH
	if [ ! -f "$pf" ]; then
		mkdir -p "$DL"
		curl -fsSL -m 600 -o "$pf.part" "$OW_URL" || { rm -f "$pf.part"; echo "wpa: $OW_URL could not be had (set OW_URL)" >&2; exit 1; }
		mv "$pf.part" "$pf"
	fi
	echo "$OW_PATCH_SHA256  $pf" | sha256sum -c - >/dev/null || { echo "wpa: $pf is not OpenWrt $OW_TAG's $OW_PATCH" >&2; exit 1; }
	awk '/^\+\+\+ b\/src\/crypto\/crypto_mbedtls.c/ {f=1; next} f && /^--- / {f=0} f && /^@@/ {next} f && /^\+/ {print substr($0, 2)}' \
		"$pf" > "$CM.part"
	echo "$OW_FILE_SHA256  $CM.part" | sha256sum -c - >/dev/null || { rm -f "$CM.part"; echo "wpa: crypto_mbedtls.c out of $pf is not as expected" >&2; exit 1; }
	mv "$CM.part" "$CM"
	echo "wpa: wpa_supplicant's layer for Mbed TLS in $CM"
}

fetch() {
	[ -f "$D/rsn_supp/wpa.c" ] && { fetch_layer; return 0; }
	local tgz=$DL/src.tgz
	if [ ! -f "$tgz" ]; then
		mkdir -p "$DL"
		if ! curl -fsSL -m 3600 -o "$tgz.part" "$NB_URL"; then
			rm -f "$tgz.part"
			echo "wpa: NetBSD $NB_VER's src.tgz could not be had from $NB_URL (set NB_URL to a place" \
			     "that has it, or NBDL to the directory it is in); tools/wpa/build.sh all makes" \
			     "$OUT/wpasupp.o from it" >&2
			exit 1
		fi
		mv "$tgz.part" "$tgz"
	fi
	echo "$NB_SRC_SHA512  $tgz" | sha512sum -c - || { echo "wpa: $tgz is not NetBSD $NB_VER's src.tgz" >&2; exit 1; }
	mkdir -p "$WSRC"
	tar xzf "$tgz" -C "$WSRC" --strip-components=5 --exclude='*/CVS' \
		usr/src/external/bsd/wpa/dist/src usr/src/external/bsd/wpa/dist/COPYING \
		usr/src/external/bsd/wpa/dist/README
	echo "wpa: wpa_supplicant's sources in $WSRC"
	fetch_layer
}

# Each source into its object when it or a header of the glue is newer: wpa_supplicant's,
# crypto_mbedtls.c and the glue with wpa_supplicant's flags, Mbed TLS's with its configuration
compile_list() {
	local list=$1 flags=$2
	xargs -P "$JOBS" -n 1 -a "$list" sh -c '
		src=$1; o='"$WORK"'/obj/$(basename "$(dirname "$src")")_$(basename "$src" .c).o
		if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find '"$GLUE"' -newer "$o" -name "*.h" | head -1)" ]; then
			'"$CC"' '"$(echo $flags)"' -fstack-usage -c "$src" -o "$o" || exit 255
		fi' _
}

compile() {
	mkdir -p "$WORK/obj"
	local list=$WORK/sources.txt mlist=$WORK/mbedtls.txt s
	: > "$list"
	for s in $SRCS; do
		[ -f "$D/$s" ] || { echo "wpa: $s is not in the release" >&2; exit 1; }
		echo "$D/$s" >> "$list"
	done
	echo "$CM" >> "$list"
	ls "$GLUE"/*.c >> "$list"
	[ -z "$AP_GLUE" ] || echo "$AP_GLUE" >> "$list"
	: > "$mlist"
	for s in $MSRCS; do echo "$MBED/library/$s" >> "$mlist"; done
	compile_list "$list" "$CFLAGS -I$D/crypto -I$MBED/include $MBEDDEFS $AP_FLAGS"
	compile_list "$mlist" "$MCFLAGS"
	# the objects of these sources alone are linked (not those of sources built before)
	cat "$list" "$mlist" | while read -r s; do
		echo "$WORK/obj/$(basename "$(dirname "$s")")_$(basename "$s" .c).o"
	done > "$WORK/objects.txt"
	echo "wpa: $(wc -l < "$list") sources of wpa_supplicant and the glue, $(wc -l < "$mlist") of Mbed TLS compiled"
	# the deepest stacks, for the manager's task (its stack is W_STKSZ)
	cat "$WORK"/obj/*.su 2>/dev/null | sort -t'	' -k2 -n -r | head -8 > "$WORK/stack.txt"
	echo "wpa: the largest frames:"; sed 's/^/wpa:   /' "$WORK/stack.txt"
}

link() {
	local o=$OUT/${LINK_NAME:-wpasupp.o}
	$LD -r -o "$o.all" $(cat "$WORK/objects.txt")
	$NM -g --defined-only "$o.all" | awk '{print $3}' | grep -E "${LINK_KEEP:-^knl_wpa_}" > "$WORK/keep.txt"
	$OBJCOPY --keep-global-symbols="$WORK/keep.txt" "$o.all" "$o"
	rm -f "$o.all"
	cp "$WSRC/dist/COPYING" "$OUT/COPYING-wpa_supplicant.txt"
	cp "$MBED/LICENSE" "$OUT/LICENSE-mbedtls.txt"
	${TP}size "$o" | tail -1 | awk '{printf "wpa: in the kernel: text %d, data %d, bss %d bytes\n", $1, $2, $3}'
	local undef
	undef=$($NM -u "$o" | awk '{print $2}' | sort -u | tr '\n' ' ')
	echo "wpa: $o ($(du -k "$o" | cut -f1) KB); it needs of the kernel: $undef"
}

# The handshakes between the supplicant and hostapd's authenticator, on the host
HDEFS="-DCONFIG_NO_RANDOM_POOL -DCONFIG_SHA256 -DCONFIG_SHA384 -DCONFIG_SHA512
	-DCONFIG_HMAC_SHA256_KDF -DCONFIG_HMAC_SHA384_KDF -DCONFIG_HMAC_SHA512_KDF -DCONFIG_SAE -DCONFIG_ECC
	-DCONFIG_NO_RADIUS -DCONFIG_NO_ACCOUNTING -DCONFIG_NO_VLAN -DCONFIG_NO_CONFIG_BLOBS -DTSWPA_HOST_TEST"
# What the module tests need besides: GAS's frames, the event loop's timers, and the algorithms
# of their other cases that the kernel does not have (MS-CHAPv2's, with MD4 and DES, and FIPS
# 186-2's PRF on SHA-1's internals); for the host only, linked after Mbed TLS's, so that the
# kernel's functions are those of Mbed TLS (--allow-multiple-definition takes the first)
MODSRCS="$D/common/gas.c $D/utils/eloop.c $D/crypto/ms_funcs.c $D/crypto/md4-internal.c
	$D/crypto/des-internal.c $D/crypto/fips_prf_internal.c $D/crypto/sha1-internal.c"

# Mbed TLS with the kernel's configuration for the host, kept between runs (MOBJS)
test_host_mbedtls() {
	local T=$OUT/test s o
	mkdir -p "$T/obj"
	MOBJS=""
	for s in $MSRCS; do
		s=$MBED/library/$s
		o=$T/obj/$(basename "$s" .c).o
		if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ "$GLUE/ts_wpa_mbedtls_config.h" -nt "$o" ]; then
			gcc -O1 -g -std=gnu11 -w -I"$GLUE" -I"$MBED/include" \
				'-DMBEDTLS_CONFIG_FILE="ts_wpa_mbedtls_config.h"' -c "$s" -o "$o" || exit 1
		fi
		MOBJS="$MOBJS $o"
	done
}

test_host() {
	local T=$OUT/test
	test_host_mbedtls
	local srcs="" s
	for s in $SRCS; do srcs="$srcs $D/$s"; done
	gcc -O1 -g -std=gnu11 -w $HDEFS -I"$GLUE" -I"$D" -I"$D/utils" -I"$D/crypto" -I"$MBED/include" \
		'-DMBEDTLS_CONFIG_FILE="ts_wpa_mbedtls_config.h"' \
		-o "$T/test_wpa" "$TOP/tools/wpa/test_wpa.c" "$GLUE/wpa_glue.c" $srcs "$CM" \
		"$D/ap/wpa_auth.c" "$D/ap/wpa_auth_ie.c" "$D/ap/pmksa_cache_auth.c" \
		"$D/utils/os_unix.c" "$D/utils/wpa_debug.c" $MOBJS 2> "$T/cc.log" \
		|| { tail -30 "$T/cc.log"; exit 1; }
	"$T/test_wpa"
}

# wpa_supplicant's own module tests (src/common/common_module_tests.c, src/crypto/
# crypto_module_tests.c, as the release has them) with the same sources and Mbed TLS
mod_test() {
	local T=$OUT/test
	test_host_mbedtls
	local srcs="" s
	for s in $SRCS; do srcs="$srcs $D/$s"; done
	gcc -O1 -g -std=gnu11 -w $HDEFS -DCONFIG_MODULE_TESTS -I"$GLUE" -I"$D" -I"$D/utils" -I"$D/crypto" \
		-I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="ts_wpa_mbedtls_config.h"' \
		-o "$T/modtest" "$TOP/tools/wpa/modtest.c" $srcs "$CM" "$D/common/common_module_tests.c" \
		"$D/crypto/crypto_module_tests.c" $MODSRCS "$D/utils/os_unix.c" "$D/utils/wpa_debug.c" \
		$MOBJS -Wl,--allow-multiple-definition 2> "$T/cc-mod.log" || { tail -30 "$T/cc-mod.log"; exit 1; }
	"$T/modtest"
}
# The access point's side of the Wi-Fi simulator (WIFI_SIM=1, docs/tessronos/09-network.md 9.6.12):
# hostapd's authenticator (src/ap) with its glue (device/net/wifi/sim/wsap_glue.c) and a copy of
# the station's sources, glue and Mbed TLS of its own, into $OUT/wpaap.o (OUT is to be another
# place than the station's). Only the knl_wsap_ names stay global; what the copy of the station's
# glue calls of the Wi-Fi manager (knl_wifi_*) is renamed to the access point's at the compilation,
# so that nothing of the two meets
ap_build() {
	SRCS="$SRCS ap/wpa_auth.c ap/wpa_auth_ie.c ap/pmksa_cache_auth.c"
	AP_GLUE=$TOP/device/net/wifi/sim/wsap_glue.c
	AP_FLAGS="-I$TOP/device/net/wifi/sim -include $TOP/device/net/wifi/sim/wsap_compat.h -Dknl_wifi_say=knl_wsap_say -Dknl_wifi_ms=knl_wsap_ms
	 -Dknl_wifi_random=knl_wsap_random -Dknl_wifi_halt=knl_wsap_halt
	 -Dknl_wifi_eapol_tx=wsap_sta_eapol_tx -Dknl_wifi_setkey=wsap_sta_setkey
	 -Dknl_wifi_delkey=wsap_sta_delkey -Dknl_wifi_deauth=wsap_sta_deauth"
	compile
	LINK_NAME=wpaap.o LINK_KEEP='^knl_wsap_' link
}

case $STAGE in
fetch)		fetch ;;
compile)	fetch; compile ;;
link)		link ;;
all)		fetch; compile; link ;;
test)		fetch; test_host ;;
modtest)	fetch; mod_test ;;
ap)		fetch; ap_build ;;
clean)		rm -rf "$OUT" ;;
*)		echo "usage: $0 [fetch|compile|link|all|test|modtest|ap|clean]" >&2; exit 2 ;;
esac
