/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfsobj.c
 *	Real objects on a native volume (design 11.6, 11.7).
 *
 *	Most of this works the store directly, because the reference count,
 *	the garbage list and the moment a transaction becomes real are
 *	below the object API. The last test goes through that API instead,
 *	to show that a volume opened with TSFS_STORE_BLK answers the same
 *	calls as the FAT backed one.
 *
 *	The test task has a small stack, so every buffer of any size is
 *	taken from the heap.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>

#define ODEV		kt_scratch()
#define BIG_REC		10000		/* bytes: more than two blocks */
#define BIG_META	5000		/* bytes: more than fits in the header */
#define MANY_REC	60
#define SMALL_REC	100
#define LINK_MANY	150		/* more than one step of relinking */

LOCAL ID	bvol = 0;
LOCAL TS_UUID	first_uuid;
LOCAL BOOL	have_first = FALSE;

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != '\0' ) n++;

	return n;
}

/* Whether the text `t` of `n` bytes holds `w` */
LOCAL BOOL has( CONST UB *t, INT n, CONST char *w )
{
	INT	i, k, m = s_len(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

/* A byte that follows its position, so a copy out of place shows up */
LOCAL UB pat( INT i )
{
	return (UB)(i * 7 + (i >> 8));
}

LOCAL UD free_blocks( void )
{
	T_TSFSBLK_SB	sb;

	if ( ts_ref_vol_blk(bvol, &sb) < E_OK ) {
		return 0;
	}

	return sb.free_blocks;
}

/* ---------------------------------------------------------------- */

/* a native volume is made and opened, and holds no objects */
LOCAL void test_mount( void )
{
	T_RVOL	rvol;

	if ( ts_format_blk(ODEV, "OBJECTS") < E_OK ) {
		KT_SKIP("no block device to make a volume on");
	}
	bvol = ts_obj_mount(ODEV);
	if ( bvol <= 0 ) {
		KT_SKIP("the volume would not open");
	}

	KT_ASSERT_ER(ts_obj_ref_vol(bvol, &rvol), E_OK);
	KT_ASSERT_EQ(rvol.nobj, 0);
	KT_ASSERT_EQ(rvol.bsize, TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(rvol.bfree > 0);
	KT_ASSERT(rvol.time_valid);		/* the clock came from the RTC */

	tm_printf((UB*)"  %s: %d blocks free of %d\n", ODEV,
		  (INT)rvol.bfree, (INT)rvol.blocks);
}

/* an object is made, read back whole and found again by UUID */
LOCAL void test_object( void )
{
	CONST char	*json = "{\"name\":\"first\",\"maker\":\"ktest\","
				"\"refCount\":0,\"recordCount\":0}";
	T_ROBJ		robj;
	TS_UUID		uuid, buf[4];
	UB		*out;
	SZ		asize = 0;
	INT		cnt = 0, len = s_len(json);

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)json, len, &uuid), E_OK);
	first_uuid = uuid;
	have_first = TRUE;

	/* the items the header keeps a copy of came out of the text */
	KT_ASSERT_ER(ts_obj_ref(bvol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 0);
	KT_ASSERT_EQ(robj.nrec, 0);
	KT_ASSERT_EQ(robj.size, 0);
	KT_ASSERT_EQ(robj.name[0], (UB)'f');
	KT_ASSERT_EQ(robj.name[4], (UB)'t');
	KT_ASSERT((robj.flags & TSFSO_F_DELETABLE) != 0);
	KT_ASSERT((robj.flags & TSFSO_F_GARBAGE) == 0);

	/* the text keeps what it was given; the members the object block
	   holds are put in from there as it is read (design 11.5) */
	out = (UB *)Kmalloc(512);
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT_ER(ts_obj_get_meta(bvol, &uuid, out, 512, &asize), E_OK);
		KT_ASSERT(has(out, (INT)asize, "\"name\":\"first\""));
		KT_ASSERT(has(out, (INT)asize, "\"maker\":\"ktest\""));
		KT_ASSERT(has(out, (INT)asize, "\"refCount\":0"));
		KT_ASSERT(has(out, (INT)asize, "\"recordCount\":0"));
		KT_ASSERT(has(out, (INT)asize, "\"makeDate\":\"20"));
		KT_ASSERT_EQ(out[0], (UB)'{');
		KT_ASSERT_EQ(out[asize - 1], (UB)'}');

		/* a count written into the text does not count */
		KT_ASSERT_ER(ts_obj_set_meta(bvol, &uuid,
			(CONST UB *)"{\"name\":\"first\",\"refCount\":9}", 29), E_OK);
		KT_ASSERT_ER(ts_obj_get_meta(bvol, &uuid, out, 512, &asize), E_OK);
		KT_ASSERT(has(out, (INT)asize, "\"refCount\":0"));
		KT_ASSERT(!has(out, (INT)asize, "\"refCount\":9"));
		KT_ASSERT(!has(out, (INT)asize, "maker"));
		Kfree(out);
	}
	(void)len;

	/* and the index carries it */
	KT_ASSERT_ER(ts_obj_exists(bvol, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_list(bvol, NULL, buf, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&buf[0], &uuid), 0);

	/* the name can be set on its own */
	KT_ASSERT_ER(ts_obj_set_name(bvol, &uuid, (CONST UB *)"renamed"), E_OK);
	KT_ASSERT_ER(ts_obj_ref(bvol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.name[0], (UB)'r');
}

/* a record is written and read back, over more than one block */
LOCAL void test_record( void )
{
	T_RREC	rrec[4];
	UB	*buf;
	SZ	asize = 0;
	INT	recno = -1, cnt = 0, i, bad;

	if ( bvol <= 0 || !have_first ) KT_SKIP("no object");

	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &first_uuid, TSFS_REC_XTAD, &recno), E_OK);
	KT_ASSERT_EQ(recno, 0);

	buf = (UB *)Kmalloc(BIG_REC);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;

	for ( i = 0; i < BIG_REC; i++ ) {
		buf[i] = pat(i);
	}
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &first_uuid, 0, 0, buf, BIG_REC,
				    &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_REC);

	knl_memset(buf, 0, BIG_REC);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &first_uuid, 0, 0, buf, BIG_REC,
				    &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_REC);
	for ( i = 0, bad = 0; i < BIG_REC; i++ ) {
		if ( buf[i] != pat(i) ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);

	/* a read that starts inside the record gives what is left of it */
	knl_memset(buf, 0, BIG_REC);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &first_uuid, 0, BIG_REC - 10, buf,
				    BIG_REC, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, 10);
	KT_ASSERT_EQ(buf[0], pat(BIG_REC - 10));

	/* a read past the end gives nothing rather than an error */
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &first_uuid, 0, BIG_REC, buf, 16,
				    &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, 0);

	/* a write in the middle leaves the rest of the block alone */
	buf[0] = 0x5A;
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &first_uuid, 0, 4100, buf, 1,
				    &asize), E_OK);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &first_uuid, 0, 4099, buf, 3,
				    &asize), E_OK);
	KT_ASSERT_EQ(buf[0], pat(4099));
	KT_ASSERT_EQ(buf[1], 0x5A);
	KT_ASSERT_EQ(buf[2], pat(4101));

	Kfree(buf);

	KT_ASSERT_ER(ts_obj_lst_rec(bvol, &first_uuid, rrec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT_EQ(rrec[0].recno, 0);
	KT_ASSERT_EQ(rrec[0].rectype, TSFS_REC_XTAD);
	KT_ASSERT_EQ(rrec[0].size, BIG_REC);

	/* cutting it short gives the blocks past the end back */
	KT_ASSERT_ER(ts_obj_trn_rec(bvol, &first_uuid, 0, 100), E_OK);
	KT_ASSERT_ER(ts_obj_lst_rec(bvol, &first_uuid, rrec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(rrec[0].size, 100);
}

/*
 * More records than the header block holds, and a metadata text too big
 * for it: both move to a run of their own and read back the same.
 */
LOCAL void test_outgrow( void )
{
	T_RREC		*rrec;
	TS_UUID		uuid;
	UB		*buf;
	SZ		asize = 0;
	INT		i, recno = -1, cnt = 0, bad = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &uuid), E_OK);

	/* Many records with a few bytes each: the bytes fill the inline
	   area, and what does not fit there -- bytes and table alike --
	   goes into blocks of its own */
	for ( i = 0; i < MANY_REC; i++ ) {
		KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_BIN, &recno), E_OK);
		KT_ASSERT_EQ(recno, i);
	}
	/* every one of them keeps the type and the bytes it was given */
	buf = (UB *)Kmalloc(SMALL_REC);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	for ( i = 0; i < MANY_REC; i++ ) {
		knl_memset(buf, (UB)(0x40 + i), SMALL_REC);
		KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, i, 0, buf, SMALL_REC, &asize), E_OK);
	}
	for ( i = 0; i < MANY_REC; i++ ) {
		knl_memset(buf, 0, SMALL_REC);
		KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, i, 0, buf, SMALL_REC, &asize), E_OK);
		if ( asize != SMALL_REC || buf[0] != (UB)(0x40 + i)
		  || buf[SMALL_REC - 1] != (UB)(0x40 + i) ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);
	Kfree(buf);

	rrec = (T_RREC *)Kmalloc(sizeof(T_RREC) * (MANY_REC + 2));
	KT_ASSERT(rrec != NULL);
	if ( rrec != NULL ) {
		KT_ASSERT_ER(ts_obj_lst_rec(bvol, &uuid, rrec, MANY_REC + 2, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, MANY_REC);
		KT_ASSERT_EQ(rrec[MANY_REC - 1].rectype, TSFS_REC_BIN);
		KT_ASSERT_EQ((INT)rrec[MANY_REC - 1].size, SMALL_REC);
		Kfree(rrec);
	}

	/* and a text of more than the inline area holds */
	buf = (UB *)Kmalloc(BIG_META + 1);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	for ( i = 0; i < BIG_META; i++ ) {
		buf[i] = (UB)('a' + (i % 26));
	}
	KT_ASSERT(BIG_META > TSFSO_INLINE_BYTES);
	KT_ASSERT_ER(ts_obj_set_meta(bvol, &uuid, buf, BIG_META), E_OK);

	knl_memset(buf, 0, BIG_META);
	KT_ASSERT_ER(ts_obj_get_meta(bvol, &uuid, buf, BIG_META, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_META);
	for ( i = 0, bad = 0; i < BIG_META; i++ ) {
		if ( buf[i] != (UB)('a' + (i % 26)) ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);

	/* a short text goes back into the header block, with the order of
	   the records put back in */
	KT_ASSERT_ER(ts_obj_set_meta(bvol, &uuid, (CONST UB *)"{\"n\":1}", 7), E_OK);
	knl_memset(buf, 0, BIG_META);
	KT_ASSERT_ER(ts_obj_get_meta(bvol, &uuid, buf, BIG_META, &asize), E_OK);
	KT_ASSERT(has(buf, (INT)asize, "\"n\":1"));
	KT_ASSERT(has(buf, (INT)asize, "\"recordCount\":60"));
	KT_ASSERT(has(buf, (INT)asize, "{\"rid\":60,\"rt\":15,\"sub\":0}]"));
	Kfree(buf);

	/* it is not wanted any further */
	KT_ASSERT_ER(ts_obj_delete(bvol, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_exists(bvol, &uuid), E_NOEXS);
}

/* Whether the records read back hold the marks, in this order */
LOCAL INT order_is( CONST TS_UUID *uuid, CONST UB *marks, INT n )
{
	UB	b = 0;
	SZ	asize = 0;
	INT	i, bad = 0;

	for ( i = 0; i < n; i++ ) {
		if ( ts_obj_rea_rec(bvol, uuid, i, 0, &b, 1, &asize) < E_OK
		  || asize != 1 || b != marks[i] ) bad++;
	}
	return bad;
}

/* records are put in, moved and taken out anywhere; types go with them */
LOCAL void test_order( void )
{
	T_RREC	rrec[8];
	TS_UUID	uuid;
	UB	b, *t;
	SZ	asize = 0;
	INT	i, pos = -1, cnt = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)"{\"name\":\"order\"}", 16, &uuid), E_OK);
	for ( i = 0; i < 3; i++ ) {
		b = (UB)( 'a' + i );
		KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_XTAD, &pos), E_OK);
		KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, pos, 0, &b, 1, &asize), E_OK);
	}
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"abc", 3), 0);

	/* put in at 1: the others move down, its type sets its kind */
	KT_ASSERT_ER(ts_obj_ins_rec(bvol, &uuid, 1, 5, 2, &pos), E_OK);
	KT_ASSERT_EQ(pos, 1);
	b = 'x';
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, 1, 0, &b, 1, &asize), E_OK);
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"axbc", 4), 0);
	KT_ASSERT_ER(ts_obj_lst_rec(bvol, &uuid, rrec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 4);
	KT_ASSERT_EQ(rrec[1].rt, 5);
	KT_ASSERT_EQ(rrec[1].sub, 2);
	KT_ASSERT_EQ(rrec[1].rectype, TSFS_REC_BIN);
	KT_ASSERT_EQ(rrec[2].rt, 1);
	KT_ASSERT_EQ(rrec[2].rectype, TSFS_REC_XTAD);

	/* a resource belongs to its record, wherever that goes */
	b = 'R';
	KT_ASSERT_ER(ts_obj_wri_res(bvol, &uuid, 3, 0, (CONST UB *)"png", &b, 1), E_OK);

	/* moved from 3 to 0 and from 1 to 2 */
	KT_ASSERT_ER(ts_obj_mov_rec(bvol, &uuid, 3, 0), E_OK);
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"caxb", 4), 0);
	KT_ASSERT_ER(ts_obj_mov_rec(bvol, &uuid, 1, 2), E_OK);
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"cxab", 4), 0);
	b = 0;
	KT_ASSERT_ER(ts_obj_rea_res(bvol, &uuid, 0, 0, &b, 1, &asize, NULL), E_OK);
	KT_ASSERT_EQ(b, (UB)'R');
	KT_ASSERT_ER(ts_obj_mov_rec(bvol, &uuid, 0, 4), E_NOEXS);

	/* the type changed, the kind with it */
	KT_ASSERT_ER(ts_obj_set_rtp(bvol, &uuid, 1, 1, 0), E_OK);
	KT_ASSERT_ER(ts_obj_lst_rec(bvol, &uuid, rrec, 8, &cnt), E_OK);
	KT_ASSERT_EQ(rrec[1].rectype, TSFS_REC_XTAD);
	KT_ASSERT_EQ(rrec[1].rt, 1);

	/* the order is in the text, and the text set again keeps it */
	t = (UB *)Kmalloc(512);
	KT_ASSERT(t != NULL);
	if ( t != NULL ) {
		KT_ASSERT_ER(ts_obj_get_meta(bvol, &uuid, t, 512, &asize), E_OK);
		KT_ASSERT(has(t, (INT)asize, "\"records\":[{\"rid\":3,"));
		KT_ASSERT_ER(ts_obj_set_meta(bvol, &uuid,
			(CONST UB *)"{\"name\":\"o2\",\"records\":[]}", 26), E_OK);
		KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"cxab", 4), 0);
		Kfree(t);
	}

	/* taken out from the front: the resource goes with the record */
	KT_ASSERT_ER(ts_obj_del_rec(bvol, &uuid, 0), E_OK);
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"xab", 3), 0);
	KT_ASSERT_ER(ts_obj_rea_res(bvol, &uuid, 0, 0, &b, 1, &asize, NULL), E_NOEXS);

	/* and it all holds over a remount */
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_EQ(order_is(&uuid, (CONST UB *)"xab", 3), 0);
	KT_ASSERT_ER(ts_obj_delete(bvol, &uuid), E_OK);
}

