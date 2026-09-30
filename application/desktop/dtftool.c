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
 *	curve, line, sector, chord, arc, rectangle, polygon, triangle,
 *	ellipse, a box of text, and the look of text.
 *
 *	A palette comes up as a small window of its own under its button,
 *	and what is chosen in it takes effect at once: on the shapes that
 *	are picked, and on those drawn afterwards. Pressing anywhere else
 *	puts it away.
 *
 *	A shape is drawn by pressing and dragging out its box, or, for a
 *	polygon, by pressing each corner in turn. What is drawn is written
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
#include "desktop.h"

#define KEY_BS		0x2A
#define KEY_ENTER	0x28
#define KEY_ESC		0x29

#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- the look */

/* What a new shape is drawn with, as the palettes set it */
typedef struct {
	UW	fill;			/* the colour of the inside */
	BOOL	fill_on;		/* filled at all */
	INT	fill_pat;		/* a fixed pattern chosen, or 0 for the colour */
	UW	stroke;			/* the colour of lines */
	INT	width;
	INT	ltype;			/* 0 実線 .. 5 長破線 */
	INT	conn;			/* TV_CONN_*: how a joined line runs */
	INT	corner;			/* the corners' radius */
	INT	arrows;			/* 0 none, 1 start, 2 end, 3 both */
	BOOL	arrow_filled;
	INT	grid_mode;		/* GRID_* */
	INT	grid;			/* its spacing */
	INT	zoom;			/* in eighths: 8 is life size */

	/* 画材: what the canvas paints with, and how wide */
	INT	paint_tool;
	INT	paint_size;

	/* the look of text in a new box of text */
	INT	font_size;
	INT	font_face;		/* 0 ゴシック, 1 明朝, 2 等幅 */
	UW	text_col;
	BOOL	bold, italic, under, strike;
} FLOOK;

#define GRID_NONE	0
#define GRID_SHOW	1
#define GRID_SNAP	2

LOCAL FLOOK	fl = {
	0x00FFFFFFU, TRUE, 0, 0x00000000U, 2, 0, TV_CONN_STRAIGHT, 0, 0, FALSE,
	GRID_NONE, 16, 8, DF_PAINT_PENCIL, 5,
	16, 0, 0x00000000U, FALSE, FALSE, FALSE, FALSE
};

EXPORT INT df_paint_tool( void )	{ return fl.paint_tool; }
EXPORT INT df_paint_size( void )	{ return fl.paint_size; }
EXPORT UW  df_paint_colour( void )	{ return fl.stroke; }

/* ---------------------------------------------------------------- the tools */

#define TL_SELECT	0
#define TL_CANVAS	1
#define TL_CURVE	2
#define TL_LINE		3
#define TL_ARC		4		/* 扇形 */
#define TL_CHORD	5		/* 弦 */
#define TL_EARC		6		/* 楕円弧 */
#define TL_RECT		7
#define TL_POLYGON	8
#define TL_TRIANGLE	9
#define TL_ELLIPSE	10
#define TL_TEXT		11
#define TL_N		12

LOCAL INT	df_tool = TL_SELECT;

/*
 * The tools that can draw from the middle out: pressing the tool that
 * is already in hand turns that on or off for it, and the place first
 * pressed is then the middle of what is drawn.
 */
LOCAL BOOL	df_centre[TL_N];

LOCAL BOOL centre_able( INT t )
{
	return (BOOL)( t == TL_LINE || t == TL_RECT || t == TL_ELLIPSE
		    || t == TL_ARC || t == TL_CHORD || t == TL_EARC );
}

/* A shape being drawn: where the hand went down, and where it is */
LOCAL DTWIN	*df_drawing = NULL;
LOCAL INT	df_x0, df_y0, df_x1, df_y1;	/* in the figure's coordinates */
LOCAL T_DPPOINT	df_pts[TV_MAX_PT];
LOCAL INT	df_npt = 0;
LOCAL UD	df_last_at = 0;
LOCAL BOOL	df_first_edge = FALSE;	/* a polygon's first side, being dragged */
LOCAL INT	df_c0_place = -1, df_c0_point = -1;	/* a line's joined ends */
LOCAL INT	df_c1_place = -1, df_c1_point = -1;

/* The pointer, in the figure's coordinates, for the panel to show */
LOCAL BOOL	df_have_xy = FALSE;
LOCAL INT	df_px, df_py;

/* ---------------------------------------------------------------- numbers and colours */

LOCAL INT put_num( UB *buf, INT at, INT v )
{
	char	t[12];
	INT	k = 0;

	if ( v < 0 ) {
		buf[at++] = '-';
		v = -v;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 ) {
		buf[at++] = (UB)t[--k];
	}
	buf[at] = 0;

	return at;
}

LOCAL void colour_hex( UW col, UB *out )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	out[0] = '#';
	for ( i = 0; i < 6; i++ ) {
		out[1 + i] = (UB)hex[( col >> ( 20 - i * 4 ) ) & 0xF];
	}
	out[7] = 0;
}

LOCAL BOOL hex_colour( CONST UB *s, UW *p_col )
{
	UW	v = 0;
	INT	i;

	while ( *s == ' ' ) {
		s++;
	}
	if ( *s == '#' ) {
		s++;
	}
	for ( i = 0; i < 6; i++ ) {
		UB	c = s[i];

		if ( c >= '0' && c <= '9' )      v = ( v << 4 ) | (UW)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) v = ( v << 4 ) | (UW)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) v = ( v << 4 ) | (UW)( c - 'A' + 10 );
		else return FALSE;
	}
	*p_col = v;

	return TRUE;
}

LOCAL BOOL same_word( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) {
		return FALSE;
	}
	for ( i = 0; b[i] != 0 && a[i] == (UB)b[i]; i++ ) {
		;
	}

	return (BOOL)( b[i] == 0 && a[i] == 0 );
}

/*
 * The pattern number a colour is written as: a fixed one when it is
 * one of them, else a pattern of the record's own -- one already
 * there of that colour alone, or a new one after the last. The record's
 * patterns stand in its <patterns>, ahead of the shapes.
 */
LOCAL INT pattern_for( T_TAD *rec, UW col )
{
	T_TADNODE	*body = tad_body(rec), *pats = NULL, *n;
	UB		hex[8], num[16];
	INT		fixed = ( col == 0x00000000U ) ? 1 : tv_pattern_of(col);
	INT		top = TV_PAT_FIXED - 1;

	if ( fixed > 0 || body == NULL ) {
		return ( fixed > 0 ) ? fixed : 1;
	}
	colour_hex(col, hex);
	for ( n = body->first; n != NULL; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "patterns") ) {
			pats = n;
			break;
		}
	}
	for ( n = ( pats != NULL ) ? pats->first : NULL; n != NULL; n = n->next ) {
		CONST UB	*id, *fg;
		INT		v = 0, k;

		if ( n->kind != TAD_ND_ELEM || !same_word(n->name, "pattern") ) {
			continue;
		}
		id = tad_attr(n, "id");
		for ( k = 0; id != NULL && id[k] >= '0' && id[k] <= '9'; k++ ) {
			v = v * 10 + ( id[k] - '0' );
		}
		if ( v > top ) {
			top = v;
		}
		fg = tad_attr(n, "fgcolors");
		if ( fg != NULL && ( tad_attr(n, "masks") == NULL
				     || tad_attr(n, "masks")[0] == 0 ) ) {
			UW	c;

			if ( hex_colour(fg, &c) && c == col && fg[7] == 0 ) {
				return v;
			}
		}
	}
	if ( pats == NULL ) {
		T_TADNODE	*at = body->first;

		/* after the figure's view, drawing area and scale */
		while ( at != NULL && ( at->kind != TAD_ND_ELEM
			|| same_word(at->name, "figView")
			|| same_word(at->name, "figDraw")
			|| same_word(at->name, "figScale") ) ) {
			at = at->next;
		}
		pats = tad_elem_new(rec, "patterns", body, at, FALSE);
		if ( pats == NULL ) {
			return 1;
		}
	}
	n = tad_elem_new(rec, "pattern", pats, NULL, TRUE);
	if ( n == NULL ) {
		return 1;
	}
	(void)put_num(num, 0, top + 1);
	(void)tad_set_attr(rec, n, "id", num);
	(void)tad_set_attr(rec, n, "type", (CONST UB *)"0");
	(void)tad_set_attr(rec, n, "width", (CONST UB *)"16");
	(void)tad_set_attr(rec, n, "height", (CONST UB *)"16");
	(void)tad_set_attr(rec, n, "ncol", (CONST UB *)"1");
	(void)tad_set_attr(rec, n, "fgcolors", hex);
	(void)tad_set_attr(rec, n, "bgcolor", (CONST UB *)"transparent");
	(void)tad_set_attr(rec, n, "masks", (CONST UB *)"");

	return top + 1;
}

/* What the inside is filled with, as a pattern number: 0 for nothing */
LOCAL INT fill_pattern( T_TAD *rec )
{
	if ( !fl.fill_on ) {
		return 0;
	}
	if ( fl.fill_pat > 0 ) {
		return fl.fill_pat;
	}

	return pattern_for(rec, fl.fill);
}

/* ---------------------------------------------------------------- places */

/* A place in the window's work area, in the figure's own coordinates */
LOCAL void to_fig( DTWIN *d, INT x, INT y, INT *fx, INT *fy )
{
	T_DPRECT	page;

	dt_work_rect(d, &page);
	*fx = ( x - page.left + d->scroll_x ) * 8 / fl.zoom;
	*fy = ( y - page.top + d->scroll_y ) * 8 / fl.zoom;
}

/* On the grid, when the grid holds */
LOCAL void snap( INT *x, INT *y )
{
	INT	g = fl.grid;

	if ( fl.grid_mode != GRID_SNAP || g <= 1 ) {
		return;
	}
	*x = ( ( *x >= 0 ? *x + g / 2 : *x - g / 2 ) / g ) * g;
	*y = ( ( *y >= 0 ? *y + g / 2 : *y - g / 2 ) / g ) * g;
}

/*
 * The point of a shape a line may be joined to that is under a place,
 * if any is within reach: which shape by its place in the figure, and
 * which of its points.
 */
#define JOIN_REACH	8

LOCAL BOOL join_at( DTWIN *d, INT x, INT y, INT *p_place, INT *p_point,
		    INT *p_x, INT *p_y )
{
	INT		i, k, best = JOIN_REACH * JOIN_REACH + 1;
	T_DPPOINT	p, dir;
	BOOL		found = FALSE;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];

		if ( s->kind == TV_SH_LINE || s->place < 0 ) {
			continue;
		}
		for ( k = 0; k < 64 && tv_connector(s, k, &p, &dir); k++ ) {
			INT	dx = p.x - x, dy = p.y - y;

			if ( dx * dx + dy * dy < best ) {
				best = dx * dx + dy * dy;
				*p_place = s->place;
				*p_point = k;
				*p_x = p.x;
				*p_y = p.y;
				found = TRUE;
			}
		}
	}

	return found;
}

/* ---------------------------------------------------------------- making */

LOCAL T_TAD *rec_of( DTWIN *d )
{
	return (T_TAD *)d->rec;
}

LOCAL INT top_z( DTWIN *d )
{
	INT	i, top = 0;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->fig->sh[i].z > top ) {
			top = d->fig->sh[i].z;
		}
	}

	return top;
}

/* The look every new shape is written with */
LOCAL void look_put( DTWIN *d, T_TADNODE *nd, BOOL filled )
{
	T_TAD	*rec = rec_of(d);

	df_attr_put(rec, nd, "mode", 0);
	df_attr_put(rec, nd, "lineType", fl.ltype);
	df_attr_put(rec, nd, "lineWidth", fl.width);
	df_attr_put(rec, nd, "l_pat", pattern_for(rec, fl.stroke));
	df_attr_put(rec, nd, "f_pat", filled ? fill_pattern(rec) : 0);
}

LOCAL void arrows_put( T_TAD *rec, T_TADNODE *nd )
{
	df_attr_put(rec, nd, "start_arrow", ( fl.arrows & 1 ) ? 1 : 0);
	df_attr_put(rec, nd, "end_arrow", ( fl.arrows & 2 ) ? 1 : 0);
	(void)tad_set_attr(rec, nd, "arrow_type",
			   (CONST UB *)( fl.arrow_filled ? "filled" : "simple" ));
}

LOCAL void frame_put( T_TAD *rec, T_TADNODE *nd, INT l, INT t, INT r, INT b )
{
	df_attr_put(rec, nd, "frameLeft", l);
	df_attr_put(rec, nd, "frameTop", t);
	df_attr_put(rec, nd, "frameRight", r);
	df_attr_put(rec, nd, "frameBottom", b);
}

LOCAL CONST char *conn_word( INT c )
{
	return ( c == TV_CONN_ELBOW ) ? "elbow"
	     : ( c == TV_CONN_CURVE ) ? "curve" : "straight";
}

/* "place,point" for a joined end, or nothing */
LOCAL void join_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT place,
		     INT point )
{
	UB	buf[24];
	INT	at;

	buf[0] = 0;
	if ( place >= 0 ) {
		at = put_num(buf, 0, place);
		buf[at++] = ',';
		(void)put_num(buf, at, point);
	}
	(void)tad_set_attr(rec, nd, name, buf);
}

/*
 * A box of text put into the figure where it was drawn, and edited
 * where it stands at once: a <document> with where it stands and how it
 * is drawn, the face, size and colour the 書式 palette gives, and one
 * paragraph with nothing in it yet.
 */
