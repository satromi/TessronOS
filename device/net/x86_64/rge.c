/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rge.c (x64 PC)
 *	The Realtek RTL8125, RTL8125B and RTL8125D 2.5 gigabit Ethernet, device
 *	10ec:8125.
 *
 *	These chips run from RAM: at each start the MAC's and the PHY's MCU are
 *	loaded and the PHY given its settings, which depend on the revision
 *	(rge_hw.h, made by tools/x64/nic/gen_hw.py). Another revision on this
 *	device number is refused with a line on the console.
 *
 *	The card raises an interrupt when its link changes.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "nicring.h"
#include "lanpci.h"

typedef UH	uint16_t;

#define ETHER_HDR_LEN		14
#define ETHER_CRC_LEN		4
#define ETHER_VLAN_ENCAP_LEN	4

#define MAC_R25			0
#define MAC_R25B		1
#define MAC_R25D_1		2
#define MAC_R25D_2		3

struct rge_softc {
	INT	rge_type;
	UH	rge_rcodever;
};

LOCAL T_LAN		rge;
LOCAL struct rge_softc	sc_;

#define nitems(a)	( (INT)( sizeof(a) / sizeof((a)[0]) ) )
#define DELAY(us)	WaitUsec(us)

#define RGE_READ_1(sc, r)	in_b(rge.base + (r))
#define RGE_READ_2(sc, r)	in_h(rge.base + (r))
#define RGE_READ_4(sc, r)	in_w(rge.base + (r))
#define RGE_WRITE_1(sc, r, v)	out_b(rge.base + (r), (UB)(v))
#define RGE_WRITE_2(sc, r, v)	out_h(rge.base + (r), (UH)(v))
#define RGE_WRITE_4(sc, r, v)	out_w(rge.base + (r), (UW)(v))
#define RGE_SETBIT_1(sc, r, v)	RGE_WRITE_1(sc, r, RGE_READ_1(sc, r) | (v))
#define RGE_CLRBIT_1(sc, r, v)	RGE_WRITE_1(sc, r, RGE_READ_1(sc, r) & ~(v))
#define RGE_SETBIT_2(sc, r, v)	RGE_WRITE_2(sc, r, RGE_READ_2(sc, r) | (v))
#define RGE_CLRBIT_2(sc, r, v)	RGE_WRITE_2(sc, r, RGE_READ_2(sc, r) & ~(v))
#define RGE_SETBIT_4(sc, r, v)	RGE_WRITE_4(sc, r, RGE_READ_4(sc, r) | (v))
#define RGE_CLRBIT_4(sc, r, v)	RGE_WRITE_4(sc, r, RGE_READ_4(sc, r) & ~(v))
#define RGE_PHY_SETBIT(sc, r, v) rge_write_phy_ocp(sc, r, rge_read_phy_ocp(sc, r) | (v))
#define RGE_PHY_CLRBIT(sc, r, v) rge_write_phy_ocp(sc, r, rge_read_phy_ocp(sc, r) & ~(v))
#define RGE_MAC_SETBIT(sc, r, v) rge_write_mac_ocp(sc, r, rge_read_mac_ocp(sc, r) | (v))
#define RGE_MAC_CLRBIT(sc, r, v) rge_write_mac_ocp(sc, r, rge_read_mac_ocp(sc, r) & ~(v))
#define RGE_EPHY_SETBIT(sc, r, v) rge_write_ephy(sc, r, rge_read_ephy(sc, r) | (v))
#define RGE_EPHY_CLRBIT(sc, r, v) rge_write_ephy(sc, r, rge_read_ephy(sc, r) & ~(v))

LOCAL void	rge_write_phy_ocp( struct rge_softc *sc, UH reg, UH val );
LOCAL UH	rge_read_phy_ocp( struct rge_softc *sc, UH reg );
LOCAL void	rge_write_ephy( struct rge_softc *sc, UH reg, UH val );
LOCAL UH	rge_read_ephy( struct rge_softc *sc, UH reg );
LOCAL void	rge_patch_phy_mcu( struct rge_softc *sc, INT set );
LOCAL void	rge_phy_config_mcu( struct rge_softc *sc, UH rcodever );

#include "rge_hw.h"

/* ---------------------------------------------------------------- the indirect registers */

LOCAL void rge_write_csi( struct rge_softc *sc, UW reg, UW val )
{
	INT	i;

	RGE_WRITE_4(sc, RGE_CSIDR, val);
	RGE_WRITE_4(sc, RGE_CSIAR, ( reg & RGE_CSIAR_ADDR_MASK ) | ( RGE_CSIAR_BYTE_EN << RGE_CSIAR_BYTE_EN_SHIFT )
		    | RGE_CSIAR_BUSY);
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		if ( ( RGE_READ_4(sc, RGE_CSIAR) & RGE_CSIAR_BUSY ) == 0 ) break;
	}
	DELAY(20);
}

LOCAL UW rge_read_csi( struct rge_softc *sc, UW reg )
{
	INT	i;

	RGE_WRITE_4(sc, RGE_CSIAR, ( reg & RGE_CSIAR_ADDR_MASK ) | ( RGE_CSIAR_BYTE_EN << RGE_CSIAR_BYTE_EN_SHIFT ));
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		if ( RGE_READ_4(sc, RGE_CSIAR) & RGE_CSIAR_BUSY ) break;
	}
	DELAY(20);
	return RGE_READ_4(sc, RGE_CSIDR);
}

