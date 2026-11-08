/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_mbed_config.h
 *	What of mbed TLS the kernel has for LE pairing (MBEDTLS_CONFIG_FILE,
 *	design 6.8): AES-128 and the curve P-256, nothing else. The pieces use
 *	no floating point or vector registers, as the kernel saves none: no
 *	accelerated AES and no assembly. The tables stay in ROM, and the
 *	double-length division that would need the compiler's library is left
 *	out.
 */

#ifndef BT_MBED_CONFIG_H
#define BT_MBED_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO	knl_bts_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO	knl_bts_free
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_DEPRECATED_REMOVED

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES

#include <stddef.h>
void *knl_bts_calloc( size_t n, size_t size );
void knl_bts_free( void *p );

#endif /* BT_MBED_CONFIG_H */
