/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfsbtree.c
 *	The object index of a native volume (design 11.6).
 *
 *	The keys are made here rather than taken from the clock, so that the
 *	order they go in can be chosen: in order, which is what real UUIDs
 *	do, and out of order, which is what a restore does.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/time.h>

#define BTDEV		kt_scratch()

LOCAL ID	vol = 0;

/* A key whose byte order follows n, as a version 7 UUID follows time */
LOCAL void key_of( TS_UUID *k, UD n )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		k->b[i] = 0;
	}
	for ( i = 7; i >= 0; i-- ) {
		k->b[i] = (UB)(n & 0xff);
		n >>= 8;
	}
}

/* the index starts empty and takes the first key */
LOCAL void test_open( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0;

	if ( ts_format_blk(BTDEV, "BTREE") < E_OK ) {
		KT_SKIP("no block device to make a volume on");
	}
	vol = ts_opn_vol_blk(BTDEV);
	if ( vol <= 0 ) {
		KT_SKIP("the volume would not open");
	}

	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 0);

	key_of(&k, 1);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_NOEXS);
	KT_ASSERT_ER(ts_btree_insert(vol, &k, 100), E_OK);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
	KT_ASSERT_EQ(v, 100);

	/* the same key twice is refused and changes nothing */
	KT_ASSERT_ER(ts_btree_insert(vol, &k, 200), E_OBJ);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
	KT_ASSERT_EQ(v, 100);

	/* pointing it somewhere else is a different call */
	KT_ASSERT_ER(ts_btree_update(vol, &k, 200), E_OK);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
	KT_ASSERT_EQ(v, 200);

	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 1);

	KT_ASSERT_ER(ts_btree_delete(vol, &k), E_OK);
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 0);
}

/* one leaf holds 166 keys, so 500 of them make a tree of three levels */
LOCAL void test_many( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0;
	UD	i;
	UD	n = 500;

	if ( vol <= 0 ) KT_SKIP("no volume");

	for ( i = 1; i <= n; i++ ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_insert(vol, &k, i * 10), E_OK);
	}
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, n);

	/* every one of them is found, and points where it was put */
	for ( i = 1; i <= n; i++ ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
		if ( v != i * 10 ) {
			tm_printf((UB*)"  key %d gave %d\n", (INT)i, (INT)v);
		}
		KT_ASSERT_EQ(v, i * 10);
	}

	/* one that was never put in is not found */
	key_of(&k, n + 1);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_NOEXS);
	key_of(&k, 0);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_NOEXS);
}

/* a walk gives the keys in order, once each */
LOCAL void test_walk( void )
{
	TS_UUID	k, prev;
	UD	v = 0, seen = 0, count = 0;
	ER	er;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT(count > 0);

	er = ts_btree_first(vol, &k, &v);
	KT_ASSERT_ER(er, E_OK);
	seen = 1;
	prev = k;

	for (;;) {
		er = ts_btree_next(vol, &prev, &k, &v);
		if ( er == E_NOEXS ) {
			break;
		}
		KT_ASSERT_ER(er, E_OK);
		KT_ASSERT(ts_uuid_cmp(&prev, &k) < 0);	/* strictly rising */
		prev = k;
		seen++;
		if ( seen > count ) {
			break;			/* a loop: caught below */
		}
	}
	KT_ASSERT_EQ(seen, count);
}

/* keys given in reverse order end up in the same place */
LOCAL void test_reverse( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0, i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_drop(vol), E_OK);
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 0);

	for ( i = 400; i >= 1; i-- ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_insert(vol, &k, i), E_OK);
	}
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 400);

	for ( i = 1; i <= 400; i++ ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
		KT_ASSERT_EQ(v, i);
	}
}

