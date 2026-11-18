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
#include <ts/dtreq.h>
#include <ts/conf.h>
#include <ts/ob.h>
#include "../../application/calendar/cal_tad.h"
#include "../../application/desktop/desktop.h"

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

LOCAL void set_appl( T_VOBJ *v, CONST char *s )
{
	(void)tad_vobj_set(v, TAD_W_APPLIST, (CONST UB *)s, -1);
}

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
	UB		buf[512], want[512];	/* room for the whole label of these links */
	TS_TIME		t;
	TS_TM		tm;
	CONST UW	*px = NULL;
	INT		w = 0, h = 0, n;
	CONST char	*head = "仮身サンプル : 資料 (仮身一覧)";

	if ( !ready ) KT_SKIP("no records on the disk");
	v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));
	if ( v == NULL ) KT_SKIP("no memory");
	tad_vobj_clear(v);
	id_of(ST_SAMPLE, &v->target);
	v->disp = TAD_D_NAME;
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, "仮身サンプル"));

	/* the relationship, the program and the date */
	v->disp = TAD_D_NAME | TAD_D_ROLE | TAD_D_TYPE | TAD_D_UPDATE;
	(void)tad_vobj_set(v, TAD_W_REL, (CONST UB *)"資料", -1);
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
	tad_vobj_clear(v);
	id_of("019a0000-0000-7000-8000-000000000001", &v->target);
	v->disp = TAD_D_TYPE;
	set_appl(v,
		   "{&quot;basic-text-editor&quot;:{&quot;name&quot;:&quot;基本文章編集&quot;,&quot;defaultOpen&quot;:true}}");
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, " (基本文章編集)"));
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "basic-text-editor"));

	/* 起動アプリの固定: the link's own program, whatever the object says */
	tad_vobj_clear(v);
	id_of(ST_SAMPLE, &v->target);
	v->disp = TAD_D_TYPE;
	set_appl(v,
		   "{&quot;basic-figure-editor&quot;:{&quot;name&quot;:&quot;基本図形編集&quot;,"
		   "&quot;defaultOpen&quot;:true,&quot;defaultOpenLock&quot;:true}}");
	KT_ASSERT(om_store_label(v, buf, sizeof(buf)) > 0);
	KT_ASSERT(same_z(buf, " (基本図形編集)"));
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "basic-figure-editor"));
	/* not fixed: the object's own */
	set_appl(v,
		   "{&quot;basic-figure-editor&quot;:{&quot;name&quot;:&quot;基本図形編集&quot;,&quot;defaultOpen&quot;:true}}");
	KT_ASSERT(om_link_default_app(v, want, sizeof(want)) > 0);
	KT_ASSERT(same_z(want, "virtual-object-list"));

	/* the icon beside the record, 32 by 32 */
	tad_vobj_clear(v);
	id_of(ST_SAMPLE, &v->target);
	KT_ASSERT_ER(om_store_icon(v, &px, &w, &h), E_OK);
	KT_ASSERT(px != NULL && w == 32 && h == 32);
	om_store_icon_done(px);
	tad_vobj_clear(v);
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

	om_store_relink(NULL, (CONST UB *)a, la, (CONST UB *)b, lb);
	KT_ASSERT_EQ(om_store_refs(&x), rx - 1);	/* two, then one */
	KT_ASSERT_EQ(om_store_refs(&y), ry - 1);	/* one, then none */
	KT_ASSERT_EQ(om_store_refs(&z), rz + 2);	/* none, then two */

	om_store_relink(NULL, (CONST UB *)b, lb, (CONST UB *)a, la);
	KT_ASSERT_EQ(om_store_refs(&x), rx);
	KT_ASSERT_EQ(om_store_refs(&y), ry);
	KT_ASSERT_EQ(om_store_refs(&z), rz);

	/* a record that is new, and one that is gone */
	om_store_relink(NULL, NULL, 0, (CONST UB *)a, la);
	KT_ASSERT_EQ(om_store_refs(&x), rx + 2);
	om_store_relink(NULL, (CONST UB *)a, la, NULL, 0);
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
	UB	**names;
	INT	nn = 0;
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
	names = om_store_apps(&nid, &nn);
	KT_ASSERT(names != NULL && nn >= 1);
	om_apps_free(names, nn);

	/* its applist written anew (付箋指定): two programs, the second the one it opens with */
	{
		CONST UB	*ids[2] = { (CONST UB *)"basic-text-editor", (CONST UB *)"basic-figure-editor" };
		CONST UB	*nms[2] = { (CONST UB *)"基本文章編集", (CONST UB *)"基本図形編集" };
		UB		aid[OM_APP_ID];

		KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 2, 1), E_OK);
		names = om_store_apps(&nid, &nn);
		KT_ASSERT_EQ(nn, 2);
		KT_ASSERT(names != NULL && nn == 2 && names[0][0] == 0xE5 && names[1][0] == 0xE5);
		om_apps_free(names, nn);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 'f');		/* basic-figure-editor */
		KT_ASSERT(om_store_app_id(&nid, 0, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 't');
		/* the one run last is the one it opens with (not 起動固定) */
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"basic-text-editor"), E_OK);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 't');
		names = om_store_apps(&nid, &nn);
		KT_ASSERT_EQ(nn, 2);
		om_apps_free(names, nn);
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"no-such-program"), E_NOEXS);
		KT_ASSERT_ER(om_store_set_default_app(&nid, (CONST UB *)"basic-figure-editor"), E_OK);
		KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0);
		KT_ASSERT(aid[6] == 'f');
		KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 1, 0), E_OK);
		names = om_store_apps(&nid, &nn);
		KT_ASSERT_EQ(nn, 1);
		om_apps_free(names, nn);
	}

	/*
	 * A program shown under a name of 100 letters (300 bytes) with '"'
	 * and '\' in it: written escaped, read back whole
	 */
	{
		static CONST char	k3[] = "長";
		CONST UB	*ids[1] = { (CONST UB *)"basic-text-editor" };
		CONST UB	*nms[1];
		UB		*ln = (UB *)Kmalloc(400);
		INT		j, m = 0;

		KT_ASSERT(ln != NULL);
		if ( ln != NULL ) {
			for ( j = 0; j < 100; j++ ) {
				ln[m++] = (UB)k3[0]; ln[m++] = (UB)k3[1]; ln[m++] = (UB)k3[2];
			}
			ln[m++] = '"'; ln[m++] = 0x5C; ln[m] = 0;
			nms[0] = ln;
			KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 1, 0), E_OK);
			names = om_store_apps(&nid, &nn);
			KT_ASSERT(names != NULL && nn == 1 && same_z(names[0], (CONST char *)ln));
			om_apps_free(names, nn);
			Kfree(ln);
		}
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
		om_store_relink(NULL, NULL, 0, rec, rl);
		KT_ASSERT_EQ(om_store_refs(&nid), 1);
		om_store_relink(NULL, rec, rl, NULL, 0);
		KT_ASSERT_EQ(om_store_refs(&nid), 0);
	}
	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
}

