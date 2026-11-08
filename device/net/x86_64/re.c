/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	re.c (x64 PC)
 *	The Realtek RTL8111/RTL8168 gigabit Ethernet of a PC's board or of a
 *	PCIe card, device 10ec:8168 or 10ec:8161.
 *
 *	The revision read from the transmit configuration register decides
 *	how it is stopped and what it needs (the table below). A revision
 *	not in the table is refused with a line on the console and the card
 *	is left alone. The first ones (8168B) have their address in a serial
 *	EEPROM behind the configuration register, and no large-frame mode.
 *	The PHY is reached through the MAC and negotiates by itself; its link
 *	is read from the MAC's status register.
 *
 *	The card has no link interrupt among those asked for: the adapters'
 *	manager looks at the link every second (the told entry says so).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "nicring.h"
#include "lanpci.h"

#define R_IDR0		0x0000
#define R_IDR4		0x0004
#define R_MAR0		0x0008
#define R_MAR4		0x000c
#define R_TXLIST_LO	0x0020
#define R_TXLIST_HI	0x0024
#define R_COMMAND	0x0037
#define R_GTXSTART	0x0038
#define R_IMR		0x003c
#define R_ISR		0x003e
#define R_TXCFG		0x0040
#define R_RXCFG		0x0044
#define R_MISSEDPKT	0x004c
#define R_EECMD		0x0050
#define R_CFG1		0x0052
#define R_CFG2		0x0053
#define R_CFG3		0x0054
#define R_CFG4		0x0055
#define R_TIMERINT	0x0058
#define R_PHYAR		0x0060
#define R_GMEDIASTAT	0x006c
#define R_PMCH		0x006f
#define R_MAXRXPKTLEN	0x00da
#define R_CPLUS_CMD	0x00e0
#define R_RXLIST_LO	0x00e4
#define R_RXLIST_HI	0x00e8
#define R_EARLY_TX	0x00ec
#define R_MISC		0x00f0

#define TXCFG_HWREV	0x7c800000U
#define TXCFG_QUEUE_EMPTY 0x00000800U
#define TXCFG_CONFIG	0x03000700U	/* the interframe gap, DMA bursts of 2048 bytes */
#define RXCFG_CONFIG	0x0000ff00U	/* no FIFO threshold, unlimited DMA, the 64KB setting */
#define RXCFG_EARLYOFF	0x00003800U
#define RXCFG_EARLYOFFV2 0x00000800U
#define RXCFG_INDIV	0x00000002U
#define RXCFG_MULTI	0x00000004U
#define RXCFG_BROAD	0x00000008U
#define RXCFG_ALLPHYS	0x00000001U
#define CMD_TX_ENB	0x04
#define CMD_RX_ENB	0x08
#define CMD_RESET	0x10
#define CMD_STOPREQ	0x80
#define EEMODE_OFF	0x00
#define EEMODE_WRITECFG	0xc0
#define CFG1_DRVLOAD	0x20
#define CFG2_MSI	0x20
#define CFG3_JUMBO_EN0	0x04
#define CFG4_JUMBO_EN1	0x02
#define CFG4_8168E_JUMBO_EN1 0x01
#define CPLUS_TXENB	0x0001
#define CPLUS_PCI_MRW	0x0008
#define CPLUS_RXCSUM	0x0020
#define CPLUS_MACSTAT_DIS 0x0080
#define TXSTART_START	0x40
#define PHYAR_BUSY	0x80000000U
#define PHYAR_DATA	0x0000ffffU
#define PHY_TIMEOUT	20
#define RESET_TIMEOUT	1000
#define ISR_RX_OK	0x0001
#define ISR_RX_ERR	0x0002
#define ISR_TX_OK	0x0004
#define ISR_TX_ERR	0x0008
#define ISR_RX_OVERRUN	0x0010
#define ISR_FIFO_OFLOW	0x0040
#define ISR_SYSTEM_ERR	0x8000
#define INTRS		( ISR_RX_OK | ISR_RX_ERR | ISR_TX_ERR | ISR_RX_OVERRUN | ISR_FIFO_OFLOW \
			  | ISR_SYSTEM_ERR | ISR_TX_OK )