/* ---------------------------------------------------------------- links */

/* A text of links to the objects given, one each, after `head` */
LOCAL INT link_text( UB *out, CONST TS_UUID *to, INT n )
{
	char	u[40];
	TS_UUID	v;
	INT	k = 0, i, m;
	CONST char *p;

	for ( p = "<document><p>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	for ( i = 0; i < n; i++ ) {
		for ( p = "<link id=\""; *p != '\0'; p++ ) out[k++] = (UB)*p;
		ts_uuid_to_str(&to[i], u, sizeof(u));
		for ( m = 0; u[m] != '\0'; m++ ) out[k++] = (UB)u[m];
		for ( p = "_0.xtad\" vobjid=\""; *p != '\0'; p++ ) out[k++] = (UB)*p;
		ts_gen_uuid(&v);
		ts_uuid_to_str(&v, u, sizeof(u));
		for ( m = 0; u[m] != '\0'; m++ ) out[k++] = (UB)u[m];
		for ( p = "\" width=\"100\"/>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	}
	for ( p = "</p></document>"; *p != '\0'; p++ ) out[k++] = (UB)*p;
	return k;
}

LOCAL INT refs( CONST TS_UUID *u )
{
	T_ROBJ	robj;

	if ( ts_obj_ref(bvol, u, &robj) < E_OK ) return -1;
	return robj.refcnt;
}

LOCAL BOOL on_gc( CONST TS_UUID *u )
{
	T_ROBJ	robj;

	return (BOOL)( ts_obj_ref(bvol, u, &robj) >= E_OK
		    && ( robj.flags & TSFSO_F_GARBAGE ) != 0 );
}

/* A document with one xmlTAD record of links to the objects given */
LOCAL ER make_doc( TS_UUID *doc, CONST TS_UUID *to, INT n, UB *txt )
{
	SZ	asize = 0;
	INT	recno = -1, len;
	ER	er;

	er = ts_obj_create(bvol, NULL, 0, doc);
	if ( er >= E_OK ) er = ts_obj_apd_rec(bvol, doc, TSFS_REC_XTAD, &recno);
	len = link_text(txt, to, n);
	if ( er >= E_OK ) er = ts_obj_wri_rec(bvol, doc, recno, 0, txt, len, &asize);
	return er;
}

/* the store reads the links of a document and counts them itself */
LOCAL void test_links( void )
{
	T_RLNK	lk[8];
	TS_UUID	a, b, x, d, to[4];
	T_ROBJ	robj;
	UB	*txt;
	SZ	asize = 0;
	INT	cnt = 0, len, i, ext;

	if ( bvol <= 0 ) KT_SKIP("no volume");
	txt = (UB *)Kmalloc(LINK_MANY * 160 + 64);
	KT_ASSERT(txt != NULL);
	if ( txt == NULL ) return;

	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &a), E_OK);
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &b), E_OK);
	ts_gen_uuid(&x);			/* on no volume */

	/* two links to a, one to b, one off the volume */
	to[0] = a; to[1] = b; to[2] = a; to[3] = x;
	KT_ASSERT_ER(make_doc(&d, to, 4, txt), E_OK);
	KT_ASSERT_EQ(refs(&a), 0);		/* nothing until it is relinked */
	KT_ASSERT_ER(ts_obj_ref(bvol, &d, &robj), E_OK);
	KT_ASSERT((robj.flags & TSFSO_F_RELINK) != 0);
	KT_ASSERT((robj.flags & TSFSO_F_AUTOREF) != 0);
	KT_ASSERT_ER(ts_obj_relink(bvol, &d), E_OK);
	KT_ASSERT_EQ(refs(&a), 2);
	KT_ASSERT_EQ(refs(&b), 1);
	KT_ASSERT_ER(ts_obj_ref(bvol, &d, &robj), E_OK);
	KT_ASSERT((robj.flags & TSFSO_F_RELINK) == 0);
	KT_ASSERT_ER(ts_obj_lst_lnk(bvol, &d, lk, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 4);
	for ( i = 0, ext = 0; i < cnt; i++ ) {
		if ( ( lk[i].flags & TSFS_LNK_EXTERNAL ) != 0 ) ext++;
		KT_ASSERT_EQ(lk[i].recno, 0);
	}
	KT_ASSERT_EQ(ext, 1);

	/* rewritten with one link to a: b falls to zero and is garbage */
	len = link_text(txt, &a, 1);
	KT_ASSERT_ER(ts_obj_rpl_rec(bvol, &d, 0, txt, len), E_OK);
	KT_ASSERT_ER(ts_obj_relink(bvol, &d), E_OK);
	KT_ASSERT_EQ(refs(&a), 1);
	KT_ASSERT_EQ(refs(&b), 0);
	KT_ASSERT(on_gc(&b));

	/* holds are counted apart: letting go of one not held is refused */
	KT_ASSERT_ER(ts_obj_link(bvol, &a), E_OK);
	KT_ASSERT_EQ(refs(&a), 2);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &a), E_OK);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &a), E_OBJ);
	KT_ASSERT_EQ(refs(&a), 1);

	/* the document goes, and its link with it */
	KT_ASSERT_ER(ts_obj_delete(bvol, &d), E_OK);
	KT_ASSERT_EQ(refs(&a), 0);
	KT_ASSERT(on_gc(&a));

	/* the power goes after a write: the next mount counts the links */
	KT_ASSERT_ER(ts_obj_link(bvol, &b), E_OK);	/* off the list */
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &d), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &d, TSFS_REC_XTAD, &i), E_OK);
	len = link_text(txt, &b, 1);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &d, 0, 0, txt, len, &asize), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);
	ts_obj_unmount(bvol);			/* it cannot finish anything */
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(txt); return; }
	KT_ASSERT_EQ(refs(&b), 2);
	KT_ASSERT_ER(ts_obj_ref(bvol, &d, &robj), E_OK);
	KT_ASSERT((robj.flags & TSFSO_F_RELINK) == 0);

	/* deleted while open: gone from the index, still readable, then reaped */
	KT_ASSERT_ER(ts_obj_orphan(bvol, &d), E_OK);
	KT_ASSERT_ER(ts_obj_exists(bvol, &d), E_NOEXS);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &d, 0, 0, txt, 16, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, 16);
	KT_ASSERT_ER(ts_obj_delete(bvol, &d), E_NOEXS);
	KT_ASSERT_ER(ts_obj_reap(bvol, &d), E_OK);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &d, 0, 0, txt, 16, &asize), E_NOEXS);
	KT_ASSERT_EQ(refs(&b), 1);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &b), E_OK);

	/* an orphan the power left behind goes at the next mount */
	{
		UD	before = free_blocks();

		KT_ASSERT_ER(make_doc(&d, &a, 1, txt), E_OK);
		KT_ASSERT_ER(ts_obj_relink(bvol, &d), E_OK);
		KT_ASSERT_EQ(refs(&a), 1);
		KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
		KT_ASSERT_ER(ts_obj_orphan(bvol, &d), E_OK);
		KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);
		ts_obj_unmount(bvol);
		bvol = ts_obj_mount(ODEV);
		KT_ASSERT(bvol > 0);
		if ( bvol <= 0 ) { Kfree(txt); return; }
		KT_ASSERT_ER(ts_obj_rea_rec(bvol, &d, 0, 0, txt, 16, &asize), E_NOEXS);
		KT_ASSERT_EQ(refs(&a), 0);
		KT_ASSERT_EQ(free_blocks(), before);
	}
	Kfree(txt);
}