LOCAL void make_text( DTWIN *d, INT l, INT t, INT r, INT b )
{
	LOCAL CONST char *CONST faces[] = { "sans-serif", "serif", "monospace" };
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *doc, *e;
	UB		col[8];

	if ( body == NULL ) {
		return;
	}
	ed_before(d);
	doc = tad_elem_new(rec, "document", body, NULL, FALSE);
	if ( doc == NULL ) {
		return;
	}
	e = tad_elem_new(rec, "docView", doc, NULL, TRUE);
	if ( e != NULL ) {
		df_attr_put(rec, e, "viewleft", l);
		df_attr_put(rec, e, "viewtop", t);
		df_attr_put(rec, e, "viewright", r);
		df_attr_put(rec, e, "viewbottom", b);
	}
	e = tad_elem_new(rec, "docDraw", doc, NULL, TRUE);
	if ( e != NULL ) {
		df_attr_put(rec, e, "drawleft", l);
		df_attr_put(rec, e, "drawtop", t);
		df_attr_put(rec, e, "drawright", r);
		df_attr_put(rec, e, "drawbottom", b);
	}
	e = tad_elem_new(rec, "docScale", doc, NULL, TRUE);
	if ( e != NULL ) {
		df_attr_put(rec, e, "hunit", -72);
		df_attr_put(rec, e, "vunit", -72);
	}
	e = tad_elem_new(rec, "text", doc, NULL, TRUE);
	if ( e != NULL ) {
		df_attr_put(rec, e, "lang", 0);
		df_attr_put(rec, e, "bpat", 0);
		df_attr_put(rec, e, "zIndex", top_z(d) + 1);
	}
	e = tad_elem_new(rec, "font", doc, NULL, TRUE);
	if ( e != NULL ) {
		df_attr_put(rec, e, "size", fl.font_size);
		(void)tad_set_attr(rec, e, "face",
				   (CONST UB *)faces[fl.font_face % 3]);
		colour_hex(fl.text_col, col);
		(void)tad_set_attr(rec, e, "color", col);
		if ( fl.bold ) {
			(void)tad_set_attr(rec, e, "weight", (CONST UB *)"700");
		}
		if ( fl.italic ) {
			(void)tad_set_attr(rec, e, "style", (CONST UB *)"italic");
		}
	}
	(void)tad_elem_new(rec, "p", doc, NULL, FALSE);
	df_adopt(d, doc);
	df_tool_select();
	df_text_begin(d, doc, -1, -1);
}

/*
 * The box a shape is drawn in, from where the hand went down to where
 * it is now: square when Shift is held, and doubled out about the
 * first place when the tool draws from the middle.
 */
LOCAL void drawn_box( INT *l, INT *t, INT *r, INT *b )
{
	INT	x0 = df_x0, y0 = df_y0, x1 = df_x1, y1 = df_y1;

	if ( df_centre[df_tool] && centre_able(df_tool) ) {
		x0 = 2 * df_x0 - df_x1;
		y0 = 2 * df_y0 - df_y1;
	}
	*l = ( x0 < x1 ) ? x0 : x1;
	*r = ( x0 < x1 ) ? x1 : x0;
	*t = ( y0 < y1 ) ? y0 : y1;
	*b = ( y0 < y1 ) ? y1 : y0;
}

/* The two ends a line or an arc is drawn between */
LOCAL void drawn_ends( INT *x0, INT *y0, INT *x1, INT *y1 )
{
	*x0 = df_x0;  *y0 = df_y0;
	*x1 = df_x1;  *y1 = df_y1;
	if ( df_centre[df_tool] && centre_able(df_tool) ) {
		*x0 = 2 * df_x0 - df_x1;
		*y0 = 2 * df_y0 - df_y1;
	}
}

/* A freehand curve that ends where it began is closed, and filled */
#define CLOSE_REACH	8

/* The shape drawn, put into the record */
LOCAL void make_shape( DTWIN *d )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *nd = NULL;
	INT		l, t, r, b, x0, y0, x1, y1;

	if ( body == NULL ) {
		return;
	}
	drawn_box(&l, &t, &r, &b);
	drawn_ends(&x0, &y0, &x1, &y1);
	if ( df_tool != TL_LINE && df_tool != TL_POLYGON && df_tool != TL_CURVE
	  && ( r - l < 2 || b - t < 2 ) ) {
		return;				/* a press with no drag draws nothing */
	}
	if ( df_tool == TL_TEXT ) {
		make_text(d, l, t, r, b);
		return;
	}
	if ( df_tool == TL_CANVAS ) {
		df_canvas_new(d, l, t, r, b);
		return;
	}
	ed_before(d);
	switch ( df_tool ) {
	case TL_LINE: {
		T_DPPOINT	pt[2];
		INT		cp = 0;

		if ( x0 == x1 && y0 == y1 ) {
			return;
		}
		nd = tad_elem_new(rec, "line", body, NULL, TRUE);
		if ( nd == NULL ) {
			break;
		}
		look_put(d, nd, FALSE);
		arrows_put(rec, nd);
		if ( df_c0_place >= 0 ) cp |= 1;
		if ( df_c1_place >= 0 ) cp |= 2;
		df_attr_put(rec, nd, "conn_pat", ( cp == 1 ) ? 1 : ( cp == 2 ) ? 2
						 : ( cp == 3 ) ? 3 : 0);
		join_put(rec, nd, "start_conn", df_c0_place, df_c0_point);
		join_put(rec, nd, "end_conn", df_c1_place, df_c1_point);
		(void)tad_set_attr(rec, nd, "lineConnectionType",
				   (CONST UB *)conn_word(fl.conn));
		pt[0].x = x0;  pt[0].y = y0;
		pt[1].x = x1;  pt[1].y = y1;
		df_points_put(rec, nd, pt, 2);
		break;
	}
	case TL_CURVE:
		if ( df_npt < 2 ) {
			return;
		}
		nd = tad_elem_new(rec, "curve", body, NULL, TRUE);
		if ( nd != NULL ) {
			INT	dx = df_pts[0].x - df_pts[df_npt - 1].x;
			INT	dy = df_pts[0].y - df_pts[df_npt - 1].y;
			BOOL	shut = (BOOL)( df_npt >= 3
				       && dx * dx + dy * dy <= CLOSE_REACH * CLOSE_REACH );

			look_put(d, nd, shut);
			df_attr_put(rec, nd, "type", 0);
			df_attr_put(rec, nd, "closed", shut ? 1 : 0);
			df_attr_put(rec, nd, "start_arrow", 0);
			df_attr_put(rec, nd, "end_arrow", 0);
			df_attr_put(rec, nd, "rotation", 0);
			df_points_put(rec, nd, df_pts, df_npt);
		}
		break;
	case TL_POLYGON:
		if ( df_npt < 2 ) {
			return;
		}
		if ( df_npt == 2 ) {
			/* two corners are a line of one segment */
			nd = tad_elem_new(rec, "polyline", body, NULL, TRUE);
			if ( nd != NULL ) {
				look_put(d, nd, FALSE);
				df_attr_put(rec, nd, "round", 0);
				arrows_put(rec, nd);
				df_attr_put(rec, nd, "rotation", 0);
				df_points_put(rec, nd, df_pts, df_npt);
			}
			break;
		}
		nd = tad_elem_new(rec, "polygon", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(d, nd, TRUE);
			df_attr_put(rec, nd, "cornerRadius", fl.corner);
			df_attr_put(rec, nd, "rotation", 0);
			df_points_put(rec, nd, df_pts, df_npt);
		}
		break;
	case TL_TRIANGLE:
		nd = tad_elem_new(rec, "polygon", body, NULL, TRUE);
		if ( nd != NULL ) {
			T_DPPOINT	pt[3];

			/* the right angle where the hand went down */
			pt[0].x = df_x0;  pt[0].y = df_y0;
			pt[1].x = df_x0;  pt[1].y = df_y1;
			pt[2].x = df_x1;  pt[2].y = df_y1;
			look_put(d, nd, TRUE);
			df_attr_put(rec, nd, "cornerRadius", 0);
			df_attr_put(rec, nd, "rotation", 0);
			df_points_put(rec, nd, pt, 3);
		}
		break;
	case TL_ELLIPSE:
		nd = tad_elem_new(rec, "ellipse", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(d, nd, TRUE);
			df_attr_put(rec, nd, "angle", 0);
			df_attr_put(rec, nd, "rotation", 0);
			frame_put(rec, nd, l, t, r, b);
		}
		break;
	case TL_ARC:
	case TL_CHORD:
	case TL_EARC:
		nd = tad_elem_new(rec, ( df_tool == TL_ARC ) ? "arc"
				       : ( df_tool == TL_CHORD ) ? "chord"
				       : "elliptical_arc", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(d, nd, (BOOL)( df_tool != TL_EARC ));
			df_attr_put(rec, nd, "angle", 0);
			frame_put(rec, nd, l, t, r, b);
			/* its ends are towards where the drag began and ended */
			df_attr_put(rec, nd, "startX", x0);
			df_attr_put(rec, nd, "startY", y0);
			df_attr_put(rec, nd, "endX", x1);
			df_attr_put(rec, nd, "endY", y1);
			arrows_put(rec, nd);
		}
		break;
	default:
		nd = tad_elem_new(rec, "rect", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(d, nd, TRUE);
			df_attr_put(rec, nd, "round", ( fl.corner > 0 ) ? 1 : 0);
			df_attr_put(rec, nd, "cornerRadius", fl.corner);
			df_attr_put(rec, nd, "figRH", fl.corner * 2);
			df_attr_put(rec, nd, "figRV", fl.corner * 2);
			df_attr_put(rec, nd, "angle", 0);
			df_attr_put(rec, nd, "rotation", 0);
			df_attr_put(rec, nd, "left", l);
			df_attr_put(rec, nd, "top", t);
			df_attr_put(rec, nd, "right", r);
			df_attr_put(rec, nd, "bottom", b);
		}
		break;
	}
	if ( nd == NULL ) {
		return;
	}
	df_attr_put(rec, nd, "zIndex", top_z(d) + 1);
	df_adopt(d, nd);
}

/* ---------------------------------------------------------------- showing it being drawn */

LOCAL void fig_to_win( DTWIN *d, INT fx, INT fy, INT *x, INT *y )
{
	T_DPRECT	page;

	dt_work_rect(d, &page);
	*x = page.left + fx * fl.zoom / 8 - d->scroll_x;
	*y = page.top + fy * fl.zoom / 8 - d->scroll_y;
}

LOCAL void xor_line( DTWIN *d, INT gid, INT ax, INT ay, INT bx, INT by )
{
	INT	x0, y0, x1, y1;

	fig_to_win(d, ax, ay, &x0, &y0);
	fig_to_win(d, bx, by, &x1, &y1);
	dp_line(gid, x0, y0, x1, y1, 0x00FFFFFFU);
}

/* The shape being drawn, over the figure as it is, in inverted lines */
LOCAL void preview( DTWIN *d )
{
	INT		gid = wm_gid(d->wid);
	T_DPRECT	box;
	T_DPPAT		pat;
	INT		k, l, t, r, b, x0, y0, x1, y1;

	dt_draw(d);
	if ( gid < 0 ) {
		return;
	}
	drawn_box(&l, &t, &r, &b);
	drawn_ends(&x0, &y0, &x1, &y1);
	fig_to_win(d, l, t, &box.left, &box.top);
	fig_to_win(d, r, b, &box.right, &box.bottom);
	box.right++;
	box.bottom++;
	dp_pat_colour(&pat, 0x00FFFFFFU);
	dp_set_mode(gid, DP_MODE_XOR);
	switch ( df_tool ) {
	case TL_LINE:
		xor_line(d, gid, x0, y0, x1, y1);
		break;
	case TL_CURVE:
		for ( k = 0; k + 1 < df_npt; k++ ) {
			xor_line(d, gid, df_pts[k].x, df_pts[k].y,
				 df_pts[k + 1].x, df_pts[k + 1].y);
		}
		break;
	case TL_POLYGON:
		for ( k = 0; k + 1 < df_npt; k++ ) {
			xor_line(d, gid, df_pts[k].x, df_pts[k].y,
				 df_pts[k + 1].x, df_pts[k + 1].y);
		}
		if ( df_npt > 0 ) {
			xor_line(d, gid, df_pts[df_npt - 1].x, df_pts[df_npt - 1].y,
				 df_x1, df_y1);
		}
		break;
	case TL_TRIANGLE:
		xor_line(d, gid, df_x0, df_y0, df_x0, df_y1);
		xor_line(d, gid, df_x0, df_y1, df_x1, df_y1);
		xor_line(d, gid, df_x1, df_y1, df_x0, df_y0);
		break;
	case TL_ELLIPSE:
	case TL_ARC:
	case TL_CHORD:
	case TL_EARC:
		dp_frame_oval(gid, &box, 1, &pat);
		if ( df_tool != TL_ELLIPSE ) {
			/* and where its ends will be */
			INT	cx = ( l + r ) / 2, cy = ( t + b ) / 2;

			xor_line(d, gid, cx, cy, x0, y0);
			xor_line(d, gid, cx, cy, x1, y1);
		}
		break;
	case TL_RECT:
		if ( fl.corner > 0 ) {
			dp_frame_round(gid, &box, fl.corner * 2 * fl.zoom / 8,
				       fl.corner * 2 * fl.zoom / 8, 1, &pat);
			break;
		}
		dp_frame_rect_pat(gid, &box, &pat, 1);
		break;
	default:
		dp_frame_rect_pat(gid, &box, &pat, 1);
		break;
	}
	dp_set_mode(gid, DP_MODE_COPY);
	{
		T_WMWIN		w;
		T_DPRECT	all;

		if ( wm_ref(d->wid, &w) >= E_OK ) {
			all.left = 0;  all.top = 0;
			all.right = w.work.right - w.work.left;
			all.bottom = w.work.bottom - w.work.top;
			wm_damage(d->wid, &all);
		}
	}
	wm_update();
}

