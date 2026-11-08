/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttip.c
 *	The text input port (TIP): かな漢字変換 in the text windows
 *
 *	The keyboard is in one of two input modes, as BTRON's is: 日本語
 *	("Rあ": what is typed goes to the converter, ts/kconv.h) or 英語
 *	("Ra": letters go into the text as they are). The mode is the
 *	window manager's, worked as the keys come and before each is
 *	addressed (outer_kernel/wm/msgline.c, where the mode keys are), and
 *	shown on the message line before the clock; a key read here finds
 *	the mode already as that key left it.
 *
 *	In 日本語 the keys are the converter's BTRON keys: 変換 (CNV),
 *	Shift+変換 (RCNV), 無変換 (CNV0, Alt+Space), Shift+無変換 (CNV1),
 *	空白 (KSP, Alt+Tab), Enter (NL), Shift+Enter (CR), Esc or F9 (CAN),
 *	右Ctrl or F10 (IEND), Alt+英数 or F11 (ASSIST), the cursor keys (CC),
 *	Alt with them (SC), Page Up/Down, Home, End and F1..F8 (PF). Letters
 *	are romaji, or with かな入力 the kana on the Japanese keyboard's keys.
 *
 *	What the converter is working on -- the composition -- is drawn
 *	over the text at the caret: each clause underlined, the one being
 *	converted turned over, and the caret in it. While candidates are
 *	being chosen they are listed under it; a number or a press on a row
 *	takes one. What the converter commits is put into the text as typing
 *	is, and a key the converter does not want (nothing being composed)
 *	is the text's.
 *
 *	One composition at a time: it belongs to the window it was begun
 *	in, and is committed when the hand presses anywhere else, the keys
 *	go to another window, or the mode becomes 英語.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/kconv.h>
#include "desktop.h"

/* keyboard usages */
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_TAB		0x2B
#define KEY_SPACE	0x2C
#define KEY_F1		0x3A
#define KEY_F8		0x41
#define KEY_F9		0x42
#define KEY_F10		0x43
#define KEY_F11		0x44
#define KEY_HOME	0x4A
#define KEY_PGUP	0x4B
#define KEY_DEL		0x4C
#define KEY_END		0x4D
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define KEY_KANA	0x88		/* ひらがなカタカナ */
#define KEY_HENKAN	0x8A
#define KEY_MUHENKAN	0x8B
#define KEY_RCTRL	0xE4
#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define MOD_ALT		( HID_MOD_LALT | HID_MOD_RALT )

#define TIP_LIST_MAX	4096
#define TIP_ROWS	9		/* candidates on a page */

#define TIP_GROUND	0x00FFFFFFU
#define TIP_INK		0x00000000U
#define TIP_TARGET	0x000050A0U	/* the clause being converted */
#define TIP_FRAME	0x00808080U

/* The input mode: the keyboard's, whichever window the keys go to */
#define tip_mode()	wm_msg_get_mode()
#define tip_jp		( ( tip_mode() & WM_MODE_ALPH ) == 0 )			/* 日本語; else 英語 */
#define tip_kata	( tip_jp && ( tip_mode() & WM_MODE_KANA ) != 0 )	/* 日本語 in katakana */
#define tip_caps	( !tip_jp && ( tip_mode() & WM_MODE_KANA ) != 0 )	/* 英語 in capitals */
#define tip_han		( ( tip_mode() & WM_MODE_HAN ) != 0 )		/* letters half width */
#define tip_roman	( ( tip_mode() & WM_MODE_ROMAN ) != 0 )		/* in 日本語, romaji typed */

LOCAL INT	tip_kid = 0;		/* the converter's session */
LOCAL INT	tip_said_roman = -1;	/* ローマ字入力 as the session was last told: 1, 0, or -1 not yet */
LOCAL DTWIN	*tip_win = NULL;	/* the window composed in */
LOCAL T_KCOUT	*tip_out = NULL;	/* the composition, as the converter gave it */
LOCAL INT	tip_base;		/* where the composition starts in it */
LOCAL BOOL	tip_conv = FALSE;	/* converted, not just typed */

