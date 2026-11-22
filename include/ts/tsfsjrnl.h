/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsjrnl.h
 *	Write ahead log of a native volume (design 11.14.1)
 *
 *	Metadata blocks are written to the log before they are written where
 *	they belong, so that a volume whose power went out comes back either
 *	with the whole change or with none of it. Data blocks are not
 *	logged: they are written before the transaction is committed, which
 *	is what ordered mode means.
 *
 *	The journal area is a head block followed by a ring. A transaction
 *	goes into the ring as a descriptor naming where its blocks belong,
 *	the copies, a revoke block when it freed blocks the ring still holds
 *	copies of, and a commit block that checks all of them; on a version
 *	5 volume as many descriptors, each followed by its copies, and as
 *	many revoke blocks as the transaction needs, numbered in order. The
 *	commit block going down is the moment the transaction becomes real.
 *
 *	A committed transaction's blocks are kept in memory and put where
 *	they belong later, all together (a checkpoint): when the ring or the
 *	memory runs short, every few seconds, and when the volume is synced
 *	or closed. Until then a read of such a block is answered from memory.
 *	The head block says where in the ring the oldest transaction that
 *	has not been put in place starts; coming back after a cut in the
 *	power puts back everything from there that has a good commit block.
 *
 *	Callers that change the volume take a handle on the running
 *	transaction (ts_jrnl_begin) and let it go. Several may hold one at
 *	once; the last one to let go closes the transaction. ts_jrnl_close
 *	only closes it: its blocks are what every read sees from then on and
 *	it waits in a queue for the ring, with nothing written to the device.
 *	ts_jrnl_durable makes everything closed up to a number real: the
 *	first caller to come writes the whole queue into the ring at once,
 *	between two flushes of the device's cache, and those who come while
 *	it writes are made real by it (group commit). ts_jrnl_commit is the
 *	two together. A volume's lock is let go between the two, so that the
 *	device's time is not spent holding it (docs/tessronos 7.3).
 */

#ifndef __TS_TSFSJRNL_H__
#define __TS_TSFSJRNL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/tsfsblk.h>

#define TSFSJ_VERSION		2
#define TSFSJ_VERSION5		5	/* the head and the ring blocks of a version 5 volume */
#define TSFSJ_HDR_SIZE		32
#define TSFSJ_DESC_ENT		16	/* where a block goes (8), its CRC (4), spare (4) */

/*
 * Blocks one descriptor names. A transaction of a version 4 volume has
 * one descriptor, so this is all it may change; one of a version 5
 * volume has as many descriptors as it needs, and its budget is the
 * larger of this and a quarter of the ring (ts_jrnl_budget).
 */
#define TSFSJ_MAX_BLOCKS		( ( TSFSBLK_BLOCK_SIZE - TSFSJ_HDR_SIZE ) / TSFSJ_DESC_ENT )

/* Blocks one revoke block names (a version 4 transaction has one revoke block at most) */
#define TSFSJ_MAX_REVOKE		( ( TSFSBLK_BLOCK_SIZE - TSFSJ_HDR_SIZE ) / 8 )

/*
 * The smallest journal area: the head block and a ring with room for two
 * of the largest transactions (descriptor, copies, revoke, commit).
 */
#define TSFSJ_TXN_MAX_FOOT	( TSFSJ_MAX_BLOCKS + 3 )
#define TSFSJ_AREA_BLOCKS	( 1 + 2 * TSFSJ_TXN_MAX_FOOT )

/* What the log is doing, for a caller that wants to look */
typedef struct {
	UD	start;			/* first block of the journal area */
	UD	blocks;
	UD	seq;			/* the number the next transaction takes */
	UD	in_txn;			/* blocks put in the one being built */
	UINT	open;			/* handles on the running transaction */
	UD	head_seq;		/* the oldest transaction not put in place */
	UD	used;			/* ring blocks between head and tail */
	UD	cached;			/* committed blocks waiting for a checkpoint */
	BOOL	dead;			/* a pretended power cut: nothing more is written */
	UD	durable;		/* every transaction numbered below this is in the ring */
	UD	queued;			/* ring blocks the closed ones not yet in it will take */
	UD	closed;			/* transactions closed since the volume was opened */
	UD	batches;		/* times the queue was written into the ring */
	UINT	budget;			/* copies one transaction may hold (ts_jrnl_budget) */
	UINT	last_desc;		/* descriptor blocks of the last transaction closed */
	UINT	last_rvk;		/* revoke blocks of it */
	BOOL	v5;			/* the ring is of version 5 (TSFSJ_VERSION5) */
} T_TSFSJRNL;

IMPORT ER ts_jrnl_begin( ID vol );
IMPORT ER ts_jrnl_write( ID vol, UD blk, CONST void *buf );
IMPORT ER ts_jrnl_close( ID vol, UD *p_seq );
IMPORT ER ts_jrnl_durable( ID vol, UD upto );
IMPORT UD ts_jrnl_next( ID vol );
IMPORT ER ts_jrnl_commit( ID vol );
IMPORT ER ts_jrnl_abort( ID vol );
IMPORT ER ts_jrnl_ref( ID vol, T_TSFSJRNL *p_jrnl );

