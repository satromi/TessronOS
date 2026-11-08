/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igc.c (x64 PC)
 *	The Intel I225 and I226 2.5 gigabit Ethernet. Their ring and register
 *	layout are not those of the e1000e family (e1k.c), which must not take
 *	them: knl_igc_is says which device numbers are here.
 *
 *	One queue. Its interrupt is message-signalled with MSI-X, one vector
 *	for the queue and the link; without MSI-X the card is polled, as the
 *	chip's other interrupt modes are not used here. The internal PHY,
 *	shared with the firmware through a semaphore, is asked to negotiate
 *	every speed, without flow control. Its address comes from the NVM at
 *	the reset; a chip with a blank NVM has none, and is refused.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "nicring.h"
#include "lanpci.h"

#define CTRL		0x00000
#define STATUS		0x00008
#define EECD		0x00010
#define CTRL_EXT	0x00018
#define MDIC		0x00020
#define ITR		0x000c4
#define RCTL		0x00100
#define TCTL		0x00400
#define ICR		0x01500
#define IMS		0x01508
#define IMC		0x0150c
#define GPIE		0x01514
#define EIMS		0x01524
#define EIMC		0x01528
#define EIAC		0x0152c
#define EIAM		0x01530
#define EITR0		0x01680
#define IVAR0		0x01700
#define IVAR_MISC	0x01740
#define RDBAL0		0x02800
#define RDBAH0		0x02804
#define RDLEN0		0x02808
#define SRRCTL0		0x0280c
#define RDH0		0x02810
#define RDT0		0x02818
#define RXDCTL0		0x02828
#define TDBAL0		0x03800
#define TDBAH0		0x03804
#define TDLEN0		0x03808
#define TDH0		0x03810
#define TDT0		0x03818
#define TXDCTL0		0x03828
#define RXCSUM		0x05000
#define MTA		0x05200
#define RAL0		0x05400
#define RAH0		0x05404
#define WUC		0x05800
#define SWSM		0x05b50
#define SW_FW_SYNC	0x05b5c
#define UTA		0x0a000
#define I225_PHPM	0x00e14

#define CTRL_GIO_MASTER_DISABLE	0x00000004
#define CTRL_SLU	0x00000040
#define CTRL_FRCSPD	0x00000800
#define CTRL_FRCDPX	0x00001000
#define CTRL_DEV_RST	0x20000000
#define STATUS_FD	0x00000001
#define STATUS_LU	0x00000002
#define STATUS_100	0x00000040
#define STATUS_1000	0x00000080
#define STATUS_2500	0x00400000
#define STATUS_GIO_MASTER 0x00080000
#define EECD_AUTO_RD	0x00000200
#define CTRL_EXT_DRV_LOAD 0x10000000
#define IVAR_VALID	0x80
#define GPIE_NSICR	0x00000001
#define GPIE_MSIX_MODE	0x00000010
#define GPIE_EIAME	0x40000000
#define GPIE_PBA	0x80000000
#define EITR_CNT_IGNR	0x80000000
#define ICR_LSC		0x00000004
#define RCTL_EN		0x00000002
#define RCTL_MPE	0x00000010
#define RCTL_BAM	0x00008000
#define RCTL_SECRC	0x04000000
#define TCTL_EN		0x00000002
#define TCTL_PSP	0x00000008
#define TCTL_CT		0x00000ff0
#define TCTL_RTLC	0x01000000
#define CT_THRESHOLD	( 15 << 4 )
#define SRRCTL_BSIZE	2		/* in KB: the buffers' 2048 bytes */
#define SRRCTL_ONEBUF	0x02000000
#define RXDCTL_ENABLE	0x02000000
#define RXDCTL_THRESH	( 8 | ( 8 << 8 ) | ( 4 << 16 ) )
#define TXDCTL_CONFIG	( 0x1f | ( 1 << 8 ) | ( 1 << 16 ) | ( 1 << 22 ) | 0x01000000 | ( 1 << 25 ) )
#define SWSM_SMBI	0x00000001
#define SWSM_SWESMBI	0x00000002
#define SWFW_PHY0	0x02
#define SWFW_PHY1	0x04
#define PHPM_GO_LINKD	0x00000020
#define MDIC_OP_WRITE	0x04000000
#define MDIC_OP_READ	0x08000000
#define MDIC_READY	0x10000000
#define MDIC_ERROR	0x40000000
#define PHY_ADDR	1
#define MMDAC		13
#define MMDAAD		14
#define MMDAC_DATA	0x4000
#define AN_MMD		7		/* the auto-negotiation's own registers */
#define AN_MULTIGBT	0x0020
#define ADV_2500FD	0x0080
#define BMCR_ANEG	0x1200		/* auto-negotiation on, and again */
#define ANAR_SPEEDS	0x01e0		/* 10 and 100, full and half */
#define ANAR_PAUSE	0x0c00
#define GTCR_1000FD	0x0200
#define GTCR_1000HD	0x0100
#define ITR_8000	( 1000000000 / ( 8000 * 256 ) )
#define EITR_8000	( ( 4000000 / 8000 ) & 0x7ffc )

