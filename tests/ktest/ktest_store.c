/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_store.c
 *	The store of objects: how many links reach each (design 16.5.17)
 *
 *	The count of links to an object is kept in its metadata and moved
 *	where links come and go: a record written back, an object copied,
 *	an object thrown away. What these tests hold to is that the count
 *	always comes back to where it was when what was done is undone --
 *	a copy thrown away, a text put back -- and that counting everything
 *	again from the records agrees with itself.
 *
 *	The objects are the records the system is given, on the test disk.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/om.h>
#include <ts/uuid.h>
#include <ts/tad.h>
#include <ts/dt.h>

#define ST_DOC		"019a6c96-e262-7dfd-a3bc-1e85d495d60d"	/* TADjsについて */
#define ST_FIG		"019aaa4e-de25-72ad-8518-ab392b7ea301"	/* 基本図形編集サンプル */
#define ST_X		"019a6c9b-e67e-7a35-a461-0d199550e4cf"	/* 実身/仮身 */
#define ST_Y		"019aab65-6bbb-7566-bd9e-5f3d96485719"	/* 閉じた仮身 */
#define ST_Z		"019aab65-c5fc-7e2c-a251-b8cebe0e7350"	/* 閉じた仮身 */

LOCAL BOOL	ready = FALSE;

LOCAL INT s_copy( UB *out, CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) {
		out[n] = (UB)s[n];
		n++;
	}
	out[n] = 0;
	return n;
}

LOCAL void id_of( CONST char *s, TS_UUID *id )
{
	(void)ts_str_to_uuid(s, id);
}

#define ST_SAMPLE	"019a4370-3819-79a2-9758-881e1e0de90a"	/* 仮身サンプル */

LOCAL BOOL same_z( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/*
 * The words of a link's band: the object's name, its relationship with
 * the link's own after it, the program it opens with, the date it was
 * last changed -- each only when the link asks -- and its icon.
 */
LOCAL void test_label( void )
{
	T_VOBJ		*v;
	UB		buf[OM_LOOK_TEXT], want[OM_LOOK_TEXT];
	TS_TIME		t;
	TS_TM		tm;
	CONST UW	*px = NULL;
	INT		w = 0, h = 0, n;
	CONST char	*head = "仮身サンプル : 資料 (仮身一覧)";

	if ( !ready ) KT_SKIP("no records on the disk");
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) KT_SKIP("no memory");
	knl_memset(v, 0, sizeof(*v));
	id_of(ST_SAMPLE, &v->target);
	v->disp = TAD_D_NAME;
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, "仮身サンプル"));

	/* the relationship, the program and the date */
	v->disp = TAD_D_NAME | TAD_D_ROLE | TAD_D_TYPE | TAD_D_UPDATE;
	knl_strcpy((char *)v->relationship, "資料");
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	/* updateDate 2026-01-25T04:43:22.199Z, in the system's time zone */
	knl_memset(&tm, 0, sizeof(tm));
	tm.tm_year = 126;  tm.tm_mon = 0;  tm.tm_mday = 25;
	tm.tm_hour = 4;  tm.tm_min = 43;  tm.tm_sec = 22;
	(void)dt_mktime(&tm, &t);
	(void)dt_localtime(&t, &tm);
	n = s_copy(want, head);
	n += dt_strftime((char *)want + n, sizeof(want) - n, " %Y/%m/%d %H:%M:%S", &tm);
	KT_ASSERT(same_z(buf, (CONST char *)want));

	/* nothing asked: nothing said */
	v->disp = TAD_D_FRAME;
	KT_ASSERT_EQ(om_store_label(v, buf, sizeof(buf)), 0);

	/* an object with no applist of its own: the program the link names */
	knl_memset(v, 0, sizeof(*v));
	id_of("019a0000-0000-7000-8000-000000000001", &v->target);
	v->disp = TAD_D_TYPE;
	knl_strcpy((char *)v->applist,
		   "{&quot;basic-text-editor&quot;:{&quot;name&quot;:&quot;基本文章編集&quot;,&quot;defaultOpen&quot;:true}}");
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, " (基本文章編集)"));
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "basic-text-editor"));

	/* 起動アプリの固定: the link's own program, whatever the object says */
	knl_memset(v, 0, sizeof(*v));
	id_of(ST_SAMPLE, &v->target);
	v->disp = TAD_D_TYPE;
	knl_strcpy((char *)v->applist,
		   "{&quot;basic-figure-editor&quot;:{&quot;name&quot;:&quot;基本図形編集&quot;,"
		   "&quot;defaultOpen&quot;:true,&quot;defaultOpenLock&quot;:true}}");
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, " (基本図形編集)"));
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "basic-figure-editor"));
	/* not fixed: the object's own */
	knl_strcpy((char *)v->applist,
		   "{&quot;basic-figure-editor&quot;:{&quot;name&quot;:&quot;基本図形編集&quot;,&quot;defaultOpen&quot;:true}}");
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "virtual-object-list"));

	/* the icon beside the record, 32 by 32 */
	knl_memset(v, 0, sizeof(*v));
	id_of(ST_SAMPLE, &v->target);
	KT_ASSERT_ER(om_store_icon(v, &px, &w, &h), E_OK);
	KT_ASSERT(px != NULL && w == 32 && h == 32);
	Kfree(v);
}

