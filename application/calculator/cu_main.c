/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cu_main.c
 *	電卓: the accessory (design 11.17)
 *
 *	One window of push buttons from the data box (SYSDEF_BOX_CALCU):
 *	panel 1 the plain calculator, panel 2 the function calculator with
 *	its keys to the left of the plain ones. Each button's number is a
 *	key of the engine (cu_eng.h). Above the buttons the panel leaves a
 *	strip that is drawn here: the number, right aligned and as large as
 *	fits, and over it in small letters the memory's mark, the unit of
 *	angles, and the operations waiting or what went wrong.
 *
 *	The keyboard types as the buttons do (cu_hid_key); Ctrl+C copies
 *	the number shown to the tray as a text, to be put into a document,
 *	Ctrl+V takes the first number in the text in hand in the tray, and
 *	Ctrl+E closes. Switching between the two calculators makes the
 *	window again with the other panel where it was; what is waiting is
 *	worked out first.
 *
 *	Started with "selftest", it works the cases of cu_cases.h with its
 *	functions in double precision, says on the console which differed
 *	and ends with their number.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/tray.h>
#include <ts/sysdef.h>
#include "cu_eng.h"
#include "cu_cases.h"

#define TICK_MS		20
#define PN_STD		1
#define PN_SCI		2
#define WIN_X		600
#define WIN_Y		120

/* The display strip, in the work area */
#define D_LEFT		8
#define D_TOP		8
#define D_BOTTOM	58
#define D_PAD		6
#define SMALL_PX	12
#define LARGE_PX	24

#define C_GROUND	0x00FFFFFF
#define C_FRAME		0x00606060
#define C_TEXT		0x00000000
#define C_SUB		0x00606060
#define C_ERR		0x00C00000

#define MOD_CTRL	( 0x01 | 0x10 )
#define PASTE_MAX	( 64 * 1024 )

LOCAL UI	ui;
LOCAL CUENG	eng;
LOCAL BOOL	closing;
LOCAL INT	g_gid = -1;
LOCAL char	g_note[96];		/* said over the number until the next key */

/* A line to the console, which the tests read */
LOCAL void say( CONST char *fmt, ... )
{
	char	s[256];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	tm_putstring((CONST UB *)s);
}

/* ---------------------------------------------------------------- the display */

/* The bytes of a UTF-8 letter starting at s */
LOCAL INT letter_len( CONST char *s )
{
	UB	c = (UB)s[0];

	return ( c < 0x80 ) ? 1 : ( c & 0xE0 ) == 0xC0 ? 2 : ( c & 0xF0 ) == 0xE0 ? 3 : 4;
}

/* Small words ending at right; those that do not fit after left are cut off at the front */
LOCAL void small_right( CONST char *s, INT left, INT right, UW colour )
{
	char	b[200];
	CONST char *p = s;
	INT	w = dp_text_width((CONST UB *)s, SMALL_PX);

	snprintf(b, sizeof(b), "%s", s);
	while ( w > right - left && *p != 0 ) {
		p += letter_len(p);
		snprintf(b, sizeof(b), "…%s", p);
		w = dp_text_width((CONST UB *)b, SMALL_PX);
	}
	if ( w > 0 ) (void)dp_text(g_gid, right - w, D_TOP + 16, (CONST UB *)b, colour, SMALL_PX);
}

LOCAL void draw( void )
{
	T_DPRECT	r;
	char		num[64], up[200], ind[32];
	INT		x, w, px;
	UW		col = C_SUB;

	if ( g_gid < 0 ) return;
	r.left = D_LEFT;
	r.top = D_TOP;
	r.right = ui.w - D_LEFT;
	r.bottom = D_BOTTOM;
	(void)dp_fill_rect(g_gid, &r, C_GROUND);
	(void)dp_frame_rect(g_gid, &r, C_FRAME, 1);

	/* the marks: the memory, the angles */
	ind[0] = 0;
	if ( !cu_is_zero(&eng.mem) ) strcat(ind, "M ");
	if ( eng.sci ) strcat(ind, eng.deg ? "DEG" : "RAD");
	x = r.left + D_PAD;
	if ( ind[0] != 0 ) x += dp_text(g_gid, x, D_TOP + 16, (CONST UB *)ind, C_SUB, SMALL_PX) + 8;

	/* what is waiting, what went wrong, or what was done */
	if ( eng.err != CU_OK ) {
		snprintf(up, sizeof(up), "%s", cu_err_text(eng.err));
		col = C_ERR;
	} else if ( g_note[0] != 0 ) {
		snprintf(up, sizeof(up), "%s", g_note);
	} else {
		(void)cu_expr(&eng, up, sizeof(up));
	}
	if ( up[0] != 0 ) small_right(up, x, r.right - D_PAD, col);

	/* the number, as large as fits */
	(void)cu_show(&eng, num, sizeof(num));
	for ( px = LARGE_PX; px > SMALL_PX; px -= 2 ) {
		w = dp_text_width((CONST UB *)num, px);
		if ( w <= r.right - r.left - D_PAD * 2 ) break;
	}
	w = dp_text_width((CONST UB *)num, px);
	(void)dp_text(g_gid, r.right - D_PAD - w, r.bottom - 8, (CONST UB *)num,
		      ( eng.err != CU_OK ) ? C_ERR : C_TEXT, px);
	(void)wm_obj_flush(ui.kw, NULL);
}

