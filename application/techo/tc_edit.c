/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tc_edit.c
 *	電子手帳: a 記入欄 typed into (design 11.23)
 *
 *	One 記入欄 of the three windows at a time is typed into. What it
 *	holds is drawn over the field from a letter that keeps the caret in
 *	sight; it is one line, and the keys that would make another (Enter,
 *	Tab) are the window's, which put what was typed in the field and go
 *	on. Letters are taken with Shift and the arrows, or by carrying the
 *	pointer over them; what is taken is replaced by what is typed, and
 *	goes to the tray or is carried to another field (tc_main.c).
 *
 *	The keys go to a session of the system's かな漢字変換 in the input
 *	mode the message line shows: what it commits goes in at the caret,
 *	what it is still working on is drawn at the caret, each clause
 *	underlined and the one being converted turned over. While candidates
 *	are chosen the system lists them under the field in a window of its
 *	own (OB_WR_CAND); a digit, or a press on a name there, takes one.
 */

#include "tc.h"
#include <ts/kconv.h>

#define TEXT_MAX	TC_LINE_MAX
#define LIST_MAX	4096
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U

static BOOL	on;
static INT	ewin, efield;
static UB	buf[TEXT_MAX];
static INT	len, caret, anchor;
static INT	shown_from;

static INT	kid;
static T_KCOUT	out;
static UINT	kmode = WM_MODE_ROMAN;	/* the input mode what is being composed was typed in */
static INT	base;
static BOOL	list;
static UB	cand[LIST_MAX];
static INT	ncand, first, total, chosen;
static ID	listed = 0;		/* the window the system lists them for, 0 none */

BOOL ed_on( void )	{ return on; }
INT  ed_win( void )	{ return on ? ewin : -1; }
INT  ed_field( void )	{ return on ? efield : -1; }

BOOL ed_busy( void )
{
	return (BOOL)( kid > 0 && out.cl[out.n_cl] > base );
}

static void dirty( void )
{
	if ( ewin >= 0 && ewin < W_N ) g_w[ewin].dirty = TRUE;
}

/* The system's list of candidates put away */
static void unlist( void )
{
	if ( listed > 0 ) (void)ob_cand_put(listed, 0, 0, 0, NULL, 0, -1, 0, 0);
	listed = 0;
}

static INT sel_lo( void )	{ return ( anchor < caret ) ? anchor : caret; }
static INT sel_hi( void )	{ return ( anchor < caret ) ? caret : anchor; }

BOOL ed_has_sel( void )
{
	return (BOOL)( on && anchor != caret );
}

void ed_sel_delete( void )
{
	INT	a = sel_lo(), b = sel_hi();

	if ( a == b ) return;
	memmove(buf + a, buf + b, (size_t)( len - b ));
	len -= b - a;
	caret = anchor = a;
	buf[len] = 0;
	dirty();
}

INT ed_sel_text( UB *o, INT max )
{
	INT	a = sel_lo(), n = sel_hi() - a;

	if ( !on || max <= 0 ) return 0;
	if ( n > max - 1 ) n = max - 1;
	memcpy(o, buf + a, (size_t)n);
	o[n] = 0;
	return n;
}

void ed_select_all( void )
{
	if ( !on ) return;
	anchor = 0;
	caret = len;
	dirty();
}

