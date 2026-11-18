/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttool.c
 *	What the desktop does for the system's tools (design 16.5.17)
 *
 *	屑実身操作, 実身/仮身検索, 仮身ネットワーク and ウインドウ一覧 are
 *	programs running as processes (application/systool), each with its
 *	program object. They make their lists themselves from what they may
 *	read; here is what only the desktop can do for them:
 *
 *	  - started, each runs once: run again while its window is open, the
 *	    window is brought to the front with the keys and the tool is told
 *	    (DT_HOW_AGAIN), 仮身ネットワーク with the object to start from
 *	    (dt_prog_again, from dt_prog_run). The same is done for every
 *	    program run again while a window of it stands for its program
 *	    object, and it is told when it asked to be (DT_RQ_TELL)
 *	  - a tool is told when what it lists may have changed: 屑実身操作
 *	    when a record is saved or a package installed or deleted
 *	    (dt_trash_tell), ウインドウ一覧 when the windows on the screen or
 *	    their order change (dt_winlist_poll, from the desktop's loop)
 *	  - 屑実身操作's 削除 and 再探索: an object thrown away for good, the
 *	    counts of links made again (DT_RQ_PURGE, DT_RQ_RECOUNT)
 *	  - ウインドウ一覧's list, and the window it opens brought to the
 *	    front (DT_RQ_WIN_LIST)
 *	  - what the system itself holds, which no record links to
 *	    (DT_RQ_ROOTS): 屑実身操作 leaves it out of its list
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>
#include <ts/sysdef.h>
#include "obj/obj.h"
#include "desktop.h"

#define ID_TRASH	"trash-real-objects"
#define ID_SEARCH	"real-object-search"
#define ID_NETWORK	"virtual-object-network"
#define ID_WINLIST	"window-list-viewer"

LOCAL BOOL same_id( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

LOCAL BOOL is_tool( CONST UB *id )
{
	return (BOOL)( same_id(id, ID_TRASH) || same_id(id, ID_SEARCH) || same_id(id, ID_NETWORK)
		    || same_id(id, ID_WINLIST) );
}

/* ---------------------------------------------------------------- telling the programs */

/*
 * The channels the programs named (DT_RQ_TELL), each with the program's
 * id, until it can no longer be opened (the process ended and its
 * channel went). The oldest gives way when there is no room.
 */
#define TOLD_MAX	32
#define TOLD_ID		32

typedef struct {
	TS_UUID	ch;
	UB	prog[TOLD_ID];
	BOOL	tail;			/* it asked in the form of now: told with a tail */
} TOLD;

LOCAL TOLD	told[TOLD_MAX];
LOCAL INT	ntold = 0;

LOCAL void told_drop( INT i )
{
	INT	j;

	for ( j = i + 1; j < ntold; j++ ) told[j - 1] = told[j];
	ntold--;
}

LOCAL ER tool_listen( CONST T_DTREQ *rq, ID asker )
{
	TS_UUID		u;
	SZ		nl;
	CONST UB	*name = dt_rq_word(rq, DT_TAIL_NAME, &nl);
	INT		i;

	if ( ts_str_to_uuid((CONST char *)dt_rq_word(rq, DT_TAIL_TEXT, NULL), &u) < E_OK ) {
		return E_PAR;
	}
	if ( nl >= TOLD_ID ) {
		return E_PAR;			/* longer than any program's id */
	}
	if ( same_id(name, DT_TELL_FIRST) ) {
		/* the first window's: only the process of the window of the cabinet */
		TS_UUID	cab;
		INT	wid;

		if ( ts_str_to_uuid(SYSDEF_CABINET, &cab) < E_OK || ( wid = wm_obj_showing(&cab) ) <= 0
		  || wm_obj_maker(wid) != asker ) {
			return E_PAR;
		}
	} else if ( dt_prog_find(name) == NULL ) {
		return E_PAR;
	}
	for ( i = 0; i < ntold; i++ ) {
		if ( ts_uuid_cmp(&told[i].ch, &u) == 0 ) {
			told[i].tail = dt_rq_tailed();	/* told as it asks now */
			return E_OK;
		}
	}
	if ( ntold >= TOLD_MAX ) told_drop(0);
	told[ntold].ch = u;
	knl_memcpy(told[ntold].prog, name, nl + 1);
	told[ntold].tail = dt_rq_tailed();
	ntold++;
	return E_OK;
}

/*
 * Every channel of the program told how (DT_HOW_*), into the object it is
 * about, text and data with it; FALSE: none heard
 */
EXPORT BOOL dt_prog_tell_data( CONST char *prog, UINT how, CONST TS_UUID *into,
			       CONST UB *text, CONST UB *data, SZ dlen )
{
	T_DTANS	an;
	INT	i = 0;
	BOOL	heard = FALSE, gone;

	knl_memset(&an, 0, sizeof(an));
	an.er = E_OK;
	an.how = how;
	if ( into != NULL ) an.into = *into;
	while ( i < ntold ) {
		if ( !same_id(told[i].prog, prog) ) {
			i++;
			continue;
		}
		if ( dt_tell_words(&told[i].ch, &an, told[i].tail, text, -1, data, dlen, &gone) >= E_OK ) {
			heard = TRUE;
		}
		if ( gone ) {
			told_drop(i);
			continue;
		}
		i++;
	}
	return heard;
}

/*
 * Every channel of the program told how, into the object, with a link's
 * name and its own applist as the parts DT_TAIL_NAME and DT_TAIL_APPLIST
 * of the answer's tail, each as long as it is (DT_HOW_LINK_PUT with an
 * applist). A channel named in a form before the tail cannot be told the
 * applist: it is told nothing, and E_LIMIT. FALSE: none heard.
 */
EXPORT BOOL dt_prog_tell_link( CONST char *prog, UINT how, CONST TS_UUID *into,
			       CONST UB *name, CONST UB *applist )
{
	T_DTANS	*m;
	SZ	nl = 0, al = 0, cap, asz = 0;
	INT	i = 0;
	BOOL	heard = FALSE;
	ID	k;

	for ( ; name != NULL && name[nl] != 0; nl++ ) ;
	for ( ; applist != NULL && applist[al] != 0; al++ ) ;
	cap = dt_tail_room(nl) + dt_tail_room(al);
	m = (T_DTANS *)Kcalloc(1, sizeof(T_DTANS) + (SZ)cap);
	if ( m == NULL ) {
		return FALSE;
	}
	m->er = E_OK;
	m->how = how;
	if ( into != NULL ) m->into = *into;
	(void)dt_tail_put((UB *)( m + 1 ), &m->tailsz, cap, DT_TAIL_NAME, name, nl);
	if ( al > 0 ) (void)dt_tail_put((UB *)( m + 1 ), &m->tailsz, cap, DT_TAIL_APPLIST, applist, al);
	while ( i < ntold ) {
		if ( !same_id(told[i].prog, prog) || !told[i].tail ) {
			i++;
			continue;
		}
		k = ob_opn_obj(&told[i].ch, OB_OP_WRITE | OB_O_NOWAIT);
		if ( k <= 0 ) {
			told_drop(i);
			continue;
		}
		if ( ob_wri_rec(k, 0, 0, m, (SZ)sizeof(T_DTANS) + (SZ)m->tailsz, &asz) >= E_OK ) {
			knl_ob_post(&told[i].ch, 0, OB_E_CHANGE, NULL);
			heard = TRUE;
		}
		(void)ob_cls_obj(k);
		i++;
	}
	Kfree(m);
	return heard;
}

EXPORT BOOL dt_prog_tell_text( CONST char *prog, UINT how, CONST TS_UUID *into, CONST UB *text )
{
	return dt_prog_tell_data(prog, how, into, text, NULL, 0);
}

EXPORT BOOL dt_prog_tell( CONST char *prog, UINT how, CONST TS_UUID *into )
{
	return dt_prog_tell_text(prog, how, into, NULL);
}

LOCAL BOOL tool_heard( CONST char *prog )
{
	INT	i;

	for ( i = 0; i < ntold; i++ ) {
		if ( same_id(told[i].prog, prog) ) return TRUE;
	}
	return FALSE;
}

/* 屑実身操作 told that the counts of links may have moved */
EXPORT void dt_trash_tell( void )
{
	(void)dt_prog_tell(ID_TRASH, DT_HOW_TOOL_REMADE, NULL);
}

/* ---------------------------------------------------------------- starting them */

/*
 * A program run while a window of it stands for its program object: the
 * window brought to the front with the keys, and the program told, with
 * the object a tool was run on (仮身ネットワーク starts from it). TRUE
 * when it was so, *p_er what dt_prog_run answers; FALSE for a program
 * with no such window, to be started. A program other than a tool is
 * run again so only when run by itself or on its program object: run on
 * another object, it opens that one. 仮身ネットワーク starts only from an
 * object.
 */
EXPORT BOOL dt_prog_again( CONST DTPROG *p, CONST T_VOBJ *v, ER *p_er )
{
	INT	wid;
	UB	*data = NULL;
	SZ	dl = 0;

	if ( p == NULL || p->builtin ) {
		return FALSE;
	}
	if ( same_id(p->id, ID_NETWORK) && v == NULL ) {
		*p_er = E_PAR;
		return TRUE;
	}
	if ( !is_tool(p->id) && !p->once && v != NULL && ts_uuid_cmp(&v->target, &p->uuid) != 0 ) {
		return FALSE;
	}
	wid = wm_obj_showing(&p->uuid);
	if ( wid <= 0 ) {
		return FALSE;
	}
	/*
	 * the 固有データ it would have been started with (design 11.3), and,
	 * for a program of one window, the object it was run on
	 */
	if ( !dt_prog_data_take(&data, &dl) ) {
		data = dt_prog_data(p, ( v != NULL ) ? &v->target : NULL, ( v != NULL ) ? v->vapplist : NULL, &dl);
	}
	wm_raise(wid);
	(void)wm_focus(wid);
	wm_update();
	/* the 固有データ in the told answer's tail (DT_TAIL_DATA), as long as it is */
	(void)dt_prog_tell_data((CONST char *)p->id, DT_HOW_AGAIN,
				( v != NULL && ( is_tool(p->id)
					       || ( p->once && ts_uuid_cmp(&v->target, &p->uuid) != 0 ) ) )
				? &v->target : NULL, NULL, data, dl);
	if ( data != NULL ) Kfree(data);
	*p_er = E_OK;
	return TRUE;
}

LOCAL void tool_run( CONST char *id, DTWIN *from, CONST T_VOBJ *v )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)id);

	if ( p == NULL ) {
		(void)wm_msg_put("このプログラムはシステムにありません");
		return;
	}
	(void)dt_prog_run(from, p, v);
}