LOCAL void rge_write_mac_ocp( struct rge_softc *sc, UH reg, UH val )
{
	UW	t = (UW)( reg >> 1 ) << RGE_MACOCP_ADDR_SHIFT;

	t += val;
	t |= RGE_MACOCP_BUSY;
	RGE_WRITE_4(sc, RGE_MACOCP, t);
}

LOCAL UH rge_read_mac_ocp( struct rge_softc *sc, UH reg )
{
	RGE_WRITE_4(sc, RGE_MACOCP, (UW)( reg >> 1 ) << RGE_MACOCP_ADDR_SHIFT);
	return (UH)( RGE_READ_4(sc, RGE_MACOCP) & RGE_MACOCP_DATA_MASK );
}

LOCAL void rge_write_ephy( struct rge_softc *sc, UH reg, UH val )
{
	UW	t = ( (UW)reg & RGE_EPHYAR_ADDR_MASK ) << RGE_EPHYAR_ADDR_SHIFT;
	INT	i;

	t |= RGE_EPHYAR_BUSY | ( val & RGE_EPHYAR_DATA_MASK );
	RGE_WRITE_4(sc, RGE_EPHYAR, t);
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		if ( ( RGE_READ_4(sc, RGE_EPHYAR) & RGE_EPHYAR_BUSY ) == 0 ) break;
	}
	DELAY(20);
}

LOCAL UH rge_read_ephy( struct rge_softc *sc, UH reg )
{
	UW	v = ( (UW)reg & RGE_EPHYAR_ADDR_MASK ) << RGE_EPHYAR_ADDR_SHIFT;
	INT	i;

	RGE_WRITE_4(sc, RGE_EPHYAR, v);
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		v = RGE_READ_4(sc, RGE_EPHYAR);
		if ( v & RGE_EPHYAR_BUSY ) break;
	}
	DELAY(20);
	return (UH)( v & RGE_EPHYAR_DATA_MASK );
}

LOCAL void rge_write_phy_ocp( struct rge_softc *sc, UH reg, UH val )
{
	UW	t = (UW)( reg >> 1 ) << RGE_PHYOCP_ADDR_SHIFT;
	INT	i;

	t |= RGE_PHYOCP_BUSY | val;
	RGE_WRITE_4(sc, RGE_PHYOCP, t);
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		if ( ( RGE_READ_4(sc, RGE_PHYOCP) & RGE_PHYOCP_BUSY ) == 0 ) break;
	}
}

LOCAL UH rge_read_phy_ocp( struct rge_softc *sc, UH reg )
{
	UW	v = (UW)( reg >> 1 ) << RGE_PHYOCP_ADDR_SHIFT;
	INT	i;

	RGE_WRITE_4(sc, RGE_PHYOCP, v);
	for ( i = 0; i < 20000; i++ ) {
		DELAY(1);
		v = RGE_READ_4(sc, RGE_PHYOCP);
		if ( v & RGE_PHYOCP_BUSY ) break;
	}
	return (UH)( v & RGE_PHYOCP_DATA_MASK );
}

/* A register of the MII block, by its number: the PHY's own window */
LOCAL void rge_write_phy( struct rge_softc *sc, UH reg, UH val )
{
	UW	addr = (UW)( RGE_PHYBASE + reg / 8 ) << 4;

	addr += ( ( 0x10 + reg % 8 ) - 16 ) << 1;
	rge_write_phy_ocp(sc, (UH)addr, val);
}

LOCAL UH rge_read_phy( struct rge_softc *sc, UH reg )
{
	UW	addr = (UW)( RGE_PHYBASE + reg / 8 ) << 4;

	addr += ( ( 0x10 + reg % 8 ) - 16 ) << 1;
	return rge_read_phy_ocp(sc, (UH)addr);
}

/* ---------------------------------------------------------------- the MCUs */

LOCAL void rge_patch_phy_mcu( struct rge_softc *sc, INT set )
{
	INT	i;

	if ( set ) RGE_PHY_SETBIT(sc, 0xb820, 0x0010);
	else RGE_PHY_CLRBIT(sc, 0xb820, 0x0010);
	for ( i = 0; i < 1000; i++ ) {
		if ( set ) {
			if ( rge_read_phy_ocp(sc, 0xb800) & 0x0040 ) break;
		} else {
			if ( ( rge_read_phy_ocp(sc, 0xb800) & 0x0040 ) == 0 ) break;
		}
		DELAY(100);
	}
	if ( i == 1000 ) tm_printf((UB *)"net: rge: the PHY's MCU did not answer\n");
}