LOCAL void test_open( void )
{
	TS_UUID	id;

	om_store_files("/boot");
	id_of(ST_DOC, &id);
	if ( om_store_refs(&id) < 0 ) KT_SKIP("no records on the disk");
	ready = TRUE;

	/* every object is found by its metadata */
	KT_ASSERT(om_store_objects(NULL, 0) > 20);
}

/* the links of a record's text, one for every link */
LOCAL void test_links( void )
{
	TS_UUID	id, *ids;
	UB	*buf;
	SZ	len = 0;
	INT	n;

	if ( !ready ) KT_SKIP("no store");
	id_of(ST_FIG, &id);
	buf = om_store_read(&id, 0, &len);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	KT_ASSERT(len > 0);
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 64);
	n = om_store_links(buf, len, ids, 64);
	KT_ASSERT(n >= 1);
	{
		TS_UUID	x;

		id_of(ST_X, &x);
		KT_ASSERT_EQ(ts_uuid_cmp(&ids[0], &x), 0);
	}
	/* a record that does not exist reads as nothing */
	KT_ASSERT(om_store_read(&id, 7, &len) == NULL);
	Kfree(ids);
	Kfree(buf);
}

/*
 * A text replaced by another: each object gains or loses by what it
 * gained or lost, and putting the first text back puts every count back.
 */
LOCAL void test_relink( void )
{
	CONST char	*a = "<link id=\"" ST_X "_0.xtad\"/><link id=\"" ST_X
			     "_0.xtad\"/><link id=\"" ST_Y "_0.xtad\"/>";
	CONST char	*b = "<link id=\"" ST_X "_0.xtad\"/><link id=\"" ST_Z
			     "_0.xtad\"/><link vobjid=\"x\" id=\"" ST_Z "_0.xtad\"/>";
	TS_UUID		x, y, z;
	INT		rx, ry, rz;
	SZ		la = 0, lb = 0;

	if ( !ready ) KT_SKIP("no store");
	while ( a[la] != 0 ) la++;
	while ( b[lb] != 0 ) lb++;
	id_of(ST_X, &x);
	id_of(ST_Y, &y);
	id_of(ST_Z, &z);
	/* counts high enough that losing one is not the floor */
	om_store_bump(&x, 5);
	om_store_bump(&y, 5);
	rx = om_store_refs(&x);
	ry = om_store_refs(&y);
	rz = om_store_refs(&z);
	KT_ASSERT(rx >= 5 && ry >= 5 && rz >= 0);

	om_store_relink((CONST UB *)a, la, (CONST UB *)b, lb);
	KT_ASSERT_EQ(om_store_refs(&x), rx - 1);	/* two, then one */
	KT_ASSERT_EQ(om_store_refs(&y), ry - 1);	/* one, then none */
	KT_ASSERT_EQ(om_store_refs(&z), rz + 2);	/* none, then two */

	om_store_relink((CONST UB *)b, lb, (CONST UB *)a, la);
	KT_ASSERT_EQ(om_store_refs(&x), rx);
	KT_ASSERT_EQ(om_store_refs(&y), ry);
	KT_ASSERT_EQ(om_store_refs(&z), rz);

	/* a record that is new, and one that is gone */
	om_store_relink(NULL, 0, (CONST UB *)a, la);
	KT_ASSERT_EQ(om_store_refs(&x), rx + 2);
	om_store_relink((CONST UB *)a, la, NULL, 0);
	KT_ASSERT_EQ(om_store_refs(&x), rx);

	om_store_bump(&x, -5);
	om_store_bump(&y, -5);
}

