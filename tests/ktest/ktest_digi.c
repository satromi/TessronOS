/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_digi.c
 *	Pens and touch screens (device/usb/hid.c, design 6.4.1).
 *
 *	- desc: report descriptors of a touch screen of Microsoft's protocol
 *	  (two contacts a report, Contact Count, Input Mode), a pen, a touch
 *	  pad and a Wacom device on its own pages, read as the input driver
 *	  reads them: where the contacts are, and the feature report that
 *	  puts each into its mode (none for the touch pad).
 *	- touch: a pen and a touch screen on one device, plugged into the
 *	  USB controller in software (device/usb/vhc.c) and given reports
 *	  as the device sends them. The Input Mode report is sent to it. A
 *	  touch let go at once is a click of the main button where it
 *	  touched; one held a second is a click of the second button, the
 *	  pointer showing the menu's shape meanwhile; one moved is a drag.
 *	  A second finger does not become the pointer; contacts spread over
 *	  two reports are one frame; a palm leaves no click. Two fingers
 *	  moved together are the wheel, the way what is shown goes with
 *	  them, and go on a while when let go moving; the first one's touch
 *	  is then no click, no hold, and no drag after the other lets go; a
 *	  pinch is nothing; two tapped are a click of the second button.
 *	  Not taken together (二本指スクロール off), a second finger is left
 *	  alone as before. The pen's tip
 *	  presses at once, the barrel switch makes it the second button,
 *	  and while it is near, touches are not taken.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/usb.h>
#include <ts/hid.h>
#include <ts/time.h>
#include <ts/disp.h>
#include <ts/wm.h>
#include <ts/look.h>
#if defined(_QEMU_VIRT_) || defined(X64)
#include "../../device/usb/usbdev.h"
#include "../../device/usb/vhc.h"
#define KT_VHC		1
#else
#define KT_VHC		0
#endif

IMPORT BOOL knl_hid_probe_dg( CONST UB *dsc, INT n, BOOL wacom, INT *info, UB *mode, INT *p_mlen, INT *p_mrid );
IMPORT BOOL knl_pt_holding( void ) __attribute__((weak));

#define DGK_PEN		1		/* as device/usb/hid.c */
#define DGK_TOUCH	2

/*
 * A touch screen as Microsoft's protocol has it: report 12 with two
 * fingers (Tip Switch, Confidence, 6 bits of padding, Contact
 * Identifier, X and Y of 16 bits, 0..12368, over 309.9 by 174.3 mm as
 * the X1 Yoga's 14 inch screen: Physical Maximum 3099 and 1743 in
 * centimetres to the power -2) and Contact Count; a Device
 * Configuration application with feature report 3 (Input Mode, Device
 * Index).
 */
#define FINGER \
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, \
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
	0x09, 0x47, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, \
	0x09, 0x51, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, \
	0x05, 0x01, 0x26, 0x50, 0x30, 0x75, 0x10, 0x95, 0x01, \
	0x55, 0x0e, 0x65, 0x11, 0x46, 0x1b, 0x0c, \
	0x09, 0x30, 0x81, 0x02, 0x46, 0xcf, 0x06, 0x09, 0x31, 0x81, 0x02, 0xc0

LOCAL CONST UB rd_touch[] = {
	0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x0c,
	FINGER,
	FINGER,
	0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	0xc0,
	0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x23, 0xa1, 0x02,
	0x09, 0x52, 0x09, 0x53, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x02,
	0xc0, 0xc0
};

/*
 * A pen: report 6 with Tip Switch, Barrel Switch, Invert, Eraser, a
 * bit of padding, In Range, two bits of padding, X and Y of 16 bits
 * (0..30927) and Tip Pressure -- X at bit 8 and Y at bit 24, as the
 * X1 Yoga's pen interface says.
 */
LOCAL CONST UB rd_pen[] = {
	0x05, 0x0d, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x06, 0x09, 0x20, 0xa1, 0x00,
	0x09, 0x42, 0x09, 0x44, 0x09, 0x3c, 0x09, 0x45, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03,
	0x09, 0x32, 0x81, 0x02, 0x95, 0x02, 0x81, 0x03,
	0x05, 0x01, 0x26, 0xcf, 0x78, 0x75, 0x10, 0x95, 0x01,
	0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
	0x05, 0x0d, 0x09, 0x30, 0x26, 0xff, 0x0f, 0x81, 0x02,
	0xc0, 0xc0
};

/* A touch pad with its Input Mode: not read as a touch screen, and not put into its mode */
LOCAL CONST UB rd_touchpad[] = {
	0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
	FINGER,
	0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	0xc0,
	0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x23, 0xa1, 0x02,
	0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,
	0xc0, 0xc0
};

/*
 * A Wacom touch screen on Wacom's page 0xFF0D: the touch screen
 * application 0x0004, a finger 0x0022, Tip Switch 0x0042, X 0x0130 and
 * Y 0x0131, which stand for the digitizer and generic desktop pages'
 */
LOCAL CONST UB rd_wacom[] = {
	0x06, 0x0d, 0xff, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x02,
	0x09, 0x22, 0xa1, 0x02,
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
	0x95, 0x07, 0x81, 0x03,
	0x0a, 0x30, 0x01, 0x0a, 0x31, 0x01, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0xc0, 0xc0
};

LOCAL void test_digi_desc( void )
{
	INT	info[1 + 8 * 2];
	UB	mode[64];
	INT	mlen = -1, mrid = -1;

	/* the touch screen: two contacts a report, the count after them; Input Mode 2 */
	KT_ASSERT(knl_hid_probe_dg(rd_touch, sizeof(rd_touch), FALSE, info, mode, &mlen, &mrid));
	KT_ASSERT_EQ(info[0], 1);
	KT_ASSERT_EQ(info[1], DGK_TOUCH);
	KT_ASSERT_EQ(info[2], 12);
	KT_ASSERT_EQ(info[3], 2);
	KT_ASSERT_EQ(info[4], 0);		/* Tip Switch */
	KT_ASSERT_EQ(info[5], 8);		/* Contact Identifier */
	KT_ASSERT_EQ(info[6], 16);		/* X */
	KT_ASSERT_EQ(info[7], 64);		/* the second contact's X */
	KT_ASSERT_EQ(info[8], 96);		/* Contact Count */
	KT_ASSERT_EQ(mlen, 3);
	KT_ASSERT_EQ(mrid, 3);
	KT_ASSERT_EQ(mode[0], 3);
	KT_ASSERT_EQ(mode[1], 2);		/* Input Mode: the multi-input device mode */
	KT_ASSERT_EQ(mode[2], 0);		/* Device Index */

	/* the pen: one contact, X at bit 8; no mode to set */
	KT_ASSERT(knl_hid_probe_dg(rd_pen, sizeof(rd_pen), FALSE, info, mode, &mlen, &mrid));
	KT_ASSERT_EQ(info[0], 1);
	KT_ASSERT_EQ(info[1], DGK_PEN);
	KT_ASSERT_EQ(info[2], 6);
	KT_ASSERT_EQ(info[3], 1);
	KT_ASSERT_EQ(info[4], 0);
	KT_ASSERT_EQ(info[6], 8);
	KT_ASSERT_EQ(mlen, 0);

	/* the touch pad is neither, and keeps its mode */
	(void)knl_hid_probe_dg(rd_touchpad, sizeof(rd_touchpad), FALSE, info, mode, &mlen, &mrid);
	KT_ASSERT_EQ(info[0], 0);
	KT_ASSERT_EQ(mlen, 0);

	/* Wacom's page: read as the usages it stands for only for a Wacom device */
	KT_ASSERT(knl_hid_probe_dg(rd_wacom, sizeof(rd_wacom), TRUE, info, mode, &mlen, &mrid));
	KT_ASSERT_EQ(info[0], 1);
	KT_ASSERT_EQ(info[1], DGK_TOUCH);
	KT_ASSERT_EQ(info[2], 2);
	KT_ASSERT_EQ(info[4], 0);
	KT_ASSERT_EQ(info[6], 8);
	KT_ASSERT(!knl_hid_probe_dg(rd_wacom, sizeof(rd_wacom), FALSE, info, mode, &mlen, &mrid));
}