/* links to more objects than one step moves */
LOCAL void test_many_links( void )
{
	TS_UUID	*to, d;
	UB	*txt;
	T_RLNK	*lk;
	INT	i, bad, cnt = 0, gone = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");
	to = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * LINK_MANY);
	txt = (UB *)Kmalloc(LINK_MANY * 160 + 64);
	lk = (T_RLNK *)Kmalloc(sizeof(T_RLNK) * ( LINK_MANY + 1 ));
	KT_ASSERT(to != NULL && txt != NULL && lk != NULL);
	if ( to == NULL || txt == NULL || lk == NULL ) return;

	for ( i = 0; i < LINK_MANY; i++ ) {
		KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &to[i]), E_OK);
	}
	KT_ASSERT_ER(make_doc(&d, to, LINK_MANY, txt), E_OK);
	KT_ASSERT_ER(ts_obj_relink(bvol, &d), E_OK);
	for ( i = 0, bad = 0; i < LINK_MANY; i++ ) {
		if ( refs(&to[i]) != 1 ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_ER(ts_obj_lst_lnk(bvol, &d, lk, LINK_MANY + 1, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, LINK_MANY);

	/* the document goes: every one of them is garbage, and collected */
	KT_ASSERT_ER(ts_obj_delete(bvol, &d), E_OK);
	for ( i = 0, bad = 0; i < LINK_MANY; i++ ) {
		if ( refs(&to[i]) != 0 || !on_gc(&to[i]) ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_ER(ts_obj_gc_all(bvol, &gone), E_OK);
	KT_ASSERT(gone >= LINK_MANY);
	KT_ASSERT_ER(ts_obj_exists(bvol, &to[0]), E_NOEXS);

	Kfree(to);
	Kfree(txt);
	Kfree(lk);
}

/* the count goes up and down, and zero puts the object on the list */
LOCAL void test_refcount( void )
{
	T_ROBJ	robj;
	TS_UUID	buf[4];
	INT	cnt = 0;

	if ( bvol <= 0 || !have_first ) KT_SKIP("no object");

	KT_ASSERT_ER(ts_obj_gc_list(bvol, NULL, buf, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);			/* nothing is garbage yet */

	KT_ASSERT_ER(ts_obj_link(bvol, &first_uuid), E_OK);
	KT_ASSERT_ER(ts_obj_link(bvol, &first_uuid), E_OK);
	KT_ASSERT_ER(ts_obj_ref(bvol, &first_uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 2);

	/* an object that is referred to may not be deleted */
	KT_ASSERT_ER(ts_obj_delete(bvol, &first_uuid), E_OBJ);

	KT_ASSERT_ER(ts_obj_unlink(bvol, &first_uuid), E_OK);
	KT_ASSERT_ER(ts_obj_ref(bvol, &first_uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 1);
	KT_ASSERT((robj.flags & TSFSO_F_GARBAGE) == 0);

	/* the last reference puts it on the garbage list */
	KT_ASSERT_ER(ts_obj_unlink(bvol, &first_uuid), E_OK);
	KT_ASSERT_ER(ts_obj_ref(bvol, &first_uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 0);
	KT_ASSERT((robj.flags & TSFSO_F_GARBAGE) != 0);

	KT_ASSERT_ER(ts_obj_gc_list(bvol, NULL, buf, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&buf[0], &first_uuid), 0);

	/* below zero is refused */
	KT_ASSERT_ER(ts_obj_unlink(bvol, &first_uuid), E_OBJ);

	/* and a reference again takes it off the list */
	KT_ASSERT_ER(ts_obj_link(bvol, &first_uuid), E_OK);
	KT_ASSERT_ER(ts_obj_gc_list(bvol, NULL, buf, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);
	KT_ASSERT_ER(ts_obj_ref(bvol, &first_uuid, &robj), E_OK);
	KT_ASSERT((robj.flags & TSFSO_F_GARBAGE) == 0);

	KT_ASSERT_ER(ts_obj_unlink(bvol, &first_uuid), E_OK);
}

/*
 * Collecting frees the blocks an object held, and an object that says it
 * may not be deleted stays on the list.
 */
LOCAL void test_collect( void )
{
	TS_UUID	uuid, buf[4];
	UB	*data;
	UD	before, after;
	SZ	asize = 0;
	INT	cnt = 0, recno = -1;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	before = free_blocks();
	KT_ASSERT(before > 0);

	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_XTAD, &recno), E_OK);

	data = (UB *)Kmalloc(BIG_REC);
	KT_ASSERT(data != NULL);
	if ( data == NULL ) return;
	knl_memset(data, 0x33, BIG_REC);
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, 0, 0, data, BIG_REC, &asize), E_OK);
	KT_ASSERT_ER(ts_obj_wri_res(bvol, &uuid, TSFSO_ICON_REC, 0,
				    (CONST UB *)"ico", data, 300), E_OK);
	{
		/* the icon is its own part: read by its own call, listed once as ".ico" */
		T_TSFSRES	rl[4];
		SZ		isz = 0;
		UB		b4[4];

		KT_ASSERT_ER(ts_obj_get_icon(bvol, &uuid, NULL, 0, &isz), E_OK);
		KT_ASSERT_EQ(isz, 300);
		KT_ASSERT_ER(ts_obj_get_icon(bvol, &uuid, b4, 4, &isz), E_OK);
		KT_ASSERT_EQ(b4[0], 0x33);
		KT_ASSERT_ER(ts_obj_lst_res(bvol, &uuid, rl, 4, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, 1);
		KT_ASSERT_EQ(rl[0].recno, TSFSO_ICON_REC);
		KT_ASSERT_EQ(rl[0].size, 300);
		cnt = 0;
	}
	Kfree(data);

	KT_ASSERT(free_blocks() < before);

	/* while it is referred to it is not on the list at all */
	KT_ASSERT_ER(ts_obj_link(bvol, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_gc_one(bvol, &uuid), E_OBJ);

	/* an object that may not be deleted goes on the list and stays */
	KT_ASSERT_ER(ts_obj_set_flags(bvol, &uuid, 0, TSFSO_F_DELETABLE), E_OK);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_gc_one(bvol, &uuid), E_RONLY);
	KT_ASSERT_ER(ts_obj_gc_list(bvol, NULL, buf, 4, &cnt), E_OK);
	KT_ASSERT(cnt >= 1);
	KT_ASSERT_ER(ts_obj_exists(bvol, &uuid), E_OK);

	/* once it may, collecting gives every block it held back */
	KT_ASSERT_ER(ts_obj_set_flags(bvol, &uuid, TSFSO_F_DELETABLE,
				      TSFSO_F_DELETABLE), E_OK);
	KT_ASSERT_ER(ts_obj_gc_one(bvol, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_exists(bvol, &uuid), E_NOEXS);

	after = free_blocks();
	tm_printf((UB*)"  free blocks: %d before, %d after\n",
		  (INT)before, (INT)after);
	KT_ASSERT_EQ(after, before);
}

/* collecting the whole volume takes the objects that may go */
LOCAL void test_collect_all( void )
{
	TS_UUID	keep, drop, buf[8];
	INT	cnt = 0, taken = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &keep), E_OK);
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &drop), E_OK);
	KT_ASSERT_ER(ts_obj_link(bvol, &keep), E_OK);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &keep), E_OK);
	KT_ASSERT_ER(ts_obj_link(bvol, &drop), E_OK);
	KT_ASSERT_ER(ts_obj_unlink(bvol, &drop), E_OK);
	KT_ASSERT_ER(ts_obj_set_flags(bvol, &keep, 0, TSFSO_F_DELETABLE), E_OK);

	KT_ASSERT_ER(ts_obj_gc_all(bvol, &taken), E_OK);
	KT_ASSERT(taken >= 2);			/* drop and the earlier one */

	KT_ASSERT_ER(ts_obj_exists(bvol, &drop), E_NOEXS);
	KT_ASSERT_ER(ts_obj_exists(bvol, &keep), E_OK);

	/* the one that may not go is all that is left on the list */
	KT_ASSERT_ER(ts_obj_gc_list(bvol, NULL, buf, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&buf[0], &keep), 0);

	have_first = FALSE;			/* it was collected with the rest */

	/* let it go, so that the volume is left tidy */
	KT_ASSERT_ER(ts_obj_set_flags(bvol, &keep, TSFSO_F_DELETABLE,
				      TSFSO_F_DELETABLE), E_OK);
	KT_ASSERT_ER(ts_obj_gc_one(bvol, &keep), E_OK);
}

/*
 * The power goes at the moment the journal makes a change real. Before
 * the record there is nothing to put back and the object never existed;
 * after it the object comes back whole.
 */
LOCAL void test_crash( void )
{
	T_ROBJ		robj;
	CONST char	*json = "{\"name\":\"survivor\",\"refCount\":0}";
	TS_UUID		lost, kept;
	INT		len = s_len(json);

	if ( bvol <= 0 ) KT_SKIP("no volume");

	/* something has to be in the index, so that the root does not move
	   in a transaction that is then thrown away */
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &kept), E_OK);

	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_BEFORE_COMMIT), E_OK);
	KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)json, len, &lost), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);

	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;

	/* no record was written, so the transaction never happened */
	KT_ASSERT_ER(ts_obj_exists(bvol, &lost), E_NOEXS);
	KT_ASSERT_ER(ts_obj_exists(bvol, &kept), E_OK);

	/* now the other side of the same moment */
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(ts_obj_create(bvol, (CONST UB *)json, len, &kept), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);

	/* the blocks are not where they belong yet */
	KT_ASSERT(ts_obj_exists(bvol, &kept) < E_OK);

	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;

	/* the record was written, so coming back puts the change in place */
	KT_ASSERT_ER(ts_obj_exists(bvol, &kept), E_OK);
	KT_ASSERT_ER(ts_obj_ref(bvol, &kept, &robj), E_OK);
	KT_ASSERT_EQ(robj.name[0], (UB)'s');
	KT_ASSERT_EQ(robj.refcnt, 0);
}

