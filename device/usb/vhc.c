/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	vhc.c
 *	A USB controller in software, for the tests (USE_KTEST; design
 *	10.14): devices a test describes are plugged into its ports and
 *	answer as real ones would, so that what drives a kind of device the
 *	test machine does not have (a gamepad, a MIDI instrument) can be
 *	tried end to end, through the manager, the class drivers and the
 *	devices a process opens (uusb.c).
 *
 *	A device is its descriptors (the device's, the configuration's, its
 *	strings and the report descriptors of its HID interfaces) and what it
 *	does with the data: the test puts bytes for an IN endpoint with
 *	knl_vhc_in, and is told of what is sent to an OUT endpoint. The
 *	controller answers the standard requests itself (addresses, the
 *	configuration, settings, halts) and the class requests of HID that
 *	need no answer (SET_IDLE, SET_PROTOCOL, SET_REPORT). A stream on an
 *	IN endpoint is given the pieces the test put, one transfer each, when
 *	the manager serves the controllers. An isochronous OUT stream is asked
 *	for one packet each time the manager serves the controllers, and what
 *	it gives goes to the device's out as a bulk transfer's would.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/usb.h>
#include "usbdev.h"
#include "vhc.h"

#ifdef USE_KTEST

#define VHC_PORTS	2
#define VHC_PIPES	8
#define VHC_QUEUE	32		/* pieces waiting on an IN endpoint */
#define VHC_PIECE	64

typedef struct {
	UB	ep;
	INT	len;
	UB	data[VHC_PIECE];
} PIECE;

struct usb_pipe {
	BOOL		used;
	INT		port;
	UB		ep;
	INT		xfer;
	USB_XFERFN	fn;
	USB_FILLFN	fill;		/* an OUT stream's, NULL for IN */
	void		*ctx;
	DMABUF		buf;
};

typedef struct {
	CONST T_VHCDEV	*dev;		/* NULL: nothing plugged in */
	BOOL		changed;
	INT		addr;
	USBDEV		*usb;
	PIECE		q[VHC_QUEUE];
	INT		qhead, qn;
} PORT;

LOCAL USBHC		vhc_hc;
LOCAL PORT		vhc_port[VHC_PORTS];
LOCAL struct usb_pipe	vhc_pipe[VHC_PIPES];
LOCAL BOOL		vhc_added;
LOCAL ID		vhc_lock;	/* the queues, between the tests and the manager */

LOCAL void q_lock( void )
{
	tk_wai_sem(vhc_lock, 1, TMO_FEVR);
}

LOCAL void q_unlock( void )
{
	tk_sig_sem(vhc_lock, 1);
}

LOCAL PORT *port_of( USBDEV *dev )
{
	INT	i;

	for ( i = 0; i < VHC_PORTS; i++ ) {
		if ( vhc_port[i].usb == dev && dev != NULL ) {
			return &vhc_port[i];
		}
	}
	return ( dev != NULL && dev->parent == NULL && dev->port >= 0 && dev->port < VHC_PORTS )
		? &vhc_port[dev->port] : NULL;
}

/* ---------------------------------------------------------------- the ports */

LOCAL INT vhc_nports( USBHC *hc )
{
	(void)hc;
	return VHC_PORTS;
}

LOCAL BOOL vhc_port_connected( USBHC *hc, INT port )
{
	(void)hc;
	return port >= 0 && port < VHC_PORTS && vhc_port[port].dev != NULL;
}

LOCAL ER vhc_port_status( USBHC *hc, INT port, UW ack, UW *st )
{
	(void)hc;
	if ( port < 0 || port >= VHC_PORTS ) {
		return E_PAR;
	}
	*st = ( vhc_port[port].dev != NULL ) ? ( HUB_PS_CONNECTION | HUB_PS_ENABLE | HUB_PS_POWER ) : HUB_PS_POWER;
	if ( vhc_port[port].changed ) {
		*st |= HUB_PC_CONNECTION;
		if ( (ack & HUB_PC_CONNECTION) != 0 ) {
			vhc_port[port].changed = FALSE;
		}
	}
	return E_OK;
}

LOCAL ER vhc_port_reset( USBHC *hc, INT port, INT *speed )
{
	(void)hc;
	if ( port < 0 || port >= VHC_PORTS || vhc_port[port].dev == NULL ) {
		return E_NOEXS;
	}
	*speed = USB_SPEED_FULL;
	return E_OK;
}

LOCAL ER vhc_port_state( USBHC *hc, INT port, T_USBPORT *p )
{
	(void)hc;
	knl_memset(p, 0, sizeof(*p));
	if ( port < 0 || port >= VHC_PORTS ) {
		return E_PAR;
	}
	p->connected = ( vhc_port[port].dev != NULL );
	p->enabled = p->connected;
	p->speed = USB_SPEED_FULL;
	return E_OK;
}

