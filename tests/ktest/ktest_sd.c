/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_sd.c
 *	SD card driver (device/sd/sdhci.c): the size read from the card,
 *	the partitions as subunits, and sector writes and reads in each way
 *	the controller moves data -- through the data port, the single
 *	address mode and the descriptor tables -- each checked through
 *	another way and through the whole card, with the sectors on either
 *	side of every write checked to be untouched; the same with multiple
 *	block transfers ended by CMD12, CMD23 and Auto-CMD23; and the ways
 *	back to 3.3V from a switch to 1.8V that fails.
 *
 *	Only the scratch partition of the card is written (the type the
 *	tests own, mksd.py --scratch-mb). Under QEMU the card is an image
 *	made the same way on the machine's sdhci-pci; on the Raspberry Pi 5
 *	it is the card the machine started from. Skipped when there is no
 *	card, and the writes when it has no scratch partition.
 */

#include "ktest.h"
#include <ts/blk.h>
#include "sysman/pfalloc.h"

#define SD_DISK		"sda"

/* the scratch type, in GPT mixed-endian form */
LOCAL CONST UB	sd_scratch_type[16] = {
	0x31, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x40,
	0x80, 0x00, 0x54, 0x46, 0x53, 0x2d, 0x53, 0x43
};

LOCAL CONST char *mode_name[4] = { "data port", "SDMA", "ADMA2/32", "ADMA2/64" };

/*
 * Sizes written, in sectors: one; a few; more than one descriptor of
 * the table carries (32KB); more than the 512KB boundary at which the
 * single address mode stops and asks for the rest.
 */
LOCAL CONST UD	sizes[] = { 1, 7, 129, 1100 };
#define NSIZES		(sizeof(sizes) / sizeof(sizes[0]))
#define MAX_SECT	1100
#define BUF_ORDER	8			/* 1MB of pages: MAX_SECT + 2 sectors and an offset */

LOCAL ID	dd_whole;
LOCAL UD	whole_blocks;
LOCAL T_PARTTBL	*tbl;
LOCAL INT	nparts;

LOCAL ER disk_read( void *exinf, UD start, UD nsect, void *buf )
{
	SZ	asz = 0;

	return tk_srea_dev(*(ID *)exinf, (W)start, buf, (SZ)( nsect * BLK_SECTOR_SIZE ), &asz);
}

LOCAL BOOL same( CONST UB *a, CONST UB *b, SZ n )
{
	while ( n-- > 0 ) {
		if ( *a++ != *b++ ) return FALSE;
	}
	return TRUE;
}

LOCAL void wipe( UB *p, SZ n )
{
	while ( n-- > 0 ) *p++ = 0xEE;
}

LOCAL void part_name( UB *name, INT i )
{
	INT	k;

	for ( k = 0; SD_DISK[k] != '\0'; k++ ) {
		name[k] = (UB)SD_DISK[k];
	}
	name[k++] = (UB)( '0' + i );
	name[k] = '\0';
}

/* the byte 'i' of the sector at 'lba', as written by pass 'tag' */
LOCAL UB pat( UW tag, UD lba, UW i )
{
	return (UB)( tag * 31 + (UW)lba * 13 + i * 3 + ( i >> 8 ) + ( (UW)lba >> 8 ) );
}

LOCAL void fill( UB *p, UW tag, UD lba, UD n )
{
	UD	j;
	UW	i;

	for ( j = 0; j < n; j++ ) {
		for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
			*p++ = pat(tag, lba + j, i);
		}
	}
}

/* how many sectors from 'lba' on hold pass 'tag'; n when all do */
LOCAL UD check( CONST UB *p, UW tag, UD lba, UD n )
{
	UD	j;
	UW	i;

	for ( j = 0; j < n; j++ ) {
		for ( i = 0; i < BLK_SECTOR_SIZE; i++ ) {
			if ( *p++ != pat(tag, lba + j, i) ) {
				tm_printf((UB *)"  sector %d byte %d: %02x, not %02x\n",
					  (INT)(lba + j), (INT)i, (INT)p[-1],
					  (INT)pat(tag, lba + j, i));
				return j;
			}
		}
	}
	return n;
}

