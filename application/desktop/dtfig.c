/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfig.c
 *	The figure editor (design 17.13)
 *
 *	A figure whose object opens with the figure editor has every shape
 *	in it worked on, not only its virtual objects: taken, carried,
 *	sized, thrown away, copied, put in front or behind, turned over and
 *	turned round. And shapes are drawn into it with the tools of the
 *	道具パネル, a small window beside it: a line, a rectangle with square
 *	or round corners, an ellipse, a line of several segments -- in the
 *	colour of line and of inside chosen in the panel, and the width of
 *	line chosen there.
 *
 *	What is picked, carried and sized is dtedit.c's, the same code a
 *	cabinet uses; what is done to a shape that is not a virtual object
 *	is here, by changing the attributes of its element in the record.
 *	Colours are written as the fixed pattern numbers every record may
 *	use, so that what is drawn here reads back anywhere.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dbox.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/cabmenu.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include <ts/fn.h>
#include <ts/look.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESC		0x29

/* ---------------------------------------------------------------- numbers */

LOCAL INT num_word( UB *buf, INT at, INT v )
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

LOCAL void attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v )
{
	UB	buf[16];

	(void)num_word(buf, 0, v);
	(void)tad_set_attr(rec, nd, name, buf);
}

/* "x,y x,y ..." written into an element */
LOCAL void points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt,
		       INT n )
{
	UB	*buf;
	INT	i, at = 0;

	buf = (UB *)Kmalloc((SZ)n * 26 + 2);
	if ( buf == NULL ) {
		return;
	}
	buf[0] = 0;
	for ( i = 0; i < n; i++ ) {
		if ( i > 0 ) {
			buf[at++] = ' ';
		}
		at = num_word(buf, at, pt[i].x);
		buf[at++] = ',';
		at = num_word(buf, at, pt[i].y);
	}
	(void)tad_set_attr(rec, nd, "points", buf);
	Kfree(buf);
}

LOCAL T_TAD *rec_of( DTWIN *d )
{
	return (T_TAD *)d->rec;
}

EXPORT void df_attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v )
{
	attr_put(rec, nd, name, v);
}

EXPORT void df_points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt,
			   INT n )
{
	points_put(rec, nd, pt, n);
}

/* ---------------------------------------------------------------- which */

EXPORT BOOL df_is( CONST DTWIN *d )
{
	return (BOOL)( d != NULL && d->figed && d->fig != NULL && !d->sealed );
}

/* A shape this editor works on that is not a virtual object */
EXPORT BOOL df_takeable( CONST DTWIN *d, INT i )
{
	CONST T_TVSHAPE	*sh;

	if ( !df_is(d) || i < 0 || i >= d->fig->nsh ) {
		return FALSE;
	}
	sh = &d->fig->sh[i];
	if ( sh->node == NULL ) {
		return FALSE;
	}
	switch ( sh->kind ) {
	case TV_SH_RECT:
	case TV_SH_ELLIPSE:
	case TV_SH_LINE:
	case TV_SH_POLY:
	case TV_SH_IMAGE:
	case TV_SH_DOC:
	case TV_SH_ARC:
	case TV_SH_CHORD:
	case TV_SH_EARC:
	case TV_SH_CURVE:
	case TV_SH_PIXMAP:
	case TV_SH_MARKER:
		return TRUE;
	default:
		return FALSE;
	}
}

/* ---------------------------------------------------------------- groups */

/*
 * The element a shape stands for in the figure: itself, or the group
 * it is in -- the outermost one, the child of the figure. A group is
 * taken, carried, thrown away and copied whole.
 */
LOCAL T_TADNODE *top_of( DTWIN *d, T_TADNODE *nd )
{
	T_TADNODE	*body = tad_body(rec_of(d));

	while ( nd != NULL && nd->parent != NULL && nd->parent != body ) {
		nd = nd->parent;
	}

	return nd;
}

LOCAL BOOL in_group( DTWIN *d, INT i )
{
	return (BOOL)( d->fig->sh[i].node != NULL
		    && top_of(d, d->fig->sh[i].node) != d->fig->sh[i].node );
}

/*
 * Every shape of a group taken when any one of it is: what is taken is
 * the group. Called after any change to what is taken.
 */
EXPORT void df_whole_groups( DTWIN *d )
{
	INT	i, k;

	if ( !df_is(d) ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*top;

		if ( d->pick[i] == 0 || !in_group(d, i) ) {
			continue;
		}
		top = top_of(d, d->fig->sh[i].node);
		for ( k = 0; k < d->fig->nsh; k++ ) {
			if ( d->pick[k] == 0 && d->fig->sh[k].node != NULL
			  && top_of(d, d->fig->sh[k].node) == top ) {
				d->pick[k] = d->pick[i];
				d->npick++;
			}
		}
	}
}

