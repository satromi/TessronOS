/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wm.c
 *	Windows (design 16.4, 16.5.5).
 *
 *	Three invariants hold everything here together, and each of them is
 *	checked by wm_self_check() rather than only believed.
 *
 *	The work area is a function of the outer rectangle and the window's
 *	attributes, and of nothing else. It is worked out by work_of() and
 *	stored only as a convenience; the check recomputes it. That is what
 *	lets the frame be redesigned in one place.
 *
 *	A window's own pixels are laid out as if they were a piece of the
 *	screen: the same number of bytes in a row as the screen has. The
 *	offset of a pixel is taken from the window's own corner, never from
 *	the corner of whatever is drawing. Getting that wrong writes
 *	outside the surface.
 *
 *	The order of the windows is a permutation of 0..n-1 with no gaps and
 *	no repeats, 0 being nearest the viewer. Compositing walks it from
 *	the back forwards, so what is in front is written last.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <ts/hid.h>
#include <ts/fn.h>
#include <ts/part.h>
#include <ts/img.h>
#include <ts/proc.h>
#include "sysman/pfalloc.h"
#include "wmobj.h"

/*
 * What the frame takes, in pixels. One place, per 16.2.2: the numbers
 * below and the two lengths in the look table are the whole of it.
 *
 * A strip of frame is built the same way on every side: the window's
 * outline, a lit line, the band, a shaded line, and then the line round
 * the work area. Three on the left and top, four on the right and the
 * foot, because the lit lines fall on the first two sides and the
 * shaded ones on the other two.
 */
#define EDGE_L		3
#define EDGE_T		3
#define EDGE_R		4
#define EDGE_B		4
#define GRIP_W		12		/* a side that can be pulled but has no bar */
#define BAR_SMALL	12		/* the bar of a window whose size is fixed */
#define WORK_LEAST	16		/* the least work area a window may be left with */

typedef struct {
	BOOL		used;
	UINT		attr;
	T_DPRECT	outer;		/* screen coordinates */
	T_DPRECT	work;		/* worked out from outer and attr */
	T_DPPOINT	scroll;
	INT		gid;
	INT		z;		/* 0 is nearest the viewer */
	INT		parent;		/* a subordinate window's window, kept just behind it; 0 none */
	BOOL		visible;
	char		title[WM_TITLE_MAX];
	INT		pict;		/* the picture in the band, by number */
	ID		owner;		/* whose pictures and panels are looked in */
	UW		*icon;		/* the icon of the object it stands for, or NULL */
	INT		icon_w, icon_h;

	UB		*pixels;	/* this window's own */
	UD		pix_pages;
	BOOL		dirty;
	T_DPRECT	damage;		/* in work area coordinates */
	T_WMBAR		bar[WM_BAR_MAX];
	T_DPRGN		*shape;		/* NULL: the plain rectangle */
	T_DPRGN		*own;		/* the outline it was given, or NULL */
	UINT		opacity;	/* 255: nothing behind it shows */
	UW		tint;		/* laid over the frame before mixing */
	UINT		tint_str;	/* 0: no tint at all */
} WMWIN;

LOCAL WMWIN		wm_win[WM_MAX_WIN];

/* The outlines of what is being carried, on the screen */
LOCAL T_DPRECT		wm_drag[WM_DRAG_MAX];
LOCAL INT		wm_ndrag = 0;
LOCAL INT		wm_nwin = 0;
LOCAL BOOL		wm_ready = FALSE;
LOCAL ID		wm_mtxid = 0;
LOCAL T_DISPSPEC	wm_spec;
LOCAL T_WMSTAT		wm_count;
LOCAL INT		wm_focus_wid = 0;
LOCAL INT		wm_sys_gid = -1;	/* what the frames are drawn through */
/*
 * Held while wm_sys_gid is aimed somewhere and drawn through. The one
 * environment is aimed at a window's pixels for its frame, by whatever
 * task changes that window, with or without wm_mtxid, and at the screen
 * for the pointer by whoever builds the screen: two of them at once
 * would draw one window's frame with the other's target, frame and
 * limits, and write past the end of a window's pixels. Taken after
 * wm_mtxid when both are held.
 */
LOCAL ID		wm_sys_mtxid = 0;

LOCAL void sys_lock( void )
{
	(void)tk_loc_mtx(wm_sys_mtxid, TMO_FEVR);
}

LOCAL void sys_unlock( void )
{
	(void)tk_unl_mtx(wm_sys_mtxid);
}

/*
 * The look table lives in look.c: it is numbers with no knowledge of
 * windows, and windows are what this file is about. What stays here is
 * what a change of scheme does to the windows.
 */
IMPORT INT knl_look_init( void );
IMPORT ER  knl_look_scheme( UINT n );

/* The message line (msgline.c) */
IMPORT INT  knl_msg_top( void );
IMPORT void knl_msg_lay( UB *back, UINT pitch, CONST T_DPRECT *box );
IMPORT void knl_msg_press( INT x, INT y );
IMPORT void knl_msg_tick( void );
IMPORT void knl_msg_restyle( void );

/* The picture the windows stand on */
LOCAL CONST UW		*wm_wall = NULL;
LOCAL INT		wm_wall_w = 0, wm_wall_h = 0;
LOCAL UINT		wm_wall_mode = WM_WALL_TILE;

/*
 * The ground, worked out once.
 *
 * Every screen begins with the desk laid down under the windows. Doing
 * that from the picture each time means a division and a look-up for
 * every pixel of the screen, which is a million of each before a single
 * window has been drawn -- and none of it changes between one screen
 * and the next. So it is worked out into a run of pixels the size of
 * the screen, and laying the desk becomes a copy of whole rows.
 *
 * It is thrown away rather than rebuilt when the picture, the colours
 * or the size of the screen change; the next screen builds it again.
 */
LOCAL UW		*wm_ground = NULL;
LOCAL UD		wm_ground_pages = 0;
LOCAL INT		wm_ground_w = 0, wm_ground_h = 0;

LOCAL void ground_drop( void )
{
	if ( wm_ground != NULL ) {
		knl_vunmap((UB *)wm_ground, wm_ground_pages);
		wm_ground = NULL;
		wm_ground_pages = 0;
		wm_ground_w = wm_ground_h = 0;
	}
}

LOCAL void ground_make( void )
{
	UW	desk = wm_look(WM_LOOK_DESK);
	INT	sw = (INT)wm_spec.width, sh = (INT)wm_spec.height;
	INT	x, y, ox = 0, oy = 0;
	SZ	want;

	if ( wm_ground != NULL && wm_ground_w == sw && wm_ground_h == sh ) {
		return;
	}
	ground_drop();
	if ( sw <= 0 || sh <= 0 ) {
		return;
	}
	want = (SZ)sw * sh * sizeof(UW);
	wm_ground_pages = (UD)( ( want + 4095 ) / 4096 );
	wm_ground = (UW *)knl_vmap((UD)wm_ground_pages, 0);
	if ( wm_ground == NULL ) {
		wm_ground_pages = 0;
		return;				/* drawn the long way this time */
	}
	wm_ground_w = sw;
	wm_ground_h = sh;

	if ( wm_wall != NULL && wm_wall_mode == WM_WALL_CENTRE ) {
		ox = (sw - wm_wall_w) / 2;
		oy = (sh - wm_wall_h) / 2;
	}
	for ( y = 0; y < sh; y++ ) {
		UW	*p = wm_ground + (SZ)y * sw;

		if ( wm_wall == NULL ) {
			for ( x = 0; x < sw; x++ ) {
				*p++ = desk;
			}
			continue;
		}
		for ( x = 0; x < sw; x++, p++ ) {
			INT	wx, wy;

			switch ( wm_wall_mode ) {
			case WM_WALL_CENTRE:
				wx = x - ox;  wy = y - oy;
				if ( wx < 0 || wx >= wm_wall_w
				  || wy < 0 || wy >= wm_wall_h ) {
					*p = desk;
					continue;
				}
				break;
			case WM_WALL_FIT:
				wx = ( sw > 0 ) ? x * wm_wall_w / sw : 0;
				wy = ( sh > 0 ) ? y * wm_wall_h / sh : 0;
				break;
			default:
				wx = x % wm_wall_w;
				wy = y % wm_wall_h;
				break;
			}
			*p = wm_wall[(SZ)wy * wm_wall_w + wx];
		}
	}
}

LOCAL void draw_frame( CONST WMWIN *w );
LOCAL void shape_of( WMWIN *w );

EXPORT ER wm_set( UINT num, INT value )
{
	INT	i;
	ER	er;

	if ( value < 0 ) {
		return E_PAR;
	}
	if ( ( num == WM_SET_SHADOW || num == WM_SET_OPACITY
	    || num == WM_SET_WASH_STR ) && value > 255 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	er = wm_set_look(num, (UW)value);
	if ( er >= E_OK ) {
		ground_drop();		/* the desk's own colour may have moved */
	}
	if ( er >= E_OK && num == WM_SET_ROUND ) {
		for ( i = 0; i < WM_MAX_WIN; i++ ) {
			if ( wm_win[i].used ) {
				shape_of(&wm_win[i]);
			}
		}
	}
	tk_unl_mtx(wm_mtxid);

	return er;
}

EXPORT INT wm_get( UINT num )
{
	return wm_num(num, -1);
}

/*
 * A scheme put on: the colours written, then every window drawn again
 * -- its frame here, and its work area by saying the whole of it needs
 * drawing, which is how the program that owns it hears that the
 * colours it drew with have changed.
 */
EXPORT ER wm_set_scheme( UINT n )
{
	INT	i;
	ER	er;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	er = knl_look_scheme(n);
	if ( er < E_OK ) {
		tk_unl_mtx(wm_mtxid);
		return er;
	}
	ground_drop();				/* the kept ground is out of date */
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		WMWIN	*w = &wm_win[i];

		if ( !w->used ) {
			continue;
		}
		draw_frame(w);
		w->dirty = TRUE;
		w->damage.left = 0;
		w->damage.top = 0;
		w->damage.right = w->work.right - w->work.left;
		w->damage.bottom = w->work.bottom - w->work.top;
	}
	tk_unl_mtx(wm_mtxid);
	knl_msg_restyle();

	return E_OK;
}

