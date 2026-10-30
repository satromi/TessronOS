/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sk_draw.c
 *	ソフトウェアキーボード: shapes filled with their edges smoothed
 *	(design 11.31)
 *
 *	The keys drawn by shape and the marks on them are outlines of many
 *	points, turned and curved, which a fill pixel by pixel draws with
 *	steps along every edge that is not upright or level. Here each pixel
 *	is given the colour in the share the shape covers of it: every row
 *	of pixels is cut by four lines across, the crossings of the edges
 *	with each line are taken in pairs (even-odd) or by winding, and each
 *	stretch between two crossings adds to the pixels it passes over as
 *	much as it covers of each, the part pixels at its ends their part.
 *	The points are in sixteenths of a pixel, so that a shape may stand
 *	anywhere between pixels and still be drawn the same.
 *
 *	The pixels are those of the window's surface (wm_obj_surf), drawn in
 *	by the program and put in the window afterwards.
 */

#include <string.h>
#include "sk.h"

#define SUB		4			/* lines across a row of pixels */
#define EDGE_MAX	4096
#define CROSS_MAX	512
#define ROW_MAX		4096

typedef struct {
	INT	x0, y0, x1, y1;			/* y0 < y1 */
	INT	dir;				/* +1 down, -1 up */
} EDGE;

static EDGE	e_tab[EDGE_MAX];
static INT	acc[ROW_MAX + 2];		/* the share of each pixel of the row covered, of SUB * SK_FIX */

typedef struct {
	INT	x, dir;
} CROSS;

static CROSS	c_tab[CROSS_MAX];

static void blend( UINT *d, UINT c, INT a, INT full )
{
	UINT	o = *d;
	INT	r = (INT)( ( o >> 16 ) & 0xFF ), g = (INT)( ( o >> 8 ) & 0xFF ), b = (INT)( o & 0xFF );
	INT	R = (INT)( ( c >> 16 ) & 0xFF ), G = (INT)( ( c >> 8 ) & 0xFF ), B = (INT)( c & 0xFF );

	if ( a >= full ) {
		*d = c & 0x00FFFFFFU;
		return;
	}
	r += ( R - r ) * a / full;
	g += ( G - g ) * a / full;
	b += ( B - b ) * a / full;
	*d = (UINT)( ( r << 16 ) | ( g << 8 ) | b );
}

/* A stretch of a line across, from xa to xb (sixteenths), added to the row */
static void span( INT xa, INT xb, INT lo, INT hi )
{
	INT	pa, pb, p;

	if ( xa < lo * SK_FIX ) xa = lo * SK_FIX;
	if ( xb > hi * SK_FIX ) xb = hi * SK_FIX;
	if ( xb <= xa ) return;
	pa = xa / SK_FIX;
	pb = ( xb - 1 ) / SK_FIX;
	if ( pa == pb ) {
		acc[pa - lo] += xb - xa;
		return;
	}
	acc[pa - lo] += ( pa + 1 ) * SK_FIX - xa;
	for ( p = pa + 1; p < pb; p++ ) acc[p - lo] += SK_FIX;
	acc[pb - lo] += xb - pb * SK_FIX;
}