/* The picked shapes put into one group, in the place of the first */
LOCAL void group_picked( DTWIN *d )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *g = NULL, *tops[TV_MAX_SHAPE];
	INT		i, k, n = 0;

	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;
		BOOL		seen = FALSE;

		if ( d->pick[i] == 0 || d->fig->sh[i].node == NULL ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		for ( k = 0; k < n; k++ ) {
			seen = (BOOL)( seen || tops[k] == t );
		}
		if ( !seen && n < TV_MAX_SHAPE ) {
			tops[n++] = t;
		}
	}
	if ( n < 2 || body == NULL ) {
		return;
	}
	ed_before(d);
	g = tad_elem_new(rec, "group", body, tops[0], FALSE);
	if ( g == NULL ) {
		return;
	}
	for ( k = 0; k < n; k++ ) {
		tad_node_move(tops[k], g, NULL);
	}
	ed_pick_none(d);
	ed_changed(d);
}

/* The groups the picked shapes are in, taken apart: their shapes go back */
LOCAL void ungroup_picked( DTWIN *d )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *g, *c, *next;
	INT		i;
	BOOL		any = FALSE;

	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && in_group(d, i) ) {
			any = TRUE;
		}
	}
	if ( !any || body == NULL ) {
		return;
	}
	ed_before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !in_group(d, i) ) {
			continue;
		}
		g = top_of(d, d->fig->sh[i].node);
		if ( g == NULL || g->parent != body ) {
			continue;
		}
		for ( c = g->first; c != NULL; c = next ) {
			next = c->next;
			tad_node_move(c, body, g);
		}
		tad_node_remove(g);
	}
	ed_pick_none(d);
	ed_changed(d);
}

/* ---------------------------------------------------------------- one shape */

/* The box of a piece of text in a figure, where its record keeps it */
LOCAL void doc_box_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPRECT *r )
{
	T_TADNODE	*c;

	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM || c->name == NULL ) {
			continue;
		}
		if ( tad_attr(c, "viewleft") != NULL ) {
			attr_put(rec, c, "viewleft", r->left);
			attr_put(rec, c, "viewtop", r->top);
			attr_put(rec, c, "viewright", r->right);
			attr_put(rec, c, "viewbottom", r->bottom);
		}
		if ( tad_attr(c, "drawleft") != NULL ) {
			attr_put(rec, c, "drawleft", r->left);
			attr_put(rec, c, "drawtop", r->top);
			attr_put(rec, c, "drawright", r->right);
			attr_put(rec, c, "drawbottom", r->bottom);
		}
	}
}

/* A shape's box put into its element, as the element says boxes */
LOCAL void box_put( T_TAD *rec, CONST T_TVSHAPE *sh, CONST T_DPRECT *r )
{
	T_TADNODE	*nd = sh->node;

	switch ( sh->kind ) {
	case TV_SH_ELLIPSE:
		if ( tad_attr(nd, "frameLeft") != NULL ) {
			attr_put(rec, nd, "frameLeft", r->left);
			attr_put(rec, nd, "frameTop", r->top);
			attr_put(rec, nd, "frameRight", r->right);
			attr_put(rec, nd, "frameBottom", r->bottom);
		} else if ( tad_attr(nd, "cx") != NULL || tad_attr(nd, "left") == NULL ) {
			attr_put(rec, nd, "cx", ( r->left + r->right ) / 2);
			attr_put(rec, nd, "cy", ( r->top + r->bottom ) / 2);
			attr_put(rec, nd, "rx", ( r->right - r->left ) / 2);
			attr_put(rec, nd, "ry", ( r->bottom - r->top ) / 2);
		} else {
			attr_put(rec, nd, "left", r->left);
			attr_put(rec, nd, "top", r->top);
			attr_put(rec, nd, "right", r->right);
			attr_put(rec, nd, "bottom", r->bottom);
		}
		break;
	case TV_SH_DOC:
		doc_box_put(rec, nd, r);
		break;
	default:
		attr_put(rec, nd, "left", r->left);
		attr_put(rec, nd, "top", r->top);
		attr_put(rec, nd, "right", r->right);
		attr_put(rec, nd, "bottom", r->bottom);
		break;
	}
}

/* A line's points put into its element, as the element says points */
LOCAL void pts_put( T_TAD *rec, CONST T_TVSHAPE *sh, CONST T_DPPOINT *pt, INT n )
{
	if ( tad_attr(sh->node, "points") == NULL && n == 2 ) {
		attr_put(rec, sh->node, "x1", pt[0].x);
		attr_put(rec, sh->node, "y1", pt[0].y);
		attr_put(rec, sh->node, "x2", pt[1].x);
		attr_put(rec, sh->node, "y2", pt[1].y);
		return;
	}
	points_put(rec, sh->node, pt, n);
}

/* A number an element gives, or 'dflt' */
LOCAL INT attr_num( CONST T_TADNODE *nd, CONST char *name, INT dflt )
{
	CONST UB	*v = tad_attr(nd, name);
	INT		n = 0, sign = 1;

	if ( v == NULL ) {
		return dflt;
	}
	if ( *v == '-' ) {
		sign = -1;
		v++;
	}
	if ( *v < '0' || *v > '9' ) {
		return dflt;
	}
	while ( *v >= '0' && *v <= '9' ) {
		n = n * 10 + ( *v++ - '0' );
	}

	return sign * n;
}

