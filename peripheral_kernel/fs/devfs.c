/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devfs.c
 *	The devices of characters a POSIX program expects, and the pseudo
 *	terminals (design 5.8.4), as device objects listed in the object dev.
 *
 *	null, zero, full, random, urandom, tty and ptmx are added to the
 *	device manager when the system starts (knl_ttydev_start), the slave
 *	side of a pseudo terminal, ptsN, each time ptmx is opened, and taken
 *	away once both sides are closed. Opened as files of the POSIX view
 *	(objfs, knl_ttydev_open) they work as below; read or written through
 *	the object layer, null, zero, full and random give and take bytes and
 *	the terminals answer E_NOSPT. random and urandom read the random
 *	source as the caller. tty is the terminal of the caller's session.
 *
 *	A pseudo terminal has two sides. Opening ptmx makes one and gives
 *	its master side to the opener: what the master writes is what was
 *	typed, what it reads is what the programs wrote. The programs open
 *	the slave side, ptsN. Between them is the line discipline, as a
 *	terminal of Unix has it: with ICANON a line is edited (VERASE takes
 *	back a character -- a whole UTF-8 character with IUTF8 --, VWERASE a
 *	word, VKILL the line) and is read only once it is ended by a newline,
 *	VEOL or VEOF; without it, bytes are read as they come, by VMIN and
 *	VTIME. ECHO puts what was typed back out to the master; ISIG makes
 *	VINTR, VQUIT and VSUSP signals to the foreground process group;
 *	ICRNL turns a carriage return typed into a newline; OPOST with ONLCR
 *	writes a newline as carriage return and newline. The master sets
 *	the size (FS_IOC_SWINSZ), which tells the foreground group SIGWINCH.
 *
 *	A process that leads its session and has no terminal gets the first
 *	slave it opens (without FS_O_NOCTTY) as its session's terminal, and its
 *	group as the foreground group. A process of another group of the
 *	session that reads the terminal is sent SIGTTIN instead.
 *
 *	When the master is closed the session's terminal is hung up: its
 *	foreground group gets SIGHUP and SIGCONT, reads answer 0 and writes
 *	EX_IO.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/psx.h>
#include <ts/dt.h>
#include <ts/time.h>
#include "../obj/obj.h"

#define PTY_MAX		64
#define PTY_INQ		4096		/* bytes typed and not yet read */
#define PTY_OUTQ	16384		/* bytes written and not yet taken by the master */
#define PTY_LINES	256		/* lines ended and not yet read */
#define PTY_EDIT	1024		/* bytes of the line being edited */
#define PTY_SLICE	50

#define VDISABLE	0xFF

/* termios, NetBSD's values (include/ts/fs.h T_FSTERMIOS) */
#define T_ICRNL		0x00000100
#define T_INLCR		0x00000040
#define T_IGNCR		0x00000080
#define T_ISTRIP	0x00000020
#define T_IXON		0x00000200
#define T_IMAXBEL	0x00002000
#define T_IUTF8		0x00004000
#define T_OPOST		0x00000001
#define T_ONLCR		0x00000002
#define T_OCRNL		0x00000010
#define T_CREAD		0x00000800
#define T_CS8		0x00000300
#define T_HUPCL		0x00004000
#define T_ECHOKE	0x00000001
#define T_ECHOE		0x00000002
#define T_ECHOK		0x00000004
#define T_ECHO		0x00000008
#define T_ECHONL	0x00000010
#define T_ECHOCTL	0x00000040
#define T_ISIG		0x00000080
#define T_ICANON	0x00000100
#define T_IEXTEN	0x00000400
#define T_TOSTOP	0x00400000
#define T_NOFLSH	0x80000000

#define V_EOF		0
#define V_EOL		1
#define V_EOL2		2
#define V_ERASE		3
#define V_WERASE	4
#define V_KILL		5
#define V_REPRINT	6
#define V_INTR		8
#define V_QUIT		9
#define V_SUSP		10
#define V_DSUSP		11
#define V_START		12
#define V_STOP		13
#define V_LNEXT		14
#define V_DISCARD	15
#define V_MIN		16
#define V_TIME		17
#define V_STATUS	18

/* What an open file of /dev is (T_FILE.priv) */
#define DV_DIR		1
#define DV_PTSDIR	2
#define DV_NULL		3
#define DV_ZERO		4
#define DV_FULL		5
#define DV_RANDOM	6
#define DV_MASTER	7
#define DV_SLAVE	8

typedef struct {
	BOOL		used;
	INT		no;
	INT		masters;	/* open files of the master side */
	INT		slaves;		/* and of the slave side */
	BOOL		hung;		/* the master went: no more to read or write */
	T_FSTERMIOS	tio;
	T_FSWINSZ	ws;
	ID		pgrp;		/* the foreground group, 0 none */
	ID		sid;		/* the session whose terminal it is, 0 none */
	TS_TIME		mtime;

	/* typed: readable bytes, and where the lines end in them (ICANON) */
	UB		inq[PTY_INQ];
	INT		inhead, inlen;
	INT		lines[PTY_LINES];	/* bytes of each line ended, 0 for VEOF alone */
	INT		lhead, nlines;
	UB		edit[PTY_EDIT];		/* the line being edited */
	INT		nedit;
	BOOL		lnext;			/* the next byte is taken as it is */

	/* written: for the master to read */
	UB		outq[PTY_OUTQ];
	INT		outhead, outlen;
	INT		column;			/* where the echo is, for taking a tab back */
	T_OBEDGE	out_edge;		/* told on ptsN's record 1 (ts/ob.h): armed by a read that found nothing */
} PTY;

