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
 *	On a volume of version 5 (TSFSBLK_INCOMPAT_FMT5) a transaction has
 *	as many descriptors as its copies need, each followed by the copies
 *	it names, and as many revoke blocks, each numbered in the
 *	transaction from 0; the commit block says how many of each there
 *	were. One transaction may hold a quarter of the ring (no fewer than
 *	one descriptor names), and the head and every block of the ring say
 *	version 5. A volume of version 4 keeps one descriptor and one
 *	revoke block, written as before.
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
#define REPLAY_RVK_FIRST	512	/* revokes the replay makes room for first; the table grows */
#define LAY_PIECE		64	/* blocks laid out at once when a transaction is too large for memory */

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
#define JB_INDEX	28		/* version 5: the descriptor's or revoke block's number in its transaction */
#define JB_ENT		TSFSJ_HDR_SIZE
#define JC_TIME		32
#define JC_CHAIN	40
#define JC_BLOCKS	44		/* ring blocks of the transaction before the commit block */
#define JC_NDESC	48		/* version 5: its descriptor blocks */
#define JC_NRVK		52		/* version 5: its revoke blocks */

/*
 * The chain of a commit block: the CRC of the first descriptor's CRC,
 * then for each block after it, in the order of the ring, the CRC of
 * its CRC plus a term that says what it is: the copy's number in the
 * transaction (counted over every descriptor), DESC_SALT plus the
 * number of a descriptor after the first, REVOKE_SALT plus the number
 * of a revoke block, every term put in by exclusive or. A version 4
 * transaction has one descriptor and one revoke block at most, so it is
 * chained as before.
 */
#define DESC_SALT	0xA5A5A5A5U
#define REVOKE_SALT	0x5A5A5A5AU

/* The ring blocks a transaction of n copies and r revokes takes, and its descriptor blocks */
#define NDESC(n)	( ( (n) == 0 ) ? 1 : ( (n) + TSFSJ_MAX_BLOCKS - 1 ) / TSFSJ_MAX_BLOCKS )
#define NRVKB(r)	( ( (r) + TSFSJ_MAX_REVOKE - 1 ) / TSFSJ_MAX_REVOKE )

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
	BOOL		skip;		/* TSFSJ_FAULT_SKIP: its second descriptor is numbered 2 */
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
	BOOL	v5;			/* the ring and the head are of version 5 */
	UD	vbudget;		/* copies a transaction of this volume may hold */
	JENT	*txn;			/* the running transaction's copies, room for txn_max */
	INT	ntxn, txn_max;
	UD	*revoke;		/* its revokes, room for rvk_max */
	INT	nrevoke, rvk_max;
	BOOL	revoke_over;		/* more revokes than the ring has room for */
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
	UD	budget;			/* TSFSJ_FAULT_BUDGET: copies of a transaction, 0 the volume's own */
	BOOL	nockpt;			/* TSFSJ_FAULT_NOCKPT: the checkpointer leaves the volume alone */
	BOOL	conv_left;		/* opening wrote the head of a conversion cut short */
	UINT	last_desc, last_rvk;	/* blocks of the last transaction closed, for ts_jrnl_ref */
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

/* The version the head and the ring blocks carry: that of the ring (v5) */
LOCAL UW ring_ver( BOOL v5 )
{
	return v5 ? TSFSJ_VERSION5 : TSFSJ_VERSION;
}

LOCAL BOOL is_kind( CONST UB *buf, CONST char *magic, BOOL v5 )
{
	return (BOOL)( buf[0] == (UB)magic[0] && buf[1] == (UB)magic[1]
		    && buf[2] == (UB)magic[2] && buf[3] == (UB)magic[3]
		    && rd32(buf + JB_VERSION) == ring_ver(v5) );
}

LOCAL void make_kind( UB *buf, CONST char *magic, BOOL v5, UD seq, UW count )
{
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	buf[0] = (UB)magic[0]; buf[1] = (UB)magic[1];
	buf[2] = (UB)magic[2]; buf[3] = (UB)magic[3];
	wr32(buf + JB_VERSION, ring_ver(v5));
	wr64(buf + JB_SEQ, seq);
	wr32(buf + JB_COUNT, count);
}