LOCAL ER vhc_dev_init( USBHC *hc, USBDEV *dev )
{
	PORT	*pt;

	(void)hc;
	if ( dev->parent != NULL || dev->port < 0 || dev->port >= VHC_PORTS ) {
		return E_NOSPT;
	}
	pt = &vhc_port[dev->port];
	pt->usb = dev;
	pt->addr = dev->port + 1;
	dev->addr = pt->addr;
	return E_OK;
}

LOCAL ER vhc_dev_mps0( USBHC *hc, USBDEV *dev )
{
	(void)hc;
	(void)dev;
	return E_OK;
}

LOCAL ER vhc_hub_config( USBHC *hc, USBDEV *hub )
{
	(void)hc;
	(void)hub;
	return E_NOSPT;
}

LOCAL void vhc_dev_free( USBHC *hc, USBDEV *dev )
{
	INT	i;

	(void)hc;
	for ( i = 0; i < VHC_PIPES; i++ ) {
		if ( vhc_pipe[i].used && dev != NULL && dev->parent == NULL && vhc_pipe[i].port == dev->port ) {
			if ( vhc_pipe[i].fill == NULL ) knl_dmabuf_free(&vhc_pipe[i].buf);
			vhc_pipe[i].used = FALSE;
		}
	}
	for ( i = 0; i < VHC_PORTS; i++ ) {
		if ( vhc_port[i].usb == dev ) {
			vhc_port[i].usb = NULL;
		}
	}
}

/* ---------------------------------------------------------------- transfers */

/* Bytes of a descriptor into the data stage: how many */
LOCAL INT give( UB *data, INT want, CONST UB *src, INT len )
{
	INT	n = ( len < want ) ? len : want;

	if ( n > 0 && data != NULL && src != NULL ) {
		knl_memcpy(data, src, n);
	}
	return ( n > 0 ) ? n : 0;
}

LOCAL ER vhc_control( USBHC *hc, USBDEV *dev, CONST UB *setup, UB *data, INT *actual, TMO tmo )
{
	PORT		*pt = port_of(dev);
	CONST T_VHCDEV	*d = ( pt != NULL ) ? pt->dev : NULL;
	UB		rt = setup[0], req = setup[1];
	UH		value = (UH)( setup[2] | ( setup[3] << 8 ) ), index = (UH)( setup[4] | ( setup[5] << 8 ) );
	INT		len = setup[6] | ( setup[7] << 8 ), n = 0, i;
	UB		lang[4] = { 4, USB_DT_STRING, 0x09, 0x04 };
	UB		str[2 + 2 * 32];

	(void)hc;
	(void)tmo;
	if ( actual != NULL ) *actual = 0;
	if ( d == NULL ) {
		return E_IO;
	}
	if ( req == USB_REQ_GET_DESCRIPTOR && ( rt & 0x80 ) != 0 ) {
		UB	type = (UB)( value >> 8 ), idx = (UB)value;

		if ( ( rt & 0x1f ) == USB_RT_INTERFACE && type == USB_DT_HID_REPORT ) {
			/* a HID interface's report descriptor */
			if ( index >= VHC_MAX_IF || d->report[index] == NULL ) return E_IO;
			n = give(data, len, d->report[index], d->report_len[index]);
		} else if ( type == USB_DT_DEVICE ) {
			n = give(data, len, d->ddesc, 18);
		} else if ( type == USB_DT_CONFIG ) {
			n = give(data, len, d->cfg, d->cfglen);
		} else if ( type == USB_DT_STRING ) {
			if ( idx == 0 ) {
				n = give(data, len, lang, 4);
			} else if ( idx <= VHC_MAX_STR && d->str[idx - 1] != NULL ) {
				CONST char *s = d->str[idx - 1];

				for ( i = 0; s[i] != '\0' && i < 32; i++ ) {
					str[2 + 2 * i] = (UB)s[i];
					str[3 + 2 * i] = 0;
				}
				str[0] = (UB)( 2 + 2 * i );
				str[1] = USB_DT_STRING;
				n = give(data, len, str, str[0]);
			} else {
				return E_IO;
			}
		} else {
			return E_IO;			/* a stall */
		}
	} else if ( req == USB_REQ_GET_STATUS && ( rt & 0x80 ) != 0 ) {
		UB	st[2] = { 0, 0 };

		n = give(data, len, st, 2);
	} else if ( ( rt & 0x80 ) == 0 && ( rt & 0x60 ) == 0 ) {
		/* SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE, SET_FEATURE */
		n = 0;
	} else if ( ( rt & 0x60 ) == 0x20 && ( rt & 0x1f ) <= 1 ) {
		/* a class request to an interface (HID's SET_IDLE, SET_PROTOCOL, SET_REPORT) or the device (HCI's commands) */
		if ( d->klass != NULL ) {
			n = d->klass(setup, data, len);
			if ( n < 0 ) return E_IO;
		} else if ( ( rt & 0x80 ) != 0 ) {
			return E_IO;
		}
	} else {
		return E_IO;
	}
	if ( actual != NULL ) *actual = n;
	return E_OK;
}

