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
#include <ts/fs.h>
#include <ts/ob.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_TRIGONOMETRY_H

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

EXPORT ER fn_state_set( ID fid, CONST T_FNSTATE *st )
{
	FONT	*f = ( fid >= 1 && fid <= FN_MAX_FONT && fn_tab[fid - 1].used ) ? &fn_tab[fid - 1] : NULL;

	if ( f == NULL || st == NULL ) {
		return E_PAR;
	}
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

typedef struct {
	BOOL	used;
	ID	fid;
	INT	px;
	UINT	look;			/* the style and the width it was made in */
	UW	cp;
	INT	adv;			/* how far the pen moves */
	INT	up, down;		/* how far its box reaches above and below
					   the baseline */
	INT	left, top;		/* where the pixels sit against the pen */
	INT	w, h, pitch;
	BOOL	drawn;			/* whether the pixels were made */
	BOOL	nopix;			/* and whether they could not be kept */
	BOOL	has_adv;		/* the advance is settled */
	UD	when;			/* the last time it was asked for */
	UB	*bm;			/* the pixels, or NULL */
	BOOL	gray;			/* a byte a pixel, how much it covers; else a bit */
	INT	nbytes;			/* how many they came to */
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
	tm_printf((UB *)"  fn: ask %d hit %d made %d put %d\n",
		  (INT)fn_count.ask, (INT)fn_count.hit,
		  (INT)fn_count.made, (INT)fn_count.put);
	tm_printf((UB *)"  fn: make %d ms, put %d ms\n",
		  (INT)fn_count.ms_make, (INT)fn_count.ms_put);
}

/* Let a letter go, pixels and all */
LOCAL void glyph_drop( GLYPH *g )
{
	if ( g->bm != NULL ) {
		fn_held -= (SZ)g->nbytes;
		Kfree(g->bm);
		g->bm = NULL;
		g->nbytes = 0;
	}
	g->used = FALSE;
	g->drawn = FALSE;
}

/*
 * Room for one letter's pixels. The letters asked for longest ago are
 * dropped until what is held is within what may be held; the letter
 * being made is not among them, because it has not been asked for yet.
 */
LOCAL UB *glyph_room( GLYPH *keep, INT n )
{
	UB	*mem;

	while ( fn_held + (SZ)n > GC_CAP ) {
		GLYPH	*old = NULL;
		INT	i;

		for ( i = 0; i < GC_MAX; i++ ) {
			GLYPH	*g = &fn_cache[i];

			if ( !g->used || g == keep || g->bm == NULL ) {
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
	mem = (UB *)Kmalloc((SZ)n);
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
	take->left    = 0;
	take->top     = 0;
	take->w       = 0;
	take->h       = 0;
	take->pitch   = 0;
	take->has_adv = FALSE;
	take->nopix   = FALSE;
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
 * (64ths), thickened and turned as the face is set; and how far its box
 * reaches above and below the baseline.
 */
LOCAL INT advance_of( CONST FONT *f, FT_Pos adv, INT *p_up, INT *p_down )
{
	CONST FT_Size_Metrics	*m = &f->face->size->metrics;
	FT_Pos			cy, h, w;

	adv += (FT_Pos)bold_extra(f) * 64;
	if ( f->angle == 0 ) {
		*p_up = (INT)( m->ascender >> 6 );
		*p_down = (INT)( -( m->descender >> 6 ) );
		return (INT)( adv >> 6 );
	}
	w = turn_box(f, adv, &cy, &h);
	*p_up = (INT)( ( cy + h / 2 + 63 ) >> 6 );
	*p_down = (INT)( ( h / 2 - cy + 63 ) >> 6 );

	return (INT)( ( w + 32 ) >> 6 );
}

/*
 * A letter loaded in the look the face is set to. A plain letter is
 * loaded as it always was. One that is thickened, leaned or turned is
 * loaded as an outline, the outline changed -- pushed out on every side
 * for bold, sheared a fifth of its height for italic, turned in its box
 * (turn_box) -- and then made into pixels.
 */
LOCAL FT_Error load_styled( FONT *f, FT_UInt gi, FT_Int32 flags )
{
	FT_Face		face = f->face;
	FT_Error	err;

	if ( ( f->style & ( FN_ST_BOLD | FN_ST_ITALIC ) ) == 0 && f->angle == 0 ) {
		return FT_Load_Glyph(face, gi, flags);
	}
	err = FT_Load_Glyph(face, gi, ( flags & ~FT_LOAD_RENDER )
				      | FT_LOAD_NO_BITMAP);
	if ( err != 0 || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE ) {
		return ( err != 0 ) ? err : FT_Load_Glyph(face, gi, flags);
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
	if ( f->angle != 0 ) {
		FT_Angle	a = (FT_Angle)f->angle << 16;
		FT_Pos		adv = face->glyph->advance.x + (FT_Pos)bold_extra(f) * 64;
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
	if ( ( flags & FT_LOAD_RENDER ) != 0 ) {
		err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO);
	}

	return err;
}

/* The letter, made if it is not kept. NULL when it cannot be made. */
LOCAL CONST GLYPH *glyph_of( FONT *f, ID fid, UW cp, BOOL want_pixels )
{
	GLYPH		*g;
	FT_Face		face = f->face;
	FT_UInt		gi;
	FT_Int32	flags;
	CONST FT_Bitmap	*bm;
	INT		row, n;

	fn_count.ask++;
	g = cache_find(fid, f->px, look_of(f), cp);
	if ( g != NULL && ( !want_pixels || g->drawn || g->nopix ) ) {
		fn_count.hit++;
		return g;
	}
	fn_count.made++;
	gi = FT_Get_Char_Index(face, (FT_ULong)cp);
	if ( !want_pixels ) {
		/*
		 * Only the width is wanted. Loading the outline without
		 * turning it into pixels is most of the cost saved, and
		 * breaking the lines of a page asks for the width of every
		 * letter on it whether the letter is on the screen or not.
		 */
		{
			/*
			 * The advance out of the font's own table of
			 * advances, which every face carries and which every
			 * proportional face needs: one number, read without
			 * touching the outline. Reading outlines to work out
			 * where a line breaks is what made a page take a
			 * minute to open.
			 */
			FT_Fixed	fx = 0;

			{
				UD	 t = fn_now();
				FT_Error err;

				/*
				 * The width out of the face's own table of
				 * widths, without touching the outline. Asked
				 * for this way the library answers from the
				 * table or says it cannot; loading the outline
				 * to find a width costs as much as drawing the
				 * letter, and breaking a page into lines asks
				 * for the width of every letter on it.
				 */
				err = FT_Get_Advance(face, gi,
						     FT_LOAD_NO_HINTING
						     | FT_ADVANCE_FLAG_FAST_ONLY,
						     &fx);
				if ( err != 0 ) {
					err = FT_Get_Advance(face, gi,
							     FT_LOAD_NO_HINTING,
							     &fx);
				}
				fn_count.ms_make += fn_now() - t;
				if ( err != 0 ) {
					return NULL;
				}
			}
			if ( g == NULL ) {
				g = cache_room(fid, f->px, look_of(f), cp);
				g->used = TRUE;
			}
			if ( !g->has_adv ) {
				if ( f->angle == 0 ) {
					g->adv = (INT)( (fx + 0x8000) >> 16 ) + bold_extra(f);
					(void)advance_of(f, 0, &g->up, &g->down);
				} else {
					g->adv = advance_of(f, (FT_Pos)( ( fx + 0x200 ) >> 10 ),
							    &g->up, &g->down);
				}
				g->has_adv = TRUE;
			}
			g->when = ++fn_clock;

			return g;
		}
	}
	/*
	 * No hinting. Fitting an outline to the pixel grid is by far the
	 * most expensive thing the font library does -- on a face with
	 * PostScript outlines it is most of the work of making a letter --
	 * and what it buys is a stem a pixel straighter at small sizes.
	 * Measured here at a sixth of a second per letter, which is the
	 * difference between a page that appears and a page that does not.
	 */
	/*
	 * In 256 levels of how much of each pixel the letter covers: its
	 * edges are mixed into what is under them where it is put down
	 * (dp_put_gray), which is what makes a proportional face read as
	 * it was drawn rather than as stairs.
	 */
	flags = FT_LOAD_RENDER | FT_LOAD_NO_HINTING;
	{
		UD	t = fn_now();
		FT_Error err = load_styled(f, gi, flags);

		fn_count.ms_make += fn_now() - t;
		if ( err != 0 ) {
			return NULL;
		}
	}
	bm = &face->glyph->bitmap;
	if ( g == NULL ) {
		g = cache_room(fid, f->px, look_of(f), cp);
	}
	g->used = TRUE;
	/*
	 * The advance is settled the first time it is asked for and never
	 * changes afterwards.
	 *
	 * Where a line breaks is worked out before the letters are made,
	 * and the letters are made when they are drawn. If the number the
	 * breaking used and the number the drawing used could differ -- the
	 * font's own advance against the one the grid rounded it to -- the
	 * two would drift apart along the line, and the line would run past
	 * the edge it was broken to fit inside.
	 */
	if ( !g->has_adv ) {
		g->adv = advance_of(f, face->glyph->advance.x, &g->up, &g->down);
		g->has_adv = TRUE;
	}
	g->left  = face->glyph->bitmap_left;
	g->top   = face->glyph->bitmap_top;
	g->w     = (INT)bm->width;
	g->h     = (INT)bm->rows;
	g->pitch = (INT)bm->pitch;
	g->when  = ++fn_clock;
	g->drawn = TRUE;
	g->nopix = FALSE;
	n = g->pitch * g->h;
	if ( g->bm != NULL ) {
		fn_held -= (SZ)g->nbytes;	/* the size it was before */
		Kfree(g->bm);
		g->bm = NULL;
		g->nbytes = 0;
	}
	if ( bm->buffer != NULL && n > 0
	  && ( bm->pixel_mode == FT_PIXEL_MODE_MONO || bm->pixel_mode == FT_PIXEL_MODE_GRAY ) ) {
		g->bm = glyph_room(g, n);
		g->gray = ( bm->pixel_mode == FT_PIXEL_MODE_GRAY );
	}
	if ( g->bm == NULL ) {
		/* not the form this store holds, or no room to hold it: the
		   measurements are still worth keeping, the pixels are not */
		g->w = 0;
		g->h = 0;
		g->drawn = FALSE;
		g->nopix = TRUE;
		if ( want_pixels && bm->buffer != NULL ) {
			return NULL;	/* the caller draws it the long way */
		}
		return g;
	}
	g->nbytes = n;
	for ( row = 0; row < n; row++ ) {
		g->bm[row] = bm->buffer[row];
	}

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
 * The one walk. `gid` below zero means do not draw; `limit` at or above
 * zero means stop when the pen would pass it and say how many bytes fit.
 */
LOCAL INT walk( INT gid, FONT *f, INT x, INT y, CONST UB *s, UW colour,
		INT limit, INT *p_fit )
{
	FT_Face	face = f->face;
	INT	pen = 0, i = 0;
	/*
	 * A walk that is only measuring does not need the pixels. Making
	 * them is the expensive part by a wide margin, and a page of text
	 * is measured at least as often as it is drawn -- once to break
	 * its lines and once to know how tall it came to.
	 */
	FT_Int32 flags = ( gid >= 0 )
			 ? (FT_LOAD_RENDER
			    | FT_LOAD_NO_HINTING)
			 : (FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);

	while ( s[i] != '\0' ) {
		UW		cp;
		INT		n = 1, adv;
		CONST GLYPH	*g;

		cp = utf8_next(s + i, &n);
		g = glyph_of(f, fn_id_of(f), cp, (BOOL)( gid >= 0 ));
		if ( g != NULL ) {
			adv = g->adv;
			if ( limit >= 0 && pen + adv > limit ) {
				break;
			}
			if ( gid >= 0 && g->w > 0 && g->h > 0 ) {
				UD	t = fn_now();

				if ( g->gray ) {
					dp_put_gray(gid, x + pen + g->left, y - g->top,
						    g->bm, g->pitch, g->w, g->h, colour);
				} else {
					dp_put_mono(gid, x + pen + g->left, y - g->top,
						    g->bm, g->pitch, g->w, g->h, colour);
				}
				fn_count.ms_put += fn_now() - t;
				fn_count.put++;
			}
			pen += adv;
			i += n;
			continue;
		}
		{
			FT_UInt	gi = FT_Get_Char_Index(face, (FT_ULong)cp);

			/* not in the store and not the form it holds: make
			   it and put it down without keeping it */
			if ( load_styled(f, gi, flags) != 0 ) {
				i += n;
				continue;
			}
			{
				INT	up, down;

				adv = advance_of(f, face->glyph->advance.x, &up, &down);
			}
			if ( limit >= 0 && pen + adv > limit ) {
				break;
			}
			if ( gid >= 0 ) {
				blit(gid, &face->glyph->bitmap,
				     x + pen + face->glyph->bitmap_left,
				     y - face->glyph->bitmap_top, colour);
			}
			pen += adv;
			i += n;
		}
	}
	if ( p_fit != NULL ) {
		*p_fit = i;
	}

	return pen;
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
			CONST GLYPH	*g = glyph_of(f, fn_id_of(f), cp, FALSE);

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

EXPORT INT fn_nface( void )
{
	INT	i, n = 0;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( fn_tab[i].used ) {
			n++;
		}
	}

	return n;
}

EXPORT ID fn_face_at( INT k )
{
	INT	i;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( fn_tab[i].used && k-- == 0 ) {
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
	nm = ( f->face->family_name != NULL ) ? f->face->family_name : "";
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

/* The first face whose family has one of the words in it, or 0 */
LOCAL ID face_with( CONST char *w1, CONST char *w2 )
{
	INT	i;

	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		CONST char	*nm;

		if ( !fn_tab[i].used || fn_tab[i].face->family_name == NULL ) {
			continue;
		}
		nm = fn_tab[i].face->family_name;
		if ( has_word(nm, w1) || ( w2 != NULL && has_word(nm, w2) ) ) {
			return (ID)( i + 1 );
		}
	}

	return 0;
}

EXPORT ID fn_find( CONST UB *names )
{
	CONST UB	*p = names;

	while ( p != NULL && *p != 0 ) {
		CONST UB	*a, *e;
		INT		i, n;
		ID		fid = 0;

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
		for ( i = 0; i < FN_MAX_FONT; i++ ) {
			if ( fn_tab[i].used && fn_tab[i].face->family_name != NULL
			  && same_name(a, n, fn_tab[i].face->family_name) ) {
				return (ID)( i + 1 );
			}
		}
		/* the general names: the first face of that kind */
		if ( same_name(a, n, "serif") || same_name(a, n, "明朝")
		  || same_name(a, n, "mincho") ) {
			fid = face_with("Serif", "Mincho");
		} else if ( same_name(a, n, "sans-serif") || same_name(a, n, "ゴシック")
			 || same_name(a, n, "gothic") ) {
			fid = face_with("Sans", "Gothic");
		} else if ( same_name(a, n, "monospace") ) {
			fid = face_with("Mono", NULL);
		}
		if ( fid > 0 ) {
			return fid;
		}
	}

	return fn_sys;
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
