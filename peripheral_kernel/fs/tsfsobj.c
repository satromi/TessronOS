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

/* ---------------------------------------------------------------- layout */
/*
 *	The object block, 4096 bytes, little endian throughout.
 *
 *	   0	the common head: kind "TFOB", its own block number, the
 *		volume, the object (low 64 bits of its UUID) as owner, the
 *		transaction, the CRC
 *	  64	UUID of the object
 *	  80	flags (TSFSO_F_*)
 *	  84	reference count
 *	  88	entries of the placement table, of the resource table, of
 *		the link table (32 bits each), and the next rid
 *	 104	made, updated, read	64 bit ns since 1985-01-01 UTC
 *	 128	where the metadata text is (a place, 32 bytes)
 *	 160	where the placement table is
 *	 192	where the resource table is
 *	 224	where the link table is
 *	 256	which units of the inline area are taken (56 bits)
 *	 264	holds: the part of the count no link of the volume makes
 *	 288	where the icon is (a place, 32 bytes); all 0 when it has none
 *	 512	the inline area, 56 units of 64 bytes
 *
 *	A place (32 bytes) is one of
 *	   0	form: none, inline, a run, an extent tree
 *	   8	bytes
 *	  16	inline: offset (16 bits) and units (16 bits)
 *		a run: first block; its length at 24
 *		an extent tree: the block of its leaf; extents at 24
 *
 *	The metadata text, the icon and the tables are management parts:
 *	inline, or a run of blocks each with the common head (kind "TFTB")
 *	that goes through the journal. A volume written before the icon had
 *	a place of its own kept it as a resource of the owner all ones; it
 *	is read from there until it is next written. The bytes of a record or a resource are data:
 *	inline while they are small, else a run of plain blocks, else an
 *	extent tree, whose one leaf (kind "TFTB") lists up to 165 runs.
 *
 *	A placement table entry, 48 bytes, in the order of the rids:
 *	   0	rid
 *	   4	kind (TSFS_REC_XTAD or TSFS_REC_BIN)
 *	   8	bytes
 *	  16	where the bytes are (a place)
 *
 *	A resource table entry, 64 bytes:
 *	   0	rid of the record it belongs to; all ones for the icon; the
 *		top bit and a place for one written before its record was
 *	   4	its number within that record
 *	   8	file name extension, 16 bytes padded with NUL
 *	  24	bytes
 *	  32	where the bytes are (a place)
 *
 *	A link table entry, 48 bytes, in the order of the whole entry:
 *	   0	the link's own identity (vobjid), all 0 when it has none
 *	  16	the object it points at
 *	  32	rid of the record it is in
 *	  36	flags: 1 it points off the volume and is not counted
 *	  40	reserved
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>
#include <ts/dt.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>
#include <ts/json.h>
#include "tstdlib.h"

#define OB_UUID		64
#define OB_FLAGS	80
#define OB_REFCNT	84
#define OB_NPLACE	88
#define OB_NRES		92
#define OB_NLINK	96
#define OB_NEXTRID	100
#define OB_MADE		104
#define OB_UPDATED	112
#define OB_READ		120
#define OB_META		128
#define OB_PLACE	160
#define OB_RES		192
#define OB_LINK		224
#define OB_INLMAP	256
#define OB_PINS		264
#define OB_ICON		288
#define OB_INLINE	TSFSO_HEAD_SIZE
#define INL_UNITS	( TSFSO_INLINE_BYTES / TSFSO_INLINE_UNIT )

/* a place */
#define PL_SIZE		32
#define PL_FORM		0
#define PL_BYTES	8
#define PL_A		16
#define PL_B		24
#define F_NONE		0
#define F_INLINE	1
#define F_RUN		2
#define F_TREE		3

/* a placement table entry */
#define PE_SIZE		48
#define PE_RID		0
#define PE_KIND		4
#define PE_BYTES	8
#define PE_PLACE	16

/* a resource table entry */
#define RS_SIZE		64
#define RS_OWNER	0
#define RS_RESNO	4
#define RS_EXT		8
#define RS_BYTES	24
#define RS_PLACE	32
#define RID_ICON	0xFFFFFFFFU

/* a link table entry */
#define LK_SIZE		48
#define LK_VOBJ		0
#define LK_TARGET	16
#define LK_RID		32
#define LK_FLAGS	36
#define LK_KEY		40		/* the bytes entries are ordered by */
#define LK_F_EXTERNAL	1
#define RELINK_STEP	64		/* links one transaction changes the counts of */
#define RELINK_GUARD	1000		/* steps one object may take */
#define RID_PLACE	0x80000000U	/* owner by place: its record was not there yet */

/* a block of a management part: the common head, then the bytes */
#define MB_PAY		( TSFSBLK_BLOCK_SIZE - TSFSBLK_HDR_SIZE )

/* the leaf of an extent tree */
#define XL_N		64
#define XL_ENT		128
#define XL_ESZ		24

#define TSFSO_META_MAX	65536		/* CNF_TSFS_META_MAX (design 11.5) */
#define TSFSO_ICON_MAX	TSFS_ICON_MAX	/* bytes of an icon */
#define DATA_INLINE_MAX	2048		/* the most bytes of a record held inline */
#define TSFSO_GC_GUARD	100000		/* objects one collection walks over */
#define PEND_MAX	64		/* runs waiting for the transaction to be real */

/* Open user transactions, one slot per volume the block layer holds */
LOCAL INT	obj_trx[TSFSBLK_MAX_VOL];

/* Runs a replacement gave up, to be given back once it is real */
LOCAL UD	pend_start[TSFSBLK_MAX_VOL][PEND_MAX];
LOCAL UD	pend_count[TSFSBLK_MAX_VOL][PEND_MAX];
LOCAL INT	pend_n[TSFSBLK_MAX_VOL];

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

/* A string into a fixed field, cut to fit and padded with NUL */
LOCAL void field_put( UB *dst, INT max, CONST UB *src )
{
	INT	i;

	for ( i = 0; i < max; i++ ) {
		dst[i] = 0;
	}
	for ( i = 0; src != NULL && src[i] != '\0' && i < max - 1; i++ ) {
		dst[i] = src[i];
	}
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
} OBJ;

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

LOCAL ER pend_add( ID vol, UD start, UD count )
{
	INT	v = vol - 1;

	if ( pend_n[v] >= PEND_MAX ) {
		return E_LIMIT;
	}
	pend_start[v][pend_n[v]] = start;
	pend_count[v][pend_n[v]] = count;
	pend_n[v]++;
	return E_OK;
}

/*
 * The runs a replacement gave up, freed in a transaction of their own
 * now that the one that stopped using them is real. A cut in the power
 * between the two leaves them taken but unused, which a check of the
 * volume finds and gives back.
 */
LOCAL void pend_apply( ID vol )
{
	INT	v = vol - 1, i;

	if ( pend_n[v] == 0 ) {
		return;
	}
	if ( ts_jrnl_begin(vol) >= E_OK ) {
		for ( i = 0; i < pend_n[v]; i++ ) {
			ts_free_ext(vol, pend_start[v][i], pend_count[v][i]);
		}
		ts_jrnl_commit(vol);
	}
	pend_n[v] = 0;
}

/* ---------------------------------------------------------------- management parts */

/*
 * A management part read whole: inline, or from its run of blocks, each
 * checked. At most `max` bytes are copied; *p_len is how many it holds.
 */
