/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_part.c
 *	Parts, panels and menus (design 16.2.2, stage 10b)
 *
 *	What the tests are for is the behaviour, not the look: a definition
 *	is copied and not held, a choice turns its group off, an answer is
 *	given on the release and not the press, and a menu answers with the
 *	command of the item that was chosen.
 *
 *	Labels are not drawn until there is a font, so nothing here reads a
 *	pixel where a word will be.
 *
 *	The test task has a small stack, so a definition comes from the heap.
 */

#include "ktest.h"
#include <ts/part.h>
#include <ts/dbox.h>
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/hid.h>
#include <ts/fn.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

LOCAL INT	wid = 0;
LOCAL BOOL	ready = FALSE;

LOCAL void ev_at( T_WMEV *ev, UINT type, INT x, INT y )
{
	ev->type = type;
	ev->code = 0;
	ev->mods = 0;
	ev->x    = x;
	ev->y    = y;
	ev->when = 0;
	ev->wid  = wid;
}

/*
 * An event on a row of a menu. Each list of a menu stands in a popup
 * window of its own, and the rows are measured from that window's work
 * area: the event is that window's, in the middle of the row.
 */
LOCAL void ev_row( T_WMEV *ev, UINT type, INT pid, INT row )
{
	T_DPRECT	mr;

	mr.left = 0;  mr.top = 0;  mr.right = 0;
	(void)wm_panel_rect(pid, &mr);
	ev_at(ev, type, ( mr.left + mr.right ) / 2, mr.top + row * wm_menu_row_h()
					+ wm_menu_row_h() / 2);
	ev->wid = wm_panel_wid(pid);
}

LOCAL void part_set( T_WMPART *pt, UINT kind, INT num,
		     INT l, INT t, INT r, INT b )
{
	INT	i;

	{ UB *z = (UB *)pt; SZ k; for ( k = 0; k < (SZ)sizeof(*pt); k++ ) z[k] = 0; }	/* what is not set here is nought */
	pt->type   = kind;
	pt->num    = num;
	pt->r.left = l;  pt->r.top = t;  pt->r.right = r;  pt->r.bottom = b;
	pt->answer = WM_ANS_NONE;
	pt->group  = 0;
	pt->value  = 0;
	pt->colour = 0;
	for ( i = 0; i < WM_LABEL_MAX; i++ ) pt->label[i] = 0;
	for ( i = 0; i < WM_LABEL_MAX; i++ ) pt->text[i] = 0;
	pt->pool  = 0;
	pt->count = 0;
	pt->top   = 1;
	pt->rows  = 0;
	pt->caret = 0;
	for ( i = 0; i < WM_SB_MAX; i++ ) {
		pt->sbv[i] = 0;
		pt->sbw[i] = 0;
		pt->sblo[i] = 0;
		pt->sbhi[i] = 0;
	}
	pt->sb_n = 0;
	pt->sb_at = 0;
	pt->lo = 0;
	pt->hi = 0;
}

/* a window to put panels in */
LOCAL void test_start( void )
{
	T_DPRECT	o;
	T_DISPSPEC	spec;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	o.left = 60;  o.top = 60;  o.right = 560;  o.bottom = 440;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "panels");
	if ( wid < 0 ) {
		KT_SKIP("windows did not start");
	}
	ready = TRUE;
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
}

/* a definition is copied when the panel opens, not held by pointer */
LOCAL void test_copied( void )
{
	T_WMPANEL	*d;
	INT		pid, v = -1;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;

	d->num = 3;
	d->owner = 0;
	d->r.left = 10;  d->r.top = 10;  d->r.right = 210;  d->r.bottom = 110;
	d->npart = 1;
	part_set(&d->part[0], WM_PT_CHECK, 7, 10, 10, 30, 30);
	d->part[0].value = 1;

	pid = wm_panel_open(wid, d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) { Kfree(d); return; }

	/* changing the caller's own definition afterwards changes nothing */
	d->part[0].value = 0;
	d->npart = 0;
	KT_ASSERT_ER(wm_panel_get(pid, 7, &v), E_OK);
	KT_ASSERT_EQ(v, 1);
	Kfree(d);				/* and freeing it is safe */

	KT_ASSERT_ER(wm_panel_get(pid, 7, &v), E_OK);
	KT_ASSERT_EQ(v, 1);
	KT_ASSERT_ER(wm_panel_get(pid, 99, &v), E_NOEXS);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	KT_ASSERT_ER(wm_panel_close(pid), E_ID);
}

/* a choice turns its group off; a check stands alone */
LOCAL void test_choice( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	INT		pid, v;
	UINT		ans = WM_ANS_NONE;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;

	d->num = 1;  d->owner = 0;
	d->r.left = 20;  d->r.top = 20;  d->r.right = 260;  d->r.bottom = 160;
	d->npart = 4;
	part_set(&d->part[0], WM_PT_CHOICE, 1, 10, 10, 40, 40);
	part_set(&d->part[1], WM_PT_CHOICE, 2, 50, 10, 80, 40);
	part_set(&d->part[2], WM_PT_CHOICE, 3, 90, 10, 120, 40);
	part_set(&d->part[3], WM_PT_CHECK, 4, 10, 60, 40, 90);
	d->part[0].value = 1;

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/* press and let go of the third choice: it comes on and the first goes off */
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 100, 20 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 100, 20 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);
	KT_ASSERT_ER(wm_panel_get(pid, 3, &v), E_OK);
	KT_ASSERT_EQ(v, 1);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 0);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);

	/* the check has nothing to do with the choices */
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 20, 20 + 70);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 20, 20 + 70);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 4, &v), E_OK);
	KT_ASSERT_EQ(v, 1);
	KT_ASSERT_ER(wm_panel_get(pid, 3, &v), E_OK);
	KT_ASSERT_EQ(v, 1);

	/* pressing it again turns it off */
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 20, 20 + 70);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 20, 20 + 70);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 4, &v), E_OK);
	KT_ASSERT_EQ(v, 0);

	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* the answer comes on the release, and moving away first takes it back */