#define GMEDIA_FDX	0x01
#define GMEDIA_LINK	0x02
#define GMEDIA_100	0x08
#define GMEDIA_1000	0x10

/* The revisions, as the transmit configuration register has them */
#define HWREV_8168B_1	0x30000000U
#define HWREV_8168B_2	0x38000000U
#define HWREV_8168B_3	0x38400000U
#define HWREV_8168C	0x3c000000U
#define HWREV_8168C_2	0x3c400000U
#define HWREV_8168CP	0x3c800000U
#define HWREV_8168D	0x28000000U
#define HWREV_8168DP	0x28800000U
#define HWREV_8168E	0x2c000000U
#define HWREV_8168E_VL	0x2c800000U
#define HWREV_8168F	0x48000000U
#define HWREV_8411	0x48800000U
#define HWREV_8168G	0x4c000000U
#define HWREV_8168EP	0x50000000U
#define HWREV_8168GU	0x50800000U
#define HWREV_8168H	0x54000000U
#define HWREV_8168FP	0x54800000U
#define HWREV_8411B	0x5c800000U

/* What a revision needs */
#define F_PHYWAKE_PM	0x0001		/* the PHY taken out of its power down through the MAC */
#define F_CMDSTOP	0x0002		/* stopped by a request */
#define F_WAIT_TXQ	0x0004		/* and its transmit queue waited for */
#define F_WAIT_TXPOLL	0x0008		/* stopped when its transmit poll is over */
#define F_EARLYOFF	0x0010
#define F_EARLYOFFV2	0x0020
#define F_RXDV_GATED	0x0040
#define F_EEPROM	0x0080		/* its address in the EEPROM, and no large frames */

#define F_G_AND_LATER	( F_CMDSTOP | F_WAIT_TXQ | F_EARLYOFFV2 | F_RXDV_GATED )

LOCAL CONST struct {
	UW		hwrev;
	CONST char	*name;
	UH		flags;
} revs[] = {
	{ HWREV_8168B_1,	"RTL8168B/8111B",	F_EEPROM },
	{ HWREV_8168B_2,	"RTL8168B/8111B",	F_EEPROM },
	{ HWREV_8168B_3,	"RTL8168B/8111B",	F_EEPROM },
	{ HWREV_8168C,		"RTL8168C/8111C",	F_CMDSTOP },
	{ HWREV_8168C_2,	"RTL8168C/8111C",	F_CMDSTOP },
	{ HWREV_8168CP,		"RTL8168CP/8111CP",	F_CMDSTOP },
	{ HWREV_8168D,		"RTL8168D/8111D",	F_PHYWAKE_PM | F_CMDSTOP },
	{ HWREV_8168DP,		"RTL8168DP/8111DP",	F_WAIT_TXPOLL },
	{ HWREV_8168E,		"RTL8168E/8111E",	F_PHYWAKE_PM | F_CMDSTOP },
	{ HWREV_8168E_VL,	"RTL8168E/8111E-VL",	F_EARLYOFF | F_CMDSTOP | F_WAIT_TXQ },
	{ HWREV_8168F,		"RTL8168F/8111F",	F_EARLYOFF | F_CMDSTOP | F_WAIT_TXQ },
	{ HWREV_8411,		"RTL8411",		F_CMDSTOP | F_WAIT_TXQ },
	{ HWREV_8168G,		"RTL8168G/8111G",	F_G_AND_LATER },
	{ HWREV_8168EP,		"RTL8168EP/8111EP",	F_G_AND_LATER },
	{ HWREV_8168GU,		"RTL8168GU/8111GU",	F_G_AND_LATER },
	{ HWREV_8168H,		"RTL8168H/8111H",	F_G_AND_LATER },
	{ HWREV_8168FP,		"RTL8168FP/8111FP",	F_G_AND_LATER },
	{ HWREV_8411B,		"RTL8411B",		F_G_AND_LATER },
};