LOCAL PTY	*pty_tab[PTY_MAX];
LOCAL ID	pty_mtxid = 0;
LOCAL TS_TIME	dev_mtime = 0;

/* Signals to send once the lock is let go */
typedef struct {
	ID	pgrp[4];
	INT	sig[4];
	INT	n;
} SIGOUT;

LOCAL void sig_add( SIGOUT *so, ID pgrp, INT sig )
{
	if ( pgrp > 0 && so->n < 4 ) {
		so->pgrp[so->n] = pgrp;
		so->sig[so->n] = sig;
		so->n++;
	}
}

LOCAL void sig_send( SIGOUT *so )
{
	INT	i;

	for ( i = 0; i < so->n; i++ ) {
		(void)knl_psx_killpg(so->pgrp[i], so->sig[i]);
	}
	so->n = 0;
}

LOCAL void lock( void )
{
	tk_loc_mtx(pty_mtxid, TMO_FEVR);
}

LOCAL void unlock( void )
{
	tk_unl_mtx(pty_mtxid);
}

LOCAL void tio_default( T_FSTERMIOS *t )
{
	knl_memset(t, 0, sizeof(*t));
	t->c_iflag = T_ICRNL | T_IXON | T_IMAXBEL | T_IUTF8;
	t->c_oflag = T_OPOST | T_ONLCR;
	t->c_cflag = T_CREAD | T_CS8 | T_HUPCL;
	t->c_lflag = T_ECHOKE | T_ECHOE | T_ECHOK | T_ECHO | T_ECHOCTL | T_ISIG | T_ICANON | T_IEXTEN;
	t->c_cc[V_EOF] = 0x04;
	t->c_cc[V_EOL] = VDISABLE;
	t->c_cc[V_EOL2] = VDISABLE;
	t->c_cc[V_ERASE] = 0x7F;
	t->c_cc[V_WERASE] = 0x17;
	t->c_cc[V_KILL] = 0x15;
	t->c_cc[V_REPRINT] = 0x12;
	t->c_cc[7] = VDISABLE;
	t->c_cc[V_INTR] = 0x03;
	t->c_cc[V_QUIT] = 0x1C;
	t->c_cc[V_SUSP] = 0x1A;
	t->c_cc[V_DSUSP] = 0x19;
	t->c_cc[V_START] = 0x11;
	t->c_cc[V_STOP] = 0x13;
	t->c_cc[V_LNEXT] = 0x16;
	t->c_cc[V_DISCARD] = 0x0F;
	t->c_cc[V_MIN] = 1;
	t->c_cc[V_TIME] = 0;
	t->c_cc[V_STATUS] = 0x14;
	t->c_ispeed = t->c_ospeed = 38400;
}

/* ---------------------------------------------------------------- the queues */

/* Out to the master; what does not fit is dropped (echo never waits) */
LOCAL void out_put( PTY *p, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && p->outlen < PTY_OUTQ; i++ ) {
		p->outq[( p->outhead + p->outlen ) % PTY_OUTQ] = s[i];
		p->outlen++;
	}
	if ( i > 0 ) {
		(void)knl_obedge_came(&p->out_edge);	/* told once the lock is let go (pty_tell) */
	}
}

LOCAL void out_byte( PTY *p, UB c )
{
	out_put(p, &c, 1);
}

/* Columns a UTF-8 character starting with these bytes takes */
LOCAL INT char_cols( CONST UB *s, INT n )
{
	UINT	c;

	if ( n <= 0 ) return 0;
	if ( s[0] < 0x80 ) return 1;
	if ( n >= 3 && ( s[0] & 0xF0 ) == 0xE0 ) {
		c = ( (UINT)( s[0] & 0x0F ) << 12 ) | ( (UINT)( s[1] & 0x3F ) << 6 ) | ( s[2] & 0x3F );
		if ( ( c >= 0x1100 && c <= 0x115F ) || ( c >= 0x2E80 && c <= 0xA4CF )
		  || ( c >= 0xAC00 && c <= 0xD7A3 ) || ( c >= 0xF900 && c <= 0xFAFF )
		  || ( c >= 0xFE30 && c <= 0xFE4F ) || ( c >= 0xFF00 && c <= 0xFF60 )
		  || ( c >= 0xFFE0 && c <= 0xFFE6 ) ) {
			return 2;
		}
		return 1;
	}
	if ( n >= 4 && ( s[0] & 0xF8 ) == 0xF0 ) return 2;
	return 1;
}

/* What was typed, echoed: control characters as ^X with ECHOCTL */
LOCAL void echo_byte( PTY *p, UB c )
{
	T_FSTERMIOS *t = &p->tio;

	if ( ( t->c_lflag & T_ECHO ) == 0 ) {
		if ( c == '\n' && ( t->c_lflag & ( T_ECHONL | T_ICANON ) ) == ( T_ECHONL | T_ICANON ) ) {
			out_put(p, (CONST UB *)"\r\n", 2);
		}
		return;
	}
	if ( c == '\n' ) {
		out_put(p, ( t->c_oflag & ( T_OPOST | T_ONLCR ) ) == ( T_OPOST | T_ONLCR )
			   ? (CONST UB *)"\r\n" : (CONST UB *)"\n", ( t->c_oflag & T_ONLCR ) ? 2 : 1);
		p->column = 0;
	} else if ( c == '\t' ) {
		out_byte(p, c);
		p->column = ( p->column + 8 ) & ~7;
	} else if ( c < 0x20 || c == 0x7F ) {
		if ( ( t->c_lflag & T_ECHOCTL ) != 0 ) {
			out_byte(p, '^');
			out_byte(p, ( c == 0x7F ) ? '?' : (UB)( c + '@' ));
			p->column += 2;
		} else {
			out_byte(p, c);
		}
	} else {
		out_byte(p, c);
		if ( c < 0x80 || ( c & 0xC0 ) == 0xC0 ) p->column++;
	}
}