/* a count never goes below nought */
LOCAL void test_floor( void )
{
	TS_UUID	y;
	INT	ry;

	if ( !ready ) KT_SKIP("no store");
	id_of(ST_Y, &y);
	ry = om_store_refs(&y);
	om_store_bump(&y, -1000);
	KT_ASSERT_EQ(om_store_refs(&y), 0);
	om_store_bump(&y, ry);
	KT_ASSERT_EQ(om_store_refs(&y), ry);
}

/*
 * An object copied without what it links to, then thrown away: what it
 * links to gained a link by the copy and loses it again, and nothing of
 * the copy is left.
 */
/* Whether a stretch stands in a buffer */
LOCAL BOOL holds( CONST UB *b, SZ n, CONST char *w )
{
	SZ	i, k, wl = 0;

	while ( w[wl] != 0 ) wl++;
	for ( i = 0; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && b[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return TRUE;
	}
	return FALSE;
}

#define ST_TEXT_BASE	"019a1132-762b-7b02-ba2a-a918a9b37c39"	/* 基本文章編集's template */

/*
 * A new object made from a template, as the 原紙箱 and 新たな実身に保存
 * make one: the template's metadata under its own name, the template's
 * icon, and linked to by nothing until the link put to it is counted --
 * once, when the record it is in is saved.
 */
LOCAL void test_copy_new( void )
{
	TS_UUID	base, nid;
	UB	*meta, *ic;
	SZ	len = 0, ilen = 0;
	UB	names[2][OM_APP_NAME];
	char	us[40];
	UB	rec[96];
	SZ	rl = 0;
	INT	i;

	if ( !ready ) KT_SKIP("no records on the disk");
	id_of(ST_TEXT_BASE, &base);
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)"複製の試験", FALSE, &nid), E_OK);

	/* its metadata: its own name, the template's programs and window */
	meta = om_obj_meta(&nid, &len);
	KT_ASSERT(meta != NULL);
	if ( meta != NULL ) {
		KT_ASSERT(holds(meta, len, "複製の試験"));
		KT_ASSERT(holds(meta, len, "\"window\""));
		KT_ASSERT(!holds(meta, len, "\"file\""));
		Kfree(meta);
	}
	KT_ASSERT(om_store_apps(&nid, names, 2) >= 1);

	/* its applist written anew (付箋指定): two programs, the second the one it opens with */
	{
		CONST UB	*ids[2] = { (CONST UB *)"basic-text-editor", (CONST UB *)"basic-figure-editor" };
		CONST UB	*nms[2] = { (CONST UB *)"基本文章編集", (CONST UB *)"基本図形編集" };
		UB		aid[OM_APP_ID];

		KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 2, 1), E_OK);
		KT_ASSERT_EQ(om_store_apps(&nid, names, 2), 2);
		KT_ASSERT(names[0][0] == 0xE5 && names[1][0] == 0xE5);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 'f');		/* basic-figure-editor */
		KT_ASSERT(om_store_app_id(&nid, 0, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 't');
		/* the one run last is the one it opens with (not 起動固定) */
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"basic-text-editor"), E_OK);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 't');
		KT_ASSERT_EQ(om_store_apps(&nid, names, 2), 2);
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"no-such-program"), E_NOEXS);
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"basic-figure-editor"), E_OK);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 'f');
		KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 1, 0), E_OK);
		KT_ASSERT_EQ(om_store_apps(&nid, names, 2), 1);
	}

	/* its icon, the template's */
	ic = om_obj_icon(&nid, &ilen);
	KT_ASSERT(ic != NULL && ilen > 6);
	if ( ic != NULL ) {
		KT_ASSERT(ic[0] == 0 && ic[2] == 1);
		Kfree(ic);
	}

	/* nothing links to it yet */
	KT_ASSERT_EQ(om_store_refs(&nid), 0);
	if ( !om_store_counts(&nid) ) {
		/* a record with a link to it written: one, not two */
		(void)ts_uuid_to_str(&nid, us, sizeof(us));
		rl = 0;
		for ( i = 0; "<link id=\""[i] != 0; i++ ) rec[rl++] = (UB)"<link id=\""[i];
		for ( i = 0; us[i] != 0; i++ ) rec[rl++] = (UB)us[i];
		for ( i = 0; "_0.xtad\"/>"[i] != 0; i++ ) rec[rl++] = (UB)"_0.xtad\"/>"[i];
		om_store_relink(NULL, 0, rec, rl);
		KT_ASSERT_EQ(om_store_refs(&nid), 1);
		om_store_relink(rec, rl, NULL, 0);
		KT_ASSERT_EQ(om_store_refs(&nid), 0);
	}
	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
}

