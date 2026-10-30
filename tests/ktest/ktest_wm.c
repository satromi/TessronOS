/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wm.c
 *	Windows (design 16.5.5, stage 10a).
 *
 *	The self check is run after every case, not only at the end. What
 *	it looks at are the three things the whole design rests on: that
 *	the order is a permutation, that a work area is what the single
 *	function says it is, and that no window's surface is too small for
 *	it. A fault in any of them corrupts memory rather than looking
 *	wrong, so they are worth checking after every step.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/wm.h>
#include <ts/fn.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <ts/hid.h>
#include <ts/conf.h>
#include <ts/look.h>
#include <ts/time.h>
#include <ts/snd.h>
#include "../../outer_kernel/wm/wmobj.h"
#include "../../outer_kernel/wm/compose.h"
#include "../../device/disp/disp_hw.h"

IMPORT ID knl_wm_compositor( void );

LOCAL BOOL		have_wm = FALSE;
LOCAL T_DISPSPEC	spec;

/* What the screen's back buffer holds */
LOCAL UW at( INT x, INT y )
{
	UB	*p = (UB *)ts_disp_buffer();

	return *(UW *)(p + (UBINT)y * spec.pitch + (UBINT)x * 4) & 0x00FFFFFF;
}

LOCAL void rect( T_DPRECT *r, INT l, INT t, INT rr, INT b )
{
	r->left = l;  r->top = t;  r->right = rr;  r->bottom = b;
}

/* a window opens with a work area inside its frame */
LOCAL void test_open( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	rect(&o, 100, 100, 300, 250);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "one");
	if ( wid < 0 ) {
		KT_SKIP("windows did not start");
	}
	have_wm = TRUE;

	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_EQ(w.outer.left, 100);
	KT_ASSERT_EQ(w.outer.right, 300);
	KT_ASSERT_EQ(w.z, 0);
	KT_ASSERT_EQ(w.visible, 1);

	/* the work area is inside the outer rectangle on every side */
	KT_ASSERT(w.work.left > w.outer.left);
	KT_ASSERT(w.work.top > w.outer.top);
	KT_ASSERT(w.work.right < w.outer.right);
	KT_ASSERT(w.work.bottom < w.outer.bottom);
	/* and the title bar makes the top deeper than the sides */
	KT_ASSERT(w.work.top - w.outer.top > w.work.left - w.outer.left);

	/* a window with no frame has a work area equal to the whole of it */
	{
		INT	bare = wm_open(&o, 0, "bare");

		KT_ASSERT(bare >= 1);
		KT_ASSERT_ER(wm_ref(bare, &w), E_OK);
		KT_ASSERT_EQ(w.work.left, w.outer.left);
		KT_ASSERT_EQ(w.work.top, w.outer.top);
		KT_ASSERT_EQ(w.work.right, w.outer.right);
		KT_ASSERT_EQ(w.work.bottom, w.outer.bottom);
		KT_ASSERT_ER(wm_close(bare), E_OK);
	}

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_close(wid), E_ID);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

/* the three rectangles convert to one another and back again */
LOCAL void test_convert( void )
{
	T_DPRECT	o, in, mid, out;
	T_WMWIN		w;
	INT		wid;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 200, 150, 500, 400);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "two");
	KT_ASSERT(wid >= 1);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_ER(wm_set_scroll(wid, 40, 25), E_OK);

	/* the corner of the work area, in screen terms, is the origin */
	rect(&in, 0, 0, 10, 10);
	KT_ASSERT_ER(wm_convert(wid, WM_RECT_WORK, WM_RECT_OUTER, &in, &out), E_OK);
	KT_ASSERT_EQ(out.left, w.work.left);
	KT_ASSERT_EQ(out.top, w.work.top);
	KT_ASSERT_EQ(out.right, w.work.left + 10);

	/* and back again gives what went in */
	KT_ASSERT_ER(wm_convert(wid, WM_RECT_OUTER, WM_RECT_WORK, &out, &mid), E_OK);
	KT_ASSERT_EQ(mid.left, in.left);
	KT_ASSERT_EQ(mid.top, in.top);
	KT_ASSERT_EQ(mid.right, in.right);
	KT_ASSERT_EQ(mid.bottom, in.bottom);

	/* the drawing rectangle is the work area before it was wound up */
	KT_ASSERT_ER(wm_convert(wid, WM_RECT_WORK, WM_RECT_DRAW, &in, &out), E_OK);
	KT_ASSERT_EQ(out.left, in.left + 40);
	KT_ASSERT_EQ(out.top, in.top + 25);
	KT_ASSERT_ER(wm_convert(wid, WM_RECT_DRAW, WM_RECT_WORK, &out, &mid), E_OK);
	KT_ASSERT_EQ(mid.left, in.left);
	KT_ASSERT_EQ(mid.top, in.top);

	/* every pair of the three, there and back, ends where it began */
	{
		UINT	a, b;

		for ( a = WM_RECT_OUTER; a <= WM_RECT_DRAW; a++ ) {
			for ( b = WM_RECT_OUTER; b <= WM_RECT_DRAW; b++ ) {
				rect(&in, 7, 11, 23, 29);
				KT_ASSERT_ER(wm_convert(wid, a, b, &in, &mid), E_OK);
				KT_ASSERT_ER(wm_convert(wid, b, a, &mid, &out), E_OK);
				KT_ASSERT_EQ(out.left, in.left);
				KT_ASSERT_EQ(out.top, in.top);
				KT_ASSERT_EQ(out.right, in.right);
				KT_ASSERT_EQ(out.bottom, in.bottom);
			}
		}
	}
	KT_ASSERT_ER(wm_convert(wid, 99, 0, &in, &out), E_PAR);

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
}

/* a program draws into its own window and the screen shows it */
LOCAL void test_draw( void )
{
	T_DPRECT	o, r;
	T_WMWIN		w;
	INT		wid, gid;

	if ( !have_wm ) KT_SKIP("no windows");

	ts_disp_clear(0x00000000);

	rect(&o, 400, 300, 600, 420);
	wid = wm_open(&o, 0, "plain");		/* no frame: work area is all of it */
	KT_ASSERT(wid >= 1);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);

	gid = wm_gid(wid);
	KT_ASSERT(gid >= 1);

	/* the program's own coordinates start at the corner of its work area */
	rect(&r, 0, 0, 50, 20);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00224466), E_OK);

	/* nothing has reached the screen yet */
	KT_ASSERT_EQ(at(400, 300), 0);

	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(400, 300), 0x224466);
	KT_ASSERT_EQ(at(449, 319), 0x224466);
	/* past the rectangle it drew: the work area as the look says */
	KT_ASSERT_EQ((INT)at(450, 300), (INT)wm_look(WM_LOOK_WORK));
	/* outside the window: the desk, which is what the screen is built from */
	KT_ASSERT_EQ(at(399, 300), wm_look(WM_LOOK_DESK));

	/* it cannot reach outside its own window however it tries */
	rect(&r, -1000, -1000, 5000, 5000);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FFFFFF), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(400, 300), 0xFFFFFF);
	KT_ASSERT_EQ(at(599, 419), 0xFFFFFF);
	KT_ASSERT_EQ(at(399, 300), wm_look(WM_LOOK_DESK));
	KT_ASSERT_EQ(at(600, 300), wm_look(WM_LOOK_DESK));
	KT_ASSERT_EQ(at(400, 299), wm_look(WM_LOOK_DESK));
	KT_ASSERT_EQ(at(400, 420), wm_look(WM_LOOK_DESK));

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
}

/* windows in front cover windows behind, in the order they are in */
LOCAL void test_order( void )
{
	T_DPRECT	o, r;
	INT		back, front;

	if ( !have_wm ) KT_SKIP("no windows");

	ts_disp_clear(0x00000000);

	rect(&o, 100, 500, 300, 600);
	back = wm_open(&o, 0, "back");
	KT_ASSERT(back >= 1);
	rect(&r, 0, 0, 200, 100);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(back), &r, 0x00111111), E_OK);

	rect(&o, 200, 500, 400, 600);		/* overlapping the first */
	front = wm_open(&o, 0, "front");
	KT_ASSERT(front >= 1);
	rect(&r, 0, 0, 200, 100);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(front), &r, 0x00222222), E_OK);

	/* the one opened last is in front */
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(150, 550), 0x111111);	/* only the first covers this */
	KT_ASSERT_EQ(at(250, 550), 0x222222);	/* both do; the front wins */
	KT_ASSERT_EQ(at(350, 550), 0x222222);	/* only the second */

	/* bringing the other one up changes what is seen where they meet */
	KT_ASSERT_ER(wm_raise(back), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 550), 0x111111);
	KT_ASSERT_EQ(at(350, 550), 0x222222);

	/* and sending it behind again puts it back */
	KT_ASSERT_ER(wm_lower(back), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 550), 0x222222);

	/* a window hidden is not laid down at all */
	KT_ASSERT_ER(wm_show(front, FALSE), E_OK);
	ts_disp_clear(0x00000000);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 550), 0x111111);
	KT_ASSERT_EQ(at(350, 550), wm_look(WM_LOOK_DESK));

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(front), E_OK);
	KT_ASSERT_ER(wm_close(back), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

/*
 * A floating window (WM_ATTR_FLOAT) stays in front of the others: one
 * opened after it, or brought forward, comes up behind it; a popup is in
 * front of it
 */
LOCAL void test_float( void )
{
	T_DPRECT	o, r;
	INT		fl, plain, pop;

	if ( !have_wm ) KT_SKIP("no windows");

	ts_disp_clear(0x00000000);
	rect(&o, 100, 500, 300, 600);
	fl = wm_open(&o, WM_ATTR_FLOAT, "float");
	KT_ASSERT(fl >= 1);
	rect(&r, 0, 0, 200, 100);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(fl), &r, 0x00333333), E_OK);

	rect(&o, 200, 500, 400, 600);		/* opened later, overlapping it */
	plain = wm_open(&o, 0, "plain");
	KT_ASSERT(plain >= 1);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(plain), &r, 0x00444444), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 550), 0x333333);	/* the floating one is in front */
	KT_ASSERT_EQ(at(350, 550), 0x444444);

	/* brought forward, the other still comes up behind it */
	KT_ASSERT_ER(wm_raise(plain), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 550), 0x333333);

	/* a popup over both */
	rect(&o, 240, 520, 280, 560);
	pop = wm_open(&o, WM_ATTR_POPUP, "pop");
	KT_ASSERT(pop >= 1);
	rect(&r, 0, 0, 40, 40);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(pop), &r, 0x00555555), E_OK);
	KT_ASSERT_ER(wm_raise(pop), E_OK);
	KT_ASSERT_ER(wm_raise(fl), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 530), 0x555555);
	KT_ASSERT_EQ(at(250, 590), 0x333333);

	KT_ASSERT_ER(wm_close(pop), E_OK);
	KT_ASSERT_ER(wm_close(plain), E_OK);
	KT_ASSERT_ER(wm_close(fl), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

/* Whether wid is among the windows shown with a name (the ウインドウ menu's list) */
LOCAL BOOL listed( INT wid )
{
	INT	wids[64], n = wm_list(wids, 64), i;

	for ( i = 0; i < n; i++ ) {
		if ( wids[i] == wid ) return TRUE;
	}
	return FALSE;
}

/*
 * A subordinate window (a tool panel) is out of sight while a window of
 * another group is in front, and comes back just in front of its window
 * when that is in front again; a pop-up, a screen keyboard or a floating
 * window over it does not put it away, a window a program opens does,
 * what its program hid stays hidden,
 * and when its window closes it is its own window, shown again.
 */
LOCAL void test_subs_away( void )
{
	T_DPRECT	o, r;
	T_WMWIN		ws, wa;
	INT		a, s, other, pop, kb, fl, nw;

	if ( !have_wm ) KT_SKIP("no windows");

	ts_disp_clear(0x00000000);
	rect(&r, 0, 0, 400, 300);
	rect(&o, 100, 100, 500, 400);
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "subs owner");
	rect(&o, 520, 100, 900, 400);
	other = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "subs other");
	rect(&o, 150, 420, 350, 520);
	s = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "subs tool");
	KT_ASSERT(a >= 1 && other >= 1 && s >= 1);
	if ( a < 1 || other < 1 || s < 1 ) return;
	KT_ASSERT_ER(dp_fill_rect(wm_gid(s), &r, 0x00666666), E_OK);
	KT_ASSERT_ER(wm_set_parent(s, a), E_OK);

	/* its window in use: shown, just in front of it, the two in front */
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_focus(a), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_ER(wm_ref(a, &wa), E_OK);
	KT_ASSERT_EQ(ws.away, 0);
	KT_ASSERT_EQ(ws.z, 0);
	KT_ASSERT_EQ(wa.z, 1);
	KT_ASSERT(listed(s));
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 480), 0x666666);

	/* another window in use: out of sight, though its program still has it shown */
	KT_ASSERT_ER(wm_raise(other), E_OK);
	KT_ASSERT_ER(wm_focus(other), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_EQ(ws.away, 1);
	KT_ASSERT_EQ(ws.visible, 1);
	KT_ASSERT(!listed(s));
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT(at(250, 480) != 0x666666);
	KT_ASSERT(wm_at(250, 480) != s);

	/* its window in use again: back, just in front of it, the two in front */
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_focus(a), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_ER(wm_ref(a, &wa), E_OK);
	KT_ASSERT_EQ(ws.away, 0);
	KT_ASSERT_EQ(ws.z, 0);
	KT_ASSERT_EQ(wa.z, 1);
	KT_ASSERT(listed(s));
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(250, 480), 0x666666);

	/* the tool window itself pressed and given the keys: its group is the one in use */
	KT_ASSERT_ER(wm_raise(s), E_OK);
	KT_ASSERT_ER(wm_focus(s), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_EQ(ws.away, 0);
	KT_ASSERT_ER(wm_focus(a), E_OK);

	/* a pop-up with the keys, or a screen keyboard pressed: it stays */
	rect(&o, 600, 500, 700, 560);
	pop = wm_open(&o, WM_ATTR_POPUP, NULL);
	KT_ASSERT(pop >= 1);
	if ( pop >= 1 ) {
		KT_ASSERT_ER(wm_focus(pop), E_OK);
		KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
		KT_ASSERT_ER(wm_close(pop), E_OK);
	}
	KT_ASSERT_ER(wm_focus(a), E_OK);
	rect(&o, 600, 600, 800, 700);
	kb = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_NOFOCUS, "subs keyboard");
	KT_ASSERT(kb >= 1);
	if ( kb >= 1 ) {
		KT_ASSERT_ER(wm_raise(kb), E_OK);
		KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
		KT_ASSERT_ER(wm_close(kb), E_OK);
	}

	/* a floating window without the keys stays in front of all: it stays */
	rect(&o, 600, 600, 800, 700);
	fl = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_FLOAT, "subs float");
	KT_ASSERT(fl >= 1);
	if ( fl >= 1 ) {
		KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
		KT_ASSERT_ER(wm_close(fl), E_OK);
	}

	/* a window a program opens comes up in front: out of sight; closed: back */
	rect(&o, 560, 450, 760, 560);
	nw = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "subs new");
	KT_ASSERT(nw >= 1);
	if ( nw >= 1 ) {
		KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 1);
		KT_ASSERT_ER(wm_close(nw), E_OK);
		KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
	}

	/* hidden by its program: hidden whichever group is in use */
	KT_ASSERT_ER(wm_show(s, FALSE), E_OK);
	KT_ASSERT_ER(wm_raise(other), E_OK);
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_EQ(ws.visible, 0);
	KT_ASSERT_EQ(ws.away, 0);
	/* shown by its program while another group is in use: it waits out of sight */
	KT_ASSERT_ER(wm_raise(other), E_OK);
	KT_ASSERT_ER(wm_focus(other), E_OK);
	KT_ASSERT_ER(wm_show(s, TRUE), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_EQ(ws.away, 1);
	KT_ASSERT_EQ(wm_self_check(), 0);

	/* its window closed while it is out of sight: its own window, shown */
	KT_ASSERT_ER(wm_close(a), E_OK);
	KT_ASSERT_ER(wm_ref(s, &ws), E_OK);
	KT_ASSERT_EQ(ws.parent, 0);
	KT_ASSERT_EQ(ws.away, 0);
	KT_ASSERT_EQ(ws.visible, 1);
	KT_ASSERT(listed(s));
	KT_ASSERT_EQ(wm_self_check(), 0);

	KT_ASSERT_ER(wm_close(s), E_OK);
	KT_ASSERT_ER(wm_close(other), E_OK);
	(void)wm_focus(0);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

/* a window wholly behind another is not laid down */
LOCAL void test_skip( void )
{
	T_WMSTAT	before, after;
	T_DPRECT	o;
	INT		small, big;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 700, 100, 750, 150);
	small = wm_open(&o, 0, "small");
	KT_ASSERT(small >= 1);

	rect(&o, 650, 50, 850, 250);		/* covering it entirely */
	big = wm_open(&o, 0, "big");
	KT_ASSERT(big >= 1);

	KT_ASSERT_ER(wm_stat(&before), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_stat(&after), E_OK);
	KT_ASSERT(after.windows_skipped > before.windows_skipped);

	/* moved out from under it, it is laid down again */
	rect(&o, 100, 700, 150, 750);
	KT_ASSERT_ER(wm_move(small, &o), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_stat(&before), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_stat(&after), E_OK);
	KT_ASSERT_EQ(after.windows_skipped, before.windows_skipped);

	KT_ASSERT_ER(wm_close(big), E_OK);
	KT_ASSERT_ER(wm_close(small), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

/* what needs drawing again is gathered and handed over once */
LOCAL void test_damage( void )
{
	T_DPRECT	o, r, got;
	INT		wid;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 50, 50, 250, 200);
	wid = wm_open(&o, 0, "damaged");
	KT_ASSERT(wid >= 1);

	/* nothing outstanding to begin with */
	KT_ASSERT_ER(wm_take_damage(wid, &got), E_NOEXS);

	rect(&r, 10, 10, 20, 20);
	KT_ASSERT_ER(wm_damage(wid, &r), E_OK);
	rect(&r, 100, 5, 110, 15);
	KT_ASSERT_ER(wm_damage(wid, &r), E_OK);

	/* the two come back as one rectangle covering both */
	KT_ASSERT_ER(wm_take_damage(wid, &got), E_OK);
	KT_ASSERT_EQ(got.left, 10);
	KT_ASSERT_EQ(got.top, 5);
	KT_ASSERT_EQ(got.right, 110);
	KT_ASSERT_EQ(got.bottom, 20);

	/* and taking it twice finds nothing the second time */
	KT_ASSERT_ER(wm_take_damage(wid, &got), E_NOEXS);

	/* moving a window makes the whole of it need drawing again */
	rect(&o, 60, 60, 260, 210);
	KT_ASSERT_ER(wm_move(wid, &o), E_OK);
	KT_ASSERT_ER(wm_take_damage(wid, &got), E_OK);
	KT_ASSERT_EQ(got.left, 0);
	KT_ASSERT_EQ(got.top, 0);
	KT_ASSERT_EQ(got.right, 200);

	KT_ASSERT_ER(wm_damage(wid, NULL), E_PAR);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
}

/*
 * Many windows opened, shuffled and closed. What this is really testing
 * is that the order stays a permutation through all of it, which is
 * what the self check says.
 */
LOCAL void test_many( void )
{
	T_DPRECT	o;
	INT		wid[8];
	INT		i;

	if ( !have_wm ) KT_SKIP("no windows");

	for ( i = 0; i < 8; i++ ) {
		rect(&o, 10 + i * 20, 10 + i * 10, 110 + i * 20, 90 + i * 10);
		wid[i] = wm_open(&o, WM_ATTR_FRAME, "many");
		KT_ASSERT(wid[i] >= 1);
		KT_ASSERT_EQ(wm_self_check(), 0);
	}
	for ( i = 0; i < 8; i++ ) {
		KT_ASSERT_ER(wm_raise(wid[i]), E_OK);
		KT_ASSERT_EQ(wm_self_check(), 0);
	}
	for ( i = 7; i >= 0; i-- ) {
		KT_ASSERT_ER(wm_lower(wid[i]), E_OK);
		KT_ASSERT_EQ(wm_self_check(), 0);
	}
	/* closed from the middle outwards, which is what leaves gaps */
	KT_ASSERT_ER(wm_close(wid[3]), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid[0]), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid[7]), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	for ( i = 1; i < 7; i++ ) {
		if ( i != 3 ) {
			KT_ASSERT_ER(wm_close(wid[i]), E_OK);
			KT_ASSERT_EQ(wm_self_check(), 0);
		}
	}
}

/*
 * More windows than the table first has room for, and more than the
 * 1024 there once was: each opens with a number of its own, the order
 * stays whole, and each closes.
 */
#define KT_WIN_MANY	1100

