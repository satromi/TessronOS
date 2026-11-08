/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfcanvas.c
 *	The figure editor's canvas in a window of the desktop's (design 17.13)
 *
 *	What the canvas paints and 焼き付け are the figure editor's code's
 *	(application/fig/figcanvas.c); here a window of the desktop's is
 *	given to it as a view of its figure, the pictures are read from and
 *	written to the store beside the records, and the shapes of 焼き付け
 *	are drawn into a picture's pixels in a drawing environment of their
 *	own.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/figedit.h>
#include <ts/figtool.h>
#include "desktop.h"

/* ---------------------------------------------------------------- what the code asks of the window */

EXPORT ER df_pic_get( T_FTVIEW *v, CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h )
{
	(void)v;
	return om_store_picture(href, p_px, p_w, p_h);
}

EXPORT void df_pic_done( T_FTVIEW *v, CONST UW *px )
{
	(void)v;
	om_store_picture_done(px);
}

EXPORT ER df_pic_put( T_FTVIEW *v, CONST UB *href, CONST UW *px, INT w, INT h )
{
	ER	er = om_store_put_picture(href, px, w, h);

	((DTWIN *)v->arg)->dirty = TRUE;
	return er;
}

EXPORT BOOL df_self_id( T_FTVIEW *v, TS_UUID *id )
{
	*id = ((DTWIN *)v->arg)->id;
	return TRUE;
}

EXPORT void df_tell_view( T_FTVIEW *v, CONST char *msg )
{
	dt_tell((DTWIN *)v->arg, msg, NULL);
}

/* ---------------------------------------------------------------- the canvas in a window */

/* Painting ended: what was painted written, and the picture let go */
EXPORT void df_canvas_leave( void )
{
	DTWIN		*d = (DTWIN *)ft_canvas_arg();
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL ) {
		return;
	}
	if ( !d->used ) {
		ft_canvas_forget(d);
		return;
	}
	df_view_of(d, &v, &e);
	ft_canvas_leave(&v);
	df_ed_back(&e);
}

/*
 * A press with the canvas in hand: in the picture being painted, paint;
 * outside it, painting ends; on another picture, painting in it begins.
 * FALSE when nothing was pressed that is the canvas's.
 */
EXPORT BOOL df_canvas_press( DTWIN *d, INT fx, INT fy )
{
	T_FTVIEW	v;
	T_FGED		e;
	BOOL		r;

	if ( ft_canvas_arg() != NULL && ft_canvas_arg() != (void *)d ) {
		df_canvas_leave();
	}
	df_view_of(d, &v, &e);
	r = ft_canvas_press(&v, fx, fy);
	df_ed_back(&e);
	return r;
}

EXPORT BOOL df_canvas_painting( void )
{
	return ft_canvas_painting();
}

EXPORT void df_canvas_follow( INT sx, INT sy )
{
	DTWIN		*d = (DTWIN *)ft_canvas_arg();
	T_WMWIN		w;
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	df_view_of(d, &v, &e);
	ft_canvas_follow(&v, sx - w.work.left, sy - w.work.top);
	df_ed_back(&e);
}

EXPORT void df_canvas_release( void )
{
	DTWIN		*d = (DTWIN *)ft_canvas_arg();
	T_FTVIEW	v;
	T_FGED		e;

	if ( d == NULL || !d->used ) {
		return;
	}
	df_view_of(d, &v, &e);
	ft_canvas_release(&v);
	df_ed_back(&e);
}

EXPORT BOOL df_canvas_active( void )
{
	return (BOOL)( ft_canvas_arg() != NULL );
}

/* The picture being painted, over the figure as it now is */
EXPORT void df_canvas_draw( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( ft_canvas_arg() != (void *)d ) {
		return;
	}
	df_view_of(d, &v, &e);
	v.gid = gid;
	v.page = *page;
	ft_canvas_draw(&v, ( d->paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : d->paper);
}

/* A window gone: a picture being painted in it is let go, not written */
EXPORT void df_canvas_forget( DTWIN *d )
{
	ft_canvas_forget(d);
}

/* 焼き付け on the one picture picked */
EXPORT void df_burn( DTWIN *d )
{
	T_FTVIEW	v;
	T_FGED		e;

	if ( d->fig == NULL || d->sealed ) {
		return;
	}
	if ( ft_canvas_arg() != NULL && ft_canvas_arg() != (void *)d ) {
		df_canvas_leave();
	}
	df_view_of(d, &v, &e);
	ft_burn(&v);
	df_ed_back(&e);
}