LOCAL void rge_phy_config_mcu( struct rge_softc *sc, UH rcodever )
{
	INT	i;

	if ( sc->rge_rcodever == rcodever ) return;
	rge_patch_phy_mcu(sc, 1);
	if ( sc->rge_type == MAC_R25 ) {
		rge_write_phy_ocp(sc, 0xa436, 0x8024);
		rge_write_phy_ocp(sc, 0xa438, 0x8601);
		rge_write_phy_ocp(sc, 0xa436, 0xb82e);
		rge_write_phy_ocp(sc, 0xa438, 0x0001);
		RGE_PHY_SETBIT(sc, 0xb820, 0x0080);
		for ( i = 0; i < nitems(mac_r25_mcu); i++ ) {
			rge_write_phy_ocp(sc, mac_r25_mcu[i].reg, mac_r25_mcu[i].val);
		}
		RGE_PHY_CLRBIT(sc, 0xb820, 0x0080);
		rge_write_phy_ocp(sc, 0xa436, 0);
		rge_write_phy_ocp(sc, 0xa438, 0);
		RGE_PHY_CLRBIT(sc, 0xb82e, 0x0001);
		rge_write_phy_ocp(sc, 0xa436, 0x8024);
		rge_write_phy_ocp(sc, 0xa438, 0);
	} else if ( sc->rge_type == MAC_R25D_1 ) {
		/* the image goes in three parts, the MCU's patch mode left and entered between them */
		for ( i = 0; i < 2403; i++ ) rge_write_phy_ocp(sc, mac_r25d_1_mcu[i].reg, mac_r25d_1_mcu[i].val);
		rge_patch_phy_mcu(sc, 0);
		rge_patch_phy_mcu(sc, 1);
		for ( ; i < 2528; i++ ) rge_write_phy_ocp(sc, mac_r25d_1_mcu[i].reg, mac_r25d_1_mcu[i].val);
		rge_patch_phy_mcu(sc, 0);
		rge_patch_phy_mcu(sc, 1);
		for ( ; i < nitems(mac_r25d_1_mcu); i++ ) {
			rge_write_phy_ocp(sc, mac_r25d_1_mcu[i].reg, mac_r25d_1_mcu[i].val);
		}
	} else if ( sc->rge_type == MAC_R25D_2 ) {
		for ( i = 0; i < 1269; i++ ) rge_write_phy_ocp(sc, mac_r25d_2_mcu[i].reg, mac_r25d_2_mcu[i].val);
		rge_patch_phy_mcu(sc, 0);
		rge_patch_phy_mcu(sc, 1);
		for ( ; i < nitems(mac_r25d_2_mcu); i++ ) {
			rge_write_phy_ocp(sc, mac_r25d_2_mcu[i].reg, mac_r25d_2_mcu[i].val);
		}
	} else {
		for ( i = 0; i < nitems(mac_r25b_mcu); i++ ) {
			rge_write_phy_ocp(sc, mac_r25b_mcu[i].reg, mac_r25b_mcu[i].val);
		}
	}
	rge_patch_phy_mcu(sc, 0);
	rge_write_phy_ocp(sc, 0xa436, 0x801e);
	rge_write_phy_ocp(sc, 0xa438, rcodever);
}

LOCAL void switch_mcu_page( struct rge_softc *sc, INT page )
{
	UH	v = rge_read_mac_ocp(sc, 0xe446) & ~0x0003;

	rge_write_mac_ocp(sc, 0xe446, v | (UH)page);
}

#define is_r25d(sc)	( (sc)->rge_type == MAC_R25D_1 || (sc)->rge_type == MAC_R25D_2 )

LOCAL void mac_config_mcu( struct rge_softc *sc )
{
	INT	i, p;
	UH	r;

	if ( sc->rge_type == MAC_R25 ) {
		for ( p = 0; p < 3; p++ ) {
			switch_mcu_page(sc, p);
			for ( i = 0; i < nitems(rtl8125_mac_bps); i++ ) {
				if ( p == 0 ) {
					rge_write_mac_ocp(sc, rtl8125_mac_bps[i].reg, rtl8125_mac_bps[i].val);
				} else if ( p == 1 || rtl8125_mac_bps[i].reg < 0xf9f8 ) {
					rge_write_mac_ocp(sc, rtl8125_mac_bps[i].reg, 0);
				}
			}
			if ( p == 2 ) {
				rge_write_mac_ocp(sc, 0xf9f8, 0x6486);
				rge_write_mac_ocp(sc, 0xf9fa, 0x0b15);
				rge_write_mac_ocp(sc, 0xf9fc, 0x090e);
				rge_write_mac_ocp(sc, 0xf9fe, 0x1139);
			}
		}
		rge_write_mac_ocp(sc, 0xfc26, 0x8000);
		rge_write_mac_ocp(sc, 0xfc2a, 0x0540);
		rge_write_mac_ocp(sc, 0xfc2e, 0x0a06);
		rge_write_mac_ocp(sc, 0xfc30, 0x0eb8);
		rge_write_mac_ocp(sc, 0xfc32, 0x3a5c);
		rge_write_mac_ocp(sc, 0xfc34, 0x10a8);
		rge_write_mac_ocp(sc, 0xfc40, 0x0d54);
		rge_write_mac_ocp(sc, 0xfc42, 0x0e24);
		rge_write_mac_ocp(sc, 0xfc48, 0x307a);
	} else if ( sc->rge_type == MAC_R25B ) {
		switch_mcu_page(sc, 0);
		for ( i = 0; i < nitems(rtl8125b_mac_bps); i++ ) {
			rge_write_mac_ocp(sc, rtl8125b_mac_bps[i].reg, rtl8125b_mac_bps[i].val);
		}
	} else if ( sc->rge_type == MAC_R25D_1 ) {
		static CONST struct rge_regval first[] = {
			{ 0xf800, 0xe002 }, { 0xf802, 0xe006 }, { 0xf804, 0x4166 }, { 0xf806, 0x9cf6 },
			{ 0xf808, 0xc002 }, { 0xf80a, 0xb800 }, { 0xf80c, 0x14a4 }, { 0xf80e, 0xc102 },
			{ 0xf810, 0xb900 }
		};
		for ( p = 0; p < 3; p++ ) {
			switch_mcu_page(sc, p);
			for ( i = 0; i < nitems(first); i++ ) {
				rge_write_mac_ocp(sc, first[i].reg, ( p == 0 ) ? first[i].val : 0);
			}
			for ( r = 0xf812; r <= 0xf9f6; r += 2 ) rge_write_mac_ocp(sc, r, 0);
			rge_write_mac_ocp(sc, 0xf9f8, ( p == 2 ) ? 0x6938 : 0);
			rge_write_mac_ocp(sc, 0xf9fa, ( p == 2 ) ? 0x0a18 : 0);
			rge_write_mac_ocp(sc, 0xf9fc, ( p == 2 ) ? 0x0217 : 0);
			rge_write_mac_ocp(sc, 0xf9fe, ( p == 2 ) ? 0x0d2a : 0);
		}
		rge_write_mac_ocp(sc, 0xfc26, 0x8000);
		rge_write_mac_ocp(sc, 0xfc28, 0x14a2);
		rge_write_mac_ocp(sc, 0xfc48, 0x0001);
	} else {
		for ( p = 0; p < 3; p++ ) {
			switch_mcu_page(sc, p);
			for ( i = 0; i < nitems(rtl8125d_2_mac_bps); i++ ) {
				rge_write_mac_ocp(sc, rtl8125d_2_mac_bps[i].reg, ( p == 0 ) ? rtl8125d_2_mac_bps[i].val : 0);
			}
			for ( r = 0xf884; r <= 0xf9f6; r += 2 ) rge_write_mac_ocp(sc, r, 0);
			rge_write_mac_ocp(sc, 0xf9f8, ( p == 2 ) ? 0x6938 : 0);
			rge_write_mac_ocp(sc, 0xf9fa, ( p == 2 ) ? 0x0a19 : 0);
			rge_write_mac_ocp(sc, 0xf9fc, ( p == 2 ) ? 0x030e : 0);
			rge_write_mac_ocp(sc, 0xf9fe, ( p == 2 ) ? 0x0b2f : 0);
		}
		rge_write_mac_ocp(sc, 0xfc26, 0x8000);
		rge_write_mac_ocp(sc, 0xfc28, 0x2382);
		rge_write_mac_ocp(sc, 0xfc48, 0x0001);
	}
}

