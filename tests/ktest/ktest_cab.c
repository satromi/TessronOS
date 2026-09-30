/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_cab.c
 *	The cabinet (design 17.5.1, phase 13)
 *
 *	A cabinet is made on the volume as an xmlTAD <figure> with three
 *	<link> elements in it, opened into a window, and worked on the way
 *	a person would: pick something, move it, take the move back, save.
 *
 *	Two properties are what the tests are really for. Reading and
 *	drawing a cabinet must not change a single byte of the record, nor
 *	write to any other real object. And a save must leave the links
 *	that did not move exactly as they were.
 *
 *	The test task has a small stack, so anything larger than a few
 *	words comes from the heap.
 */

#include "ktest.h"
#include <ts/tsfs.h>
#include <ts/tsfsobj.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/cab.h>
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/hid.h>
#include <ts/uuid.h>

#define CABDEV		kt_scratch()
#define NLINK		3

LOCAL ID	vol = 0;			/* above 0 once the volume is attached */
LOCAL TS_UUID	cabinet;
LOCAL TS_UUID	target;
LOCAL INT	wid = 0;
LOCAL BOOL	ready = FALSE;

/* The bytes of the record as it stands, so that a change can be seen */
LOCAL ER doc_bytes( UB **p_buf, SZ *p_len )
{
	T_TAD	*doc = NULL;
	UB	*buf;
	SZ	want = 0;
	ER	er;

	er = om_rea_doc(&cabinet, 0, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	er = tad_write_mem(doc, NULL, 0, &want);
	if ( er < E_OK && er != E_NOMEM ) {
		tad_free(doc);
		return er;
	}
	buf = (UB *)Kmalloc(want + 1);
	if ( buf == NULL ) {
		tad_free(doc);
		return E_NOMEM;
	}
	er = tad_write_mem(doc, buf, want + 1, &want);
	tad_free(doc);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	*p_buf = buf;
	*p_len = want;

	return E_OK;
}

/* How many links the record holds just now */
LOCAL INT links_now( void )
{
	T_TAD	*doc = NULL;
	INT	n = -1;

	if ( om_rea_doc(&cabinet, 0, NULL, &doc) >= E_OK && doc != NULL ) {
		n = tad_lnk_count(doc);
		tad_free(doc);
	}

	return n;
}

/* a cabinet with three links in it, and a window to show it in */
LOCAL void test_make( void )
{
	T_TAD		*doc = NULL;
	T_VOBJ		*v;
	T_DPRECT	o;
	T_DISPSPEC	spec;
	INT		i;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	if ( CABDEV == NULL ) {
		KT_SKIP(KT_NO_SCRATCH);
	}
	vol =( ob_att_vol(CABDEV, TSFS_STORE_BLK) >= E_OK ) ? 1 : 0;
	if ( vol <= 0 ) {
		if ( ts_format_blk(CABDEV, "CABINET") < E_OK ) {
			KT_SKIP("no block device to make a volume on");
		}
		vol = ( ob_att_vol(CABDEV, TSFS_STORE_BLK) >= E_OK ) ? 1 : 0;
	}
	if ( vol <= 0 ) {
		KT_SKIP("the volume would not open");
	}
	KT_ASSERT_ER(om_cre_obj(CABDEV, (CONST UB *)"tsukue", OM_BODY_FIG,
				&cabinet), E_OK);
	KT_ASSERT_ER(om_cre_obj(CABDEV, (CONST UB *)"nakami", OM_BODY_DOC,
				&target), E_OK);

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;

	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }

	for ( i = 0; i < NLINK; i++ ) {
		INT	k;

		for ( k = 0; k < (INT)sizeof(T_VOBJ); k++ ) ((UB *)v)[k] = 0;
		v->target = target;
		v->left   = 10 + i * 130;
		v->top    = 20;
		v->right  = v->left + 120;
		v->bottom = v->top + 40;
		v->height = 40;
		v->chsz   = 14;
		v->disp   = TAD_D_DEFAULT;
		v->zoom   = 100;
		v->frcol  = 0x00000000U;
		v->bgcol  = 0x00FFFFFFU;
		v->tbcol  = 0x00E1F2F9U;
		KT_ASSERT_ER(tad_lnk_add(doc, v, NULL), E_OK);
	}
	KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
	tad_free(doc);
	Kfree(v);

	/* the links raised the count of what they point at */
	{
		T_OBREF	robj;

		KT_ASSERT_ER(ob_ref_obj(&target, &robj), E_OK);
		KT_ASSERT_EQ(robj.refcnt, NLINK);
	}

	o.left = 40;  o.top = 40;  o.right = 600;  o.bottom = 400;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "tsukue");
	if ( wid < 0 ) {
		KT_SKIP("windows did not start");
	}
	ready = TRUE;
}