/* Whether a shape is its points: a line, a polygon, a curve, marks */
LOCAL BOOL of_points( CONST T_TVSHAPE *sh )
{
	return (BOOL)( sh->kind == TV_SH_LINE || sh->kind == TV_SH_POLY
		    || sh->kind == TV_SH_CURVE || sh->kind == TV_SH_MARKER );
}

LOCAL BOOL of_arc( CONST T_TVSHAPE *sh )
{
	return (BOOL)( sh->kind == TV_SH_ARC || sh->kind == TV_SH_CHORD
		    || sh->kind == TV_SH_EARC );
}

/* A place in one box put at the same place in another */
LOCAL INT map_x( INT x, CONST T_DPRECT *from, CONST T_DPRECT *to )
{
	INT	ow = from->right - from->left, nw = to->right - to->left;

	return to->left + ( ( ow > 0 ) ? ( x - from->left ) * nw / ow : 0 );
}

LOCAL INT map_y( INT y, CONST T_DPRECT *from, CONST T_DPRECT *to )
{
	INT	oh = from->bottom - from->top, nh = to->bottom - to->top;

	return to->top + ( ( oh > 0 ) ? ( y - from->top ) * nh / oh : 0 );
}

/* An arc's ends, as its element gives them */
LOCAL void arc_ends_of( CONST T_TVSHAPE *sh, T_DPPOINT *s, T_DPPOINT *e )
{
	s->x = attr_num(sh->node, "startX", sh->r.right);
	s->y = attr_num(sh->node, "startY", sh->r.top);
	e->x = attr_num(sh->node, "endX", sh->r.left);
	e->y = attr_num(sh->node, "endY", sh->r.bottom);
}

/* An arc written: its frame, and where its ends are towards */
LOCAL void arc_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPRECT *r,
		    CONST T_DPPOINT *s, CONST T_DPPOINT *e )
{
	attr_put(rec, nd, "frameLeft", r->left);
	attr_put(rec, nd, "frameTop", r->top);
	attr_put(rec, nd, "frameRight", r->right);
	attr_put(rec, nd, "frameBottom", r->bottom);
	attr_put(rec, nd, "startX", s->x);
	attr_put(rec, nd, "startY", s->y);
	attr_put(rec, nd, "endX", e->x);
	attr_put(rec, nd, "endY", e->y);
}

/*
 * One shape taken to a new box: a box shape takes the box, a line has
 * each of its points moved to the same place in the new box as it had
 * in the old, and an arc its frame with its ends at the same places in
 * it.
 */
LOCAL void to_box( DTWIN *d, INT i, CONST T_DPRECT *nb )
{
	CONST T_TVSHAPE	*sh = &d->fig->sh[i];

	if ( of_arc(sh) ) {
		T_DPPOINT	s, e;

		arc_ends_of(sh, &s, &e);
		s.x = map_x(s.x, &sh->r, nb);  s.y = map_y(s.y, &sh->r, nb);
		e.x = map_x(e.x, &sh->r, nb);  e.y = map_y(e.y, &sh->r, nb);
		arc_put(rec_of(d), sh->node, nb, &s, &e);
		return;
	}
	if ( of_points(sh) ) {
		T_DPPOINT	pt[TV_MAX_PT];
		INT		k, ow = sh->r.right - sh->r.left - 1;
		INT		oh = sh->r.bottom - sh->r.top - 1;
		INT		nw = nb->right - nb->left - 1;
		INT		nh = nb->bottom - nb->top - 1;

		for ( k = 0; k < sh->npt && k < TV_MAX_PT; k++ ) {
			pt[k].x = nb->left + ( ( ow > 0 )
				  ? ( sh->pt[k].x - sh->r.left ) * nw / ow : 0 );
			pt[k].y = nb->top + ( ( oh > 0 )
				  ? ( sh->pt[k].y - sh->r.top ) * nh / oh : 0 );
		}
		pts_put(rec_of(d), sh, pt, sh->npt);
		return;
	}
	box_put(rec_of(d), sh, nb);
}

EXPORT void df_shift( DTWIN *d, INT i, INT dx, INT dy )
{
	T_DPRECT	nb;

	if ( !df_takeable(d, i) ) {
		return;
	}
	nb = d->fig->sh[i].r;
	nb.left += dx;  nb.right += dx;
	nb.top += dy;   nb.bottom += dy;
	to_box(d, i, &nb);
}

EXPORT void df_resize( DTWIN *d, INT i, CONST T_DPRECT *nb )
{
	if ( df_takeable(d, i) && nb->right > nb->left && nb->bottom > nb->top ) {
		to_box(d, i, nb);
	}
}

/* ---------------------------------------------------------------- what is picked */

EXPORT INT df_picked( CONST DTWIN *d )
{
	INT	i, n = 0;

	for ( i = 0; df_is(d) && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && df_takeable(d, i) ) {
			n++;
		}
	}

	return n;
}

EXPORT void df_delete_picked( DTWIN *d )
{
	INT	i;

	for ( i = 0; df_is(d) && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && df_takeable(d, i) ) {
			/* a group goes whole; taking it out twice is nothing */
			tad_node_remove(top_of(d, d->fig->sh[i].node));
		}
	}
}

