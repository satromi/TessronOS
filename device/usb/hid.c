/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hid.c
 *	Keyboard and pointer on the bus (design 10.4, 10.14).
 *
 *	A keyboard reports which keys are down, so a press is a key that is
 *	in this report and was not in the last one, and a release is the
 *	other way round. Keeping the last report is the whole of it.
 *
 *	A pointer reports either how far it moved or where it is, and which
 *	of the two depends on the device. Its report descriptor says where
 *	in a report the buttons, the two axes and the wheel are, how wide
 *	each is and whether it is a position or a movement. Both kinds become
 *	a place on the screen, kept here so that anything can ask where the
 *	pointer is without waiting for it to move. A mouse that gives no
 *	usable descriptor is switched to the boot protocol, whose reports
 *	are three bytes: the buttons and a step on each axis.
 *
 *	A mouse of the boot subclass whose descriptor was read is to talk the
 *	report protocol. Some do not: it says the boot protocol when asked
 *	(GET_PROTOCOL), before or after it was told otherwise, or after
 *	another interface of the same device was put into the boot protocol;
 *	or its reports show it, the boot protocol's three bytes, or those
 *	padded with zeros to the length of the descriptor's report. It is
 *	then read as the boot protocol's mouse, and the console says why.
 *
 *	A pen or a touch screen (the digitizer page) says it touches with
 *	Tip Switch rather than a button. A touch screen's report carries
 *	several fingers, and only the first to touch is the pointer, its
 *	main button; a pen's tip is the main button, or the second with its
 *	barrel switch held. A touch screen of Microsoft's protocol is told
 *	its Input Mode first, or it may send nothing but its mouse's
 *	reports. A finger's press is kept back until it is known to be a
 *	click, a drag, or a hold that is the second button (touch_shape,
 *	design 6.4.1); a second finger touching cuts that press, and the
 *	two moved together are the wheel, tapped the second button
 *	(two_shape). A touch screen whose controller does not speak HID
 *	(device/i2c/gt911.c) hands its contacts in a frame at a time
 *	(knl_hid_mt_attach, knl_hid_mt_contact, knl_hid_mt_sync), and they
 *	are taken the same way.
 *
 *	The volume and playing keys of a keyboard are mostly on the consumer
 *	page, in an interface or a report apart from its keys, either as a
 *	bit each or as an array of usages held. Both are read from the report
 *	descriptor (HIDCC) and go on as keys of the keyboard page's numbering
 *	(cc_keys), so that what comes after does not tell them from a key's.
 *	An interface with nothing else counts as a keyboard. A keyboard read
 *	in the boot protocol has only its eight bytes: any consumer report it
 *	would send in the report protocol is not read.
 *
 *	The USB manager hands every HID interface here when the device is
 *	plugged in. The interrupt endpoint is kept armed as a stream; each
 *	report that comes in is turned into events at once and put into a
 *	ring that anyone may take from.
 *
 *	Each event is also copied, with the interface it came from, into a
 *	second ring that nobody takes from: what the objects of the input
 *	devices show when read (peripheral_kernel/obj/obinput.c). Reading
 *	them follows that copy with a place of its own, so it never takes an
 *	event from whoever reads the first ring.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/hid.h>
#include <ts/keymap.h>
#include <ts/disp.h>
#include <ts/time.h>
#include "usbdev.h"

#define HID_FIRST	8		/* interfaces the table has room for at first; it grows */
#define HID_RING	64		/* events waiting to be taken */
#define HID_REPORT_MAX	64		/* the longest report read */
#define HID_RD_MAX	4096		/* the longest report descriptor read */
#define HID_RD_TRIES	3		/* times the report descriptor is asked for */
#define HID_FIELDS	192		/* input fields kept from one: a touch screen has seven or so a finger */
#define HID_FFIELDS	16		/* feature fields kept from one (the input mode and its like) */
#define HID_COLL_MAX	16		/* collections nested in one another */
#define HID_ITEMS_MAX	1024		/* fields of one Input item looked at */
#define HID_BITS_MAX	0x10000		/* Report Size and Count taken at most */
#define HID_PUSH_MAX	4		/* global states a descriptor may push */
#define HID_SHOW_REPORTS 12		/* a pointer's first reports shown on the console */
#define HID_SHOW_DG	64		/* and a pen's or a touch screen's */
#define HID_TAP		256		/* events kept for those who look */
#define HID_BOOT_VOTES	3		/* padded boot reports that decide it is one */

#define HID_PROTO_KBD	1
#define HID_PROTO_MOUSE	2

#define HID_SET_REPORT		0x09
#define HID_SET_IDLE		0x0a
#define HID_SET_PROTOCOL	0x0b
#define HID_GET_PROTOCOL	0x03
#define HID_RT_OUT		(USB_RT_CLASS | USB_RT_INTERFACE)
#define HID_RT_IN		(USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE)
#define HID_REPORT_OUTPUT	2	/* SET_REPORT's report type */
#define HID_REPORT_FEATURE	3

/* The usages the pointer is made of */
#define USAGE_X			0x10030
#define USAGE_Y			0x10031
#define USAGE_WHEEL		0x10038
#define USAGE_PAN		0xC0238		/* the consumer page's AC Pan: a wheel across */
#define USAGE_PAGE_BUTTON	9
#define USAGE_PAGE_CONSUMER	0x0C

/*
 * The digitizer page (HID Usage Tables, 0x0D): a pen or a touch screen
 * says it touches with Tip Switch, not with a button. Its applications,
 * the collections of one contact, and the usages read or set here.
 */
#define USAGE_PAGE_DIGI		0x0D
#define DG_DIGITIZER		0xD0001
#define DG_PEN			0xD0002
#define DG_TOUCHSCREEN		0xD0004
#define DG_TOUCHPAD		0xD0005
#define DG_CONFIG		0xD000E		/* Device Configuration: where Input Mode is */
#define DG_STYLUS		0xD0020
#define DG_FINGER		0xD0022
#define DG_INRANGE		0xD0032
#define DG_INVERT		0xD003C
#define DG_TIP			0xD0042
#define DG_BARREL		0xD0044
#define DG_ERASER		0xD0045
#define DG_CONFIDENCE		0xD0047
#define DG_CONTACTID		0xD0051
#define DG_INPUTMODE		0xD0052
#define DG_CONTACTCOUNT		0xD0054
#define DG_SURFACESW		0xD0057
#define DG_BUTTONSW		0xD0058
#define DG_BARREL2		0xD005A
#define DG_LATENCY		0xD0060

#define DG_INPUTMODE_TOUCHSCREEN 2	/* Input Mode: the multi-input device mode */

#define USB_VID_WACOM		0x056a

/* The kind of collection a field is in */
#define CK_NONE		0
#define CK_FINGER	1		/* one contact of a touch screen */
#define CK_STYLUS	2

/* One variable data field of an input report */
typedef struct {
	INT	off, size;	/* bit position after the report ID, width; 0 absent */
	INT	lmin, lmax;
	BOOL	rel;		/* a movement rather than a position */
	BOOL	sgn;		/* two's complement */
	INT	rid;		/* the report ID it is in, 0 none */
	UW	usage;		/* page << 16 | usage */
	UW	app;		/* the application collection it is in */
	INT	coll;		/* the finger or stylus collection it is in, by number; -1 none */
	INT	ck;		/* that collection's kind, CK_* */
	INT	ext10;		/* how long its range is on the device, 0.1 mm; 0 not said */
} HIDFIELD;

/*
 * A pen or a touch screen: one input report of a digitizer application.
 * A touch screen's report carries one or more contacts, each a finger
 * collection with its Tip Switch, Confidence, Contact Identifier and
 * place, and the contacts of a frame may be spread over several
 * reports, the first saying how many there are (Contact Count) and the
 * others 0 (the hybrid mode of Microsoft's touch screen protocol).
 */
#define HID_DG_APPS	2		/* digitizer applications of one interface: a pen and a touch screen */
#define HID_DG_SLOTS	10		/* contacts of one report */
#define HID_DG_KEEP	10		/* contacts of one frame kept for two fingers */
#define DGK_PEN		1
#define DGK_TOUCH	2

typedef struct {
	HIDFIELD	x, y;
	HIDFIELD	tip, conf, cid, inrange;
} DGSLOT;

typedef struct {
	INT		rid;
	INT		kind;		/* DGK_* */
	INT		nslot;
	DGSLOT		s[HID_DG_SLOTS];
	HIDFIELD	count;		/* Contact Count */
	HIDFIELD	barrel, barrel2, invert, eraser;
	/* a touch screen's frame, and the first finger it follows */
	INT		expect, got;	/* contacts the frame has, and taken so far */
	INT		prim;		/* the first finger's Contact Identifier, -1 none */
	BOOL		p_seen, p_on, p_palm;
	INT		p_x, p_y;	/* where it is, as the device says */
	INT		n_on, was_on;	/* contacts touching in this frame, and in the last */
	BOOL		have_cand;
	INT		cand, cand_x, cand_y;
	INT		shown;		/* its touches and lets go shown on the console */
	INT		diag10;		/* a touch screen's diagonal, 0.1 mm; 0 not known */
	/* the contacts touching in the frame being taken, for two fingers (dg_multi) */
	INT		t_n;
	INT		t_id[HID_DG_KEEP], t_x[HID_DG_KEEP], t_y[HID_DG_KEEP];
	/*
	 * more than one finger has touched since the screen was last
	 * free: the fingers are followed together until every one lets go
	 */
	BOOL		multi;
	BOOL		m_pair;		/* two fingers touching, m_a and m_b */
	INT		m_a, m_b;
	INT		m_n;		/* contacts touching in the last frame */
	INT		m_x, m_y;	/* the pair's midpoint on the screen when last seen */
	/* a pen */
	BOOL		near;		/* in range */
	UINT		pen_btn;	/* what its tip holds: 1 the main button, 2 the second, 0 none */
} DGAPP;

typedef struct {
	INT		n;
	DGAPP		a[HID_DG_APPS];
} HIDDG;

/*
 * The consumer page's keys of one interface: those given a bit each, and
 * those given as an array whose values are usages (from umin, or by
 * place in us when the descriptor lists them one by one). A report
 * holds the keys of its own report ID only, so what is held is kept
 * with the ID it came in.
 */
#define HID_CC_VARS	16
#define HID_CC_ARRAYS	4
#define HID_CC_HELD	8

typedef struct {
	INT	rid, off, size;
	UINT	key;
} CCVAR;

typedef struct {
	INT	rid, off, size, count;
	INT	lmin, lmax;
	BOOL	range;
	UW	umin;
	INT	nus;
	UW	us[16];
} CCARRAY;

typedef struct {
	INT	nvar, narr;
	CCVAR	v[HID_CC_VARS];
	CCARRAY	a[HID_CC_ARRAYS];
	INT	nheld;
	UINT	held[HID_CC_HELD];
	INT	hrid[HID_CC_HELD];
} HIDCC;

typedef struct {
	BOOL	used;
	UINT	id;		/* the interface's number while it is there, from 1 */
	UD	reports;	/* reports taken off it */
	USBDEV	*dev;
	INT	ifno;
	BOOL	is_kbd;
	USBPIPE	*pipe;
	UB	last[8];	/* the report before this one */
	BOOL	have_last;
	/* a pointer */
	INT	report_id;	/* 0: reports carry no ID */
	INT	rlen;		/* the longest input report, its ID included; 0 not known */
	INT	plen;		/* the pointer's report, the same way */
	HIDFIELD btn, x, y, wheel, pan;
	BOOL	boot;		/* read as the boot protocol's mouse */
	BOOL	bootable;	/* it can talk the boot protocol (subclass 1) */
	INT	votes;		/* reports seen that only the boot protocol explains */
	BOOL	settled;	/* a report seen that the boot protocol cannot explain */
	UB	rids[32];	/* the report IDs its descriptor declares, a bit each */
	INT	nbtn;
	UINT	buttons;	/* held according to this interface */
	INT	lastx, lasty;
	INT	lastax, lastay;	/* a pointer that says where it is, taken as moves */
	INT	remx, remy;	/* what the speed left over of the last move */
	BOOL	mid;		/* the middle button, when it is a double press */
	INT	slot;		/* its place in the table, from 0 */
	UB	ext_name[16];	/* not on USB: its device object's name ("ps2kbd") */
	HIDDG	*dg;		/* its pen and touch screen, NULL none */
	HIDCC	*cc;		/* its consumer page's keys, NULL none */
	BOOL	cc_only;	/* it has nothing else: a keyboard of those keys, no pointer */
	INT	tdiag10;	/* the diagonal of the touch screen its touches are on, 0.1 mm; 0 not known */
} HIDDEV;

/*
 * The interfaces, as many as come: a table of entries each allocated
 * once and kept. A larger table is made whole -- the entries before it
 * and new ones -- and put in place by one store of its pointer; the one
 * before is kept, as an interrupt or another task may be walking it.
 */
typedef struct {
	INT	cap;
	HIDDEV	*e[1];		/* cap of them */
} HIDTBL;

LOCAL HIDTBL		*hid_tbl = NULL;

#define HID_N		( ( hid_tbl != NULL ) ? hid_tbl->cap : 0 )
#define HID_AT(i)	( hid_tbl->e[(i)] )
LOCAL BOOL		hid_ready = FALSE;

LOCAL T_HIDEV		hid_ring[HID_RING];
LOCAL BOOL		hid_inj[HID_RING];	/* put in by hand: taken as it is */
LOCAL UB		hid_tch[HID_RING];	/* TCH_*: a touch screen's, shaped as touches are */
LOCAL INT		hid_px[HID_RING];	/* and its view pixels in 100 mm of that screen (touch_px) */
LOCAL UINT		hid_head = 0, hid_tail = 0;
LOCAL ID		hid_mtxid = 0;
LOCAL ID		hid_semid = 0;

LOCAL INT		ptr_x = 0, ptr_y = 0;
LOCAL UINT		ptr_buttons = 0;
LOCAL INT		ptr_press_x = 0, ptr_press_y = 0;	/* where button 0 last went down */
LOCAL BOOL		ptr_injected = FALSE;	/* pointer events were put in without a device */
LOCAL INT		scr_w = 0, scr_h = 0;

LOCAL T_HIDSTAT		hid_count;
LOCAL UINT		hid_next_id = 0;

LOCAL T_HIDEV		tap_ev[HID_TAP];	/* a copy of every event, for those who look */
LOCAL UINT		tap_src[HID_TAP];	/* the interface it came from, 0 put in */
LOCAL UD		tap_seq = 0;		/* events copied so far */

/* How the keyboard and the pointer are taken: until told, as the devices say */
LOCAL T_HIDATTR		hid_attr = { 0, 0, 12, 7, 0, 1, 1, 0, 0, 0, 1, 800, 100, 0, 0, 0, 0, 0, 0, 0 };

/* ---------------------------------------------------------------- ring */

/*
 * Put one event in. When nobody has taken the oldest by the time the
 * ring fills, that one is dropped rather than the newest: what just
 * happened matters more than what happened a while ago.
 */
/*
 * The window manager's compositor, told the pointer moved: it lays the
 * pointer where it now is at its next build, whatever the program that
 * reads the events is busy with. Not there when there are no windows.
 */
IMPORT void knl_wm_pointer_hint( void ) __attribute__((weak));

/*
 * 強制終了 (outer_kernel/wm/fquit.c): every event is shown to it before
 * the ring, with the keys as the keyboard's layout made them. Shift+Pause
 * and Ctrl+Shift+Pause, and everything while its window is up, are its
 * own and go no further: it answers whether it took the event. Taken
 * here, they work whatever the reader of the ring is doing. Not there
 * without windows.
 */
IMPORT BOOL knl_fq_input( CONST T_HIDEV *ev, BOOL inj ) __attribute__((weak));

/* Whether 強制終了's window takes every event, the desktop reading none */
IMPORT BOOL knl_fq_grabbing( void ) __attribute__((weak));

LOCAL void moved_hint( CONST T_HIDEV *ev )
{
	if ( ev->type == HID_EV_MOVE && knl_wm_pointer_hint != NULL ) {
		knl_wm_pointer_hint();
	}
}

/* What a pointer event is, for the shaping when it is taken (touch_shape) */
#define TCH_NO		0		/* a mouse's, a pen's, a key's */
#define TCH_TOUCH	1		/* the first finger on a touch screen */
#define TCH_CANCEL	2		/* that finger gone without a touch meant (a palm, a pen near) */
#define TCH_MULTI	3		/* that finger's press cut by another finger touching */
#define TCH_TWO		4		/* two fingers together (HID_EV_TWO) */

/*
 * Two fingers as the input layer passes them from the frames to the
 * shaping (two_shape): never out of ts_hid_read, nor in the copy the
 * input devices' objects show. x, y is the midpoint of the two on the
 * screen, dz how far apart they are (view pixels).
 */
#define HID_EV_TWO	0x40
#define TW_DOWN		1		/* two fingers touch: a new pair */
#define TW_MOVE		2		/* the pair as it is now */
#define TW_UP		3		/* the pair no longer touches; a finger may stay */
#define TW_MANY		4		/* a third finger: no pair, and no tap */
#define TW_END		5		/* every finger let go */
#define TW_CANCEL	6		/* gone without a touch meant (a pen near, taken away) */

/*
 * How many pixels of the view 100 mm of a touch screen is, from its
 * diagonal (0.1 mm) and the view's: what a length given in millimetres
 * for a finger (TC_DBL, the handles' reach of the window layer) comes
 * to on that screen. A screen that does not say its size is taken to
 * be 240 mm across, a 48th of its width 5 mm, and at least 4 pixels a
 * millimetre: the measure a finger was given before sizes were read.
 */
#define TC_GUESS_W10	2400
#define TC_GUESS_MIN	400

LOCAL INT isqrt( UD v );

LOCAL INT touch_px( INT diag10 )
{
	INT	w = ( scr_w > 0 ) ? scr_w : 640, h = ( scr_h > 0 ) ? scr_h : 480, px;

	if ( diag10 > 0 ) {
		px = (INT)( (D)isqrt((UD)w * w + (UD)h * h) * 1000 / diag10 );
	} else {
		px = (INT)( (D)w * 1000 / TC_GUESS_W10 );
		if ( px < TC_GUESS_MIN ) px = TC_GUESS_MIN;
	}
	return ( px > 0 ) ? px : 1;
}

LOCAL void ring_put_as( CONST T_HIDEV *ev, BOOL inj, UINT src, UB tch, INT px )
{
	UINT	next;
	T_HIDEV	cut;

	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	/*
	 * Two fingers while 強制終了's window takes every event: the desktop
	 * reads none, so what they would make of the wheel goes to no one
	 * until it reads again, and then to the windows behind. They are let
	 * go here; their end is passed on as not a touch, so the shaping
	 * makes nothing of what it had of them.
	 */
	if ( tch == TCH_TWO && knl_fq_grabbing != NULL && knl_fq_grabbing() ) {
		if ( ev->code != TW_END && ev->code != TW_CANCEL ) {
			hid_count.events++;
			tk_unl_mtx(hid_mtxid);
			return;
		}
		cut = *ev;
		cut.code = TW_CANCEL;
		ev = &cut;
	}
	if ( tch != TCH_TWO ) {
		tap_ev[tap_seq % HID_TAP] = *ev;
		tap_src[tap_seq % HID_TAP] = src;
		tap_seq++;
	}
	if ( tch != TCH_TWO && knl_fq_input != NULL && knl_fq_input(ev, inj) ) {
		hid_count.events++;
		tk_unl_mtx(hid_mtxid);
		moved_hint(ev);
		return;
	}
	/*
	 * A move after a move not yet taken: only where the pointer now is
	 * matters, so the waiting one is brought up to date; so with two
	 * fingers' moves, whose shaping looks at where they are. A pointer that
	 * keeps moving while the taker is slow then never fills the ring,
	 * and a press or a let go waiting in it is never pushed out.
	 */
	if ( ( ev->type == HID_EV_MOVE || ( ev->type == HID_EV_TWO && ev->code == TW_MOVE ) ) && hid_head != hid_tail ) {
		UINT	last = ( hid_tail + HID_RING - 1 ) % HID_RING;

		if ( hid_ring[last].type == ev->type && hid_ring[last].code == ev->code
		  && hid_inj[last] == inj && hid_tch[last] == tch && hid_px[last] == px ) {
			hid_ring[last] = *ev;
			hid_count.events++;
			tk_unl_mtx(hid_mtxid);
			moved_hint(ev);
			return;
		}
	}
	next = (hid_tail + 1) % HID_RING;
	if ( next == hid_head ) {
		hid_head = (hid_head + 1) % HID_RING;
		hid_count.dropped++;
	} else {
		tk_sig_sem(hid_semid, 1);
	}
	hid_ring[hid_tail] = *ev;
	hid_inj[hid_tail] = inj;
	hid_tch[hid_tail] = tch;
	hid_px[hid_tail] = px;
	hid_tail = next;
	hid_count.events++;
	tk_unl_mtx(hid_mtxid);
	moved_hint(ev);
}

