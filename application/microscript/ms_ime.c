/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_ime.c
 *	Micro Script: かな漢字変換 for KINPUT
 *
 *	The keys typed into a KINPUT segment go to a session of the system's
 *	converter (kc_hid_key), in the input mode the message line shows.
 *	What the converter commits goes into the segment's text as typing
 *	does; what it is still working on -- the composition -- is drawn at
 *	the caret, each clause underlined and the one being converted turned
 *	over, with the candidates listed under it while they are chosen. A
 *	number, or a press on a row, takes a candidate.
 *
 *	In 英語 a letter goes in as it is when the mode is 半角, and in its
 *	full-width form otherwise. INPUT is not converted at all.
 */

#include "ms.h"
#include <string.h>
#include <stdio.h>
#include <ts/wm.h>
#include <ts/hid.h>

#define LIST_MAX	4096
#define LIST_ROWS	9
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U
#define IME_FRAME	0x00808080U
#define KEY_ENTER	0x28

static INT	kid;
static T_KCOUT	out;
static INT	base;			/* where the composition starts in out.text */
static BOOL	list;
static UB	cand[LIST_MAX];
static INT	ncand, first, total, chosen;
static T_DPRECT	listbox;		/* where the list was drawn */
static INT	row_h;

static BOOL ready( void )
{
	if ( kid <= 0 ) {
		kid = kc_open();
		if ( kid <= 0 ) kid = 0;
	}
	return (BOOL)( kid > 0 );
}

BOOL ms_ime_busy( void )
{
	return (BOOL)( kid > 0 && out.cl[out.n_cl] > base );
}

/* UTF-8 into the segment as typed characters */
static void type_utf8( const UB *s, INT n )
{
	UW	c[256];
	INT	i = 0, k = 0;

	while ( i < n ) {
		UW	cp;
		INT	d = ms_utf8_dec(s + i, n - i, &cp);

		if ( d <= 0 ) break;
		i += d;
		c[k++] = cp;
		if ( k == 256 ) {
			st_input_text(c, k);
			k = 0;
		}
	}
	if ( k > 0 ) st_input_text(c, k);
}

/* What the converter gave: the committed part typed, the rest kept, the list fetched */
static void take( INT r )
{
	base = 0;
	if ( out.n_out > 0 ) {
		INT	end = out.cl[out.n_out];

		if ( end > 0 ) type_utf8((const UB *)out.text, end);
		base = end;
	}
	list = FALSE;
	if ( ( r & TSMOZC_LIST ) != 0 && ms_ime_busy() ) {
		INT	sel = kc_list(kid, cand, LIST_MAX, &ncand, &first, &total);

		if ( sel > 0 && ncand > 0 ) {
			list = TRUE;
			chosen = sel;
		}
	}
	ms_dirty();
}

/* A letter as it is (半角) or full width */
static BOOL plain( UB ch, BOOL han )
{
	UW	u;

	if ( ch < 0x20 || ch >= 0x7F ) return FALSE;
	u = han ? ch : ( ch == ' ' ) ? 0x3000 : 0xFF01 + ( ch - 0x21 );
	st_input_text(&u, 1);
	return TRUE;
}

/* A key typed into a KINPUT segment: TRUE when it was taken here */
BOOL ms_ime_key( UINT key, UINT mods )
{
	UINT	mode = kc_mode();
	BOOL	han = (BOOL)( ( mode & WM_MODE_HAN ) != 0 );
	UB	ch = wm_key_char(key, mods);
	INT	r;

	if ( ( mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) return FALSE;
	if ( ( mode & WM_MODE_ALPH ) != 0 ) {
		if ( ( mode & WM_MODE_KANA ) != 0 && ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ) ) {
			ch ^= 0x20;		/* 英語 in capitals: the shift turns it back */
		}
		return ( ch > 0x20 && ch < 0x7F ) ? plain(ch, han) : FALSE;
	}
	if ( !ready() ) return FALSE;
	if ( list && ch >= '1' && ch <= '9' && ch - '1' < ncand ) {
		r = kc_choose(kid, first + ( ch - '1' ), &out);
		if ( r >= 0 && r != KC_NOTMINE ) take(r);
		return TRUE;
	}
	r = kc_hid_key(kid, key, mods, &out);
	if ( r >= 0 && r != KC_NOTMINE ) {
		take(r);
		return TRUE;
	}
	/* nothing being composed: a space as the mode types it, the rest the segment's */
	if ( ch == ' ' && !ms_ime_busy() ) return plain(' ', han);
	return ms_ime_busy();
}