/* The last character of the line being edited taken back, and from the screen */
LOCAL void erase_char( PTY *p )
{
	T_FSTERMIOS *t = &p->tio;
	INT	start, cols, i;

	if ( p->nedit <= 0 ) return;
	start = p->nedit - 1;
	if ( ( t->c_iflag & T_IUTF8 ) != 0 ) {
		while ( start > 0 && ( p->edit[start] & 0xC0 ) == 0x80 ) start--;
	}
	cols = char_cols(p->edit + start, p->nedit - start);
	if ( p->edit[start] < 0x20 || p->edit[start] == 0x7F ) {
		cols = ( p->edit[start] == '\t' ) ? 1 : ( ( t->c_lflag & T_ECHOCTL ) ? 2 : 0 );
	}
	p->nedit = start;
	if ( ( t->c_lflag & T_ECHO ) != 0 && ( t->c_lflag & T_ECHOE ) != 0 ) {
		for ( i = 0; i < cols; i++ ) out_put(p, (CONST UB *)"\b \b", 3);
		p->column -= cols;
		if ( p->column < 0 ) p->column = 0;
	}
}

/* The line being edited goes to the readable bytes; 'eof' ends it without a byte of its own */
LOCAL void line_commit( PTY *p, BOOL eof )
{
	INT	i, n = 0;

	for ( i = 0; i < p->nedit && p->inlen < PTY_INQ; i++ ) {
		p->inq[( p->inhead + p->inlen ) % PTY_INQ] = p->edit[i];
		p->inlen++;
		n++;
	}
	p->nedit = 0;
	if ( p->nlines < PTY_LINES ) {
		p->lines[( p->lhead + p->nlines ) % PTY_LINES] = n;
		p->nlines++;
	}
	(void)eof;
}

LOCAL void in_flush( PTY *p )
{
	p->inhead = p->inlen = 0;
	p->lhead = p->nlines = 0;
	p->nedit = 0;
}

/* One byte typed, through the line discipline. Under the lock */
LOCAL void input_byte( PTY *p, UB c, SIGOUT *so )
{
	T_FSTERMIOS *t = &p->tio;
	UB	*cc = t->c_cc;

	if ( p->lnext ) {
		p->lnext = FALSE;
		goto plain;
	}
	if ( ( t->c_iflag & T_ISTRIP ) != 0 ) c &= 0x7F;
	if ( c == '\r' ) {
		if ( ( t->c_iflag & T_IGNCR ) != 0 ) return;
		if ( ( t->c_iflag & T_ICRNL ) != 0 ) c = '\n';
	} else if ( c == '\n' && ( t->c_iflag & T_INLCR ) != 0 ) {
		c = '\r';
	}
	if ( ( t->c_lflag & T_ISIG ) != 0 && c != VDISABLE ) {
		INT	sig = 0;

		if ( c == cc[V_INTR] ) sig = TS_SIGINT;
		else if ( c == cc[V_QUIT] ) sig = TS_SIGQUIT;
		else if ( c == cc[V_SUSP] ) sig = TS_SIGTSTP;
		else if ( c == cc[V_STATUS] ) sig = TS_SIGINFO;
		if ( sig != 0 ) {
			if ( ( t->c_lflag & T_NOFLSH ) == 0 && sig != TS_SIGINFO ) {
				in_flush(p);
			}
			if ( sig != TS_SIGINFO ) echo_byte(p, c);
			sig_add(so, p->pgrp, sig);
			return;
		}
	}
	if ( ( t->c_lflag & T_IEXTEN ) != 0 && c != VDISABLE && c == cc[V_LNEXT] ) {
		p->lnext = TRUE;
		if ( ( t->c_lflag & T_ECHO ) != 0 && ( t->c_lflag & T_ECHOCTL ) != 0 ) {
			out_put(p, (CONST UB *)"^\b", 2);
		}
		return;
	}
	if ( ( t->c_lflag & T_ICANON ) != 0 && c != VDISABLE ) {
		if ( c == cc[V_ERASE] || ( c == 0x08 && cc[V_ERASE] == 0x7F ) ) {
			erase_char(p);
			return;
		}
		if ( c == cc[V_KILL] ) {
			if ( ( t->c_lflag & T_ECHOKE ) != 0 && ( t->c_lflag & T_ECHO ) != 0 ) {
				while ( p->nedit > 0 ) erase_char(p);
			} else {
				p->nedit = 0;
				echo_byte(p, c);
				if ( ( t->c_lflag & T_ECHOK ) != 0 ) echo_byte(p, '\n');
			}
			return;
		}
		if ( ( t->c_lflag & T_IEXTEN ) != 0 && c == cc[V_WERASE] ) {
			while ( p->nedit > 0 && ( p->edit[p->nedit - 1] == ' ' || p->edit[p->nedit - 1] == '\t' ) ) {
				erase_char(p);
			}
			while ( p->nedit > 0 && p->edit[p->nedit - 1] != ' ' && p->edit[p->nedit - 1] != '\t' ) {
				erase_char(p);
			}
			return;
		}
		if ( ( t->c_lflag & T_IEXTEN ) != 0 && c == cc[V_REPRINT] ) {
			echo_byte(p, c);
			echo_byte(p, '\n');
			out_put(p, p->edit, p->nedit);
			return;
		}
		if ( c == cc[V_EOF] ) {
			line_commit(p, TRUE);
			return;
		}
	}
    plain:
	if ( ( t->c_lflag & T_ICANON ) != 0 ) {
		if ( c == '\n' || ( c != VDISABLE && ( c == cc[V_EOL] || c == cc[V_EOL2] ) ) ) {
			if ( p->nedit < PTY_EDIT ) p->edit[p->nedit++] = c;
			echo_byte(p, c);
			line_commit(p, FALSE);
			return;
		}
		if ( p->nedit >= PTY_EDIT - 1 ) {
			if ( ( t->c_iflag & T_IMAXBEL ) != 0 ) out_byte(p, 0x07);
			return;
		}
		p->edit[p->nedit++] = c;
		echo_byte(p, c);
		return;
	}
	if ( p->inlen < PTY_INQ ) {
		p->inq[( p->inhead + p->inlen ) % PTY_INQ] = c;
		p->inlen++;
		echo_byte(p, c);
	} else if ( ( t->c_iflag & T_IMAXBEL ) != 0 ) {
		out_byte(p, 0x07);
	}
}

