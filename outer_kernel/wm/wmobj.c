/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wmobj.c
 *	The window manager of the object layer: windows, panels, menus and
 *	the parts of panels as real objects (design 18.13)
 *
 *	The windows and panels themselves stay where they are, in wm.c and
 *	part.c, known there by their numbers. This file gives each a UUID
 *	when it opens, answers the basic operations on it through the name
 *	manager, and turns what happens to it into notices.
 *
 *	The records of a window:
 *	  0  drawing: xmlTAD; writing it has the window show that
 *	  1  shape: xmlTAD figure; what it draws is the window's outline
 *	  2  place: T_OBWPOS; writing it moves the window
 *	  3  state: a part's value, or the text of a box
 *	  4- a panel's links to its parts, or a menu's items (the item's
 *	     state in the record's subtype)
 *
 *	A handle is not a table entry: it says which kind of thing and
 *	which one, so an object that has gone answers E_NOEXS the next
 *	time it is used rather than leaving a slot to be cleaned up.
 *
 *	A panel, a menu or a part can also be made as an object on its own
 *	and then placed with ob_map_rec: a part on a panel, a panel or a
 *	menu on a window. Such an object is kept here whether it is placed
 *	or not (wo_free); while it is placed it has the same UUID in the
 *	window layer's tables, which hold its value and draw it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include <ts/hid.h>
#include <ts/uuid.h>
#include "obj/obj.h"
#include <ts/img.h>
#include <ts/om.h>
#include <ts/keymap.h>
#include "wmobj.h"
#include "pdraw.h"

#define H_WIN		1
#define H_PNL		2
#define H_PART		3
#define H_FREE		4		/* made on its own: wo_free */
/*
 * A handle: the kind in bits 28 to 30, then a window's number in the 28
 * bits below (WM_WIN_LAST), a panel's or a made one's place the same,
 * or for a part its panel's number in bits 12 to 27 and its place on
 * the panel in the 12 below.
 */
#define H_MAKE(k, a, b)	( ( (k) == H_PART ) ? ( ( (k) << 28 ) | ( (a) << 12 ) | (b) ) \
					    : ( ( (k) << 28 ) | (a) ) )
#define H_KIND(h)	( (h) >> 28 )
#define H_A(h)		( ( H_KIND(h) == H_PART ) ? ( ( (h) >> 12 ) & 0xFFFF ) : ( (h) & 0x0FFFFFFF ) )
#define H_B(h)		( ( H_KIND(h) == H_PART ) ? ( (h) & 0xFFF ) : 0 )

#define WO_REC_MAX	( 1024 * 1024 )	/* bytes of a drawing or a shape */
#define WO_NREC		OB_WR_KIND	/* the records every window has */


/* The protection of a window: its maker's, and nobody else's */
typedef struct {
	TS_UUID	owner;
	TS_UUID	group;
	UINT	mode;
} WOPRT;

typedef struct {
	BOOL	used;
	TS_UUID	uuid;
	WOPRT	prt;
	UB	*rec[2];		/* the drawing and the shape */
	SZ	len[2];
	BOOL	drive;			/* its panels are worked from here */
	TS_UUID	shows;			/* the object it shows, 0 none said */
	T_OBDROP *drop;			/* the latest drop on it, NULL none */
	SZ	droplen;		/* its bytes: OB_DROP_SIZE(n), or the letters and their nought */
	ID	taker;			/* the process granted that drop's objects */
	BOOL	answered;		/* that drop was answered */
	BOOL	gave;			/* that drop granted its taker rights: a refusal takes them back */
	INT	dropback;		/* the window that had the keys before that drop, 0 none */
	UINT	askseq;			/* the last question asked it (wm_obj_ask), 0 none */
	UINT	askans;			/* and its answer: OB_DR_*, 0 none yet */
	ID	maker;			/* the process that made it, 0 the system */
	ID	forpid;			/* one of the system's opened for a process's
					   request: that process (knl_wmobj_act_for) */
	UINT	seq;			/* drops so far */
	INT	back;			/* a panel of a process's own: the window that had
					   the keys before it, given them back when it goes */
	BOOL	nofocus;		/* it never has the keys (WM_ATTR_NOFOCUS): a screen
					   keyboard, told every key and typing them */
	INT	cand;			/* the window of the candidates listed for it (OB_WR_CAND), 0 none */
	INT	ncand;			/* the names listed there */
	UINT	hold_code;		/* the key it holds down (WM_KEY_PUT_DOWN), 0 none */
	UINT	hold_mods;		/* the modifiers held with it */
} WOWIN;

typedef struct {
	BOOL	used;
	TS_UUID	uuid;
	WOPRT	prt;
	INT	npart;
	TS_UUID	part[WM_PART_MAX];
} WOPNL;

#define WO_FREE_MAX	64

typedef struct {
	UB	label[WM_LABEL_MAX];
	UINT	state;			/* OB_MI_* */
} WOITEM;

typedef struct {
	BOOL	used;
	TS_UUID	uuid;
	WOPRT	prt;
	UINT	sub;			/* OB_S_PANEL, OB_S_MENU or OB_S_PART */
	T_WMPART pt;			/* a part: what it is; all: pt.r its size, pt.label its name */
	UB	*look[2];		/* a part: how it looks released, and pressed */
	SZ	looklen[2];
	INT	nlook;
	UB	*shape;
	SZ	shapelen;
	UB	*draw;			/* what its definition says of it, kept */
	SZ	drawlen;
	WOITEM	*item;			/* a menu's items, room for icap of them */
	INT	nitem, icap;
	TS_UUID	box;			/* a panel: the data box it is made from */
	INT	boxnum;			/* and its number there, 0 none */
	BOOL	beside;			/* a panel beside what its program draws ("beside":1, ui_strip) */
	INT	pid;			/* the panel or menu it is placed as or on, 0 none */
	INT	idx;			/* a part: which of that panel's parts it is */
} WOFREE;

LOCAL WOFREE		wo_free[WO_FREE_MAX];
LOCAL INT		wo_nfree = 0;
/*
 * The windows, by number (1 up): as many as the window layer has room
 * for (knl_wm_nwin), the table made larger when a window of a higher
 * number opens. An entry is allocated once and never freed, and the
 * table before a larger one is kept, so that it may be read while it
 * grows.
 */
LOCAL WOWIN		wo_none;			/* number 0: never used */
LOCAL WOWIN		*wo_tbl0[1] = { &wo_none };
LOCAL WOWIN		**wo_wtbl = wo_tbl0;
LOCAL INT		wo_nwin = 0;

#define WOW(i)		( *wo_wtbl[(i)] )
LOCAL WOPNL		wo_pnl[WM_PANEL_MAX + 1];	/* by panel number */
LOCAL ID		wo_mtx = 0;
LOCAL ER key_put_one( UINT type, UINT code, UINT mods );
LOCAL FP_WMPAINT	wo_paint = NULL;
LOCAL FP_WMSHAPE	wo_shape = NULL;
LOCAL INT		wo_hover = 0;	/* the window the pointer was last over */
LOCAL INT		wo_down = 0;	/* the window the last press went down on */
LOCAL UD		wo_down_ms = 0;	/* and when, ms since start */
LOCAL volatile ID	wo_for_tsk = 0;	/* a task of the system's doing what a process asked */
LOCAL volatile ID	wo_for_pid = 0;	/* and that process */

#define LOCK()		tk_loc_mtx(wo_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(wo_mtx)

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* Whoever is opening it: the process's user, its first group */
LOCAL void prt_of_maker( WOPRT *p )
{
	CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());

	knl_memset(p, 0, sizeof(*p));
	p->owner = crd->user;
	if ( crd->ngrp > 0 ) {
		p->group = crd->grp[0];
	}
	p->mode = 0600;
}

EXPORT INT wm_obj_wid( CONST TS_UUID *uuid )
{
	INT	i, wid = E_NOEXS;

	if ( wo_mtx <= 0 || uuid == NULL ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 1; i <= wo_nwin; i++ ) {
		if ( WOW(i).used && same_uuid(&WOW(i).uuid, uuid) ) {
			wid = i;
			break;
		}
	}
	UNLOCK();

	return wid;
}

EXPORT ER wm_obj_uuid( INT wid, TS_UUID *p_uuid )
{
	ER	er = E_ID;

	if ( p_uuid == NULL ) {
		return E_PAR;
	}
	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_ID;
	}
	LOCK();
	if ( WOW(wid).used ) {
		*p_uuid = WOW(wid).uuid;
		er = E_OK;
	}
	UNLOCK();

	return er;
}

/*
 * The window's pictogram made the icon of the object it stands for: the
 * object it shows, or the program it belongs to. An object with no icon
 * leaves the pictogram as it was.
 */
LOCAL void icon_of( INT wid, CONST TS_UUID *obj )
{
	UB	*file;
	UW	*px = NULL;
	SZ	size = 0;
	INT	w = 0, h = 0;

	file = om_obj_icon(obj, &size);
	if ( file == NULL ) {
		return;
	}
	if ( img_decode(file, size, &px, &w, &h) >= E_OK && px != NULL ) {
		(void)wm_set_icon(wid, px, w, h);
		Kfree(px);
	}
	Kfree(file);
}

EXPORT BOOL wm_obj_shown( INT wid, TS_UUID *p_obj )
{
	static CONST TS_UUID	none;
	BOOL			got = FALSE;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin || p_obj == NULL ) {
		return FALSE;
	}
	LOCK();
	if ( WOW(wid).used && ts_uuid_cmp(&WOW(wid).shows, &none) != 0 ) {
		*p_obj = WOW(wid).shows;
		got = TRUE;
	}
	UNLOCK();
	return got;
}

EXPORT INT wm_obj_showing( CONST TS_UUID *obj )
{
	INT	*wids, n, i, found = 0;
	TS_UUID	u;

	if ( obj == NULL ) {
		return 0;
	}
	n = knl_wm_nwin();
	wids = (INT *)Kmalloc(sizeof(INT) * (SZ)( n + 1 ));
	if ( wids == NULL ) {
		return 0;
	}
	n = wm_list(wids, n + 1);
	for ( i = 0; i < n && found == 0; i++ ) {
		if ( wm_obj_shown(wids[i], &u) && ts_uuid_cmp(&u, obj) == 0 ) {
			found = wids[i];
		}
	}
	Kfree(wids);
	return found;
}

EXPORT ER wm_obj_shows( INT wid, CONST TS_UUID *obj )
{
	ER	er = E_ID;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_ID;
	}
	LOCK();
	if ( WOW(wid).used ) {
		if ( obj != NULL ) {
			WOW(wid).shows = *obj;
		} else {
			knl_memset(&WOW(wid).shows, 0, sizeof(TS_UUID));
		}
		er = E_OK;
	}
	UNLOCK();
	if ( er >= E_OK && obj != NULL ) {
		icon_of(wid, obj);
	}

	return er;
}

EXPORT void wm_obj_painter( FP_WMPAINT paint, FP_WMSHAPE shape )
{
	wo_paint = paint;
	wo_shape = shape;
}

/* ---------------------------------------------------------------- what the windows say */

EXPORT void knl_wmobj_open( INT wid )
{
	TS_UUID	u;
	WOPRT	p;
	T_WMWIN	w;
	BOOL	nofocus;

	if ( wo_mtx <= 0 || wid < 1 || wid > WM_WIN_LAST || ts_gen_uuid(&u) < E_OK ) {
		return;
	}
	prt_of_maker(&p);
	nofocus = (BOOL)( wm_ref(wid, &w) >= E_OK && ( w.attr & WM_ATTR_NOFOCUS ) != 0 );
	LOCK();
	if ( wid > wo_nwin ) {
		/* room up to the window layer's, made whole before it is put in place */
		INT	cap = knl_wm_nwin(), i;
		WOWIN	**t, *e;

		if ( cap < wid ) cap = wid;
		t = (WOWIN **)Kcalloc((SZ)cap + 1, sizeof(WOWIN *));
		e = (WOWIN *)Kcalloc((SZ)( cap - wo_nwin ), sizeof(WOWIN));
		if ( t == NULL || e == NULL ) {
			UNLOCK();
			if ( t != NULL ) Kfree(t);
			if ( e != NULL ) Kfree(e);
			return;
		}
		t[0] = &wo_none;
		for ( i = 1; i <= cap; i++ ) {
			t[i] = ( i <= wo_nwin ) ? wo_wtbl[i] : &e[i - wo_nwin - 1];
		}
		__sync_synchronize();
		wo_wtbl = t;			/* the table before is not freed */
		__sync_synchronize();
		wo_nwin = cap;
	}
	knl_memset(&WOW(wid), 0, sizeof(WOWIN));
	WOW(wid).uuid = u;
	WOW(wid).prt = p;
	WOW(wid).used = TRUE;
	WOW(wid).nofocus = nofocus;
	WOW(wid).hold_code = 0;
	if ( wo_for_pid > 0 && wo_for_tsk == tk_get_tid() ) {
		WOW(wid).forpid = wo_for_pid;
	}
	UNLOCK();
}

EXPORT void knl_wmobj_close( INT wid )
{
	TS_UUID	u;
	UB	*r0 = NULL, *r1 = NULL;
	T_OBDROP *dr = NULL;
	BOOL	was = FALSE;
	INT	cpop;
	UINT	hold_code = 0, hold_mods = 0;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return;
	}
	LOCK();
	if ( WOW(wid).used ) {
		u = WOW(wid).uuid;
		r0 = WOW(wid).rec[0];
		r1 = WOW(wid).rec[1];
		dr = WOW(wid).drop;
		WOW(wid).drop = NULL;
		WOW(wid).used = FALSE;
		was = TRUE;
		hold_code = WOW(wid).hold_code;
		hold_mods = WOW(wid).hold_mods;
		WOW(wid).hold_code = 0;
	}
	cpop = WOW(wid).cand;
	WOW(wid).cand = 0;
	WOW(wid).ncand = 0;
	if ( wo_hover == wid ) {
		wo_hover = 0;			/* the number may come back as another window */
	}
	if ( wo_down == wid ) {
		wo_down = 0;
	}
	UNLOCK();
	if ( hold_code != 0 ) {
		(void)key_put_one(HID_EV_KEY_UP, hold_code, hold_mods);	/* no key stays down for a window gone */
	}
	if ( r0 != NULL ) Kfree(r0);
	if ( r1 != NULL ) Kfree(r1);
	if ( dr != NULL ) Kfree(dr);
	if ( cpop > 0 ) {
		(void)wm_close(cpop);
		wm_update();
	}
	if ( was ) {
		knl_ob_post(&u, -1, OB_E_DELETE, NULL);
	}
}

/*
 * A process ended: the windows it made are closed. A program closes its
 * windows itself as it ends; one taken down by a fault or by ts_ter_prc
 * cannot, and its windows (a sub window among them) would stay on the
 * screen with nobody to draw them or to close them. The surfaces it drew
 * any window in go first.
 */
EXPORT void knl_wmobj_prc_end( ID pid )
{
	INT	*wids, n = 0, i;

	if ( wo_mtx <= 0 || pid <= 0 ) {
		return;
	}
	knl_wm_surf_prc_end(pid);
	LOCK();
	wids = (INT *)Kmalloc(sizeof(INT) * (SZ)( wo_nwin + 1 ));
	for ( i = 1; i <= wo_nwin && wids != NULL; i++ ) {
		if ( WOW(i).used && WOW(i).maker == pid ) {
			wids[n++] = i;
		}
	}
	UNLOCK();
	for ( i = 0; i < n; i++ ) {
		(void)wm_close(wids[i]);
	}
	if ( wids != NULL ) Kfree(wids);
	if ( n > 0 ) {
		wm_update();
	}
}

/*
 * The calling task (the desktop's) does what process pid asked, until
 * it is called again with 0: the windows it opens meanwhile -- a panel,
 * the page of 印刷 -- are that process's to 強制終了 (design 5.5.1).
 */
EXPORT void knl_wmobj_act_for( ID pid )
{
	if ( pid > 0 ) {
		wo_for_pid = 0;
		wo_for_tsk = tk_get_tid();
		wo_for_pid = pid;
	} else {
		wo_for_pid = 0;
		wo_for_tsk = 0;
	}
}

/*
 * Who a window is for, for 強制終了: the process that made it, and the
 * process a window of the system's was opened for (0 none). E_TMOUT
 * when the lock is not had within tmo, E_NOEXS for no such window.
 */
EXPORT ER knl_wmobj_who( INT wid, TMO tmo, ID *p_maker, ID *p_for )
{
	ER	er = E_NOEXS;

	*p_maker = 0;
	*p_for = 0;
	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_NOEXS;
	}
	if ( tk_loc_mtx(wo_mtx, tmo) < E_OK ) {
		return E_TMOUT;
	}
	if ( WOW(wid).used ) {
		*p_maker = WOW(wid).maker;
		*p_for = WOW(wid).forpid;
		er = E_OK;
	}
	UNLOCK();

	return er;
}

/* How many windows process pid has made */
EXPORT INT knl_wmobj_count( ID pid, TMO tmo )
{
	INT	i, n = 0;

	if ( wo_mtx <= 0 || pid <= 0 ) {
		return 0;
	}
	if ( tk_loc_mtx(wo_mtx, tmo) < E_OK ) {
		return E_TMOUT;
	}
	for ( i = 1; i <= wo_nwin; i++ ) {
		if ( WOW(i).used && WOW(i).maker == pid ) {
			n++;
		}
	}
	UNLOCK();

	return n;
}

