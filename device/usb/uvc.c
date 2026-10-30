/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uvc.c
 *	USB video class cameras, as the cameras of the camera device "cama"
 *	(design 10.16).
 *
 *	The VideoControl interface (class 14, subclass 1) gives bcdUVC; the
 *	first VideoStreaming interface (subclass 2) gives the formats
 *	(uncompressed YUY2, NV12 and UYVY, and MJPEG), their frame sizes and
 *	intervals, and the isochronous alternate settings with their
 *	bandwidth, or one bulk endpoint.
 *
 *	A mode is agreed with VS_PROBE SET_CUR and GET_CUR, then VS_COMMIT;
 *	the alternate setting taken is the smallest whose bytes per service
 *	interval cover the dwMaxPayloadTransferSize agreed.
 *
 *	A payload is a UVC header (bHeaderLength, bmHeaderInfo with FID, EOF,
 *	PTS and ERR) and the picture's bytes. On an isochronous endpoint each
 *	transaction is one payload; on a bulk endpoint a payload is one bulk
 *	transfer of up to dwMaxPayloadTransferSize bytes, ended by a short
 *	packet, and may take several of the controller's transfers. The
 *	payloads are put together in the camera's own buffer; an EOF or a FID
 *	that changes ends the frame, which goes to the camera device
 *	(knl_cam_frame) when its size is right for the format.
 *
 *	The manager calls knl_uvc_attach and knl_uvc_detach with its lock
 *	held; the camera device is told of a camera that came or went only
 *	afterwards, by knl_uvc_notify, since the device's requests hold its
 *	own lock while they take the manager's. A camera's slot is not given
 *	to another one until the device has let it go.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/cam.h>
#include "usbdev.h"

#define CC_VIDEO		0x0e
#define SC_VIDEOCONTROL		1
#define SC_VIDEOSTREAMING	2

#define VC_HEADER		0x01
#define VS_FORMAT_UNCOMPRESSED	0x04
#define VS_FRAME_UNCOMPRESSED	0x05
#define VS_FORMAT_MJPEG		0x06
#define VS_FRAME_MJPEG		0x07

#define UVC_SET_CUR		0x01
#define UVC_GET_CUR		0x81
#define VS_PROBE_CONTROL	0x01
#define VS_COMMIT_CONTROL	0x02

#define PH_FID			0x01
#define PH_EOF			0x02
#define PH_PTS			0x04
#define PH_ERR			0x40

#define UVC_ALTS		16
#define FMT_UNKNOWN		0xff	/* a format not taken */
#define FRAME_MAX		( 8 * 1024 * 1024 )

typedef struct {
	UB	alt;
	USBEP	ep;		/* isochronous IN; ep.esit is the bytes per service interval */
} VSALT;

typedef struct {
	BOOL		used;		/* a device is here */
	BOOL		listed;		/* the camera device has it */
	USBDEV		*dev;		/* NULL once it is gone */
	T_CAMSRC	src;
	INT		vc_if, vs_if;
	UH		bcd;
	INT		probe_len;
	T_CAMMODE	modes[CAM_MAXMODES];
	INT		nmodes;
	VSALT		alts[UVC_ALTS];
	INT		nalts;
	BOOL		bulk;		/* streams over a bulk endpoint */
	USBEP		bulk_ep;
	UB		bulk_alt;

	INT		curmode;	/* -1 none agreed */
	T_CAMSETMODE	cur;
	UB		probe[48];

	BOOL		streaming;
	USBPIPE		*pipe;
	INT		xfer;		/* bulk: bytes a controller transfer */
	INT		pl_bytes;	/* bulk: bytes of the payload so far */
	UB		pl_info;	/* bulk: bmHeaderInfo of the payload */

	/* the frame being put together */
	UB		*asmb;
	INT		asm_cap;
	INT		fill;
	BOOL		frame_err;
	BOOL		have_fid;
	UB		fid;
	UW		pts;
} UVC;

/*
 * The cameras attached. Each is allocated on its own and never freed:
 * the camera device holds its T_CAMSRC, and dev->video points at it.
 * A larger table is made whole before it is put in place.
 */
#define UVC_FIRST	4

typedef struct {
	INT	cap;
	UVC	*e[];
} UVCTBL;

LOCAL UVCTBL	*uvc_tbl = NULL;

#define UVC_N		( ( uvc_tbl != NULL ) ? uvc_tbl->cap : 0 )
#define UVC_AT(i)	( uvc_tbl->e[(i)] )
LOCAL BOOL	uvc_changed = FALSE;