LOCAL ER mseg_read( OBJ *o, CONST UB *pl, UB *out, SZ max, SZ *p_len )
{
	UB	*buf;
	UD	len = rd64(pl + PL_BYTES), start, i;
	SZ	done = 0, n;
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
		break;
	default:
		return E_OBJ;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	start = rd64(pl + PL_A);
	for ( i = 0; done < max && done < (SZ)len; i++ ) {
		er = meta_read(o->vol, start + i, buf);
		if ( er >= E_OK && ts_blk_check(o->vol, buf, TSFSBLK_MAGIC_TBL, start + i) < E_OK ) {
			er = E_IO;
		}
		if ( er < E_OK ) break;
		n = MB_PAY;
		if ( n > (SZ)len - done ) n = (SZ)len - done;
		if ( n > max - done ) n = max - done;
		knl_memcpy(out + done, buf + TSFSBLK_HDR_SIZE, (INT)n);
		done += n;
	}
	Kfree(buf);
	return er;
}

LOCAL void mseg_free( OBJ *o, UB *pl )
{
	if ( pl[PL_FORM] == F_INLINE ) {
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
	} else if ( pl[PL_FORM] == F_RUN && rd64(pl + PL_B) > 0 ) {
		ts_free_ext(o->vol, rd64(pl + PL_A), rd64(pl + PL_B));
	}
	knl_memset(pl, 0, PL_SIZE);
}

/*
 * A management part written whole: inline when it fits there, else a
 * run of blocks through the journal. A run of the right length is used
 * again; one of another length is given back and a new one taken.
 */
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

LOCAL ER mseg_write( OBJ *o, UB *pl, CONST UB *src, SZ len )
{
	UB	*buf;
	UD	start = 0, want, i;
	SZ	done = 0, n;
	INT	off;
	ER	er = E_OK;

	if ( pl[PL_FORM] == F_INLINE ) {
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		pl[PL_FORM] = F_NONE;
	}
	if ( len <= 0 ) {
		mseg_free(o, pl);
		return E_OK;
	}
	off = ( len <= TSFSO_INLINE_BYTES ) ? inl_take(o->hdr, len) : -1;
	if ( off >= 0 ) {
		mseg_free(o, pl);
		knl_memcpy(o->hdr + off, src, (INT)len);
		pl[PL_FORM] = F_INLINE;
		wr64(pl + PL_BYTES, (UD)len);
		wr16(pl + PL_A, (UH)off);
		wr16(pl + PL_A + 2, (UH)BLKS_FOR(len, TSFSO_INLINE_UNIT));
		return E_OK;
	}

	want = BLKS_FOR(len, MB_PAY);
	if ( pl[PL_FORM] == F_RUN && rd64(pl + PL_B) == want ) {
		start = rd64(pl + PL_A);
	} else {
		mseg_free(o, pl);
		er = run_take(o->vol, want, &start);
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
		ts_blk_seal(o->vol, buf, TSFSBLK_MAGIC_TBL, start + i, o->owner);
		er = meta_write(o->vol, start + i, buf);
	}
	Kfree(buf);
	pl[PL_FORM] = F_RUN;
	wr64(pl + PL_BYTES, (UD)len);
	wr64(pl + PL_A, start);
	wr64(pl + PL_B, want);
	return er;
}

/* ---------------------------------------------------------------- extents */

typedef struct {
	UD	l;			/* first block within the part */
	UD	p;			/* where it is on the volume */
	UD	n;			/* blocks */
} XT;

/* The runs a data part's bytes lie in, in order */
LOCAL ER xt_load( OBJ *o, CONST UB *pl, XT *x, INT *p_n )
{
	UB	*buf;
	INT	i, n;
	ER	er;

	*p_n = 0;
	if ( pl[PL_FORM] == F_RUN ) {
		x[0].l = 0;
		x[0].p = rd64(pl + PL_A);
		x[0].n = rd64(pl + PL_B);
		*p_n = ( x[0].n > 0 ) ? 1 : 0;
		return E_OK;
	}
	if ( pl[PL_FORM] != F_TREE ) {
		return E_OK;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = meta_read(o->vol, rd64(pl + PL_A), buf);
	if ( er >= E_OK && ts_blk_check(o->vol, buf, TSFSBLK_MAGIC_TBL, rd64(pl + PL_A)) < E_OK ) {
		er = E_IO;
	}
	n = ( er >= E_OK ) ? (INT)rd32(buf + XL_N) : 0;
	if ( n > TSFSO_MAX_EXTENTS ) {
		er = E_OBJ;
		n = 0;
	}
	for ( i = 0; i < n; i++ ) {
		x[i].l = rd64(buf + XL_ENT + i * XL_ESZ);
		x[i].p = rd64(buf + XL_ENT + i * XL_ESZ + 8);
		x[i].n = rd64(buf + XL_ENT + i * XL_ESZ + 16);
	}
	Kfree(buf);
	*p_n = n;
	return er;
}

/*
 * The runs written back: one run in the place itself, more than one in
 * the leaf of an extent tree, taken when the part first needs one and
 * given back when it no longer does.
 */
LOCAL ER xt_store( OBJ *o, UB *pl, CONST XT *x, INT n )
{
	UB	*buf;
	UD	leaf = ( pl[PL_FORM] == F_TREE ) ? rd64(pl + PL_A) : 0, got = 0;
	INT	i;
	ER	er;

	if ( n <= 1 ) {
		if ( leaf != 0 ) {
			ts_free_ext(o->vol, leaf, 1);
		}
		pl[PL_FORM] = ( n == 1 ) ? F_RUN : F_NONE;
		wr64(pl + PL_A, ( n == 1 ) ? x[0].p : 0);
		wr64(pl + PL_B, ( n == 1 ) ? x[0].n : 0);
		return E_OK;
	}
	if ( n > TSFSO_MAX_EXTENTS ) {
		return E_NOMEM;			/* the part is broken into too many runs */
	}
	if ( leaf == 0 ) {
		er = ts_alloc_ext(o->vol, 1, &leaf, &got);
		if ( er < E_OK ) {
			return er;
		}
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	wr32(buf + XL_N, (UW)n);
	for ( i = 0; i < n; i++ ) {
		wr64(buf + XL_ENT + i * XL_ESZ, x[i].l);
		wr64(buf + XL_ENT + i * XL_ESZ + 8, x[i].p);
		wr64(buf + XL_ENT + i * XL_ESZ + 16, x[i].n);
	}
	ts_blk_seal(o->vol, buf, TSFSBLK_MAGIC_TBL, leaf, o->owner);
	er = meta_write(o->vol, leaf, buf);
	Kfree(buf);
	pl[PL_FORM] = F_TREE;
	wr64(pl + PL_A, leaf);
	wr64(pl + PL_B, (UD)n);
	return er;
}

LOCAL UD xt_blocks( CONST XT *x, INT n )
{
	return ( n > 0 ) ? x[n - 1].l + x[n - 1].n : 0;
}

/* Where block `l` of the part is, and how many follow it in the same run */
LOCAL UD xt_map( CONST XT *x, INT n, UD l, UD *p_run )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( l >= x[i].l && l < x[i].l + x[i].n ) {
			if ( p_run != NULL ) *p_run = x[i].l + x[i].n - l;
			return x[i].p + ( l - x[i].l );
		}
	}
	return 0;
}

/*
 * Blocks added until the part has `need`: right behind its last run
 * when that is free, which keeps it one run, else wherever a run is.
 * The blocks added are cleared unless the caller is about to fill them:
 * [keep_from, keep_to) is what it will write whole.
 */