void ed_insert( CONST UB *s, INT n )
{
	INT	i, k = 0;
	UB	t[TEXT_MAX];

	if ( !on ) return;
	/* one line: a tab or a line end in what comes is a space, or ends it */
	for ( i = 0; i < n && k < TEXT_MAX - 1; i++ ) {
		if ( s[i] == '\n' || s[i] == '\r' ) break;
		t[k++] = ( s[i] == '\t' ) ? ' ' : s[i];
	}
	n = k;
	ed_sel_delete();
	if ( len + n >= TEXT_MAX - 1 ) n = TEXT_MAX - 1 - len;
	while ( n > 0 && ( t[n] & 0xC0 ) == 0x80 && n < k ) n--;	/* not in the middle of a letter */
	if ( n <= 0 ) return;
	memmove(buf + caret + n, buf + caret, (size_t)( len - caret ));
	memcpy(buf + caret, t, (size_t)n);
	len += n;
	caret += n;
	anchor = caret;
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
	if ( ( r & TSMOZC_LIST ) != 0 && ed_busy() ) {
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

	if ( !ed_busy() ) {
		list = FALSE;
		return;
	}
	r = kc_hid_key(kid, K_ENTER, HID_MOD_LSHIFT, kmode, &out);
	if ( r >= 0 && r != KC_NOTMINE ) take(r);
	base = out.cl[out.n_cl];
	list = FALSE;
}

void ed_begin( INT w, INT f, CONST UB *text )
{
	on = TRUE;
	ewin = w;
	efield = f;
	len = caret = anchor = 0;
	shown_from = 0;
	buf[0] = 0;
	if ( text != NULL ) {
		INT	n = (INT)strlen((CONST char *)text);

		if ( n > TEXT_MAX - 1 ) n = TEXT_MAX - 1;
		memcpy(buf, text, (size_t)n);
		buf[n] = 0;
		len = caret = anchor = n;
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

void ed_commit( void )
{
	INT	w = ewin, f = efield;

	if ( !on ) return;
	commit_ime();
	unlist();
	on = FALSE;
	buf[len] = 0;
	field_commit(w, f, buf, len);
	dirty();
}

void ed_cancel( void )
{
	if ( !on ) return;
	if ( ed_busy() ) (void)kc_hid_key(kid, K_ESC, 0, kmode, &out);
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

/* The caret moved; with Shift the letters from the anchor are taken */
static void move_to( INT at, UINT mods )
{
	caret = at;
	if ( ( mods & M_SHIFT ) == 0 ) anchor = caret;
	dirty();
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
	if ( kid > 0 && ( mode & WM_MODE_ALPH ) != 0 && ed_busy() ) {
		commit_ime();
	}
	if ( ( mods & ( M_CTRL | M_ALT ) ) != 0 ) {
		/* Ctrl and Alt are the window's, unless the converter is busy */
		if ( !ed_busy() ) return FALSE;
	}
	if ( kid > 0 && ( mode & WM_MODE_ALPH ) == 0 ) {
		if ( list && ch >= '1' && ch <= '9' && ch - '1' < ncand ) {
			r = kc_choose(kid, first + ( ch - '1' ), &out);
			if ( r >= 0 && r != KC_NOTMINE ) take(r);
			return TRUE;
		}
		if ( ed_busy() || ( ch > 0x20 && ch < 0x7F && ( mods & ( M_CTRL | M_ALT ) ) == 0 ) ) {
			kmode = mode;
			r = kc_hid_key(kid, key, mods, mode, &out);
			if ( r >= 0 && r != KC_NOTMINE ) {
				take(r);
				return TRUE;
			}
			if ( ed_busy() ) return TRUE;
		}
	}
	switch ( key ) {
	case K_ESC:
		ed_cancel();
		return TRUE;
	case K_BS:
		if ( anchor != caret ) {
			ed_sel_delete();
		} else if ( caret > 0 ) {
			INT	at = step(caret, -1);

			memmove(buf + at, buf + caret, (size_t)( len - caret ));
			len -= caret - at;
			caret = anchor = at;
		}
		buf[len] = 0;
		dirty();
		return TRUE;
	case K_DEL:
		if ( anchor != caret ) {
			ed_sel_delete();
		} else if ( caret < len ) {
			INT	at = step(caret, 1);

			memmove(buf + caret, buf + at, (size_t)( len - at ));
			len -= at - caret;
		}
		buf[len] = 0;
		dirty();
		return TRUE;
	case K_LEFT:
		move_to(step(caret, -1), mods);
		return TRUE;
	case K_RIGHT:
		move_to(step(caret, 1), mods);
		return TRUE;
	case K_HOME:
		move_to(0, mods);
		return TRUE;
	case K_END:
		move_to(len, mods);
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

static INT width_of( CONST UB *s, INT a, INT b, INT px )
{
	UB	t[TEXT_MAX];

	if ( b <= a ) return 0;
	memcpy(t, s + a, (size_t)( b - a ));
	t[b - a] = 0;
	return dp_text_width(t, px);
}

INT text_offset( CONST UB *s, INT x, INT px )
{
	INT	n = (INT)strlen((CONST char *)s), at = 0;

	while ( at < n ) {
		INT	next = at;

		do {
			next++;
		} while ( next < n && ( s[next] & 0xC0 ) == 0x80 );
		if ( x < ( width_of(s, 0, at, px) + width_of(s, 0, next, px) ) / 2 ) break;
		at = next;
	}
	return at;
}

void ed_caret_at( INT x, BOOL extend )
{
	T_DPRECT	r;
	INT		at;

	if ( !on || !field_box(ewin, efield, &r) ) return;
	commit_ime();
	x -= r.left + 4;
	for ( at = shown_from; at < len; ) {
		INT	next = step(at, 1);

		if ( x < ( width_of(buf, shown_from, at, FIELD_PX) + width_of(buf, shown_from, next, FIELD_PX) ) / 2 ) break;
		at = next;
	}
	if ( x < 0 ) {
		while ( at > 0 && x < 0 ) {
			at = step(at, -1);
			x += FIELD_PX;
		}
	}
	caret = at;
	if ( !extend ) anchor = caret;
	dirty();
}

INT ed_offset_at( INT x )
{
	T_DPRECT	r;
	INT		at;

	if ( !on || !field_box(ewin, efield, &r) ) return 0;
	x -= r.left + 4;
	for ( at = shown_from; at < len; ) {
		INT	next = step(at, 1);

		if ( x < ( width_of(buf, shown_from, at, FIELD_PX) + width_of(buf, shown_from, next, FIELD_PX) ) / 2 ) break;
		at = next;
	}
	return at;
}

void ed_set_caret( INT at )
{
	if ( !on ) return;
	if ( at < 0 ) at = 0;
	if ( at > len ) at = len;
	while ( at > 0 && at < len && ( buf[at] & 0xC0 ) == 0x80 ) at--;
	caret = anchor = at;
	dirty();
}

INT ed_sel_from( void )
{
	return sel_lo();
}

BOOL ed_in_sel( INT x )
{
	T_DPRECT	r;
	INT		a, b;

	if ( !ed_has_sel() || !field_box(ewin, efield, &r) ) return FALSE;
	a = r.left + 4 + width_of(buf, shown_from, ( sel_lo() > shown_from ) ? sel_lo() : shown_from, FIELD_PX);
	b = r.left + 4 + width_of(buf, shown_from, sel_hi(), FIELD_PX);
	return (BOOL)( x >= a && x <= b );
}

static void piece( INT a, INT b, UB *dst, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) dst[n++] = (UB)out.text[a];
	dst[n] = 0;
}

static void ime_text( INT gid, INT x, INT y, INT px, INT lh )
{
	UB		s[TSMOZC_TEXT_MAX];
	T_DPRECT	r;
	INT		i, cx = x, end;

	if ( !ed_busy() ) return;
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

void ed_draw( void )
{
	T_DPRECT	r, c;
	UB		s[TEXT_MAX];
	INT		px = FIELD_PX, w, end, cx, y, gid;

	if ( !on || ewin < 0 || ewin >= W_N || !g_w[ewin].open || !field_box(ewin, efield, &r) ) return;
	gid = g_w[ewin].gid;
	if ( gid < 0 ) return;
	w = r.right - r.left - 8;
	if ( shown_from > caret ) shown_from = caret;
	while ( shown_from < caret && width_of(buf, shown_from, caret, px) > w - 4 ) shown_from = step(shown_from, 1);
	end = shown_from;
	while ( end < len ) {
		INT	next = step(end, 1);

		if ( width_of(buf, shown_from, next, px) > w ) break;
		end = next;
	}
	(void)dp_fill_rect(gid, &r, C_PAPER);
	if ( anchor != caret ) {
		INT	a = ( sel_lo() > shown_from ) ? sel_lo() : shown_from, b = ( sel_hi() < end ) ? sel_hi() : end;

		if ( b > a ) {
			c.left = r.left + 4 + width_of(buf, shown_from, a, px);
			c.right = r.left + 4 + width_of(buf, shown_from, b, px);
			c.top = r.top + 2;
			c.bottom = r.bottom - 2;
			(void)dp_fill_rect(gid, &c, C_SEL);
		}
	}
	(void)dp_frame_rect(gid, &r, C_PICK, 1);
	memcpy(s, buf + shown_from, (size_t)( end - shown_from ));
	s[end - shown_from] = 0;
	y = r.top + ( r.bottom - r.top + px ) / 2 - px / 8;
	(void)dp_text(gid, r.left + 4, y, s, C_TEXT, px);
	cx = r.left + 4 + width_of(buf, shown_from, caret, px);
	if ( ed_busy() ) {
		ime_text(gid, cx, r.top + 2, px, r.bottom - r.top - 4);
		/* the candidates, listed by the system under the field */
		if ( !list ) {
			unlist();
		} else if ( ob_cand_put(g_w[ewin].kw, cx, r.top, r.bottom, cand, ncand, chosen - first,
					first, total) >= E_OK ) {
			listed = g_w[ewin].kw;
		}
	} else {
		unlist();
		c.left = cx;
		c.right = cx + 1;
		c.top = r.top + 3;
		c.bottom = r.bottom - 3;
		(void)dp_fill_rect(gid, &c, C_TEXT);
	}
}