/* what is taken out stays out, and what is left is still reachable */
LOCAL void test_delete( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0, i, before = 0;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_count(vol, &before), E_OK);
	KT_ASSERT(before >= 400);

	/* every other key goes */
	for ( i = 2; i <= 400; i += 2 ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_delete(vol, &k), E_OK);
	}
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, before - 200);

	for ( i = 1; i <= 400; i++ ) {
		key_of(&k, i);
		if ( (i % 2) == 0 ) {
			KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_NOEXS);
		} else {
			KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
			KT_ASSERT_EQ(v, i);
		}
	}

	/* taking one out twice is refused */
	key_of(&k, 2);
	KT_ASSERT_ER(ts_btree_delete(vol, &k), E_NOEXS);

	/* and the walk still gives what is left, in order */
	{
		TS_UUID	prev;
		UD	seen = 1;
		ER	er = ts_btree_first(vol, &prev, &v);

		KT_ASSERT_ER(er, E_OK);
		for (;;) {
			er = ts_btree_next(vol, &prev, &k, &v);
			if ( er == E_NOEXS ) break;
			KT_ASSERT_ER(er, E_OK);
			KT_ASSERT(ts_uuid_cmp(&prev, &k) < 0);
			prev = k;
			seen++;
			if ( seen > count ) break;
		}
		KT_ASSERT_EQ(seen, count);
	}
}

/*
 * Enough keys that the root itself fills and splits, which is the only
 * way an internal node is divided. One leaf takes 166 keys and splits
 * into halves, so about 15000 keys make the root overflow its 166
 * separators.
 */
LOCAL void test_deep( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0, i;
	UD	n = 15000;
	UD	t0 = 0, t1 = 0;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_drop(vol), E_OK);

	ts_get_mono(&t0);
	for ( i = 1; i <= n; i++ ) {
		key_of(&k, i);
		if ( (i % 5000) == 0 ) {
			UD tn = 0;

			ts_get_mono(&tn);
			tm_printf((UB*)"  %d keys at %d ms\n",
				  (INT)i, (INT)((tn - t0) / 1000000U));
		}
		if ( ts_btree_insert(vol, &k, i) < E_OK ) {
			tm_printf((UB*)"  insert of key %d failed\n", (INT)i);
			KT_ASSERT(0);
			break;
		}
	}
	ts_get_mono(&t1);

	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, n);
	tm_printf((UB*)"  %d keys in %d ms\n",
		  (INT)n, (INT)((t1 - t0) / 1000000U));

	/* a sample of them, spread across the tree */
	for ( i = 1; i <= n; i += 97 ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
		KT_ASSERT_EQ(v, i);
	}
	key_of(&k, n);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
	KT_ASSERT_EQ(v, n);

	KT_ASSERT_ER(ts_btree_drop(vol), E_OK);
}

/* the index survives the volume being closed and opened again */
LOCAL void test_remount( void )
{
	TS_UUID	k;
	UD	count = 0, v = 0, before = 0;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_count(vol, &before), E_OK);
	KT_ASSERT_ER(ts_sync_blk(vol), E_OK);
	KT_ASSERT_ER(ts_cls_vol_blk(vol), E_OK);

	vol = ts_opn_vol_blk(BTDEV);
	KT_ASSERT(vol > 0);

	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, before);

	key_of(&k, 1);
	KT_ASSERT_ER(ts_btree_lookup(vol, &k, &v), E_OK);
	KT_ASSERT_EQ(v, 1);
}

/* dropping the index gives every node back to the volume */
LOCAL void test_drop( void )
{
	T_TSFSBLK_SB	sb;
	TS_UUID		k;
	UD		count = 0, free_before = 0, free_after = 0, i;

	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_btree_drop(vol), E_OK);
	KT_ASSERT_ER(ts_btree_count(vol, &count), E_OK);
	KT_ASSERT_EQ(count, 0);

	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	free_before = sb.free_blocks;

	/* put a tree in and take it away again: the blocks come back */
	for ( i = 1; i <= 200; i++ ) {
		key_of(&k, i);
		KT_ASSERT_ER(ts_btree_insert(vol, &k, i), E_OK);
	}
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	KT_ASSERT(sb.free_blocks < free_before);

	KT_ASSERT_ER(ts_btree_drop(vol), E_OK);
	KT_ASSERT_ER(ts_ref_vol_blk(vol, &sb), E_OK);
	free_after = sb.free_blocks;
	KT_ASSERT_EQ(free_after, free_before);

	ts_cls_vol_blk(vol);
	vol = 0;
}

EXPORT void ktest_tsfsbtree( void )
{
	KT_RUN(test_open);
	KT_RUN(test_many);
	KT_RUN(test_walk);
	KT_RUN(test_reverse);
	KT_RUN(test_delete);
	KT_RUN(test_remount);
	KT_RUN(test_deep);
	KT_RUN(test_drop);
}
