/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	con_ime.c
 *	コンソール: かな漢字変換 on the line typed (design 16.5.19)
 *
 *	The keys go to a session of the system's converter (kc_hid_key) in
 *	the input mode each key came in (T_OBNTM imode), as in the other
 *	programs that run as processes. What it commits goes into the line; what it is
 *	still working on -- the composition -- is drawn where the line ends,
 *	each clause underlined and the one being converted turned over. While
 *	candidates are chosen the system lists them under it (over it when
 *	there is no room below) in a window of its own (OB_WR_CAND); a digit,
 *	or a press on a name there, takes a candidate.
 *
 *	In 英語 a letter goes in as it is when the mode is 半角, and in its
 *	full-width form otherwise; the line is read with those forms taken
 *	as ASCII (con_fold), so a command typed either way is the same.
 */

#include "console.h"
#include <ts/wm.h>
#include <ts/hid.h>

#define LIST_MAX	4096
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U
#define KEY_ENTER	0x28

LOCAL INT	kid;
LOCAL T_KCOUT	out;
LOCAL UINT	kmode = WM_MODE_ROMAN;	/* the input mode what is being composed was typed in */
LOCAL INT	base;			/* where the composition starts in out.text */
LOCAL BOOL	list;
LOCAL UB	cand[LIST_MAX];
LOCAL INT	ncand, first, total, chosen;
LOCAL ID	listed = 0;		/* the window the system lists them for, 0 none */

LOCAL BOOL ready( void )
{
	if ( kid <= 0 ) {
		kid = kc_open();
		if ( kid <= 0 ) kid = 0;
	}
	return (BOOL)( kid > 0 );
}

EXPORT BOOL ime_busy( void )
{
	return (BOOL)( kid > 0 && out.cl[out.n_cl] > base );
}

/* What the converter gave: the committed part into the line, the rest kept, the list fetched */
LOCAL void take( INT r )
{
	base = 0;
	if ( out.n_out > 0 ) {
		INT	end = out.cl[out.n_out];

		if ( end > 0 ) con_insert((CONST UB *)out.text, end);
		base = end;
	}
	list = FALSE;
	if ( ( r & TSMOZC_LIST ) != 0 && ime_busy() ) {
		INT	sel = kc_list(kid, cand, LIST_MAX, &ncand, &first, &total);

		if ( sel > 0 && ncand > 0 ) {
			list = TRUE;
			chosen = sel;
		}
	}
}

/* A letter as it is (半角) or in its full-width form */
LOCAL BOOL plain( UB ch, BOOL han )
{
	UB	u[4];
	UW	cp;

	if ( ch < 0x20 || ch >= 0x7F ) return FALSE;
	if ( han ) {
		con_insert(&ch, 1);
		return TRUE;
	}
	cp = ( ch == ' ' ) ? 0x3000 : 0xFF01 + ( ch - 0x21 );
	u[0] = (UB)( 0xE0 | ( cp >> 12 ) );
	u[1] = (UB)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
	u[2] = (UB)( 0x80 | ( cp & 0x3F ) );
	con_insert(u, 3);
	return TRUE;
}

/*
 * A key, in the input mode it came in (T_OBNTM imode). A key in 英語
 * that comes while something is being composed -- 英数 itself, or one
 * typed after it -- commits that first, then is 英語's.
 */
EXPORT BOOL ime_key( UINT key, UINT mods, UINT mode )
{
	BOOL	han = (BOOL)( ( mode & WM_MODE_HAN ) != 0 );
	UB	ch = wm_key_char(key, mods);
	INT	r;

	if ( ( mode & WM_MODE_ALPH ) != 0 && ime_busy() ) {
		ime_commit();
	}
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
	kmode = mode;
	r = kc_hid_key(kid, key, mods, mode, &out);
	if ( r >= 0 && r != KC_NOTMINE ) {
		take(r);
		return TRUE;
	}
	/* nothing being composed: a space as the mode types it, the rest the line's */
	if ( ch == ' ' && !ime_busy() ) return plain(' ', han);
	return ime_busy();
}

EXPORT void ime_commit( void )
{
	INT	r;

	if ( !ime_busy() ) {
		list = FALSE;
		return;
	}
	r = kc_hid_key(kid, KEY_ENTER, HID_MOD_LSHIFT, kmode, &out);
	if ( r >= 0 && r != KC_NOTMINE ) take(r);
	base = out.cl[out.n_cl];
	list = FALSE;
}

EXPORT void ime_press( void )
{
	ime_commit();
}

EXPORT void ime_pick( INT row )
{
	INT	r;

	if ( list && kid > 0 && row >= 0 && row < ncand ) {
		r = kc_choose(kid, first + row, &out);
		if ( r >= 0 && r != KC_NOTMINE ) take(r);
	}
}

/* The system's list put away */
LOCAL void unlist( void )
{
	if ( listed > 0 ) (void)ob_cand_put(listed, 0, 0, 0, NULL, 0, -1, 0, 0);
	listed = 0;
}

EXPORT void ime_end( void )
{
	unlist();
	if ( kid > 0 ) kc_close(kid);
	kid = 0;
	base = 0;
	list = FALSE;
	memset(&out, 0, sizeof(out));
}

/* Bytes a..b of the composition as a string */
LOCAL void piece( INT a, INT b, UB *buf, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) buf[n++] = (UB)out.text[a];
	buf[n] = 0;
}

EXPORT void ime_draw( LTWIN *w, INT x, INT y )
{
	static UB	s[TSMOZC_TEXT_MAX];
	T_DPRECT	r, u;
	INT		i, cx = x, end, wd, gid = w->gid;

	if ( !ime_busy() || x < 0 || y < 0 ) {
		unlist();
		return;
	}
	end = out.cl[out.n_cl];
	piece(base, end, s, sizeof(s));
	r.left = x;
	r.top = y;
	r.right = x + dp_text_width(s, LT_PX) + 2;
	r.bottom = y + LT_LH;
	(void)dp_fill_rect(gid, &r, IME_GROUND);
	for ( i = out.n_out; i < out.n_cl; i++ ) {
		INT	a = out.cl[i], b = out.cl[i + 1];
		BOOL	target = (BOOL)( i == out.clause && !out.yomi );

		if ( b <= base ) continue;
		if ( a < base ) a = base;
		piece(a, b, s, sizeof(s));
		wd = dp_text_width(s, LT_PX);
		if ( target ) {
			u.left = cx;
			u.top = y;
			u.right = cx + wd;
			u.bottom = y + LT_LH;
			(void)dp_fill_rect(gid, &u, IME_TARGET);
		}
		(void)dp_text(gid, cx, y + LT_PX, s, target ? IME_GROUND : IME_INK, LT_PX);
		u.left = cx + 1;
		u.right = cx + wd - 1;
		u.top = y + LT_LH - 2;
		u.bottom = y + LT_LH - 1;
		(void)dp_fill_rect(gid, &u, IME_INK);
		cx += wd;
	}
	if ( out.caret >= base && out.caret <= end ) {
		piece(base, out.caret, s, sizeof(s));
		r.left = x + dp_text_width(s, LT_PX);
		r.right = r.left + 2;
		r.top = y;
		r.bottom = y + LT_LH;
		(void)dp_fill_rect(gid, &r, IME_INK);
	}
	if ( !list ) {
		unlist();
		return;
	}
	/* the candidates, listed by the system under the composition */
	if ( ob_cand_put(w->kw, x, y, y + LT_LH, cand, ncand, chosen - first, first, total) >= E_OK ) {
		listed = w->kw;
	}
}
