/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	serial.c
 *	シリアル通信: an accessory that runs as a process of its own
 *	(design 16.5.19)
 *
 *	A terminal on one serial port. The ports are the device objects the
 *	device manager keeps for them (design 18.7): record 1 is the line,
 *	read for what has come in and written to send; record 2 its speed;
 *	record 3 what the port is (T_SERINFO). What comes in is shown, what
 *	is typed is sent. The second button's menu chooses the port, its
 *	speed and what Enter sends, turns local echo on and off, clears the
 *	window and hangs up. It starts on the first port that is not the
 *	console's. Shift with PageUp and PageDown winds the view.
 */

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/ser.h>
#include "../lterm/lterm.h"

#define TICK_MS		30
#define READ_MAX	512

#define KEY_A		0x04
#define KEY_E		0x08
#define KEY_Z		0x1D
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_TAB		0x2B
#define KEY_PGUP	0x4B
#define KEY_DEL		0x4C
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define MOD_CTRL	0x11
#define MOD_SHIFT	0x22

#define NL_CR		0
#define NL_LF		1
#define NL_CRLF		2

LOCAL CONST char win_json[] = "{\"rect\":[140,120,800,540],\"attr\":15}";

/* The ports there are */
typedef struct {
	TS_UUID		uuid;
	UB		name[16];
	T_SERINFO	info;
} PORT;

LOCAL LTERM	term;
LOCAL LTWIN	win;
LOCAL PORT	port;			/* the port in use */
LOCAL INT	cur = -1;		/* its place among the ports, -1 none */
LOCAL ID	kdev = 0;		/* its key */
LOCAL UW	speed = 0;
LOCAL UINT	nl = NL_CR;
LOCAL BOOL	echo = FALSE;

LOCAL BOOL same_str( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 );
}

LOCAL INT put_s( UB *out, INT n, CONST UB *s )
{
	while ( *s != 0 ) out[n++] = *s++;
	out[n] = 0;
	return n;
}

/*
 * The serial ports: the device objects whose record 3 says what a port
 * is, as many as there are. The program keeps no heap, so they are not
 * kept: the devices are read a page at a time, each port handed to fn
 * with its place among them, and found again by its place when it is
 * chosen. How many there are.
 */
#define LIST_PAGE	32

LOCAL INT ports_each( void (*fn)( INT k, CONST PORT *p, void *ctx ), void *ctx )
{
	TS_UUID	ids[LIST_PAGE], last;
	T_OBREF	r;
	PORT	p;
	SZ	asz;
	INT	cnt, i, k, all = 0, n = 0;
	ID	key;

	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_DEVICE, OB_S_CHAR, ( all > 0 ) ? &last : NULL, ids, LIST_PAGE, &cnt) < E_OK ) {
			break;
		}
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&ids[i], &r) < E_OK || r.name[0] != 's' || r.name[1] != 'e'
			  || r.name[2] != 'r' ) {
				continue;
			}
			key = ob_opn_obj(&ids[i], OB_OP_READ);
			if ( key <= 0 ) {
				continue;
			}
			asz = 0;
			if ( ob_rea_rec(key, 3, 0, &p.info, sizeof(T_SERINFO), &asz) >= E_OK
			  && asz == (SZ)sizeof(T_SERINFO) ) {
				p.uuid = ids[i];
				for ( k = 0; r.name[k] != 0 && k < 15; k++ ) p.name[k] = r.name[k];
				p.name[k] = 0;
				if ( fn != NULL ) fn(n, &p, ctx);
				n++;
			}
			ob_cls_obj(key);
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == LIST_PAGE );
	return n;
}

/* The port at place k into the PORT ctx points at */
typedef struct {
	INT	want;
	PORT	*out;
	BOOL	found;
} PORTAT;

LOCAL void port_take( INT k, CONST PORT *p, void *ctx )
{
	PORTAT	*a = (PORTAT *)ctx;

	if ( k == a->want ) {
		*a->out = *p;
		a->found = TRUE;
	}
}

LOCAL BOOL port_at( INT k, PORT *out )
{
	PORTAT	a;

	a.want = k;
	a.out = out;
	a.found = FALSE;
	(void)ports_each(port_take, &a);
	return a.found;
}

/* The first port that is not the console's, else the first: its place, -1 none */
LOCAL void port_first( INT k, CONST PORT *p, void *ctx )
{
	INT	*first = (INT *)ctx;

	if ( first[0] < 0 && !p->info.console ) first[0] = k;
	if ( first[1] < 0 ) first[1] = k;
}

/* The window's name says which port, how fast, what Enter sends */
LOCAL void title( void )
{
	UB	t[96];
	INT	n = 0;

	n = put_s(t, n, (CONST UB *)"{\"name\":\"シリアル通信 ");
	if ( kdev > 0 ) {
		n = put_s(t, n, port.name);
		n = put_s(t, n, (CONST UB *)" ");
		n += lt_num(t + n, (D)speed);
		n = put_s(t, n, (CONST UB *)( ( nl == NL_CR ) ? " CR" : ( nl == NL_LF ) ? " LF" : " CR+LF" ));
		if ( echo ) n = put_s(t, n, (CONST UB *)" エコー");
	} else {
		n = put_s(t, n, (CONST UB *)"(未接続)");
	}
	n = put_s(t, n, (CONST UB *)"\"}");
	(void)ob_set_atr(win.kw, t, n);
}

