/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cdcnet.c (x64 PC)
 *	USB network adapters of the communications class: the Ethernet
 *	control model (CDC-ECM) and the network control model (CDC-NCM).
 *
 *	A machine without a network card of its own (the Surface Go 2)
 *	takes a USB LAN adapter. Many of them (the Realtek RTL8153 among
 *	them) come up in a configuration of their maker's, and have another
 *	of the class besides: the device's configurations are read, and the
 *	first that has an ECM or NCM function is set. Its data interface's
 *	alternate setting with the two bulk endpoints is chosen, the address
 *	read from the string the Ethernet functional descriptor names, and a
 *	stream of IN transfers started.
 *
 *	ECM carries one Ethernet frame a transfer (a frame of a whole number
 *	of packets is ended by a packet of nothing). NCM carries transfer
 *	blocks (NTB, 16 bit form): a header (NTH16), a table of the
 *	datagrams (NDP16), the frames; the frames are taken out of each
 *	block received, and each frame sent goes in a block of its own.
 *
 *	Frames received are kept in a ring until the protocol stack reads
 *	them (knl_cdcnet_recv, through net_recv of netsel.c). One adapter
 *	is served at a time.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include "../usbdev.h"

#define CDC_CLASS_COMM		0x02
#define CDC_CLASS_DATA		0x0a
#define CDC_SUB_ECM		0x06
#define CDC_SUB_NCM		0x0d

#define CDC_FD_UNION		0x06
#define CDC_FD_ETHERNET		0x0f
#define CDC_FD_NCM		0x1a

#define CDC_SET_PACKET_FILTER	0x43
#define CDC_GET_NTB_PARAMETERS	0x80
#define CDC_SET_NTB_INPUT_SIZE	0x86

#define PF_ALL_MULTICAST	0x0002
#define PF_DIRECTED		0x0004
#define PF_BROADCAST		0x0008

#define RX_SLOTS		128
#define RX_FRAME		1536
#define NCM_IN_MAX		16384
#define ECM_XFER		2048

typedef struct {
	BOOL		used;
	USBDEV		*dev;
	BOOL		ncm;
	INT		cfg_value;
	INT		ctrl_if, data_if, data_alt;
	USBEP		ep_in, ep_out;
	USBPIPE		*pipe;
	UB		mac[NET_MAC_LEN];
	UW		in_size;		/* NCM: the largest block it sends */
	UH		out_align;		/* NCM: where a datagram it is sent starts */
	UH		seq;
	UB		*txbuf;
} CDCNET;

LOCAL CDCNET		cn;
LOCAL UB		rx_ring[RX_SLOTS][RX_FRAME];
LOCAL UH		rx_len[RX_SLOTS];
LOCAL volatile UW	rx_head = 0, rx_tail = 0;
LOCAL ID		rx_sem = 0, tx_mtx = 0;
LOCAL UD		n_sent, n_recv, n_drop, n_err;

/* ---------------------------------------------------------------- descriptors */

/* The configuration's functions: an ECM or NCM control interface with its data interface */
typedef struct {
	INT	ctrl_if, data_if, data_alt;
	BOOL	ncm;
	INT	mac_str;
	CONST UB *ep_in, *ep_out;
} FOUND;

LOCAL BOOL find_function( UB *cfg, INT len, FOUND *f )
{
	UB	*p, *end = cfg + len;
	INT	cur_if = -1, cur_alt = 0, cur_cls = -1, cur_sub = -1;
	CONST UB *in = NULL, *out = NULL;

	knl_memset(f, 0, sizeof(*f));
	f->ctrl_if = f->data_if = -1;
	for ( p = cfg; p != NULL && p < end; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			cur_if = p[2];
			cur_alt = p[3];
			cur_cls = p[5];
			cur_sub = p[6];
			in = out = NULL;
			if ( f->ctrl_if < 0 && cur_cls == CDC_CLASS_COMM
			  && ( cur_sub == CDC_SUB_ECM || cur_sub == CDC_SUB_NCM ) ) {
				f->ctrl_if = cur_if;
				f->ncm = (BOOL)( cur_sub == CDC_SUB_NCM );
			}
		} else if ( p[1] == USB_DT_CS_INTERFACE && p[0] >= 3 && cur_if == f->ctrl_if && cur_cls == CDC_CLASS_COMM ) {
			if ( p[2] == CDC_FD_UNION && p[0] >= 5 ) {
				f->data_if = p[4];
			} else if ( p[2] == CDC_FD_ETHERNET && p[0] >= 13 ) {
				f->mac_str = p[3];
			}
		} else if ( p[1] == USB_DT_ENDPOINT && p[0] >= 7 && cur_cls == CDC_CLASS_DATA && cur_if == f->data_if
			 && ( p[3] & 3 ) == 2 ) {
			if ( p[2] & 0x80 ) in = p; else out = p;
			if ( in != NULL && out != NULL && f->ep_in == NULL ) {
				f->ep_in = in;
				f->ep_out = out;
				f->data_alt = cur_alt;
			}
		}
	}
	return (BOOL)( f->ctrl_if >= 0 && f->data_if >= 0 && f->ep_in != NULL );
}

