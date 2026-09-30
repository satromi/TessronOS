/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsblk.c
 *	TSFS native volume, block layer (design 11.6).
 *
 *	Layout of a volume:
 *		block 0, 1		superblock A and B, written in turn
 *		2 ..			the journal area
 *		then			allocation groups 0, 1, 2 ...
 *
 *	An allocation group starts with its head, then (for groups 1, 3, 5,
 *	7, 9, 25, 27, 49 ...) a spare copy of the superblock, then its free
 *	map, one bit per block of the group, and then data. The map's own
 *	blocks carry no head; the group head holds a checksum for each of
 *	them. Every block the group itself uses is marked taken in its map
 *	when the volume is made, so the allocator never hands one out.
 *
 *	The superblock is written only when the volume is opened and closed,
 *	when an error is recorded, and when a transaction moves a root; each
 *	write goes to the copy the last one did not, so a write cut short
 *	leaves the other copy as it was.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/dt.h>
#include <ts/blk.h>

/*
 * Map blocks a transaction may change before it becomes real. A change
 * usually touches one, because the blocks it takes lie together; one
 * that would touch more than this many is refused (E_LIMIT).
 */
#define TSFSBLK_HELD_MAP		16

#define AG_MAX_BLOCKS		( 1UL << 18 )	/* a gigabyte of 4KB blocks */
#define AG_MIN_BLOCKS		1024
#define AG_MIN_TAIL		64		/* a last group smaller than this is left out */
#define BITS_PER_MAP		( TSFSBLK_BLOCK_SIZE * 8 )
#define AG_MAX_MAP		( AG_MAX_BLOCKS / BITS_PER_MAP )

#define JRNL_MAX_BLOCKS		( 1UL << 18 )

/* ---------------------------------------------------------------- places in blocks */

/* the superblock */
#define SB_MAGIC	0
#define SB_VERSION	8
#define SB_BSIZE	12
#define SB_GEN		16
#define SB_VOLUUID	24
#define SB_LABEL	40
#define SB_TOTAL	112
#define SB_FREE		120
#define SB_AGBLOCKS	128
#define SB_AGCOUNT	136
#define SB_JSTART	144
#define SB_JBLOCKS	152
#define SB_JSEQ		160
#define SB_OBJROOT	176
#define SB_OBJNODES	184
#define SB_GCROOT	192
#define SB_GCNODES	200
#define SB_ORROOT	208
#define SB_ORNODES	216
#define SB_ROOTUUID	224
#define SB_SYSUUID	240
#define SB_DOMUUID	256
#define SB_COMPAT	272
#define SB_ROCOMPAT	280
#define SB_INCOMPAT	288
#define SB_STATE	296
#define SB_ERRKIND	300
#define SB_ERRFIRST	304
#define SB_ERRLAST	312
#define SB_ERRBLK	320
#define SB_MOUNTS	360
#define SB_MOUNTTIME	368
#define SB_FSCKTIME	376
#define SB_MKFSTIME	384
#define SB_CRC		4092

/* the common head */
#define HD_MAGIC	0
#define HD_BLK		8
#define HD_VOLUUID	16
#define HD_OWNER	32
#define HD_GEN		40
#define HD_CRC		48

/* the group head, after the common head */
#define AG_NO		64
#define AG_FIRST	72
#define AG_LEN		80
#define AG_FREE		88
#define AG_MAPFIRST	96
#define AG_MAPBLOCKS	104
#define AG_SPARE	108
#define AG_MAPCRC	128

/* Features: none are known yet beyond what version 4 is */
#define KNOWN_COMPAT	0ULL
#define KNOWN_ROCOMPAT	0ULL
#define KNOWN_INCOMPAT	0ULL

typedef struct {
	UD	first;			/* first block of the group */
	UD	len;			/* blocks of the group */
	UD	free;
	UD	map_first;
	UW	map_blocks;
	BOOL	spare;			/* holds a copy of the superblock */
	UW	crc[AG_MAX_MAP];	/* of each map block */
	BOOL	dirty;			/* the head on the medium is behind */
} TSFSAG;

typedef struct {
	ID		dd;		/* the block device */
	ID		mtxid;
	T_TSFSBLK_SB	sb;
	TSFSAG		*ag;
	UD		ag_first;	/* first block of group 0 */
	UD		data_end;	/* one past the last block of the last group */
	UB		*cache;		/* one block of a free map */
	UD		cache_blk;	/* which block is in it, ~0 when none */
	UD		cache_ag;
	UW		cache_k;	/* which map block of its group */
	UD		hint;		/* where the next search starts */
	BOOL		cache_dirty;
	BOOL		jrnl_on;	/* a transaction is open on this volume */
	UB		*held_buf[TSFSBLK_HELD_MAP];
	UD		held_blk[TSFSBLK_HELD_MAP];
	UD		held_ag[TSFSBLK_HELD_MAP];
	UW		held_k[TSFSBLK_HELD_MAP];
	INT		held_n;	/* map blocks waiting for the record */
	BOOL		sb_dirty;
	BOOL		rdonly;		/* an error was found, or a feature is not known */
	UW		err_kind;	/* an error of this run, kept across a discard */
	UD		err_first, err_last, err_blk;
	ID		bc_mtx;		/* the block cache, below every other lock */
	struct bcent	**bc_hash;
	struct bcent	*bc_head;	/* the most recently used */
	struct bcent	*bc_tail;	/* the least */
	UD		bc_n;
	UD		bc_hits, bc_misses;
	UD		writes;		/* to the device, since ts_cut_blk last asked */
	UD		cut_after;	/* writes still to go before the pretended cut */
	BOOL		cut;		/* a cut is set */
	BOOL		used;
} TSFSBLKVOL;

LOCAL TSFSBLKVOL	tsfsblk_vol[TSFSBLK_MAX_VOL];
LOCAL ID	tsfsblk_mtxid = 0;

#define VOL_ID(v)	( (ID)( (v) - tsfsblk_vol ) + 1 )

/* ---------------------------------------------------------------- little endian */

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
	p[0] = (UB)v; p[1] = (UB)(v >> 8); p[2] = (UB)(v >> 16); p[3] = (UB)(v >> 24);
}

LOCAL void wr64( UB *p, UD v )
{
	wr32(p, (UW)v);
	wr32(p + 4, (UW)(v >> 32));
}

LOCAL void cpy16( UB *d, CONST UB *s )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) d[i] = s[i];
}

LOCAL TS_TIME now_time( void )
{
	TS_TIME	t = 0;

	return ( dt_gettime(&t) >= E_OK ) ? t : 0;
}

/* ---------------------------------------------------------------- superblock */

LOCAL void sb_pack( CONST T_TSFSBLK_SB *sb, UB *buf )
{
	INT	i;

	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	for ( i = 0; i < 8; i++ ) buf[SB_MAGIC + i] = sb->magic[i];
	wr32(buf + SB_VERSION, sb->version);
	wr32(buf + SB_BSIZE, sb->block_size);
	wr64(buf + SB_GEN, sb->generation);
	cpy16(buf + SB_VOLUUID, sb->vol_uuid.b);
	for ( i = 0; i < TSFSBLK_LABEL_MAX; i++ ) buf[SB_LABEL + i] = sb->label[i];
	wr64(buf + SB_TOTAL, sb->total_blocks);
	wr64(buf + SB_FREE, sb->free_blocks);
	wr64(buf + SB_AGBLOCKS, sb->ag_blocks);
	wr64(buf + SB_AGCOUNT, sb->ag_count);
	wr64(buf + SB_JSTART, sb->journal_start);
	wr64(buf + SB_JBLOCKS, sb->journal_blocks);
	wr64(buf + SB_JSEQ, sb->journal_seq);
	wr64(buf + SB_OBJROOT, sb->objtbl_start);
	wr64(buf + SB_OBJNODES, sb->objtbl_blocks);
	wr64(buf + SB_GCROOT, sb->gclist_start);
	wr64(buf + SB_GCNODES, sb->gclist_blocks);
	wr64(buf + SB_ORROOT, sb->orphan_start);
	wr64(buf + SB_ORNODES, sb->orphan_blocks);
	cpy16(buf + SB_ROOTUUID, sb->root_uuid.b);
	cpy16(buf + SB_SYSUUID, sb->sysbox_uuid.b);
	cpy16(buf + SB_DOMUUID, sb->domain_uuid.b);
	wr64(buf + SB_COMPAT, sb->compat);
	wr64(buf + SB_ROCOMPAT, sb->ro_compat);
	wr64(buf + SB_INCOMPAT, sb->incompat);
	wr32(buf + SB_STATE, sb->state);
	wr32(buf + SB_ERRKIND, sb->err_kind);
	wr64(buf + SB_ERRFIRST, sb->err_first);
	wr64(buf + SB_ERRLAST, sb->err_last);
	wr64(buf + SB_ERRBLK, sb->err_blk);
	wr64(buf + SB_MOUNTS, sb->mount_count);
	wr64(buf + SB_MOUNTTIME, sb->mount_time);
	wr64(buf + SB_FSCKTIME, sb->fsck_time);
	wr64(buf + SB_MKFSTIME, sb->mkfs_time);
	wr32(buf + SB_CRC, ts_crc32c(buf, SB_CRC));
}

