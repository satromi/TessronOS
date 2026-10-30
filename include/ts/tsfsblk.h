/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsblk.h
 *	TSFS native volume, block layer (design 11.6)
 *
 *	The superblock (two copies written in turn), the journal area, the
 *	allocation groups with their free maps, the common head every
 *	management block carries, and allocation. The object index, the
 *	objects and the journal itself sit on top of this.
 *
 *	Everything on the medium is little endian. A block is 4096 bytes,
 *	eight sectors of the device.
 */

#ifndef __TS_TSFSBLK_H__
#define __TS_TSFSBLK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

#define TSFSBLK_MAGIC		"TFSVOL02"	/* eight bytes, no NUL */
#define TSFSBLK_VERSION		4
#define TSFSBLK_BLOCK_SIZE	4096
#define TSFSBLK_SECT_PER_BLK	(TSFSBLK_BLOCK_SIZE / 512)
#define TSFSBLK_LABEL_MAX	64
#define TSFSBLK_MAX_VOL		4096	/* native volumes mounted at once (CNF_MAX_VOL); an entry is made when it is first wanted */
#define TSFSBLK_MIN_BLOCKS	1024	/* the smallest volume that is made */

/* The common head of a management block (design 11.6.4) */
#define TSFSBLK_HDR_SIZE		64
#define TSFSBLK_MAGIC_AGH	"TFGH"		/* allocation group head */
#define TSFSBLK_MAGIC_NODE	"TFBT"		/* node of a B+tree */
#define TSFSBLK_MAGIC_OBJ	"TFOB"		/* object block */
#define TSFSBLK_MAGIC_TBL	"TFTB"		/* a table that ran out of its block */

/* What went wrong, as the superblock records it */
#define TSFSBLK_ERR_CSUM		1	/* a block did not pass its check */
#define TSFSBLK_ERR_IO		2	/* the device failed a write */
#define TSFSBLK_ERR_TREE		3	/* a tree is not in order */

/* State bits of the superblock */
#define TSFSBLK_ST_CLEAN		0x0001	/* taken down properly */
#define TSFSBLK_ST_ERROR		0x0002	/* something was found wrong: read only until checked */
#define TSFSBLK_ST_NOFLUSH	0x0004	/* the device cannot be told to empty its cache */

/*
 * Features. A volume that has been grown (ts_grow_vol_blk) keeps the
 * journal and the size of a group it was made with, so its groups are
 * not the ones layout() would choose for its size now: GROWN says so to
 * a check. GROWING is set while a grow is being written; a kernel that
 * does not know it does not open such a volume, and one that does
 * finishes the grow when it opens it (grow_to is where it is going).
 */
#define TSFSBLK_COMPAT_GROWN		0x0001ULL
#define TSFSBLK_INCOMPAT_GROWING	0x0001ULL

/*
 * XT2: a record's runs in a tree of two levels, an index of leaves
 * (design 7.3), on a volume that has one record of more runs than one
 * leaf holds. Set the first time one is written and kept; a kernel that
 * does not know it (R1.110 and before) does not open the volume.
 */
#define TSFSBLK_INCOMPAT_XT2		0x0002ULL

/* The volume marked as having trees of two levels, in the running transaction */
IMPORT ER ts_blk_xt2( ID vol );

/*
 * Superblock, as it is held in memory. Two copies sit in blocks 0 and 1;
 * each write goes to the one the last write did not touch, with the
 * generation one higher, and a volume is opened from the valid copy of
 * the higher generation.
 */
typedef struct {
	UB	magic[8];
	UW	version;
	UW	block_size;
	UD	generation;
	TS_UUID	vol_uuid;
	UB	label[TSFSBLK_LABEL_MAX];
	UD	total_blocks;
	UD	free_blocks;		/* the sum over the groups */
	UD	ag_blocks;		/* blocks of one allocation group */
	UD	ag_count;
	UD	journal_start;
	UD	journal_blocks;
	UD	journal_seq;		/* the next transaction number */
	UD	objtbl_start;		/* object index: root, 0 while there is none */
	UD	objtbl_blocks;		/* and the nodes it takes */
	UD	gclist_start;		/* garbage list */
	UW	gclist_blocks;
	UD	orphan_start;		/* orphans */
	UW	orphan_blocks;
	TS_UUID	root_uuid;		/* the object a volume opens with */
	TS_UUID	sysbox_uuid;		/* the system box */
	TS_UUID	domain_uuid;		/* the protection domain */
	TS_UUID	xref_uuid;		/* the object that keeps what other volumes link to here */
	UD	compat, ro_compat, incompat;
	UW	state;			/* TSFSBLK_ST_* */
	UW	err_kind;		/* TSFSBLK_ERR_* of the last error */
	UD	err_first, err_last;	/* when (TS_TIME), 0 if never */
	UD	err_blk;		/* where the last one was */
	UD	mount_count;
	UD	mount_time, fsck_time, mkfs_time;
	UD	grow_to;		/* blocks a grow cut short is going to, 0 when none */
} T_TSFSBLK_SB;