LOCAL BOOL	tip_list = FALSE;	/* candidates shown */
LOCAL UB	tip_cand[TIP_LIST_MAX];
LOCAL INT	tip_ncand, tip_first, tip_total, tip_sel;
LOCAL T_DPRECT	tip_list_box;		/* where the list is, in the window */
LOCAL INT	tip_row_h;

/* Whether there is a composition in the window */
EXPORT BOOL tip_busy( CONST DTWIN *d )
{
	return (BOOL)( d != NULL && d == tip_win && tip_out != NULL
		    && tip_out->cl[tip_out->n_cl] > tip_base );
}

LOCAL BOOL tip_ready( void )
{
	if ( tip_out == NULL ) {
		tip_out = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
		if ( tip_out == NULL ) {
			return FALSE;
		}
		knl_memset(tip_out, 0, sizeof(*tip_out));
	}
	if ( tip_kid <= 0 ) {
		tip_kid = kc_open();
		tip_said_roman = -1;
	}
	return (BOOL)( tip_kid > 0 );
}

/* The session told ローマ字入力 or かな入力 as the mode has it, before a letter */
LOCAL void tip_tell_roman( void )
{
	INT	r = tip_roman ? 1 : 0;

	if ( tip_kid > 0 && r != tip_said_roman ) {
		(void)kc_input(tip_kid, r ? TSMOZC_M_ROMAN : 0);
		tip_said_roman = r;
	}
}

/* The window drawn again, with the composition over it */
LOCAL void tip_show( DTWIN *d )
{
	dt_draw(( d->tb_host != NULL ) ? d->tb_host : d);
	wm_update();
}

/*
 * What the converter gave, taken: the committed clauses into the text,
 * the rest kept as the composition, and the list fetched when there is
 * one to show.
 */
LOCAL void tip_take( DTWIN *d, INT flags )
{
	T_KCOUT	*o = tip_out;

	tip_base = 0;
	if ( o->n_out > 0 ) {
		INT	end = o->cl[o->n_out];

		if ( end > 0 ) {
			dd_type_text(d, (CONST UB *)o->text, end);
		}
		tip_base = end;
	}
	tip_win = ( o->cl[o->n_cl] > tip_base ) ? d : NULL;
	if ( tip_win == NULL ) {
		tip_conv = FALSE;
	}
	tip_list = FALSE;
	if ( ( flags & TSMOZC_LIST ) != 0 && tip_win != NULL ) {
		INT	r = kc_list(tip_kid, tip_cand, TIP_LIST_MAX, &tip_ncand,
				    &tip_first, &tip_total);

		if ( r > 0 && tip_ncand > 0 ) {
			tip_list = TRUE;
			tip_sel = r;
		}
	}
	tip_show(d);
}

/* A key to the converter; FALSE when it is not the converter's */
LOCAL BOOL tip_send( DTWIN *d, UINT code, UINT stat )
{
	INT	r;

	if ( !tip_ready() ) {
		return FALSE;
	}
	r = kc_key(tip_kid, code, stat, tip_out);
	if ( r == KC_NOTMINE || r < 0 ) {
		return FALSE;
	}
	tip_take(d, r);
	return TRUE;
}

/* Whatever is being composed, committed where it was begun */
EXPORT void tip_commit( void )
{
	DTWIN	*d = tip_win;

	if ( d == NULL || !d->used || !tip_busy(d) ) {
		tip_win = NULL;
		tip_list = FALSE;
		return;
	}
	(void)tip_send(d, TSMOZC_K_CR, 0);
	tip_win = NULL;
	tip_list = FALSE;
}

/* ---------------------------------------------------------------- the input mode */

EXPORT BOOL tip_japanese( void )
{
	return tip_jp;
}

/* The mode the machine's settings say to start in, put on the message line */
EXPORT void tip_start( void )
{
	wm_kbmode_start();
}