LOCAL void panel_draw( void );

/* The pointer's place shown in the panel */
LOCAL void pointer_at( INT fx, INT fy )
{
	df_have_xy = TRUE;
	df_px = fx;
	df_py = fy;
	panel_draw();
}

/* ---------------------------------------------------------------- the pointer in a figure */

/*
 * A press in a figure with a drawing tool in hand: the start of a
 * shape. A polygon's first side is dragged; after it, each press is a
 * corner, a press near the first corner closes it, and a double press
 * or Enter ends it open, as a line of segments.
 */
EXPORT BOOL df_tool_press( DTWIN *d, INT x, INT y, UD when, UINT mods )
{
	INT	fx, fy, jx, jy;
	UD	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;

	if ( !df_is(d) || df_tool == TL_SELECT ) {
		return FALSE;
	}
	to_fig(d, x, y, &fx, &fy);
	if ( df_tool != TL_CURVE ) {
		snap(&fx, &fy);
	}
	if ( df_tool == TL_CANVAS && df_canvas_press(d, fx, fy) ) {
		return TRUE;
	}
	pointer_at(fx, fy);
	if ( df_tool == TL_POLYGON ) {
		if ( df_drawing == d && df_npt > 0 ) {
			INT	dx = fx - df_pts[0].x, dy = fy - df_pts[0].y;

			if ( when - df_last_at < gap ) {
				/* a double press: the corners as a line of segments */
				if ( df_npt >= 2 ) {
					T_TAD		*rec = rec_of(d);
					T_TADNODE	*nd;

					ed_before(d);
					nd = tad_elem_new(rec, "polyline", tad_body(rec),
							  NULL, TRUE);
					if ( nd != NULL ) {
						look_put(d, nd, FALSE);
						df_attr_put(rec, nd, "round", 0);
						arrows_put(rec, nd);
						df_attr_put(rec, nd, "rotation", 0);
						df_points_put(rec, nd, df_pts, df_npt);
						df_attr_put(rec, nd, "zIndex", top_z(d) + 1);
						df_adopt(d, nd);
					}
				}
				df_drawing = NULL;
				df_npt = 0;
				return TRUE;
			}
			if ( df_npt >= 3 && dx * dx + dy * dy <= CLOSE_REACH * CLOSE_REACH ) {
				make_shape(d);	/* back at the first corner: closed */
				df_drawing = NULL;
				df_npt = 0;
				return TRUE;
			}
			if ( ( mods & MOD_SHIFT ) != 0 ) {
				/* across or up and down from the last corner */
				INT	ax = fx - df_pts[df_npt - 1].x;
				INT	ay = fy - df_pts[df_npt - 1].y;

				if ( ( ax < 0 ? -ax : ax ) >= ( ay < 0 ? -ay : ay ) ) {
					fy = df_pts[df_npt - 1].y;
				} else {
					fx = df_pts[df_npt - 1].x;
				}
			}
			if ( df_npt < TV_MAX_PT
			  && ( df_pts[df_npt - 1].x != fx || df_pts[df_npt - 1].y != fy ) ) {
				df_pts[df_npt].x = fx;
				df_pts[df_npt].y = fy;
				df_npt++;
			}
			df_x1 = fx;
			df_y1 = fy;
			df_last_at = when;
			df_first_edge = FALSE;
			preview(d);
			return TRUE;
		}
		df_drawing = d;
		df_npt = 1;
		df_pts[0].x = fx;
		df_pts[0].y = fy;
		df_x0 = df_x1 = fx;
		df_y0 = df_y1 = fy;
		df_last_at = when;
		df_first_edge = TRUE;
		preview(d);
		return TRUE;
	}
	df_drawing = d;
	df_c0_place = df_c1_place = -1;
	if ( df_tool == TL_LINE
	  && join_at(d, fx, fy, &df_c0_place, &df_c0_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	}
	df_x0 = df_x1 = fx;
	df_y0 = df_y1 = fy;
	df_npt = 0;
	if ( df_tool == TL_CURVE ) {
		df_pts[0].x = fx;
		df_pts[0].y = fy;
		df_npt = 1;
	}
	preview(d);

	return TRUE;
}

EXPORT BOOL df_drawing_now( void )
{
	return (BOOL)( df_drawing != NULL );
}

EXPORT void df_tool_follow( INT sx, INT sy, UINT mods )
{
	DTWIN	*d = df_drawing;
	T_WMWIN	w;
	INT	fx, fy, jx, jy;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	to_fig(d, sx - w.work.left, sy - w.work.top, &fx, &fy);
	if ( df_tool == TL_CURVE ) {
		if ( df_npt < TV_MAX_PT
		  && ( df_pts[df_npt - 1].x != fx || df_pts[df_npt - 1].y != fy ) ) {
			df_pts[df_npt].x = fx;
			df_pts[df_npt].y = fy;
			df_npt++;
		}
		df_x1 = fx;
		df_y1 = fy;
		pointer_at(fx, fy);
		preview(d);
		return;
	}
	df_c1_place = -1;
	if ( df_tool == TL_LINE
	  && join_at(d, fx, fy, &df_c1_place, &df_c1_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	} else {
		snap(&fx, &fy);
	}
	if ( ( mods & MOD_SHIFT ) != 0 ) {
		INT	dx = fx - df_x0, dy = fy - df_y0;
		INT	ax = ( dx < 0 ) ? -dx : dx, ay = ( dy < 0 ) ? -dy : dy;

		if ( df_tool == TL_LINE ) {
			if ( ax >= ay ) fy = df_y0; else fx = df_x0;
		} else if ( df_tool != TL_POLYGON ) {
			/* square, and a circle, the larger way */
			INT	m = ( ax > ay ) ? ax : ay;

			fx = df_x0 + ( ( dx < 0 ) ? -m : m );
			fy = df_y0 + ( ( dy < 0 ) ? -m : m );
		}
	}
	df_x1 = fx;
	df_y1 = fy;
	pointer_at(fx, fy);
	preview(d);
}

EXPORT void df_tool_release( void )
{
	DTWIN	*d = df_drawing;

	if ( d == NULL ) {
		return;
	}
	if ( df_tool == TL_POLYGON ) {
		/* the first side's far end is the second corner */
		if ( df_first_edge && df_npt == 1
		  && ( df_x1 != df_pts[0].x || df_y1 != df_pts[0].y ) ) {
			df_pts[1].x = df_x1;
			df_pts[1].y = df_y1;
			df_npt = 2;
		}
		df_first_edge = FALSE;
		preview(d);
		return;				/* the corners go on */
	}
	df_drawing = NULL;
	make_shape(d);
	dt_draw(d);
	wm_composite();
}

LOCAL BOOL ts_active( DTWIN *d );
LOCAL BOOL transformable( CONST T_TVSHAPE *s );

/*
 * Enter or Escape ends a polygon, closed; Escape abandons any other
 * shape being drawn, and leaves painting and 変形.
 */
EXPORT BOOL df_tool_key( DTWIN *d, UINT code )
{
	if ( d != NULL && code == KEY_ESC && df_drawing != d ) {
		if ( df_canvas_active() ) {
			df_canvas_leave();
			return TRUE;
		}
		if ( ts_active(d) ) {
			df_transform_end(d);
			return TRUE;
		}
		return FALSE;
	}
	if ( df_drawing != d || d == NULL ) {
		return FALSE;
	}
	if ( ( code == KEY_ENTER || code == KEY_ESC ) && df_tool == TL_POLYGON ) {
		make_shape(d);
	}
	if ( code == KEY_ENTER || code == KEY_ESC ) {
		df_drawing = NULL;
		df_npt = 0;
		dt_draw(d);
		wm_composite();
		return TRUE;
	}

	return FALSE;
}

/* The pointer over a figure, with nothing held: where it is, for the panel */
EXPORT void df_hover( DTWIN *d, INT x, INT y )
{
	T_DPRECT	page;
	INT		fx, fy;

	if ( !df_is(d) || df_panel_wid() <= 0 ) {
		return;
	}
	dt_work_rect(d, &page);
	if ( x < page.left || y < page.top || x >= page.right || y >= page.bottom ) {
		return;				/* not over the figure */
	}
	to_fig(d, x, y, &fx, &fy);
	if ( !df_have_xy || fx != df_px || fy != df_py ) {
		pointer_at(fx, fy);
	}
}

/* ---------------------------------------------------------------- a line's ends */

/*
 * With the pointer in hand, a picked line's end is carried by pressing
 * on it: it follows the hand, and joins the point of a shape it is let
 * go near, or comes away from one it was joined to.
 */
LOCAL DTWIN	*df_end_win = NULL;
LOCAL INT	df_end_shape, df_end_which;	/* which line, and which end */
LOCAL T_DPPOINT	df_end_pts[2];
LOCAL INT	df_end_place, df_end_point;

LOCAL void end_preview( DTWIN *d )
{
	INT	gid = wm_gid(d->wid);

	dt_draw(d);
	if ( gid < 0 ) {
		return;
	}
	dp_set_mode(gid, DP_MODE_XOR);
	xor_line(d, gid, df_end_pts[0].x, df_end_pts[0].y,
		 df_end_pts[1].x, df_end_pts[1].y);
	dp_set_mode(gid, DP_MODE_COPY);
	{
		T_WMWIN		w;
		T_DPRECT	all;

		if ( wm_ref(d->wid, &w) >= E_OK ) {
			all.left = 0;  all.top = 0;
			all.right = w.work.right - w.work.left;
			all.bottom = w.work.bottom - w.work.top;
			wm_damage(d->wid, &all);
		}
	}
	wm_update();
}

#define END_REACH	6

EXPORT BOOL df_end_press( DTWIN *d, INT x, INT y )
{
	INT		i, k, fx, fy;
	T_DPPOINT	path[TV_MAX_PT];

	if ( !df_is(d) || df_tool != TL_SELECT || d->sealed ) {
		return FALSE;
	}
	to_fig(d, x, y, &fx, &fy);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];
		INT		n;

		if ( d->pick[i] == 0 || s->kind != TV_SH_LINE || s->npt != 2
		  || s->node == NULL || !same_word(s->node->name, "line") ) {
			continue;
		}
		/* its ends as it is drawn: a joined end is where it joins */
		n = tv_line_path(d->fig, s, path, TV_MAX_PT);
		if ( n < 2 ) {
			path[0] = s->pt[0];
			path[1] = s->pt[1];
			n = 2;
		}
		for ( k = 0; k < 2; k++ ) {
			T_DPPOINT	e = ( k == 0 ) ? path[0] : path[n - 1];
			INT		dx = e.x - fx, dy = e.y - fy;

			if ( dx * dx + dy * dy > END_REACH * END_REACH ) {
				continue;
			}
			df_end_win = d;
			df_end_shape = i;
			df_end_which = k;
			df_end_pts[0] = path[0];
			df_end_pts[1] = path[n - 1];
			df_end_place = -1;
			end_preview(d);
			return TRUE;
		}
	}

	return FALSE;
}

EXPORT BOOL df_end_carrying( void )
{
	return (BOOL)( df_end_win != NULL );
}

EXPORT void df_end_follow( INT sx, INT sy )
{
	DTWIN	*d = df_end_win;
	T_WMWIN	w;
	INT	fx, fy, jx, jy;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	to_fig(d, sx - w.work.left, sy - w.work.top, &fx, &fy);
	df_end_place = -1;
	if ( join_at(d, fx, fy, &df_end_place, &df_end_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	} else {
		snap(&fx, &fy);
	}
	df_end_pts[df_end_which].x = fx;
	df_end_pts[df_end_which].y = fy;
	pointer_at(fx, fy);
	end_preview(d);
}

EXPORT void df_end_release( void )
{
	DTWIN		*d = df_end_win;
	T_TAD		*rec;
	T_TADNODE	*nd;
	CONST UB	*v;
	INT		cp;

	df_end_win = NULL;
	if ( d == NULL || !d->used || d->fig == NULL
	  || df_end_shape >= d->fig->nsh ) {
		return;
	}
	rec = rec_of(d);
	nd = d->fig->sh[df_end_shape].node;
	ed_before(d);
	df_points_put(rec, nd, df_end_pts, 2);
	join_put(rec, nd, ( df_end_which == 0 ) ? "start_conn" : "end_conn",
		 df_end_place, df_end_point);
	/* which ends are joined now */
	cp = 0;
	v = tad_attr(nd, "start_conn");
	if ( v != NULL && v[0] != 0 ) cp |= 1;
	v = tad_attr(nd, "end_conn");
	if ( v != NULL && v[0] != 0 ) cp |= 2;
	df_attr_put(rec, nd, "conn_pat", cp);
	ed_changed(d);
}

/* ---------------------------------------------------------------- the grid */

/* The grid's points over the figure, when it is shown */
EXPORT void df_grid_draw( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	INT	g = fl.grid, fx, fy, x, y;
	INT	fw, fh;

	if ( !df_is(d) || fl.grid_mode == GRID_NONE || g < 2 ) {
		return;
	}
	fw = ( page->right - page->left + d->scroll_x ) * 8 / fl.zoom;
	fh = ( page->bottom - page->top + d->scroll_y ) * 8 / fl.zoom;
	for ( fy = ( d->scroll_y * 8 / fl.zoom / g ) * g; fy <= fh; fy += g ) {
		for ( fx = ( d->scroll_x * 8 / fl.zoom / g ) * g; fx <= fw; fx += g ) {
			fig_to_win(d, fx, fy, &x, &y);
			if ( x >= page->left && y >= page->top ) {
				dp_put_pixel(gid, x, y, 0x00808080U);
			}
		}
	}
}

/* What a figure is magnified by, in eighths */
EXPORT INT df_zoom( void )
{
	return fl.zoom;
}

/* ---------------------------------------------------------------- the picked shapes */

/* A leaf shape of the picked ones, one element each */
LOCAL INT picked_nodes( DTWIN *d, T_TADNODE **out, UINT *kinds, INT max )
{
	INT	i, n = 0;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh && n < max; i++ ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];

		if ( d->pick[i] == 0 || s->node == NULL || s->kind == TV_SH_LINK
		  || s->kind == TV_SH_DOC || s->kind == TV_SH_GROUP ) {
			continue;
		}
		out[n] = s->node;
		kinds[n] = s->kind;
		n++;
	}

	return n;
}