#if KT_VHC
/* ---------------------------------------------------------------- on the bus */

LOCAL CONST UB dg_ddesc[18] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x45, 0x9b, 0x00, 0x01, 1, 2, 0, 1
};
LOCAL CONST UB dg_cfg[59] = {
	9, 2, 59, 0, 2, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 1, 3, 0, 0, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(rd_touch), 0,
	7, 5, 0x81, 3, 64, 0, 2,
	9, 4, 1, 0, 1, 3, 0, 0, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(rd_pen), 0,
	7, 5, 0x82, 3, 64, 0, 1
};

LOCAL INT	dg_sets;		/* SET_REPORT of a feature report */
LOCAL UB	dg_set[16];
LOCAL INT	dg_setlen, dg_setif, dg_setval;

LOCAL INT dg_klass( CONST UB *setup, UB *data, INT len )
{
	switch ( setup[1] ) {
	case 0x0a:				/* SET_IDLE */
		return 0;
	case 0x09:				/* SET_REPORT */
		if ( setup[3] == 3 ) {
			dg_sets++;
			dg_setif = setup[4];
			dg_setval = setup[2] | ( setup[3] << 8 );
			dg_setlen = ( len < (INT)sizeof(dg_set) ) ? len : (INT)sizeof(dg_set);
			if ( data != NULL ) knl_memcpy(dg_set, data, dg_setlen);
		}
		return 0;
	}
	return -1;
}

LOCAL CONST T_VHCDEV dg_dev = {
	dg_ddesc, dg_cfg, sizeof(dg_cfg), { "TessronOS test", "Pen and touch test sensor", NULL, NULL },
	{ rd_touch, rd_pen, NULL, NULL }, { sizeof(rd_touch), sizeof(rd_pen), 0, 0 },
	NULL, dg_klass
};

LOCAL UINT	tch_id, pen_id;		/* the two pointers' ids */

/* The pointers after the ids up to last, by interface; FALSE until both are there */
LOCAL BOOL dg_found( UINT last )
{
	T_HIDINFO	h[16];
	INT		n, i;

	tch_id = pen_id = 0;
	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].keyboard || h[i].id <= last ) continue;
		if ( h[i].ifno == 0 ) tch_id = h[i].id;
		if ( h[i].ifno == 1 ) pen_id = h[i].id;
	}
	return ( tch_id != 0 && pen_id != 0 );
}

LOCAL UD dg_reports( UINT id )
{
	T_HIDINFO	h[16];
	INT		n, i;

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].id == id ) return h[i].reports;
	}
	return 0;
}

/* A report given to endpoint ep, and waited for until the driver has taken it */
LOCAL void dg_send( UINT id, UB ep, CONST UB *r, INT len )
{
	UD	was = dg_reports(id);
	INT	i;

	KT_ASSERT_ER(knl_vhc_in(1, ep, r, len), E_OK);
	for ( i = 0; i < 100 && dg_reports(id) == was; i++ ) {
		tk_dly_tsk(5);
	}
	KT_ASSERT(dg_reports(id) > was);
}

/* One contact of a touch report */
typedef struct {
	BOOL	tip, conf;
	INT	id, x, y;
} CT;

#define NOT_THERE	{ FALSE, FALSE, 0, 0, 0 }

LOCAL void touch( INT count, CT a, CT b )
{
	UB	r[14];
	CT	*c[2];
	INT	i;

	c[0] = &a;
	c[1] = &b;
	knl_memset(r, 0, sizeof(r));
	r[0] = 12;
	for ( i = 0; i < 2; i++ ) {
		UB	*p = r + 1 + 6 * i;

		p[0] = (UB)( ( c[i]->tip ? 1 : 0 ) | ( c[i]->conf ? 2 : 0 ) );
		p[1] = (UB)c[i]->id;
		p[2] = (UB)c[i]->x;
		p[3] = (UB)( c[i]->x >> 8 );
		p[4] = (UB)c[i]->y;
		p[5] = (UB)( c[i]->y >> 8 );
	}
	r[13] = (UB)count;
	dg_send(tch_id, 0x81, r, sizeof(r));
}

LOCAL CT on_at( INT id, INT x, INT y )
{
	CT	c = { TRUE, TRUE, id, x, y };

	return c;
}

LOCAL CT off_at( INT id, INT x, INT y )
{
	CT	c = { FALSE, TRUE, id, x, y };

	return c;
}

LOCAL void pen( BOOL near, BOOL tip, BOOL barrel, INT x, INT y )
{
	UB	r[8];

	knl_memset(r, 0, sizeof(r));
	r[0] = 6;
	r[1] = (UB)( ( tip ? 0x01 : 0 ) | ( barrel ? 0x02 : 0 ) | ( near ? 0x20 : 0 ) );
	r[2] = (UB)x;
	r[3] = (UB)( x >> 8 );
	r[4] = (UB)y;
	r[5] = (UB)( y >> 8 );
	r[6] = tip ? 0x00 : 0;
	r[7] = tip ? 0x08 : 0;
	dg_send(pen_id, 0x82, r, sizeof(r));
}

/* The next event: of type and code (code -1 any), within tmo; answers it */
LOCAL T_HIDEV want( UINT type, INT code, TMO tmo )
{
	T_HIDEV	ev;
	ER	er;

	knl_memset(&ev, 0, sizeof(ev));
	er = ts_hid_read(&ev, tmo);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(ev.type, type);
	if ( code >= 0 ) KT_ASSERT_EQ(ev.code, code);
	return ev;
}

/* Nothing comes for tmo */
LOCAL void none( TMO tmo )
{
	T_HIDEV	ev;
	ER	er = ts_hid_read(&ev, tmo);

	if ( er >= E_OK ) {
		tm_printf((UB *)"  unwanted event: type %d code %d at %d,%d\n",
			  (INT)ev.type, (INT)ev.code, ev.x, ev.y);
	}
	KT_ASSERT_ER(er, E_TMOUT);
}

LOCAL BOOL holding( void )
{
	return ( knl_pt_holding != NULL ) ? knl_pt_holding() : FALSE;
}

/* ---------------------------------------------------------------- for the other suites */

/*
 * The test's touch screen plugged in for a suite that works the desktop
 * with a finger (ktest_dtwin.c): TRUE once it is there. Its touches go
 * through the input layer as the device's do, to whoever reads them.
 */
EXPORT BOOL kt_touch_plug( void )
{
	T_HIDINFO	h[16];
	UINT		last = 0;
	INT		n, i;

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].id > last ) last = h[i].id;
	}
	if ( knl_vhc_plug(1, &dg_dev) < E_OK ) return FALSE;
	for ( i = 0; i < 60 && !dg_found(last); i++ ) {
		tk_dly_tsk(50);
	}
	if ( !dg_found(last) ) {
		(void)knl_vhc_unplug(1);
		return FALSE;
	}
	return TRUE;
}

/* The finger id touching (on) or let go at (sx, sy) on the screen */
EXPORT void kt_touch_at( INT id, BOOL on, INT sx, INT sy )
{
	T_DISPSPEC	spec;
	INT		w = 640, h = 480, x, y;

	if ( ts_disp_ref(&spec) >= E_OK ) {
		w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
		h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	}
	/* the device's 0..12368 across the view: the first value that comes to the pixel */
	x = (INT)( ( (D)sx * 12369 + w - 1 ) / w );
	y = (INT)( ( (D)sy * 12369 + h - 1 ) / h );
	touch(1, on ? on_at(id, x, y) : off_at(id, x, y), (CT)NOT_THERE);
}

