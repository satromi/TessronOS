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

/*
 * An environment of memory: one that draws into a picture rather than
 * onto the screen (メモリの描画環境). The picture is w by h pixels of
 * argb, pitch bytes from the start of one row to the next; its first
 * pixel is 0, 0, and the origin, the frame and the visible part are all
 * of it. Nothing drawn through it reaches the screen, and what is drawn
 * is in the picture once dp_close has closed it.
 *
 * Opened by a process (design A, class 8), the kernel draws into a copy
 * of the picture taken as it is opened, and writes it back into the
 * process's picture at dp_close; only that process draws through it,
 * and it goes away with the process. A picture of more than
 * DP_MEM_MAX pixels is refused (E_PAR).
 */
typedef struct {
	UW	*px;
	INT	pitch;			/* bytes */
	INT	w, h;
} T_DPBMP;

#define DP_MEM_MAX	( 4096 * 4096 )

IMPORT INT dp_open_mem( CONST T_DPBMP *bmp );

/*
 * An environment that draws into pixels laid onto the screen later (a
 * window's) can say what it touches as it draws: with the watch on,
 * every rectangle drawn through it is handed, in screen coordinates and
 * cut to what it may touch, to the one function set by dp_watch. The
 * window manager keeps those as what is to be built again, so a window
 * drawn into needs no word of its own of what changed. The function is
 * called with no lock of this layer held.
 */
IMPORT ER   dp_set_watch( INT gid, BOOL on );
IMPORT void dp_watch( void (*fn)( CONST T_DPRECT *r ) );

/*
 * An environment whose target holds the screen's pixels at a scale
 * (percent, 100..400): its coordinates are view pixels, and each view
 * pixel is drawn as the rectangle of the target's pixels it covers,
 * floor((x - corner) * scale / 100) up to where the next begins. What is
 * drawn there is what would be drawn at 100, made larger; letters are
 * made at the target's own size (fn.c). The window manager sets it on
 * the environments of the windows. dp_scale answers 100 for one that
 * draws as it is (scale 100, or no target of its own).
 */
IMPORT ER   dp_set_scale( INT gid, UINT scale );
IMPORT INT  dp_scale( INT gid );

/*
 * Pixels of the target's own size, for a scaled environment:
 *
 *   dp_put_gray_dev, dp_put_mono_dev  a letter made at the target's size
 *		(shares of 255, or a bit a pixel), its corner dx, dy target
 *		pixels from where view place (x, y) lies
 *   dp_put_image	a picture of pw by ph laid over the view rectangle
 *		'box', the nearest of its pixels taken for each target pixel
 *		the box covers (an icon larger than its box shows its detail)
 *   dp_get_dev, dp_put_dev	the target pixels a view rectangle covers,
 *		taken (from the heap, Kfree) and put back as they were
 *
 * Unscaled, each is the call of the same name without _dev: the place
 * is (x + dx, y + dy), and a picture is taken again at the box's size.
 */
IMPORT ER  dp_put_gray_dev( INT gid, INT x, INT y, INT dx, INT dy, CONST UB *cov, INT pitch,
			    INT w, INT h, UW colour );
IMPORT ER  dp_put_mono_dev( INT gid, INT x, INT y, INT dx, INT dy, CONST UB *bits, INT pitch,
			    INT w, INT h, UW colour );
IMPORT ER  dp_put_image( INT gid, CONST T_DPRECT *box, CONST UW *px, INT pw, INT ph,
			 INT pitch, UW clear );
IMPORT ER  dp_get_dev( INT gid, CONST T_DPRECT *r, UW **p_px, INT *p_w, INT *p_h );
IMPORT ER  dp_put_dev( INT gid, CONST T_DPRECT *r, CONST UW *px, INT w, INT h );

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

/* ---------------------------------------------------------------- recording */