#define APPLY_FILL	1
#define APPLY_STROKE	2
#define APPLY_WIDTH	3
#define APPLY_LTYPE	4
#define APPLY_CONN	5
#define APPLY_CORNER	6
#define APPLY_ARROWS	7

/*
 * What a palette sets, put on the shapes that are picked as well: each
 * gets it where it has such a thing -- a line has no inside, and only
 * a rectangle or a polygon has corners.
 */
LOCAL void apply_picked( DTWIN *d, INT what )
{
	T_TADNODE	*nodes[128];
	UINT		kinds[128];
	T_TAD		*rec;
	INT		n, i;

	if ( d == NULL || !df_is(d) || d->sealed ) {
		return;
	}
	n = picked_nodes(d, nodes, kinds, 128);
	if ( n == 0 ) {
		return;
	}
	rec = rec_of(d);
	ed_before(d);
	for ( i = 0; i < n; i++ ) {
		T_TADNODE	*nd = nodes[i];
		UINT		k = kinds[i];
		BOOL		has_inside = (BOOL)( k == TV_SH_RECT || k == TV_SH_ELLIPSE
					|| k == TV_SH_POLY || k == TV_SH_ARC
					|| k == TV_SH_CHORD || k == TV_SH_CURVE );

		switch ( what ) {
		case APPLY_FILL:
			if ( has_inside ) {
				df_attr_put(rec, nd, "f_pat", fill_pattern(rec));
			}
			break;
		case APPLY_STROKE:
			df_attr_put(rec, nd, "l_pat", pattern_for(rec, fl.stroke));
			break;
		case APPLY_WIDTH:
			df_attr_put(rec, nd, "lineWidth", fl.width);
			break;
		case APPLY_LTYPE:
			df_attr_put(rec, nd, "lineType", fl.ltype);
			break;
		case APPLY_CONN:
			if ( same_word(nd->name, "line") ) {
				(void)tad_set_attr(rec, nd, "lineConnectionType",
						   (CONST UB *)conn_word(fl.conn));
			}
			break;
		case APPLY_CORNER:
			if ( same_word(nd->name, "rect") ) {
				df_attr_put(rec, nd, "round", ( fl.corner > 0 ) ? 1 : 0);
				df_attr_put(rec, nd, "cornerRadius", fl.corner);
				df_attr_put(rec, nd, "figRH", fl.corner * 2);
				df_attr_put(rec, nd, "figRV", fl.corner * 2);
			} else if ( same_word(nd->name, "polygon") ) {
				df_attr_put(rec, nd, "cornerRadius", fl.corner);
			}
			break;
		case APPLY_ARROWS:
			if ( k == TV_SH_LINE || k == TV_SH_EARC ) {
				arrows_put(rec, nd);
			}
			break;
		default:
			break;
		}
	}
	ed_changed(d);
}

/*
 * The palettes follow the shape picked: what it is drawn with is what
 * the next shape will be, until something else is chosen.
 */
EXPORT void df_take_look( DTWIN *d )
{
	INT	i;

	for ( i = 0; d != NULL && d->fig != NULL && i < d->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];

		if ( d->pick[i] == 0 || s->kind == TV_SH_LINK || s->kind == TV_SH_DOC
		  || s->kind == TV_SH_GROUP || s->kind == TV_SH_IMAGE ) {
			continue;
		}
		fl.stroke = s->line_col;
		fl.width = ( s->line_w > 0 ) ? s->line_w : fl.width;
		fl.ltype = (INT)s->line_type;
		if ( s->fill_col != TAD_COL_NONE ) {
			fl.fill = s->fill_col;
			fl.fill_on = TRUE;
		}
		fl.corner = ( s->rad_h > 0 ) ? s->rad_h : 0;
		fl.arrows = ( ( s->arrow & 2 ) ? 1 : 0 ) | ( ( s->arrow & 1 ) ? 2 : 0 );
		fl.arrow_filled = (BOOL)( s->arrow_type == 1 );
		if ( s->kind == TV_SH_LINE ) {
			fl.conn = (INT)s->conn;
		}
		panel_draw();
		return;
	}
}

/* ---------------------------------------------------------------- the panel */

/*
 * The buttons: the first row the palettes, the canvas, the pointer, the
 * magnification and the grid, and after them where the pointer is; the
 * second the templates and the look of text.
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
#define B_FONT		( B_ROW2 + 10 )
#define B_N		( B_ROW2 + 11 )

/* The tool each template button gives */
LOCAL CONST INT	df_row2[10] = {
	TL_CURVE, TL_LINE, TL_ARC, TL_CHORD, TL_EARC, TL_RECT, TL_POLYGON,
	TL_TRIANGLE, TL_ELLIPSE, TL_TEXT
};

#define PB		32		/* a button */
#define PGAP		2
#define PPAD		6
#define PW		( PPAD * 2 + 11 * ( PB + PGAP ) )
#define PH		( PPAD * 2 + 2 * PB + PGAP )

LOCAL INT	df_panel = 0;		/* its window */
LOCAL BOOL	df_panel_moving = FALSE;
LOCAL INT	df_grab_dx, df_grab_dy;

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

/* A button's raised edge, or its sunk one when it is in */
LOCAL void bevel( INT gid, CONST T_DPRECT *c, BOOL in )
{
	UW	lit = in ? wm_look(LK_SHADOW) : 0x00FFFFFFU;
	UW	dark = in ? 0x00FFFFFFU : wm_look(LK_SHADOW);

	dp_fill_rect(gid, c, in ? 0x00B0B0B0U : wm_look(LK_PNLGROUND));
	dp_line(gid, c->left, c->top, c->right - 1, c->top, lit);
	dp_line(gid, c->left, c->top, c->left, c->bottom - 1, lit);
	dp_line(gid, c->left, c->bottom - 1, c->right - 1, c->bottom - 1, dark);
	dp_line(gid, c->right - 1, c->top, c->right - 1, c->bottom - 1, dark);
}

LOCAL void fill_pat_rect( INT gid, CONST T_DPRECT *r, INT pat_no, UW col )
{
	UW		pc;
	CONST UH	*tex = NULL;
	T_DPPAT		pat;

	if ( pat_no > 0 && tv_pattern(pat_no, &pc, &tex) && tex != NULL ) {
		dp_fill_rect(gid, r, 0x00FFFFFFU);
		tv_tex_pat(&pat, pc, tex);
		dp_fill_rect_pat(gid, r, &pat);
		return;
	}
	if ( pat_no > 0 && tv_pattern(pat_no, &pc, &tex) ) {
		col = pc;
	}
	dp_fill_rect(gid, r, col);
}

LOCAL void text_at( INT gid, INT x, INT y, INT size, CONST char *s, UW col )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, size);
		fn_draw(gid, fid, x, y, (CONST UB *)s, col);
	}
}

/* The picture on a button */
LOCAL void button_icon( INT gid, INT b, CONST T_DPRECT *c )
{
	T_DPRECT	i, s;
	T_DPPAT		pat;
	INT		l = c->left + 6, t = c->top + 6;
	INT		r = c->right - 7, bt = c->bottom - 7;
	INT		mx = ( l + r ) / 2, my = ( t + bt ) / 2;

	dp_pat_colour(&pat, 0x00000000U);
	i.left = c->left + 4;  i.top = c->top + 4;
	i.right = c->right - 4;  i.bottom = c->bottom - 4;
	switch ( b ) {
	case B_FILL:
		/* the inside's colour or pattern, in a box */
		if ( fl.fill_on ) {
			fill_pat_rect(gid, &i, fl.fill_pat, fl.fill);
		} else {
			dp_fill_rect(gid, &i, 0x00FFFFFFU);
			dp_line(gid, i.left, i.bottom - 1, i.right - 1, i.top, 0x00EE0000U);
		}
		dp_frame_rect(gid, &i, 0x00000000U, 1);
		break;
	case B_STROKE:
		dp_fill_rect(gid, &i, 0x00FFFFFFU);
		dp_frame_rect(gid, &i, 0x00000000U, 1);
		s.left = i.left + 3;  s.right = i.right - 3;
		s.top = my - 1;  s.bottom = my + 2;
		dp_fill_rect(gid, &s, fl.stroke);
		break;
	case B_LSTYLE:
		dp_fill_rect(gid, &i, 0x00FFFFFFU);
		dp_frame_rect(gid, &i, 0x00000000U, 1);
		dp_line_wide(gid, i.left + 3, i.top + 6, i.right - 4, i.top + 6, 1, 0, &pat);
		dp_line_wide(gid, i.left + 3, my, i.right - 4, my, 2, 0, &pat);
		for ( s.left = i.left + 3; s.left < i.right - 4; s.left += 5 ) {
			dp_line(gid, s.left, i.bottom - 6, s.left + 2, i.bottom - 6, 0);
		}
		break;
	case B_CORNER:
		dp_line(gid, l, bt, l, t + 8, 0);
		dp_frame_round(gid, &i, 16, 16, 1, &pat);
		break;
	case B_CANVAS:
		if ( df_canvas_active() ) {
			/* 画材: a brush over a palette */
			s.left = i.left + 1;  s.top = i.top + 8;
			s.right = i.right - 1;  s.bottom = i.bottom - 1;
			dp_frame_oval(gid, &s, 1, &pat);
			s.left = i.left + 5;  s.top = i.top + 12;
			s.right = s.left + 4;  s.bottom = s.top + 4;
			dp_fill_rect(gid, &s, 0x00EE0000U);
			s.left += 6;
			dp_fill_rect(gid, &s, 0x000000FFU);
			s.left += 6;
			dp_fill_rect(gid, &s, 0x0000A000U);
			dp_line_wide(gid, i.right - 3, i.top + 1, mx, my + 2, 2, 0, &pat);
			break;
		}
		dp_frame_rect(gid, &i, 0x00000000U, 1);
		dp_line(gid, i.left + 2, i.bottom - 4, mx, my - 2, 0x000000FFU);
		dp_line(gid, mx, my - 2, i.right - 3, i.bottom - 4, 0x0000A000U);
		s.left = i.right - 9;  s.top = i.top + 3;
		s.right = i.right - 4;  s.bottom = i.top + 8;
		dp_fill_oval(gid, &s, &pat);
		break;
	case B_SELECT: {
		T_DPPOINT	pt[7];

		pt[0].x = l + 4;   pt[0].y = t;
		pt[1].x = l + 4;   pt[1].y = bt - 2;
		pt[2].x = l + 8;   pt[2].y = bt - 6;
		pt[3].x = l + 12;  pt[3].y = bt + 1;
		pt[4].x = l + 14;  pt[4].y = bt;
		pt[5].x = l + 11;  pt[5].y = bt - 7;
		pt[6].x = l + 16;  pt[6].y = bt - 7;
		dp_fill_poly(gid, pt, 7, DP_POLY_ODD, &pat);
		break;
	}
	case B_ZOOM: {
		CONST char	*z = ( fl.zoom == 1 ) ? "x1/8" : ( fl.zoom == 2 ) ? "x1/4"
				   : ( fl.zoom == 4 ) ? "x1/2" : ( fl.zoom == 8 ) ? "x1"
				   : ( fl.zoom == 16 ) ? "x2" : ( fl.zoom == 24 ) ? "x3"
				   : ( fl.zoom == 32 ) ? "x4" : ( fl.zoom == 40 ) ? "x5" : "x8";

		text_at(gid, c->left + 3, my + 5, 11, z, 0);
		break;
	}
	case B_GRID:
		for ( s.top = i.top + 2; s.top < i.bottom; s.top += 5 ) {
			for ( s.left = i.left + 2; s.left < i.right; s.left += 5 ) {
				dp_put_pixel(gid, s.left, s.top,
					     ( fl.grid_mode == GRID_NONE ) ? 0x00808080U : 0);
			}
		}
		if ( fl.grid_mode == GRID_SNAP ) {
			dp_frame_rect(gid, &i, 0x00000000U, 1);
		}
		break;
	case B_ROW2 + 0: {			/* 自由曲線 */
		T_DPPOINT	p[4], out[64];
		INT		n;

		p[0].x = l;  p[0].y = bt;
		p[1].x = l + 5;  p[1].y = t;
		p[2].x = r - 5;  p[2].y = bt;
		p[3].x = r;  p[3].y = t;
		n = tv_curve_points(out, 64, p, 4);
		tv_stroke(gid, out, n, FALSE, 1, 0, &pat);
		break;
	}
	case B_ROW2 + 1:			/* 直線 */
		dp_line_wide(gid, l, bt, r, t, 1, 0, &pat);
		break;
	case B_ROW2 + 2:			/* 扇形 */
	case B_ROW2 + 3:			/* 弦 */
	case B_ROW2 + 4: {			/* 楕円弧 */
		T_DPPOINT	out[80];
		INT		n;

		if ( b == B_ROW2 + 2 ) {
			/* a quarter, and the two radii */
			n = tv_arc_points(out, 76, (D)( mx - 5 ) * 16,
					  (D)( my + 5 ) * 16, 208, 208, 3072, 4096, 0);
			out[n].x = mx - 5;  out[n].y = my + 5;
			n++;
		} else {
			n = tv_arc_points(out, 76, (D)mx * 16, (D)( my + 5 ) * 16,
					  160, 160, 2048, 4096, 0);
		}
		tv_stroke(gid, out, n, (BOOL)( b != B_ROW2 + 4 ), 1, 0, &pat);
		break;
	}
	case B_ROW2 + 5:			/* 長方形 */
		s.left = l;  s.top = t + 3;  s.right = r + 1;  s.bottom = bt - 2;
		dp_frame_rect_pat(gid, &s, &pat, 1);
		break;
	case B_ROW2 + 6: {			/* 多角形 */
		T_DPPOINT	p[8];

		p[0].x = l + 2;  p[0].y = t + 2;   p[1].x = l + 10; p[1].y = t;
		p[2].x = r;      p[2].y = t + 5;   p[3].x = r - 2;  p[3].y = my + 2;
		p[4].x = r;      p[4].y = bt;      p[5].x = mx;     p[5].y = bt - 2;
		p[6].x = l + 3;  p[6].y = bt;      p[7].x = l;      p[7].y = my;
		tv_stroke(gid, p, 8, TRUE, 1, 0, &pat);
		break;
	}
	case B_ROW2 + 7: {			/* 三角形 */
		T_DPPOINT	p[3];

		p[0].x = l + 2;  p[0].y = t;
		p[1].x = l + 2;  p[1].y = bt;
		p[2].x = r;      p[2].y = bt;
		tv_stroke(gid, p, 3, TRUE, 1, 0, &pat);
		break;
	}
	case B_ROW2 + 8:			/* 楕円 */
		s.left = l;  s.top = t + 3;  s.right = r + 1;  s.bottom = bt - 2;
		dp_frame_oval(gid, &s, 1, &pat);
		break;
	case B_ROW2 + 9:			/* 文字 */
		text_at(gid, l + 1, bt - 1, 17, "Ａ", 0);
		break;
	case B_FONT:				/* 書式 */
		text_at(gid, l - 1, my + 1, 10, "書式", 0);
		dp_line(gid, l, my + 4, r, my + 4, 0);
		dp_line(gid, l, my + 7, r - 5, my + 7, 0);
		break;
	default:
		break;
	}
}

