/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xhci.c
 *	USB host controller (design 10.4, 10.14).
 *
 *	One controller serves every speed: SuperSpeed devices on its USB 3
 *	root ports, high, full and low speed ones on its USB 2 ports and
 *	behind hubs. For a device behind a hub the slot context carries the
 *	route string, and for a full or low speed one behind a high speed
 *	hub the hub that translates for it.
 *
 *	What it walks in memory, each of it made of pages:
 *	  the slot array	slot number to that device's context; the first
 *				entry points at the pages the controller asked
 *				to borrow
 *	  the command ring	one page closed by a Link back to its start
 *	  the event ring	up to EV_SEGS pages listed in a segment table
 *	  per device		its output and input contexts, and a transfer
 *				ring for each endpoint in use
 *
 *	Every address written into those is a bus address, which behind a
 *	bridge is not the physical one; usbdma.c keeps the two apart.
 *
 *	Control and bulk transfers are synchronous: the caller rings the
 *	doorbell and takes events until its own transfer has finished.
 *	Streams -- isochronous, bulk or interrupt IN, isochronous OUT -- keep
 *	a ring of transfer descriptors (TDs) armed; each one that finishes is
 *	handed to the class driver and put back at the end of the ring. TDs
 *	finish in the order they were armed, so a finished TD also accounts
 *	for older ones the controller skipped without an event of their own
 *	(an isochronous service interval it missed).
 *
 *	Events are only taken in task context, under ev_sem. The interrupt
 *	handler acknowledges the controller and wakes whoever is waiting.
 *
 *	TD Size is written as xHCI 1.0 defines it: the packets still to come.
 *	Before 1.0 the isochronous TBC/TLBPC fields do not exist and stay 0.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "sysdepend.h"
#include "usbdev.h"

/* ---------------------------------------------------------------- registers */

#define CAP_HCSPARAMS1		0x04
#define CAP_HCSPARAMS2		0x08
#define CAP_HCCPARAMS1		0x10
#define CAP_DBOFF		0x14
#define CAP_RTSOFF		0x18

#define HCS1_SLOTS(v)		((v) & 0xff)
#define HCS1_PORTS(v)		(((v) >> 24) & 0xff)
#define HCS2_ERSTMAX(v)		(((v) >> 4) & 0x0f)
/* the count is split in two, the high five bits above the low five */
#define HCS2_SPBUFS(v)		((INT)((((v) >> 21) & 0x1f) << 5 | (((v) >> 27) & 0x1f)))
#define HCC1_CSZ		0x00000004	/* contexts are 64 bytes */
#define HCC1_PPC		0x00000008	/* ports are powered by software */
#define HCC1_XECP(v)		(((v) >> 16) << 2)

/* extended capabilities */
#define XCAP_LEGACY		1
#define XCAP_PROTOCOL		2
#define LEG_BIOS		0x00010000
#define LEG_OS			0x01000000
#define LEGCTL_RSVDP		0x000e1fee	/* bits of USBLEGCTLSTS to keep */
#define LEGCTL_SMI_STS		0xe0000000	/* write 1 to clear */

#define OP_USBCMD		0x00
#define OP_USBSTS		0x04
#define OP_PAGESIZE		0x08
#define OP_CRCR			0x18
#define OP_DCBAAP		0x30
#define OP_CONFIG		0x38
#define OP_PORTSC(n)		(0x400 + 0x10 * (n))

#define CMD_RS			0x00000001	/* run */
#define CMD_HCRST		0x00000002	/* reset the whole controller */
#define CMD_INTE		0x00000004
#define CMD_HSEE		0x00000008

#define STS_HCH			0x00000001	/* halted */
#define STS_HSE			0x00000004
#define STS_EINT		0x00000008
#define STS_PCD			0x00000010	/* a port changed */
#define STS_CNR			0x00000800	/* not ready: do not write */
#define STS_HCE			0x00001000	/* it failed by itself */
#define STS_W1C			(STS_HSE | STS_EINT | STS_PCD)

#define RT_IMAN			0x20
#define RT_IMOD			0x24
#define RT_ERSTSZ		0x28
#define RT_ERSTBA		0x30
#define RT_ERDP			0x38

#define IMAN_IP			0x00000001
#define IMAN_IE			0x00000002
#define ERDP_EHB		0x00000008	/* the events have been taken */

#define CRCR_RCS		0x00000001	/* the cycle the ring starts on */
#define CRCR_CA			0x00000004	/* abort the command ring */
#define CRCR_CRR		0x00000008	/* it is running */

#define PS_CCS			0x00000001	/* something is connected */
#define PS_PED			0x00000002	/* and the port is enabled */
#define PS_PR			0x00000010	/* reset in progress */
#define PS_PP			0x00000200	/* powered */
#define PS_SPEED(v)		(((v) >> 10) & 0x0f)
#define PS_CSC			0x00020000	/* the connection changed */
#define PS_PRC			0x00200000	/* the reset finished */
#define PS_CHANGES		0x00fe0000	/* every change bit */
/*
 * Writing a port register back means keeping the bits that mean
 * something when written and dropping the ones that would clear a change
 * the software has not looked at yet, or disable the port.
 */
#define PS_NEUTRAL		0x4f00ffe9

/* what a ring entry is, and the flags it carries */
#define TRB_CYCLE		0x00000001
#define TRB_TC			0x00000002	/* the cycle turns round here */
#define TRB_ISP			0x00000004	/* answer on a short packet too */
#define TRB_CH			0x00000010	/* the TD goes on in the next */
#define TRB_IOC			0x00000020	/* answer when this one is done */
#define TRB_IDT			0x00000040	/* the data is in the entry */
#define TRB_BEI			0x00000200	/* the answer raises no interrupt */
#define TRB_DIR_IN		0x00010000
#define TRB_SIA			0x80000000	/* as soon as possible */
#define TRB_TYPE(t)		((UW)(t) << 10)
#define TRB_GETTYPE(c)		(((c) >> 10) & 0x3f)
#define TRB_SLOT(s)		((UW)(s) << 24)
#define TRB_EP(e)		((UW)(e) << 16)

#define T_NORMAL		1
#define T_SETUP			2
#define T_DATA			3
#define T_STATUS		4
#define T_ISOCH			5
#define T_LINK			6
#define T_ENABLE_SLOT		9
#define T_DISABLE_SLOT		10
#define T_ADDRESS_DEV		11
#define T_CONFIG_EP		12
#define T_EVAL_CTX		13
#define T_RESET_EP		14
#define T_STOP_EP		15
#define T_SET_DEQ		16
#define T_EV_TRANSFER		32
#define T_EV_CMD		33
#define T_EV_PORT		34
#define T_EV_HC			37

/* how a command or a transfer ended */
#define CC_SUCCESS		1
#define CC_STALL		6
#define CC_SHORT		13		/* less came back than was asked */
#define CC_UNDERRUN		14
#define CC_OVERRUN		15
#define CC_MISSED		23
#define CC_RING_STOPPED		24		/* after an abort */
#define CC_CMD_ABORTED		25
#define CC_STOPPED		26
#define CC_STOPPED_LEN		27
#define CC_STOPPED_SHORT	28

/* endpoint kinds, as the endpoint context has them */
#define EPT_ISOC_OUT		1
#define EPT_BULK_OUT		2
#define EPT_INTR_OUT		3
#define EPT_CONTROL		4
#define EPT_ISOC_IN		5
#define EPT_BULK_IN		6
#define EPT_INTR_IN		7

/* ---------------------------------------------------------------- layout */

#define TRB_PER_PAGE		256		/* 4096 / 16 */
#define RING_MAXPG		4
/*
 * Event ring segments of a page each. Every armed TD can put an event on
 * it -- BEI only keeps the interrupt back, not the event -- and the
 * streams keep up to ISO_NTD_MAX + ISO_OUT_NTD of them armed, so the ring
 * has to hold more than that or it overflows while a port is reset.
 */
#define EV_SEGS			8
#define MAX_SLOTS		32
#define MAX_ROOTPORTS		32
#define MAX_SCRATCH		64
#define CTRL_BUF_BYTES		(4 * USB_PAGE)
#define BULK_BUF_BYTES		(16 * USB_PAGE)
#define XHCI_PIPES		16
#define STALL_MAX		8	/* recoveries before a stream is given up */
/*
 * TDs armed on an isochronous IN ring, one per service interval. A
 * controller that fills them from larger transfers of its own packs
 * several packets into one TD when fewer than that are armed, and the
 * packet boundaries are lost with it: 384 is 48ms at high speed.
 */
#define ISO_NTD_MAX		384
#define ISO_IRQ_EVERY		8	/* an interrupt for every so many TDs */
/*
 * Isochronous OUT: TDs queued ahead, which is also how far ahead of what
 * is heard the data is. A quarter of a second at one TD per millisecond.
 */
#define ISO_OUT_NTD		256
#define BULK_NTD		8
#define INTR_NTD		8	/* interrupt IN: a report each */
#define CMD_TMO			2000

typedef struct {
	volatile UW	p0, p1, st, ctl;
} TRB;

/* What the driver put into each entry of a transfer ring */
typedef struct {
	INT	td;		/* stream: the TD; synchronous: the stage */
	UW	gen;		/* the generation the TD was armed in */
	INT	off;		/* bytes of the TD before this entry */
	INT	len;		/* bytes in this entry */
	BOOL	last;		/* the TD's last entry */
} TRBINFO;

#define TI_SETUP		0
#define TI_DATA			1
#define TI_STATUS		2

typedef struct {
	INT	npg;
	TRB	*pg[RING_MAXPG];
	UD	bus[RING_MAXPG];
	INT	enq;		/* the next entry to write, never a Link */
	UW	pcs;		/* the cycle the producer writes */
	TRBINFO	*info;		/* npg * TRB_PER_PAGE of them */
} XRING;

typedef struct {
	UW	gen;
	BOOL	pending;
	DMABUF	*b;
	INT	off;
	INT	total;		/* its bytes (IN: as armed) */
} XTD;

typedef struct xep {
	INT	dci;		/* the device context index: ep * 2 + IN */
	USBEP	ep;
	XRING	ring;
	USBPIPE	*pipe;		/* the stream on it, NULL for synchronous */
	/* the synchronous transfer in progress */
	UW	gen;
	volatile BOOL done;
	INT	cc;
	INT	actual;
	BOOL	is_bulk;
} XEP;

struct xhci;

typedef struct xdev {
	struct xhci *x;
	USBDEV	*dev;
	INT	slot;
	UB	*out;		/* the output device context */
	UD	out_bus;
	UB	*in;		/* the input context */
	UD	in_bus;
	XEP	*ep[32];
} XDEV;

struct usb_pipe {
	struct xhci *x;
	XDEV	*xd;
	XEP	*xe;
	USBEP	ep;
	USB_XFERFN fn;
	USB_FILLFN fill;	/* OUT streams */
	void	*ctx;
	BOOL	open;
	BOOL	iso;
	BOOL	out;		/* isochronous OUT */
	BOOL	halted;
	INT	halts;		/* stalls since the last good transfer */
	BOOL	ring;		/* the doorbell is due after re-arming */
	INT	ntd;
	INT	td_bytes;	/* bytes per TD */
	XTD	*td;
	INT	*order;		/* armed TDs, oldest first (a ring of ntd) */
	INT	o_head, o_count;
	DMABUF	*bufs;
	INT	nbufs;
	UB	*out_buf;	/* OUT: what the fill callback made */
	INT	irq_every;
};

