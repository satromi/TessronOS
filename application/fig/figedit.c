/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figedit.c
 *	What the figure editor does to the shapes of a figure, apart from the
 *	window it is shown in (design 17.13)
 *
 *	A shape that is not a virtual object is worked on by changing the
 *	attributes of its element in the record. Colours are written as the
 *	fixed pattern numbers every record may use, so that what is drawn
 *	here reads back anywhere. What belongs to whoever shows the figure
 *	-- taking back, making the model again, what is picked -- is asked
 *	through T_FGHOST (include/ts/figedit.h), so the same code serves the
 *	figure editor of the desktop (application/desktop/dtfig.c) and the
 *	figure editor running as a process.
 */

#include <ts/tvport.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/fn.h>
#include <ts/mn.h>
#include <ts/figedit.h>

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

EXPORT void fe_attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v )
{
	attr_put(rec, nd, name, v);
}

EXPORT void fe_points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt,
			   INT n )
{
	points_put(rec, nd, pt, n);
}

/* ---------------------------------------------------------------- which */

EXPORT BOOL fe_is( CONST T_FGED *d )
{
	return (BOOL)( d != NULL && d->usable && d->fig != NULL );
}

/* A shape this editor works on that is not a virtual object */
EXPORT BOOL fe_takeable( CONST T_FGED *d, INT i )
{
	CONST T_TVSHAPE	*sh;

	if ( !fe_is(d) || i < 0 || i >= d->fig->nsh ) {
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
LOCAL T_TADNODE *top_of( T_FGED *d, T_TADNODE *nd )
{
	T_TADNODE	*body = tad_body(d->rec);

	while ( nd != NULL && nd->parent != NULL && nd->parent != body ) {
		nd = nd->parent;
	}

	return nd;
}

LOCAL BOOL in_group( T_FGED *d, INT i )
{
	return (BOOL)( d->fig->sh[i].node != NULL
		    && top_of(d, d->fig->sh[i].node) != d->fig->sh[i].node );
}

/*
 * Nodes kept by their addresses, each with two numbers: the groups or
 * shapes a picking reaches, looked up at once rather than by going
 * through all the shapes again for each one, which with many shapes
 * takes the square of their number.
 */
typedef struct {
	CONST void	**key;
	INT		*a, *b;
	INT		n;		/* slots: a power of two, at least twice what is kept */
} PMAP;

LOCAL BOOL pm_new( PMAP *m, INT want )
{
	INT	n = 64;

	while ( n < want * 2 + 16 ) {
		n *= 2;
	}
	m->key = (CONST void **)Kmalloc(( sizeof(void *) + sizeof(INT) * 2 ) * (SZ)n);
	if ( m->key == NULL ) {
		return FALSE;
	}
	knl_memset(m->key, 0, sizeof(void *) * (SZ)n);
	m->a = (INT *)( m->key + n );
	m->b = m->a + n;
	m->n = n;

	return TRUE;
}

LOCAL void pm_free( PMAP *m )
{
	if ( m->key != NULL ) {
		Kfree(m->key);
		m->key = NULL;
	}
}

/* The slot a node is in, or would go in */
LOCAL INT pm_slot( CONST PMAP *m, CONST void *k )
{
	UW	h = (UW)( (UBINT)k >> 4 ) * 0x9E3779B1U;
	INT	i = (INT)( h & (UW)( m->n - 1 ) );

	while ( m->key[i] != NULL && m->key[i] != k ) {
		i = ( i + 1 ) & ( m->n - 1 );
	}

	return i;
}

LOCAL BOOL pm_has( CONST PMAP *m, CONST void *k )
{
	return (BOOL)( k != NULL && m->key[pm_slot(m, k)] == k );
}

/* Kept, with its numbers; FALSE when it was kept already */
LOCAL BOOL pm_put( PMAP *m, CONST void *k, INT a, INT b )
{
	INT	i = pm_slot(m, k);

	if ( m->key[i] == k ) {
		return FALSE;
	}
	m->key[i] = k;
	m->a[i] = a;
	m->b[i] = b;

	return TRUE;
}

/*
 * Every shape of a group taken when any one of it is: what is taken is
 * the group. Called after any change to what is taken.
 */
EXPORT void fe_whole_groups( T_FGED *d )
{
	INT	i, groups = 0;
	PMAP	m;

	if ( !fe_is(d) ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && in_group(d, i) ) {
			groups++;
		}
	}
	if ( groups == 0 || !pm_new(&m, groups) ) {
		return;
	}
	/* the groups picked, each with the order it was picked in */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && in_group(d, i) ) {
			(void)pm_put(&m, top_of(d, d->fig->sh[i].node), (INT)d->pick[i], 0);
		}
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*top;

		if ( d->pick[i] != 0 || d->fig->sh[i].node == NULL ) {
			continue;
		}
		top = top_of(d, d->fig->sh[i].node);
		if ( pm_has(&m, top) ) {
			d->pick[i] = (UINT)m.a[pm_slot(&m, top)];
			d->npick++;
		}
	}
	pm_free(&m);
}

