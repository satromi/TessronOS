/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_tool.c
 *	基本図形編集 as a process: its 道具パネル and palettes (design 11.3,
 *	17.13)
 *
 *	The panel is a window of the program's, a subordinate window of the
 *	figure's (ui_sub): kept just in front of it, carried with it, and
 *	out of sight while a window of something else is in use. Its parts
 *	are panel 5 of the data box (SYSDEF_BOX_FIG): the swatches of the
 *	inside and of lines, and push buttons with pictures, those of one
 *	place standing one over another and only the one that says what is
 *	now shown -- the tool in hand emphasised, the mark of drawing from
 *	the middle, the canvas or the 画材, the magnification, the grid.
 *
 *	A palette comes up under its button as a window subordinate to the
 *	panel, panel 10 and the button's place of the data box, laid out as
 *	the figure editor's code lays out its items (figtool.c); what is
 *	changed in it is changed at once, on the shapes picked and on those
 *	drawn after. The fixed patterns of 塗りつぶし are drawn over its
 *	panel by the same code as the desktop's. Enter, Escape, or a press
 *	on the figure or the panel puts it away.
 *
 *	What is drawn with the tools, the grid and the look are the figure
 *	editor's code's, through a view of the figure (T_FTVIEW).
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/look.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/docmenu.h>
#include <ts/docedit.h>
#include <ts/figtool.h>
#include <ts/img.h>
#include <ts/tvsrc.h>
#include <ts/json.h>
#include <ts/obatr.h>

#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define MSG_ROOM	24		/* the message line along the foot of the screen */

/* The panel's parts (panel 5 of the data box) */
#define PN_TOOL		5
#define PN_POP		10		/* + the button's place: its palette */
#define T_FILL		1
#define T_STROKE	2
#define T_XY		99
#define T_BTN( b, v )	( 100 + ( b ) * 10 + ( v ) )

/* The buttons, by their place: the first row and then the second */
#define B_FILL		0
#define B_STROKE	1
#define B_LSTYLE	2
#define B_CORNER	3
#define B_CANVAS	4
#define B_SELECT	5
#define B_ZOOM		6
#define B_GRID		7
#define B_ROW2		8
#define B_FONT		( B_ROW2 + 11 )
#define B_N		( B_ROW2 + 12 )

#define PB		32
#define PGAP		2
#define PPAD		6
#define PW		( PPAD * 2 + 12 * ( PB + PGAP ) )
#define PH		( PPAD * 2 + 2 * PB + PGAP )

/* The tool each template button gives */
static CONST INT	row2[11] = {
	FT_TL_CURVE, FT_TL_LINE, FT_TL_ARC, FT_TL_CHORD, FT_TL_EARC, FT_TL_RECT, FT_TL_POLYGON,
	FT_TL_POLYLINE, FT_TL_TRIANGLE, FT_TL_ELLIPSE, FT_TL_TEXT
};

static CONST INT	zooms[9] = { 1, 2, 4, 8, 16, 24, 32, 40, 64 };
static CONST char *CONST zoom_words[9] = { "x1/8", "x1/4", "x1/2", "x1", "x2", "x3", "x4", "x5", "x8" };

static UI	t_ui;			/* the panel's window */
static BOOL	t_open = FALSE;
static INT	t_at_x, t_at_y;		/* where it came up */
static INT	t_shown[B_N];		/* the variant of each place shown, -1 not yet */
static UW	t_fill = 0xFFFFFFFFU, t_stroke = 0xFFFFFFFFU;
static UB	t_xy[32];
static BOOL	t_paper = FALSE;	/* 用紙枠 shown */

/* 実寸パネル: a window of the program's, subordinate to the figure's, its own size */
#define RS_W		260
#define RS_H		180

static TS_UUID	rs_win;
static ID	rs_kw = 0;
static INT	rs_gid = -1;

static BOOL rs_event( CONST T_OBNTM *m );

/* ---------------------------------------------------------------- the figure as the tools see it */

static CONST T_FTHOST	ft_proc;

static void view_of( T_FTVIEW *v );

/*
 * Letters let go over the figure, at x, y of the work area -- the corner
 * of the outline: into the piece of text that stands there, where the
 * corner is, or else into a new piece of text whose top left is there,
 * as wide as the letters in the 書式 palette's look, as the desktop's
 * figure editor takes them. E_PAR when the figure cannot be changed.
 */
#define TX_DROP_MIN	96		/* the least width of a piece made for letters */
#define TX_DROP_MAX	480

ER fgt_text_drop( INT x, INT y, CONST UB *s, INT n )
{
	T_FTVIEW	v;
	INT		i, fx, fy, w;
	ER		er;
	char		say[48];

	if ( g_fig == NULL || !g_ed.usable || s == NULL || n <= 0 || g_zoom <= 0 ) return E_PAR;
	fg_text_end();
	fx = ( x + g_sx ) * 8 / g_zoom;
	fy = ( y + g_sy ) * 8 / g_zoom;
	for ( i = g_fig->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*sh = &g_fig->sh[i];

		if ( sh->kind == TV_SH_DOC && sh->node != NULL && fx >= sh->r.left && fx < sh->r.right
		  && fy >= sh->r.top && fy < sh->r.bottom ) {
			break;
		}
	}
	if ( i >= 0 ) {
		/* a piece of text there: the letters go in where the corner is */
		fg_before();
		fg_text_begin(g_fig->sh[i].node, -1, -1);
	} else {
		/* room for the letters as the piece draws them, with a margin */
		w = ft_str_w((CONST char *)s, ft_look.font_size) * 2 + 32;
		w = ( w < TX_DROP_MIN ) ? TX_DROP_MIN : ( w > TX_DROP_MAX ) ? TX_DROP_MAX : w;
		ft_snap(&fx, &fy);
		view_of(&v);
		ft_make_text(&v, fx, fy, fx + w, fy + ft_look.font_size * 2 + 12);
		x = fx * g_zoom / 8 - g_sx;
		y = fy * g_zoom / 8 - g_sy;
	}
	if ( !fg_text_on() ) return E_NOMEM;
	er = fg_text_letters(x, y, s, n);
	fg_text_end();
	snprintf(say, sizeof(say), "text dropped %s (%d)", ( i >= 0 ) ? "into a piece" : "as a new piece", (INT)er);
	note(say);
	return er;
}

