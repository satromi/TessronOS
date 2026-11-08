/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtftool.c
 *	The figure editor's 道具パネル and what is drawn with it (design 17.13)
 *
 *	The panel is a small window of two rows. The first holds the
 *	palettes -- what the inside is filled with, the colour of lines,
 *	the kind and width of line and its arrows, the corners -- and the
 *	canvas, the pointer for picking, the magnification, the grid and
 *	where the pointer is. The second holds the templates: freehand
 *	curve, line, sector, chord, arc, rectangle, polygon, polyline,
 *	triangle, ellipse, a box of text, and the look of text.
 *
 *	A palette comes up as a small window of its own under its button,
 *	and what is chosen in it takes effect at once: on the shapes that
 *	are picked, and on those drawn afterwards. Pressing anywhere else
 *	puts it away.
 *
 *	A shape is drawn by pressing and dragging out its box, or, for a
 *	polygon or a polyline, by pressing each corner in turn. What is drawn is written
 *	into the record in the form the record's other readers read: the
 *	colours as pattern numbers -- a fixed one when the colour is one of
 *	them, else one of the record's own in its <patterns> -- and the
 *	arcs as their frame and the two points their ends are towards.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/fn.h>
#include <ts/look.h>
#include <ts/docmenu.h>
#include <ts/sysdef.h>
#include <ts/figedit.h>
#include <ts/figtool.h>
#include "desktop.h"
#ifdef USE_UITEST
#include <ts/time.h>
#include <tm/tmonitor.h>
#endif

#define KEY_BS		0x2A
#define KEY_ENTER	0x28
#define KEY_ESC		0x29

#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- the look and the tools */

/*
 * The look, the tool in hand, the shape being drawn and the palettes'
 * items are the figure editor's code's (application/fig/figtool.c):
 * here are the windows they are shown and worked in.
 */
#define fl		ft_look
#define df_tool		ft_tool
#define df_centre	ft_centre
#define centre_able	ft_centre_able
#define df_have_xy	ft_have_xy
#define df_px		ft_px
#define df_py		ft_py
#define put_num		ft_put_num
#define colour_hex	ft_colour_hex
#define hex_colour	ft_hex_colour
#define same_word	ft_same_word
#define snap		ft_snap
#define str_w		ft_str_w
#define xy_words	ft_xy_words
#define frame_put	ft_frame_put
#define join_put	ft_join_put

#define TL_SELECT	FT_TL_SELECT
#define TL_CANVAS	FT_TL_CANVAS
#define TL_CURVE	FT_TL_CURVE
#define TL_LINE		FT_TL_LINE
#define TL_ARC		FT_TL_ARC
#define TL_CHORD	FT_TL_CHORD
#define TL_EARC		FT_TL_EARC
#define TL_RECT		FT_TL_RECT
#define TL_POLYGON	FT_TL_POLYGON
#define TL_POLYLINE	FT_TL_POLYLINE
#define TL_TRIANGLE	FT_TL_TRIANGLE
#define TL_ELLIPSE	FT_TL_ELLIPSE
#define TL_TEXT		FT_TL_TEXT

#define GRID_NONE	FT_GRID_NONE
#define GRID_SHOW	FT_GRID_SHOW
#define GRID_SNAP	FT_GRID_SNAP

LOCAL void panel_draw( void );

/* A figure's window as the figure editor's code sees it */
LOCAL CONST T_FTHOST	ft_desk;

LOCAL void view_of( DTWIN *d, T_FTVIEW *v, T_FGED *e )
{
	df_ed_of(d, e);
	v->ed = e;
	v->gid = wm_gid(d->wid);
	dt_work_rect(d, &v->page);
	v->sx = d->scroll_x;
	v->sy = d->scroll_y;
	v->host = &ft_desk;
	v->arg = d;
	v->paper_frame = d->paper_frame;
}

LOCAL void h_draw( T_FTVIEW *v )
{
	df_ed_back(v->ed);
	dt_draw((DTWIN *)v->arg);
}

LOCAL void h_show( T_FTVIEW *v )
{
	DTWIN		*d = (DTWIN *)v->arg;
	T_WMWIN		w;
	T_DPRECT	all;

	if ( wm_ref(d->wid, &w) >= E_OK ) {
		all.left = 0;  all.top = 0;
		all.right = w.work.right - w.work.left;
		all.bottom = w.work.bottom - w.work.top;
		wm_damage(d->wid, &all);
	}
	wm_update();
}

/* A part of the window put on the screen: where the lines of the shape being drawn moved */
LOCAL void h_show_part( T_FTVIEW *v, CONST T_DPRECT *r )
{
	DTWIN	*d = (DTWIN *)v->arg;

	wm_damage(d->wid, r);
	wm_update();
}

/* The lines of the shape being drawn in d put over what dt_draw drew of it last */
EXPORT void df_drawing_over( DTWIN *d )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || ft_drawing_arg() != (void *)d ) {
		return;
	}
	view_of(d, &v, &e);
	ft_drawing_over(&v);
}

LOCAL void h_text_begin( T_FTVIEW *v, T_TADNODE *doc )
{
	df_ed_back(v->ed);
	df_text_begin((DTWIN *)v->arg, doc, -1, -1);
	df_ed_of((DTWIN *)v->arg, v->ed);
}

LOCAL void h_pointer( BOOL picking )
{
	wm_set_pointer(picking ? WM_PT_SELECT : WM_PT_MODIFY);
}

LOCAL UD h_dbl_ns( void )
{
	return (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
}

LOCAL void h_zoom_set( T_FTVIEW *v )
{
	DTWIN	*d = (DTWIN *)v->arg;

	if ( d != NULL && d->used ) {
		d->scroll_x = d->scroll_y = 0;
		if ( d->fig != NULL ) {
			d->fig->zoom = fl.zoom;
		}
	}
}

LOCAL void text_look( INT item );

LOCAL void h_text_look( T_FTVIEW *v, INT item )
{
	(void)v;
	text_look(item);
}

LOCAL CONST T_FTHOST	ft_desk = {
	.draw = h_draw, .show = h_show, .show_part = h_show_part, .text_begin = h_text_begin,
	.tools_shown = panel_draw, .pointer = h_pointer, .dbl_ns = h_dbl_ns,
	.zoom_set = h_zoom_set, .text_look = h_text_look,
	.pic_get = df_pic_get, .pic_done = df_pic_done, .pic_put = df_pic_put,
	.self_id = df_self_id, .tell = df_tell_view,
};

/* A figure's window as the figure editor's code sees it, for the canvas (dtfcanvas.c) */
EXPORT void df_view_of( DTWIN *d, T_FTVIEW *v, T_FGED *e )
{
	view_of(d, v, e);
}

/* The figure's places, and the pointer's shown in the panel, for the ends and 変形 */
LOCAL void to_fig( DTWIN *d, INT x, INT y, INT *fx, INT *fy )
{
	T_DPRECT	page;

	dt_work_rect(d, &page);
	*fx = ( x - page.left + d->scroll_x ) * 8 / fl.zoom;
	*fy = ( y - page.top + d->scroll_y ) * 8 / fl.zoom;
}

/* A box of text made in the figure, edited where it stands */
LOCAL void make_text( DTWIN *d, INT l, INT t, INT r, INT b )
{
	T_FTVIEW	v;
	T_FGED		e;

	view_of(d, &v, &e);
	ft_make_text(&v, l, t, r, b);
	df_ed_back(&e);
}

/* ---------------------------------------------------------------- the pointer in a figure */

EXPORT BOOL df_tool_press( DTWIN *d, INT x, INT y, UD when, UINT mods )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( !df_is(d) || df_tool == TL_SELECT ) {
		return FALSE;
	}
	if ( ft_canvas_arg() != NULL && ft_canvas_arg() != (void *)d ) {
		df_canvas_leave();		/* painting in another window ends there first */
	}
	view_of(d, &v, &e);
	r = ft_press(&v, x, y, when, mods);
	df_ed_back(&e);
	return r;
}

