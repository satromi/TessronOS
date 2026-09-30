/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usbdev.h
 *	What the parts of the USB host stack share (design 10.14).
 *
 *	Three layers, and the ones above never touch the registers of the
 *	ones below:
 *
 *	  class drivers	hub.c, hid.c, msc.c, uac.c
 *	  manager	usbman.c: the controllers, the task that serves them,
 *			the tree of devices, what is plugged in and taken out;
 *			usbcore.c: enumeration and the standard requests
 *	  controller	xhci.c, behind the operations of USBHC_OPS
 *
 *	One lock, the manager's, is held by whatever talks to a device or
 *	changes what is attached: the manager's task while it serves the
 *	controllers and scans the ports, and a request that sends a command
 *	to a device (a disk read, a volume change). Inside the controller
 *	driver three semaphores of its own keep its commands, its transfers
 *	and its event ring apart.
 */

#ifndef __USBDEV_H__
#define __USBDEV_H__

#include <ts/usb.h>

/* ---------------------------------------------------------------- limits */

#define USB_MAX_HC		4	/* controllers served */
#define USB_MAX_DEV		32	/* devices in the tree, hubs included */
#define USB_MAX_DEPTH		5	/* hubs between a device and its root port */
#define USB_CFG_MAX		4096	/* the longest configuration taken */

/* ---------------------------------------------------------------- memory */

#define USB_PAGE		4096

/*
 * A buffer of up to DMABUF_PAGES pages that the controller reaches page by
 * page. The pages need not follow each other in memory: a transfer is one
 * ring entry per page.
 */
#define DMABUF_PAGES		16

typedef struct {
	INT	npages;
	UB	*virt[DMABUF_PAGES];
	UD	bus[DMABUF_PAGES];
} DMABUF;

IMPORT ER   knl_dmabuf_alloc( DMABUF *b, INT bytes );
IMPORT void knl_dmabuf_free( DMABUF *b );
IMPORT void knl_dmabuf_read( DMABUF *b, INT off, UB *dst, INT len );
IMPORT void knl_dmabuf_write( DMABUF *b, INT off, CONST UB *src, INT len );

/* The barrier between writing memory the controller reads and telling it */
#define USB_MB()	Asm("dsb sy" ::: "memory")

/* ---------------------------------------------------------------- descriptors */

#define USB_DT_DEVICE		1
#define USB_DT_CONFIG		2
#define USB_DT_STRING		3
#define USB_DT_INTERFACE	4
#define USB_DT_ENDPOINT		5
#define USB_DT_HID		0x21
#define USB_DT_HID_REPORT	0x22
#define USB_DT_HUB		0x29
#define USB_DT_SS_HUB		0x2a
#define USB_DT_CS_INTERFACE	0x24
#define USB_DT_CS_ENDPOINT	0x25
#define USB_DT_SS_EP_COMP	0x30

#define USB_REQ_GET_STATUS	0
#define USB_REQ_CLEAR_FEATURE	1
#define USB_REQ_SET_FEATURE	3
#define USB_REQ_SET_ADDRESS	5
#define USB_REQ_GET_DESCRIPTOR	6
#define USB_REQ_SET_CONFIG	9
#define USB_REQ_SET_INTERFACE	11

#define USB_RT_IN		0x80
#define USB_RT_CLASS		0x20
#define USB_RT_INTERFACE	0x01
#define USB_RT_ENDPOINT		0x02
#define USB_RT_OTHER		0x03

#define USB_EP_CONTROL		0

#define USB_CLASS_AUDIO		0x01
#define USB_CLASS_HID		0x03
#define USB_CLASS_MSC		0x08
#define USB_CLASS_HUB		0x09

/* little endian fields of a descriptor */
#define GET16(p)	((UH)((p)[0] | ((p)[1] << 8)))
#define GET24(p)	((UW)(p)[0] | ((UW)(p)[1] << 8) | ((UW)(p)[2] << 16))
#define GET32(p)	((UW)(p)[0] | ((UW)(p)[1] << 8) | ((UW)(p)[2] << 16) \
			 | ((UW)(p)[3] << 24))
#define PUT16(p, v)	((p)[0] = (UB)(v), (p)[1] = (UB)((v) >> 8))
#define PUT32(p, v)	((p)[0] = (UB)(v), (p)[1] = (UB)((v) >> 8), \
			 (p)[2] = (UB)((v) >> 16), (p)[3] = (UB)((v) >> 24))

/* ---------------------------------------------------------------- devices */

typedef struct usb_hc	USBHC;
typedef struct usb_dev	USBDEV;
typedef struct usb_pipe	USBPIPE;

