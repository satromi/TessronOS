/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obstrm.c
 *	The stream manager: pairs of streams between processes as objects
 *	(OB_T_CHANNEL, OB_S_STREAM; design 18.8)
 *
 *	Each end is an object of its own, made two at a time. An end holds
 *	what its peer wrote, up to OB_ST_BUF bytes in a ring, and the keys
 *	that came with them, each marked with the place in the stream of the
 *	byte it came with. A read stops before the next such place, so that
 *	keys come with the bytes they were written with and never later.
 *
 *	An end is open while a handle of the name manager is open on it: a
 *	key, a key held by a stream that carries it, or an operation under
 *	way. Its last close takes it away at once; its peer then reads the
 *	end after what is left and cannot write (E_IO), and whoever waits on
 *	the peer is woken. What an end held is let go with it, keys included,
 *	outside the lock: letting a key go may close another end.
 *
 *	A wait is on the end's own event flag, which every change that
 *	concerns the end sets; the waiter clears it under the lock before it
 *	looks, so that a change after the look is never missed. A process's
 *	wait goes in slices and comes out (E_DISWAI) when the process is to
 *	end. The same changes are told, under the lock, to the poll sets
 *	that watch the end (obpoll.c), which ask it what holds (obs_pol).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "obj.h"

#define OBS_MAX		2048		/* ends at once */
#define OBS_SLICE_MS	200
#define OBS_TEXT_MAX	512		/* bytes of record 0 */

typedef struct {
	UD		at;		/* the place of the byte it came with */
	T_OBKHOLD	k;
} OBSKEY;

typedef struct {
	TS_UUID	uuid;
	UINT	gen;			/* which end of this slot it is: a handle names it */
	INT	peer;			/* the other end's slot, -1 gone */
	ID	maker;			/* the process that made the pair */
	ID	flg;			/* set at every change that concerns this end */
	T_OBPRT	*prt;
	INT	opens;
	BOOL	opened;			/* it has been opened once */
	BOOL	shut;			/* it writes no more */
	BOOL	eof;			/* its peer writes no more */
	UB	*buf;			/* what the peer wrote: OB_ST_BUF bytes */
	INT	head, len;
	UD	rpos;			/* bytes read from it so far */
	OBSKEY	*keys;			/* keys waiting, in order: OB_ST_KEY_HELD */
	INT	nkeys;
	OBPLIST	pw;			/* the poll sets watching it */
} OBSEND;

LOCAL OBSEND	*obs[OBS_MAX];
LOCAL ID	obs_mtx = 0;
LOCAL UINT	obs_gen = 0;

#define OBS_H_SHIFT	12		/* a handle: the slot + 1 below, the end's gen above */

#define LOCK()		tk_loc_mtx(obs_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obs_mtx)

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* The slot of an end, -1 none (under the lock) */
LOCAL INT index_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; i < OBS_MAX; i++ ) {
		if ( obs[i] != NULL && same_uuid(&obs[i]->uuid, uuid) ) {
			return i;
		}
	}
	return -1;
}

/* The end a handle was opened on, while it is still there (under the lock) */
LOCAL OBSEND *end_of( INT h )
{
	INT	i = ( h & ( ( 1 << OBS_H_SHIFT ) - 1 ) ) - 1;

	if ( h <= 0 || i < 0 || i >= OBS_MAX || obs[i] == NULL ) return NULL;
	return ( obs[i]->gen == ( (UINT)h >> OBS_H_SHIFT ) ) ? obs[i] : NULL;
}

LOCAL INT slot_of( INT h )
{
	return ( h & ( ( 1 << OBS_H_SHIFT ) - 1 ) ) - 1;
}

/* The changes to tell once the lock is let go */
typedef struct {
	INT	n;
	TS_UUID	u[2];
	ID	f[2];
} OBSTELL;

