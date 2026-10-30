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
#include <ts/proc.h>
#if defined(_QEMU_VIRT_) || defined(X64)
#include "../../device/usb/usbdev.h"
#include "../../device/usb/vhc.h"
#define KT_VHC		1		/* the USB controller in software is built */
#else
#define KT_VHC		0
#endif

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

#if KT_QEMU_DEVS
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
#endif /* KT_BOARD */

/* ---------------------------------------------------------------- controllers */

LOCAL void test_hc( void )
{
	T_USBHC	hc;
	INT	i, n = ts_usb_hc_count();

	if ( n <= 0 ) {
		KT_SKIP("the machine has no USB controller");
	}
#if defined(X64)
	KT_ASSERT(n >= 1);		/* the PC's own; the RP1 and QEMU virt's test machine have two */
#else
	KT_ASSERT(n >= 2);
#endif
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(ts_usb_ref_hc(i, &hc), E_OK);
		KT_ASSERT_EQ(hc.running, 1);
		KT_ASSERT_EQ(hc.version >> 8, 1);	/* version 1.x */
		KT_ASSERT(hc.nports > 0);
		KT_ASSERT(hc.nslots > 0);
		KT_ASSERT(hc.ctx_size == 32 || hc.ctx_size == 64);
		/* a message interrupt each, edge triggered (design 10.13); the
		   RP1's controllers are polled */
#if !KT_BOARD
		KT_ASSERT(hc.intno != 0);
#endif
		tm_printf((UB*)"  xhci%d %d.%d, %d ports, %d slots, interrupt %d\n", i,
			  (INT)(hc.version >> 8), (INT)((hc.version >> 4) & 0xf),
			  (INT)hc.nports, (INT)hc.nslots, (INT)hc.intno);
	}
	KT_ASSERT_ER(ts_usb_ref_hc(n, &hc), E_NOEXS);
	KT_ASSERT_ER(ts_usb_ref_hc(0, NULL), E_PAR);
}

#if KT_QEMU_DEVS
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
#endif /* KT_BOARD */

/* ---------------------------------------------------------------- the tree */

