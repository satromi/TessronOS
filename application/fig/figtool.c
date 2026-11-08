/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figtool.c
 *	The figure editor's 道具パネル apart from its windows (design 17.13)
 *
 *	What a new shape is drawn with and the tool in hand; a shape drawn
 *	by pressing and dragging out its box, or, for a polygon or a
 *	polyline, by pressing each corner in turn, shown in inverted lines while it is drawn and
 *	then written into the record in the form the record's other readers
 *	read: the colours as pattern numbers -- a fixed one when the colour
 *	is one of them, else one of the record's own in its <patterns> --
 *	and the arcs as their frame and the two points their ends are
 *	towards. The grid over the figure, the palettes' items and how they
 *	are laid out, and the fixed patterns the 塗りつぶし palette chooses
 *	among, drawn and pressed.
 *
 *	The desktop's figure editor (application/desktop/dtftool.c) and the
 *	one running as a process (application/fig/fg_tool.c) both use this,
 *	each with its own panel and palette windows made of the system's
 *	parts, through T_FTVIEW and T_FTHOST (include/ts/figtool.h).
 */

#include <ts/tvport.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/hid.h>
#include <ts/figedit.h>
#include <ts/docedit.h>
#include <ts/figtool.h>

#define KEY_ENTER	0x28
#define KEY_ESC		0x29

#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- the look and the tools */

EXPORT T_FTLOOK	ft_look = {
	0x00FFFFFFU, TRUE, 0, 0x00000000U, 2, 0, TV_CONN_STRAIGHT, 0, 0, FALSE,
	FT_GRID_NONE, 16, 8, 0, 5,
	16, 0, 0x00000000U, FALSE, FALSE, FALSE, FALSE
};

#define fl	ft_look

EXPORT INT	ft_tool = FT_TL_SELECT;

/*
 * The tools that can draw from the middle out: pressing the tool that
 * is already in hand turns that on or off for it, and the place first
 * pressed is then the middle of what is drawn.
 */
EXPORT BOOL	ft_centre[FT_TL_N];

EXPORT BOOL ft_centre_able( INT t )
{
	return (BOOL)( t == FT_TL_LINE || t == FT_TL_RECT || t == FT_TL_ELLIPSE
		    || t == FT_TL_ARC || t == FT_TL_CHORD || t == FT_TL_EARC );
}

/*
 * The tools whose shape is drawn corner by corner: the first side
 * dragged, then a press for each corner. A polygon is closed however it
 * is ended; a polyline is left open.
 */
LOCAL BOOL by_corners( INT t )
{
	return (BOOL)( t == FT_TL_POLYGON || t == FT_TL_POLYLINE );
}

/* A shape being drawn: where the hand went down, and where it is */
LOCAL void	*ft_drawing = NULL;	/* the arg of the view it is drawn in */
LOCAL INT	ft_x0, ft_y0, ft_x1, ft_y1;	/* in the figure's coordinates */
LOCAL T_DPPOINT	*ft_pts = NULL;		/* as many as are drawn: 'ft_maxpt' of room */
LOCAL INT	ft_npt = 0, ft_maxpt = 0;
LOCAL UD	ft_last_at = 0;
LOCAL BOOL	ft_first_edge = FALSE;	/* a polygon's first side, being dragged */
LOCAL INT	ft_c0_place = -1, ft_c0_point = -1;	/* a line's joined ends */
LOCAL INT	ft_c1_place = -1, ft_c1_point = -1;
LOCAL BOOL	ft_told = FALSE;	/* the want of memory told for this shape */

EXPORT BOOL	ft_have_xy = FALSE;
EXPORT INT	ft_px, ft_py;

/*
 * Room for n points of the shape being drawn: a line drawn by hand has
 * as many as the hand gives it. Without the memory for more, that is
 * told once and the points it has are kept.
 */
LOCAL BOOL pts_room( T_FTVIEW *v, INT n )
{
	INT		m;
	T_DPPOINT	*p;

	if ( n <= ft_maxpt ) {
		return TRUE;
	}
	m = ( ft_maxpt > 0 ) ? ft_maxpt * 2 : 256;
	while ( m < n ) {
		m *= 2;
	}
	p = (T_DPPOINT *)Krealloc(ft_pts, sizeof(T_DPPOINT) * (size_t)m);
	if ( p == NULL ) {
		if ( !ft_told ) {
			ft_told = TRUE;
			v->ed->host->no_memory(v->ed);
		}
		return FALSE;
	}
	ft_pts = p;
	ft_maxpt = m;

	return TRUE;
}

/* ---------------------------------------------------------------- numbers and colours */

EXPORT INT ft_put_num( UB *buf, INT at, INT v )
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

EXPORT void ft_colour_hex( UW col, UB *out )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	out[0] = '#';
	for ( i = 0; i < 6; i++ ) {
		out[1 + i] = (UB)hex[( col >> ( 20 - i * 4 ) ) & 0xF];
	}
	out[7] = 0;
}

EXPORT BOOL ft_hex_colour( CONST UB *s, UW *p_col )
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

EXPORT BOOL ft_same_word( CONST UB *a, CONST char *b )
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
EXPORT INT ft_pattern_for( T_TAD *rec, UW col )
{
	T_TADNODE	*body = tad_body(rec), *pats = NULL, *n;
	UB		hex[8], num[16];
	INT		fixed = ( col == 0x00000000U ) ? 1 : tv_pattern_of(col);
	INT		top = TV_PAT_FIXED - 1;

	if ( fixed > 0 || body == NULL ) {
		return ( fixed > 0 ) ? fixed : 1;
	}
	ft_colour_hex(col, hex);
	for ( n = body->first; n != NULL; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && ft_same_word(n->name, "patterns") ) {
			pats = n;
			break;
		}
	}
	for ( n = ( pats != NULL ) ? pats->first : NULL; n != NULL; n = n->next ) {
		CONST UB	*id, *fg;
		INT		v = 0, k;

		if ( n->kind != TAD_ND_ELEM || !ft_same_word(n->name, "pattern") ) {
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

			if ( ft_hex_colour(fg, &c) && c == col && fg[7] == 0 ) {
				return v;
			}
		}
	}
	if ( pats == NULL ) {
		T_TADNODE	*at = body->first;

		/* after the figure's view, drawing area and scale */
		while ( at != NULL && ( at->kind != TAD_ND_ELEM
			|| ft_same_word(at->name, "figView")
			|| ft_same_word(at->name, "figDraw")
			|| ft_same_word(at->name, "figScale") ) ) {
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
	(void)ft_put_num(num, 0, top + 1);
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
EXPORT INT ft_fill_pattern( T_TAD *rec )
{
	if ( !fl.fill_on ) {
		return 0;
	}
	if ( fl.fill_pat > 0 ) {
		return fl.fill_pat;
	}

	return ft_pattern_for(rec, fl.fill);
}

/* ---------------------------------------------------------------- places */

EXPORT BOOL ft_is( CONST T_FTVIEW *v )
{
	return (BOOL)( v != NULL && v->ed != NULL && v->ed->usable && v->ed->fig != NULL );
}

/* A place in the window, in the figure's own coordinates */
EXPORT void ft_to_fig( CONST T_FTVIEW *v, INT x, INT y, INT *fx, INT *fy )
{
	*fx = ( x - v->page.left + v->sx ) * 8 / fl.zoom;
	*fy = ( y - v->page.top + v->sy ) * 8 / fl.zoom;
}

EXPORT void ft_fig_to_win( CONST T_FTVIEW *v, INT fx, INT fy, INT *x, INT *y )
{
	*x = v->page.left + fx * fl.zoom / 8 - v->sx;
	*y = v->page.top + fy * fl.zoom / 8 - v->sy;
}

/* On the grid, when the grid holds */
EXPORT void ft_snap( INT *x, INT *y )
{
	INT	g = fl.grid;

	if ( fl.grid_mode != FT_GRID_SNAP || g <= 1 ) {
		return;
	}
	*x = ( ( *x >= 0 ? *x + g / 2 : *x - g / 2 ) / g ) * g;
	*y = ( ( *y >= 0 ? *y + g / 2 : *y - g / 2 ) / g ) * g;
}

/*
 * A place of a window, in its pixels, on the grid when the grid holds:
 * what is carried or sized follows the grid as the hand moves, so that
 * its outline shows where it will be let go.
 */
EXPORT void ft_snap_win( INT ox, INT oy, INT zoom, INT *x, INT *y )
{
	INT	fx, fy;

	if ( fl.grid_mode != FT_GRID_SNAP || fl.grid <= 1 || zoom <= 0 ) {
		return;
	}
	fx = ( *x - ox ) * 8 / zoom;
	fy = ( *y - oy ) * 8 / zoom;
	ft_snap(&fx, &fy);
	*x = ox + fx * zoom / 8;
	*y = oy + fy * zoom / 8;
}

/*
 * The point of a shape a line may be joined to that is under a place,
 * if any is within reach: which shape by its place in the figure, and
 * which of its points.
 */
#define JOIN_REACH	8

EXPORT BOOL ft_join_at( CONST T_FTVIEW *v, INT x, INT y, INT *p_place, INT *p_point,
			INT *p_x, INT *p_y )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		i, k, best = JOIN_REACH * JOIN_REACH + 1;
	T_DPPOINT	p, dir;
	BOOL		found = FALSE;

	for ( i = 0; f != NULL && i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];

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

LOCAL INT top_z( CONST T_FTVIEW *v )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		i, top = 0;

	for ( i = 0; f != NULL && i < f->nsh; i++ ) {
		if ( f->sh[i].z > top ) {
			top = f->sh[i].z;
		}
	}

	return top;
}

/* The look every new shape is written with */
LOCAL void look_put( T_TAD *rec, T_TADNODE *nd, BOOL filled )
{
	fe_attr_put(rec, nd, "mode", 0);
	fe_attr_put(rec, nd, "lineType", fl.ltype);
	fe_attr_put(rec, nd, "lineWidth", fl.width);
	fe_attr_put(rec, nd, "l_pat", ft_pattern_for(rec, fl.stroke));
	fe_attr_put(rec, nd, "f_pat", filled ? ft_fill_pattern(rec) : 0);
}

LOCAL void arrows_put( T_TAD *rec, T_TADNODE *nd )
{
	fe_attr_put(rec, nd, "start_arrow", ( fl.arrows & 1 ) ? 1 : 0);
	fe_attr_put(rec, nd, "end_arrow", ( fl.arrows & 2 ) ? 1 : 0);
	(void)tad_set_attr(rec, nd, "arrow_type",
			   (CONST UB *)( fl.arrow_filled ? "filled" : "simple" ));
}

EXPORT void ft_frame_put( T_TAD *rec, T_TADNODE *nd, INT l, INT t, INT r, INT b )
{
	fe_attr_put(rec, nd, "frameLeft", l);
	fe_attr_put(rec, nd, "frameTop", t);
	fe_attr_put(rec, nd, "frameRight", r);
	fe_attr_put(rec, nd, "frameBottom", b);
}

LOCAL CONST char *conn_word( INT c )
{
	return ( c == TV_CONN_ELBOW ) ? "elbow"
	     : ( c == TV_CONN_CURVE ) ? "curve" : "straight";
}

/* "place,point" for a joined end, or nothing */
EXPORT void ft_join_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT place,
		     INT point )
{
	UB	buf[24];
	INT	at;

	buf[0] = 0;
	if ( place >= 0 ) {
		at = ft_put_num(buf, 0, place);
		buf[at++] = ',';
		(void)ft_put_num(buf, at, point);
	}
	(void)tad_set_attr(rec, nd, name, buf);
}

/*
 * A box of text put into the figure where it was drawn, and edited
 * where it stands at once: a <document> with where it stands and how it
 * is drawn, the face, size and colour the 書式 palette gives, and one
 * paragraph with nothing in it yet.
 */