/*
 * The same volume through the object API, which is what a program uses.
 * A volume opened with TSFS_STORE_BLK answers the calls the FAT backed
 * one answers.
 */
LOCAL void test_api( void )
{
	T_COBJ	cobj;
	T_ROBJ	robj;
	T_RVOL	rvol;
	TS_UUID	uuid, buf[8];
	UB	*data;
	SZ	asize = 0;
	ID	vol, od;
	INT	recno = -1, cnt = 0, taken = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = 0;

	vol = ts_opn_vol(ODEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;

	KT_ASSERT_ER(ts_ref_vol(vol, &rvol), E_OK);
	KT_ASSERT_EQ(rvol.bsize, TSFSBLK_BLOCK_SIZE);

	cobj.json   = (CONST UB *)"{\"name\":\"through the api\",\"refCount\":0}";
	cobj.jsonsz = s_len("{\"name\":\"through the api\",\"refCount\":0}");
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od > 0 ) {
		KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_XTAD, &recno), E_OK);
		KT_ASSERT_EQ(recno, 0);

		data = (UB *)Kmalloc(256);
		KT_ASSERT(data != NULL);
		if ( data != NULL ) {
			knl_memset(data, 0x77, 256);
			KT_ASSERT_ER(ts_wri_rec(od, 0, 0, data, 256, &asize), E_OK);
			knl_memset(data, 0, 256);
			KT_ASSERT_ER(ts_rea_rec(od, 0, 0, data, 256, &asize), E_OK);
			KT_ASSERT_EQ((INT)asize, 256);
			KT_ASSERT_EQ(data[255], 0x77);

			/* replaced whole through the same calls */
			knl_memset(data, 0x66, 256);
			KT_ASSERT_ER(ts_rpl_rec(od, 0, data, 100), E_OK);
			knl_memset(data, 0, 256);
			KT_ASSERT_ER(ts_rea_rec(od, 0, 0, data, 256, &asize), E_OK);
			KT_ASSERT_EQ((INT)asize, 100);
			KT_ASSERT_EQ(data[99], 0x66);
			knl_memset(data, 0x77, 256);
			KT_ASSERT_ER(ts_rpl_rec(od, 0, data, 256), E_OK);	/* as it was */

			/* a resource of the same object */
			KT_ASSERT_ER(ts_wri_res(od, 0, 0, (CONST UB *)"png",
						data, 200), E_OK);
			knl_memset(data, 0, 256);
			KT_ASSERT_ER(ts_rea_res(od, 0, 0, data, 256, &asize,
						NULL), E_OK);
			KT_ASSERT_EQ((INT)asize, 200);
			KT_ASSERT_EQ(data[199], 0x77);
			Kfree(data);
		}
		KT_ASSERT_ER(ts_set_nam(od, (CONST UB *)"by name"), E_OK);
		KT_ASSERT_ER(ts_cls_obj(od), E_OK);
	}

	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.nrec, 1);
	KT_ASSERT_EQ(robj.size, 256);
	KT_ASSERT_EQ(robj.name[0], (UB)'b');

	/* the reference count and the garbage list, through the API */
	KT_ASSERT_ER(ts_lnk_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_lst_gc(vol, NULL, buf, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);
	KT_ASSERT_ER(ts_unl_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_lst_gc(vol, NULL, buf, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);

	KT_ASSERT_ER(ts_gc_vol(vol, &taken), E_OK);
	KT_ASSERT(taken >= 1);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_NOEXS);
	KT_ASSERT_ER(ts_lst_gc(vol, NULL, buf, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);		/* the list is empty, the volume is not:
					   objects that were never referred to
					   are not garbage */

	/* several changes as one transaction */
	KT_ASSERT_ER(ts_beg_trx(vol), E_OK);
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_end_trx(vol, TRUE), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 1);

	/*
	 * Leave the volume with an object that has content, so that the
	 * host tools have something to read out of the image the run
	 * leaves behind.
	 */
	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od > 0 ) {
		data = (UB *)Kmalloc(600);
		KT_ASSERT(data != NULL);
		if ( data != NULL ) {
			for ( cnt = 0; cnt < 600; cnt++ ) {
				data[cnt] = (UB)('A' + (cnt % 26));
			}
			KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_XTAD, &recno), E_OK);
			KT_ASSERT_ER(ts_wri_rec(od, 0, 0, data, 600, &asize), E_OK);
			KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_BIN, &recno), E_OK);
			KT_ASSERT_ER(ts_wri_rec(od, 1, 0, data, 64, &asize), E_OK);
			KT_ASSERT_ER(ts_wri_res(od, TSFS_ICON_REC, 0,
						(CONST UB *)"ico", data, 40), E_OK);
			Kfree(data);
		}
		KT_ASSERT_ER(ts_cls_obj(od), E_OK);
	}

	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
}

