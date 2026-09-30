/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ftp.h
 *	A client of FTP over connections the caller gives (design 18.20)
 *
 *	The client speaks the protocol and nothing else: the connections
 *	are made, written, read and closed through the calls of a T_FTPNET,
 *	so the same engine runs over the sockets of a process, over the
 *	kernel's, or over a script in a test. How long a read may wait is
 *	the connection's business too; a read that waited too long answers
 *	E_TMOUT and the call that made it gives up with that.
 *
 *	Data goes the passive way unless the caller asks for the active
 *	one. Passive: EPSV, and PASV when the server refuses EPSV. Whatever
 *	address the reply names, the data connection goes to the host the
 *	control connection reached, only the port being taken from the
 *	reply. Active (T_FTP.active): the client listens, names where with
 *	EPRT, and PORT when the server refuses EPRT, and waits a while for
 *	the server to connect after the command that moves the data.
 *
 *	Names and paths are UTF-8 on this side. On the server they are
 *	UTF-8 when it says it knows UTF8 (FEAT, OPTS UTF8 ON), and otherwise
 *	in the encoding the caller names -- Shift_JIS is common on old
 *	servers -- turned both ways by include/ts/txc.h.
 *
 *	A file is read or written as a stream: ftp_get opens it, ftp_read
 *	or ftp_write carry the bytes, ftp_end finishes (the server's final
 *	reply). Only one transfer is under way at a time.
 */

#ifndef __TS_FTP_H__
#define __TS_FTP_H__