LOCAL void test_answer( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	INT		pid;
	UINT		ans = WM_ANS_NONE;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;

	d->num = 2;  d->owner = 0;
	d->r.left = 30;  d->r.top = 30;  d->r.right = 230;  d->r.bottom = 130;
	d->npart = 3;
	part_set(&d->part[0], WM_PT_BUTTON, 1, 10, 50, 70, 80);
	d->part[0].answer = WM_ANS_OK;
	part_set(&d->part[1], WM_PT_BUTTON, 2, 90, 50, 150, 80);
	d->part[1].answer = WM_ANS_CANCEL;
	part_set(&d->part[2], WM_PT_LABEL, 3, 10, 10, 150, 30);

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	/* press on the first button, release away from it: no answer */
	ev_at(&ev, HID_EV_BTN_DOWN, 30 + 20, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);
	ev_at(&ev, HID_EV_BTN_UP, 30 + 20, 30 + 120);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);

	/* press and release on it: it answers */
	ev_at(&ev, HID_EV_BTN_DOWN, 30 + 20, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 30 + 20, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_OK);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);

	/* a button that acts answers once and leaves the panel going */
	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 2;  d->owner = 0;
	d->r.left = 30;  d->r.top = 30;  d->r.right = 230;  d->r.bottom = 130;
	d->npart = 1;
	part_set(&d->part[0], WM_PT_BUTTON, 1, 10, 50, 70, 80);
	d->part[0].answer = WM_ANS_ACT + 3;
	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	ev_at(&ev, HID_EV_BTN_DOWN, 30 + 20, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 30 + 20, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_ACT + 3);
	ev_at(&ev, HID_EV_MOVE, 30 + 25, 30 + 60);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);

	/* a label takes nothing, so pressing one answers nothing */
	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 2;  d->owner = 0;
	d->r.left = 30;  d->r.top = 30;  d->r.right = 230;  d->r.bottom = 130;
	d->npart = 1;
	part_set(&d->part[0], WM_PT_LABEL, 1, 10, 10, 150, 30);
	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	ev_at(&ev, HID_EV_BTN_DOWN, 30 + 20, 30 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 30 + 20, 30 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* escape abandons a panel */
LOCAL void test_escape( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	INT		pid;
	UINT		ans = WM_ANS_NONE;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 5;  d->owner = 0;
	d->r.left = 40;  d->r.top = 40;  d->r.right = 200;  d->r.bottom = 120;
	d->npart = 1;
	part_set(&d->part[0], WM_PT_BUTTON, 1, 10, 10, 70, 40);
	d->part[0].answer = WM_ANS_OK;
	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	ev_at(&ev, HID_EV_KEY_DOWN, 0, 0);
	ev.code = 0x29;
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_CANCEL);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* a menu is a panel of one column, and it answers with a command */
LOCAL void test_menu( void )
{
	T_WMMENU	*m;
	T_WMEV		ev;
	INT		pid, cmd = -1, i;

	if ( !ready ) KT_SKIP("no window");

	m = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(8));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;

	m->num = 1;  m->owner = 0;  m->nitem = 4;
	for ( i = 0; i < 4; i++ ) {
		INT k;

		m->item[i].cmd = ( i == 2 ) ? 0 : ( 10 + i );	/* [2] is a line */
		m->item[i].grey = ( i == 1 );
		m->item[i].tick = FALSE;
		for ( k = 0; k < WM_LABEL_MAX; k++ ) m->item[i].label[k] = 0;
	}
	pid = wm_menu_open(wid, m, 100, 100);
	Kfree(m);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);

	/* the first item: pressing and releasing on it gives its command */
	ev_row(&ev, HID_EV_BTN_DOWN, pid, 0);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 0);			/* not yet: the press is not the answer */
	ev_row(&ev, HID_EV_BTN_UP, pid, 0);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 10);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);

	/* one that cannot be chosen answers nothing */
	m = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(8));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	m->num = 1;  m->owner = 0;  m->nitem = 2;
	for ( i = 0; i < 2; i++ ) {
		INT k;

		m->item[i].cmd = 20 + i;
		m->item[i].grey = ( i == 0 );
		m->item[i].tick = FALSE;
		for ( k = 0; k < WM_LABEL_MAX; k++ ) m->item[i].label[k] = 0;
	}
	pid = wm_menu_open(wid, m, 100, 200);
	Kfree(m);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	cmd = -1;
	ev_row(&ev, HID_EV_BTN_DOWN, pid, 0);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	ev_row(&ev, HID_EV_BTN_UP, pid, 0);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 0);

	/* the one below it can be chosen */
	ev_row(&ev, HID_EV_BTN_DOWN, pid, 1);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	ev_row(&ev, HID_EV_BTN_UP, pid, 1);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 21);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/*
 * A row with a list under it. Clicked, it is not chosen: the menu stays
 * and its list opens at once and is held open while the pointer passes
 * over the other rows; an item chosen in that list is the answer. A
 * click on another row with a list puts the first list away.
 */
#define KT_SUB_LIST	0x3E00		/* the list's number in the data box */

/* The list open beside a row of menu pid: its panel, 0 none */
LOCAL INT menu_child( INT pid )
{
	INT	k, w, pw = wm_panel_wid(pid);

	for ( k = 1; k <= WM_PANEL_MAX; k++ ) {
		w = wm_panel_wid(k);
		if ( k != pid && w > 0 && w != pw && wm_menu_has(pid, w) ) {
			return k;
		}
	}
	return 0;
}