EXPORT void kt_touch_unplug( void )
{
	INT	i;

	(void)knl_vhc_unplug(1);
	for ( i = 0; i < 60 && dg_reports(tch_id) != 0; i++ ) {
		tk_dly_tsk(50);
	}
}

/* ---------------------------------------------------------------- a finger's lengths */

LOCAL INT kt_isqrt( UD v )
{
	UD	r = 0;

	while ( ( r + 1 ) * ( r + 1 ) <= v ) r++;
	return (INT)r;
}

/* The view pixels in 100 mm of the test's touch screen (309.9 by 174.3 mm), as the input layer is to make them */
LOCAL INT px100_want( void )
{
	T_DISPSPEC	spec;
	INT		w = 640, h = 480;

	if ( ts_disp_ref(&spec) >= E_OK ) {
		w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
		h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	}
	return (INT)( (D)kt_isqrt((UD)w * w + (UD)h * h) * 1000 / kt_isqrt(3099ULL * 3099 + 1743ULL * 1743) );
}

/* Touched at (x, y) and let go at once: the click's press, its let go taken too */
LOCAL T_HIDEV tap( INT id, INT x, INT y )
{
	T_HIDEV	ev;
	INT	i;

	touch(1, on_at(id, x, y), (CT)NOT_THERE);
	tk_dly_tsk(40);
	touch(1, off_at(id, x, y), (CT)NOT_THERE);
	knl_memset(&ev, 0, sizeof(ev));
	for ( i = 0; i < 8; i++ ) {
		KT_ASSERT_ER(ts_hid_read(&ev, 500), E_OK);
		if ( ev.type != HID_EV_MOVE ) break;
	}
	KT_ASSERT_EQ(ev.type, HID_EV_BTN_DOWN);
	KT_ASSERT_EQ(ev.code, 0);
	(void)want(HID_EV_BTN_UP, 0, 500);
	return ev;
}

/* Two taps a little apart (3.8 mm across, 1.4 mm down), soon after each other: whether the second was clicked where the first was */
LOCAL BOOL taps_joined( INT id )
{
	T_HIDEV	a, b;

	none(600);				/* not the second of a tap before */
	a = tap(id, 6000, 6000);
	b = tap(id + 1, 6150, 6100);
	tm_printf((UB *)"  taps at %d,%d and %d,%d\n", a.x, a.y, b.x, b.y);
	return (BOOL)( a.x == b.x && a.y == b.y );
}

/* A look entry set as ユーザ環境設定 sets it, and the input layer told */
LOCAL void look_set( UINT num, INT v )
{
	KT_ASSERT_ER(knl_look_setting(num, (UW)v), E_OK);
}

/*
 * What ユーザ環境設定 sets of a finger (LK_TCH_*): the handles reached
 * farther by the length set, made pixels by the size the screen says
 * it is, none with the correction off, nor for a pen's press or one put
 * in; the taps of a double tap brought together within the touch's
 * tolerance, whatever the mouse's (LK_DBL_W) is, and not with the
 * correction off; a hold as long as set, the correction on or off.
 */
LOCAL void test_digi_reach( void )
{
	T_HIDEV		ev, e;
	T_HIDATTR	a;
	INT		px = 0, want_px = px100_want(), i;
	INT		was_on = wm_num(LK_TCH_ON, 1), was_dbl = wm_num(LK_TCH_DBL, 50);
	INT		was_hit = wm_num(LK_TCH_HIT, 40), was_hold = wm_num(LK_TCH_HOLD, 1000);
	INT		was_w = wm_num(LK_DBL_W, 10);
	UD		t0, t1;

	tm_printf((UB *)"  a finger's reach\n");
	KT_ASSERT_EQ(was_on, 1);		/* as nothing said: on, 5 mm, 4 mm, 1 s */
	KT_ASSERT_EQ(was_dbl, 50);
	KT_ASSERT_EQ(was_hit, 40);
	KT_ASSERT_EQ(was_hold, 1000);
	none(600);

	/* a finger's press: the screen's pixels from its size, the handles 4 mm farther */
	ev = tap(13, 7000, 5000);
	KT_ASSERT(knl_hid_taken_touch(&px));
	tm_printf((UB *)"  %d pixels in 100 mm (%d wanted), a handle %d pixels farther\n",
		  px, want_px, ( knl_wm_touch_note(ev.when), wm_hit_slop(ev.when) ));
	KT_ASSERT(px >= want_px - 1 && px <= want_px + 1);
	KT_ASSERT_EQ(wm_hit_slop(ev.when), 40 * px / 1000);
	KT_ASSERT(wm_hit_slop(ev.when) > 0);
	/* the length set: farther */
	look_set(LK_TCH_HIT, 80);
	KT_ASSERT_EQ(wm_hit_slop(ev.when), 80 * px / 1000);
	/* the correction off: as the pointer */
	look_set(LK_TCH_ON, 0);
	KT_ASSERT_EQ(wm_hit_slop(ev.when), 0);
	look_set(LK_TCH_ON, 1);
	look_set(LK_TCH_HIT, 40);
	KT_ASSERT_EQ(wm_hit_slop(0), 0);

	/* a pen's press, and one put in: not a finger's */
	pen(TRUE, FALSE, FALSE, 15000, 15000);
	pen(TRUE, TRUE, FALSE, 15000, 15000);
	for ( i = 0; i < 8; i++ ) {
		KT_ASSERT_ER(ts_hid_read(&e, 500), E_OK);
		if ( e.type != HID_EV_MOVE ) break;
	}
	KT_ASSERT_EQ(e.type, HID_EV_BTN_DOWN);
	KT_ASSERT(!knl_hid_taken_touch(NULL));
	knl_wm_touch_note(e.when);
	KT_ASSERT_EQ(wm_hit_slop(e.when), 0);
	pen(TRUE, FALSE, FALSE, 15000, 15000);
	(void)want(HID_EV_BTN_UP, 0, 200);
	pen(FALSE, FALSE, FALSE, 15000, 15000);
	knl_memset(&e, 0, sizeof(e));
	e.type = HID_EV_BTN_DOWN;
	e.x = 100;
	e.y = 100;
	(void)ts_get_mono(&e.when);
	KT_ASSERT_ER(kt_inject(&e), E_OK);
	for ( i = 0; i < 8; i++ ) {
		KT_ASSERT_ER(ts_hid_read(&ev, 500), E_OK);
		if ( ev.type == HID_EV_BTN_DOWN ) break;
	}
	KT_ASSERT_EQ(ev.type, HID_EV_BTN_DOWN);
	KT_ASSERT(!knl_hid_taken_touch(NULL));
	knl_wm_touch_note(ev.when);
	KT_ASSERT_EQ(wm_hit_slop(ev.when), 0);
	e.type = HID_EV_BTN_UP;
	KT_ASSERT_ER(kt_inject(&e), E_OK);
	(void)want(HID_EV_BTN_UP, 0, 500);

	/* a double tap: within 5 mm, brought together, whatever the mouse's tolerance */
	KT_ASSERT(taps_joined(20));
	look_set(LK_DBL_W, 4);
	KT_ASSERT(taps_joined(22));
	look_set(LK_DBL_W, 24);
	KT_ASSERT(taps_joined(24));
	look_set(LK_DBL_W, was_w);
	/* the touch's tolerance 1 mm: not */
	look_set(LK_TCH_DBL, 10);
	KT_ASSERT_ER(ts_hid_getattr(&a), E_OK);
	KT_ASSERT_EQ(a.tch_dbl, 10);
	KT_ASSERT(!taps_joined(26));
	look_set(LK_TCH_DBL, 50);
	/* the correction off: not */
	look_set(LK_TCH_ON, 0);
	KT_ASSERT_ER(ts_hid_getattr(&a), E_OK);
	KT_ASSERT_EQ(a.tch_off, 1);
	KT_ASSERT(!taps_joined(28));

	/* a hold of half a second, with the correction still off: the second button then */
	look_set(LK_TCH_HOLD, 500);
	KT_ASSERT_ER(ts_hid_getattr(&a), E_OK);
	KT_ASSERT_EQ(a.tch_hold, 500);
	none(600);
	touch(1, on_at(30, 8000, 6000), (CT)NOT_THERE);
	t0 = 0;
	(void)ts_get_mono(&t0);
	ev = want(HID_EV_MOVE, -1, 500);
	ev = want(HID_EV_BTN_DOWN, 1, 1000);
	t1 = 0;
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"  held %d ms\n", (INT)( ( t1 - t0 ) / 1000000 ));
	KT_ASSERT(t1 - t0 >= 450000000ULL);
	KT_ASSERT(t1 - t0 < 900000000ULL);
	touch(1, off_at(30, 8000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_UP, 1, 500);
	KT_ASSERT(!holding());

	look_set(LK_TCH_ON, was_on);
	look_set(LK_TCH_DBL, was_dbl);
	look_set(LK_TCH_HIT, was_hit);
	look_set(LK_TCH_HOLD, was_hold);
	none(600);
}

