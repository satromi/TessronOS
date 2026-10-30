/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_iob.c
 *	How fast a record is read and written, from the block device up
 *	through the native volume and the object calls, beside the same
 *	device read and written raw (design 7.3). Run only when named
 *	(make KTONLY=iob): it needs a disk of its own that it may wipe,
 *	plugged in beside the test disk (KT_IOB_DEV, the first one of the
 *	list that is there), and reports the times rather than judging
 *	them; what it asserts is that the bytes come back as written.
 *
 *	KT_IOB_BIG names a second disk, larger than 4GB, on which records
 *	and offsets past 2GB and 4GB are written and read back.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/blk.h>
#include <ts/ob.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/fs.h>
#include "../../peripheral_kernel/obj/obj.h"
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/conf.h>

#ifndef KT_IOB_DEV
#define KT_IOB_DEV	"nvmeb,sataa,vblkb,vblkc"
#endif
#ifndef KT_IOB_BIG
#define KT_IOB_BIG	"nvmec,satab,vblkc,vblkd"
#endif
#ifndef KT_IOB_FAT
#define KT_IOB_FAT	"nvmed,satac,vblkd"	/* a FAT32 volume larger than 4GB, no partition table */
#endif
#define FAT_AT		"/iobfat"
#ifndef KT_IOB_HUGE
#define KT_IOB_HUGE	"nvmee,satad"	/* a disk larger than 2TB (2^32 sectors), empty */
#endif
#ifndef KT_IOB_FUA
/* disks to ask for FUA, each wiped: none by default, as the USB disks are the other tests' (KT_IOB_CFLAGS names them) */
#define KT_IOB_FUA	"none"
#endif
/*
 * KT_IOB_PULL: the path, as QEMU sees it, of a disk image holding an
 * empty FAT32 volume of KT_IOB_PULL_SECT sectors, that test_fat_pull
 * plugs in as a USB disk through the QEMU monitor and pulls out
 */
#ifndef KT_IOB_PULL_SECT
#define KT_IOB_PULL_SECT	16777216
#endif
#define PULL_AT		"/iobpull"
#ifndef KT_IOB_MAXMB
#define KT_IOB_MAXMB	1024		/* the largest size measured */
#endif

#define MB		( 1024 * 1024 )
#define CHUNK_MAX	( 4 * MB )

LOCAL UB	*wbuf = NULL, *rbuf = NULL;
LOCAL char	iob_dev[16];
LOCAL char	big_dev[16];
LOCAL char	fat_dev[16];
LOCAL UD	fat_sect = 0;
LOCAL UD	iob_sect = 0, big_sect = 0;

LOCAL UD now_us( void )
{
	SYSTIM_U	t = 0;
	UW		ofs = 0;

	(void)tk_get_otm_u(&t, &ofs);
	return (UD)t;
}

/* KB/s of `bytes` in `us` */
LOCAL UD rate( UD bytes, UD us )
{
	return ( us == 0 ) ? 0 : (UD)( bytes * 1000000 / 1024 / us );
}

/* The words of a block that tell where it was written from */
LOCAL void fill( UB *b, SZ len, UD off )
{
	UD	*w = (UD *)b;
	SZ	i, n = len / 8;

	for ( i = 0; i < n; i++ ) {
		w[i] = ( off + (UD)i * 8 ) ^ 0x5A5AA5A5C3C33C3CULL;
	}
}

/* Whether what came back is what fill wrote for `off`: every 512th word */
LOCAL BOOL same( CONST UB *b, SZ len, UD off )
{
	CONST UD	*w = (CONST UD *)b;
	SZ		i, n = len / 8;

	for ( i = 0; i < n; i += 64 ) {
		if ( w[i] != ( ( off + (UD)i * 8 ) ^ 0x5A5AA5A5C3C33C3CULL ) ) return FALSE;
	}
	return ( n == 0 || w[n - 1] == ( ( off + (UD)( n - 1 ) * 8 ) ^ 0x5A5AA5A5C3C33C3CULL ) );
}

/* The first of the names split by ',' that is a disk, and its sectors */
LOCAL BOOL pick( CONST char *list, char *out, UD *p_sect )
{
	DiskInfo	di;
	SZ		asz = 0;
	ID		dd;
	INT		k;

	while ( *list != '\0' ) {
		for ( k = 0; list[k] != '\0' && list[k] != ',' && k < 15; k++ ) out[k] = list[k];
		out[k] = '\0';
		list += k;
		if ( *list == ',' ) list++;
		if ( knl_strcmp(out, KT_DISK) == 0 ) continue;
		dd = tk_opn_dev((UB *)out, TD_UPDATE);
		if ( dd <= 0 ) continue;
		knl_memset(&di, 0, sizeof(di));
		if ( tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK ) {
			UB	s1[512];

			/* never a disk with a partition table (GPT): that is the test disk or the system's */
			if ( tk_srea_dev_d(dd, 1, s1, 512, &asz) >= E_OK && s1[0] == 'E' && s1[1] == 'F'
			  && s1[2] == 'I' && s1[3] == ' ' && s1[4] == 'P' && s1[5] == 'A' ) {
				tk_cls_dev(dd, 0);
				continue;
			}
			*p_sect = (UD)di.blockcont_d;
			tk_cls_dev(dd, 0);
			return TRUE;
		}
		tk_cls_dev(dd, 0);
	}
	return FALSE;
}

LOCAL void cache_now( CONST char *dev, T_TSFSBLK_CACHE *c )
{
	ID	vol = ts_vol_blk_of(dev);

	knl_memset(c, 0, sizeof(*c));
	if ( vol > 0 ) (void)ts_cache_blk(vol, c);
}

LOCAL void say( CONST char *what, UD bytes, SZ chunk, UD us,
		CONST T_TSFSBLK_CACHE *a, CONST T_TSFSBLK_CACHE *b )
{
	tm_printf((UB *)"  IOB %s %dKB chunk %dKB: %d ms, %d KB/s",
		  what, (INT)( bytes / 1024 ), (INT)( chunk / 1024 ), (INT)( us / 1000 ),
		  (INT)rate(bytes, us));
	if ( a != NULL && b != NULL ) {
		tm_printf((UB *)" (dev reads %d writes %d flushes %d)",
			  (INT)( b->reads - a->reads ), (INT)( b->writes - a->writes ),
			  (INT)( b->flushes - a->flushes ));
	}
	tm_printf((UB *)"\n");
}

/* ---------------------------------------------------------------- the raw device */

LOCAL void raw_pass( UD bytes, BOOL wr )
{
	SZ	asz = 0;
	UD	off, us;
	BOOL	ok = TRUE;
	ER	er = E_OK;
	ID	dd = tk_opn_dev((UB *)iob_dev, TD_UPDATE);

	if ( dd <= 0 ) {
		KT_ASSERT(dd > 0);
		return;
	}
	us = now_us();
	for ( off = 0; off < bytes && er >= E_OK; off += MB ) {
		if ( wr ) {
			fill(wbuf, MB, off);
			er = tk_swri_dev(dd, (W)( off / 512 ), wbuf, MB, &asz);
		} else {
			er = tk_srea_dev(dd, (W)( off / 512 ), rbuf, MB, &asz);
			if ( er >= E_OK && !same(rbuf, MB, off) ) ok = FALSE;
		}
	}
	if ( wr && er >= E_OK ) er = tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asz);
	us = now_us() - us;
	tk_cls_dev(dd, 0);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT(ok);
	say(wr ? "raw write" : "raw read", bytes, MB, us, NULL, NULL);
}

