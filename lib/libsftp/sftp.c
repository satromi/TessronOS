/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sftp.c
 *	SSH and SFTP for programs, on libssh2 (tools/ssh/build.sh). All bytes
 *	go through the caller's T_SSHNET, whose send and receive never wait:
 *	libssh2 reads to see what has come even while it is writing. Where it
 *	would wait it calls select, which here is the network's wait.
 *
 *	Connecting is in two steps, so that the caller can look at the
 *	server's key before anything secret is sent to it: ssh_connect ends
 *	with the key exchange done and the key's SHA-256 in hand; ssh_login
 *	then gives the name and the password.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ftp.h>
#include <ts/sftp.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/base64.h>

static struct ts_ssh	*ssh_cur;

struct ts_ssh {
	LIBSSH2_SESSION	*s;
	LIBSSH2_SFTP	*sftp;
	LIBSSH2_SFTP_HANDLE *h;
	LIBSSH2_CHANNEL	*ch;
	CONST T_SSHNET	*net;
	INT		sock;
	BOOL		eof;
};

/*
 * libssh2 reads key files through this; the Mbed TLS of the programs has
 * no files, and keys come from memory here.
 */
int mbedtls_pk_load_file( const char *path, unsigned char **buf, size_t *n )
{
	(void)path;
	(void)buf;
	(void)n;
	return -1;
}

/*
 * What libssh2 waits with. The session it waits for is the one that last
 * called the network; a program talks to its sessions from one task.
 */
int ts_ssh_select( int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t )
{
	INT	ms = ( t != NULL ) ? (INT)( t->tv_sec * 1000 + t->tv_usec / 1000 ) : 1000;

	(void)n;
	(void)e;
	if ( ssh_cur == NULL ) {
		return 1;
	}
	return ssh_cur->net->wait(ssh_cur->net->ctx, ssh_cur->sock, r != NULL, w != NULL, ms);
}

/*
 * The C library's time() comes here, and a program with no POSIX layer
 * has nothing to answer it. libssh2 uses the time for its time-outs.
 */

__attribute__((weak)) int _gettimeofday( struct timeval *t, void *z )
{
	(void)z;
	t->tv_sec = (time_t)ts_tls_time(NULL);
	t->tv_usec = 0;
	return 0;
}

/* libssh2 is made ready once */
static BOOL	lib_up = FALSE;

static ssize_t cb_send( libssh2_socket_t fd, const void *buf, size_t n, int flags, void **abstract )
{
	T_SSH	*c = *(T_SSH **)abstract;
	INT	r;

	ssh_cur = c;
	(void)fd;
	(void)flags;
	r = c->net->send(c->net->ctx, c->sock, buf, (SZ)n);
	if ( r == SSH_WOULDBLOCK ) return -EAGAIN;
	return ( r < 0 ) ? -ECONNRESET : (ssize_t)r;
}

static ssize_t cb_recv( libssh2_socket_t fd, void *buf, size_t n, int flags, void **abstract )
{
	T_SSH	*c = *(T_SSH **)abstract;
	INT	r;

	ssh_cur = c;
	(void)fd;
	(void)flags;
	r = c->net->recv(c->net->ctx, c->sock, buf, (SZ)n);
	if ( r == SSH_WOULDBLOCK ) return -EAGAIN;
	return ( r <= 0 ) ? -ECONNRESET : (ssize_t)r;
}

EXPORT void ssh_close( T_SSH *c )
{
	if ( c == NULL ) return;
	if ( c->h != NULL ) libssh2_sftp_close(c->h);
	if ( c->sftp != NULL ) libssh2_sftp_shutdown(c->sftp);
	if ( c->ch != NULL ) libssh2_channel_free(c->ch);
	if ( c->s != NULL ) {
		libssh2_session_disconnect(c->s, "closed");
		libssh2_session_free(c->s);
	}
	if ( c->sock >= 0 ) c->net->close(c->net->ctx, c->sock);
	free(c);
}

