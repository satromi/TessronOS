/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_dtwin.c
 *	The desktop's windows with a part of their own (DTOWN, design 11.1):
 *	ディスク修復 and its 障害情報, スライド; and a record of 実時間制御TAD
 *	played in MPEG Player's window (design 11.12, 15.5.1)
 *
 *	The desktop is started as it starts on the machine and each of them
 *	is opened as a double press opens it, asked of the desktop as a
 *	program asks (DT_RQ_OPEN). The pointer, its buttons and the keys are
 *	put in as the devices would report them, and each window is worked
 *	as any window of the desktop's is:
 *
 *	  the second button in the work area opens the window's menu (a
 *	  popup comes up, and the desktop says so on the console), Escape
 *	  puts it away; a row of it chosen with the first button does what
 *	  the row says -- the menu opens with 閉じる on the pointer's row the
 *	  first time, the rows MN_ROW apart, a line a row too
 *	  the band carries the window and the corner at its foot sizes it
 *	  a letter with Ctrl chooses the item of the menu that has it: Ctrl+E
 *	  closes it, and the part says it closed
 *
 *	What the parts did is read from the console, where they say it.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/proc.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/desktop/desktop.h"

#define LOG_MAX		( 128 * 1024 )
#define MN_ROW		20		/* a row of a menu */
#define MN_AWAY		99		/* no row chosen: the menu put away with Escape */

#define K_ESC		0x29
#define K_E		0x08
#define K_RIGHT		0x4F
#define K_SPACE		0x2C

#define DT_SLIDE_DECK	"72c584f2-af6c-45cb-8dfc-3f181d13cd0a"	/* 実時間TADデモ: three slides */
#define DT_PLAYER_10_4	"01a0d8c5-6a04-7a04-8a04-5e1a7d000004"	/* 10.4 アニメーションとの同期 */

LOCAL UB	*logb;
LOCAL BOOL	up;

/* ---------------------------------------------------------------- the console */

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL INT find( CONST UB *s, INT n, CONST char *w, INT from )
{
	INT	i, k, wl = s_len(w);

	for ( i = from; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return i;
	}
	return -1;
}

LOCAL INT log_from( UD pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(&pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

LOCAL UD log_now( void )
{
	UD	pos = 0;

	while ( tm_log_read(&pos, logb, LOG_MAX) > 0 ) ;
	return pos;
}

/* Waited for (tries tenths of a second) until the console says w after pos */
LOCAL BOOL wait_for( UD pos, CONST char *w, INT tries )
{
	INT	i, n;

	for ( i = 0; i < tries; i++ ) {
		n = log_from(pos, logb, LOG_MAX);
		if ( find(logb, n, w, 0) >= 0 ) return TRUE;
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  waited in vain for \"%s\"\n", w);
	return FALSE;
}

/* ---------------------------------------------------------------- the hand */

LOCAL void hid( UINT type, INT x, INT y, UINT code, UINT mods )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.x = x;
	e.y = y;
	e.code = code;
	e.mods = mods;
	(void)kt_inject(&e);
	tk_dly_tsk(60);
}

/* A button pressed and let go at a place of the screen */
LOCAL void click( INT x, INT y, UINT button )
{
	hid(HID_EV_MOVE, x, y, 0, 0);
	hid(HID_EV_BTN_DOWN, x, y, button, 0);
	hid(HID_EV_BTN_UP, x, y, button, 0);
	tk_dly_tsk(500);			/* not taken for a second press */
}

/* Pressed at one place, carried to another in steps, let go there */
LOCAL void drag( INT x0, INT y0, INT x1, INT y1 )
{
	INT	k;

	hid(HID_EV_MOVE, x0, y0, 0, 0);
	hid(HID_EV_BTN_DOWN, x0, y0, 0, 0);
	for ( k = 1; k <= 8; k++ ) {
		hid(HID_EV_MOVE, x0 + ( x1 - x0 ) * k / 8, y0 + ( y1 - y0 ) * k / 8, 0, 0);
	}
	hid(HID_EV_BTN_UP, x1, y1, 0, 0);
	tk_dly_tsk(300);
}

LOCAL void key( UINT code, UINT mods )
{
	hid(HID_EV_KEY_DOWN, 0, 0, code, mods);
	hid(HID_EV_KEY_UP, 0, 0, code, mods);
	tk_dly_tsk(200);
}

/* ---------------------------------------------------------------- the windows */

/* The window of that title, waited for a few seconds; 0 when there is none */
LOCAL INT win_titled( CONST char *title, INT tries )
{
	INT	wids[32], n, i, k;
	char	t[WM_TITLE_MAX];

	for ( k = 0; k < tries; k++ ) {
		n = wm_list(wids, 32);
		for ( i = 0; i < n; i++ ) {
			if ( wm_title(wids[i], t, sizeof(t)) >= 0 && knl_strcmp(t, title) == 0 ) return wids[i];
		}
		tk_dly_tsk(100);
	}
	return 0;
}

LOCAL INT popups( void )
{
	T_WMWIN	w;
	INT	wid, n = 0;

	for ( wid = 1; wid <= knl_wm_nwin(); wid++ ) {
		if ( wm_ref(wid, &w) >= E_OK && w.visible && ( w.attr & WM_ATTR_POPUP ) != 0 ) n++;
	}
	return n;
}

/* The desktop asked to open an object, as a process asks it */
LOCAL ER ask_open( CONST char *us )
{
	TS_UUID	d;
	T_DTREQ	rq;
	SZ	asz = 0;
	ID	k;
	ER	er;

	knl_memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.seq = 9;
	rq.recno = -1;
	er = ts_str_to_uuid(us, &rq.target);
	if ( er >= E_OK ) er = ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	if ( k <= 0 ) return (ER)k;
	er = ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz);
	ob_cls_obj(k);
	return er;
}

