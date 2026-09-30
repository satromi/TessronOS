/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pcie_rc.c (rpi5)
 *	The BCM2712 PCI Express root complex: the x4 link the RP1 hangs on
 *	(design 4.7, 10.7).
 *
 *	A board can arrive here two ways. With `pciex4_reset=0` in
 *	config.txt the firmware leaves the link trained and its windows in
 *	place, and everything below - the header pins, the RJ45 socket, the
 *	USB sockets - is already reachable at RP1_WINDOW_PA. Without it the
 *	firmware puts the controller back into reset before the kernel runs
 *	and none of that answers. This file is what makes the second case
 *	work: reset released, link trained, the two windows set up, and the
 *	one endpoint given somewhere to live.
 *
 *	The windows are what makes an address mean something on either side
 *	of the bridge. The outbound window carries the processor's reads and
 *	writes of PCIE2_OUTBOUND_CPU_PA and upwards out onto the link as
 *	PCIE2_OUTBOUND_BUS_PA and upwards, which is how a peripheral window
 *	is reached at all. The inbound window carries a device's reads and
 *	writes of PCIE2_INBOUND_BUS_PA and upwards back into system memory
 *	at physical zero and upwards, which is the whole of what
 *	RP1_DMA_BUS_OFFSET means: it is the foot of this window, not a
 *	number chosen for its own sake, and the two are the same definition
 *	in sysdef.h so that they cannot drift apart.
 *
 *	WHAT IS ESTABLISHED HERE AND WHAT IS NOT
 *
 *	Established in this repository: the controller's base address and
 *	the length of its register block (design 4.2), the addresses both
 *	windows carry and their sizes (design 4.7), and the interrupt
 *	numbers (design 4.3). Those are what sysdef.h holds.
 *
 *	Not established anywhere in this repository: a single register
 *	offset inside that block, or the meaning of a single bit of one.
 *	The block is not a set of standard host bridge registers. Every
 *	offset and every bit named in the next section is therefore a
 *	proposal to be read against a board before it is believed, and the
 *	checklist carries a row for each group (items 31a to 31g). Nothing
 *	in this file writes to the controller unless CNF_RPI5_PCIE_RC is
 *	turned on, exactly because of that: a board that boots today on the
 *	firmware's own configuration is not disturbed by a wrong guess, and
 *	the switch is what a board bring-up turns on once the rows below it
 *	have been checked one at a time.
 *
 *	Configuration space is also not the flat window that
 *	kernel/sysman/pcie.c assumes. That file reads a register as one load
 *	from a base plus the bus, device and function packed above the
 *	offset, a megabyte to a bus. This controller shows one function at a
 *	time through a 4KB aperture, after its number has been written to an
 *	index register, so the generic walk cannot drive it as it stands: it
 *	would need its address calculation behind a hook the board fills in,
 *	and that hook belongs in that file rather than here. Until it has
 *	one, the short walk at the end of this file does the same job for
 *	this board - find what is there, give each window somewhere to live
 *	inside the outbound window, and let the function answer. Checklist
 *	item 31g.
 *
 *	NOT VERIFIED ON HARDWARE (docs/private/checklists/phase7.md items 31,
 *	31a to 31g).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysdepend.h"

/*
 * Whether this file may write to the controller. Off: the link is only
 * looked at and reported, and the board runs on whatever the firmware
 * left. On: the controller is brought up from reset when the link is
 * down, and the bus is walked.
 */
#ifndef CNF_RPI5_PCIE_RC
#define CNF_RPI5_PCIE_RC	0
#endif

/* ---------------------------------------------------------------- registers */

/*
 * None of the offsets or bits in this section is established by anything
 * in this repository; see the heading. They are grouped so that each
 * group can be checked on a board by itself.
 */
#define RC_REG(off)		(PCIE2_BASE + (off))

/* 31a: the root port's own configuration space is the foot of the block */
#define RC_CFG			RC_REG(0x0000)

