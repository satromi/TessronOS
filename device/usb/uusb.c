/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uusb.c
 *	USB devices for processes: SVC class 12, us_ (design 6.3.1,
 *	include/ts/usb.h).
 *
 *	A process lists the devices and reads their descriptors; a device no
 *	class driver of the kernel took (USB_ROLE_NONE, not a hub) it may
 *	open, one process at a time. It is given a number of its own for it.
 *	It then makes requests on the control endpoint, picks an interface's
 *	alternate setting, clears an endpoint's halt, resets the device, and
 *	moves data on the bulk, interrupt and isochronous endpoints of the
 *	settings it picked. What a process opened is closed when it ends,
 *	and a device that goes away answers E_NOEXS from then on.
 *
 *	A request on the control endpoint and a transfer OUT hold the USB
 *	manager's lock while they run, for up to their time-out (at most
 *	US_TMO_MAX). An IN endpoint is not read while the lock is held: the
 *	first transfer IN opens a stream on it, whose transfers the manager
 *	finishes as they come; each one's bytes are kept, as one piece, in a
 *	ring of the endpoint's, and a transfer IN takes the oldest piece,
 *	waiting outside the lock up to its time-out for one. A piece longer
 *	than the transfer asks for is cut, the rest let go. A ring that is
 *	full lets the oldest pieces go. An isochronous IN endpoint is read
 *	the same way, a piece a service interval. An isochronous OUT
 *	endpoint has a stream too, whose packets the controller asks for one
 *	a service interval: a transfer OUT on it puts its bytes, as one
 *	packet, in the endpoint's ring and returns, waiting only while the
 *	ring is full; a service with no packet waiting sends none. Setting an
 *	alternate setting, clearing a halt and resetting close the streams;
 *	the next transfer opens them again.
 *
 *	A reset resets the device's port, gives up its slot and addresses
 *	it again, with the configuration it had (knl_usb_reset_device): the
 *	device keeps its number and the process its hold on it.
 *
 *	Bytes go in and out through buffers of the kernel's, with knl_ua_get
 *	and knl_ua_put (PAN).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/usb.h>
#include "usbdev.h"

#define US_OPEN_MAX	8		/* devices open by processes in all */
#define US_IN_MAX	8		/* IN endpoints with a stream, a device */
#define US_RING		( 64 * 1024 )	/* bytes kept for an IN endpoint */
#define US_XFER_MAX	( 64 * 1024 )	/* the longest transfer */
#define US_TMO_MAX	10000		/* ms a request holds the manager at most */
#define US_IFS		32		/* interfaces whose setting is kept */

typedef struct {
	UB	addr;			/* 0: none */
	USBEP	ep;
	USBPIPE	*pipe;
	UB	*ring;
	INT	head, tail, used;	/* pieces: 2 bytes of length, then the bytes */
	INT	pieces;
	BOOL	failed;			/* the stream ended in an error */
	BOOL	out;			/* an isochronous OUT endpoint's packets to send */
} USIN;

typedef struct {
	ID	pid;			/* 0: a free entry */
	INT	handle;			/* the device's number in the tree */
	USBDEV	*dev;			/* NULL once it is gone */
	ID	flg;			/* bit n: endpoint n's ring took a piece; bit 31: gone */
	UB	alt[US_IFS];		/* the alternate setting picked for each interface */
	USIN	in[US_IN_MAX];
} USOPEN;

LOCAL USOPEN	us_open[US_OPEN_MAX];
LOCAL ID	us_mtx;			/* opening and closing, one at a time */

#define FLG_GONE	0x80000000U

LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

/* The entry of a number of the caller's; NULL when it is not one */
LOCAL USOPEN *open_of( INT h )
{
	ID	pid = ts_get_pid();

	if ( h <= 0 || h > US_OPEN_MAX || pid <= 0 || us_open[h - 1].pid != pid ) {
		return NULL;
	}
	return &us_open[h - 1];
}

