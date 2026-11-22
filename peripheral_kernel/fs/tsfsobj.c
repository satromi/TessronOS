/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsobj.c
 *	Real objects on a native volume (design 11.6.6, 11.7).
 *
 *	One object is one object block. The object index carries its UUID to
 *	that block; the block's fixed head says where each part of the
 *	object is, and its inline area holds the parts small enough to fit.
 *
 *	Making, changing and dropping an object are journal transactions, so
 *	the object block, the index, the garbage list and the management
 *	parts become real at one moment. Bytes of records and resources are
 *	written before the transaction is committed and are not logged,
 *	which is what ordered mode means.
 */

/* The layout of the object block is in tsfsfmt.h. */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>
#include <ts/dt.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include <ts/json.h>
#include "tstdlib.h"
#include "tsfsfmt.h"

#define RELINK_STEP	64		/* objects one transaction changes the counts of */

/*
 * The limits of a volume come from the block layer (ts_blk_lim): a
 * volume of the format's version 4 keeps the ones of before (256
 * records, 64 resources, 1024 links, 15 bytes of an extension, the
 * metadata one transaction of 254 copies holds, places up to F_TREE2),
 * one of version 5 has none of them (0) and may hold the forms of
 * version 5: run trees of any depth (F_TREEN), management parts in more
 * runs than one, long extensions in the string area of the resource
 * table, and the spare place. A version 4 volume is never given one of
 * those forms.
 */
#define TSFSO_ICON_MAX	TSFS_ICON_MAX	/* bytes of an icon */
#define DATA_INLINE_MAX	2048		/* the most bytes of a record held inline */
#define TSFSO_GC_GUARD	100000		/* objects one collection walks over */
#define XT_FAN		TSFSO_MAX_EXTENTS	/* entries of a node of a run tree */
#define XT_DEPTH_MAX	8		/* levels of a run tree a reader follows: 165^8 is past 2^58 */
#define V4_XT2_MAX	( (UD)TSFSO_MAX_EXTENTS * TSFSO_MAX_EXTENTS )	/* runs of a part on version 4 */
#define MAP_SPAN	( (UD)TSFSBLK_BLOCK_SIZE * 8 )	/* blocks one block of the free map covers */
#define STEP_KEEP	6		/* copies a step leaves: superblock, group head, object block, slack */

/* Open user transactions, one slot per volume the block layer holds */
LOCAL INT	obj_trx[TSFSBLK_MAX_VOL];

/* ts_obj_fan: the entries a node of a run tree is filled with, 0 for XT_FAN */
LOCAL INT	obj_fan[TSFSBLK_MAX_VOL];

/* ts_obj_cut: the transactions to close before the power is pretended to go, 0 for none */
LOCAL INT	obj_cut[TSFSBLK_MAX_VOL];

/* Before a transaction is closed: the one ts_obj_cut named is closed as the power goes */
LOCAL void cut_check( ID vol )
{
	if ( obj_cut[vol - 1] > 0 && --obj_cut[vol - 1] == 0 ) {
		(void)ts_jrnl_fault(vol, TSFSJ_FAULT_AFTER_COMMIT);
	}
}

/*
 * The runs a volume's open transaction gave up, to be given back once
 * it is real: made the first time the volume's number has any and kept
 * for that number after, the table twice as large each time it fills.
 */
typedef struct {
	UD	*start;
	UD	*count;
	INT	n, cap;
} PEND;

LOCAL PEND	*pend_tbl[TSFSBLK_MAX_VOL];

/*
 * The objects whose spare place a transaction filled (spare_take), to be
 * emptied a step at a time once it is real (spare_drain).
 */
typedef struct {
	TS_UUID	*u;
	INT	n, cap;
} SPQ;

LOCAL SPQ	*spq_tbl[TSFSBLK_MAX_VOL];

/*
 * Each volume's generation: one more at the start and at the end of
 * every transaction, so that anything read from the volume's objects
 * and kept while the generation is the same is still what the volume
 * holds (rc_*). Reads do not change it.
 */
LOCAL UD	obj_gen[TSFSBLK_MAX_VOL];
#define GEN_BUMP(vol)	__atomic_add_fetch(&obj_gen[(vol) - 1], 1, __ATOMIC_RELEASE)
#define GEN_NOW(vol)	__atomic_load_n(&obj_gen[(vol) - 1], __ATOMIC_ACQUIRE)

/* ---------------------------------------------------------------- bytes */

LOCAL UH rd16( CONST UB *p )
{
	return (UH)((UW)p[0] | ((UW)p[1] << 8));
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ((UD)rd32(p + 4) << 32);
}

LOCAL void wr16( UB *p, UH v )
{
	p[0] = (UB)v;
	p[1] = (UB)(v >> 8);
}

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)(v >> 8);
	p[2] = (UB)(v >> 16);
	p[3] = (UB)(v >> 24);
}

LOCAL void wr64( UB *p, UD v )
{
	wr32(p, (UW)v);
	wr32(p + 4, (UW)(v >> 32));
}

LOCAL INT str_len( CONST UB *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != '\0' ) {
		n++;
	}
	return n;
}

LOCAL void field_get( CONST UB *src, INT max, UB *dst, INT dstmax )
{
	INT	i;

	for ( i = 0; i < max && i < dstmax - 1 && src[i] != '\0'; i++ ) {
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

/*
 * The clock counts seconds; the object block holds nanoseconds, so that
 * the field does not have to change when a finer clock arrives. Both
 * count from 1985-01-01 UTC; a clock that has not been set gives zero.
 */
LOCAL UD now_ns( void )
{
	TS_TIME	now = 0;

	if ( dt_gettime(&now) < E_OK || now <= 0 ) {
		return 0;
	}
	return (UD)now * 1000000000ULL;
}

#define BLKS_FOR(len, per)	( ( (UD)(len) + (per) - 1 ) / (per) )

/* ---------------------------------------------------------------- blocks */

/*
 * A management block goes into the open transaction when there is one,
 * and straight to its block when there is not.
 */
LOCAL ER meta_write( ID vol, UD blk, CONST UB *buf )
{
	T_TSFSJRNL	j;

	if ( ts_jrnl_ref(vol, &j) >= E_OK && j.open != 0 ) {
		return ts_jrnl_write(vol, blk, buf);
	}
	return ts_write_blk(vol, blk, buf);
}

/*
 * Management blocks are read through the log, so that a block changed
 * earlier in the same transaction is seen as it will be.
 */
LOCAL ER meta_read( ID vol, UD blk, UB *buf )
{
	return ts_jrnl_read(vol, blk, buf);
}

/* ---------------------------------------------------------------- the object block */

typedef struct {
	ID	vol;
	UD	blk;			/* the object block */
	UD	owner;			/* low 64 bits of the UUID */
	UB	*hdr;			/* the object block, 4096 bytes */
	T_TSFSLIM lim;			/* the volume's limits (ts_blk_lim) */
	BOOL	v5;			/* the forms of version 5 may be written */
	BOOL	direct;			/* the tree being written is reached by nothing yet */
} OBJ;

/* The limits of a volume; those of version 4 when the block layer does not answer */
LOCAL void lim_of( ID vol, T_TSFSLIM *l )
{
	if ( ts_blk_lim(vol, l) < E_OK ) {
		l->max_rec = TSFSO_MAX_REC;
		l->max_res = TSFSO_MAX_RES;
		l->max_link = TSFSO_MAX_LINK;
		l->ext_max = TSFSO_EXT_LEN - 1;
		l->meta_max = (UD)( ( TSFSJ_MAX_BLOCKS - 8 ) / 2 * TSFSBLK_BLOCK_SIZE );
		l->xt_form = F_TREE2;
		l->txn_budget = TSFSJ_MAX_BLOCKS;
	}
}

LOCAL void obj_lim( OBJ *o )
{
	lim_of(o->vol, &o->lim);
	o->v5 = (BOOL)( o->lim.xt_form >= F_TREEN );
	o->direct = FALSE;
}

/* Whether `n` is past a limit, 0 being none */
LOCAL BOOL over( UD max, UD n )
{
	return (BOOL)( max != 0 && n > max );
}

/*
 * The size from which a management part, or the blocks a change gives
 * up, no longer go through the transaction of the change: an eighth of
 * the copies a transaction holds (design 7.3).
 */
LOCAL UD big_blocks( ID vol )
{
	INT	b = ts_jrnl_budget(vol);

	b = ( b > 0 ) ? b / 8 : TSFSJ_MAX_BLOCKS / 8;
	return ( b < 2 ) ? 2 : (UD)b;
}

LOCAL BOOL same16( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL UD owner_of( CONST TS_UUID *uuid )
{
	return rd64(uuid->b + 8);
}

/* The object a caller names, read and checked */
LOCAL ER obj_load( ID vol, CONST TS_UUID *uuid, OBJ *o )
{
	UD	blk = 0;
	ER	er;

	o->hdr = NULL;
	if ( uuid == NULL ) {
		return E_PAR;
	}
	er = ts_btree_lookup(vol, uuid, &blk);
	if ( er == E_NOEXS ) {
		/* one deleted while open is still there for those that have it */
		er = ts_btree_lookup_t(vol, TSFSBT_ORPHAN, uuid, &blk);
		if ( er < E_OK ) {
			return E_NOEXS;
		}
	}
	if ( er < E_OK ) {
		return er;
	}
	if ( blk == 0 ) {
		return E_OBJ;
	}
	o->hdr = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( o->hdr == NULL ) {
		return E_NOMEM;
	}
	o->vol = vol;
	o->blk = blk;
	o->owner = owner_of(uuid);
	obj_lim(o);
	er = meta_read(vol, blk, o->hdr);
	if ( er >= E_OK && ts_blk_check(vol, o->hdr, TSFSBLK_MAGIC_OBJ, blk) < E_OK ) {
		er = E_IO;			/* damaged: the volume stops writing */
	}
	if ( er >= E_OK && !same16(o->hdr + OB_UUID, uuid->b) ) {
		ts_blk_error(vol, blk, TSFSBLK_ERR_TREE);
		er = E_OBJ;			/* the index points at someone else */
	}
	if ( er < E_OK ) {
		Kfree(o->hdr);
		o->hdr = NULL;
	}
	return er;
}

LOCAL ER obj_store( OBJ *o )
{
	ts_blk_seal(o->vol, o->hdr, TSFSBLK_MAGIC_OBJ, o->blk, o->owner);
	return meta_write(o->vol, o->blk, o->hdr);
}

LOCAL void obj_drop( OBJ *o )
{
	if ( o->hdr != NULL ) {
		Kfree(o->hdr);
		o->hdr = NULL;
	}
}

LOCAL void obj_touch( OBJ *o )
{
	wr64(o->hdr + OB_UPDATED, now_ns());
}

/* ---------------------------------------------------------------- the inline area */

LOCAL UD inl_map( CONST UB *hdr )
{
	return rd64(hdr + OB_INLMAP);
}

/* Room for `bytes` in the inline area: its offset in the block, or -1 */
LOCAL INT inl_take( UB *hdr, SZ bytes )
{
	UD	map = inl_map(hdr), want;
	INT	units = (INT)BLKS_FOR(bytes, TSFSO_INLINE_UNIT), u, k;

	if ( bytes <= 0 || units > INL_UNITS ) {
		return -1;
	}
	for ( u = 0; u + units <= INL_UNITS; u++ ) {
		for ( k = 0; k < units && ( map & ( 1ULL << ( u + k ) ) ) == 0; k++ ) ;
		if ( k == units ) {
			want = ( ( units == 64 ) ? ~0ULL : ( ( 1ULL << units ) - 1 ) ) << u;
			wr64(hdr + OB_INLMAP, map | want);
			return OB_INLINE + u * TSFSO_INLINE_UNIT;
		}
		u += k;
	}
	return -1;
}

LOCAL void inl_give( UB *hdr, INT off, INT units )
{
	UD	map = inl_map(hdr);
	INT	u = ( off - OB_INLINE ) / TSFSO_INLINE_UNIT, k;

	for ( k = 0; k < units; k++ ) {
		map &= ~( 1ULL << ( u + k ) );
	}
	wr64(hdr + OB_INLMAP, map);
}

/* ---------------------------------------------------------------- runs given back later */

/* What a volume's number has waiting, NULL for nothing */
LOCAL PEND *pend_of( ID vol )
{
	return ( vol >= 1 && vol <= TSFSBLK_MAX_VOL )
	       ? __atomic_load_n(&pend_tbl[vol - 1], __ATOMIC_ACQUIRE) : NULL;
}

LOCAL void pend_clear( ID vol )
{
	PEND	*p = pend_of(vol);

	if ( p != NULL ) p->n = 0;
}

LOCAL ER pend_add( ID vol, UD start, UD count )
{
	PEND	*p = pend_of(vol), *none = NULL;

	if ( p == NULL ) {
		if ( vol < 1 || vol > TSFSBLK_MAX_VOL ) {
			return E_ID;
		}
		p = (PEND *)Kcalloc(1, sizeof(PEND));
		if ( p == NULL ) {
			return E_NOMEM;
		}
		if ( !__atomic_compare_exchange_n(&pend_tbl[vol - 1], &none, p, FALSE,
						  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE) ) {
			Kfree(p);
			p = none;
		}
	}
	if ( p->n >= p->cap ) {
		INT	cap = ( p->cap > 0 ) ? p->cap * 2 : 64;
		UD	*ns = (UD *)Kmalloc(sizeof(UD) * (SZ)cap);
		UD	*nc = (UD *)Kmalloc(sizeof(UD) * (SZ)cap);

		if ( ns == NULL || nc == NULL ) {
			if ( ns != NULL ) Kfree(ns);
			if ( nc != NULL ) Kfree(nc);
			return E_NOMEM;
		}
		if ( p->n > 0 ) {
			knl_memcpy(ns, p->start, sizeof(UD) * (SZ)p->n);
			knl_memcpy(nc, p->count, sizeof(UD) * (SZ)p->n);
		}
		if ( p->start != NULL ) Kfree(p->start);
		if ( p->count != NULL ) Kfree(p->count);
		p->start = ns;
		p->count = nc;
		p->cap = cap;
	}
	p->start[p->n] = start;
	p->count[p->n] = count;
	p->n++;
	return E_OK;
}

/*
 * The runs a replacement gave up, freed in a transaction of their own
 * behind the one that stopped using them: the log puts them on the
 * medium in that order, and the runs are not handed out again before
 * the freeing one is there (tsfsblk.c, busy runs). A cut in the power
 * between the two leaves them taken but unused, which a check of the
 * volume finds and gives back.
 */
LOCAL UD map_cost( UD start, UD count );

LOCAL void pend_apply( ID vol )
{
	PEND	*p = pend_of(vol);
	INT	i = 0, took;
	UD	used;

	if ( p == NULL || p->n == 0 ) {
		return;
	}
	/* as many in each transaction as the copies of the free map they
	   change leave room for (the block layer hands those to the log
	   when the transaction closes), one at least */
	while ( i < p->n ) {
		GEN_BUMP(vol);
		if ( ts_jrnl_begin(vol) < E_OK ) {
			GEN_BUMP(vol);
			break;
		}
		used = 0;
		for ( took = 0; i < p->n; i++, took++ ) {
			UD	c = map_cost(p->start[i], p->count[i]);

			if ( took > 0 && (UD)ts_jrnl_room(vol) < used + c + STEP_KEEP ) {
				break;
			}
			used += c;
			ts_free_ext(vol, p->start[i], p->count[i]);
		}
		ts_jrnl_close(vol, NULL);
		GEN_BUMP(vol);
	}
	p->n = 0;
}

/*
 * The copies of the free map and the group heads a change to the run
 * [start, start + count) may add to a transaction: a block of the map
 * and its group's head for each block of the map the run lies in.
 */
LOCAL UD map_cost( UD start, UD count )
{
	if ( count == 0 ) {
		return 0;
	}
	return 2 * ( ( start + count - 1 ) / MAP_SPAN - start / MAP_SPAN + 1 );
}

/* ---------------------------------------------------------------- extents */

typedef struct {
	UD	l;			/* first block within the part */
	UD	p;			/* where it is on the volume */
	UD	n;			/* blocks */
} XT;

/*
 * The runs of a part, in order of their first block: all it has, or
 * those of a stretch of it that a read or a write asked for. One run is
 * kept in the place itself (F_RUN); up to XT_FAN in one leaf block
 * (F_TREE). On a volume of version 4 more lie in leaves of XT_FAN each
 * named by an index block (F_TREE2), up to V4_XT2_MAX runs, and the
 * volume is marked for it (TSFSBLK_INCOMPAT_XT2). On a volume of version
 * 5 more lie in a run tree (F_TREEN) of as many levels as they need: its
 * nodes have the common head, the entries at XL_N and the level at
 * XN_LEVEL, 0 at the leaves; an index entry is the first logical block
 * of a child, the child's block and the blocks below it.
 */
typedef struct {
	XT	*x;
	INT	n, cap;
	INT	dirty;			/* the first run changed since they were read */
	UD	*leaf;			/* two levels: the leaves' blocks, as read */
	INT	nleaf;
} XTS;

LOCAL ER xts_new( XTS *s )
{
	s->cap = XT_FAN + 1;
	s->x = (XT *)Kmalloc(sizeof(XT) * (SZ)s->cap);
	s->n = 0;
	s->dirty = 0;
	s->leaf = NULL;
	s->nleaf = 0;
	return ( s->x == NULL ) ? E_NOMEM : E_OK;
}

LOCAL void xts_free( XTS *s )
{
	if ( s->x != NULL ) Kfree(s->x);
	if ( s->leaf != NULL ) Kfree(s->leaf);
	s->x = NULL;
	s->leaf = NULL;
}

/* Room for `need` runs, the table twice as large each time it fills */
LOCAL ER xts_fit( XTS *s, INT need )
{
	XT	*nx;
	INT	cap = s->cap;

	if ( need <= cap ) {
		return E_OK;
	}
	if ( need < 0 ) {
		return E_NOMEM;
	}
	while ( cap < need ) cap *= 2;
	nx = (XT *)Kmalloc(sizeof(XT) * (SZ)cap);
	if ( nx == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(nx, s->x, sizeof(XT) * (SZ)s->n);
	Kfree(s->x);
	s->x = nx;
	s->cap = cap;
	return E_OK;
}

/* One run put at the end, joined to the last when it follows it on the volume */
LOCAL ER xts_add( XTS *s, UD l, UD p, UD n )
{
	ER	er;

	if ( s->n > 0 && s->x[s->n - 1].p + s->x[s->n - 1].n == p
	  && s->x[s->n - 1].l + s->x[s->n - 1].n == l ) {
		s->x[s->n - 1].n += n;
		if ( s->dirty > s->n - 1 ) s->dirty = s->n - 1;
		return E_OK;
	}
	er = xts_fit(s, s->n + 1);
	if ( er < E_OK ) {
		return er;
	}
	s->x[s->n].l = l;
	s->x[s->n].p = p;
	s->x[s->n].n = n;
	if ( s->dirty > s->n ) s->dirty = s->n;
	s->n++;
	return E_OK;
}

/* ---------------------------------------------------------------- nodes of the trees */

#define NE(b, i)	( (b) + XL_ENT + (i) * XL_ESZ )
#define NE_L(b, i)	rd64(NE(b, i))
#define NE_P(b, i)	rd64(NE(b, i) + 8)
#define NE_N(b, i)	rd64(NE(b, i) + 16)

LOCAL void ne_put( UB *b, INT i, UD l, UD p, UD n )
{
	wr64(NE(b, i), l);
	wr64(NE(b, i) + 8, p);
	wr64(NE(b, i) + 16, n);
}

/* The entries a node of a run tree is filled with on this volume */
LOCAL INT fan_of( ID vol )
{
	INT	f = ( vol >= 1 && vol <= TSFSBLK_MAX_VOL ) ? obj_fan[vol - 1] : 0;

	return ( f >= 2 && f <= XT_FAN ) ? f : XT_FAN;
}

/* A node of a tree read and checked; E_IO when it does not pass */
LOCAL ER node_read( OBJ *o, UD blk, UB *buf )
{
	ER	er = meta_read(o->vol, blk, buf);

	if ( er >= E_OK && ts_blk_check(o->vol, buf, TSFSBLK_MAGIC_TBL, blk) < E_OK ) {
		er = E_IO;
	}
	return er;
}

/*
 * A node written: through the journal when the part is reached already,
 * straight to its block when nothing reaches it before the transaction
 * that makes it reached is real (a block taken in this change, or a tree
 * not yet in any place). The transaction's commit puts what was written
 * straight on the medium first.
 */
LOCAL ER node_write( OBJ *o, UD blk, UB *buf, BOOL fresh )
{
	ts_blk_seal(o->vol, buf, TSFSBLK_MAGIC_TBL, blk, o->owner);
	if ( fresh || o->direct ) {
		return ts_write_blk(o->vol, blk, buf);
	}
	return meta_write(o->vol, blk, buf);
}

/* The runs of one leaf block appended to the table */
LOCAL ER leaf_load( OBJ *o, UD blk, XTS *s, UB *buf )
{
	INT	i, n;
	ER	er;

	er = node_read(o, blk, buf);
	n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
	if ( n > XT_FAN ) {
		return E_OBJ;
	}
	if ( er >= E_OK ) er = xts_fit(s, s->n + n);
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		s->x[s->n].l = NE_L(buf, i);
		s->x[s->n].p = NE_P(buf, i);
		s->x[s->n].n = NE_N(buf, i);
		s->n++;
	}
	return er;
}

/*
 * The runs of the run tree under `blk` that lie in [lo, hi) of the part,
 * appended in order. Only the nodes over that stretch are read. `want`
 * is the level the node has to have, -1 for the root.
 */
LOCAL ER tn_collect( OBJ *o, UD blk, INT want, UD lo, UD hi, XTS *s )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	INT	i, n, lvl;
	ER	er;

	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(o, blk, buf);
	n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
	lvl = ( er >= E_OK ) ? (INT)rd32(buf + XN_LEVEL) : 0;
	if ( er >= E_OK && ( n < 1 || n > XT_FAN || lvl > XT_DEPTH_MAX || ( want >= 0 && lvl != want ) ) ) {
		er = E_OBJ;
	}
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		UD	l = NE_L(buf, i), cnt = NE_N(buf, i);

		if ( l + cnt <= lo || l >= hi ) {
			continue;
		}
		if ( lvl == 0 ) {
			er = xts_fit(s, s->n + 1);
			if ( er >= E_OK ) {
				s->x[s->n].l = l;
				s->x[s->n].p = NE_P(buf, i);
				s->x[s->n].n = cnt;
				s->n++;
			}
		} else {
			er = tn_collect(o, NE_P(buf, i), lvl - 1, lo, hi, s);
		}
	}
	Kfree(buf);
	return er;
}

/*
 * Every block of the run tree under `blk` given back, the runs of its
 * leaves and the nodes themselves, now or once the transaction is real;
 * *p_runs counts the runs.
 */
LOCAL ER pend_add( ID vol, UD start, UD count );

LOCAL ER give( ID vol, UD start, UD count, BOOL later )
{
	if ( count == 0 ) {
		return E_OK;
	}
	return later ? pend_add(vol, start, count) : ts_free_ext(vol, start, count);
}

LOCAL ER tn_free( OBJ *o, UD blk, INT want, BOOL later, BOOL runs, UD *p_runs )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	INT	i, n, lvl;
	ER	er;

	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(o, blk, buf);
	n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
	lvl = ( er >= E_OK ) ? (INT)rd32(buf + XN_LEVEL) : 0;
	if ( er >= E_OK && ( n < 1 || n > XT_FAN || lvl > XT_DEPTH_MAX || ( want >= 0 && lvl != want ) ) ) {
		er = E_OBJ;
	}
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		if ( lvl == 0 ) {
			if ( runs ) er = give(o->vol, NE_P(buf, i), NE_N(buf, i), later);
			if ( p_runs != NULL ) (*p_runs)++;
		} else {
			er = tn_free(o, NE_P(buf, i), lvl - 1, later, runs, p_runs);
		}
	}
	Kfree(buf);
	if ( er >= E_OK ) {
		er = give(o->vol, blk, 1, later);
	}
	return er;
}

/* The runs a part's bytes lie in, in order: all of them */
LOCAL ER xt_load( OBJ *o, CONST UB *pl, XTS *s )
{
	UB	*buf;
	INT	i, nl;
	ER	er = E_OK;

	s->n = 0;
	s->nleaf = 0;
	if ( pl[PL_FORM] == F_RUN ) {
		s->x[0].l = 0;
		s->x[0].p = rd64(pl + PL_A);
		s->x[0].n = rd64(pl + PL_B);
		s->n = ( s->x[0].n > 0 ) ? 1 : 0;
		s->dirty = s->n;
		return E_OK;
	}
	if ( pl[PL_FORM] == F_TREEN ) {
		er = tn_collect(o, rd64(pl + PL_A), -1, 0, ~(UD)0, s);
		if ( er < E_OK ) s->n = 0;
		s->dirty = s->n;
		return er;
	}
	if ( pl[PL_FORM] != F_TREE && pl[PL_FORM] != F_TREE2 ) {
		return E_OK;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	if ( pl[PL_FORM] == F_TREE ) {
		er = leaf_load(o, rd64(pl + PL_A), s, buf);
	} else {
		/* the index: its leaves in order, each read after it */
		UD	*leaf = NULL;

		er = node_read(o, rd64(pl + PL_A), buf);
		nl = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
		if ( nl < 2 || nl > XT_FAN ) {
			er = ( er < E_OK ) ? er : E_OBJ;
			nl = 0;
		}
		if ( er >= E_OK ) {
			leaf = (UD *)Kmalloc(sizeof(UD) * (SZ)nl);
			if ( leaf == NULL ) er = E_NOMEM;
		}
		for ( i = 0; i < nl && er >= E_OK; i++ ) {
			leaf[i] = NE_P(buf, i);
		}
		for ( i = 0; i < nl && er >= E_OK; i++ ) {
			er = leaf_load(o, leaf[i], s, buf);
		}
		if ( er >= E_OK ) {
			if ( s->leaf != NULL ) Kfree(s->leaf);
			s->leaf = leaf;
			s->nleaf = nl;
		} else if ( leaf != NULL ) {
			Kfree(leaf);
		}
	}
	Kfree(buf);
	if ( er < E_OK ) s->n = 0;
	s->dirty = s->n;
	return er;
}

/*
 * The runs over the blocks [lo, hi) of a part: of a run tree, those of
 * the leaves over the stretch only; of the other forms, all of them.
 */
LOCAL ER xt_load_range( OBJ *o, CONST UB *pl, XTS *s, UD lo, UD hi )
{
	ER	er;

	if ( pl[PL_FORM] != F_TREEN ) {
		return xt_load(o, pl, s);
	}
	s->n = 0;
	s->nleaf = 0;
	er = tn_collect(o, rd64(pl + PL_A), -1, lo, hi, s);
	if ( er < E_OK ) s->n = 0;
	s->dirty = s->n;
	return er;
}

/* One leaf written: `n` runs from x */
LOCAL ER leaf_store( OBJ *o, UD blk, CONST XT *x, INT n, UB *buf, BOOL fresh )
{
	INT	i;

	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	wr32(buf + XL_N, (UW)n);
	for ( i = 0; i < n; i++ ) {
		ne_put(buf, i, x[i].l, x[i].p, x[i].n);
	}
	return node_write(o, blk, buf, fresh);
}

/* The blocks of the tree the place has now, given back (leaves, index, nodes) */
LOCAL ER tree_free( OBJ *o, CONST UB *pl, CONST XTS *s, BOOL later )
{
	INT	i;
	ER	er = E_OK;

	if ( pl[PL_FORM] == F_TREE ) {
		er = give(o->vol, rd64(pl + PL_A), 1, later);
	} else if ( pl[PL_FORM] == F_TREE2 ) {
		for ( i = 0; i < s->nleaf && er >= E_OK; i++ ) {
			er = give(o->vol, s->leaf[i], 1, later);
		}
		if ( er >= E_OK ) {
			er = give(o->vol, rd64(pl + PL_A), 1, later);
		}
	} else if ( pl[PL_FORM] == F_TREEN ) {
		er = tn_free(o, rd64(pl + PL_A), -1, later, FALSE, NULL);
	}
	return er;
}

LOCAL UD xt_blocks( CONST XT *x, INT n )
{
	return ( n > 0 ) ? x[n - 1].l + x[n - 1].n : 0;
}

/* ---------------------------------------------------------------- the right edge of a run tree */

/*
 * A run tree is changed at its right edge only: a part grows at its end
 * and is cut from its end. What is held is the path from the root to
 * the last leaf -- the index nodes nb[1..depth] -- and the runs of that
 * leaf (the tail); the leaves before it are not read. Storing writes the
 * tail into its leaf and into new leaves behind it, and the path, a new
 * root on top when the old one fills.
 */
typedef struct {
	OBJ	*o;
	INT	fan;
	INT	depth;			/* the root's level; 0 when the tail's leaf is the root */
	UB	*nb[XT_DEPTH_MAX + 2];
	UD	nblk[XT_DEPTH_MAX + 2];
	BOOL	nnew[XT_DEPTH_MAX + 2];
	UD	tail_blk;		/* the last leaf, 0 for none yet */
	BOOL	tail_new;
	UD	tail_l;			/* the first block of the part the tail holds */
	UD	nbefore;		/* runs in the leaves before it */
	XTS	tail;
} TEDIT;

LOCAL ER te_init( TEDIT *t, OBJ *o )
{
	knl_memset(t, 0, sizeof(*t));
	t->o = o;
	t->fan = fan_of(o->vol);
	return xts_new(&t->tail);
}

LOCAL void te_done( TEDIT *t )
{
	INT	i;

	for ( i = 0; i <= XT_DEPTH_MAX + 1; i++ ) {
		if ( t->nb[i] != NULL ) Kfree(t->nb[i]);
		t->nb[i] = NULL;
	}
	xts_free(&t->tail);
}

LOCAL ER take_blk( OBJ *o, UD *p_blk )
{
	UD	got = 0;

	return ts_alloc_ext(o->vol, 1, p_blk, &got);
}

/* A node of `lvl` made in memory, empty */
LOCAL UB *nb_new( INT lvl )
{
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);

	if ( b != NULL ) {
		knl_memset(b, 0, TSFSBLK_BLOCK_SIZE);
		wr32(b + XN_LEVEL, (UW)lvl);
	}
	return b;
}