/* The process that made a window, 0 for one of the system's own or none */
EXPORT ID knl_wmobj_maker( INT wid )
{
	ID	pid = 0;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return 0;
	}
	LOCK();
	if ( WOW(wid).used ) {
		pid = WOW(wid).maker;
	}
	UNLOCK();

	return pid;
}

EXPORT void knl_wmobj_panel( INT pid )
{
	WOPNL	*p;
	INT	n, i;

	if ( wo_mtx <= 0 || pid < 1 || pid > WM_PANEL_MAX ) {
		return;
	}
	n = knl_pn_count(pid, NULL);
	if ( n < 0 ) {
		return;
	}
	p = (WOPNL *)Kmalloc(sizeof(WOPNL));
	if ( p == NULL ) {
		return;
	}
	knl_memset(p, 0, sizeof(*p));
	prt_of_maker(&p->prt);
	p->npart = ( n > WM_PART_MAX ) ? WM_PART_MAX : n;
	if ( ts_gen_uuid(&p->uuid) < E_OK ) {
		Kfree(p);
		return;
	}
	for ( i = 0; i < p->npart; i++ ) {
		(void)ts_gen_uuid(&p->part[i]);
	}
	p->used = TRUE;
	LOCK();
	wo_pnl[pid] = *p;
	UNLOCK();
	Kfree(p);
}

LOCAL void fr_keep_state( INT fi );

/*
 * A panel or menu closing. What was placed on it or as it goes back to
 * standing on its own -- a part keeping the value it had -- and only a
 * panel that was never an object of its own is gone.
 */
EXPORT void knl_wmobj_panel_close( INT pid )
{
	TS_UUID	u;
	BOOL	was = FALSE, own = FALSE;
	INT	i;

	if ( wo_mtx <= 0 || pid < 1 || pid > WM_PANEL_MAX ) {
		return;
	}
	for ( i = 0; i < WO_FREE_MAX && wo_nfree > 0; i++ ) {
		if ( wo_free[i].used && wo_free[i].pid == pid && wo_free[i].sub == OB_S_PART ) {
			fr_keep_state(i);
		}
	}
	LOCK();
	if ( wo_pnl[pid].used ) {
		u = wo_pnl[pid].uuid;
		wo_pnl[pid].used = FALSE;
		was = TRUE;
	}
	for ( i = 0; i < WO_FREE_MAX; i++ ) {
		if ( wo_free[i].used && wo_free[i].pid == pid ) {
			if ( wo_free[i].sub != OB_S_PART ) {
				own = TRUE;
			}
			wo_free[i].pid = 0;
		}
	}
	UNLOCK();
	if ( was && !own ) {
		knl_ob_post(&u, -1, OB_E_DELETE, NULL);
	}
}

/* The details of an input as a notice carries them */
LOCAL void notice_of( CONST T_WMEV *ev, T_OBNTM *n )
{
	knl_memset(n, 0, sizeof(*n));
	if ( ev != NULL ) {
		n->code = ev->code;
		n->mods = ev->mods;
		n->x = ev->x;
		n->y = ev->y;
		n->when = ev->when;
		n->dz = ev->dz;
	}
	n->imode = wm_msg_get_mode();	/* as before this key: a mode key works after the notice (wm_read_event) */
}

/* A notice to a window by its number, if it is one */
LOCAL void win_post( INT wid, UINT event, CONST T_WMEV *ev )
{
	T_OBNTM	n;
	TS_UUID	u;
	BOOL	known = FALSE;

	if ( wid < 1 || wid > wo_nwin ) {
		return;
	}
	LOCK();
	if ( WOW(wid).used ) {
		u = WOW(wid).uuid;
		known = TRUE;
	}
	UNLOCK();
	if ( known ) {
		notice_of(ev, &n);
		knl_ob_post(&u, -1, event, &n);
	}
}

/*
 * The panels and menus on a window that was made as an object: nothing
 * else reads that window's input, so what happens there is handed to
 * them from here, and they tell their parts' notices themselves. A menu
 * placed this way closes once something is chosen in it, or when a
 * button goes down anywhere else.
 */
LOCAL void drive_panels( CONST T_WMEV *ev )
{
	INT	pids[WM_PANEL_MAX], n = 0, i, cmd;
	UINT	ans;
	BOOL	menu, drive = FALSE;

	if ( ev->wid < 1 || ev->wid > wo_nwin ) {
		return;
	}
	LOCK();
	for ( i = 1; i <= WM_PANEL_MAX; i++ ) {
		if ( wo_pnl[i].used ) {
			pids[n++] = i;
		}
	}
	drive = WOW(ev->wid).used && WOW(ev->wid).drive;
	UNLOCK();
	for ( i = 0; i < n; i++ ) {
		INT	pwid = wm_panel_wid(pids[i]);
		BOOL	driven;

		if ( knl_pn_count(pids[i], &menu) < 0 ) {
			continue;
		}
		LOCK();
		driven = ( pwid >= 1 && pwid <= wo_nwin && WOW(pwid).used
			&& WOW(pwid).drive );
		UNLOCK();
		if ( !driven ) {
			continue;
		}
		if ( menu ) {
			if ( pwid == ev->wid ) {
				cmd = 0;
				(void)wm_menu_event(pids[i], ev, &cmd);
				if ( cmd != 0 && ev->type == HID_EV_BTN_UP ) {
					wm_panel_close(pids[i]);
					wm_update();
				}
			} else if ( ev->type == HID_EV_BTN_DOWN ) {
				wm_panel_close(pids[i]);	/* pressed elsewhere: let go of */
				wm_update();
			}
		} else if ( drive && pwid == ev->wid
			 && !( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP )
			       && ev->code != 0 ) ) {
			/* the parts are worked by the main button; the other opens the menu */
			ans = WM_ANS_NONE;
			(void)wm_panel_event(pids[i], ev, &ans);
			wm_update();
		}
	}
}

/*
 * A menu a program running as a process has opened on its window
 * (mn_pop_men). Nothing in the process reads the input, so the menu is
 * worked from here as each input arrives, the way the desktop works its
 * own: it stays up after the press that opened it is let go as a click
 * (wm_click_take), a row is chosen with the first button only, a press
 * elsewhere or Escape puts it away. The process waits for the
 * answer.
 */
#define WO_MSESS	4
#define KEY_ESCAPE	0x29

typedef struct {
	BOOL	used;
	BOOL	done;
	INT	pid;			/* the menu's panel */
	UD	when;			/* when the press that opened it was */
	INT	cmd;			/* what was chosen, 0 nothing */
	ID	tskid;			/* who waits */
	T_WMCLICK ck;			/* the opening press, to its release */
	UD	skip;			/* the opening release, taken before the menu was: not the menu's, 0 none */
} WOMSESS;

LOCAL WOMSESS	wo_ms[WO_MSESS];
LOCAL ID	wo_ms_flg = 0;

/*
 * When each button was last let go, as the input arrived here. The
 * process opens its menu after it has heard of the press, so a quick
 * click's release may have come and gone before the menu is there to
 * see it; the menu then knows the click is over and stays. A quick
 * click may come in one report of the device, its press and release
 * with the same time: a release at the press's time is that click's.
 * A release of an earlier click is before the press. 0: never let go.
 */
LOCAL volatile UD	wo_up_when[32];

LOCAL void menu_done( WOMSESS *s, INT cmd )
{
	s->cmd = cmd;
	s->done = TRUE;
	tk_set_flg(wo_ms_flg, 1);
}

/* Whether a menu of a program is open and not yet answered */
LOCAL BOOL menu_open( void )
{
	BOOL	open = FALSE;
	INT	i;

	LOCK();
	for ( i = 0; i < WO_MSESS && !open; i++ ) {
		open = ( wo_ms[i].used && !wo_ms[i].done );
	}
	UNLOCK();

	return open;
}

LOCAL void drive_menus( CONST T_WMEV *ev )
{
	BOOL	on;
	INT	i, cmd;

	for ( i = 0; i < WO_MSESS; i++ ) {
		WOMSESS	*s = &wo_ms[i];

		if ( !s->used || s->done ) {
			continue;
		}
		if ( s->skip != 0 && ev->type == HID_EV_BTN_UP && ev->code == s->ck.code && ev->when == s->skip ) {
			s->skip = 0;		/* the click's release, already counted when the menu began */
			continue;
		}
		if ( wm_click_take(&s->ck, ev) != WM_CK_PASS ) {
			continue;		/* a click: the menu stays; another button while it is held */
		}
		on = wm_menu_has(s->pid, ev->wid);
		if ( ev->type == HID_EV_BTN_DOWN && !on ) {
			menu_done(s, 0);	/* a press off the menu puts it away */
			continue;
		}
		cmd = 0;
		if ( wm_menu_event(s->pid, ev, &cmd) < E_OK ) {
			menu_done(s, 0);
			continue;
		}
		wm_update();
		if ( cmd != 0 ) {
			menu_done(s, cmd);
		} else if ( ev->type == HID_EV_BTN_UP && !on ) {
			menu_done(s, 0);	/* held, carried off it and let go */
		} else if ( ev->type == HID_EV_KEY_DOWN && ev->code == KEY_ESCAPE ) {
			menu_done(s, 0);
		}
	}
}

/*
 * Wait for the menu of panel `pid`, opened by a press at `when`, to be
 * answered: the command chosen, 0 for none, or an error. A process
 * ended while it waits (ts_ter_prc, knl_prc_ending) gets nothing, within
 * one slice of the wait, and its call closes the menu on the way out.
 */
EXPORT INT knl_wmobj_menu_run( INT pid, UD when )
{
	WOMSESS	*s = NULL;
	T_CFLG	cflg;
	T_RTSK	rt;
	UINT	ptn;
	INT	i, cmd;

	if ( wo_mtx <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	if ( wo_ms_flg <= 0 ) {
		cflg.exinf = NULL;
		cflg.flgatr = TA_TFIFO | TA_WMUL;
		cflg.iflgptn = 0;
		wo_ms_flg = tk_cre_flg(&cflg);
	}
	for ( i = 0; i < WO_MSESS; i++ ) {
		/* a slot whose waiter has gone may be taken again */
		if ( wo_ms[i].used && tk_ref_tsk(wo_ms[i].tskid, &rt) < E_OK ) {
			wo_ms[i].used = FALSE;
		}
		if ( !wo_ms[i].used && s == NULL ) {
			s = &wo_ms[i];
		}
	}
	if ( s != NULL ) {
		s->used = TRUE;
		s->done = FALSE;
		s->pid = pid;
		s->when = when;
		s->cmd = 0;
		s->tskid = tk_get_tid();
		wm_click_begin(&s->ck, when, WM_CK_FIND);	/* the button of the press at when */
		s->skip = 0;
		if ( s->ck.code < 32 && wo_up_when[s->ck.code] != 0 && wo_up_when[s->ck.code] >= when ) {
			/* the opening button let go already: a click, the menu stays */
			s->ck.live = FALSE;
			s->skip = wo_up_when[s->ck.code];
		}
	}
	UNLOCK();
	if ( s == NULL || wo_ms_flg <= 0 ) {
		return E_LIMIT;
	}
	while ( !s->done ) {
		(void)tk_wai_flg(wo_ms_flg, 1, TWF_ORW, &ptn, 200);
		if ( !s->done && knl_prc_ending() ) {
			LOCK();
			if ( !s->done ) {
				menu_done(s, 0);	/* its process is ending: put away */
			}
			UNLOCK();
		}
	}
	LOCK();
	cmd = s->cmd;
	s->used = FALSE;
	for ( i = 0; i < WO_MSESS && ( !wo_ms[i].used || wo_ms[i].done ); i++ ) ;
	if ( i == WO_MSESS ) {
		tk_clr_flg(wo_ms_flg, 0);	/* nobody else is waiting */
	}
	UNLOCK();
	return cmd;
}

LOCAL UD now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UD)(UW)t.lo;
}

/* While the desktop asks something over a program's window, the input is its panel's */
LOCAL volatile INT	wo_quiet = 0;

EXPORT void wm_obj_quiet( BOOL on )
{
	if ( on ) {
		wo_quiet++;
	} else if ( wo_quiet > 0 ) {
		wo_quiet--;
	}
}

/*
 * One input, addressed to a window. The pointer arriving over another
 * window is told to both, the one it left first; a press on the
 * pictogram in the band is a request to close as well as a press.
 */
/*
 * The candidates of a conversion, listed for a plain window (OB_WR_CAND):
 * a window of the window manager's own, in front and never having the
 * keys, drawn as the list under a box of a panel is (pd_cand_draw). It
 * is the system's, not the program's, so every program that converts
 * shows the same list; the program only says what is in it.
 */

/* The window the list pop is for, 0 none: called with the lock held */
LOCAL INT cand_owner( INT pop )
{
	INT	i;

	for ( i = 1; i <= wo_nwin; i++ ) {
		if ( WOW(i).used && WOW(i).cand == pop ) {
			return i;
		}
	}
	return 0;
}

/*
 * The list put up, changed or taken away as c says; its place from the
 * composition's box in the work area of window wid, under it or, with
 * no room below, over it, and kept on the screen
 */
