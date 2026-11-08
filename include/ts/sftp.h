/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sftp.h
 *	SSH and SFTP for programs (lib/libsftp), built with make SSH=1.
 *
 *	ssh_connect ends with the key exchange done and the SHA-256 of the
 *	server's key in hand; the caller decides whether it trusts that key
 *	before ssh_login sends anything secret.
 */

#ifndef __TS_SFTP_H__
#define __TS_SFTP_H__

#include <ts/ftp.h>

#define SSH_HASH_BYTES		32
#define SSH_KEY_MAX		8192		/* the longest private key, as text */
#define SSH_RSA_MAX_BYTES	1024		/* the largest modulus (8192 bits) */
#define SSH_PUB_MAX		1500		/* the longest public line, with room for a comment */
#define SSH_WOULDBLOCK		(-11111)	/* T_SSHNET send, recv: not now */
#define SSH_WAIT_MS		30000		/* a call that hears nothing from the server gives up */
#define SFTP_EPOCH_DIFF		473385600	/* seconds from 1970 to 1985 (TS_TIME) */

typedef struct ts_ssh	T_SSH;

/*
 * The connection underneath. send and recv do not wait: they answer
 * SSH_WOULDBLOCK when nothing can go or nothing has come, 0 from recv
 * when the other side closed, below 0 for a fault. wait waits at most ms
 * for the connection to become readable or writable, whichever is asked.
 */
typedef struct {
	void	*ctx;
	INT	(*open)( void *ctx, CONST char *host, UINT port );
	INT	(*send)( void *ctx, INT c, CONST void *buf, SZ n );
	INT	(*recv)( void *ctx, INT c, void *buf, SZ n );
	void	(*close)( void *ctx, INT c );
	INT	(*wait)( void *ctx, INT c, BOOL rd, BOOL wr, INT ms );
} T_SSHNET;

/* The system's sockets (lib/libsftp/sftp_so.c) */
IMPORT CONST T_SSHNET	ssh_net_so;

IMPORT ER   ssh_connect( T_SSH **pc, CONST T_SSHNET *net, CONST char *host, UINT port, UB hash[SSH_HASH_BYTES] );
IMPORT ER   ssh_login( T_SSH *c, CONST char *user, CONST char *pass );
/*
 * By a private key in memory (PEM or PKCS#8, RSA, not encrypted). E_OACV when it
 * is not let in, E_OBJ when the key is not one of these.
 */
IMPORT ER   ssh_login_key( T_SSH *c, CONST char *user, CONST char *pem, SZ len );
/*
 * The person's key, kept in two objects of their own (lib/libsftp/sftp_key.c,
 * docs/tessronos/09-network.md 9.7.2). load gives its length, E_NOEXS when
 * there is none, E_OACV when its objects have been opened to others.
 */
IMPORT ER   ssh_key_store( CONST char *pem, SZ len );
IMPORT INT  ssh_key_load( char *pem, SZ max );
/* A new RSA key of the bits, as PEM: its length, or an error */
IMPORT INT  ssh_key_generate( INT bits, char *pem, SZ max );
/* The public line of such a key ("ssh-rsa AAAA...", no comment) into line: its length, or an error */
IMPORT INT  ssh_key_public( CONST char *pem, SZ len, char *line, SZ max );
IMPORT void ssh_close( T_SSH *c );
IMPORT INT  ssh_socket( T_SSH *c );
IMPORT INT  ssh_last_errno( T_SSH *c );
IMPORT CONST char *ssh_last_error( T_SSH *c );

/* SFTP: as the calls of FTP of the same names (ts/ftp.h) */
IMPORT ER   sftp_start( T_SSH *c );
IMPORT ER   sftp_list( T_SSH *c, CONST UB *path, FTP_ENTCB cb, void *arg );
IMPORT ER   sftp_get( T_SSH *c, CONST UB *path );
IMPORT ER   sftp_put( T_SSH *c, CONST UB *path );
IMPORT INT  sftp_read( T_SSH *c, void *buf, SZ n );
IMPORT INT  sftp_write( T_SSH *c, CONST void *buf, SZ n );
IMPORT ER   sftp_end( T_SSH *c );
IMPORT ER   sftp_mkd( T_SSH *c, CONST UB *path );
IMPORT ER   sftp_mfmt( T_SSH *c, CONST UB *path, D mtime );

/* A terminal on the server, or one command */
IMPORT ER   ssh_shell( T_SSH *c, CONST char *term, INT cols, INT rows );
IMPORT ER   ssh_exec( T_SSH *c, CONST char *cmd );
IMPORT ER   ssh_resize( T_SSH *c, INT cols, INT rows );
IMPORT void ssh_nonblock( T_SSH *c );
IMPORT INT  ssh_chan_read( T_SSH *c, void *buf, SZ n );
IMPORT INT  ssh_chan_write( T_SSH *c, CONST void *buf, SZ n );

#endif /* __TS_SFTP_H__ */
