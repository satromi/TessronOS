/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_usb.c
 *	The USB host stack (design 10.14).
 *
 *	The machine the tests run on has two controllers:
 *	  xhci	  a keyboard and a tablet (high speed), a disk on a USB 3 port
 *		  (SuperSpeed), and a hub (full speed) with an audio function,
 *		  a removable disk and a mouse behind it
 *	  xhci2	  USB 2 ports only: a disk that comes up at high speed, a
 *		  keyboard, and two disks without a partition table (a FAT32
 *		  volume of 34MB and a 1.44MB floppy image), all high speed
 *	so every speed but low, a hub, and bulk, interrupt and isochronous
 *	endpoints are all there. The test asks the QEMU monitor (through
 *	tools/qemu_qmp.py) to take devices out and plug them back in, and to
 *	press a key; without the monitor those tests are skipped.
 *
 *	On the Raspberry Pi 5 the devices are the user's: what is plugged in
 *	is not known, and a disk there is not one the tests may write on.
 *	Only the controllers and a control transfer to a keyboard are
 *	tried there.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/usb.h>
#include <ts/hid.h>
#include <ts/blk.h>
#include <ts/fs.h>

#define MAXDEV		32

LOCAL T_USBDEV	devs[MAXDEV];
LOCAL INT	ndev = 0;

LOCAL void load_tree( void )
{
	ndev = ts_usb_lst_dev(devs, MAXDEV);
	if ( ndev > MAXDEV ) {
		ndev = MAXDEV;
	}
}

/* The first device of a role at a speed; speed 0 for any */
LOCAL T_USBDEV *find_dev( UINT role, UINT speed )
{
	INT	i;

	for ( i = 0; i < ndev; i++ ) {
		if ( devs[i].role == role && ( speed == 0 || devs[i].speed == speed ) ) {
			return &devs[i];
		}
	}

	return NULL;
}

#ifndef RPI5
LOCAL T_USBDEV *find_by_handle( INT h )
{
	INT	i;

	for ( i = 0; i < ndev; i++ ) {
		if ( devs[i].dev == h ) {
			return &devs[i];
		}
	}

	return NULL;
}
#endif /* RPI5 */

/* ---------------------------------------------------------------- controllers */

LOCAL void test_hc( void )
{
	T_USBHC	hc;
	INT	i, n = ts_usb_hc_count();

	if ( n <= 0 ) {
		KT_SKIP("the machine has no USB controller");
	}
	KT_ASSERT(n >= 2);
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(ts_usb_ref_hc(i, &hc), E_OK);
		KT_ASSERT_EQ(hc.running, 1);
		KT_ASSERT_EQ(hc.version >> 8, 1);	/* version 1.x */
		KT_ASSERT(hc.nports > 0);
		KT_ASSERT(hc.nslots > 0);
		KT_ASSERT(hc.ctx_size == 32 || hc.ctx_size == 64);
		/* a message interrupt each, edge triggered (design 10.13); the
		   RP1's controllers are polled */
#ifndef RPI5
		KT_ASSERT(hc.intno != 0);
#endif
		tm_printf((UB*)"  xhci%d %d.%d, %d ports, %d slots, interrupt %d\n", i,
			  (INT)(hc.version >> 8), (INT)((hc.version >> 4) & 0xf),
			  (INT)hc.nports, (INT)hc.nslots, (INT)hc.intno);
	}
	KT_ASSERT_ER(ts_usb_ref_hc(n, &hc), E_NOEXS);
	KT_ASSERT_ER(ts_usb_ref_hc(0, NULL), E_PAR);
}

#ifndef RPI5
/* every root port answers; the USB 3 ones say so */
LOCAL void test_ports( void )
{
	T_USBHC		hc;
	T_USBPORT	p;
	UINT		i;
	INT		connected = 0, usb3 = 0;

	if ( ts_usb_hc_count() <= 0 ) KT_SKIP("no controller");

	KT_ASSERT_ER(ts_usb_ref_hc(0, &hc), E_OK);
	for ( i = 0; i < hc.nports; i++ ) {
		KT_ASSERT_ER(ts_usb_ref_port(0, i, &p), E_OK);
		KT_ASSERT_EQ(p.powered, 1);
		if ( p.usb3 ) {
			usb3++;
		}
		if ( p.connected ) {
			connected++;
			KT_ASSERT_EQ(p.enabled, 1);	/* brought up by the stack */
			KT_ASSERT(p.speed != USB_SPEED_NONE);
		}
	}
	KT_ASSERT(connected >= 4);	/* keyboard, tablet, disk, hub */
	KT_ASSERT(usb3 > 0);
	KT_ASSERT_ER(ts_usb_ref_port(0, hc.nports, &p), E_PAR);
	KT_ASSERT_ER(ts_usb_ref_port(9, 0, &p), E_NOEXS);
}
#endif /* RPI5 */

/* ---------------------------------------------------------------- the tree */

