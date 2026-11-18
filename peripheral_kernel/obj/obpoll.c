/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obpoll.c
 *	The poll set manager: keys of a process watched together, and a wait
 *	for those of them that are ready (OB_T_CHANNEL, OB_S_POLL; design
 *	18.8)
 *
 *	A set is an object without a name, made with ob_cre_key. Each key
 *	put in it becomes an item, which goes on the list of watchers of the
 *	object the key opens (its manager's pol). The object tells its
 *	watchers of every change that may make it ready (knl_obpoll_tell),
 *	under its own lock; the item then goes on the set's ready list, and a
 *	task waiting on the set is woken. A wait takes the items off that
 *	list, asks each object what holds for it now, and gives the ones that
 *	are ready, so that what a wait costs is the items that changed, not
 *	the items there are.
 *
 *	An item is level triggered unless asked otherwise: once given, it
 *	goes back on the ready list, and the next wait asks its object again
 *	and drops it when it is no longer ready. An edge triggered item
 *	(OB_PL_ET) waits for the next change instead, and one for once
 *	(OB_PL_ONESHOT) is not watched again until it is changed (OB_PL_MOD).
 *
 *	Locks: a set's own lock keeps its items while they are changed or
 *	looked at; one lock of the manager (pl_mtx) keeps every list of
 *	watchers and every ready list, and is the innermost of all: an
 *	object takes it under its own lock to tell, and a set takes the
 *	object's lock (through pol) under the set's lock, never under pl_mtx.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "obj.h"

#define OBPL_MAX	1024		/* sets at once */
#define OBPL_SLICE_MS	200		/* a process's wait looks whether it is to end */
#define OBPL_TEXT_MAX	256
#define OBPL_BATCH	OB_PL_MAXEV	/* the most one wait gives */

#define PL_PRIVATE	( OB_PL_ONESHOT | OB_PL_ET )
#define PL_READY	( OB_PL_IN | OB_PL_OUT | OB_PL_ERR | OB_PL_HUP | OB_PL_RDHUP )

struct OBPSET;

typedef struct OBPITEM {
	OBPWAIT		w;		/* on the object's watchers: first, it is cast back */
	struct OBPSET	*set;
	struct OBPITEM	*next;		/* the set's items */
	struct OBPITEM	*rnext, *rprev;	/* the ready list */
	BOOL		rdy;		/* on the ready list */
	BOOL		gone;		/* its object went without it being taken off */
	UINT		seen;		/* the wait that last gave it */
	ID		key;		/* the key it was put in with: it is named by it */
	CONST T_OBMGR	*mgr;		/* the object it watches */
	INT		h;
	UINT		events;		/* OB_PL_*: what is asked */
	UD		data;
} OBPITEM;

typedef struct OBPSET {
	TS_UUID		uuid;
	UINT		gen;
	ID		maker;
	T_OBPRT		*prt;
	INT		opens;
	BOOL		opened;
	BOOL		doomed;		/* deleted while still open: it goes with its last close */
	ID		mtx;		/* the set's own lock */
	ID		flg;		/* set when an item becomes ready or the set is woken */
	OBPITEM		*items;
	INT		nitems;
	OBPITEM		*rfirst, *rlast;	/* ready (pl_mtx) */
	INT		waiters;	/* tasks asleep on flg (pl_mtx) */
	BOOL		woken;		/* OB_PL_WAKE not yet answered (pl_mtx) */
	UINT		serial;		/* waits so far */
} OBPSET;

LOCAL OBPSET	*obp[OBPL_MAX];
LOCAL ID	obp_mtx = 0;		/* the table */
LOCAL ID	pl_mtx = 0;		/* the lists of watchers and the ready lists */
LOCAL UINT	obp_gen = 0;

#define OBP_H_SHIFT	11		/* a handle: the slot + 1 below, the set's gen above */

#define LOCK()		tk_loc_mtx(obp_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obp_mtx)
#define PL_LOCK()	tk_loc_mtx(pl_mtx, TMO_FEVR)
#define PL_UNLOCK()	tk_unl_mtx(pl_mtx)

/* ---------------------------------------------------------------- the lists (pl_mtx held) */

LOCAL void rdy_put( OBPSET *s, OBPITEM *it )
{
	it->rnext = NULL;
	it->rprev = s->rlast;
	if ( s->rlast != NULL ) s->rlast->rnext = it; else s->rfirst = it;
	s->rlast = it;
	it->rdy = TRUE;
}

LOCAL void rdy_take( OBPSET *s, OBPITEM *it )
{
	if ( !it->rdy ) return;
	if ( it->rprev != NULL ) it->rprev->rnext = it->rnext; else s->rfirst = it->rnext;
	if ( it->rnext != NULL ) it->rnext->rprev = it->rprev; else s->rlast = it->rprev;
	it->rnext = it->rprev = NULL;
	it->rdy = FALSE;
}

/* An item made ready: on the list, and a waiter woken */
LOCAL void rdy_mark( OBPITEM *it )
{
	OBPSET	*s = it->set;

	if ( it->rdy ) return;
	rdy_put(s, it);
	if ( s->waiters > 0 ) (void)tk_set_flg(s->flg, 1);
}

LOCAL void unlink_w( OBPWAIT *w )
{
	OBPLIST	*l = w->on;

	if ( l == NULL ) return;
	if ( w->prev != NULL ) w->prev->next = w->next; else l->first = w->next;
	if ( w->next != NULL ) w->next->prev = w->prev;
	w->next = w->prev = NULL;
	w->on = NULL;
}

/* ---------------------------------------------------------------- what the watched objects call */

EXPORT void knl_obpoll_link( OBPLIST *l, OBPWAIT *w )
{
	PL_LOCK();
	unlink_w(w);
	w->prev = NULL;
	w->next = l->first;
	if ( l->first != NULL ) l->first->prev = w;
	l->first = w;
	w->on = l;
	PL_UNLOCK();
}

EXPORT void knl_obpoll_tell( OBPLIST *l )
{
	OBPWAIT	*w;

	if ( l->first == NULL || pl_mtx <= 0 ) return;
	PL_LOCK();
	for ( w = l->first; w != NULL; w = w->next ) {
		OBPITEM	*it = (OBPITEM *)w;

		if ( ( it->events & ~PL_PRIVATE ) == 0 ) continue;	/* for once, and given */
		rdy_mark(it);
	}
	PL_UNLOCK();
}

EXPORT void knl_obpoll_gone( OBPLIST *l, BOOL tell )
{
	OBPWAIT	*w;

	if ( l->first == NULL || pl_mtx <= 0 ) return;
	PL_LOCK();
	while ( ( w = l->first ) != NULL ) {
		OBPITEM	*it = (OBPITEM *)w;

		unlink_w(w);
		it->gone = TRUE;
		if ( tell ) {
			rdy_mark(it);		/* told as an error and a hang-up */
		} else {
			rdy_take(it->set, it);	/* its last key closed: nothing more of it */
			it->events &= PL_PRIVATE;
		}
	}
	PL_UNLOCK();
}

/* ---------------------------------------------------------------- sets */

LOCAL OBPSET *set_of( INT h )
{
	INT	i = ( h & ( ( 1 << OBP_H_SHIFT ) - 1 ) ) - 1;

	if ( h <= 0 || i < 0 || i >= OBPL_MAX || obp[i] == NULL ) return NULL;
	return ( obp[i]->gen == ( (UINT)h >> OBP_H_SHIFT ) ) ? obp[i] : NULL;
}

LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; i < OBPL_MAX; i++ ) {
		if ( obp[i] != NULL && !obp[i]->doomed && ts_uuid_cmp(&obp[i]->uuid, uuid) == 0 ) return i;
	}
	return -1;
}