LOCAL void test_raw( void )
{
	UD	sz;

	if ( iob_dev[0] == '\0' ) KT_SKIP("no disk of its own (KT_IOB_DEV)");
	for ( sz = MB; sz <= (UD)KT_IOB_MAXMB * MB && sz + 64 * MB <= iob_sect * 512; sz *= 100 ) {
		raw_pass(sz, TRUE);
		raw_pass(sz, FALSE);
		if ( sz == 100 * MB && (UD)KT_IOB_MAXMB >= 1024 ) sz = (UD)1024 * MB / 100;
	}
}

/* ---------------------------------------------------------------- records */

LOCAL TS_UUID	rec_uuid;
LOCAL BOOL	have_vol = FALSE;

LOCAL ID rec_open( CONST char *dev, TS_UUID *p_uuid, CONST char *name )
{
	T_OBCRE	c;
	INT	recno = -1;
	ID	key;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = dev;
	c.name = (CONST UB *)name;
	if ( ob_cre_obj(&c, p_uuid) < E_OK ) return E_OBJ;
	key = ob_opn_obj(p_uuid, OB_OP_ALL);
	if ( key > 0 && ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno) < E_OK ) {
		ob_cls_obj(key);
		return E_OBJ;
	}
	return key;
}

LOCAL void test_volume( void )
{
	if ( iob_dev[0] == '\0' ) KT_SKIP("no disk of its own (KT_IOB_DEV)");
	(void)ob_det_vol(iob_dev);
	KT_ASSERT_ER(ts_format_blk(iob_dev, "IOBENCH"), E_OK);
	KT_ASSERT_ER(ob_att_vol(iob_dev, TSFS_STORE_BLK), E_OK);
	have_vol = TRUE;
}

/* `bytes` written into record 0 of a new object `chunk` at a time, then read back the same way */
LOCAL void rec_pass( UD bytes, SZ chunk, BOOL keep )
{
	T_TSFSBLK_CACHE	a, b;
	TS_UUID		u;
	SZ		asz = 0, n;
	UD		off, us;
	BOOL		ok = TRUE;
	ER		er = E_OK;
	ID		key = rec_open(iob_dev, &u, "iob");

	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	cache_now(iob_dev, &a);
	us = now_us();
	for ( off = 0; off < bytes && er >= E_OK; off += (UD)n ) {
		n = ( bytes - off < (UD)chunk ) ? (SZ)( bytes - off ) : chunk;
		fill(wbuf, n, off);
		er = ob_wri_rec(key, 0, (D)off, wbuf, n, &asz);
	}
	us = now_us() - us;
	cache_now(iob_dev, &b);
	KT_ASSERT_ER(er, E_OK);
	say("rec write", bytes, chunk, us, &a, &b);
	ob_cls_obj(key);

	/* opened again: what the cache keeps of it is what was just written */
	key = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	cache_now(iob_dev, &a);
	us = now_us();
	for ( off = 0, er = E_OK; off < bytes && er >= E_OK; off += (UD)n ) {
		n = ( bytes - off < (UD)chunk ) ? (SZ)( bytes - off ) : chunk;
		er = ob_rea_rec(key, 0, (D)off, rbuf, n, &asz);
		if ( er >= E_OK && ( asz != n || !same(rbuf, n, off) ) ) ok = FALSE;
	}
	us = now_us() - us;
	cache_now(iob_dev, &b);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT(ok);
	say("rec read", bytes, chunk, us, &a, &b);
	ob_cls_obj(key);
	if ( keep ) {
		rec_uuid = u;
	} else {
		(void)ob_del_obj(&u);
	}
}

LOCAL void test_records( void )
{
	static CONST SZ	chunks[] = { 4096, 64 * 1024, MB, 4 * MB };
	UD		sz;
	INT		i;

	if ( !have_vol ) KT_SKIP("no volume");
	for ( sz = MB; sz <= (UD)KT_IOB_MAXMB * MB && sz + sz / 4 + 64 * MB <= iob_sect * 512; sz *= 100 ) {
		for ( i = 0; i < 4; i++ ) {
			/* block by block only where it ends in minutes */
			if ( chunks[i] < 64 * 1024 && sz > 16 * MB ) continue;
			rec_pass(sz, chunks[i], (BOOL)( sz == 100 * MB && chunks[i] == MB ));
		}
		if ( sz == 100 * MB && (UD)KT_IOB_MAXMB >= 1024 ) sz = (UD)1024 * MB / 100;
	}
}

/* A stream's writes: 1KB at a time, appended, as a C++ stream or a program's printf does */
LOCAL void test_small( void )
{
	T_TSFSBLK_CACHE	a, b;
	T_TSFSJRNL	j0, j1;
	TS_UUID		u;
	SZ		asz = 0;
	UD		off, us, bytes = MB;
	ER		er = E_OK;
	ID		key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = rec_open(iob_dev, &u, "iob small");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	knl_memset(&j0, 0, sizeof(j0));
	knl_memset(&j1, 0, sizeof(j1));
	(void)ts_jrnl_ref(ts_vol_blk_of(iob_dev), &j0);
	cache_now(iob_dev, &a);
	us = now_us();
	for ( off = 0; off < bytes && er >= E_OK; off += 1000 ) {
		fill(wbuf, 1000, off);
		er = ob_wri_rec(key, 0, (D)off, wbuf, ( bytes - off < 1000 ) ? (SZ)( bytes - off ) : 1000, &asz);
	}
	us = now_us() - us;
	cache_now(iob_dev, &b);
	(void)ts_jrnl_ref(ts_vol_blk_of(iob_dev), &j1);
	KT_ASSERT_ER(er, E_OK);
	say("rec append", bytes, 1, us, &a, &b);
	tm_printf((UB *)"  IOB  transactions %d, ring writes %d\n",
		  (INT)( j1.closed - j0.closed ), (INT)( j1.batches - j0.batches ));
	ob_cls_obj(key);
	(void)ob_del_obj(&u);
}

/* The whole object copied, records and all */
LOCAL void test_copy( void )
{
	T_TSFSBLK_CACHE	a, b;
	T_OBREC		r[2];
	TS_UUID		cp;
	INT		cnt = 0;
	UD		us, bytes = 0;
	ID		key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = ob_opn_obj(&rec_uuid, OB_OP_READ | OB_OP_ATRRD);
	if ( key <= 0 ) KT_SKIP("no 100MB record kept");
	if ( ob_lst_rec(key, r, 2, &cnt) >= E_OK && cnt > 0 ) bytes = r[0].size;
	ob_cls_obj(key);
	cache_now(iob_dev, &a);
	us = now_us();
	KT_ASSERT_ER(ob_cpy_obj(&rec_uuid, NULL, &cp), E_OK);
	us = now_us() - us;
	cache_now(iob_dev, &b);
	say("object copy", bytes, 0, us, &a, &b);
	(void)ob_del_obj(&cp);
}

/* A record read whole by the kernel's own reader */
LOCAL void test_whole( void )
{
	UB	*buf = NULL;
	INT	cap = 0, len;
	UD	us;
	ID	key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = ob_opn_obj(&rec_uuid, OB_OP_READ);
	if ( key <= 0 ) KT_SKIP("no 100MB record kept");
	us = now_us();
	len = knl_ob_rea_all(key, 0, 0, &buf, &cap);
	us = now_us() - us;
	tm_printf((UB *)"  IOB knl_ob_rea_all of the 100MB record: %d (%d ms)\n", len, (INT)( us / 1000 ));
	/* a key that may only read the bytes: the size asked under that right, the record read once */
	KT_ASSERT_EQ(len, 100 * MB);
	if ( buf != NULL && len == 100 * MB ) KT_ASSERT(same(buf, 100 * MB, 0));
	if ( buf != NULL ) Kfree(buf);
	ob_cls_obj(key);
	(void)ob_del_obj(&rec_uuid);
}