EXPORT void ft_make_text( T_FTVIEW *v, INT l, INT t, INT r, INT b )
{
	LOCAL CONST char *CONST faces[] = { "sans-serif", "serif", "monospace" };
	T_TAD		*rec;
	T_TADNODE	*body, *doc, *e;
	UB		col[8];

	v->ed->host->before(v->ed);
	rec = v->ed->rec;
	body = tad_body(rec);
	if ( body == NULL ) {
		return;
	}
	doc = tad_elem_new(rec, "document", body, NULL, FALSE);
	if ( doc == NULL ) {
		return;
	}
	e = tad_elem_new(rec, "docView", doc, NULL, TRUE);
	if ( e != NULL ) {
		fe_attr_put(rec, e, "viewleft", l);
		fe_attr_put(rec, e, "viewtop", t);
		fe_attr_put(rec, e, "viewright", r);
		fe_attr_put(rec, e, "viewbottom", b);
	}
	e = tad_elem_new(rec, "docDraw", doc, NULL, TRUE);
	if ( e != NULL ) {
		fe_attr_put(rec, e, "drawleft", l);
		fe_attr_put(rec, e, "drawtop", t);
		fe_attr_put(rec, e, "drawright", r);
		fe_attr_put(rec, e, "drawbottom", b);
	}
	e = tad_elem_new(rec, "docScale", doc, NULL, TRUE);
	if ( e != NULL ) {
		fe_attr_put(rec, e, "hunit", -72);
		fe_attr_put(rec, e, "vunit", -72);
	}
	e = tad_elem_new(rec, "text", doc, NULL, TRUE);
	if ( e != NULL ) {
		fe_attr_put(rec, e, "lang", 0);
		fe_attr_put(rec, e, "bpat", 0);
		fe_attr_put(rec, e, "zIndex", top_z(v) + 1);
	}
	e = tad_elem_new(rec, "font", doc, NULL, TRUE);
	if ( e != NULL ) {
		fe_attr_put(rec, e, "size", fl.font_size);
		(void)tad_set_attr(rec, e, "face",
				   (CONST UB *)faces[fl.font_face % 3]);
		ft_colour_hex(fl.text_col, col);
		(void)tad_set_attr(rec, e, "color", col);
		if ( fl.bold ) {
			(void)tad_set_attr(rec, e, "weight", (CONST UB *)"700");
		}
		if ( fl.italic ) {
			(void)tad_set_attr(rec, e, "style", (CONST UB *)"italic");
		}
	}
	(void)tad_elem_new(rec, "p", doc, NULL, FALSE);
	fe_adopt(v->ed, doc);
	ft_select();
	v->host->text_begin(v, doc);
}

/*
 * The box a shape is drawn in, from where the hand went down to where
 * it is now: square when Shift is held, and doubled out about the
 * first place when the tool draws from the middle.
 */
LOCAL void drawn_box( INT *l, INT *t, INT *r, INT *b )
{
	INT	x0 = ft_x0, y0 = ft_y0, x1 = ft_x1, y1 = ft_y1;

	if ( ft_centre[ft_tool] && ft_centre_able(ft_tool) ) {
		x0 = 2 * ft_x0 - ft_x1;
		y0 = 2 * ft_y0 - ft_y1;
	}
	*l = ( x0 < x1 ) ? x0 : x1;
	*r = ( x0 < x1 ) ? x1 : x0;
	*t = ( y0 < y1 ) ? y0 : y1;
	*b = ( y0 < y1 ) ? y1 : y0;
}

/* The two ends a line or an arc is drawn between */
LOCAL void drawn_ends( INT *x0, INT *y0, INT *x1, INT *y1 )
{
	*x0 = ft_x0;  *y0 = ft_y0;
	*x1 = ft_x1;  *y1 = ft_y1;
	if ( ft_centre[ft_tool] && ft_centre_able(ft_tool) ) {
		*x0 = 2 * ft_x0 - ft_x1;
		*y0 = 2 * ft_y0 - ft_y1;
	}
}

/* A freehand curve that ends where it began is closed, and filled */
#define CLOSE_REACH	8

/* A shape made: on top of the others, and picked */
LOCAL void made( T_FTVIEW *v, T_TADNODE *nd )
{
	fe_attr_put(v->ed->rec, nd, "zIndex", top_z(v) + 1);
	fe_adopt(v->ed, nd);
}

/* The shape drawn, put into the record */
LOCAL void make_shape( T_FTVIEW *v )
{
	T_TAD		*rec;
	T_TADNODE	*body, *nd = NULL;
	INT		l, t, r, b, x0, y0, x1, y1;

	if ( tad_body(v->ed->rec) == NULL ) {
		return;
	}
	drawn_box(&l, &t, &r, &b);
	drawn_ends(&x0, &y0, &x1, &y1);
	if ( ft_tool != FT_TL_LINE && !by_corners(ft_tool) && ft_tool != FT_TL_CURVE
	  && ( r - l < 2 || b - t < 2 ) ) {
		return;				/* a press with no drag draws nothing */
	}
	if ( ft_tool == FT_TL_TEXT ) {
		ft_make_text(v, l, t, r, b);
		return;
	}
	if ( ft_tool == FT_TL_CANVAS ) {
		ft_canvas_new(v, l, t, r, b);
		return;
	}
	if ( ft_tool == FT_TL_LINE && x0 == x1 && y0 == y1 ) {
		return;
	}
	if ( ft_tool == FT_TL_CURVE && ft_npt < 2 ) {
		return;
	}
	if ( ft_tool == FT_TL_POLYGON && ft_npt < 3 ) {
		return;				/* fewer than three corners close round nothing */
	}
	if ( ft_tool == FT_TL_POLYLINE && ft_npt < 2 ) {
		return;				/* one corner is no line */
	}
	v->ed->host->before(v->ed);
	rec = v->ed->rec;
	body = tad_body(rec);
	if ( body == NULL ) {
		return;
	}
	switch ( ft_tool ) {
	case FT_TL_LINE: {
		T_DPPOINT	pt[2];
		INT		cp = 0;

		nd = tad_elem_new(rec, "line", body, NULL, TRUE);
		if ( nd == NULL ) {
			break;
		}
		look_put(rec, nd, FALSE);
		arrows_put(rec, nd);
		if ( ft_c0_place >= 0 ) cp |= 1;
		if ( ft_c1_place >= 0 ) cp |= 2;
		fe_attr_put(rec, nd, "conn_pat", ( cp == 1 ) ? 1 : ( cp == 2 ) ? 2
						 : ( cp == 3 ) ? 3 : 0);
		ft_join_put(rec, nd, "start_conn", ft_c0_place, ft_c0_point);
		ft_join_put(rec, nd, "end_conn", ft_c1_place, ft_c1_point);
		(void)tad_set_attr(rec, nd, "lineConnectionType",
				   (CONST UB *)conn_word(fl.conn));
		pt[0].x = x0;  pt[0].y = y0;
		pt[1].x = x1;  pt[1].y = y1;
		fe_points_put(rec, nd, pt, 2);
		break;
	}
	case FT_TL_CURVE:
		nd = tad_elem_new(rec, "curve", body, NULL, TRUE);
		if ( nd != NULL ) {
			INT	dx = ft_pts[0].x - ft_pts[ft_npt - 1].x;
			INT	dy = ft_pts[0].y - ft_pts[ft_npt - 1].y;
			BOOL	shut = (BOOL)( ft_npt >= 3
				       && dx * dx + dy * dy <= CLOSE_REACH * CLOSE_REACH );

			look_put(rec, nd, shut);
			fe_attr_put(rec, nd, "type", 0);
			fe_attr_put(rec, nd, "closed", shut ? 1 : 0);
			fe_attr_put(rec, nd, "start_arrow", 0);
			fe_attr_put(rec, nd, "end_arrow", 0);
			fe_attr_put(rec, nd, "rotation", 0);
			fe_points_put(rec, nd, ft_pts, ft_npt);
		}
		break;
	case FT_TL_POLYGON:
		/* always closed, however it was ended: the last corner joins the first */
		nd = tad_elem_new(rec, "polygon", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(rec, nd, TRUE);
			fe_attr_put(rec, nd, "cornerRadius", fl.corner);
			fe_attr_put(rec, nd, "rotation", 0);
			fe_points_put(rec, nd, ft_pts, ft_npt);
		}
		break;
	case FT_TL_POLYLINE:
		/* left open: the last corner is not joined to the first, and nothing is filled */
		nd = tad_elem_new(rec, "polyline", body, NULL, TRUE);
		if ( nd != NULL ) {
			fe_attr_put(rec, nd, "mode", 0);
			fe_attr_put(rec, nd, "lineType", fl.ltype);
			fe_attr_put(rec, nd, "lineWidth", fl.width);
			fe_attr_put(rec, nd, "l_pat", ft_pattern_for(rec, fl.stroke));
			fe_attr_put(rec, nd, "round", 0);
			arrows_put(rec, nd);
			fe_attr_put(rec, nd, "rotation", 0);
			fe_points_put(rec, nd, ft_pts, ft_npt);
		}
		break;
	case FT_TL_TRIANGLE:
		nd = tad_elem_new(rec, "polygon", body, NULL, TRUE);
		if ( nd != NULL ) {
			T_DPPOINT	pt[3];

			/* the right angle where the hand went down */
			pt[0].x = ft_x0;  pt[0].y = ft_y0;
			pt[1].x = ft_x0;  pt[1].y = ft_y1;
			pt[2].x = ft_x1;  pt[2].y = ft_y1;
			look_put(rec, nd, TRUE);
			fe_attr_put(rec, nd, "cornerRadius", 0);
			fe_attr_put(rec, nd, "rotation", 0);
			fe_points_put(rec, nd, pt, 3);
		}
		break;
	case FT_TL_ELLIPSE:
		nd = tad_elem_new(rec, "ellipse", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(rec, nd, TRUE);
			fe_attr_put(rec, nd, "angle", 0);
			fe_attr_put(rec, nd, "rotation", 0);
			ft_frame_put(rec, nd, l, t, r, b);
		}
		break;
	case FT_TL_ARC:
	case FT_TL_CHORD:
	case FT_TL_EARC:
		nd = tad_elem_new(rec, ( ft_tool == FT_TL_ARC ) ? "arc"
				       : ( ft_tool == FT_TL_CHORD ) ? "chord"
				       : "elliptical_arc", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(rec, nd, (BOOL)( ft_tool != FT_TL_EARC ));
			fe_attr_put(rec, nd, "angle", 0);
			ft_frame_put(rec, nd, l, t, r, b);
			/* its ends are towards where the drag began and ended */
			fe_attr_put(rec, nd, "startX", x0);
			fe_attr_put(rec, nd, "startY", y0);
			fe_attr_put(rec, nd, "endX", x1);
			fe_attr_put(rec, nd, "endY", y1);
			arrows_put(rec, nd);
		}
		break;
	default:
		nd = tad_elem_new(rec, "rect", body, NULL, TRUE);
		if ( nd != NULL ) {
			look_put(rec, nd, TRUE);
			fe_attr_put(rec, nd, "round", ( fl.corner > 0 ) ? 1 : 0);
			fe_attr_put(rec, nd, "cornerRadius", fl.corner);
			fe_attr_put(rec, nd, "figRH", fl.corner * 2);
			fe_attr_put(rec, nd, "figRV", fl.corner * 2);
			fe_attr_put(rec, nd, "angle", 0);
			fe_attr_put(rec, nd, "rotation", 0);
			fe_attr_put(rec, nd, "left", l);
			fe_attr_put(rec, nd, "top", t);
			fe_attr_put(rec, nd, "right", r);
			fe_attr_put(rec, nd, "bottom", b);
		}
		break;
	}
	if ( nd == NULL ) {
		return;
	}
	made(v, nd);
}

/* ---------------------------------------------------------------- showing it being drawn */

LOCAL void xor_line( CONST T_FTVIEW *v, INT ax, INT ay, INT bx, INT by )
{
	INT	x0, y0, x1, y1;

	ft_fig_to_win(v, ax, ay, &x0, &y0);
	ft_fig_to_win(v, bx, by, &x1, &y1);
	dp_line(v->gid, x0, y0, x1, y1, 0x00FFFFFFU);
}