/* the card reports a size, and it holds the image written onto it */
LOCAL void test_card( void )
{
	DiskInfo	info;
	SZ		asize;
	UB		*buf;
	UD		alt;

	dd_whole = tk_opn_dev((UB *)SD_DISK, TD_READ);
	if ( dd_whole <= 0 ) KT_SKIP("no SD card");

	KT_ASSERT_ER(tk_srea_dev(dd_whole, TDN_DISKINFO, &info, sizeof(info), &asize), E_OK);
	whole_blocks = (UD)info.blockcont;
	tm_printf((UB *)"  %s: %d sectors (%d MB), moved by %s\n", SD_DISK,
		  (INT)whole_blocks, (INT)(whole_blocks >> 11),
		  mode_name[knl_sd_dma_mode(-1) & 3]);
#ifdef KT_SD_SECTORS
	KT_ASSERT_EQ(whole_blocks, (UD)KT_SD_SECTORS);
#endif
#ifdef KT_SD_EMMC
	/* an eMMC (make SD_EMMC=1) is soldered on, a card can be taken out */
	KT_ASSERT_EQ(info.removable, ( KT_SD_EMMC ) ? FALSE : TRUE);
#endif
	KT_ASSERT(whole_blocks > 0);

	/* the GPT header says where the image ends: the card is at least that big */
	buf = (UB *)Kmalloc(BLK_SECTOR_SIZE);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(tk_srea_dev(dd_whole, 1, buf, BLK_SECTOR_SIZE, &asize), E_OK);
	KT_ASSERT(same(buf, (CONST UB *)"EFI PART", 8));
	alt = (UD)buf[32] | ((UD)buf[33] << 8) | ((UD)buf[34] << 16) | ((UD)buf[35] << 24)
	    | ((UD)buf[36] << 32) | ((UD)buf[37] << 40);
	KT_ASSERT(alt > 1 && alt < whole_blocks);
	Kfree(buf);

	tbl = (T_PARTTBL *)Kmalloc(sizeof(T_PARTTBL));
	KT_ASSERT(tbl != NULL);
	if ( tbl == NULL ) return;
	nparts = knl_read_parttbl(disk_read, &dd_whole, whole_blocks, tbl);
	KT_ASSERT(nparts >= KT_DISK_PARTS);
	KT_ASSERT(tbl->gpt);
}

/* the fastest clock of each bus speed (SD_SPEED_*) */
LOCAL CONST UW	speed_hz[] = { 25000000, 50000000, 25000000, 50000000, 100000000, 50000000, 208000000 };
LOCAL CONST char *speed_name[] = { "default speed", "High Speed", "UHS-I SDR12", "UHS-I SDR25",
				   "UHS-I SDR50", "UHS-I DDR50", "UHS-I SDR104" };

/*
 * The bus speed: an SD card that has the High Speed function (QEMU's
 * sd-card has it, and the sdhci-pci host the High Speed mode) is
 * switched to it, its clock above the default speed's; the reads and
 * writes of the tests after this one run in that mode. On the Raspberry
 * Pi 5 a UHS-I card may be at a UHS-I bus speed. An eMMC keeps its own
 * timing.
 */
LOCAL void test_speed( void )
{
	UW	hz = 0;
	INT	sp;

	if ( dd_whole <= 0 ) KT_SKIP("no SD card");
	sp = knl_sd_speed(&hz);
	KT_ASSERT(sp >= SD_SPEED_DEFAULT && sp <= SD_SPEED_SDR104);
	if ( sp < SD_SPEED_DEFAULT || sp > SD_SPEED_SDR104 ) return;
	tm_printf((UB *)"  %s: %s, clock %d kHz, ended by %s\n", SD_DISK, speed_name[sp], (INT)( hz / 1000 ),
		  ( knl_sd_cmd23(-1) == SD_CMD23_AUTO ) ? "Auto-CMD23"
		  : ( knl_sd_cmd23(-1) == SD_CMD23_MANUAL ) ? "CMD23" : "CMD12");
#if defined(QEMU_VIRT) && defined(KT_SD_EMMC)
	if ( !KT_SD_EMMC ) {
		KT_ASSERT_EQ(sp, SD_SPEED_HS);
		KT_ASSERT(hz > 25000000 && hz <= 50000000);
		/* QEMU's sd-card does not say it takes CMD23: CMD12 */
		KT_ASSERT_EQ(knl_sd_cmd23(-1), SD_CMD23_NONE);
	}
	KT_ASSERT(hz <= speed_hz[sp]);
#else
	KT_ASSERT(hz <= speed_hz[sp]);
#endif
}

