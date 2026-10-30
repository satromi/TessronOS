/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_edit.c
 *	基本表計算: a cell's words typed (design 11.13)
 *
 *	A cell is typed into where it is: a box over it, as wide as the
 *	words need, with the caret. Typing on a cell taken begins with what
 *	is typed; Enter, F2 or a second press begins with what the cell holds
 *	(its formula, or its words), all of it taken, so that the first
 *	letter typed replaces it. Enter puts what is typed in the cell and
 *	takes the cell below, Tab the cell to the right; Esc leaves the cell
 *	as it was.
 *
 *	The keys go to a session of the system's かな漢字変換 in the input
 *	mode the message line shows: what it commits goes in at the caret,
 *	what it is still working on is drawn at the caret, each clause
 *	underlined and the one being converted turned over. While candidates
 *	are chosen the system lists them under it in a window of its own
 *	(OB_WR_CAND); a digit, or a press on a name there, takes one. In 英語 a letter
 *	goes in as it is, half width whatever the mode says of width: a
 *	number or a formula is typed in the letters a formula is read in.
 *
 *	The same words can be typed in the box of the 操作パネル instead, a
 *	box of the panel's own (calc_panel.c); while a cell is typed into
 *	where it is, the box shows the same words (ed_text).
 */

#include "calc.h"
#include <ts/hid.h>
#include <ts/wm.h>

#define LIST_MAX	4096
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U

static BOOL	on;
static INT	ecol, erow;
/*
 * What is typed, as long as it is: made larger as it grows. 'tmp' is
 * room for a part of it to be measured or drawn.
 */
static UB	*buf = NULL, *tmp = NULL;
static INT	bufmax = 0, tmpmax = 0;
static INT	len, caret;

static BOOL buf_room( INT n )
{
	UB	*p;
	INT	m;

	if ( n + 1 <= bufmax ) return TRUE;
	m = ( bufmax > 0 ) ? bufmax * 2 : 256;
	if ( m < n + 1 ) m = n + 1;
	p = realloc(buf, (size_t)m);
	if ( p == NULL ) return FALSE;
	if ( buf == NULL ) p[0] = 0;
	buf = p;
	bufmax = m;
	return TRUE;
}

/* Room for n letters and their end, apart from what is typed */
static UB *tmp_room( INT n )
{
	UB	*p;

	if ( n + 1 <= tmpmax ) return tmp;
	p = realloc(tmp, (size_t)( n + 1 ));
	if ( p == NULL ) return NULL;
	tmp = p;
	tmpmax = n + 1;
	return tmp;
}
static BOOL	all;			/* everything is taken: the next letter replaces it */

static INT	kid;
static T_KCOUT	out;
static UINT	kmode = WM_MODE_ROMAN;	/* the input mode what is being composed was typed in */
static INT	base;
static BOOL	list;
static UB	cand[LIST_MAX];
static INT	ncand, first, total, chosen;
static BOOL	listed = FALSE;		/* the system lists them for the sheet's window */


BOOL ed_on( void )
{
	return on;
}

static BOOL busy( void )
{
	return (BOOL)( kid > 0 && out.cl[out.n_cl] > base );
}

/* UTF-8 put in at the caret */
static void insert( CONST UB *s, INT n )
{
	if ( all ) {
		len = caret = 0;
		all = FALSE;
	}
	if ( !buf_room(len + n) ) {
		say("メモリが足りないため、入力できません");
		return;
	}
	memmove(buf + caret + n, buf + caret, (size_t)( len - caret ));
	memcpy(buf + caret, s, (size_t)n);
	len += n;
	caret += n;
	buf[len] = 0;
	g_dirty = TRUE;
}

static void take( INT r )
{
	base = 0;
	if ( out.n_out > 0 ) {
		INT	end = out.cl[out.n_out];

		if ( end > 0 ) insert((CONST UB *)out.text, end);
		base = end;
	}
	list = FALSE;
	if ( ( r & TSMOZC_LIST ) != 0 && busy() ) {
		INT	sel = kc_list(kid, cand, LIST_MAX, &ncand, &first, &total);

		if ( sel > 0 && ncand > 0 ) {
			list = TRUE;
			chosen = sel;
		}
	}
	g_dirty = TRUE;
}

/* A letter as it is */
static BOOL plain( UB ch )
{
	if ( ch < 0x20 || ch >= 0x7F ) return FALSE;
	insert(&ch, 1);
	return TRUE;
}

/* Whatever is being composed, committed */
static void commit_ime( void )
{
	INT	r;

	if ( !busy() ) {
		list = FALSE;
		return;
	}
	r = kc_hid_key(kid, K_ENTER, HID_MOD_LSHIFT, kmode, &out);
	if ( r >= 0 && r != KC_NOTMINE ) take(r);
	base = out.cl[out.n_cl];
	list = FALSE;
}

