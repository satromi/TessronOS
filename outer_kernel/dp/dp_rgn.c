/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dp_rgn.c
 *	Regions: a shape that is not a rectangle (design 16.4)
 *
 *	A region is a list of bands. Each band covers a run of rows and
 *	holds a sorted list of [x0, x1) spans; the rows between bands hold
 *	nothing. A rectangle is the region of one band with one span, and
 *	that is the common case, so it costs one band and two numbers.
 *
 *	The form is kept normal at every step: bands in increasing y, no
 *	band touching the one above it with the same spans, no empty band,
 *	spans sorted and merged where they touch, and the bounding
 *	rectangle the smallest that covers the spans. Two regions built by
 *	different routes from the same shape are then the same region, and
 *	comparing them is comparing their bytes.
 *
 *	Every set operation is the same walk: take the row boundaries of
 *	both regions in order, and for each strip between two boundaries
 *	combine the span list each region has there. What differs between
 *	union, intersection and difference is only how two span lists are
 *	combined, which is three short functions.
 *
 *	A region is one block of memory. The caller owns what it makes and
 *	frees it; what is handed to a drawing environment is copied, for
 *	the reason a panel's definition is copied (design 16.2.3): a shape
 *	held by pointer is a shape that can be pulled away while it is
 *	being drawn through.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>

/*
 * What a region may hold. The window manager's use of these is the
 * uncovered parts of a window, which is a handful of bands; the caps are
 * high enough that nothing real meets them and low enough that a
 * runaway shape fails instead of eating the heap.
 */
#define RGN_MAX_BAND	256
#define RGN_MAX_SPAN	64		/* spans in one band */

typedef struct {
	INT	y0, y1;			/* the rows this band covers */
	INT	first;			/* where its spans start */
	INT	n;			/* how many */
} DPBAND;

struct dp_region {
	T_DPRECT	bound;
	INT		nband;
	INT		nspan;		/* pairs in span[] */
	DPBAND		*band;
	INT		*span;		/* 2n numbers: x0, x1 */
};

/* ---------------------------------------------------------------- making */

LOCAL T_DPRGN *rgn_alloc( INT nband, INT nspan )
{
	SZ	size = sizeof(T_DPRGN) + (SZ)nband * sizeof(DPBAND)
		       + (SZ)nspan * 2 * sizeof(INT);
	UB	*p = (UB *)Kmalloc(size);
	T_DPRGN	*r;

	if ( p == NULL ) {
		return NULL;
	}
	r = (T_DPRGN *)p;
	r->band = (DPBAND *)(p + sizeof(T_DPRGN));
	r->span = (INT *)((UB *)r->band + (SZ)nband * sizeof(DPBAND));
	r->nband = 0;
	r->nspan = 0;
	r->bound.left = r->bound.top = 0;
	r->bound.right = r->bound.bottom = 0;

	return r;
}

EXPORT void dp_rgn_free( T_DPRGN *rgn )
{
	if ( rgn != NULL ) {
		Kfree(rgn);
	}
}

/* The bounding rectangle, worked out from the bands that are there */
LOCAL void rgn_bound_set( T_DPRGN *r )
{
	INT	i, j;
	BOOL	any = FALSE;

	for ( i = 0; i < r->nband; i++ ) {
		DPBAND	*b = &r->band[i];

		for ( j = 0; j < b->n; j++ ) {
			INT	x0 = r->span[(b->first + j) * 2];
			INT	x1 = r->span[(b->first + j) * 2 + 1];

			if ( !any ) {
				r->bound.left = x0;
				r->bound.right = x1;
				r->bound.top = b->y0;
				r->bound.bottom = b->y1;
				any = TRUE;
			} else {
				if ( x0 < r->bound.left )   r->bound.left = x0;
				if ( x1 > r->bound.right )  r->bound.right = x1;
				if ( b->y0 < r->bound.top ) r->bound.top = b->y0;
				if ( b->y1 > r->bound.bottom ) r->bound.bottom = b->y1;
			}
		}
	}
	if ( !any ) {
		r->bound.left = r->bound.top = 0;
		r->bound.right = r->bound.bottom = 0;
	}
}

