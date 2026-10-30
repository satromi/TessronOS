/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_wpa_mbedtls_config.h
 *	What of Mbed TLS (lib/mbedtls) the kernel's Wi-Fi has
 *	(MBEDTLS_CONFIG_FILE of tools/wpa/build.sh)
 *
 *	wpa_supplicant's cryptography is given by Mbed TLS through
 *	wpa_supplicant's own layer for it (crypto_mbedtls.c): the hashes and
 *	their HMACs and PBKDF2 of WPA2, AES with its key wrap and CMAC (the
 *	MIC of PSK-SHA256 and the protected management frames), and the big
 *	numbers and elliptic curves of SAE (WPA3-Personal: the groups 19, 20
 *	and 21, the NIST curves P-256, P-384 and P-521). No TLS, no
 *	certificates, no files, no time: what crypto_mbedtls.c's functions of
 *	EC keys need (PK, ASN.1) is in only so that the layer compiles; SAE
 *	does not use them. The random numbers come from the Wi-Fi manager
 *	(mbedtls_hardware_poll, wpa_glue.c), and the memory from the kernel's
 *	C library. Nothing here uses the floating point or vector registers
 *	(no assembly, no AES instructions), as the kernel is built.
 */

#ifndef TS_WPA_MBEDTLS_CONFIG_H
#define TS_WPA_MBEDTLS_CONFIG_H

/* the system: no assembly nor AES instructions, randomness from the manager */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_DEPRECATED_REMOVED

/* numbers and curves (SAE) */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_DHM_C				/* the layer's crypto_dh_* (SAE's finite field groups, not offered) */

/* hashes, ciphers, randomness (WPA2, PMF, SAE's KDF) */
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_CMAC_C
#define MBEDTLS_NIST_KW_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* what the layer's EC key functions need to compile */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C

#endif /* TS_WPA_MBEDTLS_CONFIG_H */