/*
 * The program a double press opens an object with (design 11.3). A link
 * that does not keep to its program opens with the one 実行 ran last on
 * the object, written into the object's metadata; a link that keeps to
 * it (起動アプリの固定) opens with the one it was fixed to, whatever is
 * run from it or from another link.
 */
LOCAL BOOL opens_with( CONST T_VOBJ *v, CONST char *app )
{
	UB	got[OM_APP_ID];

	return (BOOL)( om_link_default_app(v, got, sizeof(got)) > 0 && same_z(got, app) );
}

LOCAL void test_open_app( void )
{
	CONST UB	*ids[2] = { (CONST UB *)"basic-text-editor", (CONST UB *)"basic-figure-editor" };
	CONST UB	*nms[2] = { (CONST UB *)"基本文章編集", (CONST UB *)"基本図形編集" };
	TS_UUID		base, nid;
	T_VOBJ		*v, *w;
	UB		aid[OM_APP_ID];

	if ( !ready ) KT_SKIP("no records on the disk");
	v = (T_VOBJ *)Kcalloc(2, sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	w = v + 1;
	id_of(ST_TEXT_BASE, &base);
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)"起動の試験", FALSE, &nid), E_OK);
	KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 2, 0), E_OK);
	/* two links to it with no applist of their own, as the desktop makes them */
	tad_vobj_clear(v);
	tad_vobj_clear(v + 1);
	v->target = nid;
	w->target = nid;

	/* the one run last: 実行 with another program, and a double press opens with that */
	KT_ASSERT(!om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	KT_ASSERT_ER(om_link_ran(v, (CONST UB *)"basic-figure-editor"), E_OK);
	KT_ASSERT(opens_with(v, "basic-figure-editor"));
	KT_ASSERT(opens_with(w, "basic-figure-editor"));
	KT_ASSERT_ER(om_link_ran(v, (CONST UB *)"basic-text-editor"), E_OK);
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	KT_ASSERT_ER(om_link_ran(v, (CONST UB *)"no-such-program"), E_NOEXS);
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	/* fixed: to the one it opens with now, and so it stays */
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);
	KT_ASSERT(om_link_locked(v));
	KT_ASSERT(TAD_VS(v->vapplist)[0] == '{');
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	KT_ASSERT_ER(om_link_ran(v, (CONST UB *)"basic-figure-editor"), E_OK);
	KT_ASSERT(om_store_default_app(&nid, aid, sizeof(aid)) > 0 && same_z(aid, "basic-text-editor"));
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	/* run from the other link, which is not fixed: that one follows, the fixed one does not */
	KT_ASSERT_ER(om_link_ran(w, (CONST UB *)"basic-figure-editor"), E_OK);
	KT_ASSERT(opens_with(w, "basic-figure-editor"));
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	/* let go: the object's again, the one run last */
	KT_ASSERT_ER(om_link_lock(v, FALSE), E_OK);
	KT_ASSERT(!om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-figure-editor"));

	/* a link carrying an old copy of the applist is fixed to what the object opens with now */
	set_appl(w,
		   "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
		   " \"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false,\"defaultOpenLock\":true}}");
	KT_ASSERT(!om_link_locked(w));
	KT_ASSERT_ER(om_link_lock(w, TRUE), E_OK);
	KT_ASSERT(om_link_locked(w));
	KT_ASSERT(opens_with(w, "basic-figure-editor"));
	KT_ASSERT(same_z(TAD_VS(w->vapplist),
		   "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":false},"
		   "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true,\"defaultOpenLock\":true}}"));

	/* an object whose applist names nothing, and a link with none: nothing to fix */
	KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 0, -1), E_OK);
	tad_vobj_clear(v);
	v->target = nid;
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_NOEXS);
	KT_ASSERT(!om_link_locked(v));

	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
	tad_vobj_clear(v);
	tad_vobj_clear(v + 1);
	Kfree(v);
}

/*
 * A link of a process's window (予定表, 基本表計算, マイクロカード) opened
 * by a double press: the process carries the link's applist with its
 * request (ct_link_req), the desktop gives it to the link it makes of
 * the request (dt_req_applist), and the program is decided as for a
 * link of its own -- a fixed link opens with the one it keeps to.
 */