EXPORT void dt_trash_open( void )
{
	tool_run(ID_TRASH, NULL, NULL);
}

EXPORT void dt_winlist_open( void )
{
	tool_run(ID_WINLIST, NULL, NULL);
}

EXPORT void dt_search_open( DTWIN *from )
{
	tool_run(ID_SEARCH, from, NULL);
}

EXPORT void dt_network_open( DTWIN *from, CONST T_VOBJ *v )
{
	if ( v != NULL ) {
		tool_run(ID_NETWORK, from, v);
	}
}

/* ---------------------------------------------------------------- ウインドウ一覧 */

/*
 * The windows on the screen, nearest first, every one: a list that came
 * back full is asked for again in twice the room. How many, the list in
 * *p_wids (Kfree it).
 */
LOCAL INT wl_windows( INT **p_wids )
{
	INT	room = 64, n;
	INT	*w;

	for ( ;; ) {
		w = (INT *)Kmalloc(sizeof(INT) * (SZ)room);
		if ( w == NULL ) {
			*p_wids = NULL;
			return 0;
		}
		n = wm_list(w, room);
		if ( n < room ) break;
		Kfree(w);
		room *= 2;
	}
	*p_wids = w;
	return n;
}

/*
 * Whether a window is listed, and the object it shows: one of the
 * program's that says what object it shows; not a panel, not one of
 * the desktop's own parts,
 * not the asking process's, not a tool's own window
 */
