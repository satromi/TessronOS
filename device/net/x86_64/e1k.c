/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	e1k.c (x64 PC)
 *	Intel's gigabit Ethernet of the e1000e family: the I219 of a notebook
 *	PC's chipset (the ThinkPad X1 Yoga's), the 82574 (QEMU's e1000e) and
 *	their kin, through the legacy descriptors all of them keep.
 *
 *	The card is reset, its address taken from the receive address
 *	registers (the card loads them from its NVM at the reset), the link
 *	set up by the card itself (auto-negotiation), and one ring of 64
 *	receive buffers and one of 32 transmit descriptors given to it.
 *	A received frame raises the card's message interrupt, when it has one
 *	the machine can take, else it is looked for every millisecond while
 *	someone waits. The protocol stack reaches the card through netsel.c.
 *
 *	The I219 is the chipset's own (the PCH's MAC with a PHY outside it,
 *	reached over MDIO, or over SMBus when the PHY is powered down), and
 *	the chipset's management engine (ME, AMT) may share it. Before the
 *	reset it is brought to a state the driver can work in (pch_prepare):
 *	the hardware's own PHY configuration gated; the PHY taken out of its
 *	ultra low power state (ULP), through the ME when the ME is there,
 *	else through the PHY's own registers; the PHY found answering over
 *	MDIO, after forcing the SMBus mode or cycling the PHY's power
 *	(LANPHYPC) when it does not; and the PHY reset, unless the ME forbids
 *	it. The descriptor rings are flushed when the chipset says they must
 *	be (Skylake's PCH and later), the master access stopped, and the MAC
 *	and the PHY reset together, waiting for the LAN to say it is
 *	initialized. After the reset come the chipset's own settings of the
 *	MAC (pch_bits), the errata of Skylake's PCH (SPT) for transmitting,
 *	and, at each change of the link, the settings that depend on the
 *	speed (pch_link). What the machine shows (HW.TXT, KERNEL.TXT) decides
 *	what more it needs.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include <ts/pcie.h>
#include <ts/pm.h>
#include "sysman/pfalloc.h"

#define CTRL		0x0000
#define STATUS		0x0008
#define CTRL_EXT	0x0018
#define ICR		0x00c0
#define IMS		0x00d0
#define IMC		0x00d8
#define RCTL		0x0100
#define TCTL		0x0400
#define TIPG		0x0410
#define RDBAL		0x2800
#define RDBAH		0x2804
#define RDLEN		0x2808
#define RDH		0x2810
#define RDT		0x2818
#define TDBAL		0x3800
#define TDBAH		0x3804
#define TDLEN		0x3808
#define TDH		0x3810
#define TDT		0x3818
#define MTA		0x5200
#define RAL0		0x5400
#define RAH0		0x5404

#define CTRL_SLU	0x00000040
#define CTRL_RST	0x04000000
#define CTRL_GIO_MASTER_DISABLE	0x00000004
#define CTRL_FRCSPD	0x00000800
#define CTRL_FRCDPX	0x00001000
#define CTRL_LANPHYPC_OVERRIDE	0x00010000
#define CTRL_LANPHYPC_VALUE	0x00020000
#define CTRL_MEHE	0x00080000
#define CTRL_PHY_RST	0x80000000
#define STATUS_FD	0x00000001
#define STATUS_SPEED_100	0x00000040
#define STATUS_SPEED_1000	0x00000080
#define STATUS_LAN_INIT_DONE	0x00000200
#define STATUS_PHYRA	0x00000400
#define STATUS_GIO_MASTER	0x00080000
#define CTRL_EXT_LPCD	0x00000004
#define CTRL_EXT_FORCE_SMBUS	0x00000800
#define CTRL_EXT_PHYPDEN	0x00100000
#define CTRL_EXT_DRV_LOAD	0x10000000
#define FEXTNVM6	0x0010
#define FEXTNVM4	0x0024
#define FEXTNVM3	0x003c
#define FEXTNVM7	0x00e4
#define MDIC		0x0020
#define EXTCNF_CTRL	0x0f00
#define PCIEANACFG	0x0f18
#define IOSFPC		0x0f28
#define KABGTXD		0x3004
#define TXDCTL0		0x3828
#define TARC0		0x3840
#define TXDCTL1		0x3928
#define TARC1		0x3940
#define RXDCTL0		0x2828
#define PBECCSTS	0x100c
#define RFCTL		0x5008
#define H2ME		0x5b50
#define FWSM		0x5b54
#define FEXTNVM11	0x5bbc
#define EXTCNF_SWFLAG	0x00000020
#define EXTCNF_GATE_PHY_CFG	0x00000080
#define FWSM_RSPCIPHY	0x00000040
#define FWSM_ULP_CFG_DONE	0x00000400
#define FWSM_FW_VALID	0x00008000
#define H2ME_ULP	0x00000800
#define H2ME_ENFORCE	0x00001000
#define MDIC_READ	0x08000000
#define MDIC_WRITE	0x04000000
#define MDIC_READY	0x10000000
#define MDIC_ERROR	0x40000000

/* PHY registers: the page (32 registers each) and the register */
#define PHY_PAGE_SELECT	0x1f
#define PG_PORT		769
#define PG_PM		770
#define PG_LPI		772
#define PG_ULP		779
#define CV_SMB_CTRL	PG_PORT, 23
#define CV_SMB_FORCE	0x0001
#define HV_PM_CTRL	PG_PM, 17
#define HV_PM_K1	0x4000
#define HV_PM_K1_CLK_REQ	0x0200
#define ULP_CONFIG1	PG_ULP, 16
#define ULP_START	0x0001
#define ULP_CLEAR	0x1d74		/* the indication, sticky, reset to SMBus, waking, in-band exit, LANPHYPC, PERST bits */
#define PLL_CLOCK_GATE	PG_LPI, 28
#define SPT_776_20	776, 20
#define STATUS_LU	0x00000002
#define RCTL_EN		0x00000002
#define RCTL_BAM	0x00008000
#define RCTL_SECRC	0x04000000
#define TCTL_EN		0x00000002
#define TCTL_PSP	0x00000008
#define IM_RXT0		0x00000080
#define IM_LSC		0x00000004

#define NRX		64
#define NTX		32
#define BUF		2048

typedef struct {
	UD	addr;
	UH	len;
	UH	csum;
	UB	status;
	UB	errors;
	UH	special;
} RXD;

typedef struct {
	UD	addr;
	UH	len;
	UB	cso;
	UB	cmd;
	UB	status;
	UB	css;
	UH	special;
} TXD;

LOCAL BOOL	e1k_ok = FALSE;
LOCAL BOOL	pch = FALSE;		/* a chipset's I217/I218/I219 */
LOCAL BOOL	spt = FALSE;		/* Skylake's PCH (Sunrise Point) and the 200-series */
LOCAL BOOL	me = FALSE;		/* the management engine has the firmware valid */
LOCAL UW	link_was = 0;
LOCAL UBINT	base;
LOCAL T_PCIDEV	pci;
LOCAL volatile RXD	*rx;
LOCAL volatile TXD	*tx;
LOCAL UB	*rxbuf, *txbuf;
LOCAL UD	rx_pa, tx_pa, rxbuf_pa, txbuf_pa;
LOCAL UINT	rx_next = 0, tx_next = 0;
LOCAL volatile BOOL	e1k_asleep = FALSE;	/* the machine sleeps: its registers are not to be read */
LOCAL UINT	intno = 0;
LOCAL INT	irq_kind = PCI_IRQ_POLL;	/* how it was had (ts_pcie_irq_alloc) */
LOCAL volatile UW	n_irq = 0;		/* the interrupts it raised */
LOCAL UW	irq_mark = 1;			/* the count said next on the console */
LOCAL BOOL	irq_silent_told = FALSE;
LOCAL UW	t_start;			/* when the driver started (ms) */
LOCAL ID	flg = 0, tx_mtx = 0, rx_mtx = 0;
LOCAL UB	mac[NET_MAC_LEN];
LOCAL UD	n_sent, n_recv, n_drop;

LOCAL UW  rd( UW o ) { return in_w(base + o); }
LOCAL void wr( UW o, UW v ) { out_w(base + o, v); }

LOCAL void *pages( UINT order, UD *p_pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) {
		return NULL;
	}
	*p_pa = knl_pf_to_pa(pf);
	return PA2VA(*p_pa);
}

