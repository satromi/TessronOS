/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsjrnl.c
 *	Write ahead log of a native volume (design 11.14.1, docs/tessronos 7.3).
 *
 *	The journal area is a head block and a ring behind it:
 *
 *		head block	where the oldest transaction not yet put in
 *				place starts, and its number
 *		ring		transactions, one after another, wrapping
 *
 *	One transaction in the ring:
 *
 *		descriptor	where each copy belongs, and the CRC of each
 *		copies		the blocks as the transaction left them
 *		revoke		(when needed) blocks freed by the transaction
 *				whose older copies must not come back
 *		commit		the number, the time, and one CRC over the
 *				CRCs of everything before it
 *
 *	A transaction goes through three states. It is built while handles
 *	are on it; when the last handle lets go it is closed: its copies
 *	join the committed blocks in memory, where every later read finds
 *	them, and it waits in a queue for the ring. Closing writes nothing
 *	to the device, so the caller's lock of the volume is not held for
 *	the device's time. A closed transaction is made real (durable) by
 *	whoever waits for it first: everything queued goes into the ring at
 *	once (group commit), each transaction one request, between two
 *	flushes of the device's cache. The first flush puts the data the
 *	transactions point at on the medium before any commit block that
 *	points at it; the commit blocks need no flush of their own before
 *	them, since a replay checks every copy against the CRC its
 *	descriptor holds and stops at the first that does not match.
 *
 *	The committed copies are put in place later, together (a
 *	checkpoint), after which the head block moves past them. Only
 *	copies of transactions already in the ring are put in place: the
 *	queue is written first. The checkpoint holds the journal's lock
 *	only to look and to tidy; the copies are written without it.
 *
 *	Locks, outer first: the block layer's lock of the volume, then
 *	`iomtx` (one writer of the ring and of the places at a time), then
 *	`mtx` (the state; never held over a device request). The journal
 *	reads and writes the device past the volume's lock
 *	(knl_tsfsblk_raw_*), because the block layer asks here for copies
 *	while holding it.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/dt.h>
#include "tstdlib.h"

#ifndef CNF_TSFS_FLUSH_MS
#define CNF_TSFS_FLUSH_MS	5000	/* committed blocks are put in place at least this often */
#endif

#define TSFSJ_MAX_VOL		TSFSBLK_MAX_VOL
#define CACHE_BUCKETS		256
#define CACHE_MAX		512	/* committed blocks held before the checkpointer is woken */
#define REPLAY_MAX_REVOKE	4096

/* ---------------------------------------------------------------- layout */

/* the head block */
#define JH_MAGIC	"TFJH"
#define JH_VERSION	4
#define JH_SEQ		8
#define JH_POS		16
#define JH_CRC		24

/* a block of the ring that is not a copy */
#define JB_DESC		"TFJD"
#define JB_REVOKE	"TFJR"
#define JB_COMMIT	"TFJC"
#define JB_MAGIC	0
#define JB_VERSION	4
#define JB_SEQ		8
#define JB_COUNT	16
#define JB_CRC		24
#define JB_ENT		TSFSJ_HDR_SIZE
#define JC_TIME		32
#define JC_CHAIN	40
#define JC_BLOCKS	44

typedef struct cnode {
	struct cnode	*next;
	UD		blk;
	UB		*buf;
} CNODE;

typedef struct {
	UD	blk;
	UB	*buf;
} JENT;

/* A closed transaction waiting for the ring */
typedef struct ptxn {
	struct ptxn	*next;
	UD		seq;
	UD		foot;		/* ring blocks it takes */
	INT		n;
	JENT		*ent;		/* its copies, its own */
	INT		nrevoke;
	UD		*revoke;
} PTXN;

/* A buffer let go while a checkpoint may still be writing it */
typedef struct dfree {
	struct dfree	*next;
	UB		*buf;
} DFREE;

typedef struct {
	ID	vol;			/* 0: the entry is free */
	ID	mtx;			/* the state */
	ID	iomtx;			/* the ring and the places: one writer */
	UD	start, blocks;		/* the journal area */
	UD	len;			/* blocks of the ring */
	UD	head, tail;		/* places in the ring */
	UD	used;			/* ring blocks between head and tail */
	UD	head_seq;		/* number of the transaction at head */
	UD	seq;			/* the number the next one takes */
	UD	dseq;			/* every one numbered below this is in the ring */
	INT	handles;		/* on the running transaction */
	BOOL	committing;		/* the last handle let go; it is being closed */
	BOOL	frozen;			/* no transaction begins: the volume is being grown */
	BOOL	doomed;			/* a handle gave up: the whole of it goes */
	JENT	txn[TSFSJ_MAX_BLOCKS];
	INT	ntxn;
	UD	revoke[TSFSJ_MAX_REVOKE];
	INT	nrevoke;
	BOOL	revoke_over;		/* more than one block of revokes */
	PTXN	*pend;			/* closed, not in the ring yet, oldest first */
	PTXN	**pend_end;
	UD	pend_foot;		/* ring blocks they will take */
	CNODE	*bucket[CACHE_BUCKETS];
	UD	ncached;
	BOOL	ckpt_on;		/* a checkpoint is writing copies */
	JENT	*snap;			/* the copies it is writing */
	UD	nsnap;
	DFREE	*dfree;			/* buffers let go meanwhile */
	UINT	fault;
	BOOL	dead;			/* after TSFSJ_FAULT_AFTER_COMMIT */
	BOOL	failed;			/* the ring could not be written: nothing goes in place */
	BOOL	told_nolog;		/* a change refused for want of the log, told once */
	UW	salt;			/* of this volume, mixed into every CRC */
	UD	closed;			/* transactions closed, for ts_jrnl_ref */
	UD	batches;		/* times the queue went into the ring */
} TSFSJRNL;

/*
 * The journal of a volume sits at the volume's number less one. Its
 * entry is made when that number first has a journal and is kept for
 * whichever volume has the number next, so that the checkpointer may
 * look at every entry without a lock of the table.
 */
LOCAL TSFSJRNL	*jrnl_tbl[TSFSJ_MAX_VOL];
LOCAL ID	jrnl_tsk = 0;

/* ---------------------------------------------------------------- bytes */

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ((UD)rd32(p + 4) << 32);
}

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8);
	p[2] = (UB)(v >> 16); p[3] = (UB)(v >> 24);
}

LOCAL void wr64( UB *p, UD v )
{
	wr32(p, (UW)v);
	wr32(p + 4, (UW)(v >> 32));
}

/*
 * The CRC of a block whose own CRC sits at `off`, that field taken as
 * zero, mixed with the volume's salt: a ring left over from a volume
 * made before on the same blocks does not pass as this one's.
 */
LOCAL UW crc_at( CONST TSFSJRNL *j, UB *buf, INT off )
{
	UW	saved = rd32(buf + off);
	UW	c;

	wr32(buf + off, 0);
	c = ts_crc32c(buf, TSFSBLK_BLOCK_SIZE) ^ j->salt;
	wr32(buf + off, saved);

	return c;
}

LOCAL BOOL is_kind( CONST UB *buf, CONST char *magic )
{
	return (BOOL)( buf[0] == (UB)magic[0] && buf[1] == (UB)magic[1]
		    && buf[2] == (UB)magic[2] && buf[3] == (UB)magic[3]
		    && rd32(buf + JB_VERSION) == TSFSJ_VERSION );
}