LOCAL ER sb_unpack( CONST UB *buf, T_TSFSBLK_SB *sb )
{
	CONST char *m = TSFSBLK_MAGIC;
	INT	i;

	for ( i = 0; i < 8; i++ ) {
		if ( buf[SB_MAGIC + i] != (UB)m[i] ) return E_OBJ;
		sb->magic[i] = buf[SB_MAGIC + i];
	}
	if ( rd32(buf + SB_CRC) != ts_crc32c(buf, SB_CRC) ) {
		return E_OBJ;			/* this copy is damaged */
	}
	sb->version       = rd32(buf + SB_VERSION);
	sb->block_size    = rd32(buf + SB_BSIZE);
	sb->generation    = rd64(buf + SB_GEN);
	cpy16(sb->vol_uuid.b, buf + SB_VOLUUID);
	for ( i = 0; i < TSFSBLK_LABEL_MAX; i++ ) sb->label[i] = buf[SB_LABEL + i];
	sb->total_blocks  = rd64(buf + SB_TOTAL);
	sb->free_blocks   = rd64(buf + SB_FREE);
	sb->ag_blocks     = rd64(buf + SB_AGBLOCKS);
	sb->ag_count      = rd64(buf + SB_AGCOUNT);
	sb->journal_start = rd64(buf + SB_JSTART);
	sb->journal_blocks = rd64(buf + SB_JBLOCKS);
	sb->journal_seq   = rd64(buf + SB_JSEQ);
	sb->objtbl_start  = rd64(buf + SB_OBJROOT);
	sb->objtbl_blocks = rd64(buf + SB_OBJNODES);
	sb->gclist_start  = rd64(buf + SB_GCROOT);
	sb->gclist_blocks = (UW)rd64(buf + SB_GCNODES);
	sb->orphan_start  = rd64(buf + SB_ORROOT);
	sb->orphan_blocks = (UW)rd64(buf + SB_ORNODES);
	cpy16(sb->root_uuid.b, buf + SB_ROOTUUID);
	cpy16(sb->sysbox_uuid.b, buf + SB_SYSUUID);
	cpy16(sb->domain_uuid.b, buf + SB_DOMUUID);
	sb->compat        = rd64(buf + SB_COMPAT);
	sb->ro_compat     = rd64(buf + SB_ROCOMPAT);
	sb->incompat      = rd64(buf + SB_INCOMPAT);
	sb->state         = (UW)rd32(buf + SB_STATE);
	sb->err_kind      = (UW)rd32(buf + SB_ERRKIND);
	sb->err_first     = rd64(buf + SB_ERRFIRST);
	sb->err_last      = rd64(buf + SB_ERRLAST);
	sb->err_blk       = rd64(buf + SB_ERRBLK);
	sb->mount_count   = rd64(buf + SB_MOUNTS);
	sb->mount_time    = rd64(buf + SB_MOUNTTIME);
	sb->fsck_time     = rd64(buf + SB_FSCKTIME);
	sb->mkfs_time     = rd64(buf + SB_MKFSTIME);

	if ( sb->version != TSFSBLK_VERSION || sb->block_size != TSFSBLK_BLOCK_SIZE ) {
		return E_NOSPT;
	}
	if ( sb->ag_blocks == 0 || sb->ag_count == 0 || sb->total_blocks < TSFSBLK_MIN_BLOCKS ) {
		return E_OBJ;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- device */

/*
 * The requests each open volume's device has been given, by the
 * descriptor it was opened with: what the performance goals (design
 * 11.12) are counted in. A device not in the table (one being formatted)
 * is not counted.
 */
typedef struct {
	ID	dd;
	UD	rd, wr, fl;
} DEVIO;

LOCAL DEVIO	dev_io[TSFSBLK_MAX_VOL];

LOCAL DEVIO *io_of( ID dd )
{
	INT	i;

	for ( i = 0; i < TSFSBLK_MAX_VOL; i++ ) {
		if ( dev_io[i].dd == dd && dd > 0 ) return &dev_io[i];
	}
	return NULL;
}

LOCAL void io_open( ID dd )
{
	INT	i;

	for ( i = 0; i < TSFSBLK_MAX_VOL; i++ ) {
		if ( dev_io[i].dd <= 0 ) {
			dev_io[i].dd = dd;
			dev_io[i].rd = dev_io[i].wr = dev_io[i].fl = 0;
			return;
		}
	}
}

LOCAL void io_close( ID dd )
{
	DEVIO	*d = io_of(dd);

	if ( d != NULL ) d->dd = 0;
}

LOCAL ER blks_read( ID dd, UD blk, UD n, void *buf )
{
	DEVIO	*d = io_of(dd);
	SZ	asize;

	if ( d != NULL ) __atomic_add_fetch(&d->rd, 1, __ATOMIC_RELAXED);
	return tk_srea_dev(dd, (W)(blk * TSFSBLK_SECT_PER_BLK), buf,
			   (SZ)( n * TSFSBLK_BLOCK_SIZE ), &asize);
}

LOCAL ER blks_write( ID dd, UD blk, UD n, CONST void *buf )
{
	DEVIO	*d = io_of(dd);
	SZ	asize;

	if ( d != NULL ) __atomic_add_fetch(&d->wr, 1, __ATOMIC_RELAXED);
	return tk_swri_dev(dd, (W)(blk * TSFSBLK_SECT_PER_BLK), buf,
			   (SZ)( n * TSFSBLK_BLOCK_SIZE ), &asize);
}

LOCAL ER blk_read( ID dd, UD blk, void *buf )
{
	return blks_read(dd, blk, 1, buf);
}

LOCAL ER blk_write( ID dd, UD blk, CONST void *buf )
{
	return blks_write(dd, blk, 1, buf);
}

/*
 * The device's cache emptied onto the medium. A device that does not
 * know the request answers E_NOSPT; the volume notes that it cannot be
 * sure of the order of its writes.
 */
LOCAL ER dev_flush( ID dd )
{
	DEVIO	*d = io_of(dd);

	if ( d != NULL ) __atomic_add_fetch(&d->fl, 1, __ATOMIC_RELAXED);
	SZ	asize;
	ER	er;

	er = tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asize);
	return ( er == E_PAR ) ? E_NOSPT : er;
}

/* ---------------------------------------------------------------- the block cache */

/*
 * Blocks of the volume kept in memory, least recently used given up
 * first. Every write to the device passes through here and puts the
 * same bytes in, so a block in the cache is always the block on the
 * medium. A read that has to go to the device puts a place for the
 * block in first, marked as being read; a write that comes meanwhile
 * marks it stale, and the bytes read are then not kept.
 */
#ifndef CNF_TSFS_CACHE
#define CNF_TSFS_CACHE	( 32 * 1024 * 1024 )	/* bytes a volume keeps */
#endif
#define BC_MAX		( CNF_TSFS_CACHE / TSFSBLK_BLOCK_SIZE )
#define BC_BUCKETS	1024
#define BC_VALID	0
#define BC_LOADING	1
#define BC_STALE	2

typedef struct bcent {
	struct bcent	*hnext;		/* in its bucket */
	struct bcent	*prev, *next;	/* in the order of use */
	UD		blk;
	INT		state;
	UB		data[TSFSBLK_BLOCK_SIZE];
} BCENT;

LOCAL TSFSBLKVOL *bc_vol( ID vol )
{
	TSFSBLKVOL	*v;

	if ( vol < 1 || vol > TSFSBLK_MAX_VOL ) {
		return NULL;
	}
	v = &tsfsblk_vol[vol - 1];
	return ( v->used && v->bc_hash != NULL ) ? v : NULL;
}

LOCAL ER bc_init( TSFSBLKVOL *v )
{
	T_CMTX	cmtx;

	v->bc_hash = (BCENT **)Kmalloc(sizeof(BCENT *) * BC_BUCKETS);
	if ( v->bc_hash == NULL ) {
		return E_NOMEM;
	}
	knl_memset(v->bc_hash, 0, sizeof(BCENT *) * BC_BUCKETS);
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	v->bc_mtx = tk_cre_mtx(&cmtx);
	if ( v->bc_mtx <= 0 ) {
		Kfree(v->bc_hash);
		v->bc_hash = NULL;
		return (ER)v->bc_mtx;
	}
	v->bc_head = v->bc_tail = NULL;
	v->bc_n = v->bc_hits = v->bc_misses = 0;
	return E_OK;
}

LOCAL void bc_fini( TSFSBLKVOL *v )
{
	BCENT	*e, *n;

	if ( v->bc_hash == NULL ) {
		return;
	}
	for ( e = v->bc_head; e != NULL; e = n ) {
		n = e->next;
		Kfree(e);
	}
	Kfree(v->bc_hash);
	v->bc_hash = NULL;
	tk_del_mtx(v->bc_mtx);
}

LOCAL BCENT **bc_bucket( TSFSBLKVOL *v, UD blk )
{
	return &v->bc_hash[( blk ^ ( blk >> 10 ) ) % BC_BUCKETS];
}

LOCAL BCENT *bc_find( TSFSBLKVOL *v, UD blk )
{
	BCENT	*e;

	for ( e = *bc_bucket(v, blk); e != NULL && e->blk != blk; e = e->hnext ) ;
	return e;
}

LOCAL void bc_unuse( TSFSBLKVOL *v, BCENT *e )
{
	if ( e->prev != NULL ) e->prev->next = e->next; else v->bc_head = e->next;
	if ( e->next != NULL ) e->next->prev = e->prev; else v->bc_tail = e->prev;
	e->prev = e->next = NULL;
}

LOCAL void bc_use( TSFSBLKVOL *v, BCENT *e )
{
	e->prev = NULL;
	e->next = v->bc_head;
	if ( v->bc_head != NULL ) v->bc_head->prev = e;
	v->bc_head = e;
	if ( v->bc_tail == NULL ) v->bc_tail = e;
}

LOCAL void bc_unhash( TSFSBLKVOL *v, BCENT *e )
{
	BCENT	**pp;

	for ( pp = bc_bucket(v, e->blk); *pp != NULL && *pp != e; pp = &(*pp)->hnext ) ;
	if ( *pp == e ) *pp = e->hnext;
}

LOCAL void bc_remove( TSFSBLKVOL *v, BCENT *e )
{
	bc_unhash(v, e);
	bc_unuse(v, e);
	Kfree(e);
	v->bc_n--;
}

/* A place for a block: a new one, or the least recently used given up */
LOCAL BCENT *bc_room( TSFSBLKVOL *v, UD blk )
{
	BCENT	*e = NULL;

	if ( v->bc_n >= BC_MAX ) {
		for ( e = v->bc_tail; e != NULL && e->state == BC_LOADING; e = e->prev ) ;
		if ( e == NULL ) {
			return NULL;		/* all of it is being read */
		}
		bc_unhash(v, e);
		bc_unuse(v, e);
	} else {
		e = (BCENT *)Kmalloc(sizeof(BCENT));
		if ( e == NULL ) {
			return NULL;
		}
		v->bc_n++;
	}
	e->blk = blk;
	e->hnext = *bc_bucket(v, blk);
	*bc_bucket(v, blk) = e;
	bc_use(v, e);
	return e;
}

/*
 * A block from the cache. When it is not there, *p_e is the place its
 * bytes go once read (NULL when none could be made, or another is
 * reading it already).
 */
LOCAL BOOL bc_get( TSFSBLKVOL *v, UD blk, void *buf, BCENT **p_e )
{
	BCENT	*e;
	BOOL	hit = FALSE;

	*p_e = NULL;
	tk_loc_mtx(v->bc_mtx, TMO_FEVR);
	e = bc_find(v, blk);
	if ( e != NULL && e->state == BC_VALID ) {
		knl_memcpy(buf, e->data, TSFSBLK_BLOCK_SIZE);
		bc_unuse(v, e);
		bc_use(v, e);
		v->bc_hits++;
		hit = TRUE;
	} else {
		v->bc_misses++;
		if ( e == NULL ) {
			e = bc_room(v, blk);
			if ( e != NULL ) {
				e->state = BC_LOADING;
				*p_e = e;
			}
		}
	}
	tk_unl_mtx(v->bc_mtx);
	return hit;
}

/* The bytes read for a place bc_get made, kept unless a write came first */
LOCAL void bc_fill( TSFSBLKVOL *v, BCENT *e, CONST void *buf, BOOL ok )
{
	tk_loc_mtx(v->bc_mtx, TMO_FEVR);
	if ( ok && e->state == BC_LOADING ) {
		knl_memcpy(e->data, buf, TSFSBLK_BLOCK_SIZE);
		e->state = BC_VALID;
	} else {
		bc_remove(v, e);
	}
	tk_unl_mtx(v->bc_mtx);
}

/*
 * A block written to the device. `keep`: put it in the cache even when
 * it was not there (the volume's own blocks); else only a copy there is
 * brought up to date (the journal's writes in place). A failed write
 * leaves nothing behind.
 */
LOCAL void bc_write( TSFSBLKVOL *v, UD blk, CONST void *buf, BOOL ok, BOOL keep )
{
	BCENT	*e;

	tk_loc_mtx(v->bc_mtx, TMO_FEVR);
	e = bc_find(v, blk);
	if ( e != NULL && e->state == BC_LOADING ) {
		e->state = BC_STALE;
	} else if ( e != NULL && !ok ) {
		bc_remove(v, e);
	} else if ( ok ) {
		if ( e == NULL && keep ) {
			e = bc_room(v, blk);
		} else if ( e != NULL ) {
			bc_unuse(v, e);
			bc_use(v, e);
		}
		if ( e != NULL ) {
			knl_memcpy(e->data, buf, TSFSBLK_BLOCK_SIZE);
			e->state = BC_VALID;
		}
	}
	tk_unl_mtx(v->bc_mtx);
}

LOCAL ER cached_read( ID vol, ID dd, UD blk, void *buf )
{
	TSFSBLKVOL	*v = bc_vol(vol);
	BCENT		*e;
	ER		er;

	if ( v == NULL ) {
		return blk_read(dd, blk, buf);
	}
	if ( bc_get(v, blk, buf, &e) ) {
		return E_OK;
	}
	er = blk_read(dd, blk, buf);
	if ( e != NULL ) {
		bc_fill(v, e, buf, (BOOL)( er >= E_OK ));
	}
	return er;
}

/*
 * Whether a write goes to the device: after a pretended cut in the
 * power nothing does, and the caller is told it went.
 */
LOCAL BOOL cut_drops( TSFSBLKVOL *v )
{
	if ( v == NULL ) {
		return FALSE;
	}
	v->writes++;
	if ( !v->cut ) {
		return FALSE;
	}
	if ( v->cut_after > 0 ) {
		v->cut_after--;
		return FALSE;
	}
	return TRUE;
}

LOCAL ER cached_write( ID vol, ID dd, UD blk, CONST void *buf, BOOL keep )
{
	TSFSBLKVOL	*v = bc_vol(vol);
	ER		er;

	/* after a pretended cut the medium keeps what it had; the volume
	   goes on seeing what it wrote, as memory would until the power
	   took it */
	er = cut_drops(v) ? E_OK : blk_write(dd, blk, buf);

	if ( v != NULL ) {
		bc_write(v, blk, buf, (BOOL)( er >= E_OK ), keep);
	}
	return er;
}

/*
 * A management block of the volume read or written in its place. A
 * committed transaction may hold a newer copy the journal has not put
 * in place yet; a read takes that one, and a write in place tells the
 * journal first, so that the copy it holds does not come down on top
 * later.
 */
LOCAL ER meta_read( ID vol, ID dd, UD blk, void *buf )
{
	if ( vol > 0 && knl_jrnl_peek(vol, blk, buf) ) {
		return E_OK;
	}
	return cached_read(vol, dd, blk, buf);
}

LOCAL ER meta_write( ID vol, ID dd, UD blk, CONST void *buf )
{
	if ( vol > 0 ) {
		knl_jrnl_forget_blk(vol, blk);
	}
	return cached_write(vol, dd, blk, buf, TRUE);
}

/* ---------------------------------------------------------------- geometry */

/* Whether a group keeps a spare superblock: 1 and the powers of 3, 5, 7 */
LOCAL BOOL ag_is_spare( UD n )
{
	UD	b, p;

	if ( n == 1 ) {
		return TRUE;
	}
	for ( b = 3; b <= 7; b += 2 ) {
		for ( p = b; p <= n; p *= b ) {
			if ( p == n ) return TRUE;
		}
	}
	return FALSE;
}

/*
 * Where the journal and the groups go on a volume of `nblk` blocks.
 * The journal is a 256th of the volume, no less than one transaction's
 * room and no more than a gigabyte. A group is the largest power of two
 * up to a gigabyte that still makes four of them, and no smaller than
 * AG_MIN_BLOCKS.
 */
LOCAL ER layout( UD nblk, UD *p_jblocks, UD *p_ag_blocks, UD *p_ag_count )
{
	UD	j = nblk / 256, first, data, agb, n, tail;

	if ( j < TSFSJ_AREA_BLOCKS ) j = TSFSJ_AREA_BLOCKS;
	if ( j > JRNL_MAX_BLOCKS ) j = JRNL_MAX_BLOCKS;
	first = 2 + j;
	if ( nblk < TSFSBLK_MIN_BLOCKS || first + AG_MIN_TAIL >= nblk ) {
		return E_PAR;
	}
	data = nblk - first;
	for ( agb = AG_MAX_BLOCKS; agb > AG_MIN_BLOCKS && data / agb < 4; agb >>= 1 ) ;
	n = data / agb;
	tail = data % agb;
	if ( tail >= AG_MIN_TAIL ) n++;
	*p_jblocks = j;
	*p_ag_blocks = agb;
	*p_ag_count = n;
	return E_OK;
}

/* The shape of group `n` */
LOCAL void ag_shape( UD ag_first, UD agb, UD total, UD n, TSFSAG *a )
{
	UD	end;

	a->first = ag_first + n * agb;
	end = a->first + agb;
	if ( end > total ) end = total;
	a->len = end - a->first;
	a->spare = ag_is_spare(n);
	a->map_first = a->first + 1 + ( a->spare ? 1 : 0 );
	a->map_blocks = (UW)( ( a->len + BITS_PER_MAP - 1 ) / BITS_PER_MAP );
}

/* Blocks at the front of a group that the group uses itself */
LOCAL UD ag_meta( CONST TSFSAG *a )
{
	return ( a->map_first - a->first ) + a->map_blocks;
}

/* ---------------------------------------------------------------- the common head */

LOCAL void hd_seal( CONST TS_UUID *vol_uuid, UD gen, UB *buf, CONST char *magic, UD blk,
		    UD owner )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) buf[HD_MAGIC + i] = (UB)magic[i];
	wr32(buf + HD_MAGIC + 4, 0);
	wr64(buf + HD_BLK, blk);
	cpy16(buf + HD_VOLUUID, vol_uuid->b);
	wr64(buf + HD_OWNER, owner);
	wr64(buf + HD_GEN, gen);
	wr32(buf + HD_CRC, 0);
	for ( i = HD_CRC + 4; i < TSFSBLK_HDR_SIZE; i++ ) buf[i] = 0;
	wr32(buf + HD_CRC, ts_crc32c(buf, TSFSBLK_BLOCK_SIZE));
}

