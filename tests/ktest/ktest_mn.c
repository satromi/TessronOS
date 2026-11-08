/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_mn.c
 *	Definitions and menus (design 18.15)
 *
 *	The JSON reader the definitions are read with; the definition files
 *	on the test disk taken in as definition objects; a menu made from a
 *	definition, with what it takes in from the system's definitions;
 *	the state of its items, set by the program and by the system; and a
 *	choice made on the screen turned back into the item chosen.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/json.h>
#include <ts/mn.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/disp.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/cabmenu.h>
#include "../../application/desktop/desktop.h"
#include "../../outer_kernel/wm/wmobj.h"

LOCAL INT	wid = 0;

LOCAL TS_UUID def_of( CONST char *id )
{
	TS_UUID	u;

	knl_memset(&u, 0, sizeof(u));
	(void)ts_str_to_uuid(id, &u);
	return u;
}

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

/* ---------------------------------------------------------------- JSON */

LOCAL CONST char js_text[] =
	" { \"name\": \"\\u3042\\uD83D\\uDE00 \\\"q\\\"\", \"n\": -42, \"ok\": true,"
	"   \"list\": [ 1, {\"a\": [2, 3]}, \"x\" ], \"none\": null, \"f\": 1.5 } ";

LOCAL void test_json( void )
{
	T_JSON	root, v, it;
	UB	s[32];
	D	n = 0;
	INT	k = 0;

	KT_ASSERT_ER(js_parse((CONST UB *)js_text, sizeof(js_text) - 1, &root), E_OK);
	KT_ASSERT_EQ(js_type(&root), JS_OBJECT);

	/* a string, its escapes made UTF-8: あ, a pair of surrogates, quotes */
	KT_ASSERT(js_get_str(&root, "name", s, sizeof(s)) > 0);
	KT_ASSERT_EQ(s[0], 0xE3);
	KT_ASSERT_EQ(s[3], 0xF0);			/* U+1F600 in four bytes */
	KT_ASSERT_EQ(s[8], '"');
	KT_ASSERT_EQ(js_get_num(&root, "n", 0), -42);
	KT_ASSERT_EQ(js_get_num(&root, "f", 0), 1);	/* the whole part */
	KT_ASSERT(js_get_bool(&root, "ok", FALSE));
	KT_ASSERT_ER(js_get(&root, "none", &v), E_OK);
	KT_ASSERT_EQ(js_type(&v), JS_NULL);
	KT_ASSERT_ER(js_get(&root, "missing", &v), E_NOEXS);
	KT_ASSERT_EQ(js_get_num(&root, "missing", 7), 7);

	/* an array walked; a value inside it looked into */
	KT_ASSERT_ER(js_get(&root, "list", &v), E_OK);
	KT_ASSERT_EQ(js_count(&v), 3);
	it.s = NULL;
	it.len = 0;
	while ( js_next(&v, &it) ) {
		if ( k == 0 ) {
			KT_ASSERT_ER(js_num(&it, &n), E_OK);
			KT_ASSERT_EQ(n, 1);
		} else if ( k == 1 ) {
			T_JSON	a;

			KT_ASSERT_ER(js_get(&it, "a", &a), E_OK);
			KT_ASSERT_EQ(js_count(&a), 2);
		}
		k++;
	}
	KT_ASSERT_EQ(k, 3);

	/* too small a room: what fits, and E_LIMIT */
	KT_ASSERT_ER(js_get_str(&root, "name", s, 4), E_LIMIT);

	/* not JSON */
	KT_ASSERT_ER(js_parse((CONST UB *)"{\"a\":}", 6, &root), E_PAR);
	KT_ASSERT_ER(js_parse((CONST UB *)"[1,2", 4, &root), E_PAR);
	KT_ASSERT_ER(js_parse((CONST UB *)"{} x", 4, &root), E_PAR);
	KT_ASSERT_ER(js_parse((CONST UB *)"\"a\nb\"", 5, &root), E_PAR);
}

/* ---------------------------------------------------------------- definitions */

/*
 * The definitions are objects in the store: a menu's record 0 is xmlTAD
 * with its <menu>, and the 定義箱 lists the boxes they are sorted into.
 */