LOCAL void tell_add( OBSTELL *t, OBSEND *e )
{
	INT	i;

	knl_obpoll_tell(&e->pw);		/* under the lock, as the poll sets ask */
	for ( i = 0; i < t->n; i++ ) {
		if ( same_uuid(&t->u[i], &e->uuid) ) return;
	}
	if ( t->n < 2 ) {
		t->u[t->n] = e->uuid;
		t->f[t->n] = e->flg;
		t->n++;
	}
}

LOCAL void tell_out( OBSTELL *t )
{
	INT	i;

	for ( i = 0; i < t->n; i++ ) {
		(void)tk_set_flg(t->f[i], 1);
		knl_ob_post(&t->u[i], OB_ST_DATA, OB_E_CHANGE, NULL);
	}
}

LOCAL void end_free( OBSEND *e )
{
	if ( e->flg > 0 ) tk_del_flg(e->flg);
	if ( e->buf != NULL ) Kfree(e->buf);
	if ( e->keys != NULL ) Kfree(e->keys);
	if ( e->prt != NULL ) Kfree(e->prt);
	Kfree(e);
}

/*
 * The end in slot i taken away (under the lock): its peer told, and what
 * it held handed back to be let go out of the lock -- the end itself and
 * its keys (*p_keys, *p_nkeys; Kfree the array).
 */
LOCAL OBSEND *end_take( INT i, OBSTELL *t, OBSKEY **p_keys, INT *p_nkeys )
{
	OBSEND	*e = obs[i];

	obs[i] = NULL;
	knl_obpoll_gone(&e->pw, FALSE);	/* no key opens it any more, unless it was deleted (below) */
	if ( e->peer >= 0 && obs[e->peer] != NULL ) {
		obs[e->peer]->peer = -1;
		obs[e->peer]->eof = TRUE;
		tell_add(t, obs[e->peer]);
	}
	*p_keys = e->keys;
	*p_nkeys = e->nkeys;
	e->keys = NULL;
	e->nkeys = 0;
	return e;
}

LOCAL void keys_drop( OBSKEY *k, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		knl_ob_key_drop(&k[i].k);
	}
	if ( k != NULL ) Kfree(k);
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obs_find( CONST TS_UUID *uuid )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	UNLOCK();
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obs_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	INT	i;

	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		r->flags = OB_F_VOLATILE;
		r->refcnt = 0;
		r->nrec = OB_ST_NREC;
		r->size = (UD)obs[i]->len;
	}
	UNLOCK();
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obs_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	INT	i;

	*p_prt = NULL;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) *p_prt = knl_ob_prt_dup(obs[i]->prt);
	UNLOCK();
	return ( i < 0 ) ? E_NOEXS : ( *p_prt == NULL ) ? E_NOMEM : E_OK;
}

LOCAL ER obs_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	T_OBPRT	*n = knl_ob_prt_dup(prt), *old = NULL;
	INT	i;

	if ( n == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		old = obs[i]->prt;
		obs[i]->prt = n;
	}
	UNLOCK();
	Kfree(( i >= 0 ) ? old : n);
	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL OBSEND *end_new( CONST T_OBPRT *prt, ER *p_er )
{
	OBSEND	*e;
	T_CFLG	cflg;

	e = (OBSEND *)Kmalloc(sizeof(OBSEND));
	if ( e == NULL ) {
		*p_er = E_NOMEM;
		return NULL;
	}
	knl_memset(e, 0, sizeof(*e));
	e->peer = -1;
	e->maker = ts_get_pid();
	e->prt = knl_ob_prt_dup(prt);
	e->buf = (UB *)Kmalloc(OB_ST_BUF);
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	e->flg = tk_cre_flg(&cflg);
	*p_er = ts_gen_uuid(&e->uuid);
	if ( *p_er >= E_OK && ( e->prt == NULL || e->buf == NULL ) ) *p_er = E_NOMEM;
	if ( *p_er >= E_OK && e->flg <= 0 ) *p_er = E_LIMIT;
	if ( *p_er < E_OK ) {
		end_free(e);
		return NULL;
	}
	return e;
}

