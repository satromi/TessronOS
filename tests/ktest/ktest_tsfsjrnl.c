/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfsjrnl.c
 *	The write ahead log of a native volume (design 11.14.1).
 *
 *	The point of the log is what happens when the power goes out part
 *	way through, so the driver can be told to stop at either side of the
 *	moment a transaction becomes real. The volume is then closed and
 *	opened again, which is what a machine does when it comes back.
 *	Committed transactions wait in the ring and in memory until a
 *	checkpoint puts them in place, so several of them can be there when
 *	the power goes, and a block freed and used again must not get an old
 *	copy back.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>

#define JDEV		kt_scratch()
#define PATTERN(n)	((UB)(0xA0 + (n)))

LOCAL ID	vol = 0;
LOCAL UD	blk_a = 0, blk_b = 0;

/* Fill a buffer with one byte repeated, and say whether it still is */
LOCAL void fill( UB *buf, UB v )
{
	INT	i;

	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) {
		buf[i] = v;
	}
}

LOCAL BOOL all_are( CONST UB *buf, UB v )
{
	INT	i;

	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) {
		if ( buf[i] != v ) {
			return FALSE;
		}
	}

	return TRUE;
}

/* the log is made on the first transaction and takes blocks from the volume */
LOCAL void test_open( void )
{
	T_TSFSJRNL	j;
	T_TSFSBLK_SB	sb;
	UD		count = 0;

	if ( ts_format_blk(JDEV, "JOURNAL") < E_OK ) {
		KT_SKIP("no block device to make a volume on");
	}
	vol = ts_opn_vol_blk(JDEV);
	if ( vol <= 0 ) {
		KT_SKIP("the volume would not open");
	}

	/* two blocks of our own to change */
	KT_ASSERT_ER(ts_alloc_ext(vol, 1, &blk_a, &count), E_OK);
	KT_ASSERT_ER(ts_alloc_ext(vol, 1, &blk_b, &count), E_OK);

	/* the log's place is fixed when the volume is made */
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT_EQ(j.start, sb.journal_start);

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);

	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT(j.start > 0);
	KT_ASSERT(j.blocks >= 2);
	KT_ASSERT_EQ(j.open, 1);

	/* a second handle joins the same transaction */
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT_EQ(j.open, 2);
	KT_ASSERT_ER(ts_jrnl_abort(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_abort(vol), E_OK);

	/* and nothing may be logged outside one */
	{
		UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);

		KT_ASSERT(buf != NULL);
		fill(buf, 0);
		KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OBJ);
		Kfree(buf);
	}
}

/* a transaction that runs to the end leaves the blocks changed */
LOCAL void test_commit( void )
{
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(1));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	fill(buf, PATTERN(2));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_b, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);

	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(1)));
	KT_ASSERT_ER(ts_read_blk(vol, blk_b, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(2)));

	/* the copies wait in memory and in the ring until a checkpoint */
	{
		T_TSFSJRNL	j;

		KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
		KT_ASSERT_EQ(j.open, 0);
		KT_ASSERT(j.seq > 1);
		KT_ASSERT(j.cached >= 2);
		KT_ASSERT(j.used > 0);

		KT_ASSERT_ER(ts_jrnl_checkpoint(vol), E_OK);
		KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
		KT_ASSERT_EQ(j.cached, 0);
		KT_ASSERT_EQ(j.used, 0);
		KT_ASSERT_EQ(j.head_seq, j.seq);
	}
	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(1)));	/* and in place now */
	Kfree(buf);
}

/* one that is given up on changes nothing */
LOCAL void test_abort( void )
{
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(9));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_abort(vol), E_OK);

	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(1)));	/* still what the commit left */

	Kfree(buf);
}

/*
 * The power goes before the record is written. Nothing says the
 * transaction happened, so coming back leaves the blocks alone.
 */
LOCAL void test_crash_before( void )
{
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_BEFORE_COMMIT), E_OK);
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(3));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	fill(buf, PATTERN(4));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_b, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);

	/* the machine comes back */
	KT_ASSERT_ER(ts_sync_blk(vol), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);

	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(1)));	/* the old contents */
	KT_ASSERT_ER(ts_read_blk(vol, blk_b, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(2)));

	Kfree(buf);
}

/*
 * The power goes after the record is written but before the blocks are
 * put where they belong. Coming back finds the record and writes them.
 */
LOCAL void test_crash_after( void )
{
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(5));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	fill(buf, PATTERN(6));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_b, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);

	/* not there yet: the blocks still hold what they did */
	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(1)));

	/* the machine comes back and the log is put back */
	KT_ASSERT_ER(ts_sync_blk(vol), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);

	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(5)));
	KT_ASSERT_ER(ts_read_blk(vol, blk_b, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(6)));

	Kfree(buf);
}

