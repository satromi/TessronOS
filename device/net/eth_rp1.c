/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	eth_rp1.c
 *	Ethernet of the RP1: the RJ45 socket of the board (design 10.4).
 *
 *	The controller is a Cadence GEM inside the RP1, with a Broadcom
 *	BCM54213PE on its MDIO bus. It is reached through the same PCIe
 *	window as the header pins, so the firmware has to have left the
 *	link up (`pciex4_reset=0`; design 4.6).
 *
 *	Two rings of descriptors are used, one each way, with a buffer per
 *	descriptor. The controller owns a descriptor until it sets the
 *	ownership bit back, so the rings are read out of memory rather than
 *	waited on: this side is polled and takes no interrupt. Interrupts
 *	from the RP1 need the MSI-X block set up first (checklist item 33).
 *
 *	The controller sits behind the PCIe bridge, so every address written
 *	into a descriptor is a bus address, which is the physical address
 *	plus RP1_DMA_BUS_OFFSET. That puts the rings above 4GB as the
 *	controller sees them, which is why the 64 bit descriptor layout is
 *	used even though the memory itself is taken from below 4GB.
 *
 *	NOT VERIFIED ON HARDWARE (docs/private/checklists/phase7.md items 35-40).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include <ts/gpio.h>
#include "sysman/pfalloc.h"

/* ---------------------------------------------------------------- registers */
#define GEM_NCR		(RP1_ETH_BASE + 0x000)	/* control */
#define GEM_NCFGR	(RP1_ETH_BASE + 0x004)	/* configuration */
#define GEM_NSR		(RP1_ETH_BASE + 0x008)	/* status */
#define GEM_DMACFG	(RP1_ETH_BASE + 0x010)
#define GEM_TSR		(RP1_ETH_BASE + 0x014)	/* transmit status */
#define GEM_RBQP	(RP1_ETH_BASE + 0x018)	/* receive ring, low half */
#define GEM_TBQP	(RP1_ETH_BASE + 0x01c)	/* transmit ring, low half */
#define GEM_RSR		(RP1_ETH_BASE + 0x020)	/* receive status */
#define GEM_IDR		(RP1_ETH_BASE + 0x02c)	/* interrupt disable */
#define GEM_MAN		(RP1_ETH_BASE + 0x034)	/* PHY maintenance */
#define GEM_HRB		(RP1_ETH_BASE + 0x080)	/* multicast hash, bottom */
#define GEM_HRT		(RP1_ETH_BASE + 0x084)	/* multicast hash, top */
#define GEM_SA1B	(RP1_ETH_BASE + 0x088)	/* address 1, low four bytes */
#define GEM_SA1T	(RP1_ETH_BASE + 0x08c)	/* address 1, top two bytes */
#define GEM_TBQPH	(RP1_ETH_BASE + 0x4c8)	/* transmit ring, high half */
#define GEM_RBQPH	(RP1_ETH_BASE + 0x4d4)	/* receive ring, high half */
#define GEM_USRIO	(RP1_ETH_BASE + 0x00c)	/* what the pins carry */
#define GEM_PBUFRXCUT	(RP1_ETH_BASE + 0x044)	/* receive cut-through */
#define GEM_AMP		(RP1_ETH_BASE + 0x054)	/* AXI requests in flight */
#define GEM_DCFG1	(RP1_ETH_BASE + 0x280)	/* design: data bus width */
#define GEM_DCFG6	(RP1_ETH_BASE + 0x294)	/* design: the queues there are */
#define GEM_TBQP_Q(q)	(RP1_ETH_BASE + 0x440 + ( (q) - 1 ) * 4)	/* transmit ring of queue q >= 1 */
#define GEM_RBQP_Q(q)	(RP1_ETH_BASE + 0x480 + ( (q) - 1 ) * 4)	/* receive ring of queue q >= 1 */

#define USRIO_RGMII	0x00000001
#define AMP_PIPES	0x00010808		/* 8 reads, 8 writes, write fill: the board's device tree */
#define AMP_MASK	0x0001ffff

/* The RP1's clocks for the block, and its own words about it (RP1 window offsets) */
#define RP1_PLL_SYS_SEC	(RP1_WINDOW_BASE + 0x020014)	/* the secondary output of pll_sys */
#define PLL_SEC_DIV_SHIFT 8
#define PLL_SEC_DIV_MASK 0x00001f00
#define PLL_SEC_RST	0x00010000
#define RP1_CLK_ETH_CTRL (RP1_WINDOW_BASE + 0x018064)	/* the RGMII clock, 125MHz */
#define RP1_CLK_ETH_DIV	(RP1_WINDOW_BASE + 0x018068)
#define RP1_CLK_ETH_SEL	(RP1_WINDOW_BASE + 0x018070)
#define RP1_CLK_TSU_CTRL (RP1_WINDOW_BASE + 0x018134)	/* the timestamp unit, 50MHz */
#define RP1_CLK_TSU_DIV	(RP1_WINDOW_BASE + 0x018138)
#define RP1_CLK_TSU_SEL	(RP1_WINDOW_BASE + 0x018140)
#define CLK_CTRL_ENABLE	0x00000800
#define CLK_CTRL_AUXSRC	0x000003e0		/* 0: pll_sys_sec for the one, xosc for the other */
#define ETH_CFG_CONTROL	(RP1_WINDOW_BASE + 0x104000)
#define ETH_CFG_STATUS	(RP1_WINDOW_BASE + 0x104004)
#define ETH_CFG_CLKGEN	(RP1_WINDOW_BASE + 0x104014)
#define ETH_CFG_MEM_PD	0x00000010		/* the block's memories powered down */

