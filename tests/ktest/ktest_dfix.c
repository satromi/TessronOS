/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_dfix.c
 *	The check and repair of a native volume (ts/tsfsck.h, ディスク修復)
 *
 *	A volume is made on the scratch partition with a few objects that
 *	link to one another the way documents and cabinets do: a cabinet the
 *	superblock's root holds, two documents, one with a record that takes
 *	several blocks. Checked whole, it is found clean in every mode and a
 *	repair writes nothing. Then it is damaged on purpose, below the
 *	store: a count put wrong, blocks taken that nothing uses, a block in
 *	use marked free, a link pointed at an object that is not there, an
 *	object taken off the garbage list it belongs on, two objects that
 *	link only to each other, and an error recorded so that the volume
 *	stops writing. The check finds each; the repair puts each right; the
 *	look after it finds nothing; and every document reads as it was
 *	written. An object block that reads but does not pass its check is
 *	found, and the repair saves what reads of it into a new object in
 *	TRASH; a record placed outside the volume is found, and the repair
 *	then writes nothing. The object index and the free map damaged
 *	together are both made again from every block read. The system volume is looked at in place
 *	and found clean; repairing it is refused while it runs, and the
 *	request for the next start is carried out by what the start calls.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include "../../peripheral_kernel/fs/tsfsfmt.h"
#include "../../peripheral_kernel/obj/obj.h"

#define DEV		kt_scratch()
#define BIG		12000		/* bytes of the record that takes three blocks */
#define LEAK_N		3

LOCAL TS_UUID	u_root, u_a, u_b, u_c, u_d, u_e;
LOCAL BOOL	made = FALSE;

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != '\0' && at < max - 1 ) b[at++] = (UB)*s++;
	b[at] = '\0';
	return at;
}

LOCAL INT put_uuid( UB *b, INT at, INT max, CONST TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];

	(void)ts_uuid_to_str(u, s, sizeof(s));
	return put(b, at, max, s);
}