/*
 * Both ends made; the first is answered. Without a protection given, the
 * maker's own and only theirs (rw-------), as a socket's.
 */
LOCAL ER obs_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	OBSEND	*a, *b;
	T_OBPRT	*p;
	INT	i, ia = -1, ib = -1;
	ER	er;

	p = knl_ob_prt_dup(prt);
	if ( p == NULL ) {
		return E_NOMEM;
	}
	if ( c->prt == NULL ) {
		p->mode = 0600;
	}
	a = end_new(p, &er);
	b = ( a != NULL ) ? end_new(p, &er) : NULL;
	Kfree(p);
	if ( b == NULL ) {
		if ( a != NULL ) end_free(a);
		return er;
	}
	LOCK();
	for ( i = 0; i < OBS_MAX && ( ia < 0 || ib < 0 ); i++ ) {
		if ( obs[i] != NULL ) continue;
		if ( ia < 0 ) ia = i; else ib = i;
	}
	if ( ib >= 0 ) {
		a->peer = ib;
		b->peer = ia;
		a->gen = ( ++obs_gen ) & 0x7FFFF;
		b->gen = ( ++obs_gen ) & 0x7FFFF;
		obs[ia] = a;
		obs[ib] = b;
	}
	UNLOCK();
	if ( ib < 0 ) {
		end_free(a);
		end_free(b);
		return E_LIMIT;
	}
	*p_uuid = a->uuid;
	return E_OK;
}

/* Deleted: the end is taken away as its last close would */
LOCAL ER obs_remove( CONST TS_UUID *uuid )
{
	OBSTELL	t;
	OBSEND	*e = NULL;
	OBSKEY	*k = NULL;
	INT	i, nk = 0;

	t.n = 0;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		knl_obpoll_gone(&obs[i]->pw, TRUE);	/* keys to it are left: those watching it are told */
		e = end_take(i, &t, &k, &nk);
	}
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	(void)tk_set_flg(e->flg, 1);		/* its own waiters see it gone */
	tell_out(&t);
	keys_drop(k, nk);
	knl_ob_post(uuid, -1, OB_E_DELETE, NULL);
	end_free(e);
	return E_OK;
}

LOCAL INT obs_open( CONST TS_UUID *uuid, UINT ops )
{
	INT	i, h = E_NOEXS;

	(void)ops;
	LOCK();
	i = index_of(uuid);
	if ( i >= 0 ) {
		obs[i]->opens++;
		obs[i]->opened = TRUE;
		h = (INT)( ( obs[i]->gen << OBS_H_SHIFT ) | (UINT)( i + 1 ) );
	}
	UNLOCK();
	return h;
}

LOCAL ER obs_close( INT h )
{
	OBSTELL	t;
	OBSEND	*e, *gone = NULL;
	OBSKEY	*k = NULL;
	INT	nk = 0;

	t.n = 0;
	LOCK();
	e = end_of(h);
	if ( e != NULL && --e->opens <= 0 ) {
		gone = end_take(slot_of(h), &t, &k, &nk);
	}
	UNLOCK();
	if ( e == NULL ) {
		return E_ID;
	}
	if ( gone != NULL ) {
		tell_out(&t);
		keys_drop(k, nk);
		end_free(gone);
	}
	return E_OK;
}

LOCAL UD obs_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UD)(UW)t.lo;
}

/*
 * A wait for the next change of end h, the flag cleared under the lock
 * by the caller before it looked; up to what is left of tmo since start
 */