/*
 * The menu of the second button opened at a place of the work area: a
 * popup more, and the desktop's word for it; Escape puts it away, or a
 * row 'rows' below the one on the pointer is chosen (MN_AWAY: none)
 */
LOCAL BOOL menu_at( INT wid, INT dx, INT dy, CONST char *title, INT rows )
{
	T_WMWIN	w;
	INT	before = popups(), i, x, y;
	char	said[WM_TITLE_MAX + 24];
	UD	pos;

	if ( wm_ref(wid, &w) < E_OK ) return FALSE;
	x = w.work.left + dx;
	y = w.work.top + dy;
	pos = log_now();
	click(x, y, 1);
	for ( i = 0; i < 30 && popups() == before; i++ ) tk_dly_tsk(100);
	KT_ASSERT(popups() > before);
	tm_sprintf((UB *)said, (UB *)"desktop: menu %s", title);
	KT_ASSERT(wait_for(pos, said, 30));
	if ( rows == MN_AWAY ) {
		key(K_ESC, 0);
		for ( i = 0; i < 30 && popups() > before; i++ ) tk_dly_tsk(100);
		KT_ASSERT_EQ(popups(), before);
		return TRUE;
	}
	hid(HID_EV_MOVE, x + 8, y + rows * MN_ROW / 2, 0, 0);
	click(x + 8, y + rows * MN_ROW, 0);
	return TRUE;
}

/* The band carries the window, the corner at its foot sizes it */
LOCAL void carry_and_size( INT wid, BOOL sizes )
{
	T_WMWIN	w0, w1, w2;

	KT_ASSERT_ER(wm_ref(wid, &w0), E_OK);
	drag(w0.outer.left + 120, w0.outer.top + 8, w0.outer.left + 150, w0.outer.top + 28);
	KT_ASSERT_ER(wm_ref(wid, &w1), E_OK);
	KT_ASSERT_EQ(w1.outer.left, w0.outer.left + 30);
	KT_ASSERT_EQ(w1.outer.top, w0.outer.top + 20);
	KT_ASSERT_EQ(w1.outer.right - w1.outer.left, w0.outer.right - w0.outer.left);
	if ( !sizes ) return;
	drag(w1.outer.right - 3, w1.outer.bottom - 3, w1.outer.right + 37, w1.outer.bottom + 27);
	KT_ASSERT_ER(wm_ref(wid, &w2), E_OK);
	KT_ASSERT_EQ(w2.outer.left, w1.outer.left);
	KT_ASSERT_EQ(w2.outer.right - w2.outer.left, w1.outer.right - w1.outer.left + 40);
	KT_ASSERT_EQ(w2.outer.bottom - w2.outer.top, w1.outer.bottom - w1.outer.top + 30);
}

/* ---------------------------------------------------------------- the tests */

LOCAL void test_setup( void )
{
	UD	pos;
	INT	i;

	pos = log_now();
	KT_ASSERT_ER(dt_start(), E_OK);
	up = wait_for(pos, "TessronOS desktop: start", 100);
	KT_ASSERT(up);
	if ( !up ) return;
	for ( i = 0; i < 50; i++ ) {
		TS_UUID	d;

		if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) >= E_OK ) break;
		tk_dly_tsk(100);
	}
	tk_dly_tsk(1000);			/* the first cabinet opened, and drawn */
}