LOCAL ER cand_put( INT wid, CONST T_OBWCAND *c )
{
	T_WMWIN		w;
	T_DPRECT	o;
	INT		pop, n, at, cw, ch, sw, sh, gid;

	LOCK();
	pop = WOW(wid).used ? WOW(wid).cand : 0;
	UNLOCK();
	/* as many of the names as are there whole, each shorter than a box's letters */
	for ( n = 0, at = 0; n < c->n && n < OB_CAND_MAX; n++ ) {
		INT	len = 0;

		while ( at + len < OB_CAND_BYTES && c->names[at + len] != 0 ) len++;
		if ( at + len >= OB_CAND_BYTES || len >= PD_CONV_TEXT ) break;
		at += len + 1;
	}
	if ( n <= 0 ) {
		if ( pop > 0 ) {
			LOCK();
			if ( WOW(wid).cand == pop ) WOW(wid).cand = 0;
			WOW(wid).ncand = 0;
			UNLOCK();
			(void)wm_close(pop);
			wm_update();
		}
		return E_OK;
	}
	if ( wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	pd_cand_size(c->names, n, c->total, &cw, &ch);
	{
		T_DPRECT	on;

		wm_screen_at(w.work.left + c->x, w.work.top, &on);
		sw = on.right;
		sh = on.bottom;
	}
	o.left = w.work.left + c->x;
	o.top = w.work.top + c->bottom + 2;
	if ( o.top + ch > sh && w.work.top + c->top - 2 - ch >= 0 ) {
		o.top = w.work.top + c->top - 2 - ch;
	}
	if ( o.left + cw > sw ) o.left = sw - cw;
	if ( o.left < 0 ) o.left = 0;
	o.right = o.left + cw;
	o.bottom = o.top + ch;
	if ( pop > 0 ) {
		(void)wm_move(pop, &o);
		(void)wm_raise(pop);
	} else {
		pop = wm_open(&o, WM_ATTR_POPUP | WM_ATTR_NOFOCUS, NULL);
		if ( pop <= 0 ) {
			return ( pop < 0 ) ? (ER)pop : E_LIMIT;
		}
	}
	LOCK();
	if ( WOW(wid).used ) {
		WOW(wid).cand = pop;
		WOW(wid).ncand = n;
	}
	UNLOCK();
	gid = wm_gid(pop);
	if ( gid >= 0 ) {
		pd_cand_draw(gid, cw, ch, c->names, n, ( c->chosen < n ) ? c->chosen : -1, c->first, c->total);
		o.right -= o.left;
		o.bottom -= o.top;
		o.left = o.top = 0;
		(void)wm_damage(pop, &o);
	}
	wm_update();
	return E_OK;
}

/* A press on a list: the name under it told to the window it is for, as a press on OB_WR_CAND */
LOCAL BOOL cand_press( CONST T_WMEV *ev, UINT event )
{
	T_OBNTM	nt;
	TS_UUID	u;
	INT	owner, row = -1;

	LOCK();
	owner = cand_owner(ev->wid);
	if ( owner > 0 ) {
		u = WOW(owner).uuid;
		row = pd_cand_row(ev->y, WOW(owner).ncand);
	}
	UNLOCK();
	if ( owner <= 0 ) {
		return FALSE;
	}
	if ( event == OB_E_PRESS && ev->code == 0 && row >= 0 ) {
		notice_of(ev, &nt);
		nt.code = (UINT)row;
		knl_ob_post(&u, OB_WR_CAND, OB_E_PRESS, &nt);
	}
	return TRUE;				/* the list's own: no window under it hears it */
}

/*
 * A button let go away from the window it went down on: that window is
 * told it was let go, at the place from its own work area (outside it),
 * so that what was begun there by the press -- shapes carried, a band
 * pulled out -- ends where the hand let go, as it would if the program
 * followed the pointer itself. ev->x, ev->y are from the work area of
 * ev->wid, or of the screen for the ground.
 */
LOCAL void release_away( CONST T_WMEV *ev )
{
	T_WMWIN	w;
	T_WMEV	up = *ev;
	INT	sx = ev->x, sy = ev->y;

	if ( ev->wid > 0 ) {
		if ( wm_ref(ev->wid, &w) < E_OK ) {
			return;
		}
		sx += w.work.left;
		sy += w.work.top;
	}
	if ( wm_ref(wo_down, &w) < E_OK ) {
		return;
	}
	up.x = sx - w.work.left;
	up.y = sy - w.work.top;
	up.wid = wo_down;
	win_post(wo_down, OB_E_RELEASE, &up);
}

EXPORT void knl_wmobj_event( CONST T_WMEV *ev )
{
	UINT	event;
	INT	was;

	if ( wo_mtx <= 0 || ev == NULL ) {
		return;
	}
	if ( ev->type == HID_EV_BTN_UP && ev->code < 32 ) {
		wo_up_when[ev->code] = ev->when;	/* before any menu looks at it (knl_wmobj_menu_run) */
	}
	if ( ev->type == HID_EV_BTN_DOWN ) {
		knl_wm_press_seen(ev->when, ev->code);	/* the button a menu opened by it follows */
	}
	if ( wo_quiet > 0 ) {
		return;
	}
	switch ( ev->type ) {
	case HID_EV_BTN_DOWN:	event = OB_E_PRESS;	break;
	case HID_EV_BTN_UP:	event = OB_E_RELEASE;	break;
	case HID_EV_MOVE:	event = OB_E_MOVE;	break;
	case HID_EV_KEY_DOWN:	event = OB_E_KEY;	break;
	case HID_EV_WHEEL:	event = OB_E_WHEEL;	break;
	case HID_EV_KEY_UP:
		/*
		 * A key let go goes to the window with the keys and to no
		 * menu or panel, marked so that only a request that asked
		 * for it (OB_N_KEYUP) hears it.
		 */
		if ( ev->wid > 0 && !menu_open() ) {
			T_WMEV	up = *ev;

			up.mods |= OB_KEY_UP;
			win_post(ev->wid, OB_E_KEY, &up);
		}
		return;
	default:		return;
	}
	/*
	 * While a menu is open the pointer and the keys are the menu's: the
	 * window under it is not told of the moves over it, of the button
	 * that opened it still held, nor of the press that puts it away.
	 */
	if ( menu_open() ) {
		drive_menus(ev);
		return;
	}
	if ( ev->wid > 0 && cand_press(ev, event) ) {
		return;
	}
	/*
	 * While something is carried (its outlines on the screen), a press
	 * is the carry's -- the second button makes it a copy -- and the
	 * window under the pointer is not told of it: it would take it for
	 * a press of its own and open its menu over the carry.
	 */
	if ( event == OB_E_PRESS && knl_wm_carrying() ) {
		return;
	}
	if ( event == OB_E_PRESS ) {
		wo_down = ev->wid;
		wo_down_ms = now_ms();
	}
	if ( event == OB_E_RELEASE && wo_down > 0 && ev->wid != wo_down ) {
		release_away(ev);
	}
	if ( ev->wid == 0 ) {
		/* the window the pointer was over is left for the ground as for another window */
		if ( event != OB_E_KEY && wo_hover != 0 ) {
			was = wo_hover;
			wo_hover = 0;
			win_post(was, OB_E_LEAVE, NULL);
		}
		drive_menus(ev);		/* on the ground: only a menu cares */
		return;
	}
	if ( event != OB_E_KEY && ev->wid != wo_hover ) {
		was = wo_hover;
		wo_hover = ev->wid;
		win_post(was, OB_E_LEAVE, NULL);
		win_post(ev->wid, OB_E_ENTER, ev);
	}
	/*
	 * A button let go over a window it did not go down on ends
	 * something begun elsewhere -- links carried there and dropped --
	 * and is not a click of that window's; the window it went down on
	 * was told (release_away).
	 */
	if ( event != OB_E_RELEASE || ev->wid == wo_down ) {
		win_post(ev->wid, event, ev);
	}

	if ( event == OB_E_PRESS && ev->wid > 0 ) {
		T_WMWIN	w;
		INT	got = 0;
		UINT	bar = 0;

		if ( wm_ref(ev->wid, &w) >= E_OK
		  && wm_part_at(w.work.left + ev->x, w.work.top + ev->y, &got, &bar)
		     == WM_PART_PICT && got == ev->wid
		  && wm_pict_close(ev->wid, w.work.left + ev->x, w.work.top + ev->y, ev->when) ) {
			win_post(ev->wid, OB_E_CLOSE, ev);
		}
	}
	drive_menus(ev);
	drive_panels(ev);
}

/*
 * A key as the keyboard gave it, told to the windows that never have
 * the keys: OB_E_KEY marked OB_KEY_SEEN (and OB_KEY_UP when let go),
 * which only a request with OB_N_KEYSEEN hears. A screen keyboard
 * lights the keys held with it. A window that is not shown hears
 * nothing, so what is typed is told only to a keyboard on the screen.
 */
#define WO_SEEN_MAX	8		/* screen keyboards told at once */

EXPORT void knl_wmobj_key_seen( CONST T_WMEV *ev )
{
	TS_UUID	to[WO_SEEN_MAX];
	INT	wid[WO_SEEN_MAX];
	T_OBNTM	n;
	T_WMWIN	w;
	INT	i, cnt = 0;

	if ( wo_mtx <= 0 || ev == NULL
	  || ( ev->type != HID_EV_KEY_DOWN && ev->type != HID_EV_KEY_UP ) ) {
		return;
	}
	LOCK();
	for ( i = 1; i <= wo_nwin && cnt < WO_SEEN_MAX; i++ ) {
		if ( WOW(i).used && WOW(i).nofocus ) {
			to[cnt] = WOW(i).uuid;
			wid[cnt] = i;
			cnt++;
		}
	}
	UNLOCK();
	if ( cnt == 0 ) {
		return;
	}
	notice_of(ev, &n);
	n.x = 0;
	n.y = 0;
	n.mods |= OB_KEY_SEEN | ( ( ev->type == HID_EV_KEY_UP ) ? OB_KEY_UP : 0 );
	for ( i = 0; i < cnt; i++ ) {
		if ( wm_ref(wid[i], &w) >= E_OK && w.visible ) {
			knl_ob_post(&to[i], -1, OB_E_KEY, &n);
		}
	}
}

/*
 * A button or a key pressed, wherever it goes: OB_E_PRESSED on every
 * window made as an object, a few at a time so that the lock is not
 * held while they are told.
 */
EXPORT void knl_wmobj_press_seen( CONST T_WMEV *ev )
{
	TS_UUID	to[WO_SEEN_MAX];
	T_OBNTM	n;
	INT	i, at = 1, cnt;

	if ( wo_mtx <= 0 || ev == NULL
	  || ( ev->type != HID_EV_BTN_DOWN && ev->type != HID_EV_KEY_DOWN ) ) {
		return;
	}
	notice_of(ev, &n);
	n.x = 0;
	n.y = 0;
	do {
		cnt = 0;
		LOCK();
		for ( ; at <= wo_nwin && cnt < WO_SEEN_MAX; at++ ) {
			if ( WOW(at).used ) to[cnt++] = WOW(at).uuid;
		}
		UNLOCK();
		for ( i = 0; i < cnt; i++ ) {
			knl_ob_post(&to[i], -1, OB_E_PRESSED, &n);
		}
	} while ( cnt == WO_SEEN_MAX );
}

/* The modifiers a key typed by a screen keyboard may be held with, and the layouts it may carry */
#define WO_PUT_MODS	( 0xFFU | KM_MOD_THUMB_L | KM_MOD_THUMB_R | KM_MOD_LAYOUTS )

/* The layouts a key carries, if any, as they must be: KM_MOD_OWN with numbers there are */
LOCAL BOOL put_layouts_ok( UINT mods )
{
	if ( ( mods & KM_MOD_LAYOUTS ) == 0 ) {
		return TRUE;
	}
	return (BOOL)( ( mods & KM_MOD_OWN ) != 0
		       && ( ( mods >> KM_MOD_LATIN_SHIFT ) & 3U ) < KM_LATIN_N
		       && ( ( mods >> KM_MOD_KANA_SHIFT ) & 3U ) < KM_KANA_N );
}

/*
 * A key's press or let go, put in as if the keyboard had sent it. The
 * modifiers go with the press only: none is held on the keyboard for it,
 * and a let go that carried them would tell a window that follows the
 * keys seen (OB_N_KEYSEEN) they are still down. The layouts the key is
 * read in go with both, so that a program that reads its letter when it
 * is let go reads the same one.
 */
LOCAL ER key_put_one( UINT type, UINT code, UINT mods )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.mods = ( type == HID_EV_KEY_DOWN ) ? mods : ( mods & KM_MOD_LAYOUTS );
	return knl_hid_inject(0, &e);
}

EXPORT ER wm_obj_key_put( INT wid, UINT code, UINT mods )
{
	BOOL	nofocus, pressed, held;
	UINT	how = mods & ( WM_KEY_PUT_DOWN | WM_KEY_PUT_UP );
	UINT	other = 0, other_mods = 0;
	UD	now = now_ms();
	ER	er;

	mods &= ~how;
	if ( code == 0 || code > 0xFF || ( mods & ~WO_PUT_MODS ) != 0 || !put_layouts_ok(mods)
	  || how == ( WM_KEY_PUT_DOWN | WM_KEY_PUT_UP ) ) {
		return E_PAR;
	}
	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_ID;
	}
	LOCK();
	nofocus = ( WOW(wid).used && WOW(wid).nofocus );
	pressed = ( wo_down == wid && now - wo_down_ms <= WM_KEY_PUT_MS );
	held = ( nofocus && WOW(wid).hold_code == code );
	if ( nofocus && how == WM_KEY_PUT_UP && held ) {
		mods = WOW(wid).hold_mods;
		WOW(wid).hold_code = 0;
	} else if ( nofocus && how == WM_KEY_PUT_DOWN && ( pressed || held ) ) {
		if ( !held ) {
			other = WOW(wid).hold_code;
			other_mods = WOW(wid).hold_mods;
		}
		WOW(wid).hold_code = code;
		WOW(wid).hold_mods = mods;
	}
	UNLOCK();
	if ( !nofocus ) {
		return E_OBJ;
	}
	if ( how == WM_KEY_PUT_UP ) {
		return held ? key_put_one(HID_EV_KEY_UP, code, mods) : E_OACV;
	}
	if ( !pressed && !( how == WM_KEY_PUT_DOWN && held ) ) {
		return E_OACV;
	}
	if ( other != 0 ) {
		(void)key_put_one(HID_EV_KEY_UP, other, other_mods);
	}
	er = key_put_one(HID_EV_KEY_DOWN, code, mods);
	if ( er < E_OK || how == WM_KEY_PUT_DOWN ) {
		return er;
	}
	return key_put_one(HID_EV_KEY_UP, code, mods);
}

LOCAL void repaint( INT wid );

/*
 * A window moved. One that changed its size has lost what it showed:
 * one drawn from its drawing record is drawn again here, and one its
 * program draws directly is told to.
 */
EXPORT void knl_wmobj_moved( INT wid, BOOL resized )
{
	BOOL	drawn = FALSE, known = FALSE;

	if ( wo_mtx <= 0 || !resized || wid < 1 || wid > wo_nwin ) {
		return;
	}
	LOCK();
	if ( WOW(wid).used ) {
		known = TRUE;
		drawn = ( WOW(wid).len[OB_WR_DRAW] > 0 );
	}
	UNLOCK();
	if ( !known ) {
		return;
	}
	if ( drawn ) {
		repaint(wid);
	} else {
		INT	pid;

		win_post(wid, OB_E_REDRAW, NULL);
		/* the panels placed on it are drawn again: what was drawn went with the old size */
		for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) {
			if ( wm_panel_wid(pid) == wid ) {
				knl_pn_fit(pid);	/* one whose sheets scroll takes the new height */
				wm_panel_draw(pid);
			}
		}
	}
}

/*
 * Something happened to one part of a panel. A menu's items are its
 * records, so there it is the menu that is told, on the item's record;
 * a panel's parts are objects of their own.
 */
EXPORT void knl_wmobj_part( INT pid, INT i, UINT event, CONST T_WMEV *ev )
{
	T_OBNTM	n;
	TS_UUID	u;
	BOOL	menu = FALSE, known = FALSE;
	INT	recno = -1;

	if ( wo_mtx <= 0 || pid < 1 || pid > WM_PANEL_MAX || i < 0 ) {
		return;
	}
	(void)knl_pn_count(pid, &menu);
	LOCK();
	if ( wo_pnl[pid].used ) {
		if ( menu ) {
			u = wo_pnl[pid].uuid;
			recno = OB_WR_KIND + i;
			known = TRUE;
		} else if ( i < wo_pnl[pid].npart ) {
			u = wo_pnl[pid].part[i];
			known = TRUE;
		}
	}
	UNLOCK();
	if ( known ) {
		notice_of(ev, &n);
		knl_ob_post(&u, recno, event, &n);
	}
	/*
	 * What happens to a part of a panel is told to the panel too, on
	 * the record that links the part, so that a program working a
	 * whole panel asks the panel once rather than every part.
	 */
	if ( known && !menu ) {
		LOCK();
		known = wo_pnl[pid].used;
		u = wo_pnl[pid].uuid;
		UNLOCK();
		if ( known ) {
			notice_of(ev, &n);
			knl_ob_post(&u, OB_WR_KIND + i, event, &n);
		}
	}
}

/* ---------------------------------------------------------------- drops */

/*
 * What a drop granted its taker, taken back: the drop was refused, or a
 * new one came before it was answered and it can no longer be read.
 */
LOCAL void drop_ungrant( CONST T_OBDROP *d, ID taker )
{
	INT	i;

	for ( i = 0; i < d->n; i++ ) {
		if ( d->v[i].ops != 0 ) {
			knl_ob_grant(taker, &d->v[i].target, 0);
		}
	}
}

EXPORT ID wm_obj_maker( INT wid )
{
	ID	m = 0;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return 0;
	}
	LOCK();
	if ( WOW(wid).used ) m = WOW(wid).maker;
	UNLOCK();
	return m;
}

/*
 * Every window a program made asked to close, as its close box asks it: the
 * programs that made them, up to 'max', in 'pids'; how many. A panel or a list
 * over a window (WM_ATTR_POPUP) has no close box, and is not asked.
 */
EXPORT INT wm_obj_close_programs( ID *pids, INT max )
{
	INT	wid, i, n = 0, nwin = wo_nwin;

	if ( wo_mtx <= 0 ) {
		return 0;
	}
	for ( wid = 1; wid <= nwin; wid++ ) {
		T_WMWIN	w;
		ID	m = 0;

		LOCK();
		if ( WOW(wid).used ) {
			m = WOW(wid).maker;
		}
		UNLOCK();
		if ( m <= 0 || wm_ref(wid, &w) < E_OK || ( w.attr & WM_ATTR_POPUP ) != 0 ) {
			continue;
		}
		win_post(wid, OB_E_CLOSE, NULL);
		for ( i = 0; i < n && pids[i] != m; i++ ) ;
		if ( i == n && n < max ) {
			pids[n++] = m;
		}
	}
	return n;
}

/* A window of its own the process still has, not a panel or a list over one: 0 none */
EXPORT INT wm_obj_program_window( ID pid )
{
	INT	wid, nwin = wo_nwin;

	if ( wo_mtx <= 0 ) {
		return 0;
	}
	for ( wid = 1; wid <= nwin; wid++ ) {
		T_WMWIN	w;
		BOOL	his;

		LOCK();
		his = (BOOL)( WOW(wid).used && WOW(wid).maker == pid );
		UNLOCK();
		if ( his && wm_ref(wid, &w) >= E_OK && ( w.attr & WM_ATTR_POPUP ) == 0 ) {
			return wid;
		}
	}
	return 0;
}

/* Whether the keys are with a question that waits: nothing else takes them from it */
LOCAL BOOL keys_asked( void )
{
	T_WMWIN	f;
	INT	was = wm_focused();

	return (BOOL)( was > 0 && wm_ref(was, &f) >= E_OK && ( f.attr & WM_ATTR_POPUP ) != 0 );
}

/*
 * Something let go on a window goes on there: it comes forward with the
 * keys, as a press on it would, before its program hears of the drop.
 * Which window had them is kept, for a drop it refuses.
 */
LOCAL void drop_activate( INT wid )
{
	T_WMWIN	w;
	INT	was = wm_focused();

	if ( keys_asked() || wm_ref(wid, &w) < E_OK
	  || ( w.attr & ( WM_ATTR_POPUP | WM_ATTR_NOFOCUS ) ) != 0
	  || ( ( w.attr & WM_ATTR_OWN ) == 0 && w.parent <= 0 ) ) {
		was = 0;
	} else {
		wm_raise(wid);
		if ( was == wid || wm_focus(wid) < E_OK ) {
			was = 0;
		}
		wm_update();
	}
	LOCK();
	if ( WOW(wid).used ) WOW(wid).dropback = was;
	UNLOCK();
}

/*
 * A window a program has opened takes the keys when a person's act is
 * why it opened: the first ordinary window of a process started at their
 * asking, while they have not gone on to something else; or one that a
 * program with the keys opens just after a press. Anything else opens
 * without them: a program in the background cannot take what is being
 * typed elsewhere. A panel, a sub-window, a floating window and a window
 * that never has the keys leave the ticket for the window to come.
 */
#define OPEN_FOLLOWS_MS	2000

LOCAL void open_activate( INT wid, ID maker, UINT attr, BOOL sub )
{
	INT	f;
	UINT	t;

	if ( maker <= 0 || sub || ( attr & WM_ATTR_OWN ) == 0
	  || ( attr & ( WM_ATTR_POPUP | WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ) ) != 0 ) {
		return;
	}
	t = knl_ob_act_take(maker);
	f = wm_focused();
	if ( !keys_asked() && ( wm_act_good(t)
			       || ( f != wid && wm_obj_maker(f) == maker && wm_act_age() < OPEN_FOLLOWS_MS ) ) ) {
		(void)wm_focus(wid);
	}
}