/* Whether the bytes `t` of `n` hold `w` */
LOCAL BOOL has( CONST UB *t, INT n, CONST char *w )
{
	INT	i, k, m = (INT)knl_strlen(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

LOCAL UB pat( INT i )
{
	return (UB)( i * 13 + ( i >> 7 ) );
}

/* A document's xmlTAD: some words, and a link to each object given */
LOCAL INT doc_text( UB *x, INT max, CONST char *name, CONST TS_UUID *to, INT nto )
{
	TS_UUID	vid;
	INT	at, i;

	at = put(x, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	at = put(x, at, max, name);
	at = put(x, at, max, "\">\n<document>\n<p>");
	at = put(x, at, max, name);
	at = put(x, at, max, " の本文</p>\n");
	for ( i = 0; i < nto; i++ ) {
		(void)ts_gen_uuid(&vid);
		at = put(x, at, max, "<link id=\"");
		at = put_uuid(x, at, max, &to[i]);
		at = put(x, at, max, "_0.xtad\" vobjid=\"");
		at = put_uuid(x, at, max, &vid);
		at = put(x, at, max, "\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"200\" vobjbottom=\"33\"/>\n");
	}
	at = put(x, at, max, "</document>\n</tad>\n");
	return at;
}

/* An object with an xmlTAD record linking to `to`, and a binary record of BIG bytes when `big` */
LOCAL ER make_doc( ID vol, CONST char *name, CONST TS_UUID *to, INT nto, BOOL big, TS_UUID *p_u )
{
	T_COBJ	c;
	UB	json[160], *x;
	INT	at, pos = 0, i;
	ID	od;
	ER	er;

	at = put(json, 0, sizeof(json), "{\"name\":\"");
	at = put(json, at, sizeof(json), name);
	(void)put(json, at, sizeof(json), "\",\"editable\":true,\"deletable\":true,\"readable\":true}");
	c.json = json;
	c.jsonsz = (SZ)knl_strlen((char *)json);
	er = ts_cre_obj(vol, &c, p_u);
	if ( er < E_OK ) return er;
	od = ts_opn_obj(vol, p_u, TFO_READ | TFO_WRITE);
	if ( od < 0 ) return od;
	x = (UB *)Kmalloc(BIG);
	if ( x == NULL ) {
		ts_cls_obj(od);
		return E_NOMEM;
	}
	at = doc_text(x, 4096, name, to, nto);
	er = ts_ins_rec(od, -1, 1, 0, &pos);
	if ( er >= E_OK ) er = ts_rpl_rec(od, pos, x, (SZ)at);
	if ( er >= E_OK && big ) {
		for ( i = 0; i < BIG; i++ ) x[i] = pat(i);
		er = ts_ins_rec(od, -1, 15, 0, &pos);
		if ( er >= E_OK ) er = ts_rpl_rec(od, pos, x, BIG);
	}
	Kfree(x);
	ts_cls_obj(od);			/* the link table is made as it closes */
	return er;
}

/* What is found told on the console while the tests run */
LOCAL INT	ev_n, ev_fixed;

LOCAL void note( void *ctx, CONST T_TSFSCKEV *ev )
{
	(void)ctx;
	if ( ev->kind == TSFSCK_K_PHASE ) return;
	ev_n++;
	if ( ev->fixed ) ev_fixed++;
	if ( ev_n <= 40 ) {
		tm_printf((UB *)"  ev ph %d kind %d%s blk %d a %d b %d\n", ev->phase, ev->kind,
			  ev->fixed ? " fixed" : "", (INT)ev->blk, (INT)ev->a, (INT)ev->b);
	}
}

LOCAL ER run( UINT mode, T_TSFSCK *r )
{
	ev_n = ev_fixed = 0;
	return ob_chk_run(DEV, mode, r, note, NULL);
}

/* ---------------------------------------------------------------- the volume */

/* a volume of linked objects is made; checked in every mode, nothing is found and nothing written */
LOCAL void test_clean( void )
{
	T_TSFSCK	r;
	T_RVOL		rv;
	TS_UUID		two[2];
	UD		bfree;
	ID		vol;

	if ( ts_format_blk(DEV, "DFIX") < E_OK ) {
		KT_SKIP("no block device to make a volume on");
	}
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(make_doc(vol, "B", NULL, 0, FALSE, &u_b), E_OK);
	KT_ASSERT_ER(make_doc(vol, "A", &u_b, 1, TRUE, &u_a), E_OK);
	two[0] = u_a;
	two[1] = u_b;
	KT_ASSERT_ER(make_doc(vol, "root", two, 2, FALSE, &u_root), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &u_root), E_OK);	/* held, as a volume's root is */
	KT_ASSERT_ER(ts_ref_vol(vol, &rv), E_OK);
	bfree = rv.bfree;
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	made = TRUE;

	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  %d objects, %d records, %d links, %d blocks read\n",
		  (INT)r.objects, (INT)r.records, (INT)r.links, (INT)r.checked);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_EQ(r.objects, 3);
	KT_ASSERT_EQ(r.links, 3);
	KT_ASSERT_ER(run(TSFSCK_QUICK, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT(r.checked > r.blocks / 2);		/* the free blocks read too */

	/* a repair of a whole volume writes nothing */
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT(!r.written);
	KT_ASSERT_EQ(ev_fixed, 0);
	KT_ASSERT_EQ(r.free, bfree);
}

/* ---------------------------------------------------------------- damage, below the store */

LOCAL UD obj_blk( ID bvol, CONST TS_UUID *u )
{
	UD	blk = 0;

	(void)ts_btree_lookup(bvol, u, &blk);
	return blk;
}

/* An object block read, changed by `fn` and sealed again, in a transaction of its own */
LOCAL ER alter( ID bvol, CONST TS_UUID *u, void (*fn)( UB *hdr ) )
{
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	UD	blk = obj_blk(bvol, u);
	ER	er;

	if ( b == NULL || blk == 0 ) {
		if ( b != NULL ) Kfree(b);
		return E_NOEXS;
	}
	er = ts_jrnl_read(bvol, blk, b);
	if ( er >= E_OK ) {
		(*fn)(b);
		ts_blk_seal(bvol, b, TSFSBLK_MAGIC_OBJ, blk,
			    (UD)b[OB_UUID + 8] | ((UD)b[OB_UUID + 9] << 8) | ((UD)b[OB_UUID + 10] << 16)
			    | ((UD)b[OB_UUID + 11] << 24) | ((UD)b[OB_UUID + 12] << 32) | ((UD)b[OB_UUID + 13] << 40)
			    | ((UD)b[OB_UUID + 14] << 48) | ((UD)b[OB_UUID + 15] << 56));
		er = ts_obj_begin(bvol);
		if ( er >= E_OK ) {
			er = ts_jrnl_write(bvol, blk, b);
			er = ts_obj_end(bvol, (BOOL)( er >= E_OK ));
		}
	}
	Kfree(b);
	return er;
}

LOCAL void count_up( UB *h )
{
	UW	n = (UW)h[OB_REFCNT] | ((UW)h[OB_REFCNT + 1] << 8);

	n += 3;
	h[OB_REFCNT] = (UB)n;
	h[OB_REFCNT + 1] = (UB)( n >> 8 );
}

/* The first link of the table, which sits in the inline area, pointed at an object that is nowhere */
LOCAL void link_away( UB *h )
{
	UW	off = (UW)h[OB_LINK + PL_A] | ((UW)h[OB_LINK + PL_A + 1] << 8);

	if ( h[OB_LINK + PL_FORM] == F_INLINE ) {
		h[off + LK_TARGET + 15] ^= 0x5A;
	}
}

/* The block of a record of `bytes` bytes, from the placement table in the inline area */
LOCAL UD record_blk( ID bvol, CONST TS_UUID *u, UD bytes )
{
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	UD	got = 0;
	UW	off, n, k;

	if ( b == NULL ) return 0;
	if ( ts_jrnl_read(bvol, obj_blk(bvol, u), b) >= E_OK && b[OB_PLACE + PL_FORM] == F_INLINE ) {
		off = (UW)b[OB_PLACE + PL_A] | ((UW)b[OB_PLACE + PL_A + 1] << 8);
		n = (UW)b[OB_NPLACE];
		for ( k = 0; k < n; k++ ) {
			CONST UB	*e = b + off + k * PE_SIZE;
			UD		len = 0, a = 0;
			INT		i;

			for ( i = 7; i >= 0; i-- ) len = ( len << 8 ) | e[PE_BYTES + i];
			for ( i = 7; i >= 0; i-- ) a = ( a << 8 ) | e[PE_PLACE + PL_A + i];
			if ( len == bytes && e[PE_PLACE + PL_FORM] == F_RUN ) got = a;
		}
	}
	Kfree(b);
	return got;
}

/* the volume damaged six ways: the check finds each, the repair puts each right, nothing is left */
LOCAL void test_repair( void )
{
	T_TSFSCK	r;
	TS_UUID		one, trash;
	UD		start = 0, got = 0, rb;
	ID		vol, bvol;
	INT		cnt = 0;

	if ( !made ) KT_SKIP("no volume");

	/* two that link only to each other, and one that went to the garbage list */
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(make_doc(vol, "C", NULL, 0, FALSE, &u_c), E_OK);
	KT_ASSERT_ER(make_doc(vol, "D", &u_c, 1, FALSE, &u_d), E_OK);
	{
		ID	od = ts_opn_obj(vol, &u_c, TFO_READ | TFO_WRITE);
		UB	*x = (UB *)Kmalloc(4096);
		INT	at;

		KT_ASSERT(od > 0 && x != NULL);
		if ( od > 0 && x != NULL ) {
			at = doc_text(x, 4096, "C", &u_d, 1);
			KT_ASSERT_ER(ts_rpl_rec(od, 0, x, (SZ)at), E_OK);
		}
		if ( x != NULL ) Kfree(x);
		if ( od > 0 ) ts_cls_obj(od);
	}
	KT_ASSERT_ER(make_doc(vol, "E", NULL, 0, FALSE, &u_e), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &u_e), E_OK);
	KT_ASSERT_ER(ts_unl_obj(vol, &u_e), E_OK);	/* nought again: on the garbage list */
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);

	/* the rest below the store */
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(alter(bvol, &u_b, count_up), E_OK);
	KT_ASSERT_ER(alter(bvol, &u_root, link_away), E_OK);
	KT_ASSERT_ER(ts_obj_begin(bvol), E_OK);
	KT_ASSERT_ER(ts_alloc_ext(bvol, LEAK_N, &start, &got), E_OK);	/* taken, used by nothing */
	KT_ASSERT_ER(ts_btree_delete_t(bvol, TSFSBT_GC, &u_e), E_OK);
	KT_ASSERT_ER(ts_obj_end(bvol, TRUE), E_OK);
	rb = record_blk(bvol, &u_a, BIG);
	KT_ASSERT(rb != 0);
	KT_ASSERT_ER(ts_obj_begin(bvol), E_OK);
	KT_ASSERT_ER(ts_free_blk(bvol, rb), E_OK);			/* in use, free in the map */
	KT_ASSERT_ER(ts_obj_end(bvol, TRUE), E_OK);
	ts_blk_error(bvol, obj_blk(bvol, &u_b), TSFSBLK_ERR_TREE);	/* it stops writing */
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);
	tm_printf((UB *)"  leaked %d at %d, record block %d made free\n", (INT)got, (INT)start, (INT)rb);

	/* the check finds each, and changes nothing */
	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  found %d: count %d gc %d link %d lost %d leak %d hidden %d super %d; unfixable %d\n",
		  r.problems, r.found[TSFSCK_K_REFCNT], r.found[TSFSCK_K_GCLIST], r.found[TSFSCK_K_LINK],
		  r.found[TSFSCK_K_LOST], r.found[TSFSCK_K_LEAK], r.found[TSFSCK_K_HIDDEN],
		  r.found[TSFSCK_K_SUPER], r.unfixable);
	KT_ASSERT(r.found[TSFSCK_K_REFCNT] >= 1);
	KT_ASSERT_EQ(r.found[TSFSCK_K_GCLIST], 1);
	KT_ASSERT_EQ(r.found[TSFSCK_K_LINK], 1);
	KT_ASSERT_EQ(r.found[TSFSCK_K_LOST], 1);
	KT_ASSERT_EQ(r.found[TSFSCK_K_LEAK], (INT)got);
	KT_ASSERT_EQ(r.found[TSFSCK_K_HIDDEN], 2);
	KT_ASSERT_EQ(r.found[TSFSCK_K_SUPER], 1);
	KT_ASSERT_EQ(r.unfixable, 0);
	KT_ASSERT(!r.written);
	KT_ASSERT_ER(run(0, &r), E_OK);
	KT_ASSERT_EQ(r.found[TSFSCK_K_LEAK], (INT)got);	/* still there: only looked at */

	/* the repair */
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  fixed: count %d gc %d link %d lost %d leak %d hidden %d super %d; left %d\n",
		  r.fixed[TSFSCK_K_REFCNT], r.fixed[TSFSCK_K_GCLIST], r.fixed[TSFSCK_K_LINK],
		  r.fixed[TSFSCK_K_LOST], r.fixed[TSFSCK_K_LEAK], r.fixed[TSFSCK_K_HIDDEN],
		  r.fixed[TSFSCK_K_SUPER], r.remaining);
	KT_ASSERT(r.written);
	KT_ASSERT(!r.refused);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT(r.fixed[TSFSCK_K_REFCNT] >= 1);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_LINK], 1);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_LOST], 1);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_LEAK], (INT)got);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_HIDDEN], 2);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_SUPER], 1);
	KT_ASSERT(r.has_trash);
	trash = r.trash;

	/* the look after it: nothing, in every mode */
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT(!r.written);

	/* what was there reads as it was written; TRASH holds the two */
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	{
		ID	od = ts_opn_obj(vol, &u_a, TFO_READ);
		UB	*x = (UB *)Kmalloc(BIG);
		SZ	asz = 0;
		INT	i, bad = 0;

		KT_ASSERT(od > 0 && x != NULL);
		if ( od > 0 && x != NULL ) {
			KT_ASSERT_ER(ts_rea_rec(od, 1, 0, x, BIG, &asz), E_OK);
			KT_ASSERT_EQ(asz, BIG);
			for ( i = 0; i < BIG; i++ ) if ( x[i] != pat(i) ) bad++;
			KT_ASSERT_EQ(bad, 0);
			KT_ASSERT_ER(ts_rea_rec(od, 0, 0, x, 4096, &asz), E_OK);
			x[asz < BIG ? asz : BIG - 1] = 0;
			KT_ASSERT(has(x, (INT)asz, "A の本文"));
		}
		if ( x != NULL ) Kfree(x);
		if ( od > 0 ) ts_cls_obj(od);
	}
	{
		T_ROBJ	ro;
		T_RLNK	lk[4];
		ID	od;

		KT_ASSERT_ER(ts_ref_obj(vol, &trash, &ro), E_OK);
		KT_ASSERT_EQ(ro.refcnt, 0);			/* a 屑実身 itself */
		od = ts_opn_obj(vol, &trash, TFO_READ);
		KT_ASSERT(od > 0);
		if ( od > 0 ) {
			KT_ASSERT_ER(ts_lst_lnk(od, lk, 4, &cnt), E_OK);
			KT_ASSERT_EQ(cnt, 2);
			ts_cls_obj(od);
		}
		KT_ASSERT_ER(ts_ref_obj(vol, &u_c, &ro), E_OK);
		KT_ASSERT_EQ(ro.refcnt, 2);			/* from D and from TRASH */
		KT_ASSERT_ER(ts_ref_obj(vol, &u_b, &ro), E_OK);
		KT_ASSERT_EQ(ro.refcnt, 2);			/* from A and from the root */
		KT_ASSERT_ER(ts_ref_obj(vol, &u_e, &ro), E_OK);
		KT_ASSERT_EQ(ro.refcnt, 0);
	}
	/* and it writes again */
	one = u_e;
	KT_ASSERT_ER(ts_del_obj(vol, &one), E_OK);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
}