/* a small record is kept in the object block and takes no block of its own */
LOCAL void test_inline( void )
{
	TS_UUID	uuid;
	UB	buf[200];
	UD	before;
	SZ	asize = 0;
	INT	recno = -1, i;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_XTAD, &recno), E_OK);
	before = free_blocks();
	for ( i = 0; i < 200; i++ ) buf[i] = pat(i);
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, recno, 0, buf, 200, &asize), E_OK);
	KT_ASSERT_EQ(free_blocks(), before);			/* no block taken */
	knl_memset(buf, 0, sizeof(buf));
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, recno, 0, buf, 200, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, 200);
	KT_ASSERT_EQ(buf[199], pat(199));

	/* grown past what may stay inline, it goes into a block */
	{
		UB	*big = (UB *)Kmalloc(BIG_REC);

		KT_ASSERT(big != NULL);
		if ( big != NULL ) {
			for ( i = 0; i < BIG_REC; i++ ) big[i] = pat(i + 1);
			KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, recno, 0, big, BIG_REC, &asize), E_OK);
			KT_ASSERT(free_blocks() < before);
			knl_memset(big, 0, BIG_REC);
			KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, recno, 0, big, BIG_REC, &asize), E_OK);
			KT_ASSERT_EQ((INT)asize, BIG_REC);
			KT_ASSERT_EQ(big[0], pat(1));
			KT_ASSERT_EQ(big[BIG_REC - 1], pat(BIG_REC));
			Kfree(big);
		}
	}
	KT_ASSERT_ER(ts_obj_trn_rec(bvol, &uuid, recno, 0), E_OK);
	KT_ASSERT_EQ(free_blocks(), before);			/* all given back */
	KT_ASSERT_ER(ts_obj_delete(bvol, &uuid), E_OK);
}

/*
 * Two records grown in turn cannot stay one run each: their bytes go
 * into several runs, which an extent tree lists. What was written reads
 * back, and cutting them gives every block back.
 */
LOCAL void test_extents( void )
{
	TS_UUID	uuid;
	UB	*blk;
	UD	before;
	SZ	asize = 0;
	INT	a = -1, b = -1, i, k, bad = 0;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(blk != NULL);
	if ( blk == NULL ) return;
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_BIN, &a), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_BIN, &b), E_OK);
	/* what earlier tests left to be relinked is finished first: the
	   remount below would do it, and give back the orphan tree */
	KT_ASSERT_ER(ts_obj_tidy(bvol, NULL), E_OK);
	before = free_blocks();

	/* a block of each in turn: neither can grow in place */
	for ( i = 0; i < 12; i++ ) {
		for ( k = 0; k < TSFSBLK_BLOCK_SIZE; k++ ) blk[k] = (UB)( i * 3 + k );
		KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, a, (D)i * TSFSBLK_BLOCK_SIZE, blk,
					    TSFSBLK_BLOCK_SIZE, &asize), E_OK);
		for ( k = 0; k < TSFSBLK_BLOCK_SIZE; k++ ) blk[k] = (UB)( i * 5 + k );
		KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, b, (D)i * TSFSBLK_BLOCK_SIZE, blk,
					    TSFSBLK_BLOCK_SIZE, &asize), E_OK);
	}
	for ( i = 0; i < 12; i++ ) {
		KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, a, (D)i * TSFSBLK_BLOCK_SIZE, blk,
					    TSFSBLK_BLOCK_SIZE, &asize), E_OK);
		if ( blk[7] != (UB)( i * 3 + 7 ) ) bad++;
		KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, b, (D)i * TSFSBLK_BLOCK_SIZE, blk,
					    TSFSBLK_BLOCK_SIZE, &asize), E_OK);
		if ( blk[9] != (UB)( i * 5 + 9 ) ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);

	/* the same after the volume is opened again */
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(blk); return; }
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, a, 11 * TSFSBLK_BLOCK_SIZE, blk,
				    TSFSBLK_BLOCK_SIZE, &asize), E_OK);
	KT_ASSERT_EQ(blk[1], (UB)( 11 * 3 + 1 ));

	/* cut to half a block, then to nothing: every block comes back */
	KT_ASSERT_ER(ts_obj_trn_rec(bvol, &uuid, a, 100), E_OK);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, a, 0, blk, TSFSBLK_BLOCK_SIZE, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, 100);
	KT_ASSERT_ER(ts_obj_trn_rec(bvol, &uuid, a, 0), E_OK);
	KT_ASSERT_ER(ts_obj_trn_rec(bvol, &uuid, b, 0), E_OK);
	KT_ASSERT_EQ(free_blocks(), before);
	KT_ASSERT_ER(ts_obj_delete(bvol, &uuid), E_OK);
	Kfree(blk);
}

/*
 * A record replaced whole comes back either all old or all new after a
 * cut in the power, and a replacement that becomes real gives the old
 * blocks back.
 */
LOCAL void test_replace( void )
{
	TS_UUID	uuid;
	UB	*buf;
	UD	before;
	SZ	asize = 0;
	INT	recno = -1, i, bad;

	if ( bvol <= 0 ) KT_SKIP("no volume");

	buf = (UB *)Kmalloc(BIG_REC);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &uuid), E_OK);
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &uuid, TSFS_REC_XTAD, &recno), E_OK);
	for ( i = 0; i < BIG_REC; i++ ) buf[i] = 'A';
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &uuid, recno, 0, buf, BIG_REC, &asize), E_OK);
	before = free_blocks();

	/* replaced: the new bytes, and the old blocks back once it is real */
	for ( i = 0; i < BIG_REC - 1000; i++ ) buf[i] = 'B';
	KT_ASSERT_ER(ts_obj_rpl_rec(bvol, &uuid, recno, buf, BIG_REC - 1000), E_OK);
	knl_memset(buf, 0, BIG_REC);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, recno, 0, buf, BIG_REC, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_REC - 1000);
	for ( i = 0, bad = 0; i < BIG_REC - 1000; i++ ) if ( buf[i] != 'B' ) bad++;
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(free_blocks(), before);			/* three blocks for three */

	/* the power goes before the commit: all of the old is there */
	for ( i = 0; i < BIG_REC; i++ ) buf[i] = 'C';
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_BEFORE_COMMIT), E_OK);
	KT_ASSERT_ER(ts_obj_rpl_rec(bvol, &uuid, recno, buf, BIG_REC), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(buf); return; }
	knl_memset(buf, 0, BIG_REC);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, recno, 0, buf, BIG_REC, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_REC - 1000);
	for ( i = 0, bad = 0; i < BIG_REC - 1000; i++ ) if ( buf[i] != 'B' ) bad++;
	KT_ASSERT_EQ(bad, 0);

	/* the power goes after it: all of the new is there */
	for ( i = 0; i < BIG_REC; i++ ) buf[i] = 'D';
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_AFTER_COMMIT), E_OK);
	KT_ASSERT_ER(ts_obj_rpl_rec(bvol, &uuid, recno, buf, BIG_REC), E_OK);
	KT_ASSERT_ER(ts_jrnl_fault(bvol, TSFSJ_FAULT_NONE), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(buf); return; }
	knl_memset(buf, 0, BIG_REC);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &uuid, recno, 0, buf, BIG_REC, &asize), E_OK);
	KT_ASSERT_EQ((INT)asize, BIG_REC);
	for ( i = 0, bad = 0; i < BIG_REC; i++ ) if ( buf[i] != 'D' ) bad++;
	KT_ASSERT_EQ(bad, 0);

	KT_ASSERT_ER(ts_obj_delete(bvol, &uuid), E_OK);
	Kfree(buf);
}