/* coming back twice puts it back once: the log is cleared as it goes */
LOCAL void test_replay_once( void )
{
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	/* write something over what the replay put there */
	fill(buf, PATTERN(7));
	KT_ASSERT_ER(ts_write_blk(vol, blk_a, buf), E_OK);

	KT_ASSERT_ER(ts_sync_blk(vol), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);

	/* the old transaction is not applied a second time */
	KT_ASSERT_ER(ts_read_blk(vol, blk_a, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(7)));

	Kfree(buf);
}

/*
 * The free map is part of a transaction too. Blocks taken inside one
 * stay taken when it becomes real, and come back when it does not: the
 * map waits for the record like everything else, so a machine whose
 * power goes first does not come back with blocks marked used by a
 * change that never happened.
 */
LOCAL void test_map( void )
{
	T_TSFSBLK_SB	sb;
	UD		free_before = 0, free_after = 0;
	UD		got = 0, count = 0;
	UB		*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	fill(buf, PATTERN(8));

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	free_before = sb.free_blocks;

	/* one that runs to the end: the blocks stay taken */
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	KT_ASSERT_ER(ts_alloc_ext(vol, 4, &got, &count), E_OK);
	KT_ASSERT_EQ(count, 4);
	KT_ASSERT_ER(ts_jrnl_write(vol, got, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_before - 4);

	/* and what it wrote is there */
	KT_ASSERT_ER(ts_read_blk(vol, got, buf), E_OK);
	KT_ASSERT(all_are(buf, PATTERN(8)));

	KT_ASSERT_ER(ts_free_ext(vol, got, 4), E_OK);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_before);

	/* one whose power goes before the record: they come back */
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_BEFORE_COMMIT), E_OK);
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	KT_ASSERT_ER(ts_alloc_ext(vol, 4, &got, &count), E_OK);
	KT_ASSERT_EQ(count, 4);
	fill(buf, PATTERN(9));
	KT_ASSERT_ER(ts_jrnl_write(vol, got, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	free_after = sb.free_blocks;
	if ( free_after != free_before ) {
		tm_printf((UB*)"  free blocks: %d before, %d after\n",
			  (INT)free_before, (INT)free_after);
	}
	KT_ASSERT_EQ(free_after, free_before);

	/* and the same holds when it is opened again */
	KT_ASSERT_ER(ts_sync_blk(vol), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_before);

	Kfree(buf);
}

/*
 * A transaction names only as many blocks as its descriptor has room
 * for; the same block put in again is kept once.
 */
LOCAL void test_limit( void )
{
	UB	*buf;
	INT	i;
	ER	er = E_OK;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	fill(buf, 0x5A);

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	for ( i = 0; i < (INT)TSFSJ_MAX_BLOCKS + 10; i++ ) {
		KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);	/* once, however often */
	}
	for ( i = 1; i < (INT)TSFSJ_MAX_BLOCKS; i++ ) {
		er = ts_jrnl_write(vol, blk_a + (UD)i, buf);
		if ( er < E_OK ) {
			break;
		}
	}
	KT_ASSERT_EQ(i, (INT)TSFSJ_MAX_BLOCKS);
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a + TSFSJ_MAX_BLOCKS, buf), E_LIMIT);
	KT_ASSERT_ER(ts_jrnl_abort(vol), E_OK);

	Kfree(buf);

	ts_free_ext(vol, blk_a, 1);
	ts_free_ext(vol, blk_b, 1);
	ts_cls_vol_blk(vol);
	vol = 0;
}

/* Commit one transaction that puts one pattern in one block */
LOCAL ER put( UD blk, UB pat )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	ER	er;

	if ( buf == NULL ) return E_NOMEM;
	fill(buf, pat);
	er = ts_jrnl_begin(vol);
	if ( er >= E_OK ) er = ts_jrnl_write(vol, blk, buf);
	if ( er >= E_OK ) er = ts_jrnl_commit(vol);
	Kfree(buf);
	return er;
}

LOCAL BOOL holds( UD blk, UB pat )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	BOOL	ok;

	if ( buf == NULL ) return FALSE;
	ok = (BOOL)( ts_read_blk(vol, blk, buf) >= E_OK && all_are(buf, pat) );
	Kfree(buf);
	return ok;
}

/*
 * Several committed transactions wait in the ring when the power goes;
 * coming back puts all of them back, in order.
 */
LOCAL void test_several( void )
{
	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_jrnl_checkpoint(vol), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(10)), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(11)), E_OK);
	KT_ASSERT_ER(put(blk_b, PATTERN(12)), E_OK);
	KT_ASSERT(holds(blk_a, PATTERN(11)));		/* read from memory */

	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(put(blk_b, PATTERN(13)), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);

	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT(holds(blk_a, PATTERN(11)));
	KT_ASSERT(holds(blk_b, PATTERN(13)));
}

/*
 * A block the ring holds a copy of is freed and then used for data.
 * The transaction that freed it revokes the copy, so coming back after
 * a cut in the power leaves the data alone.
 */