LOCAL INT hexval( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* The address, as the string names it: twelve hexadecimal digits */
LOCAL ER read_mac( USBDEV *dev, INT idx, UB *mac )
{
	UB	s[64];
	INT	n = 0, i;

	if ( idx == 0 || knl_usb_get_desc(dev, USB_DT_STRING, (UB)idx, 0x0409, s, sizeof(s), &n) < E_OK || n < 2 + 24 ) {
		return E_NOEXS;
	}
	for ( i = 0; i < 6; i++ ) {
		INT	h = hexval(s[2 + i * 4]), l = hexval(s[2 + i * 4 + 2]);

		if ( h < 0 || l < 0 ) return E_OBJ;
		mac[i] = (UB)( ( h << 4 ) | l );
	}
	return E_OK;
}

/* ---------------------------------------------------------------- receiving */

LOCAL void rx_put( CONST UB *frame, INT len )
{
	UW	h = rx_head;

	if ( len < 14 || len > RX_FRAME ) {
		n_err++;
		return;
	}
	if ( h - rx_tail >= RX_SLOTS ) {
		n_drop++;
		return;
	}
	knl_memcpy(rx_ring[h % RX_SLOTS], frame, len);
	rx_len[h % RX_SLOTS] = (UH)len;
	rx_head = h + 1;
	n_recv++;
	tk_sig_sem(rx_sem, 1);
}

LOCAL UB	blk[NCM_IN_MAX];

/* One transfer: a frame (ECM), or a block of frames (NCM) */
LOCAL void rx_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	(void)ctx;
	if ( !cn.used || err != 0 || len <= 0 ) {
		if ( err != 0 ) n_err++;
		return;
	}
	if ( len > (INT)sizeof(blk) ) len = sizeof(blk);
	knl_dmabuf_read(b, off, blk, len);
	if ( !cn.ncm ) {
		rx_put(blk, len);
		return;
	}
	/* NTH16: "NCMH", its length, the sequence, the block's length, the first NDP16 */
	if ( len < 12 || GET32(blk) != 0x484d434e ) {
		n_err++;
		return;
	}
	{
		UW	ndp = GET16(blk + 10), k = 0;

		while ( ndp != 0 && ndp + 8 <= (UW)len && k++ < 8 ) {
			UW	sig = GET32(blk + ndp), nlen = GET16(blk + ndp + 4), i;

			if ( ( sig & 0x00ffffff ) != 0x004d434e || nlen < 16 || ndp + nlen > (UW)len ) {
				n_err++;
				return;
			}
			for ( i = 8; i + 4 <= nlen; i += 4 ) {
				UW	at = GET16(blk + ndp + i), dl = GET16(blk + ndp + i + 2);

				if ( at == 0 || dl == 0 ) break;
				if ( at + dl <= (UW)len ) rx_put(blk + at, (INT)dl);
			}
			ndp = GET16(blk + ndp + 6);
		}
	}
}

/* ---------------------------------------------------------------- the class driver */

LOCAL ER use_config( USBDEV *dev, FOUND *f )
{
	UB	hdr[9], *c;
	INT	n, i, total, ncfg = dev->ddesc[17];

	if ( find_function(dev->cfg, dev->cfglen, f) ) {
		return E_OK;
	}
	for ( i = 0; i < ncfg && i < 8; i++ ) {
		if ( knl_usb_get_desc(dev, USB_DT_CONFIG, (UB)i, 0, hdr, 9, &n) < E_OK || n < 9 ) continue;
		total = GET16(hdr + 2);
		if ( total < 9 || total > USB_CFG_MAX || hdr[5] == dev->cfg_value ) continue;
		c = (UB *)Kmalloc(total);
		if ( c == NULL ) return E_NOMEM;
		if ( knl_usb_get_desc(dev, USB_DT_CONFIG, (UB)i, 0, c, (UH)total, &n) < E_OK || n < 9
		  || !find_function(c, n, f) ) {
			Kfree(c);
			continue;
		}
		if ( knl_usb_control(dev, 0x00, USB_REQ_SET_CONFIG, c[5], 0, NULL, 0, NULL) < E_OK ) {
			Kfree(c);
			return E_IO;
		}
		USB_LOG("usb: cdc: %04x:%04x configuration %d set (of %d) for its %s function\n",
			(INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10), c[5], ncfg, f->ncm ? "NCM" : "ECM");
		Kfree(dev->cfg);
		dev->cfg = c;
		dev->cfglen = n;
		dev->cfg_value = c[5];
		return E_OK;
	}
	return E_NOEXS;
}