/* A record lengthened by a truncation: the blocks added read as zeros */
LOCAL void test_lengthen( void )
{
	T_TSFSBLK_CACHE	a, b;
	TS_UUID		u;
	SZ		asz = 0;
	UD		us, to = 256 * MB;
	ID		key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = rec_open(iob_dev, &u, "iob long");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	fill(wbuf, 1000, 0);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, wbuf, 1000, &asz), E_OK);
	cache_now(iob_dev, &a);
	us = now_us();
	KT_ASSERT_ER(ob_trn_rec(key, 0, to), E_OK);
	us = now_us() - us;
	cache_now(iob_dev, &b);
	say("rec lengthen", to, 0, us, &a, &b);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, rbuf, MB, &asz), E_OK);
	KT_ASSERT(asz == MB && same(rbuf, 1000, 0) && ((UD *)rbuf)[200] == 0 && ((UD *)rbuf)[MB / 8 - 1] == 0);
	KT_ASSERT_ER(ob_rea_rec(key, 0, (D)( to - 4096 ), rbuf, MB, &asz), E_OK);
	KT_ASSERT(asz == 4096 && ((UD *)rbuf)[0] == 0 && ((UD *)rbuf)[511] == 0);
	ob_cls_obj(key);
	(void)ob_del_obj(&u);
}

/*
 * What reading keeps (the runs of a record, the bytes read ahead) never
 * outlives a change: read in small pieces in order, written over, cut,
 * lengthened, replaced, and read again each time.
 */
LOCAL void test_kept( void )
{
	TS_UUID	u;
	SZ	asz = 0;
	UD	off;
	BOOL	ok = TRUE;
	ID	key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = rec_open(iob_dev, &u, "iob kept");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	fill(wbuf, 2 * MB, 0);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, wbuf, 2 * MB, &asz), E_OK);
	for ( off = 0; off < 2 * MB; off += 65536 ) {
		if ( ob_rea_rec(key, 0, (D)off, rbuf, 65536, &asz) < E_OK || asz != 65536
		  || !same(rbuf, 65536, off) ) ok = FALSE;
	}
	KT_ASSERT(ok);

	/* written over in the middle, while the reading is kept and read ahead: the next read sees it */
	fill(wbuf, 65536, 0x5000000);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 65536 * 3, wbuf, 65536, &asz), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 65536 * 3, rbuf, 65536, &asz), E_OK);
	KT_ASSERT(asz == 65536 && same(rbuf, 65536, 0x5000000));
	KT_ASSERT_ER(ob_rea_rec(key, 0, 65536 * 2, rbuf, 65536, &asz), E_OK);
	KT_ASSERT(asz == 65536 && same(rbuf, 65536, 65536 * 2));

	/* cut: past the end there is nothing */
	KT_ASSERT_ER(ob_trn_rec(key, 0, 100000), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 65536, rbuf, 65536, &asz), E_OK);
	KT_ASSERT_EQ(asz, 100000 - 65536);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 200000, rbuf, 65536, &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);

	/* lengthened: zeros where it was cut */
	KT_ASSERT_ER(ob_trn_rec(key, 0, MB), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 200000, rbuf, 65536, &asz), E_OK);
	KT_ASSERT(asz == 65536 && ((UD *)rbuf)[0] == 0 && ((UD *)rbuf)[8191] == 0);

	/* replaced: the new bytes only */
	fill(wbuf, 3 * 65536, 0x7000000);
	KT_ASSERT_ER(ob_rpl_rec(key, 0, wbuf, 3 * 65536), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 65536, rbuf, 65536, &asz), E_OK);
	KT_ASSERT(asz == 65536 && same(rbuf, 65536, 0x7000000 + 65536));
	KT_ASSERT_ER(ob_rea_rec(key, 0, 65536 * 3, rbuf, 65536, &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);
	ob_cls_obj(key);
	(void)ob_del_obj(&u);
}

/*
 * When what is written is on the medium (design 7.3): a replacement and
 * the closing of a key that wrote are; writes between may wait for the
 * next batch of the log, and many of them go in one.
 */
LOCAL void test_durable( void )
{
	T_TSFSJRNL	j0, j1;
	TS_UUID		u;
	SZ		asz = 0;
	UD		us;
	INT		i;
	ID		key, vol = ts_vol_blk_of(iob_dev);

	if ( !have_vol || vol <= 0 ) KT_SKIP("no volume");
	key = rec_open(iob_dev, &u, "iob durable");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;

	/* a replacement is on the medium when it returns */
	fill(wbuf, 100000, 1);
	KT_ASSERT_ER(ob_rpl_rec(key, 0, wbuf, 100000), E_OK);
	knl_memset(&j0, 0, sizeof(j0));
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j0), E_OK);
	KT_ASSERT(j0.durable >= j0.seq);

	/* 1000 small writes, then the key closed: on the medium then */
	us = now_us();
	for ( i = 0; i < 1000; i++ ) {
		fill(wbuf, 1000, (UD)i * 1000);
		KT_ASSERT_ER(ob_wri_rec(key, 0, (D)i * 1000, wbuf, 1000, &asz), E_OK);
	}
	knl_memset(&j1, 0, sizeof(j1));
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j1), E_OK);
	tm_printf((UB *)"  IOB 1000 writes of 1000 bytes: %d ms, %d transactions, %d batches, %d not yet on the medium\n",
		  (INT)( ( now_us() - us ) / 1000 ), (INT)( j1.closed - j0.closed ),
		  (INT)( j1.batches - j0.batches ), (INT)( j1.seq - j1.durable ));
	KT_ASSERT_ER(ob_cls_obj(key), E_OK);
	KT_ASSERT_ER(ts_jrnl_ref(vol, &j1), E_OK);
	KT_ASSERT(j1.durable >= j1.seq);
	(void)ob_del_obj(&u);
}

/* A record replaced whole: shorter, then longer, the old bytes gone each time */
LOCAL void test_replace( void )
{
	T_OBREC	r[2];
	TS_UUID	u;
	SZ	asz = 0;
	INT	cnt = 0;
	ID	key;

	if ( !have_vol ) KT_SKIP("no volume");
	key = rec_open(iob_dev, &u, "iob rpl");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	fill(wbuf, MB, 0);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, wbuf, MB, &asz), E_OK);
	fill(wbuf, 300000, 7777);
	KT_ASSERT_ER(ob_rpl_rec(key, 0, wbuf, 300000), E_OK);
	KT_ASSERT_ER(ob_lst_rec(key, r, 2, &cnt), E_OK);
	KT_ASSERT(cnt == 1 && r[0].size == 300000);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, rbuf, MB, &asz), E_OK);
	KT_ASSERT(asz == 300000 && same(rbuf, 296000, 7777));
	fill(wbuf, 3 * MB, 99);
	KT_ASSERT_ER(ob_rpl_rec(key, 0, wbuf, 3 * MB), E_OK);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, rbuf, 4 * MB, &asz), E_OK);
	KT_ASSERT(asz == 3 * MB && same(rbuf, 3 * MB, 99));
	KT_ASSERT_ER(ob_rpl_rec(key, 0, NULL, 0), E_OK);
	KT_ASSERT_ER(ob_lst_rec(key, r, 2, &cnt), E_OK);
	KT_ASSERT(cnt == 1 && r[0].size == 0);
	ob_cls_obj(key);
	(void)ob_del_obj(&u);
}