LOCAL void test_revoke( void )
{
	UD	blk_c = 0, count = 0;
	UB	*buf;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_alloc_ext(vol, 1, &blk_c, &count), E_OK);
	KT_ASSERT_ER(ts_jrnl_checkpoint(vol), E_OK);

	KT_ASSERT_ER(put(blk_c, PATTERN(14)), E_OK);		/* a copy in the ring */

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);			/* freed by a transaction */
	KT_ASSERT_ER(ts_free_ext(vol, blk_c, 1), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);

	fill(buf, PATTERN(15));					/* and used for data */
	KT_ASSERT_ER(ts_write_blk(vol, blk_c, buf), E_OK);

	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(16)), E_OK);		/* the power goes */
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) { Kfree(buf); return; }

	KT_ASSERT(holds(blk_c, PATTERN(15)));			/* not the old copy */
	KT_ASSERT(holds(blk_a, PATTERN(16)));
	Kfree(buf);
}

/* several handles on one transaction: the last one to let go commits it */
LOCAL void test_handles( void )
{
	T_TSFSJRNL	j;
	UB		*buf;
	UD		seq;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	seq = j.seq;

	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(17));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT_EQ(j.open, 1);				/* one still holds it */
	KT_ASSERT_EQ(j.seq, seq);
	KT_ASSERT(!holds(blk_a, PATTERN(17)));
	fill(buf, PATTERN(18));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_b, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT_EQ(j.open, 0);
	KT_ASSERT_EQ(j.seq, seq + 1);				/* one transaction */
	KT_ASSERT(holds(blk_a, PATTERN(17)));
	KT_ASSERT(holds(blk_b, PATTERN(18)));

	/* one giving up takes the whole of it */
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
	fill(buf, PATTERN(19));
	KT_ASSERT_ER(ts_jrnl_write(vol, blk_a, buf), E_OK);
	KT_ASSERT_ER(ts_jrnl_abort(vol), E_OK);
	KT_ASSERT_ER(ts_jrnl_commit(vol), E_IO);
	KT_ASSERT(holds(blk_a, PATTERN(17)));
	Kfree(buf);
}

/* the ring goes round many times; checkpoints keep up with it */
LOCAL void test_wrap( void )
{
	T_TSFSJRNL	j;
	UB		*buf;
	INT		t, i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	for ( t = 0; t < 40; t++ ) {
		KT_ASSERT_ER(ts_jrnl_begin(vol), E_OK);
		for ( i = 0; i < 20; i++ ) {
			fill(buf, (UB)( t + i ));
			KT_ASSERT_ER(ts_jrnl_write(vol, blk_a + 1 + (UD)i, buf), E_OK);
		}
		KT_ASSERT_ER(ts_jrnl_commit(vol), E_OK);
	}
	for ( i = 0; i < 20; i++ ) {
		KT_ASSERT(holds(blk_a + 1 + (UD)i, (UB)( 39 + i )));
	}
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j), E_OK);
	KT_ASSERT(j.used < j.blocks);

	/* and a volume closed in the middle comes back the same */
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	if ( vol > 0 ) {
		for ( i = 0; i < 20; i++ ) {
			KT_ASSERT(holds(blk_a + 1 + (UD)i, (UB)( 39 + i )));
		}
	}
	Kfree(buf);
}

/*
 * A volume made again on the same blocks. The ring still holds the old
 * volume's committed transactions, numbered as the new one's could be;
 * opening the new volume must not put them back.
 */
LOCAL void test_stale_ring( void )
{
	T_TSFSBLK_SB	sb;
	UD		fresh, count = 0;

	if ( ts_format_blk(JDEV, "OLD") < E_OK ) KT_SKIP("no block device");
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_alloc_ext(vol, 1, &blk_a, &count), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(20)), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(21)), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(put(blk_a, PATTERN(22)), E_OK);	/* the power goes with all three in the ring */
	KT_ASSERT_ER(ts_jrnl_fault(vol, TSFSJ_FAULT_NONE), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	KT_ASSERT(ts_format_blk(JDEV, "NEW") >= E_OK);
	vol = ts_opn_vol_blk(JDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	fresh = sb.free_blocks;
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(JDEV);			/* opened again: the ring is looked at */
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.state & TSFSBLK_ST_ERROR, 0);
	KT_ASSERT_EQ(sb.free_blocks, fresh);
	KT_ASSERT_EQ(sb.label[0], (UB)'N');
	ts_cls_vol_blk(vol);
	vol = 0;
}

EXPORT void ktest_tsfsjrnl( void )
{
	KT_RUN(test_open);
	KT_RUN(test_commit);
	KT_RUN(test_abort);
	KT_RUN(test_crash_before);
	KT_RUN(test_crash_after);
	KT_RUN(test_replay_once);
	KT_RUN(test_map);
	KT_RUN(test_several);
	KT_RUN(test_revoke);
	KT_RUN(test_handles);
	KT_RUN(test_wrap);
	KT_RUN(test_limit);
	KT_RUN(test_stale_ring);
}
