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
#include <ts/conf.h>
#include <ts/time.h>
#include "sysman/pfalloc.h"
#include <ts/fquit.h>
#include <ts/lockup.h>
#include "wmobj.h"
#include "compose.h"

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

/* A surface a process draws a window's work area in (wm_surf_get) */
typedef struct {
	UB		*kva;		/* NULL: none */
	UD		pages;
	UBINT		uva;		/* where its process has it */
	ID		pid;
	INT		w, h;		/* pixels; rows w long */
	UW		serial;
} WMSURF;

typedef struct {
	BOOL		used;
	UINT		attr;
	T_DPRECT	outer;		/* screen coordinates */
	/*
	 * Where the window was asked to stand: the rectangle it was opened
	 * with or its program placed it at (wm_place), or the one the person
	 * moved it to or gave it. A screen too small for it makes outer
	 * smaller or moves it; a screen with room for it again gives it this
	 * back (outer_on_screen). A move that keeps the size of a window the
	 * screen made smaller moves this along and keeps its size.
	 */
	T_DPRECT	want;
	T_DPRECT	work;		/* worked out from outer and attr */
	T_DPPOINT	scroll;
	INT		gid;
	INT		z;		/* 0 is nearest the viewer */
	INT		parent;		/* a subordinate window's window, kept just in front of it; 0 none */
	BOOL		visible;
	/*
	 * A subordinate window put out of sight because its group is not the
	 * one in use (subs_away): shown as far as its program knows, and
	 * shown again when its group is. visible is FALSE while it is.
	 */
	BOOL		away;
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
	T_DPRGN		*shape_dev;	/* the outline it was given, in its pixels where the scale is not 100 */
	UINT		opacity;	/* the program's own: 255, nothing behind it shows */
	UW		tint;		/* laid over the frame before mixing */
	UINT		tint_str;	/* 0: no tint at all */
	UD		mo_t0;		/* when its opening began (ms) */
	UINT		mo_dur;		/* and how long it takes; 0: not opening */
	UW		serial;		/* which opening of its number this is */
	WMSURF		surf;		/* what its process draws in, if any */
	ID		mktsk;		/* the task that opened it */
	ID		mkpid;		/* and its process, 0 none */
	INT		wid;		/* its number: its place in the table, from 1 */
	/* what a change of the screen keeps of it between laying the windows and telling their programs */
	UB		*chg_old;
	UD		chg_oldpages;
	BOOL		chg_resized, chg_moved;
} WMWIN;

/*
 * The windows, by number - 1: as many as are open at once. Each is
 * allocated once and never freed, and keeps its number; the table of
 * them grows twice as large when every one is in use, made whole before
 * it is put in place (the one before is kept), so that whoever holds a
 * window, or walks the table without the lock, finds it where it was.
 * The number goes into a window's handle (wmobj.c) in 28 bits.
 */
#define WM_WIN_FIRST	64

LOCAL WMWIN		**wm_wtbl = NULL;
LOCAL INT		wm_wcap = 0;

#define WMW(i)		( *wm_wtbl[(i)] )

EXPORT INT knl_wm_nwin( void )
{
	return wm_wcap;
}

/* Twice as many windows, with the lock held: FALSE for no memory, or no more numbers */
LOCAL BOOL win_grow( void )
{
	INT	cap = ( wm_wcap > 0 ) ? wm_wcap * 2 : WM_WIN_FIRST, i;
	WMWIN	**t, *e;

	if ( cap > WM_WIN_LAST ) {
		cap = WM_WIN_LAST;
	}
	if ( cap <= wm_wcap ) {
		return FALSE;
	}
	t = (WMWIN **)Kmalloc(sizeof(WMWIN *) * (SZ)cap);
	e = (WMWIN *)Kcalloc((SZ)( cap - wm_wcap ), sizeof(WMWIN));
	if ( t == NULL || e == NULL ) {
		if ( t != NULL ) Kfree(t);
		if ( e != NULL ) Kfree(e);
		return FALSE;
	}
	for ( i = 0; i < cap; i++ ) {
		t[i] = ( i < wm_wcap ) ? wm_wtbl[i] : &e[i - wm_wcap];
		t[i]->wid = i + 1;
	}
	__sync_synchronize();
	wm_wtbl = t;
	__sync_synchronize();
	wm_wcap = cap;
	return TRUE;
}
LOCAL UW		wm_win_serial = 0;	/* counted up as windows open */

/* The outlines of what is being carried, on the screen */
LOCAL T_DPRECT		wm_drag[WM_DRAG_MAX];
LOCAL INT		wm_ndrag = 0;
/*
 * What each carried thing looks like (wm_set_drag_from), taken off the
 * screen where it lay when the carry began; NULL draws its outline. A
 * picture is laid only where the outline given is of its size.
 */
#define WM_DRAG_PX_MAX	( 4 * 1024 * 1024 )	/* pixels of all of them together */
LOCAL UW		*wm_dpx[WM_DRAG_MAX];
LOCAL INT		wm_dpw[WM_DRAG_MAX], wm_dph[WM_DRAG_MAX];

/* The pictures of the carried things let go (under the lock) */
LOCAL void drag_pict_free( void )
{
	INT	i;

	for ( i = 0; i < WM_DRAG_MAX; i++ ) {
		if ( wm_dpx[i] != NULL ) {
			Kfree(wm_dpx[i]);
			wm_dpx[i] = NULL;
		}
	}
}

/* Whether carried thing k is laid as its picture (under the lock) */
LOCAL BOOL drag_pict( INT k )
{
	return (BOOL)( wm_dpx[k] != NULL && wm_drag[k].right - wm_drag[k].left == wm_dpw[k]
		    && wm_drag[k].bottom - wm_drag[k].top == wm_dph[k] );
}
/* A picture laid over every window (wm_set_loupe), NULL none */
LOCAL UW		*wm_loupe = NULL;
LOCAL T_DPRECT		wm_loupe_at;
LOCAL BOOL		wm_loupe_on = FALSE;
LOCAL INT		wm_nwin = 0;
LOCAL BOOL		wm_ready = FALSE;
LOCAL ID		wm_mtxid = 0;
LOCAL T_DISPSPEC	wm_spec;

/*
 * The scale the screen is drawn at (ts_disp_setscale) and the view the
 * windows are laid in. Everything a window is -- where it stands, its
 * work area, its damage, where it is pressed -- is in view pixels; the
 * pixels of a window, the back buffer and what the compositor is handed
 * are the screen's. A window's view pixel x lies at the window's pixel
 * floor((x - outer.left) * scale / 100), and the window's pixels stand
 * on the screen from floor(outer.left * scale / 100). A row of a
 * window's pixels is wm_wpitch bytes: the screen's, or more where a
 * window as wide as the view is wider in pixels than the screen (the
 * view is rounded up). At 100 all of it is the screen as it is.
 */
LOCAL UINT		wm_scale = 100;
LOCAL INT		wm_vw = 0, wm_vh = 0;
LOCAL UINT		wm_wpitch = 0;

IMPORT void knl_pt_scale( UINT scale );

LOCAL void view_take( void )
{
	UINT	need;

	wm_scale = ( wm_spec.scale >= 100 ) ? wm_spec.scale : 100;
	wm_vw = ( wm_spec.view_w > 0 ) ? (INT)wm_spec.view_w : (INT)wm_spec.width;
	wm_vh = ( wm_spec.view_h > 0 ) ? (INT)wm_spec.view_h : (INT)wm_spec.height;
	need = (UINT)disp_dev_of(wm_vw, wm_scale) * 4;
	wm_wpitch = ( need > wm_spec.pitch ) ? ( ( need + 63 ) & ~63U ) : wm_spec.pitch;
	(void)knl_look_setting(LK_SCALE, wm_scale);
	knl_pt_scale(wm_scale);
}

/* A length or a place in view pixels as pixels of the screen */
LOCAL INT dv( INT v )
{
	return disp_dev_of(v, wm_scale);
}

/* Where a window standing at view rectangle o stands on the screen, in pixels */
LOCAL void win_dev( CONST T_DPRECT *o, T_DPRECT *d )
{
	d->left = dv(o->left);
	d->top = dv(o->top);
	d->right = d->left + dv(o->right - o->left);
	d->bottom = d->top + dv(o->bottom - o->top);
}

/* A view rectangle within a window at o, standing at d, as pixels of the screen */
LOCAL void in_win_dev( CONST T_DPRECT *o, CONST T_DPRECT *d, CONST T_DPRECT *r, T_DPRECT *out )
{
	out->left = d->left + dv(r->left - o->left);
	out->top = d->top + dv(r->top - o->top);
	out->right = d->left + dv(r->right - o->left);
	out->bottom = d->top + dv(r->bottom - o->top);
}

/*
 * A view rectangle of the screen as the pixels of the screen it may
 * touch: a pixel more on each side, since a window's pixels are placed
 * from its own corner and may lie one pixel off the screen's reckoning
 */
LOCAL void scr_dev( CONST T_DPRECT *r, T_DPRECT *d )
{
	if ( wm_scale == 100 ) {
		*d = *r;
		return;
	}
	d->left = dv(r->left) - 1;
	d->top = dv(r->top) - 1;
	d->right = dv(r->right) + 1;
	d->bottom = dv(r->bottom) + 1;
}
LOCAL T_WMSTAT		wm_count;
LOCAL INT		wm_focus_wid = 0;
LOCAL INT		wm_lit_wid = 0;		/* shown as having the input (wm_lit), 0 none */
LOCAL INT		wm_pop_from = 0;	/* the window a pop-up with the input took it from, 0 none */
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
IMPORT BOOL knl_msg_mode_key( UINT key, UINT mods );
IMPORT void knl_msg_restyle( void );
IMPORT void knl_msg_screen_changed( void );
IMPORT INT  knl_wm_volume_key( UINT code );	/* beep.c */

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
LOCAL UW		wm_ground_desk = 0;	/* the desk's colour it was worked out with */
LOCAL UINT		wm_ground_gen = 0;	/* counted up each time it is worked out */
LOCAL BOOL		wm_ground_told = FALSE;	/* a ground that could not be made, said once */

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
	INT	x, y, ox = 0, oy = 0, i, vx, vy, vw, vh;
	SZ	want;

	if ( wm_ground != NULL && wm_ground_w == sw && wm_ground_h == sh
	  && wm_ground_desk == desk ) {
		return;
	}
	ground_drop();
	wm_ground_desk = desk;
	if ( sw <= 0 || sh <= 0 ) {
		return;
	}
	want = (SZ)sw * sh * sizeof(UW);
	wm_ground_pages = (UD)( ( want + 4095 ) / 4096 );
	wm_ground = (UW *)knl_vmap((UD)wm_ground_pages, 0);
	if ( wm_ground == NULL ) {
		wm_ground_pages = 0;
		if ( !wm_ground_told ) {
			wm_ground_told = TRUE;
			tm_printf((UB *)"wm: no room for the ground of %d x %d (%ld pages): the desk is laid without the wallpaper\n",
				  sw, sh, (D)( ( want + 4095 ) / 4096 ));
		}
		return;				/* drawn the long way this time */
	}
	wm_ground_w = sw;
	wm_ground_h = sh;
	wm_ground_gen++;

	/* a picture laid as it is, or in tiles, is laid in view pixels, each as large as the scale makes it */
	for ( i = 0; ts_disp_screen(i, &vx, &vy, &vw, &vh) >= E_OK; i++ ) {
		INT	dx0 = disp_dev_of(vx, wm_scale), dw = disp_dev_of(vx + vw, wm_scale) - dx0;

		ox = oy = 0;
		if ( wm_wall != NULL && wm_wall_mode == WM_WALL_CENTRE ) {
			ox = (vw - wm_wall_w) / 2;
			oy = (vh - wm_wall_h) / 2;
		}
		for ( y = 0; y < sh; y++ ) {
			UW	*p = wm_ground + (SZ)y * sw + dx0;

			if ( wm_wall == NULL ) {
				for ( x = 0; x < dw && dx0 + x < sw; x++ ) {
					*p++ = desk;
				}
				continue;
			}
			for ( x = 0; x < dw && dx0 + x < sw; x++, p++ ) {
				INT	wx, wy;

				switch ( wm_wall_mode ) {
				case WM_WALL_CENTRE:
					wx = disp_view_of(x, wm_scale) - ox;
					wy = disp_view_of(y, wm_scale) - oy;
					if ( wx < 0 || wx >= wm_wall_w
					  || wy < 0 || wy >= wm_wall_h ) {
						*p = desk;
						continue;
					}
					break;
				case WM_WALL_FIT:
					wx = ( dw > 0 ) ? x * wm_wall_w / dw : 0;
					wy = ( sh > 0 ) ? y * wm_wall_h / sh : 0;
					break;
				default:
					wx = disp_view_of(x, wm_scale) % wm_wall_w;
					wy = disp_view_of(y, wm_scale) % wm_wall_h;
					break;
				}
				*p = wm_wall[(SZ)wy * wm_wall_w + wx];
			}
		}
	}
}

LOCAL void draw_frame( CONST WMWIN *w );
LOCAL void gone_add( CONST T_DPRECT *r );
LOCAL void gone_all( void );
LOCAL void shape_of( WMWIN *w );

/*
 * One of the windows' look (LK_LOOK..LK_OPA_WHOLE) set, within the range
 * the machine's settings give it (cf_look_items): 1 when it changed, 0
 * when it already was that, or an error. The shapes follow a change of
 * the corners and of the switch; what is on the screen is built again
 * by the caller, once for however many were set.
 */
