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
 *	What changed is remembered as one rectangle covering all of it. A
 *	list of separate rectangles would send fewer pixels, but the cost
 *	that matters is the number of times the screen is written to and
 *	the number of rows within that, not the pixels skipped between
 *	them; one rectangle keeps the code short and the sends few. If a
 *	measurement ever says otherwise, this is the place to change.
 *
 *	The back buffer is always four bytes a pixel, blue lowest, whatever
 *	the screen is. When the screen is the same, a row is sent as one
 *	copy. When it is not -- red lowest, two bytes a pixel, or a top
 *	byte it reads as opacity -- each pixel is converted on its way out.
 *	That costs little beside the write itself, and it keeps every
 *	drawing routine to one layout.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include "sysman/pfalloc.h"
#include "disp_hw.h"

LOCAL T_DISPSPEC	disp_spec;
LOCAL BOOL		disp_found = FALSE;
LOCAL UB		*disp_back = NULL;	/* where everything draws */
LOCAL UB		*disp_fb = NULL;	/* where the screen reads from */
LOCAL UINT		disp_alpha;		/* DISP_ALPHA_* */
LOCAL ID		disp_mtxid = 0;

LOCAL INT		disp_held = 0;
LOCAL BOOL		disp_dirty = FALSE;
LOCAL T_DISPRECT	disp_rect;		/* all of what changed */
LOCAL T_DISPSTAT	disp_stat;

/* ---------------------------------------------------------------- rows */