#ifndef RPI5
LOCAL CONST char *role_name( UINT r )
{
	switch ( r ) {
	case USB_ROLE_HUB:	return "hub";
	case USB_ROLE_HID:	return "hid";
	case USB_ROLE_STORAGE:	return "storage";
	case USB_ROLE_AUDIO:	return "audio";
	default:		return "-";
	}
}

/*
 * Everything plugged in was brought up and given to its driver, at the
 * speed of the port it is on, with the hub's children under it.
 */
LOCAL void test_tree( void )
{
	T_USBDEV	*hub, *aud, *d;
	INT		i, nhid = 0, nstor = 0;

	if ( ts_usb_hc_count() <= 0 ) KT_SKIP("no controller");
	load_tree();
	for ( i = 0; i < ndev; i++ ) {
		d = &devs[i];
		tm_printf((UB*)"  %d-%d port %d depth %d route %05x speed %d addr %d "
			  "%04x:%04x class %02x %s %s %s\n", (INT)d->hc,
			  (INT)d->root_port, (INT)d->port, (INT)d->depth, d->route,
			  (INT)d->speed, (INT)d->addr, (INT)d->vendor, (INT)d->product,
			  (INT)d->dev_class, role_name(d->role), d->devnm, d->name);
		KT_ASSERT(d->dev > 0);
		KT_ASSERT(d->addr >= 1 && d->addr <= 127);
		if ( d->role == USB_ROLE_HID ) nhid++;
		if ( d->role == USB_ROLE_STORAGE ) nstor++;
		/* parents come before their children */
		if ( d->parent != 0 ) {
			T_USBDEV	*p = find_by_handle(d->parent);

			KT_ASSERT(p != NULL);
			if ( p != NULL ) {
				KT_ASSERT(p < d);
				KT_ASSERT_EQ(p->role, USB_ROLE_HUB);
				KT_ASSERT_EQ(d->depth, p->depth + 1);
				KT_ASSERT_EQ(d->root_port, p->root_port);
			}
		}
	}
	KT_ASSERT(ndev >= 10);
	KT_ASSERT(nhid >= 4);		/* two keyboards, the tablet, the mouse */
	KT_ASSERT_EQ(nstor, 5);

	/* a disk at each of the three speeds of the machine */
	KT_ASSERT(find_dev(USB_ROLE_STORAGE, USB_SPEED_SUPER) != NULL);
	KT_ASSERT(find_dev(USB_ROLE_STORAGE, USB_SPEED_HIGH) != NULL);
	KT_ASSERT(find_dev(USB_ROLE_STORAGE, USB_SPEED_FULL) != NULL);
	KT_ASSERT(find_dev(USB_ROLE_HID, USB_SPEED_HIGH) != NULL);

	/* the hub, and the audio function behind it on its first port */
	hub = find_dev(USB_ROLE_HUB, 0);
	KT_ASSERT(hub != NULL);
	aud = find_dev(USB_ROLE_AUDIO, 0);
	KT_ASSERT(aud != NULL);
	if ( hub != NULL && aud != NULL ) {
		KT_ASSERT(hub->hub_ports >= 3);
		KT_ASSERT_EQ(aud->parent, hub->dev);
		KT_ASSERT_EQ(aud->depth, 1);
		KT_ASSERT_EQ(aud->port, 1);
		KT_ASSERT_EQ(aud->route, 1);
		KT_ASSERT_EQ(aud->speed, USB_SPEED_FULL);
	}

	/* the whole list, and a list cut short */
	KT_ASSERT_EQ(ts_usb_lst_dev(NULL, 0), ndev);
	KT_ASSERT_EQ(ts_usb_lst_dev(devs, 2), ndev);
	load_tree();
}
#endif /* RPI5 */

/* A device answers a request over its control endpoint */
LOCAL void test_control( void )
{
	T_USBDEV	*d, r;
	UB		desc[18], cfg[64], str[64], setup[8];
	INT		n;

	if ( ts_usb_hc_count() <= 0 ) KT_SKIP("no controller");
	load_tree();
	d = find_dev(USB_ROLE_HID, USB_SPEED_HIGH);
	if ( d == NULL ) KT_SKIP("no keyboard");

	n = ts_usb_get_desc(d->dev, 1, 0, desc, sizeof(desc));
	KT_ASSERT_EQ(n, 18);
	KT_ASSERT_EQ(desc[0], 18);
	KT_ASSERT_EQ(desc[1], 1);
	KT_ASSERT_EQ(desc[8] | (desc[9] << 8), d->vendor);
	KT_ASSERT_EQ(desc[10] | (desc[11] << 8), d->product);
	KT_ASSERT(desc[7] == 64);		/* high speed: 64 byte control packets */

	n = ts_usb_get_desc(d->dev, 2, 0, cfg, sizeof(cfg));
	KT_ASSERT(n >= 9);
	KT_ASSERT_EQ(cfg[1], 2);

	/* the product string, UTF-16 */
	if ( desc[15] != 0 ) {
		n = ts_usb_get_desc(d->dev, 3, desc[15], str, sizeof(str));
		KT_ASSERT(n >= 4);
		KT_ASSERT_EQ(str[1], 3);
	}

	/* a request the device does not know is refused, and it goes on */
	setup[0] = 0xc0; setup[1] = 0x55; setup[2] = 0; setup[3] = 0;
	setup[4] = 0; setup[5] = 0; setup[6] = 8; setup[7] = 0;
	KT_ASSERT_EQ(ts_usb_control(d->dev, setup, str, 8), E_OBJ);
	KT_ASSERT_EQ(ts_usb_get_desc(d->dev, 1, 0, desc, 8), 8);

	/* a handle that is no device */
	KT_ASSERT_EQ(ts_usb_get_desc(0x7fff00, 1, 0, desc, 8), E_NOEXS);
	KT_ASSERT_ER(ts_usb_ref_dev(d->dev, &r), E_OK);
	KT_ASSERT_EQ(r.vendor, d->vendor);
	KT_ASSERT_ER(ts_usb_ref_dev(0, &r), E_NOEXS);
}