EXPORT BOOL df_drawing_now( void )
{
	return (BOOL)( ft_drawing_arg() != NULL );
}

EXPORT void df_tool_follow( INT sx, INT sy, UINT mods )
{
	DTWIN		*d = (DTWIN *)ft_drawing_arg();
	T_WMWIN		w;
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	view_of(d, &v, &e);
	ft_follow(&v, sx - w.work.left, sy - w.work.top, mods);
	df_ed_back(&e);
}

EXPORT void df_tool_release( void )
{
	DTWIN		*d = (DTWIN *)ft_drawing_arg();
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL ) {
		return;
	}
	view_of(d, &v, &e);
	ft_release(&v);
	df_ed_back(&e);
}


/*
 * Enter or Escape ends a polygon, closed, or a polyline, open; Escape
 * abandons any other shape being drawn, and leaves painting and 変形.
 */
EXPORT BOOL df_tool_key( DTWIN *d, UINT code )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( d != NULL && code == KEY_ESC && ft_drawing_arg() != (void *)d ) {
		if ( df_canvas_active() ) {
			df_canvas_leave();
			return TRUE;
		}
		if ( ft_transform_arg() == (void *)d ) {
			df_transform_end(d);
			return TRUE;
		}
		return FALSE;
	}
	if ( d == NULL || ft_drawing_arg() != (void *)d ) {
		return FALSE;
	}
	view_of(d, &v, &e);
	r = ft_key(&v, code);
	df_ed_back(&e);
	return r;
}

/* The pointer over a figure, with nothing held: where it is, for the panel */
EXPORT void df_hover( DTWIN *d, INT x, INT y )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( !df_is(d) || df_panel_wid() <= 0 ) {
		return;
	}
	view_of(d, &v, &e);
	ft_hover(&v, x, y);
}

/* The grid's points over the figure, when it is shown */
EXPORT void df_grid_draw( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( !df_is(d) ) {
		return;
	}
	view_of(d, &v, &e);
	v.gid = gid;
	v.page = *page;
	ft_grid_draw(&v);
}

/* What a figure is magnified by, in eighths */
EXPORT INT df_zoom( void )
{
	return fl.zoom;
}

/*
 * The palettes follow the shape picked: what it is drawn with is what
 * the next shape will be, until something else is chosen.
 */
EXPORT void df_take_look( DTWIN *d )
{
	T_FGED	e;

	if ( d == NULL || d->fig == NULL ) {
		return;
	}
	df_ed_of(d, &e);
	if ( ft_take_look(&e) ) {
		panel_draw();
	}
}


/* ---------------------------------------------------------------- a line's ends */

/* A picked line's end carried with the pointer in hand (figtool.c) */
EXPORT BOOL df_end_press( DTWIN *d, INT x, INT y, UD when )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( !df_is(d) || d->sealed ) {
		return FALSE;
	}
	view_of(d, &v, &e);
	r = ft_end_press(&v, x, y, wm_hit_slop(when));
	df_ed_back(&e);
	return r;
}

EXPORT BOOL df_end_carrying( void )
{
	return (BOOL)( ft_end_arg() != NULL );
}

EXPORT void df_end_follow( INT sx, INT sy )
{
	DTWIN		*d = (DTWIN *)ft_end_arg();
	T_WMWIN		w;
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	view_of(d, &v, &e);
	ft_end_follow(&v, sx - w.work.left, sy - w.work.top);
	df_ed_back(&e);
}

EXPORT void df_end_release( void )
{
	DTWIN		*d = (DTWIN *)ft_end_arg();
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL ) {
		return;
	}
	if ( !d->used ) {
		ft_forget(d);
		return;
	}
	view_of(d, &v, &e);
	ft_end_release(&v);
	df_ed_back(&e);
}

/* ---------------------------------------------------------------- the panel */

/*
 * The buttons: the first row the palettes, the canvas, the pointer, the
 * magnification and the grid, and after them where the pointer is; the
 * second the templates and the look of text. They are the parts of a
 * panel that fills the window (the window manager draws and works
 * them): the inside's colour and the colour of lines are swatches, the
 * tools and the templates are buttons with pictures (PNG resources of
 * the data box 基本図形編集, SYSDEF_BOX_FIG), the one in hand drawn
 * with the heavy edge, and its picture with a red mark while it draws
 * from the middle. The panel is made again whenever what it shows
 * changes; where the pointer is is only written into its words.
 */
#define B_FILL		0
#define B_STROKE	1
#define B_LSTYLE	2
#define B_CORNER	3
#define B_CANVAS	4
#define B_SELECT	5
#define B_ZOOM		6
#define B_GRID		7
#define B_ROW2		8		/* the templates start here */
#define B_FONT		( B_ROW2 + 11 )
#define B_N		( B_ROW2 + 12 )

#define N_XY		100		/* the words of where the pointer is */

/* The tool each template button gives */
LOCAL CONST INT	df_row2[11] = {
	TL_CURVE, TL_LINE, TL_ARC, TL_CHORD, TL_EARC, TL_RECT, TL_POLYGON,
	TL_POLYLINE, TL_TRIANGLE, TL_ELLIPSE, TL_TEXT
};

/* The pictures, by their resource numbers */
#define PIC_SELECT	0
#define PIC_CANVAS	1
#define PIC_BRUSH	2
#define PIC_LSTYLE	3
#define PIC_CORNER	4
#define PIC_GRID	5		/* 5..7: none, shown, snapped to */
#define PIC_ROW2	8		/* 8..17: the templates but the polyline */
#define PIC_CENTRE	18		/* 18..23: line, sector, chord, arc, rectangle, ellipse drawing from the middle */
#define PIC_POLYLINE	24

/* The picture of each template button, as df_row2 has them */
LOCAL CONST INT	df_row2_pict[11] = {
	PIC_ROW2 + 0, PIC_ROW2 + 1, PIC_ROW2 + 2, PIC_ROW2 + 3, PIC_ROW2 + 4, PIC_ROW2 + 5,
	PIC_ROW2 + 6, PIC_POLYLINE, PIC_ROW2 + 7, PIC_ROW2 + 8, PIC_ROW2 + 9
};

#define PB		32		/* a button */
#define PGAP		2
#define PPAD		6
#define PW		( PPAD * 2 + 12 * ( PB + PGAP ) )
#define PH		( PPAD * 2 + 2 * PB + PGAP )

LOCAL INT	df_panel = 0;		/* its window */
LOCAL INT	df_pid = -1;		/* the panel of parts in it */
LOCAL BOOL	df_panel_moving = FALSE;
LOCAL BOOL	df_pressing = FALSE;	/* a button of it held in */
LOCAL INT	df_grab_dx, df_grab_dy;

/* What the panel shows, to see when it has to be made again */
typedef struct {
	INT	tool;
	BOOL	centre, canvas;
	UW	fill, stroke;
	BOOL	fill_on;
	INT	fill_pat, zoom, grid_mode;
} DFSIG;

LOCAL DFSIG	df_sig;
LOCAL BOOL	df_sig_ok = FALSE;

LOCAL void button_rect( INT b, T_DPRECT *r )
{
	INT	row = ( b < B_ROW2 ) ? 0 : 1;
	INT	col = ( b < B_ROW2 ) ? b : b - B_ROW2;

	r->left = PPAD + col * ( PB + PGAP );
	r->top = PPAD + row * ( PB + PGAP );
	r->right = r->left + PB;
	r->bottom = r->top + PB;
}

LOCAL INT button_tool( INT b )
{
	if ( b == B_SELECT ) return TL_SELECT;
	if ( b == B_CANVAS ) return TL_CANVAS;
	if ( b >= B_ROW2 && b < B_FONT ) return df_row2[b - B_ROW2];

	return -1;
}