/*
 * An environment that keeps what is drawn as drawing, not as pixels:
 * what a page is printed through (design 11.22). Every call that
 * would draw through it is handed, as it was asked, to the functions
 * of whoever opened it -- a rectangle stays a rectangle, a line a line,
 * a picture a picture -- and the letters a face draws through it come
 * as glyphs of that face (fn.c asks dp_rec_glyph). Nothing is drawn
 * into memory and nothing reaches the screen.
 *
 * The origin, the frame, the visible rectangle and the region work as
 * they do for any environment; each call carries them in T_DPRECST,
 * the clip as one rectangle in the coordinates past the origin (a
 * region is taken as its bounds). A call drawn with a mode other than
 * copy -- a rubber band, a highlight -- is not passed on: it is not
 * part of what is printed.
 *
 * A shape the functions do not take (dp_fill_sector and the rest of
 * the pieces of an ellipse, dp_fill_seed) is drawn into pixels the
 * size of its box and handed on as a picture.
 */
#define DP_REC_RECT	1		/* r filled */
#define DP_REC_FRAME	2		/* r outlined inside, 'width' wide */
#define DP_REC_LINE	3		/* p[0] to p[1], both ends drawn */
#define DP_REC_FILL	4		/* the polygon p[0 .. n) filled by 'rule' */
#define DP_REC_STROKE	5		/* the line through p[0 .. n), 'closed' or not */
#define DP_REC_ROUND	6		/* r with corners rx by ry, filled */
#define DP_REC_ROUNDF	7		/* and outlined inside, 'width' wide */

typedef struct {
	UINT		kind;		/* DP_REC_* */
	T_DPRECT	r;
	CONST T_DPPOINT	*p;
	INT		n;
	INT		width;		/* 0 is 1 */
	UINT		line;		/* DP_LINE_* */
	UINT		rule;		/* DP_POLY_* */
	BOOL		closed;
	INT		rx, ry;
} T_DPRECOP;

typedef struct {
	INT		ox, oy;		/* the origin: add it to what a call gives */
	T_DPRECT	clip;		/* what may be touched, past the origin */
} T_DPRECST;

/* A picture handed on */
#define DP_REC_ARGB	0		/* 'px' rows of UW, 'colour' the clear pixel */
#define DP_REC_MONO	1		/* a bit a pixel, in 'colour' */
#define DP_REC_GRAY	2		/* a byte of cover a pixel, in 'colour' */

typedef struct {
	UINT		kind;		/* DP_REC_ARGB .. DP_REC_GRAY */
	INT		x, y, w, h;
	CONST void	*px;
	INT		pitch;		/* pixels for ARGB, bytes otherwise */
	UW		colour;
} T_DPRECIMG;

/*
 * A glyph of a face: its number in the face and the letter it was
 * drawn for, the size of the em in pixels, and where it lands as a
 * matrix of 16.16 numbers taking the glyph's own space -- pixels at
 * that size, y going up -- to the caller's: x' = a x + c y + e,
 * y' = b x + d y + f, m[] being a, b, c, d, e, f. 'bold' is how much
 * it was thickened, in 16.16 pixels; 0 when it was not.
 */
typedef struct {
	ID		fid;
	UINT		gi;
	UW		cp;
	INT		px;
	W		m[6];
	W		bold;
	UW		colour;
} T_DPRECGLY;

typedef struct {
	void	(*draw)( void *arg, CONST T_DPRECST *st, CONST T_DPRECOP *op,
			 CONST T_DPPAT *pat );
	void	(*image)( void *arg, CONST T_DPRECST *st, CONST T_DPRECIMG *im );
	void	(*glyph)( void *arg, CONST T_DPRECST *st, CONST T_DPRECGLY *g );
} T_DPREC;

/*
 * An environment that records, its frame w by h from its own corner.
 * It is closed with dp_close like any other.
 */
IMPORT INT  dp_open_rec( CONST T_DPREC *ops, void *arg, INT w, INT h );
IMPORT BOOL dp_is_rec( INT gid );
/* A glyph passed on through a recording environment; FALSE for any other */
IMPORT BOOL dp_rec_glyph( INT gid, CONST T_DPRECGLY *g );

/* What the environments have drawn, for a test or a measurement */
typedef struct {
	UD	calls;			/* drawing calls that did something */
	UD	pixels;			/* pixels written */
	UD	clipped;		/* calls that fell wholly outside */
} T_DPSTAT;

IMPORT ER  dp_stat( T_DPSTAT *st );

IMPORT INT knl_dp_init( void );
IMPORT void knl_dp_screen_changed( void );	/* the screen's size taken again */

#ifdef __cplusplus
}
#endif

#endif /* __TS_DP_H__ */