/* ---------------------------------------------------------------- descriptors */

LOCAL CONST UB guid_tail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
				 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };

LOCAL BOOL same( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) {
			return FALSE;
		}
	}

	return TRUE;
}

LOCAL UB guid_format( CONST UB *g )
{
	if ( !same(g + 4, guid_tail, 12) ) {
		return FMT_UNKNOWN;
	}
	if ( same(g, (CONST UB *)"YUY2", 4) ) return CAMFMT_YUY2;
	if ( same(g, (CONST UB *)"NV12", 4) ) return CAMFMT_NV12;
	if ( same(g, (CONST UB *)"UYVY", 4) ) return CAMFMT_UYVY;

	return FMT_UNKNOWN;
}

LOCAL void add_frame( UVC *u, CONST UB *p, UB fmt_idx, UB fmt, INT bpp )
{
	T_CAMMODE	*m;
	INT		i, n;

	if ( u->nmodes >= CAM_MAXMODES || p[0] < 26 ) {
		return;
	}
	m = &u->modes[u->nmodes];
	knl_memset(m, 0, sizeof(*m));
	m->fmtIndex = fmt_idx;
	m->frmIndex = p[3];
	m->format = fmt;
	m->width = GET16(p + 5);
	m->height = GET16(p + 7);
	m->maxBytes = GET32(p + 17);
	m->defInterval = GET32(p + 21);
	n = p[25];
	if ( n == 0 ) {				/* a continuous range */
		m->nintv = 0;
		if ( p[0] >= 38 ) {
			m->interval[0] = GET32(p + 26);
			m->interval[1] = GET32(p + 30);
			m->interval[2] = GET32(p + 34);
		}
	} else {
		if ( n > CAM_MAXINTV ) {
			n = CAM_MAXINTV;
		}
		for ( i = 0; i < n && 26 + 4 * i + 4 <= p[0]; i++ ) {
			m->interval[i] = GET32(p + 26 + 4 * i);
		}
		m->nintv = (UB)i;
	}
	if ( m->maxBytes == 0 && fmt != CAMFMT_MJPEG ) {
		m->maxBytes = (UW)m->width * m->height * (UW)( bpp ? bpp : 16 ) / 8;
	}
	if ( m->maxBytes == 0 ) {
		m->maxBytes = (UW)m->width * m->height * 2;
	}
	u->nmodes++;
}

LOCAL ER parse_config( UVC *u, USBDEV *dev )
{
	UB	*p, *end = dev->cfg + dev->cfglen;
	INT	ifno = -1, alt = 0, cls = -1, sub = -1, bpp = 0;
	UB	fmt_idx = 0, fmt = CAMFMT_NONE;

	u->vc_if = u->vs_if = -1;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		switch ( p[1] ) {
		case USB_DT_INTERFACE:
			if ( p[0] < 9 ) {
				break;
			}
			ifno = p[2];
			alt = p[3];
			cls = p[5];
			sub = p[6];
			if ( cls == CC_VIDEO && sub == SC_VIDEOCONTROL && u->vc_if < 0 ) {
				u->vc_if = ifno;
			}
			if ( cls == CC_VIDEO && sub == SC_VIDEOSTREAMING && u->vs_if < 0 ) {
				u->vs_if = ifno;
			}
			break;

		case USB_DT_CS_INTERFACE:
			if ( cls != CC_VIDEO || p[0] < 3 ) {
				break;
			}
			if ( sub == SC_VIDEOCONTROL && ifno == u->vc_if ) {
				if ( p[2] == VC_HEADER && p[0] >= 5 ) {
					u->bcd = GET16(p + 3);
				}
				break;
			}
			if ( sub != SC_VIDEOSTREAMING || ifno != u->vs_if ) {
				break;
			}
			switch ( p[2] ) {
			case VS_FORMAT_UNCOMPRESSED:
				if ( p[0] < 27 ) {
					fmt = CAMFMT_NONE;
					break;
				}
				fmt_idx = p[3];
				fmt = guid_format(p + 5);
				bpp = p[21];
				break;
			case VS_FORMAT_MJPEG:
				fmt_idx = p[3];
				fmt = CAMFMT_MJPEG;
				bpp = 0;
				break;
			case VS_FRAME_UNCOMPRESSED:
			case VS_FRAME_MJPEG:
				if ( fmt != CAMFMT_NONE && fmt != FMT_UNKNOWN ) {
					add_frame(u, p, fmt_idx, fmt, bpp);
				}
				break;
			default:
				if ( p[2] >= 0x10 ) {
					fmt = CAMFMT_NONE;	/* frame based, H.264 and the like */
				}
				break;
			}
			break;

		case USB_DT_ENDPOINT:
			if ( p[0] < 7 || ifno != u->vs_if || cls != CC_VIDEO
			  || ( p[2] & 0x80 ) == 0 ) {
				break;
			}
			if ( ( p[3] & 3 ) == USB_EP_ISOC && alt > 0 && u->nalts < UVC_ALTS ) {
				VSALT	*a = &u->alts[u->nalts++];

				a->alt = (UB)alt;
				knl_usb_ep_from_desc(&a->ep, dev, p, knl_usb_next_desc(p, end));
			} else if ( ( p[3] & 3 ) == USB_EP_BULK && !u->bulk ) {
				u->bulk = TRUE;
				u->bulk_alt = (UB)alt;
				knl_usb_ep_from_desc(&u->bulk_ep, dev, p,
						     knl_usb_next_desc(p, end));
			}
			break;
		}
	}
	if ( u->vc_if < 0 || u->vs_if < 0 ) {
		return E_NOSPT;
	}
	u->probe_len = ( u->bcd >= 0x0150 ) ? 48 : ( u->bcd >= 0x0110 ) ? 34 : 26;

	return E_OK;
}