/* ---------------------------------------------------------------- two fingers */

/* What came until nothing more came for quiet: the wheels' notches, the buttons, and the rest */
typedef struct {
	INT	v, h;			/* notches summed, up and down and across */
	INT	nv, nh;			/* the wheel's events */
	INT	down, up;		/* the buttons pressed and let go */
	INT	other;			/* anything else (a move) */
	INT	wrong;			/* a notch of the up and down wheel the other way from the first */
	T_HIDEV	btn;			/* the first press */
	T_HIDEV	wheel;			/* the first notch */
} GOT;

LOCAL GOT gather( TMO quiet )
{
	GOT	g;
	T_HIDEV	ev;

	knl_memset(&g, 0, sizeof(g));
	while ( ts_hid_read(&ev, quiet) >= E_OK ) {
		switch ( ev.type ) {
		case HID_EV_WHEEL:
			if ( g.nv + g.nh == 0 ) g.wheel = ev;
			if ( ev.code == HID_WHEEL_H ) {
				g.h += ev.dz;
				g.nh++;
			} else {
				if ( g.nv > 0 && ( ( g.v > 0 ) != ( ev.dz > 0 ) ) ) g.wrong++;
				g.v += ev.dz;
				g.nv++;
			}
			break;
		case HID_EV_BTN_DOWN:
			if ( g.down == 0 ) g.btn = ev;
			g.down++;
			break;
		case HID_EV_BTN_UP:
			g.up++;
			break;
		default:
			g.other++;
			break;
		}
	}
	tm_printf((UB *)"    wheel %d (%d) across %d (%d), buttons %d/%d, others %d\n",
		  g.v, g.nv, g.h, g.nh, g.down, g.up, g.other);
	return g;
}

/* The device's units across and down of the view's pixels n of them */
LOCAL INT ux_of( INT px )
{
	T_DISPSPEC	spec;
	INT		w = 640;

	if ( ts_disp_ref(&spec) >= E_OK ) w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
	return (INT)( (D)px * 12369 / w );
}

LOCAL INT uy_of( INT px )
{
	T_DISPSPEC	spec;
	INT		h = 480;

	if ( ts_disp_ref(&spec) >= E_OK ) h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	return (INT)( (D)px * 12369 / h );
}

/* Where on the view a device's place across is, as the input layer makes it */
LOCAL INT sx_of( INT u )
{
	T_DISPSPEC	spec;
	INT		w = 640;

	if ( ts_disp_ref(&spec) >= E_OK ) w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
	return (INT)( (D)u * w / 12369 );
}

/* Two fingers a and b moved together by (dx, dy) from where they are, in n frames gap ms apart */
LOCAL void two_go( INT a, INT *ax, INT *ay, INT b, INT *bx, INT *by, INT dx, INT dy, INT n, TMO gap )
{
	INT	k, x0 = *ax, y0 = *ay, x1 = *bx, y1 = *by;

	for ( k = 1; k <= n; k++ ) {
		*ax = x0 + dx * k / n;
		*ay = y0 + dy * k / n;
		*bx = x1 + dx * k / n;
		*by = y1 + dy * k / n;
		touch(2, on_at(a, *ax, *ay), on_at(b, *bx, *by));
		if ( gap > 0 ) tk_dly_tsk(gap);
	}
}

/* Held still a while and let go together: nothing thrown */
LOCAL void two_still_off( INT a, INT ax, INT ay, INT b, INT bx, INT by )
{
	tk_dly_tsk(150);
	touch(2, on_at(a, ax, ay), on_at(b, bx, by));
	tk_dly_tsk(20);
	touch(2, on_at(a, ax, ay), on_at(b, bx, by));
	touch(2, off_at(a, ax, ay), off_at(b, bx, by));
}

/*
 * Two fingers, with two fingers taken together as by default (design
 * 6.4.1): moved together they are the wheel, a notch for each 10 mm,
 * the way what is shown goes with them; the first one's touch is no
 * click and no hold, nor a drag after the other lets go; a pinch is
 * nothing; let go while moving, the scroll goes on and stops; a tap of
 * the two is a click of the second button, but not after a hold.
 */