LOCAL void pict_put( T_WMPART *pt, INT n )
{
	CONST char	*box = SYSDEF_BOX_FIG;
	UB		num[12];
	INT		k = 0, i;

	for ( i = 0; box[i] != 0 && k < WM_LABEL_MAX - 16; i++ ) pt->text[k++] = (UB)box[i];
	pt->text[k++] = '_';
	pt->text[k++] = '0';
	pt->text[k++] = '_';
	(void)put_num(num, 0, n);
	for ( i = 0; num[i] != 0; i++ ) pt->text[k++] = num[i];
	pt->text[k++] = '.';
	pt->text[k++] = 'p';
	pt->text[k++] = 'n';
	pt->text[k++] = 'g';
	pt->text[k] = 0;
	pt->label[0] = 0;
	knl_pn_pict_load(pt->text);
}

/* The picture a template button wears, its mark while it draws from the middle */
LOCAL INT template_pict( INT b )
{
	INT	t = df_row2[b - B_ROW2];

	if ( t == df_tool && centre_able(t) && df_centre[t] ) {
		switch ( t ) {
		case TL_LINE:		return PIC_CENTRE + 0;
		case TL_ARC:		return PIC_CENTRE + 1;
		case TL_CHORD:		return PIC_CENTRE + 2;
		case TL_EARC:		return PIC_CENTRE + 3;
		case TL_RECT:		return PIC_CENTRE + 4;
		default:		return PIC_CENTRE + 5;
		}
	}
	return df_row2_pict[b - B_ROW2];
}

LOCAL void panel_build( void )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_DPRECT	c;
	INT		b, t;
	UW		pc;
	CONST UH	*tex = NULL;
	static CONST char *CONST zooms[9] = { "x1/8", "x1/4", "x1/2", "x1", "x2", "x3", "x4", "x5", "x8" };
	static CONST INT zv[9] = { 1, 2, 4, 8, 16, 24, 32, 40, 64 };

	def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( def == NULL ) {
		return;
	}
	knl_memset(def, 0, sizeof(*def));
	def->num = 3;
	def->kind = WM_PNL_BARE;
	def->r.right = PW;
	def->r.bottom = PH;
	for ( b = 0; b < B_N; b++ ) {
		pt = &def->part[def->npart++];
		button_rect(b, &c);
		pt->r = c;
		pt->num = b + 1;
		t = button_tool(b);
		if ( b == B_FILL || b == B_STROKE ) {
			pt->type = WM_PT_SWATCH;
			pt->group = b + 1;
			pt->colour = ( b == B_STROKE ) ? fl.stroke
				   : !fl.fill_on ? 0x00FFFFFFU
				   : ( fl.fill_pat > 0 && tv_pattern(fl.fill_pat, &pc, &tex) ) ? pc : fl.fill;
			continue;
		}
		pt->type = WM_PT_BUTTON | ( ( t >= 0 && t == df_tool ) ? P_EMPHAS : 0 );
		pt->answer = WM_ANS_ACT + b;
		switch ( b ) {
		case B_LSTYLE:	pict_put(pt, PIC_LSTYLE);			break;
		case B_CORNER:	pict_put(pt, PIC_CORNER);			break;
		case B_CANVAS:	pict_put(pt, df_canvas_active() ? PIC_BRUSH : PIC_CANVAS);	break;
		case B_SELECT:	pict_put(pt, PIC_SELECT);			break;
		case B_GRID:	pict_put(pt, PIC_GRID + ( ( fl.grid_mode == GRID_SNAP ) ? 2 : ( fl.grid_mode == GRID_SHOW ) ? 1 : 0 ));	break;
		case B_ZOOM:
			{
				INT	k;

				for ( k = 0; k < 8 && zv[k] != fl.zoom; k++ ) ;
				dt_pn_text(pt->label, WM_LABEL_MAX, zooms[k]);
			}
			break;
		case B_FONT:	dt_pn_text(pt->label, WM_LABEL_MAX, "書式");	break;
		default:	pict_put(pt, template_pict(b));			break;
		}
	}
	/*
	 * where the pointer is, after the first row: the words in the middle
	 * of the row of buttons, the rectangle tall enough for the letters
	 * that hang below the base line (y, g), which it cuts off
	 */
	pt = &def->part[def->npart++];
	button_rect(B_GRID, &c);
	pt->type = WM_PT_LABEL;
	pt->num = N_XY;
	pt->r.left = c.right + 6;
	pt->r.top = c.top + 4;
	pt->r.right = PW - PPAD;
	pt->r.bottom = c.bottom - 4;
	xy_words(pt->label);
	if ( df_pid >= 0 ) {
		(void)wm_panel_close(df_pid);
	}
	df_pid = wm_panel_open(df_panel, def);
	Kfree(def);
}

LOCAL BOOL sig_same( CONST DFSIG *a, CONST DFSIG *b )
{
	return (BOOL)( a->tool == b->tool && a->centre == b->centre && a->canvas == b->canvas
		    && a->fill == b->fill && a->stroke == b->stroke && a->fill_on == b->fill_on
		    && a->fill_pat == b->fill_pat && a->zoom == b->zoom && a->grid_mode == b->grid_mode );
}

LOCAL void panel_draw( void )
{
	DFSIG	s;
	UB	txt[32];

	if ( df_panel <= 0 ) {
		return;
	}
	knl_memset(&s, 0, sizeof(s));
	s.tool = df_tool;
	s.centre = (BOOL)( centre_able(df_tool) && df_centre[df_tool] );
	s.canvas = df_canvas_active();
	s.fill = fl.fill;
	s.stroke = fl.stroke;
	s.fill_on = fl.fill_on;
	s.fill_pat = fl.fill_pat;
	s.zoom = fl.zoom;
	s.grid_mode = fl.grid_mode;
	if ( !df_sig_ok || df_pid < 0 || !sig_same(&s, &df_sig) ) {
		df_sig = s;
		df_sig_ok = TRUE;
		panel_build();
	} else {
		xy_words(txt);
		(void)wm_panel_set_text(df_pid, N_XY, txt);
	}
	if ( df_pid >= 0 ) {
		wm_panel_draw(df_pid);
	}
}

/* Where windows may stand: the screen above the message line */
LOCAL void screen_room( T_DPRECT *room )
{
	INT	sw = 0, sh = 0;

	wm_screen_main(&sw, &sh);
	room->left = 0;
	room->top = 0;
	room->right = sw;
	room->bottom = sh - wm_msg_height();
}

/* A window's rectangle moved, its size kept, until it is whole inside the room */
LOCAL void fit_in( T_DPRECT *o, CONST T_DPRECT *room )
{
	INT	dx = 0, dy = 0;

	if ( o->right > room->right ) dx = room->right - o->right;
	if ( o->left + dx < room->left ) dx = room->left - o->left;
	if ( o->bottom > room->bottom ) dy = room->bottom - o->bottom;
	if ( o->top + dy < room->top ) dy = room->top - o->top;
	o->left += dx;  o->right += dx;
	o->top += dy;  o->bottom += dy;
}

/* ---------------------------------------------------------------- the palettes */

/*
 * A palette: a small window of rows, each a label and a control. What
 * is changed in it is changed at once, and each change is handed to
 * the palette's own function to be put on what is picked. It goes away
 * when the pointer is pressed outside it, or on Enter or Escape. The
 * controls are the parts of a panel the window manager draws and works
 * (choices, a switch, a box of letters, swatches, a push button); only
 * the fixed patterns to choose from are drawn here, as what is chosen
 * among rather than as parts.
 */
#define PI_CHOICE	FT_PI_CHOICE
#define PI_CHECK	FT_PI_CHECK
#define PI_GRID		FT_PI_GRID
#define PI_TEXT		FT_PI_TEXT
#define PI_BUTTON	FT_PI_BUTTON
#define PI_SWATCH	FT_PI_SWATCH
#define PI_COLOUR	FT_PI_COLOUR
#define POP_W		FT_POP_W
#define ROW_H		FT_ROW_H
#define LABEL_W		FT_LABEL_W
#define PART_FH		FT_PART_FH
#define PN_ITEM		FT_PN_ITEM

