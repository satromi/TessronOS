/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	msgline.c
 *	The message line (the status bar along the foot of the screen)
 *
 *	A band as high as the system's letters and a few more, laid over
 *	the windows. From the left: a program's message; the keyboard's
 *	input mode; a rule; and the day of the week with the time. A press
 *	on the clock shows the date for three seconds instead.
 *
 *	The measurements come from the letters' size h (and width w, the
 *	same): the band is h + h/32 + 3 + (h/32 + 1) high and the letters
 *	sit h/32 + 3 down. From the right, the clock takes four and a
 *	quarter letters, half a letter more before its rule; before the
 *	rule two letters show the input mode, and four letters before those
 *	are kept for the keyboard's lamps, where the message stops.
 *
 *	The band has pixels of its own, laid out like a piece of the screen
 *	(the screen's pitch, from its own corner), and an environment that
 *	draws into them with the band's corner as its origin. What is drawn
 *	there is laid onto the screen by the compositor after the windows
 *	and before the menus (knl_msg_lay).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <ts/fn.h>
#include <ts/dt.h>

#define ML_TEXT_MAX	256		/* bytes of a message */
#define ML_DATE_HOLD	3000		/* ms the date stays after a press */

LOCAL BOOL	ml_on = FALSE;
LOCAL INT	ml_gid = -1;
LOCAL UB	*ml_pixels = NULL;
LOCAL UD	ml_pages = 0;
LOCAL UINT	ml_pitch;
LOCAL INT	ml_w, ml_h, ml_top;		/* the band, on the screen */
LOCAL INT	ml_fh = 16, ml_fw = 16;		/* the letters */
LOCAL INT	ml_margin;			/* the rules above the letters */
LOCAL INT	ml_x0, ml_lim;			/* where a message starts and ends */
LOCAL INT	ml_modex, ml_sepx, ml_clkx;

LOCAL char	ml_text[ML_TEXT_MAX];
LOCAL UINT	ml_mode = WM_MODE_ALPH | WM_MODE_ROMAN;
LOCAL UD	ml_date_at = 0;			/* when the date was asked for, or 0 */
LOCAL INT	ml_shown[4] = { -1, -1, -1, -1 };	/* what the clock shows */

/* ---------------------------------------------------------------- drawing */

LOCAL UD ml_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UW)t.lo;
}

LOCAL void ml_fill( INT l, INT t, INT r, INT b, UW colour )
{
	T_DPRECT	q;

	if ( r <= l || b <= t ) {
		return;
	}
	q.left = l;  q.top = t;  q.right = r;  q.bottom = b;
	dp_fill_rect(ml_gid, &q, colour);
}

/* Only what is inside l..r is drawn until it is said again */
LOCAL void ml_clip( INT l, INT r )
{
	T_DPRECT	v;

	v.left = l;  v.top = 0;  v.right = r;  v.bottom = ml_h;
	dp_set_visible(ml_gid, &v);
}

/* The part of the band from l to r laid onto the screen again */
LOCAL void ml_show( INT l, INT r )
{
	T_DPRECT	s;

	s.left = l;  s.top = ml_top;  s.right = r;  s.bottom = ml_top + ml_h;
	(void)wm_composite_at(&s);
}

/*
 * Words drawn from x on the letters' line: at full size, at half width
 * (half), or smaller by a quarter (small), which is how the mark for
 * romaji is. Answers how far the pen went.
 */
LOCAL INT ml_words( INT x, CONST char *s, UW colour, BOOL half, BOOL small )
{
	ID		fid = fn_system();
	T_FNMET		met;
	INT		px = small ? 3 * ml_fh / 4 : ml_fh, off, w;

	if ( fid <= 0 ) {
		return 0;
	}
	(void)fn_set_size(fid, px);
	(void)fn_set_style(fid, 0, half ? ml_fw / 2 : 0);
	if ( fn_metrics(fid, &met) < E_OK ) {
		met.ascent = px * 7 / 8;
		met.descent = px - met.ascent;
	}
	/* in the middle of what is under the rules, the smaller ones too */
	off = ml_margin + 2 + ( ml_h - ml_margin - 2 - met.ascent - met.descent ) / 2;
	w = fn_draw(ml_gid, fid, x, off + met.ascent, (CONST UB *)s, colour);
	(void)fn_set_style(fid, 0, 0);

	return w;
}

