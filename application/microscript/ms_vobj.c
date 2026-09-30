/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_vobj.c
 *	Micro Script: virtual objects opened, closed and waited for
 *
 *	VOPEN asks the desktop (its channel DT_REQ_NAME) to open an object
 *	as a double click opens it, or to start an accessory by its name.
 *	When a process was started for it, the answer says which, and that
 *	is the segment's PID. The windows an object is open in are found by
 *	what each window says it shows; VCLOSE deletes them, which asks the
 *	program behind each to close, and VWAIT waits until none is left
 *	and the process started for it has ended.
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ts/dtreq.h>
#include <ts/json.h>

#define OPENED_MAX	64
#define ANSWER_MS	5000

typedef struct {
	BOOL	used;
	BOOL	tool;		/* an accessory, by name */
	TS_UUID	target;		/* the object, when not */
	TS_UUID	proc;		/* the process started; 0 none */
	char	name[64];
} OPENED;

static OPENED	opened[OPENED_MAX];
static TS_UUID	ans_ch;
static ID	ans_key;
static UINT	seq;

static BOOL uuid_zero( const TS_UUID *u )
{
	static const TS_UUID	zero;

	return (BOOL)( memcmp(u, &zero, sizeof(zero)) == 0 );
}

/* A process's number: its object's name is "process" and the number */
INT ms_pid_of( const TS_UUID *pu )
{
	T_OBREF		r;
	const char	*q;

	if ( ob_ref_obj(pu, &r) < E_OK ) return -1;
	q = strchr((const char *)r.name, ' ');
	return ( q != NULL ) ? atoi(q + 1) : -1;
}

static void pause_ms( UD ms )
{
	if ( rt.cur != NULL ) ms_yield(ms_now_ms() + ms);
	else (void)tk_dly_tsk((TMO)ms);
}

/* A request to the desktop, and the process its answer names */
static ER ask( T_DTREQ *rq, TS_UUID *p_proc )
{
	TS_UUID	d;
	T_DTANS	an;
	T_OBCRE	c;
	SZ	asz = 0;
	ID	k;
	ER	er;
	UD	until;

	memset(p_proc, 0, sizeof(*p_proc));
	if ( ob_fnd_nam((const UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	if ( ans_key <= 0 ) {
		memset(&c, 0, sizeof(c));
		c.type = OB_T_CHANNEL;
		if ( ob_cre_obj(&c, &ans_ch) < E_OK ) return E_NOMEM;
		ans_key = ob_opn_obj(&ans_ch, OB_OP_READ | OB_O_NOWAIT);
		if ( ans_key <= 0 ) { (void)ob_del_obj(&ans_ch); ans_key = 0; return E_OBJ; }
	}
	rq->seq = ++seq;
	rq->reply = ans_ch;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	if ( k <= 0 ) return E_OBJ;
	er = ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz);
	ob_cls_obj(k);
	if ( er < E_OK ) return er;
	until = ms_now_ms() + ANSWER_MS;
	while ( ms_now_ms() < until ) {
		while ( ob_rea_rec(ans_key, 0, 0, &an, sizeof(an), &asz) >= E_OK ) {
			if ( asz != (SZ)sizeof(an) || an.seq != rq->seq ) continue;
			*p_proc = an.proc;
			return an.er;
		}
		pause_ms(20);
	}
	return E_TMOUT;
}

static OPENED *remember( BOOL tool, const TS_UUID *target, const char *name, const TS_UUID *proc )
{
	INT	i, free_at = -1;

	for ( i = 0; i < OPENED_MAX; i++ ) {
		OPENED	*o = &opened[i];

		if ( !o->used ) { if ( free_at < 0 ) free_at = i; continue; }
		if ( o->tool == tool && ( tool ? strcmp(o->name, name) == 0
					       : memcmp(&o->target, target, sizeof(TS_UUID)) == 0 ) ) {
			if ( !uuid_zero(proc) ) o->proc = *proc;
			return o;
		}
	}
	if ( free_at < 0 ) return NULL;
	memset(&opened[free_at], 0, sizeof(OPENED));
	opened[free_at].used = TRUE;
	opened[free_at].tool = tool;
	if ( target != NULL ) opened[free_at].target = *target;
	if ( name != NULL ) strncpy(opened[free_at].name, name, sizeof(opened[free_at].name) - 1);
	opened[free_at].proc = *proc;
	return &opened[free_at];
}

/* VOPEN of an object; the process started for it (its number), or -1 */
ER ms_vopen_obj( const TS_UUID *target, INT *p_pid )
{
	T_DTREQ	rq;
	TS_UUID	pu;
	ER	er;

	*p_pid = -1;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = *target;
	rq.recno = -1;
	er = ask(&rq, &pu);
	if ( er < E_OK ) return er;
	(void)remember(FALSE, target, NULL, &pu);
	if ( !uuid_zero(&pu) ) *p_pid = ms_pid_of(&pu);
	return E_OK;
}

/* VOPEN of an accessory by its name in the 小物 menu */
ER ms_vopen_tool( const char *name, INT *p_pid )
{
	T_DTREQ	rq;
	TS_UUID	pu;
	ER	er;

	*p_pid = -1;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_TOOL;
	strncpy((char *)rq.name, name, sizeof(rq.name) - 1);
	er = ask(&rq, &pu);
	if ( er < E_OK ) return er;
	(void)remember(TRUE, NULL, name, &pu);
	if ( !uuid_zero(&pu) ) *p_pid = ms_pid_of(&pu);
	return E_OK;
}

/* The windows that say they show the object */
static INT windows_showing( const TS_UUID *target, TS_UUID *out, INT max )
{
	TS_UUID	list[64];
	INT	cnt = 0, i, n = 0;
	char	j[512], want[40], got[40];

	if ( ob_lst_obj(OB_T_WINDOW, OB_S_WINDOW, NULL, list, 64, &cnt) < E_OK ) return 0;
	ms_uuid_str(target, want);
	for ( i = 0; i < cnt && n < max; i++ ) {
		ID	k = ob_opn_obj(&list[i], OB_OP_ATRRD);
		SZ	asz = 0;
		T_JSON	root, tf, w;

		if ( k <= 0 ) continue;
		if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) >= E_OK
		  && js_parse((const UB *)j, (INT)asz, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "window", &w) >= E_OK
		  && js_get_str(&w, "shows", (UB *)got, sizeof(got)) > 0
		  && strcasecmp(got, want) == 0 ) {
			out[n++] = list[i];
		}
		ob_cls_obj(k);
	}
	return n;
}

