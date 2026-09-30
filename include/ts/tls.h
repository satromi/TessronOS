/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tls.h
 *	TLS over a connected socket, for a program (lib/libtls)
 *
 *	The client's side of TLS 1.2 and 1.3 on mbed TLS. The server's
 *	certificate is checked against the roots of ルート証明書
 *	(SYSDEF_CA_CERTS) and the name given, which also goes out as SNI.
 *	The socket stays the caller's: it is opened and connected before,
 *	and closed after ts_tls_close. A call that has to wait for the other
 *	side calls the caller's wait function between tries, so that a
 *	program that runs several things in one task keeps running them.
 */

#ifndef __TS_TLS_H__
#define __TS_TLS_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_tls TS_TLS;

/*
 * Waits a little (a program's own turn to the others), or answers
 * FALSE when the wait is to end: the program is stopping.
 */
typedef BOOL (*TS_TLS_WAIT)( void *arg );

/* Errors: an mbed TLS error (negative), or one of these */
#define TS_TLS_E_NOCA		(-0x7F01)	/* the roots could not be read */
#define TS_TLS_E_NOMEM		(-0x7F02)
#define TS_TLS_E_TIMEOUT	(-0x7F03)
#define TS_TLS_E_STOPPED	(-0x7F04)	/* the wait function said to end */
#define TS_TLS_E_VERIFY		(-0x7F05)	/* the certificate was not good: see ts_tls_verify */

/*
 * The handshake on socket s with the server named host. tmo_ms bounds
 * each wait for the other side. NULL on failure, the reason in *p_err.
 */
IMPORT TS_TLS *ts_tls_open( INT s, CONST char *host, INT tmo_ms, TS_TLS_WAIT wait, void *arg, INT *p_err );

/* Bytes sent (all of them, or an error), bytes received (0: closed), or an error */
IMPORT INT  ts_tls_write( TS_TLS *t, CONST void *buf, INT len );
IMPORT INT  ts_tls_read( TS_TLS *t, void *buf, INT len );
IMPORT void ts_tls_close( TS_TLS *t );

/* What the error says, in a few words */
IMPORT void ts_tls_strerror( INT err, char *out, INT max );

/* Why the last certificate was refused (mbed TLS's flags, 0: it was not) */
IMPORT UW   ts_tls_verify( void );

#ifdef __cplusplus
}
#endif
#endif /* __TS_TLS_H__ */