LOCAL T_LAN	re;
LOCAL UH	flags;

#define RD1(o)		in_b(re.base + (o))
#define RD2(o)		in_h(re.base + (o))
#define RD4(o)		in_w(re.base + (o))
#define WR1(o, v)	out_b(re.base + (o), (UB)(v))
#define WR2(o, v)	out_h(re.base + (o), (UH)(v))
#define WR4(o, v)	out_w(re.base + (o), (UW)(v))

/* ---------------------------------------------------------------- the EEPROM */

/*
 * A 93C46 or 93C56 on four bits of the configuration register, the same
 * in programming mode: a word is read by selecting it, clocking out the
 * read opcode and the address, and clocking in 16 bits, first the highest.
 */
#define EE_DATAOUT	0x01
#define EE_DATAIN	0x02
#define EE_CLK		0x04
#define EE_SEL		0x08
#define EEMODE_PROGRAM	0x80
#define EE_READ		0x6
#define EE_ALEN_93C46	6
#define EE_ALEN_93C56	8
#define EE_ID_93C56	0x8129		/* word 0 read with the longer address */
#define EE_EADDR	7		/* the word the station address starts at */

LOCAL void ee_bit( UB bit, BOOL on )
{
	WR1(R_EECMD, on ? ( RD1(R_EECMD) | bit ) : ( RD1(R_EECMD) & ~bit ));
}

LOCAL UH ee_word( INT alen, INT addr )
{
	UW	cmd = (UW)addr | ( (UW)EE_READ << alen );
	UW	i;
	UH	w = 0;

	ee_bit(EE_SEL, TRUE);
	for ( i = 1U << ( alen + 3 ); i != 0; i >>= 1 ) {
		ee_bit(EE_DATAIN, ( cmd & i ) != 0);
		WaitUsec(100);
		ee_bit(EE_CLK, TRUE);
		WaitUsec(150);
		ee_bit(EE_CLK, FALSE);
		WaitUsec(100);
	}
	for ( i = 0x8000; i != 0; i >>= 1 ) {
		ee_bit(EE_CLK, TRUE);
		WaitUsec(100);
		if ( RD1(R_EECMD) & EE_DATAOUT ) w |= (UH)i;
		ee_bit(EE_CLK, FALSE);
		WaitUsec(100);
	}
	ee_bit(EE_SEL, FALSE);
	return w;
}

/* The station address from the EEPROM: the size of the part found from word 0 */
LOCAL void ee_macaddr( UB *mac )
{
	INT	alen, k;

	WR1(R_EECMD, RD1(R_EECMD) | EEMODE_PROGRAM);
	WaitUsec(100);
	alen = ( ee_word(EE_ALEN_93C56, 0) == EE_ID_93C56 ) ? EE_ALEN_93C56 : EE_ALEN_93C46;
	for ( k = 0; k < 3; k++ ) {
		UH	w = ee_word(alen, EE_EADDR + k);

		mac[k * 2] = (UB)w;
		mac[k * 2 + 1] = (UB)( w >> 8 );
	}
	WR1(R_EECMD, RD1(R_EECMD) & ~EEMODE_PROGRAM);
}

/* ---------------------------------------------------------------- the PHY */

LOCAL UW phy_rd( UW reg )
{
	UW	v = 0;
	INT	i;

	WR4(R_PHYAR, reg << 16);
	for ( i = 0; i < PHY_TIMEOUT; i++ ) {
		v = RD4(R_PHYAR);
		if ( v & PHYAR_BUSY ) break;		/* the bit goes up when the value is there */
		WaitUsec(25);
	}
	WaitUsec(20);
	return v & PHYAR_DATA;
}

LOCAL void phy_wr( UW reg, UW val )
{
	INT	i;

	WR4(R_PHYAR, ( reg << 16 ) | ( val & PHYAR_DATA ) | PHYAR_BUSY);
	for ( i = 0; i < PHY_TIMEOUT; i++ ) {
		if ( ( RD4(R_PHYAR) & PHYAR_BUSY ) == 0 ) break;
		WaitUsec(25);
	}
	WaitUsec(20);
}