LOCAL void test_def( void )
{
	TS_UUID	win = def_of(SYSDEF_MENU_WINDOW), box = def_of(SYSDEF_BOX);
	TS_UUID	*ids;
	T_OBREF	r;
	UB	*rec;
	SZ	size = 0;
	INT	n, i;
	BOOL	menu = FALSE;

	if ( ob_ref_obj(&win, &r) < E_OK ) {
		KT_SKIP("no definitions in the store");
	}
	rec = om_obj_record(&win, 0, &size);
	KT_ASSERT(rec != NULL);
	for ( i = 0; rec != NULL && i + 6 < (INT)size && !menu; i++ ) {
		menu = (BOOL)( rec[i] == '<' && rec[i + 1] == 'm' && rec[i + 2] == 'e'
			       && rec[i + 3] == 'n' && rec[i + 4] == 'u' && rec[i + 5] == '>' );
	}
	KT_ASSERT(menu);
	if ( rec != NULL ) Kfree(rec);

	rec = om_obj_record(&box, 0, &size);
	KT_ASSERT(rec != NULL);
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 8);
	if ( rec != NULL && ids != NULL ) {
		n = om_store_links(rec, size, ids, 8);
		KT_ASSERT(n >= 2);			/* the menus and the parts */
	}
	if ( rec != NULL ) Kfree(rec);
	if ( ids != NULL ) Kfree(ids);
}

/* ---------------------------------------------------------------- menus */

LOCAL void test_menu( void )
{
	TS_UUID		cab = def_of(SYSDEF_MENU_CAB), win = def_of(SYSDEF_MENU_WINDOW);
	TS_UUID		acc = def_of(SYSDEF_MENU_ACC), save;
	T_CABMENU	st;
	T_MNSEL		sel;
	UB		names[2][WM_LABEL_MAX];
	ID		mid;
	T_OBREF		r;

	if ( ob_ref_obj(&cab, &r) < E_OK ) {
		KT_SKIP("no definitions");
	}
	knl_memset(&st, 0, sizeof(st));
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);

	/* nothing taken: what acts on it is grey, and its letter chooses nothing */
	KT_ASSERT_EQ(mn_get_atr(mid, NULL, "copy"), MN_GREY);
	KT_ASSERT_ER(mn_fnd_key(mid, 'C', &sel), E_NOEXS);
	KT_ASSERT_EQ(mn_chg_atr(mid, NULL, "copy", 0), 1);
	KT_ASSERT_ER(mn_fnd_key(mid, 'c', &sel), E_OK);
	KT_ASSERT(mn_is(&sel, "copy"));

	/* 閉じる is the system's, taken in from its definition */
	KT_ASSERT_ER(mn_fnd_key(mid, 'E', &sel), E_OK);
	KT_ASSERT(mn_is(&sel, "close"));
	KT_ASSERT_EQ(ts_uuid_cmp(&sel.def, &win), 0);
	KT_ASSERT_EQ(cab_menu_cmd(&sel), 0);	/* not one of the cabinet's */

	/* an item in a list the root opens (保存), named by its definition */
	KT_ASSERT_ER(mn_fnd_key(mid, 'S', &sel), E_OK);
	KT_ASSERT(mn_is(&sel, "save"));
	save = sel.def;
	KT_ASSERT(ts_uuid_cmp(&save, &cab) != 0);
	KT_ASSERT_EQ(cab_menu_cmd(&sel), CM_SAVE);
	KT_ASSERT_EQ(mn_get_atr(mid, &cab, "save"), E_NOEXS);	/* not in the root's own */
	KT_ASSERT(mn_get_atr(mid, &save, "save") >= 0);

	/* a list the program fills */
	knl_memcpy(names[0], "一", 4);
	knl_memcpy(names[1], "二", 4);
	KT_ASSERT_ER(mn_set_lst(mid, &acc, "acc.item", (CONST UB *)names, WM_LABEL_MAX, 2), E_OK);
	KT_ASSERT_ER(mn_set_lst(mid, &acc, "acc.item", (CONST UB *)names, WM_LABEL_MAX, 2),
		     E_NOEXS);			/* filled once */
	KT_ASSERT_EQ(mn_get_atr(mid, &acc, "acc.item"), 0);
	KT_ASSERT_ER(mn_del_men(mid), E_OK);
	KT_ASSERT_ER(mn_del_men(mid), E_ID);
}