LOCAL void test_menu_sub( void )
{
	T_WMMENU	*m, *s;
	T_WMEV		ev;
	INT		pid, child, cmd = -1, i;

	if ( !ready ) KT_SKIP("no window");

	s = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(2));
	m = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(3));
	KT_ASSERT(s != NULL && m != NULL);
	if ( s == NULL || m == NULL ) {
		if ( s != NULL ) Kfree(s);
		if ( m != NULL ) Kfree(m);
		return;
	}
	s->num = KT_SUB_LIST;  s->owner = 0;  s->nitem = 2;
	for ( i = 0; i < 2; i++ ) {
		s->item[i].cmd = 50 + i;
		s->item[i].label[0] = (UB)( 'a' + i );
	}
	KT_ASSERT_ER(db_put(DB_MENU, KT_SUB_LIST, 0, s, WM_MENU_SIZE(2)), E_OK);
	Kfree(s);
	/* [0] a plain item, [1] and [2] rows with the list under them */
	m->num = 1;  m->owner = 0;  m->nitem = 3;
	for ( i = 0; i < 3; i++ ) {
		m->item[i].cmd = 40 + i;
		m->item[i].label[0] = (UB)( 'A' + i );
		m->item[i].sub = ( i > 0 ) ? KT_SUB_LIST : 0;
	}
	pid = wm_menu_open(wid, m, 100, 100);
	Kfree(m);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) {
		(void)db_del(DB_MENU, KT_SUB_LIST, 0);
		return;
	}
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/* a click on the row opens its list and chooses nothing */
	ev_row(&ev, HID_EV_BTN_DOWN, pid, 1);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	ev_row(&ev, HID_EV_BTN_UP, pid, 1);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 0);
	child = menu_child(pid);
	KT_ASSERT(child > 0);

	/* the pointer over the other rows leaves it open */
	ev_row(&ev, HID_EV_MOVE, pid, 0);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 0);
	KT_ASSERT_EQ(menu_child(pid), child);

	/* a click on the other row with a list: that one's list instead */
	ev_row(&ev, HID_EV_BTN_DOWN, pid, 2);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	ev_row(&ev, HID_EV_BTN_UP, pid, 2);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 0);
	child = menu_child(pid);
	KT_ASSERT(child > 0);

	/* an item of the list held open is the answer */
	if ( child > 0 ) {
		ev_row(&ev, HID_EV_MOVE, child, 1);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		KT_ASSERT_EQ(cmd, 0);
		ev_row(&ev, HID_EV_BTN_DOWN, child, 1);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		ev_row(&ev, HID_EV_BTN_UP, child, 1);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		KT_ASSERT_EQ(cmd, 51);
	}
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	(void)db_del(DB_MENU, KT_SUB_LIST, 0);
}

/*
 * A menu of more items than the 32 there once was room for: each is
 * there, the last one is chosen where it is (in a column of its own when
 * the menu is taller than the screen) and answers its command.
 */
#define KT_MENU_MANY	40

LOCAL void test_menu_many( void )
{
	T_WMMENU	*m;
	T_WMEV		ev;
	T_DPRECT	mr;
	INT		pid, cmd = -1, i, rh, rows, cols, last = KT_MENU_MANY - 1;

	if ( !ready ) KT_SKIP("no window");

	m = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(KT_MENU_MANY));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	m->num = 1;  m->owner = 0;  m->nitem = KT_MENU_MANY;
	for ( i = 0; i < KT_MENU_MANY; i++ ) {
		m->item[i].cmd = 100 + i;
		m->item[i].label[0] = (UB)( 'A' + i % 26 );
	}
	pid = wm_menu_open(wid, m, 100, 100);
	Kfree(m);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);

	/* where the last item is: its column and its row */
	mr.left = 0;  mr.top = 0;  mr.right = 0;  mr.bottom = 0;
	(void)wm_panel_rect(pid, &mr);
	rh = wm_menu_row_h();
	rows = ( rh > 0 ) ? ( mr.bottom - mr.top ) / rh : 0;
	if ( rows > KT_MENU_MANY ) rows = KT_MENU_MANY;
	KT_ASSERT(rows > 0);
	if ( rows <= 0 ) {
		(void)wm_panel_close(pid);
		return;
	}
	cols = ( KT_MENU_MANY + rows - 1 ) / rows;
	ev_at(&ev, HID_EV_BTN_DOWN, mr.left + ( mr.right - mr.left ) * ( 2 * ( last / rows ) + 1 ) / ( 2 * cols ),
	      mr.top + ( last % rows ) * rh + rh / 2);
	ev.wid = wm_panel_wid(pid);
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	ev.type = HID_EV_BTN_UP;
	KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
	KT_ASSERT_EQ(cmd, 100 + last);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* the table fills up and says so rather than overwriting something */
LOCAL void test_limit( void )
{
	T_WMPANEL	*d;
	INT		pid[WM_PANEL_MAX + 1];
	INT		i, n = 0;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 9;  d->owner = 0;
	d->r.left = 10;  d->r.top = 10;  d->r.right = 60;  d->r.bottom = 60;
	d->npart = 0;

	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		pid[i] = wm_panel_open(wid, d);
		KT_ASSERT(pid[i] > 0);
		if ( pid[i] > 0 ) n++;
	}
	KT_ASSERT_EQ(wm_panel_open(wid, d), E_LIMIT);

	/* a definition that says nothing sensible is refused */
	d->r.right = d->r.left;
	KT_ASSERT_EQ(wm_panel_open(wid, d), E_PAR);
	d->r.right = 60;
	d->npart = WM_PART_MAX + 1;
	KT_ASSERT_EQ(wm_panel_open(wid, d), E_PAR);
	Kfree(d);

	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(wm_panel_close(pid[i]), E_OK);
	}
	KT_ASSERT_EQ(wm_panel_self_check(), 0);

	if ( wid > 0 ) {
		wm_close(wid);
		wid = 0;
		ready = FALSE;
	}
}