LOCAL void make_kind( UB *buf, CONST char *magic, UD seq, UW count )
{
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	buf[0] = (UB)magic[0]; buf[1] = (UB)magic[1];
	buf[2] = (UB)magic[2]; buf[3] = (UB)magic[3];
	wr32(buf + JB_VERSION, TSFSJ_VERSION);
	wr64(buf + JB_SEQ, seq);
	wr32(buf + JB_COUNT, count);
}

/* ---------------------------------------------------------------- the table */

LOCAL TSFSJRNL *jrnl_find( ID vol )
{
	TSFSJRNL	*j;

	if ( vol < 1 || vol > TSFSJ_MAX_VOL ) {
		return NULL;
	}
	j = __atomic_load_n(&jrnl_tbl[vol - 1], __ATOMIC_ACQUIRE);
	return ( j != NULL && j->vol == vol ) ? j : NULL;
}

LOCAL void checkpointer( INT stacd, void *exinf );

LOCAL TSFSJRNL *jrnl_make( ID vol )
{
	T_CMTX	cm;
	T_CTSK	ct;
	TSFSJRNL	*j = jrnl_find(vol), *none = NULL;

	if ( j != NULL ) {
		return j;
	}
	if ( vol < 1 || vol > TSFSJ_MAX_VOL ) {
		return NULL;
	}
	j = __atomic_load_n(&jrnl_tbl[vol - 1], __ATOMIC_ACQUIRE);
	if ( j == NULL ) {
		j = (TSFSJRNL *)Kcalloc(1, sizeof(TSFSJRNL));
		if ( j == NULL ) {
			return NULL;
		}
		if ( !__atomic_compare_exchange_n(&jrnl_tbl[vol - 1], &none, j, FALSE,
						  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE) ) {
			Kfree(j);
			j = none;		/* made meanwhile */
		}
	}
	knl_memset(j, 0, sizeof(*j));
	j->pend_end = &j->pend;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cm.ceilpri = 0;
	j->mtx = tk_cre_mtx(&cm);
	if ( j->mtx <= 0 ) {
		return NULL;
	}
	j->iomtx = tk_cre_mtx(&cm);
	if ( j->iomtx <= 0 ) {
		tk_del_mtx(j->mtx);
		return NULL;
	}
	j->vol = vol;

	/* one task puts committed blocks in place for every volume */
	if ( jrnl_tsk <= 0 ) {
		ct.exinf = NULL;
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)checkpointer;
		ct.itskpri = CNF_MAX_TSKPRI - 1;	/* behind everything, and still a priority there is */
		ct.stksz = 8192;
		jrnl_tsk = tk_cre_tsk(&ct);
		if ( jrnl_tsk > 0 ) {
			tk_sta_tsk(jrnl_tsk, 0);
		} else {
			tm_printf((UB *)"tsfs: the checkpointer could not be made (%d)\n", (INT)jrnl_tsk);
		}
	}
	return j;
}

LOCAL void txn_drop( TSFSJRNL *j )
{
	INT	i;

	for ( i = 0; i < j->ntxn; i++ ) {
		Kfree(j->txn[i].buf);
	}
	j->ntxn = 0;
	j->nrevoke = 0;
	j->revoke_over = FALSE;
	j->doomed = FALSE;
}

LOCAL void ptxn_free( PTXN *t )
{
	INT	i;

	for ( i = 0; i < t->n; i++ ) {
		Kfree(t->ent[i].buf);
	}
	if ( t->ent != NULL ) Kfree(t->ent);
	if ( t->revoke != NULL ) Kfree(t->revoke);
	Kfree(t);
}

/* ---------------------------------------------------------------- committed blocks */

/*
 * A buffer of a committed copy given up. While a checkpoint is writing
 * copies without the lock, the one it may be writing is kept until it
 * is done.
 */
LOCAL void buf_drop( TSFSJRNL *j, UB *buf )
{
	DFREE	*d;

	if ( !j->ckpt_on ) {
		Kfree(buf);
		return;
	}
	d = (DFREE *)Kmalloc(sizeof(DFREE));
	if ( d == NULL ) {
		return;				/* kept for good rather than freed under a writer */
	}
	d->buf = buf;
	d->next = j->dfree;
	j->dfree = d;
}

LOCAL CNODE **cache_slot( TSFSJRNL *j, UD blk )
{
	CNODE	**pp = &j->bucket[blk % CACHE_BUCKETS];

	while ( *pp != NULL && (*pp)->blk != blk ) {
		pp = &(*pp)->next;
	}
	return pp;
}

/* The copy a transaction committed takes the place of an older one */
LOCAL ER cache_put( TSFSJRNL *j, UD blk, UB *buf )
{
	CNODE	**pp = cache_slot(j, blk);
	CNODE	*n = *pp;

	if ( n != NULL ) {
		buf_drop(j, n->buf);
		n->buf = buf;
		return E_OK;
	}
	n = (CNODE *)Kmalloc(sizeof(CNODE));
	if ( n == NULL ) {
		return E_NOMEM;
	}
	n->blk = blk;
	n->buf = buf;
	n->next = j->bucket[blk % CACHE_BUCKETS];
	j->bucket[blk % CACHE_BUCKETS] = n;
	j->ncached++;
	return E_OK;
}

LOCAL void cache_del( TSFSJRNL *j, UD blk )
{
	CNODE	**pp = cache_slot(j, blk);
	CNODE	*n = *pp;

	if ( n != NULL ) {
		*pp = n->next;
		buf_drop(j, n->buf);
		Kfree(n);
		j->ncached--;
	}
}

LOCAL void cache_clear( TSFSJRNL *j )
{
	CNODE	*n, *next;
	INT	i;

	for ( i = 0; i < CACHE_BUCKETS; i++ ) {
		for ( n = j->bucket[i]; n != NULL; n = next ) {
			next = n->next;
			buf_drop(j, n->buf);
			Kfree(n);
		}
		j->bucket[i] = NULL;
	}
	j->ncached = 0;
}

/* ---------------------------------------------------------------- the ring */

LOCAL UD ring_blk( CONST TSFSJRNL *j, UD pos )
{
	return j->start + 1 + ( pos % j->len );
}

/*
 * Blocks laid out from place `pos` of the ring on, in one request up to
 * where it wraps. With `fua`, each request is on the medium when it
 * completes if the device can (*p_fua stays TRUE only when every one was).
 */
LOCAL ER ring_put( TSFSJRNL *j, UD pos, CONST UB *buf, UD n, BOOL fua, BOOL *p_fua )
{
	UD	first, k;
	BOOL	f = FALSE;
	ER	er = E_OK;

	while ( n > 0 && er >= E_OK ) {
		first = pos % j->len;
		k = j->len - first;
		if ( k > n ) k = n;
		if ( fua ) {
			er = knl_tsfsblk_raw_writes_fua(j->vol, j->start + 1 + first, k, buf, &f);
			if ( !f && p_fua != NULL ) *p_fua = FALSE;
		} else {
			er = knl_tsfsblk_raw_writes(j->vol, j->start + 1 + first, k, buf);
		}
		pos += k;
		buf += k * TSFSBLK_BLOCK_SIZE;
		n -= k;
	}
	return er;
}

