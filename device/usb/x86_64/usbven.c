/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usbven.c (x64 PC)
 *	USB LAN adapters run through their makers' own protocol: the Realtek
 *	RTL8152, RTL8153 and RTL8153B, the ASIX AX88179, and the ASIX AX88772,
 *	AX88772A and AX88772B (USB 2.0, 10 and 100 Mbit/s).
 *
 *	A device is taken by its vendor and product number only (the tables
 *	below). Many of the Realtek ones also have a configuration of the
 *	communications class, which cdcnet.c can run: when this driver cannot
 *	use the device (another chip behind the same number, as the RTL8156
 *	in some docks, or a device that answers oddly) its attach fails, and
 *	the manager goes on to cdcnet.c as before.
 *
 *	Frames are kept as cdcnet.c keeps them. The link is read every second
 *	at the most, when the adapters' manager asks (nothing here listens to
 *	the adapter's interrupt endpoint), and its line is said on the console
 *	in the form the PCIe drivers use. One adapter is served at a time.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include "../usbdev.h"
#include "../../net/netsel.h"
#include "../../net/x86_64/nicring.h"

#define RT_VENDOR_OUT	0x40
#define RT_VENDOR_IN	0xc0

#define XFER		USB_STREAM_XFER_MAX
#define RX_SLOTS	128
#define RX_FRAME	1536
#define TX_BUF		2048
#define LINK_LOOK_MS	900		/* how old a reading of the link may be */

/* ---------------------------------------------------------------- the devices */

#define KIND_URE	1
#define KIND_AXEN	2
#define KIND_AXE	3
#define KIND_AXE_B	4		/* the AX88772B: as KIND_AXE once attached, with AX772B in the flags */

/*
 * The names of the AX88772's registers: its protocol is the older one of
 * the maker, not the AX88179's
 */
#include "axe_reg.h"

typedef struct {
	UH	vendor, product;
	UB	kind;
} T_ID;

#define AXE_772_DEV(v, p, f)	{ v, p, ( ( (f) & AX772B ) != 0 ) ? KIND_AXE_B : KIND_AXE },

/* The numbers of adapters with the chips of this driver, and of docks that may hold them */
LOCAL CONST T_ID ids[] = {
	{ 0x050d, 0x047a, KIND_URE }, { 0x050d, 0x048a, KIND_URE }, { 0x13b1, 0x0041, KIND_URE },
	{ 0x30da, 0x5101, KIND_URE }, { 0x2d1c, 0x8153, KIND_URE }, { 0x2001, 0x7e34, KIND_URE },
	{ 0x2001, 0xa710, KIND_URE }, { 0x30f3, 0x0419, KIND_URE }, { 0x30f3, 0x0425, KIND_URE },
	{ 0x056e, 0x4013, KIND_URE }, { 0x0f21, 0x8153, KIND_URE }, { 0x17ef, 0x3062, KIND_URE },
	{ 0x17ef, 0x720a, KIND_URE }, { 0x17ef, 0x3054, KIND_URE }, { 0x17ef, 0x304f, KIND_URE },
	{ 0x17ef, 0x3069, KIND_URE }, { 0x17ef, 0x3098, KIND_URE }, { 0x17ef, 0x309b, KIND_URE },
	{ 0x17ef, 0x309c, KIND_URE }, { 0x17ef, 0x309d, KIND_URE }, { 0x17ef, 0x7214, KIND_URE },
	{ 0x17ef, 0x721e, KIND_URE }, { 0x17ef, 0x8153, KIND_URE }, { 0x17ef, 0xa359, KIND_URE },
	{ 0x17ef, 0x7205, KIND_URE }, { 0x17ef, 0x720b, KIND_URE }, { 0x17ef, 0x720c, KIND_URE },
	{ 0x17ef, 0x3052, KIND_URE }, { 0x17ef, 0xa3c1, KIND_URE }, { 0x17ef, 0x3082, KIND_URE },
	{ 0x17ef, 0x3065, KIND_URE }, { 0x17ef, 0xa387, KIND_URE }, { 0x17ef, 0x3057, KIND_URE },
	{ 0x043e, 0x9819, KIND_URE }, { 0x043e, 0x9820, KIND_URE }, { 0x208e, 0xd003, KIND_URE },
	{ 0x045e, 0x07ab, KIND_URE }, { 0x045e, 0x07c6, KIND_URE }, { 0x045e, 0x0927, KIND_URE },
	{ 0x045e, 0x0c5e, KIND_URE }, { 0x0955, 0x09ff, KIND_URE }, { 0x2b73, 0x0007, KIND_URE },
	{ 0x2b73, 0x0031, KIND_URE }, { 0x0bda, 0x8152, KIND_URE }, { 0x0bda, 0x8050, KIND_URE },
	{ 0x0bda, 0x8153, KIND_URE }, { 0x04e8, 0xa101, KIND_URE }, { 0x0930, 0x0416, KIND_URE },
	{ 0x2357, 0x0601, KIND_URE }, { 0x2357, 0x0602, KIND_URE }, { 0x2357, 0x0603, KIND_URE },
	{ 0x1025, 0x104e, KIND_URE }, { 0x14ff, 0x1400, KIND_URE }, { 0x2717, 0x0011, KIND_URE },

	{ 0x0b95, 0x1790, KIND_AXEN }, { 0x2001, 0x4a00, KIND_AXEN }, { 0x17ef, 0x304b, KIND_AXEN },
	{ 0x04e8, 0xa100, KIND_AXEN }, { 0x0df6, 0x0072, KIND_AXEN },

	AXE_772_DEVS
};