LOCAL void hang_up( void )
{
	if ( kdev > 0 ) {
		ob_cls_obj(kdev);
	}
	kdev = 0;
	cur = -1;
}

LOCAL ER connect( INT i )
{
	SZ	asz = 0;
	ID	key;
	PORT	p;

	if ( i < 0 || !port_at(i, &p) ) {
		return E_PAR;
	}
	key = ob_opn_obj(&p.uuid, OB_OP_READ | OB_OP_WRITE);
	if ( key <= 0 ) {
		return (ER)key;
	}
	hang_up();
	kdev = key;
	cur = i;
	port = p;
	{
		T_OBNTF	req;

		memset(&req, 0, sizeof(req));
		req.events = OB_E_CHANGE;
		(void)ob_ntf_evt(kdev, 1, &req, win.port);	/* what comes in, told; dropped with the key */
	}
	speed = p.info.speed;
	(void)ob_rea_rec(kdev, 2, 0, &speed, sizeof(speed), &asz);
	title();
	return E_OK;
}

LOCAL void send( CONST UB *s, INT n )
{
	SZ	asz = 0;

	if ( kdev <= 0 || n <= 0 ) {
		return;
	}
	(void)ob_wri_rec(kdev, 1, 0, s, n, &asz);
	if ( echo ) {
		lt_put(&term, s, n);
	}
	term.back = 0;
}

/* What has come in on the line; TRUE when anything did */
/*
 * What came in, onto the screen: read until there is nothing, which asks
 * the port to tell (OB_E_CHANGE of its record 1) of what comes next, or
 * for 16 pieces at most, the rest left for the next round (line_more)
 * rather than waited for
 */
LOCAL BOOL	line_more = FALSE;

LOCAL BOOL take_line( void )
{
	UB	buf[READ_MAX];
	SZ	asz;
	BOOL	got = FALSE;
	INT	guard;

	line_more = FALSE;
	for ( guard = 0; kdev > 0; guard++ ) {
		if ( guard >= 16 ) {
			line_more = TRUE;
			break;
		}
		asz = 0;
		if ( ob_rea_rec(kdev, 1, 0, buf, sizeof(buf), &asz) < E_OK || asz == 0 ) {
			break;
		}
		lt_put(&term, buf, (INT)asz);
		got = TRUE;
	}
	if ( got ) {
		term.back = 0;
	}
	return got;
}

/* ---------------------------------------------------------------- keys */

LOCAL void key( CONST T_OBNTM *m, BOOL *p_done )
{
	BOOL	ctrl = (BOOL)( ( m->mods & MOD_CTRL ) != 0 );
	BOOL	shift = (BOOL)( ( m->mods & MOD_SHIFT ) != 0 );
	UB	s[4], ch;
	INT	n = 0;

	if ( ctrl && wm_key_cmd(m->code) == KEY_E ) {
		*p_done = TRUE;			/* 閉じる, the window menu's letter */
		return;
	}
	if ( shift && ( m->code == KEY_PGUP || m->code == KEY_PGDN ) ) {
		lt_wind(&term, ( m->code == KEY_PGUP ) ? 10 : -10);
		return;
	}
	if ( ctrl && wm_key_cmd(m->code) >= KEY_A && wm_key_cmd(m->code) <= KEY_Z ) {
		s[n++] = (UB)( 1 + wm_key_cmd(m->code) - KEY_A );	/* the control code of the letter typed */
	} else {
		switch ( m->code ) {
		case KEY_ENTER:
			if ( nl != NL_LF ) s[n++] = '\r';
			if ( nl != NL_CR ) s[n++] = '\n';
			break;
		case KEY_BS:	s[n++] = 0x08;	break;
		case KEY_TAB:	s[n++] = '\t';	break;
		case KEY_ESC:	s[n++] = 0x1B;	break;
		case KEY_DEL:	s[n++] = 0x7F;	break;
		case KEY_UP:	case KEY_DOWN:	case KEY_RIGHT:	case KEY_LEFT:
			s[n++] = 0x1B;
			s[n++] = '[';
			s[n++] = (UB)( ( m->code == KEY_UP ) ? 'A' : ( m->code == KEY_DOWN ) ? 'B'
				     : ( m->code == KEY_RIGHT ) ? 'C' : 'D' );
			break;
		default:
			ch = wm_key_char(m->code, m->mods);
			if ( ch >= 0x20 && ch < 0x7F ) s[n++] = ch;
			break;
		}
	}
	send(s, n);
}

/* ---------------------------------------------------------------- the menu */

LOCAL CONST UW speeds[] = {
	1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600
};

/* A port's line in the menu, into the labels ctx points at */
LOCAL void port_label( INT k, CONST PORT *p, void *ctx )
{
	UB	(*labels)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])ctx;
	INT	n;

	n = put_s(labels[k], 0, p->name);
	n = put_s(labels[k], n, (CONST UB *)"  ");
	n = put_s(labels[k], n, p->info.label);
	if ( p->info.console ) n = put_s(labels[k], n, (CONST UB *)" (コンソール)");
}