typedef struct xhci {
	USBHC	hc;
	UD	mmio;
	UD	mmio_size;
	UBINT	cap, op, rt, db;
	UH	version;
	UW	hcs1, hcs2, hcc1;
	INT	nports;
	INT	max_slots;
	INT	csz;		/* context size: 32 or 64 bytes */
	UB	port_major[MAX_ROOTPORTS];	/* 2 or 3, 0 unknown */
	UW	*dcbaa;
	UD	dcbaa_bus;
	UW	*sp_array;
	UD	sp_array_bus;
	UB	*sp_page[MAX_SCRATCH];
	INT	nsp;
	XRING	cmd;
	TRB	*ev_seg[EV_SEGS];
	UD	ev_bus[EV_SEGS];
	INT	nevseg;
	UW	*erst;
	UD	erst_bus;
	INT	ev_seg_idx, ev_idx;
	UW	ccs;		/* the cycle the consumer expects */
	/* the command being waited for */
	volatile BOOL cmd_wait;
	volatile BOOL cmd_done;
	volatile BOOL ring_stopped;	/* Command Ring Stopped came */
	UD	cmd_bus;
	INT	cmd_cc;
	INT	cmd_slot;
	XDEV	*slots[MAX_SLOTS + 1];
	DMABUF	ctrl_buf, bulk_buf;
	ID	cmd_sem, xfer_sem, ev_sem, ev_flg;
	USBPIPE	pipes[XHCI_PIPES];
	BOOL	running;
	BOOL	dead;		/* stopped on an error: it may still read memory */
} XHCI;

LOCAL XHCI	*xhci_tbl[USB_MAX_HC];

/* ---------------------------------------------------------------- access */

LOCAL UW rd32( UBINT base, UINT off )
{
	return *(volatile UW *)(base + off);
}

LOCAL void wr32( UBINT base, UINT off, UW v )
{
	*(volatile UW *)(base + off) = v;
	Asm("dsb sy" ::: "memory");
}

/*
 * A 64 bit register is written as two words, low half first: a
 * controller that latches on the high half would otherwise act on an
 * address that is half old.
 */
LOCAL void wr64( UBINT base, UINT off, UD v )
{
	*(volatile UW *)(base + off) = (UW)v;
	*(volatile UW *)(base + off + 4) = (UW)(v >> 32);
	Asm("dsb sy" ::: "memory");
}

LOCAL void doorbell( XHCI *x, INT slot, UW v )
{
	USB_MB();
	*(volatile UW *)(x->db + (UBINT)slot * 4) = v;
	Asm("dsb sy" ::: "memory");
}

/* Wait for some bits of a register to take a value, a millisecond a look */
LOCAL BOOL wait_reg( UBINT base, UINT off, UW mask, UW want, INT ms )
{
	INT	i;

	for ( i = 0; i <= ms; i++ ) {
		if ( (rd32(base, off) & mask) == want ) {
			return TRUE;
		}
		tk_dly_tsk(1);
	}

	return FALSE;
}

LOCAL void sem_take( ID id )
{
	tk_wai_sem(id, 1, TMO_FEVR);
}

LOCAL void sem_give( ID id )
{
	tk_sig_sem(id, 1);
}

/* ---------------------------------------------------------------- rings */

LOCAL void ring_free( XRING *r )
{
	INT	i;

	for ( i = 0; i < r->npg; i++ ) {
		knl_usb_page_free(r->pg[i]);
	}
	if ( r->info != NULL ) {
		Kfree(r->info);
	}
	knl_memset(r, 0, sizeof(*r));
}

/*
 * A ring of npg pages, each closed by a Link to the next; the last one
 * links back to the first and turns the cycle round.
 */
LOCAL ER ring_alloc( XRING *r, INT npg )
{
	INT	i;

	knl_memset(r, 0, sizeof(*r));
	for ( i = 0; i < npg; i++ ) {
		r->pg[i] = (TRB *)knl_usb_page_alloc(&r->bus[i]);
		if ( r->pg[i] == NULL ) {
			ring_free(r);
			return E_NOMEM;
		}
		r->npg = i + 1;
	}
	for ( i = 0; i < npg; i++ ) {
		TRB	*l = &r->pg[i][TRB_PER_PAGE - 1];
		UD	next = r->bus[(i + 1) % npg];

		l->p0  = (UW)next;
		l->p1  = (UW)(next >> 32);
		l->st  = 0;
		l->ctl = TRB_TYPE(T_LINK) | (( i == npg - 1 ) ? TRB_TC : 0);
	}
	r->info = (TRBINFO *)Kcalloc((SZ)npg * TRB_PER_PAGE, sizeof(TRBINFO));
	if ( r->info == NULL ) {
		ring_free(r);
		return E_NOMEM;
	}
	r->pcs = 1;
	r->enq = 0;
	USB_MB();

	return E_OK;
}

LOCAL UD ring_bus_of( XRING *r, INT i )
{
	return r->bus[i / TRB_PER_PAGE] + (UD)(i % TRB_PER_PAGE) * 16;
}

LOCAL INT ring_index_of( XRING *r, UD ptr )
{
	INT	i;

	for ( i = 0; i < r->npg; i++ ) {
		if ( ptr >= r->bus[i] && ptr < r->bus[i] + USB_PAGE ) {
			return i * TRB_PER_PAGE + (INT)((ptr - r->bus[i]) / 16);
		}
	}

	return -1;
}

/* Entries that can be used without writing over a Link */
LOCAL INT ring_capacity( XRING *r )
{
	return r->npg * (TRB_PER_PAGE - 1);
}

/*
 * Write one entry where the ring is to be written next. With hold the
 * cycle bit is left the wrong way round, so the controller does not start
 * on a TD still being written; ring_release() hands it over once the rest
 * is there. Answers where the entry went.
 */
LOCAL INT ring_put( XRING *r, UD p, UW st, UW ctl, CONST TRBINFO *inf,
		    BOOL hold, UW *held )
{
	INT	i = r->enq, at = r->enq;
	TRB	*t = &r->pg[i / TRB_PER_PAGE][i % TRB_PER_PAGE];
	UW	cyc = r->pcs;

	t->p0 = (UW)p;
	t->p1 = (UW)(p >> 32);
	t->st = st;
	USB_MB();
	/* the cycle bit is written last: it is what hands the entry over */
	t->ctl = (ctl & ~TRB_CYCLE) | ( hold ? (cyc ^ 1) : cyc );
	USB_MB();
	if ( inf != NULL ) {
		r->info[i] = *inf;
	}
	if ( hold && held != NULL ) {
		*held = cyc;
	}

	i++;
	if ( i % TRB_PER_PAGE == TRB_PER_PAGE - 1 ) {
		/* the Link is handed over too; chained when it is inside a TD */
		INT	pg = (i - 1) / TRB_PER_PAGE;
		TRB	*l = &r->pg[pg][TRB_PER_PAGE - 1];

		l->ctl = (l->ctl & ~(TRB_CYCLE | TRB_CH)) | (ctl & TRB_CH) | r->pcs;
		USB_MB();
		if ( pg == r->npg - 1 ) {
			r->pcs ^= 1;
			i = 0;
		} else {
			i = (pg + 1) * TRB_PER_PAGE;
		}
	}
	r->enq = i;

	return at;
}

LOCAL void ring_release( XRING *r, INT idx, UW cyc )
{
	TRB	*t = &r->pg[idx / TRB_PER_PAGE][idx % TRB_PER_PAGE];

	USB_MB();
	t->ctl = (t->ctl & ~TRB_CYCLE) | cyc;
	USB_MB();
}

/* ---------------------------------------------------------------- interrupts */

/*
 * The controller raised one. Both reason bits are cleared, the one in the
 * status and the one in the interrupter, or it will not raise another;
 * then whoever is waiting and the manager's task are woken.
 */
EXPORT void knl_xhci_inthdr( UINT intno )
{
	INT	i;

	for ( i = 0; i < USB_MAX_HC; i++ ) {
		XHCI	*x = xhci_tbl[i];
		UW	sts, iman;

		if ( x == NULL || x->hc.intno != intno || !x->running ) {
			continue;
		}
		sts = rd32(x->op, OP_USBSTS);
		if ( sts == 0xffffffff ) {
			continue;
		}
		if ( (sts & STS_W1C) != 0 ) {
			wr32(x->op, OP_USBSTS, sts & STS_W1C);
		}
		iman = rd32(x->rt, RT_IMAN);
		if ( (iman & IMAN_IP) != 0 ) {
			wr32(x->rt, RT_IMAN, IMAN_IP | IMAN_IE);
		}
		knl_usb_stat.irqs++;
		tk_set_flg(x->ev_flg, 1);
		tk_set_flg(x->hc.flgid, USBF_IRQ);
	}
	EndOfInt(intno);
}

/* ---------------------------------------------------------------- events */

LOCAL void stream_event( USBPIPE *s, INT idx, UW st );
LOCAL void pipe_arm( USBPIPE *s, INT i );

LOCAL void sync_event( XEP *e, INT idx, UW st )
{
	TRBINFO	*inf;
	INT	cc = (INT)(st >> 24), res = (INT)(st & 0xffffff);

	if ( idx < 0 ) {
		return;
	}
	inf = &e->ring.info[idx];
	if ( inf->gen != e->gen || e->done ) {
		return;			/* the remains of an earlier transfer */
	}
	if ( cc == CC_SUCCESS || cc == CC_SHORT ) {
		if ( inf->td == TI_DATA && ( cc == CC_SHORT || inf->last ) ) {
			e->actual = inf->off + inf->len - res;
			if ( e->actual < 0 ) {
				e->actual = 0;
			}
		}
		if ( ( cc == CC_SHORT && e->is_bulk )
		  || ( inf->last && inf->td != TI_DATA )
		  || ( inf->last && e->is_bulk ) ) {
			e->cc = CC_SUCCESS;
			e->done = TRUE;
		}
	} else {
		e->cc = cc;
		e->done = TRUE;
	}
}

/*
 * Take everything the controller has answered. The read pointer is
 * written back even when nothing was there: that write is what clears
 * Event Handler Busy, and while it is set no further interrupt comes.
 */