typedef struct {
	BOOL		used;
	INT		kind;
	UB		flags;
	USBDEV		*dev;
	USBEP		ep_in, ep_out;
	USBPIPE		*pipe;
	UB		mac[NET_MAC_LEN];
	UB		*txbuf;
	CONST char	*chip;
	UW		rev;
	INT		rx_pad;		/* AX88179: what a frame's length holds besides the frame */
	BOOL		link_up, link_fdx;
	INT		link_mbps;
	UW		link_at;
	BOOL		link_read;
} T_USBVEN;

LOCAL T_USBVEN		uv;
LOCAL UB		rx_ring[RX_SLOTS][RX_FRAME];
LOCAL UH		rx_len[RX_SLOTS];
LOCAL volatile UW	rx_head = 0, rx_tail = 0;
LOCAL UB		blk[XFER];
LOCAL ID		rx_sem = 0, tx_mtx = 0;
LOCAL UD		n_sent, n_recv, n_drop, n_err;

/* ---------------------------------------------------------------- the RTL8152 and RTL8153 */

typedef UB	uint8_t;
typedef UH	uint16_t;
typedef UW	uint32_t;
typedef UB	u_char;
typedef UB	uByte;
typedef UB	uWord[2];

#define ETHER_ADDR_LEN		6
#define ETHER_HDR_LEN		14
#define ETHER_CRC_LEN		4
#define ETHER_MAX_LEN		1518
#define ETHER_VLAN_ENCAP_LEN	4
#define UGETW(w)		GET16(w)
#define USETW(w, v)		PUT16(w, v)
#define UGETDW(w)		GET32(w)
#define USETDW(w, v)		PUT32(w, v)
#define htole32(v)		(v)
#define letoh32(v)		(v)
#define roundup(x, y)		( ( ( (x) + ( (y) - 1 ) ) / (y) ) * (y) )
#define DELAY(us)		WaitUsec(us)
#define usbd_delay_ms(d, ms)	knl_usb_wait(ms)
#define printf(...)		tm_printf((UB *)__VA_ARGS__)
#define memset(d, c, n)		knl_memset(d, c, n)
#define ETIMEDOUT		1
#define IFCAP_VLAN_HWTAGGING	0

struct ifnet {
	struct ure_softc *if_softc;
	INT		if_mtu;
	INT		if_capabilities;
};

struct ure_softc {
	INT	ure_flags;
	INT	ure_chip;
	INT	ure_rxbufsz;
	USBDEV	*ure_udev;
	struct {
		UB		ac_enaddr[8];	/* written as 8 bytes */
		struct ifnet	ac_if;
	} ure_ac;
	struct {
		CONST char *dv_xname;
	} ure_dev;
};

/* The sizes of the headers, for the settings made from them */
struct ure_rxpkt { UB b[URE_RX_HDR]; };
struct ure_rxpkt_v2 { UB b[URE_RX_HDR]; };

/* A request of the chip's register space: failures read as zeros, and no version is then known */
LOCAL int ure_ctl( struct ure_softc *sc, uint8_t rw, uint16_t val, uint16_t index, void *buf, int len )
{
	ER	er;

	er = knl_usb_control(sc->ure_udev, ( rw == 0x02 ) ? RT_VENDOR_OUT : RT_VENDOR_IN, USB_REQ_SET_ADDRESS,
			     val, index, (UB *)buf, (UH)len, NULL);
	return ( er < E_OK ) ? -1 : 0;
}

LOCAL uint16_t ure_ocp_reg_read( struct ure_softc *sc, uint16_t addr );
LOCAL void ure_ocp_reg_write( struct ure_softc *sc, uint16_t addr, uint16_t data );
#define ure_phy_read(sc, a)	ure_ocp_reg_read(sc, a)
#define ure_phy_write(sc, a, d)	ure_ocp_reg_write(sc, a, d)

#include "ure_hw.h"

LOCAL struct ure_softc	ure;

/* The version register's value: its name, and what it needs */
LOCAL BOOL ure_identify( UW ver )
{
	switch ( ver ) {
	case 0x4c00: ure.ure_flags = URE_FLAG_8152; ure.ure_chip = URE_CHIP_VER_4C00; uv.chip = "RTL8152"; break;
	case 0x4c10: ure.ure_flags = URE_FLAG_8152; ure.ure_chip = URE_CHIP_VER_4C10; uv.chip = "RTL8152"; break;
	case 0x5c00: ure.ure_chip = URE_CHIP_VER_5C00; uv.chip = "RTL8153"; break;
	case 0x5c10: ure.ure_chip = URE_CHIP_VER_5C10; uv.chip = "RTL8153"; break;
	case 0x5c20: ure.ure_chip = URE_CHIP_VER_5C20; uv.chip = "RTL8153"; break;
	case 0x5c30: ure.ure_chip = URE_CHIP_VER_5C30; uv.chip = "RTL8153"; break;
	case 0x6000: ure.ure_flags = URE_FLAG_8153B; uv.chip = "RTL8153B"; break;
	case 0x6010: ure.ure_flags = URE_FLAG_8153B; ure.ure_chip = URE_CHIP_VER_6010; uv.chip = "RTL8153B"; break;
	default:
		return FALSE;
	}
	return TRUE;
}

LOCAL void ure_phy_start( void )
{
	INT	i;

	ure_phy_write(&ure, URE_OCP_BASE_MII + 0 * 2, 0x8000);		/* BMCR: reset */
	for ( i = 0; i < 100 && ( ure_phy_read(&ure, URE_OCP_BASE_MII) & 0x8000 ); i++ ) knl_usb_wait(1);
	ure_phy_write(&ure, URE_OCP_BASE_MII + 4 * 2, 0x0de1);		/* ANAR: 10 and 100, both ways of pausing */
	if ( ( ure.ure_flags & URE_FLAG_8152 ) == 0 ) {
		ure_phy_write(&ure, URE_OCP_BASE_MII + 9 * 2, 0x0300);	/* 1000 full and half */
	}
	ure_phy_write(&ure, URE_OCP_BASE_MII, 0x1200);			/* BMCR: auto-negotiation on, and again */
}

