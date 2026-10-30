/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_edit.c
 *	マイクロカード: a box typed into (design 11.18)
 *
 *	One box of the card window at a time is typed into: a field's value,
 *	its condition, a name or size of 設計画面, or the place of the card
 *	shown. What it holds is drawn over the box from a letter that keeps
 *	the caret in sight. Begun with what the box held, all of it is
 *	taken, so that the first letter typed replaces it.
 *
 *	The keys go to a session of the system's かな漢字変換 in the input
 *	mode the message line shows: what it commits goes in at the caret,
 *	what it is still working on is drawn at the caret, each clause
 *	underlined and the one being converted turned over. While candidates
 *	are chosen the system lists them under the box in a window of its
 *	own (OB_WR_CAND); a digit, or a press on a name there, takes one.
 *	The keys that move
 *	between boxes (Enter, Tab, the arrows up and down) are the card
 *	window's: it puts what is typed in the box and goes on.
 */

#include "mc.h"
#include <ts/kconv.h>

#define TEXT_MAX	( MC_VALUE_MAX + 64 )
#define LIST_MAX	4096
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U

static BOOL	on;
static INT	ekind, eidx;
static UB	buf[TEXT_MAX];
static INT	len, caret;
static BOOL	all;			/* everything is taken: the next letter replaces it */
static INT	shown_from;

static INT	kid;
static T_KCOUT	out;
static UINT	kmode = WM_MODE_ROMAN;	/* the input mode what is being composed was typed in */
static INT	base;
static BOOL	list;
static UB	cand[LIST_MAX];
static INT	ncand, first, total, chosen;
static BOOL	listed = FALSE;		/* the system lists them for the card window */

BOOL ed_on( void )	{ return on; }
INT  ed_kind( void )	{ return on ? ekind : ED_NONE; }
INT  ed_index( void )	{ return on ? eidx : -1; }

static BOOL busy( void )
{
	return (BOOL)( kid > 0 && out.cl[out.n_cl] > base );
}

static void dirty( void )
{
	g_cw.dirty = TRUE;
}

void ed_insert( CONST UB *s, INT n )
{
	if ( all ) {
		len = caret = 0;
		all = FALSE;
	}
	if ( len + n >= TEXT_MAX - 1 ) n = TEXT_MAX - 1 - len;
	if ( n <= 0 ) return;
	memmove(buf + caret + n, buf + caret, (size_t)( len - caret ));
	memcpy(buf + caret, s, (size_t)n);
	len += n;
	caret += n;
	buf[len] = 0;
	dirty();
}

static void take( INT r )
{
	base = 0;
	if ( out.n_out > 0 ) {
		INT	end = out.cl[out.n_out];

		if ( end > 0 ) ed_insert((CONST UB *)out.text, end);
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
	dirty();
}

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

void ed_begin( INT kind, INT k, CONST UB *text, BOOL take_all )
{
	on = TRUE;
	ekind = kind;
	eidx = k;
	len = caret = 0;
	shown_from = 0;
	buf[0] = 0;
	all = FALSE;
	if ( text != NULL ) {
		INT	n = (INT)strlen((CONST char *)text);

		if ( n > TEXT_MAX - 1 ) n = TEXT_MAX - 1;
		memcpy(buf, text, (size_t)n);
		buf[n] = 0;
		len = caret = n;
		all = (BOOL)( take_all && n > 0 );
	}
	if ( kid <= 0 ) {
		kid = kc_open();
		if ( kid <= 0 ) kid = 0;
	}
	memset(&out, 0, sizeof(out));
	base = 0;
	list = FALSE;
	dirty();
}

INT ed_text( UB *o, INT max )
{
	INT	n = ( len < max - 1 ) ? len : max - 1;

	memcpy(o, buf, (size_t)n);
	o[n] = 0;
	return n;
}

/* The system's list of candidates put away */
static void unlist( void )
{
	if ( listed ) (void)ob_cand_put(g_cw.kw, 0, 0, 0, NULL, 0, -1, 0, 0);
	listed = FALSE;
}

void ed_commit( void )
{
	INT	kind = ekind, k = eidx;

	if ( !on ) return;
	commit_ime();
	unlist();
	on = FALSE;
	buf[len] = 0;
	card_commit(kind, k, buf, len);
	dirty();
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
	dirty();
}

void ed_end( void )
{
	unlist();
	if ( kid > 0 ) kc_close(kid);
	kid = 0;
}

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
	if ( ( mods & ( M_CTRL | M_ALT ) ) != 0 ) {
		/* Ctrl and Alt are the window's, unless the converter is busy */
		if ( !busy() ) return FALSE;
	}
	if ( kid > 0 && ( mode & WM_MODE_ALPH ) == 0 ) {
		if ( list && ch >= '1' && ch <= '9' && ch - '1' < ncand ) {
			r = kc_choose(kid, first + ( ch - '1' ), &out);
			if ( r >= 0 && r != KC_NOTMINE ) take(r);
			return TRUE;
		}
		if ( busy() || ( ch > 0x20 && ch < 0x7F && ( mods & ( M_CTRL | M_ALT ) ) == 0 ) ) {
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
		dirty();
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
		dirty();
		return TRUE;
	case K_LEFT:
		all = FALSE;
		caret = step(caret, -1);
		dirty();
		return TRUE;
	case K_RIGHT:
		all = FALSE;
		caret = step(caret, 1);
		dirty();
		return TRUE;
	case K_HOME:
		all = FALSE;
		caret = 0;
		dirty();
		return TRUE;
	case K_END:
		all = FALSE;
		caret = len;
		dirty();
		return TRUE;
	default:
		break;
	}
	if ( ( mode & WM_MODE_ALPH ) != 0 && ( mode & WM_MODE_KANA ) != 0
	  && ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ) ) {
		ch ^= 0x20;		/* 英語 in capitals: the shift turns it back */
	}
	if ( ch >= 0x20 && ch < 0x7F ) {
		ed_insert(&ch, 1);
		return TRUE;
	}
	return FALSE;
}