EXPORT ER knl_wm_look( UINT num, INT value )
{
	CONST CF_ITEM	*it = cf_look_look(num);
	INT		i;

	if ( it == NULL || value < it->lo || value > it->hi ) {
		return E_PAR;
	}
	if ( wm_num(num, it->dflt) == value ) {
		return 0;
	}
	if ( !wm_ready ) {
		(void)knl_look_setting(num, (UW)value);
		return 1;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	(void)knl_look_setting(num, (UW)value);
	if ( num == LK_ROUND || num == LK_LOOK ) {
		for ( i = 0; i < wm_wcap; i++ ) {
			if ( WMW(i).used ) {
				shape_of(&WMW(i));
			}
		}
	}
	if ( ( num == LK_BLUR || num == LK_LOOK ) && value == 0 ) {
		cp_release();			/* the room the spread kept */
	}
	gone_all();
	tk_unl_mtx(wm_mtxid);

	return 1;
}

EXPORT ER wm_set( UINT num, INT value )
{
	ER	er;

	if ( cf_look_look(num) != NULL ) {
		er = knl_wm_look(num, value);
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( value < 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	er = wm_set_look(num, (UW)value);
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
	gone_all();
	for ( i = 0; i < wm_wcap; i++ ) {
		WMWIN	*w = &WMW(i);

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
	if ( wm_ready ) gone_all();
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/* ---------------------------------------------------------------- shapes */

LOCAL WMWIN *win_of( INT wid )
{
	if ( wid < 1 || wid > wm_wcap ) {
		return NULL;
	}
	if ( !WMW(wid - 1).used ) {
		return NULL;
	}

	return &WMW(wid - 1);
}

/*
 * The window whose band is drawn as the band of the window with the
 * input: one lit (wm_lit); else, while a pop-up has the input -- a
 * panel of a program's that asks something, in a window of its own
 * with no band -- the window it took the input from, as when the
 * system's own panel stands over its owner and the owner keeps the
 * input; else the window with the input. Called with the lock held.
 */
LOCAL INT band_wid( void )
{
	WMWIN	*f;

	if ( wm_lit_wid != 0 ) {
		return wm_lit_wid;
	}
	f = win_of(wm_focus_wid);
	if ( f != NULL && ( f->attr & WM_ATTR_POPUP ) != 0 && win_of(wm_pop_from) != NULL ) {
		return wm_pop_from;
	}
	return wm_focus_wid;
}

/* The bands drawn again when band_wid changed from before. Called with the lock held. */
LOCAL void band_redraw( INT before )
{
	INT	now = band_wid(), k, ids[2];
	WMWIN	*w;

	if ( now == before ) {
		return;
	}
	ids[0] = before;
	ids[1] = now;
	for ( k = 0; k < 2; k++ ) {
		w = win_of(ids[k]);
		if ( w != NULL ) {
			draw_frame(w);
			if ( w->visible ) gone_add(&w->outer);
		}
	}
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

LOCAL BOOL rect_meets( CONST T_DPRECT *a, CONST T_DPRECT *b )
{
	return ( a->left < b->right && b->left < a->right
	      && a->top < b->bottom && b->top < a->bottom );
}

/* ---------------------------------------------------------------- parts of the screen */

/*
 * What is to be built, as a few separate rectangles rather than one
 * round all of them: a caret blinking at the top left and a window
 * drawn in at the bottom right are two small parts, not the screen
 * between them. Two that overlap or lie close together are joined when
 * the rectangle round both is at most WM_PART_SLACK pixels larger than
 * the two are, because each part built costs a walk of the windows
 * besides its pixels. When the list is full, a new rectangle is joined
 * to the one it adds least to. Parts may overlap: each is built from
 * the ground up, so a pixel built twice comes out the same.
 */
#define WM_PARTS	16
#define WM_PART_SLACK	8192

typedef struct {
	INT		n;
	T_DPRECT	r[WM_PARTS];
} PARTS;

LOCAL UD part_area( CONST T_DPRECT *r )
{
	return (UD)( r->right - r->left ) * (UD)( r->bottom - r->top );
}

/* The rectangle round a and b, and how many pixels it holds that neither of them does */
LOCAL UD part_waste( CONST T_DPRECT *a, CONST T_DPRECT *b, T_DPRECT *u )
{
	T_DPRECT	o = *a;
	UD		both = part_area(a) + part_area(b);

	u->left   = ( a->left < b->left ) ? a->left : b->left;
	u->top    = ( a->top < b->top ) ? a->top : b->top;
	u->right  = ( a->right > b->right ) ? a->right : b->right;
	u->bottom = ( a->bottom > b->bottom ) ? a->bottom : b->bottom;
	if ( rect_cut(&o, b) ) {
		both -= part_area(&o);
	}
	return part_area(u) - both;
}

LOCAL void parts_add( PARTS *s, CONST T_DPRECT *r0 )
{
	T_DPRECT	r = *r0, u;
	UD		w, best_w;
	INT		i, best;

	if ( r.right <= r.left || r.bottom <= r.top ) {
		return;
	}
	for ( ;; ) {
		for ( i = 0; i < s->n; i++ ) {
			if ( part_waste(&s->r[i], &r, &u) <= WM_PART_SLACK ) {
				break;
			}
		}
		if ( i == s->n ) {
			if ( s->n < WM_PARTS ) {
				s->r[s->n++] = r;
				return;
			}
			best = 0;
			best_w = part_waste(&s->r[0], &r, &u);
			for ( i = 1; i < s->n; i++ ) {
				w = part_waste(&s->r[i], &r, &u);
				if ( w < best_w ) {
					best_w = w;
					best = i;
				}
			}
			i = best;
			(void)part_waste(&s->r[i], &r, &u);
		}
		r = u;				/* joined, and tried against the rest again */
		s->r[i] = s->r[--s->n];
	}
}

/* ---------------------------------------------------------------- pixels */

/*
 * A window's own surface: its rows as long as the screen's, each pixel
 * found from the window's corner (compose.c reads it so).
 */

LOCAL UD win_pages( CONST T_DPRECT *outer )
{
	UD	h = (UD)dv(outer->bottom - outer->top);

	return ((UD)wm_wpitch * h + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* ---------------------------------------------------------------- order */

/* The window at a given place in the order, or NULL */
LOCAL WMWIN *win_at_z( INT z )
{
	INT	i;

	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && WMW(i).z == z ) {
			return &WMW(i);
		}
	}

	return NULL;
}

/* Close the gap a window left behind when it was removed from the order */
LOCAL void z_close_gap( INT gone )
{
	INT	i;

	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && WMW(i).z > gone ) {
			WMW(i).z--;
		}
	}
}

/* ---------------------------------------------------------------- the look */

/*
 * The windows' look as it stands (LK_LOOK..LK_OPA_WHOLE), read once for
 * each screen built rather than for every window of it. With the look
 * turned off every one of them reads as nothing: solid, square, no
 * wash, no shadow, no spread, no motion.
 */
typedef struct {
	UINT	opa;			/* how solid a frame is */
	BOOL	whole;			/* the work area as well */
	UW	wash, bar_wash;
	UINT	wash_str, bar_str;
	INT	round;
	INT	dx, dy, size;
	UINT	dark;
	INT	blur;
	INT	motion;
} LOOKNOW;

LOCAL INT within( INT v, INT lo, INT hi )
{
	return ( v < lo ) ? lo : ( v > hi ) ? hi : v;
}

LOCAL void look_now( LOOKNOW *l )
{
	UINT	from = (UINT)wm_num(LK_FROM, 1);

	knl_memset(l, 0, sizeof(*l));
	l->opa = 255;
	if ( wm_num(LK_LOOK, 1) == 0 ) {
		return;
	}
	l->opa = (UINT)within(wm_num(LK_OPACITY, 255), 0, 255);
	l->whole = (BOOL)( wm_num(LK_OPA_WHOLE, 0) != 0 );
	l->wash = (UW)wm_num(LK_WASH, 0) & 0x00FFFFFFU;
	l->wash_str = ( ( from & 1 ) != 0 ) ? 0 : (UINT)within(wm_num(LK_WASH_STR, 0), 0, 255);
	l->bar_wash = (UW)wm_num(LK_BAR_WASH, 0) & 0x00FFFFFFU;
	l->bar_str = ( ( from & 2 ) != 0 ) ? 0 : (UINT)within(wm_num(LK_BAR_STR, 0), 0, 255);
	l->round = within(wm_num(LK_ROUND, 0), 0, CP_ROUND_MAX);
	l->dx = within(wm_num(LK_SHD_DX, 4), -32, 32);
	l->dy = within(wm_num(LK_SHD_DY, 4), -32, 32);
	l->size = within(wm_num(LK_SHD_SIZE, 2), 0, 32);
	l->dark = (UINT)within(wm_num(LK_SHD_DARK, 0), 0, 255);
	l->blur = within(wm_num(LK_BLUR, 0), 0, CP_BLUR_MAX);
	l->motion = within(wm_num(LK_MOTION, 0), 0, 2000);
}

/*
 * A window as the stages lay it (compose.c). The look is on the windows
 * with a frame: a menu or a list that pops up has none, draws its own
 * shadow and stays as it drew itself. The program's own opacity and
 * tint are on every window, as they always were.
 */
LOCAL void cp_of_view( CONST LOOKNOW *l, CONST T_DPRECT *outer, CONST T_DPRECT *work,
		       UINT attr, CONST UB *pixels, UINT opacity, UW tint, UINT tint_str,
		       T_CPWIN *cp )
{
	BOOL	fx = (BOOL)( ( attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) != 0 );
	UINT	a = ( opacity > 255 ) ? 255 : opacity;

	knl_memset(cp, 0, sizeof(*cp));
	cp->outer = *outer;
	cp->work = *work;
	cp->band = *outer;
	cp->band.bottom = ( ( attr & WM_ATTR_TITLE ) != 0 ) ? work->top : outer->top;
	cp->pixels = pixels;
	cp->pitch = wm_spec.pitch;
	if ( fx ) {
		a = a * l->opa / 255U;
	}
	cp->a_frame = a;
	cp->a_work = ( fx && l->whole ) ? a : 255;
	cp->keyed = (BOOL)( ( attr & WM_ATTR_POPUP ) != 0 );
	cp->tint = tint;
	cp->tint_str = tint_str;
	if ( fx ) {
		cp->round = l->round;
		cp->wash = l->wash;
		cp->wash_str = l->wash_str;
		cp->bar_wash = l->bar_wash;
		cp->bar_str = l->bar_str;
		cp->blur = l->blur;
		cp->shd_dx = l->dx;
		cp->shd_dy = l->dy;
		cp->shd_size = l->size;
		cp->shd_dark = l->dark;
	}
	cp->shown = *outer;
	cp->fade = 255;
}

/*
 * A window as the stages lay it, in the pixels of the screen: where it
 * stands and its parts as its own pixels lie on the screen, and the
 * look's lengths (the corners, the shadow, the spread) at the scale.
 */
LOCAL INT scaled_len( INT v, INT most )
{
	INT	d = dv(v);

	return ( d > most ) ? most : ( d < -most ) ? -most : d;
}

LOCAL void cp_of( CONST LOOKNOW *l, CONST T_DPRECT *outer, CONST T_DPRECT *work,
		  UINT attr, CONST UB *pixels, UINT opacity, UW tint, UINT tint_str,
		  T_CPWIN *cp )
{
	T_DPRECT	o, d;

	cp_of_view(l, outer, work, attr, pixels, opacity, tint, tint_str, cp);
	cp->pitch = wm_wpitch;
	if ( wm_scale == 100 ) {
		return;
	}
	o = cp->outer;
	win_dev(&o, &d);
	in_win_dev(&o, &d, &cp->work, &cp->work);
	in_win_dev(&o, &d, &cp->band, &cp->band);
	cp->outer = d;
	cp->shown = d;
	cp->round = scaled_len(cp->round, CP_ROUND_MAX);
	cp->blur = scaled_len(cp->blur, CP_BLUR_MAX);
	cp->shd_dx = scaled_len(cp->shd_dx, 32);
	cp->shd_dy = scaled_len(cp->shd_dy, 32);
	cp->shd_size = scaled_len(cp->shd_size, 32);
}

/* ---------------------------------------------------------------- opening and closing */

/*
 * A window that opens comes from a little smaller and fainter, and one
 * that closes goes the same way out, over LK_MOTION milliseconds. How
 * far it has come is worked out from the clock whenever the screen is
 * built, so a machine that cannot keep up shows fewer steps rather than
 * a slower motion. A task of this file builds the part of the screen
 * the motion covers until it is over.
 *
 * A window that closes lets go of everything at once but its pixels,
 * which stay as a ghost laid over the screen until its motion is over.
 */
#define MO_SCALE0	236		/* how large it starts or ends, of 256 */
#define MO_FADE0	72		/* how much of it shows then */
#define MO_TICK		16		/* ms between the steps */
#define MO_PRI		21		/* behind the desktop's own work */
#define GHOST_MAX	8

typedef struct {
	BOOL		used;
	UB		*pixels;
	UD		pages;
	T_DPRECT	outer, work;
	UINT		attr;
	UINT		opacity;
	UW		tint;
	UINT		tint_str;
	UD		t0;
	UINT		dur;
} GHOST;

LOCAL GHOST	wm_ghost[GHOST_MAX];
LOCAL ID	wm_mo_tsk = 0;

LOCAL UD now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UW)t.lo;
}

/* How far a motion has come, 0..255: quickly at first, settling at the end */
LOCAL UINT mo_part( UD t0, UINT dur, UD now )
{
	UD	gone = ( now > t0 ) ? now - t0 : 0;
	UINT	p, q;

	if ( dur == 0 || gone >= dur ) {
		return 255;
	}
	p = (UINT)( gone * 255U / dur );
	q = 255U - p;
	return 255U - q * q / 255U * q / 255U;
}

LOCAL void mo_place( T_CPWIN *cp, UINT e, BOOL closing )
{
	INT	w = cp->outer.right - cp->outer.left, h = cp->outer.bottom - cp->outer.top;
	INT	s = closing ? 256 - ( 256 - MO_SCALE0 ) * (INT)e / 255
			    : MO_SCALE0 + ( 256 - MO_SCALE0 ) * (INT)e / 255;
	INT	nw = w * s / 256, nh = h * s / 256;

	cp->shown.left = cp->outer.left + ( w - nw ) / 2;
	cp->shown.top = cp->outer.top + ( h - nh ) / 2;
	cp->shown.right = cp->shown.left + nw;
	cp->shown.bottom = cp->shown.top + nh;
	cp->fade = closing ? 255U - ( 255U - MO_FADE0 ) * e / 255U
			   : MO_FADE0 + ( 255U - MO_FADE0 ) * e / 255U;
}

/* A window as the stages lay it now, its own outline and its motion with it */
LOCAL void win_cp( CONST WMWIN *w, CONST LOOKNOW *l, UD now, T_CPWIN *cp )
{
	cp_of(l, &w->outer, &w->work, w->attr, w->pixels, w->opacity, w->tint, w->tint_str, cp);
	if ( w->own != NULL ) {
		cp->shape = ( wm_scale != 100 ) ? w->shape_dev : w->shape;
		cp->round = 0;
	}
	if ( w->mo_dur != 0 ) {
		mo_place(cp, mo_part(w->mo_t0, w->mo_dur, now), FALSE);
	}
}

LOCAL void ghost_cp( CONST GHOST *g, CONST LOOKNOW *l, UD now, T_CPWIN *cp )
{
	cp_of(l, &g->outer, &g->work, g->attr, g->pixels, g->opacity, g->tint, g->tint_str, cp);
	mo_place(cp, mo_part(g->t0, g->dur, now), TRUE);
}

/* How far a window reaches on the screen, its shadow with it */
LOCAL void reach_of( CONST WMWIN *w, T_DPRECT *r )
{
	LOOKNOW		l;
	T_CPWIN		cp;
	T_DPRECT	s;

	look_now(&l);
	cp_of_view(&l, &w->outer, &w->work, w->attr, w->pixels, w->opacity, 0, 0, &cp);
	*r = w->outer;
	if ( cp_shadow_extent(&cp, &s) ) {
		if ( s.left < r->left )     r->left = s.left;
		if ( s.top < r->top )       r->top = s.top;
		if ( s.right > r->right )   r->right = s.right;
		if ( s.bottom > r->bottom ) r->bottom = s.bottom;
	}
}

LOCAL void spread_add( PARTS *p, CONST T_DPRECT *r, CONST LOOKNOW *look, UD now );
LOCAL ER   compose_parts( CONST PARTS *ask );
LOCAL UW   ask( UINT what, CONST PARTS *parts );

/*
 * The windows on their way in or out carried a step on: the screen
 * under them built again, a window that has arrived left as it is and a
 * ghost that has gone let go of. Whether any are still on their way.
 */
LOCAL BOOL mo_step( void )
{
	T_DPRECT	r;
	PARTS		p;
	LOOKNOW		look;
	UB		*drop[GHOST_MAX];
	UD		drop_pages[GHOST_MAX];
	BOOL		more = FALSE;
	UD		now = now_ms();
	INT		i, nd = 0;

	p.n = 0;
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	look_now(&look);
	for ( i = 0; i < wm_wcap; i++ ) {
		WMWIN	*w = &WMW(i);

		if ( !w->used || w->mo_dur == 0 ) {
			continue;
		}
		reach_of(w, &r);
		if ( now >= w->mo_t0 + w->mo_dur ) {
			w->mo_dur = 0;		/* arrived: laid as itself from now on */
		} else {
			more = TRUE;
		}
		spread_add(&p, &r, &look, now);
	}
	for ( i = 0; i < GHOST_MAX; i++ ) {
		GHOST	*g = &wm_ghost[i];

		if ( !g->used ) {
			continue;
		}
		r = g->outer;
		if ( now >= g->t0 + g->dur ) {
			g->used = FALSE;
			drop[nd] = g->pixels;
			drop_pages[nd++] = g->pages;
		} else {
			more = TRUE;
		}
		spread_add(&p, &r, &look, now);
	}
	tk_unl_mtx(wm_mtxid);

	if ( p.n > 0 ) {
		(void)ask(0, &p);		/* the compositor builds it; nothing waits */
	}
	for ( i = 0; i < nd; i++ ) {
		knl_vunmap(drop[i], drop_pages[i]);
	}
	return more;
}

LOCAL void mo_task( INT stacd, void *exinf )
{
	for ( ;; ) {
		(void)tk_slp_tsk(TMO_FEVR);
		while ( mo_step() ) {
			(void)tk_dly_tsk(MO_TICK);
		}
	}
}

LOCAL void mo_wake( void )
{
	if ( wm_mo_tsk > 0 ) {
		(void)tk_wup_tsk(wm_mo_tsk);
	}
}

/*
 * A window that closes kept as a ghost, its pixels taken from it: TRUE
 * when it was, and the pixels are the ghost's to let go of. Called with
 * the lock held.
 */
LOCAL BOOL ghost_of( WMWIN *w )
{
	LOOKNOW	l;
	INT	i;

	look_now(&l);
	if ( l.motion <= 0 || wm_mo_tsk <= 0 || !w->visible || w->pixels == NULL
	  || ( w->attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) == 0 ) {
		return FALSE;
	}
	for ( i = 0; i < GHOST_MAX && wm_ghost[i].used; i++ ) ;
	if ( i == GHOST_MAX ) {
		return FALSE;
	}
	wm_ghost[i].pixels = w->pixels;
	wm_ghost[i].pages = w->pix_pages;
	wm_ghost[i].outer = w->outer;
	wm_ghost[i].work = w->work;
	wm_ghost[i].attr = w->attr;
	wm_ghost[i].opacity = w->opacity;
	wm_ghost[i].tint = w->tint;
	wm_ghost[i].tint_str = w->tint_str;
	wm_ghost[i].t0 = now_ms();
	wm_ghost[i].dur = (UINT)l.motion;
	wm_ghost[i].used = TRUE;
	w->pixels = NULL;
	w->pix_pages = 0;

	return TRUE;
}

/* Every ghost let go of at once: the screen they were laid on is gone */
LOCAL void ghosts_drop( void )
{
	INT	i;

	for ( i = 0; i < GHOST_MAX; i++ ) {
		if ( wm_ghost[i].used ) {
			wm_ghost[i].used = FALSE;
			knl_vunmap(wm_ghost[i].pixels, wm_ghost[i].pages);
		}
	}
}

/* ---------------------------------------------------------------- surfaces of processes */

/*
 * The surface of a window, taken from it with the lock held and let go
 * of after: putting the page of nothing in the process's place waits for
 * the process table, which is not taken while this lock is held.
 */
LOCAL INT	wm_surf_n = 0;		/* surfaces there are; under the lock */
LOCAL UD	wm_surf_pg = 0;		/* and their pages */
LOCAL UW	wm_surf_serial = 0;

LOCAL void surf_take( WMWIN *w, WMSURF *out )
{
	*out = w->surf;
	if ( w->surf.kva != NULL ) {
		wm_surf_n--;
		wm_surf_pg -= w->surf.pages;
	}
	w->surf.kva = NULL;
}

/*
 * The process's addresses first, then the pages: a write the process
 * still makes there after this goes to the page of nothing, never into
 * pages given to something else.
 */
LOCAL void surf_drop( CONST WMSURF *s )
{
	if ( s->kva == NULL ) {
		return;
	}
	(void)knl_prc_sink_shared(s->pid, s->uva, s->pages * PAGE_SIZE);
	knl_vunmap(s->kva, s->pages);
}

LOCAL void surf_tell( CONST WMSURF *s, T_WMSURF *out )
{
	out->addr = (void *)s->uva;
	out->width = s->w;
	out->height = s->h;
	out->pitch = s->w * (INT)sizeof(UW);
	out->serial = s->serial;
}

/* ---------------------------------------------------------------- windows */

LOCAL void floats_front( void );
LOCAL void subs_away( void );

LOCAL void screen_win( BOOL opened );

EXPORT INT wm_open( CONST T_DPRECT *outer, UINT attr, CONST char *title )
{
	WMWIN		*w;
	INT		i, k;
	UD		pages;
	T_DPRECT	fit, ask;
	T_DPRECT	on;
	INT		base;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( outer == NULL || outer->right <= outer->left
	  || outer->bottom <= outer->top ) {
		return E_PAR;
	}
	ask = *outer;
	wm_screen_at(( outer->left + outer->right ) / 2, ( outer->top + outer->bottom ) / 2, &on);
	base = ( on.left == 0 ) ? wm_msg_height() : 0;
	if ( outer->right - outer->left > on.right - on.left
	  || outer->bottom - outer->top > on.bottom - on.top ) {
		/*
		 * Larger than its screen (a window kept from a larger screen,
		 * or one drawn at a larger scale than it was kept at): made as
		 * large as the screen above the message line, at its corner
		 */
		fit = *outer;
		if ( fit.right - fit.left > on.right - on.left ) {
			fit.left = on.left;
			fit.right = on.right;
		}
		if ( fit.bottom - fit.top > on.bottom - on.top ) {
			fit.top = on.top;
			fit.bottom = on.bottom - base;
		}
		/* and the rest of it moved onto the screen as far as it reaches past it */
		if ( fit.right > on.right ) {
			fit.left -= fit.right - on.right;
			fit.right = on.right;
			if ( fit.left < on.left ) fit.left = on.left;
		}
		if ( fit.bottom > on.bottom - base && fit.top > on.top ) {
			INT	d = fit.bottom - ( on.bottom - base );

			if ( d > fit.top - on.top ) d = fit.top - on.top;
			fit.top -= d;
			fit.bottom -= d;
		}
		outer = &fit;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);

	for ( i = 0; i < wm_wcap; i++ ) {
		if ( !WMW(i).used ) {
			break;
		}
	}
	if ( i == wm_wcap && !win_grow() ) {
		tk_unl_mtx(wm_mtxid);
		return E_NOMEM;
	}
	w = &WMW(i);
	knl_memset(w, 0, sizeof(*w));
	w->wid = i + 1;
	w->serial = ++wm_win_serial;

	w->attr = attr;
	w->outer = *outer;
	w->want = ask;
	work_of(outer, attr, &w->work);
	w->shape = NULL;
	w->shape_dev = NULL;
	w->own = NULL;
	shape_of(w);
	w->opacity = 255;			/* the look's own comes on top, as it is set */
	w->tint = 0;
	w->tint_str = 0;
	w->mo_dur = 0;
	if ( wm_num(LK_LOOK, 1) != 0 && wm_num(LK_MOTION, 0) > 0 && wm_mo_tsk > 0
	  && ( attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) != 0 ) {
		w->mo_t0 = now_ms();
		w->mo_dur = (UINT)within(wm_num(LK_MOTION, 0), 0, 2000);
	}

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
	w->mktsk = tk_get_tid();
	w->mkpid = ( ts_get_pid() > 0 ) ? ts_get_pid() : 0;
	for ( k = 0; k < WM_TITLE_MAX - 1 && title != NULL && title[k] != '\0'; k++ ) {
		w->title[k] = title[k];
	}

	/* it comes up in front of everything else (of the windows that do not float, when it does not) */
	for ( k = 0; k < wm_wcap; k++ ) {
		if ( WMW(k).used ) {
			WMW(k).z++;
		}
	}
	w->z = 0;
	w->used = TRUE;
	w->visible = TRUE;
	wm_nwin++;
	if ( ( attr & ( WM_ATTR_POPUP | WM_ATTR_FLOAT ) ) == 0 ) {
		floats_front();
	}
	subs_away();			/* it is in front: another group may be in use now */

	/*
	 * The environment that draws into this window. Its origin is the
	 * corner of the work area and its frame is the work area, so the
	 * program draws in its own coordinates and can reach nothing else.
	 */
	w->gid = dp_open();
	if ( w->gid > 0 ) {
		(void)dp_set_watch(w->gid, TRUE);	/* what is drawn in it is built again */
		(void)dp_set_scale(w->gid, wm_scale);	/* in view pixels, into pixels of the screen */
	}
	if ( w->gid < 0 ) {
		INT	er = w->gid;		/* no memory for its drawing environment */

		knl_vunmap(w->pixels, pages);
		w->used = FALSE;
		wm_nwin--;
		z_close_gap(0);
		tk_unl_mtx(wm_mtxid);
		return er;
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
			dp_set_target(wm_sys_gid, w->pixels, wm_wpitch,
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
	dp_set_target(w->gid, w->pixels, wm_wpitch,
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
	if ( w->mo_dur != 0 ) {
		mo_wake();			/* it comes in step by step */
	}
	if ( ( attr & WM_ATTR_SCREEN ) != 0 ) {
		screen_win(TRUE);
	}

	return i + 1;
}

/*
 * Places of the screen to be laid again at the next wm_update that no
 * window's damage covers: where windows stood that have since been
 * closed, a window brought forward or sent back (what it covers has
 * changed, not what it holds), and a frame drawn again when the input
 * moved (the frame is outside the work area the damage speaks of).
 */
LOCAL PARTS	wm_gone;

LOCAL void gone_add( CONST T_DPRECT *r )
{
	parts_add(&wm_gone, r);
}

/* A window's place laid again, with the shadow it throws */
LOCAL void gone_win( CONST WMWIN *w )
{
	T_DPRECT	r;

	reach_of(w, &r);
	gone_add(&r);
}

/* All of the screen laid again: the look, the scheme or the ground changed */
LOCAL void gone_all( void )
{
	T_DPRECT	r;

	r.left = 0;  r.top = 0;
	r.right = wm_vw;
	r.bottom = wm_vh;
	gone_add(&r);
}

/*
 * The window nearest the viewer that a person works in: shown, with a
 * name band, and not a menu, panel or other pop-up, nor a window that
 * never has the keys. 0 when there is none. Called with the lock held.
 */
LOCAL INT front_window( INT but )
{
	INT	i, best = 0, bz = 0;

	for ( i = 1; i <= wm_wcap; i++ ) {
		WMWIN	*w = win_of(i);

		if ( w == NULL || i == but || !w->visible
		  || ( w->attr & ( WM_ATTR_POPUP | WM_ATTR_NOFOCUS ) ) != 0
		  || ( w->attr & WM_ATTR_OWN ) == 0 ) {
			continue;
		}
		if ( best == 0 || w->z < bz ) {
			best = i;
			bz = w->z;
		}
	}
	return best;
}

/* ---------------------------------------------------------------- where a finger presses at once */

/*
 * A window's places, read by the input layer without the lock, so each
 * table is written under a count: odd while it is being written, moved on
 * when done (wm_mtxid keeps writers apart). A reader that saw it odd or
 * changed has seen no places.
 */
typedef struct {
	volatile UW	seq;
	volatile INT	wid;		/* 0: free */
	volatile INT	n;
	T_DPRECT	r[WM_PRESS_RECTS];
} PRESSAREA;

LOCAL PRESSAREA		wm_press[WM_PRESS_WINS];
LOCAL volatile INT	wm_press_n = 0;		/* of them in use: no touch looks further while it is 0 */

/* A table given a window and its places, or freed. Called with the lock held. */
LOCAL void press_write( PRESSAREA *p, INT wid, CONST T_DPRECT *r, INT n )
{
	INT	i;

	if ( ( p->wid != 0 ) != ( wid != 0 ) ) {
		wm_press_n += ( wid != 0 ) ? 1 : -1;
	}
	p->seq++;
	__sync_synchronize();
	p->wid = wid;
	p->n = n;
	for ( i = 0; i < n; i++ ) {
		p->r[i] = r[i];
	}
	__sync_synchronize();
	p->seq++;
	__sync_synchronize();
}

/* A window's table, 0 for its wid being free: NULL when it has none. Called with the lock held. */
LOCAL PRESSAREA *press_of( INT wid )
{
	INT	i;

	for ( i = 0; i < WM_PRESS_WINS; i++ ) {
		if ( wm_press[i].wid == wid ) {
			return &wm_press[i];
		}
	}
	return NULL;
}

EXPORT ER wm_obj_press_area( INT wid, CONST T_DPRECT *r, INT n )
{
	PRESSAREA	*p;
	ER		er = E_OK;

	if ( n < 0 || n > WM_PRESS_RECTS || ( n > 0 && r == NULL ) ) {
		return E_PAR;
	}
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( win_of(wid) == NULL ) {
		er = E_ID;
	} else {
		p = press_of(wid);
		if ( n == 0 ) {
			if ( p != NULL ) {
				press_write(p, 0, NULL, 0);
			}
		} else {
			if ( p == NULL ) {
				p = press_of(0);
			}
			if ( p == NULL ) {
				er = E_LIMIT;
			} else {
				press_write(p, wid, r, n);
			}
		}
	}
	tk_unl_mtx(wm_mtxid);
	return er;
}

/*
 * Without the lock, so nothing is followed that a window's closing frees:
 * a window with a shape covers all of its outer rectangle here. That errs
 * on the side of the touch waiting to be known, as anywhere else. The
 * tables of pointers and the windows themselves stay once made.
 */
EXPORT BOOL knl_wm_press_now( INT x, INT y )
{
	CONST WMWIN	*w, *top = NULL;
	PRESSAREA	*p;
	INT		cap, i, k, n, wx, wy, z, top_z = 0;
	UW		seq;
	BOOL		in = FALSE;

	if ( !wm_ready || wm_press_n == 0 ) {
		return FALSE;
	}
	cap = wm_wcap;
	__sync_synchronize();
	for ( i = 0; i < cap; i++ ) {
		w = wm_wtbl[i];
		if ( !w->used || !w->visible ) {
			continue;
		}
		if ( y >= knl_msg_top() && ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			continue;
		}
		z = w->z;
		if ( x >= w->outer.left && x < w->outer.right && y >= w->outer.top && y < w->outer.bottom
		  && ( top == NULL || z < top_z ) ) {
			top = w;
			top_z = z;
		}
	}
	if ( top == NULL || x < top->work.left || x >= top->work.right
	  || y < top->work.top || y >= top->work.bottom ) {
		return FALSE;
	}
	wx = x - top->work.left;
	wy = y - top->work.top;
	for ( k = 0; k < WM_PRESS_WINS; k++ ) {
		p = &wm_press[k];
		seq = p->seq;
		__sync_synchronize();
		if ( ( seq & 1 ) != 0 || p->wid != top->wid ) {
			continue;
		}
		n = p->n;
		for ( i = 0; i < n && i < WM_PRESS_RECTS; i++ ) {
			if ( wx >= p->r[i].left && wx < p->r[i].right && wy >= p->r[i].top && wy < p->r[i].bottom ) {
				in = TRUE;
				break;
			}
		}
		__sync_synchronize();
		return (BOOL)( in && p->seq == seq );
	}
	return FALSE;
}

/*
 * The windows over the whole screen (WM_ATTR_SCREEN): the message line
 * taken down when the first opens, and put back when the last closes if
 * it was up when the first came
 */
LOCAL INT	wm_screen_n = 0;
LOCAL BOOL	wm_screen_took = FALSE;

LOCAL void screen_win( BOOL opened )
{
	BOOL	down = FALSE, up = FALSE;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( opened ) {
		if ( wm_screen_n++ == 0 && wm_msg_height() > 0 ) {
			wm_screen_took = TRUE;
			down = TRUE;
		}
	} else if ( wm_screen_n > 0 && --wm_screen_n == 0 && wm_screen_took ) {
		wm_screen_took = FALSE;
		up = TRUE;
	}
	tk_unl_mtx(wm_mtxid);
	if ( down ) (void)wm_msg_show(FALSE);
	if ( up ) (void)wm_msg_show(TRUE);
}

EXPORT ER wm_close( INT wid )
{
	WMWIN	*w;
	WMSURF	surf;
	INT	z, band_at;
	BOOL	ghost, screen;

	knl_hid_repeat_cut();		/* a key held for the window repeats into no other */
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	{
		PRESSAREA	*pa = press_of(wid);	/* the number may be another window's next */

		if ( pa != NULL ) {
			press_write(pa, 0, NULL, 0);
		}
	}
	surf_take(w, &surf);
	z = w->z;
	screen = (BOOL)( ( w->attr & WM_ATTR_SCREEN ) != 0 );
	if ( w->visible ) {
		gone_win(w);
	}
	if ( w->gid > 0 ) {
		dp_close(w->gid);
	}
	/* nothing draws in its pixels now: they go out as a ghost, or go */
	ghost = ghost_of(w);
	if ( w->pixels != NULL ) {
		knl_vunmap(w->pixels, w->pix_pages);
	}
	w->pixels = NULL;
	w->mo_dur = 0;
	dp_rgn_free(w->shape);
	w->shape = NULL;
	dp_rgn_free(w->shape_dev);
	w->shape_dev = NULL;
	dp_rgn_free(w->own);
	if ( w->icon != NULL ) {
		Kfree(w->icon);
		w->icon = NULL;
	}
	w->own = NULL;
	w->used = FALSE;
	w->away = FALSE;
	wm_nwin--;
	z_close_gap(z);
	{
		/*
		 * Its subordinate windows are on their own now: a later window
		 * given the same number must not take them as its own. One put
		 * out of sight comes back (subs_away below), so that what its
		 * program has not closed can still be seen and closed.
		 */
		INT	i;

		for ( i = 0; i < wm_wcap; i++ ) {
			if ( WMW(i).used && WMW(i).parent == wid ) {
				WMW(i).parent = 0;
			}
		}
	}
	band_at = band_wid();
	if ( wm_lit_wid == wid ) {
		wm_lit_wid = 0;
	}
	if ( wm_pop_from == wid ) {
		wm_pop_from = 0;
	}
	if ( wm_focus_wid == wid ) {
		/*
		 * The input goes on to the window that was behind: the one in
		 * front of those left. A person who closes a window goes on
		 * working in the next without having to press it first.
		 */
		WMWIN	*n;

		wm_focus_wid = front_window(wid);
		wm_pop_from = 0;		/* front_window gives no pop-up */
		n = win_of(wm_focus_wid);
		if ( n != NULL ) {
			draw_frame(n);
			gone_add(&n->outer);
		}
	}
	if ( band_at != wid ) {
		band_redraw(band_at);
	} else {
		band_redraw(0);
	}
	subs_away();
	tk_unl_mtx(wm_mtxid);
	surf_drop(&surf);
	knl_wmobj_close(wid);
	if ( ghost ) {
		mo_wake();			/* it goes out step by step */
	}
	if ( screen ) {
		screen_win(FALSE);
	}

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
		out->visible = ( w->visible || w->away ) ? 1 : 0;
		out->parent  = w->parent;
		out->away    = ( w->away ) ? 1 : 0;
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

/*
 * The subordinate windows of a window that was carried by (dx, dy)
 * carried with it, so that a tool panel stays where it was against the
 * window it works; one that would go above the screen stops at its top.
 */
LOCAL void subs_follow( INT wid, INT dx, INT dy )
{
	INT		*subs, n = 0, i;
	T_DPRECT	*o;

	if ( dx == 0 && dy == 0 ) {
		return;
	}
	/* every window it carries: room for as many as there are windows */
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	subs = (INT *)Kmalloc(sizeof(INT) * (SZ)( wm_wcap + 1 ));
	o = (T_DPRECT *)Kmalloc(sizeof(T_DPRECT) * (SZ)( wm_wcap + 1 ));
	if ( subs == NULL || o == NULL ) {
		tk_unl_mtx(wm_mtxid);
		if ( subs != NULL ) Kfree(subs);
		if ( o != NULL ) Kfree(o);
		return;
	}
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && WMW(i).parent == wid && i + 1 != wid ) {
			subs[n] = i + 1;
			o[n] = WMW(i).outer;
			n++;
		}
	}
	tk_unl_mtx(wm_mtxid);
	for ( i = 0; i < n; i++ ) {
		INT	ddy = dy;

		if ( o[i].top + ddy < 0 ) {
			ddy = -o[i].top;
		}
		o[i].left += dx;
		o[i].right += dx;
		o[i].top += ddy;
		o[i].bottom += ddy;
		(void)wm_move(subs[i], &o[i]);
	}
	Kfree(subs);
	Kfree(o);
}

/*
 * A window moved to outer, and where it was asked to stand set: to ask
 * when it is given (wm_place), or else as the person's move makes it --
 * outer itself, but for a window the screen made smaller that keeps its
 * size: there only its corner moves, and the size asked for stays.
 */
LOCAL ER move_win( INT wid, CONST T_DPRECT *outer, CONST T_DPRECT *ask );

EXPORT ER wm_move( INT wid, CONST T_DPRECT *outer )
{
	return move_win(wid, outer, NULL);
}

/*
 * A program's window placed where it asks (OB_WR_PLACE): kept on the
 * screen above the message line, made smaller only when it is larger
 * than that, and moved up and left as far as it reaches past it. What
 * it asked for is kept, for a larger screen to give back.
 */
EXPORT ER wm_place( INT wid, CONST T_DPRECT *outer )
{
	T_DPRECT	o;
	INT		sh, wide, high;
	T_DPRECT	on;

	if ( outer == NULL || outer->right <= outer->left
	  || outer->bottom <= outer->top ) {
		return E_PAR;
	}
	o = *outer;
	wm_screen_at(( o.left + o.right ) / 2, ( o.top + o.bottom ) / 2, &on);
	sh = on.bottom - ( ( on.left == 0 ) ? wm_msg_height() : 0 );
	wide = o.right - o.left;
	high = o.bottom - o.top;
	if ( wide > on.right - on.left ) wide = on.right - on.left;
	if ( high > sh - on.top ) high = sh - on.top;
	if ( o.left + wide > on.right ) o.left = on.right - wide;
	if ( o.top + high > sh ) o.top = sh - high;
	if ( o.left < on.left ) o.left = on.left;
	if ( o.top < on.top ) o.top = on.top;
	o.right = o.left + wide;
	o.bottom = o.top + high;

	return move_win(wid, &o, outer);
}

LOCAL ER move_win( INT wid, CONST T_DPRECT *outer, CONST T_DPRECT *ask )
{
	WMWIN		*w;
	T_DPRECT	v, was;
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
	if ( w->visible ) {
		gone_win(w);			/* where it stood, and its shadow */
	}
	w->mo_dur = 0;				/* a window moved has arrived */
	v = w->work;
	was = w->outer;
	if ( ask != NULL ) {
		w->want = *ask;
	} else if ( !resized && ( w->want.right - w->want.left != was.right - was.left
				  || w->want.bottom - w->want.top != was.bottom - was.top ) ) {
		INT	ww = w->want.right - w->want.left, wh = w->want.bottom - w->want.top;

		w->want.left = outer->left;
		w->want.top = outer->top;
		w->want.right = outer->left + ww;
		w->want.bottom = outer->top + wh;
	} else {
		w->want = *outer;
	}
	w->outer = *outer;
	work_of(outer, w->attr, &w->work);
	/* the same outside with a frame measured anew is a new work area too */
	resized = (BOOL)( resized || v.right - v.left != w->work.right - w->work.left
			  || v.bottom - v.top != w->work.bottom - w->work.top );
	shape_of(w);				/* a new size is a new shape */
	if ( w->visible ) {
		gone_win(w);			/* and where it stands now */
	}
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
	dp_set_place(w->gid, pixels, wm_wpitch, outer->left, outer->top, &v);
	if ( old != NULL ) {
		knl_vunmap(old, oldpages);
	}

	draw_frame(w);				/* the frame moved with it */
	knl_wmobj_moved(wid, resized);
	subs_follow(wid, outer->left - was.left, outer->top - was.top);

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
	for ( i = 0; i < wm_wcap; i++ ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
		used = WMW(i).used;
		o = WMW(i).outer;
		attr = WMW(i).attr;
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
 * The windows that have 'attr' put back in front of everything, in the
 * order they stood among themselves
 */
LOCAL void attr_front( UINT attr )
{
	INT	i, k, best;

	for ( k = 0; k < wm_nwin; k++ ) {
		best = -1;
		/* the one furthest back that is not yet in front */
		for ( i = 0; i < wm_wcap; i++ ) {
			if ( WMW(i).used && ( WMW(i).attr & attr ) != 0
			  && WMW(i).z >= k
			  && ( best < 0 || WMW(i).z > WMW(best).z ) ) {
				best = i;
			}
		}
		if ( best < 0 || WMW(best).z == k ) {
			break;
		}
		{
			INT	old = WMW(best).z;

			for ( i = 0; i < wm_wcap; i++ ) {
				if ( WMW(i).used && WMW(i).z < old ) {
					WMW(i).z++;
				}
			}
			WMW(best).z = 0;
		}
	}
}

/*
 * The popups put back in front of everything: a window brought forward
 * comes up behind them, as a menu's lists stay over the window they were
 * opened from
 */
LOCAL void popups_front( void )
{
	attr_front(WM_ATTR_POPUP);
}

/* The floating windows put back in front of the others, then the popups in front of them */
LOCAL void floats_front( void )
{
	attr_front(WM_ATTR_FLOAT);
	popups_front();
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
	for ( z = 0; z < wm_wcap && n < max; z++ ) {
		for ( i = 0; i < wm_wcap && n < max; i++ ) {
			WMWIN	*w = &WMW(i);

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
	BOOL	between = FALSE;

	if ( s->z == p->z - 1 ) {
		return;
	}
	if ( s->z < p->z ) {
		/* in front of it with only others of its window between: it stays */
		for ( i = 0; i < wm_wcap && !between; i++ ) {
			if ( WMW(i).used && WMW(i).z > s->z && WMW(i).z < p->z
			  && WMW(i).parent != s->parent ) {
				between = TRUE;
			}
		}
		if ( !between ) {
			return;
		}
	}
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && &WMW(i) != s && WMW(i).z > old ) {
			WMW(i).z--;
		}
	}
	pz = p->z;
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && &WMW(i) != s && WMW(i).z >= pz ) {
			WMW(i).z++;
		}
	}
	s->z = pz;
	if ( s->visible ) {
		gone_win(s);
	}
}

LOCAL void subs_front( void )
{
	INT	i;

	for ( i = 0; i < wm_wcap; i++ ) {
		WMWIN	*s = &WMW(i);
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

/*
 * The window a window belongs with in the end: its window's window and
 * so on, up to one that is subordinate to none. Called with the lock held.
 */
LOCAL INT root_of( INT wid )
{
	WMWIN	*w;
	INT	guard;

	for ( guard = 0; guard < wm_wcap; guard++ ) {
		w = win_of(wid);
		if ( w == NULL || w->parent <= 0 || w->parent == wid
		  || win_of(w->parent) == NULL ) {
			break;
		}
		wid = w->parent;
	}
	return wid;
}

/*
 * The group in use, by the window it is rooted at: that of the window
 * nearest the viewer that a person works in -- shown, with a name band
 * or subordinate to a window, and not a pop-up (a menu, a panel asking
 * something, which stands over the window it was opened from) nor a
 * window that never has the keys (a screen keyboard). A floating window
 * counts only while it has the keys: it stays in front of the others
 * whichever is worked in. A window a program opens comes up in front,
 * so its group is the one in use from then on, as it is when a window
 * is pressed and brought forward. 0 when there is none. Called with the
 * lock held.
 */
LOCAL INT active_root( void )
{
	INT	i, best = 0, bz = 0;

	for ( i = 1; i <= wm_wcap; i++ ) {
		WMWIN	*w = win_of(i);

		if ( w == NULL || !w->visible
		  || ( w->attr & ( WM_ATTR_POPUP | WM_ATTR_NOFOCUS ) ) != 0
		  || ( ( w->attr & WM_ATTR_OWN ) == 0 && w->parent <= 0 )
		  || ( ( w->attr & WM_ATTR_FLOAT ) != 0 && i != wm_focus_wid ) ) {
			continue;
		}
		if ( best == 0 || w->z < bz ) {
			best = i;
			bz = w->z;
		}
	}
	return ( best > 0 ) ? root_of(best) : 0;
}

/*
 * The subordinate windows of every group but the one in use put out of
 * sight, and those of the one in use shown again. A tool panel works
 * the window it belongs to and nothing else: over another program's
 * window it would only be in the way, and pressed there it would bring
 * its own window forward unasked. Its place among the others stays as
 * it is (sub_place), so it comes back just in front of its window. A
 * pop-up is left as it is: it is up only while it is answered. Called
 * with the lock held, after anything that changes which group is in use.
 */
LOCAL void subs_away( void )
{
	INT	i, act = active_root();
	BOOL	out;

	for ( i = 0; i < wm_wcap; i++ ) {
		WMWIN	*s = &WMW(i);

		if ( !s->used ) {
			continue;
		}
		if ( s->parent <= 0 || win_of(s->parent) == NULL
		  || ( s->attr & WM_ATTR_POPUP ) != 0 ) {
			out = FALSE;
		} else {
			out = (BOOL)( act != 0 && root_of(i + 1) != act );
		}
		if ( out && s->visible ) {
			gone_win(s);
			s->visible = FALSE;
			s->away = TRUE;
		} else if ( !out && s->away ) {
			s->away = FALSE;
			s->visible = TRUE;
			gone_win(s);
		}
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
	floats_front();
	subs_away();
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
	/* a subordinate window comes forward with its window, just in front of it */
	if ( w->parent > 0 && win_of(w->parent) != NULL && win_of(w->parent) != w ) {
		w = win_of(w->parent);
	}
	old = w->z;
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && WMW(i).z < old ) {
			WMW(i).z++;
		}
	}
	w->z = 0;
	subs_front();
	if ( ( w->attr & WM_ATTR_POPUP ) == 0 ) {
		if ( ( w->attr & WM_ATTR_FLOAT ) == 0 ) {
			floats_front();
		} else {
			popups_front();
		}
	}
	if ( old != w->z && w->visible ) {
		gone_win(w);		/* it now covers what covered it */
	}
	subs_away();
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
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( WMW(i).used && WMW(i).z > old ) {
			WMW(i).z--;
		}
	}
	w->z = wm_nwin - 1;
	subs_front();
	if ( old != w->z && w->visible ) {
		gone_win(w);		/* what it covered shows now */
	}
	subs_away();
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_show( INT wid, BOOL on )
{
	WMWIN	*w;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w != NULL ) {
		if ( w->visible != on ) {
			gone_win(w);		/* it shows, or what it covered does */
		}
		w->visible = on;
		/* shown while its group is not in use: it waits out of sight */
		w->away = FALSE;
		subs_away();
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
LOCAL UW	*wm_def_icon = NULL;	/* what a window with no icon shows (wm_set_icon_default) */
LOCAL INT	wm_def_w, wm_def_h;

LOCAL void draw_icon( INT gid, CONST T_DPRECT *box, CONST UW *px, INT w, INT h )
{
	UW	*sq;
	INT	side = box->bottom - box->top, x, y;

	if ( side <= 0 || w <= 0 || h <= 0 ) {
		return;
	}
	if ( wm_scale != 100 ) {
		T_DPRECT	b = *box;

		/* at a scale the icon is taken again at the box's own pixels, from all of its detail */
		b.right = b.left + side;
		(void)dp_put_image(gid, &b, px, w, h, w, IMG_CLEAR);
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
		} else if ( box.right < fw - 2 && wm_def_icon != NULL ) {
			draw_icon(gid, &box, wm_def_icon, wm_def_w, wm_def_h);
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
	dp_set_target(wm_sys_gid, w->pixels, wm_wpitch,
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

	active = ( band_wid() == w->wid );
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
 * An outline of a window, rows from its corner in view pixels, as the
 * rows of the window's own pixels at the scale: each row of pixels
 * takes the row of view pixels it belongs to, each run widened to the
 * pixels its view pixels cover. NULL when there is no room.
 */
LOCAL T_DPRGN *rgn_dev( CONST T_DPRGN *src, INT h )
{
	INT	H = dv(h), Y, *x0, *x1;
	T_DPRGN	*out = NULL;

	if ( H <= 0 ) {
		return NULL;
	}
	x0 = (INT *)Kmalloc((SZ)H * 2 * sizeof(INT));
	if ( x0 == NULL ) {
		return NULL;
	}
	x1 = x0 + H;
	for ( Y = 0; Y < H; Y++ ) {
		CONST INT	*sp = NULL;
		INT		ns = dp_rgn_row(src, disp_view_of(Y, wm_scale), &sp);

		/* a row of one run: the first and the last of the view row's, what lies between taken too */
		x0[Y] = ( ns > 0 ) ? dv(sp[0]) : 0;
		x1[Y] = ( ns > 0 ) ? dv(sp[ns * 2 - 1]) : 0;
	}
	if ( dp_rgn_from_rows(0, H, x0, x1, &out) < E_OK ) {
		out = NULL;
	}
	Kfree(x0);
	return out;
}

/*
 * The shape a window is found in. A square window has none; one with
 * round corners (LK_ROUND, a window with a frame while the look is on)
 * is the rows its corners cover at least half of each pixel of, from
 * the same cover the corners are laid with (compose.c), so that where
 * a press finds the window is where it shows. It is worked out once
 * here, when the size or the corners change, not at every press.
 */
LOCAL void shape_of( WMWIN *w )
{
	T_DPRECT	r;
	T_DPRGN		*rgn = NULL;
	CONST UB	*cov;
	INT		rad, y, *x0;

	dp_rgn_free(w->shape);
	w->shape = NULL;
	dp_rgn_free(w->shape_dev);
	w->shape_dev = NULL;
	if ( w->own != NULL ) {
		if ( dp_rgn_copy(w->own, &rgn) >= E_OK ) {
			w->shape = rgn;		/* the one it was given wins */
		}
		if ( wm_scale != 100 ) {
			w->shape_dev = rgn_dev(w->own, w->outer.bottom - w->outer.top);
		}
		return;
	}
	if ( wm_num(LK_LOOK, 1) == 0 || ( w->attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) == 0 ) {
		return;
	}
	r.left = 0;  r.top = 0;
	r.right = w->outer.right - w->outer.left;
	r.bottom = w->outer.bottom - w->outer.top;
	rad = cp_round_of(&r, wm_num(LK_ROUND, 0));
	cov = cp_corner_cov(rad);
	if ( cov == NULL ) {
		return;
	}
	x0 = (INT *)Kmalloc((SZ)r.bottom * 2 * sizeof(INT));
	if ( x0 == NULL ) {
		return;
	}
	for ( y = 0; y < r.bottom; y++ ) {
		INT	ey = ( y < r.bottom - 1 - y ) ? y : r.bottom - 1 - y, ex = 0;

		if ( ey < rad ) {
			while ( ex < rad && cov[ey * rad + ex] < 128 ) {
				ex++;
			}
		}
		x0[y] = ex;
		x0[r.bottom + y] = r.right - ex;
	}
	if ( dp_rgn_from_rows(0, r.bottom, x0, x0 + r.bottom, &rgn) >= E_OK ) {
		w->shape = rgn;
	}
	Kfree(x0);
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
	gone_win(w);			/* what showed round it shows again */
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
	if ( win->visible ) gone_add(&win->outer);
	tk_unl_mtx(wm_mtxid);
	if ( old != NULL ) Kfree(old);
	(void)wm_update();

	return E_OK;
}

/*
 * The icon a window with none of its own shows in its band, in colour,
 * in place of the plain pictogram drawn in the band's text colour: the
 * pixels are copied. Until it is given (early in the start) the plain
 * pictogram stands.
 */
EXPORT ER wm_set_icon_default( CONST UW *px, INT w, INT h )
{
	UW	*copy, *old;
	WMWIN	*win;
	INT	z;

	if ( px == NULL || w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	copy = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
	if ( copy == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(copy, px, (INT)( (SZ)w * h * sizeof(UW) ));
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	old = wm_def_icon;
	wm_def_icon = copy;
	wm_def_w = w;
	wm_def_h = h;
	for ( z = 0; z < wm_nwin; z++ ) {
		win = win_at_z(z);
		if ( win != NULL && win->icon == NULL ) {
			draw_frame(win);
			if ( win->visible ) gone_add(&win->outer);
		}
	}
	tk_unl_mtx(wm_mtxid);
	if ( old != NULL ) Kfree(old);
	(void)wm_update();

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
	if ( w->visible ) gone_add(&w->outer);
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
	if ( w->visible ) gone_add(&w->outer);
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
	if ( w->visible ) gone_win(w);		/* and what shows through it */
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
	if ( w->visible ) gone_add(&w->outer);
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
	if ( w->bar[which].lo == b->lo && w->bar[which].hi == b->hi
	  && w->bar[which].clo == b->clo && w->bar[which].chi == b->chi ) {
		tk_unl_mtx(wm_mtxid);
		return E_OK;			/* as it was: nothing to draw or build */
	}
	w->bar[which] = *b;
	draw_frame(w);
	if ( w->visible ) gone_add(&w->outer);	/* the frame goes to the screen too */
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

LOCAL void touched( CONST T_DPRECT *r );

/*
 * A surface as large as the work area is now: the one the window has
 * when it is the process's and of that size, else a new one in place of
 * whatever it had. The pages are cleared before the process sees them.
 * They are made and mapped with the lock let go of (mapping waits for
 * the process table), so the window is looked for again after, by its
 * serial: its number may have been closed and opened again meanwhile.
 */
EXPORT ER wm_surf_get( INT wid, ID pid, T_WMSURF *s )
{
	WMWIN	*w;
	WMSURF	nw, old;
	INT	ww, wh;
	UW	serial;
	ER	er;

	if ( s == NULL || pid <= 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	ww = w->work.right - w->work.left;
	wh = w->work.bottom - w->work.top;
	serial = w->serial;
	if ( w->surf.kva != NULL && w->surf.pid == pid && w->surf.w == ww && w->surf.h == wh ) {
		surf_tell(&w->surf, s);
		tk_unl_mtx(wm_mtxid);
		return E_OK;
	}
	tk_unl_mtx(wm_mtxid);
	if ( ww <= 0 || wh <= 0 ) {
		return E_OBJ;
	}

	knl_memset(&nw, 0, sizeof(nw));
	nw.pages = ( (UD)ww * (UD)wh * sizeof(UW) + PAGE_SIZE - 1 ) / PAGE_SIZE;
	nw.kva = (UB *)knl_vmap(nw.pages, 0);
	if ( nw.kva == NULL ) {
		return E_NOMEM;
	}
	knl_memset(nw.kva, 0, (SZ)( nw.pages * PAGE_SIZE ));
	er = knl_prc_map_kernel(pid, nw.kva, nw.pages * PAGE_SIZE, &nw.uva);
	if ( er < E_OK ) {
		knl_vunmap(nw.kva, nw.pages);
		return er;
	}
	nw.pid = pid;
	nw.w = ww;
	nw.h = wh;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL || w->serial != serial ) {
		tk_unl_mtx(wm_mtxid);
		surf_drop(&nw);
		return E_ID;
	}
	surf_take(w, &old);
	nw.serial = ++wm_surf_serial;
	w->surf = nw;
	wm_surf_n++;
	wm_surf_pg += nw.pages;
	surf_tell(&w->surf, s);
	tk_unl_mtx(wm_mtxid);
	surf_drop(&old);

	return E_OK;
}

/* A row of pixels, a word at a time: the two rows need not lie alike against eight bytes */
LOCAL void row_copy( UW *d, CONST UW *s, INT n )
{
	for ( ; n >= 4; n -= 4, d += 4, s += 4 ) {
		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
	}
	for ( ; n > 0; n-- ) {
		*d++ = *s++;
	}
}

/*
 * A rectangle c of a surface in view pixels into the window's own
 * pixels at the scale, each of the surface's pixels as large as a view
 * pixel is (the nearest one taken for each pixel of the window); ox, oy
 * is the work area's corner from the window's.
 */
LOCAL void surf_put_scaled( CONST WMWIN *w, CONST T_DPRECT *c, INT ox, INT oy )
{
	INT	Y0 = dv(oy + c->top), Y1 = dv(oy + c->bottom), X0 = dv(ox + c->left), X1 = dv(ox + c->right);
	INT	X, Y;

	for ( Y = Y0; Y < Y1; Y++ ) {
		INT		y = disp_view_of(Y, wm_scale) - oy;
		UW		*d = (UW *)( w->pixels + (UD)Y * wm_wpitch );
		CONST UW	*s = (CONST UW *)w->surf.kva + (UD)y * (UD)w->surf.w;

		for ( X = X0; X < X1; X++ ) {
			d[X] = s[disp_view_of(X, wm_scale) - ox];
		}
	}
}

/*
 * Rectangles of the surface (work area coordinates) into the window's
 * own pixels, as far as both reach, and noted as drawn by the calling
 * task: its next wm_update builds them. The copy is made with the lock
 * held, which the compositor holds while it builds, so no build reads
 * the window half written.
 */
EXPORT ER wm_surf_put( INT wid, ID pid, CONST T_DPRECT *r, INT n )
{
	WMWIN		*w;
	T_DPRECT	c, lim, s;
	INT		i, y, ox, oy;

	if ( r == NULL || n <= 0 || n > WM_PUT_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_ID;
	}
	if ( w->surf.kva == NULL || w->surf.pid != pid || w->pixels == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_OBJ;			/* no surface of this process's */
	}
	lim.left = 0;
	lim.top = 0;
	lim.right = w->work.right - w->work.left;
	lim.bottom = w->work.bottom - w->work.top;
	if ( lim.right > w->surf.w )  lim.right = w->surf.w;
	if ( lim.bottom > w->surf.h ) lim.bottom = w->surf.h;
	ox = w->work.left - w->outer.left;
	oy = w->work.top - w->outer.top;
	for ( i = 0; i < n; i++ ) {
		c = r[i];
		if ( !rect_cut(&c, &lim) ) {
			continue;
		}
		if ( wm_scale != 100 ) {
			surf_put_scaled(w, &c, ox, oy);
		} else {
			for ( y = c.top; y < c.bottom; y++ ) {
				row_copy((UW *)( w->pixels + (UD)( oy + y ) * wm_spec.pitch ) + ox + c.left,
					 (CONST UW *)w->surf.kva + (UD)y * (UD)w->surf.w + c.left, c.right - c.left);
			}
		}
		s.left = c.left + w->work.left;
		s.top = c.top + w->work.top;
		s.right = c.right + w->work.left;
		s.bottom = c.bottom + w->work.top;
		touched(&s);
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT void knl_wm_surf_prc_end( ID pid )
{
	WMSURF	gone[16];
	INT	i, n;

	if ( pid <= 0 || wm_mtxid <= 0 ) {
		return;
	}
	do {
		n = 0;
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
		for ( i = 0; i < wm_wcap && n < 16; i++ ) {
			if ( WMW(i).used && WMW(i).surf.kva != NULL && WMW(i).surf.pid == pid ) {
				surf_take(&WMW(i), &gone[n++]);
			}
		}
		tk_unl_mtx(wm_mtxid);
		for ( i = 0; i < n; i++ ) {
			surf_drop(&gone[i]);
		}
	} while ( n == 16 );
}

EXPORT INT knl_wm_surf_count( UD *p_pages )
{
	INT	n;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	n = wm_surf_n;
	if ( p_pages != NULL ) {
		*p_pages = wm_surf_pg;
	}
	tk_unl_mtx(wm_mtxid);

	return n;
}

EXPORT ER knl_wm_surf_ref( INT wid, ID *p_pid, UBINT *p_va, UD *p_pages )
{
	WMWIN	*w;
	ER	er = E_NOEXS;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL ) {
		er = E_ID;
	} else if ( w->surf.kva != NULL ) {
		if ( p_pid != NULL )   *p_pid = w->surf.pid;
		if ( p_va != NULL )    *p_va = w->surf.uva;
		if ( p_pages != NULL ) *p_pages = w->surf.pages;
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

EXPORT void wm_frame_of( UINT attr, INT *p_wide, INT *p_high )
{
	INT	l, t, r, b;

	borders_of(attr, &l, &t, &r, &b);
	*p_wide = l + r;
	*p_high = t + b;
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
			found = w->wid;
			break;
		}
	}
	tk_unl_mtx(wm_mtxid);

	return found;
}

/*
 * Which window a finger's press at (x, y) is for, its handles reaching
 * slop farther: the one nearest the viewer that covers the place, or
 * whose corner that sizes it (WM_GRIP_CORNER) the place is within slop
 * outside of. A window's corner reached so is before the windows behind
 * it, as its frame is; a window in front of it covering the place is
 * before it.
 */
LOCAL INT at_reach( INT x, INT y, INT slop )
{
	WMWIN	*w;
	INT	z, found = 0;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( z = 0; z < wm_nwin; z++ ) {
		w = win_at_z(z);
		if ( w == NULL || !w->visible ) {
			continue;
		}
		if ( y >= knl_msg_top() && ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			continue;
		}
		if ( x >= w->outer.left && x < w->outer.right
		  && y >= w->outer.top && y < w->outer.bottom && in_shape(w, x, y) ) {
			found = w->wid;
			break;
		}
		if ( ( w->attr & WM_ATTR_RESIZE ) != 0 && ( w->attr & WM_ATTR_POPUP ) == 0
		  && ( w->attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) != 0
		  && x > w->outer.right - WM_GRIP_CORNER && x < w->outer.right + slop
		  && y > w->outer.bottom - WM_GRIP_CORNER && y < w->outer.bottom + slop ) {
			found = w->wid;
			break;
		}
	}
	tk_unl_mtx(wm_mtxid);

	return found;
}

/* Presses of buttons and keys, modifiers aside: never 0, which is no ticket */
LOCAL UINT	act_seq = 1;
LOCAL UD	act_when;		/* the press counted last: read, then handed on, it is one */
LOCAL UD	act_ms;			/* when it was read */

LOCAL UD act_now( void )
{
	SYSTIM	t;

	if ( tk_get_otm(&t) < E_OK ) {
		return 0;
	}
	return ( (UD)(UW)t.hi << 32 ) | (UD)t.lo;
}

LOCAL void act_press( UD when )
{
	if ( when != act_when ) {
		act_when = when;
		act_seq++;
		if ( act_seq == 0 ) act_seq = 1;
		act_ms = act_now();
	}
}

EXPORT UINT wm_act_ticket( void )
{
	return act_seq;
}

EXPORT UW wm_act_age( void )
{
	UD	d = act_now() - act_ms;

	return ( d > 0xFFFFFFFFUL ) ? 0xFFFFFFFFU : (UW)d;
}

EXPORT BOOL wm_act_good( UINT ticket )
{
	return (BOOL)( ticket != 0 && ticket == act_seq && wm_act_age() < WM_ACT_MS );
}

EXPORT ER wm_focus( INT wid )
{
	WMWIN	*w, *was;
	INT	was_wid, band_at;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	was_wid = wm_focus_wid;
	band_at = band_wid();
	if ( wid == 0 ) {
		wm_focus_wid = 0;
	} else {
		w = win_of(wid);
		if ( w == NULL ) {
			tk_unl_mtx(wm_mtxid);
			return E_ID;
		}
		if ( ( w->attr & WM_ATTR_NOFOCUS ) != 0 ) {
			tk_unl_mtx(wm_mtxid);
			return E_OBJ;		/* a screen keyboard: the keys stay where they are */
		}
		/* a pop-up given the input: the window it was taken from keeps its band lit */
		if ( ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			wm_pop_from = 0;
		} else if ( wid != was_wid ) {
			WMWIN	*f = win_of(was_wid);

			if ( f != NULL && ( f->attr & WM_ATTR_POPUP ) == 0 ) {
				wm_pop_from = was_wid;
			}
		}
		wm_focus_wid = wid;
	}
	if ( wid == 0 ) {
		wm_pop_from = 0;
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
	band_redraw(band_at);
	subs_away();			/* another group may be in use now */
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

EXPORT ER wm_lit( INT wid, BOOL on )
{
	WMWIN	*w;
	INT	was;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( !on ) {
		if ( wm_lit_wid != wid ) {
			tk_unl_mtx(wm_mtxid);
			return E_OK;		/* another is lit, or none */
		}
		wid = 0;
	} else {
		w = win_of(wid);
		if ( w == NULL ) {
			tk_unl_mtx(wm_mtxid);
			return E_ID;
		}
		if ( ( w->attr & WM_ATTR_NOFOCUS ) == 0 ) {
			tk_unl_mtx(wm_mtxid);
			return E_OBJ;
		}
	}
	was = band_wid();
	wm_lit_wid = wid;
	band_redraw(was);
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

/* ---------------------------------------------------------------- 強制終了 */

/* The window nearest the viewer that is not a popup, 0 none. Called with the lock held. */
LOCAL INT front_any( void )
{
	WMWIN	*w;
	INT	z;

	for ( z = 0; z < wm_nwin; z++ ) {
		w = win_at_z(z);
		if ( w != NULL && w->visible && ( w->attr & WM_ATTR_POPUP ) == 0 ) {
			return w->wid;
		}
	}
	return 0;
}

/*
 * For 強制終了 (fquit.c): the window a person works in -- the one with
 * the keys, or else the one nearest the viewer that is not a popup -- in
 * *p_wid (0 none), and the popups shown in front of it, nearest first:
 * a panel the desktop shows for a program stands there. E_TMOUT when
 * the lock of the windows is not had within tmo: a task that holds it
 * and does not let it go must not stop the keyboard's last resort.
 */
EXPORT ER knl_wm_front_pick( TMO tmo, INT *p_wid, INT *pops, INT max, INT *p_npop )
{
	WMWIN	*f, *w;
	INT	wid, z, n = 0;

	*p_wid = 0;
	*p_npop = 0;
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( tk_loc_mtx(wm_mtxid, tmo) < E_OK ) {
		return E_TMOUT;
	}
	wid = wm_focus_wid;
	if ( wid != 0 && win_of(wid) == NULL ) {
		wid = 0;
	}
	if ( wid == 0 ) {
		wid = front_any();
	}
	f = win_of(wid);
	for ( z = 0; z < wm_nwin && n < max; z++ ) {
		w = win_at_z(z);
		if ( w == NULL ) {
			continue;
		}
		if ( f != NULL && w->z >= f->z ) {
			break;
		}
		if ( w->visible && ( w->attr & WM_ATTR_POPUP ) != 0 ) {
			pops[n++] = w->wid;
		}
	}
	tk_unl_mtx(wm_mtxid);
	*p_wid = wid;
	*p_npop = n;

	return E_OK;
}

/*
 * For 強制終了: the window nearest the viewer that is not a popup, in
 * *p_wid (0 none), whether it has the keys or not. E_TMOUT as above.
 */
EXPORT ER knl_wm_front_any( TMO tmo, INT *p_wid )
{
	*p_wid = 0;
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( tk_loc_mtx(wm_mtxid, tmo) < E_OK ) {
		return E_TMOUT;
	}
	*p_wid = front_any();
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * The windows the tasks tids opened, closed with the panels and menus
 * standing in them, and what those tasks may have left laid over the
 * screen -- outlines being carried, the loupe, the pointer's shape --
 * put back: the desktop's tasks were stopped, and it starts again
 * (fquit.c). Answers how many windows were closed.
 */
EXPORT INT knl_wm_tsk_end( CONST ID *tids, INT ntid )
{
	INT	*wids, n = 0, i, k;

	if ( !wm_ready || ntid <= 0 ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	wids = (INT *)Kmalloc(sizeof(INT) * (SZ)( wm_wcap + 1 ));
	if ( wids == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_NOMEM;
	}
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( !WMW(i).used ) {
			continue;
		}
		for ( k = 0; k < ntid; k++ ) {
			if ( WMW(i).mktsk == tids[k] ) {
				wids[n++] = i + 1;
				break;
			}
		}
	}
	tk_unl_mtx(wm_mtxid);
	knl_pn_wins_gone(wids, n);
	for ( i = 0; i < n; i++ ) {
		(void)wm_close(wids[i]);	/* E_ID for one a panel took with it */
	}
	Kfree(wids);
	(void)wm_set_drag(NULL, 0);
	(void)wm_set_loupe(NULL, 0, 0, 0, 0);
	(void)wm_set_pointer(WM_PT_SELECT);
	(void)wm_show_pointer(TRUE);
	wm_update();

	return n;
}

/*
 * The last press read and where the pointer was at the last event of
 * the pointer read, on the screen: what a menu or a pop-up's list opened
 * by that press measures its release against (wm_click_*)
 */
LOCAL UD	lp_when = 0;
LOCAL UINT	lp_code = 0;
LOCAL INT	lp_x = 0, lp_y = 0;
LOCAL INT	lr_x = 0, lr_y = 0;

#define CLICK_SLOP	8		/* pixels the pointer may move in a click */

/*
 * The opening press: its button as its notice says it, and where it went
 * down when it is the last press read from the input (a press handed in
 * directly, as the tests hand one, has no place, and is followed by time
 * and by what its list says of it)
 */
#define PRESS_KEPT	8

LOCAL UD	ps_when[PRESS_KEPT];
LOCAL UINT	ps_code[PRESS_KEPT];
LOCAL INT	ps_next = 0;

EXPORT void knl_wm_press_seen( UD when, UINT code )
{
	INT	i;

	for ( i = 0; i < PRESS_KEPT; i++ ) {
		if ( ps_when[i] == when && ps_code[i] == code ) {
			return;			/* read, then handed on: one press */
		}
	}
	ps_when[ps_next] = when;
	ps_code[ps_next] = code;
	ps_next = ( ps_next + 1 ) % PRESS_KEPT;
}

/*
 * The presses of a finger read lately: when each was made and the view
 * pixels in 100 mm of the screen it touched, for wm_hit_slop. Only a
 * finger's are kept; a press not among them is a mouse's, a pen's or
 * one put in.
 */
LOCAL UD	tp_when[PRESS_KEPT];
LOCAL INT	tp_px[PRESS_KEPT];
LOCAL INT	tp_next = 0;

#define TCH_HIT_MM10	40		/* a handle a finger reaches 4 mm farther, unless set */

EXPORT void knl_wm_touch_note( UD when )
{
	INT	px = 0, i;

	if ( !knl_hid_taken_touch(&px) || px <= 0 || when == 0 ) {
		return;
	}
	for ( i = 0; i < PRESS_KEPT; i++ ) {
		if ( tp_when[i] == when ) {
			tp_px[i] = px;
			return;
		}
	}
	tp_when[tp_next] = when;
	tp_px[tp_next] = px;
	tp_next = ( tp_next + 1 ) % PRESS_KEPT;
}

EXPORT INT wm_hit_slop( UD when )
{
	INT	i;

	if ( when == 0 || wm_num(LK_TCH_ON, 1) == 0 ) {
		return 0;
	}
	for ( i = 0; i < PRESS_KEPT; i++ ) {
		if ( tp_when[i] == when && tp_px[i] > 0 ) {
			return (INT)( (D)wm_num(LK_TCH_HIT, TCH_HIT_MM10) * tp_px[i] / 1000 );
		}
	}
	return 0;
}

EXPORT void wm_click_begin( T_WMCLICK *c, UD when, UINT code )
{
	INT	i;

	knl_memset(c, 0, sizeof(*c));
	c->when = when;
	if ( code == WM_CK_FIND ) {
		for ( i = 0; i < PRESS_KEPT; i++ ) {
			if ( when != 0 && ps_when[i] == when ) {
				code = ps_code[i];
			}
		}
		if ( code == WM_CK_FIND ) {
			return;			/* no press known: nothing to follow */
		}
	}
	c->live = TRUE;
	c->code = code;
	if ( lp_when == when && lp_code == code ) {
		c->x = lp_x;
		c->y = lp_y;
		c->placed = TRUE;
	}
}

EXPORT INT wm_click_take( T_WMCLICK *c, CONST T_WMEV *ev )
{
	UD	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	UINT	bit;

	if ( ev->type != HID_EV_MOVE && ev->type != HID_EV_BTN_DOWN && ev->type != HID_EV_BTN_UP ) {
		return WM_CK_PASS;
	}
	if ( c->placed && c->live
	  && ( lr_x - c->x > CLICK_SLOP || c->x - lr_x > CLICK_SLOP
	    || lr_y - c->y > CLICK_SLOP || c->y - lr_y > CLICK_SLOP ) ) {
		c->moved = TRUE;
	}
	if ( ev->type == HID_EV_MOVE || ev->code >= 32 ) {
		return WM_CK_PASS;
	}
	bit = 1U << ev->code;
	if ( ev->type == HID_EV_BTN_UP && ( c->eaten & bit ) != 0 ) {
		c->eaten &= ~bit;
		return WM_CK_EAT;		/* the release of a press not passed on */
	}
	if ( !c->live ) {
		return WM_CK_PASS;
	}
	if ( ev->code != c->code ) {
		/* another button while the opening one is held: not the menu's */
		if ( ev->type == HID_EV_BTN_DOWN ) {
			c->eaten |= bit;
		}
		return WM_CK_EAT;
	}
	if ( ev->type == HID_EV_BTN_DOWN ) {
		c->live = FALSE;		/* pressed again: let go unseen, and over */
		return WM_CK_PASS;
	}
	c->live = FALSE;
	/*
	 * A click only while the pointer has not moved: carried farther than
	 * a click allows, or onto another row of a list (moved, set by the
	 * list), it is let go where it is however soon
	 */
	if ( !c->moved && ( ev->when < c->when || ev->when - c->when < gap || c->placed ) ) {
		return WM_CK_KEPT;
	}
	return WM_CK_PASS;
}

/*
 * Address one thing that happened to a window. A key goes to whoever
 * has the input; anything with a place goes to the window under that
 * place, whatever has the input. The place is then measured from that
 * window's work area, because that is what its program draws in.
 *
 * Shift+Pause never comes here: the input layer hands it to 強制終了
 * as the keyboard reports it (fquit.c, design 5.5.1). While the reader
 * waits for something to happen, the watch on the desktop's loop counts
 * it as come round (knl_wdog_idle): waiting for a person is not being
 * stuck.
 *
 * Nothing here changes the order. Pressing a button does not bring a
 * window forward: the press is carried, and coming forward is something
 * a program asks for when it decides to.
 */
/* The key that types a letter in the latin layout in use; was when none does */
LOCAL UINT key_of_letter( UB letter, UINT was )
{
	UINT	c;

	for ( c = 0x04; c <= 0x38; c++ ) {
		if ( wm_key_char(c, 0) == letter ) {
			return c;
		}
	}
	return was;
}

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
again:
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
	knl_wdog_idle(TRUE);
	er = ts_hid_read(&h, tmout);
	knl_wdog_idle(FALSE);
	knl_msg_tick();
	knl_menu_tick();	/* a row stayed on: its list opens */
	knl_pn_tick();		/* the caret of a panel's box blinks */
	if ( er < E_OK ) {
		return er;
	}
	if ( h.type == HID_EV_BTN_DOWN || ( h.type == HID_EV_KEY_DOWN && h.code < 0xE0 ) ) {
		act_press(h.when);
	}
	if ( h.type == HID_EV_BTN_DOWN ) {
		knl_wm_touch_note(h.when);
		knl_msg_press(h.x, h.y);
		knl_wm_press_seen(h.when, h.code);
		lp_when = h.when;
		lp_code = h.code;
		lp_x = h.x;
		lp_y = h.y;
	}
	if ( h.type == HID_EV_MOVE || h.type == HID_EV_BTN_DOWN || h.type == HID_EV_BTN_UP ) {
		lr_x = h.x;
		lr_y = h.y;
	}
	/* every key as it came, to the screen keyboards that asked (OB_N_KEYSEEN) */
	if ( h.type == HID_EV_KEY_DOWN || h.type == HID_EV_KEY_UP ) {
		T_WMEV	seen;

		knl_memset(&seen, 0, sizeof(seen));
		seen.type = h.type;
		seen.code = h.code;
		seen.mods = h.mods;
		seen.when = h.when;
		knl_wmobj_key_seen(&seen);
	}
	/*
	 * The volume keys are the system's: the whole's volume a step up
	 * or down at each press, and at each repeat while one is held, as
	 * any key repeats; the mute key turns the mute over. They go to no
	 * window.
	 */
	if ( ( h.type == HID_EV_KEY_DOWN || h.type == HID_EV_KEY_UP )
	  && ( h.code == HID_KEY_VOLUP || h.code == HID_KEY_VOLDOWN || h.code == HID_KEY_MUTE ) ) {
		if ( h.type == HID_EV_KEY_DOWN ) {
			(void)knl_wm_volume_key(h.code);
		}
		goto again;
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
		/* by the letters the keys type, as the commands are taken (wm_key_cmd) */
		switch ( wm_key_cmd(h.code) ) {
		case 0x19:	h.code = key_of_letter('z', h.code);	break;	/* V as Z */
		case 0x1B:	h.code = key_of_letter('v', h.code);	break;	/* X as V */
		case 0x1D:	h.code = key_of_letter('x', h.code);	break;	/* Z as X */
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
		INT	slop = ( h.type == HID_EV_BTN_DOWN && h.code == 0 ) ? wm_hit_slop(h.when) : 0;

		/* a finger's press: a window's corner is reached farther (wm_hit_slop) */
		ev->wid = ( slop > 0 ) ? at_reach(h.x, h.y, slop) : wm_at(h.x, h.y);
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
	/* while a pop-up's list is open, every input is the list's and goes no further */
	if ( knl_pn_list_event(ev) ) {
		goto again;
	}
	/*
	 * The input mode worked here, in the keys' order: the notice of
	 * this key and of each key after it carries the mode as it then
	 * stands (msgline.c).
	 */
	if ( h.type == HID_EV_KEY_DOWN ) {
		(void)knl_msg_mode_key(h.code, h.mods);
	}
	/* a press, wherever it goes, to the programs that count a turn from it (OB_E_PRESSED) */
	knl_wmobj_press_seen(ev);
	/* to whoever asked to be told; a press on the ground too, which puts
	   a program's menu away */
	knl_wmobj_event(ev);

	return E_OK;
}

/* ---------------------------------------------------------------- screen */

/*
 * The part of a window nothing behind it shows through, which a window
 * behind it may be left out under: the whole of it when it is solid,
 * its work area when only its frame is seen through, less the rows of
 * its round corners; none when it is seen through all over, is on its
 * way in, has an outline of its own or pops up (a menu draws round
 * itself what it does not cover). FALSE for none.
 */
LOCAL BOOL solid_of( CONST WMWIN *f, CONST LOOKNOW *l, UD now, T_DPRECT *r )
{
	T_CPWIN	cp;
	INT	rad;

	if ( !f->visible || ( f->attr & WM_ATTR_POPUP ) != 0 || f->own != NULL ) {
		return FALSE;
	}
	win_cp(f, l, now, &cp);
	if ( CP_MOVING(&cp) || cp.a_work < 255 ) {
		return FALSE;
	}
	/* in pixels of the screen, as what it is laid over is */
	*r = ( cp.a_frame < 255 ) ? cp.work : cp.outer;
	rad = cp_round_of(&cp.outer, cp.round);
	if ( r->top < cp.outer.top + rad )       r->top = cp.outer.top + rad;
	if ( r->bottom > cp.outer.bottom - rad ) r->bottom = cp.outer.bottom - rad;

	return ( r->left < r->right && r->top < r->bottom );
}

/* How many windows seen through and spread stand within 'near' of a part of the screen */
LOCAL INT seen_through_near( CONST T_DPRECT *box, CONST LOOKNOW *l, UD now, INT near )
{
	T_CPWIN		cp;
	T_DPRECT	r;
	INT		i, n = 0;

	for ( i = 0; i < wm_wcap; i++ ) {
		CONST WMWIN	*w = &WMW(i);

		if ( !w->used || !w->visible ) {
			continue;
		}
		win_cp(w, l, now, &cp);
		if ( cp.blur <= 0 || ( cp.a_frame >= 255 && cp.a_work >= 255 ) ) {
			continue;
		}
		r = cp.outer;			/* in pixels of the screen, as box is */
		r.left -= near;  r.top -= near;  r.right += near;  r.bottom += near;
		if ( rect_cut(&r, box) ) {
			n++;
		}
	}
	return n;
}

/*
 * What the windows' look asks of whoever builds the screen (CP_FX_*): a
 * look beyond solid and square makes a part dearer on the processor, and
 * a spread dearer again, differently on the GPU.
 */
LOCAL UINT look_fx( CONST LOOKNOW *l )
{
	UINT	fx = 0;

	if ( l->opa < 255 || l->whole || l->wash_str != 0 || l->bar_str != 0
	  || l->round > 0 || l->dark != 0 || l->blur > 0 ) {
		fx |= CP_FX_LOOK;
	}
	if ( l->blur > 0 && ( l->opa < 255 || l->whole ) ) {
		fx |= CP_FX_BLUR;
	}
	return fx;
}

/*
 * Whether the window at place z of the order, visible and with pixels,
 * is laid into 'abox': its description in 'cp'. Not when no part of it
 * or its shadow is there, nor when a window in front of it covers all
 * of that with a solid part (counted when 'count').
 */
LOCAL BOOL lay_test( CONST WMWIN *w, INT z, CONST LOOKNOW *look, UD now, CONST T_DPRECT *abox,
		     T_CPWIN *cp, BOOL count )
{
	T_DPRECT	vis;
	INT		fz;

	win_cp(w, look, now, cp);

	/* how far it reaches, with its shadow */
	vis = cp->shown;
	if ( !CP_MOVING(cp) ) {
		T_DPRECT	s;

		if ( cp_shadow_extent(cp, &s) ) {
			if ( s.left < vis.left )     vis.left = s.left;
			if ( s.top < vis.top )       vis.top = s.top;
			if ( s.right > vis.right )   vis.right = s.right;
			if ( s.bottom > vis.bottom ) vis.bottom = s.bottom;
		}
	}
	if ( !rect_cut(&vis, abox) ) {
		return FALSE;			/* no part of it is being built */
	}

	/* anything in front whose solid part covers the whole of that */
	for ( fz = z - 1; fz >= 0; fz-- ) {
		WMWIN		*f = win_at_z(fz);
		T_DPRECT	s;

		if ( f != NULL && solid_of(f, look, now, &s) && rect_covers(&s, &vis) ) {
			if ( count ) {
				wm_count.windows_skipped++;
			}
			return FALSE;
		}
	}
	return TRUE;
}

/* The windows handed to the GPU, and those the processor lays together, under the lock */
LOCAL T_CPWIN		wm_gcp[CP_GPU_MAX];
LOCAL T_CPWIN		wm_lcp[CP_LAY_MAX];

/*
 * Over a window manager's lock, for whoever has to use the stages of
 * compose.c beside it (the GPU's check at start-up, on scenes of its own).
 */
EXPORT void knl_wm_hold( BOOL hold )
{
	if ( !wm_ready ) {
		return;
	}
	if ( hold ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
	} else {
		tk_unl_mtx(wm_mtxid);
	}
}

/* The pointer, kept in the back buffer with what it covers (pointer.c) */
IMPORT BOOL knl_pt_want( T_DPRECT *r );
IMPORT BOOL knl_pt_on( T_DPRECT *r );
IMPORT BOOL knl_pt_fresh( CONST T_DPRECT *want );
IMPORT void knl_pt_lift( UB *back, UINT pitch, INT sw, INT sh );
IMPORT void knl_pt_lay( UB *back, UINT pitch, INT sw, INT sh, CONST T_DPRECT *at );
IMPORT void knl_pt_forget( void );
IMPORT void knl_pt_under( UW *dst, INT pitch, CONST T_DPRECT *at );

/*
 * How wide a margin a part of the screen needs round it for what is
 * spread near it: as far as a spread reaches for each window seen
 * through and spread that stands within that of it, since what a window
 * spreads is what the windows under it left, and theirs reaches as far
 * again. 0 when nothing near is spread or the part is the whole screen.
 * The same width says how far a change spreads out: a pixel of a window
 * seen through changes when what lies within it under the window does.
 */
LOCAL INT spread_margin( CONST T_DPRECT *box, CONST LOOKNOW *look, UD now )
{
	INT	sw = (INT)wm_spec.width, sh = (INT)wm_spec.height;
	INT	reach, n, margin = 0;

	if ( look->blur <= 0
	  || ( box->left <= 0 && box->top <= 0 && box->right >= sw && box->bottom >= sh ) ) {
		return 0;
	}
	reach = cp_spread_reach(scaled_len(look->blur, CP_BLUR_MAX));
	for ( n = seen_through_near(box, look, now, reach); n * reach > margin;
	      n = seen_through_near(box, look, now, margin) ) {
		margin = n * reach;
	}
	return margin;
}

/*
 * A part of the screen whose content changed, with it as much round it
 * as a window seen through and spread over it shows the change in: a
 * part built alone then comes out as the whole screen built would.
 * Called with the lock held.
 */
LOCAL void spread_add( PARTS *p, CONST T_DPRECT *r, CONST LOOKNOW *look, UD now )
{
	T_DPRECT	g = *r, d;
	INT		m;

	/* the margin is the screen's pixels: as many view pixels as cover them */
	scr_dev(r, &d);
	m = spread_margin(&d, look, now);
	if ( wm_scale != 100 ) {
		m = (INT)( ( (UINT)m * 100 + wm_scale - 1 ) / wm_scale );
	}

	g.left -= m;   g.top -= m;
	g.right += m;  g.bottom += m;
	parts_add(p, &g);
}

/*
 * One part of the screen as it is to be built: what is built and sent,
 * that with the margin round it a spread needs (built for the spread and
 * put back as it was), and the GPU when the GPU builds it.
 */
typedef struct {
	T_DPRECT	box;
	T_DPRECT	abox;
	CONST T_CPGPU	*gpu;
} PLAN;

/*
 * A picture in view pixels laid over the view rectangle at, within the
 * part box of the screen's pixels: each of its pixels as large as a
 * view pixel is. Rows of the picture are 'pw' long.
 */
LOCAL void pict_scaled( UB *back, CONST T_DPRECT *box, CONST T_DPRECT *at, CONST UW *px, INT pw )
{
	T_DPRECT	e;
	INT		x, y;

	e.left = dv(at->left);  e.top = dv(at->top);
	e.right = dv(at->right);  e.bottom = dv(at->bottom);
	if ( px == NULL || !rect_cut(&e, box) ) {
		return;
	}
	for ( y = e.top; y < e.bottom; y++ ) {
		UW		*p = (UW *)( back + (UBINT)y * wm_spec.pitch );
		CONST UW	*s = px + (SZ)( disp_view_of(y, wm_scale) - at->top ) * pw - at->left;

		for ( x = e.left; x < e.right; x++ ) {
			p[x] = s[disp_view_of(x, wm_scale)];
		}
	}
}

/*
 * One part of the screen built afresh, from the ground up, with the
 * outlines of what is carried over it. Called with the lock held, the
 * screen held and the pointer taken up from it if it lay there.
 */
LOCAL void build_part( CONST PLAN *pl, UB *back, CONST LOOKNOW *look, UD now )
{
	WMWIN		*w;
	BOOL		laid, kept = FALSE;
	T_DPRECT	box = pl->box, abox = pl->abox;
	T_CPDST		dst;
	T_CPWIN		cp;
	UD		t0 = 0, t1;
	INT		z, x, y, i, gdone = -1, seen, nl;
	INT		sw = (INT)wm_spec.width, sh = (INT)wm_spec.height;
	CONST T_CPGPU	*gpu = pl->gpu;

	if ( cp_gpu_get() != NULL ) {
		(void)ts_get_mono(&t0);
	}
	if ( abox.left != box.left || abox.top != box.top
	  || abox.right != box.right || abox.bottom != box.bottom ) {
		kept = cp_margin_keep(back, wm_spec.pitch, &abox, &box);
		if ( !kept ) {
			abox = box;
		}
	}

	/*
	 * What was built is what has changed. Saying otherwise leaves the
	 * ground in the back buffer and never on the screen, and what the
	 * person then sees around the windows is whatever was in the card's
	 * memory when the machine started.
	 */
	{
		T_DISPRECT	all;

		all.left = box.left;  all.top = box.top;
		all.right = box.right;  all.bottom = box.bottom;
		ts_disp_damage(&all);
	}

	dst.back = back;
	dst.pitch = wm_spec.pitch;
	dst.box = abox;
	dst.rows = sh;

	/* the ground and the windows up to the first that pops up, on the GPU */
	if ( gpu != NULL && wm_ground != NULL ) {
		INT	n = 0;

		for ( z = wm_nwin - 1; z >= 0 && n < CP_GPU_MAX; z-- ) {
			w = win_at_z(z);
			if ( w == NULL || !w->visible || w->pixels == NULL ) {
				continue;
			}
			if ( ( w->attr & WM_ATTR_POPUP ) != 0 ) {
				break;			/* the message line goes under it */
			}
			if ( lay_test(w, z, look, now, &abox, &wm_gcp[n], FALSE) ) {
				n++;
			}
		}
		gdone = gpu->lay(&dst, wm_ground, (UINT)sw * sizeof(UW), wm_ground_gen, wm_gcp, n);
	}

	if ( gdone < 0 ) {
		INT	wide = abox.right - abox.left;

		if ( wm_ground != NULL ) {
			for ( y = abox.top; y < abox.bottom; y++ ) {
				cp_copy((UW *)( back + (UBINT)y * wm_spec.pitch ) + abox.left,
					wm_ground + (SZ)y * sw + abox.left, wide);
			}
		} else {
			UW	desk = wm_look(WM_LOOK_DESK);

			/* no room to keep it: the plain desk, laid straight */
			for ( y = abox.top; y < abox.bottom; y++ ) {
				UW	*p = (UW *)(back + (UBINT)y * wm_spec.pitch);

				for ( x = abox.left; x < abox.right; x++ ) {
					p[x] = desk;
				}
			}
		}
	}

	/*
	 * The windows up to the first that pops up are laid together, each
	 * where those in front of it leave it to show (cp_lay); the menus
	 * over the message line one by one
	 */
	laid = FALSE;
	seen = 0;
	nl = 0;
	for ( z = wm_nwin - 1; z >= 0; z-- ) {
		w = win_at_z(z);
		if ( w == NULL || !w->visible || w->pixels == NULL ) {
			continue;
		}
		if ( !laid && ( w->attr & WM_ATTR_POPUP ) != 0 ) {
			/* the message line over the windows, under the menus */
			cp_lay(&dst, wm_lcp, nl);
			nl = 0;
			knl_msg_lay(back, wm_spec.pitch, &abox);
			laid = TRUE;
		}
		if ( !lay_test(w, z, look, now, &abox, &cp, TRUE) ) {
			continue;
		}
		wm_count.windows_drawn++;
		if ( seen++ < gdone ) {
			continue;			/* the GPU laid it */
		}
		if ( laid ) {
			cp_window(&dst, &cp);
			continue;
		}
		if ( nl >= CP_LAY_MAX ) {
			cp_lay(&dst, wm_lcp, nl);
			nl = 0;
		}
		wm_lcp[nl++] = cp;
	}
	cp_lay(&dst, wm_lcp, nl);

	/* the windows that have closed, on their way out over the rest */
	for ( i = 0; i < GHOST_MAX; i++ ) {
		if ( wm_ghost[i].used ) {
			ghost_cp(&wm_ghost[i], look, now, &cp);
			cp_moving(&dst, &cp);
		}
	}
	if ( !laid ) {
		knl_msg_lay(back, wm_spec.pitch, &abox);
	}
	if ( kept ) {
		cp_margin_put(back, wm_spec.pitch, &abox, &box);
	}

	/*
	 * What is being carried, over every window: the outline of each
	 * thing, drawn by turning each pixel on its edge to its opposite.
	 * It belongs to no window -- it goes wherever the hand takes it,
	 * across windows and the ground between them -- so it is laid on
	 * the screen, like the pointer, and not into any window's pixels.
	 * Only inside the part, which was built afresh: a pixel turned
	 * there is turned once whatever other part also holds it.
	 */
	{
		INT	k;

		for ( k = 0; k < wm_ndrag; k++ ) {
			T_DPRECT	e[4], d = wm_drag[k];
			INT		j, t = 1;

			if ( wm_scale != 100 ) {
				/* in pixels of the screen, the line as wide as a view pixel */
				if ( drag_pict(k) ) {
					pict_scaled(back, &box, &wm_drag[k], wm_dpx[k], wm_dpw[k]);
					continue;
				}
				d.left = dv(d.left);  d.top = dv(d.top);
				d.right = dv(d.right);  d.bottom = dv(d.bottom);
				t = dv(1);
			}
			if ( drag_pict(k) ) {
				/* the thing itself, as it looked where it was taken */
				e[0] = d;
				if ( rect_cut(&e[0], &box) ) {
					for ( y = e[0].top; y < e[0].bottom; y++ ) {
						UW	*p = (UW *)(back + (UBINT)y * wm_spec.pitch);
						CONST UW *s = wm_dpx[k] + (UBINT)( y - d.top ) * (UBINT)wm_dpw[k] - d.left;

						for ( x = e[0].left; x < e[0].right; x++ ) {
							p[x] = s[x];
						}
					}
				}
				continue;
			}
			e[0].left = d.left;  e[0].right = d.right;
			e[0].top = d.top;    e[0].bottom = d.top + t;
			e[1].left = d.left;  e[1].right = d.right;
			e[1].top = d.bottom - t;  e[1].bottom = d.bottom;
			e[2].left = d.left;  e[2].right = d.left + t;
			e[2].top = d.top + t;  e[2].bottom = d.bottom - t;
			e[3].left = d.right - t;  e[3].right = d.right;
			e[3].top = d.top + t;  e[3].bottom = d.bottom - t;
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

	/* the picture over every window, after the outlines, as it was given */
	if ( wm_loupe_on && wm_scale != 100 ) {
		pict_scaled(back, &box, &wm_loupe_at, wm_loupe, wm_loupe_at.right - wm_loupe_at.left);
	} else if ( wm_loupe_on ) {
		T_DPRECT	e = wm_loupe_at;
		INT		lw = wm_loupe_at.right - wm_loupe_at.left;

		if ( rect_cut(&e, &box) ) {
			for ( y = e.top; y < e.bottom; y++ ) {
				UW	*p = (UW *)(back + (UBINT)y * wm_spec.pitch);
				CONST UW *s = wm_loupe + (UBINT)( y - wm_loupe_at.top ) * lw - wm_loupe_at.left;

				for ( x = e.left; x < e.right; x++ ) {
					p[x] = s[x];
				}
			}
		}
	}

	wm_count.parts++;
	wm_count.pixels += part_area(&abox);
	if ( cp_gpu_get() != NULL ) {
		(void)ts_get_mono(&t1);
		cp_gpu_get()->note(gdone >= 0, &box, ( t1 - t0 ) / 1000);
	}
}

/*
 * The counts written to the console now and then while the screen is
 * being built, as so much a second: what was built, what was sent, and
 * how often the pointer moved with nothing built for it.
 */
#define WM_STATS_MS	60000

LOCAL UD		wm_st_t0 = 0;
LOCAL T_WMSTAT		wm_st_was;
LOCAL T_DISPSTAT	wm_st_dwas;

/*
 * Whether it is time the counts were written, and what they came to
 * since they last were: taken with the lock held, written after it is
 * let go of, since writing to the console is slow.
 */
typedef struct {
	UD		secs;
	T_WMSTAT	w;
	T_DISPSTAT	d;
	UD		hidden;		/* pixels of windows left out under others */
} WMSTATS;

LOCAL UD		wm_st_hid;

LOCAL BOOL stats_take( UD now, WMSTATS *o )
{
	T_DISPSTAT	ds;

	if ( wm_st_t0 == 0 ) {
		wm_st_t0 = now;
		wm_st_was = wm_count;
		wm_st_hid = cp_hidden();
		(void)ts_disp_stat(&wm_st_dwas);
		return FALSE;
	}
	if ( now - wm_st_t0 < WM_STATS_MS ) {
		return FALSE;
	}
	(void)ts_disp_stat(&ds);
	o->secs = ( now - wm_st_t0 ) / 1000;
	o->w.composites = wm_count.composites - wm_st_was.composites;
	o->w.parts = wm_count.parts - wm_st_was.parts;
	o->w.pixels = wm_count.pixels - wm_st_was.pixels;
	o->w.ptr_moves = wm_count.ptr_moves - wm_st_was.ptr_moves;
	o->w.ptr_built = wm_count.ptr_built - wm_st_was.ptr_built;
	o->d.sends = ds.sends - wm_st_dwas.sends;
	o->d.rects = ds.rects - wm_st_dwas.rects;
	o->d.pixels = ds.pixels - wm_st_dwas.pixels;
	o->d.send_us = ds.send_us - wm_st_dwas.send_us;
	o->d.flips = ds.flips - wm_st_dwas.flips;
	o->d.waits = ds.waits - wm_st_dwas.waits;
	o->d.wait_us = ds.wait_us - wm_st_dwas.wait_us;
	o->hidden = cp_hidden() - wm_st_hid;
	wm_st_t0 = now;
	wm_st_was = wm_count;
	wm_st_hid = cp_hidden();
	wm_st_dwas = ds;
	return ( o->w.composites != 0 || o->w.ptr_moves != 0 );
}

LOCAL void stats_print( CONST WMSTATS *o )
{
	UD	s = ( o->secs != 0 ) ? o->secs : 1;
	UD	ns = ( o->d.sends != 0 ) ? o->d.sends : 1;
	UD	nw = ( o->d.waits != 0 ) ? o->d.waits : 1;

	tm_printf((UB *)"wm: in %ld s: %ld builds (%ld parts), %ld kpx built, %ld kpx/s, "
		  "%ld kpx of windows hidden and left out; "
		  "%ld rects sent, %ld kpx, %ld kpx/s; pointer moved %ld times, %ld with something built; "
		  "%ld sends of %ld us each, %ld flips, %ld waited for a flip, %ld us each\n",
		  o->secs, o->w.composites, o->w.parts, o->w.pixels / 1000, o->w.pixels / 1000 / s,
		  o->hidden / 1000,
		  o->d.rects, o->d.pixels / 1000, o->d.pixels / 1000 / s, o->w.ptr_moves, o->w.ptr_built,
		  o->d.sends, o->d.send_us / ns, o->d.flips, o->d.waits, o->d.wait_us / nw);
}

/*
 * How the builds and the flips kept to the refresh (T_WMFRAMES,
 * T_DISPFRAME), written every WM_FRAMES_MS while the screen is built at
 * least WM_FRAMES_MIN times a second: something moving, or a program
 * putting frame after frame. Two lines, "wm: frames:" and "disp:
 * frames:", of what the counts came to in that time (design 10.4.5).
 */
#define WM_FRAMES_MS	10000
#define WM_FRAMES_MIN	20
#define WM_FRAME_US	16667		/* a refresh at 60 Hz: the least time between two builds where the sends do not keep it */

LOCAL T_WMFRAMES	wm_fr;
LOCAL UD		wm_fr_t0 = 0;
LOCAL T_WMFRAMES	wm_fr_was;
LOCAL T_DISPFRAME	wm_fr_dwas;

typedef struct {
	UD		ms;
	T_WMFRAMES	w;
	T_DISPFRAME	d;
} WMFRAMES;

EXPORT ER wm_frames( T_WMFRAMES *st )
{
	if ( st == NULL ) {
		return E_PAR;
	}
	*st = wm_fr;
	return E_OK;
}

/* Whether it is time the counts were written, and what they came to since */
LOCAL BOOL frames_take( UD now, WMFRAMES *o )
{
	T_DISPFRAME	ds;
	INT		i;

	knl_memset(&ds, 0, sizeof(ds));
	(void)ts_disp_frames(&ds);
	if ( wm_fr_t0 == 0 || now < wm_fr_t0 ) {
		wm_fr_t0 = now;
		wm_fr_was = wm_fr;
		wm_fr_dwas = ds;
		return FALSE;
	}
	if ( now - wm_fr_t0 < WM_FRAMES_MS ) {
		return FALSE;
	}
	o->ms = now - wm_fr_t0;
	o->w.builds = wm_fr.builds - wm_fr_was.builds;
	o->w.apart = wm_fr.apart - wm_fr_was.apart;
	o->w.apart_us = wm_fr.apart_us - wm_fr_was.apart_us;
	o->w.late = wm_fr.late - wm_fr_was.late;
	o->w.lay_us = wm_fr.lay_us - wm_fr_was.lay_us;
	o->w.send_us = wm_fr.send_us - wm_fr_was.send_us;
	o->w.paced_us = wm_fr.paced_us - wm_fr_was.paced_us;
	o->w.waits = wm_fr.waits - wm_fr_was.waits;
	o->w.wait_us = wm_fr.wait_us - wm_fr_was.wait_us;
	/* the most of each since it was last written */
	o->w.apart_max = wm_fr.apart_max;
	o->w.lay_max = wm_fr.lay_max;
	o->w.send_max = wm_fr.send_max;
	o->w.wait_max = wm_fr.wait_max;
	wm_fr.apart_max = wm_fr.lay_max = wm_fr.send_max = wm_fr.wait_max = 0;
	o->d = ds;
	o->d.flips = ds.flips - wm_fr_dwas.flips;
	for ( i = 0; i < 5; i++ ) o->d.apart[i] = ds.apart[i] - wm_fr_dwas.apart[i];
	for ( i = 0; i < 4; i++ ) o->d.phase[i] = ds.phase[i] - wm_fr_dwas.phase[i];
	o->d.due = ds.due - wm_fr_dwas.due;
	o->d.taken = ds.taken - wm_fr_dwas.taken;
	o->d.waited = ds.waited - wm_fr_dwas.waited;
	o->d.spin_us = ds.spin_us - wm_fr_dwas.spin_us;
	o->d.sleep_us = ds.sleep_us - wm_fr_dwas.sleep_us;
	o->d.off = ds.off - wm_fr_dwas.off;
	wm_fr_t0 = now;
	wm_fr_was = wm_fr;
	wm_fr_dwas = ds;
	return ( o->w.builds * 1000 >= (UD)WM_FRAMES_MIN * o->ms );
}

/* A build's laying and sending counted, us */
LOCAL void frames_lay( UD lay, UD send )
{
	wm_fr.lay_us += lay;
	if ( lay > wm_fr.lay_max ) wm_fr.lay_max = lay;
	wm_fr.send_us += send;
	if ( send > wm_fr.send_max ) wm_fr.send_max = send;
}

LOCAL void frames_print( CONST WMFRAMES *o )
{
	UD	ms = ( o->ms != 0 ) ? o->ms : 1;
	UD	nb = ( o->w.builds != 0 ) ? o->w.builds : 1;
	UD	na = ( o->w.apart != 0 ) ? o->w.apart : 1;
	UD	nw = ( o->w.waits != 0 ) ? o->w.waits : 1;
	UD	nd = ( o->d.waited != 0 ) ? o->d.waited : 1;

	tm_printf((UB *)"wm: frames: in %ld ms %ld builds, %ld.%ld a second; %ld us apart (most %ld), "
		  "%ld over a refresh and a half; laying %ld us (most %ld), sending %ld us (most %ld), "
		  "asleep for the pace %ld us, a build each; %ld waits for a build, %ld us (most %ld)\n",
		  o->ms, o->w.builds, o->w.builds * 1000 / ms, o->w.builds * 10000 / ms % 10,
		  o->w.apart_us / na, o->w.apart_max, o->w.late,
		  o->w.lay_us / nb, o->w.lay_max, o->w.send_us / nb, o->w.send_max,
		  o->w.paced_us / nb, o->w.waits, o->w.wait_us / nw, o->w.wait_max);
	if ( o->d.flips == 0 ) {
		return;				/* one page: nothing of flips to say */
	}
	tm_printf((UB *)"disp: frames: refresh %ld us%s; %ld flips, refreshes apart 0:%ld 1:%ld 2:%ld 3:%ld 4+:%ld; "
		  "quarter after the blank %ld/%ld/%ld/%ld; %ld sends during a flip: %ld taken by the clock, "
		  "%ld waited (watching %ld us, asleep %ld us each), %ld blanks off the clock\n",
		  ( o->d.frame_ns != 0 ) ? o->d.frame_ns / 1000 : (UD)WM_FRAME_US,
		  ( o->d.frame_ns != 0 ) ? "" : " (taken to be)", o->d.flips,
		  o->d.apart[0], o->d.apart[1], o->d.apart[2], o->d.apart[3], o->d.apart[4],
		  o->d.phase[0], o->d.phase[1], o->d.phase[2], o->d.phase[3],
		  o->d.due, o->d.taken, o->d.waited, o->d.spin_us / nd, o->d.sleep_us / nd, o->d.off);
}

/*
 * How far in from each edge of each corner the lines run parallel to
 * it, for bending them round the corners: the whole corner where the
 * edge is plain -- the frame's strip and whatever is drawn inside it
 * along the edge, such as the thick border a panel draws round its
 * work area, which the cut of the corner would otherwise break -- but
 * where a scroll bar fills the strip or the title band lies along the
 * top only the outline and the line inside it (the bar and the band are
 * not lines, and the band carries the pictogram).
 */
LOCAL void frame_lines( CONST WMWIN *w, T_CPLINES *ln )
{
	INT	l = CP_ROUND_MAX, r = CP_ROUND_MAX, t = CP_ROUND_MAX, b = CP_ROUND_MAX;

	if ( ( w->attr & WM_ATTR_LBAR ) != 0 ) l = 2;
	if ( ( w->attr & WM_ATTR_RBAR ) != 0 ) r = 2;
	if ( ( w->attr & WM_ATTR_TITLE ) != 0 ) t = 2;
	if ( ( w->attr & WM_ATTR_BBAR ) != 0 ) b = 2;
	if ( wm_scale != 100 ) {
		/* the two lines of each side, as many pixels as they cover */
		if ( l == 2 ) l = dv(2);
		if ( r == 2 ) r = dv(2);
		if ( t == 2 ) t = dv(2);
		if ( b == 2 ) b = dv(2);
	}
	ln->side[0] = l;  ln->end[0] = t;
	ln->side[1] = r;  ln->end[1] = t;
	ln->side[2] = l;  ln->end[2] = b;
	ln->side[3] = r;  ln->end[3] = b;
}

/*
 * The parts of the screen asked for built afresh, each by itself, and
 * the pointer brought to where it is now.
 *
 * Each part is laid the same way -- the ground, then the windows from
 * the back forward -- because a screen built two different ways is two
 * different screens. Each window goes down in the stages of compose.c:
 * its shadow, what shows through it spread, its pixels mixed over that
 * and its frame washed. A window that some single window in front of
 * it covers with a solid part, its shadow and all, is not laid down at
 * all, which is what the order is for; of the rest, each is laid only
 * where the windows in front of it do not lay their pixels solid over
 * it (compose.c cp_lay).
 *
 * Where a GPU composites (compose.h T_CPGPU), each part is put to it on
 * its own: one large enough to be worth it is widened to the GPU's step
 * and the GPU lays the ground and the windows up to the first that pops
 * up, the stages here the rest over them; a small one is built here.
 * A spread needs what lies round what it spreads, so when a window seen
 * through stands near a part, a margin as wide as the spread reaches is
 * built round it as well, from the ground up like the rest, and what
 * the back buffer held there is put back afterwards.
 *
 * The pointer lies in the back buffer with what it covers kept
 * (pointer.c). It is taken up first when a part to be built (with its
 * margin) touches it, or when it is to be laid elsewhere, in another
 * shape or not at all; it is laid again last, over the outlines of
 * what is carried. With no part asked for this moves the pointer and
 * nothing else: two small squares sent and no window laid.
 */
LOCAL ER compose_parts( CONST PARTS *ask )
{
	PLAN		plan[WM_PARTS];
	T_DPRECT	box, was, want;
	LOOKNOW		look;
	UD		now;
	UB		*back;
	CONST T_CPGPU	*gpu;
	BOOL		on, wanted, lift, moved, print, fprint;
	INT		i, z, np = 0, sw, sh, margin;
	WMSTATS		st;
	WMFRAMES	fs;
	UD		t0, t1, t2;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	(void)ts_get_mono(&t0);
	ts_disp_hold();
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	back = (UB *)ts_disp_buffer();		/* under the lock: the screen's size may change */
	if ( back == NULL ) {
		tk_unl_mtx(wm_mtxid);
		ts_disp_release();
		return E_NOEXS;
	}
	sw = (INT)wm_spec.width;
	sh = (INT)wm_spec.height;
	look_now(&look);
	now = now_ms();

	gpu = cp_gpu_get();
	if ( gpu != NULL && wm_num(LK_GPU, 1) == 0 ) {
		gpu = NULL;
	}
	for ( i = 0; i < ask->n; i++ ) {
		PLAN	*pl = &plan[np];

		scr_dev(&ask->r[i], &box);	/* the parts are in view pixels, the build in the screen's */
		if ( box.left < 0 )    box.left = 0;
		if ( box.top < 0 )     box.top = 0;
		if ( box.right > sw )  box.right = sw;
		if ( box.bottom > sh ) box.bottom = sh;
		if ( box.right <= box.left || box.bottom <= box.top ) {
			continue;			/* nothing of it is on the screen */
		}
		np++;

		/*
		 * What the GPU builds starts on a whole number of its steps,
		 * so a part it builds is widened to that: a part built wider
		 * is the same screen, and goes to the screen with the rest.
		 */
		pl->gpu = ( gpu != NULL && gpu->wants(&box, look_fx(&look)) ) ? gpu : NULL;
		if ( pl->gpu != NULL && pl->gpu->align > 1 ) {
			INT	a = (INT)pl->gpu->align;

			box.left -= box.left % a;
			box.right += ( a - box.right % a ) % a;
			if ( box.right > sw ) box.right = sw;
		}
		pl->box = box;
		pl->abox = box;
		margin = spread_margin(&box, &look, now);
		if ( margin > 0 ) {
			pl->abox.left -= margin;    pl->abox.top -= margin;
			pl->abox.right += margin;   pl->abox.bottom += margin;
			if ( pl->abox.left < 0 )    pl->abox.left = 0;
			if ( pl->abox.top < 0 )     pl->abox.top = 0;
			if ( pl->abox.right > sw )  pl->abox.right = sw;
			if ( pl->abox.bottom > sh ) pl->abox.bottom = sh;
			if ( pl->gpu != NULL && pl->gpu->align > 1 ) {
				pl->abox.left -= pl->abox.left % (INT)pl->gpu->align;
			}
		}
	}

	/* the pointer: where it lies, where it is to lie, and whether it is in the way */
	on = knl_pt_on(&was);
	wanted = knl_pt_want(&want);
	lift = ( on && !( wanted && knl_pt_fresh(&want) ) );
	for ( i = 0; on && !lift && i < np; i++ ) {
		lift = rect_meets(&was, &plan[i].abox);
	}
	if ( np == 0 && !lift && ( on || !wanted ) ) {
		tk_unl_mtx(wm_mtxid);
		ts_disp_release();
		return E_OK;			/* nothing changed at all */
	}
	moved = ( wanted && ( !on || was.left != want.left || was.top != want.top
			      || was.right != want.right ) );
	if ( lift ) {
		T_DISPRECT	d;

		knl_pt_lift(back, wm_spec.pitch, sw, sh);
		d.left = was.left;  d.top = was.top;
		d.right = was.right;  d.bottom = was.bottom;
		ts_disp_damage(&d);
	}

	if ( np > 0 ) {
		ground_make();

		/*
		 * The frames' lines bent round the corners, in the windows' own
		 * pixels, before anything reads them: what a program drew into
		 * a corner since is put right, and the GPU and the processor
		 * read the same pixels
		 */
		if ( look.round > 0 ) {
			for ( z = 0; z < wm_nwin; z++ ) {
				WMWIN		*w = win_at_z(z);
				T_DPRECT	wd;

				if ( w == NULL || !w->visible || w->pixels == NULL || w->own != NULL
				  || ( w->attr & ( WM_ATTR_FRAME | WM_ATTR_TITLE ) ) == 0 ) {
					continue;
				}
				win_dev(&w->outer, &wd);
				for ( i = 0; i < np; i++ ) {
					if ( rect_meets(&wd, &plan[i].abox) ) {
						T_CPLINES	ln;

						frame_lines(w, &ln);
						cp_corner_lines(w->pixels, wm_wpitch, &wd,
								scaled_len(look.round, CP_ROUND_MAX), &ln);
						break;
					}
				}
			}
		}
		for ( i = 0; i < np; i++ ) {
			build_part(&plan[i], back, &look, now);
		}
		wm_count.composites++;
	}

	/* the pointer goes on last, over everything */
	if ( wanted && !knl_pt_on(NULL) ) {
		T_DISPRECT	d;

		knl_pt_lay(back, wm_spec.pitch, sw, sh, &want);
		d.left = want.left;  d.top = want.top;
		d.right = want.right;  d.bottom = want.bottom;
		ts_disp_damage(&d);
	}
	if ( moved ) {
		wm_count.ptr_moves++;
		if ( np > 0 ) {
			wm_count.ptr_built++;
		}
	}
	print = stats_take(now, &st);
	fprint = frames_take(now, &fs);
	tk_unl_mtx(wm_mtxid);
	(void)ts_get_mono(&t1);
	ts_disp_release();
	(void)ts_get_mono(&t2);
	frames_lay(( t1 - t0 ) / 1000, ( t2 - t1 ) / 1000);
	if ( print ) {
		stats_print(&st);
	}
	if ( fprint ) {
		frames_print(&fs);
	}

	return E_OK;
}

/* ---------------------------------------------------------------- the compositor */

/*
 * The screen is built by one task of its own, the compositor, and by no
 * one else. Whoever changes something says so -- a part of the screen
 * to be built (wm_composite_at), the whole of it (wm_composite), or
 * whatever has changed (wm_update) -- and the compositor builds what has
 * been asked for since it last built, all of it at once, at most once a
 * refresh of the screen. Ten changes made in the time the screen takes
 * to show one are built once. Where the screen flips and says when a
 * flip was taken, the send of a build waits for the flip before, and
 * that keeps the pace in step with the screen; elsewhere the builds keep
 * to places WM_FRAME_US apart. A build begins at once when none has
 * been made for a refresh; the asks that come while one is being made,
 * or before a refresh is over, wait for the next.
 *
 * What has changed is gathered when the build begins, not when it is
 * asked for: the damage the windows were given (wm_damage), the places
 * windows left or changed (wm_gone), and the rectangles drawn into
 * windows' pixels through their environments (dp_watch), each with as
 * much round it as a spread over it shows of the change. What a task
 * draws is kept apart, its own, until that task asks for a build: a
 * build another asks for, or the pointer moving, does not show a window
 * half drawn -- its page cleared and its letters not yet on it.
 *
 * wm_update, wm_composite and wm_composite_at return once what they asked
 * for is on the screen: what a caller draws next cannot be caught half
 * drawn, and a test reads the back buffer as the call left it. wm_post
 * asks for whatever has changed and returns at once, for one that does
 * not draw again straight away (a window coming in step by step);
 * wm_flush waits for everything asked so far. The pointer is laid at
 * each build where the input says it is; a move of it only wakes
 * the compositor (wm_pointer_moved, and the input layer itself through
 * knl_wm_pointer_hint), so the pointer follows the hand whatever the
 * program that moves it is doing.
 *
 * The compositor owns the FP/SIMD registers (TA_FPU), so the stages use
 * the vector unit there (compose_simd.c). Its priority is below the
 * input and the GPU, which it waits for, and above every program.
 */
#define WM_CP_PRI	10		/* below the input (8) and the GPU (9), above the programs */
#define WM_CP_STACK	( 64 * 1024 )
#define WM_WAITERS	16		/* callers waiting at once for a build, each with a semaphore */

#define ASK_UPD		0x01		/* whatever has changed */
#define ASK_ALL		0x02		/* the whole screen */

LOCAL PARTS	wm_ask;			/* the parts asked for, not yet built */
LOCAL UINT	wm_ask_what = 0;	/* and ASK_* */
LOCAL UW	wm_ask_seq = 0;		/* asks made; under the lock */
LOCAL UW	wm_built_seq = 0;	/* of those, built */
LOCAL ID	wm_cp_tsk = 0;
LOCAL UD	wm_cp_due = 0;		/* the earliest the next build may begin, ns */
LOCAL UD	wm_cp_began = 0;	/* when the last build began, ns */
LOCAL UW	wm_ptr_hint = 0;	/* the pointer moved since the compositor last looked */

/*
 * What was drawn into windows' pixels, each task's apart (a task with no
 * place of its own shares the last), under a lock of its own taken last
 * of all; and what the tasks that asked had drawn, to be built.
 */
#define WM_DRAWERS	16

typedef struct {
	ID	tid;			/* 0: free */
	PARTS	p;
} DRAWN;

LOCAL DRAWN	wm_drawn[WM_DRAWERS];
LOCAL PARTS	wm_drawn_any;
LOCAL ID	wm_touch_mtx = 0;
LOCAL PARTS	wm_ask_chg;		/* drawn and asked for; under the lock */

typedef struct {
	BOOL	used;
	BOOL	told;
	UW	seq;			/* waiting for this ask to be built */
	ID	sem;
} WMWAIT;

LOCAL WMWAIT	wm_wait[WM_WAITERS];

LOCAL BOOL seq_reached( UW done, UW seq )
{
	return (BOOL)( (INT)( done - seq ) >= 0 );
}

/*
 * A rectangle drawn into a window's pixels (dp_watch). One that lies
 * inside one kept already, which is most of them -- the letters of a
 * line, the parts of a frame -- changes nothing.
 */
LOCAL void touched( CONST T_DPRECT *r )
{
	PARTS	*p = &wm_drawn_any;
	ID	tid = tk_get_tid();
	INT	i, k, spare = -1;

	if ( wm_touch_mtx <= 0 || r->right <= r->left || r->bottom <= r->top ) {
		return;
	}
	tk_loc_mtx(wm_touch_mtx, TMO_FEVR);
	for ( k = 0; k < WM_DRAWERS; k++ ) {
		if ( wm_drawn[k].tid == tid ) {
			break;
		}
		if ( wm_drawn[k].tid == 0 && spare < 0 ) {
			spare = k;
		}
	}
	if ( k < WM_DRAWERS ) {
		p = &wm_drawn[k].p;
	} else if ( spare >= 0 ) {
		wm_drawn[spare].tid = tid;
		wm_drawn[spare].p.n = 0;
		p = &wm_drawn[spare].p;
	}
	for ( i = p->n - 1; i >= 0; i-- ) {
		if ( rect_covers(&p->r[i], r) ) {
			break;
		}
	}
	if ( i < 0 ) {
		parts_add(p, r);
	}
	tk_unl_mtx(wm_touch_mtx);
}

/*
 * What the task asking has drawn, and what tasks with no place of their
 * own have, given over to be built. Called with the lock held.
 */
LOCAL void drawn_give( void )
{
	ID	tid = tk_get_tid();
	INT	i, k;

	if ( wm_touch_mtx <= 0 ) {
		return;
	}
	tk_loc_mtx(wm_touch_mtx, TMO_FEVR);
	for ( k = 0; k < WM_DRAWERS; k++ ) {
		if ( wm_drawn[k].tid == tid ) {
			for ( i = 0; i < wm_drawn[k].p.n; i++ ) {
				parts_add(&wm_ask_chg, &wm_drawn[k].p.r[i]);
			}
			wm_drawn[k].p.n = 0;
			wm_drawn[k].tid = 0;
		}
	}
	for ( i = 0; i < wm_drawn_any.n; i++ ) {
		parts_add(&wm_ask_chg, &wm_drawn_any.r[i]);
	}
	wm_drawn_any.n = 0;
	tk_unl_mtx(wm_touch_mtx);
}

/*
 * What is to be built now, taken from what was asked: the number of the
 * last ask it answers. Called with the lock held.
 */
LOCAL UW take_asks( PARTS *p )
{
	LOOKNOW		look;
	T_DPRECT	r;
	UD		now;
	UINT		what = wm_ask_what;
	INT		i;

	p->n = 0;
	look_now(&look);
	now = now_ms();
	if ( ( what & ASK_ALL ) != 0
	  || ( wm_ask_seq != wm_built_seq && !wm_pointer_last(NULL, NULL, NULL) ) ) {
		/* all of it (the pointer never laid: everything is laid) */
		r.left = 0;  r.top = 0;
		r.right = wm_vw;
		r.bottom = wm_vh;
		parts_add(p, &r);
		wm_gone.n = 0;
	} else {
		for ( i = 0; i < wm_ask.n; i++ ) {
			parts_add(p, &wm_ask.r[i]);
		}
	}
	if ( ( what & ASK_UPD ) != 0 ) {
		for ( i = 0; i < wm_wcap; i++ ) {
			WMWIN	*w = &WMW(i);

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
			spread_add(p, &r, &look, now);
		}
		for ( i = 0; i < wm_gone.n; i++ ) {
			spread_add(p, &wm_gone.r[i], &look, now);
		}
		wm_gone.n = 0;
	}

	/* what the tasks that asked had drawn into the windows */
	for ( i = 0; i < wm_ask_chg.n; i++ ) {
		spread_add(p, &wm_ask_chg.r[i], &look, now);
	}

	wm_ask_chg.n = 0;
	wm_ask.n = 0;
	wm_ask_what = 0;
	return wm_ask_seq;
}

#if USE_UITEST
EXPORT volatile UW	knl_wm_torn = 0;
#endif

/* One build of what was asked, and those who waited for it told */
LOCAL void build_asked( void )
{
	PARTS	p;
	UW	seq;
	INT	i;
	UD	t, d, frame;

	/* how far from the one before it began */
	(void)ts_get_mono(&t);
	wm_fr.builds++;
	if ( wm_cp_began != 0 && t > wm_cp_began && t - wm_cp_began < (UD)WM_APART_MS * 1000000 ) {
		d = ( t - wm_cp_began ) / 1000;
		frame = ts_disp_paced();
		if ( frame == 0 ) frame = WM_FRAME_US;
		wm_fr.apart++;
		wm_fr.apart_us += d;
		if ( d > wm_fr.apart_max ) wm_fr.apart_max = d;
		if ( d * 2 > frame * 3 ) wm_fr.late++;
	}
	wm_cp_began = t;

	(void)__atomic_exchange_n(&wm_ptr_hint, 0, __ATOMIC_ACQ_REL);
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	seq = take_asks(&p);
	tk_unl_mtx(wm_mtxid);

#if USE_UITEST
	{
		/* a build that read the windows while a panel was being drawn, counted for pnstat */
		IMPORT volatile UW	knl_pn_draws, knl_pn_drawing;
		UW	d0 = knl_pn_draws, b0 = knl_pn_drawing;

		(void)compose_parts(&p);
		if ( b0 != 0 || knl_pn_drawing != 0 || knl_pn_draws != d0 ) knl_wm_torn++;
	}
#else
	(void)compose_parts(&p);
#endif

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( seq_reached(seq, wm_built_seq) ) {
		wm_built_seq = seq;
	}
	for ( i = 0; i < WM_WAITERS; i++ ) {
		WMWAIT	*k = &wm_wait[i];

		if ( k->used && !k->told && seq_reached(wm_built_seq, k->seq) ) {
			k->told = TRUE;
			(void)tk_sig_sem(k->sem, 1);
		}
	}
	tk_unl_mtx(wm_mtxid);
}

LOCAL void cp_task( INT stacd, void *exinf )
{
	UD	t, t2, gap;
	BOOL	more;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		(void)tk_slp_tsk(TMO_FEVR);
		for ( ;; ) {
			tk_loc_mtx(wm_mtxid, TMO_FEVR);
			more = (BOOL)( wm_ask_seq != wm_built_seq );
			tk_unl_mtx(wm_mtxid);
			if ( !more && __atomic_load_n(&wm_ptr_hint, __ATOMIC_ACQUIRE) == 0 ) {
				break;
			}

			/*
			 * At most one build a refresh: what comes meanwhile is
			 * built with the next. Where the sends wait for the flip
			 * before (ts_disp_paced), they keep that pace themselves,
			 * in step with the screen, and a build begins as soon as
			 * it is asked for; a pace kept here as well would put a
			 * wait of its own before theirs. Else the builds keep to
			 * places a refresh apart, each from where the last was
			 * due rather than from when it began, slept for to the
			 * microsecond, so that they come sixty times a second and
			 * not fewer.
			 */
			(void)ts_get_mono(&t);
			gap = ( ts_disp_paced() != 0 ) ? 0 : (UD)WM_FRAME_US * 1000;
			if ( gap != 0 && wm_cp_due != 0 && t < wm_cp_due ) {
				(void)tk_dly_tsk_u((RELTIM_U)( ( wm_cp_due - t + 999 ) / 1000 ));
				(void)ts_get_mono(&t2);
				wm_fr.paced_us += ( t2 - t ) / 1000;
				t = t2;
			}
			if ( gap != 0 && wm_cp_due != 0 && t < wm_cp_due + gap ) {
				/* in its place: the next a refresh on, but never within half of one */
				wm_cp_due += gap;
				if ( wm_cp_due < t + gap / 2 ) wm_cp_due = t + gap / 2;
			} else {
				wm_cp_due = t + gap;
			}
			build_asked();
		}
	}
}

LOCAL void cp_wake( void )
{
	if ( wm_cp_tsk > 0 ) {
		(void)tk_wup_tsk(wm_cp_tsk);
	}
}

/* An ask put in: its number, the compositor woken */
LOCAL UW ask( UINT what, CONST PARTS *parts )
{
	UW	seq;
	INT	i;

	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	wm_ask_what |= what;
	for ( i = 0; parts != NULL && i < parts->n; i++ ) {
		parts_add(&wm_ask, &parts->r[i]);
	}
	drawn_give();
	seq = ++wm_ask_seq;
	tk_unl_mtx(wm_mtxid);
	cp_wake();

	return seq;
}

/*
 * Until the ask 'seq' is built. With no compositor -- before it is made,
 * or when it could not be -- and in the compositor itself, the caller
 * builds it, as everyone did before there was one.
 */
LOCAL void wait_built( UW seq )
{
	WMWAIT	*k = NULL;
	INT	i;
	UD	t0, t1, d;

	if ( wm_cp_tsk <= 0 || tk_get_tid() == wm_cp_tsk ) {
		build_asked();
		return;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( seq_reached(wm_built_seq, seq) ) {
		tk_unl_mtx(wm_mtxid);
		return;
	}
	for ( i = 0; i < WM_WAITERS; i++ ) {
		if ( !wm_wait[i].used && wm_wait[i].sem > 0 ) {
			k = &wm_wait[i];
			k->used = TRUE;
			k->told = FALSE;
			k->seq = seq;
			break;
		}
	}
	tk_unl_mtx(wm_mtxid);

	if ( k == NULL ) {
		/* more waiting than there are places: looked at each millisecond */
		for ( ;; ) {
			BOOL	done;

			tk_loc_mtx(wm_mtxid, TMO_FEVR);
			done = seq_reached(wm_built_seq, seq);
			tk_unl_mtx(wm_mtxid);
			if ( done ) {
				return;
			}
			cp_wake();
			(void)tk_dly_tsk(1);
		}
	}
	(void)ts_get_mono(&t0);
	(void)tk_wai_sem(k->sem, 1, TMO_FEVR);
	(void)ts_get_mono(&t1);
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	k->used = FALSE;
	d = ( t1 - t0 ) / 1000;
	wm_fr.waits++;
	wm_fr.wait_us += d;
	if ( d > wm_fr.wait_max ) wm_fr.wait_max = d;
	tk_unl_mtx(wm_mtxid);
}

/*
 * The screen, or a part of it, built afresh: 'area' is what is to be
 * built, NULL the whole screen. Building only part of it is what keeps
 * a change to a corner of the screen cheap; the rest is not touched.
 * Returns once it is built.
 */
EXPORT ER wm_composite_at( CONST T_DPRECT *area )
{
	PARTS	p;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( area == NULL ) {
		wait_built(ask(ASK_ALL, NULL));
		return E_OK;
	}
	p.n = 0;
	parts_add(&p, area);
	wait_built(ask(0, &p));

	return E_OK;
}

/*
 * The whole screen built afresh, for what changes all of it: the look,
 * the scheme, the wallpaper, the screen's size. Returns once it is built.
 */
EXPORT ER wm_composite( void )
{
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	wait_built(ask(ASK_ALL, NULL));

	return E_OK;
}

/* The four edges of each outline, as parts of the screen */
LOCAL void drag_parts( PARTS *p, CONST T_DPRECT *r, INT n )
{
	T_DPRECT	e;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( drag_pict(i) ) {
			parts_add(p, &r[i]);	/* a picture covers all of it */
			continue;
		}
		e = r[i];
		e.bottom = e.top + 1;
		parts_add(p, &e);
		e = r[i];
		e.top = e.bottom - 1;
		parts_add(p, &e);
		e = r[i];
		e.right = e.left + 1;
		parts_add(p, &e);
		e = r[i];
		e.left = e.right - 1;
		parts_add(p, &e);
	}
}

/*
 * The outlines of what is being carried, in screen coordinates, or none
 * (n 0). The edges where they were and where they are now are built
 * again, and nothing else: what lies inside an outline is not touched
 * by it. Returns once they are on the screen.
 */
EXPORT ER wm_set_drag( CONST T_DPRECT *r, INT n )
{
	PARTS	p;
	INT	i;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( n < 0 || n > WM_DRAG_MAX || ( n > 0 && r == NULL ) ) {
		return E_PAR;
	}
	p.n = 0;
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	drag_parts(&p, wm_drag, wm_ndrag);
	for ( i = 0; i < n; i++ ) {
		wm_drag[i] = r[i];
	}
	wm_ndrag = n;
	drag_parts(&p, wm_drag, wm_ndrag);
	if ( n == 0 ) {
		drag_pict_free();	/* the carry is over */
	}
	tk_unl_mtx(wm_mtxid);

	if ( p.n == 0 ) {
		return E_OK;
	}
	wait_built(ask(0, &p));

	return E_OK;
}

EXPORT BOOL knl_wm_carrying( void )
{
	BOOL	on;

	if ( !wm_ready ) {
		return FALSE;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	on = (BOOL)( wm_ndrag > 0 );
	tk_unl_mtx(wm_mtxid);

	return on;
}

/*
 * What the things about to be carried look like: each rectangle of the
 * screen (where the thing lies now, in screen coordinates) copied as it
 * is shown, without the pointer, to be laid in place of the outline
 * wm_set_drag gives for it at the same place in the list. A rectangle
 * not wholly on the screen, or past WM_DRAG_PX_MAX pixels in all, keeps
 * its outline. They are let go with the outlines (wm_set_drag n 0).
 */
EXPORT ER wm_set_drag_from( CONST T_DPRECT *r, INT n )
{
	UW	*px[WM_DRAG_MAX];
	INT	i, w, h;
	SZ	total = 0;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( n < 0 || n > WM_DRAG_MAX || ( n > 0 && r == NULL ) ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) {
		px[i] = NULL;
		w = r[i].right - r[i].left;
		h = r[i].bottom - r[i].top;
		if ( w <= 0 || h <= 0 || r[i].left < 0 || r[i].top < 0
		  || r[i].right > wm_vw || r[i].bottom > wm_vh
		  || total + (SZ)w * h > WM_DRAG_PX_MAX ) {
			continue;
		}
		px[i] = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
		if ( px[i] == NULL ) {
			continue;
		}
		if ( wm_grab_screen(px[i], w, &r[i], FALSE) < E_OK ) {
			Kfree(px[i]);
			px[i] = NULL;
			continue;
		}
		total += (SZ)w * h;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	drag_pict_free();
	for ( i = 0; i < n; i++ ) {
		wm_dpx[i] = px[i];
		wm_dpw[i] = r[i].right - r[i].left;
		wm_dph[i] = r[i].bottom - r[i].top;
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * A picture of w by h pixels laid on the screen with its corner at
 * (x, y), over every window and the outlines, or none (px NULL). Where
 * it was and where it is now are built again. Returns once it is on the
 * screen.
 */
EXPORT ER wm_set_loupe( CONST UW *px, INT w, INT h, INT x, INT y )
{
	PARTS	p;
	INT	i;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( px != NULL && ( w <= 0 || h <= 0 || w > WM_LOUPE_MAX || h > WM_LOUPE_MAX ) ) {
		return E_PAR;
	}
	if ( px != NULL && wm_loupe == NULL ) {
		wm_loupe = (UW *)Kmalloc((SZ)WM_LOUPE_MAX * WM_LOUPE_MAX * sizeof(UW));
		if ( wm_loupe == NULL ) {
			return E_NOMEM;
		}
	}
	p.n = 0;
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	if ( wm_loupe_on ) {
		parts_add(&p, &wm_loupe_at);
	}
	wm_loupe_on = (BOOL)( px != NULL );
	if ( px != NULL ) {
		for ( i = 0; i < w * h; i++ ) {
			wm_loupe[i] = px[i];
		}
		wm_loupe_at.left = x;
		wm_loupe_at.top = y;
		wm_loupe_at.right = x + w;
		wm_loupe_at.bottom = y + h;
		parts_add(&p, &wm_loupe_at);
	}
	tk_unl_mtx(wm_mtxid);

	if ( p.n == 0 ) {
		return E_OK;
	}
	wait_built(ask(0, &p));

	return E_OK;
}

/*
 * The pointer moved: what it covered where it was is put back, and it
 * is laid where it is now, what it covers there kept first. No window
 * is laid again, and the two squares are all that goes to the screen.
 *
 * A screen is a megapixel and the pointer is a few hundred, so the
 * difference between this and building the screen is the difference
 * between a pointer that follows the hand and one that catches up
 * afterwards. It is laid where the input layer says it is when the
 * compositor gets to it, which is where the hand has got to by then, so
 * to_x and to_y only say that it moved. Returns at once; wm_flush waits
 * until it is laid.
 */
EXPORT ER wm_pointer_moved( INT to_x, INT to_y )
{
	(void)to_x;
	(void)to_y;
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	(void)ask(0, NULL);

	return E_OK;
}

/*
 * The input layer saw the pointer move. Called from its task, which is
 * not to wait for anything here: no lock is taken, the compositor is
 * only woken.
 */
EXPORT void knl_wm_pointer_hint( void )
{
	if ( !wm_ready || wm_cp_tsk <= 0 ) {
		return;
	}
	__atomic_store_n(&wm_ptr_hint, 1, __ATOMIC_RELEASE);
	cp_wake();
}

/*
 * Whatever has changed since the screen was last built, built again,
 * and only that: the parts of windows that were drawn in, and the places
 * windows left or changed (wm_gone), each with as much round it as a
 * window spread over it shows of the change; and the pointer moved. A
 * menu whose lit row follows the pointer changes a row at a time, and
 * building a megapixel for each row is what makes a menu fall seconds
 * behind the hand. Returns once it is built.
 */
EXPORT ER wm_update( void )
{
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	wait_built(ask(ASK_UPD, NULL));

	return E_OK;
}

/* The same, without waiting for it */
EXPORT ER wm_post( void )
{
	if ( !wm_ready ) {
		return E_NOEXS;
	}
	if ( wm_cp_tsk <= 0 ) {
		return wm_update();
	}
	(void)ask(ASK_UPD, NULL);

	return E_OK;
}

/* Until everything asked for so far is on the screen */
EXPORT ER wm_flush( void )
{
	UW	seq;

	if ( !wm_ready ) {
		return E_NOEXS;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	seq = wm_ask_seq;
	tk_unl_mtx(wm_mtxid);
	if ( wm_cp_tsk > 0 && tk_get_tid() != wm_cp_tsk ) {
		wait_built(seq);
	} else if ( !seq_reached(wm_built_seq, seq) ) {
		build_asked();
	}
	return E_OK;
}

/*
 * The compositor, made once the windows are: its own task, owning the
 * FP/SIMD registers, and the semaphores its callers wait on. Without it
 * every caller builds for itself.
 */
LOCAL void cp_start( void )
{
	T_CTSK	ct;
	T_CSEM	cs;
	T_CMTX	cm;
	INT	i;
	ID	tid;

	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	wm_touch_mtx = tk_cre_mtx(&cm);
	if ( wm_touch_mtx <= 0 ) {
		wm_touch_mtx = 0;
		return;
	}
	knl_memset(wm_drawn, 0, sizeof(wm_drawn));
	wm_drawn_any.n = 0;
	wm_ask_chg.n = 0;
	dp_watch(touched);

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 1;
	for ( i = 0; i < WM_WAITERS; i++ ) {
		wm_wait[i].used = FALSE;
		wm_wait[i].sem = tk_cre_sem(&cs);
		if ( wm_wait[i].sem < 0 ) {
			wm_wait[i].sem = 0;
		}
	}

	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)cp_task;
	ct.itskpri = WM_CP_PRI;
	ct.stksz = WM_CP_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 ) {
		return;
	}
	cp_simd_allow(tid, TRUE);
	wm_cp_tsk = tid;
	if ( tk_sta_tsk(tid, 0) < E_OK ) {
		wm_cp_tsk = 0;
		cp_simd_allow(tid, FALSE);
		(void)tk_del_tsk(tid);
	}
}

/* The compositor's task, for a test that needs to know what it does */
EXPORT ID knl_wm_compositor( void )
{
	return wm_cp_tsk;
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
		for ( i = 0; i < wm_wcap; i++ ) {
			if ( WMW(i).used && WMW(i).z == z ) {
				seen++;
			}
		}
		if ( seen != 1 ) {
			faults++;
		}
	}
	for ( i = 0; i < wm_wcap; i++ ) {
		if ( !WMW(i).used ) {
			continue;
		}
		if ( WMW(i).z < 0 || WMW(i).z >= wm_nwin ) {
			faults++;
		}

		/* one out of sight for its group is a subordinate window, and not shown */
		if ( WMW(i).away && ( WMW(i).visible || WMW(i).parent <= 0 ) ) {
			faults++;
		}

		/* the work area is what the one function says it is */
		work_of(&WMW(i).outer, WMW(i).attr, &work);
		if ( work.left != WMW(i).work.left
		  || work.top != WMW(i).work.top
		  || work.right != WMW(i).work.right
		  || work.bottom != WMW(i).work.bottom ) {
			faults++;
		}

		/* the surface is large enough for the window it belongs to */
		if ( WMW(i).pix_pages < win_pages(&WMW(i).outer) ) {
			faults++;
		}
		if ( WMW(i).pixels == NULL ) {
			faults++;
		}
	}
	tk_unl_mtx(wm_mtxid);

	return faults;
}

EXPORT void wm_screen( INT *p_w, INT *p_h )
{
	*p_w = wm_ready ? wm_vw : 0;
	*p_h = wm_ready ? wm_vh : 0;
}

/* The main screen's size: its corner is the view's, so what is centred or kept on it needs no more */
EXPORT void wm_screen_main( INT *p_w, INT *p_h )
{
	INT	x, y;

	*p_w = *p_h = 0;
	(void)ts_disp_screen(0, &x, &y, p_w, p_h);
}

/* The screen a point of the view is on, or the nearest one */
EXPORT void wm_screen_at( INT px, INT py, T_DPRECT *r )
{
	INT	i, x = 0, y = 0, w = 0, h = 0;

	(void)py;
	for ( i = 0; ts_disp_screen(i, &x, &y, &w, &h) >= E_OK; i++ ) {
		if ( px < x + w ) break;
	}
	if ( i > 0 && i == ts_disp_screens() ) (void)ts_disp_screen(i - 1, &x, &y, &w, &h);
	r->left = x;
	r->top = y;
	r->right = x + w;
	r->bottom = y + h;
}

/*
 * The view rectangle r of the screen into dst, a pixel for each view
 * pixel, the first pixel of the screen it covers (cut to the screen);
 * without the pointer where 'pointer' is FALSE. With the lock held.
 */
IMPORT BOOL knl_pt_under_px( INT X, INT Y, UW *c );

LOCAL void grab_scaled( UB *back, UW *dst, INT pitch, CONST T_DPRECT *r, BOOL pointer )
{
	INT	x, y;

	for ( y = r->top; y < r->bottom; y++ ) {
		INT	Y = dv(y);

		if ( Y >= (INT)wm_spec.height ) Y = (INT)wm_spec.height - 1;
		for ( x = r->left; x < r->right; x++ ) {
			INT	X = dv(x);
			UW	c;

			if ( X >= (INT)wm_spec.width ) X = (INT)wm_spec.width - 1;
			if ( pointer || !knl_pt_under_px(X, Y, &c) ) {
				c = *(UW *)( back + (UBINT)Y * wm_spec.pitch + (UBINT)X * sizeof(UW) );
			}
			dst[(SZ)( y - r->top ) * pitch + ( x - r->left )] = c;
		}
	}
}

EXPORT ER wm_copy_screen( UW *dst, INT pitch )
{
	UB	*back;
	INT	y;

	if ( !wm_ready || dst == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	back = (UB *)ts_disp_buffer();
	if ( back == NULL || pitch < wm_vw ) {
		tk_unl_mtx(wm_mtxid);
		return ( back == NULL ) ? E_NOEXS : E_PAR;
	}
	if ( wm_scale != 100 ) {
		T_DPRECT	all;

		all.left = 0;  all.top = 0;  all.right = wm_vw;  all.bottom = wm_vh;
		grab_scaled(back, dst, pitch, &all, TRUE);
		tk_unl_mtx(wm_mtxid);
		return E_OK;
	}
	for ( y = 0; y < (INT)wm_spec.height; y++ ) {
		knl_memcpy(dst + (SZ)y * pitch, back + (UBINT)y * wm_spec.pitch,
			   (SZ)wm_spec.width * sizeof(UW));
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * A rectangle of the screen as last built, copied under the same lock as
 * wm_copy_screen. Without the pointer, the pixels it lies on are those it
 * covers, kept as it was laid (knl_pt_under), so the copy is what the
 * screen shows with the pointer taken away.
 */
EXPORT ER wm_grab_screen( UW *dst, INT pitch, CONST T_DPRECT *r, BOOL pointer )
{
	UB	*back;
	INT	y, w;

	if ( !wm_ready || dst == NULL || r == NULL
	  || r->left < 0 || r->top < 0 || r->right > wm_vw || r->bottom > wm_vh
	  || r->left >= r->right || r->top >= r->bottom || pitch < r->right - r->left ) {
		return E_PAR;
	}
	w = r->right - r->left;
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	back = (UB *)ts_disp_buffer();
	if ( back == NULL || r->right > wm_vw || r->bottom > wm_vh ) {
		tk_unl_mtx(wm_mtxid);
		return ( back == NULL ) ? E_NOEXS : E_PAR;
	}
	if ( wm_scale != 100 ) {
		grab_scaled(back, dst, pitch, r, pointer);
		tk_unl_mtx(wm_mtxid);
		return E_OK;
	}
	for ( y = r->top; y < r->bottom; y++ ) {
		knl_memcpy(dst + (SZ)( y - r->top ) * pitch,
			   back + (UBINT)y * wm_spec.pitch + (UBINT)r->left * sizeof(UW), (SZ)w * sizeof(UW));
	}
	if ( !pointer ) {
		knl_pt_under(dst, pitch, r);
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * The windows that can be shared, nearest the viewer first. Whose program
 * each is of is asked after the lock is let go: the process table has its
 * own lock.
 */
EXPORT INT wm_share_list( T_WMSHARE *v, INT max )
{
	INT	i, z, n = 0;
	ID	me = ts_get_pid();
	TS_UUID	mine, other;

	if ( v == NULL || max <= 0 || !wm_ready ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	for ( z = 0; z < wm_wcap && n < max; z++ ) {
		for ( i = 0; i < wm_wcap && n < max; i++ ) {
			WMWIN		*w = &WMW(i);
			T_WMSHARE	*s = &v[n];

			if ( !w->used || w->z != z || !w->visible || w->parent != 0
			  || ( w->attr & WM_ATTR_POPUP ) || !( w->attr & WM_ATTR_TITLE ) ) {
				continue;
			}
			s->wid = i + 1;
			s->serial = w->serial;
			s->gid = w->gid;
			s->pid = w->mkpid;
			s->w = w->work.right - w->work.left;
			s->h = w->work.bottom - w->work.top;
			s->flags = 0;
			knl_memcpy(s->title, w->title, WM_TITLE_MAX);
			s->title[WM_TITLE_MAX - 1] = 0;
			n++;
		}
	}
	tk_unl_mtx(wm_mtxid);
	knl_memset(&mine, 0, sizeof(mine));
	if ( me > 0 ) {
		(void)knl_prc_uuid(me, NULL, &mine);
	}
	for ( i = 0; i < n; i++ ) {
		if ( me > 0 && v[i].pid == me ) {
			v[i].flags |= WM_SHARE_OWN | WM_SHARE_SAMEPROG;
		} else if ( me > 0 && v[i].pid > 0 && knl_prc_uuid(v[i].pid, NULL, &other) >= E_OK
			 && ts_uuid_cmp(&mine, &other) == 0 ) {
			v[i].flags |= WM_SHARE_SAMEPROG;
		}
	}

	return n;
}

EXPORT ER wm_grab_rows( INT wid, UINT serial, UW *dst, INT pitch, INT top, INT rows, INT maxw, INT *p_w, INT *p_h )
{
	WMWIN	*w;
	INT	ww, wh, ox, oy, x, y;

	if ( dst == NULL || top < 0 || rows <= 0 || maxw <= 0 || pitch < maxw || !wm_ready ) {
		return E_PAR;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	w = win_of(wid);
	if ( w == NULL || w->serial != serial ) {
		tk_unl_mtx(wm_mtxid);
		return E_NOEXS;
	}
	if ( w->pixels == NULL ) {
		tk_unl_mtx(wm_mtxid);
		return E_OBJ;
	}
	ww = w->work.right - w->work.left;
	wh = w->work.bottom - w->work.top;
	ox = w->work.left - w->outer.left;
	oy = w->work.top - w->outer.top;
	if ( p_w != NULL ) *p_w = ww;
	if ( p_h != NULL ) *p_h = wh;
	if ( maxw > ww ) maxw = ww;
	for ( y = top; y < top + rows && y < wh; y++ ) {
		UW		*d = dst + (SZ)( y - top ) * pitch;

		if ( wm_scale != 100 ) {
			CONST UW	*s = (CONST UW *)( w->pixels + (UD)dv(oy + y) * wm_wpitch );

			for ( x = 0; x < maxw; x++ ) {
				d[x] = s[dv(ox + x)];
			}
		} else {
			row_copy(d, (CONST UW *)( w->pixels + (UD)( oy + y ) * wm_wpitch ) + ox, maxw);
		}
	}
	tk_unl_mtx(wm_mtxid);

	return E_OK;
}

/*
 * The window in front and those that belong with it: the nearest window
 * with a name band (front_window), taken back to the window it is
 * subordinate to, and every shown window subordinate to that one, at any
 * depth. The first is the one they belong to. 0 when there is none.
 */
EXPORT INT wm_front_group( INT *wids, INT max )
{
	INT	root, i, p, guard, n = 0;
	WMWIN	*w;

	if ( wids == NULL || max <= 0 || !wm_ready ) {
		return 0;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	root = front_window(0);
	for ( guard = 0; root != 0 && guard < wm_wcap; guard++ ) {
		w = win_of(root);
		if ( w == NULL || w->parent == 0 || win_of(w->parent) == NULL ) {
			break;
		}
		root = w->parent;
	}
	if ( root != 0 ) {
		wids[n++] = root;
		for ( i = 1; i <= wm_wcap && n < max; i++ ) {
			w = win_of(i);
			if ( w == NULL || i == root || !w->visible ) {
				continue;
			}
			for ( p = w->parent, guard = 0; p != 0 && p != root && guard < wm_wcap; guard++ ) {
				WMWIN	*q = win_of(p);

				p = ( q != NULL ) ? q->parent : 0;
			}
			if ( p == root ) {
				wids[n++] = i;
			}
		}
	}
	tk_unl_mtx(wm_mtxid);

	return n;
}

EXPORT ER wm_stat( T_WMSTAT *st )
{
	if ( !wm_ready || st == NULL ) {
		return E_NOEXS;
	}
	*st = wm_count;

	return E_OK;
}

/*
 * The screen is about to come to another size (ts_disp_setmode): the
 * lock is taken and kept until knl_wm_screen_changed (or
 * knl_wm_screen_kept when the size could not be had), so that nothing
 * builds the screen in the back buffer that is being replaced, and no
 * window is drawn while its rows are laid again.
 */
LOCAL BOOL	wm_chg_held = FALSE;

EXPORT void knl_wm_screen_changing( void )
{
	if ( !wm_ready ) {
		return;
	}
	tk_loc_mtx(wm_mtxid, TMO_FEVR);
	wm_chg_held = TRUE;
}

EXPORT void knl_wm_screen_kept( void )
{
	if ( !wm_ready || !wm_chg_held ) {
		return;
	}
	wm_chg_held = FALSE;
	tk_unl_mtx(wm_mtxid);
}

/*
 * Where a window stands on the new screen. One that was as large as the
 * old screen above the message line (全画面表示) is made as large as the
 * new one. Any other is laid from where it was asked to stand (want),
 * not from what an earlier, smaller screen left of it: one asked for
 * inside the old screen, or one a screen made smaller or moved, is kept
 * inside the new, moved up and left as far as it has to be and made
 * smaller only when it is larger than the screen -- so a screen with
 * room again gives it its size and place back; one that stood partly
 * off the old screen keeps at least a corner of its frame on the new one.
 */
LOCAL void outer_on_screen( CONST WMWIN *w, INT ow, INT oh, INT sw, INT sh, INT mh, T_DPRECT *o )
{
	INT	wide, high, least_w = 0, least_h = 0;
	BOOL	fitted;

	fitted = (BOOL)( w->want.left != w->outer.left || w->want.top != w->outer.top
			 || w->want.right != w->outer.right || w->want.bottom != w->outer.bottom );
	*o = w->outer;
	wide = o->right - o->left;
	high = o->bottom - o->top;
	if ( !fitted && o->left == 0 && o->top == 0 && wide == ow && ( high == oh - mh || high == oh ) ) {
		o->right = sw;
		o->bottom = ( high == oh ) ? sh : sh - mh;
		return;
	}
	*o = w->want;
	wide = o->right - o->left;
	high = o->bottom - o->top;
	(void)wm_least(w->attr, &least_w, &least_h);
	if ( wide > sw ) wide = ( sw > least_w ) ? sw : least_w;
	if ( high > sh - mh ) high = ( sh - mh > least_h ) ? sh - mh : least_h;
	if ( fitted || ( o->left >= 0 && o->top >= 0 && o->right <= ow && o->bottom <= oh ) ) {
		if ( o->left + wide > sw ) o->left = sw - wide;
		if ( o->top + high > sh - mh ) o->top = sh - mh - high;
		if ( o->left < 0 ) o->left = 0;
		if ( o->top < 0 ) o->top = 0;
	} else {
		if ( o->left > sw - 64 ) o->left = sw - 64;
		if ( o->top > sh - mh - 24 ) o->top = sh - mh - 24;
		if ( o->top < 0 ) o->top = 0;
	}
	o->right = o->left + wide;
	o->bottom = o->top + high;
}

/*
 * A window's pixels at the scale it was drawn at (from, rows 'fpitch'
 * bytes, standing at view rectangle fo) into its new pixels at the scale
 * now (to, rows wm_wpitch, at o): each new pixel the nearest old one,
 * by the view pixel it belongs to, as far as both reach.
 */
LOCAL void pixels_rescale( UB *to, CONST UB *from, UINT fpitch, UINT fscale,
			   CONST T_DPRECT *o, CONST T_DPRECT *fo )
{
	INT	W = dv(o->right - o->left), H = dv(o->bottom - o->top);
	INT	fw = disp_dev_of(fo->right - fo->left, fscale), fh = disp_dev_of(fo->bottom - fo->top, fscale);
	INT	X, Y;

	for ( Y = 0; Y < H; Y++ ) {
		INT		fy = disp_dev_of(disp_view_of(Y, wm_scale), fscale);
		UW		*d = (UW *)( to + (UD)Y * wm_wpitch );
		CONST UW	*s = (CONST UW *)( from + (UD)fy * fpitch );

		if ( fy >= fh ) {
			break;
		}
		for ( X = 0; X < W; X++ ) {
			INT	fx = disp_dev_of(disp_view_of(X, wm_scale), fscale);

			if ( fx >= fw ) {
				break;
			}
			d[X] = s[fx];
		}
	}
}

/*
 * The screen came to another size: what is kept of it taken again, and
 * all of it laid. A window's pixels are rows as long as the screen's
 * (win_pages), so every window is given new pixels of the new row
 * length, with what it held copied across as far as it still reaches,
 * after it is put where it stands on the new screen; its program is told
 * it moved, and the environments that draw into it follow it as wm_move
 * makes them.
 */
/*
 * What a change of the screen keeps of each window between laying them
 * and telling their programs is kept in the window itself (chg_*): not
 * on the stack of the task that changed the screen, which goes on to
 * draw every frame again and may make letters of a new size on the way.
 */
#define old(i)		( WMW(i).chg_old )
#define oldpages(i)	( WMW(i).chg_oldpages )
#define resized(i)	( WMW(i).chg_resized )
#define moved(i)	( WMW(i).chg_moved )

EXPORT void knl_wm_screen_changed( void )
{
	T_DISPSPEC	was;
	UINT		was_scale, was_wpitch;
	INT		was_vw, was_vh;
	INT		i, r, rows, bytes, mh, nwin;

	if ( !wm_ready ) {
		return;
	}
	if ( !wm_chg_held ) {
		tk_loc_mtx(wm_mtxid, TMO_FEVR);
	}
	wm_chg_held = FALSE;
	was = wm_spec;
	was_scale = wm_scale;
	was_vw = wm_vw;
	was_vh = wm_vh;
	was_wpitch = wm_wpitch;
	(void)ts_disp_ref(&wm_spec);
	view_take();
	(void)dp_set_scale(wm_sys_gid, wm_scale);
	ghosts_drop();				/* their rows were the old screen's */
	knl_pt_forget();			/* and what the pointer covered */
	knl_dp_screen_changed();
	mh = wm_msg_height();
	nwin = wm_wcap;
	for ( i = 0; i < nwin; i++ ) {
		WMWIN		*w = &WMW(i);
		T_DPRECT	o;
		UD		pages;
		UB		*pixels;

		old(i) = NULL;
		oldpages(i) = 0;
		resized(i) = moved(i) = FALSE;
		if ( !w->used ) {
			continue;
		}
		outer_on_screen(w, was_vw, was_vh, wm_vw, wm_vh, mh, &o);
		pages = win_pages(&o);
		pixels = (UB *)knl_vmap(pages, 0);
		if ( pixels == NULL ) {
			w->visible = FALSE;		/* nothing to show it from; its program is told below */
			o = w->outer;
		} else {
			knl_memset(pixels, 0, (SZ)( pages * PAGE_SIZE ));
			if ( w->pixels != NULL && was_scale != wm_scale ) {
				/* until its program draws it again, what it held at the old scale, the nearest pixel */
				pixels_rescale(pixels, w->pixels, was_wpitch, was_scale, &o, &w->outer);
			} else if ( w->pixels != NULL ) {
				rows = o.bottom - o.top;
				if ( rows > w->outer.bottom - w->outer.top ) rows = w->outer.bottom - w->outer.top;
				bytes = ( o.right - o.left ) * (INT)sizeof(UW);
				if ( bytes > ( w->outer.right - w->outer.left ) * (INT)sizeof(UW) ) {
					bytes = ( w->outer.right - w->outer.left ) * (INT)sizeof(UW);
				}
				for ( r = 0; r < dv(rows); r++ ) {
					knl_memcpy(pixels + (UD)r * wm_wpitch, w->pixels + (UD)r * was_wpitch,
						   (SZ)dv(bytes / (INT)sizeof(UW)) * sizeof(UW));
				}
			}
			old(i) = w->pixels;
			oldpages(i) = w->pix_pages;
			w->pixels = pixels;
			w->pix_pages = pages;
		}
		resized(i) = (BOOL)( o.right - o.left != w->outer.right - w->outer.left
				  || o.bottom - o.top != w->outer.bottom - w->outer.top
				  || was_scale != wm_scale );
		moved(i) = (BOOL)( resized(i) || o.left != w->outer.left || o.top != w->outer.top );
		w->mo_dur = 0;
		w->outer = o;
		work_of(&o, w->attr, &w->work);
		shape_of(w);
		w->dirty = TRUE;
		w->damage.left = 0;
		w->damage.top = 0;
		w->damage.right = w->work.right - w->work.left;
		w->damage.bottom = w->work.bottom - w->work.top;
	}
	knl_msg_screen_changed();
	tk_unl_mtx(wm_mtxid);

	/* the environments follow, then the old pixels go, as wm_move lets them go */
	for ( i = 0; i < nwin; i++ ) {
		WMWIN		*w = &WMW(i);
		T_DPRECT	v;

		if ( !w->used ) {
			continue;
		}
		v = w->work;
		(void)dp_set_scale(w->gid, wm_scale);
		if ( w->pixels != NULL ) {
			dp_set_place(w->gid, w->pixels, wm_wpitch, w->outer.left, w->outer.top, &v);
		}
		if ( old(i) != NULL ) {
			knl_vunmap(old(i), oldpages(i));
		}
		draw_frame(w);
		if ( moved(i) ) {
			knl_wmobj_moved(i + 1, resized(i));
		}
	}
	tm_printf((UB *)"wm: screen %d x %d -> %d x %d, the windows laid again\n", (INT)was.width,
		  (INT)was.height, (INT)wm_spec.width, (INT)wm_spec.height);
	if ( was_scale != wm_scale || wm_scale != 100 ) {
		tm_printf((UB *)"wm: drawn at %d%% (was %d%%), a view of %d x %d\n", (INT)wm_scale,
			  (INT)was_scale, wm_vw, wm_vh);
	}
	wm_composite();
}

#undef old
#undef oldpages
#undef resized
#undef moved

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
	view_take();
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
	(void)knl_pn_init();			/* the panels drawn one task at a time */
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
	(void)dp_set_watch(wm_sys_gid, TRUE);	/* the frames drawn are built again */
	(void)dp_set_scale(wm_sys_gid, wm_scale);
	wm_ready = TRUE;
	cp_start();				/* the compositor (without it each caller builds) */

	/* the task that carries the windows opening and closing; without it they just appear */
	{
		T_CTSK	ct;

		knl_memset(&ct, 0, sizeof(ct));
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)mo_task;
		ct.itskpri = MO_PRI;
		ct.stksz = 32 * 1024;
		wm_mo_tsk = tk_cre_tsk(&ct);
		if ( wm_mo_tsk > 0 && tk_sta_tsk(wm_mo_tsk, 0) < E_OK ) {
			(void)tk_del_tsk(wm_mo_tsk);
			wm_mo_tsk = 0;
		}
		if ( wm_mo_tsk < 0 ) {
			wm_mo_tsk = 0;
		}
	}
	(void)knl_fq_init();			/* 強制終了: Shift+Pause and its window */

	return 1;
}