/*
 * A list the program fills with nothing goes, and the items after it stay
 * as they were: トレー has a line and トレーを空にする after its list, and
 * that item must not be written over the others.
 */
LOCAL void test_empty_list( void )
{
	TS_UUID	cab = def_of(SYSDEF_MENU_CAB), td = def_of(SYSDEF_MENU_TRAY);
	ID	mid;

	if ( mn_cre_men(&cab, &mid) < E_OK ) {
		KT_SKIP("no definitions");
	}
	KT_ASSERT_ER(mn_set_lst(mid, &td, "tray.level", NULL, 0, 0), E_OK);
	KT_ASSERT_EQ(mn_chg_atr(mid, &td, "tray.clear", 0), 1);
	KT_ASSERT_ER(mn_del_men(mid), E_OK);
}

#define KT_MANY_APPS	12
LOCAL void test_many_apps_end( TS_UUID *u, T_VOBJ *v, DTAPP *a )
{
	(void)ob_del_obj(u);
	Kfree(v);
	Kfree(a);
}

/*
 * 機能付箋 in any number. Twelve programs in an object's applist: all of
 * them in the list, in the menu's list, and the link can be fixed to the
 * one it opens with though the list does not fit a link. A link read from
 * a record with such a list in its attribute is still a link, with that
 * one entry.
 */
LOCAL void test_many_apps( void )
{
	CONST char	*ids[KT_MANY_APPS] = {
		"basic-text-editor", "basic-figure-editor", "virtual-object-list", "basic-calc-editor",
		"basic-slide", "base-calendar", "tessronos-microcard", "tessronos-techo",
		"web-browser", "archive-manager", "kana-typing-trainer", "microscript"
	};
	UB		*js = (UB *)Kmalloc(4096);
	UB		*xml = (UB *)Kmalloc(4096);
	T_VOBJ		*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	DTAPP		*a = (DTAPP *)Kmalloc(sizeof(DTAPP) * 64);
	T_OBCRE		c;
	TS_UUID		u, cab = def_of(SYSDEF_MENU_CAB), ed = def_of(SYSDEF_MENU_EXEC);
	T_TAD		*doc = NULL;
	UB		rows[KT_MANY_APPS][WM_LABEL_MAX];
	ID		mid;
	INT		i, n, k, dflt = -1;

	KT_ASSERT(js != NULL && xml != NULL && v != NULL && a != NULL);
	(void)dt_prog_start();
	k = tm_sprintf(js, (UB *)"{\"name\":\"KT機能付箋の多い実身\",\"applist\":{");
	for ( i = 0; i < KT_MANY_APPS; i++ ) {
		k += tm_sprintf(js + k, (UB *)"%s\"%s\":{\"name\":\"名前%d\",\"defaultOpen\":%s}",
				( i > 0 ) ? "," : "", ids[i], i, ( i == 3 ) ? "true" : "false");
	}
	k += tm_sprintf(js + k, (UB *)"}}");
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = js;
	c.jsonsz = k;
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	knl_memset(v, 0, sizeof(*v));
	v->target = u;

	KT_ASSERT_EQ(om_link_napps(v), KT_MANY_APPS);
	n = dt_app_list(v, a, 64, &dflt);
	KT_ASSERT_EQ(n, KT_MANY_APPS);
	KT_ASSERT_EQ(dflt, 3);
	for ( i = 0; i < n && i < KT_MANY_APPS; i++ ) KT_ASSERT(a[i].listed);

	/* each of them is a row of the menu's list, and is chosen as itself */
	KT_ASSERT_ER(mn_cre_men(&cab, &mid), E_OK);
	for ( i = 0; i < KT_MANY_APPS; i++ ) tm_sprintf(rows[i], (UB *)"行%d", i);
	KT_ASSERT_ER(mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, KT_MANY_APPS), E_OK);
	for ( i = 0; i < KT_MANY_APPS; i++ ) {
		KT_ASSERT_ER(mn_chg_idx(mid, &ed, "exec.app", i, 0), E_OK);
	}
	KT_ASSERT_ER(mn_chg_idx(mid, &ed, "exec.app", KT_MANY_APPS, 0), E_NOEXS);
	KT_ASSERT_ER(mn_del_men(mid), E_OK);

	/* fixed: the entry it opens with; the list is still the object's twelve */
	KT_ASSERT_ER(om_link_lock(v, TRUE), E_OK);
	KT_ASSERT(om_link_locked(v));
	KT_ASSERT_EQ(dt_app_list(v, a, 64, NULL), KT_MANY_APPS);
	KT_ASSERT_ER(om_link_lock(v, FALSE), E_OK);
	KT_ASSERT(!om_link_locked(v));

	/* a record whose link carries all twelve, as another system may have saved it */
	k = tm_sprintf(xml, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure><link id=\"");
	KT_ASSERT_ER(ts_uuid_to_str(&u, (char *)xml + k, 64), E_OK);
	while ( xml[k] != 0 ) k++;
	k += tm_sprintf(xml + k, (UB *)"_0.xtad\" name=\"KT\" applist=\"{");
	for ( i = 0; i < KT_MANY_APPS; i++ ) {
		k += tm_sprintf(xml + k, (UB *)"%s&quot;%s&quot;:{&quot;name&quot;:&quot;名前%d&quot;,&quot;defaultOpen&quot;:%s}",
				( i > 0 ) ? "," : "", ids[i], i, ( i == 3 ) ? "true" : "false");
	}
	k += tm_sprintf(xml + k, (UB *)"}\"/></figure></tad>\n");
	KT_ASSERT_ER(tad_parse(xml, k, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		knl_memset(v, 0, sizeof(*v));
		KT_ASSERT_EQ(tad_lnk_count(doc), 1);
		KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
		KT_ASSERT(same(v->applist, "{\"basic-calc-editor\":{\"name\":\"名前3\",\"defaultOpen\":true}}"));
		KT_ASSERT_EQ(dt_app_list(v, a, 64, NULL), KT_MANY_APPS);
		tad_free(doc);
	}
	Kfree(js);
	Kfree(xml);
	test_many_apps_end(&u, v, a);
}