LOCAL BOOL fresh_volume( void );

/* The long record of A placed past the end of the volume */
LOCAL void place_out( UB *h )
{
	UW	off = (UW)h[OB_PLACE + PL_A] | ((UW)h[OB_PLACE + PL_A + 1] << 8), n = (UW)h[OB_NPLACE], k;

	if ( h[OB_PLACE + PL_FORM] != F_INLINE ) return;
	for ( k = 0; k < n; k++ ) {
		UB	*e = h + off + k * PE_SIZE;

		if ( e[PE_PLACE + PL_FORM] == F_RUN ) {
			e[PE_PLACE + PL_A + 3] = 0x7F;	/* a block far past the end */
		}
	}
}

/* a record placed outside the volume is found, and the repair then writes nothing */
LOCAL void test_refused( void )
{
	T_TSFSCK	r;
	ID		bvol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(alter(bvol, &u_a, place_out), E_OK);
	(void)ts_obj_unmount(bvol);

	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  found %d, unfixable %d, refused %d\n", r.problems, r.unfixable, r.refused);
	KT_ASSERT(r.found[TSFSCK_K_DATA] >= 1);
	KT_ASSERT(r.refused);
	KT_ASSERT(!r.written);
	KT_ASSERT_EQ(ev_fixed, 0);
	KT_ASSERT(r.remaining > 0);
}