/* What a process may do to an object: by its credentials, and what a drop granted it */
LOCAL UINT rights_of( ID pid, CONST TS_UUID *target )
{
	return knl_ob_permit_on(target, knl_ob_crd_of(pid)) | knl_ob_granted(pid, target);
}

EXPORT ER wm_obj_drop_may( INT wid, ID giver, CONST TS_UUID *target )
{
	TS_UUID	u;
	ID	taker = 0;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_PAR;
	}
	if ( wm_obj_uuid(wid, &u) < E_OK ) {
		return E_NOEXS;
	}
	if ( !knl_ob_taker(&u, OB_E_DROP, &taker) ) {
		return E_NOSPT;
	}
	if ( target != NULL
	  && ( ( rights_of(giver, target) & OB_OP_R ) != OB_OP_R
	    || ( rights_of(taker, target) & OB_OP_R ) != OB_OP_R ) ) {
		return E_OACV;
	}
	return E_OK;
}

/*
 * A question whether links would be taken: told to whoever asked
 * OB_E_ASK of the window, numbered so that an answer to one asked before
 * is not taken for this one's.
 */
LOCAL UINT	ask_count = 0;

/* The question of links, or with letters of letters, asked of the one who takes them */
LOCAL ER ask_window( INT wid, INT sx, INT sy, UINT mods, BOOL letters, UINT *p_seq )
{
	T_OBNTM	nt;
	T_WMWIN	w;
	SYSTIM	now;
	TS_UUID	u;
	ID	taker = 0;
	UINT	seq;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin || p_seq == NULL ) {
		return E_PAR;
	}
	if ( wm_obj_uuid(wid, &u) < E_OK || wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	if ( !knl_ob_taker(&u, OB_E_ASK, &taker)
	  || !knl_ob_taker(&u, letters ? OB_E_TEXT : OB_E_DROP, &taker) ) {
		return E_NOSPT;
	}
	LOCK();
	if ( ++ask_count == 0 ) {
		ask_count = 1;
	}
	seq = ask_count;
	WOW(wid).askseq = seq;
	WOW(wid).askans = 0;
	UNLOCK();
	knl_memset(&nt, 0, sizeof(nt));
	nt.code = seq;
	nt.mods = mods | ( letters ? OB_ASK_LETTERS : 0 );
	nt.x = sx - w.work.left;
	nt.y = sy - w.work.top;
	if ( tk_get_otm(&now) >= E_OK ) {
		nt.when = ( (UD)(UW)now.hi << 32 ) | (UD)now.lo;
	}
	knl_ob_post(&u, OB_WR_ASK, OB_E_ASK, &nt);
	*p_seq = seq;
	return E_OK;
}

EXPORT ER wm_obj_ask( INT wid, INT sx, INT sy, UINT mods, UINT *p_seq )
{
	return ask_window(wid, sx, sy, mods, FALSE, p_seq);
}

EXPORT ER wm_obj_ask_text( INT wid, INT sx, INT sy, UINT mods, UINT *p_seq )
{
	return ask_window(wid, sx, sy, mods, TRUE, p_seq);
}

EXPORT ER wm_obj_asked( INT wid, UINT seq )
{
	ER	er;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin ) {
		return E_PAR;
	}
	LOCK();
	er = ( !WOW(wid).used || WOW(wid).askseq != seq ) ? E_OBJ
	   : ( WOW(wid).askans == OB_DR_ACCEPT ) ? E_OK
	   : ( WOW(wid).askans == OB_DR_REFUSE ) ? E_NOEXS : E_TMOUT;
	UNLOCK();
	return er;
}

/* The answer to a question, written by whoever takes the window's drops */
LOCAL ER ask_answer( INT wid, CONST T_OBDRANS *an )
{
	ER	er = E_OBJ;

	if ( an->answer != OB_DR_ACCEPT && an->answer != OB_DR_REFUSE ) {
		return E_PAR;
	}
	LOCK();
	if ( WOW(wid).used && WOW(wid).askseq == an->seq && WOW(wid).askans == 0 ) {
		WOW(wid).askans = an->answer;
		er = E_OK;
	}
	UNLOCK();
	return er;
}

/*
 * byprog: the links come from the process giver, not from the person at
 * the desktop. Each must be readable by both processes, and the taker
 * gains nothing: ops only says what it already may do that the giver may
 * too. Otherwise the taker is granted what the caller may do.
 */
LOCAL ER drop_links( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
		     BOOL byprog, ID giver, CONST T_OBDROPV *v, INT n )
{
	CONST T_OBCRD	*crd;
	T_OBDROP	*d, *old = NULL;
	T_WMWIN		w;
	T_OBNTM		nt;
	SYSTIM		now;
	TS_UUID		u;
	ID		taker = 0, oldtaker = 0;
	INT		i;
	BOOL		revoke = FALSE;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin || v == NULL || n <= 0 ) {
		return E_PAR;
	}
	if ( wm_obj_uuid(wid, &u) < E_OK || wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	if ( !knl_ob_taker(&u, OB_E_DROP, &taker) ) {
		return E_NOSPT;			/* nobody there takes drops */
	}
	if ( byprog ) {
		for ( i = 0; i < n; i++ ) {
			if ( wm_obj_drop_may(wid, giver, &v[i].target) == E_OACV ) {
				return E_OACV;
			}
		}
	}
	/* every link let go */
	d = (T_OBDROP *)Kcalloc(1, OB_DROP_SIZE(n));
	if ( d == NULL ) {
		return E_NOMEM;
	}
	d->n = n;
	d->x = sx - w.work.left;
	d->y = sy - w.work.top;
	d->mods = mods;
	if ( from != NULL ) {
		INT	fw = wm_obj_wid(from);

		d->from = *from;
		LOCK();
		if ( fw >= 1 && fw <= wo_nwin && WOW(fw).used ) {
			d->fromobj = WOW(fw).shows;	/* what the links were in */
		}
		UNLOCK();
	}

	/*
	 * The taker acts on each object with the right of the one who
	 * dropped it, not with its own: it is granted what that one may do
	 * to it, but never deleting it nor changing its protection.
	 */
	crd = knl_ob_crd_of(ts_get_pid());
	for ( i = 0; i < n; i++ ) {
		d->v[i] = v[i];
		d->v[i].name[OB_DROP_NAME - 1] = 0;
		if ( byprog ) {
			d->v[i].ops = rights_of(giver, &v[i].target) & rights_of(taker, &v[i].target) & OB_DROP_OPS;
			continue;
		}
		d->v[i].ops = knl_ob_permit_on(&v[i].target, crd) & OB_DROP_OPS;
		if ( d->v[i].ops != 0 ) {
			knl_ob_grant(taker, &v[i].target, d->v[i].ops);
		}
	}

	LOCK();
	if ( !WOW(wid).used || !same_uuid(&WOW(wid).uuid, &u) ) {
		UNLOCK();
		if ( !byprog ) {
			drop_ungrant(d, taker);
		}
		Kfree(d);
		return E_NOEXS;			/* it closed meanwhile */
	}
	old = WOW(wid).drop;
	if ( old != NULL && !WOW(wid).answered && WOW(wid).gave ) {
		oldtaker = WOW(wid).taker;
		revoke = TRUE;
	}
	d->seq = ++WOW(wid).seq;
	WOW(wid).drop = d;
	WOW(wid).droplen = (SZ)OB_DROP_SIZE(n);
	WOW(wid).taker = taker;
	WOW(wid).answered = FALSE;
	WOW(wid).gave = !byprog;
	UNLOCK();
	if ( old != NULL ) {
		if ( revoke ) {
			/* only what the new one does not give again */
			for ( i = 0; i < old->n; i++ ) {
				INT	k;

				for ( k = 0; k < n && !same_uuid(&old->v[i].target, &v[k].target); k++ ) ;
				if ( k == n || oldtaker != taker ) {
					knl_ob_grant(oldtaker, &old->v[i].target, 0);
				}
			}
		}
		Kfree(old);
	}

	knl_memset(&nt, 0, sizeof(nt));
	nt.code = (UINT)n;
	nt.mods = mods;
	nt.x = sx - w.work.left;
	nt.y = sy - w.work.top;
	if ( tk_get_otm(&now) >= E_OK ) {
		nt.when = ( (UD)(UW)now.hi << 32 ) | (UD)now.lo;
	}
	drop_activate(wid);
	knl_ob_post(&u, OB_WR_DROP, OB_E_DROP, &nt);

	return E_OK;
}

EXPORT ER wm_obj_drop( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
		       CONST T_OBDROPV *v, INT n )
{
	return drop_links(wid, sx, sy, mods, from, FALSE, 0, v, n);
}

EXPORT ER wm_obj_drop_for( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
			   ID giver, CONST T_OBDROPV *v, INT n )
{
	return drop_links(wid, sx, sy, mods, from, TRUE, giver, v, n);
}