LOCAL BOOL wl_listed( INT wid, ID asker, TS_UUID *p_id )
{
	DTWIN		*d = dt_win_of(wid);
	CONST DTPROG	*p;
	T_WMWIN		w;

	if ( d != NULL ) {
		return FALSE;			/* a part of the desktop's, which shows no object */
	}
	if ( !wm_obj_shown(wid, p_id) || wm_ref(wid, &w) < E_OK || ( w.attr & WM_ATTR_POPUP ) != 0 ) {
		return FALSE;
	}
	if ( asker > 0 && wm_obj_maker(wid) == asker ) {
		return FALSE;
	}
	p = dt_prog_of(p_id);
	return (BOOL)( p == NULL || !is_tool(p->id) );
}

LOCAL UW	wl_seen = 0;		/* the order of the windows when ウインドウ一覧 was last told */

/*
 * What the windows come to -- their order, the object each says it
 * shows and its title -- to see when the list would change: a program's
 * window says what it shows a moment after it is on the screen
 */
LOCAL UW wl_sign( void )
{
	INT	*wids, n, i, k;
	UW	h = 2166136261U;
	TS_UUID	id;
	char	t[WM_TITLE_MAX];

	n = wl_windows(&wids);
	for ( i = 0; i < n; i++ ) {
		h = ( h ^ (UW)wids[i] ) * 16777619U;
		if ( dt_win_of(wids[i]) == NULL && wm_obj_shown(wids[i], &id) ) {
			for ( k = 0; k < (INT)sizeof(id); k++ ) h = ( h ^ ((CONST UB *)&id)[k] ) * 16777619U;
		}
		if ( wm_title(wids[i], t, sizeof(t)) > 0 ) {
			for ( k = 0; t[k] != 0; k++ ) h = ( h ^ (UB)t[k] ) * 16777619U;
		}
	}
	if ( wids != NULL ) Kfree(wids);
	return h ^ (UW)n;
}