/*
 * A keyboard's event with the keys given other numbers (キー配列): the
 * key's number changed, and the modifiers held made what they are after
 * it -- a key made a modifier holds that modifier while it is down, and
 * a modifier made another key no longer holds its own.
 */
LOCAL UINT	km_held = 0;		/* modifier bits held by keys made modifiers */

LOCAL void ring_put( CONST HIDDEV *d, CONST T_HIDEV *ev )
{
	T_HIDEV	e = *ev;

	if ( e.type == HID_EV_KEY_DOWN || e.type == HID_EV_KEY_UP ) {
		UINT	to = km_remap(e.code);

		if ( to != e.code ) {
			if ( to >= 0xE0 && to <= 0xE7 ) {
				UINT	bit = 1U << ( to - 0xE0 );

				if ( e.type == HID_EV_KEY_DOWN ) km_held |= bit;
				else km_held &= ~bit;
			}
			e.code = to;
		}
	}
	e.mods = ( e.mods & ~km_mods_away() ) | km_held;
	ring_put_as(&e, FALSE, d->id, TCH_NO, 0);
}

/* A pointer's event, marked as what it is for the shaping */
LOCAL void ring_put_ptr( CONST HIDDEV *d, CONST T_HIDEV *ev, UB tch )
{
	T_HIDEV	e = *ev;

	e.mods = ( e.mods & ~km_mods_away() ) | km_held;
	ring_put_as(&e, FALSE, d->id, tch, ( tch != TCH_NO ) ? touch_px(d->tdiag10) : 0);
}

LOCAL volatile UW	hid_late_ms = 0;

LOCAL BOOL ring_take( T_HIDEV *ev, BOOL *p_inj, UB *p_tch, INT *p_px )
{
	BOOL	got = FALSE;

	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	if ( hid_head != hid_tail ) {
		*ev = hid_ring[hid_head];
		*p_inj = hid_inj[hid_head];
		*p_tch = hid_tch[hid_head];
		*p_px = hid_px[hid_head];
		hid_head = (hid_head + 1) % HID_RING;
		got = TRUE;
	}
	tk_unl_mtx(hid_mtxid);

	return got;
}