/* The system's items set for a virtual object, and what is not theirs */
LOCAL void test_vmn( void )
{
	TS_UUID		cab = def_of(SYSDEF_MENU_CAB), vd = def_of(SYSDEF_MENU_VOBJ);
	TS_UUID		rd = def_of(SYSDEF_MENU_REAL), obj = def_of(SYSDEF_MENU_OBJECT);
	T_CABMENU	st;
	T_MNSEL		sel;
	DTWIN		*d;
	T_VOBJ		*v;
	ID		mid;
	T_OBREF		r;

	if ( ob_ref_obj(&cab, &r) < E_OK ) {
		KT_SKIP("no definitions");
	}
	d = (DTWIN *)Kmalloc(sizeof(DTWIN));
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(d != NULL && v != NULL);
	if ( d == NULL || v == NULL ) return;
	knl_memset(d, 0, sizeof(*d));
	knl_memset(v, 0, sizeof(*v));
	knl_memset(&st, 0, sizeof(st));

	/* away from a virtual object: 仮身操作 and 実身操作 grey */
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);
	dt_get_vmn(d, mid, FALSE, NULL);
	KT_ASSERT_EQ(mn_get_atr(mid, &obj, "vobj"), MN_GREY);
	KT_ASSERT_EQ(mn_get_atr(mid, &obj, "real"), MN_GREY);
	/* ウインドウ is there, between 屑実身操作 and 小物, and can be chosen */
	KT_ASSERT_EQ(mn_get_atr(mid, &obj, "win"), 0);
	(void)mn_del_men(mid);

	/* on one that points at a reachable object that is not open */
	v->target = cab;
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);
	dt_get_vmn(d, mid, TRUE, v);
	KT_ASSERT_EQ(mn_get_atr(mid, &obj, "vobj"), 0);
	KT_ASSERT_EQ(mn_get_atr(mid, &vd, "vobj.open"), 0);
	KT_ASSERT_EQ(mn_get_atr(mid, &vd, "vobj.close"), MN_GREY);
	KT_ASSERT_EQ(mn_get_atr(mid, &vd, "vobj.reconnect"), MN_HIDE);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.lock"), MN_GREY);	/* its applist names no program */
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.unlock"), MN_HIDE);
	(void)mn_del_men(mid);

	/* on one that opens with a program: what it opens with can be fixed */
	v->target = def_of(SYSDEF_PROG_BOX);
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);
	dt_get_vmn(d, mid, TRUE, v);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.lock"), 0);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.unlock"), MN_HIDE);
	(void)mn_del_men(mid);
	v->target = cab;

	/* in a sealed window, what changes its record is grey */
	d->sealed = TRUE;
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);
	dt_get_vmn(d, mid, TRUE, v);
	KT_ASSERT_EQ(mn_get_atr(mid, &vd, "vobj.attr"), MN_GREY);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.duplicate"), MN_GREY);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.rename"), 0);

	/* an item of the program's own is not the system's to do */
	KT_ASSERT_ER(mn_fnd_key(mid, 'L', &sel), E_OK);
	KT_ASSERT(mn_is(&sel, "fullscreen"));
	KT_ASSERT(!dt_exe_vmn(d, &sel, TRUE, v));
	(void)mn_del_men(mid);
	Kfree(d);
	Kfree(v);
}

