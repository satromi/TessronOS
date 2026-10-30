/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fn.c
 *	Letters on the screen (design 16.6 stage 9c)
 *
 *	Three things happen here and nothing else does. A face is opened
 *	from a block of bytes. A size is set. A string of UTF-8 is walked,
 *	each character turned into a pattern of pixels by FreeType, and the
 *	pattern put down through a drawing environment.
 *
 *	The walk is the same whether the caller wants to draw, to measure,
 *	or to know where a string stops fitting, so it is written once and
 *	told which of the three it is doing. A measuring pass that is not
 *	the drawing pass is how text ends up drawn one pixel wider than it
 *	was measured.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include <ts/fn.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <ts/fs.h>
#include <ts/ob.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_TRIGONOMETRY_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#define VSUB_MAX	16		/* subtables of the vertical forms kept */

typedef struct {
	BOOL		used;
	FT_Face		face;
	INT		px;			/* the size it is set to */
	UINT		style;			/* FN_ST_* it is drawn in */
	INT		xpx;			/* how wide, when not px */
	INT		angle;			/* turned clockwise, in degrees */
	INT		fd;			/* the file it is read from, -1 for none */
	FT_StreamRec	stream;			/* only while fd is open */
	UB		*bytes;			/* the whole file, when it was read in */
	UD		pages;			/* how many pages that took */

	/*
	 * The vertical forms (FN_ST_VERT): the face's table of
	 * substitutions, read the first time a letter is wanted in them,
	 * and where in it each single substitution of its 'vert' feature
	 * begins.
	 */
	BOOL		gsub_read;
	UB		*gsub;
	UW		gsub_len;
	INT		nvsub;
	UW		vsub[VSUB_MAX];

	/* the object and the face of it this was opened from (fn_open_obj) */
	BOOL		from_obj;
	TS_UUID		obj;
	INT		index;
	T_FNREG		reg;			/* what it is registered as */
} FONT;

LOCAL FT_Library	fn_lib = NULL;
LOCAL FONT		fn_tab[FN_MAX_FONT];
LOCAL ID		fn_mtxid = 0;
LOCAL ID		fn_sys = 0;		/* what the system draws words with */

/*
 * The font library is not to be entered by two tasks at once: its
 * faces keep the letter last made in one place (face->glyph), and a
 * letter made for one task while another is putting its own down comes
 * out as the other's. Every call of this layer that reaches the library
 * or the store of letters holds this lock; the task holding it may take
 * it again (a call made from inside another), and a caller that sets a
 * size and draws on someone else's behalf holds it across both
 * (fn_hold, fn_state_get and fn_state_set).
 */
LOCAL ID		fn_holder = 0;
LOCAL INT		fn_depth = 0;

LOCAL void enter( void )
{
	ID	me = tk_get_tid();

	if ( fn_holder == me ) {
		fn_depth++;
		return;
	}
	tk_loc_mtx(fn_mtxid, TMO_FEVR);
	fn_holder = me;
	fn_depth = 1;
}

LOCAL void leave( void )
{
	if ( --fn_depth == 0 ) {
		fn_holder = 0;
		tk_unl_mtx(fn_mtxid);
	}
}

EXPORT void fn_hold( void )
{
	enter();
}

/*
 * What a face is set to -- its size, its style and width, its angle --
 * kept and put back, by a caller that holds the layer (fn_hold) and sets
 * the face for a while on someone else's behalf.
 */
EXPORT ER fn_state_get( ID fid, T_FNSTATE *st )
{
	FONT	*f = ( fid >= 1 && fid <= FN_MAX_FONT && fn_tab[fid - 1].used ) ? &fn_tab[fid - 1] : NULL;

	if ( f == NULL || st == NULL ) {
		return E_PAR;
	}
	st->px = f->px;
	st->style = f->style;
	st->xpx = f->xpx;
	st->angle = f->angle;
	return E_OK;
}

LOCAL ER state_put( FONT *f, CONST T_FNSTATE *st )
{
	if ( st->px > 0 && ( st->px != f->px || st->xpx != f->xpx ) ) {
		if ( FT_Set_Pixel_Sizes(f->face, (FT_UInt)st->xpx, (FT_UInt)st->px) != 0 ) {
			return E_OBJ;
		}
	}
	f->px = st->px;
	f->style = st->style;
	f->xpx = st->xpx;
	f->angle = st->angle;
	return E_OK;
}

EXPORT ER fn_state_set( ID fid, CONST T_FNSTATE *st )
{
	FONT	*f = ( fid >= 1 && fid <= FN_MAX_FONT && fn_tab[fid - 1].used ) ? &fn_tab[fid - 1] : NULL;

	if ( f == NULL || st == NULL ) {
		return E_PAR;
	}
	return state_put(f, st);
}

EXPORT void fn_release( void )
{
	leave();
}

/* ---------------------------------------------------------------- start */

EXPORT INT knl_fn_init( void )
{
	T_CMTX	cmtx;
	INT	i;

	if ( fn_lib != NULL ) {
		return 1;
	}
	if ( FT_Init_FreeType(&fn_lib) != 0 ) {
		fn_lib = NULL;
		return 0;
	}
	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		fn_tab[i].used = FALSE;
		fn_tab[i].fd   = -1;
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	cmtx.ceilpri = 0;
	fn_mtxid = tk_cre_mtx(&cmtx);
	if ( fn_mtxid <= 0 ) {
		FT_Done_FreeType(fn_lib);
		fn_lib = NULL;
		return 0;
	}

	return 1;
}

/* ---------------------------------------------------------------- the store */

/*
 * Letters made once and kept.
 *
 * A page of text asks for the same letters over and over: Japanese
 * repeats its kana in every line, and every letter is asked for at
 * least twice anyway -- once to work out where the line breaks and once
 * to put it down. Making a letter is by far the most expensive thing
 * this layer does, so each one is made once and kept with its size and
 * its advance.
 *
 * How many letters are kept is fixed; how large each one is, is not. A
 * letter's pixels are taken from the heap when it is made and given
 * back when it is dropped, and the store drops the letter asked for
 * longest ago whenever what it holds passes what it may hold.
 *
 * Keeping the pixels in a slot of a fixed size instead -- which is the
 * obvious thing, and what this did at first -- means every letter
 * larger than that slot is made afresh every single time it is drawn.
 * Headings and titles are exactly the letters that do not fit, so the
 * store would work for the letters that cost least to make and fail for
 * the ones that cost most.
 *
 * How many letters are kept has to be counted against what a page of
 * Japanese actually contains, not chosen for looking reasonable. One
 * page of this system's own sample documents holds some six hundred
 * different characters, and it asks for them at three or four sizes --
 * a heading, the text, a caption -- so upwards of two thousand
 * different letters are in play while a single page is drawn. A store
 * of five hundred holds none of them for long: every letter is thrown
 * out before it is asked for the second time, and the whole page is
 * made again from the outlines on every redraw.
 *
 * That is what made drawing a page take a minute. The store is sized
 * for the page now, and the letters themselves are on the heap, so
 * what this costs when it is not full is the table alone.
 */
#define GC_MAX		4096		/* letters kept */
#define GC_PROBE	16		/* places one letter may sit in */
#define GC_CAP		( 4 * 1024 * 1024 )	/* pixels held at once */

/*
 * Where a letter starts is kept to a quarter of a pixel. The pen moves
 * by the face's own advances, in 64ths of a pixel, and each letter is
 * made for the quarter its start falls on: a letter starting a quarter
 * of a pixel along is made from its outline moved a quarter of a pixel,
 * not from the same pixels put down a whole pixel early or late. Text
 * then keeps the spacing the face was drawn with, instead of every
 * letter's gap being rounded to a whole pixel on its own.
 */
#define FN_PHASES	4		/* places in a pixel a letter may start at */
#define FN_PHASE_SH	4		/* 64ths of a pixel in a quarter, as a shift */

/* A letter's pixels, made for one of the places in a pixel it may start at */
typedef struct {
	INT	left, top;		/* where the pixels sit against the pen */
	INT	w, h, pitch;
	BOOL	gray;			/* a byte a pixel, how much it covers; else a bit */
	INT	nbytes;			/* this and the pixels, as taken from the heap */
	UB	bm[];			/* the pixels */
} GPIX;

typedef struct {
	BOOL	used;
	ID	fid;
	INT	px;
	UINT	look;			/* the style and the width it was made in */
	UW	cp;
	INT	adv;			/* how far the pen moves, in 64ths of a pixel */
	INT	up, down;		/* how far its box reaches above and below
					   the baseline */
	BOOL	has_adv;		/* the advance is settled */
	UINT	nopix;			/* a bit for each place whose pixels could
					   not be kept */
	UD	when;			/* the last time it was asked for */
	GPIX	*pix[FN_PHASES];	/* the pixels for each place, or NULL */
} GLYPH;

LOCAL GLYPH	fn_cache[GC_MAX];
LOCAL UD	fn_clock = 0;
LOCAL SZ	fn_held = 0;		/* bytes of pixels kept just now */

/*
 * What the work came to.
 *
 * Kept because the cost of putting text on a screen is never where it
 * looks as though it should be, and a count settles in one run what
 * reading the code argues about for an afternoon.
 */
typedef struct {
	UD	ask;			/* letters asked for */
	UD	hit;			/* found in the store */
	UD	made;			/* made by the font library */
	UD	put;			/* laid on the screen */
	UD	ms_make;		/* in the font library */
	UD	ms_put;			/* laying the pixels down */
} FNCOUNT;

LOCAL FNCOUNT	fn_count;

LOCAL UD fn_now( void )
{
	SYSTIM	t;

	tk_get_tim(&t);

	return (UD)t.lo;
}

EXPORT void fn_report( void )
{
	INT	i, k, nlet = 0, npix = 0;

	for ( i = 0; i < GC_MAX; i++ ) {
		if ( !fn_cache[i].used ) {
			continue;
		}
		nlet++;
		for ( k = 0; k < FN_PHASES; k++ ) {
			if ( fn_cache[i].pix[k] != NULL ) {
				npix++;
			}
		}
	}
	tm_printf((UB *)"  fn: ask %d hit %d made %d put %d\n",
		  (INT)fn_count.ask, (INT)fn_count.hit,
		  (INT)fn_count.made, (INT)fn_count.put);
	tm_printf((UB *)"  fn: make %d ms, put %d ms\n",
		  (INT)fn_count.ms_make, (INT)fn_count.ms_put);
	tm_printf((UB *)"  fn: kept %d letters, %d pixel sets, %d KB of %d KB\n",
		  nlet, npix, (INT)( fn_held / 1024 ), (INT)( GC_CAP / 1024 ));
}