LOCAL void test_delete( void )
{
	TS_UUID	src, x, cp;
	INT	rx;
	SZ	len = 0;

	if ( !ready ) KT_SKIP("no store");
	id_of(ST_FIG, &src);
	id_of(ST_X, &x);
	rx = om_store_refs(&x);
	KT_ASSERT_ER(om_store_copy(&src, (CONST UB *)"copy", FALSE, &cp), E_OK);
	KT_ASSERT_EQ(om_store_refs(&x), rx + 1);
	KT_ASSERT_EQ(om_store_refs(&cp), 0);	/* no link to it until one is saved */

	KT_ASSERT_ER(om_store_delete(&cp), E_OK);
	KT_ASSERT_EQ(om_store_refs(&x), rx);
	KT_ASSERT_EQ(om_store_refs(&cp), -1);
	KT_ASSERT(om_store_read(&cp, 0, &len) == NULL);
	KT_ASSERT_ER(om_store_delete(&cp), E_NOEXS);
}

/*
 * Every count made again from the records: done twice, the second
 * finds nothing to put right.
 */
LOCAL void test_recount( void )
{
	TS_UUID	root;
	INT	total = 0, changed = 0;

	if ( !ready ) KT_SKIP("no store");
	id_of("019a4370-3819-79a2-9758-881e1e0de90a", &root);
	KT_ASSERT_ER(om_store_recount(&root, 1, &total, &changed), E_OK);
	KT_ASSERT(total > 20);
	KT_ASSERT_ER(om_store_recount(&root, 1, &total, &changed), E_OK);
	KT_ASSERT_EQ(changed, 0);
	/* what the system holds is counted once, though nothing links to it */
	KT_ASSERT(om_store_refs(&root) >= 1);
}

EXPORT void ktest_store( void )
{
	KT_RUN(test_open);
	KT_RUN(test_links);
	KT_RUN(test_relink);
	KT_RUN(test_floor);
	KT_RUN(test_delete);
	KT_RUN(test_recount);
	KT_RUN(test_label);
	KT_RUN(test_copy_new);
}