/* What the display shows, to the console */
LOCAL void told( void )
{
	char	s[64], e[200];

	(void)cu_show(&eng, s, sizeof(s));
	(void)cu_expr(&eng, e, sizeof(e));
	say("calculator: shown %s [%s]\n", s, e);
}

LOCAL void note( CONST char *s )
{
	snprintf(g_note, sizeof(g_note), "%s", s);
}

LOCAL void key( INT k )
{
	g_note[0] = 0;
	cu_key(&eng, k);
	told();
	draw();
}

/* ---------------------------------------------------------------- the tray */

/* 複写: the number shown, as a text of one paragraph */
LOCAL void copy( void )
{
	char	s[64], frag[256];
	T_TRREC	r;

	if ( eng.err != CU_OK ) {
		note("エラーは複写できません");
		draw();
		return;
	}
	(void)cu_show(&eng, s, sizeof(s));
	snprintf(frag, sizeof(frag),
		 "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>%s</p></document></tad>", s);
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = frag;
	r.size = (SZ)strlen(frag);
	if ( tr_psh_dat(&r, 1, (CONST UB *)s) < E_OK ) {
		note("トレーに入れられませんでした");
	} else {
		note("トレーに複写しました");
		say("calculator: copied %s\n", s);
	}
	draw();
}

/* The letters of an xmlTAD text, its tags taken out (each a blank) and its entities read; its length */
LOCAL INT text_of( UB *s, INT n )
{
	INT	i = 0, o = 0;

	while ( i < n ) {
		if ( s[i] == '<' ) {
			while ( i < n && s[i] != '>' ) i++;
			i++;
			s[o++] = ' ';
			continue;
		}
		if ( s[i] == '&' ) {
			CONST char	*ent[] = { "&lt;", "&gt;", "&amp;", "&quot;", "&apos;" };
			CONST char	ch[] = { '<', '>', '&', '"', '\'' };
			INT		k;

			for ( k = 0; k < 5; k++ ) {
				INT	l = (INT)strlen(ent[k]);

				if ( i + l <= n && memcmp(s + i, ent[k], (size_t)l) == 0 ) break;
			}
			if ( k < 5 ) {
				s[o++] = (UB)ch[k];
				i += (INT)strlen(ent[k]);
				continue;
			}
		}
		s[o++] = s[i++];
	}
	return o;
}

/* 貼り込み: the first number in the set in hand */
LOCAL void paste( void )
{
	T_TRINF	inf;
	UB	*b;
	SZ	asz = 0;
	INT	i, n;
	BOOL	ok = FALSE;
	char	s[64];

	for ( i = 0; i < TR_REC_MAX && !ok; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( ( inf.kind != TR_TAD && inf.kind != TR_REC ) || inf.size <= 0 || inf.size > PASTE_MAX ) continue;
		b = malloc((size_t)inf.size + 1);
		if ( b == NULL ) break;
		if ( tr_rea_rec(TR_HAND, i, b, inf.size, &asz) >= E_OK ) {
			n = ( inf.kind == TR_TAD ) ? text_of(b, (INT)asz) : (INT)asz;
			ok = cu_paste(&eng, b, n);
		}
		free(b);
	}
	if ( ok ) {
		note("トレーから貼り込みました");
		(void)cu_show(&eng, s, sizeof(s));
		say("calculator: pasted %s\n", s);
	} else {
		note("トレーに数値がありません");
		say("calculator: nothing to paste\n");
	}
	draw();
}

/* ---------------------------------------------------------------- the window */