#define NCR_LB		0x00000001		/* loop back at the pins */
#define NCR_LLB		0x00000002		/* loop back inside the block */
#define NCR_RE		0x00000004		/* receive enable */
#define NCR_TE		0x00000008		/* transmit enable */
#define NCR_MPE		0x00000010		/* the MDIO bus runs */
#define NCR_CLRSTAT	0x00000020		/* zero the counters */
#define NCR_TSTART	0x00000200		/* start on the transmit ring */

#define NCFGR_SPD100	0x00000001
#define NCFGR_FD	0x00000002		/* full duplex */
#define NCFGR_CAF	0x00000010		/* take frames of any address */
#define NCFGR_NBC	0x00000020		/* drop broadcast */
#define NCFGR_MTI	0x00000040		/* multicast through the hash */
#define NCFGR_GBE	0x00000400		/* gigabit */
#define NCFGR_CLK_SHIFT	18			/* MDC is pclk over a divisor */
#define NCFGR_CLK_96	(5U << NCFGR_CLK_SHIFT)	/* the slowest one there is */
#define NCFGR_DRFCS	0x00020000		/* the CRC is not handed up with the frame */
#define NCFGR_DBW_SHIFT	21			/* data bus width: 0 32, 1 64, 2 128 bits */

#define NSR_IDLE	0x00000004		/* the MDIO bus is free */

#define DMACFG_FBLDO16	0x00000010		/* bursts of up to 16 */
#define DMACFG_TXPBMS	0x00000400		/* the whole transmit memory */
#define DMACFG_RXBMS	0x00000300		/* the whole receive memory */
#define DMACFG_RXBS_SHIFT 16			/* buffer size, in 64 byte units */
#define DMACFG_ADDR64	0x40000000		/* 64 bit descriptors */

#define TSR_UBR		0x00000001		/* ran into a used descriptor */
#define TSR_COL		0x00000002
#define TSR_TFC		0x00000004		/* the frame was given up on */
#define TSR_TXCOMP	0x00000020
#define TSR_HRESP	0x00000100		/* the bus faulted */

#define RSR_BNA		0x00000001		/* no buffer was free */
#define RSR_REC		0x00000002		/* a frame arrived */
#define RSR_OVR		0x00000004		/* the block ran out of memory */
#define RSR_HRESP	0x00000008

/* PHY maintenance: a whole read or write in one word */
#define MAN_SOF		0x40000000
#define MAN_WRITE	0x10000000
#define MAN_READ	0x20000000
#define MAN_CODE	0x00020000
#define MAN_PHYA_SHIFT	23
#define MAN_REGA_SHIFT	18

/* The registers every PHY has */
#define MII_BMCR	0
#define MII_BMSR	1
#define MII_ID1		2
#define MII_ID2		3
#define MII_ANLPAR	5
#define MII_GBCR	9
#define MII_GBSR	10

#define BMCR_ANENABLE	0x1000
#define BMCR_ANRESTART	0x0200

#define BMSR_LSTATUS	0x0004
#define BMSR_ANEGCOMP	0x0020

#define ANLPAR_100FULL	0x0100
#define ANLPAR_100HALF	0x0080
#define ANLPAR_10FULL	0x0040

#define GBCR_1000FULL	0x0200
#define GBSR_1000FULL	0x0800
#define GBSR_1000HALF	0x0400

/* ---------------------------------------------------------------- rings */
#define ETH_PHY_RESET_PIN	32	/* RP1 GPIO 32 holds the PHY in reset while low */
#define ETH_PHY_ADDR		1	/* where the board's PHY answers (its device tree) */
#define ETH_RX_RING	64		/* a page's worth of frames from many connections at once */
#define ETH_TX_RING	8
#define ETH_BUF_ORDER	6		/* 2^6 pages hold a buffer per descriptor */
#define ETH_BUF_LEN	2048		/* a whole frame, a multiple of 64 */

/*
 * A descriptor in the 64 bit layout: the address in two halves, with the
 * flags in the low bits of the first, and the per-frame word between.
 */
typedef struct {
	UW	addr;			/* low half; bits 0 and 1 are flags */
	UW	ctrl;
	UW	addr_hi;
	UW	unused;
} ETH_DESC;

#define RXD_OWN		0x00000001	/* set by the controller when filled */
#define RXD_WRAP	0x00000002	/* the last descriptor of the ring */
#define RXD_ADDR_MASK	0xfffffffc

#define RXC_LEN_MASK	0x00001fff
#define RXC_SOF		0x00004000
#define RXC_EOF		0x00008000

#define TXC_LEN_MASK	0x00003fff
#define TXC_LAST	0x00008000	/* the end of the frame */
#define TXC_WRAP	0x40000000	/* the last descriptor of the ring */
#define TXC_USED	0x80000000	/* set by the controller when sent */