/*
 * Read a metadata block as the running transaction sees it: from the
 * transaction when it is in there, from what a committed one left in
 * memory, from its own block otherwise.
 */
IMPORT ER ts_jrnl_read( ID vol, UD blk, void *buf );

/* Put every committed block where it belongs, and free the ring */
IMPORT ER ts_jrnl_checkpoint( ID vol );

/*
 * Every committed block put where it belongs, then the whole ring written
 * over with zeroes, so that no copy of a block as it was before stays in
 * the journal (ob_wip_vol). Waits for the running transaction to end.
 */
IMPORT ER ts_jrnl_wipe( ID vol );

/*
 * Put back whatever a cut in the power left behind. Called when a volume
 * is opened, before anything reads its metadata.
 */
IMPORT ER ts_jrnl_replay( ID vol );

/*
 * Stop a transaction part way through on purpose, so that the coming
 * back can be tested. TSFSJ_FAULT_NONE puts it back to normal. After
 * TSFSJ_FAULT_AFTER_COMMIT the journal behaves as a machine whose power
 * went: it neither writes nor checkpoints until the volume is opened
 * again.
 */
#define TSFSJ_FAULT_NONE		0
#define TSFSJ_FAULT_BEFORE_COMMIT 1	/* the ring is written, the commit block is not */
#define TSFSJ_FAULT_AFTER_COMMIT	2	/* the commit block is written, nothing is put in place */
#define TSFSJ_FAULT_HOLD		3	/* nothing goes into the ring until the fault is taken off */
#define TSFSJ_FAULT_SKIP		4	/* as AFTER_COMMIT, with the second descriptor numbered 2
					   (a ring whose descriptors skip a number; version 5) */

IMPORT ER ts_jrnl_fault( ID vol, UINT where );

/*
 * TSFSJ_FAULT_BUDGET | n: the budget of a transaction made n copies
 * (n 0: the volume's own), until the volume is closed, so that work
 * split over several transactions can be tried on a small budget. It
 * does not change the fault set with the other values.
 */
#define TSFSJ_FAULT_BUDGET	0x40000000U
#define TSFSJ_FAULT_ARG		0x00FFFFFFU

/*
 * TSFSJ_FAULT_NOCKPT | 1: committed copies stay in the ring and in
 * memory until a transaction finds no room in the ring, or the volume is
 * synced or closed; the checkpointer leaves the volume alone, so that a
 * test knows what the ring holds. | 0 takes it off.
 */
#define TSFSJ_FAULT_NOCKPT	0x20000000U

/*
 * The budget: the copies one transaction may hold (a version 4 volume:
 * TSFSJ_MAX_BLOCKS; a version 5 volume: the larger of that and a quarter
 * of the ring, so long as two of the largest transactions fit in the
 * ring), and what is left of it in the running transaction
 * (the budget when none is running). A step of work split over
 * transactions looks at ts_jrnl_room before it goes on. E_ID for a
 * volume that is not.
 */
IMPORT INT ts_jrnl_budget( ID vol );
IMPORT INT ts_jrnl_room( ID vol );

/* ---------------------------------------------------------------- for the block layer */

/* A newer copy of a block than its own place has; TRUE if there is one */
IMPORT BOOL knl_jrnl_peek( ID vol, UD blk, void *buf );

/* A block written straight to its place: any copy waiting for it goes */
IMPORT void knl_jrnl_forget_blk( ID vol, UD blk );

/* Blocks freed by the running transaction: copies of them must not come back */
IMPORT void knl_jrnl_revoke( ID vol, UD start, UD count );

/* The number the running transaction will take when it is closed */
IMPORT UD knl_jrnl_running( ID vol );

/* A volume that is closing: what waits goes in place, and its entry goes */
IMPORT ER knl_jrnl_close( ID vol );

/* Whether the log was read at open (not for a volume opened with an error recorded) */
IMPORT BOOL knl_jrnl_ready( ID vol );

/*
 * Transactions held off while the volume is grown (on), and let begin
 * again (off). Freezing waits for the ones running and puts everything
 * committed in place; E_BUSY when one does not end.
 */
IMPORT ER knl_jrnl_freeze( ID vol, BOOL on );

/*
 * The last step of the conversion to version 5: the head block written
 * as version 5, the ring empty, the next number where it was; the
 * transactions from here on are laid out as version 5. The volume is
 * frozen (knl_jrnl_freeze) and its ring empty.
 */
IMPORT ER knl_jrnl_fmt5( ID vol );

/*
 * Whether opening found a version 5 volume whose head was still of
 * version 4 with an empty ring (a conversion cut short after its
 * superblock) and wrote the head as version 5.
 */
IMPORT BOOL knl_jrnl_conv_left( ID vol );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSJRNL_H__ */