/* A device with an ECM or NCM function in one of its configurations (with the manager's lock held) */
EXPORT BOOL knl_cdcnet_match( USBDEV *dev )
{
	FOUND	f;
	UB	hdr[9];
	INT	n, i;

	if ( cn.used ) {
		return FALSE;
	}
	if ( find_function(dev->cfg, dev->cfglen, &f) ) {
		return TRUE;
	}
	for ( i = 0; i < dev->ddesc[17] && i < 8; i++ ) {
		UB	*c;
		INT	total;
		BOOL	yes;

		if ( knl_usb_get_desc(dev, USB_DT_CONFIG, (UB)i, 0, hdr, 9, &n) < E_OK || n < 9 ) continue;
		total = GET16(hdr + 2);
		if ( total < 9 || total > USB_CFG_MAX ) continue;
		c = (UB *)Kmalloc(total);
		if ( c == NULL ) return FALSE;
		yes = (BOOL)( knl_usb_get_desc(dev, USB_DT_CONFIG, (UB)i, 0, c, (UH)total, &n) >= E_OK
			      && n >= 9 && find_function(c, n, &f) );
		Kfree(c);
		if ( yes ) return TRUE;
	}
	return FALSE;
}

EXPORT ER knl_cdcnet_attach( USBDEV *dev )
{
	USBHC	*hc = dev->hc;
	FOUND	f;
	ER	er;
	INT	xfer;

	if ( rx_sem == 0 ) {
		T_CSEM	cs;
		T_CMTX	cm;

		knl_memset(&cs, 0, sizeof(cs));
		cs.sematr = TA_TFIFO;
		cs.maxsem = RX_SLOTS;
		rx_sem = tk_cre_sem(&cs);
		knl_memset(&cm, 0, sizeof(cm));
		cm.mtxatr = TA_TFIFO;
		tx_mtx = tk_cre_mtx(&cm);
		if ( rx_sem <= 0 || tx_mtx <= 0 ) return E_NOMEM;
	}
	er = use_config(dev, &f);
	if ( er < E_OK ) {
		return er;
	}
	knl_memset(&cn, 0, sizeof(cn));
	cn.dev = dev;
	cn.ncm = f.ncm;
	cn.ctrl_if = f.ctrl_if;
	cn.data_if = f.data_if;
	cn.data_alt = f.data_alt;
	cn.cfg_value = dev->cfg_value;
	cn.out_align = 4;
	knl_usb_ep_from_desc(&cn.ep_in, dev, f.ep_in, NULL);
	knl_usb_ep_from_desc(&cn.ep_out, dev, f.ep_out, NULL);
	if ( read_mac(dev, f.mac_str, cn.mac) < E_OK ) {
		USB_LOG("usb: cdc: no address in its descriptors (string %d)\n", f.mac_str);
		return E_NOEXS;
	}
	if ( cn.ncm ) {
		UB	p[28];
		INT	n = 0;
		UW	in;

		/* the blocks' sizes and alignment, before the data interface is set up */
		if ( knl_usb_control(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE, CDC_GET_NTB_PARAMETERS, 0,
				     (UH)cn.ctrl_if, p, sizeof(p), &n) < E_OK || n < 28 ) {
			return E_IO;
		}
		in = GET32(p + 4);
		cn.in_size = ( in > NCM_IN_MAX ) ? NCM_IN_MAX : in;
		cn.out_align = GET16(p + 26);
		if ( cn.out_align < 4 || cn.out_align > 64 ) cn.out_align = 4;
		PUT32(p, cn.in_size);
		(void)knl_usb_control(dev, USB_RT_CLASS | USB_RT_INTERFACE, CDC_SET_NTB_INPUT_SIZE, 0, (UH)cn.ctrl_if,
				      p, 4, NULL);
		xfer = (INT)cn.in_size;
	} else {
		xfer = ECM_XFER;
	}
	er = knl_usb_set_interface(dev, cn.data_if, cn.data_alt);
	if ( er < E_OK ) {
		return er;
	}
	if ( !cn.ncm ) {
		(void)knl_usb_control(dev, USB_RT_CLASS | USB_RT_INTERFACE, CDC_SET_PACKET_FILTER,
				      PF_DIRECTED | PF_BROADCAST | PF_ALL_MULTICAST, (UH)cn.ctrl_if, NULL, 0, NULL);
	}
	cn.txbuf = (UB *)Kmalloc(2048);
	if ( cn.txbuf == NULL ) {
		return E_NOMEM;
	}
	rx_head = rx_tail = 0;
	cn.used = TRUE;
	er = hc->ops->stream_open(hc, dev, &cn.ep_in, xfer, rx_xfer, &cn, &cn.pipe);
	if ( er < E_OK ) {
		cn.used = FALSE;
		Kfree(cn.txbuf);
		return er;
	}
	USB_LOG("usb: cdc: %s, %04x:%04x configuration %d, interfaces %d/%d alt %d, endpoints %02x/%02x,"
		" address %02x:%02x:%02x:%02x:%02x:%02x\n", cn.ncm ? "NCM" : "ECM",
		(INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10), cn.cfg_value, cn.ctrl_if, cn.data_if,
		cn.data_alt, cn.ep_in.addr, cn.ep_out.addr, cn.mac[0], cn.mac[1], cn.mac[2], cn.mac[3], cn.mac[4],
		cn.mac[5]);
	knl_net_link_said();			/* an adapter came: the adapters are looked at */
	return E_OK;
}