/* From what the adapter came up as to receiving; FALSE: not a chip here, or it did not answer */
LOCAL BOOL ure_start( USBDEV *dev )
{
	UB		a[8];
	UW		hash[2], rcr;
	UW		ver;
	ER		er;

	knl_memset(&ure, 0, sizeof(ure));
	ure.ure_udev = dev;
	ure.ure_rxbufsz = XFER;			/* the chip gathers frames up to what one transfer takes */
	ure.ure_dev.dv_xname = "ure";
	ure.ure_ac.ac_if.if_softc = &ure;
	ure.ure_ac.ac_if.if_mtu = 1500;
	ver = ure_read_2(&ure, URE_PLA_TCR1, URE_MCU_TYPE_PLA) & URE_VERSION_MASK;
	uv.rev = ver;
	if ( !ure_identify(ver) ) {
		tm_printf((UB *)"net: %04x:%04x link none: version %04x is not a chip of this driver\n",
			  (INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10), ver);
		return FALSE;
	}
	switch ( ure.ure_flags & URE_FLAG_CHIP_MASK ) {
	case URE_FLAG_8152:
		ure_rtl8152_init(&ure);
		er = E_OK;
		break;
	case URE_FLAG_8153B:
		er = ( ure_rtl8153b_init(&ure) != 0 ) ? E_IO : E_OK;
		break;
	default:
		er = ( ure_rtl8153_init(&ure) != 0 ) ? E_IO : E_OK;
		break;
	}
	if ( er < E_OK ) return FALSE;
	if ( ure.ure_chip & URE_CHIP_VER_4C00 ) ure_read_mem(&ure, URE_PLA_IDR, URE_MCU_TYPE_PLA, a, sizeof(a));
	else ure_read_mem(&ure, URE_PLA_BACKUP, URE_MCU_TYPE_PLA, a, sizeof(a));
	knl_memcpy(uv.mac, a, NET_MAC_LEN);
	knl_memcpy(ure.ure_ac.ac_enaddr, a, NET_MAC_LEN);

	if ( ure.ure_flags & URE_FLAG_8152 ) er = ( ure_rtl8152_nic_reset(&ure) != 0 ) ? E_IO : E_OK;
	else er = ( ure_rtl8153_nic_reset(&ure) != 0 ) ? E_IO : E_OK;
	if ( er < E_OK ) return FALSE;

	ure_ifmedia_init(&ure.ure_ac.ac_if);

	/* its own address, broadcasts, and every multicast: IPv6 asks for it */
	rcr = ure_read_4(&ure, URE_PLA_RCR, URE_MCU_TYPE_PLA);
	rcr = ( rcr & ~URE_RCR_ACPT_ALL ) | URE_RCR_APM | URE_RCR_AB | URE_RCR_AM;
	hash[0] = hash[1] = 0xffffffffU;
	ure_write_mem(&ure, URE_PLA_MAR, URE_MCU_TYPE_PLA | URE_BYTE_EN_DWORD, hash, sizeof(hash));
	ure_write_4(&ure, URE_PLA_RCR, URE_MCU_TYPE_PLA, rcr);
	return TRUE;
}

LOCAL void ure_read_link( void )
{
	UH	s = ure_read_2(&ure, URE_PLA_PHYSTATUS, URE_MCU_TYPE_PLA);

	uv.link_up = ( s & URE_PHYSTATUS_LINK ) != 0;
	uv.link_mbps = ( s & URE_PHYSTATUS_1000MBPS ) ? 1000 : ( s & URE_PHYSTATUS_100MBPS ) ? 100 : 10;
	uv.link_fdx = ( s & URE_PHYSTATUS_FDX ) != 0;
}

/* ---------------------------------------------------------------- the AX88179 */

/*
 * The names of the two chips' registers do not meet: these are the names
 * as the AX88179's protocol has them
 */
#include "axen_reg.h"

LOCAL void axen_cmd( INT cmd, INT index, INT val, void *buf )
{
	(void)knl_usb_control(uv.dev, AXEN_CMD_DIR(cmd) ? RT_VENDOR_OUT : RT_VENDOR_IN, AXEN_CMD_CMD(cmd),
			      (UH)val, (UH)index, (UB *)buf, AXEN_CMD_LEN(cmd), NULL);
}

LOCAL UH axen_mii_rd( INT reg )
{
	uWord	v;

	v[0] = v[1] = 0;
	axen_cmd(AXEN_CMD_MII_READ_REG, reg, AXEN_PHY_ID, v);
	return UGETW(v);
}

LOCAL void axen_mii_wr( INT reg, UH val )
{
	uWord	v;

	USETW(v, val);
	axen_cmd(AXEN_CMD_MII_WRITE_REG, reg, AXEN_PHY_ID, v);
}