/* The settings changed: a line being edited is readable once ICANON is off */
LOCAL void tio_set( PTY *p, CONST T_FSTERMIOS *nt )
{
	BOOL	was = ( ( p->tio.c_lflag & T_ICANON ) != 0 );

	p->tio = *nt;
	if ( was && ( nt->c_lflag & T_ICANON ) == 0 ) {
		INT	i;

		for ( i = 0; i < p->nedit && p->inlen < PTY_INQ; i++ ) {
			p->inq[( p->inhead + p->inlen ) % PTY_INQ] = p->edit[i];
			p->inlen++;
		}
		p->nedit = 0;
		p->nlines = 0;
		p->lhead = 0;
	} else if ( !was && ( nt->c_lflag & T_ICANON ) != 0 ) {
		p->nlines = 0;			/* what is there forms the line being edited */
		while ( p->inlen > 0 && p->nedit < PTY_EDIT ) {
			p->edit[p->nedit++] = p->inq[p->inhead];
			p->inhead = ( p->inhead + 1 ) % PTY_INQ;
			p->inlen--;
		}
		p->inhead = 0;
	}
}

/* Bytes the slave may read now (ICANON: of the first line, -1 for an end of file) */
LOCAL INT readable( PTY *p )
{
	if ( ( p->tio.c_lflag & T_ICANON ) != 0 ) {
		if ( p->nlines == 0 ) return 0;
		return ( p->lines[p->lhead] == 0 ) ? -1 : p->lines[p->lhead];
	}
	return p->inlen;
}

/* ---------------------------------------------------------------- making one */

LOCAL PTY *pty_new( void )
{
	INT	i;
	PTY	*p;

	for ( i = 0; i < PTY_MAX; i++ ) {
		if ( pty_tab[i] == NULL ) {
			p = (PTY *)Kcalloc(1, sizeof(PTY));
			if ( p == NULL ) return NULL;
			pty_tab[i] = p;
			break;
		}
		if ( !pty_tab[i]->used ) {
			p = pty_tab[i];
			knl_memset(p, 0, sizeof(PTY));
			break;
		}
	}
	if ( i >= PTY_MAX ) {
		return NULL;
	}
	p->used = TRUE;
	p->no = i;
	tio_default(&p->tio);
	p->ws.ws_row = 24;
	p->ws.ws_col = 80;
	(void)dt_gettime(&p->mtime);
	return p;
}

LOCAL PTY *pty_of( INT no )
{
	return ( no >= 0 && no < PTY_MAX && pty_tab[no] != NULL && pty_tab[no]->used ) ? pty_tab[no] : NULL;
}

/* ---------------------------------------------------------------- the device objects */

/*
 * The devices served here, as objects of the device manager: their
 * names in /dev, the kind of each, and the UUID the manager gave it. The
 * slave side of a pseudo terminal is added as "ptsN" when its master is
 * opened, and taken away once both sides are closed.
 */
typedef struct {
	CONST char	*name;
	UINT		kind;
	UINT		mode;
	TS_UUID		u;
	BOOL		ok;
} DVOBJ;

LOCAL DVOBJ dv_objs[] = {
	{ .name = "null",	.kind = DV_NULL,	.mode = 0666 },
	{ .name = "zero",	.kind = DV_ZERO,	.mode = 0666 },
	{ .name = "full",	.kind = DV_FULL,	.mode = 0666 },
	{ .name = "random",	.kind = DV_RANDOM,	.mode = 0644 },
	{ .name = "urandom",	.kind = DV_RANDOM,	.mode = 0644 },
	{ .name = "tty",	.kind = DV_SLAVE,	.mode = 0666 },
	{ .name = "ptmx",	.kind = DV_MASTER,	.mode = 0666 },
};
#define DV_NOBJS	( (INT)( sizeof(dv_objs) / sizeof(dv_objs[0]) ) )

LOCAL TS_UUID	pts_u[PTY_MAX];
LOCAL BOOL	pts_ok[PTY_MAX];

/* What a read or write through the object (ob_rea_rec, not a file of the POSIX view) does */
LOCAL INT dvo_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>A device of characters of the POSIX programs (design 5.8.4): "
			  "opened as a file of /dev.</p>");
}