LOCAL void panel_draw( void )
{
	INT		gid = wm_gid(df_panel), b;
	T_WMWIN		w;
	T_DPRECT	all, c;

	if ( df_panel <= 0 || gid < 0 || wm_ref(df_panel, &w) < E_OK ) {
		return;
	}
	all.left = 0;  all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	dp_fill_rect(gid, &all, wm_look(LK_PNLGROUND));

	for ( b = 0; b < B_N; b++ ) {
		INT	t = button_tool(b);
		BOOL	in = (BOOL)( t >= 0 && t == df_tool );

		button_rect(b, &c);
		bevel(gid, &c, in);
		button_icon(gid, b, &c);
		if ( in && centre_able(t) && df_centre[t] ) {
			/* drawing from the middle: a dot in the middle */
			T_DPRECT	m;

			m.left = ( c.left + c.right ) / 2 - 2;
			m.top = c.bottom - 6;
			m.right = m.left + 4;
			m.bottom = m.top + 4;
			dp_fill_rect(gid, &m, 0x00EE0000U);
		}
	}
	/* where the pointer is, after the first row */
	if ( df_have_xy ) {
		char	txt[32];
		INT	k = 0;
		UB	num[16];
		INT	j;

		txt[k++] = 'x';  txt[k++] = ':';
		(void)put_num(num, 0, df_px);
		for ( j = 0; num[j] != 0 && k < 14; j++ ) txt[k++] = (char)num[j];
		txt[k++] = ' ';  txt[k++] = 'y';  txt[k++] = ':';
		(void)put_num(num, 0, df_py);
		for ( j = 0; num[j] != 0 && k < 30; j++ ) txt[k++] = (char)num[j];
		txt[k] = 0;
		button_rect(B_GRID, &c);
		text_at(gid, c.right + 6, c.top + 20, 11, txt, 0);
	}
	wm_damage(df_panel, &all);
}

/* ---------------------------------------------------------------- the palettes */

/*
 * A palette: a small window of rows, each a label and a control. What
 * is changed in it is changed at once, and each change is handed to
 * the palette's own function to be put on what is picked. It goes away
 * when the pointer is pressed outside it, or on Enter or Escape.
 */
#define PI_CHOICE	1		/* one of a row of words */
#define PI_CHECK	2		/* on or off */
#define PI_GRID		3		/* the fixed patterns, sixteen across */
#define PI_TEXT		4		/* words typed: a number or a colour */
#define PI_BUTTON	5		/* ends the palette with its number */
#define PI_SWATCH	6		/* a row of colours to pick from */

#define PI_MAX		8
#define POP_W		360
#define ROW_H		24
#define LABEL_W		84

typedef struct {
	UINT		type;
	CONST char	*label;
	CONST char *CONST *names;	/* PI_CHOICE */
	INT		n;
	INT		*value;		/* PI_CHOICE, PI_GRID, PI_CHECK (0/1) */
	UB		*text;		/* PI_TEXT */
	INT		max;
	CONST UW	*cols;		/* PI_SWATCH */
	UW		*col;
	INT		apply;		/* APPLY_* when it changes */
	T_DPRECT	r;		/* where it was laid */
} POPITEM;

typedef struct {
	POPITEM		it[PI_MAX];
	INT		n;
	INT		focus;		/* the PI_TEXT that takes letters, or -1 */
	void		(*changed)( INT item );
} POPUP;

LOCAL INT	pop_wid = 0;
LOCAL DTWIN	*pop_win = NULL;	/* whose picked shapes it changes */

LOCAL INT str_w( CONST char *s, INT size )
{
	ID	fid = fn_system();

	if ( fid <= 0 ) {
		return 0;
	}
	fn_set_size(fid, size);

	return fn_width(fid, (CONST UB *)s);
}

LOCAL INT item_h( CONST POPITEM *it )
{
	if ( it->type == PI_GRID ) {
		return 8 * 17 + 4;
	}
	if ( it->type == PI_CHOICE ) {
		/* the words laid across, as many rows as they take */
		INT	x = LABEL_W, rows = 1, k;

		for ( k = 0; k < it->n; k++ ) {
			INT	w = str_w(it->names[k], 12) + 12;

			if ( x + w > POP_W - 8 && x > LABEL_W ) {
				rows++;
				x = LABEL_W;
			}
			x += w + 2;
		}
		return rows * ROW_H;
	}

	return ROW_H;
}

LOCAL void pop_draw( POPUP *p )
{
	INT		gid = wm_gid(pop_wid), i, k, y = 6;
	T_WMWIN		w;
	T_DPRECT	all, c;

	if ( gid < 0 || wm_ref(pop_wid, &w) < E_OK ) {
		return;
	}
	all.left = 0;  all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	dp_fill_rect(gid, &all, wm_look(LK_PNLGROUND));
	for ( i = 0; i < p->n; i++ ) {
		POPITEM	*it = &p->it[i];
		INT	h = item_h(it);

		it->r.left = 6;  it->r.top = y;
		it->r.right = POP_W - 6;  it->r.bottom = y + h;
		if ( it->label != NULL && it->type != PI_CHECK && it->type != PI_BUTTON ) {
			text_at(gid, 8, y + 17, 12, it->label, 0);
		}
		switch ( it->type ) {
		case PI_CHOICE: {
			INT	x = LABEL_W, row = 0;

			for ( k = 0; k < it->n; k++ ) {
				INT	wd = str_w(it->names[k], 12) + 12;

				if ( x + wd > POP_W - 8 && x > LABEL_W ) {
					row++;
					x = LABEL_W;
				}
				c.left = x;  c.top = y + row * ROW_H + 1;
				c.right = x + wd;  c.bottom = c.top + ROW_H - 3;
				bevel(gid, &c, (BOOL)( *it->value == k ));
				text_at(gid, x + 6, c.top + 15, 12, it->names[k], 0);
				x += wd + 2;
			}
			break;
		}
		case PI_CHECK:
			c.left = 8;  c.top = y + 5;  c.right = 22;  c.bottom = y + 19;
			dp_fill_rect(gid, &c, 0x00FFFFFFU);
			dp_frame_rect(gid, &c, 0x00000000U, 1);
			if ( *it->value ) {
				dp_line(gid, c.left + 3, c.top + 7, c.left + 6, c.top + 10, 0);
				dp_line(gid, c.left + 6, c.top + 10, c.left + 11, c.top + 3, 0);
				dp_line(gid, c.left + 3, c.top + 8, c.left + 6, c.top + 11, 0);
				dp_line(gid, c.left + 6, c.top + 11, c.left + 11, c.top + 4, 0);
			}
			text_at(gid, 28, y + 17, 12, it->label, 0);
			break;
		case PI_GRID:
			for ( k = 0; k < TV_PAT_FIXED; k++ ) {
				c.left = 8 + ( k % 16 ) * 17;
				c.top = y + 2 + ( k / 16 ) * 17;
				c.right = c.left + 16;
				c.bottom = c.top + 16;
				if ( k == 0 ) {
					dp_fill_rect(gid, &c, fl.fill);
				} else {
					fill_pat_rect(gid, &c, k, 0);
				}
				dp_frame_rect(gid, &c, 0x00999999U, 1);
				if ( *it->value == k ) {
					T_DPRECT	o = c;

					o.left--;  o.top--;  o.right++;  o.bottom++;
					dp_frame_rect(gid, &o, 0x000078D7U, 2);
				}
			}
			break;
		case PI_TEXT:
			c.left = LABEL_W;  c.top = y + 2;
			c.right = LABEL_W + 110;  c.bottom = y + ROW_H - 2;
			dp_fill_rect(gid, &c, 0x00FFFFFFU);
			dp_frame_rect(gid, &c, ( p->focus == i ) ? 0x000078D7U : 0x00000000U, 1);
			text_at(gid, c.left + 4, c.top + 15, 12, (CONST char *)it->text, 0);
			if ( p->focus == i ) {
				INT	cx = c.left + 5 + str_w((CONST char *)it->text, 12);

				dp_line(gid, cx, c.top + 3, cx, c.bottom - 4, 0);
			}
			break;
		case PI_SWATCH:
			for ( k = 0; k < it->n; k++ ) {
				c.left = LABEL_W + k * 18;  c.top = y + 3;
				c.right = c.left + 16;  c.bottom = c.top + 16;
				dp_fill_rect(gid, &c, it->cols[k]);
				dp_frame_rect(gid, &c, ( *it->col == it->cols[k] )
					      ? 0x000078D7U : 0x00999999U, 1);
			}
			break;
		case PI_BUTTON:
			c.left = 8;  c.top = y + 1;
			c.right = 8 + str_w(it->label, 12) + 16;  c.bottom = y + ROW_H - 2;
			bevel(gid, &c, FALSE);
			text_at(gid, c.left + 8, c.top + 15, 12, it->label, 0);
			break;
		default:
			break;
		}
		y += h + 4;
	}
	dp_frame_rect(gid, &all, 0x00000000U, 1);
	wm_damage(pop_wid, &all);
	wm_update();
}

LOCAL INT pop_height( POPUP *p )
{
	INT	i, h = 12;

	for ( i = 0; i < p->n; i++ ) {
		h += item_h(&p->it[i]) + 4;
	}

	return h;
}

/* A press inside the palette: what it changed, or -1; a button's answer */
LOCAL INT pop_press( POPUP *p, INT x, INT y, INT *p_button )
{
	INT		i, k;
	T_DPRECT	c;

	for ( i = 0; i < p->n; i++ ) {
		POPITEM	*it = &p->it[i];

		if ( y < it->r.top || y >= it->r.bottom ) {
			continue;
		}
		switch ( it->type ) {
		case PI_CHOICE: {
			INT	cx = LABEL_W, row = 0;

			for ( k = 0; k < it->n; k++ ) {
				INT	wd = str_w(it->names[k], 12) + 12;

				if ( cx + wd > POP_W - 8 && cx > LABEL_W ) {
					row++;
					cx = LABEL_W;
				}
				c.left = cx;  c.top = it->r.top + row * ROW_H;
				c.right = cx + wd;  c.bottom = c.top + ROW_H;
				if ( x >= c.left && x < c.right && y >= c.top && y < c.bottom ) {
					*it->value = k;
					return i;
				}
				cx += wd + 2;
			}
			break;
		}
		case PI_CHECK:
			*it->value = !*it->value;
			return i;
		case PI_GRID:
			k = ( ( y - it->r.top - 2 ) / 17 ) * 16 + ( x - 8 ) / 17;
			if ( x >= 8 && x < 8 + 16 * 17 && k >= 0 && k < TV_PAT_FIXED ) {
				*it->value = k;
				return i;
			}
			break;
		case PI_TEXT:
			p->focus = i;
			return -1;
		case PI_SWATCH:
			k = ( x - LABEL_W ) / 18;
			if ( x >= LABEL_W && k < it->n ) {
				*it->col = it->cols[k];
				return i;
			}
			break;
		case PI_BUTTON:
			*p_button = i;
			return -1;
		default:
			break;
		}
	}

	return -1;
}

