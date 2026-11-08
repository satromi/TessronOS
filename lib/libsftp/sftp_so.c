/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sftp_so.c
 *	The connection of SSH (ts/sftp.h) on the system's sockets.
 */

#include <string.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/fs.h>
#include <ts/soapp.h>
#include <ts/sftp.h>

#define WOULD_BLOCK(r)	( (r) == EX_AGAIN || (r) == E_TMOUT )

static INT n_open( void *ctx, CONST char *host, UINT port )
{
	struct sockaddr_in	sa;
	UINT			addr;
	INT			s;
	ER			er;

	(void)ctx;
	er = so_resolve(host, &addr);
	if ( er < E_OK ) return er;
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return s;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons((UH)port);
	sa.sin_addr.s_addr = addr;
	er = so_connect(s, (CONST struct sockaddr *)&sa, sizeof(sa));
	if ( er < E_OK ) {
		(void)so_close(s);
		return er;
	}
	return s;
}

static INT n_send( void *ctx, INT c, CONST void *b, SZ n )
{
	INT	r = so_send(c, b, n, MSG_DONTWAIT);

	(void)ctx;
	return WOULD_BLOCK(r) ? SSH_WOULDBLOCK : r;
}

static INT n_recv( void *ctx, INT c, void *b, SZ n )
{
	INT	r = so_recv(c, b, n, MSG_DONTWAIT);

	(void)ctx;
	return WOULD_BLOCK(r) ? SSH_WOULDBLOCK : r;
}

static void n_close( void *ctx, INT c )
{
	(void)ctx;
	(void)so_close(c);
}

static INT n_wait( void *ctx, INT c, BOOL rd, BOOL wr, INT ms )
{
	struct pollfd	p;

	(void)ctx;
	p.fd = c;
	p.events = (short)( ( rd ? POLLIN : 0 ) | ( wr ? POLLOUT : 0 ) );
	p.revents = 0;
	return so_poll(&p, 1, ms);
}

EXPORT CONST T_SSHNET	ssh_net_so = { NULL, n_open, n_send, n_recv, n_close, n_wait };