/* The PHY reset, and asked to negotiate every speed it has */
LOCAL void phy_start( void )
{
	INT	i;

	if ( flags & F_PHYWAKE_PM ) WR1(R_PMCH, RD1(R_PMCH) | 0x80);
	phy_wr(0x1f, 0);
	phy_wr(0x0e, 0);
	phy_wr(0x00, 0x8000);			/* BMCR: reset */
	for ( i = 0; i < 100 && ( phy_rd(0x00) & 0x8000 ); i++ ) tk_dly_tsk(1);
	phy_wr(0x04, 0x0de1);			/* ANAR: 10 and 100, both ways of pausing */
	WaitUsec(1000);
	phy_wr(0x09, 0x0300);			/* 1000 full and half */
	WaitUsec(1000);
	phy_wr(0x00, 0x1200);			/* BMCR: auto-negotiation on, and again */
	WaitUsec(100);
}

/* ---------------------------------------------------------------- the chip */

LOCAL void chip_stop( void )
{
	INT	i;

	WR4(R_RXCFG, RD4(R_RXCFG) & ~( RXCFG_ALLPHYS | RXCFG_BROAD | RXCFG_INDIV | RXCFG_MULTI ));
	if ( flags & F_WAIT_TXPOLL ) {
		for ( i = 0; i < RESET_TIMEOUT && ( RD1(R_GTXSTART) & TXSTART_START ); i++ ) WaitUsec(20);
		WR1(R_COMMAND, 0);
	} else if ( flags & F_CMDSTOP ) {
		WR1(R_COMMAND, CMD_STOPREQ | CMD_TX_ENB | CMD_RX_ENB);
		if ( flags & F_WAIT_TXQ ) {
			for ( i = 0; i < RESET_TIMEOUT && ( RD4(R_TXCFG) & TXCFG_QUEUE_EMPTY ) == 0; i++ ) WaitUsec(100);
		}
	} else {
		WR1(R_COMMAND, 0);
	}
	WaitUsec(1000);
	WR2(R_IMR, 0);
	WR2(R_ISR, 0xffff);
}

LOCAL void chip_reset( void )
{
	INT	i;

	WR1(R_COMMAND, CMD_RESET);
	for ( i = 0; i < RESET_TIMEOUT; i++ ) {
		WaitUsec(10);
		if ( ( RD1(R_COMMAND) & CMD_RESET ) == 0 ) break;
	}
	if ( i >= RESET_TIMEOUT ) tm_printf((UB *)"net: re: the reset did not end\n");
}