EXPORT ER wm_obj_drop_text( INT wid, INT sx, INT sy, UINT mods, CONST TS_UUID *from,
			    CONST UB *text, INT n )
{
	T_OBDROP	*d, *old = NULL;
	T_WMWIN		w;
	T_OBNTM		nt;
	SYSTIM		now;
	TS_UUID		u;
	ID		taker = 0, oldtaker = 0;
	BOOL		revoke = FALSE;

	if ( wo_mtx <= 0 || wid < 1 || wid > wo_nwin || text == NULL || n <= 0 ) {
		return E_PAR;
	}
	if ( wm_obj_uuid(wid, &u) < E_OK || wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	if ( !knl_ob_taker(&u, OB_E_TEXT, &taker) ) {
		return E_NOSPT;			/* nobody there takes letters */
	}
	/* every letter let go, and a nought after them */
	d = (T_OBDROP *)Kcalloc(1, sizeof(T_OBDROP) + (SZ)n + 1);
	if ( d == NULL ) {
		return E_NOMEM;
	}
	d->x = sx - w.work.left;
	d->y = sy - w.work.top;
	d->mods = mods;
	knl_memcpy(OB_DROP_LETTERS(d), text, n);
	if ( from != NULL ) {
		INT	fw = wm_obj_wid(from);

		d->from = *from;
		LOCK();
		if ( fw >= 1 && fw <= wo_nwin && WOW(fw).used ) {
			d->fromobj = WOW(fw).shows;
		}
		UNLOCK();
	}

	LOCK();
	if ( !WOW(wid).used || !same_uuid(&WOW(wid).uuid, &u) ) {
		UNLOCK();
		Kfree(d);
		return E_NOEXS;			/* it closed meanwhile */
	}
	old = WOW(wid).drop;
	if ( old != NULL && !WOW(wid).answered && WOW(wid).gave ) {
		oldtaker = WOW(wid).taker;
		revoke = TRUE;
	}
	d->seq = ++WOW(wid).seq;
	WOW(wid).drop = d;
	WOW(wid).droplen = (SZ)sizeof(T_OBDROP) + (SZ)n + 1;
	WOW(wid).taker = taker;
	WOW(wid).answered = FALSE;
	WOW(wid).gave = FALSE;
	UNLOCK();
	if ( old != NULL ) {
		if ( revoke ) {
			drop_ungrant(old, oldtaker);	/* the links not taken: nothing granted any more */
		}
		Kfree(old);
	}

	knl_memset(&nt, 0, sizeof(nt));
	nt.code = 0;
	nt.mods = mods;
	nt.x = d->x;
	nt.y = d->y;
	if ( tk_get_otm(&now) >= E_OK ) {
		nt.when = ( (UD)(UW)now.hi << 32 ) | (UD)now.lo;
	}
	drop_activate(wid);
	knl_ob_post(&u, OB_WR_DROP, OB_E_DROP, &nt);

	return E_OK;
}

/*
 * The answer to a drop, written by its taker: taken or not, said on the
 * message line to the one who dropped it. A refused drop's grants go.
 */
/*
 * A bar of a window worked by the user, told to whoever asked
 * OB_E_SCROLL of it: where the bar's start is asked to go, and how.
 */
EXPORT void wm_obj_scroll( INT wid, UINT which, INT pos, UINT how )
{
	T_OBNTM	nt;
	SYSTIM	now;
	TS_UUID	u;
	BOOL	known = FALSE;

	if ( wid < 1 || wid > wo_nwin ) {
		return;
	}
	LOCK();
	if ( WOW(wid).used ) {
		u = WOW(wid).uuid;
		known = TRUE;
	}
	UNLOCK();
	if ( !known ) {
		return;
	}
	if ( which == WM_BAR_R && knl_pn_scroll(wid, pos) ) {
		return;				/* the bar of a panel whose sheets scroll: the panel's own */
	}
	knl_memset(&nt, 0, sizeof(nt));
	nt.code = which;
	nt.x = pos;
	nt.y = (INT)how;
	if ( tk_get_otm(&now) >= E_OK ) {
		nt.when = ( (UD)(UW)now.hi << 32 ) | (UD)now.lo;
	}
	knl_ob_post(&u, OB_WR_BARS, OB_E_SCROLL, &nt);
}

LOCAL ER drop_answer( INT wid, CONST T_OBDRANS *an )
{
	T_OBDROP	*d, *keep = NULL;
	ID		taker = 0;
	BOOL		refuse;
	INT		back;

	if ( an->answer != OB_DR_ACCEPT && an->answer != OB_DR_REFUSE ) {
		return E_PAR;
	}
	refuse = (BOOL)( an->answer == OB_DR_REFUSE );
	LOCK();
	d = WOW(wid).drop;
	if ( !WOW(wid).used || d == NULL || d->seq != an->seq || WOW(wid).answered ) {
		UNLOCK();
		return E_OBJ;			/* not the drop there is, or answered already */
	}
	if ( refuse && WOW(wid).gave ) {
		/* the links it gave, kept to take their grants back outside the lock */
		keep = (T_OBDROP *)Kmalloc(WOW(wid).droplen);
		if ( keep == NULL ) {
			UNLOCK();
			return E_NOMEM;
		}
		knl_memcpy(keep, d, (INT)WOW(wid).droplen);
	}
	WOW(wid).answered = TRUE;
	taker = WOW(wid).taker;
	back = WOW(wid).dropback;
	WOW(wid).dropback = 0;
	UNLOCK();
	/* refused, nothing came in: the keys go back, unless the person has moved them since */
	if ( refuse && back > 0 && wm_focused() == wid ) {
		wm_raise(back);
		(void)wm_focus(back);
		wm_update();
	}
	if ( keep != NULL ) {
		drop_ungrant(keep, taker);
		Kfree(keep);
	}

	if ( an->msg[0] != 0 ) {
		(void)wm_msg_put((CONST char *)an->msg);
	} else if ( refuse ) {
		(void)wm_msg_put("このウインドウには置けません");
	}
	return E_OK;
}

/* ---------------------------------------------------------------- finding */

LOCAL INT locate( CONST TS_UUID *uuid )
{
	INT	i, k, h = 0;

	if ( wo_mtx <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < WO_FREE_MAX && h == 0; i++ ) {
		if ( wo_free[i].used && same_uuid(&wo_free[i].uuid, uuid) ) {
			h = H_MAKE(H_FREE, i, 0);	/* first: placed, it is also below */
		}
	}
	for ( i = 1; i <= wo_nwin && h == 0; i++ ) {
		if ( WOW(i).used && same_uuid(&WOW(i).uuid, uuid) ) {
			h = H_MAKE(H_WIN, i, 0);
		}
	}
	for ( i = 1; i <= WM_PANEL_MAX && h == 0; i++ ) {
		if ( !wo_pnl[i].used ) {
			continue;
		}
		if ( same_uuid(&wo_pnl[i].uuid, uuid) ) {
			h = H_MAKE(H_PNL, i, 0);
		}
		for ( k = 0; k < wo_pnl[i].npart && h == 0; k++ ) {
			if ( same_uuid(&wo_pnl[i].part[k], uuid) ) {
				h = H_MAKE(H_PART, i, k);
			}
		}
	}
	UNLOCK();

	return ( h != 0 ) ? h : E_NOEXS;
}

/* Whether what a handle names is still there */
LOCAL BOOL alive( INT h )
{
	BOOL	yes = FALSE;

	LOCK();
	switch ( H_KIND(h) ) {
	case H_WIN:
		yes = ( H_A(h) >= 1 && H_A(h) <= wo_nwin && WOW(H_A(h)).used );
		break;
	case H_PNL:
	case H_PART:
		yes = ( H_A(h) >= 1 && H_A(h) <= WM_PANEL_MAX && wo_pnl[H_A(h)].used
		     && ( H_KIND(h) == H_PNL || H_B(h) < wo_pnl[H_A(h)].npart ) );
		break;
	case H_FREE:
		yes = ( H_A(h) < WO_FREE_MAX && wo_free[H_A(h)].used );
		break;
	default:
		break;
	}
	UNLOCK();

	return yes;
}

LOCAL BOOL is_menu( INT h )
{
	BOOL	menu = FALSE;

	return ( H_KIND(h) == H_PNL && knl_pn_count(H_A(h), &menu) >= 0 && menu );
}

LOCAL UINT sub_of( INT h )
{
	switch ( H_KIND(h) ) {
	case H_WIN:	return OB_S_WINDOW;
	case H_PART:	return OB_S_PART;
	case H_FREE:	return wo_free[H_A(h)].sub;
	default:	return is_menu(h) ? OB_S_MENU : OB_S_PANEL;
	}
}

LOCAL INT nrec_of( INT h )
{
	INT	n = 0;

	if ( H_KIND(h) == H_WIN ) {
		return WO_NREC + 3;		/* and the drop record, the bars, the candidates */
	}
	if ( H_KIND(h) == H_PART ) {
		return WO_NREC + 1;		/* and a selector's names */
	}
	if ( H_KIND(h) != H_PNL ) {
		return WO_NREC;
	}
	LOCK();
	if ( wo_pnl[H_A(h)].used ) {
		n = wo_pnl[H_A(h)].npart;
	}
	UNLOCK();

	return WO_NREC + n;
}

LOCAL WOPRT *prt_slot( INT h )
{
	return ( H_KIND(h) == H_WIN ) ? &WOW(H_A(h)).prt
	     : ( H_KIND(h) == H_FREE ) ? &wo_free[H_A(h)].prt : &wo_pnl[H_A(h)].prt;
}

/* A part of a panel copied out, in memory the caller frees */
LOCAL T_WMPART *part_copy( INT pid, INT i )
{
	T_WMPART	*pt = (T_WMPART *)Kmalloc(sizeof(T_WMPART));

	if ( pt != NULL && knl_pn_part(pid, i, pt) < E_OK ) {
		Kfree(pt);
		pt = NULL;
	}
	return pt;
}

/* Whether a part's state is text rather than a number */
LOCAL BOOL part_is_text( CONST T_WMPART *pt )
{
	UINT	k = WM_PT_KIND(pt->type);

	return (BOOL)( k == TB_PARTS || k == XB_PARTS || k == NB_PARTS || k == SB_PARTS );
}

LOCAL INT str_len( CONST UB *s, INT max )
{
	INT	n = 0;

	while ( n < max && s[n] != 0 ) n++;
	return n;
}

/* ---------------------------------------------------------------- records */

/*
 * A record's bytes, made afresh into memory the caller frees. What the
 * window layer holds is asked of it each time, so what is read is what
 * is on the screen.
 */
LOCAL ER rec_bytes( INT h, INT recno, UB **p_buf, SZ *p_len )
{
	UB		*b = NULL;
	SZ		n = 0;
	T_WMPART	*pt;
	ER		er = E_OK;

	*p_buf = NULL;
	*p_len = 0;
	if ( recno < 0 || recno >= nrec_of(h) ) {
		return E_NOEXS;
	}
	if ( recno == OB_WR_DRAW || recno == OB_WR_SHAPE ) {
		if ( H_KIND(h) != H_WIN ) {
			return E_OK;			/* a panel draws itself */
		}
		LOCK();
		n = WOW(H_A(h)).len[recno];
		if ( n > 0 ) {
			b = (UB *)Kmalloc(n);
			if ( b != NULL ) {
				knl_memcpy(b, WOW(H_A(h)).rec[recno], (INT)n);
			}
		}
		UNLOCK();
		if ( n > 0 && b == NULL ) {
			return E_NOMEM;
		}
		*p_buf = b;
		*p_len = n;
		return E_OK;
	}
	if ( recno == OB_WR_PLACE ) {
		T_OBWPOS	*wp = (T_OBWPOS *)Kmalloc(sizeof(T_OBWPOS));

		if ( wp == NULL ) {
			return E_NOMEM;
		}
		knl_memset(wp, 0, sizeof(*wp));
		wm_screen(&wp->sw, &wp->sh);
		if ( H_KIND(h) == H_WIN ) {
			T_WMWIN	w;

			er = wm_ref(H_A(h), &w);
			wp->left = w.outer.left;
			wp->top = w.outer.top;
			wp->right = w.outer.right;
			wp->bottom = w.outer.bottom;
			wp->wleft = w.work.left;
			wp->wtop = w.work.top;
			wp->wright = w.work.right;
			wp->wbottom = w.work.bottom;
			wp->z = w.z;
			wp->flags = w.visible ? OB_WP_SHOWN : 0;
			if ( er >= E_OK && wm_focused() == H_A(h) ) {
				wp->flags |= OB_WP_FOCUS;	/* the keys go to it */
			}
		} else if ( H_KIND(h) == H_PNL ) {
			T_DPRECT	r;
			INT		wid = wm_panel_wid(H_A(h));

			er = wm_panel_rect(H_A(h), &r);
			wp->left = r.left;
			wp->top = r.top;
			wp->right = r.right;
			wp->bottom = r.bottom;
			wp->flags = OB_WP_SHOWN;
			LOCK();
			if ( wid >= 1 && wid <= wo_nwin && WOW(wid).used ) {
				wp->parent = WOW(wid).uuid;
			}
			UNLOCK();
		} else {
			pt = part_copy(H_A(h), H_B(h));
			if ( pt == NULL ) {
				er = E_NOEXS;
			} else {
				wp->left = pt->r.left;
				wp->top = pt->r.top;
				wp->right = pt->r.right;
				wp->bottom = pt->r.bottom;
				wp->flags = knl_pn_flags(H_A(h), H_B(h));
				Kfree(pt);
			}
			LOCK();
			wp->parent = wo_pnl[H_A(h)].uuid;
			UNLOCK();
		}
		if ( er < E_OK ) {
			Kfree(wp);
			return er;
		}
		*p_buf = (UB *)wp;
		*p_len = sizeof(*wp);
		return E_OK;
	}
	if ( recno == OB_WR_STATE ) {
		if ( H_KIND(h) != H_PART ) {
			return E_OK;
		}
		b = (UB *)Kmalloc(WM_LABEL_MAX);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		n = knl_pn_state(H_A(h), H_B(h), b, WM_LABEL_MAX);
		if ( n < 0 ) {
			Kfree(b);
			return (ER)n;
		}
		*p_buf = b;
		*p_len = n;
		return E_OK;
	}
	if ( H_KIND(h) == H_PART ) {
		/* a selector's names, each ending in a nought */
		CONST UB	*e;

		pt = part_copy(H_A(h), H_B(h));
		if ( pt == NULL ) {
			return E_NOEXS;
		}
		if ( pt->count > 0 && ( WM_PT_KIND(pt->type) == WS_PARTS || WM_PT_KIND(pt->type) == SS_PARTS
				    || WM_PT_KIND(pt->type) == WM_PT_POPUP ) ) {
			b = (UB *)Kmalloc(WM_POOL_BYTES);
			if ( b != NULL ) {
				n = knl_pn_names(H_A(h), H_B(h), b, WM_POOL_BYTES);
				if ( n < 0 ) n = 0;
			}
		}
		(void)e;
		Kfree(pt);
		*p_buf = b;
		*p_len = n;
		return E_OK;
	}

	if ( H_KIND(h) == H_WIN && recno == OB_WR_BARS ) {
		T_OBWBARS	*bs = (T_OBWBARS *)Kmalloc(sizeof(T_OBWBARS));
		UINT		i;

		if ( bs == NULL ) {
			return E_NOMEM;
		}
		knl_memset(bs, 0, sizeof(*bs));
		for ( i = 0; i < OB_BAR_MAX && i < WM_BAR_MAX; i++ ) {
			T_WMBAR	b;

			if ( wm_bar(H_A(h), i, &b) >= E_OK ) {
				bs->bar[i].lo = b.lo;
				bs->bar[i].hi = b.hi;
				bs->bar[i].clo = b.clo;
				bs->bar[i].chi = b.chi;
			}
		}
		*p_buf = (UB *)bs;
		*p_len = sizeof(*bs);
		return E_OK;
	}
	if ( H_KIND(h) == H_WIN && ( recno == OB_WR_CAND || recno == OB_WR_ASK ) ) {
		return E_OK;			/* written only: nothing to read */
	}
	if ( H_KIND(h) == H_WIN ) {
		/* the latest drop, empty when there has been none */
		LOCK();
		if ( WOW(H_A(h)).drop != NULL ) {
			n = WOW(H_A(h)).droplen;
			b = (UB *)Kmalloc(n);
			if ( b != NULL ) {
				knl_memcpy(b, WOW(H_A(h)).drop, (INT)n);
			}
		}
		UNLOCK();
		if ( n > 0 && b == NULL ) {
			return E_NOMEM;
		}
		*p_buf = b;
		*p_len = n;
		return E_OK;
	}

	/* a panel's links, a menu's items */
	if ( is_menu(h) ) {
		pt = part_copy(H_A(h), recno - OB_WR_KIND);
		if ( pt == NULL ) {
			return E_NOEXS;
		}
		n = str_len(pt->label, WM_LABEL_MAX);
		b = (UB *)Kmalloc(n + 1);
		if ( b != NULL ) knl_memcpy(b, pt->label, (INT)n);
		Kfree(pt);
	} else {
		n = sizeof(TS_UUID);
		b = (UB *)Kmalloc(n);
		if ( b != NULL ) {
			LOCK();
			knl_memcpy(b, &wo_pnl[H_A(h)].part[recno - OB_WR_KIND], (INT)n);
			UNLOCK();
		}
	}
	if ( b == NULL ) {
		return E_NOMEM;
	}
	*p_buf = b;
	*p_len = n;
	return E_OK;
}

/* A menu item's state, as its record's subtype says it */
LOCAL UINT item_state( INT pid, INT i )
{
	T_WMPART	*pt = part_copy(pid, i);
	UINT		s = 0;

	if ( pt == NULL ) {
		return 0;
	}
	if ( ( pt->type & P_DISABLE ) != 0 ) s |= OB_MI_GREY;
	if ( pt->value != 0 ) s |= OB_MI_TICK;
	if ( WM_PT_KIND(pt->type) == WM_PT_LABEL ) s |= OB_MI_LINE;
	if ( pt->sub > 0 ) s |= OB_MI_SUB;
	Kfree(pt);

	return s;
}

LOCAL ER fr_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt );
LOCAL ER fr_bytes( INT h, INT recno, UB **p_buf, SZ *p_len );
LOCAL ER fr_wri( INT h, INT recno, D off, CONST void *buf, SZ size );
LOCAL ER fr_trn( INT h, INT recno, UD size );

LOCAL ER wo_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	INT	i, nrec, cnt = 0;

	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( H_KIND(h) == H_FREE ) {
		return fr_lrc(h, buf, n, p_cnt);
	}
	nrec = nrec_of(h);
	for ( i = 0; i < nrec && cnt < n; i++ ) {
		UB	*b;
		SZ	len = 0;

		buf[cnt].recno = i;
		buf[cnt].sub = 0;
		if ( i <= OB_WR_SHAPE ) {
			buf[cnt].rt = OB_RT_TAD;
		} else if ( i < OB_WR_KIND || H_KIND(h) == H_WIN ) {
			buf[cnt].rt = OB_RT_SYSDATA;
		} else if ( is_menu(h) ) {
			buf[cnt].rt = OB_RT_TAD;
			buf[cnt].sub = item_state(H_A(h), i - OB_WR_KIND);
		} else {
			buf[cnt].rt = OB_RT_LINK;
		}
		if ( rec_bytes(h, i, &b, &len) >= E_OK && b != NULL ) {
			Kfree(b);
		}
		buf[cnt].size = (UD)len;
		cnt++;
	}
	*p_cnt = cnt;

	return E_OK;
}

LOCAL ER wo_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	UB	*b;
	SZ	len, n = 0;
	ER	er;

	(void)nowait;
	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	er = ( H_KIND(h) == H_FREE ) ? fr_bytes(h, recno, &b, &len)
				     : rec_bytes(h, recno, &b, &len);
	if ( er < E_OK ) {
		return er;
	}
	if ( off < len ) {
		n = len - (SZ)off;
		if ( n > size ) n = size;
		knl_memcpy(buf, b + off, (INT)n);
	}
	if ( b != NULL ) Kfree(b);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/* A window drawn from its drawing record, and shown */
LOCAL void repaint( INT wid )
{
	T_WMWIN		w;
	T_DPRECT	r;
	UB		*text = NULL;
	SZ		len;
	INT		gid = wm_gid(wid);

	if ( gid < 0 || wm_ref(wid, &w) < E_OK ) {
		return;
	}
	r.left = 0;
	r.top = 0;
	r.right = w.work.right - w.work.left;
	r.bottom = w.work.bottom - w.work.top;

	LOCK();
	len = WOW(wid).used ? WOW(wid).len[OB_WR_DRAW] : 0;
	if ( len > 0 ) {
		text = (UB *)Kmalloc(len);
		if ( text != NULL ) {
			knl_memcpy(text, WOW(wid).rec[OB_WR_DRAW], (INT)len);
		}
	}
	UNLOCK();
	if ( wo_paint != NULL ) {
		(void)wo_paint(gid, &r, text, ( text != NULL ) ? len : 0);
	} else {
		dp_fill_rect(gid, &r, wm_look(WM_LOOK_WORK));
	}
	if ( text != NULL ) {
		Kfree(text);
	}
	wm_damage(wid, &r);
	wm_update();
}

/*
 * A window's outline from its shape record: whatever the figure draws,
 * over the whole window. An empty record gives the rectangle back.
 */
LOCAL ER reshape( INT wid )
{
	T_WMWIN	w;
	T_DPRGN	*rgn = NULL;
	UB	*fig = NULL;
	SZ	len;
	ER	er;

	if ( wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	LOCK();
	len = WOW(wid).used ? WOW(wid).len[OB_WR_SHAPE] : 0;
	if ( len > 0 ) {
		fig = (UB *)Kmalloc(len);
		if ( fig != NULL ) {
			knl_memcpy(fig, WOW(wid).rec[OB_WR_SHAPE], (INT)len);
		}
	}
	UNLOCK();
	if ( len > 0 && fig == NULL ) {
		return E_NOMEM;
	}
	if ( fig == NULL ) {
		er = wm_set_shape(wid, NULL);
	} else if ( wo_shape == NULL ) {
		er = E_NOSPT;			/* nothing here reads a figure */
	} else {
		er = wo_shape(fig, len, w.outer.right - w.outer.left,
			      w.outer.bottom - w.outer.top, &rgn);
		if ( er >= E_OK ) {
			er = wm_set_shape(wid, rgn);
		}
		dp_rgn_free(rgn);
	}
	if ( fig != NULL ) {
		Kfree(fig);
	}
	wm_update();

	return er;
}

/* A drawing or a shape written: the bytes kept grow to hold it */
LOCAL ER keep_bytes( INT wid, INT which, D off, CONST void *buf, SZ size )
{
	WOWIN	*w = &WOW(wid);
	SZ	need = (SZ)off + size;
	UB	*nb = NULL;

	if ( off < 0 || size < 0 || need > WO_REC_MAX ) {
		return E_PAR;
	}
	LOCK();
	if ( !w->used ) {
		UNLOCK();
		return E_NOEXS;
	}
	if ( off > w->len[which] ) {
		UNLOCK();
		return E_PAR;			/* no gap is left in a record */
	}
	if ( need > w->len[which] ) {
		UNLOCK();
		nb = (UB *)Kmalloc(need);	/* not under the lock */
		if ( nb == NULL ) {
			return E_NOMEM;
		}
		LOCK();
		if ( !w->used ) {
			UNLOCK();
			Kfree(nb);
			return E_NOEXS;
		}
		if ( w->len[which] > 0 ) {
			knl_memcpy(nb, w->rec[which], (INT)w->len[which]);
		}
		if ( w->rec[which] != NULL ) {
			Kfree(w->rec[which]);
		}
		w->rec[which] = nb;
		w->len[which] = need;
	}
	knl_memcpy(w->rec[which] + off, buf, (INT)size);
	UNLOCK();

	return E_OK;
}

LOCAL BOOL panel_seen( INT pid );
LOCAL void panels_draw_on( INT wid );

LOCAL ER wo_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		 BOOL nowait )
{
	ER		er = E_NOSPT;

	(void)nowait;
	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( buf == NULL && size > 0 ) {
		return E_PAR;
	}
	if ( H_KIND(h) == H_FREE ) {
		er = fr_wri(h, recno, off, buf, size);
		if ( er >= E_OK && p_asize != NULL ) {
			*p_asize = size;
		}
		return er;
	}
	if ( H_KIND(h) == H_WIN && ( recno == OB_WR_DRAW || recno == OB_WR_SHAPE ) ) {
		er = keep_bytes(H_A(h), recno, off, buf, size);
		if ( er >= E_OK && recno == OB_WR_DRAW ) {
			repaint(H_A(h));
		}
		if ( er >= E_OK && recno == OB_WR_SHAPE ) {
			er = reshape(H_A(h));
		}
	} else if ( H_KIND(h) == H_WIN && recno == OB_WR_PLACE ) {
		T_OBWPOS	wp;
		T_DPRECT	r;

		if ( off != 0 || size < (SZ)sizeof(wp) ) {
			return E_PAR;
		}
		knl_memcpy(&wp, buf, sizeof(wp));
		r.left = wp.left;
		r.top = wp.top;
		r.right = wp.right;
		r.bottom = wp.bottom;
		er = wm_place(H_A(h), &r);	/* on the screen; what was asked kept for a larger one */
		if ( er >= E_OK && ( wp.flags & OB_WP_SHOWN ) != 0 ) {
			panels_draw_on(H_A(h));	/* what a hidden window's parts were set to, drawn once */
		}
		if ( er >= E_OK ) er = wm_show(H_A(h), ( wp.flags & OB_WP_SHOWN ) != 0);
		if ( er >= E_OK && wp.z == 0 ) er = wm_raise(H_A(h));
		wm_update();
	} else if ( H_KIND(h) == H_WIN && recno == OB_WR_DROP ) {
		T_OBDRANS	an;

		if ( off != 0 || size < (SZ)( sizeof(UINT) * 2 ) ) {
			return E_PAR;
		}
		knl_memset(&an, 0, sizeof(an));
		knl_memcpy(&an, buf, (INT)( ( size < (SZ)sizeof(an) ) ? size : (SZ)sizeof(an) ));
		an.msg[OB_DROP_MSG - 1] = 0;
		er = drop_answer(H_A(h), &an);
	} else if ( H_KIND(h) == H_WIN && recno == OB_WR_ASK ) {
		T_OBDRANS	an;

		if ( off != 0 || size < (SZ)( sizeof(UINT) * 2 ) ) {
			return E_PAR;
		}
		knl_memset(&an, 0, sizeof(an));
		knl_memcpy(&an, buf, (INT)( sizeof(UINT) * 2 ));
		er = ask_answer(H_A(h), &an);
	} else if ( H_KIND(h) == H_WIN && recno == OB_WR_CAND ) {
		T_OBWCAND	*c;

		if ( off != 0 || size < (SZ)( sizeof(INT) * 7 ) ) {
			return E_PAR;
		}
		c = (T_OBWCAND *)Kcalloc(1, sizeof(T_OBWCAND));
		if ( c == NULL ) {
			return E_NOMEM;
		}
		knl_memcpy(c, buf, (INT)( ( size < (SZ)sizeof(*c) ) ? size : (SZ)sizeof(*c) ));
		c->names[OB_CAND_BYTES - 1] = 0;
		er = cand_put(H_A(h), c);
		Kfree(c);
	} else if ( H_KIND(h) == H_WIN && recno == OB_WR_BARS ) {
		T_OBWBAR	v;
		T_WMBAR		b, was;
		INT		i, n;
		BOOL		moved = FALSE;

		n = (INT)( size / (SZ)sizeof(v) );
		if ( off != 0 || n < 1 ) {
			return E_PAR;
		}
		for ( i = 0; i < n && i < OB_BAR_MAX && i < WM_BAR_MAX; i++ ) {
			knl_memcpy(&v, (CONST UB *)buf + i * (INT)sizeof(v), sizeof(v));
			b.lo = v.lo;
			b.hi = v.hi;
			b.clo = v.clo;
			b.chi = v.chi;
			if ( wm_bar(H_A(h), (UINT)i, &was) < E_OK || was.lo != b.lo || was.hi != b.hi
			  || was.clo != b.clo || was.chi != b.chi ) {
				moved = TRUE;
			}
			er = wm_set_bar(H_A(h), (UINT)i, &b);
			if ( er < E_OK ) {
				break;
			}
		}
		/*
		 * Built only when a bar changed: a build asked for within the
		 * refresh of the one before waits for the next refresh, and a
		 * program that writes its bars after each drawing would wait
		 * that long for nothing.
		 */
		if ( moved ) {
			wm_update();
		}
	} else if ( H_KIND(h) == H_PART && ( recno == OB_WR_STATE || recno == OB_WR_PLACE
					      || recno == OB_WR_KIND ) ) {
		if ( off != 0 ) {
			return E_PAR;
		}
		if ( recno == OB_WR_STATE ) {
			er = knl_pn_set_state(H_A(h), H_B(h), (CONST UB *)buf, (INT)size);
		} else if ( recno == OB_WR_KIND ) {
			er = knl_pn_set_names(H_A(h), H_B(h), (CONST UB *)buf, (INT)size);
			if ( er >= E_OK ) {
				er = 1;		/* the names are not compared: drawn again */
			}
		} else if ( size < (SZ)sizeof(T_OBWPOS) ) {
			er = E_PAR;
		} else {
			T_OBWPOS	wp;

			knl_memcpy(&wp, buf, sizeof(wp));
			er = knl_pn_set_flags(H_A(h), H_B(h), wp.flags);
		}
		/*
		 * A part written as it already was (the state and the flags
		 * say: knl_pn_set_state, knl_pn_set_flags) leaves the panel as
		 * it is: nothing is drawn or built for it.
		 */
		if ( er > E_OK && panel_seen(H_A(h)) ) {
			/*
			 * The panel drawn again and the screen asked to show it,
			 * without the writer waiting for it to be built: a program
			 * that changes a part and then draws its window has both
			 * shown in one build, not one after the other. On a window
			 * still hidden it is drawn once, as the window is shown.
			 * The panel is drawn whole, its ground first, into the
			 * pixels the compositor builds the screen from: what was
			 * asked before is built first (wm_flush, which returns at
			 * once when nothing waits), so that a build never reads the
			 * panel with its ground laid and its parts not yet on it,
			 * as it would while a program writes its parts one after
			 * another.
			 */
			(void)wm_flush();
			wm_panel_draw(H_A(h));
			wm_post();
		}
		if ( er > E_OK ) {
			er = E_OK;
		}
	}
	if ( er >= E_OK && p_asize != NULL ) {
		*p_asize = size;
	}
	return er;
}

