/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netdev.c
 *	The network card as a T-Kernel device (design 10.4, 12.6).
 *
 *	Whichever card the target has is registered as "neta", so that a
 *	protocol stack reaches it through tk_rea_dev and tk_wri_dev instead
 *	of calling the driver directly. One read or write carries one whole
 *	Ethernet frame; the frame check sequence is the card's business.
 *
 *	The attribute data numbers below answer the address, the counters,
 *	the largest payload and whether the link carries.
 *
 *	The link layer underneath is virtio-net on QEMU and the Cadence GEM
 *	of the RP1 on the board; both offer the same four calls, so this
 *	file is the same for either.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>

LOCAL ID	netdev_id = 0;

/* ---------------------------------------------------------------- driver */

LOCAL ER netdev_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

LOCAL ER netdev_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

/*
 * Attribute data (start < 0). All of it is read only.
 */
LOCAL ER netdev_attr( T_DEVREQ *req )
{
	T_NETSTAT	st;
	ER		er;

	if ( req->cmd != TDC_READ ) {
		return E_RONLY;
	}

	switch ( req->start ) {
	case TDN_NETADDR:
		if ( req->size < NET_MAC_LEN ) {
			return E_PAR;
		}
		er = net_get_mac((UB *)req->buf);
		if ( er < E_OK ) {
			return er;
		}
		req->asize = NET_MAC_LEN;
		break;

	case TDN_NETSTAT:
		if ( req->size < (SZ)sizeof(T_NETSTAT) ) {
			return E_PAR;
		}
		er = net_stat((T_NETSTAT *)req->buf);
		if ( er < E_OK ) {
			return er;
		}
		req->asize = sizeof(T_NETSTAT);
		break;

	case TDN_NETMTU:
		if ( req->size < (SZ)sizeof(W) ) {
			return E_PAR;
		}
		*(W *)req->buf = NET_FRAME_MAX - (NET_MAC_LEN * 2 + 2);
		req->asize = sizeof(W);
		break;

	case TDN_NETLINK:
		if ( req->size < (SZ)sizeof(BOOL) ) {
			return E_PAR;
		}
		er = net_stat(&st);
		if ( er < E_OK ) {
			return er;
		}
		*(BOOL *)req->buf = st.up;
		req->asize = sizeof(BOOL);
		break;

	default:
		return E_PAR;
	}

	return E_OK;
}

/*
 * One request is one frame. A read waits for as long as the caller asked
 * and gives back what arrived; a write hands the frame to the card.
 */
LOCAL ER netdev_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	INT	n;

	req->asize = 0;
	req->error = E_OK;

	if ( req->start < 0 ) {
		req->error = netdev_attr(req);
		return E_OK;
	}
	if ( req->buf == NULL || req->size <= 0 ) {
		req->error = E_PAR;
		return E_OK;
	}

	if ( req->cmd == TDC_READ ) {
		n = net_recv(req->buf, req->size, tmout);
		if ( n < 0 ) {
			req->error = (ER)n;
		} else {
			req->asize = n;
		}
	} else {
		req->error = net_send(req->buf, req->size);
		if ( req->error >= E_OK ) {
			req->asize = req->size;
		}
	}

	return E_OK;
}

/*
 * Requests complete inside the execution function, so waiting only has
 * to report them.
 */
LOCAL INT netdev_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER netdev_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT netdev_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* ---------------------------------------------------------------- start-up */

/*
 * Registered only when a card answered, so that opening "neta" fails
 * on a machine without one instead of timing out on every read.
 */
EXPORT INT knl_netdev_init( void )
{
	T_DDEV	ddev;
	UB	mac[NET_MAC_LEN];

	if ( net_get_mac(mac) < E_OK ) {
		return 0;			/* no card on this machine */
	}

	ddev.exinf   = NULL;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_UNDEF;
	ddev.nsub    = 0;
	ddev.blksz   = -1;			/* not a block device */
	ddev.openfn  = (FP)netdev_open;
	ddev.closefn = (FP)netdev_close;
	ddev.execfn  = (FP)netdev_exec;
	ddev.waitfn  = (FP)netdev_wait;
	ddev.abortfn = (FP)netdev_abort;
	ddev.eventfn = (FP)netdev_event;

	netdev_id = tk_def_dev((UB *)"neta", &ddev, NULL);
	if ( netdev_id <= 0 ) {
		return netdev_id;
	}

	return 1;
}
