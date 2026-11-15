/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp.c
 *	Screen, the part that does not depend on the machine (design 16.5).
 *
 *	The back buffer is ordinary memory, so drawing into it runs at the
 *	speed of memory and anything may read it back. The framebuffer is
 *	written to and never read: on the boards this is for, reading it
 *	costs more than writing it, and writing it already costs far more
 *	than the drawing did.
 *
 *	What changed is remembered as a few separate rectangles, and each
 *	is sent by itself. Two that overlap or lie close together are
 *	joined when the rectangle round both is at most DISP_SLACK pixels
 *	larger than the two are; past that, sending them apart costs less
 *	than sending what lies between them. A caret blinking in one corner
 *	and a clock ticking in the other are two small sends, not the whole
 *	screen. When the list is full, a new rectangle is joined to the one
 *	it adds least to.
 *
 *	The back buffer is always four bytes a pixel, blue lowest, whatever
 *	the screen is. When the screen is the same, a row is sent as one
 *	copy. When it is not -- red lowest, two bytes a pixel, or a top
 *	byte it reads as opacity -- each pixel is converted on its way out.
 *	That costs little beside the write itself, and it keeps every
 *	drawing routine to one layout.
 *
 *	Where the framebuffer has room for two screens, one under the
 *	other, and the screen can be told which of them to show (T_DISPHW
 *	flip), a send goes to the page not shown and that page is shown
 *	when the send is over: the screen never shows a frame half sent.
 *	The page not shown is a send behind, so a send writes it the
 *	rectangles of the last send as well as its own, from the back
 *	buffer, which holds the newest of both. A flip takes from the
 *	screen's next refresh, and until then the page that was shown is
 *	still being read out, so the next send waits until it has surely
 *	been taken: for the next vertical blank where the screen can say
 *	when that is, else for the time a refresh takes. Sends further
 *	apart than that do not wait at all.
 *
 *	Where the screen also says how long a refresh is and when it last
 *	saw a blank end (T_DISPHW frame_ns, blank_seen), the blanks are a
 *	clock: a send that comes after the blank that took the flip before
 *	goes on at once, where watching the screen for the next blank would
 *	wait a whole refresh more, and one that comes early sleeps until
 *	just before the blank and leaves the processor to the others. How
 *	the flips kept to the refresh is counted (ts_disp_frames).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/time.h>
#include "sysman/pfalloc.h"
#include "disp_hw.h"

LOCAL T_DISPSPEC	disp_spec;

/*
 * The scale the screen is drawn at (ts_disp_setscale): the largest of
 * the list, up to the one asked for, that leaves a view of at least the
 * least size; 100 when none does. The view is the screen's pixels at
 * that scale, rounded up.
 */
LOCAL CONST UINT	disp_scales[DISP_NSCALE] = DISP_SCALES;

LOCAL UINT scale_for( UINT want, UINT w, UINT h )
{
	UINT	s = 100;
	INT	i;

	for ( i = 0; i < DISP_NSCALE; i++ ) {
		if ( disp_scales[i] <= want
		  && disp_view_len(w, disp_scales[i]) >= DISP_VIEW_MIN_W
		  && disp_view_len(h, disp_scales[i]) >= DISP_VIEW_MIN_H ) {
			s = disp_scales[i];
		}
	}
	return s;
}

LOCAL void view_set( void )
{
	if ( disp_spec.scale_want == 0 ) {
		disp_spec.scale_want = 100;
	}
	disp_spec.scale = scale_for(disp_spec.scale_want, disp_spec.width, disp_spec.height);
	disp_spec.view_w = disp_view_len(disp_spec.width, disp_spec.scale);
	disp_spec.view_h = disp_view_len(disp_spec.height, disp_spec.scale);
}
LOCAL T_DISPHW		disp_hw;
LOCAL BOOL		disp_found = FALSE;
LOCAL UB		*disp_back = NULL;	/* where everything draws */
LOCAL UB		*disp_fb = NULL;	/* where the screen reads from: its first page */
LOCAL UB		*disp_fb2 = NULL;	/* its second, when not the rows after the first */
LOCAL UINT		disp_alpha;		/* DISP_ALPHA_* */
LOCAL ID		disp_mtxid = 0;

LOCAL INT		disp_held = 0;
LOCAL ID		disp_hold_tsk = 0;	/* the task that took the first hold of those now held */
LOCAL UD		disp_hold_at = 0;	/* and when, ns */
LOCAL T_DISPSTAT	disp_stat;

#define DISP_RECTS	16		/* separate rectangles kept at most */
#define DISP_SLACK	4096		/* pixels a join may add before two are kept apart */

typedef struct {
	T_DISPRECT	r[DISP_RECTS];	/* cut to the screen */
	INT		n;
} RECTS;

LOCAL RECTS		disp_new;	/* what changed and is not sent */

/*
 * The outputs besides the main one (10.4.8), in the order they were put:
 * each shows the rectangle of the back buffer at x, y of its own size.
 * The main one, disp_hw, shows the rectangle at disp_spec.main_x, main_y:
 * the corner, unless an output is left of it or above it.
 *
 * An output that has two pages and can be told which to show keeps its
 * own frames: what changed on it gathers in pend, and goes to the page
 * not shown, with what that page lacks (old), once the output's last
 * flip has been taken; then that page is shown. A send never waits for
 * another output's flip: while one is not yet taken, what changed waits
 * in pend, and the output task (out_task) sends it as soon as the flip is
 * taken, at that output's own refresh.
 */
#define DISP_OUTS	( DISP_SCREENS_MAX - 1 )

typedef struct {
	T_DISPHW	hw;
	INT		id;		/* which it is, as long as it is on a screen: 1 or more */
	INT		side;		/* DISP_SIDE_*: which edge of the screens before it it is put past, or MIRROR */
	INT		offset;		/* how far along that edge */
	INT		x, y;		/* where its rectangle of the back buffer is */
	UINT		sw, sh;		/* and how large: its framebuffer's, or the main screen's when it mirrors it */
	BOOL		scaled;		/* that rectangle is shown scaled, into fit of its framebuffer */
	T_DISPRECT	fit;
	BOOL		flipping;	/* two pages, the one not shown sent to and then shown */
	UINT		shown;		/* the page shown */
	RECTS		pend;		/* what changed and has not been sent to it */
	RECTS		old;		/* sent to the page shown and not to the other */
	BOOL		due;		/* a flip that may not have been taken yet */
	UD		due_at;		/* when it was made, ns */
	UD		flips;		/* how many it took */
	UD		deferred;	/* sends that waited for its flip, not for the compositor */
	UD		pixels;		/* pixels copied to it, with what a page lacked */
	UD		send_us;	/* the time that took */
} DISPOUT;

LOCAL DISPOUT		disp_out[DISP_OUTS];
LOCAL INT		disp_nout = 0;
LOCAL INT		disp_next_id = 1;

/*
 * The screens as they were before the last change of their sizes, places
 * or scale, in view pixels, each with the id of its output (0: the main
 * one), so that a point of the old view can be told where it is now
 * (knl_disp_follow)
 */
typedef struct {
	INT	id;
	INT	x, y, w, h;
} DISPWAS;

LOCAL DISPWAS		disp_was[DISP_SCREENS_MAX];
LOCAL INT		disp_nwas = 0;
LOCAL ID		disp_out_tsk = 0;	/* sends what waited for an output's flip */

/* Two pages: which is shown, and what the other lacks of it */
LOCAL BOOL		disp_flipping = FALSE;
LOCAL UINT		disp_shown = 0;
LOCAL RECTS		disp_old;	/* sent to the page shown and not to the other */
LOCAL BOOL		disp_due = FALSE;	/* a flip that may not have been taken yet */
LOCAL INT		disp_vs_miss = 0;	/* blanks not seen in a row */
LOCAL UD		disp_due_at = 0;	/* when it was made, ns */
LOCAL UD		disp_flip_us = 0;	/* how long the last flip took to ask for */

/* The refresh: how the flips kept to it, and the blanks as a clock */
#define DISP_FRAME_NS	16666667ULL	/* a refresh at 60 Hz, where the screen does not say */
#define DISP_SEEN_NS	1000000000ULL	/* a blank seen this long ago still sets the clock */
#define DISP_GUARD_US	200		/* a flip made this close before a blank may miss it */
#define DISP_WAKE_US	1000		/* a sleep for the blank ends this long before it begins */

LOCAL T_DISPFRAME	disp_fr;
LOCAL UD		disp_flip_at = 0;	/* when the last flip was made, ns; 0: none yet */

/* ---------------------------------------------------------------- rows */

/* The first byte of a page of the framebuffer */
LOCAL UB *page_of( UINT page )
{
	if ( page == 1 && disp_fb2 != NULL ) {
		return disp_fb2;
	}
	return disp_fb + (UBINT)page * disp_spec.fb_pitch * disp_hw.height;
}

/* One row of pixels converted to what a screen of that format and alpha takes */
LOCAL void send_row( UW *d, CONST UW *s, INT n, UINT format, UINT alpha )
{
	UW	keep = ( alpha == DISP_ALPHA_ANY ) ? 0xFFFFFFFFU : 0x00FFFFFFU;
	UW	set = ( alpha == DISP_ALPHA_FF ) ? 0xFF000000U : 0;
	INT	x;

	if ( format == DISP_FMT_XBGR8888 ) {
		for ( x = 0; x < n; x++ ) {
			UW	v = s[x];

			d[x] = ( ( ( v & 0xFF00FF00U ) | ( ( v >> 16 ) & 0xFFU )
				   | ( ( v & 0xFFU ) << 16 ) ) & keep ) | set;
		}
	} else if ( format == DISP_FMT_RGB565 ) {
		UH	*h = (UH *)d;

		for ( x = 0; x < n; x++ ) {
			UW	v = s[x];

			h[x] = (UH)( ( ( v >> 8 ) & 0xF800U ) | ( ( v >> 5 ) & 0x07E0U )
				   | ( ( v >> 3 ) & 0x001FU ) );
		}
	} else {
		for ( x = 0; x < n; x++ ) {
			d[x] = ( s[x] & keep ) | set;
		}
	}
}