/* opening reads the links and puts every one of them in the register */
LOCAL void test_open( void )
{
	T_CABENT	e;
	INT		i;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_open(&cabinet, 0, wid), E_OK);
	KT_ASSERT_EQ(cab_count(), NLINK);
	KT_ASSERT_EQ(cab_picked(), 0);
	KT_ASSERT_EQ(cab_self_check(), 0);

	for ( i = 0; i < NLINK; i++ ) {
		KT_ASSERT_ER(cab_ref(i, &e), E_OK);
		KT_ASSERT_EQ(e.r.left, 10 + i * 130);
		KT_ASSERT_EQ(e.r.top, 20);
		KT_ASSERT_EQ(e.pick, -1);
		KT_ASSERT(e.vid > 0);
	}
	/* a second cabinet in the same program is refused */
	KT_ASSERT_ER(cab_open(&cabinet, 0, wid), E_OBJ);
}

/* a place inside an entry finds it, a place outside finds nothing */
LOCAL void test_at( void )
{
	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_EQ(cab_at(15, 25), 0);
	KT_ASSERT_EQ(cab_at(145, 25), 1);
	KT_ASSERT_EQ(cab_at(275, 25), 2);
	KT_ASSERT_EQ(cab_at(15, 300), -1);	/* below them all */
	KT_ASSERT_EQ(cab_at(131, 25), -1);	/* in the gap between two */
}

/* picking carries an order, and dropping one closes the gap it left */
LOCAL void test_pick( void )
{
	T_CABENT	e;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick(2, CAB_SEL_ONE), E_OK);
	KT_ASSERT_EQ(cab_picked(), 1);
	KT_ASSERT_ER(cab_ref(2, &e), E_OK);
	KT_ASSERT_EQ(e.pick, 0);

	KT_ASSERT_ER(cab_pick(0, CAB_SEL_ADD), E_OK);
	KT_ASSERT_ER(cab_pick(1, CAB_SEL_ADD), E_OK);
	KT_ASSERT_EQ(cab_picked(), 3);
	KT_ASSERT_ER(cab_ref(0, &e), E_OK);
	KT_ASSERT_EQ(e.pick, 1);
	KT_ASSERT_ER(cab_ref(1, &e), E_OK);
	KT_ASSERT_EQ(e.pick, 2);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* taking the middle one out leaves 0 and 1, not 0 and 2 */
	KT_ASSERT_ER(cab_pick(0, CAB_SEL_TOGGLE), E_OK);
	KT_ASSERT_EQ(cab_picked(), 2);
	KT_ASSERT_ER(cab_ref(2, &e), E_OK);
	KT_ASSERT_EQ(e.pick, 0);
	KT_ASSERT_ER(cab_ref(1, &e), E_OK);
	KT_ASSERT_EQ(e.pick, 1);
	KT_ASSERT_ER(cab_ref(0, &e), E_OK);
	KT_ASSERT_EQ(e.pick, -1);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* all of them, then none */
	KT_ASSERT_ER(cab_pick_all(), E_OK);
	KT_ASSERT_EQ(cab_picked(), NLINK);
	KT_ASSERT_EQ(cab_self_check(), 0);
	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_EQ(cab_picked(), 0);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* moving what is picked moves that and nothing else */
