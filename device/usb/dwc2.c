/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dwc2.c
 *	The DesignWare USB 2 controller behind the USB-C port of the
 *	Raspberry Pi 4 and 5, as a host (design 10.14; docs/tessronos/
 *	03-platform.md 3.6.7). It sits behind the operations of USBHC_OPS,
 *	beside the xHCI, and everything above sees one more root port.
 *
 *	Whether it is used.  No setting is asked of anyone: what is on the
 *	port decides. A little after the system started, the manager's task
 *	turns the block on, makes it a host and looks at the root port for a
 *	while (knl_dwc2_probe_step). With a device there it is brought up and
 *	joins the manager's controllers; with none, the block is put back as
 *	it was found, with no interrupt defined, no memory kept and no
 *	controller added. On most boards this port is where the power comes
 *	in. On the Raspberry Pi 4 being a host for that moment does nothing
 *	there: the port's VBUS is wired to the board's supply with no switch
 *	to drive, its CC lines keep their pull-downs, and nothing is sent on
 *	the data lines unless a device pulls one of them up. The boards whose
 *	circuits are not published are in the design's table.
 *
 *	The device tree is obeyed where it speaks. A node turned on with
 *	dr_mode host is used at once, keeping its hot plugging; dr_mode
 *	anything else is left alone. tessronos.dwc2=off on the command line
 *	and make DWC2=0 leave the block untouched, and tessronos.dwc2=host
 *	uses it at once.
 *
 *	How it moves data.  One channel register block carries one transfer
 *	at a time, by the controller's buffer DMA. The memory it reaches is
 *	given by the dma-ranges of the node's parent: on the BCM2711 the
 *	first 1GB, at bus address 0xC0000000, which the DMA zone lies in; on
 *	the BCM2712 the same as the physical address. Nothing here assumes
 *	it sees the caches: the buffers are the uncached pages of usbdma.c,
 *	and a transfer goes through this driver's buffer, not the caller's.
 *
 *	What needs software.  The controller retries NAKs of control and
 *	bulk transfers itself, but halts the channel on a NAK of an interrupt
 *	endpoint, and it has no schedule: the manager's task asks again when
 *	the endpoint is due. Interrupts wake that task and whoever waits for
 *	a transfer, but everything is decided from the registers, so that a
 *	line that does not arrive slows the transfers down and stops nothing.
 *
 *	The interrupt handler only masks.  Reasons stay in the registers,
 *	unacknowledged, for the one task that holds the manager's lock; the
 *	handler removes a channel's mask, or the port's, so that the line
 *	drops, and wakes it. So there is no state to share with the handler
 *	on another processor, and no lock around it.
 *
 *	Not done: a full or low speed device behind a high speed hub (split
 *	transactions), a low speed one behind a full speed hub (preamble),
 *	and isochronous endpoints. Those are refused when asked for.
 *
 *	NOT VERIFIED ON HARDWARE. Under QEMU's raspi4b it has been run with
 *	an emulated keyboard at both speeds.
 */

#include <sys/machine.h>

#if defined(RPI4) || defined(RPI5)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/usb.h>
#include "sysdepend.h"
#include "usbdev.h"
#include "../../kernel/sysdepend/rpi/rpi_fw.h"

IMPORT BOOL knl_bootarg( CONST UB *args, CONST char *key, UB *val, INT max );

#ifndef CNF_DWC2
#define CNF_DWC2	1
#endif

/* ---------------------------------------------------------------- registers */

#define GOTGCTL		0x000
#define GAHBCFG		0x008
#define  AHB_GLBL	0x00000001		/* the interrupt line is let out */
#define  AHB_BURST	0x00000010		/* the burst this block's integration wants */
#define  AHB_DMA	0x00000020
#define GUSBCFG		0x00c
#define  USB_SRP	0x00000100
#define  USB_HNP	0x00000200
#define  USB_FORCE_HOST	0x20000000
#define  USB_FORCE_DEV	0x40000000
#define GRSTCTL		0x010
#define  RST_CORE	0x00000001
#define  RST_RXFLUSH	0x00000010
#define  RST_TXFLUSH	0x00000020
#define  RST_TXALL	( 0x10 << 6 )		/* the flush takes every FIFO */
#define  RST_AHBIDLE	0x80000000
#define GINTSTS		0x014
#define  GI_HOSTMODE	0x00000001
#define  GI_PORT	0x01000000
#define  GI_CHANNEL	0x02000000
#define GINTMSK		0x018
#define GRXFSIZ		0x024
#define GNPTXFSIZ	0x028
#define GSNPSID		0x040
#define  ID_MASK	0xffff0000
#define  ID_CORE	0x4f540000		/* "OT": the only thing it may read as */
#define GHWCFG1		0x044
#define GHWCFG2		0x048
#define  HW2_ARCH(v)	( ( (v) >> 3 ) & 3 )	/* 2: the DMA inside the core */
#define  HW2_NCH(v)	( ( ( (v) >> 14 ) & 0xf ) + 1 )
#define GHWCFG3		0x04c
#define  HW3_XFERW(v)	( ( (v) & 0xf ) + 11 )	/* bits of a transfer's size */
#define  HW3_PKTW(v)	( ( ( (v) >> 4 ) & 7 ) + 4 )	/* of its packet count */
#define  HW3_DEPTH(v)	( (v) >> 16 )		/* words of FIFO */
#define GHWCFG4		0x050
#define GDFIFOCFG	0x05c
#define HPTXFSIZ	0x100
#define HCFG		0x400
#define  HCFG_CLK	0x00000003		/* 0: the PHY's 30/60 MHz clock */
#define  HCFG_FSONLY	0x00000004
#define  HCFG_DESCDMA	0x00800000
#define HFNUM		0x408
#define HAINT		0x414
#define HAINTMSK	0x418
#define HPRT		0x440
#define  PRT_CONN	0x00000001
#define  PRT_CONNDET	0x00000002		/* written 1 to clear, as are the next two */
#define  PRT_ENA	0x00000004		/* written 1: disabled */
#define  PRT_ENACHG	0x00000008
#define  PRT_OC		0x00000010
#define  PRT_OCCHG	0x00000020
#define  PRT_RST	0x00000100
#define  PRT_PWR	0x00001000
#define  PRT_SPD(v)	( ( (v) >> 17 ) & 3 )	/* 0 high, 1 full, 2 low */
#define  PRT_W1C	( PRT_CONNDET | PRT_ENA | PRT_ENACHG | PRT_OCCHG )
#define PCGCTL		0xe00
#define HC(ch, r)	( 0x500 + 0x20 * (ch) + (r) )
#define HCCHAR		0x00
#define  CH_EPNUM(n)	( (UW)(n) << 11 )
#define  CH_IN		0x00008000
#define  CH_LOWSPEED	0x00020000
#define  CH_TYPE(t)	( (UW)(t) << 18 )	/* USB_EP_*, control being 0 */
#define  CH_MC(n)	( (UW)(n) << 20 )
#define  CH_ADDR(a)	( (UW)(a) << 22 )
#define  CH_ODD		0x20000000
#define  CH_DIS		0x40000000
#define  CH_ENA		0x80000000
#define HCSPLT		0x04
#define HCINT		0x08
#define  CI_DONE	0x00000001
#define  CI_HALTED	0x00000002
#define  CI_AHBERR	0x00000004
#define  CI_STALL	0x00000008
#define  CI_NAK		0x00000010
#define  CI_ACK		0x00000020
#define  CI_XACTERR	0x00000080
#define  CI_BABBLE	0x00000100
#define  CI_FRMOVER	0x00000200
#define  CI_TOGERR	0x00000400
#define HCINTMSK	0x0c
#define HCTSIZ		0x10
#define  TS_SIZE_MASK	0x0007ffff
#define  TS_PKT_SHIFT	19
#define  TS_PKT_MASK	0x3ff
#define  TS_PID_SHIFT	29
#define  PID_DATA0	0
#define  PID_DATA1	2
#define  PID_SETUP	3
#define HCDMA		0x14

