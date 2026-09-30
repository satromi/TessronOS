/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dp.h
 *	Drawing (design 16.4, phase 9c)
 *
 *	Everything is drawn through a drawing environment, which carries
 *	the things that would otherwise be passed to every call: where the
 *	origin is, what may be written to, and what colour to use. A
 *	program holds the number of one and draws through it.
 *
 *	Two rectangles decide what a call may touch. The clip frame is the
 *	manager's: it is set when a window's shape changes and says what
 *	part of the screen belongs to that window at all. The visible
 *	region is the program's own and may be narrowed further. A pixel is
 *	written only if it is inside both.
 *
 *	Nothing here reaches the screen by itself. Drawing changes the back
 *	buffer and says what changed; when it arrives is the screen's
 *	business (design 16.5).
 */

#ifndef __TS_DP_H__
#define __TS_DP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/disp.h>

/* A part of the screen. The right and bottom edges are past the end. */
typedef struct {
	INT	left, top, right, bottom;
} T_DPRECT;

/* A point */
typedef struct {
	INT	x, y;
} T_DPPOINT;

/*
 * How what is drawn meets what is there. Copy is the usual one; the
 * other two are what a rubber band and a highlight are made of.
 */
#define DP_MODE_COPY	0
#define DP_MODE_XOR	1		/* drawing twice puts it back */
#define DP_MODE_AND	2

/* ---------------------------------------------------------------- patterns */

/*
 * A pattern is what a drawing call puts down. Everything that fills or
 * draws takes one; a plain colour is the pattern that covers everything
 * in that colour, which is why the calls that take a colour are the
 * short way of saying the same thing.
 *
 * There are two kinds. A tone is two colours in one of eight dithers,
 * from nothing through halves to solid; it costs no memory and is what
 * grey comes from on a screen that has no grey. A tile is a block of
 * pixels the caller owns, laid over and over from the environment's
 * origin, with a mask saying which of its pixels show.
 *
 * A pattern is laid from the origin, not from the rectangle being
 * filled, so that two calls filling neighbouring rectangles in the same
 * pattern meet without a seam.
 */
#define DP_PAT_TONE	0
#define DP_PAT_TILE	1

#define DP_TONE_MAX	8		/* 1 is all of the second colour, 7 all of the first */

typedef struct {
	UINT	kind;
	UW	fore, back;		/* a tone's two colours */
	INT	tone;			/* 0..7 */
	INT	hs, vs;			/* a tile's size in pixels */
	CONST UW *tile;			/* vs rows of hs pixels */
	CONST UW *mask;			/* a bit per pixel, rows of 32 with the
					   leftmost column in the top bit;
					   NULL when all of the tile shows */
} T_DPPAT;

/*
 * The eight dithers, four rows of thirty-two columns each. A tone is
 * read at (x & 31) of row (y & 3): the bit set means the first colour,
 * clear the second.
 */
IMPORT CONST UW dp_tone_rows[DP_TONE_MAX][4];

/* The pattern of one colour, and of a tone of two */
IMPORT void dp_pat_colour( T_DPPAT *pat, UW colour );
IMPORT void dp_pat_tone( T_DPPAT *pat, INT tone, UW fore, UW back );

/* Open a drawing environment and close it again */
IMPORT INT dp_open( void );
IMPORT ER  dp_close( INT gid );

/*
 * Where the origin of this environment sits on the screen. Everything
 * a program passes afterwards is measured from there.
 */
IMPORT ER  dp_set_origin( INT gid, INT x, INT y );

/* What this environment may write to at all, in screen coordinates */
IMPORT ER  dp_set_frame( INT gid, CONST T_DPRECT *r );

/* What the program has narrowed it to, measured from the origin */
IMPORT ER  dp_set_visible( INT gid, CONST T_DPRECT *r );

/*
 * Where this environment's pixels go. With no target set it is the back
 * buffer and the corner is the corner of the screen. A window sets its
 * own surface and the place on the screen that surface's corner sits.
 *
 * Coordinates do not change: they stay screen coordinates throughout.
 * What changes is the address a coordinate lands on, and the offset
 * taken is the target's own corner, never anything else's. Getting that
 * wrong writes outside the surface.
 *
 * Nothing is sent to the screen while the target is not the screen. The
 * pixels belong to whoever set the target, and it decides when they are
 * laid down.
 */
IMPORT ER  dp_set_target( INT gid, void *pixels, UINT pitch,
			  INT corner_x, INT corner_y );

/*
 * A window's environment moved with it, all at once: its surface and
 * the surface's corner, the origin and the frame at the work area, and
 * the visible part the whole work area. Set one after another, a draw
 * in between -- from the window's program, while the window is moved --
 * would take the new corner with the old frame and write outside the
 * surface.
 */
IMPORT ER  dp_set_place( INT gid, void *pixels, UINT pitch, INT corner_x, INT corner_y,
			 CONST T_DPRECT *work );