/* ---------------------------------------------------------------- on the screen */

/* A choice made on the screen, turned back into the item */
LOCAL void test_choose( void )
{
	TS_UUID		cab = def_of(SYSDEF_MENU_CAB);
	T_CABMENU	st;
	T_DISPSPEC	spec;
	T_DPRECT	o, mr;
	T_MNSEL		sel;
	T_WMEV		ev;
	T_OBREF		r;
	INT		pid, cmd = 0;
	ID		mid;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	if ( ob_ref_obj(&cab, &r) < E_OK ) {
		KT_SKIP("no definitions");
	}
	o.left = 80;  o.top = 80;  o.right = 480;  o.bottom = 400;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "menus");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;

	knl_memset(&st, 0, sizeof(st));
	KT_ASSERT_ER(cab_menu_make(&st, &mid), E_OK);
	pid = mn_opn_men(mid, wid, 100, 60);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
		mr.left = mr.top = mr.right = mr.bottom = 0;
		(void)wm_panel_rect(pid, &mr);
		knl_memset(&ev, 0, sizeof(ev));
		ev.wid = wm_panel_wid(pid);
		ev.x = ( mr.left + mr.right ) / 2;
		ev.y = mr.top + wm_menu_row_h() / 2;		/* the first row: 閉じる */
		ev.type = HID_EV_BTN_DOWN;
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		ev.type = HID_EV_BTN_UP;
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		KT_ASSERT(cmd != 0);
		KT_ASSERT_ER(mn_get_sel(mid, cmd, &sel), E_OK);
		KT_ASSERT(mn_is(&sel, "close"));
		KT_ASSERT(same(sel.label, "閉じる"));
		(void)wm_panel_close(pid);
	}
	KT_ASSERT_ER(mn_get_sel(mid, 0, &sel), E_NOEXS);	/* nothing chosen */
	(void)mn_del_men(mid);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	(void)wm_close(wid);
}

/*
 * A menu a program running as a process has opened (mn_pop_men): the
 * process waits in knl_wmobj_menu_run, and the input the window layer
 * reads works the menu. The input comes here from a task of the test's.
 */
IMPORT void knl_wmobj_event( CONST T_WMEV *ev );

LOCAL INT	ss_pid;
LOCAL INT	ss_kind;			/* 1 a choice, 2 a press elsewhere, 3 the first button on the first row, 4 that and the second let go on the ground */
LOCAL UD	ss_when;			/* when the input of 3 and 4 comes */