LOCAL void test_proc_open( void )
{
	CONST UB	*ids[2] = { (CONST UB *)"basic-text-editor", (CONST UB *)"basic-figure-editor" };
	CONST UB	*nms[2] = { (CONST UB *)"基本文章編集", (CONST UB *)"基本図形編集" };
	CONST char	*fixed = "\" applist=\"{&quot;basic-text-editor&quot;:{&quot;name&quot;:&quot;基本文章編集&quot;,"
				 "&quot;defaultOpen&quot;:false},&quot;basic-figure-editor&quot;:{&quot;name&quot;:"
				 "&quot;基本図形編集&quot;,&quot;defaultOpen&quot;:true,&quot;defaultOpenLock&quot;:true}}\"/>";
	CONST char	*free_ = "\" applist=\"{&quot;basic-figure-editor&quot;:{&quot;name&quot;:&quot;基本図形編集&quot;,"
				 "&quot;defaultOpen&quot;:true}}\"/>";
	TS_UUID		base, nid;
	T_DTREQ		*rq;
	T_VOBJ		*v;
	UB		*el;
	CONST UB	*w;
	char		us[40];
	INT		n;
	SZ		cap = 1024;		/* the room of the request's tail */

	if ( !ready ) KT_SKIP("no records on the disk");
	rq = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ) + cap);
	v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));
	el = (UB *)Kmalloc(1024);
	KT_ASSERT(rq != NULL && v != NULL && el != NULL);
	if ( rq == NULL || v == NULL || el == NULL ) goto done;
	id_of(ST_TEXT_BASE, &base);
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)"プロセスの仮身の試験", FALSE, &nid), E_OK);
	KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 2, 0), E_OK);	/* the object opens with 基本文章編集 */
	(void)ts_uuid_to_str(&nid, us, sizeof(us));

	/* fixed to 基本図形編集 in the process's record: opened with that one */
	n = s_copy(el, "<link id=\"");
	n += s_copy(el + n, us);
	n += s_copy(el + n, fixed);
	knl_memset(rq, 0, sizeof(T_DTREQ));
	rq->req = DT_RQ_OPEN;
	rq->target = nid;
	KT_ASSERT(ct_link_room(el, n) <= cap);
	KT_ASSERT_ER(ct_link_req(rq, el, n, (UB *)( rq + 1 ), &rq->tailsz, cap), E_OK);
	KT_ASSERT(( rq->flags & DT_F_APPLIST ) != 0);
	w = dt_tail_get((CONST UB *)( rq + 1 ), rq->tailsz, DT_TAIL_APPLIST, NULL);
	KT_ASSERT(w != NULL && w[0] == '{' && w[1] == '"');
	tad_vobj_clear(v);
	v->target = rq->target;
	KT_ASSERT_ER(dt_req_applist(rq, v), E_OK);
	KT_ASSERT(om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-figure-editor"));
	/* 実行 from it with 基本文章編集 does not move the object's default, nor the link */
	KT_ASSERT_ER(om_link_ran(v, (CONST UB *)"basic-text-editor"), E_OK);
	KT_ASSERT(opens_with(v, "basic-figure-editor"));

	/* not fixed: the object's own, whatever the link's copy says */
	n = s_copy(el, "<link id=\"");
	n += s_copy(el + n, us);
	n += s_copy(el + n, free_);
	knl_memset(rq, 0, sizeof(T_DTREQ));
	KT_ASSERT_ER(ct_link_req(rq, el, n, (UB *)( rq + 1 ), &rq->tailsz, cap), E_OK);
	KT_ASSERT(( rq->flags & DT_F_APPLIST ) != 0);
	tad_vobj_clear(v);
	v->target = nid;
	KT_ASSERT_ER(dt_req_applist(rq, v), E_OK);
	KT_ASSERT(!om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	/* a link with no applist, and a request from a program that carries none: the object's own */
	n = s_copy(el, "<link id=\"");
	n += s_copy(el + n, us);
	n += s_copy(el + n, "\"/>");
	knl_memset(rq, 0, sizeof(T_DTREQ));
	KT_ASSERT_ER(ct_link_req(rq, el, n, (UB *)( rq + 1 ), &rq->tailsz, cap), E_OK);
	KT_ASSERT_EQ(rq->flags & DT_F_APPLIST, 0U);
	KT_ASSERT_EQ(rq->tailsz, 0U);
	tad_vobj_clear(v);
	v->target = nid;
	set_appl(v, "xxxxxxxx");	/* what was there is not left */
	KT_ASSERT_ER(dt_req_applist(rq, v), E_OK);
	KT_ASSERT_EQ(v->vapplist_len, 0);
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
done:
	if ( rq != NULL ) Kfree(rq);
	if ( v != NULL ) {
		tad_vobj_clear(v);
		Kfree(v);
	}
	if ( el != NULL ) Kfree(el);
}

/*
 * 固有データ (design 11.3): the "data" of an applist entry is given to the
 * program started as it is. The link's own entry comes first, whether
 * or not the link is fixed to its program; the object's otherwise. Data
 * longer than DT_DATA_MAX - 1 bytes is left out whole. A link fixed or
 * let go (om_link_lock) keeps its own data; a link of a process's
 * window carries it with its request. A device object's applist names
 * システム環境設定 with the device's page, and a 写し of a page has
 * only the lines of that page's table read from it (cf_copy_lines).
 */
LOCAL ER make_with( CONST char *json, TS_UUID *p_u )
{
	T_OBCRE	c;
	INT	n = 0;

	while ( json[n] != 0 ) n++;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)n;
	return ob_cre_obj(&c, p_u);
}