/*
 * Turned over, left to right or top to bottom, or turned a quarter
 * round, each shape about the middle of its own box.
 */
#define TS_FLIP_H	1
#define TS_FLIP_V	2
#define TS_ROT_L	3
#define TS_ROT_R	4

LOCAL void turn( DTWIN *d, INT i, INT how )
{
	CONST T_TVSHAPE	*sh = &d->fig->sh[i];
	INT		cx2 = sh->r.left + sh->r.right - 1;	/* twice the middle */
	INT		cy2 = sh->r.top + sh->r.bottom - 1;

	if ( sh->kind == TV_SH_IMAGE || sh->kind == TV_SH_PIXMAP ) {
		/* a picture keeps its box and says how it is turned */
		T_TAD		*rec = rec_of(d);
		CONST UB	*f;
		INT		rot = attr_num(sh->node, "rotation", 0);

		switch ( how ) {
		case TS_FLIP_H:
			f = tad_attr(sh->node, "flipH");
			(void)tad_set_attr(rec, sh->node, "flipH", (CONST UB *)
					   ( ( f != NULL && f[0] == 't' ) ? "false" : "true" ));
			break;
		case TS_FLIP_V:
			f = tad_attr(sh->node, "flipV");
			(void)tad_set_attr(rec, sh->node, "flipV", (CONST UB *)
					   ( ( f != NULL && f[0] == 't' ) ? "false" : "true" ));
			break;
		default:
			rot = ( rot + ( ( how == TS_ROT_R ) ? 90 : 270 ) ) % 360;
			attr_put(rec, sh->node, "rotation", rot);
			break;
		}
		return;
	}
	if ( of_arc(sh) ) {
		/*
		 * An arc: its ends turned with it. Turned over, it runs the
		 * other way round, so its ends change places.
		 */
		T_DPPOINT	s, e, t;
		T_DPRECT	nb = sh->r;
		T_DPPOINT	*p[2];
		INT		k;

		arc_ends_of(sh, &s, &e);
		p[0] = &s;
		p[1] = &e;
		for ( k = 0; k < 2; k++ ) {
			INT	x = p[k]->x, y = p[k]->y;

			switch ( how ) {
			case TS_FLIP_H:	p[k]->x = cx2 - x;  break;
			case TS_FLIP_V:	p[k]->y = cy2 - y;  break;
			case TS_ROT_R:
				p[k]->x = ( cx2 - ( 2 * y - cy2 ) ) / 2;
				p[k]->y = ( cy2 + ( 2 * x - cx2 ) ) / 2;
				break;
			default:
				p[k]->x = ( cx2 + ( 2 * y - cy2 ) ) / 2;
				p[k]->y = ( cy2 - ( 2 * x - cx2 ) ) / 2;
				break;
			}
		}
		if ( how == TS_FLIP_H || how == TS_FLIP_V ) {
			t = s;  s = e;  e = t;
		} else {
			INT	w = sh->r.right - sh->r.left;
			INT	h = sh->r.bottom - sh->r.top;

			nb.left = ( cx2 + 1 - h ) / 2;
			nb.top = ( cy2 + 1 - w ) / 2;
			nb.right = nb.left + h;
			nb.bottom = nb.top + w;
		}
		arc_put(rec_of(d), sh->node, &nb, &s, &e);
		return;
	}
	if ( of_points(sh) ) {
		T_DPPOINT	pt[TV_MAX_PT];
		INT		k;

		for ( k = 0; k < sh->npt && k < TV_MAX_PT; k++ ) {
			INT	x = sh->pt[k].x, y = sh->pt[k].y;

			switch ( how ) {
			case TS_FLIP_H:	pt[k].x = cx2 - x;  pt[k].y = y;  break;
			case TS_FLIP_V:	pt[k].x = x;  pt[k].y = cy2 - y;  break;
			case TS_ROT_R:
				pt[k].x = ( cx2 - ( 2 * y - cy2 ) ) / 2;
				pt[k].y = ( cy2 + ( 2 * x - cx2 ) ) / 2;
				break;
			default:
				pt[k].x = ( cx2 + ( 2 * y - cy2 ) ) / 2;
				pt[k].y = ( cy2 - ( 2 * x - cx2 ) ) / 2;
				break;
			}
		}
		pts_put(rec_of(d), sh, pt, sh->npt);
		return;
	}
	if ( how == TS_ROT_L || how == TS_ROT_R ) {
		/* a box turned a quarter round is the same box, wide for tall */
		T_DPRECT	nb;
		INT		w = sh->r.right - sh->r.left;
		INT		h = sh->r.bottom - sh->r.top;

		nb.left = ( cx2 + 1 - h ) / 2;
		nb.top = ( cy2 + 1 - w ) / 2;
		nb.right = nb.left + h;
		nb.bottom = nb.top + w;
		box_put(rec_of(d), sh, &nb);
	}
}

EXPORT void df_turn( DTWIN *d, INT how )
{
	INT	i;

	if ( df_picked(d) == 0 ) {
		return;
	}
	ed_before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && df_takeable(d, i) ) {
			turn(d, i, how);
		}
	}
	ed_changed(d);
}