EXPORT ER ssh_connect( T_SSH **pc, CONST T_SSHNET *net, CONST char *host, UINT port, UB hash[SSH_HASH_BYTES] )
{
	T_SSH		*c;
	CONST char	*h;

	*pc = NULL;
	if ( !lib_up ) {
		if ( libssh2_init(0) != 0 ) return E_SYS;
		lib_up = TRUE;
	}
	c = (T_SSH *)calloc(1, sizeof(T_SSH));
	if ( c == NULL ) return E_NOMEM;
	c->net = net;
	c->sock = net->open(net->ctx, host, port);
	if ( c->sock < 0 ) {
		INT	e = c->sock;

		free(c);
		return ( e < 0 ) ? (ER)e : E_IO;
	}
	c->s = libssh2_session_init_ex(NULL, NULL, NULL, c);
	if ( c->s == NULL ) {
		ssh_close(c);
		return E_NOMEM;
	}
	libssh2_session_callback_set2(c->s, LIBSSH2_CALLBACK_SEND, (libssh2_cb_generic *)cb_send);
	libssh2_session_callback_set2(c->s, LIBSSH2_CALLBACK_RECV, (libssh2_cb_generic *)cb_recv);
	libssh2_session_set_blocking(c->s, 1);
	libssh2_session_set_timeout(c->s, SSH_WAIT_MS);
	if ( libssh2_session_handshake(c->s, c->sock) != 0 ) {
		ssh_close(c);
		return E_IO;
	}
	h = libssh2_hostkey_hash(c->s, LIBSSH2_HOSTKEY_HASH_SHA256);
	if ( h == NULL ) {
		ssh_close(c);
		return E_NOSPT;		/* no way to tell who the server is: not to be talked to */
	}
	memcpy(hash, h, SSH_HASH_BYTES);
	*pc = c;
	return E_OK;
}

/* The number of the connection, what T_FTPNET open gave */
EXPORT INT ssh_socket( T_SSH *c )
{
	return c->sock;
}

EXPORT INT ssh_last_errno( T_SSH *c )
{
	return libssh2_session_last_errno(c->s);
}

/* What the library said last, for a message */
EXPORT CONST char *ssh_last_error( T_SSH *c )
{
	char	*m = NULL;

	(void)libssh2_session_last_error(c->s, &m, NULL, 0);
	return ( m != NULL ) ? m : "";
}

EXPORT ER ssh_login( T_SSH *c, CONST char *user, CONST char *pass )
{
	return ( libssh2_userauth_password(c->s, user, pass) == 0 ) ? E_OK : E_OACV;
}

/* A length and a big number as the SSH wire has them, with a zero before a high bit */
static UB *wire_mpi( UB *p, CONST mbedtls_mpi *m )
{
	size_t	n = mbedtls_mpi_size(m);
	UB	*at = p + 4;
	BOOL	pad;

	(void)mbedtls_mpi_write_binary(m, at + 1, n);
	pad = ( at[1] & 0x80 ) != 0;
	if ( pad ) {
		at[0] = 0;
		n++;
	} else {
		memmove(at, at + 1, n);
	}
	p[0] = (UB)( n >> 24 );
	p[1] = (UB)( n >> 16 );
	p[2] = (UB)( n >> 8 );
	p[3] = (UB)n;
	return at + n;
}

/*
 * The public half is worked out here and handed to the library, which
 * would otherwise derive it itself: libssh2 1.11.1's Mbed TLS layer
 * returns an uninitialized status from that derivation and, in
 * gen_publickey_from_rsa, does not step past the exponent and the modulus
 * as it writes them, so that no request ever reaches the server.
 */