typedef struct {
	ETH_DESC	*rxd, *txd;	/* the two rings (linear map) */
	UD		rxd_pa, txd_pa;
	UB		*rxbuf, *txbuf;
	UD		rxbuf_pa, txbuf_pa;
	INT		rx_next;	/* the descriptor to look at next */
	INT		tx_next;
	UB		mac[NET_MAC_LEN];
	UINT		phy;		/* its address on the MDIO bus */
	UD		n_sent, n_recv, n_drop;
	UINT		idle;		/* empty looks at the ring since the last frame */
	UINT		link_polls;	/* empty looks since the link was last looked at */
	ID		mtxid;		/* the sending side */
	ID		mtxid_mdio;	/* the management bus, used from more than one task */
	ID		mtxid_rx;	/* and the receiving side, which waits */
	BOOL		up;		/* the link carries */
	BOOL		told_tx, told_rx;	/* a failure of each kind has been described */
	BOOL		rx_stalled;	/* the receiver found no free descriptor and stopped */
	UINT		n_restart;	/* times the receiver was started again */
} ETH_DEV;

LOCAL ETH_DEV	eth;
LOCAL BOOL	eth_found = FALSE;

/* ---------------------------------------------------------------- helpers */

/*
 * What the controller says about itself when a frame does not go or
 * does not come: once per kind of failure, since the receiving side
 * would otherwise say it every time it looks.
 */
#define GEM_FRAMES_TX	(RP1_ETH_BASE + 0x108)	/* frames sent, statistics */
#define GEM_FRAMES_RX	(RP1_ETH_BASE + 0x158)	/* frames received, statistics */
#define RP1_ETH_CFG	(RP1_WINDOW_BASE + 0x104000)	/* the RP1's own words about the block */

LOCAL void eth_dump( CONST char *why, UW st )
{
	tm_printf((UB *)"eth: %s (%08x): ncr %08x ncfgr %08x dmacfg %08x tsr %08x rsr %08x "
		  "tx %d rx %d\n", why, st, in_w(GEM_NCR), in_w(GEM_NCFGR), in_w(GEM_DMACFG),
		  in_w(GEM_TSR), in_w(GEM_RSR), in_w(GEM_FRAMES_TX), in_w(GEM_FRAMES_RX));
	tm_printf((UB *)"eth: rings %08x%08x %08x%08x, eth_cfg %08x %08x %08x %08x %08x\n",
		  in_w(GEM_RBQPH), in_w(GEM_RBQP), in_w(GEM_TBQPH), in_w(GEM_TBQP),
		  in_w(RP1_ETH_CFG + 0x0), in_w(RP1_ETH_CFG + 0x4), in_w(RP1_ETH_CFG + 0x8),
		  in_w(RP1_ETH_CFG + 0xc), in_w(RP1_ETH_CFG + 0x10));
}

/*
 * The controller reaches memory through the bridge, which puts system
 * memory at a fixed offset in its own address space.
 */