LOCAL void test_digi_two( void )
{
	INT	px = px100_want(), step = (INT)( (D)100 * px / 1000 );
	INT	dy = uy_of(step * 5 / 2), dx = ux_of(step * 5 / 2);
	INT	ax, ay, bx, by, moved;
	GOT	g;

	tm_printf((UB *)"  two fingers together: %d pixels a notch, %d by %d of the device for 2.5\n", step, dx, dy);
	none(600);

	/* the first, then the second: down 2.5 notches' worth, slowly */
	tm_printf((UB *)"  two fingers down\n");
	ax = 5000; ay = 4000; bx = 6500; by = 4000;
	touch(1, on_at(20, ax, ay), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	touch(2, on_at(20, ax, ay), on_at(21, bx, by));
	none(100);
	two_go(20, &ax, &ay, 21, &bx, &by, 0, dy, 5, 30);
	two_still_off(20, ax, ay, 21, bx, by);
	g = gather(400);
	KT_ASSERT_EQ(g.v, 2);			/* turned away: what is shown goes down with the fingers */
	KT_ASSERT_EQ(g.wrong, 0);
	KT_ASSERT_EQ(g.nh, 0);
	KT_ASSERT_EQ(g.down + g.up + g.other, 0);
	if ( g.nv > 0 ) KT_ASSERT_EQ(g.wheel.x, ( sx_of(5000) + sx_of(6500) ) / 2);	/* where they touched together */

	/* resting together longer than a hold: neither a click nor the second button; then up */
	tm_printf((UB *)"  two fingers held, then up\n");
	ax = 5000; ay = 8000; bx = 6500; by = 8000;
	touch(1, on_at(22, ax, ay), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	touch(2, on_at(22, ax, ay), on_at(23, bx, by));
	none(1300);
	KT_ASSERT(!holding());
	two_go(22, &ax, &ay, 23, &bx, &by, 0, -dy, 5, 30);
	tk_dly_tsk(150);
	/* the first lets go, the second stays and moves far: no drag, no pointer */
	touch(1, on_at(23, bx, by), (CT)NOT_THERE);
	tk_dly_tsk(20);
	touch(1, on_at(23, bx - 3000, by + 2000), (CT)NOT_THERE);
	touch(1, on_at(23, bx - 3000, by + 2000), (CT)NOT_THERE);
	g = gather(300);
	KT_ASSERT_EQ(g.v, -2);
	KT_ASSERT_EQ(g.down + g.up + g.other, 0);
	touch(1, off_at(23, bx - 3000, by + 2000), (CT)NOT_THERE);
	none(1200);

	/* across, to the left: the wheel across turned to the right */
	tm_printf((UB *)"  two fingers left\n");
	ax = 8000; ay = 6000; bx = 8000; by = 7500;
	touch(2, on_at(24, ax, ay), on_at(25, bx, by));
	two_go(24, &ax, &ay, 25, &bx, &by, -dx, 0, 5, 30);
	two_still_off(24, ax, ay, 25, bx, by);
	g = gather(400);
	KT_ASSERT_EQ(g.h, 2);
	KT_ASSERT_EQ(g.nv, 0);
	KT_ASSERT_EQ(g.down + g.up + g.other, 0);

	/* let go while moving fast: the scroll goes on, slows and stops */
	tm_printf((UB *)"  two fingers thrown\n");
	ax = 5000; ay = 2000; bx = 6500; by = 2000;
	touch(2, on_at(26, ax, ay), on_at(27, bx, by));
	{
		INT	k, early = 0;
		T_HIDEV	ev;

		/* taken as they come, as the desktop takes them: the speed is from the moves as they were */
		for ( k = 1; k <= 8; k++ ) {
			touch(2, on_at(26, ax, ay + dy * 2 * k / 8), on_at(27, bx, by + dy * 2 * k / 8));
			while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) {
				if ( ev.type == HID_EV_WHEEL && ev.code == HID_WHEEL_V ) early += ev.dz;
			}
		}
		ay += dy * 2;
		by += dy * 2;
		touch(2, off_at(26, ax, ay), off_at(27, bx, by));
		moved = 5;			/* 2.5 notches' worth twice over */
		g = gather(400);
		tm_printf((UB *)"    %d notches while moving, %d after\n", early, g.v);
		g.v += early;
	}
	KT_ASSERT(g.v > moved);
	KT_ASSERT(g.v < moved + 60);
	KT_ASSERT_EQ(g.wrong, 0);
	KT_ASSERT_EQ(g.nh, 0);
	KT_ASSERT_EQ(g.down + g.up + g.other, 0);

	/* a pinch, both apart and one still: no scroll, and no tap after it */
	tm_printf((UB *)"  pinch\n");
	ax = 5000; ay = 6000; bx = 7000; by = 6000;
	touch(2, on_at(28, ax, ay), on_at(29, bx, by));
	{
		INT	k;

		for ( k = 1; k <= 5; k++ ) {
			touch(2, on_at(28, 5000 - 400 * k, 6000), on_at(29, 7000 + 400 * k, 6000));
			tk_dly_tsk(20);
		}
		touch(2, off_at(28, 3000, 6000), off_at(29, 9000, 6000));
		g = gather(300);
		KT_ASSERT_EQ(g.nv + g.nh + g.down + g.up + g.other, 0);
		touch(2, on_at(30, 5000, 6000), on_at(31, 7000, 6000));
		for ( k = 1; k <= 5; k++ ) {
			touch(2, on_at(30, 5000, 6000), on_at(31, 7000 + 700 * k, 6000 + 100 * k));
			tk_dly_tsk(20);
		}
		touch(2, off_at(30, 5000, 6000), off_at(31, 10500, 6500));
		g = gather(300);
		KT_ASSERT_EQ(g.nv + g.nh + g.down + g.up + g.other, 0);
	}

	/* a tap of two: the second button clicked where they touched together */
	tm_printf((UB *)"  two fingers tapped\n");
	none(600);
	touch(2, on_at(32, 5000, 6000), on_at(33, 6000, 6000));
	tk_dly_tsk(60);
	touch(2, off_at(32, 5000, 6000), off_at(33, 6000, 6000));
	g = gather(300);
	KT_ASSERT_EQ(g.down, 1);
	KT_ASSERT_EQ(g.up, 1);
	KT_ASSERT_EQ(g.btn.code, 1);
	KT_ASSERT_EQ(g.btn.x, ( sx_of(5000) + sx_of(6000) ) / 2);
	KT_ASSERT_EQ(g.nv + g.nh + g.other, 0);
	/* the second soon after the first, let go one after the other: the same */
	touch(1, on_at(34, 5000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	tk_dly_tsk(100);
	touch(2, on_at(34, 5000, 6000), on_at(35, 6000, 6000));
	tk_dly_tsk(60);
	touch(1, on_at(35, 6000, 6000), (CT)NOT_THERE);
	tk_dly_tsk(30);
	touch(1, off_at(35, 6000, 6000), (CT)NOT_THERE);
	g = gather(300);
	KT_ASSERT_EQ(g.down, 1);
	KT_ASSERT_EQ(g.up, 1);
	KT_ASSERT_EQ(g.btn.code, 1);
	KT_ASSERT_EQ(g.nv + g.nh + g.other, 0);

	/* held into the second button, then a second finger: let go, and no second click */
	tm_printf((UB *)"  held, then a second finger\n");
	none(600);
	touch(1, on_at(36, 5200, 6300), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	(void)want(HID_EV_BTN_DOWN, 1, 1500);
	touch(2, on_at(36, 5200, 6300), on_at(37, 6000, 6000));
	(void)want(HID_EV_BTN_UP, 1, 500);
	KT_ASSERT(!holding());
	tk_dly_tsk(60);
	touch(2, off_at(36, 5200, 6300), off_at(37, 6000, 6000));
	none(600);

	/* dragging, then a second finger: the drag let go; the first alone after it drags nothing */
	tm_printf((UB *)"  dragging, then a second finger\n");
	touch(1, on_at(38, 5000, 5000), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	touch(1, on_at(38, 8000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_DOWN, 0, 500);
	(void)want(HID_EV_MOVE, -1, 500);
	touch(2, on_at(38, 8000, 6000), on_at(39, 9000, 6000));
	(void)want(HID_EV_BTN_UP, 0, 500);
	touch(1, on_at(38, 8000, 6000), (CT)NOT_THERE);
	touch(1, on_at(38, 4000, 3000), (CT)NOT_THERE);
	none(1200);
	touch(1, off_at(38, 4000, 3000), (CT)NOT_THERE);
	none(600);

	/* three fingers together: no click of any button */
	tm_printf((UB *)"  three fingers\n");
	touch(3, on_at(40, 5000, 5000), on_at(41, 7000, 7000));
	touch(0, on_at(42, 8000, 8000), (CT)NOT_THERE);
	tk_dly_tsk(60);
	touch(0, (CT)NOT_THERE, (CT)NOT_THERE);
	none(600);
}

/* The view pixels on the screen in 1 mm, about: 10 mm of two fingers is a notch of the wheel */
LOCAL INT tw_px_of( void )
{
	INT	p = px100_want() / 100;

	return ( p > 0 ) ? p : 1;
}

LOCAL void test_digi_touch( void )
{
	T_HIDATTR	a, keep;
	T_HIDEV		ev, at, at2;
	UINT		last = 0;
	T_HIDINFO	h[16];
	INT		n, i;
	UD		t0, t1;
	UINT		b;

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.pd_on = a.pd_off = 0;
	a.main = 0;
	a.absolute = 1;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].id > last ) last = h[i].id;
	}
	dg_sets = 0;
	KT_ASSERT_ER(knl_vhc_plug(1, &dg_dev), E_OK);
	for ( i = 0; i < 60 && !dg_found(last); i++ ) {
		tk_dly_tsk(50);
	}
	KT_ASSERT(dg_found(last));
	if ( !dg_found(last) ) {
		(void)knl_vhc_unplug(1);
		(void)ts_hid_setattr(&keep);
		return;
	}
	/* the Input Mode report, to the touch screen's interface only */
	KT_ASSERT_EQ(dg_sets, 1);
	KT_ASSERT_EQ(dg_setif, 0);
	KT_ASSERT_EQ(dg_setval, 0x0303);
	KT_ASSERT_EQ(dg_setlen, 3);
	KT_ASSERT_EQ(dg_set[0], 3);
	KT_ASSERT_EQ(dg_set[1], 2);
	KT_ASSERT_EQ(dg_set[2], 0);
	while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) ;

	/* a tap: the pointer goes there, and letting go clicks the main button there */
	tm_printf((UB *)"  tap\n");
	touch(1, on_at(5, 6000, 6000), (CT)NOT_THERE);
	at = want(HID_EV_MOVE, -1, 500);
	none(150);
	touch(1, off_at(5, 6000, 6000), (CT)NOT_THERE);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	ev = want(HID_EV_BTN_UP, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	none(100);

	/* a second tap a little off, soon after: clicked where the first was */
	tm_printf((UB *)"  double tap\n");
	touch(1, on_at(6, 6150, 6100), (CT)NOT_THERE);
	at2 = want(HID_EV_MOVE, -1, 500);
	none(80);
	touch(1, off_at(6, 6150, 6100), (CT)NOT_THERE);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	tm_printf((UB *)"  the second tap at %d,%d, clicked at %d,%d\n", at2.x, at2.y, ev.x, ev.y);
	KT_ASSERT(at2.x != at.x);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	(void)want(HID_EV_BTN_UP, 0, 500);
	none(600);

	/* held a second, wavering a little: the second button pressed then, let go with the finger */
	tm_printf((UB *)"  hold\n");
	touch(1, on_at(7, 6000, 6000), (CT)NOT_THERE);
	t0 = 0;
	(void)ts_get_mono(&t0);
	ev = want(HID_EV_MOVE, -1, 500);	/* back from the second tap's place */
	KT_ASSERT_EQ(ev.x, at.x);
	none(500);
	KT_ASSERT(!holding());
	touch(1, on_at(7, 6010, 6005), (CT)NOT_THERE);
	ev = want(HID_EV_BTN_DOWN, 1, 1000);
	t1 = 0;
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"  held %d ms, the menu's shape %s\n", (INT)( ( t1 - t0 ) / 1000000 ),
		  holding() ? "shown" : "not shown");
	KT_ASSERT(t1 - t0 >= 900000000ULL);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	if ( knl_pt_holding != NULL ) KT_ASSERT(holding());
	none(100);				/* still touching: nothing more */
	touch(1, off_at(7, 6010, 6005), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_UP, 1, 500);
	KT_ASSERT(!holding());
	none(100);

	/* moved: a drag of the main button from where it touched */
	tm_printf((UB *)"  drag\n");
	touch(1, on_at(9, 6010, 6005), (CT)NOT_THERE);	/* where the last let go: no move */
	KT_ASSERT_ER(ts_hid_pointer(&at.x, &at.y, &b), E_OK);
	none(150);
	touch(1, on_at(9, 9000, 6000), (CT)NOT_THERE);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	ev = want(HID_EV_MOVE, -1, 500);
	KT_ASSERT(ev.x > at.x);
	KT_ASSERT_EQ(ev.code & 1, 1);
	touch(1, off_at(9, 9000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_UP, 0, 500);
	none(1200);				/* nor anything when it would have been held */

	/*
	 * with two fingers not taken together (二本指スクロール off): a
	 * second finger is not the pointer, before or after the first lets
	 * go, and the first one's tap is still a click
	 */
	tm_printf((UB *)"  two fingers, not together\n");
	a.tch_noscr = 1;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);
	touch(1, on_at(1, 6000, 6000), (CT)NOT_THERE);
	at = want(HID_EV_MOVE, -1, 500);
	touch(2, on_at(1, 6000, 6000), on_at(2, 3000, 3000));
	none(100);
	touch(1, on_at(2, 3000, 3000), (CT)NOT_THERE);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	(void)want(HID_EV_BTN_UP, 0, 500);
	touch(1, on_at(2, 2000, 2000), (CT)NOT_THERE);
	none(150);
	touch(0, (CT)NOT_THERE, (CT)NOT_THERE);	/* all of them gone */
	none(100);
	touch(1, on_at(3, 2000, 2000), (CT)NOT_THERE);
	at = want(HID_EV_MOVE, -1, 500);
	touch(1, off_at(3, 2000, 2000), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_DOWN, 0, 500);
	(void)want(HID_EV_BTN_UP, 0, 500);

	/* three contacts in two reports: one frame, taken when the second comes */
	tm_printf((UB *)"  two reports a frame\n");
	touch(3, on_at(4, 5000, 5000), on_at(5, 7000, 7000));
	none(100);
	touch(0, on_at(6, 8000, 8000), (CT)NOT_THERE);
	at = want(HID_EV_MOVE, -1, 500);
	touch(0, (CT)NOT_THERE, (CT)NOT_THERE);	/* none: the first finger let go */
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	(void)want(HID_EV_BTN_UP, 0, 500);
	a.tch_noscr = 0;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);

	/* a palm: no click */
	tm_printf((UB *)"  palm\n");
	touch(1, on_at(8, 4000, 4000), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	{
		CT	palm = { TRUE, FALSE, 8, 4000, 4000 };

		touch(1, palm, (CT)NOT_THERE);
	}
	none(1200);
	touch(0, (CT)NOT_THERE, (CT)NOT_THERE);
	none(100);
	KT_ASSERT(!holding());

	/* the pen: over the screen it points, its tip presses at once, the barrel makes it the second button */
	tm_printf((UB *)"  pen\n");
	pen(TRUE, FALSE, FALSE, 15000, 15000);
	(void)want(HID_EV_MOVE, -1, 500);
	none(100);
	pen(TRUE, TRUE, FALSE, 15000, 15000);
	(void)want(HID_EV_BTN_DOWN, 0, 200);
	pen(TRUE, FALSE, FALSE, 15000, 15000);
	(void)want(HID_EV_BTN_UP, 0, 200);
	pen(TRUE, FALSE, TRUE, 15000, 15000);
	none(100);
	pen(TRUE, TRUE, TRUE, 15000, 15000);
	(void)want(HID_EV_BTN_DOWN, 1, 200);
	pen(TRUE, TRUE, FALSE, 15000, 15000);	/* the barrel let go while touching: still the second */
	none(100);
	pen(TRUE, FALSE, FALSE, 15000, 15000);
	(void)want(HID_EV_BTN_UP, 1, 200);

	/* a hand on the screen while the pen is near is not a touch */
	tm_printf((UB *)"  pen near\n");
	touch(1, on_at(10, 6000, 6000), (CT)NOT_THERE);
	none(1200);
	touch(0, (CT)NOT_THERE, (CT)NOT_THERE);
	none(100);
	pen(FALSE, FALSE, FALSE, 15000, 15000);
	none(100);
	touch(1, on_at(11, 6000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	touch(1, off_at(11, 6000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_BTN_DOWN, 0, 500);
	(void)want(HID_EV_BTN_UP, 0, 500);

	test_digi_two();
	test_digi_reach();

	/* taken away while touching: no click left behind */
	touch(1, on_at(12, 7000, 6000), (CT)NOT_THERE);
	(void)want(HID_EV_MOVE, -1, 500);
	KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
	for ( i = 0; i < 60 && dg_reports(tch_id) != 0; i++ ) {
		tk_dly_tsk(50);
	}
	none(1200);
	KT_ASSERT(!holding());
	KT_ASSERT_ER(ts_hid_setattr(&keep), E_OK);
}
#endif /* KT_VHC */

#if KT_VHC
/* ---------------------------------------------------------------- consumer keys */

/*
 * A keyboard's second interface: report 3 has a bit each for volume up,
 * down and mute; report 4 is an array of two 16-bit usages, from 0.
 * Then a device's interface with a list of usages and no report IDs.
 */
LOCAL CONST UB rd_cc[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
	0x85, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
	0x09, 0xe9, 0x09, 0xea, 0x09, 0xe2, 0x81, 0x02,
	0x95, 0x05, 0x81, 0x03,
	0x85, 0x04, 0x15, 0x00, 0x26, 0x3c, 0x02, 0x19, 0x00, 0x2a, 0x3c, 0x02,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x00,
	0xc0
};
LOCAL CONST UB rd_cc2[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
	0x15, 0x01, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01,
	0x09, 0xcd, 0x09, 0xb5, 0x09, 0xe9, 0x81, 0x00,
	0xc0
};
LOCAL CONST UB cc_cfg[59] = {
	9, 2, 59, 0, 2, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 1, 3, 0, 0, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(rd_cc), 0,
	7, 5, 0x81, 3, 16, 0, 2,
	9, 4, 1, 0, 1, 3, 0, 0, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(rd_cc2), 0,
	7, 5, 0x82, 3, 16, 0, 2
};
LOCAL CONST T_VHCDEV cc_dev = {
	dg_ddesc, cc_cfg, sizeof(cc_cfg), { "TessronOS test", "Consumer keys test", NULL, NULL },
	{ rd_cc, rd_cc2, NULL, NULL }, { sizeof(rd_cc), sizeof(rd_cc2), 0, 0 },
	NULL, dg_klass
};

LOCAL void cc_want( UB ep, CONST UB *r, INT len, UINT type, UINT code )
{
	T_HIDEV	ev;

	KT_ASSERT_ER(knl_vhc_in(1, ep, r, len), E_OK);
	KT_ASSERT_ER(ts_hid_read(&ev, 1000), E_OK);
	KT_ASSERT_EQ(ev.type, type);
	KT_ASSERT_EQ(ev.code, code);
}

/*
 * Keys of the consumer page in every form a descriptor gives them come
 * out as keys, in an interface that counts as a keyboard, and what is
 * held is let go when the device goes.
 */
LOCAL void test_consumer_keys( void )
{
	T_HIDATTR	a, keep;
	T_HIDINFO	h[16];
	T_HIDSTAT	st0, st1;
	T_HIDEV		ev;
	INT		n, i, seen;
	UINT		last = 0;
	CONST UB	r_up[2] = { 3, 0x01 }, r_none[2] = { 3, 0 };
	CONST UB	r_mute[2] = { 3, 0x04 };
	CONST UB	r_arr[5] = { 4, 0xea, 0, 0, 0 }, r_arr0[5] = { 4, 0, 0, 0, 0 };
	CONST UB	r2_next[1] = { 2 }, r2_none[1] = { 0 }, r2_play[1] = { 1 };

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.key_on = a.key_off = a.key_sim = 0;
	a.krp = 0;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);
	KT_ASSERT_ER(ts_hid_stat(&st0), E_OK);
	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].id > last ) last = h[i].id;
	}
	KT_ASSERT_ER(knl_vhc_plug(1, &cc_dev), E_OK);
	seen = 0;
	for ( i = 0; i < 60 && seen < 2; i++ ) {
		INT	k;

		tk_dly_tsk(50);
		seen = 0;
		n = knl_hid_list(h, 16);
		for ( k = 0; k < n && k < 16; k++ ) {
			if ( h[k].id > last && h[k].keyboard ) seen++;
		}
	}
	KT_ASSERT_EQ(seen, 2);
	KT_ASSERT_ER(ts_hid_stat(&st1), E_OK);
	KT_ASSERT_EQ(st1.keyboards, st0.keyboards + 2);
	KT_ASSERT_EQ(st1.pointers, st0.pointers);
	while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) ;

	/* a bit each */
	cc_want(0x81, r_up, 2, HID_EV_KEY_DOWN, HID_KEY_VOLUP);
	cc_want(0x81, r_none, 2, HID_EV_KEY_UP, HID_KEY_VOLUP);
	cc_want(0x81, r_mute, 2, HID_EV_KEY_DOWN, HID_KEY_MUTE);
	cc_want(0x81, r_none, 2, HID_EV_KEY_UP, HID_KEY_MUTE);
	/* an array from usage 0, in another report: the first report's keys are not its */
	cc_want(0x81, r_up, 2, HID_EV_KEY_DOWN, HID_KEY_VOLUP);
	cc_want(0x81, r_arr, 5, HID_EV_KEY_DOWN, HID_KEY_VOLDOWN);
	cc_want(0x81, r_arr0, 5, HID_EV_KEY_UP, HID_KEY_VOLDOWN);
	cc_want(0x81, r_none, 2, HID_EV_KEY_UP, HID_KEY_VOLUP);
	/* an array of listed usages, no report ID, values from 1 */
	cc_want(0x82, r2_next, 1, HID_EV_KEY_DOWN, HID_KEY_NEXT);
	cc_want(0x82, r2_none, 1, HID_EV_KEY_UP, HID_KEY_NEXT);
	cc_want(0x82, r2_play, 1, HID_EV_KEY_DOWN, HID_KEY_PLAYPAUSE);
	/* held as the device goes: let go */
	KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
	KT_ASSERT_ER(ts_hid_read(&ev, 2000), E_OK);
	KT_ASSERT_EQ(ev.type, HID_EV_KEY_UP);
	KT_ASSERT_EQ(ev.code, HID_KEY_PLAYPAUSE);
	for ( i = 0; i < 60; i++ ) {
		KT_ASSERT_ER(ts_hid_stat(&st1), E_OK);
		if ( st1.keyboards == st0.keyboards ) break;
		tk_dly_tsk(50);
	}
	KT_ASSERT_EQ(st1.keyboards, st0.keyboards);
	(void)ts_hid_setattr(&keep);
}