LOCAL ER xt_grow( OBJ *o, XT *x, INT *p_n, UD need, UD keep_from, UD keep_to )
{
	UB	*zero = NULL;
	UD	have = xt_blocks(x, *p_n), start = 0, got = 0, b;
	INT	n = *p_n;
	ER	er = E_OK;

	while ( have < need && er >= E_OK ) {
		er = E_NOMEM;
		if ( n > 0 ) {
			er = ts_alloc_ext_at(o->vol, x[n - 1].p + x[n - 1].n, need - have, &got);
			if ( er >= E_OK ) {
				start = x[n - 1].p + x[n - 1].n;
				x[n - 1].n += got;
			}
		}
		if ( er < E_OK ) {
			if ( n >= TSFSO_MAX_EXTENTS ) {
				er = E_NOMEM;
				break;
			}
			er = ts_alloc_ext(o->vol, need - have, &start, &got);
			if ( er < E_OK ) break;
			x[n].l = have;
			x[n].p = start;
			x[n].n = got;
			n++;
		}
		for ( b = 0; b < got && er >= E_OK; b++ ) {
			if ( have + b >= keep_from && have + b < keep_to ) continue;
			if ( zero == NULL ) {
				zero = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
				if ( zero == NULL ) { er = E_NOMEM; break; }
				knl_memset(zero, 0, TSFSBLK_BLOCK_SIZE);
			}
			er = ts_write_blk(o->vol, start + b, zero);
		}
		have += got;
	}
	if ( zero != NULL ) Kfree(zero);
	*p_n = n;
	return er;
}

/* Blocks past `keep` given back, now or once the transaction is real */
LOCAL ER xt_shrink( OBJ *o, XT *x, INT *p_n, UD keep, BOOL later )
{
	INT	n = *p_n;
	UD	cut;
	ER	er = E_OK;

	while ( n > 0 && xt_blocks(x, n) > keep && er >= E_OK ) {
		XT	*t = &x[n - 1];

		cut = ( keep > t->l ) ? keep - t->l : 0;
		if ( later ) {
			er = pend_add(o->vol, t->p + cut, t->n - cut);
		} else {
			er = ts_free_ext(o->vol, t->p + cut, t->n - cut);
		}
		if ( cut == 0 ) {
			n--;
		} else {
			t->n = cut;
		}
	}
	*p_n = n;
	return er;
}

/* ---------------------------------------------------------------- data parts */

/* Whole blocks of a data part go to and from the device this many at a time */
#define RUN_IO_BLOCKS	32

LOCAL XT *xt_room( void )
{
	return (XT *)Kmalloc(sizeof(XT) * ( TSFSO_MAX_EXTENTS + 1 ));
}

/* The room for the blocks of one request: as many as `len` bytes can fill */
LOCAL SZ run_io_room( SZ len )
{
	UD	n = (UD)len / TSFSBLK_BLOCK_SIZE;

	if ( n > RUN_IO_BLOCKS ) n = RUN_IO_BLOCKS;
	return (SZ)( ( ( n >= 2 ) ? n : 1 ) * TSFSBLK_BLOCK_SIZE );
}

/*
 * How many whole blocks from `at` can go in one request: the part of the
 * run left, of the bytes asked for, and of the room there is for them.
 */
LOCAL UD run_io_n( UD at, SZ left, UD run )
{
	UD	n = (UD)left / TSFSBLK_BLOCK_SIZE;

	if ( at % TSFSBLK_BLOCK_SIZE != 0 ) return 0;
	if ( n > run ) n = run;
	if ( n > RUN_IO_BLOCKS ) n = RUN_IO_BLOCKS;
	return ( n >= 2 ) ? n : 0;
}