LOCAL BOOL hd_good( CONST TS_UUID *vol_uuid, CONST UB *buf, CONST char *magic, UD blk )
{
	UB	*tmp;
	UW	crc;
	INT	i;
	BOOL	ok = TRUE;

	for ( i = 0; i < 4; i++ ) {
		if ( buf[HD_MAGIC + i] != (UB)magic[i] ) return FALSE;
	}
	if ( rd64(buf + HD_BLK) != blk ) return FALSE;		/* written somewhere else */
	for ( i = 0; i < 16; i++ ) {
		if ( buf[HD_VOLUUID + i] != vol_uuid->b[i] ) return FALSE;	/* another volume's */
	}
	tmp = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( tmp == NULL ) {
		return FALSE;
	}
	knl_memcpy(tmp, buf, TSFSBLK_BLOCK_SIZE);
	crc = rd32(tmp + HD_CRC);
	wr32(tmp + HD_CRC, 0);
	if ( ts_crc32c(tmp, TSFSBLK_BLOCK_SIZE) != crc ) ok = FALSE;
	Kfree(tmp);
	return ok;
}

/* ---------------------------------------------------------------- group heads */

LOCAL void ag_pack_as( CONST TS_UUID *vol_uuid, UD gen, UD n, CONST TSFSAG *a, UB *buf )
{
	INT	k;

	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	wr64(buf + AG_NO, n);
	wr64(buf + AG_FIRST, a->first);
	wr64(buf + AG_LEN, a->len);
	wr64(buf + AG_FREE, a->free);
	wr64(buf + AG_MAPFIRST, a->map_first);
	wr32(buf + AG_MAPBLOCKS, a->map_blocks);
	wr32(buf + AG_SPARE, a->spare ? 1 : 0);
	for ( k = 0; k < (INT)a->map_blocks; k++ ) {
		wr32(buf + AG_MAPCRC + 4 * k, a->crc[k]);
	}
	hd_seal(vol_uuid, gen, buf, TSFSBLK_MAGIC_AGH, a->first, n);
}