/* Whatever is being composed, committed */
void ms_ime_commit( void )
{
	INT	r;

	if ( !ms_ime_busy() ) {
		list = FALSE;
		return;
	}
	r = kc_hid_key(kid, KEY_ENTER, HID_MOD_LSHIFT, &out);
	if ( r >= 0 && r != KC_NOTMINE ) take(r);
	base = out.cl[out.n_cl];
	list = FALSE;
	ms_dirty();
}

/* A press: on the list, that candidate; TRUE when it was the list's */
BOOL ms_ime_press( INT x, INT y )
{
	if ( list && row_h > 0 && x >= listbox.left && x < listbox.right
	  && y >= listbox.top && y < listbox.bottom ) {
		INT	row = ( y - listbox.top - 2 ) / row_h, r;

		if ( row >= 0 && row < ncand ) {
			r = kc_choose(kid, first + row, &out);
			if ( r >= 0 && r != KC_NOTMINE ) take(r);
		}
		return TRUE;
	}
	ms_ime_commit();
	return FALSE;
}

void ms_ime_end( void )
{
	if ( kid > 0 ) kc_close(kid);
	kid = 0;
	base = 0;
	list = FALSE;
	memset(&out, 0, sizeof(out));
}

/* Bytes a..b of the composition as a string */
static void piece( INT a, INT b, UB *buf, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) buf[n++] = (UB)out.text[a];
	buf[n] = 0;
}

/* The composition at the caret, (x, y) its top left; px its letters' size */
void ms_ime_draw( INT gid, INT x, INT y, INT px, INT lh )
{
	UB		s[TSMOZC_TEXT_MAX];
	T_DPRECT	r;
	INT		i, cx = x, end;

	if ( !ms_ime_busy() ) return;
	end = out.cl[out.n_cl];
	piece(base, end, s, sizeof(s));
	r.left = x;
	r.top = y;
	r.right = x + dp_text_width(s, px) + 2;
	r.bottom = y + lh;
	(void)dp_fill_rect(gid, &r, IME_GROUND);
	for ( i = out.n_out; i < out.n_cl; i++ ) {
		INT	a = out.cl[i], b = out.cl[i + 1], w;
		BOOL	target = (BOOL)( i == out.clause && !out.yomi );
		T_DPRECT u;

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
		(void)dp_text(gid, cx, y + px, s, target ? IME_GROUND : IME_INK, px);
		u.left = cx + 1;
		u.right = cx + w - 1;
		u.top = y + lh - 2;
		u.bottom = y + lh - 1;
		(void)dp_fill_rect(gid, &u, IME_INK);
		cx += w;
	}
	if ( out.caret >= base && out.caret <= end ) {
		piece(base, out.caret, s, sizeof(s));
		r.left = x + dp_text_width(s, px);
		r.right = r.left + 2;
		r.top = y;
		r.bottom = y + lh;
		(void)dp_fill_rect(gid, &r, IME_INK);
	}
	if ( list ) {
		/* the candidates under it, a number before each */
		const UB	*c = cand;
		INT		w = 0, k;
		UB		row[TSMOZC_TEXT_MAX + 8];

		row_h = lh + 2;
		for ( k = 0; k < ncand && k < LIST_ROWS; k++ ) {
			INT	cw;

			snprintf((char *)row, sizeof(row), "%d %s", k + 1, (const char *)c);
			cw = dp_text_width(row, px);
			if ( cw > w ) w = cw;
			c += strlen((const char *)c) + 1;
		}
		listbox.left = x;
		listbox.top = y + lh + 2;
		listbox.right = x + w + 8;
		listbox.bottom = listbox.top + row_h * k + 4;
		(void)dp_fill_rect(gid, &listbox, IME_GROUND);
		(void)dp_frame_rect(gid, &listbox, IME_FRAME, 1);
		c = cand;
		for ( i = 0; i < k; i++ ) {
			BOOL	sel = (BOOL)( first + i == chosen );

			snprintf((char *)row, sizeof(row), "%d %s", i + 1, (const char *)c);
			if ( sel ) {
				r.left = listbox.left + 1;
				r.right = listbox.right - 1;
				r.top = listbox.top + 2 + i * row_h;
				r.bottom = r.top + row_h;
				(void)dp_fill_rect(gid, &r, IME_TARGET);
			}
			(void)dp_text(gid, listbox.left + 4, listbox.top + 2 + i * row_h + px, row,
				      sel ? IME_GROUND : IME_INK, px);
			c += strlen((const char *)c) + 1;
		}
	}
}
