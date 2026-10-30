/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	eth_genet.c
 *	Ethernet of the BCM2711: the RJ45 socket of the Raspberry Pi 4, the
 *	Raspberry Pi 400 and the Compute Module 4's carrier (design 10.4;
 *	docs/tessronos/03-platform.md 3.6).
 *
 *	The controller is the SoC's GENET, version 5, at 0xFD58_0000. Its
 *	PHY is a Broadcom BCM54213PE on GENET's own management bus, at MDIO
 *	address 1 on the Pi 4 and the Pi 400 and at 0 on the Compute Module
 *	4 (include/ts/board.h), joined to it by RGMII with the receive clock
 *	delayed in the PHY and the transmit clock in the controller
 *	("rgmii-rxid" in the device tree). The PHY is driven through its
 *	standard registers, and through two of Broadcom's own for the clock
 *	delays.
 *
 *	The block is in parts at fixed offsets: the system registers (port
 *	mode, buffer flush), the RGMII control, two interrupt controllers,
 *	the receive buffer control, the UniMAC (the MAC proper, with the
 *	management bus at UMAC + 0x614, the device tree's mdio@e14), and the
 *	receive and transmit DMA at 0x2000 and 0x4000. Each DMA holds 256
 *	descriptors of three words (status and length, address low and
 *	high), seventeen rings that take slices of them, and its own control
 *	registers after the rings. Only the default ring, number 16, is used
 *	each way, over the first ETH_RX_RING and ETH_TX_RING descriptors.
 *
 *	A ring is a pair of free-running 16 bit counters: the producer's and
 *	the consumer's. On receive the controller produces (fills a buffer
 *	and counts it) and this side consumes (copies the frame out and
 *	counts it back); on transmit the other way round. The buffers stay
 *	where they were put, so a descriptor is given back by the count
 *	alone. The receive buffers start the frame two bytes in, so that
 *	what follows the 14 byte Ethernet header is word aligned; the length
 *	the controller writes includes those two bytes. The frame check
 *	sequence is not handed up.
 *
 *	The controller reaches all of memory at its physical address (the
 *	device tree's dma-ranges of /scb), but nothing here is cache
 *	coherent with it: buffers and descriptors are cleaned before it
 *	reads them and invalidated before they are read. Descriptors live in
 *	the controller's own register space and need neither. Like the
 *	Raspberry Pi 5's driver, this one takes no interrupt: the receive
 *	ring is looked at by the reader, often while frames come and seldom
 *	when they do not. The controller takes every frame (promiscuous);
 *	the protocol stack above keeps what is its own.
 *
 *	NOT VERIFIED ON HARDWARE: QEMU's raspi4b has no GENET. What to check
 *	on a board is listed in docs/tessronos/03-platform.md 3.6.4.
 */

#include <sys/machine.h>

#ifdef RPI4

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include <ts/board.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"

/* ---------------------------------------------------------------- registers */
#define G_REG(off)		(GENET_BASE + (off))

/* System */
#define SYS_REV_CTRL		G_REG(0x0000)
#define SYS_PORT_CTRL		G_REG(0x0004)
#define PORT_MODE_EXT_GPHY	3		/* an external gigabit PHY */
#define SYS_RBUF_FLUSH_CTRL	G_REG(0x0008)
#define RBUF_FLUSH_RESET	0x00000002

/* RGMII */
#define EXT_RGMII_OOB_CTRL	G_REG(0x008c)
#define RGMII_LINK		0x00000010	/* the MAC is told the link is up */
#define RGMII_OOB_DISABLE	0x00000020
#define RGMII_MODE_EN		0x00000040
#define RGMII_ID_MODE_DISABLE	0x00010000	/* the controller does not delay the transmit clock */

/* Interrupt controller 0 (receive, transmit, MDIO, link) */
#define INTRL2_0_CLEAR		G_REG(0x0208)
#define INTRL2_0_MASK_SET	G_REG(0x0210)
#define INTRL2_1_CLEAR		G_REG(0x0248)
#define INTRL2_1_MASK_SET	G_REG(0x0250)

/* Receive buffer */
#define RBUF_CTRL		G_REG(0x0300)
#define RBUF_64B_EN		0x00000001	/* a status block before the frame: not used */
#define RBUF_ALIGN_2B		0x00000002	/* the frame starts two bytes in */
#define RBUF_TBUF_SIZE_CTRL	G_REG(0x03b4)

/* UniMAC */
#define UMAC_CMD		G_REG(0x0808)
#define CMD_TX_EN		0x00000001
#define CMD_RX_EN		0x00000002
#define CMD_SPEED_MASK		0x0000000c
#define CMD_SPEED_10		0x00000000
#define CMD_SPEED_100		0x00000004
#define CMD_SPEED_1000		0x00000008
#define CMD_PROMISC		0x00000010
#define CMD_HD_EN		0x00000400	/* half duplex */
#define CMD_SW_RESET		0x00002000
#define CMD_LCL_LOOP_EN		0x00008000
#define UMAC_MAC0		G_REG(0x080c)	/* address bytes 0 to 3, the first highest */
#define UMAC_MAC1		G_REG(0x0810)	/* bytes 4 and 5 */
#define UMAC_MAX_FRAME_LEN	G_REG(0x0814)
#define UMAC_MIB_CTRL		G_REG(0x0d80)
#define MIB_RESET_ALL		0x00000007	/* the receive, runt and transmit counters */
#define UMAC_MDIO_CMD		G_REG(0x0e14)
#define MDIO_START_BUSY		0x20000000
#define MDIO_READ_FAIL		0x10000000
#define MDIO_OP_READ		0x08000000
#define MDIO_OP_WRITE		0x04000000
#define MDIO_PHY_SHIFT		21
#define MDIO_REG_SHIFT		16

/* DMA: descriptors, the rings that take slices of them, and the control */
#define RDMA_BASE		0x2000
#define TDMA_BASE		0x4000
#define DESC_WORDS		3
#define DESC_STATUS(b, i)	G_REG((b) + (i) * 12 + 0x00)
#define DESC_ADDR_LO(b, i)	G_REG((b) + (i) * 12 + 0x04)
#define DESC_ADDR_HI(b, i)	G_REG((b) + (i) * 12 + 0x08)
#define DEF_RING		16
#define RING_REG(b, off)	G_REG((b) + 0xc00 + DEF_RING * 0x40 + (off))
#define DMA_REG(b, off)		G_REG((b) + 0x1040 + (off))

#define RDMA_WRITE_PTR_LO	RING_REG(RDMA_BASE, 0x00)
#define RDMA_WRITE_PTR_HI	RING_REG(RDMA_BASE, 0x04)
#define RDMA_PROD_INDEX		RING_REG(RDMA_BASE, 0x08)	/* the controller's count */
#define RDMA_CONS_INDEX		RING_REG(RDMA_BASE, 0x0c)	/* this side's */
#define RDMA_RING_BUF_SIZE	RING_REG(RDMA_BASE, 0x10)
#define RDMA_START_ADDR_LO	RING_REG(RDMA_BASE, 0x14)
#define RDMA_START_ADDR_HI	RING_REG(RDMA_BASE, 0x18)
#define RDMA_END_ADDR_LO	RING_REG(RDMA_BASE, 0x1c)
#define RDMA_END_ADDR_HI	RING_REG(RDMA_BASE, 0x20)
#define RDMA_XON_XOFF_THRESH	RING_REG(RDMA_BASE, 0x28)
#define RDMA_READ_PTR_LO	RING_REG(RDMA_BASE, 0x2c)
#define RDMA_READ_PTR_HI	RING_REG(RDMA_BASE, 0x30)

#define TDMA_READ_PTR_LO	RING_REG(TDMA_BASE, 0x00)
#define TDMA_READ_PTR_HI	RING_REG(TDMA_BASE, 0x04)
#define TDMA_CONS_INDEX		RING_REG(TDMA_BASE, 0x08)	/* the controller's count */
#define TDMA_PROD_INDEX		RING_REG(TDMA_BASE, 0x0c)	/* this side's */
#define TDMA_RING_BUF_SIZE	RING_REG(TDMA_BASE, 0x10)
#define TDMA_START_ADDR_LO	RING_REG(TDMA_BASE, 0x14)
#define TDMA_START_ADDR_HI	RING_REG(TDMA_BASE, 0x18)
#define TDMA_END_ADDR_LO	RING_REG(TDMA_BASE, 0x1c)
#define TDMA_END_ADDR_HI	RING_REG(TDMA_BASE, 0x20)
#define TDMA_MBUF_DONE_THRESH	RING_REG(TDMA_BASE, 0x24)
#define TDMA_FLOW_PERIOD	RING_REG(TDMA_BASE, 0x28)
#define TDMA_WRITE_PTR_LO	RING_REG(TDMA_BASE, 0x2c)
#define TDMA_WRITE_PTR_HI	RING_REG(TDMA_BASE, 0x30)

#define RING_SIZE_SHIFT		16		/* descriptors in the ring, above the buffer length */
#define INDEX_MASK		0x0000ffff

#define DMA_RING_CFG(b)		DMA_REG(b, 0x00)
#define DMA_CTRL(b)		DMA_REG(b, 0x04)
#define DMA_STATUS(b)		DMA_REG(b, 0x08)
#define DMA_SCB_BURST(b)	DMA_REG(b, 0x0c)
#define DMA_EN			0x00000001
#define DMA_RING_EN(q)		( 1U << ( (q) + 1 ) )
#define DMA_DISABLED		0x00000001

/* Descriptor status: the length in the high half, flags in the low */
#define DS_LEN_SHIFT		16
#define DS_LEN_MASK		0x0fff
#define DS_OWN			0x8000
#define DS_EOP			0x4000
#define DS_SOP			0x2000
#define DS_TX_APPEND_CRC	0x0040
#define DS_TX_QTAG		( 0x3f << 7 )
#define DS_RX_ERRORS		0x001f		/* overrun, CRC, receive, no buffer, too long */

/* The registers every PHY has */
#define MII_BMCR		0
#define MII_BMSR		1
#define MII_ID1			2
#define MII_ID2			3
#define MII_ANAR		4
#define MII_ANLPAR		5
#define MII_GBCR		9
#define MII_GBSR		10

#define BMCR_RESET		0x8000
#define BMCR_ANENABLE		0x1000
#define BMCR_ANRESTART		0x0200
#define BMSR_LSTATUS		0x0004
#define BMSR_ANEGCOMP		0x0020
#define ANAR_ALL		0x01e1		/* 100 and 10, full and half, 802.3 */
#define ANLPAR_100FULL		0x0100
#define ANLPAR_100HALF		0x0080
#define ANLPAR_10FULL		0x0040
#define GBCR_1000FULL		0x0200
#define GBSR_1000FULL		0x0800
#define GBSR_1000HALF		0x0400

/*
 * Broadcom's own registers for the RGMII clock delays. The auxiliary
 * control register (0x18) shows one of its shadows, picked by its low
 * bits, and its miscellaneous control shadow (7) holds the receive clock
 * delay; the shadow register at 0x1c, picked by bits 14:10, holds in its
 * clock alignment shadow (3) the transmit clock delay.
 */
#define BRCM_AUXCTL		0x18
#define AUXCTL_MISC		0x0007
#define AUXCTL_MISC_READ	( 0x0007 << 12 )
#define AUXCTL_MISC_WREN	0x8000
#define AUXCTL_MISC_RXC_SKEW	0x0100
#define BRCM_SHADOW		0x1c
#define SHADOW_WRITE		0x8000
#define SHADOW_SEL(n)		( (n) << 10 )
#define SHADOW_DATA		0x03ff
#define SHADOW_CLK_ALIGN	0x03
#define CLK_ALIGN_GTXCLK_DELAY	0x0200

/* ---------------------------------------------------------------- the driver */
#define ETH_RX_RING		128		/* descriptors each way */
#define ETH_TX_RING		128
#define ETH_TX_BUFS		8		/* sent one at a time and waited for */
#define ETH_BUF_LEN		2048		/* a whole frame and the two bytes before it */
#define ETH_RX_ORDER		6		/* 128 buffers of 2KB */
#define ETH_TX_ORDER		2		/* 8 of them */
#define ETH_MAX_FRAME		1536
#define ETH_RX_PAD		2		/* the frame starts this far into its buffer */

#define ETH_BUSY_POLLS		200		/* looks a millisecond apart while frames come */
#define ETH_IDLE_POLL_MS	20		/* and this far apart when they do not */

typedef struct {
	UB	mac[NET_MAC_LEN];
	UINT	phy;
	BOOL	up;
	UB	*rxbuf, *txbuf;
	UD	rxbuf_pa, txbuf_pa;
	UW	rx_cons;			/* frames taken out, a 16 bit count */
	UW	tx_prod;			/* frames handed in */
	ID	mtxid, mtxid_rx, mtxid_mdio;
	UD	n_sent, n_recv, n_drop;
	INT	idle, link_polls;
	BOOL	told_tx;
} GENET_DEV;

LOCAL GENET_DEV	gen;
LOCAL BOOL	gen_found = FALSE;

LOCAL void dcache_flush( CONST void *p, UD len )
{
	DCacheFlushRange(p, len);
}

/* ---------------------------------------------------------------- PHY */

/* One management bus command, waited for */
LOCAL INT mdio_cmd( UW cmd )
{
	INT	i;
	UW	v;

	out_w(UMAC_MDIO_CMD, cmd | MDIO_START_BUSY);
	for ( i = 0; i < 1000; i++ ) {
		v = in_w(UMAC_MDIO_CMD);
		if ( ( v & MDIO_START_BUSY ) == 0 ) {
			return (INT)v;
		}
		if ( ( i & 15 ) == 15 && tk_dly_tsk(1) != E_OK ) {
			CpuRelax();
		}
	}
	return E_TMOUT;
}

LOCAL INT phy_read( UINT phy, UINT reg )
{
	INT	v;

	if ( gen.mtxid_mdio > 0 ) tk_loc_mtx(gen.mtxid_mdio, TMO_FEVR);
	v = mdio_cmd(MDIO_OP_READ | ( phy << MDIO_PHY_SHIFT ) | ( reg << MDIO_REG_SHIFT ));
	if ( gen.mtxid_mdio > 0 ) tk_unl_mtx(gen.mtxid_mdio);
	if ( v < 0 ) return v;
	if ( ( (UW)v & MDIO_READ_FAIL ) != 0 ) return E_IO;
	return v & 0xffff;
}

LOCAL ER phy_write( UINT phy, UINT reg, UINT val )
{
	INT	v;

	if ( gen.mtxid_mdio > 0 ) tk_loc_mtx(gen.mtxid_mdio, TMO_FEVR);
	v = mdio_cmd(MDIO_OP_WRITE | ( phy << MDIO_PHY_SHIFT ) | ( reg << MDIO_REG_SHIFT ) | ( val & 0xffff ));
	if ( gen.mtxid_mdio > 0 ) tk_unl_mtx(gen.mtxid_mdio);
	return ( v < 0 ) ? v : E_OK;
}

/*
 * Where the PHY answers: the board's address first (its device tree),
 * then every other one. A PHY answers its identifier with something
 * other than all ones or all zeroes.
 */
LOCAL ER phy_find( void )
{
	INT	id1, id2;
	UINT	k, a;

	for ( k = 0; k < 32; k++ ) {
		a = ( k == 0 ) ? knl_board.eth_phy : ( ( k <= knl_board.eth_phy ) ? k - 1 : k );
		id1 = phy_read(a, MII_ID1);
		id2 = phy_read(a, MII_ID2);
		if ( id1 == E_TMOUT ) {
			tm_printf((UB *)"genet: management bus timed out at address %d\n", (INT)a);
			return E_IO;
		}
		if ( id1 >= 0 && id2 >= 0 && id1 != 0xffff && ( id1 != 0 || id2 != 0 ) ) {
			gen.phy = a;
			tm_printf((UB *)"genet: PHY %04x:%04x at address %d\n", id1, id2, (INT)a);
			return E_OK;
		}
	}
	return E_NOEXS;
}

/*
 * The clock delays "rgmii-rxid" asks of the PHY: the receive clock
 * delayed against the data, the transmit clock not (the controller
 * delays that one).
 */
LOCAL void phy_delays( void )
{
	INT	v;

	if ( phy_write(gen.phy, BRCM_AUXCTL, AUXCTL_MISC_READ | AUXCTL_MISC) >= E_OK
	  && ( v = phy_read(gen.phy, BRCM_AUXCTL) ) >= 0 ) {
		v = ( v | AUXCTL_MISC_RXC_SKEW | AUXCTL_MISC_WREN | AUXCTL_MISC ) & 0xffff;
		(void)phy_write(gen.phy, BRCM_AUXCTL, (UINT)v);
	}
	if ( phy_write(gen.phy, BRCM_SHADOW, SHADOW_SEL(SHADOW_CLK_ALIGN)) >= E_OK
	  && ( v = phy_read(gen.phy, BRCM_SHADOW) ) >= 0 ) {
		v = ( v & SHADOW_DATA ) & ~CLK_ALIGN_GTXCLK_DELAY;
		(void)phy_write(gen.phy, BRCM_SHADOW, SHADOW_WRITE | SHADOW_SEL(SHADOW_CLK_ALIGN) | (UINT)v);
	}
}

/*
 * The controller told what the two ends agreed on: gigabit, 100 or 10,
 * full or half. Done whenever the link is found up, not only at start.
 */
LOCAL ER phy_speed( void )
{
	INT	gbsr, lpa;
	UW	cmd;
	CONST char *spd;
	BOOL	fd = TRUE;

	gbsr = phy_read(gen.phy, MII_GBSR);
	lpa  = phy_read(gen.phy, MII_ANLPAR);
	if ( gbsr < 0 || lpa < 0 ) {
		return E_IO;
	}
	cmd = in_w(UMAC_CMD) & ~( CMD_SPEED_MASK | CMD_HD_EN );
	if ( ( gbsr & ( GBSR_1000FULL | GBSR_1000HALF ) ) != 0 ) {
		cmd |= CMD_SPEED_1000;
		fd = ( ( gbsr & GBSR_1000FULL ) != 0 );
		spd = "1000";
	} else if ( ( lpa & ( ANLPAR_100FULL | ANLPAR_100HALF ) ) != 0 ) {
		cmd |= CMD_SPEED_100;
		fd = ( ( lpa & ANLPAR_100FULL ) != 0 );
		spd = "100";
	} else {
		cmd |= CMD_SPEED_10;
		fd = ( ( lpa & ANLPAR_10FULL ) != 0 );
		spd = "10";
	}
	if ( !fd ) {
		cmd |= CMD_HD_EN;
	}
	out_w(UMAC_CMD, cmd);
	out_w(EXT_RGMII_OOB_CTRL, in_w(EXT_RGMII_OOB_CTRL) | RGMII_LINK);
	if ( !gen.up ) knl_net_link_said();
	gen.up = TRUE;
	tm_printf((UB *)"genet: link up %s %s\n", spd, fd ? "full" : "half");
	return E_OK;
}

/* Offer everything and wait a few seconds for the other end to agree */
LOCAL ER phy_up( void )
{
	INT	bmsr, spin;

	if ( phy_write(gen.phy, MII_ANAR, ANAR_ALL) < E_OK
	  || phy_write(gen.phy, MII_GBCR, GBCR_1000FULL) < E_OK
	  || phy_write(gen.phy, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART) < E_OK ) {
		return E_IO;
	}
	for ( spin = 0; spin < 500; spin++ ) {
		(void)phy_read(gen.phy, MII_BMSR);	/* the link bit latches low */
		bmsr = phy_read(gen.phy, MII_BMSR);
		if ( bmsr < 0 ) {
			return E_IO;
		}
		if ( ( bmsr & ( BMSR_LSTATUS | BMSR_ANEGCOMP ) ) == ( BMSR_LSTATUS | BMSR_ANEGCOMP ) ) {
			return phy_speed();
		}
		tk_dly_tsk(10);
	}
	gen.up = FALSE;
	return E_TMOUT;				/* nothing plugged in, or slower than that */
}

/* The link looked at again: the controller set up for it when it came up */
LOCAL void link_check( void )
{
	INT	bmsr;

	(void)phy_read(gen.phy, MII_BMSR);
	bmsr = phy_read(gen.phy, MII_BMSR);
	if ( bmsr < 0 ) {
		return;
	}
	if ( ( bmsr & BMSR_LSTATUS ) != 0 ) {
		if ( !gen.up && ( bmsr & BMSR_ANEGCOMP ) != 0 ) {
			(void)phy_speed();
		}
	} else if ( gen.up ) {
		gen.up = FALSE;
		out_w(EXT_RGMII_OOB_CTRL, in_w(EXT_RGMII_OOB_CTRL) & ~RGMII_LINK);
		tm_printf((UB *)"genet: link down\n");
		knl_net_link_said();
	}
}

/* ---------------------------------------------------------------- rings */

LOCAL void dma_stop( UW base )
{
	INT	i;

	out_w(DMA_CTRL(base), in_w(DMA_CTRL(base)) & ~( DMA_EN | DMA_RING_EN(DEF_RING) ));
	for ( i = 0; i < 100 && ( in_w(DMA_STATUS(base)) & DMA_DISABLED ) == 0; i++ ) {
		tk_dly_tsk(1);
	}
}

/*
 * The receive ring: every descriptor given its buffer, the counts at 0,
 * and the ring spanning the first ETH_RX_RING descriptors (its ends are
 * counted in words). The transmit ring the same, its buffers given as
 * frames are sent.
 */
LOCAL void rings_init( void )
{
	INT	i;
	UD	pa;

	dma_stop(RDMA_BASE);
	dma_stop(TDMA_BASE);

	for ( i = 0; i < ETH_RX_RING; i++ ) {
		pa = gen.rxbuf_pa + (UD)i * ETH_BUF_LEN;
		out_w(DESC_ADDR_LO(RDMA_BASE, i), (UW)pa);
		out_w(DESC_ADDR_HI(RDMA_BASE, i), (UW)( pa >> 32 ));
		out_w(DESC_STATUS(RDMA_BASE, i), 0);
	}
	out_w(RDMA_WRITE_PTR_LO, 0);
	out_w(RDMA_WRITE_PTR_HI, 0);
	out_w(RDMA_PROD_INDEX, 0);
	out_w(RDMA_CONS_INDEX, 0);
	out_w(RDMA_RING_BUF_SIZE, ( ETH_RX_RING << RING_SIZE_SHIFT ) | ETH_BUF_LEN);
	out_w(RDMA_START_ADDR_LO, 0);
	out_w(RDMA_START_ADDR_HI, 0);
	out_w(RDMA_END_ADDR_LO, ETH_RX_RING * DESC_WORDS - 1);
	out_w(RDMA_END_ADDR_HI, 0);
	out_w(RDMA_XON_XOFF_THRESH, ( 5 << 16 ) | ( ETH_RX_RING >> 4 ));
	out_w(RDMA_READ_PTR_LO, 0);
	out_w(RDMA_READ_PTR_HI, 0);
	gen.rx_cons = 0;

	for ( i = 0; i < ETH_TX_RING; i++ ) {
		out_w(DESC_STATUS(TDMA_BASE, i), 0);
		out_w(DESC_ADDR_LO(TDMA_BASE, i), 0);
		out_w(DESC_ADDR_HI(TDMA_BASE, i), 0);
	}
	out_w(TDMA_READ_PTR_LO, 0);
	out_w(TDMA_READ_PTR_HI, 0);
	out_w(TDMA_CONS_INDEX, 0);
	out_w(TDMA_PROD_INDEX, 0);
	out_w(TDMA_RING_BUF_SIZE, ( ETH_TX_RING << RING_SIZE_SHIFT ) | ETH_BUF_LEN);
	out_w(TDMA_START_ADDR_LO, 0);
	out_w(TDMA_START_ADDR_HI, 0);
	out_w(TDMA_END_ADDR_LO, ETH_TX_RING * DESC_WORDS - 1);
	out_w(TDMA_END_ADDR_HI, 0);
	out_w(TDMA_MBUF_DONE_THRESH, 1);
	out_w(TDMA_FLOW_PERIOD, 0);
	out_w(TDMA_WRITE_PTR_LO, 0);
	out_w(TDMA_WRITE_PTR_HI, 0);
	gen.tx_prod = 0;

	out_w(DMA_SCB_BURST(RDMA_BASE), 0x08);
	out_w(DMA_SCB_BURST(TDMA_BASE), 0x08);
	out_w(DMA_RING_CFG(RDMA_BASE), in_w(DMA_RING_CFG(RDMA_BASE)) | ( 1U << DEF_RING ));
	out_w(DMA_RING_CFG(TDMA_BASE), in_w(DMA_RING_CFG(TDMA_BASE)) | ( 1U << DEF_RING ));
	out_w(DMA_CTRL(RDMA_BASE), in_w(DMA_CTRL(RDMA_BASE)) | DMA_EN | DMA_RING_EN(DEF_RING));
	out_w(DMA_CTRL(TDMA_BASE), in_w(DMA_CTRL(TDMA_BASE)) | DMA_EN | DMA_RING_EN(DEF_RING));
}

/* ---------------------------------------------------------------- interface */

EXPORT ER net_get_mac( UB *mac )
{
	INT	i;

	if ( !gen_found || mac == NULL ) {
		return E_NOEXS;
	}
	for ( i = 0; i < NET_MAC_LEN; i++ ) {
		mac[i] = gen.mac[i];
	}
	return E_OK;
}

EXPORT ER net_send( CONST void *frame, SZ len )
{
	UB	*buf;
	UD	pa;
	UINT	slot, desc;
	INT	spin;
	ER	er = E_TMOUT;

	if ( !gen_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL || len < NET_FRAME_MIN || len > NET_FRAME_MAX ) {
		return E_PAR;
	}
	tk_loc_mtx(gen.mtxid, TMO_FEVR);

	slot = gen.tx_prod % ETH_TX_BUFS;
	desc = gen.tx_prod % ETH_TX_RING;
	buf = gen.txbuf + (UD)slot * ETH_BUF_LEN;
	pa  = gen.txbuf_pa + (UD)slot * ETH_BUF_LEN;
	knl_memcpy(buf, frame, (INT)len);
	dcache_flush(buf, (UD)len);

	out_w(DESC_ADDR_LO(TDMA_BASE, desc), (UW)pa);
	out_w(DESC_ADDR_HI(TDMA_BASE, desc), (UW)( pa >> 32 ));
	out_w(DESC_STATUS(TDMA_BASE, desc), ( (UW)len << DS_LEN_SHIFT ) | DS_SOP | DS_EOP
					    | DS_TX_APPEND_CRC | DS_TX_QTAG);
	DeviceBarrier();
	gen.tx_prod = ( gen.tx_prod + 1 ) & INDEX_MASK;
	out_w(TDMA_PROD_INDEX, gen.tx_prod);

	/* the controller's count catches up when the frame has gone */
	for ( spin = 0; spin < 1000; spin++ ) {
		if ( ( in_w(TDMA_CONS_INDEX) & INDEX_MASK ) == gen.tx_prod ) {
			er = E_OK;
			break;
		}
		if ( spin >= 20 ) tk_dly_tsk(1);
	}
	if ( er >= E_OK ) {
		gen.n_sent++;
		gen.idle = 0;			/* an answer may be on its way */
	} else if ( !gen.told_tx ) {
		gen.told_tx = TRUE;
		tm_printf((UB *)"genet: a frame did not go (produced %d, consumed %d, cmd %08x)\n",
			  (INT)gen.tx_prod, (INT)( in_w(TDMA_CONS_INDEX) & INDEX_MASK ), in_w(UMAC_CMD));
	}
	tk_unl_mtx(gen.mtxid);
	return er;
}

EXPORT INT net_recv( void *frame, SZ size, TMO tmout )
{
	INT	got = E_TMOUT;
	TMO	left = tmout;

	if ( !gen_found ) {
		return E_NOEXS;
	}
	if ( frame == NULL ) {
		return E_PAR;
	}
	/* the protocol stack keeps a reader on the card; a second is told so */
	if ( tk_loc_mtx(gen.mtxid_rx, tmout) < E_OK ) {
		return E_BUSY;
	}
	for (;;) {
		UW	prod = in_w(RDMA_PROD_INDEX) & INDEX_MASK;

		if ( prod != gen.rx_cons ) {
			UINT	desc = gen.rx_cons % ETH_RX_RING;
			UW	st = in_w(DESC_STATUS(RDMA_BASE, desc));
			INT	len = (INT)( ( st >> DS_LEN_SHIFT ) & DS_LEN_MASK ) - ETH_RX_PAD;
			UB	*buf = gen.rxbuf + (UD)desc * ETH_BUF_LEN;

			if ( ( st & DS_RX_ERRORS ) == 0 && ( st & ( DS_SOP | DS_EOP ) ) == ( DS_SOP | DS_EOP )
			  && len > 0 && len <= (INT)size ) {
				dcache_flush(buf, (UD)( len + ETH_RX_PAD ));
				knl_memcpy(frame, buf + ETH_RX_PAD, len);
				gen.n_recv++;
				gen.idle = 0;
				got = len;
			} else {
				gen.n_drop++;
			}
			/* the buffer goes back by the count */
			gen.rx_cons = ( gen.rx_cons + 1 ) & INDEX_MASK;
			out_w(RDMA_CONS_INDEX, gen.rx_cons);
			if ( got > 0 ) {
				break;
			}
			continue;
		}
		if ( tmout == TMO_POL ) {
			break;
		}
		{
			TMO	wait = ( gen.idle < ETH_BUSY_POLLS ) ? 1 : ETH_IDLE_POLL_MS;

			if ( gen.idle < ETH_BUSY_POLLS ) gen.idle++;
			if ( ++gen.link_polls >= 1000 / ETH_IDLE_POLL_MS ) {
				gen.link_polls = 0;
				link_check();
			}
			if ( tmout > 0 && wait > left ) wait = left;
			tk_dly_tsk(wait);
			if ( tmout > 0 && ( left -= wait ) <= 0 ) {
				break;
			}
		}
	}
	tk_unl_mtx(gen.mtxid_rx);
	return got;
}

EXPORT ER net_stat( T_NETSTAT *st )
{
	if ( !gen_found || st == NULL ) {
		return E_NOEXS;
	}
	link_check();
	st->sent = gen.n_sent;
	st->recv = gen.n_recv;
	st->drop = gen.n_drop;
	st->up   = gen.up;
	return E_OK;
}

/* ---------------------------------------------------------------- start-up */

/* Whether an address is one a card may have: not zero, not a group address */
LOCAL BOOL mac_ok( CONST UB *m )
{
	return (BOOL)( ( m[0] | m[1] | m[2] | m[3] | m[4] | m[5] ) != 0 && ( m[0] & 0x01 ) == 0 );
}

/*
 * The board's address: the firmware writes it into the device tree's
 * node (local-mac-address); without that, it is asked for through the
 * property interface (GET_BOARD_MAC_ADDRESS); without either the card
 * gets a locally administered one.
 */
#define TAG_BOARD_MAC		0x00010003U

LOCAL void mac_get( void )
{
	LOCAL UW	b[8] __attribute__((aligned(64)));
	CONST UB	*v;
	INT		len, i;

	v = knl_dtb_prop(GENET_DT_PATH, "local-mac-address", &len);
	if ( v != NULL && len >= NET_MAC_LEN && mac_ok(v) ) {
		for ( i = 0; i < NET_MAC_LEN; i++ ) gen.mac[i] = v[i];
		return;
	}
	b[0] = 8 * 4;
	b[1] = 0;
	b[2] = TAG_BOARD_MAC;
	b[3] = 8;
	b[4] = 0;
	b[5] = 0;
	b[6] = 0;
	b[7] = 0;
	if ( knl_mbox_property(b) == E_OK && ( b[4] & 0x80000000U ) != 0 && mac_ok((UB *)&b[5]) ) {
		for ( i = 0; i < NET_MAC_LEN; i++ ) gen.mac[i] = ((UB *)&b[5])[i];
		return;
	}
	gen.mac[0] = 0x02;			/* locally administered */
	gen.mac[1] = 0x00;
	gen.mac[2] = 0x00;
	gen.mac[3] = 0x00;
	gen.mac[4] = 0x00;
	gen.mac[5] = 0x01;
}

/*
 * The UniMAC reset and its counters cleared, frames up to 1536 bytes,
 * the receive buffer starting each frame two bytes in, without a status
 * block before it.
 */
LOCAL void umac_reset( void )
{
	UW	v;

	out_w(SYS_RBUF_FLUSH_CTRL, in_w(SYS_RBUF_FLUSH_CTRL) | RBUF_FLUSH_RESET);
	tk_dly_tsk(1);
	out_w(SYS_RBUF_FLUSH_CTRL, in_w(SYS_RBUF_FLUSH_CTRL) & ~RBUF_FLUSH_RESET);
	tk_dly_tsk(1);
	out_w(SYS_RBUF_FLUSH_CTRL, 0);
	tk_dly_tsk(1);

	out_w(UMAC_CMD, 0);
	out_w(UMAC_CMD, CMD_LCL_LOOP_EN | CMD_SW_RESET);
	tk_dly_tsk(1);
	out_w(UMAC_CMD, 0);

	out_w(UMAC_MIB_CTRL, MIB_RESET_ALL);
	out_w(UMAC_MIB_CTRL, 0);
	out_w(UMAC_MAX_FRAME_LEN, ETH_MAX_FRAME);

	v = in_w(RBUF_CTRL);
	v = ( v & ~RBUF_64B_EN ) | RBUF_ALIGN_2B;
	out_w(RBUF_CTRL, v);
	out_w(RBUF_TBUF_SIZE_CTRL, 1);
}

EXPORT INT knl_genet_init( void )
{
	T_CMTX	cmtx;
	PFRAME	*pf;
	UW	rev, v;

	if ( !knl_dtb_okay(GENET_DT_PATH) ) {
		tm_printf((UB *)"genet: not in the device tree, or disabled there\n");
		return 0;
	}
	rev = in_w(SYS_REV_CTRL);
	if ( rev == 0 || rev == 0xFFFFFFFFU ) {
		tm_printf((UB *)"genet: does not answer at %08x (%08x)\n", (UW)GENET_PA, rev);
		return 0;
	}
	tm_printf((UB *)"genet: revision %08x (major %d)\n", rev, (INT)( ( rev >> 24 ) & 0x0f ));

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	gen.mtxid_mdio = tk_cre_mtx(&cmtx);
	gen.mtxid = tk_cre_mtx(&cmtx);
	gen.mtxid_rx = tk_cre_mtx(&cmtx);
	if ( gen.mtxid_mdio <= 0 || gen.mtxid <= 0 || gen.mtxid_rx <= 0 ) {
		return E_SYS;
	}

	/* everything quiet: no interrupt wanted, the ring polled */
	out_w(INTRL2_0_MASK_SET, 0xFFFFFFFFU);
	out_w(INTRL2_0_CLEAR, 0xFFFFFFFFU);
	out_w(INTRL2_1_MASK_SET, 0xFFFFFFFFU);
	out_w(INTRL2_1_CLEAR, 0xFFFFFFFFU);

	umac_reset();

	/* an external PHY by RGMII, the transmit clock delayed in the controller */
	out_w(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
	v = in_w(EXT_RGMII_OOB_CTRL);
	v &= ~( RGMII_OOB_DISABLE | RGMII_ID_MODE_DISABLE | RGMII_LINK );
	v |= RGMII_MODE_EN;
	out_w(EXT_RGMII_OOB_CTRL, v);

	mac_get();
	out_w(UMAC_MAC0, ( (UW)gen.mac[0] << 24 ) | ( (UW)gen.mac[1] << 16 )
			| ( (UW)gen.mac[2] << 8 ) | gen.mac[3]);
	out_w(UMAC_MAC1, ( (UW)gen.mac[4] << 8 ) | gen.mac[5]);

	/* the buffers: 128 of 2KB to receive into, 8 to send from */
	pf = knl_alloc_pages(ETH_RX_ORDER, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	gen.rxbuf_pa = knl_pf_to_pa(pf);
	gen.rxbuf = (UB *)PA2VA(gen.rxbuf_pa);
	pf = knl_alloc_pages(ETH_TX_ORDER, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	gen.txbuf_pa = knl_pf_to_pa(pf);
	gen.txbuf = (UB *)PA2VA(gen.txbuf_pa);
	dcache_flush(gen.rxbuf, (UD)ETH_RX_RING * ETH_BUF_LEN);

	rings_init();

	if ( phy_find() < E_OK ) {
		tm_printf((UB *)"genet: no PHY answers on the management bus\n");
		return 0;
	}
	phy_delays();
	(void)phy_up();				/* an empty socket is not a failure */

	out_w(UMAC_CMD, in_w(UMAC_CMD) | CMD_PROMISC | CMD_TX_EN | CMD_RX_EN);

	gen_found = TRUE;
	tm_printf((UB *)"genet: %02x:%02x:%02x:%02x:%02x:%02x phy %d %s\n",
		  gen.mac[0], gen.mac[1], gen.mac[2], gen.mac[3], gen.mac[4], gen.mac[5],
		  gen.phy, gen.up ? "up" : "down");
	return 1;
}

#endif /* RPI4 */