LOCAL BOOL has_word( CONST UB *b, SZ size, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < (INT)size; i++ ) {
		for ( k = 0; w[k] != 0 && i + k < (INT)size && b[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL void test_app_data( void )
{
	CONST char	*copy = "# システム環境設定の写し\nSCHEME\t2\nWALLPAPER\t0\nCPU_MHZ\t2200\nWATCHDOG\t30\n"
				"WIN_ROUND 6\nNET_ADAPTER\tauto\nDISPLAY.0a1b2c\ton 1024x768 right 0\n"
				"DISPLAY.zz\toff\nSIZE\t1280x800\nSYSUP\t1\nSYSTEMFACE\tx\n";
	INT		held0 = tad_vobj_held();
	T_VOBJ		*v;
	UB		*js, *big, *d, out[256], *meta;
	TS_UUID		nid;
	SZ		size = 0, dl = 0;
	INT		n, i;

	if ( !ready ) KT_SKIP("no records on the disk");
	v = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));
	js = (UB *)Kmalloc(2048);
	big = (UB *)Kmalloc(8192);
	KT_ASSERT(v != NULL && js != NULL && big != NULL);
	if ( v == NULL || js == NULL || big == NULL ) goto done;

	/* the object's data, three hundred bytes of it, given whole */
	n = s_copy(js, "{\"name\":\"固有データの試験\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		       "\"applist\":{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true,\"data\":\"");
	for ( i = 0; i < 300; i++ ) js[n++] = (UB)( 'a' + i % 26 );
	n += s_copy(js + n, "\"},\"basic-figure-editor\":{\"name\":\"基本図形編集\"}}}");
	KT_ASSERT_ER(make_with((CONST char *)js, &nid), E_OK);
	v->target = nid;
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && dl == 300 && d[299] == (UB)( 'a' + 299 % 26 ) && d[300] == 0);
	if ( d != NULL ) Kfree(d);
	/* and to no other program */
	KT_ASSERT(om_link_app_data(v, (CONST UB *)"basic-figure-editor", &dl) == NULL && dl == 0);

	/* the link's own first, fixed or not; its quotes and backslashes read */
	(void)tad_vobj_set(v, TAD_W_APPLIST, (CONST UB *)
		   "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true,\"data\":\"of-link \\\"q\\\" \\\\\"}}", -1);
	KT_ASSERT(!om_link_locked(v));
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && same_z(d, "of-link \"q\" \\"));
	if ( d != NULL ) Kfree(d);
	/* fixed and let go: the link keeps its data, and opens as before */
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);
	KT_ASSERT(om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && same_z(d, "of-link \"q\" \\"));
	if ( d != NULL ) Kfree(d);
	KT_ASSERT_ER(om_link_lock(v, FALSE), E_OK);
	KT_ASSERT(!om_link_locked(v));
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && same_z(d, "of-link \"q\" \\"));
	if ( d != NULL ) Kfree(d);
	/* a link with no data of its own, fixed: the object's whole list, its data with it */
	(void)tad_vobj_set(v, TAD_W_APPLIST, NULL, 0);
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);
	KT_ASSERT(om_link_locked(v));
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && dl == 300);
	if ( d != NULL ) Kfree(d);
	/* data alone, no entry fixed: the data is the link's, the object's program opens */
	(void)tad_vobj_set(v, TAD_W_APPLIST, (CONST UB *)"{\"basic-text-editor\":{\"data\":\"only\"}}", -1);
	KT_ASSERT(!om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-text-editor"));
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && same_z(d, "only"));
	if ( d != NULL ) Kfree(d);

	/*
	 * a link's applist of several kilobytes -- a hundred programs, each with
	 * data -- fixed and let go whole, every entry's data kept
	 */
	n = s_copy(big, "{");
	for ( i = 0; i < 100; i++ ) {
		n += s_copy(big + n, ( i > 0 ) ? ",\"prog-" : "\"prog-");
		big[n++] = (UB)( '0' + i / 10 );
		big[n++] = (UB)( '0' + i % 10 );
		n += s_copy(big + n, "\":{\"name\":\"プログラム\",\"data\":\"page-");
		big[n++] = (UB)( '0' + i / 10 );
		big[n++] = (UB)( '0' + i % 10 );
		n += s_copy(big + n, "\"}");
	}
	n += s_copy(big + n, "}");
	KT_ASSERT(n > 5000);
	(void)tad_vobj_set(v, TAD_W_APPLIST, big, n);
	KT_ASSERT_EQ(v->vapplist_len, n);
	d = om_link_app_data(v, (CONST UB *)"prog-99", &dl);
	KT_ASSERT(d != NULL && same_z(d, "page-99"));
	if ( d != NULL ) Kfree(d);
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);	/* the object's list, the link's data on its entries */
	d = om_link_app_data(v, (CONST UB *)"basic-text-editor", &dl);
	KT_ASSERT(d != NULL && dl == 300);
	if ( d != NULL ) Kfree(d);
	d = om_link_app_data(v, (CONST UB *)"prog-99", &dl);		/* and the link's own entries that give data */
	KT_ASSERT(d != NULL && same_z(d, "page-99"));
	if ( d != NULL ) Kfree(d);
	ob_del_obj(&nid);
	/* an object whose list is the link's own: fixed, all of it kept */
	KT_ASSERT_ER(make_with("{\"name\":\"固有データの試験２\",\"relationship\":[],\"refCount\":0}", &nid), E_OK);
	tad_vobj_clear(v);
	v->target = nid;
	(void)tad_vobj_set(v, TAD_W_APPLIST, big, n);
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);
	KT_ASSERT(om_link_locked(v));
	KT_ASSERT(v->vapplist_len >= n);
	for ( i = 0; i < 100; i += 33 ) {
		UB	app[8] = "prog-00", want[8] = "page-00";

		app[5] = want[5] = (UB)( '0' + i / 10 );
		app[6] = want[6] = (UB)( '0' + i % 10 );
		d = om_link_app_data(v, app, &dl);
		KT_ASSERT(d != NULL && same_z(d, (CONST char *)want));
		if ( d != NULL ) Kfree(d);
	}
	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);

	/* a device: the screen's object opens システム環境設定's page of the screen */
	meta = om_obj_meta(&ob_uuid_display, &size);
	KT_ASSERT(meta != NULL);
	if ( meta != NULL ) {
		tad_vobj_clear(v);
		v->target = ob_uuid_display;
		KT_ASSERT(opens_with(v, "tessronos-sysenv"));
		d = om_link_app_data(v, (CONST UB *)"tessronos-sysenv", &dl);
		KT_ASSERT(d != NULL && same_z(d, "screen"));
		if ( d != NULL ) Kfree(d);
		Kfree(meta);
	}

	/* a 写し: the lines of the page's table, none of what is the machine's own */
	n = 0;
	while ( copy[n] != 0 ) n++;
	KT_ASSERT(cf_copy_lines((CONST UB *)copy, n, "colour", out, sizeof(out)) > 0);
	KT_ASSERT(same_z(out, "SCHEME\t2\n"));
	KT_ASSERT(cf_copy_lines((CONST UB *)copy, n, "screen", out, sizeof(out)) > 0);
	KT_ASSERT(same_z(out, "DISPLAY.0a1b2c\ton 1024x768 right 0\nSIZE\t1280x800\n"));
	KT_ASSERT(cf_copy_lines((CONST UB *)copy, n, "window", out, sizeof(out)) > 0);
	KT_ASSERT(same_z(out, "WIN_ROUND\t6\n"));
	KT_ASSERT(cf_copy_lines((CONST UB *)copy, n, "font", out, sizeof(out)) > 0);
	KT_ASSERT(same_z(out, "SYSTEMFACE\tx\n"));
	KT_ASSERT_EQ(cf_copy_lines((CONST UB *)copy, n, "power", out, sizeof(out)), 0);
	KT_ASSERT_EQ(cf_copy_lines((CONST UB *)copy, n, "cpu", out, sizeof(out)), 0);
	KT_ASSERT_EQ(cf_copy_lines((CONST UB *)copy, n, "network", out, sizeof(out)), 0);
	KT_ASSERT(!cf_copy_allowed("power", "WATCHDOG") && !cf_copy_allowed("power", "CPU_MHZ"));
	KT_ASSERT(!cf_copy_allowed("cpu", "CPU_CHORE") && !cf_copy_allowed("screen", "NET_ADAPTER"));
	KT_ASSERT(!cf_copy_allowed("screen", "SYSUP") && !cf_copy_allowed("nosuch", "SCHEME"));
	KT_ASSERT(cf_copy_allowed("keyboard", "KBTYPE") && cf_copy_allowed("sound", "SOUND_KEEP"));
	/* ユーザ環境設定's pages: their own sheet's lines, never the person's own (個人属性) */
	KT_ASSERT(cf_copy_allowed("user.pointer", "PD_SPEED") && !cf_copy_allowed("user.key", "PD_SPEED"));
	KT_ASSERT(cf_copy_allowed("user.display", "TITLE_SIZE") && cf_copy_allowed("user.editkeys", "EDIT_KEYS"));
	KT_ASSERT(!cf_copy_allowed("user.pointer", "USER_NAME") && !cf_copy_allowed("user.key", "KANA_INPUT"));
	KT_ASSERT_EQ(cf_copy_lines((CONST UB *)copy, n, "screen", out, 8), -1);

	/* a 写し opened: システム環境設定, given the page as the entry's data */
	KT_ASSERT_ER(make_with("{\"name\":\"システム環境設定（配色）の写し\",\"relationship\":[],\"refCount\":0,"
			       "\"applist\":{\"tessronos-sysenv\":{\"name\":\"システム環境設定\",\"defaultOpen\":true,"
			       "\"data\":\"colour\"}},\"tessronos\":{\"confcopy\":{\"page\":\"colour\",\"machine\":\"ktest\"}}}",
			       &nid), E_OK);
	tad_vobj_clear(v);
	v->target = nid;
	KT_ASSERT(opens_with(v, "tessronos-sysenv"));
	d = om_link_app_data(v, (CONST UB *)"tessronos-sysenv", &dl);
	KT_ASSERT(d != NULL && same_z(d, "colour"));
	if ( d != NULL ) Kfree(d);
	meta = om_obj_meta(&nid, &size);
	KT_ASSERT(meta != NULL);
	if ( meta != NULL ) {
		KT_ASSERT(has_word(meta, size, "\"confcopy\""));
		KT_ASSERT(has_word(meta, size, "\"access\""));
		Kfree(meta);
	}
	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
