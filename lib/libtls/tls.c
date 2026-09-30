/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tls.c
 *	TLS over a connected socket (include/ts/tls.h), on mbed TLS
 *
 *	What mbed TLS asks of the system is here: its entropy from the
 *	random source object 乱数 (ts_get_random, which keeps a key to it
 *	open, lib/libts/ts_random.c), the time from the calendar (seconds since 1985 turned
 *	into seconds since 1970), and the bytes of the socket. The socket is
 *	read only when so_poll says there is something, so that a wait is
 *	the caller's wait and not a block of the task. The roots are read
 *	once, the first time they are needed, from the record 1 of
 *	ルート証明書.
 */

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/soapp.h>
#include <ts/ob.h>
#include <ts/dt.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/tls.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include <ts/time.h>
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

#define CA_CHUNK	16384

/* what the socket answers mbed TLS with (its own net layer is not built) */
#define ERR_SEND	(-0x004E)
#define ERR_RECV	(-0x004C)

struct ts_tls {
	INT			s;
	INT			tmo_ms;
	TS_TLS_WAIT		wait;
	void			*arg;
	BOOL			stopped;
	mbedtls_ssl_context	ssl;
	mbedtls_ssl_config	conf;
};

static BOOL			ready;
static mbedtls_entropy_context	entropy;
static mbedtls_ctr_drbg_context	drbg;
static mbedtls_x509_crt		roots;
static UW			last_verify;

/* ---------------------------------------------------------------- what mbed TLS asks of the system */

int mbedtls_hardware_poll( void *data, unsigned char *output, size_t len, size_t *olen )
{
	(void)data;
	if ( ts_get_random(output, (SZ)len) < E_OK ) {
		*olen = 0;
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}
	*olen = len;
	return 0;
}

/* The calendar counts from 1985; the certificates from 1970 */
#define EPOCH_1985	473385600

time_t ts_tls_time( time_t *t )
{
	TS_TIME	now = 0;
	time_t	v;

	(void)dt_gettime(&now);
	v = (time_t)now + EPOCH_1985;
	if ( t != NULL ) {
		*t = v;
	}
	return v;
}

/* Milliseconds from any fixed moment: the monotonic clock's */
mbedtls_ms_time_t mbedtls_ms_time( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (mbedtls_ms_time_t)( ns / 1000000 );
}

struct tm *mbedtls_platform_gmtime_r( const mbedtls_time_t *tt, struct tm *out )
{
	TS_TIME	t = (TS_TIME)( *tt - EPOCH_1985 );
	TS_TM	tm;

	if ( dt_gmtime(&t, &tm) < E_OK ) {
		return NULL;
	}
	memset(out, 0, sizeof(*out));
	out->tm_sec = tm.tm_sec;
	out->tm_min = tm.tm_min;
	out->tm_hour = tm.tm_hour;
	out->tm_mday = tm.tm_mday;
	out->tm_mon = tm.tm_mon;
	out->tm_year = tm.tm_year;
	out->tm_wday = tm.tm_wday;
	out->tm_yday = tm.tm_yday;
	return out;
}

/* ---------------------------------------------------------------- the roots */

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", as the object's number */
static BOOL uuid_of( const char *s, TS_UUID *u )
{
	UB	b[16];
	INT	n = 0, v;

	for ( ; *s != 0 && n < 32; s++ ) {
		if ( *s == '-' ) continue;
		if ( *s >= '0' && *s <= '9' ) v = *s - '0';
		else if ( *s >= 'a' && *s <= 'f' ) v = *s - 'a' + 10;
		else if ( *s >= 'A' && *s <= 'F' ) v = *s - 'A' + 10;
		else return FALSE;
		if ( ( n & 1 ) == 0 ) b[n / 2] = (UB)( v << 4 );
		else b[n / 2] |= (UB)v;
		n++;
	}
	if ( n != 32 ) return FALSE;
	memcpy(u, b, sizeof(b));
	return TRUE;
}

static INT roots_load( void )
{
	TS_UUID	u;
	ID	key;
	UB	*pem = NULL;
	INT	n = 0, cap = 0, r;
	SZ	asz;

	if ( !uuid_of(SYSDEF_CA_CERTS, &u) ) {
		return TS_TLS_E_NOCA;
	}
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key <= 0 ) {
		return TS_TLS_E_NOCA;
	}
	for ( ;; ) {
		if ( n + CA_CHUNK + 1 > cap ) {
			UB	*p = realloc(pem, (size_t)( cap + CA_CHUNK * 4 + 1 ));

			if ( p == NULL ) {
				free(pem);
				ob_cls_obj(key);
				return TS_TLS_E_NOMEM;
			}
			pem = p;
			cap += CA_CHUNK * 4 + 1;
		}
		asz = 0;
		if ( ob_rea_rec(key, 1, n, pem + n, CA_CHUNK, &asz) < E_OK || asz <= 0 ) {
			break;
		}
		n += (INT)asz;
		if ( asz < CA_CHUNK ) {
			break;
		}
	}
	ob_cls_obj(key);
	if ( n == 0 ) {
		free(pem);
		return TS_TLS_E_NOCA;
	}
	pem[n] = 0;
	r = mbedtls_x509_crt_parse(&roots, pem, (size_t)n + 1);
	free(pem);
	/* a root this build cannot read is left out; the others stay */
	return ( r < 0 ) ? r : 0;
}

