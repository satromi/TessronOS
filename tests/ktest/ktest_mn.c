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
#include <ts/sysdef.h>
#include <ts/cabmenu.h>
#include "../../application/desktop/desktop.h"

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
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.lock"), 0);
	KT_ASSERT_EQ(mn_get_atr(mid, &rd, "real.unlock"), MN_HIDE);
	(void)mn_del_men(mid);

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
LOCAL INT	ss_kind;			/* 1 a choice, 2 a press elsewhere */

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
	(void)mn_del_men(mid);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	(void)wm_close(wid);
}

EXPORT void ktest_mn( void )
{
	KT_RUN(test_json);
	KT_RUN(test_def);
	KT_RUN(test_menu);
	KT_RUN(test_vmn);
	KT_RUN(test_choose);
	KT_RUN(test_session);
}