LOCAL T_LAN	igc;
LOCAL UW	swfw_phy;		/* the semaphore of this function's PHY */

#define RD(o)		in_w(igc.base + (o))
#define WR(o, v)	out_w(igc.base + (o), (UW)(v))

/* The I225 and I226 by their device numbers */
LOCAL CONST struct {
	UH		id;
	CONST char	*name;
} ids[] = {
	{ 0x15f2, "I225-LM" }, { 0x15f3, "I225-V" }, { 0x15f8, "I225-I" }, { 0x15fd, "I225 (blank NVM)" },
	{ 0x0d9f, "I225-IT" }, { 0x3100, "I225-K" }, { 0x3101, "I225-K2" }, { 0x5502, "I225-LMvP" },
	{ 0x125b, "I226-LM" }, { 0x125c, "I226-V" }, { 0x125d, "I226-IT" }, { 0x125f, "I226 (blank NVM)" },
	{ 0x3102, "I226-K" }, { 0x5503, "I226-LMvP" },
};

/* A device of this driver, which e1k.c must leave alone */
EXPORT BOOL knl_igc_is( UH device )
{
	INT	i;

	for ( i = 0; i < (INT)( sizeof(ids) / sizeof(ids[0]) ); i++ ) {
		if ( ids[i].id == device ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the semaphores and the PHY */

LOCAL void sem_put( void )
{
	WR(SWSM, RD(SWSM) & ~( SWSM_SMBI | SWSM_SWESMBI ));
}

LOCAL BOOL sem_get( void )
{
	INT	i;

	for ( i = 0; i < 2000 && ( RD(SWSM) & SWSM_SMBI ); i++ ) WaitUsec(50);
	if ( i >= 2000 ) {
		/* it can be left held across a reset: cleared once, then given up */
		sem_put();
		for ( i = 0; i < 2000 && ( RD(SWSM) & SWSM_SMBI ); i++ ) WaitUsec(50);
		if ( i >= 2000 ) return FALSE;
	}
	for ( i = 0; i < 2000; i++ ) {
		WR(SWSM, RD(SWSM) | SWSM_SWESMBI);
		if ( RD(SWSM) & SWSM_SWESMBI ) return TRUE;
		WaitUsec(50);
	}
	sem_put();
	return FALSE;
}

LOCAL BOOL phy_take( void )
{
	UW	v;
	INT	i;

	for ( i = 0; i < 200; i++ ) {
		if ( !sem_get() ) return FALSE;
		v = RD(SW_FW_SYNC);
		if ( ( v & ( swfw_phy | ( swfw_phy << 16 ) ) ) == 0 ) {
			WR(SW_FW_SYNC, v | swfw_phy);
			sem_put();
			return TRUE;
		}
		sem_put();
		tk_dly_tsk(5);
	}
	return FALSE;
}

LOCAL void phy_give( void )
{
	if ( sem_get() ) {
		WR(SW_FW_SYNC, RD(SW_FW_SYNC) & ~swfw_phy);
		sem_put();
	}
}

/* One MDIO access, the semaphore held: the value read, or ~0 when it fails */
LOCAL UW mdic( UW op, UW reg, UW val )
{
	UW	v = 0;
	INT	i;

	WR(MDIC, op | ( PHY_ADDR << 21 ) | ( ( reg & 0x1f ) << 16 ) | ( val & 0xffff ));
	for ( i = 0; i < 1920; i++ ) {
		WaitUsec(50);
		v = RD(MDIC);
		if ( v & MDIC_READY ) break;
	}
	if ( ( v & MDIC_READY ) == 0 || ( v & MDIC_ERROR ) ) return ~0U;
	return v & 0xffff;
}

/* The speeds and auto-negotiation; an access that fails leaves the chip's own defaults */
LOCAL void phy_start( void )
{
	UW	v;

	WR(CTRL, ( RD(CTRL) | CTRL_SLU ) & ~( CTRL_FRCSPD | CTRL_FRCDPX ));
	WR(I225_PHPM, RD(I225_PHPM) & ~PHPM_GO_LINKD);
	if ( !phy_take() ) {
		tm_printf((UB *)"net: igc: the PHY's semaphore not had\n");
		return;
	}
	v = mdic(MDIC_OP_READ, 4, 0);
	if ( v != ~0U ) (void)mdic(MDIC_OP_WRITE, 4, ( v & ~ANAR_PAUSE ) | ANAR_SPEEDS);
	v = mdic(MDIC_OP_READ, 9, 0);
	if ( v != ~0U ) (void)mdic(MDIC_OP_WRITE, 9, ( v & ~GTCR_1000HD ) | GTCR_1000FD);

	/* the multi-gigabit advertisement lies in the MMD registers: an address, then its data */
	(void)mdic(MDIC_OP_WRITE, MMDAC, AN_MMD);
	(void)mdic(MDIC_OP_WRITE, MMDAAD, AN_MULTIGBT);
	(void)mdic(MDIC_OP_WRITE, MMDAC, MMDAC_DATA | AN_MMD);
	v = mdic(MDIC_OP_READ, MMDAAD, 0);
	(void)mdic(MDIC_OP_WRITE, MMDAC, 0);
	if ( v != ~0U ) {
		(void)mdic(MDIC_OP_WRITE, MMDAC, AN_MMD);
		(void)mdic(MDIC_OP_WRITE, MMDAAD, AN_MULTIGBT);
		(void)mdic(MDIC_OP_WRITE, MMDAC, MMDAC_DATA | AN_MMD);
		(void)mdic(MDIC_OP_WRITE, MMDAAD, v | ADV_2500FD);
		(void)mdic(MDIC_OP_WRITE, MMDAC, 0);
	}
	v = mdic(MDIC_OP_READ, 0, 0);
	if ( v != ~0U ) (void)mdic(MDIC_OP_WRITE, 0, v | BMCR_ANEG);
	phy_give();
}

/* ---------------------------------------------------------------- the chip */

LOCAL void chip_reset( void )
{
	INT	i;

	WR(CTRL, RD(CTRL) | CTRL_GIO_MASTER_DISABLE);
	for ( i = 0; i < 800 && ( RD(STATUS) & STATUS_GIO_MASTER ); i++ ) WaitUsec(100);
	WR(IMC, ~0U);
	WR(RCTL, 0);
	WR(TCTL, TCTL_PSP);
	(void)RD(STATUS);
	tk_dly_tsk(10);
	WR(CTRL, RD(CTRL) | CTRL_DEV_RST);
	for ( i = 0; i < 10 && ( RD(EECD) & EECD_AUTO_RD ) == 0; i++ ) tk_dly_tsk(1);
	tk_dly_tsk(1);
	WR(IMC, ~0U);
	(void)RD(ICR);
	WR(WUC, 0);
}

LOCAL void igc_hw_start( T_LAN *l )
{
	UW	r;
	INT	i;

	WR(EIMC, ~0U);
	WR(EIAC, 0);
	chip_reset();
	WR(CTRL_EXT, RD(CTRL_EXT) | CTRL_EXT_DRV_LOAD);	/* the firmware told that the driver has it */

	WR(RAL0, l->mac[0] | ( (UW)l->mac[1] << 8 ) | ( (UW)l->mac[2] << 16 ) | ( (UW)l->mac[3] << 24 ));
	WR(RAH0, l->mac[4] | ( (UW)l->mac[5] << 8 ) | 0x80000000U);
	for ( i = 1; i < 16; i++ ) {
		WR(RAL0 + 8 * i, 0);
		WR(RAH0 + 8 * i, 0);
	}
	for ( i = 0; i < 128; i++ ) {
		WR(MTA + 4 * i, 0);
		WR(UTA + 4 * i, 0);
	}
	phy_start();

	/* transmitting */
	knl_memset(l->tx, 0, 4096);
	l->tx_next = 0;
	WR(TDLEN0, LAN_NTX * sizeof(IGC_TXD));
	WR(TDBAH0, (UW)( l->tx_pa >> 32 ));
	WR(TDBAL0, (UW)l->tx_pa);
	WR(TDT0, 0);
	WR(TDH0, 0);
	WR(TXDCTL0, TXDCTL_CONFIG);
	WR(TCTL, ( RD(TCTL) & ~TCTL_CT ) | TCTL_PSP | TCTL_RTLC | TCTL_EN | CT_THRESHOLD);

	/* receiving: every multicast, as IPv6 asks for it */
	knl_memset(l->rx, 0, 4096);
	l->rx_next = 0;
	for ( i = 0; i < LAN_NRX; i++ ) {
		igc_rx_give((IGC_RXD *)l->rx + i, l->rxbuf_pa + (UD)i * LAN_BUF);
	}
	r = RD(RCTL);
	WR(RCTL, r & ~RCTL_EN);
	WR(ITR, ITR_8000);
	WR(RXCSUM, 0);
	WR(RXDCTL0, 0);
	WR(RDLEN0, LAN_NRX * sizeof(IGC_RXD));
	WR(RDBAH0, (UW)( l->rx_pa >> 32 ));
	WR(RDBAL0, (UW)l->rx_pa);
	WR(SRRCTL0, SRRCTL_BSIZE | SRRCTL_ONEBUF);
	WR(RDH0, 0);
	WR(RDT0, 0);
	WR(RXDCTL0, ( RD(RXDCTL0) & 0xfff00000U ) | RXDCTL_ENABLE | RXDCTL_THRESH);
	WR(RCTL, RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SECRC);
	DeviceBarrier();
	WR(RDT0, LAN_NRX - 1);

	if ( l->intno != 0 ) {
		WR(GPIE, GPIE_MSIX_MODE | GPIE_EIAME | GPIE_PBA | GPIE_NSICR);
		WR(EITR0, EITR_8000 | EITR_CNT_IGNR);
		WR(IVAR0, IVAR_VALID | ( IVAR_VALID << 8 ));	/* the queue's both ways: vector 0 */
		WR(IVAR_MISC, IVAR_VALID << 8);			/* the link's: vector 0 */
	}
}

LOCAL void igc_hw_stop( T_LAN *l )
{
	(void)l;
	chip_reset();
}

LOCAL void igc_irq_on( T_LAN *l )
{
	(void)l;
	(void)RD(ICR);
	WR(EIAC, 1);
	WR(EIAM, 1);
	WR(EIMS, 1);
	WR(IMS, ICR_LSC);
	(void)RD(STATUS);
}

LOCAL void igc_irq_off( T_LAN *l )
{
	(void)l;
	WR(EIMC, ~0U);
	WR(EIAC, 0);
	WR(IMC, ~0U);
	(void)RD(STATUS);
}

/* The vector masks itself when it fires: unmasked again here */
LOCAL BOOL igc_irq_ack( T_LAN *l )
{
	UW	icr = RD(ICR);

	(void)l;
	WR(IMS, ICR_LSC);
	WR(EIMS, 1);
	return ( icr & ICR_LSC ) != 0;
}

LOCAL BOOL igc_link( T_LAN *l, INT *p_mbps, BOOL *p_fdx )
{
	UW	s = RD(STATUS);

	(void)l;
	if ( ( s & STATUS_LU ) == 0 ) return FALSE;
	*p_mbps = ( s & STATUS_1000 ) ? ( ( s & STATUS_2500 ) ? 2500 : 1000 ) : ( s & STATUS_100 ) ? 100 : 10;
	*p_fdx = ( s & STATUS_FD ) != 0;
	return TRUE;
}

LOCAL INT igc_rx_take( T_LAN *l, UINT i )
{
	return igc_rx_len((CONST IGC_RXD *)l->rx + i);
}

LOCAL void igc_rx_giveback( T_LAN *l, UINT i )
{
	igc_rx_give((IGC_RXD *)l->rx + i, l->rxbuf_pa + (UD)i * LAN_BUF);
	DeviceBarrier();
	WR(RDT0, i);
}

LOCAL BOOL igc_tx_isfree( T_LAN *l, UINT i )
{
	return igc_tx_free((CONST IGC_TXD *)l->tx + i);
}

LOCAL void igc_tx_go( T_LAN *l, UINT i, UINT len )
{
	igc_tx_put((IGC_TXD *)l->tx + i, l->txbuf_pa + (UD)i * LAN_BUF, len);
	DeviceBarrier();
	WR(TDT0, ( i + 1 ) % LAN_NTX);
}

LOCAL CONST T_LANOPS igc_ops = {
	igc_hw_start, igc_hw_stop, igc_irq_on, igc_irq_off, igc_irq_ack, igc_link,
	igc_rx_take, igc_rx_giveback, igc_tx_isfree, igc_tx_go
};

LOCAL void igc_inthdr( UINT n )
{
	knl_lan_inthdr(&igc, n);
}

EXPORT INT knl_igc_init( void )
{
	INT	i, k;
	UW	ral, rah;

	for ( k = 0; ; k++ ) {
		if ( ts_pcie_find(0x02, 0x00, 0xff, k, &igc.pci) != E_OK ) return 0;
		if ( igc.pci.vendor == 0x8086 && knl_igc_is(igc.pci.device) ) break;
	}
	for ( i = 0; ids[i].id != igc.pci.device; i++ ) {
	}
	ts_pcie_enable(&igc.pci);
	igc.base = ts_pcie_bar_base(&igc.pci, 0);
	if ( igc.base == 0 ) {
		knl_lan_refuse(ids[i].name, &igc.pci, 0, "has no memory window");
		return 0;
	}
	igc.chip = ids[i].name;
	igc.rev = ts_pcie_cfg_read(igc.pci.bus, igc.pci.dev, igc.pci.fn, 0x08, 1);
	igc.card = "igc";
	igc.ops = &igc_ops;
	swfw_phy = ( igc.pci.fn == 1 ) ? SWFW_PHY1 : SWFW_PHY0;

	chip_reset();
	ral = RD(RAL0);
	rah = RD(RAH0);
	for ( k = 0; k < 4; k++ ) igc.mac[k] = (UB)( ral >> ( 8 * k ) );
	igc.mac[4] = (UB)rah;
	igc.mac[5] = (UB)( rah >> 8 );
	if ( ( rah & 0x80000000U ) == 0 || ( ral == 0 && ( rah & 0xffff ) == 0 ) || ( igc.mac[0] & 1 ) ) {
		knl_lan_refuse(igc.chip, &igc.pci, igc.rev, "has no address in its NVM");
		return 0;
	}

	igc.irq_kind = PCI_IRQ_POLL;
	igc.intno = 0;
	if ( ts_pcie_msi_alloc(&igc.pci, &igc.intno) >= E_OK && igc.intno != 0 ) {
		igc.irq_kind = PCI_IRQ_MSIX;
		knl_define_inthdr((INT)igc.intno, TA_HLNG, (FP)igc_inthdr);
		EnableInt(igc.intno, INTPRI_DEVICE);
		knl_int_owner(igc.intno, (CONST UB *)"neta");
	} else {
		igc.intno = 0;
	}
	return knl_lan_start(&igc, NETSEL_IGC);
}

#endif /* X64 */