/* The whole square root */
LOCAL INT isqrt( UD v )
{
	UD	r = 0, b = 1ULL << 62;

	while ( b > v ) b >>= 2;
	while ( b != 0 ) {
		if ( v >= r + b ) {
			v -= r + b;
			r = ( r >> 1 ) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return (INT)r;
}

LOCAL UD now_ns( void )
{
	UD	ns = 0;

	ts_get_mono(&ns);

	return ns;
}

/* ---------------------------------------------------------------- keyboard */

/*
 * A keyboard report is the keys held down, so what changed since the
 * last one is what happened. Six keys at a time is what the boot
 * protocol carries.
 */
LOCAL void kbd_report( HIDDEV *d, CONST UB *r )
{
	T_HIDEV	ev;
	INT	i, k;
	BOOL	found;

	UB	keys[8];

	knl_memset(&ev, 0, sizeof(ev));
	ev.mods = r[0];
	ev.when = now_ns();
	ev.x = ptr_x;
	ev.y = ptr_y;

	/*
	 * Usages 1 to 3 in the key array (ErrorRollOver, POSTFail,
	 * ErrorUndefined) say the keyboard cannot tell which keys are held:
	 * more are down than it can report, or it has a ghost. The keys
	 * are then still the ones last reported, not none of them, or each
	 * would be let go and pressed again when the jam clears. The
	 * modifiers are bits of their own and still true.
	 */
	knl_memcpy(keys, r, 8);
	for ( i = 2; i < 8; i++ ) {
		if ( r[i] >= 1 && r[i] <= 3 ) {
			knl_memcpy(keys + 2, d->last + 2, 6);
			break;
		}
	}
	r = keys;

	/*
	 * The first report is compared like any other. What was last
	 * seen starts as all zero, which is a keyboard with no key held
	 * -- exactly what a keyboard nobody has touched is. Keeping the
	 * first report only to compare the next with would lose the
	 * first key anyone presses.
	 */
	for ( i = 2; i < 8; i++ ) {		/* down now and were not */
		if ( r[i] <= 3 ) {
			continue;		/* nothing, or an error code */
		}
		found = FALSE;
		for ( k = 2; k < 8; k++ ) {
			if ( d->last[k] == r[i] ) {
				found = TRUE;
				break;
			}
		}
		if ( !found ) {
			ev.type = HID_EV_KEY_DOWN;
			ev.code = r[i];
			ring_put(d, &ev);
		}
	}
	for ( i = 2; i < 8; i++ ) {		/* were down and are not */
		if ( d->last[i] <= 3 ) {
			continue;
		}
		found = FALSE;
		for ( k = 2; k < 8; k++ ) {
			if ( r[k] == d->last[i] ) {
				found = TRUE;
				break;
			}
		}
		if ( !found ) {
			ev.type = HID_EV_KEY_UP;
			ev.code = d->last[i];
			ring_put(d, &ev);
		}
	}
	/*
	 * The modifier keys as keys of their own, by their usages (0xE0
	 * left Ctrl .. 0xE7 right GUI): some are keys with a meaning when
	 * pressed alone, as right Ctrl is 入力終 to the text input port.
	 */
	for ( i = 0; i < 8; i++ ) {
		UB	bit = (UB)( 1U << i );

		if ( ( r[0] & bit ) != ( d->last[0] & bit ) ) {
			ev.type = ( ( r[0] & bit ) != 0 ) ? HID_EV_KEY_DOWN : HID_EV_KEY_UP;
			ev.code = 0xE0 + i;
			ring_put(d, &ev);
		}
	}
	knl_memcpy(d->last, r, 8);
	d->have_last = TRUE;
}

/* ---------------------------------------------------------------- pointer */

LOCAL INT get_field( CONST UB *r, INT len, CONST HIDFIELD *f )
{
	INT	i;
	UW	v = 0;

	if ( f->size <= 0 || f->size > 32 ) {
		return 0;
	}
	for ( i = 0; i < f->size; i++ ) {
		INT	bit = f->off + i;

		if ( (bit >> 3) >= len ) {
			break;
		}
		if ( (r[bit >> 3] & (1 << (bit & 7))) != 0 ) {
			v |= 1UL << i;
		}
	}
	if ( f->sgn && f->size < 32 && (v & (1UL << (f->size - 1))) != 0 ) {
		v |= ~0UL << f->size;
	}

	return (INT)v;
}

/* A position across the whole of its range, as a place across max */
LOCAL INT abs_scale( INT v, CONST HIDFIELD *f, INT max )
{
	INT	r = f->lmax - f->lmin + 1;

	if ( r <= 0 ) {
		return 0;
	}

	return (INT)((D)(v - f->lmin) * max / r);
}

/* The buttons held on any pointer: a device may split itself in two */
LOCAL UINT all_buttons( void )
{
	UINT	b = 0;
	INT	i;

	for ( i = 0; i < HID_N; i++ ) {
		if ( HID_AT(i)->used && !HID_AT(i)->is_kbd ) {
			b |= HID_AT(i)->buttons;
		}
	}

	return b;
}

/*
 * A pointer report: the buttons, then either how far it moved or where
 * it is. Either way the result is a place on the screen, kept inside it.
 */
LOCAL void boot_mouse( HIDDEV *h );

/* What one pointer report says, as the device put it */
typedef struct {
	UINT	buttons;
	INT	x, y;		/* a movement, or a position when the axes are absolute */
	INT	wheel, pan;
} PTRIN;

/*
 * A report of three or four bytes that cannot be one the descriptor
 * describes, from a device that can talk the boot protocol: it is still
 * talking that, whatever it was asked (the board's boot loader left it
 * so). With report IDs, it starts with none the descriptor declares,
 * or with the pointer's own (its buttons byte says so when the first
 * button is held) while the axes do not fit after it; without, the
 * axes the descriptor places do not fit in it.
 */
LOCAL BOOL boot_shaped( CONST HIDDEV *d, CONST UB *r, INT len )
{
	INT	bits = len * 8;

	if ( d->boot || !d->bootable || len < 3 || len > 4 ) {
		return FALSE;
	}
	if ( d->report_id != 0 ) {
		if ( ( d->rids[r[0] >> 3] & ( 1 << ( r[0] & 7 ) ) ) == 0 ) {
			return TRUE;
		}
		if ( r[0] != d->report_id ) {
			return FALSE;		/* a short report of another kind */
		}
		bits -= 8;
	}

	return (BOOL)( d->x.off + d->x.size > bits || d->y.off + d->y.size > bits );
}

/*
 * A report as long as the descriptor's, from a device of the boot
 * subclass without report IDs whose axes are wider than a byte, that is
 * the boot protocol's report padded with zeros: the buttons, a byte of
 * X, a byte of Y, perhaps a byte of wheel, and nothing after. Read as
 * the descriptor says, the bytes of X and Y make one axis a number no
 * byte holds (00 09 f9 is X 0xf909, a move of 1783 to the left), and
 * the axis after it reads the padding, nothing.
 *
 * So big a move is not impossible: a mouse of high resolution moved
 * fast makes one. What tells the two apart, report by report:
 *   - a byte not zero after the fourth is not the boot protocol's: it is
 *     the report protocol, and nothing more is looked for (a 16-bit Y
 *     moving up does this at once);
 *   - a move wider than a byte with the fourth byte zero is explained as
 *     well by the boot protocol as by the descriptor: a vote for the boot
 *     protocol;
 *   - such a move with the fourth byte set (the descriptor's Y moving,
 *     or the boot protocol's wheel turning) is more likely the
 *     descriptor's own fast move: the votes start again;
 *   - a move a byte holds says nothing either way.
 * HID_BOOT_VOTES votes decide it. Until then a voting report's move is
 * held back rather than sending the pointer across the screen; its
 * buttons go as they are. Answers whether the move is held back.
 */
LOCAL BOOL boot_padded( HIDDEV *d, CONST UB *r, INT len )
{
	INT	i, v;
	BOOL	wide = FALSE;

	if ( d->boot || d->settled || !d->bootable || d->report_id != 0 || !d->x.rel
	  || ( d->x.size <= 8 && d->y.size <= 8 ) || len < 3 ) {
		return FALSE;
	}
	for ( i = 4; i < len; i++ ) {
		if ( r[i] != 0 ) {
			d->settled = TRUE;
			return FALSE;
		}
	}
	if ( d->x.size > 8 ) {
		v = get_field(r, len, &d->x);
		wide = ( v < -128 || v > 127 );
	}
	if ( !wide && d->y.size > 8 ) {
		v = get_field(r, len, &d->y);
		wide = ( v < -128 || v > 127 );
	}
	if ( !wide ) {
		return FALSE;
	}
	if ( len > 3 && r[3] != 0 ) {
		d->votes = 0;
		return FALSE;
	}
	if ( ++d->votes < HID_BOOT_VOTES ) {
		return TRUE;
	}
	boot_mouse(d);
	d->boot = TRUE;
	USB_LOG("usb: hid: %d sends the boot protocol's reports padded to %d bytes "
		"(%d moves wider than a byte, report %d): read as those\n",
		(INT)d->id, len, d->votes, (INT)d->reports);

	return FALSE;
}

/* One report into what it says; FALSE for a report of another kind */
LOCAL BOOL ptr_decode( HIDDEV *d, CONST UB *r, INT len, PTRIN *o )
{
	INT	i;
	BOOL	held;

	if ( boot_shaped(d, r, len) ) {
		boot_mouse(d);
		d->boot = TRUE;
		USB_LOG("usb: hid: %d sends the boot protocol's reports: read as those\n", (INT)d->id);
	}
	held = boot_padded(d, r, len);
	if ( d->report_id != 0 ) {
		if ( len < 1 || r[0] != d->report_id ) {
			return FALSE;
		}
		r++;
		len--;
	}
	knl_memset(o, 0, sizeof(*o));
	for ( i = 0; i < d->nbtn && i < 3; i++ ) {
		HIDFIELD	b = d->btn;

		b.off += i;
		b.size = 1;
		b.sgn = FALSE;
		if ( get_field(r, len, &b) != 0 ) {
			o->buttons |= 1U << i;
		}
	}
	o->x = get_field(r, len, &d->x);
	o->y = get_field(r, len, &d->y);
	if ( d->wheel.size > 0 ) {
		o->wheel = get_field(r, len, &d->wheel);
	}
	if ( d->pan.size > 0 ) {
		o->pan = get_field(r, len, &d->pan);
	}
	if ( held ) {
		o->x = o->y = 0;
	}

	return TRUE;
}

/*
 * Where the pointer goes for a report's X and Y, as fields fx and fy
 * place them: a movement moves it, sped up and slowed as the attributes
 * say; a position puts it there when it is to be taken as one (direct:
 * a touch screen, which is where the finger is whatever is set), or is
 * taken as a move from the last position (a pen taken as a mouse).
 * Answers whether it moved.
 */
LOCAL BOOL ptr_place( HIDDEV *d, CONST HIDFIELD *fx, CONST HIDFIELD *fy, INT x, INT y, BOOL direct )
{
	BOOL	moved;

	if ( !fx->rel && !direct && !hid_attr.absolute ) {
		/* a pen taken as a mouse: where it went from where it was */
		INT	ax = abs_scale(x, fx, scr_w), ay = abs_scale(y, fy, scr_h);
		BOOL	had = d->have_last;
		INT	dx = ax - d->lastax, dy = ay - d->lastay;

		d->lastax = ax;
		d->lastay = ay;
		d->lastx = x;
		d->lasty = y;
		x = had ? dx : 0;
		y = had ? dy : 0;
	} else if ( !fx->rel ) {
		ptr_x = abs_scale(x, fx, scr_w);
		ptr_y = abs_scale(y, fy, scr_h);
		/* a position moved when it differs from the last report's */
		moved = ( !d->have_last || x != d->lastx || y != d->lasty );
		d->lastx = x;
		d->lasty = y;
		goto kept;
	}
	moved = ( x != 0 || y != 0 );
	/*
	 * Speeding up: a move longer than the threshold goes twice as
	 * far, the threshold the shorter the sooner it is set to speed
	 * up; 7 never does.
	 */
	if ( hid_attr.accel < 7 ) {
		INT	thr = ( (INT)hid_attr.accel + 1 ) * 3;

		if ( x > thr || x < -thr ) x *= 2;
		if ( y > thr || y < -thr ) y *= 2;
	}
	/* the speed: 12 is as the device moved, 1 twelve times as far */
	if ( hid_attr.speed != 12 && hid_attr.speed >= 1 && hid_attr.speed <= 15 ) {
		INT	s = (INT)hid_attr.speed;

		d->remx += x * 12;
		d->remy += y * 12;
		x = d->remx / s;
		y = d->remy / s;
		d->remx -= x * s;
		d->remy -= y * s;
	}
	ptr_x += x;
	ptr_y += y;
kept:
	if ( ptr_x < 0 ) ptr_x = 0;
	if ( ptr_y < 0 ) ptr_y = 0;
	if ( ptr_x >= scr_w ) ptr_x = scr_w - 1;
	if ( ptr_y >= scr_h ) ptr_y = scr_h - 1;

	return moved;
}

/*
 * The pointer where ptr_place left it and this interface's buttons now
 * as events: a move when it moved, then each button that changed on any
 * pointer. tch marks them for the shaping (TCH_*).
 */
LOCAL void ptr_emit( HIDDEV *d, T_HIDEV *ev, BOOL moved, UINT buttons, UB tch )
{
	UINT	changed;
	INT	i;

	ev->x = ptr_x;
	ev->y = ptr_y;
	d->buttons = buttons;
	buttons = all_buttons();
	if ( !d->have_last || moved ) {
		ev->type = HID_EV_MOVE;
		ev->code = buttons;
		ring_put_ptr(d, ev, tch);
	}
	changed = buttons ^ ptr_buttons;
	for ( i = 0; i < 3; i++ ) {
		if ( (changed & (1U << i)) == 0 ) {
			continue;
		}
		ev->type = ( (buttons & (1U << i)) != 0 ) ? HID_EV_BTN_DOWN : HID_EV_BTN_UP;
		ev->code = (UINT)i;
		if ( i == 0 && ev->type == HID_EV_BTN_DOWN ) {
			ptr_press_x = ptr_x;
			ptr_press_y = ptr_y;
		}
		ring_put_ptr(d, ev, tch);
	}
	ptr_buttons = buttons;
	d->have_last = TRUE;
}

LOCAL BOOL dg_report( HIDDEV *d, CONST UB *r, INT len );

/* ---------------------------------------------------------------- consumer keys */

/*
 * What a consumer page's usage is as a key: 0 for one that is not taken.
 * Their numbers are of the keyboard page, so that a key of either page
 * is the same to everything above this.
 */
LOCAL CONST struct { UH usage; UB key; } cc_keys[] = {
	{ 0xE9, HID_KEY_VOLUP },	{ 0xEA, HID_KEY_VOLDOWN },	{ 0xE2, HID_KEY_MUTE },
	{ 0xCD, HID_KEY_PLAYPAUSE },	{ 0xB0, HID_KEY_PLAYPAUSE },	{ 0xB1, HID_KEY_PLAYPAUSE },
	{ 0xB7, HID_KEY_STOP },		{ 0xB6, HID_KEY_PREV },		{ 0xB5, HID_KEY_NEXT }
};

LOCAL UINT cc_key( UW u )
{
	INT	i;

	if ( ( u >> 16 ) != USAGE_PAGE_CONSUMER ) {
		return 0;
	}
	for ( i = 0; i < (INT)( sizeof(cc_keys) / sizeof(cc_keys[0]) ); i++ ) {
		if ( cc_keys[i].usage == ( u & 0xffff ) ) {
			return cc_keys[i].key;
		}
	}
	return 0;
}

/* A key put among those down in this report, once */
LOCAL void cc_note( UINT *now, INT *p_n, UINT key )
{
	INT	i;

	if ( key == 0 ) {
		return;
	}
	for ( i = 0; i < *p_n; i++ ) {
		if ( now[i] == key ) return;
	}
	if ( *p_n < HID_CC_HELD ) {
		now[(*p_n)++] = key;
	}
}

LOCAL INT get_field( CONST UB *r, INT len, CONST HIDFIELD *f );

/* The keys down in a report of ID rid, r being what comes after the ID */
LOCAL INT cc_scan( CONST HIDCC *c, INT rid, CONST UB *r, INT len, UINT *now )
{
	INT		i, k, n = 0;
	HIDFIELD	f;

	knl_memset(&f, 0, sizeof(f));
	for ( i = 0; i < c->nvar; i++ ) {
		if ( c->v[i].rid != rid ) continue;
		f.off = c->v[i].off;
		f.size = c->v[i].size;
		if ( get_field(r, len, &f) != 0 ) {
			cc_note(now, &n, c->v[i].key);
		}
	}
	for ( i = 0; i < c->narr; i++ ) {
		CONST CCARRAY	*a = &c->a[i];

		if ( a->rid != rid ) continue;
		f.size = a->size;
		f.sgn = ( a->lmin < 0 );
		for ( k = 0; k < a->count; k++ ) {
			INT	v;

			f.off = a->off + k * a->size;
			v = get_field(r, len, &f);
			if ( v < a->lmin || v > a->lmax ) {
				continue;	/* out of range: no key in this place */
			}
			v -= a->lmin;
			if ( a->range ) {
				cc_note(now, &n, cc_key(a->umin + (UW)v));
			} else if ( v < a->nus ) {
				cc_note(now, &n, cc_key(a->us[v]));
			}
		}
	}
	return n;
}

/* A key of the consumer page down or let go, as a keyboard's would be */
LOCAL void cc_event( HIDDEV *d, UINT type, UINT key )
{
	T_HIDEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = key;
	ev.when = now_ns();
	ev.x = ptr_x;
	ev.y = ptr_y;
	ring_put(d, &ev);
}

/* What changed among this report's keys. Keys of another report ID stay as they are. */
LOCAL void cc_report( HIDDEV *d, CONST UB *r, INT len )
{
	HIDCC	*c = d->cc;
	UINT	now[HID_CC_HELD];
	INT	rid = 0, n, i, k;

	if ( c == NULL || d->boot ) {
		return;
	}
	/* a descriptor that gives one report an ID gives them all one */
	if ( ( c->nvar > 0 ? c->v[0].rid : c->a[0].rid ) != 0 ) {
		if ( len < 1 ) {
			return;
		}
		rid = r[0];
		r++;
		len--;
	}
	n = cc_scan(c, rid, r, len, now);
	for ( i = 0; i < c->nheld; ) {
		BOOL	still = ( c->hrid[i] != rid );

		for ( k = 0; !still && k < n; k++ ) {
			still = ( now[k] == c->held[i] );
		}
		if ( still ) {
			i++;
			continue;
		}
		cc_event(d, HID_EV_KEY_UP, c->held[i]);
		c->nheld--;
		c->held[i] = c->held[c->nheld];
		c->hrid[i] = c->hrid[c->nheld];
	}
	for ( k = 0; k < n; k++ ) {
		BOOL	had = FALSE;

		for ( i = 0; i < c->nheld; i++ ) {
			had = had || ( c->held[i] == now[k] );
		}
		if ( !had && c->nheld < HID_CC_HELD ) {
			c->held[c->nheld] = now[k];
			c->hrid[c->nheld] = rid;
			c->nheld++;
			cc_event(d, HID_EV_KEY_DOWN, now[k]);
		}
	}
}

/* The interface went: what it held is let go */
LOCAL void cc_let_go( HIDDEV *d )
{
	HIDCC	*c = d->cc;

	while ( c != NULL && c->nheld > 0 ) {
		c->nheld--;
		cc_event(d, HID_EV_KEY_UP, c->held[c->nheld]);
	}
}

LOCAL void ptr_report( HIDDEV *d, CONST UB *r, INT len )
{
	T_HIDEV	ev;
	UINT	buttons;
	INT	i;
	BOOL	moved;
	PTRIN	in;

	cc_report(d, r, len);
	if ( d->cc_only ) {
		return;
	}
	if ( dg_report(d, r, len) ) {
		return;				/* a pen's or a touch screen's */
	}
	if ( !ptr_decode(d, r, len, &in) ) {
		return;				/* a report of another kind */
	}
	knl_memset(&ev, 0, sizeof(ev));
	ev.when = now_ns();
	buttons = in.buttons;
	moved = ptr_place(d, &d->x, &d->y, in.x, in.y, FALSE);
	/* the main button on the right: the first two change places */
	if ( hid_attr.main != 0 ) {
		buttons = ( buttons & ~3U ) | ( ( buttons & 1U ) << 1 ) | ( ( buttons & 2U ) >> 1 );
	}
	/* the middle button as a double press of the main one */
	if ( hid_attr.middbl ) {
		BOOL	mid = ( buttons & 4U ) != 0;

		buttons &= ~4U;
		if ( mid && !d->mid && ( ptr_buttons & 1U ) == 0 ) {
			for ( i = 0; i < 2; i++ ) {
				ev.x = ptr_x;
				ev.y = ptr_y;
				ev.type = HID_EV_BTN_DOWN;
				ev.code = 0;
				ring_put(d, &ev);
				ev.type = HID_EV_BTN_UP;
				ring_put(d, &ev);
			}
		}
		d->mid = mid;
	}
	ptr_emit(d, &ev, moved, buttons, TCH_NO);

	/* the wheels: a notch or more turned since the last report */
	if ( in.wheel != 0 ) {
		ev.type = HID_EV_WHEEL;
		ev.code = HID_WHEEL_V;
		ev.dz = in.wheel;
		ring_put(d, &ev);
	}
	if ( in.pan != 0 ) {
		ev.type = HID_EV_WHEEL;
		ev.code = HID_WHEEL_H;
		ev.dz = in.pan;
		ring_put(d, &ev);
	}
}

/* ---------------------------------------------------------------- pens and touch screens */

/*
 * Whether a pen of any interface is in range. While one is, a touch
 * screen's contacts are taken as a hand resting on the screen to write,
 * not as touches.
 */
LOCAL BOOL pen_near( void )
{
	INT	i, k;

	for ( i = 0; i < HID_N; i++ ) {
		HIDDEV	*h = HID_AT(i);

		if ( !h->used || h->dg == NULL ) continue;
		for ( k = 0; k < h->dg->n; k++ ) {
			if ( h->dg->a[k].kind == DGK_PEN && h->dg->a[k].near ) return TRUE;
		}
	}
	return FALSE;
}

/*
 * A pen's report: In Range is the pen over the screen, which moves the
 * pointer without a button; Tip Switch (or Eraser, the other end
 * touching) holds a button for as long as it touches -- the second
 * button when the barrel switch was held as it touched, the main one
 * otherwise. Leaving the range lets go of whatever it held.
 */
LOCAL void dg_pen( HIDDEV *d, DGAPP *a, CONST UB *r, INT len )
{
	T_HIDEV		ev;
	DGSLOT		*s = &a->s[0];
	BOOL		tip, near, moved = FALSE;
	UINT		btn;

	knl_memset(&ev, 0, sizeof(ev));
	ev.when = now_ns();
	tip = ( s->tip.size > 0 && get_field(r, len, &s->tip) != 0 )
	   || ( a->eraser.size > 0 && get_field(r, len, &a->eraser) != 0 );
	near = ( s->inrange.size > 0 ) ? ( get_field(r, len, &s->inrange) != 0 ) : TRUE;
	near = near || tip;
	a->near = near;
	if ( !near ) {
		a->pen_btn = 0;
		ptr_emit(d, &ev, FALSE, 0, TCH_NO);
		return;
	}
	moved = ptr_place(d, &s->x, &s->y, get_field(r, len, &s->x), get_field(r, len, &s->y), FALSE);
	if ( !tip ) {
		a->pen_btn = 0;
	} else if ( a->pen_btn == 0 ) {
		BOOL	barrel = ( a->barrel.size > 0 && get_field(r, len, &a->barrel) != 0 );

		a->pen_btn = barrel ? 2U : 1U;
	}
	btn = a->pen_btn;
	ptr_emit(d, &ev, moved, btn, TCH_NO);
}

/* The first finger's touch, move or let go, as the pointer's main button */
LOCAL void dg_touch_out( HIDDEV *d, DGAPP *a, BOOL on, INT rx, INT ry, UB tch )
{
	T_HIDEV	ev;
	BOOL	moved = FALSE;

	knl_memset(&ev, 0, sizeof(ev));
	ev.when = now_ns();
	d->tdiag10 = a->diag10;
	if ( on ) {
		moved = ptr_place(d, &a->s[0].x, &a->s[0].y, rx, ry, TRUE);
	}
	if ( a->shown < HID_SHOW_REPORTS && ( on != ( d->buttons != 0 ) ) ) {
		a->shown++;
		USB_LOG("usb: hid: %d touch: %s, contact %d at %d,%d (%d,%d on the screen)\n",
			(INT)d->id, on ? "down" : ( tch == TCH_CANCEL ? "gone, not a touch"
						 : tch == TCH_MULTI ? "cut, another finger" : "up" ),
			a->prim, rx, ry, ptr_x, ptr_y);
	}
	ptr_emit(d, &ev, moved, on ? 1U : 0U, tch);
}

/* Two fingers' state passed to the shaping: where their midpoint is and how far apart they are */
LOCAL void tw_put( HIDDEV *d, DGAPP *a, UINT code, INT x, INT y, INT dist )
{
	T_HIDEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_TWO;
	ev.code = code;
	ev.x = x;
	ev.y = y;
	ev.dz = dist;
	ev.when = now_ns();
	d->tdiag10 = a->diag10;
	ring_put_ptr(d, &ev, TCH_TWO);
}

/* Whether two fingers are taken together: ユーザ環境設定's 二本指スクロール (LK_TCH_SCR) */
LOCAL BOOL two_on( void )
{
	return (BOOL)( hid_attr.tch_noscr == 0 );
}

/*
 * The frame of a touch screen whose fingers are followed together
 * (design 6.4.1): two touching are a pair, whose midpoint and distance
 * go to the shaping each frame (two_shape), which makes them a scroll
 * or nothing; a third touching ends the pair, and the pair begins
 * again when two are left. It goes on until no finger touches, so a
 * finger left on the screen after the others let go is never the
 * pointer. A pen coming near ends it as not a touch.
 */
LOCAL void dg_multi( HIDDEV *d, DGAPP *a, BOOL pen )
{
	INT	n = a->n_on;

	if ( pen || n == 0 ) {
		tw_put(d, a, pen ? TW_CANCEL : TW_END, a->m_x, a->m_y, 0);
		a->multi = FALSE;
		a->m_pair = FALSE;
		a->m_n = 0;
		return;
	}
	if ( n == 2 && a->t_n >= 2 ) {
		INT	ia = ( a->t_id[0] < a->t_id[1] ) ? a->t_id[0] : a->t_id[1];
		INT	ib = ( a->t_id[0] < a->t_id[1] ) ? a->t_id[1] : a->t_id[0];
		INT	xa = abs_scale(a->t_x[0], &a->s[0].x, scr_w), ya = abs_scale(a->t_y[0], &a->s[0].y, scr_h);
		INT	xb = abs_scale(a->t_x[1], &a->s[0].x, scr_w), yb = abs_scale(a->t_y[1], &a->s[0].y, scr_h);
		INT	dx = xa - xb, dy = ya - yb;
		INT	dist = isqrt((UD)( dx * dx ) + (UD)( dy * dy ));

		a->m_x = ( xa + xb ) / 2;
		a->m_y = ( ya + yb ) / 2;
		if ( a->m_pair && a->m_a == ia && a->m_b == ib ) {
			tw_put(d, a, TW_MOVE, a->m_x, a->m_y, dist);
		} else {
			a->m_pair = TRUE;
			a->m_a = ia;
			a->m_b = ib;
			tw_put(d, a, TW_DOWN, a->m_x, a->m_y, dist);
		}
	} else if ( n > 2 ) {
		if ( a->m_pair || a->m_n <= 2 ) {
			tw_put(d, a, TW_MANY, a->m_x, a->m_y, 0);
		}
		a->m_pair = FALSE;
	} else if ( a->m_pair ) {
		tw_put(d, a, TW_UP, a->m_x, a->m_y, 0);
		a->m_pair = FALSE;
	}
	a->m_n = n;
}

/* The fingers' midpoint before there is a pair: the first two touching */
LOCAL void multi_start( DGAPP *a )
{
	INT	k = ( a->t_n >= 2 ) ? 2 : a->t_n, i, x = 0, y = 0;

	for ( i = 0; i < k; i++ ) {
		x += abs_scale(a->t_x[i], &a->s[0].x, scr_w);
		y += abs_scale(a->t_y[i], &a->s[0].y, scr_h);
	}
	a->m_x = ( k > 0 ) ? x / k : ptr_x;
	a->m_y = ( k > 0 ) ? y / k : ptr_y;
	a->multi = TRUE;
	a->m_pair = FALSE;
	a->m_n = 0;
}

/*
 * A frame of a touch screen's contacts is all in: the first finger is
 * followed by its Contact Identifier while it touches. It lets go when
 * it no longer touches, or is no longer in the frame; one taken for a
 * palm (Confidence 0) or touching while a pen is near goes as not a
 * touch at all. A new first finger is taken only when no contact was
 * touching in the frame before, as Windows takes its primary contact:
 * a second finger left on the screen does not become the pointer.
 * With two fingers taken together, a second finger touching cuts the
 * first one's press (TCH_MULTI), and the fingers are followed together
 * from then on (dg_multi); so are two that touch in the same frame.
 */
LOCAL void dg_frame( HIDDEV *d, DGAPP *a )
{
	BOOL	pen = pen_near();

	if ( a->multi ) {
		dg_multi(d, a, pen);
	} else if ( a->prim >= 0 ) {
		if ( a->p_seen && a->p_on && !pen && a->n_on >= 2 && two_on() ) {
			dg_touch_out(d, a, FALSE, a->p_x, a->p_y, TCH_MULTI);
			a->prim = -1;
			multi_start(a);
			dg_multi(d, a, FALSE);
		} else if ( a->p_seen && a->p_on && !pen ) {
			dg_touch_out(d, a, TRUE, a->p_x, a->p_y, TCH_TOUCH);
		} else {
			dg_touch_out(d, a, FALSE, a->p_x, a->p_y,
				     ( pen || ( a->p_seen && a->p_palm ) ) ? TCH_CANCEL : TCH_TOUCH);
			a->prim = -1;
		}
	} else if ( a->have_cand && a->was_on == 0 && !pen ) {
		if ( a->n_on >= 2 && two_on() ) {
			multi_start(a);
			dg_multi(d, a, FALSE);
		} else {
			a->prim = a->cand;
			dg_touch_out(d, a, TRUE, a->cand_x, a->cand_y, TCH_TOUCH);
		}
	}
	a->was_on = a->n_on;
	a->expect = a->got = 0;
	a->p_seen = a->p_on = a->p_palm = FALSE;
	a->n_on = 0;
	a->t_n = 0;
	a->have_cand = FALSE;
}

/*
 * One contact of the frame being taken: the first finger's place while
 * it is followed, else the first contact touching, which may become the
 * first finger when the frame is all in (dg_frame). It touches when its
 * tip is down and it is not taken for a palm. Every contact touching is
 * kept as well, for two fingers.
 */
LOCAL void dg_contact( DGAPP *a, INT id, BOOL tip, BOOL conf, INT x, INT y )
{
	BOOL	on = ( tip && conf );

	if ( on ) {
		a->n_on++;
		if ( a->t_n < HID_DG_KEEP ) {
			a->t_id[a->t_n] = id;
			a->t_x[a->t_n] = x;
			a->t_y[a->t_n] = y;
			a->t_n++;
		}
	}
	if ( a->prim >= 0 && id == a->prim ) {
		a->p_seen = TRUE;
		a->p_on = on;
		a->p_palm = ( tip && !conf );
		a->p_x = x;
		a->p_y = y;
	} else if ( on && !a->have_cand ) {
		a->have_cand = TRUE;
		a->cand = id;
		a->cand_x = x;
		a->cand_y = y;
	}
}

/*
 * A touch screen's report: the contacts in it, as many as the frame
 * still has (Contact Count of its first report; every contact of the
 * report when there is no count), each touching when its Tip Switch is
 * set and its Confidence not cleared.
 */
LOCAL void dg_touch( HIDDEV *d, DGAPP *a, CONST UB *r, INT len )
{
	INT	i;

	if ( a->count.size > 0 ) {
		INT	cc = get_field(r, len, &a->count);

		if ( cc > 0 ) {
			if ( a->got > 0 ) {
				dg_frame(d, a);		/* the last frame was cut short */
			}
			a->expect = cc;
			a->got = 0;
		} else if ( a->got >= a->expect ) {
			a->expect = 0;		/* a frame with no contact */
		}
	} else {
		a->expect = a->nslot;
		a->got = 0;
	}
	for ( i = 0; i < a->nslot && a->got < a->expect; i++ ) {
		DGSLOT	*s = &a->s[i];
		BOOL	tip, conf;
		INT	id;

		a->got++;
		if ( s->tip.size > 0 ) {
			tip = ( get_field(r, len, &s->tip) != 0 );
		} else {
			tip = ( s->inrange.size > 0 ) ? ( get_field(r, len, &s->inrange) != 0 ) : TRUE;
		}
		conf = ( s->conf.size > 0 ) ? ( get_field(r, len, &s->conf) != 0 ) : TRUE;
		id = ( s->cid.size > 0 ) ? get_field(r, len, &s->cid) : i;
		dg_contact(a, id, tip, conf, get_field(r, len, &s->x), get_field(r, len, &s->y));
	}
	if ( a->got >= a->expect ) {
		dg_frame(d, a);
	}
}

/* A report of a pen or a touch screen of d's; FALSE when it is neither */
LOCAL BOOL dg_report( HIDDEV *d, CONST UB *r, INT len )
{
	INT	k;

	if ( d->dg == NULL || d->boot ) {
		return FALSE;
	}
	for ( k = 0; k < d->dg->n; k++ ) {
		DGAPP	*a = &d->dg->a[k];

		if ( a->rid != 0 ) {
			if ( len < 1 || r[0] != a->rid ) continue;
			r++;
			len--;
		}
		if ( a->kind == DGK_PEN ) {
			dg_pen(d, a, r, len);
		} else {
			dg_touch(d, a, r, len);
		}
		return TRUE;
	}
	return FALSE;
}

/* Whether an input field is one a pointer is made of: its axes, wheels and buttons, a pen's and a finger's */
LOCAL BOOL pointer_usage( UW u )
{
	switch ( u ) {
	case USAGE_X: case USAGE_Y: case USAGE_WHEEL: case USAGE_PAN:
	case DG_INRANGE: case DG_INVERT: case DG_TIP: case DG_BARREL: case DG_ERASER:
	case DG_CONFIDENCE: case DG_CONTACTID: case DG_CONTACTCOUNT: case DG_BARREL2:
		return TRUE;
	}
	return (BOOL)( ( u >> 16 ) == USAGE_PAGE_BUTTON );
}

/* Whether a feature field is one set when a pen or a touch screen is taken */
LOCAL BOOL mode_usage( UW u )
{
	return (BOOL)( u == DG_INPUTMODE || u == DG_SURFACESW || u == DG_BUTTONSW || u == DG_LATENCY );
}

/*
 * Wacom's own pages, read as the usages they stand for: on 0xFF0D a
 * usage 0x00nn is the digitizer page's nn and 0x01nn the generic desktop
 * page's (0x0130 X), and so on 0xFF00 with the digitizer page under
 * 0x00. The sub-pages Wacom keeps for itself (pads, buttons, its
 * digitizer and digitizer information) and the few usages that are
 * its own are left as they are.
 */
LOCAL UW wacom_usage( UW u )
{
	UW	page = u >> 16, sub = ( u >> 8 ) & 0xff, nn = u & 0xff;

	if ( page == 0xff0d ) {
		if ( sub == 0x04 || sub == 0x09 || sub == 0x0d || sub == 0x10 ) return u;
		switch ( u & 0xffff ) {
		case 0x0036: case 0x005c: case 0x005d: case 0x0077: case 0x0132: case 0x0136:
		case 0x0137: case 0x0138: case 0x0139: case 0x01d0: case 0x0220:
			return u;
		}
		return ( ( sub == 0 ? USAGE_PAGE_DIGI : sub ) << 16 ) | nn;
	}
	if ( page == 0xff00 ) {
		if ( ( u & 0xffff ) == 0x01d0 ) return u;
		return ( ( sub == 0 ? USAGE_PAGE_DIGI : sub ) << 16 ) | nn;
	}
	return u;
}

/*
 * The global items in force: what Push saves and Pop brings back
 */
typedef struct {
	UW	page;
	INT	lmin, lmax;
	INT	rsize, rcount;
	INT	rid;
	INT	pmin, pmax;		/* Physical Minimum and Maximum */
	INT	uexp;			/* Unit Exponent */
	UW	unit;
} HIDGLOBAL;

/*
 * How long a field's range is on the device, in tenths of a millimetre,
 * from its Physical Minimum and Maximum (the logical ones when the
 * physical are not given) and its Unit: centimetres (0x11) or inches
 * (0x13), times ten to the Unit Exponent. 0 when no length is said.
 */
LOCAL INT phys_ext10( CONST HIDGLOBAL *g )
{
	D	v = ( g->pmax != g->pmin ) ? (D)g->pmax - g->pmin : (D)g->lmax - g->lmin;
	INT	e = g->uexp;

	if ( v <= 0 ) return 0;
	if ( g->unit == 0x11 ) {
		e += 2;				/* centimetres into tenths of a millimetre */
	} else if ( g->unit == 0x13 ) {
		v *= 254;			/* inches into tenths of a millimetre */
	} else {
		return 0;
	}
	for ( ; e > 0 && v < 100000000; e-- ) v *= 10;
	for ( ; e < 0; e++ ) v /= 10;
	return ( e == 0 && v > 0 && v < 100000000 ) ? (INT)v : 0;
}

/*
 * What a report descriptor is read into: the variable data input fields
 * a pointer is made of (in, at most maxin), the feature fields set when
 * a pen or a touch screen is taken (ft), the report IDs its inputs
 * have (rids, a bit each), and the length in bits of each input and
 * feature report without its ID byte (bits, fbits, 256 each).
 */
typedef struct {
	HIDFIELD	*in;
	INT		nin, maxin;
	HIDFIELD	*ft;
	INT		nft, maxft;
	UB		*rids;
	INT		*bits, *fbits;
	BOOL		wacom;		/* Wacom's pages read as the usages they stand for */
	BOOL		digi;		/* the digitizer page is used */
	BOOL		touchpad;	/* it has a touch pad, which is not read here */
	HIDCC		*cc;		/* the consumer page's keys it declares */
	UW		apps[8];	/* its application collections, for the console */
	INT		napps;
} HIDPARSE;

/*
 * A report descriptor into its fields, bit offsets counted per report
 * ID after the ID byte. Answers how many input fields there are.
 *
 * Only the fields a pointer is made of are kept: a mouse whose buttons
 * can be given keys declares a keyboard of a hundred and more one-bit
 * fields beside it, and vendors' own pages as well, which would fill
 * the room before its X and Y came. Every field still moves the offset
 * of those after it, however many there are.
 *
 * A movement is a signed number whatever its Logical Minimum says: a
 * descriptor that gives 0 as the least of a relative axis still sends
 * a move to the left as a negative number, and read as unsigned that
 * would send the pointer across the screen.
 *
 * Each field is told the application collection it is in, and the
 * finger or stylus collection, by number, so that the contacts of a
 * touch screen's report can be told apart.
 */
LOCAL INT hid_parse( CONST UB *dsc, INT n, HIDPARSE *ps )
{
	HIDGLOBAL	g, stack[HID_PUSH_MAX];
	UW		usages[16], umin = 0, umax = 0, app = 0;
	INT		nus = 0, nst = 0, i = 0, k;
	BOOL		range = FALSE;
	INT		*off = ps->bits;	/* the input bit offset per report ID */
	INT		*foff = ps->fbits;	/* the feature bit offset per report ID */
	INT		cstk[HID_COLL_MAX], kstk[HID_COLL_MAX], depth = 0, coll = -1, ck = CK_NONE, ncoll = 0;

	knl_memset(off, 0, sizeof(INT) * 256);
	knl_memset(foff, 0, sizeof(INT) * 256);
	knl_memset(&g, 0, sizeof(g));
	ps->nin = ps->nft = 0;
	ps->digi = ps->touchpad = FALSE;
	ps->napps = 0;
	while ( i < n ) {
		UB	b = dsc[i];
		INT	sz, type, tag;
		UW	val = 0;
		INT	sval;

		if ( b == 0xfe ) {		/* a long item */
			if ( i + 1 >= n ) {
				break;
			}
			i += 3 + dsc[i + 1];
			continue;
		}
		sz = b & 3;
		if ( sz == 3 ) {
			sz = 4;
		}
		if ( i + 1 + sz > n ) {
			break;
		}
		for ( k = 0; k < sz; k++ ) {
			val |= (UW)dsc[i + 1 + k] << (8 * k);
		}
		sval = (INT)val;
		if ( sz == 1 && (val & 0x80) != 0 ) sval = (INT)(val | 0xffffff00);
		if ( sz == 2 && (val & 0x8000) != 0 ) sval = (INT)(val | 0xffff0000);
		type = (b >> 2) & 3;
		tag  = b >> 4;
		i += 1 + sz;

		if ( type == 1 ) {		/* global */
			switch ( tag ) {
			case 0:
				g.page = val;
				if ( val == USAGE_PAGE_DIGI ) ps->digi = TRUE;
				break;
			case 1: g.lmin = sval; break;
			case 2: g.lmax = ( g.lmin < 0 ) ? sval : (INT)val; break;
			case 3: g.pmin = sval; break;
			case 4: g.pmax = ( g.pmin < 0 ) ? sval : (INT)val; break;
			case 5:
				/* a nibble of two's complement, as most devices give it, or the whole value */
				g.uexp = ( ( val & ~0xfU ) == 0 ) ? ( ( val & 8 ) ? (INT)val - 16 : (INT)val ) : sval;
				break;
			case 6: g.unit = val; break;
			case 7: g.rsize = ( val > HID_BITS_MAX ) ? HID_BITS_MAX : (INT)val; break;
			case 8: g.rid = (INT)(val & 0xff); break;
			case 9: g.rcount = ( val > HID_BITS_MAX ) ? HID_BITS_MAX : (INT)val; break;
			case 10:			/* Push */
				if ( nst < HID_PUSH_MAX ) {
					stack[nst] = g;
				}
				nst++;
				break;
			case 11:			/* Pop */
				if ( nst > 0 ) {
					nst--;
					if ( nst < HID_PUSH_MAX ) {
						g = stack[nst];
					}
				}
				break;
			}
			continue;
		}
		if ( type == 2 ) {		/* local */
			if ( tag == 0 && nus < 16 ) {
				usages[nus++] = ( sz == 4 ) ? val : (g.page << 16) | val;
			}
			if ( tag == 1 ) {
				umin = ( sz == 4 ) ? val : (g.page << 16) | val;
				range = TRUE;
			}
			if ( tag == 2 ) {
				umax = ( sz == 4 ) ? val : (g.page << 16) | val;
				range = TRUE;
			}
			continue;
		}
		if ( type != 0 ) {
			continue;
		}
		if ( tag == 0xa ) {		/* Collection */
			UW	u = ( nus > 0 ) ? usages[0] : ( range ? umin : 0 );

			if ( ps->wacom ) u = wacom_usage(u);
			if ( depth == 0 ) {
				app = u;
				if ( ps->napps < 8 ) ps->apps[ps->napps++] = u;
				if ( u == DG_TOUCHPAD ) ps->touchpad = TRUE;
			}
			if ( depth < HID_COLL_MAX ) {
				cstk[depth] = coll;
				kstk[depth] = ck;
			}
			depth++;
			if ( u == DG_FINGER || u == DG_STYLUS ) {
				coll = ncoll++;
				ck = ( u == DG_FINGER ) ? CK_FINGER : CK_STYLUS;
			}
		} else if ( tag == 0xc ) {	/* End Collection */
			if ( depth > 0 ) {
				depth--;
				if ( depth < HID_COLL_MAX ) {
					coll = cstk[depth];
					ck = kstk[depth];
				}
			}
		} else if ( tag == 8 || tag == 0xb ) {	/* Input, Feature */
			BOOL	input = ( tag == 8 );
			INT	*o = input ? off : foff;

			if ( input ) {
				ps->rids[g.rid >> 3] |= (UB)( 1 << ( g.rid & 7 ) );
			}
			/* data, an array: its values are usages, some perhaps keys */
			if ( input && ( val & 3 ) == 0 && g.rsize >= 1 && g.rsize <= 32 && g.rcount > 0
			  && ps->cc != NULL && ps->cc->narr < HID_CC_ARRAYS ) {
				BOOL	any = FALSE;
				INT	m;

				if ( range ) {
					for ( m = 0; m < (INT)( sizeof(cc_keys) / sizeof(cc_keys[0]) ); m++ ) {
						UW	cu = ( USAGE_PAGE_CONSUMER << 16 ) | cc_keys[m].usage;

						any = any || ( cu >= umin && cu <= umax );
					}
				} else {
					for ( m = 0; m < nus; m++ ) {
						any = any || ( cc_key(usages[m]) != 0 );
					}
				}
				if ( any ) {
					CCARRAY	*ca = &ps->cc->a[ps->cc->narr++];

					ca->rid = g.rid;
					ca->off = o[g.rid];
					ca->size = g.rsize;
					ca->count = g.rcount;
					ca->lmin = g.lmin;
					ca->lmax = g.lmax;
					ca->range = range;
					ca->umin = umin;
					ca->nus = nus;
					for ( m = 0; m < nus; m++ ) ca->us[m] = usages[m];
				}
			}
			for ( k = 0; k < g.rcount && k < HID_ITEMS_MAX; k++ ) {
				UW		u;
				HIDFIELD	*f;

				if ( range ) {
					u = ( umin + k <= umax ) ? umin + k : umax;
				} else {
					u = ( k < nus ) ? usages[k]
					    : ( nus > 0 ? usages[nus - 1] : 0 );
				}
				if ( ps->wacom ) u = wacom_usage(u);
				/* data, not constant, and variable, not an array */
				if ( (val & 3) != 2 || g.rsize < 1 || g.rsize > 32 ) {
					continue;
				}
				if ( input && ps->cc != NULL && cc_key(u) != 0 ) {
					if ( ps->cc->nvar < HID_CC_VARS ) {
						CCVAR	*cv = &ps->cc->v[ps->cc->nvar++];

						cv->rid = g.rid;
						cv->off = o[g.rid] + k * g.rsize;
						cv->size = g.rsize;
						cv->key = cc_key(u);
					}
					continue;
				}
				if ( input && pointer_usage(u) && ps->nin < ps->maxin ) {
					f = &ps->in[ps->nin++];
				} else if ( !input && mode_usage(u) && ps->nft < ps->maxft ) {
					f = &ps->ft[ps->nft++];
				} else {
					continue;
				}
				f->off = o[g.rid] + k * g.rsize;
				f->size = g.rsize;
				f->lmin = g.lmin;
				f->lmax = g.lmax;
				f->rel = ( (val & 4) != 0 );
				f->sgn = ( g.lmin < 0 )
				      || ( f->rel && g.rsize > 1 && (u >> 16) != USAGE_PAGE_BUTTON );
				f->rid = g.rid;
				f->usage = u;
				f->app = app;
				f->coll = coll;
				f->ck = ck;
				f->ext10 = phys_ext10(&g);
			}
			{
				D	end = (D)o[g.rid] + (D)g.rsize * g.rcount;

				o[g.rid] = ( end > HID_BITS_MAX * 64 ) ? HID_BITS_MAX * 64 : (INT)end;
			}
		}
		if ( tag == 8 || tag == 9 || tag == 0xb || tag == 0xa || tag == 0xc ) {
			nus = 0;
			range = FALSE;
		}
	}
	return ps->nin;
}

/*
 * The X and Y of one report, the buttons and wheels beside them. A
 * movement is taken before a position, so that a mouse that also
 * declares an absolute pointer (for a remote, a pen) is read as the
 * mouse it is.
 */
LOCAL BOOL pick_pointer( HIDDEV *h, CONST HIDFIELD *f, INT nf )
{
	HIDFIELD	btn, x, y, wheel, pan;
	INT		nbtn = 0, i, j, pass;

	knl_memset(&btn, 0, sizeof(btn));
	knl_memset(&x, 0, sizeof(x));
	knl_memset(&y, 0, sizeof(y));
	knl_memset(&wheel, 0, sizeof(wheel));
	knl_memset(&pan, 0, sizeof(pan));
	for ( pass = 0; pass < 2 && x.size == 0; pass++ ) {
		for ( i = 0; i < nf && x.size == 0; i++ ) {
			if ( f[i].usage != USAGE_X || ( pass == 0 && !f[i].rel ) ) continue;
			for ( j = 0; j < nf; j++ ) {
				if ( f[j].usage == USAGE_Y && f[j].rid == f[i].rid ) {
					x = f[i];
					y = f[j];
					break;
				}
			}
		}
	}
	if ( x.size == 0 || y.size == 0 ) {
		return FALSE;
	}
	for ( i = 0; i < nf; i++ ) {
		CONST HIDFIELD	*p = &f[i];

		if ( p->rid != x.rid ) continue;
		if ( (p->usage >> 16) == USAGE_PAGE_BUTTON && p->size == 1 ) {
			if ( nbtn == 0 ) {
				btn = *p;
			}
			if ( p->off == btn.off + nbtn ) {
				nbtn++;
			}
		}
		if ( p->usage == USAGE_WHEEL && wheel.size == 0 ) wheel = *p;
		if ( p->usage == USAGE_PAN && pan.size == 0 ) pan = *p;
	}
	h->report_id = x.rid;
	h->x = x;
	h->y = y;
	h->btn = btn;
	h->nbtn = nbtn;
	if ( wheel.size > 1 ) {
		h->wheel = wheel;
		h->wheel.sgn = TRUE;		/* a movement, turned either way */
	}
	if ( pan.size > 1 ) {
		h->pan = pan;
		h->pan.sgn = TRUE;
	}

	return TRUE;
}

/*
 * What a field is of: a pen (DGK_PEN), a touch screen (DGK_TOUCH), or
 * neither (0). A pen's field is in a
 * stylus collection or in a Pen or Digitizer application, a finger's in
 * a finger collection or a Touch Screen application. A touch pad's is
 * neither: it is left as the mouse it says it is besides.
 */
LOCAL INT dg_kind( CONST HIDFIELD *f )
{
	if ( f->app == DG_TOUCHPAD ) return 0;
	if ( f->ck == CK_FINGER || f->app == DG_TOUCHSCREEN ) return DGK_TOUCH;
	if ( f->ck == CK_STYLUS || f->app == DG_PEN || f->app == DG_DIGITIZER ) return DGK_PEN;
	return 0;
}

/*
 * The pens and touch screens of a descriptor's fields, each one input
 * report: its contacts (a finger collection each, in the order they
 * come; a pen's is one), and where their switches and places are.
 * Answers how many were found; one needs X, Y and a Tip Switch or In
 * Range to be taken.
 */
LOCAL INT pick_digitizer( HIDDG *dg, CONST HIDFIELD *f, INT nf )
{
	INT	colls[HID_DG_APPS][HID_DG_SLOTS];
	INT	i, k, n = 0, slot;

	knl_memset(dg, 0, sizeof(*dg));
	for ( i = 0; i < nf; i++ ) {
		INT	kind = dg_kind(&f[i]);
		DGAPP	*a = NULL;
		DGSLOT	*s;

		if ( kind == 0 || f[i].rel ) continue;
		for ( k = 0; k < n; k++ ) {
			if ( dg->a[k].rid == f[i].rid && dg->a[k].kind == kind ) {
				a = &dg->a[k];
				break;
			}
		}
		if ( a == NULL ) {
			if ( n >= HID_DG_APPS ) continue;
			a = &dg->a[n];
			a->rid = f[i].rid;
			a->kind = kind;
			n++;
		}
		/* the contact it belongs to: the collection's place among the report's */
		slot = 0;
		if ( kind == DGK_TOUCH && f[i].coll >= 0 ) {
			INT	*c = colls[a - dg->a];

			for ( slot = 0; slot < a->nslot && c[slot] != f[i].coll; slot++ ) ;
			if ( slot == a->nslot ) {
				if ( a->nslot >= HID_DG_SLOTS ) continue;
				c[a->nslot++] = f[i].coll;
			}
		} else if ( a->nslot == 0 ) {
			colls[a - dg->a][0] = -1;
			a->nslot = 1;
		}
		s = &a->s[slot];
		switch ( f[i].usage ) {
		case USAGE_X:		if ( s->x.size == 0 ) s->x = f[i]; break;
		case USAGE_Y:		if ( s->y.size == 0 ) s->y = f[i]; break;
		case DG_TIP:		if ( s->tip.size == 0 ) s->tip = f[i]; break;
		case DG_CONFIDENCE:	if ( s->conf.size == 0 ) s->conf = f[i]; break;
		case DG_CONTACTID:	if ( s->cid.size == 0 ) s->cid = f[i]; break;
		case DG_INRANGE:	if ( s->inrange.size == 0 ) s->inrange = f[i]; break;
		case DG_CONTACTCOUNT:	if ( a->count.size == 0 ) a->count = f[i]; break;
		case DG_BARREL:		if ( a->barrel.size == 0 ) a->barrel = f[i]; break;
		case DG_BARREL2:	if ( a->barrel2.size == 0 ) a->barrel2 = f[i]; break;
		case DG_INVERT:		if ( a->invert.size == 0 ) a->invert = f[i]; break;
		case DG_ERASER:		if ( a->eraser.size == 0 ) a->eraser = f[i]; break;
		}
	}
	/* the contacts kept are those with a place, and the first is to have a switch */
	for ( k = 0; k < n; ) {
		DGAPP	*a = &dg->a[k];
		INT	m = 0;

		for ( i = 0; i < a->nslot; i++ ) {
			if ( a->s[i].x.size > 0 && a->s[i].y.size > 0 ) {
				if ( m != i ) a->s[m] = a->s[i];
				m++;
			}
		}
		a->nslot = m;
		if ( m == 0 || ( a->s[0].tip.size == 0 && a->s[0].inrange.size == 0 ) ) {
			for ( i = k; i + 1 < n; i++ ) dg->a[i] = dg->a[i + 1];
			n--;
			continue;
		}
		a->prim = -1;
		if ( a->s[0].x.ext10 > 0 && a->s[0].y.ext10 > 0 ) {
			a->diag10 = isqrt((UD)a->s[0].x.ext10 * a->s[0].x.ext10
					  + (UD)a->s[0].y.ext10 * a->s[0].y.ext10);
		}
		k++;
	}
	dg->n = n;
	return n;
}

/* The length in bytes of input report rid, its ID byte included; 0 none */
LOCAL INT report_bytes( CONST INT *bits, INT rid )
{
	if ( bits[rid] == 0 ) {
		return 0;
	}

	return ( bits[rid] + 7 ) / 8 + ( rid != 0 ? 1 : 0 );
}

/*
 * The feature report that puts a pen or a touch screen into the mode it
 * is read in, into buf (max bytes): the first Input Mode the descriptor
 * declares set to 2, the multi-input device mode, and the rest of that
 * report 0 but Surface Switch and Button Switch, which are 1 (Latency
 * Mode 0, the normal latency). Until it is set, a touch screen of
 * Microsoft's protocol may send only its mouse's reports, or nothing.
 * Answers its length, its ID in
 * *p_rid; 0 when there is none to send, or when it has a touch pad,
 * whose mode is the mouse it is read as.
 */
LOCAL INT dg_mode_report( HIDPARSE *ps, UB *buf, INT max, INT *p_rid )
{
	INT	i, rid = -1, len;

	if ( ps->touchpad ) {
		return 0;
	}
	for ( i = 0; i < ps->nft; i++ ) {
		if ( ps->ft[i].usage == DG_INPUTMODE ) {
			rid = ps->ft[i].rid;
			break;
		}
	}
	if ( rid < 0 ) {
		return 0;
	}
	len = ( ps->fbits[rid] + 7 ) / 8 + ( rid != 0 ? 1 : 0 );
	if ( len <= 0 || len > max ) {
		return 0;
	}
	knl_memset(buf, 0, (SZ)len);
	if ( rid != 0 ) buf[0] = (UB)rid;
	for ( i = 0; i < ps->nft; i++ ) {
		CONST HIDFIELD	*f = &ps->ft[i];
		INT		v, b;

		if ( f->rid != rid ) continue;
		switch ( f->usage ) {
		case DG_INPUTMODE:	v = DG_INPUTMODE_TOUCHSCREEN; break;
		case DG_SURFACESW:
		case DG_BUTTONSW:	v = 1; break;
		default:		v = 0; break;
		}
		for ( b = 0; b < f->size && b < 32; b++ ) {
			INT	bit = f->off + b + ( rid != 0 ? 8 : 0 );

			if ( ( v >> b ) & 1 ) buf[bit >> 3] |= (UB)( 1 << ( bit & 7 ) );
		}
	}
	*p_rid = rid;
	return len;
}

/* A descriptor shown on the console, sixteen bytes a line, to tell later what the device declares */
LOCAL void show_desc( INT ifno, CONST UB *rd, INT n )
{
	char	line[8 + 3 * 16 + 8];
	INT	i, k, m;

	USB_LOG("usb: hid: interface %d report descriptor, %d bytes:\n", ifno, n);
	for ( i = 0; i < n; i += 16 ) {
		m = tm_sprintf((UB *)line, (UB *)"  %04x:", i);
		for ( k = i; k < n && k < i + 16; k++ ) {
			m += tm_sprintf((UB *)line + m, (UB *)" %02x", (INT)rd[k]);
		}
		USB_LOG("%s\n", line);
	}
}

/* Where a pen's or a touch screen's report says what, for the console */
LOCAL void show_dg( CONST HIDDEV *h, CONST HIDPARSE *ps )
{
	INT	k, i;

	for ( i = 0; i < ps->napps; i++ ) {
		USB_LOG("usb: hid: interface %d application %d: usage %x\n", (INT)h->ifno, i, (INT)ps->apps[i]);
	}
	for ( k = 0; h->dg != NULL && k < h->dg->n; k++ ) {
		CONST DGAPP	*a = &h->dg->a[k];
		CONST DGSLOT	*s = &a->s[0];

		USB_LOG("usb: hid: interface %d %s, report id %d, %d contacts a report; contact 0: tip bit %d, "
			"in range bit %d, confidence bit %d, id bit %d size %d, x bit %d size %d range %d..%d, "
			"y bit %d size %d range %d..%d; count bit %d size %d; barrel bit %d, eraser bit %d, invert bit %d\n",
			(INT)h->ifno, a->kind == DGK_PEN ? "pen" : "touch screen", a->rid, a->nslot,
			s->tip.size ? s->tip.off : -1, s->inrange.size ? s->inrange.off : -1,
			s->conf.size ? s->conf.off : -1, s->cid.size ? s->cid.off : -1, s->cid.size,
			s->x.off, s->x.size, s->x.lmin, s->x.lmax, s->y.off, s->y.size, s->y.lmin, s->y.lmax,
			a->count.size ? a->count.off : -1, a->count.size,
			a->barrel.size ? a->barrel.off : -1, a->eraser.size ? a->eraser.off : -1,
			a->invert.size ? a->invert.off : -1);
	}
}

/*
 * An interface's report descriptor: its pointer, its pens and touch
 * screens, the report IDs it declares, how long its reports are. FALSE
 * when it has none of them. When mode is not NULL, the feature report
 * that puts its pen or touch screen into the mode it is read in is left
 * there (*p_mlen bytes, at most 64, its ID *p_mrid; 0 bytes none).
 */
LOCAL BOOL hid_read_desc( HIDDEV *h, CONST UB *rd, INT n, BOOL wacom, UB *mode, INT *p_mlen, INT *p_mrid )
{
	HIDPARSE	ps;
	HIDDG		*dg;
	HIDCC		*cc;
	BOOL		found = FALSE;
	INT		k;

	if ( p_mlen != NULL ) *p_mlen = 0;
	knl_memset(&ps, 0, sizeof(ps));
	ps.in = (HIDFIELD *)Kmalloc(sizeof(HIDFIELD) * HID_FIELDS);
	ps.ft = (HIDFIELD *)Kmalloc(sizeof(HIDFIELD) * HID_FFIELDS);
	ps.bits = (INT *)Kmalloc(sizeof(INT) * 256);
	ps.fbits = (INT *)Kmalloc(sizeof(INT) * 256);
	dg = (HIDDG *)Kmalloc(sizeof(HIDDG));
	cc = (HIDCC *)Kcalloc(1, sizeof(HIDCC));
	ps.cc = cc;
	ps.maxin = HID_FIELDS;
	ps.maxft = HID_FFIELDS;
	ps.rids = h->rids;
	ps.wacom = wacom;
	if ( ps.in != NULL && ps.ft != NULL && ps.bits != NULL && ps.fbits != NULL && dg != NULL && cc != NULL ) {
		INT	nf = hid_parse(rd, n, &ps);

		found = pick_pointer(h, ps.in, nf);
		h->rlen = 0;
		for ( k = 0; k < 256; k++ ) {
			if ( report_bytes(ps.bits, k) > h->rlen ) {
				h->rlen = report_bytes(ps.bits, k);
			}
		}
		h->plen = found ? report_bytes(ps.bits, h->report_id) : 0;
		if ( pick_digitizer(dg, ps.in, nf) > 0 ) {
			for ( k = 0; k < dg->n; k++ ) {
				if ( report_bytes(ps.bits, dg->a[k].rid) > h->plen ) {
					h->plen = report_bytes(ps.bits, dg->a[k].rid);
				}
			}
			h->dg = dg;
			dg = NULL;
			found = TRUE;
			if ( mode != NULL && p_mlen != NULL ) {
				*p_mlen = dg_mode_report(&ps, mode, 64, p_mrid);
			}
		}
		if ( cc->nvar + cc->narr > 0 ) {
			INT	m = 0;

			USB_LOG("usb: hid: interface %d consumer keys: %d a bit each, %d arrays%s\n",
				h->ifno, cc->nvar, cc->narr, found ? "" : ", and nothing else");
			h->cc_only = !found;
			for ( k = 0; k < cc->nvar; k++ ) {
				if ( report_bytes(ps.bits, cc->v[k].rid) > m ) m = report_bytes(ps.bits, cc->v[k].rid);
			}
			for ( k = 0; k < cc->narr; k++ ) {
				if ( report_bytes(ps.bits, cc->a[k].rid) > m ) m = report_bytes(ps.bits, cc->a[k].rid);
			}
			if ( m > h->plen ) h->plen = m;
			if ( !found ) show_desc(h->ifno, rd, n);
			h->cc = cc;
			cc = NULL;
			found = TRUE;
		}
		if ( ps.digi || !found ) {
			show_desc(h->ifno, rd, n);
		}
		if ( h->dg != NULL ) {
			show_dg(h, &ps);
		}
	}
	if ( ps.in != NULL ) Kfree(ps.in);
	if ( ps.ft != NULL ) Kfree(ps.ft);
	if ( ps.bits != NULL ) Kfree(ps.bits);
	if ( ps.fbits != NULL ) Kfree(ps.fbits);
	if ( dg != NULL ) Kfree(dg);
	if ( cc != NULL ) Kfree(cc);

	return found;
}

/* The room of an interface's pens, touch screens and consumer keys given back */
LOCAL void dg_forget( HIDDEV *h )
{
	if ( h->dg != NULL ) {
		HIDDG	*dg = h->dg;

		h->dg = NULL;
		Kfree(dg);
	}
	if ( h->cc != NULL ) {
		HIDCC	*cc = h->cc;

		h->cc = NULL;
		Kfree(cc);
	}
}

/*
 * The boot protocol's mouse report: buttons, then a signed byte per
 * axis, and a fourth byte, when the mouse sends one, for its wheel.
 * Whatever the descriptor placed is forgotten, the wheel across too.
 */
LOCAL void boot_mouse( HIDDEV *h )
{
	knl_memset(&h->btn, 0, sizeof(HIDFIELD));
	knl_memset(&h->x, 0, sizeof(HIDFIELD));
	knl_memset(&h->y, 0, sizeof(HIDFIELD));
	knl_memset(&h->wheel, 0, sizeof(HIDFIELD));
	knl_memset(&h->pan, 0, sizeof(HIDFIELD));
	h->report_id = 0;
	h->nbtn = 3;
	h->x.off = 8;   h->x.size = 8;  h->x.rel = TRUE; h->x.sgn = TRUE;
	h->y.off = 16;  h->y.size = 8;  h->y.rel = TRUE; h->y.sgn = TRUE;
	h->wheel.off = 24;  h->wheel.size = 8;  h->wheel.rel = TRUE; h->wheel.sgn = TRUE;
}

#ifdef USE_KTEST
/*
 * For the tests: a report descriptor read as an interface's would be,
 * then reports given to it one after another, lens[i] bytes each, laid
 * end to end in reps. bootable says the interface is of the boot
 * subclass. info[16] gets where the pointer's fields are: report ID,
 * longest report, X offset, size, relative, signed, Y offset, size,
 * buttons, their offset, wheel offset, size, wheel across offset,
 * size, whether it came to be read as the boot protocol, and the
 * length of the pointer's report. out gets
 * four numbers a report: buttons (-1 for a report of another kind),
 * X, Y and wheel. Answers whether a pointer was found.
 */
EXPORT BOOL knl_hid_probe( CONST UB *dsc, INT n, BOOL bootable, INT *info,
			   CONST UB *reps, CONST INT *lens, INT nrep, INT *out )
{
	HIDDEV		h;
	PTRIN		in;
	BOOL		found;
	INT		i;

	knl_memset(&h, 0, sizeof(h));
	found = hid_read_desc(&h, dsc, n, FALSE, NULL, NULL, NULL);
	h.bootable = bootable;
	for ( i = 0; found && i < nrep; i++ ) {
		if ( ptr_decode(&h, reps, lens[i], &in) ) {
			out[4 * i]     = (INT)in.buttons;
			out[4 * i + 1] = in.x;
			out[4 * i + 2] = in.y;
			out[4 * i + 3] = in.wheel;
		} else {
			out[4 * i] = -1;
			out[4 * i + 1] = out[4 * i + 2] = out[4 * i + 3] = 0;
		}
		reps += lens[i];
	}
	info[0]  = h.report_id;
	info[1]  = h.rlen;
	info[2]  = h.x.off;
	info[3]  = h.x.size;
	info[4]  = h.x.rel;
	info[5]  = h.x.sgn;
	info[6]  = h.y.off;
	info[7]  = h.y.size;
	info[8]  = h.nbtn;
	info[9]  = h.btn.off;
	info[10] = h.wheel.off;
	info[11] = h.wheel.size;
	info[12] = h.pan.off;
	info[13] = h.pan.size;
	info[14] = h.boot;
	info[15] = h.plen;
	dg_forget(&h);

	return found;
}

/*
 * For the tests: a pen's or a touch screen's descriptor read as an
 * interface's would be (wacom: Wacom's pages read as the usages they
 * stand for). info[0] gets how many digitizer applications there are,
 * then eight numbers each: its kind (DGK_*), report ID, contacts a
 * report, the first contact's Tip Switch offset, Contact Identifier
 * offset, X offset, the second contact's X offset (-1 none) and the
 * Contact Count offset (-1 none). mode gets the feature report that
 * sets its mode (at most 64 bytes), *p_mlen its length (0 none) and
 * *p_mrid its ID. Answers whether a pointer was found.
 */
EXPORT BOOL knl_hid_probe_dg( CONST UB *dsc, INT n, BOOL wacom, INT *info, UB *mode, INT *p_mlen, INT *p_mrid )
{
	HIDDEV	h;
	BOOL	found;
	INT	k;

	knl_memset(&h, 0, sizeof(h));
	found = hid_read_desc(&h, dsc, n, wacom, mode, p_mlen, p_mrid);
	info[0] = ( h.dg != NULL ) ? h.dg->n : 0;
	for ( k = 0; k < info[0]; k++ ) {
		CONST DGAPP	*a = &h.dg->a[k];
		INT		*o = info + 1 + 8 * k;

		o[0] = a->kind;
		o[1] = a->rid;
		o[2] = a->nslot;
		o[3] = a->s[0].tip.size ? a->s[0].tip.off : -1;
		o[4] = a->s[0].cid.size ? a->s[0].cid.off : -1;
		o[5] = a->s[0].x.off;
		o[6] = ( a->nslot > 1 ) ? a->s[1].x.off : -1;
		o[7] = a->count.size ? a->count.off : -1;
	}
	dg_forget(&h);

	return found;
}
#endif

/* ---------------------------------------------------------------- streams */

/*
 * A pointer's first reports as they came, for the console; a pen's or a
 * touch screen's whole, and more of them, so that one touch, one hold
 * and one stroke can be read back.
 */
LOCAL void show_report( CONST HIDDEV *d, CONST UB *r, INT len )
{
	char	line[3 * HID_REPORT_MAX + 48];
	INT	k, m, most = ( d->dg != NULL ) ? HID_REPORT_MAX : 24;

	if ( d->is_kbd || d->reports >= (UD)( d->dg != NULL ? HID_SHOW_DG : HID_SHOW_REPORTS ) ) {
		return;
	}
	m = tm_sprintf((UB *)line, (UB *)"usb: hid: %d report %d:", (INT)d->id, (INT)d->reports + 1);
	for ( k = 0; k < len && k < most; k++ ) {
		m += tm_sprintf((UB *)line + m, (UB *)" %02x", (INT)r[k]);
	}
	USB_LOG("%s\n", line);
}

LOCAL void hid_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	HIDDEV	*d = (HIDDEV *)ctx;
	UB	r[HID_REPORT_MAX];

	if ( !d->used || err != 0 || len <= 0 ) {
		return;
	}
	if ( len > HID_REPORT_MAX ) {
		len = HID_REPORT_MAX;
	}
	knl_memset(r, 0, sizeof(r));
	knl_dmabuf_read(b, off, r, len);
	show_report(d, r, len);
	if ( d->is_kbd ) {
		kbd_report(d, r);
	} else {
		ptr_report(d, r, len);
	}
	/* counted once its events are out: whoever sees the count finds them there */
	hid_count.reports++;
	d->reports++;
}

/*
 * Keyboards and pointers that come another way than USB (the PC's
 * i8042, HID over I2C, a touch screen's controller on I2C): a slot of
 * their own, fed reports as a USB interface's are, by the caller
 * holding the USB manager's lock as the USB streams do. A keyboard's report is the boot protocol's eight
 * bytes; a pointer's as its report descriptor rd says (NULL: the boot
 * protocol's mouse). Answers the slot's handle (from 1), or an error.
 */
LOCAL HIDDEV *hid_slot( void );

LOCAL INT ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name, UB *mode, INT *p_mlen, INT *p_mrid )
{
	HIDDEV	*h = hid_slot();
	INT	i;

	if ( h == NULL ) {
		return E_NOMEM;
	}
	i = h->slot;
	dg_forget(h);
	knl_memset(h, 0, sizeof(*h));
	h->slot = i;
	for ( i = 0; i < 15 && name != NULL && name[i] != 0; i++ ) h->ext_name[i] = name[i];
	h->ext_name[i] = 0;
	h->is_kbd = kbd;
	if ( !kbd && ( rd == NULL || !hid_read_desc(h, rd, rdlen, FALSE, mode, p_mlen, p_mrid) ) ) {
		boot_mouse(h);
		h->boot = TRUE;
	}
	h->id = ++hid_next_id;
	h->used = TRUE;
	if ( kbd || h->cc_only ) {
		hid_count.keyboards++;
	} else {
		hid_count.pointers++;
	}
	return h->slot + 1;
}