LOCAL UD bus_addr( UD pa )
{
	return pa + RP1_DMA_BUS_OFFSET;
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

/* ---------------------------------------------------------------- MDIO */

/*
 * The block does the framing on the MDIO bus, so a transfer is one write
 * and a wait for the bus to go idle again.
 */
LOCAL ER mdio_wait( void )
{
	INT	spin;

	for ( spin = 0; spin < 1000000; spin++ ) {
		if ( (in_w(GEM_NSR) & NSR_IDLE) != 0 ) {
			return E_OK;
		}
	}
	return E_TMOUT;
}

/* One transaction at a time on the bus: the receiving task and a caller of net_stat both ask */
LOCAL void mdio_lock( void )
{
	if ( eth.mtxid_mdio > 0 ) tk_loc_mtx(eth.mtxid_mdio, TMO_FEVR);
}

LOCAL void mdio_unlock( void )
{
	if ( eth.mtxid_mdio > 0 ) tk_unl_mtx(eth.mtxid_mdio);
}

LOCAL INT phy_read( UINT phy, UINT reg )
{
	INT	v = E_TMOUT;

	mdio_lock();
	if ( mdio_wait() >= E_OK ) {
		out_w(GEM_MAN, MAN_SOF | MAN_READ | MAN_CODE
				| (phy << MAN_PHYA_SHIFT) | (reg << MAN_REGA_SHIFT));
		if ( mdio_wait() >= E_OK ) {
			v = (INT)(in_w(GEM_MAN) & 0xffff);
		}
	}
	mdio_unlock();
	return v;
}

LOCAL ER phy_write( UINT phy, UINT reg, UINT val )
{
	ER	er = E_TMOUT;

	mdio_lock();
	if ( mdio_wait() >= E_OK ) {
		out_w(GEM_MAN, MAN_SOF | MAN_WRITE | MAN_CODE
				| (phy << MAN_PHYA_SHIFT) | (reg << MAN_REGA_SHIFT)
				| (val & 0xffff));
		er = mdio_wait();
	}
	mdio_unlock();
	return er;
}

/*
 * Find the PHY: the first address on the bus whose identity is neither
 * all ones nor all zeroes.
 */
LOCAL ER phy_find( void )
{
	UINT	a;
	INT	id1, id2;
	INT	seen1 = 0x10000, seen2 = 0x10000;	/* what address 1 said */

	for ( a = 0; a < 32; a++ ) {
		id1 = phy_read(a, MII_ID1);
		id2 = phy_read(a, MII_ID2);
		if ( a == ETH_PHY_ADDR ) {
			seen1 = id1;
			seen2 = id2;
		}
		if ( id1 < 0 || id2 < 0 ) {
			tm_printf((UB *)"eth: management bus timed out at address %d (nsr %08x)\n",
				  (INT)a, in_w(GEM_NSR));
			return E_IO;
		}
		if ( id1 != 0xffff && (id1 != 0 || id2 != 0) ) {
			eth.phy = a;
			return E_OK;
		}
	}
	/* every address answered all ones or all zeroes: nobody drives the data line */
	{
		UW	st[4] = { 0, 0, 0, 0 };

		(void)rp1_gpio_state(ETH_PHY_RESET_PIN, st);
		tm_printf((UB *)"eth: address %d reads %04x %04x, nsr %08x, reset pin %d "
			  "(ctrl %08x pad %08x out %08x oe %08x)\n",
			  ETH_PHY_ADDR, seen1, seen2, in_w(GEM_NSR),
			  rp1_gpio_read(ETH_PHY_RESET_PIN), st[0], st[1], st[2], st[3]);
	}
	return E_NOEXS;
}

/*
 * Offer everything the PHY can do and wait for the other end to agree.
 * What comes back is the speed and the duplex, which the controller has
 * to be told about separately.
 */
LOCAL ER phy_speed( void );

LOCAL ER phy_up( void )
{
	INT	bmsr;
	INT	spin;

	if ( phy_write(eth.phy, MII_GBCR, GBCR_1000FULL) < E_OK
	  || phy_write(eth.phy, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART) < E_OK ) {
		return E_IO;
	}

	/* up to about five seconds, which is what a gigabit link takes after a reset */
	for ( spin = 0; spin < 500; spin++ ) {
		bmsr = phy_read(eth.phy, MII_BMSR);
		if ( bmsr < 0 ) {
			return E_IO;
		}
		/* the link bit latches low, so it is read twice */
		bmsr = phy_read(eth.phy, MII_BMSR);
		if ( (bmsr & (BMSR_LSTATUS | BMSR_ANEGCOMP))
				== (BMSR_LSTATUS | BMSR_ANEGCOMP) ) {
			break;
		}
		tk_dly_tsk(10);
	}
	if ( spin >= 500 ) {
		eth.up = FALSE;
		return E_TMOUT;			/* nothing plugged in, or slower than that */
	}
	return phy_speed();
}

/*
 * The controller told what the two ends agreed on: gigabit, 100 or 10,
 * full or half. Done whenever the link is found up, not only at start:
 * a link that settles later would otherwise carry nothing, the
 * controller still set for 10 half duplex.
 */
LOCAL ER phy_speed( void )
{
	INT	gbsr, lpa;
	UW	cfg;

	gbsr = phy_read(eth.phy, MII_GBSR);
	lpa  = phy_read(eth.phy, MII_ANLPAR);
	if ( gbsr < 0 || lpa < 0 ) {
		return E_IO;
	}

	cfg = in_w(GEM_NCFGR) & ~(NCFGR_SPD100 | NCFGR_FD | NCFGR_GBE);
	if ( (gbsr & (GBSR_1000FULL | GBSR_1000HALF)) != 0 ) {
		cfg |= NCFGR_GBE;
		if ( (gbsr & GBSR_1000FULL) != 0 ) {
			cfg |= NCFGR_FD;
		}
	} else if ( (lpa & (ANLPAR_100FULL | ANLPAR_100HALF)) != 0 ) {
		cfg |= NCFGR_SPD100;
		if ( (lpa & ANLPAR_100FULL) != 0 ) {
			cfg |= NCFGR_FD;
		}
	} else if ( (lpa & ANLPAR_10FULL) != 0 ) {
		cfg |= NCFGR_FD;
	}
	out_w(GEM_NCFGR, cfg);
	eth.up = TRUE;
	tm_printf((UB *)"eth: link up %s %s\n",
		  ( cfg & NCFGR_GBE ) ? "1000" : ( cfg & NCFGR_SPD100 ) ? "100" : "10",
		  ( cfg & NCFGR_FD ) ? "full" : "half");

	return E_OK;
}

/*
 * Look at the link again: set the controller up for it when it has come
 * up, note it when it has gone.
 */
LOCAL void link_check( void )
{
	INT	bmsr;

	(void)phy_read(eth.phy, MII_BMSR);	/* the link bit latches low */
	bmsr = phy_read(eth.phy, MII_BMSR);
	if ( bmsr < 0 ) {
		return;
	}
	if ( (bmsr & BMSR_LSTATUS) != 0 ) {
		if ( !eth.up && (bmsr & BMSR_ANEGCOMP) != 0 ) {
			(void)phy_speed();
		}
	} else if ( eth.up ) {
		eth.up = FALSE;
		tm_printf((UB *)"eth: link down\n");
	}
}

/* ---------------------------------------------------------------- rings */

/*
 * The queues beyond the first, which this driver does not use, pointed
 * at a descriptor that says "nothing here": left at address 0, the
 * transmitter could fetch a descriptor from the foot of memory. The two
 * spare descriptors sit in the ring page after the rings.
 */
#define ETH_PARK_OFS	0x800

LOCAL void queues_park( void )
{
	ETH_DESC	*t = (ETH_DESC *)( (UB *)eth.rxd + ETH_PARK_OFS );
	ETH_DESC	*r = t + 1;
	UD		tpa = bus_addr(eth.rxd_pa + ETH_PARK_OFS);
	UD		rpa = tpa + sizeof(ETH_DESC);
	UW		qmask = ( in_w(GEM_DCFG6) >> 1 ) & 0x7f;
	INT		q;

	t->addr = 0;
	t->ctrl = TXC_USED | TXC_WRAP;
	t->addr_hi = 0;
	r->addr = RXD_OWN | RXD_WRAP;
	r->ctrl = 0;
	r->addr_hi = 0;
	dcache_flush(t, 2 * sizeof(ETH_DESC));
	for ( q = 1; q <= 7; q++ ) {
		if ( ( qmask & ( 1U << ( q - 1 ) ) ) != 0 ) {
			out_w(GEM_TBQP_Q(q), (UW)tpa);
			out_w(GEM_RBQP_Q(q), (UW)rpa);
		}
	}
}

LOCAL void rings_init( void )
{
	INT	i;
	UD	pa;

	for ( i = 0; i < ETH_RX_RING; i++ ) {
		pa = bus_addr(eth.rxbuf_pa + (UD)i * ETH_BUF_LEN);
		eth.rxd[i].addr    = (UW)pa & RXD_ADDR_MASK;
		eth.rxd[i].addr_hi = (UW)(pa >> 32);
		eth.rxd[i].ctrl    = 0;
		eth.rxd[i].unused  = 0;
	}
	eth.rxd[ETH_RX_RING - 1].addr |= RXD_WRAP;

	for ( i = 0; i < ETH_TX_RING; i++ ) {
		pa = bus_addr(eth.txbuf_pa + (UD)i * ETH_BUF_LEN);
		eth.txd[i].addr    = (UW)pa;
		eth.txd[i].addr_hi = (UW)(pa >> 32);
		eth.txd[i].ctrl    = TXC_USED;	/* nothing to send yet */
		eth.txd[i].unused  = 0;
	}
	eth.txd[ETH_TX_RING - 1].ctrl |= TXC_WRAP;

	eth.rx_next = 0;
	eth.tx_next = 0;
	dcache_flush(eth.rxd, sizeof(ETH_DESC) * ETH_RX_RING);
	dcache_flush(eth.txd, sizeof(ETH_DESC) * ETH_TX_RING);
}

/*
 * The controller stops receiving when it finds no free descriptor, and
 * stays stopped. With the ring read to its end, the receiver is turned
 * off, the ring given back whole from its start and the receiver turned
 * on again.
 */
LOCAL void rx_restart( void )
{
	INT	i;
	UD	pa;

	out_w(GEM_NCR, in_w(GEM_NCR) & ~NCR_RE);
	for ( i = 0; i < ETH_RX_RING; i++ ) {
		eth.rxd[i].addr &= ~RXD_OWN;
		eth.rxd[i].ctrl = 0;
	}
	eth.rx_next = 0;
	dcache_flush(eth.rxd, sizeof(ETH_DESC) * ETH_RX_RING);
	pa = bus_addr(eth.rxd_pa);
	out_w(GEM_RBQP,  (UW)pa);
	out_w(GEM_RBQPH, (UW)(pa >> 32));
	out_w(GEM_NCR, in_w(GEM_NCR) | NCR_RE);
	eth.rx_stalled = FALSE;
	if ( eth.n_restart++ < 4 ) {
		tm_printf((UB *)"eth: receiving started again (%d)\n", (INT)eth.n_restart);
	}
}

/* ---------------------------------------------------------------- interface */

EXPORT ER net_get_mac( UB *mac )
{
	INT	i;

	if ( !eth_found || mac == NULL ) {
		return E_NOEXS;
	}
	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		mac[i] = eth.mac[i];
	}

	return E_OK;
}