LOCAL void re_hw_start( T_LAN *l )
{
	UW	rxcfg;
	UB	c4;

	chip_stop();
	chip_reset();

	WR2(R_CPLUS_CMD, CPLUS_TXENB | CPLUS_PCI_MRW | CPLUS_RXCSUM | CPLUS_MACSTAT_DIS);

	/* its address in place, as the reset read it, and the large-frame mode these revisions run in */
	WR1(R_EECMD, EEMODE_WRITECFG);
	WR4(R_IDR4, l->mac[4] | ( (UW)l->mac[5] << 8 ));
	WR4(R_IDR0, l->mac[0] | ( (UW)l->mac[1] << 8 ) | ( (UW)l->mac[2] << 16 ) | ( (UW)l->mac[3] << 24 ));
	if ( !( flags & F_EEPROM ) ) WR1(R_CFG3, RD1(R_CFG3) | CFG3_JUMBO_EN0);
	if ( !( flags & F_EEPROM ) && l->rev != HWREV_8168DP ) {
		c4 = ( l->rev == HWREV_8168E ) ? CFG4_8168E_JUMBO_EN1 : CFG4_JUMBO_EN1;
		WR1(R_CFG4, RD1(R_CFG4) | c4);
	}
	WR1(R_EECMD, EEMODE_OFF);

	knl_memset(l->rx, 0, 4096);
	knl_memset(l->tx, 0, 4096);
	re_ring_init((RE_DESC *)l->rx, LAN_NRX, l->rxbuf_pa, LAN_BUF, (RE_DESC *)l->tx, LAN_NTX);
	l->rx_next = l->tx_next = 0;
	WR4(R_RXLIST_HI, (UW)( l->rx_pa >> 32 ));
	WR4(R_RXLIST_LO, (UW)l->rx_pa);
	WR4(R_TXLIST_HI, (UW)( l->tx_pa >> 32 ));
	WR4(R_TXLIST_LO, (UW)l->tx_pa);

	if ( flags & F_RXDV_GATED ) WR4(R_MISC, RD4(R_MISC) & ~0x00080000U);

	WR4(R_TXCFG, TXCFG_CONFIG);
	WR1(R_EARLY_TX, 16);
	rxcfg = RXCFG_CONFIG;
	if ( flags & F_EARLYOFF ) rxcfg |= RXCFG_EARLYOFF;
	else if ( flags & F_EARLYOFFV2 ) rxcfg |= RXCFG_EARLYOFFV2;
	WR4(R_RXCFG, rxcfg);
	WR1(R_COMMAND, CMD_TX_ENB | CMD_RX_ENB);

	/* its own address, broadcasts, and every multicast: IPv6 asks for it */
	WR4(R_MAR0, 0xffffffffU);
	WR4(R_MAR4, 0xffffffffU);
	WR4(R_RXCFG, RD4(R_RXCFG) | RXCFG_INDIV | RXCFG_BROAD | RXCFG_MULTI);

	WR4(R_MISSEDPKT, 0);
	WR4(R_TIMERINT, 0);
	/* without the large-frame mode, nothing longer than a receive buffer may be let in */
	WR2(R_MAXRXPKTLEN, ( flags & F_EEPROM ) ? LAN_BUF : 16383);
	WR1(R_CFG1, RD1(R_CFG1) | CFG1_DRVLOAD);
	phy_start();
}

LOCAL void re_hw_stop( T_LAN *l )
{
	(void)l;
	chip_stop();
}

LOCAL void re_irq_on( T_LAN *l )
{
	(void)l;
	WR2(R_ISR, 0xffff);
	WR2(R_IMR, INTRS);
}

LOCAL void re_irq_off( T_LAN *l )
{
	(void)l;
	WR2(R_IMR, 0);
}

LOCAL BOOL re_irq_ack( T_LAN *l )
{
	UH	st = RD2(R_ISR);

	(void)l;
	if ( st != 0xffff && st != 0 ) WR2(R_ISR, st);
	return FALSE;
}

LOCAL BOOL re_link( T_LAN *l, INT *p_mbps, BOOL *p_fdx )
{
	UB	s = RD1(R_GMEDIASTAT);

	(void)l;
	if ( ( s & GMEDIA_LINK ) == 0 ) return FALSE;
	*p_mbps = ( s & GMEDIA_1000 ) ? 1000 : ( s & GMEDIA_100 ) ? 100 : 10;
	*p_fdx = ( s & GMEDIA_FDX ) != 0;
	return TRUE;
}

LOCAL INT re_rx_take( T_LAN *l, UINT i )
{
	return rtl_rx_len(( (RE_DESC *)l->rx )[i].cmdsts, RE_RX_FS, RE_RX_LS);
}

LOCAL void re_rx_give( T_LAN *l, UINT i )
{
	RE_DESC	*d = (RE_DESC *)l->rx + i;

	d->vlan = 0;
	DeviceBarrier();
	d->cmdsts = rtl_rx_word(i, LAN_NRX, LAN_BUF);
}

LOCAL BOOL re_tx_free( T_LAN *l, UINT i )
{
	return ( ( (RE_DESC *)l->tx )[i].cmdsts & RTL_OWN ) == 0;
}

