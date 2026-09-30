/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfsblk.c
 *	TSFS native volume, block layer: making a volume, mounting it,
 *	allocating and freeing blocks, that the state survives a remount,
 *	the two superblocks, the heads of the groups and the common head of
 *	a management block, and emptying the device's cache (design 11.6,
 *	11.14). The first partition of the test disk carries the TSFS type
 *	GUID and is used for this.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/crc32c.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/blk.h>

#define TSFSDEV	kt_scratch()

LOCAL ID	vol = 0;
LOCAL UD	free_after_format = 0;

/* a volume is made and its superblock reads back */
LOCAL void test_format( void )
{
	T_TSFSBLK_SB	sb;

	if ( ts_format_blk(TSFSDEV, "TessronOS test") < E_OK ) {
		KT_SKIP("no block device for the native volume");
	}
	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION);
	KT_ASSERT_EQ(sb.block_size, TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(sb.total_blocks > 0);
	KT_ASSERT(sb.ag_count >= 4);			/* even a small volume has four */
	KT_ASSERT_EQ(sb.journal_start, 2);
	KT_ASSERT(sb.journal_blocks >= TSFSJ_AREA_BLOCKS);
	KT_ASSERT(sb.free_blocks < sb.total_blocks);
	KT_ASSERT_EQ(sb.label[0], (UB)'T');
	KT_ASSERT_EQ(sb.mount_count, 1);
	KT_ASSERT_EQ(sb.state & TSFSBLK_ST_CLEAN, 0);	/* in use */
	KT_ASSERT_EQ(sb.state & TSFSBLK_ST_ERROR, 0);
	free_after_format = sb.free_blocks;

	tm_printf((UB*)"  %s: %d blocks, journal %d, %d groups of %d, %d free\n",
		TSFSDEV, (INT)sb.total_blocks, (INT)sb.journal_blocks,
		(INT)sb.ag_count, (INT)sb.ag_blocks, (INT)sb.free_blocks);
}

/*
 * The checksum the whole format rests on. AArch64 computes it with
 * instructions of its own, so the answers are checked against values
 * that are known from elsewhere rather than against the code itself.
 */
LOCAL void test_crc32c( void )
{
	UB	*buf;
	INT	i;

	KT_ASSERT_EQ(ts_crc32c((CONST UB *)"123456789", 9), 0xE3069283U);
	KT_ASSERT_EQ(ts_crc32c((CONST UB *)"", 0), 0x00000000U);

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);

	/* thirty two zero bytes, a value the storage standards quote */
	knl_memset(buf, 0, 32);
	KT_ASSERT_EQ(ts_crc32c(buf, 32), 0x8A9136AAU);

	/* a whole block, and a run that starts and ends off a boundary:
	   the instruction version handles those two differently */
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) {
		buf[i] = (UB)(i & 0xff);
	}
	KT_ASSERT_EQ(ts_crc32c(buf, TSFSBLK_BLOCK_SIZE), 0x9C71FE32U);
	KT_ASSERT_EQ(ts_crc32c(buf + 3, 1021), 0x426F53D8U);

	Kfree(buf);
}

/* blocks come out of the map and go back into it */
LOCAL void test_alloc( void )
{
	T_TSFSBLK_SB	sb;
	UD		blk[16];
	INT		i, k;

	if ( vol <= 0 ) KT_SKIP("no volume");

	for ( i = 0; i < 16; i++ ) {
		KT_ASSERT_ER(ts_alloc_blk(vol, &blk[i]), E_OK);
		/* never one of the blocks the volume itself uses */
		KT_ASSERT(blk[i] > 0);
		for ( k = 0; k < i; k++ ) {
			KT_ASSERT(blk[i] != blk[k]);	/* and never twice */
		}
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format - 16);

	for ( i = 0; i < 16; i++ ) {
		KT_ASSERT_ER(ts_free_blk(vol, blk[i]), E_OK);
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);

	/* freeing what is already free, or what the volume needs, is refused:
	   the two superblocks, the journal, a group's head and its map */
	KT_ASSERT_ER(ts_free_blk(vol, blk[0]), E_OBJ);
	KT_ASSERT_ER(ts_free_blk(vol, 0), E_PAR);
	KT_ASSERT_ER(ts_free_blk(vol, 1), E_PAR);
	KT_ASSERT_ER(ts_free_blk(vol, sb.journal_start), E_PAR);
	KT_ASSERT_ER(ts_free_blk(vol, sb.journal_start + sb.journal_blocks), E_PAR);
	KT_ASSERT_ER(ts_free_blk(vol, sb.journal_start + sb.journal_blocks + sb.ag_blocks + 1), E_PAR);
	KT_ASSERT_ER(ts_free_blk(vol, sb.total_blocks), E_PAR);
}