/*
 * Send one run of rows of the back buffer, inside the main screen's
 * rectangle of it, to a page of the main screen, which shows that
 * rectangle from main_x, main_y. When the screen is laid out as the back
 * buffer is, each row is one straight copy. The caller makes the writes
 * go out (dsb) once all of what is outstanding is sent.
 */
LOCAL void send_rows( UB *fb, INT top, INT bottom, INT left, INT right )
{
	UINT	bytes = (UINT)(right - left) * 4;
	UINT	off = (UINT)left * 4;
	UINT	fb_off = (UINT)( left - disp_spec.main_x ) * ( disp_spec.fb_bpp / 8 );
	BOOL	same = ( disp_spec.fb_format == DISP_FMT_XRGB8888
			 && disp_alpha == DISP_ALPHA_ANY );
	INT	y;

	for ( y = top; y < bottom; y++ ) {
		UB	*d = fb + (UBINT)( y - disp_spec.main_y ) * disp_spec.fb_pitch + fb_off;
		CONST UB *s = disp_back + (UBINT)y * disp_spec.pitch + off;

		if ( same ) {
			knl_memcpy(d, s, (SZ)bytes);
		} else {
			send_row((UW *)d, (CONST UW *)s, right - left, disp_spec.fb_format, disp_alpha);
		}
	}
	disp_stat.rects++;
	disp_stat.rows += (UD)(bottom - top);
	disp_stat.pixels += (UD)(bottom - top) * (UD)(right - left);
}

/* Bring the rectangle inside the screen, and say whether anything is left */
LOCAL BOOL clip( T_DISPRECT *r )
{
	if ( r->left < 0 ) {
		r->left = 0;
	}
	if ( r->top < 0 ) {
		r->top = 0;
	}
	if ( r->right > (INT)disp_spec.width ) {
		r->right = (INT)disp_spec.width;
	}
	if ( r->bottom > (INT)disp_spec.height ) {
		r->bottom = (INT)disp_spec.height;
	}

	return ( r->left < r->right && r->top < r->bottom );
}

/* The rectangle cut to left, top, right, bottom, and whether anything is left */
LOCAL BOOL clip_to( T_DISPRECT *r, INT left, INT top, INT right, INT bottom )
{
	if ( r->left < left ) r->left = left;
	if ( r->top < top ) r->top = top;
	if ( r->right > right ) r->right = right;
	if ( r->bottom > bottom ) r->bottom = bottom;
	return ( r->left < r->right && r->top < r->bottom );
}

LOCAL UD area_of( CONST T_DISPRECT *r )
{
	return (UD)( r->right - r->left ) * (UD)( r->bottom - r->top );
}

/*
 * The rectangle round a and b, and how many pixels it holds that
 * neither of them does
 */
LOCAL UD join_waste( CONST T_DISPRECT *a, CONST T_DISPRECT *b, T_DISPRECT *u )
{
	T_DISPRECT	o;
	UD		both = area_of(a) + area_of(b);

	u->left   = ( a->left < b->left ) ? a->left : b->left;
	u->top    = ( a->top < b->top ) ? a->top : b->top;
	u->right  = ( a->right > b->right ) ? a->right : b->right;
	u->bottom = ( a->bottom > b->bottom ) ? a->bottom : b->bottom;
	o.left   = ( a->left > b->left ) ? a->left : b->left;
	o.top    = ( a->top > b->top ) ? a->top : b->top;
	o.right  = ( a->right < b->right ) ? a->right : b->right;
	o.bottom = ( a->bottom < b->bottom ) ? a->bottom : b->bottom;
	if ( o.left < o.right && o.top < o.bottom ) {
		both -= area_of(&o);
	}
	return area_of(u) - both;
}

/*
 * One more rectangle with those of a list: joined to any it is cheaper
 * to send with than apart from, again and again as the joined one
 * grows, and kept by itself otherwise. The caller holds the lock.
 */
LOCAL void gather( RECTS *l, CONST T_DISPRECT *r0 )
{
	T_DISPRECT	r = *r0, u;
	UD		w, best_w;
	INT		i, best;

	if ( !clip(&r) ) {
		return;
	}
	for ( ;; ) {
		for ( i = 0; i < l->n; i++ ) {
			if ( join_waste(&l->r[i], &r, &u) <= DISP_SLACK ) {
				break;
			}
		}
		if ( i == l->n ) {
			if ( l->n < DISP_RECTS ) {
				l->r[l->n++] = r;
				return;
			}
			/* no room: joined to the one it adds least to */
			best = 0;
			best_w = join_waste(&l->r[0], &r, &u);
			for ( i = 1; i < l->n; i++ ) {
				w = join_waste(&l->r[i], &r, &u);
				if ( w < best_w ) {
					best_w = w;
					best = i;
				}
			}
			i = best;
			(void)join_waste(&l->r[i], &r, &u);
		}
		r = u;
		l->r[i] = l->r[--l->n];
	}
}

/* The whole screen as a list of one */
LOCAL void whole_of( RECTS *l )
{
	l->r[0].left = 0;
	l->r[0].top = 0;
	l->r[0].right = (INT)disp_spec.width;
	l->r[0].bottom = (INT)disp_spec.height;
	l->n = 1;
}

/*
 * Whoever drew on the screen before the driver (the x64 screen console)
 * is told once, before the first pixels are sent or the size is first
 * changed, so that it goes on telling the start-up until then
 */
IMPORT void knl_disp_hw_taken( void ) __attribute__((weak));
LOCAL BOOL		disp_taken = FALSE;

LOCAL void taken( void )
{
	if ( !disp_taken ) {
		disp_taken = TRUE;
		if ( knl_disp_hw_taken != NULL ) knl_disp_hw_taken();
	}
}

/*
 * The main output told that what it shows changed (front buffer tracking,
 * a panel's self refresh made to leave): after each send and each flip,
 * the driver's dirty and its output's flushed with each rectangle
 */
/*
 * A rectangle of the back buffer as the part of the main screen it
 * covers, in the screen's own pixels, and whether it covers any
 */
LOCAL BOOL on_main( CONST T_DISPRECT *r0, T_DISPRECT *m )
{
	T_DISPRECT	r = *r0;

	if ( !clip_to(&r, disp_spec.main_x, disp_spec.main_y, disp_spec.main_x + (INT)disp_hw.width,
		      disp_spec.main_y + (INT)disp_hw.height) ) {
		return FALSE;
	}
	m->left = r.left - disp_spec.main_x;
	m->right = r.right - disp_spec.main_x;
	m->top = r.top - disp_spec.main_y;
	m->bottom = r.bottom - disp_spec.main_y;
	return TRUE;
}

LOCAL void main_told( CONST RECTS *l )
{
	T_DISPRECT	m;
	INT		i;

	if ( disp_hw.dirty != NULL ) {
		disp_hw.dirty();
	}
	if ( disp_hw.out != NULL && disp_hw.out->flushed != NULL ) {
		for ( i = 0; i < l->n; i++ ) {
			if ( on_main(&l->r[i], &m) ) {
				disp_hw.out->flushed(disp_hw.out->ctx, &m);
			}
		}
	}
}

/* Each rectangle of a list sent to a page, and the writes made to go out */
LOCAL void send_list( CONST RECTS *l, UINT page )
{
	UB	*fb = page_of(page);
	UD	t0, t1;
	INT	i;

	taken();
	(void)ts_get_mono(&t0);
	for ( i = 0; i < l->n; i++ ) {
		T_DISPRECT	r = l->r[i], m;

		/* the screen may have changed size since; only the main screen's rectangle of the back buffer */
		if ( clip(&r) && on_main(&r, &m) ) {
			if ( disp_hw.out != NULL && disp_hw.out->invalidate != NULL && page == disp_shown ) {
				disp_hw.out->invalidate(disp_hw.out->ctx, &m);
			}
			send_rows(fb, m.top + disp_spec.main_y, m.bottom + disp_spec.main_y,
				  m.left + disp_spec.main_x, m.right + disp_spec.main_x);
		}
	}
	DeviceBarrier();
	(void)ts_get_mono(&t1);
	disp_stat.send_us += ( t1 - t0 ) / 1000;
}

/* The first byte of an output's page */
LOCAL UB *out_page( CONST DISPOUT *o, UINT page )
{
	if ( page == 1 && o->hw.page2_va != 0 ) {
		return (UB *)o->hw.page2_va;
	}
	return (UB *)o->hw.fb_va + (UBINT)page * o->hw.pitch * o->hw.height;
}

/*
 * A rectangle r of the back buffer, inside an output's rectangle of it,
 * sent scaled to a page of the output (fb): every pixel of the part of
 * fit it covers takes the nearest pixel of the rectangle, a row at a time
 * through send_row in pieces of OS_RUN, as the straight send converts
 * them. The bars round fit are left as they were made (black).
 */
#define OS_RUN		256

LOCAL void out_send_scaled( DISPOUT *o, UB *fb, CONST T_DISPRECT *r )
{
	CONST T_DISPRECT *f = &o->fit;
	INT	fw = f->right - f->left, fh = f->bottom - f->top;
	INT	l, t, rr, b, dx, dy, n, k;
	UINT	px = o->hw.bpp / 8;
	UW	run[OS_RUN];
	UD	stepx, stepy;

	if ( fw <= 0 || fh <= 0 || o->sw == 0 || o->sh == 0 ) {
		return;
	}
	/* the part of fit r covers: rounded out, so that nothing it reaches is left */
	l = f->left + (INT)( (UD)( r->left - o->x ) * (UD)fw / o->sw );
	t = f->top + (INT)( (UD)( r->top - o->y ) * (UD)fh / o->sh );
	rr = f->left + (INT)( ( (UD)( r->right - o->x ) * (UD)fw + o->sw - 1 ) / o->sw );
	b = f->top + (INT)( ( (UD)( r->bottom - o->y ) * (UD)fh + o->sh - 1 ) / o->sh );
	if ( rr > f->right ) rr = f->right;
	if ( b > f->bottom ) b = f->bottom;
	stepx = ( (UD)o->sw << 16 ) / (UD)fw;
	stepy = ( (UD)o->sh << 16 ) / (UD)fh;
	for ( dy = t; dy < b; dy++ ) {
		/* the source row of the middle of this one */
		UD		sy = ( (UD)( dy - f->top ) * stepy + ( stepy >> 1 ) ) >> 16;
		CONST UW	*s = (CONST UW *)( disp_back + (UBINT)( o->y + (INT)( ( sy < o->sh ) ? sy : o->sh - 1 ) ) * disp_spec.pitch )
				     + o->x;
		UB		*d = fb + (UBINT)dy * o->hw.pitch;

		for ( dx = l; dx < rr; dx += n ) {
			n = ( rr - dx < OS_RUN ) ? rr - dx : OS_RUN;
			for ( k = 0; k < n; k++ ) {
				UD	sx = ( (UD)( dx + k - f->left ) * stepx + ( stepx >> 1 ) ) >> 16;

				run[k] = s[( sx < o->sw ) ? sx : o->sw - 1];
			}
			send_row((UW *)( d + (UBINT)dx * px ), run, n, o->hw.format, o->hw.alpha);
		}
	}
	o->pixels += (UD)( b - t ) * (UD)( rr - l );
}