/* a label is drawn when there is a font to draw it with */
LOCAL void test_label( void )
{
	T_WMPANEL	*d;
	T_DISPSPEC	spec;
	T_WMWIN		w;
	INT		pid, i, j, ink = 0;
	UB		*fb;

	if ( !ready ) KT_SKIP("no window");
	if ( fn_system() == 0 ) KT_SKIP("no system font");
	if ( ts_disp_ref(&spec) < E_OK ) KT_SKIP("the machine has no screen");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 8;  d->owner = 0;  d->kind = WM_PNL_PLAIN;
	d->r.left = 20;  d->r.top = 200;  d->r.right = 260;  d->r.bottom = 250;
	d->npart = 1;
	part_set(&d->part[0], WM_PT_LABEL, 1, 10, 10, 200, 34);
	d->part[0].label[0] = 'A';
	d->part[0].label[1] = 'V';
	d->part[0].label[2] = 0;

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/*
	 * Something was put down where the label sits. The place is
	 * measured from the work area's corner, which is where a panel's
	 * own coordinates start; taking it from the window's outer corner
	 * would measure from the frame and find the frame's own lines
	 * rather than the words.
	 */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	fb = (UB *)ts_disp_buffer();
	for ( j = 200 + 10; j < 200 + 34; j++ ) {
		for ( i = 20 + 4; i < 20 + 200; i++ ) {
			INT	sx = w.work.left + i, sy = w.work.top + j;
			UW	px = *(UW *)(fb + (UBINT)sy * spec.pitch
					     + (UBINT)sx * 4) & 0x00FFFFFF;

			if ( px == wm_look(WM_LOOK_PART_TEXT) ) {
				ink++;
			}
		}
	}
	KT_ASSERT(ink > 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* a key, as the keyboard sends it */
LOCAL void ev_key( T_WMEV *ev, UINT code, UINT mods )
{
	ev->type = HID_EV_KEY_DOWN;
	ev->code = code;
	ev->mods = mods;
	ev->x    = 0;
	ev->y    = 0;
	ev->when = 0;
	ev->wid  = wid;
}

/*
 * A list shows a few of its names at a time, says which was chosen, and
 * is wound by its own bar. Nothing scrolls by itself: what is shown is
 * what it was told to show.
 */
LOCAL void test_list( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	UINT		ans;
	INT		pid, i, at, value = 0;
	UB		name[8];

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 11;  d->owner = 0;  d->kind = WM_PNL_PLAIN;
	d->r.left = 20;  d->r.top = 20;  d->r.right = 260;  d->r.bottom = 140;
	d->npart = 1;
	d->used = 0;
	part_set(&d->part[0], WM_PT_LIST, 1, 10, 10, 220, 110);

	/* ten names, the first of them where the part says to look */
	at = -1;
	for ( i = 0; i < 10; i++ ) {
		INT	k;

		name[0] = 'n';  name[1] = 'a';  name[2] = 'm';  name[3] = 'e';
		name[4] = (UB)('0' + i);  name[5] = 0;
		k = wm_panel_name(d, name);
		KT_ASSERT(k >= 0);
		if ( at < 0 ) at = k;
	}
	d->part[0].pool  = at;
	d->part[0].count = 10;
	d->part[0].top   = 1;

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/* the wheel turned toward the person over the list rolls it three rows: the first row is the fourth name */
	ev_at(&ev, HID_EV_WHEEL, 20 + 10 + 40, 20 + 10 + 30);
	ev.dz = -1;
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 10 + 40, 20 + 10 + 2 + 9);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 10 + 40, 20 + 10 + 2 + 9);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &value), E_OK);
	KT_ASSERT_EQ(value, 4);
	/* and back, turned away; not past the first */
	ev_at(&ev, HID_EV_WHEEL, 20 + 10 + 40, 20 + 10 + 30);
	ev.dz = 2;
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 10 + 40, 20 + 10 + 2 + 9);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 10 + 40, 20 + 10 + 2 + 9);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &value), E_OK);
	KT_ASSERT_EQ(value, 1);

	/* pressing the second row chooses the second name */
	{
		INT	rowh = 0, y;

		/* the rows start two inside the box; one row is the letters
		   and two more, which is what the layer uses */
		y = 20 + 10 + 2;
		rowh = 18;
		ev_at(&ev, HID_EV_BTN_DOWN, 20 + 10 + 40, y + rowh + 2);
		KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
		ev_at(&ev, HID_EV_BTN_UP, 20 + 10 + 40, y + rowh + 2);
		KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	}
	KT_ASSERT_ER(wm_panel_get(pid, 1, &value), E_OK);
	KT_ASSERT(value >= 1 && value <= 10);

	/* pressing low on its bar winds it down */
	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 220 - 10, 20 + 100);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 220 - 10, 20 + 100);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* a box takes letters, keeps them in order, and gives them back */
