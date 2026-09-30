/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_draw.c
 *	ファイル変換: the windows drawn (design 17.17)
 *
 *	Each entry of a list is drawn as a closed 仮身 is: a box 25 pixels
 *	tall with a frame, the pictogram at the left of its band and the
 *	name after it. What the entry is, is told by the colour of the band:
 *
 *	  青	a disk or a volume, or an FTP connection
 *	  水色	the directory above
 *	  緑	a directory
 *	  クリーム	a hidden directory (a name that starts with a dot)
 *	  灰	a file
 *	  白	a hidden file
 *
 *	A disk with no medium in it is drawn faint. The size of a file, and
 *	of a disk where it is mounted, is written after its 仮身. An entry
 *	chosen is drawn with its colours turned over. A scroll bar runs down
 *	the right and a line at the foot says what the window is doing.
 */

#include "xfc.h"
#include <stdio.h>
#include <string.h>

#define TEXT_PX		16
#define PICT		( XFC_VOBJ_H - 4 )

#define C_INK		0x00000000
#define C_WHITE		0x00FFFFFF
#define C_GROUND	0x00FFFFFF
#define C_DIM		0x00707070
#define C_FAINT		0x00A8A8A8
#define C_ERR		0x00C00000
#define C_LINE		0x00A0A0A0
#define C_TRACK		0x00E8E8E8
#define C_KNOB		0x00A8B8D0
#define C_BAND		0x004070C0

#define C_VOL		0x004A78D8	/* 青 */
#define C_VOL_FAINT	0x00C4D2F0
#define C_PARENT	0x00A0DCF4	/* 水色 */
#define C_DIR		0x0098D898	/* 緑 */
#define C_HIDDIR	0x00FFF2C8	/* クリーム */
#define C_FILE		0x00C8C8C8	/* 灰 */
#define C_HIDFILE	0x00FFFFFF	/* 白 */

EXPORT void xfc_size_text( D v, char *out, INT max )
{
	char	d[24], g[32];
	INT	n, i, k = 0;

	if ( v < 0 ) {
		out[0] = 0;
		return;
	}
	if ( v >= (D)10 * 1024 * 1024 * 1024 ) {
		snprintf(out, max, "%d GB", (INT)( v / ( (D)1024 * 1024 * 1024 ) ));
		return;
	}
	if ( v >= (D)100 * 1024 * 1024 ) {
		snprintf(out, max, "%d MB", (INT)( v / ( 1024 * 1024 ) ));
		return;
	}
	/* 147780 as "147,780" */
	n = snprintf(d, sizeof(d), "%d", (INT)v);
	for ( i = 0; i < n; i++ ) {
		if ( i > 0 && ( n - i ) % 3 == 0 ) g[k++] = ',';
		g[k++] = d[i];
	}
	g[k] = 0;
	snprintf(out, max, "%s バイト", g);
}

EXPORT void xfc_work_size( XWIN *w, INT *p_w, INT *p_h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	*p_w = 400;
	*p_h = 400;
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
		*p_w = wp.wright - wp.wleft;
		*p_h = wp.wbottom - wp.wtop;
	}
}

EXPORT INT xfc_rows( XWIN *w )
{
	INT	ww, wh, n;

	xfc_work_size(w, &ww, &wh);
	n = ( wh - XFC_TOP - XFC_STATUS_H ) / XFC_ROW_H;
	return ( n < 1 ) ? 1 : n;
}

/* The words a 仮身 of the entry shows */
static CONST char *shown_name( CONST XFCENT *e )
{
	return (CONST char *)e->name;
}

EXPORT INT xfc_vobj_w( XWIN *w, CONST XFCENT *e )
{
	INT	ww, wh, v, most;

	xfc_work_size(w, &ww, &wh);
	v = 2 + PICT + 3 + dp_text_width((CONST UB *)shown_name(e), TEXT_PX) + 10;
	most = ww - XFC_LEFT - XFC_SB_W - 12 - ( ( e->kind == E_FILE ) ? 96 : ( e->kind == E_DISK ) ? 150 : 0 );
	if ( most < 120 ) most = 120;
	if ( v < 120 ) v = 120;
	if ( v > most ) v = most;
	return v;
}

EXPORT BOOL xfc_vobj_rect( XWIN *w, INT i, T_DPRECT *r )
{
	INT	rows = xfc_rows(w);

	if ( i < w->top || i >= w->top + rows || i >= w->nent ) return FALSE;
	r->left = XFC_LEFT;
	r->top = XFC_TOP + ( i - w->top ) * XFC_ROW_H + ( XFC_ROW_H - XFC_VOBJ_H ) / 2;
	r->right = r->left + ( ( w->ent[i].vw > 0 ) ? w->ent[i].vw : xfc_vobj_w(w, &w->ent[i]) );
	r->bottom = r->top + XFC_VOBJ_H;
	return TRUE;
}