LOCAL ER vhc_bulk( USBHC *hc, USBDEV *dev, CONST USBEP *ep, UB *data, INT len, INT *actual, TMO tmo )
{
	PORT	*pt = port_of(dev);

	(void)hc;
	(void)tmo;
	if ( actual != NULL ) *actual = 0;
	if ( pt == NULL || pt->dev == NULL ) {
		return E_IO;
	}
	if ( ( ep->addr & 0x80 ) != 0 ) {
		return E_NOSPT;			/* IN goes through a stream */
	}
	if ( pt->dev->out != NULL ) {
		pt->dev->out(ep->addr, data, len);
	}
	if ( actual != NULL ) *actual = len;
	return E_OK;
}

LOCAL ER vhc_stream_open( USBHC *hc, USBDEV *dev, CONST USBEP *ep, INT xfer, USB_XFERFN fn, void *ctx,
			  USBPIPE **out )
{
	PORT	*pt = port_of(dev);
	INT	i;

	(void)hc;
	if ( pt == NULL || ( ep->addr & 0x80 ) == 0 ) {
		return E_NOSPT;
	}
	for ( i = 0; i < VHC_PIPES; i++ ) {
		if ( !vhc_pipe[i].used ) break;
	}
	if ( i == VHC_PIPES ) {
		return E_LIMIT;
	}
	if ( knl_dmabuf_alloc(&vhc_pipe[i].buf, ( xfer > VHC_PIECE ) ? xfer : VHC_PIECE) < E_OK ) {
		return E_NOMEM;
	}
	vhc_pipe[i].used = TRUE;
	vhc_pipe[i].port = (INT)( pt - vhc_port );
	vhc_pipe[i].ep = ep->addr;
	vhc_pipe[i].xfer = xfer;
	vhc_pipe[i].fn = fn;
	vhc_pipe[i].fill = NULL;
	vhc_pipe[i].ctx = ctx;
	*out = &vhc_pipe[i];
	return E_OK;
}

LOCAL ER vhc_stream_open_out( USBHC *hc, USBDEV *dev, CONST USBEP *ep, USB_FILLFN fill, void *ctx, USBPIPE **out )
{
	PORT	*pt = port_of(dev);
	INT	i;

	(void)hc;
	if ( pt == NULL || ( ep->addr & 0x80 ) != 0 || ep->type != USB_EP_ISOC || ep->size <= 0 || ep->size > VHC_PIECE ) {
		return E_NOSPT;
	}
	for ( i = 0; i < VHC_PIPES; i++ ) {
		if ( !vhc_pipe[i].used ) break;
	}
	if ( i == VHC_PIPES ) {
		return E_LIMIT;
	}
	vhc_pipe[i].used = TRUE;
	vhc_pipe[i].port = (INT)( pt - vhc_port );
	vhc_pipe[i].ep = ep->addr;
	vhc_pipe[i].xfer = ep->size;
	vhc_pipe[i].fn = NULL;
	vhc_pipe[i].fill = fill;
	vhc_pipe[i].ctx = ctx;
	*out = &vhc_pipe[i];
	return E_OK;
}

LOCAL void vhc_stream_close( USBHC *hc, USBPIPE *pipe )
{
	(void)hc;
	if ( pipe != NULL && pipe->used ) {
		if ( pipe->fill == NULL ) knl_dmabuf_free(&pipe->buf);
		pipe->used = FALSE;
	}
}