LOCAL void events_service( XHCI *x )
{
	INT	i;

	sem_take(x->ev_sem);
	for (;;) {
		TRB	*t = &x->ev_seg[x->ev_seg_idx][x->ev_idx];
		UW	p0, p1, st, ctl, type;

		USB_MB();
		ctl = t->ctl;
		if ( (ctl & TRB_CYCLE) != x->ccs ) {
			break;			/* the rest is not written yet */
		}
		p0 = t->p0;
		p1 = t->p1;
		st = t->st;
		knl_usb_stat.events++;
		if ( ++x->ev_idx == TRB_PER_PAGE ) {
			x->ev_idx = 0;
			if ( ++x->ev_seg_idx == x->nevseg ) {
				x->ev_seg_idx = 0;
				x->ccs ^= 1;
			}
		}
		type = TRB_GETTYPE(ctl);
		switch ( type ) {
		case T_EV_TRANSFER: {
			INT	slot = (INT)(ctl >> 24), dci = (INT)((ctl >> 16) & 0x1f);
			XDEV	*d = ( slot >= 1 && slot <= MAX_SLOTS ) ? x->slots[slot] : NULL;
			XEP	*e = ( d != NULL ) ? d->ep[dci] : NULL;
			UD	ptr = (UD)p0 | ((UD)p1 << 32);

			if ( e == NULL ) {
				break;
			}
			if ( e->pipe != NULL ) {
				stream_event(e->pipe, ring_index_of(&e->ring, ptr), st);
			} else {
				sync_event(e, ring_index_of(&e->ring, ptr), st);
			}
			break;
		}
		case T_EV_CMD: {
			UD	ptr = (UD)p0 | ((UD)p1 << 32);

			/*
			 * An abort ends with Command Ring Stopped, after the
			 * aborted command's own completion. Neither belongs to
			 * a command being waited for, even one that sits at the
			 * same place of the rebuilt ring.
			 */
			if ( (st >> 24) == CC_RING_STOPPED ) {
				x->ring_stopped = TRUE;
				break;
			}
			if ( (st >> 24) == CC_CMD_ABORTED ) {
				break;
			}
			/* a command that timed out may still answer, much later */
			if ( x->cmd_wait && ptr == x->cmd_bus && !x->cmd_done ) {
				x->cmd_cc   = (INT)(st >> 24);
				x->cmd_slot = (INT)(ctl >> 24);
				x->cmd_done = TRUE;
			}
			break;
		}
		case T_EV_PORT:
			tk_set_flg(x->hc.flgid, USBF_RESCAN);
			break;
		case T_EV_HC:
			USB_LOG("usb: xhci%d: controller event, code %d\n",
				x->hc.index, (INT)(st >> 24));
			break;
		default:
			break;
		}
	}
	wr64(x->rt, RT_ERDP,
	     (x->ev_bus[x->ev_seg_idx] + (UD)x->ev_idx * 16) | ERDP_EHB);
	for ( i = 0; i < XHCI_PIPES; i++ ) {
		USBPIPE	*s = &x->pipes[i];

		if ( s->open && s->ring ) {
			s->ring = FALSE;
			doorbell(x, s->xd->slot, (UW)s->xe->dci);
		}
	}
	sem_give(x->ev_sem);
}

/*
 * Take events until *flag becomes true or tmo milliseconds pass. With an
 * interrupt the wait sleeps until one comes, in slices so that a wake
 * taken by another waiter costs at most one slice; without one it looks
 * every millisecond.
 */
LOCAL BOOL wait_for( XHCI *x, volatile BOOL *flag, TMO tmo )
{
	UW	start = knl_usb_ms();
	UINT	ptn;

	for (;;) {
		events_service(x);
		if ( *flag ) {
			return TRUE;
		}
		if ( (INT)(knl_usb_ms() - start) > tmo ) {
			return FALSE;
		}
		tk_wai_flg(x->ev_flg, 1, TWF_ORW | TWF_CLR, &ptn,
			   ( x->hc.intno != 0 ) ? 20 : 1);
	}
}

/* ---------------------------------------------------------------- commands */

/*
 * The command ring after an abort: every entry back to nothing (the
 * controller must not run what is left of the old commands), the links
 * put back, and the controller pointed at the start again.
 */
LOCAL void cmd_ring_reset( XHCI *x )
{
	XRING	*r = &x->cmd;
	INT	i, k;

	for ( i = 0; i < r->npg; i++ ) {
		for ( k = 0; k < TRB_PER_PAGE; k++ ) {
			r->pg[i][k].p0 = r->pg[i][k].p1 = 0;
			r->pg[i][k].st = r->pg[i][k].ctl = 0;
		}
		{
			TRB	*l = &r->pg[i][TRB_PER_PAGE - 1];
			UD	next = r->bus[(i + 1) % r->npg];

			l->p0  = (UW)next;
			l->p1  = (UW)(next >> 32);
			l->ctl = TRB_TYPE(T_LINK) | (( i == r->npg - 1 ) ? TRB_TC : 0);
		}
	}
	r->enq = 0;
	r->pcs = 1;
	USB_MB();
	wr64(x->op, OP_CRCR, r->bus[0] | CRCR_RCS);
}

/*
 * A command that never finished: tell the controller to give the command
 * ring up, then start the ring over. Otherwise it may still be working on
 * it, and every later command would queue up behind it.
 */
LOCAL void cmd_abort( XHCI *x )
{
	INT	i;

	x->ring_stopped = FALSE;
	/* only the abort bit counts while the ring runs */
	wr64(x->op, OP_CRCR, CRCR_CA);
	if ( !wait_reg(x->op, OP_CRCR, CRCR_CRR, 0, 100) ) {
		USB_LOG("usb: xhci%d: the command ring did not stop, controller given up\n",
			x->hc.index);
		wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~CMD_INTE);
		x->running = FALSE;
		x->dead = TRUE;
		x->hc.lost = TRUE;
		return;
	}
	/*
	 * Command Ring Stopped comes last, after whatever was on the ring,
	 * so waiting for it takes everything the old ring left behind.
	 */
	for ( i = 0; i < 100 && !x->ring_stopped; i++ ) {
		events_service(x);
		if ( !x->ring_stopped ) {
			tk_dly_tsk(1);
		}
	}
	cmd_ring_reset(x);
}

/* Answers the completion code, or -1 when nothing came back */
LOCAL INT x_cmd( XHCI *x, UD p, UW ctl, INT *p_slot )
{
	INT	cc;

	if ( !x->running ) {
		return -1;
	}
	sem_take(x->cmd_sem);
	x->cmd_done = FALSE;
	x->cmd_bus = ring_bus_of(&x->cmd, x->cmd.enq);
	x->cmd_wait = TRUE;
	ring_put(&x->cmd, p, 0, ctl, NULL, FALSE, NULL);
	doorbell(x, 0, 0);
	if ( !wait_for(x, &x->cmd_done, CMD_TMO) ) {
		USB_LOG("usb: xhci%d: command %d timed out (sts %08x)\n", x->hc.index,
			(INT)TRB_GETTYPE(ctl), rd32(x->op, OP_USBSTS));
		knl_usb_stat.cmd_timeouts++;
		x->cmd_wait = FALSE;
		cmd_abort(x);
		sem_give(x->cmd_sem);
		return -1;
	}
	x->cmd_wait = FALSE;
	cc = x->cmd_cc;
	if ( p_slot != NULL ) {
		*p_slot = x->cmd_slot;
	}
	sem_give(x->cmd_sem);

	return cc;
}

/* ---------------------------------------------------------------- contexts */

LOCAL UW *in_ctx( XDEV *d, INT i )	/* 0 control, 1 slot, 1 + dci endpoint */
{
	return (UW *)(d->in + (UBINT)i * d->x->csz);
}

LOCAL UW *out_ctx( XDEV *d, INT i )	/* 0 slot, dci endpoint */
{
	return (UW *)(d->out + (UBINT)i * d->x->csz);
}

LOCAL void in_clear( XDEV *d )
{
	knl_memset(d->in, 0, 33 * d->x->csz);
}

/*
 * The slot of a device being addressed. A full or low speed device behind
 * a high speed hub names that hub's slot and port, its transaction
 * translator. Hubs run with a single translator (the multi-TT setting is
 * never chosen), so the MTT bit stays clear.
 */
LOCAL void slot_fill( XDEV *d, UW *s )
{
	USBDEV	*dev = d->dev;
	INT	tt_port = 0;
	USBDEV	*tt = knl_usb_tt_hub(dev, &tt_port);
	UW	speed = ( dev->speed == USB_SPEED_SUPER_PLUS ) ? 5 : (UW)dev->speed;

	s[0] = (dev->route & 0xfffff) | (speed << 20) | (1UL << 27);
	s[1] = (UW)(dev->root_port + 1) << 16;
	s[2] = 0;
	s[3] = 0;
	if ( tt != NULL && tt->hcpriv != NULL ) {
		s[2] = (UW)((XDEV *)tt->hcpriv)->slot | ((UW)tt_port << 8);
	}
}

LOCAL void ep0_fill( XDEV *d, UW *c )
{
	XEP	*e = d->ep[1];
	UD	b = e->ring.bus[0] | e->ring.pcs;

	c[0] = 0;
	c[1] = (3 << 1) | (EPT_CONTROL << 3) | ((UW)d->dev->mps0 << 16);
	c[2] = (UW)b;
	c[3] = (UW)(b >> 32);
	c[4] = 8;
}

LOCAL INT log2floor( INT v )
{
	INT	n = 0;

	while ( v > 1 ) {
		v >>= 1;
		n++;
	}

	return n;
}

LOCAL void ep_fill( XDEV *d, XEP *e, UW *c )
{
	USBEP	*ep = &e->ep;
	BOOL	in = ( (ep->addr & 0x80) != 0 );
	INT	type, interval = 0, mult = 0, burst = 0, cerr = 3, avg, esit = 0;
	INT	speed = d->dev->speed;
	UD	b = e->ring.bus[0] | e->ring.pcs;

	switch ( ep->type ) {
	case USB_EP_ISOC: type = in ? EPT_ISOC_IN : EPT_ISOC_OUT; cerr = 0; break;
	case USB_EP_BULK: type = in ? EPT_BULK_IN : EPT_BULK_OUT; break;
	case USB_EP_INTR: type = in ? EPT_INTR_IN : EPT_INTR_OUT; break;
	default:	  type = EPT_CONTROL; break;
	}
	/*
	 * How often, as a power of two of 125us. A fast device counts in
	 * microframes already; a full speed isochronous one gives a power of
	 * two of frames, an interrupt one a number of frames.
	 */
	if ( ep->type == USB_EP_ISOC || ep->type == USB_EP_INTR ) {
		INT	v = ep->interval;

		if ( speed == USB_SPEED_HIGH || speed >= USB_SPEED_SUPER ) {
			if ( v < 1 ) v = 1;
			if ( v > 16 ) v = 16;
			interval = v - 1;
		} else if ( ep->type == USB_EP_ISOC ) {
			if ( v < 1 ) v = 1;
			if ( v > 16 ) v = 16;
			interval = v - 1 + 3;
		} else {
			if ( v < 1 ) v = 1;
			interval = log2floor(v * 8);
			if ( interval < 3 ) interval = 3;
			if ( interval > 10 ) interval = 10;
		}
		esit = ep->esit;
	}
	if ( speed >= USB_SPEED_SUPER ) {
		burst = ep->burst;
		mult  = ( ep->type == USB_EP_ISOC ) ? ep->ss_mult : 0;
	} else if ( speed == USB_SPEED_HIGH
		 && ( ep->type == USB_EP_ISOC || ep->type == USB_EP_INTR ) ) {
		burst = ep->mult - 1;
	}
	avg = ( ep->type == USB_EP_BULK ) ? 3072 : ( esit > 0 ? esit : 8 );
	c[0] = ((UW)mult << 8) | ((UW)interval << 16) | ((UW)(esit >> 16) << 24);
	c[1] = ((UW)cerr << 1) | ((UW)type << 3) | ((UW)burst << 8)
	     | ((UW)ep->size << 16);
	c[2] = (UW)b;
	c[3] = (UW)(b >> 32);
	c[4] = (UW)avg | ((UW)(esit & 0xffff) << 16);
}