EXPORT INT xfc_hit( XWIN *w, INT x, INT y )
{
	T_DPRECT	r;
	INT		i, rows = xfc_rows(w);

	for ( i = w->top; i < w->nent && i < w->top + rows; i++ ) {
		if ( xfc_vobj_rect(w, i, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom ) {
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- the pictograms */

static void line( XWIN *w, INT x0, INT y0, INT x1, INT y1, UW c )
{
	(void)dp_line(w->gid, x0, y0, x1, y1, c);
}

static void frame( XWIN *w, INT l, INT t, INT r, INT b, UW c )
{
	T_DPRECT	q = { l, t, r, b };

	(void)dp_frame_rect(w->gid, &q, c, 1);
}

static void fill( XWIN *w, INT l, INT t, INT r, INT b, UW c )
{
	T_DPRECT	q = { l, t, r, b };

	(void)dp_fill_rect(w->gid, &q, c);
}

/* In the square of side PICT at (x, y): what the entry is, in ink on paper */
static void pictogram( XWIN *w, UINT kind, INT x, INT y, UW ink, UW paper )
{
	INT	s = PICT, f;

	switch ( kind ) {
	case E_DISK:
	case E_VOL:
		/* a drive: its case, the slot and the lamp */
		fill(w, x + 1, y + 5, x + s - 1, y + s - 4, paper);
		frame(w, x + 1, y + 5, x + s - 1, y + s - 4, ink);
		line(w, x + 4, y + 10, x + s - 8, y + 10, ink);
		fill(w, x + s - 6, y + s - 9, x + s - 3, y + s - 7, ink);
		break;
	case E_FTP:
	case E_NEWFTP:
		/* two machines and the line between them */
		fill(w, x + 1, y + 2, x + 10, y + 9, paper);
		frame(w, x + 1, y + 2, x + 10, y + 9, ink);
		fill(w, x + s - 10, y + s - 10, x + s - 1, y + s - 3, paper);
		frame(w, x + s - 10, y + s - 10, x + s - 1, y + s - 3, ink);
		line(w, x + 5, y + 9, x + 5, y + s - 6, ink);
		line(w, x + 5, y + s - 6, x + s - 10, y + s - 6, ink);
		break;
	case E_UP:
		/* an arrow pointing up */
		line(w, x + s / 2, y + 3, x + s / 2, y + s - 3, ink);
		line(w, x + s / 2, y + 3, x + s / 2 - 5, y + 8, ink);
		line(w, x + s / 2, y + 3, x + s / 2 + 5, y + 8, ink);
		line(w, x + s / 2, y + s - 3, x + s - 3, y + s - 3, ink);
		break;
	case E_DIR:
		/* a folder: its tab and its body */
		fill(w, x + 1, y + 4, x + 9, y + 8, paper);
		frame(w, x + 1, y + 4, x + 9, y + 8, ink);
		fill(w, x + 1, y + 7, x + s - 1, y + s - 3, paper);
		frame(w, x + 1, y + 7, x + s - 1, y + s - 3, ink);
		break;
	default:
		/* a leaf of paper with its corner turned */
		f = 5;
		fill(w, x + 3, y + 1, x + s - 3, y + s - 1, paper);
		frame(w, x + 3, y + 1, x + s - 3, y + s - 1, ink);
		line(w, x + s - 4 - f, y + 1, x + s - 4, y + 1 + f, ink);
		line(w, x + s - 4 - f, y + 1, x + s - 4 - f, y + 1 + f, ink);
		line(w, x + s - 4 - f, y + 1 + f, x + s - 4, y + 1 + f, ink);
		break;
	}
}

/* ---------------------------------------------------------------- a 仮身 */

/* The band's colour and the letters' for an entry */
static void colours( CONST XFCENT *e, UW *tb, UW *ch, UW *fr )
{
	*fr = C_INK;
	*ch = C_INK;
	switch ( e->kind ) {
	case E_DISK:
	case E_VOL:
	case E_FTP:
	case E_NEWFTP:
		*tb = e->faint ? C_VOL_FAINT : C_VOL;
		*ch = e->faint ? C_FAINT : C_WHITE;
		break;
	case E_UP:	*tb = C_PARENT; break;
	case E_DIR:	*tb = e->hidden ? C_HIDDIR : C_DIR; break;
	default:	*tb = e->hidden ? C_HIDFILE : C_FILE; break;
	}
	if ( e->faint ) *fr = C_FAINT;
}

/* As much of s as is room wide, cut at a character */
static void fit( CONST char *s, INT room, char *out, INT max )
{
	INT	n = (INT)strlen(s);

	if ( n >= max ) n = max - 1;
	memcpy(out, s, n);
	out[n] = 0;
	while ( n > 0 && dp_text_width((CONST UB *)out, TEXT_PX) > room ) {
		do {
			n--;
		} while ( n > 0 && ( (UB)out[n] & 0xC0 ) == 0x80 );
		out[n] = 0;
	}
}

static void draw_vobj( XWIN *w, INT i )
{
	XFCENT		*e = &w->ent[i];
	T_DPRECT	r;
	UW		tb, ch, fr, t;
	char		s[300], sz[120];
	INT		tx;

	e->vw = xfc_vobj_w(w, e);
	if ( !xfc_vobj_rect(w, i, &r) ) return;
	colours(e, &tb, &ch, &fr);
	if ( e->marked ) {
		/* chosen: turned over, as a 仮身 chosen on the desktop */
		t = tb ^ 0x00FFFFFF;
		tb = t;
		ch ^= 0x00FFFFFF;
	}
	(void)dp_fill_rect(w->gid, &r, tb);
	pictogram(w, e->kind, r.left + 2, r.top + 2, e->marked ? C_WHITE : C_INK, e->marked ? C_INK : C_WHITE);
	tx = r.left + 2 + PICT + 3;
	fit(shown_name(e), r.right - tx - 4, s, sizeof(s));
	(void)dp_text(w->gid, tx, r.top + 18, (CONST UB *)s, ch, TEXT_PX);
	(void)dp_frame_rect(w->gid, &r, fr, 1);

	/* after it: the size, and where a disk is mounted */
	sz[0] = 0;
	if ( e->kind == E_FILE ) {
		xfc_size_text(e->size, sz, sizeof(sz));
	} else if ( e->kind == E_DISK ) {
		char	b[24];

		xfc_size_text(e->size, b, sizeof(b));
		if ( e->faint ) snprintf(sz, sizeof(sz), "媒体なし");
		else if ( e->mount[0] != 0 ) snprintf(sz, sizeof(sz), "%s  %s%s", b, e->mount, e->ro ? " 読込みのみ" : "");
		else snprintf(sz, sizeof(sz), "%s", b);
	}
	if ( sz[0] != 0 ) {
		(void)dp_text(w->gid, r.right + 8, r.top + 18, (CONST UB *)sz, e->faint ? C_FAINT : C_DIM, TEXT_PX - 2);
	}
}

/* ---------------------------------------------------------------- the window */

static void scroll_bar( XWIN *w, INT ww, INT wh )
{
	INT	rows = xfc_rows(w), h = wh - XFC_STATUS_H, kt, kb;

	fill(w, ww - XFC_SB_W, 0, ww, h, C_TRACK);
	line(w, ww - XFC_SB_W, 0, ww - XFC_SB_W, h - 1, C_LINE);
	if ( w->nent <= rows ) return;
	kt = h * w->top / w->nent;
	kb = h * ( w->top + rows ) / w->nent;
	if ( kb - kt < 12 ) kb = kt + 12;
	if ( kb > h ) kb = h;
	fill(w, ww - XFC_SB_W + 2, kt + 1, ww - 2, kb - 1, C_KNOB);
	frame(w, ww - XFC_SB_W + 2, kt + 1, ww - 2, kb - 1, C_DIM);
}

EXPORT void xfc_paint( XWIN *w )
{
	T_DPRECT	all;
	INT		ww, wh, i, rows;
	char		s[320];

	if ( w->gid < 0 ) return;
	xfc_work_size(w, &ww, &wh);
	all.left = 0;
	all.top = 0;
	all.right = ww;
	all.bottom = wh;
	(void)dp_fill_rect(w->gid, &all, C_GROUND);
	rows = xfc_rows(w);
	for ( i = w->top; i < w->nent && i < w->top + rows; i++ ) draw_vobj(w, i);
	if ( w->band ) {
		T_DPRECT	b;

		b.left = ( w->bx0 < w->bx1 ) ? w->bx0 : w->bx1;
		b.right = ( w->bx0 < w->bx1 ) ? w->bx1 : w->bx0;
		b.top = ( w->by0 < w->by1 ) ? w->by0 : w->by1;
		b.bottom = ( w->by0 < w->by1 ) ? w->by1 : w->by0;
		if ( b.right > b.left && b.bottom > b.top ) (void)dp_frame_rect(w->gid, &b, C_BAND, 1);
	}
	scroll_bar(w, ww, wh);

	/* the line at the foot */
	line(w, 0, wh - XFC_STATUS_H, ww - 1, wh - XFC_STATUS_H, C_LINE);
	fit(w->status, ww - 16, s, sizeof(s));
	(void)dp_text(w->gid, 8, wh - 7, (CONST UB *)s, w->status_err ? C_ERR : C_INK, TEXT_PX - 2);
	(void)wm_obj_flush(w->kw, NULL);
	w->dirty = FALSE;
}