/* An output's framebuffer, every page, made black: the bars round a scaled picture */
LOCAL void out_black( DISPOUT *o )
{
	UINT	p, pages = o->flipping ? 2 : 1;

	for ( p = 0; p < pages; p++ ) {
		knl_memset(out_page(o, p), 0, (SZ)( (UBINT)o->hw.pitch * o->hw.height ));
	}
}

/* Each rectangle of a list sent to a page of an output, the part of it the output shows */
LOCAL void out_send( DISPOUT *o, CONST RECTS *l, UINT page )
{
	UB	*fb = out_page(o, page);
	UINT	px = o->hw.bpp / 8;
	BOOL	same = ( o->hw.format == DISP_FMT_XRGB8888 && o->hw.alpha == DISP_ALPHA_ANY );
	INT	i, y;

	for ( i = 0; i < l->n; i++ ) {
		T_DISPRECT	r = l->r[i];

		if ( !clip(&r) || !clip_to(&r, o->x, o->y, o->x + (INT)o->sw, o->y + (INT)o->sh) ) {
			continue;
		}
		if ( o->hw.out != NULL && o->hw.out->invalidate != NULL && page == o->shown ) {
			o->hw.out->invalidate(o->hw.out->ctx, &r);
		}
		if ( o->scaled ) {
			out_send_scaled(o, fb, &r);
			if ( o->hw.out != NULL && o->hw.out->flushed != NULL && page == o->shown ) {
				o->hw.out->flushed(o->hw.out->ctx, &r);
			}
			continue;
		}
		for ( y = r.top; y < r.bottom; y++ ) {
			UB	*d = fb + (UBINT)( y - o->y ) * o->hw.pitch + (UBINT)( r.left - o->x ) * px;
			CONST UB *s = disp_back + (UBINT)y * disp_spec.pitch + (UBINT)r.left * 4;

			if ( same ) {
				knl_memcpy(d, s, (SZ)( r.right - r.left ) * 4);
			} else {
				send_row((UW *)d, (CONST UW *)s, r.right - r.left, o->hw.format, o->hw.alpha);
			}
		}
		o->pixels += (UD)( r.bottom - r.top ) * (UD)( r.right - r.left );
		if ( o->hw.out != NULL && o->hw.out->flushed != NULL && page == o->shown ) {
			o->hw.out->flushed(o->hw.out->ctx, &r);
		}
	}
}

/*
 * What changed on an output, counted with the sends (disp_stat) when it
 * is sent: what a page lacked, sent again to the other page, is counted
 * only in the output's own pixels
 */
LOCAL void out_count( CONST DISPOUT *o )
{
	INT	i;

	for ( i = 0; i < o->pend.n; i++ ) {
		disp_stat.rects++;
		disp_stat.rows += (UD)( o->pend.r[i].bottom - o->pend.r[i].top );
		disp_stat.pixels += (UD)( o->pend.r[i].bottom - o->pend.r[i].top )
				    * (UD)( o->pend.r[i].right - o->pend.r[i].left );
	}
}

/*
 * What waits for an output sent, as far as it can be now: straight to
 * the page shown where it does not flip; with two pages, to the page not
 * shown with what that page lacks, then shown, once the last flip has
 * surely been taken (latch_us after it, or a vertical blank seen since,
 * blanked TRUE). Answers whether something still waits for a flip. The
 * caller holds the lock.
 */
LOCAL BOOL out_step( DISPOUT *o, BOOL blanked )
{
	RECTS	all;
	UINT	to;
	UD	now;
	INT	i;

	if ( o->pend.n == 0 ) {
		return FALSE;
	}
	if ( !o->flipping ) {
		out_count(o);
		out_send(o, &o->pend, o->shown);
		o->pend.n = 0;
		return FALSE;
	}
	(void)ts_get_mono(&now);
	if ( o->due && !blanked && now - o->due_at < (UD)o->hw.latch_us * 1000 ) {
		return TRUE;			/* the page it put away may still be read out */
	}
	o->due = FALSE;
	out_count(o);
	all = o->pend;
	for ( i = 0; i < o->old.n; i++ ) {
		gather(&all, &o->old.r[i]);
	}
	to = 1 - o->shown;
	out_send(o, &all, to);
	DeviceBarrier();
	if ( o->hw.flip(to) < E_OK ) {
		/* not taken: the page shown brought up to date, and sent to from now on */
		out_send(o, &all, o->shown);
		o->flipping = FALSE;
		o->old.n = 0;
	} else {
		(void)ts_get_mono(&o->due_at);
		o->shown = to;
		o->due = TRUE;
		if ( o->hw.out != NULL && o->hw.out->flushed != NULL ) {
			T_DISPRECT	w;

			w.left = o->x;  w.top = o->y;
			w.right = o->x + (INT)o->sw;  w.bottom = o->y + (INT)o->sh;
			o->hw.out->flushed(o->hw.out->ctx, &w);
		}
		o->old = o->pend;
		o->flips++;
	}
	o->pend.n = 0;
	return FALSE;
}

/*
 * Each rectangle of a list for the outputs besides the main one: each
 * gathers what is on it, and is sent what it can be sent now. What waits
 * for an output's flip is left to the output task.
 */
LOCAL void send_outs( CONST RECTS *l )
{
	UD	t0, t1;
	INT	i, k;
	BOOL	later = FALSE;

	if ( disp_nout == 0 ) {
		return;
	}
	for ( k = 0; k < disp_nout; k++ ) {
		DISPOUT	*o = &disp_out[k];

		(void)ts_get_mono(&t0);

		for ( i = 0; i < l->n; i++ ) {
			T_DISPRECT	r = l->r[i];

			if ( clip_to(&r, o->x, o->y, o->x + (INT)o->sw, o->y + (INT)o->sh) ) {
				gather(&o->pend, &r);
			}
		}
		if ( out_step(o, FALSE) ) {
			o->deferred++;
			later = TRUE;
		}
		(void)ts_get_mono(&t1);
		o->send_us += ( t1 - t0 ) / 1000;
	}
	DeviceBarrier();
	if ( later && disp_out_tsk > 0 ) {
		(void)tk_wup_tsk(disp_out_tsk);
	}
}

/*
 * The output task: what waits for an output's flip is sent once that
 * flip is taken. An output that can wait for its vertical blank is
 * waited for there, outside the lock; one that cannot is looked at every
 * millisecond until latch_us has passed since its flip.
 */
LOCAL void out_task( INT stacd, void *exinf )
{
	INT	k;
	BOOL	waiting;
	ER	(*vsync)( void );

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		(void)tk_slp_tsk(TMO_FEVR);
		do {
			waiting = FALSE;
			tk_loc_mtx(disp_mtxid, TMO_FEVR);
			for ( k = 0; k < disp_nout; k++ ) {
				DISPOUT	*o = &disp_out[k];
				BOOL	blanked = FALSE;

				if ( o->pend.n == 0 ) continue;
				vsync = o->hw.vsync;
				if ( o->due && vsync != NULL ) {
					tk_unl_mtx(disp_mtxid);
					blanked = (BOOL)( vsync() >= E_OK );
					tk_loc_mtx(disp_mtxid, TMO_FEVR);
					if ( k >= disp_nout ) break;	/* taken away meanwhile */
				}
				if ( out_step(&disp_out[k], blanked) ) {
					waiting = TRUE;
				}
			}
			DeviceBarrier();
			tk_unl_mtx(disp_mtxid);
			if ( waiting ) {
				tk_dly_tsk(1);
			}
		} while ( waiting );
	}
}

/* The output task started, the first time an output that flips is put */
LOCAL void out_task_start( void )
{
	T_CTSK	ctsk;

	if ( disp_out_tsk > 0 ) {
		return;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.exinf = NULL;
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)out_task;
	ctsk.itskpri = 10;
	ctsk.stksz = 4096;
	disp_out_tsk = tk_cre_tsk(&ctsk);
	if ( disp_out_tsk > 0 && tk_sta_tsk(disp_out_tsk, 0) < E_OK ) {
		(void)tk_del_tsk(disp_out_tsk);
		disp_out_tsk = 0;
	}
	if ( disp_out_tsk <= 0 ) {
		disp_out_tsk = 0;
		tm_printf((UB *)"disp: no task for the outputs' flips: they are sent to the page shown\n");
	}
}

/* So many microseconds, asleep where the caller is a task */
LOCAL void pause_us( UD us )
{
	UD	t0, now;

	if ( tk_dly_tsk_u((RELTIM_U)us) == E_OK ) {
		return;
	}
	(void)ts_get_mono(&t0);
	do {
		(void)ts_get_mono(&now);
	} while ( now - t0 < us * 1000 );
}

/*
 * Where the clock of the blanks puts the end of the first blank that
 * begins after 't' (ns), a flip made within DISP_GUARD_US before a blank
 * counted as missing it; FALSE when the screen keeps no clock, or its
 * last blank was seen too long ago to go by.
 */