LOCAL ER dseg_read( OBJ *o, CONST UB *pl, UD off, UB *dst, SZ len, SZ *p_asize )
{
	UD	size = rd64(pl + PL_BYTES), l, p, run;
	UB	*buf;
	XT	*x;
	INT	n = 0;
	SZ	done = 0, k;
	ER	er;

	*p_asize = 0;
	if ( off >= size || len <= 0 ) {
		return E_OK;
	}
	if ( (UD)len > size - off ) len = (SZ)( size - off );
	if ( pl[PL_FORM] == F_INLINE ) {
		knl_memcpy(dst, o->hdr + rd16(pl + PL_A) + off, (INT)len);
		*p_asize = len;
		return E_OK;
	}
	x = xt_room();
	buf = (UB *)Kmalloc(run_io_room(len));
	if ( x == NULL || buf == NULL ) {
		if ( x != NULL ) Kfree(x);
		if ( buf != NULL ) Kfree(buf);
		return E_NOMEM;
	}
	er = xt_load(o, pl, x, &n);
	while ( er >= E_OK && done < len ) {
		UD	nb;

		l = ( off + (UD)done ) / TSFSBLK_BLOCK_SIZE;
		p = xt_map(x, n, l, &run);
		if ( p == 0 ) {
			er = E_OBJ;
			break;
		}
		nb = run_io_n(off + (UD)done, len - done, run);
		if ( nb > 0 ) {
			er = ts_read_blks(o->vol, p, nb, buf);
			if ( er < E_OK ) break;
			knl_memcpy(dst + done, buf, (INT)( nb * TSFSBLK_BLOCK_SIZE ));
			done += (SZ)( nb * TSFSBLK_BLOCK_SIZE );
			continue;
		}
		er = ts_read_blk(o->vol, p, buf);
		if ( er < E_OK ) break;
		k = TSFSBLK_BLOCK_SIZE - (SZ)( ( off + (UD)done ) % TSFSBLK_BLOCK_SIZE );
		if ( k > len - done ) k = len - done;
		knl_memcpy(dst + done, buf + ( off + (UD)done ) % TSFSBLK_BLOCK_SIZE, (INT)k);
		done += k;
	}
	Kfree(buf);
	Kfree(x);
	if ( er >= E_OK ) *p_asize = done;
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
 * Write into a data part at `off`. A small part stays inline while it
 * fits; past that it is blocks, grown as the write needs. The bytes go
 * straight to their blocks: they are not in the journal.
 */
LOCAL ER dseg_write( OBJ *o, UB *pl, UD off, CONST UB *src, SZ len )
{
	UD	size = rd64(pl + PL_BYTES), end = off + (UD)len, l, p, need;
	UB	*buf;
	XT	*x;
	INT	n = 0, noff;
	SZ	done = 0, k;
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

	x = xt_room();
	buf = (UB *)Kmalloc(run_io_room(len));
	if ( x == NULL || buf == NULL ) {
		if ( x != NULL ) Kfree(x);
		if ( buf != NULL ) Kfree(buf);
		return E_NOMEM;
	}
	er = xt_load(o, pl, x, &n);
	need = BLKS_FOR(end, TSFSBLK_BLOCK_SIZE);
	if ( er >= E_OK && need > xt_blocks(x, n) ) {
		/* blocks the write covers whole need not be cleared first */
		UD	kf = BLKS_FOR(off, TSFSBLK_BLOCK_SIZE);
		UD	kt = end / TSFSBLK_BLOCK_SIZE;

		er = xt_grow(o, x, &n, need, kf, ( kt > kf ) ? kt : kf);
		if ( er >= E_OK ) er = xt_store(o, pl, x, n);
	}
	while ( er >= E_OK && done < len ) {
		UD	at = off + (UD)done, run = 0, nb;

		l = at / TSFSBLK_BLOCK_SIZE;
		p = xt_map(x, n, l, &run);
		if ( p == 0 ) {
			er = E_OBJ;
			break;
		}
		nb = run_io_n(at, len - done, run);
		if ( nb > 0 ) {
			knl_memcpy(buf, src + done, (INT)( nb * TSFSBLK_BLOCK_SIZE ));
			er = ts_write_blks(o->vol, p, nb, buf);
			done += (SZ)( nb * TSFSBLK_BLOCK_SIZE );
			continue;
		}
		k = TSFSBLK_BLOCK_SIZE - (SZ)( at % TSFSBLK_BLOCK_SIZE );
		if ( k > len - done ) k = len - done;
		if ( k < TSFSBLK_BLOCK_SIZE ) {
			er = ts_read_blk(o->vol, p, buf);
			if ( er < E_OK ) break;
		}
		knl_memcpy(buf + at % TSFSBLK_BLOCK_SIZE, src + done, (INT)k);
		er = ts_write_blk(o->vol, p, buf);
		done += k;
	}
	Kfree(buf);
	Kfree(x);
	if ( er >= E_OK && end > size ) {
		wr64(pl + PL_BYTES, end);
	}
	return er;
}

/*
 * The part cut or lengthened to `size`. What is cut is given back; the
 * tail of the last block is cleared, so that lengthening it again later
 * shows zeros and not what was there before.
 */
LOCAL ER dseg_trunc( OBJ *o, UB *pl, UD nsize )
{
	UD	size = rd64(pl + PL_BYTES);
	UB	*zero;
	XT	*x;
	INT	n = 0;
	ER	er = E_OK;

	if ( nsize > size ) {
		zero = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( zero == NULL ) return E_NOMEM;
		knl_memset(zero, 0, TSFSBLK_BLOCK_SIZE);
		while ( size < nsize && er >= E_OK ) {
			SZ	k = ( nsize - size > TSFSBLK_BLOCK_SIZE ) ? TSFSBLK_BLOCK_SIZE : (SZ)( nsize - size );

			er = dseg_write(o, pl, size, zero, k);
			size += (UD)k;
		}
		Kfree(zero);
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
	x = xt_room();
	if ( x == NULL ) return E_NOMEM;
	er = xt_load(o, pl, x, &n);
	if ( er >= E_OK ) er = xt_shrink(o, x, &n, BLKS_FOR(nsize, TSFSBLK_BLOCK_SIZE), FALSE);
	if ( er >= E_OK ) er = xt_store(o, pl, x, n);
	if ( er >= E_OK && nsize % TSFSBLK_BLOCK_SIZE != 0 ) {
		UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		UD	p = xt_map(x, n, nsize / TSFSBLK_BLOCK_SIZE, NULL);

		if ( buf == NULL ) {
			er = E_NOMEM;
		} else if ( p != 0 ) {
			er = ts_read_blk(o->vol, p, buf);
			if ( er >= E_OK ) {
				knl_memset(buf + nsize % TSFSBLK_BLOCK_SIZE, 0,
					   (INT)( TSFSBLK_BLOCK_SIZE - nsize % TSFSBLK_BLOCK_SIZE ));
				er = ts_write_blk(o->vol, p, buf);
			}
		}
		if ( buf != NULL ) Kfree(buf);
	}
	Kfree(x);
	if ( er >= E_OK ) {
		wr64(pl + PL_BYTES, nsize);
		if ( n == 0 ) knl_memset(pl, 0, PL_SIZE);
	}
	return er;
}

/* Everything a data part holds given back, now or once it is real */
LOCAL ER dseg_free( OBJ *o, UB *pl, BOOL later )
{
	XT	*x;
	INT	n = 0;
	ER	er;

	if ( pl[PL_FORM] == F_INLINE ) {
		inl_give(o->hdr, rd16(pl + PL_A), rd16(pl + PL_A + 2));
		knl_memset(pl, 0, PL_SIZE);
		return E_OK;
	}
	x = xt_room();
	if ( x == NULL ) return E_NOMEM;
	er = xt_load(o, pl, x, &n);
	if ( er >= E_OK ) er = xt_shrink(o, x, &n, 0, later);
	if ( er >= E_OK && pl[PL_FORM] == F_TREE ) {
		er = later ? pend_add(o->vol, rd64(pl + PL_A), 1) : ts_free_ext(o->vol, rd64(pl + PL_A), 1);
	}
	Kfree(x);
	knl_memset(pl, 0, PL_SIZE);
	return er;
}

/* ---------------------------------------------------------------- tables */

/* A table read whole into memory with room for one more entry */
LOCAL ER tab_load( OBJ *o, INT at, INT cnt_at, INT esz, INT max, UB **p_buf, INT *p_n )
{
	UB	*buf;
	SZ	len = 0;
	INT	n = (INT)rd32(o->hdr + cnt_at);
	ER	er;

	*p_buf = NULL;
	*p_n = 0;
	if ( n > max ) {
		return E_OBJ;
	}
	buf = (UB *)Kmalloc((SZ)( max + 1 ) * esz);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	knl_memset(buf, 0, ( max + 1 ) * esz);
	er = mseg_read(o, o->hdr + at, buf, (SZ)max * esz, &len);
	if ( er >= E_OK && len != (SZ)n * esz ) {
		er = E_OBJ;
	}
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	*p_buf = buf;
	*p_n = n;
	return E_OK;
}

LOCAL ER tab_store( OBJ *o, INT at, INT cnt_at, INT esz, UB *buf, INT n )
{
	wr32(o->hdr + cnt_at, (UW)n);
	return mseg_write(o, o->hdr + at, buf, (SZ)n * esz);
}

#define PLACE_LOAD(o, b, n)	tab_load(o, OB_PLACE, OB_NPLACE, PE_SIZE, TSFSO_MAX_REC, b, n)
#define PLACE_STORE(o, b, n)	tab_store(o, OB_PLACE, OB_NPLACE, PE_SIZE, b, n)
#define RES_LOAD(o, b, n)	tab_load(o, OB_RES, OB_NRES, RS_SIZE, TSFSO_MAX_RES, b, n)
#define RES_STORE(o, b, n)	tab_store(o, OB_RES, OB_NRES, RS_SIZE, b, n)
#define LINK_LOAD(o, b, n)	tab_load(o, OB_LINK, OB_NLINK, LK_SIZE, TSFSO_MAX_LINK, b, n)
#define LINK_STORE(o, b, n)	tab_store(o, OB_LINK, OB_NLINK, LK_SIZE, b, n)

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
#define ORDER_TEXT_MAX(n)	( (n) * 48 + 8 )

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
		n = order_parse(j, (INT)len, r, TSFSO_MAX_REC);
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
 * when it becomes real.
 */
LOCAL ER txn_begin( ID vol )
{
	ER	er;

	if ( !vol_ok(vol) ) {
		return E_ID;
	}
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

LOCAL ER txn_end( ID vol, ER er )
{
	if ( !vol_ok(vol) || obj_trx[vol - 1] > 0 ) {
		return er;			/* the caller's transaction goes on */
	}
	ts_btree_jrnl(vol, FALSE);
	if ( er < E_OK ) {
		ts_jrnl_abort(vol);
		pend_n[vol - 1] = 0;		/* the old places are still in use */
		return er;
	}
	er = ts_jrnl_commit(vol);
	if ( er >= E_OK ) {
		pend_apply(vol);
	} else {
		pend_n[vol - 1] = 0;
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
	er = ts_jrnl_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	ts_btree_jrnl(vol, TRUE);
	obj_trx[vol - 1] = 1;

	return E_OK;
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
	obj_trx[vol - 1] = 0;
	ts_btree_jrnl(vol, FALSE);
	if ( !commit ) {
		pend_n[vol - 1] = 0;
		return ts_jrnl_abort(vol);
	}
	er = ts_jrnl_commit(vol);
	if ( er >= E_OK ) {
		pend_apply(vol);
	} else {
		pend_n[vol - 1] = 0;
	}
	return er;
}

/* ---------------------------------------------------------------- volume */

EXPORT ID ts_obj_mount( CONST char *devnm )
{
	ID	vol = ts_opn_vol_blk(devnm);

	if ( vol > 0 && vol_ok(vol) ) {
		obj_trx[vol - 1] = 0;
		pend_n[vol - 1] = 0;
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

	return ts_cls_vol_blk(vol);
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
	if ( jsonlen > TSFSO_META_MAX || iconlen < 0 || iconlen > TSFSO_ICON_MAX
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
	UINT	flags;
	INT	i, n = 0;
	ER	er;

	if ( PLACE_LOAD(o, &tab, &n) >= E_OK ) {
		for ( i = 0; i < n; i++ ) {
			dseg_free(o, tab + i * PE_SIZE + PE_PLACE, FALSE);
		}
		Kfree(tab);
	}
	if ( RES_LOAD(o, &tab, &n) >= E_OK ) {
		for ( i = 0; i < n; i++ ) {
			dseg_free(o, tab + i * RS_SIZE + RS_PLACE, FALSE);
		}
		Kfree(tab);
	}
	mseg_free(o, o->hdr + OB_PLACE);
	mseg_free(o, o->hdr + OB_RES);
	mseg_free(o, o->hdr + OB_LINK);
	mseg_free(o, o->hdr + OB_META);
	mseg_free(o, o->hdr + OB_ICON);

	flags = (UINT)rd32(o->hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_GARBAGE ) != 0 ) {
		ts_btree_delete_t(o->vol, TSFSBT_GC, uuid);
	}
	if ( ( flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK ) ) != 0 ) {
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
	UB	*tab = NULL, *j;
	SZ	len = 0;
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
	pk_robj->name[0] = '\0';
	j = meta_all(&o, 0, &len);
	if ( j != NULL ) {
		knl_json_get_str(j, (INT)len, "name", pk_robj->name, TSFS_NAME_MAX);
		Kfree(j);
	}
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
 * Whether a link of the same identity came before: a virtual object is
 * one link however often its element is written, and the first counts.
 * One with no identity is always a link of its own.
 */
LOCAL BOOL lk_seen( CONST UB *out, INT n, CONST UB *e )
{
	INT	i, k;

	for ( k = 0; k < 16 && e[LK_VOBJ + k] == 0; k++ ) ;
	if ( k == 16 ) {
		return FALSE;
	}
	for ( i = 0; i < n; i++ ) {
		if ( same16(out + i * LK_SIZE + LK_VOBJ, e + LK_VOBJ) ) {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * The <link> elements of an xmlTAD text, as entries after the `n` there
 * are. Elements and attributes are taken as words; the XML is not
 * parsed. Answers the entries there are then, or -1 for more than `max`.
 */
LOCAL INT lk_scan( CONST UB *t, SZ len, UW rid, UB *out, INT n, INT max )
{
	TS_UUID	target, vo;
	UB	*e;
	SZ	i, j, v;

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
		if ( v >= 0 && lk_uuid(t + v, j - v, &target) ) {
			if ( n >= max ) {
				return -1;
			}
			e = out + n * LK_SIZE;
			knl_memset(e, 0, LK_SIZE);
			v = lk_attr(t, i + 5, j, "vobjid");
			if ( v >= 0 && lk_uuid(t + v, j - v, &vo) ) {
				knl_memcpy(e + LK_VOBJ, vo.b, 16);
			}
			knl_memcpy(e + LK_TARGET, target.b, 16);
			wr32(e + LK_RID, rid);
			if ( !lk_seen(out, n, e) ) {
				n++;
			}
		}
		i = j;
	}
	return n;
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

LOCAL void lk_sort( UB *t, INT n )
{
	UB	x[LK_SIZE];
	INT	i, k;

	for ( i = 1; i < n; i++ ) {
		knl_memcpy(x, t + i * LK_SIZE, LK_SIZE);
		for ( k = i; k > 0 && lk_cmp(t + ( k - 1 ) * LK_SIZE, x) > 0; k-- ) {
			knl_memcpy(t + k * LK_SIZE, t + ( k - 1 ) * LK_SIZE, LK_SIZE);
		}
		knl_memcpy(t + k * LK_SIZE, x, LK_SIZE);
	}
}

/* The link table the object's xmlTAD records call for now, in order */
LOCAL ER lk_build( OBJ *o, UB *out, INT *p_n )
{
	RECE	*ord;
	UB	*tab = NULL, *e, *data;
	TS_UUID	t;
	UD	blk;
	SZ	got;
	INT	ntab = 0, nord, i, k, n = 0;
	ER	er;

	ord = (RECE *)Kmalloc(sizeof(RECE) * ( TSFSO_MAX_REC + 1 ));
	if ( ord == NULL ) {
		return E_NOMEM;
	}
	er = PLACE_LOAD(o, &tab, &ntab);
	nord = ( er >= E_OK ) ? order_of(o, tab, ntab, ord) : 0;
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
		if ( er >= E_OK ) {
			n = lk_scan(data, got, ord[i].rid, out, n, TSFSO_MAX_LINK);
			if ( n < 0 ) er = E_LIMIT;
		}
		Kfree(data);
	}
	/* what is not an object of this volume is not counted here */
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		knl_memcpy(t.b, out + i * LK_SIZE + LK_TARGET, 16);
		if ( ts_btree_lookup(o->vol, &t, &blk) < E_OK ) {
			wr32(out + i * LK_SIZE + LK_FLAGS, LK_F_EXTERNAL);
		}
	}
	lk_sort(out, ( er >= E_OK ) ? n : 0);
	if ( tab != NULL ) Kfree(tab);
	Kfree(ord);
	*p_n = ( er >= E_OK ) ? n : 0;
	return er;
}

/* The counts one step moves, applied once the object block is stored */
typedef struct {
	TS_UUID	t[RELINK_STEP];
	INT	d[RELINK_STEP];
	INT	n;
} DELTAS;

LOCAL void delta_add( DELTAS *dl, CONST UB *e, INT d )
{
	if ( ( rd32(e + LK_FLAGS) & LK_F_EXTERNAL ) == 0 && dl->n < RELINK_STEP ) {
		knl_memcpy(dl->t[dl->n].b, e + LK_TARGET, 16);
		dl->d[dl->n] = d;
		dl->n++;
	}
}

LOCAL void delta_apply( ID vol, DELTAS *dl )
{
	INT	i;

	/* one gone meanwhile has nothing to count */
	for ( i = 0; i < dl->n; i++ ) {
		(void)ref_move(vol, &dl->t[i], dl->d[i], 0);
	}
}

/*
 * One step of making the link table again: the table moves toward the
 * one the records call for by at most RELINK_STEP entries, and the
 * counts of what they point at move with it, in one transaction. The
 * last step takes the mark off.
 */
LOCAL ER relink_step( ID vol, CONST TS_UUID *uuid, BOOL *p_done )
{
	OBJ	o;
	DELTAS	*dl;
	UB	*old = NULL, *now = NULL, *next = NULL;
	UINT	flags;
	INT	nold = 0, nnow = 0, nnext = 0, a = 0, b = 0, c, took = 0;
	BOOL	more = FALSE;
	ER	er;

	*p_done = TRUE;
	dl = (DELTAS *)Kmalloc(sizeof(DELTAS));
	if ( dl == NULL ) {
		return E_NOMEM;
	}
	dl->n = 0;
	er = txn_begin(vol);
	if ( er < E_OK ) {
		Kfree(dl);
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er < E_OK ) {
		goto exit;
	}
	flags = (UINT)rd32(o.hdr + OB_FLAGS);
	if ( ( flags & TSFSO_F_RELINK ) == 0 || ( flags & TSFSO_F_ORPHAN ) != 0 ) {
		goto exit;			/* nothing to do, or reaping does it */
	}
	now = (UB *)Kmalloc((SZ)TSFSO_MAX_LINK * LK_SIZE);
	next = (UB *)Kmalloc((SZ)( TSFSO_MAX_LINK + 1 ) * LK_SIZE);
	if ( now == NULL || next == NULL ) {
		er = E_NOMEM;
		goto exit;
	}
	er = lk_build(&o, now, &nnow);
	if ( er >= E_OK ) er = LINK_LOAD(&o, &old, &nold);
	if ( er < E_OK ) {
		goto exit;
	}
	/* both in order: a walk through the two finds what went and came */
	while ( a < nold || b < nnow ) {
		c = ( a >= nold ) ? 1 : ( b >= nnow ) ? -1 : lk_cmp(old + a * LK_SIZE, now + b * LK_SIZE);
		if ( c == 0 ) {
			knl_memcpy(next + nnext++ * LK_SIZE, old + a * LK_SIZE, LK_SIZE);
			a++;
			b++;
		} else if ( c < 0 ) {
			if ( took < RELINK_STEP ) {
				delta_add(dl, old + a * LK_SIZE, -1);
				took++;
			} else {
				knl_memcpy(next + nnext++ * LK_SIZE, old + a * LK_SIZE, LK_SIZE);
				more = TRUE;
			}
			a++;
		} else {
			if ( took < RELINK_STEP && nnext < TSFSO_MAX_LINK ) {
				knl_memcpy(next + nnext++ * LK_SIZE, now + b * LK_SIZE, LK_SIZE);
				delta_add(dl, now + b * LK_SIZE, 1);
				took++;
			} else {
				more = TRUE;
			}
			b++;
		}
	}
	er = LINK_STORE(&o, next, nnext);
	if ( er >= E_OK && !more ) {
		wr32(o.hdr + OB_FLAGS, (UW)( flags & ~(UINT)TSFSO_F_RELINK ));
		er = ts_btree_delete_t(vol, TSFSBT_ORPHAN, uuid);
		if ( er == E_NOEXS ) er = E_OK;
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
	if ( now != NULL ) Kfree(now);
	if ( next != NULL ) Kfree(next);
	Kfree(dl);
	return txn_end(vol, er);
}

EXPORT ER ts_obj_relink( ID vol, CONST TS_UUID *uuid )
{
	BOOL	done = FALSE;
	INT	i;
	ER	er = E_OK;

	for ( i = 0; i < RELINK_GUARD && !done && er >= E_OK; i++ ) {
		er = relink_step(vol, uuid, &done);
	}
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
	INT	nold = 0, i, g;
	ER	er = E_OK;

	dl = (DELTAS *)Kmalloc(sizeof(DELTAS));
	if ( dl == NULL ) {
		return E_NOMEM;
	}
	for ( g = 0; g < RELINK_GUARD && er >= E_OK; g++ ) {
		dl->n = 0;
		er = txn_begin(vol);
		if ( er < E_OK ) {
			break;
		}
		er = obj_load(vol, uuid, &o);
		if ( er >= E_OK ) er = LINK_LOAD(&o, &old, &nold);
		if ( er >= E_OK && nold == 0 ) {
			obj_drop(&o);
			if ( old != NULL ) { Kfree(old); old = NULL; }
			er = txn_end(vol, E_OK);
			break;
		}
		if ( er >= E_OK ) {
			for ( i = nold - 1; i >= 0 && nold - i <= RELINK_STEP; i-- ) {
				delta_add(dl, old + i * LK_SIZE, -1);
			}
			nold = i + 1;
			er = mark_relink(&o);
			if ( er >= E_OK ) er = LINK_STORE(&o, old, nold);
			if ( er >= E_OK ) er = obj_store(&o);
		}
		obj_drop(&o);
		if ( old != NULL ) { Kfree(old); old = NULL; }
		if ( er >= E_OK ) {
			delta_apply(vol, dl);
		}
		er = txn_end(vol, er);
	}
	Kfree(dl);
	return er;
}

EXPORT ER ts_obj_lst_lnk( ID vol, CONST TS_UUID *uuid, T_RLNK *buf, INT n, INT *p_cnt )
{
	OBJ	o;
	RECE	*ord;
	UB	*lk = NULL, *tab = NULL, *e;
	INT	nlk = 0, ntab = 0, nord = 0, i, k, got = 0;
	ER	er;

	if ( buf == NULL || n <= 0 || p_cnt == NULL ) {
		return E_PAR;
	}
	ord = (RECE *)Kmalloc(sizeof(RECE) * ( TSFSO_MAX_REC + 1 ));
	if ( ord == NULL ) {
		return E_NOMEM;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = LINK_LOAD(&o, &lk, &nlk);
	if ( er >= E_OK ) er = PLACE_LOAD(&o, &tab, &ntab);
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
	Kfree(ord);
	obj_drop(&o);
	return er;
}

/* ---------------------------------------------------------------- going */

/* Why an object may not go, or E_OK */
#define GO_DELETE	0		/* ts_obj_delete */
#define GO_COLLECT	1		/* from the garbage list */
#define GO_REAP		2		/* an orphan */

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
 * An object that goes: its links given up first, a step at a time, then
 * everything else in one transaction, which looks again whether it may.
 */
LOCAL ER obj_go( ID vol, CONST TS_UUID *uuid, INT how )
{
	OBJ	o;
	ER	er;

	er = go_check(vol, uuid, how);
	if ( er >= E_OK ) er = links_release(vol, uuid);
	if ( er < E_OK ) {
		return er;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
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
		if ( ( flags & TSFSO_F_RELINK ) == 0 ) {
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
		} else if ( ( flags & TSFSO_F_RELINK ) != 0 ) {
			(void)ts_obj_relink(vol, &cur);
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

/* The blocks a place lies in: management blocks checked, data read */
LOCAL void scrub_place( OBJ *o, CONST UB *pl, BOOL mgmt, BOOL data, T_TSFSSCRUB *pk )
{
	XT	*x;
	UB	*buf;
	UD	i, k;
	INT	n = 0;

	if ( pl[PL_FORM] == F_RUN && mgmt ) {
		for ( i = 0; i < rd64(pl + PL_B); i++ ) {
			(void)ts_scrub_one_blk(o->vol, rd64(pl + PL_A) + i, TSFSBLK_MAGIC_TBL, pk);
		}
		return;
	}
	if ( pl[PL_FORM] == F_TREE ) {
		(void)ts_scrub_one_blk(o->vol, rd64(pl + PL_A), TSFSBLK_MAGIC_TBL, pk);
	}
	if ( mgmt || !data || ( pl[PL_FORM] != F_RUN && pl[PL_FORM] != F_TREE ) ) {
		return;
	}
	x = xt_room();
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( x != NULL && buf != NULL && xt_load(o, pl, x, &n) >= E_OK ) {
		for ( k = 0; k < (UD)n; k++ ) {
			for ( i = 0; i < x[k].n; i++ ) {
				pk->checked++;
				if ( knl_tsfsblk_raw_read(o->vol, x[k].p + i, buf) < E_OK ) {
					ts_blk_error(o->vol, x[k].p + i, TSFSBLK_ERR_IO);
					pk->bad++;
					if ( pk->first_bad == 0 ) pk->first_bad = x[k].p + i;
				}
			}
		}
	}
	if ( x != NULL ) Kfree(x);
	if ( buf != NULL ) Kfree(buf);
}

EXPORT ER ts_obj_scrub( ID vol, BOOL data, T_TSFSSCRUB *pk )
{
	SCRUB	sc;
	TS_UUID	key, cur;
	OBJ	o;
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
			if ( RES_LOAD(&o, &tab, &n) >= E_OK ) {
				for ( i = 0; i < n; i++ ) {
					scrub_place(&o, tab + i * RS_SIZE + RS_PLACE, FALSE, data, pk);
				}
				Kfree(tab);
			}
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
	UB	*j, *tab = NULL, *txt;
	RECE	ord[TSFSO_MAX_REC + 1];
	INT	n = (INT)size, cap, ntab = 0, nord, tl;
	ER	er;

	if ( json == NULL || size > TSFSO_META_MAX ) {
		return E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) {
		cap = n + ORDER_TEXT_MAX(TSFSO_MAX_REC) + 32;
		j = (UB *)Kmalloc(cap);
		txt = (UB *)Kmalloc(ORDER_TEXT_MAX(TSFSO_MAX_REC));
		if ( j == NULL || txt == NULL ) {
			er = E_NOMEM;
		} else {
			knl_memcpy(j, json, n);
			if ( is_object(j, n) ) {
				n = meta_strip(j, n);
				n = jx_del(j, n, "records");
				er = PLACE_LOAD(&o, &tab, &ntab);
				if ( er >= E_OK && ntab > 0 ) {
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
	UB	*pl = o->hdr + OB_ICON, *tab = NULL, *buf;
	SZ	len, got = 0;
	INT	n = 0, at;
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
		er = RES_LOAD(o, &tab, &n);
		at = ( er >= E_OK ) ? icon_row(tab, n) : -1;
		if ( er >= E_OK && at < 0 ) {
			er = E_NOEXS;
		}
		if ( er < E_OK ) {
			if ( tab != NULL ) Kfree(tab);
			return er;
		}
		len = (SZ)rd64(tab + at * RS_SIZE + RS_BYTES);
		buf = (UB *)Kmalloc(( len > 0 ) ? len : 1);
		er = ( buf == NULL ) ? E_NOMEM
		   : dseg_read(o, tab + at * RS_SIZE + RS_PLACE, 0, buf, len, &got);
		Kfree(tab);
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
	UB	*tab = NULL;
	INT	n = 0, at;
	BOOL	moved = FALSE;
	ER	er;

	if ( size < 0 || size > TSFSO_ICON_MAX || ( size > 0 && icon == NULL ) ) {
		return E_PAR;
	}
	er = txn_begin(vol);
	if ( er < E_OK ) {
		return er;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = mseg_write(&o, o.hdr + OB_ICON, icon, size);
	if ( er >= E_OK ) er = RES_LOAD(&o, &tab, &n);
	while ( er >= E_OK && ( at = icon_row(tab, n) ) >= 0 ) {
		er = dseg_free(&o, tab + at * RS_SIZE + RS_PLACE, TRUE);
		knl_memcpy(tab + at * RS_SIZE, tab + ( n - 1 ) * RS_SIZE, RS_SIZE);
		n--;
		moved = TRUE;
	}
	if ( er >= E_OK && moved ) er = RES_STORE(&o, tab, n);
	if ( er >= E_OK ) {
		obj_touch(&o);
		er = obj_store(&o);
	}
	if ( tab != NULL ) Kfree(tab);
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
	UB	*tab;			/* the placement table */
	INT	n;			/* its entries */
	RECE	ord[TSFSO_MAX_REC + 1];	/* the records in order */
	INT	nord;
} RECS;

LOCAL ER recs_open( ID vol, CONST TS_UUID *uuid, RECS *r )
{
	ER	er;

	r->tab = NULL;
	r->nord = 0;
	er = obj_load(vol, uuid, &r->o);
	if ( er >= E_OK ) er = PLACE_LOAD(&r->o, &r->tab, &r->n);
	if ( er >= E_OK ) r->nord = order_of(&r->o, r->tab, r->n, r->ord);
	return er;
}

LOCAL void recs_close( RECS *r )
{
	if ( r->tab != NULL ) Kfree(r->tab);
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
	UB	*e;
	INT	i, got = 0;
	ER	er;

	if ( buf == NULL || n <= 0 || p_cnt == NULL ) {
		return E_PAR;
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

	if ( r->n >= TSFSO_MAX_REC ) {
		return E_LIMIT;
	}
	if ( pos < 0 || pos > r->nord ) pos = r->nord;
	rid = rd32(r->o.hdr + OB_NEXTRID);
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

EXPORT ER ts_obj_rea_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  void *buf, SZ size, SZ *p_asize )
{
	RECS	*r;
	UB	*e;
	SZ	got = 0;
	ER	er;

	if ( buf == NULL || off < 0 ) {
		return E_PAR;
	}
	r = (RECS *)Kmalloc(sizeof(RECS));
	if ( r == NULL ) return E_NOMEM;
	er = recs_open(vol, uuid, r);
	if ( er >= E_OK ) {
		e = recs_entry(r, recno);
		if ( e == NULL ) {
			er = E_NOEXS;
		} else {
			er = dseg_read(&r->o, e + PE_PLACE, (UD)off, (UB *)buf, size, &got);
			if ( er >= E_OK && p_asize != NULL ) *p_asize = got;
		}
	}
	recs_close(r);
	Kfree(r);
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
	ER	er;

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
			er = ( size > 0 ) ? dseg_write(&r->o, fresh, 0, buf, size) : E_OK;
			if ( er >= E_OK ) er = dseg_free(&r->o, e + PE_PLACE, TRUE);
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

EXPORT ER ts_obj_wri_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  CONST void *buf, SZ size, SZ *p_asize )
{
	ER	er;

	if ( buf == NULL || off < 0 ) {
		return E_PAR;
	}
	er = rec_change(vol, uuid, recno, RC_WRITE, off, (CONST UB *)buf, size);
	if ( er >= E_OK && p_asize != NULL ) {
		*p_asize = size;
	}
	return er;
}

EXPORT ER ts_obj_rpl_rec( ID vol, CONST TS_UUID *uuid, INT recno, CONST void *buf,
			  SZ size )
{
	if ( ( buf == NULL && size > 0 ) || size < 0 ) {
		return E_PAR;
	}
	return rec_change(vol, uuid, recno, RC_REPLACE, 0, (CONST UB *)buf, size);
}

EXPORT ER ts_obj_trn_rec( ID vol, CONST TS_UUID *uuid, INT recno, UD newsize )
{
	return rec_change(vol, uuid, recno, RC_TRUNC, (D)newsize, NULL, 0);
}

/*
 * A record taken out from any place: its bytes, its entry, its place in
 * the order, and the resources that belong to it.
 */
EXPORT ER ts_obj_del_rec( ID vol, CONST TS_UUID *uuid, INT recno )
{
	RECS	*r;
	UB	*e, *res = NULL;
	UW	rid;
	INT	i, k, nres = 0;
	ER	er;

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
	if ( er >= E_OK ) {
		rid = rd32(e + PE_RID);
		er = dseg_free(&r->o, e + PE_PLACE, FALSE);
		if ( er >= E_OK && r->ord[recno].rt == 1 ) {
			er = mark_relink(&r->o);
		}
		if ( er >= E_OK ) er = RES_LOAD(&r->o, &res, &nres);
		if ( er >= E_OK ) {
			for ( i = 0, k = 0; i < nres; i++ ) {
				UB	*x = res + i * RS_SIZE;

				if ( rd32(x + RS_OWNER) == rid ) {
					dseg_free(&r->o, x + RS_PLACE, FALSE);
					continue;
				}
				if ( k != i ) knl_memcpy(res + k * RS_SIZE, x, RS_SIZE);
				k++;
			}
			er = RES_STORE(&r->o, res, k);
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
	if ( res != NULL ) Kfree(res);
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
	RECE	ord[TSFSO_MAX_REC + 1];
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
		nord = order_of(o, tab, n, ord);
		*p_rid = ( recno < nord ) ? ord[recno].rid : ( RID_PLACE | (UW)recno );
	}
	if ( tab != NULL ) Kfree(tab);
	return er;
}

/* The place of a record's rid among the records, or TSFSO_ICON_REC */
LOCAL INT recno_of( OBJ *o, UW rid )
{
	RECE	ord[TSFSO_MAX_REC + 1];
	UB	*tab = NULL;
	INT	n = 0, nord, i, at = -2;

	if ( rid == RID_ICON ) {
		return TSFSO_ICON_REC;
	}
	if ( ( rid & RID_PLACE ) != 0 ) {
		return (INT)( rid & ~RID_PLACE );
	}
	if ( PLACE_LOAD(o, &tab, &n) >= E_OK ) {
		nord = order_of(o, tab, n, ord);
		for ( i = 0; i < nord; i++ ) {
			if ( ord[i].rid == rid ) {
				at = i;
				break;
			}
		}
		Kfree(tab);
	}
	return at;
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

EXPORT ER ts_obj_rea_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  void *buf, SZ size, SZ *p_asize, UB *ext )
{
	return ts_obj_rea_res_at(vol, uuid, recno, resno, 0, buf, size, p_asize, ext);
}

EXPORT ER ts_obj_rea_res_at( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			     D off, void *buf, SZ size, SZ *p_asize, UB *ext )
{
	OBJ	o;
	UB	*tab = NULL;
	UW	rid = 0;
	SZ	got = 0;
	INT	n = 0, at = -1;
	ER	er;

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
	if ( er >= E_OK ) er = RES_LOAD(&o, &tab, &n);
	if ( er >= E_OK ) {
		at = res_find(tab, n, rid, recno, resno);
		if ( at < 0 ) er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		er = dseg_read(&o, tab + at * RS_SIZE + RS_PLACE, (UD)off, (UB *)buf, size, &got);
		if ( er >= E_OK ) {
			if ( p_asize != NULL ) *p_asize = got;
			if ( ext != NULL ) {
				field_get(tab + at * RS_SIZE + RS_EXT, TSFSO_EXT_LEN, ext, TSFSO_EXT_LEN);
			}
		}
	}
	if ( tab != NULL ) Kfree(tab);
	obj_drop(&o);
	return er;
}

EXPORT ER ts_obj_wri_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  CONST UB *ext, CONST void *buf, SZ size )
{
	OBJ	o;
	UB	*tab = NULL, *e, fresh[PL_SIZE];
	UW	rid = 0;
	INT	n = 0, at;
	ER	er;

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
	if ( er >= E_OK ) er = rid_of(&o, recno, &rid);
	if ( er >= E_OK ) er = RES_LOAD(&o, &tab, &n);
	if ( er >= E_OK ) {
		at = res_find(tab, n, rid, recno, resno);
		if ( at < 0 ) {
			if ( n >= TSFSO_MAX_RES ) {
				er = E_LIMIT;
			} else {
				at = n++;
				e = tab + at * RS_SIZE;
				knl_memset(e, 0, RS_SIZE);
				wr32(e + RS_OWNER, rid);
				wr32(e + RS_RESNO, (UW)resno);
			}
		}
		if ( er >= E_OK ) {
			/* a resource is always written whole: a new place,
			   the old one given up once the change is real */
			e = tab + at * RS_SIZE;
			knl_memset(fresh, 0, PL_SIZE);
			er = ( size > 0 ) ? dseg_write(&o, fresh, 0, (CONST UB *)buf, size) : E_OK;
			if ( er >= E_OK ) er = dseg_free(&o, e + RS_PLACE, TRUE);
			if ( er >= E_OK ) {
				knl_memcpy(e + RS_PLACE, fresh, PL_SIZE);
				wr64(e + RS_BYTES, (UD)size);
				field_put(e + RS_EXT, TSFSO_EXT_LEN, ext);
				er = RES_STORE(&o, tab, n);
			}
		}
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	if ( tab != NULL ) Kfree(tab);
	obj_drop(&o);
	return txn_end(vol, er);
}

EXPORT ER ts_obj_lst_res( ID vol, CONST TS_UUID *uuid, T_TSFSRES *buf, INT n, INT *p_cnt )
{
	OBJ	o;
	UB	*tab = NULL;
	INT	i, all = 0, cnt = 0;
	ER	er;

	if ( buf == NULL || p_cnt == NULL ) {
		return E_PAR;
	}
	er = obj_load(vol, uuid, &o);
	if ( er >= E_OK ) er = RES_LOAD(&o, &tab, &all);
	if ( er >= E_OK ) {
		UD	isz = 0;

		/* the icon first, wherever it is kept */
		if ( icon_size(&o, tab, all, &isz) && cnt < n ) {
			buf[cnt].recno = TSFSO_ICON_REC;
			buf[cnt].resno = 0;
			field_get((CONST UB *)"ico", 4, buf[cnt].ext, TSFSO_EXT_LEN);
			buf[cnt].size = isz;
			cnt++;
		}
		for ( i = 0; i < all && cnt < n; i++ ) {
			UB	*e = tab + i * RS_SIZE;

			if ( rd32(e + RS_OWNER) == RID_ICON ) {
				continue;		/* listed above */
			}
			buf[cnt].recno = recno_of(&o, rd32(e + RS_OWNER));
			buf[cnt].resno = (INT)rd32(e + RS_RESNO);
			field_get(e + RS_EXT, TSFSO_EXT_LEN, buf[cnt].ext, TSFSO_EXT_LEN);
			buf[cnt].size = rd64(e + RS_BYTES);
			cnt++;
		}
		*p_cnt = cnt;
	}
	if ( tab != NULL ) Kfree(tab);
	obj_drop(&o);
	return er;
}

EXPORT ER ts_obj_del_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno )
{
	OBJ	o;
	UB	*tab = NULL;
	UW	rid = 0;
	INT	n = 0, at = -1;
	ER	er;

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
	if ( er >= E_OK ) er = RES_LOAD(&o, &tab, &n);
	if ( er >= E_OK ) {
		at = res_find(tab, n, rid, recno, resno);
		if ( at < 0 ) er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		er = dseg_free(&o, tab + at * RS_SIZE + RS_PLACE, FALSE);
		if ( er >= E_OK ) {
			knl_memcpy(tab + at * RS_SIZE, tab + ( n - 1 ) * RS_SIZE, RS_SIZE);
			er = RES_STORE(&o, tab, n - 1);
		}
		obj_touch(&o);
		if ( er >= E_OK ) er = obj_store(&o);
	}
	if ( tab != NULL ) Kfree(tab);
	obj_drop(&o);
	return txn_end(vol, er);
}
