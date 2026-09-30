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
 *	Write ahead log of a native volume (design 11.14.1).
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
 *	The commit block is written last, after the device has been told to
 *	empty its cache, and again after it: the transaction is real from
 *	then on. Its copies stay in memory and are put in place later,
 *	together (a checkpoint), after which the head block moves past it.
 *
 *	The journal reads and writes the device past the volume's lock
 *	(knl_tsfsblk_raw_*), because the volume's lock is taken outside this
 *	one: the block layer asks here for copies while holding it.
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
#define CACHE_MAX		512	/* committed blocks held before a checkpoint is forced */
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

typedef struct {
	ID	vol;			/* 0: the entry is free */
	ID	mtx;
	UD	start, blocks;		/* the journal area */
	UD	len;			/* blocks of the ring */
	UD	head, tail;		/* places in the ring */
	UD	used;			/* ring blocks between head and tail */
	UD	head_seq;		/* number of the transaction at head */
	UD	seq;			/* the number the next one takes */
	INT	handles;		/* on the running transaction */
	BOOL	committing;		/* the last handle let go; it is being written */
	BOOL	doomed;			/* a handle gave up: the whole of it goes */
	JENT	txn[TSFSJ_MAX_BLOCKS];
	INT	ntxn;
	UD	revoke[TSFSJ_MAX_REVOKE];
	INT	nrevoke;
	BOOL	revoke_over;		/* more than one block of revokes */
	CNODE	*bucket[CACHE_BUCKETS];
	UD	ncached;
	UINT	fault;
	BOOL	dead;			/* after TSFSJ_FAULT_AFTER_COMMIT */
	UW	salt;			/* of this volume, mixed into every CRC */
} TSFSJRNL;

LOCAL TSFSJRNL	jrnl_tbl[TSFSJ_MAX_VOL];
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
	INT	i;

	for ( i = 0; i < TSFSJ_MAX_VOL; i++ ) {
		if ( jrnl_tbl[i].vol == vol && vol > 0 ) {
			return &jrnl_tbl[i];
		}
	}
	return NULL;
}

LOCAL void checkpointer( INT stacd, void *exinf );