/* The window panel pid is placed on can be seen: what is written of its parts is drawn at once */
LOCAL BOOL panel_seen( INT pid )
{
	T_WMWIN	w;
	INT	wid = wm_panel_wid(pid);

	return (BOOL)( wid <= 0 || wm_ref(wid, &w) < E_OK || w.visible );
}

/* The panels placed on window wid drawn, as it comes out of hiding */
LOCAL void panels_draw_on( INT wid )
{
	T_WMWIN	w;
	INT	pids[WM_PANEL_MAX], n = 0, i;

	if ( wm_ref(wid, &w) < E_OK || w.visible ) {
		return;				/* drawn already, as each part was set */
	}
	LOCK();
	for ( i = 1; i <= WM_PANEL_MAX; i++ ) {
		if ( wo_pnl[i].used ) {
			pids[n++] = i;
		}
	}
	UNLOCK();
	for ( i = 0; i < n; i++ ) {
		if ( wm_panel_wid(pids[i]) == wid ) {
			wm_panel_draw(pids[i]);
		}
	}
}

LOCAL ER wo_trn( INT h, INT recno, UD size )
{
	BOOL	redraw = FALSE;

	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( H_KIND(h) == H_FREE ) {
		return fr_trn(h, recno, size);
	}
	if ( H_KIND(h) == H_WIN && ( recno == OB_WR_DRAW || recno == OB_WR_SHAPE ) ) {
		LOCK();
		if ( (UD)WOW(H_A(h)).len[recno] > size ) {
			WOW(H_A(h)).len[recno] = (SZ)size;
			redraw = ( recno == OB_WR_DRAW );
		}
		UNLOCK();
		if ( redraw ) {
			repaint(H_A(h));
		}
		return ( recno == OB_WR_SHAPE ) ? reshape(H_A(h)) : E_OK;
	}
	/* the place, the state, a drop and the bars are rewritten whole: nothing to cut */
	if ( recno == OB_WR_PLACE || recno == OB_WR_STATE
	  || ( H_KIND(h) == H_WIN && ( recno == OB_WR_DROP || recno == OB_WR_BARS || recno == OB_WR_CAND
				      || recno == OB_WR_ASK ) ) ) {
		return E_OK;
	}
	return E_NOSPT;
}


/* ---------------------------------------------------------------- objects made on their own */

LOCAL WOFREE *fr_of( INT h )
{
	return &wo_free[H_A(h)];
}

/* The handle of what it is placed as, 0 while it is not placed */
LOCAL INT fr_placed( INT h )
{
	WOFREE	*e = fr_of(h);
	INT	r = 0;

	LOCK();
	if ( e->used && e->pid > 0 ) {
		r = ( e->sub == OB_S_PART ) ? H_MAKE(H_PART, e->pid, e->idx)
					    : H_MAKE(H_PNL, e->pid, 0);
	}
	UNLOCK();

	return r;
}

/* What a placed part holds now, kept for when it is taken off */
LOCAL void fr_keep_state( INT fi )
{
	WOFREE		*e = &wo_free[fi];
	T_WMPART	*pt;
	INT		pid, idx;

	LOCK();
	pid = e->pid;
	idx = e->idx;
	UNLOCK();
	if ( pid <= 0 || ( pt = part_copy(pid, idx) ) == NULL ) {
		return;
	}
	LOCK();
	if ( e->used ) {
		e->pt.value = pt->value;
		knl_memcpy(e->pt.text, pt->text, WM_LABEL_MAX);
	}
	UNLOCK();
	Kfree(pt);
}

LOCAL INT fr_nrec( INT h )
{
	WOFREE	*e = fr_of(h);
	INT	n = WO_NREC, pl = fr_placed(h);

	LOCK();
	if ( e->sub == OB_S_PART ) {
		n += e->nlook;
	} else if ( e->sub == OB_S_MENU ) {
		n += e->nitem;
	}
	UNLOCK();
	if ( e->sub == OB_S_PANEL && pl > 0 ) {
		n = nrec_of(pl);			/* its links are the panel's */
	}
	return n;
}

/* Bytes kept here copied out, in memory the caller frees */
LOCAL ER fr_copy( CONST UB *src, SZ len, UB **p_buf, SZ *p_len )
{
	UB	*b = NULL;

	if ( len > 0 ) {
		b = (UB *)Kmalloc(len);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		knl_memcpy(b, src, (INT)len);
	}
	*p_buf = b;
	*p_len = len;
	return E_OK;
}

LOCAL ER fr_bytes( INT h, INT recno, UB **p_buf, SZ *p_len )
{
	WOFREE	*e = fr_of(h);
	INT	pl = fr_placed(h);
	ER	er = E_OK;

	*p_buf = NULL;
	*p_len = 0;
	if ( recno < 0 || recno >= fr_nrec(h) ) {
		return E_NOEXS;
	}
	if ( pl > 0 && ( recno == OB_WR_PLACE || recno == OB_WR_STATE
		      || ( e->sub == OB_S_PANEL && recno >= OB_WR_KIND ) ) ) {
		return rec_bytes(pl, recno, p_buf, p_len);
	}
	LOCK();
	if ( !e->used ) {
		er = E_NOEXS;
	} else if ( recno == OB_WR_DRAW ) {
		er = fr_copy(e->draw, e->drawlen, p_buf, p_len);
	} else if ( recno == OB_WR_SHAPE ) {
		er = fr_copy(e->shape, e->shapelen, p_buf, p_len);
	} else if ( recno == OB_WR_PLACE ) {
		T_OBWPOS	wp;

		knl_memset(&wp, 0, sizeof(wp));
		wp.left = e->pt.r.left;
		wp.top = e->pt.r.top;
		wp.right = e->pt.r.right;
		wp.bottom = e->pt.r.bottom;
		er = fr_copy((CONST UB *)&wp, sizeof(wp), p_buf, p_len);
	} else if ( recno == OB_WR_STATE ) {
		if ( e->sub != OB_S_PART ) {
			er = E_OK;
		} else if ( part_is_text(&e->pt) ) {
			er = fr_copy(e->pt.text, str_len(e->pt.text, WM_LABEL_MAX), p_buf, p_len);
		} else {
			er = fr_copy((CONST UB *)&e->pt.value, sizeof(INT), p_buf, p_len);
		}
	} else if ( e->sub == OB_S_PART ) {
		INT	k = recno - OB_WR_KIND;

		er = fr_copy(e->look[k], e->looklen[k], p_buf, p_len);
	} else if ( e->sub == OB_S_MENU ) {
		CONST UB *l = e->item[recno - OB_WR_KIND].label;

		er = fr_copy(l, str_len(l, WM_LABEL_MAX), p_buf, p_len);
	} else {
		er = E_NOEXS;
	}
	UNLOCK();

	return er;
}

LOCAL ER fr_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	WOFREE	*e = fr_of(h);
	INT	i, nrec = fr_nrec(h), cnt = 0;

	for ( i = 0; i < nrec && cnt < n; i++ ) {
		UB	*b = NULL;
		SZ	len = 0;

		buf[cnt].recno = i;
		buf[cnt].sub = 0;
		if ( i <= OB_WR_SHAPE ) {
			buf[cnt].rt = OB_RT_TAD;
		} else if ( i < OB_WR_KIND ) {
			buf[cnt].rt = OB_RT_SYSDATA;
		} else if ( e->sub == OB_S_PANEL ) {
			buf[cnt].rt = OB_RT_LINK;
		} else {
			buf[cnt].rt = OB_RT_TAD;
			if ( e->sub == OB_S_MENU ) {
				LOCK();
				buf[cnt].sub = e->item[i - OB_WR_KIND].state;
				UNLOCK();
			}
		}
		if ( fr_bytes(h, i, &b, &len) >= E_OK && b != NULL ) {
			Kfree(b);
		}
		buf[cnt].size = (UD)len;
		cnt++;
	}
	*p_cnt = cnt;

	return E_OK;
}

/*
 * Bytes written into one of the blocks an object keeps here: a look or
 * the shape. They grow to hold what is written; the allocation is made
 * with the lock let go, and the entry looked at again after.
 */
LOCAL ER fr_keep( INT fi, INT which, D off, CONST void *buf, SZ size )
{
	WOFREE	*e = &wo_free[fi];
	UB	**pp;
	SZ	*pl, need = (SZ)off + size;
	UB	*nb = NULL;

	if ( off < 0 || size < 0 || need > WO_REC_MAX ) {
		return E_PAR;
	}
	pp = ( which < 2 ) ? &e->look[which] : ( which == 2 ) ? &e->shape : &e->draw;
	pl = ( which < 2 ) ? &e->looklen[which] : ( which == 2 ) ? &e->shapelen : &e->drawlen;
	LOCK();
	if ( !e->used || off > *pl ) {
		UNLOCK();
		return e->used ? E_PAR : E_NOEXS;
	}
	if ( need > *pl ) {
		UNLOCK();
		nb = (UB *)Kmalloc(need);
		if ( nb == NULL ) {
			return E_NOMEM;
		}
		LOCK();
		if ( !e->used ) {
			UNLOCK();
			Kfree(nb);
			return E_NOEXS;
		}
		if ( *pl > 0 ) {
			knl_memcpy(nb, *pp, (INT)*pl);
		}
		if ( *pp != NULL ) {
			Kfree(*pp);
		}
		*pp = nb;
		*pl = need;
	}
	knl_memcpy(*pp + off, buf, (INT)size);
	UNLOCK();

	return E_OK;
}

/* A placed part drawn again, when how it looks or where it is changed */
LOCAL void fr_redraw( INT h )
{
	WOFREE	*e = fr_of(h);
	INT	pid;

	LOCK();
	pid = e->pid;
	UNLOCK();
	if ( pid > 0 ) {
		wm_panel_draw(pid);
		wm_update();
	}
}

LOCAL ER fr_wri( INT h, INT recno, D off, CONST void *buf, SZ size )
{
	WOFREE	*e = fr_of(h);
	INT	fi = H_A(h), pl = fr_placed(h);
	ER	er = E_NOSPT;

	if ( recno == OB_WR_SHAPE ) {
		return fr_keep(fi, 2, off, buf, size);
	}
	if ( recno == OB_WR_DRAW ) {
		return fr_keep(fi, 3, off, buf, size);	/* kept; a part draws itself */
	}
	if ( recno == OB_WR_PLACE ) {
		T_OBWPOS	wp;
		T_DPRECT	r;

		if ( off != 0 || size < (SZ)sizeof(wp) ) {
			return E_PAR;
		}
		knl_memcpy(&wp, buf, sizeof(wp));
		r.left = wp.left;
		r.top = wp.top;
		r.right = wp.right;
		r.bottom = wp.bottom;
		if ( r.right <= r.left || r.bottom <= r.top ) {
			return E_PAR;
		}
		if ( pl > 0 && e->sub != OB_S_PART ) {
			return E_NOSPT;			/* a placed panel stays where it was put */
		}
		if ( pl > 0 ) {
			er = knl_pn_set_rect(H_A(pl), H_B(pl), &r);
			if ( er < E_OK ) {
				return er;
			}
		}
		LOCK();
		e->pt.r = r;
		UNLOCK();
		fr_redraw(h);
		return E_OK;
	}
	if ( recno == OB_WR_STATE ) {
		if ( e->sub != OB_S_PART ) {
			return E_NOSPT;
		}
		if ( pl > 0 ) {
			return wo_wri(pl, recno, off, buf, size, NULL, FALSE);
		}
		if ( off != 0 ) {
			return E_PAR;
		}
		LOCK();
		if ( part_is_text(&e->pt) ) {
			SZ	n = ( size < WM_LABEL_MAX - 1 ) ? size : WM_LABEL_MAX - 1;

			knl_memcpy(e->pt.text, buf, (INT)n);
			e->pt.text[n] = 0;
			er = E_OK;
		} else if ( size >= (SZ)sizeof(INT) ) {
			knl_memcpy(&e->pt.value, buf, sizeof(INT));
			er = E_OK;
		} else {
			er = E_PAR;
		}
		UNLOCK();
		return er;
	}
	if ( recno >= OB_WR_KIND && recno < fr_nrec(h) ) {
		if ( e->sub == OB_S_PART ) {
			er = fr_keep(fi, recno - OB_WR_KIND, off, buf, size);
			if ( er >= E_OK ) {
				fr_redraw(h);
			}
		} else if ( e->sub == OB_S_MENU ) {
			SZ	n = ( size < WM_LABEL_MAX - 1 ) ? size : WM_LABEL_MAX - 1;

			if ( off != 0 ) {
				return E_PAR;
			}
			LOCK();
			knl_memcpy(e->item[recno - OB_WR_KIND].label, buf, (INT)n);
			e->item[recno - OB_WR_KIND].label[n] = 0;
			UNLOCK();
			er = E_OK;
		}
	}
	return er;
}