/* runs of blocks come out whole, and go back whole */
LOCAL void test_extent( void )
{
	T_TSFSBLK_SB	sb;
	UD		start, count, s2, c2, hole;
	INT		i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	/* a run of eight is eight blocks in a row */
	KT_ASSERT_ER(ts_alloc_ext(vol, 8, &start, &count), E_OK);
	KT_ASSERT_EQ(count, 8);
	KT_ASSERT(start > 0);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format - 8);

	/* the next run does not overlap it */
	KT_ASSERT_ER(ts_alloc_ext(vol, 4, &s2, &c2), E_OK);
	KT_ASSERT_EQ(c2, 4);
	KT_ASSERT(s2 >= start + count || s2 + c2 <= start);

	KT_ASSERT_ER(ts_free_ext(vol, start, count), E_OK);
	KT_ASSERT_ER(ts_free_ext(vol, s2, c2), E_OK);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);

	/* a run that is already free, and one outside the volume */
	KT_ASSERT_ER(ts_free_ext(vol, start, count), E_OBJ);
	KT_ASSERT_ER(ts_free_ext(vol, 0, 1), E_PAR);
	KT_ASSERT_ER(ts_free_ext(vol, sb.journal_start, 1), E_PAR);
	KT_ASSERT_ER(ts_free_ext(vol, start, 0), E_PAR);
	KT_ASSERT_ER(ts_alloc_ext(vol, 0, &s2, &c2), E_PAR);

	/* A hole of three between blocks that are in use gives a run of
	   three, not the ten that were asked for. */
	KT_ASSERT_ER(ts_alloc_ext(vol, 16, &start, &count), E_OK);
	KT_ASSERT_EQ(count, 16);
	hole = start + 4;
	KT_ASSERT_ER(ts_free_ext(vol, hole, 3), E_OK);

	KT_ASSERT_ER(ts_alloc_ext(vol, 10, &s2, &c2), E_OK);
	KT_ASSERT_EQ(s2, hole);
	KT_ASSERT_EQ(c2, 3);

	KT_ASSERT_ER(ts_free_ext(vol, s2, c2), E_OK);
	KT_ASSERT_ER(ts_free_ext(vol, start, 4), E_OK);
	KT_ASSERT_ER(ts_free_ext(vol, start + 7, 9), E_OK);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);

	/* single blocks still work, and are runs of one */
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_ER(ts_alloc_blk(vol, &s2), E_OK);
		KT_ASSERT_ER(ts_free_blk(vol, s2), E_OK);
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);
}

/* what is written to a block is there when it is read back */
LOCAL void test_read_write( void )
{
	UB	*wbuf, *rbuf;
	UD	blk;
	INT	i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	wbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	rbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(wbuf != NULL && rbuf != NULL);
	if ( wbuf == NULL || rbuf == NULL ) return;

	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_OK);
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) {
		wbuf[i] = (UB)(i * 13 + 5);
		rbuf[i] = 0;
	}
	KT_ASSERT_ER(ts_write_blk(vol, blk, wbuf), E_OK);
	KT_ASSERT_ER(ts_read_blk(vol, blk, rbuf), E_OK);
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) {
		if ( rbuf[i] != wbuf[i] ) {
			KT_ASSERT_EQ(rbuf[i], wbuf[i]);
			break;
		}
	}
	KT_ASSERT_ER(ts_free_blk(vol, blk), E_OK);

	Kfree(wbuf);
	Kfree(rbuf);
}

/* a block read twice comes from the cache the second time, and a write
   puts the new bytes there */