static void view_of( T_FTVIEW *v )
{
	fg_sync();
	v->ed = &g_ed;
	v->gid = g_gid;
	v->page.left = v->page.top = 0;
	v->page.right = g_w;
	v->page.bottom = g_h;
	v->sx = g_sx;
	v->sy = g_sy;
	v->host = &ft_proc;
	v->arg = &g_ed;
	v->paper_frame = t_paper;
}

static void h_draw( T_FTVIEW *v )
{
	(void)v;
	fg_paint_held();
}

static void h_show( T_FTVIEW *v )
{
	(void)v;
	(void)wm_obj_flush(g_kw, NULL);
	g_shown_px += (UD)g_w * (UD)g_h;
}

/* A part of the window put on the screen: where the lines of the shape being drawn moved */
static void h_show_part( T_FTVIEW *v, CONST T_DPRECT *r )
{
	T_DPRECT	u = *r;

	(void)v;
	if ( u.left < 0 ) u.left = 0;
	if ( u.top < 0 ) u.top = 0;
	if ( u.right > g_w ) u.right = g_w;
	if ( u.bottom > g_h ) u.bottom = g_h;
	if ( u.right <= u.left || u.bottom <= u.top ) return;
	(void)wm_obj_flush(g_kw, &u);
	g_shown_px += (UD)( u.right - u.left ) * (UD)( u.bottom - u.top );
}

/* The lines of the shape being drawn put over what was just drawn of the window, or taken off it */
void fgt_drawing_over( void )
{
	T_FTVIEW	v;

	if ( ft_drawing_arg() == NULL ) return;
	view_of(&v);
	ft_drawing_over(&v);
}

static void h_text_begin( T_FTVIEW *v, T_TADNODE *doc )
{
	(void)v;
	fg_text_begin(doc, -1, -1);
}

static UD h_dbl_ns( void )
{
	INT	ms = (INT)wm_look(WM_LOOK_DBLTIME);

	return (UD)( ( ms > 0 ) ? ms : 400 ) * 1000000ULL;
}

/* The magnification the 倍率 palette chose, taken by the figure: scrolled home */
static void h_zoom_set( T_FTVIEW *v )
{
	char	s[40];

	(void)v;
	g_zoom = ft_look.zoom;
	g_sx = g_sy = 0;
	fg_model();
	g_dirty = TRUE;
	snprintf(s, sizeof(s), "zoom %d", g_zoom * 100 / 8);
	note(s);
}

/* The 書式 palette's item, on the piece of text edited or on each of those picked */
static void h_text_look( T_FTVIEW *v, INT item )
{
	T_TADNODE	*nodes[32];
	INT		i, k, n = 0;

	(void)v;
	if ( g_fig == NULL ) return;
	if ( fg_text_on() ) {
		fg_text_look(item);
		return;
	}
	for ( i = 0; i < g_fig->nsh && n < 32; i++ ) {
		if ( g_pick[i] != 0 && g_fig->sh[i].kind == TV_SH_DOC && g_fig->sh[i].node != NULL ) {
			nodes[n++] = g_fig->sh[i].node;
		}
	}
	for ( k = 0; k < n; k++ ) {
		fg_text_begin(nodes[k], -1, -1);
		if ( fg_text_on() ) fg_text_look(item);
		fg_text_end();
	}
	/* the pieces picked again, as they were */
	for ( i = 0; n > 0 && g_fig != NULL && i < g_fig->nsh; i++ ) {
		for ( k = 0; k < n; k++ ) {
			if ( g_fig->sh[i].node == nodes[k] && g_pick[i] == 0 ) fg_pick_add(i);
		}
	}
}

/* A picture the record names, as the drawing reads it */
static ER h_pic_get( T_FTVIEW *v, CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h )
{
	(void)v;
	if ( knl_tv_src == NULL || knl_tv_src->picture == NULL ) return E_NOEXS;
	return knl_tv_src->picture(href, p_px, p_w, p_h, knl_tv_src_arg);
}

static void h_pic_done( T_FTVIEW *v, CONST UW *px )
{
	(void)v;
	if ( knl_tv_src != NULL && knl_tv_src->picture_done != NULL ) {
		knl_tv_src->picture_done(px, knl_tv_src_arg);
	}
}

/*
 * A picture written beside the record of the object its name gives
 * ("<uuid>_0_N.png"), as a PNG, and what was read of that object
 * forgotten, so that it is drawn as it now is
 */
static ER h_pic_put( T_FTVIEW *v, CONST UB *href, CONST UW *px, INT w, INT h )
{
	char	txt[TS_UUID_STRLEN + 1];
	TS_UUID	id;
	UB	*png = NULL;
	SZ	len = 0;
	ER	er;
	ID	k;

	(void)v;
	if ( href == NULL || strlen((CONST char *)href) <= TS_UUID_STRLEN ) return E_PAR;
	memcpy(txt, href, TS_UUID_STRLEN);
	txt[TS_UUID_STRLEN] = 0;
	if ( ts_str_to_uuid(txt, &id) < E_OK
	  || ( href[TS_UUID_STRLEN] != '_' && href[TS_UUID_STRLEN] != '.' ) ) return E_PAR;
	er = img_png_encode(px, w, h, &png, &len);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(&id, OB_OP_R | OB_OP_W);
	er = ( k > 0 ) ? ob_wri_res(k, href + TS_UUID_STRLEN, png, len) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	free(png);
	tv_src_forget(&id);
	{
		char	s[TS_UUID_STRLEN + 48];

		snprintf(s, sizeof(s), "picture written %s %d", (CONST char *)href, (INT)er);
		note(s);
	}
	return er;
}

static BOOL h_self_id( T_FTVIEW *v, TS_UUID *id )
{
	(void)v;
	*id = g_obj;
	return TRUE;
}

static void h_tell( T_FTVIEW *v, CONST char *msg )
{
	(void)v;
	tell(msg, NULL);
}