LOCAL void e1k_inthdr( UINT n )
{
	UW	icr = rd(ICR);			/* read: cleared */

	n_irq++;
	tk_set_flg(flg, 1);
	if ( ( icr & IM_LSC ) != 0 ) knl_net_link_said();	/* the link came up or went down */
	EndOfInt(n);
}

/* ---------------------------------------------------------------- the chipset's LAN (PCH) */

/* The I219s of Skylake's and Kaby Lake's PCH, and of the PCHs after them */
LOCAL CONST UH	spt_ids[] = { 0x156f, 0x1570, 0x15b7, 0x15b8, 0x15b9, 0x15d7, 0x15d8, 0x15e3, 0x15d6 };
LOCAL CONST UH	later_ids[] = { 0x15bd, 0x15be, 0x15bb, 0x15bc, 0x15df, 0x15e0, 0x15e1, 0x15e2, 0x0d4e, 0x0d4f,
				0x0d4c, 0x0d4d, 0x0d53, 0x0d55, 0x15fb, 0x15fc, 0x15f9, 0x15fa, 0x1a1e, 0x1a1f,
				0x1a1c, 0x1a1d, 0x0dc5, 0x0dc6, 0x0dc7, 0x0dc8, 0x15f4, 0x15f5, 0x1a1e, 0x550a,
				0x550b, 0x550c, 0x550d, 0x550e, 0x550f, 0x5510, 0x5511, 0x57a0, 0x57a1, 0x57b3,
				0x57b4, 0x57b5, 0x57b6, 0x57b7, 0x57b8, 0x57b9, 0x57ba };