/* Whether a letter holds pixels for any place */
LOCAL BOOL has_pix( CONST GLYPH *g )
{
	INT	k;

	for ( k = 0; k < FN_PHASES; k++ ) {
		if ( g->pix[k] != NULL ) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Let a letter go, pixels and all */
LOCAL void glyph_drop( GLYPH *g )
{
	INT	k;

	for ( k = 0; k < FN_PHASES; k++ ) {
		if ( g->pix[k] != NULL ) {
			fn_held -= (SZ)g->pix[k]->nbytes;
			Kfree(g->pix[k]);
			g->pix[k] = NULL;
		}
	}
	g->used = FALSE;
	g->nopix = 0;
}

/*
 * Room for one letter's pixels. The letters asked for longest ago are
 * dropped until what is held is within what may be held; the letter
 * being made is not among them, because it has not been asked for yet.
 */
LOCAL void *glyph_room( GLYPH *keep, INT n )
{
	void	*mem;

	while ( fn_held + (SZ)n > GC_CAP ) {
		GLYPH	*old = NULL;
		INT	i;

		for ( i = 0; i < GC_MAX; i++ ) {
			GLYPH	*g = &fn_cache[i];

			if ( !g->used || g == keep || !has_pix(g) ) {
				continue;
			}
			if ( old == NULL || g->when < old->when ) {
				old = g;
			}
		}
		if ( old == NULL ) {
			break;			/* nothing left to give back */
		}
		glyph_drop(old);
	}
	mem = Kmalloc((SZ)n);
	if ( mem != NULL ) {
		fn_held += (SZ)n;
	}

	return mem;
}

/* The look a letter is made in, as one number: the style, the width, the turn */
LOCAL UINT look_of( CONST FONT *f )
{
	return ( f->style & 0xFF ) | ( (UINT)( f->xpx & 0xFFF ) << 8 )
	       | ( (UINT)( f->angle & 0x1FF ) << 20 );
}

LOCAL GLYPH *cache_find( ID fid, INT px, UINT look, UW cp )
{
	UINT	h = ( (UINT)cp * 131U + (UINT)px * 17U + (UINT)fid + look * 7U )
		    % GC_MAX;
	UINT	i;

	for ( i = 0; i < GC_PROBE; i++ ) {
		GLYPH	*g = &fn_cache[(h + i) % GC_MAX];

		if ( g->used && g->fid == fid && g->px == px && g->cp == cp
		  && g->look == look ) {
			g->when = ++fn_clock;
			return g;
		}
	}

	return NULL;
}

/*
 * Where a new letter goes: a free place, or the one asked for longest
 * ago. Whichever it is, it comes back empty -- its pixels given back
 * and every measurement of the letter that was there cleared.
 *
 * Clearing it is not tidiness. The advance is settled once and then
 * trusted for the life of the letter, so a slot handed over with the
 * advance of the letter before it still set would give the new letter
 * that width, and the width of a letter is where the line breaks.
 */
LOCAL GLYPH *cache_room( ID fid, INT px, UINT look, UW cp )
{
	UINT	h = ( (UINT)cp * 131U + (UINT)px * 17U + (UINT)fid + look * 7U )
		    % GC_MAX;
	UINT	i;
	GLYPH	*old = &fn_cache[h];
	GLYPH	*take = NULL;

	for ( i = 0; i < GC_PROBE; i++ ) {
		GLYPH	*g = &fn_cache[(h + i) % GC_MAX];

		if ( !g->used ) {
			take = g;
			break;
		}
		if ( g->when < old->when ) {
			old = g;
		}
	}
	if ( take == NULL ) {
		take = old;
	}
	glyph_drop(take);
	take->fid     = fid;
	take->px      = px;
	take->look    = look;
	take->cp      = cp;
	take->adv     = 0;
	take->up      = 0;
	take->down    = 0;
	take->has_adv = FALSE;
	take->nopix   = 0;
	take->when    = ++fn_clock;

	return take;
}

/* How much wider a thickened letter is, in pixels */
LOCAL INT bold_extra( CONST FONT *f )
{
	return ( ( f->style & FN_ST_BOLD ) != 0 ) ? ( f->px + 15 ) / 16 : 0;
}

/*
 * A letter turned. Its box is its advance across and the face's size
 * down, its middle halfway along the advance and halfway between the
 * face's ascent and descent. The box is turned clockwise about that
 * middle, and the turned box, from its left edge, is what the letter
 * takes on the line: its width is how far the pen moves, and its middle
 * stays as high above the baseline as it was.
 *
 * 'adv' is the letter's advance upright, in 64ths of a pixel. Answers
 * the turned box's width in 64ths; *p_cy is the middle's height above
 * the baseline and *p_h the turned box's height, both in 64ths.
 */
LOCAL FT_Pos turn_box( CONST FONT *f, FT_Pos adv, FT_Pos *p_cy, FT_Pos *p_h )
{
	CONST FT_Size_Metrics	*m = &f->face->size->metrics;
	FT_Angle		a = (FT_Angle)f->angle << 16;
	FT_Fixed		c = FT_Cos(a), s = FT_Sin(a);
	FT_Pos			em = (FT_Pos)f->px * 64;

	if ( c < 0 ) c = -c;
	if ( s < 0 ) s = -s;
	*p_cy = ( m->ascender + m->descender ) / 2;
	*p_h = FT_MulFix(adv, s) + FT_MulFix(em, c);

	return FT_MulFix(adv, c) + FT_MulFix(em, s);
}

/*
 * How far the pen moves for a letter whose upright advance is 'adv'
 * (64ths), thickened and turned as the face is set, in 64ths of a pixel;
 * and how far its box reaches above and below the baseline.
 */
LOCAL INT advance_of( CONST FONT *f, FT_Pos adv, INT *p_up, INT *p_down )
{
	CONST FT_Size_Metrics	*m = &f->face->size->metrics;
	FT_Pos			cy, h, w;

	adv += (FT_Pos)bold_extra(f) * 64;
	if ( f->angle == 0 ) {
		*p_up = (INT)( m->ascender >> 6 );
		*p_down = (INT)( -( m->descender >> 6 ) );
		return (INT)adv;
	}
	w = turn_box(f, adv, &cy, &h);
	*p_up = (INT)( ( cy + h / 2 + 63 ) >> 6 );
	*p_down = (INT)( ( h / 2 - cy + 63 ) >> 6 );

	return (INT)w;
}

/*
 * A letter's advance as the face is set, in 64ths of a pixel, and its
 * box above and below the baseline; FALSE when the face cannot say.
 *
 * The advance comes from the face's own table of advances, read without
 * touching the outline, and it is the one number used for the letter
 * whether it is being measured or drawn. Where a line breaks is worked
 * out before the letters are made; if the measuring and the drawing
 * took the advance from two places that could differ, the two would
 * drift apart along the line and the line would run past the edge it
 * was broken to fit inside.
 */
LOCAL BOOL advance_read( FONT *f, FT_UInt gi, INT *p_adv, INT *p_up, INT *p_down )
{
	FT_Fixed	fx = 0;
	UD		t = fn_now();
	FT_Error	err;

	/*
	 * Asked for this way the library answers from the table or says
	 * it cannot; loading the outline to find a width costs as much as
	 * drawing the letter, and breaking a page into lines asks for the
	 * width of every letter on it.
	 */
	err = FT_Get_Advance(f->face, gi, FT_LOAD_NO_HINTING | FT_ADVANCE_FLAG_FAST_ONLY, &fx);
	if ( err != 0 ) {
		err = FT_Get_Advance(f->face, gi, FT_LOAD_NO_HINTING, &fx);
	}
	fn_count.ms_make += fn_now() - t;
	if ( err != 0 ) {
		return FALSE;
	}
	*p_adv = advance_of(f, (FT_Pos)( ( fx + 0x200 ) >> 10 ), p_up, p_down);

	return TRUE;
}

/*
 * How much a letter's strokes are thickened, in 64ths of a pixel across
 * and down (the whole of the widening; each side moves out by half).
 *
 * Letters mixed into what is under them as light (dp_put_gray) keep
 * every edge as light as the share of the pixel they cover, and at the
 * sizes of a menu or of running text, where a stroke is one or two
 * pixels and most of its pixels are edge, the letters come out thin and
 * grey. Moving each edge of the outline out a little before the pixels
 * are made puts back the weight the eye expects: 0.0121 of the size in
 * pixels each side across, a quarter more down -- a fifth of a pixel
 * either side at 16 pixels -- growing with the size until it is 0.3 of
 * a pixel, and none above 72 pixels, where a stroke is many pixels wide
 * and its edges no longer matter.
 *
 * The thickening is the same for outlines of every kind, the PostScript
 * outlines of an OpenType face and the quadratic ones of a TrueType face
 * alike, and it leaves the advance alone: only how dark the letters are
 * changes, not where they stand.
 */
#define DARK_MAX_PX	72		/* no thickening above this size */
#define DARK_X_PERMIL	1549		/* 2 x 0.0121 x 64, in thousandths */
#define DARK_Y_PERMIL	1936		/* the same, a quarter more */
#define DARK_LIMIT	38		/* 2 x 0.3 pixel, in 64ths */

LOCAL void darken( CONST FONT *f, FT_Outline *o )
{
	INT	ppx = ( f->xpx > 0 ) ? f->xpx : f->px;	/* the size across */
	FT_Pos	sx, sy;

	if ( f->px > DARK_MAX_PX ) {
		return;
	}
	sx = (FT_Pos)( ( ppx * DARK_X_PERMIL + 500 ) / 1000 );
	sy = (FT_Pos)( ( f->px * DARK_Y_PERMIL + 500 ) / 1000 );
	if ( sx > DARK_LIMIT ) sx = DARK_LIMIT;
	if ( sy > DARK_LIMIT ) sy = DARK_LIMIT;
	(void)FT_Outline_EmboldenXY(o, sx, sy);
}

/*
 * A letter made into pixels in the glyph slot, in the look the face is
 * set to and moved 'phase' quarters of a pixel to the right.
 *
 * The outline is loaded as it is, without fitting it to the pixel grid,
 * and changed: sheared a fifth of its height for italic, pushed out on
 * every side for bold, thickened a little for its size (darken), turned
 * in its box (turn_box), moved to the quarter it starts on. Then it is
 * made into 256 levels of how much of each pixel it covers.
 *
 * A face that carries ready-made pixels for the size gives those
 * instead, unless a look is asked for that only an outline can take.
 * Those pixels are put down as they are, at whatever quarter.
 */
LOCAL FT_Error render_letter( FONT *f, FT_UInt gi, INT phase )
{
	FT_Face		face = f->face;
	FT_Error	err;
	FT_Int32	flags = FT_LOAD_NO_HINTING;

	if ( ( f->style & ( FN_ST_BOLD | FN_ST_ITALIC ) ) != 0 || f->angle != 0 ) {
		flags |= FT_LOAD_NO_BITMAP;
	}
	err = FT_Load_Glyph(face, gi, flags);
	if ( err != 0 ) {
		return err;
	}
	if ( face->glyph->format == FT_GLYPH_FORMAT_BITMAP ) {
		return 0;
	}
	if ( face->glyph->format != FT_GLYPH_FORMAT_OUTLINE ) {
		return FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
	}
	if ( ( f->style & FN_ST_ITALIC ) != 0 ) {
		FT_Matrix	m;

		m.xx = 0x10000;
		m.xy = 0x3333;			/* a fifth of the height across */
		m.yx = 0;
		m.yy = 0x10000;
		FT_Outline_Transform(&face->glyph->outline, &m);
	}
	if ( ( f->style & FN_ST_BOLD ) != 0 ) {
		FT_Pos	strength = (FT_Pos)bold_extra(f) * 64;

		FT_Outline_EmboldenXY(&face->glyph->outline, strength, strength / 2);
	}
	darken(f, &face->glyph->outline);
	if ( f->angle != 0 ) {
		FT_Angle	a = (FT_Angle)f->angle << 16;
		FT_Pos		adv = ( ( face->glyph->linearHoriAdvance + 0x200 ) >> 10 )
				      + (FT_Pos)bold_extra(f) * 64;
		FT_Pos		cy, h, w = turn_box(f, adv, &cy, &h);
		FT_Matrix	m;

		/* clockwise on the screen, the outline's y going up */
		m.xx = FT_Cos(a);
		m.xy = FT_Sin(a);
		m.yx = -m.xy;
		m.yy = m.xx;
		FT_Outline_Translate(&face->glyph->outline, -adv / 2, -cy);
		FT_Outline_Transform(&face->glyph->outline, &m);
		FT_Outline_Translate(&face->glyph->outline, w / 2, cy);
	}
	if ( phase != 0 ) {
		FT_Outline_Translate(&face->glyph->outline, (FT_Pos)phase << FN_PHASE_SH, 0);
	}

	return FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
}

/* ---------------------------------------------------------------- vertical forms */

/* A number of the table, big-endian; 0 past its end */
LOCAL UW gs16( CONST FONT *f, UW at )
{
	if ( f->gsub == NULL || at + 2 > f->gsub_len ) {
		return 0;
	}

	return ( (UW)f->gsub[at] << 8 ) | f->gsub[at + 1];
}

LOCAL UW gs32( CONST FONT *f, UW at )
{
	return ( gs16(f, at) << 16 ) | gs16(f, at + 2);
}

/*
 * The table of substitutions read, and the single substitutions its
 * 'vert' feature names found in it: through the list of features to the
 * lookups they name, and through a lookup that only points elsewhere
 * (type 7) to the subtable it points at. A lookup of another type is not
 * a substitution of one letter for one and is passed over.
 */
LOCAL void vert_read( FONT *f )
{
	FT_ULong	len = 0;
	UW		feat, look, nf, nl, i, k;
	UW		idx[VSUB_MAX];
	INT		nidx = 0;

	f->gsub_read = TRUE;
	f->nvsub = 0;
	if ( FT_Load_Sfnt_Table(f->face, TTAG_GSUB, 0, NULL, &len) != 0
	  || len < 10 || len > 0x00FFFFFFUL ) {
		return;
	}
	f->gsub = (UB *)Kmalloc((SZ)len);
	if ( f->gsub == NULL ) {
		return;
	}
	if ( FT_Load_Sfnt_Table(f->face, TTAG_GSUB, 0, f->gsub, &len) != 0 ) {
		Kfree(f->gsub);
		f->gsub = NULL;
		return;
	}
	f->gsub_len = (UW)len;
	feat = gs16(f, 6);
	look = gs16(f, 8);
	nf = gs16(f, feat);
	for ( i = 0; i < nf; i++ ) {
		UW	rec = feat + 2 + 6 * i, ft, nlk;

		if ( !( f->gsub[rec] == 'v' && f->gsub[rec + 1] == 'e'
		     && f->gsub[rec + 2] == 'r' && f->gsub[rec + 3] == 't' )
		  || rec + 6 > f->gsub_len ) {
			continue;
		}
		ft = feat + gs16(f, rec + 4);
		nlk = gs16(f, ft + 2);
		for ( k = 0; k < nlk; k++ ) {
			UW	li = gs16(f, ft + 4 + 2 * k);
			INT	j;

			for ( j = 0; j < nidx && idx[j] != li; j++ ) {
				;
			}
			if ( j == nidx && nidx < VSUB_MAX ) {
				idx[nidx++] = li;
			}
		}
	}
	nl = gs16(f, look);
	for ( i = 0; i < (UW)nidx; i++ ) {
		UW	lk, type, nsub;

		if ( idx[i] >= nl ) {
			continue;
		}
		lk = look + gs16(f, look + 2 + 2 * idx[i]);
		type = gs16(f, lk);
		nsub = gs16(f, lk + 4);
		for ( k = 0; k < nsub && f->nvsub < VSUB_MAX; k++ ) {
			UW	st = lk + gs16(f, lk + 6 + 2 * k);

			if ( type == 7 ) {
				if ( gs16(f, st + 2) != 1 ) {
					continue;
				}
				st = st + gs32(f, st + 4);
			} else if ( type != 1 ) {
				continue;
			}
			if ( st < f->gsub_len ) {
				f->vsub[f->nvsub++] = st;
			}
		}
	}
}

/* Where a letter stands in a coverage table, or -1 when it is not in it */
LOCAL INT cover_of( CONST FONT *f, UW cov, UW gi )
{
	UW	fmt = gs16(f, cov), n = gs16(f, cov + 2);
	INT	lo = 0, hi = (INT)n - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;

		if ( fmt == 1 ) {
			UW	g = gs16(f, cov + 4 + 2 * (UW)mid);

			if ( g == gi ) return mid;
			if ( g < gi ) lo = mid + 1; else hi = mid - 1;
		} else if ( fmt == 2 ) {
			UW	r = cov + 4 + 6 * (UW)mid;
			UW	a = gs16(f, r), b = gs16(f, r + 2);

			if ( gi >= a && gi <= b ) return (INT)( gs16(f, r + 4) + gi - a );
			if ( b < gi ) lo = mid + 1; else hi = mid - 1;
		} else {
			break;
		}
	}

	return -1;
}

/* A letter in its vertical form, when the face has one; else as it is */
LOCAL FT_UInt vert_of( FONT *f, FT_UInt gi )
{
	INT	i;

	if ( !f->gsub_read ) {
		vert_read(f);
	}
	for ( i = 0; i < f->nvsub; i++ ) {
		UW	st = f->vsub[i], fmt = gs16(f, st);
		INT	c = cover_of(f, st + gs16(f, st + 2), (UW)gi);

		if ( c < 0 ) {
			continue;
		}
		if ( fmt == 1 ) {
			return (FT_UInt)( ( gi + gs16(f, st + 4) ) & 0xFFFFU );
		}
		if ( fmt == 2 && (UW)c < gs16(f, st + 4) ) {
			return (FT_UInt)gs16(f, st + 6 + 2 * (UW)c);
		}
	}

	return gi;
}

/* The letter's glyph in the face, in the form the face is set to */
LOCAL FT_UInt glyph_index( FONT *f, UW cp )
{
	FT_UInt	gi = FT_Get_Char_Index(f->face, (FT_ULong)cp);

	if ( ( f->style & FN_ST_VERT ) != 0 && gi != 0 ) {
		gi = vert_of(f, gi);
	}

	return gi;
}

/*
 * The letter, with its pixels for the place in a pixel it starts at
 * ('phase' quarters) when they are wanted; made if it is not kept. NULL
 * when the face cannot say how wide it is.
 *
 * When the pixels were made but could not be kept, the letter comes back
 * without them and *p_loose says so: they are in the face's glyph slot,
 * to be put down from there before anything else is made.
 */
LOCAL CONST GLYPH *glyph_of( FONT *f, ID fid, UW cp, BOOL want_pixels, INT phase,
			     BOOL *p_loose )
{
	GLYPH		*g;
	FT_Face		face = f->face;
	FT_UInt		gi;
	CONST FT_Bitmap	*bm;
	GPIX		*px;
	INT		row, n;
	UINT		bit = 1U << phase;

	if ( p_loose != NULL ) {
		*p_loose = FALSE;
	}
	fn_count.ask++;
	g = cache_find(fid, f->px, look_of(f), cp);
	if ( g != NULL && g->has_adv
	  && ( !want_pixels || g->pix[phase] != NULL || ( g->nopix & bit ) != 0 ) ) {
		fn_count.hit++;
		return g;
	}
	gi = glyph_index(f, cp);
	if ( g == NULL || !g->has_adv ) {
		INT	adv, up, down;

		if ( !advance_read(f, gi, &adv, &up, &down) ) {
			return NULL;
		}
		if ( g == NULL ) {
			g = cache_room(fid, f->px, look_of(f), cp);
			g->used = TRUE;
		}
		g->adv = adv;
		g->up = up;
		g->down = down;
		g->has_adv = TRUE;
	}
	g->when = ++fn_clock;
	if ( !want_pixels ) {
		return g;
	}
	/*
	 * No hinting. Fitting an outline to the pixel grid is by far the
	 * most expensive thing the font library does -- on a face with
	 * PostScript outlines it is most of the work of making a letter --
	 * and it moves stems by up to half a pixel, which puts back the
	 * very rounding that placing letters by quarters of a pixel takes
	 * away. Unhinted, a letter is the same shape at every quarter.
	 */
	fn_count.made++;
	{
		UD	 t = fn_now();
		FT_Error err = render_letter(f, gi, phase);

		fn_count.ms_make += fn_now() - t;
		if ( err != 0 ) {
			g->nopix |= bit;	/* the advance stands; nothing to draw */
			return g;
		}
	}
	bm = &face->glyph->bitmap;
	n = (INT)bm->pitch * (INT)bm->rows;
	if ( bm->buffer == NULL || n <= 0 || bm->pitch < 0
	  || ( bm->pixel_mode != FT_PIXEL_MODE_MONO && bm->pixel_mode != FT_PIXEL_MODE_GRAY ) ) {
		/* nothing to draw (a space), or not the form this store
		   holds: the slot has it, for the long way */
		g->nopix |= bit;
		if ( p_loose != NULL && bm->buffer != NULL && n != 0 ) {
			*p_loose = TRUE;
			g->nopix &= ~bit;
		}
		return g;
	}
	px = (GPIX *)glyph_room(g, (INT)sizeof(GPIX) + n);
	if ( px == NULL ) {
		/* no room to keep it: put down from the slot this once */
		if ( p_loose != NULL ) {
			*p_loose = TRUE;
		}
		return g;
	}
	px->left   = face->glyph->bitmap_left;
	px->top    = face->glyph->bitmap_top;
	px->w      = (INT)bm->width;
	px->h      = (INT)bm->rows;
	px->pitch  = (INT)bm->pitch;
	px->gray   = (BOOL)( bm->pixel_mode == FT_PIXEL_MODE_GRAY );
	px->nbytes = (INT)sizeof(GPIX) + n;
	for ( row = 0; row < n; row++ ) {
		px->bm[row] = bm->buffer[row];
	}
	g->pix[phase] = px;

	return g;
}

/* The number a face is known by, which is where it sits in the table */
LOCAL ID fn_id_of( CONST FONT *f )
{
	return (ID)( f - fn_tab ) + 1;
}

LOCAL FONT *fn_of( ID fid )
{
	if ( fid < 1 || fid > FN_MAX_FONT ) {
		return NULL;
	}

	return ( fn_tab[fid - 1].used ) ? &fn_tab[fid - 1] : NULL;
}

EXPORT ER fn_set_system( ID fid )
{
	if ( fid != 0 && fn_of(fid) == NULL ) {
		return E_ID;
	}
	fn_sys = fid;

	return E_OK;
}

EXPORT ID fn_system( void )
{
	return fn_sys;
}


/* ---------------------------------------------------------------- faces */

EXPORT ER fn_open_mem( CONST UB *data, SZ size, INT index, ID *p_fid )
{
	FONT	*f;
	FT_Face	face = NULL;
	INT	i;

	if ( data == NULL || size <= 0 || p_fid == NULL ) {
		return E_PAR;
	}
	if ( fn_lib == NULL && knl_fn_init() == 0 ) {
		return E_NOEXS;
	}
	enter();
	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( !fn_tab[i].used ) {
			break;
		}
	}
	if ( i == FN_MAX_FONT ) {
		leave();
		return E_LIMIT;
	}
	/*
	 * The bytes are not copied: FreeType reads from them as it needs
	 * them, which is what makes opening a font of several megabytes
	 * cost nothing. The caller keeps them until fn_close.
	 */
	if ( FT_New_Memory_Face(fn_lib, (CONST FT_Byte *)data, (FT_Long)size,
				(FT_Long)index, &face) != 0 ) {
		leave();
		return E_OBJ;			/* not a font this build understands */
	}
	f = &fn_tab[i];
	f->face  = face;
	f->px    = 0;
	f->style = 0;
	f->xpx   = 0;
	f->angle = 0;
	f->fd    = -1;			/* the bytes are the caller's, not a file */
	f->bytes = NULL;
	f->pages = 0;
	f->gsub_read = FALSE;
	f->gsub = NULL;
	f->gsub_len = 0;
	f->nvsub = 0;
	f->from_obj = FALSE;
	f->index = (INT)index;
	knl_memset(&f->reg, 0, sizeof(f->reg));
	f->used  = TRUE;
	leave();

	*p_fid = (ID)(i + 1);

	return E_OK;
}

