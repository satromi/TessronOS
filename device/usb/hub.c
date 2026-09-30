/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hub.c
 *	USB 2.0 and SuperSpeed hubs (design 10.14).
 *
 *	A hub says which of its ports changed on its status change endpoint:
 *	one bit per port, bit 0 for the hub itself. That endpoint is kept
 *	armed as a stream, and a report wakes the manager's task, which then
 *	asks the ports that changed for their status. So a hub costs nothing
 *	while nothing is plugged in or taken out.
 *
 *	A high speed device behind a high speed hub needs nothing more; a
 *	full or low speed one is reached through the hub's transaction
 *	translator, which the controller addresses (the slot context names
 *	the hub and the port).
 *
 *	A SuperSpeed hub differs in its descriptor (type 0x2A), needs its
 *	tier set with SET_HUB_DEPTH before its ports are used, and reports
 *	the port status with other bits: power at bit 9, the link state
 *	where the speed was, and change bits of its own. Its ports carry only
 *	SuperSpeed devices; the USB 2 devices plugged into the same box come
 *	up on the USB 2 hub the box shows on a USB 2 port.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "usbdev.h"

#define HUB_REQ_OUT		(USB_RT_CLASS | USB_RT_OTHER)		/* 0x23 */
#define HUB_REQ_IN		(USB_RT_IN | USB_RT_CLASS | USB_RT_OTHER)	/* 0xa3 */
#define HUB_REQ_DEVIN		(USB_RT_IN | USB_RT_CLASS)		/* 0xa0 */
#define HUB_REQ_DEVOUT		(USB_RT_CLASS)				/* 0x20 */

#define REQ_SET_HUB_DEPTH	12

#define FEAT_PORT_RESET		4
#define FEAT_PORT_POWER		8
#define FEAT_C_PORT_CONNECTION	16
#define FEAT_C_PORT_ENABLE	17
#define FEAT_C_PORT_SUSPEND	18
#define FEAT_C_PORT_OVERCURRENT	19
#define FEAT_C_PORT_RESET	20
#define FEAT_C_PORT_LINK_STATE	25
#define FEAT_C_PORT_CONFIG_ERR	26
#define FEAT_C_BH_PORT_RESET	29

LOCAL BOOL ss_hub( USBDEV *hub )
{
	return ( hub->speed >= USB_SPEED_SUPER );
}

/* The status change endpoint reported: the ports it names are looked at */
LOCAL void hub_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	USBDEV	*hub = (USBDEV *)ctx;
	UB	map[4];
	INT	n = ( len > 4 ) ? 4 : len;

	if ( err != 0 || n <= 0 ) {
		return;
	}
	map[0] = map[1] = map[2] = map[3] = 0;
	knl_dmabuf_read(b, off, map, n);
	hub->hub_change |= GET32(map);
	knl_usb_hub_changed();
}

/* The hub's status change endpoint, if it has one */
LOCAL void hub_stream( USBDEV *hub )
{
	UB	*p, *end = hub->cfg + hub->cfglen, *comp;
	USBEP	ep;

	for ( p = hub->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] != USB_DT_ENDPOINT || p[0] < 7 || (p[2] & 0x80) == 0
		  || (p[3] & 3) != USB_EP_INTR ) {
			continue;
		}
		comp = knl_usb_next_desc(p, end);
		knl_usb_ep_from_desc(&ep, hub, p, comp);
		if ( hub->hc->ops->stream_open(hub->hc, hub, &ep,
				( ep.size > 4 ) ? 4 : ep.size,
				hub_xfer, hub, &hub->hub_pipe) < E_OK ) {
			hub->hub_pipe = NULL;
		}
		return;
	}
}

EXPORT ER knl_hub_attach( USBDEV *hub )
{
	UB	d[16];
	INT	n = 0, i, pwr_delay;
	ER	er;
	BOOL	ss = ss_hub(hub);

	er = knl_usb_control(hub, HUB_REQ_DEVIN, USB_REQ_GET_DESCRIPTOR,
			     (UH)(( ss ? USB_DT_SS_HUB : USB_DT_HUB ) << 8), 0,
			     d, sizeof(d), &n);
	if ( er < E_OK || n < 7 ) {
		return ( er < E_OK ) ? er : E_IO;
	}
	hub->hub_ports = d[2];
	if ( hub->hub_ports > 15 ) {
		hub->hub_ports = 15;
	}
	pwr_delay = d[5] * 2;
	hub->role = USB_ROLE_HUB;
	hub->hub_ignored = 0;
	hub->hub_change = 0;
	if ( ss ) {
		knl_usb_control(hub, HUB_REQ_DEVOUT, REQ_SET_HUB_DEPTH,
				(UH)hub->depth, 0, NULL, 0, NULL);
	}
	er = hub->hc->ops->hub_config(hub->hc, hub);
	if ( er < E_OK ) {
		USB_LOG("usb: hub: the controller was not told (%d)\n", (INT)er);
	}
	for ( i = 1; i <= hub->hub_ports; i++ ) {
		knl_usb_control(hub, HUB_REQ_OUT, USB_REQ_SET_FEATURE,
				FEAT_PORT_POWER, (UH)i, NULL, 0, NULL);
	}
	knl_usb_wait(( pwr_delay < 100 ) ? 100 : pwr_delay);
	hub_stream(hub);
	USB_LOG("usb: hub: %s%d ports%s\n", ss ? "SuperSpeed, " : "",
		hub->hub_ports, ( hub->hub_pipe != NULL ) ? "" : ", polled");

	return E_OK;
}