LOCAL void test_move( void )
{
	T_CABENT	a, b;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick(1, CAB_SEL_ONE), E_OK);
	KT_ASSERT_ER(cab_ref(1, &a), E_OK);
	KT_ASSERT_ER(cab_ref(0, &b), E_OK);

	KT_ASSERT_ER(cab_move(30, 45), E_OK);
	{
		T_CABENT	c, d;

		KT_ASSERT_ER(cab_ref(1, &c), E_OK);
		KT_ASSERT_EQ(c.r.left, a.r.left + 30);
		KT_ASSERT_EQ(c.r.top, a.r.top + 45);
		KT_ASSERT_EQ(c.r.right, a.r.right + 30);

		KT_ASSERT_ER(cab_ref(0, &d), E_OK);
		KT_ASSERT_EQ(d.r.left, b.r.left);	/* not picked, not moved */
	}
	KT_ASSERT_EQ(cab_self_check(), 0);
	KT_ASSERT_EQ(cab_undo_depth(), 1);
}

/* any number of moves can be taken back, and put back again */
LOCAL void test_undo( void )
{
	T_CABENT	was, now;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_ref(1, &was), E_OK);
	KT_ASSERT_ER(cab_move(10, 0), E_OK);
	KT_ASSERT_ER(cab_move(0, 10), E_OK);
	KT_ASSERT_EQ(cab_undo_depth(), 3);

	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(1, &now), E_OK);
	KT_ASSERT_EQ(now.r.left, was.r.left);
	KT_ASSERT_EQ(now.r.top, was.r.top);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* and forward again */
	KT_ASSERT_ER(cab_redo(), E_OK);
	KT_ASSERT_ER(cab_ref(1, &now), E_OK);
	KT_ASSERT_EQ(now.r.left, was.r.left + 10);
	KT_ASSERT_ER(cab_redo(), E_OK);
	KT_ASSERT_ER(cab_ref(1, &now), E_OK);
	KT_ASSERT_EQ(now.r.top, was.r.top + 10);
	KT_ASSERT_ER(cab_redo(), E_NOEXS);	/* there is no more forward */

	/* back to where the first move left it */
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* drawing a cabinet changes the screen and not the record */
LOCAL void test_draw_writes_nothing( void )
{
	UB	*before = NULL, *after = NULL;
	SZ	nb = 0, na = 0;
	SZ	i;
	T_CABSTAT st;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(doc_bytes(&before, &nb), E_OK);
	if ( before == NULL ) return;

	KT_ASSERT_ER(cab_draw(TRUE), E_OK);
	KT_ASSERT_ER(cab_draw(TRUE), E_OK);
	KT_ASSERT_ER(cab_stat(&st), E_OK);
	KT_ASSERT(st.entries_drawn >= (UD)(2 * NLINK));

	KT_ASSERT_ER(doc_bytes(&after, &na), E_OK);
	if ( after == NULL ) { Kfree(before); return; }

	KT_ASSERT_EQ((INT)na, (INT)nb);
	for ( i = 0; i < nb && i < na; i++ ) {
		if ( before[i] != after[i] ) {
			KT_ASSERT_EQ((INT)i, -1);	/* says where it differs */
			break;
		}
	}
	Kfree(before);
	Kfree(after);
}

