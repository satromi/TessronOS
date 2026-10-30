/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpt.h
 *	Partition tables written: GPT partitions made and made larger
 *	(design 7.7), and TSFS volumes on them.
 *
 *	A disk is named as the device manager names it ("sda", "uda"), a
 *	partition as its subunit ("sda1": the second partition the table
 *	lists). Sizes are in 512 byte sectors unless said otherwise. A
 *	partition is placed on whole megabytes (2048 sectors).
 */

#ifndef __TS_GPT_H__
#define __TS_GPT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define GPT_ALIGN		2048		/* sectors: a megabyte */
#define GPT_NAME_MAX		36		/* UTF-16 units of a partition's name */

/* A stretch of a disk no partition takes */
typedef struct {
	UD	first, last;		/* sectors, both inside */
} T_GPTFREE;

/* A partition, as the table on the disk has it */
typedef struct {
	UB	disk[L_DEVNM + 1];	/* the whole disk's device */
	INT	entry;			/* its entry in the table */
	INT	unit;			/* its subunit: the n in "sdan" */
	UD	first, last;		/* sectors */
	UD	grow_last;		/* the last sector it can be made to reach */
	UD	nsect;			/* sectors of the whole disk */
	BOOL	tail;			/* no partition lies after it */
	BOOL	tsfs;			/* of the TSFS type */
	UB	type[16];		/* its type GUID, as the table has it */
} T_GPTPART;

/*
 * The free stretches of a disk, each on whole megabytes and at least
 * one megabyte, in order, up to `max`. The end of the disk counts as it
 * will be once the table is written: the backup table at the last
 * sectors, wherever it is now (a card written with an image smaller
 * than the card has it in the middle). A disk with no table at all,
 * nothing in its first sectors, is all free: a table is made when a
 * partition is (*p_fresh). Answers how many, E_NOSPT for a disk with
 * an MBR, E_OBJ for a GPT neither copy of which passes its CRCs.
 */
IMPORT INT knl_gpt_free( CONST char *disk, T_GPTFREE *buf, INT max, UD *p_nsect, BOOL *p_fresh );

/* Where a partition is, and how far it can grow */
IMPORT ER knl_gpt_part( CONST char *part, T_GPTPART *pk );

/*
 * A partition made to end at `last` (it only grows, into the free
 * stretch right after it), both copies of the table written: the
 * backup at the end of the disk first, then the entries and the header
 * at its start. The driver reads the table again (TDN_PARTSCAN), so the
 * partition's device is larger when this returns.
 */
IMPORT ER knl_gpt_grow( CONST char *part, UD last );

/*
 * A partition made in [first, last] with the type and the name given
 * (UTF-8), in the entry after the last one used, so that the partitions
 * there were keep their numbers. Its device's name goes to `p_part`.
 */
IMPORT ER knl_gpt_add( CONST char *disk, UD first, UD last, CONST UB *type, CONST char *name,
		       UB *p_part, INT max );

/*
 * A partition made another in its place: its entry given the type and
 * the name given (UTF-8) and a new unique GUID (to `p_uniq` unless
 * NULL), its attributes cleared; where it lies, its entry and its
 * number stay. What it holds is not touched. The driver reads the
 * table again.
 */
IMPORT ER knl_gpt_retype( CONST char *part, CONST UB *type, CONST char *name, UB *p_uniq );

/*
 * For a test: every write to a disk's table after the next `after` is
 * thrown away, as if the power went then; 0 puts it off. The writes
 * made since the last call go to p_writes.
 */
IMPORT void knl_gpt_cut( UW after, UW *p_writes );

/*
 * A TSFS partition and its volume made larger: the partition to `bytes`
 * (0: as far as the free stretch after it goes), then the volume to the
 * partition, open or not (ts_grow_vol_blk). A partition that is already
 * larger than its volume -- a grow the power cut between the two --
 * only has its volume grown. The volume's size before and after, in
 * bytes, go to p_from and p_to. E_LIMIT when there is nothing to grow
 * into.
 */
IMPORT ER knl_tsfs_part_grow( CONST char *part, UD bytes, UD *p_from, UD *p_to );

/*
 * A TSFS partition of `bytes` (0: the whole stretch) made in the free
 * stretch that starts at `first`, and a volume made on it with the
 * label given. Its device's name goes to `p_part`.
 */
IMPORT ER knl_tsfs_part_make( CONST char *disk, UD first, UD bytes, CONST char *label,
			      UB *p_part, INT max );

/*
 * At the start, before the system volume is attached: its partition
 * grown to CNF_SYSVOL_GROW_MB when it is the last of the disk and
 * smaller, and the volume with it; one line on the console says what
 * was done.
 */
IMPORT void knl_sysvol_grow( CONST char *part );

/* What knl_sysvol_grow did: the volume's size before and after in bytes (both 0 before it ran) */
IMPORT void knl_sysvol_grown( UD *p_from, UD *p_to );

#ifdef __cplusplus
}
#endif

#endif /* __TS_GPT_H__ */
