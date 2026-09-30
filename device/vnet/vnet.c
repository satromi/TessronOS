/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vnet.c
 *	virtio-net over virtio-mmio (QEMU virt), design 10.4.
 *
 *	The card is found on the same transport as the block device. Two
 *	queues are used: queue 0 receives, queue 1 sends. Each frame is
 *	preceded by the virtio header the modern interface requires.
 *
 *	This is the link layer only: frames go in and out whole. The
 *	protocol stack of phase 8 sits on top of it.
 */

#include <sys/machine.h>

#ifdef QEMU_VIRT

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include "sysman/pfalloc.h"

/* ---------------------------------------------------------------- virtio-mmio */
#define VIRTIO_MMIO_MAGIC		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW	0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG		0x100

#define VIRTIO_MAGIC_VALUE		0x74726976
#define VIRTIO_ID_NET			1

#define VIRTIO_STATUS_ACKNOWLEDGE	1
#define VIRTIO_STATUS_DRIVER		2
#define VIRTIO_STATUS_DRIVER_OK		4
#define VIRTIO_STATUS_FEATURES_OK	8
#define VIRTIO_STATUS_FAILED		128

#define VIRTIO_NET_F_MAC		5
#define VIRTIO_F_VERSION_1		32

#define VRING_DESC_F_NEXT		1
#define VRING_DESC_F_WRITE		2

#define VNET_QSIZE			8
#define VNET_HDR_LEN			12	/* modern virtio-net header */
#define VNET_BUF_LEN			(VNET_HDR_LEN + NET_FRAME_MAX)

typedef struct {
	UD	addr;
	UW	len;
	UH	flags;
	UH	next;
} VRING_DESC;

typedef struct {
	UH	flags;
	UH	idx;
	UH	ring[VNET_QSIZE];
	UH	used_event;
} VRING_AVAIL;

typedef struct {
	UW	id;
	UW	len;
} VRING_USED_ELEM;

typedef struct {
	UH		flags;
	UH		idx;
	VRING_USED_ELEM	ring[VNET_QSIZE];
	UH		avail_event;
} VRING_USED;

/* One queue: the three rings, kept in one page */
typedef struct {
	VRING_DESC	desc[VNET_QSIZE];
	UB		pad0[64 - (sizeof(VRING_DESC) * VNET_QSIZE) % 64];
	VRING_AVAIL	avail;
	UB		pad1[64 - sizeof(VRING_AVAIL) % 64];
	VRING_USED	used;
} VNET_QUEUE;

typedef struct {
	UBINT		base;
	UINT		intno;
	VNET_QUEUE	*rx, *tx;		/* the two queues (linear map) */
	UD		rx_pa, tx_pa;
	UB		*rxbuf, *txbuf;		/* VNET_QSIZE buffers each */
	UD		rxbuf_pa, txbuf_pa;
	UH		rx_last, tx_last;	/* used ring indexes consumed */
	UB		mac[6];
	ID		mtxid;		/* the sending side */
	ID		mtxid_rx;	/* and the receiving side, which waits */
	ID		flgid;
	UD		n_sent, n_recv, n_drop;
	BOOL		up;
} VNET_DEV;

LOCAL VNET_DEV	vnet;
LOCAL BOOL	vnet_found = FALSE;

/*
 * The card has one interrupt for both queues. It sets a bit for each
 * side, so that the sender waiting for the card to take a frame does
 * not consume the notice the reader waits for, and the other way round.
 */
#define VNET_FLG_RX	0x0001
#define VNET_FLG_TX	0x0002

/* ---------------------------------------------------------------- helpers */

LOCAL void vnet_out( UW off, UW val )
{
	out_w(vnet.base + off, val);
}

LOCAL UW vnet_in( UW off )
{
	return in_w(vnet.base + off);
}

LOCAL void dcache_flush( CONST void *p, UD len )
{
	UBINT	a = (UBINT)p & ~63ULL, end = (UBINT)p + len;

	Asm("dsb sy" ::: "memory");
	for ( ; a < end; a += 64 ) {
		Asm("dc civac, %0" :: "r"(a) : "memory");
	}
	Asm("dsb sy" ::: "memory");
}

