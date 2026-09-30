/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_blk.c
 *	Block device: open, disk information, sector read and write,
 *	partitions as subunits and the bounds of each one.
 *	Skipped when the machine has no block device attached.
 *
 *	The sectors written are at the end of the device written through:
 *	on the QEMU machine the whole test disk, past its partitions; on
 *	the Raspberry Pi 5 the scratch partition of the card, which the
 *	whole card is only read through.
 */

#include "ktest.h"
#include <ts/blk.h>

LOCAL ID	dd_whole = 0;			/* descriptor of the whole disk */
LOCAL UD	whole_blocks = 0;
LOCAL ID	dd_raw = 0;			/* where sectors are written */
LOCAL UD	raw_blocks = 0;

LOCAL void *blkbuf( SZ size )
{
	return Kmalloc(size);
}

/* the device registered itself and reports a size */
LOCAL void test_open( void )
{
	DiskInfo	info;
	ID		dd;
	SZ		asize;

#ifdef RPI5
	dd = tk_opn_dev((UB*)KT_DISK, TD_READ);
#else
	dd = tk_opn_dev((UB*)KT_DISK, TD_UPDATE);
#endif
	if ( dd < E_OK ) {
		KT_SKIP("no block device");
	}
	dd_whole = dd;

	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asize), E_OK);
	KT_ASSERT_EQ(asize, sizeof(info));
	KT_ASSERT_EQ(info.blocksize, BLK_SECTOR_SIZE);
	KT_ASSERT(info.blockcont > 0);
	whole_blocks = (UD)info.blockcont;
	tm_printf((UB*)"  %s: %d blocks of %d bytes\n", KT_DISK, (INT)whole_blocks, info.blocksize);

#ifdef RPI5
	if ( kt_scratch() != NULL ) {
		dd_raw = tk_opn_dev((UB*)kt_scratch(), TD_UPDATE);
		if ( dd_raw > 0 && tk_srea_dev(dd_raw, TDN_DISKINFO, &info, sizeof(info), &asize) >= E_OK ) {
			raw_blocks = (UD)info.blockcont;
		}
	}
#else
	dd_raw = dd;
	raw_blocks = whole_blocks;
#endif
}

/* a sector written comes back unchanged, and neighbours are untouched */
LOCAL void test_read_write( void )
{
	UB	*wbuf, *rbuf;
	SZ	asize;
	INT	i;
	UD	sect;

	if ( dd_whole <= 0 ) KT_SKIP("no block device");
	if ( dd_raw <= 0 || raw_blocks < 16 ) KT_SKIP(KT_NO_SCRATCH);

	wbuf = (UB *)blkbuf(BLK_SECTOR_SIZE * 3);
	rbuf = (UB *)blkbuf(BLK_SECTOR_SIZE * 3);
	KT_ASSERT(wbuf != NULL && rbuf != NULL);

	sect = raw_blocks - 4;			/* past any partition data */
	for ( i = 0; i < BLK_SECTOR_SIZE * 3; i++ ) {
		wbuf[i] = (UB)(i * 7 + 1);
		rbuf[i] = 0;
	}

	KT_ASSERT_ER(tk_swri_dev(dd_raw, (W)sect, wbuf, BLK_SECTOR_SIZE * 3, &asize), E_OK);
	KT_ASSERT_EQ(asize, BLK_SECTOR_SIZE * 3);
	KT_ASSERT_ER(tk_srea_dev(dd_raw, (W)sect, rbuf, BLK_SECTOR_SIZE * 3, &asize), E_OK);
	KT_ASSERT_EQ(asize, BLK_SECTOR_SIZE * 3);
	for ( i = 0; i < BLK_SECTOR_SIZE * 3; i++ ) {
		if ( rbuf[i] != wbuf[i] ) {
			KT_ASSERT_EQ(rbuf[i], wbuf[i]);
			break;
		}
	}

	/* one sector at a time, reading back the middle one only */
	for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
		wbuf[i] = (UB)(255 - (i & 0xff));
	}
	KT_ASSERT_ER(tk_swri_dev(dd_raw, (W)(sect + 1), wbuf, BLK_SECTOR_SIZE, &asize), E_OK);
	KT_ASSERT_ER(tk_srea_dev(dd_raw, (W)sect, rbuf, BLK_SECTOR_SIZE * 3, &asize), E_OK);
	for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
		if ( rbuf[BLK_SECTOR_SIZE + i] != wbuf[i] ) {
			KT_ASSERT_EQ(rbuf[BLK_SECTOR_SIZE + i], wbuf[i]);
			break;
		}
	}
	KT_ASSERT_EQ(rbuf[0], (UB)1);				/* sector before: unchanged */
	KT_ASSERT_EQ(rbuf[BLK_SECTOR_SIZE * 2], (UB)(BLK_SECTOR_SIZE * 2 * 7 + 1));

	Kfree(wbuf);
	Kfree(rbuf);
}

