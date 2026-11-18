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
 *	reader, unless the key was opened not to wait. A message is as long
 *	as it was written, in memory of its own; the queue is full when the
 *	messages on it take OB_CH_BYTES, as a socket's buffer is, however
 *	many they are. A message longer than that is refused (E_LIMIT), and
 *	a reader with less room than the message is told its length
 *	(E_LIMIT) and finds it first again, nothing of it cut or lost. Deleting the channel
 *	wakes whoever waits on it with E_DLT. A channel made with a name
 *	is found by it, among the same names as the global names.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "obj.h"

#define OBC_MAX		8192

typedef struct obcmsg {
	struct obcmsg *next;
	SZ	size;
	ID	from;			/* the process that wrote it, 0 the kernel */
	UB	body[];			/* size bytes */
} OBCMSG;

#define OBC_SEM_MAX	0x7FFFFFFF

typedef struct {
	TS_UUID	uuid;
	UB	*name;			/* as long as it was given, "" for none; never NULL once made */
	SZ	namelen;
	T_OBPRT	*prt;			/* its own copy */
	OBCMSG	*head, *tail;		/* the queue, first to last */
	INT	count;			/* messages on it */
	SZ	bytes;			/* and the bytes they take (OB_CH_BYTES at most) */
	ID	sem_free;		/* room on the queue may have come: a writer looks again */
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

/*
 * A notice of a move put on the channel port (knl_ob_ntf): when the
 * newest message still waiting on it is the same writer's notice of a
 * move, for the same request about the same thing, that one is brought
 * up to date instead of a second one put behind it, as the input layer
 * does with the moves waiting in its ring (device/usb/hid.c). The one
 * who reads takes where the pointer is now rather than every place it
 * passed, and a pointer that moves faster than a program follows it
 * never fills the queue, so that a press or a let go behind the moves
 * is never refused for want of room. The notices dropped before either
 * are counted on the one left. TRUE when it was brought up to date.
 */
EXPORT BOOL knl_obchan_merge_move( CONST TS_UUID *port, CONST T_OBNTM *msg )
{
	OBCH	*c;
	OBCMSG	*q;
	T_OBNTM	was, now;
	INT	i;
	BOOL	done = FALSE;

	if ( port == NULL || msg == NULL || msg->event != OB_E_MOVE ) {
		return FALSE;
	}
	LOCK();
	i = index_of(port);
	if ( i >= 0 && obc[i]->count > 0 && obc[i]->tail != NULL ) {
		c = obc[i];
		q = c->tail;
		if ( q->size == (SZ)sizeof(T_OBNTM) && q->from == ts_get_pid() ) {
			knl_memcpy(&was, q->body, (INT)sizeof(was));
			if ( was.event == OB_E_MOVE && was.nid == msg->nid && was.id == msg->id
			  && was.recno == msg->recno && ts_uuid_cmp(&was.uuid, &msg->uuid) == 0 ) {
				now = *msg;
				now.lost += was.lost;
				knl_memcpy(q->body, &now, (INT)sizeof(now));
				done = TRUE;
			}
		}
	}
	UNLOCK();

	return done;
}

LOCAL void ch_free( INT i )
{
	OBCH	*c = obc[i];

	if ( c->sem_free > 0 ) tk_del_sem(c->sem_free);
	if ( c->sem_used > 0 ) tk_del_sem(c->sem_used);
	while ( c->head != NULL ) {
		OBCMSG	*m = c->head;

		c->head = m->next;
		Kfree(m);
	}
	if ( c->prt != NULL ) Kfree(c->prt);
	if ( c->name != NULL ) Kfree(c->name);
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
		r->flags = OB_F_VOLATILE | ( obc[i]->name[0] != 0 ? OB_F_GLOBAL : 0 );
		r->refcnt = obc[i]->refcnt;
		r->nrec = 1;
		r->size = (UD)obc[i]->count;	/* messages waiting */
	}
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obc_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *p_prt = knl_ob_prt_dup(obc[i]->prt);
	UNLOCK();

	return ( i < 0 ) ? E_NOEXS : ( *p_prt == NULL ) ? E_NOMEM : E_OK;
}

LOCAL ER obc_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	T_OBPRT	*n = knl_ob_prt_dup(prt), *old = NULL;
	INT	i;

	if ( n == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		old = obc[i]->prt;
		obc[i]->prt = n;
	}
	UNLOCK();
	Kfree( ( i >= 0 ) ? old : n );

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
	for ( n = 0; cr->name != NULL && cr->name[n] != 0; n++ ) ;
	c->name = (UB *)Kmalloc((SZ)n + 1);
	if ( c->name == NULL ) {
		Kfree(c);
		return E_NOMEM;
	}
	if ( n > 0 ) knl_memcpy(c->name, cr->name, n);
	c->name[n] = 0;
	c->namelen = n;
	c->prt = knl_ob_prt_dup(prt);
	if ( c->prt == NULL ) {
		Kfree(c->name);
		Kfree(c);
		return E_NOMEM;
	}
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem = OBC_SEM_MAX;
	c->sem_free = tk_cre_sem(&csem);
	c->sem_used = tk_cre_sem(&csem);
	if ( c->sem_free <= 0 || c->sem_used <= 0 ) {
		er = E_LIMIT;
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
	if ( c->prt != NULL ) Kfree(c->prt);
	Kfree(c->name);
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
 * up to tmo (TMO_POL not at all, TMO_FEVR without end), a slice at a
 * time: between slices a process's call looks whether the process is
 * to end, and comes out with E_DISWAI to end on its way back
 * (knl_prc_call_enter). The kernel's own callers wait in one piece,
 * not woken for nothing. The channel is looked up again for each
 * slice: deleted between two, its semaphores are gone and their
 * numbers may be another's (E_DLT). *p_c, *p_sf and *p_su are the
 * channel and its semaphores.
 */
#define OBC_SLICE_MS	200

LOCAL UD ch_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UD)(UW)t.lo;
}