/* The path to the last leaf of a run tree, and that leaf's runs */
LOCAL ER te_open_treen( TEDIT *t, CONST UB *pl )
{
	UD	cur = rd64(pl + PL_A), total = rd64(pl + PL_B);
	INT	want = -1, n, lvl, guard;
	UB	*buf;
	ER	er = E_OK;

	for ( guard = 0; guard <= XT_DEPTH_MAX + 1; guard++ ) {
		buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( buf == NULL ) {
			return E_NOMEM;
		}
		er = node_read(t->o, cur, buf);
		n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
		lvl = ( er >= E_OK ) ? (INT)rd32(buf + XN_LEVEL) : 0;
		if ( er >= E_OK && ( n < 1 || n > XT_FAN || lvl > XT_DEPTH_MAX || ( want >= 0 && lvl != want ) ) ) {
			er = E_OBJ;
		}
		if ( er < E_OK ) {
			Kfree(buf);
			return er;
		}
		if ( want < 0 ) {
			t->depth = lvl;
		}
		if ( lvl == 0 ) {
			INT	i;

			t->tail_blk = cur;
			er = xts_fit(&t->tail, n);
			for ( i = 0; i < n && er >= E_OK; i++ ) {
				t->tail.x[i].l = NE_L(buf, i);
				t->tail.x[i].p = NE_P(buf, i);
				t->tail.x[i].n = NE_N(buf, i);
			}
			t->tail.n = ( er >= E_OK ) ? n : 0;
			t->tail.dirty = t->tail.n;
			Kfree(buf);
			break;
		}
		t->nb[lvl] = buf;
		t->nblk[lvl] = cur;
		t->tail_l = NE_L(buf, n - 1);
		cur = NE_P(buf, n - 1);
		want = lvl - 1;
	}
	if ( er >= E_OK && ( t->tail_blk == 0 || total < (UD)t->tail.n ) ) {
		er = E_OBJ;
	}
	t->nbefore = ( er >= E_OK ) ? total - (UD)t->tail.n : 0;
	return er;
}

/*
 * The right edge of any form of place. A place of another form becomes
 * the tail whole: one leaf (F_TREE) is kept as its block, a tree of two
 * levels has its index and leaves given back and is written again as a
 * run tree.
 */
LOCAL ER te_open( TEDIT *t, CONST UB *pl )
{
	ER	er;

	switch ( pl[PL_FORM] ) {
	case F_TREEN:
		return te_open_treen(t, pl);
	case F_TREE:
		er = xt_load(t->o, pl, &t->tail);
		if ( er >= E_OK ) t->tail_blk = rd64(pl + PL_A);
		return er;
	case F_TREE2:
		er = xt_load(t->o, pl, &t->tail);
		if ( er >= E_OK ) er = tree_free(t->o, pl, &t->tail, FALSE);
		return er;
	case F_RUN:
		return xt_load(t->o, pl, &t->tail);
	default:
		return E_OK;
	}
}

/*
 * A child put at the end of the node of `lvl` on the path: the entry
 * before it ends where it starts. A full node leaves the path as it is,
 * written, and a new one takes its place under its parent; a full root
 * gets a new root over it.
 */
LOCAL ER te_append( TEDIT *t, INT lvl, UD first_l, UD child )
{
	UB	*b, *nbuf;
	UD	blk = 0, old;
	INT	n;
	ER	er;

	if ( lvl > t->depth ) {
		if ( lvl > XT_DEPTH_MAX ) {
			return E_LIMIT;
		}
		old = ( t->depth == 0 ) ? t->tail_blk : t->nblk[t->depth];
		nbuf = nb_new(lvl);
		if ( nbuf == NULL ) {
			return E_NOMEM;
		}
		er = take_blk(t->o, &blk);
		if ( er < E_OK ) {
			Kfree(nbuf);
			return er;
		}
		ne_put(nbuf, 0, 0, old, first_l);
		ne_put(nbuf, 1, first_l, child, 0);
		wr32(nbuf + XL_N, 2);
		t->depth = lvl;
		t->nb[lvl] = nbuf;
		t->nblk[lvl] = blk;
		t->nnew[lvl] = TRUE;
		return E_OK;
	}
	b = t->nb[lvl];
	n = (INT)rd32(b + XL_N);
	if ( n > 0 ) {
		ne_put(b, n - 1, NE_L(b, n - 1), NE_P(b, n - 1), first_l - NE_L(b, n - 1));
	}
	if ( n < t->fan ) {
		ne_put(b, n, first_l, child, 0);
		wr32(b + XL_N, (UW)( n + 1 ));
		return E_OK;
	}
	/* full: it leaves the path */
	er = node_write(t->o, t->nblk[lvl], b, t->nnew[lvl]);
	nbuf = ( er >= E_OK ) ? nb_new(lvl) : NULL;
	if ( er >= E_OK && nbuf == NULL ) er = E_NOMEM;
	if ( er >= E_OK ) er = take_blk(t->o, &blk);
	if ( er >= E_OK ) {
		ne_put(nbuf, 0, first_l, child, 0);
		wr32(nbuf + XL_N, 1);
		er = te_append(t, lvl + 1, first_l, blk);
	}
	if ( er < E_OK ) {
		if ( nbuf != NULL ) Kfree(nbuf);
		return er;
	}
	Kfree(b);
	t->nb[lvl] = nbuf;
	t->nblk[lvl] = blk;
	t->nnew[lvl] = TRUE;
	return E_OK;
}

/*
 * The tail written back, and the place made to say where the tree is:
 * one run or none in the place itself, one leaf as F_TREE, more as a run
 * tree.
 */
