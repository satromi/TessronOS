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
#include <ts/disp.h>
#include <ts/time.h>
#include "usbdev.h"

#define HID_MAX_DEV	8		/* interfaces taken at once */
#define HID_RING	64		/* events waiting to be taken */
#define HID_REPORT_MAX	64		/* the longest report read */
#define HID_RD_MAX	4096		/* the longest report descriptor read */
#define HID_RD_TRIES	3		/* times the report descriptor is asked for */
#define HID_FIELDS	64		/* input fields kept from one */
#define HID_ITEMS_MAX	1024		/* fields of one Input item looked at */
#define HID_BITS_MAX	0x10000		/* Report Size and Count taken at most */
#define HID_PUSH_MAX	4		/* global states a descriptor may push */
#define HID_SHOW_REPORTS 12		/* a pointer's first reports shown on the console */
#define HID_TAP		256		/* events kept for those who look */

#define HID_PROTO_KBD	1
#define HID_PROTO_MOUSE	2

#define HID_SET_REPORT		0x09
#define HID_SET_IDLE		0x0a
#define HID_SET_PROTOCOL	0x0b
#define HID_RT_OUT		(USB_RT_CLASS | USB_RT_INTERFACE)
#define HID_REPORT_OUTPUT	2	/* SET_REPORT's report type */

/* The usages the pointer is made of */
#define USAGE_X			0x10030
#define USAGE_Y			0x10031
#define USAGE_WHEEL		0x10038
#define USAGE_PAN		0xC0238		/* the consumer page's AC Pan: a wheel across */
#define USAGE_PAGE_BUTTON	9

/* One variable data field of an input report */
typedef struct {
	INT	off, size;	/* bit position after the report ID, width; 0 absent */
	INT	lmin, lmax;
	BOOL	rel;		/* a movement rather than a position */
	BOOL	sgn;		/* two's complement */
	INT	rid;		/* the report ID it is in, 0 none */
	UW	usage;		/* page << 16 | usage */
} HIDFIELD;

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
	UB	rids[32];	/* the report IDs its descriptor declares, a bit each */
	INT	nbtn;
	UINT	buttons;	/* held according to this interface */
	INT	lastx, lasty;
	INT	lastax, lastay;	/* a pointer that says where it is, taken as moves */
	INT	remx, remy;	/* what the speed left over of the last move */
	BOOL	mid;		/* the middle button, when it is a double press */
} HIDDEV;

LOCAL HIDDEV		hid_dev[HID_MAX_DEV];
LOCAL BOOL		hid_ready = FALSE;

LOCAL T_HIDEV		hid_ring[HID_RING];
LOCAL BOOL		hid_inj[HID_RING];	/* put in by hand: taken as it is */
LOCAL UINT		hid_head = 0, hid_tail = 0;
LOCAL ID		hid_mtxid = 0;
LOCAL ID		hid_semid = 0;

LOCAL INT		ptr_x = 0, ptr_y = 0;
LOCAL UINT		ptr_buttons = 0;
LOCAL BOOL		ptr_injected = FALSE;	/* pointer events were put in without a device */
LOCAL INT		scr_w = 0, scr_h = 0;

LOCAL T_HIDSTAT		hid_count;
LOCAL UINT		hid_next_id = 0;

LOCAL T_HIDEV		tap_ev[HID_TAP];	/* a copy of every event, for those who look */
LOCAL UINT		tap_src[HID_TAP];	/* the interface it came from, 0 put in */
LOCAL UD		tap_seq = 0;		/* events copied so far */

/* How the keyboard and the pointer are taken: until told, as the devices say */
LOCAL T_HIDATTR		hid_attr = { 0, 0, 12, 7, 0, 1, 1, 0, 0, 0, 1, 800, 100, 0, 0 };

/* ---------------------------------------------------------------- ring */

/*
 * Put one event in. When nobody has taken the oldest by the time the
 * ring fills, that one is dropped rather than the newest: what just
 * happened matters more than what happened a while ago.
 */
