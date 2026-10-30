/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	compose.c
 *	Laying one window onto the screen being built (design 16.5.5)
 *
 *	The screen is built in the back buffer from the ground up, the
 *	windows from the back forward, so that when a window goes down the
 *	back buffer holds exactly what is behind it. A window goes down in
 *	stages, each reading what the stages before it left:
 *
 *	  1. its shadow darkens what is behind it, fading out linearly over
 *	     its size, and not where the window itself stands: a shadow
 *	     falls on what is behind, and a window seen through would
 *	     otherwise show its own shadow through itself;
 *	  2. what shows through the parts of it that are not solid is
 *	     spread: halved a few times, smoothed, and doubled back, worked
 *	     out only for those parts and only within what is being built;
 *	  3. its pixels are mixed over that, each by how solid its part is
 *	     and by how much of the pixel a round corner covers;
 *	  4. on the way, the frame takes the colour it is washed with and
 *	     the title band its own. A wash is laid here, never painted into
 *	     the window's pixels, so it goes with the window wherever it is
 *	     laid and leaves nothing behind where it was.
 *
 *	A window is cut into five parts for this: its work area, and the
 *	strips of frame along the top, the foot, the left and the right.
 *	The work area of a window seen through only at its frame is solid,
 *	and is laid as a copy of whole rows: what the look costs is the
 *	strips and the shadow, not the screen.
 *
 *	The windows of a part are laid together by cp_lay, each only where
 *	no window in front of it lays its pixels solid over it: what is
 *	hidden costs nothing, and a pixel a solid window shows is copied
 *	from that window alone.
 *
 *	Nothing here keeps anything between one screen and the next but
 *	room to work in; everything is worked out from the description of
 *	the window (T_CPWIN) and the back buffer. That is what lets a part
 *	of the screen be built alone and come out as the whole screen would.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>
#include "sysman/pfalloc.h"
#include "compose.h"

#define COV_STEP	8		/* samples across a pixel, each way */

/* ---------------------------------------------------------------- small things */

LOCAL BOOL cut( T_DPRECT *r, CONST T_DPRECT *by )
{
	if ( by->left   > r->left )   r->left   = by->left;
	if ( by->top    > r->top )    r->top    = by->top;
	if ( by->right  < r->right )  r->right  = by->right;
	if ( by->bottom < r->bottom ) r->bottom = by->bottom;

	return ( r->left < r->right && r->top < r->bottom );
}