LOCAL void vnet_inthdr( UINT intno, UW iar )
{
	vnet_out(VIRTIO_MMIO_INTERRUPT_ACK, vnet_in(VIRTIO_MMIO_INTERRUPT_STATUS));
	tk_set_flg(vnet.flgid, VNET_FLG_RX | VNET_FLG_TX);
	EndOfInt(intno);
}

/*
 * Hand one receive buffer back to the card.
 */
LOCAL void rx_post( INT i )
{
	VNET_QUEUE	*q = vnet.rx;
	UH		idx;

	q->desc[i].addr  = vnet.rxbuf_pa + (UD)i * VNET_BUF_LEN;
	q->desc[i].len   = VNET_BUF_LEN;
	q->desc[i].flags = VRING_DESC_F_WRITE;
	q->desc[i].next  = 0;

	idx = q->avail.idx;
	q->avail.ring[idx % VNET_QSIZE] = (UH)i;
	Asm("dmb ishst" ::: "memory");
	q->avail.idx = idx + 1;
	dcache_flush(q, sizeof(*q));

	vnet_out(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

/* ---------------------------------------------------------------- interface */

EXPORT ER net_get_mac( UB *mac )
{
	INT	i;

	if ( !vnet_found || mac == NULL ) {
		return E_NOEXS;
	}
	for ( i = 0; i < 6; i++ ) mac[i] = vnet.mac[i];

	return E_OK;
}

EXPORT ER net_send( CONST void *frame, SZ len )
{
	VNET_QUEUE	*q;
	UB		*buf;
	UH		idx;
	INT		slot;
	UINT		ptn;
	ER		er;

	if ( !vnet_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL || len < NET_FRAME_MIN || len > NET_FRAME_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(vnet.mtxid, TMO_FEVR);

	q = vnet.tx;
	slot = (INT)(q->avail.idx % VNET_QSIZE);
	buf = vnet.txbuf + (UD)slot * VNET_BUF_LEN;

	knl_memset(buf, 0, VNET_HDR_LEN);		/* no offload asked for */
	knl_memcpy(buf + VNET_HDR_LEN, frame, (INT)len);

	q->desc[slot].addr  = vnet.txbuf_pa + (UD)slot * VNET_BUF_LEN;
	q->desc[slot].len   = (UW)(VNET_HDR_LEN + len);
	q->desc[slot].flags = 0;
	q->desc[slot].next  = 0;

	dcache_flush(buf, VNET_HDR_LEN + len);
	tk_clr_flg(vnet.flgid, ~VNET_FLG_TX);	/* a notice from here on is this frame's */

	idx = q->avail.idx;
	q->avail.ring[idx % VNET_QSIZE] = (UH)slot;
	Asm("dmb ishst" ::: "memory");
	q->avail.idx = idx + 1;
	dcache_flush(q, sizeof(*q));

	vnet_out(VIRTIO_MMIO_QUEUE_NOTIFY, 1);

	/* wait for the card to take it */
	er = E_TMOUT;
	{
		INT	spin;

		for ( spin = 0; spin < 200; spin++ ) {
			dcache_flush(&q->used, sizeof(q->used));
			if ( q->used.idx != vnet.tx_last ) {
				vnet.tx_last = q->used.idx;
				vnet.n_sent++;
				er = E_OK;
				break;
			}
			tk_wai_flg(vnet.flgid, VNET_FLG_TX, TWF_ORW | TWF_BITCLR, &ptn, 5);
		}
	}

	tk_unl_mtx(vnet.mtxid);

	return er;
}

EXPORT INT net_recv( void *frame, SZ size, TMO tmout )
{
	VNET_QUEUE	*q;
	UINT		ptn;
	UD		t0 = 0;
	INT		got = -1;

	if ( !vnet_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL ) {
		return E_PAR;
	}
	/* Whoever reads the card holds it while waiting, and the
	   protocol stack keeps a reader on it for good. A second
	   caller is told so rather than left waiting. */
	if ( tk_loc_mtx(vnet.mtxid_rx, tmout) < E_OK ) {
		return E_BUSY;
	}
	q = vnet.rx;

	for (;;) {
		/* the notice is cleared before the ring is looked at, so that
		   a frame landing in between leaves it set for the wait */
		tk_clr_flg(vnet.flgid, ~VNET_FLG_RX);
		dcache_flush(&q->used, sizeof(q->used));
		if ( q->used.idx != vnet.rx_last ) {
			VRING_USED_ELEM	*e = &q->used.ring[vnet.rx_last % VNET_QSIZE];
			INT		slot = (INT)e->id;
			INT		len = (INT)e->len - VNET_HDR_LEN;
			UB		*buf = vnet.rxbuf + (UD)slot * VNET_BUF_LEN;

			vnet.rx_last++;
			if ( len > 0 && len <= (INT)size ) {
				dcache_flush(buf, e->len);
				knl_memcpy(frame, buf + VNET_HDR_LEN, len);
				got = len;
				vnet.n_recv++;
			} else {
				vnet.n_drop++;		/* too long for the caller */
			}
			rx_post(slot);			/* the buffer goes back */
			break;
		}
		if ( tmout == TMO_POL ) {
			got = E_TMOUT;
			break;
		}
		/* What is left is waited out in one call. Waking in slices
		   would keep the timer running and cost an interrupt each
		   time round, on a machine that is otherwise idle. */
		ts_get_mono(&t0);
		if ( tk_wai_flg(vnet.flgid, VNET_FLG_RX, TWF_ORW | TWF_BITCLR,
				&ptn, ( tmout > 0 ) ? tmout : TMO_FEVR) < E_OK ) {
			got = E_TMOUT;
			break;
		}
		if ( tmout > 0 ) {
			UD	t1 = 0;

			ts_get_mono(&t1);
			tmout -= (TMO)((t1 - t0) / 1000000U);
			if ( tmout <= 0 ) {
				got = E_TMOUT;
				break;
			}
		}
	}

	tk_unl_mtx(vnet.mtxid_rx);

	return got;
}

EXPORT ER net_stat( T_NETSTAT *st )
{
	if ( !vnet_found || st == NULL ) {
		return E_NOEXS;
	}
	st->sent = vnet.n_sent;
	st->recv = vnet.n_recv;
	st->drop = vnet.n_drop;
	st->up   = vnet.up;

	return E_OK;
}

/* ---------------------------------------------------------------- start-up */

LOCAL ER queue_setup( INT qno, VNET_QUEUE *q, UD q_pa )
{
	if ( vnet_in(VIRTIO_MMIO_QUEUE_NUM_MAX) < VNET_QSIZE ) {
		return E_NOSPT;
	}
	vnet_out(VIRTIO_MMIO_QUEUE_SEL, (UW)qno);
	vnet_out(VIRTIO_MMIO_QUEUE_NUM, VNET_QSIZE);
	vnet_out(VIRTIO_MMIO_QUEUE_DESC_LOW,    (UW)(q_pa + ((UBINT)q->desc - (UBINT)q)));
	vnet_out(VIRTIO_MMIO_QUEUE_DESC_HIGH,   (UW)((q_pa + ((UBINT)q->desc - (UBINT)q)) >> 32));
	vnet_out(VIRTIO_MMIO_QUEUE_DRIVER_LOW,  (UW)(q_pa + ((UBINT)&q->avail - (UBINT)q)));
	vnet_out(VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (UW)((q_pa + ((UBINT)&q->avail - (UBINT)q)) >> 32));
	vnet_out(VIRTIO_MMIO_QUEUE_DEVICE_LOW,  (UW)(q_pa + ((UBINT)&q->used - (UBINT)q)));
	vnet_out(VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (UW)((q_pa + ((UBINT)&q->used - (UBINT)q)) >> 32));
	vnet_out(VIRTIO_MMIO_QUEUE_READY, 1);

	return E_OK;
}

EXPORT INT knl_vnet_init( void )
{
	T_CMTX	cmtx;
	T_CFLG	cflg;
	PFRAME	*pf;
	UD	features;
	UW	status;
	INT	slot, i;

	for ( slot = 0; slot < VIRTIO_MMIO_NUM; slot++ ) {
		vnet.base  = VIRTIO_MMIO_BASE + (UBINT)slot * VIRTIO_MMIO_STRIDE;
		vnet.intno = VIRTIO_MMIO_INTNO(slot);

		if ( vnet_in(VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC_VALUE ) continue;
		if ( vnet_in(VIRTIO_MMIO_VERSION) != 2 ) continue;
		if ( vnet_in(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_NET ) continue;
		break;
	}
	if ( slot >= VIRTIO_MMIO_NUM ) {
		return 0;			/* the machine has no card */
	}

	/* Reset and announce */
	vnet_out(VIRTIO_MMIO_STATUS, 0);
	status = VIRTIO_STATUS_ACKNOWLEDGE;
	vnet_out(VIRTIO_MMIO_STATUS, status);
	status |= VIRTIO_STATUS_DRIVER;
	vnet_out(VIRTIO_MMIO_STATUS, status);

	vnet_out(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
	features = vnet_in(VIRTIO_MMIO_DEVICE_FEATURES);
	vnet_out(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
	features |= (UD)vnet_in(VIRTIO_MMIO_DEVICE_FEATURES) << 32;

	if ( (features & (1ULL << VIRTIO_F_VERSION_1)) == 0 ) {
		vnet_out(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return 0;
	}
	vnet_out(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
	vnet_out(VIRTIO_MMIO_DRIVER_FEATURES,
		 (UW)(features & (1U << VIRTIO_NET_F_MAC)));
	vnet_out(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
	vnet_out(VIRTIO_MMIO_DRIVER_FEATURES, 1U << (VIRTIO_F_VERSION_1 - 32));

	status |= VIRTIO_STATUS_FEATURES_OK;
	vnet_out(VIRTIO_MMIO_STATUS, status);
	if ( (vnet_in(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0 ) {
		vnet_out(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return 0;
	}

	/* the address the card was given */
	if ( (features & (1ULL << VIRTIO_NET_F_MAC)) != 0 ) {
		for ( i = 0; i < 6; i++ ) {
			vnet.mac[i] = (UB)(vnet_in(VIRTIO_MMIO_CONFIG + (i & ~3)) >> ((i & 3) * 8));
		}
	}

	/* one page for the two queues, four for the buffers of each side */
	pf = knl_alloc_pages(1, ZONE_DMA32, KAF_ZERO | KAF_DMA32);	/* 2 pages */
	if ( pf == NULL ) return E_NOMEM;
	vnet.rx_pa = knl_pf_to_pa(pf);
	vnet.tx_pa = vnet.rx_pa + PAGE_SIZE;
	vnet.rx = (VNET_QUEUE *)PA2VA(vnet.rx_pa);
	vnet.tx = (VNET_QUEUE *)PA2VA(vnet.tx_pa);

	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_ZERO | KAF_DMA32);	/* 16 pages */
	if ( pf == NULL ) return E_NOMEM;
	vnet.rxbuf_pa = knl_pf_to_pa(pf);
	vnet.txbuf_pa = vnet.rxbuf_pa + (UD)VNET_QSIZE * VNET_BUF_LEN;
	vnet.rxbuf = (UB *)PA2VA(vnet.rxbuf_pa);
	vnet.txbuf = (UB *)PA2VA(vnet.txbuf_pa);

	if ( queue_setup(0, vnet.rx, vnet.rx_pa) < E_OK
	  || queue_setup(1, vnet.tx, vnet.tx_pa) < E_OK ) {
		vnet_out(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
		return 0;
	}

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	vnet.mtxid = tk_cre_mtx(&cmtx);
	vnet.mtxid_rx = tk_cre_mtx(&cmtx);
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	vnet.flgid = tk_cre_flg(&cflg);
	if ( vnet.mtxid <= 0 || vnet.mtxid_rx <= 0 || vnet.flgid <= 0 ) {
		return E_LIMIT;
	}

	vnet_found = TRUE;
	vnet.up = TRUE;

	knl_define_inthdr((INT)vnet.intno, TA_HLNG, (FP)vnet_inthdr);
	EnableInt(vnet.intno, INTPRI_DEVICE);

	status |= VIRTIO_STATUS_DRIVER_OK;
	vnet_out(VIRTIO_MMIO_STATUS, status);

	/* give the card somewhere to put what arrives */
	vnet_out(VIRTIO_MMIO_QUEUE_SEL, 0);
	for ( i = 0; i < VNET_QSIZE; i++ ) {
		rx_post(i);
	}

	tm_printf((UB*)"TessronOS: vnet %02x:%02x:%02x:%02x:%02x:%02x\n",
		vnet.mac[0], vnet.mac[1], vnet.mac[2],
		vnet.mac[3], vnet.mac[4], vnet.mac[5]);

	return 1;
}

#endif /* QEMU_VIRT */