/* A key typed into the palette's box: a letter, or one taken away */
LOCAL BOOL pop_key( POPUP *p, UINT code, UINT mods )
{
	POPITEM	*it;
	INT	n = 0;
	UB	c = 0;

	if ( p->focus < 0 ) {
		return FALSE;
	}
	it = &p->it[p->focus];
	while ( it->text[n] != 0 ) {
		n++;
	}
	if ( code == KEY_BS ) {
		if ( n > 0 ) {
			it->text[n - 1] = 0;
		}
		return TRUE;
	}
	if ( code >= 0x1E && code <= 0x27 ) {
		c = (UB)( ( code == 0x27 ) ? '0' : '1' + ( code - 0x1E ) );
		if ( code == 0x20 && ( mods & MOD_SHIFT ) != 0 ) {
			c = '#';
		}
	} else if ( code >= 0x04 && code <= 0x09 ) {
		c = (UB)( 'a' + ( code - 0x04 ) );	/* the hex letters */
	} else if ( code == 0x37 ) {
		c = '.';
	}
	if ( c != 0 && n < it->max - 1 ) {
		it->text[n] = c;
		it->text[n + 1] = 0;
		return TRUE;
	}

	return FALSE;
}

/*
 * The palette brought up under a button of the panel, and worked until
 * it is put away. Answers the button of it that was pressed, or -1.
 */