/* What is found on the system volume: the objects (隠れ屑実身, metadata not right) kept, to be named after the check */
#define HID_MAX		64
LOCAL TS_UUID	hid[HID_MAX];
LOCAL INT	nhid;

LOCAL void note_sys( void *ctx, CONST T_TSFSCKEV *ev )
{
	note(ctx, ev);
	if ( ev->has_uuid && ( ev->kind == TSFSCK_K_HIDDEN || ev->kind == TSFSCK_K_META ) && nhid < HID_MAX ) {
		hid[nhid++] = ev->uuid;
	}
}

/* An object found, by its name, so that whoever left it is known */
LOCAL void say_hidden( CONST TS_UUID *u )
{
	UB	*j = (UB *)Kmalloc(KT_META_ROOM + 1);
	SZ	asz = 0;
	ID	key = ob_opn_obj(u, OB_OP_ATRRD);
	INT	i = 0, at, k;
	char	nm[64], id[40];
	static CONST char w[] = "\"name\":\"";

	nm[0] = 0;
	if ( j != NULL && key > 0 && ob_get_atr(key, j, KT_META_ROOM, &asz) >= E_OK ) {
		for ( at = 0; at + 8 <= (INT)asz; at++ ) {
			for ( k = 0; k < 8 && j[at + k] == (UB)w[k]; k++ ) ;
			if ( k == 8 ) break;
		}
		for ( at += 8; at < (INT)asz && j[at] != '"' && i < (INT)sizeof(nm) - 1; at++ ) nm[i++] = (char)j[at];
		nm[i] = 0;
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( j != NULL ) Kfree(j);
	(void)ts_uuid_to_str(u, id, sizeof(id));
	tm_printf((UB *)"  found %s \"%s\"\n", id, nm);
}

/* the system volume, in use: looked at in place and found clean; its repair waits for the next start */
LOCAL void test_system( void )
{
	T_TSFSCKDEV	d[8];
	T_TSFSCK	r;
	INT		n, i, sys = -1, scr = -1;

	n = ob_chk_list(d, 8);
	if ( n > 8 ) n = 8;		/* the first 8: the volumes of the test disk come first */
	for ( i = 0; i < n; i++ ) {
		tm_printf((UB *)"  %s \"%s\" %d blocks%s%s\n", d[i].devnm, d[i].label, (INT)d[i].blocks,
			  d[i].attached ? " attached" : "", d[i].system ? " system" : "");
		if ( d[i].system ) sys = i;
		if ( knl_strcmp((char *)d[i].devnm, DEV) == 0 ) scr = i;
	}
	KT_ASSERT(scr >= 0);
	if ( scr >= 0 ) KT_ASSERT(!d[scr].attached);
	if ( sys < 0 ) {
		KT_SKIP("no system volume");
	}
	KT_ASSERT(d[sys].attached);
	ev_n = ev_fixed = 0;
	nhid = 0;
	KT_ASSERT_ER(ob_chk_run((CONST char *)d[sys].devnm, 0, &r, note_sys, NULL), E_OK);
	for ( i = 0; i < nhid; i++ ) say_hidden(&hid[i]);
	tm_printf((UB *)"  system volume: %d objects, %d links, %d found\n", (INT)r.objects, (INT)r.links,
		  r.problems);
	KT_ASSERT(r.objects > 10);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_ER(ob_chk_run((CONST char *)d[sys].devnm, TSFSCK_REPAIR, &r, note, NULL), E_BUSY);
}

/* a request for the next start, carried out as the start carries it out */
LOCAL void test_boot( void )
{
	T_TSFSCK	r;
	T_TSFSCKEV	ev[8];
	T_FSTAT		st;
	UB		dev[TSFSCK_DEVNM_MAX];
	ID		bvol;
	INT		n;

	if ( !made ) KT_SKIP("no volume");
	/* a clean volume with one count put wrong */
	KT_ASSERT_ER(ts_format_blk(DEV, "DFIX"), E_OK);
	{
		ID	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);

		KT_ASSERT(vol > 0);
		if ( vol <= 0 ) return;
		KT_ASSERT_ER(make_doc(vol, "B", NULL, 0, FALSE, &u_b), E_OK);
		KT_ASSERT_ER(make_doc(vol, "A", &u_b, 1, FALSE, &u_a), E_OK);
		KT_ASSERT_ER(ts_lnk_obj(vol, &u_a), E_OK);
		KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	}
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(alter(bvol, &u_b, count_up), E_OK);
	KT_ASSERT_ER(ts_obj_unmount(bvol), E_OK);

	KT_ASSERT_ER(ob_chk_request(DEV, 0), E_OK);
	KT_ASSERT_ER(fs_stat(TSFSCK_REQUEST, &st), E_OK);
	KT_ASSERT_ER(knl_ob_chk_boot(DEV), E_OK);
	KT_ASSERT(fs_stat(TSFSCK_REQUEST, &st) < E_OK);	/* taken away */
	n = ob_chk_boot_result(dev, sizeof(dev), &r, ev, 8);
	KT_ASSERT(n > 0);
	KT_ASSERT(knl_strcmp((char *)dev, DEV) == 0);
	KT_ASSERT_EQ(r.found[TSFSCK_K_REFCNT], 1);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_REFCNT], 1);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT_ER(run(0, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);

	/* the partition left as the other suites find it */
	(void)ts_format_blk(DEV, "DFIX");
}