#define RD_TASKS	3
#define RD_ROUNDS	30
#define RD_BYTES	6000		/* two blocks: from the cache and the device */

LOCAL ID		rd_vol;
LOCAL TS_UUID		rd_uuid;
LOCAL volatile INT	rd_bad;
LOCAL ID		rd_sem;

/* One reader: the object opened and its record read whole, over and over */
LOCAL void rd_task( INT stacd, void *exinf )
{
	UB	*buf = (UB *)Kmalloc(RD_BYTES);
	SZ	asize;
	ID	od;
	INT	n, i, bad = 0;

	for ( n = 0; buf != NULL && n < RD_ROUNDS; n++ ) {
		od = ts_opn_obj(rd_vol, &rd_uuid, TFO_READ);
		if ( od <= 0 ) { bad++; continue; }
		if ( ts_rea_rec(od, 0, 0, buf, RD_BYTES, &asize) < E_OK || asize != RD_BYTES ) {
			bad++;
		} else {
			for ( i = 0; i < RD_BYTES; i++ ) {
				if ( buf[i] != pat(i) ) { bad++; break; }
			}
		}
		ts_cls_obj(od);
	}
	if ( buf == NULL ) bad++; else Kfree(buf);
	rd_bad += bad;
	tk_sig_sem(rd_sem, 1);
	tk_exd_tsk();
}

/*
 * Readers of one volume go together while a writer changes another
 * object of it: what they read is always whole.
 */
LOCAL void test_readers( void )
{
	T_COBJ	cobj;
	T_CSEM	csem;
	T_CTSK	ctsk;
	TS_UUID	other;
	UB	*data;
	SZ	asize;
	ID	od, id;
	INT	i, n;

	rd_vol = ts_opn_vol(ODEV, TSFS_STORE_BLK);
	KT_ASSERT(rd_vol > 0);
	if ( rd_vol <= 0 ) return;
	data = (UB *)Kmalloc(RD_BYTES);
	KT_ASSERT(data != NULL);
	if ( data == NULL ) { ts_cls_vol(rd_vol); return; }
	for ( i = 0; i < RD_BYTES; i++ ) data[i] = pat(i);

	cobj.json = NULL;
	cobj.jsonsz = 0;
	KT_ASSERT_ER(ts_cre_obj(rd_vol, &cobj, &rd_uuid), E_OK);
	KT_ASSERT_ER(ts_cre_obj(rd_vol, &cobj, &other), E_OK);
	od = ts_opn_obj(rd_vol, &rd_uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od > 0 ) {
		KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_BIN, &n), E_OK);
		KT_ASSERT_ER(ts_wri_rec(od, 0, 0, data, RD_BYTES, &asize), E_OK);
		ts_cls_obj(od);
	}

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO;
	csem.isemcnt = 0;
	csem.maxsem = RD_TASKS;
	rd_sem = tk_cre_sem(&csem);
	rd_bad = 0;
	for ( i = 0; i < RD_TASKS; i++ ) {
		ctsk.exinf   = NULL;
		ctsk.tskatr  = TA_HLNG | TA_RNG0;
		ctsk.task    = (FP)rd_task;
		ctsk.itskpri = KT_PRI_MAIN;
		ctsk.stksz   = 16384;
		ctsk.assprc  = 0;
		id = tk_cre_tsk(&ctsk);
		KT_ASSERT(id > 0);
		if ( id > 0 ) KT_ASSERT_ER(tk_sta_tsk(id, i), E_OK);
	}
	/* the writer, meanwhile */
	od = ts_opn_obj(rd_vol, &other, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od > 0 ) {
		KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_BIN, &n), E_OK);
		for ( i = 0; i < RD_ROUNDS; i++ ) {
			data[0] = (UB)i;
			KT_ASSERT_ER(ts_wri_rec(od, 0, 0, data, RD_BYTES, &asize), E_OK);
		}
		ts_cls_obj(od);
	}
	for ( i = 0; i < RD_TASKS; i++ ) {
		KT_ASSERT_ER(tk_wai_sem(rd_sem, 1, 60000), E_OK);
	}
	KT_ASSERT_EQ(rd_bad, 0);
	tk_del_sem(rd_sem);

	KT_ASSERT_ER(ts_del_obj(rd_vol, &rd_uuid), E_OK);
	KT_ASSERT_ER(ts_del_obj(rd_vol, &other), E_OK);
	KT_ASSERT_ER(ts_cls_vol(rd_vol), E_OK);
	Kfree(data);
}

/* ---------------------------------------------------------------- the performance goals */

#define PF_OBJS		400		/* enough for the index to have two levels */
#define PF_SEQ		( 2 * 1024 * 1024 )	/* bytes written and read in a row */
#define PF_PASS		3		/* passes; the fastest is taken */
#define PF_CHUNK	( 64 * 1024 )	/* bytes a call */

LOCAL UD pf_us( void )
{
	SYSTIM_U	t;
	UW		ofs;

	(void)tk_get_otm_u(&t, &ofs);
	return (UD)t;
}

LOCAL void pf_io( T_TSFSBLK_CACHE *c )
{
	knl_memset(c, 0, sizeof(*c));
	(void)ts_cache_blk(bvol, c);
}

/*
 * Raw speed of the device: the same bytes in requests of the same size,
 * in the second quarter of the partition. Writing there destroys the
 * volume; reading does not.
 */
LOCAL UD pf_raw( UB *buf, BOOL wr )
{
	SZ	asz;
	UD	t0;
	INT	i;
	ID	dd = tk_opn_dev((UB *)ODEV, wr ? TD_UPDATE : TD_READ);

	if ( dd <= 0 ) return 0;
	t0 = pf_us();
	for ( i = 0; i < PF_SEQ / PF_CHUNK; i++ ) {
		if ( wr ) {
			(void)tk_swri_dev(dd, (W)( 8192 + i * ( PF_CHUNK / 512 ) ), buf, PF_CHUNK, &asz);
		} else {
			(void)tk_srea_dev(dd, (W)( 8192 + i * ( PF_CHUNK / 512 ) ), buf, PF_CHUNK, &asz);
		}
	}
	if ( wr ) (void)tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asz);
	t0 = pf_us() - t0;
	tk_cls_dev(dd, 0);
	return t0;
}

/* The least of two times, one of them not yet taken (0) */
LOCAL UD pf_min( UD a, UD b )
{
	return ( a == 0 || b < a ) ? b : a;
}

/*
 * The goals of design 11.12, on a volume made for them: what can be
 * counted is held to its goal; the times are shown, and held to theirs
 * where the machine's own speed does not decide them.
 */