/* saving writes the moved link and leaves the others byte for byte */
LOCAL void test_save( void )
{
	UB		*before = NULL, *after = NULL;
	SZ		nb = 0, na = 0;
	T_TAD		*doc = NULL;
	T_VOBJ		*v;
	T_CABENT	e;
	INT		i, k, diff;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(doc_bytes(&before, &nb), E_OK);
	KT_ASSERT_ER(cab_save(), E_OK);
	KT_ASSERT_ER(doc_bytes(&after, &na), E_OK);
	if ( before == NULL || after == NULL ) {
		if ( before != NULL ) Kfree(before);
		if ( after != NULL ) Kfree(after);
		return;
	}
	/* something changed: entry 1 was moved */
	diff = 0;
	if ( na != nb ) {
		diff = 1;
	} else {
		for ( i = 0; i < (INT)nb; i++ ) {
			if ( before[i] != after[i] ) { diff = 1; break; }
		}
	}
	KT_ASSERT_EQ(diff, 1);
	Kfree(before);
	Kfree(after);

	/* and what the record now says is where the entry now is */
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	KT_ASSERT_ER(cab_ref(1, &e), E_OK);
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_EQ(tad_lnk_count(doc), NLINK);
		for ( k = 0; k < (INT)sizeof(T_VOBJ); k++ ) ((UB *)v)[k] = 0;
		KT_ASSERT_ER(tad_lnk_find(doc, &e.vobjid, v), E_OK);
		KT_ASSERT_EQ(v->left, e.r.left);
		KT_ASSERT_EQ(v->top, e.r.top);
		KT_ASSERT_EQ(v->right, e.r.right);
		KT_ASSERT_EQ(v->bottom, e.r.bottom);
		tad_free(doc);
	}
	Kfree(v);

	/* saving again with nothing moved leaves the bytes alone */
	KT_ASSERT_ER(doc_bytes(&before, &nb), E_OK);
	KT_ASSERT_ER(cab_save(), E_OK);
	KT_ASSERT_ER(doc_bytes(&after, &na), E_OK);
	if ( before != NULL && after != NULL ) {
		KT_ASSERT_EQ((INT)na, (INT)nb);
		for ( i = 0; i < (INT)nb && i < (INT)na; i++ ) {
			if ( before[i] != after[i] ) {
				KT_ASSERT_EQ(i, -1);
				break;
			}
		}
	}
	if ( before != NULL ) Kfree(before);
	if ( after != NULL ) Kfree(after);
}

/* closing gives the register back and lets another cabinet open */
LOCAL void test_close( void )
{
	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_close(FALSE), E_OK);
	KT_ASSERT_EQ(cab_count(), 0);
	KT_ASSERT_ER(cab_close(FALSE), E_OBJ);

	/* what it registered is gone, so it can all be done again */
	{
		INT	n = links_now();

		KT_ASSERT_ER(cab_open(&cabinet, 0, wid), E_OK);
		KT_ASSERT_EQ(cab_count(), n);	/* what the record holds, no more */
	}
	KT_ASSERT_EQ(cab_self_check(), 0);
	KT_ASSERT_ER(cab_close(FALSE), E_OK);

	wm_close(wid);
	wid = 0;
	ready = FALSE;
}


/* Make an event as the window manager would hand one over */
LOCAL void ev_make( T_WMEV *ev, UINT type, INT x, INT y, UD when )
{
	ev->type = type;
	ev->code = 0;
	ev->mods = 0;
	ev->x    = x;
	ev->y    = y;
	ev->when = when;
	ev->wid  = wid;
}

/* pressing picks, dragging moves, and the whole drag is one operation */
LOCAL void test_drag( void )
{
	T_WMEV		ev;
	T_CABENT	was, now;
	INT		depth;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &was), E_OK);
	depth = cab_undo_depth();

	/* press inside the first entry */
	ev_make(&ev, HID_EV_BTN_DOWN, was.r.left + 5, was.r.top + 5, 1000000000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 1);
	KT_ASSERT_ER(cab_ref(0, &now), E_OK);
	KT_ASSERT_EQ(now.pick, 0);
	KT_ASSERT_EQ(cab_undo_depth(), depth);	/* a press is not an operation */

	/* three steps of a drag */
	ev_make(&ev, HID_EV_MOVE, was.r.left + 15, was.r.top + 5, 1000100000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	ev_make(&ev, HID_EV_MOVE, was.r.left + 25, was.r.top + 12, 1000200000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	ev_make(&ev, HID_EV_MOVE, was.r.left + 25, was.r.top + 20, 1000300000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_undo_depth(), depth);	/* still one operation in the making */

	ev_make(&ev, HID_EV_BTN_UP, was.r.left + 25, was.r.top + 20, 1000400000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_undo_depth(), depth + 1);

	KT_ASSERT_ER(cab_ref(0, &now), E_OK);
	KT_ASSERT_EQ(now.r.left, was.r.left + 20);
	KT_ASSERT_EQ(now.r.top, was.r.top + 15);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* and one undo puts the whole drag back */
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &now), E_OK);
	KT_ASSERT_EQ(now.r.left, was.r.left);
	KT_ASSERT_EQ(now.r.top, was.r.top);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* pressing bare work area lets everything go */