/* ---------------------------------------------------------------- front and back */

#define TO_FRONT	1
#define TO_BACK		2
#define TO_FORWARD	3
#define TO_BACKWARD	4

/* A link that is 背景化: it stays behind everything else */
LOCAL BOOL is_bg( DTWIN *d, INT i )
{
	T_VOBJ	*v;
	BOOL	bg = FALSE;

	if ( d->fig->sh[i].kind != TV_SH_LINK || d->fig->sh[i].link < 0 ) {
		return FALSE;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v != NULL ) {
		bg = (BOOL)( tad_lnk_get(rec_of(d), d->fig->sh[i].link, v) >= E_OK && v->background );
		Kfree(v);
	}
	return bg;
}

/*
 * The order shapes are drawn in is their zIndex. To the front: each
 * picked one after the one in front of all, keeping their own order;
 * to the back, before the one behind all -- but in front of the links
 * that are 背景化, which stay behind everything and are not moved. One
 * step: a picked one changes places with the next unpicked one in front
 * of it, or behind, unless that one is 背景化.
 */
EXPORT void df_order( DTWIN *d, INT how )
{
	T_TAD	*rec = rec_of(d);
	INT	i, k, n = d->fig->nsh, top, bottom, nbg = 0;
	BOOL	*bg;

	if ( !df_is(d) || d->npick == 0 ) {
		return;
	}
	bg = (BOOL *)Kmalloc(sizeof(BOOL) * (SZ)( n + 1 ));
	if ( bg == NULL ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		bg[i] = is_bg(d, i);
		if ( bg[i] ) nbg++;
	}
	ed_before(d);
	top = bottom = ( n > 0 ) ? d->fig->sh[0].z : 0;
	for ( i = 0; i < n; i++ ) {
		if ( d->fig->sh[i].z > top )    top = d->fig->sh[i].z;
		if ( d->fig->sh[i].z < bottom ) bottom = d->fig->sh[i].z;
	}
	/* the shapes are in drawing order, back first */
	switch ( how ) {
	case TO_FRONT:
		for ( i = 0; i < n; i++ ) {
			if ( d->pick[i] != 0 && !bg[i] && d->fig->sh[i].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex", ++top);
			}
		}
		break;
	case TO_BACK:
		if ( nbg == 0 ) {
			for ( i = n - 1; i >= 0; i-- ) {
				if ( d->pick[i] != 0 && d->fig->sh[i].node != NULL ) {
					attr_put(rec, d->fig->sh[i].node, "zIndex", --bottom);
				}
			}
			break;
		}
		/* numbered again: the 背景化 ones, then the picked, then the rest */
		k = 0;
		for ( i = 0; i < n; i++ ) {
			if ( bg[i] && d->fig->sh[i].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex", ++k);
			}
		}
		for ( i = 0; i < n; i++ ) {
			if ( !bg[i] && d->pick[i] != 0 && d->fig->sh[i].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex", ++k);
			}
		}
		for ( i = 0; i < n; i++ ) {
			if ( !bg[i] && d->pick[i] == 0 && d->fig->sh[i].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex", ++k);
			}
		}
		break;
	case TO_FORWARD:
		for ( i = n - 2; i >= 0; i-- ) {
			if ( d->pick[i] == 0 || bg[i] || d->fig->sh[i].node == NULL ) {
				continue;
			}
			for ( k = i + 1; k < n && d->pick[k] != 0; k++ ) {
				;
			}
			if ( k < n && !bg[k] && d->fig->sh[k].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex",
					 d->fig->sh[k].z);
				attr_put(rec, d->fig->sh[k].node, "zIndex",
					 d->fig->sh[i].z);
				d->fig->sh[k].z = d->fig->sh[i].z;
			}
		}
		break;
	default:
		for ( i = 1; i < n; i++ ) {
			if ( d->pick[i] == 0 || bg[i] || d->fig->sh[i].node == NULL ) {
				continue;
			}
			for ( k = i - 1; k >= 0 && d->pick[k] != 0; k-- ) {
				;
			}
			if ( k >= 0 && !bg[k] && d->fig->sh[k].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex",
					 d->fig->sh[k].z);
				attr_put(rec, d->fig->sh[k].node, "zIndex",
					 d->fig->sh[i].z);
				d->fig->sh[k].z = d->fig->sh[i].z;
			}
		}
		break;
	}
	Kfree(bg);
	ed_pick_none(d);
	ed_changed(d);
}

/* ---------------------------------------------------------------- the tray */

#define PASTE_SHAPES	128

LOCAL T_TADNODE	*df_new[PASTE_SHAPES];	/* the elements the last paste made */
LOCAL INT	df_nnew = 0;

LOCAL BOOL is_link_elem( CONST T_TADNODE *c )
{
	CONST UB *n = c->name;

	return (BOOL)( n != NULL && n[0] == 'l' && n[1] == 'i' && n[2] == 'n'
		       && n[3] == 'k' && n[4] == 0 );
}

/*
 * The shapes taken, copied into the <figure> of a fragment for the tray.
 * A group is copied once, with everything in it. Links are the caller's
 * to add: they are counted where they go.
 */
