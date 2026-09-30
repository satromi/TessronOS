/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dp_shape.c
 *	Shapes: polygons, arcs, sectors, chords, seeded fills, wide and
 *	dotted lines, and moving pixels between environments (design 16.4)
 *
 *	Everything here comes down to runs of pixels on one row, which is
 *	the one thing the drawing layer knows how to put down; what differs
 *	between a polygon and a sector is only how the runs of a row are
 *	worked out.
 *
 *	A polygon is done by the crossings of its edges with the middle of
 *	each row, sorted, and taken in pairs (or by winding, when that is
 *	what was asked for). Rows are sampled at their middle rather than
 *	their top edge, so that two polygons sharing an edge meet without a
 *	seam and without drawing the shared pixels twice.
 *
 *	A sector and a chord are the ellipse cut by the two lines from its
 *	middle to the ends given. Which side of a line a point lies on is
 *	the sign of a cross product, so none of this needs angles, and
 *	therefore none of it needs a trigonometric table or a division that
 *	can lose a pixel.
 *
 *	A seeded fill spreads by runs: a run is filled, the runs above and
 *	below it that are still the colour it started from are remembered,
 *	and the work goes on until nothing is left. The memory it takes is
 *	the runs waiting, not the pixels, so a large plain area costs
 *	almost nothing.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>
#include <ts/disp.h>

#define SHAPE_MAX_X	64		/* crossings of one row a polygon may have */
#define SEED_MAX	2048		/* runs waiting in a seeded fill */

/* ---------------------------------------------------------------- polygons */

/* The crossings of the edges with the middle of a row, sorted */
LOCAL INT row_cross( CONST T_DPPOINT *p, INT n, INT y, INT *xs, INT *dir )
{
	INT	i, m = 0;
	D	fy = (D)y * 2 + 1;		/* the row's middle, doubled */

	for ( i = 0; i < n && m < SHAPE_MAX_X; i++ ) {
		CONST T_DPPOINT	*a = &p[i];
		CONST T_DPPOINT	*b = &p[(i + 1) % n];
		D		y0 = (D)a->y * 2, y1 = (D)b->y * 2;
		D		x;

		if ( y0 == y1 ) {
			continue;		/* an edge along the row crosses nothing */
		}
		if ( ( fy < y0 && fy < y1 ) || ( fy > y0 && fy > y1 ) ) {
			continue;
		}
		x = (D)a->x + ((D)(b->x - a->x) * (fy - y0)) / (y1 - y0);
		xs[m] = (INT)x;
		dir[m] = ( y1 > y0 ) ? 1 : -1;
		m++;
	}
	/* sorted, carrying each crossing's direction with it */
	for ( i = 1; i < m; i++ ) {
		INT	x = xs[i], d = dir[i], j = i - 1;

		while ( j >= 0 && xs[j] > x ) {
			xs[j + 1] = xs[j];
			dir[j + 1] = dir[j];
			j--;
		}
		xs[j + 1] = x;
		dir[j + 1] = d;
	}

	return m;
}

EXPORT ER dp_fill_poly( INT gid, CONST T_DPPOINT *p, INT n, UINT rule,
			CONST T_DPPAT *pat )
{
	T_DPRECT	box;
	INT		xs[SHAPE_MAX_X], dir[SHAPE_MAX_X];
	INT		i, y, m, wind;

	if ( p == NULL || n < 3 || pat == NULL ) {
		return E_PAR;
	}
	box.left = box.right = p[0].x;
	box.top = box.bottom = p[0].y;
	for ( i = 1; i < n; i++ ) {
		if ( p[i].x < box.left )   box.left = p[i].x;
		if ( p[i].x > box.right )  box.right = p[i].x;
		if ( p[i].y < box.top )    box.top = p[i].y;
		if ( p[i].y > box.bottom ) box.bottom = p[i].y;
	}
	for ( y = box.top; y <= box.bottom; y++ ) {
		m = row_cross(p, n, y, xs, dir);
		if ( rule == DP_POLY_WIND ) {
			wind = 0;
			for ( i = 0; i < m - 1; i++ ) {
				wind += dir[i];
				if ( wind != 0 && xs[i + 1] > xs[i] ) {
					dp_fill_run(gid, xs[i], xs[i + 1], y, pat);
				}
			}
		} else {
			for ( i = 0; i + 1 < m; i += 2 ) {
				if ( xs[i + 1] > xs[i] ) {
					dp_fill_run(gid, xs[i], xs[i + 1], y, pat);
				}
			}
		}
	}

	return E_OK;
}

EXPORT ER dp_draw_poly( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed,
			INT width, UINT kind, CONST T_DPPAT *pat )
{
	INT	i;
	ER	er;

	if ( p == NULL || n < 2 || pat == NULL ) {
		return E_PAR;
	}
	for ( i = 0; i + 1 < n; i++ ) {
		er = dp_line_wide(gid, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y,
				  width, kind, pat);
		if ( er < E_OK ) {
			return er;
		}
	}
	if ( closed ) {
		return dp_line_wide(gid, p[n - 1].x, p[n - 1].y, p[0].x, p[0].y,
				    width, kind, pat);
	}

	return E_OK;
}