EXPORT void knl_hub_detach( USBDEV *hub )
{
	if ( hub->hub_pipe != NULL ) {
		hub->hc->ops->stream_close(hub->hc, hub->hub_pipe);
		hub->hub_pipe = NULL;
	}
}

EXPORT ER knl_hub_port_status( USBDEV *hub, INT port, UW *st )
{
	UB	s[4];
	INT	n = 0;
	ER	er;

	er = knl_usb_control(hub, HUB_REQ_IN, USB_REQ_GET_STATUS, 0, (UH)port,
			     s, 4, &n);
	if ( er < E_OK || n < 4 ) {
		return ( er < E_OK ) ? er : E_IO;
	}
	*st = GET16(s) | ((UW)GET16(s + 2) << 16);

	return E_OK;
}

/* Acknowledge every change bit set in st */
EXPORT void knl_hub_clear_changes( USBDEV *hub, INT port, UW st )
{
	LOCAL CONST struct { UW bit; UH feat; } ch2[] = {
		{ 0x00010000, FEAT_C_PORT_CONNECTION },
		{ 0x00020000, FEAT_C_PORT_ENABLE },
		{ 0x00040000, FEAT_C_PORT_SUSPEND },
		{ 0x00080000, FEAT_C_PORT_OVERCURRENT },
		{ 0x00100000, FEAT_C_PORT_RESET },
	}, ch3[] = {
		{ 0x00010000, FEAT_C_PORT_CONNECTION },
		{ 0x00080000, FEAT_C_PORT_OVERCURRENT },
		{ 0x00100000, FEAT_C_PORT_RESET },
		{ 0x00200000, FEAT_C_BH_PORT_RESET },
		{ 0x00400000, FEAT_C_PORT_LINK_STATE },
		{ 0x00800000, FEAT_C_PORT_CONFIG_ERR },
	};
	BOOL	ss = ss_hub(hub);
	INT	i, n = ss ? (INT)(sizeof(ch3) / sizeof(ch3[0]))
			  : (INT)(sizeof(ch2) / sizeof(ch2[0]));

	for ( i = 0; i < n; i++ ) {
		UW	bit  = ss ? ch3[i].bit : ch2[i].bit;
		UH	feat = ss ? ch3[i].feat : ch2[i].feat;

		if ( (st & bit) != 0 ) {
			knl_usb_control(hub, HUB_REQ_OUT, USB_REQ_CLEAR_FEATURE,
					feat, (UH)port, NULL, 0, NULL);
		}
	}
}

EXPORT ER knl_hub_port_reset( USBDEV *hub, INT port, INT *speed )
{
	UW	st = 0;
	INT	i;
	ER	er;

	er = knl_usb_control(hub, HUB_REQ_OUT, USB_REQ_SET_FEATURE,
			     FEAT_PORT_RESET, (UH)port, NULL, 0, NULL);
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < 25; i++ ) {
		knl_usb_wait(20);
		er = knl_hub_port_status(hub, port, &st);
		if ( er < E_OK ) {
			return er;
		}
		if ( (st & HUB_PC_RESET) != 0 || (st & HUB_PS_RESET) == 0 ) {
			break;
		}
	}
	knl_hub_clear_changes(hub, port, st);
	if ( (st & HUB_PS_CONNECTION) == 0 ) {
		return E_NOEXS;
	}
	if ( (st & HUB_PS_ENABLE) == 0 ) {
		return E_IO;
	}
	knl_usb_wait(10);				/* reset recovery */
	if ( ss_hub(hub) ) {
		*speed = USB_SPEED_SUPER;
	} else if ( (st & HUB_PS_LOW_SPEED) != 0 ) {
		*speed = USB_SPEED_LOW;
	} else if ( (st & HUB_PS_HIGH_SPEED) != 0 ) {
		*speed = USB_SPEED_HIGH;
	} else {
		*speed = USB_SPEED_FULL;
	}

	return E_OK;
}
