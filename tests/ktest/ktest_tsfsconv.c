/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfsconv.c
 *	The conversion of a native volume from version 4 to version 5
 *	(docs/tessronos/07-filesystem.md 7.3, ts/tsfsck.h ts_cnv_vol and
 *	knl_ob_conv_boot), on the scratch partition
 *
 *	A volume of version 4 is made with objects the way the format had
 *	them: two records grown a block at a time in turn, so that each is
 *	in more runs than a leaf holds (a tree of two levels, XT2), and a
 *	document held as a volume's top is. Its count put wrong, the volume
 *	is not converted, and no block of it is written but those a plain
 *	opening and closing write. With CONVREQ, the start's conversion
 *	leaves it of version 4, keeps what its check found, and takes the
 *	request off. Put right, it is converted: of version 5, the request
 *	gone, every byte read back as written, and the check finding
 *	nothing; converted again it is not a volume of version 4. Disk
 *	operation's conversion takes an attached volume off and puts it
 *	back, refuses one an object of which is open, and the system's. The
 *	power cut after each step of the conversion's writes leaves a
 *	volume of version 4 that converts again, or one of version 5.
 *
 *	Until the block layer makes the conversion final (ts_blk_convert
 *	and ts_blk_convreq answer E_NOSPT), the side that converts nothing
 *	is what is tried: what a converted volume is like is said to be
 *	not made yet.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include <ts/crc32c.h>
#include <ts/ob.h>
#include <ts/om.h>
#include "../../peripheral_kernel/obj/obj.h"

#define DEV		kt_scratch()
#define RUNS_N		( TSFSO_MAX_EXTENTS * 2 + 40 )
#define SB_COMPAT	272
#define SB_CRC		4092

LOCAL BOOL	made = FALSE;		/* a volume of version 4 is on the partition */
LOCAL BOOL	final = FALSE;		/* the block layer makes a conversion final */
LOCAL TS_UUID	u_a, u_b, u_top;

/* ---------------------------------------------------------------- words */

LOCAL UB pat( INT i )
{
	return (UB)( i * 11 + ( i >> 9 ) );
}

/* A volume of the format before on the partition, as R1.410's mktsfs made one */
LOCAL ER make_v4( void )
{
	T_TSFSFMT	f;

	knl_memset(&f, 0, sizeof(f));
	f.version = TSFSBLK_VERSION;
	return ts_format_blk_opt(DEV, "CONV", &f);
}

LOCAL BOOL same( CONST UB *a, CONST char *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( i == n );
}

LOCAL ER sb_of( T_TSFSBLK_SB *sb )
{
	ID	bvol = ts_obj_mount(DEV);
	ER	er;

	if ( bvol <= 0 ) {
		return ( bvol < 0 ) ? (ER)bvol : E_SYS;
	}
	er = ts_ref_vol_blk(bvol, sb);
	(void)ts_obj_unmount(bvol);
	return er;
}

/* A CRC of every block of the partition, the device read below the volume (Kmalloc) */
LOCAL UW *crcs( UD nblk )
{
	UW	*c = (UW *)Kmalloc(sizeof(UW) * (SZ)nblk);
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE * 16);
	ID	dd;
	UD	i, k;
	SZ	asz = 0;

	if ( c == NULL || b == NULL ) {
		if ( c != NULL ) Kfree(c);
		if ( b != NULL ) Kfree(b);
		return NULL;
	}
	dd = tk_opn_dev((UB *)DEV, TD_READ);
	for ( i = 0; dd > 0 && i < nblk; i += 16 ) {
		UD	n = ( nblk - i < 16 ) ? nblk - i : 16;

		if ( tk_srea_dev_d(dd, (D)( i * TSFSBLK_SECT_PER_BLK ), b, (SZ)( n * TSFSBLK_BLOCK_SIZE ), &asz) < E_OK ) {
			for ( k = 0; k < n; k++ ) c[i + k] = 0;
			continue;
		}
		for ( k = 0; k < n; k++ ) {
			c[i + k] = ts_crc32c(b + k * TSFSBLK_BLOCK_SIZE, TSFSBLK_BLOCK_SIZE);
		}
	}
	if ( dd > 0 ) {
		tk_cls_dev(dd, 0);
	}
	Kfree(b);
	return c;
}