EXPORT ER wm_set_wall( CONST UW *pixels, INT w, INT h, UINT mode )
{
	if ( pixels != NULL && ( w <= 0 || h <= 0 || mode > WM_WALL_FIT ) ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	wm_wall = pixels;
	wm_wall_w = ( pixels != NULL ) ? w : 0;
	wm_wall_h = ( pixels != NULL ) ? h : 0;
	wm_wall_mode = mode;
	ground_drop();				/* the kept ground is out of date */
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}


/* Whether a place falls in one of a row's spans */
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

/*
 * Two colours mixed, channel by channel: 'over' at the strength given
 * and what was there for the rest. The channels are kept apart because
 * mixing the whole word at once carries the bits of one channel into
 * the next, which shows as a colour nobody chose.
 */
LOCAL UW mix( UW under, UW over, UINT strength )
{
	UINT	a = strength & 0xFFU, b = 255U - (strength & 0xFFU);
	UW	r, g, bl;

	r  = ((( over >> 16) & 0xFFU) * a + ((under >> 16) & 0xFFU) * b) / 255U;
	g  = ((( over >>  8) & 0xFFU) * a + ((under >>  8) & 0xFFU) * b) / 255U;
	bl = ((  over        & 0xFFU) * a + ( under        & 0xFFU) * b) / 255U;

	return (r << 16) | (g << 8) | bl;
}

/* ---------------------------------------------------------------- shapes */

LOCAL WMWIN *win_of( INT wid )
{
	if ( wid < 1 || wid > WM_MAX_WIN ) {
		return NULL;
	}
	if ( !wm_win[wid - 1].used ) {
		return NULL;
	}

	return &wm_win[wid - 1];
}

/* A length out of the look table, or what to use when it holds none */
LOCAL INT look_len( UINT n, INT dflt )
{
	UW	v = wm_look(n);

	return ( v > 0 && v < 0x10000U ) ? (INT)v : dflt;
}

/*
 * How far the work area lies inside the outer rectangle on each side.
 * This and work_of() below are the only places that know what a frame
 * takes; everything that needs a measurement asks one of them.
 *
 * Each strip holds, from the outside in: the window's outline, a lit
 * line, the band, a shaded line, and the line round the work area --
 * four lines and the band. A strip with a scroll bar in it is as wide
 * as the bar and two more; a side of a window whose size can be changed
 * keeps the same width whether or not it carries a bar, because that is
 * where the size is taken hold of. The band along the top is as tall as
 * the name plus eight.
 *
 * A window whose size cannot be changed has narrower strips, and a
 * narrower bar with them: there is nothing to take hold of there.
 */
LOCAL void borders_of( UINT attr, INT *l, INT *t, INT *r, INT *b )
{
	INT	th = look_len(WM_LOOK_TITLE_H, 16);
	INT	bw = look_len(WM_LOOK_BAR_W, 20);
	BOOL	big = ( (attr & WM_ATTR_RESIZE) != 0 );

	if ( (attr & (WM_ATTR_FRAME | WM_ATTR_TITLE)) == 0 ) {
		*l = *t = *r = *b = 0;		/* a window with no frame */
		return;
	}
	if ( big ) {
		*l = 4 + ( ((attr & WM_ATTR_LBAR) != 0) ? bw - 2 : 6 );
		*r = ( ((attr & WM_ATTR_RBAR) != 0) ? bw + 2 : GRIP_W - 2 );
		*b = ( ((attr & WM_ATTR_BBAR) != 0) ? bw + 2 : GRIP_W - 2 );
	} else {
		*l = 4 + ( ((attr & WM_ATTR_LBAR) != 0) ? BAR_SMALL - 2 : 2 );
		*r = ( ((attr & WM_ATTR_RBAR) != 0) ? BAR_SMALL + 2 : 6 );
		*b = ( ((attr & WM_ATTR_BBAR) != 0) ? BAR_SMALL + 2 : 6 );
	}
	*t = 4 + ( ((attr & WM_ATTR_TITLE) != 0) ? th + 8 : ( big ? 6 : 2 ) );
}

/*
 * The work area of a window, from its outer rectangle and its
 * attributes. Nothing else knows the measurements of a frame, and
 * nothing else may: this is the one function that does.
 */
LOCAL void work_of( CONST T_DPRECT *outer, UINT attr, T_DPRECT *work )
{
	INT	l, t, r, b;

	borders_of(attr, &l, &t, &r, &b);

	work->left   = outer->left + l;
	work->top    = outer->top + t;
	work->right  = outer->right - r;
	work->bottom = outer->bottom - b;

	/* a window too small for its own frame keeps an empty work area */
	if ( work->right < work->left ) {
		work->right = work->left;
	}
	if ( work->bottom < work->top ) {
		work->bottom = work->top;
	}
}

LOCAL BOOL rect_cut( T_DPRECT *r, CONST T_DPRECT *by )
{
	if ( by->left   > r->left )   r->left   = by->left;
	if ( by->top    > r->top )    r->top    = by->top;
	if ( by->right  < r->right )  r->right  = by->right;
	if ( by->bottom < r->bottom ) r->bottom = by->bottom;

	return ( r->left < r->right && r->top < r->bottom );
}

LOCAL BOOL rect_covers( CONST T_DPRECT *a, CONST T_DPRECT *b )
{
	return ( a->left <= b->left && a->top <= b->top
	      && a->right >= b->right && a->bottom >= b->bottom );
}

/* ---------------------------------------------------------------- pixels */

/*
 * A pixel of a window's own surface. The row is as long as a row of the
 * screen, and the offset is measured from this window's corner.
 */
LOCAL UW *win_pixel( CONST WMWIN *w, INT sx, INT sy )
{
	INT	x = sx - w->outer.left;
	INT	y = sy - w->outer.top;

	return (UW *)(w->pixels + (UBINT)y * wm_spec.pitch + (UBINT)x * 4);
}

LOCAL UD win_pages( CONST T_DPRECT *outer )
{
	UD	h = (UD)(outer->bottom - outer->top);

	return ((UD)wm_spec.pitch * h + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* ---------------------------------------------------------------- order */

/* The window at a given place in the order, or NULL */
LOCAL WMWIN *win_at_z( INT z )
{
	INT	i;

	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && wm_win[i].z == z ) {
			return &wm_win[i];
		}
	}

	return NULL;
}

/* Close the gap a window left behind when it was removed from the order */
LOCAL void z_close_gap( INT gone )
{
	INT	i;

	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && wm_win[i].z > gone ) {
			wm_win[i].z--;
		}
	}
}

/* ---------------------------------------------------------------- windows */

EXPORT INT wm_open( CONST T_DPRECT *outer, UINT attr, CONST char *title )
{
	WMWIN	*w;
	INT	i, k;
	UD	pages;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( outer == NULL || outer->right <= outer->left
	  || outer->bottom <= outer->top ) {
		return E_PAR;
	}
	if ( outer->right - outer->left > (INT)wm_spec.width
	  || outer->bottom - outer->top > (INT)wm_spec.height ) {
		return E_PAR;			/* larger than the screen */
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);

	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( !wm_win[i].used ) {
			break;
		}
	}
	if ( i == WM_MAX_WIN ) {
		tk_unl_mtx(wm_mtxid);
		return E_LIMIT;
	}
	w = &wm_win[i];
	knl_memset(w, 0, sizeof(*w));

	w->attr = attr;
	w->outer = *outer;
	work_of(outer, attr, &w->work);
	w->shape = NULL;
	w->own = NULL;
	shape_of(w);
	w->opacity = (UINT)wm_num(WM_SET_OPACITY, 255);
	w->tint = 0;
	w->tint_str = 0;

	pages = win_pages(outer);
	w->pixels = (UB *)knl_vmap(pages, 0);
	if ( w->pixels == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_NOMEM;
	}
	w->pix_pages = pages;
	knl_memset(w->pixels, 0, (SZ)(pages * PAGE_SIZE));

	w->pict = 0;
	w->owner = 0;
	for ( k = 0; k < WM_TITLE_MAX - 1 && title != NULL && title[k] != '\0'; k++ ) {
		w->title[k] = title[k];
	}

	/* it comes up in front of everything else */
	for ( k = 0; k < WM_MAX_WIN; k++ ) {
		if ( wm_win[k].used ) {
			wm_win[k].z++;
		}
	}
	w->z = 0;
	w->used = TRUE;
	w->visible = TRUE;
	wm_nwin++;

	/*
	 * The environment that draws into this window. Its origin is the
	 * corner of the work area and its frame is the work area, so the
	 * program draws in its own coordinates and can reach nothing else.
	 */
	w->gid = dp_open();
	if ( w->gid < 0 ) {
		knl_vunmap(w->pixels, pages);
		w->used = FALSE;
		wm_nwin--;
		z_close_gap(0);
		tk_unl_mtx(wm_mtxid);
		return E_LIMIT;
	}
	tk_unl_mtx(wm_mtxid);

	/* the work area starts as the look says, not as black */
	{
		T_DPRECT	work;

		/* from the window's own corner, like everything the frame draws */
		work.left   = w->work.left - w->outer.left;
		work.top    = w->work.top - w->outer.top;
		work.right  = w->work.right - w->outer.left;
		work.bottom = w->work.bottom - w->outer.top;

		if ( wm_sys_gid >= 0 && work.right > work.left ) {
			sys_lock();
			dp_set_target(wm_sys_gid, w->pixels, wm_spec.pitch,
					      w->outer.left, w->outer.top);
			dp_set_origin(wm_sys_gid, w->outer.left, w->outer.top);
			dp_set_frame(wm_sys_gid, &w->outer);
			{
				T_DPRECT v;

				v.left = 0;  v.top = 0;
				v.right = w->outer.right - w->outer.left;
				v.bottom = w->outer.bottom - w->outer.top;
				dp_set_visible(wm_sys_gid, &v);
			}
			dp_fill_rect(wm_sys_gid, &work,
				     ( ( attr & WM_ATTR_POPUP ) != 0 )
				     ? WM_CLEAR : wm_look(WM_LOOK_WORK));
			sys_unlock();
		}
	}
	draw_frame(w);

	/* it draws into this window's own pixels, not onto the screen */
	dp_set_target(w->gid, w->pixels, wm_spec.pitch,
			      w->outer.left, w->outer.top);
	dp_set_origin(w->gid, w->work.left, w->work.top);
	dp_set_frame(w->gid, &w->work);
	{
		T_DPRECT	v;

		v.left = 0;  v.top = 0;
		v.right = w->work.right - w->work.left;
		v.bottom = w->work.bottom - w->work.top;
		dp_set_visible(w->gid, &v);
	}
	knl_wmobj_open(i + 1);			/* it is an object from now on */

	return i + 1;
}

/*
 * Places of the screen to be laid again at the next wm_update that no
 * window's damage covers: where windows stood that have since been
 * closed, a window brought forward or sent back (what it covers has
 * changed, not what it holds), and a frame drawn again when the input
 * moved (the frame is outside the work area the damage speaks of).
 */
LOCAL T_DPRECT	wm_gone;
LOCAL BOOL	wm_gone_any = FALSE;

LOCAL void gone_add( CONST T_DPRECT *r )
{
	if ( !wm_gone_any ) {
		wm_gone = *r;
		wm_gone_any = TRUE;
		return;
	}
	if ( r->left < wm_gone.left )     wm_gone.left = r->left;
	if ( r->top < wm_gone.top )       wm_gone.top = r->top;
	if ( r->right > wm_gone.right )   wm_gone.right = r->right;
	if ( r->bottom > wm_gone.bottom ) wm_gone.bottom = r->bottom;
}

/*
 * The window nearest the viewer that a person works in: shown, with a
 * name band, and not a menu, panel or other pop-up. 0 when there is
 * none. Called with the lock held.
 */
LOCAL INT front_window( INT but )
{
	INT	i, best = 0, bz = 0;

	for ( i = 1; i <= WM_MAX_WIN; i++ ) {
		WMWIN	*w = win_of(i);

		if ( w == NULL || i == but || !w->visible
		  || ( w->attr & WM_ATTR_POPUP ) != 0 || ( w->attr & WM_ATTR_TITLE ) == 0 ) {
			continue;
		}
		if ( best == 0 || w->z < bz ) {
			best = i;
			bz = w->z;
		}
	}
	return best;
}

EXPORT ER wm_close( INT wid )
{
	WMWIN	*w;
	INT	z;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	z = w->z;
	if ( w->visible ) {
		gone_add(&w->outer);
	}
	if ( w->gid > 0 ) {
		dp_close(w->gid);
	}
	if ( w->pixels != NULL ) {
		knl_vunmap(w->pixels, w->pix_pages);
	}
	dp_rgn_free(w->shape);
	w->shape = NULL;
	dp_rgn_free(w->own);
	if ( w->icon != NULL ) {
		Kfree(w->icon);
		w->icon = NULL;
	}
	w->own = NULL;
	w->used = FALSE;
	wm_nwin--;
	z_close_gap(z);
	if ( wm_focus_wid == wid ) {
		/*
		 * The input goes on to the window that was behind: the one in
		 * front of those left. A person who closes a window goes on
		 * working in the next without having to press it first.
		 */
		WMWIN	*n;

		wm_focus_wid = front_window(wid);
		n = win_of(wm_focus_wid);
		if ( n != NULL ) {
			draw_frame(n);
			gone_add(&n->outer);
		}
	}
	tk_unl_mtx(wm_mtxid);
	knl_wmobj_close(wid);

	return E_OK;
}

/* A window's title, for what reads it as the object's name */
EXPORT ER knl_wm_title( INT wid, char *buf, INT max )
{
	WMWIN	*w;
	INT	k = 0;

	if ( buf == NULL || max <= 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w != NULL ) {
		for ( k = 0; k < max - 1 && w->title[k] != '\0'; k++ ) {
			buf[k] = w->title[k];
		}
	}
	buf[k] = '\0';
	tk_unl_mtx(wm_mtxid);

	return ( w != NULL ) ? E_OK : E_ID;
}

EXPORT ER wm_ref( INT wid, T_WMWIN *out )
{
	WMWIN	*w;

	if ( out == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w != NULL ) {
		out->attr    = w->attr;
		out->outer   = w->outer;
		out->work    = w->work;
		out->scroll  = w->scroll;
		out->gid     = w->gid;
		out->z       = w->z;
		out->visible = ( w->visible ) ? 1 : 0;
		out->parent  = w->parent;
	}
	tk_unl_mtx(wm_mtxid);

	return ( w != NULL ) ? E_OK : E_ID;
}

/*
 * One conversion serves all six directions, because both sides go
 * through the work area. Nothing else knows how the three relate.
 */
