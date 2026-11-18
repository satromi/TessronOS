/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtinfo.c
 *	管理情報: what the system keeps of a real object, and setting it
 *	(design 18.15), started from the desktop
 *
 *	管理情報 is a program of its own (application/realinfo), a process
 *	for each object, started on the link 管理情報 was chosen on: the
 *	link is its start-up argument (openArg "link": the object, the
 *	object whose record holds the link, the link's vobjid). Several can
 *	be open, each of its own object, IF_WIN_MAX at most as before; asked
 *	again for an object whose 管理情報 is open, its window is brought to
 *	the front with the keys, and while its process is still being
 *	started nothing more is started.
 *
 *	What it shows the process reads itself, and the protection it sets
 *	itself (ob_set_prt), with the rights of the person it runs for, as
 *	the desktop did: the owner or an administrator may change it. What
 *	only the desktop keeps it asks here, for the link it was started on
 *	and no other, and only the process started for it is answered:
 *	the list 実行 shows (DT_RQ_APPS with DT_F_INFO), the applist written
 *	(DT_RQ_INFO_APPS), and 起動アプリの固定 of the link in the window it
 *	was chosen in (DT_RQ_INFO_KEEP), while that window stays open.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "desktop.h"

#define IF_WIN_MAX	8		/* 管理情報 open at once */
#define IF_START_MS	30000		/* a start that has not shown a window by then is forgotten */

/* What a place of the table is */
#define IS_FREE		0
#define IS_STARTING	1		/* handed to the starter */
#define IS_STARTED	2		/* the process is there */
#define IS_FAILED	3

/*
 * One 管理情報 open, or being started. The desktop's task takes a free
 * place and starts it; the starter says how the start went
 * (dt_info_started), with __atomic as dtprog.c's opening does; the
 * desktop's task alone frees a place again.
 */
typedef struct {
	UINT	state;
	TS_UUID	proc;			/* the process, once IS_STARTED */
	UW	asked;			/* when it was started, in ms */
	T_VOBJ	link;			/* the link it was started on */
	DTWIN	*parent;		/* the window the link was chosen in, while it stays open */
	INT	parent_wid;
	BOOL	parent_sealed;
} INFOPROC;

LOCAL INFOPROC	info_tab[IF_WIN_MAX];

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

EXPORT void dt_info_started( INT slot, ER er, CONST TS_UUID *proc )
{
	INFOPROC	*e;

	if ( slot < 1 || slot > IF_WIN_MAX ) {
		return;
	}
	e = &info_tab[slot - 1];
	if ( er >= E_OK && proc != NULL ) {
		e->proc = *proc;
		__atomic_store_n(&e->state, IS_STARTED, __ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&e->state, IS_FAILED, __ATOMIC_RELEASE);
	}
}

/* The window the process of place e shows, 0 none yet */
LOCAL INT window_of( CONST INFOPROC *e )
{
	INT	wids[64], n, i;
	ID	pid = knl_prc_of_uuid(&e->proc);

	if ( pid <= 0 ) {
		return 0;
	}
	n = wm_list(wids, 64);
	for ( i = 0; i < n && i < 64; i++ ) {
		if ( wm_obj_maker(wids[i]) == pid ) {
			return wids[i];
		}
	}
	return 0;
}

/* Whether place e still stands for a 管理情報 open or being started; freed when not */
LOCAL BOOL stands( INFOPROC *e )
{
	UINT	st = __atomic_load_n(&e->state, __ATOMIC_ACQUIRE);

	if ( st == IS_FREE ) {
		return FALSE;
	}
	if ( st == IS_STARTING ) {
		if ( (UW)( now_ms() - e->asked ) < IF_START_MS ) {
			return TRUE;
		}
	} else if ( st == IS_STARTED && knl_prc_of_uuid(&e->proc) > 0 ) {
		/* there, or still putting its window up */
		if ( window_of(e) > 0 || (UW)( now_ms() - e->asked ) < IF_START_MS ) {
			return TRUE;
		}
	}
	__atomic_store_n(&e->state, IS_FREE, __ATOMIC_RELEASE);
	tad_vobj_clear(&e->link);	/* the place's copy of the link let go */
	return FALSE;
}

/* The place of the process that asked from, standing for target; NULL none */
LOCAL INFOPROC *asker( ID from, CONST TS_UUID *target )
{
	INT	i;

	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		INFOPROC	*e = &info_tab[i];

		if ( __atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == IS_STARTED && stands(e)
		  && from > 0 && knl_prc_of_uuid(&e->proc) == from
		  && ts_uuid_cmp(&e->link.target, target) == 0 ) {
			return e;
		}
	}
	return NULL;
}