/*
 * ディスク修復: its menu; ドライブの一覧を更新 and 完全 chosen in it, the
 * window carried and sized, 障害情報 grey until a check found something,
 * and closed with Ctrl+E
 */
LOCAL void test_diskfix( void )
{
	UD	pos;
	INT	wid;

	if ( !up ) KT_SKIP("no desktop");
	pos = log_now();
	KT_ASSERT_ER(ask_open(SYSDEF_PROG_DISKFIX), E_OK);
	/* opened, or brought to the front when the repair at start (the dfix suite's) had opened it */
	(void)wait_for(pos, "dfix: opened", 30);
	wid = win_titled("ディスク修復", 30);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	tk_dly_tsk(300);
	(void)menu_at(wid, 300, 280, "ディスク修復", MN_AWAY);
	(void)kt_shot("/boot/DTWIN0.PPM");
	/* 完全, the tenth row below 閉じる */
	pos = log_now();
	(void)menu_at(wid, 300, 120, "ディスク修復", 10);
	KT_ASSERT(wait_for(pos, "desktop: chosen dfix.full", 30));
	/* ドライブの一覧を更新, two rows below 完全 now on the pointer's row */
	pos = log_now();
	(void)menu_at(wid, 300, 120, "ディスク修復", 2);
	KT_ASSERT(wait_for(pos, "desktop: chosen dfix.list", 30));
	carry_and_size(wid, TRUE);
	pos = log_now();
	key(K_E, HID_MOD_LCTRL);
	KT_ASSERT(wait_for(pos, "dfix: closed", 30));
	KT_ASSERT_EQ(win_titled("ディスク修復", 3), 0);
}

/*
 * スライド: over the whole screen with no frame; its menu, 次へ進む
 * chosen in it, a key of its own, and closed with Ctrl+E
 */
LOCAL void test_slide( void )
{
	UD	pos;
	INT	wid;

	if ( !up ) KT_SKIP("no desktop");
	pos = log_now();
	KT_ASSERT_ER(ask_open(DT_SLIDE_DECK), E_OK);
	if ( !wait_for(pos, "slide: 1 of", 80) ) {
		KT_SKIP("no slides to show");
	}
	/* a window with no band has no name in the list of windows: the one that has the keys */
	tk_dly_tsk(300);
	wid = wm_focused();
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	{
		T_WMWIN	w;
		INT	sw = 0, sh = 0;

		wm_screen(&sw, &sh);
		KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
		KT_ASSERT_EQ(w.outer.left, 0);
		KT_ASSERT_EQ(w.outer.right, sw);
		KT_ASSERT_EQ(w.work.bottom, sh);		/* over the message line too */
	}
	(void)menu_at(wid, 640, 400, "実時間TADデモ", MN_AWAY);
	(void)kt_shot("/boot/DTWIN1.PPM");
	pos = log_now();
	(void)menu_at(wid, 640, 300, "実時間TADデモ", 2);		/* 次へ進む */
	KT_ASSERT(wait_for(pos, "desktop: chosen slide.next", 30));
	KT_ASSERT(wait_for(pos, "slide: 2 of", 30));
	pos = log_now();
	key(K_RIGHT, 0);
	KT_ASSERT(wait_for(pos, "slide: 3 of", 30));
	pos = log_now();
	key(K_E, HID_MOD_LCTRL);
	KT_ASSERT(wait_for(pos, "slide: closed", 30));
	KT_ASSERT(wm_focused() != wid);
}

/*
 * MPEG Player playing a record of 実時間制御TAD: opened as a double press
 * opens it (its applist names basic-player, the player's alias), the
 * scene handed to the desktop, which plays it in the player's window
 * (dtrthost.c); a press there given to the program, Space begins or
 * holds it, the pointer's moves given to it, the window sized (the scene laid out again), closed with
 * Ctrl+E -- the player let go by its process -- and opened again and
 * ended from outside, the player going with the window
 */