/* The next press or let go, moves passed over: FALSE when it is not this one by tmo */
LOCAL BOOL btn_is( UINT type, UINT code, TMO tmo )
{
	T_HIDEV	ev;

	while ( ts_hid_read(&ev, tmo) >= E_OK ) {
		if ( ev.type == HID_EV_MOVE ) continue;
		if ( ev.type == type && ev.code == code ) return TRUE;
		tm_printf((UB *)"  got type %d code %d\n", (INT)ev.type, (INT)ev.code);
		return FALSE;
	}
	return FALSE;
}

/* No press or let go by tmo */
LOCAL BOOL btn_none( TMO tmo )
{
	T_HIDEV	ev;

	while ( ts_hid_read(&ev, tmo) >= E_OK ) {
		if ( ev.type != HID_EV_MOVE ) {
			tm_printf((UB *)"  got type %d code %d\n", (INT)ev.type, (INT)ev.code);
			return FALSE;
		}
	}
	return TRUE;
}

/* A finger on (x, y): pressed as it touches, staying pressed past a hold, let go with it */
LOCAL void press_now( INT id, INT x, INT y )
{
	kt_touch_at(id, TRUE, x, y);
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 200));
	KT_ASSERT(btn_none(1300));
	KT_ASSERT(!holding());
	kt_touch_at(id, FALSE, x, y);
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));
	KT_ASSERT(btn_none(100));
}