/* A file of the object world written past its end, as a program's lseek and write do */
LOCAL void test_gap( void )
{
	T_FSTAT	st;
	UB	c = 'x';
	UD	us, gap = 4 * MB;
	INT	fd, n;

	fd = fs_open("/tmp/iobgap", O_RDWR | O_CREAT | O_TRUNC);
	if ( fd < 0 ) KT_SKIP("no /tmp");
	KT_ASSERT(fs_lseek(fd, (D)gap, SEEK_SET_) == (D)gap);
	us = now_us();
	n = fs_write(fd, &c, 1);
	us = now_us() - us;
	KT_ASSERT_EQ(n, 1);
	tm_printf((UB *)"  IOB write 1 byte %d KB past the end of a file: %d ms\n",
		  (INT)( gap / 1024 ), (INT)( us / 1000 ));
	KT_ASSERT(fs_lseek(fd, 1000, SEEK_SET_) == 1000);
	n = fs_read(fd, rbuf, 4096);
	KT_ASSERT(n == 4096 && rbuf[0] == 0 && rbuf[4095] == 0);
	fs_close(fd);
	KT_ASSERT(fs_stat("/tmp/iobgap", &st) >= EX_OK && st.size == gap + 1);
	(void)fs_unlink("/tmp/iobgap");
}

/* ---------------------------------------------------------------- past 2GB and 4GB */

/* Where 1MB is written and read back: on either side of 2^31 and 2^32 */
LOCAL CONST UD big_at[] = {
	0, 0x7FF80000ULL, 0x80000000ULL, 0xFFF80000ULL, 0x100000000ULL, 0x140000000ULL
};