#define PAR_TASKS	4
#define PAR_ROUNDS	40
#define PAR_SECT	2		/* sectors of each task */

LOCAL volatile INT	par_bad;
LOCAL volatile INT	par_done;
LOCAL ID		par_sem;

/* One task: its own sectors written and read back, over and over */
LOCAL void par_task( INT stacd, void *exinf )
{
	UB	*w = (UB *)Kmalloc(BLK_SECTOR_SIZE * PAR_SECT);
	UB	*r = (UB *)Kmalloc(BLK_SECTOR_SIZE * PAR_SECT);
	UD	sect = raw_blocks - 12 + (UD)stacd * PAR_SECT;
	SZ	asize;
	INT	n, i, bad = 0;

	for ( n = 0; w != NULL && r != NULL && n < PAR_ROUNDS; n++ ) {
		for ( i = 0; i < BLK_SECTOR_SIZE * PAR_SECT; i++ ) {
			w[i] = (UB)( stacd * 61 + n * 7 + i );
		}
		if ( tk_swri_dev(dd_raw, (W)sect, w, BLK_SECTOR_SIZE * PAR_SECT, &asize) < E_OK
		  || tk_srea_dev(dd_raw, (W)sect, r, BLK_SECTOR_SIZE * PAR_SECT, &asize) < E_OK ) {
			bad++;
			continue;
		}
		for ( i = 0; i < BLK_SECTOR_SIZE * PAR_SECT; i++ ) {
			if ( r[i] != w[i] ) { bad++; break; }
		}
	}
	if ( w == NULL || r == NULL ) bad++;
	if ( w != NULL ) Kfree(w);
	if ( r != NULL ) Kfree(r);
	par_bad += bad;
	par_done++;
	tk_sig_sem(par_sem, 1);
	tk_exd_tsk();
}

/* several tasks at once, each with its own sectors: the device takes
   their requests together and every one comes back to the right task */
LOCAL void test_parallel( void )
{
	T_CSEM	csem;
	T_CTSK	ctsk;
	ID	id;
	INT	i;

	if ( dd_whole <= 0 ) KT_SKIP("no block device");
	if ( dd_raw <= 0 || raw_blocks < 16 ) KT_SKIP(KT_NO_SCRATCH);

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO;
	csem.isemcnt = 0;
	csem.maxsem = PAR_TASKS;
	par_sem = tk_cre_sem(&csem);
	KT_ASSERT(par_sem > 0);
	if ( par_sem <= 0 ) return;
	par_bad = 0;
	par_done = 0;
	for ( i = 0; i < PAR_TASKS; i++ ) {
		ctsk.exinf   = NULL;
		ctsk.tskatr  = TA_HLNG | TA_RNG0;
		ctsk.task    = (FP)par_task;
		ctsk.itskpri = KT_PRI_MAIN;
		ctsk.stksz   = 8192;
		ctsk.assprc  = 0;
		id = tk_cre_tsk(&ctsk);
		KT_ASSERT(id > 0);
		if ( id > 0 ) KT_ASSERT_ER(tk_sta_tsk(id, i), E_OK);
	}
	for ( i = 0; i < PAR_TASKS; i++ ) {
		KT_ASSERT_ER(tk_wai_sem(par_sem, 1, 20000), E_OK);
	}
	KT_ASSERT_EQ(par_done, PAR_TASKS);
	KT_ASSERT_EQ(par_bad, 0);
	tk_del_sem(par_sem);
}

/* requests outside the device or not a whole number of sectors are refused */
LOCAL void test_bounds( void )
{
	UB	*buf;
	SZ	asize;

	if ( dd_whole <= 0 ) KT_SKIP("no block device");

	buf = (UB *)blkbuf(BLK_SECTOR_SIZE);
	KT_ASSERT(buf != NULL);

	KT_ASSERT_ER(tk_srea_dev(dd_whole, (W)whole_blocks, buf, BLK_SECTOR_SIZE, &asize), E_PAR);
	KT_ASSERT_ER(tk_srea_dev(dd_whole, 0, buf, BLK_SECTOR_SIZE - 1, &asize), E_PAR);
	KT_ASSERT_ER(tk_srea_dev(dd_whole, 0, buf, BLK_SECTOR_SIZE, &asize), E_OK);

	Kfree(buf);
}

