/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtrthost.c
 *	実時間制御TAD played in a window of a process's own (design 15.5.1)
 *
 *	The scenes of a record of 実時間制御TAD are documents and figures,
 *	which only the desktop can draw (11.11). The program that plays
 *	media, MPEG Player, asks the desktop to play such a record in its
 *	window (DT_RQ_RT_OPEN, ts/dtreq.h): the scene is laid out in the top
 *	left of the window's work area, as large as the program says, and
 *	the player of dtrt.c draws it there from its task, as it draws a
 *	slide, starting the processes that sound and decode for it. The
 *	program passes on what is done to its window and its parts
 *	(DT_RQ_RT_DO) and is told in each answer how the program goes. Only
 *	the process that made the window may ask about its player. The
 *	player goes when the process says so (DT_RQ_RT_CLOSE), or when its
 *	window is gone -- the process ended, or was ended -- which the
 *	desktop's loop looks for (dt_rthost_poll).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/rtad.h>
#include <ts/rtmedia.h>
#include <ts/time.h>
#include <ts/dtreq.h>
#include <ts/fquit.h>
#include "desktop.h"

#define RH_MAX		4		/* players in the windows of processes at once */
#define RH_TRACKS	64
#define RH_MIN		16		/* the least of a scene's area */

typedef struct {
	BOOL		used;
	UINT		no;		/* what the process names it by */
	ID		pid;		/* the process that made the window */
	TS_UUID		win;		/* the window */
	INT		wid;
	TS_UUID		obj;
	DTRT		*rt;
	UW		ground;		/* round the scene */
	INT		w, h;		/* the area it is laid out in */
	INT		track;		/* the one of the playlist chosen last, -1 none */
	BOOL		moved;		/* a move was told on the console (only the first is) */
	BOOL		slide;		/* played as a slide: what its program asks of the slides is kept */
	UINT		act;		/* and what it asked last, not yet handed on (RT_ACT_*, 0 none) */
	INT		act_n;
} RTHOST;

LOCAL RTHOST	rh[RH_MAX];
LOCAL UINT	rh_last = 0;

/* What each DT_RT_* is called on the console */
LOCAL CONST char *CONST rh_ops[] = {
	"state", "toggle", "stop", "restart", "seek", "track", "area", "redraw",
	"ground", "list", "press", "release", "key", "move"
};
#define RH_NOPS	( (INT)( sizeof(rh_ops) / sizeof(rh_ops[0]) ) )

LOCAL RTHOST *rh_of( UINT no, ID pid )
{
	INT	i;

	for ( i = 0; i < RH_MAX; i++ ) {
		if ( rh[i].used && rh[i].no == no && rh[i].pid == pid ) return &rh[i];
	}
	return NULL;
}

/* The ground round the scene, and the scene over it */
LOCAL void rh_draw( RTHOST *e, BOOL relayout )
{
	T_DPRECT	all;
	INT		gid = wm_gid(e->wid);

	if ( gid < 0 ) return;
	knl_memset(&all, 0, sizeof(all));
	all.right = e->w;
	all.bottom = e->h;
	(void)dp_fill_rect(gid, &all, e->ground);
	if ( relayout ) {
		dtrt_relayout(e->rt, &all);
		dtrt_visible(e->rt, TRUE);
	} else {
		dtrt_redraw(e->rt);
	}
	wm_update();
}

LOCAL void rh_end( RTHOST *e, CONST char *why )
{
	if ( e->rt != NULL ) dtrt_close(e->rt);
	e->rt = NULL;
	e->used = FALSE;
	tm_printf((UB *)"player: %u closed %s\n", e->no, why);
}

/* How the program goes, in the answer */
LOCAL void rh_state( RTHOST *e, T_DTANS *an )
{
	UB	(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * RH_TRACKS);
	INT	idx[RH_TRACKS];
	UINT	s = 0;

	if ( dtrt_waiting(e->rt) ) s |= DT_RTS_WAITING;
	else if ( dtrt_done(e->rt) ) s |= DT_RTS_DONE;
	else if ( dtrt_paused(e->rt) ) s |= DT_RTS_PAUSED;
	else s |= DT_RTS_PLAYING;
	an->flags = ( an->flags & DT_RTS_TAKEN ) | s;
	if ( e->slide && e->act != 0 ) {
		an->flags |= DT_RTS_GO;
		an->look.disp = e->act;
		an->look.autoopen = (UINT)e->act_n;
		e->act = 0;
	}
	an->look.w = dtrt_time(e->rt);
	an->look.h = ( names != NULL ) ? dtrt_tracks(e->rt, names, idx, RH_TRACKS) : 0;
	if ( names != NULL ) Kfree(names);
	(void)dtrt_now_playing(e->rt, an->text, DT_TEXT_MAX);
}