LOCAL void test_big( void )
{
	T_OBREC	r[2];
	TS_UUID	u, cp;
	SZ	asz = 0;
	INT	i, cnt = 0;
	UD	us, end = 0x140000000ULL + MB;
	ID	key;

	if ( big_dev[0] == '\0' ) KT_SKIP("no disk past 4GB (KT_IOB_BIG)");
	(void)ob_det_vol(big_dev);
	KT_ASSERT_ER(ts_format_blk(big_dev, "IOBIG"), E_OK);
	KT_ASSERT_ER(ob_att_vol(big_dev, TSFS_STORE_BLK), E_OK);
	key = rec_open(big_dev, &u, "iob big");
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;

	us = now_us();
	for ( i = 0; i < (INT)( sizeof(big_at) / sizeof(big_at[0]) ); i++ ) {
		fill(wbuf, MB, big_at[i]);
		KT_ASSERT_ER(ob_wri_rec(key, 0, (D)big_at[i], wbuf, MB, &asz), E_OK);
		KT_ASSERT_EQ(asz, MB);
	}
	us = now_us() - us;
	tm_printf((UB *)"  IOB writes up to %d MB: %d ms\n", (INT)( end / MB ), (INT)( us / 1000 ));
	KT_ASSERT_ER(ob_lst_rec(key, r, 2, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT(r[0].size == end);
	for ( i = 0; i < (INT)( sizeof(big_at) / sizeof(big_at[0]) ); i++ ) {
		asz = 0;
		KT_ASSERT_ER(ob_rea_rec(key, 0, (D)big_at[i], rbuf, MB, &asz), E_OK);
		KT_ASSERT_EQ(asz, MB);
		KT_ASSERT(same(rbuf, MB, big_at[i]));
	}
	/* what lies between is zeros */
	KT_ASSERT_ER(ob_rea_rec(key, 0, (D)0xC0000000ULL, rbuf, 4096, &asz), E_OK);
	KT_ASSERT(((UD *)rbuf)[0] == 0 && ((UD *)rbuf)[511] == 0);
	/* the last bytes, and past the end nothing */
	KT_ASSERT_ER(ob_rea_rec(key, 0, (D)( end - 16 ), rbuf, MB, &asz), E_OK);
	KT_ASSERT_EQ(asz, 16);
	KT_ASSERT_ER(ob_rea_rec(key, 0, (D)end, rbuf, MB, &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);

	/* cut back past 4GB and below 2GB */
	KT_ASSERT_ER(ob_trn_rec(key, 0, 0x100000000ULL + 4096), E_OK);
	KT_ASSERT_ER(ob_lst_rec(key, r, 2, &cnt), E_OK);
	KT_ASSERT(r[0].size == 0x100000000ULL + 4096);
	KT_ASSERT_ER(ob_rea_rec(key, 0, (D)0x100000000ULL, rbuf, MB, &asz), E_OK);
	KT_ASSERT_EQ(asz, 4096);
	KT_ASSERT(same(rbuf, 4096, 0x100000000ULL));
	ob_cls_obj(key);

	/* the object copied whole: 4GB and more of it */
	us = now_us();
	KT_ASSERT_ER(ob_cpy_obj(&u, NULL, &cp), E_OK);
	us = now_us() - us;
	tm_printf((UB *)"  IOB copy of the 4GB record: %d ms\n", (INT)( us / 1000 ));
	key = ob_opn_obj(&cp, OB_OP_READ | OB_OP_ATRRD);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_lst_rec(key, r, 2, &cnt), E_OK);
		KT_ASSERT(cnt == 1 && r[0].size == 0x100000000ULL + 4096);
		KT_ASSERT_ER(ob_rea_rec(key, 0, (D)0xFFF80000ULL, rbuf, MB, &asz), E_OK);
		KT_ASSERT(asz == 512 * 1024 + 4096 && same(rbuf, 512 * 1024 + 4096, 0xFFF80000ULL));
		ob_cls_obj(key);
	}
	(void)ob_del_obj(&cp);
	(void)ob_del_obj(&u);
	(void)ob_det_vol(big_dev);
}

/* ---------------------------------------------------------------- FAT */

/* `bytes` written to a new file `chunk` at a time, then read back the same way */
LOCAL void fat_pass( UD bytes, SZ chunk )
{
	UD	off, us;
	INT	fd, n = 0;
	BOOL	ok = TRUE;

	fd = fs_open(FAT_AT "/IOB.BIN", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	us = now_us();
	for ( off = 0; off < bytes; off += (UD)chunk ) {
		fill(wbuf, chunk, off);
		n = fs_write(fd, wbuf, chunk);
		if ( n != (INT)chunk ) { ok = FALSE; break; }
	}
	fs_close(fd);
	(void)fs_sync();
	us = now_us() - us;
	KT_ASSERT(ok);
	say("fat write", bytes, chunk, us, NULL, NULL);
	fd = fs_open(FAT_AT "/IOB.BIN", O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	us = now_us();
	for ( off = 0; off < bytes; off += (UD)chunk ) {
		n = fs_read(fd, rbuf, chunk);
		if ( n != (INT)chunk || !same(rbuf, chunk, off) ) { ok = FALSE; break; }
	}
	us = now_us() - us;
	fs_close(fd);
	KT_ASSERT(ok);
	say("fat read", bytes, chunk, us, NULL, NULL);
	(void)fs_unlink(FAT_AT "/IOB.BIN");
}

LOCAL void test_fat( void )
{
	if ( fat_dev[0] == '\0' ) KT_SKIP("no FAT disk (KT_IOB_FAT)");
	(void)fs_detach(FAT_AT);
	KT_ASSERT_ER(fs_attach("fatfs", fat_dev, FAT_AT, 0), EX_OK);
	fat_pass(MB, 4096);
	fat_pass(MB, 65536);
	fat_pass(100 * MB, 65536);
	fat_pass(100 * MB, MB);
	fat_pass(100 * MB, 4 * MB);
}

/*
 * What a write leaves in the cache of a FAT volume is on the disk some
 * seconds later without the file being closed (the flusher, FAT_FLUSH_MS):
 * a stick taken out then reads whole elsewhere. The disk itself is read
 * past the cache: the entry of the file in the root has its size.
 */
LOCAL BOOL raw_entry( CONST char *dev, CONST char *name83, UD *p_size )
{
	UB	*s = rbuf;
	SZ	asz = 0;
	UD	rsv, nfat, fatsz, spc, data, k;
	INT	j;
	BOOL	found = FALSE;
	ID	dd = tk_opn_dev((UB *)dev, TD_READ);

	if ( dd <= 0 ) return FALSE;
	if ( tk_srea_dev_d(dd, 0, s, 512, &asz) >= E_OK ) {
		rsv = (UD)( s[14] | ( s[15] << 8 ) );
		nfat = s[16];
		spc = s[13];
		fatsz = (UD)s[36] | ( (UD)s[37] << 8 ) | ( (UD)s[38] << 16 ) | ( (UD)s[39] << 24 );
		data = rsv + nfat * fatsz;			/* cluster 2, the root */
		for ( k = 0; k < spc && !found; k++ ) {
			if ( tk_srea_dev_d(dd, (D)( data + k ), s, 512, &asz) < E_OK ) break;
			for ( j = 0; j < 512 && !found; j += 32 ) {
				INT	c;

				for ( c = 0; c < 11 && s[j + c] == (UB)name83[c]; c++ ) ;
				if ( c == 11 ) {
					*p_size = (UD)s[j + 28] | ( (UD)s[j + 29] << 8 ) | ( (UD)s[j + 30] << 16 )
						| ( (UD)s[j + 31] << 24 );
					found = TRUE;
				}
			}
		}
	}
	tk_cls_dev(dd, 0);
	return found;
}

LOCAL void test_fat_flush( void )
{
	UD	size = 0;
	INT	fd;

	if ( fat_dev[0] == '\0' ) KT_SKIP("no FAT disk (KT_IOB_FAT)");
	(void)fs_detach(FAT_AT);
	KT_ASSERT_ER(fs_attach("fatfs", fat_dev, FAT_AT, 0), EX_OK);
	fd = fs_open(FAT_AT "/FLUSH.TXT", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	fill(wbuf, 10000, 0);
	KT_ASSERT_EQ(fs_write(fd, wbuf, 10000), 10000);
	tk_dly_tsk(7000);				/* more than FAT_FLUSH_MS, the file still open */
	KT_ASSERT(raw_entry(fat_dev, "FLUSH   TXT", &size));
	KT_ASSERT(size == 10000);
	tm_printf((UB *)"  IOB the entry on the disk 7 s after the write, the file open: %d bytes\n", (INT)size);
	fs_close(fd);
	(void)fs_unlink(FAT_AT "/FLUSH.TXT");
	KT_ASSERT_ER(fs_detach(FAT_AT), EX_OK);
	KT_ASSERT_ER(fs_attach("fatfs", fat_dev, FAT_AT, 0), EX_OK);	/* for the tests after */
}

#ifdef KT_IOB_PULL
/* The USB disk of KT_IOB_PULL_SECT sectors, or "" when there is none */
LOCAL BOOL pull_find( char *out )
{
	T_LDEV		ld[64];
	DiskInfo	di;
	SZ		asz;
	INT		n, i;
	ID		dd;

	n = tk_lst_dev(ld, 0, 64);
	for ( i = 0; i < n && i < 64; i++ ) {
		if ( ld[i].devnm[0] != 'u' || ld[i].devnm[1] != 'd' ) continue;
		dd = tk_opn_dev(ld[i].devnm, TD_READ);
		if ( dd <= 0 ) continue;
		knl_memset(&di, 0, sizeof(di));
		if ( tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK
		  && di.blockcont_d == (D)KT_IOB_PULL_SECT ) {
			tk_cls_dev(dd, 0);
			knl_strcpy(out, (CONST char *)ld[i].devnm);
			return TRUE;
		}
		tk_cls_dev(dd, 0);
	}
	return FALSE;
}

LOCAL BOOL pull_plug( char *dev )
{
	INT	t;

	tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
		  "\"drive_add 0 if=none,id=iobpd,file=%s,format=qcow2\"}}\n", KT_IOB_PULL);
	tk_dly_tsk(300);
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-storage\","
		  "\"id\":\"iobpu\",\"bus\":\"xhci.0\",\"port\":\"4.6\",\"drive\":\"iobpd\",\"removable\":true}}\n");
	for ( t = 0; t < 100; t++ ) {
		tk_dly_tsk(100);
		if ( pull_find(dev) ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL pull_out( void )
{
	char	dev[16];
	INT	t;

	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"iobpu\"}}\n");
	tk_dly_tsk(300);
	tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
		  "\"drive_del iobpd\"}}\n");
	for ( t = 0; t < 100; t++ ) {
		if ( !pull_find(dev) ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/*
 * A USB disk pulled out while a file on its FAT volume is open, more
 * than FAT_FLUSH_MS after the last write: plugged in again, the file
 * has the size and the bytes written. The entry and the table were
 * kept in the cache and written by the flusher, not by a close.
 */
LOCAL void test_fat_pull( void )
{
	char	dev[16];
	INT	fd, n;

	if ( !pull_plug(dev) ) KT_SKIP("no USB disk plugged in (QEMU monitor, KT_IOB_PULL)");
	tm_printf((UB *)"  IOB pull disk %s\n", dev);
	KT_ASSERT_ER(fs_attach("fatfs", dev, PULL_AT, 0), EX_OK);
	fd = fs_open(PULL_AT "/PULL.BIN", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		fill(wbuf, 300000, 0);
		KT_ASSERT_EQ(fs_write(fd, wbuf, 300000), 300000);
		tk_dly_tsk(7000);			/* more than FAT_FLUSH_MS, the file still open */
	}
	KT_ASSERT(pull_out());
	if ( fd >= 0 ) (void)fs_close(fd);	/* the disk is gone: what the close answers does not matter */
	(void)fs_detach(PULL_AT);

	KT_ASSERT(pull_plug(dev));
	KT_ASSERT_ER(fs_attach("fatfs", dev, PULL_AT, 0), EX_OK);
	fd = fs_open(PULL_AT "/PULL.BIN", O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		knl_memset(rbuf, 0, 300000);
		n = fs_read(fd, rbuf, 400000);
		tm_printf((UB *)"  IOB after the pull: %d bytes\n", n);
		KT_ASSERT_EQ(n, 300000);
		KT_ASSERT(same(rbuf, 300000, 0));
		fs_close(fd);
	}
	(void)fs_unlink(PULL_AT "/PULL.BIN");
	KT_ASSERT_ER(fs_detach(PULL_AT), EX_OK);
	KT_ASSERT(pull_out());
}
#endif

/*
 * A FAT file stops at 4GB-1: a write that crosses it writes what fits,
 * a write and a truncation past it are refused, and the volume is whole
 * afterwards. A truncation that lengthens a file grows its chain, with
 * zeros, also where a shorter cut left bytes behind in the last cluster.
 */
LOCAL void test_fat_limits( void )
{
	T_FSTAT	st;
	UD	edge = 0xFFFFFFFFULL;
	INT	fd, n;

	if ( fat_dev[0] == '\0' ) KT_SKIP("no FAT disk (KT_IOB_FAT)");
	if ( fat_sect * 512 < 0x120000000ULL ) KT_SKIP("the FAT disk is smaller than 4.5GB");

	/* lengthened by a truncation, after a cut that left bytes in the cluster */
	fd = fs_open(FAT_AT "/LONG.BIN", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	fill(wbuf, 65536, 0);
	KT_ASSERT_EQ(fs_write(fd, wbuf, 65536), 65536);
	KT_ASSERT_ER(fs_ftruncate(fd, 1000), EX_OK);
	KT_ASSERT_ER(fs_ftruncate(fd, 3 * MB), EX_OK);
	KT_ASSERT(fs_lseek(fd, 0, SEEK_SET_) == 0);
	n = fs_read(fd, rbuf, 4 * MB);
	KT_ASSERT_EQ(n, 3 * MB);
	KT_ASSERT(same(rbuf, 1000 - 1000 % 8, 0));
	KT_ASSERT(rbuf[1000] == 0 && rbuf[65535] == 0 && rbuf[3 * MB - 1] == 0);
	/* written past the end: what was between reads as zeros */
	KT_ASSERT_ER(fs_ftruncate(fd, 1000), EX_OK);
	KT_ASSERT(fs_lseek(fd, 20000, SEEK_SET_) == 20000);
	KT_ASSERT_EQ(fs_write(fd, wbuf, 10), 10);
	KT_ASSERT(fs_lseek(fd, 1000, SEEK_SET_) == 1000);
	KT_ASSERT_EQ(fs_read(fd, rbuf, 19000), 19000);
	KT_ASSERT(rbuf[0] == 0 && rbuf[18999] == 0);
	fs_close(fd);
	(void)fs_unlink(FAT_AT "/LONG.BIN");

	/* the edge */
	fd = fs_open(FAT_AT "/EDGE.BIN", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	KT_ASSERT(fs_lseek(fd, (D)( edge - 4096 ), SEEK_SET_) == (D)( edge - 4096 ));
	fill(wbuf, 8192, 0);
	n = fs_write(fd, wbuf, 8192);
	KT_ASSERT_EQ(n, 4096);
	n = fs_write(fd, wbuf, 1);
	KT_ASSERT_EQ(n, EX_FBIG);
	KT_ASSERT_ER(fs_ftruncate(fd, edge + 1), EX_FBIG);
	KT_ASSERT(fs_fstat(fd, &st) >= EX_OK && st.size == edge);
	KT_ASSERT(fs_lseek(fd, (D)( edge - 4096 ), SEEK_SET_) == (D)( edge - 4096 ));
	n = fs_read(fd, rbuf, 8192);
	KT_ASSERT(n == 4096 && same(rbuf, 4096, 0));
	fs_close(fd);
	(void)fs_unlink(FAT_AT "/EDGE.BIN");
	KT_ASSERT_ER(fs_detach(FAT_AT), EX_OK);
}

/* ---------------------------------------------------------------- past 2^31 and 2^32 sectors */

/*
 * A disk of more than 1TB of 512-byte sectors (B1): its size whole in
 * blockcont_d and held at 2^31 - 1 in blockcont; sectors past 2^31 and
 * 2^32 written and read back with the 64-bit start; a native volume made
 * on the whole of it, which writes its groups' heads to its end, opened
 * and checked, and an object written on it.
 */
LOCAL void test_huge( void )
{
	static CONST UD	at[] = { 0x7FFFFFF0ULL, 0x80000000ULL, 0xFFFFFFF0ULL, 0x100000010ULL };
	char		dev[16];
	DiskInfo	di;
	TS_UUID		u;
	SZ		asz = 0;
	UD		sect = 0;
	INT		i;
	ID		dd, key;

	if ( !pick(KT_IOB_HUGE, dev, &sect) ) KT_SKIP("no disk past 2TB (KT_IOB_HUGE)");
	dd = tk_opn_dev((UB *)dev, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	knl_memset(&di, 0, sizeof(di));
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
	tm_printf((UB *)"  IOB huge disk %s: %ld sectors (blockcont %d)\n", dev, (D)di.blockcont_d, di.blockcont);
	KT_ASSERT(di.blockcont_d > 0x100000000LL);
	KT_ASSERT_EQ(di.blockcont, 0x7FFFFFFF);
	for ( i = 0; i < 4; i++ ) {
		fill(wbuf, 8192, at[i] * 512);
		KT_ASSERT_ER(tk_swri_dev_d(dd, (D)at[i], wbuf, 8192, &asz), E_OK);
	}
	for ( i = 0; i < 4; i++ ) {
		knl_memset(rbuf, 0, 8192);
		KT_ASSERT_ER(tk_srea_dev_d(dd, (D)at[i], rbuf, 8192, &asz), E_OK);
		KT_ASSERT(asz == 8192 && same(rbuf, 8192, at[i] * 512));
	}
	/* the last sector, and one past it */
	KT_ASSERT_ER(tk_srea_dev_d(dd, di.blockcont_d - 1, rbuf, 512, &asz), E_OK);
	KT_ASSERT(tk_srea_dev_d(dd, di.blockcont_d, rbuf, 512, &asz) < E_OK);
	tk_cls_dev(dd, 0);

	/* a volume on the whole of it */
	(void)ob_det_vol(dev);
	KT_ASSERT_ER(ts_format_blk(dev, "IOHUGE"), E_OK);
	KT_ASSERT_ER(ob_att_vol(dev, TSFS_STORE_BLK), E_OK);
	{
		T_TSFSBLK_SB	sb;

		KT_ASSERT_ER(ts_ref_vol_blk(ts_vol_blk_of(dev), &sb), E_OK);
		KT_ASSERT(sb.total_blocks == (UD)di.blockcont_d / 8);
		tm_printf((UB *)"  IOB huge volume: %ld blocks, %ld free, %d groups\n",
			  (D)sb.total_blocks, (D)sb.free_blocks, (INT)sb.ag_count);
	}
	key = rec_open(dev, &u, "iob huge");
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		fill(wbuf, MB, 0);
		KT_ASSERT_ER(ob_wri_rec(key, 0, 0, wbuf, MB, &asz), E_OK);
		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, rbuf, MB, &asz), E_OK);
		KT_ASSERT(asz == MB && same(rbuf, MB, 0));
		ob_cls_obj(key);
		(void)ob_del_obj(&u);
	}
	(void)ob_det_vol(dev);
}

/*
 * Writes through the device's cache (TDN_FUAWRITE) on each disk of
 * KT_IOB_FUA: a disk that says it can (ATA: WRITE DMA FUA EXT in its
 * IDENTIFY; SCSI: DPOFUA in its mode parameter header) writes the blocks
 * and they read back; one that cannot answers E_PAR and has written
 * nothing. Either way a native volume on it keeps a record over an
 * unmount, its journal written through FUA or written and flushed.
 */
LOCAL void test_fua( void )
{
	CONST char	*list = KT_IOB_FUA;
	char		dev[16];
	T_DEVFUA	fw;
	TS_UUID		u;
	SZ		asz;
	UD		sect;
	INT		k, seen = 0;
	ID		dd, key;
	ER		er;

	while ( *list != '\0' ) {
		char	one[16];

		for ( k = 0; list[k] != '\0' && list[k] != ',' && k < 15; k++ ) one[k] = list[k];
		one[k] = '\0';
		list += k;
		if ( *list == ',' ) list++;
		if ( knl_strcmp(one, iob_dev) == 0 || knl_strcmp(one, big_dev) == 0
		  || knl_strcmp(one, fat_dev) == 0 || !pick(one, dev, &sect) ) continue;
		seen++;
		dd = tk_opn_dev((UB *)dev, TD_UPDATE);
		KT_ASSERT(dd > 0);
		if ( dd <= 0 ) continue;

		/* the blocks first zeroed, so that a refused FUA write is seen to write nothing */
		knl_memset(wbuf, 0, 65536);
		KT_ASSERT_ER(tk_swri_dev_d(dd, 2048, wbuf, 65536, &asz), E_OK);
		fill(wbuf, 65536, 2048 * 512);
		fw.start = 2048;
		fw.buf = wbuf;
		fw.size = 65536;
		asz = 0;
		er = tk_swri_dev(dd, TDN_FUAWRITE, &fw, sizeof(fw), &asz);
		if ( er >= E_OK ) er = E_OK;
		tm_printf((UB *)"  IOB FUA %s: %s (%d)\n", dev, ( er == E_OK ) ? "written through" : "not taken", er);
		knl_memset(rbuf, 0xA5, 65536);
		KT_ASSERT_ER(tk_srea_dev_d(dd, 2048, rbuf, 65536, &asz), E_OK);
		if ( er == E_OK ) {
			KT_ASSERT(asz == 65536 && same(rbuf, 65536, 2048 * 512));
		} else {
			KT_ASSERT_ER(er, E_PAR);
			for ( k = 0; k < 65536 && rbuf[k] == 0; k++ ) ;
			KT_ASSERT_EQ(k, 65536);
		}
		/* a request that does not fit the disk is refused either way */
		fw.start = (D)sect;
		KT_ASSERT(tk_swri_dev(dd, TDN_FUAWRITE, &fw, sizeof(fw), &asz) < E_OK);
		tk_cls_dev(dd, 0);

		/* a native volume on it: the journal by FUA, or written and flushed */
		(void)ob_det_vol(dev);
		KT_ASSERT_ER(ts_format_blk(dev, "IOFUA"), E_OK);
		KT_ASSERT_ER(ob_att_vol(dev, TSFS_STORE_BLK), E_OK);
		key = rec_open(dev, &u, "iob fua");
		KT_ASSERT(key > 0);
		if ( key <= 0 ) {
			(void)ob_det_vol(dev);
			continue;
		}
		fill(wbuf, MB, 77);
		KT_ASSERT_ER(ob_wri_rec(key, 0, 0, wbuf, MB, &asz), E_OK);
		KT_ASSERT_ER(ob_cls_obj(key), E_OK);
		KT_ASSERT_ER(ob_det_vol(dev), E_OK);
		KT_ASSERT_ER(ob_att_vol(dev, TSFS_STORE_BLK), E_OK);
		key = ob_opn_obj(&u, OB_OP_ALL);
		KT_ASSERT(key > 0);
		if ( key > 0 ) {
			knl_memset(rbuf, 0, MB);
			KT_ASSERT_ER(ob_rea_rec(key, 0, 0, rbuf, MB, &asz), E_OK);
			KT_ASSERT(asz == MB && same(rbuf, MB, 77));
			ob_cls_obj(key);
			(void)ob_del_obj(&u);
		}
		(void)ob_det_vol(dev);
	}
	if ( seen == 0 ) KT_SKIP("no disk to ask (KT_IOB_FUA)");
}

/* The adapters' object (netadp.c): whether its record says SELECT `name` */
LOCAL BOOL adp_says( CONST char *name )
{
	TS_UUID	u;
	UB	t[512];
	SZ	asz = 0;
	ID	key;
	INT	k, n;

	if ( knl_obdev_uuid((CONST UB *)"netadp", &u) < E_OK ) return FALSE;
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key <= 0 ) return FALSE;
	knl_memset(t, 0, sizeof(t));
	(void)ob_rea_rec(key, 1, 0, t, sizeof(t) - 1, &asz);
	ob_cls_obj(key);
	if ( t[0] != 'S' || t[6] != ' ' ) return FALSE;
	for ( k = 0, n = 7; name[k] != '\0' && t[n] == (UB)name[k]; k++, n++ ) ;
	return ( name[k] == '\0' && t[n] == '\n' );
}

/* A line of the machine's settings replaced through a key already open */
LOCAL ER conf_line_put( ID key, CONST char *k, CONST char *v )
{
	UB	*text = NULL;
	INT	len, cap = 0;
	ER	er;

	len = knl_ob_rea_all(key, 1, 64, &text, &cap);
	if ( len < 0 ) return (ER)len;
	len = cf_put(text, len, cap, k, 0, v);
	er = ( len < 0 ) ? E_LIMIT : ob_rpl_rec(key, 1, text, (SZ)len);
	Kfree(text);
	return er;
}

LOCAL ID	held_key;

/* Another writer: the machine's settings held open to be written for 300 ms */
LOCAL void hold_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	tk_dly_tsk(300);
	ob_cls_obj(held_key);
	tk_exd_tsk();
}

/*
 * The machine's settings written while others read them (design 7.3,
 * 9.6): a key opened to be written with OB_O_WEXCL keeps no reader out,
 * so the network's task, which reads them every 2 seconds, takes a line
 * written through that key while the key is still open; a second writer
 * waits for the first to close, then writes; a key that keeps everything
 * out (OB_O_EXCL) makes the reader fail (E_BUSY), and that moment is not
 * taken for a choice of 自動. A writer that asks for no exclusive mode
 * keeps nobody out.
 */
LOCAL void test_conf_busy( void )
{
	T_CTSK	ct;
	TS_UUID	u;
	ID	key, tid;
	INT	i;

	if ( knl_obdev_uuid((CONST UB *)"netadp", &u) < E_OK ) KT_SKIP("no adapters' object");
	KT_ASSERT_ER(ob_fnd_sys(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u), E_OK);
	KT_ASSERT_ER(wm_dev_conf_put("NET_ADAPTER", "vnet"), E_OK);
	for ( i = 0; i < 60 && !adp_says("vnet"); i++ ) tk_dly_tsk(100);
	KT_ASSERT(adp_says("vnet"));

	/* written while held open with OB_O_WEXCL: read by the network's task meanwhile */
	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE | OB_O_WEXCL);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		ID	r2 = ob_opn_obj(&u, OB_OP_READ);
		ID	w2 = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE | OB_O_WEXCL);

		KT_ASSERT(r2 > 0);		/* a reader let in */
		KT_ASSERT_ER(w2, E_BUSY);	/* another writer kept out */
		if ( r2 > 0 ) ob_cls_obj(r2);
		if ( w2 > 0 ) ob_cls_obj(w2);
		KT_ASSERT_ER(conf_line_put(key, "NET_ADAPTER", "cdcnet"), E_OK);
		for ( i = 0; i < 60 && !adp_says("cdcnet"); i++ ) tk_dly_tsk(100);
		tm_printf((UB *)"  IOB the machine's settings held to be written (OB_O_WEXCL): the line written %s\n",
			  adp_says("cdcnet") ? "taken meanwhile" : "not taken");
		KT_ASSERT(adp_says("cdcnet"));
		ob_cls_obj(key);
	}

	/* a second writer waits for the first */
	held_key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE | OB_O_WEXCL);
	KT_ASSERT(held_key > 0);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)hold_task;
	ct.itskpri = 20;
	ct.stksz = 4096;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid > 0 && held_key > 0 ) {
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		KT_ASSERT_ER(wm_dev_conf_put("NET_ADAPTER", "vnet"), E_OK);
		tk_dly_tsk(100);
		(void)tk_del_tsk(tid);
	} else if ( held_key > 0 ) {
		ob_cls_obj(held_key);
	}
	for ( i = 0; i < 60 && !adp_says("vnet"); i++ ) tk_dly_tsk(100);
	KT_ASSERT(adp_says("vnet"));

	/* a writer that keeps everything out (OB_O_EXCL): the reader fails, the choice stays */
	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE | OB_O_EXCL);
	KT_ASSERT(key > 0);
	KT_ASSERT_ER(ob_opn_obj(&u, OB_OP_READ), E_BUSY);
	tk_dly_tsk(5000);			/* two of its readings and more */
	tm_printf((UB *)"  IOB the machine's settings held with every other key kept out 5 s: the adapter chosen %s\n",
		  adp_says("vnet") ? "stays vnet" : "changed");
	KT_ASSERT(adp_says("vnet"));
	if ( key > 0 ) ob_cls_obj(key);
	KT_ASSERT_ER(wm_dev_conf_put("NET_ADAPTER", "auto"), E_OK);
	for ( i = 0; i < 60 && !adp_says("auto"); i++ ) tk_dly_tsk(100);
	KT_ASSERT(adp_says("auto"));
}