EXPORT ER dp_rgn_rect( CONST T_DPRECT *rect, T_DPRGN **p_rgn )
{
	T_DPRGN	*r;

	if ( rect == NULL || p_rgn == NULL ) {
		return E_PAR;
	}
	r = rgn_alloc(1, 1);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	if ( rect->right > rect->left && rect->bottom > rect->top ) {
		r->nband = 1;
		r->nspan = 1;
		r->band[0].y0 = rect->top;
		r->band[0].y1 = rect->bottom;
		r->band[0].first = 0;
		r->band[0].n = 1;
		r->span[0] = rect->left;
		r->span[1] = rect->right;
		r->bound = *rect;
	}
	*p_rgn = r;

	return E_OK;
}

EXPORT ER dp_rgn_copy( CONST T_DPRGN *src, T_DPRGN **p_rgn )
{
	T_DPRGN	*r;
	INT	i;

	if ( src == NULL || p_rgn == NULL ) {
		return E_PAR;
	}
	r = rgn_alloc(src->nband > 0 ? src->nband : 1,
		      src->nspan > 0 ? src->nspan : 1);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	r->nband = src->nband;
	r->nspan = src->nspan;
	r->bound = src->bound;
	for ( i = 0; i < src->nband; i++ ) {
		r->band[i] = src->band[i];
	}
	for ( i = 0; i < src->nspan * 2; i++ ) {
		r->span[i] = src->span[i];
	}
	*p_rgn = r;

	return E_OK;
}

/* ---------------------------------------------------------------- asking */

EXPORT ER dp_rgn_bound( CONST T_DPRGN *rgn, T_DPRECT *out )
{
	if ( rgn == NULL || out == NULL ) {
		return E_PAR;
	}
	*out = rgn->bound;

	return E_OK;
}

EXPORT BOOL dp_rgn_empty( CONST T_DPRGN *rgn )
{
	return ( rgn == NULL || rgn->nband == 0 );
}

/* The band covering a row, or NULL when no band does */
LOCAL CONST DPBAND *band_at( CONST T_DPRGN *rgn, INT y )
{
	INT	lo = 0, hi = rgn->nband - 1;

	while ( lo <= hi ) {
		INT	mid = (lo + hi) / 2;

		if ( y < rgn->band[mid].y0 ) {
			hi = mid - 1;
		} else if ( y >= rgn->band[mid].y1 ) {
			lo = mid + 1;
		} else {
			return &rgn->band[mid];
		}
	}

	return NULL;
}

/*
 * The spans of one row, for whoever is putting pixels down. The numbers
 * point into the region and stay put until it is freed.
 */
EXPORT INT dp_rgn_row( CONST T_DPRGN *rgn, INT y, CONST INT **p_spans )
{
	CONST DPBAND	*b;

	if ( rgn == NULL ) {
		return 0;
	}
	b = band_at(rgn, y);
	if ( b == NULL ) {
		return 0;
	}
	if ( p_spans != NULL ) {
		*p_spans = &rgn->span[b->first * 2];
	}

	return b->n;
}

