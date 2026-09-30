/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tsfs.c
 *	Real objects on the FAT store: creation, metadata, records,
 *	reference counts, listing in creation order, and the file names a
 *	TADjs Desktop expects.
 */

#include "ktest.h"
#include <ts/tsfs.h>
#include <ts/fs.h>

#define STORE	"/boot/TSFS"

LOCAL ID	vol = 0;

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;
	while ( s[n] != '\0' ) n++;
	return n;
}

/* the store opens and reports its state */
LOCAL void test_volume( void )
{
	T_RVOL	rvol;

	vol = ts_opn_vol(STORE, 0);
	if ( vol < 0 ) {
		KT_SKIP("no file system for the store");
	}
	KT_ASSERT(vol > 0);

	/* The store lives on the disk image and keeps what earlier runs left
	   there, so start from an empty one. */
	for (;;) {
		TS_UUID	old[8];
		INT	cnt = 0, i;

		if ( ts_lst_obj(vol, NULL, old, 8, &cnt) < E_OK || cnt == 0 ) break;
		for ( i = 0; i < cnt; i++ ) {
			while ( ts_unl_obj(vol, &old[i]) >= E_OK ) ;	/* drop references */
			ts_del_obj(vol, &old[i]);
		}
	}

	KT_ASSERT_ER(ts_ref_vol(vol, &rvol), E_OK);
	KT_ASSERT(rvol.bsize > 0);
	KT_ASSERT(rvol.time_valid);		/* the clock came from the RTC */
	tm_printf((UB*)"  store: %d objects, %d blocks free\n",
		rvol.nobj, (INT)rvol.bfree);
}

/* an object is created, found again by UUID and deleted */
LOCAL void test_object( void )
{
	T_COBJ	cobj;
	T_ROBJ	robj;
	TS_UUID	uuid;
	char	us[TS_UUID_STRLEN + 1];
	UB	path[96];
	T_FSTAT	st;
	ID	od;
	INT	i, n;

	if ( vol <= 0 ) KT_SKIP("no store");

	cobj.json   = (CONST UB *)"{\"name\":\"first\",\"refCount\":0}";
	cobj.jsonsz = s_len("{\"name\":\"first\",\"refCount\":0}");
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	/* the metadata file carries the UUID as its name, as TADjs writes it */
	KT_ASSERT_ER(ts_uuid_to_str(&uuid, us, sizeof(us)), E_OK);
	n = 0;
	for ( i = 0; STORE[i] != '\0'; i++ ) path[n++] = (UB)STORE[i];
	path[n++] = '/';
	for ( i = 0; us[i] != '\0'; i++ ) path[n++] = (UB)us[i];
	path[n++] = '.'; path[n++] = 'j'; path[n++] = 's';
	path[n++] = 'o'; path[n++] = 'n'; path[n] = '\0';
	KT_ASSERT_ER(fs_stat((CONST char *)path, &st), EX_OK);
	tm_printf((UB*)"  %s: %d bytes\n", path, (INT)st.size);

	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 0);
	KT_ASSERT_EQ(robj.nrec, 0);
	KT_ASSERT_EQ(robj.name[0], (UB)'f');
	KT_ASSERT_EQ(robj.name[4], (UB)'t');

	/* opening for writing is exclusive */
	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	KT_ASSERT_EQ(ts_opn_obj(vol, &uuid, TFO_READ), E_BUSY);
	KT_ASSERT_ER(ts_cls_obj(od), E_OK);

	/* two readers are allowed */
	od = ts_opn_obj(vol, &uuid, TFO_READ);
	KT_ASSERT(od > 0);
	{
		ID od2 = ts_opn_obj(vol, &uuid, TFO_READ);
		KT_ASSERT(od2 > 0);
		KT_ASSERT_ER(ts_cls_obj(od2), E_OK);
	}
	KT_ASSERT_ER(ts_cls_obj(od), E_OK);

	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_NOEXS);
	KT_ASSERT_ER(fs_stat((CONST char *)path, &st), EX_NOENT);
}