EXPORT INT knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name )
{
	return ext_attach(kbd, rd, rdlen, name, NULL, NULL, NULL);
}

/*
 * A pointer that may be a pen or a touch screen (HID over I2C): as
 * knl_hid_ext_attach, and the feature report that puts it into the mode
 * it is read in left in mode (at most 64 bytes, *p_mlen of them, 0 none;
 * its ID *p_mrid), for the caller to send over its own bus as a USB
 * interface's is sent with SET_REPORT.
 */
EXPORT INT knl_hid_ext_attach_dg( CONST UB *rd, INT rdlen, CONST UB *name, UB *mode, INT *p_mlen, INT *p_mrid )
{
	*p_mlen = 0;
	*p_mrid = 0;
	return ext_attach(FALSE, rd, rdlen, name, mode, p_mlen, p_mrid);
}

/*
 * A device not on USB gone (a Bluetooth keyboard or mouse): what it holds
 * is let go as a USB device's is, and its slot is free for another.
 */
EXPORT void knl_hid_ext_detach( INT hd )
{
	HIDDEV	*d;
	INT	k;

	if ( hd < 1 || hd > HID_N ) {
		return;
	}
	d = HID_AT(hd - 1);
	if ( !d->used || d->dev != NULL ) {
		return;
	}
	cc_let_go(d);
	if ( d->is_kbd ) {
		UB	none[8];

		knl_memset(none, 0, sizeof(none));
		kbd_report(d, none);
		hid_count.keyboards--;
	} else if ( d->cc_only ) {
		hid_count.keyboards--;
	} else {
		T_HIDEV	ev;

		knl_memset(&ev, 0, sizeof(ev));
		ev.when = now_ns();
		ptr_emit(d, &ev, FALSE, 0, TCH_NO);
		hid_count.pointers--;
		for ( k = 0; d->dg != NULL && k < d->dg->n; k++ ) {
			d->dg->a[k].prim = -1;
			d->dg->a[k].multi = FALSE;
		}
	}
	d->used = FALSE;
	dg_forget(d);
}