LOCAL ER ch_wait( INT h, BOOL room, TMO tmo, OBCH **p_c, ID *p_sf, ID *p_su )
{
	BOOL	sliced = (BOOL)( tmo != TMO_POL && ts_get_pid() > 0 );
	UD	start = ( tmo > 0 ) ? ch_now() : 0, gone;
	TMO	t;
	ER	er;

	for ( ;; ) {
		*p_c = ch_of(h, p_sf, p_su);
		if ( *p_c == NULL ) {
			return E_DLT;
		}
		t = tmo;
		if ( sliced ) {
			if ( tmo == TMO_FEVR ) {
				t = OBC_SLICE_MS;
			} else {
				gone = ch_now() - start;
				if ( gone >= (UD)tmo ) {
					return E_TMOUT;
				}
				t = ( (UD)tmo - gone < OBC_SLICE_MS ) ? (TMO)( (UD)tmo - gone ) : OBC_SLICE_MS;
			}
		}
		er = tk_wai_sem(room ? *p_sf : *p_su, 1, t);
		if ( er != E_TMOUT || !sliced ) {
			return er;		/* E_TMOUT: the time ran out, or E_DLT: the channel went */
		}
		if ( knl_prc_ending() ) {
			return E_DISWAI;
		}
	}
}

/*
 * One message taken off, waited for up to tmo. With less room than it
 * takes, nothing is taken: E_LIMIT and its length in *p_asize, the
 * message still first, for the reader to read again with room enough.
 */
LOCAL ER obc_rtm( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	OBCH	*c;
	OBCMSG	*m;
	ID	sf, su;
	SZ	n;
	ER	er;

	if ( recno != 0 || off != 0 ) {
		return E_PAR;
	}
	er = ch_wait(h, FALSE, tmo, &c, &sf, &su);
	if ( er < E_OK ) {
		return er;
	}
	LOCK();
	m = c->head;
	if ( m == NULL ) {
		UNLOCK();
		return E_SYS;
	}
	n = m->size;
	if ( n > size ) {
		UNLOCK();
		tk_sig_sem(su, 1);		/* still there for the next read */
		if ( p_asize != NULL ) {
			*p_asize = n;
		}
		return E_LIMIT;
	}
	c->head = m->next;
	if ( c->head == NULL ) c->tail = NULL;
	c->count--;
	c->bytes -= n;
	c->last_from = m->from;
	UNLOCK();
	knl_memcpy(buf, m->body, (INT)n);
	Kfree(m);
	tk_sig_sem(sf, 1);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/* One message taken off, waited for as the key says */
LOCAL ER obc_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	return obc_rtm(h, recno, off, buf, size, p_asize, nowait ? TMO_POL : TMO_FEVR);
}

/* One message put on */
LOCAL ER obc_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBCH	*c;
	ID	sf, su;
	ER	er;

	OBCMSG	*m;

	if ( recno != 0 || off != 0 || size < 0 ) {
		return E_PAR;
	}
	if ( size > OB_CH_BYTES ) {
		return E_LIMIT;			/* more than the whole room: never to go on */
	}
	m = (OBCMSG *)Kmalloc(sizeof(OBCMSG) + (SZ)size);
	if ( m == NULL ) {
		return E_NOMEM;
	}
	m->next = NULL;
	m->size = size;
	m->from = ts_get_pid();
	knl_memcpy(m->body, buf, (INT)size);
	for ( ;; ) {
		c = ch_of(h, &sf, &su);
		if ( c == NULL ) {
			Kfree(m);
			return E_DLT;
		}
		LOCK();
		if ( !c->doomed && c->bytes + size <= OB_CH_BYTES ) {
			if ( c->tail != NULL ) c->tail->next = m;
			else c->head = m;
			c->tail = m;
			c->count++;
			c->bytes += size;
			UNLOCK();
			break;
		}
		UNLOCK();
		/* no room: until a reader has taken something, and then looked at again */
		er = ch_wait(h, TRUE, nowait ? TMO_POL : TMO_FEVR, &c, &sf, &su);
		if ( er < E_OK ) {
			Kfree(m);
			return er;
		}
	}
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

/* The name, whole */
LOCAL ER obc_refnam( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	INT	i;
	ER	er = E_NOEXS;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		er = knl_ob_name_put(obc[i]->name, obc[i]->namelen, buf, size, p_asize);
	}
	UNLOCK();

	return er;
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
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	obc_rtm,
	NULL,
	obc_refnam
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