LOCAL void test_perf( void )
{
	T_TSFSBLK_CACHE	a, b;
	T_TSFSJRNL	j0, j1;
	TS_UUID		*ids, target, big[PF_PASS];
	UB		*buf;
	SZ		asize;
	UD		t0, raw_w = 0, raw_r = 0, fs_w = 0, fs_r = 0, mount_us, commit_us;
	INT		i, k, recno = -1;
	CONST char	*doc = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>a small document</p></document></tad>";

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * PF_OBJS);
	buf = (UB *)Kmalloc(PF_CHUNK);
	KT_ASSERT(ids != NULL && buf != NULL);
	if ( ids == NULL || buf == NULL ) return;
	for ( i = 0; i < PF_CHUNK; i++ ) buf[i] = pat(i);

	if ( bvol > 0 ) ts_obj_unmount(bvol);
	bvol = 0;
	for ( k = 0; k < PF_PASS; k++ ) raw_w = pf_min(raw_w, pf_raw(buf, TRUE));
	KT_ASSERT(ts_format_blk(ODEV, "TessronOS perf") >= E_OK);
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(ids); Kfree(buf); return; }

	/* making an object: one transaction */
	KT_ASSERT_ER(ts_jrnl_ref(bvol, &j0), E_OK);
	for ( i = 0; i < PF_OBJS; i++ ) {
		KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &ids[i]), E_OK);
	}
	KT_ASSERT_ER(ts_jrnl_ref(bvol, &j1), E_OK);
	tm_printf((UB *)"  PERF create: %d transactions for %d objects\n",
		  (INT)( j1.seq - j0.seq ), PF_OBJS);
	KT_ASSERT_EQ((INT)( j1.seq - j0.seq ), PF_OBJS);

	/* a small document in the last one */
	target = ids[PF_OBJS - 1];
	KT_ASSERT_ER(ts_obj_apd_rec(bvol, &target, TSFS_REC_XTAD, &recno), E_OK);
	KT_ASSERT_ER(ts_obj_wri_rec(bvol, &target, recno, 0, doc, s_len(doc), &asize), E_OK);

	/* the commit: what it writes, and how long it takes */
	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	KT_ASSERT_ER(ts_obj_begin(bvol), E_OK);
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &big[0]), E_OK);
	pf_io(&a);
	t0 = pf_us();
	KT_ASSERT_ER(ts_obj_end(bvol, TRUE), E_OK);
	commit_us = pf_us() - t0;
	pf_io(&b);
	tm_printf((UB *)"  PERF commit: %d writes, %d flushes, %d us\n",
		  (INT)( b.writes - a.writes ), (INT)( b.flushes - a.flushes ), (INT)commit_us);

	/* mounting with the journal empty */
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	t0 = pf_us();
	bvol = ts_obj_mount(ODEV);
	mount_us = pf_us() - t0;
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(ids); Kfree(buf); return; }
	tm_printf((UB *)"  PERF mount: %d us\n", (INT)mount_us);
	KT_ASSERT(mount_us < 100000);

	/* opening by UUID with the top of the tree in memory: the leaf and the object block */
	KT_ASSERT_ER(ts_obj_exists(bvol, &ids[0]), E_OK);	/* the root in the cache */
	pf_io(&a);
	{
		T_ROBJ	r;

		KT_ASSERT_ER(ts_obj_ref(bvol, &target, &r), E_OK);
	}
	pf_io(&b);
	tm_printf((UB *)"  PERF open: %d reads\n", (INT)( b.reads - a.reads ));
	KT_ASSERT((INT)( b.reads - a.reads ) <= 2);

	/* and its small document, with nothing more read */
	pf_io(&a);
	KT_ASSERT_ER(ts_obj_rea_rec(bvol, &target, recno, 0, buf, PF_CHUNK, &asize), E_OK);
	pf_io(&b);
	tm_printf((UB *)"  PERF small document: %d reads\n", (INT)( b.reads - a.reads ));
	KT_ASSERT_EQ((INT)( b.reads - a.reads ), 0);
	for ( i = 0; i < PF_CHUNK; i++ ) buf[i] = pat(i);

	/* in a row, against the device itself: the writes made one change,
	   as a program saving a large record does; each pass a new record */
	for ( k = 0; k < PF_PASS; k++ ) {
		if ( k > 0 ) KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &big[k]), E_OK);
		KT_ASSERT_ER(ts_obj_apd_rec(bvol, &big[k], TSFS_REC_BIN, &recno), E_OK);
		pf_io(&a);
		t0 = pf_us();
		KT_ASSERT_ER(ts_obj_begin(bvol), E_OK);
		for ( i = 0; i < PF_SEQ / PF_CHUNK; i++ ) {
			KT_ASSERT_ER(ts_obj_wri_rec(bvol, &big[k], recno, (D)i * PF_CHUNK, buf, PF_CHUNK, &asize), E_OK);
		}
		KT_ASSERT_ER(ts_obj_end(bvol, TRUE), E_OK);
		fs_w = pf_min(fs_w, pf_us() - t0);
		pf_io(&b);
		if ( k == 0 ) {
			tm_printf((UB *)"  PERF %d bytes written: %d writes, %d flushes\n", PF_SEQ,
				  (INT)( b.writes - a.writes ), (INT)( b.flushes - a.flushes ));
		}
	}
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);		/* nothing of it in the cache */
	bvol = ts_obj_mount(ODEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) { Kfree(ids); Kfree(buf); return; }
	for ( k = 0; k < PF_PASS; k++ ) {
		t0 = pf_us();
		for ( i = 0; i < PF_SEQ / PF_CHUNK; i++ ) {
			KT_ASSERT_ER(ts_obj_rea_rec(bvol, &big[k], recno, (D)i * PF_CHUNK, buf, PF_CHUNK, &asize), E_OK);
		}
		fs_r = pf_min(fs_r, pf_us() - t0);
		for ( i = 0; i < PF_CHUNK; i++ ) {
			if ( buf[i] != pat(i) ) break;
		}
		KT_ASSERT_EQ(i, PF_CHUNK);	/* the last piece read back is what was written */
		raw_r = pf_min(raw_r, pf_raw(buf, FALSE));
		for ( i = 0; i < PF_CHUNK; i++ ) buf[i] = pat(i);
	}
	tm_printf((UB *)"  PERF sequential: write %d%% of the device (%d / %d us), "
		  "read %d%% (%d / %d us)\n",
		  (INT)( fs_w > 0 ? raw_w * 100 / fs_w : 0 ), (INT)fs_w, (INT)raw_w,
		  (INT)( fs_r > 0 ? raw_r * 100 / fs_r : 0 ), (INT)fs_r, (INT)raw_r);
	/* one pass on QEMU differs from the next by half again, so the goal
	   of 80% is judged on the machine itself (design 14.8); here what a
	   request per block (5% and 17%) would fall under */
	KT_ASSERT(fs_r * 40 <= raw_r * 100);
	KT_ASSERT(fs_w * 40 <= raw_w * 100);

	if ( bvol > 0 ) ts_obj_unmount(bvol);
	bvol = 0;
	KT_ASSERT(ts_format_blk(ODEV, "TessronOS test") >= E_OK);
	bvol = ts_obj_mount(ODEV);
	Kfree(ids);
	Kfree(buf);
}

/* ---------------------------------------------------------------- cuts and the patrol */

#define CUT_OLD		9000		/* bytes of the record before */
#define CUT_NEW		12000		/* and after it is replaced */
#define CUT_POINTS	60		/* cut points tried at most */

/* A record of links to `a` (n of them) filled out to `size` with `fill` */
LOCAL INT cut_text( UB *out, CONST TS_UUID *a, INT n, INT size, UB fill )
{
	TS_UUID	to[2];
	INT	k;

	to[0] = *a;
	to[1] = *a;
	k = link_text(out, to, n);
	while ( k < size ) out[k++] = fill;
	return k;
}

/* How many links to `a` the record 0 of `d` holds now, or -1 */
LOCAL INT links_to( CONST TS_UUID *d, CONST TS_UUID *a, UB *buf, INT max, SZ *p_len )
{
	char	u[40];
	SZ	len = 0;
	INT	i, k, n = 0, m;

	*p_len = 0;
	if ( ts_obj_rea_rec(bvol, d, 0, 0, buf, max, &len) < E_OK ) {
		return ( ts_obj_exists(bvol, d) >= E_OK ) ? 0 : -1;
	}
	*p_len = len;
	ts_uuid_to_str(a, u, sizeof(u));
	m = s_len(u);
	for ( i = 0; i + m <= (INT)len; i++ ) {
		for ( k = 0; k < m && buf[i + k] == (UB)u[k]; k++ ) ;
		if ( k == m ) n++;
	}
	return n;
}

/*
 * One run of the work a cut is tried on: a record replaced whole, the
 * links counted again, a new document made, an object deleted.
 */
LOCAL void cut_work( CONST TS_UUID *d0, CONST TS_UUID *a, CONST TS_UUID *x, UB *txt )
{
	TS_UUID	d1;
	INT	len;

	len = cut_text(txt, a, 2, CUT_NEW, 'N');
	(void)ts_obj_rpl_rec(bvol, d0, 0, txt, len);
	(void)ts_obj_relink(bvol, d0);
	if ( make_doc(&d1, a, 1, txt) >= E_OK ) {
		(void)ts_obj_relink(bvol, &d1);
	}
	(void)ts_obj_delete(bvol, x);
}

/* A fresh volume with the objects the work starts from */
LOCAL ER cut_setup( TS_UUID *d0, TS_UUID *a, TS_UUID *x, UB *txt )
{
	INT	recno = -1, len;
	SZ	asize = 0;
	ER	er;

	if ( bvol > 0 ) ts_obj_unmount(bvol);
	bvol = 0;
	er = ts_format_blk(ODEV, "TessronOS cut");
	if ( er < E_OK ) return er;
	bvol = ts_obj_mount(ODEV);
	if ( bvol <= 0 ) return (ER)bvol;
	er = ts_obj_create(bvol, NULL, 0, a);
	if ( er >= E_OK ) er = ts_obj_create(bvol, NULL, 0, x);
	if ( er >= E_OK ) er = ts_obj_create(bvol, NULL, 0, d0);
	if ( er >= E_OK ) er = ts_obj_apd_rec(bvol, d0, TSFS_REC_XTAD, &recno);
	len = cut_text(txt, a, 1, CUT_OLD, 'O');
	if ( er >= E_OK ) er = ts_obj_wri_rec(bvol, d0, 0, 0, txt, len, &asize);
	if ( er >= E_OK ) er = ts_obj_relink(bvol, d0);
	if ( er >= E_OK ) er = ts_sync_blk(bvol);
	return er;
}