/* Every device that goes passes here: ours is let go */
EXPORT void knl_cdcnet_detach( USBDEV *dev )
{
	if ( !cn.used || cn.dev != dev ) {
		return;
	}
	cn.used = FALSE;
	if ( cn.pipe != NULL ) {
		dev->hc->ops->stream_close(dev->hc, cn.pipe);
		cn.pipe = NULL;
	}
	USB_LOG("usb: cdc: the adapter is gone\n");
	knl_net_link_said();
}

/* ---------------------------------------------------------------- what netsel.c calls */

EXPORT BOOL knl_cdcnet_present( void )
{
	return cn.used;
}

EXPORT ER knl_cdcnet_get_mac( UB *mac )
{
	if ( !cn.used ) return E_NOEXS;
	knl_memcpy(mac, cn.mac, NET_MAC_LEN);
	return E_OK;
}

EXPORT ER knl_cdcnet_send( CONST void *frame, SZ len )
{
	USBHC	*hc;
	INT	actual = 0, n;
	ER	er;

	if ( !cn.used ) return E_NOEXS;
	if ( frame == NULL || len < 14 || len > NET_FRAME_MAX ) return E_PAR;
	tk_loc_mtx(tx_mtx, TMO_FEVR);
	if ( cn.ncm ) {
		/* one block: NTH16 (12), NDP16 with the datagram and the end (16), the frame aligned */
		UW	at = ( 12 + 16 + cn.out_align - 1 ) / cn.out_align * cn.out_align;
		UB	*t = cn.txbuf;

		knl_memset(t, 0, at);
		PUT32(t, 0x484d434e);			/* "NCMH" */
		PUT16(t + 4, 12);
		PUT16(t + 6, cn.seq++);
		PUT16(t + 8, at + len);
		PUT16(t + 10, 12);
		PUT32(t + 12, 0x304d434e);		/* "NCM0" */
		PUT16(t + 16, 16);
		PUT16(t + 18, 0);
		PUT16(t + 20, at);
		PUT16(t + 22, len);
		knl_memcpy(t + at, frame, (INT)len);
		n = (INT)( at + len );
	} else {
		knl_memcpy(cn.txbuf, frame, (INT)len);
		n = (INT)len;
	}
	knl_usb_lock();
	er = cn.used ? E_OK : E_NOEXS;
	if ( er >= E_OK ) {
		hc = cn.dev->hc;
		er = hc->ops->bulk(hc, cn.dev, &cn.ep_out, cn.txbuf, n, &actual, 1000);
		if ( er >= E_OK && ( n % cn.ep_out.size ) == 0 ) {
			(void)hc->ops->bulk(hc, cn.dev, &cn.ep_out, cn.txbuf, 0, &actual, 1000);	/* the end of the transfer */
		}
	}
	knl_usb_unlock();
	tk_unl_mtx(tx_mtx);
	if ( er >= E_OK ) n_sent++; else n_err++;
	return er;
}

EXPORT INT knl_cdcnet_recv( void *frame, SZ size, TMO tmout )
{
	UW	t;
	INT	len;
	ER	er;

	if ( rx_sem <= 0 ) {
		if ( tmout != TMO_POL ) tk_dly_tsk(( tmout == TMO_FEVR || tmout > 1000 ) ? 1000 : tmout);
		return E_TMOUT;
	}
	er = tk_wai_sem(rx_sem, 1, tmout);
	if ( er < E_OK ) {
		return er;
	}
	t = rx_tail;
	len = rx_len[t % RX_SLOTS];
	if ( len > (INT)size ) len = (INT)size;
	knl_memcpy(frame, rx_ring[t % RX_SLOTS], len);
	rx_tail = t + 1;
	return len;
}

EXPORT ER knl_cdcnet_stat( T_NETSTAT *st )
{
	knl_memset(st, 0, sizeof(*st));
	st->sent = n_sent;
	st->recv = n_recv;
	st->drop = n_drop + n_err;
	st->up = cn.used;
	return cn.used ? E_OK : E_NOEXS;
}

#endif /* X64 */