/* ---------------------------------------------------------------- the streams IN */

/*
 * The streams are told apart by a context of their own: the device's
 * entry and the endpoint's place in it.
 */
typedef struct {
	USOPEN	*o;
	INT	k;
} USCTX;

LOCAL USCTX	us_ctx[US_OPEN_MAX][US_IN_MAX];

/* One finished transfer of an IN endpoint, with the manager's lock held: kept as a piece */
LOCAL void in_piece( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	USCTX	*c = (USCTX *)ctx;
	USIN	*e = &c->o->in[c->k];
	INT	need, i;
	UB	hdr[2];

	if ( e->ring == NULL ) {
		return;
	}
	if ( err != 0 ) {
		e->failed = TRUE;
		if ( c->o->flg > 0 ) tk_set_flg(c->o->flg, 1U << c->k);
		return;
	}
	if ( len < 0 ) {
		return;
	}
	need = len + 2;
	if ( need > US_RING ) {
		return;
	}
	while ( e->used + need > US_RING && e->pieces > 0 ) {	/* full: the oldest go */
		INT	n = e->ring[e->head] | ( e->ring[( e->head + 1 ) % US_RING] << 8 );

		e->head = ( e->head + 2 + n ) % US_RING;
		e->used -= n + 2;
		e->pieces--;
	}
	hdr[0] = (UB)len;
	hdr[1] = (UB)( len >> 8 );
	for ( i = 0; i < 2; i++ ) {
		e->ring[e->tail] = hdr[i];
		e->tail = ( e->tail + 1 ) % US_RING;
	}
	while ( len > 0 ) {
		INT	n = US_RING - e->tail;

		if ( n > len ) n = len;
		knl_dmabuf_read(b, off, e->ring + e->tail, n);
		e->tail = ( e->tail + n ) % US_RING;
		off += n;
		len -= n;
	}
	e->used += need;
	e->pieces++;
	if ( c->o->flg > 0 ) tk_set_flg(c->o->flg, 1U << c->k);
}

/* The packet an isochronous OUT stream sends next, with the manager's lock held: none, 0 */
LOCAL INT take_piece( USIN *e, UB *k, INT len );

LOCAL INT out_fill( void *ctx, UB *dst, INT max )
{
	USCTX	*c = (USCTX *)ctx;
	USIN	*e = &c->o->in[c->k];
	INT	n;

	if ( e->ring == NULL ) {
		return 0;
	}
	n = take_piece(e, dst, max);
	if ( n < 0 ) {
		return 0;
	}
	if ( c->o->flg > 0 ) tk_set_flg(c->o->flg, 1U << c->k);	/* room in the ring */
	return n;
}

/* The streams of a device closed, with the manager's lock held */
LOCAL void close_streams( USOPEN *o )
{
	INT	i;

	for ( i = 0; i < US_IN_MAX; i++ ) {
		USIN	*e = &o->in[i];

		if ( e->pipe != NULL && o->dev != NULL ) {
			o->dev->hc->ops->stream_close(o->dev->hc, e->pipe);
		}
		e->pipe = NULL;
		e->addr = 0;
		e->head = e->tail = e->used = e->pieces = 0;
		e->failed = FALSE;
		e->out = FALSE;
	}
}

/*
 * The endpoint ep of the settings picked, as its descriptors say: FALSE
 * when no interface of the device has it now. With the lock held.
 */