/* An item let go (the set's lock held): off the watchers and the ready list */
LOCAL void item_free( OBPSET *s, OBPITEM *it )
{
	PL_LOCK();
	unlink_w(&it->w);
	rdy_take(s, it);
	PL_UNLOCK();
	Kfree(it);
}

LOCAL void items_drop( OBPSET *s )
{
	OBPITEM	*it;

	while ( ( it = s->items ) != NULL ) {
		s->items = it->next;
		item_free(s, it);
	}
	s->nitems = 0;
}

LOCAL void set_free( OBPSET *s )
{
	items_drop(s);
	if ( s->flg > 0 ) tk_del_flg(s->flg);
	if ( s->mtx > 0 ) tk_del_mtx(s->mtx);
	if ( s->prt != NULL ) Kfree(s->prt);
	Kfree(s);
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obp_find( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	UNLOCK();
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obp_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		r->flags = OB_F_VOLATILE;
		r->refcnt = 0;
		r->nrec = OB_PL_NREC;
		r->size = (UD)obp[i]->nitems;
	}
	UNLOCK();
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obp_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	INT	i;

	*p_prt = NULL;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *p_prt = knl_ob_prt_dup(obp[i]->prt);
	UNLOCK();
	return ( i < 0 ) ? E_NOEXS : ( *p_prt == NULL ) ? E_NOMEM : E_OK;
}

