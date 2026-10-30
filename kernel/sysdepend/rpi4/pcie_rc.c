/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pcie_rc.c (rpi4)
 *	The BCM2711 PCI Express root complex: the one lane to the VL805 xHCI
 *	on the Raspberry Pi 4 and the Raspberry Pi 400, or to whatever card
 *	is in the Compute Module 4 carrier's slot (docs/tessronos/03-platform.md
 *	3.6, design 10.7).
 *
 *	The firmware leaves the controller in reset, so it is brought up here
 *	in the order the controller needs: the bridge out of reset with the
 *	far end still held, the analogue side powered, and only then the
 *	first read of the root port (read while the bridge is held, it is an
 *	SError); the bridge set up to
 *	answer configuration reads of nothing with all ones rather than an
 *	error, the inbound window (where a card finds memory) and the
 *	outbound window (where the processor finds the cards), the far end
 *	let go, and the link waited for. Then the generic walk
 *	(kernel/sysman/pcie.c) numbers the bus, sizes and places the cards'
 *	windows, and this file answers its configuration reads and writes.
 *
 *	Configuration space is not one flat window. The root port's own
 *	registers are the foot of the block; a function on a bus below is
 *	seen through a 4KB aperture at 0x8000 once its bus, device and
 *	function are written to the index register at 0x9000. The link is
 *	point to point, so on the bus right below the port there is only
 *	device 0.
 *
 *	The windows come from the device tree (/scb/pcie@7d500000): ranges
 *	gives the outbound window, processor 0x6_0000_0000 onwards for PCIe
 *	0xC000_0000 onwards, 1GB; dma-ranges the inbound one, PCIe 0 onwards
 *	for physical 0 onwards, 3GB. The inbound window's register takes only
 *	a power of two, so it is set to 4GB from PCIe 0; the top gigabyte of
 *	it is where the outbound window's addresses are, which is why a card
 *	reaches only the 3GB below. DMA memory is taken below 1GB on this
 *	board anyway (DMA_ZONE_LIMIT).
 *
 *	The Raspberry Pi 4 Model B (from its board revision 1.4) and the
 *	Raspberry Pi 400 keep the VL805's program in the board's EEPROM, not
 *	in an EEPROM of the VL805's own; after a reset of the link the VL805
 *	has none until the VideoCore firmware is asked to load it, with the
 *	address of the function on the bus (the property tag
 *	NOTIFY_XHCI_RESET). It is asked on those two boards whenever the
 *	VL805 is found without a program running (its version word in
 *	configuration space reads 0, as it does after the link's reset). On the Compute Module 4 a card in the slot is any card, and
 *	nothing of the kind is asked.
 *
 *	A card's interrupt is its INTA line, SPI 143 (to 146 for the other
 *	three): the controller's own message interrupt controller is not
 *	used.
 *
 *	NOT VERIFIED ON HARDWARE: QEMU's raspi4b has no PCIe. What to check
 *	on a board is listed in docs/tessronos/03-platform.md 3.6.4.
 */

#include <sys/machine.h>

#ifdef RPI4

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include <ts/board.h>
#include "sysdepend.h"

/* ---------------------------------------------------------------- registers */

#define RC_REG(off)		(PCIE_BASE + (off))

#define RC_CFG			RC_REG(0x0000)		/* the root port's own configuration space */
#define RC_VENDOR_REG1		RC_REG(0x0188)		/* how the inbound window orders bytes */
#define VENDOR_REG1_ENDIAN_MASK	0x0000000c		/* 0: little endian */
#define RC_PRIV1_ID_VAL3	RC_REG(0x043c)		/* the class code the port answers with */
#define CLASS_PCI_BRIDGE	0x00060400
#define CLASS_MASK		0x00ffffff

#define RC_MISC_CTRL		RC_REG(0x4008)
#define MISC_SCB_ACCESS_EN	0x00001000		/* the inbound window may reach memory */
#define MISC_CFG_READ_UR_MODE	0x00002000		/* a read of nothing answers all ones */
#define MISC_MAX_BURST_MASK	0x00300000		/* 0: 128 bytes */
#define MISC_SCB0_SIZE_SHIFT	27
#define MISC_SCB0_SIZE_MASK	0xf8000000

#define RC_MEM_WIN0_LO		RC_REG(0x400c)		/* the PCIe address the window starts at */
#define RC_MEM_WIN0_HI		RC_REG(0x4010)
#define RC_BAR1_CONFIG_LO	RC_REG(0x402c)
#define RC_BAR2_CONFIG_LO	RC_REG(0x4034)		/* the inbound window */
#define RC_BAR2_CONFIG_HI	RC_REG(0x4038)
#define RC_BAR3_CONFIG_LO	RC_REG(0x403c)
#define BAR_CONFIG_SIZE_MASK	0x0000001f		/* 0: off; else log2 of the size less 15 */