/* each partition is a subunit whose size matches its table entry */
LOCAL void test_partitions( void )
{
	DiskInfo	info;
	T_RDEV		rdev;
	ID		dd;
	SZ		asize;
	UB		name[8];
	UB		*buf;
	INT		nsub, i;

	if ( dd_whole <= 0 ) KT_SKIP("no block device");

	KT_ASSERT(tk_oref_dev(dd_whole, &rdev) > 0);
	nsub = rdev.nsub;
	tm_printf((UB*)"  %s: %d partitions\n", KT_DISK, nsub);
	if ( nsub == 0 ) KT_SKIP("no partition table");

	buf = (UB *)blkbuf(BLK_SECTOR_SIZE);
	KT_ASSERT(buf != NULL);

	for ( i = 0; i < nsub; i++ ) {
		INT	k;

		for ( k = 0; KT_DISK[k] != '\0'; k++ ) {
			name[k] = (UB)KT_DISK[k];
		}
		name[k++] = (UB)('0' + i);
		name[k] = '\0';

		dd = tk_opn_dev(name, TD_READ);
		KT_ASSERT(dd > 0);
		if ( dd <= 0 ) continue;

		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asize), E_OK);
		KT_ASSERT(info.blockcont > 0);
		KT_ASSERT((UD)info.blockcont < whole_blocks);
		tm_printf((UB*)"  %s: %d blocks\n", name, info.blockcont);

		/* the first sector of the partition is readable, one past its end is not */
		KT_ASSERT_ER(tk_srea_dev(dd, 0, buf, BLK_SECTOR_SIZE, &asize), E_OK);
		KT_ASSERT_ER(tk_srea_dev(dd, info.blockcont, buf, BLK_SECTOR_SIZE, &asize), E_PAR);

		KT_ASSERT_ER(tk_cls_dev(dd, 0), E_OK);
	}
	Kfree(buf);
}

/* a partition holds what was written through it, seen from the whole device */
LOCAL void test_partition_offset( void )
{
	T_RDEV	rdev;
	ID	dd;
	SZ	asize;
	UB	*wbuf, *rbuf;
	DiskInfo info;
	INT	i;

	if ( dd_whole <= 0 ) KT_SKIP("no block device");
	KT_ASSERT(tk_oref_dev(dd_whole, &rdev) > 0);
	if ( rdev.nsub == 0 ) KT_SKIP("no partition table");
	if ( kt_scratch() == NULL ) KT_SKIP(KT_NO_SCRATCH);

	dd = tk_opn_dev((UB*)kt_scratch(), TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &info, sizeof(info), &asize), E_OK);

	wbuf = (UB *)blkbuf(BLK_SECTOR_SIZE);
	rbuf = (UB *)blkbuf(BLK_SECTOR_SIZE);
	KT_ASSERT(wbuf != NULL && rbuf != NULL);
	for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
		wbuf[i] = (UB)(i ^ 0x5a);
	}

	/* last sector of the partition, then the same sector of the disk */
	KT_ASSERT_ER(tk_swri_dev(dd, info.blockcont - 1, wbuf, BLK_SECTOR_SIZE, &asize), E_OK);
	KT_ASSERT_ER(tk_srea_dev(dd, info.blockcont - 1, rbuf, BLK_SECTOR_SIZE, &asize), E_OK);
	for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
		if ( rbuf[i] != wbuf[i] ) {
			KT_ASSERT_EQ(rbuf[i], wbuf[i]);
			break;
		}
	}
	KT_ASSERT_ER(tk_cls_dev(dd, 0), E_OK);

	Kfree(wbuf);
	Kfree(rbuf);
}

EXPORT void ktest_blk( void )
{
	KT_RUN(test_open);
	KT_RUN(test_read_write);
	KT_RUN(test_parallel);
	KT_RUN(test_bounds);
	KT_RUN(test_partitions);
	KT_RUN(test_partition_offset);

	if ( dd_raw > 0 && dd_raw != dd_whole ) {
		tk_cls_dev(dd_raw, 0);
	}
	dd_raw = 0;
	if ( dd_whole > 0 ) {
		tk_cls_dev(dd_whole, 0);
		dd_whole = 0;
	}
}