static CONST T_FTHOST	ft_proc = {
	.draw = h_draw, .show = h_show, .show_part = h_show_part, .text_begin = h_text_begin,
	.tools_shown = fgt_sync, .dbl_ns = h_dbl_ns,
	.zoom_set = h_zoom_set, .text_look = h_text_look,
	.pic_get = h_pic_get, .pic_done = h_pic_done, .pic_put = h_pic_put,
	.self_id = h_self_id, .tell = h_tell,
};

/* ---------------------------------------------------------------- the panel */

static INT button_tool( INT b )
{
	if ( b == B_SELECT ) return FT_TL_SELECT;
	if ( b == B_CANVAS ) return FT_TL_CANVAS;
	if ( b >= B_ROW2 && b < B_FONT ) return row2[b - B_ROW2];
	return -1;
}

/* How many of a place's buttons stand one over another, and which is to be shown now */
static INT variants( INT b )
{
	switch ( b ) {
	case B_LSTYLE: case B_CORNER: case B_FONT: case B_ZOOM:	return 1;
	case B_CANVAS:	return 4;
	case B_SELECT:	return 2;
	case B_GRID:	return 3;
	default:	return ( b >= B_ROW2 && b < B_FONT ) ? ( ft_centre_able(row2[b - B_ROW2]) ? 3 : 2 ) : 0;
	}
}

static INT variant_now( INT b )
{
	INT	t = button_tool(b), k;
	BOOL	in_hand = (BOOL)( t >= 0 && t == ft_tool );

	switch ( b ) {
	case B_CANVAS:	return ( in_hand ? 1 : 0 ) + ( ( ft_canvas_arg() != NULL ) ? 2 : 0 );
	case B_SELECT:	return in_hand ? 1 : 0;
	case B_ZOOM:
		for ( k = 0; k < 8 && zooms[k] != ft_look.zoom; k++ ) ;
		return k;		/* the words the one button wears (fgt_sync) */
	case B_GRID:
		return ( ft_look.grid_mode == FT_GRID_SNAP ) ? 2 : ( ft_look.grid_mode == FT_GRID_SHOW ) ? 1 : 0;
	default:
		if ( b >= B_ROW2 && b < B_FONT ) {
			if ( !in_hand ) return 0;
			return ( ft_centre_able(t) && ft_centre[t] ) ? 2 : 1;
		}
		return 0;
	}
}

/* The panel made to show what is now: each place's button, the swatches, where the pointer is */
void fgt_sync( void )
{
	INT		b, v, now, n;
	UW		pc, fill;
	CONST UH	*tex = NULL;
	BOOL		any = FALSE;

	if ( !t_open ) return;
	for ( b = B_LSTYLE; b < B_N; b++ ) {
		n = variants(b);
		now = variant_now(b);
		if ( n <= 1 || now == t_shown[b] ) continue;
		for ( v = 0; v < n; v++ ) {
			if ( v != now ) ui_show(&t_ui, T_BTN(b, v), FALSE);
		}
		ui_show(&t_ui, T_BTN(b, now), TRUE);
		t_shown[b] = now;
		any = TRUE;
	}
	now = variant_now(B_ZOOM);
	if ( now != t_shown[B_ZOOM] ) {
		/* the magnification in the words of the 倍率 button, as the desktop's panel shows it */
		ui_set_text(&t_ui, T_BTN(B_ZOOM, 0), zoom_words[now]);
		t_shown[B_ZOOM] = now;
		any = TRUE;
	}
	fill = !ft_look.fill_on ? 0x00FFFFFFU
	     : ( ft_look.fill_pat > 0 && tv_pattern(ft_look.fill_pat, &pc, &tex) ) ? pc : ft_look.fill;
	/* a swatch whose colour changes is shown anew, as the desktop's panel is made again: unmarked */
	if ( fill != t_fill ) {
		ui_set_swatch(&t_ui, T_FILL, fill, FALSE);
		t_fill = fill;
		any = TRUE;
	}
	if ( ft_look.stroke != t_stroke ) {
		ui_set_swatch(&t_ui, T_STROKE, ft_look.stroke, FALSE);
		t_stroke = ft_look.stroke;
		any = TRUE;
	}
	{
		UB	xy[32];

		ft_xy_words(xy);
		if ( strcmp((CONST char *)xy, (CONST char *)t_xy) != 0 ) {
			ui_set_text(&t_ui, T_XY, (CONST char *)xy);
			strcpy((char *)t_xy, (CONST char *)xy);
		}
	}
	if ( any ) note("tools shown");
}

/*
 * A window of the program's put with its corner at (x, y), whole on the
 * screen above the message line: when its foot would go below that and
 * over is not NOWHERE, it stands with its foot 4 above over instead.
 * Its size is the one the system gave it, its work area the panel's.
 */
#define NOWHERE		( -100000 )