/* The picked shapes put into one group, in the place of the first */
EXPORT void fe_group( T_FGED *d )
{
	T_TAD		*rec = d->rec;
	T_TADNODE	*body = tad_body(rec), *g = NULL, **tops;
	INT		i, k, n = 0;
	PMAP		m;

	tops = (T_TADNODE **)Kmalloc(sizeof(T_TADNODE *) * (SZ)( d->fig->nsh + 1 ));
	if ( tops == NULL || !pm_new(&m, d->npick) ) {
		if ( tops != NULL ) Kfree(tops);
		d->host->no_memory(d);
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;

		if ( d->pick[i] == 0 || d->fig->sh[i].node == NULL ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		if ( pm_put(&m, t, 0, 0) ) {
			tops[n++] = t;
		}
	}
	pm_free(&m);
	if ( n < 2 || body == NULL ) {
		Kfree(tops);
		return;
	}
	d->host->before(d);
	g = tad_elem_new(rec, "group", body, tops[0], FALSE);
	if ( g == NULL ) {
		Kfree(tops);
		return;
	}
	for ( k = 0; k < n; k++ ) {
		tad_node_move(tops[k], g, NULL);
	}
	Kfree(tops);
	d->host->pick_none(d);
	d->host->changed(d);
}

/* The groups the picked shapes are in, taken apart: their shapes go back */
EXPORT void fe_ungroup( T_FGED *d )
{
	T_TAD		*rec = d->rec;
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
	d->host->before(d);
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
	d->host->pick_none(d);
	d->host->changed(d);
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
/*
 * Shape i put in box nb, what it is drawn by carried with it; with
 * FE_SIZE_FLIP_H or FE_SIZE_FLIP_V in 'flip', turned over that way in the
 * box as well: the points of a line or a polygon, and the ends of an arc
 * (which then runs the other way round), each mirrored across the box.
 * What is not drawn by points -- a box, an ellipse, a piece of text, a
 * link, a picture -- only takes the box.
 */
LOCAL void to_box_flip( T_FGED *d, INT i, CONST T_DPRECT *nb, UINT flip )
{
	CONST T_TVSHAPE	*sh = &d->fig->sh[i];
	INT		mx = nb->left + nb->right - 1, my = nb->top + nb->bottom - 1;

	if ( of_arc(sh) ) {
		T_DPPOINT	s, e, t;

		arc_ends_of(sh, &s, &e);
		s.x = map_x(s.x, &sh->r, nb);  s.y = map_y(s.y, &sh->r, nb);
		e.x = map_x(e.x, &sh->r, nb);  e.y = map_y(e.y, &sh->r, nb);
		if ( ( flip & FE_SIZE_FLIP_H ) != 0 ) {
			s.x = mx - s.x;
			e.x = mx - e.x;
		}
		if ( ( flip & FE_SIZE_FLIP_V ) != 0 ) {
			s.y = my - s.y;
			e.y = my - e.y;
		}
		if ( flip == FE_SIZE_FLIP_H || flip == FE_SIZE_FLIP_V ) {
			t = s;			/* turned over one way: the other way round */
			s = e;
			e = t;
		}
		arc_put(d->rec, sh->node, nb, &s, &e);
		return;
	}
	if ( of_points(sh) ) {
		T_DPPOINT	*pt = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * (SZ)( sh->npt + 1 ));
		INT		k, ow = sh->r.right - sh->r.left - 1;
		INT		oh = sh->r.bottom - sh->r.top - 1;
		INT		nw = nb->right - nb->left - 1;
		INT		nh = nb->bottom - nb->top - 1;

		if ( pt == NULL ) {
			d->host->no_memory(d);
			return;
		}
		for ( k = 0; k < sh->npt; k++ ) {
			pt[k].x = nb->left + ( ( ow > 0 )
				  ? ( sh->pt[k].x - sh->r.left ) * nw / ow : 0 );
			pt[k].y = nb->top + ( ( oh > 0 )
				  ? ( sh->pt[k].y - sh->r.top ) * nh / oh : 0 );
			if ( ( flip & FE_SIZE_FLIP_H ) != 0 ) pt[k].x = mx - pt[k].x;
			if ( ( flip & FE_SIZE_FLIP_V ) != 0 ) pt[k].y = my - pt[k].y;
		}
		pts_put(d->rec, sh, pt, sh->npt);
		Kfree(pt);
		return;
	}
	box_put(d->rec, sh, nb);
}

LOCAL void to_box( T_FGED *d, INT i, CONST T_DPRECT *nb )
{
	to_box_flip(d, i, nb, 0);
}

EXPORT void fe_shift( T_FGED *d, INT i, INT dx, INT dy )
{
	T_DPRECT	nb;

	if ( !fe_takeable(d, i) ) {
		return;
	}
	nb = d->fig->sh[i].r;
	nb.left += dx;  nb.right += dx;
	nb.top += dy;   nb.bottom += dy;
	to_box(d, i, &nb);
}

EXPORT void fe_resize( T_FGED *d, INT i, CONST T_DPRECT *nb )
{
	fe_resize_flip(d, i, nb, 0);
}

EXPORT void fe_resize_flip( T_FGED *d, INT i, CONST T_DPRECT *nb, UINT flip )
{
	if ( fe_takeable(d, i) && nb->right > nb->left && nb->bottom > nb->top ) {
		to_box_flip(d, i, nb, flip);
	}
}

/*
 * The box a shape is sized to by its handle: 'was' its box as the handle
 * was taken, its right and bottom edge carried to (cx, cy) -- across and
 * down say which of them the handle moves. The left and top edge stay;
 * carried past one, the box goes on to the other side of it, as far as
 * the handle went, and is turned over that way (*p_flip). Either way it
 * is no narrower than least_w and no lower than least_h.
 */
EXPORT void fe_size_box( CONST T_DPRECT *was, INT cx, INT cy, BOOL across, BOOL down,
			 INT least_w, INT least_h, T_DPRECT *nb, UINT *p_flip )
{
	INT	w, h;

	*nb = *was;
	*p_flip = 0;
	if ( across ) {
		w = cx - was->left;
		if ( w >= 0 ) {
			nb->right = was->left + ( ( w < least_w ) ? least_w : w );
		} else {
			w = -w;
			nb->right = was->left;
			nb->left = was->left - ( ( w < least_w ) ? least_w : w );
			*p_flip |= FE_SIZE_FLIP_H;
		}
	} else if ( nb->right < nb->left + least_w ) {
		nb->right = nb->left + least_w;
	}
	if ( down ) {
		h = cy - was->top;
		if ( h >= 0 ) {
			nb->bottom = was->top + ( ( h < least_h ) ? least_h : h );
		} else {
			h = -h;
			nb->bottom = was->top;
			nb->top = was->top - ( ( h < least_h ) ? least_h : h );
			*p_flip |= FE_SIZE_FLIP_V;
		}
	} else if ( nb->bottom < nb->top + least_h ) {
		nb->bottom = nb->top + least_h;
	}
}

/* ---------------------------------------------------------------- what is picked */

EXPORT INT fe_picked( CONST T_FGED *d )
{
	INT	i, n = 0;

	for ( i = 0; fe_is(d) && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && fe_takeable(d, i) ) {
			n++;
		}
	}

	return n;
}