/* The name of a device that is not on USB, by its id (T_HIDINFO.id); FALSE when it is on USB */
EXPORT BOOL knl_hid_ext_name( UINT id, UB *name, INT max )
{
	INT	i, k;

	for ( i = 0; i < HID_N; i++ ) {
		if ( HID_AT(i)->used && HID_AT(i)->id == id && HID_AT(i)->dev == NULL ) {
			for ( k = 0; k < max - 1 && HID_AT(i)->ext_name[k] != 0; k++ ) name[k] = HID_AT(i)->ext_name[k];
			name[k] = 0;
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT void knl_hid_ext_report( INT hd, CONST UB *r, INT len )
{
	HIDDEV	*d;

	if ( hd < 1 || hd > HID_N ) {
		return;
	}
	d = HID_AT(hd - 1);
	if ( !d->used || d->dev != NULL || len <= 0 ) {
		return;
	}
	show_report(d, r, len);
	if ( d->is_kbd ) {
		UB	k[8];

		knl_memset(k, 0, sizeof(k));
		knl_memcpy(k, r, ( len < 8 ) ? len : 8);
		kbd_report(d, k);
	} else {
		ptr_report(d, r, len);
	}
	hid_count.reports++;
	d->reports++;
}

/*
 * A touch screen whose controller does not speak HID (a Goodix GT911 on
 * I2C). Its contacts come as the multi-touch protocol of slots has them:
 * each contact touching in a frame, by its tracking number and its
 * place, then the end of the frame (knl_hid_mt_sync); a contact the
 * frame leaves out has let go. They are taken as the contacts of a HID
 * touch screen's frame are (dg_contact, dg_frame): the first finger is
 * the pointer and its main button, its touch shaped into a click, a
 * drag or a hold (touch_shape). A place runs 0..xmax and 0..ymax across
 * the screen. The caller holds the USB manager's lock, as for
 * knl_hid_ext_report. Answers the handle (from 1), or an error.
 */
EXPORT INT knl_hid_mt_attach( CONST UB *name, INT xmax, INT ymax )
{
	HIDDEV	*h;
	HIDDG	*dg;
	DGAPP	*a;
	INT	i;

	if ( xmax <= 0 || ymax <= 0 ) {
		return E_PAR;
	}
	dg = (HIDDG *)Kcalloc(1, sizeof(HIDDG));
	if ( dg == NULL ) {
		return E_NOMEM;
	}
	h = hid_slot();
	if ( h == NULL ) {
		Kfree(dg);
		return E_NOMEM;
	}
	i = h->slot;
	dg_forget(h);
	knl_memset(h, 0, sizeof(*h));
	h->slot = i;
	for ( i = 0; i < 15 && name != NULL && name[i] != 0; i++ ) h->ext_name[i] = name[i];
	h->ext_name[i] = 0;
	/* an absolute pointer of one button, as the list of the devices shows it */
	h->x.size = h->y.size = 16;
	h->x.lmax = xmax;
	h->y.lmax = ymax;
	h->nbtn = 1;
	dg->n = 1;
	a = &dg->a[0];
	a->kind = DGK_TOUCH;
	a->prim = -1;
	a->s[0].x = h->x;
	a->s[0].y = h->y;
	h->dg = dg;
	h->id = ++hid_next_id;
	h->used = TRUE;
	hid_count.pointers++;
	return h->slot + 1;
}

LOCAL DGAPP *mt_app( INT hd, HIDDEV **p_d );

/*
 * How large the screen under the touch screen of knl_hid_mt_attach is:
 * its diagonal in tenths of a millimetre, for the lengths a finger is
 * given in millimetres (touch_px). Not told, it is guessed.
 */
EXPORT ER knl_hid_mt_size( INT hd, INT diag10 )
{
	HIDDEV	*d;
	DGAPP	*a = mt_app(hd, &d);

	if ( a == NULL ) return E_ID;
	a->diag10 = ( diag10 > 0 ) ? diag10 : 0;
	return E_OK;
}

LOCAL DGAPP *mt_app( INT hd, HIDDEV **p_d )
{
	HIDDEV	*d;

	if ( hd < 1 || hd > HID_N ) {
		return NULL;
	}
	d = HID_AT(hd - 1);
	if ( !d->used || d->dev != NULL || d->dg == NULL || d->dg->a[0].kind != DGK_TOUCH ) {
		return NULL;
	}
	*p_d = d;
	return &d->dg->a[0];
}

/* A contact touching in the frame: its tracking number and place */
EXPORT void knl_hid_mt_contact( INT hd, INT id, INT x, INT y )
{
	HIDDEV	*d;
	DGAPP	*a = mt_app(hd, &d);

	if ( a != NULL ) {
		dg_contact(a, id, TRUE, TRUE, x, y);
	}
}

/* The frame is all in */
EXPORT void knl_hid_mt_sync( INT hd )
{
	HIDDEV	*d;
	DGAPP	*a = mt_app(hd, &d);

	if ( a != NULL ) {
		dg_frame(d, a);
		hid_count.reports++;
		d->reports++;
	}
}

/* ---------------------------------------------------------------- attach */

#ifdef USE_KTEST
/*
 * For the tests: a device of this product name that comes from now on is
 * not taken, and is left to the programs (SVC class 12, WebHID); NULL
 * takes them all again.
 */
LOCAL UB	hid_leave_name[32];

EXPORT void knl_hid_leave( CONST UB *product )
{
	INT	i;

	for ( i = 0; i < (INT)sizeof(hid_leave_name) - 1 && product != NULL && product[i] != 0; i++ ) {
		hid_leave_name[i] = product[i];
	}
	hid_leave_name[i] = 0;
}
#endif

EXPORT BOOL knl_hid_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( !hid_ready || dev->cfg == NULL ) {
		return FALSE;
	}
#ifdef USE_KTEST
	if ( hid_leave_name[0] != 0 && knl_strcmp((CONST char *)dev->product, (CONST char *)hid_leave_name) == 0 ) {
		return FALSE;
	}
#endif
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[3] == 0
		  && p[5] == USB_CLASS_HID ) {
			return TRUE;
		}
	}

	return FALSE;
}