LOCAL BOOL in_span( CONST INT *sp, INT n, INT x )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( x >= sp[i * 2] && x < sp[i * 2 + 1] ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL UW isqrt( UW v )
{
	UW	r = 0, b = 1U << 30;

	while ( b > v ) {
		b >>= 2;
	}
	while ( b != 0 ) {
		if ( v >= r + b ) {
			v -= r + b;
			r = ( r >> 1 ) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return r;
}

/*
 * 'over' laid at a of 255 over 'under', channel by channel and rounded.
 * The channels are kept apart: mixing the whole word at once carries
 * the bits of one channel into the next.
 */
LOCAL UW over_of( UW under, UW over, UINT a )
{
	UINT	b = 255U - a;
	UW	r, g, bl;

	r  = ( ( ( over >> 16 ) & 0xFFU ) * a + ( ( under >> 16 ) & 0xFFU ) * b + 127U ) / 255U;
	g  = ( ( ( over >> 8 ) & 0xFFU ) * a + ( ( under >> 8 ) & 0xFFU ) * b + 127U ) / 255U;
	bl = ( ( over & 0xFFU ) * a + ( under & 0xFFU ) * b + 127U ) / 255U;

	return ( r << 16 ) | ( g << 8 ) | bl;
}

/* A colour taken toward black by a of 255 */
LOCAL UW darker( UW c, UINT a )
{
	UINT	k = 255U - a;
	UW	r, g, b;

	r = ( ( ( c >> 16 ) & 0xFFU ) * k + 127U ) / 255U;
	g = ( ( ( c >> 8 ) & 0xFFU ) * k + 127U ) / 255U;
	b = ( ( c & 0xFFU ) * k + 127U ) / 255U;

	return ( r << 16 ) | ( g << 8 ) | b;
}

/* ---------------------------------------------------------------- room to work in */

/*
 * Kept between screens and grown when a larger piece comes: the spread
 * of what shows through, the levels it is worked out through, a row of
 * one level and the sums of one pass, and the edge of a part built
 * larger than asked. Only the spread needs them, and only as much as
 * the largest piece spread so far.
 */
LOCAL UW	*bl_out = NULL;		/* what shows through, spread */
LOCAL UD	bl_out_pages = 0;
LOCAL UW	*bl_lv[2] = { NULL, NULL };	/* a level, and the next worked out from it */
LOCAL UD	bl_lv_pages[2] = { 0, 0 };
LOCAL UW	*bl_row = NULL;		/* a row of a level, its channels apart (UD) */
LOCAL UD	bl_row_pages = 0;
LOCAL UW	*bl_mid = NULL;		/* the sums of a pass along the rows (UD) */
LOCAL UD	bl_mid_pages = 0;
LOCAL UW	*mg_buf = NULL;		/* the edge kept */
LOCAL UD	mg_pages = 0;

LOCAL BOOL room( UW **p, UD *pages, SZ words )
{
	UD	want = (UD)( ( words * (SZ)sizeof(UW) + PAGE_SIZE - 1 ) / PAGE_SIZE );

	if ( want == 0 ) {
		want = 1;
	}
	if ( *p != NULL && *pages >= want ) {
		return TRUE;
	}
	if ( *p != NULL ) {
		knl_vunmap(*p, *pages);
	}
	*p = (UW *)knl_vmap(want, 0);
	*pages = ( *p != NULL ) ? want : 0;

	return (BOOL)( *p != NULL );
}

LOCAL void let_go( UW **p, UD *pages )
{
	if ( *p != NULL ) {
		knl_vunmap(*p, *pages);
	}
	*p = NULL;
	*pages = 0;
}

EXPORT void cp_release( void )
{
	let_go(&bl_out, &bl_out_pages);
	let_go(&bl_lv[0], &bl_lv_pages[0]);
	let_go(&bl_lv[1], &bl_lv_pages[1]);
	let_go(&bl_row, &bl_row_pages);
	let_go(&bl_mid, &bl_mid_pages);
	let_go(&mg_buf, &mg_pages);
}

/* ---------------------------------------------------------------- corners */

/*
 * How much of each pixel of a round corner a window covers, 0..255: row
 * y and column x counted in from the corner, for a radius of rad. Each
 * pixel is sixty-four samples, counted inside the circle or not, which
 * gives more steps of cover than an edge needs to look smooth. Worked
 * out again only when the radius changes.
 */
LOCAL UB	cov_tab[CP_ROUND_MAX * CP_ROUND_MAX];
LOCAL INT	cov_rad = 0;

EXPORT CONST UB *cp_corner_cov( INT rad )
{
	INT	x, y, i, j, u, in;

	if ( rad <= 0 || rad > CP_ROUND_MAX ) {
		return NULL;
	}
	if ( rad == cov_rad ) {
		return cov_tab;
	}
	u = rad * 2 * COV_STEP;
	for ( y = 0; y < rad; y++ ) {
		for ( x = 0; x < rad; x++ ) {
			in = 0;
			for ( j = 0; j < COV_STEP; j++ ) {
				for ( i = 0; i < COV_STEP; i++ ) {
					INT	px = ( x * COV_STEP + i ) * 2 + 1 - u;
					INT	py = ( y * COV_STEP + j ) * 2 + 1 - u;

					if ( px * px + py * py <= u * u ) {
						in++;
					}
				}
			}
			cov_tab[y * rad + x] = (UB)( in * 255 / ( COV_STEP * COV_STEP ) );
		}
	}
	cov_rad = rad;

	return cov_tab;
}

EXPORT INT cp_round_of( CONST T_DPRECT *r, INT rad )
{
	INT	w = r->right - r->left, h = r->bottom - r->top;

	if ( rad <= 0 ) {
		return 0;
	}
	if ( rad > w / 2 ) rad = w / 2;
	if ( rad > h / 2 ) rad = h / 2;
	if ( rad > CP_ROUND_MAX ) rad = CP_ROUND_MAX;

	return ( rad > 0 ) ? rad : 0;
}

/* The cover of the row y of a rectangle with corners of radius rad, or NULL where it is whole */
LOCAL CONST UB *cover_row( CONST T_DPRECT *r, INT rad, CONST UB *cov, INT y )
{
	INT	ey = y - r->top;

	if ( rad <= 0 || cov == NULL ) {
		return NULL;
	}
	if ( r->bottom - 1 - y < ey ) {
		ey = r->bottom - 1 - y;
	}
	return ( ey < rad ) ? cov + ey * rad : NULL;
}

/* And of the pixel x of that row */
LOCAL UINT cover_x( CONST T_DPRECT *r, INT rad, CONST UB *row, INT x )
{
	INT	ex = x - r->left;

	if ( row == NULL ) {
		return 255;
	}
	if ( r->right - 1 - x < ex ) {
		ex = r->right - 1 - x;
	}
	return ( ex < rad ) ? row[ex] : 255;
}

/*
 * The frame's lines round a corner. The corner is cut on a circle, and
 * the lines that run along the frame's edges -- the outline, the light
 * or dark lines inside it, the band between, and on a thick frame the
 * lines further in -- are drawn again on circles inside that one, a dot
 * further in each, so that the whole frame bends with the corner and no
 * line of it looks cut off. How far in a frame's lines go is given for
 * each edge of each corner (T_CPLINES): the width of a side's strip, or
 * only the outline and the line inside it where a scroll bar or the
 * title band lies along the edge, whose insides are not lines. What
 * each line is, is read from the frame itself just past the corner
 * along each of its two edges, so the lines of any frame go round. A
 * pixel is the same sixty-four samples as the cover: a sample on the
 * n-th circle in takes the n-th dot of the edge it is nearer to, one
 * outside the corner the outline, and one further in than that edge's
 * lines the dot after them. A pixel with every sample further in is
 * left as it is; the rest are written from the edges alone, so drawing
 * them again changes nothing.
 */
#define LN_TABS		4		/* the depths last used, kept worked out */

typedef struct {
	INT	rad;			/* 0: not worked out */
	INT	lim[2];			/* lines along the side edge, the end edge */
	UB	n[CP_ROUND_MAX * CP_ROUND_MAX][2][CP_ROUND_MAX + 1];
	BOOL	on[CP_ROUND_MAX * CP_ROUND_MAX];
} LNTAB;

LOCAL LNTAB	ln_tab[LN_TABS];
LOCAL INT	ln_next = 0;

LOCAL CONST LNTAB *ln_make( INT rad, INT lside, INT lend )
{
	LNTAB	*t;
	INT	x, y, i, j, n, k, u = rad * 2 * COV_STEP;

	for ( k = 0; k < LN_TABS; k++ ) {
		t = &ln_tab[k];
		if ( t->rad == rad && t->lim[0] == lside && t->lim[1] == lend ) {
			return t;
		}
	}
	t = &ln_tab[ln_next];
	ln_next = ( ln_next + 1 ) % LN_TABS;
	knl_memset(t, 0, sizeof(*t));
	for ( y = 0; y < rad; y++ ) {
		for ( x = 0; x < rad; x++ ) {
			INT	at = y * rad + x, lines = 0;

			for ( j = 0; j < COV_STEP; j++ ) {
				for ( i = 0; i < COV_STEP; i++ ) {
					INT	px = ( x * COV_STEP + i ) * 2 + 1 - u;
					INT	py = ( y * COV_STEP + j ) * 2 + 1 - u;
					INT	d2 = px * px + py * py;
					/* the side edge where it is further across than down */
					INT	e = ( -px > -py ) ? 0 : 1;
					INT	lim = ( e == 0 ) ? lside : lend;

					/* the line it is on: 0 the outline (and outside it), lim further in */
					k = 0;
					if ( d2 <= u * u ) {
						for ( n = 1; n <= lim; n++ ) {
							INT	r = u - n * 2 * COV_STEP;

							if ( r <= 0 || d2 > r * r ) {
								break;
							}
							k = n;
						}
					}
					t->n[at][e][k]++;
					if ( k < lim ) {
						lines++;
					}
				}
			}
			t->on[at] = (BOOL)( lines > 0 );
		}
	}
	t->rad = rad;
	t->lim[0] = lside;
	t->lim[1] = lend;
	return t;
}

LOCAL INT ln_clip( INT v, INT rad )
{
	if ( v < 1 ) v = 1;
	if ( v > rad - 1 ) v = rad - 1;
	return v;
}

EXPORT void cp_corner_lines( UB *pixels, UINT pitch, CONST T_DPRECT *outer, INT round,
			     CONST T_CPLINES *lines )
{
	INT	rad = cp_round_of(outer, round);
	INT	fw = outer->right - outer->left, fh = outer->bottom - outer->top;
	INT	c, x, y, n;

	if ( pixels == NULL || lines == NULL || rad < 3 ) {
		return;
	}
	for ( c = 0; c < 4; c++ ) {
		BOOL		right = (BOOL)( ( c & 1 ) != 0 ), bottom = (BOOL)( ( c & 2 ) != 0 );
		INT		lim[2];
		CONST LNTAB	*t;
		UW		edge[2][CP_ROUND_MAX + 1];

		lim[0] = ln_clip(lines->side[c], rad);
		lim[1] = ln_clip(lines->end[c], rad);
		t = ln_make(rad, lim[0], lim[1]);

		/* the dots of the side edge just below (or above) the corner, and of the end edge beside it */
		for ( n = 0; n <= lim[0]; n++ ) {
			INT	sx = right ? fw - 1 - n : n, sy = bottom ? fh - 1 - rad : rad;

			edge[0][n] = *(UW *)( pixels + (UBINT)sy * pitch + (UBINT)sx * 4 );
		}
		for ( n = 0; n <= lim[1]; n++ ) {
			INT	ex = right ? fw - 1 - rad : rad, ey = bottom ? fh - 1 - n : n;

			edge[1][n] = *(UW *)( pixels + (UBINT)ey * pitch + (UBINT)ex * 4 );
		}
		for ( y = 0; y < rad; y++ ) {
			INT	py = bottom ? fh - 1 - y : y;
			UW	*row = (UW *)( pixels + (UBINT)py * pitch );

			for ( x = 0; x < rad; x++ ) {
				INT	at = y * rad + x, e;
				UINT	r = 0, g = 0, b = 0;
				UW	*p;

				if ( !t->on[at] ) {
					continue;
				}
				for ( e = 0; e < 2; e++ ) {
					for ( n = 0; n <= lim[e]; n++ ) {
						UINT	k = t->n[at][e][n];
						UW	v = edge[e][n];

						if ( k == 0 ) {
							continue;
						}
						r += k * ( ( v >> 16 ) & 0xFFU );
						g += k * ( ( v >> 8 ) & 0xFFU );
						b += k * ( v & 0xFFU );
					}
				}
				p = &row[right ? fw - 1 - x : x];
				*p = ( *p & 0xFF000000U )
				     | ( ( ( r + COV_STEP * COV_STEP / 2 ) / ( COV_STEP * COV_STEP ) ) << 16 )
				     | ( ( ( g + COV_STEP * COV_STEP / 2 ) / ( COV_STEP * COV_STEP ) ) << 8 )
				     | ( ( b + COV_STEP * COV_STEP / 2 ) / ( COV_STEP * COV_STEP ) );
			}
		}
	}
}

/* ---------------------------------------------------------------- 1. the shadow */

EXPORT BOOL cp_shadow_extent( CONST T_CPWIN *w, T_DPRECT *out )
{
	if ( w->shd_dark == 0 ) {
		return FALSE;
	}
	out->left   = w->outer.left + w->shd_dx - w->shd_size;
	out->top    = w->outer.top + w->shd_dy - w->shd_size;
	out->right  = w->outer.right + w->shd_dx + w->shd_size;
	out->bottom = w->outer.bottom + w->shd_dy + w->shd_size;

	return TRUE;
}

/*
 * How dark the shadow is at a pixel: the window's outline thrown by the
 * offset, round as the window is; all of the darkness inside it, and
 * past it less and less until none at 'size' away. The distance is in
 * sixteenths, so that the fade is smooth round a corner too.
 */
LOCAL UINT shd_of( CONST T_CPWIN *w, INT rad, INT dx, INT dy )
{
	INT	d16, size16 = w->shd_size * 16;

	if ( dx == 0 ) {
		d16 = dy * 16;
	} else if ( dy == 0 ) {
		d16 = dx * 16;
	} else {
		d16 = (INT)isqrt((UW)( dx * dx + dy * dy ) * 256U);
	}
	d16 -= rad * 16;
	if ( d16 <= 0 ) {
		return w->shd_dark;
	}
	if ( d16 >= size16 ) {
		return 0;
	}
	return (UINT)( (INT)w->shd_dark * ( size16 - d16 ) / size16 );
}

/*
 * The darkness depends only on how far across and down a pixel is from
 * the nearest point of the outline's straight part, and is none from
 * 'rad + size' on either way: it is worked out once for each pair of
 * those and looked up for each pixel, rather than a square root a pixel.
 */
#define SHD_TAB		( CP_ROUND_MAX + 32 + 1 )	/* the most a side of the table holds */

typedef struct {
	INT	l, r, t, b;		/* the straight part of the outline thrown */
	INT	n;			/* pairs from 0 to n - 1 held; past that, none */
	UB	a[SHD_TAB * SHD_TAB];
} SHDTAB;

LOCAL void shd_make( SHDTAB *tb, CONST T_CPWIN *w, CONST T_DPRECT *s, INT rad )
{
	INT	dx, dy;

	tb->l = s->left + rad;
	tb->r = s->right - 1 - rad;
	tb->t = s->top + rad;
	tb->b = s->bottom - 1 - rad;
	tb->n = rad + w->shd_size + 1;
	if ( tb->n > SHD_TAB ) {
		tb->n = SHD_TAB;
	}
	for ( dy = 0; dy < tb->n; dy++ ) {
		for ( dx = 0; dx < tb->n; dx++ ) {
			tb->a[dy * SHD_TAB + dx] = (UB)shd_of(w, rad, dx, dy);
		}
	}
}

LOCAL UINT shd_at( CONST SHDTAB *tb, INT x, INT y )
{
	INT	dx = ( x < tb->l ) ? tb->l - x : ( x > tb->r ) ? x - tb->r : 0;
	INT	dy = ( y < tb->t ) ? tb->t - y : ( y > tb->b ) ? y - tb->b : 0;

	if ( dx >= tb->n || dy >= tb->n ) {
		return 0;
	}
	return tb->a[dy * SHD_TAB + dx];
}

#define SHD_RUN		64		/* pixels of a shadow darkened at once on the vector unit */

EXPORT void cp_shadow( CONST T_CPDST *d, CONST T_CPWIN *w )
{
	T_DPRECT	e, s;
	CONST UB	*cov;
	INT		rad, y, x, k, ol = w->outer.left, orr = w->outer.right;
	BOOL		simd = cp_simd_here();
	SHDTAB		*tbp;

	if ( !cp_shadow_extent(w, &e) || !cut(&e, &d->box) ) {
		return;
	}
	/* the table is 4KB and more: from the heap, since a task of the kernel has a small stack */
	tbp = (SHDTAB *)Kmalloc(sizeof(SHDTAB));
	if ( tbp == NULL ) {
		return;				/* no shadow this time */
	}
	rad = ( w->shape == NULL ) ? cp_round_of(&w->outer, w->round) : 0;
	cov = cp_corner_cov(rad);
	if ( cov == NULL ) {
		rad = 0;
	}
	s = w->outer;
	s.left += w->shd_dx;   s.right += w->shd_dx;
	s.top += w->shd_dy;    s.bottom += w->shd_dy;
	shd_make(tbp, w, &s, rad);

	for ( y = e.top; y < e.bottom; y++ ) {
		UW		*row = (UW *)( d->back + (UBINT)y * d->pitch );
		BOOL		inrow = ( y >= w->outer.top && y < w->outer.bottom );
		CONST UB	*cr = inrow ? cover_row(&w->outer, rad, cov, y) : NULL;
		INT		seg[4][2], n = 0;

		/*
		 * Where the window stands it covers its own shadow, and
		 * nothing is worked out there: only the parts of the row
		 * either side of it, and in a row of its round corners the
		 * corners, which it covers only in part.
		 */
		if ( !inrow ) {
			seg[n][0] = e.left;  seg[n][1] = e.right;  n++;
		} else {
			seg[n][0] = e.left;  seg[n][1] = ( e.right < ol ) ? e.right : ol;  n++;
			if ( cr != NULL ) {
				seg[n][0] = ol;  seg[n][1] = ol + rad;  n++;
				seg[n][0] = orr - rad;  seg[n][1] = orr;  n++;
			}
			seg[n][0] = ( e.left > orr ) ? e.left : orr;  seg[n][1] = e.right;  n++;
		}
		for ( k = 0; k < n; k++ ) {
			INT	x0 = ( seg[k][0] > e.left ) ? seg[k][0] : e.left;
			INT	x1 = ( seg[k][1] < e.right ) ? seg[k][1] : e.right;

			/*
			 * On the vector unit: how dark each pixel goes worked out
			 * as below, a run at a time, and the run darkened at once
			 */
			while ( simd && x0 < x1 ) {
				UB	dark[SHD_RUN];
				INT	m = ( x1 - x0 < SHD_RUN ) ? x1 - x0 : SHD_RUN, i;

				for ( i = 0; i < m; i++ ) {
					UINT	a = shd_at(tbp, x0 + i, y);

					if ( a != 0 && inrow && x0 + i >= ol && x0 + i < orr ) {
						a = a * ( 255U - cover_x(&w->outer, rad, cr, x0 + i) ) / 255U;
					}
					dark[i] = (UB)a;
				}
				cps_darken(row + x0, dark, m);
				x0 += m;
			}
			for ( x = x0; x < x1; x++ ) {
				UINT	a = shd_at(tbp, x, y);

				if ( a != 0 && inrow && x >= ol && x < orr ) {
					a = a * ( 255U - cover_x(&w->outer, rad, cr, x) ) / 255U;
				}
				if ( a != 0 ) {
					row[x] = darker(row[x], a);
				}
			}
		}
	}
	Kfree(tbp);
}

/* ---------------------------------------------------------------- the parts of a window */

typedef struct {
	T_DPRECT	r;		/* on the screen, within what is being built */
	UINT		a;		/* how solid */
	BOOL		frame;		/* a strip of frame: tinted and washed */
	UW		wcol;		/* the wash it takes */
	UINT		wstr;
	UW		*under;		/* what shows through it, spread, r's width a row; or NULL */
} PIECE;

#define NPIECE	5

/*
 * The work area, and the four strips of frame round it: along the top
 * (the title band, when there is one), the foot, the left and the
 * right, each cut to what is being built.
 */
LOCAL INT pieces_of( CONST T_CPDST *d, CONST T_CPWIN *w, PIECE *p )
{
	CONST T_DPRECT	*o = &w->outer, *k = &w->work;
	T_DPRECT	part[NPIECE];
	INT		i, n = 0;

	part[0] = *k;
	part[1].left = o->left;   part[1].top = o->top;
	part[1].right = o->right; part[1].bottom = k->top;
	part[2].left = o->left;   part[2].top = k->bottom;
	part[2].right = o->right; part[2].bottom = o->bottom;
	part[3].left = o->left;   part[3].top = k->top;
	part[3].right = k->left;  part[3].bottom = k->bottom;
	part[4].left = k->right;  part[4].top = k->top;
	part[4].right = o->right; part[4].bottom = k->bottom;

	for ( i = 0; i < NPIECE; i++ ) {
		PIECE	*q = &p[n];

		q->r = part[i];
		if ( !cut(&q->r, &d->box) ) {
			continue;
		}
		q->frame = (BOOL)( i != 0 );
		q->a = ( i == 0 ) ? w->a_work : w->a_frame;
		q->wcol = 0;
		q->wstr = 0;
		q->under = NULL;
		if ( i == 1 && w->bar_str > 0 && w->band.right > w->band.left ) {
			q->wcol = w->bar_wash;
			q->wstr = w->bar_str;
		} else if ( i != 0 && w->wash_str > 0 ) {
			q->wcol = w->wash;
			q->wstr = w->wash_str;
		}
		n++;
	}
	return n;
}

EXPORT INT cp_pieces( CONST T_CPDST *d, CONST T_CPWIN *w, T_CPPIECE *p )
{
	PIECE	q[NPIECE];
	INT	i, n = pieces_of(d, w, q);

	for ( i = 0; i < n; i++ ) {
		p[i].r = q[i].r;
		p[i].a = q[i].a;
		p[i].frame = q[i].frame;
		p[i].wcol = q[i].wcol;
		p[i].wstr = q[i].wstr;
	}
	return n;
}

/* ---------------------------------------------------------------- another way */

LOCAL CONST T_CPGPU	*cp_gpu_ops = NULL;

EXPORT void cp_gpu_set( CONST T_CPGPU *g )
{
	cp_gpu_ops = g;
}

EXPORT CONST T_CPGPU *cp_gpu_get( void )
{
	return cp_gpu_ops;
}

/* ---------------------------------------------------------------- the vector unit */

/*
 * The tasks that own the FP/SIMD registers and may use the loops of
 * compose_simd.c: the compositor, and a test that checks those loops
 * against the plain ones. Asked once for each piece or row laid, not
 * for each pixel.
 */
#define CP_SIMD_TASKS	4

LOCAL ID	cp_simd_tid[CP_SIMD_TASKS];
LOCAL BOOL	cp_simd_on = TRUE;

EXPORT void cp_simd_allow( ID tid, BOOL on )
{
	INT	i;

	for ( i = 0; i < CP_SIMD_TASKS; i++ ) {
		if ( cp_simd_tid[i] == tid ) {
			if ( !on ) {
				cp_simd_tid[i] = 0;
			}
			return;
		}
	}
	for ( i = 0; on && i < CP_SIMD_TASKS; i++ ) {
		if ( cp_simd_tid[i] == 0 ) {
			cp_simd_tid[i] = tid;
			return;
		}
	}
}

EXPORT void cp_simd_use( BOOL on )
{
	cp_simd_on = on;
}

EXPORT BOOL cp_simd_here( void )
{
	ID	tid;
	INT	i;

	if ( !cp_simd_on ) {
		return FALSE;
	}
	tid = tk_get_tid();
	for ( i = 0; i < CP_SIMD_TASKS; i++ ) {
		if ( cp_simd_tid[i] != 0 && cp_simd_tid[i] == tid ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT void cp_copy( UW *dst, CONST UW *src, INT n )
{
	if ( n <= 0 ) {
		return;
	}
	if ( cp_simd_here() ) {
		cps_copy(dst, src, n);
	} else {
		knl_memcpy(dst, src, (SZ)n * sizeof(UW));
	}
}

/*
 * The parts of the window cp_blur last worked out, with what shows
 * through them spread, for cp_blend to lay. Both are called for one
 * window after the other, under the window manager's lock. A window
 * laid in rectangles (cp_lay) has the parts of all of them here at once.
 */
#define HD_CUT		16		/* rectangles cp_lay lays a window in, at most */

LOCAL PIECE		pc[NPIECE * HD_CUT];
LOCAL INT		npc = 0;
LOCAL CONST T_CPWIN	*pc_for = NULL;

/* ---------------------------------------------------------------- 2. what shows through, spread */

/*
 * The spread is worked out through levels of what shows through, each
 * half as wide and as high as the one below it. The pixel (x, y) of
 * level k stands for the screen's pixels from (x << k, y << k) up to
 * ((x + 1) << k, (y + 1) << k), whatever part of the screen is being
 * built, so that a pixel of a level comes out the same however the
 * screen is cut up. Level 0 is the screen. Three steps make the levels:
 *
 *   halving	a pixel of level k from the four by four pixels of level
 *		k - 1 round its place, weighed 1 3 3 1 each way (of 64);
 *   smoothing	a pixel of the deepest level from the five by five round
 *		it, weighed a b m b a each way (of 4096);
 *   doubling	a pixel of level k - 1 from the two by two pixels of level
 *		k nearest its centre, the nearer weighed 3 and the further
 *		1 each way (of 16).
 *
 * What shows through is halved 'depth' times, smoothed, and doubled back
 * as many times. Each channel's sum is rounded to eight bits once, at
 * the end of each step, so a pixel of each level depends on nothing but
 * the level it is worked out from, which is what lets the GPU work each
 * one out on its own and come to the same. A step reads only the pixels
 * of a level that touch what is being built; past its edge, the pixel at
 * the edge again.
 *
 * Every step spreads what it is given by a known amount, the variance of
 * its weights in pixels of the screen squared: halving and doubling back
 * d times by (4^d - 1) / 2 together, smoothing at level d by its own
 * variance times 4^d. A distance of r dots is taken as the spread of the
 * average of the 2r + 1 pixels round each, r (r + 1) / 3 each way. The
 * depth is the deepest whose halving and doubling spread no more than
 * that, and the smoothing makes up the rest; its weights are of 64, with
 * the middle no less than its neighbours.
 */
LOCAL CONST UB	sp_plan[CP_BLUR_MAX + 1][4] = {	/* depth, a, b, m */
	{ 0, 0, 0, 64 },
	{ 0, 0, 21, 22 }, { 1, 0, 4, 56 }, { 1, 0, 20, 24 }, { 1, 7, 13, 24 },
	{ 2, 0, 5, 54 }, { 2, 0, 13, 38 }, { 2, 3, 10, 38 }, { 2, 5, 13, 28 },
	{ 2, 8, 13, 22 }, { 3, 0, 3, 58 }, { 3, 0, 6, 52 }, { 3, 0, 10, 44 },
	{ 3, 0, 15, 34 }, { 3, 0, 19, 26 }, { 3, 3, 12, 34 }, { 3, 5, 10, 34 }
};

LOCAL INT imin( INT a, INT b )
{
	return ( a < b ) ? a : b;
}

LOCAL INT imax( INT a, INT b )
{
	return ( a > b ) ? a : b;
}

/* How many pixels either way the smoothing of a plan reads */
LOCAL INT sp_taps( INT rad )
{
	return ( sp_plan[rad][1] != 0 ) ? 2 : ( sp_plan[rad][2] != 0 ) ? 1 : 0;
}

/*
 * How far the spread reaches: halving and doubling back d times read
 * 3 (2^d - 1) pixels of the screen either way, and the smoothing at
 * level d its taps, 2^d pixels each.
 */
EXPORT INT cp_spread_reach( INT rad )
{
	INT	d;

	if ( rad <= 0 ) {
		return 0;
	}
	if ( rad > CP_BLUR_MAX ) {
		rad = CP_BLUR_MAX;
	}
	d = sp_plan[rad][0];
	return 3 * ( ( 1 << d ) - 1 ) + ( sp_taps(rad) << d );
}

/* v / 2 rounded down, below zero too */
LOCAL INT half_down( INT v )
{
	return ( v >= 0 ) ? v / 2 : -( ( 1 - v ) / 2 );
}

/*
 * One way across of a plan: the rectangle lo..hi in a box blo..bhi, and
 * for each level [lo, hi) of the pixels that touch the box (v), those
 * worked out on the way down (dn) and on the way up (up).
 */
LOCAL void plan_axis( INT lo, INT hi, INT blo, INT bhi, INT depth, INT taps,
		      INT (*v)[2], INT (*dn)[2], INT (*up)[2] )
{
	INT	k;

	for ( k = 0; k <= depth; k++ ) {
		v[k][0] = blo >> k;
		v[k][1] = ( ( bhi - 1 ) >> k ) + 1;
	}

	/* doubling: a pixel from the two of the level above nearest its centre */
	up[0][0] = lo;
	up[0][1] = hi;
	for ( k = 1; k <= depth; k++ ) {
		up[k][0] = imax(half_down(up[k - 1][0] - 1), v[k][0]);
		up[k][1] = imin(( up[k - 1][1] >> 1 ) + 1, v[k][1]);
	}

	/* smoothing: its taps either way; halving: the four round twice its place */
	dn[depth][0] = imax(up[depth][0] - taps, v[depth][0]);
	dn[depth][1] = imin(up[depth][1] + taps, v[depth][1]);
	for ( k = depth - 1; k >= 0; k-- ) {
		dn[k][0] = imax(2 * dn[k + 1][0] - 1, v[k][0]);
		dn[k][1] = imin(2 * dn[k + 1][1] + 1, v[k][1]);
	}
}

EXPORT BOOL cp_spread_plan( CONST T_DPRECT *box, CONST T_DPRECT *r, INT rad, T_CPSPREAD *p )
{
	INT	xv[CP_BLUR_DEPTH + 1][2], xd[CP_BLUR_DEPTH + 1][2], xu[CP_BLUR_DEPTH + 1][2];
	INT	yv[CP_BLUR_DEPTH + 1][2], yd[CP_BLUR_DEPTH + 1][2], yu[CP_BLUR_DEPTH + 1][2];
	INT	k, depth, taps;

	if ( rad <= 0 || r->left >= r->right || r->top >= r->bottom ) {
		return FALSE;
	}
	if ( rad > CP_BLUR_MAX ) {
		rad = CP_BLUR_MAX;
	}
	depth = sp_plan[rad][0];
	taps = sp_taps(rad);
	p->depth = depth;
	p->w[0] = p->w[4] = sp_plan[rad][1];
	p->w[1] = p->w[3] = sp_plan[rad][2];
	p->w[2] = sp_plan[rad][3];
	plan_axis(r->left, r->right, box->left, box->right, depth, taps, xv, xd, xu);
	plan_axis(r->top, r->bottom, box->top, box->bottom, depth, taps, yv, yd, yu);
	for ( k = 0; k <= depth; k++ ) {
		p->valid[k].left = xv[k][0];  p->valid[k].right = xv[k][1];
		p->valid[k].top = yv[k][0];   p->valid[k].bottom = yv[k][1];
		p->down[k].left = xd[k][0];   p->down[k].right = xd[k][1];
		p->down[k].top = yd[k][0];    p->down[k].bottom = yd[k][1];
		p->up[k].left = xu[k][0];     p->up[k].right = xu[k][1];
		p->up[k].top = yu[k][0];      p->up[k].bottom = yu[k][1];
	}
	return TRUE;
}

/*
 * A sum of pixels is kept with its channels apart in one 64 bit word,
 * blue from bit 0, green from 21 and red from 42: a sum of the most
 * weight a step gives, 4096 times 255, stays inside its lane.
 */
#define LN_G		21
#define LN_R		42
#define LN_EACH(n)	( ( (UD)(n) << LN_R ) | ( (UD)(n) << LN_G ) | (UD)(n) )

LOCAL UD lanes( UW p )
{
	return ( (UD)( p & 0x00FF0000U ) << ( LN_R - 16 ) ) | ( (UD)( p & 0x0000FF00U ) << ( LN_G - 8 ) )
	       | (UD)( p & 0x000000FFU );
}

/*
 * Each lane of a sum divided by 2^s and rounded: a pixel. What a shift
 * brings down from the lane above lies over the eighth bit and is cut
 * off.
 */
LOCAL UW unlanes( UD v, INT s )
{
	v = ( v + LN_EACH(1U << ( s - 1 )) ) >> s;
	return (UW)( ( ( ( v >> LN_R ) & 0xFFU ) << 16 ) | ( ( ( v >> LN_G ) & 0xFFU ) << 8 ) | ( v & 0xFFU ) );
}

/* A level, or the part of it worked out */
typedef T_CPLEVEL	LEVEL;

LOCAL INT within( INT v, INT n )
{
	return ( v < 0 ) ? 0 : ( v >= n ) ? n - 1 : v;
}

/*
 * Row y (from the top of what it holds) of a level in lanes, from x =
 * lo (0 or less) to hi counted from its left edge, past its edges the
 * pixel at the edge again: a read past what a level holds is past the
 * pixels that touch what is being built. The answer is indexed by x.
 */
LOCAL CONST UD *row_lanes( CONST LEVEL *s, INT y, INT lo, INT hi )
{
	CONST UW	*p = s->p + (SZ)y * s->pitch;
	UD		*e = (UD *)bl_row - lo;
	INT		x, n = s->r.right - s->r.left;
	UD		first = lanes(p[0]), last = lanes(p[n - 1]);

	for ( x = lo; x < 0; x++ ) {
		e[x] = first;
	}
	for ( x = 0; x < n; x++ ) {
		e[x] = lanes(p[x]);
	}
	for ( x = n; x <= hi; x++ ) {
		e[x] = last;
	}
	return e;
}

/*
 * The steps, each from what a level holds (s) into the rectangle d of
 * the next, its width a row: along the rows of s into bl_mid, then down
 * those.
 */
LOCAL void halve( CONST LEVEL *s, CONST T_DPRECT *d, UW *out )
{
	INT	w = d->right - d->left, sh = s->r.bottom - s->r.top;
	INT	lo = 2 * d->left - 1 - s->r.left, hi = 2 * d->right - s->r.left;
	UD	*mid = (UD *)bl_mid;
	INT	y, i;

	if ( cp_simd_here() ) {
		cps_halve(s, d, out, (UH *)bl_row, (UH *)bl_mid);
		return;
	}
	for ( y = 0; y < sh; y++ ) {
		CONST UD	*q = row_lanes(s, y, lo, hi) + lo;
		UD		*m = mid + (SZ)y * w;

		for ( i = 0; i < w; i++, q += 2 ) {
			m[i] = q[0] + 3 * ( q[1] + q[2] ) + q[3];
		}
	}
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		t = 2 * y - 1 - s->r.top;
		CONST UD	*m0 = mid + (SZ)within(t, sh) * w, *m1 = mid + (SZ)within(t + 1, sh) * w;
		CONST UD	*m2 = mid + (SZ)within(t + 2, sh) * w, *m3 = mid + (SZ)within(t + 3, sh) * w;
		UW		*o = out + (SZ)( y - d->top ) * w;

		for ( i = 0; i < w; i++ ) {
			o[i] = unlanes(m0[i] + 3 * ( m1[i] + m2[i] ) + m3[i], 6);
		}
	}
}

/* Smoothing, the weights wt[0..4] being a b m b a: five taps, or three when a is 0 */
LOCAL void smooth( CONST LEVEL *s, CONST T_DPRECT *d, CONST INT *wt, UW *out )
{
	INT	w = d->right - d->left, sh = s->r.bottom - s->r.top, taps = ( wt[0] != 0 ) ? 2 : 1;
	INT	lo = d->left - taps - s->r.left, hi = d->right - 1 + taps - s->r.left;
	UD	a = (UD)wt[0], b = (UD)wt[1], c = (UD)wt[2];
	UD	*mid = (UD *)bl_mid;
	INT	y, i;

	if ( cp_simd_here() ) {
		cps_smooth(s, d, wt, out, (UH *)bl_row, (UH *)bl_mid);
		return;
	}
	for ( y = 0; y < sh; y++ ) {
		CONST UD	*q = row_lanes(s, y, lo, hi) + d->left - s->r.left;
		UD		*m = mid + (SZ)y * w;

		if ( taps == 2 ) {
			for ( i = 0; i < w; i++, q++ ) {
				m[i] = a * ( q[-2] + q[2] ) + b * ( q[-1] + q[1] ) + c * q[0];
			}
		} else {
			for ( i = 0; i < w; i++, q++ ) {
				m[i] = b * ( q[-1] + q[1] ) + c * q[0];
			}
		}
	}
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		t = y - s->r.top;
		CONST UD	*m0 = mid + (SZ)within(t - 2, sh) * w, *m1 = mid + (SZ)within(t - 1, sh) * w;
		CONST UD	*m2 = mid + (SZ)within(t, sh) * w;
		CONST UD	*m3 = mid + (SZ)within(t + 1, sh) * w, *m4 = mid + (SZ)within(t + 2, sh) * w;
		UW		*o = out + (SZ)( y - d->top ) * w;

		if ( taps == 2 ) {
			for ( i = 0; i < w; i++ ) {
				o[i] = unlanes(a * ( m0[i] + m4[i] ) + b * ( m1[i] + m3[i] ) + c * m2[i], 12);
			}
		} else {
			for ( i = 0; i < w; i++ ) {
				o[i] = unlanes(b * ( m1[i] + m3[i] ) + c * m2[i], 12);
			}
		}
	}
}

LOCAL void double_up( CONST LEVEL *s, CONST T_DPRECT *d, UW *out )
{
	INT	w = d->right - d->left, sh = s->r.bottom - s->r.top;
	INT	lo = ( d->left >> 1 ) - 1 - s->r.left, hi = ( ( d->right - 1 ) >> 1 ) + 1 - s->r.left;
	UD	*mid = (UD *)bl_mid;
	INT	y, i, x;

	if ( cp_simd_here() ) {
		cps_double(s, d, out, (UH *)bl_row, (UH *)bl_mid);
		return;
	}
	for ( y = 0; y < sh; y++ ) {
		CONST UD	*e = row_lanes(s, y, lo, hi);
		UD		*m = mid + (SZ)y * w;

		for ( i = 0, x = d->left; i < w; i++, x++ ) {
			CONST UD	*q = e + ( ( x >> 1 ) - s->r.left );

			m[i] = ( ( x & 1 ) != 0 ) ? 3 * q[0] + q[1] : q[-1] + 3 * q[0];
		}
	}
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		c = ( y >> 1 ) - s->r.top;
		CONST UD	*ma, *mb;
		UW		*o = out + (SZ)( y - d->top ) * w;

		if ( ( y & 1 ) != 0 ) {
			ma = mid + (SZ)within(c, sh) * w;
			mb = mid + (SZ)within(c + 1, sh) * w;
			for ( i = 0; i < w; i++ ) {
				o[i] = unlanes(3 * ma[i] + mb[i], 4);
			}
		} else {
			ma = mid + (SZ)within(c - 1, sh) * w;
			mb = mid + (SZ)within(c, sh) * w;
			for ( i = 0; i < w; i++ ) {
				o[i] = unlanes(ma[i] + 3 * mb[i], 4);
			}
		}
	}
}

LOCAL SZ sz_max( SZ a, SZ b )
{
	return ( a > b ) ? a : b;
}

LOCAL SZ area_of( CONST T_DPRECT *r )
{
	return (SZ)( r->right - r->left ) * ( r->bottom - r->top );
}

LOCAL void level_at( LEVEL *s, CONST UW *p, CONST T_DPRECT *r )
{
	s->p = p;
	s->pitch = r->right - r->left;
	s->r = *r;
}

/*
 * What shows through the rectangle r of what is being built, spread into
 * 'out', its width a row; 'whole' is the part of the screen being built,
 * which r may be only a piece of, and past whose edge a step reads the
 * pixel at the edge again. FALSE when there is no room to work in.
 */
LOCAL BOOL spread( CONST T_CPDST *d, CONST T_DPRECT *whole, CONST T_DPRECT *r, INT rad, UW *out )
{
	T_CPSPREAD	sp;
	LEVEL		s;
	SZ		lv = 0, row = 0, mid = 0;
	INT		k, c, at = 1;

	if ( !cp_spread_plan(whole, r, rad, &sp) ) {
		return TRUE;
	}
	c = sp.depth;

	/* room for two levels, a row of the widest, the sums of the largest step */
	for ( k = 0; k <= c; k++ ) {
		row = sz_max(row, sp.down[k].right - sp.down[k].left);
		if ( k > 0 ) {
			lv = sz_max(sz_max(lv, area_of(&sp.down[k])), area_of(&sp.up[k]));
			row = sz_max(row, sp.up[k].right - sp.up[k].left);
			mid = sz_max(mid, (SZ)( sp.down[k - 1].bottom - sp.down[k - 1].top )
					   * ( sp.down[k].right - sp.down[k].left ));
			mid = sz_max(mid, (SZ)( sp.up[k].bottom - sp.up[k].top ) * ( sp.up[k - 1].right - sp.up[k - 1].left ));
		}
	}
	mid = sz_max(mid, (SZ)( sp.down[c].bottom - sp.down[c].top ) * ( sp.up[c].right - sp.up[c].left ));
	if ( ( c > 0 && ( !room(&bl_lv[0], &bl_lv_pages[0], lv) || !room(&bl_lv[1], &bl_lv_pages[1], lv) ) )
	  || !room(&bl_row, &bl_row_pages, ( row + 8 ) * 2) || !room(&bl_mid, &bl_mid_pages, mid * 2) ) {
		return FALSE;
	}

	/* down from what is being built, halved; smoothed at the deepest level; doubled back up */
	s.p = (CONST UW *)( d->back + (UBINT)sp.down[0].top * d->pitch ) + sp.down[0].left;
	s.pitch = (SZ)( d->pitch / sizeof(UW) );
	s.r = sp.down[0];
	for ( k = 1; k <= c; k++ ) {
		at = 1 - at;
		halve(&s, &sp.down[k], bl_lv[at]);
		level_at(&s, bl_lv[at], &sp.down[k]);
	}
	if ( c == 0 ) {
		smooth(&s, &sp.up[0], sp.w, out);
		return TRUE;
	}
	at = 1 - at;
	smooth(&s, &sp.up[c], sp.w, bl_lv[at]);
	level_at(&s, bl_lv[at], &sp.up[c]);
	for ( k = c; k > 1; k-- ) {
		at = 1 - at;
		double_up(&s, &sp.up[k - 1], bl_lv[at]);
		level_at(&s, bl_lv[at], &sp.up[k - 1]);
	}
	double_up(&s, &sp.up[0], out);
	return TRUE;
}

/*
 * What shows through the parts pc[0..npc-1] of w spread, every one of
 * them before any is laid, each read in the part 'whole' being built
 */
LOCAL void spread_pieces( CONST T_CPDST *d, CONST T_CPWIN *w, CONST T_DPRECT *whole )
{
	SZ	total = 0, at = 0;
	INT	rad = w->blur, i;

	pc_for = w;
	if ( rad <= 0 ) {
		return;
	}
	for ( i = 0; i < npc; i++ ) {
		if ( pc[i].a < 255 ) {
			total += area_of(&pc[i].r);
		}
	}
	if ( total == 0 ) {
		return;				/* solid all through: nothing shows */
	}
	if ( !room(&bl_out, &bl_out_pages, total) ) {
		return;				/* no room this time: laid unspread */
	}
	for ( i = 0; i < npc; i++ ) {
		if ( pc[i].a < 255 ) {
			if ( spread(d, whole, &pc[i].r, rad, bl_out + at) ) {
				pc[i].under = bl_out + at;
			}
			at += area_of(&pc[i].r);
		}
	}
}

EXPORT void cp_blur( CONST T_CPDST *d, CONST T_CPWIN *w )
{
	npc = pieces_of(d, w, pc);
	spread_pieces(d, w, &d->box);
}

/* ---------------------------------------------------------------- 3 and 4. the pixels, washed */

/* The pixels x0..x1 of a row of a piece, one by one */
LOCAL void lay_run( CONST T_CPWIN *w, CONST PIECE *q, UW *dst, CONST UW *src, CONST UW *und,
		    CONST INT *sp, INT ns, INT rad, CONST UB *cr, UINT tstr, BOOL keyed,
		    INT x0, INT x1 )
{
	INT	x;

	for ( x = x0; x < x1; x++ ) {
		UW	c = src[x];
		UINT	a = q->a;
		INT	sx = q->r.left + x;

		if ( ns > 0 && !in_span(sp, ns, sx - w->outer.left) ) {
			continue;	/* outside the outline */
		}
		if ( keyed && ( c >> 24 ) == 0xFF ) {
			continue;	/* see-through */
		}
		if ( cr != NULL ) {
			a = ( a * cover_x(&w->outer, rad, cr, sx) + 127U ) / 255U;
			if ( a == 0 ) {
				continue;
			}
		}
		if ( tstr != 0 ) {
			c = over_of(c, w->tint, tstr);
		}
		if ( q->wstr != 0 ) {
			c = over_of(c, q->wcol, q->wstr);
		}
		dst[x] = ( a >= 255 ) ? c : over_of(( und != NULL ) ? und[x] : dst[x], c, a);
	}
}

/*
 * On the vector unit a row without an outline of its own is laid in one
 * run where nothing but the piece's solidity varies: all of it, or in a
 * row of the round corners all but the corners, where the cover is
 * whole and a pixel is mixed at the piece's own a ((a * 255 + 127) / 255
 * is a again). The corners go one pixel at a time; a piece with a of 0
 * lays nothing in such a row.
 */
LOCAL void lay_piece( CONST T_CPDST *d, CONST T_CPWIN *w, CONST PIECE *q,
		      INT rad, CONST UB *cov )
{
	INT	n = q->r.right - q->r.left, y;
	UINT	tstr = q->frame ? w->tint_str : 0;
	BOOL	keyed = (BOOL)( !q->frame && w->keyed );
	BOOL	plain = (BOOL)( q->a >= 255 && q->wstr == 0 && tstr == 0 && !keyed );
	BOOL	simd = cp_simd_here();

	for ( y = q->r.top; y < q->r.bottom; y++ ) {
		UW		*dst = (UW *)( d->back + (UBINT)y * d->pitch ) + q->r.left;
		CONST UW	*src = (CONST UW *)( w->pixels + (UBINT)( y - w->outer.top ) * w->pitch )
				       + ( q->r.left - w->outer.left );
		CONST UW	*und = ( q->under != NULL ) ? q->under + (SZ)( y - q->r.top ) * n : NULL;
		CONST UB	*cr = cover_row(&w->outer, rad, cov, y);
		CONST INT	*sp = NULL;
		INT		ns = 0;

		if ( w->shape != NULL ) {
			ns = dp_rgn_row(w->shape, y - w->outer.top, &sp);
			if ( ns == 0 ) {
				continue;	/* this row is past the outline */
			}
		}
		if ( plain && cr == NULL && ns == 0 ) {
			if ( simd ) {
				cps_copy(dst, src, n);
			} else {
				knl_memcpy(dst, src, (SZ)n * sizeof(UW));
			}
			continue;
		}
		if ( simd && ns == 0 ) {
			INT	x0 = 0, x1 = n;

			if ( cr != NULL ) {
				if ( q->a == 0 ) {
					continue;
				}
				x0 = w->outer.left + rad - q->r.left;
				x1 = w->outer.right - rad - q->r.left;
				if ( x0 < 0 ) x0 = 0;
				if ( x0 > n ) x0 = n;
				if ( x1 > n ) x1 = n;
				if ( x1 < x0 ) x1 = x0;
				lay_run(w, q, dst, src, und, sp, ns, rad, cr, tstr, keyed, 0, x0);
				lay_run(w, q, dst, src, und, sp, ns, rad, cr, tstr, keyed, x1, n);
			}
			if ( plain ) {
				cps_copy(dst + x0, src + x0, x1 - x0);
			} else {
				cps_lay(dst + x0, src + x0, ( und != NULL ) ? und + x0 : NULL, x1 - x0,
					q->a, keyed, w->tint, tstr, q->wcol, q->wstr);
			}
			continue;
		}
		lay_run(w, q, dst, src, und, sp, ns, rad, cr, tstr, keyed, 0, n);
	}
}

EXPORT void cp_blend( CONST T_CPDST *d, CONST T_CPWIN *w )
{
	CONST UB	*cov;
	INT		rad, i;

	if ( pc_for != w ) {
		npc = pieces_of(d, w, pc);	/* nothing spread for it */
	}
	rad = ( w->shape == NULL ) ? cp_round_of(&w->outer, w->round) : 0;
	cov = cp_corner_cov(rad);
	if ( cov == NULL ) {
		rad = 0;
	}
	for ( i = 0; i < npc; i++ ) {
		lay_piece(d, w, &pc[i], rad, cov);
	}
	pc_for = NULL;
}

/* ---------------------------------------------------------------- opening and closing */

/*
 * A window on its way in or out: its pixels laid smaller than it is,
 * each pixel of 'shown' taken from the pixel of the window it falls on,
 * and fainter; the work area by how solid it is, the frame by how
 * solid the frame is, both by how far the motion has come. No shadow
 * and no spread: they come with the window once it has arrived.
 */
EXPORT void cp_moving( CONST T_CPDST *d, CONST T_CPWIN *w )
{
	T_DPRECT	v = w->shown;
	INT		ow = w->outer.right - w->outer.left, oh = w->outer.bottom - w->outer.top;
	INT		sw = w->shown.right - w->shown.left, sh = w->shown.bottom - w->shown.top;
	INT		rad, y, x;
	UW		stepx, stepy;
	CONST UB	*cov;
	UINT		af = w->a_frame * w->fade / 255U, aw = w->a_work * w->fade / 255U;

	if ( sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0 || !cut(&v, &d->box) ) {
		return;
	}
	rad = cp_round_of(&w->shown, w->round);
	cov = cp_corner_cov(rad);
	if ( cov == NULL ) {
		rad = 0;
	}
	stepx = ( (UW)ow << 16 ) / (UW)sw;
	stepy = ( (UW)oh << 16 ) / (UW)sh;
	for ( y = v.top; y < v.bottom; y++ ) {
		INT		wy = (INT)( ( (UW)( y - w->shown.top ) * stepy ) >> 16 );
		INT		ay = w->outer.top + wy;
		CONST UW	*src = (CONST UW *)( w->pixels + (UBINT)wy * w->pitch );
		UW		*dst = (UW *)( d->back + (UBINT)y * d->pitch );
		BOOL		workrow = ( ay >= w->work.top && ay < w->work.bottom );
		BOOL		bandrow = ( ay < w->work.top && w->band.right > w->band.left );
		CONST UB	*cr = cover_row(&w->shown, rad, cov, y);
		UW		fx = (UW)( v.left - w->shown.left ) * stepx;

		for ( x = v.left; x < v.right; x++, fx += stepx ) {
			INT	wx = (INT)( fx >> 16 );
			INT	ax = w->outer.left + wx;
			UW	c = src[wx];
			BOOL	inwork = ( workrow && ax >= w->work.left && ax < w->work.right );
			UINT	a = inwork ? aw : af;

			if ( inwork && w->keyed && ( c >> 24 ) == 0xFF ) {
				continue;
			}
			if ( cr != NULL ) {
				a = ( a * cover_x(&w->shown, rad, cr, x) + 127U ) / 255U;
			}
			if ( a == 0 ) {
				continue;
			}
			if ( !inwork ) {
				if ( w->tint_str != 0 ) {
					c = over_of(c, w->tint, w->tint_str);
				}
				if ( bandrow && w->bar_str != 0 ) {
					c = over_of(c, w->bar_wash, w->bar_str);
				} else if ( w->wash_str != 0 ) {
					c = over_of(c, w->wash, w->wash_str);
				}
			}
			dst[x] = ( a >= 255 ) ? c : over_of(dst[x], c, a);
		}
	}
}

/* ---------------------------------------------------------------- the whole */

EXPORT void cp_window( CONST T_CPDST *d, CONST T_CPWIN *w )
{
	if ( CP_MOVING(w) ) {
		cp_moving(d, w);
		return;
	}
	cp_shadow(d, w);
	cp_blur(d, w);
	cp_blend(d, w);
}

/* ---------------------------------------------------------------- what is hidden */

/*
 * A window in front lays some of its pixels solid: its own pixel, washed
 * or tinted perhaps, mixed with nothing behind it. What a window behind
 * it does to such a pixel -- its shadow, its pixels mixed in -- is lost
 * when the one in front goes down, so it need not be done at all. Only a
 * spread reads pixels other than its own, and it reads them as the
 * windows behind it left them; a pixel within reach of what a window
 * spreads is therefore laid by the windows behind that window, whatever
 * lies over it further forward.
 *
 * cp_lay works it out from the front back. What the windows in front of
 * a window lay solid, less what any of them reads to spread what of it
 * shows, is where the window is left out: it is cut out of how far the
 * window reaches, and the rectangles left are kept. Only what shows of
 * a window is spread and mixed; its shadow goes down there and also
 * where that spread reads, hidden or not. Then the windows are laid from
 * the back forward as cp_window lays them, each in its own rectangles,
 * every stage in all of them before the next, a spread reading past
 * them in the whole part as it always does: what a window spreads is
 * what lay behind it with its shadow over that, never a piece of the
 * window itself. A rectangle that would be cut into more than there is
 * room for is left as it was, and a solid rectangle there is no room to
 * keep is let go of: either only lays more than it needs to, which
 * changes nothing.
 */
#define HD_KEEP		64		/* rectangles laid solid in front, kept */
#define HD_SOLID	( 3 * NPIECE )	/* rectangles one window lays solid, at most */

LOCAL T_DPRECT	hd_front[HD_KEEP];	/* laid solid in front, read by no spread between */
LOCAL INT	hd_nfront;
LOCAL T_DPRECT	hd_tmp[HD_KEEP];
LOCAL T_DPRECT	hd_rect[CP_LAY_MAX * HD_CUT * 2];	/* where each window is laid, and its shadow */
LOCAL INT	hd_at[CP_LAY_MAX], hd_n[CP_LAY_MAX];
LOCAL INT	hd_sat[CP_LAY_MAX], hd_sn[CP_LAY_MAX];
LOCAL T_DPRECT	hd_read[NPIECE * HD_CUT];	/* what a window's spread reads */
LOCAL BOOL	hd_on = TRUE;
LOCAL UD	hd_hidden = 0;

EXPORT void cp_hide_use( BOOL on )
{
	hd_on = on;
}

EXPORT UD cp_hidden( void )
{
	return hd_hidden;
}

LOCAL BOOL meets( CONST T_DPRECT *a, CONST T_DPRECT *b )
{
	return ( a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom );
}

/* Whether a holds all of b */
LOCAL BOOL holds( CONST T_DPRECT *a, CONST T_DPRECT *b )
{
	return ( a->left <= b->left && a->top <= b->top && a->right >= b->right && a->bottom >= b->bottom );
}

/*
 * The rectangles v[0..n-1] with c taken out of each, into o: above c,
 * below it, and either side of it in the rows they share. How many, or
 * -1 when that would be more than max.
 */
LOCAL INT rects_cut( CONST T_DPRECT *v, INT n, CONST T_DPRECT *c, T_DPRECT *o, INT max )
{
	INT	i, m = 0;

	for ( i = 0; i < n; i++ ) {
		CONST T_DPRECT	*a = &v[i];
		INT		t, b;

		if ( !meets(a, c) ) {
			if ( m >= max ) return -1;
			o[m++] = *a;
			continue;
		}
		t = imax(a->top, c->top);
		b = imin(a->bottom, c->bottom);
		if ( a->top < c->top ) {
			if ( m >= max ) return -1;
			o[m] = *a;  o[m].bottom = c->top;  m++;
		}
		if ( c->bottom < a->bottom ) {
			if ( m >= max ) return -1;
			o[m] = *a;  o[m].top = c->bottom;  m++;
		}
		if ( a->left < c->left ) {
			if ( m >= max ) return -1;
			o[m].left = a->left;  o[m].top = t;  o[m].right = c->left;  o[m].bottom = b;  m++;
		}
		if ( c->right < a->right ) {
			if ( m >= max ) return -1;
			o[m].left = c->right;  o[m].top = t;  o[m].right = a->right;  o[m].bottom = b;  m++;
		}
	}
	return m;
}

LOCAL void grow( T_DPRECT *e, CONST T_DPRECT *s )
{
	e->left = imin(e->left, s->left);
	e->top = imin(e->top, s->top);
	e->right = imax(e->right, s->right);
	e->bottom = imax(e->bottom, s->bottom);
}

/*
 * How far a window reaches within the box, its shadow and all, and its
 * pieces, which a work area not inside the window takes past it; FALSE
 * for nowhere
 */
LOCAL BOOL reach_of( CONST T_DPRECT *box, CONST T_CPWIN *w, T_DPRECT *e )
{
	T_DPRECT	s;
	T_CPDST		d;
	PIECE		p[NPIECE];
	INT		i, np;

	*e = w->shown;
	if ( !CP_MOVING(w) ) {
		if ( cp_shadow_extent(w, &s) ) {
			grow(e, &s);
		}
		d.box = *box;
		np = pieces_of(&d, w, p);
		for ( i = 0; i < np; i++ ) {
			grow(e, &p[i].r);
		}
	}
	return cut(e, box);
}

/*
 * What a window lays solid within the box: each piece that is solid and
 * not keyed (the whole window at once when all of it is), less its
 * round corners, whose pixels are mixed by how much of each the corner
 * covers -- the rows of the corners between them are solid. Nothing of
 * a window opening or closing or with an outline of its own. How many
 * rectangles, up to HD_SOLID.
 */
LOCAL INT solid_rects( CONST T_DPRECT *box, CONST T_CPWIN *w, T_DPRECT *out )
{
	CONST T_DPRECT	*o = &w->outer;
	T_CPDST		d;
	PIECE		q[NPIECE];
	T_DPRECT	band[3], p[NPIECE], r;
	INT		rad, np, nb, i, k, n = 0;

	/* the pieces stand apart only with the work area inside the window */
	if ( CP_MOVING(w) || w->shape != NULL || !holds(o, &w->work) ) {
		return 0;
	}
	rad = cp_round_of(o, w->round);
	nb = 1;
	band[0] = *o;
	if ( rad > 0 ) {
		band[0].top = o->top + rad;
		band[0].bottom = o->bottom - rad;
		band[1] = *o;
		band[1].left = o->left + rad;  band[1].right = o->right - rad;
		band[1].bottom = o->top + rad;
		band[2] = band[1];
		band[2].top = o->bottom - rad;  band[2].bottom = o->bottom;
		nb = 3;
	}
	if ( w->a_frame >= 255 && w->a_work >= 255 && !w->keyed ) {
		p[0] = *o;
		np = cut(&p[0], box) ? 1 : 0;
	} else {
		d.box = *box;
		k = pieces_of(&d, w, q);
		np = 0;
		for ( i = 0; i < k; i++ ) {
			if ( q[i].a >= 255 && ( q[i].frame || !w->keyed ) ) {
				p[np++] = q[i].r;
			}
		}
	}
	for ( i = 0; i < np; i++ ) {
		for ( k = 0; k < nb; k++ ) {
			r = p[i];
			if ( cut(&r, &band[k]) ) {
				out[n++] = r;
			}
		}
	}
	return n;
}

/*
 * What a window's spread reads within the box, laid in the rectangles
 * v[0..nv-1]: each piece not solid within them, as far round it as a
 * spread reaches. How many rectangles, up to NPIECE * HD_CUT.
 */
LOCAL INT spread_rects( CONST T_DPRECT *box, CONST T_CPWIN *w, CONST T_DPRECT *v, INT nv,
			T_DPRECT *out )
{
	T_CPDST	d;
	PIECE	p[NPIECE];
	INT	reach, np, i, k, n = 0;

	if ( w->blur <= 0 || CP_MOVING(w) ) {
		return 0;
	}
	reach = cp_spread_reach(w->blur);
	for ( k = 0; k < nv; k++ ) {
		d.box = v[k];
		np = pieces_of(&d, w, p);
		for ( i = 0; i < np; i++ ) {
			if ( p[i].a < 255 ) {
				out[n] = p[i].r;
				out[n].left -= reach;   out[n].top -= reach;
				out[n].right += reach;  out[n].bottom += reach;
				if ( cut(&out[n], box) ) {
					n++;
				}
			}
		}
	}
	return n;
}

/* A rectangle laid solid in front kept, unless one kept already holds it; when full, over the smallest */
LOCAL void front_add( CONST T_DPRECT *r )
{
	INT	i, k = -1;
	SZ	least = 0;

	for ( i = 0; i < hd_nfront; i++ ) {
		if ( holds(&hd_front[i], r) ) {
			return;
		}
	}
	for ( i = 0; i < hd_nfront; ) {
		if ( holds(r, &hd_front[i]) ) {
			hd_front[i] = hd_front[--hd_nfront];
		} else {
			i++;
		}
	}
	if ( hd_nfront < HD_KEEP ) {
		hd_front[hd_nfront++] = *r;
		return;
	}
	for ( i = 0; i < hd_nfront; i++ ) {
		if ( k < 0 || area_of(&hd_front[i]) < least ) {
			k = i;
			least = area_of(&hd_front[i]);
		}
	}
	if ( area_of(r) > least ) {
		hd_front[k] = *r;
	}
}

/* What a spread reads taken out of what is kept, as much of the rest kept as there is room for */
LOCAL void front_take( CONST T_DPRECT *c )
{
	T_DPRECT	o[4];
	INT		i, k, m, n = 0;

	for ( i = 0; i < hd_nfront; i++ ) {
		if ( !meets(&hd_front[i], c) ) {
			hd_tmp[n++] = hd_front[i];
		}
	}
	for ( i = 0; i < hd_nfront && n < HD_KEEP; i++ ) {
		if ( !meets(&hd_front[i], c) ) {
			continue;
		}
		m = rects_cut(&hd_front[i], 1, c, o, 4);
		for ( k = 0; k < m && n < HD_KEEP; k++ ) {
			hd_tmp[n++] = o[k];
		}
	}
	for ( i = 0; i < n; i++ ) {
		hd_front[i] = hd_tmp[i];
	}
	hd_nfront = n;
}

/* What is left of e with what is kept cut out of it, into v: how many rectangles */
LOCAL INT shown_rects( CONST T_DPRECT *e, T_DPRECT *v )
{
	T_DPRECT	o[HD_CUT];
	INT		i, k, m, n = 1;

	v[0] = *e;
	for ( i = 0; i < hd_nfront && n > 0; i++ ) {
		if ( !meets(&hd_front[i], e) ) {
			continue;
		}
		m = rects_cut(v, n, &hd_front[i], o, HD_CUT);
		if ( m < 0 ) {
			continue;		/* too many pieces: laid under this one too */
		}
		for ( k = 0; k < m; k++ ) {
			v[k] = o[k];
		}
		n = m;
	}
	return n;
}

/*
 * A window's stages within the rectangles v[0..nv-1] alone, its shadow
 * within s[0..ns-1] (those and what its spread reads there), each stage
 * in all of them before the next, as cp_window does them in the whole
 * part: a spread reads what the shadow left and nothing of the window
 */
LOCAL void lay_in( CONST T_CPDST *d, CONST T_DPRECT *v, INT nv, CONST T_DPRECT *s, INT ns,
		   CONST T_CPWIN *w )
{
	T_CPDST	in = *d;
	INT	k;

	if ( CP_MOVING(w) ) {
		for ( k = 0; k < nv; k++ ) {
			in.box = v[k];
			cp_moving(&in, w);
		}
		return;
	}
	for ( k = 0; k < ns; k++ ) {
		in.box = s[k];
		cp_shadow(&in, w);
	}
	if ( nv == 0 ) {
		return;
	}
	npc = 0;
	for ( k = 0; k < nv; k++ ) {
		in.box = v[k];
		npc += pieces_of(&in, w, pc + npc);
	}
	spread_pieces(d, w, &d->box);
	cp_blend(d, w);
}

EXPORT void cp_lay( CONST T_CPDST *d, CONST T_CPWIN *w, INT n )
{
	T_DPRECT	e, r[HD_SOLID];
	INT		i, k, m, ns, at = 0;
	SZ		left;

	for ( i = 0; i < n && ( !hd_on || n - i > CP_LAY_MAX ); i++ ) {
		cp_window(d, &w[i]);		/* the furthest back of more than it takes */
	}
	w += i;
	n -= i;
	if ( n <= 0 ) {
		return;
	}

	/*
	 * From the front back: where each shows; what its spread reads
	 * there, where its shadow goes down as well; then what it hides from
	 * those behind, less what that spread reads
	 */
	hd_nfront = 0;
	for ( i = n - 1; i >= 0; i-- ) {
		hd_at[i] = hd_sat[i] = at;
		hd_n[i] = hd_sn[i] = 0;
		ns = 0;
		if ( reach_of(&d->box, &w[i], &e) ) {
			hd_n[i] = hd_sn[i] = shown_rects(&e, hd_rect + at);
			left = area_of(&e);
			for ( k = 0; k < hd_n[i]; k++ ) {
				left -= area_of(&hd_rect[at + k]);
			}
			hd_hidden += (UD)left;
			at += hd_n[i];
			ns = spread_rects(&d->box, &w[i], hd_rect + hd_at[i], hd_n[i], hd_read);
			if ( ns > 0 ) {
				for ( k = 0; k < ns; k++ ) {
					front_take(&hd_read[k]);
				}
				hd_sat[i] = at;
				hd_sn[i] = shown_rects(&e, hd_rect + at);
				at += hd_sn[i];
			}
		}
		if ( i == 0 ) {
			break;
		}
		m = solid_rects(&d->box, &w[i], r);
		for ( k = 0; k < m; k++ ) {
			front_add(&r[k]);
		}
		for ( k = 0; k < ns; k++ ) {
			front_take(&hd_read[k]);
		}
	}

	/* from the back forward, each where it shows */
	for ( i = 0; i < n; i++ ) {
		lay_in(d, &hd_rect[hd_at[i]], hd_n[i], &hd_rect[hd_sat[i]], hd_sn[i], &w[i]);
	}
}

/* ---------------------------------------------------------------- the edge kept */

LOCAL void margin_copy( UB *back, UINT pitch, CONST T_DPRECT *o, CONST T_DPRECT *in, BOOL keep )
{
	UW	*m = mg_buf;
	INT	y;

	for ( y = o->top; y < o->bottom; y++ ) {
		UW	*row = (UW *)( back + (UBINT)y * pitch );
		INT	k, seg[2][2], n = 0;

		if ( y < in->top || y >= in->bottom ) {
			seg[n][0] = o->left;  seg[n][1] = o->right;  n++;
		} else {
			seg[n][0] = o->left;  seg[n][1] = in->left;  n++;
			seg[n][0] = in->right;  seg[n][1] = o->right;  n++;
		}
		for ( k = 0; k < n; k++ ) {
			SZ	len = (SZ)( seg[k][1] - seg[k][0] );

			if ( len <= 0 ) {
				continue;
			}
			if ( keep ) {
				cp_copy(m, row + seg[k][0], (INT)len);
			} else {
				cp_copy(row + seg[k][0], m, (INT)len);
			}
			m += len;
		}
	}
}

EXPORT BOOL cp_margin_keep( UB *back, UINT pitch, CONST T_DPRECT *outer, CONST T_DPRECT *inner )
{
	SZ	n = (SZ)( outer->right - outer->left ) * ( outer->bottom - outer->top )
		  - (SZ)( inner->right - inner->left ) * ( inner->bottom - inner->top );

	if ( n <= 0 ) {
		return TRUE;
	}
	if ( !room(&mg_buf, &mg_pages, n) ) {
		return FALSE;
	}
	margin_copy(back, pitch, outer, inner, TRUE);

	return TRUE;
}

EXPORT void cp_margin_put( UB *back, UINT pitch, CONST T_DPRECT *outer, CONST T_DPRECT *inner )
{
	if ( mg_buf != NULL ) {
		margin_copy(back, pitch, outer, inner, FALSE);
	}
}