/*
 * The superblock's feature written below the volume, on a volume nothing
 * has open: both copies, their CRCs made again, as a start that set it
 * would leave them.
 */
LOCAL ER sb_compat( UD bit, BOOL on )
{
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	ID	dd;
	INT	c, i;
	UD	v;
	SZ	asz = 0;
	ER	er = E_OK;

	if ( b == NULL ) {
		return E_NOMEM;
	}
	dd = tk_opn_dev((UB *)DEV, TD_UPDATE);
	if ( dd <= 0 ) {
		Kfree(b);
		return ( dd < 0 ) ? (ER)dd : E_SYS;
	}
	for ( c = 0; c < 2 && er >= E_OK; c++ ) {
		er = tk_srea_dev_d(dd, (D)( c * TSFSBLK_SECT_PER_BLK ), b, TSFSBLK_BLOCK_SIZE, &asz);
		if ( er < E_OK ) break;
		for ( v = 0, i = 7; i >= 0; i-- ) v = ( v << 8 ) | b[SB_COMPAT + i];
		v = on ? ( v | bit ) : ( v & ~bit );
		for ( i = 0; i < 8; i++ ) b[SB_COMPAT + i] = (UB)( v >> ( i * 8 ) );
		v = ts_crc32c(b, SB_CRC);
		for ( i = 0; i < 4; i++ ) b[SB_CRC + i] = (UB)( v >> ( i * 8 ) );
		er = tk_swri_dev_d(dd, (D)( c * TSFSBLK_SECT_PER_BLK ), b, TSFSBLK_BLOCK_SIZE, &asz);
	}
	tk_cls_dev(dd, 0);
	Kfree(b);
	return er;
}

LOCAL INT	ev_n;

LOCAL void note( void *ctx, CONST T_TSFSCKEV *ev )
{
	(void)ctx;
	if ( ev->kind == TSFSCK_K_PHASE ) return;
	if ( ev_n++ < 8 ) {
		tm_printf((UB *)"  ev ph %d kind %d blk %d a %d b %d\n", ev->phase, ev->kind,
			  (INT)ev->blk, (INT)ev->a, (INT)ev->b);
	}
}

/* The conversion of the partition, opened for it alone as the start opens it */
LOCAL ER convert( T_TSFSCNV *r )
{
	ID	bvol = ts_obj_mount(DEV);
	ER	er;

	if ( bvol <= 0 ) {
		return ( bvol < 0 ) ? (ER)bvol : E_SYS;
	}
	ev_n = 0;
	er = ts_cnv_vol(bvol, 0, r, note, NULL);
	tm_printf((UB *)"  convert: state %d er %d, %d found, check %d ms, all %d ms\n", (INT)r->state, (INT)r->er,
		  r->ck.problems, (INT)r->check_ms, (INT)r->total_ms);
	(void)ts_obj_unmount(bvol);
	return er;
}

/* ---------------------------------------------------------------- the volume of version 4 */