LOCAL INT ml_width( CONST char *s )
{
	ID	fid = fn_system();

	if ( fid <= 0 ) {
		return 0;
	}
	(void)fn_set_size(fid, ml_fh);
	(void)fn_set_style(fid, 0, 0);
	return fn_width(fid, (CONST UB *)s);
}

/* The rules: black along the top, lit under it, and before the clock */
LOCAL void ml_rules( void )
{
	ml_clip(0, ml_w);
	ml_fill(0, 0, ml_w, ml_h, wm_look(LK_MSGGREY));
	ml_fill(0, 0, ml_w, 1, 0x00000000U);
	ml_fill(0, 1, ml_w, 2, wm_look(LK_LIGHT));
	ml_fill(ml_sepx, 1, ml_sepx + 1, ml_h, wm_look(LK_SHADOW));
	ml_fill(ml_sepx + 1, 1, ml_sepx + 2, ml_h, wm_look(LK_LIGHT));
}

/* The message, from the left; cut with … when it does not fit */
LOCAL void ml_message( void )
{
	ID	fid = fn_system();
	char	buf[ML_TEXT_MAX + 4];
	INT	n, room;

	ml_clip(0, ml_lim);
	ml_fill(0, ml_margin + 2, ml_lim, ml_h, wm_look(LK_MSGGREY));
	if ( ml_text[0] == 0 || fid <= 0 ) {
		return;
	}
	room = ml_lim - ml_x0;
	for ( n = 0; ml_text[n] != 0; n++ ) {
		buf[n] = ml_text[n];
	}
	buf[n] = 0;
	if ( ml_width(buf) > room ) {
		n = fn_fit(fid, (CONST UB *)buf, room - ml_width("…"));
		if ( n < 0 ) n = 0;
		buf[n] = 0;
		knl_memcpy(buf + n, "…", sizeof("…"));
	}
	(void)ml_words(ml_x0, buf, wm_look(LK_MSGCOL), FALSE, FALSE);
}

/*
 * The input mode, before the rule: R, smaller, when romaji is typed,
 * then the mode's letter -- あ, ア, ａ or Ａ -- or two of them at half
 * width when letters are typed half width.
 */
LOCAL void ml_input_mode( void )
{
	LOCAL CONST char	*one[4] = { "あ", "ａ", "ア", "Ａ" };
	LOCAL CONST char	*two[4] = { "あい", "ａｂ", "アイ", "ＡＢ" };
	UINT	k = 0;
	BOOL	half = (BOOL)( ( ml_mode & WM_MODE_HAN ) != 0 );

	if ( ( ml_mode & WM_MODE_ALPH ) != 0 ) k |= 1;
	if ( ( ml_mode & WM_MODE_KANA ) != 0 ) k |= 2;
	ml_clip(ml_modex, ml_sepx);
	ml_fill(ml_modex, ml_margin + 2, ml_sepx, ml_h, wm_look(LK_MSGGREY));
	if ( ( ml_mode & WM_MODE_ROMAN ) != 0 ) {
		(void)ml_words(ml_modex, "Ｒ", wm_look(LK_MSGCOL), TRUE, TRUE);
	}
	(void)ml_words(ml_modex + 3 * ml_fw / 4, half ? two[k] : one[k],
		       wm_look(LK_MSGCOL), half, FALSE);
}

/* Two figures of a number, full width, a space before one alone */
LOCAL INT ml_figures( char *out, INT v, BOOL spaced )
{
	LOCAL CONST char	*fig[10] = {
		"０", "１", "２", "３", "４", "５", "６", "７", "８", "９"
	};
	CONST char	*a = ( spaced && v < 10 ) ? "　" : fig[( v / 10 ) % 10];
	CONST char	*b = fig[v % 10];
	INT		n = 0;

	while ( *a != 0 ) out[n++] = *a++;
	while ( *b != 0 ) out[n++] = *b++;
	out[n] = 0;
	return n;
}

/*
 * The clock, at the right: the day's name -- red on a Sunday, blue on a
 * Saturday -- and the time in half-width figures; the date instead while
 * a press has asked for it. Drawn again only when what it shows changed;
 * TRUE when it was.
 */