/* ---------------------------------------------------------------- the chip */

LOCAL void disable_aspm_clkreq( struct rge_softc *sc )
{
	BOOL	unlock = ( RGE_READ_1(sc, RGE_EECMD) & RGE_EECMD_WRITECFG ) != RGE_EECMD_WRITECFG;

	if ( unlock ) RGE_SETBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
	RGE_CLRBIT_1(sc, RGE_CFG2, RGE_CFG2_CLKREQ_EN);
	RGE_CLRBIT_1(sc, RGE_CFG5, RGE_CFG5_PME_STS);
	if ( unlock ) RGE_CLRBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
}

LOCAL void chip_reset( struct rge_softc *sc )
{
	INT	i;

	RGE_CLRBIT_4(sc, RGE_RXCFG, RGE_RXCFG_ALLPHYS | RGE_RXCFG_INDIV | RGE_RXCFG_MULTI | RGE_RXCFG_BROAD
		     | RGE_RXCFG_RUNT | RGE_RXCFG_ERRPKT);
	RGE_SETBIT_1(sc, RGE_PPSW, 0x08);		/* the RXDV gate on */
	RGE_SETBIT_1(sc, RGE_CMD, RGE_CMD_STOPREQ);
	DELAY(200);
	for ( i = 0; i < 3000; i++ ) {
		DELAY(50);
		if ( ( RGE_READ_1(sc, RGE_MCUCMD) & ( RGE_MCUCMD_RXFIFO_EMPTY | RGE_MCUCMD_TXFIFO_EMPTY ) )
		     == ( RGE_MCUCMD_RXFIFO_EMPTY | RGE_MCUCMD_TXFIFO_EMPTY ) ) break;
	}
	if ( sc->rge_type != MAC_R25 ) {
		for ( i = 0; i < 3000; i++ ) {
			DELAY(50);
			if ( ( RGE_READ_2(sc, RGE_IM) & 0x0103 ) == 0x0103 ) break;
		}
	}
	RGE_WRITE_1(sc, RGE_CMD, RGE_READ_1(sc, RGE_CMD) & ( RGE_CMD_TXENB | RGE_CMD_RXENB ));
	RGE_WRITE_1(sc, RGE_CMD, RGE_CMD_RESET);
	for ( i = 0; i < RGE_TIMEOUT; i++ ) {
		DELAY(100);
		if ( ( RGE_READ_1(sc, RGE_CMD) & RGE_CMD_RESET ) == 0 ) break;
	}
	if ( i == RGE_TIMEOUT ) tm_printf((UB *)"net: rge: the reset did not end\n");
}

LOCAL void hw_reset( struct rge_softc *sc )
{
	RGE_WRITE_4(sc, RGE_IMR, 0);
	RGE_WRITE_4(sc, RGE_ISR, RGE_READ_4(sc, RGE_ISR));
	RGE_WRITE_4(sc, RGE_TIMERINT0, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT1, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT2, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT3, 0);
	chip_reset(sc);
}