/*
 * How long 8MB from the start of the card takes, read in requests of
 * 128KB, 512KB and 1MB (design 7.3, the amount read at once): printed
 * only, the reads checked to succeed.
 */
LOCAL void test_read_size( void )
{
	static CONST UD	sizes_kb[] = { 128, 512, 1024 };
	PFRAME		*pf;
	UB		*buf;
	SYSTIM		t0, t1;
	SZ		asz;
	UD		at, k, n;

	if ( dd_whole <= 0 ) KT_SKIP("no SD card");
	pf = knl_alloc_pages(BUF_ORDER, ZONE_DMA32, KAF_DMA32);
	KT_ASSERT(pf != NULL);
	if ( pf == NULL ) return;
	buf = (UB *)knl_pf_to_va(pf);
	for ( k = 0; k < sizeof(sizes_kb) / sizeof(sizes_kb[0]); k++ ) {
		n = sizes_kb[k] * 1024 / BLK_SECTOR_SIZE;
		tk_get_tim(&t0);
		for ( at = 0; at < 8 * 2048; at += n ) {
			asz = 0;
			KT_ASSERT_ER(tk_srea_dev(dd_whole, (W)at, buf, (SZ)( n * BLK_SECTOR_SIZE ), &asz), E_OK);
		}
		tk_get_tim(&t1);
		tm_printf((UB *)"  %s: 8MB in requests of %d KB: %d ms\n", SD_DISK, (INT)sizes_kb[k],
			  (INT)( t1.lo - t0.lo ));
	}
	knl_free_pages(pf, BUF_ORDER);
}

/* each partition's first and last sectors are the card's sectors at its offset */
LOCAL void test_parts( void )
{
	UB	name[8];
	UB	*a, *b;
	SZ	asize;
	ID	dd;
	INT	i;

	if ( dd_whole <= 0 || tbl == NULL || nparts <= 0 ) KT_SKIP("no SD card");

	a = (UB *)Kmalloc(BLK_SECTOR_SIZE);
	b = (UB *)Kmalloc(BLK_SECTOR_SIZE);
	KT_ASSERT(a != NULL && b != NULL);
	if ( a == NULL || b == NULL ) return;

	for ( i = 0; i < nparts; i++ ) {
		T_PARTITION	*pt = &tbl->part[i];
		UD		last = pt->nsect - 1;

		part_name(name, i);
		dd = tk_opn_dev(name, TD_READ);
		KT_ASSERT(dd > 0);
		if ( dd <= 0 ) continue;
		tm_printf((UB *)"  %s: %d+%d\n", name, (INT)pt->start, (INT)pt->nsect);

		KT_ASSERT_ER(tk_srea_dev(dd, 0, a, BLK_SECTOR_SIZE, &asize), E_OK);
		KT_ASSERT_ER(tk_srea_dev(dd_whole, (W)pt->start, b, BLK_SECTOR_SIZE, &asize), E_OK);
		KT_ASSERT(same(a, b, BLK_SECTOR_SIZE));

		KT_ASSERT_ER(tk_srea_dev(dd, (W)last, a, BLK_SECTOR_SIZE, &asize), E_OK);
		KT_ASSERT_ER(tk_srea_dev(dd_whole, (W)(pt->start + last), b, BLK_SECTOR_SIZE, &asize), E_OK);
		KT_ASSERT(same(a, b, BLK_SECTOR_SIZE));

		/* one past the end is refused */
		KT_ASSERT_ER(tk_srea_dev(dd, (W)pt->nsect, a, BLK_SECTOR_SIZE, &asize), E_PAR);
		tk_cls_dev(dd, 0);
	}
	Kfree(a);
	Kfree(b);
}

/*
 * One pass: through 'mode', write 'n' sectors from 'buf' at 'base' of
 * the scratch partition, with a guard sector written on either side
 * first. Then read the whole run back twice, through the data port and
 * the whole card, and through 'mode' and the partition, and check it all.
 */