done:
	if ( v != NULL ) {
		tad_vobj_clear(v);
		tad_vobj_clear(v);
		Kfree(v);
	}
	if ( js != NULL ) Kfree(js);
	if ( big != NULL ) Kfree(big);
	KT_ASSERT_EQ(tad_vobj_held(), held0);
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

/*
 * Many objects shown, as cabinets of hundreds and of more than a
 * thousand links would show them: every one named and given its icon,
 * twice over (two drawings), with the time each pass takes, how many
 * entries were made room of and the memory the table holds. Then the
 * table held to 100 entries (om_store_cap, as on a machine with little
 * memory) over 300 of them: entries are made room of, each object still
 * reads back its own name (one made room of is read again, never another
 * object's), a document asked for first stays the same throughout, an
 * icon a drawing holds is not let go under it, and an object made after
 * it all opens.
 */
IMPORT void om_store_stat( INT *p_used, INT *p_docs, INT *p_max, UW *p_evicted,
			   SZ *p_bytes, SZ *p_budget );
IMPORT void om_store_cap( INT n );

#define MANY_N		1200
#define MANY_SMALL	300

LOCAL void many_name( UB *out, INT i )
{
	INT	k = s_copy(out, "多数の実身");

	out[k++] = (UB)( '0' + i / 1000 % 10 );
	out[k++] = (UB)( '0' + i / 100 % 10 );
	out[k++] = (UB)( '0' + i / 10 % 10 );
	out[k++] = (UB)( '0' + i % 10 );
	out[k] = 0;
}