#define POWER_GET_TAG	0x00020001		/* GET_POWER_STATE */
#define POWER_TAG	0x00028001		/* SET_POWER_STATE */
#define POWER_USB	3			/* its number for this controller */
#define POWER_ON_WAIT	3			/* on, and answer once it is */
#define POWER_OFF_WAIT	2

/* The FIFO in words, as this block's integration divides it */
#define FIFO_RX		774
#define FIFO_NPTX	256
#define FIFO_PTX	512

#define IRQ_KIND_LINE	3		/* the kind the manager calls a line of its own */

/*
 * How long the port is watched. A device must pull its line up within
 * 100 ms of having power (USB 2.0 7.1.7.3), and one on the board has had
 * power since the firmware started, seconds ago; the rest allows for a
 * microcontroller whose program starts its USB late. Nothing waits for
 * this but the manager's task between its turns.
 */
#define PROBE_MS	3000
#define PROBE_STEADY_MS	100		/* the connection debounced, as a hub's would be */

#define MAX_CHANNELS	16
#define SYNC_CH		0		/* control and bulk, one at a time under the lock */
#define BOUNCE_ORDER	4		/* 64KB, the most a channel carries here */
#define BOUNCE_BYTES	( USB_PAGE << BOUNCE_ORDER )
#define SETUP_ROOM	64		/* the request, then the data, word aligned */
#define CTRL_MAX	USB_CFG_MAX
#define XACT_RETRIES	3
#define WAIT_STEP_MS	2		/* a look at the channel without its interrupt */
#define HALT_TMO_MS	20
#define EP_SLOW_MS	100		/* an endpoint that keeps failing is asked less */

/* What became of a halted channel */
#define R_DONE		0
#define R_NAK		1
#define R_STALL		2
#define R_ERROR		3

typedef struct {
	UB	toggle[2][16];		/* out, in: the next packet's DATA0 or DATA1 */
} DWDEV;

typedef struct dwc2 DWC2;

struct usb_pipe {
	DWC2		*d;
	struct usb_pipe	*next;
	USBDEV		*dev;
	USBEP		ep;
	INT		xfer;		/* bytes asked for */
	USB_XFERFN	fn;
	void		*ctx;
	DMABUF		buf;
	INT		order;
	INT		ch;		/* the channel in use, -1 between transfers */
	INT		sent;		/* what the channel was given */
	UW		due;		/* the millisecond it is next asked */
	UW		ivl;
	INT		errs;
};

struct dwc2 {
	USBHC	hc;			/* first: what the manager has is a pointer to this */
	UBINT	r;
	UD	mmio;
	UD	bus_off;		/* a physical address plus this: the controller's */
	UD	dma_size;		/* what it reaches, from physical dma_base */
	UD	dma_base;
	INT	nch;
	INT	xfer_max;
	INT	pkt_max;
	UB	*bounce;
	UD	bounce_bus;
	ID	ev;			/* the interrupt says a channel halted */
	BOOL	running;
	BOOL	ch_busy[MAX_CHANNELS];
	UB	addr_used[128];
	struct usb_pipe	*pipes;
	UW	port_seen;
	CONST char *node;
	UW	found_usbcfg;	/* as it was, to put back */
	UW	found_power;
	BOOL	touched;
};

LOCAL DWC2	*dwc2_one;

Inline UW rd( DWC2 *d, UW o )
{
	return *(volatile UW *)( d->r + o );
}

Inline void wr( DWC2 *d, UW o, UW v )
{
	*(volatile UW *)( d->r + o ) = v;
}

/* ---------------------------------------------------------------- interrupts */

/* The line down, and the task told: see the head of this file */
EXPORT void knl_dwc2_inthdr( UINT intno )
{
	DWC2	*d = dwc2_one;
	UW	st;

	if ( d == NULL || intno != d->hc.intno || !d->running ) {
		EndOfInt(intno);
		return;
	}
	st = rd(d, GINTSTS);
	if ( (st & GI_PORT) != 0 ) {
		wr(d, GINTMSK, GI_CHANNEL);
		tk_set_flg(d->hc.flgid, USBF_RESCAN);
	}
	if ( (st & GI_CHANNEL) != 0 ) {
		UW	haint = rd(d, HAINT);
		INT	ch;

		for ( ch = 0; ch < d->nch; ch++ ) {
			if ( (haint & (1UL << ch)) != 0 ) {
				wr(d, HC(ch, HCINTMSK), 0);
			}
		}
		tk_set_flg(d->ev, 1);
		tk_set_flg(d->hc.flgid, USBF_IRQ);
	}
	knl_usb_stat.irqs++;
	d->hc.nirq++;
	EndOfInt(intno);
}

/* ---------------------------------------------------------------- channels */

LOCAL UW frame_odd_next( DWC2 *d )
{
	return ( ( rd(d, HFNUM) & 1 ) == 0 ) ? CH_ODD : 0;
}

/*
 * One transfer on a channel: len bytes at bus, in as many packets as it
 * takes. An IN channel is told whole packets, as that is how much room
 * it may fill; the buffers here have the room.
 */
LOCAL void chan_start( DWC2 *d, INT ch, USBDEV *dev, UW epn, BOOL in, INT type,
		       INT mps, INT mult, UW bus, INT len, UW pid )
{
	INT	pkts = ( len > 0 ) ? ( len + mps - 1 ) / mps : 1;
	INT	size = in ? pkts * mps : len;
	UW	c;

	c = (UW)mps | CH_EPNUM(epn) | CH_TYPE(type) | CH_MC(mult) | CH_ADDR(dev->addr);
	if ( in ) c |= CH_IN;
	if ( dev->speed == USB_SPEED_LOW ) c |= CH_LOWSPEED;

	wr(d, HC(ch, HCINT), 0xffffffff);
	wr(d, HC(ch, HCINTMSK), CI_HALTED | CI_AHBERR);
	wr(d, HC(ch, HCSPLT), 0);
	wr(d, HC(ch, HCCHAR), c);
	wr(d, HC(ch, HCTSIZ), (UW)size | ( (UW)pkts << TS_PKT_SHIFT ) | ( pid << TS_PID_SHIFT ));
	wr(d, HC(ch, HCDMA), bus);
	USB_MB();
	if ( type == USB_EP_INTR || type == USB_EP_ISOC ) {
		c |= frame_odd_next(d);
	}
	wr(d, HC(ch, HCCHAR), c | CH_ENA);
}

/*
 * What the channel left. The reasons other than the first two say why
 * it halted without finishing.
 */