LOCAL ER head_write( TSFSJRNL *j, UD head, UD head_seq )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	ER	er;

	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	buf[0] = (UB)JH_MAGIC[0]; buf[1] = (UB)JH_MAGIC[1];
	buf[2] = (UB)JH_MAGIC[2]; buf[3] = (UB)JH_MAGIC[3];
	wr32(buf + JH_VERSION, TSFSJ_VERSION);
	wr64(buf + JH_SEQ, head_seq);
	wr64(buf + JH_POS, head);
	wr32(buf + JH_CRC, crc_at(j, buf, JH_CRC));
	er = knl_tsfsblk_raw_write(j->vol, j->start, buf);
	Kfree(buf);
	return er;
}

/*
 * One closed transaction laid out as the ring holds it, in `out` (foot
 * blocks): descriptor, copies, revoke block when it has revokes, and
 * the commit block unless `commit` is FALSE (a pretended cut before it).
 */
LOCAL void txn_lay( CONST TSFSJRNL *j, CONST PTXN *t, UB *out, BOOL commit )
{
	UB	*b = out;
	UW	chain, c;
	INT	i;

	/* descriptor */
	make_kind(b, JB_DESC, t->seq, (UW)t->n);
	for ( i = 0; i < t->n; i++ ) {
		wr64(b + JB_ENT + i * TSFSJ_DESC_ENT, t->ent[i].blk);
		wr32(b + JB_ENT + i * TSFSJ_DESC_ENT + 8,
		     ts_crc32c(t->ent[i].buf, TSFSBLK_BLOCK_SIZE));
	}
	c = crc_at(j, b, JB_CRC);
	wr32(b + JB_CRC, c);
	chain = ts_crc32c((CONST UB *)&c, 4);
	b += TSFSBLK_BLOCK_SIZE;

	/* copies */
	for ( i = 0; i < t->n; i++ ) {
		knl_memcpy(b, t->ent[i].buf, TSFSBLK_BLOCK_SIZE);
		c = ts_crc32c(b, TSFSBLK_BLOCK_SIZE);
		chain ^= ts_crc32c((CONST UB *)&c, 4) + (UW)i;
		b += TSFSBLK_BLOCK_SIZE;
	}

	/* revokes */
	if ( t->nrevoke > 0 ) {
		make_kind(b, JB_REVOKE, t->seq, (UW)t->nrevoke);
		for ( i = 0; i < t->nrevoke; i++ ) {
			wr64(b + JB_ENT + i * 8, t->revoke[i]);
		}
		c = crc_at(j, b, JB_CRC);
		wr32(b + JB_CRC, c);
		chain ^= ts_crc32c((CONST UB *)&c, 4) + 0x5A5A5A5AU;
		b += TSFSBLK_BLOCK_SIZE;
	}

	/* commit */
	if ( commit ) {
		TS_TIME	tm = 0;

		make_kind(b, JB_COMMIT, t->seq, 0);
		(void)dt_gettime(&tm);
		wr64(b + JC_TIME, (UD)tm);
		wr32(b + JC_CHAIN, chain);
		wr32(b + JC_BLOCKS, (UW)( t->foot - 1 ));
		wr32(b + JB_CRC, crc_at(j, b, JB_CRC));
	}
}

/*
 * The transactions of the queue, `t` on, laid out from `pos` of the
 * ring on. A batch of committed ones is laid out whole and written in
 * one request (two where the ring wraps), on the medium when it
 * completes when the device can: *p_fua says whether it was, and when
 * it was, the cache need not be emptied after (pend_write). One that
 * is too large for memory, and those of a pretended cut, go one by one.
 */
LOCAL ER ring_txns( TSFSJRNL *j, PTXN *t, UD pos, BOOL commit, BOOL *p_fua )
{
	UB	*buf = NULL;
	UD	room = 0, all = 0, at;
	PTXN	*u;
	ER	er = E_OK;

	if ( p_fua != NULL ) *p_fua = FALSE;
	if ( commit ) {
		for ( u = t; u != NULL; u = u->next ) all += u->foot;
		buf = ( all > 0 ) ? (UB *)Kmalloc((SZ)( all * TSFSBLK_BLOCK_SIZE )) : NULL;
	}
	if ( buf != NULL ) {
		for ( u = t, at = 0; u != NULL; u = u->next ) {
			txn_lay(j, u, buf + at * TSFSBLK_BLOCK_SIZE, TRUE);
			at += u->foot;
		}
		if ( p_fua != NULL ) *p_fua = TRUE;
		er = ring_put(j, pos, buf, all, TRUE, p_fua);
		Kfree(buf);
		return er;
	}
	for ( ; t != NULL && er >= E_OK; t = t->next ) {
		UD	n = commit ? t->foot : t->foot - 1;

		if ( n > room ) {
			if ( buf != NULL ) Kfree(buf);
			buf = (UB *)Kmalloc((SZ)( t->foot * TSFSBLK_BLOCK_SIZE ));
			room = ( buf != NULL ) ? t->foot : 0;
			if ( buf == NULL ) {
				er = E_NOMEM;
				break;
			}
		}
		txn_lay(j, t, buf, commit);
		er = ring_put(j, pos, buf, n, FALSE, NULL);
		pos += t->foot;
	}
	if ( buf != NULL ) Kfree(buf);
	return er;
}

/*
 * The ring could not be written. What was closed is in memory only and
 * may be half on the medium: none of it may go in place, and the
 * volume stops writing.
 */
LOCAL void ring_failed( TSFSJRNL *j )
{
	j->failed = TRUE;
	tm_printf((UB *)"tsfs: volume %d: the log could not be written: the volume stops writing\n",
		  (INT)j->vol);
	knl_tsfsblk_stop(j->vol, j->start);
}

/*
 * Everything queued goes into the ring: the data the transactions point
 * at reaches the medium first, then every transaction, then the commit
 * blocks are made sure of. The caller holds iomtx.
 */