/*
 * A mode key, whichever window the keys go to (d, or none): TRUE when
 * the key was one. The mode is already as the key set it; what is being
 * composed is committed when it became 英語 or かな入力 and ローマ字入力
 * changed, and the window is drawn again for its caret and the mode
 * shown in it. 右Ctrl (入力終) commits what is being composed.
 */
EXPORT BOOL tip_mode_key( DTWIN *d, UINT key, UINT mods )
{
	if ( key == KEY_RCTRL ) {
		if ( tip_win != NULL ) {
			tip_commit();
		}
		return TRUE;
	}
	if ( !wm_kbmode_key(key, mods) ) {
		return FALSE;
	}
	if ( !tip_jp || ( key == KEY_KANA && ( mods & MOD_ALT ) != 0 ) ) {
		tip_commit();
	}
	if ( d != NULL && d->used ) {
		tip_show(d);			/* the caret's colour */
	}
	return TRUE;
}

/*
 * A letter typed as it is, in the width and case of the mode: TRUE when
 * it went into the text.
 */
LOCAL BOOL tip_plain( DTWIN *d, UB ch )
{
	UB	t[4];
	UINT	u;

	if ( ch < 0x20 || ch >= 0x7F ) {
		return FALSE;
	}
	if ( !tip_jp && tip_caps
	  && ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ) ) {
		ch ^= 0x20;			/* the shift turns it back */
	}
	if ( tip_han ) {
		dd_type_text(d, &ch, 1);
		return TRUE;
	}
	u = ( ch == ' ' ) ? 0x3000 : 0xFF01 + ( ch - 0x21 );
	t[0] = (UB)( 0xE0 | ( u >> 12 ) );
	t[1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
	t[2] = (UB)( 0x80 | ( u & 0x3F ) );
	dd_type_text(d, t, 3);
	return TRUE;
}

/* ---------------------------------------------------------------- keys */

/* A key in a text. TRUE when the port took it. */
EXPORT BOOL tip_key( DTWIN *d, UINT key, UINT mods )
{
	BOOL	shift = (BOOL)( ( mods & MOD_SHIFT ) != 0 );
	UINT	stat = shift ? TSMOZC_S_SHIFT : 0;
	UB	ch = wm_key_char(key, mods);
	UINT	code, tc;
	BOOL	busy;

	if ( tip_win != NULL && tip_win != d ) {
		tip_commit();			/* the keys went elsewhere */
	}
	if ( !tip_jp ) {
		if ( tip_win != NULL ) {
			tip_commit();		/* the mode became 英語 where no mode key was seen here */
		}
		return tip_plain(d, ch);	/* 英語 */
	}
	busy = tip_busy(d);
	if ( tip_kata ) {
		stat |= TSMOZC_S_KANA;
	}

	/* a candidate by its number */
	if ( tip_list && d == tip_win && ch >= '1' && ch <= '9'
	  && ch - '1' < tip_ncand ) {
		INT	r = kc_choose(tip_kid, tip_first + ( ch - '1' ), tip_out);

		if ( r >= 0 && r != KC_NOTMINE ) {
			tip_take(d, r);
		}
		return TRUE;
	}

	/* a key that is not a letter: the converter's while it composes */
	code = kc_code_key(key, mods);
	if ( code != 0 ) {
		if ( !busy ) {
			return ( code == TSMOZC_K_SPACE ) ? tip_plain(d, ' ') : FALSE;
		}
		switch ( code ) {
		case TSMOZC_K_SPACE:
		case TSMOZC_K_CNV:
		case TSMOZC_K_RCNV:
		case TSMOZC_K_DOWN:
		case TSMOZC_K_SC_D:
			tip_conv = TRUE;
			break;
		case TSMOZC_K_BS:
		case TSMOZC_K_CAN:
		case TSMOZC_K_ASSIST:
			tip_conv = FALSE;
			break;
		case TSMOZC_K_PG_U:
		case TSMOZC_K_PG_D:
			/* with the list shown, its pages */
			if ( tip_list ) {
				code = ( code == TSMOZC_K_PG_U ) ? TSMOZC_K_LIST_PREV
								 : TSMOZC_K_LIST_NEXT;
			}
			break;
		default:
			break;
		}
		(void)tip_send(d, code, stat);
		return TRUE;			/* nothing else has the keys meanwhile */
	}

	/* a letter: romaji, or the kana on the key */
	tip_tell_roman();
	tc = tip_roman ? ( ( ch > 0x20 && ch < 0x7F ) ? ch : 0 ) : kc_kana_key_m(key, mods);
	if ( tc != 0 ) {
		tip_conv = FALSE;
		return (BOOL)( tip_send(d, tc, stat) || busy );
	}
	return busy;
}