LOCAL ER te_store( TEDIT *t, UB *pl )
{
	XTS	*s = &t->tail;
	UB	*buf;
	UD	total = t->nbefore + (UD)s->n, blk, end;
	INT	k, c, lvl;
	ER	er = E_OK;

	if ( t->depth == 0 && total <= 1 ) {
		if ( t->tail_blk != 0 ) er = ts_free_ext(t->o->vol, t->tail_blk, 1);
		pl[PL_FORM] = ( s->n == 1 ) ? F_RUN : F_NONE;
		wr64(pl + PL_A, ( s->n == 1 ) ? s->x[0].p : 0);
		wr64(pl + PL_B, ( s->n == 1 ) ? s->x[0].n : 0);
		t->tail_blk = 0;
		return er;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	for ( k = 0; k < s->n && er >= E_OK; k += c ) {
		c = ( s->n - k > t->fan ) ? t->fan : s->n - k;
		if ( k == 0 ) {
			if ( t->tail_blk == 0 ) {
				er = take_blk(t->o, &t->tail_blk);
				t->tail_new = TRUE;
			}
			if ( er >= E_OK ) er = leaf_store(t->o, t->tail_blk, s->x, c, buf, t->tail_new);
			if ( er >= E_OK && t->depth >= 1 ) {
				UB	*b1 = t->nb[1];
				INT	n1 = (INT)rd32(b1 + XL_N);

				ne_put(b1, n1 - 1, s->x[0].l, t->tail_blk, NE_N(b1, n1 - 1));
			}
		} else {
			blk = 0;
			er = take_blk(t->o, &blk);
			if ( er >= E_OK ) er = leaf_store(t->o, blk, s->x + k, c, buf, TRUE);
			if ( er >= E_OK ) er = te_append(t, 1, s->x[k].l, blk);
		}
	}
	/* the path's last entries reach to the end of the part */
	end = xt_blocks(s->x, s->n);
	for ( lvl = 1; lvl <= t->depth && er >= E_OK; lvl++ ) {
		UB	*b = t->nb[lvl];
		INT	n = (INT)rd32(b + XL_N);

		ne_put(b, n - 1, NE_L(b, n - 1), NE_P(b, n - 1), end - NE_L(b, n - 1));
		knl_memcpy(buf, b, TSFSBLK_BLOCK_SIZE);
		er = node_write(t->o, t->nblk[lvl], buf, t->nnew[lvl]);
	}
	Kfree(buf);
	if ( er >= E_OK ) {
		if ( t->depth == 0 ) {
			pl[PL_FORM] = F_TREE;
			wr64(pl + PL_A, t->tail_blk);
			wr64(pl + PL_B, (UD)s->n);
		} else {
			pl[PL_FORM] = F_TREEN;
			wr64(pl + PL_A, t->nblk[t->depth]);
			wr64(pl + PL_B, total);
		}
		s->dirty = s->n;
	}
	return er;
}

/*
 * A run tree cut to its first `keep` blocks (1 at least): the path to
 * the leaf holding the last block kept is read, everything right of it
 * given back, that leaf made the tail and cut, and a root with one child
 * taken away, as often as there is one. *p_runs counts the runs given
 * back with the subtrees.
 */
LOCAL ER xt_shrink( OBJ *o, XTS *s, UD keep, BOOL later );

LOCAL ER te_cut( TEDIT *t, UB *pl, UD keep )
{
	UD	cur = rd64(pl + PL_A), total = rd64(pl + PL_B), freed = 0, target = keep - 1;
	INT	want = -1, n, lvl, c, i, guard;
	UB	*buf;
	ER	er = E_OK;

	for ( guard = 0; guard <= XT_DEPTH_MAX + 1; guard++ ) {
		buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( buf == NULL ) {
			return E_NOMEM;
		}
		er = node_read(t->o, cur, buf);
		n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
		lvl = ( er >= E_OK ) ? (INT)rd32(buf + XN_LEVEL) : 0;
		if ( er >= E_OK && ( n < 1 || n > XT_FAN || lvl > XT_DEPTH_MAX || ( want >= 0 && lvl != want ) ) ) {
			er = E_OBJ;
		}
		if ( er < E_OK ) {
			Kfree(buf);
			return er;
		}
		if ( want < 0 ) {
			t->depth = lvl;
		}
		if ( lvl == 0 ) {
			t->tail_blk = cur;
			er = xts_fit(&t->tail, n);
			for ( i = 0; i < n && er >= E_OK; i++ ) {
				t->tail.x[i].l = NE_L(buf, i);
				t->tail.x[i].p = NE_P(buf, i);
				t->tail.x[i].n = NE_N(buf, i);
			}
			t->tail.n = ( er >= E_OK ) ? n : 0;
			t->tail.dirty = t->tail.n;
			Kfree(buf);
			break;
		}
		for ( c = n - 1; c > 0 && NE_L(buf, c) > target; c-- ) ;
		for ( i = c + 1; i < n && er >= E_OK; i++ ) {
			er = tn_free(t->o, NE_P(buf, i), lvl - 1, FALSE, TRUE, &freed);
		}
		wr32(buf + XL_N, (UW)( c + 1 ));
		t->nb[lvl] = buf;
		t->nblk[lvl] = cur;
		t->tail_l = NE_L(buf, c);
		cur = NE_P(buf, c);
		want = lvl - 1;
		if ( er < E_OK ) {
			return er;
		}
	}
	if ( t->tail_blk == 0 || total < freed + (UD)t->tail.n ) {
		return E_OBJ;
	}
	t->nbefore = total - freed - (UD)t->tail.n;
	er = xt_shrink(t->o, &t->tail, keep, FALSE);
	/* a root with one child is no root */
	while ( er >= E_OK && t->depth >= 1 && rd32(t->nb[t->depth] + XL_N) == 1 ) {
		er = ts_free_ext(t->o->vol, t->nblk[t->depth], 1);
		Kfree(t->nb[t->depth]);
		t->nb[t->depth] = NULL;
		t->depth--;
	}
	if ( er >= E_OK && t->depth == 0 ) {
		t->nbefore = 0;
		t->tail_l = 0;
	}
	if ( er >= E_OK && t->tail.n == 0 ) {
		er = E_OBJ;			/* the leaf of the last block kept holds no run */
	}
	if ( er >= E_OK ) er = te_store(t, pl);
	return er;
}

/* ---------------------------------------------------------------- writing the runs back */

/*
 * The runs written back, the form of version 4: one run in the place
 * itself, up to a leaf's worth in one leaf, more in leaves under an
 * index. The blocks of the tree are taken when the part first needs them
 * and given back when it no longer does. Of a tree of two levels, only
 * the leaves from the one holding the first changed run on are written
 * again, so that a part that grows at its end changes its last leaf and
 * the index.
 */
LOCAL ER xt_store4( OBJ *o, UB *pl, XTS *s )
{
	UB	*buf;
	UD	got = 0, idx = 0, blk;
	INT	n = s->n, nl, i, from;
	ER	er = E_OK;

	if ( n <= 1 ) {
		er = tree_free(o, pl, s, FALSE);
		pl[PL_FORM] = ( n == 1 ) ? F_RUN : F_NONE;
		wr64(pl + PL_A, ( n == 1 ) ? s->x[0].p : 0);
		wr64(pl + PL_B, ( n == 1 ) ? s->x[0].n : 0);
		s->nleaf = 0;
		return er;
	}
	if ( (UD)n > V4_XT2_MAX ) {
		return E_LIMIT;			/* the part is broken into more runs than the form holds */
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}

	if ( n <= TSFSO_MAX_EXTENTS ) {
		/* one leaf: the tree's first leaf kept, the rest given back */
		if ( pl[PL_FORM] == F_TREE ) {
			blk = rd64(pl + PL_A);
		} else if ( pl[PL_FORM] == F_TREE2 && s->nleaf > 0 ) {
			blk = s->leaf[0];
			for ( i = 1; i < s->nleaf && er >= E_OK; i++ ) er = ts_free_ext(o->vol, s->leaf[i], 1);
			if ( er >= E_OK ) er = ts_free_ext(o->vol, rd64(pl + PL_A), 1);
		} else {
			er = ts_alloc_ext(o->vol, 1, &blk, &got);
		}
		if ( er >= E_OK ) er = leaf_store(o, blk, s->x, n, buf, FALSE);
		Kfree(buf);
		if ( er >= E_OK ) {
			pl[PL_FORM] = F_TREE;
			wr64(pl + PL_A, blk);
			wr64(pl + PL_B, (UD)n);
			s->nleaf = 0;
			s->dirty = n;
		}
		return er;
	}

	/* two levels: the leaves it has kept, more taken or the last given back */
	nl = ( n + TSFSO_MAX_EXTENTS - 1 ) / TSFSO_MAX_EXTENTS;
	from = s->dirty / TSFSO_MAX_EXTENTS;
	if ( pl[PL_FORM] != F_TREE2 ) {
		UD	*leaf = (UD *)Kmalloc(sizeof(UD) * (SZ)TSFSO_MAX_EXTENTS);

		if ( leaf == NULL ) {
			Kfree(buf);
			return E_NOMEM;
		}
		if ( s->leaf != NULL ) Kfree(s->leaf);
		s->leaf = leaf;
		s->nleaf = 0;
		if ( pl[PL_FORM] == F_TREE ) {
			s->leaf[s->nleaf++] = rd64(pl + PL_A);	/* the leaf it had is the first */
		}
		er = ts_alloc_ext(o->vol, 1, &idx, &got);
		from = 0;
		if ( er >= E_OK ) er = ts_blk_xt2(o->vol);	/* the volume says it has the form */
	} else {
		UD	*leaf = (UD *)Kmalloc(sizeof(UD) * (SZ)TSFSO_MAX_EXTENTS);

		idx = rd64(pl + PL_A);
		if ( leaf == NULL ) {
			Kfree(buf);
			return E_NOMEM;
		}
		knl_memcpy(leaf, s->leaf, sizeof(UD) * (SZ)s->nleaf);
		Kfree(s->leaf);
		s->leaf = leaf;
	}
	while ( er >= E_OK && s->nleaf < nl ) {
		if ( from > s->nleaf ) from = s->nleaf;
		er = ts_alloc_ext(o->vol, 1, &s->leaf[s->nleaf], &got);
		if ( er >= E_OK ) s->nleaf++;
	}
	while ( er >= E_OK && s->nleaf > nl ) {
		er = ts_free_ext(o->vol, s->leaf[--s->nleaf], 1);
	}
	for ( i = from; i < nl && er >= E_OK; i++ ) {
		INT	k = i * TSFSO_MAX_EXTENTS;
		INT	m = ( n - k > TSFSO_MAX_EXTENTS ) ? TSFSO_MAX_EXTENTS : n - k;

		er = leaf_store(o, s->leaf[i], s->x + k, m, buf, FALSE);
	}
	if ( er >= E_OK ) {
		/* the index: each leaf, where its runs start and how many it holds */
		knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
		wr32(buf + XL_N, (UW)nl);
		for ( i = 0; i < nl; i++ ) {
			INT	k = i * TSFSO_MAX_EXTENTS;

			ne_put(buf, i, s->x[k].l, s->leaf[i],
			       (UD)( ( n - k > TSFSO_MAX_EXTENTS ) ? TSFSO_MAX_EXTENTS : n - k ));
		}
		er = node_write(o, idx, buf, FALSE);
	}
	Kfree(buf);
	if ( er >= E_OK ) {
		pl[PL_FORM] = F_TREE2;
		wr64(pl + PL_A, idx);
		wr64(pl + PL_B, (UD)n);
		s->dirty = n;
	}
	return er;
}

/*
 * All the runs of a part written back. A volume of version 4 keeps its
 * forms. On one of version 5 a part of more runs than a node holds is a
 * run tree; one of the forms before (a leaf, a tree of two levels) is
 * written again as one, and a run tree written whole is made new.
 */
LOCAL ER xt_store( OBJ *o, UB *pl, XTS *s )
{
	TEDIT	t;
	INT	i;
	ER	er;

	if ( !o->v5 ) {
		return xt_store4(o, pl, s);
	}
	if ( pl[PL_FORM] != F_TREEN && ( s->n <= 1 || s->n <= fan_of(o->vol) ) ) {
		return xt_store4(o, pl, s);
	}
	er = te_init(&t, o);
	if ( er >= E_OK ) {
		if ( pl[PL_FORM] == F_TREE ) {
			t.tail_blk = rd64(pl + PL_A);
		} else if ( pl[PL_FORM] == F_TREE2 || pl[PL_FORM] == F_TREEN ) {
			er = tree_free(o, pl, s, FALSE);
		}
	}
	if ( er >= E_OK ) er = xts_fit(&t.tail, s->n);
	if ( er >= E_OK ) {
		for ( i = 0; i < s->n; i++ ) t.tail.x[i] = s->x[i];
		t.tail.n = s->n;
		er = te_store(&t, pl);
	}
	if ( er >= E_OK ) {
		s->nleaf = 0;
		s->dirty = s->n;
	}
	te_done(&t);
	return er;
}

/* Where block `l` of the part is, and how many follow it in the same run */
LOCAL UD xt_map( CONST XT *x, INT n, UD l, UD *p_run )
{
	INT	lo = 0, hi = n - 1, i;

	/* the runs are in order of their first block: halved to it */
	while ( lo <= hi ) {
		i = ( lo + hi ) / 2;
		if ( l < x[i].l ) {
			hi = i - 1;
		} else if ( l >= x[i].l + x[i].n ) {
			lo = i + 1;
		} else {
			if ( p_run != NULL ) *p_run = x[i].l + x[i].n - l;
			return x[i].p + ( l - x[i].l );
		}
	}
	return 0;
}

LOCAL UD run_io_max( OBJ *o );

/*
 * `cnt` blocks at `p` on the volume made zeros: as many together in one
 * request as the device takes at once, from a buffer of zeros made the
 * first time it is needed and made again larger for a longer run
 * (*p_zero, *p_zn blocks of it; Kfree *p_raw).
 */
LOCAL ER zero_run( OBJ *o, UD p, UD cnt, UB **p_zero, void **p_raw, UD *p_zn )
{
	UD	k, zn = run_io_max(o);
	ER	er = E_OK;

	if ( zn > cnt ) zn = cnt;
	if ( *p_zero != NULL && *p_zn < zn ) {
		Kfree(*p_raw);			/* a longer run than the last: a larger buffer */
		*p_zero = NULL;
	}
	if ( *p_zero == NULL ) {
		*p_raw = Kmalloc((SZ)( zn * TSFSBLK_BLOCK_SIZE ) + 64);
		if ( *p_raw == NULL ) return E_NOMEM;
		*p_zero = (UB *)( ( (UBINT)*p_raw + 63 ) & ~(UBINT)63 );
		knl_memset(*p_zero, 0, (SZ)( zn * TSFSBLK_BLOCK_SIZE ));
		*p_zn = zn;
	}
	while ( cnt > 0 && er >= E_OK ) {
		k = ( cnt < *p_zn ) ? cnt : *p_zn;
		er = ( k == 1 ) ? ts_write_blk(o->vol, p, *p_zero) : ts_write_blks(o->vol, p, k, *p_zero);
		p += k;
		cnt -= k;
	}
	return er;
}

/*
 * Blocks added until the part has `need`: right behind its last run
 * when that is free, which keeps it one run, else wherever a run is.
 * The blocks added are cleared unless the caller is about to fill them:
 * [keep_from, keep_to) is what it will write whole.
 */
LOCAL ER xt_grow( OBJ *o, XTS *s, UD need, UD keep_from, UD keep_to )
{
	UB	*zero = NULL;
	void	*zraw = NULL;
	UD	have = xt_blocks(s->x, s->n), start = 0, got = 0, zn = 0, a, e;
	INT	n = s->n;
	ER	er = E_OK;

	while ( have < need && er >= E_OK ) {
		er = E_NOMEM;
		if ( n > 0 ) {
			er = ts_alloc_ext_at(o->vol, s->x[n - 1].p + s->x[n - 1].n, need - have, &got);
			if ( er >= E_OK ) {
				start = s->x[n - 1].p + s->x[n - 1].n;
				s->x[n - 1].n += got;
				if ( s->dirty > n - 1 ) s->dirty = n - 1;
			}
		}
		if ( er < E_OK ) {
			if ( !o->v5 && (UD)n + 1 > V4_XT2_MAX ) {
				er = E_LIMIT;		/* more runs than the form of version 4 holds */
				break;
			}
			if ( xts_fit(s, n + 1) < E_OK ) {
				er = E_NOMEM;
				break;
			}
			er = ts_alloc_ext(o->vol, need - have, &start, &got);
			if ( er < E_OK ) break;
			s->x[n].l = have;
			s->x[n].p = start;
			s->x[n].n = got;
			if ( s->dirty > n ) s->dirty = n;
			n++;
			s->n = n;
		}
		/* of [have, have + got), what lies before keep_from and after
		   keep_to is cleared; the caller writes the rest whole */
		a = ( keep_from > have ) ? keep_from - have : 0;
		if ( a > got ) a = got;
		if ( a > 0 && er >= E_OK ) er = zero_run(o, start, a, &zero, &zraw, &zn);
		e = ( keep_to > have ) ? keep_to - have : 0;
		if ( e < a ) e = a;
		if ( e < got && er >= E_OK ) er = zero_run(o, start + e, got - e, &zero, &zraw, &zn);
		have += got;
	}
	if ( zraw != NULL ) Kfree(zraw);
	s->n = n;
	return er;
}

/* Blocks past `keep` given back, now or once the transaction is real */
LOCAL ER xt_shrink( OBJ *o, XTS *s, UD keep, BOOL later )
{
	INT	n = s->n;
	UD	cut;
	ER	er = E_OK;

	while ( n > 0 && xt_blocks(s->x, n) > keep && er >= E_OK ) {
		XT	*t = &s->x[n - 1];

		cut = ( keep > t->l ) ? keep - t->l : 0;
		er = give(o->vol, t->p + cut, t->n - cut, later);
		if ( cut == 0 ) {
			n--;
		} else {
			t->n = cut;
		}
		if ( s->dirty > ( ( cut == 0 ) ? n : n - 1 ) ) s->dirty = ( cut == 0 ) ? n : n - 1;
	}
	s->n = n;
	return er;
}

/* How many blocks the runs of a part cover (more than its bytes need while it is cut a step at a time) */
LOCAL ER place_have( OBJ *o, CONST UB *pl, UD *p_have )
{
	UB	*buf;
	XTS	xs;
	INT	n;
	ER	er;

	*p_have = 0;
	switch ( pl[PL_FORM] ) {
	case F_RUN:
		*p_have = rd64(pl + PL_B);
		return E_OK;
	case F_TREEN:
		buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( buf == NULL ) return E_NOMEM;
		er = node_read(o, rd64(pl + PL_A), buf);
		n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
		if ( er >= E_OK && ( n < 1 || n > XT_FAN ) ) er = E_OBJ;
		if ( er >= E_OK ) *p_have = NE_L(buf, n - 1) + NE_N(buf, n - 1);
		Kfree(buf);
		return er;
	case F_TREE:
	case F_TREE2:
		er = xts_new(&xs);
		if ( er >= E_OK ) er = xt_load(o, pl, &xs);
		if ( er >= E_OK ) *p_have = xt_blocks(xs.x, xs.n);
		xts_free(&xs);
		return er;
	default:
		return E_OK;
	}
}

/* Whether giving a part back is more than one change should hold: a tree, or a run over many blocks of the free map */
LOCAL BOOL place_big( OBJ *o, CONST UB *pl )
{
	switch ( pl[PL_FORM] ) {
	case F_TREE:
	case F_TREE2:
	case F_TREEN:
		return TRUE;
	case F_RUN:
		return (BOOL)( map_cost(rd64(pl + PL_A), rd64(pl + PL_B)) > big_blocks(o->vol) );
	default:
		return FALSE;
	}
}

/* ---------------------------------------------------------------- data parts */

/*
 * Whole blocks of a data part go to and from the device this many at a
 * time, unless the device says how much it takes best (TDN_IOMAX; the SD
 * card's 1280KB, design 7.3), up to RUN_IO_CAP. The buffer they go
 * through starts on a 64-byte line, so that a controller that moves the
 * data itself takes it without a copy through its own bounce buffer.
 */
#define RUN_IO_BLOCKS	32
#define RUN_IO_CAP	1024		/* 4MB */
#define RUN_IO_ALIGN	64

LOCAL UD run_io_max( OBJ *o )
{
	UD	n = ts_run_blocks(o->vol);

	if ( n == 0 ) return RUN_IO_BLOCKS;
	if ( n < 2 ) return 2;
	return ( n > RUN_IO_CAP ) ? RUN_IO_CAP : n;
}

/* A buffer of 'room' bytes on a 64-byte line; *p_raw is what Kfree takes */
LOCAL UB *run_io_buf( SZ room, void **p_raw )
{
	UB	*raw = (UB *)Kmalloc(room + RUN_IO_ALIGN);

	*p_raw = raw;
	return ( raw == NULL ) ? NULL : (UB *)( ( (UBINT)raw + RUN_IO_ALIGN - 1 ) & ~(UBINT)( RUN_IO_ALIGN - 1 ) );
}


/* The room for the blocks of one request: as many as `len` bytes can fill */
LOCAL SZ run_io_room( SZ len, UD max )
{
	UD	n = (UD)len / TSFSBLK_BLOCK_SIZE;

	if ( n > max ) n = max;
	return (SZ)( ( ( n >= 2 ) ? n : 1 ) * TSFSBLK_BLOCK_SIZE );
}

/*
 * How many whole blocks from `at` can go in one request: the part of the
 * run left, of the bytes asked for, and of the room there is for them.
 */
LOCAL UD run_io_n( UD at, SZ left, UD run, UD max )
{
	UD	n = (UD)left / TSFSBLK_BLOCK_SIZE;

	if ( at % TSFSBLK_BLOCK_SIZE != 0 ) return 0;
	if ( n > run ) n = run;
	if ( n > max ) n = max;
	return ( n >= 2 ) ? n : 0;
}

/*
 * `len` bytes from `off` of a part whose blocks lie in the runs x[0..n),
 * already cut to what the part holds: whole blocks that lie together go
 * in one request each, as many as the device takes at once.
 */
LOCAL ER runs_read( ID vol, CONST XT *x, INT n, UD off, UB *dst, SZ len, SZ *p_asize )
{
	UD	l, p, run, rmax = ts_run_blocks(vol);
	UB	*buf;
	void	*raw = NULL;
	SZ	done = 0, k;
	ER	er = E_OK;

	if ( rmax == 0 ) rmax = RUN_IO_BLOCKS;
	if ( rmax < 2 ) rmax = 2;
	if ( rmax > RUN_IO_CAP ) rmax = RUN_IO_CAP;
	buf = run_io_buf(run_io_room(len, rmax), &raw);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	while ( er >= E_OK && done < len ) {
		UD	nb;

		l = ( off + (UD)done ) / TSFSBLK_BLOCK_SIZE;
		p = xt_map(x, n, l, &run);
		if ( p == 0 ) {
			er = E_OBJ;
			break;
		}
		nb = run_io_n(off + (UD)done, len - done, run, rmax);
		if ( nb > 0 ) {
			er = ts_read_blks(vol, p, nb, buf);
			if ( er < E_OK ) break;
			knl_memcpy(dst + done, buf, (SZ)( nb * TSFSBLK_BLOCK_SIZE ));
			done += (SZ)( nb * TSFSBLK_BLOCK_SIZE );
			continue;
		}
		er = ts_read_blk(vol, p, buf);
		if ( er < E_OK ) break;
		k = TSFSBLK_BLOCK_SIZE - (SZ)( ( off + (UD)done ) % TSFSBLK_BLOCK_SIZE );
		if ( k > len - done ) k = len - done;
		knl_memcpy(dst + done, buf + ( off + (UD)done ) % TSFSBLK_BLOCK_SIZE, k);
		done += k;
	}
	Kfree(raw);
	if ( er >= E_OK ) *p_asize = done;
	return er;
}

/*
 * `len` bytes written at `off` into blocks lying in the runs x[0..n):
 * whole blocks that lie together in one request each; a block written in
 * part is read first, unless `clear`, when what the write does not cover
 * is made zeros (a block no one has written yet).
 */
LOCAL ER runs_write( OBJ *o, CONST XT *x, INT n, UD off, CONST UB *src, SZ len, BOOL clear )
{
	UD	rmax = run_io_max(o), l, p;
	UB	*buf;
	void	*raw = NULL;
	SZ	done = 0, k;
	ER	er = E_OK;

	buf = run_io_buf(run_io_room(len, rmax), &raw);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	while ( er >= E_OK && done < len ) {
		UD	at = off + (UD)done, run = 0, nb;

		l = at / TSFSBLK_BLOCK_SIZE;
		p = xt_map(x, n, l, &run);
		if ( p == 0 ) {
			er = E_OBJ;
			break;
		}
		nb = run_io_n(at, len - done, run, rmax);
		if ( nb > 0 ) {
			knl_memcpy(buf, src + done, (INT)( nb * TSFSBLK_BLOCK_SIZE ));
			er = ts_write_blks(o->vol, p, nb, buf);
			done += (SZ)( nb * TSFSBLK_BLOCK_SIZE );
			continue;
		}
		k = TSFSBLK_BLOCK_SIZE - (SZ)( at % TSFSBLK_BLOCK_SIZE );
		if ( k > len - done ) k = len - done;
		if ( k < TSFSBLK_BLOCK_SIZE ) {
			if ( clear ) {
				knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
			} else {
				er = ts_read_blk(o->vol, p, buf);
				if ( er < E_OK ) break;
			}
		}
		knl_memcpy(buf + at % TSFSBLK_BLOCK_SIZE, src + done, (INT)k);
		er = ts_write_blk(o->vol, p, buf);
		done += k;
	}
	Kfree(raw);
	return er;
}

LOCAL ER dseg_read( OBJ *o, CONST UB *pl, UD off, UB *dst, SZ len, SZ *p_asize )
{
	UD	size = rd64(pl + PL_BYTES);
	XTS	xs;
	ER	er;

	*p_asize = 0;
	if ( off >= size || len <= 0 ) {
		return E_OK;
	}
	if ( (UD)len > size - off ) len = (SZ)( size - off );
	if ( pl[PL_FORM] == F_INLINE ) {
		knl_memcpy(dst, o->hdr + rd16(pl + PL_A) + off, len);
		*p_asize = len;
		return E_OK;
	}
	er = xts_new(&xs);
	if ( er >= E_OK ) er = xt_load_range(o, pl, &xs, off / TSFSBLK_BLOCK_SIZE,
					     BLKS_FOR(off + (UD)len, TSFSBLK_BLOCK_SIZE));
	if ( er >= E_OK ) er = runs_read(o->vol, xs.x, xs.n, off, dst, len, p_asize);
	xts_free(&xs);
	return er;
}

/*
 * Bytes out of the inline area into blocks: the part is about to grow
 * past what it may hold there.
 */
LOCAL ER dseg_unline( OBJ *o, UB *pl )
{
	UB	*tmp;
	UD	size = rd64(pl + PL_BYTES), start = 0, got = 0;
	ER	er;

	if ( pl[PL_FORM] != F_INLINE ) {
		return E_OK;
	}
	tmp = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( tmp == NULL ) {
		return E_NOMEM;
	}
	knl_memset(tmp, 0, TSFSBLK_BLOCK_SIZE);
	knl_memcpy(tmp, o->hdr + rd16(pl + PL_A), (INT)size);
	er = ts_alloc_ext(o->vol, 1, &start, &got);
	if ( er >= E_OK ) {
		er = ts_write_blk(o->vol, start, tmp);
		if ( er < E_OK ) ts_free_ext(o->vol, start, 1);
	}
	Kfree(tmp);
	if ( er < E_OK ) {
		return er;
	}
	inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
	pl[PL_FORM] = F_RUN;
	wr64(pl + PL_A, start);
	wr64(pl + PL_B, 1);
	return E_OK;
}

/*
 * A part of blocks grown to `need` blocks, [kf, kt) of them left for the
 * caller to write whole. A run tree changes at its right edge only; the
 * other forms are read whole and written back.
 */
LOCAL ER dseg_grow( OBJ *o, UB *pl, UD need, UD kf, UD kt )
{
	TEDIT	t;
	XTS	xs;
	ER	er;

	if ( pl[PL_FORM] == F_TREEN ) {
		er = te_init(&t, o);
		if ( er >= E_OK ) er = te_open(&t, pl);
		if ( er >= E_OK && need > xt_blocks(t.tail.x, t.tail.n) ) {
			er = xt_grow(o, &t.tail, need, kf, kt);
			if ( er >= E_OK ) er = te_store(&t, pl);
		}
		te_done(&t);
		return er;
	}
	er = xts_new(&xs);
	if ( er >= E_OK ) er = xt_load(o, pl, &xs);
	if ( er >= E_OK && need > xt_blocks(xs.x, xs.n) ) {
		er = xt_grow(o, &xs, need, kf, kt);
		if ( er >= E_OK ) er = xt_store(o, pl, &xs);
	}
	xts_free(&xs);
	return er;
}

/*
 * Write into a data part at `off`. A small part stays inline while it
 * fits; past that it is blocks, grown as the write needs. The bytes go
 * straight to their blocks: they are not in the journal.
 */
LOCAL ER dseg_write( OBJ *o, UB *pl, UD off, CONST UB *src, SZ len )
{
	UD	size = rd64(pl + PL_BYTES), end = off + (UD)len, need, have = 0;
	XTS	xs;
	INT	noff;
	ER	er;

	if ( len <= 0 ) {
		return E_OK;
	}

	/* small: inline, moved to a place big enough for the new size */
	if ( ( pl[PL_FORM] == F_NONE || pl[PL_FORM] == F_INLINE ) && end <= DATA_INLINE_MAX ) {
		UB	tmp[DATA_INLINE_MAX];
		UD	nsize = ( end > size ) ? end : size;

		knl_memset(tmp, 0, sizeof(tmp));
		if ( pl[PL_FORM] == F_INLINE ) {
			knl_memcpy(tmp, o->hdr + rd16(pl + PL_A), (INT)size);
			inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		}
		knl_memcpy(tmp + off, src, (INT)len);
		noff = inl_take(o->hdr, (SZ)nsize);
		if ( noff >= 0 ) {
			knl_memcpy(o->hdr + noff, tmp, (INT)nsize);
			pl[PL_FORM] = F_INLINE;
			wr64(pl + PL_BYTES, nsize);
			wr16(pl + PL_A, (UH)noff);
			wr16(pl + PL_A + 2, (UH)BLKS_FOR(nsize, TSFSO_INLINE_UNIT));
			return E_OK;
		}
		/* no room inline: the old bytes are put back and it becomes blocks */
		if ( pl[PL_FORM] == F_INLINE ) {
			noff = inl_take(o->hdr, (SZ)size);	/* the place it just gave up */
			if ( noff < 0 ) {
				return E_NOMEM;
			}
			knl_memcpy(o->hdr + noff, tmp, (INT)size);
			wr16(pl + PL_A, (UH)noff);
		}
	}
	er = dseg_unline(o, pl);
	if ( er < E_OK ) {
		return er;
	}

	need = BLKS_FOR(end, TSFSBLK_BLOCK_SIZE);
	er = place_have(o, pl, &have);
	if ( er >= E_OK && need > have ) {
		/* blocks the write covers whole need not be cleared first */
		UD	kf = BLKS_FOR(off, TSFSBLK_BLOCK_SIZE);
		UD	kt = end / TSFSBLK_BLOCK_SIZE;

		er = dseg_grow(o, pl, need, kf, ( kt > kf ) ? kt : kf);
	}
	if ( er >= E_OK ) er = xts_new(&xs);
	if ( er < E_OK ) {
		return er;
	}
	er = xt_load_range(o, pl, &xs, off / TSFSBLK_BLOCK_SIZE, need);
	if ( er >= E_OK ) er = runs_write(o, xs.x, xs.n, off, src, len, FALSE);
	xts_free(&xs);
	if ( er >= E_OK && end > size ) {
		wr64(pl + PL_BYTES, end);
	}
	return er;
}

/* The tail of the block that holds byte `nsize` made zeros, past it */
LOCAL ER tail_clear( OBJ *o, CONST XT *x, INT n, UD nsize )
{
	UB	*buf;
	UD	p;
	ER	er = E_OK;

	if ( nsize % TSFSBLK_BLOCK_SIZE == 0 ) {
		return E_OK;
	}
	p = xt_map(x, n, nsize / TSFSBLK_BLOCK_SIZE, NULL);
	if ( p == 0 ) {
		return E_OK;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = ts_read_blk(o->vol, p, buf);
	if ( er >= E_OK ) {
		knl_memset(buf + nsize % TSFSBLK_BLOCK_SIZE, 0,
			   (INT)( TSFSBLK_BLOCK_SIZE - nsize % TSFSBLK_BLOCK_SIZE ));
		er = ts_write_blk(o->vol, p, buf);
	}
	Kfree(buf);
	return er;
}

/* Everything a data part holds given back, now or once it is real */
LOCAL ER dseg_free( OBJ *o, UB *pl, BOOL later )
{
	XTS	xs;
	ER	er;

	if ( pl[PL_FORM] == F_INLINE ) {
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		knl_memset(pl, 0, PL_SIZE);
		return E_OK;
	}
	if ( pl[PL_FORM] == F_TREEN ) {
		er = tn_free(o, rd64(pl + PL_A), -1, later, TRUE, NULL);
		knl_memset(pl, 0, PL_SIZE);
		return er;
	}
	er = xts_new(&xs);
	if ( er >= E_OK ) er = xt_load(o, pl, &xs);
	if ( er >= E_OK ) er = xt_shrink(o, &xs, 0, later);
	if ( er >= E_OK ) er = tree_free(o, pl, &xs, later);
	xts_free(&xs);
	knl_memset(pl, 0, PL_SIZE);
	return er;
}

/*
 * The part cut or lengthened to `size`. What is cut is given back; the
 * tail of the last block is cleared, so that lengthening it again later
 * shows zeros and not what was there before.
 */
LOCAL ER dseg_trunc( OBJ *o, UB *pl, UD nsize )
{
	UD	size = rd64(pl + PL_BYTES), have = 0;
	UB	*zero;
	XTS	xs;
	TEDIT	t;
	ER	er = E_OK;

	if ( nsize > size ) {
		/* lengthened: small, it stays inline, written with zeros */
		if ( ( pl[PL_FORM] == F_NONE || pl[PL_FORM] == F_INLINE ) && nsize <= DATA_INLINE_MAX ) {
			zero = (UB *)Kmalloc(DATA_INLINE_MAX);
			if ( zero == NULL ) return E_NOMEM;
			knl_memset(zero, 0, DATA_INLINE_MAX);
			er = dseg_write(o, pl, size, zero, (SZ)( nsize - size ));
			Kfree(zero);
			return er;
		}
		/* else blocks: the last one's bytes past the end are zeros
		   already, and the blocks added are cleared as they are taken */
		er = dseg_unline(o, pl);
		if ( er >= E_OK ) er = place_have(o, pl, &have);
		if ( er >= E_OK && BLKS_FOR(nsize, TSFSBLK_BLOCK_SIZE) > have ) {
			er = dseg_grow(o, pl, BLKS_FOR(nsize, TSFSBLK_BLOCK_SIZE), 0, 0);
		}
		if ( er >= E_OK ) wr64(pl + PL_BYTES, nsize);
		return er;
	}
	if ( pl[PL_FORM] == F_INLINE ) {
		if ( nsize == 0 ) {
			inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
			knl_memset(pl, 0, PL_SIZE);
		} else {
			knl_memset(o->hdr + rd16(pl + PL_A) + nsize, 0, (INT)( size - nsize ));
			wr64(pl + PL_BYTES, nsize);
		}
		return E_OK;
	}
	if ( pl[PL_FORM] == F_TREEN ) {
		if ( nsize == 0 ) {
			return dseg_free(o, pl, FALSE);
		}
		er = te_init(&t, o);
		if ( er >= E_OK ) er = te_cut(&t, pl, BLKS_FOR(nsize, TSFSBLK_BLOCK_SIZE));
		if ( er >= E_OK ) er = tail_clear(o, t.tail.x, t.tail.n, nsize);
		te_done(&t);
		if ( er >= E_OK ) wr64(pl + PL_BYTES, nsize);
		return er;
	}
	er = xts_new(&xs);
	if ( er >= E_OK ) er = xt_load(o, pl, &xs);
	if ( er >= E_OK ) er = xt_shrink(o, &xs, BLKS_FOR(nsize, TSFSBLK_BLOCK_SIZE), FALSE);
	if ( er >= E_OK ) er = xt_store(o, pl, &xs);
	if ( er >= E_OK ) er = tail_clear(o, xs.x, xs.n, nsize);
	if ( er >= E_OK ) {
		wr64(pl + PL_BYTES, nsize);
		if ( xs.n == 0 ) knl_memset(pl, 0, PL_SIZE);
	}
	xts_free(&xs);
	return er;
}

/* ---------------------------------------------------------------- management parts */

/*
 * A run of exactly `want` blocks. The allocator gives the first free run
 * it meets, which can be a short hole; those are held aside while the
 * search goes on, and given back at the end.
 */
#define RUN_TRIES	16

LOCAL ER run_take( ID vol, UD want, UD *p_start )
{
	UD	hs[RUN_TRIES], hn[RUN_TRIES], start = 0, got = 0;
	INT	nh = 0;
	ER	er;

	for ( ;; ) {
		er = ts_alloc_ext(vol, want, &start, &got);
		if ( er < E_OK || got >= want ) {
			break;
		}
		if ( nh == RUN_TRIES ) {
			ts_free_ext(vol, start, got);
			er = E_NOMEM;		/* no run long enough */
			break;
		}
		hs[nh] = start;
		hn[nh] = got;
		nh++;
	}
	while ( nh > 0 ) {
		nh--;
		ts_free_ext(vol, hs[nh], hn[nh]);
	}
	if ( er >= E_OK ) {
		*p_start = start;
	}
	return er;
}

/*
 * A management part read whole: inline, or from its blocks, each with
 * the common head and checked. Of version 4 the blocks are one run; of
 * version 5 they may also lie in the runs of a leaf or a run tree. At
 * most `max` bytes are copied; *p_len is how many it holds.
 */
LOCAL ER mseg_read( OBJ *o, CONST UB *pl, UB *out, SZ max, SZ *p_len )
{
	UB	*buf;
	UD	len = rd64(pl + PL_BYTES), b;
	SZ	done = 0, n;
	XTS	xs;
	INT	k;
	ER	er = E_OK;

	*p_len = (SZ)len;
	switch ( pl[PL_FORM] ) {
	case F_NONE:
		*p_len = 0;
		return E_OK;
	case F_INLINE:
		n = ( (SZ)len < max ) ? (SZ)len : max;
		knl_memcpy(out, o->hdr + rd16(pl + PL_A), (INT)n);
		return E_OK;
	case F_RUN:
	case F_TREE:
	case F_TREEN:
		break;
	default:
		return E_OBJ;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = xts_new(&xs);
	if ( er >= E_OK ) er = xt_load(o, pl, &xs);
	for ( k = 0; k < xs.n && er >= E_OK && done < max && done < (SZ)len; k++ ) {
		for ( b = 0; b < xs.x[k].n && done < max && done < (SZ)len; b++ ) {
			er = node_read(o, xs.x[k].p + b, buf);
			if ( er < E_OK ) break;
			n = MB_PAY;
			if ( n > (SZ)len - done ) n = (SZ)len - done;
			if ( n > max - done ) n = max - done;
			knl_memcpy(out + done, buf + TSFSBLK_HDR_SIZE, (INT)n);
			done += n;
		}
	}
	if ( er >= E_OK && done < max && done < (SZ)len ) {
		er = E_OBJ;			/* the blocks hold less than the place says */
	}
	xts_free(&xs);
	Kfree(buf);
	return er;
}

LOCAL ER spare_take( OBJ *o, UB *pl );

LOCAL void mseg_free( OBJ *o, UB *pl )
{
	if ( pl[PL_FORM] == F_INLINE || pl[PL_FORM] == F_RUN ) {
		if ( pl[PL_FORM] == F_INLINE ) {
			inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		} else if ( rd64(pl + PL_B) > 0 ) {
			ts_free_ext(o->vol, rd64(pl + PL_A), rd64(pl + PL_B));
		}
		knl_memset(pl, 0, PL_SIZE);
		return;
	}
	if ( pl[PL_FORM] == F_TREE || pl[PL_FORM] == F_TREEN ) {
		(void)dseg_free(o, pl, FALSE);
	}
	knl_memset(pl, 0, PL_SIZE);
}

/*
 * The part a management part had given up: a large one, of version 5,
 * into the spare place, from where it is given back a step at a time
 * once the change is real; another now (its blocks are not handed out
 * again before the change is real).
 */
LOCAL ER mseg_drop( OBJ *o, UB *pl )
{
	if ( o->v5 && pl[PL_FORM] != F_INLINE && place_big(o, pl) ) {
		return spare_take(o, pl);
	}
	mseg_free(o, pl);
	return E_OK;
}

/*
 * `len` bytes into blocks with the common head lying in the runs x[0..n),
 * written straight: the part is new and nothing reaches it yet.
 */
LOCAL ER mseg_put( OBJ *o, CONST XT *x, INT n, CONST UB *src, SZ len )
{
	UD	rmax = run_io_max(o), b, k, j;
	UB	*buf;
	void	*raw = NULL;
	SZ	done = 0, c;
	INT	i;
	ER	er = E_OK;

	buf = run_io_buf((SZ)( rmax * TSFSBLK_BLOCK_SIZE ), &raw);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		for ( b = 0; b < x[i].n && er >= E_OK; b += k ) {
			k = x[i].n - b;
			if ( k > rmax ) k = rmax;
			for ( j = 0; j < k; j++ ) {
				UB	*blk = buf + j * TSFSBLK_BLOCK_SIZE;

				knl_memset(blk, 0, TSFSBLK_BLOCK_SIZE);
				c = MB_PAY;
				if ( c > len - done ) c = len - done;
				if ( c > 0 ) knl_memcpy(blk + TSFSBLK_HDR_SIZE, src + done, (INT)c);
				done += c;
				ts_blk_seal(o->vol, blk, TSFSBLK_MAGIC_TBL, x[i].p + b + j, o->owner);
			}
			er = ( k == 1 ) ? ts_write_blk(o->vol, x[i].p + b, buf)
					: ts_write_blks(o->vol, x[i].p + b, k, buf);
		}
	}
	Kfree(raw);
	return er;
}

/*
 * A management part written whole: inline when it fits there, else
 * blocks with the common head.
 *
 * On a volume of version 4, and for a part of up to an eighth of what a
 * transaction holds, the blocks go through the journal: one run, used
 * again when it has the right length; of version 5, the runs wherever
 * there is room when no run is long enough.
 *
 * A larger part, of version 5, is written into blocks taken new,
 * straight and not through the journal; the transaction of the change
 * only makes the place say where they are and gives the old part up
 * (design 7.3). Its size is then not bound by what a transaction holds.
 */
LOCAL ER mseg_write( OBJ *o, UB *pl, CONST UB *src, SZ len )
{
	UB	*buf, fresh[PL_SIZE];
	UD	start = 0, want, i;
	SZ	done = 0, n;
	INT	off;
	XTS	xs;
	ER	er = E_OK;

	if ( pl[PL_FORM] == F_INLINE ) {
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		pl[PL_FORM] = F_NONE;
	}
	if ( len <= 0 ) {
		return mseg_drop(o, pl);
	}
	off = ( len <= TSFSO_INLINE_BYTES ) ? inl_take(o->hdr, len) : -1;
	if ( off >= 0 ) {
		er = mseg_drop(o, pl);
		if ( er < E_OK ) {
			inl_give(o->hdr, off, (INT)BLKS_FOR(len, TSFSO_INLINE_UNIT));
			return er;
		}
		knl_memcpy(o->hdr + off, src, (INT)len);
		pl[PL_FORM] = F_INLINE;
		wr64(pl + PL_BYTES, (UD)len);
		wr16(pl + PL_A, (UH)off);
		wr16(pl + PL_A + 2, (UH)BLKS_FOR(len, TSFSO_INLINE_UNIT));
		return E_OK;
	}

	want = BLKS_FOR(len, MB_PAY);
	if ( o->v5 && want > big_blocks(o->vol) ) {
		/* new blocks, straight, then the place moved to them */
		knl_memset(fresh, 0, PL_SIZE);
		er = xts_new(&xs);
		if ( er >= E_OK ) er = xt_grow(o, &xs, want, 0, want);
		if ( er >= E_OK ) er = mseg_put(o, xs.x, xs.n, src, len);
		if ( er >= E_OK ) {
			o->direct = TRUE;
			er = xt_store(o, fresh, &xs);
			o->direct = FALSE;
		}
		if ( er < E_OK ) {
			(void)xt_shrink(o, &xs, 0, FALSE);
			xts_free(&xs);
			return er;
		}
		xts_free(&xs);
		wr64(fresh + PL_BYTES, (UD)len);
		er = mseg_drop(o, pl);
		if ( er >= E_OK ) knl_memcpy(pl, fresh, PL_SIZE);
		return er;
	}

	if ( pl[PL_FORM] == F_RUN && rd64(pl + PL_B) == want ) {
		start = rd64(pl + PL_A);
	} else {
		mseg_free(o, pl);
		er = run_take(o->vol, want, &start);
		if ( er < E_OK && o->v5 ) {
			/* no run long enough: the blocks wherever they are */
			er = xts_new(&xs);
			if ( er >= E_OK ) er = xt_grow(o, &xs, want, 0, want);
			if ( er >= E_OK ) er = mseg_put(o, xs.x, xs.n, src, len);
			if ( er >= E_OK ) er = xt_store(o, pl, &xs);
			if ( er < E_OK ) (void)xt_shrink(o, &xs, 0, FALSE);
			xts_free(&xs);
			if ( er >= E_OK ) wr64(pl + PL_BYTES, (UD)len);
			return er;
		}
		if ( er < E_OK ) {
			return er;
		}
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < want && er >= E_OK; i++ ) {
		knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
		n = MB_PAY;
		if ( n > len - done ) n = len - done;
		knl_memcpy(buf + TSFSBLK_HDR_SIZE, src + done, (INT)n);
		done += n;
		er = node_write(o, start + i, buf, FALSE);
	}
	Kfree(buf);
	pl[PL_FORM] = F_RUN;
	wr64(pl + PL_BYTES, (UD)len);
	wr64(pl + PL_A, start);
	wr64(pl + PL_B, want);
	return er;
}

/* ---------------------------------------------------------------- tables */

/*
 * A table read whole into memory with room for one more entry. `max` is
 * the volume's limit of its entries, 0 for none: a table with more is
 * not a table of the volume.
 */
LOCAL ER tab_load( OBJ *o, INT at, INT cnt_at, INT esz, UINT max, UB **p_buf, INT *p_n )
{
	UB	*buf;
	SZ	len = 0;
	UD	n = rd32(o->hdr + cnt_at);
	ER	er;

	*p_buf = NULL;
	*p_n = 0;
	if ( over(max, n) || n > 0x7FFFFFF0UL / (UD)esz ) {
		return E_OBJ;
	}
	buf = (UB *)Kmalloc((SZ)( n + 1 ) * esz);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(buf, 0, (SZ)( n + 1 ) * esz);
	er = mseg_read(o, o->hdr + at, buf, (SZ)n * esz, &len);
	if ( er >= E_OK && len != (SZ)n * esz ) {
		er = E_OBJ;
	}
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	*p_buf = buf;
	*p_n = (INT)n;
	return E_OK;
}

LOCAL ER tab_store( OBJ *o, INT at, INT cnt_at, INT esz, UB *buf, INT n )
{
	wr32(o->hdr + cnt_at, (UW)n);
	return mseg_write(o, o->hdr + at, buf, (SZ)n * esz);
}

#define PLACE_LOAD(o, b, n)	tab_load(o, OB_PLACE, OB_NPLACE, PE_SIZE, (o)->lim.max_rec, b, n)
#define PLACE_STORE(o, b, n)	tab_store(o, OB_PLACE, OB_NPLACE, PE_SIZE, b, n)
#define LINK_LOAD(o, b, n)	tab_load(o, OB_LINK, OB_NLINK, LK_SIZE, (o)->lim.max_link, b, n)
#define LINK_STORE(o, b, n)	tab_store(o, OB_LINK, OB_NLINK, LK_SIZE, b, n)

/*
 * The resource table. On a volume of version 5 an extension of more
 * than 15 bytes, or one that starts with RSX_MARK, is kept in the string
 * area behind the entries (design 7.3): the entry's RS_EXT starts with
 * RSX_MARK and gives the offset and the length. The area is made again,
 * packed in the order of the entries, each time the table is written.
 */
typedef struct {
	UB	*tab;			/* the entries, room for one more */
	INT	n;
	UB	*str;			/* the string area as read, and what was put behind */
	UD	slen, scap;
} RTAB;

LOCAL void res_free( RTAB *r )
{
	if ( r->tab != NULL ) Kfree(r->tab);
	if ( r->str != NULL ) Kfree(r->str);
	r->tab = NULL;
	r->str = NULL;
	r->n = 0;
	r->slen = r->scap = 0;
}

LOCAL BOOL res_long( CONST UB *e )
{
	return (BOOL)( e[RS_EXT] == RSX_MARK );
}

LOCAL ER res_load( OBJ *o, RTAB *r )
{
	UB	*buf;
	SZ	len = 0;
	UD	n = rd32(o->hdr + OB_NRES), all = rd64(o->hdr + OB_RES + PL_BYTES), sl;
	INT	i;
	ER	er;

	knl_memset(r, 0, sizeof(*r));
	if ( over(o->lim.max_res, n) || n > 0x7FFFFFF0UL / RS_SIZE ) {
		return E_OBJ;
	}
	if ( o->hdr[OB_RES + PL_FORM] == F_NONE ) all = 0;
	sl = ( o->v5 && all > n * RS_SIZE ) ? all - n * RS_SIZE : 0;
	if ( sl > (UD)TSFSO_EXT_MAX * ( n + 1 ) ) {
		return E_OBJ;			/* more string than the entries can name */
	}
	buf = (UB *)Kmalloc((SZ)( ( n + 1 ) * RS_SIZE + sl ));
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(buf, 0, (SZ)( ( n + 1 ) * RS_SIZE ));
	er = mseg_read(o, o->hdr + OB_RES, buf, (SZ)( n * RS_SIZE + sl ), &len);
	if ( er >= E_OK && len != (SZ)( n * RS_SIZE + sl ) ) {
		er = E_OBJ;
	}
	if ( er >= E_OK && sl > 0 ) {
		r->str = (UB *)Kmalloc((SZ)sl);
		if ( r->str == NULL ) {
			er = E_NOMEM;
		} else {
			knl_memcpy(r->str, buf + n * RS_SIZE, (SZ)sl);
			knl_memset(buf + n * RS_SIZE, 0, RS_SIZE);
			r->slen = r->scap = sl;
		}
	}
	for ( i = 0; i < (INT)n && er >= E_OK && o->v5; i++ ) {
		UB	*e = buf + i * RS_SIZE;

		if ( res_long(e) && ( (UD)rd32(e + RS_EXT + RSX_OFF) + rd32(e + RS_EXT + RSX_LEN) > sl
				      || rd32(e + RS_EXT + RSX_LEN) > TSFSO_EXT_MAX ) ) {
			er = E_OBJ;
		}
	}
	if ( er < E_OK ) {
		Kfree(buf);
		if ( r->str != NULL ) Kfree(r->str);
		knl_memset(r, 0, sizeof(*r));
		return er;
	}
	r->tab = buf;
	r->n = (INT)n;
	return E_OK;
}

/* The extension of entry `i`: at most max - 1 bytes and a NUL into out (when not NULL); its length */
LOCAL INT res_ext( CONST RTAB *r, BOOL v5, INT i, UB *out, INT max )
{
	CONST UB *e = r->tab + i * RS_SIZE, *s;
	INT	len, k;

	if ( v5 && res_long(e) ) {
		s = r->str + rd32(e + RS_EXT + RSX_OFF);
		len = (INT)rd32(e + RS_EXT + RSX_LEN);
	} else {
		s = e + RS_EXT;
		for ( len = 0; len < TSFSO_EXT_LEN && s[len] != 0; len++ ) ;
	}
	if ( out != NULL && max > 0 ) {
		k = ( len < max - 1 ) ? len : max - 1;
		while ( k > 0 && k < len && ( s[k] & 0xC0 ) == 0x80 ) k--;	/* cut at a whole letter */
		knl_memcpy(out, s, k);
		out[k] = 0;
	}
	return len;
}

/* Entry `i` given the extension `ext` (`len` bytes) */
LOCAL ER res_ext_set( RTAB *r, BOOL v5, INT i, CONST UB *ext, INT len )
{
	UB	*e = r->tab + i * RS_SIZE;

	knl_memset(e + RS_EXT, 0, TSFSO_EXT_LEN);
	if ( len < TSFSO_EXT_LEN && ( !v5 || len == 0 || ext[0] != RSX_MARK ) ) {
		if ( len > 0 ) knl_memcpy(e + RS_EXT, ext, len);
		return E_OK;
	}
	if ( !v5 ) {
		return E_PAR;
	}
	if ( r->slen + (UD)len > r->scap ) {
		UD	cap = ( r->scap > 0 ) ? r->scap * 2 : 256;
		UB	*ns;

		while ( cap < r->slen + (UD)len ) cap *= 2;
		ns = (UB *)Kmalloc((SZ)cap);
		if ( ns == NULL ) {
			return E_NOMEM;
		}
		if ( r->slen > 0 ) knl_memcpy(ns, r->str, (SZ)r->slen);
		if ( r->str != NULL ) Kfree(r->str);
		r->str = ns;
		r->scap = cap;
	}
	knl_memcpy(r->str + r->slen, ext, len);
	e[RS_EXT] = RSX_MARK;
	wr32(e + RS_EXT + RSX_OFF, (UW)r->slen);
	wr32(e + RS_EXT + RSX_LEN, (UW)len);
	r->slen += (UD)len;
	return E_OK;
}

/* Entry `i` taken out: the last one moves into its room */
LOCAL void res_remove( RTAB *r, INT i )
{
	if ( i != r->n - 1 ) {
		knl_memcpy(r->tab + i * RS_SIZE, r->tab + ( r->n - 1 ) * RS_SIZE, RS_SIZE);
	}
	r->n--;
}

/* The table written whole: the entries, then the string area packed */
LOCAL ER res_store( OBJ *o, RTAB *r )
{
	UB	*buf;
	UD	sl = 0, at;
	INT	i;
	ER	er;

	for ( i = 0; i < r->n && o->v5; i++ ) {
		if ( res_long(r->tab + i * RS_SIZE) ) sl += rd32(r->tab + i * RS_SIZE + RS_EXT + RSX_LEN);
	}
	if ( sl == 0 ) {
		wr32(o->hdr + OB_NRES, (UW)r->n);
		return mseg_write(o, o->hdr + OB_RES, r->tab, (SZ)r->n * RS_SIZE);
	}
	buf = (UB *)Kmalloc((SZ)( (UD)r->n * RS_SIZE + sl ));
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(buf, r->tab, (SZ)r->n * RS_SIZE);
	at = 0;
	for ( i = 0; i < r->n; i++ ) {
		UB	*e = buf + i * RS_SIZE;
		UD	len;

		if ( !res_long(e) ) continue;
		len = rd32(e + RS_EXT + RSX_LEN);
		knl_memcpy(buf + (UD)r->n * RS_SIZE + at, r->str + rd32(e + RS_EXT + RSX_OFF), (SZ)len);
		wr32(e + RS_EXT + RSX_OFF, (UW)at);
		at += len;
	}
	wr32(o->hdr + OB_NRES, (UW)r->n);
	er = mseg_write(o, o->hdr + OB_RES, buf, (SZ)( (UD)r->n * RS_SIZE + sl ));
	Kfree(buf);
	return er;
}

/* ---------------------------------------------------------------- the metadata text */

LOCAL UB *meta_all( OBJ *o, SZ extra, SZ *p_len )
{
	UD	len = rd64(o->hdr + OB_META + PL_BYTES);
	UB	*j = (UB *)Kmalloc((SZ)len + extra + 1);
	SZ	got = 0;

	if ( j == NULL ) {
		return NULL;
	}
	if ( mseg_read(o, o->hdr + OB_META, j, (SZ)len, &got) < E_OK ) {
		Kfree(j);
		return NULL;
	}
	j[got] = 0;
	*p_len = got;
	return j;
}

/*
 * A member of the outer object of a JSON text, edited in place. The
 * text is a buffer of `max` bytes holding `len`; each edit answers the
 * new length, or -1 when the text is not an object or has no room.
 */

LOCAL BOOL is_blank( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* Where member `key` starts (its opening quote), where its value starts and ends */
LOCAL BOOL jx_member( CONST UB *j, INT len, CONST char *key, INT *p_from, INT *p_val, INT *p_to )
{
	T_JSON	root, v;
	INT	p, q;

	if ( js_span(j, len, 0, &root) < E_OK || js_type(&root) != JS_OBJECT
	  || js_get(&root, key, &v) < E_OK ) {
		return FALSE;
	}
	p = (INT)( v.s - j );
	for ( q = p - 1; q > 0 && j[q] != ':'; q-- ) ;
	for ( q--; q > 0 && j[q] != '"'; q-- ) ;	/* the key's closing quote */
	for ( q--; q > 0 && j[q] != '"'; q-- ) ;	/* and its opening one */
	*p_from = q;
	*p_val = p;
	*p_to = p + v.len;
	return TRUE;
}

LOCAL BOOL is_object( CONST UB *j, INT len )
{
	T_JSON	root;

	return (BOOL)( js_span(j, len, 0, &root) >= E_OK && js_type(&root) == JS_OBJECT );
}

/* The member taken out, with the comma that went with it */
LOCAL INT jx_del( UB *j, INT len, CONST char *key )
{
	INT	from, val, to, i, k;

	if ( !jx_member(j, len, key, &from, &val, &to) ) {
		return len;
	}
	for ( k = to; k < len && is_blank(j[k]); k++ ) ;
	if ( k < len && j[k] == ',' ) {
		to = k + 1;
	} else {
		for ( k = from - 1; k > 0 && is_blank(j[k]); k-- ) ;
		if ( j[k] == ',' ) from = k;
	}
	for ( i = to; i < len; i++ ) {
		j[from + i - to] = j[i];
	}
	return len - ( to - from );
}

/* The member given the value `val` (JSON text): replaced, or put first */
LOCAL INT jx_set( UB *j, INT len, INT max, CONST char *key, CONST UB *val, INT vlen )
{
	INT	from, p, to, i, k = 0, add, open, n;
	BOOL	empty;

	if ( jx_member(j, len, key, &from, &p, &to) ) {
		add = vlen - ( to - p );
		if ( len + add > max ) return -1;
		if ( add > 0 ) {
			for ( i = len - 1; i >= to; i-- ) j[i + add] = j[i];
		} else if ( add < 0 ) {
			for ( i = to; i < len; i++ ) j[i + add] = j[i];
		}
		for ( i = 0; i < vlen; i++ ) j[p + i] = val[i];
		return len + add;
	}
	if ( !is_object(j, len) ) {
		return -1;
	}
	for ( open = 0; open < len && j[open] != '{'; open++ ) ;
	for ( n = open + 1; n < len && is_blank(j[n]); n++ ) ;
	empty = (BOOL)( n < len && j[n] == '}' );
	while ( key[k] != '\0' ) k++;
	add = k + 3 + vlen + ( empty ? 0 : 1 );
	if ( len + add > max ) return -1;
	for ( i = len - 1; i > open; i-- ) j[i + add] = j[i];
	n = open + 1;
	j[n++] = '"';
	for ( i = 0; i < k; i++ ) j[n++] = (UB)key[i];
	j[n++] = '"';
	j[n++] = ':';
	for ( i = 0; i < vlen; i++ ) j[n++] = val[i];
	if ( !empty ) j[n++] = ',';
	return len + add;
}

LOCAL INT num_text( UB *out, D v )
{
	UB	t[24];
	INT	n = 0, k = 0;
	UD	u = ( v < 0 ) ? (UD)( -v ) : (UD)v;

	do {
		t[n++] = (UB)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	if ( v < 0 ) out[k++] = '-';
	while ( n > 0 ) out[k++] = t[--n];
	return k;
}

/* A string as JSON text, quoted and escaped */
LOCAL INT str_text( UB *out, INT max, CONST UB *str )
{
	INT	n = 0, i;

	out[n++] = '"';
	for ( i = 0; str[i] != '\0' && n < max - 3; i++ ) {
		if ( str[i] == '"' || str[i] == '\\' ) out[n++] = '\\';
		out[n++] = str[i];
	}
	out[n++] = '"';
	return n;
}

/* A time of the object block as ISO 8601 in UTC, quoted; 0 for none */
LOCAL INT date_text( UB *out, UD ns )
{
	TS_TIME	t = (TS_TIME)( ns / 1000000000ULL );
	TS_TM	tm;
	INT	n;

	if ( ns == 0 || dt_gmtime(&t, &tm) < E_OK ) {
		return 0;
	}
	out[0] = '"';
	n = dt_strftime((char *)out + 1, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
	if ( n <= 0 ) return 0;
	out[n + 1] = '"';
	return n + 2;
}

/*
 * The members of the metadata the object block holds (design 11.5):
 * never kept in the text, put in when the text is read.
 */
LOCAL CONST char * CONST struct_keys[] = {
	"refCount", "recordCount", "makeDate", "updateDate", "accessDate", NULL
};

LOCAL INT meta_strip( UB *j, INT len )
{
	INT	i;

	for ( i = 0; struct_keys[i] != NULL; i++ ) {
		len = jx_del(j, len, struct_keys[i]);
	}
	return len;
}

/* The text as it is read: the structural members put in from the object block */
LOCAL INT meta_synth( OBJ *o, UB *j, INT len, INT max )
{
	UB	t[40];
	INT	n, k;

	if ( !is_object(j, len) ) {
		return len;			/* not an object: given back as it was */
	}
	n = date_text(t, rd64(o->hdr + OB_READ));
	if ( n > 0 && ( k = jx_set(j, len, max, "accessDate", t, n) ) > 0 ) len = k;
	n = date_text(t, rd64(o->hdr + OB_UPDATED));
	if ( n > 0 && ( k = jx_set(j, len, max, "updateDate", t, n) ) > 0 ) len = k;
	n = date_text(t, rd64(o->hdr + OB_MADE));
	if ( n > 0 && ( k = jx_set(j, len, max, "makeDate", t, n) ) > 0 ) len = k;
	n = num_text(t, (D)rd32(o->hdr + OB_NPLACE));
	if ( ( k = jx_set(j, len, max, "recordCount", t, n) ) > 0 ) len = k;
	n = num_text(t, (D)(INT)rd32(o->hdr + OB_REFCNT));
	if ( ( k = jx_set(j, len, max, "refCount", t, n) ) > 0 ) len = k;
	return len;
}

/* ---------------------------------------------------------------- the order of the records */

/*
 * The records in order, with their types, are the member "records" of
 * the metadata text: [{"rid":1,"rt":1,"sub":0}, ...] (design 11.5). A
 * text without it, or one that does not name every record once, gives
 * the order of the rids.
 */
typedef struct {
	UW	rid;
	UW	rt;
	UW	sub;
} RECE;

LOCAL INT order_parse( CONST UB *j, INT len, RECE *r, INT max )
{
	T_JSON	root, arr, it;
	INT	n = 0;

	if ( js_span(j, len, 0, &root) < E_OK || js_type(&root) != JS_OBJECT
	  || js_get(&root, "records", &arr) < E_OK || js_type(&arr) != JS_ARRAY ) {
		return -1;
	}
	it.s = NULL;
	while ( js_next(&arr, &it) ) {
		if ( n >= max || js_type(&it) != JS_OBJECT ) return -1;
		r[n].rid = (UW)js_get_num(&it, "rid", 0);
		r[n].rt  = (UW)js_get_num(&it, "rt", 1);
		r[n].sub = (UW)js_get_num(&it, "sub", 0);
		if ( r[n].rid == 0 ) return -1;
		n++;
	}
	return n;
}

LOCAL INT order_text( CONST RECE *r, INT n, UB *out )
{
	INT	k = 0, i;

	out[k++] = '[';
	for ( i = 0; i < n; i++ ) {
		if ( i > 0 ) out[k++] = ',';
		knl_memcpy(out + k, "{\"rid\":", 7);   k += 7;
		k += num_text(out + k, (D)r[i].rid);
		knl_memcpy(out + k, ",\"rt\":", 6);    k += 6;
		k += num_text(out + k, (D)r[i].rt);
		knl_memcpy(out + k, ",\"sub\":", 7);   k += 7;
		k += num_text(out + k, (D)r[i].sub);
		out[k++] = '}';
	}
	out[k++] = ']';
	return k;
}
#define ORDER_TEXT_MAX(n)	( (SZ)(n) * 56 + 8 )

/* Room for the order of `n` records, and one put in */
LOCAL RECE *ord_new( INT n )
{
	return (RECE *)Kmalloc(sizeof(RECE) * (SZ)( ( n > 0 ? n : 0 ) + 2 ));
}

LOCAL UW default_rt( UW kind )
{
	return ( kind == TSFS_REC_XTAD ) ? 1 : 15;
}

LOCAL INT order_of( OBJ *o, CONST UB *tab, INT ntab, RECE *r )
{
	UB	*j;
	SZ	len = 0;
	INT	n = -1, i, k;

	j = meta_all(o, 0, &len);
	if ( j != NULL ) {
		n = order_parse(j, (INT)len, r, ntab);
		Kfree(j);
	}
	if ( n == ntab ) {
		/* each rid of the table once: one named twice leaves another
		   that cannot be reached */
		for ( i = 0; i < n; i++ ) {
			for ( k = 0; k < ntab && rd32(tab + k * PE_SIZE + PE_RID) != r[i].rid; k++ ) ;
			if ( k == ntab ) break;
			for ( k = 0; k < i && r[k].rid != r[i].rid; k++ ) ;
			if ( k < i ) break;
		}
		if ( i == n ) return n;
	}
	for ( i = 0; i < ntab; i++ ) {
		r[i].rid = rd32(tab + i * PE_SIZE + PE_RID);
		r[i].rt  = default_rt(rd32(tab + i * PE_SIZE + PE_KIND));
		r[i].sub = 0;
	}
	return ntab;
}

LOCAL ER order_store( OBJ *o, CONST RECE *r, INT n )
{
	UB	*j, *txt;
	SZ	len = 0;
	INT	tl, nl;
	ER	er;

	txt = (UB *)Kmalloc(ORDER_TEXT_MAX(n));
	if ( txt == NULL ) {
		return E_NOMEM;
	}
	tl = order_text(r, n, txt);
	j = meta_all(o, tl + 32, &len);
	if ( j == NULL ) {
		Kfree(txt);
		return E_NOMEM;
	}
	if ( len == 0 ) {
		j[0] = '{';
		j[1] = '}';
		len = 2;
	}
	nl = jx_set(j, (INT)len, (INT)len + tl + 32, "records", txt, tl);
	/* a text that is not an object keeps the order of the rids */
	er = ( nl > 0 ) ? mseg_write(o, o->hdr + OB_META, j, (SZ)nl) : E_OK;
	Kfree(j);
	Kfree(txt);
	return er;
}

/* ---------------------------------------------------------------- transactions */

LOCAL BOOL vol_ok( ID vol )
{
	return ( vol >= 1 && vol <= TSFSBLK_MAX_VOL );
}

/*
 * One operation is one transaction unless the caller has opened one of
 * its own, in which case the operation joins it and the caller decides
 * when it becomes real. The operation's transaction is closed into the
 * log's queue at its end; the caller that let the volume's lock go makes
 * it real (tsfs.c, vol_unlock).
 */
LOCAL ER txn_begin( ID vol )
{
	ER	er;

	if ( !vol_ok(vol) ) {
		return E_ID;
	}
	GEN_BUMP(vol);
	if ( obj_trx[vol - 1] > 0 ) {
		return E_OK;
	}
	er = ts_jrnl_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	ts_btree_jrnl(vol, TRUE);

	return E_OK;
}

LOCAL void spq_clear( ID vol );
LOCAL void spq_run( ID vol );

LOCAL ER txn_end( ID vol, ER er )
{
	if ( vol_ok(vol) ) GEN_BUMP(vol);
	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 ) {
		return er;			/* the caller's transaction goes on */
	}
	ts_btree_jrnl(vol, FALSE);
	if ( er < E_OK ) {
		ts_jrnl_abort(vol);
		pend_clear(vol);		/* the old places are still in use */
		spq_clear(vol);			/* and no spare place was filled */
		return er;
	}
	cut_check(vol);
	er = ts_jrnl_close(vol, NULL);
	if ( er >= E_OK ) {
		pend_apply(vol);
		spq_run(vol);
	} else {
		pend_clear(vol);
		spq_clear(vol);
	}
	return er;
}

EXPORT ER ts_obj_begin( ID vol )
{
	ER	er;

	if ( !vol_ok(vol) ) {
		return E_ID;
	}
	if ( obj_trx[vol - 1] > 0 ) {
		return E_BUSY;			/* one at a time */
	}
	GEN_BUMP(vol);
	er = ts_jrnl_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	ts_btree_jrnl(vol, TRUE);
	obj_trx[vol - 1] = 1;

	return E_OK;
}

/* Whether a caller holds a transaction of its own open on the volume */
EXPORT BOOL ts_obj_trx_open( ID vol )
{
	return (BOOL)( vol_ok(vol) && obj_trx[vol - 1] > 0 );
}

EXPORT ER ts_obj_end( ID vol, BOOL commit )
{
	ER	er;

	if ( !vol_ok(vol) ) {
		return E_ID;
	}
	if ( obj_trx[vol - 1] == 0 ) {
		return E_OBJ;			/* none was opened */
	}
	GEN_BUMP(vol);
	obj_trx[vol - 1] = 0;
	ts_btree_jrnl(vol, FALSE);
	if ( !commit ) {
		pend_clear(vol);
		spq_clear(vol);
		return ts_jrnl_abort(vol);
	}
	/* closed, the freeing behind it closed too, and both made real together */
	er = ts_jrnl_close(vol, NULL);
	if ( er >= E_OK ) {
		pend_apply(vol);
		spq_run(vol);
		er = ts_jrnl_durable(vol, ts_jrnl_next(vol));
	} else {
		pend_clear(vol);
		spq_clear(vol);
	}
	return er;
}

/* ---------------------------------------------------------------- volume */

EXPORT ID ts_obj_mount( CONST char *devnm )
{
	ID	vol = ts_opn_vol_blk(devnm);

	if ( vol > 0 && vol_ok(vol) ) {
		GEN_BUMP(vol);			/* nothing kept of a volume before under this number */
		obj_trx[vol - 1] = 0;
		obj_fan[vol - 1] = 0;
		obj_cut[vol - 1] = 0;
		pend_clear(vol);
		spq_clear(vol);
		ts_btree_jrnl(vol, FALSE);
		(void)ts_obj_tidy(vol, NULL);	/* what a cut in the power left */
	}

	return vol;
}

EXPORT ER ts_obj_unmount( ID vol )
{
	if ( vol_ok(vol) && obj_trx[vol - 1] > 0 ) {
		ts_obj_end(vol, FALSE);
	}
	(void)ts_obj_tidy(vol, NULL);
	if ( vol_ok(vol) ) {
		GEN_BUMP(vol);
		obj_fan[vol - 1] = 0;
	}

	return ts_cls_vol_blk(vol);
}

/*
 * For a test: the nodes of the run trees written from now on hold `n`
 * entries (2 to TSFSO_MAX_EXTENTS; 0 for the format's), until the volume
 * is closed. Trees of many levels can then be made of few runs.
 */
EXPORT ER ts_obj_fan( ID vol, INT n )
{
	if ( !vol_ok(vol) || n < 0 || n == 1 || n > XT_FAN ) {
		return ( !vol_ok(vol) ) ? E_ID : E_PAR;
	}
	obj_fan[vol - 1] = n;
	return E_OK;
}

/*
 * For a test: the power pretended to go (TSFSJ_FAULT_AFTER_COMMIT) as
 * the k-th transaction of the changes from now on is closed, so that a
 * change made of several can be cut after any of them; 0 takes it off.
 */
EXPORT ER ts_obj_cut( ID vol, INT k )
{
	if ( !vol_ok(vol) || k < 0 ) {
		return ( !vol_ok(vol) ) ? E_ID : E_PAR;
	}
	obj_cut[vol - 1] = k;
	return E_OK;
}

EXPORT ER ts_obj_ref_vol( ID vol, T_RVOL *pk_rvol )
{
	T_TSFSBLK_SB	sb;
	SYSTIM		tim;
	UD		count = 0;
	ER		er;

	if ( pk_rvol == NULL ) {
		return E_PAR;
	}
	er = ts_ref_vol_blk(vol, &sb);
	if ( er < E_OK ) {
		return er;
	}
	if ( ts_btree_count(vol, &count) < E_OK ) {
		count = 0;
	}
	pk_rvol->nobj   = (INT)count;
	pk_rvol->blocks = sb.total_blocks;
	pk_rvol->bfree  = sb.free_blocks;
	pk_rvol->bsize  = sb.block_size;
	pk_rvol->time_valid = ( tk_get_tim(&tim) >= E_OK
			     && ((((UD)(UW)tim.hi << 32) | tim.lo) > 1000000000000ULL) );

	return E_OK;
}

/* ---------------------------------------------------------------- objects */

LOCAL CONST char tsfso_default_json[] =
	"{\"name\":\"\",\"refCount\":0,\"recordCount\":0}";

/* An object made under the identity given, with its icon when one is given */
LOCAL ER obj_make( ID vol, CONST TS_UUID *id, CONST UB *json, INT jsonlen,
		   CONST UB *icon, SZ iconlen )
{
	OBJ	o;
	UD	blk = 0, now;
	ER	er;

	if ( json == NULL || jsonlen <= 0 ) {
		json    = (CONST UB *)tsfso_default_json;
		jsonlen = str_len((CONST UB *)tsfso_default_json);
	}
	lim_of(vol, &o.lim);
	if ( over(o.lim.meta_max, (UD)jsonlen) || iconlen < 0 || iconlen > TSFSO_ICON_MAX
	  || ( iconlen > 0 && icon == NULL ) ) {
		return E_PAR;
	}
	o.hdr = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( o.hdr == NULL ) {
		return E_NOMEM;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(o.hdr);
		return er;
	}
	er = ts_alloc_blk(vol, &blk);
	if ( er < E_OK ) {
		goto exit;
	}
	o.vol = vol;
	o.blk = blk;
	o.owner = owner_of(id);
	obj_lim(&o);
	now = now_ns();
	knl_memset(o.hdr, 0, TSFSBLK_BLOCK_SIZE);
	knl_memcpy(o.hdr + OB_UUID, id->b, 16);
	wr32(o.hdr + OB_FLAGS, TSFSO_F_EDITABLE | TSFSO_F_DELETABLE | TSFSO_F_READABLE);
	wr32(o.hdr + OB_NEXTRID, 1);
	wr64(o.hdr + OB_MADE, now);
	wr64(o.hdr + OB_UPDATED, now);
	wr64(o.hdr + OB_READ, now);

	{
		UB	*j = (UB *)Kmalloc((SZ)jsonlen + 1);
		INT	n = jsonlen;

		if ( j == NULL ) {
			er = E_NOMEM;
			goto exit;
		}
		knl_memcpy(j, json, jsonlen);
		if ( is_object(j, n) ) {
			n = meta_strip(j, n);
			n = jx_del(j, n, "records");	/* the order follows the records made */
		}
		er = mseg_write(&o, o.hdr + OB_META, j, (SZ)n);
		Kfree(j);
	}
	if ( er >= E_OK && iconlen > 0 ) er = mseg_write(&o, o.hdr + OB_ICON, icon, iconlen);
	if ( er >= E_OK ) er = obj_store(&o);
	if ( er >= E_OK ) er = ts_btree_insert(vol, id, blk);

    exit:
	if ( er < E_OK && blk != 0 ) {
		ts_free_blk(vol, blk);
	}
	er = txn_end(vol, er);
	Kfree(o.hdr);

	return er;
}

EXPORT ER ts_obj_create( ID vol, CONST UB *json, INT jsonlen, TS_UUID *p_uuid )
{
	TS_UUID	uuid;
	ER	er;

	if ( p_uuid == NULL ) {
		return E_PAR;
	}
	er = ts_gen_uuid(&uuid);
	if ( er < E_OK ) {
		return er;			/* the clock has not been set */
	}
	er = obj_make(vol, &uuid, json, jsonlen, NULL, 0);
	if ( er >= E_OK ) {
		*p_uuid = uuid;
	}
	return er;
}

/*
 * One made with its icon as well as its metadata, in the one
 * transaction: under the identity given, or a new one when `as` is NULL.
 */
EXPORT ER ts_obj_create_icon( ID vol, CONST TS_UUID *as, CONST UB *json, INT jsonlen,
			      CONST UB *icon, SZ iconlen, TS_UUID *p_uuid )
{
	TS_UUID	uuid;
	ER	er;

	if ( as != NULL ) {
		if ( ts_obj_exists(vol, as) >= E_OK ) {
			return E_OBJ;
		}
		uuid = *as;
	} else if ( ( er = ts_gen_uuid(&uuid) ) < E_OK ) {
		return er;
	}
	er = obj_make(vol, &uuid, json, jsonlen, icon, iconlen);
	if ( er >= E_OK && p_uuid != NULL ) {
		*p_uuid = uuid;
	}
	return er;
}

/*
 * One the system knows by a fixed identity (a box, a program of its
 * own): made under that identity, and not over one that is there.
 */
EXPORT ER ts_obj_create_as( ID vol, CONST TS_UUID *uuid, CONST UB *json, INT jsonlen )
{
	if ( uuid == NULL ) {
		return E_PAR;
	}
	if ( ts_obj_exists(vol, uuid) >= E_OK ) {
		return E_OBJ;
	}
	return obj_make(vol, uuid, json, jsonlen, NULL, 0);
}

/*
 * Give everything an object holds back to the volume and take it out of
 * both trees. The caller has checked that it may go.
 */
LOCAL ER obj_free( OBJ *o, CONST TS_UUID *uuid )
{
	UB	*tab = NULL;
	RTAB	rt;
	UINT	flags;
	INT	i, n = 0;
	ER	er;

	if ( PLACE_LOAD(o, &tab, &n) >= E_OK ) {
		for ( i = 0; i < n; i++ ) {
			dseg_free(o, tab + i * PE_SIZE + PE_PLACE, FALSE);
		}
		Kfree(tab);
	}
	if ( res_load(o, &rt) >= E_OK ) {
		for ( i = 0; i < rt.n; i++ ) {
			dseg_free(o, rt.tab + i * RS_SIZE + RS_PLACE, FALSE);
		}
		res_free(&rt);
	}
	if ( o->hdr[OB_SPARE + PL_FORM] != F_TRIM ) {
		dseg_free(o, o->hdr + OB_SPARE, FALSE);	/* what a change left there */
	}
	knl_memset(o->hdr + OB_SPARE, 0, PL_SIZE);
	mseg_free(o, o->hdr + OB_PLACE);
	mseg_free(o, o->hdr + OB_RES);
	mseg_free(o, o->hdr + OB_LINK);
	mseg_free(o, o->hdr + OB_META);
	mseg_free(o, o->hdr + OB_ICON);

	flags = (UINT)rd32(o->hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_GARBAGE ) != 0 ) {
		ts_btree_delete_t(o->vol, TSFSBT_GC, uuid);
	}
	if ( ( flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK | TSFSO_F_SPARE ) ) != 0 ) {
		ts_btree_delete_t(o->vol, TSFSBT_ORPHAN, uuid);
	}
	if ( ( flags & TSFSO_F_ORPHAN ) == 0 ) {
		er = ts_btree_delete(o->vol, uuid);
		if ( er < E_OK ) {
			return er;
		}
	}
	return ts_free_blk(o->vol, o->blk);
}


/* ---------------------------------------------------------------- what is left of a damaged object */

/*
 * The bytes of one record saved. The record is read whole into memory
 * before it is written into the new object, so this bounds the memory a
 * salvage takes, whatever the volume's version allows a record to hold.
 */
#define SALV_REC_MAX	( 64UL * 1024 * 1024 )
#define SALV_NAME	"救出: "

/* A place as a damaged object block gives it, looked at before anything is read through it */
LOCAL BOOL salv_place( CONST UB *pl, UD total, UD max )
{
	UD	bytes = rd64(pl + PL_BYTES), a = rd64(pl + PL_A), b = rd64(pl + PL_B);
	UW	off = rd16(pl + PL_A), units = rd16(pl + PL_A + 2);

	if ( bytes > max ) {
		return FALSE;
	}
	switch ( pl[PL_FORM] ) {
	case F_NONE:
		return (BOOL)( bytes == 0 );
	case F_INLINE:
		return (BOOL)( off >= OB_INLINE && (UD)off + bytes <= TSFSBLK_BLOCK_SIZE
			       && (UD)units * TSFSO_INLINE_UNIT >= bytes );
	case F_RUN:
		return (BOOL)( a > 0 && b > 0 && a < total && b <= total - a
			       && b * TSFSBLK_BLOCK_SIZE >= bytes );
	case F_TREE:
	case F_TREE2:
	case F_TREEN:
		return (BOOL)( a > 0 && a < total );
	default:
		return FALSE;
	}
}

/*
 * An object block that does not pass its check, read for what can still
 * be had (design 7.5): where its metadata and its placement table are
 * is taken from the block as it is, each place looked at before it is
 * followed, and every record that reads whole is written into a new
 * object named after the old one (救出: ...). The management parts in
 * blocks of their own still have to pass their own checks. The resources
 * and the links of the old object are not carried over. Answers the new
 * object and how many records it got; E_OBJ when nothing could be read.
 */
EXPORT ER ts_obj_salvage( ID vol, UD blk, TS_UUID *p_new, INT *p_recs )
{
	T_TSFSBLK_SB	*sb;
	OBJ		o;
	RECE		*ord = NULL;
	UB		*tab = NULL, *j = NULL, *json = NULL, *data, *name = NULL;
	SZ		jlen = 0, asz;
	TS_UUID		u;
	INT		ntab = 0, nord = 0, i, k, got = 0, pos, nl = 0, at;
	UD		total;
	ER		er;

	*p_recs = 0;
	sb = (T_TSFSBLK_SB *)Kmalloc(sizeof(T_TSFSBLK_SB));
	o.hdr = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( sb == NULL || o.hdr == NULL ) {
		er = E_NOMEM;
		goto exit;
	}
	er = ts_ref_vol_blk(vol, sb);
	if ( er >= E_OK ) er = ts_jrnl_read(vol, blk, o.hdr);
	if ( er < E_OK ) {
		goto exit;
	}
	total = sb->total_blocks;
	o.vol = vol;
	o.blk = blk;
	o.owner = rd64(o.hdr + OB_UUID + 8);
	obj_lim(&o);

	/* the name, whole, when the metadata still reads: never longer than the metadata */
	if ( salv_place(o.hdr + OB_META, total, ( o.lim.meta_max != 0 ) ? o.lim.meta_max : (UD)SALV_REC_MAX) ) {
		j = meta_all(&o, 0, &jlen);
		if ( j != NULL ) {
			T_JSON	root;

			if ( js_span(j, (INT)jlen, 0, &root) >= E_OK && js_type(&root) == JS_OBJECT
			  && ( name = (UB *)Kmalloc(jlen + 1) ) != NULL ) {
				nl = js_get_str(&root, "name", name, (INT)jlen + 1);
				if ( nl < 0 ) nl = 0;
				name[nl] = 0;
			}
		}
	}
	/* the records, as the placement table says */
	if ( !over(o.lim.max_rec, rd32(o.hdr + OB_NPLACE))
	  && salv_place(o.hdr + OB_PLACE, total, (UD)rd32(o.hdr + OB_NPLACE) * PE_SIZE)
	  && PLACE_LOAD(&o, &tab, &ntab) >= E_OK && ntab > 0 ) {
		ord = ord_new(ntab);
		nord = ( ord != NULL ) ? order_of(&o, tab, ntab, ord) : 0;
	}

	/* what is left out of the name below only makes it shorter */
	json = (UB *)Kmalloc(9 + sizeof(SALV_NAME) + (SZ)nl + 6 + 15 + 1);
	if ( json == NULL ) {
		er = E_NOMEM;
		goto exit;
	}
	at = 0;
	knl_memcpy(json, "{\"name\":\"" SALV_NAME, 9 + sizeof(SALV_NAME) - 1);
	at = 9 + (INT)sizeof(SALV_NAME) - 1;
	for ( i = 0; i < nl; i++ ) {
		if ( name[i] == '"' || name[i] == '\\' || name[i] < 0x20 ) continue;
		json[at++] = name[i];
	}
	if ( nl == 0 ) {
		knl_memcpy(json + at, "実身", 6);
		at += 6;
	}
	knl_memcpy(json + at, "\",\"refCount\":0}", 15);
	at += 15;
	if ( nord == 0 && j == NULL ) {
		er = E_OBJ;			/* nothing of it reads */
		goto exit;
	}
	er = ts_gen_uuid(&u);
	if ( er >= E_OK ) er = obj_make(vol, &u, json, at, NULL, 0);
	if ( er < E_OK ) {
		goto exit;
	}
	for ( i = 0; i < nord; i++ ) {
		UB	*e;
		UD	bytes;

		for ( k = 0; k < ntab && rd32(tab + k * PE_SIZE + PE_RID) != ord[i].rid; k++ ) ;
		if ( k == ntab ) continue;
		e = tab + k * PE_SIZE;
		bytes = rd64(e + PE_BYTES);
		if ( !salv_place(e + PE_PLACE, total, SALV_REC_MAX) || rd64(e + PE_PLACE + PL_BYTES) != bytes ) {
			continue;
		}
		data = (UB *)Kmalloc((SZ)bytes + 1);
		if ( data == NULL ) continue;
		asz = 0;
		if ( ( bytes == 0 || ( dseg_read(&o, e + PE_PLACE, 0, data, (SZ)bytes, &asz) >= E_OK
				       && (UD)asz == bytes ) )
		  && ts_obj_ins_rec(vol, &u, -1, ord[i].rt, ord[i].sub, &pos) >= E_OK
		  && ( bytes == 0 || ts_obj_rpl_rec(vol, &u, pos, data, (SZ)bytes) >= E_OK ) ) {
			got++;
		}
		Kfree(data);
	}
	*p_new = u;
	*p_recs = got;
	er = E_OK;

    exit:
	if ( j != NULL ) Kfree(j);
	if ( tab != NULL ) Kfree(tab);
	if ( json != NULL ) Kfree(json);
	if ( name != NULL ) Kfree(name);
	if ( ord != NULL ) Kfree(ord);
	if ( o.hdr != NULL ) Kfree(o.hdr);
	if ( sb != NULL ) Kfree(sb);
	return er;
}

EXPORT ER ts_obj_exists( ID vol, CONST TS_UUID *uuid )
{
	UD	blk = 0;
	ER	er;

	if ( uuid == NULL ) {
		return E_PAR;
	}
	er = ts_btree_lookup(vol, uuid, &blk);
	return ( er >= E_OK && blk == 0 ) ? E_OBJ : er;
}

EXPORT ER ts_obj_ref( ID vol, CONST TS_UUID *uuid, T_ROBJ *pk_robj )
{
	OBJ	o;
	UB	*tab = NULL;
	INT	i, n = 0;
	ER	er;

	if ( pk_robj == NULL ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		return er;
	}
	pk_robj->uuid   = *uuid;
	pk_robj->refcnt = (INT)rd32(o.hdr + OB_REFCNT);
	pk_robj->flags  = (UINT)rd32(o.hdr + OB_FLAGS) | TSFSO_F_AUTOREF;
	pk_robj->nrec = (INT)rd32(o.hdr + OB_NPLACE);
	pk_robj->size = 0;
	er = PLACE_LOAD(&o, &tab, &n);
	if ( er >= E_OK ) {
		for ( i = 0; i < n; i++ ) {
			pk_robj->size += rd64(tab + i * PE_SIZE + PE_BYTES);
		}
		Kfree(tab);
	}
	obj_drop(&o);
	return er;
}

/* Objects in UUID order, which is the order they were made in */
LOCAL ER tree_list( ID vol, UINT tree, CONST TS_UUID *from,
		    TS_UUID *buf, INT n, INT *p_cnt )
{
	TS_UUID	key, prev;
	UD	value = 0;
	INT	got = 0;
	ER	er;

	if ( buf == NULL || n <= 0 || p_cnt == NULL ) {
		return E_PAR;
	}
	if ( from == NULL ) {
		er = ts_btree_first_t(vol, tree, &key, &value);
	} else {
		er = ts_btree_next_t(vol, tree, from, &key, &value);
	}
	while ( er >= E_OK && got < n ) {
		buf[got++] = key;
		prev = key;
		er = ts_btree_next_t(vol, tree, &prev, &key, &value);
	}
	*p_cnt = got;

	return ( er == E_NOEXS || er >= E_OK ) ? E_OK : er;
}

EXPORT ER ts_obj_list( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n,
		       INT *p_cnt )
{
	return tree_list(vol, TSFSBT_OBJ, from, buf, n, p_cnt);
}

EXPORT ER ts_obj_gc_list( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n,
			  INT *p_cnt )
{
	return tree_list(vol, TSFSBT_GC, from, buf, n, p_cnt);
}

/* ---------------------------------------------------------------- counting */

/*
 * The count moved by links and by holds, inside the caller's
 * transaction. The count is never less than the holds: one that would
 * be is a count gone wrong, which fsck puts right.
 *
 * An object whose count falls to zero goes on the garbage list, and one
 * that is referred to again comes off it. Nothing is freed here: that is
 * what collecting is for (design 11.7).
 */
LOCAL ER ref_move( ID vol, CONST TS_UUID *uuid, INT dlinks, INT dpins )
{
	OBJ	o;
	UINT	flags;
	INT	cur, pins;
	ER	er;

	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		return er;
	}
	pins = (INT)rd32(o.hdr + OB_PINS) + dpins;
	cur = (INT)rd32(o.hdr + OB_REFCNT) + dlinks + dpins;
	if ( pins < 0 ) {
		er = E_OBJ;			/* nothing held it */
		goto exit;
	}
	if ( cur < pins ) {
		cur = pins;
	}
	flags = (UINT)rd32(o.hdr + OB_FLAGS);
	if ( cur == 0 && ( flags & ( TSFSO_F_GARBAGE | TSFSO_F_ORPHAN ) ) == 0 ) {
		er = ts_btree_insert_t(vol, TSFSBT_GC, uuid, o.blk);
		if ( er < E_OK ) goto exit;
		flags |= TSFSO_F_GARBAGE;
	} else if ( cur > 0 && ( flags & TSFSO_F_GARBAGE ) != 0 ) {
		er = ts_btree_delete_t(vol, TSFSBT_GC, uuid);
		if ( er < E_OK && er != E_NOEXS ) goto exit;
		flags &= ~(UINT)TSFSO_F_GARBAGE;
		er = E_OK;
	}
	wr32(o.hdr + OB_REFCNT, (UW)cur);
	wr32(o.hdr + OB_PINS, (UW)pins);
	wr32(o.hdr + OB_FLAGS, (UW)flags);
	er = obj_store(&o);

    exit:
	obj_drop(&o);
	return er;
}

LOCAL ER refcnt_add( ID vol, CONST TS_UUID *uuid, INT delta )
{
	ER	er;

	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	return txn_end(vol, ref_move(vol, uuid, 0, delta));
}

EXPORT ER ts_obj_link( ID vol, CONST TS_UUID *uuid )
{
	return refcnt_add(vol, uuid, 1);
}

EXPORT ER ts_obj_unlink( ID vol, CONST TS_UUID *uuid )
{
	return refcnt_add(vol, uuid, -1);
}

/*
 * The count put right by the check of a volume: set to what the link
 * tables and the holds make it (never below the holds), the garbage
 * list and the object's mark made to agree with it. One transaction.
 */
EXPORT ER ts_obj_set_ref( ID vol, CONST TS_UUID *uuid, INT cnt )
{
	OBJ	o;
	UINT	flags;
	INT	pins;
	UD	blk = 0;
	ER	er;

	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		return txn_end(vol, er);
	}
	pins = (INT)rd32(o.hdr + OB_PINS);
	if ( cnt < pins ) {
		cnt = pins;
	}
	flags = (UINT)rd32(o.hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_ORPHAN ) != 0 ) {
		er = E_OBJ;			/* deleted while open: it goes when it is closed */
	} else if ( cnt == 0 ) {
		er = ts_btree_lookup_t(vol, TSFSBT_GC, uuid, &blk);
		if ( er == E_NOEXS ) {
			er = ts_btree_insert_t(vol, TSFSBT_GC, uuid, o.blk);
		} else if ( er >= E_OK && blk != o.blk ) {
			er = ts_btree_update_t(vol, TSFSBT_GC, uuid, o.blk);
		}
		flags |= TSFSO_F_GARBAGE;
	} else {
		er = ts_btree_delete_t(vol, TSFSBT_GC, uuid);
		if ( er == E_NOEXS ) er = E_OK;
		flags &= ~(UINT)TSFSO_F_GARBAGE;
	}
	if ( er >= E_OK ) {
		wr32(o.hdr + OB_REFCNT, (UW)cnt);
		wr32(o.hdr + OB_FLAGS, (UW)flags);
		er = obj_store(&o);
	}
	obj_drop(&o);
	return txn_end(vol, er);
}

