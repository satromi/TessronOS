/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtacc.c
 *	The order of the accessories and which of them 小物 lists (design
 *	11.2.1)
 *
 *	The object 小物の設定 (SYSDEF_CONF_ACC) keeps in its record 1 a line
 *	for each accessory, in the order the 小物箱 and the menu show them:
 *	  ACC	clock	show
 *	  ACC	besfc	hide
 *	An accessory is named by its program's id, which stays when a package
 *	updates it even if its object changes. A line whose id no accessory
 *	has now is passed over. An accessory with no line comes after the
 *	others in the order of the program box, shown as its metadata's
 *	"menu" says, so a new one is at the end until the person moves it.
 *	With no object, or no line, that is every accessory. Nothing is
 *	written until the person changes something.
 *
 *	The 小物箱 is 仮身一覧 running as a process, opened as the 小物箱
 *	(application/accbox). It reads the order here one accessory at a
 *	time (DT_RQ_ACC_LIST) and lays the list out itself, as the system's
 *	lists are laid out (lib/libtv/tv_list.c), and asks the changes the
 *	person makes (DT_RQ_ACC_SET). Every 小物箱 that asked to be told
 *	(DT_RQ_TELL for accessory-box) is told when the list changes
 *	(dt_acc_boxes_tell).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/mn.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "desktop.h"

#define ACC_KEY		"ACC"
#define ACC_RT		15		/* a record of text, as the other settings have */
#define ACC_HEAD	"# 小物の順番と、小物メニューに出すか(show)出さないか(hide)\n"

typedef struct {
	CONST DTPROG	*p;
	BOOL		show;
} ACCENT;

/* The settings' object: FALSE when the system has none */
LOCAL BOOL acc_obj( TS_UUID *u )
{
	T_OBREF	r;

	return (BOOL)( knl_sys_find(SYSROLE_CONF_ACC, SYSDEF_CONF_ACC, u) && ob_ref_obj(u, &r) >= E_OK );
}

LOCAL BOOL acc_taken( CONST ACCENT *e, INT n, CONST DTPROG *p )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( e[i].p == p ) return TRUE;
	}
	return FALSE;
}

/*
 * Every accessory in its order, in a table of its own (*p_e, Kfree it):
 * how many. *p_lines says how many lines the object had.
 */
LOCAL INT acc_read( ACCENT **p_e, INT *p_lines )
{
	INT		all = dt_prog_count(DT_PK_ACCESSORY), n = 0, i, nth = 0;
	ACCENT		*e = (ACCENT *)Kmalloc(sizeof(ACCENT) * (SZ)( all + 1 ));
	CONST DTPROG	**list = (CONST DTPROG **)Kmalloc(sizeof(DTPROG *) * (SZ)( all + 1 ));
	UB		*text = NULL;
	SZ		size = 0;
	TS_UUID		u;

	*p_e = e;
	if ( p_lines != NULL ) *p_lines = 0;
	if ( e == NULL || list == NULL ) {
		if ( list != NULL ) Kfree((void *)list);
		return 0;
	}
	if ( acc_obj(&u) ) {
		text = om_obj_record(&u, 1, &size);
	}
	if ( text != NULL ) {
		CONST UB	*val, *w;
		INT		vlen, wlen;

		for ( ; cf_find(text, (INT)size, ACC_KEY, nth, &val, &vlen); nth++ ) {
			UB		id[DT_PROG_ID];
			CONST DTPROG	*p;
			BOOL		show = TRUE;

			if ( !cf_wfind(val, vlen, 0, &w, &wlen) || wlen <= 0 || wlen >= DT_PROG_ID ) {
				continue;
			}
			for ( i = 0; i < wlen; i++ ) id[i] = w[i];
			id[wlen] = 0;
			if ( cf_wfind(val, vlen, 1, &w, &wlen) && wlen == 4 && w[0] == 'h' ) {
				show = FALSE;
			}
			p = dt_prog_find(id);
			if ( p != NULL && p->kind == DT_PK_ACCESSORY && n < all && !acc_taken(e, n, p) ) {
				e[n].p = p;
				e[n].show = show;
				n++;
			}
		}
		Kfree(text);
	}
	if ( p_lines != NULL ) *p_lines = nth;
	all = dt_prog_list(DT_PK_ACCESSORY, list, all);
	for ( i = 0; i < all; i++ ) {
		if ( !acc_taken(e, n, list[i]) ) {
			e[n].p = list[i];
			e[n].show = list[i]->menu;
			n++;
		}
	}
	Kfree((void *)list);
	return n;
}