/*
 * A press in a window: on the list, the candidate under it chosen;
 * anywhere else, what is being composed committed first. TRUE when the
 * press was the list's.
 */
EXPORT BOOL tip_press( DTWIN *d, INT x, INT y )
{
	if ( tip_list && tip_win != NULL && ( d == tip_win || d == tip_win->tb_host )
	  && x >= tip_list_box.left && x < tip_list_box.right
	  && y >= tip_list_box.top && y < tip_list_box.bottom && tip_row_h > 0 ) {
		INT	row = ( y - tip_list_box.top - 2 ) / tip_row_h;

		if ( row >= 0 && row < tip_ncand ) {
			DTWIN	*t = tip_win;	/* a piece of text in a figure, perhaps */
			INT	r = kc_choose(tip_kid, tip_first + row, tip_out);

			if ( r >= 0 && r != KC_NOTMINE ) {
				tip_take(t, r);
			}
		}
		return TRUE;
	}
	if ( tip_win != NULL ) {
		tip_commit();
	}
	return FALSE;
}

/* A window going away: what is composed in it is committed first */
EXPORT void tip_forget( DTWIN *d )
{
	if ( tip_win != NULL && ( tip_win == d || tip_win->tb_host == d ) ) {
		tip_commit();
	}
}

/* ---------------------------------------------------------------- drawing */

/* Bytes a..b of the composition, as a string */
LOCAL void tip_piece( INT a, INT b, UB *buf, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) {
		buf[n++] = (UB)tip_out->text[a];
	}
	buf[n] = 0;
}

LOCAL INT tip_width( ID fid, INT a, INT b )
{
	UB	buf[TSMOZC_TEXT_MAX];

	tip_piece(a, b, buf, sizeof(buf));
	return fn_width(fid, buf);
}

/* The same down the line */
LOCAL INT tip_vwidth( ID fid, INT a, INT b )
{
	UB	buf[TSMOZC_TEXT_MAX];

	tip_piece(a, b, buf, sizeof(buf));
	return fn_vwidth(fid, buf);
}