#define RC_STATUS		RC_REG(0x4068)
#define STATUS_PHYLINKUP	0x00000010		/* the lane is talking */
#define STATUS_DL_ACTIVE	0x00000020		/* and packets get through */
#define STATUS_PORT_RC		0x00000080		/* the controller is a root complex */
#define RC_REVISION		RC_REG(0x406c)

#define RC_MEM_WIN0_BASE_LIMIT	RC_REG(0x4070)		/* processor side, in megabytes */
#define WIN_BASE_SHIFT		4			/* bits 15:4 */
#define WIN_LIMIT_SHIFT		20			/* bits 31:20 */
#define RC_MEM_WIN0_BASE_HI	RC_REG(0x4080)		/* the megabyte number above 12 bits */
#define RC_MEM_WIN0_LIMIT_HI	RC_REG(0x4084)

#define RC_HARD_DEBUG		RC_REG(0x4204)
#define HARD_DEBUG_CLKREQ_EN	0x00000002		/* the card's CLKREQ# reaches the controller */
#define HARD_DEBUG_SERDES_IDDQ	0x08000000		/* the analogue side is powered down */

#define RC_INTR2_STATUS		RC_REG(0x4300)
#define RC_INTR2_CLR		RC_REG(0x4308)
#define RC_INTR2_MASK_SET	RC_REG(0x4310)

#define RC_EXT_CFG_DATA		RC_REG(0x8000)		/* the aperture: one function below */
#define RC_EXT_CFG_INDEX	RC_REG(0x9000)

#define RC_SW_INIT_1		RC_REG(0x9210)
#define SW_INIT_1_PERST		0x00000001		/* the far end is held in reset */
#define SW_INIT_1_INIT		0x00000002		/* the bridge is held in reset */

/* Bridge registers of the root port */
#define BRIDGE_BUSES		0x18
#define BRIDGE_MEM_BASE		0x20			/* base and limit of what it carries across */
#define BRIDGE_PREF_BASE	0x24
#define BRIDGE_PREF_BASE_HI	0x28
#define BRIDGE_PREF_LIMIT_HI	0x2c

#define LINK_WAIT_MS		100			/* after the far end is let go */
#define LINK_POLL_MS		5

#define VL805_ID		0x34831106		/* device 3483, vendor 1106 (VIA) */
#define VL805_VERSION		0x50			/* configuration space: the program's version, 0 with none */

/* The windows (sysdef.h): what the device tree says, or the defaults */
EXPORT __UINT64_TYPE__	knl_pcie_cpu_offset = PCIE_OUTBOUND_CPU_PA - PCIE_OUTBOUND_BUS_PA;
EXPORT __UINT64_TYPE__	knl_pcie_dma_offset = PCIE_INBOUND_BUS_PA;

LOCAL BOOL	rc_up = FALSE;

/* ---------------------------------------------------------------- config */

/*
 * Where one register of one function is read, 0 for a function that
 * cannot be there: the root port is the only one on bus 0, and the bus
 * right below it, at the far end of the lane, has only device 0. A bus
 * further down, behind a switch, has as many as the switch has.
 */
LOCAL UBINT rc_cfg_addr( UINT bus, UINT dev, UINT fn, UINT off )
{
	if ( !rc_up && bus != 0 ) {
		return 0;
	}
	if ( bus == 0 ) {
		return ( dev == 0 && fn == 0 ) ? RC_CFG + (UBINT)(off & 0xfff) : 0;
	}
	if ( bus == 1 && dev != 0 ) {
		return 0;
	}
	out_w(RC_EXT_CFG_INDEX, (UW)((bus << 20) | (dev << 15) | (fn << 12)));
	(void)in_w(RC_EXT_CFG_INDEX);		/* landed before the aperture is used */

	return RC_EXT_CFG_DATA + (UBINT)(off & 0xfff);
}

EXPORT UW knl_pcie_cfg_in( UINT bus, UINT dev, UINT fn, UINT off, INT width )
{
	UBINT	a = rc_cfg_addr(bus, dev, fn, off);

	if ( a == 0 ) {
		return ( width == 1 ) ? 0xff : ( width == 2 ) ? 0xffff : 0xFFFFFFFFU;
	}
	switch ( width ) {
	case 1:		return (UW)(*(volatile UB *)a);
	case 2:		return (UW)(*(volatile UH *)a);
	default:	return *(volatile UW *)a;
	}
}

