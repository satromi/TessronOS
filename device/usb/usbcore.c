/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usbcore.c
 *	Bringing a device up, and the standard requests (design 10.14).
 *
 *	A device on a port that was just reset answers at address 0 with a
 *	control endpoint of a size its speed suggests. Enumeration is:
 *	    an address (the controller chooses it)
 *	    the first eight bytes of the device descriptor, for the real size
 *	    of the control endpoint
 *	    the whole device descriptor
 *	    the configuration descriptor, its head first and then all of it
 *	    the maker and product strings
 *	    SET_CONFIGURATION with the first configuration
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "usbdev.h"

#define CTRL_TMO	1000	/* ms for a control transfer */
#define QUIESCE_TMO	200	/* ms for a request that only stops a stream */

EXPORT ER knl_usb_control_tmo( USBDEV *dev, UB rt, UB req, UH value, UH index,
			       UB *data, UH len, INT *actual, TMO tmo )
{
	UB	setup[8];

	setup[0] = rt;
	setup[1] = req;
	PUT16(setup + 2, value);
	PUT16(setup + 4, index);
	PUT16(setup + 6, len);

	return dev->hc->ops->control(dev->hc, dev, setup, data, actual, tmo);
}

EXPORT ER knl_usb_control( USBDEV *dev, UB rt, UB req, UH value, UH index,
			   UB *data, UH len, INT *actual )
{
	return knl_usb_control_tmo(dev, rt, req, value, index, data, len,
				   actual, CTRL_TMO);
}

/*
 * An interface back on its setting 0 once its stream is closed. Streams
 * are stopped with the manager's lock held, so a device that does not
 * answer must not hold everything up for a whole second; nothing
 * depends on the answer.
 */
EXPORT ER knl_usb_quiesce( USBDEV *dev, INT ifno )
{
	return knl_usb_control_tmo(dev, USB_RT_INTERFACE, USB_REQ_SET_INTERFACE,
				   0, (UH)ifno, NULL, 0, NULL, QUIESCE_TMO);
}

EXPORT ER knl_usb_get_desc( USBDEV *dev, UB type, UB idx, UH lang,
			    UB *buf, UH len, INT *actual )
{
	return knl_usb_control(dev, USB_RT_IN, USB_REQ_GET_DESCRIPTOR,
			       (UH)((type << 8) | idx), lang, buf, len, actual);
}

EXPORT ER knl_usb_set_interface( USBDEV *dev, INT ifno, INT alt )
{
	return knl_usb_control(dev, USB_RT_INTERFACE, USB_REQ_SET_INTERFACE,
			       (UH)alt, (UH)ifno, NULL, 0, NULL);
}

/* ENDPOINT_HALT is feature 0 */
EXPORT ER knl_usb_clear_halt( USBDEV *dev, UB ep )
{
	return knl_usb_control(dev, USB_RT_ENDPOINT, USB_REQ_CLEAR_FEATURE,
			       0, ep, NULL, 0, NULL);
}

/* The descriptor after p, NULL at the end or at one that does not fit */
EXPORT UB *knl_usb_next_desc( UB *p, UB *end )
{
	UB	*q;

	if ( p == NULL || p >= end || p[0] < 2 ) {
		return NULL;
	}
	q = p + p[0];

	return ( q + 2 <= end && q[0] >= 2 && q + q[0] <= end ) ? q : NULL;
}

/*
 * An endpoint from its descriptor d and, on SuperSpeed, the companion
 * descriptor that follows it (NULL if none).
 */