LOCAL BOOL blank_after( UD t, UD now, UD *p_end )
{
	UD	f = disp_hw.frame_ns, seen, b, k;

	if ( f == 0 || disp_hw.blank_seen == NULL ) {
		return FALSE;
	}
	seen = disp_hw.blank_seen();
	if ( seen == 0 || now < seen || now - seen > DISP_SEEN_NS ) {
		return FALSE;
	}
	b = t + disp_hw.blank_ns + DISP_GUARD_US * 1000;	/* a blank ending after this began after t */
	if ( b >= seen ) {
		k = ( b - seen ) / f + 1;
		*p_end = seen + k * f;
	} else {
		k = ( seen - b - 1 ) / f;
		*p_end = seen - k * f;
	}
	return TRUE;
}

/* A flip made at t counted: how far from the one before, and where in the refresh */
LOCAL void flip_note( UD t )
{
	UD	f = ( disp_hw.frame_ns != 0 ) ? disp_hw.frame_ns : DISP_FRAME_NS;
	UD	n, seen;

	disp_fr.flips++;
	if ( disp_flip_at != 0 && t >= disp_flip_at ) {
		n = ( t - disp_flip_at + f / 2 ) / f;
		disp_fr.apart[( n > 4 ) ? 4 : n]++;
	}
	disp_flip_at = t;
	if ( disp_hw.frame_ns != 0 && disp_hw.blank_seen != NULL ) {
		seen = disp_hw.blank_seen();
		if ( seen != 0 && t >= seen && t - seen <= DISP_SEEN_NS ) {
			disp_fr.phase[( ( t - seen ) % f ) * 4 / f]++;
		}
	}
}

/*
 * Until the last flip has surely been taken, so that the page it put
 * away is no longer being read out and may be written. The caller
 * holds the lock.
 */
LOCAL void settle( void )
{
	UD	now, t0, t1, since, end = 0, wake, seen;
	BOOL	clock = FALSE;
	ER	er;

	if ( !disp_due ) {
		return;
	}
	disp_due = FALSE;
	(void)ts_get_mono(&now);
	disp_fr.due++;
	since = ( now - disp_due_at ) / 1000;
	if ( since >= disp_hw.latch_us ) {
		disp_fr.taken++;
		return;				/* a refresh has passed since */
	}
	if ( disp_hw.vsync != NULL ) {
		/*
		 * By the clock: the blank that takes the flip already over,
		 * or far enough off to sleep until shortly before it. Watching
		 * the screen after that blank would wait for the next one.
		 */
		clock = blank_after(disp_due_at, now, &end);
		if ( clock && now >= end + (UD)DISP_BLANK_SLOP_US * 1000 ) {
			disp_fr.taken++;
			return;
		}
		if ( clock ) {
			wake = end - disp_hw.blank_ns - (UD)DISP_WAKE_US * 1000;
			if ( end > disp_hw.blank_ns + (UD)DISP_WAKE_US * 1000 && wake > now + 1000000 ) {
				pause_us(( wake - now ) / 1000);
				(void)ts_get_mono(&t1);
				disp_fr.sleep_us += ( t1 - now ) / 1000;
			}
		}
		(void)ts_get_mono(&t0);
		er = disp_hw.vsync();
		(void)ts_get_mono(&t1);
		disp_fr.waited++;
		disp_fr.spin_us += ( t1 - t0 ) / 1000;
		if ( er >= E_OK && clock && disp_hw.blank_seen != NULL ) {
			seen = disp_hw.blank_seen();
			if ( seen + (UD)DISP_BLANK_SLOP_US * 1000 < end
			  || seen > end + (UD)DISP_BLANK_SLOP_US * 1000 ) {
				disp_fr.off++;
			}
		}
		if ( er < E_OK ) {
			/* a blank missed now and then is waited for by the time; three in a row, for good */
			if ( ++disp_vs_miss >= 3 ) {
				disp_hw.vsync = NULL;
				tm_printf((UB *)"disp: flip: the wait for the vertical blank failed %d times in a row; "
					  "waiting %d us after each flip from now on\n", disp_vs_miss, (INT)disp_hw.latch_us);
			}
			(void)ts_get_mono(&t1);
			since = ( t1 - disp_due_at ) / 1000;
			if ( since < disp_hw.latch_us ) pause_us(disp_hw.latch_us - since);
		} else {
			disp_vs_miss = 0;
		}
	} else {
		pause_us(disp_hw.latch_us - since);
	}
	(void)ts_get_mono(&t1);
	disp_stat.waits++;
	disp_stat.wait_us += ( t1 - now ) / 1000;
}

/*
 * Send whatever is outstanding, each rectangle by itself: to the page
 * shown, or with two pages to the other with what it lacks besides,
 * which is then shown. The caller holds the lock.
 */
LOCAL void flush_locked( void )
{
	RECTS	all;
	UINT	to;
	UD	t0, t1;
	INT	i;

	if ( disp_new.n == 0 ) {
		return;
	}
	if ( !disp_flipping ) {
		send_list(&disp_new, disp_shown);
		main_told(&disp_new);
		send_outs(&disp_new);
		disp_new.n = 0;
		disp_stat.sends++;
		return;
	}

	settle();
	all = disp_new;
	for ( i = 0; i < disp_old.n; i++ ) {
		gather(&all, &disp_old.r[i]);
	}
	to = 1 - disp_shown;
	send_list(&all, to);
	(void)ts_get_mono(&t0);
	if ( disp_hw.flip(to) < E_OK ) {
		/* not taken: the page shown brought up to date, and sent to from now on */
		send_list(&all, disp_shown);
		disp_flipping = FALSE;
		disp_old.n = 0;
		tm_printf((UB *)"disp: flip: the screen did not take page %d; "
			  "sending to the page it shows from now on\n", (INT)to);
	} else {
		(void)ts_get_mono(&t1);
		disp_flip_us = ( t1 - t0 ) / 1000;
		disp_shown = to;
		disp_due = TRUE;
		disp_due_at = t1;
		disp_old = disp_new;
		disp_stat.flips++;
		flip_note(t1);
	}
	main_told(&all);
	send_outs(&disp_new);
	disp_new.n = 0;
	disp_stat.sends++;
}

/* ---------------------------------------------------------------- interface */

EXPORT ER ts_disp_ref( T_DISPSPEC *spec )
{
	if ( !disp_found || spec == NULL ) {
		return E_NOEXS;
	}
	*spec = disp_spec;
	spec->held = (UINT)disp_held;
	spec->fb_shown = disp_shown;
	spec->flipping = disp_flipping;

	return E_OK;
}

LOCAL INT	disp_nscreens = 1;

EXPORT void knl_disp_screens( INT n )
{
	if ( n >= 1 && n <= DISP_SCREENS_MAX ) disp_nscreens = n;
}

/* The outputs besides the main one that are screens of their own: those that do not mirror it */
LOCAL INT ext_count( void )
{
	INT	k, n = 0;

	for ( k = 0; k < disp_nout; k++ ) {
		if ( disp_out[k].side != DISP_SIDE_MIRROR ) n++;
	}
	return n;
}

/* The output (of disp_out) that shows screen s (1 or more), or -1 */
LOCAL INT screen_out( INT s )
{
	INT	k, n = 0;

	for ( k = 0; k < disp_nout; k++ ) {
		if ( disp_out[k].side != DISP_SIDE_MIRROR && ++n == s ) return k;
	}
	return -1;
}

EXPORT INT ts_disp_screens( void )
{
	INT	n = ext_count();

	return ( n > 0 ) ? 1 + n : disp_nscreens;
}

EXPORT INT knl_disp_out_count( void )
{
	return disp_nout;
}

EXPORT INT knl_disp_out_screen( INT out )
{
	INT	k, n = 0;

	if ( out == 0 ) return 0;
	if ( out < 0 || out > disp_nout || disp_out[out - 1].side == DISP_SIDE_MIRROR ) return -1;
	for ( k = 0; k < out; k++ ) {
		if ( disp_out[k].side != DISP_SIDE_MIRROR ) n++;
	}
	return n;
}

/* A place or a length in pixels of the back buffer as view pixels, rounded up as the view is */
LOCAL INT view_at( INT px )
{
	UINT	sc = ( disp_spec.scale >= 100 ) ? disp_spec.scale : 100;

	return ( px <= 0 ) ? 0 : (INT)disp_view_len((UINT)px, sc);
}

EXPORT ER ts_disp_screen( INT i, INT *p_x, INT *p_y, INT *p_w, INT *p_h )
{
	INT	vw, vh;

	if ( disp_found && ext_count() > 0 ) {
		/* each output a screen of its own size and place; one that mirrors the main screen is none */
		INT	px = disp_spec.main_x, py = disp_spec.main_y;
		INT	pw = (INT)disp_hw.width, ph = (INT)disp_hw.height;

		if ( i < 0 || i > ext_count() ) return E_NOEXS;
		if ( i > 0 ) {
			CONST DISPOUT	*o = &disp_out[screen_out(i)];

			px = o->x;
			py = o->y;
			pw = (INT)o->hw.width;
			ph = (INT)o->hw.height;
		}
		*p_x = view_at(px);
		*p_y = view_at(py);
		*p_w = view_at(px + pw) - *p_x;
		*p_h = view_at(py + ph) - *p_y;
		return E_OK;
	}
	if ( !disp_found || i < 0 || i >= disp_nscreens ) return E_NOEXS;
	vw = (INT)( ( disp_spec.view_w > 0 ) ? disp_spec.view_w : disp_spec.width );
	vh = (INT)( ( disp_spec.view_h > 0 ) ? disp_spec.view_h : disp_spec.height );
	*p_x = vw * i / disp_nscreens;
	*p_y = 0;
	*p_w = vw * ( i + 1 ) / disp_nscreens - *p_x;
	*p_h = vh;
	return E_OK;
}

/*
 * The screens as they are now kept as those before a change (disp_was):
 * with one output, the whole view as the main screen. The caller holds
 * the lock, before it changes anything.
 */
LOCAL void was_take( void )
{
	INT	i, x, y, w, h;

	disp_nwas = 0;
	if ( ext_count() == 0 ) {
		disp_was[0].id = 0;
		disp_was[0].x = disp_was[0].y = 0;
		disp_was[0].w = (INT)( ( disp_spec.view_w > 0 ) ? disp_spec.view_w : disp_spec.width );
		disp_was[0].h = (INT)( ( disp_spec.view_h > 0 ) ? disp_spec.view_h : disp_spec.height );
		disp_nwas = 1;
		return;
	}
	for ( i = 0; i <= ext_count() && i < DISP_SCREENS_MAX; i++ ) {
		if ( ts_disp_screen(i, &x, &y, &w, &h) < E_OK ) break;
		disp_was[i].id = ( i == 0 ) ? 0 : disp_out[screen_out(i)].id;
		disp_was[i].x = x;
		disp_was[i].y = y;
		disp_was[i].w = w;
		disp_was[i].h = h;
		disp_nwas = i + 1;
	}
}