EXPORT INT df_copy_into( DTWIN *d, T_TAD *frag )
{
	T_TADNODE	*body = tad_body(frag);
	INT		i, k, n = 0;

	if ( !df_is(d) || body == NULL ) {
		return 0;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;
		BOOL		seen = FALSE;

		if ( d->pick[i] == 0 || !df_takeable(d, i) ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		for ( k = 0; k < i; k++ ) {
			seen = (BOOL)( seen || ( d->pick[k] != 0 && df_takeable(d, k)
				&& top_of(d, d->fig->sh[k].node) == t ) );
		}
		if ( !seen && t != NULL && tad_node_copy(frag, body, NULL, t) != NULL ) {
			n++;
		}
	}
	return n;
}

/*
 * A copy of each shape taken, left where it is just under it: the
 * shapes themselves are then carried away, and the copies are what
 * stays behind. Links are copied as links to the same objects.
 */
EXPORT void df_dup_picked( DTWIN *d )
{
	T_TAD	*rec = rec_of(d);
	INT	i, k;

	if ( d->fig == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;
		BOOL		seen = FALSE;

		if ( d->pick[i] == 0 ) {
			continue;
		}
		if ( d->fig->sh[i].kind == TV_SH_LINK && d->fig->sh[i].link >= 0 ) {
			T_VOBJ	v;
			TS_UUID	id;

			if ( tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
				knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
				(void)tad_lnk_add(rec, &v, &id);
			}
			continue;
		}
		if ( !df_is(d) || !df_takeable(d, i) || d->fig->sh[i].node == NULL ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		/* a group once, with everything in it */
		for ( k = 0; k < i; k++ ) {
			seen = (BOOL)( seen || ( d->pick[k] != 0 && df_takeable(d, k)
				&& d->fig->sh[k].node != NULL
				&& top_of(d, d->fig->sh[k].node) == t ) );
		}
		if ( seen || t == NULL || t->parent == NULL ) {
			continue;
		}
		(void)tad_node_copy(rec, t->parent, t, t);	/* just under it */
	}
}

/* The shapes of a fragment from the tray put into an element of any record */
EXPORT INT df_frag_into( T_TAD *rec, T_TADNODE *parent, CONST T_TAD *frag )
{
	CONST T_TADNODE	*body = tad_body(frag), *c;
	INT		n = 0;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && !is_link_elem(c)
		  && tad_node_copy(rec, parent, NULL, c) != NULL ) {
			n++;
		}
	}
	return n;
}

/*
 * The shapes of a fragment from the tray put into this figure, moved
 * 'off' down and across. The model is not made again here; df_pick_pasted
 * takes what was put in once it has been.
 */
EXPORT INT df_paste_frag( DTWIN *d, CONST T_TAD *frag, INT off )
{
	CONST T_TADNODE	*fb = tad_body(frag), *c;
	T_TADNODE	*body;
	INT		i;

	df_nnew = 0;
	if ( !df_is(d) || fb == NULL ) {
		return 0;
	}
	body = tad_body(rec_of(d));
	if ( body == NULL ) {
		return 0;
	}
	for ( c = fb->first; c != NULL; c = c->next ) {
		T_TADNODE	*nd;

		if ( c->kind != TAD_ND_ELEM || is_link_elem(c) ) {
			continue;
		}
		nd = tad_node_copy(rec_of(d), body, NULL, c);
		if ( nd != NULL && df_nnew < PASTE_SHAPES ) {
			df_new[df_nnew++] = nd;
		}
	}
	if ( off != 0 && df_nnew > 0 ) {
		ed_model(d);
		for ( i = 0; i < d->fig->nsh; i++ ) {
			INT	k;

			for ( k = 0; k < df_nnew; k++ ) {
				if ( d->fig->sh[i].node != NULL
				  && top_of(d, d->fig->sh[i].node) == df_new[k] ) {
					df_shift(d, i, off, off);
				}
			}
		}
	}

	return df_nnew;
}

/*
 * A shape just written into the record taken up: the model made again,
 * the new shape picked and nothing else, and the window drawn.
 */
EXPORT void df_adopt( DTWIN *d, T_TADNODE *nd )
{
	ed_pick_none(d);
	df_new[0] = nd;
	df_nnew = 1;
	ed_model(d);
	df_pick_pasted(d);
	ed_changed(d);
}

EXPORT void df_pick_pasted( DTWIN *d )
{
	INT	i, k;

	for ( i = 0; df_is(d) && i < d->fig->nsh; i++ ) {
		for ( k = 0; k < df_nnew; k++ ) {
			if ( d->fig->sh[i].node != NULL && d->pick[i] == 0
			  && top_of(d, d->fig->sh[i].node) == df_new[k] ) {
				ed_pick_toggle(d, i);
			}
		}
	}
	df_nnew = 0;
}

/* ---------------------------------------------------------------- 位置あわせ, 色反転 */

/*
 * The picked shapes lined up with the one picked first: each group or
 * shape moved as a whole, across to the same left edge, middle or right
 * edge, and up and down to the same top, middle or foot.
 */