/* ---------------------------------------------------------------- what is made again */

/*
 * A fresh volume: a held cabinet linking to A (a record of three blocks,
 * linking to B) and to B, and E, an object that went to the garbage list
 */
LOCAL BOOL fresh_volume( void )
{
	TS_UUID	two[2];
	ID	vol;
	BOOL	ok;

	if ( ts_format_blk(DEV, "DFIX") < E_OK ) {
		return FALSE;
	}
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	if ( vol <= 0 ) {
		return FALSE;
	}
	ok = (BOOL)( make_doc(vol, "B", NULL, 0, FALSE, &u_b) >= E_OK
		  && make_doc(vol, "A", &u_b, 1, TRUE, &u_a) >= E_OK );
	two[0] = u_a;
	two[1] = u_b;
	ok = (BOOL)( ok && make_doc(vol, "root", two, 2, FALSE, &u_root) >= E_OK
		  && ts_lnk_obj(vol, &u_root) >= E_OK
		  && make_doc(vol, "E", NULL, 0, FALSE, &u_e) >= E_OK
		  && ts_lnk_obj(vol, &u_e) >= E_OK && ts_unl_obj(vol, &u_e) >= E_OK );
	(void)ts_cls_vol(vol);
	return ok;
}

/* Whether A's long record reads as it was written */
LOCAL BOOL big_reads( ID vol )
{
	ID	od = ts_opn_obj(vol, &u_a, TFO_READ);
	UB	*x = (UB *)Kmalloc(BIG);
	SZ	asz = 0;
	INT	i, bad = 0;

	if ( od > 0 && x != NULL && ts_rea_rec(od, 1, 0, x, BIG, &asz) >= E_OK && asz == BIG ) {
		for ( i = 0; i < BIG; i++ ) if ( x[i] != pat(i) ) bad++;
	} else {
		bad = 1;
	}
	if ( x != NULL ) Kfree(x);
	if ( od > 0 ) ts_cls_obj(od);
	return (BOOL)( bad == 0 );
}

/* A block of the volume with a byte changed, so that its checksum no longer holds */
LOCAL ER spoil( UD blk, INT at )
{
	UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	ID	bvol = ts_obj_mount(DEV);
	ER	er = E_NOMEM;

	if ( b != NULL && bvol > 0 ) {
		er = ts_read_blk(bvol, blk, b);
		if ( er >= E_OK ) {
			b[at] ^= 0xFF;
			er = ts_write_blk(bvol, blk, b);
		}
	}
	if ( bvol > 0 ) (void)ts_obj_unmount(bvol);
	if ( b != NULL ) Kfree(b);
	return er;
}

/* the root of the object index damaged: the index, made again from the object blocks, finds every object */
LOCAL void test_index( void )
{
	T_TSFSCK	r;
	T_ROBJ		ro;
	UD		root = 0, nodes = 0;
	ID		vol, bvol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	(void)ts_get_root_blk(bvol, &root, &nodes);
	(void)ts_obj_unmount(bvol);
	KT_ASSERT(root != 0);
	KT_ASSERT_ER(spoil(root, TSFSBT_HDR_SIZE + 5), E_OK);

	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  index damaged: found %d (node %d), unfixable %d, %d objects found from the blocks\n",
		  r.problems, r.found[TSFSCK_K_NODE], r.unfixable, (INT)r.objects);
	KT_ASSERT(r.found[TSFSCK_K_NODE] >= 1);
	KT_ASSERT_EQ(r.unfixable, 0);
	KT_ASSERT_EQ(r.objects, 4);
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  fixed: node %d leak %d; left %d\n", r.fixed[TSFSCK_K_NODE], r.fixed[TSFSCK_K_LEAK],
		  r.remaining);
	KT_ASSERT(!r.refused);
	KT_ASSERT(r.fixed[TSFSCK_K_NODE] >= 1);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_EQ(r.objects, 4);

	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT(big_reads(vol));
	KT_ASSERT_ER(ts_ref_obj(vol, &u_b, &ro), E_OK);
	KT_ASSERT_EQ(ro.refcnt, 2);
	KT_ASSERT_ER(ts_ref_obj(vol, &u_e, &ro), E_OK);
	KT_ASSERT_EQ(ro.refcnt, 0);
	KT_ASSERT((ro.flags & TSFSO_F_GARBAGE) != 0);	/* still on the garbage list, made again too */
	KT_ASSERT_ER(ts_gc_obj(vol, &u_e), E_OK);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
}