LOCAL void test_box( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	UINT		ans;
	INT		pid;
	UB		buf[WM_LABEL_MAX];

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 12;  d->owner = 0;  d->kind = WM_PNL_PLAIN;
	d->r.left = 20;  d->r.top = 160;  d->r.right = 280;  d->r.bottom = 210;
	d->npart = 1;
	d->used = 0;
	part_set(&d->part[0], WM_PT_BOX, 1, 10, 10, 200, 36);

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	/* the first box has the letters from the moment the panel opens */
	ev_key(&ev, 0x04, 0);				/* 'a' */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'a');
	KT_ASSERT_EQ((INT)buf[1], 0);
	KT_ASSERT_ER(wm_panel_set_text(pid, 1, (CONST UB *)""), E_OK);

	ev_at(&ev, HID_EV_BTN_DOWN, 20 + 20, 160 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 20 + 20, 160 + 20);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);

	ev_key(&ev, 0x04, 0);				/* a */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x05, HID_MOD_LSHIFT);		/* B */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x06, 0);				/* c */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'a');
	KT_ASSERT_EQ((INT)buf[1], 'B');
	KT_ASSERT_EQ((INT)buf[2], 'c');
	KT_ASSERT_EQ((INT)buf[3], 0);

	/* what was typed before the caret goes away */
	ev_key(&ev, 0x2A, 0);				/* backspace */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[2], 0);

	/* the caret moves, and a letter goes in where it is */
	ev_key(&ev, 0x50, 0);				/* left */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x07, 0);				/* d */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'a');
	KT_ASSERT_EQ((INT)buf[1], 'd');
	KT_ASSERT_EQ((INT)buf[2], 'B');
	KT_ASSERT_EQ((INT)buf[3], 0);

	/*
	 * A letter of a Japanese name is three bytes, and the caret steps
	 * over it and takes it away whole: half a letter is no letter.
	 */
	KT_ASSERT_ER(wm_panel_set_text(pid, 1,
		(CONST UB *)"\xE3\x81\x82\xE3\x81\x84"), E_OK);	/* あい */
	ev_key(&ev, 0x2A, 0);				/* backspace */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 0xE3);
	KT_ASSERT_EQ((INT)buf[2], 0x82);
	KT_ASSERT_EQ((INT)buf[3], 0);
	ev_key(&ev, 0x50, 0);				/* left, over あ */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x04, 0);				/* a, before it */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'a');
	KT_ASSERT_EQ((INT)buf[1], 0xE3);
	KT_ASSERT_EQ((INT)buf[4], 0);
	ev_key(&ev, 0x4C, 0);				/* delete: あ, whole */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'a');
	KT_ASSERT_EQ((INT)buf[1], 0);

	/* and setting it outright replaces the lot */
	KT_ASSERT_ER(wm_panel_set_text(pid, 1, (CONST UB *)"hi"), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
	KT_ASSERT_EQ((INT)buf[0], 'h');
	KT_ASSERT_EQ((INT)buf[1], 'i');
	KT_ASSERT_EQ((INT)buf[2], 0);
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* One pixel of the screen as it is shown, without its alpha */
LOCAL UW screen_px( CONST UB *fb, CONST T_DISPSPEC *sp, INT x, INT y )
{
	return *(CONST UW *)( fb + (UBINT)y * sp->pitch + (UBINT)x * 4 ) & 0x00FFFFFF;
}

/* a menu's row follows the pointer, and a row with nothing in it is a line */
LOCAL void test_menu_look( void )
{
	T_WMMENU	*m;
	T_WMEV		ev;
	T_WMWIN		w;
	T_DISPSPEC	spec;
	UB		*fb;
	UW		*before;
	INT		pid, cmd = 0, i, lit = 0, black = 0;

	if ( !ready ) KT_SKIP("no window");
	if ( ts_disp_ref(&spec) < E_OK ) KT_SKIP("the machine has no screen");

	m = (T_WMMENU *)Kcalloc(1, WM_MENU_SIZE(8));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	m->num = 3;  m->owner = 0;  m->nitem = 3;
	for ( i = 0; i < 3; i++ ) {
		INT	k;

		m->item[i].cmd  = ( i == 1 ) ? 0 : ( 21 + i / 2 );
		m->item[i].grey = FALSE;
		m->item[i].tick = FALSE;
		for ( k = 0; k < WM_LABEL_MAX; k++ ) m->item[i].label[k] = 0;
	}
	m->item[0].label[0] = 'h';  m->item[0].label[1] = 'i';
	m->item[2].label[0] = 't';  m->item[2].label[1] = 'o';

	pid = wm_menu_open(wid, m, 300, 30);
	Kfree(m);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	{
		T_DPRECT	mr;
		INT		my;

		KT_ASSERT_ER(wm_panel_rect(pid, &mr), E_OK);
		my = mr.top + wm_menu_row_h() / 2;	/* the middle of the first row */

		/*
		 * The pointer over the first row lights it: the row is
		 * turned over, so every pixel across its middle becomes the
		 * other colour of what it was -- the white ground black and
		 * the letters' pixels light. The row is looked at before and
		 * after, not for black alone: the list is as narrow as its
		 * names, and with the system's font set up the letters of a
		 * name of two can be most of a narrow row's middle.
		 */
		KT_ASSERT_ER(wm_composite(), E_OK);
		KT_ASSERT_ER(wm_ref(wm_panel_wid(pid), &w), E_OK);
		fb = (UB *)ts_disp_buffer();
		before = (UW *)Kmalloc(sizeof(UW) * (SZ)( mr.right - mr.left ));
		KT_ASSERT(before != NULL);
		if ( before == NULL ) {
			(void)wm_panel_close(pid);
			return;
		}
		for ( i = mr.left + 2; i < mr.right - 2; i++ ) {
			before[i - mr.left] = screen_px(fb, &spec, w.work.left + i, w.work.top + my);
		}
		ev_row(&ev, HID_EV_MOVE, pid, 0);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		KT_ASSERT_ER(wm_composite(), E_OK);
		for ( i = mr.left + 2; i < mr.right - 2; i++ ) {
			UW	px = screen_px(fb, &spec, w.work.left + i, w.work.top + my);

			if ( px == ( before[i - mr.left] ^ 0x00FFFFFF ) ) {
				lit++;
			}
			if ( px == 0 ) {
				black++;
			}
		}
		Kfree(before);
		tm_printf((UB *)"  the first row, %d wide: %d of %d pixels across its middle turned over, %d black\n",
			  mr.right - mr.left, lit, mr.right - mr.left - 4, black);
		KT_ASSERT(lit > ( mr.right - mr.left ) / 2);
		KT_ASSERT(black > 0);

		/* and pressing it answers with that item's command */
		ev_row(&ev, HID_EV_BTN_DOWN, pid, 0);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		ev_row(&ev, HID_EV_BTN_UP, pid, 0);
		KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
		KT_ASSERT_EQ(cmd, 21);
	}

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/*
 * The data box: definitions kept by kind, number and owner. Nothing is
 * handed out by pointer, and what a program owned goes when it does.
 */
LOCAL void test_dbox( void )
{
	T_WMPANEL	*d, *back;
	INT		n, pid;

	KT_ASSERT(knl_db_init() != 0);

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	back = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL && back != NULL);
	if ( d == NULL || back == NULL ) return;

	d->num = 30;  d->owner = 7;  d->kind = WM_PNL_PLAIN;
	d->r.left = 10;  d->r.top = 10;  d->r.right = 210;  d->r.bottom = 90;
	d->npart = 1;
	d->used = 0;
	part_set(&d->part[0], WM_PT_BUTTON, 1, 10, 10, 90, 40);
	d->part[0].answer = WM_ANS_OK;

	KT_ASSERT_ER(db_put(DB_PANEL, 30, 7, d, sizeof(T_WMPANEL)), E_OK);
	KT_ASSERT_EQ(db_size(DB_PANEL, 30, 7), (INT)sizeof(T_WMPANEL));

	/* what comes back is a copy, not the caller's own */
	d->npart = 99;
	n = db_get(DB_PANEL, 30, 7, back, sizeof(T_WMPANEL));
	KT_ASSERT_EQ(n, (INT)sizeof(T_WMPANEL));
	KT_ASSERT_EQ(back->npart, 1);

	/* a number is not a name: another owner's 30 is another panel */
	KT_ASSERT_EQ(db_get(DB_PANEL, 30, 8, back, sizeof(T_WMPANEL)), E_NOEXS);

	/* the system's own is what a program falls back on */
	d->npart = 1;
	KT_ASSERT_ER(db_put(DB_PANEL, 31, 0, d, sizeof(T_WMPANEL)), E_OK);
	KT_ASSERT_EQ(db_get(DB_PANEL, 31, 8, back, sizeof(T_WMPANEL)),
		     (INT)sizeof(T_WMPANEL));

	/* too little room says so and writes nothing */
	KT_ASSERT_EQ(db_get(DB_PANEL, 30, 7, back, 8), E_LIMIT);

	/* and a panel opens straight out of the box */
	if ( ready ) {
		pid = wm_panel_open_box(wid, 30, 7);
		KT_ASSERT(pid > 0);
		if ( pid > 0 ) {
			KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
			KT_ASSERT_ER(wm_panel_close(pid), E_OK);
		}
	}

	/* what a program owned goes when it does */
	KT_ASSERT_EQ(db_del_owner(7), 1);
	KT_ASSERT_EQ(db_get(DB_PANEL, 30, 7, back, sizeof(T_WMPANEL)), E_NOEXS);
	KT_ASSERT_ER(db_del(DB_PANEL, 31, 0), E_OK);
	KT_ASSERT_ER(db_del(DB_PANEL, 31, 0), E_NOEXS);

	Kfree(d);
	Kfree(back);
}

/* a volume: a knob on a track, set to a number between its two ends */
LOCAL void test_volume( void )
{
	T_WMPANEL	*d;
	INT		pid, value = 0;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 13;  d->owner = 0;  d->kind = WM_PNL_PLAIN;
	d->r.left = 300;  d->r.top = 160;  d->r.right = 540;  d->r.bottom = 220;
	d->npart = 1;
	d->used = 0;
	part_set(&d->part[0], WM_PT_VOL, 1, 10, 10, 220, 42);
	d->part[0].type |= P_HALIGN;
	d->part[0].lo = 0;
	d->part[0].hi = 100;
	d->part[0].value = 50;

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &value), E_OK);
	KT_ASSERT_EQ(value, 50);
	KT_ASSERT_ER(wm_panel_set(pid, 1, 80), E_OK);
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/*
 * A number box takes digits and steps; a box of fields along a line
 * reads its fields out of its own words, one field per run of hashes.
 */