/* a volume of version 4 with two records of a tree of two levels and a top held; checked, nothing is found */
LOCAL void test_make( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSCK	*ck;
	UB		*w = NULL;
	SZ		asz = 0;
	ID		vol, bvol;
	INT		i, k, pos = -1;

	(void)ob_det_vol(DEV);
	if ( make_v4() < E_OK ) {
		KT_SKIP("no block device to make a volume on");
	}
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	if ( sb.version != TSFSBLK_VERSION || ( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) != 0 ) {
		KT_SKIP("no volume of version 4 made");
	}
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	bvol = ts_vol_blk_of(DEV);
	w = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	ck = (T_TSFSCK *)Kmalloc(sizeof(T_TSFSCK));
	KT_ASSERT(bvol > 0 && w != NULL && ck != NULL);
	if ( bvol > 0 && w != NULL ) {
		KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)"{\"name\":\"runs a\"}", 17, &u_a), E_OK);
		KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)"{\"name\":\"runs b\"}", 17, &u_b), E_OK);
		KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)"{\"name\":\"top\"}", 14, &u_top), E_OK);
		KT_ASSERT_ER(ts_obj_ins_rec(bvol, &u_a, -1, 5, 0, &pos), E_OK);
		KT_ASSERT_ER(ts_obj_ins_rec(bvol, &u_b, -1, 5, 0, &pos), E_OK);
		for ( i = 0; i < RUNS_N; i++ ) {
			for ( k = 0; k < TSFSBLK_BLOCK_SIZE; k++ ) w[k] = pat(i * 17 + k);
			KT_ASSERT_ER(ts_obj_wri_rec(bvol, &u_a, 0, (D)i * TSFSBLK_BLOCK_SIZE, w, TSFSBLK_BLOCK_SIZE, &asz), E_OK);
			w[0] ^= 0xFF;
			KT_ASSERT_ER(ts_obj_wri_rec(bvol, &u_b, 0, (D)i * TSFSBLK_BLOCK_SIZE, w, TSFSBLK_BLOCK_SIZE, &asz), E_OK);
		}
		KT_ASSERT_ER(ts_lnk_obj(vol, &u_a), E_OK);
		KT_ASSERT_ER(ts_lnk_obj(vol, &u_b), E_OK);
		KT_ASSERT_ER(ts_lnk_obj(vol, &u_top), E_OK);
		KT_ASSERT_ER(ts_ref_vol_blk(bvol, &sb), E_OK);
		KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_XT2 ) != 0);
		/* the block layer makes the conversion final, or not yet */
		final = (BOOL)( ts_blk_convreq(bvol, FALSE) != E_NOSPT );
		tm_printf((UB *)"  the conversion %s made final by the block layer\n", final ? "is" : "is not yet");
	}
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	if ( ck != NULL ) {
		KT_ASSERT_ER(ob_chk_run(DEV, 0, ck, NULL, NULL), E_OK);
		KT_ASSERT_EQ(ck->problems, 0);
		made = (BOOL)( ck->problems == 0 );
		Kfree(ck);
	}
	if ( w != NULL ) Kfree(w);
}

/*
 * its count put wrong, the volume is not converted: no block is written
 * but those any opening and closing write, and it is of version 4
 */
LOCAL void test_damage( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSCNV	r;
	UW		*c0 = NULL, *c1 = NULL, *c2 = NULL;
	UD		n, i, plain = 0, more = 0;
	ID		bvol;

	if ( !made ) KT_SKIP("no volume");
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(ts_obj_set_ref(bvol, &u_top, 7), E_OK);
	(void)ts_obj_unmount(bvol);
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	n = sb.total_blocks;

	/* what an opening and a closing write by themselves */
	c0 = crcs(n);
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	c1 = crcs(n);
	KT_ASSERT_ER(convert(&r), E_OK);
	c2 = crcs(n);
	KT_ASSERT(c0 != NULL && c1 != NULL && c2 != NULL);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_DAMAGE);
	KT_ASSERT(r.ck.problems > 0);
	KT_ASSERT(r.ck.found[TSFSCK_K_REFCNT] > 0);
	KT_ASSERT(!r.ck.written);
	if ( c0 != NULL && c1 != NULL && c2 != NULL ) {
		for ( i = 0; i < n; i++ ) {
			if ( c0[i] != c1[i] ) plain++;
			else if ( c1[i] != c2[i] ) more++;
		}
		tm_printf((UB *)"  %d blocks: %d changed by an opening, %d more by the conversion\n",
			  (INT)n, (INT)plain, (INT)more);
		KT_ASSERT_EQ(more, 0);
	}
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION);
	KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) == 0);
	if ( c0 != NULL ) Kfree(c0);
	if ( c1 != NULL ) Kfree(c1);
	if ( c2 != NULL ) Kfree(c2);
}

/*
 * with CONVREQ, what the start does: the damaged volume is left of
 * version 4, what its check found is kept, and the request is taken off
 */