LOCAL void test_beyond( void )
{
	T_DPRECT	o;
	INT		*wid;
	INT		i, n = 0;

	if ( !have_wm ) KT_SKIP("no windows");
	wid = (INT *)Kmalloc(sizeof(INT) * KT_WIN_MANY);
	if ( wid == NULL ) KT_SKIP("no memory");
	for ( i = 0; i < KT_WIN_MANY; i++ ) {
		rect(&o, 10 + i % 200, 10 + i % 150, 26 + i % 200, 26 + i % 150);
		wid[i] = wm_open(&o, 0, "beyond");
		if ( wid[i] < 1 ) break;
		n++;
	}
	KT_ASSERT_EQ(n, KT_WIN_MANY);
	KT_ASSERT(knl_wm_nwin() >= KT_WIN_MANY);
	KT_ASSERT_EQ(wm_self_check(), 0);
	for ( i = 1; i < n; i++ ) {
		if ( wid[i] == wid[i - 1] ) break;
	}
	KT_ASSERT_EQ(i, n);
	KT_ASSERT_ER(wm_raise(wid[0]), E_OK);
	KT_ASSERT_EQ(wm_self_check(), 0);
	for ( i = n - 1; i >= 0; i-- ) {
		if ( wm_close(wid[i]) < E_OK ) break;
	}
	KT_ASSERT_EQ(i, -1);
	KT_ASSERT_EQ(wm_self_check(), 0);
	Kfree(wid);
}

/*
 * Input finds its way to a window. Keys go to whichever window has the
 * input; anything with a place goes to the window under that place.
 * Events are put in as if a device had reported them, because nothing
 * presses a key here.
 */
LOCAL void test_events( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	T_HIDEV		in;
	T_WMEV		ev;
	INT		a, b;
	T_HIDSTAT	hs;

	if ( !have_wm ) KT_SKIP("no windows");
	if ( ts_hid_stat(&hs) < E_OK ) {
		KT_SKIP("the machine reports nothing");
	}

	rect(&o, 100, 100, 300, 200);
	a = wm_open(&o, 0, "a");
	KT_ASSERT(a >= 1);
	rect(&o, 400, 100, 600, 200);
	b = wm_open(&o, 0, "b");
	KT_ASSERT(b >= 1);

	/* a place belongs to the window over it, and to nothing elsewhere */
	KT_ASSERT_EQ(wm_at(150, 150), a);
	KT_ASSERT_EQ(wm_at(450, 150), b);
	KT_ASSERT_EQ(wm_at(700, 700), 0);
	KT_ASSERT_EQ(wm_at(-5, 150), 0);

	/* the one nearest the viewer wins where two overlap */
	rect(&o, 250, 120, 450, 180);
	{
		INT	over = wm_open(&o, 0, "over");

		KT_ASSERT(over >= 1);
		KT_ASSERT_EQ(wm_at(280, 150), over);
		KT_ASSERT_ER(wm_lower(over), E_OK);
		KT_ASSERT_EQ(wm_at(280, 150), a);
		KT_ASSERT_ER(wm_close(over), E_OK);
	}

	/* a button goes to the window under the pointer */
	knl_memset(&in, 0, sizeof(in));
	in.type = HID_EV_BTN_DOWN;
	in.code = 0;
	in.x = 150;  in.y = 150;
	KT_ASSERT_ER(kt_inject(&in), E_OK);

	KT_ASSERT_ER(wm_read_event(&ev, 500), E_OK);
	KT_ASSERT_EQ(ev.type, HID_EV_BTN_DOWN);
	KT_ASSERT_EQ(ev.wid, a);

	/* and its place is measured from that window's work area */
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT_EQ(ev.x, 150 - w.work.left);
	KT_ASSERT_EQ(ev.y, 150 - w.work.top);

	/* pressing does not bring it forward: that is asked for */
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT(w.z != 0);

	/* a key goes to whichever window has the input */
	KT_ASSERT_ER(wm_focus(b), E_OK);
	KT_ASSERT_EQ(wm_focused(), b);

	knl_memset(&in, 0, sizeof(in));
	in.type = HID_EV_KEY_DOWN;
	in.code = 0x04;			/* the first letter key */
	in.mods = HID_MOD_LSHIFT;
	KT_ASSERT_ER(kt_inject(&in), E_OK);

	KT_ASSERT_ER(wm_read_event(&ev, 500), E_OK);
	KT_ASSERT_EQ(ev.type, HID_EV_KEY_DOWN);
	KT_ASSERT_EQ(ev.code, 0x04);
	KT_ASSERT_EQ(ev.mods, HID_MOD_LSHIFT);
	KT_ASSERT_EQ(ev.wid, b);

	/* a place no window covers is addressed to none */
	knl_memset(&in, 0, sizeof(in));
	in.type = HID_EV_BTN_DOWN;
	in.x = 900;  in.y = 700;
	KT_ASSERT_ER(kt_inject(&in), E_OK);
	KT_ASSERT_ER(wm_read_event(&ev, 500), E_OK);
	KT_ASSERT_EQ(ev.wid, 0);

	/* a window that closes gives the input up */
	KT_ASSERT_ER(wm_close(b), E_OK);
	KT_ASSERT_EQ(wm_focused(), 0);

	/* and nothing waiting means nothing, rather than waiting for ever */
	{
		ER	er = wm_read_event(&ev, 50);

		KT_ASSERT(er == E_TMOUT || er == E_OK);
	}

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(a), E_OK);
}

/* A volume key pressed and let go, and what reached the windows read: none of it */
LOCAL void vol_key( UINT code )
{
	T_HIDEV	in;
	T_WMEV	ev;

	knl_memset(&in, 0, sizeof(in));
	in.type = HID_EV_KEY_DOWN;
	in.code = code;
	KT_ASSERT_ER(kt_inject(&in), E_OK);
	in.type = HID_EV_KEY_UP;
	KT_ASSERT_ER(kt_inject(&in), E_OK);
	while ( wm_read_event(&ev, 100) >= E_OK ) {
		KT_ASSERT(!( ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP )
			  && ( ev.code == HID_KEY_VOLUP || ev.code == HID_KEY_VOLDOWN ) ));
	}
}

LOCAL void vol_set( UINT l, UINT r, UINT mute )
{
	SDMixSet	s;

	knl_memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = l;
	s.rv = r;
	s.mute = mute;
	KT_ASSERT_ER(knl_snd_mix_set(&s, FALSE), E_OK);
}

LOCAL void vol_is( UINT v )
{
	SDMix	m;

	knl_memset(&m, 0, sizeof(m));
	(void)knl_snd_mix_get(&m, 0);
	KT_ASSERT_EQ(m.lv, v);
	KT_ASSERT_EQ(m.rv, v);
	KT_ASSERT_EQ(m.mute, 0);
}

/*
 * The volume keys step the mixer's whole by 5, from the louder side,
 * both sides alike and within 0 .. 100, let a mute go, reach no window,
 * and say the volume on the message line
 */
LOCAL void test_volume_keys( void )
{
	SDMix	was;
	char	t[64];

	if ( !have_wm ) KT_SKIP("no windows");
	knl_memset(&was, 0, sizeof(was));
	(void)knl_snd_mix_get(&was, 0);

	vol_set(40, 50, 1);
	vol_key(HID_KEY_VOLUP);
	vol_is(55);
	KT_ASSERT(wm_msg_text(t, sizeof(t)) > 0);
	tm_printf((UB *)"wm: volume keys: the message line says \"%s\"\n", t);
	KT_ASSERT(knl_strcmp(t, "音量 55%") == 0);

	vol_key(HID_KEY_VOLDOWN);
	vol_key(HID_KEY_VOLDOWN);
	vol_is(45);
	(void)wm_msg_text(t, sizeof(t));
	KT_ASSERT(knl_strcmp(t, "音量 45%") == 0);

	vol_set(98, 97, 0);
	vol_key(HID_KEY_VOLUP);
	vol_is(100);
	vol_key(HID_KEY_VOLUP);
	vol_is(100);
	(void)wm_msg_text(t, sizeof(t));
	KT_ASSERT(knl_strcmp(t, "音量 100%") == 0);

	vol_set(3, 0, 0);
	vol_key(HID_KEY_VOLDOWN);
	vol_is(0);
	(void)wm_msg_text(t, sizeof(t));
	KT_ASSERT(knl_strcmp(t, "音量 0%") == 0);

	/* the note goes after two seconds, the program's message back */
	KT_ASSERT_ER(wm_msg_put("kt"), E_OK);
	(void)wm_msg_text(t, sizeof(t));
	KT_ASSERT(knl_strcmp(t, "音量 0%") == 0);
	{
		T_WMEV	ev;
		INT	i;

		for ( i = 0; i < 30; i++ ) (void)wm_read_event(&ev, 100);
	}
	(void)wm_msg_text(t, sizeof(t));
	KT_ASSERT(knl_strcmp(t, "kt") == 0);
	KT_ASSERT_ER(wm_msg_put(NULL), E_OK);

	vol_set(was.lv, was.rv, was.mute);
}

/* the frame is drawn by the manager, from the numbered look table */
LOCAL void test_frame( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid;
	UW		edge, bar, work;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 400, 300, 600, 420);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "waku");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	edge = wm_look(WM_LOOK_FRAME);
	bar  = wm_look(WM_LOOK_TITLE_ON);
	work = wm_look(WM_LOOK_WORK);

	/* the border, on each side */
	KT_ASSERT_EQ((INT)at(o.left, o.top + 40), (INT)edge);
	KT_ASSERT_EQ((INT)at(o.right - 1, o.top + 40), (INT)edge);
	KT_ASSERT_EQ((INT)at(o.left + 40, o.bottom - 1), (INT)edge);

	/*
	 * The strip between the border and the work area is built in
	 * layers: the window's outline, a lit line, the band, and the
	 * shaded line that meets the work area.
	 */
	KT_ASSERT_EQ((INT)at(o.left + 1, o.top + 1), (INT)wm_look(WM_LOOK_LIGHT));
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)bar);
	KT_ASSERT_EQ((INT)at(w.work.left + 2, w.work.top - 2),
		     (INT)wm_look(WM_LOOK_SHADOW));
	KT_ASSERT_EQ((INT)at(w.work.left - 1, w.work.top - 1), (INT)edge);

	/* and the work area starts as the look says, not as black */
	KT_ASSERT_EQ((INT)at(w.work.left + 2, w.work.top + 2), (INT)work);

	/* a window that does not have the input wears the other bar */
	KT_ASSERT_ER(wm_focus(0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6),
		     (INT)wm_look(WM_LOOK_TITLE_OFF));

	/* changing a number changes the design and nothing else */
	KT_ASSERT_ER(wm_set_look(WM_LOOK_FRAME, 0x00123456U), E_OK);
	KT_ASSERT_ER(wm_move(wid, &o), E_OK);		/* draws the frame again */
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(o.left, o.top + 40), 0x00123456);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_FRAME, edge), E_OK);

	/* a number nothing was ever put in answers nought */
	KT_ASSERT_EQ((INT)wm_look(7777), 0);

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
}

/*
 * A window's own scroll bars: they take room from the work area, they
 * are drawn in the strip that room came from, they say what was
 * pressed, and dragging the knob winds the bar by as much as the knob
 * moved and no more.
 */
LOCAL void test_bars( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	T_WMBAR		b, q;
	INT		wid, x, ytop, ybot, ymid;
	UINT		which = 99;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 300, 200, 620, 500);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			  | WM_ATTR_RBAR | WM_ATTR_BBAR, "bar");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);

	/*
	 * A side with a bar is the bar and the two lines beside it. The
	 * side without one is the four lines and the band between them.
	 */
	KT_ASSERT_EQ(o.right - w.work.right, (INT)wm_look(WM_LOOK_BAR_W) + 2);
	KT_ASSERT_EQ(o.bottom - w.work.bottom, (INT)wm_look(WM_LOOK_BAR_W) + 2);
	KT_ASSERT_EQ(w.work.left - o.left, 10);

	/* a tenth of the whole is shown, from its start */
	b.lo = 0;  b.hi = 1000;  b.clo = 0;  b.chi = 100;
	KT_ASSERT_ER(wm_set_bar(wid, WM_BAR_R, &b), E_OK);
	KT_ASSERT_ER(wm_bar(wid, WM_BAR_R, &q), E_OK);
	KT_ASSERT_EQ(q.hi, 1000);
	KT_ASSERT_ER(wm_set_bar(wid, 9, &b), E_PAR);

	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/* the middle of the track, and two places along it */
	x    = (w.work.right + 2 + o.right - 2) / 2;
	ytop = w.work.top + 4;
	ybot = w.work.bottom - 4;
	ymid = (w.work.top + w.work.bottom) / 2;

	/* the knob is at the near end, the track behind it at the far end */
	KT_ASSERT_EQ((INT)at(x, ytop), (INT)wm_look(WM_LOOK_BAR_KNOB));
	KT_ASSERT_EQ((INT)at(x, ybot), (INT)wm_look(WM_LOOK_BAR_BACK));

	/* what a press falls on */
	KT_ASSERT_EQ(wm_bar_at(wid, x, ytop, &which), WM_BARHIT_KNOB);
	KT_ASSERT_EQ((INT)which, WM_BAR_R);
	KT_ASSERT_EQ(wm_bar_at(wid, x, ybot, &which), WM_BARHIT_AFTER);
	KT_ASSERT_EQ(wm_bar_at(wid, w.work.left + 4, w.work.top + 4, &which),
		     WM_BARHIT_NONE);

	/* dragging the knob to the middle shows the middle */
	KT_ASSERT_ER(wm_bar_drag(wid, WM_BAR_R, x, ymid), E_OK);
	KT_ASSERT_ER(wm_bar(wid, WM_BAR_R, &q), E_OK);
	KT_ASSERT_EQ(q.chi - q.clo, 100);		/* as much is shown as before */
	KT_ASSERT(q.clo > 300 && q.clo < 600);

	/* and past the end it stops at the end */
	KT_ASSERT_ER(wm_bar_drag(wid, WM_BAR_R, x, o.bottom + 50), E_OK);
	KT_ASSERT_ER(wm_bar(wid, WM_BAR_R, &q), E_OK);
	KT_ASSERT_EQ(q.clo, 900);
	KT_ASSERT_EQ(q.chi, 1000);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, ybot), (INT)wm_look(WM_LOOK_BAR_KNOB));

	/* a bar showing the whole of what there is has nothing to wind */
	b.clo = 0;  b.chi = 1000;
	KT_ASSERT_ER(wm_set_bar(wid, WM_BAR_R, &b), E_OK);
	KT_ASSERT_ER(wm_bar_drag(wid, WM_BAR_R, x, ytop), E_OK);
	KT_ASSERT_ER(wm_bar(wid, WM_BAR_R, &q), E_OK);
	KT_ASSERT_EQ(q.clo, 0);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, ymid + 10), (INT)wm_look(WM_LOOK_BAR_KNOB));

	/* the mark lies across the middle of the knob */
	KT_ASSERT_EQ((INT)at(x, ymid), (INT)wm_look(WM_LOOK_BAR_MARK));

	/* a window without the input wears the quiet colour */
	KT_ASSERT_ER(wm_focus(0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, ymid + 10), (INT)wm_look(WM_LOOK_BAR_OFF));

	/*
	 * And back to a window that has the input, showing the middle of a
	 * long document on both bars: the knob in the middle of the track,
	 * the track itself above and below it.
	 */
	b.lo = 0;  b.hi = 1000;  b.clo = 400;  b.chi = 600;
	KT_ASSERT_ER(wm_set_bar(wid, WM_BAR_R, &b), E_OK);
	b.clo = 300;  b.chi = 500;
	KT_ASSERT_ER(wm_set_bar(wid, WM_BAR_B, &b), E_OK);
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, ymid), (INT)wm_look(WM_LOOK_BAR_MARK));
	KT_ASSERT_EQ((INT)at(x, ytop), (INT)wm_look(WM_LOOK_BAR_BACK));
	KT_ASSERT_EQ((INT)at(x, ybot), (INT)wm_look(WM_LOOK_BAR_BACK));

	KT_ASSERT_EQ(wm_self_check(), 0);
	wm_close(wid);
}

/*
 * What part of a window a place falls on, and the least a window may be
 * made. Nothing here moves a window: the answer is what was pressed,
 * and what that means belongs to whoever owns the window.
 */
LOCAL void test_grab( void )
{
	T_DPRECT	o, small;
	T_WMWIN		w;
	INT		wid, wide, high, got = 0;
	UINT		bar = 99;

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 200, 120, 520, 400);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			  | WM_ATTR_RBAR | WM_ATTR_BBAR, "tsukamu");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_ER(wm_raise(wid), E_OK);

	/* the work area, the band with the name, the bars, the corner */
	KT_ASSERT_EQ(wm_part_at(w.work.left + 5, w.work.top + 5, &got, &bar),
		     WM_PART_WORK);
	KT_ASSERT_EQ(got, wid);
	KT_ASSERT_EQ(wm_part_at(w.work.left + 60, o.top + 6, &got, &bar),
		     WM_PART_TITLE);
	/* the pictogram at the head of the band: pressing it closes */
	KT_ASSERT_EQ(wm_part_at(w.work.left + 5, o.top + 6, &got, &bar),
		     WM_PART_PICT);
	KT_ASSERT_EQ(wm_part_at(w.work.right + 6, w.work.top + 20, &got, &bar),
		     WM_PART_BAR);
	KT_ASSERT_EQ((INT)bar, WM_BAR_R);
	KT_ASSERT_EQ(wm_part_at(w.work.left + 20, w.work.bottom + 6, &got, &bar),
		     WM_PART_BAR);
	KT_ASSERT_EQ((INT)bar, WM_BAR_B);
	KT_ASSERT_EQ(wm_part_at(o.right - 2, o.bottom - 2, &got, &bar),
		     WM_PART_GRIP_BR);

	/* and bare screen is not any part of a window */
	KT_ASSERT_EQ(wm_part_at(o.left - 20, o.top - 20, &got, &bar),
		     WM_PART_NONE);
	KT_ASSERT_EQ(got, 0);

	/* the least it may be made is its frame plus a little work area */
	KT_ASSERT_ER(wm_least(w.attr, &wide, &high), E_OK);
	KT_ASSERT(wide > 40 && high > 40);
	KT_ASSERT_ER(wm_least(w.attr, NULL, &high), E_PAR);

	rect(&small, 200, 120, 200 + wide - 1, 120 + high - 1);
	KT_ASSERT_ER(wm_move(wid, &small), E_PAR);	/* smaller than its frame */
	rect(&small, 200, 120, 200 + wide, 120 + high);
	KT_ASSERT_ER(wm_move(wid, &small), E_OK);	/* exactly the least */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_EQ(w.work.right - w.work.left, 16);
	KT_ASSERT_EQ(w.work.bottom - w.work.top, 16);
	KT_ASSERT_EQ(wm_self_check(), 0);

	wm_close(wid);
}

/*
 * A window's name is drawn in the band, and the name may be in
 * Japanese: it is UTF-8 like every other string in the system, and what
 * draws it steps by characters rather than by bytes.
 */
LOCAL void test_title_jp( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid2, i, j, ink = 0;

	if ( !have_wm ) KT_SKIP("no windows");
	if ( fn_system() <= 0 ) KT_SKIP("no system font");

	rect(&o, 240, 40, 700, 220);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "TessronOS\u03b1\u7248");
	KT_ASSERT(wid2 > 0);
	if ( wid2 <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid2), E_OK);
	KT_ASSERT_ER(wm_ref(wid2, &w), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/* something was put down in the band, in the name's own colour */
	for ( j = o.top + 4; j < w.work.top - 2; j++ ) {
		for ( i = o.left + 4; i < o.right - 8; i++ ) {
			if ( at(i, j) == wm_look(WM_LOOK_TITLE_TEXT) ) {
				ink++;
			}
		}
	}
	KT_ASSERT(ink > 0);

	wm_close(wid2);
}

/*
 * A scheme is every colour at once. Changing one changes what the table
 * answers, draws every frame again, and gives every program the whole
 * of its work area to draw again -- which is the only way a program
 * that drew in the old colours hears about the new ones.
 */