/* From power on to receiving, without the checksum work: the stack does its own */
LOCAL BOOL axen_start( USBDEV *dev )
{
	uWord	wval;
	UB	val, q[5], f[AXEN_MCAST_FILTER_SIZE];
	UH	ctl, temp, bcd = GET16(dev->ddesc + 12);
	INT	i;

	uv.chip = ( bcd == 0x200 ) ? "AX88179A" : ( bcd == 0x300 ) ? "AX88772D" : "AX88179";
	uv.rev = bcd;
	uv.rx_pad = ( bcd == 0x200 || bcd == 0x300 ) ? 2 : 6;

	USETW(wval, 0);
	knl_memset(uv.mac, 0, NET_MAC_LEN);
	(void)knl_usb_control(dev, RT_VENDOR_IN, AXEN_CMD_CMD(AXEN_CMD_MAC_READ_ETHER), AXEN_CMD_MAC_NODE_ID,
			      ETHER_ADDR_LEN, uv.mac, ETHER_ADDR_LEN, NULL);
	if ( ( uv.mac[0] | uv.mac[1] | uv.mac[2] | uv.mac[3] | uv.mac[4] | uv.mac[5] ) == 0 ) {
		tm_printf((UB *)"net: %s %04x:%04x link none: no address read\n", uv.chip,
			  (INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10));
		return FALSE;
	}

	axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_PHYPWR_RSTCTL, wval);
	USETW(wval, AXEN_PHYPWR_RSTCTL_IPRL);
	axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_PHYPWR_RSTCTL, wval);
	knl_usb_wait(200);
	val = AXEN_PHYCLK_ACS | AXEN_PHYCLK_BCS;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_PHYCLK, &val);
	knl_usb_wait(100);
	val = AXEN_MONITOR_NONE;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_MONITOR_MODE, &val);

	axen_cmd(AXEN_CMD_EEPROM_READ, 2, AXEN_EEPROM_STAT, wval);
	temp = UGETW(wval);
	if ( temp != 0xffff && ( temp & 0x0100 ) == 0 ) {
		val = 0;
		axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_PHYCLK, &val);
		val = AXEN_PHYCLK_ULR;
		axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_PHYCLK, &val);
		knl_usb_wait(100);
		axen_cmd(AXEN_CMD_MAC_READ2, 2, AXEN_PHYPWR_RSTCTL, wval);
		ctl = UGETW(wval) | AXEN_PHYPWR_RSTCTL_AUTODETACH;
		USETW(wval, ctl);
		axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_PHYPWR_RSTCTL, wval);
		knl_usb_wait(200);
	}

	/* the chip gathers frames up to what one transfer takes: the host's 16KB at both fast speeds */
	val = 0;
	axen_cmd(AXEN_CMD_MAC_READ, 1, AXEN_USB_UPLINK, &val);
	q[0] = 0x07;
	switch ( val ) {
	case AXEN_USB_FS: q[1] = 0xcc; q[2] = 0x4c; q[3] = AXEN_BUFSZ_LS - 1; q[4] = 0x08; break;
	case AXEN_USB_HS: q[1] = 0x02; q[2] = 0xa0; q[3] = AXEN_BUFSZ_HS - 1; q[4] = 0xff; break;
	case AXEN_USB_SS: q[1] = 0x4f; q[2] = 0x00; q[3] = AXEN_BUFSZ_HS - 1; q[4] = 0xff; break;
	default:
		tm_printf((UB *)"net: %s: the uplink is unknown (%02x)\n", uv.chip, val);
		return FALSE;
	}
	axen_cmd(AXEN_CMD_MAC_SET_RXSR, 5, AXEN_RX_BULKIN_QCTRL, q);

	axen_cmd(AXEN_CMD_MAC_WRITE_ETHER, ETHER_ADDR_LEN, AXEN_CMD_MAC_NODE_ID, uv.mac);
	val = 0x34;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_PAUSE_LOW_WATERMARK, &val);
	val = 0x52;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_PAUSE_HIGH_WATERMARK, &val);
	val = AXEN_RXCOE_OFF;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_RX_COE, &val);
	val = AXEN_TXCOE_OFF;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_TX_COE, &val);

	ctl = AXEN_RXCTL_IPE | AXEN_RXCTL_DROPCRCERR | AXEN_RXCTL_AUTOB | AXEN_RXCTL_ACPT_BCAST
	    | AXEN_RXCTL_ACPT_PHY_MCAST | AXEN_RXCTL_ACPT_ALL_MCAST | AXEN_RXCTL_START;
	USETW(wval, ctl);
	axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_MAC_RXCTL, wval);

	val = AXEN_MONITOR_PMETYPE | AXEN_MONITOR_PMEPOL | AXEN_MONITOR_RWMP;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_MONITOR_MODE, &val);
	ctl = AXEN_MEDIUM_GIGA | AXEN_MEDIUM_FDX | AXEN_MEDIUM_ALWAYS_ONE
	    | AXEN_MEDIUM_RXFLOW_CTRL_EN | AXEN_MEDIUM_TXFLOW_CTRL_EN | AXEN_MEDIUM_RECV_EN;
	USETW(wval, ctl);
	axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_MEDIUM_STATUS, wval);
	knl_usb_wait(100);

	/* the PHY: a setting of an extension page, then reset and asked for every speed */
	axen_mii_wr(0x1f, 0x0005);
	axen_mii_wr(0x0c, 0x0000);
	axen_mii_wr(0x01, axen_mii_rd(0x01) | 0x0080);
	axen_mii_wr(0x1f, 0x0000);

	val = 0x01;
	axen_cmd(AXEN_CMD_MAC_WRITE, 1, AXEN_UNK_28, &val);
	for ( i = 0; i < AXEN_MCAST_FILTER_SIZE; i++ ) f[i] = 0xff;	/* every multicast: IPv6 asks for it */
	axen_cmd(AXEN_CMD_MAC_WRITE_FILTER, AXEN_MCAST_FILTER_SIZE, AXEN_FILTER_MULTI, f);

	axen_mii_wr(0x00, 0x8000);
	for ( i = 0; i < 100 && ( axen_mii_rd(0x00) & 0x8000 ); i++ ) knl_usb_wait(1);
	axen_mii_wr(0x04, 0x0de1);
	axen_mii_wr(0x09, 0x0300);
	axen_mii_wr(0x00, 0x1200);
	return TRUE;
}