LOCAL INT chan_result( UW hi )
{
	if ( (hi & CI_DONE) != 0 ) return R_DONE;
	if ( (hi & CI_STALL) != 0 ) return R_STALL;
	if ( (hi & CI_NAK) != 0 ) return R_NAK;
	return R_ERROR;
}

/* A channel made to give up. E_OK once it has halted */
LOCAL ER chan_abort( DWC2 *d, INT ch )
{
	UW	c = rd(d, HC(ch, HCCHAR));
	UW	t0 = knl_usb_ms();

	wr(d, HC(ch, HCINTMSK), 0);
	if ( (c & CH_ENA) == 0 ) {
		return E_OK;
	}
	wr(d, HC(ch, HCCHAR), c | CH_ENA | CH_DIS);
	while ( (rd(d, HC(ch, HCINT)) & CI_HALTED) == 0 ) {
		if ( knl_usb_ms() - t0 >= HALT_TMO_MS ) {
			USB_LOG("dwc2: channel %d did not halt (char %08x int %08x)\n", ch,
				rd(d, HC(ch, HCCHAR)), rd(d, HC(ch, HCINT)));
			return E_TMOUT;
		}
		tk_dly_tsk(1);
	}
	return E_OK;
}

/*
 * One stretch of a control or bulk transfer on the channel kept for
 * them: what was moved in *moved. The controller's own retries are out
 * of sight; a NAK that halts it anyway (an interrupt endpoint, and some
 * emulations of bulk) is asked again until the time is up, and a broken
 * transaction up to XACT_RETRIES times from the packet it stopped at.
 */
LOCAL ER sync_run( DWC2 *d, USBDEV *dev, UW epn, BOOL in, INT type, INT mps, UW *pid,
		   UW bus, INT len, TMO tmo, INT *moved )
{
	UW	t0 = knl_usb_ms();
	INT	done = 0, tries = 0;

	*moved = 0;
	for (;;) {
		INT	left = len - done, pkts, sz;
		UW	hi, ts;

		pkts = ( left > 0 ) ? ( left + mps - 1 ) / mps : 1;
		sz = in ? pkts * mps : left;
		chan_start(d, SYNC_CH, dev, epn, in, type, mps, 1, bus + (UW)done, left, *pid);
		while ( ( (hi = rd(d, HC(SYNC_CH, HCINT))) & CI_HALTED ) == 0 ) {
			UINT	p;

			if ( tmo >= 0 && (INT)( knl_usb_ms() - t0 ) >= tmo ) {
				(void)chan_abort(d, SYNC_CH);
				USB_LOG("dwc2: addr %d ep %d %s timed out (char %08x int %08x siz %08x)\n",
					dev->addr, (INT)epn, in ? "in" : "out", rd(d, HC(SYNC_CH, HCCHAR)),
					rd(d, HC(SYNC_CH, HCINT)), rd(d, HC(SYNC_CH, HCTSIZ)));
				return E_TMOUT;
			}
			(void)tk_wai_flg(d->ev, 1, TWF_ORW | TWF_BITCLR, &p, WAIT_STEP_MS);
		}
		wr(d, HC(SYNC_CH, HCINTMSK), 0);
		ts = rd(d, HC(SYNC_CH, HCTSIZ));
		/* the toggle goes on from where the channel stopped */
		if ( ( ( ts >> TS_PID_SHIFT ) & 3 ) != PID_SETUP ) {
			*pid = ( ( ( ts >> TS_PID_SHIFT ) & 3 ) == PID_DATA0 ) ? PID_DATA0 : PID_DATA1;
		}
		if ( in ) {
			done += sz - (INT)( ts & TS_SIZE_MASK );
		} else if ( (hi & CI_DONE) != 0 ) {
			done = len;
		} else {
			done += ( pkts - (INT)( ( ts >> TS_PKT_SHIFT ) & TS_PKT_MASK ) ) * mps;
		}
		if ( done > len ) {
			done = len;
		}
		*moved = done;
		switch ( chan_result(hi) ) {
		case R_DONE:
			return E_OK;
		case R_STALL:
			return E_OBJ;
		case R_NAK:
			if ( tmo >= 0 && (INT)( knl_usb_ms() - t0 ) >= tmo ) {
				return E_TMOUT;
			}
			tk_dly_tsk(1);
			break;
		default:
			if ( (hi & CI_AHBERR) != 0 || ++tries >= XACT_RETRIES ) {
				USB_LOG("dwc2: addr %d ep %d %s failed (int %08x siz %08x)\n", dev->addr,
					(INT)epn, in ? "in" : "out", hi, ts);
				return E_IO;
			}
			tk_dly_tsk(1);
			break;
		}
	}
}

/* ---------------------------------------------------------------- the root port */

LOCAL INT dw_nports( USBHC *hc )
{
	(void)hc;
	return 1;
}

LOCAL BOOL dw_port_connected( USBHC *hc, INT port )
{
	DWC2	*d = (DWC2 *)hc;

	return ( port == 0 && (rd(d, HPRT) & PRT_CONN) != 0 );
}

LOCAL ER dw_port_status( USBHC *hc, INT port, UW ack, UW *st )
{
	DWC2	*d = (DWC2 *)hc;
	UW	v, s = 0, clr = 0;

	if ( port != 0 ) {
		return E_PAR;
	}
	v = rd(d, HPRT);
	if ( (v & PRT_CONN) != 0 ) s |= HUB_PS_CONNECTION;
	if ( (v & PRT_ENA) != 0 ) s |= HUB_PS_ENABLE;
	if ( (v & PRT_OC) != 0 ) s |= HUB_PS_OVERCURRENT;
	if ( (v & PRT_RST) != 0 ) s |= HUB_PS_RESET;
	if ( (v & PRT_PWR) != 0 ) s |= HUB_PS_POWER;
	if ( (v & PRT_ENA) != 0 && PRT_SPD(v) == 2 ) s |= HUB_PS_LOW_SPEED;
	if ( (v & PRT_ENA) != 0 && PRT_SPD(v) == 0 ) s |= HUB_PS_HIGH_SPEED;
	if ( (v & PRT_CONNDET) != 0 ) {
		s |= HUB_PC_CONNECTION;
		if ( (ack & HUB_PC_CONNECTION) != 0 ) clr |= PRT_CONNDET;
	}
	if ( (v & PRT_ENACHG) != 0 ) {
		s |= HUB_PC_ENABLE;
		if ( (ack & HUB_PC_ENABLE) != 0 ) clr |= PRT_ENACHG;
	}
	if ( (v & PRT_OCCHG) != 0 ) {
		s |= HUB_PC_OVERCURRENT;
		if ( (ack & HUB_PC_OVERCURRENT) != 0 ) clr |= PRT_OCCHG;
	}
	if ( clr != 0 ) {
		wr(d, HPRT, ( v & ~PRT_W1C ) | clr);
	}
	/* acknowledged, the port may interrupt again */
	if ( ( ( v & ~clr ) & ( PRT_CONNDET | PRT_ENACHG | PRT_OCCHG ) ) == 0 ) {
		wr(d, GINTMSK, GI_CHANNEL | GI_PORT);
	}
	if ( ( v & PRT_CONN ) != ( d->port_seen & PRT_CONN ) ) {
		USB_LOG("dwc2: port %s (HPRT %08x, line state %d)\n",
			( (v & PRT_CONN) != 0 ) ? "connected" : "not connected", v, (INT)( ( v >> 10 ) & 3 ));
	}
	d->port_seen = v;
	*st = s;
	return E_OK;
}