LOCAL void test_scheme( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid2;
	UW		band_light, band_dark;
	T_DPRECT	dmg;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_EQ((INT)wm_scheme(), WM_SCHEME_LIGHT);
	KT_ASSERT(!wm_scheme_changed());
	band_light = wm_look(WM_LOOK_TITLE_ON);

	rect(&o, 120, 460, 460, 600);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "haishoku");
	KT_ASSERT(wid2 > 0);
	if ( wid2 <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid2), E_OK);
	KT_ASSERT_ER(wm_ref(wid2, &w), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)band_light);

	/* the whole of it is outstanding after a change, not a corner */
	while ( wm_take_damage(wid2, &dmg) == E_OK ) {
		/* take whatever was outstanding from opening it */
	}
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_DARK), E_OK);
	KT_ASSERT_EQ((INT)wm_scheme(), WM_SCHEME_DARK);
	band_dark = wm_look(WM_LOOK_TITLE_ON);
	KT_ASSERT(band_dark != band_light);
	KT_ASSERT_ER(wm_take_damage(wid2, &dmg), E_OK);
	KT_ASSERT_EQ(dmg.left, 0);
	KT_ASSERT_EQ(dmg.top, 0);
	KT_ASSERT_EQ(dmg.right, w.work.right - w.work.left);

	/* and the frame was drawn again without anyone asking */
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)band_dark);

	/* every scheme has a name, and numbers past the last have none */
	KT_ASSERT(wm_scheme_name(WM_SCHEME_CONTRAST) != NULL);
	KT_ASSERT(wm_scheme_name(WM_SCHEME_MAX) == NULL);
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_MAX), E_PAR);

	/* setting one number by hand says so */
	KT_ASSERT(!wm_scheme_changed());
	KT_ASSERT_ER(wm_set_look(WM_LOOK_TITLE_ON, 0x00123456U), E_OK);
	KT_ASSERT(wm_scheme_changed());
	KT_ASSERT_EQ((INT)wm_look(WM_LOOK_TITLE_ON), 0x00123456);

	wm_close(wid2);
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
}

/*
 * What is behind a window shows through its frame, and never through
 * its work area: letters have to be readable, and mixing every pixel of
 * every window would cost the whole screen instead of the strip.
 */
LOCAL void test_seethrough( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid2;
	UW		band, ground = 0x00204060U, frame_px, work_px;
	UINT		op = 0;

	if ( !have_wm ) KT_SKIP("no windows");

	/* a known desk behind it */
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, ground), E_OK);
	rect(&o, 500, 460, 800, 620);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "sukeru");
	KT_ASSERT(wid2 > 0);
	if ( wid2 <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid2), E_OK);
	KT_ASSERT_ER(wm_ref(wid2, &w), E_OK);

	KT_ASSERT_ER(wm_opacity(wid2, &op), E_OK);
	KT_ASSERT_EQ((INT)op, 255);		/* a window starts solid */

	band = wm_look(WM_LOOK_TITLE_ON);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)band);

	/* half way through: the band moves toward the ground behind it */
	KT_ASSERT_ER(wm_set_opacity(wid2, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	frame_px = at(w.work.right - 12, w.work.top - 6);
	work_px  = at(w.work.left + 2, w.work.top + 2);
	KT_ASSERT(frame_px != band);
	KT_ASSERT(frame_px != ground);
	KT_ASSERT_EQ((INT)work_px, (INT)wm_look(WM_LOOK_WORK));  /* never through */

	/* a tint lays a colour over the frame before it is mixed */
	KT_ASSERT_ER(wm_set_opacity(wid2, 255), E_OK);
	KT_ASSERT_ER(wm_set_tint(wid2, 0x00FF0000U, 255), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), 0x00FF0000);
	KT_ASSERT_EQ((INT)at(w.work.left + 2, w.work.top + 2),
		     (INT)wm_look(WM_LOOK_WORK));

	/* and the desk shows where no window stands */
	KT_ASSERT_EQ((INT)at(o.left - 8, o.top + 8), (INT)ground);

	KT_ASSERT_ER(wm_set_tint(wid2, 0, 0), E_OK);
	KT_ASSERT_ER(wm_set_opacity(wid2, 300), E_PAR);

	/* left as a window you can see the desk through */
	KT_ASSERT_ER(wm_set_opacity(wid2, 160), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT(at(w.work.right - 12, w.work.top - 6) != band);
	KT_ASSERT_EQ((INT)at(w.work.left + 2, w.work.top + 2),
		     (INT)wm_look(WM_LOOK_WORK));

	KT_ASSERT_EQ(wm_self_check(), 0);
	wm_close(wid2);
}

/*
 * The settings that change the shape of a frame rather than its ink: a
 * round corner, and a shadow. Both are on every window at once, because
 * they are how the system looks rather than how one window looks.
 */
LOCAL void test_look_settings( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid2;
	UW		desk;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, 0x00304050U), E_OK);
	desk = wm_look(WM_LOOK_DESK);

	KT_ASSERT_EQ(wm_get(WM_SET_ROUND), 0);
	KT_ASSERT_EQ(wm_get(7777), -1);
	KT_ASSERT_ER(wm_set(WM_SET_ROUND, -1), E_PAR);
	KT_ASSERT_ER(wm_set(WM_SET_SHADOW, 300), E_PAR);

	rect(&o, 520, 80, 800, 240);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "katachi");
	KT_ASSERT(wid2 > 0);
	if ( wid2 <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid2), E_OK);
	KT_ASSERT_ER(wm_ref(wid2, &w), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/* square: the very corner belongs to the window */
	KT_ASSERT_EQ((INT)at(o.left, o.top), (INT)wm_look(WM_LOOK_FRAME));

	/* round: the corner is the desk, the middle of the edge is not */
	KT_ASSERT_ER(wm_set(WM_SET_ROUND, 16), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(o.left, o.top), (INT)desk);
	KT_ASSERT_EQ((INT)at(o.left + 100, o.top), (INT)wm_look(WM_LOOK_FRAME));
	KT_ASSERT_EQ((INT)at(o.right - 1, o.bottom - 1), (INT)desk);

	/* a shadow darkens the desk past the window's foot */
	KT_ASSERT_ER(wm_set(WM_SET_ROUND, 0), E_OK);
	KT_ASSERT_ER(wm_set(WM_SET_SHADOW, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	{
		UW	under = at(o.left + 100, o.bottom + 2);

		KT_ASSERT(under != desk);
		KT_ASSERT(under != 0);			/* darkened, not black */
	}
	/* and where no window stands it is the plain desk */
	KT_ASSERT_EQ((INT)at(o.left - 40, o.top + 40), (INT)desk);

	/* left round and with a shadow, which is how it is meant to look */
	KT_ASSERT_ER(wm_set(WM_SET_ROUND, 14), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	KT_ASSERT_EQ(wm_self_check(), 0);
	wm_close(wid2);
	KT_ASSERT_ER(wm_set(WM_SET_SHADOW, 0), E_OK);
	KT_ASSERT_ER(wm_set(WM_SET_ROUND, 0), E_OK);
}

/* a numbered entry may be a tone of two colours rather than one colour */
LOCAL void test_look_tone( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	T_DPPAT		pat;
	INT		wid2, x, fore = 0, back = 0;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_look_pat(WM_LOOK_TITLE_ON, &pat), E_OK);
	KT_ASSERT_EQ((INT)pat.kind, DP_PAT_TONE);
	KT_ASSERT_EQ(pat.tone, 7);		/* a colour is the solid tone */
	KT_ASSERT_ER(wm_look_pat(7777, &pat), E_NOEXS);

	KT_ASSERT_ER(wm_set_look_tone(WM_LOOK_TITLE_ON, 4,
				      0x00FFFFFFU, 0x00000000U), E_OK);
	KT_ASSERT_ER(wm_look_pat(WM_LOOK_TITLE_ON, &pat), E_OK);
	KT_ASSERT_EQ(pat.tone, 4);
	KT_ASSERT(wm_scheme_changed());

	/* no title: the letters, in whatever face the system has, are not the band */
	rect(&o, 520, 260, 800, 400);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "");
	KT_ASSERT(wid2 > 0);
	if ( wid2 <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid2), E_OK);
	KT_ASSERT_ER(wm_ref(wid2, &w), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/* half the band is one colour and half the other */
	for ( x = w.work.left + 40; x < w.work.left + 80; x++ ) {
		if ( at(x, w.work.top - 6) == 0x00FFFFFF ) fore++;
		if ( at(x, w.work.top - 6) == 0x00000000 ) back++;
	}
	KT_ASSERT_EQ(fore, 20);
	KT_ASSERT_EQ(back, 20);

	wm_close(wid2);
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_look_pat(WM_LOOK_TITLE_ON, &pat), E_OK);
	KT_ASSERT_EQ(pat.tone, 7);		/* a scheme puts the tones back */
}

/*
 * The wall: a picture the windows stand on. Nothing is copied, so the
 * pixels have to outlive the wall being set; what is laid where is the
 * only thing tested here.
 */
LOCAL UW	wall_pix[4];

LOCAL void test_wall( void )
{
	T_DPRECT	o;
	INT		wid2;

	if ( !have_wm ) KT_SKIP("no windows");

	/* four pixels: red, green, blue, white */
	wall_pix[0] = 0x00FF0000U;  wall_pix[1] = 0x0000FF00U;
	wall_pix[2] = 0x000000FFU;  wall_pix[3] = 0x00FFFFFFU;

	KT_ASSERT_ER(wm_set_wall(wall_pix, 2, 2, WM_WALL_TILE), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(0, 0), 0x00FF0000);
	KT_ASSERT_EQ((INT)at(1, 0), 0x0000FF00);
	KT_ASSERT_EQ((INT)at(0, 1), 0x000000FF);
	KT_ASSERT_EQ((INT)at(1, 1), 0x00FFFFFF);
	KT_ASSERT_EQ((INT)at(2, 0), 0x00FF0000);	/* laid over and over */
	KT_ASSERT_EQ((INT)at(3, 1), 0x00FFFFFF);

	/*
	 * Stretched, each of its four pixels covers a quarter of the
	 * screen. Where the second one begins depends on how wide the
	 * screen is, so the place to look is worked out from that rather
	 * than written down.
	 */
	KT_ASSERT_ER(wm_set_wall(wall_pix, 2, 2, WM_WALL_FIT), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	{
		T_DISPSPEC	sp;

		KT_ASSERT_ER(ts_disp_ref(&sp), E_OK);
		KT_ASSERT_EQ((INT)at(4, 4), 0x00FF0000);
		KT_ASSERT_EQ((INT)at((INT)sp.width / 2 + 4, 4), 0x0000FF00);
	}

	/* in the middle, the ground shows round it */
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, 0x00123456U), E_OK);
	KT_ASSERT_ER(wm_set_wall(wall_pix, 2, 2, WM_WALL_CENTRE), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(4, 4), 0x00123456);

	/* a window still stands on it */
	rect(&o, 100, 100, 300, 200);
	wid2 = wm_open(&o, WM_ATTR_FRAME, "kabe");
	KT_ASSERT(wid2 > 0);
	if ( wid2 > 0 ) {
		KT_ASSERT_ER(wm_composite(), E_OK);
		KT_ASSERT_EQ((INT)at(150, 150), (INT)wm_look(WM_LOOK_WORK));
		wm_close(wid2);
	}

	KT_ASSERT_ER(wm_set_wall(NULL, 0, 0, WM_WALL_TILE), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(4, 4), 0x00123456);
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
}

/*
 * The look table is found by number, and the numbers are the ones the
 * definitions being brought over use. A name in this system's own code
 * is the same entry as the number a definition writes.
 */
LOCAL void test_look_numbers( void )
{
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);

	KT_ASSERT_EQ(WM_LOOK_LIGHT, 451);
	KT_ASSERT_EQ(WM_LOOK_SHADOW, 452);
	KT_ASSERT_EQ(WM_LOOK_TITLE_ON, 454);
	KT_ASSERT_EQ(WM_LOOK_BAR_KNOB, 472);
	KT_ASSERT_EQ(WM_LOOK_BAR_BACK, 473);
	KT_ASSERT_EQ(WM_LOOK_BAR_MARK, 474);
	KT_ASSERT_EQ(WM_LOOK_PART_TEXT, 563);
	KT_ASSERT_EQ(WM_SET_ROUND, 9104);

	/* writing the number and reading the name is the same entry */
	KT_ASSERT_ER(wm_set_look(472, 0x00ABCDEFU), E_OK);
	KT_ASSERT_EQ((INT)wm_look(WM_LOOK_BAR_KNOB), 0x00ABCDEF);
	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT(wm_look(WM_LOOK_BAR_KNOB) != 0x00ABCDEFU);

	/* lengths and settings live in the same table */
	KT_ASSERT_EQ(wm_num(LK_TITLE_H, 0), 16);
	KT_ASSERT_EQ(wm_num(LK_BAR_W, 0), 16);
	KT_ASSERT_EQ(wm_num(7777, 42), 42);
}

/*
 * The name's height and the bars' width changed as ユーザ環境設定 changes
 * them: a window open already takes the new frame, its work area moves
 * in with it, and nothing kept disagrees with the one measurement.
 * Then the pointer at its large size: the square repaired where it was
 * is as large as the one it was laid in.
 */
LOCAL void test_relayout( void )
{
	T_DPRECT	o, r;
	T_WMWIN		w0, w1;
	INT		wid, lx = 0, ly = 0, side = 0;
	UW		serial = wm_layout_serial();

	if ( !have_wm ) KT_SKIP("no windows");

	rect(&o, 200, 150, 600, 450);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR | WM_ATTR_BBAR, "relayout");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w0), E_OK);

	KT_ASSERT_ER(knl_look_setting(LK_TITLE_H, 28), E_OK);
	KT_ASSERT_ER(knl_look_setting(LK_BAR_W, 24), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w1), E_OK);
	KT_ASSERT_EQ(w1.work.top - w0.work.top, 12);		/* the band 12 taller */
	KT_ASSERT_EQ(w0.work.right - w1.work.right, 8);		/* the bar 8 wider */
	KT_ASSERT_EQ(w0.work.bottom - w1.work.bottom, 8);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT(wm_layout_serial() != serial);

	KT_ASSERT_ER(knl_look_setting(LK_TITLE_H, 16), E_OK);
	KT_ASSERT_ER(knl_look_setting(LK_BAR_W, 16), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w1), E_OK);
	KT_ASSERT_EQ(w1.work.top, w0.work.top);
	KT_ASSERT_EQ(w1.work.right, w0.work.right);
	KT_ASSERT_EQ(wm_self_check(), 0);

	/*
	 * The pointer, large, as it is laid on the screen. It is shown for
	 * this alone (the tests keep it off): what wm_pointer_last tells is
	 * the last one laid, which, hidden, is whatever an earlier suite
	 * that showed it left there.
	 */
	KT_ASSERT_ER(knl_look_setting(LK_PTR_SIZE, 2), E_OK);
	wm_pointer_box(300, 300, &r);
	KT_ASSERT_EQ(r.right - r.left, WM_POINTER_SIDE * 3 / 2);
	wm_show_pointer(TRUE);
	KT_ASSERT_ER(wm_composite(), E_OK);
	if ( wm_pointer_last(&lx, &ly, &side) ) {
		KT_ASSERT_EQ(side, WM_POINTER_SIDE * 3 / 2);
	}
	KT_ASSERT_ER(knl_look_setting(LK_PTR_SIZE, 1), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	if ( wm_pointer_last(&lx, &ly, &side) ) {
		KT_ASSERT_EQ(side, WM_POINTER_SIDE);
	}
	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_composite(), E_OK);

	KT_ASSERT_ER(wm_close(wid), E_OK);
}

/* ---------------------------------------------------------------- the windows' look */

/*
 * The look of the windows measured on the screen, pixel by pixel, not
 * looked at: a light thing laid over a light thing looks the same mixed
 * or not, and only the numbers tell. Every expected value is worked out
 * the way the look says it is made -- the frame laid at so much of 255
 * over what is behind it, rounded -- and the measured one is printed
 * beside it.
 */

