/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	clock.c
 *	時計: an accessory that runs as a process of its own (design 18.18,
 *	stage 5c)
 *
 *	It is what every program outside the kernel is made of, and nothing
 *	more: a window made as a real object, drawn in through the window's
 *	own drawing environment, the time read from the clock device's
 *	object, and what happens to the window told at a channel of its own.
 *	It shows the time until it is asked to close -- the pictogram in its
 *	band pressed, or its window deleted by another program -- and then
 *	takes its window away and ends. A press in it switches between the
 *	time and the date; the second button opens its menu (閉じる,
 *	日付を表示, and the items every window's menu ends with).
 */

#include <tk/typedef.h>
#include <tk/syscall.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>

#define PORT_NAME	"clock.port"
#define TICK_MS		250

LOCAL CONST char win_json[] = "{\"rect\":[520,80,780,190],\"attr\":3}";

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

/* A number on the console, for saying what went wrong */
LOCAL void put_num( CONST char *what, INT v )
{
	UB	b[40], d[12];
	UW	u = ( v < 0 ) ? (UW)-v : (UW)v;
	INT	n = 0, i = 0;

	while ( what[n] != 0 && n < 20 ) {
		b[n] = (UB)what[n];
		n++;
	}
	b[n++] = ' ';
	if ( v < 0 ) b[n++] = '-';
	do {
		d[i++] = (UB)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	while ( i > 0 ) b[n++] = d[--i];
	b[n++] = '\n';
	b[n] = 0;
	tm_putstring(b);
}

/* The clock's reading, "2026-09-25T18:30:12+09:00", cut to what is shown */
LOCAL void now_text( ID kclk, BOOL date, UB *out )
{
	UB	t[40];
	SZ	asz = 0;
	INT	from, n, i;

	out[0] = 0;
	if ( ob_rea_rec(kclk, 1, 0, t, sizeof(t) - 1, &asz) < E_OK || asz < 19 ) {
		return;
	}
	from = date ? 0 : 11;
	n = date ? 10 : 8;
	for ( i = 0; i < n; i++ ) {
		out[i] = t[from + i];
	}
	out[n] = 0;
}

LOCAL INT hexv( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* A UUID written out, as the system's definitions name them */
LOCAL ER uuid_of( CONST char *s, TS_UUID *u )
{
	INT	i = 0, k = 0, h, l;

	while ( k < 16 ) {
		if ( s[i] == '-' ) {
			i++;
			continue;
		}
		h = hexv(s[i]);
		l = ( h >= 0 ) ? hexv(s[i + 1]) : -1;
		if ( l < 0 ) {
			return E_PAR;
		}
		u->b[k++] = (UB)( h * 16 + l );
		i += 2;
	}
	return E_OK;
}

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	INT	i;

	for ( i = 0; code[i] != 0; i++ ) {
		if ( sel->code[i] != (UB)code[i] ) return FALSE;
	}
	return (BOOL)( sel->code[i] == 0 );
}

/* The second button: the menu, and what was chosen in it */
LOCAL void menu( ID kw, CONST T_OBNTM *m, BOOL *p_date, BOOL *p_done )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;

	if ( uuid_of(SYSDEF_MENU_CLOCK, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	(void)mn_chg_atr(mid, NULL, "date", *p_date ? MN_TICK : 0);
	if ( mn_pop_men(mid, kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		if ( is_code(&sel, "close") ) {
			*p_done = TRUE;
		} else if ( is_code(&sel, "date") ) {
			*p_date = (BOOL)!*p_date;
		}
	}
	(void)mn_del_men(mid);
}

/* Drawn and shown; below E_OK when the window is no longer there */
/*
 * ms to the next second of the system's time, the clock's reading, a
 * little past it so that the reading has turned over when it is read
 */
LOCAL TMO next_second( void )
{
	SYSTIM	t;
	UD	ms;

	if ( tk_get_tim(&t) < E_OK ) return TICK_MS;
	ms = ( ( (UD)(UW)t.hi << 32 ) | (UD)(UW)t.lo ) % 1000;
	return (TMO)( 1000 - ms ) + 5;
}

LOCAL ER draw( INT gid, ID kw, CONST UB *text )
{
	T_DPRECT	all = { 0, 0, 240, 80 };
	T_DPRECT	face = { 8, 8, 232, 72 };
	INT		w = dp_text_width(text, 32);
	INT		x = ( w > 0 && w < 224 ) ? ( 240 - w ) / 2 : 16;

	(void)dp_fill_rect(gid, &all, 0x00FFFFFF);
	(void)dp_frame_rect(gid, &face, 0x00606060, 2);
	(void)dp_text(gid, x, 52, text, 0x00000000, 32);
	return wm_obj_flush(kw, NULL);
}

int main( void )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBNTM		m;
	TS_UUID		win, ch;
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		text[16], shown[16];
	SZ		asz = 0;
	BOOL		date = FALSE, done = FALSE;
	INT		gid, i;
	ID		kw, port, kclk;

	/* the window, as an object: 時計 in its band */
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"時計";
	c.json = (CONST UB *)win_json;
	c.jsonsz = s_len(win_json);
	if ( ob_cre_obj(&c, &win) < E_OK ) {
		tm_putstring((CONST UB *)"clock: no window\n");
		return 1;
	}
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	kclk = ob_opn_obj(&clk, OB_OP_READ);

	/* where what happens to it arrives */
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK )
	     ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_REDRAW | OB_E_DELETE;
	if ( kw <= 0 || gid < 0 || port <= 0 || ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) {
		tm_putstring((CONST UB *)"clock: the window cannot be used\n");
		put_num("  key", kw);
		put_num("  gid", gid);
		put_num("  port", port);
		(void)ob_del_obj(&win);
		return 2;
	}
	tm_putstring((CONST UB *)"clock: up\n");

	shown[0] = 0;
	while ( !done ) {
		BOOL	again = FALSE;

		/* nothing shown yet: no wait, or the window stays empty until the next second */
		for ( TMO tmo = ( shown[0] == 0 ) ? TMO_POL : next_second(); ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) {
			if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				done = TRUE;
			} else if ( m.event == OB_E_PRESS && m.code == 1 ) {
				menu(kw, &m, &date, &done);
				again = TRUE;
			} else if ( m.event == OB_E_PRESS ) {
				date = !date;
				again = TRUE;
			} else if ( m.event == OB_E_REDRAW ) {
				again = TRUE;
			}
		}
		if ( done ) {
			break;
		}
		now_text(kclk, date, text);
		for ( i = 0; text[i] != 0 && text[i] == shown[i]; i++ ) ;
		if ( again || text[i] != shown[i] ) {
			if ( draw(gid, kw, text) < E_OK ) {
				break;		/* its window went without word */
			}
			memcpy(shown, text, sizeof(shown));
		}
	}

	tm_putstring((CONST UB *)"clock: closed\n");
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	ob_cls_obj(kclk);
	ob_cls_obj(kw);
	(void)ob_del_obj(&win);

	return 0;
}