LOCAL void menu( CONST T_OBNTM *m, BOOL *p_done )
{
	TS_UUID	def, pdef;
	T_MNSEL	sel;
	UB	code[12];
	UW	v;
	SZ	asz;
	INT	i, n;
	ID	mid;
	ER	er;

	if ( lt_uuid(SYSDEF_MENU_SERIAL, &def) < E_OK || lt_uuid(SYSDEF_MENU_SER_PORT, &pdef) < E_OK
	  || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	n = ports_each(NULL, NULL);
	if ( n > 0 ) {
		/* a line each, on the stack: as many as there are, read again into them */
		UB	labels[n][WM_LABEL_MAX];
		INT	got;

		got = ports_each(port_label, labels);
		if ( got > n ) got = n;		/* more came meanwhile: the next menu has them */
		(void)mn_set_lst(mid, &pdef, "port", labels[0], WM_LABEL_MAX, got);
		if ( kdev > 0 && cur >= 0 ) {
			(void)mn_chg_idx(mid, &pdef, "port", cur, MN_TICK);
		}
	}
	for ( i = 0; i < (INT)( sizeof(speeds) / sizeof(speeds[0]) ); i++ ) {
		code[0] = 's';
		(void)lt_num(code + 1, (D)speeds[i]);
		(void)mn_chg_atr(mid, NULL, (CONST char *)code,
				 ( kdev <= 0 ) ? MN_GREY : ( speeds[i] == speed ) ? MN_TICK : 0);
	}
	(void)mn_chg_atr(mid, NULL, "nl.cr", ( nl == NL_CR ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "nl.lf", ( nl == NL_LF ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "nl.crlf", ( nl == NL_CRLF ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "echo", echo ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "hangup", ( kdev > 0 ) ? 0 : MN_GREY);

	if ( mn_pop_men(mid, win.kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		if ( same_str(sel.code, "close") ) {
			*p_done = TRUE;
		} else if ( same_str(sel.code, "port") ) {
			if ( connect(sel.index) < E_OK ) {
				lt_puts(&term, "\n[ポートを開けませんでした]\n");
			}
		} else if ( same_str(sel.code, "nl.cr") ) {
			nl = NL_CR;
		} else if ( same_str(sel.code, "nl.lf") ) {
			nl = NL_LF;
		} else if ( same_str(sel.code, "nl.crlf") ) {
			nl = NL_CRLF;
		} else if ( same_str(sel.code, "echo") ) {
			echo = (BOOL)!echo;
		} else if ( same_str(sel.code, "clear") ) {
			lt_clear(&term);
		} else if ( same_str(sel.code, "hangup") ) {
			hang_up();
		} else if ( sel.code[0] == 's' && sel.value > 0 && kdev > 0 ) {
			v = (UW)sel.value;
			er = ob_wri_rec(kdev, 2, 0, &v, sizeof(v), &asz);
			if ( er >= E_OK ) {
				speed = v;
			} else {
				lt_puts(&term, ( er == E_NOSPT ) ? "\n[コンソールのポートは速度を変えられません]\n"
								 : "\n[速度を変えられませんでした]\n");
			}
		}
		title();
	}
	(void)mn_del_men(mid);
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	BOOL	done = FALSE, again;
	INT	i;

	lt_clear(&term);
	if ( lt_open(&win, "シリアル通信", win_json) < E_OK ) {
		tm_putstring((CONST UB *)"serial: no window\n");
		return 1;
	}
	{
		INT	first[2] = { -1, -1 };

		(void)ports_each(port_first, first);
		i = ( first[0] >= 0 ) ? first[0] : first[1];
	}
	if ( i < 0 || connect(i) < E_OK ) {
		lt_puts(&term, "[シリアルポートがありません]\n");
		title();
	}
	lt_draw(&term, &win, NULL, TRUE);

	while ( !done ) {
		again = FALSE;
		/* asleep until the window, or the port, has something */
		for ( TMO tmo = line_more ? TMO_POL : TMO_FEVR;
		      !done && ob_rea_tmo(win.port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) {
			switch ( m.event ) {
			case OB_E_CLOSE:
			case OB_E_DELETE:
				done = TRUE;
				break;
			case OB_E_PRESS:
				if ( m.code == 1 ) menu(&m, &done);
				else (void)lt_pointer(&term, &m);
				again = TRUE;
				break;
			case OB_E_MOVE:
			case OB_E_RELEASE:
			case OB_E_WHEEL:
			case OB_E_SCROLL:
				if ( lt_pointer(&term, &m) ) again = TRUE;
				break;
			case OB_E_KEY:
				key(&m, &done);
				again = TRUE;
				break;
			case OB_E_REDRAW:
				again = TRUE;
				break;
			default:
				break;
			}
		}
		if ( done ) {
			break;
		}
		if ( take_line() || again ) {
			lt_draw(&term, &win, NULL, TRUE);
		}
	}
	hang_up();
	lt_close(&win);
	return 0;
}