LOCAL INT pop_run( POPUP *p, INT under )
{
	T_WMWIN		pw;
	T_DPRECT	o, c;
	T_WMEV		ev;
	INT		answer = -1;
	BOOL		done = FALSE;

	if ( df_panel <= 0 || wm_ref(df_panel, &pw) < E_OK ) {
		return -1;
	}
	button_rect(under, &c);
	o.left = pw.work.left + c.left;
	o.top = pw.work.top + c.bottom + 4;
	o.right = o.left + POP_W + 2;
	o.bottom = o.top + pop_height(p) + 2;
	if ( o.right > 1280 ) {
		o.left -= o.right - 1280;
		o.right = 1280;
	}
	pop_wid = wm_open(&o, WM_ATTR_FRAME, NULL);
	if ( pop_wid < 0 ) {
		pop_wid = 0;
		return -1;
	}
	wm_raise(pop_wid);
	pop_draw(p);
	wm_composite();
	while ( !done ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		switch ( ev.type ) {
		case HID_EV_BTN_DOWN:
			if ( ev.wid != pop_wid ) {
				done = TRUE;	/* pressed elsewhere: put away */
				break;
			}
			{
				INT	button = -1;
				INT	i = pop_press(p, ev.x, ev.y, &button);

				if ( i >= 0 && p->changed != NULL ) {
					p->changed(i);
				}
				if ( button >= 0 ) {
					answer = button;
					done = TRUE;
				}
				pop_draw(p);
				panel_draw();
				wm_composite();
			}
			break;
		case HID_EV_KEY_DOWN:
			if ( ev.code == KEY_ENTER || ev.code == KEY_ESC ) {
				if ( ev.code == KEY_ENTER && p->focus >= 0 && p->changed != NULL ) {
					p->changed(p->focus);
				}
				done = TRUE;
				break;
			}
			if ( pop_key(p, ev.code, ev.mods) ) {
				pop_draw(p);
				wm_composite();
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
	/* a box still being typed in is taken as it stands */
	if ( p->focus >= 0 && p->changed != NULL ) {
		p->changed(p->focus);
	}
	wm_close(pop_wid);
	pop_wid = 0;
	if ( pop_win != NULL && pop_win->used ) {
		dt_draw(pop_win);
	}
	panel_draw();
	wm_composite();

	return answer;
}

/* 描画パレット */

LOCAL UB	pop_hex[16];
LOCAL INT	pop_on, pop_pat;

LOCAL void fill_changed( INT item )
{
	UW	c;

	switch ( item ) {
	case 0:				/* the colour typed */
		if ( hex_colour(pop_hex, &c) ) {
			fl.fill = c;
			fl.fill_pat = 0;
			pop_pat = 0;
		}
		break;
	case 1:
		fl.fill_on = (BOOL)pop_on;
		break;
	case 2:
		fl.fill_pat = ( pop_pat >= 2 ) ? pop_pat : 0;
		if ( pop_pat == 1 ) {
			fl.fill = 0x00000000U;	/* the first is black itself */
			fl.fill_pat = 0;
		}
		fl.fill_on = TRUE;
		pop_on = 1;
		break;
	default:
		return;
	}
	apply_picked(pop_win, APPLY_FILL);
}

LOCAL void fill_palette( void )
{
	POPUP	p;

	knl_memset(&p, 0, sizeof(p));
	colour_hex(fl.fill, pop_hex);
	pop_on = fl.fill_on ? 1 : 0;
	pop_pat = fl.fill_pat;
	p.it[0].type = PI_TEXT;  p.it[0].label = "塗りつぶし色:";
	p.it[0].text = pop_hex;  p.it[0].max = 8;
	p.it[1].type = PI_CHECK;  p.it[1].label = "塗りつぶし有効";
	p.it[1].value = &pop_on;
	p.it[2].type = PI_GRID;  p.it[2].label = NULL;
	p.it[2].value = &pop_pat;
	p.it[3].type = PI_BUTTON;  p.it[3].label = "パターン編集...";
	p.n = 4;
	p.focus = -1;
	p.changed = fill_changed;
	if ( pop_run(&p, B_FILL) == 3 && pop_win != NULL ) {
		df_pattern_edit(pop_win, fl.fill_pat);
	}
}

/* 線パレット */

LOCAL CONST UW	df_line_cols[] = {
	0x000000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xEE0000, 0xFF8C00, 0xFFFF00,
	0x008000, 0x7FFF00, 0x00FFFF, 0x0000FF, 0x800080, 0xFF69B4
};
LOCAL UW	pop_col;

LOCAL void stroke_changed( INT item )
{
	UW	c;

	if ( item == 0 ) {
		if ( !hex_colour(pop_hex, &c) ) {
			return;
		}
		fl.stroke = c;
		pop_col = c;
	} else {
		fl.stroke = pop_col;
		colour_hex(pop_col, pop_hex);
	}
	apply_picked(pop_win, APPLY_STROKE);
}

LOCAL void stroke_palette( void )
{
	POPUP	p;

	knl_memset(&p, 0, sizeof(p));
	colour_hex(fl.stroke, pop_hex);
	pop_col = fl.stroke;
	p.it[0].type = PI_TEXT;  p.it[0].label = "線色:";
	p.it[0].text = pop_hex;  p.it[0].max = 8;
	p.it[1].type = PI_SWATCH;  p.it[1].label = "";
	p.it[1].cols = df_line_cols;
	p.it[1].n = (INT)( sizeof(df_line_cols) / sizeof(df_line_cols[0]) );
	p.it[1].col = &pop_col;
	p.n = 2;
	p.focus = -1;
	p.changed = stroke_changed;
	(void)pop_run(&p, B_STROKE);
}

/* 線種パレット */

LOCAL CONST INT	df_widths[] = { 1, 2, 3, 5, 10 };
LOCAL INT	pop_w, pop_t, pop_c, pop_ap, pop_at;

LOCAL void lstyle_changed( INT item )
{
	switch ( item ) {
	case 0:
		fl.width = df_widths[pop_w];
		apply_picked(pop_win, APPLY_WIDTH);
		break;
	case 1:
		fl.ltype = pop_t;
		apply_picked(pop_win, APPLY_LTYPE);
		break;
	case 2:
		fl.conn = pop_c;
		apply_picked(pop_win, APPLY_CONN);
		break;
	default:
		fl.arrows = pop_ap;
		fl.arrow_filled = (BOOL)( pop_at == 1 );
		apply_picked(pop_win, APPLY_ARROWS);
		break;
	}
}

LOCAL void lstyle_palette( void )
{
	LOCAL CONST char *CONST wn[] = { "1px", "2px", "3px", "5px", "10px" };
	LOCAL CONST char *CONST tn[] = {
		"実線", "破線", "点線", "一点鎖線", "二点鎖線", "長破線"
	};
	LOCAL CONST char *CONST cn[] = { "直線", "カギ線", "曲線" };
	LOCAL CONST char *CONST an[] = { "なし", "始点のみ", "終点のみ", "両端" };
	LOCAL CONST char *CONST kn[] = { "線のみ", "塗りつぶし" };
	POPUP	p;
	INT	k;

	knl_memset(&p, 0, sizeof(p));
	pop_w = 1;
	for ( k = 0; k < 5; k++ ) {
		if ( df_widths[k] == fl.width ) {
			pop_w = k;
		}
	}
	pop_t = fl.ltype;
	pop_c = fl.conn;
	pop_ap = fl.arrows;
	pop_at = fl.arrow_filled ? 1 : 0;
	p.it[0].type = PI_CHOICE;  p.it[0].label = "線の太さ:";
	p.it[0].names = wn;  p.it[0].n = 5;  p.it[0].value = &pop_w;
	p.it[1].type = PI_CHOICE;  p.it[1].label = "線種:";
	p.it[1].names = tn;  p.it[1].n = 6;  p.it[1].value = &pop_t;
	p.it[2].type = PI_CHOICE;  p.it[2].label = "接続形状:";
	p.it[2].names = cn;  p.it[2].n = 3;  p.it[2].value = &pop_c;
	p.it[3].type = PI_CHOICE;  p.it[3].label = "矢印位置:";
	p.it[3].names = an;  p.it[3].n = 4;  p.it[3].value = &pop_ap;
	p.it[4].type = PI_CHOICE;  p.it[4].label = "矢印種類:";
	p.it[4].names = kn;  p.it[4].n = 2;  p.it[4].value = &pop_at;
	p.n = 5;
	p.focus = -1;
	p.changed = lstyle_changed;
	(void)pop_run(&p, B_LSTYLE);
}

/* 角属性パレット */

LOCAL UB	pop_num[8];

LOCAL INT word_int( CONST UB *s, INT dflt )
{
	INT	v = 0;
	BOOL	any = FALSE;

	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + ( *s++ - '0' );
		any = TRUE;
	}

	return any ? v : dflt;
}

LOCAL void corner_changed( INT item )
{
	INT	v = word_int(pop_num, -1);

	(void)item;
	if ( v < 0 || v > 100 ) {
		return;
	}
	fl.corner = v;
	apply_picked(pop_win, APPLY_CORNER);
}

LOCAL void corner_palette( void )
{
	POPUP	p;

	knl_memset(&p, 0, sizeof(p));
	(void)put_num(pop_num, 0, fl.corner);
	p.it[0].type = PI_TEXT;  p.it[0].label = "角丸半径:";
	p.it[0].text = pop_num;  p.it[0].max = 4;
	p.n = 1;
	p.focus = 0;
	p.changed = corner_changed;
	(void)pop_run(&p, B_CORNER);
}

/* 倍率 */

LOCAL CONST INT	df_zooms[] = { 1, 2, 4, 8, 16, 24, 32, 40, 64 };
LOCAL INT	pop_z;

LOCAL void zoom_changed( INT item )
{
	(void)item;
	fl.zoom = df_zooms[pop_z];
	if ( pop_win != NULL && pop_win->used ) {
		pop_win->scroll_x = pop_win->scroll_y = 0;
		if ( pop_win->fig != NULL ) {
			pop_win->fig->zoom = fl.zoom;
		}
	}
}

LOCAL void zoom_palette( void )
{
	LOCAL CONST char *CONST zn[] = {
		"x1/8", "x1/4", "x1/2", "x1", "x2", "x3", "x4", "x5", "x8"
	};
	POPUP	p;
	INT	k;

	knl_memset(&p, 0, sizeof(p));
	pop_z = 3;
	for ( k = 0; k < 9; k++ ) {
		if ( df_zooms[k] == fl.zoom ) {
			pop_z = k;
		}
	}
	p.it[0].type = PI_CHOICE;  p.it[0].label = "倍率:";
	p.it[0].names = zn;  p.it[0].n = 9;  p.it[0].value = &pop_z;
	p.n = 1;
	p.focus = -1;
	p.changed = zoom_changed;
	(void)pop_run(&p, B_ZOOM);
}

/* 格子点 */

LOCAL CONST INT	df_grids[] = { 4, 8, 16, 32, 64, 5, 10, 20, 25, 40, 50 };
LOCAL INT	pop_gm, pop_gi;

LOCAL void grid_changed( INT item )
{
	INT	v;

	switch ( item ) {
	case 0:
		fl.grid_mode = pop_gm;
		break;
	case 1:
		fl.grid = df_grids[pop_gi];
		(void)put_num(pop_num, 0, fl.grid);
		break;
	default:
		v = word_int(pop_num, -1);
		if ( v >= 1 && v <= 200 ) {
			fl.grid = v;
		}
		break;
	}
}

LOCAL void grid_palette( void )
{
	LOCAL CONST char *CONST mn[] = { "なし", "表示のみ", "格子点拘束" };
	LOCAL CONST char *CONST gn[] = {
		"4", "8", "16", "32", "64", "5", "10", "20", "25", "40", "50"
	};
	POPUP	p;
	INT	k;

	knl_memset(&p, 0, sizeof(p));
	pop_gm = fl.grid_mode;
	pop_gi = -1;
	for ( k = 0; k < 11; k++ ) {
		if ( df_grids[k] == fl.grid ) {
			pop_gi = k;
		}
	}
	(void)put_num(pop_num, 0, fl.grid);
	p.it[0].type = PI_CHOICE;  p.it[0].label = "表示モード:";
	p.it[0].names = mn;  p.it[0].n = 3;  p.it[0].value = &pop_gm;
	p.it[1].type = PI_CHOICE;  p.it[1].label = "間隔 (px):";
	p.it[1].names = gn;  p.it[1].n = 11;  p.it[1].value = &pop_gi;
	p.it[2].type = PI_TEXT;  p.it[2].label = "自由入力:";
	p.it[2].text = pop_num;  p.it[2].max = 4;
	p.n = 3;
	p.focus = -1;
	p.changed = grid_changed;
	(void)pop_run(&p, B_GRID);
}

LOCAL void text_look( INT item );

/* 書式: the look of the text of a new box of text */

LOCAL CONST INT	df_sizes[] = { 10, 12, 14, 16, 18, 20, 24, 28, 32, 36 };
LOCAL CONST UW	df_text_cols[] = {
	0x000000, 0x0000FF, 0xEE0000, 0xFF69B4, 0xFF8C00, 0x008000, 0x7FFF00,
	0x00FFFF, 0xFFFF00, 0xFFFFFF
};
LOCAL INT	pop_fs, pop_ff, pop_b, pop_i, pop_u, pop_s;

LOCAL void font_changed( INT item )
{
	UW	c;

	switch ( item ) {
	case 0:	fl.font_size = df_sizes[pop_fs];	break;
	case 1:	fl.font_face = pop_ff;			break;
	case 2:
		if ( hex_colour(pop_hex, &c) ) {
			fl.text_col = c;
			pop_col = c;
		}
		break;
	case 3:
		fl.text_col = pop_col;
		colour_hex(pop_col, pop_hex);
		break;
	case 4:	fl.bold = (BOOL)pop_b;			break;
	case 5:	fl.italic = (BOOL)pop_i;		break;
	case 6:	fl.under = (BOOL)pop_u;			break;
	default: fl.strike = (BOOL)pop_s;		break;
	}
	text_look(item);
}

LOCAL void font_palette( void )
{
	LOCAL CONST char *CONST sn[] = {
		"10", "12", "14", "16", "18", "20", "24", "28", "32", "36"
	};
	LOCAL CONST char *CONST fn[] = { "ゴシック", "明朝", "等幅" };
	POPUP	p;
	INT	k;

	knl_memset(&p, 0, sizeof(p));
	pop_fs = 3;
	for ( k = 0; k < 10; k++ ) {
		if ( df_sizes[k] == fl.font_size ) {
			pop_fs = k;
		}
	}
	pop_ff = fl.font_face;
	colour_hex(fl.text_col, pop_hex);
	pop_col = fl.text_col;
	pop_b = fl.bold;  pop_i = fl.italic;  pop_u = fl.under;  pop_s = fl.strike;
	p.it[0].type = PI_CHOICE;  p.it[0].label = "サイズ:";
	p.it[0].names = sn;  p.it[0].n = 10;  p.it[0].value = &pop_fs;
	p.it[1].type = PI_CHOICE;  p.it[1].label = "フォント:";
	p.it[1].names = fn;  p.it[1].n = 3;  p.it[1].value = &pop_ff;
	p.it[2].type = PI_TEXT;  p.it[2].label = "文字色:";
	p.it[2].text = pop_hex;  p.it[2].max = 8;
	p.it[3].type = PI_SWATCH;  p.it[3].label = "";
	p.it[3].cols = df_text_cols;  p.it[3].n = 10;  p.it[3].col = &pop_col;
	p.it[4].type = PI_CHECK;  p.it[4].label = "太字";  p.it[4].value = &pop_b;
	p.it[5].type = PI_CHECK;  p.it[5].label = "斜体";  p.it[5].value = &pop_i;
	p.it[6].type = PI_CHECK;  p.it[6].label = "下線";  p.it[6].value = &pop_u;
	p.it[7].type = PI_CHECK;  p.it[7].label = "打ち消し線";  p.it[7].value = &pop_s;
	p.n = 8;
	p.focus = -1;
	p.changed = font_changed;
	(void)pop_run(&p, B_FONT);
}

/* 画材: while a canvas is painted in, the canvas button offers these */

LOCAL INT	pop_pt, pop_ps;

LOCAL void paint_changed( INT item )
{
	UW	c;

	switch ( item ) {
	case 0:
		fl.paint_tool = pop_pt;
		break;
	case 1:
		if ( hex_colour(pop_hex, &c) ) {
			fl.stroke = c;
			pop_col = c;
		}
		break;
	case 2:
		fl.stroke = pop_col;
		colour_hex(pop_col, pop_hex);
		break;
	default:
		fl.paint_size = ( pop_ps == 0 ) ? 1 : ( pop_ps == 1 ) ? 3
			      : ( pop_ps == 2 ) ? 5 : ( pop_ps == 3 ) ? 10 : 20;
		break;
	}
}

LOCAL void paint_palette( void )
{
	LOCAL CONST char *CONST tn[] = {
		"鉛筆", "絵筆", "消しゴム", "エアブラシ", "絵の具", "カッター"
	};
	LOCAL CONST char *CONST sn[] = { "1px", "3px", "5px", "10px", "20px" };
	POPUP	p;

	knl_memset(&p, 0, sizeof(p));
	pop_pt = fl.paint_tool;
	pop_ps = ( fl.paint_size <= 1 ) ? 0 : ( fl.paint_size <= 3 ) ? 1
	       : ( fl.paint_size <= 5 ) ? 2 : ( fl.paint_size <= 10 ) ? 3 : 4;
	colour_hex(fl.stroke, pop_hex);
	pop_col = fl.stroke;
	p.it[0].type = PI_CHOICE;  p.it[0].label = "画材:";
	p.it[0].names = tn;  p.it[0].n = 6;  p.it[0].value = &pop_pt;
	p.it[1].type = PI_TEXT;  p.it[1].label = "描画色:";
	p.it[1].text = pop_hex;  p.it[1].max = 8;
	p.it[2].type = PI_SWATCH;  p.it[2].label = "";
	p.it[2].cols = df_line_cols;
	p.it[2].n = (INT)( sizeof(df_line_cols) / sizeof(df_line_cols[0]) );
	p.it[2].col = &pop_col;
	p.it[3].type = PI_CHOICE;  p.it[3].label = "太さ:";
	p.it[3].names = sn;  p.it[3].n = 5;  p.it[3].value = &pop_ps;
	p.n = 4;
	p.focus = -1;
	p.changed = paint_changed;
	(void)pop_run(&p, B_CANVAS);
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
	T_DPRECT	o;
	INT		h = PH + 30;

	if ( df_panel > 0 || d == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	o.left = w.outer.left;
	o.top = w.outer.top - h - 4;
	if ( o.top < 0 ) {
		o.top = w.outer.bottom + 4;
	}
	if ( o.left + PW + 8 > 1280 ) {
		o.left = 1280 - PW - 8;
	}
	if ( o.left < 0 ) {
		o.left = 0;
	}
	o.right = o.left + PW + 8;
	o.bottom = o.top + h;
	df_panel = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "道具パネル");
	if ( df_panel < 0 ) {
		df_panel = 0;
		return;
	}
	panel_draw();
	wm_raise(d->wid);
	wm_raise(df_panel);
	wm_focus(d->wid);
}

EXPORT void df_panel_hide( void )
{
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
	wm_composite();
}

/*
 * A press, if it is on the panel: a tool taken, a palette brought up,
 * or the panel taken by its band to be carried. TRUE when it was.
 */
EXPORT BOOL df_panel_press( CONST T_WMEV *ev )
{
	T_DPRECT	c;
	INT		b, t;

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
	for ( b = 0; b < B_N; b++ ) {
		button_rect(b, &c);
		if ( ev->x < c.left || ev->x >= c.right
		  || ev->y < c.top || ev->y >= c.bottom ) {
			continue;
		}
		t = button_tool(b);
		if ( t == TL_CANVAS && df_tool == TL_CANVAS && df_canvas_active() ) {
			/* painting: the canvas button is the 画材 */
			pop_win = panel_target();
			paint_palette();
			break;
		}
		if ( t >= 0 ) {
			if ( t == df_tool && centre_able(t) ) {
				df_centre[t] = (BOOL)!df_centre[t];
			} else if ( t != df_tool ) {
				df_canvas_leave();
				df_tool = t;
			}
			df_drawing = NULL;
			df_npt = 0;
			wm_set_pointer(( t == TL_SELECT ) ? WM_PT_SELECT : WM_PT_MODIFY);
			break;
		}
		pop_win = panel_target();
		switch ( b ) {
		case B_FILL:	fill_palette();		break;
		case B_STROKE:	stroke_palette();	break;
		case B_LSTYLE:	lstyle_palette();	break;
		case B_CORNER:	corner_palette();	break;
		case B_ZOOM:	zoom_palette();		break;
		case B_GRID:	grid_palette();		break;
		case B_FONT:	font_palette();		break;
		default:	break;
		}
		if ( pop_win != NULL && pop_win->used ) {
			dt_draw(pop_win);
			wm_focus(pop_win->wid);
		}
		break;
	}
	panel_draw();
	wm_update();

	return TRUE;
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
		wm_composite();
	}
}

EXPORT void df_panel_let_go( void )
{
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
	df_tool = TL_SELECT;
	df_drawing = NULL;
	df_npt = 0;
	panel_draw();
}

/* ---------------------------------------------------------------- 変形 */

/*
 * A picked shape changed point by point: a polygon's corners, the ends
 * of an arc -- and a sector's middle, which carries the whole of it --
 * and the two ends of a curve. Each point is a handle carried by the
 * hand; pressing anywhere else ends it.
 */
#define TS_POINT	0		/* + which corner of a polygon */
#define TS_CENTRE	1000
#define TS_START	1001
#define TS_END		1002

#define TS_MAX		( TV_MAX_PT + 3 )

LOCAL DTWIN	*ts_win = NULL;
LOCAL INT	ts_shape = -1;
LOCAL INT	ts_drag = -1;		/* which handle, while carried */
LOCAL T_DPPOINT	ts_pts[TV_MAX_PT];	/* the points as they are being moved */
LOCAL INT	ts_npt;
LOCAL T_DPPOINT	ts_c, ts_s, ts_e;	/* an arc's middle and ends */

LOCAL BOOL transformable( CONST T_TVSHAPE *s )
{
	if ( s->node == NULL ) {
		return FALSE;
	}
	return (BOOL)( s->kind == TV_SH_ARC || s->kind == TV_SH_CHORD
		    || s->kind == TV_SH_EARC || s->kind == TV_SH_CURVE
		    || ( s->kind == TV_SH_POLY && same_word(s->node->name, "polygon") ) );
}

/* Where an arc's ends are: on its ellipse, in the directions its angles give */
LOCAL void arc_ends( CONST T_TVSHAPE *s )
{
	T_DPPOINT	p[4];
	D		cx = (D)( s->r.left + s->r.right ) * 8;
	D		cy = (D)( s->r.top + s->r.bottom ) * 8;
	D		rx = (D)( s->r.right - s->r.left ) * 8;
	D		ry = (D)( s->r.bottom - s->r.top ) * 8;

	ts_c.x = ( s->r.left + s->r.right ) / 2;
	ts_c.y = ( s->r.top + s->r.bottom ) / 2;
	(void)tv_arc_points(p, 3, cx, cy, rx, ry, s->a0, s->a0, s->angle);
	ts_s = p[0];
	(void)tv_arc_points(p, 3, cx, cy, rx, ry, s->a1, s->a1, s->angle);
	ts_e = p[0];
}

/* The handles of the shape being changed, in the figure's coordinates */
LOCAL INT ts_handles( T_DPPOINT *out, INT *which )
{
	CONST T_TVSHAPE	*s;
	INT		n = 0, i;

	if ( ts_win == NULL || ts_win->fig == NULL || ts_shape < 0
	  || ts_shape >= ts_win->fig->nsh ) {
		return 0;
	}
	s = &ts_win->fig->sh[ts_shape];
	switch ( s->kind ) {
	case TV_SH_POLY:
		for ( i = 0; i < ts_npt; i++ ) {
			out[n] = ts_pts[i];
			which[n++] = TS_POINT + i;
		}
		break;
	case TV_SH_CURVE:
		if ( ts_npt >= 2 ) {
			out[n] = ts_pts[0];
			which[n++] = TS_START;
			out[n] = ts_pts[ts_npt - 1];
			which[n++] = TS_END;
		}
		break;
	default:
		if ( s->kind == TV_SH_ARC ) {
			out[n] = ts_c;
			which[n++] = TS_CENTRE;
		}
		out[n] = ts_s;
		which[n++] = TS_START;
		out[n] = ts_e;
		which[n++] = TS_END;
		break;
	}

	return n;
}

/* The shape's points taken to be moved */
LOCAL void ts_take( void )
{
	CONST T_TVSHAPE	*s = &ts_win->fig->sh[ts_shape];
	INT		i;

	ts_npt = 0;
	for ( i = 0; i < s->npt && i < TV_MAX_PT; i++ ) {
		ts_pts[ts_npt++] = s->pt[i];
	}
	if ( s->kind == TV_SH_ARC || s->kind == TV_SH_CHORD || s->kind == TV_SH_EARC ) {
		arc_ends(s);
	}
}

EXPORT void df_transform( DTWIN *d )
{
	INT	i;

	ts_win = NULL;
	for ( i = 0; d != NULL && d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && transformable(&d->fig->sh[i]) ) {
			ts_win = d;
			ts_shape = i;
			ts_drag = -1;
			ts_take();
			dt_draw(d);
			wm_composite();
			return;
		}
	}
	dt_tell(d, "この図形は変形できません", NULL);
}

/* A press while a shape is being changed: on a handle, it is carried */
EXPORT BOOL df_transform_press( DTWIN *d, INT x, INT y )
{
	T_DPPOINT	h[TS_MAX];
	INT		which[TS_MAX], n, i, wx, wy;

	if ( ts_win == NULL || ts_win != d ) {
		return FALSE;
	}
	n = ts_handles(h, which);
	for ( i = 0; i < n; i++ ) {
		fig_to_win(d, h[i].x, h[i].y, &wx, &wy);
		if ( x >= wx - 5 && x <= wx + 5 && y >= wy - 5 && y <= wy + 5 ) {
			ts_drag = which[i];
			return TRUE;
		}
	}
	/* anywhere else: the change is over */
	ts_win = NULL;
	dt_draw(d);
	wm_composite();

	return FALSE;
}