LOCAL BOOL ml_clock( BOOL force )
{
	LOCAL CONST char	*days[7] = { "日", "月", "火", "水", "木", "金", "土" };
	TS_TIME	t;
	TS_TM	tm;
	char	buf[64];
	INT	n = 0, v[4];
	BOOL	date = (BOOL)( ml_date_at != 0 );

	if ( dt_gettime(&t) < E_OK || dt_localtime(&t, &tm) < E_OK ) {
		return FALSE;
	}
	v[0] = date ? 1 : 0;
	v[1] = date ? tm.tm_year : tm.tm_hour;
	v[2] = date ? tm.tm_mon : tm.tm_min;
	v[3] = date ? tm.tm_mday : tm.tm_wday;
	if ( !force && v[0] == ml_shown[0] && v[1] == ml_shown[1]
	  && v[2] == ml_shown[2] && v[3] == ml_shown[3] ) {
		return FALSE;
	}
	knl_memcpy(ml_shown, v, sizeof(v));

	ml_clip(ml_clkx - 2, ml_w);
	ml_fill(ml_clkx - 2, ml_margin + 2, ml_w, ml_h, wm_look(LK_MSGGREY));
	if ( date ) {
		n += ml_figures(buf + n, tm.tm_year % 100, FALSE);
		knl_memcpy(buf + n, "／", sizeof("／"));  n += sizeof("／") - 1;
		n += ml_figures(buf + n, tm.tm_mon + 1, FALSE);
		knl_memcpy(buf + n, "／", sizeof("／"));  n += sizeof("／") - 1;
		(void)ml_figures(buf + n, tm.tm_mday, FALSE);
		(void)ml_words(ml_clkx, buf, wm_look(LK_MSGCOL), TRUE, FALSE);
	} else {
		UW	col = wm_look(LK_MSGCOL);
		INT	c = 3 * ml_fw / 2, b = ( c - ml_fw ) / 2;

		if ( tm.tm_wday == 0 ) {
			col = 0x00FF4040U;
		} else if ( tm.tm_wday == 6 ) {
			col = 0x0000FFFFU;
		}
		if ( tm.tm_wday >= 0 && tm.tm_wday < 7 ) {
			(void)ml_words(ml_clkx + b - 2, days[tm.tm_wday], col,
				       FALSE, FALSE);
		}
		n += ml_figures(buf + n, tm.tm_hour, TRUE);
		knl_memcpy(buf + n, "：", sizeof("："));  n += sizeof("：") - 1;
		(void)ml_figures(buf + n, tm.tm_min, FALSE);
		(void)ml_words(ml_clkx + c, buf, wm_look(LK_MSGCOL), TRUE, FALSE);
	}
	return TRUE;
}

/* Everything in the band drawn again */
LOCAL void ml_redraw( void )
{
	ml_rules();
	ml_message();
	ml_input_mode();
	(void)ml_clock(TRUE);
}

/* ---------------------------------------------------------------- the calls */

/* The band's shape and its pixels, for a screen of the size it is */
LOCAL ER ml_make( void )
{
	T_DISPSPEC	spec;
	T_DPRECT	r;
	INT		q;

	if ( ts_disp_ref(&spec) < E_OK ) {
		return E_NOEXS;
	}
	q = ml_fh / 32;
	ml_margin = q + 1;
	ml_h = ml_fh + q + 3 + ml_margin;
	ml_w = (INT)spec.width;
	ml_top = (INT)spec.height - ml_h;
	ml_pitch = spec.pitch;
	ml_x0 = ml_fw / 4;
	ml_clkx = ml_w - ( 4 * ml_fw + ml_fw / 4 );
	ml_sepx = ml_clkx - ml_fw / 2;
	ml_modex = ml_sepx - 2 * ml_fw;
	ml_lim = ml_modex - 4 * ml_fw + 1;

	if ( ml_pixels == NULL ) {
		ml_pages = (UD)( ( (SZ)ml_pitch * ml_h + 4095 ) / 4096 );
		ml_pixels = (UB *)knl_vmap(ml_pages, 0);
		if ( ml_pixels == NULL ) {
			return E_NOMEM;
		}
	}
	if ( ml_gid < 0 ) {
		ml_gid = dp_open();
		if ( ml_gid < 0 ) {
			return E_LIMIT;
		}
	}
	dp_set_target(ml_gid, ml_pixels, ml_pitch, 0, ml_top);
	dp_set_origin(ml_gid, 0, ml_top);
	r.left = 0;  r.top = ml_top;  r.right = ml_w;  r.bottom = ml_top + ml_h;
	dp_set_frame(ml_gid, &r);
	ml_clip(0, ml_w);

	return E_OK;
}