/* ------------------------------------------------- a face out of a file */

/*
 * FreeType asks for bytes at an offset; the file answers. It never asks
 * backwards for long, so a seek and a read each time is enough and
 * nothing here caches: the file system below does that already.
 */
LOCAL unsigned long fn_stream_read( FT_Stream stream, unsigned long offset,
				    unsigned char *buffer, unsigned long count )
{
	INT	fd = (INT)(BINT)stream->descriptor.pointer;
	INT	got = 0, n;

	if ( count == 0 ) {
		/* a seek with nothing to read: FreeType uses this to move */
		return ( fs_lseek(fd, (D)offset, SEEK_SET_) >= 0 ) ? 0 : 1;
	}
	if ( fs_lseek(fd, (D)offset, SEEK_SET_) < 0 ) {
		return 0;
	}
	while ( got < (INT)count ) {
		n = fs_read(fd, buffer + got, (INT)count - got);
		if ( n <= 0 ) {
			break;
		}
		got += n;
	}

	return (unsigned long)got;
}

LOCAL void fn_stream_close( FT_Stream stream )
{
	(void)stream;			/* the file is closed by fn_close */
}

/*
 * The whole of a font file, in pages of its own.
 *
 * A face left reading from the file reads a few bytes at a time,
 * wherever the tables and the outlines happen to lie, and every one of
 * those reads is a seek through the file system. Drawing a page of text
 * is thousands of them, and the drawing then costs more in the file
 * system than in the rasteriser -- which is why a font is read in once
 * and kept.
 *
 * The pages are given back when the face is closed. A font too large to
 * be held is not a fault: the caller falls back to reading it as a
 * stream, which is slow but works.
 */