EXPORT void dt_real_info( DTWIN *d, CONST T_VOBJ *v )
{
	CONST DTPROG	*p;
	INFOPROC	*e = NULL;
	TS_UUID		pu;
	INT		i, n = 0, wid;
	ER		er;

	/* the object's 管理情報 open already: brought to the front */
	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		INFOPROC	*t = &info_tab[i];

		if ( !stands(t) ) {
			if ( e == NULL ) e = t;
			continue;
		}
		n++;
		if ( ts_uuid_cmp(&t->link.target, &v->target) == 0 ) {
			if ( __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == IS_STARTED
			  && ( wid = window_of(t) ) > 0 ) {
				wm_raise(wid);
				(void)wm_focus(wid);
				wm_update();
				tm_printf((UB *)"info: brought forward\n");
			}
			return;			/* or being started: it comes up by itself */
		}
	}
	if ( n >= IF_WIN_MAX || e == NULL ) {
		dt_tell(d, "管理情報のウインドウをこれ以上開けません", NULL);
		return;
	}
	if ( ts_str_to_uuid(SYSDEF_PROG_INFO, &pu) < E_OK || ( p = dt_prog_of(&pu) ) == NULL ) {
		dt_tell(d, "管理情報のプログラムがありません", NULL);
		return;
	}
	tad_vobj_clear(&e->link);
	knl_memset(e, 0, sizeof(*e));
	if ( tad_vobj_copy(&e->link, v) < E_OK ) {
		dt_tell(d, "メモリが足りないため、管理情報を開けません", NULL);
		return;
	}
	e->parent = d;
	e->parent_wid = ( d != NULL ) ? d->wid : 0;
	e->parent_sealed = FALSE;
	e->asked = now_ms();
	__atomic_store_n(&e->state, IS_STARTING, __ATOMIC_RELEASE);
	er = dt_prog_start_info(d, p, v, (INT)( e - info_tab ) + 1);
	if ( er < E_OK ) {
		__atomic_store_n(&e->state, IS_FREE, __ATOMIC_RELEASE);
		tad_vobj_clear(&e->link);
	}
}

/* The window the link was chosen in, if it is still open */
LOCAL DTWIN *parent_of( CONST INFOPROC *e )
{
	DTWIN	*p = dt_win_of(e->parent_wid);

	return ( p != NULL && p == e->parent && p->used ) ? p : NULL;
}

/* A number at *p of text, and the tab or line feed after it passed */
LOCAL INT text_num( CONST UB *t, INT *p )
{
	INT	i = *p, v = 0;
	BOOL	neg = FALSE;

	if ( t[i] == '-' ) {
		neg = TRUE;
		i++;
	}
	while ( t[i] >= '0' && t[i] <= '9' ) {
		v = v * 10 + ( t[i++] - '0' );
	}
	*p = i;
	return neg ? -v : v;
}

/*
 * DT_RQ_INFO_APPS: the applist written as the text says, the place of
 * the one it opens with first, then each program on a line of its own
 * (its id, a tab, its name)
 */