LOCAL ER obp_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	T_OBPRT	*n = knl_ob_prt_dup(prt), *old = NULL;
	INT	i;

	if ( n == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		old = obp[i]->prt;
		obp[i]->prt = n;
	}
	UNLOCK();
	Kfree(( i >= 0 ) ? old : n);
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

/* Without a protection given, the maker's own and only theirs (rw-------), as a stream's */
LOCAL ER obp_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	OBPSET	*s;
	T_CFLG	cflg;
	T_CMTX	cmtx;
	INT	i;
	ER	er;

	s = (OBPSET *)Kmalloc(sizeof(OBPSET));
	if ( s == NULL ) {
		return E_NOMEM;
	}
	knl_memset(s, 0, sizeof(*s));
	s->maker = ts_get_pid();
	s->prt = knl_ob_prt_dup(prt);
	if ( s->prt != NULL && c->prt == NULL ) s->prt->mode = 0600;
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	s->flg = tk_cre_flg(&cflg);
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	s->mtx = tk_cre_mtx(&cmtx);
	er = ts_gen_uuid(&s->uuid);
	if ( er >= E_OK && s->prt == NULL ) er = E_NOMEM;
	if ( er >= E_OK && ( s->flg <= 0 || s->mtx <= 0 ) ) er = E_LIMIT;
	if ( er < E_OK ) {
		set_free(s);
		return er;
	}
	LOCK();
	for ( i = 0; i < OBPL_MAX && obp[i] != NULL; i++ ) ;
	if ( i < OBPL_MAX ) {
		s->gen = ( ++obp_gen ) & 0xFFFFF;
		obp[i] = s;
	}
	UNLOCK();
	if ( i == OBPL_MAX ) {
		set_free(s);
		return E_LIMIT;
	}
	*p_uuid = s->uuid;
	return E_OK;
}

/* Deleted: the set goes as its last close would; a waiter on it is woken and finds it gone */
LOCAL ER obp_remove( CONST TS_UUID *uuid )
{
	OBPSET	*s = NULL;
	INT	i;

	BOOL	open = FALSE;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		s = obp[i];
		open = (BOOL)( s->opens > 0 );
		if ( open ) s->doomed = TRUE; else obp[i] = NULL;
	}
	UNLOCK();
	if ( s == NULL ) {
		return E_NOEXS;
	}
	knl_ob_post(uuid, -1, OB_E_DELETE, NULL);
	if ( open ) {
		/* still opened: what was in it goes now, the set itself with its last close */
		tk_loc_mtx(s->mtx, TMO_FEVR);
		items_drop(s);
		tk_unl_mtx(s->mtx);
		(void)tk_set_flg(s->flg, 1);
		return E_OK;
	}
	set_free(s);
	return E_OK;
}