/* a free map damaged: made again from the blocks in use; nothing in use is given out after */
LOCAL void test_map( void )
{
	T_TSFSCK	r;
	T_TSFSAGREF	g;
	ID		vol, bvol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(ts_blk_group(bvol, 0, &g), E_OK);
	(void)ts_obj_unmount(bvol);
	/* the byte of the map where the objects' blocks are marked: cleared, so they look free */
	KT_ASSERT_ER(spoil(g.map_first, (INT)( ( g.meta + 8 ) / 8 )), E_OK);

	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  map damaged: found %d (group %d), unfixable %d\n", r.problems, r.found[TSFSCK_K_GROUP],
		  r.unfixable);
	KT_ASSERT_EQ(r.found[TSFSCK_K_GROUP], 1);
	KT_ASSERT_EQ(r.unfixable, 0);
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	KT_ASSERT(!r.refused);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_GROUP], 1);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);

	/* new objects take free blocks, not those of A's record */
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(make_doc(vol, "F", NULL, 0, TRUE, &u_d), E_OK);
	KT_ASSERT(big_reads(vol));
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	KT_ASSERT_ER(run(0, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
}

/*
 * the object index and a free map damaged together: the objects found
 * by reading every block under the map, an old copy of an object given
 * up since told from the live one that took its blocks, and both made
 * again; every object reads after
 */
LOCAL void touch( UB *h )
{
	(void)h;
}

LOCAL void test_both( void )
{
	T_TSFSCK	r;
	T_TSFSAGREF	g;
	T_ROBJ		ro;
	TS_UUID		gone, two[2];
	UD		root = 0, nodes = 0;
	ID		vol, bvol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	/* an object with a long record given up, and another made over its blocks */
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(make_doc(vol, "X", NULL, 0, TRUE, &gone), E_OK);
	KT_ASSERT_ER(ts_del_obj(vol, &gone), E_OK);
	two[0] = u_a;
	two[1] = u_b;
	KT_ASSERT_ER(make_doc(vol, "Y", two, 2, TRUE, &u_c), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &u_c), E_OK);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);

	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	(void)ts_get_root_blk(bvol, &root, &nodes);
	KT_ASSERT_ER(ts_blk_group(bvol, 0, &g), E_OK);
	{
		/*
		 * An old copy of an object of another identity, as a block of an
		 * object deleted long ago would be, whose long record lies where
		 * A's is now; A written again after it
		 */
		UB	*b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		UD	at = g.first + g.meta + ( ( g.len - g.meta ) / 2 < 20000 ? ( g.len - g.meta ) / 2 : 20000 ), own = 0;
		INT	i;

		KT_ASSERT(b != NULL && g.len > g.meta + 64);
		if ( b != NULL && ts_read_blk(bvol, obj_blk(bvol, &u_a), b) >= E_OK ) {
			b[OB_UUID + 15] ^= 0x33;
			for ( i = 7; i >= 0; i-- ) own = ( own << 8 ) | b[OB_UUID + 8 + i];
			KT_ASSERT_ER(ts_blk_seal(bvol, b, TSFSBLK_MAGIC_OBJ, at, own), E_OK);
			KT_ASSERT_ER(ts_write_blk(bvol, at, b), E_OK);
		}
		if ( b != NULL ) Kfree(b);
		KT_ASSERT_ER(alter(bvol, &u_a, touch), E_OK);
		KT_ASSERT_ER(alter(bvol, &u_a, touch), E_OK);
		KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	}
	(void)ts_obj_unmount(bvol);
	KT_ASSERT(root != 0);
	KT_ASSERT_ER(spoil(root, TSFSBT_HDR_SIZE + 5), E_OK);
	KT_ASSERT_ER(spoil(g.map_first, (INT)( ( g.meta + 8 ) / 8 )), E_OK);

	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  index and map damaged: found %d (node %d, group %d), unfixable %d, %d objects\n",
		  r.problems, r.found[TSFSCK_K_NODE], r.found[TSFSCK_K_GROUP], r.unfixable, (INT)r.objects);
	KT_ASSERT(r.found[TSFSCK_K_NODE] >= 1);
	KT_ASSERT(r.found[TSFSCK_K_GROUP] >= 1);
	KT_ASSERT_EQ(r.unfixable, 0);
	KT_ASSERT(r.objects >= 5);			/* root, A, B, E and Y; X only where nothing took its blocks */
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  fixed: node %d group %d leak %d; left %d, refused %d\n", r.fixed[TSFSCK_K_NODE],
		  r.fixed[TSFSCK_K_GROUP], r.fixed[TSFSCK_K_LEAK], r.remaining, r.refused);
	KT_ASSERT(!r.refused);
	KT_ASSERT(r.fixed[TSFSCK_K_NODE] >= 1);
	KT_ASSERT(r.fixed[TSFSCK_K_GROUP] >= 1);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT(r.objects >= 5);

	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT(big_reads(vol));
	tm_printf((UB *)"  the object given up: %s\n", ( ts_ref_obj(vol, &gone, &ro) >= E_OK ) ? "found again" : "not there");
	gone = u_a;
	gone.b[15] ^= 0x33;
	KT_ASSERT_ER(ts_ref_obj(vol, &gone, &ro), E_NOEXS);	/* the old copy left out */
	KT_ASSERT_ER(ts_ref_obj(vol, &u_b, &ro), E_OK);
	KT_ASSERT_EQ(ro.refcnt, 3);			/* the root, A and Y */
	/* what is made now takes free blocks only */
	KT_ASSERT_ER(make_doc(vol, "Z", NULL, 0, TRUE, &u_d), E_OK);
	KT_ASSERT(big_reads(vol));
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
}