/* A box's edge in inverted lines, its corners round when r is not 0 */
LOCAL void xor_box( CONST T_FTVIEW *v, CONST T_DPRECT *box, INT r )
{
#ifndef TV_USER
	T_DPPAT	pat;

	dp_pat_colour(&pat, 0x00FFFFFFU);
	if ( r > 0 ) {
		dp_frame_round(v->gid, box, r, r, 1, &pat);
	} else {
		dp_frame_rect_pat(v->gid, box, &pat, 1);
	}
#else
	/* a process draws the edge itself: its corners cut across where they are round */
	T_DPPOINT	p[8];
	INT		c = r / 2, k;

	if ( c > ( box->right - box->left ) / 2 ) c = ( box->right - box->left ) / 2;
	if ( c > ( box->bottom - box->top ) / 2 ) c = ( box->bottom - box->top ) / 2;
	if ( c <= 0 ) {
		(void)dp_frame_rect(v->gid, box, 0x00FFFFFFU, 1);
		return;
	}
	p[0].x = box->left + c;		p[0].y = box->top;
	p[1].x = box->right - 1 - c;	p[1].y = box->top;
	p[2].x = box->right - 1;	p[2].y = box->top + c;
	p[3].x = box->right - 1;	p[3].y = box->bottom - 1 - c;
	p[4].x = box->right - 1 - c;	p[4].y = box->bottom - 1;
	p[5].x = box->left + c;		p[5].y = box->bottom - 1;
	p[6].x = box->left;		p[6].y = box->bottom - 1 - c;
	p[7].x = box->left;		p[7].y = box->top + c;
	for ( k = 0; k < 8; k++ ) {
		(void)dp_line(v->gid, p[k].x, p[k].y, p[( k + 1 ) % 8].x, p[( k + 1 ) % 8].y, 0x00FFFFFFU);
	}
#endif
}

/*
 * The shape being drawn is shown in inverted lines over the figure as it
 * is, and taken off again by the same lines: a move of the hand inverts
 * the lines where it was and the lines where it is now, and only the box
 * round both is put on the screen (show_part); the figure is not drawn
 * again. What is inverted in the window is kept (ft_sh), so that whatever
 * the host draws of the window while the shape is drawn -- the whole of
 * it the first time, a part where the frames of what is picked turn --
 * ends with the lines put over it again (ft_drawing_over), and the window
 * always holds the figure with the lines inverted over it once.
 */
typedef struct {
	BOOL	on;
	void	*arg;			/* the view it is shown in */
	INT	x0, y0, x1, y1, npt;
} FTSHOWN;

LOCAL FTSHOWN	ft_sh;

/* The lines of the shape as the drawing now stands, inverted */
LOCAL void xor_drawn( CONST T_FTVIEW *v )
{
	T_DPRECT	box;
	T_DPPAT		pat;
	INT		k, l, t, r, b, x0, y0, x1, y1;

	drawn_box(&l, &t, &r, &b);
	drawn_ends(&x0, &y0, &x1, &y1);
	ft_fig_to_win(v, l, t, &box.left, &box.top);
	ft_fig_to_win(v, r, b, &box.right, &box.bottom);
	box.right++;
	box.bottom++;
	dp_pat_colour(&pat, 0x00FFFFFFU);
	(void)dp_set_mode(v->gid, DP_MODE_XOR);
	switch ( ft_tool ) {
	case FT_TL_LINE:
		xor_line(v, x0, y0, x1, y1);
		break;
	case FT_TL_CURVE:
		for ( k = 0; k + 1 < ft_npt; k++ ) {
			xor_line(v, ft_pts[k].x, ft_pts[k].y, ft_pts[k + 1].x, ft_pts[k + 1].y);
		}
		break;
	case FT_TL_POLYGON:
		for ( k = 0; k + 1 < ft_npt; k++ ) {
			xor_line(v, ft_pts[k].x, ft_pts[k].y, ft_pts[k + 1].x, ft_pts[k + 1].y);
		}
		if ( ft_npt > 0 ) {
			xor_line(v, ft_pts[ft_npt - 1].x, ft_pts[ft_npt - 1].y, ft_x1, ft_y1);
		}
		/*
		 * and back to the first corner, as it will be closed: from the
		 * pointer, or from the last corner when the pointer is on it.
		 * Not while the first side is all there is, where the line back
		 * is that side again and would invert it off.
		 */
		if ( ft_npt >= 2 && ( ft_x1 != ft_pts[0].x || ft_y1 != ft_pts[0].y ) ) {
			BOOL	at_last = (BOOL)( ft_x1 == ft_pts[ft_npt - 1].x
						&& ft_y1 == ft_pts[ft_npt - 1].y );

			if ( !at_last || ft_npt >= 3 ) {
				xor_line(v, ft_x1, ft_y1, ft_pts[0].x, ft_pts[0].y);
			}
		}
		break;
	case FT_TL_POLYLINE:
		/* the corners and the line on to the pointer; left open, so no line back to the first */
		for ( k = 0; k + 1 < ft_npt; k++ ) {
			xor_line(v, ft_pts[k].x, ft_pts[k].y, ft_pts[k + 1].x, ft_pts[k + 1].y);
		}
		if ( ft_npt > 0 ) {
			xor_line(v, ft_pts[ft_npt - 1].x, ft_pts[ft_npt - 1].y, ft_x1, ft_y1);
		}
		break;
	case FT_TL_TRIANGLE:
		xor_line(v, ft_x0, ft_y0, ft_x0, ft_y1);
		xor_line(v, ft_x0, ft_y1, ft_x1, ft_y1);
		xor_line(v, ft_x1, ft_y1, ft_x0, ft_y0);
		break;
	case FT_TL_ELLIPSE:
	case FT_TL_ARC:
	case FT_TL_CHORD:
	case FT_TL_EARC:
		(void)dp_frame_oval(v->gid, &box, 1, &pat);
		if ( ft_tool != FT_TL_ELLIPSE ) {
			/* and where its ends will be */
			INT	cx = ( l + r ) / 2, cy = ( t + b ) / 2;

			xor_line(v, cx, cy, x0, y0);
			xor_line(v, cx, cy, x1, y1);
		}
		break;
	case FT_TL_RECT:
		xor_box(v, &box, ( fl.corner > 0 ) ? fl.corner * 2 * fl.zoom / 8 : 0);
		break;
	default:
		xor_box(v, &box, 0);
		break;
	}
	(void)dp_set_mode(v->gid, DP_MODE_COPY);
}

/* The box round the lines as the drawing now stands, with two round it for their width */
LOCAL void drawn_win_box( CONST T_FTVIEW *v, T_DPRECT *r )
{
	INT	k, l, t, rt, b;

	drawn_box(&l, &t, &rt, &b);
	if ( ft_tool == FT_TL_CURVE || by_corners(ft_tool) ) {
		for ( k = 0; k < ft_npt; k++ ) {
			if ( ft_pts[k].x < l ) l = ft_pts[k].x;
			if ( ft_pts[k].x > rt ) rt = ft_pts[k].x;
			if ( ft_pts[k].y < t ) t = ft_pts[k].y;
			if ( ft_pts[k].y > b ) b = ft_pts[k].y;
		}
	}
	ft_fig_to_win(v, l, t, &r->left, &r->top);
	ft_fig_to_win(v, rt, b, &r->right, &r->bottom);
	r->left -= 2;
	r->top -= 2;
	r->right += 3;
	r->bottom += 3;
}

/* What is inverted in the window, as it stood when it was: its lines inverted again, and their box */
LOCAL void xor_shown( CONST T_FTVIEW *v, T_DPRECT *r )
{
	INT	x0 = ft_x0, y0 = ft_y0, x1 = ft_x1, y1 = ft_y1, npt = ft_npt;

	/* the corners only grow in number while a shape is drawn: those it had are still there */
	ft_x0 = ft_sh.x0;  ft_y0 = ft_sh.y0;
	ft_x1 = ft_sh.x1;  ft_y1 = ft_sh.y1;
	ft_npt = ft_sh.npt;
	xor_drawn(v);
	if ( r != NULL ) {
		drawn_win_box(v, r);
	}
	ft_x0 = x0;  ft_y0 = y0;
	ft_x1 = x1;  ft_y1 = y1;
	ft_npt = npt;
}

LOCAL void shown_take( CONST T_FTVIEW *v )
{
	ft_sh.on = TRUE;
	ft_sh.arg = v->arg;
	ft_sh.x0 = ft_x0;  ft_sh.y0 = ft_y0;
	ft_sh.x1 = ft_x1;  ft_sh.y1 = ft_y1;
	ft_sh.npt = ft_npt;
}

EXPORT void ft_drawing_over( CONST T_FTVIEW *v )
{
	if ( ft_sh.on && ft_drawing != NULL && ft_drawing == v->arg && ft_sh.arg == v->arg && v->gid >= 0 ) {
		xor_shown(v, NULL);
	}
}

/* The shape being drawn shown as it now stands */
LOCAL void preview( T_FTVIEW *v )
{
	T_DPRECT	was, now;

	if ( v->gid >= 0 && v->host->show_part != NULL
	  && ft_sh.on && ft_sh.arg == v->arg && ft_drawing == v->arg ) {
		xor_shown(v, &was);		/* taken off where it was */
		shown_take(v);
		xor_shown(v, &now);		/* and put where it is */
		if ( now.left > was.left ) now.left = was.left;
		if ( now.top > was.top ) now.top = was.top;
		if ( now.right < was.right ) now.right = was.right;
		if ( now.bottom < was.bottom ) now.bottom = was.bottom;
		v->host->show_part(v, &now);
		return;
	}
	/* the first time: the window drawn whole, the lines put over it as it ends (ft_drawing_over) */
	shown_take(v);
	v->host->draw(v);
	v->host->show(v);
}

/* The pointer's place shown in the panel */
LOCAL void pointer_at( T_FTVIEW *v, INT fx, INT fy )
{
	ft_have_xy = TRUE;
	ft_px = fx;
	ft_py = fy;
	if ( v->host->tools_shown != NULL ) {
		v->host->tools_shown();
	}
}

/* The words of where the pointer is */
EXPORT void ft_xy_words( UB *txt )
{
	UB	num[16];
	INT	k = 0, j;

	if ( !ft_have_xy ) {
		txt[0] = ' ';
		txt[1] = 0;
		return;
	}
	txt[k++] = 'x';  txt[k++] = ':';
	(void)ft_put_num(num, 0, ft_px);
	for ( j = 0; num[j] != 0 && k < 14; j++ ) txt[k++] = num[j];
	txt[k++] = ' ';  txt[k++] = 'y';  txt[k++] = ':';
	(void)ft_put_num(num, 0, ft_py);
	for ( j = 0; num[j] != 0 && k < 30; j++ ) txt[k++] = num[j];
	txt[k] = 0;
}

/* ---------------------------------------------------------------- the pointer in a figure */

/*
 * A shape drawn corner by corner ended: a polygon closed whichever way
 * it was ended, put into the record when it has three corners or more;
 * a polyline left open, put in when it has two or more. The window is
 * drawn again without the inverted lines, which are no longer kept
 * over it.
 */
LOCAL void corners_end( T_FTVIEW *v )
{
	ft_drawing = NULL;
	make_shape(v);
	ft_npt = 0;
	v->host->draw(v);
	v->host->show(v);
}

/*
 * A press in a figure with a drawing tool in hand: the start of a
 * shape. A polygon's or a polyline's first side is dragged; after it,
 * each press is a corner. A double press, Enter or Escape ends either;
 * a press near the first corner ends a polygon as well, always closed,
 * while a polyline takes it as one more corner and is left open.
 */