LOCAL void test_boxes( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	UINT		ans;
	INT		pid, v = 0;

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 14;  d->owner = 0;  d->kind = WM_PNL_PLAIN;
	d->r.left = 300;  d->r.top = 240;  d->r.right = 600;  d->r.bottom = 340;
	d->npart = 2;
	d->used = 0;
	part_set(&d->part[0], WM_PT_NUMBOX, 1, 10, 10, 120, 40);
	part_set(&d->part[1], SB_PARTS, 2, 10, 50, 280, 82);
	{
		INT	k;
		CONST char *tmpl = "####/##/##";

		for ( k = 0; tmpl[k] != 0; k++ ) {
			d->part[1].label[k] = (UB)tmpl[k];
		}
		d->part[1].label[k] = 0;
		d->part[1].sblo[0] = 0;  d->part[1].sbhi[0] = 9999;
		d->part[1].sblo[1] = 1;  d->part[1].sbhi[1] = 12;
		d->part[1].sblo[2] = 1;  d->part[1].sbhi[2] = 31;
	}

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/* the number box: press it, then type */
	ev_at(&ev, HID_EV_BTN_DOWN, 300 + 40, 240 + 25);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 300 + 40, 240 + 25);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x21, 0);				/* 4 */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_key(&ev, 0x22, 0);				/* 5 */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 45);
	ev_key(&ev, 0x52, 0);				/* up */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 46);

	/* the fields: three of them, and each keeps to its range */
	KT_ASSERT_ER(wm_panel_set_field(pid, 2, 0, 2026), E_OK);
	KT_ASSERT_ER(wm_panel_set_field(pid, 2, 1, 99), E_OK);
	KT_ASSERT_ER(wm_panel_field(pid, 2, 0, &v), E_OK);
	KT_ASSERT_EQ(v, 2026);
	KT_ASSERT_ER(wm_panel_field(pid, 2, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 12);				/* cut to its range */
	KT_ASSERT_ER(wm_panel_field(pid, 2, 9, &v), E_PAR);
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
}

/* A pop-up's part pressed, at a time given in ms: its list opens */
LOCAL INT popup_press( INT pid, UD ms, INT *p_lw )
{
	T_WMEV	ev;
	UINT	ans;

	ev_at(&ev, HID_EV_BTN_DOWN, 40 + 60, 260 + 30);
	ev.when = ms * 1000000ULL;
	(void)wm_panel_event(pid, &ev, &ans);
	return knl_pn_list(p_lw);
}

/* An input to the open list: on a row of it, at a time given in ms */
LOCAL BOOL popup_row( UINT type, INT lp, INT row, UD ms )
{
	T_WMEV	ev;

	ev_row(&ev, type, lp, row);
	ev.when = ms * 1000000ULL;
	return knl_pn_list_event(&ev);
}

LOCAL BOOL popup_key( UINT code )
{
	T_WMEV	ev;

	ev_key(&ev, code, 0);
	return knl_pn_list_event(&ev);
}

/*
 * A pop-up: pressed, its list opens in a window of its own with the row
 * of the name chosen over the part. A click leaves the list standing; a
 * row pressed and let go on is chosen; held and let go on another row
 * at once, that row is chosen. Up and Down move the lit row, Enter
 * chooses it, Escape puts the list away unchanged, and so does a press
 * off it. Closing the panel takes the list with it. A panel that warns
 * wears its frame.
 */