/* Where two runs of bytes first differ, 0 for none */
LOCAL INT bytes_cmp( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return i + 1;
	}
	return 0;
}

/* A record of an object read whole into memory, the caller's to free */
LOCAL UB *rec_of( ID vol, CONST TS_UUID *u, INT recno, SZ *p_n )
{
	ID	od = ts_opn_obj(vol, u, TFO_READ);
	UB	*x = (UB *)Kmalloc(BIG + 1);
	SZ	asz = 0;

	*p_n = 0;
	if ( od <= 0 || x == NULL || ts_rea_rec(od, recno, 0, x, BIG, &asz) < E_OK ) {
		if ( x != NULL ) Kfree(x);
		x = NULL;
	} else {
		*p_n = asz;
	}
	if ( od > 0 ) ts_cls_obj(od);
	return x;
}

/* The object TRASH's first link points at */
LOCAL BOOL first_link( ID vol, CONST TS_UUID *trash, TS_UUID *p_u )
{
	SZ	n = 0, i;
	UB	*x = rec_of(vol, trash, 0, &n);
	char	us[TS_UUID_STRLEN + 1];
	BOOL	ok = FALSE;
	INT	k;

	for ( i = 0; x != NULL && i + 10 + TS_UUID_STRLEN < n && !ok; i++ ) {
		if ( bytes_cmp(x + i, "<link id=\"", 10) != 0 ) continue;
		for ( k = 0; k < TS_UUID_STRLEN; k++ ) us[k] = (char)x[i + 10 + k];
		us[TS_UUID_STRLEN] = 0;
		ok = (BOOL)( ts_str_to_uuid(us, p_u) >= E_OK );
	}
	if ( x != NULL ) Kfree(x);
	return ok;
}

/*
 * an object block that reads but does not pass its check: its records,
 * read whole, are saved into a new object in TRASH, and its UUID names
 * nothing after
 */
LOCAL void test_salvage( void )
{
	T_TSFSCK	r;
	T_ROBJ		ro;
	TS_UUID		nu, trash;
	UB		*t0, *t1, *b;
	SZ		n0 = 0, n1 = 0, m0 = 0, m1 = 0;
	UD		blk;
	ID		vol, bvol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(make_doc(vol, "S", &u_b, 1, TRUE, &u_c), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &u_c), E_OK);
	t0 = rec_of(vol, &u_c, 0, &n0);
	t1 = rec_of(vol, &u_c, 1, &n1);
	KT_ASSERT(t0 != NULL && t1 != NULL && n1 == BIG);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);

	/* a byte the head does not use changed: the block reads, its checksum no longer holds */
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	blk = obj_blk(bvol, &u_c);
	b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	KT_ASSERT(b != NULL && blk != 0);
	if ( b != NULL && blk != 0 && ts_read_blk(bvol, blk, b) >= E_OK ) {
		b[OB_ICON + PL_SIZE + 40] ^= 0xFF;
		KT_ASSERT_ER(ts_write_blk(bvol, blk, b), E_OK);
	}
	if ( b != NULL ) Kfree(b);
	(void)ts_obj_unmount(bvol);

	KT_ASSERT_ER(run(0, &r), E_OK);
	tm_printf((UB *)"  head damaged: found %d (head %d), unfixable %d\n", r.problems,
		  r.found[TSFSCK_K_HEAD], r.unfixable);
	KT_ASSERT_EQ(r.found[TSFSCK_K_HEAD], 1);
	KT_ASSERT_EQ(r.unfixable, 0);
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  fixed: head %d; trash %d; left %d, refused %d\n", r.fixed[TSFSCK_K_HEAD],
		  r.has_trash, r.remaining, r.refused);
	KT_ASSERT(!r.refused);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_HEAD], 1);
	KT_ASSERT(r.has_trash);
	KT_ASSERT_EQ(r.remaining, 0);
	trash = r.trash;
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);

	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_obj(vol, &u_c, &ro), E_NOEXS);
	KT_ASSERT(first_link(vol, &trash, &nu));
	KT_ASSERT_ER(ts_ref_obj(vol, &nu, &ro), E_OK);
	{
		UB	*s0 = rec_of(vol, &nu, 0, &m0), *s1 = rec_of(vol, &nu, 1, &m1);

		KT_ASSERT(s0 != NULL && s1 != NULL);
		KT_ASSERT_EQ(m0, n0);
		KT_ASSERT_EQ(m1, n1);
		if ( s0 != NULL && t0 != NULL && m0 == n0 ) KT_ASSERT_EQ(bytes_cmp(s0, t0, (INT)n0), 0);
		if ( s1 != NULL && t1 != NULL && m1 == n1 ) KT_ASSERT_EQ(bytes_cmp(s1, t1, (INT)n1), 0);
		if ( s0 != NULL ) Kfree(s0);
		if ( s1 != NULL ) Kfree(s1);
	}
	KT_ASSERT_ER(ts_ref_obj(vol, &u_b, &ro), E_OK);
	KT_ASSERT_EQ(ro.refcnt, 3);			/* the root, A and what S's record became */
	if ( t0 != NULL ) Kfree(t0);
	if ( t1 != NULL ) Kfree(t1);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
}