LOCAL ER mark_relink( OBJ *o );

/*
 * The object marked for its link table to be made from its records
 * again, as a change to an xmlTAD record marks it; ts_obj_relink then
 * does the work.
 */
EXPORT ER ts_obj_mark_relink( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	ER	er;

	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = mark_relink(&o);
	if ( er >= E_OK ) er = obj_store(&o);
	obj_drop(&o);
	return txn_end(vol, er);
}

/* ---------------------------------------------------------------- links */

LOCAL void uuid_of( OBJ *o, TS_UUID *u )
{
	knl_memcpy(u->b, o->hdr + OB_UUID, 16);
}

/*
 * The object marked for its link table to be made again, and put on the
 * orphan tree so that the next mount finds it if nothing does before.
 * Inside the caller's transaction; the caller stores the object block.
 */
LOCAL ER mark_relink( OBJ *o )
{
	TS_UUID	u;
	UINT	flags = (UINT)rd32(o->hdr + OB_FLAGS);
	ER	er;

	if ( ( flags & ( TSFSO_F_RELINK | TSFSO_F_ORPHAN ) ) != 0 ) {
		return E_OK;			/* on the tree already */
	}
	uuid_of(o, &u);
	if ( ( flags & TSFSO_F_SPARE ) != 0 ) {
		wr32(o->hdr + OB_FLAGS, (UW)( flags | TSFSO_F_RELINK ));
		return E_OK;			/* on it for its spare place */
	}
	er = ts_btree_insert_t(o->vol, TSFSBT_ORPHAN, &u, o->blk);
	if ( er < E_OK && er != E_OBJ ) {
		return er;
	}
	wr32(o->hdr + OB_FLAGS, (UW)( flags | TSFSO_F_RELINK ));
	return E_OK;
}