LOCAL SZ acc_put( UB *b, SZ at, SZ max, CONST char *s )
{
	while ( *s != 0 && at < max ) {
		b[at++] = (UB)*s++;
	}
	return at;
}

/* The table written to the object */
LOCAL ER acc_write( CONST ACCENT *e, INT n )
{
	TS_UUID	u;
	SZ	max = (SZ)sizeof(ACC_HEAD) + (SZ)( n + 1 ) * ( DT_PROG_ID + 16 ), at = 0;
	UB	*b;
	INT	i;
	ER	er;

	if ( !acc_obj(&u) ) {
		return E_NOEXS;
	}
	b = (UB *)Kmalloc(max);
	if ( b == NULL ) {
		return E_NOMEM;
	}
	at = acc_put(b, at, max, ACC_HEAD);
	for ( i = 0; i < n; i++ ) {
		at = acc_put(b, at, max, ACC_KEY "\t");
		at = acc_put(b, at, max, (CONST char *)e[i].p->id);
		at = acc_put(b, at, max, e[i].show ? "\tshow\n" : "\thide\n");
	}
	er = om_obj_record_put(&u, 1, ACC_RT, 0, b, at);
	Kfree(b);
	return er;
}

/* A change the person made: kept, and every 小物箱 shows it */
LOCAL void acc_changed( CONST ACCENT *e, INT n )
{
	ER	er = acc_write(e, n);

	if ( er < E_OK ) {
		(void)wm_msg_put("小物の設定に書き込めませんでした");
		tm_printf((UB *)"accbox: not written (%d)\n", (INT)er);
		return;
	}
	dt_acc_boxes_tell();
}

LOCAL INT acc_index( CONST ACCENT *e, INT n, CONST TS_UUID *target )
{
	INT	i;

	for ( i = 0; target != NULL && i < n; i++ ) {
		if ( ts_uuid_cmp(&e[i].p->uuid, target) == 0 ) {
			return i;
		}
	}
	return -1;
}

/* The entry at 'from' put at 'to', those between closing up */
LOCAL void acc_shift( ACCENT *e, INT from, INT to )
{
	ACCENT	m = e[from];
	INT	i;

	for ( i = from; i < to; i++ ) e[i] = e[i + 1];
	for ( i = from; i > to; i-- ) e[i] = e[i - 1];
	e[to] = m;
}

/* ---------------------------------------------------------------- what the others ask */

EXPORT INT dt_acc_menu_list( CONST DTPROG ***p_acc )
{
	ACCENT		*e;
	INT		n = acc_read(&e, NULL), i, k = 0;
	CONST DTPROG	**out = (CONST DTPROG **)Kmalloc(sizeof(DTPROG *) * (SZ)( n + 1 ));

	*p_acc = out;
	for ( i = 0; out != NULL && i < n; i++ ) {
		if ( e[i].show ) out[k++] = e[i].p;
	}
	if ( e != NULL ) Kfree(e);
	return k;
}

/* The lines of what is no longer an accessory taken out, when the object has lines at all */
EXPORT void dt_acc_sync( void )
{
	ACCENT	*e;
	INT	lines, n = acc_read(&e, &lines);

	if ( e != NULL && lines > 0 ) {
		(void)acc_write(e, n);
	}
	if ( e != NULL ) Kfree(e);
}

#if USE_UITEST
/*
 * What a test looks at: where the program of an id stands among the
 * accessories (0 up, -1 not among them), of how many, and whether 小物
 * lists it.
 */
EXPORT INT dt_acc_pos( CONST UB *id, INT *p_n, BOOL *p_show )
{
	ACCENT		*e;
	INT		n = acc_read(&e, NULL), at = -1, i;
	CONST DTPROG	*p = dt_prog_find(id);

	for ( i = 0; e != NULL && i < n; i++ ) {
		if ( e[i].p == p ) {
			at = i;
			*p_show = e[i].show;
		}
	}
	*p_n = n;
	if ( e != NULL ) Kfree(e);
	return at;
}
#endif

/* ---------------------------------------------------------------- the person's acts */

/* An accessory carried within the 小物箱 and let go before another (NULL: after them all) */
EXPORT void dt_acc_place( CONST TS_UUID *target, CONST TS_UUID *before )
{
	ACCENT	*e;
	INT	n = acc_read(&e, NULL), from, to;

	from = ( e != NULL ) ? acc_index(e, n, target) : -1;
	if ( from >= 0 ) {
		to = ( before != NULL ) ? acc_index(e, n, before) : n;
		if ( to < 0 ) to = n;
		if ( to > from ) to--;
		if ( to != from ) {
			acc_shift(e, from, to);
			acc_changed(e, n);
		}
	}
	if ( e != NULL ) Kfree(e);
}