/* From what the firmware left: out of its management mode, the PHY powered, the MCUs loaded */
LOCAL void chipinit( struct rge_softc *sc )
{
	UH	reg;
	INT	i;

	rge_write_mac_ocp(sc, 0xc0bc, 0x00ff);		/* no wake-up from the network */
	chip_reset(sc);
	RGE_CLRBIT_1(sc, RGE_MCUCMD, RGE_MCUCMD_IS_OOB);
	RGE_MAC_CLRBIT(sc, 0xe8de, 0x4000);
	for ( i = 0; i < 10; i++ ) {
		DELAY(100);
		if ( RGE_READ_2(sc, RGE_TWICMD) & 0x0200 ) break;
	}
	rge_write_mac_ocp(sc, 0xc0aa, 0x07d0);
	rge_write_mac_ocp(sc, 0xc0a6, 0x01b5);
	rge_write_mac_ocp(sc, 0xc01e, 0x5555);
	for ( i = 0; i < 10; i++ ) {
		DELAY(100);
		if ( RGE_READ_2(sc, RGE_TWICMD) & 0x0200 ) break;
	}
	if ( rge_read_mac_ocp(sc, 0xd42c) & 0x0100 ) {
		for ( i = 0; i < RGE_TIMEOUT; i++ ) {
			if ( ( rge_read_phy_ocp(sc, 0xa420) & 0x0007 ) == 2 ) break;
			DELAY(1000);
		}
		RGE_MAC_CLRBIT(sc, 0xd42c, 0x0100);
		if ( sc->rge_type != MAC_R25 ) RGE_PHY_CLRBIT(sc, 0xa466, 0x0001);
		RGE_PHY_CLRBIT(sc, 0xa468, 0x000a);
	}

	RGE_SETBIT_1(sc, RGE_PMCH, 0xc0);
	rge_write_phy(sc, 0, 0x1000);			/* BMCR: auto-negotiation on */
	for ( i = 0; i < RGE_TIMEOUT; i++ ) {
		if ( ( rge_read_phy_ocp(sc, 0xa420) & 0x0007 ) == 3 ) break;
		DELAY(1000);
	}

	disable_aspm_clkreq(sc);
	RGE_CLRBIT_1(sc, 0xf1, 0x80);
	RGE_MAC_CLRBIT(sc, 0xd40a, 0x0010);		/* UPS off */
	disable_aspm_clkreq(sc);
	rge_write_mac_ocp(sc, 0xfc48, 0);
	for ( reg = 0xfc28; reg < 0xfc48; reg += 2 ) rge_write_mac_ocp(sc, reg, 0);
	DELAY(3000);
	rge_write_mac_ocp(sc, 0xfc26, 0);
	mac_config_mcu(sc);
	if ( sc->rge_type == MAC_R25 && rge_read_phy_ocp(sc, 0xc416) != 0x0500 ) {
		rge_patch_phy_mcu(sc, 1);
		rge_write_phy_ocp(sc, 0xc416, 0);
		rge_write_phy_ocp(sc, 0xc416, 0x0500);
		rge_patch_phy_mcu(sc, 0);
	}
	rge_write_csi(sc, 0x108, rge_read_csi(sc, 0x108) | 0x00100000);
	hw_reset(sc);
}

LOCAL BOOL phy_config( struct rge_softc *sc )
{
	INT	i;

	/* the RTL8125D's PCIe PHY needs nothing set */
	if ( sc->rge_type == MAC_R25 ) rge_ephy_config_mac_r25(sc);
	else if ( sc->rge_type == MAC_R25B ) rge_ephy_config_mac_r25b(sc);

	/* the PHY reset, advertising nothing */
	rge_write_phy(sc, 4, rge_read_phy(sc, 4) & ~0x01e0);
	rge_write_phy(sc, 9, rge_read_phy(sc, 9) & ~0x0300);
	RGE_PHY_CLRBIT(sc, 0xa5d4, RGE_ADV_2500TFDX);
	rge_write_phy(sc, 0, 0x9200);			/* BMCR: reset, auto-negotiation on, and again */
	for ( i = 0; i < 2500; i++ ) {
		if ( ( rge_read_phy(sc, 0) & 0x8000 ) == 0 ) break;
		DELAY(1000);
	}
	if ( i == 2500 ) {
		tm_printf((UB *)"net: rge: the PHY's reset did not end\n");
		return FALSE;
	}
	rge_write_phy_ocp(sc, 0xa436, 0x801e);
	sc->rge_rcodever = rge_read_phy_ocp(sc, 0xa438);
	switch ( sc->rge_type ) {
	case MAC_R25:		rge_phy_config_mac_r25(sc); break;
	case MAC_R25B:		rge_phy_config_mac_r25b(sc); break;
	case MAC_R25D_1:	rge_phy_config_mac_r25d_1(sc); break;
	default:		rge_phy_config_mac_r25d_2(sc); break;
	}

	RGE_PHY_CLRBIT(sc, 0xa5b4, 0x8000);
	/* Energy-Efficient Ethernet off */
	RGE_MAC_CLRBIT(sc, 0xe040, 0x0003);
	if ( sc->rge_type == MAC_R25 ) {
		RGE_MAC_CLRBIT(sc, 0xeb62, 0x0006);
		RGE_PHY_CLRBIT(sc, 0xa432, 0x0010);
	} else {
		RGE_PHY_SETBIT(sc, 0xa432, 0x0010);
	}
	RGE_PHY_CLRBIT(sc, 0xa5d0, 0x0006);
	RGE_PHY_CLRBIT(sc, 0xa6d4, 0x0001);
	RGE_PHY_CLRBIT(sc, 0xa6d8, 0x0010);
	RGE_PHY_CLRBIT(sc, 0xa428, 0x0080);
	RGE_PHY_CLRBIT(sc, 0xa4a2, 0x0200);
	RGE_MAC_CLRBIT(sc, 0xe052, 0x0001);
	RGE_PHY_CLRBIT(sc, 0xa442, 0x3000);
	RGE_PHY_CLRBIT(sc, 0xa430, 0x8000);
	return TRUE;
}