LOCAL void test_cache( void )
{
	T_TSFSBLK_CACHE	c0, c1;
	UB		*a, *b;
	UD		blk;
	INT		i, bad = 0;

	if ( vol <= 0 ) KT_SKIP("no volume");

	a = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(a != NULL && b != NULL);
	if ( a == NULL || b == NULL ) return;

	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_OK);
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) a[i] = (UB)( i * 3 + 1 );
	KT_ASSERT_ER(ts_write_blk(vol, blk, a), E_OK);
	KT_ASSERT_ER(ts_cache_blk(vol, &c0), E_OK);
	KT_ASSERT_ER(ts_read_blk(vol, blk, b), E_OK);
	KT_ASSERT_ER(ts_read_blk(vol, blk, b), E_OK);
	KT_ASSERT_ER(ts_cache_blk(vol, &c1), E_OK);
	KT_ASSERT_EQ(c1.hits - c0.hits, 2);		/* the write left it there */
	KT_ASSERT(c1.blocks >= 1 && c1.blocks <= c1.max);
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) if ( b[i] != a[i] ) bad++;
	KT_ASSERT_EQ(bad, 0);

	/* written again: the next read has the new bytes */
	for ( i = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) a[i] = (UB)( i * 5 + 2 );
	KT_ASSERT_ER(ts_write_blk(vol, blk, a), E_OK);
	KT_ASSERT_ER(ts_read_blk(vol, blk, b), E_OK);
	for ( i = 0, bad = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) if ( b[i] != a[i] ) bad++;
	KT_ASSERT_EQ(bad, 0);

	/* and a remount starts the cache again with what the medium holds */
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol > 0 ) {
		KT_ASSERT_ER(ts_read_blk(vol, blk, b), E_OK);
		for ( i = 0, bad = 0; i < TSFSBLK_BLOCK_SIZE; i++ ) if ( b[i] != a[i] ) bad++;
		KT_ASSERT_EQ(bad, 0);
		KT_ASSERT_ER(ts_free_blk(vol, blk), E_OK);
	}
	Kfree(a);
	Kfree(b);
}

/* the map and the counters survive a remount */
LOCAL void test_remount( void )
{
	T_TSFSBLK_SB	sb;
	UD		blk[4];
	INT		i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_ER(ts_alloc_blk(vol, &blk[i]), E_OK);
	}
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format - 4);
	KT_ASSERT_EQ(sb.mount_count, 2);

	/* the same blocks are still taken, so new ones differ */
	{
		UD	again;

		KT_ASSERT_ER(ts_alloc_blk(vol, &again), E_OK);
		for ( i = 0; i < 4; i++ ) {
			KT_ASSERT(again != blk[i]);
		}
		KT_ASSERT_ER(ts_free_blk(vol, again), E_OK);
	}
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_ER(ts_free_blk(vol, blk[i]), E_OK);
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);
}

/* a device that holds no volume is not taken for one */
LOCAL void test_not_a_volume( void )
{
	if ( vol <= 0 ) KT_SKIP("no volume");

	/* the FAT partition is not a native volume */
	KT_ASSERT(ts_opn_vol_blk(KT_BOOTDEV) < E_OK);
	KT_ASSERT_ER(ts_opn_vol_blk("nosuch"), E_NOEXS);
}

/* a run never crosses from one group into the next */
LOCAL void test_groups( void )
{
	T_TSFSBLK_SB	sb;
	UD		start, count, first, i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	first = sb.journal_start + sb.journal_blocks;
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_ER(ts_alloc_ext(vol, sb.ag_blocks, &start, &count), E_OK);
		KT_ASSERT(count < sb.ag_blocks);		/* the next group's head is in the way */
		KT_ASSERT_EQ(( start - first ) / sb.ag_blocks,
			     ( start + count - 1 - first ) / sb.ag_blocks);
		KT_ASSERT_ER(ts_free_ext(vol, start, count), E_OK);
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);
}

/* the device empties its cache when asked, directly and through the volume */
LOCAL void test_flush( void )
{
	SZ	asize;
	ID	dd;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_flush_blk(vol), E_OK);
	dd = tk_opn_dev((UB *)KT_DISK, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asize), E_OK);
		tk_cls_dev(dd, 0);
	}

	/* USB mass storage, when there is one: SYNCHRONIZE CACHE */
	dd = tk_opn_dev((UB *)"uda", TD_UPDATE);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asize), E_OK);
		tk_cls_dev(dd, 0);
	}
}

/* Write one block of the device behind the volume's back */
LOCAL ER spoil( UD blk )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	SZ	asize;
	ID	dd;
	ER	er;

	if ( buf == NULL ) return E_NOMEM;
	knl_memset(buf, 0xA5, TSFSBLK_BLOCK_SIZE);
	dd = tk_opn_dev((UB *)TSFSDEV, TD_UPDATE);
	er = ( dd > 0 ) ? tk_swri_dev(dd, (W)( blk * TSFSBLK_SECT_PER_BLK ), buf,
				       TSFSBLK_BLOCK_SIZE, &asize) : (ER)dd;
	if ( dd > 0 ) tk_cls_dev(dd, 0);
	Kfree(buf);
	return er;
}