/* ---------------------------------------------------------------- HID */

#ifndef RPI5
LOCAL void drain_hid( void )
{
	T_HIDEV	ev;

	while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) {
		;
	}
}

/*
 * The keyboards and pointers were taken by the input driver, and a key
 * pressed on the emulated keyboard comes out of it as a press and a
 * release.
 */
LOCAL void test_hid( void )
{
	T_HIDSTAT	hs, hs2;
	T_HIDEV		ev;
	INT		x, y, got_down = 0, got_up = 0, i;
	UINT		b;

	if ( ts_hid_stat(&hs) < E_OK ) KT_SKIP("no input driver");
	KT_ASSERT(hs.keyboards >= 2);
	KT_ASSERT(hs.pointers >= 2);
	KT_ASSERT_ER(ts_hid_pointer(&x, &y, &b), E_OK);

	drain_hid();
	tm_printf((UB*)"KTEST QMP {\"execute\":\"input-send-event\",\"arguments\":"
		  "{\"events\":[{\"type\":\"key\",\"data\":"
		  "{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"a\"}}}]}}\n");
	for ( i = 0; i < 20 && !got_down; i++ ) {
		if ( ts_hid_read(&ev, 500) >= E_OK && ev.type == HID_EV_KEY_DOWN
		  && ev.code == 0x04 ) {
			got_down = 1;
		}
	}
	if ( !got_down ) {
		KT_SKIP("no QEMU monitor to press a key with");
	}
	tm_printf((UB*)"KTEST QMP {\"execute\":\"input-send-event\",\"arguments\":"
		  "{\"events\":[{\"type\":\"key\",\"data\":"
		  "{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"a\"}}}]}}\n");
	for ( i = 0; i < 20 && !got_up; i++ ) {
		if ( ts_hid_read(&ev, 500) >= E_OK && ev.type == HID_EV_KEY_UP
		  && ev.code == 0x04 ) {
			got_up = 1;
		}
	}
	KT_ASSERT(got_up);

	/* the tablet: a place across the whole screen */
	tm_printf((UB*)"KTEST QMP {\"execute\":\"input-send-event\",\"arguments\":"
		  "{\"events\":[{\"type\":\"abs\",\"data\":"
		  "{\"axis\":\"x\",\"value\":16384}},{\"type\":\"abs\",\"data\":"
		  "{\"axis\":\"y\",\"value\":8192}}]}}\n");
	for ( i = 0; i < 20; i++ ) {
		if ( ts_hid_read(&ev, 500) >= E_OK && ev.type == HID_EV_MOVE ) {
			break;
		}
	}
	KT_ASSERT(i < 20);
	KT_ASSERT_ER(ts_hid_pointer(&x, &y, &b), E_OK);
	tm_printf((UB*)"  tablet moved the pointer to %d,%d\n", x, y);
	KT_ASSERT(x > 0 && y > 0);
	KT_ASSERT(x > y);

	KT_ASSERT_ER(ts_hid_stat(&hs2), E_OK);
	KT_ASSERT(hs2.reports > hs.reports);
	drain_hid();
}
#endif /* RPI5 */

/*
 * Report descriptors of the kinds of pointer there are, read as the
 * input driver reads a device's, and reports of each decoded
 */
IMPORT BOOL knl_hid_probe( CONST UB *dsc, INT n, BOOL bootable, INT *info,
			   CONST UB *reps, CONST INT *lens, INT nrep, INT *out );

enum { PI_RID, PI_RLEN, PI_XOFF, PI_XSIZE, PI_XREL, PI_XSGN, PI_YOFF, PI_YSIZE,
       PI_NBTN, PI_BOFF, PI_WOFF, PI_WSIZE, PI_POFF, PI_PSIZE, PI_BOOT, PI_PLEN, PI_N };

/* A cheap mouse: three buttons, a byte an axis and a wheel, no report ID */
LOCAL CONST UB rd_plain[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f,
	0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0
};

/*
 * A gaming mouse: report 1 with five buttons, 16-bit axes, a wheel and
 * AC Pan; report 2 an absolute pointer (for macros); report 3 the
 * consumer keys as an array
 */