struct usb_dev {
	USBHC	*hc;
	USBDEV	*parent;	/* the hub it hangs off, NULL on a root port */
	INT	port;		/* root port (from 0) or hub port (from 1) */
	INT	root_port;	/* the root port at the top of its path */
	UW	route;		/* hub ports on the way down, four bits a tier */
	INT	depth;		/* hubs between it and the root port */
	INT	speed;		/* USB_SPEED_* */
	INT	addr;		/* its address, 0 before it has one */
	INT	mps0;		/* the largest packet of its control endpoint */
	UB	ddesc[18];	/* its device descriptor */
	UB	*cfg;		/* its whole configuration descriptor */
	INT	cfglen;
	UB	cfg_value;
	UB	product[32];
	UB	maker[32];
	INT	role;		/* USB_ROLE_* */
	INT	handle;		/* what the interface calls it */
	UB	devnm[8];	/* a disk: its device name */

	/* a hub */
	INT	hub_ports;
	UW	hub_ignored;	/* bit n: port n holds a device given up on */
	USBPIPE	*hub_pipe;	/* its status change endpoint */
	volatile UW hub_change;	/* ports that reported a change */

	void	*hcpriv;	/* the controller driver's */
	void	*hid;		/* the class drivers' */
	void	*msc;
	void	*audio;
};

/* An endpoint as its descriptors describe it */
typedef struct {
	UB	addr;		/* bEndpointAddress */
	UB	type;		/* USB_EP_* */
	INT	size;		/* the largest packet */
	INT	mult;		/* transactions per microframe, high speed */
	INT	interval;	/* bInterval */
	INT	burst;		/* SuperSpeed bMaxBurst */
	INT	ss_mult;	/* SuperSpeed isochronous Mult */
	INT	esit;		/* bytes per service interval */
} USBEP;

/*
 * One finished transfer of a stream: len bytes at offset off of b. err is
 * not 0 for one that failed or that the controller missed. Called with the
 * manager's lock held; it must not wait.
 */
typedef void (*USB_XFERFN)( void *ctx, DMABUF *b, INT off, INT len, INT err );

/*
 * An isochronous OUT stream asks for each packet just before it goes on
 * the ring: up to max bytes into dst, and how many were put there. 0
 * leaves the packet idle, to be asked for again at the next service.
 */
typedef INT (*USB_FILLFN)( void *ctx, UB *dst, INT max );

/* A bulk stream moves this much at most per transfer */
#define USB_STREAM_XFER_MAX	16384

/*
 * What a controller driver does for the layers above. Ports count from 0.
 * All of it is called with the manager's lock held.
 *
 *   nports		its root ports
 *   port_connected	something is plugged in
 *   port_changed	the connection changed since the last look (clears it)
 *   port_reset		reset the port and enable it; *speed is USB_SPEED_*
 *   port_state		the raw state, for the interface
 *   dev_init		the device about to be enumerated gets its address
 *   dev_mps0		the control packet size the device named
 *   hub_config		the device is a hub
 *   dev_free		the device is gone, with every endpoint it had
 *   control, bulk	one transfer, waiting up to tmo milliseconds
 *   stream_open	a stream of IN transfers delivered through fn
 *   stream_open_out	an isochronous OUT stream fed through fill
 *   stream_close
 *   poll		take what the controller finished, look at the ports
 *   info		what it is
 */
typedef struct {
	INT	(*nports)( USBHC *hc );
	BOOL	(*port_connected)( USBHC *hc, INT port );
	BOOL	(*port_changed)( USBHC *hc, INT port );
	ER	(*port_reset)( USBHC *hc, INT port, INT *speed );
	ER	(*port_state)( USBHC *hc, INT port, T_USBPORT *p );
	ER	(*dev_init)( USBHC *hc, USBDEV *dev );
	ER	(*dev_mps0)( USBHC *hc, USBDEV *dev );
	ER	(*hub_config)( USBHC *hc, USBDEV *hub );
	void	(*dev_free)( USBHC *hc, USBDEV *dev );
	ER	(*control)( USBHC *hc, USBDEV *dev, CONST UB *setup, UB *data,
			    INT *actual, TMO tmo );
	ER	(*bulk)( USBHC *hc, USBDEV *dev, CONST USBEP *ep, UB *data,
			 INT len, INT *actual, TMO tmo );
	ER	(*stream_open)( USBHC *hc, USBDEV *dev, CONST USBEP *ep,
				INT xfer, USB_XFERFN fn, void *ctx,
				USBPIPE **out );
	ER	(*stream_open_out)( USBHC *hc, USBDEV *dev, CONST USBEP *ep,
				    USB_FILLFN fill, void *ctx, USBPIPE **out );
	void	(*stream_close)( USBHC *hc, USBPIPE *pipe );
	void	(*poll)( USBHC *hc );
	void	(*info)( USBHC *hc, T_USBHC *info );
} USBHC_OPS;

struct usb_hc {
	CONST USBHC_OPS *ops;
	INT	index;		/* its place among the controllers */
	UINT	intno;		/* the interrupt it raises, 0 when polled */
	ID	flgid;		/* the manager's flag, raised on its events */
	BOOL	lost;		/* it stopped on an error of its own */
};

/* the bits of the manager's flag */
#define USBF_IRQ	0x0001	/* a controller raised an interrupt */
#define USBF_RESCAN	0x0002	/* the ports are to be looked at */
#define USBF_HUB	0x0004	/* a hub reported a change */