LOCAL ER dw_port_reset( USBHC *hc, INT port, INT *speed )
{
	DWC2	*d = (DWC2 *)hc;
	UW	v;

	if ( port != 0 ) {
		return E_PAR;
	}
	v = rd(d, HPRT);
	if ( (v & PRT_CONN) == 0 ) {
		return E_NOEXS;
	}
	wr(d, HPRT, ( v & ~PRT_W1C ) | PRT_RST);
	knl_usb_wait(60);		/* a root port's reset is at least 50 ms */
	v = rd(d, HPRT);
	wr(d, HPRT, v & ~( PRT_W1C | PRT_RST ));
	knl_usb_wait(20);		/* reset recovery */
	v = rd(d, HPRT);
	if ( (v & PRT_ENA) == 0 ) {
		USB_LOG("dwc2: port not enabled after reset (HPRT %08x)\n", v);
		return E_IO;
	}
	switch ( PRT_SPD(v) ) {
	case 0:	 *speed = USB_SPEED_HIGH; break;
	case 1:	 *speed = USB_SPEED_FULL; break;
	default: *speed = USB_SPEED_LOW; break;
	}
	USB_LOG("dwc2: port reset: HPRT %08x, %s speed\n", v,
		( *speed == USB_SPEED_HIGH ) ? "high" : ( *speed == USB_SPEED_FULL ) ? "full" : "low");
	return E_OK;
}

LOCAL ER dw_port_state( USBHC *hc, INT port, T_USBPORT *p )
{
	DWC2	*d = (DWC2 *)hc;
	UW	v;

	if ( port != 0 ) {
		return E_PAR;
	}
	v = rd(d, HPRT);
	p->connected = ( (v & PRT_CONN) != 0 ) ? 1 : 0;
	p->enabled   = ( (v & PRT_ENA) != 0 ) ? 1 : 0;
	p->powered   = ( (v & PRT_PWR) != 0 ) ? 1 : 0;
	p->speed     = !p->enabled ? USB_SPEED_NONE
		     : ( PRT_SPD(v) == 0 ) ? USB_SPEED_HIGH
		     : ( PRT_SPD(v) == 1 ) ? USB_SPEED_FULL : USB_SPEED_LOW;
	p->changed   = ( (v & ( PRT_CONNDET | PRT_ENACHG | PRT_OCCHG )) != 0 ) ? 1 : 0;
	p->usb3      = 0;
	return E_OK;
}

LOCAL void dw_port_power( USBHC *hc, INT port )
{
	DWC2	*d = (DWC2 *)hc;

	if ( port == 0 ) {
		wr(d, HPRT, ( rd(d, HPRT) & ~PRT_W1C ) | PRT_PWR);
	}
}

/* ---------------------------------------------------------------- transfers */

/*
 * The three stages. The request and the data share the buffer, the
 * request first, and the data stage starts from DATA1 whatever went
 * before.
 */
LOCAL ER dw_control( USBHC *hc, USBDEV *dev, CONST UB *setup, UB *data, INT *actual, TMO tmo )
{
	DWC2	*d = (DWC2 *)hc;
	INT	len = GET16(setup + 6), got = 0, moved = 0;
	BOOL	in = ( (setup[0] & USB_RT_IN) != 0 );
	UW	pid, bus = (UW)d->bounce_bus;
	ER	er;

	if ( actual != NULL ) {
		*actual = 0;
	}
	if ( !d->running ) {
		return E_NOEXS;
	}
	if ( len > CTRL_MAX ) {
		return E_PAR;
	}
	knl_memcpy(d->bounce, setup, 8);
	if ( !in && len > 0 ) {
		knl_memcpy(d->bounce + SETUP_ROOM, data, len);
	}
	USB_MB();
	pid = PID_SETUP;
	er = sync_run(d, dev, 0, FALSE, 0, dev->mps0, &pid, bus, 8, tmo, &moved);
	if ( er < E_OK ) {
		return er;
	}
	if ( len > 0 ) {
		pid = PID_DATA1;
		er = sync_run(d, dev, 0, in, 0, dev->mps0, &pid, bus + SETUP_ROOM, len, tmo, &got);
		if ( er < E_OK ) {
			return er;
		}
	}
	pid = PID_DATA1;
	er = sync_run(d, dev, 0, ( !in || len == 0 ), 0, dev->mps0, &pid, bus, 0, tmo, &moved);
	if ( er < E_OK ) {
		return er;
	}
	if ( in && len > 0 && data != NULL ) {
		USB_MB();
		knl_memcpy(data, d->bounce + SETUP_ROOM, got);
	}
	if ( actual != NULL ) {
		*actual = got;
	}
	return E_OK;
}

/* A bulk or interrupt transfer, cut in what a channel and the buffer carry at once */
LOCAL ER dw_bulk( USBHC *hc, USBDEV *dev, CONST USBEP *ep, UB *data, INT len, INT *actual, TMO tmo )
{
	DWC2	*d = (DWC2 *)hc;
	DWDEV	*dv = (DWDEV *)dev->hcpriv;
	BOOL	in = ( (ep->addr & 0x80) != 0 );
	UW	epn = ep->addr & 0x0f;
	INT	mps = ep->size, total = 0, chunk;
	UW	pid;
	ER	er = E_OK;

	if ( actual != NULL ) {
		*actual = 0;
	}
	if ( !d->running || dv == NULL || mps <= 0 ) {
		return E_NOEXS;
	}
	if ( ep->type == USB_EP_ISOC ) {
		return E_NOSPT;
	}
	chunk = BOUNCE_BYTES;
	if ( chunk > d->xfer_max ) chunk = d->xfer_max;
	if ( chunk > d->pkt_max * mps ) chunk = d->pkt_max * mps;
	chunk -= chunk % mps;
	if ( chunk <= 0 ) {
		return E_PAR;
	}
	pid = dv->toggle[in][epn] ? PID_DATA1 : PID_DATA0;
	do {
		INT	n = ( len - total < chunk ) ? len - total : chunk, moved = 0;

		if ( !in && n > 0 ) {
			knl_memcpy(d->bounce, data + total, n);
			USB_MB();
		}
		er = sync_run(d, dev, epn, in, ep->type, mps, &pid, (UW)d->bounce_bus, n, tmo, &moved);
		if ( in && moved > 0 ) {
			USB_MB();
			knl_memcpy(data + total, d->bounce, moved);
		}
		total += moved;
		if ( er < E_OK || moved < n ) {
			break;			/* a short packet ends a transfer */
		}
	} while ( total < len );
	dv->toggle[in][epn] = ( pid == PID_DATA1 ) ? 1 : 0;
	if ( actual != NULL ) {
		*actual = total;
	}
	return er;
}