/* 'over' at a of 255 over 'under', rounded, channel by channel */
LOCAL UW lk_over( UW under, UW over, UINT a )
{
	UW	r, g, b;

	r = ( ( ( over >> 16 ) & 0xFF ) * a + ( ( under >> 16 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	g = ( ( ( over >> 8 ) & 0xFF ) * a + ( ( under >> 8 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	b = ( ( over & 0xFF ) * a + ( under & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	return ( r << 16 ) | ( g << 8 ) | b;
}

/* Every setting of the look back to what nothing says */
LOCAL void lk_reset( void )
{
	INT	i;

	for ( i = 0; i < cf_look_nitem; i++ ) {
		(void)wm_set(cf_look_items[i].look, cf_look_items[i].dflt);
	}
}

LOCAL void lk_show( CONST char *what, UW got, UW want )
{
	tm_printf((UB *)"  look: %s 0x%06x (worked out 0x%06x)\n", what, got, want);
}

/* How far apart two colours are, the largest of the three channels */
LOCAL INT lk_apart( UW a, UW b )
{
	INT	d = 0, k;

	for ( k = 0; k < 24; k += 8 ) {
		INT	x = (INT)( ( a >> k ) & 0xFF ) - (INT)( ( b >> k ) & 0xFF );

		if ( x < 0 ) x = -x;
		if ( x > d ) d = x;
	}
	return d;
}

/*
 * A frame seen through: the band mixes with the desk behind it, the
 * work area is laid as the program drew it; with the whole window seen
 * through, the work area mixes as well. A window behind the frame shows
 * through it, rather than being left out as covered.
 */
LOCAL void test_look_opacity( void )
{
	T_DPRECT	o, r;
	T_WMWIN		w;
	INT		wid, under;
	UW		desk = 0x00204060U, band, white, got, want;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_wall(NULL, 0, 0, WM_WALL_TILE), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, desk), E_OK);
	lk_reset();
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "toumei");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	band = wm_look(WM_LOOK_TITLE_ON);
	white = wm_look(WM_LOOK_WORK);

	/* solid as it opens: the band is the band */
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)band);

	/* the frame half through */
	KT_ASSERT_ER(wm_set(LK_OPACITY, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.right - 12, w.work.top - 6);
	want = lk_over(desk, band, 128);
	lk_show("band at 128", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	got = at(w.work.left + 20, w.work.top + 20);
	lk_show("work area, the frame alone seen through", got, white);
	KT_ASSERT_EQ((INT)got, (INT)white);

	/* the whole window */
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.left + 20, w.work.top + 20);
	want = lk_over(desk, white, 128);
	lk_show("work area, the whole window seen through", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 0), E_OK);

	/* a window behind the band shows through it, red under grey */
	rect(&r, w.work.right - 40, o.top - 30, w.work.right + 20, o.top + 10);
	under = wm_open(&r, 0, "shita");
	KT_ASSERT(under > 0);
	if ( under > 0 ) {
		T_DPRECT	all;

		rect(&all, 0, 0, r.right - r.left, r.bottom - r.top);
		KT_ASSERT_ER(dp_fill_rect(wm_gid(under), &all, 0x00FF0000U), E_OK);
		KT_ASSERT_ER(wm_raise(wid), E_OK);
		KT_ASSERT_ER(wm_composite(), E_OK);
		got = at(w.work.right - 12, o.top + 4);
		want = lk_over(0x00FF0000U, band, 128);
		lk_show("band over a red window", got, want);
		KT_ASSERT_EQ((INT)got, (INT)want);
		KT_ASSERT_ER(wm_close(under), E_OK);
	}

	/* the program's own on top of the look's: 128 of 128 */
	KT_ASSERT_ER(wm_set_opacity(wid, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.right - 12, w.work.top - 6);
	want = lk_over(desk, band, 128 * 128 / 255);
	lk_show("band at 128 of 128", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	KT_ASSERT_ER(wm_set_opacity(wid, 255), E_OK);

	/* the look turned off: solid again, whatever the opacity says */
	KT_ASSERT_ER(wm_set(LK_LOOK, 0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.right - 12, w.work.top - 6);
	lk_show("band, the look off", got, band);
	KT_ASSERT_EQ((INT)got, (INT)band);
	KT_ASSERT_ER(wm_set(LK_LOOK, 1), E_OK);
	KT_ASSERT_ER(wm_set(LK_OPACITY, 300), E_PAR);	/* out of its range */

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	lk_reset();
}

/*
 * The washes: the frame takes a colour and the title band its own, at
 * the strength set, when they do not keep the scheme's (LK_FROM). They
 * are laid as the window is laid, never painted into its pixels: taken
 * off, the frame is what it was without being drawn again.
 */
LOCAL void test_look_wash( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid, lx, ly;
	UW		band, left, got, want;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	lk_reset();
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "arau");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	lx = o.left + 2;			/* the band of the left strip */
	ly = o.top + 60;
	KT_ASSERT_ER(wm_composite(), E_OK);
	band = at(w.work.right - 12, w.work.top - 6);
	left = at(lx, ly);

	/* the scheme's colours are kept: nothing changes */
	KT_ASSERT_ER(wm_set(LK_WASH, 0x00FF0000), E_OK);
	KT_ASSERT_ER(wm_set(LK_WASH_STR, 255), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(lx, ly), (INT)left);

	/* the frame's own wash, all the way */
	KT_ASSERT_ER(wm_set(LK_FROM, 2), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(lx, ly);
	lk_show("left strip washed red at 255", got, 0x00FF0000U);
	KT_ASSERT_EQ((INT)got, 0x00FF0000);
	KT_ASSERT_EQ((INT)at(w.work.left + 20, w.work.top + 20), (INT)wm_look(WM_LOOK_WORK));

	/* half way, and the band with a blue of its own */
	KT_ASSERT_ER(wm_set(LK_WASH_STR, 100), E_OK);
	KT_ASSERT_ER(wm_set(LK_FROM, 0), E_OK);
	KT_ASSERT_ER(wm_set(LK_BAR_WASH, 0x000000FF), E_OK);
	KT_ASSERT_ER(wm_set(LK_BAR_STR, 160), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(lx, ly);
	want = lk_over(left, 0x00FF0000U, 100);
	lk_show("left strip washed red at 100", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	got = at(w.work.right - 12, w.work.top - 6);
	want = lk_over(band, 0x000000FFU, 160);
	lk_show("band washed blue at 160", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);

	/* and seen through, the wash before the mixing */
	KT_ASSERT_ER(wm_set(LK_OPACITY, 200), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.right - 12, w.work.top - 6);
	want = lk_over(wm_look(WM_LOOK_DESK), lk_over(band, 0x000000FFU, 160), 200);
	lk_show("band washed and seen through", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	KT_ASSERT_ER(wm_set(LK_OPACITY, 255), E_OK);

	/* taken off: the frame as it was, though it was not drawn again */
	KT_ASSERT_ER(wm_set(LK_WASH_STR, 0), E_OK);
	KT_ASSERT_ER(wm_set(LK_BAR_STR, 0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(lx, ly), (INT)left);
	KT_ASSERT_EQ((INT)at(w.work.right - 12, w.work.top - 6), (INT)band);

	KT_ASSERT_ER(wm_close(wid), E_OK);
	lk_reset();
}

/*
 * Round corners: the pixel at the very corner is the desk, the pixels
 * along the curve are part desk and part frame -- the edge is not a
 * staircase -- and a press finds the window where it shows.
 */
LOCAL void test_look_round( void )
{
	T_DPRECT	o;
	INT		wid, k, partial = -1;
	UW		desk = 0x00204060U, got = 0;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, desk), E_OK);
	lk_reset();
	KT_ASSERT_ER(wm_set(LK_ROUND, 12), E_OK);
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "kado");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_composite(), E_OK);

	KT_ASSERT_EQ((INT)at(o.left, o.top), (INT)desk);
	KT_ASSERT_EQ((INT)at(o.right - 1, o.bottom - 1), (INT)desk);
	/* along the top row toward the middle: desk, then part covered, then the outline */
	for ( k = 0; k < 12; k++ ) {
		UW	c = at(o.left + k, o.top);

		if ( c != desk && c != wm_look(WM_LOOK_FRAME) && partial < 0 ) {
			partial = k;
			got = c;
		}
	}
	tm_printf((UB *)"  look: the top row of the corner from %d in: 0x%06x (desk 0x%06x, outline 0x%06x)\n",
		  partial, got, desk, wm_look(WM_LOOK_FRAME));
	KT_ASSERT(partial > 0);
	KT_ASSERT(( got & 0xFF ) > 0 && ( got & 0xFF ) < ( desk & 0xFF ));	/* between the two */
	KT_ASSERT_EQ((INT)at(o.left + 100, o.top), (INT)wm_look(WM_LOOK_FRAME));

	/*
	 * The frame's lines go round the corner: where the light line
	 * inside the outline meets the curve, a pixel of the band near the
	 * corner holds the lines now, unlike the band further along the same
	 * row; the same along the left side. Laid again, nothing changes.
	 */
	{
		UW	diag = at(o.left + 4, o.top + 4), band = at(o.left + 60, o.top + 4);
		UW	low = at(o.left + 4, o.bottom - 5), side = at(o.left + 4, o.bottom - 60);
		UW	light = wm_look(WM_LOOK_LIGHT) & 0x00FFFFFFU;

		tm_printf((UB *)"  look: the lines round the corner: 0x%06x (band 0x%06x, light 0x%06x); "
			  "below 0x%06x (frame 0x%06x)\n", diag, band, light, low, side);
		KT_ASSERT(diag != band);
		KT_ASSERT(low != side);
		KT_ASSERT_ER(wm_composite(), E_OK);
		KT_ASSERT_EQ((INT)at(o.left + 4, o.top + 4), (INT)diag);
		KT_ASSERT_EQ((INT)at(o.left + 4, o.bottom - 5), (INT)low);
		/* the straight edges as they were drawn */
		KT_ASSERT_EQ((INT)at(o.left + 100, o.top + 1), (INT)at(o.left + 150, o.top + 1));
	}

	/* a press at the corner is on the desk; inside the curve, on the window */
	KT_ASSERT(wm_at(o.left, o.top) != wid);
	KT_ASSERT_EQ(wm_at(o.left + 12, o.top + 12), wid);

	/* off: square again, the corner the outline */
	KT_ASSERT_ER(wm_set(LK_LOOK, 0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(o.left, o.top), (INT)wm_look(WM_LOOK_FRAME));
	KT_ASSERT_EQ(wm_at(o.left, o.top), wid);

	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	lk_reset();
}

/*
 * The shadow: all its darkness inside the window's outline thrown by
 * the offset, then less and less to nothing at its size, measured row
 * by row below the window. Where the window stands it is not: a frame
 * seen through shows the desk behind it, not its own shadow.
 */
LOCAL UW lk_darker( UW c, UINT a )
{
	UW	r, g, b;

	r = ( ( ( c >> 16 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	g = ( ( ( c >> 8 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	b = ( ( c & 0xFF ) * ( 255 - a ) + 127 ) / 255;
	return ( r << 16 ) | ( g << 8 ) | b;
}

LOCAL void test_look_shadow( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid, d, x;
	UW		desk = 0x00C08040U, got, want, plain;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, desk), E_OK);
	lk_reset();
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "kage");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_DX, 4), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_DY, 4), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_SIZE, 8), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_DARK, 160), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	/* below the middle of the window: inside the thrown outline, then fading */
	x = ( o.left + o.right ) / 2;
	got = at(x, o.bottom + 2);
	want = lk_darker(desk, 160);
	lk_show("shadow 2 below the foot (inside)", got, want);
	KT_ASSERT_EQ((INT)got, (INT)want);
	for ( d = 1; d <= 9; d++ ) {
		INT	a = ( d * 16 >= 128 ) ? 0 : 160 * ( 128 - d * 16 ) / 128;

		got = at(x, o.bottom + 3 + d);
		want = lk_darker(desk, (UINT)a);
		tm_printf((UB *)"  look: shadow %d past its outline 0x%06x (worked out 0x%06x, %d of 255)\n",
			  d, got, want, a);
		KT_ASSERT_EQ((INT)got, (INT)want);
	}
	KT_ASSERT_EQ((INT)at(x, o.bottom + 3 + 9), (INT)desk);	/* past its size, nothing */
	KT_ASSERT_EQ((INT)at(o.left - 5, o.top + 40), (INT)desk);	/* thrown away from the left */

	/* the window does not darken itself, seen through or not */
	KT_ASSERT_ER(wm_set(LK_OPACITY, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(w.work.right - 12, w.work.top - 6);
	KT_ASSERT_ER(wm_set(LK_SHD_DARK, 0), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	plain = at(w.work.right - 12, w.work.top - 6);
	lk_show("band seen through, with a shadow", got, plain);
	KT_ASSERT_EQ((INT)got, (INT)plain);
	got = at(o.right - 3, o.bottom - 3);		/* where the shadow would fall on itself */
	KT_ASSERT_ER(wm_set(LK_SHD_DARK, 160), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	lk_show("foot corner seen through, with a shadow", at(o.right - 3, o.bottom - 3), got);
	KT_ASSERT_EQ((INT)at(o.right - 3, o.bottom - 3), (INT)got);

	/* closed, its shadow goes with it */
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, o.bottom + 2), (INT)desk);
	lk_reset();
}

/*
 * What shows through is spread (compose.c): halved, smoothed, doubled
 * back. Over a ground of black and white dots side by side, a frame
 * seen through shows the dots unspread, and spread an even grey: the
 * first halving weighs each dot's neighbours so that black and white
 * come to half each, 128 to the channel, and the grey stays. Over
 * stripes running down the screen, the spread of each column is the
 * spread of one row, worked out here step by step as compose.c does it
 * and compared pixel by pixel. A part of the screen built alone comes
 * out as the whole screen built does -- the margin round it is built
 * for the spread and put back -- also where two windows spread one
 * over the other.
 */
LOCAL UW	dots[4] = { 0x00000000U, 0x00FFFFFFU, 0x00FFFFFFU, 0x00000000U };

#define SP_STRIPE	20		/* the stripes repeat after so many dots */
#define SP_W		2048		/* the widest screen worked out here */

LOCAL UW	stripes[SP_STRIPE];
LOCAL INT	sp_in[SP_W], sp_a[SP_W], sp_b[SP_W];

LOCAL INT sp_at( CONST INT *v, INT n, INT i )
{
	return v[( i < 0 ) ? 0 : ( i >= n ) ? n - 1 : i];
}

/*
 * One channel of a row n wide spread, at depth 'depth' smoothed with
 * w[0..4], as compose.c does a part the same all the way down: there a
 * sum down a column of the same pixels is the sum along the row times
 * the weights' total, which the rounding divides out again, so that
 * each step comes to a sum along the row rounded by its own weights.
 */
LOCAL CONST INT *sp_chan( INT n, INT depth, CONST INT *w )
{
	INT	*cur = sp_a, *nxt = sp_b, *t, len[4], k, i, j, s;

	for ( i = 0; i < n; i++ ) cur[i] = sp_in[i];
	len[0] = n;
	for ( k = 1; k <= depth; k++ ) {		/* halved: 1 3 3 1 of 8 */
		len[k] = ( ( n - 1 ) >> k ) + 1;
		for ( i = 0; i < len[k]; i++ ) {
			s = sp_at(cur, len[k - 1], 2 * i - 1) + 3 * sp_at(cur, len[k - 1], 2 * i)
			    + 3 * sp_at(cur, len[k - 1], 2 * i + 1) + sp_at(cur, len[k - 1], 2 * i + 2);
			nxt[i] = ( s + 4 ) >> 3;
		}
		t = cur;  cur = nxt;  nxt = t;
	}
	for ( i = 0; i < len[depth]; i++ ) {		/* smoothed: of 64 */
		for ( s = 0, j = 0; j < 5; j++ ) s += w[j] * sp_at(cur, len[depth], i + j - 2);
		nxt[i] = ( s + 32 ) >> 6;
	}
	t = cur;  cur = nxt;  nxt = t;
	for ( k = depth; k >= 1; k-- ) {		/* doubled: 3 to 1 of 4 */
		for ( i = 0; i < len[k - 1]; i++ ) {
			INT	c = i >> 1;

			s = ( ( i & 1 ) != 0 ) ? 3 * sp_at(cur, len[k], c) + sp_at(cur, len[k], c + 1)
					       : sp_at(cur, len[k], c - 1) + 3 * sp_at(cur, len[k], c);
			nxt[i] = ( s + 2 ) >> 2;
		}
		t = cur;  cur = nxt;  nxt = t;
	}
	return cur;
}

LOCAL void test_look_blur( void )
{
	static CONST INT	w6[5] = { 0, 13, 38, 13, 0 };	/* 6 dots: halved twice, smoothed so */
	T_DPRECT		o, part;
	T_WMWIN			w;
	INT			wid, wid2, x, y, i, k, n, bad;
	UW			a, b, full, grey, solid[48], spread[48];

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	lk_reset();
	KT_ASSERT_ER(wm_set_wall(dots, 2, 2, WM_WALL_TILE), E_OK);
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "bokasu");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) {
		(void)wm_set_wall(NULL, 0, 0, WM_WALL_TILE);
		return;
	}
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	x = w.work.right - 14;
	y = w.work.top - 6;
	KT_ASSERT_ER(wm_composite(), E_OK);
	for ( i = 0; i < 48; i++ ) {
		solid[i] = at(x - 47 + i, y);		/* the band as it is, solid */
	}
	KT_ASSERT_ER(wm_set(LK_OPACITY, 128), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	a = at(x, y);
	b = at(x + 1, y);
	tm_printf((UB *)"  look: band over the dots, unspread: 0x%06x 0x%06x (%d apart)\n", a, b, lk_apart(a, b));
	KT_ASSERT(lk_apart(a, b) >= 100);

	KT_ASSERT_ER(wm_set(LK_BLUR, 4), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	a = at(x, y);
	b = at(x + 1, y);
	grey = lk_over(0x00808080U, solid[47], 128);
	tm_printf((UB *)"  look: band over the dots, spread 4: 0x%06x 0x%06x (worked out 0x%06x, over 0x808080)\n",
		  a, b, grey);
	KT_ASSERT_EQ((INT)a, (INT)grey);
	KT_ASSERT_EQ((INT)b, (INT)grey);
	/* the work area, solid, is not spread */
	KT_ASSERT_EQ((INT)at(w.work.left + 20, w.work.top + 20), (INT)wm_look(WM_LOOK_WORK));

	/* a small part built alone is what the whole screen built is */
	{
		UW	edge = at(x - 4, y), edge2 = at(x - 3, y - 4);

		full = at(x, y);
		rect(&part, x - 2, y - 2, x + 3, y + 3);
		KT_ASSERT_ER(wm_composite_at(&part), E_OK);
		tm_printf((UB *)"  look: built alone 0x%06x, built whole 0x%06x\n", at(x, y), full);
		KT_ASSERT_EQ((INT)at(x, y), (INT)full);
		KT_ASSERT_EQ((INT)at(x + 1, y), (INT)b);
		/* and round it, in the margin built for the spread, what was there */
		KT_ASSERT_EQ((INT)at(x - 4, y), (INT)edge);
		KT_ASSERT_EQ((INT)at(x - 3, y - 4), (INT)edge2);
	}

	/* over stripes, 6 dots: each pixel as the steps work it out */
	for ( i = 0; i < SP_STRIPE; i++ ) {
		stripes[i] = ( i < 8 ) ? 0x00000000U : ( i < 16 ) ? 0x00FF8040U : 0x00406080U;
	}
	KT_ASSERT_ER(wm_set_wall(stripes, SP_STRIPE, 1, WM_WALL_TILE), E_OK);
	KT_ASSERT_ER(wm_set(LK_BLUR, 6), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	n = ( (INT)spec.width < SP_W ) ? (INT)spec.width : SP_W;
	for ( i = 0; i < 48; i++ ) spread[i] = 0;
	for ( k = 0; k < 3; k++ ) {
		CONST INT	*v;

		for ( i = 0; i < n; i++ ) sp_in[i] = (INT)( ( stripes[i % SP_STRIPE] >> ( 8 * k ) ) & 0xFF );
		v = sp_chan(n, 2, w6);
		for ( i = 0; i < 48; i++ ) spread[i] |= (UW)v[x - 47 + i] << ( 8 * k );
	}
	for ( i = 0, bad = 0; i < 48; i++ ) {
		UW	want = lk_over(spread[i], solid[i], 128), got = at(x - 47 + i, y);

		if ( i % 8 == 7 ) {
			tm_printf((UB *)"  look: band over stripes at %d, spread 6: 0x%06x (worked out 0x%06x, "
				  "over 0x%06x)\n", x - 47 + i, got, want, spread[i]);
		}
		if ( got != want ) bad++;
	}
	KT_ASSERT_EQ(bad, 0);

	/*
	 * Two windows seen through all over, one over the other: what the
	 * upper spreads is what the lower left spread, and a part where both
	 * stand, built alone, is the whole screen's
	 */
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
	rect(&o, 560, 150, 860, 330);
	wid2 = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "kasanaru");
	KT_ASSERT(wid2 > 0);
	if ( wid2 > 0 ) {
		UW	before[25];

		KT_ASSERT_ER(wm_composite(), E_OK);
		for ( i = 0; i < 25; i++ ) before[i] = at(620 + i % 5, 220 + i / 5);
		rect(&part, 620, 220, 625, 225);
		KT_ASSERT_ER(wm_composite_at(&part), E_OK);
		for ( i = 0, bad = 0; i < 25; i++ ) {
			if ( at(620 + i % 5, 220 + i / 5) != before[i] ) bad++;
		}
		tm_printf((UB *)"  look: two spread one over the other, built alone 0x%06x, built whole 0x%06x; "
			  "%d of 25 differ\n", at(622, 222), before[12], bad);
		KT_ASSERT_EQ(bad, 0);
		KT_ASSERT_ER(wm_close(wid2), E_OK);
	}

	/* solid again: nothing to spread, the band is the band */
	KT_ASSERT_ER(wm_set(LK_OPACITY, 255), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ((INT)at(x, y), (INT)wm_look(WM_LOOK_TITLE_ON));

	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_set_wall(NULL, 0, 0, WM_WALL_TILE), E_OK);
	lk_reset();
}

/*
 * A window opening comes in smaller and fainter and arrives as itself;
 * one closing goes out as a ghost that is gone once its time is over.
 * The steps are taken by the window layer's own task, so the test only
 * waits.
 */
LOCAL void test_look_motion( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		wid;
	UW		desk = 0x00204060U, got;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	KT_ASSERT_ER(wm_set_look(WM_LOOK_DESK, desk), E_OK);
	lk_reset();
	KT_ASSERT_ER(wm_set(LK_MOTION, 1000), E_OK);
	rect(&o, 480, 100, 780, 280);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ugoki");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(( w.work.left + w.work.right ) / 2, ( w.work.top + w.work.bottom ) / 2);
	tm_printf((UB *)"  look: the middle as it opens 0x%06x, at its corner 0x%06x\n", got, at(o.left + 1, o.top + 1));
	KT_ASSERT(got != wm_look(WM_LOOK_WORK));	/* not yet all there */
	KT_ASSERT_EQ((INT)at(o.left + 1, o.top + 1), (INT)desk);	/* smaller than it will be */

	tk_dly_tsk(1500);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(( w.work.left + w.work.right ) / 2, ( w.work.top + w.work.bottom ) / 2);
	tm_printf((UB *)"  look: the middle once open 0x%06x\n", got);
	KT_ASSERT_EQ((INT)got, (INT)wm_look(WM_LOOK_WORK));

	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(( w.work.left + w.work.right ) / 2, ( w.work.top + w.work.bottom ) / 2);
	tm_printf((UB *)"  look: the middle as it closes 0x%06x\n", got);
	KT_ASSERT(got != desk);				/* still going */
	tk_dly_tsk(1500);
	KT_ASSERT_ER(wm_composite(), E_OK);
	got = at(( w.work.left + w.work.right ) / 2, ( w.work.top + w.work.bottom ) / 2);
	tm_printf((UB *)"  look: the middle once closed 0x%06x\n", got);
	KT_ASSERT_EQ((INT)got, (INT)desk);
	lk_reset();
}

/*
 * What the look costs, printed rather than held to a number (the
 * machine the tests run on is not the one that matters): the whole
 * screen built ten times with the look off, with the frames seen
 * through, round and shadowed, with what shows through spread, and with
 * the whole windows seen through; and a square the size of the pointer
 * built a hundred times over a frame that is spread.
 */
LOCAL UD lk_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UW)t.lo;
}

LOCAL INT lk_time( INT n, CONST T_DPRECT *part )
{
	UD	t0 = lk_ms();
	INT	i;

	for ( i = 0; i < n; i++ ) {
		(void)( ( part != NULL ) ? wm_composite_at(part) : wm_composite() );
	}
	return (INT)( lk_ms() - t0 );
}

LOCAL void test_look_cost( void )
{
	T_DPRECT	o, part;
	T_WMWIN		w;
	INT		wid[3], i, plain, frame, spread, whole, small;

	if ( !have_wm ) KT_SKIP("no windows");

	KT_ASSERT_ER(wm_set_scheme(WM_SCHEME_LIGHT), E_OK);
	lk_reset();
	for ( i = 0; i < 3; i++ ) {
		rect(&o, 100 + i * 120, 60 + i * 80, 700 + i * 120, 460 + i * 80);
		wid[i] = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR, "hiyou");
		KT_ASSERT(wid[i] > 0);
		if ( wid[i] <= 0 ) return;
	}
	KT_ASSERT_ER(wm_ref(wid[2], &w), E_OK);

	KT_ASSERT_ER(wm_set(LK_LOOK, 0), E_OK);
	plain = lk_time(10, NULL);
	KT_ASSERT_ER(wm_set(LK_LOOK, 1), E_OK);
	KT_ASSERT_ER(wm_set(LK_OPACITY, 160), E_OK);
	KT_ASSERT_ER(wm_set(LK_ROUND, 10), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_DARK, 110), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_SIZE, 10), E_OK);
	KT_ASSERT_ER(wm_set(LK_FROM, 0), E_OK);
	KT_ASSERT_ER(wm_set(LK_WASH_STR, 72), E_OK);
	KT_ASSERT_ER(wm_set(LK_BAR_STR, 110), E_OK);
	frame = lk_time(10, NULL);
	KT_ASSERT_ER(wm_set(LK_BLUR, 6), E_OK);
	spread = lk_time(10, NULL);
	rect(&part, w.work.right - 40, w.work.top - 20, w.work.right - 8, w.work.top + 12);
	small = lk_time(100, &part);
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
	whole = lk_time(10, NULL);
	tm_printf((UB *)"  look: ten whole screens, three windows: off %d ms, frames seen through, round, "
		  "shadowed, washed %d ms, and spread %d ms, the whole windows spread %d ms\n",
		  plain, frame, spread, whole);
	tm_printf((UB *)"  look: a hundred 32x32 squares over a spread frame: %d ms\n", small);
	KT_ASSERT(plain >= 0 && frame >= 0 && spread >= 0 && whole >= 0);

	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT_ER(wm_close(wid[i]), E_OK);
	}
	lk_reset();
}

/*
 * Kept from one start to the next: the machine's settings carry the
 * lines (WIN_*), and taking them again sets the look; a line out of its
 * range is held to it.
 */
LOCAL void test_look_kept( void )
{
	char	v[32];

	if ( !have_wm ) KT_SKIP("no windows");
	if ( wm_dev_conf_put("WIN_OPACITY", "200") < E_OK ) KT_SKIP("no machine settings object");

	lk_reset();
	KT_ASSERT_ER(wm_dev_conf_put("WIN_ROUND", "99"), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(wm_num(LK_OPACITY, 0), 200);
	KT_ASSERT_EQ(wm_num(LK_ROUND, 0), 32);		/* held to its range */
	KT_ASSERT(wm_dev_conf_get("WIN_OPACITY", v, sizeof(v)));
	KT_ASSERT_EQ(knl_strcmp(v, "200"), 0);

	/* back as nothing said */
	KT_ASSERT_ER(wm_dev_conf_put("WIN_OPACITY", "255"), E_OK);
	KT_ASSERT_ER(wm_dev_conf_put("WIN_ROUND", "0"), E_OK);
	KT_ASSERT_ER(wm_dev_conf_apply(), E_OK);
	KT_ASSERT_EQ(wm_num(LK_OPACITY, 0), 255);
	KT_ASSERT_EQ(wm_num(LK_ROUND, -1), 0);
	(void)wm_set_wall(NULL, 0, 0, WM_WALL_TILE);
	lk_reset();
}

/* ---------------------------------------------------------------- parts and the pointer */

/*
 * The screen as a sum for each block of SH_BLK pixels of a row: two
 * screens with the same sums are, as near as matters, the same screen,
 * and a block that differs says where.
 */
#define SH_BLK		64
#define SH_MAXW		2048
#define SH_MAXH		1600
#define SH_N		( ( SH_MAXW / SH_BLK ) * SH_MAXH )

LOCAL UW	sh_a[SH_N], sh_b[SH_N];

LOCAL void sh_take( UW *out )
{
	INT	w = ( (INT)spec.width < SH_MAXW ) ? (INT)spec.width : SH_MAXW;
	INT	h = ( (INT)spec.height < SH_MAXH ) ? (INT)spec.height : SH_MAXH;
	INT	nb = SH_MAXW / SH_BLK, x, y;

	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < w; x++ ) {
			UW	*s = &out[y * nb + x / SH_BLK];

			if ( x % SH_BLK == 0 ) *s = 2166136261U;
			*s = ( *s ^ at(x, y) ) * 16777619U;
		}
	}
}

/* How many blocks differ, the first few of them said */
LOCAL INT sh_differ( CONST char *what )
{
	INT	w = ( (INT)spec.width < SH_MAXW ) ? (INT)spec.width : SH_MAXW;
	INT	h = ( (INT)spec.height < SH_MAXH ) ? (INT)spec.height : SH_MAXH;
	INT	nb = SH_MAXW / SH_BLK, x, y, n = 0;

	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < ( w + SH_BLK - 1 ) / SH_BLK; x++ ) {
			if ( sh_a[y * nb + x] != sh_b[y * nb + x] ) {
				if ( n < 4 ) {
					tm_printf((UB *)"  parts: %s: the block at (%d,%d) differs\n", what, x * SH_BLK, y);
				}
				n++;
			}
		}
	}
	tm_printf((UB *)"  parts: %s: %d blocks differ from the whole screen built\n", what, n);
	return n;
}