/* Named and given its icon, objects 0 to n - 1, twice: the time of each pass */
LOCAL void many_passes( CONST TS_UUID *ids, INT n, CONST char *what )
{
	T_VOBJ		v = { .recno = 0 };
	UB		got[96];		/* a label: what fits is enough here, it is only drawn */
	CONST UW	*px;
	INT		i, pass, w, h, used, docs, max;
	UW		ev0, ev1;
	SZ		bytes = 0, budget = 0;
	SYSTIM		t0, t1;

	for ( pass = 0; pass < 2; pass++ ) {
		om_store_stat(&used, &docs, &max, &ev0, NULL, NULL);
		tk_get_otm(&t0);
		for ( i = 0; i < n; i++ ) {
			knl_memset(&v, 0, sizeof(v));
			v.target = ids[i];
			(void)om_store_label(&v, got, (INT)sizeof(got));
			if ( om_store_icon(&v, &px, &w, &h) >= E_OK ) om_store_icon_done(px);
		}
		tk_get_otm(&t1);
		om_store_stat(&used, &docs, &max, &ev1, &bytes, &budget);
		tm_printf((UB *)"  %s, pass %d over %d objects: %d ms, %d made room of,"
			  " %d entries of %d in use, %d KB of %d KB\n", what, pass + 1, n,
			  (INT)( t1.lo - t0.lo ), (INT)( ev1 - ev0 ), used, max,
			  (INT)( bytes / 1024 ), (INT)( budget / 1024 ));
		KT_ASSERT(bytes <= budget);
	}
}

LOCAL void test_many( void )
{
	TS_UUID		base, extra, doc;
	TS_UUID		*ids;
	T_VOBJ		v = { .recno = 0 };
	T_TAD		*held;
	UB		want[64], *got;
	CONST UW	*px, *px2;
	INT		made, i, w, h, bad = 0, used, docs, max;
	UW		ev0, ev1;

	if ( !ready ) KT_SKIP("no records on the disk");
	ids = (TS_UUID *)Kcalloc(MANY_N, sizeof(TS_UUID));
	KT_ASSERT(ids != NULL);
	if ( ids == NULL ) return;
	id_of(ST_DOC, &doc);
	held = om_store_get(&doc, 0);
	KT_ASSERT(held != NULL);
	id_of(ST_TEXT_BASE, &base);
	for ( made = 0; made < MANY_N; made++ ) {
		many_name(want, made);
		if ( om_store_copy(&base, want, FALSE, &ids[made]) < E_OK ) break;
	}
	KT_ASSERT_EQ(made, MANY_N);

	/* as large as the memory makes it */
	many_passes(ids, ( made < MANY_SMALL ) ? made : MANY_SMALL, "the whole table");
	many_passes(ids, made, "the whole table");

	/* held to 100 entries: room is made, nothing is mixed up (what was read let go first) */
	for ( i = 0; i < made; i++ ) {
		om_store_forget(&ids[i]);
	}
	om_store_cap(100);
	om_store_stat(&used, &docs, &max, &ev0, NULL, NULL);
	knl_memset(&v, 0, sizeof(v));
	v.target = ids[0];
	KT_ASSERT_ER(om_store_icon(&v, &px, &w, &h), E_OK);	/* a drawing holds the icon of the first */
	many_passes(ids, ( made < MANY_SMALL ) ? made : MANY_SMALL, "held to 100");
	for ( i = 0; i < made; i++ ) {
		many_name(want, i);
		got = om_store_name_dup(&ids[i], NULL);
		if ( got == NULL || !same_z(got, (CONST char *)want) ) {
			bad++;
		}
		if ( got != NULL ) Kfree(got);
	}
	KT_ASSERT_EQ(bad, 0);
	om_store_stat(&used, &docs, &max, &ev1, NULL, NULL);
	KT_ASSERT(ev1 > ev0);
	KT_ASSERT(used <= 100 + 16);
	/* the icon held all along is still the same pixels, given again while held */
	KT_ASSERT_ER(om_store_icon(&v, &px2, &w, &h), E_OK);
	KT_ASSERT(px2 == px);
	om_store_icon_done(px2);
	om_store_icon_done(px);
	/* the document asked for first is the same one still */
	KT_ASSERT(om_store_get(&doc, 0) == held);
	/* and an object made now opens */
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)"多数の後の実身", FALSE, &extra), E_OK);
	KT_ASSERT(om_store_get(&extra, 0) != NULL);
	KT_ASSERT_ER(om_store_delete(&extra), E_OK);
	om_store_cap(0);
	for ( i = 0; i < made; i++ ) {
		(void)om_store_delete(&ids[i]);
	}
	Kfree(ids);
}