/* ------------------------------------------------------- arcs and sectors */

/*
 * Which side of a line through the middle a point falls on. The rows of
 * a screen run downwards, so the sign is taken the other way round:
 * what this answers is the turn as it is seen, not as the numbers go,
 * and an arc that runs from east to north is the quarter a person would
 * draw with their hand.
 */
LOCAL D cross( INT ax, INT ay, INT bx, INT by )
{
	return (D)ay * bx - (D)ax * by;
}

/*
 * Whether a point lies in the wedge that runs from the start to the end,
 * the way the hands of a clock run backwards. A wedge wider than half a
 * turn is the two outer pieces rather than the one between, which is
 * what the sign of the cross product of the two ends says.
 */
LOCAL BOOL in_wedge( INT sx, INT sy, INT ex, INT ey, INT px, INT py )
{
	D	se = cross(sx, sy, ex, ey);
	D	sp = cross(sx, sy, px, py);
	D	pe = cross(px, py, ex, ey);

	if ( sx == ex && sy == ey ) {
		return TRUE;			/* the whole way round */
	}
	if ( se >= 0 ) {
		return ( sp >= 0 && pe >= 0 );
	}

	return ( sp >= 0 || pe >= 0 );
}

/*
 * A piece of an ellipse. 'kind' says whether the straight edge is the
 * two radii (a sector) or the line joining the ends (a chord), and
 * whether the inside is filled or only its edge is drawn.
 */
LOCAL ER oval_piece( INT gid, CONST T_DPRECT *r, INT sx, INT sy, INT ex, INT ey,
		     UINT kind, INT width, CONST T_DPPAT *pat )
{
	INT	cx, cy, x, y, a, b;
	D	inside;

	if ( r == NULL || pat == NULL ) {
		return E_PAR;
	}
	cx = (r->left + r->right) / 2;
	cy = (r->top + r->bottom) / 2;
	a = (r->right - r->left) / 2;
	b = (r->bottom - r->top) / 2;
	if ( a <= 0 || b <= 0 ) {
		return E_PAR;
	}
	for ( y = r->top; y < r->bottom; y++ ) {
		INT	run0 = -1;

		for ( x = r->left; x < r->right; x++ ) {
			INT	dx = x - cx, dy = y - cy;
			BOOL	take;

			/* inside the ellipse: (dx/a)^2 + (dy/b)^2 <= 1 */
			inside = (D)dx * dx * b * b + (D)dy * dy * a * a
				 - (D)a * a * b * b;
			take = ( inside <= 0 );
			if ( take && width > 0 ) {
				/* only the rim, when a width was asked for */
				INT	ia = a - width, ib = b - width;

				if ( ia > 0 && ib > 0 ) {
					D	in2 = (D)dx * dx * ib * ib
						      + (D)dy * dy * ia * ia
						      - (D)ia * ia * ib * ib;

					if ( in2 <= 0 ) {
						take = FALSE;
					}
				}
			}
			if ( take && kind == DP_PIECE_CHORD ) {
				/* on the far side of the line joining the ends */
				take = ( cross(ex - sx, ey - sy,
					       x - (cx + sx), y - (cy + sy)) <= 0 );
			} else if ( take ) {
				take = in_wedge(sx, sy, ex, ey, dx, dy);
			}
			if ( take ) {
				if ( run0 < 0 ) {
					run0 = x;
				}
			} else if ( run0 >= 0 ) {
				dp_fill_run(gid, run0, x, y, pat);
				run0 = -1;
			}
		}
		if ( run0 >= 0 ) {
			dp_fill_run(gid, run0, r->right, y, pat);
		}
	}

	return E_OK;
}

EXPORT ER dp_fill_sector( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
			  INT ex, INT ey, CONST T_DPPAT *pat )
{
	return oval_piece(gid, r, sx, sy, ex, ey, DP_PIECE_SECTOR, 0, pat);
}

EXPORT ER dp_fill_chord( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
			 INT ex, INT ey, CONST T_DPPAT *pat )
{
	return oval_piece(gid, r, sx, sy, ex, ey, DP_PIECE_CHORD, 0, pat);
}

EXPORT ER dp_draw_arc( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
		       INT ex, INT ey, INT width, CONST T_DPPAT *pat )
{
	if ( width <= 0 ) {
		width = 1;
	}

	return oval_piece(gid, r, sx, sy, ex, ey, DP_PIECE_SECTOR, width, pat);
}

/* ---------------------------------------------------------------- wide lines */

/*
 * A line a given number of pixels wide, and dotted when asked for. The
 * width is laid as a square brush along the line rather than as a
 * rectangle turned to face it: at the widths a frame or a rule is drawn
 * with, the two are the same picture, and the brush needs no arithmetic
 * that can overflow.
 *
 * The kinds are the mask the brush is put down through, so a dotted
 * line of any width is the same walk.
 */