typedef T_FTITEM	POPITEM;
typedef T_FTPOP		POPUP;

LOCAL INT	pop_wid = 0;
LOCAL INT	pop_pid = -1;
LOCAL DTWIN	*pop_win = NULL;	/* whose picked shapes it changes */

#define choice_at	ft_choice_at
#define item_h		ft_item_h
#define pop_height	ft_pop_height

/* The palette's parts, laid row by row, as its items stand */
LOCAL void pop_build( POPUP *p )
{
	T_WMPANEL	*def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	T_WMPART	*pt;
	INT		i, k, y = 6;

	if ( def == NULL ) {
		return;
	}
	knl_memset(def, 0, sizeof(*def));
	def->num = 4;
	def->kind = WM_PNL_BARE;
	def->r.right = POP_W;
	def->r.bottom = pop_height(p);
	for ( i = 0; i < p->n; i++ ) {
		POPITEM	*it = &p->it[i];
		INT	h = item_h(it);

		y = it->r.top;			/* laid out by ft_pop_layout */
		if ( it->label != NULL && it->type != PI_CHECK && it->type != PI_BUTTON && def->npart < WM_PART_MAX ) {
			pt = &def->part[def->npart++];
			pt->type = WM_PT_LABEL;
			pt->r.left = 8;  pt->r.top = y + 2;  pt->r.right = LABEL_W - 4;  pt->r.bottom = y + ROW_H - 4;
			dt_pn_text(pt->label, WM_LABEL_MAX, it->label);
		}
		switch ( it->type ) {
		case PI_CHOICE:
			for ( k = 0; k < it->n && def->npart < WM_PART_MAX; k++ ) {
				pt = &def->part[def->npart++];
				pt->type = WM_PT_CHOICE;
				pt->num = PN_ITEM(i, k);
				pt->group = i + 1;
				pt->value = ( *it->value == k ) ? 1 : 0;
				(void)choice_at(it, k, &pt->r);
				dt_pn_text(pt->label, WM_LABEL_MAX, it->names[k]);
			}
			break;
		case PI_CHECK:
			pt = &def->part[def->npart++];
			pt->type = WM_PT_CHECK;
			pt->num = PN_ITEM(i, 0);
			pt->value = *it->value ? 1 : 0;
			pt->r.left = 8;  pt->r.top = y + 1;  pt->r.right = POP_W - 8;  pt->r.bottom = y + ROW_H - 3;
			dt_pn_text(pt->label, WM_LABEL_MAX, it->label);
			break;
		case PI_TEXT:
			pt = &def->part[def->npart++];
			pt->type = TB_PARTS | P_ASCII;
			pt->num = PN_ITEM(i, 0);
			pt->max = it->max - 1;
				pt->r.left = LABEL_W;  pt->r.top = y + 1;  pt->r.right = LABEL_W + 110;  pt->r.bottom = y + ROW_H - 3;
			dt_pn_text(pt->text, WM_LABEL_MAX, (CONST char *)it->text);
			pt->caret = (INT)knl_strlen((CONST char *)pt->text);
			break;
		case PI_COLOUR:
			pt = &def->part[def->npart++];
			pt->type = WM_PT_SWATCH;
			pt->num = PN_ITEM(i, 0);
			pt->answer = WM_ANS_ACT + i;
			(void)hex_colour(it->text, &pt->colour);
			pt->r.left = LABEL_W;  pt->r.top = y + 1;  pt->r.right = LABEL_W + 110;  pt->r.bottom = y + ROW_H - 3;
			break;
		case PI_SWATCH:
			for ( k = 0; k < it->n && def->npart < WM_PART_MAX; k++ ) {
				pt = &def->part[def->npart++];
				pt->type = WM_PT_SWATCH;
				pt->num = PN_ITEM(i, k);
				pt->group = i + 1;
				pt->colour = it->cols[k];
				pt->value = ( *it->col == it->cols[k] ) ? 1 : 0;
				pt->r.left = LABEL_W + k * 20;  pt->r.top = y + 3;
				pt->r.right = pt->r.left + 18;  pt->r.bottom = pt->r.top + 18;
			}
			break;
		case PI_BUTTON:
			pt = &def->part[def->npart++];
			pt->type = WM_PT_BUTTON;
			pt->num = PN_ITEM(i, 0);
			pt->answer = WM_ANS_ACT + i;
			pt->r.left = 8;  pt->r.top = y + 1;
			pt->r.right = 8 + str_w(it->label, PART_FH) + 24;  pt->r.bottom = y + ROW_H - 3;
			dt_pn_text(pt->label, WM_LABEL_MAX, it->label);
			break;
		default:
			break;
		}
		y += h + 4;
	}
	pop_pid = wm_panel_open(pop_wid, def);
	Kfree(def);
}

/* The palette's parts, and the patterns of a PI_GRID drawn over the panel's ground (figtool.c) */
LOCAL void pop_draw( POPUP *p )
{
	INT		gid = wm_gid(pop_wid), i;
	T_DPRECT	all;

	if ( pop_pid >= 0 ) {
		wm_panel_draw(pop_pid);
	}
	ft_pop_grid_draw(gid, p);
	for ( i = 0; gid >= 0 && i < p->n; i++ ) {
		if ( p->it[i].type == PI_GRID ) {
			all = p->it[i].r;
			wm_damage(pop_wid, &all);
		}
	}
	wm_update();
}

/* A press on the patterns: the one under it chosen; the item, or -1 */
#define pop_grid_press	ft_pop_grid_press

/* Whether two strings are the same */
LOCAL BOOL same_text( CONST UB *a, CONST UB *b )
{
	INT	k;

	for ( k = 0; a[k] != 0 && a[k] == b[k]; k++ ) {
		;
	}
	return (BOOL)( a[k] == b[k] );
}

/*
 * What the parts hold, taken back into the items, each item that
 * changed handed on; a box's words only when 'texts', and only when
 * they are other than the item's. What the items hold afterwards is
 * shown again in the boxes: a colour chosen from the swatches is
 * written into the box of its number.
 */
LOCAL void pop_follow( POPUP *p, BOOL texts )
{
	INT	i, k, v;
	UW	c;
	UB	buf[WM_LABEL_MAX];

	for ( i = 0; pop_pid >= 0 && i < p->n; i++ ) {
		POPITEM	*it = &p->it[i];
		BOOL	changed = FALSE;

		switch ( it->type ) {
		case PI_CHOICE:
			for ( k = 0; k < it->n; k++ ) {
				if ( wm_panel_get(pop_pid, PN_ITEM(i, k), &v) >= E_OK && v != 0 && *it->value != k ) {
					*it->value = k;
					changed = TRUE;
				}
			}
			break;
		case PI_CHECK:
			if ( wm_panel_get(pop_pid, PN_ITEM(i, 0), &v) >= E_OK && ( v != 0 ) != ( *it->value != 0 ) ) {
				*it->value = ( v != 0 ) ? 1 : 0;
				changed = TRUE;
			}
			break;
		case PI_SWATCH:
			for ( k = 0; k < it->n; k++ ) {
				if ( wm_panel_get(pop_pid, PN_ITEM(i, k), &v) >= E_OK && v != 0 && *it->col != it->cols[k] ) {
					*it->col = it->cols[k];
					changed = TRUE;
				}
			}
			break;
		case PI_TEXT:
			if ( texts && wm_panel_text(pop_pid, PN_ITEM(i, 0), buf, sizeof(buf)) >= E_OK
			  && !same_text(buf, it->text) ) {
				for ( k = 0; buf[k] != 0 && k < it->max - 1; k++ ) it->text[k] = buf[k];
				it->text[k] = 0;
				changed = TRUE;
			}
			break;
		default:
			break;
		}
		if ( changed && p->changed != NULL ) {
			p->changed(i);
		}
	}
	for ( i = 0; pop_pid >= 0 && i < p->n; i++ ) {
		POPITEM	*it = &p->it[i];

		if ( it->type == PI_COLOUR && hex_colour(it->text, &c) ) {
			(void)wm_panel_set_colour(pop_pid, PN_ITEM(i, 0), c);
		}
		if ( it->type == PI_TEXT && wm_panel_text(pop_pid, PN_ITEM(i, 0), buf, sizeof(buf)) >= E_OK
		  && !same_text(buf, it->text) && !texts ) {
			(void)wm_panel_set_text(pop_pid, PN_ITEM(i, 0), it->text);
		}
	}
}