/*
 * Pictures beside records, more of them than the picture budget holds
 * (held to 10 pictures' worth, om_store_picture_cap): each read back
 * with its own colour (one let go is read again, never another's), some
 * let go on the way, the bytes held within the budget; a picture a
 * drawing holds keeps its pixels and colour throughout, is given again
 * as the same pixels while held, and, written anew while held, the
 * drawing still reads the old pixels and the next reader the new ones.
 */
IMPORT void om_store_picture_stat( INT *p_n, SZ *p_bytes, SZ *p_budget, UW *p_evicted );
IMPORT void om_store_picture_cap( SZ bytes );

#define PIC_N		40
#define PIC_W		64

LOCAL void pic_href( UB *out, CONST TS_UUID *id )
{
	char	us[40];
	INT	k;

	ts_uuid_to_str(id, us, sizeof(us));
	k = s_copy(out, us);
	(void)s_copy(out + k, "_0_1.png");
}

LOCAL BOOL pic_is( CONST UW *px, INT w, INT h, UW colour )
{
	INT	i;

	if ( px == NULL || w != PIC_W || h != PIC_W ) return FALSE;
	for ( i = 0; i < w * h; i++ ) {
		if ( ( px[i] & 0x00ffffff ) != colour ) return FALSE;
	}
	return TRUE;
}

LOCAL void test_pictures( void )
{
	TS_UUID		base, ids[PIC_N];
	UB		href[PIC_N][64], want[64];
	UW		*fill;
	CONST UW	*px, *held, *again;
	INT		made, i, k, w, h, hw, hh, n, bad = 0, pass;
	SZ		bytes = 0, budget = 0;
	UW		ev0, ev1;

	if ( !ready ) KT_SKIP("no records on the disk");
	fill = (UW *)Kmalloc(sizeof(UW) * PIC_W * PIC_W);
	KT_ASSERT(fill != NULL);
	if ( fill == NULL ) return;
	id_of(ST_TEXT_BASE, &base);
	for ( made = 0; made < PIC_N; made++ ) {
		many_name(want, 5000 + made);
		if ( om_store_copy(&base, want, FALSE, &ids[made]) < E_OK ) break;
		pic_href(href[made], &ids[made]);
		for ( k = 0; k < PIC_W * PIC_W; k++ ) fill[k] = 0xff000000 | (UW)( made * 0x050301 + 0x102030 );
		if ( om_store_put_picture(href[made], fill, PIC_W, PIC_W) < E_OK ) break;
	}
	KT_ASSERT_EQ(made, PIC_N);
	om_store_picture_cap((SZ)PIC_W * PIC_W * 4 * 10);
	om_store_picture_stat(&n, &bytes, &budget, &ev0);

	/* the first held by a drawing all along */
	KT_ASSERT_ER(om_store_picture(href[0], &held, &hw, &hh), E_OK);
	KT_ASSERT(pic_is(held, hw, hh, 0x102030));
	for ( pass = 0; pass < 2; pass++ ) {
		for ( i = 1; i < made; i++ ) {
			if ( om_store_picture(href[i], &px, &w, &h) < E_OK ) {
				bad++;
				continue;
			}
			if ( !pic_is(px, w, h, (UW)( i * 0x050301 + 0x102030 ) ) ) bad++;
			om_store_picture_done(px);
		}
	}
	KT_ASSERT_EQ(bad, 0);
	om_store_picture_stat(&n, &bytes, &budget, &ev1);
	tm_printf((UB *)"  %d pictures read twice: %d held, %d KB of %d KB, %d let go\n", made - 1, n,
		  (INT)( bytes / 1024 ), (INT)( budget / 1024 ), (INT)( ev1 - ev0 ));
	KT_ASSERT(ev1 > ev0);
	KT_ASSERT(bytes <= budget);
	KT_ASSERT(pic_is(held, hw, hh, 0x102030));
	KT_ASSERT_ER(om_store_picture(href[0], &again, &w, &h), E_OK);
	KT_ASSERT(again == held);
	om_store_picture_done(again);

	/* written anew while held: the drawing keeps the old, the next reader gets the new */
	for ( k = 0; k < PIC_W * PIC_W; k++ ) fill[k] = 0xff00ff00;
	KT_ASSERT_ER(om_store_put_picture(href[0], fill, PIC_W, PIC_W), E_OK);
	KT_ASSERT(pic_is(held, hw, hh, 0x102030));
	KT_ASSERT_ER(om_store_picture(href[0], &again, &w, &h), E_OK);
	KT_ASSERT(again != held && pic_is(again, w, h, 0x00ff00));
	om_store_picture_done(again);
	om_store_picture_done(held);

	om_store_picture_cap(0);
	for ( i = 0; i < made; i++ ) {
		(void)om_store_delete(&ids[i]);
	}
	Kfree(fill);
}

/*
 * Names longer than any table of the store once held (64 bytes) and
 * names with what JSON escapes. A name of 100 letters of Japanese (300
 * bytes) is read whole by om_store_name, om_store_name_dup and
 * om_store_info_v; om_store_name with less room puts what fits, cut at
 * a whole letter, and still answers the whole length. A name with '"', '\', a tab, a newline
 * and a control letter is written escaped as JSON writes it and read
 * back as it was, from the cache and from the disk. The cache follows
 * every rename, and a copy (om_store_copy) keeps a long name and the
 * special letters whole.
 */
#define LN_LETTERS	100

