/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obchan.c
 *	The channel manager: queues of messages between processes as
 *	objects (design 18.8)
 *
 *	Writing record 0 puts one message on the queue, reading it takes
 *	one off; a full queue holds the writer back and an empty one the
 *	reader, unless the key was opened not to wait. Deleting the channel
 *	wakes whoever waits on it with E_DLT. A channel made with a name
 *	is found by it, among the same names as the global names.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "obj.h"

#define OBC_MAX		8192

typedef struct {
	SZ	size;
	ID	from;			/* the process that wrote it, 0 the kernel */
	UB	body[OB_CH_MSG_MAX];
} OBCMSG;

typedef struct {
	TS_UUID	uuid;
	UB	name[OB_NAME_MAX];
	T_OBPRT	prt;
	OBCMSG	*q;
	INT	head, tail, count;
	ID	sem_free;		/* room on the queue */
	ID	sem_used;		/* messages waiting */
	INT	refcnt;
	INT	opens;
	BOOL	doomed;
	ID	last_from;		/* who wrote the message read last */
} OBCH;

LOCAL OBCH	*obc[OBC_MAX];
LOCAL ID	obc_mtx = 0;

#define LOCK()		tk_loc_mtx(obc_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obc_mtx)

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == b[i] );
}

LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; i < OBC_MAX; i++ ) {
		if ( obc[i] != NULL && !obc[i]->doomed
		  && ts_uuid_cmp(&obc[i]->uuid, uuid) == 0 ) {
			return i;
		}
	}
	return -1;
}