LOCAL CONST UB rd_gaming[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
	0x05, 0x01, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
	0x09, 0x30, 0x09, 0x31, 0x81, 0x06,
	0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
	0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0, 0xc0,
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x26, 0xff, 0x03,
	0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0
};

/* A receiver's mouse without a report ID: 16-bit axes, so not the boot layout */
LOCAL CONST UB rd_noid16[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
	0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	0xc0, 0xc0
};

/* Axes that say their least is 0 while moving both ways */
LOCAL CONST UB rd_unsigned[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0
};

/*
 * A vendor's 300 bits between Push and Pop: the axes after them are on
 * the generic desktop page again, and placed after all 300
 */
LOCAL CONST UB rd_push[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x05, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x01,
	0x05, 0x01,
	0xa4,
	0x06, 0x00, 0xff, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x96, 0x2c, 0x01, 0x81, 0x02,
	0xb4,
	0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0
};

LOCAL void test_hid_desc( void )
{
	INT	info[PI_N], out[4 * 4];
	INT	lens[4];

	/* the cheap mouse: the boot layout, and its reports as they are */
	{
		static CONST UB r[] = { 0x01, 0x05, 0xfb, 0x01 };

		lens[0] = 4;
		KT_ASSERT(knl_hid_probe(rd_plain, sizeof(rd_plain), TRUE, info, r, lens, 1, out));
		KT_ASSERT_EQ(info[PI_RID], 0);
		KT_ASSERT_EQ(info[PI_RLEN], 4);
		KT_ASSERT_EQ(info[PI_XOFF], 8);
		KT_ASSERT_EQ(info[PI_XSIZE], 8);
		KT_ASSERT(info[PI_XREL] && info[PI_XSGN]);
		KT_ASSERT_EQ(info[PI_YOFF], 16);
		KT_ASSERT_EQ(info[PI_NBTN], 3);
		KT_ASSERT_EQ(info[PI_WOFF], 24);
		KT_ASSERT_EQ(info[PI_WSIZE], 8);
		KT_ASSERT_EQ(info[PI_BOOT], 0);
		KT_ASSERT_EQ(out[0], 1);
		KT_ASSERT_EQ(out[1], 5);
		KT_ASSERT_EQ(out[2], -5);
		KT_ASSERT_EQ(out[3], 1);
	}

	/* the gaming mouse: the relative report, not the absolute one */
	{
		static CONST UB r[] = {
			0x01, 0x02, 0x30, 0x00, 0xf6, 0xff, 0xff, 0x00,
			0x03, 0xe9, 0x00,
			0x02, 0x00, 0x40, 0x00, 0x20
		};

		lens[0] = 8;
		lens[1] = 3;
		lens[2] = 5;
		KT_ASSERT(knl_hid_probe(rd_gaming, sizeof(rd_gaming), TRUE, info, r, lens, 3, out));
		KT_ASSERT_EQ(info[PI_RID], 1);
		KT_ASSERT_EQ(info[PI_RLEN], 8);
		KT_ASSERT_EQ(info[PI_PLEN], 8);
		KT_ASSERT_EQ(info[PI_XOFF], 8);
		KT_ASSERT_EQ(info[PI_XSIZE], 16);
		KT_ASSERT(info[PI_XREL] && info[PI_XSGN]);
		KT_ASSERT_EQ(info[PI_YOFF], 24);
		KT_ASSERT_EQ(info[PI_NBTN], 5);
		KT_ASSERT_EQ(info[PI_BOFF], 0);
		KT_ASSERT_EQ(info[PI_WOFF], 40);
		KT_ASSERT_EQ(info[PI_POFF], 48);
		KT_ASSERT_EQ(info[PI_PSIZE], 8);
		KT_ASSERT_EQ(out[0], 2);
		KT_ASSERT_EQ(out[1], 48);
		KT_ASSERT_EQ(out[2], -10);
		KT_ASSERT_EQ(out[3], -1);
		KT_ASSERT_EQ(out[4], -1);		/* the consumer keys */
		KT_ASSERT_EQ(out[8], -1);		/* the absolute pointer */
		KT_ASSERT_EQ(info[PI_BOOT], 0);
	}

	/*
	 * The same mouse left in the boot protocol: a report with the first
	 * button held starts with the pointer's own ID, but its axes do not
	 * fit; one with no button starts with no ID at all.
	 */
	{
		static CONST UB r[] = { 0x01, 0x05, 0xfb, 0x00 };
		static CONST UB r0[] = { 0x00, 0xfd, 0x02, 0x00 };

		lens[0] = 4;
		KT_ASSERT(knl_hid_probe(rd_gaming, sizeof(rd_gaming), TRUE, info, r, lens, 1, out));
		KT_ASSERT_EQ(info[PI_BOOT], 1);
		KT_ASSERT_EQ(info[PI_PSIZE], 0);
		KT_ASSERT_EQ(out[0], 1);
		KT_ASSERT_EQ(out[1], 5);
		KT_ASSERT_EQ(out[2], -5);
		KT_ASSERT(knl_hid_probe(rd_gaming, sizeof(rd_gaming), TRUE, info, r0, lens, 1, out));
		KT_ASSERT_EQ(info[PI_BOOT], 1);
		KT_ASSERT_EQ(out[0], 0);
		KT_ASSERT_EQ(out[1], -3);
		KT_ASSERT_EQ(out[2], 2);
		/* not of the boot subclass: never taken as the boot protocol */
		KT_ASSERT(knl_hid_probe(rd_gaming, sizeof(rd_gaming), FALSE, info, r0, lens, 1, out));
		KT_ASSERT_EQ(info[PI_BOOT], 0);
		KT_ASSERT_EQ(out[0], -1);
	}

	/* no report ID and 16-bit axes: the report protocol, then the boot one */
	{
		static CONST UB r[] = { 0x00, 0x05, 0x00, 0xfb, 0xff, 0x00 };
		static CONST UB rb[] = { 0x00, 0x05, 0xfb, 0x00 };

		lens[0] = 6;
		KT_ASSERT(knl_hid_probe(rd_noid16, sizeof(rd_noid16), TRUE, info, r, lens, 1, out));
		KT_ASSERT_EQ(info[PI_RID], 0);
		KT_ASSERT_EQ(info[PI_RLEN], 6);
		KT_ASSERT_EQ(info[PI_NBTN], 8);
		KT_ASSERT_EQ(info[PI_BOOT], 0);
		KT_ASSERT_EQ(out[1], 5);
		KT_ASSERT_EQ(out[2], -5);
		lens[0] = 4;
		KT_ASSERT(knl_hid_probe(rd_noid16, sizeof(rd_noid16), TRUE, info, rb, lens, 1, out));
		KT_ASSERT_EQ(info[PI_BOOT], 1);
		KT_ASSERT_EQ(out[1], 5);
		KT_ASSERT_EQ(out[2], -5);
	}

	/* a relative axis whose least is said to be 0 still moves both ways */
	{
		static CONST UB r[] = { 0x00, 0xff, 0x01 };

		lens[0] = 3;
		KT_ASSERT(knl_hid_probe(rd_unsigned, sizeof(rd_unsigned), TRUE, info, r, lens, 1, out));
		KT_ASSERT(info[PI_XSGN]);
		KT_ASSERT_EQ(out[1], -1);
		KT_ASSERT_EQ(out[2], 1);
	}

	/* Push and Pop, and a field of more than 256 counted in full */
	{
		static CONST UB r[] = {
			0x05, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0xa0, 0xff, 0x00
		};

		lens[0] = sizeof(r);
		KT_ASSERT(knl_hid_probe(rd_push, sizeof(rd_push), FALSE, info, r, lens, 1, out));
		KT_ASSERT_EQ(info[PI_RID], 5);
		KT_ASSERT_EQ(info[PI_NBTN], 3);
		KT_ASSERT_EQ(info[PI_XOFF], 308);
		KT_ASSERT_EQ(info[PI_YOFF], 316);
		KT_ASSERT_EQ(info[PI_RLEN], 42);
		KT_ASSERT_EQ(info[PI_PLEN], 42);
		KT_ASSERT_EQ(out[0], 2);
	}
}