/* 31b: one function of a bus below, through an aperture and an index */
#define RC_EXT_CFG_DATA		RC_REG(0x8000)
#define RC_EXT_CFG_INDEX	RC_REG(0x9000)

/* 31c: reset of the controller and of whatever is on the far end */
#define RC_SW_INIT_1		RC_REG(0x9210)
#define SW_INIT_1_PERST		0x00000001	/* the far end is held in reset */
#define SW_INIT_1_INIT		0x00000002	/* the controller is held in reset */

/* 31d: the analogue side, and whether the link has come up */
#define RC_HARD_DEBUG		RC_REG(0x4204)
#define HARD_DEBUG_SERDES_IDDQ	0x08000000	/* the analogue side is powered down */
#define RC_STATUS		RC_REG(0x4068)
#define STATUS_PHYLINKUP	0x00000010	/* the wires are talking */
#define STATUS_DL_ACTIVE	0x00000020	/* and packets are getting through */
#define RC_REVISION		RC_REG(0x406c)

/*
 * The bridge between the controller and the SoC's bus. A read the far
 * end does not answer comes back as an error on the processor's bus
 * (an asynchronous SError) unless the bridge is told to hand back a
 * fixed word instead; that word is all ones, the value every driver
 * already takes for "nothing there".
 */
#define RC_UBUS_CTRL		RC_REG(0x40a4)
#define UBUS_REPLY_ERR_DIS	0x00002000
#define UBUS_REPLY_DECERR_DIS	0x00080000
#define RC_AXI_READ_ERROR_DATA	RC_REG(0x4170)

/* 31e: the inbound window, as a base address register of the bridge itself */
#define RC_MISC_CTRL		RC_REG(0x4008)
#define MISC_CTRL_SCB0_SIZE_SH	27		/* log2 of the window size, less 15 */
#define MISC_CTRL_SCB0_SIZE_MSK	0xf8000000
#define RC_BAR2_CONFIG_LO	RC_REG(0x4034)
#define RC_BAR2_CONFIG_HI	RC_REG(0x4038)
#define BAR_CONFIG_SIZE_MSK	0x0000001f	/* log2 of the window size, less 15 */

/* 31f: the outbound window */
#define RC_MEM_WIN0_LO		RC_REG(0x400c)	/* what a processor address becomes */
#define RC_MEM_WIN0_HI		RC_REG(0x4010)
#define RC_MEM_WIN0_BASE_LIMIT	RC_REG(0x4070)	/* which processor addresses it takes */
#define RC_MEM_WIN0_BASE_HI	RC_REG(0x4080)
#define RC_MEM_WIN0_LIMIT_HI	RC_REG(0x4084)

/* Bridge registers, which no function that is not one has */
#define RC_BRIDGE_MEM_BASE	0x20		/* base and limit of what it carries across */
#define RC_BRIDGE_PREF_BASE	0x24		/* the same for prefetchable memory */

#define RC_LINK_SPIN		1000		/* milliseconds waited for the link */
#define RC_BAR_MAX		16		/* windows the walk can place */
#define RC_DEV_MAX		8		/* functions it reports */
#define RC_DEPTH_MAX		4		/* bridges below bridges the walk goes through */

/* ---------------------------------------------------------------- link */

/*
 * Whether the link carries packets. A controller held in reset reads as
 * all ones, which is not a link either.
 */
LOCAL BOOL rc_link_up( void )
{
	UW	st = in_w(RC_STATUS);

	if ( st == 0xFFFFFFFFU ) {
		return FALSE;
	}

	return ( (st & (STATUS_PHYLINKUP | STATUS_DL_ACTIVE))
		 == (STATUS_PHYLINKUP | STATUS_DL_ACTIVE) );
}

/* ---------------------------------------------------------------- config */

/*
 * Where one register of one function is read. The root port itself is at
 * the foot of the block; everything on a bus below is seen through the
 * aperture, so its number goes into the index register first and the
 * write has to have landed before the aperture is touched.
 */