/* The screen as it is, then built whole, compared: 0 when they are the same */
LOCAL INT as_whole( CONST char *what )
{
	sh_take(sh_a);
	(void)wm_composite();
	sh_take(sh_b);
	return sh_differ(what);
}

/* The pointer brought to (x, y) by an event put in, and the events taken again */
LOCAL BOOL pt_to( INT x, INT y )
{
	T_HIDEV	in;
	T_WMEV	ev;
	INT	px = -1, py = -1;
	UINT	b;

	knl_memset(&in, 0, sizeof(in));
	in.type = HID_EV_MOVE;
	in.x = x;
	in.y = y;
	if ( kt_inject(&in) < E_OK ) {
		return FALSE;
	}
	while ( wm_read_event(&ev, 30) >= E_OK ) ;
	return ( ts_hid_pointer(&px, &py, &b) >= E_OK && px == x && py == y );
}

/* A square of a window's work area painted and said to have changed */
LOCAL void paint( INT wid, INT l, INT t, INT r, INT b, UW colour )
{
	T_DPRECT	d;

	rect(&d, l, t, r, b);
	KT_ASSERT_ER(dp_fill_rect(wm_gid(wid), &d, colour), E_OK);
	KT_ASSERT_ER(wm_damage(wid, &d), E_OK);
}

/*
 * Two changes far apart, one near the top left and one near the bottom
 * right, are built and sent as two small parts, not as the screen
 * between them; and the screen is what the whole of it built would be.
 */