LOCAL void ag_pack( CONST TSFSBLKVOL *v, UD n, UB *buf )
{
	ag_pack_as(&v->sb.vol_uuid, v->sb.journal_seq, n, &v->ag[n], buf);
}

LOCAL ER ag_unpack( TSFSBLKVOL *v, UD n, CONST UB *buf )
{
	TSFSAG	*a = &v->ag[n];
	TSFSAG	want;
	INT	k;

	ag_shape(v->ag_first, v->sb.ag_blocks, v->data_end, n, &want);
	if ( !hd_good(&v->sb.vol_uuid, buf, TSFSBLK_MAGIC_AGH, want.first)
	  || rd64(buf + AG_NO) != n || rd64(buf + AG_FIRST) != want.first
	  || rd64(buf + AG_LEN) != want.len || rd64(buf + AG_MAPFIRST) != want.map_first
	  || rd32(buf + AG_MAPBLOCKS) != want.map_blocks || rd64(buf + AG_FREE) > want.len ) {
		return E_OBJ;
	}
	*a = want;
	a->free = rd64(buf + AG_FREE);
	for ( k = 0; k < (INT)a->map_blocks; k++ ) {
		a->crc[k] = rd32(buf + AG_MAPCRC + 4 * k);
	}
	a->dirty = FALSE;
	return E_OK;
}

/* ---------------------------------------------------------------- errors */

/*
 * Something on the medium is not as it should be. The volume stops
 * writing, so that the damage does not spread, and the superblock says
 * so; it opens read only until it has been checked. With a transaction
 * open the note waits: the transaction will not be made real, and the
 * superblock read back after throwing it away gets the note again.
 */
LOCAL void note_error( TSFSBLKVOL *v, UD blk, UW kind )
{
	TS_TIME	t = now_time();

	v->rdonly = TRUE;
	v->err_kind = kind;
	v->err_blk = blk;
	if ( v->err_first == 0 ) v->err_first = (UD)t;
	v->err_last = (UD)t;
	v->sb_dirty = TRUE;
}

LOCAL void err_merge( TSFSBLKVOL *v )
{
	if ( v->err_kind == 0 ) {
		return;
	}
	v->sb.state |= TSFSBLK_ST_ERROR;
	v->sb.err_kind = v->err_kind;
	v->sb.err_blk = v->err_blk;
	if ( v->sb.err_first == 0 ) v->sb.err_first = v->err_first;
	v->sb.err_last = v->err_last;
}

/* The superblock, to the copy the last write did not touch */
LOCAL ER sb_write( TSFSBLKVOL *v )
{
	UB	*buf;
	ER	er;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	err_merge(v);
	v->sb.generation++;
	sb_pack(&v->sb, buf);
	er = meta_write(VOL_ID(v), v->dd, v->sb.generation & 1, buf);
	Kfree(buf);
	if ( er < E_OK ) {
		return E_IO;
	}
	if ( dev_flush(v->dd) == E_NOSPT && ( v->sb.state & TSFSBLK_ST_NOFLUSH ) == 0 ) {
		v->sb.state |= TSFSBLK_ST_NOFLUSH;	/* written next time */
		return E_OK;
	}
	v->sb_dirty = FALSE;

	return E_OK;
}

/* ---------------------------------------------------------------- format */

EXPORT ER ts_format_blk( CONST char *devnm, CONST char *label )
{
	T_TSFSBLK_SB	sb;
	TSFSAG		a;
	DiskInfo	info;
	UB		*buf, *sbbuf;
	UD		nblk, jblocks, agb, agn, n, k, bit, meta;
	SZ		asize;
	ID		dd;
	ER		er = E_OK;
	INT		i;

	if ( devnm == NULL ) {
		return E_PAR;
	}
	dd = tk_opn_dev((UB *)devnm, TD_UPDATE);
	if ( dd < E_OK ) {
		return E_NOEXS;
	}
	er = tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asize);
	if ( er < E_OK ) {
		tk_cls_dev(dd, 0);
		return E_IO;
	}
	nblk = (UD)info.blockcont / TSFSBLK_SECT_PER_BLK;
	if ( layout(nblk, &jblocks, &agb, &agn) < E_OK ) {
		tk_cls_dev(dd, 0);
		return E_PAR;			/* too small to be a volume */
	}

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	sbbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL || sbbuf == NULL ) {
		er = E_NOMEM;
		goto exit;
	}

	knl_memset(&sb, 0, sizeof(sb));
	for ( i = 0; i < 8; i++ ) sb.magic[i] = (UB)TSFSBLK_MAGIC[i];
	sb.version        = TSFSBLK_VERSION;
	sb.block_size     = TSFSBLK_BLOCK_SIZE;
	sb.total_blocks   = nblk;
	sb.ag_blocks      = agb;
	sb.ag_count       = agn;
	sb.journal_start  = 2;
	sb.journal_blocks = jblocks;
	/* A ring left on these blocks by a volume made before must not be
	   taken for this one's: its numbers start somewhere else. */
	{
		UW	r = 0;

		(void)ts_get_random(&r, sizeof(r));
		sb.journal_seq = ( (UD)r << 8 ) + 1;
	}
	sb.state          = TSFSBLK_ST_CLEAN;
	sb.mkfs_time      = (UD)now_time();
	ts_gen_uuid(&sb.vol_uuid);		/* an unset clock only costs the UUID */
	for ( i = 0; i < TSFSBLK_LABEL_MAX - 1 && label != NULL && label[i] != '\0'; i++ ) {
		sb.label[i] = (UB)label[i];
	}

	/* the journal holds no record */
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	if ( blk_write(dd, sb.journal_start, buf) < E_OK ) { er = E_IO; goto exit; }

	/* each group: its map, then its head; the spare superblock last */
	sb.free_blocks = 0;
	for ( n = 0; n < agn; n++ ) {
		ag_shape(2 + jblocks, agb, 2 + jblocks + agn * agb < nblk ? 2 + jblocks + agn * agb : nblk,
			 n, &a);
		meta = ag_meta(&a);
		for ( k = 0; k < a.map_blocks; k++ ) {
			knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
			for ( bit = 0; bit < BITS_PER_MAP; bit++ ) {
				UD	off = k * BITS_PER_MAP + bit;

				if ( off < meta || off >= a.len ) {
					buf[bit / 8] |= (UB)( 1 << ( bit % 8 ) );
				}
			}
			a.crc[k] = ts_crc32c(buf, TSFSBLK_BLOCK_SIZE);
			if ( blk_write(dd, a.map_first + k, buf) < E_OK ) { er = E_IO; goto exit; }
		}
		a.free = a.len - meta;
		sb.free_blocks += a.free;

		ag_pack_as(&sb.vol_uuid, sb.journal_seq, n, &a, buf);
		if ( blk_write(dd, a.first, buf) < E_OK ) { er = E_IO; goto exit; }
	}

	/* both copies, the higher generation in A; spares as A */
	sb.generation = 1;
	sb_pack(&sb, sbbuf);
	if ( blk_write(dd, 1, sbbuf) < E_OK ) { er = E_IO; goto exit; }
	sb.generation = 2;
	sb_pack(&sb, sbbuf);
	if ( blk_write(dd, 0, sbbuf) < E_OK ) { er = E_IO; goto exit; }
	for ( n = 1; n < agn; n++ ) {
		if ( ag_is_spare(n) ) {
			if ( blk_write(dd, 2 + jblocks + n * agb + 1, sbbuf) < E_OK ) {
				er = E_IO;
				goto exit;
			}
		}
	}
	(void)dev_flush(dd);

    exit:
	if ( buf != NULL ) Kfree(buf);
	if ( sbbuf != NULL ) Kfree(sbbuf);
	tk_cls_dev(dd, 0);

	return er;
}

/* ---------------------------------------------------------------- volume */

LOCAL TSFSBLKVOL *vol_of( ID vol )
{
	if ( vol < 1 || vol > TSFSBLK_MAX_VOL || !tsfsblk_vol[vol - 1].used ) {
		return NULL;
	}
	return &tsfsblk_vol[vol - 1];
}

/* The valid superblock of the higher generation */
LOCAL ER sb_load( ID vol, ID dd, T_TSFSBLK_SB *sb, UB *buf )
{
	T_TSFSBLK_SB	s[2];
	ER		er[2];
	INT		i;

	for ( i = 0; i < 2; i++ ) {
		er[i] = ( meta_read(vol, dd, (UD)i, buf) >= E_OK ) ? sb_unpack(buf, &s[i]) : E_IO;
	}
	if ( er[0] < E_OK && er[1] < E_OK ) {
		return ( er[0] == E_NOSPT || er[1] == E_NOSPT ) ? E_NOSPT : E_OBJ;
	}
	if ( er[1] < E_OK || ( er[0] >= E_OK && s[0].generation >= s[1].generation ) ) {
		*sb = s[0];
	} else {
		*sb = s[1];
	}
	return E_OK;
}

/* The heads of the groups, into the table */
LOCAL ER ag_load( TSFSBLKVOL *v, UD n, UB *buf )
{
	TSFSAG	a;

	ag_shape(v->ag_first, v->sb.ag_blocks, v->data_end, n, &a);
	if ( meta_read(VOL_ID(v), v->dd, a.first, buf) < E_OK ) {
		return E_IO;
	}
	return ag_unpack(v, n, buf);
}

/*
 * Read what the volume is: the superblock, then every group head.
 * A group head that fails its check is an error on the volume.
 */