LOCAL UBINT rc_cfg_addr( UINT bus, UINT dev, UINT fn, UINT off )
{
	if ( bus == 0 ) {
		if ( dev != 0 || fn != 0 ) {
			return 0;	/* this root complex has the one port */
		}
		return RC_CFG + (UBINT)(off & 0xfff);
	}
	out_w(RC_EXT_CFG_INDEX, (UW)((bus << 20) | (dev << 15) | (fn << 12)));
	(void)in_w(RC_EXT_CFG_INDEX);

	return RC_EXT_CFG_DATA + (UBINT)(off & 0xfff);
}

LOCAL UW rc_cfg_read( UINT bus, UINT dev, UINT fn, UINT off, INT width )
{
	UBINT	a = rc_cfg_addr(bus, dev, fn, off);

	if ( a == 0 ) {
		return 0xFFFFFFFFU;	/* nothing answers there */
	}
	switch ( width ) {
	case 1:		return (UW)(*(volatile UB *)a);
	case 2:		return (UW)(*(volatile UH *)a);
	default:	return *(volatile UW *)a;
	}
}

LOCAL void rc_cfg_write( UINT bus, UINT dev, UINT fn, UINT off, UW val, INT width )
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

/* Where the RP1's peripherals and its view of memory are (sysdef.h) */
EXPORT __UINT64_TYPE__	knl_rp1_window_pa  = PCIE2_OUTBOUND_CPU_PA;
EXPORT __UINT64_TYPE__	knl_rp1_dma_offset = PCIE2_INBOUND_BUS_PA;

#define RP1_ID			0x00011de4	/* device 0001, vendor 1de4 */
#define RP1_PERI_BAR		1		/* the peripherals are behind BAR1 */
#define PCI_CMD_MEM_MASTER	0x0006		/* answers memory cycles, masters the bus */

/*
 * Find the RP1 the way the firmware left it and point RP1_WINDOW_PA and
 * RP1_DMA_BUS_OFFSET at it.
 *
 * The outbound window says which processor addresses become which PCIe
 * addresses; the inbound window which PCIe address system memory starts
 * at. The root port may have been left without bus numbers, and then
 * nothing below it answers configuration reads: it is given bus 1 below
 * it. The RP1's BAR1 is read; one left unplaced is put at the foot of
 * the port's memory window. The RP1 is told to answer memory cycles and
 * to master the bus (its USB and Ethernet reach memory by themselves).
 */