LOCAL void test_boot_damage( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSCNV	r;
	T_TSFSCKEV	ev[4];
	UB		dev[TSFSCK_DEVNM_MAX];
	BOOL		req = FALSE;
	INT		n;

	if ( !made ) KT_SKIP("no volume");
	KT_ASSERT_ER(sb_compat(TSFSBLK_COMPAT_CONVREQ, TRUE), E_OK);
	KT_ASSERT_EQ(knl_ob_cnv_form(DEV, &req), TSFSBLK_VERSION);
	KT_ASSERT(req);
	KT_ASSERT_ER(knl_ob_conv_boot(NULL), E_OK);	/* no system volume: only those asked for */
	n = ob_cnv_boot_result(dev, sizeof(dev), &r, ev, 4);
	KT_ASSERT(n > 0);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_DAMAGE);
	KT_ASSERT(knl_strcmp((CONST char *)dev, DEV) == 0);
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION);
	(void)knl_ob_cnv_form(DEV, &req);
	if ( final ) {
		KT_ASSERT(!req);		/* not tried again at every start */
	} else {
		tm_printf((UB *)"  the request left on: ts_blk_convreq is not made yet\n");
		KT_ASSERT_ER(sb_compat(TSFSBLK_COMPAT_CONVREQ, FALSE), E_OK);
	}
	knl_ob_cnv_boot_forget();
	n = ob_cnv_boot_result(dev, sizeof(dev), &r, NULL, 0);
	KT_ASSERT_EQ(n, 0);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_NONE);
}

/* put right, the volume is converted: version 5, every byte as it was written, nothing found */
LOCAL void test_convert( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSCNV	r;
	T_TSFSCK	*ck;
	UB		*b;
	SZ		asz = 0;
	ID		bvol;
	INT		i, k, bad = 0;
	ER		er;

	if ( !made ) KT_SKIP("no volume");
	ck = (T_TSFSCK *)Kmalloc(sizeof(T_TSFSCK));
	b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(ck != NULL && b != NULL);
	if ( ck == NULL || b == NULL ) goto out;
	KT_ASSERT_ER(ob_chk_run(DEV, TSFSCK_REPAIR, ck, NULL, NULL), E_OK);
	KT_ASSERT_ER(ob_chk_run(DEV, 0, ck, NULL, NULL), E_OK);
	KT_ASSERT_EQ(ck->problems, 0);

	er = convert(&r);
	if ( !final ) {
		/* the block layer does not make it final yet: nothing is written, the volume of version 4 */
		KT_ASSERT_ER(er, E_NOSPT);
		KT_ASSERT_EQ(r.state, TSFSCNV_S_FAILED);
		KT_ASSERT_EQ(r.ck.problems, 0);
		KT_ASSERT_ER(sb_of(&sb), E_OK);
		KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION);
		KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) == 0);
		KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_XT2 ) != 0);
		tm_printf((UB *)"  a converted volume not tried: ts_blk_convert is not made yet\n");
		goto out;
	}
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_DONE);
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION5);
	KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) != 0);
	KT_ASSERT(( sb.compat & TSFSBLK_COMPAT_CONVREQ ) == 0);
	KT_ASSERT_EQ(knl_ob_cnv_form(DEV, NULL), TSFSBLK_VERSION5);
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol > 0 ) {
		for ( i = 0; i < RUNS_N; i++ ) {
			if ( ts_obj_rea_rec(bvol, &u_a, 0, (D)i * TSFSBLK_BLOCK_SIZE, b, TSFSBLK_BLOCK_SIZE, &asz) < E_OK
			  || asz != TSFSBLK_BLOCK_SIZE ) {
				bad++;
				continue;
			}
			for ( k = 0; k < TSFSBLK_BLOCK_SIZE && b[k] == pat(i * 17 + k); k++ ) ;
			if ( k < TSFSBLK_BLOCK_SIZE ) bad++;
		}
		KT_ASSERT_EQ(bad, 0);
		(void)ts_obj_unmount(bvol);
	}
	KT_ASSERT_ER(ob_chk_run(DEV, TSFSCK_FULL, ck, NULL, NULL), E_OK);
	KT_ASSERT_EQ(ck->problems, 0);

	/* a volume of version 5 is not converted again */
	KT_ASSERT_ER(convert(&r), E_OK);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_NOTV4);
	KT_ASSERT_ER(ob_cnv_request(DEV, TRUE), E_OBJ);
    out:
	if ( ck != NULL ) Kfree(ck);
	if ( b != NULL ) Kfree(b);
}