LOCAL ER vol_load( TSFSBLKVOL *v )
{
	UB	*buf;
	UD	n, sum = 0;
	ER	er;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = sb_load(VOL_ID(v), v->dd, &v->sb, buf);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	v->ag_first = v->sb.journal_start + v->sb.journal_blocks;
	v->data_end = v->ag_first + v->sb.ag_count * v->sb.ag_blocks;
	if ( v->data_end > v->sb.total_blocks ) v->data_end = v->sb.total_blocks;
	if ( v->ag == NULL ) {
		v->ag = (TSFSAG *)Kmalloc(sizeof(TSFSAG) * v->sb.ag_count);
		if ( v->ag == NULL ) {
			Kfree(buf);
			return E_NOMEM;
		}
	}
	for ( n = 0; n < v->sb.ag_count; n++ ) {
		if ( ag_load(v, n, buf) < E_OK ) {
			TSFSAG	a;

			ag_shape(v->ag_first, v->sb.ag_blocks, v->data_end, n, &a);
			note_error(v, a.first, TSFSBLK_ERR_CSUM);
			v->ag[n] = a;
			v->ag[n].free = 0;		/* nothing is taken from it */
			continue;
		}
		sum += v->ag[n].free;
	}
	v->sb.free_blocks = sum;
	Kfree(buf);
	return E_OK;
}

/* Which group a block is in, or -1 outside every group */
LOCAL INT ag_of( CONST TSFSBLKVOL *v, UD blk )
{
	UD	n;

	if ( blk < v->ag_first || blk >= v->data_end ) {
		return -1;
	}
	n = ( blk - v->ag_first ) / v->sb.ag_blocks;
	return ( n < v->sb.ag_count ) ? (INT)n : -1;
}

/* ---------------------------------------------------------------- the map cache */

/* A map block held back by the open transaction, or -1 */
LOCAL INT held_of( CONST TSFSBLKVOL *v, UD mapblk )
{
	INT	i;

	for ( i = 0; i < v->held_n; i++ ) {
		if ( v->held_blk[i] == mapblk ) return i;
	}
	return -1;
}

/*
 * Put the map block in the cache where it belongs. Its checksum goes
 * into its group head, which now has to be written too. While a
 * transaction is open it goes nowhere yet: it is held in memory until
 * the record that makes the transaction real is written, so that a
 * machine whose power goes first comes back with the map it had.
 * Holding it costs a buffer; a transaction that changes more map blocks
 * than there are buffers is refused room (E_LIMIT) rather than let one
 * out ahead of its record.
 */
LOCAL ER map_evict( TSFSBLKVOL *v )
{
	TSFSAG	*a;
	INT	i;

	if ( !v->cache_dirty || v->cache_blk == ~(UD)0 ) {
		return E_OK;
	}
	a = &v->ag[v->cache_ag];
	a->crc[v->cache_k] = ts_crc32c(v->cache, TSFSBLK_BLOCK_SIZE);
	a->dirty = TRUE;
	if ( v->jrnl_on ) {
		i = held_of(v, v->cache_blk);
		if ( i < 0 ) {
			if ( v->held_n >= TSFSBLK_HELD_MAP ) {
				return E_LIMIT;
			}
			i = v->held_n;
			if ( v->held_buf[i] == NULL ) {
				v->held_buf[i] = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
				if ( v->held_buf[i] == NULL ) return E_NOMEM;
			}
			v->held_n = i + 1;
		}
		knl_memcpy(v->held_buf[i], v->cache, TSFSBLK_BLOCK_SIZE);
		v->held_blk[i] = v->cache_blk;
		v->held_ag[i] = v->cache_ag;
		v->held_k[i] = v->cache_k;
		v->cache_dirty = FALSE;
		return E_OK;
	}
	if ( meta_write(VOL_ID(v), v->dd, v->cache_blk, v->cache) < E_OK ) {
		note_error(v, v->cache_blk, TSFSBLK_ERR_IO);
		return E_IO;
	}
	v->cache_dirty = FALSE;

	/* the head right behind it, so the two never disagree for long */
	{
		UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);

		if ( buf == NULL ) {
			return E_NOMEM;
		}
		ag_pack(v, v->cache_ag, buf);
		if ( meta_write(VOL_ID(v), v->dd, a->first, buf) < E_OK ) {
			Kfree(buf);
			note_error(v, a->first, TSFSBLK_ERR_IO);
			return E_IO;
		}
		Kfree(buf);
		a->dirty = FALSE;
	}

	return E_OK;
}

/*
 * The map block `k` of group `n` in the cache. One held back by the
 * open transaction is taken from there, since the medium does not have
 * it yet; one read from the medium has to match the checksum its group
 * head keeps.
 */
LOCAL ER cache_load( TSFSBLKVOL *v, UD n, UW k )
{
	UD	mapblk = v->ag[n].map_first + k;
	INT	i;
	ER	er;

	if ( v->cache_blk == mapblk ) {
		return E_OK;
	}
	er = map_evict(v);
	if ( er < E_OK ) {
		return er;
	}
	i = held_of(v, mapblk);
	if ( i >= 0 ) {
		knl_memcpy(v->cache, v->held_buf[i], TSFSBLK_BLOCK_SIZE);
	} else {
		if ( meta_read(VOL_ID(v), v->dd, mapblk, v->cache) < E_OK ) {
			v->cache_blk = ~(UD)0;
			return E_IO;
		}
		if ( ts_crc32c(v->cache, TSFSBLK_BLOCK_SIZE) != v->ag[n].crc[k] ) {
			v->cache_blk = ~(UD)0;
			note_error(v, mapblk, TSFSBLK_ERR_CSUM);
			return E_IO;
		}
	}
	v->cache_blk = mapblk;
	v->cache_ag = n;
	v->cache_k = k;

	return E_OK;
}

/*
 * Everything owed to the medium, with no transaction open: the map
 * block, then the group heads whose checksums it moved, then the
 * superblock.
 */
LOCAL ER cache_flush( TSFSBLKVOL *v )
{
	UB	*buf;
	UD	n;
	ER	er;

	er = map_evict(v);
	if ( er < E_OK ) {
		return er;
	}
	if ( v->jrnl_on ) {
		return E_OK;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) return E_NOMEM;
	for ( n = 0; n < v->sb.ag_count && er >= E_OK; n++ ) {
		if ( !v->ag[n].dirty ) continue;
		ag_pack(v, n, buf);
		if ( meta_write(VOL_ID(v), v->dd, v->ag[n].first, buf) < E_OK ) {
			note_error(v, v->ag[n].first, TSFSBLK_ERR_IO);
			er = E_IO;
		} else {
			v->ag[n].dirty = FALSE;
		}
	}
	Kfree(buf);
	if ( er >= E_OK && v->sb_dirty ) {
		er = sb_write(v);
	}

	return er;
}

EXPORT ID ts_opn_vol_blk( CONST char *devnm )
{
	T_CMTX		cmtx;
	TSFSBLKVOL	*v = NULL;
	ID		dd, vol;
	INT		i;
	ER		er;

	if ( devnm == NULL ) {
		return E_PAR;
	}
	if ( tsfsblk_mtxid <= 0 ) {
		cmtx.exinf = NULL;
		cmtx.mtxatr = TA_TFIFO;
		tsfsblk_mtxid = tk_cre_mtx(&cmtx);
		if ( tsfsblk_mtxid <= 0 ) return (ER)tsfsblk_mtxid;
	}
	tk_loc_mtx(tsfsblk_mtxid, TMO_FEVR);
	for ( i = 0; i < TSFSBLK_MAX_VOL; i++ ) {
		if ( !tsfsblk_vol[i].used ) { v = &tsfsblk_vol[i]; break; }
	}
	if ( v != NULL ) {
		knl_memset(v, 0, sizeof(*v));
		v->used = TRUE;			/* claimed while it is being opened */
	}
	tk_unl_mtx(tsfsblk_mtxid);
	if ( v == NULL ) {
		return E_LIMIT;
	}

	dd = tk_opn_dev((UB *)devnm, TD_UPDATE);
	if ( dd < E_OK ) {
		v->used = FALSE;
		return E_NOEXS;
	}
	v->dd = dd;
	io_open(dd);
	v->cache_blk = ~(UD)0;
	v->cache = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	v->mtxid = tk_cre_mtx(&cmtx);
	er = ( v->cache == NULL ) ? E_NOMEM : ( v->mtxid <= 0 ) ? (ER)v->mtxid : bc_init(v);
	if ( er >= E_OK ) er = vol_load(v);
	if ( er < E_OK ) {
		goto fail;
	}
	if ( ( v->sb.incompat & ~KNOWN_INCOMPAT ) != 0 ) {
		er = E_NOSPT;			/* a feature this code cannot read */
		goto fail;
	}
	vol = (ID)( v - tsfsblk_vol ) + 1;

	/* Whatever a cut in the power left behind goes back before
	   anything else reads the metadata, and the volume is read again
	   as the log left it. */
	if ( ( v->sb.state & TSFSBLK_ST_ERROR ) == 0 ) {
		ts_jrnl_replay(vol);
		er = vol_load(v);
		if ( er < E_OK ) {
			goto fail;
		}
	}
	if ( ( v->sb.state & TSFSBLK_ST_ERROR ) != 0
	  || ( v->sb.ro_compat & ~KNOWN_ROCOMPAT ) != 0 ) {
		v->rdonly = TRUE;		/* until it has been checked */
	}
	v->hint = v->ag_first;
	if ( !v->rdonly ) {
		v->sb.mount_count++;
		v->sb.mount_time = (UD)now_time();
		v->sb.state &= (UW)~TSFSBLK_ST_CLEAN;	/* in use */
		v->sb_dirty = TRUE;
		cache_flush(v);
	}
	return vol;

    fail:
	bc_fini(v);
	if ( v->mtxid > 0 ) tk_del_mtx(v->mtxid);
	if ( v->cache != NULL ) Kfree(v->cache);
	if ( v->ag != NULL ) Kfree(v->ag);
	io_close(dd);
	tk_cls_dev(dd, 0);
	v->used = FALSE;
	return er;
}