LOCAL void rc_find_rp1( void )
{
	UD	out_cpu, out_pci, in_pci, bar;
	UW	bl, bus, id, lo, hi, cmd, mem;

	bl = in_w(RC_MEM_WIN0_BASE_LIMIT);
	out_cpu = ((UD)in_w(RC_MEM_WIN0_BASE_HI) << 32) | ((UD)((bl >> 4) & 0xfff) << 20);
	out_pci = ((UD)in_w(RC_MEM_WIN0_HI) << 32) | in_w(RC_MEM_WIN0_LO);
	/* the low five bits of the inbound window are its size, not its address */
	in_pci  = ((UD)in_w(RC_BAR2_CONFIG_HI) << 32)
		| (in_w(RC_BAR2_CONFIG_LO) & ~(UW)BAR_CONFIG_SIZE_MSK);

	bus = rc_cfg_read(0, 0, 0, 0x18, 4);
	if ( ((bus >> 8) & 0xff) == 0 ) {
		rc_cfg_write(0, 0, 0, 0x18, (bus & 0xff000000U) | 0x00010100U, 4);
		tm_printf((UB *)"TessronOS: pcie2 port given bus 1 below it (was %08x)\n", bus);
	}
	id = rc_cfg_read(1, 0, 0, 0x00, 4);
	if ( id != RP1_ID ) {
		tm_printf((UB *)"TessronOS: pcie2 no RP1 below the port (id %08x)\n", id);
		return;
	}

	lo = rc_cfg_read(1, 0, 0, 0x10 + RP1_PERI_BAR * 4, 4);
	hi = ( (lo & 0x6) == 0x4 ) ? rc_cfg_read(1, 0, 0, 0x14 + RP1_PERI_BAR * 4, 4) : 0;
	bar = ((UD)hi << 32) | (lo & 0xFFFFFFF0U);
	if ( bar == 0 ) {
		/* not placed: at the foot of what the port carries across */
		mem = rc_cfg_read(0, 0, 0, 0x20, 4);
		bar = (UD)(mem & 0xfff0) << 16;
		rc_cfg_write(1, 0, 0, 0x10 + RP1_PERI_BAR * 4, (UW)bar | (lo & 0xf), 4);
		if ( (lo & 0x6) == 0x4 ) {
			rc_cfg_write(1, 0, 0, 0x14 + RP1_PERI_BAR * 4, (UW)(bar >> 32), 4);
		}
		tm_printf((UB *)"TessronOS: pcie2 RP1 BAR1 placed at %08x%08x\n",
			  (UW)(bar >> 32), (UW)bar);
	}
	cmd = rc_cfg_read(1, 0, 0, 0x04, 4);
	if ( (cmd & PCI_CMD_MEM_MASTER) != PCI_CMD_MEM_MASTER ) {
		rc_cfg_write(1, 0, 0, 0x04, (cmd & 0xffff) | PCI_CMD_MEM_MASTER, 4);
	}

	knl_rp1_window_pa  = out_cpu + ( bar - out_pci );
	knl_rp1_dma_offset = in_pci;
	tm_printf((UB *)"TessronOS: rp1 BAR1 %08x%08x, peripherals at %08x%08x, memory at bus %08x%08x\n",
		  (UW)(bar >> 32), (UW)bar,
		  (UW)(knl_rp1_window_pa >> 32), (UW)knl_rp1_window_pa,
		  (UW)(knl_rp1_dma_offset >> 32), (UW)knl_rp1_dma_offset);
}

/*
 * What the firmware left: the controller's windows and the first
 * function below it (the RP1) as its configuration space describes it.
 * The processor reaches the RP1's peripherals at RP1_WINDOW_PA only when
 * the outbound window turns that address into the PCIe address the RP1's
 * second window (BAR1) was placed at, and the RP1 answers memory cycles.
 */
LOCAL void rc_dump( void )
{
	UW	id, cmd;
	INT	i;

	tm_printf((UB *)"TessronOS: pcie2 rev %08x misc %08x ubus %08x inbound %08x %08x\n",
		  in_w(RC_REVISION), in_w(RC_MISC_CTRL), in_w(RC_REG(0x40a4)),
		  in_w(RC_BAR2_CONFIG_LO), in_w(RC_BAR2_CONFIG_HI));
	tm_printf((UB *)"TessronOS: pcie2 outbound to %08x%08x, from %08x (hi %08x %08x)\n",
		  in_w(RC_MEM_WIN0_HI), in_w(RC_MEM_WIN0_LO), in_w(RC_MEM_WIN0_BASE_LIMIT),
		  in_w(RC_MEM_WIN0_BASE_HI), in_w(RC_MEM_WIN0_LIMIT_HI));
	tm_printf((UB *)"TessronOS: pcie2 port id %08x cmd %08x buses %08x mem %08x\n",
		  rc_cfg_read(0, 0, 0, 0x00, 4), rc_cfg_read(0, 0, 0, 0x04, 4),
		  rc_cfg_read(0, 0, 0, 0x18, 4), rc_cfg_read(0, 0, 0, RC_BRIDGE_MEM_BASE, 4));
	id  = rc_cfg_read(1, 0, 0, 0x00, 4);
	cmd = rc_cfg_read(1, 0, 0, 0x04, 4);
	tm_printf((UB *)"TessronOS: pcie2 1:0.0 id %08x cmd %08x class %08x\n",
		  id, cmd, rc_cfg_read(1, 0, 0, 0x08, 4));
	for ( i = 0; i < 6; i += 2 ) {
		tm_printf((UB *)"TessronOS: pcie2 1:0.0 bar%d %08x bar%d %08x\n",
			  i, rc_cfg_read(1, 0, 0, 0x10 + i * 4, 4),
			  i + 1, rc_cfg_read(1, 0, 0, 0x14 + i * 4, 4));
	}
	tm_printf((UB *)"TessronOS: rp1 window reads %08x at %08x%08x\n",
		  in_w(DEV_BASE(RP1_WINDOW_PA)),
		  (UW)((UD)RP1_WINDOW_PA >> 32), (UW)RP1_WINDOW_PA);
}