void ed_pick( INT row )
{
	INT	r;

	if ( on && list && kid > 0 && row >= 0 && row < ncand ) {
		r = kc_choose(kid, first + row, &out);
		if ( r >= 0 && r != KC_NOTMINE ) take(r);
		dirty();
	}
}

static INT width_of( INT a, INT b, INT px )
{
	UB	s[TEXT_MAX];

	if ( b <= a ) return 0;
	memcpy(s, buf + a, (size_t)( b - a ));
	s[b - a] = 0;
	return dp_text_width(s, px);
}

void ed_caret_at( INT x )
{
	T_DPRECT	r;
	INT		at, px = card_px();

	if ( !on || !card_box(ekind, eidx, &r) ) return;
	commit_ime();
	x -= r.left + 4;
	for ( at = shown_from; at < len; ) {
		INT	next = step(at, 1);

		if ( x < ( width_of(shown_from, at, px) + width_of(shown_from, next, px) ) / 2 ) break;
		at = next;
	}
	caret = at;
	all = FALSE;
	dirty();
}

static void piece( INT a, INT b, UB *dst, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) dst[n++] = (UB)out.text[a];
	dst[n] = 0;
}

static void ime_text( INT x, INT y, INT px, INT lh )
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
	(void)dp_fill_rect(g_cw.gid, &r, IME_GROUND);
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
			(void)dp_fill_rect(g_cw.gid, &u, IME_TARGET);
		}
		(void)dp_text(g_cw.gid, cx, y + px + 1, s, target ? IME_GROUND : IME_INK, px);
		u.left = cx + 1;
		u.right = cx + w - 1;
		u.top = y + lh - 2;
		u.bottom = y + lh - 1;
		(void)dp_fill_rect(g_cw.gid, &u, IME_INK);
		cx += w;
	}
}

void ed_draw( void )
{
	T_DPRECT	r, c;
	UB		s[TEXT_MAX];
	INT		px = card_px(), w, end, cx, y;

	if ( !on || g_cw.gid < 0 || !card_box(ekind, eidx, &r) ) {
		unlist();
		return;
	}
	w = r.right - r.left - 8;
	if ( shown_from > caret ) shown_from = caret;
	while ( shown_from < caret && width_of(shown_from, caret, px) > w - 4 ) shown_from = step(shown_from, 1);
	end = shown_from;
	while ( end < len ) {
		INT	next = step(end, 1);

		if ( width_of(shown_from, next, px) > w ) break;
		end = next;
	}
	(void)dp_fill_rect(g_cw.gid, &r, all ? C_ROW : C_PAPER);
	(void)dp_frame_rect(g_cw.gid, &r, C_PICK, 2);
	memcpy(s, buf + shown_from, (size_t)( end - shown_from ));
	s[end - shown_from] = 0;
	y = r.top + ( r.bottom - r.top + px ) / 2 - px / 8;
	(void)dp_text(g_cw.gid, r.left + 4, y, s, C_TEXT, px);
	cx = r.left + 4 + width_of(shown_from, caret, px);
	if ( busy() ) {
		ime_text(cx, r.top + 2, px, r.bottom - r.top - 4);
		/* the candidates, listed by the system under the box */
		if ( !list ) {
			unlist();
		} else if ( ob_cand_put(g_cw.kw, cx, r.top, r.bottom, cand, ncand, chosen - first, first, total) >= E_OK ) {
			listed = TRUE;
		}
	} else {
		unlist();
		c.left = cx;
		c.right = cx + 1;
		c.top = r.top + 3;
		c.bottom = r.bottom - 3;
		(void)dp_fill_rect(g_cw.gid, &c, C_TEXT);
	}
}