/* The MAC follows the speed the PHY settled on, each time the link comes up */
LOCAL void axen_read_link( void )
{
	UH	s = axen_mii_rd(0x11), ctl;
	uWord	wval;
	BOOL	was = uv.link_up;

	uv.link_up = ( s & 0x0400 ) != 0;
	uv.link_mbps = ( ( s & 0xc000 ) == 0x8000 ) ? 1000 : ( ( s & 0xc000 ) == 0x4000 ) ? 100 : 10;
	uv.link_fdx = ( s & 0x2000 ) != 0;
	if ( uv.link_up && !was ) {
		ctl = AXEN_MEDIUM_RECV_EN | AXEN_MEDIUM_ALWAYS_ONE | AXEN_MEDIUM_RXFLOW_CTRL_EN
		    | AXEN_MEDIUM_TXFLOW_CTRL_EN;
		if ( uv.link_fdx ) ctl |= AXEN_MEDIUM_FDX;
		if ( uv.link_mbps == 1000 ) ctl |= AXEN_MEDIUM_GIGA | AXEN_MEDIUM_EN_125MHZ;
		else if ( uv.link_mbps == 100 ) ctl |= AXEN_MEDIUM_PS;
		USETW(wval, ctl);
		axen_cmd(AXEN_CMD_MAC_WRITE2, 2, AXEN_MEDIUM_STATUS, wval);
	}
}

/* ---------------------------------------------------------------- the AX88772, AX88772A and AX88772B */

LOCAL INT	axe_phyno;		/* the PHY's address on the chip's MII */
LOCAL AXE_RX	axe_rx;

/* A command of the chip: its length and direction are in its number. Index and value as the bus has them */
LOCAL ER axe_cmd( INT cmd, INT index, INT val, void *buf )
{
	return knl_usb_control(uv.dev, AXE_CMD_DIR(cmd) ? RT_VENDOR_OUT : RT_VENDOR_IN, AXE_CMD_CMD(cmd),
			       (UH)val, (UH)index, (UB *)buf, AXE_CMD_LEN(cmd), NULL);
}

/* The MII is the chip's own while it runs: it is taken for the access and given back */
LOCAL UH axe_mii_rd( INT reg )
{
	uWord	v;

	v[0] = v[1] = 0;
	(void)axe_cmd(AXE_CMD_MII_OPMODE_SW, 0, 0, NULL);
	(void)axe_cmd(AXE_CMD_MII_READ_REG, reg, axe_phyno, v);
	(void)axe_cmd(AXE_CMD_MII_OPMODE_HW, 0, 0, NULL);
	return UGETW(v);
}

LOCAL void axe_mii_wr( INT reg, UH val )
{
	uWord	v;

	USETW(v, val);
	(void)axe_cmd(AXE_CMD_MII_OPMODE_SW, 0, 0, NULL);
	(void)axe_cmd(AXE_CMD_MII_WRITE_REG, reg, axe_phyno, v);
	(void)axe_cmd(AXE_CMD_MII_OPMODE_HW, 0, 0, NULL);
}

/* One of the two PHYs the chip reports: its address, or -1 when it is not a kind that can be used */
LOCAL INT axe_phy_of( UB id )
{
	switch ( AXE_PHY_TYPE(id) ) {
	case PHY_TYPE_100_HOME:
	case PHY_TYPE_GIG:
		return AXE_PHY_NO(id);
	default:
		return -1;
	}
}

/*
 * The PHY chosen and powered. The built-in PHY (address 0x10) is
 * selected, held in reset and powered down, then powered up and let run;
 * a PHY outside the chip is selected and the built-in one is left powered
 * down. GPIO2 is driven high and the EEPROM reloaded first.
 */
LOCAL void axe_phy_power( void )
{
	(void)axe_cmd(AXE_CMD_WRITE_GPIO, 0, AXE_GPIO_RELOAD_EEPROM | AXE_GPIO2 | AXE_GPIO2_EN, NULL);
	knl_usb_wait(40);
	if ( axe_phyno == AXE_PHY_NO_AX772_EPHY ) {
		(void)axe_cmd(AXE_CMD_SW_PHY_SELECT, 0, 0x01, NULL);
		knl_usb_wait(10);
		(void)axe_cmd(AXE_CMD_SW_RESET_REG, 0, AXE_SW_RESET_CLEAR, NULL);
		knl_usb_wait(60);
		(void)axe_cmd(AXE_CMD_SW_RESET_REG, 0, AXE_SW_RESET_IPPD | AXE_SW_RESET_PRL, NULL);
		knl_usb_wait(150);
		(void)axe_cmd(AXE_CMD_SW_RESET_REG, 0, AXE_SW_RESET_PRL, NULL);
		(void)axe_cmd(AXE_CMD_SW_RESET_REG, 0, AXE_SW_RESET_IPRL | AXE_SW_RESET_PRL, NULL);
	} else {
		(void)axe_cmd(AXE_CMD_SW_PHY_SELECT, 0, 0x00, NULL);
		knl_usb_wait(10);
		(void)axe_cmd(AXE_CMD_SW_RESET_REG, 0, AXE_SW_RESET_IPPD | AXE_SW_RESET_PRL, NULL);
	}
	knl_usb_wait(150);
	(void)axe_cmd(AXE_CMD_RXCTL_WRITE, 0, 0, NULL);		/* the receiver stopped until it is set up */
}

/*
 * From power on to receiving. The receiving takes the frames for its own
 * address, broadcasts and every multicast (IPv6 asks for it). The AX88772B
 * is told to put the 4 byte header before each frame; the AX88772 and
 * AX88772A at high speed gather frames up to 16KB in one transfer, which
 * is what one transfer of the stream takes.
 */