/*
 * The power cut after each write of the work in turn. Whatever the
 * point, the volume opens writable, the record is all old or all new,
 * the count of `a` is the links the documents on the volume hold, and
 * the patrol finds nothing damaged.
 */
LOCAL void test_cut_sweep( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSSCRUB	sc;
	TS_UUID		d0, a, x;
	TS_UUID		list[8];
	UB		*txt, *buf;
	UD		w = 0, k, step;
	SZ		len;
	INT		bad = 0, tries = 0, n, i, cnt, links;

	txt = (UB *)Kmalloc(CUT_NEW + 64);
	buf = (UB *)Kmalloc(CUT_NEW + 64);
	KT_ASSERT(txt != NULL && buf != NULL);
	if ( txt == NULL || buf == NULL ) return;

	/* how many writes the work makes */
	KT_ASSERT_ER(cut_setup(&d0, &a, &x, txt), E_OK);
	KT_ASSERT_ER(ts_cut_blk(bvol, 0, NULL), E_OK);
	cut_work(&d0, &a, &x, txt);
	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	KT_ASSERT_ER(ts_cut_blk(bvol, 0, &w), E_OK);
	KT_ASSERT(w > 10);
	step = ( w + CUT_POINTS - 1 ) / CUT_POINTS;
	tm_printf((UB *)"  the work makes %d writes; a cut every %d\n", (INT)w, (INT)step);

	for ( k = 1; k <= w; k += step ) {
		if ( cut_setup(&d0, &a, &x, txt) < E_OK ) { bad++; break; }
		ts_cut_blk(bvol, k, NULL);
		cut_work(&d0, &a, &x, txt);
		ts_obj_unmount(bvol);		/* nothing more reaches the medium */
		bvol = ts_obj_mount(ODEV);
		if ( bvol <= 0 ) { bad++; tm_printf((UB *)"  cut %d: no mount\n", (INT)k); break; }
		tries++;

		ts_ref_vol_blk(bvol, &sb);
		if ( ( sb.state & TSFSBLK_ST_ERROR ) != 0 ) {
			bad++;
			tm_printf((UB *)"  cut %d: the volume says it is damaged\n", (INT)k);
			continue;
		}
		/* the record: all old or all new */
		n = links_to(&d0, &a, buf, CUT_NEW + 64, &len);
		if ( !( ( n == 1 && len == CUT_OLD && buf[CUT_OLD - 1] == 'O' )
		     || ( n == 2 && len == CUT_NEW && buf[CUT_NEW - 1] == 'N' ) ) ) {
			bad++;
			tm_printf((UB *)"  cut %d: record of %d bytes, %d links\n", (INT)k, (INT)len, n);
			continue;
		}
		/* the count: every document's links, the new one's too if it is there */
		links = n;
		if ( ts_obj_list(bvol, NULL, list, 8, &cnt) >= E_OK ) {
			for ( i = 0; i < cnt; i++ ) {
				if ( ts_uuid_cmp(&list[i], &d0) == 0 || ts_uuid_cmp(&list[i], &a) == 0
				  || ts_uuid_cmp(&list[i], &x) == 0 ) continue;
				n = links_to(&list[i], &a, buf, CUT_NEW + 64, &len);
				if ( n > 0 ) links += n;
			}
		}
		if ( refs(&a) != links ) {
			bad++;
			tm_printf((UB *)"  cut %d: count %d, links %d\n", (INT)k, refs(&a), links);
			continue;
		}
		if ( ts_obj_scrub(bvol, TRUE, &sc) < E_OK || sc.bad != 0 ) {
			bad++;
			tm_printf((UB *)"  cut %d: the patrol found %d\n", (INT)k, (INT)sc.bad);
		}
	}
	tm_printf((UB *)"  %d cuts tried\n", tries);
	KT_ASSERT(tries > 0);
	KT_ASSERT_EQ(bad, 0);
	Kfree(txt);
	Kfree(buf);
}

/* Write one block of the device behind the volume's back */
LOCAL ER spoil_blk( UD blk )
{
	UB	*buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	SZ	asize;
	ID	dd;
	ER	er;

	if ( buf == NULL ) return E_NOMEM;
	knl_memset(buf, 0xA5, TSFSBLK_BLOCK_SIZE);
	dd = tk_opn_dev((UB *)ODEV, TD_UPDATE);
	er = ( dd > 0 ) ? tk_swri_dev(dd, (W)( blk * TSFSBLK_SECT_PER_BLK ), buf,
				       TSFSBLK_BLOCK_SIZE, &asize) : (ER)dd;
	if ( dd > 0 ) tk_cls_dev(dd, 0);
	Kfree(buf);
	return er;
}

/*
 * The patrol reads what the medium holds, past the cache: a superblock
 * and a group head spoilt there are written again from the good copies,
 * a node of the index spoilt is found and stops the volume writing.
 */
LOCAL void test_scrub( void )
{
	T_TSFSBLK_SB	sb;
	T_TSFSSCRUB	sc;
	TS_UUID		d0, a, x;
	UB		*txt;
	UD		root = 0, nodes = 0;

	txt = (UB *)Kmalloc(CUT_NEW + 64);
	KT_ASSERT(txt != NULL);
	if ( txt == NULL ) return;
	KT_ASSERT_ER(cut_setup(&d0, &a, &x, txt), E_OK);
	Kfree(txt);
	if ( bvol <= 0 ) return;

	KT_ASSERT_ER(ts_obj_scrub(bvol, TRUE, &sc), E_OK);
	KT_ASSERT_EQ(sc.bad, 0);
	KT_ASSERT(sc.checked > 10);
	tm_printf((UB *)"  %d blocks checked\n", (INT)sc.checked);

	/* the older superblock and the head of group 1 */
	KT_ASSERT_ER(ts_ref_vol_blk(bvol, &sb), E_OK);
	KT_ASSERT_ER(spoil_blk(( sb.generation + 1 ) & 1), E_OK);
	KT_ASSERT_ER(spoil_blk(sb.journal_start + sb.journal_blocks + sb.ag_blocks), E_OK);
	KT_ASSERT_ER(ts_obj_scrub(bvol, FALSE, &sc), E_OK);
	KT_ASSERT_EQ(sc.bad, 2);
	KT_ASSERT_EQ(sc.repaired, 2);
	KT_ASSERT_ER(ts_obj_scrub(bvol, FALSE, &sc), E_OK);
	KT_ASSERT_EQ(sc.bad, 0);			/* put right */
	KT_ASSERT_ER(ts_ref_vol_blk(bvol, &sb), E_OK);
	KT_ASSERT_EQ(sb.state & TSFSBLK_ST_ERROR, 0);

	/* a node of the index: no second copy of it */
	KT_ASSERT_ER(ts_get_root_blk(bvol, &root, &nodes), E_OK);
	KT_ASSERT(root != 0);
	KT_ASSERT_ER(spoil_blk(root), E_OK);
	KT_ASSERT_ER(ts_obj_scrub(bvol, FALSE, &sc), E_OK);
	KT_ASSERT(sc.bad >= 1);
	KT_ASSERT_EQ(sc.first_bad, root);
	KT_ASSERT_ER(ts_ref_vol_blk(bvol, &sb), E_OK);
	KT_ASSERT(( sb.state & TSFSBLK_ST_ERROR ) != 0);
	KT_ASSERT_ER(ts_obj_create(bvol, NULL, 0, &x), E_RONLY);

	/* made again, for the suites after this one */
	ts_obj_unmount(bvol);
	bvol = 0;
	KT_ASSERT(ts_format_blk(ODEV, "TessronOS test") >= E_OK);
}

EXPORT void ktest_tsfsobj( void )
{
	KT_RUN(test_mount);
	KT_RUN(test_object);
	KT_RUN(test_record);
	KT_RUN(test_outgrow);
	KT_RUN(test_inline);
	KT_RUN(test_extents);
	KT_RUN(test_replace);
	KT_RUN(test_order);
	KT_RUN(test_refcount);
	KT_RUN(test_collect);
	KT_RUN(test_collect_all);
	KT_RUN(test_links);
	KT_RUN(test_many_links);
	KT_RUN(test_crash);
	KT_RUN(test_api);
	KT_RUN(test_readers);
	KT_RUN(test_perf);
	KT_RUN(test_cut_sweep);
	KT_RUN(test_scrub);
}