EXPORT ER wm_convert( INT wid, UINT from, UINT to, CONST T_DPRECT *in,
		      T_DPRECT *out )
{
	WMWIN		*w;
	T_DPRECT	work;
	INT		dx, dy;

	if ( in == NULL || out == NULL || from > WM_RECT_DRAW
	  || to > WM_RECT_DRAW ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}

	/* first into the work area's own coordinates */
	work = *in;
	if ( from == WM_RECT_OUTER ) {
		dx = -w->work.left;
		dy = -w->work.top;
	} else if ( from == WM_RECT_DRAW ) {
		dx = -w->scroll.x;
		dy = -w->scroll.y;
	} else {
		dx = 0;
		dy = 0;
	}
	work.left += dx;  work.right  += dx;
	work.top  += dy;  work.bottom += dy;

	/* then out of them again */
	if ( to == WM_RECT_OUTER ) {
		dx = w->work.left;
		dy = w->work.top;
	} else if ( to == WM_RECT_DRAW ) {
		dx = w->scroll.x;
		dy = w->scroll.y;
	} else {
		dx = 0;
		dy = 0;
	}
	work.left += dx;  work.right  += dx;
	work.top  += dy;  work.bottom += dy;

	tk_unl_mtx(wm_mtxid);
	*out = work;

	return E_OK;
}

EXPORT ER wm_move( INT wid, CONST T_DPRECT *outer )
{
	WMWIN		*w;
	T_DPRECT	v;
	UD		pages, oldpages = 0;
	UB		*pixels, *old;
	BOOL		resized;

	if ( outer == NULL || outer->right <= outer->left
	  || outer->bottom <= outer->top ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	{
		INT	wide, high;

		wm_least(w->attr, &wide, &high);
		if ( outer->right - outer->left < wide
		  || outer->bottom - outer->top < high ) {
			tk_unl_mtx(wm_mtxid);
			return E_PAR;		/* smaller than its own frame */
		}
	}

	/*
	 * A taller window needs a larger surface. The pixels are not
	 * carried over: what a window holds is its own business, and it
	 * will be told to draw itself again.
	 */
	pages = win_pages(outer);
	old = NULL;
	if ( pages != w->pix_pages ) {
		pixels = (UB *)knl_vmap(pages, 0);
		if ( pixels == NULL ) {
			tk_unl_mtx(wm_mtxid);
			return E_NOMEM;
		}
		knl_memset(pixels, 0, (SZ)(pages * PAGE_SIZE));
		old = w->pixels;		/* let go of once nothing draws there */
		oldpages = w->pix_pages;
		w->pixels = pixels;
		w->pix_pages = pages;
	}
	resized = ( outer->right - outer->left != w->outer.right - w->outer.left
		 || outer->bottom - outer->top != w->outer.bottom - w->outer.top );
	v = w->work;
	w->outer = *outer;
	work_of(outer, w->attr, &w->work);
	/* the same outside with a frame measured anew is a new work area too */
	resized = (BOOL)( resized || v.right - v.left != w->work.right - w->work.left
			  || v.bottom - v.top != w->work.bottom - w->work.top );
	shape_of(w);				/* a new size is a new shape */
	w->dirty = TRUE;
	w->damage.left = 0;
	w->damage.top = 0;
	w->damage.right = w->work.right - w->work.left;
	w->damage.bottom = w->work.bottom - w->work.top;
	v = w->work;
	pixels = w->pixels;
	tk_unl_mtx(wm_mtxid);

	/*
	 * The environment follows the window, in one step: its program may
	 * be drawing in it just now, and a draw between the surface and the
	 * frame would take one new and the other old. The old surface goes
	 * after, when no draw can still be on it.
	 */
	dp_set_place(w->gid, pixels, wm_spec.pitch, outer->left, outer->top, &v);
	if ( old != NULL ) {
		knl_vunmap(old, oldpages);
	}

	draw_frame(w);				/* the frame moved with it */
	knl_wmobj_moved(wid, resized);

	return E_OK;
}

/*
 * The name's height or the scroll bars' width changed (ユーザ環境設定):
 * every window measured again as wm_move measures it, its frame drawn
 * and its program told that its work area has a new size. A window
 * that would now be smaller than its own frame is made as large as the
 * frame needs. The serial number tells a program that draws its windows
 * itself, not through their objects, that it has to draw them all again.
 */
LOCAL UW	wm_layout_gen = 0;

EXPORT UW wm_layout_serial( void )
{
	return wm_layout_gen;
}

EXPORT void wm_relayout( void )
{
	T_DPRECT	o;
	UINT		attr;
	BOOL		used;
	INT		i, wide, high;

	if ( !wm_ready ) {
		return;
	}
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
		used = wm_win[i].used;
		o = wm_win[i].outer;
		attr = wm_win[i].attr;
		tk_unl_mtx(wm_mtxid);
		if ( !used ) {
			continue;
		}
		if ( wm_least(attr, &wide, &high) >= E_OK ) {
			if ( o.right - o.left < wide ) o.right = o.left + wide;
			if ( o.bottom - o.top < high ) o.bottom = o.top + high;
		}
		(void)wm_move(i + 1, &o);
	}
	wm_layout_gen++;
	(void)wm_composite();
}

/*
 * The popups put back in front of everything, in the order they stood
 * among themselves: a window brought forward comes up behind them, as
 * a menu's lists stay over the window they were opened from.
 */
LOCAL void popups_front( void )
{
	INT	i, k, best;

	for ( k = 0; k < wm_nwin; k++ ) {
		best = -1;
		/* the popup furthest back that is not yet in front */
		for ( i = 0; i < WM_MAX_WIN; i++ ) {
			if ( wm_win[i].used && ( wm_win[i].attr & WM_ATTR_POPUP ) != 0
			  && wm_win[i].z >= k
			  && ( best < 0 || wm_win[i].z > wm_win[best].z ) ) {
				best = i;
			}
		}
		if ( best < 0 || wm_win[best].z == k ) {
			break;
		}
		{
			INT	old = wm_win[best].z;

			for ( i = 0; i < WM_MAX_WIN; i++ ) {
				if ( wm_win[i].used && wm_win[i].z < old ) {
					wm_win[i].z++;
				}
			}
			wm_win[best].z = 0;
		}
	}
}

/*
 * The windows with a name in the band that are shown, nearest the
 * viewer first: how many were put in wids. Pop-ups and menus are not
 * among them.
 */
EXPORT INT wm_list( INT *wids, INT max )
{
	INT	i, k, n = 0, z;

	if ( wids == NULL || max <= 0 ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( z = 0; z < WM_MAX_WIN && n < max; z++ ) {
		for ( i = 0; i < WM_MAX_WIN && n < max; i++ ) {
			WMWIN	*w = &wm_win[i];

			if ( !w->used || w->z != z || !w->visible
			  || ( w->attr & WM_ATTR_POPUP ) || !( w->attr & WM_ATTR_TITLE ) ) {
				continue;
			}
			for ( k = 0; k < n && wids[k] != i + 1; k++ ) ;
			if ( k == n ) wids[n++] = i + 1;
		}
	}
	tk_unl_mtx(wm_mtxid);
	return n;
}

/* The name in a window's band: its length, or an error */
EXPORT INT wm_title( INT wid, char *buf, INT max )
{
	WMWIN	*w;
	INT	n;

	if ( buf == NULL || max <= 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	for ( n = 0; w->title[n] != 0 && n < max - 1; n++ ) {
		buf[n] = w->title[n];
	}
	buf[n] = 0;
	tk_unl_mtx(wm_mtxid);
	return n;
}

/*
 * Subordinate windows: each is kept just in front of the window it
 * belongs to, whatever is raised or lowered, as a tool panel stays over
 * the window it works. One whose window has closed is on its own.
 */
LOCAL void sub_place( WMWIN *s, WMWIN *p )
{
	INT	i, old = s->z, pz;

	if ( s->z == p->z - 1 ) {
		return;
	}
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && &wm_win[i] != s && wm_win[i].z > old ) {
			wm_win[i].z--;
		}
	}
	pz = p->z;
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && &wm_win[i] != s && wm_win[i].z >= pz ) {
			wm_win[i].z++;
		}
	}
	s->z = pz;
	if ( s->visible ) {
		gone_add(&s->outer);
	}
}

LOCAL void subs_front( void )
{
	INT	i;

	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		WMWIN	*s = &wm_win[i];
		WMWIN	*p;

		if ( !s->used || s->parent <= 0 ) {
			continue;
		}
		p = win_of(s->parent);
		if ( p == NULL || p == s ) {
			s->parent = 0;
			continue;
		}
		sub_place(s, p);
	}
}

/* wid made a subordinate window of parent (0: its own again) */
EXPORT ER wm_set_parent( INT wid, INT parent )
{
	WMWIN	*w;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL || ( parent != 0 && ( parent == wid || win_of(parent) == NULL ) ) ) {
		tk_unl_mtx(wm_mtxid);
		return E_PAR;
	}
	w->parent = parent;
	subs_front();
	popups_front();
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_raise( INT wid )
{
	WMWIN	*w;
	INT	old, i;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	old = w->z;
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && wm_win[i].z < old ) {
			wm_win[i].z++;
		}
	}
	w->z = 0;
	subs_front();
	if ( ( w->attr & WM_ATTR_POPUP ) == 0 ) {
		popups_front();
	}
	if ( old != w->z && w->visible ) {
		gone_add(&w->outer);		/* it now covers what covered it */
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_lower( INT wid )
{
	WMWIN	*w;
	INT	old, i;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	old = w->z;
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( wm_win[i].used && wm_win[i].z > old ) {
			wm_win[i].z--;
		}
	}
	w->z = wm_nwin - 1;
	subs_front();
	if ( old != w->z && w->visible ) {
		gone_add(&w->outer);		/* what it covered shows now */
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_show( INT wid, BOOL on )
{
	WMWIN	*w;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w != NULL ) {
		w->visible = on;
	}
	tk_unl_mtx(wm_mtxid);

	return ( w != NULL ) ? E_OK : E_ID;
}

EXPORT ER wm_set_scroll( INT wid, INT x, INT y )
{
	WMWIN	*w;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w != NULL ) {
		w->scroll.x = x;
		w->scroll.y = y;
	}
	tk_unl_mtx(wm_mtxid);

	return ( w != NULL ) ? E_OK : E_ID;
}

EXPORT INT wm_gid( INT wid )
{
	WMWIN	*w;
	INT	gid;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	gid = ( w != NULL ) ? w->gid : E_ID;
	tk_unl_mtx(wm_mtxid);

	return gid;
}

/* ---------------------------------------------------------------- damage */

/*
 * A window remembers one rectangle of what needs drawing again. The
 * change from nothing outstanding to something is what a program would
 * be told about; until events are carried, taking it is how a program
 * asks.
 */
/* ---------------------------------------------------------------- the frame */

/*
 * Draw a window's frame into that window's own pixels.
 *
 * The frame is the system's, not the program's: the window's own
 * drawing environment is held to the work area on purpose, so this uses
 * a second environment that the manager keeps, pointed at the same
 * pixels but allowed to reach the whole of the window.
 *
 * Every measurement comes from work_of(), the one function that knows
 * what a frame takes, so a frame drawn here can never disagree with the
 * work area a program was given.
 */
LOCAL void fill_r( INT gid, INT l, INT t, INT r, INT b, UW colour )
{
	T_DPRECT	q;

	if ( r <= l || b <= t ) {
		return;
	}
	q.left = l;  q.top = t;  q.right = r;  q.bottom = b;
	dp_fill_rect(gid, &q, colour);
}

/* The same in whatever a numbered entry of the look table holds, which
 * may be a tone of two colours rather than one colour */
LOCAL void fill_look( INT gid, INT l, INT t, INT r, INT b, UINT n )
{
	T_DPRECT	q;
	T_DPPAT		pat;

	if ( r <= l || b <= t || wm_look_pat(n, &pat) < E_OK ) {
		return;
	}
	q.left = l;  q.top = t;  q.right = r;  q.bottom = b;
	dp_fill_rect_pat(gid, &q, &pat);
}

/*
 * A scroll bar, in the window's own coordinates: the whole of it,
 * lines and all, and the stretch along which the knob runs. The bar
 * fills the strip its side of the frame would otherwise hold, less one
 * pixel for the window's outline; the knob runs one short of it at
 * each end, which is where the lines past the knob's ends go.
 *
 * FALSE when the window has no bar on that side, or is too small for
 * one.
 */
