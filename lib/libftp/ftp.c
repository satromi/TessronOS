/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ftp.c
 *	A client of FTP over connections the caller gives (design 18.20)
 *
 *	A reply is one line, "234 text", or several: "234-" first, then any
 *	lines, then "234 " again with the same code. What a reply says is
 *	its code; the text is kept for whoever wants to show it. A command
 *	is sent as one line, its argument turned into the server's encoding
 *	and refused when it holds a line end, which would be a second
 *	command.
 *
 *	The listing of a directory comes on a data connection as lines: the
 *	facts of MLSD, which say plainly what each name is, or the text of
 *	LIST, which is what "ls -l" or a DOS "dir" prints and is read the
 *	way those are laid out.
 *
 *	A data connection is made before the command that moves the data.
 *	The passive way connects to where EPSV or PASV said; the active way
 *	listens where EPRT or PORT said and takes the server's connection
 *	once the command's first reply has come, since a server may connect
 *	before or after sending it.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/txc.h>
#include <ts/ftp.h>

#define ANON_USER	"anonymous"
#define ANON_PASS	"anonymous@"

/* ---------------------------------------------------------------- small things */

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != 0 ) n++;
	return n;
}

LOCAL INT s_cpy( UB *d, INT max, CONST UB *s )
{
	INT	n = 0;

	if ( max <= 0 ) {
		return 0;
	}
	while ( s != NULL && s[n] != 0 && n < max - 1 ) {
		d[n] = s[n];
		n++;
	}
	d[n] = 0;
	return n;
}

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Whether a line starts with a word, case ignored */
LOCAL BOOL starts( CONST UB *s, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( lower(s[i]) != lower((UB)w[i]) ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL is_digit( UB c )
{
	return (BOOL)( c >= '0' && c <= '9' );
}

LOCAL BOOL is_blank( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' );
}

/* A number of digits at s, how many digits in *p_n; -1 when there are none */
LOCAL D number( CONST UB *s, INT *p_n )
{
	D	v = 0;
	INT	n = 0;

	while ( is_digit(s[n]) ) {
		v = v * 10 + ( s[n] - '0' );
		n++;
	}
	if ( p_n != NULL ) *p_n = n;
	return ( n > 0 ) ? v : -1;
}

/* ---------------------------------------------------------------- time */

/* Days from 1970-01-01 to a date of the Gregorian calendar */
LOCAL D days_of( D y, INT m, INT d )
{
	D	era, yoe, doy, doe;

	y -= ( m <= 2 ) ? 1 : 0;
	era = ( ( y >= 0 ) ? y : y - 399 ) / 400;
	yoe = y - era * 400;
	doy = ( 153 * ( m + ( ( m > 2 ) ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

#define DAYS_1985	5479		/* days_of(1985, 1, 1) */

/* A time of the day on a date, as TS_TIME */
LOCAL D ts_time( D y, INT mo, INT d, INT h, INT mi, INT s )
{
	if ( mo < 1 || mo > 12 || d < 1 || d > 31 ) {
		return 0;
	}
	return ( days_of(y, mo, d) - DAYS_1985 ) * 86400 + h * 3600 + mi * 60 + s;
}

/* The year a TS_TIME falls in */
LOCAL D year_of( D t )
{
	D	z = t / 86400 + DAYS_1985 + 719468, era, doe, yoe, y, doy, mp;

	era = ( ( z >= 0 ) ? z : z - 146096 ) / 146097;
	doe = z - era * 146097;
	yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
	y = yoe + era * 400;
	doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
	mp = ( 5 * doy + 2 ) / 153;
	return ( mp >= 10 ) ? y + 1 : y;
}

/* YYYYMMDDHHMMSS, as MDTM and MLSD give it (UTC); 0 when it is not that */
LOCAL D stamp( CONST UB *s )
{
	INT	i, v[6], w[6] = { 4, 2, 2, 2, 2, 2 }, k, at = 0;

	for ( i = 0; i < 6; i++ ) {
		v[i] = 0;
		for ( k = 0; k < w[i]; k++, at++ ) {
			if ( !is_digit(s[at]) ) return 0;
			v[i] = v[i] * 10 + ( s[at] - '0' );
		}
	}
	return ts_time(v[0], v[1], v[2], v[3], v[4], v[5]);
}

/* ---------------------------------------------------------------- the control connection */

EXPORT ER ftp_error( INT code )
{
	if ( code < 0 ) {
		return (ER)code;
	}
	if ( code >= 100 && code < 400 ) {
		return E_OK;
	}
	switch ( code ) {
	case 530: case 532: case 331: case 332:
		return E_OACV;
	case 450: case 550:
		return E_NOEXS;
	case 452: case 552:
		return E_LIMIT;
	case 501: case 553:
		return E_PAR;
	case 500: case 502: case 504:
		return E_NOSPT;
	default:
		return E_IO;
	}
}

LOCAL ER send_all( T_FTP *f, INT c, CONST UB *b, INT n )
{
	INT	k;

	while ( n > 0 ) {
		k = f->net->send(f->net->ctx, c, b, n);
		if ( k <= 0 ) {
			return ( k < 0 ) ? (ER)k : E_IO;
		}
		b += k;
		n -= k;
	}
	return E_OK;
}

/* A line of the control connection, its end taken off; its length */
LOCAL INT read_line( T_FTP *f, UB *line, INT max )
{
	INT	n = 0, got;
	UB	c;

	for (;;) {
		if ( f->rat >= f->rlen ) {
			got = f->net->recv(f->net->ctx, f->ctl, f->rbuf, sizeof(f->rbuf));
			if ( got <= 0 ) {
				return ( got < 0 ) ? got : E_IO;
			}
			f->rat = 0;
			f->rlen = got;
		}
		c = f->rbuf[f->rat++];
		if ( c == '\n' ) {
			break;
		}
		if ( c != '\r' && n < max - 1 ) {
			line[n++] = c;
		}
	}
	line[n] = 0;
	return n;
}

/* Called with each line of a reply */
typedef void (*LINECB)( T_FTP *f, CONST UB *line, void *arg );

/*
 * A reply, all its lines: its code. A line in the middle of one that
 * looks like its end but is not ("234-" again) is passed over.
 *
 * A reply starts with its code. Lines that come before any code are the
 * rest of an earlier reply the server ended too soon -- "211 Features:"
 * with the features and "211 End" after it, where "211-" was due -- and
 * the code line that closes them closes that reply, not this one; it is
 * passed over too, so that each command still gets its own reply. With
 * tail TRUE the caller knows such lines are coming and wants them as
 * the reply (their lines go to cb either way).
 */
LOCAL INT get_reply_x( T_FTP *f, LINECB cb, void *arg, BOOL tail )
{
	INT	n, code = -1;
	BOOL	stray = tail;

	for (;;) {
		n = read_line(f, f->text, sizeof(f->text));
		if ( n < 0 ) {
			return n;
		}
		if ( cb != NULL ) {
			cb(f, f->text, arg);
		}
		if ( n >= 3 && is_digit(f->text[0]) && is_digit(f->text[1]) && is_digit(f->text[2]) ) {
			INT	c = ( f->text[0] - '0' ) * 100 + ( f->text[1] - '0' ) * 10 + f->text[2] - '0';

			if ( code < 0 && stray ) {
				if ( f->text[3] != '-' ) {
					stray = FALSE;
					if ( tail ) {
						code = c;
						break;
					}
				}
			} else if ( code < 0 ) {
				code = c;
				if ( f->text[3] != '-' ) break;
			} else if ( c == code && f->text[3] != '-' ) {
				break;
			}
		} else if ( code < 0 && n > 0 ) {
			stray = TRUE;
		}
	}
	f->code = code;
	return code;
}

LOCAL INT get_reply( T_FTP *f, LINECB cb, void *arg )
{
	return get_reply_x(f, cb, arg, FALSE);
}

/* Whether a line that cannot start a reply has come already and waits to be read */
LOCAL BOOL tail_waits( T_FTP *f )
{
	return (BOOL)( f->rat < f->rlen && f->rbuf[f->rat] == ' ' );
}

/* A command line sent: "CMD arg", the argument in the server's encoding */
LOCAL ER send_cmd( T_FTP *f, CONST char *cmd, CONST UB *arg )
{
	INT	n = 0, i, max = (INT)sizeof(f->cmd) - 3;

	if ( f->ctl < 0 ) {
		return E_OBJ;
	}
	for ( i = 0; cmd[i] != 0 && n < max; i++ ) f->cmd[n++] = (UB)cmd[i];
	if ( arg != NULL ) {
		INT	len = s_len(arg);

		for ( i = 0; i < len; i++ ) {
			if ( arg[i] == '\r' || arg[i] == '\n' ) return E_PAR;
		}
		f->cmd[n++] = ' ';
		if ( f->enc != TXC_UTF8 && f->enc != TXC_AUTO ) {
			SZ	w = txc_from_utf8(f->enc, arg, len, f->cmd + n, max - n, NULL);

			if ( (INT)w >= max - n ) return E_LIMIT;
			n += (INT)w;
		} else {
			if ( len >= max - n ) return E_LIMIT;
			for ( i = 0; i < len; i++ ) f->cmd[n++] = arg[i];
		}
	}
	f->cmd[n++] = '\r';
	f->cmd[n++] = '\n';
	return send_all(f, f->ctl, f->cmd, n);
}

EXPORT INT ftp_cmd( T_FTP *f, CONST char *cmd, CONST UB *arg )
{
	ER	er;

	if ( f == NULL || cmd == NULL ) {
		return E_PAR;
	}
	er = send_cmd(f, cmd, arg);
	return ( er < E_OK ) ? (INT)er : get_reply(f, NULL, NULL);
}

/* A command that has to be answered with one code */
LOCAL ER cmd_want( T_FTP *f, CONST char *cmd, CONST UB *arg, INT want )
{
	INT	code = ftp_cmd(f, cmd, arg);

	if ( code < 0 ) {
		return (ER)code;
	}
	if ( code == want || ( want == 200 && code >= 200 && code < 300 ) ) {
		return E_OK;
	}
	return ( code < 400 ) ? E_IO : ftp_error(code);
}

/* ---------------------------------------------------------------- a session */

/* The lines of FEAT: one feature a line, after a blank */
LOCAL void feat_line( T_FTP *f, CONST UB *line, void *arg )
{
	(void)arg;
	if ( line[0] != ' ' ) {
		return;
	}
	line++;
	if ( starts(line, "UTF8") ) f->feat |= FTP_F_UTF8;
	else if ( starts(line, "MLST") ) f->feat |= FTP_F_MLSD;
	else if ( starts(line, "SIZE") ) f->feat |= FTP_F_SIZE;
	else if ( starts(line, "MDTM") ) f->feat |= FTP_F_MDTM;
	else if ( starts(line, "MFMT") ) f->feat |= FTP_F_MFMT;
	else if ( starts(line, "EPSV") ) f->feat |= FTP_F_EPSV;
}

EXPORT ER ftp_open( T_FTP *f, CONST T_FTPNET *net, CONST char *host, UINT port,
		    CONST UB *user, CONST UB *pass, INT enc )
{
	INT	code;
	ER	er;

	if ( f == NULL || net == NULL || host == NULL ) {
		return E_PAR;
	}
	f->net = net;
	f->data = -1;
	f->lsn = -1;
	f->feat = 0;
	f->pasv = FALSE;
	f->active = FALSE;
	f->port = FALSE;
	f->done = FALSE;
	f->enc = ( enc == TXC_AUTO ) ? TXC_UTF8 : enc;
	f->code = 0;
	f->text[0] = 0;
	f->syst[0] = 0;
	f->rat = f->rlen = 0;
	f->ctl = net->open(net->ctx, host, ( port != 0 ) ? port : FTP_PORT, -1);
	if ( f->ctl < 0 ) {
		return (ER)f->ctl;
	}

	/* the greeting, after any "wait": no reply came before it, so lines without a code are its own */
	do {
		code = get_reply_x(f, NULL, NULL, TRUE);
	} while ( code >= 100 && code < 200 );
	er = ( code == 220 ) ? E_OK : ( code < 0 ) ? (ER)code : ftp_error(code);

	if ( er >= E_OK ) {
		code = ftp_cmd(f, "USER", ( user != NULL && user[0] != 0 ) ? user : (CONST UB *)ANON_USER);
		if ( code == 331 ) {
			code = ftp_cmd(f, "PASS", ( pass != NULL ) ? pass : (CONST UB *)ANON_PASS);
		}
		er = ( code == 230 || code == 202 ) ? E_OK
		   : ( code < 0 ) ? (ER)code : ( code == 332 ) ? E_NOSPT : E_OACV;
	}
	if ( er >= E_OK ) {
		if ( ftp_cmd(f, "SYST", NULL) == 215 ) {
			(void)s_cpy(f->syst, sizeof(f->syst), f->text + 4);
		}
		er = send_cmd(f, "FEAT", NULL);
	}
	if ( er >= E_OK ) {
		code = get_reply(f, feat_line, NULL);
		if ( code == 211 && tail_waits(f) ) {
			/* "211 text" and the features after it: the list the server meant as "211-" */
			code = get_reply_x(f, feat_line, NULL, TRUE);
		}
		if ( code < 0 ) er = (ER)code;
		if ( code != 211 ) f->feat = 0;
	}
	if ( er >= E_OK && ( f->feat & FTP_F_UTF8 ) != 0 ) {
		/* some servers are UTF-8 always and refuse the option; either is UTF-8 */
		code = ftp_cmd(f, "OPTS", (CONST UB *)"UTF8 ON");
		if ( code < 0 ) er = (ER)code;
		f->enc = TXC_UTF8;
	}
	if ( er >= E_OK ) {
		er = cmd_want(f, "TYPE", (CONST UB *)"I", 200);
	}
	if ( er < E_OK ) {
		net->close(net->ctx, f->ctl);
		f->ctl = -1;
	}
	return er;
}

EXPORT ER ftp_close( T_FTP *f )
{
	ER	er = E_OK;

	if ( f == NULL || f->ctl < 0 ) {
		return E_PAR;
	}
	if ( f->data >= 0 ) {
		f->net->close(f->net->ctx, f->data);
		f->data = -1;
	}
	if ( f->lsn >= 0 ) {
		f->net->close(f->net->ctx, f->lsn);
		f->lsn = -1;
	}
	if ( ftp_cmd(f, "QUIT", NULL) < 0 ) {
		er = E_IO;
	}
	f->net->close(f->net->ctx, f->ctl);
	f->ctl = -1;
	return er;
}

EXPORT ER ftp_noop( T_FTP *f )
{
	return cmd_want(f, "NOOP", NULL, 200);
}

EXPORT ER ftp_cwd( T_FTP *f, CONST UB *path )
{
	return cmd_want(f, "CWD", path, 250);
}

EXPORT ER ftp_cdup( T_FTP *f )
{
	return cmd_want(f, "CDUP", NULL, 200);
}

EXPORT ER ftp_mkd( T_FTP *f, CONST UB *path )
{
	return cmd_want(f, "MKD", path, 257);
}

EXPORT ER ftp_rmd( T_FTP *f, CONST UB *path )
{
	return cmd_want(f, "RMD", path, 250);
}

EXPORT ER ftp_dele( T_FTP *f, CONST UB *path )
{
	return cmd_want(f, "DELE", path, 250);
}

EXPORT ER ftp_rename( T_FTP *f, CONST UB *from, CONST UB *to )
{
	INT	code = ftp_cmd(f, "RNFR", from);

	if ( code != 350 ) {
		return ( code < 0 ) ? (ER)code : ( code < 400 ) ? E_IO : ftp_error(code);
	}
	return cmd_want(f, "RNTO", to, 250);
}

/* A name the server gave turned into UTF-8 */
LOCAL void from_server( T_FTP *f, CONST UB *s, INT n, UB *out, INT max )
{
	if ( f->enc == TXC_UTF8 || f->enc == TXC_AUTO ) {
		INT	i;

		for ( i = 0; i < n && i < max - 1; i++ ) out[i] = s[i];
		out[i] = 0;
	} else {
		(void)txc_to_utf8(f->enc, s, n, out, max, NULL);
	}
}

EXPORT ER ftp_pwd( T_FTP *f, UB *path, INT max )
{
	INT	code, i, n = 0;
	UB	*t;

	code = ftp_cmd(f, "PWD", NULL);
	if ( code != 257 ) {
		return ( code < 0 ) ? (ER)code : ftp_error(code);
	}
	/* 257 "the path" with a quote inside doubled */
	for ( t = f->text + 4; *t != 0 && *t != '"'; t++ ) ;
	if ( *t == 0 ) {
		return E_IO;
	}
	for ( i = 1; t[i] != 0; i++ ) {
		if ( t[i] == '"' ) {
			if ( t[i + 1] != '"' ) break;
			i++;
		}
		f->cmd[n++] = t[i];
	}
	from_server(f, f->cmd, n, path, max);
	return E_OK;
}

EXPORT ER ftp_size( T_FTP *f, CONST UB *path, D *p_size )
{
	INT	code = ftp_cmd(f, "SIZE", path), k;
	D	v;

	if ( code != 213 ) {
		return ( code < 0 ) ? (ER)code : ( code < 400 ) ? E_IO : ftp_error(code);
	}
	v = number(f->text + 4, &k);
	if ( v < 0 ) {
		return E_IO;
	}
	if ( p_size != NULL ) *p_size = v;
	return E_OK;
}

EXPORT ER ftp_mdtm( T_FTP *f, CONST UB *path, D *p_time )
{
	INT	code = ftp_cmd(f, "MDTM", path);
	D	t;

	if ( code != 213 ) {
		return ( code < 0 ) ? (ER)code : ( code < 400 ) ? E_IO : ftp_error(code);
	}
	t = stamp(f->text + 4);
	if ( t == 0 ) {
		return E_IO;
	}
	if ( p_time != NULL ) *p_time = t;
	return E_OK;
}

/* A TS_TIME as YYYYMMDDHHMMSS into s (15 bytes) */
LOCAL void put_stamp( UB *s, D t )
{
	D	z = t / 86400 + DAYS_1985 + 719468, era, doe, yoe, y, doy, mp, d, m;
	D	sec = t % 86400, v[6];
	INT	i, k, w[6] = { 4, 2, 2, 2, 2, 2 }, at = 0;

	era = ( ( z >= 0 ) ? z : z - 146096 ) / 146097;
	doe = z - era * 146097;
	yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
	y = yoe + era * 400;
	doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
	mp = ( 5 * doy + 2 ) / 153;
	d = doy - ( 153 * mp + 2 ) / 5 + 1;
	m = ( mp < 10 ) ? mp + 3 : mp - 9;
	v[0] = ( m <= 2 ) ? y + 1 : y;
	v[1] = m;
	v[2] = d;
	v[3] = sec / 3600;
	v[4] = ( sec / 60 ) % 60;
	v[5] = sec % 60;
	for ( i = 0; i < 6; i++ ) {
		for ( k = w[i] - 1; k >= 0; k-- ) {
			s[at + k] = (UB)( '0' + v[i] % 10 );
			v[i] /= 10;
		}
		at += w[i];
	}
	s[at] = 0;
}

EXPORT ER ftp_mfmt( T_FTP *f, CONST UB *path, D time )
{
	UB	arg[FTP_PATH_MAX + 16];
	INT	n;

	if ( f == NULL || path == NULL ) {
		return E_PAR;
	}
	if ( ( f->feat & FTP_F_MFMT ) == 0 ) {
		return E_NOSPT;
	}
	put_stamp(arg, time);
	arg[14] = ' ';
	n = s_cpy(arg + 15, sizeof(arg) - 15, path);
	if ( n != s_len(path) ) {
		return E_LIMIT;
	}
	return cmd_want(f, "MFMT", arg, 213);
}

/* ---------------------------------------------------------------- data */

/* The port a 229 reply names: "(|||port|)" */
LOCAL INT epsv_port( CONST UB *t )
{
	INT	i, k;
	D	v;

	for ( i = 0; t[i] != 0 && t[i] != '('; i++ ) ;
	if ( t[i] == 0 || t[i + 1] == 0 || t[i + 2] != t[i + 1] || t[i + 3] != t[i + 1] ) {
		return -1;
	}
	v = number(t + i + 4, &k);
	return ( v > 0 && v < 65536 && t[i + 4 + k] == t[i + 1] ) ? (INT)v : -1;
}

/* The port a 227 reply names: the last two of six numbers h1,h2,h3,h4,p1,p2 */
LOCAL INT pasv_port( CONST UB *t )
{
	D	v[6];
	INT	i, k, n;

	for ( i = 4; t[i] != 0 && !is_digit(t[i]); i++ ) ;
	for ( n = 0; n < 6; n++ ) {
		v[n] = number(t + i, &k);
		if ( v[n] < 0 || v[n] > 255 ) return -1;
		i += k;
		if ( n < 5 ) {
			if ( t[i] != ',' ) return -1;
			i++;
		}
	}
	return (INT)( v[4] * 256 + v[5] );
}

/*
 * A data connection the passive way, to the host of the control
 * connection on the port the reply names. The address in a 227 reply is
 * not used: a server behind NAT or listening on a loopback names one
 * the client cannot reach, and a data connection to a third machine is
 * not wanted anyway. *p_ctl is set when the control connection itself
 * failed, the session then being of no further use.
 */
LOCAL ER pasv_open( T_FTP *f, BOOL epsv, BOOL *p_ctl )
{
	INT	code, port;

	code = ftp_cmd(f, epsv ? "EPSV" : "PASV", NULL);
	if ( code < 0 ) {
		*p_ctl = TRUE;
		return (ER)code;
	}
	if ( code != ( epsv ? 229 : 227 ) ) {
		return ( code < 400 ) ? E_IO : ftp_error(code);
	}
	port = epsv ? epsv_port(f->text + 4) : pasv_port(f->text);
	if ( port <= 0 ) {
		return E_IO;
	}
	f->data = f->net->open(f->net->ctx, NULL, (UINT)port, f->ctl);
	if ( f->data < 0 ) {
		ER	er = (ER)f->data;

		f->data = -1;
		return er;
	}
	return E_OK;
}

/*
 * A data connection opened the passive way: EPSV, and PASV once EPSV
 * was refused or what it named could not be reached. f->pasv keeps that
 * PASV is the one to use.
 */
LOCAL ER data_open( T_FTP *f, BOOL *p_ctl )
{
	ER	er;

	if ( !f->pasv ) {
		er = pasv_open(f, TRUE, p_ctl);
		if ( er >= E_OK || *p_ctl ) {
			return er;
		}
		f->pasv = TRUE;
	}
	return pasv_open(f, FALSE, p_ctl);
}

/* v in decimal at s; how many digits */
LOCAL INT put_dec( UB *s, UINT v )
{
	UB	d[10];
	INT	n = 0, k;

	do {
		d[n++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	for ( k = 0; k < n; k++ ) s[k] = d[n - 1 - k];
	return n;
}

/*
 * The active way: a listening end, and the server told where it is --
 * EPRT |1|a.b.c.d|port|, or PORT a,b,c,d,p1,p2 once EPRT was refused.
 */
LOCAL ER data_listen( T_FTP *f )
{
	UB	a[4], arg[48];
	UINT	port = 0;
	INT	l, code, n, i;

	if ( f->net->listen == NULL || f->net->accept == NULL ) {
		return E_NOSPT;
	}
	l = f->net->listen(f->net->ctx, f->ctl, a, &port);
	if ( l < 0 ) {
		return (ER)l;
	}
	code = 0;
	if ( !f->port ) {
		n = 0;
		arg[n++] = '|';
		arg[n++] = '1';
		arg[n++] = '|';
		for ( i = 0; i < 4; i++ ) {
			n += put_dec(arg + n, a[i]);
			arg[n++] = ( i < 3 ) ? '.' : '|';
		}
		n += put_dec(arg + n, port);
		arg[n++] = '|';
		arg[n] = 0;
		code = ftp_cmd(f, "EPRT", arg);
		if ( code >= 500 ) {
			f->port = TRUE;			/* not known here: PORT from then on */
		}
	}
	if ( f->port ) {
		n = 0;
		for ( i = 0; i < 4; i++ ) {
			n += put_dec(arg + n, a[i]);
			arg[n++] = ',';
		}
		n += put_dec(arg + n, ( port >> 8 ) & 0xFF);
		arg[n++] = ',';
		n += put_dec(arg + n, port & 0xFF);
		arg[n] = 0;
		code = ftp_cmd(f, "PORT", arg);
	}
	if ( code != 200 ) {
		f->net->close(f->net->ctx, l);
		return ( code < 0 ) ? (ER)code : ( code < 400 ) ? E_IO : ftp_error(code);
	}
	f->lsn = l;
	return E_OK;
}

/* The listening end of the active way let go of */
LOCAL void lsn_close( T_FTP *f )
{
	if ( f->lsn >= 0 ) {
		f->net->close(f->net->ctx, f->lsn);
		f->lsn = -1;
	}
}

/* A transfer begun: the data connection, the command, its first reply */
LOCAL ER xfer_begin( T_FTP *f, CONST char *cmd, CONST UB *arg )
{
	INT	code;
	ER	er;

	if ( f == NULL || f->ctl < 0 ) {
		return E_PAR;
	}
	if ( f->data >= 0 || f->lsn >= 0 ) {
		return E_OBJ;				/* one transfer at a time */
	}
	if ( f->active ) {
		er = data_listen(f);
	} else {
		BOOL	ctl = FALSE;

		er = data_open(f, &ctl);
		if ( er < E_OK && !ctl && f->net->listen != NULL && f->net->accept != NULL ) {
			/* no passive way to this server: the active one, from then on, once it works */
			if ( data_listen(f) >= E_OK ) {
				f->active = TRUE;
				er = E_OK;
			}
		}
	}
	if ( er < E_OK ) {
		return er;
	}
	f->done = FALSE;
	code = ftp_cmd(f, cmd, arg);
	if ( code >= 100 && code < 300 ) {
		f->done = (BOOL)( code >= 200 );	/* over already: what came is all */
		if ( f->lsn < 0 ) {
			return E_OK;
		}
		/* the server connects to us now, if it has not already */
		f->data = f->net->accept(f->net->ctx, f->lsn, ( f->atmo > 0 ) ? f->atmo : FTP_ACCEPT_TMO);
		lsn_close(f);
		if ( f->data >= 0 ) {
			return E_OK;
		}
		er = ( f->data < 0 ) ? (ER)f->data : E_IO;
		f->data = -1;
		if ( !f->done ) {
			/* its word on the connection that did not come, to keep in step */
			do {
				code = get_reply(f, NULL, NULL);
			} while ( code >= 100 && code < 200 );
		}
		f->done = FALSE;
		return er;
	}
	if ( f->data >= 0 ) {
		f->net->close(f->net->ctx, f->data);
		f->data = -1;
	}
	lsn_close(f);
	return ( code < 0 ) ? (ER)code : ftp_error(code);
}

EXPORT ER ftp_get( T_FTP *f, CONST UB *path )
{
	return xfer_begin(f, "RETR", path);
}

EXPORT ER ftp_put( T_FTP *f, CONST UB *path )
{
	return xfer_begin(f, "STOR", path);
}

EXPORT INT ftp_read( T_FTP *f, void *buf, SZ n )
{
	if ( f == NULL || f->data < 0 ) {
		return E_OBJ;
	}
	return f->net->recv(f->net->ctx, f->data, buf, n);
}

EXPORT INT ftp_write( T_FTP *f, CONST void *buf, SZ n )
{
	ER	er;

	if ( f == NULL || f->data < 0 ) {
		return E_OBJ;
	}
	er = send_all(f, f->data, (CONST UB *)buf, (INT)n);
	return ( er < E_OK ) ? (INT)er : (INT)n;
}

EXPORT ER ftp_end( T_FTP *f )
{
	INT	code;

	if ( f == NULL || f->data < 0 ) {
		return E_OBJ;
	}
	f->net->close(f->net->ctx, f->data);
	f->data = -1;
	if ( f->done ) {
		f->done = FALSE;
		return E_OK;
	}
	do {
		code = get_reply(f, NULL, NULL);
	} while ( code >= 100 && code < 200 );
	return ( code < 0 ) ? (ER)code : ( code >= 300 ) ? ftp_error(code) : E_OK;
}

/* ---------------------------------------------------------------- listings */

LOCAL CONST char * CONST month[12] = {
	"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};

/* The month a word names (1 to 12), or 0 */
LOCAL INT month_of( CONST UB *w, INT n )
{
	INT	m;

	if ( n != 3 ) {
		return 0;
	}
	for ( m = 0; m < 12; m++ ) {
		if ( lower(w[0]) == (UB)month[m][0] && lower(w[1]) == (UB)month[m][1]
		  && lower(w[2]) == (UB)month[m][2] ) {
			return m + 1;
		}
	}
	return 0;
}

#define NTOK	12

/* Where the words of a line start and how long each is; how many */
LOCAL INT words( CONST UB *s, INT *at, INT *len )
{
	INT	i = 0, n = 0;

	while ( s[i] != 0 && n < NTOK ) {
		while ( is_blank(s[i]) ) i++;
		if ( s[i] == 0 ) break;
		at[n] = i;
		while ( s[i] != 0 && !is_blank(s[i]) ) i++;
		len[n] = i - at[n];
		n++;
	}
	return n;
}

/* "." and ".." are no names of a listing */
LOCAL BOOL dots( CONST UB *s, INT n )
{
	return (BOOL)( ( n == 1 && s[0] == '.' ) || ( n == 2 && s[0] == '.' && s[1] == '.' ) );
}

/* YYYY-MM-DD as a whole word: the year, month and day; FALSE when it is not that */
LOCAL BOOL iso_date( CONST UB *w, INT n, D *p_y, INT *p_mo, INT *p_d )
{
	INT	k;

	if ( n != 10 || w[4] != '-' || w[7] != '-' ) {
		return FALSE;
	}
	*p_y = number(w, &k);
	if ( k != 4 ) return FALSE;
	*p_mo = (INT)number(w + 5, &k);
	if ( k != 2 || *p_mo < 1 || *p_mo > 12 ) return FALSE;
	*p_d = (INT)number(w + 8, &k);
	return (BOOL)( k == 2 && *p_d >= 1 && *p_d <= 31 );
}

/* HH:MM or HH:MM:SS, with any fraction after the seconds, as a whole word */
LOCAL BOOL iso_time( CONST UB *w, INT n, D *p_h, D *p_mi, D *p_s )
{
	INT	k;

	*p_s = 0;
	if ( n < 5 || w[2] != ':' ) {
		return FALSE;
	}
	*p_h = number(w, &k);
	if ( k != 2 || *p_h > 23 ) return FALSE;
	*p_mi = number(w + 3, &k);
	if ( k != 2 || *p_mi > 59 ) return FALSE;
	if ( n == 5 ) return TRUE;
	if ( w[5] != ':' ) return FALSE;
	*p_s = number(w + 6, &k);
	if ( k != 2 || *p_s > 60 ) return FALSE;
	return (BOOL)( n == 8 || w[8] == '.' || w[8] == ',' );
}

/* A zone after a time: +HHMM or -HHMM, its offset in seconds; FALSE when it is not that */
LOCAL BOOL iso_zone( CONST UB *w, INT n, D *p_off )
{
	INT	k;
	D	v;

	if ( n != 5 || ( w[0] != '+' && w[0] != '-' ) ) {
		return FALSE;
	}
	v = number(w + 1, &k);
	if ( k != 4 ) return FALSE;
	*p_off = ( v / 100 ) * 3600 + ( v % 100 ) * 60;
	if ( w[0] == '-' ) *p_off = -*p_off;
	return TRUE;
}

/* The name of a line of "ls -l", from i on (a link's " -> where" taken off) */
LOCAL BOOL unix_name( T_FTP *f, CONST UB *s, INT i, T_FTPENT *e )
{
	INT	k, end;

	if ( s[i] == ' ' || s[i] == '\t' ) i++;
	for ( end = i; s[end] != 0; end++ ) ;
	if ( s[0] == 'l' ) {
		for ( k = i; k + 3 < end; k++ ) {
			if ( s[k] == ' ' && s[k + 1] == '-' && s[k + 2] == '>' && s[k + 3] == ' ' ) {
				end = k;
				break;
			}
		}
	}
	if ( end <= i || dots(s + i, end - i) ) {
		return FALSE;
	}
	from_server(f, s + i, end - i, e->name, sizeof(e->name));
	return TRUE;
}

/*
 * A line of "ls -l" with the date as ISO 8601 has it, as GNU ls
 * --time-style=long-iso or full-iso prints it and some servers send:
 * "-rw-r--r-- 1 user group 1234 2026-11-10 12:34 name", the time and,
 * after seconds, a zone (+0900) there or not. The date is the first
 * word past the owner that is one, the size the word before it; the
 * time is local to the server unless a zone is given.
 */
LOCAL BOOL parse_iso( T_FTP *f, CONST UB *s, T_FTPENT *e, CONST INT *at, CONST INT *len, INT n )
{
	INT	m, mo = 0, d = 0, last;
	D	y = 0, hh = 0, mi = 0, sec = 0, off = 0;

	for ( m = 2; m + 1 < n; m++ ) {
		if ( iso_date(s + at[m], len[m], &y, &mo, &d) ) break;
	}
	if ( m + 1 >= n || !is_digit(s[at[m - 1]]) ) {
		return FALSE;
	}
	last = m;
	if ( last + 2 < n && iso_time(s + at[last + 1], len[last + 1], &hh, &mi, &sec) ) {
		last++;
		if ( last + 2 < n && len[last] > 5 && iso_zone(s + at[last + 1], len[last + 1], &off) ) last++;
	}
	e->dir = (BOOL)( s[0] == 'd' );
	e->size = number(s + at[m - 1], NULL);
	e->mtime = ts_time(y, mo, d, (INT)hh, (INT)mi, (INT)sec) - off;
	return unix_name(f, s, at[last] + len[last], e);
}

/*
 * A line of "ls -l": the kind in the first letter, then somewhere a
 * month, a day and a time or a year, the size just before the month
 * and the name after them (a link's " -> where" taken off).
 */
LOCAL BOOL parse_unix( T_FTP *f, CONST UB *s, T_FTPENT *e )
{
	INT	at[NTOK], len[NTOK], n, m, mo = 0, k;
	D	day, y, hh = 0, mm = 0;

	if ( s[0] != '-' && s[0] != 'd' && s[0] != 'l' ) {
		return FALSE;
	}
	n = words(s, at, len);
	for ( m = 2; m + 3 < n; m++ ) {
		if ( ( mo = month_of(s + at[m], len[m]) ) > 0 ) break;
	}
	if ( mo == 0 ) {
		return parse_iso(f, s, e, at, len, n);
	}
	day = number(s + at[m + 1], &k);
	if ( day < 1 || k != len[m + 1] ) {
		return FALSE;
	}
	y = number(s + at[m + 2], &k);
	if ( y < 0 ) {
		return FALSE;
	}
	if ( s[at[m + 2] + k] == ':' ) {
		hh = y;
		mm = number(s + at[m + 2] + k + 1, NULL);
		if ( f->now > 0 ) {
			/* no year: the last twelve months */
			y = year_of(f->now);
			if ( ts_time(y, mo, (INT)day, 0, 0, 0) > f->now + 86400 ) y--;
		} else {
			y = 0;
		}
	}
	e->dir = (BOOL)( s[0] == 'd' );
	e->size = number(s + at[m - 1], NULL);
	e->mtime = ( y > 0 ) ? ts_time(y, mo, (INT)day, (INT)hh, (INT)mm, 0) : 0;

	/* the name: after one blank past the time or year */
	return unix_name(f, s, at[m + 2] + len[m + 2], e);
}

/* A line of DOS "dir": MM-DD-YY  HH:MMAM  <DIR> or size  name */
LOCAL BOOL parse_dos( T_FTP *f, CONST UB *s, T_FTPENT *e )
{
	INT	at[NTOK], len[NTOK], n, k, i, end;
	D	mo, d, y, hh, mm;
	CONST UB *t;

	n = words(s, at, len);
	if ( n < 4 ) {
		return FALSE;
	}
	mo = number(s, &k);
	if ( mo < 1 || s[k] != '-' ) return FALSE;
	d = number(s + k + 1, &i);
	if ( d < 1 || s[k + 1 + i] != '-' ) return FALSE;
	y = number(s + k + 2 + i, NULL);
	if ( y < 0 ) return FALSE;
	if ( y < 100 ) y += ( y < 70 ) ? 2000 : 1900;
	t = s + at[1];
	hh = number(t, &k);
	if ( hh < 0 || t[k] != ':' ) return FALSE;
	mm = number(t + k + 1, &i);
	t += k + 1 + i;
	if ( ( t[0] == 'P' || t[0] == 'p' ) && hh < 12 ) hh += 12;
	if ( ( t[0] == 'A' || t[0] == 'a' ) && hh == 12 ) hh = 0;

	e->dir = (BOOL)( len[2] == 5 && starts(s + at[2], "<DIR>") );
	e->size = e->dir ? -1 : number(s + at[2], NULL);
	e->mtime = ts_time(y, (INT)mo, (INT)d, (INT)hh, (INT)mm, 0);
	i = at[3];
	for ( end = i; s[end] != 0; end++ ) ;
	if ( dots(s + i, end - i) ) {
		return FALSE;
	}
	from_server(f, s + i, end - i, e->name, sizeof(e->name));
	return TRUE;
}

EXPORT BOOL ftp_parse_list( T_FTP *f, CONST UB *line, T_FTPENT *e )
{
	if ( line == NULL || e == NULL || starts(line, "total ") ) {
		return FALSE;
	}
	e->name[0] = 0;
	e->dir = FALSE;
	e->size = -1;
	e->mtime = 0;
	if ( is_digit(line[0]) ) {
		return parse_dos(f, line, e);
	}
	return parse_unix(f, line, e);
}

/* "fact=value;fact=value; name" */
EXPORT BOOL ftp_parse_mlsd( T_FTP *f, CONST UB *line, T_FTPENT *e )
{
	INT	i = 0, k, end;
	BOOL	skip = FALSE;

	if ( line == NULL || e == NULL ) {
		return FALSE;
	}
	e->name[0] = 0;
	e->dir = FALSE;
	e->size = -1;
	e->mtime = 0;
	while ( line[i] != 0 && line[i] != ' ' ) {
		CONST UB	*fact = line + i;

		for ( k = 0; fact[k] != 0 && fact[k] != ';' && fact[k] != ' '; k++ ) ;
		if ( starts(fact, "type=") ) {
			if ( starts(fact + 5, "dir;") || starts(fact + 5, "dir ") ) e->dir = TRUE;
			else if ( starts(fact + 5, "cdir") || starts(fact + 5, "pdir") ) skip = TRUE;
		} else if ( starts(fact, "size=") ) {
			e->size = number(fact + 5, NULL);
		} else if ( starts(fact, "modify=") ) {
			e->mtime = stamp(fact + 7);
		}
		i += k;
		if ( line[i] == ';' ) i++;
	}
	if ( line[i] != ' ' ) {
		return FALSE;
	}
	i++;
	for ( end = i; line[end] != 0; end++ ) ;
	if ( skip || end <= i || dots(line + i, end - i) ) {
		return FALSE;
	}
	from_server(f, line + i, end - i, e->name, sizeof(e->name));
	return TRUE;
}

#define L_LIST	0
#define L_MLSD	1
#define L_NLST	2

/*
 * Whether a line of LIST that gave no entry is one with no name in it to
 * lose: an empty one, "total N", or the directory itself or the one
 * above (a name "." or ".." last)
 */
LOCAL BOOL list_nothing( CONST UB *s )
{
	INT	n = s_len(s), b;

	if ( n == 0 || starts(s, "total ") ) {
		return TRUE;
	}
	for ( b = n; b > 0 && !is_blank(s[b - 1]); b-- ) ;
	return dots(s + b, n - b);
}

/*
 * The lines of a listing's data, each read into an entry and given to
 * cb. LIST is text for people, laid out as the server likes; when not
 * one of its lines could be read, though some held names, the names are
 * asked for again with NLST, which gives them alone, rather than none
 * shown.
 */
LOCAL ER listing( T_FTP *f, CONST UB *path, INT how, FTP_ENTCB cb, void *arg )
{
	UB	chunk[256];
	INT	got, i, n = 0, stop = 0, good = 0, bad = 0;
	BOOL	ok;
	ER	er;

	if ( f == NULL || cb == NULL ) {
		return E_PAR;
	}
	er = xfer_begin(f, ( how == L_MLSD ) ? "MLSD" : ( how == L_NLST ) ? "NLST" : "LIST", path);
	if ( er < E_OK ) {
		return er;
	}
	for (;;) {
		got = ftp_read(f, chunk, sizeof(chunk));
		if ( got < 0 ) {
			er = (ER)got;
			break;
		}
		for ( i = 0; i < got || ( got == 0 && n > 0 && i == 0 ); i++ ) {
			UB	c = ( got > 0 ) ? chunk[i] : '\n';

			if ( c == '\r' ) continue;
			if ( c != '\n' ) {
				if ( n < (INT)sizeof(f->line) - 1 ) f->line[n++] = c;
				continue;
			}
			f->line[n] = 0;
			n = 0;
			if ( stop < 0 ) continue;
			if ( how == L_MLSD ) {
				ok = ftp_parse_mlsd(f, f->line, &f->ent);
			} else if ( how == L_NLST ) {
				INT	len = s_len(f->line), b;

				/* a name alone, or a path whose last part it is */
				for ( b = len; b > 0 && f->line[b - 1] != '/'; b-- ) ;
				ok = (BOOL)( len > b && !dots(f->line + b, len - b) );
				if ( ok ) {
					from_server(f, f->line + b, len - b, f->ent.name, sizeof(f->ent.name));
					f->ent.dir = FALSE;
					f->ent.size = -1;
					f->ent.mtime = 0;
				}
			} else {
				ok = ftp_parse_list(f, f->line, &f->ent);
				if ( ok ) good++;
				else if ( !list_nothing(f->line) ) bad++;
			}
			if ( ok ) {
				stop = cb(arg, &f->ent);
			}
		}
		if ( got == 0 ) {
			break;
		}
	}
	if ( er < E_OK ) {
		(void)ftp_end(f);
		return er;
	}
	er = ftp_end(f);
	if ( er >= E_OK && how == L_LIST && good == 0 && bad > 0 ) {
		er = listing(f, path, L_NLST, cb, arg);
	}
	return er;
}

EXPORT ER ftp_list( T_FTP *f, CONST UB *path, FTP_ENTCB cb, void *arg )
{
	if ( f == NULL ) {
		return E_PAR;
	}
	return listing(f, path, ( ( f->feat & FTP_F_MLSD ) != 0 ) ? L_MLSD : L_LIST, cb, arg);
}

EXPORT ER ftp_nlst( T_FTP *f, CONST UB *path, FTP_ENTCB cb, void *arg )
{
	return listing(f, path, L_NLST, cb, arg);
}