LOCAL BOOL axe_start( USBDEV *dev )
{
	UB	phy[2], ipg[3];
	uWord	w, rom;
	UH	rxmode;
	INT	i;

	uv.chip = ( uv.flags & AX772B ) ? "AX88772B" : "AX88772";
	uv.rev = GET16(dev->ddesc + 12);
	axe_rx_reset(&axe_rx);

	phy[0] = phy[1] = 0;
	if ( axe_cmd(AXE_CMD_READ_PHYID, 0, 0, phy) < E_OK ) {
		tm_printf((UB *)"net: %s %04x:%04x link none: the PHY was not read\n", uv.chip,
			  (INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10));
		return FALSE;
	}
	axe_phyno = axe_phy_of(phy[AXE_PHY_SEL_PRI]);
	if ( axe_phyno < 0 ) axe_phyno = axe_phy_of(phy[AXE_PHY_SEL_SEC]);
	if ( axe_phyno < 0 ) axe_phyno = 0;
	axe_phy_power();

	knl_memset(uv.mac, 0, NET_MAC_LEN);
	(void)axe_cmd(AXE_178_CMD_READ_NODEID, 0, 0, uv.mac);
	if ( ( uv.flags & AX772B ) && ( uv.mac[0] | uv.mac[1] | uv.mac[2] | uv.mac[3] | uv.mac[4] | uv.mac[5] ) == 0 ) {
		/* the B may come up with no address: it is in the EEPROM, a word at a time in the order sent */
		for ( i = 0; i < ETHER_ADDR_LEN / 2; i++ ) {
			rom[0] = rom[1] = 0;
			(void)axe_cmd(AXE_CMD_SROM_READ, 0, AXE_EEPROM_772B_NODEID + i, rom);
			uv.mac[i * 2] = rom[0];
			uv.mac[i * 2 + 1] = rom[1];
		}
	}
	if ( ( uv.mac[0] | uv.mac[1] | uv.mac[2] | uv.mac[3] | uv.mac[4] | uv.mac[5] ) == 0 ) {
		tm_printf((UB *)"net: %s %04x:%04x link none: no address read\n", uv.chip,
			  (INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10));
		return FALSE;
	}

	/* the gaps between frames: as the chip came up with them */
	ipg[0] = ipg[1] = ipg[2] = 0;
	(void)axe_cmd(AXE_CMD_READ_IPG012, 0, 0, ipg);
	(void)axe_cmd(AXE_178_CMD_WRITE_NODEID, 0, 0, uv.mac);
	(void)axe_cmd(AXE_178_CMD_WRITE_IPG012, ipg[2], ( ipg[1] << 8 ) | ipg[0], NULL);

	w[0] = w[1] = 0;
	(void)axe_cmd(AXE_CMD_RXCTL_READ, 0, 0, w);
	rxmode = UGETW(w) & ~( AXE_RXCMD_ALLMULTI | AXE_RXCMD_MULTICAST | AXE_RXCMD_PROMISC );
	rxmode |= AXE_RXCMD_BROADCAST | AXE_RXCMD_ALLMULTI | AXE_RXCMD_ENABLE;
	if ( uv.flags & AX772B ) rxmode |= AXE_772B_RXCMD_RH1M;
	else if ( dev->speed == USB_SPEED_HIGH ) rxmode |= AXE_178_RXCMD_MFB;
	if ( axe_cmd(AXE_CMD_RXCTL_WRITE, 0, rxmode, NULL) < E_OK ) {
		tm_printf((UB *)"net: %s %04x:%04x link none: the receiver was not set\n", uv.chip,
			  (INT)GET16(dev->ddesc + 8), (INT)GET16(dev->ddesc + 10));
		return FALSE;
	}

	/* the PHY: reset, then asked for what it can do, without pausing (the MAC is not set to pause) */
	axe_mii_wr(0x00, 0x8000);					/* BMCR: reset */
	for ( i = 0; i < 100 && ( axe_mii_rd(0x00) & 0x8000 ); i++ ) knl_usb_wait(1);
	axe_mii_wr(0x04, (UH)( ( ( axe_mii_rd(0x01) & 0xf800 ) >> 6 ) | 0x0001 ));	/* ANAR from BMSR, IEEE 802.3 */
	axe_mii_wr(0x00, 0x1200);					/* BMCR: auto-negotiation on, and again */
	return TRUE;
}

/*
 * The link from the PHY: the status register twice (its link bit holds a
 * loss until read), then the best of what both ends offered. Each time
 * the link comes up or settles on another speed, the MAC is told it.
 */
LOCAL void axe_read_link( void )
{
	UH	bmcr, bmsr, both;
	BOOL	was = uv.link_up, fdx = uv.link_fdx;
	INT	mbps = uv.link_mbps, val;

	(void)axe_mii_rd(0x01);
	bmsr = axe_mii_rd(0x01);
	bmcr = axe_mii_rd(0x00);
	uv.link_up = ( bmsr & 0x0004 ) != 0;
	if ( ( bmcr & 0x1000 ) && ( bmsr & 0x0020 ) == 0 ) uv.link_up = FALSE;	/* still negotiating */
	if ( !uv.link_up ) return;
	if ( bmcr & 0x1000 ) {
		both = axe_mii_rd(0x04) & axe_mii_rd(0x05);			/* ANAR and ANLPAR */
		if ( both & 0x0100 ) { uv.link_mbps = 100; uv.link_fdx = TRUE; }
		else if ( both & 0x0080 ) { uv.link_mbps = 100; uv.link_fdx = FALSE; }
		else if ( both & 0x0040 ) { uv.link_mbps = 10; uv.link_fdx = TRUE; }
		else { uv.link_mbps = 10; uv.link_fdx = FALSE; }
	} else {
		uv.link_mbps = ( bmcr & 0x2000 ) ? 100 : 10;
		uv.link_fdx = ( bmcr & 0x0100 ) != 0;
	}
	if ( !was || uv.link_mbps != mbps || uv.link_fdx != fdx ) {
		val = AXE_178_MEDIA_RX_EN | AXE_178_MEDIA_MAGIC;
		if ( uv.link_fdx ) val |= AXE_MEDIA_FULL_DUPLEX;
		if ( uv.link_mbps == 100 ) val |= AXE_178_MEDIA_100TX;
		(void)axe_cmd(AXE_CMD_WRITE_MEDIA, 0, val, NULL);
	}
}