/* with CONVREQ, the start converts a volume found clean, and the request goes with it */
LOCAL void test_boot_convert( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSCNV	r;
	UB		dev[TSFSCK_DEVNM_MAX];
	BOOL		req = TRUE;

	if ( !made ) KT_SKIP("no volume");
	if ( !final ) KT_SKIP("ts_blk_convert is not made yet");
	(void)ob_det_vol(DEV);
	KT_ASSERT_ER(make_v4(), E_OK);
	KT_ASSERT_ER(sb_of(&sb), E_OK);
	if ( sb.version != TSFSBLK_VERSION ) {
		KT_SKIP("no volume of version 4 made");
	}
	KT_ASSERT_ER(ob_cnv_request(DEV, TRUE), E_OK);
	KT_ASSERT_EQ(knl_ob_cnv_form(DEV, &req), TSFSBLK_VERSION);
	KT_ASSERT(req);
	KT_ASSERT_ER(knl_ob_conv_boot(NULL), E_OK);
	(void)ob_cnv_boot_result(dev, sizeof(dev), &r, NULL, 0);
	KT_ASSERT_EQ(r.state, TSFSCNV_S_DONE);
	KT_ASSERT_EQ(knl_ob_cnv_form(DEV, &req), TSFSBLK_VERSION5);
	KT_ASSERT(!req);
	knl_ob_cnv_boot_forget();
}

/*
 * the power cut after each step of the conversion's writes: after the
 * ring was emptied the volume is of version 4 still and converts again;
 * after the first copy of the superblock and later it is of version 5,
 * opens, and its objects read as they were written
 */
LOCAL void test_cut( void )
{
	static CONST UINT	where[] = { TSFSBLK_CNV_RING, TSFSBLK_CNV_SB, TSFSBLK_CNV_PAIR, TSFSBLK_CNV_SPARE };
	T_TSFSBLK_SB	sb;
	T_TSFSCNV	r;
	T_TSFSCK	*ck;
	TS_UUID		u;
	UB		b[64];
	SZ		asz = 0;
	ID		bvol;
	INT		i, pos = -1;

	if ( !made ) KT_SKIP("no volume");
	if ( !final ) KT_SKIP("ts_blk_convert is not made yet");
	ck = (T_TSFSCK *)Kmalloc(sizeof(T_TSFSCK));
	KT_ASSERT(ck != NULL);
	if ( ck == NULL ) return;
	for ( i = 0; i < (INT)( sizeof(where) / sizeof(where[0]) ); i++ ) {
		KT_ASSERT_ER(make_v4(), E_OK);
		bvol = ts_obj_mount(DEV);
		KT_ASSERT(bvol > 0);
		if ( bvol <= 0 ) break;
		KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)"{\"name\":\"cut\"}", 14, &u), E_OK);
		KT_ASSERT_ER(ts_obj_ins_rec(bvol, &u, -1, 5, 0, &pos), E_OK);
		KT_ASSERT_ER(ts_obj_wri_rec(bvol, &u, 0, 0, (CONST UB *)"the bytes written before the cut", 32, &asz), E_OK);
		KT_ASSERT_ER(ts_blk_convert_fault(bvol, where[i]), E_OK);
		KT_ASSERT_ER(ts_cnv_vol(bvol, 0, &r, NULL, NULL), E_OK);
		(void)ts_obj_unmount(bvol);

		KT_ASSERT_ER(sb_of(&sb), E_OK);
		tm_printf((UB *)"  cut after step %d: version %d, FMT5 %d\n", (INT)where[i], (INT)sb.version,
			  ( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) != 0 ? 1 : 0);
		if ( where[i] == TSFSBLK_CNV_RING ) {
			KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION);
			KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) == 0);
			KT_ASSERT_ER(convert(&r), E_OK);
			KT_ASSERT_EQ(r.state, TSFSCNV_S_DONE);
			KT_ASSERT_ER(sb_of(&sb), E_OK);
		}
		KT_ASSERT_EQ(sb.version, TSFSBLK_VERSION5);
		KT_ASSERT(( sb.incompat & TSFSBLK_INCOMPAT_FMT5 ) != 0);
		bvol = ts_obj_mount(DEV);
		KT_ASSERT(bvol > 0);
		if ( bvol <= 0 ) break;
		knl_memset(b, 0, sizeof(b));
		KT_ASSERT_ER(ts_obj_rea_rec(bvol, &u, 0, 0, b, 32, &asz), E_OK);
		KT_ASSERT(asz == 32 && same(b, "the bytes written before the cut", 32));
		(void)ts_obj_unmount(bvol);
		KT_ASSERT_ER(ob_chk_run(DEV, TSFSCK_FULL, ck, NULL, NULL), E_OK);
		KT_ASSERT_EQ(ck->problems, 0);
	}
	Kfree(ck);
}