/* records are files beside the metadata; the types come from the names */
LOCAL void test_records( void )
{
	T_COBJ	cobj;
	T_RREC	rec[4];
	T_ROBJ	robj;
	TS_UUID	uuid;
	UB	buf[64];
	SZ	asize;
	ID	od;
	INT	recno, cnt, i;

	if ( vol <= 0 ) KT_SKIP("no store");

	cobj.json = NULL;
	cobj.jsonsz = 0;
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od <= 0 ) return;

	/* record 0 is XML TAD, record 1 holds bytes */
	KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_XTAD, &recno), E_OK);
	KT_ASSERT_EQ(recno, 0);
	KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_BIN, &recno), E_OK);
	KT_ASSERT_EQ(recno, 1);

	KT_ASSERT_ER(ts_wri_rec(od, 0, 0, (CONST UB*)"<tad></tad>", 11, &asize), E_OK);
	KT_ASSERT_EQ(asize, 11);
	KT_ASSERT_ER(ts_wri_rec(od, 1, 0, (CONST UB*)"\x7f" "ELF", 4, &asize), E_OK);

	KT_ASSERT_ER(ts_lst_rec(od, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(rec[0].rectype, TSFS_REC_XTAD);
	KT_ASSERT_EQ(rec[0].size, 11);
	KT_ASSERT_EQ(rec[1].rectype, TSFS_REC_BIN);
	KT_ASSERT_EQ(rec[1].size, 4);

	/* read back, whole and from an offset */
	for ( i = 0; i < 16; i++ ) buf[i] = 0;
	KT_ASSERT_ER(ts_rea_rec(od, 0, 0, buf, sizeof(buf), &asize), E_OK);
	KT_ASSERT_EQ(asize, 11);
	KT_ASSERT_EQ(buf[0], (UB)'<');
	KT_ASSERT_ER(ts_rea_rec(od, 0, 5, buf, 4, &asize), E_OK);
	KT_ASSERT_EQ(asize, 4);
	KT_ASSERT_EQ(buf[0], (UB)'<');		/* "</tad>" starts at 5 */
	KT_ASSERT_EQ(buf[1], (UB)'/');

	/* shorten and remove */
	KT_ASSERT_ER(ts_trn_rec(od, 0, 4), E_OK);
	KT_ASSERT_ER(ts_lst_rec(od, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(rec[0].size, 4);

	KT_ASSERT_ER(ts_del_rec(od, 0), E_PAR);		/* not the last one */
	KT_ASSERT_ER(ts_del_rec(od, 1), E_OK);
	KT_ASSERT_ER(ts_lst_rec(od, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 1);

	KT_ASSERT_ER(ts_rea_rec(od, 5, 0, buf, 4, &asize), E_NOEXS);
	KT_ASSERT_ER(ts_cls_obj(od), E_OK);

	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.nrec, 1);
	KT_ASSERT_EQ(robj.size, 4);

	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OK);
}

/* the metadata is kept as it was given, and the name can be changed */
LOCAL void test_metadata( void )
{
	T_COBJ	cobj;
	T_ROBJ	robj;
	TS_UUID	uuid;
	UB	json[256];
	SZ	asize;
	ID	od;
	CONST char *src = "{\"name\":\"before\",\"refCount\":0,\"tessronos\":{\"exec\":{\"record\":1}}}";

	if ( vol <= 0 ) KT_SKIP("no store");

	cobj.json = (CONST UB *)src;
	cobj.jsonsz = s_len(src);
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od <= 0 ) return;

	/* what was written comes back byte for byte */
	KT_ASSERT_ER(ts_get_met(od, json, sizeof(json), &asize), E_OK);
	KT_ASSERT_EQ(asize, s_len(src));
	KT_ASSERT_EQ(json[0], (UB)'{');

	/* the name changes, the rest of the metadata stays */
	KT_ASSERT_ER(ts_set_nam(od, (CONST UB *)"after the change"), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.name[0], (UB)'a');
	KT_ASSERT_EQ(robj.name[5], (UB)' ');
	KT_ASSERT_EQ(robj.name[6], (UB)'t');
	KT_ASSERT_ER(ts_get_met(od, json, sizeof(json), &asize), E_OK);
	json[asize] = '\0';
	tm_printf((UB*)"  metadata: %s\n", json);

	/* the nested key of the same name is not touched */
	KT_ASSERT(asize > (SZ)s_len("{\"name\":\"after the change\""));
	KT_ASSERT_ER(ts_cls_obj(od), E_OK);
	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OK);
}

/* the reference count decides whether an object may go */
LOCAL void test_refcount( void )
{
	T_COBJ	cobj;
	T_ROBJ	robj;
	TS_UUID	uuid;

	if ( vol <= 0 ) KT_SKIP("no store");

	cobj.json = NULL;
	cobj.jsonsz = 0;
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	KT_ASSERT_ER(ts_lnk_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_lnk_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 2);

	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OBJ);		/* still referred to */

	KT_ASSERT_ER(ts_unl_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_unl_obj(vol, &uuid), E_OK);
	KT_ASSERT_ER(ts_ref_obj(vol, &uuid, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, 0);
	KT_ASSERT_ER(ts_unl_obj(vol, &uuid), E_OBJ);		/* never below zero */

	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OK);
}