LOCAL ER fr_apd( INT h, UINT rt, UINT sub, INT *p_recno )
{
	WOFREE	*e = fr_of(h);
	ER	er = E_NOSPT;

	(void)rt;
	LOCK();
	if ( e->sub == OB_S_PART ) {
		if ( e->nlook < 2 ) {
			*p_recno = OB_WR_KIND + e->nlook++;
			er = E_OK;
		} else {
			er = E_LIMIT;			/* a switch looks two ways */
		}
	} else if ( e->sub == OB_S_MENU && e->pid == 0 ) {
		if ( e->nitem == e->icap ) {
			/* as many items as are added: twice the room when it is full */
			INT	cap = ( e->icap > 0 ) ? e->icap * 2 : 16;
			WOITEM	*t = (WOITEM *)Kmalloc(sizeof(WOITEM) * (SZ)cap);

			if ( t != NULL ) {
				if ( e->item != NULL ) {
					knl_memcpy(t, e->item, sizeof(WOITEM) * (SZ)e->nitem);
					Kfree(e->item);
				}
				e->item = t;
				e->icap = cap;
			}
		}
		if ( e->nitem < e->icap ) {
			knl_memset(&e->item[e->nitem], 0, sizeof(WOITEM));
			e->item[e->nitem].state = sub;
			*p_recno = OB_WR_KIND + e->nitem++;
			er = E_OK;
		} else {
			er = E_NOMEM;
		}
	}
	UNLOCK();

	return er;
}

LOCAL ER fr_trn( INT h, INT recno, UD size )
{
	WOFREE	*e = fr_of(h);
	SZ	*pl = NULL;

	if ( recno == OB_WR_PLACE || recno == OB_WR_STATE ) {
		return E_OK;
	}
	LOCK();
	if ( recno == OB_WR_DRAW ) {
		pl = &e->drawlen;
	} else if ( recno == OB_WR_SHAPE ) {
		pl = &e->shapelen;
	} else if ( e->sub == OB_S_PART && recno - OB_WR_KIND < e->nlook ) {
		pl = &e->looklen[recno - OB_WR_KIND];
	} else if ( e->sub == OB_S_MENU && recno - OB_WR_KIND < e->nitem ) {
		if ( size < WM_LABEL_MAX ) {
			e->item[recno - OB_WR_KIND].label[size] = 0;
		}
		UNLOCK();
		return E_OK;
	}
	if ( pl != NULL && (UD)*pl > size ) {
		*pl = (SZ)size;
	}
	UNLOCK();
	if ( pl == NULL ) {
		return E_NOEXS;
	}
	fr_redraw(h);
	return E_OK;
}

/* A number for a part put on a panel: one past the largest there */
LOCAL INT next_num( INT pid )
{
	T_WMPART	*pt;
	INT		i, n = knl_pn_count(pid, NULL), top = 0;

	for ( i = 0; i < n; i++ ) {
		pt = part_copy(pid, i);
		if ( pt != NULL ) {
			if ( pt->num > top ) top = pt->num;
			Kfree(pt);
		}
	}
	return top + 1;
}

/*
 * Placing it: a part on a panel at (x, y) in the panel, a panel or a
 * menu on a window at (x, y) in its work area. The window layer takes
 * it from there under the same UUID.
 */
LOCAL ER fr_map( INT h, INT target, CONST T_OBMAP *m )
{
	WOFREE		*e = fr_of(h);
	T_WMPART	*pt = NULL;
	INT		t = target, pid, i, w, hgt;
	ER		er = E_OK;

	if ( H_KIND(t) == H_FREE ) {
		t = fr_placed(t);			/* onto a panel that was itself placed */
	}
	if ( t <= 0 || !alive(t) ) {
		return E_NOEXS;
	}
	if ( fr_placed(h) > 0 ) {
		return E_OBJ;				/* take it off first */
	}
	if ( e->sub == OB_S_PART ) {
		if ( H_KIND(t) != H_PNL || is_menu(t) ) {
			return E_PAR;
		}
		pid = H_A(t);
		pt = (T_WMPART *)Kmalloc(sizeof(T_WMPART));
		if ( pt == NULL ) {
			return E_NOMEM;
		}
		LOCK();
		*pt = e->pt;
		UNLOCK();
		w = pt->r.right - pt->r.left;
		hgt = pt->r.bottom - pt->r.top;
		pt->r.left = m->x;
		pt->r.top = m->y;
		pt->r.right = m->x + w;
		pt->r.bottom = m->y + hgt;
		pt->num = next_num(pid);
		i = knl_pn_add(pid, pt);
		Kfree(pt);
		if ( i < 0 ) {
			return (ER)i;
		}
		LOCK();
		wo_pnl[pid].part[i] = e->uuid;
		if ( i >= wo_pnl[pid].npart ) {
			wo_pnl[pid].npart = i + 1;
		}
		e->pid = pid;
		e->idx = i;
		UNLOCK();
		wm_panel_draw(pid);
		wm_update();
		return E_OK;
	}

	if ( H_KIND(t) != H_WIN ) {
		return E_PAR;				/* a panel or a menu goes on a window */
	}
	if ( e->sub == OB_S_PANEL ) {
		T_WMPANEL	*def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));

		if ( def == NULL ) {
			return E_NOMEM;
		}
		knl_memset(def, 0, sizeof(*def));
		LOCK();
		w = e->pt.r.right - e->pt.r.left;
		hgt = e->pt.r.bottom - e->pt.r.top;
		UNLOCK();
		if ( e->boxnum > 0 ) {
			TS_UUID	box = e->box;

			if ( knl_pn_load(&box, e->boxnum, def, NULL, 0) < E_OK ) {
				Kfree(def);
				return E_OBJ;
			}
			w = def->r.right - def->r.left;
			hgt = def->r.bottom - def->r.top;
		}
		def->r.left = m->x;
		def->r.top = m->y;
		def->r.right = m->x + w;
		def->r.bottom = m->y + hgt;
		/* a panel made on its own may say what it is for: "kind":3 the ground alone (WM_PNL_BARE) */
		LOCK();
		if ( e->boxnum == 0 && e->pt.type <= WM_PNL_BARE ) {
			def->kind = e->pt.type;
		}
		UNLOCK();
		pid = wm_panel_open(H_A(t), def);
		Kfree(def);
		if ( pid > 0 ) {
			BOOL	bs;

			LOCK();
			bs = e->beside;
			UNLOCK();
			if ( bs ) knl_pn_set_beside(pid);	/* ui_strip's: its boxes wait for a press */
		}
	} else {
		T_WMMENU	*md;

		LOCK();
		md = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(e->nitem));
		if ( md == NULL ) {
			UNLOCK();
			return E_NOMEM;
		}
		md->nitem = e->nitem;
		for ( i = 0; i < e->nitem; i++ ) {
			WOITEM	*it = &e->item[i];

			if ( ( it->state & OB_MI_LINE ) == 0 ) {
				md->item[i].cmd = i + 1;
				knl_memcpy(md->item[i].label, it->label, WM_LABEL_MAX);
			}
			md->item[i].grey = ( it->state & OB_MI_GREY ) != 0;
			md->item[i].tick = ( it->state & OB_MI_TICK ) != 0;
		}
		UNLOCK();
		pid = wm_menu_open(H_A(t), md, m->x, m->y);
		Kfree(md);
	}
	if ( pid < 0 ) {
		return (ER)pid;
	}
	LOCK();
	wo_pnl[pid].uuid = e->uuid;
	wo_pnl[pid].prt = e->prt;
	e->pid = pid;
	UNLOCK();
	if ( e->sub == OB_S_MENU ) {
		INT	pop = wm_panel_wid(pid);

		LOCK();
		if ( pop >= 1 && pop <= wo_nwin && WOW(pop).used ) {
			WOW(pop).drive = TRUE;	/* its list is worked from here */
		}
		UNLOCK();
	}
	if ( e->sub == OB_S_MENU || panel_seen(pid) ) {
		wm_panel_draw(pid);	/* a panel on a window still hidden: drawn once as it is shown */
		wm_update();
	}

	return er;
}

/* Taking it off what it was placed on; it keeps what it was */
LOCAL ER fr_unm( INT h, INT target )
{
	WOFREE	*e = fr_of(h);
	INT	pl = fr_placed(h), pid, idx, i, k;
	ER	er;

	(void)target;
	if ( pl <= 0 ) {
		return E_OBJ;				/* it is not placed */
	}
	if ( e->sub != OB_S_PART ) {
		er = wm_panel_close(H_A(pl));		/* the close sees it is ours */
		wm_update();
		return er;
	}
	fr_keep_state(H_A(h));
	pid = H_A(pl);
	idx = H_B(pl);
	er = knl_pn_del(pid, idx);
	if ( er < E_OK ) {
		return er;
	}
	LOCK();
	for ( k = idx; k + 1 < wo_pnl[pid].npart; k++ ) {
		wo_pnl[pid].part[k] = wo_pnl[pid].part[k + 1];
	}
	if ( wo_pnl[pid].npart > 0 ) {
		wo_pnl[pid].npart--;
	}
	for ( i = 0; i < WO_FREE_MAX; i++ ) {
		if ( wo_free[i].used && wo_free[i].pid == pid && wo_free[i].sub == OB_S_PART
		  && wo_free[i].idx > idx ) {
			wo_free[i].idx--;
		}
	}
	e->pid = 0;
	UNLOCK();
	wm_panel_draw(pid);
	wm_update();

	return E_OK;
}

EXPORT BOOL knl_wmobj_part_look( INT pid, INT i, INT gid, CONST T_DPRECT *r, BOOL on )
{
	UB	*b = NULL;
	SZ	len = 0;
	INT	k;

	if ( wo_nfree == 0 || wo_paint == NULL ) {
		return FALSE;
	}
	LOCK();
	for ( k = 0; k < WO_FREE_MAX; k++ ) {
		WOFREE	*e = &wo_free[k];
		INT	which;

		if ( !e->used || e->sub != OB_S_PART || e->pid != pid || e->idx != i
		  || e->nlook == 0 ) {
			continue;
		}
		which = ( on && e->nlook > 1 ) ? 1 : 0;
		if ( e->looklen[which] > 0 ) {
			(void)fr_copy(e->look[which], e->looklen[which], &b, &len);
		}
		break;
	}
	UNLOCK();
	if ( b == NULL ) {
		return FALSE;
	}
	(void)wo_paint(gid, r, b, len);
	Kfree(b);

	return TRUE;
}

/* A rectangle from a JSON array [l, t, r, b] */
LOCAL BOOL json_rect( CONST UB *j, INT len, INT v, T_DPRECT *r )
{
	INT	pos = v, e, k = 0;
	D	val[4];

	while ( v >= 0 && k < 4 && ( e = knl_oj_next(j, len, v, &pos) ) >= 0 ) {
		if ( !knl_oj_num(j, len, e, &val[k]) ) break;
		k++;
	}
	if ( k != 4 || val[2] <= val[0] || val[3] <= val[1] ) {
		return FALSE;
	}
	r->left = (INT)val[0];
	r->top = (INT)val[1];
	r->right = (INT)val[2];
	r->bottom = (INT)val[3];
	return TRUE;
}

/*
 * A panel, a menu or a part made on its own. What it is comes from the
 * metadata, as a definition carries it:
 *   {"name":"OK","tessronos":{"part":{"kind":6,"rect":[0,0,80,24]}}}
 * and a plain {"rect":[...]} does as well. The name is its label.
 */
LOCAL ER fr_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	WOFREE	*e = NULL;
	WOITEM	*items = NULL;
	TS_UUID	u, box;
	T_DPRECT r;
	UINT	kind = MS_PARTS;
	INT	i, n, boxnum = 0;
	BOOL	beside = FALSE;
	UB	title[WM_LABEL_MAX];

	title[0] = 0;

	if ( c->sub == OB_S_PART ) {
		r.left = 0;  r.top = 0;  r.right = 80;  r.bottom = 24;
	} else {
		r.left = 0;  r.top = 0;  r.right = 200;  r.bottom = 100;
	}
	if ( c->json != NULL && c->jsonsz > 0 ) {
		CONST UB	*j = c->json;
		INT		len = (INT)c->jsonsz;
		INT		root = knl_oj_root(j, len);
		INT		pdef = knl_oj_path(j, len, "tessronos", "part");
		INT		v;
		D		d;

		if ( pdef >= 0 && ( v = knl_oj_member(j, len, pdef, "kind") ) >= 0
		  && knl_oj_num(j, len, v, &d) ) {
			kind = (UINT)d;
		}
		if ( pdef >= 0 && ( v = knl_oj_member(j, len, pdef, "beside") ) >= 0
		  && knl_oj_num(j, len, v, &d) && d != 0 ) {
			beside = TRUE;
		}
		if ( !( pdef >= 0 && ( v = knl_oj_member(j, len, pdef, "rect") ) >= 0
		     && json_rect(j, len, v, &r) ) && root >= 0 ) {
			v = knl_oj_member(j, len, root, "rect");
			(void)json_rect(j, len, v, &r);
		}
	}
	/* a panel of a data box: its size and its name from the definition */
	if ( c->sub == OB_S_PANEL && c->json != NULL && c->jsonsz > 0 ) {
		INT	pdef = knl_oj_path(c->json, (INT)c->jsonsz, "tessronos", "part");
		INT	v = ( pdef >= 0 ) ? knl_oj_member(c->json, (INT)c->jsonsz, pdef, "box") : -1;
		UB	us[40];
		D	d;

		if ( v >= 0 && knl_oj_str(c->json, (INT)c->jsonsz, v, us, sizeof(us)) > 0
		  && ts_str_to_uuid((CONST char *)us, &box) >= E_OK ) {
			T_WMPANEL	*def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));

			v = knl_oj_member(c->json, (INT)c->jsonsz, pdef, "num");
			boxnum = ( v >= 0 && knl_oj_num(c->json, (INT)c->jsonsz, v, &d) ) ? (INT)d : 1;
			if ( def == NULL ) {
				return E_NOMEM;
			}
			if ( knl_pn_load(&box, boxnum, def, title, sizeof(title)) < E_OK ) {
				Kfree(def);
				return E_OBJ;
			}
			r.left = 0;
			r.top = 0;
			r.right = def->r.right - def->r.left;
			r.bottom = def->r.bottom - def->r.top;
			Kfree(def);
		}
	}
	if ( ts_gen_uuid(&u) < E_OK ) {
		if ( items != NULL ) Kfree(items);
		return E_SYS;
	}
	LOCK();
	for ( i = 0; i < WO_FREE_MAX; i++ ) {
		if ( !wo_free[i].used ) {
			e = &wo_free[i];
			break;
		}
	}
	if ( e != NULL ) {
		knl_memset(e, 0, sizeof(*e));
		e->uuid = u;
		e->sub = c->sub;
		e->prt.owner = prt->owner;
		e->prt.group = prt->group;
		e->prt.mode = prt->mode & 0777;
		e->pt.type = kind;
		e->pt.r = r;
		e->pt.answer = WM_ANS_NONE;
		for ( n = 0; c->name != NULL && c->name[n] != 0 && n < WM_LABEL_MAX - 1; n++ ) {
			e->pt.label[n] = c->name[n];
		}
		for ( n = 0; boxnum > 0 && title[n] != 0 && ( c->name == NULL || c->name[0] == 0 ); n++ ) {
			e->pt.label[n] = title[n];
		}
		e->boxnum = boxnum;
		e->beside = beside;
		if ( boxnum > 0 ) {
			e->box = box;
		}
		e->item = items;
		e->used = TRUE;
		wo_nfree++;
	}
	UNLOCK();
	if ( e == NULL ) {
		if ( items != NULL ) Kfree(items);
		return E_LIMIT;
	}
	*p_uuid = u;

	return E_OK;
}

/* One made on its own gone: taken off first, then forgotten */
LOCAL ER fr_remove( INT h )
{
	WOFREE	*e = fr_of(h);
	TS_UUID	u;
	UB	*l0, *l1, *sh, *dr;
	WOITEM	*it;

	if ( fr_placed(h) > 0 ) {
		(void)fr_unm(h, 0);
	}
	LOCK();
	if ( !e->used ) {
		UNLOCK();
		return E_NOEXS;
	}
	u = e->uuid;
	l0 = e->look[0];
	l1 = e->look[1];
	sh = e->shape;
	dr = e->draw;
	it = e->item;
	e->used = FALSE;
	wo_nfree--;
	UNLOCK();
	if ( l0 != NULL ) Kfree(l0);
	if ( l1 != NULL ) Kfree(l1);
	if ( sh != NULL ) Kfree(sh);
	if ( dr != NULL ) Kfree(dr);
	if ( it != NULL ) Kfree(it);
	knl_ob_post(&u, -1, OB_E_DELETE, NULL);

	return E_OK;
}

/* ---------------------------------------------------------------- attributes */

LOCAL CONST char *sub_name( UINT sub )
{
	switch ( sub ) {
	case OB_S_WINDOW:	return "window";
	case OB_S_PANEL:	return "panel";
	case OB_S_MENU:		return "menu";
	default:		return "part";
	}
}

/* The name a thing shows: a window's title, a part's label */
LOCAL void name_of( INT h, UB *out, INT max )
{
	out[0] = 0;
	if ( H_KIND(h) == H_WIN ) {
		(void)knl_wm_title(H_A(h), (char *)out, max);
	} else if ( H_KIND(h) == H_FREE ) {
		INT	n;

		LOCK();
		n = str_len(wo_free[H_A(h)].pt.label, max - 1);
		knl_memcpy(out, wo_free[H_A(h)].pt.label, n);
		out[n] = 0;
		UNLOCK();
	} else if ( H_KIND(h) == H_PART ) {
		T_WMPART	*pt = part_copy(H_A(h), H_B(h));
		INT		n;

		if ( pt != NULL ) {
			n = str_len(pt->label, max - 1);
			knl_memcpy(out, pt->label, n);
			out[n] = 0;
			Kfree(pt);
		}
	}
}