LOCAL UB *slurp_font( CONST char *path, SZ size, UD *p_pages )
{
	UD	pages = (UD)( ( size + 4095 ) / 4096 );
	UB	*mem;
	INT	fd;
	SZ	at = 0;

	mem = (UB *)knl_vmap(pages, 0);
	if ( mem == NULL ) {
		return NULL;
	}
	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		knl_vunmap(mem, pages);
		return NULL;
	}
	while ( at < size ) {
		SZ	chunk = size - at;
		INT	got;

		if ( chunk > 65536 ) {
			chunk = 65536;
		}
		got = fs_read(fd, mem + at, (INT)chunk);
		if ( got <= 0 ) {
			break;
		}
		at += (SZ)got;
	}
	fs_close(fd);
	if ( at < size ) {
		knl_vunmap(mem, pages);
		return NULL;
	}
	*p_pages = pages;

	return mem;
}

/*
 * A record read whole into pages of its own, the way slurp_font reads a
 * file; NULL when it cannot be.
 */
LOCAL UB *slurp_record( CONST TS_UUID *uuid, INT recno, SZ *p_size, UD *p_pages )
{
	T_OBREC	rec[16];
	UB	*mem = NULL;
	UD	pages = 0;
	SZ	size = 0, at = 0, got;
	INT	cnt = 0, i;
	ID	key = ob_opn_obj(uuid, OB_OP_READ | OB_OP_ATRRD);

	if ( key <= 0 ) {
		return NULL;
	}
	if ( ob_lst_rec(key, rec, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( rec[i].recno == recno ) size = (SZ)rec[i].size;
		}
	}
	if ( size > 0 ) {
		pages = (UD)( ( size + 4095 ) / 4096 );
		mem = (UB *)knl_vmap(pages, 0);
	}
	while ( mem != NULL && at < size ) {
		SZ	chunk = ( size - at > 65536 ) ? 65536 : size - at;

		got = 0;
		if ( ob_rea_rec(key, recno, (D)at, mem + at, chunk, &got) < E_OK || got <= 0 ) {
			break;
		}
		at += got;
	}
	(void)ob_cls_obj(key);
	if ( mem != NULL && at < size ) {
		knl_vunmap(mem, pages);
		mem = NULL;
	}
	*p_size = size;
	*p_pages = pages;

	return mem;
}

EXPORT ER fn_open_obj( CONST TS_UUID *uuid, INT recno, INT index, ID *p_fid )
{
	UB	*mem;
	UD	pages = 0;
	SZ	size = 0;
	ER	er;

	if ( uuid == NULL || p_fid == NULL ) {
		return E_PAR;
	}
	if ( fn_lib == NULL && knl_fn_init() == 0 ) {
		return E_NOEXS;
	}
	mem = slurp_record(uuid, recno, &size, &pages);
	if ( mem == NULL ) {
		return E_NOEXS;
	}
	er = fn_open_mem(mem, size, index, p_fid);
	if ( er >= E_OK ) {
		FONT	*g = fn_of(*p_fid);

		if ( g != NULL ) {
			g->bytes = mem;			/* ours to give back */
			g->pages = pages;
			g->from_obj = TRUE;
			g->obj = *uuid;
		}
		return E_OK;
	}
	knl_vunmap(mem, pages);

	return er;
}

EXPORT ER fn_open_file( CONST char *path, INT index, ID *p_fid )
{
	FONT		*f;
	FT_Open_Args	args;
	FT_Face		face = NULL;
	T_FSTAT		st;
	UB		*mem;
	UD		pages = 0;
	INT		i, fd;

	if ( path == NULL || p_fid == NULL ) {
		return E_PAR;
	}
	if ( fn_lib == NULL && knl_fn_init() == 0 ) {
		return E_NOEXS;
	}
	if ( fs_stat(path, &st) < EX_OK || st.size == 0 ) {
		return E_NOEXS;
	}
	mem = slurp_font(path, (SZ)st.size, &pages);
	if ( mem != NULL ) {
		ER	er;

		er = fn_open_mem(mem, (SZ)st.size, index, p_fid);
		if ( er >= E_OK ) {
			FONT	*g = fn_of(*p_fid);

			if ( g != NULL ) {
				g->bytes = mem;		/* ours to give back */
				g->pages = pages;
			}
			return E_OK;
		}
		knl_vunmap(mem, pages);
	}
	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	enter();
	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( !fn_tab[i].used ) {
			break;
		}
	}
	if ( i == FN_MAX_FONT ) {
		leave();
		fs_close(fd);
		return E_LIMIT;
	}
	f = &fn_tab[i];
	f->bytes = NULL;
	f->pages = 0;
	knl_memset(&f->stream, 0, sizeof(f->stream));
	f->stream.size               = (unsigned long)st.size;
	f->stream.pos                = 0;
	f->stream.descriptor.pointer = (void *)(BINT)fd;
	f->stream.read               = fn_stream_read;
	f->stream.close              = fn_stream_close;

	knl_memset(&args, 0, sizeof(args));
	args.flags  = FT_OPEN_STREAM;
	args.stream = &f->stream;

	if ( FT_Open_Face(fn_lib, &args, (FT_Long)index, &face) != 0 ) {
		leave();
		fs_close(fd);
		return E_OBJ;
	}
	f->face = face;
	f->px   = 0;
	f->style = 0;
	f->xpx  = 0;
	f->angle = 0;
	f->fd   = fd;
	f->gsub_read = FALSE;
	f->gsub = NULL;
	f->gsub_len = 0;
	f->nvsub = 0;
	f->from_obj = FALSE;
	f->index = index;
	knl_memset(&f->reg, 0, sizeof(f->reg));
	f->used = TRUE;
	leave();

	*p_fid = (ID)(i + 1);

	return E_OK;
}