LOCAL INT dvo_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(j, n, max, ",\"virtual\":true,\"posix\":true");
}

LOCAL ER dvo_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UINT	kind = (UINT)(UBINT)ctx;
	SZ	i;

	(void)pos;
	(void)off;
	if ( recno != 1 ) return E_NOEXS;
	if ( size < 0 ) return E_PAR;
	switch ( kind ) {
	  case DV_NULL:
		*p_asize = 0;
		return E_OK;
	  case DV_ZERO:
	  case DV_FULL:
		for ( i = 0; i < size; i++ ) ((UB *)buf)[i] = 0;
		*p_asize = size;
		return E_OK;
	  case DV_RANDOM:
		*p_asize = size;
		return ts_get_random(buf, size);
	  default:
		return E_NOSPT;			/* a terminal: opened as a file only */
	}
}

LOCAL ER dvo_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	UINT	kind = (UINT)(UBINT)ctx;

	(void)pos;
	(void)off;
	(void)buf;
	if ( recno != 1 ) return E_NOEXS;
	if ( size < 0 ) return E_PAR;
	switch ( kind ) {
	  case DV_NULL:
	  case DV_ZERO:
		*p_asize = size;
		return E_OK;
	  case DV_FULL:
		return E_LIMIT;
	  default:
		return E_NOSPT;
	}
}

#define DVO_OPS(mode)	{ OB_S_CHAR, "posix", 2, (mode), 0, 0, dvo_text, dvo_attr, dvo_rea, dvo_wri, NULL, NULL }

LOCAL CONST T_OBDVOPS dvo_ops_666 = DVO_OPS(0666);
LOCAL CONST T_OBDVOPS dvo_ops_644 = DVO_OPS(0644);
LOCAL CONST T_OBDVOPS dvo_ops_pts = DVO_OPS(0620);

/* The name of the slave side of pty no: "ptsN" */
LOCAL void pts_name( INT no, UB *name )
{
	INT	k = 0;

	name[k++] = 'p';
	name[k++] = 't';
	name[k++] = 's';
	if ( no >= 10 ) name[k++] = (UB)( '0' + no / 10 );
	name[k++] = (UB)( '0' + no % 10 );
	name[k] = 0;
}

LOCAL void pts_add( INT no )
{
	UB	name[8];

	pts_name(no, name);
	pts_ok[no] = ( knl_obdev_add(name, NULL, &dvo_ops_pts, (void *)(UBINT)DV_SLAVE, &pts_u[no]) >= E_OK );
	knl_objfs_dev_sync(1000);		/* listed in dev before the opener looks for it */
}

LOCAL void pts_del( INT no )
{
	UB	name[8];

	if ( no < 0 || no >= PTY_MAX || !pts_ok[no] ) return;
	pts_name(no, name);
	(void)knl_obdev_del(name);
	pts_ok[no] = FALSE;
	knl_objfs_dev_sync(TMO_POL);
}

EXPORT void knl_ttydev_start( void )
{
	T_CMTX	c;
	INT	i;

	if ( pty_mtxid <= 0 ) {
		c.exinf = NULL;
		c.mtxatr = TA_TFIFO;
		pty_mtxid = tk_cre_mtx(&c);
		if ( pty_mtxid <= 0 ) return;
	}
	(void)dt_gettime(&dev_mtime);
	for ( i = 0; i < DV_NOBJS; i++ ) {
		DVOBJ	*d = &dv_objs[i];

		d->ok = ( knl_obdev_add((CONST UB *)d->name, NULL,
					( d->mode == 0644 ) ? &dvo_ops_644 : &dvo_ops_666,
					(void *)(UBINT)d->kind, &d->u) >= E_OK );
	}
}

/* ---------------------------------------------------------------- opened as a file */

/* A slave opened: becomes the session's terminal when the caller leads one that has none */
LOCAL void slave_open( PTY *p, UINT oflags )
{
	ID	sid = knl_psx_sid();

	if ( ( oflags & FS_O_NOCTTY ) != 0 || sid <= 0 || p->sid != 0 ) {
		return;
	}
	if ( sid != ts_get_pid() || knl_psx_ctty() >= 0 ) {
		return;				/* only the leader takes one, and only the first */
	}
	if ( knl_psx_set_ctty(p->no) >= E_OK ) {
		p->sid = sid;
		p->pgrp = knl_psx_pgid();
	}
}

/* The kind of a device object of these, and for ptsN its pty; EX_NOENT for another */
LOCAL ER dv_which( CONST TS_UUID *u, UINT *p_kind, INT *p_no )
{
	INT	i;

	*p_no = -1;
	for ( i = 0; i < DV_NOBJS; i++ ) {
		if ( dv_objs[i].ok && ts_uuid_cmp(&dv_objs[i].u, u) == 0 ) {
			*p_kind = dv_objs[i].kind;
			return EX_OK;
		}
	}
	for ( i = 0; i < PTY_MAX; i++ ) {
		if ( pts_ok[i] && ts_uuid_cmp(&pts_u[i], u) == 0 ) {
			*p_kind = DV_SLAVE;
			*p_no = i;
			return EX_OK;
		}
	}
	return EX_NOENT;
}

EXPORT BOOL knl_ttydev_is( CONST TS_UUID *u )
{
	UINT	kind;
	INT	no;

	return ( pty_mtxid > 0 && dv_which(u, &kind, &no) >= EX_OK );
}