#if CNF_RPI5_PCIE_RC

typedef struct {
	UB	bus, dev, fn;
	UB	bar;			/* which base address register it is */
	BOOL	is64;
	UD	size;
} RC_BAR;

LOCAL RC_BAR	rc_bar[RC_BAR_MAX];
LOCAL INT	rc_nbar = 0;
LOCAL UD	mmio_next = 0;		/* the next PCIe address to hand out */
LOCAL UD	mmio_end = 0;
LOCAL INT	rc_ndev = 0;
LOCAL UB	bus_next = 0;		/* the last bus number handed to a bridge */

/* ---------------------------------------------------------------- reset */

/*
 * Release the controller and the far end from reset and wait for the
 * link to train. The far end is held in reset until the controller
 * itself is out of it and its analogue side is powered, otherwise the
 * endpoint starts talking to something that is not listening.
 */
LOCAL ER rc_link_start( void )
{
	INT	i;

	out_w(RC_SW_INIT_1, SW_INIT_1_INIT | SW_INIT_1_PERST);
	tk_dly_tsk(1);

	out_w(RC_SW_INIT_1, SW_INIT_1_PERST);		/* the controller runs */
	tk_dly_tsk(1);

	out_w(RC_HARD_DEBUG, in_w(RC_HARD_DEBUG) & ~HARD_DEBUG_SERDES_IDDQ);
	tk_dly_tsk(1);

	out_w(RC_SW_INIT_1, 0);				/* and so does the far end */

	for ( i = 0; i < RC_LINK_SPIN; i++ ) {
		if ( rc_link_up() ) {
			return E_OK;
		}
		tk_dly_tsk(1);
	}

	return E_TMOUT;
}

/* ---------------------------------------------------------------- windows */

/*
 * The log2 of a size, as the window registers want it: a power of two
 * counted from 32KB, so 32KB is 1 and 64GB is 22.
 */
LOCAL UW win_size_code( UD size )
{
	UW	code = 0;
	UD	s = 0x8000;

	while ( s < size && code < 31 ) {
		s <<= 1;
		code++;
	}

	return code + 1;
}

/*
 * The inbound window: where a device below the link finds system memory.
 * It is a base address register of the bridge itself, holding the PCIe
 * address the window starts at and how far it runs; what it reaches is
 * physical address zero upwards.
 */
LOCAL void rc_inbound_setup( void )
{
	UW	code = win_size_code(PCIE2_INBOUND_SIZE);

	out_w(RC_BAR2_CONFIG_LO,
	      (UW)((UD)PCIE2_INBOUND_BUS_PA & 0xFFFFFFF0U) | (code & BAR_CONFIG_SIZE_MSK));
	out_w(RC_BAR2_CONFIG_HI, (UW)((UD)PCIE2_INBOUND_BUS_PA >> 32));

	out_w(RC_MISC_CTRL, (in_w(RC_MISC_CTRL) & ~MISC_CTRL_SCB0_SIZE_MSK)
			  | ((code << MISC_CTRL_SCB0_SIZE_SH) & MISC_CTRL_SCB0_SIZE_MSK));
}