LOCAL void test_popup_warn( void )
{
	T_WMPANEL	*d;
	T_WMEV		ev;
	T_WMWIN		pw, lwin;
	T_DPRECT	mr;
	INT		pid, at0, i, v = 0, lp, lw = 0, mid;
	UB		name[8];

	if ( !ready ) KT_SKIP("no window");

	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 15;  d->owner = 0;  d->kind = WM_PNL_WARN;
	d->r.left = 40;  d->r.top = 260;  d->r.right = 280;  d->r.bottom = 340;
	d->npart = 1;
	d->used = 0;
	part_set(&d->part[0], WM_PT_POPUP, 1, 10, 10, 200, 44);
	at0 = -1;
	for ( i = 0; i < 3; i++ ) {
		INT	k;

		name[0] = 'h';  name[1] = 'i';  name[2] = (UB)('1' + i);  name[3] = 0;
		k = wm_panel_name(d, name);
		KT_ASSERT(k >= 0);
		if ( at0 < 0 ) at0 = k;
	}
	d->part[0].pool = at0;
	d->part[0].count = 3;
	d->part[0].value = 2;

	pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	ev_at(&ev, HID_EV_MOVE, 5, 5);
	KT_ASSERT(!knl_pn_list_event(&ev));	/* none open: nothing taken */

	/* pressed: the list opens, the row of hi2 over the part */
	lp = popup_press(pid, 1000, &lw);
	KT_ASSERT(lp > 0);
	if ( lp <= 0 ) {
		(void)wm_panel_close(pid);
		return;
	}
	KT_ASSERT(lw > 0 && lw != wid);
	KT_ASSERT_ER(wm_ref(wid, &pw), E_OK);
	KT_ASSERT_ER(wm_ref(lw, &lwin), E_OK);
	KT_ASSERT_ER(wm_panel_rect(lp, &mr), E_OK);
	mid = pw.work.top + 260 + ( 10 + 44 ) / 2;
	tm_printf((UB *)"  list at %d..%d, row %d, the part's middle %d\n",
		  lwin.work.top + mr.top, lwin.work.top + mr.bottom, wm_menu_row_h(), mid);
	KT_ASSERT(mid >= lwin.work.top + 1 + wm_menu_row_h() && mid < lwin.work.top + 1 + 2 * wm_menu_row_h() + 2);
	KT_ASSERT(lwin.work.right - lwin.work.left >= 190 - 4);	/* as wide as the part */

	/* the click's release: the list stays */
	KT_ASSERT(popup_row(HID_EV_BTN_UP, lp, 1, 1050));
	KT_ASSERT_EQ(knl_pn_list(NULL), lp);
	/* the second button chooses nothing */
	ev_row(&ev, HID_EV_BTN_DOWN, lp, 2);
	ev.code = 1;
	KT_ASSERT(knl_pn_list_event(&ev));
	KT_ASSERT_EQ(knl_pn_list(NULL), lp);
	/* hi3 pressed and let go on: chosen, the list gone */
	KT_ASSERT(popup_row(HID_EV_MOVE, lp, 2, 3000));
	KT_ASSERT(popup_row(HID_EV_BTN_DOWN, lp, 2, 3000));
	KT_ASSERT(popup_row(HID_EV_BTN_UP, lp, 2, 3100));
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT(wm_ref(lw, &lwin) < E_OK);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 3);

	/* held and let go on hi1 at once: chosen */
	lp = popup_press(pid, 5000, &lw);
	KT_ASSERT(lp > 0);
	KT_ASSERT(popup_row(HID_EV_MOVE, lp, 0, 5040));
	KT_ASSERT(popup_row(HID_EV_BTN_UP, lp, 0, 5080));
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 1);

	/* Down and Enter: hi2 */
	lp = popup_press(pid, 7000, &lw);
	KT_ASSERT(lp > 0);
	KT_ASSERT(popup_key(0x51));
	KT_ASSERT(popup_key(0x28));
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 2);

	/* Up, then Escape: unchanged */
	lp = popup_press(pid, 9000, &lw);
	KT_ASSERT(lp > 0);
	KT_ASSERT(popup_key(0x52));
	KT_ASSERT(popup_key(0x29));
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 2);

	/* a press off the list puts it away unchanged, and goes no further */
	lp = popup_press(pid, 11000, &lw);
	KT_ASSERT(lp > 0);
	ev_at(&ev, HID_EV_BTN_DOWN, 5, 5);
	KT_ASSERT(knl_pn_list_event(&ev));
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT_ER(wm_panel_get(pid, 1, &v), E_OK);
	KT_ASSERT_EQ(v, 2);

	/* closing the panel takes the list with it */
	lp = popup_press(pid, 13000, &lw);
	KT_ASSERT(lp > 0);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	KT_ASSERT_EQ(knl_pn_list(NULL), 0);
	KT_ASSERT(wm_ref(lw, &lwin) < E_OK);
}

/*
 * A pop-up written in a data box's panel: システム環境設定's フォント設定
 * has two, 個別定義 and ファミリ, whose names the program sets.
 */
LOCAL void test_popup_box( void )
{
	T_WMPANEL	*d;
	TS_UUID		box;
	INT		i, n = 0;
	ER		er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_BOX_SYSENV, &box), E_OK);
	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	er = knl_pn_load(&box, 14, d, NULL, 0);
	if ( er == E_NOEXS ) {
		Kfree(d);
		KT_SKIP("no system data box");
	}
	KT_ASSERT_ER(er, E_OK);
	for ( i = 0; i < d->npart; i++ ) {
		if ( WM_PT_KIND(d->part[i].type) == WM_PT_POPUP ) {
			KT_ASSERT(d->part[i].num == 1 || d->part[i].num == 16);
			KT_ASSERT_EQ(d->part[i].count, 0);
			KT_ASSERT(d->part[i].r.bottom - d->part[i].r.top <= 30);
			n++;
		}
	}
	KT_ASSERT_EQ(n, 2);
	Kfree(d);
}

/*
 * A panel that asks something, in a window of its own in the middle of
 * the screen: in front of the window it asks for, the keys given for
 * that window reaching its box, and a press anywhere but on it not an
 * answer.
 */
LOCAL void test_centre( void )
{
	T_WMPANEL	*def;
	T_WMWIN		w;
	T_WMEV		ev;
	INT		pid, pw;
	UINT		ans = WM_ANS_NONE;
	UB		txt[8];

	if ( !ready ) KT_SKIP("no window");
	def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(def != NULL);
	if ( def == NULL ) return;
	{
		UB	*z = (UB *)def;
		SZ	k;

		for ( k = 0; k < (SZ)sizeof(*def); k++ ) z[k] = 0;
	}
	def->num = 7;
	def->r.left = 10;  def->r.top = 10;
	def->r.right = 310;  def->r.bottom = 130;	/* only the size counts */
	def->npart = 2;
	part_set(&def->part[0], WM_PT_BOX, 1, 20, 20, 280, 46);
	part_set(&def->part[1], WM_PT_BUTTON, 2, 200, 80, 280, 106);
	def->part[1].answer = WM_ANS_OK;
	pid = wm_panel_open_centre(wid, def);
	Kfree(def);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;

	pw = wm_panel_wid(pid);
	KT_ASSERT(pw > 0 && pw != wid);
	KT_ASSERT_ER(wm_ref(pw, &w), E_OK);
	{
		T_DISPSPEC	spec;

		if ( ts_disp_ref(&spec) >= E_OK ) {
			KT_ASSERT_EQ(w.outer.left, ( (INT)spec.width - 300 ) / 2);
			KT_ASSERT_EQ(w.outer.top, ( (INT)spec.height - 120 ) / 2);
		}
	}
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/* a key given for the window it asks for goes into its box */
	ev_at(&ev, HID_EV_KEY_DOWN, 0, 0);
	ev.code = 0x04;			/* a */
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_ER(wm_panel_text(pid, 1, txt, sizeof(txt)), E_OK);
	KT_ASSERT_EQ((INT)txt[0], 'a');

	/* a press in the window it asks for is not a press on it */
	ev_at(&ev, HID_EV_BTN_DOWN, 240, 90);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 240, 90);
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_NONE);

	/* one on its own button is */
	ev_at(&ev, HID_EV_BTN_DOWN, 240, 90);
	ev.wid = pw;
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	ev_at(&ev, HID_EV_BTN_UP, 240, 90);
	ev.wid = pw;
	KT_ASSERT_ER(wm_panel_event(pid, &ev, &ans), E_OK);
	KT_ASSERT_EQ((INT)ans, WM_ANS_OK);

	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	KT_ASSERT(wm_ref(pw, &w) < E_OK);	/* its window went with it */
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
}

