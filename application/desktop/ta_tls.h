/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ta_tls.h
 *	The TLS of the test agent (design 12.4.1), in plain C types: the
 *	mbed TLS it is made of is kept apart from the kernel's headers and,
 *	at link time, from every other copy of mbed TLS in the kernel.
 *
 *	ta_tls_setup	the key (32 to 64 bytes), once
 *	ta_tls_open	the handshake over the two functions given: a
 *			connection, or NULL with the error in *p_err
 *	ta_tls_read	bytes (more than 0), 0 when the other side closed, or
 *			an error (less than 0)
 *	ta_tls_write	all of it: 0, or an error
 *	ta_tls_close	the connection closed and given up
 */

#ifndef TA_TLS_H
#define TA_TLS_H

#include <stddef.h>

typedef int (*ta_io_send)( void *ctx, const unsigned char *buf, size_t n );
typedef int (*ta_io_recv)( void *ctx, unsigned char *buf, size_t n );

int  ta_tls_setup( const unsigned char *psk, int len );
void *ta_tls_open( ta_io_send snd, ta_io_recv rcv, void *ctx, int *p_err );
int  ta_tls_read( void *conn, void *buf, int max );
int  ta_tls_write( void *conn, const void *buf, size_t n );
void ta_tls_close( void *conn );

#endif /* TA_TLS_H */