/*
 * The two superblocks: each write goes to the other copy with the
 * generation one higher, and a volume whose newer copy is ruined opens
 * from the older one.
 */
LOCAL void test_two_superblocks( void )
{
	T_TSFSBLK_SB	sb;
	UD		gen;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);	/* written on closing */
	vol = ts_opn_vol_blk(TSFSDEV);			/* and on opening */
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	gen = sb.generation;
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);	/* generation gen + 1, in copy (gen + 1) & 1 */

	KT_ASSERT_ER(spoil(( gen + 1 ) & 1), E_OK);
	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT_EQ(sb.generation, gen + 1);		/* the older copy, then this opening */
	KT_ASSERT_EQ(sb.state & TSFSBLK_ST_ERROR, 0);
	KT_ASSERT_EQ(sb.free_blocks, free_after_format);
}

/* a sealed block checks; one changed afterwards does not */
LOCAL void test_seal( void )
{
	UB	*buf;
	UD	blk;

	if ( vol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_OK);
	knl_memset(buf, 0x3C, TSFSBLK_BLOCK_SIZE);
	KT_ASSERT_ER(ts_blk_seal(vol, buf, TSFSBLK_MAGIC_NODE, blk, 7), E_OK);
	KT_ASSERT_ER(ts_write_blk(vol, blk, buf), E_OK);
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	KT_ASSERT_ER(ts_read_blk(vol, blk, buf), E_OK);
	KT_ASSERT_ER(ts_blk_check(vol, buf, TSFSBLK_MAGIC_NODE, blk), E_OK);
	KT_ASSERT_ER(ts_free_blk(vol, blk), E_OK);
	Kfree(buf);
}

/*
 * What does not pass its check stops the volume writing, and says so
 * in the superblock; the volume opens read only after that. Last, since
 * it leaves the volume that way.
 */
LOCAL void test_damage( void )
{
	T_TSFSBLK_SB	sb;
	UB		*buf;
	UD		blk, first;

	if ( vol <= 0 ) KT_SKIP("no volume");

	/* a block that claims to be somewhere else */
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_OK);
	knl_memset(buf, 0x3C, TSFSBLK_BLOCK_SIZE);
	KT_ASSERT_ER(ts_blk_seal(vol, buf, TSFSBLK_MAGIC_NODE, blk + 1, 7), E_OK);
	KT_ASSERT_ER(ts_blk_check(vol, buf, TSFSBLK_MAGIC_NODE, blk), E_OBJ);
	Kfree(buf);

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT(( sb.state & TSFSBLK_ST_ERROR ) != 0);
	KT_ASSERT_EQ(sb.err_blk, blk);
	KT_ASSERT_EQ(sb.err_kind, TSFSBLK_ERR_CSUM);
	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_RONLY);	/* no more writing */
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_RONLY);	/* still, until it is checked */
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	/* a group head ruined on the medium is found when the volume opens */
	KT_ASSERT(ts_format_blk(TSFSDEV, "TessronOS test") >= E_OK);
	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	first = sb.journal_start + sb.journal_blocks + sb.ag_blocks;	/* group 1 */
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);
	KT_ASSERT_ER(spoil(first), E_OK);
	vol = ts_opn_vol_blk(TSFSDEV);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT(( sb.state & TSFSBLK_ST_ERROR ) != 0);
	KT_ASSERT_EQ(sb.err_blk, first);
	KT_ASSERT_ER(ts_alloc_blk(vol, &blk), E_RONLY);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	/* made again, for the suites after this one */
	KT_ASSERT(ts_format_blk(TSFSDEV, "TessronOS test") >= E_OK);
	vol = 0;
}

EXPORT void ktest_tsfsblk( void )
{
	KT_RUN(test_crc32c);
	KT_RUN(test_format);
	KT_RUN(test_alloc);
	KT_RUN(test_extent);
	KT_RUN(test_read_write);
	KT_RUN(test_remount);
	KT_RUN(test_cache);
	KT_RUN(test_groups);
	KT_RUN(test_flush);
	KT_RUN(test_not_a_volume);
	KT_RUN(test_two_superblocks);
	KT_RUN(test_seal);
	KT_RUN(test_damage);

	if ( vol > 0 ) {
		ts_cls_vol_blk(vol);
		vol = 0;
	}
}