LOCAL BOOL find_ep( USOPEN *o, UB addr, USBEP *ep )
{
	USBDEV	*d = o->dev;
	UB	*p, *end;
	INT	ifno = -1, alt = 0;

	if ( d == NULL || d->cfg == NULL ) {
		return FALSE;
	}
	end = d->cfg + d->cfglen;
	for ( p = d->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			ifno = p[2];
			alt = p[3];
		} else if ( p[1] == USB_DT_ENDPOINT && p[0] >= 7 && p[2] == addr && ifno >= 0
			 && alt == ( ifno < US_IFS ? o->alt[ifno] : 0 ) ) {
			knl_usb_ep_from_desc(ep, d, p, knl_usb_next_desc(p, end));
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the calls */

/* us_lst_dev: the devices, up to max of them into list; their count */
LOCAL INT svc_us_lst_dev( T_USBDEV *list, INT max )
{
	T_USBDEV	*k;
	INT		n, i;

	if ( max < 0 ) return E_PAR;
	if ( max > 0 && !user_ok(list, sizeof(*list) * (SZ)max, TRUE) ) return E_MACV;
	n = ts_usb_lst_dev(NULL, 0);		/* no more room than there are devices */
	if ( max > n ) max = n;
	k = ( max > 0 ) ? (T_USBDEV *)Kmalloc(sizeof(*k) * (SZ)max) : NULL;
	if ( max > 0 && k == NULL ) return E_NOMEM;
	n = ts_usb_lst_dev(k, max);
	for ( i = 0; i < n && i < max; i++ ) {
		knl_ua_put(&list[i], &k[i], sizeof(*k));
	}
	if ( k != NULL ) Kfree(k);
	return n;
}

/* us_get_dsc: a descriptor of any device, read over its control endpoint */
LOCAL INT svc_us_get_dsc( INT dev, UINT type, UINT index, void *buf, INT len )
{
	UB	*k;
	INT	n;

	if ( len <= 0 || len > 4096 ) return E_PAR;
	if ( !user_ok(buf, len, TRUE) ) return E_MACV;
	k = (UB *)Kmalloc((SZ)len);
	if ( k == NULL ) return E_NOMEM;
	n = ts_usb_get_desc(dev, type, index, k, len);
	if ( n > 0 ) knl_ua_put(buf, k, n);
	Kfree(k);
	return n;
}

/* us_opn_dev: a device no class driver took, opened; the caller's number for it */
LOCAL INT svc_us_opn_dev( INT dev )
{
	T_CFLG	cf;
	USBDEV	*d;
	ID	pid = ts_get_pid(), flg;
	INT	i, h = 0;
	ER	er = E_OK;

	if ( pid <= 0 ) return E_CTX;
	knl_memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WMUL;
	flg = tk_cre_flg(&cf);
	if ( flg < E_OK ) return flg;
	tk_loc_mtx(us_mtx, TMO_FEVR);
	knl_usb_lock();
	d = knl_usb_dev_of(dev);
	if ( d == NULL ) {
		er = E_NOEXS;
	} else if ( d->role != USB_ROLE_NONE || d->user != NULL ) {
		er = E_BUSY;				/* a class driver's, or another process's */
	} else {
		for ( i = 0; i < US_OPEN_MAX && h == 0; i++ ) {
			if ( us_open[i].pid == 0 ) {
				USOPEN	*o = &us_open[i];
				INT	k;

				for ( k = 0; k < US_IN_MAX; k++ ) {
					UB	*ring = o->in[k].ring;

					knl_memset(&o->in[k], 0, sizeof(o->in[k]));
					o->in[k].ring = ring;
				}
				knl_memset(o->alt, 0, sizeof(o->alt));
				o->pid = pid;
				o->handle = dev;
				o->dev = d;
				o->flg = flg;
				d->user = o;
				h = i + 1;
			}
		}
		if ( h == 0 ) er = E_LIMIT;
	}
	knl_usb_unlock();
	tk_unl_mtx(us_mtx);
	if ( er < E_OK ) {
		tk_del_flg(flg);
		return er;
	}
	return h;
}

/* The entry closed: its streams, its hold on the device */
LOCAL void close_entry( USOPEN *o )
{
	ID	flg;

	tk_loc_mtx(us_mtx, TMO_FEVR);
	knl_usb_lock();
	close_streams(o);
	if ( o->dev != NULL ) {
		o->dev->user = NULL;
	}
	o->dev = NULL;
	flg = o->flg;
	o->flg = 0;
	o->pid = 0;
	knl_usb_unlock();
	tk_unl_mtx(us_mtx);
	if ( flg > 0 ) tk_del_flg(flg);
}

/* us_cls_dev */
LOCAL ER svc_us_cls_dev( INT h )
{
	USOPEN	*o = open_of(h);

	if ( o == NULL ) return E_ID;
	close_entry(o);
	return E_OK;
}

/* us_ctl: a request on the control endpoint; the bytes it moved */
LOCAL INT svc_us_ctl( INT h, CONST UB *setup, void *data, INT len, TMO tmo )
{
	USOPEN	*o = open_of(h);
	UB	s[8];
	UB	*k = NULL;
	INT	actual = 0;
	ER	er;

	if ( o == NULL ) return E_ID;
	if ( len < 0 || len > 4096 || tmo < TMO_FEVR ) return E_PAR;
	if ( !user_ok(setup, 8, FALSE) ) return E_MACV;
	knl_ua_get(s, setup, 8);
	PUT16(s + 6, len);
	if ( len > 0 ) {
		if ( !user_ok(data, len, ( s[0] & USB_RT_IN ) != 0) ) return E_MACV;
		k = (UB *)Kmalloc((SZ)len);
		if ( k == NULL ) return E_NOMEM;
		if ( ( s[0] & USB_RT_IN ) == 0 ) knl_ua_get(k, data, len);
	}
	if ( tmo < 0 || tmo > US_TMO_MAX ) tmo = US_TMO_MAX;
	knl_usb_lock();
	er = ( o->dev == NULL ) ? E_NOEXS : o->dev->hc->ops->control(o->dev->hc, o->dev, s, k, &actual, tmo);
	knl_usb_unlock();
	if ( er >= E_OK && ( s[0] & USB_RT_IN ) != 0 && actual > 0 ) {
		knl_ua_put(data, k, actual);
	}
	if ( k != NULL ) Kfree(k);
	return ( er < E_OK ) ? (INT)er : actual;
}

/* us_set_alt: an interface's alternate setting picked (SET_INTERFACE) */
LOCAL ER svc_us_set_alt( INT h, INT ifno, INT alt )
{
	USOPEN	*o = open_of(h);
	ER	er;

	if ( o == NULL ) return E_ID;
	if ( ifno < 0 || ifno >= US_IFS || alt < 0 || alt > 255 ) return E_PAR;
	knl_usb_lock();
	if ( o->dev == NULL ) {
		er = E_NOEXS;
	} else {
		close_streams(o);
		er = knl_usb_set_interface(o->dev, ifno, alt);
		if ( er >= E_OK ) o->alt[ifno] = (UB)alt;
	}
	knl_usb_unlock();
	return er;
}

/* us_clr_hlt: an endpoint's halt cleared (CLEAR_FEATURE(ENDPOINT_HALT)) */
LOCAL ER svc_us_clr_hlt( INT h, UINT ep )
{
	USOPEN	*o = open_of(h);
	ER	er;

	if ( o == NULL ) return E_ID;
	knl_usb_lock();
	if ( o->dev == NULL ) {
		er = E_NOEXS;
	} else {
		close_streams(o);
		er = knl_usb_clear_halt(o->dev, (UB)ep);
	}
	knl_usb_unlock();
	return er;
}

/* A piece of an IN endpoint taken, up to len bytes into k; its length, -1 none. With the lock held */
LOCAL INT take_piece( USIN *e, UB *k, INT len )
{
	INT	n, i, got;

	if ( e->pieces == 0 ) return -1;
	n = e->ring[e->head] | ( e->ring[( e->head + 1 ) % US_RING] << 8 );
	e->head = ( e->head + 2 ) % US_RING;
	got = ( n < len ) ? n : len;
	for ( i = 0; i < n; i++ ) {
		if ( i < got ) k[i] = e->ring[e->head];
		e->head = ( e->head + 1 ) % US_RING;
	}
	e->used -= n + 2;
	e->pieces--;
	return got;
}

/* A transfer IN: the oldest piece the endpoint's stream kept, waiting up to tmo for one */
LOCAL INT xfer_in( USOPEN *o, UB addr, UB *k, INT len, TMO tmo )
{
	USBEP	ep;
	USIN	*e = NULL;
	INT	i, slot = -1, got = -1;
	UINT	ptn;
	ER	er = E_OK;

	knl_usb_lock();
	if ( o->dev == NULL ) {
		knl_usb_unlock();
		return E_NOEXS;
	}
	for ( i = 0; i < US_IN_MAX; i++ ) {
		if ( o->in[i].addr == addr && o->in[i].pipe != NULL ) e = &o->in[i];
		if ( o->in[i].pipe == NULL && slot < 0 ) slot = i;
	}
	if ( e == NULL ) {
		if ( slot < 0 ) {
			er = E_LIMIT;
		} else if ( !find_ep(o, addr, &ep) ) {
			er = E_NOEXS;
		} else {
			e = &o->in[slot];
			if ( e->ring == NULL ) e->ring = (UB *)Kmalloc(US_RING);
			if ( e->ring == NULL ) {
				er = E_NOMEM;
			} else {
				INT	x = ( ep.type == USB_EP_INTR ) ? ep.size
					  : ( ep.type == USB_EP_ISOC ) ? ep.esit
					  : ( ( len + ep.size - 1 ) / ep.size ) * ep.size;

				if ( x <= 0 ) x = ep.size;
				if ( x > USB_STREAM_XFER_MAX ) x = USB_STREAM_XFER_MAX / ep.size * ep.size;
				e->addr = addr;
				e->ep = ep;
				e->head = e->tail = e->used = e->pieces = 0;
				e->failed = FALSE;
				us_ctx[o - us_open][slot].o = o;
				us_ctx[o - us_open][slot].k = slot;
				er = o->dev->hc->ops->stream_open(o->dev->hc, o->dev, &e->ep, x, in_piece,
								  &us_ctx[o - us_open][slot], &e->pipe);
				if ( er < E_OK ) {
					e->pipe = NULL;
					e->addr = 0;
				} else {
					knl_usb_kick();
				}
			}
		}
	}
	if ( er >= E_OK ) {
		(void)tk_clr_flg(o->flg, ~( 1U << ( e - o->in ) ));
		got = take_piece(e, k, len);
	}
	knl_usb_unlock();
	if ( er < E_OK ) return er;
	if ( got >= 0 ) return got;

	/* nothing yet: wait for a piece, outside the lock */
	for ( ;; ) {
		er = tk_wai_flg(o->flg, ( 1U << ( e - o->in ) ) | FLG_GONE, TWF_ORW | TWF_BITCLR, &ptn, tmo);
		if ( er < E_OK ) return er;
		knl_usb_lock();
		if ( o->dev == NULL ) {
			knl_usb_unlock();
			return E_NOEXS;
		}
		got = take_piece(e, k, len);
		if ( got < 0 && e->failed ) {
			e->failed = FALSE;
			knl_usb_unlock();
			return E_IO;
		}
		knl_usb_unlock();
		if ( got >= 0 ) return got;
	}
}

/* A transfer OUT on an isochronous endpoint: one packet put in its stream's ring, waiting up to tmo for room */
LOCAL INT xfer_iso_out( USOPEN *o, CONST USBEP *ep, UB *k, INT len, TMO tmo )
{
	USIN	*e = NULL;
	INT	i, slot = -1;
	UINT	ptn;
	ER	er = E_OK;

	knl_usb_lock();
	for ( i = 0; i < US_IN_MAX; i++ ) {
		if ( o->in[i].addr == ep->addr && o->in[i].pipe != NULL ) e = &o->in[i];
		if ( o->in[i].pipe == NULL && slot < 0 ) slot = i;
	}
	if ( len > ep->esit ) {
		er = E_PAR;				/* more than a service carries */
	} else if ( e == NULL ) {
		if ( slot < 0 ) {
			er = E_LIMIT;
		} else {
			e = &o->in[slot];
			if ( e->ring == NULL ) e->ring = (UB *)Kmalloc(US_RING);
			if ( e->ring == NULL ) {
				er = E_NOMEM;
			} else {
				e->addr = ep->addr;
				e->ep = *ep;
				e->head = e->tail = e->used = e->pieces = 0;
				e->failed = FALSE;
				e->out = TRUE;
				us_ctx[o - us_open][slot].o = o;
				us_ctx[o - us_open][slot].k = slot;
				er = o->dev->hc->ops->stream_open_out(o->dev->hc, o->dev, &e->ep, out_fill,
								      &us_ctx[o - us_open][slot], &e->pipe);
				if ( er < E_OK ) {
					e->pipe = NULL;
					e->addr = 0;
					e->out = FALSE;
				}
			}
		}
	}
	for ( ;; ) {
		if ( er < E_OK ) {
			knl_usb_unlock();
			return er;
		}
		if ( e->used + len + 2 <= US_RING ) {
			INT	n = len, at = 0;

			e->ring[e->tail] = (UB)len;
			e->tail = ( e->tail + 1 ) % US_RING;
			e->ring[e->tail] = (UB)( len >> 8 );
			e->tail = ( e->tail + 1 ) % US_RING;
			while ( n > 0 ) {
				INT	c = US_RING - e->tail;

				if ( c > n ) c = n;
				knl_memcpy(e->ring + e->tail, k + at, c);
				e->tail = ( e->tail + c ) % US_RING;
				at += c;
				n -= c;
			}
			e->used += len + 2;
			e->pieces++;
			knl_usb_unlock();
			knl_usb_kick();
			return len;
		}
		(void)tk_clr_flg(o->flg, ~( 1U << ( e - o->in ) ));
		knl_usb_unlock();
		er = tk_wai_flg(o->flg, ( 1U << ( e - o->in ) ) | FLG_GONE, TWF_ORW | TWF_BITCLR, &ptn, tmo);
		knl_usb_lock();
		if ( er >= E_OK && o->dev == NULL ) er = E_NOEXS;
		if ( er >= E_OK && e->pipe == NULL ) er = E_OBJ;	/* the stream was closed meanwhile */
	}
}

/* us_xfer: a transfer on a bulk, interrupt or isochronous endpoint; the bytes it moved */
LOCAL INT svc_us_xfer( INT h, UINT ep, void *data, INT len, TMO tmo )
{
	USOPEN	*o = open_of(h);
	BOOL	in = ( ( ep & 0x80 ) != 0 );
	USBEP	e;
	UB	*k;
	INT	n, actual = 0;
	ER	er;

	if ( o == NULL ) return E_ID;
	if ( len < 0 || len > US_XFER_MAX || tmo < TMO_FEVR || ( ep & 0x0f ) == 0 ) return E_PAR;
	if ( len > 0 && !user_ok(data, len, in) ) return E_MACV;
	k = (UB *)Kmalloc(len > 0 ? (SZ)len : 1);
	if ( k == NULL ) return E_NOMEM;
	if ( in ) {
		n = xfer_in(o, (UB)ep, k, len, tmo);
		if ( n > 0 ) knl_ua_put(data, k, n);
		Kfree(k);
		return n;
	}
	if ( len > 0 ) knl_ua_get(k, data, len);
	knl_usb_lock();
	if ( o->dev != NULL && find_ep(o, (UB)ep, &e) && e.type == USB_EP_ISOC ) {
		knl_usb_unlock();
		n = xfer_iso_out(o, &e, k, len, tmo);
		Kfree(k);
		return n;
	}
	if ( tmo < 0 || tmo > US_TMO_MAX ) tmo = US_TMO_MAX;
	if ( o->dev == NULL ) {
		er = E_NOEXS;
	} else if ( !find_ep(o, (UB)ep, &e) || ( e.type != USB_EP_BULK && e.type != USB_EP_INTR ) ) {
		er = E_NOEXS;
	} else {
		er = o->dev->hc->ops->bulk(o->dev->hc, o->dev, &e, k, len, &actual, tmo);
	}
	knl_usb_unlock();
	Kfree(k);
	return ( er < E_OK ) ? (INT)er : actual;
}

/* us_rst_dev: the device reset where it is plugged in, and addressed and configured again */
LOCAL ER svc_us_rst_dev( INT h )
{
	USOPEN	*o = open_of(h);
	ER	er;

	if ( o == NULL ) return E_ID;
	knl_usb_lock();
	if ( o->dev == NULL ) {
		er = E_NOEXS;
	} else {
		close_streams(o);
		knl_memset(o->alt, 0, sizeof(o->alt));
		er = knl_usb_reset_device(o->dev);
	}
	knl_usb_unlock();
	return er;
}

#define F(fn)	((FP)(fn))

EXPORT const FP knl_us_svc_tbl[TSN_US_MAX + 1] = {
	[TSN_NUMBER(TSN_US_LST_DEV)]	= F(svc_us_lst_dev),
	[TSN_NUMBER(TSN_US_GET_DSC)]	= F(svc_us_get_dsc),
	[TSN_NUMBER(TSN_US_OPN_DEV)]	= F(svc_us_opn_dev),
	[TSN_NUMBER(TSN_US_CLS_DEV)]	= F(svc_us_cls_dev),
	[TSN_NUMBER(TSN_US_CTL)]	= F(svc_us_ctl),
	[TSN_NUMBER(TSN_US_SET_ALT)]	= F(svc_us_set_alt),
	[TSN_NUMBER(TSN_US_CLR_HLT)]	= F(svc_us_clr_hlt),
	[TSN_NUMBER(TSN_US_XFER)]	= F(svc_us_xfer),
	[TSN_NUMBER(TSN_US_RST_DEV)]	= F(svc_us_rst_dev),
};

/* ---------------------------------------------------------------- from the manager and the processes */

EXPORT ER knl_uusb_init( void )
{
	T_CMTX	cm;

	knl_memset(&cm, 0, sizeof(cm));
	cm.mtxatr = TA_INHERIT;
	us_mtx = tk_cre_mtx(&cm);
	return ( us_mtx > 0 ) ? E_OK : us_mtx;
}

/* The device is gone, with the manager's lock held: its streams close, its waiters are woken */
EXPORT void knl_uusb_detach( USBDEV *dev )
{
	USOPEN	*o = (USOPEN *)dev->user;

	if ( o == NULL ) {
		return;
	}
	close_streams(o);
	o->dev = NULL;
	dev->user = NULL;
	if ( o->flg > 0 ) tk_set_flg(o->flg, FLG_GONE);
}

/* A program's stream is open: the manager serves the controllers often */
EXPORT BOOL knl_uusb_active( void )
{
	INT	i, k;

	for ( i = 0; i < US_OPEN_MAX; i++ ) {
		for ( k = 0; k < US_IN_MAX && us_open[i].pid != 0; k++ ) {
			if ( us_open[i].in[k].pipe != NULL ) return TRUE;
		}
	}
	return FALSE;
}

/* The devices a process opened, closed as it ends (proc.c) */
EXPORT void knl_us_prc_end( ID pid )
{
	INT	i;

	if ( pid <= 0 ) return;
	for ( i = 0; i < US_OPEN_MAX; i++ ) {
		if ( us_open[i].pid == pid ) {
			close_entry(&us_open[i]);
		}
	}
}