/*
 * A panel whose sheet scrolls (P_SCROLL), drawn by two tasks at once on
 * two processors, as a program writing a part and the input taking a
 * press both draw it: each drawing narrows what may be drawn to the
 * sheet for the parts that scroll and puts it back, so once both are
 * done what may be drawn is what it was, and the panel stays as short as
 * its window lets it.
 */
#define SD_ROUNDS	60

LOCAL INT	sd_pid;
LOCAL INT	sd_left;

LOCAL void sd_draw( INT stacd, void *exinf )
{
	INT	k;

	(void)exinf;
	for ( k = 0; k < SD_ROUNDS; k++ ) {
		(void)wm_panel_draw(sd_pid);
		if ( ( k & 7 ) == stacd ) {
			(void)knl_pn_scroll(wid, k * 3);	/* the bar worked meanwhile */
		}
		if ( stacd == 1 ) {
			(void)wm_panel_set_text(sd_pid, 1 + k % 10, ( k & 1 ) ? (CONST UB *)"abc" : (CONST UB *)"");	/* a program writing a part */
		}
	}
	sd_left--;
	tk_exd_tsk();
}

LOCAL void test_scroll_draw( void )
{
	T_WMPANEL	*d;
	T_DPENV		e0, e1;
	T_DPRECT	pr;
	T_CTSK		ctsk;
	ID		t[2];
	INT		gid, i, k, at;

	if ( !ready ) KT_SKIP("no window");
	d = (T_WMPANEL *)Kcalloc(1, sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) return;
	d->num = 30;  d->owner = 0;  d->kind = WM_PNL_BARE;
	d->r.left = 0;  d->r.top = 0;  d->r.right = 400;  d->r.bottom = 700;
	part_set(&d->part[0], WM_PT_TAGS | P_SCROLL, 100, 0, 0, 400, 700);
	d->part[0].inner.left = 4;  d->part[0].inner.top = 28;
	d->part[0].inner.right = 396;  d->part[0].inner.bottom = 696;
	at = wm_panel_name(d, (CONST UB *)"a");
	(void)wm_panel_name(d, (CONST UB *)"b");
	d->part[0].pool = at;
	d->part[0].count = 2;
	d->part[0].value = 1;
	for ( i = 1; i <= 10; i++ ) {
		part_set(&d->part[i], WM_PT_BOX, i, 10, 40 + ( i - 1 ) * 60, 380, 66 + ( i - 1 ) * 60);
		d->part[i].sheet = 1;
	}
	part_set(&d->part[11], WM_PT_BUTTON, 20, 300, 660, 380, 684);
	d->part[11].type |= P_FOOT;
	d->part[11].sheet = 1;
	d->npart = 12;
	sd_pid = wm_panel_open(wid, d);
	Kfree(d);
	KT_ASSERT(sd_pid > 0);
	if ( sd_pid <= 0 ) return;
	KT_ASSERT_ER(wm_panel_rect(sd_pid, &pr), E_OK);
	KT_ASSERT(pr.bottom - pr.top < 700);		/* made as short as the window */
	gid = wm_gid(wid);
	KT_ASSERT_ER(dp_ref(gid, &e0), E_OK);

	sd_left = 2;
	for ( k = 0; k < 2; k++ ) {
		{ UB *z = (UB *)&ctsk; SZ n; for ( n = 0; n < (SZ)sizeof(ctsk); n++ ) z[n] = 0; }
		ctsk.tskatr  = TA_HLNG | TA_RNG0;
		ctsk.task    = (FP)sd_draw;
		ctsk.itskpri = KT_PRI_LOW;
		ctsk.stksz   = 32768;		/* the letters are made on the way */
		t[k] = tk_cre_tsk(&ctsk);	/* free to run on any processor */
		KT_ASSERT(t[k] > 0);
		if ( t[k] > 0 ) {
			KT_ASSERT_ER(tk_sta_tsk(t[k], k), E_OK);
		} else {
			sd_left--;
		}
	}
	for ( k = 0; k < 600 && sd_left > 0; k++ ) {
		tk_dly_tsk(50);
	}
	KT_ASSERT_EQ(sd_left, 0);
	KT_ASSERT_ER(dp_ref(gid, &e1), E_OK);
	tm_printf((UB *)"  what may be drawn %d,%d-%d,%d before, %d,%d-%d,%d after\n", e0.visible.left,
		  e0.visible.top, e0.visible.right, e0.visible.bottom, e1.visible.left, e1.visible.top,
		  e1.visible.right, e1.visible.bottom);
	KT_ASSERT(e1.visible.left == e0.visible.left && e1.visible.top == e0.visible.top
		  && e1.visible.right == e0.visible.right && e1.visible.bottom == e0.visible.bottom);
	KT_ASSERT_ER(wm_panel_rect(sd_pid, &pr), E_OK);
	KT_ASSERT(pr.bottom - pr.top < 700);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
	KT_ASSERT_ER(wm_panel_close(sd_pid), E_OK);
}

EXPORT void ktest_part( void )
{
	KT_RUN(test_start);
	KT_RUN(test_copied);
	KT_RUN(test_choice);
	KT_RUN(test_answer);
	KT_RUN(test_escape);
	KT_RUN(test_menu);
	KT_RUN(test_menu_many);
	KT_RUN(test_menu_sub);
	KT_RUN(test_label);
	KT_RUN(test_list);
	KT_RUN(test_box);
	KT_RUN(test_menu_look);
	KT_RUN(test_centre);
	KT_RUN(test_volume);
	KT_RUN(test_dbox);
	KT_RUN(test_boxes);
	KT_RUN(test_popup_warn);
	KT_RUN(test_popup_box);
	KT_RUN(test_scroll_draw);
	KT_RUN(test_limit);
}