LOCAL void re_tx_go( T_LAN *l, UINT i, UINT len )
{
	RE_DESC	*d = (RE_DESC *)l->tx + i;

	d->addr = l->txbuf_pa + (UD)i * LAN_BUF;
	d->vlan = 0;
	DeviceBarrier();
	d->cmdsts = rtl_tx_word(i, LAN_NTX, len);
	DeviceBarrier();
	WR1(R_GTXSTART, TXSTART_START);
}

LOCAL CONST T_LANOPS re_ops = {
	re_hw_start, re_hw_stop, re_irq_on, re_irq_off, re_irq_ack, re_link,
	re_rx_take, re_rx_give, re_tx_free, re_tx_go
};

LOCAL void re_inthdr( UINT n )
{
	knl_lan_inthdr(&re, n);
}

EXPORT INT knl_re_init( void )
{
	INT	k, i;
	UINT	cap;
	UW	rev;
	UH	v;

	for ( k = 0; ; k++ ) {
		if ( ts_pcie_find(0x02, 0x00, 0xff, k, &re.pci) != E_OK ) return 0;
		if ( re.pci.vendor == 0x10ec && ( re.pci.device == 0x8168 || re.pci.device == 0x8161 ) ) break;
	}
	ts_pcie_enable(&re.pci);
	re.base = ts_pcie_bar_base(&re.pci, 2);
	if ( re.base == 0 ) re.base = ts_pcie_bar_base(&re.pci, 1);
	if ( re.base == 0 ) {
		knl_lan_refuse("RTL8168", &re.pci, 0, "has no memory window");
		return 0;
	}
	rev = RD4(R_TXCFG) & TXCFG_HWREV;
	for ( i = 0; i < (INT)( sizeof(revs) / sizeof(revs[0]) ); i++ ) {
		if ( revs[i].hwrev == rev ) break;
	}
	if ( i >= (INT)( sizeof(revs) / sizeof(revs[0]) ) ) {
		knl_lan_refuse("RTL8168", &re.pci, rev, "is not a revision this driver has the settings of");
		return 0;
	}
	re.chip = revs[i].name;
	flags = revs[i].flags;
	re.rev = rev;
	re.card = "re";
	re.ops = &re_ops;

	/* the PCIe link's power saving off, as this family is run */
	cap = ts_pcie_cap_find(&re.pci, 0x10);
	if ( cap != 0 ) {
		v = (UH)ts_pcie_cfg_read(re.pci.bus, re.pci.dev, re.pci.fn, cap + 0x10, 2);
		v &= ~( 0x0003 | 0x0100 );		/* ASPM L0s and L1, the clock power management */
		ts_pcie_cfg_write(re.pci.bus, re.pci.dev, re.pci.fn, cap + 0x10, 2, v);
	}

	chip_stop();
	chip_reset();
	if ( flags & F_EEPROM ) {
		ee_macaddr(re.mac);
	} else {
		for ( k = 0; k < NET_MAC_LEN; k++ ) re.mac[k] = RD1(R_IDR0 + k);
	}

	/* the plain message interrupt, which the chip must also be told of; polled when there is none */
	re.irq_kind = PCI_IRQ_POLL;
	re.intno = 0;
	WR1(R_EECMD, EEMODE_WRITECFG);
	if ( ts_pcie_msi_plain_alloc(&re.pci, &re.intno) >= E_OK && re.intno != 0 ) {
		re.irq_kind = PCI_IRQ_MSI;
		WR1(R_CFG2, RD1(R_CFG2) | CFG2_MSI);
	} else {
		re.intno = 0;
		WR1(R_CFG2, RD1(R_CFG2) & ~CFG2_MSI);
	}
	WR1(R_EECMD, EEMODE_OFF);
	if ( re.intno != 0 ) {
		knl_define_inthdr((INT)re.intno, TA_HLNG, (FP)re_inthdr);
		EnableInt(re.intno, INTPRI_DEVICE);
		knl_int_owner(re.intno, (CONST UB *)"neta");
	}
	return knl_lan_start(&re, NETSEL_RE);
}

#endif /* X64 */