/*
 * Another colour chosen for a PI_COLOUR item: what it changes follows at
 * once, and a row of swatches marks the colour only if it has it.
 */
LOCAL void pop_pick( POPUP *p, INT i )
{
	POPITEM	*it = &p->it[i];
	UW	c = 0;
	INT	j, k;

	(void)hex_colour(it->text, &c);
	if ( dt_pick_colour_part(pop_wid, pop_pid, PN_ITEM(i, 0), &c) ) {
		colour_hex(c, it->text);
		if ( p->changed != NULL ) {
			p->changed(i);
		}
		for ( j = 0; j < p->n; j++ ) {
			if ( p->it[j].type != PI_SWATCH ) continue;
			for ( k = 0; k < p->it[j].n; k++ ) {
				(void)wm_panel_set(pop_pid, PN_ITEM(j, k), ( p->it[j].cols[k] == c ) ? 1 : 0);
			}
		}
		pop_follow(p, FALSE);
	}
	if ( pop_win != NULL && pop_win->used ) {
		dt_draw(pop_win);
	}
	pop_draw(p);
	panel_draw();
	wm_update();
}

/*
 * The palette brought up under a button of the panel, and worked until
 * it is put away. Answers the button of it that was pressed, or -1.
 */
LOCAL INT pop_run( POPUP *p, INT under )
{
	T_WMWIN		pw;
	T_DPRECT	o, c, room;
	T_WMEV		ev, e2;
	UINT		ans;
	INT		answer = -1, i, fw, fh;
	BOOL		done = FALSE;

	if ( df_panel <= 0 || wm_ref(df_panel, &pw) < E_OK ) {
		return -1;
	}
	/* under its button, or over the panel when the screen ends first; the work area the palette's size */
	wm_frame_of(WM_ATTR_FRAME, &fw, &fh);
	button_rect(under, &c);
	screen_room(&room);
	o.left = pw.work.left + c.left;
	o.top = pw.work.top + c.bottom + 4;
	o.right = o.left + POP_W + fw;
	o.bottom = o.top + pop_height(p) + fh;
	if ( o.bottom > room.bottom ) {
		o.top = pw.outer.top - ( pop_height(p) + fh ) - 4;
		o.bottom = pw.outer.top - 4;
	}
	fit_in(&o, &room);
	pop_wid = wm_open(&o, WM_ATTR_FRAME, NULL);
	if ( pop_wid < 0 ) {
		pop_wid = 0;
		return -1;
	}
	wm_raise(pop_wid);
	pop_build(p);
	pop_draw(p);
	while ( !done ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		e2 = ev;
		switch ( ev.type ) {
		case HID_EV_BTN_DOWN:
			if ( ev.wid != pop_wid ) {
				done = TRUE;	/* pressed elsewhere: put away */
				break;
			}
			if ( ( i = pop_grid_press(p, ev.x, ev.y) ) >= 0 ) {
				if ( p->changed != NULL ) p->changed(i);
			} else if ( pop_pid >= 0 ) {
				(void)wm_panel_event(pop_pid, &ev, &ans);
				pop_follow(p, FALSE);
			}
			pop_draw(p);
			panel_draw();
			wm_update();
			break;
		case HID_EV_BTN_UP:
			ans = WM_ANS_NONE;
			if ( pop_pid >= 0 && ev.wid == pop_wid ) {
				(void)wm_panel_event(pop_pid, &ev, &ans);
				pop_follow(p, FALSE);
				pop_draw(p);
				panel_draw();
				wm_update();
			}
			if ( ans >= WM_ANS_ACT && p->it[ans - WM_ANS_ACT].type == PI_COLOUR ) {
				pop_pick(p, (INT)( ans - WM_ANS_ACT ));
			} else if ( ans >= WM_ANS_ACT ) {
				answer = (INT)( ans - WM_ANS_ACT );
				done = TRUE;
			}
			break;
		case HID_EV_KEY_DOWN:
			if ( ev.code == KEY_ENTER || ev.code == KEY_ESC ) {
				done = TRUE;
				break;
			}
			/* the keys are the palette's box's, whichever window has them */
			e2.wid = pop_wid;
			if ( pop_pid >= 0 ) {
				(void)wm_panel_event(pop_pid, &e2, &ans);
				wm_update();
			}
			break;
		case HID_EV_MOVE: {
			INT	px, py;
			UINT	btn;

			if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
				wm_pointer_moved(px, py);
			}
			break;
		}
		default:
			break;
		}
	}
	/* a box being typed in is taken as it stands */
	pop_follow(p, TRUE);
	if ( pop_pid >= 0 ) {
		(void)wm_panel_close(pop_pid);
	}
	pop_pid = -1;
	wm_close(pop_wid);
	pop_wid = 0;
	if ( pop_win != NULL && pop_win->used ) {
		dt_draw(pop_win);
	}
	df_sig_ok = FALSE;		/* the swatch that brought it up let go */
	panel_draw();
	wm_update();

	return answer;
}

/*
 * The palette of button under brought up over the figure in use, what
 * is changed in it put on its picked shapes; the button of it that
 * ended it, or -1.
 */
LOCAL INT palette_run( INT which, INT under )
{
	POPUP		p;
	T_FTVIEW	v;
	T_FGED		e;
	INT		r;

	if ( !ft_palette(which, &p) ) {
		return -1;
	}
	if ( pop_win != NULL && pop_win->used ) {
		view_of(pop_win, &v, &e);
		ft_pal_on(&v);
	}
	r = pop_run(&p, under);
	if ( ft_pal_view() != NULL ) {
		df_ed_back(&e);
	}
	ft_pal_on(NULL);
	return r;
}

/* ---------------------------------------------------------------- the panel's window */

/* The figure the panel works on: the one in front that is a figure */
LOCAL DTWIN *panel_target( void )
{
	DTWIN	*d = dt_win_of(wm_focused());

	return ( d != NULL && df_is(d) ) ? d : NULL;
}