EXPORT ER ts_cls_vol_blk( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er = E_OK;
	INT		i;

	if ( v == NULL ) {
		return E_ID;
	}
	/* what the journal holds goes in place before the last superblock */
	(void)knl_jrnl_close(vol);

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( !v->rdonly ) {
		v->sb.state |= TSFSBLK_ST_CLEAN;	/* taken down properly */
		v->sb_dirty = TRUE;
		er = cache_flush(v);
	} else if ( v->err_kind != 0 ) {
		er = sb_write(v);		/* the error is kept, nothing else */
	}
	tk_unl_mtx(v->mtxid);

	tk_del_mtx(v->mtxid);
	bc_fini(v);
	for ( i = 0; i < TSFSBLK_HELD_MAP; i++ ) {
		if ( v->held_buf[i] != NULL ) Kfree(v->held_buf[i]);
	}
	Kfree(v->cache);
	Kfree(v->ag);
	io_close(v->dd);
	tk_cls_dev(v->dd, 0);
	v->used = FALSE;

	return er;
}

EXPORT ER ts_ref_vol_blk( ID vol, T_TSFSBLK_SB *pk_sb )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL || pk_sb == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	err_merge(v);
	*pk_sb = v->sb;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/* ---------------------------------------------------------------- blocks */

LOCAL ER bit_get( TSFSBLKVOL *v, UD n, UD blk, BOOL *p_used )
{
	UD	off = blk - v->ag[n].first;
	UD	bit = off % BITS_PER_MAP;
	ER	er;

	er = cache_load(v, n, (UW)( off / BITS_PER_MAP ));
	if ( er < E_OK ) {
		return er;
	}
	*p_used = ( ( v->cache[bit / 8] & ( 1 << ( bit % 8 ) ) ) != 0 );

	return E_OK;
}

LOCAL ER bit_put( TSFSBLKVOL *v, UD n, UD blk, BOOL used )
{
	UD	off = blk - v->ag[n].first;
	UD	bit = off % BITS_PER_MAP;
	ER	er;

	er = cache_load(v, n, (UW)( off / BITS_PER_MAP ));
	if ( er < E_OK ) {
		return er;
	}
	if ( used ) {
		v->cache[bit / 8] |=  (UB)( 1 << ( bit % 8 ) );
	} else {
		v->cache[bit / 8] &= (UB)~( 1 << ( bit % 8 ) );
	}
	v->cache_dirty = TRUE;

	return E_OK;
}

/*
 * Look for a run of free blocks of group `n` in [from, to), taking the
 * first one found and stretching it up to `want` blocks.
 */
LOCAL ER run_find( TSFSBLKVOL *v, UD n, UD from, UD to, UD want,
		   UD *p_start, UD *p_count )
{
	UD	blk, k;
	BOOL	used;
	ER	er;

	for ( blk = from; blk < to; blk++ ) {
		er = bit_get(v, n, blk, &used);
		if ( er < E_OK ) {
			return er;
		}
		if ( used ) {
			continue;
		}
		for ( k = 1; k < want && blk + k < to; k++ ) {
			er = bit_get(v, n, blk + k, &used);
			if ( er < E_OK ) {
				return er;
			}
			if ( used ) {
				break;
			}
		}
		*p_start = blk;
		*p_count = k;
		return E_OK;
	}

	return E_NOMEM;
}

/*
 * Take a run of up to `want` blocks. Fewer are given when the volume is
 * broken up, so the caller has to look at what came back; a run of at
 * least one is always returned or the volume is full. A run lies in one
 * group.
 *
 * The search starts where the last one left off, which keeps a run of
 * writes together, goes through the groups in turn from there, and
 * wraps once so that blocks freed behind it are found.
 */
EXPORT ER ts_alloc_ext( ID vol, UD want, UD *p_start, UD *p_count )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UD		start = 0, count = 0, i, n, from, lo, hi;
	INT		h;
	ER		er = E_NOMEM;

	if ( v == NULL || p_start == NULL || p_count == NULL ) {
		return E_ID;
	}
	if ( want == 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->rdonly ) {
		er = E_RONLY;
		goto exit;
	}
	h = ag_of(v, v->hint);
	if ( h < 0 ) {
		h = 0;
		v->hint = v->ag[0].first;
	}
	/* the hint's group from the hint on, the others whole, then the
	   hint's group before the hint */
	for ( i = 0; i <= v->sb.ag_count && er == E_NOMEM; i++ ) {
		n = ( (UD)h + i ) % v->sb.ag_count;
		lo = v->ag[n].first + ag_meta(&v->ag[n]);
		hi = v->ag[n].first + v->ag[n].len;
		if ( v->ag[n].free == 0 ) continue;
		if ( i == 0 ) {
			from = ( v->hint > lo ) ? v->hint : lo;
		} else if ( i == v->sb.ag_count ) {
			hi = ( v->hint > lo ) ? v->hint : lo;
			from = lo;
		} else {
			from = lo;
		}
		er = run_find(v, n, from, hi, want, &start, &count);
	}
	if ( er < E_OK ) {
		goto exit;
	}
	n = (UD)ag_of(v, start);
	for ( i = 0; i < count; i++ ) {
		er = bit_put(v, n, start + i, TRUE);
		if ( er < E_OK ) {
			/* put back what was taken, so the map stays true */
			while ( i > 0 ) {
				bit_put(v, n, start + --i, FALSE);
			}
			goto exit;
		}
	}
	v->ag[n].free -= count;
	v->ag[n].dirty = TRUE;
	v->sb.free_blocks -= count;
	v->hint = start + count;

	*p_start = start;
	*p_count = count;

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

EXPORT ER ts_alloc_ext_at( ID vol, UD blk, UD want, UD *p_count )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UD		i, end;
	INT		n;
	BOOL		used;
	ER		er = E_OK;

	if ( v == NULL || p_count == NULL ) {
		return E_ID;
	}
	if ( want == 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->rdonly ) {
		er = E_RONLY;
		goto exit;
	}
	n = ag_of(v, blk);
	if ( n < 0 || blk < v->ag[n].first + ag_meta(&v->ag[n]) ) {
		er = E_NOMEM;
		goto exit;
	}
	end = v->ag[n].first + v->ag[n].len;
	if ( want > end - blk ) {
		want = end - blk;
	}
	for ( i = 0; i < want; i++ ) {
		er = bit_get(v, (UD)n, blk + i, &used);
		if ( er < E_OK ) goto exit;
		if ( used ) break;
	}
	if ( i == 0 ) {
		er = E_NOMEM;			/* the block is taken */
		goto exit;
	}
	want = i;
	for ( i = 0; i < want; i++ ) {
		er = bit_put(v, (UD)n, blk + i, TRUE);
		if ( er < E_OK ) {
			while ( i > 0 ) bit_put(v, (UD)n, blk + --i, FALSE);
			goto exit;
		}
	}
	v->ag[n].free -= want;
	v->ag[n].dirty = TRUE;
	v->sb.free_blocks -= want;
	*p_count = want;

    exit:
	tk_unl_mtx(v->mtxid);
	return er;
}

/*
 * Give a run back. Every block of it has to be a data block of one
 * group that was taken.
 */
EXPORT ER ts_free_ext( ID vol, UD start, UD count )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UD		i;
	INT		n;
	BOOL		used;
	ER		er = E_OK;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( count == 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->rdonly ) {
		er = E_RONLY;
		goto exit;
	}
	/* what the volume uses itself is never given back */
	n = ag_of(v, start);
	if ( n < 0 || start < v->ag[n].first + ag_meta(&v->ag[n])
	  || count > v->ag[n].first + v->ag[n].len - start ) {
		er = E_PAR;
		goto exit;
	}
	for ( i = 0; i < count; i++ ) {
		er = bit_get(v, (UD)n, start + i, &used);
		if ( er < E_OK ) {
			goto exit;
		}
		if ( !used ) {
			er = E_OBJ;		/* part of it was already free */
			goto exit;
		}
	}
	for ( i = 0; i < count; i++ ) {
		er = bit_put(v, (UD)n, start + i, FALSE);
		if ( er < E_OK ) {
			goto exit;
		}
	}
	v->ag[n].free += count;
	v->ag[n].dirty = TRUE;
	v->sb.free_blocks += count;
	knl_jrnl_revoke(vol, start, count);	/* a copy of what was here must not come back */
	if ( start < v->hint ) {
		v->hint = start;		/* fill the hole before going on */
	}

    exit:
	tk_unl_mtx(v->mtxid);

	return er;
}

/* One block is a run of one */
EXPORT ER ts_alloc_blk( ID vol, UD *p_blk )
{
	UD	start = 0, count = 0;
	ER	er;

	if ( p_blk == NULL ) {
		return E_ID;
	}
	er = ts_alloc_ext(vol, 1, &start, &count);
	if ( er >= E_OK ) {
		*p_blk = start;
	}

	return er;
}

EXPORT ER ts_free_blk( ID vol, UD blk )
{
	return ts_free_ext(vol, blk, 1);
}

/* ---------------------------------------------------------------- the common head */

EXPORT ER ts_blk_seal( ID vol, UB *buf, CONST char *magic, UD blk, UD owner )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL || buf == NULL || magic == NULL ) {
		return E_PAR;
	}
	hd_seal(&v->sb.vol_uuid, v->sb.journal_seq, buf, magic, blk, owner);
	return E_OK;
}

EXPORT ER ts_blk_check( ID vol, CONST UB *buf, CONST char *magic, UD blk )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL || buf == NULL || magic == NULL ) {
		return E_PAR;
	}
	if ( hd_good(&v->sb.vol_uuid, buf, magic, blk) ) {
		return E_OK;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	note_error(v, blk, TSFSBLK_ERR_CSUM);
	if ( !v->jrnl_on ) {
		(void)sb_write(v);
	}
	tk_unl_mtx(v->mtxid);
	return E_OBJ;
}

EXPORT void ts_blk_error( ID vol, UD blk, UW kind )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	note_error(v, blk, kind);
	if ( !v->jrnl_on ) {
		(void)sb_write(v);
	}
	tk_unl_mtx(v->mtxid);
}