LOCAL BOOL ts_active( DTWIN *d )
{
	return (BOOL)( ts_win != NULL && ts_win == d );
}

EXPORT void df_transform_end( DTWIN *d )
{
	if ( ts_win == d ) {
		ts_win = NULL;
		ts_drag = -1;
		dt_draw(d);
		wm_composite();
	}
}

EXPORT BOOL df_transform_carrying( void )
{
	return (BOOL)( ts_win != NULL && ts_drag >= 0 );
}

EXPORT void df_transform_follow( INT sx, INT sy )
{
	DTWIN	*d = ts_win;
	T_WMWIN	w;
	INT	fx, fy;

	if ( d == NULL || ts_drag < 0 || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	to_fig(d, sx - w.work.left, sy - w.work.top, &fx, &fy);
	snap(&fx, &fy);
	if ( ts_drag == TS_CENTRE ) {
		INT	dx = fx - ts_c.x, dy = fy - ts_c.y;

		ts_c.x += dx;  ts_c.y += dy;
		ts_s.x += dx;  ts_s.y += dy;
		ts_e.x += dx;  ts_e.y += dy;
	} else if ( ts_drag == TS_START ) {
		if ( ts_npt >= 2 && ts_win->fig->sh[ts_shape].kind == TV_SH_CURVE ) {
			ts_pts[0].x = fx;  ts_pts[0].y = fy;
		} else {
			ts_s.x = fx;  ts_s.y = fy;
		}
	} else if ( ts_drag == TS_END ) {
		if ( ts_npt >= 2 && ts_win->fig->sh[ts_shape].kind == TV_SH_CURVE ) {
			ts_pts[ts_npt - 1].x = fx;  ts_pts[ts_npt - 1].y = fy;
		} else {
			ts_e.x = fx;  ts_e.y = fy;
		}
	} else if ( ts_drag >= TS_POINT && ts_drag < TS_POINT + ts_npt ) {
		ts_pts[ts_drag - TS_POINT].x = fx;
		ts_pts[ts_drag - TS_POINT].y = fy;
	}
	pointer_at(fx, fy);
	dt_draw(d);
	wm_composite();
}

/* The change put into the record, and the handles kept for more */
EXPORT void df_transform_release( void )
{
	DTWIN		*d = ts_win;
	CONST T_TVSHAPE	*s;
	T_TAD		*rec;
	T_TADNODE	*nd;

	if ( d == NULL || ts_drag < 0 ) {
		return;
	}
	ts_drag = -1;
	if ( d->fig == NULL || ts_shape >= d->fig->nsh ) {
		return;
	}
	s = &d->fig->sh[ts_shape];
	nd = s->node;
	rec = rec_of(d);
	ed_before(d);
	if ( s->kind == TV_SH_POLY || s->kind == TV_SH_CURVE ) {
		df_points_put(rec, nd, ts_pts, ts_npt);
	} else {
		INT	hw = ( s->r.right - s->r.left ) / 2;
		INT	hh = ( s->r.bottom - s->r.top ) / 2;

		/* the frame follows the middle; the ends are where they are towards */
		frame_put(rec, nd, ts_c.x - hw, ts_c.y - hh, ts_c.x + hw, ts_c.y + hh);
		df_attr_put(rec, nd, "startX", ts_s.x);
		df_attr_put(rec, nd, "startY", ts_s.y);
		df_attr_put(rec, nd, "endX", ts_e.x);
		df_attr_put(rec, nd, "endY", ts_e.y);
	}
	ed_changed(d);
	/* the model is made again: take the points afresh from it */
	if ( ts_win != NULL && ts_win->fig != NULL && ts_shape < ts_win->fig->nsh ) {
		ts_take();
	}
	dt_draw(d);
	wm_composite();
}

/* The shape's new lines while a handle is carried, or else the handles */
LOCAL void ts_draw( DTWIN *d, INT gid, BOOL lines )
{
	T_DPPOINT	h[TS_MAX];
	INT		which[TS_MAX], n, i, wx, wy;

	if ( ts_win != d ) {
		return;
	}
	if ( lines ) {
		if ( ts_drag < 0 ) {
			return;
		}
		CONST T_TVSHAPE	*s = &d->fig->sh[ts_shape];

		if ( s->kind == TV_SH_POLY || s->kind == TV_SH_CURVE ) {
			for ( i = 0; i + 1 < ts_npt; i++ ) {
				xor_line(d, gid, ts_pts[i].x, ts_pts[i].y,
					 ts_pts[i + 1].x, ts_pts[i + 1].y);
			}
			if ( s->kind == TV_SH_POLY && ts_npt > 2 ) {
				xor_line(d, gid, ts_pts[ts_npt - 1].x, ts_pts[ts_npt - 1].y,
					 ts_pts[0].x, ts_pts[0].y);
			}
		} else {
			xor_line(d, gid, ts_c.x, ts_c.y, ts_s.x, ts_s.y);
			xor_line(d, gid, ts_c.x, ts_c.y, ts_e.x, ts_e.y);
		}
		return;
	}
	n = ts_handles(h, which);
	for ( i = 0; i < n; i++ ) {
		T_DPRECT	b;

		fig_to_win(d, h[i].x, h[i].y, &wx, &wy);
		b.left = wx - 4;  b.top = wy - 4;
		b.right = wx + 4;  b.bottom = wy + 4;
		dp_fill_rect(gid, &b, ( which[i] == TS_CENTRE ) ? 0x00FF8F00U : 0x00FFFFFFU);
		dp_frame_rect(gid, &b, ( which[i] == TS_CENTRE ) ? 0x00000000U : 0x00FF8F00U, 1);
	}
}

/* ---------------------------------------------------------------- 用紙枠 */

/*
 * The paper a figure is laid on, drawn over it: each page's edge, and
 * inside it its margins, in broken lines, as many pages as the figure
 * and the window reach. The paper is in points; the figure's own units
 * are pixels at 96 to the inch.
 */
LOCAL void paper_draw( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	T_TVFIG		*f = d->fig;
	INT		pw, ph, fw = 0, fh = 0, x, y, k;
	INT		m[4];
	T_DPPAT		pat;

	if ( !d->paper_frame || f == NULL ) {
		return;
	}
	pw = ( ( f->paper_w > 0 ) ? f->paper_w : DT_PAPER_W ) * 4 / 3;
	ph = ( ( f->paper_h > 0 ) ? f->paper_h : DT_PAPER_H ) * 4 / 3;
	for ( k = 0; k < 4; k++ ) {
		m[k] = f->pmargin[k] * 4 / 3;
	}
	(void)tv_fig_size(f, &fw, &fh);
	x = ( page->right - page->left + d->scroll_x ) * 8 / fl.zoom;
	y = ( page->bottom - page->top + d->scroll_y ) * 8 / fl.zoom;
	if ( fw < x ) fw = x;
	if ( fh < y ) fh = y;
	dp_pat_colour(&pat, 0x00808080U);
	for ( y = 0; y < fh; y += ph ) {
		for ( x = 0; x < fw; x += pw ) {
			INT	x0, y0, x1, y1;

			fig_to_win(d, x, y, &x0, &y0);
			fig_to_win(d, x + pw, y + ph, &x1, &y1);
			dp_line_wide(gid, x0, y0, x1, y0, 1, DP_LINE_DASH, &pat);
			dp_line_wide(gid, x0, y1, x1, y1, 1, DP_LINE_DASH, &pat);
			dp_line_wide(gid, x0, y0, x0, y1, 1, DP_LINE_DASH, &pat);
			dp_line_wide(gid, x1, y0, x1, y1, 1, DP_LINE_DASH, &pat);
			if ( m[0] + m[1] + m[2] + m[3] > 0 ) {
				T_DPPAT	mp;

				dp_pat_colour(&mp, 0x00C0C0C0U);
				fig_to_win(d, x + m[0], y + m[1], &x0, &y0);
				fig_to_win(d, x + pw - m[2], y + ph - m[3], &x1, &y1);
				dp_line_wide(gid, x0, y0, x1, y0, 1, DP_LINE_DOT, &mp);
				dp_line_wide(gid, x0, y1, x1, y1, 1, DP_LINE_DOT, &mp);
				dp_line_wide(gid, x0, y0, x0, y1, 1, DP_LINE_DOT, &mp);
				dp_line_wide(gid, x1, y0, x1, y1, 1, DP_LINE_DOT, &mp);
			}
		}
	}
}

EXPORT void df_paper_toggle( DTWIN *d )
{
	d->paper_frame = (BOOL)!d->paper_frame;
	dt_draw(d);
	wm_composite();
}

/* 用紙枠設定: the paper and its margins, written ahead of the figure */
EXPORT void df_page_setup( DTWIN *d )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*root = tad_root(rec), *body = tad_body(rec), *pp = NULL, *dm = NULL, *n;
	T_DTPAPER	ps;
	T_TVFIG		*f = d->fig;
	INT		r;

	if ( root == NULL || body == NULL || f == NULL || d->sealed ) {
		return;
	}
	for ( n = root->first; n != NULL && n != body; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "paper") ) pp = n;
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "docmargin") ) dm = n;
	}
	/* points as tenths of a millimetre, to the whole millimetre */
	ps.w = ( ( ( f->paper_w > 0 ? f->paper_w : DT_PAPER_W ) * 254 / 72 ) + 5 ) / 10 * 10;
	ps.h = ( ( ( f->paper_h > 0 ? f->paper_h : DT_PAPER_H ) * 254 / 72 ) + 5 ) / 10 * 10;
	ps.left = ( f->pmargin[0] * 254 / 72 + 5 ) / 10 * 10;
	ps.top = ( f->pmargin[1] * 254 / 72 + 5 ) / 10 * 10;
	ps.right = ( f->pmargin[2] * 254 / 72 + 5 ) / 10 * 10;
	ps.bottom = ( f->pmargin[3] * 254 / 72 + 5 ) / 10 * 10;
	ps.imposition = ( pp != NULL && tad_attr(pp, "imposition") != NULL
			  && tad_attr(pp, "imposition")[0] == '1' ) ? 1 : 0;
	r = dt_paper_form(d, &ps);
	if ( r == DT_PAPER_CANCEL ) {
		return;
	}
	if ( r == DT_PAPER_STD ) {
		ps.w = 2100;  ps.h = 2970;
		ps.top = 200;  ps.bottom = 200;
		ps.left = 200;  ps.right = 100;
		ps.imposition = 0;
	}
	ed_before(d);
	if ( pp == NULL ) {
		pp = tad_elem_new(rec, "paper", root, body, TRUE);
	}
	if ( dm == NULL ) {
		dm = tad_elem_new(rec, "docmargin", root, body, TRUE);
	}
	if ( pp != NULL ) {
		df_attr_put(rec, pp, "imposition", ps.imposition);
		df_attr_put(rec, pp, "binding", 0);
		df_attr_put(rec, pp, "length", ( ps.h * 72 + 127 ) / 254);
		df_attr_put(rec, pp, "width", ( ps.w * 72 + 127 ) / 254);
		df_attr_put(rec, pp, "top", ( ps.top * 72 + 127 ) / 254);
		df_attr_put(rec, pp, "bottom", ( ps.bottom * 72 + 127 ) / 254);
		df_attr_put(rec, pp, "left", ( ps.left * 72 + 127 ) / 254);
		df_attr_put(rec, pp, "right", ( ps.right * 72 + 127 ) / 254);
	}
	if ( dm != NULL ) {
		df_attr_put(rec, dm, "top", ( ps.top * 72 + 127 ) / 254);
		df_attr_put(rec, dm, "bottom", ( ps.bottom * 72 + 127 ) / 254);
		df_attr_put(rec, dm, "left", ( ps.left * 72 + 127 ) / 254);
		df_attr_put(rec, dm, "right", ( ps.right * 72 + 127 ) / 254);
	}
	ed_changed(d);
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
	INT		gid, z;
	T_WMWIN		w;
	T_DPRECT	all;

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
	z = d->fig->zoom;
	d->fig->zoom = 8;
	tv_fig_draw(gid, d->fig, &all, d->scroll_x * 8 / fl.zoom,
		    d->scroll_y * 8 / fl.zoom, d->rec, d->paper);
	d->fig->zoom = z;
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
		wm_composite();
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
	wm_raise(rs_wid);
	wm_focus(d->wid);
	wm_composite();
}

/* A window gone: a 実寸 panel of it goes too */
EXPORT void df_forget( DTWIN *d )
{
	if ( rs_of == d && rs_wid > 0 ) {
		wm_close(rs_wid);
		rs_wid = 0;
		rs_of = NULL;
	}
	if ( ts_win == d ) {
		ts_win = NULL;
	}
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
		dd_press(tb, x, y, FALSE);
		dd_release();
	}
	dt_draw(d);
	wm_composite();
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
		wm_composite();
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
			dd_press(d->tb, x, y, FALSE);
			dt_draw(d);
			wm_composite();
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
			df_tool = TL_CANVAS;
			df_drawing = NULL;
			df_npt = 0;
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
		  && transformable(&d->fig->sh[which]) && df_takeable(d, which) ) {
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
	paper_draw(d, gid, page);
	if ( ts_win == d ) {
		dp_set_mode(gid, DP_MODE_XOR);
		ts_draw(d, gid, TRUE);
		dp_set_mode(gid, DP_MODE_COPY);
		ts_draw(d, gid, FALSE);
	}
	rs_draw(d);
}

/* The pattern the inside is filled with now, for パターン編集 to start from */
EXPORT INT df_fill_pattern( void )
{
	return ( fl.fill_pat > 0 ) ? fl.fill_pat : 2;
}