/* 36 letters of a UUID */
LOCAL BOOL lk_uuid( CONST UB *s, SZ n, TS_UUID *u )
{
	char	t[37];
	INT	k;

	if ( n < 36 ) {
		return FALSE;
	}
	for ( k = 0; k < 36; k++ ) {
		t[k] = (char)s[k];
	}
	t[36] = '\0';
	return (BOOL)( ts_str_to_uuid(t, u) >= E_OK );
}

/* Where the value of attribute `name` starts in the tag [a, b), or -1 */
LOCAL SZ lk_attr( CONST UB *t, SZ a, SZ b, CONST char *name )
{
	SZ	i;
	INT	k, m = str_len((CONST UB *)name);

	for ( i = a; i + m + 3 < b; i++ ) {
		if ( !is_blank(t[i]) ) {
			continue;
		}
		for ( k = 0; k < m && t[i + 1 + k] == (UB)name[k]; k++ ) ;
		if ( k == m && t[i + 1 + m] == '=' && t[i + 2 + m] == '"' ) {
			return i + 3 + m;
		}
	}
	return -1;
}

/*
 * The entries of a link table being made: as many as there are, and the
 * ones with an identity found by a hash of it, so that a virtual object
 * is one link however often its element is written, and the first
 * counts. One with no identity is always a link of its own.
 */
typedef struct {
	UB	*b;			/* the entries, LK_SIZE each */
	INT	n, cap;
	INT	*h;			/* entry + 1 by the hash of its identity, 0 for none */
	INT	hcap;			/* a power of two, more than twice the entries with one */
	INT	hn;
} LKB;

LOCAL void lkb_free( LKB *k )
{
	if ( k->b != NULL ) Kfree(k->b);
	if ( k->h != NULL ) Kfree(k->h);
	knl_memset(k, 0, sizeof(*k));
}

LOCAL BOOL lk_noid( CONST UB *e )
{
	INT	k;

	for ( k = 0; k < 16 && e[LK_VOBJ + k] == 0; k++ ) ;
	return (BOOL)( k == 16 );
}

LOCAL UW lk_hash( CONST UB *e )
{
	UW	h = 2166136261U;
	INT	k;

	for ( k = 0; k < 16; k++ ) {
		h = ( h ^ e[LK_VOBJ + k] ) * 16777619U;
	}
	return h;
}

/* The entry with the identity of `e`, or -1 */
LOCAL INT lkb_find( CONST LKB *k, CONST UB *e )
{
	UW	i;
	INT	at;

	if ( k->hcap == 0 ) {
		return -1;
	}
	for ( i = lk_hash(e) & (UW)( k->hcap - 1 ); ( at = k->h[i] ) != 0; i = ( i + 1 ) & (UW)( k->hcap - 1 ) ) {
		if ( same16(k->b + ( at - 1 ) * LK_SIZE + LK_VOBJ, e + LK_VOBJ) ) {
			return at - 1;
		}
	}
	return -1;
}

LOCAL void lkb_hput( LKB *k, INT at )
{
	UW	i;

	for ( i = lk_hash(k->b + at * LK_SIZE) & (UW)( k->hcap - 1 ); k->h[i] != 0;
	      i = ( i + 1 ) & (UW)( k->hcap - 1 ) ) ;
	k->h[i] = at + 1;
}

/* Room for one more entry, and for its identity in the hash */
LOCAL ER lkb_fit( LKB *k )
{
	INT	i;

	if ( k->n + 1 > k->cap ) {
		INT	cap = ( k->cap > 0 ) ? k->cap * 2 : 64;
		UB	*nb = (UB *)Kmalloc((SZ)cap * LK_SIZE);

		if ( nb == NULL ) {
			return E_NOMEM;
		}
		if ( k->n > 0 ) knl_memcpy(nb, k->b, (SZ)k->n * LK_SIZE);
		if ( k->b != NULL ) Kfree(k->b);
		k->b = nb;
		k->cap = cap;
	}
	if ( ( k->hn + 1 ) * 2 > k->hcap ) {
		INT	hcap = ( k->hcap > 0 ) ? k->hcap * 2 : 128;
		INT	*nh = (INT *)Kmalloc(sizeof(INT) * (SZ)hcap);

		if ( nh == NULL ) {
			return E_NOMEM;
		}
		knl_memset(nh, 0, sizeof(INT) * (SZ)hcap);
		if ( k->h != NULL ) Kfree(k->h);
		k->h = nh;
		k->hcap = hcap;
		for ( i = 0; i < k->n; i++ ) {
			if ( !lk_noid(k->b + i * LK_SIZE) ) lkb_hput(k, i);
		}
	}
	return E_OK;
}

/* An entry put at the end, unless one of its identity came before */
LOCAL ER lkb_add( LKB *k, CONST UB *e )
{
	BOOL	id = !lk_noid(e);
	ER	er;

	if ( id && lkb_find(k, e) >= 0 ) {
		return E_OK;
	}
	er = lkb_fit(k);
	if ( er < E_OK ) {
		return er;
	}
	knl_memcpy(k->b + k->n * LK_SIZE, e, LK_SIZE);
	if ( id ) {
		lkb_hput(k, k->n);
		k->hn++;
	}
	k->n++;
	return E_OK;
}

/*
 * The <link> elements of an xmlTAD text, as entries after those there
 * are. Elements and attributes are taken as words; the XML is not
 * parsed. E_LIMIT when a link comes after `max` entries (0: no limit).
 */
LOCAL ER lk_scan( CONST UB *t, SZ len, UW rid, LKB *out, UINT max )
{
	TS_UUID	target, vo;
	UB	e[LK_SIZE];
	SZ	i, j, v;
	ER	er;

	for ( i = 0; i + 5 < len; i++ ) {
		if ( t[i] != '<' || t[i + 1] != 'l' || t[i + 2] != 'i' || t[i + 3] != 'n'
		  || t[i + 4] != 'k' || !is_blank(t[i + 5]) ) {
			continue;
		}
		for ( j = i + 5; j < len && t[j] != '>'; j++ ) ;
		if ( j >= len ) {
			break;
		}
		v = lk_attr(t, i + 5, j, "id");
		if ( v >= 0 ) {
			v += ts_lnk_split(t + v, (INT)( j - v ), NULL, NULL);	/* past a volume it names */
		}
		if ( v >= 0 && lk_uuid(t + v, j - v, &target) ) {
			if ( over(max, (UD)out->n + 1) ) {
				return E_LIMIT;
			}
			knl_memset(e, 0, LK_SIZE);
			v = lk_attr(t, i + 5, j, "vobjid");
			if ( v >= 0 && lk_uuid(t + v, j - v, &vo) ) {
				knl_memcpy(e + LK_VOBJ, vo.b, 16);
			}
			knl_memcpy(e + LK_TARGET, target.b, 16);
			wr32(e + LK_RID, rid);
			er = lkb_add(out, e);
			if ( er < E_OK ) {
				return er;
			}
		}
		i = j;
	}
	return E_OK;
}

LOCAL INT lk_cmp( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < LK_KEY; i++ ) {
		if ( a[i] != b[i] ) {
			return ( a[i] < b[i] ) ? -1 : 1;
		}
	}
	return 0;
}

/* The entries in order of LK_KEY, those that are equal as they came: merged in runs twice as long each time */
LOCAL ER lk_sort( UB *t, INT n )
{
	UB	*tmp, *a = t, *b, *x;
	INT	w, lo, mid, hi, i, j, k;

	if ( n < 2 ) {
		return E_OK;
	}
	tmp = (UB *)Kmalloc((SZ)n * LK_SIZE);
	if ( tmp == NULL ) {
		return E_NOMEM;
	}
	b = tmp;
	for ( w = 1; w < n; w *= 2 ) {
		for ( lo = 0; lo < n; lo += 2 * w ) {
			mid = ( lo + w < n ) ? lo + w : n;
			hi = ( lo + 2 * w < n ) ? lo + 2 * w : n;
			for ( i = lo, j = mid, k = lo; k < hi; k++ ) {
				if ( j >= hi || ( i < mid && lk_cmp(a + i * LK_SIZE, a + j * LK_SIZE) <= 0 ) ) {
					knl_memcpy(b + k * LK_SIZE, a + i++ * LK_SIZE, LK_SIZE);
				} else {
					knl_memcpy(b + k * LK_SIZE, a + j++ * LK_SIZE, LK_SIZE);
				}
			}
		}
		x = a; a = b; b = x;
	}
	if ( a != t ) {
		knl_memcpy(t, a, (SZ)n * LK_SIZE);
	}
	Kfree(tmp);
	return E_OK;
}

/* The link table the object's xmlTAD records call for now, in order */
LOCAL ER lk_build( OBJ *o, LKB *out )
{
	RECE	*ord = NULL;
	UB	*tab = NULL, *e, *data;
	TS_UUID	t;
	UD	blk;
	SZ	got;
	INT	ntab = 0, nord = 0, i, k;
	ER	er;

	knl_memset(out, 0, sizeof(*out));
	er = PLACE_LOAD(o, &tab, &ntab);
	if ( er >= E_OK ) {
		ord = ord_new(ntab);
		if ( ord == NULL ) er = E_NOMEM;
	}
	if ( er >= E_OK ) nord = order_of(o, tab, ntab, ord);
	for ( i = 0; i < nord && er >= E_OK; i++ ) {
		if ( ord[i].rt != 1 ) {
			continue;
		}
		for ( k = 0; k < ntab && rd32(tab + k * PE_SIZE + PE_RID) != ord[i].rid; k++ ) ;
		if ( k == ntab ) {
			continue;
		}
		e = tab + k * PE_SIZE;
		if ( rd64(e + PE_BYTES) == 0 ) {
			continue;
		}
		data = (UB *)Kmalloc((SZ)rd64(e + PE_BYTES) + 1);
		if ( data == NULL ) {
			er = E_NOMEM;
			break;
		}
		got = 0;
		er = dseg_read(o, e + PE_PLACE, 0, data, (SZ)rd64(e + PE_BYTES), &got);
		if ( er >= E_OK ) er = lk_scan(data, got, ord[i].rid, out, o->lim.max_link);
		Kfree(data);
	}
	/* what is not an object of this volume is not counted here */
	for ( i = 0; i < out->n && er >= E_OK; i++ ) {
		knl_memcpy(t.b, out->b + i * LK_SIZE + LK_TARGET, 16);
		if ( ts_btree_lookup(o->vol, &t, &blk) < E_OK ) {
			wr32(out->b + i * LK_SIZE + LK_FLAGS, LK_F_EXTERNAL);
		}
	}
	if ( er >= E_OK ) er = lk_sort(out->b, out->n);
	if ( tab != NULL ) Kfree(tab);
	if ( ord != NULL ) Kfree(ord);
	if ( er < E_OK ) {
		lkb_free(out);
	}
	return er;
}

/*
 * The counts one step moves, applied once the object block is stored:
 * one entry for each object of this volume, the links to it that came
 * less those that went, as many objects as the step's transaction has
 * room to change (`room`); and the links to objects of other volumes
 * that came or went, told to the object layer afterwards
 * (ts_obj_xq_take), as many as there are.
 */
typedef struct {
	TS_UUID	t[RELINK_STEP];
	INT	d[RELINK_STEP];
	INT	n, room;
	TS_UUID	*xt;
	INT	*xd;
	INT	xn, xcap;
} DELTAS;

LOCAL DELTAS *delta_new( ID vol )
{
	DELTAS	*dl = (DELTAS *)Kmalloc(sizeof(DELTAS));
	INT	avail;

	if ( dl == NULL ) {
		return NULL;
	}
	knl_memset(dl, 0, sizeof(*dl));
	/* each object counted changes its block and perhaps the garbage
	   list; the link table and the object's own block go too */
	avail = ts_jrnl_room(vol) - STEP_KEEP - (INT)big_blocks(vol) - 2;
	dl->room = ( avail / 4 < 1 ) ? 1 : ( avail / 4 > RELINK_STEP ) ? RELINK_STEP : avail / 4;
	return dl;
}

LOCAL void delta_free( DELTAS *dl )
{
	if ( dl->xt != NULL ) Kfree(dl->xt);
	if ( dl->xd != NULL ) Kfree(dl->xd);
	Kfree(dl);
}

/* The link of entry `e` came (d +1) or went (-1): FALSE when the step has no room for it */
LOCAL BOOL delta_add( DELTAS *dl, CONST UB *e, INT d )
{
	INT	i;

	if ( ( rd32(e + LK_FLAGS) & LK_F_EXTERNAL ) == 0 ) {
		for ( i = 0; i < dl->n; i++ ) {
			if ( same16(dl->t[i].b, e + LK_TARGET) ) {
				dl->d[i] += d;
				return TRUE;
			}
		}
		if ( dl->n >= dl->room ) {
			return FALSE;
		}
		knl_memcpy(dl->t[dl->n].b, e + LK_TARGET, 16);
		dl->d[dl->n] = d;
		dl->n++;
		return TRUE;
	}
	if ( dl->xn >= dl->xcap ) {
		INT	cap = ( dl->xcap > 0 ) ? dl->xcap * 2 : 64;
		TS_UUID	*nt = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)cap);
		INT	*nd = (INT *)Kmalloc(sizeof(INT) * (SZ)cap);

		if ( nt == NULL || nd == NULL ) {
			if ( nt != NULL ) Kfree(nt);
			if ( nd != NULL ) Kfree(nd);
			return FALSE;
		}
		if ( dl->xn > 0 ) {
			knl_memcpy(nt, dl->xt, sizeof(TS_UUID) * (SZ)dl->xn);
			knl_memcpy(nd, dl->xd, sizeof(INT) * (SZ)dl->xn);
		}
		if ( dl->xt != NULL ) Kfree(dl->xt);
		if ( dl->xd != NULL ) Kfree(dl->xd);
		dl->xt = nt;
		dl->xd = nd;
		dl->xcap = cap;
	}
	knl_memcpy(dl->xt[dl->xn].b, e + LK_TARGET, 16);
	dl->xd[dl->xn] = d;
	dl->xn++;
	return TRUE;
}

/* ---------------------------------------------------------------- links to other volumes */

/*
 * The links to objects of other volumes that came and went, waiting for
 * the object layer, which keeps the record of them on the volume they
 * point at (design 8.3). Kept in memory only, in a ring made twice as
 * large each time it fills, so that taking and putting never move what
 * waits while the lock is held: what is lost -- the memory short, the
 * power cut -- is found again when the volumes are next matched against
 * one another, at attaching.
 */
#define XQ_FIRST	1024

LOCAL T_TSXQ	*xq_tbl = NULL;
LOCAL INT	xq_n = 0, xq_cap = 0, xq_head = 0;
LOCAL BOOL	xq_lost = FALSE;
LOCAL T_SPLOCK	xq_lock;
LOCAL UW	xq_init = 0;

LOCAL void xq_ready( void )
{
	UW	was = 0;

	if ( __atomic_load_n(&xq_init, __ATOMIC_ACQUIRE) == 2 ) {
		return;
	}
	if ( __atomic_compare_exchange_n(&xq_init, &was, 1, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) {
		InitSpinLock(&xq_lock);
		__atomic_store_n(&xq_init, 2, __ATOMIC_RELEASE);
		return;
	}
	while ( __atomic_load_n(&xq_init, __ATOMIC_ACQUIRE) != 2 ) ;
}

LOCAL void xq_push( ID vol, CONST TS_UUID *host, DELTAS *dl )
{
	T_TSFSBLK_SB	*sb;
	T_TSXQ		*fresh, *old;
	UINT		imask;
	INT		i, need, cap;
	BOOL		done = FALSE;

	if ( dl->xn == 0 ) {
		return;
	}
	xq_ready();
	sb = (T_TSFSBLK_SB *)Kmalloc(sizeof(T_TSFSBLK_SB));
	if ( sb == NULL || ts_ref_vol_blk(vol, sb) < E_OK ) {
		if ( sb != NULL ) Kfree(sb);
		xq_lost = TRUE;
		return;
	}
	while ( !done ) {
		ISpinLock(&xq_lock, &imask);
		need = xq_n + dl->xn;
		if ( need <= xq_cap ) {
			for ( i = 0; i < dl->xn; i++ ) {
				T_TSXQ	*q = &xq_tbl[( xq_head + xq_n ) % xq_cap];

				q->hvol = sb->vol_uuid;
				q->host = *host;
				q->target = dl->xt[i];
				q->d = dl->xd[i];
				xq_n++;
			}
			done = TRUE;
		}
		cap = xq_cap;
		ISpinUnlock(&xq_lock, &imask);
		if ( done ) {
			break;
		}
		/* a larger table, made outside the lock and put in under it */
		cap = ( cap > 0 ) ? cap * 2 : XQ_FIRST;
		while ( cap < need ) cap *= 2;
		fresh = (T_TSXQ *)Kmalloc(sizeof(T_TSXQ) * (SZ)cap);
		if ( fresh == NULL ) {
			xq_lost = TRUE;
			break;
		}
		ISpinLock(&xq_lock, &imask);
		old = NULL;
		if ( cap > xq_cap ) {
			/* what waits, in order from the start of the new ring */
			for ( i = 0; i < xq_n; i++ ) fresh[i] = xq_tbl[( xq_head + i ) % xq_cap];
			old = xq_tbl;
			xq_tbl = fresh;
			xq_cap = cap;
			xq_head = 0;
			fresh = NULL;
		}
		ISpinUnlock(&xq_lock, &imask);
		if ( old != NULL ) Kfree(old);
		if ( fresh != NULL ) Kfree(fresh);
	}
	Kfree(sb);
}

/* Whether anything waits, looked at without the lock: a quick look for one who polls */
EXPORT BOOL ts_obj_xq_any( void )
{
	return (BOOL)( __atomic_load_n(&xq_n, __ATOMIC_RELAXED) > 0
		       || __atomic_load_n(&xq_lost, __ATOMIC_RELAXED) );
}

EXPORT INT ts_obj_xq_take( T_TSXQ *buf, INT max, BOOL *p_lost )
{
	UINT	imask;
	INT	n = 0, i;

	xq_ready();
	ISpinLock(&xq_lock, &imask);
	if ( xq_tbl != NULL && buf != NULL && max > 0 ) {
		n = ( xq_n < max ) ? xq_n : max;
		for ( i = 0; i < n; i++ ) buf[i] = xq_tbl[( xq_head + i ) % xq_cap];
		xq_head = ( xq_cap > 0 ) ? ( xq_head + n ) % xq_cap : 0;
		xq_n -= n;
	}
	if ( p_lost != NULL ) {
		*p_lost = xq_lost;
		xq_lost = FALSE;
	}
	ISpinUnlock(&xq_lock, &imask);
	return n;
}

LOCAL void delta_apply( ID vol, DELTAS *dl )
{
	INT	i;

	/* one gone meanwhile has nothing to count */
	for ( i = 0; i < dl->n; i++ ) {
		if ( dl->d[i] != 0 ) (void)ref_move(vol, &dl->t[i], dl->d[i], 0);
	}
}

/* The entry of the orphan tree goes when nothing it stands for is left */
LOCAL ER orphan_off( OBJ *o, UINT flags, CONST TS_UUID *uuid )
{
	ER	er = E_OK;

	if ( ( flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK | TSFSO_F_SPARE ) ) == 0 ) {
		er = ts_btree_delete_t(o->vol, TSFSBT_ORPHAN, uuid);
		if ( er == E_NOEXS ) er = E_OK;
	}
	return er;
}

/*
 * One step of making the link table again: the table moves toward the
 * one the records call for (`now`, made before the first step: the
 * records do not change meanwhile, the volume is held), the counts of
 * what the links point at move with it, as many objects as the step's
 * transaction has room for, in one transaction. The last step takes
 * the mark off.
 */
LOCAL ER relink_step( ID vol, CONST TS_UUID *uuid, CONST LKB *now, BOOL *p_done )
{
	OBJ	o;
	DELTAS	*dl;
	UB	*old = NULL, *next = NULL, *nw = now->b;
	UINT	flags;
	INT	nold = 0, nnow = now->n, nnext = 0, a = 0, b = 0, c;
	BOOL	more = FALSE;
	ER	er;

	*p_done = TRUE;
	o.hdr = NULL;
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	dl = delta_new(vol);
	if ( dl == NULL ) {
		return txn_end(vol, E_NOMEM);
	}
	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		goto exit;
	}
	flags = (UINT)rd32(o.hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_RELINK ) == 0 || ( flags & TSFSO_F_ORPHAN ) != 0 ) {
		goto exit;			/* nothing to do, or reaping does it */
	}
	er = LINK_LOAD(&o, &old, &nold);
	if ( er >= E_OK ) {
		next = (UB *)Kmalloc((SZ)( nold + nnow + 1 ) * LK_SIZE);
		if ( next == NULL ) er = E_NOMEM;
	}
	if ( er < E_OK ) {
		goto exit;
	}
	/* both in order: a walk through the two finds what went and came */
	while ( a < nold || b < nnow ) {
		c = ( a >= nold ) ? 1 : ( b >= nnow ) ? -1 : lk_cmp(old + a * LK_SIZE, nw + b * LK_SIZE);
		if ( c == 0 ) {
			knl_memcpy(next + nnext++ * LK_SIZE, old + a * LK_SIZE, LK_SIZE);
			a++;
			b++;
		} else if ( c < 0 ) {
			if ( !delta_add(dl, old + a * LK_SIZE, -1) ) {
				knl_memcpy(next + nnext++ * LK_SIZE, old + a * LK_SIZE, LK_SIZE);
				more = TRUE;
			}
			a++;
		} else {
			if ( !over(o.lim.max_link, (UD)nnext + 1) && delta_add(dl, nw + b * LK_SIZE, 1) ) {
				knl_memcpy(next + nnext++ * LK_SIZE, nw + b * LK_SIZE, LK_SIZE);
			} else {
				more = TRUE;
			}
			b++;
		}
	}
	er = LINK_STORE(&o, next, nnext);
	if ( er >= E_OK && !more ) {
		flags &= ~(UINT)TSFSO_F_RELINK;
		wr32(o.hdr + OB_FLAGS, (UW)flags);
		er = orphan_off(&o, flags, uuid);
	}
	if ( er >= E_OK ) er = obj_store(&o);
	obj_drop(&o);
	if ( er >= E_OK ) {
		delta_apply(vol, dl);
		*p_done = (BOOL)!more;
	}

    exit:
	obj_drop(&o);
	if ( old != NULL ) Kfree(old);
	if ( next != NULL ) Kfree(next);
	er = txn_end(vol, er);
	if ( er >= E_OK ) {
		xq_push(vol, uuid, dl);
	}
	delta_free(dl);
	return er;
}

EXPORT ER ts_obj_relink( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	LKB	now;
	UINT	flags;
	BOOL	done = FALSE;
	INT	i, guard;
	ER	er;

	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		return er;
	}
	flags = (UINT)rd32(o.hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_RELINK ) == 0 || ( flags & TSFSO_F_ORPHAN ) != 0 ) {
		obj_drop(&o);
		return E_OK;			/* nothing to do, or reaping does it */
	}
	er = lk_build(&o, &now);
	/* every step moves one link at least */
	guard = 2 + now.n + (INT)rd32(o.hdr + OB_NLINK);
	obj_drop(&o);
	for ( i = 0; er >= E_OK && i < guard && !done; i++ ) {
		er = relink_step(vol, uuid, &now, &done);
	}
	lkb_free(&now);
	return er;
}