LOCAL BOOL name_taken( CONST UB *name )
{
	INT	i;

	for ( i = 0; name[0] != 0 && i < OBC_MAX; i++ ) {
		if ( obc[i] != NULL && !obc[i]->doomed && same_name(obc[i]->name, name) ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT BOOL knl_obchan_name_taken( CONST UB *name )
{
	BOOL	t;

	LOCK();
	t = name_taken(name);
	UNLOCK();

	return t;
}

LOCAL void ch_free( INT i )
{
	OBCH	*c = obc[i];

	if ( c->sem_free > 0 ) tk_del_sem(c->sem_free);
	if ( c->sem_used > 0 ) tk_del_sem(c->sem_used);
	if ( c->q != NULL ) Kfree(c->q);
	Kfree(c);
	obc[i] = NULL;
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obc_find( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obc_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		knl_memcpy(r->name, obc[i]->name, OB_NAME_MAX);
		r->flags = OB_F_VOLATILE | ( obc[i]->name[0] != 0 ? OB_F_GLOBAL : 0 );
		r->refcnt = obc[i]->refcnt;
		r->nrec = 1;
		r->size = (UD)obc[i]->count;	/* messages waiting */
	}
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obc_prot( CONST TS_UUID *uuid, T_OBPRT *prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *prt = obc[i]->prt;
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obc_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) obc[i]->prt = *prt;
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obc_create( CONST T_OBCRE *cr, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	OBCH	*c;
	T_CSEM	csem;
	INT	i, slot = -1, n;
	ER	er;

	c = (OBCH *)Kmalloc(sizeof(OBCH));
	if ( c == NULL ) {
		return E_NOMEM;
	}
	knl_memset(c, 0, sizeof(*c));
	er = ts_gen_uuid(&c->uuid);
	if ( er < E_OK ) {
		Kfree(c);
		return er;
	}
	for ( n = 0; cr->name != NULL && cr->name[n] != 0 && n < OB_NAME_MAX - 1; n++ ) {
		c->name[n] = cr->name[n];
	}
	c->prt = *prt;
	c->q = (OBCMSG *)Kmalloc(sizeof(OBCMSG) * OB_CH_QLEN);
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = OB_CH_QLEN;
	csem.maxsem = OB_CH_QLEN;
	c->sem_free = tk_cre_sem(&csem);
	csem.isemcnt = 0;
	c->sem_used = tk_cre_sem(&csem);
	if ( c->q == NULL || c->sem_free <= 0 || c->sem_used <= 0 ) {
		er = ( c->q == NULL ) ? E_NOMEM : E_LIMIT;
		goto err;
	}

	/* a named channel is found by its name, which no global name may share */
	if ( c->name[0] != 0 && knl_obmem_name_taken(c->name) ) {
		er = E_OBJ;
		goto err;
	}
	LOCK();
	if ( c->name[0] != 0 && name_taken(c->name) ) {
		UNLOCK();
		er = E_OBJ;
		goto err;
	}
	for ( i = 0; i < OBC_MAX; i++ ) {
		if ( obc[i] == NULL ) {
			slot = i;
			obc[i] = c;
			break;
		}
	}
	UNLOCK();
	if ( slot < 0 ) {
		er = E_LIMIT;
		goto err;
	}
	*p_uuid = c->uuid;
	return E_OK;

    err:
	if ( c->sem_free > 0 ) tk_del_sem(c->sem_free);
	if ( c->sem_used > 0 ) tk_del_sem(c->sem_used);
	if ( c->q != NULL ) Kfree(c->q);
	Kfree(c);
	return er;
}

/*
 * Deleted: whoever waits is woken with E_DLT now; the memory goes when
 * the last handle is closed, since a woken waiter still holds one.
 */
LOCAL ER obc_remove( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		obc[i]->doomed = TRUE;
		tk_del_sem(obc[i]->sem_free);
		tk_del_sem(obc[i]->sem_used);
		obc[i]->sem_free = obc[i]->sem_used = 0;
		if ( obc[i]->opens <= 0 ) {
			ch_free(i);
		}
	}
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL INT obc_open( CONST TS_UUID *uuid, UINT ops )
{
	INT	i;

	(void)ops;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) obc[i]->opens++;
	UNLOCK();

	return ( i >= 0 ) ? i + 1 : E_NOEXS;
}

LOCAL ER obc_close( INT h )
{
	OBCH	*c = NULL;

	LOCK();
	if ( h >= 1 && h <= OBC_MAX && obc[h - 1] != NULL ) {
		c = obc[h - 1];
		if ( --c->opens <= 0 && c->doomed ) {
			ch_free(h - 1);
		}
	}
	UNLOCK();

	return ( c != NULL ) ? E_OK : E_ID;
}

LOCAL OBCH *ch_of( INT h, ID *p_free, ID *p_used )
{
	OBCH	*c = NULL;

	LOCK();
	if ( h >= 1 && h <= OBC_MAX && obc[h - 1] != NULL && !obc[h - 1]->doomed ) {
		c = obc[h - 1];
		*p_free = c->sem_free;
		*p_used = c->sem_used;
	}
	UNLOCK();

	return c;
}

/*
 * A wait for room ('room') or for a message on the channel of handle h,
 * a slice at a time: between slices a process's call looks whether the
 * process is to end, and comes out with E_DISWAI to end on its way back
 * (knl_prc_call_enter). The kernel's own callers wait in one piece,
 * not woken for nothing. The channel is looked up again for each
 * slice: deleted between two, its semaphores are gone and their
 * numbers may be another's (E_DLT). *p_c, *p_sf and *p_su are the
 * channel and its semaphores.
 */
#define OBC_SLICE_MS	200

LOCAL ER ch_wait( INT h, BOOL room, BOOL nowait, OBCH **p_c, ID *p_sf, ID *p_su )
{
	TMO	tmo = nowait ? TMO_POL : ( ts_get_pid() > 0 ) ? OBC_SLICE_MS : TMO_FEVR;
	ER	er;

	for ( ;; ) {
		*p_c = ch_of(h, p_sf, p_su);
		if ( *p_c == NULL ) {
			return E_DLT;
		}
		er = tk_wai_sem(room ? *p_sf : *p_su, 1, tmo);
		if ( er != E_TMOUT || tmo != OBC_SLICE_MS ) {
			return er;		/* E_TMOUT only for nowait, or E_DLT: the channel went */
		}
		if ( knl_prc_ending() ) {
			return E_DISWAI;
		}
	}
}

/* One message taken off */
LOCAL ER obc_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBCH	*c;
	ID	sf, su;
	SZ	n;
	ER	er;

	if ( recno != 0 || off != 0 ) {
		return E_PAR;
	}
	er = ch_wait(h, FALSE, nowait, &c, &sf, &su);
	if ( er < E_OK ) {
		return er;
	}
	LOCK();
	n = c->q[c->head].size;
	if ( n > size ) n = size;
	knl_memcpy(buf, c->q[c->head].body, (INT)n);
	c->last_from = c->q[c->head].from;
	c->head = ( c->head + 1 ) % OB_CH_QLEN;
	c->count--;
	UNLOCK();
	tk_sig_sem(sf, 1);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/* One message put on */
LOCAL ER obc_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBCH	*c;
	ID	sf, su;
	ER	er;

	if ( recno != 0 || off != 0 || size < 0 || size > OB_CH_MSG_MAX ) {
		return E_PAR;
	}
	er = ch_wait(h, TRUE, nowait, &c, &sf, &su);
	if ( er < E_OK ) {
		return er;
	}
	LOCK();
	c->q[c->tail].size = size;
	c->q[c->tail].from = ts_get_pid();
	knl_memcpy(c->q[c->tail].body, buf, (INT)size);
	c->tail = ( c->tail + 1 ) % OB_CH_QLEN;
	c->count++;
	UNLOCK();
	tk_sig_sem(su, 1);
	if ( p_asize != NULL ) {
		*p_asize = size;
	}
	return E_OK;
}

LOCAL ER obc_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBCH	*c;
	ID	sf, su;

	c = ch_of(h, &sf, &su);
	if ( c == NULL ) {
		return E_ID;
	}
	if ( n > 0 ) {
		buf[0].recno = 0;
		buf[0].rt = OB_RT_SYSDATA;
		buf[0].sub = 0;
		buf[0].size = (UD)c->count;
	}
	*p_cnt = ( n > 0 ) ? 1 : 0;

	return E_OK;
}

/* Its attributes are its name */
LOCAL ER obc_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBCH	*c;
	ID	sf, su;
	INT	n;

	c = ch_of(h, &sf, &su);
	if ( c == NULL ) {
		return E_ID;
	}
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":");
	n = knl_oj_put_str(json, n, (INT)size, c->name);
	n = knl_oj_put(json, n, (INT)size, "}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obc_lnk( CONST TS_UUID *uuid, INT delta )
{
	INT	i;
	ER	er = E_OK;

	LOCK();
	i = index_of(uuid);
	if ( i < 0 ) {
		er = E_NOEXS;
	} else if ( obc[i]->refcnt + delta < 0 ) {
		er = E_OBJ;
	} else {
		obc[i]->refcnt += delta;
	}
	UNLOCK();

	return er;
}

LOCAL ER obc_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	LOCK();
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < OBC_MAX; i++ ) {
			if ( obc[i] == NULL || obc[i]->doomed ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&obc[i]->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&obc[i]->uuid, from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&obc[i]->uuid, &obc[best]->uuid) < 0 ) {
				best = i;
			}
		}
		if ( best < 0 ) break;
		buf[cnt++] = obc[best]->uuid;
	}
	UNLOCK();
	*p_cnt = cnt;

	return E_OK;
}