static INT setup( void )
{
	static const char pers[] = "TessronOS TLS";
	INT	r;

	if ( ready ) {
		return 0;
	}
	if ( psa_crypto_init() != PSA_SUCCESS ) {
		return TS_TLS_E_NOMEM;
	}
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&roots);
	r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
				  (const unsigned char *)pers, sizeof(pers) - 1);
	if ( r != 0 ) {
		return r;
	}
	r = roots_load();
	if ( r != 0 ) {
		return r;
	}
	ready = TRUE;
	return 0;
}

/* ---------------------------------------------------------------- the socket under it */

static int bio_send( void *ctx, const unsigned char *buf, size_t len )
{
	TS_TLS	*t = ctx;
	INT	n = so_send(t->s, buf, (SZ)len, 0);

	if ( n < 0 ) {
		return ERR_SEND;
	}
	return n;
}

static int bio_recv( void *ctx, unsigned char *buf, size_t len )
{
	TS_TLS		*t = ctx;
	struct pollfd	p;
	INT		n;

	p.fd = t->s;
	p.events = POLLIN;
	p.revents = 0;
	if ( so_poll(&p, 1, 0) <= 0 ) {
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	n = so_recv(t->s, buf, (SZ)len, 0);
	if ( n < 0 ) {
		return ERR_RECV;
	}
	return n;
}

/* The caller's wait, and whether the other side has been waited for too long */
static INT waited( TS_TLS *t, INT *p_spent )
{
	if ( t->wait != NULL && !t->wait(t->arg) ) {
		t->stopped = TRUE;
		return TS_TLS_E_STOPPED;
	}
	*p_spent += 20;
	if ( t->tmo_ms > 0 && *p_spent >= t->tmo_ms ) {
		return TS_TLS_E_TIMEOUT;
	}
	return 0;
}

/* ---------------------------------------------------------------- the calls */

TS_TLS *ts_tls_open( INT s, CONST char *host, INT tmo_ms, TS_TLS_WAIT wait, void *arg, INT *p_err )
{
	TS_TLS	*t;
	INT	r, spent = 0;

	*p_err = setup();
	if ( *p_err != 0 ) {
		return NULL;
	}
	t = calloc(1, sizeof(TS_TLS));
	if ( t == NULL ) {
		*p_err = TS_TLS_E_NOMEM;
		return NULL;
	}
	t->s = s;
	t->tmo_ms = tmo_ms;
	t->wait = wait;
	t->arg = arg;
	mbedtls_ssl_init(&t->ssl);
	mbedtls_ssl_config_init(&t->conf);
	r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT);
	if ( r == 0 ) {
		mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&t->conf, &roots, NULL);
		mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &drbg);
		r = mbedtls_ssl_setup(&t->ssl, &t->conf);
	}
	if ( r == 0 ) {
		r = mbedtls_ssl_set_hostname(&t->ssl, host);
	}
	if ( r == 0 ) {
		mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
		last_verify = 0;
		while ( ( r = mbedtls_ssl_handshake(&t->ssl) ) != 0 ) {
			if ( r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE ) {
				break;
			}
			if ( ( r = waited(t, &spent) ) != 0 ) {
				break;
			}
		}
		if ( r != 0 ) {
			UW	f = mbedtls_ssl_get_verify_result(&t->ssl);

			if ( f != 0 && f != (UW)-1 ) {
				last_verify = f;
				r = TS_TLS_E_VERIFY;
			}
		}
	}
	if ( r != 0 ) {
		*p_err = r;
		mbedtls_ssl_free(&t->ssl);
		mbedtls_ssl_config_free(&t->conf);
		free(t);
		return NULL;
	}
	*p_err = 0;
	return t;
}

INT ts_tls_write( TS_TLS *t, CONST void *buf, INT len )
{
	INT	done = 0, r, spent = 0;

	while ( done < len ) {
		r = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf + done, (size_t)( len - done ));
		if ( r > 0 ) {
			done += r;
			continue;
		}
		if ( r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE ) {
			return r;
		}
		if ( ( r = waited(t, &spent) ) != 0 ) {
			return r;
		}
	}
	return done;
}

INT ts_tls_read( TS_TLS *t, void *buf, INT len )
{
	INT	r, spent = 0;

	for ( ;; ) {
		r = mbedtls_ssl_read(&t->ssl, buf, (size_t)len);
		if ( r >= 0 ) {
			return r;
		}
		if ( r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ) {
			return 0;
		}
		if ( r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE
		  && r != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ) {
			return r;
		}
		if ( r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ) {
			continue;
		}
		if ( ( r = waited(t, &spent) ) != 0 ) {
			return r;
		}
	}
}

void ts_tls_close( TS_TLS *t )
{
	if ( t == NULL ) {
		return;
	}
	if ( !t->stopped ) {
		(void)mbedtls_ssl_close_notify(&t->ssl);
	}
	mbedtls_ssl_free(&t->ssl);
	mbedtls_ssl_config_free(&t->conf);
	free(t);
}

void ts_tls_strerror( INT err, char *out, INT max )
{
	switch ( err ) {
	case TS_TLS_E_NOCA:	strncpy(out, "no root certificates", (size_t)max); break;
	case TS_TLS_E_NOMEM:	strncpy(out, "no memory", (size_t)max); break;
	case TS_TLS_E_TIMEOUT:	strncpy(out, "timed out", (size_t)max); break;
	case TS_TLS_E_STOPPED:	strncpy(out, "stopped", (size_t)max); break;
	case TS_TLS_E_VERIFY:	(void)mbedtls_x509_crt_verify_info(out, (size_t)max, "", last_verify); break;
	default:		mbedtls_strerror(err, out, (size_t)max); break;
	}
	if ( max > 0 ) {
		out[max - 1] = 0;
	}
}

UW ts_tls_verify( void )
{
	return last_verify;
}