#if KT_QEMU_DEVS
LOCAL CONST char *role_name( UINT r )
{
	switch ( r ) {
	case USB_ROLE_HUB:	return "hub";
	case USB_ROLE_HID:	return "hid";
	case USB_ROLE_STORAGE:	return "storage";
	case USB_ROLE_AUDIO:	return "audio";
	case USB_ROLE_CAMERA:	return "camera";
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
#endif /* KT_BOARD */

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

#if KT_QEMU_DEVS
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
#endif /* KT_BOARD */

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

/*
 * A mouse of the boot subclass without a report ID whose descriptor says
 * an 8-byte report: eight buttons, 16-bit X and Y (-32767..32767), a
 * wheel, AC Pan and a byte of padding
 */
LOCAL CONST UB rd_pad16[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
	0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
	0xc0, 0xc0
};

/*
 * What such a mouse sent while still in the boot protocol: the boot
 * protocol's three bytes padded with zeros to eight, moving right and
 * up. Read as the descriptor says, the first is X 0xff02.
 */
#define PAD_N	11
LOCAL CONST UB pad_boot[PAD_N][8] = {
	{ 0x00, 0x02, 0xff }, { 0x00, 0x04, 0xff }, { 0x00, 0x04, 0xfd }, { 0x00, 0x05, 0xfd },
	{ 0x00, 0x06, 0xfd }, { 0x00, 0x06, 0xfc }, { 0x00, 0x06, 0xfa }, { 0x00, 0x07, 0xfb },
	{ 0x00, 0x07, 0xfa }, { 0x00, 0x08, 0xfa }, { 0x00, 0x09, 0xf9 }
};

/*
 * The padded boot reports told from the report protocol's: three moves
 * no byte holds with nothing after the third byte decide it, their
 * moves held back until then; a byte set after the fourth says the
 * report protocol, and a fast move with the descriptor's Y in it starts
 * the count again.
 */
LOCAL void test_hid_padded_desc( void )
{
	INT	info[PI_N], out[4 * 16];
	INT	lens[16];
	INT	i;

	for ( i = 0; i < 16; i++ ) lens[i] = 8;

	/* moving right and up: held twice, then read as the boot protocol from the third */
	KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), TRUE, info, &pad_boot[0][0], lens, PAD_N, out));
	KT_ASSERT_EQ(info[PI_RID], 0);
	KT_ASSERT_EQ(info[PI_PLEN], 8);
	KT_ASSERT_EQ(info[PI_XSIZE], 8);	/* the boot layout now */
	KT_ASSERT_EQ(info[PI_BOOT], 1);
	KT_ASSERT_EQ(out[1], 0);
	KT_ASSERT_EQ(out[2], 0);
	KT_ASSERT_EQ(out[5], 0);
	KT_ASSERT_EQ(out[6], 0);
	KT_ASSERT_EQ(out[9], 4);
	KT_ASSERT_EQ(out[10], -3);
	KT_ASSERT_EQ(out[4 * 10 + 1], 9);
	KT_ASSERT_EQ(out[4 * 10 + 2], -7);

	/* slowly to the right: the moves a byte holds go between the votes */
	{
		static CONST UB r[8][8] = {
			{ 0x00, 0x01, 0x00 }, { 0x00, 0x00, 0xff }, { 0x00, 0x01, 0x00 }, { 0x00, 0x01, 0x00 },
			{ 0x00, 0x01, 0xff }, { 0x00, 0x01, 0xff }, { 0x00, 0x01, 0x00 }, { 0x00, 0x00, 0xff }
		};

		KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), TRUE, info, &r[0][0], lens, 8, out));
		KT_ASSERT_EQ(info[PI_BOOT], 1);
		KT_ASSERT_EQ(out[1], 1);
		KT_ASSERT_EQ(out[2], 0);
		KT_ASSERT_EQ(out[5], 0);		/* a vote, held */
		KT_ASSERT_EQ(out[6], 0);
		KT_ASSERT_EQ(out[4 * 5 + 1], 1);	/* the third vote, read as the boot protocol */
		KT_ASSERT_EQ(out[4 * 5 + 2], -1);
		KT_ASSERT_EQ(out[4 * 7 + 1], 0);
		KT_ASSERT_EQ(out[4 * 7 + 2], -1);
	}

	/* the report protocol: up a little, then fast to the right with Y 0 */
	{
		static CONST UB r[3][8] = {
			{ 0x00, 0x05, 0x00, 0xfd, 0xff }, { 0x00, 0x2c, 0x01 }, { 0x00, 0x2c, 0x01 }
		};
		static CONST UB r4[3][8] = {
			{ 0x00, 0x2c, 0x01 }, { 0x00, 0x2c, 0x01 }, { 0x00, 0x2c, 0x01 }
		};

		KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), TRUE, info, &r[0][0], lens, 3, out));
		KT_ASSERT_EQ(info[PI_BOOT], 0);
		KT_ASSERT_EQ(info[PI_XSIZE], 16);
		KT_ASSERT_EQ(out[1], 5);
		KT_ASSERT_EQ(out[2], -3);
		KT_ASSERT_EQ(out[5], 300);
		KT_ASSERT_EQ(out[9], 300);
		/* the same fast moves first, and three of them: the boot protocol's they could be */
		KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), TRUE, info, &r4[0][0], lens, 3, out));
		KT_ASSERT_EQ(info[PI_BOOT], 1);
	}

	/* fast moves with the descriptor's Y in them between: the count starts again */
	{
		static CONST UB r[6][8] = {
			{ 0x00, 0x2c, 0x01 }, { 0x00, 0x2c, 0x01, 0x05 }, { 0x00, 0x2c, 0x01 },
			{ 0x00, 0x2c, 0x01 }, { 0x00, 0x2c, 0x01, 0x03 }, { 0x00, 0x2c, 0x01 }
		};

		KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), TRUE, info, &r[0][0], lens, 6, out));
		KT_ASSERT_EQ(info[PI_BOOT], 0);
		KT_ASSERT_EQ(out[1], 0);		/* held while it could be either */
		KT_ASSERT_EQ(out[5], 300);
		KT_ASSERT_EQ(out[6], 5);
		KT_ASSERT_EQ(out[4 * 4 + 1], 300);
		KT_ASSERT_EQ(out[4 * 5 + 1], 0);
	}

	/* not of the boot subclass: read as the descriptor says, whatever it sends */
	KT_ASSERT(knl_hid_probe(rd_pad16, sizeof(rd_pad16), FALSE, info, &pad_boot[0][0], lens, PAD_N, out));
	KT_ASSERT_EQ(info[PI_BOOT], 0);
	KT_ASSERT_EQ(out[1], -254);
}

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

	test_hid_padded_desc();
}