/*
 * The links of an object that is going, given up a step at a time. It
 * is marked meanwhile, so that if it does not go after all -- a cut in
 * the power, a count that rose -- its table is made again.
 */
LOCAL ER links_release( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	DELTAS	*dl;
	UB	*old = NULL;
	INT	nold = 0, i, g, guard = 1;
	ER	er = E_OK;

	for ( g = 0; g < guard && er >= E_OK; g++ ) {
		er = txn_begin(vol);
		if ( er < E_OK ) {
			break;
		}
		dl = delta_new(vol);
		if ( dl == NULL ) {
			er = txn_end(vol, E_NOMEM);
			break;
		}
		er = obj_load(vol, uuid, &o);
		if ( er >= E_OK ) er = LINK_LOAD(&o, &old, &nold);
		if ( g == 0 && er >= E_OK ) guard = nold + 2;
		if ( er >= E_OK && nold == 0 ) {
			obj_drop(&o);
			if ( old != NULL ) { Kfree(old); old = NULL; }
			er = txn_end(vol, E_OK);
			delta_free(dl);
			break;
		}
		if ( er >= E_OK ) {
			for ( i = nold - 1; i >= 0 && delta_add(dl, old + i * LK_SIZE, -1); i-- ) ;
			if ( i == nold - 1 ) er = E_NOMEM;	/* a step has room for one at least */
			nold = i + 1;
			if ( er >= E_OK ) er = mark_relink(&o);
			if ( er >= E_OK ) er = LINK_STORE(&o, old, nold);
			if ( er >= E_OK ) er = obj_store(&o);
		}
		obj_drop(&o);
		if ( old != NULL ) { Kfree(old); old = NULL; }
		if ( er >= E_OK ) {
			delta_apply(vol, dl);
		}
		er = txn_end(vol, er);
		if ( er >= E_OK ) {
			xq_push(vol, uuid, dl);
		}
		delta_free(dl);
	}
	return er;
}

/*
 * The links of an object, in the order of the link table: at most `n`
 * into buf; with buf NULL and n 0, how many there are.
 */
EXPORT ER ts_obj_lst_lnk( ID vol, CONST TS_UUID *uuid, T_RLNK *buf, INT n, INT *p_cnt )
{
	OBJ	o;
	RECE	*ord = NULL;
	UB	*lk = NULL, *tab = NULL, *e;
	INT	nlk = 0, ntab = 0, nord = 0, i, k, got = 0;
	ER	er;

	if ( p_cnt == NULL || ( buf == NULL && n != 0 ) || n < 0 || ( buf != NULL && n == 0 ) ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		return er;
	}
	if ( buf == NULL ) {
		*p_cnt = (INT)rd32(o.hdr + OB_NLINK);
		obj_drop(&o);
		return E_OK;
	}
	er = LINK_LOAD(&o, &lk, &nlk);
	if ( er >= E_OK ) er = PLACE_LOAD(&o, &tab, &ntab);
	if ( er >= E_OK ) {
		ord = ord_new(ntab);
		if ( ord == NULL ) er = E_NOMEM;
	}
	if ( er >= E_OK ) nord = order_of(&o, tab, ntab, ord);
	for ( i = 0; er >= E_OK && i < nlk && got < n; i++ ) {
		e = lk + i * LK_SIZE;
		knl_memcpy(buf[got].vobjid.b, e + LK_VOBJ, 16);
		knl_memcpy(buf[got].target.b, e + LK_TARGET, 16);
		buf[got].flags = ( ( rd32(e + LK_FLAGS) & LK_F_EXTERNAL ) != 0 ) ? TSFS_LNK_EXTERNAL : 0;
		buf[got].recno = -1;
		for ( k = 0; k < nord; k++ ) {
			if ( ord[k].rid == rd32(e + LK_RID) ) {
				buf[got].recno = k;
				break;
			}
		}
		got++;
	}
	if ( er >= E_OK ) {
		*p_cnt = got;
	}
	if ( lk != NULL ) Kfree(lk);
	if ( tab != NULL ) Kfree(tab);
	if ( ord != NULL ) Kfree(ord);
	obj_drop(&o);
	return er;
}

/* How an object goes (may_go, obj_go) */
#define GO_DELETE	0		/* ts_obj_delete */
#define GO_COLLECT	1		/* from the garbage list */
#define GO_REAP		2		/* an orphan */

/* ---------------------------------------------------------------- the spare place */

/*
 * The spare place of an object block (OB_SPARE, version 5) holds runs
 * the object owns that no record and no management part does: those a
 * change takes before it puts them in a place, or those it gave up and
 * has still to give back. It has the forms of a data part, the runs one
 * after another (its bytes say nothing), or F_TRIM: the runs of record
 * PL_A past its first PL_BYTES bytes. While it is not empty the object
 * has TSFSO_F_SPARE and is on the orphan tree, and mounting the volume
 * empties it (ts_obj_tidy). A change that uses it empties it before it
 * returns, a step -- a transaction -- at a time, each step as large as
 * the journal has room for (design 7.3).
 */
#define SPARE_GUARD	10000000	/* steps one emptying takes; each gives back a run at least */

LOCAL BOOL	spq_busy[TSFSBLK_MAX_VOL];

LOCAL SPQ *spq_of( ID vol )
{
	return vol_ok(vol) ? __atomic_load_n(&spq_tbl[vol - 1], __ATOMIC_ACQUIRE) : NULL;
}

LOCAL void spq_clear( ID vol )
{
	SPQ	*q = spq_of(vol);

	if ( q != NULL ) q->n = 0;
}

/* An object whose spare place is to be emptied once the transaction is real */
LOCAL ER spq_add( ID vol, CONST TS_UUID *u )
{
	SPQ	*q = spq_of(vol), *none = NULL;
	INT	i;

	if ( q == NULL ) {
		if ( !vol_ok(vol) ) {
			return E_ID;
		}
		q = (SPQ *)Kcalloc(1, sizeof(SPQ));
		if ( q == NULL ) {
			return E_NOMEM;
		}
		if ( !__atomic_compare_exchange_n(&spq_tbl[vol - 1], &none, q, FALSE,
						  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE) ) {
			Kfree(q);
			q = none;
		}
	}
	for ( i = 0; i < q->n; i++ ) {
		if ( ts_uuid_cmp(&q->u[i], u) == 0 ) return E_OK;
	}
	if ( q->n >= q->cap ) {
		INT	cap = ( q->cap > 0 ) ? q->cap * 2 : 8;
		TS_UUID	*nu = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)cap);

		if ( nu == NULL ) {
			return E_NOMEM;		/* left for the next mount */
		}
		if ( q->n > 0 ) knl_memcpy(nu, q->u, sizeof(TS_UUID) * (SZ)q->n);
		if ( q->u != NULL ) Kfree(q->u);
		q->u = nu;
		q->cap = cap;
	}
	q->u[q->n++] = *u;
	return E_OK;
}

/* The flag and the entry of the orphan tree, in the caller's transaction; the caller stores the block */
LOCAL ER spare_mark( OBJ *o )
{
	UINT	flags = (UINT)rd32(o->hdr + OB_FLAGS);
	TS_UUID	u;
	ER	er = E_OK;

	if ( ( flags & TSFSO_F_SPARE ) != 0 ) {
		return E_OK;
	}
	if ( ( flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK ) ) == 0 ) {
		uuid_of(o, &u);
		er = ts_btree_insert_t(o->vol, TSFSBT_ORPHAN, &u, o->blk);
		if ( er == E_OBJ ) er = E_OK;	/* on it already */
	}
	if ( er >= E_OK ) {
		wr32(o->hdr + OB_FLAGS, (UW)( flags | TSFSO_F_SPARE ));
	}
	return er;
}

LOCAL ER spare_unmark( OBJ *o )
{
	UINT	flags = (UINT)rd32(o->hdr + OB_FLAGS);
	TS_UUID	u;

	if ( ( flags & TSFSO_F_SPARE ) == 0 ) {
		return E_OK;
	}
	flags &= ~(UINT)TSFSO_F_SPARE;
	wr32(o->hdr + OB_FLAGS, (UW)flags);
	uuid_of(o, &u);
	return orphan_off(o, flags, &u);
}

/*
 * A part given up into the spare place, in the caller's transaction: put
 * there as it is when the spare place is empty, its runs put behind the
 * ones there when it is not (the nodes of its tree given back now). The
 * place is left empty. While the spare place is F_TRIM, the runs are
 * given back once the transaction is real instead.
 */
LOCAL ER spare_take( OBJ *o, UB *pl )
{
	UB	*sp = o->hdr + OB_SPARE;
	TEDIT	t;
	XTS	xs;
	TS_UUID	u;
	UD	end;
	INT	i;
	ER	er = E_OK;

	switch ( pl[PL_FORM] ) {
	case F_NONE:
		knl_memset(pl, 0, PL_SIZE);
		return E_OK;
	case F_INLINE:
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		knl_memset(pl, 0, PL_SIZE);
		return E_OK;
	default:
		break;
	}
	if ( sp[PL_FORM] == F_TRIM ) {
		return dseg_free(o, pl, TRUE);
	}
	if ( sp[PL_FORM] == F_NONE ) {
		knl_memcpy(sp, pl, PL_SIZE);
	} else {
		er = xts_new(&xs);
		if ( er >= E_OK ) er = xt_load(o, pl, &xs);
		if ( er >= E_OK ) er = tree_free(o, pl, &xs, FALSE);
		if ( er >= E_OK ) er = te_init(&t, o);
		if ( er >= E_OK ) {
			er = te_open(&t, sp);
			end = xt_blocks(t.tail.x, t.tail.n);
			for ( i = 0; i < xs.n && er >= E_OK; i++ ) {
				er = xts_add(&t.tail, end, xs.x[i].p, xs.x[i].n);
				end += xs.x[i].n;
			}
			if ( er >= E_OK ) er = te_store(&t, sp);
			if ( er >= E_OK ) wr64(sp + PL_BYTES, end * TSFSBLK_BLOCK_SIZE);
			te_done(&t);
		}
		xts_free(&xs);
	}
	if ( er >= E_OK ) {
		knl_memset(pl, 0, PL_SIZE);
		er = spare_mark(o);
	}
	if ( er >= E_OK ) {
		uuid_of(o, &u);
		(void)spq_add(o->vol, &u);
	}
	return er;
}

/*
 * Of the runs x[0..n), from the end, those past `target` that `left`
 * copies have room for, one at least: the blocks of the part to keep,
 * and in *p_cost what giving the rest back adds to the transaction.
 */
LOCAL UD cut_point( CONST XT *x, INT n, UD target, UD left, UD *p_cost )
{
	UD	keep = target, cost = 0, c, from;
	INT	i;

	for ( i = n - 1; i >= 0; i-- ) {
		if ( x[i].l + x[i].n <= target ) {
			break;
		}
		from = ( target > x[i].l ) ? target - x[i].l : 0;
		c = map_cost(x[i].p + from, x[i].n - from);
		if ( cost > 0 && cost + c > left ) {
			keep = x[i].l + x[i].n;
			break;
		}
		cost += c;
	}
	*p_cost = cost;
	return keep;
}

/*
 * The runs of a part past its first `target` blocks given back from its
 * end, as many as the running transaction has room for (of a run tree,
 * a leaf at a time), one at least: *p_done once none is left.
 */
LOCAL ER place_cut_step( OBJ *o, UB *pl, UD target, BOOL *p_done )
{
	TEDIT	t;
	XTS	xs;
	UD	have = 0, keep, cost, used = 0, left, room, from;
	INT	r;
	ER	er = E_OK;

	*p_done = FALSE;
	for ( ;; ) {
		if ( pl[PL_FORM] == F_NONE || pl[PL_FORM] == F_INLINE || pl[PL_FORM] == F_TRIM ) {
			*p_done = TRUE;
			return E_OK;
		}
		er = place_have(o, pl, &have);
		if ( er < E_OK ) {
			return er;
		}
		if ( have <= target ) {
			*p_done = TRUE;
			return E_OK;
		}
		r = ts_jrnl_room(o->vol);
		room = ( r > 0 ) ? (UD)r : 0;
		if ( used > 0 && used + 4 + STEP_KEEP > room ) {
			return E_OK;			/* the transaction is full: the next step goes on */
		}
		left = ( room > used + STEP_KEEP + XT_DEPTH_MAX + 2 ) ? room - used - STEP_KEEP - XT_DEPTH_MAX - 2 : 0;
		if ( pl[PL_FORM] == F_TREEN ) {
			er = te_init(&t, o);
			if ( er >= E_OK ) er = te_open(&t, pl);
			from = ( target > t.tail_l ) ? target : t.tail_l;
			keep = ( er >= E_OK ) ? cut_point(t.tail.x, t.tail.n, from, left, &cost) : 0;
			cost += (UD)t.depth + 2;
			te_done(&t);
			if ( er < E_OK ) {
				return er;
			}
			if ( used > 0 && used + cost + STEP_KEEP > room ) {
				return E_OK;
			}
			er = te_init(&t, o);
			if ( er >= E_OK ) er = ( keep == 0 ) ? dseg_free(o, pl, FALSE) : te_cut(&t, pl, keep);
			te_done(&t);
		} else {
			er = xts_new(&xs);
			if ( er >= E_OK ) er = xt_load(o, pl, &xs);
			keep = ( er >= E_OK ) ? cut_point(xs.x, xs.n, target, left, &cost) : 0;
			cost += 2;
			if ( er >= E_OK ) er = xt_shrink(o, &xs, keep, FALSE);
			if ( er >= E_OK ) {
				if ( xs.n == 0 ) {
					er = tree_free(o, pl, &xs, FALSE);
					pl[PL_FORM] = F_NONE;
					wr64(pl + PL_A, 0);
					wr64(pl + PL_B, 0);
				} else {
					er = xt_store(o, pl, &xs);
				}
			}
			xts_free(&xs);
		}
		if ( er < E_OK ) {
			return er;
		}
		used += cost;
	}
}

/*
 * The spare place of an object emptied, a transaction at a time; the
 * flag and the entry of the orphan tree go with the last.
 */
LOCAL ER spare_drain( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	UB	*sp, *tab = NULL, *e;
	UW	rid;
	INT	g, n = 0, k;
	BOOL	done = FALSE;
	ER	er = E_OK;

	for ( g = 0; g < SPARE_GUARD && er >= E_OK && !done; g++ ) {
		er = txn_begin(vol);
		if ( er < E_OK ) {
			break;
		}
		er = obj_load(vol, uuid, &o);
		if ( er < E_OK ) {
			(void)txn_end(vol, er);
			return ( er == E_NOEXS ) ? E_OK : er;
		}
		sp = o.hdr + OB_SPARE;
		if ( sp[PL_FORM] == F_TRIM ) {
			/* the runs of a record cut short past its bytes */
			rid = (UW)rd64(sp + PL_A);
			er = PLACE_LOAD(&o, &tab, &n);
			for ( k = 0; er >= E_OK && k < n && rd32(tab + k * PE_SIZE + PE_RID) != rid; k++ ) ;
			if ( er >= E_OK && k == n ) {
				done = TRUE;		/* the record is gone, and its runs with it */
			} else if ( er >= E_OK ) {
				e = tab + k * PE_SIZE;
				er = place_cut_step(&o, e + PE_PLACE, BLKS_FOR(rd64(sp + PL_BYTES), TSFSBLK_BLOCK_SIZE), &done);
				if ( er >= E_OK ) er = PLACE_STORE(&o, tab, n);
			}
			if ( tab != NULL ) {
				Kfree(tab);
				tab = NULL;
			}
		} else {
			er = place_cut_step(&o, sp, 0, &done);
		}
		if ( er >= E_OK && done ) {
			knl_memset(sp, 0, PL_SIZE);
			er = spare_unmark(&o);
		}
		if ( er >= E_OK ) er = obj_store(&o);
		obj_drop(&o);
		er = txn_end(vol, er);
	}
	return er;
}

/* Emptied first when a change finds it holding something: a cut in the power, or a change that failed on the way */
LOCAL ER spare_settle( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	UINT	flags = 0;

	if ( obj_load(vol, uuid, &o) >= E_OK ) {
		flags = (UINT)rd32(o.hdr + OB_FLAGS);
		obj_drop(&o);
	}
	return ( ( flags & TSFSO_F_SPARE ) != 0 ) ? spare_drain(vol, uuid) : E_OK;
}

LOCAL void spq_run( ID vol )
{
	SPQ	*q = spq_of(vol);
	TS_UUID	u;

	if ( q == NULL || q->n == 0 || spq_busy[vol - 1] || obj_trx[vol - 1] > 0 ) {
		return;
	}
	spq_busy[vol - 1] = TRUE;
	while ( q->n > 0 ) {
		u = q->u[--q->n];
		(void)spare_drain(vol, &u);
	}
	spq_busy[vol - 1] = FALSE;
}

/*
 * A part given up by a change: into the spare place when it is large
 * and the change is a transaction of its own (version 5), else given
 * back now or once the transaction is real.
 */
LOCAL ER part_give( OBJ *o, UB *pl, BOOL later )
{
	if ( o->v5 && obj_trx[o->vol - 1] == 0 && place_big(o, pl) ) {
		return spare_take(o, pl);
	}
	return dseg_free(o, pl, later);
}

/*
 * Of an object of version 5 that goes, the parts too large to give back
 * in the one transaction that takes it away -- records and resources in
 * trees or long runs, the metadata, the icon, the link table -- moved
 * one by one into the spare place and given back from there a step at a
 * time. The object is an orphan first, out of the index, so that a cut
 * in the power on the way leaves it for the next mount to finish.
 */
LOCAL BOOL strip_needed( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	UB	*tab = NULL;
	RTAB	rt;
	INT	i, n = 0;
	BOOL	need = FALSE;

	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 || obj_load(vol, uuid, &o) < E_OK ) {
		return FALSE;
	}
	if ( o.v5 ) {
		need = (BOOL)( place_big(&o, o.hdr + OB_META) || place_big(&o, o.hdr + OB_ICON)
			       || place_big(&o, o.hdr + OB_LINK) );
		if ( !need && PLACE_LOAD(&o, &tab, &n) >= E_OK ) {
			for ( i = 0; i < n && !need; i++ ) need = place_big(&o, tab + i * PE_SIZE + PE_PLACE);
			Kfree(tab);
		}
		if ( !need && res_load(&o, &rt) >= E_OK ) {
			for ( i = 0; i < rt.n && !need; i++ ) need = place_big(&o, rt.tab + i * RS_SIZE + RS_PLACE);
			res_free(&rt);
		}
	}
	obj_drop(&o);
	return need;
}

LOCAL ER may_go( OBJ *o, INT how );

LOCAL ER obj_strip( ID vol, CONST TS_UUID *uuid, INT how )
{
	OBJ	o;
	UB	*tab = NULL;
	RTAB	rt;
	UINT	flags;
	INT	i, n = 0, g;
	BOOL	found = TRUE;
	ER	er = E_OK;

	for ( g = 0; g < SPARE_GUARD && found && er >= E_OK; g++ ) {
		found = FALSE;
		er = txn_begin(vol);
		if ( er < E_OK ) {
			break;
		}
		er = obj_load(vol, uuid, &o);
		if ( er >= E_OK && g == 0 && how != GO_REAP ) {
			/* out of the index, onto the orphan tree */
			er = may_go(&o, how);
			flags = (UINT)rd32(o.hdr + OB_FLAGS);
			if ( er >= E_OK && ( flags & TSFSO_F_GARBAGE ) != 0 ) {
				(void)ts_btree_delete_t(vol, TSFSBT_GC, uuid);
			}
			if ( er >= E_OK && ( flags & ( TSFSO_F_RELINK | TSFSO_F_SPARE ) ) == 0 ) {
				er = ts_btree_insert_t(vol, TSFSBT_ORPHAN, uuid, o.blk);
			}
			if ( er >= E_OK ) er = ts_btree_delete(vol, uuid);
			flags &= ~(UINT)( TSFSO_F_GARBAGE | TSFSO_F_RELINK );
			wr32(o.hdr + OB_FLAGS, (UW)( flags | TSFSO_F_ORPHAN ));
			found = TRUE;
		}
		if ( er >= E_OK && PLACE_LOAD(&o, &tab, &n) >= E_OK ) {
			for ( i = 0; i < n && !found; i++ ) {
				UB	*e = tab + i * PE_SIZE;

				if ( place_big(&o, e + PE_PLACE) ) {
					er = spare_take(&o, e + PE_PLACE);
					wr64(e + PE_BYTES, 0);
					if ( er >= E_OK ) er = PLACE_STORE(&o, tab, n);
					found = TRUE;
				}
			}
			Kfree(tab);
			tab = NULL;
		}
		if ( er >= E_OK && !found && res_load(&o, &rt) >= E_OK ) {
			for ( i = 0; i < rt.n && !found; i++ ) {
				UB	*e = rt.tab + i * RS_SIZE;

				if ( place_big(&o, e + RS_PLACE) ) {
					er = spare_take(&o, e + RS_PLACE);
					wr64(e + RS_BYTES, 0);
					if ( er >= E_OK ) er = res_store(&o, &rt);
					found = TRUE;
				}
			}
			res_free(&rt);
		}
		if ( er >= E_OK && !found ) {
			LOCAL CONST INT parts[] = { OB_META, OB_ICON, OB_LINK };

			for ( i = 0; i < 3 && !found; i++ ) {
				if ( place_big(&o, o.hdr + parts[i]) ) {
					er = spare_take(&o, o.hdr + parts[i]);
					if ( parts[i] == OB_LINK ) wr32(o.hdr + OB_NLINK, 0);
					found = TRUE;
				}
			}
		}
		if ( er >= E_OK && found ) er = obj_store(&o);
		obj_drop(&o);
		er = txn_end(vol, er);
	}
	return er;
}

/* ---------------------------------------------------------------- going */


LOCAL ER may_go( OBJ *o, INT how )
{
	UINT	flags = (UINT)rd32(o->hdr + OB_FLAGS);

	if ( how == GO_REAP ) {
		return ( ( flags & TSFSO_F_ORPHAN ) != 0 ) ? E_OK : E_OBJ;
	}
	if ( ( flags & TSFSO_F_ORPHAN ) != 0 ) {
		return E_NOEXS;			/* deleted already */
	}
	if ( how == GO_COLLECT && ( flags & TSFSO_F_GARBAGE ) == 0 ) {
		return E_OBJ;			/* it is not on the list */
	}
	if ( (INT)rd32(o->hdr + OB_REFCNT) > 0 ) {
		return E_OBJ;			/* something still refers to it */
	}
	if ( ( flags & TSFSO_F_DELETABLE ) == 0 ) {
		return E_RONLY;			/* it says it may not go */
	}
	return E_OK;
}

/* Whether it may go, looked at in a transaction of its own */
LOCAL ER go_check( ID vol, CONST TS_UUID *uuid, INT how )
{
	OBJ	o;
	ER	er;

	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		er = may_go(&o, how);
	}
	obj_drop(&o);
	return er;
}

/*
 * What a TRASH links to, the caller's to Kfree, before its links are
 * given up: *p_n is 0 for any other object.
 */
LOCAL ER trash_reach( ID vol, CONST TS_UUID *uuid, TS_UUID **p_to, INT *p_n )
{
	OBJ	o;
	UB	*tab = NULL;
	INT	n = 0, i, k = 0;
	ER	er;

	*p_to = NULL;
	*p_n = 0;
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK && ( rd32(o.hdr + OB_FLAGS) & TSFSO_F_TRASH ) != 0 ) {
		er = LINK_LOAD(&o, &tab, &n);
		if ( er >= E_OK && n > 0 ) {
			*p_to = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
			if ( *p_to == NULL ) {
				er = E_NOMEM;
			}
			for ( i = 0; i < n && *p_to != NULL; i++ ) {
				if ( ( rd32(tab + i * LK_SIZE + LK_FLAGS) & LK_F_EXTERNAL ) == 0 ) {
					knl_memcpy((*p_to)[k++].b, tab + i * LK_SIZE + LK_TARGET, 16);
				}
			}
			*p_n = k;
		}
		if ( tab != NULL ) Kfree(tab);
	}
	obj_drop(&o);
	return ( er == E_NOMEM ) ? er : E_OK;
}

/*
 * An object that goes: its links given up first, a step at a time, then
 * everything else in one transaction, which looks again whether it may.
 * When it is a TRASH, what only it reached goes after it, groups of
 * objects linking only to one another included (ts_obj_drop_cycles).
 */