LOCAL void ring_put_as( CONST T_HIDEV *ev, BOOL inj, UINT src )
{
	UINT	next;

	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	tap_ev[tap_seq % HID_TAP] = *ev;
	tap_src[tap_seq % HID_TAP] = src;
	tap_seq++;
	/*
	 * A move after a move not yet taken: only where the pointer now is
	 * matters, so the waiting one is brought up to date. A pointer that
	 * keeps moving while the taker is slow then never fills the ring,
	 * and a press or a let go waiting in it is never pushed out.
	 */
	if ( ev->type == HID_EV_MOVE && hid_head != hid_tail ) {
		UINT	last = ( hid_tail + HID_RING - 1 ) % HID_RING;

		if ( hid_ring[last].type == HID_EV_MOVE && hid_ring[last].code == ev->code
		  && hid_inj[last] == inj ) {
			hid_ring[last] = *ev;
			hid_count.events++;
			tk_unl_mtx(hid_mtxid);
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
	hid_tail = next;
	hid_count.events++;
	tk_unl_mtx(hid_mtxid);
}

LOCAL void ring_put( CONST HIDDEV *d, CONST T_HIDEV *ev )
{
	ring_put_as(ev, FALSE, d->id);
}

LOCAL BOOL ring_take( T_HIDEV *ev, BOOL *p_inj )
{
	BOOL	got = FALSE;

	tk_loc_mtx(hid_mtxid, TMO_FEVR);
	if ( hid_head != hid_tail ) {
		*ev = hid_ring[hid_head];
		*p_inj = hid_inj[hid_head];
		hid_head = (hid_head + 1) % HID_RING;
		got = TRUE;
	}
	tk_unl_mtx(hid_mtxid);

	return got;
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

	for ( i = 0; i < HID_MAX_DEV; i++ ) {
		if ( hid_dev[i].used && !hid_dev[i].is_kbd ) {
			b |= hid_dev[i].buttons;
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

/* One report into what it says; FALSE for a report of another kind */
LOCAL BOOL ptr_decode( HIDDEV *d, CONST UB *r, INT len, PTRIN *o )
{
	INT	i;

	if ( boot_shaped(d, r, len) ) {
		boot_mouse(d);
		d->boot = TRUE;
		USB_LOG("usb: hid: %d sends the boot protocol's reports: read as those\n", (INT)d->id);
	}
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

	return TRUE;
}

LOCAL void ptr_report( HIDDEV *d, CONST UB *r, INT len )
{
	T_HIDEV	ev;
	UINT	buttons, changed;
	INT	i, x, y;
	BOOL	moved;
	PTRIN	in;

	if ( !ptr_decode(d, r, len, &in) ) {
		return;				/* a report of another kind */
	}
	knl_memset(&ev, 0, sizeof(ev));
	ev.when = now_ns();
	buttons = in.buttons;
	x = in.x;
	y = in.y;
	if ( !d->x.rel && !hid_attr.absolute ) {
		/* a pen taken as a mouse: where it went from where it was */
		INT	ax = abs_scale(x, &d->x, scr_w), ay = abs_scale(y, &d->y, scr_h);

		if ( d->have_last ) {
			INT	dx = ax - d->lastax, dy = ay - d->lastay;

			d->lastax = ax;
			d->lastay = ay;
			d->lastx = x;
			d->lasty = y;
			x = dx;
			y = dy;
			goto moves;
		}
		d->lastax = ax;
		d->lastay = ay;
		d->lastx = x;
		d->lasty = y;
		x = y = 0;
		goto moves;
	}
	if ( d->x.rel ) {
	moves:
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
	} else {
		ptr_x = abs_scale(x, &d->x, scr_w);
		ptr_y = abs_scale(y, &d->y, scr_h);
		/* a position moved when it differs from the last report's */
		moved = ( !d->have_last || x != d->lastx || y != d->lasty );
		d->lastx = x;
		d->lasty = y;
	}
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
	if ( ptr_x < 0 ) ptr_x = 0;
	if ( ptr_y < 0 ) ptr_y = 0;
	if ( ptr_x >= scr_w ) ptr_x = scr_w - 1;
	if ( ptr_y >= scr_h ) ptr_y = scr_h - 1;
	ev.x = ptr_x;
	ev.y = ptr_y;

	d->buttons = buttons;
	buttons = all_buttons();
	if ( !d->have_last || moved ) {
		ev.type = HID_EV_MOVE;
		ev.code = buttons;
		ring_put(d, &ev);
	}
	changed = buttons ^ ptr_buttons;
	for ( i = 0; i < 3; i++ ) {
		if ( (changed & (1U << i)) == 0 ) {
			continue;
		}
		ev.type = ( (buttons & (1U << i)) != 0 ) ? HID_EV_BTN_DOWN : HID_EV_BTN_UP;
		ev.code = (UINT)i;
		ring_put(d, &ev);
	}
	ptr_buttons = buttons;
	d->have_last = TRUE;

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

/* Whether an input field is one a pointer is made of: its axes, wheels and buttons */
LOCAL BOOL pointer_usage( UW u )
{
	return (BOOL)( u == USAGE_X || u == USAGE_Y || u == USAGE_WHEEL || u == USAGE_PAN
		       || ( u >> 16 ) == USAGE_PAGE_BUTTON );
}

/*
 * The global items in force: what Push saves and Pop brings back
 */
typedef struct {
	UW	page;
	INT	lmin, lmax;
	INT	rsize, rcount;
	INT	rid;
} HIDGLOBAL;

/*
 * A report descriptor into its variable data input fields, bit offsets
 * counted per report ID after the ID byte. Answers how many there are;
 * bits[256] is left with the length in bits of each input report,
 * without its ID byte.
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
 */
LOCAL INT hid_parse( CONST UB *dsc, INT n, HIDFIELD *out, INT max, UB *rids, INT *bits )
{
	HIDGLOBAL	g, stack[HID_PUSH_MAX];
	UW		usages[16], umin = 0, umax = 0;
	INT		nus = 0, nst = 0, i = 0, nf = 0, k;
	BOOL		range = FALSE;
	INT		*off = bits;	/* the input bit offset per report ID */

	knl_memset(off, 0, sizeof(INT) * 256);
	knl_memset(&g, 0, sizeof(g));
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
			case 0: g.page = val; break;
			case 1: g.lmin = sval; break;
			case 2: g.lmax = ( g.lmin < 0 ) ? sval : (INT)val; break;
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
		if ( tag == 8 ) {		/* Input */
			rids[g.rid >> 3] |= (UB)( 1 << ( g.rid & 7 ) );
			for ( k = 0; k < g.rcount && k < HID_ITEMS_MAX; k++ ) {
				UW	u;

				if ( range ) {
					u = ( umin + k <= umax ) ? umin + k : umax;
				} else {
					u = ( k < nus ) ? usages[k]
					    : ( nus > 0 ? usages[nus - 1] : 0 );
				}
				/* data, not constant, and variable, not an array */
				if ( (val & 3) == 2 && nf < max && g.rsize >= 1 && g.rsize <= 32
				  && pointer_usage(u) ) {
					HIDFIELD	*o = &out[nf++];

					o->off = off[g.rid] + k * g.rsize;
					o->size = g.rsize;
					o->lmin = g.lmin;
					o->lmax = g.lmax;
					o->rel = ( (val & 4) != 0 );
					o->sgn = ( g.lmin < 0 )
					      || ( o->rel && g.rsize > 1 && (u >> 16) != USAGE_PAGE_BUTTON );
					o->rid = g.rid;
					o->usage = u;
				}
			}
			{
				D	end = (D)off[g.rid] + (D)g.rsize * g.rcount;

				off[g.rid] = ( end > HID_BITS_MAX * 64 ) ? HID_BITS_MAX * 64 : (INT)end;
			}
		}
		if ( tag == 8 || tag == 9 || tag == 0xb || tag == 0xa || tag == 0xc ) {
			nus = 0;
			range = FALSE;
		}
	}
	return nf;
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

/* The length in bytes of input report rid, its ID byte included; 0 none */
LOCAL INT report_bytes( CONST INT *bits, INT rid )
{
	if ( bits[rid] == 0 ) {
		return 0;
	}

	return ( bits[rid] + 7 ) / 8 + ( rid != 0 ? 1 : 0 );
}

/*
 * An interface's report descriptor: its pointer, the report IDs it
 * declares, how long its reports are. FALSE when it has no pointer.
 */
LOCAL BOOL hid_read_desc( HIDDEV *h, CONST UB *rd, INT n )
{
	HIDFIELD	*fields;
	INT		*bits;
	BOOL		found = FALSE;
	INT		k;

	fields = (HIDFIELD *)Kmalloc(sizeof(HIDFIELD) * HID_FIELDS);
	bits = (INT *)Kmalloc(sizeof(INT) * 256);
	if ( fields != NULL && bits != NULL ) {
		found = pick_pointer(h, fields, hid_parse(rd, n, fields, HID_FIELDS, h->rids, bits));
		h->rlen = 0;
		for ( k = 0; k < 256; k++ ) {
			if ( report_bytes(bits, k) > h->rlen ) {
				h->rlen = report_bytes(bits, k);
			}
		}
		h->plen = found ? report_bytes(bits, h->report_id) : 0;
	}
	if ( fields != NULL ) Kfree(fields);
	if ( bits != NULL ) Kfree(bits);

	return found;
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
	found = hid_read_desc(&h, dsc, n);
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

	return found;
}
#endif

/* ---------------------------------------------------------------- streams */

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
	hid_count.reports++;
	d->reports++;
	if ( !d->is_kbd && d->reports <= HID_SHOW_REPORTS ) {
		/* a pointer's first reports as they came, for the console */
		char	line[3 * 24 + 48];
		INT	k, m;

		m = tm_sprintf((UB *)line, (UB *)"usb: hid: %d report %d:", (INT)d->id, (INT)d->reports);
		for ( k = 0; k < len && k < 24; k++ ) {
			m += tm_sprintf((UB *)line + m, (UB *)" %02x", (INT)r[k]);
		}
		USB_LOG("%s\n", line);
	}
	if ( d->is_kbd ) {
		kbd_report(d, r);
	} else {
		ptr_report(d, r, len);
	}
}

/* ---------------------------------------------------------------- attach */

EXPORT BOOL knl_hid_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( !hid_ready || dev->cfg == NULL ) {
		return FALSE;
	}
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
	INT	i;

	for ( i = 0; i < HID_MAX_DEV; i++ ) {
		if ( !hid_dev[i].used ) {
			return &hid_dev[i];
		}
	}

	return NULL;
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
		return E_LIMIT;
	}
	knl_memset(h, 0, sizeof(*h));

	if ( sub == 1 && proto == HID_PROTO_KBD ) {
		h->is_kbd = TRUE;
		knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 0, (UH)ifno,
				NULL, 0, NULL);
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
				found = hid_read_desc(h, rd, n);
			}
			if ( rd != NULL ) Kfree(rd);
		}
		if ( !found ) {
			if ( !( sub == 1 && proto == HID_PROTO_MOUSE ) ) {
				return E_NOSPT;	/* neither a keyboard nor a pointer */
			}
			boot_mouse(h);
			h->boot = TRUE;
			knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 0, (UH)ifno,
					NULL, 0, NULL);
		} else if ( sub == 1 ) {
			h->bootable = TRUE;
			/*
			 * Read as its descriptor says: the report protocol asked
			 * for, since a device that can talk the boot protocol may
			 * have been left in it (the board's boot loader reads the
			 * keyboard and the mouse that way) and would go on sending
			 * the boot protocol's reports, without the report ID.
			 */
			knl_usb_control(dev, HID_RT_OUT, HID_SET_PROTOCOL, 1, (UH)ifno,
					NULL, 0, NULL);
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
	if ( h->is_kbd ) {
		hid_count.keyboards++;
	} else {
		hid_count.pointers++;
	}
	USB_LOG("usb: hid: %s on interface %d, endpoint %02x, %s\n",
		h->is_kbd ? "keyboard" : "pointer", ifno, (INT)ep.addr,
		h->is_kbd ? "boot protocol" : ( h->x.rel ? "relative" : "absolute" ));
	if ( !h->is_kbd ) {
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
	if ( n > 0 ) {
		dev->hid = &hid_dev[0];
		return E_OK;
	}

	return er;
}

/* The device went: its keys and buttons are let go, as they would be */
EXPORT void knl_hid_detach( USBDEV *dev )
{
	INT	i;

	for ( i = 0; i < HID_MAX_DEV; i++ ) {
		HIDDEV	*h = &hid_dev[i];

		if ( !h->used || h->dev != dev ) {
			continue;
		}
		if ( h->pipe != NULL ) {
			dev->hc->ops->stream_close(dev->hc, h->pipe);
			h->pipe = NULL;
		}
		if ( h->is_kbd ) {
			UB	none[8];

			knl_memset(none, 0, sizeof(none));
			kbd_report(h, none);
			hid_count.keyboards--;
		} else {
			T_HIDEV	ev;
			UINT	b;
			INT	k;

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
					ring_put(h, &ev);
				}
			}
			ptr_buttons = b;
			hid_count.pointers--;
		}
		h->used = FALSE;
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
} SHWAIT;

LOCAL T_HIDEV	sh_out[SH_OUT];
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

/* A key that counts: sent, with the shifts that stick, and made the one that repeats */
LOCAL void key_counts( T_HIDEV *ev )
{
	if ( !is_mod_key(ev->code) ) {
		ev->mods |= sh_sticky;
		if ( sh_sticky != 0 && !sh_locked ) sh_sticky = 0;	/* one-shot: used */
		sh_shift_alone = FALSE;
		if ( hid_attr.krp ) {
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
			sh_emit(ev);
			return;
		}
		sh_rep_key = 0;				/* another key: the repeat stops */
		if ( hid_attr.key_on > 0 || hid_attr.key_sim > 0 ) {
			if ( sh_nwait < SH_WAIT ) {
				UINT	wait = ( hid_attr.key_on > hid_attr.key_sim ) ? hid_attr.key_on : hid_attr.key_sim;

				sh_wait[sh_nwait].ev = *ev;
				sh_wait[sh_nwait].due = ev->when + ms_ns(wait);
				sh_wait[sh_nwait].up = FALSE;
				sh_nwait++;
			}
			return;
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
				key_counts(&sh_wait[i].ev);
				sh_wait[i] = sh_wait[--sh_nwait];
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

/* The time now: presses that have waited long enough count, a held key repeats */
LOCAL void shape_time( UD now )
{
	INT	i;

	for ( i = 0; i < sh_nwait; ) {
		if ( now >= sh_wait[i].due ) {
			T_HIDEV	e = sh_wait[i].ev;

			sh_wait[i] = sh_wait[--sh_nwait];
			if ( e.type == HID_EV_KEY_DOWN ) {
				key_counts(&e);
			} else {
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

EXPORT ER ts_hid_getattr( T_HIDATTR *a )
{
	if ( a == NULL ) {
		return E_PAR;
	}
	*a = hid_attr;
	return E_OK;
}

EXPORT ER ts_hid_read( T_HIDEV *ev, TMO tmout )
{
	T_HIDEV	raw;
	BOOL	inj;
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
		while ( ring_take(&raw, &inj) ) {
			tk_wai_sem(hid_semid, 1, TMO_POL);
			if ( inj ) sh_emit(&raw);
			else shape(&raw);
		}
		now = now_ns();
		shape_time(now);
		if ( sh_oh != sh_ot ) {
			*ev = sh_out[sh_oh];
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
		ptr_buttons |= 1U << e.code;
		ptr_injected = TRUE;
	} else if ( e.type == HID_EV_BTN_UP && e.code < 32 ) {
		ptr_buttons &= ~( 1U << e.code );
		ptr_injected = TRUE;
	}
	ring_put_as(&e, TRUE, src);

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
	for ( i = 0; i < HID_MAX_DEV; i++ ) {
		HIDDEV	*h = &hid_dev[i];

		if ( !h->used || h->pipe == NULL ) {
			continue;
		}
		if ( n < max && buf != NULL ) {
			buf[n].id = h->id;
			buf[n].usbdev = ( h->dev != NULL ) ? h->dev->handle : 0;
			buf[n].ifno = h->ifno;
			buf[n].keyboard = h->is_kbd;
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

/* The screen came to another size: the pointer is kept within it */
EXPORT void knl_hid_screen_changed( void )
{
	T_DISPSPEC	spec;

	if ( ts_disp_ref != NULL && ts_disp_ref(&spec) >= E_OK ) {
		scr_w = (INT)spec.width;
		scr_h = (INT)spec.height;
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
		scr_w = (INT)spec.width;
		scr_h = (INT)spec.height;
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