/* ---------------------------------------------------------------- disks */

#if KT_QEMU_DEVS
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

IMPORT INT knl_msc_fault( CONST char *devnm, INT n, BOOL bot );	/* msc.c, for the tests */

/*
 * A disk whose commands time out: each is sent again after the device
 * was brought back (the port reset, or the Bulk-Only reset when the
 * port reset cannot be made), and the reads and writes come out as
 * they would have; the endpoint the timed out transfer was on is used
 * again after it. The time outs are made on the controller (a status
 * read with no command), so the controller's side of the recovery
 * (Stop Endpoint, Set TR Dequeue Pointer, the endpoint started over
 * after the halt is cleared) runs as it does for a real one.
 */
LOCAL void test_msc_recover( void )
{
	CONST char	*nm = disk_of(USB_SPEED_SUPER);
	DiskInfo	di;
	UB		sec[512], ref[512];
	SZ		asz = 0;
	ID		dd;
	INT		r0, r1;
	BOOL		bot;

	if ( nm == NULL ) KT_SKIP("no SuperSpeed disk");
	dd = tk_opn_dev((UB *)nm, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
	KT_ASSERT_ER(tk_srea_dev(dd, 0, ref, 512, &asz), E_OK);
	for ( bot = FALSE; bot <= TRUE; bot++ ) {
		r0 = knl_msc_fault(nm, 2, bot);
		KT_ASSERT(r0 >= 0);
		knl_memset(sec, 0, sizeof(sec));
		KT_ASSERT_ER(tk_srea_dev(dd, 0, sec, 512, &asz), E_OK);
		for ( r1 = 0; r1 < 512 && sec[r1] == ref[r1]; r1++ ) ;
		KT_ASSERT_EQ(r1, 512);
		r1 = knl_msc_fault(nm, 0, FALSE);
		tm_printf((UB *)"  %s: %d recoveries by the %s\n", nm, r1 - r0, bot ? "Bulk-Only reset" : "port reset");
		KT_ASSERT_EQ(r1 - r0, 2);
		/* a round trip after it, through both endpoints */
		rw_check(dd, (W)di.blockcont - 2048 + 40, 130, bot ? 0x6b : 0x6a);
	}
	KT_ASSERT_ER(tk_cls_dev(dd, 0), E_OK);
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
#endif /* KT_BOARD */

/* ---------------------------------------------------------------- hot-plug */

#if KT_QEMU_DEVS
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
#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_USBDEV	37		/* as tests/uprog/svcprog.c */

/*
 * A device of a program's own (SVC class 12): an FTDI serial converter
 * (QEMU's usb-serial, which no class driver takes) is plugged into the
 * hub's fourth port, its line reading /dev/zero of the host (a line
 * with no end: QEMU takes the device out when its line ends); a process
 * lists it, opens it, makes requests on its control endpoint, writes
 * and reads its bulk endpoints (svcprog's SP_USBDEV). Then it is taken
 * out.
 */
LOCAL void test_usb_prog( void )
{
	T_USBSTAT	st;
	T_CPRC		cprc;
	T_PSTS		psts;
	UW		arg[8], gen;
	ID		pid;

	if ( ts_usb_hc_count() <= 0 ) KT_SKIP("no controller");
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"chardev-add\",\"arguments\":{\"id\":\"usr1\","
		  "\"backend\":{\"type\":\"file\",\"data\":{\"in\":\"/dev/zero\",\"out\":\"/dev/null\"}}}}\n");
	tk_dly_tsk(300);
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":"
		  "{\"driver\":\"usb-serial\",\"bus\":\"xhci.0\",\"port\":\"4.4\",\"chardev\":\"usr1\",\"id\":\"usr\"}}\n");
	if ( !wait_gen(gen, 5000) ) {
		KT_SKIP("no QEMU monitor to plug a device in with");
	}
	tk_dly_tsk(500);

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_USBDEV;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 30000), E_OK);
		KT_ASSERT_EQ(psts.exitcd, 0);
	}

	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"usr\"}}\n");
	KT_ASSERT(wait_gen(gen, 5000));
	tk_dly_tsk(300);
	tm_printf((UB*)"KTEST QMP {\"execute\":\"chardev-remove\",\"arguments\":{\"id\":\"usr1\"}}\n");
}

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
#endif /* KT_BOARD */