/*
 * ディスク操作's conversion: the system's volume refused, an attached one
 * taken off for it and put back, one with an object open refused
 */
LOCAL void test_run( void )
{
	T_TSFSCNV	r;
	TS_UUID		u;
	ID		k;
	ER		er;

	if ( !made ) KT_SKIP("no volume");
#ifdef CNF_OB_SYSVOL
	KT_ASSERT_ER(ob_cnv_run(CNF_OB_SYSVOL, 0, &r, NULL, NULL), E_RONLY);
#endif
	(void)ob_det_vol(DEV);
	KT_ASSERT_ER(make_v4(), E_OK);
	if ( knl_ob_cnv_form(DEV, NULL) != TSFSBLK_VERSION ) {
		KT_SKIP("no volume of version 4 made");
	}
	KT_ASSERT_ER(ob_att_vol(DEV, TSFS_STORE_BLK), E_OK);

	/* an object of it open: not taken off, not converted */
	KT_ASSERT_ER(om_cre_obj(DEV, (CONST UB *)"CONV", OM_BODY_FIG, &u), E_OK);
	k = ob_opn_obj(&u, OB_OP_READ);
	KT_ASSERT(k > 0);
	er = ob_cnv_run(DEV, 0, &r, NULL, NULL);
	KT_ASSERT_ER(er, E_BUSY);
	KT_ASSERT(r.ck.inuse);
	KT_ASSERT(knl_obfile_vol_path(DEV) > 0);
	if ( k > 0 ) (void)ob_cls_obj(k);

	/* closed: taken off, converted (or not yet), and put back */
	er = ob_cnv_run(DEV, 0, &r, NULL, NULL);
	tm_printf((UB *)"  disk operation's conversion: %d, state %d\n", (INT)er, (INT)r.state);
	KT_ASSERT(r.ck.detached);
	KT_ASSERT(knl_obfile_vol_path(DEV) > 0);
	if ( final ) {
		KT_ASSERT_ER(er, E_OK);
		KT_ASSERT_EQ(r.state, TSFSCNV_S_DONE);
		KT_ASSERT_EQ(knl_ob_cnv_form(DEV, NULL), TSFSBLK_VERSION5);
	} else {
		KT_ASSERT_ER(er, E_NOSPT);
		KT_ASSERT_EQ(r.state, TSFSCNV_S_FAILED);
		KT_ASSERT_EQ(knl_ob_cnv_form(DEV, NULL), TSFSBLK_VERSION);
	}
	(void)ob_del_obj(&u);
	KT_ASSERT_ER(ob_det_vol(DEV), E_OK);
	(void)ts_format_blk(DEV, "CONV");
}

EXPORT void ktest_tsfsconv( void )
{
	KT_RUN(test_make);
	KT_RUN(test_damage);
	KT_RUN(test_boot_damage);
	KT_RUN(test_convert);
	KT_RUN(test_boot_convert);
	KT_RUN(test_cut);
	KT_RUN(test_run);
}