LOCAL void ss_input( INT stacd, void *exinf )
{
	T_DPRECT	mr;
	T_WMEV		ev;

	tk_dly_tsk(100);
	knl_memset(&ev, 0, sizeof(ev));
	ev.when = 1000000000000ULL;		/* long after the press that opened it */
	if ( ss_kind == 1 ) {
		mr.left = mr.top = mr.right = mr.bottom = 0;
		(void)wm_panel_rect(ss_pid, &mr);
		ev.wid = wm_panel_wid(ss_pid);
		ev.x = ( mr.left + mr.right ) / 2;
		ev.y = mr.top + wm_menu_row_h() / 2;	/* the first row */
		ev.type = HID_EV_MOVE;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
	} else if ( ss_kind == 3 || ss_kind == 4 ) {
		mr.left = mr.top = mr.right = mr.bottom = 0;
		(void)wm_panel_rect(ss_pid, &mr);
		ev.when = ss_when;
		ev.wid = wm_panel_wid(ss_pid);
		ev.x = ( mr.left + mr.right ) / 2;
		ev.y = mr.top + wm_menu_row_h() / 2;	/* the first row */
		ev.type = HID_EV_MOVE;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
		if ( ss_kind == 4 ) {
			ev.wid = 0;			/* the second button, held all along, let go on the ground */
			ev.code = 1;
			ev.when = ss_when + 1000000000ULL;
			knl_wmobj_event(&ev);
		}
	} else {
		ev.wid = 0;				/* on the ground */
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);
	}
	tk_exd_tsk();
}

/* The input's task draws the menu, and a face needs a stack of some size */
LOCAL ID ss_task( void )
{
	T_CTSK	ctsk;

	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)ss_input;
	ctsk.itskpri = KT_PRI_LOW;
	ctsk.stksz   = 32768;
	return tk_cre_tsk(&ctsk);
}

LOCAL void test_session( void )
{
	TS_UUID		def = def_of(SYSDEF_MENU_CONSOLE);
	T_DISPSPEC	spec;
	T_DPRECT	o;
	T_MNSEL		sel;
	T_OBREF		r;
	T_WMEV		ev;
	INT		cmd;
	ID		mid, tid;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	if ( ob_ref_obj(&def, &r) < E_OK ) {
		KT_SKIP("no definitions");
	}
	o.left = 80;  o.top = 80;  o.right = 480;  o.bottom = 400;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "a program's menu");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(mn_cre_men(&def, &mid), E_OK);

	/* an item chosen: its command, and the item */
	ss_pid = mn_opn_men(mid, wid, 60, 40);
	KT_ASSERT(ss_pid > 0);
	if ( ss_pid > 0 ) {
		ss_kind = 1;
		tid = ss_task();
		KT_ASSERT(tid > 0);
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		cmd = knl_wmobj_menu_run(ss_pid, 0);
		KT_ASSERT(cmd > 0);
		KT_ASSERT_ER(mn_get_sel(mid, cmd, &sel), E_OK);
		KT_ASSERT(mn_is(&sel, "close"));
		(void)wm_panel_close(ss_pid);
	}

	/* a press on the ground: put away with nothing chosen */
	ss_pid = mn_opn_men(mid, wid, 60, 40);
	KT_ASSERT(ss_pid > 0);
	if ( ss_pid > 0 ) {
		ss_kind = 2;
		tid = ss_task();
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		KT_ASSERT_EQ(knl_wmobj_menu_run(ss_pid, 0), 0);
		(void)wm_panel_close(ss_pid);
	}

	/*
	 * A quick click of the second button, its press and release in one
	 * report with the same time, let go before the menu is there: a
	 * click, so the menu stays and the first button chooses on it.
	 */
	ss_pid = mn_opn_men(mid, wid, 60, 40);
	KT_ASSERT(ss_pid > 0);
	if ( ss_pid > 0 ) {
		knl_memset(&ev, 0, sizeof(ev));
		ev.code = 1;
		ev.when = 2000000000000ULL;
		ev.type = HID_EV_BTN_DOWN;
		knl_wmobj_event(&ev);			/* the press that opens it, on the ground */
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);			/* on the ground: only the time is kept */
		ss_kind = 3;
		ss_when = ev.when + 1000000000ULL;
		tid = ss_task();
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		cmd = knl_wmobj_menu_run(ss_pid, ev.when);
		KT_ASSERT(cmd > 0);
		KT_ASSERT_ER(mn_get_sel(mid, cmd, &sel), E_OK);
		KT_ASSERT(mn_is(&sel, "close"));
		(void)wm_panel_close(ss_pid);
	}

	/*
	 * The release of an earlier click, before this press: the second
	 * button is held, the first does nothing on the menu, and the second
	 * let go off it puts it away.
	 */
	ss_pid = mn_opn_men(mid, wid, 60, 40);
	KT_ASSERT(ss_pid > 0);
	if ( ss_pid > 0 ) {
		knl_memset(&ev, 0, sizeof(ev));
		ev.code = 1;
		ev.when = 3000000000000ULL;
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_DOWN;
		ev.when += 1000000000ULL;
		knl_wmobj_event(&ev);			/* the press that opens it: the second button, held */
		ev.when -= 1000000000ULL;
		ss_kind = 4;
		ss_when = ev.when + 2000000000ULL;
		tid = ss_task();
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		KT_ASSERT_EQ(knl_wmobj_menu_run(ss_pid, ev.when + 1000000000ULL), 0);
		(void)wm_panel_close(ss_pid);
	}
	(void)mn_del_men(mid);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	(void)wm_close(wid);
}