EXPORT ER knl_ttydev_open( CONST TS_UUID *u, UINT oflags, T_FILE *f )
{
	UINT	kind;
	INT	no;
	PTY	*p;
	ER	er;

	if ( pty_mtxid <= 0 ) {
		return EX_NOENT;
	}
	er = dv_which(u, &kind, &no);
	if ( er < EX_OK ) {
		return er;
	}
	f->priv = kind;
	f->obj = NULL;
	f->mode = FS_IFCHR;
	switch ( kind ) {
	  case DV_MASTER:
		lock();
		p = pty_new();
		if ( p != NULL ) p->masters = 1;
		unlock();
		if ( p == NULL ) return EX_NOSPC;
		pts_add(p->no);
		f->obj = p;
		f->ino = 0x100 + (UD)p->no;
		return EX_OK;
	  case DV_SLAVE:
		if ( no < 0 ) {
			no = knl_psx_ctty();
			if ( no < 0 ) return EX_NXIO;
		}
		lock();
		p = pty_of(no);
		if ( p == NULL || p->masters == 0 || p->hung ) {
			unlock();
			return ( p == NULL ) ? EX_NOENT : EX_IO;
		}
		p->slaves++;
		unlock();
		slave_open(p, oflags);
		f->obj = p;
		f->ino = 0x200 + (UD)p->no;
		return EX_OK;
	  default:
		f->ino = kind;
		return EX_OK;
	}
}

LOCAL ER dv_close( T_FILE *f )
{
	PTY	*p = (PTY *)f->obj;
	SIGOUT	so;
	INT	no = -1, gone = -1;

	so.n = 0;
	if ( p == NULL ) {
		return EX_OK;
	}
	lock();
	if ( f->priv == DV_MASTER ) {
		p->masters--;
		if ( p->masters == 0 ) {
			p->hung = TRUE;
			if ( p->pgrp > 0 ) {
				sig_add(&so, p->pgrp, TS_SIGHUP);
				sig_add(&so, p->pgrp, TS_SIGCONT);
			}
			if ( p->sid != 0 ) no = p->no;
		}
	} else {
		p->slaves--;
	}
	if ( p->masters == 0 && p->slaves == 0 ) {
		if ( p->sid != 0 ) no = p->no;
		gone = p->no;		/* kept until its slave's object is gone too */
	}
	unlock();
	f->obj = NULL;
	sig_send(&so);
	if ( no >= 0 ) {
		knl_psx_tty_gone(no);
	}
	if ( gone >= 0 ) {
		pts_del(gone);
		lock();
		if ( p->masters == 0 && p->slaves == 0 ) p->used = FALSE;
		unlock();
	}
	knl_fs_wake();
	return EX_OK;
}

/* random bytes, as the caller may read the random source */
LOCAL INT rnd_read( void *buf, SZ len )
{
	SZ	asz = 0;
	ID	key;
	ER	er;

	key = ob_opn_obj(&ob_uuid_random, OB_OP_READ);
	if ( key < E_OK ) {
		return EX_ACCES;
	}
	er = ob_rea_rec(key, OB_RND_DATA, 0, buf, len, &asz);
	(void)ob_cls_obj(key);
	return ( er < E_OK ) ? EX_IO : (INT)asz;
}

/* Whether the caller is to read or write its terminal: SIGTTIN/SIGTTOU to a group in the background */
LOCAL ER background( PTY *p, INT sig )
{
	ID	pg;

	if ( p->sid == 0 || knl_psx_ctty() != p->no ) {
		return EX_OK;			/* not the caller's terminal: no group to judge by */
	}
	pg = knl_psx_pgid();
	if ( pg <= 0 || p->pgrp == 0 || pg == p->pgrp ) {
		return EX_OK;
	}
	if ( knl_psx_sig_blocked(sig) ) {
		return ( sig == TS_SIGTTIN ) ? EX_IO : EX_OK;
	}
	(void)knl_psx_killpg(pg, sig);
	return EX_INTR;
}

LOCAL INT slave_read( T_FILE *f, PTY *p, UB *d, SZ len )
{
	UD	start = 0, now;
	INT	n, i, got = 0;
	ER	er;

	er = background(p, TS_SIGTTIN);
	if ( er < EX_OK ) {
		return er;
	}
	(void)ts_get_mono(&start);
	for ( ;; ) {
		lock();
		if ( p->hung ) {
			unlock();
			return 0;
		}
		n = readable(p);
		if ( n < 0 ) {				/* VEOF at the start of a line */
			p->lhead = ( p->lhead + 1 ) % PTY_LINES;
			p->nlines--;
			unlock();
			return 0;
		}
		if ( n > 0 ) {
			BOOL canon = ( ( p->tio.c_lflag & T_ICANON ) != 0 );

			if ( n > len ) n = (INT)len;
			for ( i = 0; i < n; i++ ) {
				d[i] = p->inq[p->inhead];
				p->inhead = ( p->inhead + 1 ) % PTY_INQ;
			}
			p->inlen -= n;
			if ( canon ) {
				p->lines[p->lhead] -= n;
				if ( p->lines[p->lhead] == 0 ) {
					p->lhead = ( p->lhead + 1 ) % PTY_LINES;
					p->nlines--;
				}
			}
			unlock();
			knl_fs_wake();
			return n;
		}
		if ( ( p->tio.c_lflag & T_ICANON ) == 0 ) {
			UINT	vmin = p->tio.c_cc[V_MIN], vtime = p->tio.c_cc[V_TIME];

			if ( vmin == 0 && vtime == 0 ) {
				unlock();
				return got;
			}
			if ( vmin == 0 ) {
				(void)ts_get_mono(&now);
				if ( ( now - start ) / 100000000ULL >= vtime ) {
					unlock();
					return 0;
				}
			}
		}
		unlock();
		if ( ( f->oflags & FS_O_NONBLOCK ) != 0 ) {
			return EX_AGAIN;
		}
		if ( !knl_fs_wait_slice(PTY_SLICE) ) {
			return EX_INTR;
		}
	}
}