/* Which screen now shows the output of an id (0: the main one), or -1: none, or it mirrors the main screen */
LOCAL INT screen_of_id( INT id )
{
	INT	k;

	if ( id == 0 ) return 0;
	for ( k = 0; k < disp_nout; k++ ) {
		if ( disp_out[k].id == id ) return knl_disp_out_screen(k + 1);
	}
	return -1;
}

EXPORT BOOL knl_disp_follow( INT *p_x, INT *p_y, T_DISPRECT *on )
{
	INT	i, best = 0, s, x, y, w, h, px, py, dx, dy;
	D	d, bd = -1;
	BOOL	to_main = FALSE;

	if ( !disp_found || p_x == NULL || p_y == NULL ) {
		return FALSE;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	px = *p_x;
	py = *p_y;
	/* the screen it was on, or the nearest */
	for ( i = 0; i < disp_nwas; i++ ) {
		CONST DISPWAS	*v = &disp_was[i];

		dx = ( px < v->x ) ? v->x - px : ( px >= v->x + v->w ) ? px - ( v->x + v->w - 1 ) : 0;
		dy = ( py < v->y ) ? v->y - py : ( py >= v->y + v->h ) ? py - ( v->y + v->h - 1 ) : 0;
		d = (D)dx * dx + (D)dy * dy;
		if ( bd < 0 || d < bd ) {
			bd = d;
			best = i;
			if ( d == 0 ) break;
		}
	}
	s = ( disp_nwas > 0 ) ? screen_of_id(disp_was[best].id) : 0;
	if ( s < 0 ) {
		s = 0;
		to_main = TRUE;
	}
	if ( ts_disp_screen(s, &x, &y, &w, &h) >= E_OK ) {
		if ( disp_nwas > 0 ) {
			/* as far from the corner of the screen it goes with as it was from its own */
			px = x + ( px - disp_was[best].x );
			py = y + ( py - disp_was[best].y );
		}
		if ( to_main ) {
			if ( px >= x + w ) px = x + w - 1;
			if ( py >= y + h ) py = y + h - 1;
			if ( px < x ) px = x;
			if ( py < y ) py = y;
		}
		if ( on != NULL ) {
			on->left = x;
			on->top = y;
			on->right = x + w;
			on->bottom = y + h;
		}
	}
	tk_unl_mtx(disp_mtxid);
	*p_x = px;
	*p_y = py;
	return to_main;
}

EXPORT void *ts_disp_buffer( void )
{
	return ( disp_found ) ? disp_back : NULL;
}

EXPORT ER ts_disp_damage( CONST T_DISPRECT *r )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	if ( r == NULL || r->left >= r->right || r->top >= r->bottom ) {
		return E_PAR;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	gather(&disp_new, r);
	if ( disp_held == 0 ) {
		flush_locked();
	}
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

EXPORT ER ts_disp_hold( void )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( disp_held == 0 ) {
		disp_hold_tsk = tk_get_tid();
		(void)ts_get_mono(&disp_hold_at);
	}
	disp_held++;
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

EXPORT ER ts_disp_release( void )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( disp_held > 0 ) {
		disp_held--;
	}
	if ( disp_held == 0 ) {
		flush_locked();
	}
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

/*
 * How many holds there are now, which task took the first of them and
 * how many milliseconds ago: for the reports of a screen that is not
 * brought up to date (the desktop's watch at its start). Read without
 * the lock, as it is.
 */
EXPORT INT knl_disp_hold_note( ID *p_tsk, UW *p_ms )
{
	INT	held = disp_held;
	UD	now, at = disp_hold_at;

	(void)ts_get_mono(&now);
	if ( p_tsk != NULL ) *p_tsk = ( held > 0 ) ? disp_hold_tsk : 0;
	if ( p_ms != NULL ) *p_ms = ( held > 0 && now > at ) ? (UW)( ( now - at ) / 1000000 ) : 0;
	return held;
}

EXPORT ER ts_disp_flush( void )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	flush_locked();
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

EXPORT ER ts_disp_clear( UW colour )
{
	T_DISPRECT	r;
	UW		*p;
	UBINT		n, i;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);

	if ( disp_spec.bpp == 32 ) {
		p = (UW *)disp_back;
		n = (UBINT)disp_spec.pitch / 4 * disp_spec.height;
		for ( i = 0; i < n; i++ ) {
			p[i] = colour;
		}
	} else {
		UH	*q = (UH *)disp_back;
		UH	v = (UH)colour;

		n = (UBINT)disp_spec.pitch / 2 * disp_spec.height;
		for ( i = 0; i < n; i++ ) {
			q[i] = v;
		}
	}
	tk_unl_mtx(disp_mtxid);

	r.left = 0;
	r.top = 0;
	r.right = (INT)disp_spec.width;
	r.bottom = (INT)disp_spec.height;

	return ts_disp_damage(&r);
}

EXPORT ER ts_disp_stat( T_DISPSTAT *st )
{
	if ( !disp_found || st == NULL ) {
		return E_NOEXS;
	}
	*st = disp_stat;

	return E_OK;
}

EXPORT ER ts_disp_frames( T_DISPFRAME *st )
{
	if ( !disp_found || st == NULL ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	*st = disp_fr;
	st->frame_ns = disp_hw.frame_ns;
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

EXPORT UINT ts_disp_paced( void )
{
	if ( !disp_found || !disp_flipping ) {
		return 0;
	}
	if ( disp_hw.vsync != NULL ) {
		return ( disp_hw.frame_ns != 0 ) ? (UINT)( disp_hw.frame_ns / 1000 ) : (UINT)( DISP_FRAME_NS / 1000 );
	}
	return disp_hw.latch_us;
}

/* ---------------------------------------------------------------- a refresh made up */

LOCAL BOOL	sim_on = FALSE;
LOCAL UD	sim_origin, sim_period;
LOCAL T_DISPHW	sim_saved;

/* The end of the last blank made up, ns */
LOCAL UD sim_seen( void )
{
	UD	now;

	(void)ts_get_mono(&now);
	if ( !sim_on || now < sim_origin ) {
		return 0;
	}
	return sim_origin + ( now - sim_origin ) / sim_period * sim_period;
}

/*
 * Until the end of the first blank that begins after the last flip, a
 * flip made within DISP_GUARD_US before a blank missing it, as the
 * clock reckons on a screen that has one. Called by settle, the lock
 * held.
 */
LOCAL ER sim_vsync( void )
{
	UD	b = disp_due_at + sim_period / 20 + (UD)DISP_GUARD_US * 1000;
	UD	end, now;

	end = ( b >= sim_origin ) ? sim_origin + ( ( b - sim_origin ) / sim_period + 1 ) * sim_period : sim_origin;
	(void)ts_get_mono(&now);
	if ( end > now ) {
		(void)tk_dly_tsk_u((RELTIM_U)( ( end - now + 999 ) / 1000 ));
	}
	do {
		(void)ts_get_mono(&now);
	} while ( now < end );
	return E_OK;
}

EXPORT ER knl_disp_sim_blank( UINT period_us )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( period_us != 0 ) {
		if ( !disp_flipping ) {
			tk_unl_mtx(disp_mtxid);
			return E_OBJ;
		}
		if ( !sim_on ) {
			sim_saved = disp_hw;
		}
		settle();
		(void)ts_get_mono(&sim_origin);
		sim_period = (UD)period_us * 1000;
		disp_hw.vsync = sim_vsync;
		disp_hw.latch_us = period_us + period_us / 8;
		disp_hw.frame_ns = sim_period;
		disp_hw.blank_ns = sim_period / 20;
		disp_hw.blank_seen = sim_seen;
		sim_on = TRUE;
	} else if ( sim_on ) {
		settle();
		disp_hw.vsync = sim_saved.vsync;
		disp_hw.latch_us = sim_saved.latch_us;
		disp_hw.frame_ns = sim_saved.frame_ns;
		disp_hw.blank_ns = sim_saved.blank_ns;
		disp_hw.blank_seen = sim_saved.blank_seen;
		sim_on = FALSE;
	}
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

/*
 * Flipping turned on: what is outstanding goes to the page shown first,
 * and the other page, which holds nothing of the screen, is to be sent
 * the whole of it by the next send. Turned off: what is outstanding is
 * sent and shown, and sends go to that page from then on.
 */
EXPORT ER ts_disp_flip( BOOL on )
{
	if ( !disp_found ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( on && !disp_flipping ) {
		if ( disp_spec.fb_pages < 2 ) {
			tk_unl_mtx(disp_mtxid);
			return E_NOSPT;
		}
		flush_locked();
		whole_of(&disp_old);
		disp_flipping = TRUE;
	} else if ( !on && disp_flipping ) {
		flush_locked();
		settle();
		disp_flipping = FALSE;
		disp_old.n = 0;
	}
	tk_unl_mtx(disp_mtxid);

	return E_OK;
}

/*
 * The screen set to another size (システム環境設定, at the desktop's
 * start or while it runs): the hardware first, then a back buffer of the
 * new size, then whoever keeps the size told. The window manager's lock
 * is held from before the old back buffer goes until the windows are
 * laid again for the new one (knl_wm_screen_changing, _changed, _kept).
 * The new size shows its first page; with two pages, the other is sent
 * the whole screen by the next send.
 */
IMPORT void knl_wm_screen_changing( void ) __attribute__((weak));
IMPORT void knl_wm_screen_kept( void ) __attribute__((weak));
IMPORT void knl_wm_screen_changed( void ) __attribute__((weak));
IMPORT void knl_hid_screen_changed( void ) __attribute__((weak));
IMPORT void knl_disp_out_main_changed( void ) __attribute__((weak));	/* dispout.c: outputs that mirror the main screen made again */

/* The rectangle a picture is shown in, its ratio kept (disp_hw.h) */
EXPORT void knl_disp_fit( UINT src_w, UINT src_h, UINT dst_w, UINT dst_h, T_DISPRECT *r )
{
	UINT	w = dst_w, h = dst_h;

	if ( src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0 ) {
		r->left = r->top = 0;
		r->right = (INT)dst_w;
		r->bottom = (INT)dst_h;
		return;
	}
	/* the side that fills first decides the scale: the other is made as long as the ratio says */
	if ( (UD)src_w * dst_h > (UD)src_h * dst_w ) {
		h = (UINT)( ( (UD)src_h * dst_w + src_w / 2 ) / src_w );	/* wider: bars above and below */
	} else if ( (UD)src_w * dst_h < (UD)src_h * dst_w ) {
		w = (UINT)( ( (UD)src_w * dst_h + src_h / 2 ) / src_h );	/* taller: bars left and right */
	}
	if ( w == 0 ) w = 1;
	if ( h == 0 ) h = 1;
	r->left = (INT)( ( dst_w - w ) / 2 );
	r->top = (INT)( ( dst_h - h ) / 2 );
	r->right = r->left + (INT)w;
	r->bottom = r->top + (INT)h;
}

/*
 * Where each output besides the main one goes, from the main screen's
 * size and the side each was put at, in order: past the right edge, the
 * bottom, the left edge or the top of the rectangle round the screens
 * before it, along that edge from the main screen's corner. The places
 * are worked out with the main screen at 0, 0, then all of them moved so
 * that the rectangle round them starts at 0, 0, which the back buffer
 * and the view are: its size into *p_w, *p_h, and where the main screen
 * is in it into *p_mx, *p_my (0, 0 unless a screen is left of it or
 * above it). One that mirrors the main screen takes no room of its own:
 * its rectangle is the main screen's.
 */
LOCAL void outs_place( UINT mw, UINT mh, UINT *p_w, UINT *p_h, INT *p_mx, INT *p_my )
{
	INT	l = 0, t = 0, r = (INT)mw, b = (INT)mh;
	INT	k;

	for ( k = 0; k < disp_nout; k++ ) {
		DISPOUT	*o = &disp_out[k];
		INT	ow = (INT)o->hw.width, oh = (INT)o->hw.height;

		o->sw = o->hw.width;
		o->sh = o->hw.height;
		o->scaled = FALSE;
		if ( o->side == DISP_SIDE_MIRROR ) {
			/* the main screen's rectangle, scaled into its framebuffer where the sizes differ */
			o->x = 0;
			o->y = 0;
			o->sw = mw;
			o->sh = mh;
			o->scaled = (BOOL)( mw != o->hw.width || mh != o->hw.height );
			knl_disp_fit(mw, mh, o->hw.width, o->hw.height, &o->fit);
			continue;
		}
		switch ( o->side ) {
		case DISP_SIDE_BELOW:
			o->x = o->offset;
			o->y = b;
			break;
		case DISP_SIDE_LEFT:
			o->x = l - ow;
			o->y = o->offset;
			break;
		case DISP_SIDE_ABOVE:
			o->x = o->offset;
			o->y = t - oh;
			break;
		default:
			o->x = r;
			o->y = o->offset;
			break;
		}
		if ( o->x < l ) l = o->x;
		if ( o->y < t ) t = o->y;
		if ( o->x + ow > r ) r = o->x + ow;
		if ( o->y + oh > b ) b = o->y + oh;
	}
	for ( k = 0; k < disp_nout; k++ ) {
		disp_out[k].x -= l;		/* one that mirrors comes to the main screen's corner */
		disp_out[k].y -= t;
	}
	*p_w = (UINT)( r - l );
	*p_h = (UINT)( b - t );
	*p_mx = -l;
	*p_my = -t;
}

/*
 * The row stride of a back buffer w pixels wide whose corner is the
 * main screen m: the screen's own where there is one screen of four
 * byte pixels, so that a row is sent as one straight copy
 */
LOCAL UINT back_pitch( CONST T_DISPHW *m, UINT w )
{
	if ( disp_nout == 0 ) {
		return ( m->format == DISP_FMT_XRGB8888 ) ? m->pitch : m->width * 4;
	}
	return ( w * 4 + 63 ) & ~63U;
}

/* Whether the framebuffer described can be sent to */
LOCAL BOOL sendable( CONST T_DISPHW *hw )
{
	return ( ( hw->bpp == 32 && ( hw->format == DISP_FMT_XRGB8888 || hw->format == DISP_FMT_XBGR8888 ) )
		 || ( hw->bpp == 16 && hw->format == DISP_FMT_RGB565 ) )
		&& hw->width != 0 && hw->height != 0 && hw->pitch >= hw->width * ( hw->bpp / 8 );
}

/*
 * A new back buffer round the main screen m and the outputs besides it,
 * in place of the one there was: E_NOMEM keeps the one there was (the
 * outputs are placed again round the main screen there was). With
 * new_mode the main screen has just been set to its size and shows its
 * first page; otherwise it goes on showing the page it shows, and with
 * two pages the other is sent the whole screen by the next send, as the
 * page shown is. The caller holds the lock and has settled the last flip.
 */
LOCAL ER lay_locked( CONST T_DISPHW *m, BOOL new_mode )
{
	T_DISPHW	main = *m;
	UINT		w, h, pitch;
	INT		mx, my;
	UBINT		pages, was_pages;
	UB		*back;

	outs_place(main.width, main.height, &w, &h, &mx, &my);
	pitch = back_pitch(&main, w);
	pages = ( (UBINT)pitch * h + PAGE_SIZE - 1 ) / PAGE_SIZE;
	back = (UB *)knl_vmap(pages, 0);
	if ( back == NULL ) {
		outs_place(disp_hw.width, disp_hw.height, &w, &h, &mx, &my);
		return E_NOMEM;
	}
	knl_memset(back, 0, (SZ)( (UBINT)pitch * h ));
	if ( disp_back != NULL ) {
		was_pages = ( (UBINT)disp_spec.pitch * disp_spec.height + PAGE_SIZE - 1 ) / PAGE_SIZE;
		knl_vunmap(disp_back, was_pages);
	}
	disp_back = back;
	disp_hw = main;
	disp_spec.width = w;
	disp_spec.height = h;
	disp_spec.pitch = pitch;
	disp_spec.main_w = main.width;
	disp_spec.main_h = main.height;
	disp_spec.main_x = mx;
	disp_spec.main_y = my;
	disp_spec.fb_pa = main.fb_pa;
	disp_spec.fb_pitch = main.pitch;
	disp_spec.fb_bpp = main.bpp;
	disp_spec.fb_format = main.format;
	disp_spec.fb_pages = ( main.pages >= 2 && main.flip != NULL ) ? 2 : 1;
	view_set();
	disp_fb = (UB *)main.fb_va;
	disp_fb2 = (UB *)main.page2_va;
	disp_new.n = 0;			/* of the old screen; the new one is laid whole */
	disp_old.n = 0;
	if ( new_mode ) {
		disp_shown = 0;
		disp_due = FALSE;
	}
	if ( disp_flipping && disp_spec.fb_pages < 2 ) {
		disp_flipping = FALSE;
	}
	if ( disp_flipping ) {
		whole_of(&disp_old);
	}
	/* the outputs' pages hold nothing of the new back buffer: the next sends bring both up to date */
	{
		INT	k;

		for ( k = 0; k < disp_nout; k++ ) {
			DISPOUT	*o = &disp_out[k];

			o->pend.n = 0;
			o->old.n = 0;
			if ( o->scaled ) {
				out_black(o);		/* the bars round the picture */
			}
			if ( o->flipping ) {
				o->old.r[0].left = o->x;
				o->old.r[0].top = o->y;
				o->old.r[0].right = o->x + (INT)o->sw;
				o->old.r[0].bottom = o->y + (INT)o->sh;
				o->old.n = 1;
			}
		}
	}
	return E_OK;
}

EXPORT ER ts_disp_setmode( UINT w, UINT h )
{
	T_DISPHW	hw, was;
	ER		er;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	if ( w == disp_hw.width && h == disp_hw.height ) {
		return E_OK;
	}
	/* nothing builds the screen in the back buffer while it is replaced */
	if ( knl_wm_screen_changing != NULL ) knl_wm_screen_changing();
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	settle();
	taken();
	was_take();
	hw = disp_hw;
	was = disp_hw;
	if ( knl_disp_hw_setmode(&hw, w, h) <= 0 ) {
		tk_unl_mtx(disp_mtxid);
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return E_NOSPT;
	}
	er = lay_locked(&hw, TRUE);
	if ( er < E_OK ) {
		hw = was;
		(void)knl_disp_hw_setmode(&hw, was.width, was.height);
		tk_unl_mtx(disp_mtxid);
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return er;
	}
	tk_unl_mtx(disp_mtxid);
	tm_printf((UB *)"TessronOS: display now %d x %d%s\n", (INT)hw.width, (INT)hw.height,
		  disp_flipping ? ", two pages" : "");
	if ( knl_wm_screen_changed != NULL ) knl_wm_screen_changed();
	if ( knl_hid_screen_changed != NULL ) knl_hid_screen_changed();
	/* an output that mirrors the main screen: its mode chosen again for the new size, and put on again */
	if ( knl_disp_out_main_changed != NULL ) knl_disp_out_main_changed();
	return E_OK;
}

/* An output besides the main one put in the list, or why it cannot be */
LOCAL ER out_put( CONST T_DISPHW *hw, INT side, INT offset )
{
	DISPOUT	*o;

	if ( hw == NULL || !sendable(hw) || hw->fb_va == 0
	  || ( side != DISP_SIDE_RIGHT && side != DISP_SIDE_BELOW
	       && side != DISP_SIDE_LEFT && side != DISP_SIDE_ABOVE && side != DISP_SIDE_MIRROR ) ) {
		return E_PAR;
	}
	if ( offset < 0 ) {
		return E_NOSPT;			/* before the main screen's corner along the edge: not taken */
	}
	if ( disp_nout >= DISP_OUTS ) {
		return E_LIMIT;
	}
	o = &disp_out[disp_nout++];
	knl_memset(o, 0, sizeof(*o));
	o->hw = *hw;
	o->id = disp_next_id++;
	if ( disp_next_id <= 0 ) disp_next_id = 1;
	o->side = side;
	o->offset = offset;
	o->flipping = (BOOL)( hw->pages >= 2 && hw->flip != NULL );
	if ( o->flipping ) {
		out_task_start();
		if ( disp_out_tsk == 0 ) o->flipping = FALSE;
	}
	return E_OK;
}

/* What a screen besides the main one shows, for the console */
LOCAL void out_tell( INT screen, CONST char *what )
{
	CONST DISPOUT	*o = &disp_out[screen - 1];

	if ( o->side == DISP_SIDE_MIRROR ) {
		tm_printf((UB *)"disp: output %d %s: %d x %d, %d bpp, the main screen's %d x %d at %d, %d%s, via %s%s\n",
			  screen, what, (INT)o->hw.width, (INT)o->hw.height, (INT)o->hw.bpp, (INT)o->sw, (INT)o->sh,
			  o->x, o->y, o->scaled ? " scaled in the send" : "", ( o->hw.via != NULL ) ? o->hw.via : "?",
			  o->flipping ? ", two pages flipped at its own refresh" : "");
		return;
	}
	tm_printf((UB *)"disp: screen %d %s: %d x %d, %d bpp, at %d, %d of the back buffer (%d x %d), via %s%s\n",
		  screen, what, (INT)o->hw.width, (INT)o->hw.height, (INT)o->hw.bpp, o->x, o->y,
		  (INT)disp_spec.width, (INT)disp_spec.height, ( o->hw.via != NULL ) ? o->hw.via : "?",
		  o->flipping ? ", two pages flipped at its own refresh" : "");
}

EXPORT ER knl_disp_screen_hw( INT screen, T_DISPHW *hw, INT *p_side, INT *p_offset )
{
	if ( !disp_found || hw == NULL ) {
		return E_NOEXS;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( screen < 0 || screen > disp_nout ) {
		tk_unl_mtx(disp_mtxid);
		return E_NOEXS;
	}
	*hw = ( screen == 0 ) ? disp_hw : disp_out[screen - 1].hw;
	if ( p_side != NULL ) *p_side = ( screen == 0 ) ? DISP_SIDE_RIGHT : disp_out[screen - 1].side;
	if ( p_offset != NULL ) *p_offset = ( screen == 0 ) ? 0 : disp_out[screen - 1].offset;
	tk_unl_mtx(disp_mtxid);
	return E_OK;
}

EXPORT INT knl_disp_out_attach( CONST T_DISPHW *hw, INT side, INT offset )
{
	ER	er;
	INT	screen;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	if ( knl_wm_screen_changing != NULL ) knl_wm_screen_changing();
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	settle();
	was_take();
	er = out_put(hw, side, offset);
	if ( er >= E_OK ) {
		er = lay_locked(&disp_hw, FALSE);
		if ( er < E_OK ) {
			disp_nout--;
		}
	}
	if ( er < E_OK ) {
		tk_unl_mtx(disp_mtxid);
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return er;
	}
	screen = disp_nout;
	out_tell(screen, "added");
	tk_unl_mtx(disp_mtxid);
	if ( knl_wm_screen_changed != NULL ) knl_wm_screen_changed();
	if ( knl_hid_screen_changed != NULL ) knl_hid_screen_changed();
	return screen;
}

EXPORT ER knl_disp_out_detach( INT screen )
{
	DISPOUT	gone;
	INT	k;
	ER	er;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	if ( knl_wm_screen_changing != NULL ) knl_wm_screen_changing();
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	if ( screen < 1 || screen > disp_nout ) {
		tk_unl_mtx(disp_mtxid);
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return E_NOEXS;
	}
	settle();
	was_take();
	gone = disp_out[screen - 1];
	for ( k = screen - 1; k + 1 < disp_nout; k++ ) {
		disp_out[k] = disp_out[k + 1];
	}
	disp_nout--;
	er = lay_locked(&disp_hw, FALSE);
	if ( er < E_OK ) {
		/* no room for the smaller back buffer: the output stays */
		for ( k = disp_nout; k > screen - 1; k-- ) {
			disp_out[k] = disp_out[k - 1];
		}
		disp_out[screen - 1] = gone;
		disp_nout++;
		{
			UINT	w, h;
			INT	mx, my;

			outs_place(disp_hw.width, disp_hw.height, &w, &h, &mx, &my);
		}
		tk_unl_mtx(disp_mtxid);
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return er;
	}
	tm_printf((UB *)"disp: screen %d taken away: the back buffer is %d x %d\n",
		  screen, (INT)disp_spec.width, (INT)disp_spec.height);
	tk_unl_mtx(disp_mtxid);
	if ( knl_wm_screen_changed != NULL ) knl_wm_screen_changed();
	if ( knl_hid_screen_changed != NULL ) knl_hid_screen_changed();
	return E_OK;
}

/*
 * The screen drawn at another scale: nothing of the hardware or the
 * back buffer changes, only the view the windows are laid in, so the
 * window manager lays every window again at the new scale as it does
 * for a new size, and the pointer is kept within the new view.
 */
EXPORT ER ts_disp_setscale( UINT scale )
{
	INT	i;
	UINT	was;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	for ( i = 0; i < DISP_NSCALE && disp_scales[i] != scale; i++ ) ;
	if ( i == DISP_NSCALE ) {
		return E_PAR;
	}
	if ( scale_for(scale, disp_spec.width, disp_spec.height) != scale ) {
		return E_NOSPT;			/* the view would be too small */
	}
	if ( knl_wm_screen_changing != NULL ) knl_wm_screen_changing();
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	was_take();
	was = disp_spec.scale;
	disp_spec.scale_want = scale;
	view_set();
	tk_unl_mtx(disp_mtxid);
	if ( disp_spec.scale == was ) {
		if ( knl_wm_screen_kept != NULL ) knl_wm_screen_kept();
		return E_OK;
	}
	tm_printf((UB *)"TessronOS: display drawn at %d%%, a view of %d x %d\n", (INT)disp_spec.scale,
		  (INT)disp_spec.view_w, (INT)disp_spec.view_h);
	if ( knl_wm_screen_changed != NULL ) knl_wm_screen_changed();
	if ( knl_hid_screen_changed != NULL ) knl_hid_screen_changed();
	return E_OK;
}

/* ---------------------------------------------------------------- the check of two pages */

/*
 * Two pages are tried before they are used. The back buffer is given a
 * pattern dark enough to pass for black, and sent whole to the page
 * shown; then, with flipping, FC_SENDS sends of one to three rectangles
 * of new colours each, after each of which every pixel of what that
 * send and the one before it wrote, and some whole rows besides, are
 * read back from the page now shown and compared with the back buffer
 * as a send converts it; at the end the whole of the page shown is.
 * The pages are read through the driver's own mapping, which is not
 * cached, so what is read is what the screen reads. A difference turns
 * flipping off.
 *
 * The time of a send of the whole screen is taken both ways, the least
 * of three: to the page shown, and to the other with the flip. Sent
 * back to back, each send with flipping also waits for the last flip
 * to be taken, which is the most a send ever waits.
 */
#define FC_SENDS	8
#define FC_ROWS		4		/* whole rows read after each send */

LOCAL UW	fc_seed;

LOCAL UW fc_rand( void )
{
	fc_seed = fc_seed * 1103515245U + 12345U;
	return fc_seed >> 8;
}

typedef struct {
	INT	x, y;
	UW	got, want;
	UD	read;			/* pixels compared */
} FCBAD;

/* Whether a page holds what the back buffer does over r, as a send writes it */
LOCAL BOOL page_same( UINT page, CONST T_DISPRECT *r0, FCBAD *b )
{
	CONST UB	*fb = page_of(page);
	UINT		bytes = disp_spec.fb_bpp / 8;
	T_DISPRECT	r = *r0;
	INT		x, y;
	UW		want, got;

	if ( !clip(&r) ) {
		return TRUE;
	}
	for ( y = r.top; y < r.bottom; y++ ) {
		CONST UW	*s = (CONST UW *)( disp_back + (UBINT)y * disp_spec.pitch );
		CONST UB	*d = fb + (UBINT)y * disp_spec.fb_pitch;

		for ( x = r.left; x < r.right; x++ ) {
			send_row(&want, &s[x], 1, disp_spec.fb_format, disp_alpha);
			if ( bytes == 2 ) {
				want &= 0xFFFFU;
				got = *(volatile CONST UH *)( d + (UBINT)x * 2 );
			} else {
				got = *(volatile CONST UW *)( d + (UBINT)x * 4 );
			}
			if ( got != want ) {
				b->x = x;
				b->y = y;
				b->got = got;
				b->want = want;
				return FALSE;
			}
		}
		b->read += (UD)( r.right - r.left );
	}
	return TRUE;
}

/* A rectangle somewhere on the screen, up to a third of it each way */
LOCAL void fc_rect( T_DISPRECT *r )
{
	INT	w = (INT)disp_spec.width, h = (INT)disp_spec.height;
	INT	rw = 1 + (INT)( fc_rand() % (UW)( w / 3 ) );
	INT	rh = 1 + (INT)( fc_rand() % (UW)( h / 3 ) );

	r->left = (INT)( fc_rand() % (UW)( w - rw + 1 ) );
	r->top = (INT)( fc_rand() % (UW)( h - rh + 1 ) );
	r->right = r->left + rw;
	r->bottom = r->top + rh;
}

/* The back buffer over r filled with a dark pattern of the colour c */
LOCAL void fc_fill( CONST T_DISPRECT *r, UW c )
{
	INT	x, y;

	for ( y = r->top; y < r->bottom; y++ ) {
		UW	*p = (UW *)( disp_back + (UBINT)y * disp_spec.pitch );

		for ( x = r->left; x < r->right; x++ ) {
			p[x] = ( c + (UW)( ( x ^ y ) & 0x07 ) * 0x00010101U ) & 0x001F1F1FU;
		}
	}
}

/* One send of the whole screen, and how long it took, us */
LOCAL UD fc_whole( void )
{
	UD	t0, t1;

	(void)ts_get_mono(&t0);
	whole_of(&disp_new);
	flush_locked();
	(void)ts_get_mono(&t1);
	return ( t1 - t0 ) / 1000;
}

LOCAL BOOL flip_check( void )
{
	T_DISPRECT	r[3], was[3], whole, row;
	FCBAD		b;
	INT		k, i, j, n, nw = 0;
	UD		t, one = ~(UD)0, two = ~(UD)0, copy = 0, flip = 0, wait = 0;
	T_DISPSTAT	s0;
	CONST char	*how;

	knl_memset(&b, 0, sizeof(b));
	fc_seed = 0x2545F491U;
	whole.left = 0;
	whole.top = 0;
	whole.right = (INT)disp_spec.width;
	whole.bottom = (INT)disp_spec.height;
	fc_fill(&whole, 0);

	for ( k = 0; k < 3; k++ ) {
		t = fc_whole();
		one = ( t < one ) ? t : one;
	}
	if ( !page_same(disp_shown, &whole, &b) ) {
		k = -1;
		goto bad;
	}

	disp_flipping = TRUE;
	whole_of(&disp_old);
	for ( k = 0; k < FC_SENDS; k++ ) {
		n = 1 + (INT)( fc_rand() % 3 );
		for ( i = 0; i < n; i++ ) {
			fc_rect(&r[i]);
			fc_fill(&r[i], fc_rand() & 0x00101010U);
			gather(&disp_new, &r[i]);
		}
		flush_locked();
		if ( !disp_flipping ) {
			goto bad;		/* the screen did not take a flip */
		}
		for ( i = 0; i < n; i++ ) {
			if ( !page_same(disp_shown, &r[i], &b) ) goto bad;
		}
		for ( i = 0; i < nw; i++ ) {
			if ( !page_same(disp_shown, &was[i], &b) ) goto bad;
		}
		for ( j = 0; j < FC_ROWS; j++ ) {
			row = whole;
			row.top = (INT)( fc_rand() % (UW)disp_spec.height );
			row.bottom = row.top + 1;
			if ( !page_same(disp_shown, &row, &b) ) goto bad;
		}
		for ( i = 0; i < n; i++ ) {
			was[i] = r[i];
		}
		nw = n;
	}
	if ( !page_same(disp_shown, &whole, &b) ) {
		goto bad;
	}

	for ( j = 0; j < 3; j++ ) {
		s0 = disp_stat;
		t = fc_whole();
		if ( t < two ) {
			two = t;
			copy = disp_stat.send_us - s0.send_us;
			flip = disp_flip_us;
			wait = disp_stat.wait_us - s0.wait_us;
		}
	}
	if ( !disp_flipping ) {
		goto bad;
	}

	/* the screen black again, on both pages */
	fc_fill(&whole, 0);
	(void)fc_whole();
	(void)fc_whole();

	how = ( disp_hw.vsync != NULL ) ? "the next vertical blank"
	    : ( disp_hw.latch_us > 0 ) ? "a refresh, without a blank to wait for" : "nothing: taken at once";
	tm_printf((UB *)"disp: flip: check passed: %d sends, %ld Kpx read back from the page shown, "
		  "all as the back buffer holds them\n", FC_SENDS, (D)( b.read / 1000 ));
	tm_printf((UB *)"disp: flip: whole screen %d x %d: one page %ld us; two pages %ld us "
		  "(copy %ld, flip %ld, waiting %ld for the flip before to be taken); waits for %s\n",
		  (INT)disp_spec.width, (INT)disp_spec.height, (D)one, (D)two,
		  (D)copy, (D)flip, (D)wait, how);
	return TRUE;

bad:
	if ( k < 0 ) {
		tm_printf((UB *)"disp: flip: check FAILED: the page shown does not hold what was sent "
			  "to it: (%d,%d) holds %08x, meant %08x; one page\n",
			  b.x, b.y, b.got, b.want);
	} else if ( !disp_flipping ) {
		tm_printf((UB *)"disp: flip: check FAILED at send %d: the screen did not take a flip; "
			  "one page\n", k + 1);
	} else {
		tm_printf((UB *)"disp: flip: check FAILED at send %d: (%d,%d) of page %d holds %08x, "
			  "meant %08x; one page\n", k + 1, b.x, b.y, (INT)disp_shown, b.got, b.want);
	}
	settle();
	disp_flipping = FALSE;
	disp_old.n = 0;
	fc_fill(&whole, 0);
	(void)fc_whole();
	return FALSE;
}

/* ---------------------------------------------------------------- start-up */

/*
 * The outputs besides the main one that the driver found (knl_disp_hw_more),
 * put beside it before anything is drawn. One that cannot be sent to is
 * said and left out.
 */
IMPORT INT knl_disp_hw_more( INT i, UINT main_w, UINT main_h, T_DISPHW *hw, INT *p_side, INT *p_offset )
	__attribute__((weak));

LOCAL void outs_found( void )
{
	T_DISPHW	hw;
	INT		i, side, offset, n, k;
	ER		er;

	if ( knl_disp_hw_more == NULL ) {
		return;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	for ( i = 0; i < DISP_OUTS; i++ ) {
		knl_memset(&hw, 0, sizeof(hw));
		side = DISP_SIDE_RIGHT;
		offset = 0;
		if ( knl_disp_hw_more(i, disp_hw.width, disp_hw.height, &hw, &side, &offset) <= 0 ) {
			break;
		}
		er = out_put(&hw, side, offset);
		if ( er < E_OK ) {
			tm_printf((UB *)"disp: a further screen of %d x %d, %d bpp, via %s, cannot be used (%d)\n",
				  (INT)hw.width, (INT)hw.height, (INT)hw.bpp, ( hw.via != NULL ) ? hw.via : "?", (INT)er);
		}
	}
	n = disp_nout;
	if ( n > 0 && lay_locked(&disp_hw, FALSE) < E_OK ) {
		disp_nout = 0;
		tm_printf((UB *)"disp: no room for a back buffer round %d screens: the main one only\n", n + 1);
	}
	for ( k = 1; k <= disp_nout; k++ ) {
		out_tell(k, "found");
	}
	tk_unl_mtx(disp_mtxid);
}

EXPORT INT knl_disp_init( void )
{
	T_CMTX	cmtx;
	T_DISPHW hw;
	UBINT	back_pages;
	INT	found;

	if ( disp_found ) {
		return 1;
	}
	knl_memset(&hw, 0, sizeof(hw));
	found = knl_disp_hw_init(&hw);
	if ( found <= 0 ) {
		/* the machine has no screen: everything else carries on without one */
		tm_printf((UB *)"TessronOS: no display (%d); carrying on without a screen\n",
			  (INT)found);
		return found;
	}
	if ( !( ( hw.bpp == 32 && ( hw.format == DISP_FMT_XRGB8888
				   || hw.format == DISP_FMT_XBGR8888 ) )
	     || ( hw.bpp == 16 && hw.format == DISP_FMT_RGB565 ) )
	  || hw.width == 0 || hw.height == 0 || hw.pitch < hw.width * ( hw.bpp / 8 ) ) {
		tm_printf((UB *)"TessronOS: no display: a screen of %d x %d, %d bpp, "
			  "pitch %d, format %d cannot be sent to\n",
			  (INT)hw.width, (INT)hw.height, (INT)hw.bpp,
			  (INT)hw.pitch, (INT)hw.format);
		return 0;
	}

	disp_spec.width  = hw.width;
	disp_spec.height = hw.height;
	disp_spec.bpp    = 32;
	disp_spec.format = DISP_FMT_XRGB8888;
	disp_spec.pitch  = ( hw.format == DISP_FMT_XRGB8888 ) ? hw.pitch : hw.width * 4;
	disp_spec.fb_pa  = hw.fb_pa;
	disp_spec.fb_pitch  = hw.pitch;
	disp_spec.fb_bpp    = hw.bpp;
	disp_spec.fb_format = hw.format;
	disp_spec.fb_pages  = ( hw.pages >= 2 && hw.flip != NULL ) ? 2 : 1;
	disp_spec.main_w = hw.width;
	disp_spec.main_h = hw.height;
	disp_spec.main_x = 0;
	disp_spec.main_y = 0;
	view_set();
	disp_alpha = hw.alpha;
	disp_fb = (UB *)hw.fb_va;
	disp_fb2 = (UB *)hw.page2_va;

	/*
	 * The back buffer is a whole screen of ordinary memory. It is
	 * cached, because everything that draws reads and writes it many
	 * times over before any of it reaches the screen.
	 */
	back_pages = ((UBINT)disp_spec.pitch * disp_spec.height
		      + PAGE_SIZE - 1) / PAGE_SIZE;
	disp_back = (UB *)knl_vmap(back_pages, 0);
	if ( disp_back == NULL ) {
		return E_NOMEM;
	}
	knl_memset(disp_back, 0, (SZ)((UBINT)disp_spec.pitch * disp_spec.height));

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	disp_mtxid = tk_cre_mtx(&cmtx);
	if ( disp_mtxid <= 0 ) {
		knl_vunmap(disp_back, back_pages);
		disp_back = NULL;
		return E_LIMIT;
	}

	disp_found = TRUE;
	disp_hw = hw;

	tm_printf((UB *)"TessronOS: display %d x %d, %d bpp%s, pitch %d, at %lx, via %s%s\n",
		  (INT)disp_spec.width, (INT)disp_spec.height, (INT)hw.bpp,
		  ( hw.format == DISP_FMT_XBGR8888 ) ? " (red lowest)" : "",
		  (INT)hw.pitch, hw.fb_pa, ( hw.via != NULL ) ? hw.via : "?",
		  ( disp_spec.fb_pages >= 2 ) ? ", room for two pages" : "");

	if ( disp_spec.fb_pages >= 2 && hw.flip_start ) {
		tk_loc_mtx(disp_mtxid, TMO_FEVR);
		(void)flip_check();
		{
			RECTS	w;

			whole_of(&w);
			main_told(&w);
		}
		tk_unl_mtx(disp_mtxid);
		tm_printf((UB *)"disp: flip: %s\n", disp_flipping
			  ? "on: each send goes to the page not shown, which is then shown"
			  : "off: each send goes to the page shown");
	}
	outs_found();

	return 1;
}