/*
 * Make a volume on a block device. Everything on it is lost.
 */
IMPORT ER ts_format_blk( CONST char *devnm, CONST char *label );

/*
 * Volume: opened by device name. A volume that has an error recorded
 * opens read only; so does one with a read-only feature this code does
 * not know. One with an unknown incompatible feature does not open.
 */
IMPORT ID ts_opn_vol_blk( CONST char *devnm );
IMPORT ER ts_cls_vol_blk( ID vol );
IMPORT ER ts_ref_vol_blk( ID vol, T_TSFSBLK_SB *pk_sb );

/*
 * The volume opened on a device, by the device's name: E_NOEXS when no
 * volume is open on it.
 */
IMPORT ID ts_vol_blk_of( CONST char *devnm );

/*
 * The volume made larger, to `blocks` (0: as many as its device holds
 * now), while it is open and in use. The journal and the size of a group
 * stay as they are: the last group is made whole and groups are added
 * after it. Transactions wait meanwhile. The superblock is written first
 * with GROWING and last with the new size, so that a cut in the power
 * between leaves a volume the next opening finishes growing. E_LIMIT when
 * the blocks the last group's map grows into are in use, E_PAR when the
 * device is not larger than the volume. The size before and after in
 * blocks go to p_from and p_to.
 */
IMPORT ER ts_grow_vol_blk( ID vol, UD blocks, UD *p_from, UD *p_to );

/*
 * The object the volume opens with, its top (T_TSFSBLK_SB.root_uuid),
 * written into the superblock; NULL takes it away. E_RONLY on a volume
 * that does not write.
 */
IMPORT ER ts_set_top_blk( ID vol, CONST TS_UUID *uuid );

/*
 * The object that keeps the links other volumes make to objects of this
 * one (T_TSFSBLK_SB.xref_uuid, design 8.3), written into the superblock;
 * NULL takes it away. A volume written before it was known has none.
 */
IMPORT ER ts_set_xref_blk( ID vol, CONST TS_UUID *uuid );

/*
 * Entries of the volume table made so far, how many are volumes open
 * now, and the blocks the caches of every volume hold together.
 */
IMPORT INT knl_tsfsblk_slots( INT *p_used, UD *p_cached );

/*
 * A run of up to `want` blocks. Fewer come back when the volume is
 * broken up, so the caller has to look at *p_count.
 */
IMPORT ER ts_alloc_ext( ID vol, UD want, UD *p_start, UD *p_count );
IMPORT ER ts_free_ext( ID vol, UD start, UD count );

/*
 * Up to `want` blocks starting exactly at `blk`, to make a run longer in
 * place: E_NOMEM when that block is taken or not a data block.
 */
IMPORT ER ts_alloc_ext_at( ID vol, UD blk, UD want, UD *p_count );

IMPORT ER ts_alloc_blk( ID vol, UD *p_blk );
IMPORT ER ts_free_blk( ID vol, UD blk );
IMPORT ER ts_read_blk( ID vol, UD blk, void *buf );
IMPORT ER ts_write_blk( ID vol, UD blk, CONST void *buf );

/*
 * `n` blocks in a row, in one request to the device: the bytes of an
 * object's data. `buf` is kernel memory the device can reach. A run that
 * takes in any of the volume's own bookkeeping goes block by block.
 */
IMPORT ER ts_read_blks( ID vol, UD blk, UD n, void *buf );
IMPORT UD ts_run_blocks( ID vol );	/* blocks in one request of the volume's device (TDN_IOMAX), 0: not said */
IMPORT ER ts_write_blks( ID vol, UD blk, UD n, CONST void *buf );
IMPORT ER ts_sync_blk( ID vol );