LOCAL void one_pass( ID dd, UD pstart, INT mode, UB *buf, UD base, UD n, UW tag )
{
	SZ	asize;
	UD	got;

	/* the guards and the data */
	KT_ASSERT(knl_sd_dma_mode(mode) >= E_OK);
	fill(buf, tag + 100, base - 1, 1);
	KT_ASSERT_ER(tk_swri_dev(dd, (W)(base - 1), buf, BLK_SECTOR_SIZE, &asize), E_OK);
	fill(buf, tag + 100, base + n, 1);
	KT_ASSERT_ER(tk_swri_dev(dd, (W)(base + n), buf, BLK_SECTOR_SIZE, &asize), E_OK);
	fill(buf, tag, base, n);
	KT_ASSERT_ER(tk_swri_dev(dd, (W)base, buf, (SZ)(n * BLK_SECTOR_SIZE), &asize), E_OK);
	KT_ASSERT_EQ(asize, (SZ)(n * BLK_SECTOR_SIZE));

	/* through the data port and the whole card */
	wipe(buf, (SZ)((n + 2) * BLK_SECTOR_SIZE));
	KT_ASSERT(knl_sd_dma_mode(0) >= E_OK);
	KT_ASSERT_ER(tk_srea_dev(dd_whole, (W)(pstart + base - 1), buf,
				 (SZ)((n + 2) * BLK_SECTOR_SIZE), &asize), E_OK);
	KT_ASSERT_EQ(check(buf, tag + 100, base - 1, 1), 1);
	got = check(buf + BLK_SECTOR_SIZE, tag, base, n);
	KT_ASSERT_EQ(got, n);
	KT_ASSERT_EQ(check(buf + (n + 1) * BLK_SECTOR_SIZE, tag + 100, base + n, 1), 1);

	/* through the way under test and the partition */
	wipe(buf, (SZ)(n * BLK_SECTOR_SIZE));
	KT_ASSERT(knl_sd_dma_mode(mode) >= E_OK);
	KT_ASSERT_ER(tk_srea_dev(dd, (W)base, buf, (SZ)(n * BLK_SECTOR_SIZE), &asize), E_OK);
	KT_ASSERT_EQ(check(buf, tag, base, n), n);
}