/* A number as its digits; answers how many */
LOCAL INT tip_num( INT v, UB *out )
{
	UB	t[12];
	INT	m = 0, k = 0;

	do {
		t[m++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && m < 11 );
	while ( m > 0 ) {
		out[k++] = t[--m];
	}
	out[k] = 0;
	return k;
}

/* The candidates, under the composition (or over it near the foot) */
LOCAL void tip_draw_list( INT gid, ID fid, INT x, INT top, INT bottom,
			  CONST T_DPRECT *page )
{
	INT		i, at = 0, w = 0, h, px = 14;
	T_FNMET		met;
	T_DPRECT	b;
	UB		label[TSMOZC_TEXT_MAX + 8];

	(void)fn_set_size(fid, px);
	if ( fn_metrics(fid, &met) < E_OK ) {
		met.ascent = px;
		met.descent = px / 4;
	}
	tip_row_h = met.ascent + met.descent + 4;
	for ( i = 0; i < tip_ncand; i++ ) {
		INT	n = 0, lw;

		while ( tip_cand[at + n] != 0 ) n++;
		lw = fn_width(fid, tip_cand + at);
		if ( lw > w ) w = lw;
		at += n + 1;
	}
	w += 40;
	h = tip_ncand * tip_row_h + 4 + tip_row_h;
	b.left = x;
	b.top = bottom + 2;
	if ( b.top + h > page->bottom && top - h - 2 >= page->top ) {
		b.top = top - h - 2;
	}
	b.right = b.left + w;
	b.bottom = b.top + h;
	if ( b.right > page->right && page->right - w > page->left ) {
		b.left = page->right - w;
		b.right = page->right;
	}
	tip_list_box = b;
	dp_fill_rect(gid, &b, TIP_GROUND);
	dp_frame_rect(gid, &b, TIP_FRAME, 1);
	at = 0;
	for ( i = 0; i < tip_ncand; i++ ) {
		INT		y = b.top + 2 + i * tip_row_h, n = 0, k = 0;
		BOOL		chosen = (BOOL)( tip_first + i == tip_sel );
		T_DPRECT	r;

		while ( tip_cand[at + n] != 0 ) n++;
		label[k++] = (UB)( '1' + i );
		label[k++] = ' ';
		for ( ; k < n + 2 && k < (INT)sizeof(label) - 1; k++ ) {
			label[k] = tip_cand[at + k - 2];
		}
		label[k] = 0;
		if ( chosen ) {
			r.left = b.left + 1;  r.right = b.right - 1;
			r.top = y;  r.bottom = y + tip_row_h;
			dp_fill_rect(gid, &r, TIP_TARGET);
		}
		(void)fn_draw(gid, fid, b.left + 6, y + 2 + met.ascent, label,
			      chosen ? TIP_GROUND : TIP_INK);
		at += n + 1;
	}
	/* which of how many */
	{
		UB	num[32];
		INT	k;

		k = tip_num(tip_sel, num);
		num[k++] = '/';
		(void)tip_num(tip_total, num + k);
		(void)fn_set_size(fid, 11);
		(void)fn_draw(gid, fid, b.right - 6 - fn_width(fid, num),
			      b.bottom - 4, num, TIP_FRAME);
	}
}

/*
 * The composition down the line (縦書き): from the caret downwards in
 * the vertical forms, on white, each clause lined on its left side, the
 * one being converted turned over, the caret a bar across; the
 * candidates under the end of it.
 */
LOCAL void tip_draw_v( DTWIN *d, INT gid, ID fid, CONST T_DPRECT *box,
		       CONST T_DPRECT *page )
{
	T_KCOUT		*o = tip_out;
	T_DPRECT	b;
	INT		px, cx, y, c, end;

	/* the size of the letters it goes in among, no wider than the line */
	px = dd_caret_px(d);
	if ( px > ( box->right - box->left ) ) px = box->right - box->left;
	if ( px < 10 ) px = 10;
	(void)fn_set_size(fid, px);
	(void)fn_set_style(fid, FN_ST_VERT, 0);
	end = o->cl[o->n_cl];
	b.left = box->left;
	b.right = box->right;
	b.top = box->top;
	b.bottom = box->top + tip_vwidth(fid, tip_base, end) + 2;
	dp_fill_rect(gid, &b, TIP_GROUND);
	cx = ( b.left + b.right ) / 2;

	y = b.top + 1;
	for ( c = o->n_out; c < o->n_cl; c++ ) {
		INT		h = tip_vwidth(fid, o->cl[c], o->cl[c + 1]);
		BOOL		target = (BOOL)( c == o->clause
					&& ( tip_conv || tip_list || o->n_cl - o->n_out > 1 ) );
		UB		buf[TSMOZC_TEXT_MAX];
		T_DPRECT	u;

		tip_piece(o->cl[c], o->cl[c + 1], buf, sizeof(buf));
		if ( target ) {
			u.left = b.left;  u.right = b.right;
			u.top = y;  u.bottom = y + h;
			dp_fill_rect(gid, &u, TIP_TARGET);
		}
		(void)fn_vdraw(gid, fid, cx, y, buf, target ? TIP_GROUND : TIP_INK);
		/* each clause its own line at its left, a gap between them */
		u.top = y + 1;  u.bottom = y + h - 1;
		u.left = b.left;  u.right = b.left + ( target ? 2 : 1 );
		dp_fill_rect(gid, &u, target ? TIP_TARGET : TIP_INK);
		y += h;
	}
	if ( !tip_conv && !tip_list && o->caret >= tip_base ) {
		INT		cy = b.top + 1 + tip_vwidth(fid, tip_base, o->caret);
		T_DPRECT	k;

		k.left = b.left;  k.right = b.right;
		k.top = cy;  k.bottom = cy + 2;
		dp_fill_rect(gid, &k, 0x00D00000U);
	}
	if ( tip_list ) {
		tip_draw_list(gid, fid, b.left, b.top, b.bottom, page);
	}
}

/*
 * The composition, drawn over the text at the caret: on white, each
 * clause underlined, the one being converted turned over, and the
 * caret. Called after the window's text and caret are drawn.
 */
EXPORT void tip_draw( DTWIN *d, INT gid )
{
	T_DPRECT	page, box, b;
	ID		fid = fn_system();
	INT		px, x, base, c, end;
	T_FNMET		met;
	T_KCOUT		*o = tip_out;

	if ( !tip_busy(d) || gid < 0 || fid <= 0 || d->doc == NULL ) {
		return;
	}
	dt_work_rect(d, &page);
	if ( tv_doc_caret_box(d->doc, &page, d->scroll_y, d->rec, d->dd.cpara,
			      d->dd.cpos, &box) < E_OK ) {
		return;
	}
	if ( d->doc->vert ) {
		tip_draw_v(d, gid, fid, &box, &page);
		return;
	}
	px = ( box.bottom - box.top ) * 3 / 4;
	if ( px < 10 ) px = 10;
	(void)fn_set_size(fid, px);
	if ( fn_metrics(fid, &met) < E_OK ) {
		met.ascent = px;
		met.descent = px / 4;
	}
	end = o->cl[o->n_cl];
	b.left = box.left;
	b.top = box.top;
	b.right = box.left + tip_width(fid, tip_base, end) + 2;
	b.bottom = box.bottom;
	dp_fill_rect(gid, &b, TIP_GROUND);
	base = b.bottom - ( b.bottom - b.top - met.ascent - met.descent ) / 2 - met.descent;

	x = b.left + 1;
	for ( c = o->n_out; c < o->n_cl; c++ ) {
		INT		w = tip_width(fid, o->cl[c], o->cl[c + 1]);
		BOOL		target = (BOOL)( c == o->clause
					&& ( tip_conv || tip_list || o->n_cl - o->n_out > 1 ) );
		UB		buf[TSMOZC_TEXT_MAX];
		T_DPRECT	u;

		tip_piece(o->cl[c], o->cl[c + 1], buf, sizeof(buf));
		if ( target ) {
			u.left = x;  u.right = x + w;
			u.top = b.top;  u.bottom = b.bottom;
			dp_fill_rect(gid, &u, TIP_TARGET);
		}
		(void)fn_draw(gid, fid, x, base, buf, target ? TIP_GROUND : TIP_INK);
		/* each clause its own underline, a gap between them */
		u.left = x + 1;  u.right = x + w - 1;
		u.top = b.bottom - 2;  u.bottom = b.bottom - ( target ? 0 : 1 );
		dp_fill_rect(gid, &u, target ? TIP_TARGET : TIP_INK);
		x += w;
	}
	/* the caret in the composition, while it is being typed */
	if ( !tip_conv && !tip_list && o->caret >= tip_base ) {
		INT		cx = b.left + 1 + tip_width(fid, tip_base, o->caret);
		T_DPRECT	k;

		k.left = cx;  k.right = cx + 2;
		k.top = b.top;  k.bottom = b.bottom;
		dp_fill_rect(gid, &k, 0x00D00000U);
	}
	if ( tip_list ) {
		tip_draw_list(gid, fid, b.left, b.top, b.bottom, &page);
	}
}

