/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usb.h
 *	USB host stack (design 10.4, 10.14)
 *
 *	The stack owns every xHCI it finds and everything plugged into them,
 *	hubs included. A task of its own brings a device up when it is
 *	plugged in, hands it to the class driver that takes it -- keyboards
 *	and pointers, disks, audio -- and lets it go again when it is taken
 *	out. What is here is how the rest of the system looks at that: the
 *	controllers, their ports, the tree of devices, and the control
 *	requests a program may send to a device by itself.
 *
 *	Memory the controller reaches is described by a bus address, which
 *	is not always the physical address: a controller behind a bridge
 *	sees system memory at an offset of the bridge's choosing.
 */

#ifndef __TS_USB_H__
#define __TS_USB_H__

#ifdef __cplusplus
extern "C" {
#endif

/* The speed a device talks at, as the controller numbers them */
#define USB_SPEED_NONE		0
#define USB_SPEED_FULL		1
#define USB_SPEED_LOW		2
#define USB_SPEED_HIGH		3
#define USB_SPEED_SUPER		4
#define USB_SPEED_SUPER_PLUS	5

/* The kinds of endpoint a device can have, besides the control one */
#define USB_EP_ISOC	1
#define USB_EP_BULK	2
#define USB_EP_INTR	3

/* What one root port is doing */
typedef struct {
	UINT	connected;		/* something is plugged in */
	UINT	enabled;		/* and it has been reset and enabled */
	UINT	powered;
	UINT	speed;			/* USB_SPEED_*, only once enabled */
	UINT	changed;		/* something happened since last looked */
	UINT	usb3;			/* the port is a SuperSpeed one */
} T_USBPORT;

/* What a controller is */
typedef struct {
	UH	version;		/* 0x0100 for a 1.0 controller */
	UINT	nports;			/* root ports */
	UINT	nslots;			/* devices it can address at once */
	UINT	ctx_size;		/* 32 or 64 bytes per context */
	UINT	nscratch;		/* pages it asked to borrow */
	UINT	running;
	UINT	intno;			/* what it raises, 0 when polled */
	UD	mmio;			/* where its registers are */
} T_USBHC;

/* What a device was given to do */
#define USB_ROLE_NONE		0	/* nobody took it */
#define USB_ROLE_HUB		1
#define USB_ROLE_HID		2	/* keyboard or pointer */
#define USB_ROLE_STORAGE	3
#define USB_ROLE_AUDIO		4

/* One device in the tree */
typedef struct {
	INT	dev;			/* what the calls below take */
	INT	parent;			/* the hub it hangs off, 0 on a root port */
	UINT	hc;			/* the controller */
	UINT	root_port;		/* the root port, from 0 */
	UINT	port;			/* the port of its hub, from 1 (root: as root_port) */
	UINT	depth;			/* hubs above it */
	UW	route;			/* hub ports on the way, four bits a tier */
	UINT	speed;			/* USB_SPEED_* */
	UINT	addr;			/* its address on the bus */
	UH	vendor;
	UH	product;
	UH	release;
	UB	dev_class;		/* the device's class, or its first interface's */
	UB	subclass;
	UB	protocol;
	UINT	role;			/* USB_ROLE_* */
	UINT	hub_ports;		/* for a hub */
	UB	name[32];		/* the product string, in ASCII */
	UB	devnm[8];		/* a disk: the device it is registered as */
} T_USBDEV;

/* What the stack has done, for a test or a measurement */
typedef struct {
	UW	gen;			/* goes up whenever a device comes or goes */
	UW	attached;		/* devices brought up */
	UW	detached;		/* and let go */
	UW	enum_failed;		/* devices that would not come up */
	UW	irqs;			/* interrupts taken */
	UW	events;			/* controller events read */
	UW	iso_packets;		/* isochronous packets finished */
	UW	iso_missed;		/* that the controller did not serve in time */
	UW	iso_errors;
	UW	out_underruns;		/* OUT rings that ran dry */
	UW	stream_errors;		/* bulk and interrupt streams that failed */
	UW	cmd_timeouts;		/* controller commands that never answered */
	UW	scans;			/* port scans done */
} T_USBSTAT;

/*
 * Find the controllers, bring them up, start the task that serves them,
 * and wait until what is plugged in now has been brought up. Answers how
 * many controllers were started.
 */
IMPORT INT knl_usb_init( void );

/* The controllers and their root ports */
IMPORT INT ts_usb_hc_count( void );
IMPORT ER  ts_usb_ref_hc( INT hc, T_USBHC *pk_hc );
IMPORT ER  ts_usb_ref_port( INT hc, UINT port, T_USBPORT *p );

/*
 * The devices plugged in now, parents before their children. Answers how
 * many there are; at most max are written.
 */
IMPORT INT ts_usb_lst_dev( T_USBDEV *buf, INT max );
IMPORT ER  ts_usb_ref_dev( INT dev, T_USBDEV *pk_dev );

/*
 * One request on a device's control endpoint. Answers how many bytes
 * moved. E_OBJ when the device refused it (a stall), E_NOEXS when the
 * device is gone.
 */
IMPORT INT ts_usb_control( INT dev, CONST UB *setup, void *data, INT len );

/* The descriptors a device has, read over the same endpoint */
IMPORT INT ts_usb_get_desc( INT dev, UINT type, UINT index, void *buf, INT len );

IMPORT ER  ts_usb_stat( T_USBSTAT *st );

/* Look at every port again now */
IMPORT ER  ts_usb_rescan( void );

/* ---------------------------------------------------------------- memory */

/*
 * One page the controller can reach. The bus address it must be given is
 * left in *p_bus; the address the kernel uses is what comes back. On a
 * board whose bus masters do not see the processor's caches the page is
 * mapped uncached.
 */
IMPORT void *knl_usb_page_alloc( UD *p_bus );
IMPORT void  knl_usb_page_free( void *page );

#ifdef __cplusplus
}
#endif

#endif /* __TS_USB_H__ */