EXPORT void knl_usb_ep_from_desc( USBEP *ep, USBDEV *dev, CONST UB *d,
				  CONST UB *comp )
{
	UH	w = GET16(d + 4);

	knl_memset(ep, 0, sizeof(*ep));
	ep->addr     = d[2];
	ep->type     = d[3] & 3;
	ep->size     = w & 0x7ff;
	ep->mult     = ((w >> 11) & 3) + 1;
	ep->interval = d[6];
	if ( dev->speed >= USB_SPEED_SUPER && comp != NULL
	  && comp[1] == USB_DT_SS_EP_COMP ) {
		ep->burst   = comp[2];
		ep->ss_mult = ( ep->type == USB_EP_ISOC ) ? (comp[3] & 3) : 0;
		ep->mult    = 1;
		ep->esit    = ( comp[0] >= 6 ) ? GET16(comp + 4) : 0;
		if ( ep->esit == 0 ) {
			ep->esit = ep->size * (ep->burst + 1) * (ep->ss_mult + 1);
		}
	} else if ( dev->speed == USB_SPEED_HIGH
		 && ( ep->type == USB_EP_ISOC || ep->type == USB_EP_INTR ) ) {
		ep->esit = ep->size * ep->mult;
	} else {
		ep->mult = 1;
		ep->esit = ep->size;
	}
}

/*
 * A full or low speed device behind a high speed hub is reached through
 * that hub's transaction translator: the nearest high speed hub up the
 * tree, and in *port the port its branch hangs from. NULL for a fast
 * device and for one that reaches the root without such a hub.
 */
EXPORT USBDEV *knl_usb_tt_hub( USBDEV *dev, INT *port )
{
	USBDEV	*c;

	if ( dev->speed != USB_SPEED_LOW && dev->speed != USB_SPEED_FULL ) {
		return NULL;
	}
	for ( c = dev; c->parent != NULL; c = c->parent ) {
		if ( c->parent->speed == USB_SPEED_HIGH ) {
			*port = c->port;
			return c->parent;
		}
	}

	return NULL;
}

/*
 * The device's class, or that of its first interface when the device
 * leaves it to them (0x00, or 0xef for several functions)
 */
EXPORT INT knl_usb_dev_class( USBDEV *dev )
{
	UB	*p, *end;

	if ( dev->ddesc[4] != 0x00 && dev->ddesc[4] != 0xef ) {
		return dev->ddesc[4];
	}
	if ( dev->cfg == NULL ) {
		return 0;
	}
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			return p[5];
		}
	}

	return 0;
}

/* A string descriptor, UTF-16 in, ASCII out with '?' for the rest */
LOCAL void str_desc( USBDEV *dev, UB idx, UH lang, UB *dst, INT max )
{
	UB	buf[256];
	INT	n = 0, i, o = 0;

	dst[0] = 0;
	if ( idx == 0 ) {
		return;
	}
	if ( knl_usb_get_desc(dev, USB_DT_STRING, idx, lang, buf, 255, &n) < E_OK
	  || n < 2 ) {
		return;
	}
	if ( buf[0] < n ) {
		n = buf[0];
	}
	for ( i = 2; i + 1 < n && o < max - 1; i += 2 ) {
		UH	c = GET16(buf + i);

		dst[o++] = ( c >= 0x20 && c < 0x7f ) ? (UB)c : '?';
	}
	dst[o] = 0;
}

/* A descriptor, asked for up to three times: a device may be slow to wake */
LOCAL ER get_desc_retry( USBDEV *dev, UB type, UB *buf, UH len, INT *actual )
{
	INT	tries;
	ER	er = E_OK;

	for ( tries = 0; tries < 3; tries++ ) {
		er = knl_usb_get_desc(dev, type, 0, 0, buf, len, actual);
		if ( er >= E_OK ) {
			return er;
		}
		knl_usb_wait(20);
	}

	return er;
}

EXPORT void knl_usb_dev_free( USBDEV *dev )
{
	if ( dev == NULL ) {
		return;
	}
	dev->hc->ops->dev_free(dev->hc, dev);
	if ( dev->cfg != NULL ) {
		Kfree(dev->cfg);
	}
	Kfree(dev);
}

LOCAL CONST char *speed_name( INT s )
{
	switch ( s ) {
	case USB_SPEED_LOW:	return "low";
	case USB_SPEED_FULL:	return "full";
	case USB_SPEED_HIGH:	return "high";
	case USB_SPEED_SUPER:	return "super";
	default:		return "super+";
	}
}