/* A finger on (x, y) as anywhere else: nothing as it touches, the second button after a hold */
LOCAL void press_waits( INT id, INT x, INT y )
{
	kt_touch_at(id, TRUE, x, y);
	KT_ASSERT(btn_none(200));
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 1, 1500));
	kt_touch_at(id, FALSE, x, y);
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 1, 500));
	KT_ASSERT(btn_none(100));
}

/*
 * The places of a window where a finger presses at once: in them and out
 * of them, the window moved, sized, covered and closed, the table's limits,
 * and two fingers still the wheel there
 */
LOCAL void test_digi_press( void )
{
	T_DPRECT	o, r, c;
	T_WMWIN		w;
	GOT		g;
	INT		a, b, x, y, i, ids[WM_PRESS_WINS + 1];

	r.left = 20;  r.top = 20;  r.right = 120;  r.bottom = 100;
	o.left = 100;  o.top = 80;  o.right = 460;  o.bottom = 360;
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, "press a");
	KT_ASSERT(a >= 1);
	if ( a < 1 ) return;
	if ( !kt_touch_plug() ) {
		(void)wm_close(a);
		KT_SKIP("no touch screen plugged in");
	}
	KT_ASSERT_ER(wm_raise(a), E_OK);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	x = w.work.left + 70;
	y = w.work.top + 60;
	KT_ASSERT(btn_none(300));

	/* no places: as anywhere */
	tm_printf((UB *)"  no places\n");
	press_waits(30, x, y);

	KT_ASSERT_ER(wm_obj_press_area(a, &r, 1), E_OK);
	tm_printf((UB *)"  in a place\n");
	press_now(31, x, y);
	tm_printf((UB *)"  beside it, in the window\n");
	press_waits(32, x + 100, y);
	/* a tap beside it is a click, and in it the finger's move is a drag from the press */
	kt_touch_at(33, TRUE, x + 100, y);
	tk_dly_tsk(40);
	kt_touch_at(33, FALSE, x + 100, y);
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 500));
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));
	tk_dly_tsk(600);
	kt_touch_at(34, TRUE, x, y);
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 200));
	kt_touch_at(34, TRUE, x + 150, y + 90);
	KT_ASSERT(btn_none(1300));
	kt_touch_at(34, FALSE, x + 150, y + 90);
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));

	/* moved: the place with it, and none where it was */
	tm_printf((UB *)"  moved\n");
	o.left += 200;  o.right += 200;  o.top += 40;  o.bottom += 40;
	KT_ASSERT_ER(wm_place(a, &o), E_OK);
	press_now(35, x + 200, y + 40);
	press_waits(36, x + 260, y + 40);		/* beside it where it now is */

	/* sized so that the place is out of it: out of the work area, nothing */
	tm_printf((UB *)"  sized\n");
	o.right = o.left + 100;
	KT_ASSERT_ER(wm_place(a, &o), E_OK);
	KT_ASSERT_ER(wm_ref(a, &w), E_OK);
	KT_ASSERT(btn_none(100));
	kt_touch_at(37, TRUE, w.outer.right + 40, y + 40);
	KT_ASSERT(btn_none(200));
	kt_touch_at(37, FALSE, w.outer.right + 40, y + 40);
	(void)btn_is(HID_EV_BTN_DOWN, 0, 300);
	(void)btn_is(HID_EV_BTN_UP, 0, 300);
	o.right = o.left + 360;
	KT_ASSERT_ER(wm_place(a, &o), E_OK);

	/* covered: the window in front takes it; brought forward again, it is back */
	tm_printf((UB *)"  covered\n");
	c.left = x + 150;  c.top = y;  c.right = x + 280;  c.bottom = y + 90;
	b = wm_open(&c, WM_ATTR_FRAME | WM_ATTR_TITLE, "press b");
	KT_ASSERT(b >= 1);
	KT_ASSERT_ER(wm_raise(b), E_OK);
	press_waits(38, x + 200, y + 40);
	KT_ASSERT_ER(wm_raise(a), E_OK);
	press_now(39, x + 200, y + 40);

	/* hidden: nothing there, the place gone with it */
	KT_ASSERT_ER(wm_show(a, FALSE), E_OK);
	KT_ASSERT_ER(wm_raise(b), E_OK);
	kt_touch_at(40, TRUE, x + 200, y + 40);
	KT_ASSERT(btn_none(200));
	kt_touch_at(40, FALSE, x + 200, y + 40);
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 500));
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));
	KT_ASSERT_ER(wm_show(a, TRUE), E_OK);
	KT_ASSERT_ER(wm_raise(a), E_OK);
	tk_dly_tsk(600);

	/* two fingers together on the place: the wheel, no button */
	tm_printf((UB *)"  two fingers\n");
	{
		INT	ax = ux_of(x + 190), ay = uy_of(y + 40), bx = ux_of(x + 210), by = uy_of(y + 40);

		touch(2, on_at(41, ax, ay), on_at(42, bx, by));
		two_go(41, &ax, &ay, 42, &bx, &by, 0, uy_of(y + 40 + 40 * tw_px_of()) - uy_of(y + 40), 5, 30);
		two_still_off(41, ax, ay, 42, bx, by);
		g = gather(400);
		KT_ASSERT(g.v >= 1);
		KT_ASSERT_EQ(g.down + g.up, 0);
	}
	/* the first on the place, then the second: the press let go, and the wheel */
	{
		INT	ax = ux_of(x + 190), ay = uy_of(y + 40), bx = ux_of(x + 210), by = uy_of(y + 40);

		touch(1, on_at(43, ax, ay), (CT)NOT_THERE);
		KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 200));
		touch(2, on_at(43, ax, ay), on_at(44, bx, by));
		KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));
		two_go(43, &ax, &ay, 44, &bx, &by, 0, uy_of(y + 40 + 40 * tw_px_of()) - uy_of(y + 40), 5, 30);
		two_still_off(43, ax, ay, 44, bx, by);
		g = gather(400);
		KT_ASSERT(g.v >= 1);
		KT_ASSERT_EQ(g.down + g.up, 0);
	}

	/* the list's limits, and taken away */
	KT_ASSERT_ER(wm_obj_press_area(a, &r, WM_PRESS_RECTS + 1), E_PAR);
	KT_ASSERT_ER(wm_obj_press_area(a, &r, -1), E_PAR);
	KT_ASSERT_ER(wm_obj_press_area(a, NULL, 1), E_PAR);
	KT_ASSERT_ER(wm_obj_press_area(0x7FFF, &r, 1), E_ID);
	KT_ASSERT_ER(wm_obj_press_area(a, NULL, 0), E_OK);
	press_waits(45, x + 200, y + 40);
	KT_ASSERT_ER(wm_obj_press_area(a, &r, 1), E_OK);

	/* too many windows with places; and each one's freed as it closes */
	c.left = 0;  c.top = 0;  c.right = 80;  c.bottom = 60;
	for ( i = 0; i < WM_PRESS_WINS - 1; i++ ) {
		ids[i] = wm_open(&c, WM_ATTR_FRAME, "press n");
		KT_ASSERT(ids[i] >= 1);
		KT_ASSERT_ER(wm_obj_press_area(ids[i], &r, 1), E_OK);
	}
	ids[i] = wm_open(&c, WM_ATTR_FRAME, "press n");
	KT_ASSERT(ids[i] >= 1);
	KT_ASSERT_ER(wm_obj_press_area(ids[i], &r, 1), E_LIMIT);
	for ( i = 0; i < WM_PRESS_WINS; i++ ) (void)wm_close(ids[i]);
	KT_ASSERT_ER(wm_raise(a), E_OK);

	/* closed: nothing there; the same number given a new window has no places of the old */
	tm_printf((UB *)"  closed\n");
	KT_ASSERT_ER(wm_close(a), E_OK);
	KT_ASSERT_ER(wm_close(b), E_OK);
	kt_touch_at(46, TRUE, x + 200, y + 40);
	KT_ASSERT(btn_none(200));
	kt_touch_at(46, FALSE, x + 200, y + 40);
	KT_ASSERT(btn_is(HID_EV_BTN_DOWN, 0, 500));
	KT_ASSERT(btn_is(HID_EV_BTN_UP, 0, 500));
	tk_dly_tsk(600);
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "press c");
	KT_ASSERT(a >= 1);
	KT_ASSERT_ER(wm_raise(a), E_OK);
	press_waits(47, x + 200, y + 40);
	(void)wm_close(a);
	kt_touch_unplug();
}
#endif /* KT_VHC */

EXPORT void ktest_digi( void )
{
	KT_RUN(test_digi_desc);
#if KT_VHC
	KT_RUN(test_consumer_keys);
	KT_RUN(test_digi_touch);
	KT_RUN_SCREEN(test_digi_press);
#endif
}