LOCAL TSFSJRNL *jrnl_make( ID vol )
{
	T_CMTX	cm;
	T_CTSK	ct;
	TSFSJRNL	*j = jrnl_find(vol);
	INT	i;

	if ( j != NULL ) {
		return j;
	}
	for ( i = 0; i < TSFSJ_MAX_VOL && jrnl_tbl[i].vol != 0; i++ ) ;
	if ( i >= TSFSJ_MAX_VOL ) {
		return NULL;
	}
	j = &jrnl_tbl[i];
	knl_memset(j, 0, sizeof(*j));
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cm.ceilpri = 0;
	j->mtx = tk_cre_mtx(&cm);
	if ( j->mtx <= 0 ) {
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

/* ---------------------------------------------------------------- committed blocks */

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
		Kfree(n->buf);
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
		Kfree(n->buf);
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
			Kfree(n->buf);
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

LOCAL ER head_write( TSFSJRNL *j )
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
	wr64(buf + JH_SEQ, j->head_seq);
	wr64(buf + JH_POS, j->head);
	wr32(buf + JH_CRC, crc_at(j, buf, JH_CRC));
	er = knl_tsfsblk_raw_write(j->vol, j->start, buf);
	Kfree(buf);
	return er;
}

/*
 * Every committed block in its place, then the head moved past all of
 * it. The device's cache is emptied between the two, so the head never
 * gets ahead of what it stands for.
 */
LOCAL ER ckpt_locked( TSFSJRNL *j )
{
	CNODE	*n;
	INT	i;
	ER	er = E_OK;

	if ( j->dead ) {
		return E_OK;
	}
	if ( j->ncached == 0 && j->used == 0 ) {
		return E_OK;
	}
	for ( i = 0; i < CACHE_BUCKETS && er >= E_OK; i++ ) {
		for ( n = j->bucket[i]; n != NULL && er >= E_OK; n = n->next ) {
			er = knl_tsfsblk_raw_write(j->vol, n->blk, n->buf);
		}
	}
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	if ( er < E_OK ) {
		return er;			/* the ring still holds it all */
	}
	j->head = j->tail;
	j->head_seq = j->seq;
	j->used = 0;
	er = head_write(j);
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	cache_clear(j);

	return er;
}

/* ---------------------------------------------------------------- the checkpointer */

LOCAL void checkpointer( INT stacd, void *exinf )
{
	INT	i;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		tk_dly_tsk(CNF_TSFS_FLUSH_MS);
		for ( i = 0; i < TSFSJ_MAX_VOL; i++ ) {
			TSFSJRNL	*j = &jrnl_tbl[i];

			if ( j->vol <= 0 || j->ncached == 0 ) continue;
			if ( tk_loc_mtx(j->mtx, TMO_POL) < E_OK ) continue;
			if ( j->vol > 0 && j->handles == 0 && !j->committing ) {
				(void)ckpt_locked(j);
			}
			tk_unl_mtx(j->mtx);
		}
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
		if ( j->dead ) {
			tk_unl_mtx(j->mtx);
			return E_IO;
		}
		if ( !j->committing ) {
			break;
		}
		tk_unl_mtx(j->mtx);
		tk_dly_tsk(1);			/* the last one is being written */
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

/* The running transaction, into the ring. The caller holds the lock. */
LOCAL ER ring_write( TSFSJRNL *j, UD *p_foot )
{
	UB	*buf;
	UW	chain = 0, c;
	UD	pos = j->tail, foot;
	INT	i;
	ER	er;

	foot = 1 + (UD)j->ntxn + ( j->nrevoke > 0 ? 1 : 0 ) + 1;
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}

	/* descriptor */
	make_kind(buf, JB_DESC, j->seq, (UW)j->ntxn);
	for ( i = 0; i < j->ntxn; i++ ) {
		wr64(buf + JB_ENT + i * TSFSJ_DESC_ENT, j->txn[i].blk);
		wr32(buf + JB_ENT + i * TSFSJ_DESC_ENT + 8,
		     ts_crc32c(j->txn[i].buf, TSFSBLK_BLOCK_SIZE));
	}
	c = crc_at(j, buf, JB_CRC);
	wr32(buf + JB_CRC, c);
	chain = ts_crc32c((CONST UB *)&c, 4);
	er = knl_tsfsblk_raw_write(j->vol, ring_blk(j, pos++), buf);

	/* copies */
	for ( i = 0; i < j->ntxn && er >= E_OK; i++ ) {
		c = ts_crc32c(j->txn[i].buf, TSFSBLK_BLOCK_SIZE);
		chain ^= ts_crc32c((CONST UB *)&c, 4) + (UW)i;
		er = knl_tsfsblk_raw_write(j->vol, ring_blk(j, pos++), j->txn[i].buf);
	}

	/* revokes */
	if ( er >= E_OK && j->nrevoke > 0 ) {
		make_kind(buf, JB_REVOKE, j->seq, (UW)j->nrevoke);
		for ( i = 0; i < j->nrevoke; i++ ) {
			wr64(buf + JB_ENT + i * 8, j->revoke[i]);
		}
		c = crc_at(j, buf, JB_CRC);
		wr32(buf + JB_CRC, c);
		chain ^= ts_crc32c((CONST UB *)&c, 4) + 0x5A5A5A5AU;
		er = knl_tsfsblk_raw_write(j->vol, ring_blk(j, pos++), buf);
	}

	/* the device holds all of that before the commit goes */
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	if ( er >= E_OK && j->fault == TSFSJ_FAULT_BEFORE_COMMIT ) {
		Kfree(buf);
		return E_OK;			/* as if the power went before the commit */
	}

	/* commit */
	if ( er >= E_OK ) {
		TS_TIME	t = 0;

		make_kind(buf, JB_COMMIT, j->seq, 0);
		(void)dt_gettime(&t);
		wr64(buf + JC_TIME, (UD)t);
		wr32(buf + JC_CHAIN, chain);
		wr32(buf + JC_BLOCKS, (UW)( foot - 1 ));
		wr32(buf + JB_CRC, crc_at(j, buf, JB_CRC));
		er = knl_tsfsblk_raw_write(j->vol, ring_blk(j, pos++), buf);
	}
	if ( er >= E_OK ) er = knl_tsfsblk_raw_flush(j->vol);
	Kfree(buf);

	*p_foot = foot;
	return er;
}

/*
 * Let a handle go. The last one commits: what the maps, the group heads
 * and the superblock owe goes in first, then the transaction goes into
 * the ring, and its copies are kept in memory until a checkpoint puts
 * them in place.
 */
EXPORT ER ts_jrnl_commit( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	UD	foot = 0, seq;
	INT	i;
	ER	er;

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
		return E_OK;			/* the last one commits it */
	}
	if ( j->doomed || j->fault == TSFSJ_FAULT_BEFORE_COMMIT ) {
		er = ( j->doomed ) ? E_IO : E_OK;
		if ( j->fault == TSFSJ_FAULT_BEFORE_COMMIT && j->ntxn > 0 ) {
			/* the ring is written as far as the commit, as a
			   machine that loses its power there would leave it */
			UD	f;

			(void)ring_write(j, &f);
		}
		txn_drop(j);
		tk_unl_mtx(j->mtx);
		ts_blk_jrnl_discard(vol);
		return er;
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

	/* room in the ring, and revokes that fit in one block */
	foot = 1 + (UD)j->ntxn + ( j->nrevoke > 0 ? 1 : 0 ) + 1;
	if ( j->revoke_over || foot > j->len - j->used ) {
		er = ckpt_locked(j);
		if ( er >= E_OK && j->revoke_over ) {
			j->nrevoke = 0;		/* nothing older is left to come back */
			j->revoke_over = FALSE;
		}
	} else {
		er = E_OK;
	}
	if ( er >= E_OK ) {
		er = ring_write(j, &foot);
	}
	if ( er < E_OK ) {
		txn_drop(j);
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		ts_blk_error(vol, j->start, TSFSBLK_ERR_IO);
		return er;
	}
	if ( j->fault == TSFSJ_FAULT_AFTER_COMMIT ) {
		txn_drop(j);			/* memory goes with the power */
		j->dead = TRUE;
		j->committing = FALSE;
		tk_unl_mtx(j->mtx);
		return E_OK;
	}

	/* the copies wait in memory; revoked blocks are forgotten */
	for ( i = 0; i < j->nrevoke; i++ ) {
		cache_del(j, j->revoke[i]);
	}
	for ( i = 0; i < j->ntxn; i++ ) {
		if ( cache_put(j, j->txn[i].blk, j->txn[i].buf) < E_OK ) {
			Kfree(j->txn[i].buf);
		}
	}
	j->ntxn = 0;
	j->nrevoke = 0;
	j->tail = ( j->tail + foot ) % j->len;
	j->used += foot;
	j->seq++;
	seq = j->seq;
	if ( j->ncached > CACHE_MAX || j->used > j->len - TSFSJ_TXN_MAX_FOOT ) {
		(void)ckpt_locked(j);
	}
	j->committing = FALSE;
	tk_unl_mtx(j->mtx);

	ts_set_jrnl_blk(vol, j->start, j->blocks, seq);
	return E_OK;
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
	ER	er;

	if ( j == NULL ) {
		return E_OK;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	er = ckpt_locked(j);
	tk_unl_mtx(j->mtx);
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
	tk_unl_mtx(j->mtx);

	return E_OK;
}

EXPORT ER ts_jrnl_fault( ID vol, UINT where )
{
	TSFSJRNL	*j = jrnl_make(vol);

	if ( j == NULL ) {
		return E_LIMIT;
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
 */
EXPORT void knl_jrnl_forget_blk( ID vol, UD blk )
{
	TSFSJRNL	*j = jrnl_find(vol);

	if ( j == NULL || j->ncached == 0 ) {
		return;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	if ( *cache_slot(j, blk) != NULL ) {
		(void)ckpt_locked(j);
	}
	tk_unl_mtx(j->mtx);
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
	if ( in_ring ) {
		(void)ckpt_locked(j);
	}
	tk_unl_mtx(j->mtx);
}

EXPORT ER knl_jrnl_close( ID vol )
{
	TSFSJRNL	*j = jrnl_find(vol);
	ER	er;

	if ( j == NULL ) {
		return E_OK;
	}
	tk_loc_mtx(j->mtx, TMO_FEVR);
	er = ckpt_locked(j);			/* does nothing after a pretended power cut */
	txn_drop(j);
	cache_clear(j);
	j->vol = 0;
	tk_unl_mtx(j->mtx);
	tk_del_mtx(j->mtx);

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
		er = head_write(j);
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
		er = head_write(j);
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