/* The highest context in use, which the slot context has to say */
LOCAL INT max_dci( XDEV *d )
{
	INT	i, m = 1;

	for ( i = 2; i < 32; i++ ) {
		if ( d->ep[i] != NULL ) {
			m = i;
		}
	}

	return m;
}

/* The input slot context: the slot as it stands, the entries brought up to date */
LOCAL void slot_from_output( XDEV *d )
{
	UW	*s = in_ctx(d, 1);

	knl_memcpy(s, out_ctx(d, 0), d->x->csz);
	s[0] = (s[0] & ~(0x1fUL << 27)) | ((UW)max_dci(d) << 27);
	s[3] = 0;
}

/* ---------------------------------------------------------------- devices */

LOCAL void xep_free( XEP *e )
{
	if ( e == NULL ) {
		return;
	}
	ring_free(&e->ring);
	Kfree(e);
}

/*
 * An endpoint's ring goes back only when the controller is known to have
 * let go of it. After a command that failed or timed out it may still
 * read those pages, and a page handed back would be written behind its
 * next owner's back: it is kept instead.
 */
LOCAL void xep_release( XEP *e, BOOL stopped )
{
	if ( e == NULL ) {
		return;
	}
	if ( !stopped ) {
		USB_LOG("usb: endpoint %d not stopped: its ring is kept\n", e->dci);
		return;
	}
	xep_free(e);
}

LOCAL void xhci_dev_free( USBHC *hc, USBDEV *dev );

LOCAL ER xhci_dev_init( USBHC *hc, USBDEV *dev )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d;
	XEP	*e0;
	INT	slot = 0, cc;

	cc = x_cmd(x, 0, TRB_TYPE(T_ENABLE_SLOT), &slot);
	if ( cc != CC_SUCCESS || slot < 1 || slot > MAX_SLOTS ) {
		USB_LOG("usb: xhci%d: Enable Slot failed (cc %d slot %d)\n",
			hc->index, cc, slot);
		if ( cc == CC_SUCCESS && slot > 0 ) {
			x_cmd(x, 0, TRB_TYPE(T_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
		}
		return E_LIMIT;
	}
	d = (XDEV *)Kcalloc(1, sizeof(XDEV));
	e0 = (XEP *)Kcalloc(1, sizeof(XEP));
	if ( d == NULL || e0 == NULL ) {
		if ( d != NULL ) Kfree(d);
		if ( e0 != NULL ) Kfree(e0);
		x_cmd(x, 0, TRB_TYPE(T_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
		return E_NOMEM;
	}
	d->x = x;
	d->dev = dev;
	d->slot = slot;
	e0->dci = 1;
	e0->ep.type = USB_EP_CONTROL;
	e0->ep.size = dev->mps0;
	d->ep[1] = e0;
	d->out = (UB *)knl_usb_page_alloc(&d->out_bus);
	d->in  = (UB *)knl_usb_page_alloc(&d->in_bus);
	if ( d->out == NULL || d->in == NULL || ring_alloc(&e0->ring, 1) < E_OK ) {
		knl_usb_page_free(d->out);
		knl_usb_page_free(d->in);
		Kfree(e0);
		Kfree(d);
		x_cmd(x, 0, TRB_TYPE(T_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
		return E_NOMEM;
	}
	x->dcbaa[slot * 2]     = (UW)d->out_bus;
	x->dcbaa[slot * 2 + 1] = (UW)(d->out_bus >> 32);
	USB_MB();
	x->slots[slot] = d;
	dev->hcpriv = d;

	/* Address Device: the controller gives it its address at once */
	in_clear(d);
	in_ctx(d, 0)[1] = 0x3;			/* add the slot and endpoint 0 */
	slot_fill(d, in_ctx(d, 1));
	ep0_fill(d, in_ctx(d, 2));
	USB_MB();
	cc = x_cmd(x, d->in_bus, TRB_TYPE(T_ADDRESS_DEV) | TRB_SLOT(slot), NULL);
	if ( cc != CC_SUCCESS ) {
		USB_LOG("usb: xhci%d: Address Device slot %d failed (cc %d)\n",
			hc->index, slot, cc);
		xhci_dev_free(hc, dev);
		return E_IO;
	}
	tk_dly_tsk(2);				/* the address takes a moment */
	dev->addr = (INT)(out_ctx(d, 0)[3] & 0xff);

	return E_OK;
}

/*
 * The slot was addressed in dev_init. What is left is the control
 * packet size, learnt from the first eight bytes of the device
 * descriptor, when it differs from what the speed suggested.
 */
LOCAL ER xhci_dev_mps0( USBHC *hc, USBDEV *dev )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)dev->hcpriv;
	UW	*c;
	INT	cc;

	if ( d == NULL ) {
		return E_NOEXS;
	}
	dev->addr = (INT)(out_ctx(d, 0)[3] & 0xff);
	if ( (INT)(out_ctx(d, 1)[1] >> 16) == dev->mps0 ) {
		return E_OK;
	}
	in_clear(d);
	in_ctx(d, 0)[1] = 0x2;			/* evaluate endpoint 0 */
	c = in_ctx(d, 2);
	knl_memcpy(c, out_ctx(d, 1), x->csz);
	c[1] = (c[1] & 0xffff) | ((UW)dev->mps0 << 16);
	USB_MB();
	cc = x_cmd(x, d->in_bus, TRB_TYPE(T_EVAL_CTX) | TRB_SLOT(d->slot), NULL);
	if ( cc != CC_SUCCESS ) {
		return E_IO;
	}
	d->ep[1]->ep.size = dev->mps0;

	return E_OK;
}

/* A device turned out to be a hub: the controller is told, and how many ports */
LOCAL ER xhci_hub_config( USBHC *hc, USBDEV *hub )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)hub->hcpriv;
	UW	*s;
	INT	cc;

	if ( d == NULL ) {
		return E_NOEXS;
	}
	in_clear(d);
	in_ctx(d, 0)[1] = 0x1;
	slot_from_output(d);
	s = in_ctx(d, 1);
	s[0] |= 1UL << 26;
	s[1] = (s[1] & 0x00ffffff) | ((UW)hub->hub_ports << 24);
	USB_MB();
	cc = x_cmd(x, d->in_bus, TRB_TYPE(T_CONFIG_EP) | TRB_SLOT(d->slot), NULL);

	return ( cc == CC_SUCCESS ) ? E_OK : E_IO;
}

/* Add an endpoint to the slot's configuration */
LOCAL ER ep_configure( XDEV *d, XEP *e )
{
	INT	cc;

	in_clear(d);
	in_ctx(d, 0)[1] = 0x1 | (1UL << e->dci);
	slot_from_output(d);
	ep_fill(d, e, in_ctx(d, 1 + e->dci));
	USB_MB();
	cc = x_cmd(d->x, d->in_bus, TRB_TYPE(T_CONFIG_EP) | TRB_SLOT(d->slot), NULL);

	return ( cc == CC_SUCCESS ) ? E_OK : E_IO;
}

/*
 * Take an endpoint out of the slot's configuration. TRUE when the
 * controller confirmed it, which means it has let go of the ring.
 */
LOCAL BOOL ep_drop( XDEV *d, INT dci )
{
	d->ep[dci] = NULL;
	in_clear(d);
	in_ctx(d, 0)[0] = 1UL << dci;
	in_ctx(d, 0)[1] = 0x1;
	slot_from_output(d);
	USB_MB();

	return x_cmd(d->x, d->in_bus, TRB_TYPE(T_CONFIG_EP) | TRB_SLOT(d->slot),
		     NULL) == CC_SUCCESS;
}

/* The endpoint, configured the first time it is used */
LOCAL XEP *ep_get( XDEV *d, CONST USBEP *ep, INT npg, ER *p_er )
{
	INT	dci = (ep->addr & 15) * 2 + ( ((ep->addr & 0x80) != 0) ? 1 : 0 );
	XEP	*e;
	ER	er;

	*p_er = E_OK;
	if ( d->ep[dci] != NULL ) {
		return d->ep[dci];
	}
	e = (XEP *)Kcalloc(1, sizeof(XEP));
	if ( e == NULL ) {
		*p_er = E_NOMEM;
		return NULL;
	}
	e->dci = dci;
	e->ep  = *ep;
	e->is_bulk = ( ep->type == USB_EP_BULK );
	er = ring_alloc(&e->ring, npg);
	if ( er < E_OK ) {
		Kfree(e);
		*p_er = er;
		return NULL;
	}
	d->ep[dci] = e;
	er = ep_configure(d, e);
	if ( er < E_OK ) {
		/*
		 * A Configure Endpoint that timed out may still be carried
		 * out, and the controller would then read the ring: it goes
		 * back only when the endpoint is confirmed dropped.
		 */
		USB_LOG("usb: slot %d: endpoint %02x not configured\n",
			d->slot, (INT)ep->addr);
		xep_release(e, ep_drop(d, dci));
		*p_er = er;
		return NULL;
	}

	return e;
}

LOCAL void xhci_stream_close( USBHC *hc, USBPIPE *s );

LOCAL void xhci_dev_free( USBHC *hc, USBDEV *dev )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)dev->hcpriv;
	BOOL	gone;
	INT	i;

	for ( i = 0; i < XHCI_PIPES; i++ ) {
		if ( x->pipes[i].open && x->pipes[i].xd == d ) {
			xhci_stream_close(hc, &x->pipes[i]);
		}
	}
	if ( d == NULL ) {
		return;
	}
	/* Disable Slot makes the controller drop the contexts and the rings */
	if ( x->running ) {
		gone = ( x_cmd(x, 0, TRB_TYPE(T_DISABLE_SLOT) | TRB_SLOT(d->slot),
			       NULL) == CC_SUCCESS );
	} else {
		gone = !x->dead;
	}
	x->dcbaa[d->slot * 2] = 0;
	x->dcbaa[d->slot * 2 + 1] = 0;
	x->slots[d->slot] = NULL;
	for ( i = 0; i < 32; i++ ) {
		xep_release(d->ep[i], gone);
	}
	if ( gone ) {
		knl_usb_page_free(d->out);
		knl_usb_page_free(d->in);
		Kfree(d);
	} else {
		USB_LOG("usb: slot %d not disabled: its contexts are kept\n", d->slot);
	}
	dev->hcpriv = NULL;
}

/*
 * Bring a halted or stuck endpoint back: Reset Endpoint if it halted,
 * Stop Endpoint if it still runs, then move where it reads to where the
 * ring is written, dropping whatever was left on it. FALSE when that
 * could not be done: the endpoint then still reads where it did.
 */