#if KT_VHC
/* ---------------------------------------------------------------- a mouse left in the boot protocol */

/*
 * A mouse and a keyboard on one device, plugged into the USB controller
 * in software (device/usb/vhc.c): interface 0 a mouse of the boot
 * subclass whose descriptor is rd_pad16, interface 1 a boot keyboard.
 * How it takes SET_PROTOCOL and GET_PROTOCOL is the test's to choose:
 *   PM_GOOD	each interface as it is told, the report protocol at first
 *   PM_STUCK	the boot protocol whatever it is told, and says so
 *   PM_SHARED	one protocol for both interfaces: putting the keyboard into
 *		the boot protocol puts the mouse there too
 *   PM_SILENT	stalls both requests
 * Whichever it is, it sends what it is in: the boot protocol's reports
 * padded to 8 bytes, or the descriptor's.
 */
enum { PM_GOOD, PM_STUCK, PM_SHARED, PM_SILENT };

LOCAL CONST UB pm_ddesc[18] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 8, 0x34, 0x12, 0x01, 0x9a, 0x00, 0x01, 1, 2, 0, 1
};
LOCAL CONST UB pm_kbd_report[63] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
	0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0
};
LOCAL CONST UB pm_cfg[59] = {
	9, 2, 59, 0, 2, 1, 0, 0xa0, 50,
	9, 4, 0, 0, 1, 3, 1, 2, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(rd_pad16), 0,
	7, 5, 0x81, 3, 8, 0, 1,
	9, 4, 1, 0, 1, 3, 1, 1, 0,
	9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(pm_kbd_report), 0,
	7, 5, 0x82, 3, 8, 0, 10
};

LOCAL INT	pm_mode;
LOCAL UB	pm_proto[2];		/* the protocol each interface is in */
LOCAL INT	pm_sets[2];		/* SET_PROTOCOL to each */