LOCAL BOOL in_ids( UH id, CONST UH *t, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) if ( t[i] == id ) return TRUE;
	return FALSE;
}

/* The semaphore the MAC, the ME and the PHY's own configuration share: every PHY access holds it */
LOCAL BOOL swflag_take( void )
{
	INT	i;

	for ( i = 0; i < 100 && ( rd(EXTCNF_CTRL) & EXTCNF_SWFLAG ); i++ ) tk_dly_tsk(1);
	wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) | EXTCNF_SWFLAG);
	for ( i = 0; i < 1000; i++ ) {
		if ( rd(EXTCNF_CTRL) & EXTCNF_SWFLAG ) return TRUE;
		tk_dly_tsk(1);
	}
	wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
	return FALSE;
}

LOCAL void swflag_give( void )
{
	wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
}

/* One MDIO access of a PHY address: the value read, or ~0 when it fails */
LOCAL UW mdic( UW op, UW addr, UW reg, UW val )
{
	UW	v = 0;
	INT	i;

	wr(MDIC, op | ( addr << 21 ) | ( ( reg & 0x1f ) << 16 ) | ( val & 0xffff ));
	for ( i = 0; i < 600; i++ ) {
		WaitUsec(50);
		v = rd(MDIC);
		if ( v & MDIC_READY ) break;
	}
	if ( ( v & MDIC_READY ) == 0 || ( v & MDIC_ERROR ) ) return ~0U;
	if ( op == MDIC_READ && ( ( v >> 16 ) & 0x1f ) != ( reg & 0x1f ) ) return ~0U;
	return v & 0xffff;
}

/*
 * A PHY register by its page: the pages of the PHY's own registers
 * (768 and up, 768 standing for 0) at address 1, the others at address 2;
 * a register past 15 has its page selected first. The semaphore held.
 */
LOCAL UW phy_rd( UW page, UW reg )
{
	UW	addr = ( page >= 768 ) ? 1 : 2;

	if ( page == 768 ) page = 0;
	if ( reg > 0xf && mdic(MDIC_WRITE, 1, PHY_PAGE_SELECT, page << 5) == ~0U ) return ~0U;
	return mdic(MDIC_READ, addr, reg, 0);
}

LOCAL BOOL phy_wr( UW page, UW reg, UW val )
{
	UW	addr = ( page >= 768 ) ? 1 : 2;

	if ( page == 768 ) page = 0;
	if ( reg > 0xf && mdic(MDIC_WRITE, 1, PHY_PAGE_SELECT, page << 5) == ~0U ) return FALSE;
	return mdic(MDIC_WRITE, addr, reg, val) != ~0U;
}

/* The ME lets the PHY be reset with the MAC */
LOCAL BOOL phy_reset_allowed( void )
{
	INT	i;

	for ( i = 0; i < 30; i++ ) {
		if ( rd(FWSM) & FWSM_RSPCIPHY ) return TRUE;
		tk_dly_tsk(10);
	}
	return FALSE;
}

/* The PHY answers over MDIO (its identifier reads); without an ME, the SMBus mode is left */
LOCAL BOOL phy_answers( void )
{
	UW	id = phy_rd(0, 2);
	INT	t;

	for ( t = 0; t < 2 && ( id == ~0U || id == 0xffff || id == 0 ); t++ ) id = phy_rd(0, 2);
	if ( id == ~0U || id == 0xffff || id == 0 ) return FALSE;
	if ( !me ) {
		UW	v = phy_rd(CV_SMB_CTRL);

		if ( v != ~0U ) (void)phy_wr(CV_SMB_CTRL, v & ~CV_SMB_FORCE);
		wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
	}
	return TRUE;
}

/* The PHY's power cycled by the MAC (LANPHYPC low for a moment), and its configuration waited for */
LOCAL void lanphypc_cycle( void )
{
	INT	i;

	wr(FEXTNVM3, ( rd(FEXTNVM3) & ~0x0c000000U ) | 0x08000000U);	/* the PHY's configuration counter: 50ms */
	wr(CTRL, ( rd(CTRL) | CTRL_LANPHYPC_OVERRIDE ) & ~CTRL_LANPHYPC_VALUE);
	(void)rd(STATUS);
	WaitUsec(20);
	wr(CTRL, rd(CTRL) & ~CTRL_LANPHYPC_OVERRIDE);
	(void)rd(STATUS);
	for ( i = 0; i < 20 && ( rd(CTRL_EXT) & CTRL_EXT_LPCD ) == 0; i++ ) tk_dly_tsk(5);
	tk_dly_tsk(30);
}