LOCAL BOOL ep_recover( XDEV *d, XEP *e, BOOL halted )
{
	XHCI	*x = d->x;
	UW	first = halted ? T_RESET_EP : T_STOP_EP;
	UW	other = halted ? T_STOP_EP : T_RESET_EP;
	INT	cc;

	cc = x_cmd(x, 0, TRB_TYPE(first) | TRB_EP(e->dci) | TRB_SLOT(d->slot), NULL);
	if ( cc != CC_SUCCESS ) {
		/* the controller sees the endpoint in the other state */
		x_cmd(x, 0, TRB_TYPE(other) | TRB_EP(e->dci) | TRB_SLOT(d->slot), NULL);
	}
	cc = x_cmd(x, ring_bus_of(&e->ring, e->ring.enq) | e->ring.pcs,
		   TRB_TYPE(T_SET_DEQ) | TRB_EP(e->dci) | TRB_SLOT(d->slot), NULL);
	if ( cc != CC_SUCCESS ) {
		USB_LOG("usb: slot %d endpoint %d: dequeue pointer not set (cc %d)\n",
			d->slot, e->dci, cc);
		return FALSE;
	}

	return TRUE;
}

/* ---------------------------------------------------------------- transfers */

/* TD Size: the packets still to come after this entry */
LOCAL UW td_size( INT total, INT done_incl, INT mps, BOOL last )
{
	INT	n;

	if ( last || mps <= 0 ) {
		return 0;
	}
	n = (total + mps - 1) / mps - (done_incl + mps - 1) / mps;
	if ( n < 0 ) n = 0;
	if ( n > 31 ) n = 31;

	return (UW)n << 17;
}

/*
 * A data stage of len bytes from b: an entry per page, the first of
 * type first_type, chained. With hold the first entry is left for
 * ring_release(); answers where it is.
 */
LOCAL INT queue_data( XRING *r, DMABUF *b, INT len, INT mps, UW first_type,
		      UW dir, UW gen, INT td, UW last_flags, BOOL hold, UW *cyc )
{
	INT	off = 0, k = 0, first = r->enq;

	while ( off < len ) {
		INT	n = len - off;
		BOOL	last;
		TRBINFO	inf;
		UW	ctl;

		if ( n > USB_PAGE ) {
			n = USB_PAGE;
		}
		last = ( off + n >= len );
		inf.td = td; inf.gen = gen; inf.off = off; inf.len = n; inf.last = last;
		ctl = TRB_TYPE(( k == 0 ) ? first_type : T_NORMAL) | TRB_ISP
		    | (( k == 0 ) ? dir : 0) | ( last ? last_flags : TRB_CH );
		ring_put(r, b->bus[k], (UW)n | td_size(len, off + n, mps, last), ctl,
			 &inf, hold && k == 0, cyc);
		off += n;
		k++;
	}

	return first;
}

LOCAL ER cc_to_er( INT cc )
{
	switch ( cc ) {
	case CC_SUCCESS:	return E_OK;
	case CC_STALL:		return E_OBJ;
	default:		return E_IO;
	}
}

LOCAL ER xhci_control( USBHC *hc, USBDEV *dev, CONST UB *setup, UB *data,
		       INT *actual, TMO tmo )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)dev->hcpriv;
	XEP	*e;
	XRING	*r;
	INT	len = GET16(setup + 6), first;
	BOOL	in = ( (setup[0] & USB_RT_IN) != 0 );
	TRBINFO	inf;
	UW	cyc = 0, trt;
	UD	req;
	ER	er;

	if ( actual != NULL ) {
		*actual = 0;
	}
	if ( d == NULL || (e = d->ep[1]) == NULL || !x->running ) {
		return E_NOEXS;
	}
	if ( len > CTRL_BUF_BYTES ) {
		return E_PAR;
	}
	sem_take(x->xfer_sem);
	if ( !in && len > 0 ) {
		knl_dmabuf_write(&x->ctrl_buf, 0, data, len);
	}

	r = &e->ring;
	e->gen++;
	e->done = FALSE;
	e->cc = 0;
	e->actual = len;
	e->is_bulk = FALSE;
	trt = ( len == 0 ) ? 0 : ( in ? 3 : 2 );
	req = (UD)GET32(setup) | ((UD)GET32(setup + 4) << 32);

	/* the request, carried in the entry itself rather than by address */
	inf.td = TI_SETUP; inf.gen = e->gen; inf.off = 0; inf.len = 0; inf.last = FALSE;
	first = ring_put(r, req, 8, TRB_TYPE(T_SETUP) | TRB_IDT | (trt << 16),
			 &inf, TRUE, &cyc);
	if ( len > 0 ) {
		queue_data(r, &x->ctrl_buf, len, dev->mps0, T_DATA,
			   in ? TRB_DIR_IN : 0, e->gen, TI_DATA, 0, FALSE, NULL);
	}
	/* the handshake goes the other way from the data */
	inf.td = TI_STATUS; inf.last = TRUE;
	ring_put(r, 0, 0, TRB_TYPE(T_STATUS) | TRB_IOC
		 | (( in && len > 0 ) ? 0 : TRB_DIR_IN), &inf, FALSE, NULL);
	ring_release(r, first, cyc);
	doorbell(x, d->slot, 1);

	if ( !wait_for(x, &e->done, tmo) ) {
		USB_LOG("usb: control %02x %02x slot %d timed out\n",
			setup[0], setup[1], d->slot);
		e->gen++;
		ep_recover(d, e, FALSE);
		er = E_TMOUT;
	} else if ( e->cc != CC_SUCCESS ) {
		e->gen++;
		ep_recover(d, e, TRUE);
		er = cc_to_er(e->cc);
	} else {
		er = E_OK;
		if ( in && len > 0 && data != NULL ) {
			knl_dmabuf_read(&x->ctrl_buf, 0, data, e->actual);
		}
		if ( actual != NULL ) {
			*actual = ( len > 0 ) ? e->actual : 0;
		}
	}
	sem_give(x->xfer_sem);

	return er;
}

LOCAL ER xhci_bulk( USBHC *hc, USBDEV *dev, CONST USBEP *ep, UB *data, INT len,
		    INT *actual, TMO tmo )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)dev->hcpriv;
	XEP	*e;
	XRING	*r;
	BOOL	in = ( (ep->addr & 0x80) != 0 );
	ER	er;

	if ( actual != NULL ) {
		*actual = 0;
	}
	if ( d == NULL || !x->running ) {
		return E_NOEXS;
	}
	if ( len < 0 || len > BULK_BUF_BYTES ) {
		return E_PAR;
	}
	sem_take(x->xfer_sem);
	e = ep_get(d, ep, 1, &er);
	if ( e == NULL ) {
		sem_give(x->xfer_sem);
		return er;
	}
	if ( e->pipe != NULL ) {
		sem_give(x->xfer_sem);
		return E_BUSY;			/* the endpoint carries a stream */
	}
	if ( !in && len > 0 ) {
		knl_dmabuf_write(&x->bulk_buf, 0, data, len);
	}
	r = &e->ring;
	e->gen++;
	e->done = FALSE;
	e->cc = 0;
	e->actual = 0;
	e->is_bulk = TRUE;
	if ( len == 0 ) {
		TRBINFO	inf;

		inf.td = TI_DATA; inf.gen = e->gen; inf.off = 0; inf.len = 0; inf.last = TRUE;
		ring_put(r, x->bulk_buf.bus[0], 0, TRB_TYPE(T_NORMAL) | TRB_IOC, &inf,
			 FALSE, NULL);
	} else {
		/* the first entry is handed over once the whole TD is written */
		UW	cyc = 0;
		INT	first = queue_data(r, &x->bulk_buf, len, ep->size, T_NORMAL, 0,
					   e->gen, TI_DATA, TRB_IOC, TRUE, &cyc);

		ring_release(r, first, cyc);
	}
	doorbell(x, d->slot, (UW)e->dci);

	if ( !wait_for(x, &e->done, tmo) ) {
		USB_LOG("usb: bulk %02x of %d bytes timed out\n", (INT)ep->addr, len);
		e->gen++;
		ep_recover(d, e, FALSE);
		er = E_TMOUT;
	} else if ( e->cc != CC_SUCCESS ) {
		e->gen++;
		ep_recover(d, e, TRUE);
		er = cc_to_er(e->cc);
	} else {
		er = E_OK;
		if ( in && e->actual > 0 && data != NULL ) {
			knl_dmabuf_read(&x->bulk_buf, 0, data, e->actual);
		}
		if ( actual != NULL ) {
			*actual = e->actual;
		}
	}
	sem_give(x->xfer_sem);

	return er;
}

/* ---------------------------------------------------------------- ports */

LOCAL INT xhci_nports( USBHC *hc )
{
	return ((XHCI *)hc)->nports;
}

LOCAL BOOL xhci_port_connected( USBHC *hc, INT port )
{
	return ( (rd32(((XHCI *)hc)->op, OP_PORTSC(port)) & PS_CCS) != 0 );
}

LOCAL BOOL xhci_port_changed( USBHC *hc, INT port )
{
	XHCI	*x = (XHCI *)hc;
	UW	v = rd32(x->op, OP_PORTSC(port));

	if ( (v & PS_CHANGES) == 0 ) {
		return FALSE;
	}
	/* the change bits are cleared by writing them back */
	wr32(x->op, OP_PORTSC(port), (v & PS_NEUTRAL) | (v & PS_CHANGES));

	return ( (v & PS_CSC) != 0 );
}

LOCAL ER xhci_port_state( USBHC *hc, INT port, T_USBPORT *p )
{
	XHCI	*x = (XHCI *)hc;
	UW	v;

	if ( port < 0 || port >= x->nports ) {
		return E_PAR;
	}
	v = rd32(x->op, OP_PORTSC(port));
	p->connected = ( (v & PS_CCS) != 0 ) ? 1 : 0;
	p->enabled   = ( (v & PS_PED) != 0 ) ? 1 : 0;
	p->powered   = ( (v & PS_PP) != 0 ) ? 1 : 0;
	p->speed     = ( p->enabled ) ? PS_SPEED(v) : USB_SPEED_NONE;
	p->changed   = ( (v & PS_CHANGES) != 0 ) ? 1 : 0;
	p->usb3      = ( x->port_major[port] == 3 ) ? 1 : 0;

	return E_OK;
}

/*
 * Reset a port. A USB 3 port resets itself when something is plugged in
 * and is enabled by the time anyone looks; a USB 2 port has to be told
 * to, and is only enabled once that finishes.
 */