LOCAL ER obs_wait( ID flg, TMO tmo, UD start )
{
	BOOL	sliced = (BOOL)( ts_get_pid() > 0 );
	UINT	ptn;
	TMO	t = tmo;
	ER	er;

	for ( ;; ) {
		if ( tmo != TMO_FEVR ) {
			UD	gone = obs_now() - start;

			if ( gone >= (UD)tmo ) return E_TMOUT;
			t = (TMO)( (UD)tmo - gone );
		}
		if ( sliced && ( t == TMO_FEVR || t > OBS_SLICE_MS ) ) t = OBS_SLICE_MS;
		er = tk_wai_flg(flg, 1, TWF_ORW, &ptn, t);
		if ( er != E_TMOUT ) {
			return ( er < E_OK ) ? E_DLT : E_OK;
		}
		if ( sliced && knl_prc_ending() ) {
			return E_DISWAI;
		}
	}
}

/* The keys a read of n bytes from place rpos takes, and how far it may read */
LOCAL INT read_bound( OBSEND *e, INT n, INT *p_nk )
{
	INT	k = 0;

	while ( k < e->nkeys && e->keys[k].at <= e->rpos ) k++;
	if ( k < e->nkeys && e->keys[k].at < e->rpos + (UD)n ) {
		n = (INT)( e->keys[k].at - e->rpos );
	}
	*p_nk = k;
	return n;
}

/*
 * Bytes read from end h, waited for up to tmo. With m (record 3) the
 * keys that came with them become the reader's: room entries of m->key.
 */