EXPORT ER fn_close( ID fid )
{
	FONT	*f;

	enter();
	f = fn_of(fid);
	if ( f == NULL ) {
		leave();
		return E_ID;
	}
	{
		INT	i;
		ID	gone = fn_id_of(f);

		for ( i = 0; i < GC_MAX; i++ ) {
			if ( fn_cache[i].used && fn_cache[i].fid == gone ) {
				glyph_drop(&fn_cache[i]);
			}
		}
	}
	FT_Done_Face(f->face);
	f->face = NULL;
	if ( f->gsub != NULL ) {
		Kfree(f->gsub);
		f->gsub = NULL;
	}
	f->gsub_read = FALSE;
	f->nvsub = 0;
	if ( f->fd >= 0 ) {
		fs_close(f->fd);
		f->fd = -1;
	}
	/* the file this layer read in, if it was this layer that read it */
	if ( f->bytes != NULL ) {
		knl_vunmap(f->bytes, f->pages);
		f->bytes = NULL;
		f->pages = 0;
	}
	f->used = FALSE;
	leave();

	return E_OK;
}

LOCAL ER fn_set_size_nl( ID fid, INT px )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( px <= 0 || px > 512 ) {
		return E_PAR;
	}
	if ( FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)px) != 0 ) {
		return E_OBJ;
	}
	f->px = px;
	f->style = 0;
	f->xpx = 0;
	f->angle = 0;
	return E_OK;
}

EXPORT ER fn_set_size( ID fid, INT px )
{
	ER	r;

	enter();
	r = fn_set_size_nl(fid, px);
	leave();
	return r;
}

LOCAL ER fn_set_angle_nl( ID fid, INT deg )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( f->px == 0 ) {
		return E_PAR;
	}
	f->angle = ( deg % 360 + 360 ) % 360;
	return E_OK;
}

EXPORT ER fn_set_angle( ID fid, INT deg )
{
	ER	r;

	enter();
	r = fn_set_angle_nl(fid, deg);
	leave();
	return r;
}

LOCAL ER fn_set_style_nl( ID fid, UINT style, INT xpx )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( f->px == 0 || xpx < 0 || xpx > 1024 ) {
		return E_PAR;
	}
	if ( xpx == f->px ) {
		xpx = 0;
	}
	if ( xpx != f->xpx
	  && FT_Set_Pixel_Sizes(f->face, (FT_UInt)xpx, (FT_UInt)f->px) != 0 ) {
		return E_OBJ;
	}
	f->style = style;
	f->xpx = xpx;
	return E_OK;
}

EXPORT ER fn_set_style( ID fid, UINT style, INT xpx )
{
	ER	r;

	enter();
	r = fn_set_style_nl(fid, style, xpx);
	leave();
	return r;
}

LOCAL ER fn_metrics_nl( ID fid, T_FNMET *met )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( met == NULL || f->px == 0 ) {
		return E_PAR;
	}
	/* the face keeps these in 26.6 fixed point once a size is set */
	met->ascent      = (INT)( f->face->size->metrics.ascender >> 6 );
	met->descent     = (INT)( -( f->face->size->metrics.descender >> 6 ) );
	met->height      = (INT)( f->face->size->metrics.height >> 6 );
	met->max_advance = (INT)( f->face->size->metrics.max_advance >> 6 );

	return E_OK;
}

EXPORT ER fn_metrics( ID fid, T_FNMET *met )
{
	ER	r;

	enter();
	r = fn_metrics_nl(fid, met);
	leave();
	return r;
}

/* ---------------------------------------------------------------- text */

/*
 * One character out of UTF-8, and how many bytes it took. A sequence
 * that is not well formed answers U+FFFD and steps one byte, so that a
 * bad byte costs one character rather than the rest of the string.
 */
LOCAL UW utf8_next( CONST UB *s, INT *p_len )
{
	UB	c = s[0];
	UW	v;
	INT	n, i;

	if ( c < 0x80 ) {
		*p_len = 1;
		return c;
	}
	if ( (c & 0xE0) == 0xC0 ) { n = 2; v = c & 0x1F; }
	else if ( (c & 0xF0) == 0xE0 ) { n = 3; v = c & 0x0F; }
	else if ( (c & 0xF8) == 0xF0 ) { n = 4; v = c & 0x07; }
	else { *p_len = 1; return 0xFFFD; }

	for ( i = 1; i < n; i++ ) {
		if ( (s[i] & 0xC0) != 0x80 ) {
			*p_len = 1;
			return 0xFFFD;
		}
		v = ( v << 6 ) | (UW)( s[i] & 0x3F );
	}
	*p_len = n;

	return v;
}

/* How long a string is */
LOCAL SZ tstrlen_u( CONST UB *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}

	return n;
}

/* Put one rendered glyph down, a row of bits at a time */
LOCAL void blit( INT gid, CONST FT_Bitmap *bm, INT x, INT y, UW colour )
{
	UINT	row, col;

	if ( bm->buffer == NULL ) {
		return;
	}
	if ( bm->pixel_mode == FT_PIXEL_MODE_MONO ) {
		/* the whole letter in one call: the layer takes its lock
		   once and walks the rows as runs */
		dp_put_mono(gid, x, y, bm->buffer, (INT)bm->pitch,
			    (INT)bm->width, (INT)bm->rows, colour);
		return;
	}
	if ( bm->pixel_mode == FT_PIXEL_MODE_GRAY && bm->pitch >= (INT)bm->width ) {
		dp_put_gray(gid, x, y, bm->buffer, (INT)bm->pitch,
			    (INT)bm->width, (INT)bm->rows, colour);
		return;
	}
	for ( row = 0; row < bm->rows; row++ ) {
		CONST UB *line = bm->buffer + (INT)row * bm->pitch;

		for ( col = 0; col < bm->width; col++ ) {
			UB	on;

			{
				/*
				 * Grey coverage with nothing to mix it into:
				 * half or more of the pixel is enough to put
				 * it down. This is what changes when the
				 * drawing layer can blend.
				 */
				on = ( line[col] >= 128 ) ? 1 : 0;
			}
			if ( on != 0 ) {
				dp_put_pixel(gid, x + (INT)col, y + (INT)row, colour);
			}
		}
	}
}

/*
 * A letter drawn into an environment that records (dp_open_rec): handed
 * on as the glyph of the face it is, placed by a matrix, rather than as
 * pixels. The matrix does to the glyph's outline -- in pixels at the
 * face's size, y going up -- what render_letter does before the outline
 * is made into pixels, and then puts it where the pixels would have
 * gone: across by the width asked for (xpx), leaned for italic, turned
 * in its box when the face is turned. 'x' is where the pixels' origin
 * is put on the line, in 65536ths of a pixel, and 'base' the baseline.
 */
LOCAL void rec_letter( INT gid, FONT *f, UW cp, FT_Fixed x, INT base, UW colour )
{
	T_DPRECGLY	r;
	FT_UInt		gi = glyph_index(f, cp);
	FT_Fixed	sx = ( f->xpx > 0 ) ? ( (FT_Fixed)f->xpx << 16 ) / f->px : 0x10000;
	FT_Fixed	lean = ( ( f->style & FN_ST_ITALIC ) != 0 ) ? 0x3333 : 0;

	knl_memset(&r, 0, sizeof(r));
	r.fid = fn_id_of(f);
	r.gi = (UINT)gi;
	r.cp = cp;
	r.px = f->px;
	r.colour = colour;
	r.bold = (W)( (FT_Fixed)bold_extra(f) << 16 );
	if ( f->angle == 0 ) {
		r.m[0] = (W)sx;
		r.m[1] = 0;
		r.m[2] = (W)lean;
		r.m[3] = -0x10000;
		r.m[4] = (W)x;
		r.m[5] = (W)( (FT_Fixed)base << 16 );
	} else {
		FT_Angle	a = (FT_Angle)f->angle << 16;
		FT_Fixed	c = FT_Cos(a), sn = FT_Sin(a), fx = 0;
		FT_Pos		adv, cy, h, w;

		(void)FT_Get_Advance(f->face, gi, FT_LOAD_NO_HINTING, &fx);
		adv = (FT_Pos)( fx >> 10 ) + (FT_Pos)bold_extra(f) * 64;
		w = turn_box(f, adv, &cy, &h);
		/* the outline across and leaned, then turned about the middle of its box */
		r.m[0] = (W)FT_MulFix(c, sx);
		r.m[1] = (W)FT_MulFix(sn, sx);
		r.m[2] = (W)( FT_MulFix(c, lean) + sn );
		r.m[3] = (W)( FT_MulFix(sn, lean) - c );
		r.m[4] = (W)( x + ( w << 10 ) / 2
			      - FT_MulFix(c, ( adv << 10 ) / 2) - FT_MulFix(sn, cy << 10) );
		r.m[5] = (W)( ( (FT_Fixed)base << 16 ) - ( cy << 10 )
			      + FT_MulFix(c, cy << 10) - FT_MulFix(sn, ( adv << 10 ) / 2) );
	}
	(void)dp_rec_glyph(gid, &r);
}

/* ---------------------------------------------------------------- the base face */

/*
 * A letter the face has none of is drawn from the face of its base
 * family (T_FNREG.base) when that face has it. For the length of one
 * walk the base face is set as the face is -- its size, look and turn
 * -- and what it was set to is put back when the walk is done
 * (base_end): the base is usually a face the system draws with too.
 */
typedef struct {
	FONT		*b;		/* the base face, once set as the face */
	T_FNSTATE	was;		/* what it was set to before */
} BASEW;

LOCAL FONT *face_named( CONST UB *name, CONST FONT *not );

LOCAL FONT *letter_face( FONT *f, UW cp, BASEW *w )
{
	FONT		*b;
	T_FNSTATE	st;

	if ( f->reg.base[0] == 0 || FT_Get_Char_Index(f->face, (FT_ULong)cp) != 0 ) {
		return f;
	}
	b = ( w->b != NULL ) ? w->b : face_named(f->reg.base, f);
	if ( b == NULL || FT_Get_Char_Index(b->face, (FT_ULong)cp) == 0 ) {
		return f;
	}
	if ( w->b == NULL ) {
		w->was.px = b->px;
		w->was.style = b->style;
		w->was.xpx = b->xpx;
		w->was.angle = b->angle;
		st.px = f->px;
		st.style = f->style;
		st.xpx = f->xpx;
		st.angle = f->angle;
		if ( state_put(b, &st) < E_OK ) {
			return f;
		}
		w->b = b;
	}
	b->angle = f->angle;
	return b;
}