LOCAL ER xhci_port_reset( USBHC *hc, INT port, INT *speed )
{
	XHCI	*x = (XHCI *)hc;
	UW	v = rd32(x->op, OP_PORTSC(port));
	INT	i;

	if ( (v & PS_CCS) == 0 ) {
		return E_NOEXS;
	}
	if ( !( x->port_major[port] == 3 && (v & PS_PED) != 0 ) ) {
		wr32(x->op, OP_PORTSC(port), (v & PS_NEUTRAL) | PS_PR);
		for ( i = 0; i < 50; i++ ) {
			knl_usb_wait(10);
			/* half a second of events fits in no ring */
			events_service(x);
			v = rd32(x->op, OP_PORTSC(port));
			if ( (v & PS_PRC) != 0 || (v & PS_PR) == 0 ) {
				break;
			}
		}
		wr32(x->op, OP_PORTSC(port), (v & PS_NEUTRAL) | (v & PS_CHANGES));
		knl_usb_wait(10);			/* reset recovery */
		v = rd32(x->op, OP_PORTSC(port));
	}
	if ( (v & PS_PED) == 0 ) {
		USB_LOG("usb: xhci%d port %d not enabled after reset (%08x)\n",
			hc->index, port, v);
		return E_IO;
	}
	*speed = (INT)PS_SPEED(v);
	if ( *speed < USB_SPEED_FULL || *speed > USB_SPEED_SUPER_PLUS ) {
		*speed = USB_SPEED_SUPER;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- streams */

LOCAL INT iso_tbc( XHCI *x, USBPIPE *s, INT packets, INT *tlbpc )
{
	INT	mb = s->ep.burst;

	if ( x->version < 0x100 || s->xd->dev->speed < USB_SPEED_SUPER ) {
		*tlbpc = ( x->version < 0x100 || packets == 0 ) ? 0 : packets - 1;
		return 0;
	}
	*tlbpc = ( packets % (mb + 1) == 0 ) ? mb : (packets % (mb + 1)) - 1;

	return (packets + mb) / (mb + 1) - 1;
}

/* Arm TD i where the ring is written next */
LOCAL void pipe_arm( USBPIPE *s, INT i )
{
	XTD	*t = &s->td[i];
	XRING	*r = &s->xe->ring;
	INT	off = 0, first = -1, total = t->total;
	UW	cyc = 0;
	INT	mps = ( s->ep.size > 0 ) ? s->ep.size : 512;

	if ( s->out ) {
		/* what to send goes into the TD's buffer; nothing yet: it waits */
		INT	n = s->fill(s->ctx, s->out_buf, t->total);

		if ( n <= 0 ) {
			return;
		}
		if ( n > t->total ) {
			n = t->total;
		}
		knl_dmabuf_write(t->b, t->off, s->out_buf, n);
		total = n;
	}
	t->gen++;
	t->pending = TRUE;
	s->order[(s->o_head + s->o_count) % s->ntd] = i;
	s->o_count++;
	while ( off < total ) {
		INT	pg = (t->off + off) / USB_PAGE;
		INT	po = (t->off + off) % USB_PAGE;
		INT	n = USB_PAGE - po;
		BOOL	last;
		UW	ctl;
		TRBINFO	inf;
		INT	idx;

		if ( n > total - off ) {
			n = total - off;
		}
		last = ( off + n >= total );
		if ( off == 0 && s->iso ) {
			INT	pk = (total + mps - 1) / mps, tlbpc;
			INT	tbc = iso_tbc(s->x, s, pk, &tlbpc);

			ctl = TRB_TYPE(T_ISOCH) | TRB_SIA | ((UW)tbc << 7)
			    | ((UW)tlbpc << 16);
		} else {
			ctl = TRB_TYPE(T_NORMAL);
		}
		ctl |= ( s->out ? 0 : TRB_ISP ) | ( last ? TRB_IOC : TRB_CH );
		if ( last && s->iso && (i % s->irq_every) != s->irq_every - 1 ) {
			ctl |= TRB_BEI;
		}
		inf.td = i; inf.gen = t->gen; inf.off = off; inf.len = n; inf.last = last;
		idx = ring_put(r, t->b->bus[pg] + (UD)po,
			       (UW)n | td_size(total, off + n, mps, last), ctl, &inf,
			       off == 0, ( off == 0 ) ? &cyc : NULL);
		if ( off == 0 ) {
			first = idx;
		}
		off += n;
	}
	if ( first >= 0 ) {
		ring_release(r, first, cyc);
	}
	s->ring = TRUE;
}

/*
 * TD i is done: it leaves the queue of armed ones, and the TDs armed
 * before it, which the controller skipped without an event, are given
 * up as missed and armed again.
 */
LOCAL void td_retire( USBPIPE *s, INT i )
{
	while ( s->o_count > 0 ) {
		INT	k = s->order[s->o_head];

		s->o_head = (s->o_head + 1) % s->ntd;
		s->o_count--;
		if ( k == i ) {
			return;
		}
		if ( !s->td[k].pending ) {
			continue;
		}
		s->td[k].pending = FALSE;
		knl_usb_stat.iso_missed++;
		if ( !s->out ) {
			s->fn(s->ctx, s->td[k].b, s->td[k].off, 0, 1);
		}
		if ( s->open && !s->halted ) {
			pipe_arm(s, k);
		}
	}
}

LOCAL void stream_event( USBPIPE *s, INT idx, UW st )
{
	INT	cc = (INT)(st >> 24), res = (INT)(st & 0xffffff), ti;
	TRBINFO	*inf;
	XTD	*t;
	INT	len, err = 0;

	if ( cc == CC_UNDERRUN || cc == CC_OVERRUN ) {
		/*
		 * IN: the ring ran dry, kick it again. OUT: only new TDs need
		 * the doorbell; ringing it for the event itself makes the
		 * controller look at the empty ring and report it again.
		 */
		if ( s->out ) {
			knl_usb_stat.out_underruns++;
		} else {
			s->ring = TRUE;
		}
		return;
	}
	if ( idx < 0 || !s->open ) {
		return;
	}
	inf = &s->xe->ring.info[idx];
	if ( inf->td < 0 || inf->td >= s->ntd ) {
		return;
	}
	t = &s->td[inf->td];
	if ( !t->pending || inf->gen != t->gen ) {
		return;
	}
	if ( cc == CC_STOPPED || cc == CC_STOPPED_LEN || cc == CC_STOPPED_SHORT ) {
		return;
	}
	if ( cc != CC_SUCCESS && cc != CC_SHORT && !inf->last && cc != CC_MISSED
	  && s->iso ) {
		return;			/* the TD's last entry will answer */
	}
	ti = inf->td;
	td_retire(s, ti);
	if ( s->out ) {
		/* sent, or missed: refilled and queued again */
		knl_usb_stat.iso_packets++;
		if ( cc == CC_MISSED ) {
			knl_usb_stat.iso_missed++;
		} else if ( cc != CC_SUCCESS ) {
			knl_usb_stat.iso_errors++;
		}
		t->pending = FALSE;
		if ( s->open ) {
			pipe_arm(s, ti);
		}
		return;
	}

	len = inf->off + inf->len - res;
	if ( len < 0 ) {
		len = 0;
	}
	if ( s->iso ) {
		knl_usb_stat.iso_packets++;
	}
	if ( cc == CC_MISSED ) {
		knl_usb_stat.iso_missed++;
		err = 1;
		len = 0;
	} else if ( cc != CC_SUCCESS && cc != CC_SHORT ) {
		err = cc;
		if ( s->iso ) {
			knl_usb_stat.iso_errors++;
		} else {
			knl_usb_stat.stream_errors++;
			s->halted = TRUE;	/* a bulk or interrupt endpoint halted */
		}
	} else {
		s->halts = 0;			/* the endpoint works again */
	}
	t->pending = FALSE;
	s->fn(s->ctx, t->b, t->off, len, err);
	if ( s->open && !s->halted ) {
		pipe_arm(s, ti);
	}
}

/*
 * keep_bufs: the endpoint was not confirmed stopped, so its ring is
 * still there and the entries on it point at these buffers. The
 * controller may write into them at any time, so they are left with it.
 */
LOCAL void pipe_free( USBPIPE *s, BOOL keep_bufs )
{
	INT	i;

	if ( s->bufs != NULL && keep_bufs ) {
		s->bufs  = NULL;
		s->nbufs = 0;
	}
	if ( s->bufs != NULL ) {
		for ( i = 0; i < s->nbufs; i++ ) {
			knl_dmabuf_free(&s->bufs[i]);
		}
		Kfree(s->bufs);
		s->bufs = NULL;
	}
	if ( s->td != NULL ) {
		Kfree(s->td);
		s->td = NULL;
	}
	if ( s->order != NULL ) {
		Kfree(s->order);
		s->order = NULL;
	}
	if ( s->out_buf != NULL ) {
		Kfree(s->out_buf);
		s->out_buf = NULL;
	}
	s->nbufs = 0;
	s->ntd = 0;
	s->o_head = s->o_count = 0;
}

/*
 * The TDs and their buffers. A TD never crosses a page unless it is
 * larger than one, and then it takes whole pages, an entry each.
 */
LOCAL ER pipe_buffers( USBPIPE *s )
{
	INT	per_page, pages, i, trbs;

	trbs = (s->td_bytes + USB_PAGE - 1) / USB_PAGE;
	s->irq_every = ISO_IRQ_EVERY;
	if ( s->iso ) {
		s->ntd = (ring_capacity(&s->xe->ring) - 16) / trbs;
		if ( s->ntd > ISO_NTD_MAX ) {
			s->ntd = ISO_NTD_MAX;
		}
		if ( s->out && s->ntd > ISO_OUT_NTD ) {
			s->ntd = ISO_OUT_NTD;
		}
	} else {
		s->ntd = ( s->ep.type == USB_EP_INTR ) ? INTR_NTD : BULK_NTD;
	}
	s->ntd -= s->ntd % s->irq_every;
	if ( s->ntd <= 0 ) {
		return E_PAR;
	}
	s->td = (XTD *)Kcalloc((SZ)s->ntd, sizeof(XTD));
	s->order = (INT *)Kcalloc((SZ)s->ntd, sizeof(INT));
	if ( s->td == NULL || s->order == NULL ) {
		return E_NOMEM;
	}
	if ( s->out ) {
		s->out_buf = (UB *)Kmalloc((SZ)s->td_bytes);
		if ( s->out_buf == NULL ) {
			return E_NOMEM;
		}
	}
	if ( s->td_bytes <= USB_PAGE ) {
		per_page = USB_PAGE / s->td_bytes;
		s->nbufs = (s->ntd + per_page - 1) / per_page;
		pages = 1;
	} else {
		per_page = 1;
		s->nbufs = s->ntd;
		pages = trbs;
	}
	if ( pages > DMABUF_PAGES ) {
		return E_PAR;
	}
	s->bufs = (DMABUF *)Kcalloc((SZ)s->nbufs, sizeof(DMABUF));
	if ( s->bufs == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < s->nbufs; i++ ) {
		if ( knl_dmabuf_alloc(&s->bufs[i], pages * USB_PAGE) < E_OK ) {
			s->nbufs = i;
			return E_NOMEM;
		}
	}
	for ( i = 0; i < s->ntd; i++ ) {
		s->td[i].b     = &s->bufs[i / per_page];
		s->td[i].off   = (i % per_page) * s->td_bytes;
		s->td[i].total = s->td_bytes;
	}

	return E_OK;
}

/*
 * IN streams (isochronous, bulk, interrupt) with a completion callback,
 * or isochronous OUT streams with a fill callback
 */
LOCAL ER open_stream( USBHC *hc, USBDEV *dev, CONST USBEP *ep, INT xfer,
		      USB_XFERFN fn, USB_FILLFN fill, void *ctx, USBPIPE **out )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d = (XDEV *)dev->hcpriv;
	USBPIPE	*s = NULL;
	INT	i, dci;
	ER	er;
	BOOL	is_out = ( fill != NULL );

	if ( d == NULL || !x->running ) {
		return E_NOEXS;
	}
	if ( ep->type == USB_EP_CONTROL
	  || ( is_out ? ( (ep->addr & 0x80) != 0 || ep->type != USB_EP_ISOC )
		      : (ep->addr & 0x80) == 0 ) ) {
		return E_NOSPT;
	}
	for ( i = 0; i < XHCI_PIPES; i++ ) {
		if ( !x->pipes[i].open && x->pipes[i].xe == NULL ) {
			s = &x->pipes[i];
			break;
		}
	}
	if ( s == NULL ) {
		return E_LIMIT;
	}
	knl_memset(s, 0, sizeof(*s));
	s->x    = x;
	s->xd   = d;
	s->ep   = *ep;
	s->fn   = fn;
	s->fill = fill;
	s->out  = is_out;
	s->ctx  = ctx;
	s->iso  = ( ep->type == USB_EP_ISOC );
	s->td_bytes = s->iso ? ep->esit : xfer;
	if ( s->td_bytes <= 0 || s->td_bytes > DMABUF_PAGES * USB_PAGE ) {
		return E_PAR;
	}

	sem_take(x->xfer_sem);
	dci = (ep->addr & 15) * 2 + ( is_out ? 0 : 1 );
	if ( d->ep[dci] != NULL && d->ep[dci]->pipe != NULL ) {
		sem_give(x->xfer_sem);
		return E_BUSY;			/* the endpoint carries a stream */
	}
	if ( d->ep[dci] != NULL ) {
		/* an endpoint used synchronously before: rebuilt for the stream */
		XEP	*old = d->ep[dci];

		x_cmd(x, 0, TRB_TYPE(T_STOP_EP) | TRB_EP(dci) | TRB_SLOT(d->slot), NULL);
		xep_release(old, ep_drop(d, dci));
	}
	s->xe = ep_get(d, ep, s->iso ? RING_MAXPG : 1, &er);
	if ( s->xe == NULL ) {
		sem_give(x->xfer_sem);
		return er;
	}
	er = pipe_buffers(s);
	if ( er < E_OK ) {
		BOOL	stopped = ep_drop(d, s->xe->dci);

		pipe_free(s, FALSE);		/* no TD was armed yet */
		xep_release(s->xe, stopped);
		s->xe = NULL;
		sem_give(x->xfer_sem);
		return er;
	}
	sem_take(x->ev_sem);
	s->open = TRUE;
	s->xe->pipe = s;
	for ( i = 0; i < s->ntd; i++ ) {
		pipe_arm(s, i);
	}
	s->ring = FALSE;
	sem_give(x->ev_sem);
	doorbell(x, d->slot, (UW)s->xe->dci);
	sem_give(x->xfer_sem);
	*out = s;

	return E_OK;
}

LOCAL ER xhci_stream_open( USBHC *hc, USBDEV *dev, CONST USBEP *ep, INT xfer,
			   USB_XFERFN fn, void *ctx, USBPIPE **out )
{
	return open_stream(hc, dev, ep, xfer, fn, NULL, ctx, out);
}

LOCAL ER xhci_stream_open_out( USBHC *hc, USBDEV *dev, CONST USBEP *ep,
			       USB_FILLFN fill, void *ctx, USBPIPE **out )
{
	return open_stream(hc, dev, ep, 0, NULL, fill, ctx, out);
}

LOCAL void xhci_stream_close( USBHC *hc, USBPIPE *s )
{
	XHCI	*x = (XHCI *)hc;
	XDEV	*d;
	XEP	*e;
	INT	dci;
	BOOL	stopped;

	if ( s == NULL || !s->open ) {
		return;
	}
	d = s->xd;
	e = s->xe;
	dci = e->dci;
	sem_take(x->xfer_sem);
	sem_take(x->ev_sem);
	s->open = FALSE;
	sem_give(x->ev_sem);
	if ( x->running ) {
		/*
		 * Stop Endpoint is refused for an endpoint that halted or
		 * stopped already, so its answer is not what counts: a drop
		 * that succeeds is the controller letting the ring go.
		 */
		x_cmd(x, 0, TRB_TYPE(T_STOP_EP) | TRB_EP(dci) | TRB_SLOT(d->slot), NULL);
		stopped = ep_drop(d, dci);
	} else {
		d->ep[dci] = NULL;
		stopped = !x->dead;
	}
	e->pipe = NULL;
	xep_release(e, stopped);
	pipe_free(s, !stopped);
	s->xe = NULL;
	sem_give(x->xfer_sem);
}

/*
 * The service the manager's task calls: a controller that failed is
 * given up, finished transfers are taken, OUT streams are offered their
 * idle TDs again, and streams that halted are brought back.
 */
LOCAL void xhci_poll( USBHC *hc )
{
	XHCI	*x = (XHCI *)hc;
	INT	i;
	UW	sts;

	if ( !x->running ) {
		return;
	}
	if ( hc->intno == 0 ) {
		UW	iman = rd32(x->rt, RT_IMAN);

		sts = rd32(x->op, OP_USBSTS);
		if ( (sts & STS_W1C) != 0 ) {
			wr32(x->op, OP_USBSTS, sts & STS_W1C);
		}
		if ( (iman & IMAN_IP) != 0 ) {
			wr32(x->rt, RT_IMAN, iman);
		}
	}
	sts = rd32(x->op, OP_USBSTS);
	if ( (sts & (STS_HCE | STS_HSE)) != 0 ) {
		/* every command and transfer from here on would time out */
		USB_LOG("usb: xhci%d: controller error, sts %08x; given up\n",
			hc->index, sts);
		wr32(x->rt, RT_IMAN, IMAN_IP);
		wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~(CMD_RS | CMD_INTE));
		x->dead = !wait_reg(x->op, OP_USBSTS, STS_HCH, STS_HCH, 20);
		x->running = FALSE;
		hc->lost = TRUE;
		return;
	}
	if ( (sts & STS_PCD) != 0 ) {
		tk_set_flg(hc->flgid, USBF_RESCAN);
	}
	events_service(x);

	/* OUT streams: the idle TDs are offered again, data may have come */
	for ( i = 0; i < XHCI_PIPES; i++ ) {
		USBPIPE	*s = &x->pipes[i];
		INT	k;
		BOOL	ring;

		if ( !s->open || !s->out ) {
			continue;
		}
		sem_take(x->ev_sem);
		for ( k = 0; k < s->ntd; k++ ) {
			if ( !s->td[k].pending ) {
				pipe_arm(s, k);
				if ( !s->td[k].pending ) {
					break;		/* nothing more to send now */
				}
			}
		}
		ring = s->ring;
		s->ring = FALSE;
		sem_give(x->ev_sem);
		if ( ring ) {
			doorbell(x, s->xd->slot, (UW)s->xe->dci);
		}
	}

	for ( i = 0; i < XHCI_PIPES; i++ ) {
		USBPIPE	*s = &x->pipes[i];
		INT	k;
		BOOL	ok;

		if ( !s->open || !s->halted ) {
			continue;
		}
		/* a device that stalls again at once is left alone */
		if ( ++s->halts > STALL_MAX ) {
			if ( s->halts == STALL_MAX + 1 ) {
				USB_LOG("usb: stream %02x keeps stalling, given up\n",
					(INT)s->ep.addr);
			}
			s->halted = FALSE;
			continue;
		}
		sem_take(x->xfer_sem);
		ok = ep_recover(s->xd, s->xe, TRUE);
		if ( ok ) {
			/*
			 * Where the endpoint reads moved past every TD armed,
			 * so their events are stale from here on.
			 */
			sem_take(x->ev_sem);
			s->o_head = s->o_count = 0;
			for ( k = 0; k < s->ntd; k++ ) {
				s->td[k].pending = FALSE;
				s->td[k].gen++;
			}
			sem_give(x->ev_sem);
		}
		sem_give(x->xfer_sem);
		if ( !ok ) {
			s->halted = FALSE;
			continue;
		}
		/* the device keeps its own halt until it is cleared */
		knl_usb_clear_halt(s->xd->dev, s->ep.addr);
		sem_take(x->xfer_sem);
		sem_take(x->ev_sem);
		s->halted = FALSE;
		if ( !x->running || !s->open ) {
			sem_give(x->ev_sem);
			sem_give(x->xfer_sem);
			continue;
		}
		for ( k = 0; k < s->ntd; k++ ) {
			pipe_arm(s, k);
		}
		s->ring = FALSE;
		sem_give(x->ev_sem);
		doorbell(x, s->xd->slot, (UW)s->xe->dci);
		sem_give(x->xfer_sem);
	}
}