/* The medium so many after (before) the one playing, or the one chosen last */
LOCAL void rh_step( RTHOST *e, INT by )
{
	UB	(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * RH_TRACKS);
	INT	idx[RH_TRACKS], n, k, now;

	if ( names == NULL ) return;
	n = dtrt_tracks(e->rt, names, idx, RH_TRACKS);
	Kfree(names);
	if ( n <= 0 ) return;
	now = dtrt_playing(e->rt);
	for ( k = 0; k < n && idx[k] != now; k++ ) ;
	if ( k < n ) e->track = k;
	e->track = ( e->track < 0 ) ? 0 : ( ( e->track + by ) % n + n ) % n;
	dtrt_play_track(e->rt, idx[e->track]);
}

/* プレイリスト表示: the media of the program in a panel over the window, the one chosen played */
LOCAL void rh_list( RTHOST *e )
{
	UB		(*names)[WM_LABEL_MAX] = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * RH_TRACKS);
	CONST char	**list = (CONST char **)Kmalloc(sizeof(char *) * RH_TRACKS);
	INT		idx[RH_TRACKS], n, i, at;
	DTWIN		*d = dt_host_of(&e->win);

	if ( names != NULL && list != NULL ) {
		n = dtrt_tracks(e->rt, names, idx, RH_TRACKS);
		if ( n == 0 ) {
			dt_tell(d, "プレイリストに曲がありません", NULL);
		} else {
			for ( i = 0; i < n; i++ ) list[i] = (CONST char *)names[i];
			at = dt_list_form(d, "プレイリスト", list, n, e->track);
			if ( at >= 0 && at < n ) {
				e->track = at;
				dtrt_play_track(e->rt, idx[at]);
			}
		}
	}
	if ( names != NULL ) Kfree(names);
	if ( list != NULL ) Kfree((void *)list);
}

/* 背景色変更: what is round the scene, chosen in a panel over the window */
LOCAL void rh_ground( RTHOST *e )
{
	static CONST char *CONST names[4] = { "黒", "白", "グレー", "青" };
	static CONST UW		cols[4] = { 0x00000000U, 0x00FFFFFFU, 0x00808080U, 0x00000080U };
	INT			at;

	at = dt_list_form(dt_host_of(&e->win), "背景色変更", names, 4, -1);
	if ( at >= 0 && at < 4 ) e->ground = cols[at];
}

/* A press, a release, a move or a key of the window, as the window manager would have given it */
LOCAL BOOL rh_input( RTHOST *e, UINT type, CONST T_DTREQ *rq )
{
	T_WMEV	ev;
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.wid = e->wid;
	ev.when = ns;
	if ( type == HID_EV_KEY_DOWN || type == HID_EV_KEY_UP ) {
		ev.code = (UINT)rq->grab_x;
		/* the program's own key goes to it only when it takes it; any other always */
		if ( dtrt_input(e->rt, &ev, TRUE) ) {
			(void)dtrt_input(e->rt, &ev, FALSE);
			return TRUE;
		}
		if ( ( rq->grab_y & DT_RT_KEY_MINE ) == 0 ) (void)dtrt_input(e->rt, &ev, FALSE);
		return FALSE;
	}
	ev.code = 0;
	ev.x = rq->grab_x;
	ev.y = rq->grab_y;
	return dtrt_input(e->rt, &ev, FALSE);
}

LOCAL ER rh_open( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	RTHOST		*e = NULL;
	INT		wid = wm_obj_wid(&rq->win), recno = ( rq->recno >= 0 ) ? rq->recno : 0, i;
	ID		maker = 0, forp = 0;
	T_TAD		*rec;
	T_DPRECT	all;
	UW		paper;

	if ( wid <= 0 ) return E_NOEXS;
	/* the window has to be the asking process's own */
	if ( knl_wmobj_who(wid, TMO_FEVR, &maker, &forp) < E_OK || maker != pid ) return E_OACV;
	rec = om_store_get(&rq->target, recno);
	if ( rec == NULL ) return E_NOEXS;
	if ( !dtrt_has(rec) ) return E_OBJ;
	for ( i = 0; i < RH_MAX && e == NULL; i++ ) {
		if ( !rh[i].used ) e = &rh[i];
	}
	if ( e == NULL ) return E_LIMIT;
	knl_memset(e, 0, sizeof(*e));
	e->pid = pid;
	e->win = rq->win;
	e->wid = wid;
	e->obj = rq->target;
	e->track = -1;
	e->ground = ( rq->look.bgcol == 0xFFFFFFFFU ) ? 0x00000000U : (UW)rq->look.bgcol;
	e->w = ( rq->look.w > RH_MIN ) ? rq->look.w : RH_MIN;
	e->h = ( rq->look.h > RH_MIN ) ? rq->look.h : RH_MIN;
	e->slide = (BOOL)( ( rq->flags & DT_F_RT_SLIDE ) != 0 );
	if ( e->slide && rq->look.chcol != 0xFFFFFFFFU ) {
		paper = (UW)rq->look.chcol;
	} else if ( om_store_paper(&rq->target, &paper) < E_OK || paper == TV_PAPER_LOOK ) {
		paper = e->slide ? 0x00FFFFFFU : 0x00000000U;
	}
	knl_memset(&all, 0, sizeof(all));
	all.right = e->w;
	all.bottom = e->h;
	i = wm_gid(wid);
	if ( i >= 0 ) (void)dp_fill_rect(i, &all, e->ground);
	/* a slide fitted to the area as the slides are made for, as the desktop shows one */
	e->rt = e->slide ? dtrt_open(wid, &rq->target, recno, &all, rq->grab_x, rq->grab_y, paper, (INT)rq->carry)
			 : dtrt_open(wid, &rq->target, recno, &all, 0, 0, paper, 0);
	if ( e->rt == NULL ) return E_NOMEM;
	dtrt_host(e->rt, &rq->win);
	if ( ++rh_last == 0 ) rh_last = 1;
	e->no = rh_last;
	e->used = TRUE;
	wm_update();
	tm_printf((UB *)"player: hosted %u in a window of process %d%s\n", e->no, (INT)pid,
		  e->slide ? " as a slide" : "");
	an->how = e->no;
	rh_state(e, an);
	return E_OK;
}