/*
 * TRASH deleted: the objects it gathered that link only to one another
 * go with it, while one they link to that a live object reaches stays
 */
LOCAL void test_cycles( void )
{
	T_TSFSCK	r;
	T_RVOL		rv;
	T_ROBJ		ro;
	TS_UUID		two[2], trash;
	UD		before;
	ID		vol;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_vol(vol, &rv), E_OK);
	before = rv.bfree;
	/* C and D link to each other, D to B as well */
	KT_ASSERT_ER(make_doc(vol, "C", NULL, 0, TRUE, &u_c), E_OK);
	two[0] = u_c;
	two[1] = u_b;
	KT_ASSERT_ER(make_doc(vol, "D", two, 2, FALSE, &u_d), E_OK);
	{
		ID	od = ts_opn_obj(vol, &u_c, TFO_READ | TFO_WRITE);
		UB	*x = (UB *)Kmalloc(4096);

		KT_ASSERT(od > 0 && x != NULL);
		if ( od > 0 && x != NULL ) {
			KT_ASSERT_ER(ts_rpl_rec(od, 0, x, (SZ)doc_text(x, 4096, "C", &u_d, 1)), E_OK);
		}
		if ( x != NULL ) Kfree(x);
		if ( od > 0 ) ts_cls_obj(od);
	}
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);

	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	KT_ASSERT_EQ(r.found[TSFSCK_K_HIDDEN], 2);
	KT_ASSERT(r.has_trash);
	trash = r.trash;

	vol = ts_opn_vol(DEV, TSFS_STORE_BLK);
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) return;
	KT_ASSERT_ER(ts_ref_obj(vol, &trash, &ro), E_OK);
	KT_ASSERT((ro.flags & TSFSO_F_TRASH) != 0);
	KT_ASSERT_ER(ts_del_obj(vol, &trash), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &u_c, &ro), E_NOEXS);
	KT_ASSERT_ER(ts_ref_obj(vol, &u_d, &ro), E_NOEXS);
	KT_ASSERT_ER(ts_ref_obj(vol, &u_b, &ro), E_OK);	/* the root reaches it */
	KT_ASSERT_EQ(ro.refcnt, 2);
	KT_ASSERT_ER(ts_ref_obj(vol, &u_e, &ro), E_OK);	/* garbage it never reached stays garbage */
	KT_ASSERT_ER(ts_ref_vol(vol, &rv), E_OK);
	tm_printf((UB *)"  blocks free %d before C and D, %d after TRASH went\n", (INT)before, (INT)rv.bfree);
	KT_ASSERT_EQ(rv.bfree, before);
	KT_ASSERT_ER(ts_cls_vol(vol), E_OK);
	KT_ASSERT_ER(run(TSFSCK_FULL, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
}

/*
 * A data volume attached with an error recorded: while an object of it
 * is open it cannot be taken off and is not repaired; closed, it is taken
 * off, repaired and put back
 */
LOCAL void test_attached( void )
{
	T_TSFSCK	r;
	ID		bvol, key;

	if ( !fresh_volume() ) KT_SKIP("no volume");
	bvol = ts_obj_mount(DEV);
	KT_ASSERT(bvol > 0);
	if ( bvol <= 0 ) return;
	KT_ASSERT_ER(alter(bvol, &u_b, count_up), E_OK);
	KT_ASSERT_ER(ts_sync_blk(bvol), E_OK);
	ts_blk_error(bvol, obj_blk(bvol, &u_b), TSFSBLK_ERR_TREE);
	(void)ts_obj_unmount(bvol);
	KT_ASSERT_ER(ob_att_vol(DEV, TSFS_STORE_BLK), E_OK);

	key = ob_opn_obj(&u_a, OB_OP_R);
	KT_ASSERT(key > 0);
	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  open: found %d, inuse %d, refused %d, detached %d\n", r.problems, r.inuse, r.refused,
		  r.detached);
	KT_ASSERT(r.inuse);
	KT_ASSERT(r.refused);
	KT_ASSERT(!r.written);
	if ( key > 0 ) ob_cls_obj(key);

	KT_ASSERT_ER(run(TSFSCK_REPAIR, &r), E_OK);
	tm_printf((UB *)"  closed: found %d, fixed count %d super %d, left %d, detached %d\n", r.problems,
		  r.fixed[TSFSCK_K_REFCNT], r.fixed[TSFSCK_K_SUPER], r.remaining, r.detached);
	KT_ASSERT(r.detached);
	KT_ASSERT(!r.refused);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_REFCNT], 1);
	KT_ASSERT_EQ(r.fixed[TSFSCK_K_SUPER], 1);
	KT_ASSERT_EQ(r.remaining, 0);
	KT_ASSERT(knl_obfile_vol_path(DEV) > 0);	/* put back */
	key = ob_opn_obj(&u_a, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key > 0 ) ob_cls_obj(key);
	KT_ASSERT_ER(run(0, &r), E_OK);
	KT_ASSERT_EQ(r.problems, 0);
	KT_ASSERT_ER(ob_det_vol(DEV), E_OK);
	(void)ts_format_blk(DEV, "DFIX");
}

EXPORT void ktest_dfix( void )
{
	KT_RUN(test_clean);
	KT_RUN(test_repair);
	KT_RUN(test_refused);
	KT_RUN(test_system);
	KT_RUN(test_boot);
	KT_RUN(test_index);
	KT_RUN(test_map);
	KT_RUN(test_both);
	KT_RUN(test_salvage);
	KT_RUN(test_cycles);
	KT_RUN(test_attached);
}