EXPORT ER knl_usb_enumerate( USBHC *hc, USBDEV *parent, INT port, INT speed,
			     USBDEV **out )
{
	USBDEV	*dev;
	UB	hdr[9];
	UB	lang[4];
	INT	n = 0, total;
	UH	langid = 0x0409;
	ER	er;

	*out = NULL;
	dev = (USBDEV *)Kcalloc(1, sizeof(USBDEV));
	if ( dev == NULL ) {
		return E_NOMEM;
	}
	dev->hc     = hc;
	dev->parent = parent;
	dev->port   = port;
	dev->speed  = speed;
	if ( parent == NULL ) {
		dev->root_port = port;
		dev->route = 0;
		dev->depth = 0;
	} else {
		dev->root_port = parent->root_port;
		dev->depth = parent->depth + 1;
		dev->route = parent->route
			   | ((UW)( port > 15 ? 15 : port ) << (4 * parent->depth));
	}
	switch ( speed ) {
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:	dev->mps0 = 512; break;
	case USB_SPEED_HIGH:		dev->mps0 = 64;  break;
	default:			dev->mps0 = 8;   break;
	}
	er = hc->ops->dev_init(hc, dev);
	if ( er < E_OK ) {
		Kfree(dev);
		return er;
	}

	er = get_desc_retry(dev, USB_DT_DEVICE, dev->ddesc, 8, &n);
	if ( er < E_OK || n < 8 ) {
		USB_LOG("usb: %s port %d: no device descriptor (%d)\n",
			( parent != NULL ) ? "hub" : "root", port, (INT)er);
		goto fail;
	}
	if ( speed >= USB_SPEED_SUPER ) {
		dev->mps0 = 512;			/* bMaxPacketSize0 9 is 2^9 */
	} else if ( dev->ddesc[7] == 8 || dev->ddesc[7] == 16
		 || dev->ddesc[7] == 32 || dev->ddesc[7] == 64 ) {
		dev->mps0 = dev->ddesc[7];
	}
	er = hc->ops->dev_mps0(hc, dev);
	if ( er < E_OK ) {
		goto fail;
	}

	er = get_desc_retry(dev, USB_DT_DEVICE, dev->ddesc, 18, &n);
	if ( er < E_OK || n < 18 ) {
		goto fail;
	}
	er = get_desc_retry(dev, USB_DT_CONFIG, hdr, 9, &n);
	if ( er < E_OK || n < 9 ) {
		goto fail;
	}
	total = GET16(hdr + 2);
	if ( total < 9 || total > USB_CFG_MAX ) {
		er = E_NOSPT;
		goto fail;
	}
	dev->cfg = (UB *)Kmalloc((SZ)total);
	if ( dev->cfg == NULL ) {
		er = E_NOMEM;
		goto fail;
	}
	er = get_desc_retry(dev, USB_DT_CONFIG, dev->cfg, (UH)total, &n);
	if ( er < E_OK || n < 9 ) {
		if ( er >= E_OK ) {
			er = E_IO;
		}
		goto fail;
	}
	dev->cfglen = n;
	dev->cfg_value = dev->cfg[5];

	if ( knl_usb_get_desc(dev, USB_DT_STRING, 0, 0, lang, 4, &n) >= E_OK
	  && n >= 4 ) {
		langid = GET16(lang + 2);
	}
	str_desc(dev, dev->ddesc[14], langid, dev->maker, sizeof(dev->maker));
	str_desc(dev, dev->ddesc[15], langid, dev->product, sizeof(dev->product));

	USB_LOG("usb: %d-%d route %05x addr %d %s speed %04x:%04x class %02x \"%s\"\n",
		hc->index, dev->root_port, dev->route, dev->addr, speed_name(speed),
		(INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10),
		knl_usb_dev_class(dev), dev->product);

	er = knl_usb_control(dev, 0x00, USB_REQ_SET_CONFIG, dev->cfg_value, 0,
			     NULL, 0, NULL);
	if ( er < E_OK ) {
		goto fail;
	}
	*out = dev;

	return E_OK;

    fail:
	knl_usb_dev_free(dev);

	return ( er < E_OK ) ? er : E_IO;
}