/* ---------------------------------------------------------------- counters */

IMPORT T_USBSTAT	knl_usb_stat;

/* ---------------------------------------------------------------- xhci.c */

/*
 * Bring up the xHCI whose registers start at regs (size bytes of them are
 * mapped). intno is its interrupt, 0 to poll it. The flag is the
 * manager's. mmio is the address the registers were found at, for the
 * record.
 */
IMPORT ER knl_xhci_attach( UBINT regs, UD size, UD mmio, UINT intno,
			   ID flgid, USBHC **out );

/* The interrupt of a controller that has one, from the board's code */
IMPORT void knl_xhci_inthdr( UINT intno );

/* ---------------------------------------------------------------- usbman.c */

IMPORT void knl_usb_lock( void );
IMPORT void knl_usb_unlock( void );

/*
 * A wait in the middle of a port scan. The manager's task holds the lock
 * for the whole scan; this lets it go in short steps and serves the
 * controllers between them, so the streams already running keep going.
 * Anywhere else it is a plain delay.
 */
IMPORT void knl_usb_wait( INT ms );

/* Milliseconds since the machine started */
IMPORT UW   knl_usb_ms( void );

/* The manager's task is to look at the hubs' ports */
IMPORT void knl_usb_hub_changed( void );

/* The manager's task is to call knl_snd_poll() often */
IMPORT BOOL knl_usb_streaming( void );

/* ---------------------------------------------------------------- usbcore.c */

IMPORT ER   knl_usb_control( USBDEV *dev, UB rt, UB req, UH value, UH index,
			     UB *data, UH len, INT *actual );
IMPORT ER   knl_usb_control_tmo( USBDEV *dev, UB rt, UB req, UH value,
				 UH index, UB *data, UH len, INT *actual,
				 TMO tmo );
IMPORT ER   knl_usb_get_desc( USBDEV *dev, UB type, UB idx, UH lang,
			      UB *buf, UH len, INT *actual );
IMPORT ER   knl_usb_set_interface( USBDEV *dev, INT ifno, INT alt );
IMPORT ER   knl_usb_quiesce( USBDEV *dev, INT ifno );
IMPORT ER   knl_usb_clear_halt( USBDEV *dev, UB ep );
IMPORT ER   knl_usb_enumerate( USBHC *hc, USBDEV *parent, INT port, INT speed,
			       USBDEV **out );
IMPORT void knl_usb_dev_free( USBDEV *dev );
IMPORT UB  *knl_usb_next_desc( UB *p, UB *end );
IMPORT void knl_usb_ep_from_desc( USBEP *ep, USBDEV *dev, CONST UB *d,
				  CONST UB *comp );
IMPORT INT  knl_usb_dev_class( USBDEV *dev );
IMPORT USBDEV *knl_usb_tt_hub( USBDEV *dev, INT *port );

/* ---------------------------------------------------------------- hub.c */

/* the port status, the change bits shifted up by 16 */
#define HUB_PS_CONNECTION	0x0001
#define HUB_PS_ENABLE		0x0002
#define HUB_PS_RESET		0x0010
#define HUB_PS_POWER		0x0100
#define HUB_PS_LOW_SPEED	0x0200
#define HUB_PS_HIGH_SPEED	0x0400
#define HUB_PC_CONNECTION	0x00010000
#define HUB_PC_RESET		0x00100000
#define HUB_PC_ALL		0x00ff0000

IMPORT ER   knl_hub_attach( USBDEV *hub );
IMPORT void knl_hub_detach( USBDEV *hub );
IMPORT ER   knl_hub_port_status( USBDEV *hub, INT port, UW *st );
IMPORT void knl_hub_clear_changes( USBDEV *hub, INT port, UW st );
IMPORT ER   knl_hub_port_reset( USBDEV *hub, INT port, INT *speed );

/* ---------------------------------------------------------------- classes */

/* hid.c: keyboards and pointers */
IMPORT BOOL knl_hid_match( USBDEV *dev );
IMPORT ER   knl_hid_attach( USBDEV *dev );
IMPORT void knl_hid_detach( USBDEV *dev );

/* msc.c: disks, over bulk-only transport */
IMPORT BOOL knl_msc_match( USBDEV *dev );
IMPORT ER   knl_msc_attach( USBDEV *dev );
IMPORT void knl_msc_detach( USBDEV *dev );
IMPORT void knl_msc_poll( void );
IMPORT void knl_msc_notify( void );
IMPORT BOOL knl_msc_wants_poll( void );

/* uac.c: audio */
IMPORT BOOL knl_uac_match( USBDEV *dev );
IMPORT ER   knl_uac_attach( USBDEV *dev );
IMPORT void knl_uac_detach( USBDEV *dev );
IMPORT void knl_uac_notify( void );

/* the log that goes to the console */
#define USB_LOG(...)	tm_printf((UB *)__VA_ARGS__)

#endif /* __USBDEV_H__ */
