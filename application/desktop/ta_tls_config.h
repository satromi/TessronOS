/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ta_tls_config.h
 *	What of mbed TLS the test agent has (MBEDTLS_CONFIG_FILE, make
 *	TESTAGENT=1, design 12.4.1): the server of TLS 1.2 with one cipher
 *	suite, a key shared in advance and AES-128-GCM, and nothing else: no
 *	certificates and no public key arithmetic. The pieces use no floating
 *	point or vector registers, as the kernel saves none: no accelerated
 *	AES and no assembly.
 */

#ifndef TA_TLS_CONFIG_H
#define TA_TLS_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO	ta_tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO	ta_tls_free
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_DEPRECATED_REMOVED

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#define MBEDTLS_SSL_CIPHERSUITES	MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256
#define MBEDTLS_PSK_MAX_LEN		64

#include <stddef.h>
void *ta_tls_calloc( size_t n, size_t size );
void ta_tls_free( void *p );

#endif /* TA_TLS_CONFIG_H */