/*
 * What the master has to read, told to whoever watches record 1 of the
 * slave's device object (ptsN) once the lock is let go: the edge was
 * marked under it when the bytes were put out (out_put)
 */
LOCAL void pty_tell( PTY *p )
{
	if ( knl_obedge_take(&p->out_edge) && p->no >= 0 && p->no < PTY_MAX && pts_ok[p->no] ) {
		knl_obdev_post(&pts_u[p->no], 1, OB_E_CHANGE, NULL);
	}
}

LOCAL INT slave_write( T_FILE *f, PTY *p, CONST UB *s, SZ len )
{
	SZ	done = 0;
	ER	er;

	if ( ( p->tio.c_lflag & T_TOSTOP ) != 0 ) {
		er = background(p, TS_SIGTTOU);
		if ( er < EX_OK ) return er;
	}
	while ( done < len ) {
		INT	room;

		lock();
		if ( p->hung ) {
			unlock();
			return ( done > 0 ) ? (INT)done : EX_IO;
		}
		room = PTY_OUTQ - p->outlen;
		if ( room >= 2 ) {
			BOOL post = ( ( p->tio.c_oflag & T_OPOST ) != 0 );

			while ( done < len && PTY_OUTQ - p->outlen >= 2 ) {
				UB	c = s[done++];

				if ( post && c == '\n' && ( p->tio.c_oflag & T_ONLCR ) != 0 ) {
					out_byte(p, '\r');
					p->column = 0;
				} else if ( post && c == '\r' && ( p->tio.c_oflag & T_OCRNL ) != 0 ) {
					c = '\n';
				}
				out_byte(p, c);
				if ( c == '\r' || c == '\n' ) p->column = 0;
				else if ( c >= 0x20 && ( c < 0x80 || ( c & 0xC0 ) == 0xC0 ) ) p->column++;
			}
			(void)dt_gettime(&p->mtime);
			unlock();
			pty_tell(p);
			knl_fs_wake();
			continue;
		}
		unlock();
		if ( ( f->oflags & FS_O_NONBLOCK ) != 0 ) {
			return ( done > 0 ) ? (INT)done : EX_AGAIN;
		}
		if ( !knl_fs_wait_slice(PTY_SLICE) ) {
			return ( done > 0 ) ? (INT)done : EX_INTR;
		}
	}
	return (INT)done;
}

LOCAL INT master_read( T_FILE *f, PTY *p, UB *d, SZ len )
{
	INT	n, i;

	for ( ;; ) {
		lock();
		if ( p->outlen > 0 ) {
			n = ( len < p->outlen ) ? (INT)len : p->outlen;
			for ( i = 0; i < n; i++ ) {
				d[i] = p->outq[p->outhead];
				p->outhead = ( p->outhead + 1 ) % PTY_OUTQ;
			}
			p->outlen -= n;
			unlock();
			knl_fs_wake();
			return n;
		}
		if ( ( f->oflags & FS_O_NONBLOCK ) != 0 ) {
			knl_obedge_arm(&p->out_edge);	/* nothing to read: told of what comes next */
			unlock();
			return EX_AGAIN;
		}
		unlock();
		if ( !knl_fs_wait_slice(PTY_SLICE) ) {
			return EX_INTR;
		}
	}
}

LOCAL INT master_write( PTY *p, CONST UB *s, SZ len )
{
	SIGOUT	so;
	SZ	i;

	so.n = 0;
	lock();
	for ( i = 0; i < len; i++ ) {
		input_byte(p, s[i], &so);
	}
	unlock();
	pty_tell(p);
	sig_send(&so);
	knl_fs_wake();
	return (INT)len;
}

LOCAL INT dv_read( T_FILE *f, void *buf, SZ len )
{
	UB	*d = (UB *)buf;
	SZ	i;

	switch ( f->priv ) {
	  case DV_NULL:		return 0;
	  case DV_ZERO:
	  case DV_FULL:
		for ( i = 0; i < len; i++ ) d[i] = 0;
		return (INT)len;
	  case DV_RANDOM:	return rnd_read(buf, len);
	  case DV_MASTER:	return master_read(f, (PTY *)f->obj, d, len);
	  case DV_SLAVE:	return slave_read(f, (PTY *)f->obj, d, len);
	  default:		return EX_ISDIR;
	}
}

LOCAL INT dv_write( T_FILE *f, CONST void *buf, SZ len )
{
	switch ( f->priv ) {
	  case DV_NULL:
	  case DV_ZERO:
	  case DV_RANDOM:	return (INT)len;
	  case DV_FULL:		return EX_NOSPC;
	  case DV_MASTER:	return master_write((PTY *)f->obj, (CONST UB *)buf, len);
	  case DV_SLAVE:	return slave_write(f, (PTY *)f->obj, (CONST UB *)buf, len);
	  default:		return EX_ISDIR;
	}
}