LOCAL void test_player( void )
{
	UD	pos;
	INT	wid;
	ID	pid;
	T_WMWIN	w;

	if ( !up ) KT_SKIP("no desktop");
	pos = log_now();
	KT_ASSERT_ER(ask_open(DT_PLAYER_10_4), E_OK);
	if ( !wait_for(pos, "player: hosted", 150) ) {
		KT_SKIP("no MPEG Player to play 実時間制御TAD");
	}
	wid = win_titled("MPEG Player", 30);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_EQ(dt_rthost_count(), 1);
	tk_dly_tsk(800);
	(void)kt_shot("/boot/DTWIN2.PPM");

	/* a press on the scene is the program's; the window has the keys after it */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	pos = log_now();
	click(w.work.left + 200, w.work.top + 150, 0);
	KT_ASSERT(wait_for(pos, "op press", 30));
	/* the pointer over the scene goes to the program too (mouseenter, mouseleave) */
	hid(HID_EV_MOVE, w.work.left + 260, w.work.top + 170, 0, 0);
	hid(HID_EV_MOVE, w.work.left + 300, w.work.top + 190, 0, 0);
	KT_ASSERT(wait_for(pos, "op move", 30));
	pos = log_now();
	key(K_SPACE, 0);
	KT_ASSERT(wait_for(pos, "op toggle", 30));
	tk_dly_tsk(500);
	(void)kt_shot("/boot/DTWIN3.PPM");

	/* the corner sizes the window, and the scene is laid out in the new video area */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	pos = log_now();
	drag(w.outer.right - 3, w.outer.bottom - 3, w.outer.right + 77, w.outer.bottom + 47);
	KT_ASSERT(wait_for(pos, "op area", 50));

	pos = log_now();
	key(K_E, HID_MOD_LCTRL);
	KT_ASSERT(wait_for(pos, "closed by its process", 100));
	KT_ASSERT_EQ(dt_rthost_count(), 0);
	tk_dly_tsk(500);
	KT_ASSERT_EQ(win_titled("MPEG Player", 30), 0);

	/* ended from outside: the player goes with the window */
	pos = log_now();
	KT_ASSERT_ER(ask_open(DT_PLAYER_10_4), E_OK);
	KT_ASSERT(wait_for(pos, "player: hosted", 150));
	wid = win_titled("MPEG Player", 30);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	pid = knl_wmobj_maker(wid);
	KT_ASSERT(pid > 0);
	pos = log_now();
	KT_ASSERT_ER(ts_ter_prc(pid, TS_ABORT_TERM), E_OK);
	KT_ASSERT(wait_for(pos, "closed with its window", 50));
	KT_ASSERT_EQ(dt_rthost_count(), 0);
}

/* ---------------------------------------------------------------- a finger */

#if defined(_QEMU_VIRT_) || defined(X64)
IMPORT BOOL kt_touch_plug( void );
IMPORT void kt_touch_at( INT id, BOOL on, INT sx, INT sy );
IMPORT void kt_touch_unplug( void );

LOCAL INT	finger = 40;

/* A finger touching at one place, carried to another in steps, let go there */
LOCAL void finger_drag( INT x0, INT y0, INT x1, INT y1 )
{
	INT	k;

	finger++;
	kt_touch_at(finger, TRUE, x0, y0);
	tk_dly_tsk(60);
	for ( k = 1; k <= 8; k++ ) {
		kt_touch_at(finger, TRUE, x0 + ( x1 - x0 ) * k / 8, y0 + ( y1 - y0 ) * k / 8);
		tk_dly_tsk(60);
	}
	kt_touch_at(finger, FALSE, x1, y1);
	tk_dly_tsk(600);			/* not the second of a double tap */
}

/* Link shape i of a figure, where it is on the screen (a cabinet is shown at life size) */
LOCAL BOOL link_screen( DTWIN *d, INT i, T_DPRECT *b )
{
	T_WMWIN		w;
	T_DPRECT	page;
	INT		z;

	if ( d->fig == NULL || i >= d->fig->nsh || wm_ref(d->wid, &w) < E_OK ) return FALSE;
	z = ( d->fig->zoom > 0 ) ? d->fig->zoom : 8;
	dt_work_rect(d, &page);
	b->left = w.work.left + page.left - d->scroll_x + d->fig->sh[i].r.left * z / 8;
	b->top = w.work.top + page.top - d->scroll_y + d->fig->sh[i].r.top * z / 8;
	b->right = w.work.left + page.left - d->scroll_x + d->fig->sh[i].r.right * z / 8;
	b->bottom = w.work.top + page.top - d->scroll_y + d->fig->sh[i].r.bottom * z / 8;
	return TRUE;
}