/* every way of moving data, in memory below and above 4GB, aligned and not */
LOCAL void test_modes( void )
{
	UB	name[8];
	PFRAME	*pf[2];
	UB	*mem[2];
	T_PARTITION *pt = NULL;
	INT	scratch = -1, mode, old, z, i, k;
	UW	tag = 1;
	UD	base;
	ID	dd;

	if ( dd_whole <= 0 || tbl == NULL || nparts <= 0 ) KT_SKIP("no SD card");
	for ( i = 0; i < nparts && scratch < 0; i++ ) {
		for ( k = 0; k < 16 && tbl->part[i].type_guid[k] == sd_scratch_type[k]; k++ ) ;
		if ( k == 16 && tbl->part[i].valid && i <= 9 ) {
			scratch = i;
		}
	}
	if ( scratch < 0 ) KT_SKIP("no scratch partition on the SD card (mksd.py --scratch-mb)");
	pt = &tbl->part[scratch];
	if ( pt->nsect < 4 * (MAX_SECT + 2) ) KT_SKIP("the scratch partition is too small");

	part_name(name, scratch);
	dd = tk_opn_dev(name, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;

	pf[0] = knl_alloc_pages(BUF_ORDER, ZONE_DMA32, KAF_DMA32);
	pf[1] = knl_alloc_pages(BUF_ORDER, ZONE_NORMAL, 0);	/* none on a machine with less than 4GB */
	KT_ASSERT(pf[0] != NULL);
	for ( z = 0; z < 2; z++ ) {
		mem[z] = ( pf[z] != NULL ) ? (UB *)PA2VA(knl_pf_to_pa(pf[z])) : NULL;
	}

	old = knl_sd_dma_mode(-1);
	for ( mode = 0; mode < 4; mode++ ) {
		if ( knl_sd_dma_mode(mode) < E_OK ) {
			tm_printf((UB *)"  %s: the controller does not offer it\n", mode_name[mode]);
			continue;
		}
		for ( z = 0; z < 2; z++ ) {
			if ( mem[z] == NULL ) continue;
			for ( i = 0; i < (INT)NSIZES; i++ ) {
				/* runs apart from one another, so a stray write shows */
				base = 1 + (UD)i * (MAX_SECT + 2);
				one_pass(dd, pt->start, mode, mem[z], base, sizes[i], tag++);
				/* a buffer off a cache line goes through the bounce buffer */
				one_pass(dd, pt->start, mode, mem[z] + 1, base, sizes[i], tag++);
			}
		}
		tm_printf((UB *)"  %s: %d sizes%s\n", mode_name[mode], (INT)NSIZES,
			  ( pf[1] != NULL && knl_pf_to_pa(pf[1]) >= 0x100000000ULL ) ? ", below and above 4GB" : "");
	}
	(void)knl_sd_dma_mode(old);

	for ( z = 0; z < 2; z++ ) {
		if ( pf[z] != NULL ) knl_free_pages(pf[z], BUF_ORDER);
	}
	tk_cls_dev(dd, 0);
}

/* the scratch partition, opened for update; its entry in *p_pt */
LOCAL ID open_scratch( T_PARTITION **p_pt )
{
	UB	name[8];
	INT	i, k;

	for ( i = 0; i < nparts && i <= 9; i++ ) {
		for ( k = 0; k < 16 && tbl->part[i].type_guid[k] == sd_scratch_type[k]; k++ ) ;
		if ( k == 16 && tbl->part[i].valid ) {
			*p_pt = &tbl->part[i];
			part_name(name, i);
			return tk_opn_dev(name, TD_UPDATE);
		}
	}
	return E_NOEXS;
}

/*
 * Multiple block transfers ended by CMD12, by CMD23 sent as a command of
 * its own and, where the controller has it, by Auto-CMD23 (not QEMU's
 * sdhci-pci, which lacks it): each written and read back through the
 * way of moving data in use and through the data port. QEMU's sd-card
 * takes CMD23 although its configuration register does not say so.
 */
LOCAL void test_cmd23( void )
{
	static CONST INT	ways[] = { SD_CMD23_NONE, SD_CMD23_MANUAL, SD_CMD23_AUTO };
	static CONST char	*way_name[] = { "CMD12", "CMD23", "Auto-CMD23" };
	T_PARTITION	*pt = NULL;
	PFRAME		*pf;
	UB		*buf;
	INT		mode, w, i;
	UW		tag = 500;
	UD		base;
	ID		dd;

	if ( dd_whole <= 0 || tbl == NULL || nparts <= 0 ) KT_SKIP("no SD card");
#if defined(KT_SD_EMMC)
	if ( KT_SD_EMMC ) KT_SKIP("an eMMC ends its transfers with CMD12");
#endif
	dd = open_scratch(&pt);
	if ( dd <= 0 ) KT_SKIP("no scratch partition on the SD card (mksd.py --scratch-mb)");
	if ( pt->nsect < 4 * (MAX_SECT + 2) ) { tk_cls_dev(dd, 0); KT_SKIP("the scratch partition is too small"); }
	pf = knl_alloc_pages(BUF_ORDER, ZONE_DMA32, KAF_DMA32);
	KT_ASSERT(pf != NULL);
	if ( pf == NULL ) { tk_cls_dev(dd, 0); return; }
	buf = (UB *)PA2VA(knl_pf_to_pa(pf));
	mode = knl_sd_dma_mode(-1);

	for ( w = 0; w < 3; w++ ) {
#if defined(QEMU_VIRT) || defined(X64)
		if ( ways[w] == SD_CMD23_AUTO ) continue;
#endif
		if ( knl_sd_cmd23(ways[w]) < E_OK ) {
			tm_printf((UB *)"  %s: the controller does not offer it\n", way_name[w]);
			continue;
		}
		for ( i = 1; i < (INT)NSIZES; i++ ) {
			base = 1 + (UD)i * (MAX_SECT + 2);
			one_pass(dd, pt->start, mode, buf, base, sizes[i], tag++);
		}
		KT_ASSERT(knl_sd_cmd23(-1) == ways[w] || ( ways[w] == SD_CMD23_AUTO && mode == 1 ));
		tm_printf((UB *)"  %s: %d sizes through %s\n", way_name[w], (INT)NSIZES - 1, mode_name[mode & 3]);
	}
	(void)knl_sd_cmd23(SD_CMD23_DECIDE);
	(void)knl_sd_dma_mode(mode);
	knl_free_pages(pf, BUF_ORDER);
	tk_cls_dev(dd, 0);
}

/*
 * The ways back from a switch to 1.8V that fails, each ending with a
 * card that reads the same 32KB from its start as before:
 *
 *	QEMU's sd-card cannot switch: it does not answer S18A, and has no
 *	CMD11. Offered UHS-I, it stays at 3.3V in the High Speed mode;
 *	taken to have accepted 1.8V, CMD11 goes unanswered, its power is
 *	cycled and it comes up at 3.3V; taken to have switched without
 *	CMD11, it reaches SDR25 (QEMU's card has nothing faster), and with
 *	the check after the switch spoiled, its power is cycled and it
 *	comes up at 3.3V again.
 *
 *	On the Raspberry Pi 5 the card really switches: with the check
 *	spoiled, back to 3.3V; then as at start-up again.
 */
LOCAL void test_uhs_back( void )
{
	PFRAME	*pf;
	UB	*ref, *now;
	SZ	asz;
	INT	sp, first, k;
#ifdef QEMU_VIRT
	static CONST UW	how[] = { SD_TEST_UHS, SD_TEST_UHS | SD_TEST_S18A,
				  SD_TEST_UHS | SD_TEST_S18A | SD_TEST_NO_CMD11,
				  SD_TEST_UHS | SD_TEST_S18A | SD_TEST_NO_CMD11 | SD_TEST_BAD_READ, 0 };
	static CONST INT	want[] = { SD_SPEED_HS, SD_SPEED_HS, SD_SPEED_SDR25, SD_SPEED_HS, SD_SPEED_HS };
#else
	static CONST UW	how[] = { SD_TEST_BAD_READ, 0 };
#endif
	INT	nhow = (INT)( sizeof(how) / sizeof(how[0]) );

	if ( dd_whole <= 0 ) KT_SKIP("no SD card");
#if defined(KT_SD_EMMC)
	if ( KT_SD_EMMC ) KT_SKIP("an eMMC has no UHS-I");
#endif
	if ( knl_sd_reinit(0) == E_NOEXS ) KT_SKIP("an eMMC has no UHS-I");
	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_DMA32);
	KT_ASSERT(pf != NULL);
	if ( pf == NULL ) return;
	ref = (UB *)PA2VA(knl_pf_to_pa(pf));
	now = ref + 32 * 1024;
	KT_ASSERT_ER(tk_srea_dev(dd_whole, 0, ref, 32 * 1024, &asz), E_OK);
	first = knl_sd_speed(NULL);

	for ( k = 0; k < nhow; k++ ) {
		UW	hz = 0;

		sp = knl_sd_reinit(how[k]);
		(void)knl_sd_speed(&hz);
		tm_printf((UB *)"  test %x: %s, %d kHz\n", how[k],
			  ( sp >= 0 && sp <= SD_SPEED_SDR104 ) ? speed_name[sp] : "failed", (INT)( hz / 1000 ));
#ifdef QEMU_VIRT
		KT_ASSERT_EQ(sp, want[k]);
#else
		/* spoiled: at 3.3V, whatever it was before; then as at start-up */
		KT_ASSERT(sp >= SD_SPEED_DEFAULT);
		if ( how[k] != 0 ) KT_ASSERT(sp <= SD_SPEED_HS);
		else KT_ASSERT_EQ(sp, first);
#endif
		wipe(now, 32 * 1024);
		KT_ASSERT_ER(tk_srea_dev(dd_whole, 0, now, 32 * 1024, &asz), E_OK);
		KT_ASSERT(same(ref, now, 32 * 1024));
	}
	KT_ASSERT_EQ(knl_sd_speed(NULL), first);
	knl_free_pages(pf, 4);
}

EXPORT void ktest_sd( void )
{
	dd_whole = 0;
	tbl = NULL;
	nparts = 0;
	KT_RUN(test_card);
	KT_RUN(test_speed);
	KT_RUN(test_read_size);
	KT_RUN(test_parts);
	KT_RUN(test_modes);
	KT_RUN(test_cmd23);
	KT_RUN(test_uhs_back);
	if ( dd_whole > 0 ) tk_cls_dev(dd_whole, 0);
	if ( tbl != NULL ) Kfree(tbl);
}