LOCAL UW line_mask( UINT kind )
{
	switch ( kind ) {
	case DP_LINE_DOT:	return 0xAAAAAAAAU;	/* one on, one off */
	case DP_LINE_DASH:	return 0xF0F0F0F0U;	/* four on, four off */
	case DP_LINE_CHAIN:	return 0xFF18FF18U;	/* long, gap, dot, gap */
	default:		return 0xFFFFFFFFU;
	}
}

EXPORT ER dp_line_wide( INT gid, INT x0, INT y0, INT x1, INT y1, INT width,
			UINT kind, CONST T_DPPAT *pat )
{
	INT	dx, dy, sx, sy, err, e2, step = 0, half;
	UW	mask = line_mask(kind);

	if ( pat == NULL ) {
		return E_PAR;
	}
	if ( width <= 1 && kind == DP_LINE_SOLID ) {
		return dp_line_pat(gid, x0, y0, x1, y1, pat);
	}
	if ( width < 1 ) {
		width = 1;
	}
	half = width / 2;
	dx = ( x1 > x0 ) ? x1 - x0 : x0 - x1;
	dy = ( y1 > y0 ) ? y1 - y0 : y0 - y1;
	sx = ( x0 < x1 ) ? 1 : -1;
	sy = ( y0 < y1 ) ? 1 : -1;
	err = dx - dy;

	for (;;) {
		if ( ( mask >> (step & 31) ) & 1U ) {
			INT	k;

			for ( k = 0; k < width; k++ ) {
				dp_fill_run(gid, x0 - half, x0 - half + width,
					    y0 - half + k, pat);
			}
		}
		step++;
		if ( x0 == x1 && y0 == y1 ) {
			break;
		}
		e2 = err * 2;
		if ( e2 > -dy ) {
			err -= dy;
			x0 += sx;
		}
		if ( e2 < dx ) {
			err += dx;
			y0 += sy;
		}
	}

	return E_OK;
}

/* ---------------------------------------------------------------- seeded fill */

typedef struct {
	INT	x0, x1, y, dy;
} SEEDRUN;

/*
 * Spread from a place until the colour under it runs out. What bounds
 * the fill is the colour the first pixel had: every pixel of that
 * colour that can be reached sideways, up or down is filled.
 */
EXPORT ER dp_fill_seed( INT gid, INT x, INT y, CONST T_DPPAT *pat )
{
	SEEDRUN		*stack;
	T_DPRECT	clip;
	INT		top = 0;
	INT		target, x0, x1, i;
	ER		er = E_OK;

	if ( pat == NULL ) {
		return E_PAR;
	}
	/* the fill stops at the edge of what may be touched, whatever the
	   colour does: a fill is not a way out of a window */
	if ( dp_clip_rect(gid, &clip) < E_OK ) {
		return E_OK;
	}
	if ( x < clip.left || x >= clip.right
	  || y < clip.top || y >= clip.bottom ) {
		return E_PAR;
	}
	target = dp_get_pixel(gid, x, y);
	if ( target < 0 ) {
		return E_PAR;			/* outside what may be touched */
	}
	stack = (SEEDRUN *)Kmalloc(sizeof(SEEDRUN) * SEED_MAX);
	if ( stack == NULL ) {
		return E_NOMEM;
	}
	stack[top].x0 = x;  stack[top].x1 = x;
	stack[top].y = y;   stack[top].dy = 0;
	top++;

	while ( top > 0 ) {
		SEEDRUN	run = stack[--top];
		INT	ny;

		/* how far the run reaches either way at this row */
		x0 = run.x0;
		while ( x0 - 1 >= clip.left
		     && dp_get_pixel(gid, x0 - 1, run.y) == target ) {
			x0--;
		}
		x1 = run.x1;
		while ( x1 + 1 < clip.right
		     && dp_get_pixel(gid, x1 + 1, run.y) == target ) {
			x1++;
		}
		dp_fill_run(gid, x0, x1 + 1, run.y, pat);

		/* the rows above and below, wherever they are still the colour */
		for ( i = 0; i < 2; i++ ) {
			ny = run.y + ( ( i == 0 ) ? -1 : 1 );
			if ( ny < clip.top || ny >= clip.bottom ) {
				continue;
			}
			if ( run.dy != 0 && ny == run.y - run.dy ) {
				continue;	/* where we came from */
			}
			{
				INT	sx = x0;

				while ( sx <= x1 ) {
					if ( dp_get_pixel(gid, sx, ny) != target ) {
						sx++;
						continue;
					}
					if ( top >= SEED_MAX ) {
						er = E_LIMIT;
						break;
					}
					stack[top].x0 = sx;
					stack[top].x1 = sx;
					stack[top].y = ny;
					stack[top].dy = ny - run.y;
					top++;
					while ( sx <= x1
					     && dp_get_pixel(gid, sx, ny) == target ) {
						sx++;
					}
				}
			}
			if ( er < E_OK ) {
				break;
			}
		}
		if ( er < E_OK ) {
			break;
		}
	}
	Kfree(stack);

	return er;
}