LOCAL void test_parts( void )
{
	T_WMSTAT	a, b;
	T_DISPSTAT	da, db;
	T_DPRECT	o;
	T_WMWIN		w1, w2;
	INT		one, two, sw = (INT)spec.width, sh = (INT)spec.height;
	UD		bound;

	if ( !have_wm ) KT_SKIP("no windows");
	lk_reset();
	rect(&o, 20, 20, 220, 180);
	one = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "hidariue");
	rect(&o, sw - 260, sh - 220, sw - 40, sh - 40);
	two = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "migishita");
	KT_ASSERT(one > 0 && two > 0);
	if ( one <= 0 || two <= 0 ) {
		if ( one > 0 ) (void)wm_close(one);
		if ( two > 0 ) (void)wm_close(two);
		return;
	}
	KT_ASSERT_ER(wm_ref(one, &w1), E_OK);
	KT_ASSERT_ER(wm_ref(two, &w2), E_OK);

	/* with the pointer laid once an update builds only what changed (the tests keep it off) */
	wm_show_pointer(TRUE);
	(void)pt_to(sw / 2, sh / 2);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);

	paint(one, 10, 10, 30, 30, 0x00C02040U);
	paint(two, 50, 60, 70, 80, 0x002040C0U);
	KT_ASSERT_ER(wm_stat(&a), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&da), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&db), E_OK);

	bound = (UD)( w2.work.left + 70 - ( w1.work.left + 10 ) ) * (UD)( w2.work.top + 80 - ( w1.work.top + 10 ) );
	tm_printf((UB *)"  parts: two changes far apart: %ld built in %ld parts, %ld sent in %ld rectangles "
		  "(one rectangle round both: %ld)\n", b.pixels - a.pixels, b.parts - a.parts,
		  db.pixels - da.pixels, db.rects - da.rects, bound);
	KT_ASSERT(b.parts - a.parts >= 2);
	KT_ASSERT(db.rects - da.rects >= 2);
	KT_ASSERT(b.pixels - a.pixels < bound / 8);
	KT_ASSERT(db.pixels - da.pixels < bound / 8);
	KT_ASSERT_EQ((INT)at(w1.work.left + 20, w1.work.top + 20), 0xC02040);
	KT_ASSERT_EQ((INT)at(w2.work.left + 60, w2.work.top + 70), 0x2040C0);
	KT_ASSERT_EQ(as_whole("two changes far apart"), 0);

	/* many small changes: no more than the list holds, the screen still right */
	{
		INT	i;

		for ( i = 0; i < 40; i++ ) {
			paint(( i & 1 ) ? one : two, 4 + ( i * 37 ) % 150, 4 + ( i * 23 ) % 100,
			      8 + ( i * 37 ) % 150, 8 + ( i * 23 ) % 100, 0x00010101U * (UW)( i * 5 ));
		}
		KT_ASSERT_ER(wm_stat(&a), E_OK);
		KT_ASSERT_ER(wm_update(), E_OK);
		KT_ASSERT_ER(wm_stat(&b), E_OK);
		KT_ASSERT(b.parts - a.parts <= 16);
		KT_ASSERT_EQ(as_whole("many small changes"), 0);
	}

	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_close(two), E_OK);
	KT_ASSERT_ER(wm_close(one), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/*
 * The pointer moved with nothing else changed: no window is laid, two
 * squares are sent, what it covered is back and it is where it was sent,
 * as the whole screen built would have it. So in another shape, at
 * another size, and under an outline being carried.
 */
LOCAL void test_pointer_save( void )
{
	T_WMSTAT	a, b;
	T_DISPSTAT	da, db;
	T_DPRECT	o, p0, p1, d;
	UW		under[WM_POINTER_SIDE * WM_POINTER_SIDE];
	INT		wid, i, x, y, lx, ly, side, differ;

	if ( !have_wm ) KT_SKIP("no windows");
	lk_reset();
	rect(&o, 200, 150, 560, 420);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "yubisaki");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_ER(wm_focus(wid), E_OK);
	paint(wid, 0, 0, 300, 200, 0x0080C0A0U);

	/* what lies at the new place, built with no pointer */
	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_composite(), E_OK);
	wm_pointer_box(400, 330, &p1);
	for ( i = 0; i < WM_POINTER_SIDE * WM_POINTER_SIDE; i++ ) {
		under[i] = at(p1.left + i % WM_POINTER_SIDE, p1.top + i / WM_POINTER_SIDE);
	}

	wm_show_pointer(TRUE);
	if ( !pt_to(300, 250) ) {
		wm_show_pointer(FALSE);
		(void)wm_close(wid);
		KT_SKIP("nothing points");
	}
	KT_ASSERT_ER(wm_composite(), E_OK);
	wm_pointer_box(300, 250, &p0);
	KT_ASSERT(wm_pointer_last(&lx, &ly, &side));
	KT_ASSERT_EQ(lx, p0.left);
	KT_ASSERT_EQ(ly, p0.top);

	/* moved: nothing built, the two squares sent */
	KT_ASSERT_ER(wm_stat(&a), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&da), E_OK);
	(void)pt_to(400, 330);
	KT_ASSERT_ER(wm_pointer_moved(400, 330), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&db), E_OK);
	tm_printf((UB *)"  pointer: moved: %ld builds, %ld windows laid, %ld moves (%ld with a build); "
		  "%ld pixels sent in %ld rectangles\n", b.composites - a.composites,
		  b.windows_drawn - a.windows_drawn, b.ptr_moves - a.ptr_moves, b.ptr_built - a.ptr_built,
		  db.pixels - da.pixels, db.rects - da.rects);
	KT_ASSERT_EQ((INT)( b.composites - a.composites ), 0);
	KT_ASSERT_EQ((INT)( b.windows_drawn - a.windows_drawn ), 0);
	KT_ASSERT(b.ptr_moves - a.ptr_moves >= 1);
	KT_ASSERT_EQ((INT)( b.ptr_built - a.ptr_built ), 0);
	KT_ASSERT(db.pixels - da.pixels <= (UD)( 2 * WM_POINTER_SIDE * WM_POINTER_SIDE ));
	KT_ASSERT(wm_pointer_last(&lx, &ly, &side));
	KT_ASSERT_EQ(lx, p1.left);
	KT_ASSERT_EQ(ly, p1.top);

	/* the old place as the window shows it; the new one with the pointer over what was there */
	for ( differ = 0, i = 0; i < WM_POINTER_SIDE * WM_POINTER_SIDE; i++ ) {
		x = p1.left + i % WM_POINTER_SIDE;
		y = p1.top + i / WM_POINTER_SIDE;
		if ( at(x, y) != under[i] ) differ++;
	}
	tm_printf((UB *)"  pointer: %d of its pixels at the new place differ from what is under it\n", differ);
	KT_ASSERT(differ > 50);
	KT_ASSERT_EQ((INT)at(p1.left, p1.top), 0x000000);		/* the tip, black */
	KT_ASSERT_EQ((INT)at(p0.left + 2, p0.top + 2), 0x80C0A0);	/* what it covered, back */
	KT_ASSERT_EQ(as_whole("the pointer moved"), 0);

	/* a step of a few dots, the two squares overlapping */
	KT_ASSERT_ER(wm_stat(&a), E_OK);
	(void)pt_to(403, 332);
	KT_ASSERT_ER(wm_pointer_moved(403, 332), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	KT_ASSERT_EQ((INT)( b.composites - a.composites ), 0);
	KT_ASSERT_EQ(as_whole("a step of a few dots"), 0);

	/* another shape, then larger: laid again in it, nothing built (the window's own change taken first) */
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_ER(wm_set_pointer(WM_PT_GRIP), E_OK);
	KT_ASSERT_ER(wm_stat(&a), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	KT_ASSERT_EQ((INT)( b.composites - a.composites ), 0);
	KT_ASSERT_EQ(as_whole("another shape"), 0);
	KT_ASSERT_ER(knl_look_setting(LK_PTR_SIZE, 2), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT(wm_pointer_last(&lx, &ly, &side));
	KT_ASSERT_EQ(side, WM_POINTER_SIDE * 3 / 2);
	KT_ASSERT_EQ(as_whole("larger"), 0);
	KT_ASSERT_ER(knl_look_setting(LK_PTR_SIZE, 1), E_OK);
	KT_ASSERT_ER(wm_set_pointer(WM_PT_SELECT), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);

	/* half off the screen's edge */
	(void)pt_to((INT)spec.width - 5, (INT)spec.height - 3);
	KT_ASSERT_ER(wm_pointer_moved((INT)spec.width - 5, (INT)spec.height - 3), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	KT_ASSERT_EQ(as_whole("at the edge"), 0);

	/* an outline carried across it: each edge built afresh, turned once, the pointer over it */
	(void)pt_to(380, 300);
	KT_ASSERT_ER(wm_pointer_moved(380, 300), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	rect(&d, 300, 290, 500, 380);
	KT_ASSERT_ER(wm_stat(&a), E_OK);
	KT_ASSERT_ER(wm_set_drag(&d, 1), E_OK);
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	tm_printf((UB *)"  pointer: an outline 200 by 90 laid: %ld pixels built in %ld parts\n",
		  b.pixels - a.pixels, b.parts - a.parts);
	KT_ASSERT(b.pixels - a.pixels < 200 * 90 / 2);
	KT_ASSERT_EQ((INT)at(450, 290), 0x80C0A0 ^ 0xFFFFFF);		/* the top edge, turned */
	KT_ASSERT_EQ(as_whole("an outline over the pointer"), 0);
	rect(&d, 304, 293, 504, 383);
	KT_ASSERT_ER(wm_set_drag(&d, 1), E_OK);
	(void)pt_to(384, 303);
	KT_ASSERT_ER(wm_pointer_moved(384, 303), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	KT_ASSERT_EQ(as_whole("the outline and the pointer moved"), 0);
	KT_ASSERT_ER(wm_set_drag(NULL, 0), E_OK);
	KT_ASSERT_EQ(as_whole("the outline taken away"), 0);

	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_EQ(as_whole("the pointer turned off"), 0);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/*
 * Several parts built apart come out as the whole screen built, also
 * where a window seen through all over and spread stands over and near
 * what changed: the change shows in the spread round it, which is built
 * with it, and the margin a spread needs is built and put back.
 */
LOCAL void test_parts_spread( void )
{
	T_DPRECT	o;
	T_WMWIN		wb;
	INT		under, over, far, sw = (INT)spec.width;

	if ( !have_wm ) KT_SKIP("no windows");
	lk_reset();
	for ( under = 0; under < SP_STRIPE; under++ ) {
		stripes[under] = ( under < 8 ) ? 0x00000000U : ( under < 16 ) ? 0x00FF8040U : 0x00406080U;
	}
	KT_ASSERT_ER(wm_set_wall(stripes, SP_STRIPE, 1, WM_WALL_TILE), E_OK);
	rect(&o, 160, 160, 460, 400);
	under = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "shita");
	rect(&o, 300, 120, 640, 360);
	over = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ue");
	rect(&o, sw - 240, 60, sw - 40, 200);
	far = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "tooku");
	KT_ASSERT(under > 0 && over > 0 && far > 0);
	if ( under <= 0 || over <= 0 || far <= 0 ) {
		if ( under > 0 ) (void)wm_close(under);
		if ( over > 0 ) (void)wm_close(over);
		if ( far > 0 ) (void)wm_close(far);
		(void)wm_set_wall(NULL, 0, 0, WM_WALL_TILE);
		return;
	}
	KT_ASSERT_ER(wm_raise(over), E_OK);
	KT_ASSERT_ER(wm_set(LK_OPACITY, 128), E_OK);
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
	KT_ASSERT_ER(wm_set(LK_BLUR, 6), E_OK);
	KT_ASSERT_ER(wm_set(LK_SHD_DARK, 96), E_OK);
	KT_ASSERT_ER(wm_ref(under, &wb), E_OK);

	wm_show_pointer(TRUE);
	(void)pt_to(sw / 2 + 100, 450);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);

	/* under the spread window, just inside its edge; near it outside; and far away */
	paint(under, 150, 20, 175, 45, 0x00FFFFFFU);
	paint(under, 120, 150, 135, 170, 0x0000FF00U);
	paint(far, 20, 20, 60, 50, 0x00FF0000U);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_EQ(as_whole("spread over and near the changes"), 0);

	/* the window seen through moved a little, and a part built alone in it */
	rect(&o, 310, 126, 650, 366);
	KT_ASSERT_ER(wm_move(over, &o), E_OK);
	paint(under, 140, 60, 160, 80, 0x00000000U);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_EQ(as_whole("the spread window moved"), 0);

	/* the pointer moved over the spread window: nothing built, the screen right */
	(void)pt_to(400, 250);
	KT_ASSERT_ER(wm_pointer_moved(400, 250), E_OK);
	KT_ASSERT_ER(wm_flush(), E_OK);		/* laid by the compositor */
	paint(under, 100, 30, 120, 50, 0x00FFFF00U);
	(void)pt_to(410, 255);
	KT_ASSERT_ER(wm_update(), E_OK);
	KT_ASSERT_EQ(as_whole("a change and the pointer over the spread"), 0);

	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_close(far), E_OK);
	KT_ASSERT_ER(wm_close(over), E_OK);
	KT_ASSERT_ER(wm_close(under), E_OK);
	KT_ASSERT_ER(wm_set_wall(NULL, 0, 0, WM_WALL_TILE), E_OK);
	lk_reset();
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/*
 * Many changes asked for within a refresh are built once: twenty small
 * paints, each asked for without waiting (wm_post), come to one or two
 * builds when everything asked is waited for (wm_flush), and the screen
 * is what the whole of it built shows.
 */
LOCAL void test_coalesce( void )
{
	T_WMSTAT	a, b;
	T_DPRECT	o;
	INT		wid, i;
	UD		t0, t1;

	if ( !have_wm ) KT_SKIP("no windows");
	if ( knl_wm_compositor() <= 0 ) KT_SKIP("no compositor");
	lk_reset();
	rect(&o, 100, 100, 500, 400);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "matomete");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	wm_show_pointer(TRUE);
	(void)pt_to(50, 50);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_update(), E_OK);
	tk_dly_tsk(40);					/* a refresh past the last build */

	KT_ASSERT_ER(wm_stat(&a), E_OK);
	(void)ts_get_mono(&t0);
	for ( i = 0; i < 20; i++ ) {
		T_DPRECT	d;

		rect(&d, 10 + i * 12, 20, 18 + i * 12, 40);
		KT_ASSERT_ER(dp_fill_rect(wm_gid(wid), &d, 0x00102030U * (UW)( i + 1 )), E_OK);
		KT_ASSERT_ER(wm_post(), E_OK);
	}
	(void)ts_get_mono(&t1);
	KT_ASSERT_ER(wm_flush(), E_OK);
	KT_ASSERT_ER(wm_stat(&b), E_OK);
	tm_printf((UB *)"  coalesce: 20 asks in %ld us came to %ld builds (%ld parts, %ld pixels)\n",
		  ( t1 - t0 ) / 1000, b.composites - a.composites, b.parts - a.parts, b.pixels - a.pixels);
	KT_ASSERT(b.composites - a.composites >= 1);
	KT_ASSERT(b.composites - a.composites <= 2);
	KT_ASSERT(b.pixels - a.pixels < 400 * 300);	/* the paints, not the window */
	KT_ASSERT_EQ(as_whole("twenty paints built together"), 0);

	/* drawn into the window with nothing said: the next update builds it all the same */
	{
		T_DPRECT	d;
		T_WMWIN		w;

		KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
		rect(&d, 200, 100, 260, 150);
		KT_ASSERT_ER(dp_fill_rect(wm_gid(wid), &d, 0x00A0B0C0U), E_OK);
		KT_ASSERT_ER(wm_update(), E_OK);
		KT_ASSERT_EQ((INT)at(w.work.left + 230, w.work.top + 120), 0xA0B0C0);
		KT_ASSERT_EQ(as_whole("drawn with nothing said"), 0);
	}

	/* the pointer moved by the input alone is laid by the compositor */
	{
		INT	lx, ly, side, k;
		T_DPRECT	want;

		KT_ASSERT(pt_to(70, 60));
		wm_pointer_box(70, 60, &want);
		for ( k = 0; k < 50; k++ ) {
			if ( wm_pointer_last(&lx, &ly, &side) && lx == want.left && ly == want.top ) {
				break;
			}
			tk_dly_tsk(2);
		}
		KT_ASSERT(k < 50);
	}

	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/* ---------------------------------------------------------------- the vector unit */

IMPORT void knl_wm_hold( BOOL hold );

#define SV_W		200
#define SV_H		140
#define SV_WINS		4
#define SV_SCENES	100

LOCAL UW	*sv_ground, *sv_a, *sv_b, *sv_pix[SV_WINS];
LOCAL UW	sv_seed;
LOCAL INT	sv_bad, sv_first;
LOCAL ID	sv_caller;

LOCAL UW sv_rand( void )
{
	sv_seed = sv_seed * 1103515245U + 12345U;
	return sv_seed >> 8;
}

LOCAL INT sv_in( INT lo, INT hi )
{
	return lo + (INT)( sv_rand() % (UW)( hi - lo + 1 ) );
}

/* A window of a scene, its look anything the stages take */
LOCAL void sv_win( T_CPWIN *w, INT k )
{
	INT	x, y, l, t, r, b, n;

	knl_memset(w, 0, sizeof(*w));
	l = sv_in(-30, SV_W - 20);
	t = sv_in(-30, SV_H - 20);
	r = l + sv_in(16, SV_W);
	b = t + sv_in(16, SV_H);
	w->outer.left = l;  w->outer.top = t;  w->outer.right = r;  w->outer.bottom = b;
	w->work.left = l + sv_in(0, 6);  w->work.top = t + sv_in(0, 12);
	w->work.right = r - sv_in(0, 6);  w->work.bottom = b - sv_in(0, 6);
	w->band = w->outer;
	w->band.bottom = ( sv_rand() & 1 ) ? w->work.top : w->outer.top;
	w->pixels = (CONST UB *)sv_pix[k];
	w->pitch = SV_W * 4;
	n = ( b - t ) * SV_W;
	for ( y = 0; y < n; y++ ) {
		x = (INT)sv_rand();
		sv_pix[k][y] = ( (UW)x * 2654435761U ) ^ ( ( ( x & 3 ) == 0 ) ? 0xFF000000U : 0 );
	}
	w->a_frame = ( sv_rand() % 3 == 0 ) ? 255 : (UINT)sv_in(0, 255);
	w->a_work = ( sv_rand() % 3 == 0 ) ? 255 : (UINT)sv_in(0, 255);
	w->keyed = (BOOL)( sv_rand() % 3 == 0 );
	w->round = ( sv_rand() & 1 ) ? sv_in(0, 16) : 0;
	w->wash = sv_rand() & 0x00FFFFFFU;
	w->wash_str = ( sv_rand() & 1 ) ? (UINT)sv_in(1, 255) : 0;
	w->bar_wash = sv_rand() & 0x00FFFFFFU;
	w->bar_str = ( sv_rand() & 1 ) ? (UINT)sv_in(1, 255) : 0;
	w->tint = sv_rand() & 0x00FFFFFFU;
	w->tint_str = ( sv_rand() % 3 == 0 ) ? (UINT)sv_in(1, 255) : 0;
	w->blur = ( sv_rand() & 1 ) ? sv_in(1, CP_BLUR_MAX) : 0;
	w->shd_dx = sv_in(-8, 8);
	w->shd_dy = sv_in(-8, 8);
	w->shd_size = sv_in(0, 10);
	w->shd_dark = ( sv_rand() & 1 ) ? (UINT)sv_in(1, 255) : 0;
	w->shown = w->outer;
	w->fade = 255;
}

/* One scene laid into 'back' over the ground, within 'box' */
LOCAL void sv_lay( UW *back, CONST T_DPRECT *box, CONST T_CPWIN *w, INT n )
{
	T_CPDST	d;
	INT	y, i;

	d.back = (UB *)back;
	d.pitch = SV_W * 4;
	d.box = *box;
	d.rows = SV_H;
	for ( y = box->top; y < box->bottom; y++ ) {
		knl_memcpy(back + y * SV_W + box->left, sv_ground + y * SV_W + box->left,
			   (SZ)( box->right - box->left ) * 4);
	}
	for ( i = 0; i < n; i++ ) {
		cp_window(&d, &w[i]);
	}
}

/*
 * The stages alone, without sending, on a back buffer the screen's size:
 * the ground and three windows laid over the whole of it, the plain
 * loops and the vector unit's, solid and square and with the look (seen
 * through all over, round, shadowed, spread, washed); microseconds a
 * build, the least of three.
 */
#define ST_W		1280
#define ST_H		800

LOCAL UD	st_us[2][2];		/* [look][vector unit] */

LOCAL void st_time( void )
{
	UW		*back = (UW *)Kmalloc(ST_W * ST_H * 4), *ground = (UW *)Kmalloc(ST_W * ST_H * 4);
	UW		*pix = (UW *)Kmalloc(ST_W * ST_H * 4);
	T_CPWIN		w[3];
	T_CPDST		d;
	UD		t0, t1;
	INT		k, v, rep, i, y;

	knl_memset(st_us, 0, sizeof(st_us));
	if ( back != NULL && ground != NULL && pix != NULL ) {
		for ( i = 0; i < ST_W * ST_H; i++ ) {
			ground[i] = (UW)i * 2654435761U;
			pix[i] = (UW)i * 2246822519U;
		}
		d.back = (UB *)back;
		d.pitch = ST_W * 4;
		d.box.left = 0;  d.box.top = 0;  d.box.right = ST_W;  d.box.bottom = ST_H;
		d.rows = ST_H;
		for ( k = 0; k < 2; k++ ) {
			for ( i = 0; i < 3; i++ ) {
				knl_memset(&w[i], 0, sizeof(w[i]));
				w[i].outer.left = 40 + i * 260;  w[i].outer.top = 40 + i * 120;
				w[i].outer.right = w[i].outer.left + 640;  w[i].outer.bottom = w[i].outer.top + 420;
				w[i].work = w[i].outer;
				w[i].work.left += 3;  w[i].work.top += 22;  w[i].work.right -= 4;  w[i].work.bottom -= 4;
				w[i].band = w[i].outer;
				w[i].band.bottom = w[i].work.top;
				w[i].pixels = (CONST UB *)pix;
				w[i].pitch = ST_W * 4;
				w[i].a_frame = w[i].a_work = ( k == 0 ) ? 255 : 200;
				if ( k == 1 ) {
					w[i].round = 8;
					w[i].shd_dx = 4;  w[i].shd_dy = 4;  w[i].shd_size = 8;  w[i].shd_dark = 96;
					w[i].blur = 6;
					w[i].wash = 0x202838;  w[i].wash_str = 60;
				}
				w[i].shown = w[i].outer;
				w[i].fade = 255;
			}
			for ( v = 0; v < 2; v++ ) {
				cp_simd_use((BOOL)( v != 0 ));
				for ( rep = 0; rep < 3; rep++ ) {
					knl_wm_hold(TRUE);
					(void)ts_get_mono(&t0);
					for ( y = 0; y < ST_H; y++ ) {
						cp_copy(back + y * ST_W, ground + y * ST_W, ST_W);
					}
					for ( i = 0; i < 3; i++ ) {
						cp_window(&d, &w[i]);
					}
					(void)ts_get_mono(&t1);
					knl_wm_hold(FALSE);
					if ( rep == 0 || ( t1 - t0 ) / 1000 < st_us[k][v] ) {
						st_us[k][v] = ( t1 - t0 ) / 1000;
					}
				}
			}
		}
		cp_simd_use(TRUE);
	}
	if ( back != NULL ) Kfree(back);
	if ( ground != NULL ) Kfree(ground);
	if ( pix != NULL ) Kfree(pix);
}

/*
 * Run in a task of its own that owns the FP/SIMD registers: each scene
 * laid by the plain loops and by the vector unit's, from the same
 * back buffer, and the two compared pixel by pixel.
 */
LOCAL void sv_task( INT stacd, void *exinf )
{
	T_CPWIN		w[SV_WINS];
	T_DPRECT	box;
	INT		s, i, n;

	(void)stacd;
	(void)exinf;
	cp_simd_allow(tk_get_tid(), TRUE);
	st_time();
	for ( s = 0; s < SV_SCENES; s++ ) {
		n = sv_in(1, SV_WINS);
		for ( i = 0; i < n; i++ ) {
			sv_win(&w[i], i);
		}
		if ( ( s & 1 ) != 0 ) {
			box.left = 0;  box.top = 0;  box.right = SV_W;  box.bottom = SV_H;
		} else {
			box.left = sv_in(0, SV_W / 2);  box.top = sv_in(0, SV_H / 2);
			box.right = box.left + sv_in(1, SV_W - box.left);
			box.bottom = box.top + sv_in(1, SV_H - box.top);
		}
		for ( i = 0; i < SV_W * SV_H; i++ ) {
			sv_ground[i] = sv_rand() * 2246822519U;
			sv_a[i] = sv_b[i] = 0xA5000000U ^ ( (UW)i * 0x9E3779B1U );
		}
		knl_wm_hold(TRUE);
		cp_simd_use(FALSE);
		sv_lay(sv_a, &box, w, n);
		cp_simd_use(TRUE);
		sv_lay(sv_b, &box, w, n);
		knl_wm_hold(FALSE);
		for ( i = 0; i < SV_W * SV_H; i++ ) {
			if ( sv_a[i] != sv_b[i] ) {
				if ( sv_bad++ == 0 ) {
					sv_first = s * 1000000 + i;
				}
			}
		}
	}
	cp_simd_allow(tk_get_tid(), FALSE);
	tk_wup_tsk(sv_caller);
	tk_exd_tsk();
}

/* What the vector unit lays is what the plain loops lay, over random scenes */
LOCAL void test_simd_same( void )
{
	T_CTSK	ct;
	ID	tid;
	INT	i;
	BOOL	ok = TRUE;

	if ( !have_wm ) KT_SKIP("no windows");
	sv_ground = (UW *)Kmalloc(SV_W * SV_H * 4);
	sv_a = (UW *)Kmalloc(SV_W * SV_H * 4);
	sv_b = (UW *)Kmalloc(SV_W * SV_H * 4);
	for ( i = 0; i < SV_WINS; i++ ) {
		sv_pix[i] = (UW *)Kmalloc(SV_W * ( SV_H + 40 ) * 4);
		ok = (BOOL)( ok && sv_pix[i] != NULL );
	}
	ok = (BOOL)( ok && sv_ground != NULL && sv_a != NULL && sv_b != NULL );
	KT_ASSERT(ok);
	sv_seed = 20261002U;
	sv_bad = 0;
	sv_first = -1;
	sv_caller = tk_get_tid();
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)sv_task;
	ct.itskpri = 20;
	ct.stksz = 32 * 1024;
	tid = ok ? tk_cre_tsk(&ct) : E_NOMEM;
	KT_ASSERT(tid > 0);
	if ( tid > 0 ) {
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		KT_ASSERT_ER(tk_slp_tsk(120000), E_OK);
		tk_dly_tsk(10);
		(void)tk_del_tsk(tid);
	}
	tm_printf((UB *)"  simd: %d scenes, %d pixels differ (first at scene %d, pixel %d)\n",
		  SV_SCENES, sv_bad, ( sv_first < 0 ) ? -1 : sv_first / 1000000,
		  ( sv_first < 0 ) ? -1 : sv_first % 1000000);
	KT_ASSERT_EQ(sv_bad, 0);
	tm_printf((UB *)"  simd: the stages alone, %dx%d and three windows: solid and square %ld us plain, "
		  "%ld us vector; with the look %ld us plain, %ld us vector\n", ST_W, ST_H,
		  st_us[0][0], st_us[0][1], st_us[1][0], st_us[1][1]);
	cp_simd_use(TRUE);
	for ( i = 0; i < SV_WINS; i++ ) {
		if ( sv_pix[i] != NULL ) Kfree(sv_pix[i]);
	}
	if ( sv_ground != NULL ) Kfree(sv_ground);
	if ( sv_a != NULL ) Kfree(sv_a);
	if ( sv_b != NULL ) Kfree(sv_b);
}

/*
 * How long the compositor takes to build the whole screen, with the
 * plain loops and with the vector unit's: plain windows, and windows
 * seen through with round corners, shadows and a spread. Printed; the
 * screens built the two ways are the same.
 */
LOCAL UD sv_build_us( INT times )
{
	UD	t0, t1, sum = 0;
	INT	i;

	for ( i = 0; i < times; i++ ) {
		tk_dly_tsk(30);				/* a refresh past the last build */
		(void)ts_get_mono(&t0);
		(void)wm_composite();
		(void)ts_get_mono(&t1);
		sum += t1 - t0;
	}
	return sum / (UD)times / 1000;
}

LOCAL void test_simd_time( void )
{
	T_DPRECT	o;
	INT		wid[3], i, k, sw = (INT)spec.width, sh = (INT)spec.height;
	UD		plain[2], vec[2];

	if ( !have_wm ) KT_SKIP("no windows");
	if ( knl_wm_compositor() <= 0 ) KT_SKIP("no compositor");
	lk_reset();
	rect(&o, 40, 40, sw * 2 / 3, sh * 2 / 3);
	wid[0] = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ichi");
	rect(&o, sw / 4, sh / 4, sw - 60, sh - 80);
	wid[1] = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ni");
	rect(&o, sw / 2, 60, sw - 20, sh / 2);
	wid[2] = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "san");
	KT_ASSERT(wid[0] > 0 && wid[1] > 0 && wid[2] > 0);
	for ( k = 0; k < 2; k++ ) {
		if ( k == 1 ) {
			KT_ASSERT_ER(wm_set(LK_OPACITY, 200), E_OK);
			KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
			KT_ASSERT_ER(wm_set(LK_ROUND, 8), E_OK);
			KT_ASSERT_ER(wm_set(LK_SHD_DARK, 96), E_OK);
			KT_ASSERT_ER(wm_set(LK_SHD_SIZE, 8), E_OK);
			KT_ASSERT_ER(wm_set(LK_BLUR, 6), E_OK);
			KT_ASSERT_ER(wm_set(LK_WASH_STR, 60), E_OK);
		}
		KT_ASSERT_ER(wm_composite(), E_OK);
		cp_simd_use(FALSE);
		plain[k] = sv_build_us(4);
		sh_take(sh_a);
		cp_simd_use(TRUE);
		vec[k] = sv_build_us(4);
		sh_take(sh_b);
		KT_ASSERT_EQ(sh_differ(( k == 0 ) ? "built plain and by the vector unit"
					: "the look built plain and by the vector unit"), 0);
		tm_printf((UB *)"  simd: the whole screen (%dx%d) %s: plain loops %ld us, vector unit %ld us\n",
			  sw, sh, ( k == 0 ) ? "solid and square" : "seen through, round, shadowed, spread",
			  plain[k], vec[k]);
	}
	for ( i = 0; i < 3; i++ ) {
		if ( wid[i] > 0 ) (void)wm_close(wid[i]);
	}
	lk_reset();
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/* ---------------------------------------------------------------- what is hidden */

/*
 * The windows laid each only where those in front leave it to show
 * (compose.c cp_lay) come out as every window laid whole, one after
 * another (cp_window): over random scenes of up to HD_WINS windows, most
 * of them solid in part or all over, some on their way in or out, with
 * the plain loops and the vector unit's, built whole and in pieces; and
 * over scenes the screen's size, timed both ways.
 */
#define HD_WINS		12
#define HD_SCENES	200
#define HT_WINS		8
#define HT_SCENES	3

LOCAL INT	hd_bad, hd_first, ht_bad;
LOCAL UD	hd_left;			/* pixels left out over the scenes */
LOCAL UD	ht_us[HT_SCENES][2];		/* [scene][hidden left out] */
LOCAL UD	ht_px[HT_SCENES];		/* pixels left out in one build */
LOCAL UD	ht_part[4];			/* ground, shadows, spreads, mixing */

LOCAL CONST char	*ht_what[HT_SCENES] = {
	"solid and square",
	"frames seen through, round, shadowed, spread",
	"seen through all over, round, shadowed, spread"
};

/* A window of a scene, solid more often than not, now and then on its way in or out */
LOCAL void hd_win( T_CPWIN *w, INT k )
{
	sv_win(w, k % SV_WINS);
	if ( sv_rand() % 3 != 0 ) {
		w->a_frame = 255;
		if ( sv_rand() % 3 != 0 ) {
			w->a_work = 255;
			w->keyed = FALSE;
		}
	}
	if ( sv_rand() % 8 == 0 ) {
		w->shown.left += sv_in(0, 10);
		w->shown.top += sv_in(0, 10);
		w->shown.right -= sv_in(0, 10);
		w->shown.bottom -= sv_in(0, 10);
		w->fade = (UINT)sv_in(0, 255);
	}
}

/* The ground into 'back' within 'box', and the windows over it laid together */
LOCAL void hd_lay( UW *back, CONST T_DPRECT *box, CONST T_CPWIN *w, INT n )
{
	T_CPDST	d;
	UD	h0 = cp_hidden();

	sv_lay(back, box, w, 0);
	d.back = (UB *)back;
	d.pitch = SV_W * 4;
	d.box = *box;
	d.rows = SV_H;
	cp_lay(&d, w, n);
	hd_left += cp_hidden() - h0;
}

/* Eight windows down the screen, each over the last, in the look of scene k */
LOCAL void ht_scene( T_CPWIN *w, INT k, CONST UW *pix )
{
	INT	i;

	for ( i = 0; i < HT_WINS; i++ ) {
		knl_memset(&w[i], 0, sizeof(w[i]));
		w[i].outer.left = 40 + i * 70;  w[i].outer.top = 30 + i * 50;
		w[i].outer.right = w[i].outer.left + 640;  w[i].outer.bottom = w[i].outer.top + 420;
		w[i].work = w[i].outer;
		w[i].work.left += 3;  w[i].work.top += 22;  w[i].work.right -= 4;  w[i].work.bottom -= 4;
		w[i].band = w[i].outer;
		w[i].band.bottom = w[i].work.top;
		w[i].pixels = (CONST UB *)( pix + i * 8 );
		w[i].pitch = ST_W * 4;
		w[i].a_frame = ( k == 0 ) ? 255 : 200;
		w[i].a_work = ( k == 2 ) ? 200 : 255;
		if ( k != 0 ) {
			w[i].round = 8;
			w[i].shd_dx = 4;  w[i].shd_dy = 4;  w[i].shd_size = 8;  w[i].shd_dark = 96;
			w[i].blur = 6;
			w[i].wash = 0x202838;  w[i].wash_str = 60;
		}
		w[i].shown = w[i].outer;
		w[i].fade = 255;
	}
}

/*
 * The scenes the screen's size, on the vector unit as the compositor
 * lays them: every window whole and what is hidden left out, the least
 * of three builds each, and the two compared; and, every window whole,
 * where the time of the second scene goes.
 */
LOCAL void ht_time( void )
{
	UW		*ground = (UW *)Kmalloc(ST_W * ST_H * 4), *pix = (UW *)Kmalloc(ST_W * ST_H * 4);
	UW		*back[2];
	T_CPWIN		w[HT_WINS];
	T_CPDST		d;
	UD		t0, t1, t2, t3, h0;
	INT		k, h, rep, i, y;

	back[0] = (UW *)Kmalloc(ST_W * ST_H * 4);
	back[1] = (UW *)Kmalloc(ST_W * ST_H * 4);
	knl_memset(ht_us, 0, sizeof(ht_us));
	knl_memset(ht_part, 0, sizeof(ht_part));
	ht_bad = -1;
	if ( ground == NULL || pix == NULL || back[0] == NULL || back[1] == NULL ) {
		goto out;
	}
	ht_bad = 0;
	for ( i = 0; i < ST_W * ST_H; i++ ) {
		ground[i] = (UW)i * 2654435761U;
		pix[i] = (UW)i * 2246822519U;
	}
	d.pitch = ST_W * 4;
	d.box.left = 0;  d.box.top = 0;  d.box.right = ST_W;  d.box.bottom = ST_H;
	d.rows = ST_H;
	for ( k = 0; k < HT_SCENES; k++ ) {
		ht_scene(w, k, pix);
		for ( h = 0; h < 2; h++ ) {
			d.back = (UB *)back[h];
			for ( rep = 0; rep < 3; rep++ ) {
				knl_wm_hold(TRUE);
				cp_hide_use((BOOL)( h != 0 ));
				h0 = cp_hidden();
				(void)ts_get_mono(&t0);
				for ( y = 0; y < ST_H; y++ ) {
					cp_copy(back[h] + y * ST_W, ground + y * ST_W, ST_W);
				}
				cp_lay(&d, w, HT_WINS);
				(void)ts_get_mono(&t1);
				ht_px[k] = cp_hidden() - h0;
				cp_hide_use(TRUE);
				knl_wm_hold(FALSE);
				if ( rep == 0 || ( t1 - t0 ) / 1000 < ht_us[k][h] ) {
					ht_us[k][h] = ( t1 - t0 ) / 1000;
				}
			}
		}
		for ( i = 0; i < ST_W * ST_H; i++ ) {
			if ( back[0][i] != back[1][i] ) {
				ht_bad++;
			}
		}
	}

	/* the second scene, every window whole, a stage at a time */
	ht_scene(w, 1, pix);
	d.back = (UB *)back[0];
	knl_wm_hold(TRUE);
	(void)ts_get_mono(&t0);
	for ( y = 0; y < ST_H; y++ ) {
		cp_copy(back[0] + y * ST_W, ground + y * ST_W, ST_W);
	}
	(void)ts_get_mono(&t1);
	ht_part[0] = ( t1 - t0 ) / 1000;
	for ( i = 0; i < HT_WINS; i++ ) {
		(void)ts_get_mono(&t0);
		cp_shadow(&d, &w[i]);
		(void)ts_get_mono(&t1);
		cp_blur(&d, &w[i]);
		(void)ts_get_mono(&t2);
		cp_blend(&d, &w[i]);
		(void)ts_get_mono(&t3);
		ht_part[1] += ( t1 - t0 ) / 1000;
		ht_part[2] += ( t2 - t1 ) / 1000;
		ht_part[3] += ( t3 - t2 ) / 1000;
	}
	knl_wm_hold(FALSE);
out:
	if ( ground != NULL ) Kfree(ground);
	if ( pix != NULL ) Kfree(pix);
	if ( back[0] != NULL ) Kfree(back[0]);
	if ( back[1] != NULL ) Kfree(back[1]);
}

/* Run in a task that owns the FP/SIMD registers, as sv_task is */
LOCAL void hd_task( INT stacd, void *exinf )
{
	T_CPWIN		w[HD_WINS];
	T_DPRECT	box;
	INT		s, i, n;

	(void)stacd;
	(void)exinf;
	cp_simd_allow(tk_get_tid(), TRUE);
	for ( s = 0; s < HD_SCENES; s++ ) {
		n = sv_in(2, HD_WINS);
		for ( i = 0; i < n; i++ ) {
			hd_win(&w[i], i);
		}
		if ( ( s & 1 ) != 0 ) {
			box.left = 0;  box.top = 0;  box.right = SV_W;  box.bottom = SV_H;
		} else {
			box.left = sv_in(0, SV_W / 2);  box.top = sv_in(0, SV_H / 2);
			box.right = box.left + sv_in(1, SV_W - box.left);
			box.bottom = box.top + sv_in(1, SV_H - box.top);
		}
		for ( i = 0; i < SV_W * SV_H; i++ ) {
			sv_ground[i] = sv_rand() * 2246822519U;
			sv_a[i] = sv_b[i] = 0xA5000000U ^ ( (UW)i * 0x9E3779B1U );
		}
		knl_wm_hold(TRUE);
		cp_simd_use((BOOL)( ( s & 2 ) != 0 ));
		sv_lay(sv_a, &box, w, n);
		hd_lay(sv_b, &box, w, n);
		cp_simd_use(TRUE);
		knl_wm_hold(FALSE);
		for ( i = 0; i < SV_W * SV_H; i++ ) {
			if ( sv_a[i] != sv_b[i] ) {
				if ( hd_bad++ == 0 ) {
					hd_first = s * 1000000 + i;
				}
			}
		}
	}
	ht_time();
	cp_simd_allow(tk_get_tid(), FALSE);
	tk_wup_tsk(sv_caller);
	tk_exd_tsk();
}

/*
 * A shadow looked up in its table is the shadow worked out pixel by
 * pixel: outside the window, where nothing of it covers the shadow, every
 * pixel is darkened as the distance from the outline thrown says, for
 * windows of every size, corner, offset, fade and darkness.
 */
#define SH_W		400
#define SH_H		300
#define SH_GREY		0x00808080U

LOCAL UW sh_sqrt( UW v )
{
	UW	r = 0;

	while ( ( r + 1 ) * ( r + 1 ) <= v ) {
		r++;
	}
	return r;
}

LOCAL UINT sh_ref( CONST T_CPWIN *w, CONST T_DPRECT *s, INT rad, INT x, INT y )
{
	INT	l = s->left + rad, r = s->right - 1 - rad, t = s->top + rad, b = s->bottom - 1 - rad;
	INT	cx = ( x < l ) ? l : ( x > r ) ? r : x, cy = ( y < t ) ? t : ( y > b ) ? b : y;
	INT	dx = ( x > cx ) ? x - cx : cx - x, dy = ( y > cy ) ? y - cy : cy - y;
	INT	d16, size16 = w->shd_size * 16;

	if ( dx == 0 ) d16 = dy * 16;
	else if ( dy == 0 ) d16 = dx * 16;
	else d16 = (INT)sh_sqrt((UW)( dx * dx + dy * dy ) * 256U);
	d16 -= rad * 16;
	if ( d16 <= 0 ) return w->shd_dark;
	if ( d16 >= size16 ) return 0;
	return (UINT)( (INT)w->shd_dark * ( size16 - d16 ) / size16 );
}

LOCAL UW sh_darker( UW c, UINT a )
{
	UINT	k = 255U - a;

	return ( ( ( ( ( c >> 16 ) & 0xFFU ) * k + 127U ) / 255U ) << 16 )
	     | ( ( ( ( ( c >> 8 ) & 0xFFU ) * k + 127U ) / 255U ) << 8 )
	     | ( ( ( c & 0xFFU ) * k + 127U ) / 255U );
}

LOCAL void test_shadow_same( void )
{
	UW		*buf = (UW *)Kmalloc(SH_W * SH_H * 4);
	T_CPDST		d;
	T_CPWIN		w;
	T_DPRECT	s;
	UD		t0, t1, us = 0;
	INT		n, x, y, rad, bad = 0, seen = 0;
	UW		seed = 12345;

	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	knl_memset(&d, 0, sizeof(d));
	d.back = (UB *)buf;
	d.pitch = SH_W * 4;
	d.box.right = SH_W;
	d.box.bottom = SH_H;
	d.rows = SH_H;
	for ( n = 0; n < 300 && bad == 0; n++ ) {
		INT	ww, hh;

		for ( x = 0; x < SH_W * SH_H; x++ ) buf[x] = SH_GREY;
		knl_memset(&w, 0, sizeof(w));
		seed = seed * 1103515245U + 12345U;  ww = 20 + (INT)( ( seed >> 8 ) % 260 );
		seed = seed * 1103515245U + 12345U;  hh = 20 + (INT)( ( seed >> 8 ) % 200 );
		seed = seed * 1103515245U + 12345U;  w.outer.left = (INT)( ( seed >> 8 ) % SH_W ) - 60;
		seed = seed * 1103515245U + 12345U;  w.outer.top = (INT)( ( seed >> 8 ) % SH_H ) - 60;
		w.outer.right = w.outer.left + ww;
		w.outer.bottom = w.outer.top + hh;
		w.shown = w.outer;
		seed = seed * 1103515245U + 12345U;  w.round = (INT)( ( seed >> 8 ) % ( CP_ROUND_MAX + 1 ) );
		seed = seed * 1103515245U + 12345U;  w.shd_size = (INT)( ( seed >> 8 ) % 33 );
		seed = seed * 1103515245U + 12345U;  w.shd_dx = (INT)( ( seed >> 8 ) % 65 ) - 32;
		seed = seed * 1103515245U + 12345U;  w.shd_dy = (INT)( ( seed >> 8 ) % 65 ) - 32;
		seed = seed * 1103515245U + 12345U;  w.shd_dark = 1 + ( ( seed >> 8 ) % 255 );
		(void)ts_get_mono(&t0);
		cp_shadow(&d, &w);
		(void)ts_get_mono(&t1);
		us += ( t1 - t0 ) / 1000;
		rad = cp_round_of(&w.outer, w.round);
		if ( cp_corner_cov(rad) == NULL ) rad = 0;
		s = w.outer;
		s.left += w.shd_dx;  s.right += w.shd_dx;
		s.top += w.shd_dy;   s.bottom += w.shd_dy;
		for ( y = 0; y < SH_H && bad == 0; y++ ) {
			for ( x = 0; x < SH_W; x++ ) {
				UW	want;

				if ( x >= w.outer.left && x < w.outer.right && y >= w.outer.top && y < w.outer.bottom ) {
					continue;
				}
				want = sh_darker(SH_GREY, sh_ref(&w, &s, rad, x, y));
				seen++;
				if ( buf[y * SH_W + x] != want ) {
					tm_printf((UB *)"  shadow: window %d (%d %d %d %d round %d, size %d, off %d %d, dark %d) "
						  "at %d,%d holds %06x, meant %06x\n", n, w.outer.left, w.outer.top,
						  w.outer.right, w.outer.bottom, w.round, w.shd_size, w.shd_dx, w.shd_dy,
						  (INT)w.shd_dark, x, y, buf[y * SH_W + x], want);
					bad++;
					break;
				}
			}
		}
	}
	tm_printf((UB *)"  shadow: %d windows, %d pixels compared, %d differ; %ld us of shadows\n", n, seen, bad, us);
	KT_ASSERT_EQ(bad, 0);
	Kfree(buf);
}

LOCAL void test_hide_same( void )
{
	T_CTSK	ct;
	ID	tid;
	INT	i, k;
	BOOL	ok = TRUE;

	if ( !have_wm ) KT_SKIP("no windows");
	sv_ground = (UW *)Kmalloc(SV_W * SV_H * 4);
	sv_a = (UW *)Kmalloc(SV_W * SV_H * 4);
	sv_b = (UW *)Kmalloc(SV_W * SV_H * 4);
	for ( i = 0; i < SV_WINS; i++ ) {
		sv_pix[i] = (UW *)Kmalloc(SV_W * ( SV_H + 40 ) * 4);
		ok = (BOOL)( ok && sv_pix[i] != NULL );
	}
	ok = (BOOL)( ok && sv_ground != NULL && sv_a != NULL && sv_b != NULL );
	KT_ASSERT(ok);
	sv_seed = 20261003U;
	hd_bad = 0;
	hd_first = -1;
	hd_left = 0;
	sv_caller = tk_get_tid();
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)hd_task;
	ct.itskpri = 20;
	ct.stksz = 32 * 1024;
	tid = ok ? tk_cre_tsk(&ct) : E_NOMEM;
	KT_ASSERT(tid > 0);
	if ( tid > 0 ) {
		KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
		KT_ASSERT_ER(tk_slp_tsk(600000), E_OK);
		tk_dly_tsk(10);
		(void)tk_del_tsk(tid);
	}
	tm_printf((UB *)"  hide: %d scenes of up to %d windows, %d pixels differ from every window laid whole "
		  "(first at scene %d, pixel %d); %ld kpx of windows left out\n",
		  HD_SCENES, HD_WINS, hd_bad, ( hd_first < 0 ) ? -1 : hd_first / 1000000,
		  ( hd_first < 0 ) ? -1 : hd_first % 1000000, hd_left / 1000);
	KT_ASSERT_EQ(hd_bad, 0);
	KT_ASSERT(hd_left > 0);
	for ( k = 0; k < HT_SCENES; k++ ) {
		tm_printf((UB *)"  hide: the stages alone, %dx%d, %d windows %s: every window whole %ld us, "
			  "hidden left out %ld us (%ld kpx left out)\n", ST_W, ST_H, HT_WINS, ht_what[k],
			  ht_us[k][0], ht_us[k][1], ht_px[k] / 1000);
	}
	tm_printf((UB *)"  hide: every window whole, %s: ground %ld us, shadows %ld us, spreads %ld us, "
		  "mixing %ld us; %d pixels differ the two ways\n", ht_what[1],
		  ht_part[0], ht_part[1], ht_part[2], ht_part[3], ht_bad);
	KT_ASSERT_EQ(ht_bad, 0);
	cp_hide_use(TRUE);
	cp_simd_use(TRUE);
	for ( i = 0; i < SV_WINS; i++ ) {
		if ( sv_pix[i] != NULL ) Kfree(sv_pix[i]);
	}
	if ( sv_ground != NULL ) Kfree(sv_ground);
	if ( sv_a != NULL ) Kfree(sv_a);
	if ( sv_b != NULL ) Kfree(sv_b);
}