static BOOL proc_alive( const TS_UUID *pu )
{
	T_OBREF	r;

	return (BOOL)( !uuid_zero(pu) && ob_ref_obj(pu, &r) >= E_OK );
}

/* Whether what VOPEN opened, or anything open on the object, is still there */
static BOOL still_open( const OPENED *o )
{
	TS_UUID	w[4];

	if ( proc_alive(&o->proc) ) return TRUE;
	return (BOOL)( !o->tool && windows_showing(&o->target, w, 4) > 0 );
}

static void close_one( const OPENED *o )
{
	TS_UUID	w[16];
	INT	n, i;

	if ( !o->tool ) {
		n = windows_showing(&o->target, w, 16);
		for ( i = 0; i < n; i++ ) (void)ob_del_obj(&w[i]);
	}
	/* an accessory has only its process to end it by */
	if ( o->tool && proc_alive(&o->proc) ) (void)ob_del_obj(&o->proc);
}

/*
 * The objects and accessories a VCLOSE or VWAIT names: `targets` and
 * `names`. None named: every object the figure points at, and every
 * accessory VOPEN started.
 */
static INT gather( const TS_UUID *targets, INT nt, const char (*names)[64], INT nn, OPENED *out, INT max )
{
	INT	i, k, n = 0;
	TS_UUID	pu;

	memset(&pu, 0, sizeof(pu));
	if ( nt == 0 && nn == 0 ) {
		for ( i = 0; i < ms_fig.nlink && n < max; i++ ) {
			memset(&out[n], 0, sizeof(OPENED));
			out[n].target = ms_fig.link[i].target;
			n++;
		}
		for ( i = 0; i < OPENED_MAX && n < max; i++ ) {
			if ( !opened[i].used ) continue;
			if ( !opened[i].tool ) {
				/* its process, with the object already listed */
				for ( k = 0; k < n; k++ ) {
					if ( !out[k].tool && memcmp(&out[k].target, &opened[i].target, sizeof(TS_UUID)) == 0 ) break;
				}
				if ( k < n ) { out[k].proc = opened[i].proc; continue; }
			}
			out[n++] = opened[i];
		}
		return n;
	}
	for ( i = 0; i < nt && n < max; i++ ) {
		memset(&out[n], 0, sizeof(OPENED));
		out[n].target = targets[i];
		for ( k = 0; k < OPENED_MAX; k++ ) {
			if ( opened[k].used && !opened[k].tool
			  && memcmp(&opened[k].target, &targets[i], sizeof(TS_UUID)) == 0 ) out[n].proc = opened[k].proc;
		}
		n++;
	}
	for ( i = 0; i < nn && n < max; i++ ) {
		for ( k = 0; k < OPENED_MAX; k++ ) {
			if ( opened[k].used && opened[k].tool && strcmp(opened[k].name, names[i]) == 0 ) {
				out[n++] = opened[k];
				break;
			}
		}
	}
	return n;
}

void ms_vclose( const TS_UUID *targets, INT nt, const char (*names)[64], INT nn )
{
	OPENED	*list = ms_alloc(sizeof(OPENED) * 128);
	INT	n = gather(targets, nt, names, nn, list, 128), i;

	for ( i = 0; i < n; i++ ) close_one(&list[i]);
	ms_free(list);
}

/* Whether any of them is still open */
BOOL ms_vopen_any( const TS_UUID *targets, INT nt, const char (*names)[64], INT nn )
{
	OPENED	*list = ms_alloc(sizeof(OPENED) * 128);
	INT	n = gather(targets, nt, names, nn, list, 128), i;
	BOOL	any = FALSE;

	for ( i = 0; i < n && !any; i++ ) any = still_open(&list[i]);
	ms_free(list);
	return any;
}

/* Before the program ends: the channel the answers came to */
void ms_vobj_end( void )
{
	if ( ans_key > 0 ) {
		ob_cls_obj(ans_key);
		(void)ob_del_obj(&ans_ch);
		ans_key = 0;
	}
}