/* ---------------------------------------------------------------- disks */

#ifndef RPI5
LOCAL CONST char *disk_of( UINT speed )
{
	T_USBDEV	*d;

	load_tree();
	d = find_dev(USB_ROLE_STORAGE, speed);

	return ( d != NULL ) ? (CONST char *)d->devnm : NULL;
}

/* The disk at a speed with 'blocks' blocks: several are high speed */
LOCAL CONST char *disk_sized( UINT speed, UD blocks )
{
	DiskInfo	di;
	SZ		asz;
	ID		dd;
	INT		i;

	load_tree();
	for ( i = 0; i < ndev; i++ ) {
		if ( devs[i].role != USB_ROLE_STORAGE || devs[i].speed != speed ) continue;
		dd = tk_opn_dev(devs[i].devnm, TD_READ);
		if ( dd <= 0 ) continue;
		asz = 0;
		if ( tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK
		  && (UD)di.blockcont == blocks ) {
			tk_cls_dev(dd, 0);
			return (CONST char *)devs[i].devnm;
		}
		tk_cls_dev(dd, 0);
	}

	return NULL;
}

/* Blocks written, read back and compared, over pieces a command carries */
LOCAL void rw_check( ID dd, W start, INT nblk, UB seed )
{
	UB	*wb, *rb;
	SZ	asz = 0;
	INT	i, len = nblk * BLK_SECTOR_SIZE, bad = 0;

	wb = (UB *)Kmalloc((SZ)len);
	rb = (UB *)Kmalloc((SZ)len);
	KT_ASSERT(wb != NULL && rb != NULL);
	if ( wb == NULL || rb == NULL ) {
		if ( wb != NULL ) Kfree(wb);
		if ( rb != NULL ) Kfree(rb);
		return;
	}
	for ( i = 0; i < len; i++ ) {
		wb[i] = (UB)(seed + i * 7 + (i >> 9));
		rb[i] = 0;
	}
	KT_ASSERT_ER(tk_swri_dev(dd, start, wb, len, &asz), E_OK);
	KT_ASSERT_EQ(asz, len);
	KT_ASSERT_ER(tk_srea_dev(dd, start, rb, len, &asz), E_OK);
	KT_ASSERT_EQ(asz, len);
	for ( i = 0; i < len; i++ ) {
		if ( wb[i] != rb[i] ) {
			bad++;
		}
	}
	KT_ASSERT_EQ(bad, 0);
	Kfree(wb);
	Kfree(rb);
}