LOCAL BOOL bar_rect( CONST WMWIN *w, UINT which, T_DPRECT *r, INT *p_from,
		     INT *p_to )
{
	INT	fw = w->outer.right - w->outer.left;
	INT	fh = w->outer.bottom - w->outer.top;
	INT	wl = w->work.left - w->outer.left;
	INT	wt = w->work.top - w->outer.top;
	INT	wr = w->work.right - w->outer.left;
	INT	wb = w->work.bottom - w->outer.top;

	switch ( which ) {
	case WM_BAR_R:
		if ( (w->attr & WM_ATTR_RBAR) == 0 ) return FALSE;
		r->left = wr + 1;  r->right = fw - 1;
		r->top  = wt - 2;  r->bottom = wb + 2;
		*p_from = wt - 1;  *p_to = wb + 1;
		break;
	case WM_BAR_B:
		if ( (w->attr & WM_ATTR_BBAR) == 0 ) return FALSE;
		r->left = wl - 2;  r->right = wr + 2;
		r->top  = wb + 1;  r->bottom = fh - 1;
		*p_from = wl - 1;  *p_to = wr + 1;
		break;
	case WM_BAR_L:
		if ( (w->attr & WM_ATTR_LBAR) == 0 ) return FALSE;
		r->left = 1;       r->right = wl - 1;
		r->top  = wt - 2;  r->bottom = wb + 2;
		*p_from = wt - 1;  *p_to = wb + 1;
		break;
	default:
		return FALSE;
	}

	return ( r->right > r->left + 3 && r->bottom > r->top + 3
		 && *p_to > *p_from );
}

/*
 * Where the pieces of a bar lie along a rail that long: the gap before
 * the knob, the knob itself, and the middle of the mark that lies
 * across it.
 *
 * The knob's length is the shown part as a share of the whole, and its
 * place is how far through the rest the shown part has been wound. Left
 * at that, a long document would have a knob a pixel long, so the knob
 * is never shorter than m -- a third of the rail, and never more than
 * sixteen. That length has to come from somewhere, and two corrections
 * take it:
 *
 *   p  is what the rail gives up when the range is so narrow that the
 *      least knob would leave it no room to move in. It is put back at
 *      the end the knob is not at, so that an end of the rail still
 *      means an end of the document; when the knob is at neither end,
 *      half goes to each.
 *   q  is what the rail gives up when the shown part is not the whole,
 *      which is what pays for the knob being longer than its share.
 *
 * Both are dropped when they come out negative, which is the ordinary
 * case of a knob already longer than the least.
 *
 * The four numbers may run either way. A bar along the foot is given
 * them descending, and the same arithmetic then lays it out from the
 * left; that is what the second form of p is for.
 *
 * The products are a length times a range, in wide integers: a document
 * of a few million lines in a window a thousand pixels wide overflows a
 * narrow one.
 */
EXPORT void wm_bar_pieces( CONST T_WMBAR *bar, INT len, BOOL small_knob,
			   INT *p_off, INT *p_len, INT *p_mark )
{
	D	span, range, ahead, behind;
	D	l, m, p = 0, q = 0, lead = 0, trail = 0, mid;

	if ( bar == NULL || len <= 0 ) {
		if ( p_off != NULL )  *p_off = 0;
		if ( p_len != NULL )  *p_len = ( len > 0 ) ? len : 0;
		if ( p_mark != NULL ) *p_mark = 0;
		return;
	}
	span   = (D)bar->chi - (D)bar->clo;
	range  = (D)bar->hi - (D)bar->lo;
	ahead  = (D)bar->clo - (D)bar->lo;	/* what is behind the knob */
	behind = (D)bar->hi - (D)bar->chi;	/* what is beyond it */
	l = len;

	m = l / 3;
	if ( m > 16 ) {
		m = 16;
	}
	if ( small_knob ) {
		m = 2;
	} else if ( !( ( range >= -2 && range <= 2 ) || span == range ) ) {
		if ( range < 0 ) {
			p = ( 2 * l + m * range ) / ( range + 2 );
		} else {
			p = ( 2 * l - m * range ) / ( 2 - range );
		}
		if ( p < 0 ) {
			p = 0;
		} else {
			l -= p;
		}
	}
	if ( span != range ) {
		q = ( span * l - range * m ) / ( span - range );
		if ( q < 0 ) {
			q = 0;
		} else {
			l -= q;
		}
	}
	if ( range != 0 ) {
		trail = ( l * behind ) / range;
		lead  = ( l * ahead ) / range;
		if ( bar->clo == bar->lo ) {
			trail += p;		/* wound right back: slack beyond */
		} else if ( bar->hi == bar->chi ) {
			lead += p;		/* wound right on: slack before */
		} else {
			D	half = p / 2;

			trail += half;
			lead  += half;
		}
	}
	mid = ( p + l + q - ( lead + trail ) ) / 2;

	if ( p_off != NULL ) {
		*p_off = (INT)lead;
	}
	if ( p_len != NULL ) {
		INT	klen = (INT)( (D)len - lead - trail );

		*p_len = ( klen > 0 ) ? klen : 1;
	}
	if ( p_mark != NULL ) {
		*p_mark = (INT)( lead + mid );
	}
}

/* What the knob comes to, for whoever wants only those two numbers */
EXPORT void wm_bar_knob( CONST T_WMBAR *b, INT len, INT *p_off, INT *p_len )
{
	wm_bar_pieces(b, len, FALSE, p_off, p_len, NULL);
}

/*
 * One scroll bar, in the window's own coordinates.
 *
 * The rectangle given is the whole of the bar. Along its two long sides
 * it is lit on the near one and shaded on the far one; the ends have no
 * such line, which is what lets bars meeting at a corner run into one
 * another. Inside lies the track, which is filled a row further out at
 * each end than the rail the knob is placed along -- otherwise the row
 * at the corner keeps whatever the last knob to reach the end left
 * there.
 *
 * The knob is filled a pixel inside its place, and its two ends carry
 * four lines: shaded just before it, lit at it, shaded at its last row,
 * lit just after. The mark across the middle is filled in its own
 * colour with a shaded line before it and a lit one after, so that a
 * knob filling the whole track still shows which way it slides.
 *
 * A bar that cannot be worked -- the window does not hold the input --
 * has one colour for track and knob alike and no mark: it should not
 * look as though it can.
 *
 */
EXPORT ER wm_draw_bar( INT gid, CONST T_DPRECT *rect, CONST T_WMBAR *b,
		       BOOL across, BOOL active )
{
	T_DPRECT	r;
	INT		from, to, len, off, klen, mk, k0, k1, mid;
	UW		back, knob, mark, light, dark;

	if ( rect == NULL || b == NULL ) {
		return E_PAR;
	}
	r = *rect;
	if ( r.right <= r.left + 3 || r.bottom <= r.top + 3 ) {
		return E_OK;			/* too small to hold one */
	}
	light = wm_look(WM_LOOK_LIGHT);
	dark  = wm_look(WM_LOOK_SHADOW);
	mark  = wm_look(WM_LOOK_BAR_MARK);
	back  = active ? wm_look(WM_LOOK_BAR_BACK) : wm_look(WM_LOOK_BAR_OFF);
	knob  = active ? wm_look(WM_LOOK_BAR_KNOB) : wm_look(WM_LOOK_BAR_OFF);

	/* the rail: the bar's own length, less a row at each end */
	from = across ? r.left + 1 : r.top + 1;
	to   = across ? r.right - 1 : r.bottom - 1;
	len  = to - from;
	wm_bar_pieces(b, len, FALSE, &off, &klen, &mk);
	k0  = from + off;
	k1  = k0 + klen;
	mid = from + mk;

	if ( across ) {
		/* the long edges, and no line across either end */
		dp_line(gid, r.left, r.top, r.right - 1, r.top, light);
		dp_line(gid, r.left, r.bottom - 1, r.right - 1, r.bottom - 1, dark);

		/* the track, a row past each end of the rail */
		fill_r(gid, r.left, r.top + 1, k0 + 1, r.bottom - 1, back);
		fill_r(gid, k1 - 1, r.top + 1, r.right, r.bottom - 1, back);

		fill_r(gid, k0 + 1, r.top + 1, k1 - 1, r.bottom - 1, knob);
		dp_line(gid, k0 - 1, r.top, k0 - 1, r.bottom - 1, dark);
		dp_line(gid, k0, r.top, k0, r.bottom - 1, light);
		dp_line(gid, k1 - 1, r.top, k1 - 1, r.bottom - 1, dark);
		dp_line(gid, k1, r.top, k1, r.bottom - 1, light);
		if ( active ) {
			fill_r(gid, mid - 1, r.top, mid + 2, r.bottom, mark);
			dp_line(gid, mid - 2, r.top, mid - 2, r.bottom - 1, dark);
			dp_line(gid, mid + 2, r.top, mid + 2, r.bottom - 1, light);
		}
	} else {
		dp_line(gid, r.left, r.top, r.left, r.bottom - 1, light);
		dp_line(gid, r.right - 1, r.top, r.right - 1, r.bottom - 1, dark);

		fill_r(gid, r.left + 1, r.top, r.right - 1, k0 + 1, back);
		fill_r(gid, r.left + 1, k1 - 1, r.right - 1, r.bottom, back);

		fill_r(gid, r.left + 1, k0 + 1, r.right - 1, k1 - 1, knob);
		dp_line(gid, r.left, k0 - 1, r.right - 1, k0 - 1, dark);
		dp_line(gid, r.left, k0, r.right - 1, k0, light);
		dp_line(gid, r.left, k1 - 1, r.right - 1, k1 - 1, dark);
		dp_line(gid, r.left, k1, r.right - 1, k1, light);
		if ( active ) {
			fill_r(gid, r.left, mid - 1, r.right, mid + 2, mark);
			dp_line(gid, r.left, mid - 2, r.right - 1, mid - 2, dark);
			dp_line(gid, r.left, mid + 2, r.right - 1, mid + 2, light);
		}
	}

	return E_OK;
}

/* One of a window's own bars, in the strip its side of the frame keeps */
LOCAL void draw_bar( INT gid, CONST WMWIN *w, UINT which, BOOL active )
{
	T_DPRECT	r;
	INT		from, to;

	if ( !bar_rect(w, which, &r, &from, &to) ) {
		return;
	}
	wm_draw_bar(gid, &r, &w->bar[which], (BOOL)( which == WM_BAR_B ), active);

	/*
	 * The corner where a bar meets the frame is closed with a single
	 * shaded pixel at the head of the rail. The bar down the left side
	 * does not get one; there the frame already reaches the corner.
	 */
	if ( which != WM_BAR_L ) {
		dp_put_pixel(gid, r.left, r.top, wm_look(WM_LOOK_SHADOW));
	}
}

/*
 * The band along the top: the window's pictogram, and then its name.
 *
 * The pictogram stands half the letters' height in from the head of
 * the band, in a square box as tall as the band less its edges, and
 * the name begins four thirds of that height past where the box
 * starts. The box is there whether or not the window has a picture to put
 * in it -- a name that shifts left when a picture is missing is a
 * window that looks different for a reason that has nothing to do with
 * the window -- so a window with none of its own gets the standing
 * mark.
 *
 * It is drawn twice, the second stroke one pixel down and to the right
 * in the other colour, which is what makes it legible on a band of any
 * shade. A name too long for the band is cut where it stops fitting.
 *
 * Nothing is drawn when no system font has been set: a name drawn in
 * something the caller did not choose is worse than no name.
 */
/*
 * The icon of the object a window stands for, in the pictogram's box:
 * scaled to the box's height, the pixels the icon leaves clear left as
 * the band is under them.
 */
LOCAL void draw_icon( INT gid, CONST T_DPRECT *box, CONST UW *px, INT w, INT h )
{
	UW	*sq;
	INT	side = box->bottom - box->top, x, y;

	if ( side <= 0 || w <= 0 || h <= 0 ) {
		return;
	}
	sq = (UW *)Kmalloc((SZ)side * side * sizeof(UW));
	if ( sq == NULL ) {
		return;
	}
	for ( y = 0; y < side; y++ ) {
		for ( x = 0; x < side; x++ ) {
			sq[y * side + x] = px[(SZ)( y * h / side ) * w + x * w / side];
		}
	}
	dp_put_argb(gid, box->left, box->top, sq, side, side, side, IMG_CLEAR);
	Kfree(sq);
}