/* The PHY out of its ultra low power state, as the firmware may have left it */
LOCAL void ulp_off( void )
{
	INT	i;
	UW	v;

	if ( me ) {
		/* the ME does it, told to and waited for */
		wr(H2ME, ( rd(H2ME) & ~H2ME_ULP ) | H2ME_ENFORCE);
		for ( i = 0; i < 250 && ( rd(FWSM) & FWSM_ULP_CFG_DONE ); i++ ) tk_dly_tsk(10);
		wr(H2ME, rd(H2ME) & ~H2ME_ENFORCE);
		if ( i >= 250 ) tm_printf((UB *)"e1k: the ME did not leave the low power state\n");
		return;
	}
	if ( !swflag_take() ) return;
	lanphypc_cycle();
	v = phy_rd(CV_SMB_CTRL);
	if ( v == ~0U ) {
		wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
		tk_dly_tsk(50);
		v = phy_rd(CV_SMB_CTRL);
	}
	if ( v != ~0U ) {
		(void)phy_wr(CV_SMB_CTRL, v & ~CV_SMB_FORCE);
		wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
		v = phy_rd(HV_PM_CTRL);
		if ( v != ~0U ) (void)phy_wr(HV_PM_CTRL, v | HV_PM_K1);
		v = phy_rd(ULP_CONFIG1);
		if ( v != ~0U ) {
			v &= ~ULP_CLEAR;
			(void)phy_wr(ULP_CONFIG1, v);
			(void)phy_wr(ULP_CONFIG1, v | ULP_START);	/* the configuration taken */
		}
	}
	wr(FEXTNVM7, rd(FEXTNVM7) & ~0x20U);			/* SMBus not disabled at PERST */
	swflag_give();
	if ( phy_reset_allowed() ) {
		wr(CTRL, rd(CTRL) | CTRL_PHY_RST);
		WaitUsec(100);
		wr(CTRL, rd(CTRL) & ~CTRL_PHY_RST);
		tk_dly_tsk(50);
	}
}

/* Before the reset: the PHY reachable, in a state the driver works in */
LOCAL void pch_prepare( void )
{
	BOOL	ok;

	me = ( rd(FWSM) & FWSM_FW_VALID ) != 0;
	wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) | EXTCNF_GATE_PHY_CFG);
	ulp_off();
	if ( !swflag_take() ) {
		tm_printf((UB *)"e1k: the PHY's semaphore not had\n");
		return;
	}
	ok = phy_answers();
	if ( !ok ) {
		wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
		tk_dly_tsk(50);
		ok = phy_answers();
	}
	if ( !ok && phy_reset_allowed() ) {
		lanphypc_cycle();
		ok = phy_answers();
		if ( !ok ) {
			wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
			tk_dly_tsk(50);
			ok = phy_answers();
		}
	}
	swflag_give();
	tm_printf((UB *)"e1k: PHY %s, ME %s, PHY reset %s\n", ok ? "answers" : "DOES NOT ANSWER",
		  me ? "there" : "none", phy_reset_allowed() ? "allowed" : "blocked by the ME");
}

/*
 * The rings the hardware may still hold descriptors of, emptied before
 * a reset when the PCI configuration space says so (Skylake's PCH and
 * later), so that the reset does not hang
 */
LOCAL void pch_flush_rings( void )
{
	UW	st;

	wr(FEXTNVM11, rd(FEXTNVM11) | 0x2000U);
	st = ts_pcie_cfg_read(pci.bus, pci.dev, pci.fn, 0xe4, 2);
	if ( ( st & 0x100 ) == 0 ) return;
	/* the transmit side needs a ring of its own set up: at the start there is none to flush */
	st = ts_pcie_cfg_read(pci.bus, pci.dev, pci.fn, 0xe4, 2);
	if ( st & 0x100 ) {
		wr(RCTL, rd(RCTL) & ~RCTL_EN);
		WaitUsec(150);
		wr(RXDCTL0, ( rd(RXDCTL0) & 0xffffc000U ) | 0x1f | ( 1U << 8 ) | 0x01000000U);
		wr(RCTL, rd(RCTL) | RCTL_EN);
		WaitUsec(150);
		wr(RCTL, rd(RCTL) & ~RCTL_EN);
	}
}