LOCAL ER pend_write( TSFSJRNL *j )
{
	PTXN	*list, *t;
	UD	pos, foot = 0, last = 0;
	BOOL	fua = FALSE;
	ER	er;

	while ( __atomic_load_n(&j->fault, __ATOMIC_ACQUIRE) == TSFSJ_FAULT_HOLD ) {
		tk_dly_tsk(5);			/* a test holds the writer back */
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( j->failed ) {
		tk_unl_mtx(j->mtx);
		return E_IO;
	}
	list = j->pend;
	if ( list == NULL ) {
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	j->pend = NULL;
	j->pend_end = &j->pend;
	pos = j->tail;
	tk_unl_mtx(j->mtx);

	for ( t = list; t != NULL; t = t->next ) {
		foot += t->foot;
		last = t->seq;
	}
	/* the data the transactions point at on the medium first; then the
	   batch, which needs the cache emptied after it only when the device
	   could not write it through to the medium (FUA) */
	er = knl_tsfsblk_raw_flush(j->vol);
	if ( er >= E_OK ) er = ring_txns(j, list, pos, TRUE, &fua);
	if ( er >= E_OK && !fua ) er = knl_tsfsblk_raw_flush(j->vol);

	tk_loc_mtx(j->mtx, TMO_FEVR);
	j->pend_foot -= foot;
	if ( er >= E_OK ) {
		j->tail = ( j->tail + foot ) % j->len;
		j->used += foot;
		j->dseq = last + 1;
		j->batches++;
	} else {
		ring_failed(j);
	}
	tk_unl_mtx(j->mtx);

	while ( list != NULL ) {
		t = list->next;
		ptxn_free(list);
		list = t;
	}
	if ( er >= E_OK ) {
		knl_tsfsblk_unbusy(j->vol, last + 1);
	}
	return ( er >= E_OK ) ? E_OK : E_IO;
}

/*
 * Every committed block in its place, then the head moved past all of
 * it. The queue goes into the ring first, so that nothing is put in
 * place before the record of it; the copies are then written without
 * the lock, and the device's cache is emptied before the head moves, so
 * the head never gets ahead of what it stands for. The caller holds
 * iomtx and not mtx.
 */
LOCAL ER ckpt_io( TSFSJRNL *j )
{
	CNODE	*n;
	JENT	*snap = NULL;
	UD	k = 0, i, head;
	INT	b;
	ER	er = E_OK;

	for ( ;; ) {
		er = pend_write(j);
		if ( er < E_OK ) {
			return er;
		}
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( j->pend == NULL ) {
			break;				/* still held: nothing closes past this */
		}
		tk_unl_mtx(j->mtx);
	}
	if ( j->dead || j->failed ) {
		tk_unl_mtx(j->mtx);
		return j->failed ? E_IO : E_OK;
	}
	if ( j->ncached == 0 && j->used == 0 ) {
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	if ( j->ncached > 0 ) {
		snap = (JENT *)Kmalloc(sizeof(JENT) * (SZ)j->ncached);
		if ( snap == NULL ) {
			tk_unl_mtx(j->mtx);
			return E_NOMEM;
		}
		for ( b = 0; b < CACHE_BUCKETS; b++ ) {
			for ( n = j->bucket[b]; n != NULL; n = n->next ) {
				snap[k].blk = n->blk;
				snap[k].buf = n->buf;
				k++;
			}
		}
	}
	head = j->tail;
	j->ckpt_on = TRUE;
	j->snap = snap;
	j->nsnap = k;
	tk_unl_mtx(j->mtx);

	for ( i = 0; i < k && er >= E_OK; i++ ) {
		er = knl_tsfsblk_raw_write(j->vol, snap[i].blk, snap[i].buf);
	}
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	/* no ring write runs meanwhile (iomtx): the ring is empty after this */
	if ( er >= E_OK ) er = head_write(j, head, j->dseq);
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);

	tk_loc_mtx(j->mtx, TMO_FEVR);
	j->ckpt_on = FALSE;
	j->snap = NULL;
	j->nsnap = 0;
	if ( er >= E_OK ) {
		/* a copy closed meanwhile over one written stays for the next time */
		for ( i = 0; i < k; i++ ) {
			CNODE	**pp = cache_slot(j, snap[i].blk);

			if ( *pp != NULL && (*pp)->buf == snap[i].buf ) {
				n = *pp;
				*pp = n->next;
				Kfree(n->buf);
				Kfree(n);
				j->ncached--;
			}
		}
		j->head = head;
		j->head_seq = j->dseq;
		j->used = 0;
	}
	while ( j->dfree != NULL ) {
		DFREE	*d = j->dfree;

		j->dfree = d->next;
		Kfree(d->buf);
		Kfree(d);
	}
	tk_unl_mtx(j->mtx);
	if ( snap != NULL ) Kfree(snap);

	return er;
}

LOCAL ER ckpt_all( TSFSJRNL *j )
{
	ER	er;

	tk_loc_mtx(j->iomtx, TMO_FEVR);
	er = ckpt_io(j);
	tk_unl_mtx(j->iomtx);
	return er;
}

/* ---------------------------------------------------------------- the checkpointer */

LOCAL void checkpointer( INT stacd, void *exinf )
{
	INT	i;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		(void)tk_slp_tsk(CNF_TSFS_FLUSH_MS);	/* the time, or woken when memory or the ring runs short */
		for ( i = 0; i < TSFSJ_MAX_VOL; i++ ) {
			TSFSJRNL	*j = __atomic_load_n(&jrnl_tbl[i], __ATOMIC_ACQUIRE);

			if ( j == NULL || j->vol <= 0 || ( j->ncached == 0 && j->pend == NULL ) ) continue;
			if ( tk_loc_mtx(j->iomtx, TMO_POL) < E_OK ) continue;	/* a writer is at it */
			if ( j->vol > 0 ) {
				(void)ckpt_io(j);
			}
			tk_unl_mtx(j->iomtx);
		}
	}
}

LOCAL void wake_checkpointer( void )
{
	if ( jrnl_tsk > 0 ) {
		(void)tk_wup_tsk(jrnl_tsk);
	}
}

/* ---------------------------------------------------------------- building */

EXPORT ER ts_jrnl_begin( ID vol )
{
	TSFSJRNL	*j = jrnl_make(vol);
	BOOL	first = FALSE;

	if ( j == NULL ) {
		return E_LIMIT;
	}
	for ( ;; ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( j->dead || j->failed ) {
			tk_unl_mtx(j->mtx);
			return E_IO;
		}
		if ( j->len == 0 ) {
			/*
			 * The log was never put in place: the volume was opened
			 * with an error recorded (it is read only until checked,
			 * and its log is replayed then), or it has no log area.
			 * Nothing can be committed without the ring.
			 */
			if ( !j->told_nolog ) {
				j->told_nolog = TRUE;
				tm_printf((UB *)"tsfs: volume %d: its log is not in place (opened with an error recorded, "
					  "or no log area): changes refused until it is checked\n", (INT)vol);
			}
			tk_unl_mtx(j->mtx);
			return E_RONLY;
		}
		/* a handle joining the running transaction is let in while
		   frozen: the freeze waits for that transaction to end */
		if ( !j->committing && !( j->frozen && j->handles == 0 ) ) {
			break;
		}
		tk_unl_mtx(j->mtx);
		tk_dly_tsk(1);			/* the last one is being closed, or the volume grown */
	}
	if ( j->handles == 0 ) {
		txn_drop(j);
		first = TRUE;
	}
	j->handles++;
	tk_unl_mtx(j->mtx);

	/* the maps and the superblock wait for the commit too */
	if ( first ) {
		ts_blk_jrnl(vol, TRUE);
	}
	return E_OK;
}

/*
 * Put one metadata block in the running transaction. A block put in
 * twice is kept once, as it was last put.
 */
EXPORT ER ts_jrnl_write( ID vol, UD blk, CONST void *buf )
{
	TSFSJRNL	*j = jrnl_find(vol);
	UB	*copy;
	INT	i;
	ER	er = E_OK;

	if ( j == NULL || buf == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( j->handles == 0 && !j->committing ) {
		er = E_OBJ;			/* no transaction is being built */
		goto exit;
	}
	for ( i = 0; i < j->nrevoke; i++ ) {
		if ( j->revoke[i] == blk ) {	/* taken again: the copy supersedes */
			j->revoke[i] = j->revoke[--j->nrevoke];
			break;
		}
	}
	for ( i = 0; i < j->ntxn; i++ ) {
		if ( j->txn[i].blk == blk ) {
			knl_memcpy(j->txn[i].buf, buf, TSFSBLK_BLOCK_SIZE);
			goto exit;
		}
	}
	if ( j->ntxn >= (INT)TSFSJ_MAX_BLOCKS ) {
		er = E_LIMIT;			/* more than one descriptor can name */
		goto exit;
	}
	copy = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( copy == NULL ) {
		er = E_NOMEM;
		goto exit;
	}
	knl_memcpy(copy, buf, TSFSBLK_BLOCK_SIZE);
	j->txn[j->ntxn].blk = blk;
	j->txn[j->ntxn].buf = copy;
	j->ntxn++;

    exit:
	tk_unl_mtx(j->mtx);
	return er;
}

/*
 * The running transaction, with no handle left, made into a queued one:
 * its copies, its revokes and the ring blocks it will take. The caller
 * holds mtx; on success the running transaction is empty.
 */
LOCAL PTXN *txn_queue( TSFSJRNL *j, UD foot )
{
	PTXN	*t = (PTXN *)Kcalloc(1, sizeof(PTXN));
	INT	i;

	if ( t == NULL ) {
		return NULL;
	}
	if ( j->ntxn > 0 ) {
		t->ent = (JENT *)Kmalloc(sizeof(JENT) * (SZ)j->ntxn);
	}
	if ( j->nrevoke > 0 ) {
		t->revoke = (UD *)Kmalloc(sizeof(UD) * (SZ)j->nrevoke);
	}
	if ( ( j->ntxn > 0 && t->ent == NULL ) || ( j->nrevoke > 0 && t->revoke == NULL ) ) {
		ptxn_free(t);
		return NULL;
	}
	for ( i = 0; i < j->ntxn; i++ ) {
		t->ent[i] = j->txn[i];
	}
	t->n = j->ntxn;
	for ( i = 0; i < j->nrevoke; i++ ) {
		t->revoke[i] = j->revoke[i];
	}
	t->nrevoke = j->nrevoke;
	t->seq = j->seq;
	t->foot = foot;
	j->ntxn = 0;
	j->nrevoke = 0;
	return t;
}

/*
 * The copies of a queued transaction become what every read sees, and
 * revoked blocks are forgotten. The cache keeps copies of its own: the
 * transaction's go into the ring as they are. With no memory for a copy
 * a read would find an older block, and a checkpoint would leave the
 * place old: E_NOMEM, and the caller stops the volume.
 */
LOCAL ER txn_show( TSFSJRNL *j, CONST PTXN *t )
{
	UB	*copy;
	INT	i;
	ER	er = E_OK;

	for ( i = 0; i < t->nrevoke; i++ ) {
		cache_del(j, t->revoke[i]);
	}
	for ( i = 0; i < t->n; i++ ) {
		copy = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( copy != NULL ) {
			knl_memcpy(copy, t->ent[i].buf, TSFSBLK_BLOCK_SIZE);
			if ( cache_put(j, t->ent[i].blk, copy) >= E_OK ) {
				continue;
			}
			Kfree(copy);
		}
		er = E_NOMEM;
	}
	return er;
}

/* Whether a checkpoint is writing a copy of `blk` at the moment; the caller holds mtx */
LOCAL BOOL in_snap( CONST TSFSJRNL *j, UD blk )
{
	UD	i;

	if ( !j->ckpt_on ) {
		return FALSE;
	}
	for ( i = 0; i < j->nsnap; i++ ) {
		if ( j->snap[i].blk == blk ) return TRUE;
	}
	return FALSE;
}

/*
 * The pretended cuts in the power (ts_jrnl_fault), done as the closing
 * caller waits: what is queued goes into the ring first, as it would
 * have before the power went. Before the commit the transaction's
 * blocks go into the ring but its commit block does not, and it is
 * thrown away; after the commit it is in the ring with its commit
 * block, and the journal then stops as memory would.
 */
LOCAL ER close_cut( TSFSJRNL *j, PTXN *t, BOOL after )
{
	ER	er;

	tk_loc_mtx(j->iomtx, TMO_FEVR);
	er = pend_write(j);
	if ( er >= E_OK ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( after && t->foot + j->used > j->len ) {
			tk_unl_mtx(j->mtx);
			er = ckpt_io(j);
		} else {
			tk_unl_mtx(j->mtx);
		}
	}
	if ( er >= E_OK ) {
		er = ring_txns(j, t, j->tail, after, NULL);
		if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	}
	if ( after && er >= E_OK ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		j->tail = ( j->tail + t->foot ) % j->len;
		j->used += t->foot;
		j->seq = t->seq + 1;
		j->dseq = j->seq;
		j->dead = TRUE;			/* memory goes with the power */
		tk_unl_mtx(j->mtx);
	}
	tk_unl_mtx(j->iomtx);
	return er;
}

/*
 * Let a handle go. The last one closes the transaction: what the maps,
 * the group heads and the superblock owe goes in first, then the
 * transaction is queued for the ring and its copies are what reads see
 * from then on. Nothing is written to the device here, unless the ring
 * has no room left behind what is queued: then the queue goes in and
 * everything is put in place first. *p_seq is the number it took (0
 * when nothing changed); ts_jrnl_durable makes it real.
 */
EXPORT ER ts_jrnl_close( ID vol, UD *p_seq )
{
	TSFSJRNL	*j = jrnl_find(vol);
	PTXN	*t;
	UD	foot, seq;
	BOOL	wake;
	ER	er;

	if ( p_seq != NULL ) {
		*p_seq = 0;
	}
	if ( j == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( j->handles == 0 ) {
		tk_unl_mtx(j->mtx);
		return E_OBJ;
	}
	if ( --j->handles > 0 ) {
		tk_unl_mtx(j->mtx);
		return E_OK;			/* the last one closes it */
	}
	if ( j->doomed ) {
		txn_drop(j);
		tk_unl_mtx(j->mtx);
		ts_blk_jrnl_discard(vol);
		return E_IO;
	}
	j->committing = TRUE;
	tk_unl_mtx(j->mtx);

	er = ts_blk_jrnl_flush(vol);
	if ( er < E_OK ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		txn_drop(j);
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		ts_blk_jrnl_discard(vol);
		return er;
	}
	ts_blk_jrnl(vol, FALSE);

	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( j->ntxn == 0 && j->nrevoke == 0 ) {
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		return E_OK;			/* nothing changed */
	}

	/* revokes that fit in one block, and room in the ring behind the queue */
	foot = 1 + (UD)j->ntxn + ( j->nrevoke > 0 ? 1 : 0 ) + 1;
	while ( er >= E_OK && ( j->revoke_over || j->used + j->pend_foot + foot > j->len ) ) {
		tk_unl_mtx(j->mtx);
		er = ckpt_all(j);
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( er >= E_OK && j->revoke_over ) {
			j->nrevoke = 0;		/* nothing older is left to come back */
			j->revoke_over = FALSE;
			foot = 1 + (UD)j->ntxn + 1;
		}
		if ( er >= E_OK && j->used + j->pend_foot + foot > j->len ) {
			er = E_LIMIT;		/* the ring cannot hold it even empty */
		}
	}
	t = ( er >= E_OK ) ? txn_queue(j, foot) : NULL;
	if ( t == NULL ) {
		txn_drop(j);
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		ts_blk_jrnl_discard(vol);
		ts_blk_error(vol, j->start, TSFSBLK_ERR_IO);
		return ( er < E_OK ) ? er : E_NOMEM;
	}

	if ( j->fault == TSFSJ_FAULT_BEFORE_COMMIT || j->fault == TSFSJ_FAULT_AFTER_COMMIT ) {
		BOOL	after = (BOOL)( j->fault == TSFSJ_FAULT_AFTER_COMMIT );

		tk_unl_mtx(j->mtx);
		er = close_cut(j, t, after);
		ptxn_free(t);
		tk_loc_mtx(j->mtx, TMO_FEVR);
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		if ( !after ) {
			ts_blk_jrnl_discard(vol);
		}
		return er;
	}

	er = txn_show(j, t);
	*j->pend_end = t;
	j->pend_end = &t->next;
	j->pend_foot += foot;
	j->seq++;
	j->closed++;
	seq = t->seq;
	wake = (BOOL)( j->ncached > CACHE_MAX || j->used + j->pend_foot > j->len / 2 );
	if ( er < E_OK ) {
		ring_failed(j);			/* what reads see is not what the ring holds */
	}
	j->committing = FALSE;
	tk_unl_mtx(j->mtx);

	if ( er < E_OK ) {
		return er;
	}
	if ( wake ) {
		wake_checkpointer();
	}
	ts_set_jrnl_blk(vol, j->start, j->blocks, seq + 1);
	if ( p_seq != NULL ) {
		*p_seq = seq;
	}
	return E_OK;
}

/*
 * Wait until every transaction numbered below `upto` is in the ring.
 * Whoever comes first writes the whole queue, its own and everyone
 * else's closed so far; one who finds the ring being written waits for
 * that, and is often done by it.
 */
EXPORT ER ts_jrnl_durable( ID vol, UD upto )
{
	TSFSJRNL	*j = jrnl_find(vol);
	ER	er;

	if ( j == NULL ) {
		return E_OK;
	}
	for ( ;; ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( upto > j->seq ) {
			upto = j->seq;		/* nothing past what has been closed */
		}
		if ( j->dseq >= upto || j->dead ) {
			tk_unl_mtx(j->mtx);
			return E_OK;
		}
		if ( j->failed ) {
			tk_unl_mtx(j->mtx);
			return E_IO;
		}
		/* queued, or taken from the queue by a writer at it now: the
		   lock of the ring waits for that one */
		tk_unl_mtx(j->mtx);

		tk_loc_mtx(j->iomtx, TMO_FEVR);
		er = pend_write(j);
		tk_unl_mtx(j->iomtx);
		if ( er < E_OK ) {
			return er;
		}
	}
}

/* The number the next transaction closed takes: what has been closed is below it */
EXPORT UD ts_jrnl_next( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);

	return ( j != NULL ) ? __atomic_load_n(&j->seq, __ATOMIC_ACQUIRE) : 0;
}

/* Let a handle go; the last one makes the transaction real before it returns */
EXPORT ER ts_jrnl_commit( ID vol )
{
	UD	seq = 0;
	ER	er;

	er = ts_jrnl_close(vol, &seq);
	if ( er < E_OK || seq == 0 ) {
		return er;
	}
	return ts_jrnl_durable(vol, seq + 1);
}

/*
 * Give the transaction up. With other handles still on it, the whole of
 * it goes when the last one lets go.
 */
EXPORT ER ts_jrnl_abort( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);

	if ( j == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( j->handles == 0 ) {
		tk_unl_mtx(j->mtx);
		return E_OBJ;
	}
	if ( --j->handles > 0 ) {
		j->doomed = TRUE;
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	txn_drop(j);
	tk_unl_mtx(j->mtx);
	ts_blk_jrnl_discard(vol);

	return E_OK;
}

EXPORT ER ts_jrnl_read( ID vol, UD blk, void *buf )
{
	TSFSJRNL	*j = jrnl_find(vol);
	INT	i;

	if ( buf == NULL ) {
		return E_PAR;
	}
	if ( j != NULL ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( j->handles > 0 || j->committing ) {
			for ( i = 0; i < j->ntxn; i++ ) {
				if ( j->txn[i].blk == blk ) {
					knl_memcpy(buf, j->txn[i].buf, TSFSBLK_BLOCK_SIZE);
					tk_unl_mtx(j->mtx);
					return E_OK;
				}
			}
		}
		tk_unl_mtx(j->mtx);
	}
	return ts_read_blk(vol, blk, buf);	/* which asks here for a committed copy */
}

EXPORT ER ts_jrnl_checkpoint( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);

	if ( j == NULL ) {
		return E_OK;
	}
	return ckpt_all(j);
}

/*
 * No transaction begins while the volume is frozen (on): the ones
 * running are waited for, up to five seconds, and everything committed
 * is put in place, so that the ring holds nothing a replay would bring
 * back. A volume being grown needs this: a group head or a superblock
 * copy from before, put back after, would undo the grow. E_BUSY when a
 * transaction does not end in time; nothing is frozen then.
 */
EXPORT ER knl_jrnl_freeze( ID vol, BOOL on )
{
	TSFSJRNL	*j = jrnl_find(vol);
	INT		tries;
	ER		er = E_OK;

	if ( j == NULL ) {
		return E_OK;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( !on ) {
		j->frozen = FALSE;
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	j->frozen = TRUE;
	for ( tries = 0; ( j->handles > 0 || j->committing ) && tries < 500; tries++ ) {
		tk_unl_mtx(j->mtx);
		tk_dly_tsk(10);
		tk_loc_mtx(j->mtx, TMO_FEVR);
	}
	if ( j->handles > 0 || j->committing ) {
		j->frozen = FALSE;
		tk_unl_mtx(j->mtx);
		return E_BUSY;
	}
	tk_unl_mtx(j->mtx);

	er = ckpt_all(j);			/* nothing begins meanwhile: frozen */
	if ( er < E_OK ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		j->frozen = FALSE;
		tk_unl_mtx(j->mtx);
	}
	return er;
}

EXPORT ER ts_jrnl_wipe( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	UB		*zero;
	UD		pos;
	INT		tries;
	ER		er;

	if ( j == NULL ) {
		return E_OK;
	}
	zero = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( zero == NULL ) {
		return E_NOMEM;
	}
	knl_memset(zero, 0, TSFSBLK_BLOCK_SIZE);
	tk_loc_mtx(j->mtx, TMO_FEVR);
	for ( tries = 0; ( j->handles > 0 || j->committing ) && tries < 500; tries++ ) {
		tk_unl_mtx(j->mtx);
		tk_dly_tsk(10);
		tk_loc_mtx(j->mtx, TMO_FEVR);
	}
	if ( j->handles > 0 || j->committing ) {
		tk_unl_mtx(j->mtx);
		Kfree(zero);
		return E_BUSY;
	}
	tk_unl_mtx(j->mtx);

	tk_loc_mtx(j->iomtx, TMO_FEVR);
	er = ckpt_io(j);
	/* the ring now holds no transaction to replay: none of it is needed.
	   One closed meanwhile waits in the queue and goes in after this. */
	for ( pos = 0; er >= E_OK && !j->dead && pos < j->len; pos++ ) {
		er = knl_tsfsblk_raw_write(j->vol, ring_blk(j, pos), zero);
	}
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	tk_unl_mtx(j->iomtx);
	Kfree(zero);

	return er;
}

EXPORT ER ts_jrnl_ref( ID vol, T_TSFSJRNL *p_jrnl )
{
	TSFSJRNL	*j;
	UD	start = 0, blocks = 0, seq = 0;

	if ( p_jrnl == NULL ) {
		return E_PAR;
	}
	knl_memset(p_jrnl, 0, sizeof(*p_jrnl));
	j = jrnl_find(vol);
	if ( j == NULL ) {
		if ( ts_get_jrnl_blk(vol, &start, &blocks, &seq) < E_OK ) {
			return E_ID;
		}
		p_jrnl->start = start;
		p_jrnl->blocks = blocks;
		p_jrnl->seq = seq;
		p_jrnl->head_seq = seq;
		p_jrnl->durable = seq;
		return E_OK;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	p_jrnl->start = j->start;
	p_jrnl->blocks = j->blocks;
	p_jrnl->seq = j->seq;
	p_jrnl->in_txn = (UD)j->ntxn;
	p_jrnl->open = (UINT)j->handles;
	p_jrnl->head_seq = j->head_seq;
	p_jrnl->used = j->used;
	p_jrnl->cached = j->ncached;
	p_jrnl->dead = j->dead;
	p_jrnl->durable = j->dseq;
	p_jrnl->queued = j->pend_foot;
	p_jrnl->closed = j->closed;
	p_jrnl->batches = j->batches;
	tk_unl_mtx(j->mtx);

	return E_OK;
}

EXPORT ER ts_jrnl_fault( ID vol, UINT where )
{
	TSFSJRNL	*j = jrnl_make(vol);

	if ( j == NULL ) {
		return E_LIMIT;
	}
	if ( where != TSFSJ_FAULT_NONE ) {
		(void)ts_jrnl_durable(vol, ~(UD)0);	/* what was closed before is not part of it */
	}
	j->fault = where;
	return E_OK;
}

/* ---------------------------------------------------------------- for the block layer */

EXPORT BOOL knl_jrnl_peek( ID vol, UD blk, void *buf )
{
	TSFSJRNL	*j = jrnl_find(vol);
	CNODE	*n;
	BOOL	got = FALSE;

	if ( j == NULL || j->ncached == 0 ) {
		return FALSE;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	n = *cache_slot(j, blk);
	if ( n != NULL ) {
		knl_memcpy(buf, n->buf, TSFSBLK_BLOCK_SIZE);
		got = TRUE;
	}
	tk_unl_mtx(j->mtx);
	return got;
}

/*
 * A block about to be written in place. A committed copy of it that is
 * still in the ring would be put back over the write after a cut in the
 * power, and the checkpoint would put it over the write in any case; so
 * everything waiting is put in place first, and the head moves past it.
 * A checkpoint writing at the moment is waited for, since it may be
 * writing this block.
 */
EXPORT void knl_jrnl_forget_blk( ID vol, UD blk )
{
	TSFSJRNL	*j = jrnl_find(vol);
	BOOL	need;

	if ( j == NULL || ( j->ncached == 0 && !j->ckpt_on ) ) {
		return;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	need = (BOOL)( *cache_slot(j, blk) != NULL || in_snap(j, blk) );
	tk_unl_mtx(j->mtx);
	if ( need ) {
		(void)ckpt_all(j);
	}
}

/*
 * Blocks freed. Within a transaction, the ones the ring holds copies of
 * are revoked, so that coming back after a cut in the power does not put
 * an old copy over what the block holds next; one the running
 * transaction itself changed is dropped from it. Outside a transaction
 * there is nothing to write a revoke into, so what waits is put in place
 * and the head moves past it.
 */
EXPORT void knl_jrnl_revoke( ID vol, UD start, UD count )
{
	TSFSJRNL	*j = jrnl_find(vol);
	UD	b;
	INT	i;
	BOOL	in_ring = FALSE;

	if ( j == NULL ) {
		return;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	for ( b = start; b < start + count; b++ ) {
		if ( j->handles > 0 || j->committing ) {
			for ( i = 0; i < j->ntxn; i++ ) {
				if ( j->txn[i].blk == b ) {
					Kfree(j->txn[i].buf);
					j->txn[i] = j->txn[--j->ntxn];
					break;
				}
			}
		}
		if ( !( j->handles > 0 || j->committing ) && in_snap(j, b) ) {
			in_ring = TRUE;		/* the checkpoint writing it is waited for */
		}
		if ( j->ncached == 0 || *cache_slot(j, b) == NULL ) {
			continue;
		}
		if ( j->handles > 0 || j->committing ) {
			if ( j->nrevoke < (INT)TSFSJ_MAX_REVOKE ) {
				j->revoke[j->nrevoke++] = b;
			} else {
				j->revoke_over = TRUE;
			}
		} else {
			in_ring = TRUE;
		}
	}
	tk_unl_mtx(j->mtx);
	if ( in_ring ) {
		(void)ckpt_all(j);
	}
}

/* The number the running transaction will take, for the blocks it frees */
EXPORT UD knl_jrnl_running( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	UD	seq;

	if ( j == NULL ) {
		return 0;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	seq = j->seq;
	tk_unl_mtx(j->mtx);
	return seq;
}

/*
 * Whether the volume's log has been read at open, so that a transaction
 * knows where in the ring it goes. A volume opened with an error
 * recorded is not replayed and has none until it is.
 */
EXPORT BOOL knl_jrnl_ready( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);

	return (BOOL)( j != NULL && j->blocks > 0 );
}

EXPORT ER knl_jrnl_close( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	PTXN	*t;
	ER	er;

	if ( j == NULL ) {
		return E_OK;
	}
	tk_loc_mtx(j->iomtx, TMO_FEVR);
	er = ckpt_io(j);			/* does nothing after a pretended power cut */
	tk_loc_mtx(j->mtx, TMO_FEVR);
	txn_drop(j);
	while ( j->pend != NULL ) {		/* only after a failure or a pretended cut */
		t = j->pend;
		j->pend = t->next;
		ptxn_free(t);
	}
	j->pend_end = &j->pend;
	j->pend_foot = 0;
	cache_clear(j);
	j->vol = 0;
	tk_unl_mtx(j->mtx);
	tk_unl_mtx(j->iomtx);
	tk_del_mtx(j->mtx);
	tk_del_mtx(j->iomtx);

	return er;
}

/* ---------------------------------------------------------------- replay */

typedef struct {
	UD	pos;			/* its descriptor */
	UD	seq;
} TXREC;

/*
 * Put back whatever a cut in the power left behind: every transaction
 * from the head on whose commit block is good, in order, leaving out a
 * copy of a block that the same or a later transaction revoked. Then the
 * head moves past them, so they are not put back twice.
 */
EXPORT ER ts_jrnl_replay( ID vol )
{
	TSFSJRNL	*j;
	UB	*buf, *blk;
	TXREC	*tx = NULL;
	UD	*rvk_blk = NULL, *rvk_seq = NULL;
	UD	start = 0, blocks = 0, seq = 1, pos, cur, scanned, txpos = 0, good_end;
	INT	ntx = 0, maxtx, nrvk = 0, nrvk_good = 0, i, k, n;
	UW	chain = 0, c;
	BOOL	in_tx = FALSE;
	ER	er = E_OK;

	j = jrnl_find(vol);
	if ( j != NULL ) {
		(void)knl_jrnl_close(vol);	/* left from before: gone with the power */
	}
	j = jrnl_make(vol);
	if ( j == NULL ) {
		return E_LIMIT;
	}
	er = ts_get_jrnl_blk(vol, &start, &blocks, &seq);
	if ( er < E_OK || start == 0 || blocks < TSFSJ_AREA_BLOCKS ) {
		return er;			/* no journal area */
	}
	j->start = start;
	j->blocks = blocks;
	j->len = blocks - 1;
	{
		T_TSFSBLK_SB	sb;

		if ( ts_ref_vol_blk(vol, &sb) >= E_OK ) {
			j->salt = ts_crc32c(sb.vol_uuid.b, 16) ^ (UW)sb.mkfs_time;
		}
	}

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	maxtx = (INT)( j->len / 2 + 1 );
	tx = (TXREC *)Kmalloc(sizeof(TXREC) * (SZ)maxtx);
	rvk_blk = (UD *)Kmalloc(sizeof(UD) * REPLAY_MAX_REVOKE);
	rvk_seq = (UD *)Kmalloc(sizeof(UD) * REPLAY_MAX_REVOKE);
	if ( buf == NULL || blk == NULL || tx == NULL || rvk_blk == NULL || rvk_seq == NULL ) {
		er = E_NOMEM;
		goto exit;
	}

	/* the head block; a volume just made has none yet */
	if ( knl_tsfsblk_raw_read(vol, start, buf) < E_OK ) {
		er = E_IO;
		goto exit;
	}
	if ( buf[0] == (UB)JH_MAGIC[0] && buf[1] == (UB)JH_MAGIC[1] && buf[2] == (UB)JH_MAGIC[2]
	  && buf[3] == (UB)JH_MAGIC[3] && rd32(buf + JH_VERSION) == TSFSJ_VERSION
	  && rd32(buf + JH_CRC) == crc_at(j, buf, JH_CRC) && rd64(buf + JH_POS) < j->len ) {
		j->head = rd64(buf + JH_POS);
		j->head_seq = rd64(buf + JH_SEQ);
	} else {
		j->head = 0;
		j->head_seq = seq;
		j->tail = 0;
		j->seq = seq;
		j->dseq = seq;
		er = head_write(j, j->head, j->head_seq);
		goto exit;
	}

	/* find the transactions that became real */
	pos = j->head;
	cur = j->head_seq;
	good_end = pos;
	for ( scanned = 0; scanned < j->len && ntx < maxtx; ) {
		if ( knl_tsfsblk_raw_read(vol, ring_blk(j, pos), buf) < E_OK ) {
			break;
		}
		if ( rd64(buf + JB_SEQ) != cur || rd32(buf + JB_CRC) != crc_at(j, buf, JB_CRC) ) {
			break;
		}
		if ( !in_tx && is_kind(buf, JB_DESC) ) {
			n = (INT)rd32(buf + JB_COUNT);
			if ( n > (INT)TSFSJ_MAX_BLOCKS ) break;
			c = rd32(buf + JB_CRC);
			chain = ts_crc32c((CONST UB *)&c, 4);
			txpos = pos;
			for ( i = 0; i < n; i++ ) {
				if ( knl_tsfsblk_raw_read(vol, ring_blk(j, pos + 1 + (UD)i), blk) < E_OK ) break;
				c = ts_crc32c(blk, TSFSBLK_BLOCK_SIZE);
				if ( c != rd32(buf + JB_ENT + i * TSFSJ_DESC_ENT + 8) ) break;
				chain ^= ts_crc32c((CONST UB *)&c, 4) + (UW)i;
			}
			if ( i < n ) break;		/* a copy did not reach the medium */
			pos += 1 + (UD)n;
			scanned += 1 + (UD)n;
			in_tx = TRUE;
			continue;
		}
		if ( in_tx && is_kind(buf, JB_REVOKE) ) {
			n = (INT)rd32(buf + JB_COUNT);
			if ( n > (INT)TSFSJ_MAX_REVOKE || nrvk + n > REPLAY_MAX_REVOKE ) break;
			for ( i = 0; i < n; i++ ) {
				rvk_blk[nrvk] = rd64(buf + JB_ENT + i * 8);
				rvk_seq[nrvk] = cur;
				nrvk++;
			}
			c = rd32(buf + JB_CRC);
			chain ^= ts_crc32c((CONST UB *)&c, 4) + 0x5A5A5A5AU;
			pos++;
			scanned++;
			continue;
		}
		if ( in_tx && is_kind(buf, JB_COMMIT) ) {
			if ( rd32(buf + JC_CHAIN) != chain ) break;
			tx[ntx].pos = txpos;
			tx[ntx].seq = cur;
			ntx++;
			cur++;
			pos++;
			scanned++;
			good_end = pos;
			nrvk_good = nrvk;	/* its revokes count from now */
			in_tx = FALSE;
			continue;
		}
		break;
	}
	/* one whose commit is not there never happened, nor did its revokes */
	nrvk = nrvk_good;

	/* put them back in order, leaving out what was revoked later */
	for ( k = 0; k < ntx && er >= E_OK; k++ ) {
		er = knl_tsfsblk_raw_read(vol, ring_blk(j, tx[k].pos), buf);
		n = ( er >= E_OK ) ? (INT)rd32(buf + JB_COUNT) : 0;
		for ( i = 0; i < n && er >= E_OK; i++ ) {
			UD	home = rd64(buf + JB_ENT + i * TSFSJ_DESC_ENT);
			INT	r;
			BOOL	skip = FALSE;

			for ( r = 0; r < nrvk; r++ ) {
				if ( rvk_blk[r] == home && rvk_seq[r] >= tx[k].seq ) {
					skip = TRUE;
					break;
				}
			}
			if ( skip ) continue;
			er = knl_tsfsblk_raw_read(vol, ring_blk(j, tx[k].pos + 1 + (UD)i), blk);
			if ( er >= E_OK ) er = knl_tsfsblk_raw_write(vol, home, blk);
		}
	}
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);

	/* the head past what was put back; the ring is empty */
	if ( er >= E_OK ) {
		j->head = good_end % j->len;		/* past the commit of the last good one */
		j->tail = j->head;
		j->used = 0;
		j->head_seq = cur;
		j->seq = cur;
		j->dseq = cur;
		er = head_write(j, j->head, j->head_seq);
		if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);
	}

    exit:
	if ( buf != NULL ) Kfree(buf);
	if ( blk != NULL ) Kfree(blk);
	if ( tx != NULL ) Kfree(tx);
	if ( rvk_blk != NULL ) Kfree(rvk_blk);
	if ( rvk_seq != NULL ) Kfree(rvk_seq);
	if ( er >= E_OK && j->seq != 0 ) {
		ts_set_jrnl_blk(vol, start, blocks, j->seq);
	}
	return er;
}