/*
 * The outbound window: which processor addresses go out onto the link
 * and what they become there. The base and the limit are the processor
 * side, in units of a megabyte; the translation is the PCIe address the
 * foot of the window becomes.
 */
LOCAL void rc_outbound_setup( void )
{
	UD	base = PCIE2_OUTBOUND_CPU_PA;
	UD	limit = PCIE2_OUTBOUND_CPU_PA + PCIE2_OUTBOUND_SIZE - 1;

	out_w(RC_MEM_WIN0_LO, (UW)((UD)PCIE2_OUTBOUND_BUS_PA & 0xFFFFFFFFU));
	out_w(RC_MEM_WIN0_HI, (UW)((UD)PCIE2_OUTBOUND_BUS_PA >> 32));

	out_w(RC_MEM_WIN0_BASE_LIMIT,
	      (UW)(((base >> 20) & 0xfff) << 4) | (UW)((((limit >> 20) & 0xfff) << 4) << 16));
	out_w(RC_MEM_WIN0_BASE_HI, (UW)(base >> 32));
	out_w(RC_MEM_WIN0_LIMIT_HI, (UW)(limit >> 32));
}

/* ---------------------------------------------------------------- walking */

/*
 * Give a window somewhere to live. It has to start at a whole multiple
 * of its own size, so the running mark is pushed up to one first. The
 * answer comes back through the pointer because the foot of the outbound
 * window is PCIe address zero, which is an address like any other here.
 */
LOCAL BOOL mmio_take( UD size, UD *p_addr )
{
	UD	a;

	if ( size == 0 ) {
		return FALSE;
	}
	a = (mmio_next + size - 1) & ~(size - 1);
	if ( a + size > mmio_end || a < mmio_next ) {
		return FALSE;			/* the window has run out of room */
	}
	mmio_next = a + size;
	*p_addr = a;

	return TRUE;
}

/*
 * Find out how big one window wants to be. A register answers by keeping
 * the bits it does not care about clear after all ones are written to
 * it. Nothing is placed yet: the sizes are collected first so that the
 * largest can be placed first, which is what puts the RP1's four
 * megabytes of peripheral space at the foot of the outbound window and
 * therefore at RP1_WINDOW_PA, where the drivers above expect it.
 */