EXPORT void fe_delete_picked( T_FGED *d )
{
	INT	i;

	for ( i = 0; fe_is(d) && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && fe_takeable(d, i) ) {
			/* a group goes whole; taking it out twice is nothing */
			tad_node_remove(top_of(d, d->fig->sh[i].node));
		}
	}
}

/*
 * Turned over, left to right or top to bottom, or turned a quarter
 * round, each shape about the middle of its own box.
 */
LOCAL void turn( T_FGED *d, INT i, INT how )
{
	CONST T_TVSHAPE	*sh = &d->fig->sh[i];
	INT		cx2 = sh->r.left + sh->r.right - 1;	/* twice the middle */
	INT		cy2 = sh->r.top + sh->r.bottom - 1;

	if ( sh->kind == TV_SH_IMAGE || sh->kind == TV_SH_PIXMAP ) {
		/* a picture keeps its box and says how it is turned */
		T_TAD		*rec = d->rec;
		CONST UB	*f;
		INT		rot = attr_num(sh->node, "rotation", 0);

		switch ( how ) {
		case FE_FLIP_H:
			f = tad_attr(sh->node, "flipH");
			(void)tad_set_attr(rec, sh->node, "flipH", (CONST UB *)
					   ( ( f != NULL && f[0] == 't' ) ? "false" : "true" ));
			break;
		case FE_FLIP_V:
			f = tad_attr(sh->node, "flipV");
			(void)tad_set_attr(rec, sh->node, "flipV", (CONST UB *)
					   ( ( f != NULL && f[0] == 't' ) ? "false" : "true" ));
			break;
		default:
			rot = ( rot + ( ( how == FE_ROT_R ) ? 90 : 270 ) ) % 360;
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
			case FE_FLIP_H:	p[k]->x = cx2 - x;  break;
			case FE_FLIP_V:	p[k]->y = cy2 - y;  break;
			case FE_ROT_R:
				p[k]->x = ( cx2 - ( 2 * y - cy2 ) ) / 2;
				p[k]->y = ( cy2 + ( 2 * x - cx2 ) ) / 2;
				break;
			default:
				p[k]->x = ( cx2 + ( 2 * y - cy2 ) ) / 2;
				p[k]->y = ( cy2 - ( 2 * x - cx2 ) ) / 2;
				break;
			}
		}
		if ( how == FE_FLIP_H || how == FE_FLIP_V ) {
			t = s;  s = e;  e = t;
		} else {
			INT	w = sh->r.right - sh->r.left;
			INT	h = sh->r.bottom - sh->r.top;

			nb.left = ( cx2 + 1 - h ) / 2;
			nb.top = ( cy2 + 1 - w ) / 2;
			nb.right = nb.left + h;
			nb.bottom = nb.top + w;
		}
		arc_put(d->rec, sh->node, &nb, &s, &e);
		return;
	}
	if ( of_points(sh) ) {
		T_DPPOINT	*pt = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * (SZ)( sh->npt + 1 ));
		INT		k;

		if ( pt == NULL ) {
			d->host->no_memory(d);
			return;
		}
		for ( k = 0; k < sh->npt; k++ ) {
			INT	x = sh->pt[k].x, y = sh->pt[k].y;

			switch ( how ) {
			case FE_FLIP_H:	pt[k].x = cx2 - x;  pt[k].y = y;  break;
			case FE_FLIP_V:	pt[k].x = x;  pt[k].y = cy2 - y;  break;
			case FE_ROT_R:
				pt[k].x = ( cx2 - ( 2 * y - cy2 ) ) / 2;
				pt[k].y = ( cy2 + ( 2 * x - cx2 ) ) / 2;
				break;
			default:
				pt[k].x = ( cx2 + ( 2 * y - cy2 ) ) / 2;
				pt[k].y = ( cy2 - ( 2 * x - cx2 ) ) / 2;
				break;
			}
		}
		pts_put(d->rec, sh, pt, sh->npt);
		Kfree(pt);
		return;
	}
	if ( how == FE_ROT_L || how == FE_ROT_R ) {
		/* a box turned a quarter round is the same box, wide for tall */
		T_DPRECT	nb;
		INT		w = sh->r.right - sh->r.left;
		INT		h = sh->r.bottom - sh->r.top;

		nb.left = ( cx2 + 1 - h ) / 2;
		nb.top = ( cy2 + 1 - w ) / 2;
		nb.right = nb.left + h;
		nb.bottom = nb.top + w;
		box_put(d->rec, sh, &nb);
	}
}