LOCAL BOOL same_n( CONST UB *a, CONST UB *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL SZ z_len( CONST UB *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL void test_long_name( void )
{
	static CONST char	kana[] = "あいうえおかきくけこ";	/* ten letters of three bytes */
	static CONST char	odd[] = "KT\"引用\"と\\逆\tタブ\n改行\x01制御/";
	static CONST char	odd_json[] = "\"KT\\\"引用\\\"と\\\\逆\\tタブ\\n改行\\u0001制御/\"";
	TS_UUID		base, a, b, c;
	UB		*nm, *got, *meta, small[10];
	T_OMINFOV	iv;
	SZ		nl, len = 0, glen = 0;
	INT		i, r;

	if ( !ready ) KT_SKIP("no records on the disk");
	nm = (UB *)Kmalloc(LN_LETTERS * 3 + 1);
	got = (UB *)Kmalloc(LN_LETTERS * 3 + 64);
	KT_ASSERT(nm != NULL && got != NULL);
	if ( nm == NULL || got == NULL ) {
		if ( nm != NULL ) Kfree(nm);
		if ( got != NULL ) Kfree(got);
		return;
	}
	for ( i = 0; i < LN_LETTERS; i++ ) {
		knl_memcpy(nm + i * 3, kana + ( i % 10 ) * 3, 3);
	}
	nl = LN_LETTERS * 3;
	nm[nl] = 0;
	id_of(ST_TEXT_BASE, &base);

	/* made with the long name: read whole every way */
	KT_ASSERT_ER(om_store_copy(&base, nm, FALSE, &a), E_OK);
	r = om_store_name(&a, got, (INT)( nl + 64 ));
	KT_ASSERT_EQ(r, (INT)nl);
	KT_ASSERT(z_len(got) == nl && same_n(got, nm, nl));
	r = om_store_name(&a, small, (INT)sizeof(small));
	KT_ASSERT_EQ(r, (INT)nl);				/* the whole length, as snprintf answers */
	KT_ASSERT(z_len(small) == 9 && same_n(small, nm, 9));	/* three whole letters */
	om_store_forget(&a);					/* and from the disk, not the cache */
	r = om_store_name(&a, got, (INT)( nl + 64 ));
	KT_ASSERT_EQ(r, (INT)nl);
	KT_ASSERT(same_n(got, nm, nl));
	meta = om_store_name_dup(&a, &glen);
	KT_ASSERT(meta != NULL);
	if ( meta != NULL ) {
		KT_ASSERT(glen == nl && z_len(meta) == nl && same_n(meta, nm, nl));
		Kfree(meta);
	}
	KT_ASSERT_ER(om_store_info_v(&a, &iv), E_OK);
	KT_ASSERT(iv.name != NULL && z_len(iv.name) == nl && same_n(iv.name, nm, nl));
	KT_ASSERT(iv.maker != NULL && iv.made != NULL && iv.updated != NULL && iv.accessed != NULL);
	om_store_info_free(&iv);
	KT_ASSERT(iv.name == NULL);

	/* renamed to the odd name: escaped on the disk, read back as it was */
	KT_ASSERT_ER(om_store_rename(&a, (CONST UB *)odd), E_OK);
	r = om_store_name(&a, got, (INT)( nl + 64 ));
	KT_ASSERT_EQ(r, (INT)z_len((CONST UB *)odd));
	KT_ASSERT(same_z(got, odd));
	meta = om_obj_meta(&a, &len);
	KT_ASSERT(meta != NULL);
	if ( meta != NULL ) {
		KT_ASSERT(holds(meta, len, odd_json));
		Kfree(meta);
	}
	om_store_forget(&a);
	KT_ASSERT(om_store_name(&a, got, (INT)( nl + 64 )) > 0 && same_z(got, odd));
	KT_ASSERT_ER(om_store_info_v(&a, &iv), E_OK);
	KT_ASSERT(iv.name != NULL && same_z(iv.name, odd));
	om_store_info_free(&iv);

	/* a copy keeps the odd name whole */
	KT_ASSERT_ER(om_store_copy(&a, NULL, FALSE, &b), E_OK);
	KT_ASSERT(om_store_name(&b, got, (INT)( nl + 64 )) > 0 && same_z(got, odd));
	/* and one given the odd name too */
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)odd, FALSE, &c), E_OK);
	KT_ASSERT(om_store_name(&c, got, (INT)( nl + 64 )) > 0 && same_z(got, odd));
	(void)om_store_delete(&c);

	/* back to the long name: the cache follows; a copy of it keeps it whole */
	KT_ASSERT_ER(om_store_rename(&a, nm), E_OK);
	KT_ASSERT_EQ(om_store_name(&a, got, (INT)( nl + 64 )), (INT)nl);
	KT_ASSERT(same_n(got, nm, nl));
	KT_ASSERT_ER(om_store_copy(&a, NULL, FALSE, &c), E_OK);
	KT_ASSERT_EQ(om_store_name(&c, got, (INT)( nl + 64 )), (INT)nl);
	KT_ASSERT(same_n(got, nm, nl));
	(void)om_store_delete(&c);

	(void)om_store_delete(&b);
	(void)om_store_delete(&a);
	Kfree(got);
	Kfree(nm);
}

EXPORT void ktest_store( void )
{
	KT_RUN(test_long_name);
	KT_RUN(test_open);
	KT_RUN(test_links);
	KT_RUN(test_relink);
	KT_RUN(test_floor);
	KT_RUN(test_delete);
	KT_RUN(test_recount);
	KT_RUN(test_label);
	KT_RUN(test_copy_new);
	KT_RUN(test_open_app);
	KT_RUN(test_proc_open);
	KT_RUN(test_app_data);
	KT_RUN(test_many);
	KT_RUN(test_pictures);
}