EXPORT void knl_pcie_cfg_out( UINT bus, UINT dev, UINT fn, UINT off, UW val, INT width )
{
	UBINT	a = rc_cfg_addr(bus, dev, fn, off);

	if ( a == 0 ) {
		return;
	}
	switch ( width ) {
	case 1:		*(volatile UB *)a = (UB)val;  break;
	case 2:		*(volatile UH *)a = (UH)val;  break;
	default:	*(volatile UW *)a = val;      break;
	}
	Asm("dsb sy" ::: "memory");
}

/* ---------------------------------------------------------------- bring-up */

/* The size field of a window: log2 of the size less 15, for 64KB to 32GB */
LOCAL UW win_size_code( UD size )
{
	UW	lg = 16;

	while ( lg < 35 && ( 1ULL << lg ) < size ) {
		lg++;
	}
	return lg - 15;
}

LOCAL BOOL rc_link_up( void )
{
	UW	st = in_w(RC_STATUS);

	if ( st == 0xFFFFFFFFU ) {
		return FALSE;
	}
	return ( (st & (STATUS_PHYLINKUP | STATUS_DL_ACTIVE)) == (STATUS_PHYLINKUP | STATUS_DL_ACTIVE) );
}

LOCAL void rc_outbound( UD cpu, UD bus, UD size )
{
	UD	base_mb = cpu >> 20;
	UD	limit_mb = ( cpu + size - 1 ) >> 20;

	out_w(RC_MEM_WIN0_LO, (UW)bus);
	out_w(RC_MEM_WIN0_HI, (UW)( bus >> 32 ));
	out_w(RC_MEM_WIN0_BASE_LIMIT, (UW)( ( ( base_mb & 0xfff ) << WIN_BASE_SHIFT )
					  | ( ( limit_mb & 0xfff ) << WIN_LIMIT_SHIFT ) ));
	out_w(RC_MEM_WIN0_BASE_HI, (UW)( ( base_mb >> 12 ) & 0xff ));
	out_w(RC_MEM_WIN0_LIMIT_HI, (UW)( ( limit_mb >> 12 ) & 0xff ));
}

/*
 * Each step of the bring-up is said on the console with the registers
 * that tell how far it got, so that a board that stops in it shows at
 * which step. Only the reset register is read before the bridge is out
 * of reset: a read of the root port's configuration space while the
 * bridge is held is an asynchronous external abort (SError), and the
 * rest of the block is left alone until then too.
 */
LOCAL void rc_say( CONST char *step, BOOL out_of_reset )
{
	if ( out_of_reset ) {
		tm_printf((UB *)"TessronOS: pcie %s: reset %08x status %08x debug %08x\n", step,
			  in_w(RC_SW_INIT_1), in_w(RC_STATUS), in_w(RC_HARD_DEBUG));
	} else {
		tm_printf((UB *)"TessronOS: pcie %s: reset %08x\n", step, in_w(RC_SW_INIT_1));
	}
}

/*
 * Reset, windows and the link. The steps are millisecond apart where the
 * controller wants microseconds: a little longer does no harm, and it
 * keeps the start-up task off the processor.
 *
 * The firmware hands the controller over with the bridge held in reset
 * (and the boot loader, when it looked for a USB drive to boot from,
 * reset it again before starting the kernel). Nothing of the block but
 * the reset register is touched until the bridge is let go and the
 * analogue side is powered; the root port's own configuration space is
 * read first after that.
 */