/* A code of 小物管理 done to the accessory target: E_PAR for no such code */
LOCAL ER acc_do( CONST UB *code, CONST TS_UUID *target )
{
	ACCENT	*e;
	INT	n, at, to;
	BOOL	top = (BOOL)( knl_strcmp((CONST char *)code, "acc.top") == 0 );
	BOOL	up = (BOOL)( knl_strcmp((CONST char *)code, "acc.up") == 0 );
	BOOL	down = (BOOL)( knl_strcmp((CONST char *)code, "acc.down") == 0 );
	BOOL	bottom = (BOOL)( knl_strcmp((CONST char *)code, "acc.bottom") == 0 );
	BOOL	show = (BOOL)( knl_strcmp((CONST char *)code, "acc.show") == 0 );
	BOOL	hide = (BOOL)( knl_strcmp((CONST char *)code, "acc.hide") == 0 );

	if ( !top && !up && !down && !bottom && !show && !hide ) {
		return E_PAR;
	}
	n = acc_read(&e, NULL);
	at = ( e != NULL ) ? acc_index(e, n, target) : -1;
	if ( at >= 0 ) {
		if ( show || hide ) {
			e[at].show = show;
			acc_changed(e, n);
		} else {
			to = top ? 0 : up ? at - 1 : down ? at + 1 : n - 1;
			if ( to >= 0 && to < n && to != at ) {
				acc_shift(e, at, to);
				acc_changed(e, n);
			}
		}
	}
	if ( e != NULL ) Kfree(e);
	return ( at >= 0 ) ? E_OK : E_NOEXS;
}

/* ---------------------------------------------------------------- the 小物箱s */

/*
 * The order as it was read for the 小物箱 that reads it now, one
 * accessory at a time: read again when the first is asked for, and
 * when it changes
 */
LOCAL ACCENT	*acc_e = NULL;
LOCAL INT	acc_en = 0;

LOCAL void acc_list_drop( void )
{
	if ( acc_e != NULL ) Kfree(acc_e);
	acc_e = NULL;
	acc_en = 0;
}

/* Every 小物箱 told that the list changed (DT_HOW_ACC), to make it again */
EXPORT void dt_acc_boxes_tell( void )
{
	acc_list_drop();
	(void)dt_prog_tell("accessory-box", DT_HOW_ACC, NULL);
}

/*
 * DT_RQ_ACC_LIST: the accessory at place recno of those in the store,
 * its name and whether 小物 lists it (dtreq.h); DT_RQ_ACC_SET: a change
 * of the person's
 */
EXPORT ER dt_acc_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	INT	i, k, n;
	UB	nm[TAD_NAME_MAX];

	if ( rq->req == DT_RQ_ACC_SET ) {
		UB	code[sizeof(rq->name)];
		TS_UUID	before;
		BOOL	has_before;

		for ( i = 0; i < (INT)sizeof(code) - 1 && rq->name[i] != 0; i++ ) code[i] = rq->name[i];
		code[i] = 0;
		if ( knl_strcmp((CONST char *)code, "acc.place") != 0 ) {
			return acc_do(code, &rq->target);
		}
		has_before = (BOOL)( rq->text[0] != 0 && ts_str_to_uuid((CONST char *)rq->text, &before) >= E_OK );
		dt_acc_place(&rq->target, has_before ? &before : NULL);
		return E_OK;
	}
	if ( acc_e == NULL || rq->recno == 0 ) {
		acc_list_drop();
		n = acc_read(&acc_e, NULL);
		/* those of the store alone, as a list of the store's objects shows them */
		for ( i = k = 0; acc_e != NULL && i < n; i++ ) {
			if ( om_store_name(&acc_e[i].p->uuid, nm, TAD_NAME_MAX) >= 0 ) acc_e[k++] = acc_e[i];
		}
		acc_en = k;
	}
	if ( acc_e == NULL ) {
		return E_NOMEM;
	}
	an->how = (UINT)acc_en;
	if ( rq->recno < 0 ) {
		return E_OK;
	}
	if ( rq->recno >= acc_en ) {
		return E_NOEXS;
	}
	an->into = acc_e[rq->recno].p->uuid;
	an->flags = acc_e[rq->recno].show ? 0 : DT_F_HIDDEN;
	if ( om_store_name(&an->into, nm, TAD_NAME_MAX) < 0 ) nm[0] = 0;
	for ( k = 0; nm[k] != 0 && k < DT_TEXT_MAX - 1; k++ ) an->text[k] = nm[k];
	an->text[k] = 0;
	return E_OK;
}