/* A real object of the store with a record 0 of xmlTAD, the program app its own */
LOCAL ER make_obj( CONST char *name, CONST char *app, CONST char *win, CONST char *xml, TS_UUID *u )
{
	char	json[320];
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	tm_sprintf((UB *)json, (UB *)"{\"name\":\"%s\",\"applist\":{\"%s\":{\"name\":\"x\",\"defaultOpen\":true}}%s}",
		   name, app, win);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ts_str_to_uuid(SYSDEF_CABINET, &c.near);
	if ( er >= E_OK ) er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

/* Link lk of a figure's record, where it is in the figure: what sizing it changes */
LOCAL BOOL link_rec( DTWIN *d, INT lk, T_DPRECT *b )
{
	T_VOBJ	v;

	if ( tad_lnk_get(d->rec, lk, &v) < E_OK ) return FALSE;
	b->left = v.left;
	b->top = v.top;
	b->right = v.right;
	b->bottom = v.bottom;
	return TRUE;
}

/* Whether a place of the screen is on none of the figure's links, and on its window */
LOCAL BOOL bare_at( DTWIN *d, INT x, INT y )
{
	T_DPRECT	b;
	INT		k;

	if ( wm_at(x, y) != d->wid ) return FALSE;
	for ( k = 0; k < d->fig->nsh; k++ ) {
		if ( d->fig->sh[k].kind == TV_SH_LINK && link_screen(d, k, &b)
		  && x >= b.left - 2 && x < b.right + 2 && y >= b.top - 2 && y < b.bottom + 2 ) return FALSE;
	}
	return TRUE;
}

/*
 * The handles a finger reaches farther (design 10.4.6): in the first
 * cabinet, a link taken; the pointer pressed just out of its right edge
 * draws a band and sizes nothing, a finger pressed there sizes it. The
 * window's foot corner: the pointer pressed just out of the window
 * sizes nothing, a finger pressed there sizes it.
 */
LOCAL void test_touch_reach( void )
{
	DTWIN		*d = NULL;
	T_DPRECT	b, a;
	T_WMWIN		w0, w1;
	INT		i, k = -1, lk, px, py, sw = 0, sh = 0, wid, behind;
	TS_UUID		cu;
	char		us[40];
	static char	xml[512];

	if ( !up ) KT_SKIP("no desktop");
	knl_memset(&a, 0, sizeof(a));
	/* a cabinet of one link, in a window of its own */
	tm_sprintf((UB *)xml, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000e001\" vobjleft=\"20\" vobjtop=\"20\""
		   " vobjright=\"170\" vobjbottom=\"45\" height=\"25\" chsz=\"14\"/></figure></tad>\n", SYSDEF_CABINET);
	KT_ASSERT_ER(make_obj("KT指の箱", "virtual-object-list",
			      ",\"window\":{\"pos\":{\"x\":20,\"y\":80},\"width\":320,\"height\":240}", xml, &cu), E_OK);
	ts_uuid_to_str(&cu, us, sizeof(us));
	KT_ASSERT_ER(ask_open(us), E_OK);
	wid = win_titled("KT指の箱", 50);
	KT_ASSERT(wid > 0);
	tk_dly_tsk(500);
	d = ( wid > 0 ) ? dt_win_of(wid) : NULL;
	for ( i = 0; d != NULL && d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->fig->sh[i].kind == TV_SH_LINK && d->fig->sh[i].link >= 0 ) {
			k = i;
			break;
		}
	}
	if ( d == NULL || k < 0 ) KT_SKIP("the cabinet not shown");
	(void)link_screen(d, k, &b);
	if ( !bare_at(d, b.right + 6, b.top + 5) ) d = NULL;
	if ( d == NULL ) KT_SKIP("no link with room beside it");
	if ( !kt_touch_plug() ) KT_SKIP("no touch screen plugged in");

	/* taken by the pointer */
	lk = d->fig->sh[k].link;
	(void)link_screen(d, k, &b);
	tm_printf((UB *)"  link %d at %d,%d..%d,%d\n", k, b.left, b.top, b.right, b.bottom);
	click(( b.left + b.right ) / 2, ( b.top + b.bottom ) / 2, 0);
	KT_ASSERT(ed_picked(d, k));
	px = b.right + 6;
	py = b.top + 5;
	/* the pointer on its right edge: it is sized, as it always was */
	drag(b.right - 2, py, b.right + 18, py);
	tk_dly_tsk(300);
	KT_ASSERT(link_rec(d, lk, &a));
	tm_printf((UB *)"  by the pointer on its edge: %d wide, was %d\n", a.right - a.left, b.right - b.left);
	KT_ASSERT_EQ(a.right - a.left, b.right - b.left + 20);
	ed_undo(d);
	tk_dly_tsk(300);
	tk_dly_tsk(300);
	KT_ASSERT(link_rec(d, lk, &a));
	KT_ASSERT_EQ(a.right - a.left, b.right - b.left);
	/* the pointer 6 pixels out of its right edge: a band, the link as it was */
	drag(px, py, px + 40, py);
	tk_dly_tsk(300);
	KT_ASSERT(link_rec(d, lk, &a));
	KT_ASSERT_EQ(a.right - a.left, b.right - b.left);
	/* taken again, and a finger there: it is sized */
	click(( b.left + b.right ) / 2, ( b.top + b.bottom ) / 2, 0);
	KT_ASSERT(ed_picked(d, k));
	finger_drag(px, py, px + 40, py);
	tk_dly_tsk(300);
	KT_ASSERT(link_rec(d, lk, &a));
	tm_printf((UB *)"  by a finger 6 pixels out: %d wide, was %d\n", a.right - a.left, b.right - b.left);
	KT_ASSERT(a.right - a.left >= b.right - b.left + 30);
	ed_undo(d);				/* taken back, the cabinet as it was */
	tk_dly_tsk(300);

	/* the window's corner, 6 pixels out of it, over a bare place: the window put where there is one */
	wm_screen(&sw, &sh);
	KT_ASSERT_ER(wm_ref(d->wid, &w0), E_OK);
	for ( i = 0; i < 6; i++ ) {
		T_DPRECT	o = w0.outer;
		INT		cx = sw - 80 - ( i % 3 ) * ( sw / 4 ), cy = sh - 120 - ( i / 3 ) * ( sh / 4 );

		o.right = cx;
		o.bottom = cy;
		o.left = cx - ( w0.outer.right - w0.outer.left );
		o.top = cy - ( w0.outer.bottom - w0.outer.top );
		if ( o.left < 0 || o.top < 0 ) continue;
		(void)wm_move(d->wid, &o);
		wm_update();
		tk_dly_tsk(200);
		KT_ASSERT_ER(wm_ref(d->wid, &w0), E_OK);
		px = w0.outer.right + 6;
		py = w0.outer.bottom + 6;
		behind = wm_at(px, py);
		if ( behind == 0 || ( dt_win_of(behind) != NULL && dt_win_of(behind)->fig != NULL
				   && bare_at(dt_win_of(behind), px, py) && bare_at(dt_win_of(behind), px + 30, py + 20) ) ) break;
	}
	if ( i == 6 || ( w0.attr & WM_ATTR_RESIZE ) == 0 ) {
		tm_printf((UB *)"  the window's corner: nothing bare out of it, not tried\n");
		kt_touch_unplug();
		return;
	}
	tm_printf((UB *)"  the window at %d,%d..%d,%d\n", w0.outer.left, w0.outer.top, w0.outer.right, w0.outer.bottom);
	drag(px, py, px + 30, py + 20);
	KT_ASSERT_ER(wm_ref(d->wid, &w1), E_OK);
	KT_ASSERT_EQ(w1.outer.right, w0.outer.right);
	KT_ASSERT_EQ(w1.outer.bottom, w0.outer.bottom);
	finger_drag(px, py, px + 30, py + 20);
	KT_ASSERT_ER(wm_ref(d->wid, &w1), E_OK);
	tm_printf((UB *)"  the window by a finger out of its corner: %d by %d, was %d by %d\n",
		  w1.outer.right - w1.outer.left, w1.outer.bottom - w1.outer.top,
		  w0.outer.right - w0.outer.left, w0.outer.bottom - w0.outer.top);
	KT_ASSERT_EQ(w1.outer.right - w1.outer.left, w0.outer.right - w0.outer.left + 30);
	KT_ASSERT_EQ(w1.outer.bottom - w1.outer.top, w0.outer.bottom - w0.outer.top + 20);
	kt_touch_unplug();
}
#endif

EXPORT void ktest_dtwin( void )
{
	logb = (UB *)Kmalloc(LOG_MAX);
	if ( logb == NULL ) return;
	knl_ob_make_for_person(TRUE);
	KT_RUN(test_setup);
	KT_RUN(test_diskfix);
	KT_RUN(test_slide);
	KT_RUN(test_player);
#if defined(_QEMU_VIRT_) || defined(X64)
	KT_RUN(test_touch_reach);
#endif
	if ( up ) {
		dt_quit();
		up = FALSE;
	}
	wm_update();
	knl_ob_make_for_person(FALSE);
	Kfree(logb);
	logb = NULL;
}