EXPORT BOOL dp_rgn_has( CONST T_DPRGN *rgn, INT x, INT y )
{
	CONST INT	*sp = NULL;
	INT		n, i;

	n = dp_rgn_row(rgn, y, &sp);
	for ( i = 0; i < n; i++ ) {
		if ( x >= sp[i * 2] && x < sp[i * 2 + 1] ) {
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * A region from a run of rows, each with one span. Rows that say the
 * same thing become one band, which is what keeps the form normal: a
 * rounded box is a handful of bands at the corners and one tall band
 * down the middle, not one band per row.
 */
EXPORT ER dp_rgn_from_rows( INT y0, INT n, CONST INT *x0, CONST INT *x1,
			    T_DPRGN **p_out )
{
	T_DPRGN	*r;
	INT	i, nband = 0, nspan = 0;

	if ( x0 == NULL || x1 == NULL || p_out == NULL || n < 0 ) {
		return E_PAR;
	}
	r = rgn_alloc(RGN_MAX_BAND, RGN_MAX_BAND);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		if ( x1[i] <= x0[i] ) {
			continue;		/* a row with nothing in it */
		}
		if ( nband > 0
		  && r->band[nband - 1].y1 == y0 + i
		  && r->span[r->band[nband - 1].first * 2] == x0[i]
		  && r->span[r->band[nband - 1].first * 2 + 1] == x1[i] ) {
			r->band[nband - 1].y1 = y0 + i + 1;
			continue;
		}
		if ( nband >= RGN_MAX_BAND ) {
			break;
		}
		r->band[nband].y0 = y0 + i;
		r->band[nband].y1 = y0 + i + 1;
		r->band[nband].first = nspan;
		r->band[nband].n = 1;
		r->span[nspan * 2] = x0[i];
		r->span[nspan * 2 + 1] = x1[i];
		nspan++;
		nband++;
	}
	r->nband = nband;
	r->nspan = nspan;
	rgn_bound_set(r);
	*p_out = r;

	return E_OK;
}

/* ------------------------------------------------------- putting two together */

/*
 * The three ways two span lists are combined. Each reads two sorted
 * lists of [x0, x1) and writes a third, merging what touches; they are
 * the whole of the difference between union, intersection and
 * difference, everything else being the walk over rows.
 */
LOCAL INT span_add( INT *out, INT n, INT x0, INT x1 )
{
	if ( x1 <= x0 ) {
		return n;
	}
	if ( n > 0 && x0 <= out[(n - 1) * 2 + 1] ) {
		/* it touches the one before it, so they are one span */
		if ( x1 > out[(n - 1) * 2 + 1] ) {
			out[(n - 1) * 2 + 1] = x1;
		}
		return n;
	}
	if ( n >= RGN_MAX_SPAN ) {
		return n;			/* a shape too broken up to keep */
	}
	out[n * 2] = x0;
	out[n * 2 + 1] = x1;

	return n + 1;
}

LOCAL INT span_or( CONST INT *a, INT na, CONST INT *b, INT nb, INT *out )
{
	INT	i = 0, j = 0, n = 0;

	while ( i < na || j < nb ) {
		if ( j >= nb || ( i < na && a[i * 2] <= b[j * 2] ) ) {
			n = span_add(out, n, a[i * 2], a[i * 2 + 1]);
			i++;
		} else {
			n = span_add(out, n, b[j * 2], b[j * 2 + 1]);
			j++;
		}
	}

	return n;
}

LOCAL INT span_and( CONST INT *a, INT na, CONST INT *b, INT nb, INT *out )
{
	INT	i = 0, j = 0, n = 0;

	while ( i < na && j < nb ) {
		INT	x0 = ( a[i * 2] > b[j * 2] ) ? a[i * 2] : b[j * 2];
		INT	x1 = ( a[i * 2 + 1] < b[j * 2 + 1] )
			     ? a[i * 2 + 1] : b[j * 2 + 1];

		if ( x1 > x0 ) {
			n = span_add(out, n, x0, x1);
		}
		if ( a[i * 2 + 1] < b[j * 2 + 1] ) {
			i++;
		} else {
			j++;
		}
	}

	return n;
}

LOCAL INT span_sub( CONST INT *a, INT na, CONST INT *b, INT nb, INT *out )
{
	INT	i, j, n = 0;

	for ( i = 0; i < na; i++ ) {
		INT	x = a[i * 2];
		INT	end = a[i * 2 + 1];

		for ( j = 0; j < nb && x < end; j++ ) {
			INT	bx0 = b[j * 2], bx1 = b[j * 2 + 1];

			if ( bx1 <= x ) {
				continue;
			}
			if ( bx0 >= end ) {
				break;
			}
			if ( bx0 > x ) {
				n = span_add(out, n, x, bx0);
			}
			if ( bx1 > x ) {
				x = bx1;
			}
		}
		if ( x < end ) {
			n = span_add(out, n, x, end);
		}
	}

	return n;
}

#define RGN_OR	0
#define RGN_AND	1
#define RGN_SUB	2

/* The next row boundary at or after y, over both regions */
LOCAL INT next_edge( CONST T_DPRGN *a, CONST T_DPRGN *b, INT y, INT *p_end )
{
	INT	end = 0x7FFFFFFF;
	INT	i;

	for ( i = 0; i < a->nband; i++ ) {
		if ( a->band[i].y0 > y && a->band[i].y0 < end ) end = a->band[i].y0;
		if ( a->band[i].y1 > y && a->band[i].y1 < end ) end = a->band[i].y1;
	}
	for ( i = 0; i < b->nband; i++ ) {
		if ( b->band[i].y0 > y && b->band[i].y0 < end ) end = b->band[i].y0;
		if ( b->band[i].y1 > y && b->band[i].y1 < end ) end = b->band[i].y1;
	}
	*p_end = end;

	return ( end != 0x7FFFFFFF );
}

/* The first row either region has anything in, at or after y */
LOCAL BOOL first_edge( CONST T_DPRGN *a, CONST T_DPRGN *b, INT *p_y )
{
	INT	y = 0x7FFFFFFF;
	INT	i;

	for ( i = 0; i < a->nband; i++ ) {
		if ( a->band[i].y0 < y ) y = a->band[i].y0;
	}
	for ( i = 0; i < b->nband; i++ ) {
		if ( b->band[i].y0 < y ) y = b->band[i].y0;
	}
	*p_y = y;

	return ( y != 0x7FFFFFFF );
}

LOCAL ER rgn_op( CONST T_DPRGN *a, CONST T_DPRGN *b, UINT op, T_DPRGN **p_out )
{
	T_DPRGN	*r;
	INT	work[RGN_MAX_SPAN * 2];
	INT	y, end, n, i;
	INT	nband = 0, nspan = 0;
	CONST INT *sa, *sb;
	INT	na, nb;
	INT	empty[2];

	if ( a == NULL || b == NULL || p_out == NULL ) {
		return E_PAR;
	}
	empty[0] = empty[1] = 0;
	r = rgn_alloc(RGN_MAX_BAND, RGN_MAX_BAND * 4);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	if ( !first_edge(a, b, &y) ) {
		*p_out = r;			/* both were empty */
		return E_OK;
	}
	while ( next_edge(a, b, y, &end) ) {
		sa = empty;  na = dp_rgn_row(a, y, &sa);
		sb = empty;  nb = dp_rgn_row(b, y, &sb);
		if ( na == 0 ) sa = empty;
		if ( nb == 0 ) sb = empty;

		switch ( op ) {
		case RGN_AND:	n = span_and(sa, na, sb, nb, work);	break;
		case RGN_SUB:	n = span_sub(sa, na, sb, nb, work);	break;
		default:	n = span_or(sa, na, sb, nb, work);	break;
		}
		if ( n > 0 ) {
			BOOL	same = FALSE;

			/* a band with the same spans as the one above it,
			   and touching it, is that same band made longer */
			if ( nband > 0 && r->band[nband - 1].y1 == y
			  && r->band[nband - 1].n == n ) {
				INT	*prev = &r->span[r->band[nband - 1].first * 2];

				same = TRUE;
				for ( i = 0; i < n * 2; i++ ) {
					if ( prev[i] != work[i] ) {
						same = FALSE;
						break;
					}
				}
			}
			if ( same ) {
				r->band[nband - 1].y1 = end;
			} else if ( nband < RGN_MAX_BAND
				 && nspan + n <= RGN_MAX_BAND * 4 ) {
				r->band[nband].y0 = y;
				r->band[nband].y1 = end;
				r->band[nband].first = nspan;
				r->band[nband].n = n;
				for ( i = 0; i < n * 2; i++ ) {
					r->span[nspan * 2 + i] = work[i];
				}
				nspan += n;
				nband++;
			}
		}
		y = end;
	}
	r->nband = nband;
	r->nspan = nspan;
	rgn_bound_set(r);
	*p_out = r;

	return E_OK;
}

EXPORT ER dp_rgn_or( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out )
{
	return rgn_op(a, b, RGN_OR, p_out);
}

EXPORT ER dp_rgn_and( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out )
{
	return rgn_op(a, b, RGN_AND, p_out);
}

EXPORT ER dp_rgn_sub( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out )
{
	return rgn_op(a, b, RGN_SUB, p_out);
}

/* How many bands and spans a region came to: a test's way of seeing
 * that the normal form was kept. */
EXPORT ER dp_rgn_size( CONST T_DPRGN *rgn, INT *p_bands, INT *p_spans )
{
	if ( rgn == NULL ) {
		return E_PAR;
	}
	if ( p_bands != NULL ) {
		*p_bands = rgn->nband;
	}
	if ( p_spans != NULL ) {
		*p_spans = rgn->nspan;
	}

	return E_OK;
}