EXPORT INT ssh_key_public( CONST char *pem, SZ len, char *line, SZ max )
{
	mbedtls_pk_context	pk;
	mbedtls_mpi		n, e;
	static UB		blob[SSH_RSA_MAX_BYTES + 64];
	static UB		z[SSH_KEY_MAX + 1];
	UB			*p = blob;
	size_t			o = 0;
	INT			r = E_OBJ;

	if ( len <= 0 || (size_t)len > SSH_KEY_MAX || max < 16 ) return E_OBJ;
	memcpy(z, pem, (size_t)len);
	z[len] = 0;
	mbedtls_pk_init(&pk);
	mbedtls_mpi_init(&n);
	mbedtls_mpi_init(&e);
	if ( mbedtls_pk_parse_key(&pk, z, (size_t)len + 1, NULL, 0, NULL, NULL) == 0
	  && mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA
	  && mbedtls_rsa_export(mbedtls_pk_rsa(pk), &n, NULL, NULL, NULL, &e) == 0
	  && mbedtls_mpi_size(&n) <= SSH_RSA_MAX_BYTES ) {
		memcpy(p, "\0\0\0\7ssh-rsa", 11);
		p = wire_mpi(p + 11, &e);
		p = wire_mpi(p, &n);
		memcpy(line, "ssh-rsa ", 8);
		if ( mbedtls_base64_encode((UB *)line + 8, (size_t)max - 8, &o, blob, (size_t)( p - blob )) == 0 ) {
			r = (INT)( o + 8 );
		} else {
			r = E_OBJ;
		}
	}
	mbedtls_mpi_free(&e);
	mbedtls_mpi_free(&n);
	mbedtls_pk_free(&pk);
	memset(z, 0, sizeof(z));
	return r;
}

EXPORT ER ssh_login_key( T_SSH *c, CONST char *user, CONST char *pem, SZ len )
{
	static char	line[SSH_PUB_MAX];
	INT		n = ssh_key_public(pem, len, line, sizeof(line));

	if ( n < E_OK ) return n;
	return ( libssh2_userauth_publickey_frommemory(c->s, user, strlen(user), line, (size_t)n, pem, (size_t)len, NULL) == 0 )
		? E_OK : E_OACV;
}

/* ---------------------------------------------------------------- SFTP */

static ER sftp_err( T_SSH *c )
{
	switch ( libssh2_sftp_last_error(c->sftp) ) {
	case LIBSSH2_FX_NO_SUCH_FILE:		return E_NOEXS;
	case LIBSSH2_FX_PERMISSION_DENIED:	return E_OACV;
	case LIBSSH2_FX_FILE_ALREADY_EXISTS:	return E_OBJ;
	default:				return E_IO;
	}
}

EXPORT ER sftp_start( T_SSH *c )
{
	c->sftp = libssh2_sftp_init(c->s);
	return ( c->sftp != NULL ) ? E_OK : E_IO;
}

EXPORT ER sftp_list( T_SSH *c, CONST UB *path, FTP_ENTCB cb, void *arg )
{
	LIBSSH2_SFTP_HANDLE	*d;
	LIBSSH2_SFTP_ATTRIBUTES	a;
	T_FTPENT		*e;
	ER			er = E_OK;

	d = libssh2_sftp_opendir(c->sftp, ( path != NULL && path[0] != 0 ) ? (CONST char *)path : ".");
	if ( d == NULL ) return sftp_err(c);
	e = (T_FTPENT *)malloc(sizeof(T_FTPENT));
	if ( e == NULL ) {
		libssh2_sftp_closedir(d);
		return E_NOMEM;
	}
	for ( ;; ) {
		INT	n = libssh2_sftp_readdir(d, (char *)e->name, sizeof(e->name) - 1, &a);

		if ( n <= 0 ) {
			if ( n < 0 ) er = E_IO;
			break;
		}
		e->name[n] = 0;
		if ( strcmp((char *)e->name, ".") == 0 || strcmp((char *)e->name, "..") == 0 ) continue;
		e->dir = (BOOL)( ( a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS ) != 0 && LIBSSH2_SFTP_S_ISDIR(a.permissions) );
		e->size = ( a.flags & LIBSSH2_SFTP_ATTR_SIZE ) != 0 ? (D)a.filesize : -1;
		e->mtime = ( a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME ) != 0 ? (D)a.mtime - SFTP_EPOCH_DIFF : 0;
		if ( cb(arg, e) != 0 ) break;
	}
	free(e);
	libssh2_sftp_closedir(d);
	return er;
}

EXPORT ER sftp_get( T_SSH *c, CONST UB *path )
{
	if ( c->h != NULL ) return E_OBJ;
	c->h = libssh2_sftp_open(c->sftp, (CONST char *)path, LIBSSH2_FXF_READ, 0);
	return ( c->h != NULL ) ? E_OK : sftp_err(c);
}