LOCAL ER wo_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	UB	name[WM_LABEL_MAX];
	UB	*j;
	INT	n, max = WM_LABEL_MAX * 2 + 512;	/* a name and a few numbers: no more */

	if ( !alive(h) ) {
		return E_NOEXS;
	}
	j = (UB *)Kmalloc(max);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	name_of(h, name, sizeof(name));
	n = knl_oj_put(j, 0, max, "{\"name\":");
	n = knl_oj_put_str(j, n, max, name);
	n = knl_oj_put(j, n, max, ",\"tessronos\":{\"window\":{\"sub\":\"");
	n = knl_oj_put(j, n, max, sub_name(sub_of(h)));
	n = knl_oj_put(j, n, max, "\",\"number\":");
	/* one made on its own has a number only while it is placed */
	n = knl_oj_put_num(j, n, max, (D)( ( H_KIND(h) != H_FREE ) ? H_A(h)
					 : ( fr_placed(h) > 0 ) ? H_A(fr_placed(h)) : 0 ));
	if ( H_KIND(h) == H_WIN ) {
		TS_UUID	sh;

		LOCK();
		sh = WOW(H_A(h)).shows;
		UNLOCK();
		if ( !knl_ob_uuid_zero(&sh) ) {
			n = knl_oj_put(j, n, max, ",\"shows\":");
			n = knl_oj_put_uuid(j, n, max, &sh);
		}
	}
	if ( H_KIND(h) == H_FREE ) {
		INT	pl = fr_placed(h);

		n = knl_oj_put(j, n, max, ",\"placed\":");
		n = knl_oj_put(j, n, max, ( pl > 0 ) ? "true" : "false");
		if ( wo_free[H_A(h)].sub == OB_S_PART ) {
			n = knl_oj_put(j, n, max, ",\"kind\":");
			n = knl_oj_put_num(j, n, max, (D)WM_PT_KIND(wo_free[H_A(h)].pt.type));
		}
	}
	if ( H_KIND(h) == H_PART ) {
		T_WMPART	*pt = part_copy(H_A(h), H_B(h));

		if ( pt != NULL ) {
			n = knl_oj_put(j, n, max, ",\"part\":");
			n = knl_oj_put_num(j, n, max, (D)pt->num);
			n = knl_oj_put(j, n, max, ",\"kind\":");
			n = knl_oj_put_num(j, n, max, (D)WM_PT_KIND(pt->type));
			Kfree(pt);
		}
	}
	n = knl_oj_put(j, n, max, "}}}");
	if ( n < 0 ) {
		Kfree(j);
		return E_LIMIT;
	}
	if ( n > size ) n = (INT)size;
	knl_memcpy(json, j, n);
	Kfree(j);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/* A window's title comes from the name written; nothing else is kept */
LOCAL ER wo_sat( INT h, CONST UB *json, SZ size )
{
	UB	name[WM_TITLE_MAX];
	INT	root, v;

	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( H_KIND(h) != H_WIN && H_KIND(h) != H_FREE ) {
		return E_OK;
	}
	root = knl_oj_root(json, (INT)size);
	v = ( root >= 0 ) ? knl_oj_member(json, (INT)size, root, "name") : -1;
	if ( v >= 0 && knl_oj_str(json, (INT)size, v, name, sizeof(name)) >= 0 ) {
		if ( H_KIND(h) == H_FREE ) {
			LOCK();
			knl_memcpy(wo_free[H_A(h)].pt.label, name, sizeof(name));
			wo_free[H_A(h)].pt.label[sizeof(name) - 1] = 0;
			UNLOCK();
			fr_redraw(h);
			return E_OK;
		}
		wm_set_title(H_A(h), (CONST char *)name);
		wm_update();		/* the band, drawn through the frames' watched environment */
	}
	return E_OK;
}

/* ---------------------------------------------------------------- objects */

LOCAL ER wo_find( CONST TS_UUID *uuid )
{
	INT	h = locate(uuid);

	return ( h > 0 ) ? E_OK : (ER)h;
}

LOCAL INT wo_open( CONST TS_UUID *uuid, UINT ops )
{
	(void)ops;

	return locate(uuid);
}

LOCAL ER wo_close( INT h )
{
	(void)h;

	return E_OK;
}

LOCAL ER wo_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_OBREC	*recs;
	INT	h = locate(uuid), cnt = 0, i;

	if ( h < 0 ) {
		return (ER)h;
	}
	r->type = OB_T_WINDOW;
	r->sub = sub_of(h);
	r->flags = OB_F_VOLATILE;
	r->refcnt = 0;
	name_of(h, r->name, OB_NAME_MAX);
	recs = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * ( WO_NREC + WM_PART_MAX ));
	if ( recs != NULL && wo_lrc(h, recs, WO_NREC + WM_PART_MAX, &cnt) >= E_OK ) {
		r->nrec = cnt;
		r->size = 0;
		for ( i = 0; i < cnt; i++ ) {
			r->size += recs[i].size;
		}
	}
	if ( recs != NULL ) {
		Kfree(recs);
	}
	return E_OK;
}

LOCAL ER wo_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	INT	h = locate(uuid);
	T_OBPRT	*prt;

	if ( h < 0 ) {
		return (ER)h;
	}
	prt = knl_ob_prt_new(0, 0);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	*p_prt = prt;
	LOCK();
	prt->owner = prt_slot(h)->owner;
	prt->group = prt_slot(h)->group;
	prt->mode = prt_slot(h)->mode;
	UNLOCK();

	return E_OK;
}

/* The owner, the group and the mode are kept; a window has no ACL */
LOCAL ER wo_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	INT	h = locate(uuid);

	if ( h < 0 ) {
		return (ER)h;
	}
	if ( prt->nacl > 0 || prt->nrmask > 0 ) {
		return E_NOSPT;
	}
	LOCK();
	prt_slot(h)->owner = prt->owner;
	prt_slot(h)->group = prt->group;
	prt_slot(h)->mode = prt->mode & 0777;
	UNLOCK();

	return E_OK;
}

/*
 * A window made as an object: where it goes and what frame it wears
 * are in the metadata given, {"rect":[l,t,r,b],"attr":n}, and its title
 * is the name. "full":1 makes it the whole screen, "wide":1 as wide as
 * the screen, "shows":"uuid" names the object it shows, and
 * "sub":"uuid" makes it a subordinate window of that window, kept just
 * in front of it (a tool panel over the window it works), and "hidden":1
 * makes it hidden, to be shown once what is in it is set (OB_WR_PLACE
 * with OB_WP_SHOWN). A panel, a
 * menu or a part is made from its definition, which comes in a later
 * stage.
 */
LOCAL ER wo_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	T_DPRECT	r;
	UINT		attr = 0;
	INT		wid;
	ER		er = E_SYS;
	TS_UUID		shows, icon, parent;
	BOOL		has_shows = FALSE, has_icon = FALSE, has_parent = FALSE, hidden = FALSE;
	UB		sh[40];
	ID		maker;

	if ( c->sub == 0 && c->json != NULL && c->jsonsz > 0 ) {
		/* a copy of a definition is what the definition says it is */
		INT	pdef = knl_oj_path(c->json, (INT)c->jsonsz, "tessronos", "part");
		INT	v = ( pdef >= 0 ) ? knl_oj_member(c->json, (INT)c->jsonsz, pdef, "sub") : -1;
		UB	sub[8];
		T_OBCRE	cc = *c;

		if ( v >= 0 && knl_oj_str(c->json, (INT)c->jsonsz, v, sub, sizeof(sub)) > 0 ) {
			cc.sub = ( sub[0] == 'p' && sub[1] == 'a' && sub[2] == 'n' ) ? OB_S_PANEL
			       : ( sub[0] == 'm' ) ? OB_S_MENU : OB_S_PART;
			return fr_create(&cc, prt, p_uuid);
		}
	}
	if ( c->sub == OB_S_PANEL || c->sub == OB_S_MENU || c->sub == OB_S_PART ) {
		return fr_create(c, prt, p_uuid);
	}
	if ( c->sub != 0 && c->sub != OB_S_WINDOW ) {
		return E_PAR;
	}
	r.left = 100;
	r.top = 100;
	r.right = 500;
	r.bottom = 400;
	if ( c->json != NULL && c->jsonsz > 0 ) {
		CONST UB	*j = c->json;
		INT		len = (INT)c->jsonsz;
		INT		root = knl_oj_root(j, len);
		INT		v, pos, e, k = 0;
		D		d, val[4];

		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "rect") : -1;
		pos = v;
		while ( v >= 0 && k < 4 && ( e = knl_oj_next(j, len, v, &pos) ) >= 0 ) {
			if ( !knl_oj_num(j, len, e, &val[k]) ) break;
			k++;
		}
		if ( k == 4 ) {
			r.left = (INT)val[0];
			r.top = (INT)val[1];
			r.right = (INT)val[2];
			r.bottom = (INT)val[3];
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "attr") : -1;
		if ( v >= 0 && knl_oj_num(j, len, v, &d) ) {
			attr = (UINT)d;
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "full") : -1;
		if ( v >= 0 && knl_oj_num(j, len, v, &d) && d != 0 ) {
			INT	sw, sh;

			T_DPRECT	on;

			(void)sw;
			(void)sh;
			wm_screen_at(( r.left + r.right ) / 2, 0, &on);
			r = on;
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "wide") : -1;
		if ( v >= 0 && knl_oj_num(j, len, v, &d) && d != 0 ) {
			INT	sw, sh;

			T_DPRECT	on;

			(void)sw;
			(void)sh;
			wm_screen_at(( r.left + r.right ) / 2, 0, &on);
			r.left = on.left;
			r.right = on.right;
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "shows") : -1;
		if ( v >= 0 && knl_oj_str(j, len, v, sh, sizeof(sh)) > 0
		  && ts_str_to_uuid((CONST char *)sh, &shows) >= E_OK ) {
			has_shows = TRUE;
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "sub") : -1;
		if ( v >= 0 && knl_oj_str(j, len, v, sh, sizeof(sh)) > 0
		  && ts_str_to_uuid((CONST char *)sh, &parent) >= E_OK ) {
			has_parent = TRUE;
		}
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "hidden") : -1;
		hidden = (BOOL)( v >= 0 && knl_oj_num(j, len, v, &d) && d != 0 );
		/* the object whose icon the window wears: the program's, for a program's own window */
		v = ( root >= 0 ) ? knl_oj_member(j, len, root, "icon") : -1;
		if ( v >= 0 && knl_oj_str(j, len, v, sh, sizeof(sh)) > 0
		  && ts_str_to_uuid((CONST char *)sh, &icon) >= E_OK ) {
			has_icon = TRUE;
		}
	}
	maker = knl_prc_self(NULL, NULL);
	wid = wm_open(&r, attr, (CONST char *)c->name);
	if ( wid < 0 ) {
		return (ER)wid;
	}
	if ( hidden ) {
		(void)wm_show(wid, FALSE);	/* before anything of it is built */
	}
	LOCK();
	if ( wid <= wo_nwin && WOW(wid).used ) {
		WOW(wid).prt.owner = prt->owner;
		WOW(wid).prt.group = prt->group;
		WOW(wid).prt.mode = prt->mode & 0777;
		WOW(wid).drive = TRUE;	/* nobody else reads its input */
		WOW(wid).maker = maker;
		if ( has_shows ) {
			WOW(wid).shows = shows;
		}
		*p_uuid = WOW(wid).uuid;
		er = E_OK;
	}
	UNLOCK();
	if ( er >= E_OK && has_parent ) {
		INT	pw = wm_obj_wid(&parent);

		if ( pw > 0 ) {
			(void)wm_set_parent(wid, pw);
		}
	}
	/*
	 * A window a program asks something in (a pop-up, over its own) has
	 * the keys while it is up: what is typed is typed into it.
	 */
	if ( er >= E_OK && ( attr & WM_ATTR_POPUP ) != 0 ) {
		INT	was = wm_focused();

		LOCK();
		if ( wid <= wo_nwin && WOW(wid).used ) WOW(wid).back = was;
		UNLOCK();
		(void)wm_focus(wid);
	} else if ( er >= E_OK ) {
		open_activate(wid, maker, attr, has_parent);
	}
	if ( er < E_OK ) {
		(void)wm_close(wid);
	} else if ( has_icon ) {
		icon_of(wid, &icon);
	} else if ( has_shows ) {
		icon_of(wid, &shows);
	}
	wm_update();			/* a new window: its frame as well as its work area */

	return er;
}

LOCAL ER wo_remove( CONST TS_UUID *uuid )
{
	INT	h = locate(uuid);
	ER	er;

	if ( h < 0 ) {
		return (ER)h;
	}
	switch ( H_KIND(h) ) {
	case H_FREE:
		return fr_remove(h);
	case H_WIN: {
		BOOL	drive;
		INT	back;

		LOCK();
		drive = WOW(H_A(h)).drive;
		back = WOW(H_A(h)).back;
		UNLOCK();
		if ( !drive ) {
			/*
			 * A window its program opened is the program's to
			 * close: it has things to keep before it goes. It is
			 * asked, and closes when it has.
			 */
			win_post(H_A(h), OB_E_CLOSE, NULL);
			return E_OK;
		}
		{
			BOOL	had = (BOOL)( wm_focused() == H_A(h) );

			er = wm_close(H_A(h));
			if ( had && back > 0 ) (void)wm_focus(back);	/* the keys back where they were */
		}
		wm_update();
		return er;
	}
	case H_PNL:
		er = wm_panel_close(H_A(h));
		wm_update();
		return er;
	default:
		return E_NOSPT;			/* a part goes with its panel */
	}
}

/* Every window, panel and part, after 'from', in UUID order */
LOCAL ER wo_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	TS_UUID	*all;
	INT	max;
	INT	cnt = 0, i, k;

	LOCK();
	max = WO_FREE_MAX + wo_nwin + WM_PANEL_MAX * ( 1 + WM_PART_MAX );
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)max);
	if ( all == NULL ) {
		UNLOCK();
		return E_NOMEM;
	}
	for ( i = 0; i < WO_FREE_MAX; i++ ) {
		if ( wo_free[i].used ) all[cnt++] = wo_free[i].uuid;
	}
	for ( i = 1; i <= wo_nwin; i++ ) {
		if ( WOW(i).used ) all[cnt++] = WOW(i).uuid;
	}
	for ( i = 1; i <= WM_PANEL_MAX; i++ ) {
		if ( !wo_pnl[i].used ) continue;
		all[cnt++] = wo_pnl[i].uuid;
		for ( k = 0; k < wo_pnl[i].npart; k++ ) {
			all[cnt++] = wo_pnl[i].part[k];
		}
	}
	UNLOCK();

	/* the few there are, in order */
	for ( i = 1; i < cnt; i++ ) {
		TS_UUID	u = all[i];

		for ( k = i; k > 0 && ts_uuid_cmp(&all[k - 1], &u) > 0; k-- ) {
			all[k] = all[k - 1];
		}
		all[k] = u;
	}
	for ( i = 0, k = 0; i < cnt && k < n; i++ ) {
		if ( i > 0 && ts_uuid_cmp(&all[i], &all[i - 1]) == 0 ) {
			continue;			/* one placed is in two tables */
		}
		if ( from == NULL || knl_ob_uuid_zero(from) || ts_uuid_cmp(&all[i], from) > 0 ) {
			buf[k++] = all[i];
		}
	}
	Kfree(all);
	*p_cnt = k;

	return E_OK;
}

LOCAL ER wo_apd( INT h, UINT rt, UINT sub, INT *p_recno )
{
	if ( !alive(h) ) {
		return E_NOEXS;
	}
	return ( H_KIND(h) == H_FREE && p_recno != NULL ) ? fr_apd(h, rt, sub, p_recno)
							 : E_NOSPT;
}

LOCAL ER wo_map( INT h, INT recno, INT target, T_OBMAP *m )
{
	if ( target < 0 ) {
		return E_NOSPT;			/* a window is not mapped into a space */
	}
	if ( !alive(h) || !alive(target) ) {
		return E_NOEXS;
	}
	if ( recno != 0 ) {
		return E_PAR;			/* the object as a whole */
	}
	return ( H_KIND(h) == H_FREE ) ? fr_map(h, target, m) : E_NOSPT;
}

LOCAL ER wo_unm( INT h, INT recno, INT target )
{
	if ( !alive(h) ) {
		return E_NOEXS;
	}
	if ( recno != 0 ) {
		return E_PAR;
	}
	return ( H_KIND(h) == H_FREE ) ? fr_unm(h, target) : E_NOSPT;
}

LOCAL CONST T_OBMGR wo_mgr = {
	OB_T_WINDOW, 0, "window",
	wo_find, wo_ref, wo_prot, wo_setprot, wo_create, wo_remove,
	wo_open, wo_close, wo_rea, wo_wri, wo_apd, wo_trn, NULL, wo_lrc,
	wo_gat, wo_sat, NULL, wo_lst, NULL,
	NULL, NULL, NULL, NULL,
	wo_map, wo_unm,
	/* no icon of their own */
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	NULL			/* nothing comes in to wait for (ob_rea_tmo) */
};

/* The window manager registered with the name manager, which must be up */
EXPORT ER knl_wmobj_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	if ( wo_mtx > 0 ) {
		return E_OK;
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	wo_mtx = tk_cre_mtx(&cmtx);
	if ( wo_mtx <= 0 ) {
		return (ER)wo_mtx;
	}
	no = knl_ob_regist(&wo_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