EXPORT void fe_turn( T_FGED *d, INT how )
{
	INT	i;

	if ( fe_picked(d) == 0 ) {
		return;
	}
	d->host->before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && fe_takeable(d, i) ) {
			turn(d, i, how);
		}
	}
	d->host->changed(d);
}

/* ---------------------------------------------------------------- front and back */

/* A link that is 背景化: it stays behind everything else */
LOCAL BOOL is_bg( T_FGED *d, INT i )
{
	T_VOBJ	*v;
	BOOL	bg = FALSE;

	if ( d->fig->sh[i].kind != TV_SH_LINK || d->fig->sh[i].link < 0 ) {
		return FALSE;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v != NULL ) {
		bg = (BOOL)( tad_lnk_get(d->rec, d->fig->sh[i].link, v) >= E_OK && v->background );
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
EXPORT void fe_order( T_FGED *d, INT how )
{
	T_TAD	*rec = d->rec;
	INT	i, k, n = d->fig->nsh, top, bottom, nbg = 0;
	BOOL	*bg;

	if ( !fe_is(d) || d->npick == 0 ) {
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
	d->host->before(d);
	top = bottom = ( n > 0 ) ? d->fig->sh[0].z : 0;
	for ( i = 0; i < n; i++ ) {
		if ( d->fig->sh[i].z > top )    top = d->fig->sh[i].z;
		if ( d->fig->sh[i].z < bottom ) bottom = d->fig->sh[i].z;
	}
	/* the shapes are in drawing order, back first */
	switch ( how ) {
	case FE_TO_FRONT:
		for ( i = 0; i < n; i++ ) {
			if ( d->pick[i] != 0 && !bg[i] && d->fig->sh[i].node != NULL ) {
				attr_put(rec, d->fig->sh[i].node, "zIndex", ++top);
			}
		}
		break;
	case FE_TO_BACK:
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
	case FE_TO_FORWARD:
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
	d->host->pick_none(d);
	d->host->changed(d);
}

/* ---------------------------------------------------------------- the tray */

LOCAL T_TADNODE	**fe_made = NULL;	/* the elements the last paste made */
LOCAL INT	fe_nmade = 0, fe_maxmade = 0;

/* Room for one more element a paste made */
LOCAL BOOL new_room( void )
{
	T_TADNODE	**n;
	INT		m;

	if ( fe_nmade < fe_maxmade ) {
		return TRUE;
	}
	m = ( fe_maxmade > 0 ) ? fe_maxmade * 2 : 64;
	n = (T_TADNODE **)Krealloc(fe_made, sizeof(T_TADNODE *) * (size_t)m);
	if ( n == NULL ) {
		return FALSE;
	}
	fe_made = n;
	fe_maxmade = m;

	return TRUE;
}

/* The elements the last paste made, kept by their addresses */
LOCAL BOOL new_map( PMAP *m )
{
	INT	k;

	if ( !pm_new(m, fe_nmade) ) {
		return FALSE;
	}
	for ( k = 0; k < fe_nmade; k++ ) {
		(void)pm_put(m, fe_made[k], 0, 0);
	}

	return TRUE;
}

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
EXPORT INT fe_copy_into( T_FGED *d, T_TAD *frag )
{
	T_TADNODE	*body = tad_body(frag);
	INT		i, n = 0;
	PMAP		m;

	if ( !fe_is(d) || body == NULL ) {
		return 0;
	}
	if ( !pm_new(&m, d->npick) ) {
		return -1;			/* not copied: no memory */
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;

		if ( d->pick[i] == 0 || !fe_takeable(d, i) ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		if ( t != NULL && pm_put(&m, t, 0, 0)
		  && tad_node_copy(frag, body, NULL, t) != NULL ) {
			n++;
		}
	}
	pm_free(&m);
	return n;
}

/*
 * A copy of each shape taken, left where it is just under it: the
 * shapes themselves are then carried away, and the copies are what
 * stays behind. Links are copied as links to the same objects.
 */
EXPORT void fe_dup_picked( T_FGED *d )
{
	T_TAD	*rec = d->rec;
	INT	i;
	PMAP	m;

	if ( d->fig == NULL ) {
		return;
	}
	if ( !pm_new(&m, d->npick) ) {
		d->host->no_memory(d);
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*t;

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
		if ( !fe_is(d) || !fe_takeable(d, i) || d->fig->sh[i].node == NULL ) {
			continue;
		}
		t = top_of(d, d->fig->sh[i].node);
		/* a group once, with everything in it */
		if ( t == NULL || t->parent == NULL || !pm_put(&m, t, 0, 0) ) {
			continue;
		}
		(void)tad_node_copy(rec, t->parent, t, t);	/* just under it */
	}
	pm_free(&m);
}

/* The shapes of a fragment from the tray put into an element of any record */
EXPORT INT fe_frag_into( T_TAD *rec, T_TADNODE *parent, CONST T_TAD *frag )
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
 * 'off' down and across. The model is not made again here; fe_pick_pasted
 * takes what was put in once it has been.
 */
EXPORT INT fe_paste_frag( T_FGED *d, CONST T_TAD *frag, INT off )
{
	CONST T_TADNODE	*fb = tad_body(frag), *c;
	T_TADNODE	*body;
	INT		i;

	fe_nmade = 0;
	if ( !fe_is(d) || fb == NULL ) {
		return 0;
	}
	body = tad_body(d->rec);
	if ( body == NULL ) {
		return 0;
	}
	for ( c = fb->first; c != NULL; c = c->next ) {
		T_TADNODE	*nd;

		if ( c->kind != TAD_ND_ELEM || is_link_elem(c) ) {
			continue;
		}
		nd = tad_node_copy(d->rec, body, NULL, c);
		if ( nd != NULL && new_room() ) {
			fe_made[fe_nmade++] = nd;
		}
	}
	if ( off != 0 && fe_nmade > 0 ) {
		PMAP	m;

		d->host->model(d);
		if ( new_map(&m) ) {
			for ( i = 0; i < d->fig->nsh; i++ ) {
				if ( d->fig->sh[i].node != NULL
				  && pm_has(&m, top_of(d, d->fig->sh[i].node)) ) {
					fe_shift(d, i, off, off);
				}
			}
			pm_free(&m);
		}
	}

	return fe_nmade;
}

/*
 * A shape just written into the record taken up: the model made again,
 * the new shape picked and nothing else, and the window drawn.
 */
EXPORT void fe_adopt( T_FGED *d, T_TADNODE *nd )
{
	d->host->pick_none(d);
	fe_nmade = 0;
	if ( new_room() ) {
		fe_made[fe_nmade++] = nd;
	}
	d->host->model(d);
	fe_pick_pasted(d);
	d->host->changed(d);
}

EXPORT void fe_pick_pasted( T_FGED *d )
{
	INT	i;
	PMAP	m;

	if ( fe_is(d) && fe_nmade > 0 && new_map(&m) ) {
		for ( i = 0; i < d->fig->nsh; i++ ) {
			if ( d->fig->sh[i].node != NULL && d->pick[i] == 0
			  && pm_has(&m, top_of(d, d->fig->sh[i].node)) ) {
				d->host->pick_add(d, i);
			}
		}
		pm_free(&m);
		fe_whole_groups(d);
	}
	fe_nmade = 0;
}

/* ---------------------------------------------------------------- 位置あわせ, 色反転 */

/*
 * The picked shapes lined up with the one picked first: each group or
 * shape moved as a whole, across to the same left edge, middle or right
 * edge, and up and down to the same top, middle or foot.
 */
/* The box round every shape of the element a shape stands for */
LOCAL void unit_box( T_FGED *d, T_TADNODE *top, T_DPRECT *b )
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

EXPORT void fe_align( T_FGED *d )
{
	INT		h = 0, v = 0, i, first = d->host->first_pick(d);
	T_DPRECT	ref;
	T_TADNODE	*ref_top;
	PMAP		m;

	if ( first < 0 || d->npick < 2 || d->fig->sh[first].node == NULL
	  || !d->host->align_form(d, &h, &v) || ( h == 0 && v == 0 ) ) {
		return;
	}
	ref_top = top_of(d, d->fig->sh[first].node);
	unit_box(d, ref_top, &ref);
	d->host->before(d);
	/*
	 * Each group, or shape on its own, is lined up by its whole box and
	 * moved as one: the move is worked out once from the model as it
	 * was, then made to every shape of it.
	 */
	if ( !pm_new(&m, d->npick) ) {
		d->host->no_memory(d);
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*top;
		T_DPRECT	r;
		INT		dx = 0, dy = 0;

		if ( d->pick[i] == 0 || !fe_takeable(d, i) ) {
			continue;
		}
		top = top_of(d, d->fig->sh[i].node);
		if ( top == ref_top || pm_has(&m, top) ) {
			continue;		/* the first, or its group worked out already */
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
		(void)pm_put(&m, top, dx, dy);
	}
	/* then every shape of each, by as much */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		T_TADNODE	*top;

		if ( d->fig->sh[i].node == NULL ) {
			continue;
		}
		top = top_of(d, d->fig->sh[i].node);
		if ( pm_has(&m, top) ) {
			INT	k = pm_slot(&m, top);

			fe_shift(d, i, m.a[k], m.b[k]);
		}
	}
	pm_free(&m);
	d->host->changed(d);
}

/* The colour a pattern number stands for, the other way up */
LOCAL void invert_attr( T_FGED *d, T_TADNODE *nd, CONST char *pat,
			CONST char *col_name, UW col )
{
	INT	id;

	if ( col == TAD_COL_NONE ) {
		return;				/* nothing drawn stays nothing */
	}
	col = ( ~col ) & 0x00FFFFFFU;
	id = tv_pattern_of(col);
	if ( id > 0 ) {
		attr_put(d->rec, nd, pat, id);
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
		(void)tad_set_attr(d->rec, nd, col_name, w);
	}
}

EXPORT void fe_invert( T_FGED *d )
{
	INT	i;

	if ( fe_picked(d) == 0 ) {
		return;
	}
	d->host->before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		CONST T_TVSHAPE	*sh = &d->fig->sh[i];

		if ( d->pick[i] == 0 || !fe_takeable(d, i) ) {
			continue;
		}
		if ( sh->line_w > 0 ) {
			invert_attr(d, sh->node, "l_pat", "lineColor", sh->line_col);
		}
		invert_attr(d, sh->node, "f_pat", "fillColor", sh->fill_col);
	}
	d->host->changed(d);
}

/* ---------------------------------------------------------------- 保護 */

EXPORT UINT fe_prot_can( CONST T_TAD *rec, CONST T_TVFIG *fig, CONST UINT *pick )
{
	T_VOBJ	*v;
	UINT	can = 0;
	INT	i;

	if ( rec == NULL || fig == NULL || pick == NULL ) {
		return 0;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) {
		return 0;
	}
	for ( i = 0; i < fig->nsh; i++ ) {
		if ( pick[i] == 0 || fig->sh[i].kind != TV_SH_LINK || fig->sh[i].link < 0
		  || tad_lnk_get(rec, fig->sh[i].link, v) < E_OK ) {
			continue;
		}
		can |= v->fixed ? FE_PROT_UNFIX : FE_PROT_FIX;
		can |= v->background ? FE_PROT_UNBG : FE_PROT_BG;
	}
	Kfree(v);

	return can;
}

EXPORT void fe_prot_grey( ID mid, UINT can )
{
	(void)mn_chg_atr(mid, NULL, "fix", ( can & FE_PROT_FIX ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "unfix", ( can & FE_PROT_UNFIX ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "bg", ( can & FE_PROT_BG ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "unbg", ( can & FE_PROT_UNBG ) ? 0 : MN_GREY);
}

/* ---------------------------------------------------------------- 整頓 */

/*
 * How tall a link is drawn: a closed one is as tall as its band whatever
 * its box says (a box no more than OM_OPEN_SLACK taller than the band is
 * closed), an opened one its whole box.
 */
EXPORT INT fe_shown_h( CONST T_VOBJ *v )
{
	INT	h = v->bottom - v->top, band = om_band_h(v);

	return ( h <= band + OM_OPEN_SLACK ) ? band : h;
}

/* The foot of a box as it is drawn */
LOCAL INT ar_foot( CONST T_FEARBOX *b )
{
	return b->r.top + b->shown;
}

/*
 * The room left between two boxes one under the other. 詰める leaves the
 * room it always does (FE_AR_GAP); 上揃え the room there is now between
 * the topmost box and the one next below it, so that all of them come
 * down the page as those two stand; with neither, the room between rows
 * of columns (FE_AR_ROOM).
 */
EXPORT INT fe_arrange_gap( CONST T_FEARBOX *b, INT n, INT vertical )
{
	INT	i, first = -1, second = -1, gap;

	if ( vertical == FE_AR_COMPACT ) {
		return FE_AR_GAP;
	}
	if ( vertical != FE_AR_ALIGN ) {
		return FE_AR_ROOM;
	}
	for ( i = 0; i < n; i++ ) {
		if ( first < 0 || b[i].r.top < b[first].r.top
		  || ( b[i].r.top == b[first].r.top && b[i].r.left < b[first].r.left ) ) {
			second = first;
			first = i;
		} else if ( second < 0 || b[i].r.top < b[second].r.top
			 || ( b[i].r.top == b[second].r.top && b[i].r.left < b[second].r.left ) ) {
			second = i;
		}
	}
	if ( second < 0 ) {
		return FE_AR_GAP;
	}
	gap = b[second].r.top - ar_foot(&b[first]);

	return ( gap > 0 ) ? gap : 0;	/* two that overlap are put touching */
}

/* A box put with its left edge, or with its right edge at 'right' */
LOCAL void ar_across( T_FEARBOX *b, INT how, INT left, INT right )
{
	INT	w = b->r.right - b->r.left;

	if ( how == FE_AR_RIGHT ) {
		b->r.right = right;
		b->r.left = right - w;
	} else if ( how == FE_AR_LEFT ) {
		b->r.left = left;
		b->r.right = left + w;
	}
}

/* A box put with its top at 'top', keeping its height */
LOCAL void ar_down( T_FEARBOX *b, INT top )
{
	INT	h = b->r.bottom - b->r.top;

	b->r.top = top;
	b->r.bottom = top + h;
}

/*
 * 整頓: the boxes laid out in the order given, each keeping its size.
 * One column runs down from the top; columns side by side are filled a
 * row at a time (左右) or a column at a time (上下), 10 apart; with no
 * columns, the boxes taken from the top down are only lined up. Down
 * the page, each box starts the room (fe_arrange_gap) below the foot of
 * the one above as it is drawn, not as its box says, so that a closed
 * link whose box is a few pixels taller than its band does not leave
 * more room under it than the others.
 */
EXPORT void fe_arrange_lay( T_FEARBOX *b, INT n, CONST T_FEARLAY *o )
{
	INT		i, k, gap, cols, per, c;
	T_FEARBOX	tmp;

	if ( n < 2 ) {
		return;
	}
	gap = fe_arrange_gap(b, n, o->vertical);
	if ( o->column == FE_AR_SINGLE ) {
		INT	at = o->top;

		for ( i = 0; i < n; i++ ) {
			ar_across(&b[i], ( o->horizontal == FE_AR_RIGHT ) ? FE_AR_RIGHT : FE_AR_LEFT,
				  o->left, o->right);
			ar_down(&b[i], at);
			at = ar_foot(&b[i]) + gap;
		}
		return;
	}
	if ( o->column == FE_AR_MULTI_H || o->column == FE_AR_MULTI_V ) {
		INT	colw[FE_AR_COLS_MAX], coll[FE_AR_COLS_MAX];

		cols = ( o->columns > 0 ) ? o->columns : 2;
		if ( cols > FE_AR_COLS_MAX ) {
			cols = FE_AR_COLS_MAX;
		}
		per = ( n + cols - 1 ) / cols;
		for ( k = 0; k < cols; k++ ) {
			colw[k] = 0;
		}
		for ( i = 0; i < n; i++ ) {
			INT	w = b[i].r.right - b[i].r.left;

			c = ( o->column == FE_AR_MULTI_H ) ? i % cols : i / per;
			if ( c < cols && w > colw[c] ) {
				colw[c] = w;
			}
		}
		coll[0] = o->left;
		for ( k = 1; k < cols; k++ ) {
			coll[k] = coll[k - 1] + colw[k - 1] + FE_AR_ROOM;
		}
		if ( o->column == FE_AR_MULTI_H ) {
			/* left to right, then down a row below the lowest of it */
			INT	row_top = o->top, row_foot = o->top;

			for ( i = 0; i < n; i++ ) {
				c = i % cols;
				if ( c == 0 && i > 0 ) {
					row_top = row_foot + gap;
				}
				ar_across(&b[i], FE_AR_LEFT, coll[c], coll[c]);
				ar_down(&b[i], row_top);
				if ( c == 0 || ar_foot(&b[i]) > row_foot ) {
					row_foot = ar_foot(&b[i]);
				}
			}
		} else {
			/* down a column, then the next column */
			for ( i = 0; i < n; i++ ) {
				c = i / per;
				ar_across(&b[i], FE_AR_LEFT, coll[c], coll[c]);
				ar_down(&b[i], ( i % per == 0 ) ? o->top : ar_foot(&b[i - 1]) + gap);
			}
		}
		return;
	}
	if ( o->horizontal == FE_AR_NONE && o->vertical == FE_AR_NONE ) {
		return;
	}
	/* only lined up: taken from the top down, the topmost staying where it is */
	for ( i = 1; i < n; i++ ) {
		for ( k = i; k > 0 && b[k - 1].r.top > b[k].r.top; k-- ) {
			tmp = b[k];  b[k] = b[k - 1];  b[k - 1] = tmp;
		}
	}
	for ( i = 0; i < n; i++ ) {
		ar_across(&b[i], o->horizontal, o->left, o->right);
		if ( i > 0 && o->vertical != FE_AR_NONE ) {
			ar_down(&b[i], ar_foot(&b[i - 1]) + gap);
		}
	}
}

LOCAL INT ar_cmp_text( CONST UB *a, CONST UB *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}

	return (INT)*a - (INT)*b;
}

/* How the objects of two links compare for 整列順 */
LOCAL INT ar_cmp( CONST T_FEARINFO *ia, CONST T_FEARINFO *ib, INT by )
{
	switch ( by ) {
	case FE_AR_BY_NAME:	return ar_cmp_text(ia->name, ib->name);
	case FE_AR_BY_MADE:	return ar_cmp_text(ia->made, ib->made);
	case FE_AR_BY_UPDATED:	return ar_cmp_text(ia->updated, ib->updated);
	case FE_AR_BY_SIZE:	return ia->bytes - ib->bytes;
	default:		return 0;
	}
}

/*
 * How wide a link's frame needs to be to show what 幅調整 asks for:
 * its pictogram and name, then its relationship, then what program it
 * opens with, then the date -- each step taking in what the one before
 * it did. The letters are measured in the system's face at the link's
 * size.
 */
LOCAL INT ar_width( CONST T_VOBJ *v, CONST T_FEARINFO *inf, INT what )
{
	ID	fid = fn_system();
	INT	chsz = ( v->chsz > 0 ) ? v->chsz : 14;
	INT	w = 10 + chsz + 4, i, k;
	UB	text[TAD_NAME_MAX + 8];

	if ( fid <= 0 ) {
		return v->right - v->left;
	}
	fn_set_size(fid, chsz);
	if ( inf->has_label ) {
		w += fn_width(fid, inf->label);
	}
	if ( what != FE_AR_W_NAME && inf->rel[0] != 0 ) {
		k = 0;
		text[k++] = ' ';  text[k++] = ':';  text[k++] = ' ';
		for ( i = 0; inf->rel[i] != 0 && k < TAD_NAME_MAX; i++ ) {
			if ( inf->rel[i] != '[' && inf->rel[i] != ']' ) {
				text[k++] = inf->rel[i];
			}
		}
		text[k] = 0;
		w += fn_width(fid, text);
	}
	if ( ( what == FE_AR_W_NODATE || what == FE_AR_W_FULL ) && inf->has_app ) {
		k = 0;
		text[k++] = ' ';  text[k++] = '(';
		for ( i = 0; inf->app[i] != 0 && k < TAD_NAME_MAX; i++ ) {
			text[k++] = inf->app[i];
		}
		text[k++] = ')';
		text[k] = 0;
		w += fn_width(fid, text);
	}
	if ( what == FE_AR_W_FULL ) {
		w += fn_width(fid, (CONST UB *)" YYYY/MM/DD HH:MM:SS");
	}
	fn_set_size(fid, 14);

	return w + 10;
}

EXPORT INT fe_arrange( T_FEARITEM *it, INT n, CONST T_FEARRANGE *o, FE_ARINFO info, void *arg )
{
	T_FEARITEM	tmp;
	T_FEARINFO	*inf = NULL, ti;
	T_FEARBOX	*box;
	T_FEARLAY	lay;
	INT		i, k, m = 0, top, left, right;

	/* one that is 固定化 or 背景化 stays where it is */
	for ( i = 0; i < n; i++ ) {
		if ( !it[i].v.fixed && !it[i].v.background ) {
			if ( m != i ) {
				it[m] = it[i];
			}
			m++;
		}
	}
	n = m;
	if ( n == 0 ) {
		return 0;
	}
	/* in the order they were taken */
	for ( i = 1; i < n; i++ ) {
		for ( k = i; k > 0 && it[k - 1].seq > it[k].seq; k-- ) {
			tmp = it[k];  it[k] = it[k - 1];  it[k - 1] = tmp;
		}
	}
	top = it[0].v.top;  left = it[0].v.left;  right = it[0].v.right;
	for ( i = 1; i < n; i++ ) {
		if ( it[i].v.top < top )     top = it[i].v.top;
		if ( it[i].v.left < left )   left = it[i].v.left;
		if ( it[i].v.right > right ) right = it[i].v.right;
	}

	/* what the objects are, asked once each, when the width or the order looks at it */
	if ( o->sort_by != FE_AR_NONE || ( o->width != FE_AR_NONE && o->width != FE_AR_W_FIRST ) ) {
		inf = (T_FEARINFO *)Kmalloc(sizeof(T_FEARINFO) * (SZ)n);
		if ( inf == NULL ) {
			return 0;
		}
		for ( i = 0; i < n; i++ ) {
			knl_memset(&inf[i], 0, sizeof(T_FEARINFO));
			if ( info != NULL ) {
				(void)info(arg, &it[i].v.target, &inf[i]);
			}
		}
	}

	/* 幅調整, before anything is sorted: 最初 is the one taken first */
	if ( o->width != FE_AR_NONE ) {
		INT	first_w = it[0].v.right - it[0].v.left;

		for ( i = 0; i < n; i++ ) {
			INT	w = ( o->width == FE_AR_W_FIRST ) ? first_w
				  : ar_width(&it[i].v, &inf[i], o->width);

			it[i].v.right = it[i].v.left + w;
		}
	}

	if ( n > 1 && o->sort_by != FE_AR_NONE ) {
		for ( i = 1; i < n; i++ ) {
			for ( k = i; k > 0; k-- ) {
				INT	c = ar_cmp(&inf[k - 1], &inf[k], o->sort_by);

				if ( o->descending ) {
					c = -c;
				}
				if ( c <= 0 ) {
					break;
				}
				tmp = it[k];  it[k] = it[k - 1];  it[k - 1] = tmp;
				ti = inf[k];  inf[k] = inf[k - 1];  inf[k - 1] = ti;
			}
		}
	}
	if ( inf != NULL ) {
		Kfree(inf);
	}

	lay.horizontal = o->horizontal;
	lay.vertical = o->vertical;
	lay.column = o->column;
	if ( o->sort_by != FE_AR_NONE && lay.column == FE_AR_NONE
	  && lay.horizontal == FE_AR_NONE && lay.vertical == FE_AR_NONE ) {
		lay.column = FE_AR_SINGLE;	/* an order asked for is a column */
	}
	lay.columns = o->columns;
	lay.top = top;
	lay.left = left;
	lay.right = right;
	box = (T_FEARBOX *)Kmalloc(sizeof(T_FEARBOX) * (SZ)n);
	if ( box != NULL ) {
		for ( i = 0; i < n; i++ ) {
			box[i].r.left = it[i].v.left;
			box[i].r.top = it[i].v.top;
			box[i].r.right = it[i].v.right;
			box[i].r.bottom = it[i].v.bottom;
			box[i].shown = fe_shown_h(&it[i].v);
			box[i].id = i;
		}
		fe_arrange_lay(box, n, &lay);
		for ( i = 0; i < n; i++ ) {
			T_VOBJ	*v = &it[box[i].id].v;

			v->left = box[i].r.left;
			v->top = box[i].r.top;
			v->right = box[i].r.right;
			v->bottom = box[i].r.bottom;
		}
		Kfree(box);
	}
	for ( i = 0; i < n; i++ ) {
		it[i].v.height = it[i].v.bottom - it[i].v.top;
	}

	return n;
}