LOCAL void test_press_empty( void )
{
	T_WMEV	ev;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_all(), E_OK);
	KT_ASSERT(cab_picked() > 0);
	ev_make(&ev, HID_EV_BTN_DOWN, 5, 320, 2000000000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 0);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* a modifier held down adds to what is picked instead of replacing it */
LOCAL void test_pick_with_mods( void )
{
	T_WMEV		ev;
	T_CABENT	a, b;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &a), E_OK);
	KT_ASSERT_ER(cab_ref(2, &b), E_OK);

	ev_make(&ev, HID_EV_BTN_DOWN, a.r.left + 3, a.r.top + 3, 3000000000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	ev_make(&ev, HID_EV_BTN_UP, a.r.left + 3, a.r.top + 3, 3000010000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 1);

	ev_make(&ev, HID_EV_BTN_DOWN, b.r.left + 3, b.r.top + 3, 3100000000U);
	ev.mods = HID_MOD_LSHIFT;
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 2);
	KT_ASSERT_ER(cab_ref(2, &b), E_OK);
	KT_ASSERT_EQ(b.pick, 1);		/* it went on the end of the order */
	ev_make(&ev, HID_EV_BTN_UP, b.r.left + 3, b.r.top + 3, 3100010000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_self_check(), 0);
	KT_ASSERT_ER(cab_pick_none(), E_OK);
}

/* a key runs the command the table gives it */
LOCAL void test_keys( void )
{
	T_WMEV		ev;
	T_CABENT	was, now;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_none(), E_OK);
	ev_make(&ev, HID_EV_KEY_DOWN, 0, 0, 4000000000U);
	ev.code = 0x04;				/* A: pick all */
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), NLINK);

	KT_ASSERT_ER(cab_ref(0, &was), E_OK);
	ev.code = 0x4F;				/* right */
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_ER(cab_ref(0, &now), E_OK);
	KT_ASSERT(now.r.left > was.r.left);

	ev.code = 0x1D;				/* Z: undo */
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_ER(cab_ref(0, &now), E_OK);
	KT_ASSERT_EQ(now.r.left, was.r.left);

	ev.code = 0x29;				/* escape: pick nothing */
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 0);

	ev.code = 0x3A;				/* a key with no command does nothing */
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* an event for another window is not this cabinet's business */
LOCAL void test_other_window( void )
{
	T_WMEV	ev;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_all(), E_OK);
	ev_make(&ev, HID_EV_BTN_DOWN, 5, 320, 5000000000U);
	ev.wid = wid + 99;
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), NLINK);	/* untouched */
	KT_ASSERT_ER(cab_pick_none(), E_OK);
}

/* laying out puts entries in rows, in reading order, and can be taken back */
LOCAL void test_arrange( void )
{
	T_CABARR	a;
	T_CABENT	e0, e1, e2, was0;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &was0), E_OK);

	a.width = 300;			/* two of them fit in a row, not three */
	a.gap   = 10;
	KT_ASSERT_ER(cab_arrange(&a), E_OK);

	/*
	 * Which entry lands where depends on where they were, because the
	 * layout takes them in reading order. What holds whatever the
	 * order is: every entry starts a row at the gap or sits one gap
	 * after the one before it, two fit in a row of 300 and the third
	 * does not, and nothing changed size.
	 */
	{
		INT	i, first_row = 0, second_row = 0, top0 = -1;

		for ( i = 0; i < NLINK; i++ ) {
			T_CABENT	c;
			T_CABENT	orig;

			KT_ASSERT_ER(cab_ref(i, &c), E_OK);
			KT_ASSERT(c.r.left == 10 || c.r.left == 10 + 120 + 10);
			if ( top0 < 0 ) {
				top0 = 10;
			}
			if ( c.r.top == 10 ) {
				first_row++;
			} else {
				KT_ASSERT(c.r.top > 10);
				second_row++;
			}
			/* it kept its size */
			KT_ASSERT_ER(cab_ref(i, &orig), E_OK);
			KT_ASSERT_EQ(orig.r.right - orig.r.left, 120);
		}
		KT_ASSERT_EQ(first_row, 2);
		KT_ASSERT_EQ(second_row, 1);
	}
	KT_ASSERT_EQ(cab_self_check(), 0);

	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &e0), E_OK);
	KT_ASSERT_EQ(e0.r.left, was0.r.left);
	KT_ASSERT_EQ(e0.r.top, was0.r.top);
	KT_ASSERT_EQ(cab_self_check(), 0);
	(void)e1;  (void)e2;
}