/* The panel beside a figure's window, if it is not up already */
EXPORT void df_panel_show( DTWIN *d )
{
	T_WMWIN		w;
	T_DPRECT	o, room;
	INT		fw, h;
#ifdef USE_UITEST
	UD		t0 = 0, t1 = 0;

	(void)ts_get_mono(&t0);
#endif

	if ( df_panel > 0 || d == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	/* the window's work area the panel's size, whatever its frame takes */
	wm_frame_of(WM_ATTR_FRAME | WM_ATTR_TITLE, &fw, &h);
	h += PH;
	/*
	 * Where it was left for this figure (its panelpos), unless that lies
	 * over the figure window's title bar; else over the figure's window when there
	 * is room for it there, else under it, else wherever on the screen it
	 * is whole: a panel half off the screen or under the message line
	 * cannot be used.
	 */
	screen_room(&room);
	o.left = w.outer.left;
	o.top = w.outer.top - h - 4;
	if ( o.top < room.top && w.outer.bottom + 4 + h <= room.bottom ) {
		o.top = w.outer.bottom + 4;
	}
	{
		INT	kx, ky;

		/* not over the figure window's title bar, by which it is carried and closed */
		if ( om_store_panelpos(&d->id, &kx, &ky) >= E_OK
		  && !( kx < w.outer.right && kx + PW + fw > w.outer.left
		     && ky < w.work.top && ky + h > w.outer.top ) ) {
			o.left = kx;
			o.top = ky;
		}
	}
	o.right = o.left + PW + fw;
	o.bottom = o.top + h;
	fit_in(&o, &room);
	df_panel = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "道具パネル");
	if ( df_panel < 0 ) {
		df_panel = 0;
		return;
	}
	df_pid = -1;
	df_sig_ok = FALSE;
	panel_draw();
	/*
	 * A subordinate window of the figure's: kept just in front of it,
	 * carried with it, and out of sight while a window of something
	 * else is the one in use (design 10.4).
	 */
	(void)wm_set_parent(df_panel, d->wid);
	wm_raise(d->wid);
	wm_focus(d->wid);
#ifdef USE_UITEST
	/* the tests compare it with the panel of the figure editor running as a process */
	wm_update();
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"desktop: tool panel shown in %d us\n", (INT)( ( t1 - t0 ) / 1000 ));
#endif
}

/*
 * The panel kept subordinate to the figure in use: the window with the
 * keys when that is a figure's; while another window has them, the
 * figure it is subordinate to, or, when that has closed, any figure's
 * window. There is one panel for all of them, and it works the figure
 * whose window has the keys (panel_target).
 */
EXPORT void df_panel_poll( void )
{
	DTWIN	*d = dt_win_of(wm_focused()), *p;
	T_WMWIN	w;
	INT	i;

	if ( df_panel <= 0 || wm_ref(df_panel, &w) < E_OK ) {
		return;
	}
	if ( d == NULL || !d->used || !d->figed ) {
		p = ( w.parent > 0 ) ? dt_win_of(w.parent) : NULL;
		if ( p != NULL && p->used && p->figed ) {
			return;
		}
		d = NULL;
		for ( i = 0; i < DT_MAX_WIN && d == NULL; i++ ) {
			p = dt_win_at(i);
			if ( p != NULL && p->used && p->figed && p->wid > 0 ) {
				d = p;
			}
		}
	}
	if ( d != NULL && d->wid != w.parent ) {
		(void)wm_set_parent(df_panel, d->wid);
		wm_update();
	}
}

EXPORT void df_panel_hide( void )
{
	if ( df_pid >= 0 ) {
		(void)wm_panel_close(df_pid);
		df_pid = -1;
	}
	df_pressing = FALSE;
	if ( df_panel > 0 ) {
		wm_close(df_panel);
		df_panel = 0;
		df_panel_moving = FALSE;
	}
}

EXPORT void df_panel_toggle( DTWIN *d )
{
	if ( df_panel > 0 ) {
		df_panel_hide();
	} else {
		df_panel_show(d);
	}
	wm_update();
}

/* A tool taken from the panel's button b, or a palette brought up from it */
LOCAL void panel_do( INT b )
{
	INT	t = button_tool(b);

	if ( t == TL_CANVAS && df_tool == TL_CANVAS && df_canvas_active() ) {
		/* painting: the canvas button is the 画材 */
		pop_win = panel_target();
		(void)palette_run(FT_PAL_PAINT, B_CANVAS);
		return;
	}
	if ( t >= 0 ) {
		if ( t != df_tool ) {
			df_canvas_leave();
		}
		ft_take(t);
		wm_set_pointer(( t == TL_SELECT ) ? WM_PT_SELECT : WM_PT_MODIFY);
		return;
	}
	pop_win = panel_target();
	switch ( b ) {
	case B_FILL:
		if ( palette_run(FT_PAL_FILL, B_FILL) == FT_FILL_EDIT && pop_win != NULL ) {
			df_pattern_edit(pop_win, fl.fill_pat);
		}
		break;
	case B_STROKE:	(void)palette_run(FT_PAL_STROKE, B_STROKE);	break;
	case B_LSTYLE:	(void)palette_run(FT_PAL_LSTYLE, B_LSTYLE);	break;
	case B_CORNER:	(void)palette_run(FT_PAL_CORNER, B_CORNER);	break;
	case B_ZOOM:	(void)palette_run(FT_PAL_ZOOM, B_ZOOM);		break;
	case B_GRID:	(void)palette_run(FT_PAL_GRID, B_GRID);		break;
	case B_FONT:	(void)palette_run(FT_PAL_FONT, B_FONT);		break;
	default:	break;
	}
	if ( pop_win != NULL && pop_win->used ) {
		dt_draw(pop_win);
		wm_focus(pop_win->wid);
	}
}

/*
 * A press, if it is on the panel: TRUE when it was. On its band the
 * panel is taken to be carried; on a swatch the palette of that colour
 * comes up at once; on a button the button is held in, and what it
 * does is done when it is let go over it (df_panel_release).
 */
EXPORT BOOL df_panel_press( CONST T_WMEV *ev )
{
	T_DPRECT	c;
	UINT		ans;
	INT		b;

	if ( df_panel <= 0 || ev->wid != df_panel ) {
		return FALSE;
	}
	if ( ev->y < 0 ) {
		T_WMWIN	w;

		if ( wm_ref(df_panel, &w) >= E_OK ) {
			df_panel_moving = TRUE;
			df_grab_dx = ( w.work.left + ev->x ) - w.outer.left;
			df_grab_dy = ( w.work.top + ev->y ) - w.outer.top;
			wm_raise(df_panel);
		}
		return TRUE;
	}
	for ( b = B_FILL; b <= B_STROKE; b++ ) {
		button_rect(b, &c);
		if ( ev->x >= c.left && ev->x < c.right && ev->y >= c.top && ev->y < c.bottom ) {
			if ( df_pid >= 0 ) {
				(void)wm_panel_set(df_pid, b + 1, 1);	/* the one being worked */
				wm_panel_draw(df_pid);
			}
			panel_do(b);
			panel_draw();
			wm_update();
			return TRUE;
		}
	}
	if ( df_pid >= 0 ) {
		(void)wm_panel_event(df_pid, ev, &ans);
		df_pressing = TRUE;
		wm_update();
	}
	return TRUE;
}

/*
 * The button let go: given to the panel as in its window wherever the
 * pointer is, so that one let go away from it comes up again without
 * answering, and what one let go over does is done.
 */
EXPORT void df_panel_release( CONST T_WMEV *ev )
{
	T_WMEV	e = *ev;
	T_WMWIN	w;
	INT	px, py;
	UINT	btn, ans = WM_ANS_NONE;

	if ( !df_pressing ) {
		return;
	}
	df_pressing = FALSE;
	if ( df_pid < 0 || wm_ref(df_panel, &w) < E_OK ) {
		return;
	}
	if ( ev->wid != df_panel && ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
		e.wid = df_panel;
		e.x = px - w.work.left;
		e.y = py - w.work.top;
	}
	(void)wm_panel_event(df_pid, &e, &ans);
	if ( ans >= WM_ANS_ACT && (INT)( ans - WM_ANS_ACT ) < B_N ) {
		panel_do((INT)( ans - WM_ANS_ACT ));
	}
	panel_draw();
	wm_update();
}

EXPORT BOOL df_panel_carrying( void )
{
	return df_panel_moving;
}

EXPORT void df_panel_follow( INT sx, INT sy )
{
	T_WMWIN		w;
	T_DPRECT	o;

	if ( !df_panel_moving || wm_ref(df_panel, &w) < E_OK ) {
		return;
	}
	o.left = sx - df_grab_dx;
	o.top = sy - df_grab_dy;
	o.right = o.left + ( w.outer.right - w.outer.left );
	o.bottom = o.top + ( w.outer.bottom - w.outer.top );
	if ( wm_move(df_panel, &o) >= E_OK ) {
		wm_update();
	}
}