LOCAL HIDDEV *hid_slot( void )
{
	HIDTBL	*t;
	INT	i, cap;

	for ( i = 0; i < HID_N; i++ ) {
		if ( !HID_AT(i)->used ) {
			return HID_AT(i);
		}
	}
	/* every place in use: a table twice as large, made whole before it is put in place */
	cap = ( HID_N > 0 ) ? HID_N * 2 : HID_FIRST;
	t = (HIDTBL *)Kcalloc(1, sizeof(HIDTBL) + sizeof(HIDDEV *) * (SZ)cap);
	if ( t == NULL ) {
		return NULL;
	}
	for ( i = 0; i < cap; i++ ) {
		if ( i < HID_N ) {
			t->e[i] = HID_AT(i);
			continue;
		}
		t->e[i] = (HIDDEV *)Kcalloc(1, sizeof(HIDDEV));
		if ( t->e[i] == NULL ) break;
		t->e[i]->slot = i;
	}
	if ( i == HID_N ) {
		Kfree(t);
		return NULL;
	}
	t->cap = i;
	__sync_synchronize();
	i = HID_N;
	hid_tbl = t;
	return HID_AT(i);
}

/*
 * Which protocol an interface of the boot subclass says it talks
 * (GET_PROTOCOL): 0 the boot protocol, 1 the report protocol, -1 no
 * answer (a stall, or not one byte of 0 or 1).
 */
LOCAL INT get_protocol( USBDEV *dev, INT ifno )
{
	UB	p = 0xff;
	INT	n = 0;

	if ( knl_usb_control(dev, HID_RT_IN, HID_GET_PROTOCOL, 0, (UH)ifno, &p, 1, &n) < E_OK
	  || n != 1 || p > 1 ) {
		return -1;
	}

	return (INT)p;
}

LOCAL CONST char *proto_name( INT p )
{
	return ( p == 0 ) ? "boot" : ( p == 1 ) ? "report" : "no answer";
}

/*
 * A pointer of the boot subclass whose descriptor was read is to talk
 * the report protocol, the one its descriptor describes. A bus reset
 * puts it there, but a device need not keep to that (a boot loader that
 * read it as a boot mouse may have left it in the boot protocol), so it
 * is asked which it talks. It is told the report protocol only when it
 * does not already say so: hosts that never send SET_PROTOCOL use a
 * mouse as the reset left it, and that is how mice are most often used.
 * Then it is asked again. When the last answer is the boot protocol,
 * whatever it was told, its reports are read as that; with no answer,
 * as its descriptor says, while its reports are watched (boot_padded).
 * What it said goes to the console.
 */
LOCAL void want_report_protocol( USBDEV *dev, HIDDEV *h, INT ifno )
{
	INT	before, after;
	ER	er;

	before = get_protocol(dev, ifno);
	if ( before == 1 ) {
		USB_LOG("usb: hid: interface %d says the report protocol\n", ifno);
		return;
	}
	er = knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 1, (UH)ifno, NULL, 0, NULL);
	after = get_protocol(dev, ifno);
	if ( after == 0 ) {
		boot_mouse(h);
		h->boot = TRUE;
	}
	USB_LOG("usb: hid: interface %d says %s; set report protocol (%d); then says %s%s\n",
		ifno, proto_name(before), (INT)er, proto_name(after),
		h->boot ? ": read as the boot protocol" : "");
}

/*
 * A pen or a touch screen put into the mode it is read in: the feature
 * report dg_mode_report made, sent with SET_REPORT (HID 1.11 7.2.2). A
 * device that stalls it is read as it is; what it answered goes to the
 * console.
 */
LOCAL void set_mode( USBDEV *dev, INT ifno, UB *rep, INT len, INT rid )
{
	char	line[3 * 16 + 8];
	INT	k, m = 0;
	ER	er;

	er = knl_usb_control(dev, HID_RT_OUT, HID_SET_REPORT, (UH)( ( HID_REPORT_FEATURE << 8 ) | rid ),
			     (UH)ifno, rep, (UH)len, NULL);
	line[0] = 0;
	for ( k = 0; k < len && k < 16; k++ ) {
		m += tm_sprintf((UB *)line + m, (UB *)" %02x", (INT)rep[k]);
	}
	USB_LOG("usb: hid: interface %d input mode: feature report %d (%d bytes:%s) set (%d)\n",
		ifno, rid, len, line, (INT)er);
}

/* One HID interface; id is its interface descriptor */
LOCAL ER hid_interface( USBDEV *dev, UB *id, UB *end )
{
	UB	*p, *epd = NULL;
	INT	rd_len = 0, n = 0, xfer;
	INT	ifno = id[2], sub = id[6], proto = id[7];
	HIDDEV	*h;
	USBEP	ep;
	ER	er;

	for ( p = knl_usb_next_desc(id, end); p != NULL && p[1] != USB_DT_INTERFACE;
	      p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_HID && p[0] >= 9 && rd_len == 0 ) {
			INT	k;

			/* its class descriptors, three bytes each; the report one among them */
			for ( k = 0; k < p[5] && 6 + 3 * k + 2 < p[0]; k++ ) {
				if ( p[6 + 3 * k] == USB_DT_HID_REPORT ) {
					rd_len = GET16(p + 7 + 3 * k);
					break;
				}
			}
		}
		if ( p[1] == USB_DT_ENDPOINT && p[0] >= 7 && epd == NULL
		  && (p[2] & 0x80) != 0 && (p[3] & 3) == USB_EP_INTR ) {
			epd = p;
		}
	}
	if ( epd == NULL ) {
		return E_NOEXS;
	}
	h = hid_slot();
	if ( h == NULL ) {
		return E_NOMEM;
	}
	{
		INT	slot = h->slot;		/* the entry's place stays */

		dg_forget(h);
		knl_memset(h, 0, sizeof(*h));
		h->slot = slot;
		h->ifno = ifno;
	}

	if ( sub == 1 && proto == HID_PROTO_KBD ) {
		h->is_kbd = TRUE;
		er = knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 0, (UH)ifno,
				     NULL, 0, NULL);
		if ( er < E_OK ) {
			USB_LOG("usb: hid: interface %d: set protocol boot failed (%d)\n", ifno, (INT)er);
		}
	} else {
		BOOL	found = FALSE;

		if ( rd_len > 0 && rd_len <= HID_RD_MAX ) {
			UB	*rd = (UB *)Kmalloc((SZ)rd_len);

			if ( rd != NULL ) {
				INT	t;

				/* a device just configured may not answer at once */
				for ( t = 0; t < HID_RD_TRIES; t++ ) {
					if ( t > 0 ) {
						knl_usb_wait(20);
					}
					if ( knl_usb_control(dev, USB_RT_IN | USB_RT_INTERFACE,
							USB_REQ_GET_DESCRIPTOR, USB_DT_HID_REPORT << 8,
							(UH)ifno, rd, (UH)rd_len, &n) >= E_OK && n > 0 ) {
						break;
					}
					n = 0;
				}
			}
			if ( n > 0 ) {
				UB	mode[64];
				INT	mlen = 0, mrid = 0;

				found = hid_read_desc(h, rd, n, GET16(dev->ddesc + 8) == USB_VID_WACOM,
						      mode, &mlen, &mrid);
				if ( mlen > 0 ) {
					set_mode(dev, ifno, mode, mlen, mrid);
				}
			}
			if ( rd != NULL ) Kfree(rd);
		}
		if ( !found || ( h->cc_only && sub == 1 && proto == HID_PROTO_MOUSE ) ) {
			if ( !( sub == 1 && proto == HID_PROTO_MOUSE ) ) {
				return E_NOSPT;	/* neither a keyboard nor a pointer */
			}
			h->cc_only = FALSE;
			boot_mouse(h);
			h->boot = TRUE;
			knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 0, (UH)ifno,
					NULL, 0, NULL);
		} else if ( sub == 1 ) {
			h->bootable = TRUE;
			want_report_protocol(dev, h, ifno);
		}
	}
	knl_usb_control(dev, HID_RT_OUT, HID_SET_IDLE, 0, (UH)ifno, NULL, 0, NULL);
	if ( h->is_kbd ) {
		/*
		 * The lamps put out: some keyboards send nothing until their
		 * LEDs have been set once, as a PC's firmware always does.
		 * The boot protocol's output report is the one byte of lamps.
		 */
		UB	leds = 0;

		knl_usb_control(dev, HID_RT_OUT, HID_SET_REPORT, HID_REPORT_OUTPUT << 8,
				(UH)ifno, &leds, 1, NULL);
	}

	h->dev = dev;
	h->ifno = ifno;
	h->id = ++hid_next_id;
	h->used = TRUE;
	knl_usb_ep_from_desc(&ep, dev, epd, NULL);
	/*
	 * A transfer a packet long. When the pointer's report is longer
	 * than a packet it comes in several, the last one short, and read
	 * a packet at a time each piece would be taken for a report: the
	 * transfer is then as long as the longest report the descriptor
	 * declares, so that none of them ends it early or overruns it.
	 */
	xfer = ep.size;
	if ( !h->boot && h->plen > xfer ) {
		xfer = h->rlen;
	}
	if ( xfer > HID_REPORT_MAX ) {
		xfer = HID_REPORT_MAX;
	}
	er = dev->hc->ops->stream_open(dev->hc, dev, &ep, xfer, hid_xfer, h, &h->pipe);
	if ( er < E_OK ) {
		h->used = FALSE;
		return er;
	}
	if ( h->is_kbd || h->cc_only ) {
		hid_count.keyboards++;
	} else {
		hid_count.pointers++;
	}
	USB_LOG("usb: hid: %s on interface %d, endpoint %02x, %s\n",
		( h->is_kbd || h->cc_only ) ? "keyboard" : "pointer", ifno, (INT)ep.addr,
		h->is_kbd ? "boot protocol" : h->cc_only ? "consumer keys"
		: ( h->x.rel ? "relative" : "absolute" ));
	if ( !h->is_kbd && !h->cc_only ) {
		/* where its report says what, as read from its descriptor */
		USB_LOG("usb: hid: %d %s, descriptor %d bytes, report id %d, report %d bytes, "
			"packet %d, read %d, interval %d; "
			"x bit %d size %d%s%s, y bit %d size %d, %d buttons at bit %d, wheel bit %d size %d, "
			"range %d..%d\n",
			(INT)h->id, h->boot ? "boot protocol" : "report protocol", rd_len, h->report_id,
			h->plen, (INT)ep.size, xfer, (INT)epd[6], h->x.off, h->x.size,
			h->x.rel ? " relative" : " absolute",
			h->x.sgn ? " signed" : "", h->y.off, h->y.size, h->nbtn, h->btn.off,
			h->wheel.off, h->wheel.size, h->x.lmin, h->x.lmax);
	}

	return E_OK;
}

/*
 * A device with a keyboard beside its pointer: the keyboard was put into
 * the boot protocol, and a device that keeps one protocol for all its
 * interfaces put its pointer there with it. Each pointer read as the
 * report protocol is asked again once all the interfaces are set up.
 */
LOCAL void recheck_protocol( USBDEV *dev )
{
	INT	i, k;

	for ( k = 0; k < HID_N; k++ ) {
		if ( HID_AT(k)->used && HID_AT(k)->dev == dev && HID_AT(k)->is_kbd ) {
			break;
		}
	}
	if ( k >= HID_N ) {
		return;
	}
	for ( i = 0; i < HID_N; i++ ) {
		HIDDEV	*h = HID_AT(i);

		if ( !h->used || h->dev != dev || h->is_kbd || !h->bootable || h->boot ) {
			continue;
		}
		if ( get_protocol(dev, h->ifno) == 0 ) {
			boot_mouse(h);
			h->boot = TRUE;
			USB_LOG("usb: hid: %d says the boot protocol after interface %d was put into it: "
				"read as the boot protocol\n", (INT)h->id, HID_AT(k)->ifno);
		}
	}
}

EXPORT ER knl_hid_attach( USBDEV *dev )
{
	UB	*p, *end = dev->cfg + dev->cfglen;
	INT	n = 0;
	ER	er = E_NOEXS;

	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		ER	e;

		if ( p[1] != USB_DT_INTERFACE || p[0] < 9 || p[3] != 0
		  || p[5] != USB_CLASS_HID ) {
			continue;
		}
		e = hid_interface(dev, p, end);
		if ( e >= E_OK ) {
			n++;
		} else {
			er = e;
		}
	}
	recheck_protocol(dev);
	if ( n > 0 ) {
		dev->hid = HID_AT(0);
		return E_OK;
	}

	return er;
}

/* The device went: its keys and buttons are let go, as they would be */
EXPORT void knl_hid_detach( USBDEV *dev )
{
	INT	i;

	for ( i = 0; i < HID_N; i++ ) {
		HIDDEV	*h = HID_AT(i);

		if ( !h->used || h->dev != dev ) {
			continue;
		}
		if ( h->pipe != NULL ) {
			dev->hc->ops->stream_close(dev->hc, h->pipe);
			h->pipe = NULL;
		}
		cc_let_go(h);
		if ( h->cc_only ) {
			hid_count.keyboards--;
		} else if ( h->is_kbd ) {
			UB	none[8];

			knl_memset(none, 0, sizeof(none));
			kbd_report(h, none);
			hid_count.keyboards--;
		} else {
			T_HIDEV	ev;
			UINT	b;
			INT	k;
			UB	tch = TCH_NO;

			/* a finger on a touch screen taken away: not a touch meant, nor fingers together */
			for ( k = 0; h->dg != NULL && k < h->dg->n; k++ ) {
				DGAPP	*ta = &h->dg->a[k];

				if ( ta->kind == DGK_TOUCH && ta->prim >= 0 ) tch = TCH_CANCEL;
				if ( ta->kind == DGK_TOUCH && ta->multi ) {
					tw_put(h, ta, TW_CANCEL, ta->m_x, ta->m_y, 0);
					ta->multi = FALSE;
				}
			}
			h->buttons = 0;
			b = all_buttons();
			knl_memset(&ev, 0, sizeof(ev));
			ev.when = now_ns();
			ev.x = ptr_x;
			ev.y = ptr_y;
			for ( k = 0; k < 3; k++ ) {
				if ( ((b ^ ptr_buttons) & (1U << k)) != 0 ) {
					ev.type = HID_EV_BTN_UP;
					ev.code = (UINT)k;
					ring_put_ptr(h, &ev, tch);
				}
			}
			ptr_buttons = b;
			hid_count.pointers--;
		}
		h->used = FALSE;
		dg_forget(h);
	}
	dev->hid = NULL;
}

/* ---------------------------------------------------------------- interface */

/* ---------------------------------------------------------------- how they are taken */

/*
 * What a device reports goes through the attributes a person set before
 * it is taken: a key or a button counts only once held long enough and
 * not too soon after it was let go, a shift pressed just after a key
 * still goes with it, a key held repeats, and a shift clicked on its
 * own holds for the next key or locks. This needs time as well as the
 * reports, so it is done here, as the events are taken: a press waiting
 * to count, or a repeat, is a time by which ts_hid_read wakes.
 *
 * Only the one task that reads the events (the desktop's) runs this.
 */
#define SH_OUT		32
#define SH_WAIT		8
#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

typedef struct {
	T_HIDEV	ev;
	UD	due;			/* ns: when it counts */
	BOOL	up;			/* its let go came before it counted */
	BOOL	sim;			/* a character waiting to see whether a thumb goes with it */
	UINT	tent;			/* the thumb that came, KM_MOD_THUMB_*: its until due or another key */
} SHWAIT;

/*
 * How long after a character's press a thumb still goes with it in a
 * thumb-shift layout. Longer than most fingers are apart when they mean
 * the two together, and short enough not to be seen while typing.
 */
#define SH_THUMB_SIM	75

LOCAL T_HIDEV	sh_out[SH_OUT];

/*
 * What a finger's events are marked with as they go out: the screen's
 * pixels in 100 mm while touch_shape works one, else 0; and the mark of
 * the event ts_hid_read gave last (knl_hid_taken_touch)
 */
LOCAL INT	sh_px = 0;
LOCAL INT	sh_out_px[SH_OUT];
LOCAL INT	sh_taken_px = 0;
LOCAL INT	sh_oh = 0, sh_ot = 0;
LOCAL SHWAIT	sh_wait[SH_WAIT];
LOCAL INT	sh_nwait = 0;
LOCAL UD	sh_up_at[256];		/* ns a key was last let go */
LOCAL UD	sh_btn_up_at[8];
LOCAL BOOL	sh_drop_up[256];	/* its press was not taken: nor is its let go */
LOCAL BOOL	sh_btn_drop_up[8];
LOCAL UINT	sh_rep_key = 0;		/* the key that repeats, 0 none */
LOCAL UINT	sh_rep_mods;
LOCAL UD	sh_rep_due;
LOCAL UINT	sh_mods = 0;		/* the shifts held, as the keyboard says */
LOCAL UINT	sh_sticky = 0;		/* shifts one-shot or locked */
LOCAL BOOL	sh_locked = FALSE;
LOCAL UD	sh_shift_down, sh_shift_click;	/* the last shift's press, and its last click */
LOCAL BOOL	sh_shift_alone = FALSE;

LOCAL void sh_emit( CONST T_HIDEV *ev )
{
	INT	next = ( sh_ot + 1 ) % SH_OUT;

	if ( next == sh_oh ) {
		sh_oh = ( sh_oh + 1 ) % SH_OUT;	/* the oldest goes, as in the ring */
	}
	sh_out[sh_ot] = *ev;
	sh_out_px[sh_ot] = sh_px;
	sh_ot = next;
}

LOCAL BOOL is_mod_key( UINT code )
{
	return (BOOL)( code >= 0xE0 && code <= 0xE7 );
}

LOCAL UD ms_ns( UINT ms )
{
	return (UD)ms * 1000000ULL;
}

/*
 * The thumbs of a thumb-shift kana layout (design 11.x): a thumb key's
 * press is kept back, the keys pressed while it is held are marked with
 * it, and it is let through, pressed and let go, only when nothing went
 * with it. In the 親指シフト layout a character pressed just before the
 * thumb goes with it too, so each character without a thumb waits
 * SH_THUMB_SIM to see. When a thumb comes within that, a second character
 * may still be nearer to it (the three keys of character, thumb,
 * character): the first waits as long again as it was before the thumb,
 * and goes alone if the second comes in that time, which then has the
 * thumb. Any other key sends what waits first, so the order holds.
 */
LOCAL UINT	km_thumbs = 0;		/* KM_MOD_THUMB_* held, in a thumb-shift kana layout */
LOCAL UINT	km_thumb_used = 0;	/* those of them a key went with */

LOCAL void key_counts( T_HIDEV *ev );