static void place( UI *q, INT x, INT y, INT over )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		w, h, sw, sh;

	if ( ob_rea_rec(q->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	w = wp.right - wp.left;
	h = wp.bottom - wp.top;
	sw = ( wp.sw > 0 ) ? wp.sw : 1280;
	sh = ( wp.sh > 0 ) ? wp.sh - MSG_ROOM : 800;
	if ( over != NOWHERE && y + h > sh ) y = over - h - 4;
	if ( x + w > sw ) x = sw - w;
	if ( x < 0 ) x = 0;
	if ( y + h > sh ) y = sh - h;
	if ( y < 0 ) y = 0;
	if ( x == wp.left && y == wp.top ) return;
	wp.right = x + w;
	wp.bottom = y + h;
	wp.left = x;
	wp.top = y;
	(void)ob_wri_rec(q->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

/* Over the figure's window fw when there is room for it there, else under it, else where it is whole */
static void place_over( UI *q, CONST T_OBWPOS *fw )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		h, sh, y;

	if ( ob_rea_rec(q->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	h = wp.bottom - wp.top;
	sh = ( wp.sh > 0 ) ? wp.sh - MSG_ROOM : 800;
	y = fw->top - h - 4;
	if ( y < 0 && fw->bottom + 4 + h <= sh ) y = fw->bottom + 4;
	place(q, fw->left, y, NOWHERE);
}

/*
 * Where the panel was left for this figure, kept in the object's metadata
 * as TADjs keeps it ("window": {"panelpos": {"x", "y"}}, the panel
 * window's corner on the screen), as the desktop keeps it for its own
 * panel (om_store_panelpos): FALSE when there is none.
 */
static BOOL kept_place( INT *p_x, INT *p_y )
{
	T_JSON	root, w, t;
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(&g_obj, OB_OP_ATRRD);
	BOOL	got = FALSE;

	if ( k <= 0 ) return FALSE;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n > 0 && js_parse(m, n, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK
	  && js_get(&w, "panelpos", &t) >= E_OK ) {
		*p_x = (INT)js_get_num(&t, "x", 0);
		*p_y = (INT)js_get_num(&t, "y", 0);
		got = TRUE;
	}
	free(m);
	return got;
}

/* One number of "panelpos" written in place of what is there; FALSE when it has none */
static BOOL panelpos_put( UB **p_m, INT *p_n, CONST char *key, INT v )
{
	T_JSON	root, w, t, at;
	UB	*m = *p_m, *nm;
	INT	n = *p_n, a, e, k;
	char	txt[16];

	if ( js_parse(m, n, &root) < E_OK || js_get(&root, "window", &w) < E_OK
	  || js_get(&w, "panelpos", &t) < E_OK || js_get(&t, key, &at) < E_OK ) return FALSE;
	a = (INT)( at.s - m );
	e = a + at.len;
	k = snprintf(txt, sizeof(txt), "%d", v);
	nm = malloc((size_t)( n - at.len + k ) + 1);
	if ( nm == NULL ) return FALSE;
	memcpy(nm, m, (size_t)a);
	memcpy(nm + a, txt, (size_t)k);
	memcpy(nm + a + k, m + e, (size_t)( n - e ));
	n = n - at.len + k;
	nm[n] = 0;
	free(m);
	*p_m = nm;
	*p_n = n;
	return TRUE;
}

static void keep_place( INT x, INT y )
{
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(&g_obj, OB_OP_ATRRD | OB_OP_ATRWR);
	char	s[64];
	ER	er = E_NOEXS;

	if ( k <= 0 ) return;
	n = ob_atr_read(k, 0, &m);
	if ( n > 0 && panelpos_put(&m, &n, "x", x) && panelpos_put(&m, &n, "y", y) ) {
		er = ob_set_atr(k, m, (SZ)n);
	}
	free(m);
	ob_cls_obj(k);
	snprintf(s, sizeof(s), "tool panel kept %d,%d (%d)", x, y, (INT)er);
	note(s);
}

/*
 * Where it was left for this figure (its panelpos), unless that lies over
 * the figure window's title bar, by which it is carried and closed; else over the figure's window when there is room
 * for it there, else under it, else wherever on the screen it is whole,
 * as the desktop places its panel
 */
void fgt_show( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		b;
	char		s[48];
	UD		t0 = ta_now();

	if ( t_open || g_kw <= 0 ) return;
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		memset(&wp, 0, sizeof(wp));
		wp.top = wp.bottom = NOWHERE;
	}
	/*
	 * the keys stay with the figure's window, as the desktop's panel
	 * leaves them; made hidden, it comes up once its buttons show the
	 * tool in hand and the look (fgt_sync), as the desktop's is drawn
	 */
	ui_attr(WM_ATTR_NOFOCUS | UI_ATTR_HIDDEN);
	ui_sub(&g_win);
	if ( ui_open_on(&t_ui, &g_ui, SYSDEF_BOX_FIG, PN_TOOL, wp.left, wp.bottom + 4) < E_OK ) {
		note("the tool panel cannot be opened");
		return;
	}
	{
		T_OBWPOS	pp;
		SZ		psz = 0;
		INT		kx, ky, pw = 0, ph = 0;

		if ( ob_rea_rec(t_ui.kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &psz) >= E_OK ) {
			pw = pp.right - pp.left;
			ph = pp.bottom - pp.top;
		}
		if ( kept_place(&kx, &ky)
		  && !( kx < wp.right && kx + pw > wp.left && ky < wp.wtop && ky + ph > wp.top ) ) {
			place(&t_ui, kx, ky, NOWHERE);
		} else {
			place_over(&t_ui, &wp);
		}
	}
	t_open = TRUE;
	/* the presses of its parts too: a swatch's palette comes up on each press (fgt_event) */
	{
		T_OBNTF	req;

		memset(&req, 0, sizeof(req));
		req.events = OB_E_PRESS;
		if ( ob_ntf_evt(t_ui.pk, OB_REC_ANY, &req, g_ui.port) <= 0 ) note("the panel's presses are not told");
	}
	for ( b = 0; b < B_N; b++ ) t_shown[b] = -1;
	t_fill = t_stroke = 0xFFFFFFFFU;
	t_xy[0] = 0;
	ft_look.zoom = g_zoom;
	fgt_sync();
	ui_reveal(&t_ui);
	if ( ob_rea_rec(t_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		t_at_x = wp.left;
		t_at_y = wp.top;
		snprintf(s, sizeof(s), "tool panel at %d,%d", wp.left, wp.top);
		note(s);
	}
	snprintf(s, sizeof(s), "tool panel shown in %d us", (INT)( ( ta_now() - t0 ) / 1000 ));
	note(s);
}

void fgt_hide( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( !t_open ) return;
	/* where it stands, kept for the next time, once it has been moved from where it came up */
	if ( ob_rea_rec(t_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK
	  && ( wp.left != t_at_x || wp.top != t_at_y ) ) {
		keep_place(wp.left, wp.top);
	}
	ui_close(&t_ui);
	t_open = FALSE;
	note("tool panel closed");
}

void fgt_toggle( void )
{
	if ( t_open ) fgt_hide();
	else fgt_show();
}

BOOL fgt_open( void )
{
	return t_open;
}

/* ---------------------------------------------------------------- the palettes */

static void pop_grid( UI *q, CONST T_FTPOP *p )
{
	INT	gid = wm_obj_gid(q->kw);

	if ( gid < 0 ) return;
	ft_pop_grid_draw(gid, p);
	(void)wm_obj_flush(q->kw, NULL);
}

/* What the items hold, shown in the palette's parts */
static void pop_show( UI *q, CONST T_FTPOP *p )
{
	INT	i, k;
	UW	c;

	for ( i = 0; i < p->n; i++ ) {
		CONST T_FTITEM	*it = &p->it[i];

		switch ( it->type ) {
		case FT_PI_CHOICE:
			for ( k = 0; k < it->n; k++ ) ui_set_value(q, FT_PN_ITEM(i, k), ( *it->value == k ) ? 1 : 0);
			break;
		case FT_PI_CHECK:
			ui_set_value(q, FT_PN_ITEM(i, 0), *it->value ? 1 : 0);
			break;
		case FT_PI_TEXT:
			ui_set_text(q, FT_PN_ITEM(i, 0), (CONST char *)it->text);
			break;
		case FT_PI_COLOUR:
			if ( ft_hex_colour(it->text, &c) ) ui_set_value(q, FT_PN_ITEM(i, 0), (INT)c);
			break;
		case FT_PI_SWATCH:
			for ( k = 0; k < it->n; k++ ) {
				ui_set_swatch(q, FT_PN_ITEM(i, k), it->cols[k], (BOOL)( *it->col == it->cols[k] ));
			}
			break;
		default:
			break;
		}
	}
}

/*
 * Another colour chosen for a FT_PI_COLOUR item with the system's panel,
 * brought up under the part as the desktop's palette brings it up: what
 * it changes follows at once, and a row of swatches marks the colour
 * only if it has it.
 */
static void pop_pick( UI *q, T_FTPOP *p, INT i )
{
	T_FTITEM	*it = &p->it[i];
	T_OBWPOS	wp;
	T_DTREQ		rq;
	T_DTANS		an;
	SZ		asz = 0;
	UW		c = 0;
	INT		j, k;
	char		s[48];

	(void)ft_hex_colour(it->text, &c);
	ui_set_swatch(q, FT_PN_ITEM(i, 0), c, FALSE);	/* the part answers as a button does: no mark */
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_COLOUR;
	rq.win = q->win;		/* the palette's: it shows as having the input while the panel is up */
	rq.look.bgcol = c;
	if ( ob_rea_rec(q->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		rq.flags = DT_F_AT;
		rq.grab_x = wp.wleft + FT_LABEL_W;
		rq.grab_y = wp.wtop + it->r.top + FT_ROW_H - 3 + 2;
	}
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) < E_OK ) {
		note("colour kept");
		return;
	}
	c = an.look.bgcol & 0x00FFFFFFU;
	ft_colour_hex(c, it->text);
	if ( p->changed != NULL ) p->changed(i);
	for ( j = 0; j < p->n; j++ ) {
		if ( p->it[j].type != FT_PI_SWATCH ) continue;
		for ( k = 0; k < p->it[j].n; k++ ) {
			ui_set_swatch(q, FT_PN_ITEM(j, k), p->it[j].cols[k], (BOOL)( p->it[j].cols[k] == c ));
		}
	}
	ui_set_swatch(q, FT_PN_ITEM(i, 0), c, FALSE);
	snprintf(s, sizeof(s), "colour chosen %06X", (UINT)c);
	note(s);
}

/* The item of the palette whose part is the one a notice of its panel is of, or -1 */
static INT pop_part_item( UI *q, CONST T_FTPOP *p, CONST T_OBNTM *m )
{
	INT	k = m->recno - OB_WR_KIND, num, i;

	if ( k < 0 || k >= q->np ) return -1;
	num = q->p[k].num;
	i = num / 32 - 1;
	return ( i >= 0 && i < p->n ) ? i : -1;
}

/*
 * A part of the palette changed: what it holds taken into its item, and
 * the item handed on. The item, or -1; a push button ends the palette
 * with its item in *p_end.
 */
static INT pop_take( UI *q, T_FTPOP *p, INT num, BOOL button, INT *p_end )
{
	INT		i = num / 32 - 1, k = num % 32;
	T_FTITEM	*it;
	UW		c;

	if ( i < 0 || i >= p->n ) return -1;
	it = &p->it[i];
	switch ( it->type ) {
	case FT_PI_CHOICE:
		if ( ui_value(q, num) == 0 || *it->value == k ) return -1;
		*it->value = k;
		break;
	case FT_PI_CHECK:
		*it->value = ( ui_value(q, num) != 0 ) ? 1 : 0;
		break;
	case FT_PI_SWATCH:
		if ( k >= it->n || *it->col == it->cols[k] ) return -1;
		*it->col = it->cols[k];
		break;
	case FT_PI_BUTTON:
		if ( button ) *p_end = i;
		return -1;
	default:
		return -1;
	}
	if ( p->changed != NULL ) p->changed(i);
	/* a colour chosen from the swatches is the colour in hand */
	for ( k = 0; k < p->n; k++ ) {
		if ( p->it[k].type == FT_PI_COLOUR && ft_hex_colour(p->it[k].text, &c) ) {
			ui_set_value(q, FT_PN_ITEM(k, 0), (INT)c);
		}
	}
	return i;
}

/* The words typed into the palette's boxes taken, each that differs from its item */
static void pop_texts( UI *q, T_FTPOP *p )
{
	INT	i;
	char	buf[32];

	for ( i = 0; i < p->n; i++ ) {
		T_FTITEM	*it = &p->it[i];

		if ( it->type != FT_PI_TEXT ) continue;
		(void)ui_text(q, FT_PN_ITEM(i, 0), buf, sizeof(buf));
		if ( strcmp(buf, (CONST char *)it->text) == 0 ) continue;
		snprintf((char *)it->text, (size_t)it->max, "%s", buf);
		if ( p->changed != NULL ) p->changed(i);
	}
}

/*
 * A key the figure's window was told while the palette is up: the
 * palette's box's, whichever window has the keys -- a digit put after
 * its letters, Backspace taking the last away. The boxes take numbers.
 */
static void pop_typed( UI *q, T_FTPOP *p, UINT code )
{
	INT	i, n;
	char	buf[32];

	for ( i = 0; i < p->n && p->it[i].type != FT_PI_TEXT; i++ ) ;
	if ( i >= p->n ) return;
	(void)ui_text(q, FT_PN_ITEM(i, 0), buf, sizeof(buf));
	n = (INT)strlen(buf);
	if ( code == 0x2A ) {
		if ( n > 0 ) buf[n - 1] = 0;
	} else if ( code >= 0x1E && code <= 0x27 && n < p->it[i].max - 1 && n < (INT)sizeof(buf) - 1 ) {
		buf[n] = (char)( ( code == 0x27 ) ? '0' : '1' + ( code - 0x1E ) );
		buf[n + 1] = 0;
	} else {
		return;
	}
	ui_set_text(q, FT_PN_ITEM(i, 0), buf);
}

/* What a palette's change did to the figure, drawn now */
static void pop_after( void )
{
	if ( g_dirty ) fg_paint(TRUE);
	else if ( fg_damaged() ) (void)fg_paint_part(TRUE);
	fgt_sync();
}

/*
 * The palette of button under brought up under it and worked until it
 * is put away: the item of the push button that ended it, or -1.
 */
static INT pop_run( INT which, INT under )
{
	T_FTPOP		p;
	T_FTVIEW	v;
	T_OBWPOS	wp;
	T_OBNTM		m;
	UI		q;
	SZ		asz = 0;
	INT		x = 0, y = 0, num, ev, i, end = -1, col, row, redraw = 8;
	BOOL		done = FALSE;
	char		s[48];

	if ( !t_open || !ft_palette(which, &p) ) return -1;
	col = ( under < B_ROW2 ) ? under : under - B_ROW2;
	row = ( under < B_ROW2 ) ? 0 : 1;
	/* under its button, or over the panel when the screen ends first */
	if ( ob_rea_rec(t_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		x = wp.wleft + PPAD + col * ( PB + PGAP );
		y = wp.wtop + PPAD + row * ( PB + PGAP ) + PB + 4;
	} else {
		wp.top = NOWHERE;
	}
	ui_attr(WM_ATTR_NOFOCUS | UI_ATTR_UNTITLED);
	ui_sub(&t_ui.win);
	if ( ui_open_on(&q, &g_ui, SYSDEF_BOX_FIG, PN_POP + which, x, y) < E_OK ) {
		note("the palette cannot be opened");
		return -1;
	}
	place(&q, x, y, wp.top);
	/* the presses of its parts too: a colour's part brings up the system's panel on each press */
	{
		T_OBNTF	req;

		memset(&req, 0, sizeof(req));
		req.events = OB_E_PRESS;
		(void)ob_ntf_evt(q.pk, OB_REC_ANY, &req, g_ui.port);
	}
	pop_show(&q, &p);
	view_of(&v);
	ft_pal_on(&v);
	snprintf(s, sizeof(s), "palette %d open", which);
	note(s);
	pop_grid(&q, &p);
	while ( !done ) {
		ER	er = ui_wait(&g_ui, &m, ( redraw > 0 ) ? 100 : TMO_FEVR);

		if ( er == E_TMOUT ) {
			/* the panel is drawn by the system as it comes up: the patterns over it after */
			redraw--;
			pop_grid(&q, &p);
			continue;
		}
		if ( er < E_OK ) break;
		if ( ui_mine(&q, &m) ) {
			if ( m.event == OB_E_PRESS && memcmp(&m.uuid, &q.pnl, sizeof(TS_UUID)) == 0 ) {
				/* a part pressed: a colour's part chooses its colour, the others are worked as told */
				i = pop_part_item(&q, &p, &m);
				if ( m.code == 0 && i >= 0 && p.it[i].type == FT_PI_COLOUR ) {
					pop_pick(&q, &p, i);
					view_of(&v);
					pop_after();
					pop_grid(&q, &p);
				}
				continue;
			}
			if ( m.event == OB_E_PRESS && m.code == 0
			  && ( i = ft_pop_grid_press(&p, m.x, m.y) ) >= 0 ) {
				if ( p.changed != NULL ) p.changed(i);
				view_of(&v);
				pop_grid(&q, &p);
				pop_after();
				continue;
			}
			ev = ui_event(&q, &m, &num);
			if ( ev == UI_EV_CHANGE || ev == UI_EV_BUTTON ) {
				(void)pop_take(&q, &p, num, (BOOL)( ev == UI_EV_BUTTON ), &end);
				view_of(&v);
				if ( end >= 0 ) done = TRUE;
				pop_after();
			} else if ( ev == UI_EV_CLOSE ) {
				done = TRUE;
			} else if ( ev == UI_EV_KEY && ( m.mods & OB_KEY_UP ) == 0
				 && ( m.code == KEY_ENTER || m.code == KEY_ESC || m.code == 0x58 ) ) {
				done = TRUE;
			}
			pop_grid(&q, &p);
			continue;
		}
		/* a press on the figure or on the panel puts it away; keys meant for it are its own */
		if ( m.event == OB_E_PRESS ) {
			done = TRUE;
		} else if ( m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0
			 && ( m.code == KEY_ENTER || m.code == KEY_ESC ) ) {
			done = TRUE;
		} else if ( m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0 ) {
			pop_typed(&q, &p, m.code);
		} else if ( m.event == OB_E_REDRAW && memcmp(&m.uuid, &g_win, sizeof(TS_UUID)) == 0 ) {
			g_dirty = TRUE;
			pop_after();
		}
	}
	/* a box being typed in is taken as it stands */
	pop_texts(&q, &p);
	ft_pal_on(NULL);
	ui_close(&q);
	pop_after();
	snprintf(s, sizeof(s), "palette %d closed (%d)", which, end);
	note(s);
	return end;
}

/* A tool taken from the panel's button b, or a palette brought up from it */
static void panel_do( INT b )
{
	INT	t = button_tool(b);
	char	s[40];

	if ( t == FT_TL_CANVAS && ft_tool == FT_TL_CANVAS && ft_canvas_arg() != NULL ) {
		/* painting: the canvas button is the 画材 */
		(void)pop_run(FT_PAL_PAINT, B_CANVAS);
		return;
	}
	if ( t >= 0 ) {
		if ( t != ft_tool && ft_canvas_arg() != NULL ) {
			T_FTVIEW	v;

			view_of(&v);
			ft_canvas_leave(&v);
			note("canvas left");
		}
		ft_take(t);
		fgt_sync();
		snprintf(s, sizeof(s), "tool %d%s", ft_tool, ( ft_centre_able(ft_tool) && ft_centre[ft_tool] ) ? " centre" : "");
		note(s);
		return;
	}
	switch ( b ) {
	case B_FILL:
		if ( pop_run(FT_PAL_FILL, B_FILL) == FT_FILL_EDIT ) fpe_open(ft_look.fill_pat);
		break;
	case B_STROKE:	(void)pop_run(FT_PAL_STROKE, B_STROKE);	break;
	case B_LSTYLE:	(void)pop_run(FT_PAL_LSTYLE, B_LSTYLE);	break;
	case B_CORNER:	(void)pop_run(FT_PAL_CORNER, B_CORNER);	break;
	case B_ZOOM:	(void)pop_run(FT_PAL_ZOOM, B_ZOOM);	break;
	case B_GRID:	(void)pop_run(FT_PAL_GRID, B_GRID);	break;
	case B_FONT:	(void)pop_run(FT_PAL_FONT, B_FONT);	break;
	default:	break;
	}
}

/* A notice of the panel's window: TRUE when it was one */
BOOL fgt_event( CONST T_OBNTM *m )
{
	INT	num, ev;

	if ( rs_event(m) ) return TRUE;
	if ( !t_open || !ui_mine(&t_ui, m) ) return FALSE;
	if ( m->event == OB_E_PRESS && m->code == 0 ) {
		/*
		 * A swatch brings up its palette as it is pressed, every time:
		 * its value says only that it was chosen, so the press of the
		 * part is what is told
		 */
		INT	i = m->recno - OB_WR_KIND;

		num = ( i >= 0 && i < t_ui.np ) ? t_ui.p[i].num : 0;
		if ( num == T_FILL || num == T_STROKE ) {
			/* marked while its palette is up, as the one being worked */
			ui_set_swatch(&t_ui, num, ( num == T_FILL ) ? t_fill : t_stroke, TRUE);
			panel_do(( num == T_FILL ) ? B_FILL : B_STROKE);
			ui_set_swatch(&t_ui, num, ( num == T_FILL ) ? t_fill : t_stroke, FALSE);
		}
		return TRUE;
	}
	ev = ui_event(&t_ui, m, &num);
	if ( ev == UI_EV_BUTTON && num >= 100 && num < 100 + B_N * 10 ) {
		panel_do(( num - 100 ) / 10);
	} else if ( ev == UI_EV_CLOSE ) {
		fgt_hide();
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the pointer in the figure */

/* A press in the figure with a drawing tool in hand: TRUE when the tool took it */
BOOL fgt_press( CONST T_OBNTM *m )
{
	T_FTVIEW	v;

	if ( g_fig == NULL ) return FALSE;
	view_of(&v);
	if ( ft_tool != FT_TL_SELECT ) return ft_press(&v, m->x, m->y, m->when, m->mods);
	/* with the pointer in hand: a handle of 変形, or a picked line's end, is carried */
	if ( ft_transform_press(&v, m->x, m->y, wm_hit_slop(m->when)) ) {
		note("transform handle held");
		return TRUE;
	}
	if ( g_ed.usable && ft_end_press(&v, m->x, m->y, wm_hit_slop(m->when)) ) {
		note("line end held");
		return TRUE;
	}
	return FALSE;
}

/*
 * A second press with the pointer in hand on what is not a piece of
 * text (x, y the window's, fx, fy the figure's): on a picture painted in
 * the figure, the canvas taken and painting in it begun; on a shape whose
 * points can be moved, that shape alone taken and 変形 begun. TRUE when
 * it was one of them.
 */
BOOL fgt_twice( INT x, INT y, INT fx, INT fy, INT i )
{
	T_FTVIEW	v;
	T_DPRECT	page;
	INT		which = -1;
	BOOL		r;

	if ( g_fig == NULL ) return FALSE;
	view_of(&v);
	if ( i >= 0 && i < g_fig->nsh && g_fig->sh[i].kind == TV_SH_PIXMAP ) {
		r = ft_canvas_open(&v, fx, fy);
		note("canvas taken");
		return r;
	}
	page.left = page.top = 0;
	page.right = g_w;
	page.bottom = g_h;
	if ( tv_fig_shape_at(g_fig, &page, g_sx, g_sy, x, y, &which) >= E_OK
	  && which >= 0 && which < g_fig->nsh
	  && ft_transformable(&g_fig->sh[which]) && fe_takeable(&g_ed, which) ) {
		fg_pick_none();
		fg_pick_add(which);
		view_of(&v);
		if ( !ft_transform(&v) ) tell("この図形は変形できません", NULL);
		else note("transform begun");
		g_dirty = TRUE;
		return TRUE;
	}
	return FALSE;
}

/* Something the tools carry: a shape being drawn, a line's end, a handle of 変形 */
BOOL fgt_drawing( void )
{
	return (BOOL)( ft_drawing_arg() != NULL || ft_end_arg() != NULL || ft_transform_carrying()
		    || ft_canvas_painting() );
}

void fgt_moved( INT x, INT y, UINT mods )
{
	T_FTVIEW	v;

	view_of(&v);
	if ( ft_end_arg() != NULL ) ft_end_follow(&v, x, y);
	else if ( ft_transform_carrying() ) ft_transform_follow(&v, x, y);
	else if ( ft_drawing_arg() == NULL && ft_canvas_painting() ) ft_canvas_follow(&v, x, y);
	else ft_follow(&v, x, y, mods);
}

void fgt_released( void )
{
	T_FTVIEW	v;

	view_of(&v);
	if ( ft_end_arg() != NULL ) {
		ft_end_release(&v);
		note("line end put");
	} else if ( ft_transform_carrying() ) {
		ft_transform_release(&v);
		note("transformed");
	} else if ( ft_drawing_arg() == NULL && ft_canvas_painting() ) {
		ft_canvas_release(&v);
		note("painted");
	} else {
		ft_release(&v);
		note("drawn");
	}
}

/*
 * Enter or Escape ends a polygon or a polyline; Escape abandons any
 * other shape being drawn, and leaves 変形
 */
BOOL fgt_key( CONST T_OBNTM *m )
{
	T_FTVIEW	v;

	view_of(&v);
	if ( m->code == KEY_ESC && ft_drawing_arg() == NULL && ft_canvas_arg() != NULL ) {
		ft_canvas_leave(&v);
		note("canvas left");
		return TRUE;
	}
	if ( m->code == KEY_ESC && ft_drawing_arg() == NULL && ft_transform_on(&v) ) {
		ft_transform_end(&v);
		note("transform ended");
		return TRUE;
	}
	if ( ft_drawing_arg() == NULL ) return FALSE;
	return ft_key(&v, m->code);
}

/* The pointer over the figure, nothing held: where it is, for the panel */
void fgt_hover( INT x, INT y )
{
	T_FTVIEW	v;

	if ( !t_open ) return;
	view_of(&v);
	ft_hover(&v, x, y);
}

/* What was picked gives the palettes its look */
void fgt_picked( void )
{
	fg_sync();
	if ( ft_take_look(&g_ed) ) fgt_sync();
}

/* The grid over the figure, when it is shown */
void fgt_overlay( void )
{
	T_FTVIEW	v;

	if ( fg_cabinet ) return;	/* a cabinet's figure is not edited: no grid, paper or handles */
	view_of(&v);
	ft_grid_draw(&v);
	ft_canvas_draw(&v, ( g_paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : g_paper);
	ft_overlay(&v);
	rs_draw();
}

/* ---------------------------------------------------------------- 実寸パネル */

/* The figure at its own size in the 実寸 window, from where the figure's window is scrolled to */
void rs_draw( void )
{
	T_FTVIEW	v;
	T_OBWPOS	wp;
	T_DPRECT	all;
	SZ		asz = 0;

	if ( rs_kw <= 0 || rs_gid < 0 || g_fig == NULL ) return;
	if ( ob_rea_rec(rs_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	all.left = all.top = 0;
	all.right = wp.wright - wp.wleft;
	all.bottom = wp.wbottom - wp.wtop;
	view_of(&v);
	(void)dp_set_visible(rs_gid, &all);
	ft_realsize_draw(&v, rs_gid, &all, ( g_paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : g_paper);
	(void)wm_obj_flush(rs_kw, NULL);
}

static void rs_close( void )
{
	if ( rs_kw <= 0 ) return;
	ob_cls_obj(rs_kw);
	(void)ob_del_obj(&rs_win);
	rs_kw = 0;
	rs_gid = -1;
	note("realsize closed");
}

/* 実寸 shown at the foot of the figure's window, against its right, or taken away */
static void rs_toggle( void )
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[160], sub[TS_UUID_STRLEN + 1];
	INT		r, b;

	if ( rs_kw > 0 ) {
		rs_close();
		return;
	}
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	r = wp.right;
	b = wp.bottom;
	ts_uuid_text(&g_win, sub);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"sub\":\"%s\"}",
		 r - RS_W - 8, b - RS_H - 30, r, b, WM_ATTR_FRAME | WM_ATTR_TITLE, sub);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"実寸";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &rs_win) < E_OK ) {
		note("realsize cannot be opened");
		return;
	}
	rs_kw = ob_opn_obj(&rs_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	rs_gid = ( rs_kw > 0 ) ? wm_obj_gid(rs_kw) : -1;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_REDRAW;
	if ( rs_kw <= 0 || rs_gid < 0 || ob_ntf_evt(rs_kw, OB_REC_ANY, &req, g_ui.port) <= 0 ) {
		rs_close();
		return;
	}
	rs_draw();
	/* the 道具パネル stays in front of 実寸, as the panel of the figure it was given first */
	if ( t_open && ob_rea_rec(t_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.z = 0;
		(void)ob_wri_rec(t_ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	note("realsize open");
}

/* A notice of the 実寸 window: TRUE when it was one */
static BOOL rs_event( CONST T_OBNTM *m )
{
	if ( rs_kw <= 0 || memcmp(&m->uuid, &rs_win, sizeof(TS_UUID)) != 0 ) return FALSE;
	if ( m->event == OB_E_CLOSE || m->event == OB_E_DELETE ) rs_close();
	else if ( m->event == OB_E_REDRAW ) rs_draw();
	return TRUE;
}

/* ---------------------------------------------------------------- 用紙設定 */

/* 用紙設定: the panel of the data box (TA_PN_PAPER, tvapp.h) */
INT fgt_paper_form( T_DEPAPER *pp )
{
	return ta_paper_form(pp);
}

/* ---------------------------------------------------------------- the menu's of the tools */

/* 変形, 用紙枠, 実寸, 用紙枠設定 from the menu: TRUE when code was one of them */
BOOL fgt_command( CONST char *code )
{
	T_FTVIEW	v;

	if ( strcmp(code, "transform") == 0 ) {
		view_of(&v);
		if ( g_fig == NULL || !ft_transform(&v) ) tell("この図形は変形できません", NULL);
		else note("transform begun");
	} else if ( strcmp(code, "paper") == 0 ) {
		t_paper = (BOOL)!t_paper;
		g_dirty = TRUE;
		note(t_paper ? "paper frame shown" : "paper frame hidden");
	} else if ( strcmp(code, "realsize") == 0 ) {
		rs_toggle();
	} else if ( strcmp(code, "burn") == 0 ) {
		view_of(&v);
		ft_burn(&v);
		g_npick = g_ed.npick;
		fg_sync();
		g_dirty = TRUE;
		note("burnt");
	} else if ( strcmp(code, "pagesetup") == 0 ) {
		T_DEPAPER	ps;
		INT		r;

		if ( g_fig == NULL || !g_ed.usable ) return TRUE;
		view_of(&v);
		ft_paper_of(&v, &ps);
		r = fgt_paper_form(&ps);
		view_of(&v);
		ft_paper_put(&v, &ps, r);
		g_dirty = TRUE;
	} else {
		return FALSE;
	}
	return TRUE;
}