/* ---------------------------------------------------------------- receiving */

LOCAL void rx_put( void *ctx, CONST UB *frame, INT len )
{
	UW	h = rx_head;

	(void)ctx;
	if ( len < NIC_FRAME_MIN || len > RX_FRAME ) {
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

LOCAL void rx_xfer( void *ctx, DMABUF *b, INT off, INT len, INT err )
{
	INT	bad = 0;

	(void)ctx;
	if ( !uv.used || err != 0 || len <= 0 ) {
		if ( err != 0 ) {
			n_err++;
			axe_rx_reset(&axe_rx);		/* a transfer lost: the AX88772's stream starts again with a header */
		}
		return;
	}
	if ( len > (INT)sizeof(blk) ) len = sizeof(blk);
	knl_dmabuf_read(b, off, blk, len);
	if ( uv.kind == KIND_URE ) (void)ure_rx_walk(blk, len, rx_put, NULL, &bad);
	else if ( uv.kind == KIND_AXE ) (void)axe_rx_feed(&axe_rx, blk, len, rx_put, NULL, &bad);
	else (void)axen_rx_walk(blk, len, uv.rx_pad, rx_put, NULL, &bad);
	n_err += bad;
}

/* ---------------------------------------------------------------- what netsel.c calls */

LOCAL ER uv_get_mac( void *ctx, UB *mac )
{
	(void)ctx;
	if ( !uv.used ) return E_NOEXS;
	knl_memcpy(mac, uv.mac, NET_MAC_LEN);
	return E_OK;
}

LOCAL void copy_frame( void *d, CONST void *s, INT n )
{
	knl_memcpy(d, s, n);
}

LOCAL ER uv_send( void *ctx, CONST void *frame, SZ len )
{
	USBHC	*hc;
	INT	actual = 0, n, boundary;
	ER	er;

	(void)ctx;
	if ( !uv.used ) return E_NOEXS;
	if ( frame == NULL || len < NIC_FRAME_MIN || len > NET_FRAME_MAX ) return E_PAR;
	tk_loc_mtx(tx_mtx, TMO_FEVR);
	if ( uv.kind == KIND_URE ) {
		n = ure_tx_wrap(uv.txbuf, frame, (INT)len, copy_frame);
	} else if ( uv.kind == KIND_AXE ) {
		boundary = ( uv.dev->speed == USB_SPEED_HIGH ) ? 512 : 64;
		n = axe_tx_wrap(uv.txbuf, frame, (INT)len, boundary, copy_frame);
	} else {
		boundary = ( uv.dev->speed == USB_SPEED_SUPER ) ? 4096 : ( uv.dev->speed == USB_SPEED_HIGH ) ? 512 : 64;
		n = axen_tx_wrap(uv.txbuf, frame, (INT)len, boundary, copy_frame);
	}
	knl_usb_lock();
	er = uv.used ? E_OK : E_NOEXS;
	if ( er >= E_OK ) {
		hc = uv.dev->hc;
		er = hc->ops->bulk(hc, uv.dev, &uv.ep_out, uv.txbuf, n, &actual, 1000);
		if ( er >= E_OK && ( n % uv.ep_out.size ) == 0 ) {
			(void)hc->ops->bulk(hc, uv.dev, &uv.ep_out, uv.txbuf, 0, &actual, 1000);	/* the end of the transfer */
		}
	}
	knl_usb_unlock();
	tk_unl_mtx(tx_mtx);
	if ( er >= E_OK ) n_sent++; else n_err++;
	return er;
}

LOCAL INT uv_recv( void *ctx, void *frame, SZ size, TMO tmout )
{
	UW	t;
	INT	len;
	ER	er;

	(void)ctx;
	if ( rx_sem <= 0 ) {
		if ( tmout != TMO_POL ) tk_dly_tsk(( tmout == TMO_FEVR || tmout > 1000 ) ? 1000 : tmout);
		return E_TMOUT;
	}
	er = tk_wai_sem(rx_sem, 1, tmout);
	if ( er < E_OK ) return er;
	t = rx_tail;
	len = rx_len[t % RX_SLOTS];
	if ( len > (INT)size ) len = (INT)size;
	knl_memcpy(frame, rx_ring[t % RX_SLOTS], len);
	rx_tail = t + 1;
	return len;
}

LOCAL void say_link( void )
{
	tm_printf((UB *)"net: %s %04x:%04x link %s %d %s, rev %04x, %02x:%02x:%02x:%02x:%02x:%02x, USB speed %d\n",
		  uv.chip, (INT)GET16(uv.dev->ddesc + 8), (INT)GET16(uv.dev->ddesc + 10),
		  uv.link_up ? "up" : "down", uv.link_up ? uv.link_mbps : 0,
		  uv.link_up ? ( uv.link_fdx ? "full" : "half" ) : "-", uv.rev,
		  uv.mac[0], uv.mac[1], uv.mac[2], uv.mac[3], uv.mac[4], uv.mac[5], uv.dev->speed);
}

/* The link as the adapter has it, read at most once in LINK_LOOK_MS: it takes a transfer on the bus */
LOCAL void link_look( void )
{
	UW	now = knl_usb_ms();
	BOOL	up;
	INT	mbps;

	if ( uv.link_read && now - uv.link_at < LINK_LOOK_MS ) return;
	knl_usb_lock();
	if ( uv.used ) {
		up = uv.link_up;
		mbps = uv.link_mbps;
		if ( uv.kind == KIND_URE ) {
			ure_read_link();
			if ( uv.link_up && !up ) {
				/* the chip forgets its settings at a change of the link */
				ure_ifmedia_init(&ure.ure_ac.ac_if);
			}
		} else if ( uv.kind == KIND_AXE ) {
			axe_read_link();
		} else {
			axen_read_link();
		}
		uv.link_at = now;
		if ( !uv.link_read || uv.link_up != up || ( uv.link_up && uv.link_mbps != mbps ) ) say_link();
		uv.link_read = TRUE;
	}
	knl_usb_unlock();
}

LOCAL ER uv_stat( void *ctx, T_NETSTAT *st )
{
	(void)ctx;
	knl_memset(st, 0, sizeof(*st));
	st->sent = n_sent;
	st->recv = n_recv;
	st->drop = n_drop + n_err;
	if ( !uv.used ) return E_NOEXS;
	link_look();
	st->up = uv.used && uv.link_up;
	return E_OK;
}

LOCAL BOOL uv_told( void *ctx )
{
	(void)ctx;
	return FALSE;
}

LOCAL INT uv_speed( void *ctx )
{
	(void)ctx;
	return ( uv.used && uv.link_up ) ? uv.link_mbps : 0;
}

LOCAL INT uv_label( void *ctx, UB *t, INT max )
{
	CONST char	*s;
	INT		n = 0;

	(void)ctx;
	if ( !uv.used ) return 0;
	for ( s = uv.chip; *s != '\0' && n < max - 1; s++ ) t[n++] = (UB)*s;
	for ( s = " USB LAN"; *s != '\0' && n < max - 1; s++ ) t[n++] = (UB)*s;
	return n;
}

LOCAL CONST T_NETDRV uv_drv = { uv_get_mac, uv_send, uv_recv, uv_stat, uv_told, uv_speed, uv_label, NULL };

/* ---------------------------------------------------------------- the class driver */

/* The two bulk endpoints of the configuration in use, of the maker's own class */
LOCAL BOOL find_eps( USBDEV *dev, CONST UB **p_in, CONST UB **p_out )
{
	UB	*p, *end = dev->cfg + dev->cfglen;
	INT	cls = -1;
	CONST UB *in = NULL, *out = NULL;

	for ( p = dev->cfg; p != NULL && p < end; p = knl_usb_next_desc(p, end) ) {
		if ( p[1] == USB_DT_INTERFACE && p[0] >= 9 ) {
			cls = p[5];
			in = out = NULL;
		} else if ( p[1] == USB_DT_ENDPOINT && p[0] >= 7 && cls == 0xff && ( p[3] & 3 ) == 2 ) {
			if ( p[2] & 0x80 ) in = p; else out = p;
			if ( in != NULL && out != NULL ) {
				*p_in = in;
				*p_out = out;
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* A device by its numbers, in its maker's configuration (with the manager's lock held) */
EXPORT BOOL knl_usbven_match( USBDEV *dev )
{
	UH	v = GET16(dev->ddesc + 8), p = GET16(dev->ddesc + 10);
	CONST UB *in, *out;
	INT	i;

	if ( uv.used ) return FALSE;
	for ( i = 0; i < (INT)( sizeof(ids) / sizeof(ids[0]) ); i++ ) {
		if ( ids[i].vendor == v && ids[i].product == p ) return find_eps(dev, &in, &out);
	}
	return FALSE;
}

EXPORT ER knl_usbven_attach( USBDEV *dev )
{
	USBHC		*hc = dev->hc;
	CONST UB	*in = NULL, *out = NULL;
	UH		v = GET16(dev->ddesc + 8), p = GET16(dev->ddesc + 10);
	INT		i;
	ER		er;

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
	if ( !find_eps(dev, &in, &out) ) return E_NOEXS;
	knl_memset(&uv, 0, sizeof(uv));
	uv.dev = dev;
	for ( i = 0; ids[i].vendor != v || ids[i].product != p; i++ ) {
	}
	uv.kind = ids[i].kind;
	if ( uv.kind == KIND_AXE_B ) {
		uv.kind = KIND_AXE;
		uv.flags = AX772B;
	}
	knl_usb_ep_from_desc(&uv.ep_in, dev, in, NULL);
	knl_usb_ep_from_desc(&uv.ep_out, dev, out, NULL);
	if ( uv.kind == KIND_URE ? !ure_start(dev) : uv.kind == KIND_AXE ? !axe_start(dev) : !axen_start(dev) ) return E_NOEXS;
	if ( uv.txbuf == NULL ) uv.txbuf = (UB *)Kmalloc(TX_BUF);
	if ( uv.txbuf == NULL ) return E_NOMEM;
	rx_head = rx_tail = 0;
	uv.used = TRUE;
	er = hc->ops->stream_open(hc, dev, &uv.ep_in, XFER, rx_xfer, &uv, &uv.pipe);
	if ( er < E_OK ) {
		uv.used = FALSE;
		Kfree(uv.txbuf);
		return er;
	}
	if ( uv.kind == KIND_URE ) ure_phy_start();
	knl_netsel_add(NETSEL_USBV, &uv_drv);
	say_link();
	knl_net_link_said();			/* an adapter came: the adapters are looked at */
	return E_OK;
}

EXPORT void knl_usbven_detach( USBDEV *dev )
{
	if ( !uv.used || uv.dev != dev ) return;
	uv.used = FALSE;
	if ( uv.pipe != NULL ) {
		dev->hc->ops->stream_close(dev->hc, uv.pipe);
		uv.pipe = NULL;
	}
	if ( uv.txbuf != NULL ) {
		Kfree(uv.txbuf);
		uv.txbuf = NULL;
	}
	tm_printf((UB *)"net: %s link gone: the adapter was taken out\n", uv.chip);
	knl_net_link_said();
}

#endif /* X64 */