/* The box round every shape of the element a shape stands for */
LOCAL void unit_box( DTWIN *d, T_TADNODE *top, T_DPRECT *b )
{
	INT	k;
	BOOL	any = FALSE;

	for ( k = 0; k < d->fig->nsh; k++ ) {
		CONST T_DPRECT	*r = &d->fig->sh[k].r;

		if ( d->fig->sh[k].node == NULL
		  || top_of(d, d->fig->sh[k].node) != top ) {
			continue;
		}
		if ( !any ) {
			*b = *r;
			any = TRUE;
			continue;
		}
		if ( r->left < b->left )     b->left = r->left;
		if ( r->top < b->top )       b->top = r->top;
		if ( r->right > b->right )   b->right = r->right;
		if ( r->bottom > b->bottom ) b->bottom = r->bottom;
	}
}

LOCAL void align_picked( DTWIN *d )
{
	INT		h = 0, v = 0, i, first = ed_first_pick(d);
	T_DPRECT	ref;
	T_TADNODE	*ref_top;

	if ( first < 0 || d->npick < 2 || d->fig->sh[first].node == NULL
	  || !dt_align_form(d, &h, &v) || ( h == 0 && v == 0 ) ) {
		return;
	}
	ref_top = top_of(d, d->fig->sh[first].node);
	unit_box(d, ref_top, &ref);
	ed_before(d);
	/*
	 * Each group, or shape on its own, is lined up by its whole box and
	 * moved as one: the move is worked out once from the model as it
	 * was, then made to every shape of it.
	 */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*top;
		T_DPRECT	r;
		INT		dx = 0, dy = 0, k;
		BOOL		done = FALSE;

		if ( d->pick[i] == 0 || !df_takeable(d, i) ) {
			continue;
		}
		top = top_of(d, d->fig->sh[i].node);
		if ( top == ref_top ) {
			continue;
		}
		for ( k = 0; k < i; k++ ) {
			done = (BOOL)( done || ( d->pick[k] != 0
				&& d->fig->sh[k].node != NULL
				&& top_of(d, d->fig->sh[k].node) == top ) );
		}
		if ( done ) {
			continue;		/* its group was moved already */
		}
		unit_box(d, top, &r);
		switch ( h ) {
		case 1:	dx = ref.left - r.left;					break;
		case 2:	dx = ( ref.left + ref.right - r.left - r.right ) / 2;	break;
		case 3:	dx = ref.right - r.right;				break;
		default: break;
		}
		switch ( v ) {
		case 1:	dy = ref.top - r.top;					break;
		case 2:	dy = ( ref.top + ref.bottom - r.top - r.bottom ) / 2;	break;
		case 3:	dy = ref.bottom - r.bottom;				break;
		default: break;
		}
		for ( k = 0; k < d->fig->nsh; k++ ) {
			if ( d->fig->sh[k].node != NULL
			  && top_of(d, d->fig->sh[k].node) == top ) {
				df_shift(d, k, dx, dy);
			}
		}
	}
	ed_changed(d);
}

/* The colour a pattern number stands for, the other way up */
LOCAL void invert_attr( DTWIN *d, T_TADNODE *nd, CONST char *pat,
			CONST char *col_name, UW col )
{
	INT	id;

	if ( col == TAD_COL_NONE ) {
		return;				/* nothing drawn stays nothing */
	}
	col = ( ~col ) & 0x00FFFFFFU;
	id = tv_pattern_of(col);
	if ( id > 0 ) {
		attr_put(rec_of(d), nd, pat, id);
	} else {
		UB		w[8];
		CONST char	*hex = "0123456789abcdef";
		INT		k;

		/* no fixed pattern is that colour: the colour written out */
		w[0] = '#';
		for ( k = 0; k < 6; k++ ) {
			w[1 + k] = (UB)hex[( col >> ( 20 - k * 4 ) ) & 0xF];
		}
		w[7] = 0;
		(void)tad_set_attr(rec_of(d), nd, col_name, w);
	}
}

LOCAL void invert_picked( DTWIN *d )
{
	INT	i;

	if ( df_picked(d) == 0 ) {
		return;
	}
	ed_before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*sh = &d->fig->sh[i];

		if ( d->pick[i] == 0 || !df_takeable(d, i) ) {
			continue;
		}
		if ( sh->line_w > 0 ) {
			invert_attr(d, sh->node, "l_pat", "lineColor", sh->line_col);
		}
		invert_attr(d, sh->node, "f_pat", "fillColor", sh->fill_col);
	}
	ed_changed(d);
}

/* ---------------------------------------------------------------- the menu */

/*
 * The figure editor's own commands. The items it shares with the
 * cabinet (saving, the view, the clipboard, protection) are the
 * cabinet's commands and are done by its code.
 */
#define FC_PANEL	601
#define FC_ALL		602
#define FC_FRONT	603
#define FC_BACK		604
#define FC_FWD		605
#define FC_BWD		606
#define FC_FLIP_H	607
#define FC_FLIP_V	608
#define FC_ROT_L	609
#define FC_ROT_R	610
#define FC_GROUP	611
#define FC_UNGROUP	612
#define FC_ALIGN	613
#define FC_INVERT	614
#define FC_REALSIZE	615
#define FC_PATTERN	616
#define FC_TRANSFORM	617
#define FC_BURN		618
#define FC_PAPER	619
#define FC_PAGE_SETUP	620