/* The MAC and (when the ME allows) the PHY reset together, and the LAN's initialization waited for */
LOCAL void pch_reset( void )
{
	UW	c;
	INT	i;
	BOOL	phy = phy_reset_allowed();

	if ( spt ) pch_flush_rings();
	wr(CTRL, rd(CTRL) | CTRL_GIO_MASTER_DISABLE);
	for ( i = 0; i < 800 && ( rd(STATUS) & STATUS_GIO_MASTER ); i++ ) WaitUsec(100);
	wr(IMC, ~0U);
	wr(RCTL, 0);
	wr(TCTL, TCTL_PSP);
	(void)rd(STATUS);
	tk_dly_tsk(10);
	c = rd(CTRL);
	if ( phy ) c |= CTRL_PHY_RST;
	(void)swflag_take();
	wr(CTRL, c | CTRL_RST);			/* not read back: the MAC is away during its reset */
	tk_dly_tsk(20);
	if ( phy ) {
		tk_dly_tsk(10);
		for ( i = 0; i < 1500 && ( rd(STATUS) & STATUS_LAN_INIT_DONE ) == 0; i++ ) WaitUsec(150);
		if ( i >= 1500 ) tm_printf((UB *)"e1k: the LAN did not say it is initialized\n");
		wr(STATUS, rd(STATUS) & ~( STATUS_LAN_INIT_DONE | STATUS_PHYRA ));
		tk_dly_tsk(10);
	}
	swflag_give();
	wr(IMC, ~0U);
	(void)rd(ICR);
	wr(KABGTXD, rd(KABGTXD) | 0x50000U);
	wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_DRV_LOAD);	/* the driver has the card, as the ME is told */
}

/* The chipset's own settings of the MAC, after the reset */
LOCAL void pch_bits( void )
{
	UW	t;

	wr(CTRL_EXT, rd(CTRL_EXT) | ( 1U << 22 ) | CTRL_EXT_PHYPDEN);
	wr(TXDCTL0, rd(TXDCTL0) | ( 1U << 22 ));
	wr(TXDCTL1, rd(TXDCTL1) | ( 1U << 22 ));
	wr(TARC0, rd(TARC0) | ( 1U << 23 ) | ( 1U << 24 ) | ( 1U << 26 ) | ( 1U << 27 ));
	t = rd(TARC1) | ( 1U << 24 ) | ( 1U << 26 ) | ( 1U << 30 );
	t = ( rd(TCTL) & 0x10000000U ) ? ( t & ~( 1U << 28 ) ) : ( t | ( 1U << 28 ) );
	wr(TARC1, t);
	wr(RFCTL, rd(RFCTL) | 0x40 | 0x80);
	wr(PBECCSTS, rd(PBECCSTS) | 0x10000U);
	wr(CTRL, rd(CTRL) | CTRL_MEHE);
}

/* Skylake's PCH: two errata of its transmit side, once the transmit unit is set up */
LOCAL void spt_tx_errata( void )
{
	wr(IOSFPC, rd(IOSFPC) | 0x00010000U);
	wr(TARC0, ( rd(TARC0) & ~0x30000000U ) | 0x20000000U);
}

/* What depends on the link's speed, set when the link comes up (and K1 kept off where the analog part asks) */
LOCAL void pch_link( void )
{
	UW	st = rd(STATUS), v;
	BOOL	up = ( st & STATUS_LU ) != 0, g = ( st & STATUS_SPEED_1000 ) != 0;

	if ( up ) {
		UW	ipg = ( !( st & STATUS_FD ) && !( st & ( STATUS_SPEED_100 | STATUS_SPEED_1000 ) ) ) ? 0xff
			    : ( spt && ( st & STATUS_FD ) && !g ) ? 0x0c : 0x08;

		wr(TIPG, ( rd(TIPG) & ~0x3ffU ) | ipg);
		if ( swflag_take() ) {
			v = phy_rd(PLL_CLOCK_GATE);
			if ( v != ~0U ) (void)phy_wr(PLL_CLOCK_GATE, ( v & ~0x7ffU ) | ( g ? 0xfa : 0x3e8 ));
			if ( g ) {
				v = phy_rd(HV_PM_CTRL);
				if ( v != ~0U ) (void)phy_wr(HV_PM_CTRL, v | HV_PM_K1_CLK_REQ);
			}
			if ( spt ) {
				v = phy_rd(SPT_776_20);
				if ( g ) {
					if ( v != ~0U && ( ( v >> 2 ) & 0x3ff ) < 0x18 ) {
						(void)phy_wr(SPT_776_20, ( v & ~( 0x3ffU << 2 ) ) | ( 0x18 << 2 ));
					}
				} else {
					(void)phy_wr(SPT_776_20, 0xc023);
				}
			}
			swflag_give();
		}
	}
	wr(FEXTNVM4, ( rd(FEXTNVM4) & ~7U ) | 7);		/* the beacon: 8 us */
	if ( spt ) {
		wr(FEXTNVM6, ( rd(PCIEANACFG) & 0x80000000U ) ? ( rd(FEXTNVM6) | 0x80000000U )
							      : ( rd(FEXTNVM6) & ~0x80000000U ));
	}
}