EXPORT ER net_send( CONST void *frame, SZ len )
{
	ETH_DESC	*d;
	UB		*buf;
	INT		spin;
	UW		tsr;
	ER		er;

	if ( !eth_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL || len < NET_FRAME_MIN || len > NET_FRAME_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(eth.mtxid, TMO_FEVR);

	d = &eth.txd[eth.tx_next];
	dcache_flush(d, sizeof(*d));
	if ( (d->ctrl & TXC_USED) == 0 ) {
		tk_unl_mtx(eth.mtxid);
		return E_BUSY;			/* the ring has not drained */
	}

	buf = eth.txbuf + (UD)eth.tx_next * ETH_BUF_LEN;
	knl_memcpy(buf, frame, (INT)len);
	dcache_flush(buf, (UD)len);

	/* the controller takes the descriptor when the used bit goes away */
	d->ctrl = ((UW)len & TXC_LEN_MASK) | TXC_LAST
		| (( eth.tx_next == ETH_TX_RING - 1 ) ? TXC_WRAP : 0);
	dcache_flush(d, sizeof(*d));

	out_w(GEM_NCR, in_w(GEM_NCR) | NCR_TSTART);
	(void)in_w(GEM_NCR);		/* a posted write can be lost on the way; the read brings it home */

	/* it comes back with the used bit set again */
	er = E_TMOUT;
	for ( spin = 0; spin < 1000; spin++ ) {
		dcache_flush(d, sizeof(*d));
		if ( (d->ctrl & TXC_USED) != 0 ) {
			er = E_OK;
			break;
		}
		tk_dly_tsk(1);
	}
	if ( er >= E_OK ) {
		tsr = in_w(GEM_TSR);
		if ( (tsr & (TSR_TFC | TSR_HRESP)) != 0 ) {
			er = E_IO;		/* given up on, or a bus fault */
		}
		out_w(GEM_TSR, tsr);		/* the bits clear on a write */
	}
	if ( er < E_OK && !eth.told_tx ) {
		eth.told_tx = TRUE;
		dcache_flush(d, sizeof(*d));
		eth_dump("a frame did not go", d->ctrl);
	}
	if ( er >= E_OK ) {
		eth.n_sent++;
		eth.idle = 0;		/* an answer may be on its way */
	}
	eth.tx_next = ( eth.tx_next + 1 ) % ETH_TX_RING;

	tk_unl_mtx(eth.mtxid);

	return er;
}

/*
 * How long to wait before looking at the ring again. Every millisecond
 * while frames are coming, so that an answer is picked up at once; once
 * nothing has come for a while (ETH_BUSY_POLLS looks), every
 * ETH_IDLE_POLL_MS, so that an idle machine is not woken a thousand
 * times a second by a reader that finds nothing.
 */
#define ETH_BUSY_POLLS		200
#define ETH_IDLE_POLL_MS	20

EXPORT INT net_recv( void *frame, SZ size, TMO tmout )
{
	ETH_DESC	*d;
	UB		*buf;
	INT		len;
	INT		got = E_TMOUT;
	TMO		left = tmout;
	UW		rsr;

	if ( !eth_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL ) {
		return E_PAR;
	}
	/* Whoever reads the card holds it while waiting, and the
	   protocol stack keeps a reader on it for good. A second
	   caller is told so rather than left waiting. */
	if ( tk_loc_mtx(eth.mtxid_rx, tmout) < E_OK ) {
		return E_BUSY;
	}

	for (;;) {
		d = &eth.rxd[eth.rx_next];
		dcache_flush(d, sizeof(*d));

		if ( (d->addr & RXD_OWN) != 0 ) {
			buf = eth.rxbuf + (UD)eth.rx_next * ETH_BUF_LEN;
			len = (INT)(d->ctrl & RXC_LEN_MASK);

			if ( len > 0 && len <= (INT)size ) {
				dcache_flush(buf, (UD)len);
				knl_memcpy(frame, buf, len);
				eth.n_recv++;
				eth.idle = 0;
				got = len;
			} else {
				eth.n_drop++;	/* too long for the caller */
			}

			/* the descriptor goes back to the controller */
			d->ctrl = 0;
			d->addr &= ~RXD_OWN;
			dcache_flush(d, sizeof(*d));
			eth.rx_next = ( eth.rx_next + 1 ) % ETH_RX_RING;

			if ( got > 0 ) {
				break;
			}
			continue;		/* dropped: look at the next one */
		}

		/* nothing left in the ring: a receiver that stopped is started again */
		if ( eth.rx_stalled ) {
			rx_restart();
		}
		if ( tmout == TMO_POL ) {
			break;
		}
		{
			TMO	wait = ( eth.idle < ETH_BUSY_POLLS ) ? 1 : ETH_IDLE_POLL_MS;

			if ( eth.idle < ETH_BUSY_POLLS ) eth.idle++;
			if ( ++eth.link_polls >= 1000 / ETH_IDLE_POLL_MS ) {
				eth.link_polls = 0;
				link_check();
			}
			if ( tmout > 0 && wait > left ) wait = left;
			tk_dly_tsk(wait);
			if ( tmout > 0 && ( left -= wait ) <= 0 ) {
				break;
			}
		}
	}

	rsr = in_w(GEM_RSR);
	if ( (rsr & (RSR_BNA | RSR_OVR | RSR_HRESP)) != 0 ) {
		if ( !eth.told_rx ) {
			eth.told_rx = TRUE;
			eth_dump("receiving stopped", rsr);
		}
		eth.n_drop++;			/* the ring filled up or faulted */
		out_w(GEM_RSR, rsr);
		eth.rx_stalled = TRUE;		/* started again once the ring is read */
	}

	tk_unl_mtx(eth.mtxid_rx);

	return got;
}

EXPORT ER net_stat( T_NETSTAT *st )
{
	if ( !eth_found || st == NULL ) {
		return E_NOEXS;
	}
	link_check();
	st->sent = eth.n_sent;
	st->recv = eth.n_recv;
	st->drop = eth.n_drop;
	st->up   = eth.up;

	return E_OK;
}

/* ---------------------------------------------------------------- start-up */

/*
 * The address the board was given. The firmware writes it into the
 * controller before it hands over, so it is read back out; a board that
 * comes up without one gets a locally administered address instead.
 */
LOCAL ER board_mac( UB *mac );

/*
 * The block's two clocks, set the way the board's device tree has them:
 * the RGMII clock at 125MHz from the secondary output of pll_sys (1GHz
 * over 8), the timestamp unit at 50MHz straight from the crystal. The
 * values the chip comes up with are not ones the block works at: the
 * link comes up and not a frame moves.
 */
LOCAL void rp1_clk_set( UBINT ctrl, UBINT div, UW divider )
{
	out_w(ctrl, in_w(ctrl) & ~CLK_CTRL_ENABLE);
	out_w(div, divider);
	out_w(ctrl, ( in_w(ctrl) & ~CLK_CTRL_AUXSRC ) | CLK_CTRL_ENABLE);
}

LOCAL void eth_clocks( void )
{
	UW	v = in_w(RP1_PLL_SYS_SEC);

	if ( ( ( v & PLL_SEC_DIV_MASK ) >> PLL_SEC_DIV_SHIFT ) != 8 || ( v & PLL_SEC_RST ) != 0 ) {
		v = ( v & ~PLL_SEC_DIV_MASK ) | ( 8 << PLL_SEC_DIV_SHIFT );
		out_w(RP1_PLL_SYS_SEC, v | PLL_SEC_RST);
		out_w(RP1_PLL_SYS_SEC, v & ~PLL_SEC_RST);
	}
	rp1_clk_set(RP1_CLK_ETH_CTRL, RP1_CLK_ETH_DIV, 1);
	rp1_clk_set(RP1_CLK_TSU_CTRL, RP1_CLK_TSU_DIV, 1);

	/* the block's memories are to be powered */
	if ( ( in_w(ETH_CFG_CONTROL) & ETH_CFG_MEM_PD ) != 0 ) {
		out_w(ETH_CFG_CONTROL, in_w(ETH_CFG_CONTROL) & ~ETH_CFG_MEM_PD);
	}
	tm_printf((UB *)"eth: clocks sel %08x %08x, pll_sys_sec %08x, eth_cfg control %08x "
		  "status %08x clkgen %08x\n", in_w(RP1_CLK_ETH_SEL), in_w(RP1_CLK_TSU_SEL),
		  in_w(RP1_PLL_SYS_SEC), in_w(ETH_CFG_CONTROL), in_w(ETH_CFG_STATUS),
		  in_w(ETH_CFG_CLKGEN));
}

LOCAL void mac_read( void )
{
	UW	bottom = in_w(GEM_SA1B);
	UW	top    = in_w(GEM_SA1T);

	eth.mac[0] = (UB)bottom;
	eth.mac[1] = (UB)(bottom >> 8);
	eth.mac[2] = (UB)(bottom >> 16);
	eth.mac[3] = (UB)(bottom >> 24);
	eth.mac[4] = (UB)top;
	eth.mac[5] = (UB)(top >> 8);

	if ( (eth.mac[0] | eth.mac[1] | eth.mac[2]
	    | eth.mac[3] | eth.mac[4] | eth.mac[5]) == 0
	  || (eth.mac[0] & 0x01) != 0 ) {
		/* the firmware leaves the controller's own registers empty; it knows the board's */
		if ( board_mac(eth.mac) == E_OK ) {
			return;
		}
		eth.mac[0] = 0x02;		/* locally administered */
		eth.mac[1] = 0x00;
		eth.mac[2] = 0x00;
		eth.mac[3] = 0x00;
		eth.mac[4] = 0x00;
		eth.mac[5] = 0x01;
	}
}

/*
 * The address the board was given, from the firmware's property
 * interface (tag GET_BOARD_MAC_ADDRESS): six bytes, most significant
 * first.
 */
#define TAG_BOARD_MAC		0x00010003U

IMPORT ER knl_mbox_property( UW *buf );

LOCAL ER board_mac( UB *mac )
{
	LOCAL UW	b[8] __attribute__((aligned(64)));
	UB		*v = (UB *)&b[5];
	INT		i;

	b[0] = 8 * 4;		/* total size */
	b[1] = 0;		/* request */
	b[2] = TAG_BOARD_MAC;
	b[3] = 8;		/* value buffer size */
	b[4] = 0;		/* request size */
	b[5] = 0;
	b[6] = 0;
	b[7] = 0;		/* end tag */
	if ( knl_mbox_property(b) != E_OK || (b[4] & 0x80000000U) == 0 ) {
		return E_IO;
	}
	if ( (v[0] | v[1] | v[2] | v[3] | v[4] | v[5]) == 0 || (v[0] & 0x01) != 0 ) {
		return E_OBJ;
	}
	for ( i = 0; i < 6; i++ ) {
		mac[i] = v[i];
	}
	return E_OK;
}

LOCAL void mac_write( void )
{
	out_w(GEM_SA1B, (UW)eth.mac[0] | ((UW)eth.mac[1] << 8)
			| ((UW)eth.mac[2] << 16) | ((UW)eth.mac[3] << 24));
	out_w(GEM_SA1T, (UW)eth.mac[4] | ((UW)eth.mac[5] << 8));
}

EXPORT INT knl_eth_rp1_init( void )
{
	T_CMTX	cmtx;
	PFRAME	*pf;
	UW	cfg;
	UD	pa;

	/* the window has to be there before anything is read out of it */
	if ( in_w(GEM_NCFGR) == 0xFFFFFFFF ) {
		/* the link is down or not mapped, or the block has no clock */
		tm_printf((UB *)"eth: the GEM does not answer at %08x%08x\n",
			  (UW)((UD)RP1_ETH_PA >> 32), (UW)RP1_ETH_PA);
		return 0;
	}

	eth_clocks();

	/* stop everything while the rings are built */
	out_w(GEM_NCR, NCR_CLRSTAT);
	out_w(GEM_IDR, 0xffffffff);		/* polled: no interrupt wanted */
	out_w(GEM_TSR, 0xffffffff);
	out_w(GEM_RSR, 0xffffffff);
	out_w(GEM_PBUFRXCUT, 0);
	out_w(GEM_HRB, 0);
	out_w(GEM_HRT, 0);

	mac_read();

	/* one page holds both rings */
	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	eth.rxd_pa = knl_pf_to_pa(pf);
	eth.txd_pa = eth.rxd_pa + sizeof(ETH_DESC) * ETH_RX_RING;
	eth.rxd = (ETH_DESC *)PA2VA(eth.rxd_pa);
	eth.txd = (ETH_DESC *)PA2VA(eth.txd_pa);

	/* a buffer per descriptor: 72 of 2KB, which is 36 pages */
	pf = knl_alloc_pages(ETH_BUF_ORDER, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	eth.rxbuf_pa = knl_pf_to_pa(pf);
	eth.txbuf_pa = eth.rxbuf_pa + (UD)ETH_RX_RING * ETH_BUF_LEN;
	eth.rxbuf = (UB *)PA2VA(eth.rxbuf_pa);
	eth.txbuf = (UB *)PA2VA(eth.txbuf_pa);

	rings_init();

	pa = bus_addr(eth.rxd_pa);
	out_w(GEM_RBQP,  (UW)pa);
	out_w(GEM_RBQPH, (UW)(pa >> 32));
	pa = bus_addr(eth.txd_pa);
	out_w(GEM_TBQP,  (UW)pa);
	out_w(GEM_TBQPH, (UW)(pa >> 32));
	queues_park();

	out_w(GEM_AMP, ( in_w(GEM_AMP) & ~AMP_MASK ) | AMP_PIPES);
	out_w(GEM_DMACFG, DMACFG_FBLDO16 | DMACFG_RXBMS | DMACFG_TXPBMS | DMACFG_ADDR64
			| ((ETH_BUF_LEN / 64) << DMACFG_RXBS_SHIFT));
	out_w(GEM_USRIO, USRIO_RGMII);

	/*
	 * The MDIO clock slow enough for any pclk, the CRC kept back, and
	 * the width of the path to memory as the block was built with it
	 * (the RP1's is 128 bits; left at 32, the transfers fault).
	 */
	cfg = NCFGR_CLK_96 | NCFGR_MTI | NCFGR_DRFCS;
	switch ( ( in_w(GEM_DCFG1) >> 25 ) & 7 ) {
	case 4:	cfg |= 2U << NCFGR_DBW_SHIFT;	break;
	case 2:	cfg |= 1U << NCFGR_DBW_SHIFT;	break;
	default: break;
	}
	out_w(GEM_NCFGR, cfg);
	mac_write();

	/*
	 * The PHY is held in reset by an RP1 pin until it is let go (the
	 * board's device tree: GPIO 32, active low, 5ms); held there, it
	 * does not answer on the management bus.
	 */
	if ( rp1_gpio_set_dir(ETH_PHY_RESET_PIN, GPIO_DIR_OUT) >= E_OK ) {
		rp1_gpio_write(ETH_PHY_RESET_PIN, 0);
		tk_dly_tsk(5);
		rp1_gpio_write(ETH_PHY_RESET_PIN, 1);
		tk_dly_tsk(20);
	}

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	eth.mtxid_mdio = tk_cre_mtx(&cmtx);

	/* the bus has to run before the PHY answers */
	out_w(GEM_NCR, NCR_MPE | NCR_CLRSTAT);
	if ( phy_find() < E_OK ) {
		out_w(GEM_NCR, 0);
		tm_printf((UB *)"eth: no PHY answers on the management bus (ncfgr %08x)\n",
			  in_w(GEM_NCFGR));
		return 0;			/* no PHY: no socket to use */
	}
	phy_up();				/* an empty socket is not a failure */

	out_w(GEM_NCR, in_w(GEM_NCR) | NCR_RE | NCR_TE);

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	eth.mtxid = tk_cre_mtx(&cmtx);
	eth.mtxid_rx = tk_cre_mtx(&cmtx);
	if ( eth.mtxid <= 0 || eth.mtxid_rx <= 0 ) {
		out_w(GEM_NCR, 0);
		return E_SYS;
	}

	eth_found = TRUE;
	tm_printf((UB *)"eth: %02x:%02x:%02x:%02x:%02x:%02x phy %d %s\n",
		  eth.mac[0], eth.mac[1], eth.mac[2],
		  eth.mac[3], eth.mac[4], eth.mac[5],
		  eth.phy, ( eth.up ) ? "up" : "down");

	return 1;
}

#endif /* RPI5 */