LOCAL INT obp_open( CONST TS_UUID *uuid, UINT ops )
{
	INT	i, h = E_NOEXS;

	(void)ops;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		obp[i]->opens++;
		obp[i]->opened = TRUE;
		h = (INT)( ( obp[i]->gen << OBP_H_SHIFT ) | (UINT)( i + 1 ) );
	}
	UNLOCK();
	return h;
}

LOCAL ER obp_close( INT h )
{
	OBPSET	*s, *gone = NULL;
	INT	i = ( h & ( ( 1 << OBP_H_SHIFT ) - 1 ) ) - 1;

	LOCK();
	s = set_of(h);
	if ( s != NULL && --s->opens <= 0 ) {
		gone = s;
		obp[i] = NULL;
	}
	UNLOCK();
	if ( s == NULL ) {
		return E_ID;
	}
	if ( gone != NULL ) set_free(gone);
	return E_OK;
}

LOCAL OBPITEM *item_of( OBPSET *s, ID key )
{
	OBPITEM	*it;

	for ( it = s->items; it != NULL && it->key != key; it = it->next ) ;
	return it;
}

/*
 * One change of the set (its lock held). A key is named by itself; an
 * item whose key was closed and whose object went, or whose key now
 * opens another object, is taken off as if it were not there.
 */
LOCAL ER ctl_one( OBPSET *s, CONST T_OBPLCTL *c )
{
	CONST T_OBMGR	*m = NULL;
	OBPITEM		*it, **pp;
	INT		h = 0, mask;
	UINT		ev = c->events & ( PL_READY | PL_PRIVATE );

	switch ( c->op ) {
	case OB_PL_WAKE:
		PL_LOCK();
		s->woken = TRUE;
		if ( s->waiters > 0 ) (void)tk_set_flg(s->flg, 1);
		PL_UNLOCK();
		return E_OK;
	case OB_PL_CLEAR:
		items_drop(s);
		PL_LOCK();
		s->woken = FALSE;
		PL_UNLOCK();
		return E_OK;
	case OB_PL_ADD:
	case OB_PL_MOD:
	case OB_PL_DEL:
		break;
	default:
		return E_PAR;
	}
	mask = knl_ob_key_watch(c->key, NULL, &m, &h);
	it = item_of(s, c->key);
	if ( it != NULL && ( mask < E_OK || it->mgr != m || it->h != h ) ) {
		for ( pp = &s->items; *pp != it; pp = &(*pp)->next ) ;
		*pp = it->next;
		s->nitems--;
		item_free(s, it);		/* a key of before, closed */
		it = NULL;
	}
	if ( mask < E_OK ) {
		return (ER)mask;
	}
	switch ( c->op ) {
	case OB_PL_ADD:
		if ( it != NULL ) return E_OBJ;
		it = (OBPITEM *)Kmalloc(sizeof(OBPITEM));
		if ( it == NULL ) return E_NOMEM;
		knl_memset(it, 0, sizeof(*it));
		it->set = s;
		it->key = c->key;
		it->mgr = m;
		it->h = h;
		it->events = ev;
		it->data = c->data;
		mask = knl_ob_key_watch(c->key, &it->w, &m, &h);
		if ( mask < E_OK ) {
			item_free(s, it);
			return (ER)mask;
		}
		it->next = s->items;
		s->items = it;
		s->nitems++;
		break;
	case OB_PL_MOD:
		if ( it == NULL ) return E_NOEXS;
		PL_LOCK();
		it->events = ev;
		it->data = c->data;
		PL_UNLOCK();
		break;
	default:			/* OB_PL_DEL */
		if ( it == NULL ) return E_NOEXS;
		for ( pp = &s->items; *pp != it; pp = &(*pp)->next ) ;
		*pp = it->next;
		s->nitems--;
		item_free(s, it);
		return E_OK;
	}
	/* ready now: as a change would have told it */
	if ( ( (UINT)mask & ( ev | OB_PL_ERR | OB_PL_HUP ) & PL_READY ) != 0 ) {
		PL_LOCK();
		rdy_mark(it);
		PL_UNLOCK();
	}
	return E_OK;
}