/* The device's cache emptied onto the medium (TDN_FLUSH) */
IMPORT ER ts_flush_blk( ID vol );

/*
 * The common head of a management block: its kind, its own block
 * number, the volume, whose it is, and the transaction that wrote it,
 * with a checksum over the whole block. Sealing fills the head and the
 * checksum; checking answers E_OBJ unless all of them agree, and records
 * the error on the volume, which goes read only.
 */
IMPORT ER ts_blk_seal( ID vol, UB *buf, CONST char *magic, UD blk, UD owner );
IMPORT ER ts_blk_check( ID vol, CONST UB *buf, CONST char *magic, UD blk );

/* An error found above this layer, recorded the same way */
IMPORT void ts_blk_error( ID vol, UD blk, UW kind );

/* Where the object index has its root (0 while the volume is empty) */
IMPORT ER ts_get_root_blk( ID vol, UD *p_root, UD *p_nodes );
IMPORT ER ts_set_root_blk( ID vol, UD root, UD nodes );

/*
 * Where the garbage list has its root. It is a second B+tree, holding the
 * UUID of every object whose reference count has fallen to zero, and the
 * superblock carries its root for the same reason it carries the index's:
 * so that opening a volume does not walk a tree.
 */
IMPORT ER ts_get_gc_root_blk( ID vol, UD *p_root, UD *p_nodes );
IMPORT ER ts_set_gc_root_blk( ID vol, UD root, UD nodes );

/*
 * Where the orphan tree has its root: a third B+tree, of the objects
 * deleted while open and of those whose link table has to be made again
 * (design 11.7, 11.14.5).
 */
IMPORT ER ts_get_orphan_root_blk( ID vol, UD *p_root, UD *p_nodes );

/*
 * The block cache of a volume (design 11.10): what has been read or
 * written of the volume's blocks, most recently used kept. Writes go to
 * the device at once as well, so the cache never holds anything the
 * medium does not.
 */
typedef struct {
	UD	blocks;			/* held now */
	UD	max;			/* the most it holds */
	UD	hits;
	UD	misses;
	UD	reads;			/* requests to the device: reads */
	UD	writes;			/* writes */
	UD	flushes;		/* its cache emptied */
} T_TSFSBLK_CACHE;

IMPORT ER ts_cache_blk( ID vol, T_TSFSBLK_CACHE *pk_cache );

/*
 * The patrol (design 11.14): what the medium holds is read past every
 * cache and checked. ts_scrub_blk does the block layer's own blocks --
 * both superblocks and the spares, the group heads and the free maps --
 * and puts right a superblock or a group head from the good copy the
 * volume holds in memory. ts_scrub_one_blk reads one management block
 * of the kind given and checks its common head; what fails stops the
 * volume writing, as any damage found does.
 */
typedef struct {
	UD	checked;		/* blocks read and checked */
	UD	bad;			/* found damaged */
	UD	repaired;		/* of those, written again from a good copy */
	UD	first_bad;		/* the first one found, 0 when none */
} T_TSFSSCRUB;

IMPORT ER ts_scrub_blk( ID vol, T_TSFSSCRUB *pk_scrub );
IMPORT ER ts_scrub_one_blk( ID vol, UD blk, CONST char *magic, T_TSFSSCRUB *pk_scrub );

/*
 * A cut in the power, pretended: every write to the device after the
 * next `after` is thrown away, flushes included, as if the power went
 * then. 0 puts it off. `p_writes` gives the writes the volume has made
 * since the last call, so that a test can first count what its work
 * writes and then cut it at each point in turn. Opening the volume
 * again starts afresh.
 */
IMPORT ER ts_cut_blk( ID vol, UD after, UD *p_writes );
IMPORT ER ts_set_orphan_root_blk( ID vol, UD root, UD nodes );

/*
 * While this is on, the free maps, the group heads and the superblock
 * wait for the record that makes the open transaction real instead of
 * being written where they belong. ts_blk_jrnl_flush puts what waited
 * into the log; the journal calls both, so nothing else has to.
 */
IMPORT ER ts_blk_jrnl( ID vol, BOOL on );
IMPORT ER ts_blk_jrnl_flush( ID vol );
IMPORT ER ts_blk_jrnl_discard( ID vol );