/* taking an entry out hides it but leaves the record alone until a save */
LOCAL void test_delete( void )
{
	T_CABENT	e;
	UB		*before = NULL, *after = NULL;
	SZ		nb = 0, na = 0;
	INT		i, diff = 0;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(doc_bytes(&before, &nb), E_OK);
	KT_ASSERT_ER(cab_pick(2, CAB_SEL_ONE), E_OK);
	KT_ASSERT_ER(cab_delete(), E_OK);

	KT_ASSERT_ER(cab_ref(2, &e), E_OK);
	KT_ASSERT(e.gone);
	KT_ASSERT_EQ(cab_picked(), 0);		/* what was picked is not there now */
	KT_ASSERT_EQ(cab_at(e.r.left + 3, e.r.top + 3), -1);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* the record still holds all three */
	KT_ASSERT_ER(doc_bytes(&after, &na), E_OK);
	if ( before != NULL && after != NULL ) {
		KT_ASSERT_EQ((INT)na, (INT)nb);
		for ( i = 0; i < (INT)nb && i < (INT)na; i++ ) {
			if ( before[i] != after[i] ) { diff = 1; break; }
		}
		KT_ASSERT_EQ(diff, 0);
	}
	if ( before != NULL ) Kfree(before);
	if ( after != NULL ) Kfree(after);

	/* taking it back brings the entry into the cabinet again */
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(2, &e), E_OK);
	KT_ASSERT(!e.gone);
	KT_ASSERT_EQ(cab_at(e.r.left + 3, e.r.top + 3), 2);

	/* now do it for real and write it */
	KT_ASSERT_ER(cab_pick(2, CAB_SEL_ONE), E_OK);
	KT_ASSERT_ER(cab_delete(), E_OK);
	KT_ASSERT_ER(cab_save(), E_OK);
	KT_ASSERT_EQ(cab_undo_depth(), 0);	/* what was written cannot be undone */

	KT_ASSERT_EQ(links_now(), NLINK - 1);
	/* and the object those links pointed at lost one reference */
	{
		T_OBREF	robj;

		KT_ASSERT_ER(ob_ref_obj(&target, &robj), E_OK);
		KT_ASSERT_EQ(robj.refcnt, NLINK - 1);
	}
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* scrolling moves what the window shows and nothing that is stored */
LOCAL void test_scroll( void )
{
	T_CABENT	e;
	T_WMEV		ev;
	INT		x = 0, y = 0, cw = 0, ch = 0;
	UB		*before = NULL, *after = NULL;
	SZ		nb = 0, na = 0;
	INT		i, diff = 0;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_ER(cab_extent(&cw, &ch), E_OK);
	KT_ASSERT(cw > 0);
	KT_ASSERT(ch > 0);

	KT_ASSERT_ER(cab_ref(0, &e), E_OK);
	KT_ASSERT_ER(doc_bytes(&before, &nb), E_OK);

	/* the entry can be pressed where it is */
	KT_ASSERT_EQ(cab_at(e.r.left + 3, e.r.top + 3), 0);

	KT_ASSERT_ER(cab_scroll(30, 20), E_OK);
	KT_ASSERT_ER(cab_scroll_of(&x, &y), E_OK);
	KT_ASSERT_EQ(x, 30);
	KT_ASSERT_EQ(y, 20);

	/* what is stored did not move */
	{
		T_CABENT	c;

		KT_ASSERT_ER(cab_ref(0, &c), E_OK);
		KT_ASSERT_EQ(c.r.left, e.r.left);
		KT_ASSERT_EQ(c.r.top, e.r.top);
	}
	/* but where it is pressed did: one pixel left of its left edge misses */
	KT_ASSERT_EQ(cab_at(e.r.left - 30 - 1, e.r.top + 3 - 20), -1);
	KT_ASSERT_EQ(cab_at(e.r.left + 3 - 30, e.r.top + 3 - 20), 0);

	/* a press lands on the right entry through the scroll */
	ev_make(&ev, HID_EV_BTN_DOWN, e.r.left + 3 - 30, e.r.top + 3 - 20,
		6000000000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);
	KT_ASSERT_EQ(cab_picked(), 1);
	ev_make(&ev, HID_EV_BTN_UP, e.r.left + 3 - 30, e.r.top + 3 - 20,
		6000010000U);
	KT_ASSERT_ER(cab_event(&ev), E_OK);

	/* drawing while scrolled still writes nothing */
	KT_ASSERT_ER(cab_draw(TRUE), E_OK);
	KT_ASSERT_ER(doc_bytes(&after, &na), E_OK);
	if ( before != NULL && after != NULL ) {
		KT_ASSERT_EQ((INT)na, (INT)nb);
		for ( i = 0; i < (INT)nb && i < (INT)na; i++ ) {
			if ( before[i] != after[i] ) { diff = 1; break; }
		}
		KT_ASSERT_EQ(diff, 0);
	}
	if ( before != NULL ) Kfree(before);
	if ( after != NULL ) Kfree(after);

	/* a negative scroll is the top left corner, not a place above it */
	KT_ASSERT_ER(cab_scroll(-5, -5), E_OK);
	KT_ASSERT_ER(cab_scroll_of(&x, &y), E_OK);
	KT_ASSERT_EQ(x, 0);
	KT_ASSERT_EQ(y, 0);
	KT_ASSERT_ER(cab_pick_none(), E_OK);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* a copy is another link to the same object, and it reaches the record on a save */
LOCAL void test_duplicate( void )
{
	T_CABENT	src, cop;
	T_OBREF		robj;
	INT		was, i, found = -1;

	if ( !ready ) KT_SKIP("no cabinet");

	was = links_now();			/* what the record holds now */
	KT_ASSERT(was > 0);
	KT_ASSERT_ER(cab_pick(0, CAB_SEL_ONE), E_OK);
	KT_ASSERT_ER(cab_ref(0, &src), E_OK);
	KT_ASSERT_ER(cab_duplicate(), E_OK);

	/* find the copy: the one that was put in here */
	for ( i = 0; i < cab_count(); i++ ) {
		T_CABENT	c;

		if ( cab_ref(i, &c) < E_OK ) continue;
		if ( c.added ) { found = i; cop = c; break; }
	}
	KT_ASSERT(found >= 0);
	if ( found < 0 ) return;

	/* it points at the same object and sits beside the original */
	KT_ASSERT_EQ(ts_uuid_cmp(&cop.target, &src.target), 0);
	KT_ASSERT(ts_uuid_cmp(&cop.vobjid, &src.vobjid) != 0);
	KT_ASSERT(cop.r.left > src.r.left);

	/* the record does not have it yet */
	KT_ASSERT_EQ(links_now(), was);
	/* taking it back removes it from the cabinet */
	KT_ASSERT_ER(cab_undo(), E_OK);
	{
		T_CABENT	c;

		KT_ASSERT_ER(cab_ref(found, &c), E_OK);
		KT_ASSERT(c.gone);
		KT_ASSERT_EQ(cab_at(cop.r.left + 3, cop.r.top + 3), 0);
	}
	/* and putting it back again brings it in */
	KT_ASSERT_ER(cab_redo(), E_OK);
	{
		T_CABENT	c;

		KT_ASSERT_ER(cab_ref(found, &c), E_OK);
		KT_ASSERT(!c.gone);
	}

	/* now write it: the record gains a link and the object gains a reference */
	KT_ASSERT_ER(ob_ref_obj(&target, &robj), E_OK);
	i = robj.refcnt;
	KT_ASSERT_ER(cab_save(), E_OK);
	KT_ASSERT_EQ(links_now(), was + 1);
	KT_ASSERT_ER(ob_ref_obj(&target, &robj), E_OK);
	KT_ASSERT_EQ(robj.refcnt, i + 1);
	KT_ASSERT_EQ(cab_self_check(), 0);
}

/* colour and size change what is picked, and both can be taken back */
LOCAL void test_look( void )
{
	T_CABENT	e, c;
	T_TAD		*doc = NULL;
	T_VOBJ		*v;
	INT		i, w0, h0;

	if ( !ready ) KT_SKIP("no cabinet");

	KT_ASSERT_ER(cab_pick(0, CAB_SEL_ONE), E_OK);
	KT_ASSERT_ER(cab_ref(0, &e), E_OK);
	w0 = e.r.right - e.r.left;
	h0 = e.r.bottom - e.r.top;

	/* a colour */
	KT_ASSERT_ER(cab_colour(CAB_COL_BACK, 0x00123456U), E_OK);
	KT_ASSERT_ER(cab_colour(CAB_COL_MAX, 0), E_PAR);
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_redo(), E_OK);

	/* a size: the top left stays where it is */
	KT_ASSERT_ER(cab_resize(20, 10), E_OK);
	KT_ASSERT_ER(cab_ref(0, &c), E_OK);
	KT_ASSERT_EQ(c.r.left, e.r.left);
	KT_ASSERT_EQ(c.r.top, e.r.top);
	KT_ASSERT_EQ(c.r.right - c.r.left, w0 + 20);
	KT_ASSERT_EQ(c.r.bottom - c.r.top, h0 + 10);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* it does not close up to nothing */
	KT_ASSERT_ER(cab_resize(-10000, -10000), E_OK);
	KT_ASSERT_ER(cab_ref(0, &c), E_OK);
	KT_ASSERT_EQ(c.r.right - c.r.left, CAB_MIN_W);
	KT_ASSERT_EQ(c.r.bottom - c.r.top, CAB_MIN_H);

	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &c), E_OK);
	KT_ASSERT_EQ(c.r.right - c.r.left, w0 + 20);
	KT_ASSERT_ER(cab_undo(), E_OK);
	KT_ASSERT_ER(cab_ref(0, &c), E_OK);
	KT_ASSERT_EQ(c.r.right - c.r.left, w0);
	KT_ASSERT_EQ(cab_self_check(), 0);

	/* the colour reaches the record when it is written */
	KT_ASSERT_ER(cab_save(), E_OK);
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_find(doc, &e.vobjid, v), E_OK);
		KT_ASSERT_EQ((INT)v->bgcol, 0x00123456);
		tad_free(doc);
	}
	Kfree(v);
	KT_ASSERT_ER(cab_pick_none(), E_OK);
}

EXPORT void ktest_cab( void )
{
	KT_RUN(test_make);
	KT_RUN(test_open);
	KT_RUN(test_at);
	KT_RUN(test_pick);
	KT_RUN(test_move);
	KT_RUN(test_undo);
	KT_RUN(test_drag);
	KT_RUN(test_press_empty);
	KT_RUN(test_pick_with_mods);
	KT_RUN(test_keys);
	KT_RUN(test_other_window);
	KT_RUN(test_draw_writes_nothing);
	KT_RUN(test_save);
	KT_RUN(test_arrange);
	KT_RUN(test_delete);
	KT_RUN(test_scroll);
	KT_RUN(test_duplicate);
	KT_RUN(test_look);
	KT_RUN(test_close);
}