/* Record 1 written: the changes in turn, stopping at the first that fails */
LOCAL ER obp_ctl( INT h, CONST void *buf, SZ size, SZ *p_asize )
{
	OBPSET		*s;
	T_OBPLCTL	c;
	SZ		done = 0;
	ER		er = E_OK;

	if ( size <= 0 || size % (SZ)sizeof(T_OBPLCTL) != 0 ) {
		return E_PAR;
	}
	LOCK();
	s = set_of(h);
	UNLOCK();
	if ( s == NULL || s->doomed ) {
		return E_NOEXS;
	}
	tk_loc_mtx(s->mtx, TMO_FEVR);
	while ( done < size ) {
		knl_memcpy(&c, (CONST UB *)buf + done, sizeof(c));
		er = ctl_one(s, &c);
		if ( er < E_OK ) break;
		done += (SZ)sizeof(c);
	}
	tk_unl_mtx(s->mtx);
	if ( p_asize != NULL ) {
		*p_asize = done;
	}
	return er;
}

/*
 * The ready items given, at most max (the set's lock held): each asked
 * of its object again, dropped when it is no longer ready, and put back
 * when it is level triggered.
 */
LOCAL INT harvest( OBPSET *s, T_OBPLEV *out, INT max )
{
	OBPITEM	*again = NULL, *it;
	UINT	serial = ++s->serial;
	INT	n = 0;

	while ( n < max ) {
		INT	mask;
		UINT	got;

		PL_LOCK();
		it = s->rfirst;
		if ( it != NULL ) rdy_take(s, it);
		if ( it != NULL && it->seen == serial ) {
			/* told again since it was given in this wait: the next one gives it */
			it->rnext = again;
			again = it;
			it->rdy = TRUE;
			PL_UNLOCK();
			continue;
		}
		PL_UNLOCK();
		if ( it == NULL ) break;
		if ( ( it->events & ~PL_PRIVATE ) == 0 ) continue;
		mask = it->gone ? ( OB_PL_ERR | OB_PL_HUP ) : it->mgr->pol(it->h, NULL);
		if ( mask < 0 ) mask = OB_PL_ERR | OB_PL_HUP;
		got = (UINT)mask & ( it->events | OB_PL_ERR | OB_PL_HUP ) & PL_READY;
		if ( got == 0 ) continue;	/* not ready after all: left off */
		it->seen = serial;
		out[n].events = got;
		out[n].resv = 0;
		out[n].data = it->data;
		n++;
		PL_LOCK();
		if ( it->events & OB_PL_ONESHOT ) {
			it->events &= PL_PRIVATE;	/* not watched until it is changed */
		} else if ( !( it->events & OB_PL_ET ) && !it->rdy ) {
			it->rnext = again;
			again = it;
			it->rdy = TRUE;		/* put back below, after this wait's items */
		}
		PL_UNLOCK();
	}
	if ( again != NULL ) {
		PL_LOCK();
		while ( again != NULL ) {
			it = again;
			again = it->rnext;
			it->rdy = FALSE;
			rdy_put(s, it);
		}
		PL_UNLOCK();
	}
	return n;
}

LOCAL UD obp_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UD)(UW)t.lo;
}

/*
 * Record 1 read: the items ready, waited for up to tmo. E_OK with none
 * when the set was woken (OB_PL_WAKE), E_TMOUT when the time ran out.
 */