EXPORT void sk_fill( SKSURF *s, CONST INT *xy, CONST INT *cn, INT ncn, UINT rgb, BOOL evenodd,
		     CONST T_DPRECT *clip )
{
	INT	ne = 0, i, k, at = 0, minx = 0x7FFFFFFF, maxx = -0x7FFFFFFF, miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF;
	INT	lo, hi, top, bot, y, sub;

	for ( i = 0; i < ncn; i++ ) {
		INT	n = cn[i];

		for ( k = 0; k < n; k++ ) {
			INT	x0 = xy[( at + k ) * 2], y0 = xy[( at + k ) * 2 + 1];
			INT	x1 = xy[( at + ( k + 1 ) % n ) * 2], y1 = xy[( at + ( k + 1 ) % n ) * 2 + 1];

			if ( x0 < minx ) minx = x0;
			if ( x0 > maxx ) maxx = x0;
			if ( y0 < miny ) miny = y0;
			if ( y0 > maxy ) maxy = y0;
			if ( y0 == y1 || ne >= EDGE_MAX ) continue;
			if ( y0 < y1 ) {
				e_tab[ne].x0 = x0; e_tab[ne].y0 = y0; e_tab[ne].x1 = x1; e_tab[ne].y1 = y1;
				e_tab[ne].dir = 1;
			} else {
				e_tab[ne].x0 = x1; e_tab[ne].y0 = y1; e_tab[ne].x1 = x0; e_tab[ne].y1 = y0;
				e_tab[ne].dir = -1;
			}
			ne++;
		}
		at += n;
	}
	if ( ne == 0 ) return;
	lo = minx / SK_FIX;
	hi = ( maxx + SK_FIX - 1 ) / SK_FIX + 1;
	top = miny / SK_FIX;
	bot = ( maxy + SK_FIX - 1 ) / SK_FIX + 1;
	if ( lo < clip->left ) lo = clip->left;
	if ( lo < 0 ) lo = 0;
	if ( hi > clip->right ) hi = clip->right;
	if ( hi > s->w ) hi = s->w;
	if ( top < clip->top ) top = clip->top;
	if ( top < 0 ) top = 0;
	if ( bot > clip->bottom ) bot = clip->bottom;
	if ( bot > s->h ) bot = s->h;
	if ( hi <= lo || bot <= top || hi - lo > ROW_MAX ) return;

	for ( y = top; y < bot; y++ ) {
		UINT	*row = s->px + (UD)y * (UD)s->pitch;
		BOOL	any = FALSE;

		memset(acc, 0, sizeof(acc[0]) * (size_t)( hi - lo + 1 ));
		for ( sub = 0; sub < SUB; sub++ ) {
			INT	yy = y * SK_FIX + sub * ( SK_FIX / SUB ) + SK_FIX / SUB / 2;
			INT	nc = 0, w, j;

			for ( i = 0; i < ne && nc < CROSS_MAX; i++ ) {
				EDGE	*e = &e_tab[i];

				if ( yy < e->y0 || yy >= e->y1 ) continue;
				c_tab[nc].x = e->x0 + (INT)( (D)( yy - e->y0 ) * ( e->x1 - e->x0 ) / ( e->y1 - e->y0 ) );
				c_tab[nc].dir = e->dir;
				nc++;
			}
			/* in order along the line */
			for ( i = 1; i < nc; i++ ) {
				CROSS	t = c_tab[i];

				for ( j = i - 1; j >= 0 && c_tab[j].x > t.x; j-- ) c_tab[j + 1] = c_tab[j];
				c_tab[j + 1] = t;
			}
			w = 0;
			for ( i = 0; i < nc; i++ ) {
				BOOL	in_before = evenodd ? ( ( i & 1 ) != 0 ) : ( w != 0 );

				w += c_tab[i].dir;
				if ( !evenodd ) {
					BOOL	in_after = (BOOL)( w != 0 );

					if ( !in_before && in_after ) j = i;
					if ( in_before && !in_after ) {
						span(c_tab[j].x, c_tab[i].x, lo, hi);
						any = TRUE;
					}
				} else if ( ( i & 1 ) == 1 ) {
					span(c_tab[i - 1].x, c_tab[i].x, lo, hi);
					any = TRUE;
				}
			}
		}
		if ( !any ) continue;
		for ( k = lo; k < hi; k++ ) {
			if ( acc[k - lo] > 0 ) blend(&row[k], rgb, acc[k - lo], SK_FIX * SUB);
		}
	}
}

EXPORT void sk_rect( SKSURF *s, CONST T_DPRECT *r, UINT rgb )
{
	INT	x, y, l = r->left, t = r->top, rr = r->right, b = r->bottom;

	if ( l < 0 ) l = 0;
	if ( t < 0 ) t = 0;
	if ( rr > s->w ) rr = s->w;
	if ( b > s->h ) b = s->h;
	for ( y = t; y < b; y++ ) {
		UINT	*row = s->px + (UD)y * (UD)s->pitch;

		for ( x = l; x < rr; x++ ) row[x] = rgb;
	}
}

/* A square root, by Newton's steps: the program has no mathematics library */
static double root( double v )
{
	double	r = ( v > 1.0 ) ? v : 1.0;
	INT	i;

	if ( v <= 0.0 ) return 0.0;
	for ( i = 0; i < 40; i++ ) {
		double	n = 0.5 * ( r + v / r );

		if ( n >= r ) break;
		r = n;
	}
	return r;
}

EXPORT void sk_inset( CONST INT *xy, INT n, INT d, INT *out )
{
	double	area = 0, sgn;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		INT	j = ( i + 1 ) % n;

		area += (double)xy[i * 2] * xy[j * 2 + 1] - (double)xy[j * 2] * xy[i * 2 + 1];
	}
	sgn = ( area > 0 ) ? 1.0 : -1.0;
	for ( i = 0; i < n; i++ ) {
		INT	a = ( i + n - 1 ) % n, c = ( i + 1 ) % n;
		double	ax = xy[a * 2], ay = xy[a * 2 + 1], bx = xy[i * 2], by = xy[i * 2 + 1];
		double	cx = xy[c * 2], cy = xy[c * 2 + 1];
		double	l1 = root(( bx - ax ) * ( bx - ax ) + ( by - ay ) * ( by - ay ));
		double	l2 = root(( cx - bx ) * ( cx - bx ) + ( cy - by ) * ( cy - by ));
		double	n1x, n1y, n2x, n2y, mx, my, ml, cosv, k;

		if ( l1 < 1e-9 ) l1 = 1;
		if ( l2 < 1e-9 ) l2 = 1;
		/* the normals into the shape */
		n1x = -( by - ay ) / l1 * sgn;
		n1y = ( bx - ax ) / l1 * sgn;
		n2x = -( cy - by ) / l2 * sgn;
		n2y = ( cx - bx ) / l2 * sgn;
		mx = n1x + n2x;
		my = n1y + n2y;
		ml = root(mx * mx + my * my);
		if ( ml < 1e-9 ) {
			mx = n1x;
			my = n1y;
			ml = 1;
		}
		mx /= ml;
		my /= ml;
		cosv = mx * n1x + my * n1y;
		if ( cosv < 0.3 ) cosv = 0.3;
		k = d / cosv;
		out[i * 2] = (INT)( bx + mx * k + ( ( bx + mx * k ) >= 0 ? 0.5 : -0.5 ) );
		out[i * 2 + 1] = (INT)( by + my * k + ( ( by + my * k ) >= 0 ? 0.5 : -0.5 ) );
	}
}