IMPORT ER  dp_set_colour( INT gid, UW fore, UW back );
IMPORT ER  dp_set_mode( INT gid, UINT mode );

/* What the environment is now */
typedef struct {
	T_DPPOINT	origin;
	T_DPRECT	frame;		/* screen coordinates */
	T_DPRECT	visible;	/* from the origin */
	UW		fore, back;
	UINT		mode;
} T_DPENV;

IMPORT ER  dp_ref( INT gid, T_DPENV *env );

/* ---------------------------------------------------------------- drawing */

/* One pixel. Reading answers what the back buffer holds. */
IMPORT ER  dp_put_pixel( INT gid, INT x, INT y, UW colour );
IMPORT INT dp_get_pixel( INT gid, INT x, INT y );

/* A solid rectangle, and the outline of one */
IMPORT ER  dp_fill_rect( INT gid, CONST T_DPRECT *r, UW colour );
IMPORT ER  dp_frame_rect( INT gid, CONST T_DPRECT *r, UW colour, INT width );

/* A straight line between two points, both ends drawn */
IMPORT ER  dp_line( INT gid, INT x0, INT y0, INT x1, INT y1, UW colour );

/* The same three in a pattern rather than a colour */
IMPORT ER  dp_fill_rect_pat( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat );
IMPORT ER  dp_frame_rect_pat( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat,
			      INT width );
IMPORT ER  dp_line_pat( INT gid, INT x0, INT y0, INT x1, INT y1,
			CONST T_DPPAT *pat );

/* ---------------------------------------------------------------- regions */

/*
 * A shape that is not a rectangle: a list of bands, each covering a run
 * of rows and holding sorted spans. A rectangle is the region of one
 * band with one span, which is the common case and costs almost
 * nothing.
 *
 * What the window manager wants these for is the part of a window that
 * nothing in front of it covers. What a program wants them for is
 * drawing through a shape it chose. Both are the same thing, so there
 * is one of them.
 *
 * The caller owns what it makes and frees it. A region handed to a
 * drawing environment is copied there, because a shape held by pointer
 * is a shape that can be pulled away while it is being drawn through.
 */
typedef struct dp_region T_DPRGN;

IMPORT ER   dp_rgn_rect( CONST T_DPRECT *r, T_DPRGN **p_rgn );
IMPORT ER   dp_rgn_copy( CONST T_DPRGN *src, T_DPRGN **p_rgn );
IMPORT void dp_rgn_free( T_DPRGN *rgn );

IMPORT ER   dp_rgn_or( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out );
IMPORT ER   dp_rgn_and( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out );
IMPORT ER   dp_rgn_sub( CONST T_DPRGN *a, CONST T_DPRGN *b, T_DPRGN **p_out );

/* The region of a rounded box, the same shape the fill below puts down */
IMPORT ER   dp_rgn_round( CONST T_DPRECT *r, INT rx, INT ry, T_DPRGN **p_out );

/* A region from a run of rows, each with one span */
IMPORT ER   dp_rgn_from_rows( INT y0, INT n, CONST INT *x0, CONST INT *x1,
			      T_DPRGN **p_out );

IMPORT ER   dp_rgn_bound( CONST T_DPRGN *rgn, T_DPRECT *out );
IMPORT BOOL dp_rgn_empty( CONST T_DPRGN *rgn );
IMPORT BOOL dp_rgn_has( CONST T_DPRGN *rgn, INT x, INT y );
IMPORT ER   dp_rgn_size( CONST T_DPRGN *rgn, INT *p_bands, INT *p_spans );

/* The spans of one row: how many, and where they are */
IMPORT INT  dp_rgn_row( CONST T_DPRGN *rgn, INT y, CONST INT **p_spans );

/*
 * Draw through a shape. The region is copied into the environment and
 * narrows what it may touch, the way the visible rectangle does; NULL
 * takes the narrowing away again. The coordinates are the ones the
 * caller draws in, measured from the origin.
 */
IMPORT ER   dp_set_region( INT gid, CONST T_DPRGN *rgn );

/* ---------------------------------------------------------------- shapes */

/*
 * How a polygon decides what is inside it: the crossings taken in pairs,
 * or the turns counted. They differ only where a polygon crosses
 * itself, and that is exactly where a drawing program needs to choose.
 */
#define DP_POLY_ODD	0
#define DP_POLY_WIND	1

/* What a piece of an ellipse is bounded by on its straight side */
#define DP_PIECE_SECTOR	0		/* the two lines from the middle */
#define DP_PIECE_CHORD	1		/* the line joining the two ends */

/* What a line is made of */
#define DP_LINE_SOLID	0
#define DP_LINE_DOT	1
#define DP_LINE_DASH	2
#define DP_LINE_CHAIN	3

