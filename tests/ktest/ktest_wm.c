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

EXPORT void ktest_wm( void )
{
	KT_RUN(test_open);
	KT_RUN(test_relayout);
	KT_RUN(test_convert);
	KT_RUN(test_draw);
	KT_RUN(test_order);
	KT_RUN(test_skip);
	KT_RUN(test_damage);
	KT_RUN(test_many);
	KT_RUN(test_events);
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
}