LOCAL ER obp_wait( INT h, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	T_OBPLEV	ev[OBPL_BATCH];
	OBPSET		*s;
	BOOL		sliced = (BOOL)( ts_get_pid() > 0 );
	UD		start = ( tmo > 0 ) ? obp_now() : 0;
	INT		max = (INT)( size / (SZ)sizeof(T_OBPLEV) ), n = 0;
	ER		er = E_OK;

	if ( max <= 0 ) {
		return E_PAR;
	}
	if ( p_asize != NULL ) *p_asize = 0;
	for ( ;; ) {
		UINT	ptn;
		TMO	t;

		LOCK();
		s = set_of(h);
		UNLOCK();
		if ( s == NULL || s->doomed ) {
			return ( s == NULL ) ? E_NOEXS : E_DLT;
		}
		tk_loc_mtx(s->mtx, TMO_FEVR);
		/* one look only: what is level triggered is back on the list after it */
		n = harvest(s, ev, ( max < OBPL_BATCH ) ? max : OBPL_BATCH);
		if ( n > 0 ) knl_memcpy(buf, ev, (INT)sizeof(T_OBPLEV) * n);
		PL_LOCK();
		if ( n > 0 || s->woken ) {
			s->woken = FALSE;
			PL_UNLOCK();
			tk_unl_mtx(s->mtx);
			break;
		}
		if ( s->rfirst != NULL ) {	/* came meanwhile */
			PL_UNLOCK();
			tk_unl_mtx(s->mtx);
			continue;
		}
		if ( tmo == TMO_POL ) {
			PL_UNLOCK();
			tk_unl_mtx(s->mtx);
			return E_TMOUT;
		}
		s->waiters++;
		(void)tk_clr_flg(s->flg, 0);
		PL_UNLOCK();
		tk_unl_mtx(s->mtx);
		t = tmo;
		if ( tmo != TMO_FEVR ) {
			UD	gone = obp_now() - start;

			t = ( gone >= (UD)tmo ) ? 0 : (TMO)( (UD)tmo - gone );
		}
		if ( sliced && ( t == TMO_FEVR || t > OBPL_SLICE_MS ) ) t = OBPL_SLICE_MS;
		er = ( t == 0 ) ? E_TMOUT : tk_wai_flg(s->flg, 1, TWF_ORW, &ptn, t);
		PL_LOCK();
		s->waiters--;
		PL_UNLOCK();
		if ( er == E_TMOUT ) {
			if ( sliced && knl_prc_ending() ) return E_DISWAI;
			if ( tmo != TMO_FEVR && obp_now() - start >= (UD)tmo ) {
				/* one more look: what came as the time ran out */
				tmo = TMO_POL;
			}
			continue;
		}
		if ( er < E_OK ) {
			return E_DLT;
		}
	}
	if ( p_asize != NULL ) *p_asize = (SZ)sizeof(T_OBPLEV) * n;
	return E_OK;
}

/* Record 0: what the set is, as xmlTAD */
LOCAL ER obp_text( INT h, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	t[OBPL_TEXT_MAX];
	OBPSET	*s;
	INT	n, len, items = 0;
	SZ	k = 0;

	LOCK();
	s = set_of(h);
	if ( s != NULL ) items = s->nitems;
	UNLOCK();
	if ( s == NULL ) {
		return E_NOEXS;
	}
	n = knl_oj_put(t, 0, sizeof(t), "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>poll set: ");
	n = knl_oj_put_num(t, n, sizeof(t), (D)items);
	n = knl_oj_put(t, n, sizeof(t), " keys watched. Record 1 is the keys ready</p></document></tad>");
	len = ( n > 0 ) ? n : 0;
	if ( off < len ) {
		k = (SZ)( len - off );
		if ( k > size ) k = size;
		knl_memcpy(buf, t + off, (INT)k);
	}
	if ( p_asize != NULL ) {
		*p_asize = k;
	}
	return E_OK;
}