LOCAL void set_mac( struct rge_softc *sc, CONST UB *a )
{
	RGE_SETBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
	RGE_WRITE_4(sc, RGE_MAC0, ( (UW)a[3] << 24 ) | ( (UW)a[2] << 16 ) | ( (UW)a[1] << 8 ) | a[0]);
	RGE_WRITE_4(sc, RGE_MAC4, ( (UW)a[5] << 8 ) | a[4]);
	RGE_CLRBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
}

LOCAL void rge_hw_stop( T_LAN *l )
{
	struct rge_softc *sc = &sc_;

	(void)l;
	RGE_CLRBIT_4(sc, RGE_RXCFG, RGE_RXCFG_ALLPHYS | RGE_RXCFG_INDIV | RGE_RXCFG_MULTI | RGE_RXCFG_BROAD
		     | RGE_RXCFG_RUNT | RGE_RXCFG_ERRPKT);
	hw_reset(sc);
	RGE_MAC_CLRBIT(sc, 0xc0ac, 0x1f80);
}

LOCAL void rge_hw_start( T_LAN *l )
{
	struct rge_softc *sc = &sc_;
	UW	v;
	INT	i;

	rge_hw_stop(l);
	set_mac(sc, l->mac);
	knl_memset(l->rx, 0, 4096);
	knl_memset(l->tx, 0, 4096);
	rge_ring_init((RGE_RXD *)l->rx, LAN_NRX, l->rxbuf_pa, LAN_BUF, (RGE_TXD *)l->tx, LAN_NTX);
	l->rx_next = l->tx_next = 0;
	chipinit(sc);
	if ( !phy_config(sc) ) return;

	RGE_SETBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
	RGE_CLRBIT_1(sc, 0xf1, 0x80);
	disable_aspm_clkreq(sc);
	RGE_WRITE_2(sc, RGE_EEE_TXIDLE_TIMER, RGE_JUMBO_MTU + ETHER_HDR_LEN + 32);

	RGE_WRITE_4(sc, RGE_RXDESC_ADDR_LO, (UW)l->rx_pa);
	RGE_WRITE_4(sc, RGE_RXDESC_ADDR_HI, (UW)( l->rx_pa >> 32 ));
	RGE_WRITE_4(sc, RGE_TXDESC_ADDR_LO, (UW)l->tx_pa);
	RGE_WRITE_4(sc, RGE_TXDESC_ADDR_HI, (UW)( l->tx_pa >> 32 ));

	RGE_WRITE_4(sc, RGE_RXCFG, ( sc->rge_type == MAC_R25 ) ? RGE_RXCFG_CONFIG :
		    ( sc->rge_type == MAC_R25B ) ? RGE_RXCFG_CONFIG_8125B : RGE_RXCFG_CONFIG_8125D);
	RGE_WRITE_4(sc, RGE_TXCFG, RGE_TXCFG_CONFIG);

	v = rge_read_csi(sc, 0x70c) & ~0x3f000000U;
	rge_write_csi(sc, 0x70c, v | 0x27000000U);
	if ( !is_r25d(sc) ) RGE_WRITE_2(sc, 0x0382, 0x221b);
	RGE_WRITE_1(sc, RGE_RSS_CTRL, 0);
	RGE_WRITE_2(sc, RGE_RXQUEUE_CTRL, RGE_READ_2(sc, RGE_RXQUEUE_CTRL) & ~0x001c);	/* one queue */
	RGE_CLRBIT_1(sc, RGE_CFG1, RGE_CFG1_SPEED_DOWN);
	rge_write_mac_ocp(sc, 0xc140, 0xffff);
	rge_write_mac_ocp(sc, 0xc142, 0xffff);
	RGE_MAC_SETBIT(sc, 0xeb58, 0x0001);

	v = rge_read_mac_ocp(sc, 0xe614) & ~0x0700;
	rge_write_mac_ocp(sc, 0xe614, (UH)( v | ( ( sc->rge_type == MAC_R25B ) ? 0x0200 : 0x0300 ) ));
	rge_write_mac_ocp(sc, 0xe63e, rge_read_mac_ocp(sc, 0xe63e) & ~0x0c00);
	v = rge_read_mac_ocp(sc, 0xe63e) & ~0x0030;
	rge_write_mac_ocp(sc, 0xe63e, (UH)( v | 0x0020 ));
	RGE_MAC_CLRBIT(sc, 0xc0b4, 0x0001);
	RGE_MAC_SETBIT(sc, 0xc0b4, 0x0001);
	RGE_MAC_SETBIT(sc, 0xc0b4, 0x000c);
	v = rge_read_mac_ocp(sc, 0xeb6a) & ~0x00ff;
	rge_write_mac_ocp(sc, 0xeb6a, (UH)( v | 0x0033 ));
	v = rge_read_mac_ocp(sc, 0xeb50) & ~0x03e0;
	rge_write_mac_ocp(sc, 0xeb50, (UH)( v | 0x0040 ));
	RGE_MAC_CLRBIT(sc, 0xe056, 0x00f0);
	RGE_WRITE_1(sc, RGE_TDFNR, 0x10);
	RGE_MAC_CLRBIT(sc, 0xe040, 0x1000);
	v = rge_read_mac_ocp(sc, 0xea1c) & ~0x0003;
	rge_write_mac_ocp(sc, 0xea1c, (UH)( v | 0x0001 ));
	rge_write_mac_ocp(sc, 0xe0c0, is_r25d(sc) ? 0x4403 : 0x4000);
	RGE_MAC_SETBIT(sc, 0xe052, 0x0060);
	RGE_MAC_CLRBIT(sc, 0xe052, 0x0088);
	v = rge_read_mac_ocp(sc, 0xd430) & ~0x0fff;
	rge_write_mac_ocp(sc, 0xd430, (UH)( v | 0x045f ));
	RGE_SETBIT_1(sc, RGE_DLLPR, RGE_DLLPR_PFM_EN | RGE_DLLPR_TX_10M_PS_EN);
	if ( sc->rge_type == MAC_R25 ) RGE_SETBIT_1(sc, RGE_MCUCMD, 0x01);
	if ( !is_r25d(sc) ) RGE_MAC_CLRBIT(sc, 0xe080, 0x0002);		/* EEE plus off */
	RGE_MAC_CLRBIT(sc, 0xea1c, 0x0004);
	RGE_MAC_SETBIT(sc, 0xeb54, 0x0001);		/* the TCAM's entries cleared */
	DELAY(1);
	RGE_MAC_CLRBIT(sc, 0xeb54, 0x0001);
	RGE_CLRBIT_2(sc, 0x1880, 0x0030);
	if ( sc->rge_type != MAC_R25 ) RGE_CLRBIT_1(sc, RGE_INT_CFG0, RGE_INT_CFG0_EN);
	RGE_WRITE_4(sc, RGE_TIMERINT0, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT1, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT2, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT3, 0);
	for ( i = 0; i < ( ( sc->rge_type == MAC_R25B ) ? 32 : 64 ); i++ ) RGE_WRITE_4(sc, RGE_INTMITI(i), 0);
	RGE_MAC_SETBIT(sc, 0xc0ac, 0x1f80);
	rge_write_mac_ocp(sc, 0xe098, 0xc302);
	RGE_MAC_CLRBIT(sc, 0xe032, 0x0003);
	rge_write_csi(sc, 0x98, rge_read_csi(sc, 0x98) & ~0x0000ff00U);
	if ( is_r25d(sc) ) {
		v = rge_read_mac_ocp(sc, 0xe092) & ~0x00ff;
		rge_write_mac_ocp(sc, 0xe092, (UH)( v | 0x0008 ));
	} else {
		RGE_MAC_CLRBIT(sc, 0xe092, 0x00ff);
	}
	RGE_SETBIT_2(sc, RGE_CPLUSCMD, RGE_CPLUSCMD_RXCSUM);
	(void)RGE_READ_2(sc, RGE_CPLUSCMD);
	RGE_WRITE_2(sc, RGE_RXMAXSIZE, RGE_JUMBO_FRAMELEN);
	RGE_CLRBIT_1(sc, RGE_PPSW, 0x08);		/* the RXDV gate off */
	DELAY(2000);

	/* its own address, broadcasts, and every multicast: IPv6 asks for it */
	RGE_WRITE_4(sc, RGE_RXCFG, ( RGE_READ_4(sc, RGE_RXCFG) & ~RGE_RXCFG_ALLPHYS )
		    | RGE_RXCFG_INDIV | RGE_RXCFG_BROAD | RGE_RXCFG_MULTI);
	RGE_WRITE_4(sc, RGE_MAR0, 0xffffffffU);
	RGE_WRITE_4(sc, RGE_MAR4, 0xffffffffU);

	disable_aspm_clkreq(sc);
	RGE_CLRBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
	DELAY(10);

	/* every speed it has, negotiated */
	RGE_PHY_CLRBIT(sc, 0xa428, 0x0200);
	RGE_PHY_CLRBIT(sc, 0xa5ea, 0x0001);
	v = rge_read_phy_ocp(sc, 0xa5d4) | RGE_ADV_2500TFDX;
	rge_write_phy(sc, 4, 0x0de0);			/* ANAR: 10 and 100, both ways of pausing */
	rge_write_phy(sc, 9, 0x0300);			/* 1000 full and half */
	rge_write_phy_ocp(sc, 0xa5d4, (UH)v);
	rge_write_phy(sc, 0, 0x9200);

	RGE_WRITE_1(sc, RGE_CMD, RGE_CMD_TXENB | RGE_CMD_RXENB);
}