typedef struct {
	CONST char	*code;
	INT		cmd;
} FIGCMD;

LOCAL CONST FIGCMD fig_cmds[] = {
	{ "panel",	FC_PANEL },
	{ "realsize",	FC_REALSIZE },
	{ "pattern",	FC_PATTERN },
	{ "group",	FC_GROUP },
	{ "ungroup",	FC_UNGROUP },
	{ "selectall",	FC_ALL },
	{ "front",	FC_FRONT },
	{ "back",	FC_BACK },
	{ "forward",	FC_FWD },
	{ "backward",	FC_BWD },
	{ "align",	FC_ALIGN },
	{ "transform",	FC_TRANSFORM },
	{ "invert",	FC_INVERT },
	{ "burn",	FC_BURN },
	{ "flip.h",	FC_FLIP_H },
	{ "flip.v",	FC_FLIP_V },
	{ "rot.l",	FC_ROT_L },
	{ "rot.r",	FC_ROT_R },
	{ "paper",	FC_PAPER },
	{ "pagesetup",	FC_PAGE_SETUP },
};

#define NFIGCMD	( (INT)( sizeof(fig_cmds) / sizeof(fig_cmds[0]) ) )

/* The items that act on what is taken, grey when nothing is */
LOCAL CONST char * CONST fig_on_pick[] = {
	"copy", "cut", "delete", "ungroup", "front", "back", "forward", "backward",
	"transform", "invert", "burn", "flip.h", "flip.v", "rot.l", "rot.r",
	"fix", "unfix", "bg", "unbg"
};

/* The figure editor's menu (FIGMENU.DEF), set to what the window is */
EXPORT ER df_menu_make( DTWIN *d, ID *p_mid )
{
	TS_UUID	def;
	BOOL	none = (BOOL)( d->npick == 0 );
	ID	mid;
	ER	er;
	INT	i;

	er = ts_str_to_uuid(SYSDEF_MENU_FIG, &def);
	if ( er >= E_OK ) {
		er = mn_cre_men(&def, &mid);
	}
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < (INT)( sizeof(fig_on_pick) / sizeof(fig_on_pick[0]) ); i++ ) {
		(void)mn_chg_atr(mid, NULL, fig_on_pick[i], none ? MN_GREY : 0);
	}
	(void)mn_chg_atr(mid, NULL, "undo", ( d->nundo == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "group", ( d->npick < 2 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "align", ( d->npick < 2 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "save.new", d->has_parent ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "hidden", d->show_hidden ? MN_TICK : 0);
	i = ( !d->sealed && dt_tray_kind(NULL) != DT_TRAY_NONE ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "paste", (UINT)i);
	(void)mn_chg_atr(mid, NULL, "moveback", (UINT)i);
	*p_mid = mid;

	return E_OK;
}

EXPORT void df_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			CONST T_VOBJ *v )
{
	INT	i, cmd = 0;

	for ( i = 0; i < NFIGCMD && cmd == 0; i++ ) {
		if ( mn_is(sel, fig_cmds[i].code) ) {
			cmd = fig_cmds[i].cmd;
		}
	}
	switch ( cmd ) {
	case FC_PANEL:		df_panel_toggle(d);		break;
	case FC_ALL:
		ed_pick_all(d);
		dt_draw(d);
		break;
	case FC_FRONT:		df_order(d, TO_FRONT);		break;
	case FC_BACK:		df_order(d, TO_BACK);		break;
	case FC_FWD:		df_order(d, TO_FORWARD);	break;
	case FC_BWD:		df_order(d, TO_BACKWARD);	break;
	case FC_FLIP_H:		df_turn(d, TS_FLIP_H);		break;
	case FC_FLIP_V:		df_turn(d, TS_FLIP_V);		break;
	case FC_ROT_L:		df_turn(d, TS_ROT_L);		break;
	case FC_ROT_R:		df_turn(d, TS_ROT_R);		break;
	case FC_GROUP:		group_picked(d);		break;
	case FC_UNGROUP:	ungroup_picked(d);		break;
	case FC_ALIGN:		align_picked(d);		break;
	case FC_INVERT:		invert_picked(d);		break;
	case FC_REALSIZE:	df_realsize_toggle(d);		break;
	case FC_PATTERN:	df_pattern_edit(d, df_fill_pattern());	break;
	case FC_TRANSFORM:	df_transform(d);		break;
	case FC_BURN:		df_burn(d);			break;
	case FC_PAPER:		df_paper_toggle(d);		break;
	case FC_PAGE_SETUP:	df_page_setup(d);		break;
	default:
		/* the items shared with the cabinet */
		cmd = cab_menu_cmd(sel);
		if ( cmd != 0 ) {
			dt_cab_command(d, cmd, on_vobj, v);
		}
		break;
	}
	wm_composite();
}

EXPORT void df_front( DTWIN *d, BOOL front )
{
	df_order(d, front ? TO_FRONT : TO_BACK);
}