LOCAL ER obc_fndnam( CONST UB *name, TS_UUID *p_uuid )
{
	INT	i;
	ER	er = E_NOEXS;

	LOCK();
	for ( i = 0; i < OBC_MAX; i++ ) {
		if ( obc[i] != NULL && !obc[i]->doomed && obc[i]->name[0] != 0
		  && same_name(obc[i]->name, name) ) {
			*p_uuid = obc[i]->uuid;
			er = E_OK;
			break;
		}
	}
	UNLOCK();

	return er;
}

/*
 * The process that wrote the message last read from a channel (0 the
 * kernel, E_NOEXS no such channel): so that a reader that acts on what
 * it is asked can look at who asked.
 */
EXPORT ID knl_obchan_last_from( CONST TS_UUID *uuid )
{
	INT	i;
	ID	pid;

	LOCK();
	i = index_of(uuid);
	pid = ( i >= 0 ) ? obc[i]->last_from : E_NOEXS;
	UNLOCK();
	return pid;
}

LOCAL CONST T_OBMGR obc_mgr = {
	OB_T_CHANNEL, OB_S_QUEUE, "channel",
	obc_find, obc_ref, obc_prot, obc_setprot, obc_create, obc_remove,
	obc_open, obc_close, obc_rea, obc_wri, NULL, NULL, NULL, obc_lrc,
	obc_gat, NULL, obc_lnk, obc_lst, obc_fndnam,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL
};

EXPORT ER knl_obchan_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obc_mtx = tk_cre_mtx(&cmtx);
	if ( obc_mtx <= 0 ) {
		return (ER)obc_mtx;
	}
	no = knl_ob_regist(&obc_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