/*
 * A disk's files reached from its device object's key, with no path in
 * the tree (B-2, design 7.2): its root a descriptor, files opened, made
 * and renamed below it, nothing reached above it whatever the path says,
 * nothing changed through a descriptor that only reads, the mount shared
 * with one of FS_MEDIA_DIR and gone with the last descriptor.
 */
LOCAL void test_vol_key( void )
{
	T_FSTAT	st;
	T_FSSTAT vs;
	TS_UUID	u;
	char	path[64];
	INT	vd, ro, fd, n;
	ID	key;

	if ( fat_dev[0] == '\0' ) KT_SKIP("no FAT disk (KT_IOB_FAT)");
	(void)fs_detach(FAT_AT);
	KT_ASSERT_ER(knl_obdev_uuid((CONST UB *)fat_dev, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) return;
	vd = fs_opn_vol(key, NULL, 0);
	KT_ASSERT(vd >= 0);
	if ( vd < 0 ) {
		ob_cls_obj(key);
		return;
	}
	KT_ASSERT_ER(fs_mkdirat(vd, "B2"), EX_OK);
	fd = fs_openat(vd, "B2/ONE.BIN", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	fill(wbuf, 100000, 0);
	KT_ASSERT_EQ(fs_write(fd, wbuf, 100000), 100000);
	fs_close(fd);
	KT_ASSERT_ER(fs_statat(vd, "/B2/./ONE.BIN", &st), EX_OK);
	KT_ASSERT(st.size == 100000);
	KT_ASSERT_ER(fs_renameat(vd, "B2/ONE.BIN", "B2/TWO.BIN"), EX_OK);
	fd = fs_openat(vd, "B2/SUB/../TWO.BIN", O_RDONLY);	/* SUB does not exist: the path is folded first */
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		n = fs_read(fd, rbuf, 200000);
		KT_ASSERT(n == 100000 && same(rbuf, 100000 - 100000 % 8, 0));
		fs_close(fd);
	}
	KT_ASSERT_ER(fs_fstatvfs(vd, &vs), EX_OK);
	KT_ASSERT(vs.blocks > 0 && vs.bfree > 0);

	/* nothing above the root */
	KT_ASSERT_EQ(fs_openat(vd, "..", O_RDONLY), EX_ACCES);
	KT_ASSERT_EQ(fs_openat(vd, "B2/../../x", O_RDONLY), EX_ACCES);
	KT_ASSERT_EQ(fs_openat(vd, "/../boot", O_RDONLY), EX_ACCES);
	KT_ASSERT_ER(fs_statat(vd, "../../boot", &st), EX_ACCES);

	/* a descriptor that only reads changes nothing */
	ro = fs_opn_vol(key, NULL, FS_MNT_RDONLY);
	KT_ASSERT(ro >= 0);
	KT_ASSERT_EQ(fs_openat(ro, "B2/NEW.BIN", O_RDWR | O_CREAT), EX_ROFS);
	KT_ASSERT_ER(fs_unlinkat(ro, "B2/TWO.BIN"), EX_ROFS);
	KT_ASSERT_ER(fs_statat(ro, "B2/TWO.BIN", &st), EX_OK);
	fd = fs_openat(vd, "B2/TWO.BIN", O_RDONLY);
	KT_ASSERT(fd >= 0);
	KT_ASSERT_EQ(fs_openat(fd, "x", O_RDONLY), EX_BADF);	/* not a disk's root */
	if ( fd >= 0 ) fs_close(fd);

	/* shared with the path of FS_MEDIA_DIR, and the path let go while the vds stay */
	KT_ASSERT_ER(fs_attach_dev(key, NULL, 0), EX_OK);
	knl_strcpy(path, FS_MEDIA_DIR "/");
	knl_strcat(path, fat_dev);
	knl_strcat(path, "/B2/TWO.BIN");
	KT_ASSERT_ER(fs_stat(path, &st), EX_OK);
	KT_ASSERT_ER(fs_detach_dev(key), EX_OK);
	KT_ASSERT(fs_stat(path, &st) < EX_OK);
	KT_ASSERT_ER(fs_statat(vd, "B2/TWO.BIN", &st), EX_OK);

	KT_ASSERT_ER(fs_unlinkat(vd, "B2/TWO.BIN"), EX_OK);
	KT_ASSERT_ER(fs_rmdirat(vd, "B2"), EX_OK);
	fs_close(ro);
	fs_close(vd);
	/* the last descriptor gone, the mount with it: the disk mounts again by name */
	KT_ASSERT_ER(fs_attach("fatfs", fat_dev, FAT_AT, 0), EX_OK);

	/* a disk the system mounted by name keeps its mount when the vds of it close */
	vd = fs_opn_vol(key, NULL, FS_MNT_RDONLY);
	KT_ASSERT(vd >= 0);
	if ( vd >= 0 ) fs_close(vd);
	KT_ASSERT_ER(fs_statvfs(FAT_AT, &vs), EX_OK);
	KT_ASSERT_ER(fs_detach(FAT_AT), EX_OK);
	ob_cls_obj(key);

	/* and the boot volume, which the system holds at /boot */
	if ( knl_obdev_uuid((CONST UB *)KT_BOOTDEV, &u) >= E_OK
	  && ( key = ob_opn_obj(&u, OB_OP_READ) ) > 0 ) {
		vd = fs_opn_vol(key, NULL, FS_MNT_RDONLY);
		KT_ASSERT(vd >= 0);
		if ( vd >= 0 ) {
			KT_ASSERT_ER(fs_fstatvfs(vd, &vs), EX_OK);
			fs_close(vd);
		}
		KT_ASSERT_ER(fs_statvfs("/boot", &vs), EX_OK);
		ob_cls_obj(key);
	}
}

/* ---------------------------------------------------------------- */

EXPORT void ktest_iob( void )
{
	wbuf = (UB *)Kmalloc(CHUNK_MAX);
	rbuf = (UB *)Kmalloc(CHUNK_MAX);
	if ( wbuf == NULL || rbuf == NULL ) {
		kt_skip("iob", "no memory");
		goto done;
	}
	iob_dev[0] = big_dev[0] = '\0';
	if ( pick(KT_IOB_DEV, iob_dev, &iob_sect) ) {
		tm_printf((UB *)"  IOB disk %s: %d MB\n", iob_dev, (INT)( iob_sect / 2048 ));
	} else {
		iob_dev[0] = '\0';
	}
	if ( pick(KT_IOB_BIG, big_dev, &big_sect) && knl_strcmp(big_dev, iob_dev) != 0 ) {
		tm_printf((UB *)"  IOB big disk %s: %d MB\n", big_dev, (INT)( big_sect / 2048 ));
	} else {
		big_dev[0] = '\0';
	}
	KT_RUN(test_raw);
	KT_RUN(test_volume);
	KT_RUN(test_records);
	KT_RUN(test_small);
	KT_RUN(test_copy);
	KT_RUN(test_whole);
	KT_RUN(test_lengthen);
	KT_RUN(test_replace);
	KT_RUN(test_kept);
	KT_RUN(test_durable);
	KT_RUN(test_gap);
	if ( have_vol ) (void)ob_det_vol(iob_dev);
	KT_RUN(test_big);
	if ( pick(KT_IOB_FAT, fat_dev, &fat_sect) && knl_strcmp(fat_dev, iob_dev) != 0
	  && knl_strcmp(fat_dev, big_dev) != 0 ) {
		tm_printf((UB *)"  IOB FAT disk %s: %d MB\n", fat_dev, (INT)( fat_sect / 2048 ));
	} else {
		fat_dev[0] = '\0';
	}
	KT_RUN(test_fat);
	KT_RUN(test_fat_flush);
	KT_RUN(test_fat_limits);
#ifdef KT_IOB_PULL
	KT_RUN(test_fat_pull);
#endif
	KT_RUN(test_vol_key);
	KT_RUN(test_huge);
	KT_RUN(test_fua);
	KT_RUN(test_conf_busy);
done:
	if ( wbuf != NULL ) Kfree(wbuf);
	if ( rbuf != NULL ) Kfree(rbuf);
}