/* What a particular card wants besides */
LOCAL void e1k_quirks( void )
{
}

/* ---------------------------------------------------------------- what netsel.c calls */

EXPORT ER knl_e1k_get_mac( UB *m )
{
	if ( !e1k_ok ) return E_NOEXS;
	knl_memcpy(m, mac, NET_MAC_LEN);
	return E_OK;
}

EXPORT ER knl_e1k_send( CONST void *frame, SZ len )
{
	volatile TXD	*d;
	INT		i;
	ER		er = E_OK;

	if ( !e1k_ok ) return E_NOEXS;
	if ( frame == NULL || len < 14 || len > NET_FRAME_MAX ) return E_PAR;
	tk_loc_mtx(tx_mtx, TMO_FEVR);
	d = &tx[tx_next];
	/* the slot free again: the card has sent what was in it */
	for ( i = 0; i < 1000 && d->cmd != 0 && ( d->status & 1 ) == 0; i++ ) WaitUsec(10);
	knl_memcpy(txbuf + tx_next * BUF, frame, (INT)len);
	d->addr = txbuf_pa + tx_next * BUF;
	d->len = (UH)len;
	d->cso = 0;
	d->status = 0;
	d->cmd = 0x0b;				/* end of packet, the FCS added, status reported */
	DeviceBarrier();
	tx_next = ( tx_next + 1 ) % NTX;
	wr(TDT, tx_next);
	tk_unl_mtx(tx_mtx);
	n_sent++;
	return er;
}

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return t.lo;
}

/*
 * The interrupts the card has raised, for the console (and so
 * KERNEL.TXT): the first, then each tenfold; none 10 seconds after the
 * start is said too, as the messages or the line do not arrive
 */
#define IRQ_SILENT_MS	10000

LOCAL void irq_report( void )
{
	UW	n = n_irq;

	if ( intno == 0 ) return;
	if ( n >= irq_mark ) {
		tm_printf((UB *)"e1k: %u interrupts arrived (%s, interrupt %d, PRC %d)\n", n,
			  ts_pcie_irq_name(irq_kind), intno, (INT)GetIntTarget(intno));
		while ( irq_mark <= n && irq_mark < 1000000000U ) irq_mark *= 10;
	} else if ( n == 0 && !irq_silent_told && now_ms() - t_start >= IRQ_SILENT_MS ) {
		irq_silent_told = TRUE;
		if ( !e1k_ok || ( rd(STATUS) & STATUS_LU ) == 0 ) {
			/* with no link there is nothing to receive or send: none is due */
			tm_printf((UB *)"e1k: no interrupt in %d s, as none is due: the link is down"
				  " (no cable, or nothing at its other end) (%s, interrupt %d)\n",
				  IRQ_SILENT_MS / 1000, ts_pcie_irq_name(irq_kind), intno);
		} else {
			tm_printf((UB *)"e1k: no interrupt arrived in %d s with the link up (%s, interrupt %d)\n",
				  IRQ_SILENT_MS / 1000, ts_pcie_irq_name(irq_kind), intno);
		}
	}
}

EXPORT INT knl_e1k_recv( void *frame, SZ size, TMO tmout )
{
	UINT	ptn;
	INT	waited = 0, len = 0;

	if ( !e1k_ok ) return E_NOEXS;
	if ( tk_loc_mtx(rx_mtx, tmout) < E_OK ) return E_BUSY;
	for ( ;; ) {
		if ( e1k_asleep ) {
			if ( tmout == TMO_POL ) {
				len = E_TMOUT;
				break;
			}
			tk_dly_tsk(20);
			continue;
		}
		irq_report();
		if ( pch && ( rd(STATUS) & ( STATUS_LU | STATUS_SPEED_100 | STATUS_SPEED_1000 | STATUS_FD ) ) != link_was ) {
			link_was = rd(STATUS) & ( STATUS_LU | STATUS_SPEED_100 | STATUS_SPEED_1000 | STATUS_FD );
			pch_link();
			tm_printf((UB *)"e1k: link %s\n", ( link_was & STATUS_LU ) ? "up" : "down");
		}
		volatile RXD	*d = &rx[rx_next];

		if ( d->status & 1 ) {
			BOOL	good = ( ( d->status & 2 ) != 0 && d->errors == 0 );

			len = d->len;
			if ( good && len <= (INT)size ) {
				knl_memcpy(frame, rxbuf + rx_next * BUF, len);
				n_recv++;
			} else {
				n_drop++;
				len = 0;
			}
			d->status = 0;
			DeviceBarrier();
			wr(RDT, rx_next);
			rx_next = ( rx_next + 1 ) % NRX;
			if ( len > 0 ) break;
			continue;
		}
		if ( tmout == TMO_POL || ( tmout != TMO_FEVR && waited >= tmout ) ) {
			len = E_TMOUT;
			break;
		}
		if ( intno != 0 ) {
			(void)tk_wai_flg(flg, 1, TWF_ORW | TWF_BITCLR, &ptn, 10);
			waited += 10;
		} else {
			tk_dly_tsk(1);
			waited += 1;
		}
	}
	tk_unl_mtx(rx_mtx);
	return len;
}