LOCAL ER rc_start( UD out_cpu, UD out_bus, UD out_size, UD in_bus, UD in_size )
{
	UW	v, code, id;
	INT	t;

	rc_say("as found", FALSE);

	/* the bridge and the far end into reset, then the bridge out of it */
	out_w(RC_SW_INIT_1, in_w(RC_SW_INIT_1) | SW_INIT_1_INIT | SW_INIT_1_PERST);
	tk_dly_tsk(1);
	out_w(RC_SW_INIT_1, in_w(RC_SW_INIT_1) & ~SW_INIT_1_INIT);
	tk_dly_tsk(1);
	rc_say("bridge out of reset", TRUE);

	/* the analogue side powered */
	out_w(RC_HARD_DEBUG, in_w(RC_HARD_DEBUG) & ~HARD_DEBUG_SERDES_IDDQ);
	tk_dly_tsk(1);
	rc_say("serdes on", TRUE);

	/* only now does the root port answer: is it there, and is it a root complex */
	id = in_w(RC_CFG);
	tm_printf((UB *)"TessronOS: pcie root port id %08x, revision %08x\n", id, in_w(RC_REVISION));
	if ( id == 0 || id == 0xFFFFFFFFU ) {
		return E_NOEXS;
	}
	if ( ( in_w(RC_STATUS) & STATUS_PORT_RC ) == 0 ) {
		return E_SYS;		/* the controller is not a root complex */
	}

	/* the bridge reaches memory, reads of nothing answer all ones, bursts of 128 bytes */
	code = win_size_code(in_size);
	v = in_w(RC_MISC_CTRL);
	v |= MISC_SCB_ACCESS_EN | MISC_CFG_READ_UR_MODE;
	v &= ~( MISC_MAX_BURST_MASK | MISC_SCB0_SIZE_MASK );
	v |= ( code << MISC_SCB0_SIZE_SHIFT ) & MISC_SCB0_SIZE_MASK;
	out_w(RC_MISC_CTRL, v);

	/* the inbound window; the other two of the bridge's own are off */
	out_w(RC_BAR2_CONFIG_LO, ( (UW)in_bus & ~0xfU ) | ( code & BAR_CONFIG_SIZE_MASK ));
	out_w(RC_BAR2_CONFIG_HI, (UW)( in_bus >> 32 ));
	out_w(RC_BAR1_CONFIG_LO, in_w(RC_BAR1_CONFIG_LO) & ~BAR_CONFIG_SIZE_MASK);
	out_w(RC_BAR3_CONFIG_LO, in_w(RC_BAR3_CONFIG_LO) & ~BAR_CONFIG_SIZE_MASK);

	/* nothing of the controller's own interrupts: masked and cleared */
	out_w(RC_INTR2_MASK_SET, 0xFFFFFFFFU);
	out_w(RC_INTR2_CLR, 0xFFFFFFFFU);

	/* the outbound window, the port's class and the inbound window's byte order */
	rc_outbound(out_cpu, out_bus, out_size);
	out_w(RC_PRIV1_ID_VAL3, ( in_w(RC_PRIV1_ID_VAL3) & ~CLASS_MASK ) | CLASS_PCI_BRIDGE);
	out_w(RC_VENDOR_REG1, in_w(RC_VENDOR_REG1) & ~VENDOR_REG1_ENDIAN_MASK);
	tm_printf((UB *)"TessronOS: pcie windows set: misc %08x inbound %08x\n",
		  in_w(RC_MISC_CTRL), in_w(RC_BAR2_CONFIG_LO));

	/* the far end let go, and the link waited for */
	out_w(RC_SW_INIT_1, in_w(RC_SW_INIT_1) & ~SW_INIT_1_PERST);
	tk_dly_tsk(LINK_WAIT_MS);
	for ( t = 0; !rc_link_up() && t < LINK_WAIT_MS; t += LINK_POLL_MS ) {
		tk_dly_tsk(LINK_POLL_MS);
	}
	rc_say("far end out of reset", TRUE);
	if ( !rc_link_up() ) {
		return E_TMOUT;
	}

	out_w(RC_HARD_DEBUG, in_w(RC_HARD_DEBUG) | HARD_DEBUG_CLKREQ_EN);

	return E_OK;
}

/*
 * The root port carries the whole outbound window across, nothing
 * prefetchable, and forwards the cards' reads and writes of memory.
 */
LOCAL void rc_bridge_setup( UD bus, UD size )
{
	UD	limit = bus + size - 1;

	knl_pcie_cfg_out(0, 0, 0, BRIDGE_MEM_BASE,
			 (UW)( ( ( bus >> 16 ) & 0xfff0 ) | ( ( ( limit >> 16 ) & 0xfff0 ) << 16 ) ), 4);
	knl_pcie_cfg_out(0, 0, 0, BRIDGE_PREF_BASE, 0x0000FFF0U, 4);	/* base above limit: none */
	knl_pcie_cfg_out(0, 0, 0, BRIDGE_PREF_BASE_HI, 0, 4);
	knl_pcie_cfg_out(0, 0, 0, BRIDGE_PREF_LIMIT_HI, 0, 4);
	knl_pcie_cfg_out(0, 0, 0, PCI_COMMAND,
			 knl_pcie_cfg_in(0, 0, 0, PCI_COMMAND, 2) | PCI_CMD_MEMORY | PCI_CMD_MASTER, 2);
}

/*
 * Have the firmware load the VL805's program: the function's place on
 * the bus in the form of a configuration address.
 */