/*
 * The compositor building the whole screen with six windows open one
 * over another, every window laid whole and what is hidden left out:
 * the screens the same, and how long each took, printed. The processor
 * builds them all, a GPU or not.
 */
LOCAL void test_hide_time( void )
{
	T_DPRECT	o;
	INT		wid[6], i, k, sw = (INT)spec.width, sh = (INT)spec.height;
	UD		whole[HT_SCENES], left[HT_SCENES];
	BOOL		ok = TRUE;

	if ( !have_wm ) KT_SKIP("no windows");
	if ( knl_wm_compositor() <= 0 ) KT_SKIP("no compositor");
	lk_reset();
	KT_ASSERT_ER(wm_set(LK_GPU, 0), E_OK);
	for ( i = 0; i < 6; i++ ) {
		rect(&o, 30 + i * sw / 14, 30 + i * sh / 14, 30 + i * sw / 14 + sw / 2, 30 + i * sh / 14 + sh / 2);
		wid[i] = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "kasane");
		ok = (BOOL)( ok && wid[i] > 0 );
	}
	KT_ASSERT(ok);
	for ( k = 0; ok && k < HT_SCENES; k++ ) {
		if ( k == 1 ) {
			KT_ASSERT_ER(wm_set(LK_OPACITY, 200), E_OK);
			KT_ASSERT_ER(wm_set(LK_ROUND, 8), E_OK);
			KT_ASSERT_ER(wm_set(LK_SHD_DARK, 96), E_OK);
			KT_ASSERT_ER(wm_set(LK_SHD_SIZE, 8), E_OK);
			KT_ASSERT_ER(wm_set(LK_BLUR, 6), E_OK);
			KT_ASSERT_ER(wm_set(LK_WASH_STR, 60), E_OK);
		}
		if ( k == 2 ) {
			KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
		}
		KT_ASSERT_ER(wm_composite(), E_OK);
		cp_hide_use(FALSE);
		whole[k] = sv_build_us(4);
		sh_take(sh_a);
		cp_hide_use(TRUE);
		left[k] = sv_build_us(4);
		sh_take(sh_b);
		KT_ASSERT_EQ(sh_differ("every window laid whole and what is hidden left out"), 0);
		tm_printf((UB *)"  hide: the whole screen (%dx%d), six windows %s: every window whole %ld us, "
			  "hidden left out %ld us\n", sw, sh, ht_what[k], whole[k], left[k]);
	}
	cp_hide_use(TRUE);
	for ( i = 0; i < 6; i++ ) {
		if ( wid[i] > 0 ) (void)wm_close(wid[i]);
	}
	lk_reset();
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/*
 * The pace of the builds (design 10.4.5). A window drawn in and put
 * frame after frame, as a program that animates does, is built sixty
 * times a second, not fewer: the compositor keeps to places a refresh
 * apart, slept for to the microsecond. On a screen that flips and says
 * when a flip was taken (a refresh made up for QEMU's screen,
 * knl_disp_sim_blank), the sends keep the pace, one a refresh, and the
 * compositor does not sleep for it as well.
 */
#define PACE_FRAMES	60

typedef struct {
	UD		t0;		/* when the run began (ts_get_mono, ns) */
	UD		us;		/* the whole run */
	T_WMFRAMES	w;		/* what the counts came to */
	T_DISPFRAME	d;
} PACE;

LOCAL void pace_run( INT gid, PACE *o )
{
	T_WMFRAMES	a, b;
	T_DISPFRAME	da, db;
	T_DPRECT	r;
	UD		t0, t1;
	INT		i;

	knl_memset(&da, 0, sizeof(da));
	knl_memset(&db, 0, sizeof(db));
	(void)wm_update();
	(void)wm_frames(&a);
	(void)ts_disp_frames(&da);
	(void)ts_get_mono(&t0);
	for ( i = 0; i < PACE_FRAMES; i++ ) {
		rect(&r, ( i * 7 ) % 100, 0, ( i * 7 ) % 100 + 20, 20);
		(void)dp_fill_rect(gid, &r, 0x00102030U + (UW)i * 0x010101U);
		(void)wm_update();
	}
	(void)ts_get_mono(&t1);
	(void)wm_frames(&b);
	(void)ts_disp_frames(&db);
	o->t0 = t0;
	o->us = ( t1 - t0 ) / 1000;
	o->w.builds = b.builds - a.builds;
	o->w.apart = b.apart - a.apart;
	o->w.apart_us = b.apart_us - a.apart_us;
	o->w.late = b.late - a.late;
	o->w.paced_us = b.paced_us - a.paced_us;
	o->w.waits = b.waits - a.waits;
	o->d.flips = db.flips - da.flips;
	for ( i = 0; i < 5; i++ ) o->d.apart[i] = db.apart[i] - da.apart[i];
	o->d.waited = db.waited - da.waited;
	o->d.taken = db.taken - da.taken;
}