/* Whether the key is a character a thumb can go with: not with Ctrl, Alt or the Windows key held */
LOCAL BOOL sim_key( CONST T_HIDEV *ev )
{
	UINT	c = ev->code;

	if ( km_kana() != KM_KANA_NICOLA || km_thumbs != 0 ) {
		return FALSE;
	}
	if ( ( ev->mods & 0xDD ) != 0 ) {
		return FALSE;
	}
	return (BOOL)( ( c >= 0x04 && c <= 0x27 ) || ( c >= 0x2D && c <= 0x38 ) );
}

/* A waiting key sent as it now stands; it leaves the table */
LOCAL void wait_go( INT i )
{
	T_HIDEV	e = sh_wait[i].ev;
	UINT	tent = sh_wait[i].tent;

	sh_wait[i] = sh_wait[--sh_nwait];
	e.mods |= tent;
	key_counts(&e);
}

/*
 * Before a key that is not one the waiting characters could go with:
 * they go out as they stand, the oldest first, those with a thumb whose
 * time is over with it, the others without it
 */
LOCAL void sim_flush( UD when )
{
	for ( ;; ) {
		INT	i, old = -1;

		for ( i = 0; i < sh_nwait; i++ ) {
			if ( sh_wait[i].sim && ( old < 0 || sh_wait[i].ev.when < sh_wait[old].ev.when ) ) {
				old = i;
			}
		}
		if ( old < 0 ) {
			return;
		}
		if ( sh_wait[old].tent != 0 && when < sh_wait[old].due ) {
			sh_wait[old].tent = 0;		/* the newer key is nearer the thumb */
		}
		wait_go(old);
	}
}

/* The thumb's press taken: the character it is near, if one waits, goes with it */
LOCAL void sim_thumb( UINT bit, UD when )
{
	INT	i;

	for ( i = 0; i < sh_nwait; i++ ) {
		UD	gap = when - sh_wait[i].ev.when;

		if ( !sh_wait[i].sim || sh_wait[i].tent != 0 ) {
			continue;
		}
		if ( gap <= ms_ns(SH_THUMB_SIM) ) {
			sh_wait[i].tent = bit;
			sh_wait[i].due = when + gap;
			km_thumb_used |= bit;
		}
		return;
	}
}

/* A thumb let go: what waits for it is decided now */
LOCAL void sim_thumb_up( UINT bit )
{
	INT	i;

	for ( i = 0; i < sh_nwait; ) {
		if ( sh_wait[i].sim && sh_wait[i].tent == bit ) {
			wait_go(i);
			continue;
		}
		i++;
	}
}

/*
 * A thumb key's press or let go in a thumb-shift layout: TRUE when it was
 * taken as one, and goes no further as it came
 */
LOCAL BOOL thumb_shape( T_HIDEV *ev )
{
	INT	k = km_kana();
	UINT	bit;

	if ( k != KM_KANA_TRON && k != KM_KANA_NICOLA ) {
		km_thumbs = km_thumb_used = 0;
		return FALSE;
	}
	if ( ev->code != KM_THUMB_L_KEY && ev->code != KM_THUMB_R_KEY ) {
		return FALSE;
	}
	bit = ( ev->code == KM_THUMB_L_KEY ) ? KM_MOD_THUMB_L : KM_MOD_THUMB_R;
	if ( ev->type == HID_EV_KEY_DOWN ) {
		km_thumbs |= bit;
		km_thumb_used &= ~bit;
		sim_thumb(bit, ev->when);
		return TRUE;
	}
	km_thumbs &= ~bit;
	sim_thumb_up(bit);
	if ( ( km_thumb_used & bit ) == 0 ) {
		T_HIDEV	down = *ev;

		down.type = HID_EV_KEY_DOWN;
		sh_emit(&down);			/* alone: pressed, and this let go */
		sh_emit(ev);
		return TRUE;
	}
	km_thumb_used &= ~bit;
	return TRUE;
}

/* A key that counts: sent, with the shifts that stick, and made the one that repeats */
LOCAL void key_counts( T_HIDEV *ev )
{
	if ( !is_mod_key(ev->code) ) {
		ev->mods |= sh_sticky;
		if ( sh_sticky != 0 && !sh_locked ) sh_sticky = 0;	/* one-shot: used */
		sh_shift_alone = FALSE;
		/* a key that turns something on and off is one press however long it is held */
		if ( hid_attr.krp && ev->code != HID_KEY_MUTE && !( ev->code >= HID_KEY_PLAYPAUSE && ev->code <= HID_KEY_NEXT ) ) {
			sh_rep_key = ev->code;
			sh_rep_mods = ev->mods;
			sh_rep_due = ev->when + ms_ns(hid_attr.krp_start);
		}
	}
	sh_emit(ev);
}

/* A shift pressed and let go on its own: 一時シフト and 簡易ロック */
LOCAL void shift_clicked( UINT bit, UD now )
{
	if ( hid_attr.tshift ) {
		/* each click goes on: none, one-shot, locked, none */
		if ( sh_sticky == 0 ) {
			sh_sticky = bit;
			sh_locked = FALSE;
		} else if ( !sh_locked ) {
			sh_locked = TRUE;
		} else {
			sh_sticky = 0;
			sh_locked = FALSE;
		}
		return;
	}
	if ( hid_attr.sclk == 0 || now - sh_shift_down > ms_ns(hid_attr.sclk) ) {
		return;				/* held too long to be a click */
	}
	if ( sh_locked ) {
		sh_sticky = 0;
		sh_locked = FALSE;
	} else if ( sh_sticky != 0 && now - sh_shift_click <= ms_ns(hid_attr.sclk * 5 / 2) ) {
		sh_locked = TRUE;		/* clicked twice: locked */
	} else {
		sh_sticky = bit;		/* once: the next key */
	}
	sh_shift_click = now;
}

/* One event as a device reported it, through the attributes */
LOCAL void shape( T_HIDEV *ev )
{
	INT	i;

	switch ( ev->type ) {
	case HID_EV_KEY_DOWN:
		if ( ev->code < 256 && hid_attr.key_off > 0 && ev->when - sh_up_at[ev->code] < ms_ns(hid_attr.key_off) ) {
			sh_drop_up[ev->code] = TRUE;	/* too soon after it was let go */
			return;
		}
		if ( thumb_shape(ev) ) {
			return;
		}
		if ( km_thumbs != 0 && !is_mod_key(ev->code) ) {
			km_thumb_used |= km_thumbs;
		}
		ev->mods |= km_thumbs;
		if ( is_mod_key(ev->code) ) {
			UINT	bit = 1U << ( ev->code - 0xE0 );

			sh_mods |= bit;
			if ( ( bit & MOD_SHIFT ) != 0 ) {
				sh_shift_down = ev->when;
				sh_shift_alone = TRUE;
				/* a key waiting for its shift takes this one */
				for ( i = 0; i < sh_nwait; i++ ) {
					if ( sh_wait[i].ev.type == HID_EV_KEY_DOWN && !is_mod_key(sh_wait[i].ev.code)
					  && ev->when - sh_wait[i].ev.when <= ms_ns(hid_attr.key_sim) ) {
						sh_wait[i].ev.mods |= bit;
					}
				}
			} else {
				sh_shift_alone = FALSE;
			}
			sim_flush(ev->when);
			sh_emit(ev);
			return;
		}
		sim_flush(ev->when);
		sh_rep_key = 0;				/* another key: the repeat stops */
		{
			BOOL	sim = sim_key(ev);

			if ( hid_attr.key_on > 0 || hid_attr.key_sim > 0 || sim ) {
				if ( sh_nwait < SH_WAIT ) {
					UINT	wait = ( hid_attr.key_on > hid_attr.key_sim ) ? hid_attr.key_on : hid_attr.key_sim;

					if ( sim && wait < SH_THUMB_SIM ) wait = SH_THUMB_SIM;
					sh_wait[sh_nwait].ev = *ev;
					sh_wait[sh_nwait].due = ev->when + ms_ns(wait);
					sh_wait[sh_nwait].up = FALSE;
					sh_wait[sh_nwait].sim = sim;
					sh_wait[sh_nwait].tent = 0;
					sh_nwait++;
				}
				return;
			}
		}
		key_counts(ev);
		return;

	case HID_EV_KEY_UP:
		if ( ev->code < 256 ) {
			sh_up_at[ev->code] = ev->when;
			if ( sh_drop_up[ev->code] ) {
				sh_drop_up[ev->code] = FALSE;
				return;
			}
		}
		if ( thumb_shape(ev) ) {
			return;
		}
		ev->mods |= km_thumbs;
		if ( ev->code == sh_rep_key ) sh_rep_key = 0;
		if ( is_mod_key(ev->code) ) {
			UINT	bit = 1U << ( ev->code - 0xE0 );

			sh_mods &= ~bit;
			if ( ( bit & MOD_SHIFT ) != 0 && sh_shift_alone ) shift_clicked(bit, ev->when);
			sh_emit(ev);
			return;
		}
		/* its press still waiting: held too short to count, if a hold is asked for */
		for ( i = 0; i < sh_nwait; i++ ) {
			if ( sh_wait[i].ev.type == HID_EV_KEY_DOWN && sh_wait[i].ev.code == ev->code ) {
				if ( hid_attr.key_on > 0 && ev->when - sh_wait[i].ev.when < ms_ns(hid_attr.key_on) ) {
					sh_wait[i] = sh_wait[--sh_nwait];	/* never counted */
					return;
				}
				/* it counts now, and then goes up */
				wait_go(i);
				sh_rep_key = 0;
				break;
			}
		}
		sh_emit(ev);
		return;

	case HID_EV_BTN_DOWN:
		if ( ev->code < 8 && hid_attr.pd_off > 0 && ev->when - sh_btn_up_at[ev->code] < ms_ns(hid_attr.pd_off) ) {
			sh_btn_drop_up[ev->code] = TRUE;
			return;
		}
		if ( hid_attr.pd_on > 0 && sh_nwait < SH_WAIT ) {
			sh_wait[sh_nwait].ev = *ev;
			sh_wait[sh_nwait].due = ev->when + ms_ns(hid_attr.pd_on);
			sh_wait[sh_nwait].up = FALSE;
			sh_nwait++;
			return;
		}
		sh_emit(ev);
		return;

	case HID_EV_BTN_UP:
		if ( ev->code < 8 ) {
			sh_btn_up_at[ev->code] = ev->when;
			if ( sh_btn_drop_up[ev->code] ) {
				sh_btn_drop_up[ev->code] = FALSE;
				return;
			}
		}
		for ( i = 0; i < sh_nwait; i++ ) {
			if ( sh_wait[i].ev.type == HID_EV_BTN_DOWN && sh_wait[i].ev.code == ev->code ) {
				sh_wait[i] = sh_wait[--sh_nwait];	/* not held long enough */
				return;
			}
		}
		sh_emit(ev);
		return;

	default:
		sh_emit(ev);
		return;
	}
}

/*
 * A touch screen's first finger as a mouse's buttons (design 6.4.1).
 * Where it touches is pointed at at once. On a place a window asked for
 * (wm_obj_press_area) the main button is pressed there and then and the
 * finger is a drag from there until it lets go: a hold is nothing. Anywhere
 * else the press is kept back until it is known what it is:
 *   - let go before TC_HOLD_MS without moving: a click of the main
 *     button where it touched;
 *   - moved further than TC_SLOP: the main button pressed where it
 *     touched, and a drag from there until it lets go;
 *   - held TC_HOLD_MS without moving: the second button is pressed
 *     there at that moment, so the menu opens while the finger still
 *     touches, as the right button pressed on a mouse; moving after
 *     that drags the second button over the menu, and letting go lets
 *     it go where the finger is.
 * A tap that follows a tap within TC_DBL_MS and the touch's tolerance
 * (tc_dbl_slop) of where the first touched is clicked where the first
 * was: a finger lands a few millimetres off each time, far more than a
 * mouse's double click may be apart (LK_DBL_W), so the second click is
 * put where the first was and the double click is judged as for a
 * mouse. The tolerance is a length on the screen (LK_TCH_DBL, 0.1 mm),
 * made pixels by the screen's size (touch_px); with the touch's
 * correction off (LK_TCH_ON 0) the second tap is not moved. How long a
 * hold is is LK_TCH_HOLD, the correction on or off: the hold is the
 * only second button a finger has.
 * A finger that goes without a touch meant (TCH_CANCEL: a palm, a pen
 * coming near, the device taken away), or whose press another finger
 * cut (TCH_MULTI, two fingers taken together), leaves no click, and a
 * button it was holding lets go. A mouse's, a pen's and the TrackPoint's events
 * do not come here.
 */
#define TC_HOLD_MS	1000		/* held this long: the second button, unless set */
#define TC_SLOP_MIN	8		/* moved this far (view pixels) at least: a drag */
#define TC_DBL_MS	500		/* a tap this soon after a tap may be the second of a double click */
#define TC_DBL_MM10	50		/* and this near (0.1 mm), unless set */

enum { TC_IDLE, TC_WAIT, TC_HOLD, TC_DRAG };

LOCAL INT	tc_state = TC_IDLE;
LOCAL T_HIDEV	tc_down;		/* where and when it touched */
LOCAL UD	tc_due;			/* when holding it becomes the second button */
LOCAL INT	tc_rx, tc_ry;		/* where the finger itself touched, for whether it moves */
LOCAL BOOL	tc_tapped = FALSE;	/* the last touch was a tap, at tc_tap */
LOCAL T_HIDEV	tc_tap;
LOCAL INT	tc_px;			/* the screen it touched: view pixels in 100 mm (touch_px) */

/* Whether a touch there presses at once: read from the window manager's table, never waited for */
IMPORT BOOL knl_wm_press_now( INT x, INT y ) __attribute__((weak));

/* Whether the menu's shape is to be shown, by the pointer's layer (outer_kernel/wm/pointer.c) */
IMPORT void knl_pt_hold( BOOL on ) __attribute__((weak));

LOCAL void tc_hold_shown( BOOL on )
{
	if ( knl_pt_hold != NULL ) {
		knl_pt_hold(on);
	}
}

/* How far a finger may move and still be holding still: a 128th of the screen's width */
LOCAL INT tc_slop( void )
{
	INT	s = scr_w / 128;

	return ( s > TC_SLOP_MIN ) ? s : TC_SLOP_MIN;
}

/* How far apart the taps of a double click may be, in view pixels: 0 when they are not brought together */
LOCAL INT tc_dbl_slop( void )
{
	INT	mm10 = ( hid_attr.tch_dbl > 0 ) ? (INT)hid_attr.tch_dbl : TC_DBL_MM10;

	if ( hid_attr.tch_off ) {
		return 0;
	}
	return (INT)( (D)mm10 * tc_px / 1000 );
}

LOCAL UD tc_hold_ns( void )
{
	return ms_ns(( hid_attr.tch_hold > 0 ) ? hid_attr.tch_hold : TC_HOLD_MS);
}

EXPORT BOOL knl_hid_taken_touch( INT *p_px100 )
{
	if ( p_px100 != NULL ) *p_px100 = sh_taken_px;
	return (BOOL)( sh_taken_px > 0 );
}

/* A button of the finger's pressed or let go at where it touched */
LOCAL void tc_button( UINT type, UINT code, UD when )
{
	T_HIDEV	e = tc_down;

	e.type = type;
	e.code = code;
	e.when = when;
	sh_emit(&e);
}

/*
 * Two fingers on a touch screen (design 6.4.1): their midpoint moved is
 * the wheel, a notch for every LK_TCH_STEP (0.1 mm of the screen) it
 * goes, up and down or across, so that what is shown goes with the
 * fingers: the fingers moved down are the wheel turned away (dz > 0, as
 * a mouse's turned away from the person shows what is above), moved
 * left the wheel across to the right. Which it is is decided once the
 * midpoint has gone TW_START_MM10, or the fingers' distance changed as
 * much: the distance changed more than the midpoint moved is a pinch,
 * which scrolls nothing; else the scroll keeps to the way the midpoint
 * went most, for as long as the two touch. Let go while moving, the
 * scroll goes on and slows (tw_fling), stopped by the next touch. Two
 * fingers that touch and let go within TW_TAP_MS without moving are a
 * click of the second button where they touched, unless the first was
 * already holding or dragging when the second came.
 * The wheel's events go where the fingers first touched together.
 */
#define TW_START_MM10	30		/* moved this far (0.1 mm): a scroll or a pinch */
#define TW_STEP_MM10	100		/* a notch for this far, unless set */
#define TW_TAP_MS	500		/* the two let go this soon: a tap */
#define TW_VEL_MS	100		/* the speed when let go: over the moves this long before */
#define TW_FLING_MIN	50		/* mm/s: slower than this, no fling */
#define TW_FLING_MAX	1500		/* mm/s: at most this fast */
#define TW_FLING_STOP	15		/* mm/s: the fling slowed to this has stopped */
#define TW_TICK_MS	16		/* the fling moves this often */
#define TW_DECAY	245		/* and keeps this much of its speed (/256): about 0.36 s to slow by e */
#define TW_SAMPLES	8
#define TW_LATE_MS	200		/* the fling not moved for this long has stopped */

enum { TW_NONE, TW_WAIT, TW_SCROLL, TW_PINCH };
enum { TW_CUT_NONE, TW_CUT_WAIT, TW_CUT_OUT };

LOCAL BOOL	tw_on = FALSE;		/* fingers together on the screen */
LOCAL BOOL	tw_ok;			/* still may be a tap */
LOCAL BOOL	tw_used;		/* has been a scroll or a pinch */
LOCAL INT	tw_cut = TW_CUT_NONE;	/* how the first finger's press was cut */
LOCAL UD	tw_t0;			/* when they touched together */
LOCAL UD	tw_td;			/* when the pair touched */
LOCAL T_HIDEV	tw_at;			/* where, for the wheel's events and the tap */
LOCAL INT	tw_px = 1;		/* the screen's view pixels in 100 mm */
LOCAL INT	tw_mode = TW_NONE;
LOCAL BOOL	tw_across;		/* the scroll goes across */
LOCAL INT	tw_x0, tw_y0, tw_d0;	/* the pair's midpoint and distance when it touched */
LOCAL INT	tw_lx, tw_ly;		/* the midpoint the scroll was last counted from */
LOCAL D		tw_acc;			/* moved and not yet a notch, 1/1000 view pixel */
LOCAL UD	tw_st[TW_SAMPLES];	/* the midpoint's last places along the scroll, and when */
LOCAL INT	tw_sp[TW_SAMPLES];
LOCAL INT	tw_ns = 0, tw_si = 0;
LOCAL BOOL	tw_fling = FALSE;
LOCAL D		tw_fv;			/* the fling's speed, 1/1000 view pixel a second */
LOCAL UD	tw_fdue;

/* The view pixels on the screen of a length in 0.1 mm */
LOCAL INT tw_len( INT mm10 )
{
	INT	v = (INT)( (D)mm10 * tw_px / 1000 );

	return ( v > 0 ) ? v : 1;
}

LOCAL INT tw_step( void )
{
	return tw_len(( hid_attr.tch_scr > 0 ) ? (INT)hid_attr.tch_scr : TW_STEP_MM10);
}

/* What was moved along the scroll's way (1/1000 view pixel), into notches of the wheel */
LOCAL void tw_wheel( D mpx, UD when )
{
	T_HIDEV	e;
	D	step = (D)tw_step() * 1000;
	INT	n;

	tw_acc += mpx;
	n = (INT)( tw_acc / step );
	if ( n == 0 ) {
		return;
	}
	tw_acc -= n * step;
	e = tw_at;
	e.type = HID_EV_WHEEL;
	e.code = tw_across ? HID_WHEEL_H : HID_WHEEL_V;
	e.dz = tw_across ? -n : n;		/* across: moved left is the wheel to the right */
	e.when = when;
	sh_emit(&e);
}

/* The midpoint along the scroll's way */
LOCAL INT tw_way( INT x, INT y )
{
	return tw_across ? x : y;
}

/* One place of the midpoint along the scroll's way, for its speed when let go */
LOCAL void tw_sample( UD when, INT p )
{
	tw_st[tw_si] = when;
	tw_sp[tw_si] = p;
	tw_si = ( tw_si + 1 ) % TW_SAMPLES;
	if ( tw_ns < TW_SAMPLES ) tw_ns++;
}