EXPORT ER sftp_put( T_SSH *c, CONST UB *path )
{
	if ( c->h != NULL ) return E_OBJ;
	c->h = libssh2_sftp_open(c->sftp, (CONST char *)path,
				 LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
	return ( c->h != NULL ) ? E_OK : sftp_err(c);
}

EXPORT INT sftp_read( T_SSH *c, void *buf, SZ n )
{
	ssize_t	r;

	if ( c->h == NULL ) return E_ID;
	r = libssh2_sftp_read(c->h, (char *)buf, (size_t)n);
	return ( r < 0 ) ? E_IO : (INT)r;
}

EXPORT INT sftp_write( T_SSH *c, CONST void *buf, SZ n )
{
	SZ	done = 0;

	if ( c->h == NULL ) return E_ID;
	while ( done < n ) {
		ssize_t	r = libssh2_sftp_write(c->h, (CONST char *)buf + done, (size_t)( n - done ));

		if ( r <= 0 ) return E_IO;
		done += r;
	}
	return (INT)done;
}

EXPORT ER sftp_end( T_SSH *c )
{
	INT	r;

	if ( c->h == NULL ) return E_OK;
	r = libssh2_sftp_close(c->h);
	c->h = NULL;
	return ( r == 0 ) ? E_OK : E_IO;
}

EXPORT ER sftp_mkd( T_SSH *c, CONST UB *path )
{
	return ( libssh2_sftp_mkdir(c->sftp, (CONST char *)path, 0755) == 0 ) ? E_OK : sftp_err(c);
}

EXPORT ER sftp_mfmt( T_SSH *c, CONST UB *path, D mtime )
{
	LIBSSH2_SFTP_ATTRIBUTES	a;

	memset(&a, 0, sizeof(a));
	a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
	a.mtime = a.atime = (unsigned long)( mtime + SFTP_EPOCH_DIFF );
	return ( libssh2_sftp_setstat(c->sftp, (CONST char *)path, &a) == 0 ) ? E_OK : sftp_err(c);
}

/* ---------------------------------------------------------------- a terminal on the server */

EXPORT ER ssh_shell( T_SSH *c, CONST char *term, INT cols, INT rows )
{
	c->ch = libssh2_channel_open_session(c->s);
	if ( c->ch == NULL ) return E_IO;
	if ( libssh2_channel_request_pty_ex(c->ch, term, (unsigned int)strlen(term), NULL, 0, cols, rows, 0, 0) != 0
	  || libssh2_channel_shell(c->ch) != 0 ) {
		return E_IO;
	}
	return E_OK;
}

EXPORT ER ssh_exec( T_SSH *c, CONST char *cmd )
{
	c->ch = libssh2_channel_open_session(c->s);
	if ( c->ch == NULL ) return E_IO;
	return ( libssh2_channel_exec(c->ch, cmd) == 0 ) ? E_OK : E_IO;
}

EXPORT ER ssh_resize( T_SSH *c, INT cols, INT rows )
{
	return ( libssh2_channel_request_pty_size(c->ch, cols, rows) == 0 ) ? E_OK : E_IO;
}

/*
 * From here on the channel does not wait: the network's calls answer
 * SSH_WOULDBLOCK, and the program waits on its socket and its terminal
 * together. Reading answers 0 when nothing has come, -1 at the end.
 */
EXPORT void ssh_nonblock( T_SSH *c )
{
	libssh2_session_set_blocking(c->s, 0);
}

EXPORT INT ssh_chan_read( T_SSH *c, void *buf, SZ n )
{
	ssize_t	r = libssh2_channel_read(c->ch, (char *)buf, (size_t)n);

	if ( r == LIBSSH2_ERROR_EAGAIN ) return 0;
	if ( r == 0 && libssh2_channel_eof(c->ch) ) return -1;
	return ( r < 0 ) ? -1 : (INT)r;
}

/* What the channel took of it, 0 when it can take nothing now */
EXPORT INT ssh_chan_write( T_SSH *c, CONST void *buf, SZ n )
{
	ssize_t	r = libssh2_channel_write(c->ch, (CONST char *)buf, (size_t)n);

	if ( r == LIBSSH2_ERROR_EAGAIN ) return 0;
	return ( r < 0 ) ? -1 : (INT)r;
}