LOCAL ER obp_rtm( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	if ( recno == 0 ) {
		return obp_text(h, off, buf, size, p_asize);
	}
	if ( recno != OB_PL_REC ) {
		return E_NOEXS;
	}
	if ( off != 0 ) {
		return E_PAR;
	}
	return obp_wait(h, buf, size, p_asize, tmo);
}

LOCAL ER obp_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	return obp_rtm(h, recno, off, buf, size, p_asize, nowait ? TMO_POL : TMO_FEVR);
}

LOCAL ER obp_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	(void)nowait;
	if ( recno == 0 ) {
		return E_RONLY;
	}
	if ( recno != OB_PL_REC ) {
		return E_NOEXS;
	}
	if ( off != 0 ) {
		return E_PAR;
	}
	return obp_ctl(h, buf, size, p_asize);
}

LOCAL ER obp_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBPSET	*s;
	INT	k, items = 0;

	LOCK();
	s = set_of(h);
	if ( s != NULL ) items = s->nitems;
	UNLOCK();
	if ( s == NULL ) {
		return E_NOEXS;
	}
	for ( k = 0; k < n && k < OB_PL_NREC; k++ ) {
		buf[k].recno = k;
		buf[k].rt = ( k == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA;
		buf[k].sub = 0;
		buf[k].size = ( k == OB_PL_REC ) ? (UD)items * sizeof(T_OBPLEV) : 0;
	}
	*p_cnt = k;
	return E_OK;
}

LOCAL ER obp_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBPSET	*s;
	INT	n, items = 0;

	LOCK();
	s = set_of(h);
	if ( s != NULL ) items = s->nitems;
	UNLOCK();
	if ( s == NULL ) {
		return E_NOEXS;
	}
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":\"\",\"poll\":{\"keys\":");
	n = knl_oj_put_num(json, n, (INT)size, (D)items);
	n = knl_oj_put(json, n, (INT)size, "}}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obp_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	LOCK();
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < OBPL_MAX; i++ ) {
			if ( obp[i] == NULL ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&obp[i]->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&obp[i]->uuid, from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&obp[i]->uuid, &obp[best]->uuid) < 0 ) best = i;
		}
		if ( best < 0 ) break;
		buf[cnt++] = obp[best]->uuid;
	}
	UNLOCK();
	*p_cnt = cnt;
	return E_OK;
}

/* Sets a process made and never opened go with it (knl_ob_prc_end) */
EXPORT void knl_obpoll_prc_end( ID pid )
{
	INT	i = 0;

	if ( obp_mtx <= 0 || pid <= 0 ) {
		return;
	}
	while ( i < OBPL_MAX ) {
		OBPSET	*s = NULL;

		LOCK();
		for ( ; i < OBPL_MAX && s == NULL; i++ ) {
			if ( obp[i] != NULL && !obp[i]->opened && obp[i]->maker == pid ) {
				s = obp[i];
				obp[i] = NULL;
			}
		}
		UNLOCK();
		if ( s != NULL ) set_free(s);
	}
}

LOCAL CONST T_OBMGR obp_mgr = {
	OB_T_CHANNEL, OB_S_POLL, "poll",
	obp_find, obp_ref, obp_prot, obp_setprot, obp_create, obp_remove,
	obp_open, obp_close, obp_rea, obp_wri, NULL, NULL, NULL, obp_lrc,
	obp_gat, NULL, NULL, obp_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL,
	obp_rtm,
	NULL			/* a set is not watched by another */
};

EXPORT ER knl_obpoll_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obp_mtx = tk_cre_mtx(&cmtx);
	pl_mtx = tk_cre_mtx(&cmtx);
	if ( obp_mtx <= 0 || pl_mtx <= 0 ) {
		return E_LIMIT;
	}
	no = knl_ob_regist(&obp_mgr);
	return ( no > 0 ) ? E_OK : (ER)no;
}