/* ---------------------------------------------------------------- roots */

/*
 * Where the object index has its root, and how many blocks it takes.
 * The index itself lives in tsfsbtree.c; the superblock only holds the
 * two numbers, so that a volume can be opened without walking the tree.
 */
EXPORT ER ts_get_root_blk( ID vol, UD *p_root, UD *p_nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( p_root != NULL ) *p_root = v->sb.objtbl_start;
	if ( p_nodes != NULL ) *p_nodes = v->sb.objtbl_blocks;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

EXPORT ER ts_set_root_blk( ID vol, UD root, UD nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( root != 0 && root >= v->sb.total_blocks ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	/* Marking the superblock dirty costs a write, so it is only done
	   when something moved. The index says where its root is after
	   every insert, and almost every one leaves it where it was. */
	if ( v->sb.objtbl_start != root || v->sb.objtbl_blocks != nodes ) {
		v->sb.objtbl_start  = root;
		v->sb.objtbl_blocks = nodes;
		v->sb_dirty = TRUE;
	}
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

EXPORT ER ts_get_gc_root_blk( ID vol, UD *p_root, UD *p_nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( p_root != NULL ) *p_root = v->sb.gclist_start;
	if ( p_nodes != NULL ) *p_nodes = v->sb.gclist_blocks;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

EXPORT ER ts_set_gc_root_blk( ID vol, UD root, UD nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( root != 0 && root >= v->sb.total_blocks ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->sb.gclist_start != root || v->sb.gclist_blocks != (UW)nodes ) {
		v->sb.gclist_start  = root;
		v->sb.gclist_blocks = (UW)nodes;
		v->sb_dirty = TRUE;
	}
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

EXPORT ER ts_get_orphan_root_blk( ID vol, UD *p_root, UD *p_nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( p_root != NULL ) *p_root = v->sb.orphan_start;
	if ( p_nodes != NULL ) *p_nodes = v->sb.orphan_blocks;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

EXPORT ER ts_set_orphan_root_blk( ID vol, UD root, UD nodes )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( root != 0 && root >= v->sb.total_blocks ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->sb.orphan_start != root || v->sb.orphan_blocks != (UW)nodes ) {
		v->sb.orphan_start  = root;
		v->sb.orphan_blocks = (UW)nodes;
		v->sb_dirty = TRUE;
	}
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/* ---------------------------------------------------------------- transactions */

/*
 * While this is on, the free maps, the group heads and the superblock
 * are not written where they belong: they wait for the record that
 * makes the open transaction real, and go into the log with everything
 * else.
 */
EXPORT ER ts_blk_jrnl( ID vol, BOOL on )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( on && !v->jrnl_on ) {
		/* Anything owed from before goes to the medium first. What
		   is held back after this belongs to the transaction alone,
		   so throwing it away throws away nothing else. */
		cache_flush(v);
	}
	v->jrnl_on = on;
	if ( !on ) {
		v->held_n = 0;
	}
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/*
 * Throw the open transaction's changes away. What was held back goes,
 * the cache goes with it, and the superblock and the group heads are read
 * again from the medium: their counts followed the maps into a change
 * that is not happening.
 */
EXPORT ER ts_blk_jrnl_discard( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	T_TSFSBLK_SB	sb;
	UB		*buf;
	UD		n, sum = 0;

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);

	v->jrnl_on = FALSE;
	v->held_n = 0;
	v->cache_dirty = FALSE;
	v->cache_blk = ~(UD)0;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf != NULL ) {
		if ( sb_load(VOL_ID(v), v->dd, &sb, buf) >= E_OK ) {
			/* what this run of the system knows stays */
			sb.mount_count = v->sb.mount_count;
			sb.mount_time = v->sb.mount_time;
			sb.state = v->sb.state;
			sb.generation = v->sb.generation;
			v->sb = sb;
			v->sb_dirty = FALSE;
		}
		for ( n = 0; n < v->sb.ag_count; n++ ) {
			if ( ag_load(v, n, buf) < E_OK ) {
				note_error(v, v->ag[n].first, TSFSBLK_ERR_CSUM);
			}
			sum += v->ag[n].free;
		}
		v->sb.free_blocks = sum;
		Kfree(buf);
	}
	v->hint = v->ag_first;
	if ( v->err_kind != 0 ) {
		(void)sb_write(v);
	}

	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/*
 * Hand everything the open transaction changed about the maps, the
 * group heads and the superblock to the log. Called with no lock of this
 * volume held, because writing into the log goes back through the block
 * calls. The superblock goes to the copy the last write did not touch.
 */
EXPORT ER ts_blk_jrnl_flush( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UB		*blocks[TSFSBLK_HELD_MAP + 1];
	UD		where[TSFSBLK_HELD_MAP + 1];
	UB		*agbuf = NULL, *sbbuf = NULL;
	UD		sbwhere = 0, n;
	INT		nb = 0, i;
	ER		er = E_OK;

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);

	/* whatever is still in the cache joins what was held back */
	er = map_evict(v);
	if ( er < E_OK ) {
		tk_unl_mtx(v->mtxid);
		return er;
	}
	for ( i = 0; i < v->held_n; i++ ) {
		v->ag[v->held_ag[i]].crc[v->held_k[i]] = ts_crc32c(v->held_buf[i], TSFSBLK_BLOCK_SIZE);
		blocks[nb] = v->held_buf[i];
		where[nb] = v->held_blk[i];
		nb++;
	}
	v->held_n = 0;
	tk_unl_mtx(v->mtxid);

	for ( i = 0; i < nb && er >= E_OK; i++ ) {
		er = ts_jrnl_write(vol, where[i], blocks[i]);
	}

	/* the heads of the groups the maps belong to */
	agbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( agbuf == NULL ) {
		return E_NOMEM;
	}
	for ( n = 0; n < v->sb.ag_count && er >= E_OK; n++ ) {
		if ( !v->ag[n].dirty ) continue;
		tk_loc_mtx(v->mtxid, TMO_FEVR);
		ag_pack(v, n, agbuf);
		v->ag[n].dirty = FALSE;
		tk_unl_mtx(v->mtxid);
		er = ts_jrnl_write(vol, v->ag[n].first, agbuf);
	}
	Kfree(agbuf);

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->sb_dirty ) {
		sbbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( sbbuf != NULL ) {
			err_merge(v);
			v->sb.generation++;
			sb_pack(&v->sb, sbbuf);
			sbwhere = v->sb.generation & 1;
			v->sb_dirty = FALSE;
		}
	}
	tk_unl_mtx(v->mtxid);
	if ( er >= E_OK && sbbuf != NULL ) {
		er = ts_jrnl_write(vol, sbwhere, sbbuf);
	}
	if ( sbbuf != NULL ) {
		Kfree(sbbuf);
	}

	return er;
}

EXPORT ER ts_get_jrnl_blk( ID vol, UD *p_start, UD *p_blocks, UD *p_seq )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( p_start != NULL )  *p_start  = v->sb.journal_start;
	if ( p_blocks != NULL ) *p_blocks = v->sb.journal_blocks;
	if ( p_seq != NULL )    *p_seq    = v->sb.journal_seq;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/*
 * The journal area is fixed when the volume is made; what moves is the
 * number the next transaction takes. That number lives in the superblock
 * but is not worth a write of its own: it goes out with the next
 * superblock write, and the log's own records carry it meanwhile.
 */
EXPORT ER ts_set_jrnl_blk( ID vol, UD start, UD blocks, UD seq )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( start != v->sb.journal_start || blocks != v->sb.journal_blocks ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	v->sb.journal_seq = seq;
	tk_unl_mtx(v->mtxid);

	return E_OK;
}

/* ---------------------------------------------------------------- data */

/* Whether a block is a group's head, spare superblock or free map */
LOCAL BOOL in_group_meta( TSFSBLKVOL *v, UD blk )
{
	INT	n = ag_of(v, blk);

	return (BOOL)( n >= 0 && blk < v->ag[n].first + ag_meta(&v->ag[n]) );
}

EXPORT ER ts_read_blk( ID vol, UD blk, void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er;

	if ( v == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( blk >= v->sb.total_blocks ) {
		return E_PAR;
	}
	/* A block of the volume's own bookkeeping may be newer in memory
	   than on the medium, and goes down first. Any other block is read
	   outside the volume's lock, so that readers do not wait on each
	   other; the journal and the cache keep their own. */
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = E_OK;
	if ( blk < 2 || blk < v->ag_first || in_group_meta(v, blk) ) {
		er = cache_flush(v);
	}
	tk_unl_mtx(v->mtxid);
	if ( er >= E_OK || v->rdonly ) {
		er = ( meta_read(vol, v->dd, blk, buf) >= E_OK ) ? E_OK : E_IO;
	}

	return er;
}

EXPORT ER ts_write_blk( ID vol, UD blk, CONST void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er;

	if ( v == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( blk >= v->sb.total_blocks ) {
		return E_PAR;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->rdonly ) {
		tk_unl_mtx(v->mtxid);
		return E_RONLY;
	}
	if ( v->cache_blk == blk ) {
		v->cache_blk = ~(UD)0;		/* the cached copy is stale now */
		v->cache_dirty = FALSE;
	}
	er = cache_flush(v);
	if ( er >= E_OK ) {
		if ( meta_write(vol, v->dd, blk, buf) < E_OK ) {
			note_error(v, blk, TSFSBLK_ERR_IO);
			er = E_IO;
		}
	}
	tk_unl_mtx(v->mtxid);

	return er;
}

/* Whether any block of [blk, blk + n) is the volume's own bookkeeping */
LOCAL BOOL run_has_meta( TSFSBLKVOL *v, UD blk, UD n )
{
	UD	i;

	if ( blk < 2 || blk < v->ag_first ) {
		return TRUE;
	}
	for ( i = 0; i < n; i++ ) {
		if ( in_group_meta(v, blk + i) ) return TRUE;
	}
	return FALSE;
}

EXPORT ER ts_read_blks( ID vol, UD blk, UD n, void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UD		i;
	ER		er;

	if ( v == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( n == 0 || blk >= v->sb.total_blocks || n > v->sb.total_blocks - blk ) {
		return E_PAR;
	}
	if ( n == 1 || run_has_meta(v, blk, n) ) {
		for ( i = 0, er = E_OK; i < n && er >= E_OK; i++ ) {
			er = ts_read_blk(vol, blk + i, (UB *)buf + i * TSFSBLK_BLOCK_SIZE);
		}
		return er;
	}
	/* the cache holds only what the medium has, so the medium is read
	   whole; a committed copy the journal has not put in place yet is
	   newer, and is taken over what was read */
	if ( blks_read(v->dd, blk, n, buf) < E_OK ) {
		return E_IO;
	}
	for ( i = 0; i < n; i++ ) {
		(void)knl_jrnl_peek(vol, blk + i, (UB *)buf + i * TSFSBLK_BLOCK_SIZE);
	}
	return E_OK;
}

EXPORT ER ts_write_blks( ID vol, UD blk, UD n, CONST void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UD		i;
	ER		er;

	if ( v == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( n == 0 || blk >= v->sb.total_blocks || n > v->sb.total_blocks - blk ) {
		return E_PAR;
	}
	if ( n == 1 || run_has_meta(v, blk, n) || v->cut ) {
		/* block by block: a pretended cut counts its writes one at a time */
		for ( i = 0, er = E_OK; i < n && er >= E_OK; i++ ) {
			er = ts_write_blk(vol, blk + i, (CONST UB *)buf + i * TSFSBLK_BLOCK_SIZE);
		}
		return er;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->rdonly ) {
		tk_unl_mtx(v->mtxid);
		return E_RONLY;
	}
	if ( v->cache_blk >= blk && v->cache_blk < blk + n ) {
		v->cache_blk = ~(UD)0;
		v->cache_dirty = FALSE;
	}
	er = cache_flush(v);
	if ( er >= E_OK ) {
		for ( i = 0; i < n; i++ ) {
			knl_jrnl_forget_blk(vol, blk + i);
		}
		v->writes += n;
		er = ( blks_write(v->dd, blk, n, buf) >= E_OK ) ? E_OK : E_IO;
		/* copies in the cache follow; the run itself is not kept there */
		for ( i = 0; i < n; i++ ) {
			bc_write(v, blk + i, (CONST UB *)buf + i * TSFSBLK_BLOCK_SIZE,
				 (BOOL)( er >= E_OK ), FALSE);
		}
		if ( er < E_OK ) note_error(v, blk, TSFSBLK_ERR_IO);
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

EXPORT ER ts_sync_blk( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er;

	if ( v == NULL ) {
		return E_ID;
	}
	(void)ts_jrnl_checkpoint(vol);		/* committed blocks in place */
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = cache_flush(v);
	if ( er >= E_OK ) {
		(void)dev_flush(v->dd);
	}
	tk_unl_mtx(v->mtxid);

	return er;
}

EXPORT ER ts_flush_blk( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er;

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = dev_flush(v->dd);
	if ( er == E_NOSPT ) {
		if ( ( v->sb.state & TSFSBLK_ST_NOFLUSH ) == 0 ) {
			v->sb.state |= TSFSBLK_ST_NOFLUSH;
			v->sb_dirty = TRUE;
		}
		er = E_OK;			/* nothing more can be done */
	}
	tk_unl_mtx(v->mtxid);

	return er;
}

/* ---------------------------------------------------------------- for the journal */

EXPORT ER knl_tsfsblk_raw_read( ID vol, UD blk, void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL || buf == NULL || blk >= v->sb.total_blocks ) {
		return E_PAR;
	}
	return ( blk_read(v->dd, blk, buf) >= E_OK ) ? E_OK : E_IO;
}

EXPORT ER knl_tsfsblk_raw_write( ID vol, UD blk, CONST void *buf )
{
	TSFSBLKVOL	*v = vol_of(vol);

	if ( v == NULL || buf == NULL || blk >= v->sb.total_blocks ) {
		return E_PAR;
	}
	if ( v->rdonly ) {
		return E_RONLY;
	}
	return ( cached_write(vol, v->dd, blk, buf, FALSE) >= E_OK ) ? E_OK : E_IO;
}

EXPORT ER ts_cut_blk( ID vol, UD after, UD *p_writes )
{
	TSFSBLKVOL	*v = bc_vol(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->bc_mtx, TMO_FEVR);
	if ( p_writes != NULL ) {
		*p_writes = v->writes;
	}
	v->writes = 0;
	v->cut = (BOOL)( after > 0 );
	v->cut_after = after;
	tk_unl_mtx(v->bc_mtx);
	return E_OK;
}

/* ---------------------------------------------------------------- the patrol */

LOCAL void scrub_bad( T_TSFSSCRUB *pk, UD blk )
{
	pk->bad++;
	if ( pk->first_bad == 0 ) {
		pk->first_bad = blk;
	}
}

EXPORT ER ts_scrub_one_blk( ID vol, UD blk, CONST char *magic, T_TSFSSCRUB *pk )
{
	TSFSBLKVOL	*v = vol_of(vol);
	UB		*buf;
	ER		er;

	if ( v == NULL || pk == NULL || magic == NULL ) {
		return E_ID;
	}
	if ( blk >= v->sb.total_blocks ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	pk->checked++;
	er = blk_read(v->dd, blk, buf);		/* the medium, past every cache */
	if ( er < E_OK ) {
		ts_blk_error(vol, blk, TSFSBLK_ERR_IO);
		scrub_bad(pk, blk);
	} else if ( ts_blk_check(vol, buf, magic, blk) < E_OK ) {
		scrub_bad(pk, blk);		/* the check has stopped the volume */
	}
	Kfree(buf);
	return E_OK;
}

/*
 * The block layer's own blocks. The journal is put in place and every
 * map and head written first, so that what is on the medium is what the
 * volume holds; the caller keeps writers away meanwhile.
 */
EXPORT ER ts_scrub_blk( ID vol, T_TSFSSCRUB *pk )
{
	TSFSBLKVOL	*v = vol_of(vol);
	T_TSFSBLK_SB	s;
	UB		*buf, *good;
	UD		n, k, blk;
	INT		c;
	ER		er;

	if ( v == NULL || pk == NULL ) {
		return E_ID;
	}
	er = ts_sync_blk(vol);
	if ( er < E_OK && !v->rdonly ) {
		return er;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	good = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL || good == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		if ( good != NULL ) Kfree(good);
		return E_NOMEM;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);

	/* the two superblocks: one gone bad is written again from memory */
	for ( c = 0; c < 2; c++ ) {
		pk->checked++;
		if ( blk_read(v->dd, (UD)c, buf) >= E_OK && sb_unpack(buf, &s) >= E_OK ) {
			continue;
		}
		scrub_bad(pk, (UD)c);
		if ( !v->rdonly ) {
			/* each write goes to the copy the last did not touch */
			if ( sb_write(v) >= E_OK && ( v->sb.generation & 1 ) != (UD)c ) {
				(void)sb_write(v);
			}
			pk->repaired++;
		}
	}
	sb_pack(&v->sb, good);

	for ( n = 0; n < v->sb.ag_count; n++ ) {
		TSFSAG	*a = &v->ag[n];

		/* the spare superblock */
		if ( a->spare ) {
			blk = a->first + 1;
			pk->checked++;
			if ( blk_read(v->dd, blk, buf) < E_OK || sb_unpack(buf, &s) < E_OK ) {
				scrub_bad(pk, blk);
				if ( !v->rdonly && blk_write(v->dd, blk, good) >= E_OK ) {
					pk->repaired++;
				}
			}
		}
		/* the head, against the one in memory */
		pk->checked++;
		if ( blk_read(v->dd, a->first, buf) < E_OK
		  || !hd_good(&v->sb.vol_uuid, buf, TSFSBLK_MAGIC_AGH, a->first)
		  || rd64(buf + AG_NO) != n || rd64(buf + AG_FIRST) != a->first ) {
			scrub_bad(pk, a->first);
			if ( !v->rdonly ) {
				ag_pack(v, n, buf);
				if ( meta_write(vol, v->dd, a->first, buf) >= E_OK ) {
					pk->repaired++;
				}
			}
		}
		/* the maps, against the checksums the head keeps */
		for ( k = 0; k < a->map_blocks; k++ ) {
			blk = a->map_first + k;
			pk->checked++;
			if ( blk_read(v->dd, blk, buf) < E_OK
			  || ts_crc32c(buf, TSFSBLK_BLOCK_SIZE) != a->crc[k] ) {
				scrub_bad(pk, blk);
				note_error(v, blk, TSFSBLK_ERR_CSUM);
			}
		}
	}
	if ( v->sb_dirty && !v->rdonly ) {
		(void)cache_flush(v);
	}
	tk_unl_mtx(v->mtxid);
	Kfree(buf);
	Kfree(good);
	return E_OK;
}

EXPORT ER ts_cache_blk( ID vol, T_TSFSBLK_CACHE *pk_cache )
{
	TSFSBLKVOL	*v = bc_vol(vol);

	if ( v == NULL || pk_cache == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(v->bc_mtx, TMO_FEVR);
	pk_cache->blocks = v->bc_n;
	pk_cache->max = BC_MAX;
	pk_cache->hits = v->bc_hits;
	pk_cache->misses = v->bc_misses;
	{
		DEVIO	*d = io_of(v->dd);

		pk_cache->reads = ( d != NULL ) ? d->rd : 0;
		pk_cache->writes = ( d != NULL ) ? d->wr : 0;
		pk_cache->flushes = ( d != NULL ) ? d->fl : 0;
	}
	tk_unl_mtx(v->bc_mtx);
	return E_OK;
}

EXPORT ER knl_tsfsblk_raw_flush( ID vol )
{
	TSFSBLKVOL	*v = vol_of(vol);
	ER		er;

	if ( v == NULL ) {
		return E_PAR;
	}
	er = dev_flush(v->dd);
	return ( er == E_NOSPT ) ? E_OK : er;
}