EXPORT ER wm_msg_show( BOOL on )
{
	ER	er;

	if ( on && !ml_on ) {
		er = ml_make();
		if ( er < E_OK ) {
			return er;
		}
		ml_on = TRUE;
		ml_redraw();
	} else if ( !on && ml_on ) {
		ml_on = FALSE;
	} else {
		return E_OK;
	}
	(void)wm_composite_at(NULL);

	return E_OK;
}

EXPORT INT wm_msg_height( void )
{
	return ml_on ? ml_h : 0;
}

EXPORT ER wm_msg_put( CONST char *utf8 )
{
	INT	n = 0;

	if ( utf8 != NULL ) {
		while ( utf8[n] != 0 && n < ML_TEXT_MAX - 1 ) {
			n++;
		}
		/* never half a character */
		while ( n > 0 && utf8[n] != 0 && ( (UB)utf8[n] & 0xC0 ) == 0x80 ) {
			n--;
		}
		knl_memcpy(ml_text, utf8, (SZ)n);
	}
	if ( n == 0 && ml_text[0] == 0 ) {
		return E_OK;			/* nothing was there, nothing is */
	}
	ml_text[n] = 0;
	if ( ml_on ) {
		ml_message();
		ml_show(0, ml_lim);
	}
	return E_OK;
}

/* The message shown, "" for none: the bytes written */
EXPORT INT wm_msg_text( char *buf, INT max )
{
	INT	n = 0;

	if ( buf == NULL || max <= 0 ) {
		return 0;
	}
	while ( n < max - 1 && ml_text[n] != 0 ) {
		buf[n] = ml_text[n];
		n++;
	}
	buf[n] = 0;
	return n;
}

EXPORT UINT wm_msg_get_mode( void )
{
	return ml_mode;
}

EXPORT ER wm_msg_mode( UINT mode )
{
	if ( mode == ml_mode ) {
		return E_OK;
	}
	ml_mode = mode;
	if ( ml_on ) {
		ml_input_mode();
		ml_show(ml_modex, ml_sepx);
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the manager's */

/* The top of the band on the screen: below it the windows are not */
EXPORT INT knl_msg_top( void )
{
	return ml_on ? ml_top : 0x7FFFFFFF;
}

/* The band laid onto the screen being built, as far as box reaches */
EXPORT void knl_msg_lay( UB *back, UINT pitch, CONST T_DPRECT *box )
{
	INT	y, from, to, wide;

	if ( !ml_on || ml_pixels == NULL ) {
		return;
	}
	from = ( box->top > ml_top ) ? box->top : ml_top;
	to = ( box->bottom < ml_top + ml_h ) ? box->bottom : ml_top + ml_h;
	wide = box->right - box->left;
	if ( to <= from || wide <= 0 ) {
		return;
	}
	for ( y = from; y < to; y++ ) {
		knl_memcpy(back + (SZ)y * pitch + (SZ)box->left * sizeof(UW),
			   ml_pixels + (SZ)( y - ml_top ) * ml_pitch
			   + (SZ)box->left * sizeof(UW),
			   (SZ)wide * sizeof(UW));
	}
}

/*
 * A press on the band. On the clock it shows the date, for three
 * seconds; a second press there puts the time back.
 */
EXPORT void knl_msg_press( INT x, INT y )
{
	if ( !ml_on || y < ml_top || x <= ml_sepx ) {
		return;
	}
	ml_date_at = ( ml_date_at == 0 ) ? ( ml_now() | 1 ) : 0;
	(void)ml_clock(TRUE);
	ml_show(ml_sepx, ml_w);
}

/* Time going by: the clock moved on, and the date put away when due */
EXPORT void knl_msg_tick( void )
{
	if ( !ml_on ) {
		return;
	}
	if ( ml_date_at != 0 && ml_now() - ml_date_at >= ML_DATE_HOLD ) {
		ml_date_at = 0;
	}
	if ( ml_clock(FALSE) ) {
		ml_show(ml_sepx, ml_w);
	}
}

/* The look changed: the band drawn in the new one */
EXPORT void knl_msg_restyle( void )
{
	if ( ml_on ) {
		ml_redraw();
	}
}