/* One term of a commit block's chain: the CRC of a block's CRC, plus what tells it apart */
LOCAL UW chain_term( UW c, UW plus )
{
	return ts_crc32c((CONST UB *)&c, 4) + plus;
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

/* The tables of the running transaction given back, at closing */
LOCAL void txn_tables_free( TSFSJRNL *j )
{
	if ( j->txn != NULL ) Kfree(j->txn);
	if ( j->revoke != NULL ) Kfree(j->revoke);
	j->txn = NULL;
	j->revoke = NULL;
	j->txn_max = j->rvk_max = 0;
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
	wr32(buf + JH_VERSION, ring_ver(j->v5));
	wr64(buf + JH_SEQ, head_seq);
	wr64(buf + JH_POS, head);
	wr32(buf + JH_CRC, crc_at(j, buf, JH_CRC));
	er = knl_tsfsblk_raw_write(j->vol, j->start, buf);
	Kfree(buf);
	return er;
}

/*
 * Laying a closed transaction out as the ring holds it, one block after
 * another: a descriptor, the copies it names, the next descriptor and
 * its copies (version 5), the revoke blocks, and the commit block. The
 * layout is made as it goes, so that a transaction too large to lay out
 * whole in memory goes out in pieces.
 */
typedef struct {
	CONST PTXN	*t;
	BOOL	v5;
	INT	ci;			/* copies laid out */
	INT	dleft;			/* copies the last descriptor names still to come */
	INT	nd;			/* descriptors laid out */
	INT	ri;			/* revokes laid out */
	INT	nr;			/* revoke blocks laid out */
	UW	chain;
} LAY;

LOCAL void lay_start( LAY *L, CONST TSFSJRNL *j, CONST PTXN *t )
{
	knl_memset(L, 0, sizeof(*L));
	L->t = t;
	L->v5 = j->v5;
}

/* The next block of the transaction into `b`; FALSE when only the commit block is left */
LOCAL BOOL lay_next( CONST TSFSJRNL *j, LAY *L, UB *b )
{
	CONST PTXN	*t = L->t;
	UW		c;
	INT		i, k;

	if ( L->dleft > 0 ) {
		/* a copy */
		knl_memcpy(b, t->ent[L->ci].buf, TSFSBLK_BLOCK_SIZE);
		c = ts_crc32c(b, TSFSBLK_BLOCK_SIZE);
		L->chain ^= chain_term(c, (UW)L->ci);
		L->ci++;
		L->dleft--;
		return TRUE;
	}
	if ( L->nd == 0 || L->ci < t->n ) {
		/* a descriptor, naming as many of the copies as it holds */
		k = t->n - L->ci;
		if ( k > (INT)TSFSJ_MAX_BLOCKS ) k = (INT)TSFSJ_MAX_BLOCKS;
		make_kind(b, JB_DESC, L->v5, t->seq, (UW)k);
		if ( L->v5 ) {
			wr32(b + JB_INDEX, (UW)( ( t->skip && L->nd == 1 ) ? 2 : L->nd ));
		}
		for ( i = 0; i < k; i++ ) {
			wr64(b + JB_ENT + i * TSFSJ_DESC_ENT, t->ent[L->ci + i].blk);
			wr32(b + JB_ENT + i * TSFSJ_DESC_ENT + 8,
			     ts_crc32c(t->ent[L->ci + i].buf, TSFSBLK_BLOCK_SIZE));
		}
		c = crc_at(j, b, JB_CRC);
		wr32(b + JB_CRC, c);
		if ( L->nd == 0 ) {
			L->chain = ts_crc32c((CONST UB *)&c, 4);
		} else {
			L->chain ^= chain_term(c, DESC_SALT + (UW)L->nd);
		}
		L->nd++;
		L->dleft = k;
		return TRUE;
	}
	if ( L->ri < t->nrevoke ) {
		/* a revoke block */
		k = t->nrevoke - L->ri;
		if ( k > (INT)TSFSJ_MAX_REVOKE ) k = (INT)TSFSJ_MAX_REVOKE;
		make_kind(b, JB_REVOKE, L->v5, t->seq, (UW)k);
		if ( L->v5 ) {
			wr32(b + JB_INDEX, (UW)L->nr);
		}
		for ( i = 0; i < k; i++ ) {
			wr64(b + JB_ENT + i * 8, t->revoke[L->ri + i]);
		}
		c = crc_at(j, b, JB_CRC);
		wr32(b + JB_CRC, c);
		L->chain ^= chain_term(c, REVOKE_SALT + (UW)L->nr);
		L->nr++;
		L->ri += k;
		return TRUE;
	}
	return FALSE;
}

/* The commit block, after every other block of the transaction */
LOCAL void lay_commit( CONST TSFSJRNL *j, CONST LAY *L, UB *b )
{
	TS_TIME	tm = 0;

	make_kind(b, JB_COMMIT, L->v5, L->t->seq, 0);
	(void)dt_gettime(&tm);
	wr64(b + JC_TIME, (UD)tm);
	wr32(b + JC_CHAIN, L->chain);
	wr32(b + JC_BLOCKS, (UW)( L->t->foot - 1 ));
	if ( L->v5 ) {
		wr32(b + JC_NDESC, (UW)L->nd);
		wr32(b + JC_NRVK, (UW)L->nr);
	}
	wr32(b + JB_CRC, crc_at(j, b, JB_CRC));
}

/*
 * One closed transaction laid out whole in `out` (foot blocks), the
 * commit block last unless `commit` is FALSE (a pretended cut before it).
 */
LOCAL void txn_lay( CONST TSFSJRNL *j, CONST PTXN *t, UB *out, BOOL commit )
{
	LAY	L;
	UB	*b = out;

	lay_start(&L, j, t);
	while ( lay_next(j, &L, b) ) {
		b += TSFSBLK_BLOCK_SIZE;
	}
	if ( commit ) {
		lay_commit(j, &L, b);
	}
}

/*
 * One transaction written from `pos` of the ring on through a buffer of
 * `room` blocks, a piece at a time: for one too large to lay out whole.
 */
LOCAL ER txn_pieces( TSFSJRNL *j, CONST PTXN *t, UD pos, BOOL commit, UB *buf, UD room )
{
	LAY	L;
	UD	k = 0;
	BOOL	more = TRUE;
	ER	er = E_OK;

	lay_start(&L, j, t);
	while ( er >= E_OK && ( more || commit ) ) {
		if ( more ) {
			more = lay_next(j, &L, buf + k * TSFSBLK_BLOCK_SIZE);
			if ( more ) {
				k++;
			}
		}
		if ( !more && commit ) {
			lay_commit(j, &L, buf + k * TSFSBLK_BLOCK_SIZE);
			k++;
			commit = FALSE;
		}
		if ( k > 0 && ( k == room || ( !more && !commit ) ) ) {
			er = ring_put(j, pos, buf, k, FALSE, NULL);
			pos += k;
			k = 0;
		}
	}
	return er;
}

/*
 * The transactions of the queue, `t` on, laid out from `pos` of the
 * ring on. A batch of committed ones is laid out whole and written in
 * one request (two where the ring wraps), on the medium when it
 * completes when the device can: *p_fua says whether it was, and when
 * it was, the cache need not be emptied after (pend_write). One that
 * is too large for memory, and those of a pretended cut, go one by one,
 * in pieces when one is too large even alone.
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
		}
		if ( buf == NULL ) {
			buf = (UB *)Kmalloc((SZ)( LAY_PIECE * TSFSBLK_BLOCK_SIZE ));
			if ( buf == NULL ) {
				er = E_NOMEM;
				break;
			}
			room = 0;			/* the next one tries whole again */
			er = txn_pieces(j, t, pos, commit, buf, LAY_PIECE);
			Kfree(buf);
			buf = NULL;
		} else {
			txn_lay(j, t, buf, commit);
			er = ring_put(j, pos, buf, n, FALSE, NULL);
		}
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

			if ( j == NULL || j->vol <= 0 || j->nockpt || ( j->ncached == 0 && j->pend == NULL ) ) continue;
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