LOCAL ER obs_get( INT h, UB *dst, SZ size, SZ *p_asize, TMO tmo, T_OBSTMSG *m )
{
	OBSTELL	t;
	OBSEND	*e, *p;
	OBSKEY	*taken = NULL;
	UD	start = ( tmo > 0 ) ? obs_now() : 0;
	INT	n = 0, nk = 0, i, first;
	BOOL	full;
	ER	er = E_OK;

	if ( size < 0 ) {
		return E_PAR;
	}
	for ( ;; ) {
		t.n = 0;
		LOCK();
		e = end_of(h);
		if ( e == NULL ) {
			UNLOCK();
			return E_NOEXS;
		}
		if ( e->len > 0 || e->eof || e->peer < 0 || size == 0 ) {
			break;
		}
		(void)tk_clr_flg(e->flg, 0);
		UNLOCK();
		if ( tmo == TMO_POL ) {
			return E_TMOUT;
		}
		er = obs_wait(e->flg, tmo, start);
		if ( er < E_OK ) {
			return er;
		}
	}
	/* under the lock, with something to give or the end reached */
	full = (BOOL)( e->len == OB_ST_BUF );
	n = ( size < (SZ)e->len ) ? (INT)size : e->len;
	n = read_bound(e, n, &nk);
	if ( nk > 0 ) {
		taken = (OBSKEY *)Kmalloc(sizeof(OBSKEY) * (SZ)nk);
		if ( taken == NULL ) {
			UNLOCK();
			return E_NOMEM;
		}
		knl_memcpy(taken, e->keys, (INT)sizeof(OBSKEY) * nk);
		e->nkeys -= nk;
		for ( i = 0; i < e->nkeys; i++ ) e->keys[i] = e->keys[i + nk];
	}
	first = OB_ST_BUF - e->head;
	if ( first > n ) first = n;
	knl_memcpy(dst, e->buf + e->head, first);
	knl_memcpy(dst + first, e->buf, n - first);
	e->head = ( e->head + n ) % OB_ST_BUF;
	e->len -= n;
	e->rpos += (UD)n;
	p = ( e->peer >= 0 ) ? obs[e->peer] : NULL;
	if ( full && n > 0 && p != NULL ) {
		tell_add(&t, p);		/* room came for the writer */
	}
	UNLOCK();
	tell_out(&t);

	/* the keys: the reader's own, or let go */
	if ( m != NULL ) {
		m->nkey = 0;
		m->lost = 0;
	}
	for ( i = 0; i < nk; i++ ) {
		if ( m != NULL && m->nkey < m->room ) {
			T_OBSTKEY	*o = &m->key[m->nkey];
			UINT		type = taken[i].k.type, sub = taken[i].k.sub;
			ID		key = knl_ob_key_give(&taken[i].k, ts_get_pid());

			if ( key > 0 ) {
				o->key = key;
				o->type = type;
				o->sub = sub;
				m->nkey++;
				continue;
			}
		} else {
			knl_ob_key_drop(&taken[i].k);
		}
		if ( m != NULL ) m->lost++;
	}
	if ( taken != NULL ) Kfree(taken);
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

/*
 * Bytes written to end h, for its peer, with keys held to go with the
 * first of them; waiting for room unless nowait. The keys are the
 * peer's once the first bytes are in, and are let go when nothing went.
 */
LOCAL ER obs_put( INT h, CONST UB *src, SZ size, SZ *p_asize, BOOL nowait,
		  T_OBKHOLD *held, INT nheld )
{
	OBSTELL	t;
	OBSEND	*e, *p;
	UD	start = 0;
	SZ	done = 0;
	INT	n, room, tail, first, i;
	BOOL	empty;
	ER	er = E_OK;

	while ( done < size || ( size == 0 && done == 0 && nheld == 0 ) ) {
		t.n = 0;
		LOCK();
		e = end_of(h);
		if ( e == NULL ) {
			UNLOCK();
			er = E_NOEXS;
			break;
		}
		p = ( e->peer >= 0 ) ? obs[e->peer] : NULL;
		if ( p == NULL || e->shut ) {
			UNLOCK();
			er = E_IO;
			break;
		}
		if ( size == 0 ) {
			UNLOCK();
			break;
		}
		room = OB_ST_BUF - p->len;
		if ( nheld > 0 && p->nkeys + nheld > OB_ST_KEY_HELD ) {
			UNLOCK();
			er = E_LIMIT;
			break;
		}
		if ( room == 0 ) {
			(void)tk_clr_flg(e->flg, 0);
			UNLOCK();
			if ( nowait ) {
				er = ( done > 0 ) ? E_OK : E_TMOUT;
				break;
			}
			er = obs_wait(e->flg, TMO_FEVR, start);
			if ( er < E_OK ) break;
			continue;
		}
		n = ( size - done < (SZ)room ) ? (INT)( size - done ) : room;
		empty = (BOOL)( p->len == 0 );
		if ( nheld > 0 ) {
			if ( p->keys == NULL ) {
				p->keys = (OBSKEY *)Kmalloc(sizeof(OBSKEY) * OB_ST_KEY_HELD);
				if ( p->keys == NULL ) {
					UNLOCK();
					er = E_NOMEM;
					break;
				}
			}
			for ( i = 0; i < nheld; i++ ) {
				p->keys[p->nkeys].at = p->rpos + (UD)p->len;
				p->keys[p->nkeys].k = held[i];
				p->nkeys++;
				held[i].hnd = -1;	/* the peer's now */
			}
			nheld = 0;
		}
		tail = ( p->head + p->len ) % OB_ST_BUF;
		first = OB_ST_BUF - tail;
		if ( first > n ) first = n;
		knl_memcpy(p->buf + tail, src + done, first);
		knl_memcpy(p->buf, src + done + first, n - first);
		p->len += n;
		done += n;
		if ( empty ) {
			tell_add(&t, p);
		}
		UNLOCK();
		tell_out(&t);
		if ( nowait ) break;
	}
	for ( i = 0; i < nheld; i++ ) {
		knl_ob_key_drop(&held[i]);
	}
	if ( p_asize != NULL ) {
		*p_asize = done;
	}
	return ( done > 0 ) ? E_OK : er;
}

/*
 * What holds for end h now, as a poll set asks (obj.h): bytes or keys to
 * read or the end reached, room to write, the other end gone, nothing
 * more to come
 */
LOCAL INT obs_pol( INT h, OBPWAIT *w )
{
	OBSEND	*e, *p;
	INT	r = 0;

	LOCK();
	e = end_of(h);
	if ( e == NULL ) {
		UNLOCK();
		return E_NOEXS;
	}
	p = ( e->peer >= 0 ) ? obs[e->peer] : NULL;
	if ( e->len > 0 || e->nkeys > 0 || p == NULL || e->eof ) r |= OB_PL_IN;
	if ( p != NULL && !e->shut && p->len < OB_ST_BUF ) r |= OB_PL_OUT;
	if ( p == NULL || ( e->eof && e->shut ) ) r |= OB_PL_HUP;
	if ( p == NULL || e->eof ) r |= OB_PL_RDHUP;
	if ( w != NULL ) knl_obpoll_link(&e->pw, w);
	UNLOCK();
	return r;
}

LOCAL ER obs_state( INT h, void *buf, SZ size, SZ *p_asize )
{
	T_OBSTSTAT	s;
	OBSEND		*e, *p;

	if ( size < (SZ)sizeof(s) ) {
		return E_PAR;
	}
	knl_memset(&s, 0, sizeof(s));
	LOCK();
	e = end_of(h);
	if ( e != NULL ) {
		p = ( e->peer >= 0 ) ? obs[e->peer] : NULL;
		if ( p != NULL ) {
			s.peer = p->uuid;
			s.flags |= OB_SS_PEER;
			s.room = ( e->shut ) ? 0 : (UD)( OB_ST_BUF - p->len );
		}
		if ( p == NULL || e->eof ) s.flags |= OB_SS_EOF;
		if ( e->shut ) s.flags |= OB_SS_SHUT;
		s.nkey = e->nkeys;
		s.ready = (UD)e->len;
	}
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	knl_memcpy(buf, &s, sizeof(s));
	if ( p_asize != NULL ) {
		*p_asize = sizeof(s);
	}
	return E_OK;
}

/* Record 0: what the end is, as xmlTAD */
LOCAL ER obs_text( INT h, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	t[OBS_TEXT_MAX];
	OBSEND	*e;
	INT	n, len, peer;
	UD	ready = 0;
	SZ	k = 0;

	LOCK();
	e = end_of(h);
	peer = ( e != NULL ) ? e->peer : -1;
	if ( e != NULL ) ready = (UD)e->len;
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	n = knl_oj_put(t, 0, sizeof(t), "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>stream: ");
	n = knl_oj_put_num(t, n, sizeof(t), (D)ready);
	n = knl_oj_put(t, n, sizeof(t), ( peer >= 0 ) ? " bytes to be read, the other end open. "
				       : " bytes to be read, the other end gone. ");
	n = knl_oj_put(t, n, sizeof(t), "Record 1 is the bytes</p></document></tad>");
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

/* Record 3 written: the head taken in first, then its keys held */
LOCAL ER msg_put( INT h, CONST void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	T_OBSTMSG	m;
	T_OBKHOLD	*held = NULL;
	TS_UUID		u, self, peer;
	OBSEND		*e;
	INT		i, n;
	SZ		head;
	ER		er = E_OK;

	knl_memset(&self, 0, sizeof(self));
	knl_memset(&peer, 0, sizeof(peer));
	if ( size < (SZ)sizeof(m) ) {
		return E_PAR;
	}
	knl_memcpy(&m, buf, sizeof(m));
	if ( m.room < 0 || m.room > OB_ST_KEY_MAX || m.nkey < 0 || m.nkey > m.room ) {
		return E_PAR;
	}
	head = (SZ)OB_STM_HEAD(m.room);
	if ( size < head || ( m.nkey > 0 && size == head ) ) {
		return E_PAR;
	}
	if ( m.flags & OB_STM_WAIT ) nowait = FALSE;
	if ( m.flags & OB_STM_NOWAIT ) nowait = TRUE;
	LOCK();
	e = end_of(h);
	if ( e != NULL ) {
		self = e->uuid;
		if ( e->peer >= 0 ) peer = obs[e->peer]->uuid;
	}
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	n = 0;
	if ( m.nkey > 0 ) {
		held = (T_OBKHOLD *)Kmalloc(sizeof(T_OBKHOLD) * (SZ)m.nkey);
		if ( held == NULL ) {
			return E_NOMEM;
		}
		for ( ; n < m.nkey; n++ ) {
			T_OBSTKEY	k;

			knl_memcpy(&k, (CONST UB *)buf + sizeof(T_OBSTMSG) + sizeof(T_OBSTKEY) * (SZ)n,
				   sizeof(k));
			er = knl_ob_key_hold(k.key, &held[n], &u);
			if ( er < E_OK ) break;
			if ( same_uuid(&u, &self) || same_uuid(&u, &peer) ) {
				knl_ob_key_drop(&held[n]);
				er = E_PAR;		/* the pair inside itself would never close */
				break;
			}
		}
	}
	if ( er >= E_OK ) {
		er = obs_put(h, (CONST UB *)buf + head, size - head, p_asize, nowait, held, n);
	} else {
		for ( i = 0; i < n; i++ ) knl_ob_key_drop(&held[i]);
	}
	if ( held != NULL ) Kfree(held);
	return er;
}

LOCAL ER obs_rtm( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	if ( off != 0 && recno != 0 ) {
		return E_PAR;
	}
	switch ( recno ) {
	case 0:
		return obs_text(h, off, buf, size, p_asize);
	case OB_ST_DATA:
		return obs_get(h, (UB *)buf, size, p_asize, tmo, NULL);
	case OB_ST_STATE:
		return obs_state(h, buf, size, p_asize);
	case OB_ST_MSG: {
		T_OBSTMSG	m;
		SZ		head;
		ER		er;

		if ( size < (SZ)sizeof(m) ) return E_PAR;
		knl_memcpy(&m, buf, sizeof(m));
		if ( m.room < 0 || m.room > OB_ST_KEY_MAX ) return E_PAR;
		head = (SZ)OB_STM_HEAD(m.room);
		if ( size < head ) return E_PAR;
		if ( m.flags & OB_STM_NOWAIT ) tmo = TMO_POL;
		else if ( ( m.flags & OB_STM_WAIT ) != 0 && tmo == TMO_POL ) tmo = TMO_FEVR;
		er = obs_get(h, (UB *)buf + head, size - head, p_asize, tmo, (T_OBSTMSG *)buf);
		return er;
	}
	default:
		return E_NOEXS;
	}
}

LOCAL ER obs_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	return obs_rtm(h, recno, off, buf, size, p_asize, nowait ? TMO_POL : TMO_FEVR);
}

LOCAL ER obs_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBSTELL	t;
	OBSEND	*e;
	INT	how;

	if ( off != 0 || size < 0 ) {
		return E_PAR;
	}
	switch ( recno ) {
	case 0:
		return E_RONLY;
	case OB_ST_DATA:
		return obs_put(h, (CONST UB *)buf, size, p_asize, nowait, NULL, 0);
	case OB_ST_MSG:
		return msg_put(h, buf, size, p_asize, nowait);
	case OB_ST_STATE:
		if ( size < (SZ)sizeof(how) ) return E_PAR;
		knl_memcpy(&how, buf, sizeof(how));
		if ( how != OB_SHUT_WR ) return E_PAR;
		t.n = 0;
		LOCK();
		e = end_of(h);
		if ( e != NULL && !e->shut ) {
			e->shut = TRUE;
			tell_add(&t, e);
			if ( e->peer >= 0 ) {
				obs[e->peer]->eof = TRUE;
				tell_add(&t, obs[e->peer]);
			}
		}
		UNLOCK();
		tell_out(&t);
		if ( e == NULL ) return E_NOEXS;
		if ( p_asize != NULL ) *p_asize = sizeof(how);
		return E_OK;
	default:
		return E_NOEXS;
	}
}