LOCAL void rge_irq_on( T_LAN *l )
{
	(void)l;
	RGE_WRITE_4(&sc_, RGE_ISR, RGE_READ_4(&sc_, RGE_ISR));
	RGE_WRITE_2(&sc_, RGE_IM, 0);
	RGE_WRITE_4(&sc_, RGE_TIMERINT0, 0);
	RGE_WRITE_4(&sc_, RGE_IMR, RGE_INTRS);
}

LOCAL void rge_irq_off( T_LAN *l )
{
	(void)l;
	RGE_WRITE_4(&sc_, RGE_IMR, 0);
}

LOCAL BOOL rge_irq_ack( T_LAN *l )
{
	UW	st = RGE_READ_4(&sc_, RGE_ISR);

	(void)l;
	if ( st != 0 && st != ~0U ) RGE_WRITE_4(&sc_, RGE_ISR, st);
	return st != ~0U && ( st & RGE_ISR_LINKCHG ) != 0;
}

LOCAL BOOL rge_link( T_LAN *l, INT *p_mbps, BOOL *p_fdx )
{
	UH	s = RGE_READ_2(&sc_, RGE_PHYSTAT);

	(void)l;
	if ( ( s & RGE_PHYSTAT_LINK ) == 0 ) return FALSE;
	*p_mbps = ( s & RGE_PHYSTAT_2500MBPS ) ? 2500 : ( s & RGE_PHYSTAT_1000MBPS ) ? 1000
		: ( s & RGE_PHYSTAT_100MBPS ) ? 100 : 10;
	*p_fdx = ( s & RGE_PHYSTAT_FDX ) != 0;
	return TRUE;
}