void ed_begin( INT col, INT row, CONST UB *first_letters, BOOL keep )
{
	CONST CXCELL	*c = cx_get(&g_s, col, row);

	on = TRUE;
	ecol = col;
	erow = row;
	len = caret = 0;
	all = FALSE;
	if ( !buf_room(0) ) {
		on = FALSE;
		warn("入力を始められませんでした", "メモリが足りません");
		return;
	}
	buf[0] = 0;
	if ( keep && c != NULL && c->kind == CX_TEXT ) {
		/* the whole of what it holds, however long */
		INT	n = (INT)strlen((CONST char *)c->text);

		if ( !buf_room(n) ) {
			on = FALSE;
			warn("セルを編集できませんでした", "メモリが足りません。セルは元のままです");
			return;
		}
		memcpy(buf, c->text, (size_t)n + 1);
		len = caret = n;
		all = TRUE;
	}
	if ( first_letters != NULL ) insert(first_letters, (INT)strlen((CONST char *)first_letters));
	if ( kid <= 0 ) {
		kid = kc_open();
		if ( kid <= 0 ) kid = 0;
	}
	memset(&out, 0, sizeof(out));
	base = 0;
	list = FALSE;
	scroll_to(col, row);
	g_dirty = TRUE;
}

/* The system's list of candidates put away */
static void unlist( void )
{
	if ( listed ) (void)ob_cand_put(g_kw, 0, 0, 0, NULL, 0, -1, 0, 0);
	listed = FALSE;
}

void ed_commit( void )
{
	if ( !on ) return;
	commit_ime();
	unlist();
	on = FALSE;
	buf[len] = 0;
	cell_text(ecol, erow, buf);
	g_dirty = TRUE;
}

void ed_cancel( void )
{
	if ( !on ) return;
	if ( busy() ) (void)kc_hid_key(kid, K_ESC, 0, kmode, &out);
	memset(&out, 0, sizeof(out));
	base = 0;
	list = FALSE;
	unlist();
	on = FALSE;
	g_dirty = TRUE;
}

void ed_end( void )
{
	unlist();
	if ( kid > 0 ) kc_close(kid);
	kid = 0;
}

/* The caret moved over one letter either way */
static INT step( INT at, INT d )
{
	if ( d < 0 ) {
		if ( at <= 0 ) return 0;
		do {
			at--;
		} while ( at > 0 && ( buf[at] & 0xC0 ) == 0x80 );
		return at;
	}
	if ( at >= len ) return len;
	do {
		at++;
	} while ( at < len && ( buf[at] & 0xC0 ) == 0x80 );
	return at;
}

/*
 * A key, in the input mode it came in (T_OBNTM imode). A key in 英語
 * that comes while something is being composed -- 英数 itself, or one
 * typed after it -- commits that first, then is 英語's.
 */
BOOL ed_key( UINT key, UINT mods, UINT mode )
{
	UB	ch = wm_key_char(key, mods);
	INT	r;

	if ( !on ) return FALSE;
	if ( kid > 0 && ( mode & WM_MODE_ALPH ) != 0 && busy() ) {
		commit_ime();
	}
	if ( ( mods & M_CTRL ) != 0 ) return FALSE;

	/* the converter first, while it is composing */
	if ( kid > 0 && ( mode & WM_MODE_ALPH ) == 0 ) {
		if ( list && ch >= '1' && ch <= '9' && ch - '1' < ncand ) {
			r = kc_choose(kid, first + ( ch - '1' ), &out);
			if ( r >= 0 && r != KC_NOTMINE ) take(r);
			return TRUE;
		}
		if ( busy() || ( ch > 0x20 && ch < 0x7F ) ) {
			kmode = mode;
			r = kc_hid_key(kid, key, mods, mode, &out);
			if ( r >= 0 && r != KC_NOTMINE ) {
				take(r);
				return TRUE;
			}
			if ( busy() ) return TRUE;
		}
	}
	switch ( key ) {
	case K_ENTER:
	case K_KPENTER:
		ed_commit();
		if ( erow < CX_ROWS_MAX ) g_row = g_arow = erow + 1;
		g_col = g_acol = ecol;
		cx_grow(&g_s, g_col, g_row);
		scroll_to(g_col, g_row);
		return TRUE;
	case K_TAB:
		ed_commit();
		if ( ecol < CX_COLS_MAX ) g_col = g_acol = ecol + 1;
		g_row = g_arow = erow;
		cx_grow(&g_s, g_col, g_row);
		scroll_to(g_col, g_row);
		return TRUE;
	case K_ESC:
		ed_cancel();
		return TRUE;
	case K_BS:
		if ( all ) {
			len = caret = 0;
			all = FALSE;
		} else if ( caret > 0 ) {
			INT	at = step(caret, -1);

			memmove(buf + at, buf + caret, (size_t)( len - caret ));
			len -= caret - at;
			caret = at;
		}
		buf[len] = 0;
		g_dirty = TRUE;
		return TRUE;
	case K_DEL:
		if ( all ) {
			len = caret = 0;
			all = FALSE;
		} else if ( caret < len ) {
			INT	at = step(caret, 1);

			memmove(buf + caret, buf + at, (size_t)( len - at ));
			len -= at - caret;
		}
		buf[len] = 0;
		g_dirty = TRUE;
		return TRUE;
	case K_LEFT:
		all = FALSE;
		caret = step(caret, -1);
		g_dirty = TRUE;
		return TRUE;
	case K_RIGHT:
		all = FALSE;
		caret = step(caret, 1);
		g_dirty = TRUE;
		return TRUE;
	case K_HOME:
		all = FALSE;
		caret = 0;
		g_dirty = TRUE;
		return TRUE;
	case K_END:
		all = FALSE;
		caret = len;
		g_dirty = TRUE;
		return TRUE;
	default:
		break;
	}
	if ( ( mode & WM_MODE_ALPH ) != 0 && ( mode & WM_MODE_KANA ) != 0
	  && ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ) ) {
		ch ^= 0x20;		/* 英語 in capitals: the shift turns it back */
	}
	if ( ch >= 0x20 && ch < 0x7F ) return plain(ch);
	return TRUE;
}