/*
 * The disk on the SuperSpeed port: its size, its partition table as
 * subunits, and a round trip through the scratch area left outside the
 * partition, large enough to take two commands.
 */
LOCAL void test_msc_ss( void )
{
	CONST char	*nm = disk_of(USB_SPEED_SUPER);
	DiskInfo	di;
	UB		sec[512], name[12];
	SZ		asz = 0;
	ID		dd;
	UD		start;
	INT		k;

	if ( nm == NULL ) KT_SKIP("no SuperSpeed disk");
	tm_printf((UB*)"  SuperSpeed disk is %s\n", nm);
	dd = tk_opn_dev((UB *)nm, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;

	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
	KT_ASSERT_EQ(di.blocksize, 512);
	KT_ASSERT_EQ(di.blockcont, 40 * 2048);
	KT_ASSERT_EQ(di.protect, 0);

	/* the MBR, with its one FAT32 partition */
	KT_ASSERT_ER(tk_srea_dev(dd, 0, sec, 512, &asz), E_OK);
	KT_ASSERT_EQ(sec[510], 0x55);
	KT_ASSERT_EQ(sec[511], 0xAA);
	KT_ASSERT_EQ(sec[446 + 4], 0x0C);

	/* the mark at the start of the scratch area */
	start = (UD)di.blockcont - 2048;
	KT_ASSERT_ER(tk_srea_dev(dd, (W)start, sec, 512, &asz), E_OK);
	KT_ASSERT_EQ(sec[0], 'T');
	KT_ASSERT_EQ(sec[10], 'U');

	rw_check(dd, (W)start + 16, 300, 0x11);	/* 150KB: three commands */
	rw_check(dd, (W)start + 1, 1, 0x5a);

	/* past the end, and not a whole block */
	KT_ASSERT(tk_srea_dev(dd, di.blockcont, sec, 512, &asz) < E_OK);
	KT_ASSERT(tk_srea_dev(dd, 0, sec, 100, &asz) < E_OK);
	KT_ASSERT_ER(tk_cls_dev(dd, 0), E_OK);

	/* the partition is its first subunit */
	for ( k = 0; nm[k] != 0; k++ ) {
		name[k] = (UB)nm[k];
	}
	name[k] = '0';
	name[k + 1] = 0;
	dd = tk_opn_dev(name, TD_READ);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
		KT_ASSERT_EQ(di.blockcont, 40 * 2048 - 2048 - 2048);
		KT_ASSERT_ER(tk_srea_dev(dd, 0, sec, 512, &asz), E_OK);
		KT_ASSERT_EQ(sec[510], 0x55);		/* the FAT boot sector */
		KT_ASSERT_EQ(sec[82], 'F');
		tk_cls_dev(dd, 0);
	}
}

/* The file system on the USB disk: a file read, one written and read back */
LOCAL void test_msc_fat( void )
{
	CONST char	*nm = disk_of(USB_SPEED_SUPER);
	char		part[12];
	UB		buf[64];
	CONST char	*msg = "written on a USB disk";
	INT		fd, n, k;

	if ( nm == NULL ) KT_SKIP("no SuperSpeed disk");
	for ( k = 0; nm[k] != 0; k++ ) {
		part[k] = nm[k];
	}
	part[k] = '0';
	part[k + 1] = 0;
	KT_ASSERT_ER(fs_attach("fatfs", part, "/usb", 0), EX_OK);

	fd = fs_open("/usb/HELLO.TXT", O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		n = fs_read(fd, buf, sizeof(buf));
		KT_ASSERT(n >= 7);
		KT_ASSERT(buf[0] == 'T' && buf[1] == 'e' && buf[2] == 's');
		fs_close(fd);
	}

	fd = fs_open("/usb/USBTEST.TXT", O_WRONLY | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		for ( n = 0; msg[n] != 0; n++ ) ;
		KT_ASSERT_EQ(fs_write(fd, msg, n), n);
		fs_close(fd);
	}
	KT_ASSERT_ER(fs_sync(), EX_OK);
	fd = fs_open("/usb/USBTEST.TXT", O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		knl_memset(buf, 0, sizeof(buf));
		n = fs_read(fd, buf, sizeof(buf));
		KT_ASSERT_EQ(n, 21);
		KT_ASSERT(buf[0] == 'w' && buf[20] == 'k');
		fs_close(fd);
	}
	KT_ASSERT_ER(fs_detach("/usb"), EX_OK);
}