LOCAL ER obs_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBSEND	*e;
	UD	ready;
	INT	k;

	LOCK();
	e = end_of(h);
	ready = ( e != NULL ) ? (UD)e->len : 0;
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	for ( k = 0; k < n && k < OB_ST_NREC; k++ ) {
		buf[k].recno = k;
		buf[k].rt = ( k == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA;
		buf[k].sub = 0;
		buf[k].size = ( k == OB_ST_DATA ) ? ready : ( k == OB_ST_STATE ) ? sizeof(T_OBSTSTAT) : 0;
	}
	*p_cnt = k;
	return E_OK;
}

LOCAL ER obs_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBSEND	*e;
	TS_UUID	peer;
	BOOL	has = FALSE;
	UD	ready = 0;
	INT	n;

	LOCK();
	e = end_of(h);
	if ( e != NULL ) {
		ready = (UD)e->len;
		if ( e->peer >= 0 ) {
			peer = obs[e->peer]->uuid;
			has = TRUE;
		}
	}
	UNLOCK();
	if ( e == NULL ) {
		return E_NOEXS;
	}
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":\"\",\"stream\":{\"peer\":");
	if ( has ) {
		n = knl_oj_put(json, n, (INT)size, "\"");
		n = knl_oj_put_uuid(json, n, (INT)size, &peer);
		n = knl_oj_put(json, n, (INT)size, "\"");
	} else {
		n = knl_oj_put(json, n, (INT)size, "null");
	}
	n = knl_oj_put(json, n, (INT)size, ",\"waiting\":");
	n = knl_oj_put_num(json, n, (INT)size, (D)ready);
	n = knl_oj_put(json, n, (INT)size, "}}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) {
		*p_asize = n;
	}
	return E_OK;
}