LOCAL void xhci_info( USBHC *hc, T_USBHC *info )
{
	XHCI	*x = (XHCI *)hc;

	info->version  = x->version;
	info->nports   = (UINT)x->nports;
	info->nslots   = (UINT)x->max_slots;
	info->ctx_size = (UINT)x->csz;
	info->nscratch = (UINT)x->nsp;
	info->running  = ( x->running ) ? 1 : 0;
	info->intno    = hc->intno;
	info->mmio     = x->mmio;
}

/* ---------------------------------------------------------------- start-up */

/* The extended capability id after from (NULL: the first), NULL if none */
LOCAL UBINT xcap_find( XHCI *x, INT id, UBINT from )
{
	UW	off = HCC1_XECP(x->hcc1);
	INT	guard = 0;

	if ( from != 0 ) {
		UW	v = rd32(from, 0);

		if ( ((v >> 8) & 0xff) == 0 ) {
			return 0;
		}
		off = (UW)(from - x->cap) + (((v >> 8) & 0xff) << 2);
	}
	while ( off != 0 && (UD)off + 16 <= x->mmio_size && guard++ < 64 ) {
		UBINT	c = x->cap + off;
		UW	v = rd32(c, 0);

		if ( (INT)(v & 0xff) == id ) {
			return c;
		}
		if ( ((v >> 8) & 0xff) == 0 ) {
			break;
		}
		off += ((v >> 8) & 0xff) << 2;
	}

	return 0;
}

/*
 * Take the controller from firmware that was using it, and learn which
 * root ports are USB 2 and which USB 3.
 */
LOCAL void ext_caps( XHCI *x )
{
	UBINT	c;

	c = xcap_find(x, XCAP_LEGACY, 0);
	if ( c != 0 ) {
		if ( (rd32(c, 0) & LEG_BIOS) != 0 ) {
			wr32(c, 0, rd32(c, 0) | LEG_OS);
			if ( !wait_reg(c, 0, LEG_BIOS, 0, 1000) ) {
				wr32(c, 0, rd32(c, 0) & ~LEG_BIOS);
			}
		}
		/* its interrupts to firmware off, what it latched cleared */
		wr32(c, 4, (rd32(c, 4) & LEGCTL_RSVDP) | LEGCTL_SMI_STS);
	}
	for ( c = xcap_find(x, XCAP_PROTOCOL, 0); c != 0;
	      c = xcap_find(x, XCAP_PROTOCOL, c) ) {
		UW	v0 = rd32(c, 0), v2 = rd32(c, 8);
		INT	major = (INT)(v0 >> 24), first = (INT)(v2 & 0xff);
		INT	cnt = (INT)((v2 >> 8) & 0xff), p;

		for ( p = first; p < first + cnt; p++ ) {
			if ( p >= 1 && p <= MAX_ROOTPORTS ) {
				x->port_major[p - 1] = (UB)major;
			}
		}
	}
}