/* The pair let go: what it was moving at goes on as a fling */
LOCAL void tw_let_go( UD when )
{
	INT	i, k, last, first = -1;
	D	v, mm;

	tw_fling = FALSE;
	if ( tw_mode == TW_SCROLL && tw_ns > 1 ) {
		last = ( tw_si + TW_SAMPLES - 1 ) % TW_SAMPLES;
		for ( i = 0; i < tw_ns; i++ ) {
			k = ( tw_si + TW_SAMPLES - 1 - i ) % TW_SAMPLES;
			if ( when - tw_st[k] > ms_ns(TW_VEL_MS) ) break;
			first = k;
		}
		if ( first >= 0 && first != last && tw_st[last] > tw_st[first] ) {
			v = (D)( tw_sp[last] - tw_sp[first] ) * 1000000000000LL / (D)( tw_st[last] - tw_st[first] );
			mm = v / 10 / tw_px;
			if ( mm >= TW_FLING_MIN || mm <= -TW_FLING_MIN ) {
				if ( mm > TW_FLING_MAX ) v = (D)TW_FLING_MAX * tw_px * 10;
				if ( mm < -TW_FLING_MAX ) v = -(D)TW_FLING_MAX * tw_px * 10;
				tw_fv = v;
				tw_fdue = when + ms_ns(TW_TICK_MS);
				tw_fling = TRUE;
			}
		}
	}
	tw_mode = TW_NONE;
	tw_ns = 0;
}

/* The fling's moves due by now */
LOCAL void tw_fling_time( UD now )
{
	INT	px = sh_px;
	D	mm;

	sh_px = tw_px;
	if ( tw_fling && now > tw_fdue + ms_ns(TW_LATE_MS) ) {
		tw_fling = FALSE;		/* not read for a while: what was thrown is not owed now */
	}
	while ( tw_fling && now >= tw_fdue ) {
		tw_wheel(tw_fv * TW_TICK_MS / 1000, tw_fdue);
		tw_fv = tw_fv * TW_DECAY / 256;
		tw_fdue += ms_ns(TW_TICK_MS);
		mm = tw_fv / 10 / tw_px;
		if ( mm < TW_FLING_STOP && mm > -TW_FLING_STOP ) {
			tw_fling = FALSE;
		}
	}
	sh_px = px;
}

LOCAL void two_shape( CONST T_HIDEV *ev )
{
	INT	dx, dy, dm, dd;

	if ( !tw_on ) {
		if ( ev->code == TW_END || ev->code == TW_CANCEL || ev->code == TW_UP ) return;
		tw_on = TRUE;
		tw_ok = ( tw_cut != TW_CUT_OUT );
		tw_used = FALSE;
		tw_t0 = ev->when;
		tw_at = *ev;
		tw_px = ( sh_px > 0 ) ? sh_px : 1;
		tw_mode = TW_NONE;
	}
	tw_cut = TW_CUT_NONE;
	switch ( ev->code ) {
	case TW_DOWN:
		tw_fling = FALSE;
		tw_mode = TW_WAIT;
		tw_x0 = tw_lx = ev->x;
		tw_y0 = tw_ly = ev->y;
		tw_d0 = ev->dz;
		tw_td = ev->when;
		tw_acc = 0;
		tw_ns = tw_si = 0;
		return;

	case TW_MOVE:
		if ( tw_mode != TW_WAIT && tw_mode != TW_SCROLL ) return;
		if ( tw_mode == TW_WAIT ) {
			dx = ev->x - tw_x0;
			dy = ev->y - tw_y0;
			dm = isqrt((UD)( dx * dx ) + (UD)( dy * dy ));
			dd = ev->dz - tw_d0;
			if ( dd < 0 ) dd = -dd;
			if ( dm < tw_len(TW_START_MM10) && dd < tw_len(TW_START_MM10) ) return;
			tw_used = TRUE;
			if ( dd > dm ) {
				tw_mode = TW_PINCH;	/* a pinch: nothing, for now */
				return;
			}
			tw_mode = TW_SCROLL;
			tw_across = (BOOL)( ( dx < 0 ? -dx : dx ) > ( dy < 0 ? -dy : dy ) );
			/* the move before it was known is the scroll's too, from where the pair touched */
			tw_sample(tw_td, tw_way(tw_x0, tw_y0));
		}
		tw_sample(ev->when, tw_way(ev->x, ev->y));
		tw_wheel((D)( tw_way(ev->x, ev->y) - tw_way(tw_lx, tw_ly) ) * 1000, ev->when);
		tw_lx = ev->x;
		tw_ly = ev->y;
		return;

	case TW_MANY:
		tw_ok = FALSE;
		tw_let_go(ev->when);
		return;

	case TW_UP:
		tw_let_go(ev->when);
		return;

	case TW_END:
		tw_let_go(ev->when);
		if ( tw_ok && !tw_used && ev->when - tw_t0 <= ms_ns(TW_TAP_MS) ) {
			T_HIDEV	e = tw_at;

			e.type = HID_EV_BTN_DOWN;
			e.code = 1;
			e.dz = 0;
			e.when = ev->when;
			sh_emit(&e);
			e.type = HID_EV_BTN_UP;
			sh_emit(&e);
		}
		tw_on = FALSE;
		return;

	default:				/* TW_CANCEL */
		tw_let_go(ev->when);
		tw_fling = FALSE;
		tw_on = FALSE;
		return;
	}
}

LOCAL void touch_shape( CONST T_HIDEV *ev, UB tch )
{
	T_HIDEV	e = *ev;
	INT	dx, dy, slop;

	if ( tch == TCH_TWO ) {
		two_shape(ev);
		return;
	}
	switch ( ev->type ) {
	case HID_EV_BTN_DOWN:
		if ( ev->code != 0 ) return;
		tw_fling = FALSE;		/* a touch stops what the fingers threw */
		if ( tc_state == TC_HOLD ) {
			tc_hold_shown(FALSE);
			tc_button(HID_EV_BTN_UP, 1, ev->when);
		}
		tc_down = *ev;
		tc_rx = ev->x;
		tc_ry = ev->y;
		tc_px = sh_px;
		if ( knl_wm_press_now != NULL && knl_wm_press_now(ev->x, ev->y) ) {
			tc_tapped = FALSE;
			tc_button(HID_EV_BTN_DOWN, 0, ev->when);
			tc_state = TC_DRAG;
			return;
		}
		if ( tc_tapped && ev->when >= tc_tap.when && ev->when - tc_tap.when <= ms_ns(TC_DBL_MS) ) {
			slop = tc_dbl_slop();
			dx = ev->x - tc_tap.x;
			dy = ev->y - tc_tap.y;
			if ( slop > 0 && dx <= slop && dx >= -slop && dy <= slop && dy >= -slop ) {
				tc_down.x = tc_tap.x;
				tc_down.y = tc_tap.y;
			}
		}
		tc_tapped = FALSE;
		tc_state = TC_WAIT;
		tc_due = ev->when + tc_hold_ns();
		return;

	case HID_EV_MOVE:
		if ( tc_state == TC_HOLD ) {
			e.code = ( e.code & ~1U ) | 2U;
			sh_emit(&e);
			return;
		}
		if ( tc_state == TC_WAIT ) {
			dx = ev->x - tc_rx;
			dy = ev->y - tc_ry;
			slop = tc_slop();
			if ( dx <= slop && dx >= -slop && dy <= slop && dy >= -slop ) {
				return;			/* still where it touched */
			}
			tc_button(HID_EV_BTN_DOWN, 0, ev->when);
			tc_state = TC_DRAG;
		}
		e.code = ( tc_state == TC_DRAG ) ? ( e.code | 1U ) : ( e.code & ~1U );
		sh_emit(&e);
		return;

	case HID_EV_BTN_UP:
		if ( ev->code != 0 ) return;
		if ( tch == TCH_MULTI ) {
			/* another finger: a tap of the two only when this one's press was not out yet */
			tw_cut = ( tc_state == TC_WAIT || tc_state == TC_IDLE ) ? TW_CUT_WAIT : TW_CUT_OUT;
		}
		switch ( tc_state ) {
		case TC_WAIT:
			if ( tch != TCH_CANCEL && tch != TCH_MULTI ) {
				tc_button(HID_EV_BTN_DOWN, 0, tc_down.when);
				tc_button(HID_EV_BTN_UP, 0, ev->when);
				tc_tap = tc_down;
				tc_tapped = TRUE;
			}
			break;
		case TC_HOLD:
			tc_hold_shown(FALSE);
			e.type = HID_EV_BTN_UP;
			e.code = 1;
			sh_emit(&e);
			break;
		case TC_DRAG:
			sh_emit(ev);
			break;
		default:
			break;
		}
		tc_state = TC_IDLE;
		return;

	default:
		sh_emit(ev);
		return;
	}
}

/* The time now: presses that have waited long enough count, a held key repeats */
LOCAL void shape_time( UD now )
{
	INT	i;

	if ( tw_fling ) tw_fling_time(now);

	if ( tc_state == TC_WAIT && now >= tc_due ) {
		tc_state = TC_HOLD;
		tc_hold_shown(TRUE);
		sh_px = tc_px;
		tc_button(HID_EV_BTN_DOWN, 1, now);
		sh_px = 0;
	}

	for ( i = 0; i < sh_nwait; ) {
		if ( now >= sh_wait[i].due ) {
			if ( sh_wait[i].ev.type == HID_EV_KEY_DOWN ) {
				wait_go(i);
			} else {
				T_HIDEV	e = sh_wait[i].ev;

				sh_wait[i] = sh_wait[--sh_nwait];
				sh_emit(&e);
			}
			continue;
		}
		i++;
	}
	if ( sh_rep_key != 0 && hid_attr.krp && now >= sh_rep_due ) {
		T_HIDEV	e;

		knl_memset(&e, 0, sizeof(e));
		e.type = HID_EV_KEY_DOWN;
		e.code = sh_rep_key;
		e.mods = sh_rep_mods;
		e.when = now;
		e.x = ptr_x;
		e.y = ptr_y;
		sh_emit(&e);
		sh_rep_due = now + ms_ns(hid_attr.krp_int > 0 ? hid_attr.krp_int : 100);
	}
}

/* The next time something is due, 0 when nothing is */
LOCAL UD shape_due( void )
{
	UD	due = 0;
	INT	i;

	for ( i = 0; i < sh_nwait; i++ ) {
		if ( due == 0 || sh_wait[i].due < due ) due = sh_wait[i].due;
	}
	if ( sh_rep_key != 0 && hid_attr.krp && ( due == 0 || sh_rep_due < due ) ) due = sh_rep_due;
	if ( tc_state == TC_WAIT && ( due == 0 || tc_due < due ) ) due = tc_due;
	if ( tw_fling && ( due == 0 || tw_fdue < due ) ) due = tw_fdue;
	return due;
}

EXPORT ER ts_hid_setattr( CONST T_HIDATTR *a )
{
	if ( a == NULL ) {
		return E_PAR;
	}
	hid_attr = *a;
	if ( !hid_attr.krp ) sh_rep_key = 0;
	if ( hid_attr.speed < 1 || hid_attr.speed > 15 ) hid_attr.speed = 12;
	return E_OK;
}

/*
 * The key held stops repeating: the window it was going to is gone, and
 * what it repeated would go on to whichever window has the keys next
 * (Ctrl+E closing one window and then the one behind it).
 */
EXPORT void knl_hid_repeat_cut( void )
{
	sh_rep_key = 0;
}

EXPORT ER ts_hid_getattr( T_HIDATTR *a )
{
	if ( a == NULL ) {
		return E_PAR;
	}
	*a = hid_attr;
	return E_OK;
}

/* The longest an event waited in the ring for ts_hid_read since this was last asked, ms */
EXPORT UW knl_hid_read_late( void )
{
	UW	ms = hid_late_ms;

	hid_late_ms = 0;
	return ms;
}

EXPORT ER ts_hid_read( T_HIDEV *ev, TMO tmout )
{
	T_HIDEV	raw;
	BOOL	inj;
	UB	tch;
	INT	px;
	UD	now, due, end = 0;

	if ( ev == NULL ) {
		return E_PAR;
	}
	if ( !hid_ready ) {
		return E_NOEXS;
	}
	if ( tmout > 0 ) end = now_ns() + ms_ns((UINT)tmout);
	for ( ;; ) {
		TMO	wait;

		/* what came and what is due, through the attributes */
		while ( ring_take(&raw, &inj, &tch, &px) ) {
			UD	late = now_ns() - raw.when;

			if ( !inj && late / 1000000ULL > hid_late_ms ) hid_late_ms = (UW)( late / 1000000ULL );
			tk_wai_sem(hid_semid, 1, TMO_POL);
			if ( inj ) {
				sh_emit(&raw);
			} else if ( tch != TCH_NO ) {
				sh_px = px;
				touch_shape(&raw, tch);
				sh_px = 0;
			} else {
				shape(&raw);
			}
		}
		now = now_ns();
		shape_time(now);
		if ( sh_oh != sh_ot ) {
			*ev = sh_out[sh_oh];
			sh_taken_px = sh_out_px[sh_oh];
			sh_oh = ( sh_oh + 1 ) % SH_OUT;
			return E_OK;
		}
		if ( tmout == TMO_POL ) {
			return E_TMOUT;
		}
		/* wait for the next report, or until something is due */
		due = shape_due();
		if ( tmout == TMO_FEVR ) {
			wait = ( due != 0 ) ? (TMO)( ( due > now ? due - now : 0 ) / 1000000ULL ) + 1 : TMO_FEVR;
		} else {
			UD	until = end;

			if ( now >= end ) {
				return E_TMOUT;
			}
			if ( due != 0 && due < until ) until = due;
			wait = (TMO)( ( until - now ) / 1000000ULL ) + 1;
		}
		if ( tk_wai_sem(hid_semid, 1, wait) >= E_OK ) {
			tk_sig_sem(hid_semid, 1);	/* taken again with the rest above */
		}
	}
}

/*
 * Where the pointer is. E_NOEXS when nothing is pointing -- no device
 * of that kind on the bus -- which is not the same as a pointer at the
 * corner of the screen, and is what tells the window manager not to
 * draw one.
 */
EXPORT ER ts_hid_pointer( INT *p_x, INT *p_y, UINT *p_buttons )
{
	/*
	 * A pointer there is when a device that points is plugged in, or
	 * when pointer events have been put in by hand (a test, a remote
	 * input); what is pointing then is those events, and whoever
	 * follows them has to be able to ask where they left the pointer.
	 */
	if ( !hid_ready || ( hid_count.pointers == 0 && !ptr_injected ) ) {
		return E_NOEXS;
	}
	if ( p_x != NULL )       *p_x = ptr_x;
	if ( p_y != NULL )       *p_y = ptr_y;
	if ( p_buttons != NULL ) *p_buttons = ptr_buttons;

	return E_OK;
}

EXPORT ER ts_hid_press_at( INT *p_x, INT *p_y )
{
	if ( !hid_ready || ( hid_count.pointers == 0 && !ptr_injected ) ) {
		return E_NOEXS;
	}
	if ( p_x != NULL ) *p_x = ptr_press_x;
	if ( p_y != NULL ) *p_y = ptr_press_y;

	return E_OK;
}

EXPORT ER ts_hid_stat( T_HIDSTAT *st )
{
	if ( !hid_ready || st == NULL ) {
		return E_NOEXS;
	}
	*st = hid_count;

	return E_OK;
}

EXPORT ER knl_hid_inject( UINT src, CONST T_HIDEV *ev )
{
	T_HIDEV	e;

	if ( !hid_ready || ev == NULL ) {
		return E_NOEXS;
	}
	e = *ev;
	if ( e.when == 0 ) {
		e.when = now_ns();
	}
	/* what a device's report would have changed: where the pointer is, which buttons are down */
	if ( e.type == HID_EV_MOVE ) {
		ptr_x = e.x;
		ptr_y = e.y;
		ptr_injected = TRUE;
	} else if ( e.type == HID_EV_BTN_DOWN && e.code < 32 ) {
		if ( e.code == 0 ) {
			ptr_press_x = ptr_x;
			ptr_press_y = ptr_y;
		}
		ptr_buttons |= 1U << e.code;
		ptr_injected = TRUE;
	} else if ( e.type == HID_EV_BTN_UP && e.code < 32 ) {
		ptr_buttons &= ~( 1U << e.code );
		ptr_injected = TRUE;
	}
	ring_put_as(&e, TRUE, src, TCH_NO, 0);

	return E_OK;
}

/*
 * The events copied since *pos, of the interface src (0: every one and
 * what was put in), at most max; *pos moves past what was looked at.
 * What fell out of the copy before it was looked at is passed over,
 * counted in *p_lost.
 */
EXPORT INT knl_hid_tap( UD *pos, UINT src, T_HIDEV *buf, INT max, UD *p_lost )
{
	UD	at;
	INT	n = 0;

	if ( !hid_ready || pos == NULL ) {
		return E_NOEXS;
	}
	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	/* *pos is the next to look at, plus one; 0: a new reader, from what comes next */
	at = ( *pos == 0 || *pos - 1 > tap_seq ) ? tap_seq : *pos - 1;
	if ( tap_seq - at > HID_TAP ) {
		if ( p_lost != NULL ) *p_lost += tap_seq - at - HID_TAP;
		at = tap_seq - HID_TAP;
	}
	while ( at < tap_seq && n < max ) {
		UINT	k = (UINT)( at % HID_TAP );

		if ( src == 0 || tap_src[k] == src ) {
			buf[n++] = tap_ev[k];
		}
		at++;
	}
	*pos = at + 1;
	tk_unl_mtx(hid_mtxid);
	return n;
}

/* Where a new reader of the copy starts: what comes next (as knl_hid_tap keeps *pos) */
EXPORT UD knl_hid_tap_now( void )
{
	UD	at;

	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	at = tap_seq + 1;
	tk_unl_mtx(hid_mtxid);
	return at;
}

/* The keyboards and pointers there are, at most max; answers how many */
EXPORT INT knl_hid_list( T_HIDINFO *buf, INT max )
{
	INT	i, n = 0;

	if ( !hid_ready ) {
		return 0;
	}
	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	for ( i = 0; i < HID_N; i++ ) {
		HIDDEV	*h = HID_AT(i);

		if ( !h->used || ( h->pipe == NULL && h->dev != NULL ) ) {
			continue;			/* one not on USB has no pipe of its own */
		}
		if ( n < max && buf != NULL ) {
			buf[n].id = h->id;
			buf[n].usbdev = ( h->dev != NULL ) ? h->dev->handle : 0;
			buf[n].ifno = h->ifno;
			buf[n].keyboard = ( h->is_kbd || h->cc_only );
			buf[n].absolute = ( !h->is_kbd && h->x.size > 0 && !h->x.rel );
			buf[n].wheel = ( !h->is_kbd && h->wheel.size > 0 );
			buf[n].buttons = (UINT)h->nbtn;
			buf[n].reports = h->reports;
		}
		n++;
	}
	tk_unl_mtx(hid_mtxid);
	return n;
}

/* ---------------------------------------------------------------- start-up */

/*
 * The size of the screen, from the display driver when the board has
 * one; a board without one still gets pointer positions, on a nominal
 * screen.
 */
IMPORT ER ts_disp_ref( T_DISPSPEC *spec ) __attribute__((weak));

/* The screen came to another size or scale: the pointer is kept within its view */
EXPORT void knl_hid_screen_changed( void )
{
	T_DISPSPEC	spec;

	if ( ts_disp_ref != NULL && ts_disp_ref(&spec) >= E_OK ) {
		scr_w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
		scr_h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
		if ( ptr_x >= scr_w ) ptr_x = scr_w - 1;
		if ( ptr_y >= scr_h ) ptr_y = scr_h - 1;
	}
}

/*
 * Get ready for the devices. Called before the USB manager starts, so
 * that the keyboards and pointers plugged in already are taken when it
 * first looks at the ports. Answers 0: the devices come later.
 */
EXPORT INT knl_hid_init( void )
{
	T_CMTX		cmtx;
	T_CSEM		csem;
	T_DISPSPEC	spec;

	if ( hid_ready ) {
		return 0;
	}
	if ( ts_disp_ref != NULL && ts_disp_ref(&spec) >= E_OK ) {
		scr_w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
		scr_h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	} else {
		scr_w = 640;
		scr_h = 480;
	}
	ptr_x = scr_w / 2;
	ptr_y = scr_h / 2;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	hid_mtxid = tk_cre_mtx(&cmtx);
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem = HID_RING;
	hid_semid = tk_cre_sem(&csem);
	if ( hid_mtxid <= 0 || hid_semid <= 0 ) {
		return E_LIMIT;
	}
	hid_ready = TRUE;

	return 0;
}