/* The ring blocks of a transaction of n copies and r revokes, laid out as the ring of j is */
LOCAL UD txn_foot( CONST TSFSJRNL *j, UD n, UD r )
{
	if ( !j->v5 ) {
		return 1 + n + ( r > 0 ? 1 : 0 ) + 1;
	}
	return NDESC(n) + n + NRVKB(r) + 1;
}

/*
 * The copies a transaction of the volume may hold. Version 4: what one
 * descriptor names. Version 5: a quarter of the ring and no fewer than
 * one descriptor names, so long as two of the largest transactions,
 * each with a revoke block, fit in the ring together; a ring smaller
 * than the journal area's least (TSFSJ_AREA_BLOCKS) is not opened.
 */
LOCAL UD vol_budget( CONST TSFSJRNL *j )
{
	UD	b;

	if ( !j->v5 ) {
		return (UD)TSFSJ_MAX_BLOCKS;
	}
	b = j->len / 4;
	if ( b < (UD)TSFSJ_MAX_BLOCKS ) b = (UD)TSFSJ_MAX_BLOCKS;
	while ( b > (UD)TSFSJ_MAX_BLOCKS && 2 * txn_foot(j, b, 1) > j->len ) {
		b--;
	}
	return b;
}

/*
 * The copies one transaction may hold: the volume's budget, or less
 * while TSFSJ_FAULT_BUDGET says so.
 */