/* The disk that is high speed, and the one behind the full speed hub */
LOCAL void test_msc_hs_fs( void )
{
	CONST char	*hs = disk_sized(USB_SPEED_HIGH, 4 * 2048);
	CONST char	*fsd;
	DiskInfo	di;
	SZ		asz = 0;
	ID		dd;

	if ( hs == NULL ) KT_SKIP("no high speed disk");
	tm_printf((UB*)"  high speed disk is %s\n", hs);
	dd = tk_opn_dev((UB *)hs, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
		KT_ASSERT_EQ(di.blockcont, 4 * 2048);
		rw_check(dd, 100, 256, 0x33);	/* 128KB: exactly two commands */
		rw_check(dd, 8191, 1, 0x44);	/* the last block */
		tk_cls_dev(dd, 0);
	}

	fsd = disk_of(USB_SPEED_FULL);
	if ( fsd == NULL ) KT_SKIP("no full speed disk");
	tm_printf((UB*)"  full speed disk (behind the hub) is %s\n", fsd);
	dd = tk_opn_dev((UB *)fsd, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
		KT_ASSERT_EQ(di.removable, 1);
		rw_check(dd, 10, 40, 0x77);
		tk_cls_dev(dd, 0);
	}
}
#endif /* RPI5 */

/* ---------------------------------------------------------------- hot-plug */

#ifndef RPI5
/* Wait for the device list to change from gen, up to ms */
LOCAL BOOL wait_gen( UW gen, INT ms )
{
	T_USBSTAT	st;
	INT		i;

	for ( i = 0; i < ms / 50; i++ ) {
		if ( ts_usb_stat(&st) >= E_OK && st.gen != gen ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}

	return FALSE;
}

LOCAL INT count_role( UINT role )
{
	INT	i, n = 0;

	load_tree();
	for ( i = 0; i < ndev; i++ ) {
		if ( devs[i].role == role ) {
			n++;
		}
	}

	return n;
}

/*
 * A device taken out and plugged back in, behind the hub (its status
 * change endpoint reports it) and on a root port (the controller's port
 * change event does): the stack lets go of it and brings it up again.
 */
LOCAL void test_hotplug( void )
{
	T_USBSTAT	st;
	T_HIDSTAT	hs;
	INT		nhid;
	UW		gen;

	if ( ts_usb_hc_count() <= 0 ) KT_SKIP("no controller");
	nhid = count_role(USB_ROLE_HID);
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;

	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"hpm\"}}\n");
	if ( !wait_gen(gen, 5000) ) {
		KT_SKIP("no QEMU monitor to take a device out with");
	}
	tk_dly_tsk(300);
	KT_ASSERT_EQ(count_role(USB_ROLE_HID), nhid - 1);
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	KT_ASSERT(st.detached >= 1);
	gen = st.gen;

	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
		  "{\"driver\":\"usb-mouse\",\"bus\":\"xhci.0\",\"port\":\"4.3\",\"id\":\"hpm\"}}\n");
	KT_ASSERT(wait_gen(gen, 5000));
	tk_dly_tsk(300);
	KT_ASSERT_EQ(count_role(USB_ROLE_HID), nhid);

	/* on a root port of the second controller */
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"hpk\"}}\n");
	KT_ASSERT(wait_gen(gen, 5000));
	tk_dly_tsk(300);
	KT_ASSERT_EQ(count_role(USB_ROLE_HID), nhid - 1);
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
		  "{\"driver\":\"usb-kbd\",\"bus\":\"xhci2.0\",\"port\":\"2\",\"id\":\"hpk\"}}\n");
	KT_ASSERT(wait_gen(gen, 5000));
	tk_dly_tsk(300);
	KT_ASSERT_EQ(count_role(USB_ROLE_HID), nhid);
	KT_ASSERT_ER(ts_hid_stat(&hs), E_OK);
	KT_ASSERT(hs.keyboards >= 2);

	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	tm_printf((UB*)"  attached %d, detached %d, failed %d, scans %d, "
		  "irqs %d, events %d\n", st.attached, st.detached, st.enum_failed,
		  st.scans, st.irqs, st.events);
	KT_ASSERT_EQ(st.enum_failed, 0);
	KT_ASSERT_EQ(st.cmd_timeouts, 0);
}

/*
 * A disk taken out while nothing has it open goes from the device list
 * of the kernel; one plugged back in comes back under a name.
 */