EXPORT void dt_winlist_poll( void )
{
	UW	s;

	if ( !tool_heard(ID_WINLIST) ) {
		return;
	}
	s = wl_sign();
	if ( s != wl_seen ) {
		wl_seen = s;
		(void)dt_prog_tell(ID_WINLIST, DT_HOW_TOOL_REMADE, NULL);
	}
}

/* DT_RQ_WIN_LIST: the window at place recno of those listed, or the one showing target to the front */
LOCAL ER wl_req( CONST T_DTREQ *rq, T_DTANS *an, ID asker )
{
	INT	*wids, n, i, k = 0, wid;
	TS_UUID	id;
	ER	er = E_NOEXS;

	if ( ( rq->flags & DT_F_OPEN ) != 0 ) {
		T_VOBJ	v = { .recno = 0 };

		if ( ( wid = wm_obj_showing(&rq->target) ) > 0 ) {
			wm_raise(wid);
			(void)wm_focus(wid);
			wm_update();
			return E_OK;
		}
		tad_vobj_init(&v);
		v.target = rq->target;
		v.recno = -1;
		v.frcol = v.chcol = v.tbcol = v.bgcol = TAD_COL_NONE;
		dt_open_vobj(NULL, &v);
		tad_vobj_clear(&v);
		return E_OK;
	}
	n = wl_windows(&wids);
	for ( i = 0; i < n; i++ ) {
		if ( !wl_listed(wids[i], asker, &id) ) continue;
		if ( k == rq->recno ) {
			char	t[WM_TITLE_MAX];

			an->into = id;
			if ( wm_title(wids[i], t, sizeof(t)) > 0 ) {
				er = dt_ans_cat(an, (CONST UB *)t, -1);
			} else {
				er = E_OK;
			}
		}
		k++;
	}
	if ( wids != NULL ) Kfree(wids);
	an->how = (UINT)k;
	if ( rq->recno < 0 ) er = E_OK;
	wl_seen = wl_sign();
	return er;
}

/* ---------------------------------------------------------------- 屑実身操作 */