LOCAL ER dv_ioctl( T_FILE *f, UINT cmd, void *arg )
{
	PTY	*p = (PTY *)f->obj;
	SIGOUT	so;
	ER	er = EX_OK;
	INT	v;

	so.n = 0;
	if ( p == NULL ) {
		return EX_NOTTY;
	}
	lock();
	switch ( cmd ) {
	  case FS_IOC_ISTTY:
		break;
	  case FS_IOC_TCGETS:
		*(T_FSTERMIOS *)arg = p->tio;
		break;
	  case FS_IOC_TCSETSF:
		in_flush(p);
		/* FALLTHROUGH */
	  case FS_IOC_TCSETS:
	  case FS_IOC_TCSETSW:
		tio_set(p, (CONST T_FSTERMIOS *)arg);
		break;
	  case FS_IOC_GWINSZ:
		*(T_FSWINSZ *)arg = p->ws;
		break;
	  case FS_IOC_SWINSZ:
		{
			CONST T_FSWINSZ *w = (CONST T_FSWINSZ *)arg;

			if ( w->ws_row != p->ws.ws_row || w->ws_col != p->ws.ws_col ) {
				sig_add(&so, p->pgrp, TS_SIGWINCH);
			}
			p->ws = *w;
		}
		break;
	  case FS_IOC_GPGRP:
		*(INT *)arg = ( p->pgrp > 0 ) ? (INT)p->pgrp : 0;
		break;
	  case FS_IOC_GSID:
		*(INT *)arg = (INT)p->sid;
		if ( p->sid == 0 ) er = EX_NOTTY;
		break;
	  case FS_IOC_SPGRP:
		v = *(CONST INT *)arg;
		if ( f->priv == DV_SLAVE && ( p->sid == 0 || p->sid != knl_psx_sid() ) ) {
			er = EX_NOTTY;
		} else if ( v <= 0 || ( p->sid != 0 && !knl_psx_pg_in_sid((ID)v, p->sid) ) ) {
			er = EX_PERM;
		} else {
			p->pgrp = (ID)v;
		}
		break;
	  case FS_IOC_SCTTY:
		if ( p->sid != 0 && p->sid != knl_psx_sid() ) {
			er = EX_PERM;
		} else if ( knl_psx_sid() != ts_get_pid() ) {
			er = EX_PERM;
		} else {
			unlock();
			er = knl_psx_set_ctty(p->no);
			lock();
			if ( er >= E_OK ) {
				p->sid = knl_psx_sid();
				p->pgrp = knl_psx_pgid();
				er = EX_OK;
			} else {
				er = EX_PERM;
			}
		}
		break;
	  case FS_IOC_NOTTY:
		if ( p->sid != 0 && p->sid == knl_psx_sid() ) {
			v = p->no;
			p->sid = 0;
			p->pgrp = 0;
			unlock();
			knl_psx_tty_gone(v);
			lock();
		}
		break;
	  case FS_IOC_GPTN:
		*(INT *)arg = p->no;
		break;
	  case FS_IOC_NREAD:
		v = ( f->priv == DV_MASTER ) ? p->outlen : readable(p);
		*(INT *)arg = ( v < 0 ) ? 0 : v;
		break;
	  case FS_IOC_FLUSH:
		v = *(CONST INT *)arg;
		if ( v == 0 || v == 2 ) in_flush(p);
		if ( v == 1 || v == 2 ) p->outhead = p->outlen = 0;
		break;
	  case FS_IOC_DRAIN:
		break;
	  default:
		er = EX_NOTTY;
		break;
	}
	unlock();
	sig_send(&so);
	knl_fs_wake();
	return er;
}

LOCAL UINT dv_poll( T_FILE *f, UINT events )
{
	PTY	*p = (PTY *)f->obj;
	UINT	r = 0;

	(void)events;
	switch ( f->priv ) {
	  case DV_MASTER:
		lock();
		if ( p->outlen > 0 ) r |= FS_POLL_IN;
		r |= FS_POLL_OUT;
		unlock();
		return r;
	  case DV_SLAVE:
		lock();
		if ( p->hung ) {
			r = FS_POLL_IN | FS_POLL_HUP;
		} else {
			if ( readable(p) != 0 ) r |= FS_POLL_IN;
			if ( PTY_OUTQ - p->outlen >= 256 ) r |= FS_POLL_OUT;
		}
		unlock();
		return r;
	  default:
		return FS_POLL_IN | FS_POLL_OUT;
	}
}

LOCAL void st_of( UINT kind, INT no, T_FSXSTAT *st )
{
	st->st.mode = ( kind == DV_DIR || kind == DV_PTSDIR ) ? ( FS_IFDIR | 0755 ) : ( FS_IFCHR | 0666 );
	if ( kind == DV_SLAVE ) st->st.mode = FS_IFCHR | 0620;
	st->st.size = 0;
	st->st.mtime = (UD)dev_mtime;
	st->st.ino = ( kind == DV_SLAVE && no >= 0 ) ? 0x200 + (UD)no : kind;
	st->rdev = ( kind == DV_SLAVE && no >= 0 ) ? 0x500 + (UINT)no : kind;
}

LOCAL ER dv_fxstat( T_FILE *f, T_FSXSTAT *st )
{
	PTY	*p = (PTY *)f->obj;

	st_of((UINT)f->priv, ( p != NULL ) ? p->no : -1, st);
	if ( p != NULL ) {
		st->st.mtime = (UD)p->mtime;
		if ( f->priv == DV_MASTER ) st->st.ino = 0x100 + (UD)p->no;
	}
	return EX_OK;
}

EXPORT CONST T_FIMP knl_fimp_ttydev = {
	.name     = "ttydev",
	.close    = dv_close,
	.read     = dv_read,
	.write    = dv_write,
	.fxstat   = dv_fxstat,
	.ioctl    = dv_ioctl,
	.poll     = dv_poll,
};