EXPORT BOOL knl_uvc_match( USBDEV *dev )
{
	UB	*p, *end;

	if ( dev->cfg == NULL ) {
		return FALSE;
	}
	end = dev->cfg + dev->cfglen;
	for ( p = dev->cfg; p != NULL; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[5] == CC_VIDEO
		  && p[6] == SC_VIDEOSTREAMING ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* ---------------------------------------------------------------- agreeing a mode */

LOCAL ER vs_ctl( UVC *u, UB req, UB cs, UB *buf, INT len )
{
	INT	n = 0;
	UB	rt = ( req & 0x80 ) ? ( USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE )
				    : ( USB_RT_CLASS | USB_RT_INTERFACE );

	return knl_usb_control(u->dev, rt, req, (UH)( cs << 8 ), (UH)u->vs_if,
			       buf, (UH)len, &n);
}

/* VS_PROBE, then VS_COMMIT; the shorter probe structures of older cameras are tried too */
LOCAL ER probe_commit( UVC *u, CONST T_CAMMODE *m, UW interval )
{
	UB	*pb = u->probe;
	ER	er = E_NOSPT;
	INT	lens[3], i;

	lens[0] = u->probe_len;
	lens[1] = 34;
	lens[2] = 26;
	for ( i = 0; i < 3; i++ ) {
		if ( i > 0 && lens[i] >= u->probe_len ) {
			continue;
		}
		knl_memset(pb, 0, sizeof(u->probe));
		PUT16(pb + 0, 0x0001);			/* bmHint: keep dwFrameInterval */
		pb[2] = m->fmtIndex;
		pb[3] = m->frmIndex;
		PUT32(pb + 4, interval);
		if ( ( er = vs_ctl(u, UVC_SET_CUR, VS_PROBE_CONTROL, pb, lens[i]) ) < E_OK ) {
			continue;
		}
		if ( ( er = vs_ctl(u, UVC_GET_CUR, VS_PROBE_CONTROL, pb, lens[i]) ) < E_OK ) {
			continue;
		}
		u->probe_len = lens[i];
		break;
	}
	if ( i >= 3 ) {
		USB_LOG("uvc: probe failed (%d)\n", (INT)er);
		return er;
	}
	er = vs_ctl(u, UVC_SET_CUR, VS_COMMIT_CONTROL, pb, u->probe_len);
	if ( er < E_OK ) {
		USB_LOG("uvc: commit failed (%d)\n", (INT)er);
	}

	return er;
}

LOCAL INT pick_alt( UVC *u, UW payload )
{
	INT	i, best = -1, big = -1;

	for ( i = 0; i < u->nalts; i++ ) {
		if ( big < 0 || u->alts[i].ep.esit > u->alts[big].ep.esit ) {
			big = i;
		}
		if ( (UW)u->alts[i].ep.esit >= payload
		  && ( best < 0 || u->alts[i].ep.esit < u->alts[best].ep.esit ) ) {
			best = i;
		}
	}

	return ( best >= 0 ) ? best : big;
}

/* With the manager's lock held */
LOCAL ER agree( UVC *u, INT mode, UW interval )
{
	T_CAMMODE	*m = &u->modes[mode];
	ER		er;
	INT		a;

	er = probe_commit(u, m, interval ? interval : m->defInterval);
	if ( er < E_OK ) {
		return er;
	}
	u->curmode = mode;
	u->cur.mode = mode;
	u->cur.interval = GET32(u->probe + 4);
	u->cur.maxFrame = GET32(u->probe + 18);
	u->cur.maxPayload = GET32(u->probe + 22);
	if ( u->bulk ) {
		u->cur.altSetting = u->bulk_alt;
	} else {
		a = pick_alt(u, u->cur.maxPayload);
		u->cur.altSetting = ( a >= 0 ) ? u->alts[a].alt : -1;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- putting frames together */

LOCAL void reset_frame( UVC *u )
{
	u->fill = 0;
	u->frame_err = FALSE;
	u->pts = 0;
}

/* The frame put together goes to the device when it is whole */
LOCAL void finish_frame( UVC *u )
{
	T_CAMMODE	*m = &u->modes[u->curmode];
	BOOL		ok = ( !u->frame_err && u->fill > 0 );

	if ( ok && m->format != CAMFMT_MJPEG ) {
		UW	want = ( m->format == CAMFMT_NV12 )
			       ? (UW)m->width * m->height * 3 / 2
			       : (UW)m->width * m->height * 2;

		ok = ( (UW)u->fill == want );
	}
	if ( ok && m->format == CAMFMT_MJPEG
	  && ( u->fill < 4 || u->asmb[0] != 0xff || u->asmb[1] != 0xd8 ) ) {
		ok = FALSE;
	}
	if ( ok ) {
		knl_cam_frame(&u->src, u->asmb, u->fill, u->pts);
	} else if ( u->fill > 0 ) {
		knl_cam_dropped(&u->src);
	}
	reset_frame(u);
}

LOCAL void copy_in( UVC *u, DMABUF *b, INT off, INT len )
{
	if ( len <= 0 ) {
		return;
	}
	if ( u->fill + len > u->asm_cap ) {
		u->frame_err = TRUE;			/* too large: the frame is dropped */
		return;
	}
	knl_dmabuf_read(b, off, u->asmb + u->fill, len);
	u->fill += len;
}

/* A payload's header: its length, or -1 when it cannot be (the frame is then bad) */
LOCAL INT payload_start( UVC *u, DMABUF *b, INT off, INT len, UB *info_out )
{
	UB	h[12];
	INT	hlen;
	UB	info;

	knl_dmabuf_read(b, off, h, ( len < 12 ) ? len : 12);
	hlen = h[0];
	info = h[1];
	if ( hlen < 2 || hlen > len ) {
		u->frame_err = TRUE;
		return -1;
	}
	if ( u->have_fid && ( info & PH_FID ) != u->fid && u->fill > 0 ) {
		finish_frame(u);			/* FID changed without an EOF */
	}
	u->fid = info & PH_FID;
	u->have_fid = TRUE;
	if ( info & PH_ERR ) {
		u->frame_err = TRUE;
	}
	if ( ( info & PH_PTS ) && hlen >= 6 ) {
		u->pts = GET32(h + 2);
	}
	*info_out = info;

	return hlen;
}

/* One isochronous transaction: one whole payload */
LOCAL void iso_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	UVC	*u = (UVC *)ctx;
	INT	hlen;
	UB	info = 0;

	if ( !u->streaming || u->curmode < 0 ) {
		return;
	}
	if ( err != 0 ) {
		u->frame_err = TRUE;
		return;
	}
	if ( len < 2 ) {
		return;
	}
	hlen = payload_start(u, b, off, len, &info);
	if ( hlen < 0 ) {
		return;
	}
	copy_in(u, b, off + hlen, len - hlen);
	if ( info & PH_EOF ) {
		finish_frame(u);
	}
}

/*
 * One bulk transfer: the start, middle or end of a payload. A payload ends
 * with a transfer shorter than u->xfer (a short packet) or when
 * dwMaxPayloadTransferSize bytes have come.
 */
LOCAL void bulk_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	UVC	*u = (UVC *)ctx;

	if ( !u->streaming || u->curmode < 0 ) {
		return;
	}
	if ( err != 0 ) {
		u->frame_err = TRUE;
		u->pl_bytes = 0;
		return;
	}
	if ( u->pl_bytes == 0 ) {
		INT	hlen;

		if ( len == 0 ) {
			return;
		}
		hlen = payload_start(u, b, off, len, &u->pl_info);
		if ( hlen < 0 ) {
			u->pl_info = 0;			/* not a header: the payload is skipped */
			hlen = len;
		}
		copy_in(u, b, off + hlen, len - hlen);
	} else {
		copy_in(u, b, off, len);
	}
	u->pl_bytes += len;
	if ( len < u->xfer
	  || ( u->cur.maxPayload > 0 && (UW)u->pl_bytes >= u->cur.maxPayload ) ) {
		if ( u->pl_info & PH_EOF ) {
			finish_frame(u);
		}
		u->pl_bytes = 0;
		u->pl_info = 0;
	}
}

/* ---------------------------------------------------------------- streaming */

/* Bytes a bulk transfer: a whole payload when it fits, else as many packets as one takes */
LOCAL INT bulk_xfer_bytes( UVC *u )
{
	INT	mps = ( u->bulk_ep.size > 0 ) ? u->bulk_ep.size : 512;
	INT	x = (INT)u->cur.maxPayload;

	if ( x <= 0 || x > USB_STREAM_XFER_MAX ) {
		x = USB_STREAM_XFER_MAX;
	}
	x = ( x + mps - 1 ) / mps * mps;
	if ( x > USB_STREAM_XFER_MAX ) {
		x = USB_STREAM_XFER_MAX / mps * mps;
	}

	return x;
}

/* The largest frame of the mode agreed */
LOCAL INT frame_bytes( UVC *u )
{
	T_CAMMODE	*m = &u->modes[u->curmode];
	INT		bytes = (INT)m->maxBytes;

	if ( (INT)u->cur.maxFrame > bytes ) {
		bytes = (INT)u->cur.maxFrame;
	}
	if ( m->format != CAMFMT_MJPEG && bytes < (INT)m->width * m->height * 2 ) {
		bytes = (INT)m->width * m->height * 2;
	}
	if ( bytes <= 0 || bytes > FRAME_MAX ) {
		bytes = FRAME_MAX;
	}

	return bytes;
}

/*
 * The device stops sending: the alternate setting without bandwidth for an
 * isochronous interface; for a bulk one, which stays on setting 0,
 * CLEAR_FEATURE(ENDPOINT_HALT) of its endpoint.
 */
LOCAL void quiesce( UVC *u )
{
	if ( u->bulk ) {
		knl_usb_clear_halt(u->dev, u->bulk_ep.addr);
	} else {
		knl_usb_quiesce(u->dev, u->vs_if);
	}
}

LOCAL void pipe_close( UVC *u )
{
	u->streaming = FALSE;
	if ( u->pipe != NULL && u->dev != NULL ) {
		u->dev->hc->ops->stream_close(u->dev->hc, u->pipe);
	}
	u->pipe = NULL;
}

/* ---------------------------------------------------------------- what the camera device calls */

LOCAL ER uvc_set_mode( T_CAMSRC *s, T_CAMSETMODE *m )
{
	UVC	*u = (UVC *)s->priv;
	ER	er;

	knl_usb_lock();
	if ( u->dev == NULL ) {
		knl_usb_unlock();
		return E_NOEXS;
	}
	if ( u->streaming ) {
		pipe_close(u);
		quiesce(u);
	}
	er = agree(u, m->mode, m->interval);
	if ( er >= E_OK ) {
		*m = u->cur;
		USB_LOG("uvc: mode %d: interval %d, frame %d, payload %d, setting %d\n",
			u->curmode, (INT)u->cur.interval, (INT)u->cur.maxFrame,
			(INT)u->cur.maxPayload, u->cur.altSetting);
	}
	knl_usb_unlock();

	return ( er < E_OK ) ? E_IO : E_OK;
}

LOCAL ER uvc_start( T_CAMSRC *s )
{
	UVC	*u = (UVC *)s->priv;
	CONST USBEP *ep;
	VSALT	*a = NULL;
	USBHC	*hc;
	INT	bytes, i;
	ER	er;

	knl_usb_lock();
	if ( u->dev == NULL ) {
		knl_usb_unlock();
		return E_NOEXS;
	}
	if ( u->streaming ) {
		knl_usb_unlock();
		return E_OK;
	}
	if ( u->curmode < 0 ) {
		knl_usb_unlock();
		return E_OBJ;
	}
	bytes = frame_bytes(u);
	knl_usb_unlock();

	/* the buffer frames are put together in: made without the lock, while it is not in use */
	if ( u->asm_cap < bytes ) {
		if ( u->asmb != NULL ) {
			Kfree(u->asmb);
		}
		u->asm_cap = 0;
		u->asmb = (UB *)Kmalloc((SZ)bytes);
		if ( u->asmb == NULL ) {
			return E_NOMEM;
		}
		u->asm_cap = bytes;
	}

	knl_usb_lock();
	if ( u->dev == NULL ) {
		knl_usb_unlock();
		return E_NOEXS;
	}
	if ( u->bulk ) {
		/* a bulk camera sends from VS_COMMIT on, and a stopped one wants a new commit */
		er = agree(u, u->curmode, u->cur.interval);
		if ( er < E_OK ) {
			knl_usb_unlock();
			return E_IO;
		}
		ep = &u->bulk_ep;
	} else {
		for ( i = 0; i < u->nalts; i++ ) {
			if ( u->alts[i].alt == u->cur.altSetting ) {
				a = &u->alts[i];
				break;
			}
		}
		if ( a == NULL ) {
			knl_usb_unlock();
			return E_NOSPT;
		}
		ep = &a->ep;
		er = knl_usb_set_interface(u->dev, u->vs_if, a->alt);
		if ( er < E_OK ) {
			USB_LOG("uvc: SET_INTERFACE %d/%d failed (%d)\n", u->vs_if, a->alt, (INT)er);
			knl_usb_unlock();
			return E_IO;
		}
	}
	reset_frame(u);
	u->have_fid = FALSE;
	u->pl_bytes = 0;
	u->pl_info = 0;
	u->streaming = TRUE;
	hc = u->dev->hc;
	if ( u->bulk ) {
		u->xfer = bulk_xfer_bytes(u);
		er = hc->ops->stream_open(hc, u->dev, ep, u->xfer, bulk_xfer, u, &u->pipe);
	} else {
		u->xfer = ep->esit;
		er = hc->ops->stream_open(hc, u->dev, ep, ep->esit, iso_xfer, u, &u->pipe);
	}
	if ( er < E_OK ) {
		USB_LOG("uvc: stream open failed (%d)\n", (INT)er);
		u->streaming = FALSE;
		u->pipe = NULL;
		if ( !u->bulk ) {
			knl_usb_set_interface(u->dev, u->vs_if, 0);
		}
		knl_usb_unlock();
		return E_IO;
	}
	USB_LOG("uvc: cama%d streaming mode %d, %s ep %02x, %d bytes a %s\n", s->unit,
		u->curmode, u->bulk ? "bulk" : "iso", ep->addr, u->xfer,
		u->bulk ? "transfer" : "service");
	knl_usb_unlock();
	knl_usb_kick();

	return E_OK;
}

LOCAL void uvc_stop( T_CAMSRC *s )
{
	UVC	*u = (UVC *)s->priv;

	knl_usb_lock();
	if ( u->streaming ) {
		pipe_close(u);
		if ( u->dev != NULL ) {
			quiesce(u);
		}
	}
	knl_usb_unlock();
}

/* ---------------------------------------------------------------- from the manager */

LOCAL UVC *uvc_slot( void )
{
	UVCTBL	*t;
	INT	i, cap;

	for ( i = 0; i < UVC_N; i++ ) {
		if ( !UVC_AT(i)->used && !UVC_AT(i)->listed ) {
			return UVC_AT(i);
		}
	}
	cap = ( UVC_N > 0 ) ? UVC_N * 2 : UVC_FIRST;
	t = (UVCTBL *)Kcalloc(1, sizeof(UVCTBL) + sizeof(UVC *) * (SZ)cap);
	if ( t == NULL ) {
		return NULL;
	}
	for ( i = 0; i < cap; i++ ) {
		if ( i < UVC_N ) {
			t->e[i] = UVC_AT(i);
			continue;
		}
		t->e[i] = (UVC *)Kcalloc(1, sizeof(UVC));
		if ( t->e[i] == NULL ) break;
	}
	if ( i == UVC_N ) {
		Kfree(t);
		return NULL;
	}
	t->cap = i;
	__sync_synchronize();
	i = UVC_N;
	uvc_tbl = t;
	return UVC_AT(i);
}

EXPORT ER knl_uvc_attach( USBDEV *dev )
{
	UVC	*u;
	ER	er;
	INT	i;

	u = uvc_slot();
	if ( u == NULL ) {
		return E_NOMEM;
	}
	if ( u->asmb != NULL ) {
		Kfree(u->asmb);
	}
	knl_memset(u, 0, sizeof(*u));
	u->curmode = -1;
	er = parse_config(u, dev);
	if ( er < E_OK ) {
		USB_LOG("uvc: no VideoControl and VideoStreaming interfaces\n");
		return er;
	}
	if ( u->nalts > 0 ) {
		u->bulk = FALSE;			/* isochronous when it has both */
	}
	if ( u->nalts == 0 && !u->bulk ) {
		USB_LOG("uvc: no streaming endpoint\n");
		return E_NOSPT;
	}
	if ( u->nmodes == 0 ) {
		USB_LOG("uvc: no format taken (YUY2, NV12, UYVY, MJPEG)\n");
		return E_NOSPT;
	}
	u->dev = dev;
	knl_usb_set_interface(dev, u->vs_if, 0);

	u->src.info.vid = GET16(dev->ddesc + 8);
	u->src.info.pid = GET16(dev->ddesc + 10);
	u->src.info.bcdUVC = u->bcd;
	knl_memcpy(u->src.info.product, dev->product, sizeof(dev->product));
	knl_memcpy(u->src.info.manufacturer, dev->maker, sizeof(dev->maker));
	if ( u->src.info.product[0] == 0 ) {
		knl_strcpy((char *)u->src.info.product, "USB camera");
	}
	u->src.modes = u->modes;
	u->src.nmodes = u->nmodes;
	u->src.set_mode = uvc_set_mode;
	u->src.start = uvc_start;
	u->src.stop = uvc_stop;
	u->src.priv = u;
	u->src.unit = -1;
	u->used = TRUE;
	dev->camera = u;
	uvc_changed = TRUE;
	USB_LOG("usb: camera, UVC %x.%02x: %d modes, %d isochronous settings%s\n",
		u->bcd >> 8, u->bcd & 0xff, u->nmodes, u->nalts,
		u->bulk ? ", bulk" : "");
	for ( i = 0; i < u->nmodes; i++ ) {
		T_CAMMODE	*m = &u->modes[i];

		USB_LOG("uvc:  mode %d: %s %dx%d, %d bytes\n", i,
			m->format == CAMFMT_MJPEG ? "MJPEG" :
			m->format == CAMFMT_YUY2 ? "YUY2" :
			m->format == CAMFMT_NV12 ? "NV12" : "UYVY",
			m->width, m->height, (INT)m->maxBytes);
	}

	return E_OK;
}

/* The device is gone: its stream closes without a word to it */
EXPORT void knl_uvc_detach( USBDEV *dev )
{
	UVC	*u = (UVC *)dev->camera;

	if ( u == NULL || !u->used ) {
		return;
	}
	pipe_close(u);
	u->dev = NULL;
	u->used = FALSE;
	dev->camera = NULL;
	uvc_changed = TRUE;
}

/* Outside the manager's lock: the camera device takes the cameras that came and lets go of the ones gone */
EXPORT void knl_uvc_notify( void )
{
	INT	i;

	if ( !uvc_changed ) {
		return;
	}
	uvc_changed = FALSE;
	for ( i = 0; i < UVC_N; i++ ) {
		UVC	*u = UVC_AT(i);

		if ( u->listed && !u->used ) {
			knl_cam_detach(&u->src);
			u->listed = FALSE;
			if ( u->asmb != NULL ) {
				Kfree(u->asmb);
			}
			u->asmb = NULL;
			u->asm_cap = 0;
		}
		if ( u->used && !u->listed && knl_cam_attach(&u->src) >= 0 ) {
			u->listed = TRUE;
		}
	}
}

#ifdef USE_KTEST
/*
 * For the tests: a camera made from a configuration descriptor alone,
 * without a device. Its modes and settings are read as a device's would
 * be; picking a mode takes the setting pick_alt finds for the payload
 * size the test says, without talking to anything; payloads handed to
 * knl_uvc_probe_payload go through the assembly an isochronous or a bulk
 * stream's would. alts[] gets the bytes a service interval of each
 * isochronous setting; the answer is its modes, or an error.
 */
LOCAL UVC	probe_u;
LOCAL USBDEV	probe_dev;
LOCAL UB	probe_page[USB_PAGE];
LOCAL UW	probe_payload;

LOCAL ER probe_set_mode( T_CAMSRC *s, T_CAMSETMODE *m )
{
	UVC	*u = (UVC *)s->priv;
	INT	a;

	u->curmode = m->mode;
	u->cur.mode = m->mode;
	u->cur.interval = m->interval ? m->interval : u->modes[m->mode].defInterval;
	u->cur.maxFrame = u->modes[m->mode].maxBytes;
	u->cur.maxPayload = probe_payload;
	a = pick_alt(u, probe_payload);
	u->cur.altSetting = u->bulk ? u->bulk_alt : ( a >= 0 ) ? u->alts[a].alt : -1;
	*m = u->cur;

	return E_OK;
}

LOCAL ER probe_start( T_CAMSRC *s )
{
	UVC	*u = (UVC *)s->priv;
	INT	bytes = frame_bytes(u);

	if ( u->asm_cap < bytes ) {
		if ( u->asmb != NULL ) {
			Kfree(u->asmb);
		}
		u->asmb = (UB *)Kmalloc((SZ)bytes);
		u->asm_cap = ( u->asmb != NULL ) ? bytes : 0;
		if ( u->asmb == NULL ) {
			return E_NOMEM;
		}
	}
	reset_frame(u);
	u->have_fid = FALSE;
	u->pl_bytes = u->pl_info = 0;
	u->xfer = u->bulk ? bulk_xfer_bytes(u) : 0;
	u->streaming = TRUE;

	return E_OK;
}

LOCAL void probe_stop( T_CAMSRC *s )
{
	((UVC *)s->priv)->streaming = FALSE;
}

EXPORT INT knl_uvc_probe_attach( CONST UB *cfg, INT len, INT speed, UW payload,
				 INT *alts, INT maxalts, BOOL *bulk )
{
	UVC	*u = &probe_u;
	ER	er;
	INT	i;

	knl_memset(u, 0, sizeof(*u));
	knl_memset(&probe_dev, 0, sizeof(probe_dev));
	probe_dev.cfg = (UB *)cfg;
	probe_dev.cfglen = len;
	probe_dev.speed = speed;
	probe_payload = payload;
	u->curmode = -1;
	if ( !knl_uvc_match(&probe_dev) ) {
		return E_NOSPT;
	}
	er = parse_config(u, &probe_dev);
	if ( er < E_OK ) {
		return er;
	}
	if ( u->nalts > 0 ) {
		u->bulk = FALSE;
	}
	for ( i = 0; i < u->nalts && i < maxalts; i++ ) {
		alts[i] = u->alts[i].ep.esit;
	}
	*bulk = u->bulk;
	knl_strcpy((char *)u->src.info.product, "Probe camera");
	u->src.info.bcdUVC = u->bcd;
	u->src.modes = u->modes;
	u->src.nmodes = u->nmodes;
	u->src.set_mode = probe_set_mode;
	u->src.start = probe_start;
	u->src.stop = probe_stop;
	u->src.priv = u;
	u->src.unit = -1;
	er = knl_cam_attach(&u->src);
	if ( er < E_OK ) {
		return er;
	}

	return u->nmodes;
}

/* One payload, or for a bulk stream one controller transfer */
EXPORT void knl_uvc_probe_payload( CONST UB *data, INT len )
{
	DMABUF	b;

	if ( len > USB_PAGE ) {
		len = USB_PAGE;
	}
	knl_memcpy(probe_page, data, (SZ)len);
	knl_memset(&b, 0, sizeof(b));
	b.npages = 1;
	b.virt[0] = probe_page;
	if ( probe_u.bulk ) {
		bulk_xfer(&probe_u, &b, 0, len, 0);
	} else {
		iso_xfer(&probe_u, &b, 0, len, 0);
	}
}

EXPORT void knl_uvc_probe_detach( void )
{
	knl_cam_detach(&probe_u.src);
	if ( probe_u.asmb != NULL ) {
		Kfree(probe_u.asmb);
	}
	probe_u.asmb = NULL;
	probe_u.asm_cap = 0;
}
#endif /* USE_KTEST */

/* A camera streams: the manager serves the controllers often */
EXPORT BOOL knl_uvc_active( void )
{
	INT	i;

	for ( i = 0; i < UVC_N; i++ ) {
		if ( UVC_AT(i)->used && UVC_AT(i)->streaming ) {
			return TRUE;
		}
	}

	return FALSE;
}