/* What the system itself holds, every one: how many, the list in *p_ids (Kfree it) */
LOCAL INT all_roots( TS_UUID **p_ids )
{
	INT	room = 64, n;
	TS_UUID	*r;

	for ( ;; ) {
		r = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)room);
		if ( r == NULL ) {
			*p_ids = NULL;
			return 0;
		}
		n = dt_roots(r, room);
		if ( n < room ) break;
		Kfree(r);
		room *= 2;
	}
	*p_ids = r;
	return n;
}

/*
 * DT_RQ_PURGE: a 屑実身 thrown away for good, when the asking process may
 * delete it. A window of the desktop's showing it is closed first; what
 * it linked to loses those links, and may itself become a 屑実身.
 */
LOCAL ER purge_req( CONST T_DTREQ *rq, ID asker )
{
	TS_UUID	*roots;
	INT	nr, i;
	BOOL	root = FALSE;
	ER	er;

	nr = all_roots(&roots);
	for ( i = 0; i < nr; i++ ) {
		if ( ts_uuid_cmp(&roots[i], &rq->target) == 0 ) root = TRUE;
	}
	if ( roots != NULL ) Kfree(roots);
	if ( root || om_store_refs(&rq->target) != 0 ) {
		return E_OBJ;			/* linked to, or the system's: not a 屑実身 */
	}
	/*
	 * one the person may not delete (a program the administrator
	 * installed): only an administrator throws it away, the password
	 * asked for as for taking away a program
	 */
	if ( ( ( knl_ob_permit_on(&rq->target, knl_ob_crd_of(asker)) | knl_ob_granted(asker, &rq->target) )
	       & OB_OP_DELETE ) == 0 && !knl_ob_pid_admin(asker)
	  && ( er = dt_admin_ask(asker, (CONST UB *)"屑実身の削除", &rq->win) ) < E_OK ) {
		return er;
	}
	{
		/* a window showing it asked to close, as 〈閉じる〉 of its link asks */
		T_VOBJ	v = { .recno = 0 };

		tad_vobj_init(&v);
		v.target = rq->target;
		dt_close_vobj(&v);
		tad_vobj_clear(&v);
	}
	return om_store_delete(&rq->target);
}

/* DT_RQ_ROOTS: what the system holds at place recno, how many there are in how */
LOCAL ER roots_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	TS_UUID	*roots;
	INT	nr = all_roots(&roots);
	ER	er = E_OK;

	if ( roots == NULL ) {
		return E_NOMEM;
	}
	an->how = (UINT)nr;
	if ( rq->recno >= nr ) {
		er = E_NOEXS;
	} else if ( rq->recno >= 0 ) {
		an->into = roots[rq->recno];
	}
	Kfree(roots);
	return er;
}

/* DT_RQ_RECOUNT: every count made again from the records, "TOTAL<tab>CHANGED" in text */
LOCAL ER recount_req( T_DTANS *an )
{
	TS_UUID	*roots;
	INT	nr = all_roots(&roots), total = 0, changed = 0;
	ER	er;

	er = ( roots != NULL ) ? om_store_recount(roots, nr, &total, &changed) : E_NOMEM;
	if ( roots != NULL ) Kfree(roots);
	if ( er < E_OK ) {
		return er;
	}
	(void)dt_ans_num(an, total);
	(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
	(void)dt_ans_num(an, changed);
	tm_printf((UB *)"desktop: counts made again, %d objects, %d put right\n", total, changed);
	return E_OK;
}

/* ---------------------------------------------------------------- what the tools ask */

EXPORT ER dt_tool_req( CONST T_DTREQ *rq, T_DTANS *an, ID asker )
{
	switch ( rq->req ) {
	case DT_RQ_TELL:	return tool_listen(rq, asker);
	case DT_RQ_ROOTS:	return roots_req(rq, an);
	case DT_RQ_PURGE:	return purge_req(rq, asker);
	case DT_RQ_RECOUNT:	return recount_req(an);
	case DT_RQ_WIN_LIST:	return wl_req(rq, an, asker);
	default:		return E_PAR;
	}
}