LOCAL void vl805_load( CONST T_PCIDEV *d )
{
	UW	addr = ( (UW)d->bus << 20 ) | ( (UW)d->dev << 15 ) | ( (UW)d->fn << 12 );
	UW	ans = 0, ver;
	ER	er;

	/* a VL805 that runs a program says its version in configuration space */
	ver = knl_pcie_cfg_in(d->bus, d->dev, d->fn, VL805_VERSION, 4);
	if ( ver != 0 && ver != 0xFFFFFFFFU ) {
		tm_printf((UB *)"TessronOS: pcie VL805 at %d:%d.%d runs its program (version %08x)\n",
			  d->bus, d->dev, d->fn, ver);
		return;
	}
	er = knl_fw_value(TAG_NOTIFY_XHCI_RESET, addr, &ans);
	tk_dly_tsk(10);
	tm_printf((UB *)"TessronOS: pcie VL805 at %d:%d.%d, firmware asked to load its program (%d), "
		  "version now %08x\n", d->bus, d->dev, d->fn, (INT)er,
		  knl_pcie_cfg_in(d->bus, d->dev, d->fn, VL805_VERSION, 4));
}

/* ---------------------------------------------------------------- start-up */

EXPORT INT knl_pcie_rc_init( void )
{
	UD	out_cpu = PCIE_OUTBOUND_CPU_PA, out_bus = PCIE_OUTBOUND_BUS_PA;
	UD	out_size = PCIE_OUTBOUND_SIZE;
	UD	in_bus = PCIE_INBOUND_BUS_PA, in_cpu = 0, in_size = PCIE_INBOUND_SIZE;
	UD	c, p, s;
	INT	n, i;
	ER	er;

	if ( !knl_dtb_okay(PCIE_DT_PATH) ) {
		tm_printf((UB *)"TessronOS: pcie not in the device tree, or disabled there; no USB behind it\n");
		return 0;
	}

	/* the windows as the device tree gives them */
	if ( knl_dtb_ranges(PCIE_DT_PATH, "ranges", 0, &c, &p, &s) >= E_OK && s != 0 ) {
		out_bus = c;
		out_cpu = p;
		out_size = s;
	}
	if ( knl_dtb_ranges(PCIE_DT_PATH, "dma-ranges", 0, &c, &p, &s) >= E_OK && s != 0 ) {
		in_bus = c;
		in_cpu = p;
		in_size = s;
	}

	/* the root port is not read here: until rc_start lets the bridge go it does not answer */
	er = rc_start(out_cpu, out_bus, out_size, in_bus - in_cpu, in_size);
	if ( er == E_NOEXS ) {
		tm_printf((UB *)"TessronOS: pcie no root complex at %08x; no USB behind it\n",
			  (UW)PCIE_PA);
		return 0;
	}
	if ( er < E_OK ) {
		tm_printf((UB *)"TessronOS: pcie link did not come up (%d, status %08x); "
			  "no USB behind it\n", (INT)er, in_w(RC_STATUS));
		return 0;
	}
	rc_up = TRUE;
	knl_pcie_cpu_offset = out_cpu - out_bus;
	knl_pcie_dma_offset = in_bus - in_cpu;
	tm_printf((UB *)"TessronOS: pcie link up, revision %08x; cards at %x%08x for PCIe %08x, "
		  "memory at PCIe %08x\n", in_w(RC_REVISION), (UW)( out_cpu >> 32 ), (UW)out_cpu,
		  (UW)out_bus, (UW)knl_pcie_dma_offset);

	rc_bridge_setup(out_bus, out_size);
	n = knl_pcie_scan(out_bus, out_size);
	{
		T_PCIDEV	d;

		for ( i = 0; ts_pcie_get(i, &d) >= E_OK; i++ ) {
			tm_printf((UB *)"TessronOS: pcie found %d:%d.%d %04x:%04x class %02x%02x%02x "
				  "window %08x\n", d.bus, d.dev, d.fn, d.vendor, d.device,
				  d.class_code, d.subclass, d.prog_if, (UW)d.bar[0]);
		}
	}

	/* the VL805 on the boards that keep its program for it */
	if ( knl_board.kind == BOARD_RPI4B || knl_board.kind == BOARD_RPI400 ) {
		T_PCIDEV	d;

		for ( i = 0; ts_pcie_get(i, &d) >= E_OK; i++ ) {
			if ( ( (UW)d.device << 16 | d.vendor ) == VL805_ID ) {
				vl805_load(&d);
			}
		}
	}
	return n;
}

#endif /* RPI4 */