LOCAL void base_end( BASEW *w )
{
	if ( w->b != NULL ) {
		(void)state_put(w->b, &w->was);
		w->b = NULL;
	}
}

/* ---------------------------------------------------------------- drawn at a scale */

/*
 * Text drawn into an environment whose target holds the screen's pixels
 * at a scale (dp_set_scale): the letters are made at the size times the
 * scale, in the target's own pixels, and each is put where the pen of
 * the text laid out at its own size stands, times the scale, to the
 * quarter of a target pixel. The text is laid out and measured as it
 * is unscaled -- the advances, the width answered, where it stops
 * fitting -- so lines break, carets stand and selections run where they
 * do at 100; only the letters themselves are finer. Unhinted, a face's
 * advances grow with its size, so the letters made larger stand where
 * their own advances would put them.
 */

/* The face's size in the font library set to px across xpx (0: as tall) */
LOCAL void ft_size( FONT *f, INT xpx, INT px )
{
	(void)FT_Set_Pixel_Sizes(f->face, ( xpx > 0 ) ? (FT_UInt)xpx : 0, (FT_UInt)px);
}

/*
 * One letter of f made at the scale 'sc' and put with its pen dx, dy
 * target pixels from where view place (x, y) lies, made for the quarter
 * 'phase'. The face is set to the larger size only when the store does
 * not already hold the letter at it, and set back at once: the size it
 * is set to is what everything else measures with.
 */
LOCAL void letter_scaled( INT gid, FONT *f, UW cp, INT x, INT y, INT dx, INT dy, INT phase,
			  INT sc, UW colour )
{
	INT		opx = f->px, oxpx = f->xpx;
	INT		px = ( opx * sc + 50 ) / 100;
	INT		xpx = ( oxpx > 0 ) ? ( oxpx * sc + 50 ) / 100 : 0;
	ID		fid = fn_id_of(f);
	BOOL		loose = FALSE;
	CONST GLYPH	*g;
	CONST GPIX	*pix;
	UD		t = fn_now();

	if ( px > 1024 ) px = 1024;
	if ( xpx > 1024 ) xpx = 1024;
	f->px = px;
	f->xpx = xpx;
	g = cache_find(fid, px, look_of(f), cp);
	if ( g == NULL || !g->has_adv
	  || ( g->pix[phase] == NULL && ( g->nopix & ( 1U << phase ) ) == 0 ) ) {
		ft_size(f, xpx, px);
		g = glyph_of(f, fid, cp, TRUE, phase, &loose);
		if ( loose ) {
			CONST FT_Bitmap	*bm = &f->face->glyph->bitmap;

			/* made, but not kept: from the slot, this once, before the size goes back */
			if ( bm->buffer != NULL && bm->pixel_mode == FT_PIXEL_MODE_GRAY ) {
				(void)dp_put_gray_dev(gid, x, y, dx + f->face->glyph->bitmap_left,
						      dy - f->face->glyph->bitmap_top, bm->buffer,
						      (INT)bm->pitch, (INT)bm->width, (INT)bm->rows, colour);
			} else if ( bm->buffer != NULL && bm->pixel_mode == FT_PIXEL_MODE_MONO ) {
				(void)dp_put_mono_dev(gid, x, y, dx + f->face->glyph->bitmap_left,
						      dy - f->face->glyph->bitmap_top, bm->buffer,
						      (INT)bm->pitch, (INT)bm->width, (INT)bm->rows, colour);
			}
			fn_count.put++;
		}
		ft_size(f, oxpx, opx);
	}
	f->px = opx;
	f->xpx = oxpx;
	if ( !loose && g != NULL ) {
		pix = g->pix[phase];
		if ( pix != NULL && pix->w > 0 && pix->h > 0 ) {
			if ( pix->gray ) {
				(void)dp_put_gray_dev(gid, x, y, dx + pix->left, dy - pix->top,
						      pix->bm, pix->pitch, pix->w, pix->h, colour);
			} else {
				(void)dp_put_mono_dev(gid, x, y, dx + pix->left, dy - pix->top,
						      pix->bm, pix->pitch, pix->w, pix->h, colour);
			}
			fn_count.put++;
		}
	}
	fn_count.ms_put += fn_now() - t;
}

/* A pen of 'pen' 64ths of a view pixel, at the scale: in quarters of a target pixel */
LOCAL INT quarter_of( INT pen, INT sc )
{
	D	p = (D)pen * sc;

	p = ( p >= 0 ) ? p / 100 : -( ( -p + 99 ) / 100 );
	return (INT)( ( p + ( 1 << ( FN_PHASE_SH - 1 ) ) ) >> FN_PHASE_SH );
}

/* The walk across a line (walk) when the letters are drawn at a scale */
LOCAL INT walk_scaled( INT gid, FONT *f0, INT x, INT y, CONST UB *s, UW colour,
		       INT limit, INT *p_fit, INT sc )
{
	FONT	*f;
	BASEW	bw;
	INT	pen = 0, i = 0;

	bw.b = NULL;
	while ( s[i] != '\0' ) {
		UW		cp;
		INT		n = 1, q;
		CONST GLYPH	*g;

		cp = utf8_next(s + i, &n);
		f = letter_face(f0, cp, &bw);
		g = glyph_of(f, fn_id_of(f), cp, FALSE, 0, NULL);
		if ( g == NULL ) {
			i += n;
			continue;
		}
		if ( limit >= 0 && ( ( pen + g->adv + 32 ) >> 6 ) > limit ) {
			break;
		}
		q = quarter_of(pen, sc);
		letter_scaled(gid, f, cp, x, y, q >> 2, 0, q & ( FN_PHASES - 1 ), sc, colour);
		pen += g->adv;
		i += n;
	}
	base_end(&bw);
	if ( p_fit != NULL ) {
		*p_fit = i;
	}

	return ( pen + 32 ) >> 6;
}

/*
 * The one walk. `gid` below zero means do not draw; `limit` at or above
 * zero means stop when the pen would pass it and say how many bytes fit.
 *
 * The pen is kept in 64ths of a pixel and moves by each letter's own
 * advance, unrounded; each letter is put down at the quarter of a pixel
 * nearest where the pen is. What is said back -- the width, and where
 * the text stops fitting -- is the pen rounded to the nearest pixel, so
 * that a string measures what it draws to within half a pixel, and the
 * width of the first k letters of a string is where its k-th letter
 * begins: a caret put after them stands against the letter.
 */
LOCAL INT walk( INT gid, FONT *f0, INT x, INT y, CONST UB *s, UW colour,
		INT limit, INT *p_fit )
{
	FONT	*f = f0;
	BASEW	bw;
	INT	pen = 0, i = 0;
	/*
	 * A walk that is only measuring does not need the pixels. Making
	 * them is the expensive part by a wide margin, and a page of text
	 * is measured at least as often as it is drawn -- once to break
	 * its lines and once to know how tall it came to.
	 */
	BOOL	draw = (BOOL)( gid >= 0 );
	/* a page being recorded takes the glyphs themselves, not their pixels */
	BOOL	rec = (BOOL)( draw && dp_is_rec(gid) );
	INT	sc = ( draw && !rec ) ? dp_scale(gid) : 100;

	if ( sc > 100 ) {
		return walk_scaled(gid, f0, x, y, s, colour, limit, p_fit, sc);
	}
	bw.b = NULL;
	while ( s[i] != '\0' ) {
		UW		cp;
		INT		n = 1, q, phase;
		BOOL		loose = FALSE;
		CONST GLYPH	*g;

		cp = utf8_next(s + i, &n);
		f = letter_face(f0, cp, &bw);
		q = ( pen + ( 1 << ( FN_PHASE_SH - 1 ) ) ) >> FN_PHASE_SH;
		phase = ( draw && !rec ) ? ( q & ( FN_PHASES - 1 ) ) : 0;
		g = glyph_of(f, fn_id_of(f), cp, (BOOL)( draw && !rec ), phase, &loose);
		if ( rec ) {
			INT	adv = ( g != NULL ) ? g->adv : f->px * 64;

			if ( limit >= 0 && ( ( pen + adv + 32 ) >> 6 ) > limit ) {
				break;
			}
			rec_letter(gid, f, cp, ( (FT_Fixed)x << 16 ) + ( (FT_Fixed)pen << 10 ),
				   y, colour);
			pen += adv;
			i += n;
			continue;
		}
		if ( g == NULL ) {
			i += n;			/* the face cannot say: nothing, no room */
			continue;
		}
		if ( limit >= 0 && ( ( pen + g->adv + 32 ) >> 6 ) > limit ) {
			break;
		}
		if ( draw ) {
			INT		at = x + ( q >> ( 6 - FN_PHASE_SH ) );
			CONST GPIX	*px = g->pix[phase];
			UD		t = fn_now();

			if ( loose ) {
				/* made, but not kept: from the slot, this once */
				blit(gid, &f->face->glyph->bitmap, at + f->face->glyph->bitmap_left,
				     y - f->face->glyph->bitmap_top, colour);
				fn_count.put++;
			} else if ( px != NULL && px->w > 0 && px->h > 0 ) {
				if ( px->gray ) {
					dp_put_gray(gid, at + px->left, y - px->top,
						    px->bm, px->pitch, px->w, px->h, colour);
				} else {
					dp_put_mono(gid, at + px->left, y - px->top,
						    px->bm, px->pitch, px->w, px->h, colour);
				}
				fn_count.put++;
			}
			fn_count.ms_put += fn_now() - t;
		}
		pen += g->adv;
		i += n;
	}
	base_end(&bw);
	if ( p_fit != NULL ) {
		*p_fit = i;
	}

	return ( pen + 32 ) >> 6;
}

LOCAL INT fn_draw_nl( INT gid, ID fid, INT x, INT y, CONST UB *utf8, UW colour )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( utf8 == NULL || f->px == 0 ) {
		return E_PAR;
	}

	return walk(gid, f, x, y, utf8, colour, -1, NULL);
}

EXPORT INT fn_draw( INT gid, ID fid, INT x, INT y, CONST UB *utf8, UW colour )
{
	INT	r;

	enter();
	r = fn_draw_nl(gid, fid, x, y, utf8, colour);
	leave();
	return r;
}

LOCAL INT fn_width_nl( ID fid, CONST UB *utf8 )
{
	FONT	*f = fn_of(fid);

	if ( f == NULL ) {
		return E_ID;
	}
	if ( utf8 == NULL || f->px == 0 ) {
		return E_PAR;
	}

	return walk(-1, f, 0, 0, utf8, 0, -1, NULL);
}

