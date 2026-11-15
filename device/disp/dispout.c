/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispout.c
 *	The outputs (docs/tessronos 10.4.9): each display the drivers drive,
 *	shown on a screen or off, with what its EDID says and the modes it
 *	offers, and the state they are all set to, checked as a whole before
 *	anything is touched and committed together, put back as it was when
 *	an output does not take its part. It is the layer of a kernel mode
 *	setting's connectors and CRTCs: a driver describes each output by a
 *	T_DISPOUTHW (disp_hw.h), and the screen driver (disp.c) keeps the
 *	screens the outputs that are on show.
 *
 *	An output is known by its T_DISPOUTHW, which the driver hands over
 *	with the framebuffer of the screen it shows (T_DISPHW's out). The main
 *	screen's driver may give none: the main output is then a framebuffer
 *	the firmware set up, of one mode, its size now.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/dispout.h>
#include "disp_hw.h"
#include "edid.h"

#define DOUT_MAX	4

typedef struct {
	CONST T_DISPOUTHW *ops;
	INT		slot;			/* its number on the screen driver (knl_disp_screen_hw), -1 when off */
	T_DISPHW	hw;			/* the framebuffer it last had */
	INT		side, offset;		/* where it goes when on (not the main one) */
	UH		mode_w, mode_h;		/* the mode it was set to, where its framebuffer is of another size (a scaler's); 0 */
	UB		edid[DISP_EDID_MAX];
	INT		edid_len;
	T_EDIDINFO	info;
	T_DISPMODE	modes[DISP_MODES_MAX];
	INT		nmodes;
	UINT		status;
} DOUT;

/* The size of the mode an output is in: its framebuffer's, unless it was set to another for its scaler */
#define MODE_W(o)	( ( (o)->mode_w != 0 ) ? (UINT)(o)->mode_w : (o)->hw.width )
#define MODE_H(o)	( ( (o)->mode_h != 0 ) ? (UINT)(o)->mode_h : (o)->hw.height )

LOCAL DOUT	dout[DOUT_MAX];
LOCAL INT	dout_n = 0;
LOCAL void	(*dout_watch)( INT out ) = NULL;
LOCAL ID	dout_mtx = 0;
LOCAL ID	dout_holder = 0;		/* the task that holds dout_mtx */

/* The main output of a driver that describes none: a framebuffer the firmware set up */
LOCAL CONST T_DISPOUTHW	fw_out = {
	DISP_CON_FIRMWARE, "Firmware-1", NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* An output's modes and what its EDID says, read again */
LOCAL void dout_read( DOUT *o )
{
	CONST T_DISPOUTHW *p = o->ops;
	INT		n = 0;

	o->edid_len = 0;
	o->nmodes = 0;
	knl_memset(&o->info, 0, sizeof(o->info));
	o->status = ( p->detect != NULL ) ? (UINT)p->detect(p->ctx) : DISP_ST_UNKNOWN;
	if ( p->get_edid != NULL ) {
		n = p->get_edid(p->ctx, o->edid, DISP_EDID_MAX);
		if ( n >= 128 ) {
			INT	m = knl_edid_parse(o->edid, n, &o->info, o->modes, DISP_MODES_MAX);

			if ( m >= 0 ) {
				o->edid_len = o->info.blocks * 128;
				o->nmodes = m;
			}
		}
	}
	if ( o->nmodes == 0 && p->modes != NULL ) {
		o->nmodes = p->modes(p->ctx, o->modes, DISP_MODES_MAX);
		if ( o->nmodes < 0 ) o->nmodes = 0;
	}
	if ( o->nmodes == 0 && o->hw.width != 0 ) {
		/* none known: the one it is in */
		knl_memset(&o->modes[0], 0, sizeof(o->modes[0]));
		o->modes[0].hdisp = o->modes[0].hss = o->modes[0].hse = o->modes[0].htotal = (UH)o->hw.width;
		o->modes[0].vdisp = o->modes[0].vss = o->modes[0].vse = o->modes[0].vtotal = (UH)o->hw.height;
		o->modes[0].clock_khz = (UW)o->hw.width * o->hw.height * 60 / 1000;
		o->modes[0].vrefresh = 60;
		o->modes[0].flags = DISP_MODE_PREFERRED | DISP_MODE_DRIVER;
		o->nmodes = 1;
	}
}

/*
 * The mode an output is in kept on its list, at the end, where its EDID
 * does not offer it (a driver's size, as Linux keeps the mode a CRTC is
 * in): a state as it is then passes the check
 */
LOCAL void mode_keep( DOUT *o )
{
	T_DISPMODE	*m;
	INT		k;

	if ( MODE_W(o) == 0 ) {
		return;
	}
	for ( k = 0; k < o->nmodes; k++ ) {
		if ( o->modes[k].hdisp == MODE_W(o) && o->modes[k].vdisp == MODE_H(o) ) return;
	}
	if ( o->nmodes >= DISP_MODES_MAX ) {
		o->nmodes = DISP_MODES_MAX - 1;
	}
	m = &o->modes[o->nmodes++];
	knl_memset(m, 0, sizeof(*m));
	m->hdisp = m->hss = m->hse = m->htotal = (UH)MODE_W(o);
	m->vdisp = m->vss = m->vse = m->vtotal = (UH)MODE_H(o);
	m->clock_khz = (UW)MODE_W(o) * MODE_H(o) * 60 / 1000;
	m->vrefresh = 60;
	m->flags = ( o->nmodes == 1 ) ? DISP_MODE_PREFERRED | DISP_MODE_DRIVER : DISP_MODE_DRIVER;
}

/* The outputs the screens show now taken into the list, in the order of their screens; the caller holds dout_mtx */
LOCAL void dout_scan( void )
{
	T_DISPHW	hw;
	INT		s, k, side, off;

	for ( k = 0; k < dout_n; k++ ) {
		dout[k].slot = -1;
	}
	for ( s = 0; s < DISP_SCREENS_MAX; s++ ) {
		CONST T_DISPOUTHW *ops;

		if ( knl_disp_screen_hw(s, &hw, &side, &off) < E_OK ) break;
		ops = ( hw.out != NULL ) ? hw.out : ( s == 0 ) ? &fw_out : NULL;
		if ( ops == NULL ) continue;
		for ( k = 0; k < dout_n && dout[k].ops != ops; k++ ) ;
		if ( k == dout_n ) {
			if ( dout_n >= DOUT_MAX ) continue;
			knl_memset(&dout[k], 0, sizeof(dout[k]));
			dout[k].ops = ops;
			dout[k].hw = hw;
			dout_n++;
			dout_read(&dout[k]);
		}
		dout[k].slot = s;
		dout[k].hw = hw;
		dout[k].side = side;
		dout[k].offset = off;
		mode_keep(&dout[k]);
	}
}

LOCAL ER dout_lock( void )
{
	T_CMTX	cm;

	if ( dout_mtx == 0 ) {
		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		dout_mtx = tk_cre_mtx(&cm);
		if ( dout_mtx <= 0 ) {
			dout_mtx = 0;
			return E_LIMIT;
		}
	}
	tk_loc_mtx(dout_mtx, TMO_FEVR);
	dout_holder = tk_get_tid();
	dout_scan();
	return E_OK;
}

LOCAL void dout_unlock( void )
{
	dout_holder = 0;
	tk_unl_mtx(dout_mtx);
}

EXPORT INT ts_disp_outputs( void )
{
	INT	n;

	if ( ts_disp_screens() <= 0 || dout_lock() < E_OK ) return 0;
	n = dout_n;
	dout_unlock();
	return n;
}

EXPORT ER ts_disp_output( INT i, T_DISPOUTINFO *info )
{
	DOUT	*o;
	INT	k, x, y, w, h;

	if ( info == NULL ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	if ( i < 0 || i >= dout_n ) {
		dout_unlock();
		return E_NOEXS;
	}
	o = &dout[i];
	knl_memset(info, 0, sizeof(*info));
	info->screen = ( o->slot >= 0 ) ? knl_disp_out_screen(o->slot) : -1;
	info->kind = o->ops->kind;
	info->status = ( o->status != DISP_ST_UNKNOWN ) ? o->status : ( o->slot >= 0 ) ? DISP_ST_ON : DISP_ST_UNKNOWN;
	for ( k = 0; o->ops->name != NULL && o->ops->name[k] != 0 && k < (INT)sizeof(info->name) - 1; k++ ) {
		info->name[k] = o->ops->name[k];
	}
	knl_memcpy(info->model, o->info.model, sizeof(info->model));
	knl_memcpy(info->maker, o->info.maker, sizeof(info->maker));
	info->product = o->info.product;
	info->serial = o->info.serial;
	info->ident = o->info.ident;
	info->mm_w = o->info.mm_w;
	info->mm_h = o->info.mm_h;
	info->edid_len = (UINT)o->edid_len;
	info->hdmi = o->info.hdmi;
	info->audio = o->info.audio;
	info->nmodes = o->nmodes;
	info->usable = (BOOL)( o->slot >= 0 || ( o->ops->commit != NULL && o->status != DISP_ST_OFF ) );
	if ( o->slot >= 0 ) {
		/* the mode it is in: the first of its list of the size it shows */
		for ( k = 0; k < o->nmodes; k++ ) {
			if ( o->modes[k].hdisp == MODE_W(o) && o->modes[k].vdisp == MODE_H(o) ) {
				info->mode = o->modes[k];
				break;
			}
		}
		if ( k == o->nmodes ) {
			info->mode.hdisp = (UH)MODE_W(o);
			info->mode.vdisp = (UH)MODE_H(o);
		}
		/* where it is in the view: the main screen's place for one that mirrors it */
		if ( ts_disp_screen(( info->screen >= 0 ) ? info->screen : 0, &x, &y, &w, &h) >= E_OK ) {
			info->x = x;
			info->y = y;
		}
	}
	dout_unlock();
	return E_OK;
}

EXPORT INT ts_disp_out_modes( INT i, T_DISPMODE *modes, INT max )
{
	INT	n;

	if ( modes == NULL || max < 0 ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	if ( i < 0 || i >= dout_n ) {
		dout_unlock();
		return E_NOEXS;
	}
	n = ( dout[i].nmodes < max ) ? dout[i].nmodes : max;
	knl_memcpy(modes, dout[i].modes, (SZ)n * sizeof(T_DISPMODE));
	dout_unlock();
	return n;
}

EXPORT INT ts_disp_out_edid( INT i, UB *buf, INT max )
{
	INT	n;

	if ( buf == NULL || max < 0 ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	if ( i < 0 || i >= dout_n ) {
		dout_unlock();
		return E_NOEXS;
	}
	n = ( dout[i].edid_len < max ) ? dout[i].edid_len : max;
	knl_memcpy(buf, dout[i].edid, (SZ)n);
	dout_unlock();
	return n;
}

/* The state now; the caller holds dout_mtx */
LOCAL void state_now( T_DISPSTATE *st )
{
	INT	k;

	knl_memset(st, 0, sizeof(*st));
	st->n = dout_n;
	for ( k = 0; k < dout_n; k++ ) {
		st->out[k].on = (BOOL)( dout[k].slot >= 0 );
		st->out[k].w = (UH)MODE_W(&dout[k]);
		st->out[k].h = (UH)MODE_H(&dout[k]);
		st->out[k].side = dout[k].side;
		st->out[k].offset = dout[k].offset;
	}
}

EXPORT ER ts_disp_state( T_DISPSTATE *st )
{
	if ( st == NULL ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	state_now(st);
	dout_unlock();
	return E_OK;
}

/* The first of an output's modes of a size, or NULL */
LOCAL CONST T_DISPMODE *mode_of( CONST DOUT *o, UINT w, UINT h )
{
	INT	k;

	for ( k = 0; k < o->nmodes; k++ ) {
		if ( o->modes[k].hdisp == w && o->modes[k].vdisp == h ) return &o->modes[k];
	}
	return NULL;
}

/* Whether st makes sense and every output can take its part; nothing is touched. The caller holds dout_mtx */
LOCAL ER check_locked( CONST T_DISPSTATE *st )
{
	INT	k, on = 0;
	ER	er;

	if ( st == NULL || st->n != dout_n || st->n < 1 ) {
		return E_PAR;
	}
	if ( !st->out[0].on ) {
		return E_NOSPT;			/* the main screen stays on */
	}
	for ( k = 0; k < st->n; k++ ) {
		CONST T_DISPSET	*e = &st->out[k];
		CONST DOUT	*o = &dout[k];
		CONST T_DISPMODE *m;

		if ( !e->on ) continue;
		on++;
		if ( k > 0 ) {
			if ( e->side != DISP_SIDE_RIGHT && e->side != DISP_SIDE_BELOW
			  && e->side != DISP_SIDE_LEFT && e->side != DISP_SIDE_ABOVE && e->side != DISP_SIDE_MIRROR ) return E_PAR;
			if ( e->offset < 0 ) return E_NOSPT;
			if ( o->ops->commit == NULL && !( o->slot >= 0 && e->w == o->hw.width && e->h == o->hw.height ) ) {
				return E_NOSPT;		/* a driver that cannot set a mode keeps the one it has */
			}
		}
		m = mode_of(o, e->w, e->h);
		if ( m == NULL ) return E_NOSPT;
		if ( o->ops->check != NULL ) {
			er = o->ops->check(o->ops->ctx, m);
			if ( er < E_OK ) return er;
		}
		/* the main screen shown in m: by the output's scaler where it has one, which may refuse it */
		if ( k > 0 && e->side == DISP_SIDE_MIRROR && o->ops->fit != NULL ) {
			er = o->ops->fit(o->ops->ctx, m, st->out[0].w, st->out[0].h, FALSE);
			if ( er < E_OK ) return er;
		}
	}
	return ( on > DISP_SCREENS_MAX ) ? E_LIMIT : E_OK;
}

EXPORT ER ts_disp_check( CONST T_DISPSTATE *st )
{
	ER	er;

	if ( dout_lock() < E_OK ) return E_LIMIT;
	er = check_locked(st);
	dout_unlock();
	return er;
}

/*
 * The mode an output mirrors a main screen of mw x mh in: that size where
 * the output offers it, else its preferred mode; FALSE when it has none
 */
LOCAL BOOL mirror_size( CONST DOUT *o, UINT mw, UINT mh, UH *p_w, UH *p_h )
{
	if ( o->nmodes == 0 ) return FALSE;
	if ( mode_of(o, mw, mh) != NULL ) {
		*p_w = (UH)mw;
		*p_h = (UH)mh;
	} else {
		*p_w = o->modes[0].hdisp;
		*p_h = o->modes[0].vdisp;
	}
	return TRUE;
}

/*
 * The outputs set to want, which has been checked: the main one's size
 * first, then those besides it taken off their screens (from the last
 * screen back) and put on again in the order of the list. Where the main
 * screen's size changed (or again is TRUE), each output that mirrors it
 * has its mode chosen anew for the new size (mirror_size) and is put on
 * again with it, so that what its scaler or the send scales from and to
 * is made again. The first that does not take its part ends it with its
 * error. The caller holds dout_mtx.
 */
LOCAL ER apply_locked( CONST T_DISPSTATE *want, BOOL again )
{
	T_DISPSTATE	sts = *want, *st = &sts;
	INT		k, s;
	ER		er;

	if ( st->out[0].w != dout[0].hw.width || st->out[0].h != dout[0].hw.height ) {
		er = ts_disp_setmode(st->out[0].w, st->out[0].h);
		if ( er < E_OK ) return er;
		dout_scan();
		again = TRUE;
	}
	if ( again ) {
		BOOL	any = FALSE;

		for ( k = 1; k < st->n; k++ ) {
			if ( st->out[k].on && st->out[k].side == DISP_SIDE_MIRROR ) {
				(void)mirror_size(&dout[k], st->out[0].w, st->out[0].h, &st->out[k].w, &st->out[k].h);
				any = TRUE;
			}
		}
		again = any;
	}
	/* the outputs besides the main one as they are: nothing more to do */
	for ( k = 1; k < st->n && !again; k++ ) {
		CONST T_DISPSET	*e = &st->out[k];
		CONST DOUT	*o = &dout[k];

		if ( e->on != ( o->slot >= 0 ) ) break;
		if ( e->on && ( e->w != MODE_W(o) || e->h != MODE_H(o) || e->side != o->side || e->offset != o->offset ) ) break;
	}
	if ( k == st->n && !again ) {
		return E_OK;
	}
	for ( s = knl_disp_out_count(); s >= 1; s-- ) {
		er = knl_disp_out_detach(s);
		if ( er < E_OK ) return er;
	}
	dout_scan();
	for ( k = 1; k < st->n; k++ ) {
		CONST T_DISPSET	*e = &st->out[k];
		DOUT		*o = &dout[k];
		T_DISPHW	hw = o->hw;

		if ( !e->on ) {
			if ( o->ops->commit != NULL ) (void)o->ops->commit(o->ops->ctx, NULL, &hw);
			o->mode_w = o->mode_h = 0;
			continue;
		}
		if ( o->ops->fit != NULL ) {
			/* the size of framebuffer it is to show: the main screen's when it mirrors it, else its mode's */
			BOOL	mirror = (BOOL)( e->side == DISP_SIDE_MIRROR );

			er = o->ops->fit(o->ops->ctx, mode_of(o, e->w, e->h), mirror ? st->out[0].w : e->w,
					 mirror ? st->out[0].h : e->h, TRUE);
			if ( er < E_OK ) return er;
		}
		if ( o->ops->commit != NULL ) {
			er = o->ops->commit(o->ops->ctx, mode_of(o, e->w, e->h), &hw);
			if ( er < E_OK ) return er;
		}
		hw.out = o->ops;
		er = knl_disp_out_attach(&hw, e->side, e->offset);
		if ( er < E_OK ) return er;
		o->hw = hw;
		o->mode_w = ( hw.width != e->w || hw.height != e->h ) ? e->w : 0;
		o->mode_h = ( hw.width != e->w || hw.height != e->h ) ? e->h : 0;
	}
	dout_scan();
	return E_OK;
}

/*
 * The mode output i mirrors the main screen in, as the desktops of Linux
 * choose it: the main screen's own size where the output offers it (no
 * scaling, the main screen's mode kept), else the output's preferred
 * mode, the main screen scaled into it; and whether that can be set now
 */
EXPORT ER ts_disp_mirror_mode( INT i, UINT *p_w, UINT *p_h )
{
	T_DISPSTATE	st;
	CONST DOUT	*o;
	UINT		mw, mh;
	ER		er;

	if ( p_w == NULL || p_h == NULL ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	if ( i < 1 || i >= dout_n || dout[i].nmodes == 0 ) {
		dout_unlock();
		return E_NOEXS;
	}
	o = &dout[i];
	mw = MODE_W(&dout[0]);
	mh = MODE_H(&dout[0]);
	state_now(&st);
	(void)mirror_size(o, mw, mh, &st.out[i].w, &st.out[i].h);
	*p_w = st.out[i].w;
	*p_h = st.out[i].h;
	st.out[i].on = TRUE;
	st.out[i].side = DISP_SIDE_MIRROR;
	st.out[i].offset = 0;
	er = check_locked(&st);
	dout_unlock();
	return er;
}

EXPORT ER ts_disp_commit( CONST T_DISPSTATE *st )
{
	T_DISPSTATE	was;
	ER		er;

	if ( dout_lock() < E_OK ) return E_LIMIT;
	er = check_locked(st);
	if ( er >= E_OK ) {
		state_now(&was);
		er = apply_locked(st, FALSE);
		if ( er < E_OK ) {
			tm_printf((UB *)"disp: outputs: the new state did not take (%d): put back as it was\n", (INT)er);
			(void)apply_locked(&was, FALSE);
		}
	}
	dout_unlock();
	return er;
}

/*
 * The main screen came to another size by the screen driver (a size
 * written to the screen object, the desktop's at its start): each output
 * that mirrors it chosen its mode for the new size and put on again, as a
 * commit does when it changes the main screen's size (apply_locked).
 * Called by the screen driver after the change; nothing while this layer
 * is itself setting the outputs, which does the same there.
 */
EXPORT void knl_disp_out_main_changed( void )
{
	T_DISPSTATE	st;
	ER		er;

	if ( dout_mtx == 0 || dout_holder == tk_get_tid() || dout_lock() < E_OK ) return;
	state_now(&st);
	er = apply_locked(&st, TRUE);
	if ( er < E_OK ) {
		tm_printf((UB *)"disp: outputs: mirroring not made again for the main screen's new size (%d)\n", (INT)er);
	}
	dout_unlock();
}

EXPORT ER knl_disp_out_add( CONST T_DISPOUTHW *ops )
{
	INT	k;

	if ( ops == NULL ) return E_PAR;
	if ( dout_lock() < E_OK ) return E_LIMIT;
	for ( k = 0; k < dout_n && dout[k].ops != ops; k++ ) ;
	if ( k == dout_n ) {
		if ( dout_n >= DOUT_MAX ) {
			dout_unlock();
			return E_LIMIT;
		}
		knl_memset(&dout[k], 0, sizeof(dout[k]));
		dout[k].ops = ops;
		dout[k].slot = -1;
		dout_read(&dout[k]);
		dout_n++;
	}
	dout_unlock();
	return E_OK;
}

EXPORT void knl_disp_out_watch( void (*fn)( INT out ) )
{
	dout_watch = fn;
}

/*
 * A driver's word that something was plugged into an output or taken out
 * of it (its hot plug detection): its EDID and modes are read again and
 * the watcher told. Called from a task.
 */
EXPORT void knl_disp_out_hotplug( CONST T_DISPOUTHW *ops )
{
	INT	k, which = -1;

	if ( dout_lock() < E_OK ) return;
	for ( k = 0; k < dout_n; k++ ) {
		if ( dout[k].ops == ops ) {
			dout_read(&dout[k]);
			which = k;
		}
	}
	dout_unlock();
	if ( which >= 0 && dout_watch != NULL ) {
		dout_watch(which);
	}
}