/* objects come out in UUID order, which is the order they were made in */
LOCAL void test_list( void )
{
	T_COBJ	cobj;
	TS_UUID	made[5], got[8];
	INT	cnt, i;

	if ( vol <= 0 ) KT_SKIP("no store");

	cobj.json = NULL;
	cobj.jsonsz = 0;
	for ( i = 0; i < 5; i++ ) {
		KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &made[i]), E_OK);
	}
	for ( i = 1; i < 5; i++ ) {
		KT_ASSERT(ts_uuid_cmp(&made[i - 1], &made[i]) < 0);
	}

	KT_ASSERT_ER(ts_lst_obj(vol, NULL, got, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 5);
	for ( i = 0; i < cnt; i++ ) {
		KT_ASSERT_EQ(ts_uuid_cmp(&got[i], &made[i]), 0);
	}

	/* continue after the second one */
	KT_ASSERT_ER(ts_lst_obj(vol, &made[1], got, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 3);
	KT_ASSERT_EQ(ts_uuid_cmp(&got[0], &made[2]), 0);

	/* a short buffer gives the first ones */
	KT_ASSERT_ER(ts_lst_obj(vol, NULL, got, 2, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(ts_uuid_cmp(&got[0], &made[0]), 0);
	KT_ASSERT_EQ(ts_uuid_cmp(&got[1], &made[1]), 0);

	for ( i = 0; i < 5; i++ ) {
		KT_ASSERT_ER(ts_del_obj(vol, &made[i]), E_OK);
	}
	KT_ASSERT_ER(ts_lst_obj(vol, NULL, got, 8, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 0);
}

/*
 * The completion condition of the process phase: a program held in a
 * real object is started from it and its exit code comes back.
 */
LOCAL void test_run_from_object( void )
{
	T_COBJ	cobj;
	T_CPRC	cprc;
	T_PSTS	psts;
	TS_UUID	uuid;
	UB	*buf;
	SZ	asize;
	UW	arg = 55;
	INT	fd, n, recno, total = 0;
	ID	od, pid;
	CONST char *meta =
		"{\"name\":\"hello\",\"refCount\":0,"
		"\"tessronos\":{\"exec\":{\"record\":0,\"arch\":\"aarch64\"}}}";

	if ( vol <= 0 ) KT_SKIP("no store");
	if ( fs_open("/boot/HELLO.ELF", O_RDONLY) < 0 ) KT_SKIP("no program to store");

	cobj.json = (CONST UB *)meta;
	cobj.jsonsz = s_len(meta);
	KT_ASSERT_ER(ts_cre_obj(vol, &cobj, &uuid), E_OK);

	od = ts_opn_obj(vol, &uuid, TFO_READ | TFO_WRITE);
	KT_ASSERT(od > 0);
	if ( od <= 0 ) return;

	/* record 0 holds the program image */
	KT_ASSERT_ER(ts_apd_rec(od, TSFS_REC_BIN, &recno), E_OK);
	KT_ASSERT_EQ(recno, 0);

	buf = (UB *)Kmalloc(4096);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;

	fd = fs_open("/boot/HELLO.ELF", O_RDONLY);
	KT_ASSERT(fd >= 0);
	while ( fd >= 0 && (n = fs_read(fd, buf, 4096)) > 0 ) {
		KT_ASSERT_ER(ts_wri_rec(od, 0, total, buf, n, &asize), E_OK);
		total += n;
	}
	if ( fd >= 0 ) fs_close(fd);
	Kfree(buf);
	KT_ASSERT(total > 0);
	tm_printf((UB*)"  stored %d bytes as record 0\n", total);
	KT_ASSERT_ER(ts_cls_obj(od), E_OK);

	/* start it from the object */
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = &arg;
	cprc.argsz  = sizeof(arg);

	pid = ts_cre_prc_obj(vol, &uuid, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, (INT)arg);
	}

	/* an object that is not a program is refused */
	{
		T_COBJ	c2;
		TS_UUID	u2;

		c2.json = NULL;
		c2.jsonsz = 0;
		KT_ASSERT_ER(ts_cre_obj(vol, &c2, &u2), E_OK);
		KT_ASSERT_EQ(ts_cre_prc_obj(vol, &u2, &cprc), E_NOEXS);
		KT_ASSERT_ER(ts_del_obj(vol, &u2), E_OK);
	}

	KT_ASSERT_ER(ts_del_obj(vol, &uuid), E_OK);
}

EXPORT void ktest_tsfs( void )
{
	KT_RUN(test_volume);
	KT_RUN(test_object);
	KT_RUN(test_records);
	KT_RUN(test_metadata);
	KT_RUN(test_refcount);
	KT_RUN(test_list);
	KT_RUN(test_run_from_object);

	if ( vol > 0 ) {
		ts_cls_vol(vol);
		vol = 0;
	}
}