LOCAL void test_hotplug_disk( void )
{
	T_USBSTAT	st;
	T_LDEV		ld[16];
	CONST char	*nm = disk_of(USB_SPEED_FULL);
	UB		name[8];
	INT		k, n, i;
	UW		gen;
	BOOL		there;

	if ( nm == NULL ) KT_SKIP("no disk behind the hub");
	for ( k = 0; nm[k] != 0 && k < 7; k++ ) {
		name[k] = (UB)nm[k];
	}
	name[k] = 0;
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"usbfs\"}}\n");
	if ( !wait_gen(gen, 5000) ) {
		KT_SKIP("no QEMU monitor to take a device out with");
	}
	tk_dly_tsk(500);
	n = tk_lst_dev(ld, 0, 16);
	there = FALSE;
	for ( i = 0; i < n && i < 16; i++ ) {
		for ( k = 0; name[k] != 0 && ld[i].devnm[k] == name[k]; k++ ) ;
		if ( name[k] == 0 && ld[i].devnm[k] == 0 ) {
			there = TRUE;
		}
	}
	KT_ASSERT(!there);
	KT_ASSERT(tk_opn_dev(name, TD_READ) < E_OK);
	KT_ASSERT_EQ(count_role(USB_ROLE_STORAGE), 4);

	/* put back: its blocks were kept apart from the device (-blockdev) */
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
		  "{\"driver\":\"usb-storage\",\"bus\":\"xhci.0\",\"port\":\"4.2\","
		  "\"drive\":\"usbdisk3\",\"removable\":true,\"id\":\"usbfs\"}}\n");
	KT_ASSERT(wait_gen(gen, 5000));
	for ( i = 0; i < 30 && count_role(USB_ROLE_STORAGE) < 5; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(count_role(USB_ROLE_STORAGE), 5);
}

/*
 * The two disks without a partition table: no subunit, and the FAT
 * volume mounted from the whole unit, a file read, one written, read
 * back after the volume was mounted again, and taken away.
 */
LOCAL void test_msc_whole( void )
{
	static CONST UD	blocks[2] = { 69632, 2880 };	/* FAT32 of 34MB, a 1.44MB floppy */
	CONST char	*nm;
	CONST char	*msg = "written on a whole USB disk";
	char		sub[8];
	UB		buf[64];
	INT		i, k, n, fd;

	for ( i = 0; i < 2; i++ ) {
		nm = disk_sized(USB_SPEED_HIGH, blocks[i]);
		if ( nm == NULL ) {
			tm_printf((UB*)"  no disk of %d blocks\n", (INT)blocks[i]);
			KT_ASSERT(FALSE);
			continue;
		}
		tm_printf((UB*)"  disk of %d blocks is %s\n", (INT)blocks[i], nm);
		for ( k = 0; nm[k] != 0; k++ ) sub[k] = nm[k];
		sub[k] = '0';
		sub[k + 1] = 0;
		KT_ASSERT(tk_opn_dev((UB *)sub, TD_READ) < E_OK);

		KT_ASSERT_ER(fs_attach("fatfs", nm, "/usbwhole", 0), EX_OK);
		fd = fs_open("/usbwhole/HELLO.TXT", O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			n = fs_read(fd, buf, sizeof(buf));
			KT_ASSERT(n >= 22);
			KT_ASSERT(buf[0] == 'T' && buf[1] == 'e' && buf[2] == 's');
			fs_close(fd);
		}
		fd = fs_open("/usbwhole/WHOLE.TXT", O_WRONLY | O_CREAT | O_TRUNC);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			for ( n = 0; msg[n] != 0; n++ ) ;
			KT_ASSERT_EQ(fs_write(fd, msg, n), n);
			fs_close(fd);
		}
		KT_ASSERT_ER(fs_detach("/usbwhole"), EX_OK);

		KT_ASSERT_ER(fs_attach("fatfs", nm, "/usbwhole", 0), EX_OK);
		fd = fs_open("/usbwhole/WHOLE.TXT", O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			knl_memset(buf, 0, sizeof(buf));
			n = fs_read(fd, buf, sizeof(buf));
			KT_ASSERT_EQ(n, 27);
			KT_ASSERT(buf[0] == 'w' && buf[26] == 'k');
			fs_close(fd);
		}
		KT_ASSERT_ER(fs_unlink("/usbwhole/WHOLE.TXT"), EX_OK);
		KT_ASSERT_ER(fs_detach("/usbwhole"), EX_OK);
	}
}
#endif /* RPI5 */

EXPORT void ktest_usb( void )
{
	KT_RUN(test_hc);
	KT_RUN_EXCEPT_RPI5(test_ports, "the ports the QEMU machine has in use");
	KT_RUN_EXCEPT_RPI5(test_tree, "the devices the QEMU machine has");
	KT_RUN(test_control);
	KT_RUN(test_hid_desc);
	KT_RUN_EXCEPT_RPI5(test_hid, "a key is pressed through the QEMU monitor");
	KT_RUN_EXCEPT_RPI5(test_msc_ss, "a USB disk there is the user's");
	KT_RUN_EXCEPT_RPI5(test_msc_fat, "a USB disk there is the user's");
	KT_RUN_EXCEPT_RPI5(test_msc_hs_fs, "a USB disk there is the user's");
	KT_RUN_EXCEPT_RPI5(test_msc_whole, "a USB disk there is the user's");
	KT_RUN_EXCEPT_RPI5(test_hotplug, "devices are taken out through the QEMU monitor");
	KT_RUN_EXCEPT_RPI5(test_hotplug_disk, "devices are taken out through the QEMU monitor");
}