LOCAL ER obj_go( ID vol, CONST TS_UUID *uuid, INT how )
{
	OBJ	o;
	TS_UUID	*gone = NULL;
	INT	ngone = 0;
	ER	er;

	er = go_check(vol, uuid, how);
	if ( er >= E_OK && how != GO_REAP ) er = trash_reach(vol, uuid, &gone, &ngone);
	if ( er >= E_OK ) er = links_release(vol, uuid);
	if ( er >= E_OK && strip_needed(vol, uuid) ) {
		er = obj_strip(vol, uuid, how);
		if ( er >= E_OK ) {
			how = GO_REAP;		/* an orphan now */
		} else if ( er == E_OBJ || er == E_RONLY ) {
			(void)ts_obj_relink(vol, uuid);
		}
	}
	if ( er < E_OK ) {
		if ( gone != NULL ) Kfree(gone);
		return er;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		if ( gone != NULL ) Kfree(gone);
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = may_go(&o, how);
	if ( er >= E_OK ) er = obj_free(&o, uuid);
	obj_drop(&o);
	er = txn_end(vol, er);
	if ( er == E_OBJ || er == E_RONLY ) {
		(void)ts_obj_relink(vol, uuid);	/* it stays: its links count again */
	}
	if ( er >= E_OK && ngone > 0 ) {
		(void)ts_obj_drop_cycles(vol, gone, ngone);
	}
	if ( gone != NULL ) Kfree(gone);
	return er;
}

EXPORT ER ts_obj_delete( ID vol, CONST TS_UUID *uuid )
{
	return obj_go(vol, uuid, GO_DELETE);
}

/*
 * Collect one object of the garbage list. An object that says it may not
 * be deleted stays on the list, however often it is collected, so that
 * it can be found and looked at later.
 */
EXPORT ER ts_obj_gc_one( ID vol, CONST TS_UUID *uuid )
{
	return obj_go(vol, uuid, GO_COLLECT);
}

/*
 * The whole list collected. What an object that goes linked to may fall
 * to zero and come onto the list meanwhile, so the list is walked again
 * until a walk takes nothing.
 */
EXPORT ER ts_obj_gc_all( ID vol, INT *p_cnt )
{
	TS_UUID	key, cur;
	UD	value = 0;
	INT	n = 0, guard = 0, took;
	ER	er, er_next = E_NOEXS;

	do {
		took = 0;
		er = ts_btree_first_t(vol, TSFSBT_GC, &key, &value);
		for ( ; er >= E_OK && guard < TSFSO_GC_GUARD; guard++ ) {
			cur = key;
			/* the next key is taken before the object goes, because
			   the leaf it sits in may be given back with it */
			er_next = ts_btree_next_t(vol, TSFSBT_GC, &cur, &key, &value);
			if ( ts_obj_gc_one(vol, &cur) >= E_OK ) {
				took++;
			}
			er = er_next;
		}
		n += took;
	} while ( took > 0 && guard < TSFSO_GC_GUARD );
	if ( p_cnt != NULL ) {
		*p_cnt = n;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- orphans */

EXPORT ER ts_obj_orphan( ID vol, CONST TS_UUID *uuid )
{
	OBJ	o;
	UINT	flags;
	ER	er;

	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = may_go(&o, GO_DELETE);
	if ( er >= E_OK ) {
		flags = (UINT)rd32(o.hdr + OB_FLAGS);
		if ( ( flags & TSFSO_F_GARBAGE ) != 0 ) {
			ts_btree_delete_t(vol, TSFSBT_GC, uuid);
		}
		if ( ( flags & ( TSFSO_F_RELINK | TSFSO_F_SPARE ) ) == 0 ) {
			er = ts_btree_insert_t(vol, TSFSBT_ORPHAN, uuid, o.blk);
		}
		if ( er >= E_OK ) er = ts_btree_delete(vol, uuid);
		flags &= ~(UINT)( TSFSO_F_GARBAGE | TSFSO_F_RELINK );
		wr32(o.hdr + OB_FLAGS, (UW)( flags | TSFSO_F_ORPHAN ));
		if ( er >= E_OK ) er = obj_store(&o);
	}
	obj_drop(&o);
	return txn_end(vol, er);
}

EXPORT ER ts_obj_reap( ID vol, CONST TS_UUID *uuid )
{
	return obj_go(vol, uuid, GO_REAP);
}

/*
 * Every object on the orphan tree finished: an orphan taken away, one
 * with a link table to be made again given it. Answers how many there
 * were.
 */
EXPORT ER ts_obj_tidy( ID vol, INT *p_cnt )
{
	T_TSFSJRNL	jr;
	TS_UUID	key, cur;
	OBJ	o;
	UD	value = 0;
	UINT	flags;
	INT	n = 0, guard;
	ER	er, er_next;

	if ( p_cnt != NULL ) {
		*p_cnt = 0;
	}
	/* after a pretended power cut the blocks in memory never reached
	   the disk: nothing is read or finished until the next mount */
	if ( ts_jrnl_ref(vol, &jr) >= E_OK && jr.dead ) {
		return E_IO;
	}
	er = ts_btree_first_t(vol, TSFSBT_ORPHAN, &key, &value);
	for ( guard = 0; er >= E_OK && guard < TSFSO_GC_GUARD; guard++ ) {
		cur = key;
		er_next = ts_btree_next_t(vol, TSFSBT_ORPHAN, &cur, &key, &value);
		flags = 0;
		if ( obj_load(vol, &cur, &o) >= E_OK ) {
			flags = (UINT)rd32(o.hdr + OB_FLAGS);
		}
		obj_drop(&o);
		if ( ( flags & TSFSO_F_ORPHAN ) == 0 && flags != 0
		  && ts_obj_exists(vol, &cur) == E_NOEXS && txn_begin(vol) >= E_OK ) {
			/* out of the index with no mark: an orphan all the same */
			if ( obj_load(vol, &cur, &o) >= E_OK ) {
				wr32(o.hdr + OB_FLAGS, (UW)( flags | TSFSO_F_ORPHAN ));
				(void)txn_end(vol, obj_store(&o));
				flags |= TSFSO_F_ORPHAN;
			} else {
				(void)txn_end(vol, E_OBJ);
			}
			obj_drop(&o);
		}
		if ( ( flags & TSFSO_F_ORPHAN ) != 0 ) {
			(void)obj_go(vol, &cur, GO_REAP);
		} else if ( ( flags & ( TSFSO_F_RELINK | TSFSO_F_SPARE ) ) != 0 ) {
			if ( ( flags & TSFSO_F_SPARE ) != 0 ) (void)spare_drain(vol, &cur);
			if ( ( flags & TSFSO_F_RELINK ) != 0 ) (void)ts_obj_relink(vol, &cur);
		} else if ( txn_begin(vol) >= E_OK ) {
			/* nothing to finish: the entry alone is left over */
			(void)txn_end(vol, ts_btree_delete_t(vol, TSFSBT_ORPHAN, &cur));
		}
		n++;
		er = er_next;
	}
	if ( p_cnt != NULL ) {
		*p_cnt = n;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the patrol */

typedef struct {
	ID		vol;
	T_TSFSSCRUB	*pk;
} SCRUB;

LOCAL void scrub_node( void *ctx, UD blk )
{
	SCRUB	*sc = (SCRUB *)ctx;

	(void)ts_scrub_one_blk(sc->vol, blk, TSFSBLK_MAGIC_NODE, sc->pk);
}

/* Every node of a run tree under `blk` checked as it is on the medium */
LOCAL void tn_scrub( OBJ *o, UD blk, INT want, T_TSFSSCRUB *pk )
{
	UB	*buf;
	INT	i, n, lvl;

	(void)ts_scrub_one_blk(o->vol, blk, TSFSBLK_MAGIC_TBL, pk);
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL || node_read(o, blk, buf) < E_OK ) {
		if ( buf != NULL ) Kfree(buf);
		return;
	}
	n = (INT)rd32(buf + XL_N);
	lvl = (INT)rd32(buf + XN_LEVEL);
	if ( n <= XT_FAN && lvl > 0 && lvl <= XT_DEPTH_MAX && ( want < 0 || lvl == want ) ) {
		for ( i = 0; i < n; i++ ) tn_scrub(o, NE_P(buf, i), lvl - 1, pk);
	}
	Kfree(buf);
}

/* The blocks a place lies in: the nodes of its tree and management blocks checked, data read */
LOCAL void scrub_place( OBJ *o, CONST UB *pl, BOOL mgmt, BOOL data, T_TSFSSCRUB *pk )
{
	XTS	xs;
	UB	*buf;
	UD	i, k;

	switch ( pl[PL_FORM] ) {
	case F_RUN:
		break;
	case F_TREE:
		(void)ts_scrub_one_blk(o->vol, rd64(pl + PL_A), TSFSBLK_MAGIC_TBL, pk);
		break;
	case F_TREE2:
		(void)ts_scrub_one_blk(o->vol, rd64(pl + PL_A), TSFSBLK_MAGIC_TBL, pk);
		if ( xts_new(&xs) >= E_OK ) {
			if ( xt_load(o, pl, &xs) >= E_OK ) {
				for ( k = 0; k < (UD)xs.nleaf; k++ ) {
					(void)ts_scrub_one_blk(o->vol, xs.leaf[k], TSFSBLK_MAGIC_TBL, pk);
				}
			}
			xts_free(&xs);
		}
		break;
	case F_TREEN:
		tn_scrub(o, rd64(pl + PL_A), -1, pk);
		break;
	default:
		return;
	}
	if ( !mgmt && !data ) {
		return;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( xts_new(&xs) >= E_OK && buf != NULL && xt_load(o, pl, &xs) >= E_OK ) {
		for ( k = 0; k < (UD)xs.n; k++ ) {
			for ( i = 0; i < xs.x[k].n; i++ ) {
				if ( mgmt ) {
					(void)ts_scrub_one_blk(o->vol, xs.x[k].p + i, TSFSBLK_MAGIC_TBL, pk);
					continue;
				}
				pk->checked++;
				if ( knl_tsfsblk_raw_read(o->vol, xs.x[k].p + i, buf) < E_OK ) {
					ts_blk_error(o->vol, xs.x[k].p + i, TSFSBLK_ERR_IO);
					pk->bad++;
					if ( pk->first_bad == 0 ) pk->first_bad = xs.x[k].p + i;
				}
			}
		}
	}
	xts_free(&xs);
	if ( buf != NULL ) Kfree(buf);
}

EXPORT ER ts_obj_scrub( ID vol, BOOL data, T_TSFSSCRUB *pk )
{
	SCRUB	sc;
	TS_UUID	key, cur;
	OBJ	o;
	RTAB	rt;
	UB	*tab = NULL;
	UD	value = 0;
	INT	i, n, guard;
	ER	er;

	if ( pk == NULL ) {
		return E_PAR;
	}
	knl_memset(pk, 0, sizeof(*pk));
	er = ts_scrub_blk(vol, pk);
	if ( er < E_OK ) {
		return er;
	}
	sc.vol = vol;
	sc.pk = pk;
	(void)ts_btree_nodes_t(vol, TSFSBT_OBJ, scrub_node, &sc);
	(void)ts_btree_nodes_t(vol, TSFSBT_GC, scrub_node, &sc);
	(void)ts_btree_nodes_t(vol, TSFSBT_ORPHAN, scrub_node, &sc);

	er = ts_btree_first_t(vol, TSFSBT_OBJ, &key, &value);
	for ( guard = 0; er >= E_OK && guard < TSFSO_GC_GUARD; guard++ ) {
		cur = key;
		(void)ts_scrub_one_blk(vol, value, TSFSBLK_MAGIC_OBJ, pk);
		if ( obj_load(vol, &cur, &o) >= E_OK ) {
			scrub_place(&o, o.hdr + OB_META, TRUE, data, pk);
			scrub_place(&o, o.hdr + OB_ICON, TRUE, data, pk);
			scrub_place(&o, o.hdr + OB_PLACE, TRUE, data, pk);
			scrub_place(&o, o.hdr + OB_RES, TRUE, data, pk);
			scrub_place(&o, o.hdr + OB_LINK, TRUE, data, pk);
			if ( PLACE_LOAD(&o, &tab, &n) >= E_OK ) {
				for ( i = 0; i < n; i++ ) {
					scrub_place(&o, tab + i * PE_SIZE + PE_PLACE, FALSE, data, pk);
				}
				Kfree(tab);
			}
			if ( res_load(&o, &rt) >= E_OK ) {
				for ( i = 0; i < rt.n; i++ ) {
					scrub_place(&o, rt.tab + i * RS_SIZE + RS_PLACE, FALSE, data, pk);
				}
				res_free(&rt);
			}
			scrub_place(&o, o.hdr + OB_SPARE, FALSE, FALSE, pk);
		}
		obj_drop(&o);
		er = ts_btree_next_t(vol, TSFSBT_OBJ, &cur, &key, &value);
	}
	return E_OK;
}

/* ---------------------------------------------------------------- metadata */

/* The text with the structural members put in (design 11.5) */
EXPORT ER ts_obj_get_meta( ID vol, CONST TS_UUID *uuid, UB *json, SZ size,
			   SZ *p_asize )
{
	OBJ	o;
	UB	*j;
	SZ	len = 0;
	INT	n;
	ER	er;

	if ( json == NULL ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		j = meta_all(&o, 256, &len);
		if ( j == NULL ) {
			er = E_NOMEM;
		} else {
			n = meta_synth(&o, j, (INT)len, (INT)len + 256);
			if ( (SZ)n > size ) n = (INT)size;
			knl_memcpy(json, j, n);
			if ( p_asize != NULL ) *p_asize = n;
			Kfree(j);
		}
	}
	obj_drop(&o);
	return er;
}

/*
 * The text replaced. The structural members are left out, and the order
 * of the records stays as the records have it: it changes only through
 * the calls on records.
 */
EXPORT ER ts_obj_set_meta( ID vol, CONST TS_UUID *uuid, CONST UB *json,
			   SZ size )
{
	OBJ	o;
	T_TSFSLIM lim;
	UB	*j = NULL, *tab = NULL, *txt = NULL;
	RECE	*ord = NULL;
	INT	n = (INT)size, cap, ntab = 0, nord, tl;
	ER	er;

	lim_of(vol, &lim);
	if ( json == NULL || size < 0 || over(lim.meta_max, (UD)size) || size > 0x7FFFFFF0 ) {
		return E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		er = PLACE_LOAD(&o, &tab, &ntab);
		cap = n + (INT)ORDER_TEXT_MAX(ntab) + 32;
		if ( er >= E_OK ) {
			j = (UB *)Kmalloc(cap);
			txt = (UB *)Kmalloc(ORDER_TEXT_MAX(ntab));
			ord = ord_new(ntab);
		}
		if ( er < E_OK ) {
			;
		} else if ( j == NULL || txt == NULL || ord == NULL ) {
			er = E_NOMEM;
		} else {
			knl_memcpy(j, json, n);
			if ( is_object(j, n) ) {
				n = meta_strip(j, n);
				n = jx_del(j, n, "records");
				if ( ntab > 0 ) {
					nord = order_of(&o, tab, ntab, ord);
					tl = order_text(ord, nord, txt);
					n = jx_set(j, n, cap, "records", txt, tl);
					if ( n < 0 ) er = E_LIMIT;
				}
			}
			if ( er >= E_OK ) er = mseg_write(&o, o.hdr + OB_META, j, (SZ)n);
		}
		if ( j != NULL ) Kfree(j);
		if ( txt != NULL ) Kfree(txt);
		if ( ord != NULL ) Kfree(ord);
		if ( tab != NULL ) Kfree(tab);
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	obj_drop(&o);
	return txn_end(vol, er);
}

/* ---------------------------------------------------------------- the icon */

/* Where an older volume kept the icon among the resources, or -1 */
LOCAL INT icon_row( CONST UB *tab, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( rd32(tab + i * RS_SIZE + RS_OWNER) == RID_ICON ) {
			return i;
		}
	}
	return -1;
}

/*
 * The icon read whole into a buffer of its own (the caller's to Kfree):
 * its management part, or where an older volume kept it. E_NOEXS when it
 * has none.
 */
LOCAL ER icon_load( OBJ *o, UB **p_buf, SZ *p_len )
{
	UB	*pl = o->hdr + OB_ICON, *buf;
	RTAB	rt;
	SZ	len, got = 0;
	INT	at;
	ER	er;

	*p_buf = NULL;
	*p_len = 0;
	if ( pl[PL_FORM] != F_NONE ) {
		len = (SZ)rd64(pl + PL_BYTES);
		buf = (UB *)Kmalloc(( len > 0 ) ? len : 1);
		if ( buf == NULL ) {
			return E_NOMEM;
		}
		er = mseg_read(o, pl, buf, len, &got);
	} else {
		er = res_load(o, &rt);
		at = ( er >= E_OK ) ? icon_row(rt.tab, rt.n) : -1;
		if ( er >= E_OK && at < 0 ) {
			er = E_NOEXS;
		}
		if ( er < E_OK ) {
			res_free(&rt);
			return er;
		}
		len = (SZ)rd64(rt.tab + at * RS_SIZE + RS_BYTES);
		buf = (UB *)Kmalloc(( len > 0 ) ? len : 1);
		er = ( buf == NULL ) ? E_NOMEM
		   : dseg_read(o, rt.tab + at * RS_SIZE + RS_PLACE, 0, buf, len, &got);
		res_free(&rt);
		if ( buf == NULL ) {
			return er;
		}
	}
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	*p_buf = buf;
	*p_len = got;
	return E_OK;
}

/*
 * The icon: at most `size` bytes of it from `off` into buf, and in
 * *p_asize how many the icon holds from there. With size 0 buf may be
 * NULL, to ask how large it is.
 */
EXPORT ER ts_obj_get_icon_at( ID vol, CONST TS_UUID *uuid, D off, UB *buf,
			      SZ size, SZ *p_asize )
{
	OBJ	o;
	UB	*ic = NULL;
	SZ	len = 0, n;
	ER	er;

	if ( ( buf == NULL && size > 0 ) || size < 0 || off < 0 ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = icon_load(&o, &ic, &len);
	if ( er >= E_OK ) {
		n = ( off < (D)len ) ? len - (SZ)off : 0;
		if ( p_asize != NULL ) *p_asize = n;
		if ( n > size ) n = size;
		if ( n > 0 ) knl_memcpy(buf, ic + off, (INT)n);
	}
	if ( ic != NULL ) Kfree(ic);
	obj_drop(&o);
	return er;
}

EXPORT ER ts_obj_get_icon( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	return ts_obj_get_icon_at(vol, uuid, 0, buf, size, p_asize);
}

/*
 * The icon replaced whole, in one transaction; size 0 takes it away. One
 * an older volume kept among the resources goes at the same time.
 */
EXPORT ER ts_obj_set_icon( ID vol, CONST TS_UUID *uuid, CONST UB *icon, SZ size )
{
	OBJ	o;
	RTAB	rt;
	INT	at;
	BOOL	moved = FALSE;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( size < 0 || size > TSFSO_ICON_MAX || ( size > 0 && icon == NULL ) ) {
		return E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = mseg_write(&o, o.hdr + OB_ICON, icon, size);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	while ( er >= E_OK && ( at = icon_row(rt.tab, rt.n) ) >= 0 ) {
		er = part_give(&o, rt.tab + at * RS_SIZE + RS_PLACE, TRUE);
		res_remove(&rt, at);
		moved = TRUE;
	}
	if ( er >= E_OK && moved ) er = res_store(&o, &rt);
	if ( er >= E_OK ) {
		obj_touch(&o);
		er = obj_store(&o);
	}
	res_free(&rt);
	obj_drop(&o);
	return txn_end(vol, er);
}

/* Whether it has an icon, and how large */
LOCAL BOOL icon_size( OBJ *o, CONST UB *tab, INT n, UD *p_size )
{
	UB	*pl = o->hdr + OB_ICON;
	INT	at;

	if ( pl[PL_FORM] != F_NONE ) {
		*p_size = rd64(pl + PL_BYTES);
		return TRUE;
	}
	at = icon_row(tab, n);
	if ( at >= 0 ) {
		*p_size = rd64(tab + at * RS_SIZE + RS_BYTES);
		return TRUE;
	}
	return FALSE;
}

EXPORT ER ts_obj_set_name( ID vol, CONST TS_UUID *uuid, CONST UB *name )
{
	OBJ	o;
	UB	*j, *t;
	SZ	len = 0;
	INT	n, tl, room;
	ER	er;

	if ( name == NULL ) {
		return E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		room = str_len(name) * 2 + 16;
		t = (UB *)Kmalloc(room);
		j = meta_all(&o, room + 16, &len);
		if ( j == NULL || t == NULL ) {
			er = E_NOMEM;
		} else {
			if ( len == 0 ) {
				j[0] = '{';
				j[1] = '}';
				len = 2;
			}
			tl = str_text(t, room, name);
			n = jx_set(j, (INT)len, (INT)len + room + 16, "name", t, tl);
			er = ( n > 0 ) ? mseg_write(&o, o.hdr + OB_META, j, (SZ)n) : E_PAR;
		}
		if ( j != NULL ) Kfree(j);
		if ( t != NULL ) Kfree(t);
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	obj_drop(&o);
	return txn_end(vol, er);
}

EXPORT ER ts_obj_set_flags( ID vol, CONST TS_UUID *uuid, UINT flags,
			    UINT mask )
{
	OBJ	o;
	UINT	cur;
	ER	er;

	/* whether an object is on the garbage list is not the caller's to
	   set: it follows the reference count */
	mask &= ~(UINT)( TSFSO_F_GARBAGE | TSFSO_F_ORPHAN | TSFSO_F_RELINK );

	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		cur = (UINT)rd32(o.hdr + OB_FLAGS);
		cur = ( cur & ~mask ) | ( flags & mask );
		wr32(o.hdr + OB_FLAGS, (UW)cur);
		obj_touch(&o);
		er = obj_store(&o);
	}
	obj_drop(&o);
	return txn_end(vol, er);
}

/* ---------------------------------------------------------------- records */

/*
 * An object with its placement table and the order of its records, as
 * the calls on records work on them.
 */
typedef struct {
	OBJ	o;
	UB	*tab;			/* the placement table, room for one more */
	INT	n;			/* its entries */
	RECE	*ord;			/* the records in order, room for one more */
	INT	nord;
} RECS;

LOCAL ER recs_open( ID vol, CONST TS_UUID *uuid, RECS *r )
{
	ER	er;

	r->tab = NULL;
	r->ord = NULL;
	r->nord = 0;
	r->n = 0;
	er = obj_load(vol, uuid, &r->o);
	if ( er >= E_OK ) er = PLACE_LOAD(&r->o, &r->tab, &r->n);
	if ( er >= E_OK ) {
		r->ord = ord_new(r->n);
		if ( r->ord == NULL ) er = E_NOMEM;
	}
	if ( er >= E_OK ) r->nord = order_of(&r->o, r->tab, r->n, r->ord);
	return er;
}

LOCAL void recs_close( RECS *r )
{
	if ( r->tab != NULL ) Kfree(r->tab);
	if ( r->ord != NULL ) Kfree(r->ord);
	r->tab = NULL;
	r->ord = NULL;
	obj_drop(&r->o);
}

/* The placement entry of the record at `pos`, or NULL */
LOCAL UB *recs_entry( RECS *r, INT pos )
{
	INT	k;

	if ( pos < 0 || pos >= r->nord ) {
		return NULL;
	}
	for ( k = 0; k < r->n; k++ ) {
		if ( rd32(r->tab + k * PE_SIZE + PE_RID) == r->ord[pos].rid ) {
			return r->tab + k * PE_SIZE;
		}
	}
	return NULL;
}

/* The table, the order when it changed, and the object block, written back */
LOCAL ER recs_store( RECS *r, BOOL order )
{
	ER	er = PLACE_STORE(&r->o, r->tab, r->n);

	if ( er >= E_OK && order ) er = order_store(&r->o, r->ord, r->nord);
	obj_touch(&r->o);
	if ( er >= E_OK ) er = obj_store(&r->o);
	return er;
}

/* The entry's size follows its place, and all is written back */
LOCAL ER recs_done( RECS *r, UB *e, ER er )
{
	if ( er >= E_OK ) {
		wr64(e + PE_BYTES, rd64(e + PE_PLACE + PL_BYTES));
		er = recs_store(r, FALSE);
	}
	return er;
}

EXPORT ER ts_obj_lst_rec( ID vol, CONST TS_UUID *uuid, T_RREC *buf, INT n,
			  INT *p_cnt )
{
	RECS	*r;
	OBJ	o;
	UB	*e;
	INT	i, got = 0;
	ER	er;

	if ( p_cnt == NULL || n < 0 || ( buf == NULL ) != ( n == 0 ) ) {
		return E_PAR;
	}
	if ( buf == NULL ) {
		/* how many there are, for the room to ask with */
		er = obj_load(vol, uuid, &o);
		if ( er >= E_OK ) *p_cnt = (INT)rd32(o.hdr + OB_NPLACE);
		obj_drop(&o);
		return er;
	}
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK ) {
		for ( i = 0; i < r->nord && got < n; i++ ) {
			e = recs_entry(r, i);
			if ( e == NULL ) { er = E_OBJ; break; }
			buf[got].recno   = i;
			buf[got].rectype = (UINT)rd32(e + PE_KIND);
			buf[got].size    = rd64(e + PE_BYTES);
			buf[got].rt      = r->ord[i].rt;
			buf[got].sub     = r->ord[i].sub;
			got++;
		}
		*p_cnt = got;
	}
	recs_close(r);
	Kfree(r);
	return er;
}

/* A new record at `pos` (the end for -1): a rid, an entry, a place in the order */
LOCAL ER recs_insert( RECS *r, INT pos, UINT kind, UINT rt, UINT sub, INT *p_pos )
{
	UB	*e;
	UW	rid;
	INT	i;

	if ( over(r->o.lim.max_rec, (UD)r->n + 1) ) {
		return E_LIMIT;
	}
	if ( pos < 0 || pos > r->nord ) pos = r->nord;
	rid = rd32(r->o.hdr + OB_NEXTRID);
	if ( rid == 0 || ( rid & RID_PLACE ) != 0 ) {
		return E_LIMIT;			/* every rid the field holds was given once */
	}
	e = r->tab + r->n * PE_SIZE;
	knl_memset(e, 0, PE_SIZE);
	wr32(e + PE_RID, rid);
	wr32(e + PE_KIND, (UW)kind);
	r->n++;
	wr32(r->o.hdr + OB_NEXTRID, rid + 1);
	for ( i = r->nord; i > pos; i-- ) r->ord[i] = r->ord[i - 1];
	r->ord[pos].rid = rid;
	r->ord[pos].rt  = (UW)rt;
	r->ord[pos].sub = (UW)sub;
	r->nord++;
	if ( p_pos != NULL ) *p_pos = pos;
	return recs_store(r, TRUE);
}

LOCAL ER recs_call_insert( ID vol, CONST TS_UUID *uuid, INT pos, UINT kind, UINT rt,
			   UINT sub, INT *p_pos )
{
	RECS	*r;
	ER	er;

	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = txn_begin(vol);
	if ( er >= E_OK ) {
		er = recs_open(vol, uuid, r);
		if ( er >= E_OK ) er = recs_insert(r, pos, kind, rt, sub, p_pos);
		recs_close(r);
		er = txn_end(vol, er);
	}
	Kfree(r);
	return er;
}

EXPORT ER ts_obj_apd_rec( ID vol, CONST TS_UUID *uuid, UINT rectype,
			  INT *p_recno )
{
	if ( rectype > TSFS_REC_BIN ) {
		return E_PAR;
	}
	return recs_call_insert(vol, uuid, -1, rectype, default_rt(rectype), 0, p_recno);
}

/* A record put at `pos`; its kind of file follows its type (RT 1 is xmlTAD) */
EXPORT ER ts_obj_ins_rec( ID vol, CONST TS_UUID *uuid, INT pos, UINT rt, UINT sub,
			  INT *p_pos )
{
	if ( rt > 31 ) {
		return E_PAR;
	}
	return recs_call_insert(vol, uuid, pos, ( rt == 1 ) ? TSFS_REC_XTAD : TSFS_REC_BIN,
				rt, sub, p_pos);
}

/* ---------------------------------------------------------------- what reading keeps */

/*
 * A read finds the object, its placement table and its order of
 * records (which is in the metadata), and the runs of the record --
 * several blocks read and the metadata parsed -- before the first byte.
 * What a read found is kept, per volume, for the records read last
 * (RC_SLOTS), and holds while the volume's generation is the one it was
 * found under: any change of any object of the volume starts and ends a
 * transaction, which moves the generation on, and what is kept is then
 * found again. A record of bytes inline in the object block is not kept.
 *
 * A record read from where the last read of it ended is read ahead: a
 * read smaller than RA_BYTES brings RA_BYTES into the slot's buffer,
 * and the next reads are served from it. The window is larger than a
 * page cache's usual one because the device is asked for as much at
 * once as it takes (design 7.3).
 */
#define RC_SLOTS	16
#define RA_BYTES	( 512 * 1024 )

typedef struct {
	BOOL	used;
	UD	gen;			/* the volume's generation it was found under */
	TS_UUID	uuid;
	INT	recno;
	UD	size;			/* bytes the record holds */
	XT	*x;			/* its runs */
	INT	n;
	UD	lo, hi;			/* the blocks of the record they cover (a run tree read in part) */
	UD	tick;			/* when it was used last */
	INT	busy;			/* reads at work on it: it is not taken for another */
	UD	next;			/* where the last read of it ended */
	UB	*ra;			/* RA_BYTES read ahead, from ra_off, ra_len of them */
	UD	ra_off;
	SZ	ra_len;
} RCSLOT;

typedef struct {
	ID	mtx;
	UD	tick;
	RCSLOT	s[RC_SLOTS];
} RCACHE;

LOCAL RCACHE	*rc_tbl[TSFSBLK_MAX_VOL];

/* The volume's table, made the first time a read asks for it */
LOCAL RCACHE *rc_of( ID vol )
{
	RCACHE	*c;
	T_CMTX	cm;

	if ( !vol_ok(vol) ) {
		return NULL;
	}
	c = __atomic_load_n(&rc_tbl[vol - 1], __ATOMIC_ACQUIRE);
	if ( c != NULL ) {
		return c;
	}
	c = (RCACHE *)Kcalloc(1, sizeof(RCACHE));
	if ( c == NULL ) {
		return NULL;
	}
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	c->mtx = tk_cre_mtx(&cm);
	if ( c->mtx <= 0 ) {
		Kfree(c);
		return NULL;
	}
	{
		RCACHE	*was = NULL;

		if ( !__atomic_compare_exchange_n(&rc_tbl[vol - 1], &was, c, FALSE,
						  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) {
			tk_del_mtx(c->mtx);	/* another made it first */
			Kfree(c);
			c = was;
		}
	}
	return c;
}

/* The slot of a record still as it was found, held busy; NULL when none. Under the lock */
LOCAL RCSLOT *rc_find( RCACHE *c, ID vol, CONST TS_UUID *uuid, INT recno )
{
	UD	g = GEN_NOW(vol);
	INT	i;

	for ( i = 0; i < RC_SLOTS; i++ ) {
		RCSLOT	*s = &c->s[i];

		if ( s->used && s->gen == g && s->recno == recno && ts_uuid_cmp(&s->uuid, uuid) == 0 ) {
			s->tick = ++c->tick;
			s->busy++;
			return s;
		}
	}
	return NULL;
}

/*
 * What a read of a record of blocks found, kept: in a slot not busy,
 * one that no longer holds or the one used longest ago.
 */
LOCAL void rc_keep( ID vol, UD gen, CONST TS_UUID *uuid, INT recno, CONST UB *pl, CONST XT *x, INT n,
		    UD lo, UD hi )
{
	RCACHE	*c = rc_of(vol);
	RCSLOT	*s = NULL;
	XT	*nx;
	INT	i;

	if ( c == NULL || pl[PL_FORM] == F_INLINE || n <= 0 ) {
		return;
	}
	nx = (XT *)Kmalloc(sizeof(XT) * (SZ)n);
	if ( nx == NULL ) {
		return;
	}
	knl_memcpy(nx, x, sizeof(XT) * (SZ)n);
	tk_loc_mtx(c->mtx, TMO_FEVR);
	for ( i = 0; i < RC_SLOTS; i++ ) {
		RCSLOT	*t = &c->s[i];

		if ( t->busy > 0 ) continue;
		if ( !t->used || t->gen != GEN_NOW(vol)
		  || ( t->recno == recno && ts_uuid_cmp(&t->uuid, uuid) == 0 ) ) {
			s = t;
			break;
		}
		if ( s == NULL || t->tick < s->tick ) s = t;
	}
	if ( s != NULL ) {
		if ( s->x != NULL ) Kfree(s->x);
		s->used = TRUE;
		s->gen = gen;
		s->uuid = *uuid;
		s->recno = recno;
		s->size = rd64(pl + PL_BYTES);
		s->x = nx;
		s->n = n;
		s->lo = lo;
		s->hi = hi;
		s->tick = ++c->tick;
		s->next = ~(UD)0;
		s->ra_len = 0;
		nx = NULL;
	}
	tk_unl_mtx(c->mtx);
	if ( nx != NULL ) Kfree(nx);
}

/*
 * A read served from what is kept: E_NOSPT when nothing is kept for
 * the record, and the caller reads it the long way.
 */
LOCAL ER rc_read( ID vol, CONST TS_UUID *uuid, INT recno, UD off, UB *dst, SZ len, SZ *p_asize )
{
	RCACHE	*c = rc_of(vol);
	RCSLOT	*s;
	SZ	got = 0;
	BOOL	ahead;
	ER	er = E_OK;

	if ( c == NULL ) {
		return E_NOSPT;
	}
	tk_loc_mtx(c->mtx, TMO_FEVR);
	s = rc_find(c, vol, uuid, recno);
	if ( s == NULL ) {
		tk_unl_mtx(c->mtx);
		return E_NOSPT;
	}
	*p_asize = 0;
	if ( off >= s->size || len <= 0 ) {
		s->busy--;
		tk_unl_mtx(c->mtx);
		return E_OK;
	}
	if ( (UD)len > s->size - off ) len = (SZ)( s->size - off );
	if ( off / TSFSBLK_BLOCK_SIZE < s->lo || BLKS_FOR(off + (UD)len, TSFSBLK_BLOCK_SIZE) > s->hi ) {
		s->busy--;			/* the runs kept do not reach there */
		tk_unl_mtx(c->mtx);
		return E_NOSPT;
	}
	/* in what was read ahead */
	if ( s->ra_len > 0 && off >= s->ra_off && off + (UD)len <= s->ra_off + (UD)s->ra_len ) {
		knl_memcpy(dst, s->ra + ( off - s->ra_off ), len);
		s->next = off + (UD)len;
		s->busy--;
		tk_unl_mtx(c->mtx);
		*p_asize = len;
		return E_OK;
	}
	/* read in order, in small pieces, by one reader: the window is filled */
	ahead = (BOOL)( off == s->next && len < RA_BYTES && s->busy == 1 );
	if ( ahead ) {
		s->ra_len = 0;			/* refilled: no other read takes from it meanwhile */
	}
	tk_unl_mtx(c->mtx);

	if ( ahead ) {
		UD	from = off - off % TSFSBLK_BLOCK_SIZE, rlen = 0;
		SZ	want = RA_BYTES;

		if ( s->ra == NULL ) s->ra = (UB *)Kmalloc(RA_BYTES);
		if ( s->ra != NULL ) {
			if ( (UD)want > s->size - from ) want = (SZ)( s->size - from );
			if ( s->hi != ~(UD)0 && (UD)want > s->hi * TSFSBLK_BLOCK_SIZE - from ) {
				want = (SZ)( s->hi * TSFSBLK_BLOCK_SIZE - from );
			}
			er = runs_read(vol, s->x, s->n, from, s->ra, want, &got);
			if ( er >= E_OK && got >= (SZ)( off - from ) + len ) {
				knl_memcpy(dst, s->ra + ( off - from ), len);
				rlen = (UD)got;
				got = len;
			} else if ( er >= E_OK ) {
				ahead = FALSE;		/* less came than asked: read it plainly */
			}
		} else {
			ahead = FALSE;
		}
		if ( rlen > 0 ) {
			tk_loc_mtx(c->mtx, TMO_FEVR);
			s->ra_off = from;
			s->ra_len = (SZ)rlen;
			tk_unl_mtx(c->mtx);
		}
	}
	if ( !ahead && er >= E_OK ) {
		er = runs_read(vol, s->x, s->n, off, dst, len, &got);
	}
	tk_loc_mtx(c->mtx, TMO_FEVR);
	if ( er >= E_OK ) s->next = off + (UD)got;
	s->busy--;
	tk_unl_mtx(c->mtx);
	if ( er >= E_OK ) *p_asize = got;
	return er;
}

EXPORT ER ts_obj_rea_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  void *buf, SZ size, SZ *p_asize )
{
	RECS	*r;
	UB	*e;
	SZ	got = 0;
	UD	gen;
	XTS	xs;
	ER	er;

	if ( buf == NULL || off < 0 ) {
		return E_PAR;
	}
	xs.x = NULL;
	xs.leaf = NULL;
	er = rc_read(vol, uuid, recno, (UD)off, (UB *)buf, size, &got);
	if ( er != E_NOSPT ) {
		if ( er >= E_OK && p_asize != NULL ) *p_asize = got;
		return er;
	}
	gen = vol_ok(vol) ? GEN_NOW(vol) : 0;
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL || xts_new(&xs) < E_OK ) {
		if ( r != NULL ) Kfree(r);
		xts_free(&xs);
		return E_NOMEM;
	}
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK ) {
		e = recs_entry(r, recno);
		if ( e == NULL ) {
			er = E_NOEXS;
		} else if ( e[PE_PLACE + PL_FORM] == F_INLINE ) {
			er = dseg_read(&r->o, e + PE_PLACE, (UD)off, (UB *)buf, size, &got);
		} else {
			UD	rsize = rd64(e + PE_PLACE + PL_BYTES), lo = 0, hi = ~(UD)0;

			if ( e[PE_PLACE + PL_FORM] == F_TREEN ) {
				/* of a run tree, the leaves over the read and the window read ahead */
				lo = (UD)off / TSFSBLK_BLOCK_SIZE;
				hi = BLKS_FOR((UD)off + (UD)size + RA_BYTES, TSFSBLK_BLOCK_SIZE);
				if ( hi > BLKS_FOR(rsize, TSFSBLK_BLOCK_SIZE) ) hi = BLKS_FOR(rsize, TSFSBLK_BLOCK_SIZE);
				if ( hi < lo ) hi = lo;
			}
			er = xt_load_range(&r->o, e + PE_PLACE, &xs, lo, hi);
			if ( er >= E_OK && (UD)off < rsize && size > 0 ) {
				er = runs_read(vol, xs.x, xs.n, (UD)off, (UB *)buf,
					       ( (UD)size > rsize - (UD)off ) ? (SZ)( rsize - (UD)off ) : size, &got);
			}
			if ( er >= E_OK && xs.n > 0 ) {
				rc_keep(vol, gen, uuid, recno, e + PE_PLACE, xs.x, xs.n,
					( e[PE_PLACE + PL_FORM] == F_TREEN ) ? xs.x[0].l : 0,
					( e[PE_PLACE + PL_FORM] == F_TREEN ) ? xt_blocks(xs.x, xs.n) : ~(UD)0);
			}
		}
		if ( er >= E_OK && p_asize != NULL ) *p_asize = got;
	}
	recs_close(r);
	Kfree(r);
	xts_free(&xs);
	return er;
}

/* The kinds of change to one record's bytes */
#define RC_WRITE	0
#define RC_REPLACE	1
#define RC_TRUNC	2

LOCAL ER rec_change( ID vol, CONST TS_UUID *uuid, INT recno, INT how, D off,
		     CONST UB *buf, SZ size )
{
	RECS	*r;
	UB	*e, fresh[PL_SIZE];
	INT	again = 0;
	ER	er;

	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
    retry:
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(r);
		return er;
	}
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK && again == 0 && obj_trx[vol - 1] == 0
	  && ( rd32(r->o.hdr + OB_FLAGS) & TSFSO_F_SPARE ) != 0 ) {
		/* what a change before left in the spare place goes first */
		recs_close(r);
		(void)txn_end(vol, E_OK);
		(void)spare_drain(vol, uuid);
		again++;
		goto retry;
	}
	e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
	if ( er >= E_OK && e == NULL ) {
		er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		switch ( how ) {
		case RC_WRITE:
			/* the bytes go down before the transaction becomes
			   real: they are not in the log */
			er = ( size > 0 ) ? dseg_write(&r->o, e + PE_PLACE, (UD)off, buf, size) : E_OK;
			break;
		case RC_REPLACE:
			/* the new bytes into a new place; the old one given
			   up only once the change is real */
			knl_memset(fresh, 0, PL_SIZE);
			r->o.direct = r->o.v5;		/* nothing reaches the new place yet */
			er = ( size > 0 ) ? dseg_write(&r->o, fresh, 0, buf, size) : E_OK;
			r->o.direct = FALSE;
			if ( er >= E_OK ) er = part_give(&r->o, e + PE_PLACE, TRUE);
			if ( er >= E_OK ) knl_memcpy(e + PE_PLACE, fresh, PL_SIZE);
			break;
		default:
			er = dseg_trunc(&r->o, e + PE_PLACE, (UD)off);
			break;
		}
		if ( er >= E_OK && r->ord[recno].rt == 1 ) {
			er = mark_relink(&r->o);
		}
		er = recs_done(r, e, er);
	}
	recs_close(r);
	Kfree(r);
	return txn_end(vol, er);
}

/*
 * A record grown by much at once -- written far past its end, or
 * lengthened by a truncation -- takes as many blocks as it grows by in
 * one change, and a change may hold only TSFSBLK_HELD_MAP blocks of the
 * free map until it is real (one maps 128MB). So what grows by more
 * than GROW_STEP goes in steps of that much, each a change of its own:
 * after each the record is whole and longer, with zeros. A change that fails part
 * of the way leaves the record as long as the steps made it. Inside a
 * transaction a caller opened itself, everything stays in that one.
 */
#define GROW_STEP	( (UD)256 * 1024 * 1024 )

/* How many bytes record `recno` holds now */
LOCAL ER rec_size( ID vol, CONST TS_UUID *uuid, INT recno, UD *p_size )
{
	RECS	*r;
	UB	*e;
	ER	er;

	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK ) {
		e = recs_entry(r, recno);
		if ( e == NULL ) {
			er = E_NOEXS;
		} else {
			*p_size = rd64(e + PE_PLACE + PL_BYTES);
		}
	}
	recs_close(r);
	Kfree(r);
	return er;
}

/* The record lengthened with zeros, GROW_STEP at a time, until `to` is less than one step away */
LOCAL ER grow_steps( ID vol, CONST TS_UUID *uuid, INT recno, UD to )
{
	UD	size = 0;
	ER	er;

	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 ) {
		return E_OK;
	}
	er = rec_size(vol, uuid, recno, &size);
	while ( er >= E_OK && to > size + GROW_STEP ) {
		size += GROW_STEP;
		er = rec_change(vol, uuid, recno, RC_TRUNC, (D)size, NULL, 0);
	}
	return er;
}

