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
	om_store_icon_done(px);
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
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * 2);
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	w = v + 1;
	id_of(ST_TEXT_BASE, &base);
	KT_ASSERT_ER(om_store_copy(&base, (CONST UB *)"起動の試験", FALSE, &nid), E_OK);
	KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 2, 0), E_OK);
	/* two links to it with no applist of their own, as the desktop makes them */
	knl_memset(v, 0, sizeof(T_VOBJ) * 2);
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
	KT_ASSERT(v->applist[0] == '{');
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
	knl_strcpy((char *)w->applist,
		   "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
		   " \"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false,\"defaultOpenLock\":true}}");
	KT_ASSERT(!om_link_locked(w));
	KT_ASSERT_ER(om_link_lock(w, TRUE), E_OK);
	KT_ASSERT(om_link_locked(w));
	KT_ASSERT(opens_with(w, "basic-figure-editor"));
	KT_ASSERT(same_z(w->applist,
		   "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":false},"
		   "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true,\"defaultOpenLock\":true}}"));

	/* an object whose applist names nothing, and a link with none: nothing to fix */
	KT_ASSERT_ER(om_store_set_apps(&nid, ids, nms, 0, -1), E_OK);
	knl_memset(v, 0, sizeof(T_VOBJ));
	v->target = nid;
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_NOEXS);
	KT_ASSERT(!om_link_locked(v));

	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
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
	char		us[40];
	INT		n, i;

	if ( !ready ) KT_SKIP("no records on the disk");
	rq = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ));
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
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
	ct_link_req(rq, el, n);
	KT_ASSERT(( rq->flags & DT_F_APPLIST ) != 0);
	KT_ASSERT(rq->text[0] == '{' && rq->text[1] == '"');
	knl_memset(v, 0, sizeof(T_VOBJ));
	v->target = rq->target;
	dt_req_applist(rq, v);
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
	ct_link_req(rq, el, n);
	KT_ASSERT(( rq->flags & DT_F_APPLIST ) != 0);
	knl_memset(v, 0, sizeof(T_VOBJ));
	v->target = nid;
	dt_req_applist(rq, v);
	KT_ASSERT(!om_link_locked(v));
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	/* a link with no applist, and a request from a program that carries none: the object's own */
	n = s_copy(el, "<link id=\"");
	n += s_copy(el + n, us);
	n += s_copy(el + n, "\"/>");
	knl_memset(rq, 0, sizeof(T_DTREQ));
	ct_link_req(rq, el, n);
	KT_ASSERT_EQ(rq->flags & DT_F_APPLIST, 0U);
	knl_memset(v, 0, sizeof(T_VOBJ));
	v->target = nid;
	for ( i = 0; i < 8; i++ ) v->applist[i] = 'x';	/* what was there is not left */
	dt_req_applist(rq, v);
	KT_ASSERT_EQ(v->applist[0], 0);
	KT_ASSERT(opens_with(v, "basic-text-editor"));

	KT_ASSERT_ER(ob_del_obj(&nid), E_OK);
done:
	if ( rq != NULL ) Kfree(rq);
	if ( v != NULL ) Kfree(v);
	if ( el != NULL ) Kfree(el);
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
	T_VOBJ		v;
	UB		got[TAD_NAME_MAX];
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
			(void)om_store_label(&v, got, TAD_NAME_MAX);
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
	T_VOBJ		v;
	T_TAD		*held;
	UB		want[TAD_NAME_MAX], got[TAD_NAME_MAX];
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
		if ( om_store_name(&ids[i], got, TAD_NAME_MAX) <= 0 || !same_z(got, (CONST char *)want) ) {
			bad++;
		}
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
	UB		href[PIC_N][64], want[TAD_NAME_MAX];
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
	KT_RUN(test_open_app);
	KT_RUN(test_proc_open);
	KT_RUN(test_many);
	KT_RUN(test_pictures);
}