/*
 * The panel let go of after it was carried: where it stands kept as the
 * panelpos of the figure it works (the window it is subordinate to), for
 * that figure to bring it up there when it is opened again
 */
EXPORT void df_panel_let_go( void )
{
	T_WMWIN	w;
	DTWIN	*d;

	if ( df_panel_moving && wm_ref(df_panel, &w) >= E_OK
	  && ( d = dt_win_of(w.parent) ) != NULL && d->used && d->figed ) {
		(void)om_store_set_panelpos(&d->id, w.outer.left, w.outer.top);
	}
	df_panel_moving = FALSE;
}

EXPORT INT df_panel_wid( void )
{
	return df_panel;
}

/* Whether the tool in hand is the pointer that picks */
EXPORT BOOL df_picking( void )
{
	return (BOOL)( df_tool == TL_SELECT );
}

/* The tool in hand given back to the pointer */
EXPORT void df_tool_select( void )
{
	ft_select();
	panel_draw();
}

/* ---------------------------------------------------------------- 変形 */

/* A picked shape changed point by point (figtool.c) */
EXPORT void df_transform( DTWIN *d )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( d == NULL || d->fig == NULL ) {
		return;
	}
	view_of(d, &v, &e);
	r = ft_transform(&v);
	df_ed_back(&e);
	if ( !r ) {
		dt_tell(d, "この図形は変形できません", NULL);
	}
}

EXPORT BOOL df_transform_press( DTWIN *d, INT x, INT y, UD when )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( d == NULL || ft_transform_arg() != (void *)d ) {
		return FALSE;
	}
	view_of(d, &v, &e);
	r = ft_transform_press(&v, x, y, wm_hit_slop(when));
	df_ed_back(&e);
	return r;
}

EXPORT void df_transform_end( DTWIN *d )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || ft_transform_arg() != (void *)d ) {
		return;
	}
	view_of(d, &v, &e);
	ft_transform_end(&v);
	df_ed_back(&e);
}

EXPORT BOOL df_transform_carrying( void )
{
	return ft_transform_carrying();
}

EXPORT void df_transform_follow( INT sx, INT sy )
{
	DTWIN		*d = (DTWIN *)ft_transform_arg();
	T_WMWIN		w;
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	view_of(d, &v, &e);
	ft_transform_follow(&v, sx - w.work.left, sy - w.work.top);
	df_ed_back(&e);
}

EXPORT void df_transform_release( void )
{
	DTWIN		*d = (DTWIN *)ft_transform_arg();
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used ) {
		return;
	}
	view_of(d, &v, &e);
	ft_transform_release(&v);
	df_ed_back(&e);
}

/* ---------------------------------------------------------------- 用紙枠 */

EXPORT void df_paper_toggle( DTWIN *d )
{
	d->paper_frame = (BOOL)!d->paper_frame;
	dt_draw(d);
	wm_update();
}

/* 用紙枠設定: the paper and its margins, written ahead of the figure */
EXPORT void df_page_setup( DTWIN *d )
{
	T_FTVIEW	v;
	T_FGED		e;
	T_DTPAPER	ps;
	INT		r;

	if ( d == NULL || d->fig == NULL || d->sealed ) {
		return;
	}
	view_of(d, &v, &e);
	ft_paper_of(&v, &ps);
	r = dt_paper_form(d, &ps);
	view_of(d, &v, &e);
	ft_paper_put(&v, &ps, r);
	df_ed_back(&e);
}

/* ---------------------------------------------------------------- 実寸パネル */

/*
 * The figure at its own size, whatever it is magnified by: a small
 * window showing the part of it the figure's window shows at its top
 * left, laid again whenever the figure is.
 */
#define RS_W		260
#define RS_H		180

LOCAL INT	rs_wid = 0;
LOCAL DTWIN	*rs_of = NULL;

LOCAL void rs_draw( DTWIN *d )
{
	INT		gid;
	T_WMWIN		w;
	T_DPRECT	all;
	T_FTVIEW	v;
	T_FGED		e;

	if ( rs_wid <= 0 || d != rs_of || d->fig == NULL ) {
		return;
	}
	gid = wm_gid(rs_wid);
	if ( gid < 0 || wm_ref(rs_wid, &w) < E_OK ) {
		return;
	}
	all.left = 0;  all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	view_of(d, &v, &e);
	ft_realsize_draw(&v, gid, &all, d->paper);
	wm_damage(rs_wid, &all);
}

EXPORT void df_realsize_toggle( DTWIN *d )
{
	T_WMWIN		w;
	T_DPRECT	o;

	if ( rs_wid > 0 ) {
		wm_close(rs_wid);
		rs_wid = 0;
		rs_of = NULL;
		wm_update();
		return;
	}
	if ( d == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	o.right = w.outer.right;
	o.bottom = w.outer.bottom;
	o.left = o.right - RS_W - 8;
	o.top = o.bottom - RS_H - 30;
	rs_wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "実寸");
	if ( rs_wid < 0 ) {
		rs_wid = 0;
		return;
	}
	rs_of = d;
	rs_draw(d);
	(void)wm_set_parent(rs_wid, d->wid);	/* the panel of this figure's window */
	wm_raise(d->wid);
	wm_focus(d->wid);
	wm_update();
}

/* A window gone: a 実寸 panel of it goes too */
EXPORT void df_forget( DTWIN *d )
{
	if ( rs_of == d && rs_wid > 0 ) {
		wm_close(rs_wid);
		rs_wid = 0;
		rs_of = NULL;
	}
	ft_forget(d);
}

/* ---------------------------------------------------------------- a piece of text, edited */

/*
 * Text copied in a text, pasted into the figure: a new piece of text
 * set a step in from the top left of what is shown, holding what was
 * copied as it looked.
 */
LOCAL INT	tx_pasted = 0;

EXPORT void df_paste_text( DTWIN *d )
{
	INT	l, t;

	if ( !df_is(d) || !dd_clip_has() ) {
		return;
	}
	tx_pasted = ( tx_pasted + 1 ) % 8;
	l = d->scroll_x * 8 / fl.zoom + 20 + tx_pasted * 16;
	t = d->scroll_y * 8 / fl.zoom + 20 + tx_pasted * 16;
	make_text(d, l, t, l + 320, t + 120);
	if ( d->tb != NULL ) {
		(void)dd_command(d->tb, DM_PASTE);
		df_text_end(d);
	}
}

/*
 * Letters carried here and let go at (sx, sy), on the screen, the
 * corner of the outline: into the piece of text that stands there, where
 * the corner is, or else into a new piece of text whose top left is
 * there, as wide as the letters in the 書式 palette's look. E_PAR when
 * the figure cannot be changed.
 */
#define TX_DROP_MIN	96		/* the least width of a piece made for letters */
#define TX_DROP_MAX	480

EXPORT ER df_text_drop( DTWIN *d, INT sx, INT sy, CONST UB *s, INT n )
{
	T_DPRECT	page;
	T_WMWIN		ww;
	INT		i, fx, fy, w;
	ER		er;

	if ( !df_is(d) || d->fig == NULL || d->sealed || d->xml_view || s == NULL || n <= 0
	  || wm_ref(d->wid, &ww) < E_OK ) {
		return E_PAR;
	}
	df_text_end(d);
	to_fig(d, sx - ww.work.left, sy - ww.work.top, &fx, &fy);	/* the work area's own places */
	for ( i = d->fig->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*sh = &d->fig->sh[i];

		if ( sh->kind == TV_SH_DOC && sh->node != NULL && fx >= sh->r.left && fx < sh->r.right
		  && fy >= sh->r.top && fy < sh->r.bottom ) {
			break;
		}
	}
	if ( i >= 0 ) {
		/* a piece of text there: the letters go in where the corner is */
		ed_before(d);
		df_text_begin(d, d->fig->sh[i].node, -1, -1);
	} else {
		/* room for the letters as the piece draws them, with a margin */
		w = str_w((CONST char *)s, fl.font_size) * 2 + 32;
		w = ( w < TX_DROP_MIN ) ? TX_DROP_MIN : ( w > TX_DROP_MAX ) ? TX_DROP_MAX : w;
		snap(&fx, &fy);
		make_text(d, fx, fy, fx + w, fy + fl.font_size * 2 + 12);
	}
	if ( d->tb == NULL ) {
		return E_NOMEM;
	}
	dt_work_rect(d->tb, &page);
	if ( i < 0 ) {
		sx = ww.work.left + page.left;
		sy = ww.work.top + page.top;
	}
	er = dd_text_drop(d->tb, sx, sy, s, n);
	df_text_end(d);
	tm_printf((UB *)"figure: text dropped %s (%d)\n", ( i >= 0 ) ? "into a piece" : "as a new piece", (INT)er);
	return er;
}