/*
 * A record replaced in steps (version 5, a transaction of its own): the
 * blocks for the new bytes taken into the spare place, as many in each
 * transaction as it has room for; the bytes written into them; then one
 * transaction makes the spare place the record's and the record's old
 * place the spare one, which is given back a step at a time after. A cut
 * in the power before the exchange leaves the old bytes, after it the
 * new; either way the next mount gives back what the spare place holds.
 * E_NOSPT when the record is replaced in one transaction instead.
 */
LOCAL ER rpl_split( ID vol, CONST TS_UUID *uuid, INT recno, CONST UB *buf, SZ size )
{
	RECS	*r;
	OBJ	o;
	TEDIT	t;
	XTS	xs;
	UB	*e, *sp, old[PL_SIZE];
	UD	need = BLKS_FOR((UD)size, TSFSBLK_BLOCK_SIZE), have = 0, used, start, got;
	INT	g, took, room;
	BOOL	go = FALSE;
	ER	er;

	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 || size <= DATA_INLINE_MAX ) {
		return E_NOSPT;
	}
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) {
		return E_NOMEM;
	}
	er = recs_open(vol, uuid, r);
	e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
	if ( e != NULL && r->o.v5 && need > big_blocks(vol) ) {
		go = TRUE;
	}
	recs_close(r);
	if ( !go ) {
		Kfree(r);
		return E_NOSPT;
	}
	er = spare_settle(vol, uuid);

	/* the blocks, into the spare place */
	for ( g = 0; er >= E_OK && have < need && g < SPARE_GUARD; g++ ) {
		er = txn_begin(vol);
		if ( er < E_OK ) {
			break;
		}
		er = obj_load(vol, uuid, &o);
		if ( er >= E_OK ) {
			sp = o.hdr + OB_SPARE;
			er = te_init(&t, &o);
			if ( er >= E_OK ) er = te_open(&t, sp);
			have = xt_blocks(t.tail.x, t.tail.n);
			room = ts_jrnl_room(vol);
			used = (UD)t.depth + 4 + XT_DEPTH_MAX;
			for ( took = 0; er >= E_OK && have < need; took++ ) {
				if ( took > 0 && used + 2 + STEP_KEEP > (UD)( ( room > 0 ) ? room : 0 ) ) {
					break;
				}
				got = 0;
				start = 0;
				if ( t.tail.n > 0 ) {
					start = t.tail.x[t.tail.n - 1].p + t.tail.x[t.tail.n - 1].n;
					if ( ts_alloc_ext_at(vol, start, need - have, &got) < E_OK ) got = 0;
				}
				if ( got == 0 ) {
					er = ts_alloc_ext(vol, need - have, &start, &got);
					if ( er < E_OK ) break;
				}
				used += map_cost(start, got);
				er = xts_add(&t.tail, have, start, got);
				if ( er < E_OK ) {
					ts_free_ext(vol, start, got);
					break;
				}
				have += got;
			}
			if ( er >= E_OK ) er = te_store(&t, sp);
			if ( er >= E_OK ) {
				wr64(sp + PL_BYTES, have * TSFSBLK_BLOCK_SIZE);
				er = spare_mark(&o);
			}
			if ( er >= E_OK ) er = obj_store(&o);
			te_done(&t);
		}
		obj_drop(&o);
		er = txn_end(vol, er);
	}

	/* the bytes, straight into them */
	if ( er >= E_OK ) er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		er = xts_new(&xs);
		if ( er >= E_OK ) er = xt_load(&o, o.hdr + OB_SPARE, &xs);
		if ( er >= E_OK && xt_blocks(xs.x, xs.n) < need ) er = E_OBJ;
		if ( er >= E_OK ) er = runs_write(&o, xs.x, xs.n, 0, buf, size, TRUE);
		xts_free(&xs);
		obj_drop(&o);
	}

	/* the exchange */
	if ( er >= E_OK ) er = txn_begin(vol);
	if ( er >= E_OK ) {
		er = recs_open(vol, uuid, r);
		e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
		if ( er >= E_OK && e == NULL ) er = E_NOEXS;
		if ( er >= E_OK ) {
			sp = r->o.hdr + OB_SPARE;
			knl_memcpy(old, e + PE_PLACE, PL_SIZE);
			knl_memcpy(e + PE_PLACE, sp, PL_SIZE);
			wr64(e + PE_PLACE + PL_BYTES, (UD)size);
			knl_memset(sp, 0, PL_SIZE);
			er = spare_take(&r->o, old);
			if ( er >= E_OK && sp[PL_FORM] == F_NONE ) er = spare_unmark(&r->o);
			if ( er >= E_OK && r->ord[recno].rt == 1 ) er = mark_relink(&r->o);
			er = recs_done(r, e, er);
		}
		recs_close(r);
		er = txn_end(vol, er);
	}
	if ( er < E_OK ) {
		(void)spare_drain(vol, uuid);	/* what was taken for the new bytes */
	}
	Kfree(r);
	return er;
}

/*
 * A record cut short in steps (version 5, a transaction of its own, a
 * tree or a long run): the first transaction gives it its new size and
 * notes in the spare place that its runs past it go (F_TRIM); they are
 * given back from its end a step at a time after, and by the next mount
 * after a cut in the power. E_NOSPT when it is cut in one transaction.
 */
LOCAL ER trn_split( ID vol, CONST TS_UUID *uuid, INT recno, UD nsize )
{
	RECS	*r;
	XTS	xs;
	UB	*e, *sp;
	BOOL	go = FALSE;
	ER	er;

	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 ) {
		return E_NOSPT;
	}
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) {
		return E_NOMEM;
	}
	er = recs_open(vol, uuid, r);
	e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
	if ( e != NULL && r->o.v5 && nsize < rd64(e + PE_BYTES) && place_big(&r->o, e + PE_PLACE) ) {
		go = TRUE;
	}
	recs_close(r);
	if ( !go ) {
		Kfree(r);
		return E_NOSPT;
	}
	er = spare_settle(vol, uuid);
	if ( er >= E_OK ) er = txn_begin(vol);
	if ( er >= E_OK ) {
		er = recs_open(vol, uuid, r);
		e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
		if ( er >= E_OK && e == NULL ) er = E_NOEXS;
		if ( er >= E_OK ) {
			/* past the end of the last block kept, zeros */
			er = xts_new(&xs);
			if ( er >= E_OK ) er = xt_load_range(&r->o, e + PE_PLACE, &xs, nsize / TSFSBLK_BLOCK_SIZE,
							     nsize / TSFSBLK_BLOCK_SIZE + 1);
			if ( er >= E_OK ) er = tail_clear(&r->o, xs.x, xs.n, nsize);
			xts_free(&xs);
			sp = r->o.hdr + OB_SPARE;
			if ( er >= E_OK && sp[PL_FORM] != F_NONE ) {
				er = E_OBJ;		/* it was emptied above */
			}
			if ( er >= E_OK ) {
				wr64(e + PE_PLACE + PL_BYTES, nsize);
				knl_memset(sp, 0, PL_SIZE);
				sp[PL_FORM] = F_TRIM;
				wr64(sp + PL_A, (UD)rd32(e + PE_RID));
				wr64(sp + PL_BYTES, nsize);
				er = spare_mark(&r->o);
			}
			if ( er >= E_OK ) er = spq_add(vol, uuid);
			if ( er >= E_OK && r->ord[recno].rt == 1 ) er = mark_relink(&r->o);
			er = recs_done(r, e, er);
		}
		recs_close(r);
		er = txn_end(vol, er);
	}
	Kfree(r);
	return er;
}

EXPORT ER ts_obj_wri_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  CONST void *buf, SZ size, SZ *p_asize )
{
	SZ	done = 0, n;
	ER	er;

	if ( buf == NULL || off < 0 ) {
		return E_PAR;
	}
	er = grow_steps(vol, uuid, recno, (UD)off);
	/* the bytes themselves: a write larger than a step, a step at a time */
	do {
		n = size - done;
		if ( (UD)n > GROW_STEP && vol_ok(vol) && obj_trx[vol - 1] == 0 ) n = (SZ)GROW_STEP;
		if ( er >= E_OK ) {
			er = rec_change(vol, uuid, recno, RC_WRITE, off + done,
					(CONST UB *)buf + done, n);
		}
		if ( er >= E_OK ) done += n;
	} while ( er >= E_OK && done < size );
	if ( er >= E_OK && p_asize != NULL ) {
		*p_asize = size;
	}
	return er;
}

EXPORT ER ts_obj_rpl_rec( ID vol, CONST TS_UUID *uuid, INT recno, CONST void *buf,
			  SZ size )
{
	ER	er;

	if ( ( buf == NULL && size > 0 ) || size < 0 ) {
		return E_PAR;
	}
	er = rpl_split(vol, uuid, recno, (CONST UB *)buf, size);
	if ( er != E_NOSPT ) {
		return er;
	}
	return rec_change(vol, uuid, recno, RC_REPLACE, 0, (CONST UB *)buf, size);
}

EXPORT ER ts_obj_trn_rec( ID vol, CONST TS_UUID *uuid, INT recno, UD newsize )
{
	ER	er = grow_steps(vol, uuid, recno, newsize);

	if ( er >= E_OK ) {
		er = trn_split(vol, uuid, recno, newsize);
		if ( er == E_NOSPT ) er = rec_change(vol, uuid, recno, RC_TRUNC, (D)newsize, NULL, 0);
	}
	return er;
}

/*
 * A record taken out from any place: its bytes, its entry, its place in
 * the order, and the resources that belong to it.
 */
EXPORT ER ts_obj_del_rec( ID vol, CONST TS_UUID *uuid, INT recno )
{
	RECS	*r;
	RTAB	rt;
	UB	*e;
	UW	rid;
	INT	i, k;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	(void)spare_settle(vol, uuid);
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(r);
		return er;
	}
	er = recs_open(vol, uuid, r);
	e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
	if ( er >= E_OK && e == NULL ) {
		er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		rid = rd32(e + PE_RID);
		er = part_give(&r->o, e + PE_PLACE, FALSE);
		if ( er >= E_OK && r->ord[recno].rt == 1 ) {
			er = mark_relink(&r->o);
		}
		if ( er >= E_OK ) er = res_load(&r->o, &rt);
		if ( er >= E_OK ) {
			INT	k;

			for ( i = 0, k = 0; i < rt.n && er >= E_OK; i++ ) {
				UB	*x = rt.tab + i * RS_SIZE;

				if ( rd32(x + RS_OWNER) == rid ) {
					er = part_give(&r->o, x + RS_PLACE, FALSE);
					continue;
				}
				if ( k != i ) knl_memcpy(rt.tab + k * RS_SIZE, x, RS_SIZE);
				k++;
			}
			rt.n = k;
			if ( er >= E_OK ) er = res_store(&r->o, &rt);
		}
		if ( er >= E_OK ) {
			k = (INT)( ( e - r->tab ) / PE_SIZE );
			for ( i = k; i < r->n - 1; i++ ) {
				knl_memcpy(r->tab + i * PE_SIZE, r->tab + ( i + 1 ) * PE_SIZE, PE_SIZE);
			}
			r->n--;
			for ( i = recno; i < r->nord - 1; i++ ) r->ord[i] = r->ord[i + 1];
			r->nord--;
			er = recs_store(r, TRUE);
		}
	}
	res_free(&rt);
	recs_close(r);
	Kfree(r);
	return txn_end(vol, er);
}

/* A record moved from one place to another; the others close up */
EXPORT ER ts_obj_mov_rec( ID vol, CONST TS_UUID *uuid, INT from, INT to )
{
	RECS	*r;
	RECE	x;
	INT	i;
	ER	er;

	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(r);
		return er;
	}
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK && ( from < 0 || from >= r->nord || to < 0 || to >= r->nord ) ) {
		er = E_NOEXS;
	}
	if ( er >= E_OK && from != to ) {
		x = r->ord[from];
		if ( from < to ) {
			for ( i = from; i < to; i++ ) r->ord[i] = r->ord[i + 1];
		} else {
			for ( i = from; i > to; i-- ) r->ord[i] = r->ord[i - 1];
		}
		r->ord[to] = x;
		er = recs_store(r, TRUE);
	}
	recs_close(r);
	Kfree(r);
	return txn_end(vol, er);
}

/* A record's type and subtype; its kind of file follows */
EXPORT ER ts_obj_set_rtp( ID vol, CONST TS_UUID *uuid, INT recno, UINT rt, UINT sub )
{
	RECS	*r;
	UB	*e;
	ER	er;

	if ( rt > 31 ) {
		return E_PAR;
	}
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(r);
		return er;
	}
	er = recs_open(vol, uuid, r);
	e = ( er >= E_OK ) ? recs_entry(r, recno) : NULL;
	if ( er >= E_OK && e == NULL ) {
		er = E_NOEXS;
	}
	if ( er >= E_OK && ( r->ord[recno].rt == 1 ) != ( rt == 1 ) ) {
		er = mark_relink(&r->o);	/* read for links, or no longer */
	}
	if ( er >= E_OK ) {
		r->ord[recno].rt = (UW)rt;
		r->ord[recno].sub = (UW)sub;
		wr32(e + PE_KIND, ( rt == 1 ) ? TSFS_REC_XTAD : TSFS_REC_BIN);
		er = recs_store(r, TRUE);
	}
	recs_close(r);
	Kfree(r);
	return txn_end(vol, er);
}

/* ---------------------------------------------------------------- resources */

/* The rid of the record at `recno`, the icon's, or the place of one not made yet */
LOCAL ER rid_of( OBJ *o, INT recno, UW *p_rid )
{
	RECE	*ord = NULL;
	UB	*tab = NULL;
	INT	n = 0, nord;
	ER	er;

	if ( recno == TSFSO_ICON_REC ) {
		*p_rid = RID_ICON;
		return E_OK;
	}
	if ( recno < 0 ) {
		return E_PAR;
	}
	er = PLACE_LOAD(o, &tab, &n);
	if ( er >= E_OK ) {
		ord = ord_new(n);
		if ( ord == NULL ) er = E_NOMEM;
	}
	if ( er >= E_OK ) {
		nord = order_of(o, tab, n, ord);
		*p_rid = ( recno < nord ) ? ord[recno].rid : ( RID_PLACE | (UW)recno );
	}
	if ( tab != NULL ) Kfree(tab);
	if ( ord != NULL ) Kfree(ord);
	return er;
}

/* The place of a record's rid among the records, or TSFSO_ICON_REC; -2 for none */
LOCAL INT recno_in( CONST RECE *ord, INT nord, UW rid )
{
	INT	i;

	if ( rid == RID_ICON ) {
		return TSFSO_ICON_REC;
	}
	if ( ( rid & RID_PLACE ) != 0 ) {
		return (INT)( rid & ~RID_PLACE );
	}
	for ( i = 0; i < nord; i++ ) {
		if ( ord[i].rid == rid ) {
			return i;
		}
	}
	return -2;
}

/*
 * Where the resource of a record and a number sits in the table, or -1.
 * One written before its record was made is named by the place, and is
 * still found by it afterwards.
 */
LOCAL INT res_find( CONST UB *tab, INT n, UW rid, INT recno, INT resno )
{
	UW	byplace = ( recno >= 0 ) ? ( RID_PLACE | (UW)recno ) : RID_ICON;
	UW	own;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		own = rd32(tab + i * RS_SIZE + RS_OWNER);
		if ( ( own == rid || own == byplace )
		  && (INT)rd32(tab + i * RS_SIZE + RS_RESNO) == resno ) {
			return i;
		}
	}
	return -1;
}

/* The extension a caller gives, measured: at most TSFSO_EXT_MAX bytes, and what the volume takes */
LOCAL ER ext_len( OBJ *o, CONST UB *ext, INT *p_len )
{
	INT	n = 0;

	while ( ext != NULL && ext[n] != 0 ) {
		if ( ++n > TSFSO_EXT_MAX ) {
			return E_PAR;
		}
	}
	if ( over(o->lim.ext_max, (UD)n) ) {
		return E_PAR;			/* longer than the volume keeps */
	}
	*p_len = n;
	return E_OK;
}

EXPORT ER ts_obj_rea_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  void *buf, SZ size, SZ *p_asize, UB *ext )
{
	return ts_obj_rea_res_at(vol, uuid, recno, resno, 0, buf, size, p_asize, ext);
}

EXPORT ER ts_obj_rea_res_at( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			     D off, void *buf, SZ size, SZ *p_asize, UB *ext )
{
	OBJ	o;
	RTAB	rt;
	UW	rid = 0;
	SZ	got = 0;
	INT	at = -1;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( buf == NULL || off < 0 ) {
		return E_PAR;
	}
	if ( recno == TSFSO_ICON_REC ) {
		/* the icon, as a resource by its old name */
		if ( resno != 0 ) {
			return E_NOEXS;
		}
		er = ts_obj_get_icon_at(vol, uuid, off, (UB *)buf, size, p_asize);
		if ( er >= E_OK && p_asize != NULL && *p_asize > size ) {
			*p_asize = size;
		}
		if ( er >= E_OK && ext != NULL ) {
			field_get((CONST UB *)"ico", 4, ext, TSFSO_EXT_LEN);
		}
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = rid_of(&o, recno, &rid);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	if ( er >= E_OK ) {
		at = res_find(rt.tab, rt.n, rid, recno, resno);
		if ( at < 0 ) er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		er = dseg_read(&o, rt.tab + at * RS_SIZE + RS_PLACE, (UD)off, (UB *)buf, size, &got);
		if ( er >= E_OK ) {
			if ( p_asize != NULL ) *p_asize = got;
			if ( ext != NULL ) (void)res_ext(&rt, o.v5, at, ext, TSFSO_EXT_LEN);
		}
	}
	res_free(&rt);
	obj_drop(&o);
	return er;
}

/*
 * A resource's extension, whole: at most `size` bytes of it into buf
 * (with its 0 when there is room), and in *p_asize how long it is, its
 * 0 not counted. With size 0 buf may be NULL, to ask only that.
 */
EXPORT ER ts_obj_res_ext( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  UB *buf, SZ size, SZ *p_asize )
{
	OBJ	o;
	RTAB	rt;
	UB	*tmp;
	UW	rid = 0;
	INT	at = -1, len;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( ( buf == NULL && size > 0 ) || size < 0 ) {
		return E_PAR;
	}
	if ( recno == TSFSO_ICON_REC ) {
		if ( resno != 0 ) return E_NOEXS;
		if ( p_asize != NULL ) *p_asize = 3;
		if ( size > 0 ) knl_memcpy(buf, "ico", ( size < 4 ) ? size : 4);
		return E_OK;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = rid_of(&o, recno, &rid);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	if ( er >= E_OK ) {
		at = res_find(rt.tab, rt.n, rid, recno, resno);
		if ( at < 0 ) er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		len = res_ext(&rt, o.v5, at, NULL, 0);
		tmp = (UB *)Kmalloc((SZ)len + 1);
		if ( tmp == NULL ) {
			er = E_NOMEM;
		} else {
			(void)res_ext(&rt, o.v5, at, tmp, len + 1);
			if ( size > 0 ) knl_memcpy(buf, tmp, ( size < (SZ)len + 1 ) ? size : (SZ)len + 1);
			if ( p_asize != NULL ) *p_asize = len;
			Kfree(tmp);
		}
	}
	res_free(&rt);
	obj_drop(&o);
	return er;
}

EXPORT ER ts_obj_wri_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  CONST UB *ext, CONST void *buf, SZ size )
{
	OBJ	o;
	RTAB	rt;
	UB	*e, fresh[PL_SIZE];
	UW	rid = 0;
	INT	at, xlen = 0;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( buf == NULL ) {
		return E_PAR;
	}
	if ( recno == TSFSO_ICON_REC ) {
		return ( resno == 0 ) ? ts_obj_set_icon(vol, uuid, (CONST UB *)buf, size) : E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = ext_len(&o, ext, &xlen);
	if ( er >= E_OK ) er = rid_of(&o, recno, &rid);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	if ( er >= E_OK ) {
		at = res_find(rt.tab, rt.n, rid, recno, resno);
		if ( at < 0 ) {
			if ( over(o.lim.max_res, (UD)rt.n + 1) ) {
				er = E_LIMIT;
			} else {
				at = rt.n++;
				e = rt.tab + at * RS_SIZE;
				knl_memset(e, 0, RS_SIZE);
				wr32(e + RS_OWNER, rid);
				wr32(e + RS_RESNO, (UW)resno);
			}
		}
		if ( er >= E_OK ) {
			/* a resource is always written whole: a new place,
			   the old one given up once the change is real */
			e = rt.tab + at * RS_SIZE;
			knl_memset(fresh, 0, PL_SIZE);
			o.direct = o.v5;
			er = ( size > 0 ) ? dseg_write(&o, fresh, 0, (CONST UB *)buf, size) : E_OK;
			o.direct = FALSE;
			if ( er >= E_OK ) er = part_give(&o, e + RS_PLACE, TRUE);
			if ( er >= E_OK ) {
				knl_memcpy(e + RS_PLACE, fresh, PL_SIZE);
				wr64(e + RS_BYTES, (UD)size);
				er = res_ext_set(&rt, o.v5, at, ext, xlen);
			}
			if ( er >= E_OK ) er = res_store(&o, &rt);
		}
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	res_free(&rt);
	obj_drop(&o);
	return txn_end(vol, er);
}

/*
 * The resources of an object, the icon first: at most `n` into buf;
 * with buf NULL and n 0, how many there are. An extension longer than
 * the room of T_TSFSRES.ext is cut there at a whole letter: extlen says
 * how long it is, and ts_obj_res_ext gives it whole.
 */
EXPORT ER ts_obj_lst_res( ID vol, CONST TS_UUID *uuid, T_TSFSRES *buf, INT n, INT *p_cnt )
{
	OBJ	o;
	RTAB	rt;
	RECE	*ord = NULL;
	UB	*tab = NULL;
	INT	i, cnt = 0, ntab = 0, nord = 0;
	UD	isz = 0;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( p_cnt == NULL || n < 0 || ( buf == NULL ) != ( n == 0 ) ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	if ( er >= E_OK && buf == NULL ) {
		/* how many there are: the icon once, wherever it is kept */
		cnt = ( icon_size(&o, rt.tab, rt.n, &isz) ) ? 1 : 0;
		for ( i = 0; i < rt.n; i++ ) {
			if ( rd32(rt.tab + i * RS_SIZE + RS_OWNER) != RID_ICON ) cnt++;
		}
		*p_cnt = cnt;
		res_free(&rt);
		obj_drop(&o);
		return E_OK;
	}
	if ( er >= E_OK ) er = PLACE_LOAD(&o, &tab, &ntab);
	if ( er >= E_OK ) {
		ord = ord_new(ntab);
		if ( ord == NULL ) er = E_NOMEM;
	}
	if ( er >= E_OK ) {
		nord = order_of(&o, tab, ntab, ord);
		if ( icon_size(&o, rt.tab, rt.n, &isz) && cnt < n ) {
			buf[cnt].recno = TSFSO_ICON_REC;
			buf[cnt].resno = 0;
			field_get((CONST UB *)"ico", 4, buf[cnt].ext, TSFSO_EXT_LEN);
			buf[cnt].extlen = 3;
			buf[cnt].size = isz;
			cnt++;
		}
		for ( i = 0; i < rt.n && cnt < n; i++ ) {
			UB	*e = rt.tab + i * RS_SIZE;

			if ( rd32(e + RS_OWNER) == RID_ICON ) {
				continue;		/* listed above */
			}
			buf[cnt].recno = recno_in(ord, nord, rd32(e + RS_OWNER));
			buf[cnt].resno = (INT)rd32(e + RS_RESNO);
			buf[cnt].extlen = (UINT)res_ext(&rt, o.v5, i, buf[cnt].ext, TSFSO_EXT_LEN);
			buf[cnt].size = rd64(e + RS_BYTES);
			cnt++;
		}
		*p_cnt = cnt;
	}
	if ( tab != NULL ) Kfree(tab);
	if ( ord != NULL ) Kfree(ord);
	res_free(&rt);
	obj_drop(&o);
	return er;
}

EXPORT ER ts_obj_del_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno )
{
	OBJ	o;
	RTAB	rt;
	UW	rid = 0;
	INT	at = -1;
	ER	er;

	knl_memset(&rt, 0, sizeof(rt));
	if ( recno == TSFSO_ICON_REC ) {
		SZ	had = 0;

		if ( resno != 0 || ts_obj_get_icon(vol, uuid, NULL, 0, &had) < E_OK ) {
			return E_NOEXS;
		}
		return ts_obj_set_icon(vol, uuid, NULL, 0);
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = rid_of(&o, recno, &rid);
	if ( er >= E_OK ) er = res_load(&o, &rt);
	if ( er >= E_OK ) {
		at = res_find(rt.tab, rt.n, rid, recno, resno);
		if ( at < 0 ) er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		er = part_give(&o, rt.tab + at * RS_SIZE + RS_PLACE, FALSE);
		if ( er >= E_OK ) {
			res_remove(&rt, at);
			er = res_store(&o, &rt);
		}
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	res_free(&rt);
	obj_drop(&o);
	return txn_end(vol, er);
}