LOCAL void draw_title( INT gid, CONST WMWIN *w, INT fw )
{
	INT	th = look_len(WM_LOOK_TITLE_H, 16);
	INT	x = 2 + th / 2 + (4 * (th + 4)) / 3;
	INT	y = th + 5;			/* the baseline */
	INT	room = fw - 2 - 8 - x;
	ID	fid = fn_system();
	INT	n;

	{
		T_DPRECT	box;

		box.left   = 2 + th / 2;
		box.right  = box.left + th + 4;
		box.top    = 4;
		box.bottom = box.top + th + 4;
		if ( box.right < fw - 2 && w->icon != NULL ) {
			draw_icon(gid, &box, w->icon, w->icon_w, w->icon_h);
		} else if ( box.right < fw - 2
		  && wm_pict_draw(gid, w->pict, w->owner, &box,
				  wm_look(WM_LOOK_TITLE_TEXT)) < E_OK ) {
			wm_pict_mark(gid, &box, wm_look(WM_LOOK_TITLE_TEXT),
				     wm_look(WM_LOOK_TITLE_BACK));
		}
	}
	if ( fid <= 0 || w->title[0] == '\0' || room <= 0 ) {
		return;
	}
	fn_set_size(fid, th);
	n = fn_fit(fid, (CONST UB *)w->title, room);
	if ( n <= 0 ) {
		return;
	}
	{
		char	cut[WM_TITLE_MAX];
		INT	i;

		for ( i = 0; i < n && i < WM_TITLE_MAX - 1; i++ ) {
			cut[i] = w->title[i];
		}
		cut[i] = '\0';
		fn_draw(gid, fid, x, y, (CONST UB *)cut,
			wm_look(WM_LOOK_TITLE_BACK));
		fn_draw(gid, fid, x + 1, y + 1, (CONST UB *)cut,
			wm_look(WM_LOOK_TITLE_TEXT));
	}
}

/*
 * A window's frame, drawn into the window's own pixels.
 *
 * The outline goes round the whole window and round the work area, both
 * in the frame's own colour. Between the two lie four strips. Each is
 * built the same way: a lit line along its outer top and left edges, a
 * shaded line along the other two, and the band between them. Where a
 * scroll bar stands, the bar fills the strip instead.
 *
 * Everything is measured from the window's corner, which is what the
 * drawing environment means by a coordinate once its origin is set, and
 * every measurement comes from work_of(), so the frame drawn here can
 * never disagree with the work area a program was given.
 */
LOCAL void draw_frame( CONST WMWIN *w )
{
	INT	fw, fh, wl, wt, wr, wb, x0, y0;
	BOOL	active;
	UW	edge, light, dark;
	UINT	band;

	if ( wm_sys_gid < 0 || w->pixels == NULL ) {
		return;
	}
	if ( (w->attr & (WM_ATTR_FRAME | WM_ATTR_TITLE)) == 0 ) {
		return;				/* a window with no frame has none */
	}
	sys_lock();
	dp_set_target(wm_sys_gid, w->pixels, wm_spec.pitch,
			      w->outer.left, w->outer.top);
	dp_set_origin(wm_sys_gid, w->outer.left, w->outer.top);
	dp_set_frame(wm_sys_gid, &w->outer);

	fw = w->outer.right - w->outer.left;
	fh = w->outer.bottom - w->outer.top;
	{
		T_DPRECT	v;

		v.left = 0;  v.top = 0;  v.right = fw;  v.bottom = fh;
		dp_set_visible(wm_sys_gid, &v);
	}
	wl = w->work.left - w->outer.left;
	wt = w->work.top - w->outer.top;
	wr = w->work.right - w->outer.left;
	wb = w->work.bottom - w->outer.top;

	active = ( wm_focus_wid == (INT)(w - wm_win) + 1 );
	edge  = wm_look(WM_LOOK_FRAME);
	band  = active ? WM_LOOK_TITLE_ON : WM_LOOK_TITLE_OFF;
	light = wm_look(WM_LOOK_LIGHT);
	dark  = wm_look(WM_LOOK_SHADOW);

	/* the two outlines */
	{
		T_DPRECT	q;

		q.left = 0;  q.top = 0;  q.right = fw;  q.bottom = fh;
		dp_frame_rect(wm_sys_gid, &q, edge, 1);
		q.left = wl - 1;  q.top = wt - 1;
		q.right = wr + 1;  q.bottom = wb + 1;
		dp_frame_rect(wm_sys_gid, &q, edge, 1);
	}

	/* the strip along the top, which carries the name */
	dp_line(wm_sys_gid, 1, 1, 1, wt - 3, light);
	dp_line(wm_sys_gid, 1, 1, fw - 2, 1, light);
	dp_line(wm_sys_gid, fw - 2, 2, fw - 2, wt - 3, dark);
	dp_line(wm_sys_gid, wl - 1, wt - 2, wr + 1, wt - 2, dark);
	fill_look(wm_sys_gid, 2, 2, fw - 2, wt - 2, band);

	/* the strip on the left */
	if ( (w->attr & WM_ATTR_LBAR) != 0 ) {
		draw_bar(wm_sys_gid, w, WM_BAR_L, active);
		y0 = wb + 2;
	} else {
		dp_line(wm_sys_gid, wl - 2, wt - 2, wl - 2, wb + 1, dark);
		y0 = wt - 2;
	}
	dp_line(wm_sys_gid, 1, y0, 1, fh - 2, light);
	dp_line(wm_sys_gid, 2, fh - 2, wl - 2, fh - 2, dark);
	fill_look(wm_sys_gid, 2, y0, wl - 2, fh - 2, band);

	/* the strip along the foot */
	if ( (w->attr & WM_ATTR_BBAR) != 0 ) {
		draw_bar(wm_sys_gid, w, WM_BAR_B, active);
		x0 = wr + 2;
	} else {
		dp_line(wm_sys_gid, wl - 1, wb + 1, wr + 1, wb + 1, light);
		x0 = wl - 2;
	}
	dp_line(wm_sys_gid, x0, fh - 2, fw - 2, fh - 2, dark);
	dp_line(wm_sys_gid, fw - 2, wb + 2, fw - 2, fh - 3, dark);
	fill_look(wm_sys_gid, x0, wb + 2, fw - 2, fh - 2, band);

	/* and the one on the right */
	if ( (w->attr & WM_ATTR_RBAR) != 0 ) {
		draw_bar(wm_sys_gid, w, WM_BAR_R, active);
	} else {
		dp_line(wm_sys_gid, fw - 2, wt - 2, fw - 2, wb + 1, dark);
		dp_line(wm_sys_gid, wr + 1, wt - 1, wr + 1, wb + 1, light);
		fill_look(wm_sys_gid, wr + 2, wt - 2, fw - 2, wb + 3, band);
	}

	if ( (w->attr & WM_ATTR_TITLE) != 0 ) {
		draw_title(wm_sys_gid, w, fw);
	}
	sys_unlock();
}

/* ------------------------------------------------------------ scroll bars */

/*
 * The shape a window is laid down in. A square window has none and is
 * laid as a rectangle; a rounded one is the region of a rounded box,
 * worked out once here rather than tested corner by corner for every
 * pixel of every screen.
 */
LOCAL void shape_of( WMWIN *w )
{
	T_DPRECT	r;
	INT		rad = wm_num(WM_SET_ROUND, 0);
	T_DPRGN		*rgn = NULL;

	dp_rgn_free(w->shape);
	w->shape = NULL;
	if ( w->own != NULL ) {
		if ( dp_rgn_copy(w->own, &rgn) >= E_OK ) {
			w->shape = rgn;		/* the one it was given wins */
		}
		return;
	}
	if ( rad <= 0 ) {
		return;
	}
	r.left = 0;  r.top = 0;
	r.right = w->outer.right - w->outer.left;
	r.bottom = w->outer.bottom - w->outer.top;
	if ( dp_rgn_round(&r, rad, rad, &rgn) >= E_OK ) {
		w->shape = rgn;
	}
}


