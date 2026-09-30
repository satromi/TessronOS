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
 *	copies of, and a commit block that checks all of them. The commit
 *	block going down is the moment the transaction becomes real.
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
 *	transaction (ts_jrnl_begin) and let it go (ts_jrnl_commit). Several
 *	may hold one at once; the transaction is committed when the last one
 *	lets go.
 */

#ifndef __TS_TSFSJRNL_H__
#define __TS_TSFSJRNL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/tsfsblk.h>

#define TSFSJ_VERSION		2
#define TSFSJ_HDR_SIZE		32
#define TSFSJ_DESC_ENT		16	/* where a block goes (8), its CRC (4), spare (4) */

/* Blocks one transaction may change: what one descriptor names */
#define TSFSJ_MAX_BLOCKS		( ( TSFSBLK_BLOCK_SIZE - TSFSJ_HDR_SIZE ) / TSFSJ_DESC_ENT )

/* Blocks one revoke block names */
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
} T_TSFSJRNL;

IMPORT ER ts_jrnl_begin( ID vol );
IMPORT ER ts_jrnl_write( ID vol, UD blk, CONST void *buf );
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

IMPORT ER ts_jrnl_fault( ID vol, UINT where );

/* ---------------------------------------------------------------- for the block layer */

/* A newer copy of a block than its own place has; TRUE if there is one */
IMPORT BOOL knl_jrnl_peek( ID vol, UD blk, void *buf );

/* A block written straight to its place: any copy waiting for it goes */
IMPORT void knl_jrnl_forget_blk( ID vol, UD blk );

/* Blocks freed by the running transaction: copies of them must not come back */
IMPORT void knl_jrnl_revoke( ID vol, UD start, UD count );

/* A volume that is closing: what waits goes in place, and its entry goes */
IMPORT ER knl_jrnl_close( ID vol );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSJRNL_H__ */