void ed_pick( INT row )
{
	INT	r;

	if ( on && list && kid > 0 && row >= 0 && row < ncand ) {
		r = kc_choose(kid, first + row, &out);
		if ( r >= 0 && r != KC_NOTMINE ) take(r);
		g_dirty = TRUE;
	}
}

static void piece( INT a, INT b, UB *dst, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) dst[n++] = (UB)out.text[a];
	dst[n] = 0;
}

/* The composition at (x, y) in the environment gid, its letters px tall in a line lh high */
static void ime_text( INT gid, INT x, INT y, INT px, INT lh )
{
	UB		s[TSMOZC_TEXT_MAX];
	T_DPRECT	r;
	INT		i, cx = x, end;

	if ( !busy() ) return;
	end = out.cl[out.n_cl];
	piece(base, end, s, sizeof(s));
	r.left = x;
	r.top = y;
	r.right = x + dp_text_width(s, px) + 2;
	r.bottom = y + lh;
	(void)dp_fill_rect(gid, &r, IME_GROUND);
	for ( i = out.n_out; i < out.n_cl; i++ ) {
		INT		a = out.cl[i], b = out.cl[i + 1], w;
		BOOL		target = (BOOL)( i == out.clause && !out.yomi );
		T_DPRECT	u;

		if ( b <= base ) continue;
		if ( a < base ) a = base;
		piece(a, b, s, sizeof(s));
		w = dp_text_width(s, px);
		if ( target ) {
			u.left = cx;
			u.top = y;
			u.right = cx + w;
			u.bottom = y + lh;
			(void)dp_fill_rect(gid, &u, IME_TARGET);
		}
		(void)dp_text(gid, cx, y + px + 1, s, target ? IME_GROUND : IME_INK, px);
		u.left = cx + 1;
		u.right = cx + w - 1;
		u.top = y + lh - 2;
		u.bottom = y + lh - 1;
		(void)dp_fill_rect(gid, &u, IME_INK);
		cx += w;
	}
}

/* The candidates, listed by the system under the box from x across, top to bottom */
static void ime_list( INT x, INT top, INT bottom )
{
	if ( !list ) {
		unlist();
	} else if ( ob_cand_put(g_kw, x, top, bottom, cand, ncand, chosen - first, first, total) >= E_OK ) {
		listed = TRUE;
	}
}

void ed_draw( void )
{
	T_DPRECT	r, box;
	UB		*head;
	INT		w, cx, px = 12, y;

	if ( !on || ( head = tmp_room(len) ) == NULL ) {
		unlist();
		return;
	}
	(void)cell_box(ecol, erow, &r);
	w = dp_text_width(buf, px) + 12;
	if ( busy() ) {
		UB	s[TSMOZC_TEXT_MAX];

		piece(base, out.cl[out.n_cl], s, sizeof(s));
		w += dp_text_width(s, px);
	}
	box = r;
	if ( box.right - box.left < w ) box.right = box.left + w;
	(void)dp_fill_rect(g_gid, &box, all ? C_CUR_FILL : C_PAPER);
	(void)dp_frame_rect(g_gid, &box, C_PICK, 2);
	y = r.top + ( r.bottom - r.top + px ) / 2 - px / 8;
	(void)dp_text(g_gid, box.left + 4, y, buf, C_TEXT, px);
	memcpy(head, buf, (size_t)caret);
	head[caret] = 0;
	cx = box.left + 4 + dp_text_width(head, px);
	if ( busy() ) {
		ime_text(g_gid, cx, r.top + 2, px, r.bottom - r.top - 4);
		ime_list(cx, r.top, r.bottom);
	} else {
		T_DPRECT	c;

		unlist();
		c.left = cx;
		c.right = cx + 1;
		c.top = r.top + 3;
		c.bottom = r.bottom - 3;
		(void)dp_fill_rect(g_gid, &c, C_TEXT);
	}
}

/* ---------------------------------------------------------------- what the panel's box shows */

INT ed_text( UB *out, INT max )
{
	INT	n = 0;

	if ( out == NULL || max <= 0 ) return 0;
	if ( on && buf != NULL ) {
		n = ( len < max - 1 ) ? len : max - 1;
		memcpy(out, buf, (size_t)n);
	}
	out[n] = 0;
	return n;
}