EXPORT ER knl_e1k_stat( T_NETSTAT *st )
{
	knl_memset(st, 0, sizeof(*st));
	st->sent = n_sent;
	st->recv = n_recv;
	st->drop = n_drop;
	st->up = e1k_ok && ( rd(STATUS) & STATUS_LU ) != 0;
	return e1k_ok ? E_OK : E_NOEXS;
}

/* Whether a change of the link is told, by the card's interrupt (knl_netsel_silent) */
EXPORT BOOL knl_e1k_told( void )
{
	return intno != 0;
}

/* The link's speed in Mb/s, 0 while it is down, and the card's device number (the adapters' list) */
EXPORT INT knl_e1k_speed( UH *p_device )
{
	UW	st;

	if ( !e1k_ok ) return E_NOEXS;
	if ( p_device != NULL ) *p_device = pci.device;
	st = rd(STATUS);
	if ( ( st & STATUS_LU ) == 0 ) return 0;
	return ( st & STATUS_SPEED_1000 ) ? 1000 : ( st & STATUS_SPEED_100 ) ? 100 : 10;
}

/* ---------------------------------------------------------------- start-up */

/*
 * The card from its reset to receiving and sending, on the rings it has:
 * at start-up and after a sleep
 */
LOCAL void hw_start( void )
{
	UW	ral, rah;
	INT	i;

	/* the address the card had: its NVM's, which the reset loads again */
	ral = rd(RAL0);
	rah = rd(RAH0);
	if ( pch ) {
		pch_prepare();
		pch_reset();
		pch_bits();
	} else {
		wr(IMC, ~0U);
		wr(CTRL, rd(CTRL) | CTRL_RST);
		tk_dly_tsk(20);
		for ( i = 0; i < 100 && ( rd(CTRL) & CTRL_RST ); i++ ) tk_dly_tsk(1);
		wr(IMC, ~0U);
		(void)rd(ICR);
	}
	e1k_quirks();
	if ( ( rd(RAH0) & 0x80000000U ) != 0 ) {
		ral = rd(RAL0);
		rah = rd(RAH0);
	}
	for ( i = 0; i < 4; i++ ) mac[i] = (UB)( ral >> ( 8 * i ) );
	mac[4] = (UB)rah;
	mac[5] = (UB)( rah >> 8 );
	wr(RAL0, ral);
	wr(RAH0, ( rah & 0xffff ) | 0x80000000U);
	for ( i = 0; i < 128; i++ ) wr(MTA + 4 * i, 0);
	wr(CTRL, ( rd(CTRL) | CTRL_SLU ) & ~( CTRL_FRCSPD | CTRL_FRCDPX ));
	if ( pch && swflag_take() ) {
		UW	bmcr = phy_rd(0, 0);

		if ( bmcr != ~0U ) (void)phy_wr(0, 0, bmcr | 0x1200);	/* auto-negotiation on, and again */
		swflag_give();
	}

	knl_memset((void *)rx, 0, NRX * sizeof(RXD));
	knl_memset((void *)tx, 0, NTX * sizeof(TXD));
	rx_next = 0;
	tx_next = 0;
	for ( i = 0; i < NRX; i++ ) rx[i].addr = rxbuf_pa + (UD)i * BUF;
	wr(RDBAL, (UW)rx_pa);
	wr(RDBAH, (UW)( rx_pa >> 32 ));
	wr(RDLEN, NRX * sizeof(RXD));
	wr(RDH, 0);
	wr(RDT, NRX - 1);
	wr(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | ( 1U << 4 ));	/* all multicast: IPv6 asks for it */
	wr(TDBAL, (UW)tx_pa);
	wr(TDBAH, (UW)( tx_pa >> 32 ));
	wr(TDLEN, NTX * sizeof(TXD));
	wr(TDH, 0);
	wr(TDT, 0);
	wr(TIPG, ( pch ? 8 : 10 ) | ( 8 << 10 ) | ( 6 << 20 ));
	wr(TCTL, TCTL_EN | TCTL_PSP | ( 15 << 4 ) | ( 63 << 12 ) | ( pch ? 0x01000000U : 0 ));	/* RTLC on the chipset's */
	if ( spt ) spt_tx_errata();
}