LOCAL INT rc_bar_size( UINT bus, UINT dev, UINT fn, INT i )
{
	UINT	off = PCI_BAR0 + (UINT)i * 4;
	UW	orig, mask;
	UD	size;
	BOOL	is64 = FALSE;

	orig = rc_cfg_read(bus, dev, fn, off, 4);
	rc_cfg_write(bus, dev, fn, off, 0xFFFFFFFFU, 4);
	mask = rc_cfg_read(bus, dev, fn, off, 4);
	rc_cfg_write(bus, dev, fn, off, orig, 4);

	if ( mask == 0 || mask == 0xFFFFFFFFU || (mask & PCI_BAR_IO) != 0 ) {
		return i + 1;			/* not there, or not memory */
	}
	is64 = ( (mask & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64 );
	size = (UD)(~(mask & ~0xFU) + 1) & 0xFFFFFFFFU;

	if ( is64 ) {
		UW	hi_orig, hi_mask;

		hi_orig = rc_cfg_read(bus, dev, fn, off + 4, 4);
		rc_cfg_write(bus, dev, fn, off + 4, 0xFFFFFFFFU, 4);
		hi_mask = rc_cfg_read(bus, dev, fn, off + 4, 4);
		rc_cfg_write(bus, dev, fn, off + 4, hi_orig, 4);
		if ( hi_mask != 0xFFFFFFFFU && hi_mask != 0 ) {
			size |= ((UD)~hi_mask + 1) << 32;
		}
	}

	if ( size != 0 && rc_nbar < RC_BAR_MAX ) {
		RC_BAR	*b = &rc_bar[rc_nbar++];

		b->bus  = (UB)bus;
		b->dev  = (UB)dev;
		b->fn   = (UB)fn;
		b->bar  = (UB)i;
		b->is64 = is64;
		b->size = size;
	}

	return ( is64 ) ? i + 2 : i + 1;
}

/*
 * Place every window that was found, the largest first, so that no space
 * is lost to alignment and the biggest one lands at the foot of the
 * outbound window.
 */
LOCAL void rc_bar_place( void )
{
	INT	shift, i;

	for ( shift = 63; shift >= 0; shift-- ) {
		for ( i = 0; i < rc_nbar; i++ ) {
			RC_BAR	*b = &rc_bar[i];
			UINT	off;
			UD	addr;

			if ( b->size != (1ULL << shift) ) {
				continue;
			}
			if ( !mmio_take(b->size, &addr) ) {
				tm_printf((UB *)"TessronOS: pcie %d:%d.%d bar%d does not fit\n",
					  b->bus, b->dev, b->fn, b->bar);
				continue;
			}
			off = PCI_BAR0 + (UINT)b->bar * 4;
			rc_cfg_write(b->bus, b->dev, b->fn, off, (UW)addr, 4);
			if ( b->is64 ) {
				rc_cfg_write(b->bus, b->dev, b->fn, off + 4,
					     (UW)(addr >> 32), 4);
			}
			tm_printf((UB *)"TessronOS: pcie %d:%d.%d bar%d %08x%08x+%08x\n",
				  b->bus, b->dev, b->fn, b->bar,
				  (UW)(addr >> 32), (UW)addr, (UW)b->size);
		}
	}
}

/*
 * Let a function answer on its windows and fetch memory for itself.
 */
LOCAL void rc_enable( UINT bus, UINT dev, UINT fn )
{
	UW	cmd = rc_cfg_read(bus, dev, fn, PCI_COMMAND, 2);

	cmd |= PCI_CMD_MEMORY | PCI_CMD_MASTER;
	rc_cfg_write(bus, dev, fn, PCI_COMMAND, cmd, 2);
}

/*
 * Walk one bus. The root port is a bridge: the bus behind it is numbered
 * and told which addresses to carry across before anything on it is
 * looked at, or nothing on that bus answers. The numbers are handed out
 * in the order the bridges are met, so two of them never claim the same
 * one, and the depth is bounded: a set of offsets that turns out to be
 * wrong can otherwise make the same bridge answer on every bus.
 */
LOCAL void rc_bus_walk( UINT bus, INT depth )
{
	UINT	dev, fn;

	if ( depth > RC_DEPTH_MAX ) {
		return;
	}
	for ( dev = 0; dev < 32; dev++ ) {
		for ( fn = 0; fn < 8; fn++ ) {
			UW	id = rc_cfg_read(bus, dev, fn, PCI_VENDOR_ID, 4);
			UW	cls;
			UB	hdr;
			INT	i, next;

			if ( (id & 0xffff) == 0xffff || id == 0 ) {
				if ( fn == 0 ) break;
				continue;
			}
			hdr = (UB)rc_cfg_read(bus, dev, fn, PCI_HEADER_TYPE, 1);
			cls = rc_cfg_read(bus, dev, fn, PCI_REVISION, 4);

			if ( rc_ndev++ < RC_DEV_MAX ) {
				tm_printf((UB *)"TessronOS: pcie %d:%d.%d %04x:%04x class %02x.%02x.%02x\n",
					  bus, dev, fn, id & 0xffff, id >> 16,
					  (cls >> 24) & 0xff, (cls >> 16) & 0xff,
					  (cls >> 8) & 0xff);
			}

			if ( (hdr & PCI_HDR_TYPE_MASK) == PCI_HDR_BRIDGE ) {
				UD	base = PCIE2_OUTBOUND_BUS_PA;
				UD	limit = PCIE2_OUTBOUND_BUS_PA
					      + PCIE2_OUTBOUND_SIZE - 1;
				UINT	sub = ++bus_next;

				rc_cfg_write(bus, dev, fn, PCI_PRIMARY_BUS, bus, 1);
				rc_cfg_write(bus, dev, fn, PCI_SECONDARY_BUS, sub, 1);
				rc_cfg_write(bus, dev, fn, PCI_SUBORDINATE_BUS, 0xff, 1);

				/* which addresses the bridge carries across */
				rc_cfg_write(bus, dev, fn, RC_BRIDGE_MEM_BASE,
					     (UW)(((base >> 16) & 0xfff0)
						| (((limit >> 16) & 0xfff0) << 16)), 4);
				/* nothing prefetchable: a base above the limit */
				rc_cfg_write(bus, dev, fn, RC_BRIDGE_PREF_BASE,
					     0x0000FFF0U, 4);

				rc_enable(bus, dev, fn);
				rc_bus_walk(sub, depth + 1);
				rc_cfg_write(bus, dev, fn, PCI_SUBORDINATE_BUS,
					     (UW)bus_next, 1);
			} else {
				for ( i = 0; i < PCI_MAX_BAR; i = next ) {
					next = rc_bar_size(bus, dev, fn, i);
				}
			}

			if ( fn == 0 && (hdr & PCI_HDR_MULTI) == 0 ) {
				break;		/* it has only the one */
			}
		}
	}
}

#endif /* CNF_RPI5_PCIE_RC */

/* ---------------------------------------------------------------- start-up */

/*
 * Bring the link up if it is down and this file is allowed to write to
 * the controller, then let what is on it be found. Answers how many
 * functions were found, 0 when the link is down and nothing can be, or
 * an error.
 */
EXPORT INT knl_pcie_rc_init( void )
{
#if CNF_RPI5_PCIE_RC
	ER	er;

	if ( !rc_link_up() ) {
		er = rc_link_start();
		if ( er < E_OK ) {
			tm_printf((UB *)"TessronOS: pcie2 link did not come up\n");
			return 0;
		}
		rc_inbound_setup();
		rc_outbound_setup();
	}
	tm_printf((UB *)"TessronOS: pcie2 link up, revision %08x\n",
		  in_w(RC_REVISION));

	mmio_next = PCIE2_OUTBOUND_BUS_PA;
	mmio_end  = PCIE2_OUTBOUND_BUS_PA + PCIE2_OUTBOUND_SIZE;
	rc_nbar   = 0;
	rc_ndev   = 0;

	bus_next  = 0;

	rc_bus_walk(0, 0);
	rc_bar_place();

	/*
	 * Nothing may answer on a window until it has one, so this comes
	 * after the placing. A function with several windows is named
	 * several times here, which costs a write and changes nothing.
	 */
	{
		INT	i;

		for ( i = 0; i < rc_nbar; i++ ) {
			rc_enable(rc_bar[i].bus, rc_bar[i].dev, rc_bar[i].fn);
		}
	}

	return rc_ndev;
#else
	/*
	 * Left as the firmware set it. The link is only looked at, so that
	 * a board whose config.txt is missing `pciex4_reset=0` says so here
	 * rather than through a driver further on finding all ones.
	 */
	if ( !rc_link_up() ) {
		tm_printf((UB *)"TessronOS: pcie2 link down (pciex4_reset=0 not set?)\n");
		return 0;
	}
	tm_printf((UB *)"TessronOS: pcie2 link up (from the firmware)\n");
	out_w(RC_UBUS_CTRL, in_w(RC_UBUS_CTRL) | UBUS_REPLY_ERR_DIS | UBUS_REPLY_DECERR_DIS);
	out_w(RC_AXI_READ_ERROR_DATA, 0xFFFFFFFF);
	rc_find_rp1();
	rc_dump();

	return 1;
#endif
}

#endif /* RPI5 */