/*
 * A piece of text in the figure edited where it stands: a press on it
 * twice with the pointer in hand, or a new one drawn with the 文字 tool.
 * Its editor is a text editor of its own whose page is the piece's box;
 * pressing outside it, or Escape, ends it.
 */
LOCAL UD	tx_at = 0;
LOCAL INT	tx_x, tx_y;

EXPORT void df_text_begin( DTWIN *d, T_TADNODE *node, INT x, INT y )
{
	DTWIN	*tb;

	df_text_end(d);
	tb = (DTWIN *)Kmalloc(sizeof(DTWIN));
	if ( tb == NULL ) {
		return;
	}
	knl_memset(tb, 0, sizeof(*tb));
	tb->used = TRUE;
	tb->wid = d->wid;
	tb->rec = d->rec;
	tb->kind = TV_KIND_DOC;
	tb->id = d->id;
	tb->recno = d->recno;
	tb->paper = d->paper;
	tb->tb_host = d;
	tb->tb_node = node;
	d->tb = tb;
	ed_model(tb);
	ed_pick_none(d);
	if ( x >= 0 ) {
		dd_press(tb, x, y, FALSE, 0);
		dd_release();
	}
	dt_draw(d);
	wm_update();
}

EXPORT void df_text_end( DTWIN *d )
{
	DTWIN	*tb;

	if ( d == NULL || d->tb == NULL ) {
		return;
	}
	tb = d->tb;
	d->tb = NULL;
	ed_forget(tb);
	Kfree(tb);
	if ( d->used ) {
		dt_draw(d);
		wm_update();
	}
}

EXPORT BOOL df_text_press( DTWIN *d, INT x, INT y, UD when )
{
	UD	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	BOOL	twice;
	INT	i, fx, fy;

	if ( !df_is(d) ) {
		return FALSE;
	}
	if ( d->tb != NULL ) {
		T_DPRECT	page;

		dt_work_rect(d->tb, &page);
		if ( x >= page.left && x < page.right && y >= page.top && y < page.bottom ) {
			dd_press(d->tb, x, y, FALSE, 0);
			dt_draw(d);
			wm_update();
			return TRUE;
		}
		df_text_end(d);		/* pressed elsewhere: done, and that press goes on */
		return FALSE;
	}
	twice = (BOOL)( x > tx_x - DT_DBL_NEAR && x < tx_x + DT_DBL_NEAR && y > tx_y - DT_DBL_NEAR && y < tx_y + DT_DBL_NEAR
		     && when - tx_at < gap );
	tx_x = x;
	tx_y = y;
	tx_at = when;
	if ( !twice || df_tool != TL_SELECT ) {
		return FALSE;
	}
	to_fig(d, x, y, &fx, &fy);
	for ( i = d->fig->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];

		if ( s->node == NULL || fx < s->r.left || fx >= s->r.right
		  || fy < s->r.top || fy >= s->r.bottom ) {
			continue;
		}
		if ( s->kind == TV_SH_DOC ) {
			df_text_begin(d, s->node, x, y);
			return TRUE;
		}
		if ( s->kind == TV_SH_PIXMAP ) {
			/* a picture painted in the figure: painted on, the canvas in hand */
			ft_select();
			df_tool = TL_CANVAS;
			wm_set_pointer(WM_PT_MODIFY);
			panel_draw();
			return df_canvas_press(d, fx, fy);
		}
		break;
	}
	/* a shape whose points can be moved: taken, and 変形 begun on it */
	{
		T_DPRECT	page;
		INT		which;

		dt_work_rect(d, &page);
		if ( tv_fig_shape_at(d->fig, &page, d->scroll_x, d->scroll_y, x, y,
				     &which) >= E_OK
		  && which >= 0 && which < d->fig->nsh
		  && ft_transformable(&d->fig->sh[which]) && df_takeable(d, which) ) {
			ed_pick_only(d, which);
			df_transform(d);
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * What the 書式 palette sets, put on the piece of text being edited or
 * the pieces picked: all of the letters of each.
 */
LOCAL void text_apply( DTWIN *tb, INT item )
{
	LOCAL CONST char *CONST faces[3] = { "sans-serif", "serif", "monospace" };
	UB	word[16];

	/* the letters picked in it, or else all of them */
	if ( !dd_has_sel(tb) ) {
		dd_pick_all(tb);
	}
	switch ( item ) {
	case 0:
		(void)put_num(word, 0, fl.font_size);
		dd_font(tb, "size", word);
		break;
	case 1:
		dd_font(tb, "face", (CONST UB *)faces[fl.font_face % 3]);
		break;
	case 2:
	case 3:
		colour_hex(fl.text_col, word);
		dd_font(tb, "color", word);
		break;
	case 4:	dd_look(tb, DM_STYLE_BOLD, fl.bold);		break;
	case 5:	dd_look(tb, DM_STYLE_ITALIC, fl.italic);	break;
	case 6:	dd_look(tb, DM_STYLE_UNDERLINE, fl.under);	break;
	default: dd_look(tb, DM_STYLE_STRIKE, fl.strike);	break;
	}
}

LOCAL void text_look( INT item )
{
	DTWIN		*d = pop_win;
	T_TADNODE	*nodes[32];
	INT		i, k, n = 0;

	if ( d == NULL || !df_is(d) || d->sealed ) {
		return;
	}
	if ( d->tb != NULL ) {
		text_apply(d->tb, item);	/* the one being edited is the one meant */
		return;
	}
	for ( i = 0; i < d->fig->nsh && n < 32; i++ ) {
		if ( d->pick[i] != 0 && d->fig->sh[i].kind == TV_SH_DOC
		  && d->fig->sh[i].node != NULL ) {
			nodes[n++] = d->fig->sh[i].node;
		}
	}
	for ( k = 0; k < n; k++ ) {
		df_text_begin(d, nodes[k], -1, -1);
		if ( d->tb != NULL ) {
			text_apply(d->tb, item);
		}
		df_text_end(d);
	}
	/* the pieces picked again, as they were */
	for ( i = 0; n > 0 && i < d->fig->nsh; i++ ) {
		for ( k = 0; k < n; k++ ) {
			if ( d->fig->sh[i].node == nodes[k] && d->pick[i] == 0 ) {
				ed_pick_toggle(d, i);
			}
		}
	}
}

/* ---------------------------------------------------------------- over the figure */

/* What is drawn over a figure: the grid, the paper, the handles of 変形 */
EXPORT void df_overlay( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	if ( !df_is(d) ) {
		return;
	}
	df_grid_draw(d, gid, page);
	df_canvas_draw(d, gid, page);
	{
		T_FTVIEW	v;
		T_FGED		e;

		view_of(d, &v, &e);
		v.gid = gid;
		v.page = *page;
		ft_overlay(&v);
	}
	rs_draw(d);
}

/* The pattern the inside is filled with now, for パターン編集 to start from */
EXPORT INT df_fill_pattern( void )
{
	return ( fl.fill_pat > 0 ) ? fl.fill_pat : 2;
}