LOCAL INT pm_klass( CONST UB *setup, UB *data, INT len )
{
	INT	ifno = setup[4] & 1;

	switch ( setup[1] ) {
	case 0x0a:				/* SET_IDLE */
	case 0x09:				/* SET_REPORT: the keyboard's lamps */
		return 0;
	case 0x0b:				/* SET_PROTOCOL */
		if ( pm_mode == PM_SILENT ) return -1;
		pm_sets[ifno]++;
		if ( pm_mode == PM_STUCK ) return 0;
		if ( pm_mode == PM_SHARED ) {
			pm_proto[0] = pm_proto[1] = setup[2];
		} else {
			pm_proto[ifno] = setup[2];
		}
		return 0;
	case 0x03:				/* GET_PROTOCOL */
		if ( pm_mode == PM_SILENT || len < 1 ) return -1;
		data[0] = pm_proto[ifno];
		return 1;
	}
	return -1;
}

LOCAL CONST T_VHCDEV pm_dev = {
	pm_ddesc, pm_cfg, sizeof(pm_cfg), { "TessronOS test", "Padded Boot Mouse", NULL, NULL },
	{ rd_pad16, pm_kbd_report, NULL, NULL }, { sizeof(rd_pad16), sizeof(pm_kbd_report), 0, 0 },
	NULL, pm_klass
};

/* The id of a pointer that came after the ids up to last, 0 none yet */
LOCAL UINT pm_pointer( UINT last, UD *p_reports )
{
	T_HIDINFO	h[16];
	INT		n, i;

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( !h[i].keyboard && h[i].id > last ) {
			if ( p_reports != NULL ) *p_reports = h[i].reports;
			return h[i].id;
		}
	}
	return 0;
}

LOCAL UINT pm_last_id( void )
{
	T_HIDINFO	h[16];
	INT		n, i;
	UINT		last = 0;

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].id > last ) last = h[i].id;
	}
	return last;
}

/*
 * Plug the mouse in, in mode, put the pointer at 200,200, send the
 * reports and see where they took it: no step larger than step, and
 * the pointer at ex,ey in the end. Answers the SET_PROTOCOL its mouse
 * interface was sent.
 */
LOCAL INT pm_run( INT mode, CONST UB *reps, INT nrep, INT step, INT ex, INT ey )
{
	T_HIDEV	ev, evs[64];
	UINT	id = 0, last;
	UD	pos, lost = 0, nr = 0, nr0 = 0;
	INT	i, n, px = 200, py = 200, x, y;
	UINT	b;

	pm_mode = mode;
	pm_proto[0] = pm_proto[1] = ( mode == PM_STUCK ) ? 0 : 1;
	pm_sets[0] = pm_sets[1] = 0;
	last = pm_last_id();
	KT_ASSERT_ER(knl_vhc_plug(1, &pm_dev), E_OK);
	for ( i = 0; i < 60 && ( id = pm_pointer(last, &nr0) ) == 0; i++ ) {
		tk_dly_tsk(50);
	}
	KT_ASSERT(id != 0);
	if ( id == 0 ) {
		(void)knl_vhc_unplug(1);
		return -1;
	}
	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_MOVE;
	ev.x = px;
	ev.y = py;
	KT_ASSERT_ER(knl_hid_inject(0, &ev), E_OK);
	pos = knl_hid_tap_now();
	for ( i = 0; i < nrep; i++ ) {
		KT_ASSERT_ER(knl_vhc_in(1, 0x81, reps + 8 * i, 8), E_OK);
	}
	for ( i = 0; i < 60; i++ ) {
		(void)pm_pointer(last, &nr);
		if ( nr >= nr0 + (UD)nrep ) break;
		tk_dly_tsk(50);
	}
	KT_ASSERT(nr >= nr0 + (UD)nrep);
	n = knl_hid_tap(&pos, id, evs, 64, &lost);
	KT_ASSERT(n > 0);
	KT_ASSERT_EQ((INT)lost, 0);
	for ( i = 0; i < n; i++ ) {
		if ( evs[i].type != HID_EV_MOVE ) continue;
		KT_ASSERT(evs[i].x - px <= step && px - evs[i].x <= step);
		KT_ASSERT(evs[i].y - py <= step && py - evs[i].y <= step);
		px = evs[i].x;
		py = evs[i].y;
	}
	KT_ASSERT_ER(ts_hid_pointer(&x, &y, &b), E_OK);
	tm_printf((UB*)"  mode %d: %d events, the pointer at %d,%d (%d,%d wanted), SET_PROTOCOL %d to the mouse\n",
		  mode, n, x, y, ex, ey, pm_sets[0]);
	KT_ASSERT_EQ(px, ex);
	KT_ASSERT_EQ(py, ey);
	KT_ASSERT_EQ(x, ex);
	KT_ASSERT_EQ(y, ey);
	KT_ASSERT_ER(knl_vhc_unplug(1), E_OK);
	for ( i = 0; i < 60 && pm_pointer(last, NULL) != 0; i++ ) {
		tk_dly_tsk(50);
	}
	KT_ASSERT(pm_pointer(last, NULL) == 0);
	return pm_sets[0];
}