LOCAL ER obs_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	LOCK();
	while ( cnt < n ) {
		INT	best = -1;

		for ( i = 0; i < OBS_MAX; i++ ) {
			if ( obs[i] == NULL ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&obs[i]->uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&obs[i]->uuid, from) <= 0 ) continue;
			if ( best < 0 || ts_uuid_cmp(&obs[i]->uuid, &obs[best]->uuid) < 0 ) best = i;
		}
		if ( best < 0 ) break;
		buf[cnt++] = obs[best]->uuid;
	}
	UNLOCK();
	*p_cnt = cnt;
	return E_OK;
}

/*
 * Ends a process made and never opened go with it (knl_ob_prc_end).
 * The table is searched under one take of the lock, which is let go
 * only to free an end found: every process that ends comes here, and a
 * take for each of the OBS_MAX slots would keep the lock passing among
 * the ending processes so often that the reaper, waiting its turn for
 * each slot, falls behind them without end.
 */
EXPORT void knl_obstrm_prc_end( ID pid )
{
	INT	i = 0;

	if ( obs_mtx <= 0 || pid <= 0 ) {
		return;
	}
	while ( i < OBS_MAX ) {
		OBSTELL	t;
		OBSEND	*e = NULL;
		OBSKEY	*k = NULL;
		INT	nk = 0;

		t.n = 0;
		LOCK();
		for ( ; i < OBS_MAX && e == NULL; i++ ) {
			if ( obs[i] != NULL && !obs[i]->opened && obs[i]->maker == pid ) {
				e = end_take(i, &t, &k, &nk);
			}
		}
		UNLOCK();
		if ( e != NULL ) {
			tell_out(&t);
			keys_drop(k, nk);
			end_free(e);
		}
	}
}

LOCAL CONST T_OBMGR obs_mgr = {
	OB_T_CHANNEL, OB_S_STREAM, "stream",
	obs_find, obs_ref, obs_prot, obs_setprot, obs_create, obs_remove,
	obs_open, obs_close, obs_rea, obs_wri, NULL, NULL, NULL, obs_lrc,
	obs_gat, NULL, NULL, obs_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	obs_rtm,
	obs_pol
};

EXPORT ER knl_obstrm_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obs_mtx = tk_cre_mtx(&cmtx);
	if ( obs_mtx <= 0 ) {
		return (ER)obs_mtx;
	}
	no = knl_ob_regist(&obs_mgr);
	return ( no > 0 ) ? E_OK : (ER)no;
}