LOCAL INT rge_rx_take( T_LAN *l, UINT i )
{
	return rtl_rx_len(( (RGE_RXD *)l->rx )[i].cmdsts, RGE_RX_FS, RGE_RX_LS);
}

LOCAL void rge_rx_give( T_LAN *l, UINT i )
{
	RGE_RXD	*d = (RGE_RXD *)l->rx + i;

	d->extsts = 0;
	d->addr = l->rxbuf_pa + (UD)i * LAN_BUF;
	DeviceBarrier();
	d->cmdsts = rtl_rx_word(i, LAN_NRX, LAN_BUF);
}

LOCAL BOOL rge_tx_free( T_LAN *l, UINT i )
{
	return ( ( (RGE_TXD *)l->tx )[i].cmdsts & RTL_OWN ) == 0;
}

LOCAL void rge_tx_go( T_LAN *l, UINT i, UINT len )
{
	RGE_TXD	*d = (RGE_TXD *)l->tx + i;

	d->addr = l->txbuf_pa + (UD)i * LAN_BUF;
	d->extsts = 0;
	DeviceBarrier();
	d->cmdsts = rtl_tx_word(i, LAN_NTX, len);
	DeviceBarrier();
	RGE_WRITE_2(&sc_, RGE_TXSTART, RGE_TXSTART_START);
}

LOCAL CONST T_LANOPS rge_ops = {
	rge_hw_start, rge_hw_stop, rge_irq_on, rge_irq_off, rge_irq_ack, rge_link,
	rge_rx_take, rge_rx_give, rge_tx_free, rge_tx_go
};

LOCAL void rge_inthdr( UINT n )
{
	knl_lan_inthdr(&rge, n);
}

EXPORT INT knl_rge_init( void )
{
	struct rge_softc *sc = &sc_;
	INT	k;
	UINT	cap;
	UW	rev, a;
	UH	v;

	for ( k = 0; ; k++ ) {
		if ( ts_pcie_find(0x02, 0x00, 0xff, k, &rge.pci) != E_OK ) return 0;
		if ( rge.pci.vendor == 0x10ec && rge.pci.device == 0x8125 ) break;
	}
	ts_pcie_enable(&rge.pci);
	rge.base = ts_pcie_bar_base(&rge.pci, 2);
	if ( rge.base == 0 ) rge.base = ts_pcie_bar_base(&rge.pci, 1);
	if ( rge.base == 0 ) {
		knl_lan_refuse("RTL8125", &rge.pci, 0, "has no memory window");
		return 0;
	}
	rev = RGE_READ_4(sc, RGE_TXCFG) & RGE_TXCFG_HWREV;
	switch ( rev ) {
	case 0x60900000U:
		sc->rge_type = MAC_R25;
		rge.chip = "RTL8125";
		break;
	case 0x64100000U:
		sc->rge_type = MAC_R25B;
		rge.chip = "RTL8125B";
		break;
	case 0x68800000U:
		sc->rge_type = MAC_R25D_1;
		rge.chip = "RTL8125D";
		break;
	case 0x68900000U:
		sc->rge_type = MAC_R25D_2;
		rge.chip = "RTL8125D";
		break;
	default:
		knl_lan_refuse("RTL8125", &rge.pci, rev, "is not a revision this build has the images of");
		return 0;
	}
	rge.rev = rev;
	rge.card = "rge";
	rge.ops = &rge_ops;

	cap = ts_pcie_cap_find(&rge.pci, 0x10);
	if ( cap != 0 ) {
		v = (UH)ts_pcie_cfg_read(rge.pci.bus, rge.pci.dev, rge.pci.fn, cap + 0x10, 2);
		v &= ~( 0x0003 | 0x0100 );		/* ASPM L0s and L1, the clock power management */
		ts_pcie_cfg_write(rge.pci.bus, rge.pci.dev, rge.pci.fn, cap + 0x10, 2, v);
	}

	chipinit(sc);
	a = RGE_READ_4(sc, RGE_ADDR0);
	for ( k = 0; k < 4; k++ ) rge.mac[k] = (UB)( a >> ( 8 * k ) );
	v = RGE_READ_2(sc, RGE_ADDR1);
	rge.mac[4] = (UB)v;
	rge.mac[5] = (UB)( v >> 8 );

	rge.irq_kind = ts_pcie_irq_alloc(&rge.pci, &rge.intno, TRUE);
	if ( rge.irq_kind > PCI_IRQ_POLL && rge.intno != 0 ) {
		RGE_WRITE_4(sc, RGE_IMR, 0);
		knl_define_inthdr((INT)rge.intno, TA_HLNG, (FP)rge_inthdr);
		EnableInt(rge.intno, INTPRI_DEVICE);
		knl_int_owner(rge.intno, (CONST UB *)"neta");
	} else {
		rge.irq_kind = PCI_IRQ_POLL;
		rge.intno = 0;
	}
	return knl_lan_start(&rge, NETSEL_RGE);
}

#endif /* X64 */