/*
 * The mouse left in the boot protocol, by each of the ways it can be:
 * the pointer goes where the boot protocol's reports take it, without a
 * jump; the mouse that keeps to what it is told is read as its
 * descriptor says, and is not told what it already says it talks.
 */
LOCAL void test_hid_padded( void )
{
	T_HIDATTR	a, keep;
	static CONST UB good[3][8] = {
		{ 0x00, 0x05, 0x00, 0xfd, 0xff }, { 0x00, 0x96, 0x00 }, { 0x00, 0x74, 0xff, 0x02 }
	};

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.speed = 12;			/* as the device moved */
	a.accel = 7;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);

	/* the boot protocol from the first report: 64 right and 45 up in all */
	KT_ASSERT_EQ(pm_run(PM_STUCK, &pad_boot[0][0], PAD_N, 9, 264, 155), 1);
	KT_ASSERT_EQ(pm_run(PM_SHARED, &pad_boot[0][0], PAD_N, 9, 264, 155), 0);
	/* nothing said: the first two held back, then the boot protocol */
	KT_ASSERT_EQ(pm_run(PM_SILENT, &pad_boot[0][0], PAD_N, 9, 258, 157), 0);
	/* the report protocol it says and sends: 5 - 3, 150 right, 140 left and 2 down */
	KT_ASSERT_EQ(pm_run(PM_GOOD, &good[0][0], 3, 150, 215, 199), 0);
	KT_ASSERT_EQ(pm_sets[1], 1);	/* the keyboard put into the boot protocol */

	KT_ASSERT_ER(ts_hid_setattr(&keep), E_OK);
}
#endif /* KT_VHC */

EXPORT void ktest_usb( void )
{
	KT_RUN(test_hc);
	KT_RUN_QEMU_DEVS(test_ports, "the ports the QEMU machine has in use");
	KT_RUN_QEMU_DEVS(test_tree, "the devices the QEMU machine has");
	KT_RUN(test_control);
	KT_RUN(test_hid_desc);
#if KT_VHC
	KT_RUN(test_hid_padded);
#endif
	KT_RUN_QEMU_DEVS(test_hid,"a key is pressed through the QEMU monitor");
	KT_RUN_QEMU_DEVS(test_msc_ss, "a USB disk there is the user's");
	KT_RUN_QEMU_DEVS(test_msc_recover, "a USB disk there is the user's");
	KT_RUN_QEMU_DEVS(test_msc_fat, "a USB disk there is the user's");
	KT_RUN_QEMU_DEVS(test_msc_hs_fs, "a USB disk there is the user's");
	KT_RUN_QEMU_DEVS(test_msc_whole, "a USB disk there is the user's");
	KT_RUN_QEMU_DEVS(test_hotplug, "devices are taken out through the QEMU monitor");
	KT_RUN_QEMU_DEVS(test_hotplug_disk, "devices are taken out through the QEMU monitor");
	KT_RUN_QEMU_DEVS(test_usb_prog, "a device is plugged in through the QEMU monitor");
}