LOCAL ER open_win( INT x, INT y )
{
	ER	er = ui_open(&ui, SYSDEF_BOX_CALCU, eng.sci ? PN_SCI : PN_STD, x, y);

	if ( er < E_OK ) return er;
	g_gid = wm_obj_gid(ui.kw);
	draw();
	return E_OK;
}

/* The other calculator: the window made again with its panel, where it was */
LOCAL void mode( BOOL sci )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		x = WIN_X, y = WIN_Y;

	if ( sci == eng.sci ) return;
	if ( ob_rea_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		x = wp.left;
		y = wp.top;
	}
	ui_close(&ui);
	g_gid = -1;
	cu_mode(&eng, sci);
	g_note[0] = 0;
	if ( open_win(x, y) < E_OK ) {
		tm_putstring((CONST UB *)"calculator: no window\n");
		closing = TRUE;
		return;
	}
	say("calculator: mode %s\n", sci ? "関数電卓" : "標準");
	told();
}

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

LOCAL void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(SYSDEF_MENU_CALCU, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "std", eng.sci ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, NULL, "sci", eng.sci ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sep", eng.group ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "rad", ( eng.sci ? 0 : MN_GREY ) | ( eng.deg ? 0 : MN_TICK ));
	(void)mn_chg_atr(mid, NULL, "copy", ( eng.err != CU_OK ) ? MN_GREY : 0);
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		closing = TRUE;
	} else if ( is_code(&sel, "copy") ) {
		copy();
	} else if ( is_code(&sel, "paste") ) {
		paste();
	} else if ( is_code(&sel, "std") ) {
		mode(FALSE);
	} else if ( is_code(&sel, "sci") ) {
		mode(TRUE);
	} else if ( is_code(&sel, "sep") ) {
		eng.group = (BOOL)!eng.group;
		say("calculator: grouping %s\n", eng.group ? "on" : "off");
		told();
		draw();
	} else if ( is_code(&sel, "rad") ) {
		key(CU_K_ANGLE);
	} else if ( is_code(&sel, "clear") ) {
		key(CU_K_AC);
	}
}

/* A key of the keyboard */
LOCAL void typed( CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);
	INT	k;

	if ( ( m->mods & MOD_CTRL ) != 0 ) {
		if ( c == 'c' || c == 'C' ) copy();
		else if ( c == 'v' || c == 'V' ) paste();
		else if ( c == 'e' || c == 'E' ) closing = TRUE;
		return;
	}
	k = cu_hid_key(m->code, m->mods, c);
	if ( k != 0 ) key(k);
}

/* ---------------------------------------------------------------- the selftest */

LOCAL INT selftest( void )
{
	char	show[64], expr[64];
	INT	i, bad = 0, n = (INT)( sizeof(cu_cases) / sizeof(cu_cases[0]) );

	for ( i = 0; i < n; i++ ) {
		CONST CUCASE	*k = &cu_cases[i];

		cu_case_run(k, cu_fn, show, expr);
		if ( strcmp(show, k->show) != 0 || ( k->expr != NULL && strcmp(expr, k->expr) != 0 ) ) {
			say("calculator: selftest case %d \"%s\": \"%s\" [%s], not \"%s\" [%s]\n", i, k->keys, show, expr,
			    k->show, ( k->expr != NULL ) ? k->expr : "-");
			bad++;
		}
	}
	say("calculator: selftest %d of %d\n", n - bad, n);
	return bad;
}

/* ---------------------------------------------------------------- what happens */

int main( void )
{
	T_OBNTM	m;
	SZ	argn;
	char	arg[32];
	INT	id, ev;

	memset(arg, 0, sizeof(arg));
	argn = ts_get_arg(arg, sizeof(arg) - 1);
	if ( argn == 8 && memcmp(arg, "selftest", 8) == 0 ) return selftest();

	cu_init(&eng, FALSE, cu_fn);
	ui_icon(SYSDEF_PROG_CALCU);
	if ( open_win(WIN_X, WIN_Y) < E_OK ) {
		tm_putstring((CONST UB *)"calculator: no window\n");
		return 1;
	}
	say("calculator: ready\n");

	while ( !closing ) {
		for ( TMO tmo = TMO_FEVR; !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				closing = TRUE;
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_KEY:
				typed(&m);
				break;
			case UI_EV_BUTTON:
				key(id);
				break;
			default:
				break;
			}
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"calculator: closed\n");
	return 0;
}