LOCAL ER apps_set( CONST INFOPROC *e, CONST T_DTREQ *rq )
{
	SZ		tl;
	CONST UB	*t = dt_rq_word(rq, DT_TAIL_TEXT, &tl);
	INT		lines = 1, i = 0, n = 0, dflt;
	UB		*buf;
	CONST UB	**ids, **names;
	ER		er = E_OK;

	/* as many programs as the text has lines, however many */
	for ( i = 0; i < (INT)tl; i++ ) {
		if ( t[i] == '\n' ) lines++;
	}
	i = 0;
	buf = (UB *)Kmalloc((size_t)( tl + 1 ));
	ids = (CONST UB **)Kmalloc(sizeof(UB *) * (size_t)lines);
	names = (CONST UB **)Kmalloc(sizeof(UB *) * (size_t)lines);
	if ( buf == NULL || ids == NULL || names == NULL ) {
		er = E_NOMEM;
		goto done;
	}
	knl_memcpy(buf, t, tl + 1);
	dflt = text_num(buf, &i);
	while ( buf[i] == '\n' && n < lines ) {
		buf[i++] = 0;
		ids[n] = &buf[i];
		while ( buf[i] != 0 && buf[i] != '\t' && buf[i] != '\n' ) i++;
		if ( buf[i] != '\t' ) {
			er = E_PAR;
			goto done;
		}
		buf[i++] = 0;
		names[n] = &buf[i];
		while ( buf[i] != 0 && buf[i] != '\n' ) i++;
		n++;
	}
	if ( buf[i] != 0 ) {
		er = E_PAR;
		goto done;
	}
	if ( dflt >= n ) {
		dflt = -1;
	}
	er = om_store_set_apps(&e->link.target, ids, names, n, dflt);
done:
	if ( buf != NULL ) Kfree(buf);
	if ( ids != NULL ) Kfree((void *)ids);
	if ( names != NULL ) Kfree((void *)names);
	return er;
}

/*
 * DT_RQ_APPS with DT_F_INFO: the entry at recno of the list 実行 shows
 * for the link, open or not: the program's id, a tab and its name, how
 * many there are in how, DT_F_KIND for one offered for the object's
 * kind and DT_F_DEFAULT for the one it opens with
 */
LOCAL ER apps_list( CONST INFOPROC *e, CONST T_DTREQ *rq, T_DTANS *an )
{
	DTAPP	*a;
	INT	room, n, dflt = -1;
	ER	er = E_NOEXS;

	room = dt_app_list(&e->link, NULL, 0, NULL);
	a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));
	if ( a == NULL ) {
		return E_NOMEM;
	}
	n = dt_app_list(&e->link, a, room, &dflt);
	an->how = (UINT)n;
	an->flags = 0;
	if ( rq->recno >= 0 && rq->recno < n ) {
		CONST DTAPP	*p = &a[rq->recno];

		er = dt_ans_cat(an, p->id, -1);
		if ( er >= E_OK ) er = dt_ans_cat(an, (CONST UB *)"\t", 1);
		if ( er >= E_OK ) er = dt_ans_cat(an, p->name, -1);
		if ( !p->listed ) an->flags |= DT_F_KIND;
		if ( rq->recno == dflt ) an->flags |= DT_F_DEFAULT;
	}
	dt_app_free(a, n);
	return er;
}

EXPORT ER dt_info_req( CONST T_DTREQ *rq, T_DTANS *an, ID from )
{
	INFOPROC	*e = asker(from, &rq->target);
	DTWIN		*pw;
	BOOL		lockable;

	if ( e == NULL ) {
		return E_PAR;			/* not the 管理情報 started on that object */
	}
	switch ( rq->req ) {
	case DT_RQ_APPS:
		return apps_list(e, rq, an);
	case DT_RQ_INFO_APPS:
		return apps_set(e, rq);
	case DT_RQ_INFO_KEEP:
		pw = parent_of(e);
		lockable = (BOOL)( pw != NULL && !e->parent_sealed && dt_app_lockable(&e->link) );
		an->flags = 0;
		if ( lockable ) an->flags |= DT_F_LOCKABLE;
		if ( dt_app_locked(&e->link) ) an->flags |= DT_F_KEEP;
		if ( ( rq->flags & DT_F_ASK ) != 0 ) {
			/* where that window stands, for 管理情報's to stand down and right of it */
			T_WMWIN	ww;

			an->look.w = an->look.h = -1;
			/* a program's window (the first window's too) as well as a part's: by its number */
			if ( wm_ref(( pw != NULL ) ? pw->wid : e->parent_wid, &ww) >= E_OK ) {
				an->look.w = ww.outer.left;
				an->look.h = ww.outer.top;
			}
			return E_OK;
		}
		if ( !lockable ) {
			return E_NOEXS;
		}
		/* the copy kept here as the window's record has it now */
		(void)om_link_lock(&e->link, (BOOL)( ( rq->flags & DT_F_KEEP ) != 0 ));
		return E_OK;
	default:
		return E_PAR;
	}
}