EXPORT BOOL ft_press( T_FTVIEW *v, INT x, INT y, UD when, UINT mods )
{
	INT	fx, fy, jx, jy;
	UD	gap = v->host->dbl_ns();

	if ( !ft_is(v) || ft_tool == FT_TL_SELECT ) {
		return FALSE;
	}
	ft_to_fig(v, x, y, &fx, &fy);
	if ( ft_tool != FT_TL_CURVE ) {
		ft_snap(&fx, &fy);
	}
	if ( ft_tool == FT_TL_CANVAS && ft_canvas_press(v, fx, fy) ) {
		return TRUE;
	}
	pointer_at(v, fx, fy);
	if ( by_corners(ft_tool) ) {
		if ( ft_drawing == v->arg && ft_npt > 0 ) {
			INT	dx = fx - ft_pts[0].x, dy = fy - ft_pts[0].y;

			if ( when - ft_last_at < gap
			  || ( ft_tool == FT_TL_POLYGON && ft_npt >= 3
			    && dx * dx + dy * dy <= CLOSE_REACH * CLOSE_REACH ) ) {
				/* a double press, or a polygon back at its first corner: ended */
				corners_end(v);
				return TRUE;
			}
			if ( ( mods & MOD_SHIFT ) != 0 ) {
				/* across or up and down from the last corner */
				INT	ax = fx - ft_pts[ft_npt - 1].x;
				INT	ay = fy - ft_pts[ft_npt - 1].y;

				if ( ( ax < 0 ? -ax : ax ) >= ( ay < 0 ? -ay : ay ) ) {
					fy = ft_pts[ft_npt - 1].y;
				} else {
					fx = ft_pts[ft_npt - 1].x;
				}
			}
			if ( ( ft_pts[ft_npt - 1].x != fx || ft_pts[ft_npt - 1].y != fy )
			  && pts_room(v, ft_npt + 1) ) {
				ft_pts[ft_npt].x = fx;
				ft_pts[ft_npt].y = fy;
				ft_npt++;
			}
			ft_x1 = fx;
			ft_y1 = fy;
			ft_last_at = when;
			ft_first_edge = FALSE;
			preview(v);
			return TRUE;
		}
		ft_told = FALSE;
		if ( !pts_room(v, 2) ) {
			return TRUE;
		}
		ft_drawing = v->arg;
		ft_sh.on = FALSE;
		ft_npt = 1;
		ft_pts[0].x = fx;
		ft_pts[0].y = fy;
		ft_x0 = ft_x1 = fx;
		ft_y0 = ft_y1 = fy;
		ft_last_at = when;
		ft_first_edge = TRUE;
		preview(v);
		return TRUE;
	}
	ft_told = FALSE;
	if ( !pts_room(v, 2) ) {
		return TRUE;
	}
	ft_drawing = v->arg;
	ft_sh.on = FALSE;
	ft_c0_place = ft_c1_place = -1;
	if ( ft_tool == FT_TL_LINE
	  && ft_join_at(v, fx, fy, &ft_c0_place, &ft_c0_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	}
	ft_x0 = ft_x1 = fx;
	ft_y0 = ft_y1 = fy;
	ft_npt = 0;
	if ( ft_tool == FT_TL_CURVE ) {
		ft_pts[0].x = fx;
		ft_pts[0].y = fy;
		ft_npt = 1;
	}
	preview(v);

	return TRUE;
}

EXPORT void *ft_drawing_arg( void )
{
	return ft_drawing;
}

EXPORT void ft_drawing_stop( void )
{
	ft_drawing = NULL;
	ft_npt = 0;
}

/* The hand moved while a shape is drawn, to (x, y) of the window */
EXPORT void ft_follow( T_FTVIEW *v, INT x, INT y, UINT mods )
{
	INT	fx, fy, jx, jy;

	if ( ft_drawing == NULL || ft_drawing != v->arg ) {
		return;
	}
	ft_to_fig(v, x, y, &fx, &fy);
	if ( ft_tool == FT_TL_CURVE ) {
		if ( ( ft_pts[ft_npt - 1].x != fx || ft_pts[ft_npt - 1].y != fy )
		  && pts_room(v, ft_npt + 1) ) {
			ft_pts[ft_npt].x = fx;
			ft_pts[ft_npt].y = fy;
			ft_npt++;
		}
		ft_x1 = fx;
		ft_y1 = fy;
		pointer_at(v, fx, fy);
		preview(v);
		return;
	}
	ft_c1_place = -1;
	if ( ft_tool == FT_TL_LINE
	  && ft_join_at(v, fx, fy, &ft_c1_place, &ft_c1_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	} else {
		ft_snap(&fx, &fy);
	}
	if ( ( mods & MOD_SHIFT ) != 0 ) {
		INT	dx = fx - ft_x0, dy = fy - ft_y0;
		INT	ax = ( dx < 0 ) ? -dx : dx, ay = ( dy < 0 ) ? -dy : dy;

		if ( ft_tool == FT_TL_LINE ) {
			if ( ax >= ay ) fy = ft_y0; else fx = ft_x0;
		} else if ( !by_corners(ft_tool) ) {
			/* square, and a circle, the larger way */
			INT	m = ( ax > ay ) ? ax : ay;

			fx = ft_x0 + ( ( dx < 0 ) ? -m : m );
			fy = ft_y0 + ( ( dy < 0 ) ? -m : m );
		}
	}
	ft_x1 = fx;
	ft_y1 = fy;
	pointer_at(v, fx, fy);
	preview(v);
}

EXPORT void ft_release( T_FTVIEW *v )
{
	if ( ft_drawing == NULL || ft_drawing != v->arg ) {
		return;
	}
	if ( by_corners(ft_tool) ) {
		/* the first side's far end is the second corner */
		if ( ft_first_edge && ft_npt == 1
		  && ( ft_x1 != ft_pts[0].x || ft_y1 != ft_pts[0].y ) ) {
			ft_pts[1].x = ft_x1;
			ft_pts[1].y = ft_y1;
			ft_npt = 2;
		}
		ft_first_edge = FALSE;
		preview(v);
		return;				/* the corners go on */
	}
	ft_drawing = NULL;
	make_shape(v);
	v->host->draw(v);
	v->host->show(v);
}

/*
 * Enter or Escape ends a polygon, closed, or a polyline, open; Escape
 * abandons any other shape being drawn
 */
EXPORT BOOL ft_key( T_FTVIEW *v, UINT code )
{
	if ( ft_drawing == NULL || ft_drawing != v->arg ) {
		return FALSE;
	}
	if ( ( code == KEY_ENTER || code == KEY_ESC ) && by_corners(ft_tool) ) {
		corners_end(v);
		return TRUE;
	}
	if ( code == KEY_ENTER || code == KEY_ESC ) {
		ft_drawing = NULL;
		ft_npt = 0;
		v->host->draw(v);
		v->host->show(v);
		return TRUE;
	}

	return FALSE;
}

/* The pointer over a figure, with nothing held: where it is, for the panel */
EXPORT void ft_hover( T_FTVIEW *v, INT x, INT y )
{
	INT	fx, fy;

	if ( !ft_is(v) ) {
		return;
	}
	if ( x < v->page.left || y < v->page.top || x >= v->page.right || y >= v->page.bottom ) {
		return;				/* not over the figure */
	}
	ft_to_fig(v, x, y, &fx, &fy);
	if ( !ft_have_xy || fx != ft_px || fy != ft_py ) {
		pointer_at(v, fx, fy);
	}
}

/* The tool in hand given back to the pointer */
EXPORT void ft_select( void )
{
	ft_tool = FT_TL_SELECT;
	ft_drawing = NULL;
	ft_npt = 0;
}

/* Tool t taken from the panel; taken again, one that can draws from the middle, or not */
EXPORT void ft_take( INT t )
{
	if ( t == ft_tool && ft_centre_able(t) ) {
		ft_centre[t] = (BOOL)!ft_centre[t];
	} else {
		ft_tool = t;
	}
	ft_drawing = NULL;
	ft_npt = 0;
}

/* ---------------------------------------------------------------- the grid */

EXPORT void ft_grid_draw( CONST T_FTVIEW *v )
{
	INT	g = fl.grid, fx, fy, x, y;
	INT	fw, fh;

	if ( !ft_is(v) || fl.grid_mode == FT_GRID_NONE || g < 2 || v->gid < 0 ) {
		return;
	}
	fw = ( v->page.right - v->page.left + v->sx ) * 8 / fl.zoom;
	fh = ( v->page.bottom - v->page.top + v->sy ) * 8 / fl.zoom;
	for ( fy = ( v->sy * 8 / fl.zoom / g ) * g; fy <= fh; fy += g ) {
		for ( fx = ( v->sx * 8 / fl.zoom / g ) * g; fx <= fw; fx += g ) {
			ft_fig_to_win(v, fx, fy, &x, &y);
			if ( x >= v->page.left && y >= v->page.top ) {
				(void)dp_put_pixel(v->gid, x, y, 0x00808080U);
			}
		}
	}
}

/* ---------------------------------------------------------------- the picked shapes */

/* A leaf shape of the picked ones, one element each */
LOCAL INT picked_nodes( CONST T_FGED *e, T_TADNODE **out, UINT *kinds, INT max )
{
	INT	i, n = 0;

	for ( i = 0; e->fig != NULL && i < e->fig->nsh && n < max; i++ ) {
		CONST T_TVSHAPE	*s = &e->fig->sh[i];

		if ( e->pick[i] == 0 || s->node == NULL || s->kind == TV_SH_LINK
		  || s->kind == TV_SH_DOC || s->kind == TV_SH_GROUP ) {
			continue;
		}
		out[n] = s->node;
		kinds[n] = s->kind;
		n++;
	}

	return n;
}

/*
 * What a palette sets, put on the shapes that are picked as well: each
 * gets it where it has such a thing -- a line has no inside, and only
 * a rectangle or a polygon has corners.
 */
EXPORT void ft_apply( T_FTVIEW *v, INT what )
{
	T_TADNODE	**nodes;
	UINT		*kinds;
	T_TAD		*rec;
	INT		n, i, np;

	if ( !ft_is(v) || v->ed->npick == 0 ) {
		return;
	}
	/* every shape picked: no more than are picked */
	np = v->ed->npick;
	nodes = (T_TADNODE **)Kmalloc(( sizeof(T_TADNODE *) + sizeof(UINT) ) * (size_t)np);
	if ( nodes == NULL ) {
		v->ed->host->no_memory(v->ed);
		return;
	}
	kinds = (UINT *)( nodes + np );
	n = picked_nodes(v->ed, nodes, kinds, np);
	if ( n == 0 ) {
		Kfree(nodes);
		return;
	}
	v->ed->host->before(v->ed);
	rec = v->ed->rec;
	for ( i = 0; i < n; i++ ) {
		T_TADNODE	*nd = nodes[i];
		UINT		k = kinds[i];
		BOOL		has_inside = (BOOL)( k == TV_SH_RECT || k == TV_SH_ELLIPSE
					|| k == TV_SH_POLY || k == TV_SH_ARC
					|| k == TV_SH_CHORD || k == TV_SH_CURVE );

		switch ( what ) {
		case FT_APPLY_FILL:
			if ( has_inside ) {
				fe_attr_put(rec, nd, "f_pat", ft_fill_pattern(rec));
			}
			break;
		case FT_APPLY_STROKE:
			fe_attr_put(rec, nd, "l_pat", ft_pattern_for(rec, fl.stroke));
			break;
		case FT_APPLY_WIDTH:
			fe_attr_put(rec, nd, "lineWidth", fl.width);
			break;
		case FT_APPLY_LTYPE:
			fe_attr_put(rec, nd, "lineType", fl.ltype);
			break;
		case FT_APPLY_CONN:
			if ( ft_same_word(nd->name, "line") ) {
				(void)tad_set_attr(rec, nd, "lineConnectionType",
						   (CONST UB *)conn_word(fl.conn));
			}
			break;
		case FT_APPLY_CORNER:
			if ( ft_same_word(nd->name, "rect") ) {
				fe_attr_put(rec, nd, "round", ( fl.corner > 0 ) ? 1 : 0);
				fe_attr_put(rec, nd, "cornerRadius", fl.corner);
				fe_attr_put(rec, nd, "figRH", fl.corner * 2);
				fe_attr_put(rec, nd, "figRV", fl.corner * 2);
			} else if ( ft_same_word(nd->name, "polygon") ) {
				fe_attr_put(rec, nd, "cornerRadius", fl.corner);
			}
			break;
		case FT_APPLY_ARROWS:
			if ( k == TV_SH_LINE || k == TV_SH_EARC ) {
				arrows_put(rec, nd);
			}
			break;
		default:
			break;
		}
	}
	Kfree(nodes);
	v->ed->host->changed(v->ed);
}

/*
 * The palettes follow the shape picked: what it is drawn with is what
 * the next shape will be, until something else is chosen.
 */
EXPORT BOOL ft_take_look( CONST T_FGED *e )
{
	INT	i;

	for ( i = 0; e != NULL && e->fig != NULL && i < e->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &e->fig->sh[i];

		if ( e->pick[i] == 0 || s->kind == TV_SH_LINK || s->kind == TV_SH_DOC
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
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the palettes: laying out */

#define CHOICE_PAD	30		/* a choice's lamp and its margins */

EXPORT INT ft_str_w( CONST char *s, INT size )
{
	ID	fid = fn_system();

	if ( fid <= 0 ) {
		return 0;
	}
	(void)fn_set_size(fid, size);

	return fn_width(fid, (CONST UB *)s);
}

/* A row of choices laid across: each name's cell, as many rows as they take */
EXPORT INT ft_choice_at( CONST T_FTITEM *it, INT k, T_DPRECT *c )
{
	INT	x = FT_LABEL_W, row = 0, j;

	for ( j = 0; j <= k && j < it->n; j++ ) {
		INT	w = ft_str_w(it->names[j], FT_PART_FH) + CHOICE_PAD;

		if ( x + w > FT_POP_W - 8 && x > FT_LABEL_W ) {
			row++;
			x = FT_LABEL_W;
		}
		if ( j == k ) {
			c->left = x;
			c->top = it->r.top + row * FT_ROW_H + 1;
			c->right = x + w;
			c->bottom = c->top + FT_ROW_H - 4;
		}
		x += w + 2;
	}
	return row + 1;
}

EXPORT INT ft_item_h( CONST T_FTITEM *it )
{
	T_DPRECT	c;

	if ( it->type == FT_PI_GRID ) {
		return 8 * 17 + 4;
	}
	if ( it->type == FT_PI_CHOICE && it->n > 0 ) {
		return ft_choice_at(it, it->n - 1, &c) * FT_ROW_H;
	}
	return FT_ROW_H;
}

EXPORT INT ft_pop_height( CONST T_FTPOP *p )
{
	INT	i, h = 12;

	for ( i = 0; i < p->n; i++ ) {
		h += ft_item_h(&p->it[i]) + 4;
	}

	return h;
}

/* Each item's rectangle, row by row */
EXPORT void ft_pop_layout( T_FTPOP *p )
{
	INT	i, y = 6;

	for ( i = 0; i < p->n; i++ ) {
		T_FTITEM	*it = &p->it[i];
		INT		h = ft_item_h(it);

		it->r.left = 6;  it->r.top = y;
		it->r.right = FT_POP_W - 6;  it->r.bottom = y + h;
		y += h + 4;
	}
}

/* ---------------------------------------------------------------- the palettes: the fixed patterns */

/* Cell k of the patterns of item it */
LOCAL void grid_cell( CONST T_FTITEM *it, INT k, T_DPRECT *c )
{
	c->left = 8 + ( k % 16 ) * 17;
	c->top = it->r.top + 2 + ( k / 16 ) * 17;
	c->right = c->left + 16;
	c->bottom = c->top + 16;
}

/* A cell filled with fixed pattern pat_no, or with col when it has none */
LOCAL void fill_pat_rect( INT gid, CONST T_DPRECT *r, INT pat_no, UW col )
{
	UW		pc;
	CONST UH	*tex = NULL;
	T_DPPAT		pat;
	T_TVTEX		room;
	T_DPPOINT	q[4];

	if ( pat_no > 0 && tv_pattern(pat_no, &pc, &tex) && tex != NULL ) {
		(void)dp_fill_rect(gid, r, 0x00FFFFFFU);
		tv_tex_pat(&pat, &room, pc, tex);
		q[0].x = r->left;   q[0].y = r->top;
		q[1].x = r->right;  q[1].y = r->top;
		q[2].x = r->right;  q[2].y = r->bottom;
		q[3].x = r->left;   q[3].y = r->bottom;
		(void)dp_fill_poly(gid, q, 4, DP_POLY_ODD, &pat);
		return;
	}
	if ( pat_no > 0 && tv_pattern(pat_no, &pc, &tex) ) {
		col = pc;
	}
	(void)dp_fill_rect(gid, r, col);
}

/* The patterns of an FT_PI_GRID, drawn over the palette's ground: what is chosen among, not parts */
EXPORT void ft_pop_grid_draw( INT gid, CONST T_FTPOP *p )
{
	INT		i, k;
	T_DPRECT	c;

	for ( i = 0; gid >= 0 && i < p->n; i++ ) {
		CONST T_FTITEM	*it = &p->it[i];

		if ( it->type != FT_PI_GRID ) {
			continue;
		}
		for ( k = 0; k < TV_PAT_FIXED; k++ ) {
			grid_cell(it, k, &c);
			if ( k == 0 ) {
				(void)dp_fill_rect(gid, &c, fl.fill);
			} else {
				fill_pat_rect(gid, &c, k, 0);
			}
			(void)dp_frame_rect(gid, &c, 0x00999999U, 1);
			if ( *it->value == k ) {
				T_DPRECT	o = c;

				o.left--;  o.top--;  o.right++;  o.bottom++;
				(void)dp_frame_rect(gid, &o, 0x000078D7U, 2);
			}
		}
	}
}

/* A press on the patterns: the one under it chosen; the item, or -1 */
EXPORT INT ft_pop_grid_press( T_FTPOP *p, INT x, INT y )
{
	INT	i, k;

	for ( i = 0; i < p->n; i++ ) {
		T_FTITEM	*it = &p->it[i];

		if ( it->type != FT_PI_GRID || y < it->r.top || y >= it->r.bottom ) {
			continue;
		}
		k = ( ( y - it->r.top - 2 ) / 17 ) * 16 + ( x - 8 ) / 17;
		if ( x >= 8 && x < 8 + 16 * 17 && k >= 0 && k < TV_PAT_FIXED ) {
			*it->value = k;
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- the palettes: what they offer */

LOCAL T_FTVIEW	*pal_view = NULL;	/* whose picked shapes they change */

EXPORT void ft_pal_on( T_FTVIEW *v )
{
	pal_view = v;
}

EXPORT T_FTVIEW *ft_pal_view( void )
{
	return pal_view;
}

LOCAL void apply( INT what )
{
	if ( pal_view != NULL ) {
		ft_apply(pal_view, what);
	}
}

/* 描画パレット */

LOCAL UB	pop_hex[16];
LOCAL INT	pop_on, pop_pat;

LOCAL void fill_changed( INT item )
{
	UW	c;

	switch ( item ) {
	case 0:				/* the colour typed */
		if ( ft_hex_colour(pop_hex, &c) ) {
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
	apply(FT_APPLY_FILL);
}

LOCAL void fill_palette( T_FTPOP *p )
{
	ft_colour_hex(fl.fill, pop_hex);
	pop_on = fl.fill_on ? 1 : 0;
	pop_pat = fl.fill_pat;
	p->it[0].type = FT_PI_COLOUR;  p->it[0].label = "塗りつぶし色:";
	p->it[0].text = pop_hex;  p->it[0].max = 8;
	p->it[1].type = FT_PI_CHECK;  p->it[1].label = "塗りつぶし有効";
	p->it[1].value = &pop_on;
	p->it[2].type = FT_PI_GRID;  p->it[2].label = NULL;
	p->it[2].value = &pop_pat;
	p->it[3].type = FT_PI_BUTTON;  p->it[3].label = "パターン編集...";
	p->n = 4;
	p->changed = fill_changed;
}

/* 線パレット */

LOCAL CONST UW	ft_line_cols[] = {
	0x000000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xEE0000, 0xFF8C00, 0xFFFF00,
	0x008000, 0x7FFF00, 0x00FFFF, 0x0000FF, 0x800080, 0xFF69B4
};
LOCAL UW	pop_col;

LOCAL void stroke_changed( INT item )
{
	UW	c;

	if ( item == 0 ) {
		if ( !ft_hex_colour(pop_hex, &c) ) {
			return;
		}
		fl.stroke = c;
		pop_col = c;
	} else {
		fl.stroke = pop_col;
		ft_colour_hex(pop_col, pop_hex);
	}
	apply(FT_APPLY_STROKE);
}

LOCAL void stroke_palette( T_FTPOP *p )
{
	ft_colour_hex(fl.stroke, pop_hex);
	pop_col = fl.stroke;
	p->it[0].type = FT_PI_COLOUR;  p->it[0].label = "線色:";
	p->it[0].text = pop_hex;  p->it[0].max = 8;
	p->it[1].type = FT_PI_SWATCH;  p->it[1].label = "";
	p->it[1].cols = ft_line_cols;
	p->it[1].n = (INT)( sizeof(ft_line_cols) / sizeof(ft_line_cols[0]) );
	p->it[1].col = &pop_col;
	p->n = 2;
	p->changed = stroke_changed;
}

/* 線種パレット */

LOCAL CONST INT	ft_widths[] = { 1, 2, 3, 5, 10 };
LOCAL INT	pop_w, pop_t, pop_c, pop_ap, pop_at;

LOCAL void lstyle_changed( INT item )
{
	switch ( item ) {
	case 0:
		fl.width = ft_widths[pop_w];
		apply(FT_APPLY_WIDTH);
		break;
	case 1:
		fl.ltype = pop_t;
		apply(FT_APPLY_LTYPE);
		break;
	case 2:
		fl.conn = pop_c;
		apply(FT_APPLY_CONN);
		break;
	default:
		fl.arrows = pop_ap;
		fl.arrow_filled = (BOOL)( pop_at == 1 );
		apply(FT_APPLY_ARROWS);
		break;
	}
}

LOCAL void lstyle_palette( T_FTPOP *p )
{
	LOCAL CONST char *CONST wn[] = { "1px", "2px", "3px", "5px", "10px" };
	LOCAL CONST char *CONST tn[] = {
		"実線", "破線", "点線", "一点鎖線", "二点鎖線", "長破線"
	};
	LOCAL CONST char *CONST cn[] = { "直線", "カギ線", "曲線" };
	LOCAL CONST char *CONST an[] = { "なし", "始点のみ", "終点のみ", "両端" };
	LOCAL CONST char *CONST kn[] = { "線のみ", "塗りつぶし" };
	INT	k;

	pop_w = 1;
	for ( k = 0; k < 5; k++ ) {
		if ( ft_widths[k] == fl.width ) {
			pop_w = k;
		}
	}
	pop_t = fl.ltype;
	pop_c = fl.conn;
	pop_ap = fl.arrows;
	pop_at = fl.arrow_filled ? 1 : 0;
	p->it[0].type = FT_PI_CHOICE;  p->it[0].label = "線の太さ:";
	p->it[0].names = wn;  p->it[0].n = 5;  p->it[0].value = &pop_w;
	p->it[1].type = FT_PI_CHOICE;  p->it[1].label = "線種:";
	p->it[1].names = tn;  p->it[1].n = 6;  p->it[1].value = &pop_t;
	p->it[2].type = FT_PI_CHOICE;  p->it[2].label = "接続形状:";
	p->it[2].names = cn;  p->it[2].n = 3;  p->it[2].value = &pop_c;
	p->it[3].type = FT_PI_CHOICE;  p->it[3].label = "矢印位置:";
	p->it[3].names = an;  p->it[3].n = 4;  p->it[3].value = &pop_ap;
	p->it[4].type = FT_PI_CHOICE;  p->it[4].label = "矢印種類:";
	p->it[4].names = kn;  p->it[4].n = 2;  p->it[4].value = &pop_at;
	p->n = 5;
	p->changed = lstyle_changed;
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
	apply(FT_APPLY_CORNER);
}

LOCAL void corner_palette( T_FTPOP *p )
{
	(void)ft_put_num(pop_num, 0, fl.corner);
	p->it[0].type = FT_PI_TEXT;  p->it[0].label = "角丸半径:";
	p->it[0].text = pop_num;  p->it[0].max = 4;
	p->n = 1;
	p->changed = corner_changed;
}

/* 倍率 */

LOCAL CONST INT	ft_zooms[] = { 1, 2, 4, 8, 16, 24, 32, 40, 64 };
LOCAL INT	pop_z;

LOCAL void zoom_changed( INT item )
{
	(void)item;
	fl.zoom = ft_zooms[pop_z];
	if ( pal_view != NULL && pal_view->host->zoom_set != NULL ) {
		pal_view->host->zoom_set(pal_view);
	}
}

LOCAL void zoom_palette( T_FTPOP *p )
{
	LOCAL CONST char *CONST zn[] = {
		"x1/8", "x1/4", "x1/2", "x1", "x2", "x3", "x4", "x5", "x8"
	};
	INT	k;

	pop_z = 3;
	for ( k = 0; k < 9; k++ ) {
		if ( ft_zooms[k] == fl.zoom ) {
			pop_z = k;
		}
	}
	p->it[0].type = FT_PI_CHOICE;  p->it[0].label = "倍率:";
	p->it[0].names = zn;  p->it[0].n = 9;  p->it[0].value = &pop_z;
	p->n = 1;
	p->changed = zoom_changed;
}

/* 格子点 */

LOCAL CONST INT	ft_grids[] = { 4, 8, 16, 32, 64, 5, 10, 20, 25, 40, 50 };
LOCAL INT	pop_gm, pop_gi;

LOCAL void grid_take( INT item )
{
	INT	v;

	switch ( item ) {
	case 0:
		fl.grid_mode = pop_gm;
		break;
	case 1:
		fl.grid = ft_grids[pop_gi];
		(void)ft_put_num(pop_num, 0, fl.grid);
		break;
	default:
		v = word_int(pop_num, -1);
		if ( v >= 1 && v <= 200 ) {
			fl.grid = v;
		}
		break;
	}
}

/*
 * The grid's points are drawn over the whole figure, not over a shape:
 * shown or taken off, or shown at another spacing, the figure is drawn
 * again whole at once. A part drawn again alone would show the grid
 * there and leave the rest as it was.
 */
LOCAL void grid_changed( INT item )
{
	BOOL	was_shown = (BOOL)( fl.grid_mode != FT_GRID_NONE );
	INT	was_grid = fl.grid;

	grid_take(item);
	if ( pal_view == NULL || !ft_is(pal_view) ) {
		return;
	}
	if ( was_shown != (BOOL)( fl.grid_mode != FT_GRID_NONE )
	  || ( was_shown && was_grid != fl.grid ) ) {
		pal_view->host->draw(pal_view);
		pal_view->host->show(pal_view);
	}
}

LOCAL void grid_palette( T_FTPOP *p )
{
	LOCAL CONST char *CONST mn[] = { "なし", "表示のみ", "格子点拘束" };
	LOCAL CONST char *CONST gn[] = {
		"4", "8", "16", "32", "64", "5", "10", "20", "25", "40", "50"
	};
	INT	k;

	pop_gm = fl.grid_mode;
	pop_gi = -1;
	for ( k = 0; k < 11; k++ ) {
		if ( ft_grids[k] == fl.grid ) {
			pop_gi = k;
		}
	}
	(void)ft_put_num(pop_num, 0, fl.grid);
	p->it[0].type = FT_PI_CHOICE;  p->it[0].label = "表示モード:";
	p->it[0].names = mn;  p->it[0].n = 3;  p->it[0].value = &pop_gm;
	p->it[1].type = FT_PI_CHOICE;  p->it[1].label = "間隔 (px):";
	p->it[1].names = gn;  p->it[1].n = 11;  p->it[1].value = &pop_gi;
	p->it[2].type = FT_PI_TEXT;  p->it[2].label = "自由入力:";
	p->it[2].text = pop_num;  p->it[2].max = 4;
	p->n = 3;
	p->changed = grid_changed;
}

/* 書式: the look of the text of a new box of text */

LOCAL CONST INT	ft_sizes[] = { 10, 12, 14, 16, 18, 20, 24, 28, 32, 36 };
LOCAL CONST UW	ft_text_cols[] = {
	0x000000, 0x0000FF, 0xEE0000, 0xFF69B4, 0xFF8C00, 0x008000, 0x7FFF00,
	0x00FFFF, 0xFFFF00, 0xFFFFFF
};
LOCAL INT	pop_fs, pop_ff, pop_b, pop_i, pop_u, pop_s;

LOCAL void font_changed( INT item )
{
	UW	c;

	switch ( item ) {
	case 0:	fl.font_size = ft_sizes[pop_fs];	break;
	case 1:	fl.font_face = pop_ff;			break;
	case 2:
		if ( ft_hex_colour(pop_hex, &c) ) {
			fl.text_col = c;
			pop_col = c;
		}
		break;
	case 3:
		fl.text_col = pop_col;
		ft_colour_hex(pop_col, pop_hex);
		break;
	case 4:	fl.bold = (BOOL)pop_b;			break;
	case 5:	fl.italic = (BOOL)pop_i;		break;
	case 6:	fl.under = (BOOL)pop_u;			break;
	default: fl.strike = (BOOL)pop_s;		break;
	}
	if ( pal_view != NULL && pal_view->host->text_look != NULL ) {
		pal_view->host->text_look(pal_view, item);
	}
}

LOCAL void font_palette( T_FTPOP *p )
{
	LOCAL CONST char *CONST sn[] = {
		"10", "12", "14", "16", "18", "20", "24", "28", "32", "36"
	};
	LOCAL CONST char *CONST fn[] = { "ゴシック", "明朝", "等幅" };
	INT	k;

	pop_fs = 3;
	for ( k = 0; k < 10; k++ ) {
		if ( ft_sizes[k] == fl.font_size ) {
			pop_fs = k;
		}
	}
	pop_ff = fl.font_face;
	ft_colour_hex(fl.text_col, pop_hex);
	pop_col = fl.text_col;
	pop_b = fl.bold;  pop_i = fl.italic;  pop_u = fl.under;  pop_s = fl.strike;
	p->it[0].type = FT_PI_CHOICE;  p->it[0].label = "サイズ:";
	p->it[0].names = sn;  p->it[0].n = 10;  p->it[0].value = &pop_fs;
	p->it[1].type = FT_PI_CHOICE;  p->it[1].label = "フォント:";
	p->it[1].names = fn;  p->it[1].n = 3;  p->it[1].value = &pop_ff;
	p->it[2].type = FT_PI_COLOUR;  p->it[2].label = "文字色:";
	p->it[2].text = pop_hex;  p->it[2].max = 8;
	p->it[3].type = FT_PI_SWATCH;  p->it[3].label = "";
	p->it[3].cols = ft_text_cols;  p->it[3].n = 10;  p->it[3].col = &pop_col;
	p->it[4].type = FT_PI_CHECK;  p->it[4].label = "太字";  p->it[4].value = &pop_b;
	p->it[5].type = FT_PI_CHECK;  p->it[5].label = "斜体";  p->it[5].value = &pop_i;
	p->it[6].type = FT_PI_CHECK;  p->it[6].label = "下線";  p->it[6].value = &pop_u;
	p->it[7].type = FT_PI_CHECK;  p->it[7].label = "打ち消し線";  p->it[7].value = &pop_s;
	p->n = 8;
	p->changed = font_changed;
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
		if ( ft_hex_colour(pop_hex, &c) ) {
			fl.stroke = c;
			pop_col = c;
		}
		break;
	case 2:
		fl.stroke = pop_col;
		ft_colour_hex(pop_col, pop_hex);
		break;
	default:
		fl.paint_size = ( pop_ps == 0 ) ? 1 : ( pop_ps == 1 ) ? 3
			      : ( pop_ps == 2 ) ? 5 : ( pop_ps == 3 ) ? 10 : 20;
		break;
	}
}

LOCAL void paint_palette( T_FTPOP *p )
{
	LOCAL CONST char *CONST tn[] = {
		"鉛筆", "絵筆", "消しゴム", "エアブラシ", "絵の具", "カッター"
	};
	LOCAL CONST char *CONST sn[] = { "1px", "3px", "5px", "10px", "20px" };

	pop_pt = fl.paint_tool;
	pop_ps = ( fl.paint_size <= 1 ) ? 0 : ( fl.paint_size <= 3 ) ? 1
	       : ( fl.paint_size <= 5 ) ? 2 : ( fl.paint_size <= 10 ) ? 3 : 4;
	ft_colour_hex(fl.stroke, pop_hex);
	pop_col = fl.stroke;
	p->it[0].type = FT_PI_CHOICE;  p->it[0].label = "画材:";
	p->it[0].names = tn;  p->it[0].n = 6;  p->it[0].value = &pop_pt;
	p->it[1].type = FT_PI_COLOUR;  p->it[1].label = "描画色:";
	p->it[1].text = pop_hex;  p->it[1].max = 8;
	p->it[2].type = FT_PI_SWATCH;  p->it[2].label = "";
	p->it[2].cols = ft_line_cols;
	p->it[2].n = (INT)( sizeof(ft_line_cols) / sizeof(ft_line_cols[0]) );
	p->it[2].col = &pop_col;
	p->it[3].type = FT_PI_CHOICE;  p->it[3].label = "太さ:";
	p->it[3].names = sn;  p->it[3].n = 5;  p->it[3].value = &pop_ps;
	p->n = 4;
	p->changed = paint_changed;
}

EXPORT BOOL ft_palette( INT which, T_FTPOP *p )
{
	knl_memset(p, 0, sizeof(*p));
	switch ( which ) {
	case FT_PAL_FILL:	fill_palette(p);	break;
	case FT_PAL_STROKE:	stroke_palette(p);	break;
	case FT_PAL_LSTYLE:	lstyle_palette(p);	break;
	case FT_PAL_CORNER:	corner_palette(p);	break;
	case FT_PAL_PAINT:	paint_palette(p);	break;
	case FT_PAL_ZOOM:	zoom_palette(p);	break;
	case FT_PAL_GRID:	grid_palette(p);	break;
	case FT_PAL_FONT:	font_palette(p);	break;
	default:		return FALSE;
	}
	ft_pop_layout(p);
	return TRUE;
}

/* ---------------------------------------------------------------- a line's ends */

/*
 * With the pointer in hand, a picked line's end is carried by pressing
 * on it: it follows the hand, and joins the point of a shape it is let
 * go near, or comes away from one it was joined to. The window it is
 * carried in is the view's arg.
 */
#define END_REACH	6

LOCAL void	*end_arg = NULL;
LOCAL INT	end_shape, end_which;		/* which line, and which end */
LOCAL T_DPPOINT	end_pts[2];
LOCAL INT	end_place, end_point;

/* The figure drawn again with the line as it is being carried, in inverted lines */
LOCAL void end_preview( T_FTVIEW *v )
{
	v->host->draw(v);
	(void)dp_set_mode(v->gid, DP_MODE_XOR);
	xor_line(v, end_pts[0].x, end_pts[0].y, end_pts[1].x, end_pts[1].y);
	(void)dp_set_mode(v->gid, DP_MODE_COPY);
	v->host->show(v);
}

/*
 * A press at (x, y) of the window on the end of a picked line: that end
 * carried. A finger's press reaches an end slop pixels farther, and
 * takes the nearest end in reach.
 */
EXPORT BOOL ft_end_press( T_FTVIEW *v, INT x, INT y, INT slop )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		i, k, fx, fy, reach, best = -1;
	T_DPPOINT	*path = NULL;
	BOOL		held = FALSE;

	if ( !ft_is(v) || ft_tool != FT_TL_SELECT || !v->ed->usable || f == NULL ) {
		return FALSE;
	}
	ft_to_fig(v, x, y, &fx, &fy);
	reach = END_REACH + slop * 8 / ( fl.zoom > 0 ? fl.zoom : 8 );
	for ( i = 0; i < f->nsh && !held; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];
		INT		n;

		if ( v->ed->pick[i] == 0 || s->kind != TV_SH_LINE || s->npt != 2
		  || s->node == NULL || !ft_same_word(s->node->name, "line") ) {
			continue;
		}
		/* the path of a line is some 64KB: from the heap, not the stack */
		if ( path == NULL ) {
			path = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * (SZ)TV_OUT_PTS(2));
			if ( path == NULL ) {
				break;
			}
		}
		/* its ends as it is drawn: a joined end is where it joins */
		n = tv_line_path(f, s, path, TV_OUT_PTS(2));
		if ( n < 2 ) {
			path[0] = s->pt[0];
			path[1] = s->pt[1];
			n = 2;
		}
		for ( k = 0; k < 2; k++ ) {
			T_DPPOINT	e = ( k == 0 ) ? path[0] : path[n - 1];
			INT		dx = e.x - fx, dy = e.y - fy;

			if ( dx * dx + dy * dy > reach * reach ) {
				continue;
			}
			if ( slop > 0 ) {
				/* a finger: the nearest of the ends in reach, held once all are seen */
				if ( best < 0 || dx * dx + dy * dy < best ) {
					best = dx * dx + dy * dy;
					end_shape = i;
					end_which = k;
					end_pts[0] = path[0];
					end_pts[1] = path[n - 1];
				}
				continue;
			}
			end_shape = i;
			end_which = k;
			end_pts[0] = path[0];
			end_pts[1] = path[n - 1];
			held = TRUE;
			break;
		}
	}
	if ( path != NULL ) {
		Kfree(path);
	}
	if ( held || best >= 0 ) {
		end_arg = v->arg;
		end_place = -1;
		end_preview(v);
		held = TRUE;
	}
	return held;
}

/* The window whose line's end is carried, NULL none */
EXPORT void *ft_end_arg( void )
{
	return end_arg;
}

/* The end carried to (x, y) of the window */
EXPORT void ft_end_follow( T_FTVIEW *v, INT x, INT y )
{
	INT	fx, fy, jx, jy;

	if ( end_arg == NULL || v->arg != end_arg ) {
		return;
	}
	ft_to_fig(v, x, y, &fx, &fy);
	end_place = -1;
	if ( ft_join_at(v, fx, fy, &end_place, &end_point, &jx, &jy) ) {
		fx = jx;
		fy = jy;
	} else {
		ft_snap(&fx, &fy);
	}
	end_pts[end_which].x = fx;
	end_pts[end_which].y = fy;
	ft_have_xy = TRUE;
	ft_px = fx;
	ft_py = fy;
	if ( v->host->tools_shown != NULL ) v->host->tools_shown();
	end_preview(v);
}

/* The end let go: where it is, and what it joins, put into the record */
EXPORT void ft_end_release( T_FTVIEW *v )
{
	T_FGED		*e = v->ed;
	T_TADNODE	*nd;
	CONST UB	*a;
	INT		cp;

	if ( end_arg == NULL || v->arg != end_arg ) {
		return;
	}
	end_arg = NULL;
	if ( e->fig == NULL || end_shape >= e->fig->nsh ) {
		return;
	}
	nd = e->fig->sh[end_shape].node;
	e->host->before(e);
	fe_points_put(e->rec, nd, end_pts, 2);
	ft_join_put(e->rec, nd, ( end_which == 0 ) ? "start_conn" : "end_conn", end_place, end_point);
	/* which ends are joined now */
	cp = 0;
	a = tad_attr(nd, "start_conn");
	if ( a != NULL && a[0] != 0 ) cp |= 1;
	a = tad_attr(nd, "end_conn");
	if ( a != NULL && a[0] != 0 ) cp |= 2;
	fe_attr_put(e->rec, nd, "conn_pat", cp);
	e->host->changed(e);
}

/* ---------------------------------------------------------------- 変形 */

/*
 * A picked shape changed point by point: a polygon's corners, the ends
 * of an arc -- and a sector's middle, which carries the whole of it --
 * and the two ends of a curve. Each point is a handle carried by the
 * hand; pressing anywhere else ends it. The window it is changed in is
 * the view's arg.
 */
#define TS_POINT	0		/* + which corner of a polygon */
#define TS_CENTRE	1000
#define TS_START	1001
#define TS_END		1002
#define TS_REACH	5

LOCAL void	*ts_arg = NULL;
LOCAL INT	ts_shape = -1;
LOCAL INT	ts_drag = -1;		/* which handle, while carried */
LOCAL T_DPPOINT	*ts_pts = NULL;		/* the points as they are being moved */
LOCAL INT	ts_npt, ts_maxpt = 0;
LOCAL T_DPPOINT	*ts_h = NULL;		/* and the handles: a point each, and three more */
LOCAL INT	*ts_which = NULL;
LOCAL T_DPPOINT	ts_c, ts_s, ts_e;	/* an arc's middle and ends */

EXPORT BOOL ft_transformable( CONST T_TVSHAPE *s )
{
	if ( s->node == NULL ) {
		return FALSE;
	}
	return (BOOL)( s->kind == TV_SH_ARC || s->kind == TV_SH_CHORD
		    || s->kind == TV_SH_EARC || s->kind == TV_SH_CURVE
		    || ( s->kind == TV_SH_POLY && ft_same_word(s->node->name, "polygon") ) );
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
LOCAL INT ts_handles( CONST T_FTVIEW *v, T_DPPOINT *out, INT *which )
{
	CONST T_TVFIG	*f = v->ed->fig;
	CONST T_TVSHAPE	*s;
	INT		n = 0, i;

	if ( ts_arg == NULL || f == NULL || ts_shape < 0 || ts_shape >= f->nsh
	  || out == NULL || which == NULL ) {
		return 0;
	}
	s = &f->sh[ts_shape];
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

/* The shape's points taken to be moved; FALSE when there was no memory for them */
LOCAL BOOL ts_take( T_FTVIEW *v )
{
	CONST T_TVSHAPE	*s = &v->ed->fig->sh[ts_shape];
	INT		i, m = ( s->npt > ts_maxpt ) ? s->npt : ts_maxpt;

	ts_npt = 0;
	if ( m > ts_maxpt || ts_pts == NULL || ts_h == NULL || ts_which == NULL ) {
		T_DPPOINT	*p = (T_DPPOINT *)Krealloc(ts_pts, sizeof(T_DPPOINT) * (size_t)( m + 1 ));
		T_DPPOINT	*h = ( p != NULL ) ? (T_DPPOINT *)Krealloc(ts_h, sizeof(T_DPPOINT) * (size_t)( m + 3 )) : NULL;
		INT		*w = ( h != NULL ) ? (INT *)Krealloc(ts_which, sizeof(INT) * (size_t)( m + 3 )) : NULL;

		if ( p != NULL ) ts_pts = p;
		if ( h != NULL ) ts_h = h;
		if ( w != NULL ) ts_which = w;
		if ( w == NULL ) {
			/* not changed, rather than changed with points missing */
			ts_arg = NULL;
			v->ed->host->no_memory(v->ed);
			return FALSE;
		}
		ts_maxpt = m;
	}
	for ( i = 0; i < s->npt; i++ ) {
		ts_pts[ts_npt++] = s->pt[i];
	}
	if ( s->kind == TV_SH_ARC || s->kind == TV_SH_CHORD || s->kind == TV_SH_EARC ) {
		arc_ends(s);
	}
	return TRUE;
}

/* 変形 begun on the first picked shape that can be changed; FALSE when none can */
EXPORT BOOL ft_transform( T_FTVIEW *v )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		i;

	ts_arg = NULL;
	for ( i = 0; f != NULL && i < f->nsh; i++ ) {
		if ( v->ed->pick[i] != 0 && ft_transformable(&f->sh[i]) ) {
			ts_arg = v->arg;
			ts_shape = i;
			ts_drag = -1;
			if ( ts_take(v) ) {
				v->host->draw(v);
				v->host->show(v);
			}
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT BOOL ft_transform_on( CONST T_FTVIEW *v )
{
	return (BOOL)( ts_arg != NULL && ts_arg == v->arg );
}

/* The window whose shape is being changed, NULL none */
EXPORT void *ft_transform_arg( void )
{
	return ts_arg;
}

/*
 * A press at (x, y) of the window while a shape is being changed: on a
 * handle, it is carried. A finger's press reaches a handle slop farther
 * and takes the nearest in reach. Anywhere else the change is over.
 */
EXPORT BOOL ft_transform_press( T_FTVIEW *v, INT x, INT y, INT slop )
{
	INT	n, i, wx, wy, r = TS_REACH + slop, best = -1, near = 0;

	if ( !ft_transform_on(v) ) {
		return FALSE;
	}
	n = ts_handles(v, ts_h, ts_which);
	for ( i = 0; i < n; i++ ) {
		INT	dx, dy, far;

		ft_fig_to_win(v, ts_h[i].x, ts_h[i].y, &wx, &wy);
		if ( x >= wx - r && x <= wx + r && y >= wy - r && y <= wy + r ) {
			if ( slop <= 0 ) {
				ts_drag = ts_which[i];
				return TRUE;
			}
			dx = ( x > wx ) ? x - wx : wx - x;
			dy = ( y > wy ) ? y - wy : wy - y;
			far = ( dx > dy ) ? dx : dy;
			if ( best < 0 || far < near ) {
				best = i;
				near = far;
			}
		}
	}
	if ( best >= 0 ) {
		ts_drag = ts_which[best];
		return TRUE;
	}
	ts_arg = NULL;
	v->host->draw(v);
	v->host->show(v);
	return FALSE;
}

EXPORT void ft_transform_end( T_FTVIEW *v )
{
	if ( ft_transform_on(v) ) {
		ts_arg = NULL;
		ts_drag = -1;
		v->host->draw(v);
		v->host->show(v);
	}
}

/* A window gone: its 変形 and its carried end go with it */
EXPORT void ft_forget( void *arg )
{
	if ( ts_arg == arg ) {
		ts_arg = NULL;
		ts_drag = -1;
	}
	if ( end_arg == arg ) {
		end_arg = NULL;
	}
}

EXPORT BOOL ft_transform_carrying( void )
{
	return (BOOL)( ts_arg != NULL && ts_drag >= 0 );
}

/* The handle carried to (x, y) of the window */
EXPORT void ft_transform_follow( T_FTVIEW *v, INT x, INT y )
{
	INT	fx, fy;
	BOOL	curve;

	if ( !ft_transform_on(v) || ts_drag < 0 || v->ed->fig == NULL || ts_shape >= v->ed->fig->nsh ) {
		return;
	}
	curve = (BOOL)( ts_npt >= 2 && v->ed->fig->sh[ts_shape].kind == TV_SH_CURVE );
	ft_to_fig(v, x, y, &fx, &fy);
	ft_snap(&fx, &fy);
	if ( ts_drag == TS_CENTRE ) {
		INT	dx = fx - ts_c.x, dy = fy - ts_c.y;

		ts_c.x += dx;  ts_c.y += dy;
		ts_s.x += dx;  ts_s.y += dy;
		ts_e.x += dx;  ts_e.y += dy;
	} else if ( ts_drag == TS_START ) {
		if ( curve ) {
			ts_pts[0].x = fx;  ts_pts[0].y = fy;
		} else {
			ts_s.x = fx;  ts_s.y = fy;
		}
	} else if ( ts_drag == TS_END ) {
		if ( curve ) {
			ts_pts[ts_npt - 1].x = fx;  ts_pts[ts_npt - 1].y = fy;
		} else {
			ts_e.x = fx;  ts_e.y = fy;
		}
	} else if ( ts_drag >= TS_POINT && ts_drag < TS_POINT + ts_npt ) {
		ts_pts[ts_drag - TS_POINT].x = fx;
		ts_pts[ts_drag - TS_POINT].y = fy;
	}
	ft_have_xy = TRUE;
	ft_px = fx;
	ft_py = fy;
	if ( v->host->tools_shown != NULL ) v->host->tools_shown();
	v->host->draw(v);
	v->host->show(v);
}

/* The change put into the record, and the handles kept for more */
EXPORT void ft_transform_release( T_FTVIEW *v )
{
	T_FGED		*e = v->ed;
	CONST T_TVSHAPE	*s;
	T_TADNODE	*nd;

	if ( !ft_transform_on(v) || ts_drag < 0 ) {
		return;
	}
	ts_drag = -1;
	if ( e->fig == NULL || ts_shape >= e->fig->nsh ) {
		return;
	}
	s = &e->fig->sh[ts_shape];
	nd = s->node;
	e->host->before(e);
	if ( s->kind == TV_SH_POLY || s->kind == TV_SH_CURVE ) {
		fe_points_put(e->rec, nd, ts_pts, ts_npt);
	} else {
		INT	hw = ( s->r.right - s->r.left ) / 2;
		INT	hh = ( s->r.bottom - s->r.top ) / 2;

		/* the frame follows the middle; the ends are where they are towards */
		ft_frame_put(e->rec, nd, ts_c.x - hw, ts_c.y - hh, ts_c.x + hw, ts_c.y + hh);
		fe_attr_put(e->rec, nd, "startX", ts_s.x);
		fe_attr_put(e->rec, nd, "startY", ts_s.y);
		fe_attr_put(e->rec, nd, "endX", ts_e.x);
		fe_attr_put(e->rec, nd, "endY", ts_e.y);
	}
	e->host->changed(e);
	/* the model is made again: the points taken afresh from it */
	if ( ts_arg != NULL && e->fig != NULL && ts_shape < e->fig->nsh ) {
		(void)ts_take(v);
	}
	v->host->draw(v);
	v->host->show(v);
}

/* Over the figure while it is changed: the new lines while a handle is carried, and the handles */
LOCAL void ts_draw( CONST T_FTVIEW *v )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		n, i, wx, wy;

	if ( ts_drag >= 0 && f != NULL && ts_shape < f->nsh ) {
		CONST T_TVSHAPE	*s = &f->sh[ts_shape];

		(void)dp_set_mode(v->gid, DP_MODE_XOR);
		if ( s->kind == TV_SH_POLY || s->kind == TV_SH_CURVE ) {
			for ( i = 0; i + 1 < ts_npt; i++ ) {
				xor_line(v, ts_pts[i].x, ts_pts[i].y, ts_pts[i + 1].x, ts_pts[i + 1].y);
			}
			if ( s->kind == TV_SH_POLY && ts_npt > 2 ) {
				xor_line(v, ts_pts[ts_npt - 1].x, ts_pts[ts_npt - 1].y, ts_pts[0].x, ts_pts[0].y);
			}
		} else {
			xor_line(v, ts_c.x, ts_c.y, ts_s.x, ts_s.y);
			xor_line(v, ts_c.x, ts_c.y, ts_e.x, ts_e.y);
		}
		(void)dp_set_mode(v->gid, DP_MODE_COPY);
	}
	n = ts_handles(v, ts_h, ts_which);
	for ( i = 0; i < n; i++ ) {
		T_DPRECT	b;

		ft_fig_to_win(v, ts_h[i].x, ts_h[i].y, &wx, &wy);
		b.left = wx - 4;  b.top = wy - 4;
		b.right = wx + 4;  b.bottom = wy + 4;
		(void)dp_fill_rect(v->gid, &b, ( ts_which[i] == TS_CENTRE ) ? 0x00FF8F00U : 0x00FFFFFFU);
		(void)dp_frame_rect(v->gid, &b, ( ts_which[i] == TS_CENTRE ) ? 0x00000000U : 0x00FF8F00U, 1);
	}
}

/* ---------------------------------------------------------------- 用紙枠 */

/*
 * The paper a figure is laid on, drawn over it: each page's edge, and
 * inside it its margins, in broken lines, as many pages as the figure
 * and the window reach. The paper is in points; the figure's own units
 * are pixels at 96 to the inch.
 */
LOCAL void paper_draw( CONST T_FTVIEW *v )
{
	CONST T_TVFIG	*f = v->ed->fig;
	INT		pw, ph, fw = 0, fh = 0, x, y, k, m[4];
	T_DPPAT		pat, mp;

	if ( !v->paper_frame || f == NULL ) {
		return;
	}
	pw = ( ( f->paper_w > 0 ) ? f->paper_w : DE_PAPER_W ) * 4 / 3;
	ph = ( ( f->paper_h > 0 ) ? f->paper_h : DE_PAPER_H ) * 4 / 3;
	for ( k = 0; k < 4; k++ ) {
		m[k] = f->pmargin[k] * 4 / 3;
	}
	(void)tv_fig_size(f, &fw, &fh);
	x = ( v->page.right - v->page.left + v->sx ) * 8 / fl.zoom;
	y = ( v->page.bottom - v->page.top + v->sy ) * 8 / fl.zoom;
	if ( fw < x ) fw = x;
	if ( fh < y ) fh = y;
	dp_pat_colour(&pat, 0x00808080U);
	dp_pat_colour(&mp, 0x00C0C0C0U);
	for ( y = 0; y < fh; y += ph ) {
		for ( x = 0; x < fw; x += pw ) {
			INT	x0, y0, x1, y1;

			ft_fig_to_win(v, x, y, &x0, &y0);
			ft_fig_to_win(v, x + pw, y + ph, &x1, &y1);
			(void)dp_line_wide(v->gid, x0, y0, x1, y0, 1, DP_LINE_DASH, &pat);
			(void)dp_line_wide(v->gid, x0, y1, x1, y1, 1, DP_LINE_DASH, &pat);
			(void)dp_line_wide(v->gid, x0, y0, x0, y1, 1, DP_LINE_DASH, &pat);
			(void)dp_line_wide(v->gid, x1, y0, x1, y1, 1, DP_LINE_DASH, &pat);
			if ( m[0] + m[1] + m[2] + m[3] > 0 ) {
				ft_fig_to_win(v, x + m[0], y + m[1], &x0, &y0);
				ft_fig_to_win(v, x + pw - m[2], y + ph - m[3], &x1, &y1);
				(void)dp_line_wide(v->gid, x0, y0, x1, y0, 1, DP_LINE_DOT, &mp);
				(void)dp_line_wide(v->gid, x0, y1, x1, y1, 1, DP_LINE_DOT, &mp);
				(void)dp_line_wide(v->gid, x0, y0, x0, y1, 1, DP_LINE_DOT, &mp);
				(void)dp_line_wide(v->gid, x1, y0, x1, y1, 1, DP_LINE_DOT, &mp);
			}
		}
	}
}

/*
 * 用紙枠設定: the figure's paper and margins as the paper panel asks
 * them, in tenths of a millimetre to the whole millimetre.
 */
EXPORT void ft_paper_of( CONST T_FTVIEW *v, T_DEPAPER *ps )
{
	CONST T_TVFIG	*f = v->ed->fig;
	T_TADNODE	*root = tad_root(v->ed->rec), *body = tad_body(v->ed->rec), *n, *pp = NULL;

	knl_memset(ps, 0, sizeof(*ps));
	if ( f == NULL ) {
		return;
	}
	for ( n = ( root != NULL ) ? root->first : NULL; n != NULL && n != body; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && ft_same_word(n->name, "paper") ) pp = n;
	}
	ps->w = ( ( ( f->paper_w > 0 ? f->paper_w : DE_PAPER_W ) * 254 / 72 ) + 5 ) / 10 * 10;
	ps->h = ( ( ( f->paper_h > 0 ? f->paper_h : DE_PAPER_H ) * 254 / 72 ) + 5 ) / 10 * 10;
	ps->left = ( f->pmargin[0] * 254 / 72 + 5 ) / 10 * 10;
	ps->top = ( f->pmargin[1] * 254 / 72 + 5 ) / 10 * 10;
	ps->right = ( f->pmargin[2] * 254 / 72 + 5 ) / 10 * 10;
	ps->bottom = ( f->pmargin[3] * 254 / 72 + 5 ) / 10 * 10;
	ps->imposition = ( pp != NULL && tad_attr(pp, "imposition") != NULL
			   && tad_attr(pp, "imposition")[0] == '1' ) ? 1 : 0;
}

/* The paper panel's answer r (DE_PAPER_*) written ahead of the figure */
EXPORT void ft_paper_put( T_FTVIEW *v, T_DEPAPER *ps, INT r )
{
	T_FGED		*e = v->ed;
	T_TADNODE	*root = tad_root(e->rec), *body = tad_body(e->rec), *pp = NULL, *dm = NULL, *n;

	if ( root == NULL || body == NULL || e->fig == NULL || !e->usable || r == DE_PAPER_CANCEL ) {
		return;
	}
	for ( n = root->first; n != NULL && n != body; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && ft_same_word(n->name, "paper") ) pp = n;
		if ( n->kind == TAD_ND_ELEM && ft_same_word(n->name, "docmargin") ) dm = n;
	}
	if ( r == DE_PAPER_STD ) {
		ps->w = 2100;  ps->h = 2970;
		ps->top = 200;  ps->bottom = 200;
		ps->left = 200;  ps->right = 100;
		ps->imposition = 0;
	}
	e->host->before(e);
	if ( pp == NULL ) {
		pp = tad_elem_new(e->rec, "paper", root, body, TRUE);
	}
	if ( dm == NULL ) {
		dm = tad_elem_new(e->rec, "docmargin", root, body, TRUE);
	}
	if ( pp != NULL ) {
		fe_attr_put(e->rec, pp, "imposition", ps->imposition);
		fe_attr_put(e->rec, pp, "binding", 0);
		fe_attr_put(e->rec, pp, "length", ( ps->h * 72 + 127 ) / 254);
		fe_attr_put(e->rec, pp, "width", ( ps->w * 72 + 127 ) / 254);
		fe_attr_put(e->rec, pp, "top", ( ps->top * 72 + 127 ) / 254);
		fe_attr_put(e->rec, pp, "bottom", ( ps->bottom * 72 + 127 ) / 254);
		fe_attr_put(e->rec, pp, "left", ( ps->left * 72 + 127 ) / 254);
		fe_attr_put(e->rec, pp, "right", ( ps->right * 72 + 127 ) / 254);
	}
	if ( dm != NULL ) {
		fe_attr_put(e->rec, dm, "top", ( ps->top * 72 + 127 ) / 254);
		fe_attr_put(e->rec, dm, "bottom", ( ps->bottom * 72 + 127 ) / 254);
		fe_attr_put(e->rec, dm, "left", ( ps->left * 72 + 127 ) / 254);
		fe_attr_put(e->rec, dm, "right", ( ps->right * 72 + 127 ) / 254);
	}
	e->host->changed(e);
}

/* ---------------------------------------------------------------- over the figure */

/* What is drawn over the figure after it: the paper, and the handles of 変形 */
EXPORT void ft_overlay( CONST T_FTVIEW *v )
{
	paper_draw(v);
	if ( ft_transform_on(v) ) {
		ts_draw(v);
	}
}

/* ---------------------------------------------------------------- 実寸パネル */

/*
 * The figure at its own size, whatever it is magnified by, in gid over
 * all: the part of it the figure's window shows at its top left.
 */
EXPORT void ft_realsize_draw( CONST T_FTVIEW *v, INT gid, CONST T_DPRECT *all, UW paper )
{
	T_TVFIG	*f = v->ed->fig;
	INT	z;

	if ( f == NULL ) {
		return;
	}
	z = f->zoom;
	f->zoom = 8;
	(void)tv_fig_draw(gid, f, all, v->sx * 8 / fl.zoom, v->sy * 8 / fl.zoom, v->ed->rec, paper);
	f->zoom = z;
}