/* The pieces the tests put, each to the stream of its endpoint */
LOCAL void vhc_poll( USBHC *hc )
{
	INT	p, i, k;

	(void)hc;
	for ( i = 0; i < VHC_PIPES; i++ ) {
		struct usb_pipe	*pp = &vhc_pipe[i];
		UB		pk[VHC_PIECE];
		INT		n;

		if ( !pp->used || pp->fill == NULL ) continue;
		n = pp->fill(pp->ctx, pk, pp->xfer);
		if ( n > 0 && vhc_port[pp->port].dev != NULL && vhc_port[pp->port].dev->out != NULL ) {
			vhc_port[pp->port].dev->out(pp->ep, pk, n);
		}
	}
	for ( p = 0; p < VHC_PORTS; p++ ) {
		PORT	*pt = &vhc_port[p];

		q_lock();
		for ( k = 0; k < pt->qn; ) {
			PIECE		*pc = &pt->q[( pt->qhead + k ) % VHC_QUEUE];
			struct usb_pipe	*pp = NULL;

			for ( i = 0; i < VHC_PIPES; i++ ) {
				if ( vhc_pipe[i].used && vhc_pipe[i].fill == NULL && vhc_pipe[i].port == p && vhc_pipe[i].ep == pc->ep ) {
					pp = &vhc_pipe[i];
				}
			}
			if ( pp == NULL ) {
				k++;			/* no one reads the endpoint yet: kept */
				continue;
			}
			{
				INT	n = ( pc->len < pp->xfer ) ? pc->len : pp->xfer;

				knl_memcpy(pp->buf.virt[0], pc->data, n);
				/* the piece out of the queue before the reader is told */
				if ( k == 0 ) {
					pt->qhead = ( pt->qhead + 1 ) % VHC_QUEUE;
				} else {
					INT	j;

					for ( j = k; j < pt->qn - 1; j++ ) {
						pt->q[( pt->qhead + j ) % VHC_QUEUE] = pt->q[( pt->qhead + j + 1 ) % VHC_QUEUE];
					}
				}
				pt->qn--;
				q_unlock();
				pp->fn(pp->ctx, &pp->buf, 0, n, 0);
				q_lock();
			}
		}
		q_unlock();
	}
}

LOCAL void vhc_info( USBHC *hc, T_USBHC *info )
{
	(void)hc;
	knl_memset(info, 0, sizeof(*info));
	info->nports = VHC_PORTS;
	info->version = 0x0200;
}

LOCAL CONST USBHC_OPS vhc_ops = {
	vhc_nports, vhc_port_connected, vhc_port_status, vhc_port_reset, vhc_port_state,
	vhc_dev_init, vhc_dev_mps0, vhc_hub_config, vhc_dev_free, vhc_control, vhc_bulk,
	vhc_stream_open, vhc_stream_open_out, vhc_stream_close, vhc_poll, vhc_info,
	NULL,				/* its endpoints keep no toggle of their own */
	NULL,				/* no USB 3 ports */
	NULL				/* its ports are always powered */
};

/* ---------------------------------------------------------------- for the tests */

EXPORT ER knl_vhc_plug( INT port, CONST T_VHCDEV *dev )
{
	if ( port < 0 || port >= VHC_PORTS ) {
		return E_PAR;
	}
	if ( vhc_lock <= 0 ) {
		T_CSEM	csem;

		csem.exinf = NULL;
		csem.sematr = TA_TFIFO;
		csem.isemcnt = 1;
		csem.maxsem = 1;
		vhc_lock = tk_cre_sem(&csem);
		if ( vhc_lock <= 0 ) {
			return E_LIMIT;
		}
	}
	knl_usb_lock();
	if ( !vhc_added ) {
		vhc_hc.ops = &vhc_ops;
		vhc_hc.intno = 0;
		if ( knl_usb_add_hc(&vhc_hc) < E_OK ) {
			knl_usb_unlock();
			return E_LIMIT;
		}
		vhc_added = TRUE;
	}
	q_lock();
	vhc_port[port].dev = dev;
	vhc_port[port].changed = TRUE;
	vhc_port[port].qhead = 0;
	vhc_port[port].qn = 0;
	q_unlock();
	knl_usb_unlock();
	(void)ts_usb_rescan();
	return E_OK;
}

EXPORT ER knl_vhc_unplug( INT port )
{
	if ( port < 0 || port >= VHC_PORTS || !vhc_added ) {
		return E_PAR;
	}
	knl_usb_lock();
	vhc_port[port].dev = NULL;
	vhc_port[port].changed = TRUE;
	knl_usb_unlock();
	(void)ts_usb_rescan();
	return E_OK;
}

EXPORT ER knl_vhc_in( INT port, UB ep, CONST UB *data, INT len )
{
	PORT	*pt;
	PIECE	*pc;

	if ( port < 0 || port >= VHC_PORTS || len < 0 || len > VHC_PIECE || vhc_lock <= 0 ) {
		return E_PAR;
	}
	pt = &vhc_port[port];
	q_lock();
	if ( pt->qn == VHC_QUEUE ) {
		pt->qhead = ( pt->qhead + 1 ) % VHC_QUEUE;	/* the oldest let go */
		pt->qn--;
	}
	pc = &pt->q[( pt->qhead + pt->qn ) % VHC_QUEUE];
	pc->ep = ep;
	pc->len = len;
	knl_memcpy(pc->data, data, len);
	pt->qn++;
	q_unlock();
	knl_usb_kick();
	return E_OK;
}

#endif /* USE_KTEST */