/* Where the journal is, and the number the next transaction takes */
IMPORT ER ts_get_jrnl_blk( ID vol, UD *p_start, UD *p_blocks, UD *p_seq );
IMPORT ER ts_set_jrnl_blk( ID vol, UD start, UD blocks, UD seq );

/*
 * The device itself, past the volume's lock and cache: for the journal,
 * which reads and writes its own area and puts committed blocks in place
 * while holding a lock of its own that the volume's lock may be taken
 * outside of.
 */
IMPORT ER knl_tsfsblk_raw_read( ID vol, UD blk, void *buf );
IMPORT ER knl_tsfsblk_raw_write( ID vol, UD blk, CONST void *buf );
IMPORT ER knl_tsfsblk_raw_writes( ID vol, UD blk, UD n, CONST void *buf );

/*
 * The same, on the medium when it returns (TDN_FUAWRITE) when the device
 * can: *p_fua says whether it was; when not, the caller empties the
 * device's cache after.
 */
IMPORT ER knl_tsfsblk_raw_writes_fua( ID vol, UD blk, UD n, CONST void *buf, BOOL *p_fua );
IMPORT ER knl_tsfsblk_raw_flush( ID vol );

/* The log could not be written: the volume stops writing (called from inside the log's locks) */
IMPORT void knl_tsfsblk_stop( ID vol, UD blk );

/*
 * Every transaction numbered below `dseq` is in the log: the runs they
 * freed may be handed out again. Until then the allocator passes over
 * them (a run freed by a transaction not on the medium still belongs, on
 * the medium, to what used it).
 */
IMPORT void knl_tsfsblk_unbusy( ID vol, UD dseq );

/*
 * For the check of a whole volume (ts/tsfsck.h). ts_blk_good looks at a
 * common head as ts_blk_check does but records nothing. ts_blk_group
 * says where a group is and what it uses itself; ts_blk_map copies one
 * of its map blocks as the volume holds it now (E_OBJ when the block
 * does not match the checksum its head keeps).
 */
typedef struct {
	UD	first;			/* the group's first block, its head */
	UD	len;			/* its blocks */
	UD	meta;			/* the blocks at its front it uses itself */
	UD	free;			/* free, as the head counts */
	UD	map_first;
	UW	map_blocks;
	BOOL	spare;			/* holds a copy of the superblock */
} T_TSFSAGREF;

IMPORT ER ts_blk_good( ID vol, CONST UB *buf, CONST char *magic, UD blk );
IMPORT ER ts_blk_group( ID vol, UD n, T_TSFSAGREF *pk );
IMPORT ER ts_blk_map( ID vol, UD n, UW k, UB *buf );

/* A map block put back whole (one made again from the blocks in use), its checksum and count with it */
IMPORT ER ts_blk_map_put( ID vol, UD n, UW k, CONST UB *buf );

/*
 * The block layer's own blocks looked at without writing: the copies of
 * the superblock that do not pass (the spares among them), the group
 * heads on the medium that are not the ones in memory, the maps that do
 * not match their heads' checksums, and the groups whose count of free
 * blocks is not their map's. ts_blk_mend writes what memory holds
 * correctly over the first two and counts the free blocks again from
 * the maps; a map gone bad stays as it is.
 */
typedef struct {
	UD	sb_bad;
	UD	sb_first;		/* where the first one is */
	UD	head_bad;
	UD	head_first;
	UD	map_bad;
	UD	map_first;
	UD	count_bad;
	UD	count_first;		/* the group's number */
	BOOL	error;			/* an error is recorded, or was found in this run */
	BOOL	rdonly;			/* the volume does not write */
} T_TSFSBLKINSP;

IMPORT ER ts_blk_inspect( ID vol, T_TSFSBLKINSP *pk );
IMPORT ER ts_blk_mend( ID vol, UD *p_mended );

/*
 * A volume an error stopped is let write while it is repaired (on) and
 * stopped again afterwards unless the error was cleared (off);
 * ts_blk_checked clears the error once a check finds the volume whole.
 */
IMPORT ER ts_blk_writable( ID vol, BOOL on );

/* The log of a volume opened with an error recorded put back, for a volume nothing else has open */
IMPORT ER ts_blk_replay( ID vol );
IMPORT ER ts_blk_checked( ID vol );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSBLK_H__ */