/*
 * A record of 実時間制御TAD opened: played by the program that plays media
 * (RTM_VIDEO_PROG, MPEG Player), which hands its scene to the desktop
 */
EXPORT void dt_rt_play( DTWIN *from, CONST T_VOBJ *v )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)RTM_VIDEO_PROG);

	if ( p == NULL || p->builtin ) {
		tm_printf((UB *)"player: no MPEG Player to play 実時間制御TAD\n");
		(void)wm_msg_put("MPEG Playerが入っていないので、実時間制御TADを再生できません");
		return;
	}
	(void)dt_prog_run(from, p, v);
}

EXPORT ER dt_rthost_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	RTHOST	*e;

	if ( rq->req == DT_RQ_RT_OPEN ) return rh_open(rq, an, pid);
	e = rh_of(rq->carry, pid);
	if ( e == NULL ) return E_NOEXS;
	if ( wm_obj_wid(&e->win) != e->wid ) {
		rh_end(e, "with its window");
		return E_NOEXS;
	}
	if ( rq->req == DT_RQ_RT_CLOSE ) {
		rh_end(e, "by its process");
		return E_OK;
	}
	if ( rq->recno > DT_RT_STATE && rq->recno < RH_NOPS && ( rq->recno != DT_RT_MOVE || !e->moved ) ) {
		if ( rq->recno == DT_RT_MOVE ) e->moved = TRUE;
		tm_printf((UB *)"player: %u op %s\n", e->no, rh_ops[rq->recno]);
	}
	switch ( rq->recno ) {
	case DT_RT_STATE:	break;
	case DT_RT_TOGGLE:	dtrt_toggle(e->rt);			break;
	case DT_RT_STOP:	dtrt_stop(e->rt);			break;
	case DT_RT_RESTART:	dtrt_restart(e->rt);			break;
	case DT_RT_SEEK:	dtrt_seek_by(e->rt, rq->grab_x);	break;
	case DT_RT_TRACK:	rh_step(e, rq->grab_x);			break;
	case DT_RT_AREA:
		e->w = ( rq->look.w > RH_MIN ) ? rq->look.w : RH_MIN;
		e->h = ( rq->look.h > RH_MIN ) ? rq->look.h : RH_MIN;
		rh_draw(e, TRUE);
		break;
	case DT_RT_REDRAW:	rh_draw(e, FALSE);			break;
	case DT_RT_GROUND:	rh_ground(e); rh_draw(e, FALSE);	break;
	case DT_RT_LIST:	rh_list(e);				break;
	case DT_RT_PRESS:
	case DT_RT_RELEASE:
		if ( rh_input(e, ( rq->recno == DT_RT_PRESS ) ? HID_EV_BTN_DOWN : HID_EV_BTN_UP, rq) ) {
			an->flags |= DT_RTS_TAKEN;
		}
		break;
	case DT_RT_MOVE:
		/* the pointer over the scene, for mouseenter and mouseleave */
		(void)rh_input(e, HID_EV_MOVE, rq);
		break;
	case DT_RT_KEY:
		if ( rh_input(e, ( rq->grab_y & DT_RT_KEY_UP ) ? HID_EV_KEY_UP : HID_EV_KEY_DOWN, rq) ) {
			an->flags |= DT_RTS_TAKEN;
		}
		break;
	default:
		return E_PAR;
	}
	rh_state(e, an);
	return E_OK;
}

/* From the desktop's loop: a player whose window has gone goes with it */
EXPORT void dt_rthost_poll( void )
{
	INT	i, n;

	for ( i = 0; i < RH_MAX; i++ ) {
		UINT	a;

		if ( !rh[i].used ) continue;
		/* what a slide's program asks of the slides is kept for its window's program; elsewhere nothing */
		a = dtrt_action(rh[i].rt, &n);
		if ( rh[i].slide && a != 0 && a != RT_ACT_DONE ) {
			rh[i].act = a;
			rh[i].act_n = n;
		}
		if ( wm_obj_wid(&rh[i].win) != rh[i].wid ) rh_end(&rh[i], "with its window");
	}
}

/* How many players play in the windows of processes (the tests) */
EXPORT INT dt_rthost_count( void )
{
	INT	i, n = 0;

	for ( i = 0; i < RH_MAX; i++ ) {
		if ( rh[i].used ) n++;
	}
	return n;
}