/* One row of pixels converted to what the screen takes */
LOCAL void send_row( UW *d, CONST UW *s, INT n )
{
	UW	keep = ( disp_alpha == DISP_ALPHA_ANY ) ? 0xFFFFFFFFU : 0x00FFFFFFU;
	UW	set = ( disp_alpha == DISP_ALPHA_FF ) ? 0xFF000000U : 0;
	INT	x;

	if ( disp_spec.fb_format == DISP_FMT_XBGR8888 ) {
		for ( x = 0; x < n; x++ ) {
			UW	v = s[x];

			d[x] = ( ( ( v & 0xFF00FF00U ) | ( ( v >> 16 ) & 0xFFU )
				   | ( ( v & 0xFFU ) << 16 ) ) & keep ) | set;
		}
	} else if ( disp_spec.fb_format == DISP_FMT_RGB565 ) {
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
 * Send one run of rows. When the screen is laid out as the back buffer
 * is, each row is one straight copy.
 */
LOCAL void send_rows( INT top, INT bottom, INT left, INT right )
{
	UINT	bytes = (UINT)(right - left) * 4;
	UINT	off = (UINT)left * 4;
	UINT	fb_off = (UINT)left * ( disp_spec.fb_bpp / 8 );
	BOOL	same = ( disp_spec.fb_format == DISP_FMT_XRGB8888
			 && disp_alpha == DISP_ALPHA_ANY );
	INT	y;

	for ( y = top; y < bottom; y++ ) {
		UB	*d = disp_fb + (UBINT)y * disp_spec.fb_pitch + fb_off;
		CONST UB *s = disp_back + (UBINT)y * disp_spec.pitch + off;

		if ( same ) {
			knl_memcpy(d, s, (SZ)bytes);
		} else {
			send_row((UW *)d, (CONST UW *)s, right - left);
		}
	}
	Asm("dsb sy" ::: "memory");

	disp_stat.sends++;
	disp_stat.rects++;
	disp_stat.rows += (UD)(bottom - top);
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

/* Send whatever is outstanding. The caller holds the lock. */
LOCAL void flush_locked( void )
{
	if ( !disp_dirty ) {
		return;
	}
	disp_dirty = FALSE;
	if ( clip(&disp_rect) ) {
		send_rows(disp_rect.top, disp_rect.bottom,
			  disp_rect.left, disp_rect.right);
	}
}

/* ---------------------------------------------------------------- interface */

EXPORT ER ts_disp_ref( T_DISPSPEC *spec )
{
	if ( !disp_found || spec == NULL ) {
		return E_NOEXS;
	}
	*spec = disp_spec;
	spec->held = (UINT)disp_held;

	return E_OK;
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

	if ( !disp_dirty ) {
		disp_rect = *r;
		disp_dirty = TRUE;
	} else {
		if ( r->left   < disp_rect.left )   disp_rect.left   = r->left;
		if ( r->top    < disp_rect.top )    disp_rect.top    = r->top;
		if ( r->right  > disp_rect.right )  disp_rect.right  = r->right;
		if ( r->bottom > disp_rect.bottom ) disp_rect.bottom = r->bottom;
	}
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

/*
 * The screen set to another size (システム環境設定, at the desktop's
 * start before a window is open): the hardware first, then a back
 * buffer of the new size, then whoever keeps the size told.
 */
LOCAL T_DISPHW	disp_hw;
IMPORT void knl_wm_screen_changed( void ) __attribute__((weak));
IMPORT void knl_hid_screen_changed( void ) __attribute__((weak));

EXPORT ER ts_disp_setmode( UINT w, UINT h )
{
	T_DISPHW	hw;
	UB		*back;
	UBINT		pages, was_pages;
	UINT		pitch;

	if ( !disp_found ) {
		return E_NOEXS;
	}
	if ( w == disp_spec.width && h == disp_spec.height ) {
		return E_OK;
	}
	tk_loc_mtx(disp_mtxid, TMO_FEVR);
	hw = disp_hw;
	if ( knl_disp_hw_setmode(&hw, w, h) <= 0 ) {
		tk_unl_mtx(disp_mtxid);
		return E_NOSPT;
	}
	pitch = ( hw.format == DISP_FMT_XRGB8888 ) ? hw.pitch : hw.width * 4;
	pages = ( (UBINT)pitch * hw.height + PAGE_SIZE - 1 ) / PAGE_SIZE;
	back = (UB *)knl_vmap(pages, 0);
	if ( back == NULL ) {
		(void)knl_disp_hw_setmode(&hw, disp_spec.width, disp_spec.height);
		tk_unl_mtx(disp_mtxid);
		return E_NOMEM;
	}
	knl_memset(back, 0, (SZ)( (UBINT)pitch * hw.height ));
	was_pages = ( (UBINT)disp_spec.pitch * disp_spec.height + PAGE_SIZE - 1 ) / PAGE_SIZE;
	knl_vunmap(disp_back, was_pages);
	disp_back = back;
	disp_hw = hw;
	disp_spec.width = hw.width;
	disp_spec.height = hw.height;
	disp_spec.pitch = pitch;
	disp_spec.fb_pa = hw.fb_pa;
	disp_spec.fb_pitch = hw.pitch;
	disp_spec.fb_bpp = hw.bpp;
	disp_spec.fb_format = hw.format;
	disp_fb = (UB *)hw.fb_va;
	tk_unl_mtx(disp_mtxid);
	tm_printf((UB *)"TessronOS: display now %d x %d\n", (INT)hw.width, (INT)hw.height);
	if ( knl_wm_screen_changed != NULL ) knl_wm_screen_changed();
	if ( knl_hid_screen_changed != NULL ) knl_hid_screen_changed();
	return E_OK;
}

/* ---------------------------------------------------------------- start-up */

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
	disp_alpha = hw.alpha;
	disp_fb = (UB *)hw.fb_va;

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

	tm_printf((UB *)"TessronOS: display %d x %d, %d bpp%s, pitch %d, at %lx, via %s\n",
		  (INT)disp_spec.width, (INT)disp_spec.height, (INT)hw.bpp,
		  ( hw.format == DISP_FMT_XBGR8888 ) ? " (red lowest)" : "",
		  (INT)hw.pitch, hw.fb_pa, ( hw.via != NULL ) ? hw.via : "?");

	return 1;
}