LOCAL ER dw_ep_reset( USBHC *hc, USBDEV *dev, UB ep )
{
	DWDEV	*dv = (DWDEV *)dev->hcpriv;

	(void)hc;
	if ( dv != NULL ) {
		dv->toggle[( ep & 0x80 ) != 0][ep & 0x0f] = 0;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- devices */

/*
 * The device about to be enumerated gets its address, which is what the
 * layer above leaves to the controller driver. A device reached only
 * through a hub's translator, or a low speed one behind a full speed hub,
 * is refused here: the channels would need split transactions or preamble.
 */
LOCAL ER dw_dev_init( USBHC *hc, USBDEV *dev )
{
	DWC2	*d = (DWC2 *)hc;
	DWDEV	*dv;
	UB	setup[8];
	INT	a, port = 0;
	ER	er;

	if ( knl_usb_tt_hub(dev, &port) != NULL
	  || ( dev->parent != NULL && dev->speed == USB_SPEED_LOW ) ) {
		USB_LOG("dwc2: a %s speed device behind a hub is not supported\n",
			( dev->speed == USB_SPEED_LOW ) ? "low" : "full");
		return E_NOSPT;
	}
	for ( a = 1; a < 128 && d->addr_used[a]; a++ ) ;
	if ( a == 128 ) {
		return E_LIMIT;
	}
	if ( dev->hcpriv == NULL ) {
		dv = (DWDEV *)Kcalloc(1, sizeof(DWDEV));
		if ( dv == NULL ) {
			return E_NOMEM;
		}
		dev->hcpriv = dv;
	} else {
		knl_memset(dev->hcpriv, 0, sizeof(DWDEV));
	}
	dev->addr = 0;
	setup[0] = 0;
	setup[1] = USB_REQ_SET_ADDRESS;
	PUT16(setup + 2, a);
	PUT16(setup + 4, 0);
	PUT16(setup + 6, 0);
	er = dw_control(hc, dev, setup, NULL, NULL, 1000);
	if ( er < E_OK ) {
		USB_LOG("dwc2: SET_ADDRESS %d failed (%d)\n", a, (INT)er);
		Kfree(dev->hcpriv);
		dev->hcpriv = NULL;
		return er;
	}
	d->addr_used[a] = 1;
	dev->addr = a;
	knl_usb_wait(10);		/* the device takes up to 2 ms to answer at it */
	return E_OK;
}

LOCAL ER dw_dev_mps0( USBHC *hc, USBDEV *dev )
{
	(void)hc;
	(void)dev;
	return E_OK;			/* each control transfer reads dev->mps0 */
}

LOCAL ER dw_hub_config( USBHC *hc, USBDEV *hub )
{
	(void)hc;
	(void)hub;
	return E_OK;
}

LOCAL void pipe_stop( DWC2 *d, struct usb_pipe *p )
{
	if ( p->ch >= 0 ) {
		(void)chan_abort(d, p->ch);
		d->ch_busy[p->ch] = FALSE;
		p->ch = -1;
	}
}

LOCAL void dw_stream_close( USBHC *hc, USBPIPE *pipe )
{
	DWC2		*d = (DWC2 *)hc;
	struct usb_pipe	**pp;

	if ( pipe == NULL ) {
		return;
	}
	pipe_stop(d, pipe);
	for ( pp = &d->pipes; *pp != NULL; pp = &(*pp)->next ) {
		if ( *pp == pipe ) {
			*pp = pipe->next;
			break;
		}
	}
	knl_usb_pages_free(pipe->buf.virt[0], pipe->order);
	Kfree(pipe);
}

LOCAL void dw_dev_free( USBHC *hc, USBDEV *dev )
{
	DWC2		*d = (DWC2 *)hc;
	struct usb_pipe	*p, *nx;

	for ( p = d->pipes; p != NULL; p = nx ) {
		nx = p->next;
		if ( p->dev == dev ) {
			dw_stream_close(hc, p);
		}
	}
	if ( dev->addr > 0 && dev->addr < 128 ) {
		d->addr_used[dev->addr] = 0;
	}
	if ( dev->hcpriv != NULL ) {
		Kfree(dev->hcpriv);
		dev->hcpriv = NULL;
	}
}

/* ---------------------------------------------------------------- streams */

LOCAL ER dw_stream_open( USBHC *hc, USBDEV *dev, CONST USBEP *ep, INT xfer, USB_XFERFN fn, void *ctx,
			 USBPIPE **out )
{
	DWC2		*d = (DWC2 *)hc;
	struct usb_pipe	*p;
	INT		order = 0, i;
	UD		bus;
	UB		*v;

	if ( !d->running || ( ep->addr & 0x80 ) == 0 || ep->type == USB_EP_ISOC ) {
		USB_LOG("dwc2: endpoint %02x, type %d: only interrupt and bulk IN streams\n",
			(INT)ep->addr, (INT)ep->type);
		return E_NOSPT;
	}
	/* an IN channel may fill whole packets beyond what was asked for */
	i = ( ( xfer + ep->size - 1 ) / ep->size ) * ep->size;
	while ( ( USB_PAGE << order ) < i ) {
		order++;
	}
	if ( order > 3 || i > d->xfer_max || i / ep->size > d->pkt_max ) {
		return E_PAR;
	}
	p = (struct usb_pipe *)Kcalloc(1, sizeof(*p));
	if ( p == NULL ) {
		return E_NOMEM;
	}
	v = (UB *)knl_usb_pages_alloc(order, &bus);
	if ( v == NULL ) {
		Kfree(p);
		return E_NOMEM;
	}
	p->d = d;
	p->dev = dev;
	p->ep = *ep;
	p->xfer = xfer;
	p->fn = fn;
	p->ctx = ctx;
	p->order = order;
	p->ch = -1;
	p->buf.npages = 1 << order;
	bus = knl_usb_bus_pa(bus) + d->bus_off;
	for ( i = 0; i < p->buf.npages; i++ ) {
		p->buf.virt[i] = v + i * USB_PAGE;
		p->buf.bus[i] = bus + i * USB_PAGE;
	}
	if ( ep->type == USB_EP_INTR ) {
		INT	b = ( ep->interval > 0 ) ? ep->interval : 1;

		p->ivl = ( dev->speed == USB_SPEED_HIGH )
		       ? ( ( b > 16 ? 16 : b ) >= 4 ? ( 1U << ( ( b > 16 ? 16 : b ) - 1 ) ) / 8 : 1 ) : (UW)b;
	}
	p->due = knl_usb_ms();
	p->next = d->pipes;
	d->pipes = p;
	*out = p;
	USB_LOG("dwc2: addr %d endpoint %02x: stream of %d bytes, packet %d, every %d ms\n", dev->addr,
		(INT)ep->addr, xfer, ep->size, (INT)p->ivl);
	knl_usb_kick();
	return E_OK;
}

LOCAL ER dw_stream_open_out( USBHC *hc, USBDEV *dev, CONST USBEP *ep, USB_FILLFN fill, void *ctx, USBPIPE **out )
{
	(void)hc;
	(void)dev;
	(void)ep;
	(void)fill;
	(void)ctx;
	(void)out;
	return E_NOSPT;		/* isochronous OUT */
}

/* A stream's channel halted: what it brought, and when the endpoint is asked again */
LOCAL void pipe_finish( DWC2 *d, struct usb_pipe *p, UW hi )
{
	DWDEV	*dv = (DWDEV *)p->dev->hcpriv;
	UW	ts = rd(d, HC(p->ch, HCTSIZ));
	UW	now = knl_usb_ms();
	INT	res = chan_result(hi), n;
	UW	epn = p->ep.addr & 0x0f;

	wr(d, HC(p->ch, HCINTMSK), 0);
	d->ch_busy[p->ch] = FALSE;
	p->ch = -1;
	if ( dv != NULL ) {
		dv->toggle[1][epn] = ( ( ( ts >> TS_PID_SHIFT ) & 3 ) == PID_DATA0 ) ? 0 : 1;
	}
	switch ( res ) {
	case R_DONE:
		p->errs = 0;
		p->due = now + p->ivl;
		n = p->sent - (INT)( ts & TS_SIZE_MASK );
		if ( n > p->xfer ) {
			n = p->xfer;
		}
		USB_MB();
		p->fn(p->ctx, &p->buf, 0, n, 0);
		break;
	case R_NAK:
		p->errs = 0;
		p->due = now + p->ivl;
		break;
	case R_STALL:
		p->due = now + EP_SLOW_MS;
		if ( p->errs++ == 0 ) {
			USB_LOG("dwc2: addr %d endpoint %02x stalled\n", p->dev->addr, (INT)p->ep.addr);
		}
		if ( dv != NULL ) {
			dv->toggle[1][epn] = 0;
		}
		p->fn(p->ctx, &p->buf, 0, 0, E_OBJ);
		break;
	default:
		p->errs++;
		p->due = now + ( ( p->errs >= XACT_RETRIES ) ? EP_SLOW_MS : p->ivl );
		if ( p->errs == XACT_RETRIES ) {
			USB_LOG("dwc2: addr %d endpoint %02x keeps failing (int %08x)\n", p->dev->addr,
				(INT)p->ep.addr, hi);
		}
		break;
	}
}

LOCAL void pipe_arm( DWC2 *d, struct usb_pipe *p )
{
	DWDEV	*dv = (DWDEV *)p->dev->hcpriv;
	UW	epn = p->ep.addr & 0x0f;
	INT	ch;

	for ( ch = SYNC_CH + 1; ch < d->nch && d->ch_busy[ch]; ch++ ) ;
	if ( ch >= d->nch || dv == NULL ) {
		return;				/* at the next turn */
	}
	d->ch_busy[ch] = TRUE;
	p->ch = ch;
	p->sent = ( ( p->xfer + p->ep.size - 1 ) / p->ep.size ) * p->ep.size;
	USB_MB();
	chan_start(d, ch, p->dev, epn, TRUE, p->ep.type, p->ep.size,
		   ( p->dev->speed == USB_SPEED_HIGH && p->ep.type == USB_EP_INTR ) ? p->ep.mult : 1,
		   (UW)p->buf.bus[0], p->xfer, dv->toggle[1][epn] ? PID_DATA1 : PID_DATA0);
}

/* The streams served and the port looked at; called with the manager's lock held */
LOCAL void dw_poll( USBHC *hc )
{
	DWC2		*d = (DWC2 *)hc;
	struct usb_pipe	*p, *nx;
	UW		now, v;

	if ( !d->running ) {
		return;
	}
	for ( p = d->pipes; p != NULL; p = nx ) {
		nx = p->next;
		if ( p->ch >= 0 ) {
			UW	hi = rd(d, HC(p->ch, HCINT));

			if ( (hi & CI_HALTED) != 0 ) {
				pipe_finish(d, p, hi);
			}
		}
	}
	now = knl_usb_ms();
	for ( p = d->pipes; p != NULL; p = p->next ) {
		if ( p->ch < 0 && (INT)( now - p->due ) >= 0 ) {
			pipe_arm(d, p);
		}
	}
	/* a port change the line did not bring */
	v = rd(d, HPRT);
	if ( (v & ( PRT_CONNDET | PRT_ENACHG | PRT_OCCHG )) != 0 || ( ( v ^ d->port_seen ) & PRT_CONN ) != 0 ) {
		tk_set_flg(hc->flgid, USBF_RESCAN);
	}
}

LOCAL void dw_info( USBHC *hc, T_USBHC *info )
{
	DWC2	*d = (DWC2 *)hc;

	knl_memset(info, 0, sizeof(*info));
	info->version = 0x0200;
	info->nports = 1;
	info->nslots = 127;
	info->running = d->running ? 1 : 0;
	info->intno = hc->intno;
	info->mmio = d->mmio;
}

LOCAL CONST USBHC_OPS dwc2_ops = {
	dw_nports, dw_port_connected, dw_port_status, dw_port_reset, dw_port_state,
	dw_dev_init, dw_dev_mps0, dw_hub_config, dw_dev_free, dw_control, dw_bulk,
	dw_stream_open, dw_stream_open_out, dw_stream_close, dw_poll, dw_info,
	dw_ep_reset,
	NULL,				/* no USB 3 ports */
	dw_port_power,
	NULL, NULL			/* these machines do not sleep */
};

/* ---------------------------------------------------------------- bringing it up */

/* The controller's power domain, through the firmware: what it answers */
LOCAL UW power_tag( UW tag, UW state )
{
	static UW	b[16] __attribute__((aligned(64)));

	b[0] = 8 * 4;
	b[1] = 0;
	b[2] = tag;
	b[3] = 8;
	b[4] = 8;
	b[5] = POWER_USB;
	b[6] = state;
	b[7] = 0;
	return ( knl_mbox_property(b) >= E_OK ) ? b[6] : 0xffffffff;
}

LOCAL BOOL wait_clear( DWC2 *d, UW reg, UW bit, UW set, INT ms )
{
	UW	t0 = knl_usb_ms();

	for (;;) {
		UW	v = rd(d, reg);

		if ( (v & bit) == 0 && (v & set) == set ) {
			return TRUE;
		}
		if ( (INT)( knl_usb_ms() - t0 ) >= ms ) {
			return FALSE;
		}
		tk_dly_tsk(1);
	}
}

/*
 * The block on and a host, its port powered: all that looking at the port
 * takes. No interrupt, and no DMA, until dw_go.
 */
LOCAL ER dw_core_up( DWC2 *d )
{
	UW	id, h2, h3, v, depth, rx = FIFO_RX, np = FIFO_NPTX, pt = FIFO_PTX;
	INT	ch;

	d->found_power = power_tag(POWER_GET_TAG, 0);
	(void)power_tag(POWER_TAG, POWER_ON_WAIT);
	id = rd(d, GSNPSID);
	h2 = rd(d, GHWCFG2);
	h3 = rd(d, GHWCFG3);
	d->found_usbcfg = rd(d, GUSBCFG);
	USB_LOG("dwc2: power was %x, id %08x hwcfg %08x %08x %08x %08x usbcfg %08x\n", d->found_power, id,
		rd(d, GHWCFG1), h2, h3, rd(d, GHWCFG4), d->found_usbcfg);
	if ( (id & ID_MASK) != ID_CORE ) {
		USB_LOG("dwc2: not this controller\n");
		return E_NOEXS;
	}
	if ( HW2_ARCH(h2) != 2 ) {
		USB_LOG("dwc2: no DMA inside the core (%d)\n", (INT)HW2_ARCH(h2));
		return E_NOSPT;
	}
	d->nch = (INT)HW2_NCH(h2);
	if ( d->nch > MAX_CHANNELS ) d->nch = MAX_CHANNELS;
	d->xfer_max = (INT)( ( 1UL << HW3_XFERW(h3) ) - 1 );
	d->pkt_max = (INT)( ( 1UL << HW3_PKTW(h3) ) - 1 );

	d->touched = TRUE;
	wr(d, GAHBCFG, 0);
	wr(d, GINTMSK, 0);
	if ( !wait_clear(d, GRSTCTL, 0, RST_AHBIDLE, 100) ) {
		USB_LOG("dwc2: the bus master stays busy (%08x)\n", rd(d, GRSTCTL));
		return E_IO;
	}
	wr(d, GRSTCTL, rd(d, GRSTCTL) | RST_CORE);
	if ( !wait_clear(d, GRSTCTL, RST_CORE, RST_AHBIDLE, 100) ) {
		USB_LOG("dwc2: the core did not come out of reset (%08x)\n", rd(d, GRSTCTL));
		return E_IO;
	}
	/* host whatever the ID line says */
	v = rd(d, GUSBCFG);
	v &= ~(UW)( USB_FORCE_DEV | USB_SRP | USB_HNP );
	wr(d, GUSBCFG, v | USB_FORCE_HOST);
	if ( !wait_clear(d, GINTSTS, 0, GI_HOSTMODE, 200) ) {
		USB_LOG("dwc2: did not become a host (GINTSTS %08x, GUSBCFG %08x)\n", rd(d, GINTSTS), rd(d, GUSBCFG));
		return E_IO;
	}
	wr(d, PCGCTL, 0);
	v = rd(d, HCFG);
	wr(d, HCFG, v & ~(UW)( HCFG_CLK | HCFG_FSONLY | HCFG_DESCDMA ));

	depth = HW3_DEPTH(h3);
	if ( depth != 0 && rx + np + pt > depth ) {
		rx = depth / 2;
		np = depth / 4;
		pt = depth - rx - np;
	}
	wr(d, GRXFSIZ, rx);
	wr(d, GNPTXFSIZ, ( np << 16 ) | rx);
	wr(d, HPTXFSIZ, ( pt << 16 ) | ( rx + np ));
	wr(d, GDFIFOCFG, ( rd(d, GDFIFOCFG) & 0xffff ) | ( ( rx + np + pt ) << 16 ));
	wr(d, GRSTCTL, RST_TXFLUSH | RST_TXALL);
	(void)wait_clear(d, GRSTCTL, RST_TXFLUSH, 0, 10);
	wr(d, GRSTCTL, RST_RXFLUSH);
	(void)wait_clear(d, GRSTCTL, RST_RXFLUSH, 0, 10);

	for ( ch = 0; ch < d->nch; ch++ ) {
		wr(d, HC(ch, HCINTMSK), 0);
		(void)chan_abort(d, ch);
		wr(d, HC(ch, HCINT), 0xffffffff);
	}
	dw_port_power(&d->hc, 0);
	return E_OK;
}

/* The block put back as it was found: port off, core reset, not a host, its power as it was */
LOCAL void dw_core_down( DWC2 *d )
{
	if ( d->touched ) {
		wr(d, HPRT, rd(d, HPRT) & ~( PRT_W1C | PRT_PWR ));
		wr(d, GRSTCTL, rd(d, GRSTCTL) | RST_CORE);
		(void)wait_clear(d, GRSTCTL, RST_CORE, 0, 100);
		wr(d, GUSBCFG, d->found_usbcfg);
		d->touched = FALSE;
	}
	if ( ( d->found_power & 1 ) == 0 ) {
		(void)power_tag(POWER_TAG, POWER_OFF_WAIT);
	}
}

/* From looking at the port to using it: the buffer, the interrupt, the DMA */
LOCAL ER dw_go( DWC2 *d )
{
	T_CFLG	cflg;
	UD	bpa;

	d->bounce = (UB *)knl_usb_pages_alloc(BOUNCE_ORDER, &d->bounce_bus);
	if ( d->bounce == NULL ) {
		return E_NOMEM;
	}
	bpa = knl_usb_bus_pa(d->bounce_bus);
	if ( bpa < d->dma_base || bpa + BOUNCE_BYTES - d->dma_base > d->dma_size
	  || bpa + d->bus_off + BOUNCE_BYTES > 0x100000000ULL ) {
		USB_LOG("dwc2: the buffer at %08x is out of its reach\n", (UW)bpa);
		knl_usb_pages_free(d->bounce, BOUNCE_ORDER);
		return E_NOSPT;
	}
	d->bounce_bus = bpa + d->bus_off;
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	d->ev = tk_cre_flg(&cflg);
	if ( d->ev <= 0 ) {
		knl_usb_pages_free(d->bounce, BOUNCE_ORDER);
		return E_LIMIT;
	}
	wr(d, HAINTMSK, (UW)( ( 1UL << d->nch ) - 1 ));
	wr(d, GINTSTS, 0xffffffff);
	d->running = TRUE;
	dwc2_one = d;
	if ( d->hc.intno != 0 ) {
		knl_define_inthdr((INT)d->hc.intno, TA_HLNG, (FP)knl_dwc2_inthdr);
		SetIntMode(d->hc.intno, IM_LEVEL);
		EnableInt(d->hc.intno, INTPRI_DEVICE);
	}
	wr(d, GINTMSK, GI_CHANNEL | GI_PORT);
	wr(d, GAHBCFG, AHB_BURST | AHB_DMA | AHB_GLBL);
	USB_LOG("dwc2: running: %d channels, HPRT %08x\n", d->nch, rd(d, HPRT));
	return E_OK;
}

/* The node in the firmware's device tree: the Raspberry Pi 4 family's, or the 5's */
LOCAL CONST char * CONST node_paths[] = { "/soc/usb@7e980000", "/axi/usb@480000" };
LOCAL CONST char * CONST parent_paths[] = { "/soc", "/axi" };

/* A bus address of the node's parent as a physical one, through its ranges */
LOCAL BOOL node_to_pa( CONST char *parent, UD child, UD *p_pa )
{
	UD	c, p, s;
	INT	i;

	for ( i = 0; knl_dtb_ranges(parent, "ranges", i, &c, &p, &s) >= E_OK; i++ ) {
		if ( child >= c && child - c < s ) {
			*p_pa = p + ( child - c );
			return TRUE;
		}
	}
	return FALSE;
}

/* The first cell pair's address of reg, however many cells the parent uses */
LOCAL BOOL node_reg( CONST char *node, CONST char *parent, UD *p_addr )
{
	INT		len = 0;
	CONST UB	*v = knl_dtb_prop(node, "reg", &len);
	UW		ac = knl_dtb_u32(parent, "#address-cells", 1);

	if ( v == NULL || len < (INT)ac * 4 || ac < 1 || ac > 2 ) {
		return FALSE;
	}
	*p_addr = ( (UD)v[0] << 24 ) | ( (UD)v[1] << 16 ) | ( (UD)v[2] << 8 ) | (UD)v[3];
	if ( ac == 2 ) {
		*p_addr = ( *p_addr << 32 ) | ( (UD)v[4] << 24 ) | ( (UD)v[5] << 16 ) | ( (UD)v[6] << 8 ) | (UD)v[7];
	}
	return TRUE;
}

#define DECIDE_NO	0
#define DECIDE_USE	1
#define DECIDE_PROBE	2

/* What the command line and the device tree say; a line on the console when it is no */
LOCAL INT dwc2_decide( CONST char *node )
{
	UB		arg[8];
	BOOL		given = knl_bootarg(knl_bootargs, "tessronos.dwc2", arg, sizeof(arg));
	INT		len = 0;
	CONST UB	*mode;

	if ( given && knl_strcmp((CONST char *)arg, "off") == 0 ) {
		USB_LOG("dwc2: tessronos.dwc2=off: not used\n");
		return DECIDE_NO;
	}
	if ( given && knl_strcmp((CONST char *)arg, "host") == 0 ) {
		USB_LOG("dwc2: tessronos.dwc2=host: used\n");
		return DECIDE_USE;
	}
	mode = knl_dtb_prop(node, "dr_mode", &len);
	if ( mode != NULL && len >= 5 && knl_strcmp((CONST char *)mode, "host") == 0 ) {
		if ( knl_dtb_okay(node) ) {
			USB_LOG("dwc2: %s dr_mode host: used\n", node);
			return DECIDE_USE;
		}
	} else if ( mode != NULL && len > 0 ) {
		USB_LOG("dwc2: %s dr_mode is %s: left to that\n", node, (CONST char *)mode);
		return DECIDE_NO;
	}
	return DECIDE_PROBE;
}

LOCAL DWC2	*probe_d;
LOCAL BOOL	probe_begun;
LOCAL UW	probe_t0, probe_seen;
LOCAL BOOL	probe_conn;

/*
 * The block found, from the device tree whatever the status of its node:
 * where it is, what it raises and what it reaches. NULL with a line on the
 * console when the machine has none of that.
 */
LOCAL DWC2 *dwc2_describe( ID flgid, CONST char *node, CONST char *parent )
{
	DWC2		*d;
	UD		reg = 0, pa = 0, child = 0, parent_pa = 0, size = 0;
	INT		len = 0;
	CONST UB	*irq;

	if ( !node_reg(node, parent, &reg) || !node_to_pa(parent, reg, &pa) ) {
		USB_LOG("dwc2: %s: no usable reg in its parent's ranges\n", node);
		return NULL;
	}
	if ( knl_dtb_ranges(parent, "dma-ranges", 0, &child, &parent_pa, &size) < E_OK || size == 0 ) {
		USB_LOG("dwc2: %s has no dma-ranges\n", parent);
		return NULL;
	}
	d = (DWC2 *)Kcalloc(1, sizeof(DWC2));
	if ( d == NULL ) {
		return NULL;
	}
	d->hc.ops = &dwc2_ops;
	d->hc.flgid = flgid;
	d->hc.poll_too = TRUE;		/* its interrupt endpoints are asked by software */
	d->hc.irqkind = IRQ_KIND_LINE;
	d->node = node;
	d->r = DEV_BASE(pa);
	d->mmio = pa;
	d->bus_off = child - parent_pa;
	d->dma_base = parent_pa;
	d->dma_size = size;
	/* interrupts = <0 spi flags ...>: the first is "usb", the controller's */
	irq = knl_dtb_prop(node, "interrupts", &len);
	if ( irq != NULL && len >= 12 && GET32(irq) == 0 ) {
		d->hc.intno = 32 + ( ( (UINT)irq[4] << 24 ) | ( (UINT)irq[5] << 16 )
				   | ( (UINT)irq[6] << 8 ) | (UINT)irq[7] );
	}
	return d;
}

/*
 * At the start of the USB stack. E_OK with *out when the command line or the
 * device tree says to use the block; otherwise E_NOEXS, and the port is
 * looked at later if neither said no (knl_dwc2_probing).
 */
EXPORT ER knl_dwc2_find( ID flgid, USBHC **out )
{
	CONST char	*node = NULL, *parent = NULL;
	DWC2		*d;
	INT		i, how;
	ER		er;

	if ( !CNF_DWC2 ) {
		return E_NOEXS;
	}
	for ( i = 0; i < (INT)( sizeof(node_paths) / sizeof(node_paths[0]) ); i++ ) {
		if ( knl_dtb_prop(node_paths[i], "reg", NULL) != NULL ) {
			node = node_paths[i];
			parent = parent_paths[i];
			break;
		}
	}
	if ( node == NULL ) {
		USB_LOG("dwc2: the device tree has no such node: not used\n");
		return E_NOEXS;
	}
	how = dwc2_decide(node);
	if ( how == DECIDE_NO ) {
		return E_NOEXS;
	}
	d = dwc2_describe(flgid, node, parent);
	if ( d == NULL ) {
		return E_NOEXS;
	}
	if ( how == DECIDE_PROBE ) {
		probe_d = d;
		return E_NOEXS;
	}
	er = dw_core_up(d);
	if ( er >= E_OK ) {
		er = dw_go(d);
	}
	if ( er < E_OK ) {
		dw_core_down(d);
		Kfree(d);
		return er;
	}
	*out = &d->hc;
	return E_OK;
}

EXPORT BOOL knl_dwc2_probing( void )
{
	return ( probe_d != NULL );
}

LOCAL void probe_end( void )
{
	Kfree(probe_d);
	probe_d = NULL;
}

/*
 * One look at the port, from the manager's task every PROBE_STEP_MS without
 * its lock: TRUE with *out when a device stayed connected and the controller
 * is ready to be added. A line state of both lines high is no device: some
 * chargers hold the data lines up with a divider.
 */
EXPORT BOOL knl_dwc2_probe_step( USBHC **out )
{
	DWC2	*d = probe_d;
	UW	now = knl_usb_ms(), v;
	ER	er;

	if ( d == NULL ) {
		return FALSE;
	}
	if ( !probe_begun ) {
		probe_begun = TRUE;
		probe_t0 = now;
		USB_LOG("dwc2: probing the USB-C port at %08x%08x for up to %d ms\n", (UW)( d->mmio >> 32 ), (UW)d->mmio,
			PROBE_MS);
		er = dw_core_up(d);
		if ( er < E_OK ) {
			dw_core_down(d);
			USB_LOG("dwc2: probing: the block did not come up (%d): put back, not used\n", (INT)er);
			probe_end();
		}
		return FALSE;
	}
	v = rd(d, HPRT);
	if ( (v & PRT_CONN) != 0 && ( ( v >> 10 ) & 3 ) != 3 ) {
		if ( !probe_conn ) {
			probe_conn = TRUE;
			probe_seen = now;
		}
		if ( now - probe_seen >= PROBE_STEADY_MS ) {
			er = dw_go(d);
			if ( er < E_OK ) {
				dw_core_down(d);
				USB_LOG("dwc2: probing: a device, but %d: put back, not used\n", (INT)er);
				probe_end();
				return FALSE;
			}
			USB_LOG("dwc2: probing: a device at %d ms (HPRT %08x): used, interrupt %d, bus = physical + %08x\n",
				(INT)( now - probe_t0 ), v, (INT)d->hc.intno, (UW)d->bus_off);
			probe_d = NULL;
			*out = &d->hc;
			return TRUE;
		}
		return FALSE;
	}
	probe_conn = FALSE;
	if ( now - probe_t0 >= PROBE_MS ) {
		dw_core_down(d);
		USB_LOG("dwc2: probing: nothing in %d ms (HPRT %08x): put back, not used (GUSBCFG %08x, as found %08x)\n",
			PROBE_MS, v, rd(d, GUSBCFG), d->found_usbcfg);
		probe_end();
	}
	return FALSE;
}

#endif /* RPI4 || RPI5 */