/* One run of pixels on one row: what every shape comes down to */
IMPORT ER  dp_fill_run( INT gid, INT x0, INT x1, INT y, CONST T_DPPAT *pat );

/*
 * A bitmap of one bit per pixel, put down in one colour. What draws
 * letters uses this: a page is thousands of them, and going through
 * this layer once per pixel would mean taking its lock a million times.
 */
IMPORT ER  dp_put_mono( INT gid, INT x, INT y, CONST UB *bits, INT pitch,
			INT w, INT h, UW colour );

/*
 * A bitmap of how much of each pixel is covered (a byte a pixel, 0 to
 * 255), put down in one colour mixed with what is there in that share:
 * what letters with smoothed edges are drawn with.
 */
IMPORT ER  dp_put_gray( INT gid, INT x, INT y, CONST UB *cov, INT pitch,
			INT w, INT h, UW colour );

/*
 * A picture of true-colour pixels, laid a row at a time. 'pitch' is in
 * pixels. A pixel equal to 'clear' is left alone, which is how a
 * picture gets a shape other than its rectangle.
 */
IMPORT ER  dp_put_argb( INT gid, INT x, INT y, CONST UW *pixels, INT pitch,
			INT w, INT h, UW clear );

/*
 * The other way: a rectangle read back into the caller's memory, so
 * that whatever covers it for a while can put it back afterwards.
 */
IMPORT ER  dp_get_argb( INT gid, INT x, INT y, UW *pixels, INT pitch,
			INT w, INT h );

/* What this environment may touch, in the caller's own coordinates */
IMPORT ER  dp_clip_rect( INT gid, T_DPRECT *out );

IMPORT ER  dp_fill_poly( INT gid, CONST T_DPPOINT *p, INT n, UINT rule,
			 CONST T_DPPAT *pat );
IMPORT ER  dp_draw_poly( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed,
			 INT width, UINT kind, CONST T_DPPAT *pat );

/*
 * A piece of an ellipse. The two ends are given as points measured from
 * the middle of the rectangle, the way a hand of a clock points, not as
 * angles: a point cannot be rounded to the wrong side of a pixel.
 */
IMPORT ER  dp_fill_sector( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
			   INT ex, INT ey, CONST T_DPPAT *pat );
IMPORT ER  dp_fill_chord( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
			  INT ex, INT ey, CONST T_DPPAT *pat );
IMPORT ER  dp_draw_arc( INT gid, CONST T_DPRECT *r, INT sx, INT sy,
			INT ex, INT ey, INT width, CONST T_DPPAT *pat );

/* A line of a width and a kind */
IMPORT ER  dp_line_wide( INT gid, INT x0, INT y0, INT x1, INT y1, INT width,
			 UINT kind, CONST T_DPPAT *pat );

/* Spread from a place until the colour under it runs out */
IMPORT ER  dp_fill_seed( INT gid, INT x, INT y, CONST T_DPPAT *pat );

/* ------------------------------------------- ovals and rounded boxes */

/*
 * A rounded box, and its outline. The two radii are the width and the
 * height of the corner; a radius that reaches past the side it is on is
 * cut to it, so DP_ROUND_FULL in both gives an oval and is what the two
 * calls after these pass.
 *
 * These are what a raised part is made of: the shape is drawn three
 * times -- filled, outlined in the shaded colour, and outlined again in
 * the lit colour with the near corner showing -- and that is the whole
 * of how a button looks.
 */
#define DP_ROUND_FULL	0xFFFF

IMPORT ER  dp_fill_round( INT gid, CONST T_DPRECT *r, INT rx, INT ry,
			  CONST T_DPPAT *pat );
IMPORT ER  dp_frame_round( INT gid, CONST T_DPRECT *r, INT rx, INT ry,
			   INT width, CONST T_DPPAT *pat );
IMPORT ER  dp_fill_oval( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat );
IMPORT ER  dp_frame_oval( INT gid, CONST T_DPRECT *r, INT width,
			  CONST T_DPPAT *pat );

/*
 * Move a rectangle of pixels within the back buffer. Overlapping source
 * and destination work: a window being dragged is the reason this
 * exists.
 */
IMPORT ER  dp_copy_rect( INT gid, CONST T_DPRECT *src, INT to_x, INT to_y );

/*
 * Hold the screen for a run of drawing and let it go. These are the
 * screen's own, offered here so that a program drawing through an
 * environment need not know about the screen at all.
 */
IMPORT ER  dp_hold( void );
IMPORT ER  dp_release( void );

/* What the environments have drawn, for a test or a measurement */
typedef struct {
	UD	calls;			/* drawing calls that did something */
	UD	pixels;			/* pixels written */
	UD	clipped;		/* calls that fell wholly outside */
} T_DPSTAT;

IMPORT ER  dp_stat( T_DPSTAT *st );

IMPORT INT knl_dp_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DP_H__ */