/*
 * The candidates of a conversion, listed by the system for a plain
 * window (OB_WR_CAND): a window of its own under the composition's box,
 * a press on its second name told to the window as a press on
 * OB_WR_CAND with code 1, and taken away again with n 0.
 */
LOCAL void test_cand( void )
{
	static CONST UB	names[] = "漢字\0感じ\0幹事";
	T_DISPSPEC	spec;
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBNTM		m;
	T_OBWPOS	wp;
	T_WMEV		ev;
	T_WMWIN		pw;
	TS_UUID		win, ch;
	ID		kw, port;
	SZ		asz = 0;
	INT		pop;
	CONST char	*js = "{\"rect\":[100,100,500,400],\"attr\":3}";

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"候補の試験";
	c.json = (CONST UB *)js;
	c.jsonsz = (SZ)knl_strlen(js);
	KT_ASSERT_ER(ob_cre_obj(&c, &win), E_OK);
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	KT_ASSERT(kw > 0);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	KT_ASSERT_ER(ob_cre_obj(&c, &ch), E_OK);
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	KT_ASSERT(port > 0);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS;
	KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &req, port) > 0);
	KT_ASSERT_ER(ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz), E_OK);

	/* listed under the box from (20, 30) to (20, 50) of the work area */
	KT_ASSERT_ER(ob_cand_put(kw, 20, 30, 50, names, 3, 0, 0, 12), E_OK);
	pop = wm_at(wp.wleft + 24, wp.wtop + 56);
	KT_ASSERT(pop > 0 && wm_ref(pop, &pw) >= E_OK && ( pw.attr & WM_ATTR_POPUP ) != 0);
	if ( pop > 0 ) {
		KT_ASSERT_EQ(pw.outer.left, wp.wleft + 20);
		KT_ASSERT_EQ(pw.outer.top, wp.wtop + 52);
	}

	/* the second name pressed: told to the window */
	while ( ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, TMO_POL) >= E_OK ) ;
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.code = 0;
	ev.wid = pop;
	ev.x = 8;
	ev.y = 2 + 22 + 5;				/* a row is the letters' 16 and 6 */
	knl_wmobj_event(&ev);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	KT_ASSERT_ER(ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, 1000), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_PRESS);
	KT_ASSERT_EQ(m.recno, OB_WR_CAND);
	KT_ASSERT_EQ(m.code, 1);

	/* taken away */
	KT_ASSERT_ER(ob_cand_put(kw, 0, 0, 0, NULL, 0, -1, 0, 0), E_OK);
	KT_ASSERT(wm_ref(pop, &pw) < E_OK || ( pw.attr & WM_ATTR_POPUP ) == 0);

	/* and with the window: put up again, the window closed */
	KT_ASSERT_ER(ob_cand_put(kw, 20, 30, 50, names, 3, 0, 0, 3), E_OK);
	pop = wm_at(wp.wleft + 24, wp.wtop + 56);
	KT_ASSERT(pop > 0);
	ob_cls_obj(kw);
	(void)ob_del_obj(&win);
	KT_ASSERT(pop <= 0 || wm_ref(pop, &pw) < E_OK || ( pw.attr & WM_ATTR_POPUP ) == 0);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
}

EXPORT void ktest_mn( void )
{
	KT_RUN(test_json);
	KT_RUN(test_def);
	KT_RUN(test_menu);
	KT_RUN(test_empty_list);
	KT_RUN(test_many_apps);
	KT_RUN(test_vmn);
	KT_RUN(test_choose);
	KT_RUN(test_session);
	KT_RUN(test_cand);
}