LOCAL void test_pace( void )
{
	T_DPRECT	o;
	PACE		p;
	INT		wid, gid;
	BOOL		was;
	ER		er;

	if ( !have_wm ) KT_SKIP("no windows");
	if ( knl_wm_compositor() <= 0 ) KT_SKIP("no compositor");
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	was = spec.flipping;
	if ( was ) {
		KT_ASSERT_ER(ts_disp_flip(FALSE), E_OK);
	}
	rect(&o, 200, 200, 360, 300);
	wid = wm_open(&o, 0, "pace");
	KT_ASSERT(wid >= 1);
	if ( wid < 1 ) return;
	gid = wm_gid(wid);

	/* sends to the page shown: the compositor keeps the pace */
	KT_ASSERT_EQ(ts_disp_paced(), 0U);
	pace_run(gid, &p);
	tm_printf((UB *)"  %d frames put, the compositor keeping the pace: %ld us each, %ld builds "
		  "%ld us apart, %ld late, asleep %ld us each\n", PACE_FRAMES, (D)( p.us / PACE_FRAMES ),
		  (D)p.w.builds, (D)( p.w.apart_us / ( ( p.w.apart != 0 ) ? p.w.apart : 1 ) ), (D)p.w.late,
		  (D)( p.w.paced_us / ( ( p.w.builds != 0 ) ? p.w.builds : 1 ) ));
	KT_ASSERT(p.w.builds >= PACE_FRAMES);
	KT_ASSERT(p.w.waits >= PACE_FRAMES);
	KT_ASSERT(p.us >= (UD)( PACE_FRAMES - 1 ) * 16667U);
	KT_ASSERT(p.us <= (UD)PACE_FRAMES * 16667U + 30000U);

	/* on a screen that flips, a refresh made up for it: the sends keep it */
	er = ts_disp_flip(TRUE);
	if ( er == E_OK ) {
		UD	t_sim;

		KT_ASSERT_ER(knl_disp_sim_blank(16667), E_OK);
		(void)ts_get_mono(&t_sim);	/* the made-up refreshes are counted from about here */
		KT_ASSERT_EQ(ts_disp_paced(), 16667U);
		pace_run(gid, &p);
		tm_printf((UB *)"  %d frames put, the sends keeping the pace: %ld us each, %ld flips, refreshes "
			  "apart 0:%ld 1:%ld 2:%ld 3+:%ld, %ld waited for the blank, %ld taken by the clock, "
			  "the compositor asleep %ld us\n", PACE_FRAMES, (D)( p.us / PACE_FRAMES ), (D)p.d.flips,
			  (D)p.d.apart[0], (D)p.d.apart[1], (D)p.d.apart[2], (D)( p.d.apart[3] + p.d.apart[4] ),
			  (D)p.d.waited, (D)p.d.taken, (D)p.w.paced_us);
		/*
		 * The first put finds no flip in hand and is not held; each
		 * after it waits for the refresh that takes the one before.
		 * The last returns just after the 59th refresh from the first
		 * flip, and the run began somewhere within the refresh before
		 * that flip's: at the least 58 refreshes, 59 only when the run
		 * began right at a refresh.
		 */
		tm_printf((UB *)"  the run began %ld us into a refresh\n",
			  (D)( ( ( p.t0 > t_sim ) ? p.t0 - t_sim : 0 ) / 1000 % 16667U ));
		KT_ASSERT(p.d.flips >= PACE_FRAMES);
		KT_ASSERT_EQ(p.d.apart[0], 0);
		KT_ASSERT(p.d.apart[1] >= PACE_FRAMES - 4);
		KT_ASSERT_EQ(p.w.paced_us, 0);
		KT_ASSERT(p.us >= (UD)( PACE_FRAMES - 2 ) * 16667U);
		KT_ASSERT(p.us <= (UD)PACE_FRAMES * 16667U + 30000U);
		KT_ASSERT_ER(knl_disp_sim_blank(0), E_OK);
		if ( !was ) {
			KT_ASSERT_ER(ts_disp_flip(FALSE), E_OK);
		}
	} else {
		tm_printf((UB *)"  the screen has one page: the pace the sends keep not tried\n");
		if ( was ) {
			KT_ASSERT_ER(ts_disp_flip(TRUE), E_OK);
		}
	}
	KT_ASSERT_ER(wm_close(wid), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
}

/*
 * The screen set to another size with windows open (システム環境設定
 * while the desktop runs): every window keeps what it held and can be
 * drawn in, one as large as the screen above the message line grows and
 * shrinks with it, one that would be left off the screen is brought
 * back onto it, and the surfaces stay large enough at the new row length.
 */
LOCAL void test_screen_size( void )
{
	T_DISPSPEC	s0, s1;
	T_DPRECT	o, r;
	T_WMWIN		w;
	INT		a, full, edge, gid, mh, big = 0, pl = 0, resz = 0;
	UINT		w0, h0;
	ER		er;

	if ( !have_wm ) KT_SKIP("no windows");
	KT_ASSERT_ER(ts_disp_ref(&s0), E_OK);
	w0 = s0.width;
	h0 = s0.height;
	mh = wm_msg_height();
	/*
	 * Windows larger than 800 x 600 where the screen has room for them:
	 * one opened, one a program placed (wm_place), and one the person
	 * gives another size while the screen is small
	 */
	if ( w0 >= 900 && h0 >= (UINT)( 700 + mh ) ) {
		rect(&o, 40, 30, 890, 680);
		big = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "big");
		rect(&o, 20, 20, 120, 120);
		pl = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "placed");
		rect(&o, 60, 40, 880, 660);
		if ( pl > 0 ) KT_ASSERT_ER(wm_place(pl, &o), E_OK);
		rect(&o, 80, 60, 900, 690);
		resz = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "resized");
	}

	rect(&o, 100, 80, 500, 380);
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "a");
	rect(&o, 0, 0, (INT)w0, (INT)h0 - mh);
	full = wm_open(&o, 0, "full");
	rect(&o, (INT)w0 - 300, (INT)h0 - mh - 200, (INT)w0 - 20, (INT)h0 - mh - 20);
	edge = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "edge");
	KT_ASSERT(a > 0 && full > 0 && edge > 0);
	if ( a <= 0 || full <= 0 || edge <= 0 ) goto done;
	KT_ASSERT_ER(wm_raise(a), E_OK);
	gid = wm_gid(a);
	rect(&r, 0, 0, 40, 30);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00C02020), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);

	er = ts_disp_setmode(800, 600);
	if ( er == E_NOSPT ) {
		tm_printf((UB *)"  the screen keeps its size here\n");
		goto done;
	}
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
	spec = s1;
	KT_ASSERT(s1.width == 800 && s1.height == 600);
	KT_ASSERT_EQ(wm_self_check(), 0);

	/* what it held came across, and it is drawn in as before */
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT(w.outer.left == 100 && w.outer.top == 80);
	KT_ASSERT_EQ(at(w.work.left + 5, w.work.top + 5), 0xC02020);
	rect(&r, 0, 0, 40, 30);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x0020C020), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(w.work.left + 5, w.work.top + 5), 0x20C020);

	/* the whole screen's window follows the screen; the one near the edge is on it */
	KT_ASSERT_ER(wm_ref(full, &w), E_OK);
	KT_ASSERT(w.outer.left == 0 && w.outer.top == 0 && w.outer.right == 800
		  && w.outer.bottom == 600 - wm_msg_height());
	KT_ASSERT_ER(wm_ref(edge, &w), E_OK);
	KT_ASSERT(w.outer.left >= 0 && w.outer.right <= 800 && w.outer.bottom <= 600 - wm_msg_height());
	KT_ASSERT_EQ(w.outer.right - w.outer.left, 280);

	/* the large ones made smaller, on the screen; one is given a size, another only moved */
	if ( big > 0 && pl > 0 && resz > 0 ) {
		KT_ASSERT_ER(wm_ref(big, &w), E_OK);
		KT_ASSERT(w.outer.left >= 0 && w.outer.top >= 0 && w.outer.right <= 800
			  && w.outer.bottom <= 600 - mh);
		KT_ASSERT_ER(wm_ref(pl, &w), E_OK);
		KT_ASSERT(w.outer.right <= 800 && w.outer.bottom <= 600 - mh);
		/* a program asking for more than the screen has is kept on it */
		rect(&o, 60, 40, 880, 660);
		KT_ASSERT_ER(wm_place(pl, &o), E_OK);
		KT_ASSERT_ER(wm_ref(pl, &w), E_OK);
		KT_ASSERT(w.outer.left >= 0 && w.outer.top >= 0 && w.outer.right <= 800
			  && w.outer.bottom <= 600 - mh);
		KT_ASSERT_ER(wm_ref(big, &w), E_OK);
		o = w.outer;
		o.left += 10;  o.right += 10;
		o.top += 5;    o.bottom += 5;
		if ( o.right > 800 ) {
			o.left -= 20;  o.right -= 20;
		}
		KT_ASSERT_ER(wm_move(big, &o), E_OK);
		rect(&o, 30, 20, 530, 420);
		KT_ASSERT_ER(wm_move(resz, &o), E_OK);
	}

	/* and back to the size it had */
	KT_ASSERT_ER(ts_disp_setmode(w0, h0), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
	spec = s1;
	KT_ASSERT(s1.width == w0 && s1.height == h0);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT_EQ(at(w.work.left + 5, w.work.top + 5), 0x20C020);
	KT_ASSERT_ER(wm_ref(full, &w), E_OK);
	KT_ASSERT(w.outer.right == (INT)w0 && w.outer.bottom == (INT)h0 - wm_msg_height());

	/* each as it was asked for: the size given back, where it was asked or moved to */
	KT_ASSERT_ER(wm_ref(edge, &w), E_OK);
	KT_ASSERT(w.outer.left == (INT)w0 - 300 && w.outer.top == (INT)h0 - mh - 200
		  && w.outer.right == (INT)w0 - 20 && w.outer.bottom == (INT)h0 - mh - 20);
	if ( big > 0 && pl > 0 && resz > 0 ) {
		KT_ASSERT_ER(wm_ref(big, &w), E_OK);
		KT_ASSERT_EQ(w.outer.right - w.outer.left, 850);
		KT_ASSERT_EQ(w.outer.bottom - w.outer.top, 650);
		KT_ASSERT_ER(wm_ref(pl, &w), E_OK);
		KT_ASSERT(w.outer.left == 60 && w.outer.top == 40 && w.outer.right == 880 && w.outer.bottom == 660);
		KT_ASSERT_ER(wm_ref(resz, &w), E_OK);
		KT_ASSERT(w.outer.left == 30 && w.outer.top == 20 && w.outer.right == 530 && w.outer.bottom == 420);
		KT_ASSERT_ER(wm_ref(big, &w), E_OK);
		tm_printf((UB *)"  given back: big %d x %d at %d,%d\n", w.outer.right - w.outer.left,
			  w.outer.bottom - w.outer.top, w.outer.left, w.outer.top);
	}
done:
	if ( a > 0 ) wm_close(a);
	if ( full > 0 ) wm_close(full);
	if ( edge > 0 ) wm_close(edge);
	if ( big > 0 ) wm_close(big);
	if ( pl > 0 ) wm_close(pl);
	if ( resz > 0 ) wm_close(resz);
	(void)ts_disp_ref(&spec);
}

/*
 * The screen drawn at a scale (ts_disp_setscale, design 10.2.3): the
 * windows are laid in a view of the screen's pixels times 100 over the
 * scale, a window drawn in at view coordinates is drawn larger in the
 * screen's pixels, letters are made at the larger size and measured as
 * before, an icon larger than its box shows all of its pixels, what a
 * panel covers comes back as it was, and back at 100 the windows are as
 * they were. A scale that would leave too small a view, or one not of
 * the list, is refused.
 */
LOCAL INT dvs( INT v, UINT sc )
{
	return disp_dev_of(v, sc);
}

/* How many pixels of a rectangle of the screen are neither of two colours */
LOCAL INT not_of( INT l, INT t, INT r, INT b, UW c1, UW c2 )
{
	INT	x, y, n = 0;

	for ( y = t; y < b; y++ ) {
		for ( x = l; x < r; x++ ) {
			UW	c = at(x, y);

			if ( c != c1 && c != c2 ) n++;
		}
	}
	return n;
}

/* Whether every two by two block of a rectangle of the screen is of one colour */
LOCAL BOOL blocks_whole( INT l, INT t, INT r, INT b )
{
	INT	x, y;

	for ( y = t; y + 1 < b; y += 2 ) {
		for ( x = l; x + 1 < r; x += 2 ) {
			UW	c = at(x, y);

			if ( at(x + 1, y) != c || at(x, y + 1) != c || at(x + 1, y + 1) != c ) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

LOCAL void test_scale( void )
{
	T_DISPSPEC	s0, s1;
	T_DPRECT	o, r;
	T_WMWIN		w;
	INT		a, gid, wid, n100, n150, vw, vh, X, Y;
	UINT		w0, h0;
	ID		fid = fn_system();
	UW		*icon = NULL, *saved = NULL;
	BOOL		big = FALSE;
	ER		er;

	if ( !have_wm ) KT_SKIP("no windows");
	KT_ASSERT_ER(ts_disp_ref(&s0), E_OK);
	KT_ASSERT_EQ(s0.scale, 100);
	KT_ASSERT(s0.view_w == s0.width && s0.view_h == s0.height);
	w0 = s0.width;
	h0 = s0.height;

	/* not of the list, or a view too small */
	KT_ASSERT_ER(ts_disp_setscale(133), E_PAR);
	if ( disp_view_len(w0, 300) < DISP_VIEW_MIN_W || disp_view_len(h0, 300) < DISP_VIEW_MIN_H ) {
		KT_ASSERT_ER(ts_disp_setscale(300), E_NOSPT);
	}

	rect(&o, 100, 80, 500, 380);
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "scale");
	KT_ASSERT(a > 0);
	if ( a <= 0 ) goto done;
	gid = wm_gid(a);
	KT_ASSERT_EQ(dp_scale(gid), 100);
	if ( fid > 0 ) {
		fn_set_size(fid, 16);
		n100 = fn_width(fid, (CONST UB *)"Scale 拡大の試験");
	} else {
		n100 = 0;
	}

	/* at 150%: the view, the windows' place, a rectangle in the screen's pixels */
	KT_ASSERT_ER(ts_disp_setscale(150), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
	spec = s1;
	KT_ASSERT_EQ(s1.scale, 150);
	KT_ASSERT_EQ(s1.view_w, disp_view_len(w0, 150));
	KT_ASSERT_EQ(s1.view_h, disp_view_len(h0, 150));
	KT_ASSERT(s1.width == w0 && s1.height == h0);
	wm_screen(&vw, &vh);
	KT_ASSERT(vw == (INT)s1.view_w && vh == (INT)s1.view_h);
	KT_ASSERT_EQ(wm_get(LK_SCALE), 150);
	KT_ASSERT_EQ(dp_scale(gid), 150);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT(w.outer.left == 100 && w.outer.top == 80 && w.outer.right == 500);
	rect(&r, 0, 0, 40, 30);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00C02020), E_OK);
	rect(&r, 40, 0, 400, 300);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FFFFFF), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	X = dvs(w.outer.left, 150) + dvs(w.work.left - w.outer.left, 150);
	Y = dvs(w.outer.top, 150) + dvs(w.work.top - w.outer.top, 150);
	KT_ASSERT_EQ(at(X, Y), 0xC02020);
	KT_ASSERT_EQ(at(X + dvs(40, 150) - 1, Y + dvs(30, 150) - 1), 0xC02020);
	KT_ASSERT_EQ(at(X + dvs(40, 150), Y + 5), 0xFFFFFF);
	KT_ASSERT_EQ(at(X + 5, Y + dvs(30, 150)), 0xFFFFFF);

	/* letters: measured as at 100, drawn with more pixels */
	if ( fid > 0 ) {
		fn_set_size(fid, 16);
		KT_ASSERT_EQ(fn_width(fid, (CONST UB *)"Scale 拡大の試験"), n100);
		KT_ASSERT_EQ(fn_draw(gid, fid, 4, 60, (CONST UB *)"Scale 拡大の試験", 0), n100);
		KT_ASSERT_ER(wm_composite(), E_OK);
		n150 = not_of(X, Y + dvs(40, 150), X + dvs(4 + n100 + 4, 150), Y + dvs(66, 150),
			      0xFFFFFF, 0xFFFFFF);
		tm_printf((UB *)"  150%%: a line of %d view pixels, %d pixels of letters\n", n100, n150);
		KT_ASSERT(n150 > 0);
	}

	/* a window opened now is drawn at the scale too, and fits the view */
	rect(&o, 0, 0, vw, vh - wm_msg_height());
	wid = wm_open(&o, 0, "full");
	KT_ASSERT(wid > 0);
	if ( wid > 0 ) {
		KT_ASSERT_EQ(dp_scale(wm_gid(wid)), 150);
		KT_ASSERT_EQ(wm_self_check(), 0);
		KT_ASSERT_ER(wm_close(wid), E_OK);
	}
	KT_ASSERT_ER(wm_raise(a), E_OK);

	/* at 200% where the screen can be made large enough for it */
	er = ts_disp_setmode(1600, 1200);
	if ( er >= E_OK ) {
		big = TRUE;
		KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
		spec = s1;
		KT_ASSERT_EQ(s1.scale, 150);
		KT_ASSERT_ER(ts_disp_setscale(200), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
		spec = s1;
		KT_ASSERT(s1.scale == 200 && s1.view_w == 800 && s1.view_h == 600);
		KT_ASSERT_EQ(wm_self_check(), 0);
		KT_ASSERT_ER(wm_ref(a, &w), E_OK);
		X = 2 * w.work.left;
		Y = 2 * w.work.top;
		rect(&r, 0, 0, 400, 300);
		KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FFFFFF), E_OK);
		rect(&r, 10, 10, 11, 20);
		KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00000000), E_OK);
		KT_ASSERT_ER(wm_composite(), E_OK);
		/* a line one view pixel wide is two of the screen's */
		KT_ASSERT(at(X + 20, Y + 30) == 0 && at(X + 21, Y + 30) == 0);
		KT_ASSERT(at(X + 19, Y + 30) == 0xFFFFFF && at(X + 22, Y + 30) == 0xFFFFFF);
		if ( fid > 0 ) {
			fn_set_size(fid, 16);
			(void)fn_draw(gid, fid, 4, 60, (CONST UB *)"Scale 拡大の試験", 0);
			KT_ASSERT_ER(wm_composite(), E_OK);
			/* made at 32 pixels, not 16 made larger: the blocks are not whole */
			KT_ASSERT(!blocks_whole(X + 8, Y + 80, X + 2 * ( 4 + n100 ), Y + 124));
		}
		/* an icon of 32 in a box of 16: every pixel of it */
		icon = (UW *)Kmalloc(32 * 32 * sizeof(UW));
		if ( icon != NULL ) {
			INT	i, bad = 0;

			for ( i = 0; i < 32 * 32; i++ ) {
				icon[i] = ( ( ( i & 31 ) ^ ( i >> 5 ) ) & 1 ) ? 0x00102030 : 0x00F0E0D0;
			}
			rect(&r, 200, 100, 216, 116);
			KT_ASSERT_ER(dp_put_image(gid, &r, icon, 32, 32, 32, 0xFF000000U), E_OK);
			KT_ASSERT_ER(wm_composite(), E_OK);
			for ( i = 0; i < 32 * 32; i++ ) {
				if ( at(X + 400 + ( i & 31 ), Y + 200 + ( i >> 5 )) != ( icon[i] & 0xFFFFFF ) ) bad++;
			}
			KT_ASSERT_EQ(bad, 0);
		}
		/* what a panel covers, taken in the screen's pixels and put back as it was */
		{
			INT	pw = 0, ph = 0, before;

			rect(&r, 0, 50, 120, 70);
			before = not_of(X, Y + 100, X + 240, Y + 140, 0xFFFFFF, 0xFFFFFF);
			KT_ASSERT_ER(dp_get_dev(gid, &r, &saved, &pw, &ph), E_OK);
			KT_ASSERT(pw == 240 && ph == 40);
			KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00808080), E_OK);
			KT_ASSERT_ER(dp_put_dev(gid, &r, saved, pw, ph), E_OK);
			KT_ASSERT_ER(wm_composite(), E_OK);
			KT_ASSERT_EQ(not_of(X, Y + 100, X + 240, Y + 140, 0xFFFFFF, 0xFFFFFF), before);
		}
	} else {
		tm_printf((UB *)"  the screen keeps its size here: 200%% not tried\n");
	}

	/* and back to 100 at the size it had: drawn as it always was */
	KT_ASSERT_ER(ts_disp_setscale(100), E_OK);
	if ( big ) {
		KT_ASSERT_ER(ts_disp_setmode(w0, h0), E_OK);
	}
	KT_ASSERT_ER(ts_disp_ref(&s1), E_OK);
	spec = s1;
	KT_ASSERT(s1.scale == 100 && s1.width == w0 && s1.view_w == w0 && s1.view_h == h0);
	KT_ASSERT_EQ(wm_get(LK_SCALE), 100);
	KT_ASSERT_EQ(dp_scale(gid), 100);
	KT_ASSERT_EQ(wm_self_check(), 0);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	rect(&r, 0, 0, 40, 30);
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x0020C020), E_OK);
	KT_ASSERT_ER(wm_composite(), E_OK);
	KT_ASSERT_EQ(at(w.work.left, w.work.top), 0x20C020);
	KT_ASSERT_EQ(at(w.work.left + 39, w.work.top + 29), 0x20C020);
	KT_ASSERT(at(w.work.left + 40, w.work.top + 5) != 0x20C020);
done:
	if ( icon != NULL ) Kfree(icon);
	if ( saved != NULL ) Kfree(saved);
	if ( a > 0 ) wm_close(a);
	(void)ts_disp_setscale(100);
	(void)ts_disp_ref(&spec);
}

EXPORT void ktest_wm( void )
{
	KT_RUN(test_open);
	KT_RUN(test_relayout);
	KT_RUN(test_convert);
	KT_RUN(test_draw);
	KT_RUN(test_order);
	KT_RUN(test_float);
	KT_RUN(test_subs_away);
	KT_RUN(test_skip);
	KT_RUN(test_damage);
	KT_RUN(test_many);
	KT_RUN(test_beyond);
	KT_RUN(test_events);
	KT_RUN(test_volume_keys);
	KT_RUN(test_frame);
	KT_RUN(test_bars);
	KT_RUN(test_grab);
	KT_RUN(test_title_jp);
	KT_RUN(test_scheme);
	KT_RUN(test_seethrough);
	KT_RUN(test_look_settings);
	KT_RUN(test_look_tone);
	KT_RUN(test_look_numbers);
	KT_RUN(test_wall);
	KT_RUN(test_look_opacity);
	KT_RUN(test_look_wash);
	KT_RUN(test_look_round);
	KT_RUN(test_look_shadow);
	KT_RUN(test_look_blur);
	KT_RUN(test_look_motion);
	KT_RUN(test_look_cost);
	KT_RUN(test_look_kept);
	KT_RUN(test_parts);
	KT_RUN(test_pointer_save);
	KT_RUN(test_parts_spread);
	KT_RUN(test_coalesce);
	KT_RUN(test_simd_same);
	KT_RUN(test_simd_time);
	KT_RUN(test_shadow_same);
	KT_RUN(test_hide_same);
	KT_RUN(test_hide_time);
	KT_RUN(test_pace);
	KT_RUN(test_screen_size);
	KT_RUN(test_scale);
}