LOCAL UD txn_budget( TSFSJRNL *j )
{
	UD	b = j->budget, v = ( j->vbudget > 0 ) ? j->vbudget : (UD)TSFSJ_MAX_BLOCKS;

	return ( b == 0 || b > v ) ? v : b;
}

/*
 * The revokes one transaction may hold. Version 4: one block of them.
 * Version 5: what is left of half the ring beside the largest
 * transaction's copies, so that two transactions still fit. More than
 * that and the running transaction does without them: everything before
 * it is put in place first (ts_jrnl_close), and nothing older is left
 * to come back.
 */
LOCAL UD rvk_cap( TSFSJRNL *j )
{
	UD	b, half, used;

	if ( !j->v5 ) {
		return (UD)TSFSJ_MAX_REVOKE;
	}
	b = txn_budget(j);
	half = j->len / 2;
	used = NDESC(b) + b + 1;
	return ( half > used + 1 ) ? ( half - used ) * (UD)TSFSJ_MAX_REVOKE : (UD)TSFSJ_MAX_REVOKE;
}

/* Room for one more entry of a table that grows: FALSE when there is no memory for it */
LOCAL BOOL grow_tbl( void **p_tbl, INT *p_max, INT need, SZ esize )
{
	void	*n;
	INT	m;

	if ( need <= *p_max ) {
		return TRUE;
	}
	m = ( *p_max < 32 ) ? 32 : *p_max * 2;
	while ( m < need ) m *= 2;
	n = ( *p_tbl == NULL ) ? Kmalloc((SZ)m * esize) : Krealloc(*p_tbl, (SZ)m * esize);
	if ( n == NULL ) {
		return FALSE;
	}
	*p_tbl = n;
	*p_max = m;
	return TRUE;
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
	if ( (UD)j->ntxn >= txn_budget(j) ) {
		er = E_LIMIT;			/* more than the budget */
		goto exit;
	}
	if ( !grow_tbl((void **)&j->txn, &j->txn_max, j->ntxn + 1, sizeof(JENT)) ) {
		er = E_NOMEM;
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
 * its copies, its revokes and the ring blocks it will take. The tables
 * go with it, and the next transaction makes new ones. The caller holds
 * mtx; on success the running transaction is empty.
 */
LOCAL PTXN *txn_queue( TSFSJRNL *j, UD foot )
{
	PTXN	*t = (PTXN *)Kcalloc(1, sizeof(PTXN));

	if ( t == NULL ) {
		return NULL;
	}
	if ( j->ntxn > 0 ) {
		t->ent = j->txn;
		j->txn = NULL;
		j->txn_max = 0;
	}
	t->n = j->ntxn;
	if ( j->nrevoke > 0 ) {
		t->revoke = j->revoke;
		j->revoke = NULL;
		j->rvk_max = 0;
	}
	t->nrevoke = j->nrevoke;
	t->seq = j->seq;
	t->foot = foot;
	t->skip = (BOOL)( j->fault == TSFSJ_FAULT_SKIP );
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

	/* revokes the ring has room for, and room in the ring behind the queue */
	foot = txn_foot(j, (UD)j->ntxn, (UD)j->nrevoke);
	while ( er >= E_OK && ( j->revoke_over || j->used + j->pend_foot + foot > j->len ) ) {
		tk_unl_mtx(j->mtx);
		er = ckpt_all(j);
		tk_loc_mtx(j->mtx, TMO_FEVR);
		if ( er >= E_OK && j->revoke_over ) {
			j->nrevoke = 0;		/* nothing older is left to come back */
			j->revoke_over = FALSE;
			foot = txn_foot(j, (UD)j->ntxn, 0);
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
	j->last_desc = j->v5 ? (UINT)NDESC((UD)t->n) : 1;
	j->last_rvk = j->v5 ? (UINT)NRVKB((UD)t->nrevoke) : ( t->nrevoke > 0 ? 1 : 0 );

	if ( j->fault == TSFSJ_FAULT_BEFORE_COMMIT || j->fault == TSFSJ_FAULT_AFTER_COMMIT
	  || j->fault == TSFSJ_FAULT_SKIP ) {
		BOOL	after = (BOOL)( j->fault != TSFSJ_FAULT_BEFORE_COMMIT );

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
	wake = (BOOL)( !j->nockpt && ( j->ncached > CACHE_MAX || j->used + j->pend_foot > j->len / 2 ) );
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
	p_jrnl->budget = (UINT)txn_budget(j);
	p_jrnl->last_desc = j->last_desc;
	p_jrnl->last_rvk = j->last_rvk;
	p_jrnl->v5 = j->v5;
	tk_unl_mtx(j->mtx);

	return E_OK;
}

EXPORT ER ts_jrnl_fault( ID vol, UINT where )
{
	TSFSJRNL	*j = jrnl_make(vol);

	if ( j == NULL ) {
		return E_LIMIT;
	}
	if ( ( where & TSFSJ_FAULT_BUDGET ) != 0 ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		j->budget = (UD)( where & TSFSJ_FAULT_ARG );
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	if ( ( where & TSFSJ_FAULT_NOCKPT ) != 0 ) {
		tk_loc_mtx(j->mtx, TMO_FEVR);
		j->nockpt = (BOOL)( ( where & TSFSJ_FAULT_ARG ) != 0 );
		tk_unl_mtx(j->mtx);
		return E_OK;
	}
	if ( where != TSFSJ_FAULT_NONE ) {
		(void)ts_jrnl_durable(vol, ~(UD)0);	/* what was closed before is not part of it */
	}
	j->fault = where;
	return E_OK;
}

EXPORT INT ts_jrnl_budget( ID vol )
{
	TSFSJRNL	*j;
	UD	b;

	if ( vol < 1 || vol > TSFSJ_MAX_VOL ) {
		return E_ID;
	}
	j = jrnl_find(vol);
	if ( j == NULL ) {
		return (INT)TSFSJ_MAX_BLOCKS;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	b = txn_budget(j);
	tk_unl_mtx(j->mtx);

	return (INT)b;
}

EXPORT INT ts_jrnl_room( ID vol )
{
	TSFSJRNL	*j;
	UD	b, n;

	if ( vol < 1 || vol > TSFSJ_MAX_VOL ) {
		return E_ID;
	}
	j = jrnl_find(vol);
	if ( j == NULL ) {
		return (INT)TSFSJ_MAX_BLOCKS;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	b = txn_budget(j);
	n = (UD)j->ntxn;
	tk_unl_mtx(j->mtx);

	return ( n >= b ) ? 0 : (INT)( b - n );
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
			if ( j->revoke_over ) {
				;			/* the transaction does without them */
			} else if ( (UD)j->nrevoke < rvk_cap(j)
				 && grow_tbl((void **)&j->revoke, &j->rvk_max, j->nrevoke + 1, sizeof(UD)) ) {
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
	txn_tables_free(j);
	j->vol = 0;
	tk_unl_mtx(j->mtx);
	tk_unl_mtx(j->iomtx);
	tk_del_mtx(j->mtx);
	tk_del_mtx(j->iomtx);

	return er;
}

/*
 * The head written as version 5 where the ring is now empty, the next
 * number where it was, and the device's cache emptied. From here on the
 * transactions are laid out as version 5 and have its budget. Anything
 * still waiting is put in place first, so that no transaction of
 * version 4 is left in the ring behind a head of version 5.
 */
EXPORT ER knl_jrnl_fmt5( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	ER		er;

	if ( j == NULL || j->len == 0 ) {
		return E_OBJ;			/* the log was not read at opening */
	}
	tk_loc_mtx(j->iomtx, TMO_FEVR);
	er = ckpt_io(j);
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( er >= E_OK && ( j->handles > 0 || j->committing || j->pend != NULL || j->used != 0 ) ) {
		er = E_BUSY;
	}
	if ( er >= E_OK && j->dead ) {
		er = E_IO;			/* a pretended cut in the power */
	}
	if ( er >= E_OK ) {
		j->v5 = TRUE;
		j->vbudget = vol_budget(j);
		j->head = j->tail;
		j->head_seq = j->dseq;
	}
	tk_unl_mtx(j->mtx);
	if ( er >= E_OK ) {
		er = head_write(j, j->head, j->head_seq);
		if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);
		if ( er < E_OK ) {
			/* a head of version 4 must not be left in front of a ring of version 5 */
			tk_loc_mtx(j->mtx, TMO_FEVR);
			ring_failed(j);
			tk_unl_mtx(j->mtx);
		}
	}
	tk_unl_mtx(j->iomtx);

	return er;
}

EXPORT BOOL knl_jrnl_conv_left( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);

	return (BOOL)( j != NULL && j->conv_left );
}

/* ---------------------------------------------------------------- replay */

typedef struct {
	UD	pos;			/* its first descriptor */
	UD	seq;
	INT	ndesc;			/* its descriptors */
} TXREC;

typedef struct {
	UD	blk;			/* a block revoked */
	UD	seq;			/* by this transaction */
} RVK;

/* What a scan of the ring found: the transactions that became real, and their revokes */
typedef struct {
	TXREC	*tx;
	INT	ntx, maxtx;
	RVK	*rvk;
	INT	nrvk, rvk_max;
	UD	end;			/* past the commit block of the last good one */
	UD	next;			/* the number after it */
	BOOL	short_mem;		/* the revokes did not fit in memory: the scan stopped there */
} SCAN;

/* The revokes in the order of their blocks, then of their numbers */
LOCAL void rvk_sort( RVK *r, INT n )
{
	INT	gap, i, k;
	RVK	x;

	for ( gap = n / 2; gap > 0; gap /= 2 ) {
		for ( i = gap; i < n; i++ ) {
			x = r[i];
			for ( k = i; k >= gap && ( r[k - gap].blk > x.blk
				|| ( r[k - gap].blk == x.blk && r[k - gap].seq > x.seq ) ); k -= gap ) {
				r[k] = r[k - gap];
			}
			r[k] = x;
		}
	}
}

/* Whether `blk` was revoked by transaction `seq` or one after it (the table sorted) */
LOCAL BOOL rvk_has( CONST RVK *r, INT n, UD blk, UD seq )
{
	INT	lo = 0, hi = n, mid;

	while ( lo < hi ) {
		mid = lo + ( hi - lo ) / 2;
		if ( r[mid].blk < blk ) lo = mid + 1;
		else hi = mid;
	}
	for ( ; lo < n && r[lo].blk == blk; lo++ ) {
		if ( r[lo].seq >= seq ) return TRUE;
	}
	return FALSE;
}

/*
 * The ring read from the head on, as a ring of version 5 (v5) or 4:
 * every transaction whose blocks are all there and whose commit block
 * is good, until the first that is not. A version 5 transaction is its
 * descriptors, numbered from 0, each followed by its copies, then its
 * revoke blocks, numbered from 0, then the commit block, which says how
 * many of each there were. A block that is none of what may come next
 * ends the scan, so a descriptor whose number skips one does too.
 */
LOCAL void ring_scan( TSFSJRNL *j, BOOL v5, UB *buf, UB *blk, SCAN *s )
{
	UD	pos = j->head, cur = j->head_seq, scanned, txpos = 0;
	INT	nd = 0, nc = 0, nr = 0, nrvk_good = 0, i, n;
	UW	chain = 0, c;
	BOOL	in_tx = FALSE;

	s->ntx = 0;
	s->nrvk = 0;
	s->end = pos;
	s->next = cur;
	for ( scanned = 0; scanned < j->len && s->ntx < s->maxtx; ) {
		if ( knl_tsfsblk_raw_read(j->vol, ring_blk(j, pos), buf) < E_OK ) {
			break;
		}
		if ( rd64(buf + JB_SEQ) != cur || rd32(buf + JB_CRC) != crc_at(j, buf, JB_CRC) ) {
			break;
		}
		if ( is_kind(buf, JB_DESC, v5)
		  && ( !in_tx ? ( !v5 || rd32(buf + JB_INDEX) == 0 )
			      : ( v5 && nr == 0 && rd32(buf + JB_INDEX) == (UW)nd ) ) ) {
			n = (INT)rd32(buf + JB_COUNT);
			if ( n > (INT)TSFSJ_MAX_BLOCKS ) break;
			c = rd32(buf + JB_CRC);
			if ( !in_tx ) {
				chain = ts_crc32c((CONST UB *)&c, 4);
				txpos = pos;
				nd = nc = nr = 0;
			} else {
				chain ^= chain_term(c, DESC_SALT + (UW)nd);
			}
			for ( i = 0; i < n; i++ ) {
				if ( knl_tsfsblk_raw_read(j->vol, ring_blk(j, pos + 1 + (UD)i), blk) < E_OK ) break;
				c = ts_crc32c(blk, TSFSBLK_BLOCK_SIZE);
				if ( c != rd32(buf + JB_ENT + i * TSFSJ_DESC_ENT + 8) ) break;
				chain ^= chain_term(c, (UW)( nc + i ));
			}
			if ( i < n ) break;		/* a copy did not reach the medium */
			nc += n;
			nd++;
			pos += 1 + (UD)n;
			scanned += 1 + (UD)n;
			in_tx = TRUE;
			continue;
		}
		if ( in_tx && is_kind(buf, JB_REVOKE, v5) && ( !v5 || rd32(buf + JB_INDEX) == (UW)nr ) ) {
			n = (INT)rd32(buf + JB_COUNT);
			if ( n > (INT)TSFSJ_MAX_REVOKE ) break;
			if ( !grow_tbl((void **)&s->rvk, &s->rvk_max, s->nrvk + n, sizeof(RVK)) ) {
				s->short_mem = TRUE;
				break;
			}
			for ( i = 0; i < n; i++ ) {
				s->rvk[s->nrvk].blk = rd64(buf + JB_ENT + i * 8);
				s->rvk[s->nrvk].seq = cur;
				s->nrvk++;
			}
			chain ^= chain_term(rd32(buf + JB_CRC), REVOKE_SALT + ( v5 ? (UW)nr : 0 ));
			nr++;
			pos++;
			scanned++;
			continue;
		}
		if ( in_tx && is_kind(buf, JB_COMMIT, v5) ) {
			if ( rd32(buf + JC_CHAIN) != chain ) break;
			if ( v5 && ( rd32(buf + JC_NDESC) != (UW)nd || rd32(buf + JC_NRVK) != (UW)nr ) ) break;
			s->tx[s->ntx].pos = txpos;
			s->tx[s->ntx].seq = cur;
			s->tx[s->ntx].ndesc = nd;
			s->ntx++;
			cur++;
			pos++;
			scanned++;
			s->end = pos;
			s->next = cur;
			nrvk_good = s->nrvk;	/* its revokes count from now */
			in_tx = FALSE;
			continue;
		}
		break;
	}
	/* one whose commit is not there never happened, nor did its revokes */
	s->nrvk = nrvk_good;
}

/* The transactions a scan found put back in order, leaving out what was revoked later */
LOCAL ER ring_apply( TSFSJRNL *j, SCAN *s, UB *buf, UB *blk )
{
	UD	p, home;
	INT	k, d, i, n;
	ER	er = E_OK;

	rvk_sort(s->rvk, s->nrvk);
	for ( k = 0; k < s->ntx && er >= E_OK; k++ ) {
		p = s->tx[k].pos;
		for ( d = 0; d < s->tx[k].ndesc && er >= E_OK; d++ ) {
			er = knl_tsfsblk_raw_read(j->vol, ring_blk(j, p), buf);
			n = ( er >= E_OK ) ? (INT)rd32(buf + JB_COUNT) : 0;
			for ( i = 0; i < n && er >= E_OK; i++ ) {
				home = rd64(buf + JB_ENT + i * TSFSJ_DESC_ENT);
				if ( rvk_has(s->rvk, s->nrvk, home, s->tx[k].seq) ) continue;
				er = knl_tsfsblk_raw_read(j->vol, ring_blk(j, p + 1 + (UD)i), blk);
				if ( er >= E_OK ) er = knl_tsfsblk_raw_write(j->vol, home, blk);
			}
			p += 1 + (UD)n;
		}
	}
	return er;
}

/*
 * A head and a ring that do not go with the volume: a volume of version
 * 5 whose head is of version 4 with transactions in the ring, or one of
 * version 4 whose head is of version 5. Nothing of the ring is put back
 * and the volume is marked damaged; it is read only until checked.
 */
LOCAL ER ring_wrong( TSFSJRNL *j, CONST char *what )
{
	tm_printf((UB *)"tsfs: volume %d: %s: its log is not put back, and the volume is marked damaged\n",
		  (INT)j->vol, what);
	ts_blk_error(j->vol, j->start, TSFSBLK_ERR_CSUM);
	j->len = 0;
	j->blocks = 0;
	return E_OBJ;
}

/*
 * Put back whatever a cut in the power left behind: every transaction
 * from the head on whose commit block is good, in order, leaving out a
 * copy of a block that the same or a later transaction revoked. Then the
 * head moves past them, so they are not put back twice.
 *
 * The ring is of the volume's version: 5 when the superblock has FMT5.
 * A volume of version 5 whose head is still of version 4 is one whose
 * conversion the power cut short after its superblock; its ring was
 * emptied before, so when nothing in it reads as a transaction of
 * version 4 the head is written as version 5 and the conversion is
 * done.
 */
EXPORT ER ts_jrnl_replay( ID vol )
{
	TSFSJRNL	*j;
	T_TSFSBLK_SB	sb;
	SCAN		s;
	UB	*buf, *blk;
	UD	start = 0, blocks = 0, seq = 1;
	BOOL	fmt5 = FALSE, good;
	UW	hv;
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
	if ( ts_ref_vol_blk(vol, &sb) >= E_OK ) {
		j->salt = ts_crc32c(sb.vol_uuid.b, 16) ^ (UW)sb.mkfs_time;
		fmt5 = (BOOL)( ( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) != 0 );
	}
	j->v5 = fmt5;
	j->vbudget = vol_budget(j);

	knl_memset(&s, 0, sizeof(s));
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	s.maxtx = (INT)( j->len / 2 + 1 );
	s.tx = (TXREC *)Kmalloc(sizeof(TXREC) * (SZ)s.maxtx);
	if ( buf == NULL || blk == NULL || s.tx == NULL
	  || !grow_tbl((void **)&s.rvk, &s.rvk_max, REPLAY_RVK_FIRST, sizeof(RVK)) ) {
		er = E_NOMEM;
		goto exit;
	}

	/* the head block; a volume just made has none yet */
	if ( knl_tsfsblk_raw_read(vol, start, buf) < E_OK ) {
		er = E_IO;
		goto exit;
	}
	good = (BOOL)( buf[0] == (UB)JH_MAGIC[0] && buf[1] == (UB)JH_MAGIC[1] && buf[2] == (UB)JH_MAGIC[2]
		    && buf[3] == (UB)JH_MAGIC[3] && rd32(buf + JH_CRC) == crc_at(j, buf, JH_CRC)
		    && rd64(buf + JH_POS) < j->len );
	hv = rd32(buf + JH_VERSION);
	if ( good && fmt5 && hv == TSFSJ_VERSION ) {
		/* a conversion whose last step the power cut short */
		j->head = rd64(buf + JH_POS);
		j->head_seq = rd64(buf + JH_SEQ);
		ring_scan(j, FALSE, buf, blk, &s);
		if ( s.ntx > 0 ) {
			er = ring_wrong(j, "version 5, its log's head of version 4 with transactions after it");
			goto exit;
		}
		j->tail = j->head;
		j->used = 0;
		j->seq = j->head_seq;
		j->dseq = j->head_seq;
		er = head_write(j, j->head, j->head_seq);
		if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);
		if ( er >= E_OK ) {
			j->conv_left = TRUE;
			tm_printf((UB *)"tsfs: volume %d: convert: the last step finished (the log's head of version 5)\n",
				  (INT)vol);
		}
		goto exit;
	}
	if ( good && !fmt5 && hv == TSFSJ_VERSION5 ) {
		er = ring_wrong(j, "version 4, its log's head of version 5");
		goto exit;
	}
	if ( good && hv == ring_ver(fmt5) ) {
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

	/* find the transactions that became real, and put them back */
	ring_scan(j, fmt5, buf, blk, &s);
	if ( s.short_mem ) {
		tm_printf((UB *)"tsfs: volume %d: no memory for the log's revokes: put back up to transaction %ld\n",
			  (INT)vol, (D)s.next);
	}
	er = ring_apply(j, &s, buf, blk);
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);

	/* the head past what was put back; the ring is empty */
	if ( er >= E_OK ) {
		j->head = s.end % j->len;		/* past the commit of the last good one */
		j->tail = j->head;
		j->used = 0;
		j->head_seq = s.next;
		j->seq = s.next;
		j->dseq = s.next;
		er = head_write(j, j->head, j->head_seq);
		if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(vol);
	}

    exit:
	if ( buf != NULL ) Kfree(buf);
	if ( blk != NULL ) Kfree(blk);
	if ( s.tx != NULL ) Kfree(s.tx);
	if ( s.rvk != NULL ) Kfree(s.rvk);
	if ( er >= E_OK && j->seq != 0 ) {
		ts_set_jrnl_blk(vol, start, blocks, j->seq);
	}
	return er;
}