EXPORT INT fn_width( ID fid, CONST UB *utf8 )
{
	INT	r;

	enter();
	r = fn_width_nl(fid, utf8);
	leave();
	return r;
}

/* ---------------------------------------------------------------- down the line */

/*
 * How far the top of the em square is above the baseline, in pixels: the
 * typographic ascender of OS/2 when it and the descender span the em, as
 * they do in a face made for Japanese; else 88 hundredths of the size.
 */
LOCAL INT em_top( CONST FONT *f )
{
	TT_OS2	*os2 = (TT_OS2 *)FT_Get_Sfnt_Table(f->face, FT_SFNT_OS2);
	INT	upem = f->face->units_per_EM;

	if ( os2 != NULL && upem > 0 && os2->sTypoAscender > 0 ) {
		INT	span = os2->sTypoAscender - os2->sTypoDescender;

		if ( span >= upem - upem / 10 && span <= upem + upem / 10 ) {
			return ( os2->sTypoAscender * f->px + upem / 2 ) / upem;
		}
	}

	return f->px * 88 / 100;
}

/*
 * What a face with no vertical form of a letter does with it: the long
 * vowel, the dashes, the waves, the brackets, the colons and the lines
 * of boxes are turned a quarter clockwise (1); the comma and the full
 * stop go to the upper right of their box (2), the small kana a little
 * up and to the right (3). Anything else stands as it is (0).
 */
LOCAL INT vert_kind( UW c )
{
	if ( c == 0x30FC || c == 0x2015 || c == 0x2014 || c == 0x2010
	  || c == 0xFF5E || c == 0x301C || c == 0x2026 || c == 0x2025
	  || c == 0xFF08 || c == 0xFF09 || ( c >= 0x3008 && c <= 0x3011 )
	  || c == 0x3014 || c == 0x3015 || c == 0xFF3B || c == 0xFF3D
	  || c == 0xFF5B || c == 0xFF5D || c == 0xFF1D || c == 0xFF1A
	  || c == 0xFF1B || c == 0xFF3F || c == 0xFFE3
	  || ( c >= 0x2500 && c <= 0x254B ) ) {
		return 1;
	}
	if ( c == 0x3001 || c == 0x3002 || c == 0xFF0C || c == 0xFF0E ) {
		return 2;
	}
	if ( c == 0x3041 || c == 0x3043 || c == 0x3045 || c == 0x3047
	  || c == 0x3049 || c == 0x3063 || c == 0x3083 || c == 0x3085
	  || c == 0x3087 || c == 0x308E || c == 0x30A1 || c == 0x30A3
	  || c == 0x30A5 || c == 0x30A7 || c == 0x30A9 || c == 0x30C3
	  || c == 0x30E3 || c == 0x30E5 || c == 0x30E7 || c == 0x30EE
	  || c == 0x30F5 || c == 0x30F6 ) {
		return 3;
	}

	return 0;
}

LOCAL BOOL has_vert_nl( FONT *f, UW cp )
{
	FT_UInt	gi = FT_Get_Char_Index(f->face, (FT_ULong)cp);

	return (BOOL)( gi != 0 && vert_of(f, gi) != gi );
}

/*
 * The walk down a line: each letter in a box the size's height tall (a
 * turned one as tall as its turned box), across the box centred on cx.
 */
LOCAL INT vwalk( INT gid, FONT *f0, INT cx, INT y, CONST UB *s, UW colour )
{
	FONT	*f = f0;
	INT	pen = 0, i = 0, em = f->px, top = em_top(f);
	INT	keep = f->angle;
	ID	fid;
	BOOL	rec = (BOOL)( gid >= 0 && dp_is_rec(gid) );
	INT	sc = ( gid >= 0 && !rec ) ? dp_scale(gid) : 100;
	BASEW	bw;

	bw.b = NULL;
	while ( s[i] != '\0' ) {
		INT		n = 1, dx = 0, dy = 0, base, adv, q = 0, phase = 0;
		UW		cp = utf8_next(s + i, &n);
		BOOL		turned = (BOOL)( keep != 0 ), loose = FALSE;
		CONST GLYPH	*g;

		f = letter_face(f0, cp, &bw);
		fid = fn_id_of(f);
		if ( keep == 0 && ( f->style & FN_ST_VERT ) != 0 ) {
			INT	k = vert_kind(cp);

			if ( k != 0 && !has_vert_nl(f, cp) ) {
				if ( k == 1 ) {
					f->angle = 90;
					turned = TRUE;
				} else if ( k == 2 ) {
					dx = em * 6 / 10;
					dy = -( em * 6 / 10 );
				} else {
					dx = em / 10;
					dy = -( em / 10 );
				}
			}
		}
		/*
		 * The letter's box centred on the line: its left edge half its
		 * advance left of the middle, to the quarter of a pixel.
		 */
		if ( sc > 100 ) {
			g = glyph_of(f, fid, cp, FALSE, 0, NULL);
			if ( g != NULL ) {
				q = quarter_of(( dx << 6 ) - g->adv / 2, sc);
			}
		} else if ( gid >= 0 && !rec ) {
			g = glyph_of(f, fid, cp, FALSE, 0, NULL);
			if ( g != NULL ) {
				q = ( ( ( cx + dx ) << 6 ) - g->adv / 2 + ( 1 << ( FN_PHASE_SH - 1 ) ) )
				    >> FN_PHASE_SH;
				phase = q & ( FN_PHASES - 1 );
				g = glyph_of(f, fid, cp, TRUE, phase, &loose);
			}
		} else {
			g = glyph_of(f, fid, cp, FALSE, 0, NULL);
		}
		if ( g == NULL ) {
			f->angle = keep;
			pen += em;
			i += n;
			continue;
		}
		if ( turned ) {
			adv = g->up + g->down;
			base = y + pen + g->up;
		} else {
			adv = em;
			base = y + pen + top + dy;
		}
		if ( sc > 100 ) {
			/* the box's middle on cx and its baseline below y, in the target's pixels */
			letter_scaled(gid, f, cp, cx, y, q >> 2, disp_dev_of(base - y, (UINT)sc),
				      q & ( FN_PHASES - 1 ), sc, colour);
		} else if ( rec ) {
			rec_letter(gid, f, cp, ( (FT_Fixed)( cx + dx ) << 16 ) - ( (FT_Fixed)g->adv << 9 ),
				   base, colour);
		} else if ( gid >= 0 ) {
			INT		x = q >> ( 6 - FN_PHASE_SH );
			CONST GPIX	*px = g->pix[phase];
			UD		t = fn_now();

			if ( loose ) {
				blit(gid, &f->face->glyph->bitmap, x + f->face->glyph->bitmap_left,
				     base - f->face->glyph->bitmap_top, colour);
				fn_count.put++;
			} else if ( px != NULL && px->w > 0 && px->h > 0 ) {
				if ( px->gray ) {
					dp_put_gray(gid, x + px->left, base - px->top,
						    px->bm, px->pitch, px->w, px->h, colour);
				} else {
					dp_put_mono(gid, x + px->left, base - px->top,
						    px->bm, px->pitch, px->w, px->h, colour);
				}
				fn_count.put++;
			}
			fn_count.ms_put += fn_now() - t;
		}
		f->angle = keep;
		pen += adv;
		i += n;
	}
	base_end(&bw);

	return pen;
}

EXPORT INT fn_vdraw( INT gid, ID fid, INT cx, INT y, CONST UB *utf8, UW colour )
{
	FONT	*f;
	INT	r;

	enter();
	f = fn_of(fid);
	if ( f == NULL ) {
		r = E_ID;
	} else if ( utf8 == NULL || f->px == 0 ) {
		r = E_PAR;
	} else {
		r = vwalk(gid, f, cx, y, utf8, colour);
	}
	leave();
	return r;
}

EXPORT INT fn_vwidth( ID fid, CONST UB *utf8 )
{
	return fn_vdraw(-1, fid, 0, 0, utf8, 0);
}

/*
 * A table of the face's file, by its tag (four letters, the first the
 * highest byte), as it is in the file: what a file of pages needs to
 * carry the face with it. With buf NULL only the length is said.
 */
EXPORT ER fn_sfnt_table( ID fid, UW tag, UB *buf, SZ *p_len )
{
	FONT		*f;
	FT_ULong	len;
	ER		r = E_OK;

	if ( p_len == NULL ) {
		return E_PAR;
	}
	enter();
	f = fn_of(fid);
	if ( f == NULL ) {
		r = E_ID;
	} else {
		len = ( buf != NULL ) ? (FT_ULong)*p_len : 0;
		if ( FT_Load_Sfnt_Table(f->face, (FT_ULong)tag, 0, buf, &len) != 0 ) {
			r = E_NOEXS;
		} else {
			*p_len = (SZ)len;
		}
	}
	leave();
	return r;
}

EXPORT BOOL fn_has_vert( ID fid, UW cp )
{
	FONT	*f;
	BOOL	r = FALSE;

	enter();
	f = fn_of(fid);
	if ( f != NULL ) {
		r = has_vert_nl(f, cp);
	}
	leave();
	return r;
}

LOCAL ER fn_extent_nl( ID fid, CONST UB *utf8, INT *p_above, INT *p_below )
{
	FONT	*f = fn_of(fid);
	INT	up, down, i = 0;

	if ( f == NULL ) {
		return E_ID;
	}
	if ( utf8 == NULL || f->px == 0 || p_above == NULL || p_below == NULL ) {
		return E_PAR;
	}
	/* upright, every letter's box is the face's own ascent and descent */
	(void)advance_of(f, 0, &up, &down);
	if ( f->angle != 0 ) {
		up = down = 0;
		while ( utf8[i] != '\0' ) {
			INT		n = 1;
			UW		cp = utf8_next(utf8 + i, &n);
			CONST GLYPH	*g = glyph_of(f, fn_id_of(f), cp, FALSE, 0, NULL);

			if ( g != NULL ) {
				if ( g->up > up ) up = g->up;
				if ( g->down > down ) down = g->down;
			}
			i += n;
		}
	}
	*p_above = up;
	*p_below = down;

	return E_OK;
}

EXPORT ER fn_extent( ID fid, CONST UB *utf8, INT *p_above, INT *p_below )
{
	ER	r;

	enter();
	r = fn_extent_nl(fid, utf8, p_above, p_below);
	leave();
	return r;
}

LOCAL INT fn_fit_nl( ID fid, CONST UB *utf8, INT width )
{
	FONT	*f = fn_of(fid);
	INT	fit = 0;

	if ( f == NULL ) {
		return E_ID;
	}
	if ( utf8 == NULL || f->px == 0 || width < 0 ) {
		return E_PAR;
	}
	walk(-1, f, 0, 0, utf8, 0, width, &fit);

	return fit;
}