EXPORT ER wm_set_shape( INT wid, CONST T_DPRGN *rgn )
{
	WMWIN	*w;
	T_DPRGN	*copy = NULL;

	if ( rgn != NULL && dp_rgn_copy(rgn, &copy) < E_OK ) {
		return E_NOMEM;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		dp_rgn_free(copy);
		return E_ID;
	}
	dp_rgn_free(w->own);
	w->own = copy;
	shape_of(w);
	gone_add(&w->outer);			/* what showed round it shows again */
	w->dirty = TRUE;
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/* Whether a place on the screen is inside a window's outline */
LOCAL BOOL in_shape( CONST WMWIN *w, INT x, INT y )
{
	return ( w->shape == NULL
	      || dp_rgn_has(w->shape, x - w->outer.left, y - w->outer.top) );
}

/*
 * Which picture stands in the window's title band, and whose pictures
 * are looked in for it. The number alone is not enough: two programs
 * may each have a picture 3 and they are not the same picture, so the
 * owner is set with it. An owner of 0 is the system's own.
 */
/*
 * The icon that stands in the window's title band in place of a picture
 * by number: the pixels (0x00rrggbb, IMG_CLEAR where clear) are copied.
 * NULL takes it away.
 */
EXPORT ER wm_set_icon( INT wid, CONST UW *px, INT w, INT h )
{
	WMWIN	*win;
	UW	*copy = NULL, *old;

	if ( px != NULL && w > 0 && h > 0 ) {
		copy = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
		if ( copy == NULL ) {
			return E_NOMEM;
		}
		knl_memcpy(copy, px, (INT)( (SZ)w * h * sizeof(UW) ));
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	win = win_of(wid);
	if ( win == NULL ) {
		tk_unl_mtx(wm_mtxid);
		if ( copy != NULL ) Kfree(copy);
		return E_ID;
	}
	old = win->icon;
	win->icon = copy;
	win->icon_w = ( copy != NULL ) ? w : 0;
	win->icon_h = ( copy != NULL ) ? h : 0;
	draw_frame(win);
	tk_unl_mtx(wm_mtxid);
	if ( old != NULL ) Kfree(old);
	wm_composite();

	return E_OK;
}

EXPORT ER wm_set_pict( INT wid, INT num, ID owner )
{
	WMWIN	*w;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	w->pict = num;
	w->owner = owner;
	draw_frame(w);
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * A new name in the title band. It is cut at a letter's edge when it is
 * longer than a window keeps: half a letter is no letter.
 */
EXPORT ER wm_set_title( INT wid, CONST char *title )
{
	WMWIN	*w;
	INT	k;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	for ( k = 0; k < WM_TITLE_MAX - 1 && title != NULL && title[k] != '\0'; k++ ) {
		w->title[k] = title[k];
	}
	while ( k > 0 && title != NULL && title[k] != '\0'
	     && ( (UB)title[k] & 0xC0 ) == 0x80 ) {
		k--;
	}
	w->title[k] = '\0';
	draw_frame(w);
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_set_opacity( INT wid, UINT opacity )
{
	WMWIN	*w;

	if ( opacity > 255 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	w->opacity = opacity;
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_opacity( INT wid, UINT *p_opacity )
{
	WMWIN	*w;

	if ( p_opacity == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	*p_opacity = w->opacity;
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_set_tint( INT wid, UW colour, UINT strength )
{
	WMWIN	*w;

	if ( strength > 255 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	w->tint = colour;
	w->tint_str = strength;
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_set_bar( INT wid, UINT which, CONST T_WMBAR *b )
{
	WMWIN	*w;

	if ( b == NULL || which >= WM_BAR_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	w->bar[which] = *b;
	draw_frame(w);
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_bar( INT wid, UINT which, T_WMBAR *b )
{
	WMWIN	*w;

	if ( b == NULL || which >= WM_BAR_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	*b = w->bar[which];
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT INT wm_bar_at( INT wid, INT x, INT y, UINT *p_which )
{
	WMWIN		*w;
	T_DPRECT	r;
	UINT		i;
	INT		hit = WM_BARHIT_NONE;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return WM_BARHIT_NONE;
	}
	x -= w->outer.left;
	y -= w->outer.top;
	for ( i = 0; i < WM_BAR_MAX; i++ ) {
		BOOL	across;
		INT	from, to, len, off, klen, at, k0;

		if ( !bar_rect(w, i, &r, &from, &to) ) {
			continue;
		}
		if ( x < r.left || x >= r.right || y < r.top || y >= r.bottom ) {
			continue;
		}
		across = ( i == WM_BAR_B );
		len = to - from;
		wm_bar_knob(&w->bar[i], len, &off, &klen);
		at = ( across ? x : y ) - from;
		k0 = off;
		if ( at < k0 ) {
			hit = WM_BARHIT_BEFORE;
		} else if ( at >= k0 + klen ) {
			hit = WM_BARHIT_AFTER;
		} else {
			hit = WM_BARHIT_KNOB;
		}
		if ( p_which != NULL ) {
			*p_which = i;
		}
		break;
	}
	tk_unl_mtx(wm_mtxid);

	return hit;
}

EXPORT ER wm_bar_drag( INT wid, UINT which, INT x, INT y )
{
	WMWIN		*w;
	T_DPRECT	r;
	T_WMBAR		*b;
	BOOL		across;
	INT		from, to, len, off, klen, at, room, span, range, clo;

	if ( which >= WM_BAR_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL || !bar_rect(w, which, &r, &from, &to) ) {
		tk_unl_mtx(wm_mtxid);
		return ( w == NULL ) ? E_ID : E_OBJ;
	}
	b = &w->bar[which];
	span  = b->chi - b->clo;
	range = b->hi - b->lo;
	if ( range <= 0 || span >= range ) {
		tk_unl_mtx(wm_mtxid);
		return E_OK;			/* all of it is shown already */
	}
	across = ( which == WM_BAR_B );
	len = to - from;
	wm_bar_knob(b, len, &off, &klen);

	/* the knob's middle goes where the pointer is */
	at = ( across ? (x - w->outer.left) : (y - w->outer.top) ) - from;
	at -= klen / 2;
	room = len - klen;
	if ( at < 0 )    at = 0;
	if ( at > room ) at = room;

	clo = ( room > 0 ) ? b->lo + (INT)(((D)at * (range - span)) / room) : b->lo;
	b->clo = clo;
	b->chi = clo + span;
	draw_frame(w);
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_damage( INT wid, CONST T_DPRECT *r )
{
	WMWIN	*w;

	if ( r == NULL || r->right <= r->left || r->bottom <= r->top ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	if ( !w->dirty ) {
		w->damage = *r;
		w->dirty = TRUE;
	} else {
		if ( r->left   < w->damage.left )   w->damage.left   = r->left;
		if ( r->top    < w->damage.top )    w->damage.top    = r->top;
		if ( r->right  > w->damage.right )  w->damage.right  = r->right;
		if ( r->bottom > w->damage.bottom ) w->damage.bottom = r->bottom;
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_take_damage( INT wid, T_DPRECT *r )
{
	WMWIN	*w;
	ER	er;

	if ( r == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		er = E_ID;
	} else if ( !w->dirty ) {
		er = E_NOEXS;			/* nothing is outstanding */
	} else {
		*r = w->damage;
		w->dirty = FALSE;
		er = E_OK;
	}
	tk_unl_mtx(wm_mtxid);

	return er;
}

EXPORT ER wm_least( UINT attr, INT *p_wide, INT *p_high )
{
	INT	l, t, r, b;

	if ( p_wide == NULL || p_high == NULL ) {
		return E_PAR;
	}
	borders_of(attr, &l, &t, &r, &b);
	*p_wide = l + r + WORK_LEAST;
	*p_high = t + b + WORK_LEAST;

	return E_OK;
}

EXPORT INT wm_part_at( INT x, INT y, INT *p_wid, UINT *p_bar )
{
	WMWIN		*w;
	T_DPRECT	r;
	INT		wid, part = WM_PART_NONE;
	UINT		i;

	wid = wm_at(x, y);
	if ( p_wid != NULL ) {
		*p_wid = wid;
	}
	if ( wid <= 0 ) {
		return WM_PART_NONE;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return WM_PART_NONE;
	}
	if ( x >= w->work.left && x < w->work.right
	  && y >= w->work.top && y < w->work.bottom ) {
		part = WM_PART_WORK;
	} else {
		INT	wx = x - w->outer.left;
		INT	wy = y - w->outer.top;

		part = WM_PART_FRAME;
		for ( i = 0; i < WM_BAR_MAX; i++ ) {
			INT	from, to;

			if ( !bar_rect(w, i, &r, &from, &to) ) {
				continue;
			}
			if ( wx >= r.left && wx < r.right
			  && wy >= r.top && wy < r.bottom ) {
				part = WM_PART_BAR;
				if ( p_bar != NULL ) {
					*p_bar = i;
				}
				break;
			}
		}
		if ( part == WM_PART_FRAME ) {
			BOOL	right = ( x >= w->work.right );
			BOOL	foot  = ( y >= w->work.bottom );

			if ( (w->attr & WM_ATTR_RESIZE) != 0 && right && foot ) {
				part = WM_PART_GRIP_BR;
			} else if ( (w->attr & WM_ATTR_RESIZE) != 0 && right ) {
				part = WM_PART_GRIP_R;
			} else if ( (w->attr & WM_ATTR_RESIZE) != 0 && foot ) {
				part = WM_PART_GRIP_B;
			} else if ( (w->attr & WM_ATTR_TITLE) != 0
				 && y < w->work.top ) {
				INT	th = look_len(WM_LOOK_TITLE_H, 16);
				INT	pl = w->outer.left + 2 + th / 2;
				INT	pt = w->outer.top + 4;

				/* the box draw_title puts the pictogram in */
				part = ( x >= pl && x < pl + th + 4 && y >= pt && y < pt + th + 4 )
				     ? WM_PART_PICT : WM_PART_TITLE;
			}
		}
	}
	tk_unl_mtx(wm_mtxid);

	return part;
}

/*
 * A press on a window's pictogram. The window closes on the second press
 * of a double press there: the same window's pictogram pressed again
 * within the double press time (LK_DBLTIME) and no further off than a
 * double press may be (LK_DBL_W). One press alone does nothing, so a
 * slip of the pointer onto the band does not close anything.
 */
LOCAL INT	pict_wid = 0;
LOCAL INT	pict_x, pict_y;
LOCAL UD	pict_when;
LOCAL INT	pict_last_wid = 0;	/* the press asked about last, and the answer */
LOCAL UD	pict_last_when;
LOCAL BOOL	pict_last;

EXPORT BOOL wm_pict_close( INT wid, INT sx, INT sy, UD when )
{
	UD	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	INT	near = wm_num(LK_DBL_W, 10);
	BOOL	twice;

	/*
	 * One press may be asked about twice -- by the window objects'
	 * layer as it passes the press on, and by the program that draws
	 * the window -- and is one press both times.
	 */
	if ( wid == pict_last_wid && when == pict_last_when ) {
		return pict_last;
	}
	twice = (BOOL)( pict_wid == wid && when >= pict_when && when - pict_when <= gap
		     && sx - pict_x <= near && pict_x - sx <= near
		     && sy - pict_y <= near && pict_y - sy <= near );
	if ( twice ) {
		pict_wid = 0;		/* a third press starts again */
	} else {
		pict_wid = wid;
		pict_x = sx;
		pict_y = sy;
		pict_when = when;
	}
	pict_last_wid = wid;
	pict_last_when = when;
	pict_last = twice;
	return twice;
}

/* ---------------------------------------------------------------- events */

/*
 * Which window covers a place on the screen: the one nearest the viewer
 * whose outer rectangle contains it. Walking forwards through the order
 * finds that one first.
 */
EXPORT INT wm_at( INT x, INT y )
{
	WMWIN	*w;
	INT	z, found = 0;

	if ( !wm_ready ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( z = 0; z < wm_nwin; z++ ) {
		w = win_at_z(z);
		if ( w == NULL || !w->visible ) {
			continue;
		}
		if ( y >= knl_msg_top() && ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			continue;		/* the message line is over it */
		}
		if ( x >= w->outer.left && x < w->outer.right
		  && y >= w->outer.top && y < w->outer.bottom && in_shape(w, x, y) ) {
			found = (INT)(w - wm_win) + 1;
			break;
		}
	}
	tk_unl_mtx(wm_mtxid);

	return found;
}

EXPORT ER wm_focus( INT wid )
{
	WMWIN	*w, *was;
	INT	was_wid;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	was_wid = wm_focus_wid;
	if ( wid == 0 ) {
		wm_focus_wid = 0;
	} else {
		w = win_of(wid);
		if ( w == NULL ) {
			tk_unl_mtx(wm_mtxid);
			return E_ID;
		}
		wm_focus_wid = wid;
	}
	/*
	 * Both bars change: the one that had the input and the one that has
	 * it now. Drawing them here is what keeps "which window is live"
	 * something a person can see without asking.
	 */
	was = win_of(was_wid);
	if ( was != NULL && was_wid != wm_focus_wid ) {
		draw_frame(was);
		if ( was->visible ) gone_add(&was->outer);
	}
	w = win_of(wm_focus_wid);
	if ( w != NULL ) {
		draw_frame(w);
		if ( w->visible ) gone_add(&w->outer);
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT INT wm_focused( void )
{
	INT	wid;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	/* one that has been closed no longer has it */
	if ( wm_focus_wid != 0 && win_of(wm_focus_wid) == NULL ) {
		wm_focus_wid = 0;
	}
	wid = wm_focus_wid;
	tk_unl_mtx(wm_mtxid);

	return wid;
}

/* ---------------------------------------------------------------- Shift+Pause */

#define KEY_PAUSE	0x48

LOCAL BOOL	wm_pause_down = FALSE;	/* Shift+Pause held: its repeats do nothing more */

/* The window nearest the viewer that is not a popup, 0 none. Called with the lock held. */
LOCAL INT front_any( void )
{
	WMWIN	*w;
	INT	z;

	for ( z = 0; z < wm_nwin; z++ ) {
		w = win_at_z(z);
		if ( w != NULL && w->visible && ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			return (INT)( w - wm_win ) + 1;
		}
	}
	return 0;
}

/*
 * Shift+Pause: the process that made the window in front -- the one
 * with the input, or else the one nearest the viewer -- is ended as
 * ts_ter_prc ends it (knl_prc_force_end), its windows closing with it,
 * and the message line says so. A window of the system's own (the
 * desktop, the cabinets, the editors, which are kernel tasks) has no
 * process to end, and a process the system needs is not ended.
 */
LOCAL void force_quit( void )
{
	UB	name[OB_NAME_MAX + 1];
	char	msg[OB_NAME_MAX + 96];
	CONST char *fmt;
	INT	wid = wm_focused();
	ID	pid;
	ER	er;

	if ( wid == 0 ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
		wid = front_any();
		tk_unl_mtx(wm_mtxid);
	}
	pid = ( wid > 0 ) ? knl_wmobj_maker(wid) : 0;
	if ( pid <= 0 ) {
		(void)wm_msg_put("強制終了できるプロセスがありません");
		return;
	}
	(void)knl_prc_name(pid, name, sizeof(name));
	er = knl_prc_force_end(pid);
	fmt = ( er >= E_OK )   ? "%s(pid %d)を強制終了しました"
	    : ( er == E_OACV ) ? "%s(pid %d)はシステムのプロセスなので強制終了できません"
	    : ( er == E_OBJ )  ? "%s(pid %d)は終了の途中です"
	    :                    "%s(pid %d)を強制終了できませんでした";
	(void)tm_sprintf((UB *)msg, (CONST UB *)fmt, name, pid);
	(void)wm_msg_put(msg);
}

/*
 * Whether h is Shift+Pause or the let go of it, which are taken here
 * and go to no window. A key held repeats its press: only the first
 * press ends anything.
 */
LOCAL BOOL pause_key( CONST T_HIDEV *h )
{
	BOOL	was = wm_pause_down;

	if ( ( h->type != HID_EV_KEY_DOWN && h->type != HID_EV_KEY_UP ) || h->code != KEY_PAUSE ) {
		return FALSE;
	}
	if ( h->type == HID_EV_KEY_UP ) {
		wm_pause_down = FALSE;
		return was;
	}
	if ( ( h->mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) == 0 ) {
		return FALSE;
	}
	wm_pause_down = TRUE;
	if ( !was ) {
		force_quit();
	}
	return TRUE;
}

/*
 * Address one thing that happened to a window. A key goes to whoever
 * has the input; anything with a place goes to the window under that
 * place, whatever has the input. The place is then measured from that
 * window's work area, because that is what its program draws in.
 *
 * Shift+Pause is not addressed to anyone: it ends the process in front
 * (pause_key) and the next thing that happens is read instead.
 *
 * Nothing here changes the order. Pressing a button does not bring a
 * window forward: the press is carried, and coming forward is something
 * a program asks for when it decides to.
 */
EXPORT ER wm_read_event( T_WMEV *ev, TMO tmout )
{
	T_HIDEV	h;
	WMWIN	*w;
	ER	er;

	if ( ev == NULL ) {
		return E_PAR;
	}
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	/* a row waiting for its list: not waited past when it is due */
	{
		INT	left = knl_menu_wait(), blink = knl_pn_wait();

		if ( blink >= 0 && ( left < 0 || blink < left ) ) {
			left = blink;			/* a caret's blink is due */
		}
		if ( left >= 0 && ( tmout == TMO_FEVR || tmout > left ) ) {
			tmout = ( left > 0 ) ? left : 1;
		}
	}
	do {
		er = ts_hid_read(&h, tmout);
		knl_msg_tick();
		knl_menu_tick();	/* a row stayed on: its list opens */
		knl_pn_tick();		/* the caret of a panel's box blinks */
		if ( er < E_OK ) {
			return er;
		}
	} while ( pause_key(&h) );
	if ( h.type == HID_EV_BTN_DOWN ) {
		knl_msg_press(h.x, h.y);
	}
	/* the clicks a person asked for (ユーザ環境設定〈音属性〉) */
	if ( ( h.type == HID_EV_KEY_DOWN && h.code < 0xE0 && wm_num(LK_CLK_KEY, 0) != 0 )
	  || ( h.type == HID_EV_BTN_DOWN && wm_num(LK_CLK_BTN, 0) != 0 ) ) {
		(void)wm_beep(WM_BEEP_PRESS);
	} else if ( h.type == HID_EV_BTN_UP && wm_num(LK_CLK_BTN, 0) != 0 ) {
		(void)wm_beep(WM_BEEP_RELEASE);
	}
	/*
	 * The edit menus' keys laid out as Windows lays them (〈キー操作〉):
	 * Ctrl+V takes from the tray as Ctrl+Z does, Ctrl+X puts in it as
	 * Ctrl+V does, Ctrl+Z moves from it as Ctrl+X does. Ctrl+C is the
	 * same both ways.
	 */
	if ( ( h.type == HID_EV_KEY_DOWN || h.type == HID_EV_KEY_UP )
	  && ( h.mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 && wm_num(LK_EDITKEYS, 0) != 0 ) {
		switch ( h.code ) {
		case 0x19:	h.code = 0x1D;	break;	/* V as Z */
		case 0x1B:	h.code = 0x19;	break;	/* X as V */
		case 0x1D:	h.code = 0x1B;	break;	/* Z as X */
		default:	break;
		}
	}
	ev->type = h.type;
	ev->code = h.code;
	ev->mods = h.mods;
	ev->when = h.when;
	ev->x = h.x;
	ev->y = h.y;
	ev->dz = h.dz;

	if ( h.type == HID_EV_KEY_DOWN || h.type == HID_EV_KEY_UP ) {
		ev->wid = wm_focused();
	} else {
		ev->wid = wm_at(h.x, h.y);
		/*
		 * The second button anywhere is the menu, and the menu is the
		 * active window's: a press of it on the ground, where no window
		 * is, goes to that window as if made at the same place of the
		 * screen, outside its work area.
		 */
		if ( ev->wid == 0 && h.code == 1
		  && ( h.type == HID_EV_BTN_DOWN || h.type == HID_EV_BTN_UP ) ) {
			ev->wid = wm_focused();
			if ( ev->wid == 0 ) {
				/* no window has the input: the one in front answers */
				tk_loc_mtx(wm_mtxid, TMO_FEVR);
				ev->wid = front_window(0);
				tk_unl_mtx(wm_mtxid);
			}
		}
	}

	if ( ev->wid != 0 ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
		w = win_of(ev->wid);
		if ( w != NULL ) {
			ev->x -= w->work.left;
			ev->y -= w->work.top;
		}
		tk_unl_mtx(wm_mtxid);
	}
	/* to whoever asked to be told; a press on the ground too, which puts
	   a program's menu away */
	knl_wmobj_event(ev);

	return E_OK;
}

/* ---------------------------------------------------------------- screen */

/*
 * Lay the windows onto the back buffer. They go down from the back
 * forwards, so what is in front is written last. A window that some
 * single window in front of it covers entirely is not laid down at all,
 * which is what the order is for.
 */
/*
 * The screen, or a part of it, built afresh.
 *
 * 'area' is what is to be built; NULL means the whole screen. Building
 * only part of it is what makes the pointer cheap to move: the two
 * small rectangles it left and arrived at are laid again, and the rest
 * of the screen is not touched. The same is true of anything else that
 * changes a corner of the screen.
 *
 * What is laid down is the same in either case -- the ground, then the
 * windows from the back forward -- because a screen built two different
 * ways is two different screens.
 */
EXPORT ER wm_composite_at( CONST T_DPRECT *area )
{
	WMWIN		*w, *front;
	BOOL		keyed, laid;
	T_DPRECT	vis, box;
	INT		z, fz, x, y;
	UB		*back;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	back = (UB *)ts_disp_buffer();
	if ( back == NULL ) {
		return E_NOEXS;
	}
	ts_disp_hold();
	tk_loc_mtx(wm_mtxid, TMO_FEVR);

	box.left = 0;  box.top = 0;
	box.right = (INT)wm_spec.width;  box.bottom = (INT)wm_spec.height;
	if ( area != NULL ) {
		if ( area->left > box.left )     box.left = area->left;
		if ( area->top > box.top )       box.top = area->top;
		if ( area->right < box.right )   box.right = area->right;
		if ( area->bottom < box.bottom ) box.bottom = area->bottom;
		if ( box.right <= box.left || box.bottom <= box.top ) {
			tk_unl_mtx(wm_mtxid);
			ts_disp_release();
			return E_OK;		/* nothing of it is on the screen */
		}
	}

	/*
	 * The screen is built from the desk up. Laying the windows over
	 * whatever the last screen left would mix a see-through window
	 * with the picture of itself, which is a picture that never
	 * changes; and the desk has to be somewhere.
	 */
	{
		INT	sw = (INT)wm_spec.width;
		INT	wide = box.right - box.left;

		/*
		 * What was built is what has changed. Saying otherwise
		 * leaves the ground in the back buffer and never on the
		 * screen, and what the person then sees around the windows
		 * is whatever was in the card's memory when the machine
		 * started.
		 */
		{
			T_DISPRECT	all;

			all.left = box.left;  all.top = box.top;
			all.right = box.right;  all.bottom = box.bottom;
			ts_disp_damage(&all);
		}
		ground_make();
		if ( wm_ground != NULL ) {
			for ( y = box.top; y < box.bottom; y++ ) {
				knl_memcpy(back + (UBINT)y * wm_spec.pitch
					   + (SZ)box.left * sizeof(UW),
					   wm_ground + (SZ)y * sw + box.left,
					   (SZ)wide * sizeof(UW));
			}
		} else {
			UW	desk = wm_look(WM_LOOK_DESK);

			/* no room to keep it: the plain desk, laid straight */
			for ( y = box.top; y < box.bottom; y++ ) {
				UW	*p = (UW *)(back + (UBINT)y * wm_spec.pitch);

				for ( x = box.left; x < box.right; x++ ) {
					p[x] = desk;
				}
			}
		}
	}

	laid = FALSE;
	for ( z = wm_nwin - 1; z >= 0; z-- ) {
		w = win_at_z(z);
		if ( w == NULL || !w->visible ) {
			continue;
		}
		if ( !laid && ( w->attr & WM_ATTR_POPUP ) != 0 ) {
			/* the message line over the windows, under the menus */
			knl_msg_lay(back, wm_spec.pitch, &box);
			laid = TRUE;
		}
		vis = w->outer;
		if ( !rect_cut(&vis, &box) ) {
			continue;		/* no part of it is being built */
		}

		/* anything in front that covers the whole of it */
		front = NULL;
		for ( fz = z - 1; fz >= 0; fz-- ) {
			WMWIN	*f = win_at_z(fz);

			if ( f != NULL && f->visible
			  && ( f->attr & WM_ATTR_POPUP ) == 0
			  && rect_covers(&f->outer, &vis) ) {
				front = f;
				break;
			}
		}
		if ( front != NULL ) {
			wm_count.windows_skipped++;
			continue;
		}

		/*
		 * The shadow first, so that the window covers the part of
		 * it that falls under itself. It darkens what is already
		 * there, which is the windows below and the desk.
		 */
		if ( wm_num(WM_SET_SHADOW, 0) > 0 ) {
			T_DPRECT	sh = w->outer;
			INT		grow = wm_num(WM_SET_SHADOW_GROW, 2);

			sh.left   += wm_num(WM_SET_SHADOW_DX, 4) - grow;
			sh.right  += wm_num(WM_SET_SHADOW_DX, 4) + grow;
			sh.top    += wm_num(WM_SET_SHADOW_DY, 4) - grow;
			sh.bottom += wm_num(WM_SET_SHADOW_DY, 4) + grow;
			{
				T_DPRECT	screen;

				screen.left = 0;  screen.top = 0;
				screen.right = (INT)wm_spec.width;
				screen.bottom = (INT)wm_spec.height;
				if ( rect_cut(&sh, &screen) ) {
					for ( y = sh.top; y < sh.bottom; y++ ) {
						UW *d = (UW *)(back
							+ (UBINT)y * wm_spec.pitch
							+ (UBINT)sh.left * 4);

						for ( x = sh.left; x < sh.right;
						      x++, d++ ) {
							*d = mix(*d, 0,
								 (UINT)wm_num(WM_SET_SHADOW, 0));
						}
					}
				}
			}
		}

		keyed = (BOOL)( ( w->attr & WM_ATTR_POPUP ) != 0 );
		for ( y = vis.top; y < vis.bottom; y++ ) {
			UW	*src = win_pixel(w, vis.left, y);
			UW	*dst = (UW *)(back + (UBINT)y * wm_spec.pitch
					      + (UBINT)vis.left * 4);
			BOOL	in_work = ( y >= w->work.top && y < w->work.bottom );
			CONST INT *sp = NULL;
			INT	ns = 0;

			if ( w->shape != NULL ) {
				ns = dp_rgn_row(w->shape, y - w->outer.top, &sp);
				if ( ns == 0 ) {
					continue;	/* this row is past the corner */
				}
			}

			for ( x = vis.left; x < vis.right; x++, src++, dst++ ) {
				UW	c = *src;

				if ( ns > 0 && !in_span(sp, ns,
							x - w->outer.left) ) {
					continue;	/* outside the shape */
				}

				/*
				 * The work area is laid down as it is. Only
				 * the strip of frame around it is mixed with
				 * what is behind, which is what keeps this
				 * affordable: the cost is the strip, not the
				 * screen.
				 */
				if ( in_work && x >= w->work.left
				  && x < w->work.right ) {
					if ( keyed && ( c >> 24 ) == 0xFF ) {
						continue;	/* see-through */
					}
					*dst = c;
					continue;
				}
				if ( w->tint_str > 0 ) {
					c = mix(c, w->tint, w->tint_str);
				}
				*dst = ( w->opacity >= 255 )
				       ? c : mix(*dst, c, w->opacity);
			}
		}
		wm_count.windows_drawn++;

		{
			T_DISPRECT	d;

			d.left = vis.left;  d.top = vis.top;
			d.right = vis.right;  d.bottom = vis.bottom;
			ts_disp_damage(&d);
		}
	}
	if ( !laid ) {
		knl_msg_lay(back, wm_spec.pitch, &box);
	}
	/*
	 * What is being carried, over every window: the outline of each
	 * thing, drawn by turning each pixel on its edge to its opposite.
	 * It belongs to no window -- it goes wherever the hand takes it,
	 * across windows and the ground between them -- so it is laid on
	 * the screen, like the pointer, and not into any window's pixels.
	 */
	{
		INT	k;

		for ( k = 0; k < wm_ndrag; k++ ) {
			T_DPRECT	e[4], d = wm_drag[k];
			INT		j;

			e[0].left = d.left;  e[0].right = d.right;
			e[0].top = d.top;    e[0].bottom = d.top + 1;
			e[1].left = d.left;  e[1].right = d.right;
			e[1].top = d.bottom - 1;  e[1].bottom = d.bottom;
			e[2].left = d.left;  e[2].right = d.left + 1;
			e[2].top = d.top + 1;  e[2].bottom = d.bottom - 1;
			e[3].left = d.right - 1;  e[3].right = d.right;
			e[3].top = d.top + 1;  e[3].bottom = d.bottom - 1;
			for ( j = 0; j < 4; j++ ) {
				if ( !rect_cut(&e[j], &box) ) {
					continue;
				}
				for ( y = e[j].top; y < e[j].bottom; y++ ) {
					UW	*p = (UW *)(back + (UBINT)y * wm_spec.pitch);

					for ( x = e[j].left; x < e[j].right; x++ ) {
						p[x] ^= 0x00FFFFFFU;
					}
				}
			}
		}
	}

	/*
	 * The pointer goes on last, over the windows. Nothing under it is
	 * kept: the next screen is built from the ground up, so what it
	 * covered is drawn again anyway.
	 *
	 * The system's drawing environment is pointed back at the screen
	 * first. It spends the rest of its life aimed at one window's
	 * pixels or another's, and the pointer belongs to no window.
	 */
	if ( wm_sys_gid >= 0 ) {
		T_DPRECT	screen;

		screen.left = 0;  screen.top = 0;
		screen.right = (INT)wm_spec.width;
		screen.bottom = (INT)wm_spec.height;
		sys_lock();
		dp_set_target(wm_sys_gid, NULL, wm_spec.pitch, 0, 0);
		dp_set_origin(wm_sys_gid, 0, 0);
		dp_set_frame(wm_sys_gid, &screen);
		dp_set_visible(wm_sys_gid, &screen);
		wm_draw_pointer(wm_sys_gid);
		sys_unlock();
	}

	wm_count.composites++;
	tk_unl_mtx(wm_mtxid);
	ts_disp_release();

	return E_OK;
}

EXPORT ER wm_composite( void )
{
	wm_gone_any = FALSE;		/* all of it is built */

	return wm_composite_at(NULL);
}

/* Round a set of rectangles, into what is given; FALSE for none */
LOCAL BOOL drag_bound( CONST T_DPRECT *r, INT n, T_DPRECT *out )
{
	INT	i;

	if ( n <= 0 ) {
		return FALSE;
	}
	*out = r[0];
	for ( i = 1; i < n; i++ ) {
		if ( r[i].left < out->left )     out->left = r[i].left;
		if ( r[i].top < out->top )       out->top = r[i].top;
		if ( r[i].right > out->right )   out->right = r[i].right;
		if ( r[i].bottom > out->bottom ) out->bottom = r[i].bottom;
	}

	return TRUE;
}

/*
 * The outlines of what is being carried, in screen coordinates, or none
 * (n 0). Where they were and where they are now are built again, and
 * nothing else.
 */
EXPORT ER wm_set_drag( CONST T_DPRECT *r, INT n )
{
	T_DPRECT	was, now, all;
	BOOL		had, has;
	INT		i;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( n < 0 || n > WM_DRAG_MAX || ( n > 0 && r == NULL ) ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	had = drag_bound(wm_drag, wm_ndrag, &was);
	for ( i = 0; i < n; i++ ) {
		wm_drag[i] = r[i];
	}
	wm_ndrag = n;
	has = drag_bound(wm_drag, wm_ndrag, &now);
	tk_unl_mtx(wm_mtxid);

	if ( !had && !has ) {
		return E_OK;
	}
	all = had ? was : now;
	if ( had && has ) {
		if ( now.left < all.left )     all.left = now.left;
		if ( now.top < all.top )       all.top = now.top;
		if ( now.right > all.right )   all.right = now.right;
		if ( now.bottom > all.bottom ) all.bottom = now.bottom;
	}

	return wm_composite_at(&all);
}

/*
 * The pointer moved: the place it left and the place it has arrived at
 * are built again, and nothing else is.
 *
 * A screen is a megapixel and the pointer is a few hundred, so the
 * difference between this and building the screen is the difference
 * between a pointer that follows the hand and one that catches up
 * afterwards. The two rectangles are grown by a couple of pixels
 * because a pointer's shape carries an outline a pixel outside itself.
 */
EXPORT ER wm_pointer_moved( INT to_x, INT to_y )
{
	T_DPRECT	r, to;
	INT		from_x = 0, from_y = 0, from_side = WM_POINTER_SIDE;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	/*
	 * Where it is coming from is known here, not by the caller: this
	 * layer is what drew it. Before it has been drawn at all there is
	 * nothing to repair but everything to lay, so the whole screen is
	 * built.
	 */
	if ( !wm_pointer_last(&from_x, &from_y, &from_side) ) {
		return wm_composite();
	}
	/*
	 * One rectangle around both places rather than two. They are
	 * nearly always a few pixels apart, so one covers little more
	 * than the two would and costs one pass instead of two; a pointer
	 * that has jumped across the screen is rare enough to pay for.
	 * Where it was laid is its corner; where it goes is its point.
	 */
	wm_pointer_box(to_x, to_y, &to);
	if ( from_x == to.left && from_y == to.top && from_side == to.right - to.left ) {
		return E_OK;
	}
	/* the old square as large as it was laid, the new one as large as it is now */
	r.left   = ( from_x < to.left ) ? from_x : to.left;
	r.top    = ( from_y < to.top ) ? from_y : to.top;
	r.right  = from_x + from_side;
	r.bottom = from_y + from_side;
	if ( to.right > r.right )   r.right = to.right;
	if ( to.bottom > r.bottom ) r.bottom = to.bottom;

	return wm_composite_at(&r);
}

/*
 * Whatever has changed since the screen was last built, built again,
 * and only that: the parts of windows that were drawn in and said so,
 * and the pointer's old and new places. A menu whose lit row follows
 * the pointer changes a row at a time, and building a megapixel for
 * each row is what makes a menu fall seconds behind the hand.
 */
EXPORT ER wm_update( void )
{
	T_DPRECT	all, r;
	BOOL		any = FALSE;
	INT		i, px = 0, py = 0, lx = 0, ly = 0, lside = WM_POINTER_SIDE;
	UINT		btn = 0;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		WMWIN	*w = &wm_win[i];

		if ( !w->used || !w->dirty ) {
			continue;
		}
		w->dirty = FALSE;
		if ( !w->visible ) {
			continue;
		}
		r.left   = w->work.left + w->damage.left;
		r.top    = w->work.top + w->damage.top;
		r.right  = w->work.left + w->damage.right;
		r.bottom = w->work.top + w->damage.bottom;
		if ( !any ) {
			all = r;
			any = TRUE;
		} else {
			if ( r.left < all.left )     all.left = r.left;
			if ( r.top < all.top )       all.top = r.top;
			if ( r.right > all.right )   all.right = r.right;
			if ( r.bottom > all.bottom ) all.bottom = r.bottom;
		}
	}
	if ( wm_gone_any ) {
		if ( !any ) {
			all = wm_gone;
			any = TRUE;
		} else {
			if ( wm_gone.left < all.left )     all.left = wm_gone.left;
			if ( wm_gone.top < all.top )       all.top = wm_gone.top;
			if ( wm_gone.right > all.right )   all.right = wm_gone.right;
			if ( wm_gone.bottom > all.bottom ) all.bottom = wm_gone.bottom;
		}
		wm_gone_any = FALSE;
	}
	tk_unl_mtx(wm_mtxid);

	if ( !wm_pointer_last(&lx, &ly, &lside) ) {
		return wm_composite();		/* never laid: lay everything */
	}
	{
		T_DPRECT	to;

		if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
			wm_pointer_box(px, py, &to);
		} else {
			to.left = lx;  to.top = ly;
			to.right = lx + lside;
			to.bottom = ly + lside;
		}
		r.left = ( lx < to.left ) ? lx : to.left;
		r.top  = ( ly < to.top ) ? ly : to.top;
		r.right  = lx + lside;
		r.bottom = ly + lside;
		if ( to.right > r.right )   r.right = to.right;
		if ( to.bottom > r.bottom ) r.bottom = to.bottom;
		px = to.left;
		py = to.top;
		if ( to.right - to.left != lside ) {
			px = lx - 1;		/* the same place at another size is a change */
		}
	}
	if ( !any ) {
		if ( lx == px && ly == py ) {
			return E_OK;		/* nothing changed at all */
		}
		all = r;
	} else {
		if ( r.left < all.left )     all.left = r.left;
		if ( r.top < all.top )       all.top = r.top;
		if ( r.right > all.right )   all.right = r.right;
		if ( r.bottom > all.bottom ) all.bottom = r.bottom;
	}

	return wm_composite_at(&all);
}

/* ---------------------------------------------------------------- checking */

EXPORT INT wm_self_check( void )
{
	T_DPRECT	work;
	INT		faults = 0;
	INT		i, z, seen;

	if ( !wm_ready ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);

	/* the order is a permutation of 0..n-1 */
	for ( z = 0; z < wm_nwin; z++ ) {
		seen = 0;
		for ( i = 0; i < WM_MAX_WIN; i++ ) {
			if ( wm_win[i].used && wm_win[i].z == z ) {
				seen++;
			}
		}
		if ( seen != 1 ) {
			faults++;
		}
	}
	for ( i = 0; i < WM_MAX_WIN; i++ ) {
		if ( !wm_win[i].used ) {
			continue;
		}
		if ( wm_win[i].z < 0 || wm_win[i].z >= wm_nwin ) {
			faults++;
		}

		/* the work area is what the one function says it is */
		work_of(&wm_win[i].outer, wm_win[i].attr, &work);
		if ( work.left != wm_win[i].work.left
		  || work.top != wm_win[i].work.top
		  || work.right != wm_win[i].work.right
		  || work.bottom != wm_win[i].work.bottom ) {
			faults++;
		}

		/* the surface is large enough for the window it belongs to */
		if ( wm_win[i].pix_pages < win_pages(&wm_win[i].outer) ) {
			faults++;
		}
		if ( wm_win[i].pixels == NULL ) {
			faults++;
		}
	}
	tk_unl_mtx(wm_mtxid);

	return faults;
}

EXPORT void wm_screen( INT *p_w, INT *p_h )
{
	*p_w = wm_ready ? (INT)wm_spec.width : 0;
	*p_h = wm_ready ? (INT)wm_spec.height : 0;
}

EXPORT ER wm_copy_screen( UW *dst, INT pitch )
{
	UB	*back;
	INT	y;

	if ( !wm_ready || dst == NULL || pitch < (INT)wm_spec.width ) {
		return E_PAR;
	}
	back = (UB *)ts_disp_buffer();
	if ( back == NULL ) {
		return E_NOEXS;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( y = 0; y < (INT)wm_spec.height; y++ ) {
		knl_memcpy(dst + (SZ)y * pitch, back + (UBINT)y * wm_spec.pitch,
			   (SZ)wm_spec.width * sizeof(UW));
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_stat( T_WMSTAT *st )
{
	if ( !wm_ready || st == NULL ) {
		return E_NOEXS;
	}
	*st = wm_count;

	return E_OK;
}

/* The screen came to another size: what is kept of it taken again, and all of it laid */
EXPORT void knl_wm_screen_changed( void )
{
	if ( !wm_ready ) {
		return;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	(void)ts_disp_ref(&wm_spec);
	tk_unl_mtx(wm_mtxid);
	wm_composite();
}

/* ---------------------------------------------------------------- start-up */

EXPORT INT knl_wm_init( void )
{
	T_CMTX	cmtx;

	if ( wm_ready ) {
		return 1;
	}
	if ( ts_disp_ref(&wm_spec) < E_OK ) {
		return 0;			/* no screen to put windows on */
	}
	knl_look_init();			/* the numbers before anything reads them */
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	wm_mtxid = tk_cre_mtx(&cmtx);
	if ( wm_mtxid <= 0 ) {
		return E_LIMIT;
	}
	wm_sys_mtxid = tk_cre_mtx(&cmtx);
	if ( wm_sys_mtxid <= 0 ) {
		tk_del_mtx(wm_mtxid);
		wm_mtxid = 0;
		return E_LIMIT;
	}
	/*
	 * One environment for the frames of every window. It is retargeted
	 * at each window's own pixels as that window's frame is drawn,
	 * which is why one is enough.
	 */
	wm_sys_gid = dp_open();
	if ( wm_sys_gid < 0 ) {
		tk_del_mtx(wm_sys_mtxid);
		tk_del_mtx(wm_mtxid);
		wm_sys_mtxid = 0;
		wm_mtxid = 0;
		return E_LIMIT;
	}
	wm_ready = TRUE;

	return 1;
}