#ifdef __cplusplus
extern "C" {
#endif

#define FTP_PORT	21
#define FTP_LINE_MAX	512		/* bytes of a reply's line kept */
#define FTP_NAME_MAX	768		/* bytes of a name, UTF-8, with its nought */
#define FTP_PATH_MAX	1024		/* bytes of a path, UTF-8, with its nought */

/*
 * The connections. open makes one to host:port, or, with peer 0 or
 * more, to port on the host that connection 'peer' is connected to,
 * host then being NULL; it answers a number for it (0 or more). send
 * answers the bytes it took; recv the bytes it gave, 0 when the other
 * side closed. Below 0 is a fault (E_TMOUT for a wait too long).
 */
typedef struct {
	void	*ctx;
	INT	(*open)( void *ctx, CONST char *host, UINT port, INT peer );
	INT	(*send)( void *ctx, INT c, CONST void *buf, SZ n );
	INT	(*recv)( void *ctx, INT c, void *buf, SZ n );
	void	(*close)( void *ctx, INT c );

	/*
	 * The active way; NULL when these connections cannot listen. listen
	 * makes a listening end on the address connection 'peer' goes out
	 * from and answers a number for it, with that address (addr[0] the
	 * first of a.b.c.d) and the port it listens on. accept waits at
	 * most tmo ms for a connection to it and answers a number for that
	 * one (E_TMOUT when none came). close closes a listening end too.
	 */
	INT	(*listen)( void *ctx, INT peer, UB addr[4], UINT *p_port );
	INT	(*accept)( void *ctx, INT l, INT tmo );
} T_FTPNET;

#define FTP_ACCEPT_TMO	20000		/* ms the active way waits for the server */

/* What the server said it can do (T_FTP.feat) */
#define FTP_F_UTF8	0x0001
#define FTP_F_MLSD	0x0002		/* MLST in FEAT: MLSD lists */
#define FTP_F_SIZE	0x0004
#define FTP_F_MDTM	0x0008
#define FTP_F_MFMT	0x0010		/* the time of a file can be set */
#define FTP_F_EPSV	0x0020

/* One name of a listing */
typedef struct {
	UB	name[FTP_NAME_MAX];	/* UTF-8 */
	BOOL	dir;
	D	size;			/* -1 when not said */
	D	mtime;			/* TS_TIME, UTC; 0 when not said */
} T_FTPENT;

/* One session. The caller keeps it; its fields are the client's */
typedef struct {
	CONST T_FTPNET	*net;
	INT		ctl;		/* the control connection, -1 when none */
	INT		data;		/* the data connection of a transfer, -1 when none */
	UINT		feat;		/* FTP_F_* */
	BOOL		pasv;		/* EPSV was refused: PASV from then on */
	BOOL		active;		/* the active way (the caller sets it after ftp_open) */
	BOOL		port;		/* EPRT was refused: PORT from then on */
	INT		lsn;		/* the listening end of the active way, -1 when none */
	INT		atmo;		/* ms accept waits; 0 FTP_ACCEPT_TMO */
	INT		enc;		/* names on the server: TXC_UTF8, or the caller's */
	INT		code;		/* the last reply's code */
	UB		text[FTP_LINE_MAX];	/* its last line */
	UB		syst[64];	/* what SYST said */
	D		now;		/* the time now (TS_TIME), for a listing that
					   leaves out the year; 0 when not known */
	BOOL		done;		/* the transfer's last reply came already */
	INT		rat, rlen;	/* what was read of the control connection */
	UB		rbuf[512];
	UB		cmd[FTP_PATH_MAX * 2 + 16];	/* a command being sent */
	UB		line[FTP_PATH_MAX];	/* a line of a listing */
	T_FTPENT	ent;		/* the name it gives */
} T_FTP;

/* Called for each name of a listing; below 0 stops it */
typedef INT (*FTP_ENTCB)( void *arg, CONST T_FTPENT *e );

/*
 * A session: connected, logged in (user NULL: anonymous), told what the
 * server can do, UTF-8 asked for, the binary type set. enc is how names
 * are when the server does not know UTF-8 (TXC_SJIS, or TXC_UTF8).
 */
IMPORT ER  ftp_open( T_FTP *f, CONST T_FTPNET *net, CONST char *host, UINT port,
		     CONST UB *user, CONST UB *pass, INT enc );

/* QUIT, and the connections closed */
IMPORT ER  ftp_close( T_FTP *f );

/*
 * A command and its reply: answers the reply's code, or below 0 for a
 * fault of the connection. arg (UTF-8, may be NULL) follows the command
 * after a blank.
 */
IMPORT INT ftp_cmd( T_FTP *f, CONST char *cmd, CONST UB *arg );

IMPORT ER  ftp_noop( T_FTP *f );
IMPORT ER  ftp_pwd( T_FTP *f, UB *path, INT max );
IMPORT ER  ftp_cwd( T_FTP *f, CONST UB *path );
IMPORT ER  ftp_cdup( T_FTP *f );
IMPORT ER  ftp_mkd( T_FTP *f, CONST UB *path );
IMPORT ER  ftp_rmd( T_FTP *f, CONST UB *path );
IMPORT ER  ftp_dele( T_FTP *f, CONST UB *path );
IMPORT ER  ftp_rename( T_FTP *f, CONST UB *from, CONST UB *to );
IMPORT ER  ftp_size( T_FTP *f, CONST UB *path, D *p_size );
IMPORT ER  ftp_mdtm( T_FTP *f, CONST UB *path, D *p_time );
IMPORT ER  ftp_mfmt( T_FTP *f, CONST UB *path, D time );

/*
 * The names in a directory (path NULL: the current one): MLSD when the
 * server offers it, LIST otherwise, the listing of a Unix or a DOS
 * server read. "." and ".." are left out. ftp_nlst gives the names
 * alone (NLST), dir and size unknown.
 */
IMPORT ER  ftp_list( T_FTP *f, CONST UB *path, FTP_ENTCB cb, void *arg );
IMPORT ER  ftp_nlst( T_FTP *f, CONST UB *path, FTP_ENTCB cb, void *arg );

/*
 * A transfer: ftp_get (RETR) or ftp_put (STOR) opens it, ftp_read and
 * ftp_write carry the bytes (ftp_read answers 0 at the end), ftp_end
 * closes the data connection and takes the server's last word on it.
 */
IMPORT ER  ftp_get( T_FTP *f, CONST UB *path );
IMPORT ER  ftp_put( T_FTP *f, CONST UB *path );
IMPORT INT ftp_read( T_FTP *f, void *buf, SZ n );
IMPORT INT ftp_write( T_FTP *f, CONST void *buf, SZ n );
IMPORT ER  ftp_end( T_FTP *f );

/* A line of a listing read: TRUE when it names something */
IMPORT BOOL ftp_parse_list( T_FTP *f, CONST UB *line, T_FTPENT *e );
IMPORT BOOL ftp_parse_mlsd( T_FTP *f, CONST UB *line, T_FTPENT *e );

/* The error a reply's code stands for (E_OK for 1xx to 3xx) */
IMPORT ER  ftp_error( INT code );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FTP_H__ */