EXPORT INT fn_fit( ID fid, CONST UB *utf8, INT width )
{
	INT	r;

	enter();
	r = fn_fit_nl(fid, utf8, width);
	leave();
	return r;
}

/* ---------------------------------------------------------------- which face */

/* Whether a face is one to be found by kind and listed */
LOCAL BOOL in_use( CONST FONT *f )
{
	return (BOOL)( f->used && f->reg.use == FN_USE_ON );
}

/* The family a face goes by: the one it is registered as, else its own */
LOCAL CONST char *fam_of( CONST FONT *f )
{
	if ( f->reg.family[0] != 0 ) {
		return (CONST char *)f->reg.family;
	}
	return ( f->face != NULL && f->face->family_name != NULL ) ? f->face->family_name : "";
}

EXPORT INT fn_nface( void )
{
	INT	i, n = 0;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( in_use(&fn_tab[i]) ) {
			n++;
		}
	}

	return n;
}

EXPORT ID fn_face_at( INT k )
{
	INT	i;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( in_use(&fn_tab[i]) && k-- == 0 ) {
			return (ID)( i + 1 );
		}
	}

	return 0;
}

EXPORT INT fn_family( ID fid, UB *buf, INT max )
{
	FONT		*f = fn_of(fid);
	CONST char	*nm;
	INT		n = 0;

	if ( f == NULL || buf == NULL || max <= 0 ) {
		return E_PAR;
	}
	nm = fam_of(f);
	while ( nm[n] != 0 && n < max - 1 ) {
		buf[n] = (UB)nm[n];
		n++;
	}
	buf[n] = 0;

	return n;
}

LOCAL UB lower_a( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c + 'a' - 'A' ) : c;
}

/* Whether 'a' (n bytes) and 'b' are the same name, letters either way up */
LOCAL BOOL same_name( CONST UB *a, INT n, CONST char *b )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( b[i] == 0 || lower_a(a[i]) != lower_a((UB)b[i]) ) {
			return FALSE;
		}
	}

	return (BOOL)( b[n] == 0 );
}

/* Whether 'w' (n bytes) is somewhere in 'in', letters either way up */
LOCAL BOOL has_word( CONST char *in, CONST char *w )
{
	INT	i, k;

	for ( i = 0; in[i] != 0; i++ ) {
		for ( k = 0; w[k] != 0 && lower_a((UB)in[i + k]) == lower_a((UB)w[k]); k++ ) {
			;
		}
		if ( w[k] == 0 ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* A face in use whose family is 'name', other than 'not'; NULL none */
LOCAL FONT *face_named( CONST UB *name, CONST FONT *not )
{
	INT	i, n = (INT)tstrlen_u(name);

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( in_use(&fn_tab[i]) && &fn_tab[i] != not && same_name(name, n, fam_of(&fn_tab[i])) ) {
			return &fn_tab[i];
		}
	}

	return NULL;
}

/*
 * The first face in use of a kind: one registered with a class is of
 * the kind its class says (the ends of its strokes ornamented, CLS_SERIF,
 * or not) when serif is 0 or 1; any other by a word of its family. serif
 * below zero: by the words alone.
 */
#define CLS_SERIF	0x6000U

LOCAL ID face_with( CONST char *w1, CONST char *w2, INT serif )
{
	INT	i;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		CONST FONT	*f = &fn_tab[i];
		CONST char	*nm;

		if ( !in_use(f) ) {
			continue;
		}
		if ( serif >= 0 && f->reg.cls != 0 ) {
			if ( ( ( f->reg.cls & CLS_SERIF ) != 0 ) == ( serif != 0 ) ) {
				return (ID)( i + 1 );
			}
			continue;
		}
		nm = fam_of(f);
		if ( has_word(nm, w1) || ( w2 != NULL && has_word(nm, w2) ) ) {
			return (ID)( i + 1 );
		}
	}

	return 0;
}

/*
 * How near a face's look is to the one wanted: each of the looks it has
 * counts for it when wanted and against it, more, when not -- a face of
 * bold letters is a poor answer to plain ones.
 */
LOCAL INT look_score( UINT has, UINT want )
{
	INT	s = 0, b;

	for ( b = 0; b < 4; b++ ) {
		UINT	m = 1U << b;

		if ( ( has & m ) != 0 ) {
			s += ( ( want & m ) != 0 ) ? 2 : -3;
		}
	}

	return s;
}

/* The face of the family a (n bytes) in the use asked for, most nearly the look; 0 none */
LOCAL ID family_face( CONST UB *a, INT n, UINT want, UINT use )
{
	INT	i, best = -1, bs = -100, s;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		CONST FONT	*f = &fn_tab[i];

		if ( !f->used || f->reg.use != use || !same_name(a, n, fam_of(f)) ) {
			continue;
		}
		s = look_score(f->reg.attr & FN_FA_LOOK, want);
		if ( s > bs ) {
			bs = s;
			best = i;
		}
	}

	return ( best >= 0 ) ? (ID)( best + 1 ) : 0;
}

EXPORT ID fn_find_style( CONST UB *names, UINT want, UINT *p_has )
{
	CONST UB	*p = names;
	ID		fid = 0;

	while ( fid == 0 && p != NULL && *p != 0 ) {
		CONST UB	*a, *e;
		INT		n;

		while ( *p == ' ' || *p == ',' ) {
			p++;
		}
		a = p;
		while ( *p != 0 && *p != ',' ) {
			p++;
		}
		e = p;
		while ( e > a && ( e[-1] == ' ' ) ) e--;
		if ( e > a && ( *a == '"' || *a == '\'' ) ) a++;
		if ( e > a && ( e[-1] == '"' || e[-1] == '\'' ) ) e--;
		n = (INT)( e - a );
		if ( n <= 0 ) {
			continue;
		}
		fid = family_face(a, n, want, FN_USE_ON);
		if ( fid == 0 ) {
			fid = family_face(a, n, want, FN_USE_TRIAL);
		}
		if ( fid > 0 ) {
			break;
		}
		/* the general names: the first face of that kind */
		if ( same_name(a, n, "serif") || same_name(a, n, "明朝")
		  || same_name(a, n, "mincho") ) {
			fid = face_with("Serif", "Mincho", 1);
		} else if ( same_name(a, n, "sans-serif") || same_name(a, n, "ゴシック")
			 || same_name(a, n, "gothic") ) {
			fid = face_with("Sans", "Gothic", 0);
		} else if ( same_name(a, n, "monospace") ) {
			fid = face_with("Mono", NULL, -1);
		}
	}
	if ( fid == 0 ) {
		fid = fn_sys;
	}
	if ( p_has != NULL ) {
		*p_has = ( fid > 0 && fid <= FN_MAX_FONT ) ? ( fn_tab[fid - 1].reg.attr & FN_FA_LOOK & want ) : 0;
	}

	return fid;
}

EXPORT ID fn_find( CONST UB *names )
{
	return fn_find_style(names, 0, NULL);
}

/* ---------------------------------------------------------------- what a face is registered as */

EXPORT ER fn_set_reg( ID fid, CONST T_FNREG *reg )
{
	FONT	*f;

	if ( reg == NULL ) {
		return E_PAR;
	}
	enter();
	f = fn_of(fid);
	if ( f == NULL ) {
		leave();
		return E_ID;
	}
	f->reg = *reg;
	f->reg.family[FN_NAME_MAX - 1] = 0;
	f->reg.base[FN_NAME_MAX - 1] = 0;
	leave();

	return E_OK;
}

EXPORT ER fn_get_reg( ID fid, T_FNREG *reg )
{
	FONT	*f;

	if ( reg == NULL ) {
		return E_PAR;
	}
	enter();
	f = fn_of(fid);
	if ( f != NULL ) {
		*reg = f->reg;
	}
	leave();

	return ( f != NULL ) ? E_OK : E_ID;
}

EXPORT ID fn_of_obj( CONST TS_UUID *uuid, INT index )
{
	INT	i, k;

	if ( uuid == NULL ) {
		return 0;
	}
	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		CONST FONT	*f = &fn_tab[i];

		if ( !f->used || !f->from_obj || f->index != index || f->reg.use == FN_USE_TRIAL ) {
			continue;
		}
		for ( k = 0; k < 16 && f->obj.b[k] == uuid->b[k]; k++ ) ;
		if ( k == 16 ) {
			return (ID)( i + 1 );
		}
	}

	return 0;
}

/* Whether a file name ends as a font file does */
LOCAL BOOL is_font_name( CONST UB *nm )
{
	INT	n = 0;

	while ( nm[n] != 0 ) {
		n++;
	}
	if ( n < 5 || nm[n - 4] != '.' ) {
		return FALSE;
	}

	return (BOOL)( same_name(nm + n - 3, 3, "otf") || same_name(nm + n - 3, 3, "ttf")
		    || same_name(nm + n - 3, 3, "ttc") );
}

EXPORT INT fn_open_dir( CONST char *dir )
{
	T_DIRENT	*de;
	INT		fd, cnt, i, opened = 0;

	de = (T_DIRENT *)Kmalloc(sizeof(T_DIRENT) * 8);
	if ( de == NULL ) {
		return 0;
	}
	fd = fs_open(dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) {
		Kfree(de);
		return 0;
	}
	while ( ( cnt = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < cnt; i++ ) {
			char	path[160];
			INT	k = 0, j;
			ID	fid;

			if ( !is_font_name(de[i].name) ) {
				continue;
			}
			for ( j = 0; dir[j] != 0 && k < 150; j++ ) path[k++] = dir[j];
			path[k++] = '/';
			for ( j = 0; de[i].name[j] != 0 && k < 158; j++ ) {
				path[k++] = (char)de[i].name[j];
			}
			path[k] = 0;
			/* a face that is open already is not opened again */
			{
				UB	fam[64];
				BOOL	have = FALSE;
				INT	m;

				if ( fn_open_file(path, 0, &fid) < E_OK ) {
					continue;
				}
				(void)fn_family(fid, fam, sizeof(fam));
				for ( m = 0; m < FN_MAX_FONT; m++ ) {
					if ( fn_tab[m].used && (ID)( m + 1 ) != fid
					  && fn_tab[m].face->family_name != NULL
					  && same_name(fam, (INT)tstrlen_u(fam),
						       fn_tab[m].face->family_name) ) {
						have = TRUE;
					}
				}
				if ( have ) {
					fn_close(fid);
				} else {
					opened++;
				}
			}
		}
	}
	fs_close(fd);
	Kfree(de);

	return opened;
}
