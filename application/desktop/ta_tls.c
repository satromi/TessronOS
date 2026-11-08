/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ta_tls.c
 *	The server's side of TLS 1.2 with a shared key, for the test agent
 *	(make TESTAGENT=1, design 12.4.1)
 *
 *	A client without the key gets no further than the handshake: the
 *	agent reads no line before ta_tls_open has answered.
 */

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "ta_tls.h"

extern void *Kcalloc( size_t n, size_t size );
extern void Kfree( void *p );
extern int ts_get_random( void *buf, long len );

/* What the client must name along with the key */
#define TA_IDENTITY	"tessron-test"

static mbedtls_ssl_config	ta_conf;

static const int	ta_suites[] = { MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0 };

void *ta_tls_calloc( size_t n, size_t size )
{
	return Kcalloc(n, size);
}

void ta_tls_free( void *p )
{
	Kfree(p);
}

static int ta_rng( void *ctx, unsigned char *buf, size_t n )
{
	(void)ctx;
	return ( ts_get_random(buf, (long)n) < 0 ) ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : 0;
}

int ta_tls_setup( const unsigned char *psk, int len )
{
	int	er;

	mbedtls_ssl_config_init(&ta_conf);
	er = mbedtls_ssl_config_defaults(&ta_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
					 MBEDTLS_SSL_PRESET_DEFAULT);
	if ( er != 0 ) {
		return er;
	}
	mbedtls_ssl_conf_rng(&ta_conf, ta_rng, NULL);
	mbedtls_ssl_conf_ciphersuites(&ta_conf, ta_suites);
	mbedtls_ssl_conf_min_tls_version(&ta_conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&ta_conf, MBEDTLS_SSL_VERSION_TLS1_2);
	return mbedtls_ssl_conf_psk(&ta_conf, psk, (size_t)len,
				    (const unsigned char *)TA_IDENTITY, sizeof(TA_IDENTITY) - 1);
}

void *ta_tls_open( ta_io_send snd, ta_io_recv rcv, void *ctx, int *p_err )
{
	mbedtls_ssl_context	*ssl = (mbedtls_ssl_context *)Kcalloc(1, sizeof(*ssl));
	int			er;

	if ( ssl == NULL ) {
		*p_err = MBEDTLS_ERR_SSL_ALLOC_FAILED;
		return NULL;
	}
	mbedtls_ssl_init(ssl);
	er = mbedtls_ssl_setup(ssl, &ta_conf);
	if ( er == 0 ) {
		mbedtls_ssl_set_bio(ssl, ctx, snd, rcv, NULL);
		er = mbedtls_ssl_handshake(ssl);
	}
	if ( er != 0 ) {
		mbedtls_ssl_free(ssl);
		Kfree(ssl);
		*p_err = er;
		return NULL;
	}
	*p_err = 0;
	return ssl;
}

int ta_tls_read( void *conn, void *buf, int max )
{
	int	n = mbedtls_ssl_read((mbedtls_ssl_context *)conn, (unsigned char *)buf, (size_t)max);

	return ( n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ) ? 0 : n;
}

int ta_tls_write( void *conn, const void *buf, size_t n )
{
	const unsigned char	*p = (const unsigned char *)buf;

	while ( n > 0 ) {
		int	k = mbedtls_ssl_write((mbedtls_ssl_context *)conn, p, n);

		if ( k < 0 ) {
			return k;
		}
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

void ta_tls_close( void *conn )
{
	(void)mbedtls_ssl_close_notify((mbedtls_ssl_context *)conn);
	mbedtls_ssl_free((mbedtls_ssl_context *)conn);
	Kfree(conn);
}