LOCAL void xhci_free( XHCI *x )
{
	INT	i;

	if ( x->cmd_sem > 0 ) tk_del_sem(x->cmd_sem);
	if ( x->xfer_sem > 0 ) tk_del_sem(x->xfer_sem);
	if ( x->ev_sem > 0 ) tk_del_sem(x->ev_sem);
	if ( x->ev_flg > 0 ) tk_del_flg(x->ev_flg);
	ring_free(&x->cmd);
	for ( i = 0; i < EV_SEGS; i++ ) {
		knl_usb_page_free(x->ev_seg[i]);
	}
	knl_usb_page_free(x->erst);
	for ( i = 0; i < x->nsp; i++ ) {
		knl_usb_page_free(x->sp_page[i]);
	}
	knl_usb_page_free(x->sp_array);
	knl_usb_page_free(x->dcbaa);
	knl_dmabuf_free(&x->ctrl_buf);
	knl_dmabuf_free(&x->bulk_buf);
	Kfree(x);
}

LOCAL CONST USBHC_OPS xhci_ops = {
	xhci_nports,
	xhci_port_connected,
	xhci_port_changed,
	xhci_port_reset,
	xhci_port_state,
	xhci_dev_init,
	xhci_dev_mps0,
	xhci_hub_config,
	xhci_dev_free,
	xhci_control,
	xhci_bulk,
	xhci_stream_open,
	xhci_stream_open_out,
	xhci_stream_close,
	xhci_poll,
	xhci_info
};

LOCAL ID new_sem( void )
{
	T_CSEM	csem;

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO;
	csem.isemcnt = 1;
	csem.maxsem = 1;

	return tk_cre_sem(&csem);
}

/*
 * Stop the controller, give it its memory, and start it. The order
 * matters: everything it walks has to be in place and written into its
 * registers before it is allowed to run.
 */
LOCAL ER xhci_start( XHCI *x )
{
	T_CFLG	cflg;
	INT	i, n;
	ER	er;

	/* it may still be waking up from the reset the bus gave it */
	wait_reg(x->op, OP_USBSTS, STS_CNR, 0, 500);
	wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~(CMD_RS | CMD_INTE));
	wait_reg(x->op, OP_USBSTS, STS_HCH, STS_HCH, 50);
	wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) | CMD_HCRST);
	tk_dly_tsk(2);
	if ( !wait_reg(x->op, OP_USBCMD, CMD_HCRST, 0, 1000)
	  || !wait_reg(x->op, OP_USBSTS, STS_CNR, 0, 1000) ) {
		return E_IO;			/* the reset never finished */
	}
	if ( (rd32(x->op, OP_PAGESIZE) & 1) == 0 ) {
		return E_NOSPT;			/* it wants pages of another size */
	}

	x->max_slots = HCS1_SLOTS(x->hcs1);
	if ( x->max_slots > MAX_SLOTS ) {
		x->max_slots = MAX_SLOTS;
	}
	wr32(x->op, OP_CONFIG, (rd32(x->op, OP_CONFIG) & ~0xffU) | (UW)x->max_slots);

	/* the slot array, and the pages the controller asked to borrow */
	x->dcbaa = (UW *)knl_usb_page_alloc(&x->dcbaa_bus);
	if ( x->dcbaa == NULL ) {
		return E_NOMEM;
	}
	n = HCS2_SPBUFS(x->hcs2);
	if ( n > MAX_SCRATCH ) {
		return E_NOSPT;
	}
	if ( n > 0 ) {
		x->sp_array = (UW *)knl_usb_page_alloc(&x->sp_array_bus);
		if ( x->sp_array == NULL ) {
			return E_NOMEM;
		}
		for ( i = 0; i < n; i++ ) {
			UD	bus;

			x->sp_page[i] = (UB *)knl_usb_page_alloc(&bus);
			if ( x->sp_page[i] == NULL ) {
				return E_NOMEM;
			}
			x->nsp = i + 1;
			x->sp_array[i * 2]     = (UW)bus;
			x->sp_array[i * 2 + 1] = (UW)(bus >> 32);
		}
		x->dcbaa[0] = (UW)x->sp_array_bus;
		x->dcbaa[1] = (UW)(x->sp_array_bus >> 32);
	}
	USB_MB();
	wr64(x->op, OP_DCBAAP, x->dcbaa_bus);

	/* the command ring, which starts on cycle 1 */
	er = ring_alloc(&x->cmd, 1);
	if ( er < E_OK ) {
		return er;
	}
	wr64(x->op, OP_CRCR, x->cmd.bus[0] | CRCR_RCS);

	/* the event ring: as many segments as the controller takes */
	x->erst = (UW *)knl_usb_page_alloc(&x->erst_bus);
	if ( x->erst == NULL ) {
		return E_NOMEM;
	}
	x->nevseg = 1 << HCS2_ERSTMAX(x->hcs2);
	if ( x->nevseg > EV_SEGS ) {
		x->nevseg = EV_SEGS;
	}
	for ( i = 0; i < x->nevseg; i++ ) {
		x->ev_seg[i] = (TRB *)knl_usb_page_alloc(&x->ev_bus[i]);
		if ( x->ev_seg[i] == NULL ) {
			return E_NOMEM;
		}
		x->erst[i * 4]     = (UW)x->ev_bus[i];
		x->erst[i * 4 + 1] = (UW)(x->ev_bus[i] >> 32);
		x->erst[i * 4 + 2] = TRB_PER_PAGE;
		x->erst[i * 4 + 3] = 0;
	}
	USB_MB();
	x->ev_seg_idx = 0;
	x->ev_idx = 0;
	x->ccs = 1;
	wr32(x->rt, RT_ERSTSZ, (UW)x->nevseg);
	wr64(x->rt, RT_ERDP, x->ev_bus[0]);
	wr64(x->rt, RT_ERSTBA, x->erst_bus);
	wr32(x->rt, RT_IMOD, 1000);		/* at most one interrupt per 250us */

	er = knl_dmabuf_alloc(&x->ctrl_buf, CTRL_BUF_BYTES);
	if ( er < E_OK ) {
		return er;
	}
	er = knl_dmabuf_alloc(&x->bulk_buf, BULK_BUF_BYTES);
	if ( er < E_OK ) {
		return er;
	}
	x->cmd_sem = new_sem();
	x->xfer_sem = new_sem();
	x->ev_sem = new_sem();
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	x->ev_flg = tk_cre_flg(&cflg);
	if ( x->cmd_sem <= 0 || x->xfer_sem <= 0 || x->ev_sem <= 0 || x->ev_flg <= 0 ) {
		return E_LIMIT;
	}

	x->running = TRUE;
	if ( x->hc.intno != 0 ) {
		knl_define_inthdr((INT)x->hc.intno, TA_HLNG, (FP)knl_xhci_inthdr);
		EnableInt(x->hc.intno, INTPRI_DEVICE);
	}
	wr32(x->rt, RT_IMAN, IMAN_IP | (( x->hc.intno != 0 ) ? IMAN_IE : 0));
	wr32(x->op, OP_USBSTS, STS_W1C);
	wr32(x->op, OP_USBCMD, CMD_RS | CMD_HSEE
				  | (( x->hc.intno != 0 ) ? CMD_INTE : 0));
	if ( !wait_reg(x->op, OP_USBSTS, STS_HCH, 0, 100) ) {
		x->running = FALSE;
		return E_IO;			/* it would not come out of halt */
	}

	/* ports that are powered by software have to be switched on */
	if ( (x->hcc1 & HCC1_PPC) != 0 ) {
		for ( i = 0; i < x->nports; i++ ) {
			UW	v = rd32(x->op, OP_PORTSC(i));

			if ( (v & PS_PP) == 0 ) {
				wr32(x->op, OP_PORTSC(i), (v & PS_NEUTRAL) | PS_PP);
			}
		}
		tk_dly_tsk(20);
	}

	return E_OK;
}

EXPORT ER knl_xhci_attach( UBINT regs, UD size, UD mmio, UINT intno,
			   ID flgid, USBHC **out )
{
	XHCI	*x;
	UW	w;
	UINT	caplen;
	INT	i, slot = -1;
	ER	er;

	for ( i = 0; i < USB_MAX_HC; i++ ) {
		if ( xhci_tbl[i] == NULL ) {
			slot = i;
			break;
		}
	}
	if ( slot < 0 ) {
		return E_LIMIT;
	}
	x = (XHCI *)Kcalloc(1, sizeof(XHCI));
	if ( x == NULL ) {
		return E_NOMEM;
	}
	x->hc.ops   = &xhci_ops;
	x->hc.intno = intno;
	x->hc.flgid = flgid;
	x->mmio     = mmio;
	x->mmio_size = size;
	x->cap      = regs;
	/*
	 * The first two registers share a word. Some controllers only
	 * answer a whole word here, so both come from one read.
	 */
	w = rd32(regs, 0);
	caplen = w & 0xff;
	x->version = (UH)(w >> 16);
	x->op   = regs + caplen;
	x->rt   = regs + (rd32(regs, CAP_RTSOFF) & ~0x1fU);
	x->db   = regs + (rd32(regs, CAP_DBOFF) & ~0x3U);
	x->hcs1 = rd32(regs, CAP_HCSPARAMS1);
	x->hcs2 = rd32(regs, CAP_HCSPARAMS2);
	x->hcc1 = rd32(regs, CAP_HCCPARAMS1);
	x->nports = HCS1_PORTS(x->hcs1);
	x->csz = ( (x->hcc1 & HCC1_CSZ) != 0 ) ? 64 : 32;
	if ( x->nports > MAX_ROOTPORTS ) {
		x->nports = MAX_ROOTPORTS;
	}
	/*
	 * Every register the controller names has to be inside the window
	 * it was given, or reading one would reach outside what is mapped.
	 */
	if ( w == 0xffffffff
	  || (UD)caplen + 0x400 + 0x10 * (UD)x->nports > size
	  || (UD)(rd32(regs, CAP_RTSOFF) & ~0x1fU) + 0x40 > size
	  || (UD)(rd32(regs, CAP_DBOFF) & ~0x3U) + 4 * (MAX_SLOTS + 1) > size ) {
		USB_LOG("usb: xhci at %08x: registers outside its window\n", (UW)mmio);
		Kfree(x);
		return E_NOSPT;
	}

	ext_caps(x);
	xhci_tbl[slot] = x;
	er = xhci_start(x);
	if ( er < E_OK ) {
		USB_LOG("usb: xhci at %08x would not start (%d), sts %08x\n",
			(UW)mmio, (INT)er, rd32(x->op, OP_USBSTS));
		if ( x->hc.intno != 0 ) {
			DisableInt(x->hc.intno);
		}
		xhci_tbl[slot] = NULL;
		wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~(CMD_RS | CMD_INTE));
		if ( wait_reg(x->op, OP_USBSTS, STS_HCH, STS_HCH, 100) ) {
			xhci_free(x);
		}
		return er;
	}
	*out = &x->hc;

	return E_OK;
}