/*
 * The machine asleep: nothing sent (the lock for sending stays held by the
 * sleep's task), the receiver and its looks at the registers stopped. On
 * waking the card starts over from its reset.
 */
LOCAL ER e1k_pm_suspend( void *arg, INT how )
{
	if ( !e1k_ok ) return E_OK;
	tk_loc_mtx(tx_mtx, TMO_FEVR);
	e1k_asleep = TRUE;
	tk_dly_tsk(30);
	wr(IMC, ~0U);
	wr(RCTL, 0);
	wr(TCTL, 0);
	return E_OK;
}

LOCAL void e1k_pm_resume( void *arg, INT how )
{
	if ( !e1k_ok ) return;
	hw_start();
	if ( intno != 0 ) {
		wr(IMC, ~0U);
		(void)rd(ICR);
		wr(IMS, IM_RXT0 | IM_LSC);
	}
	e1k_asleep = FALSE;
	tk_unl_mtx(tx_mtx);
	tm_printf((UB *)"e1k: back from the sleep\n");
}

LOCAL CONST T_PMDEV	e1k_pm = { "e1k", PM_ORD_NET, e1k_pm_suspend, e1k_pm_resume, NULL };

IMPORT BOOL	knl_igc_is( UH device );

EXPORT INT knl_e1k_init( void )
{
	T_CFLG	cf;
	T_CMTX	cm;
	INT	i, k;

	for ( k = 0; ; k++ ) {
		if ( ts_pcie_find(0x02, 0x00, 0xff, k, &pci) != E_OK ) {
			return 0;			/* no Intel Ethernet card */
		}
		if ( pci.vendor == 0x8086 && !knl_igc_is(pci.device) ) break;	/* those have another layout */
	}
	ts_pcie_enable(&pci);
	base = ts_pcie_bar_base(&pci, 0);
	if ( base == 0 ) {
		return 0;
	}
	spt = in_ids(pci.device, spt_ids, (INT)( sizeof(spt_ids) / sizeof(spt_ids[0]) ));
	pch = spt || in_ids(pci.device, later_ids, (INT)( sizeof(later_ids) / sizeof(later_ids[0]) ));

	rx = (volatile RXD *)pages(0, &rx_pa);
	tx = (volatile TXD *)pages(0, &tx_pa);
	rxbuf = (UB *)pages(5, &rxbuf_pa);		/* 64 buffers of 2KB */
	txbuf = (UB *)pages(4, &txbuf_pa);		/* 32 */
	if ( rx == NULL || tx == NULL || rxbuf == NULL || txbuf == NULL ) {
		return E_NOMEM;
	}
	hw_start();

	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	flg = tk_cre_flg(&cf);
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	tx_mtx = tk_cre_mtx(&cm);
	rx_mtx = tk_cre_mtx(&cm);
	if ( flg <= 0 || tx_mtx <= 0 || rx_mtx <= 0 ) {
		return E_LIMIT;
	}
	/* MSI-X, else MSI (the I219 has only MSI), else its line; polled when none */
	irq_kind = ts_pcie_irq_alloc(&pci, &intno, TRUE);
	if ( irq_kind > PCI_IRQ_POLL && intno != 0 ) {
		wr(IMC, ~0U);
		(void)rd(ICR);
		knl_define_inthdr((INT)intno, TA_HLNG, (FP)e1k_inthdr);
		EnableInt(intno, INTPRI_DEVICE);
		knl_int_owner(intno, (CONST UB *)"neta");	/* the processor its object asks for (design 4.5) */
		wr(IMS, IM_RXT0 | IM_LSC);
	} else {
		irq_kind = PCI_IRQ_POLL;
		intno = 0;
	}
	t_start = now_ms();
	e1k_ok = TRUE;
	(void)ts_pm_register(&e1k_pm);
	for ( i = 0; i < 100 && ( rd(STATUS) & STATUS_LU ) == 0; i++ ) tk_dly_tsk(10);
	if ( pch ) {
		link_was = rd(STATUS) & ( STATUS_LU | STATUS_SPEED_100 | STATUS_SPEED_1000 | STATUS_FD );
		pch_link();
	}
	tm_printf((UB *)"TessronOS: e1k %04x:%04x%s, %02x:%02x:%02x:%02x:%02x:%02x, link %s, interrupt %d\n",
		  pci.vendor, pci.device, spt ? " (Skylake PCH)" : pch ? " (PCH)" : "",
		  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
		  ( rd(STATUS) & STATUS_LU ) ? "up" : "down", intno);
	tm_printf((UB *)"e1k: interrupt by %s\n", ts_pcie_irq_name(irq_kind));
	return 1;
}

#endif /* X64 */
