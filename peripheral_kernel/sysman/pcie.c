/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pcie.c
 *	PCI Express host bridge (design 4.6, 10.4).
 *
 *	Configuration space is the enhanced kind: a megabyte per bus, so the
 *	address of a register is the base of the space plus the bus, device,
 *	function and offset packed into one number. It is ordinary device
 *	memory, which is why no port instructions appear anywhere here.
 *
 *	A kernel that starts without firmware finds cards whose base address
 *	registers hold nothing: no one has said where their windows live. So
 *	the walk hands them out, taking each in turn from the range the board
 *	set aside for the bus. A window is given an address that is a whole
 *	multiple of its own size, which is what the registers require.
 *
 *	Bridges are walked through: the buses behind one are numbered as they
 *	are found and the numbers written into the bridge, so a card two
 *	levels down is reached like any other.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysdepend.h"

#if defined(PCIE_ECAM_PA) || defined(PCIE_CFG_BOARD)

#define PCI_MAX_DEV	32		/* functions the table holds */

LOCAL T_PCIDEV	pci_dev[PCI_MAX_DEV];
LOCAL INT	pci_ndev = 0;
LOCAL UD	mmio_next = 0;		/* the next address to hand out */
LOCAL UD	mmio_end = 0;
LOCAL UB	bus_next = 0;		/* the next bus number to give a bridge */

/* ---------------------------------------------------------------- config */

#ifdef PCIE_ECAM_PA
/*
 * The address of one register where the whole space is one flat window:
 * bus, device and function are packed above the offset.
 */
LOCAL UBINT cfg_addr( UINT bus, UINT dev, UINT fn, UINT off )
{
	return PCIE_ECAM_BASE + ((UBINT)bus << 20) + ((UBINT)dev << 15)
			      + ((UBINT)fn << 12) + (UBINT)(off & 0xfff);
}
#endif

/*
 * A board whose configuration space is not one flat window answers
 * through its own pair instead: some controllers show one function at a
 * time through a small opening, after being told which. Everything
 * above this line works the same either way.
 */
EXPORT UW ts_pcie_cfg_read( UINT bus, UINT dev, UINT fn, UINT off, INT width )
{
#ifdef PCIE_CFG_BOARD
	return knl_pcie_cfg_in(bus, dev, fn, off, width);
#else
	UBINT	a = cfg_addr(bus, dev, fn, off);

	switch ( width ) {
	case 1:		return (UW)(*(volatile UB *)a);
	case 2:		return (UW)(*(volatile UH *)a);
	default:	return *(volatile UW *)a;
	}
#endif
}

EXPORT void ts_pcie_cfg_write( UINT bus, UINT dev, UINT fn, UINT off,
			       UW val, INT width )
{
#ifdef PCIE_CFG_BOARD
	knl_pcie_cfg_out(bus, dev, fn, off, val, width);
#else
	UBINT	a = cfg_addr(bus, dev, fn, off);

	switch ( width ) {
	case 1:		*(volatile UB *)a = (UB)val;  break;
	case 2:		*(volatile UH *)a = (UH)val;  break;
	default:	*(volatile UW *)a = val;      break;
	}
	Asm("dsb sy" ::: "memory");
#endif
}

/* ---------------------------------------------------------------- windows */

/*
 * Give a window an address. It has to start at a whole multiple of its
 * own size, so the running mark is pushed up to one first.
 */
LOCAL UD mmio_take( UD size )
{
	UD	a = mmio_next;

	if ( size == 0 ) {
		return 0;
	}
	a = (a + size - 1) & ~(size - 1);
	if ( a + size > mmio_end ) {
		return 0;			/* the bus has run out of room */
	}
	mmio_next = a + size;

	return a;
}

/*
 * Find out how big a window is. Nothing is placed yet: where each one
 * goes is decided once every size is known, because a window has to
 * start at a whole multiple of its own size, and placing the large ones
 * first leaves no gap that only a small one could have filled.
 */
LOCAL void bar_size( T_PCIDEV *d, INT i, INT *p_next )
{
	UINT	off = PCI_BAR0 + (UINT)i * 4;
	UW	orig, mask, cmd;
	UD	size;
	BOOL	is64 = FALSE;

	*p_next = i + 1;

	cmd = ts_pcie_cfg_read(d->bus, d->dev, d->fn, PCI_COMMAND, 2);
	ts_pcie_cfg_write(d->bus, d->dev, d->fn, PCI_COMMAND,
			  cmd & ~(PCI_CMD_IO | PCI_CMD_MEMORY), 2);

	orig = ts_pcie_cfg_read(d->bus, d->dev, d->fn, off, 4);
	ts_pcie_cfg_write(d->bus, d->dev, d->fn, off, 0xFFFFFFFFU, 4);
	mask = ts_pcie_cfg_read(d->bus, d->dev, d->fn, off, 4);
	ts_pcie_cfg_write(d->bus, d->dev, d->fn, off, orig, 4);

	if ( mask == 0 || mask == 0xFFFFFFFFU ) {
		ts_pcie_cfg_write(d->bus, d->dev, d->fn, PCI_COMMAND, cmd, 2);
		return;				/* this one is not there */
	}

	if ( (mask & PCI_BAR_IO) != 0 ) {
		d->bar_is_io[i] = 1;
		size = (UD)(~(mask & ~0x3U) + 1) & 0xffffU;
	} else {
		is64 = ( (mask & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64 );
		size = (UD)(~(mask & ~0xFU) + 1) & 0xFFFFFFFFU;
		if ( is64 ) {
			UW	hi_orig, hi_mask;

			hi_orig = ts_pcie_cfg_read(d->bus, d->dev, d->fn, off + 4, 4);
			ts_pcie_cfg_write(d->bus, d->dev, d->fn, off + 4, 0xFFFFFFFFU, 4);
			hi_mask = ts_pcie_cfg_read(d->bus, d->dev, d->fn, off + 4, 4);
			ts_pcie_cfg_write(d->bus, d->dev, d->fn, off + 4, hi_orig, 4);
			if ( hi_mask != 0xFFFFFFFFU && hi_mask != 0 ) {
				size |= ((UD)~hi_mask + 1) << 32;
			}
			*p_next = i + 2;	/* it takes two registers */
			d->bar_is_64[i] = 1;
		}
	}
	d->bar_size[i] = size;

	ts_pcie_cfg_write(d->bus, d->dev, d->fn, PCI_COMMAND, cmd, 2);
}

/*
 * Hand every window an address, the largest first. Only what lies below
 * 4GB is given out: above it is outside the part of the machine the
 * kernel maps, and every card that matters here asks for less.
 */
LOCAL void bars_place( void )
{
	T_PCIDEV	*d;
	UD		best, addr;
	INT		i, k, bd, bb;
	UINT		off;

	for (;;) {
		best = 0;
		bd = -1;
		bb = -1;
		for ( i = 0; i < pci_ndev; i++ ) {
			for ( k = 0; k < PCI_MAX_BAR; k++ ) {
				if ( pci_dev[i].bar_size[k] == 0
				  || pci_dev[i].bar[k] != 0
				  || pci_dev[i].bar_is_io[k] != 0 ) {
					continue;
				}
				if ( pci_dev[i].bar_size[k] > best ) {
					best = pci_dev[i].bar_size[k];
					bd = i;
					bb = k;
				}
			}
		}
		if ( bd < 0 ) {
			break;			/* every one of them has a place */
		}
		d = &pci_dev[bd];
		addr = mmio_take(best);
		if ( addr == 0 ) {
			d->bar_size[bb] = 0;	/* no room: it stays unusable */
			continue;
		}
		off = PCI_BAR0 + (UINT)bb * 4;
		ts_pcie_cfg_write(d->bus, d->dev, d->fn, off, (UW)addr, 4);
		if ( d->bar_is_64[bb] != 0 ) {
			ts_pcie_cfg_write(d->bus, d->dev, d->fn, off + 4,
					  (UW)(addr >> 32), 4);
		}
		d->bar[bb] = addr;
	}
}

/* ---------------------------------------------------------------- walking */

LOCAL void bus_scan( UINT bus );

/*
 * Take in one function: what it is, where its windows are, and which
 * line it raises.
 */
LOCAL void func_add( UINT bus, UINT dev, UINT fn, UW id )
{
	T_PCIDEV	*d;
	UW		cls;
	INT		i, next;

	if ( pci_ndev >= PCI_MAX_DEV ) {
		return;
	}
	d = &pci_dev[pci_ndev];
	knl_memset(d, 0, sizeof(*d));

	d->vendor = (UH)(id & 0xffff);
	d->device = (UH)(id >> 16);
	d->bus = (UB)bus;
	d->dev = (UB)dev;
	d->fn  = (UB)fn;

	cls = ts_pcie_cfg_read(bus, dev, fn, PCI_REVISION, 4);
	d->prog_if    = (UB)(cls >> 8);
	d->subclass   = (UB)(cls >> 16);
	d->class_code = (UB)(cls >> 24);
	d->header = (UB)(ts_pcie_cfg_read(bus, dev, fn, PCI_HEADER_TYPE, 1)
			 & PCI_HDR_TYPE_MASK);

	if ( d->header == PCI_HDR_BRIDGE ) {
		UINT	sub = ++bus_next;

		/* the bus behind it is numbered before it is walked */
		ts_pcie_cfg_write(bus, dev, fn, PCI_PRIMARY_BUS, (UW)bus, 1);
		ts_pcie_cfg_write(bus, dev, fn, PCI_SECONDARY_BUS, (UW)sub, 1);
		ts_pcie_cfg_write(bus, dev, fn, PCI_SUBORDINATE_BUS, 0xff, 1);
		pci_ndev++;
		bus_scan(sub);
		ts_pcie_cfg_write(bus, dev, fn, PCI_SUBORDINATE_BUS,
				  (UW)bus_next, 1);
		return;
	}

	for ( i = 0; i < PCI_MAX_BAR; i = next ) {
		bar_size(d, i, &next);
	}

	d->int_pin = (UB)ts_pcie_cfg_read(bus, dev, fn, PCI_INT_PIN, 1);
#ifdef PCIE_INTA_SPI
	if ( d->int_pin >= 1 && d->int_pin <= 4 ) {
		/*
		 * The four lines of a slot are wired one place along for
		 * each slot, so that four cards in a row do not all end up
		 * on the same one.
		 */
		d->intno = 32 + PCIE_INTA_SPI
			 + ((dev + (UINT)d->int_pin - 1) % 4);
	}
#endif

	pci_ndev++;
}

LOCAL void bus_scan( UINT bus )
{
	UINT	dev, fn;
	UW	id;
	UB	hdr;

	for ( dev = 0; dev < 32; dev++ ) {
		for ( fn = 0; fn < 8; fn++ ) {
			id = ts_pcie_cfg_read(bus, dev, fn, PCI_VENDOR_ID, 4);
			if ( (id & 0xffff) == 0xffff || id == 0 ) {
				if ( fn == 0 ) {
					break;	/* nothing in this slot at all */
				}
				continue;
			}
			func_add(bus, dev, fn, id);

			if ( fn == 0 ) {
				hdr = (UB)ts_pcie_cfg_read(bus, dev, 0,
							   PCI_HEADER_TYPE, 1);
				if ( (hdr & PCI_HDR_MULTI) == 0 ) {
					break;	/* it has only the one */
				}
			}
		}
	}
}

/* ---------------------------------------------------------------- interface */

EXPORT INT ts_pcie_count( void )
{
	return pci_ndev;
}

EXPORT ER ts_pcie_get( INT index, T_PCIDEV *dev )
{
	if ( dev == NULL || index < 0 || index >= pci_ndev ) {
		return E_PAR;
	}
	*dev = pci_dev[index];

	return E_OK;
}

EXPORT ER ts_pcie_find( UINT class_code, UINT subclass, UINT prog_if,
			INT index, T_PCIDEV *dev )
{
	INT	i, k = 0;

	if ( dev == NULL || index < 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < pci_ndev; i++ ) {
		if ( pci_dev[i].class_code != (UB)class_code ) {
			continue;
		}
		if ( subclass != 0xff && pci_dev[i].subclass != (UB)subclass ) {
			continue;
		}
		if ( prog_if != 0xff && pci_dev[i].prog_if != (UB)prog_if ) {
			continue;
		}
		if ( k++ == index ) {
			*dev = pci_dev[i];
			return E_OK;
		}
	}

	return E_NOEXS;
}

/*
 * Let it answer on its windows and fetch memory for itself. The line is
 * left on: firmware that meant to use messages can leave it switched
 * off, and then nothing the card raises ever arrives.
 */
EXPORT ER ts_pcie_enable( CONST T_PCIDEV *dev )
{
	UW	cmd;

	if ( dev == NULL ) {
		return E_PAR;
	}
	cmd = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_COMMAND, 2);
	cmd |= PCI_CMD_MEMORY | PCI_CMD_MASTER;
	cmd &= ~PCI_CMD_INTX_OFF;
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, PCI_COMMAND, cmd, 2);

	/* a cache line length of zero stops some cards from burst reading */
	if ( ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_CACHE_LINE, 1) == 0 ) {
		ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, PCI_CACHE_LINE,
				  64 / 4, 1);
	}

	return E_OK;
}

EXPORT UBINT ts_pcie_bar_base( CONST T_PCIDEV *dev, INT bar )
{
	if ( dev == NULL || bar < 0 || bar >= PCI_MAX_BAR ) {
		return 0;
	}
	if ( dev->bar[bar] == 0 || dev->bar_is_io[bar] != 0 ) {
		return 0;
	}

	return DEV_BASE(dev->bar[bar]);
}

EXPORT UINT ts_pcie_cap_find( CONST T_PCIDEV *dev, UINT cap_id )
{
	UINT	off;
	UW	sts;
	INT	guard;

	if ( dev == NULL ) {
		return 0;
	}
	sts = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_STATUS, 2);
	if ( (sts & 0x0010) == 0 ) {
		return 0;			/* it has no list at all */
	}
	off = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_CAP_PTR, 1) & 0xfc;

	for ( guard = 0; guard < 48 && off >= 0x40; guard++ ) {
		UW	hdr = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, off, 2);

		if ( (hdr & 0xff) == cap_id ) {
			return off;
		}
		off = (hdr >> 8) & 0xfc;
	}

	return 0;
}

/* ---------------------------------------------------------------- messages */

#ifdef GICV2M_BASE

/*
 * The frame that turns a message into an interrupt. A card writes a
 * number to one register of it and the distributor raises that number,
 * so a card needs no wire of its own and no line shared with others.
 *
 * Which numbers may be used the frame says itself, so nothing here is
 * built into the board's list of addresses beyond where the frame is.
 */
#define V2M_MSI_TYPER		0x008
#define V2M_MSI_SETSPI_NS	0x040

#define V2M_TYPER_BASE(v)	(((v) >> 16) & 0x3ff)
#define V2M_TYPER_NUM(v)	((v) & 0x3ff)

/* what a table entry of the message capability holds */
#define MSIX_ENT_ADDR_LO	0x0
#define MSIX_ENT_ADDR_HI	0x4
#define MSIX_ENT_DATA		0x8
#define MSIX_ENT_CTRL		0xc
#define MSIX_CTRL_MASK		0x00000001

/* the capability itself */
#define MSIX_CAP_CTRL		0x02	/* count, and the two switches */
#define MSIX_CAP_TABLE		0x04	/* which window, and where in it */
#define MSIX_CTRL_ENABLE	0x8000
#define MSIX_CTRL_FUNCMASK	0x4000
#define MSIX_TABLE_BIR(v)	((v) & 0x7)
#define MSIX_TABLE_OFF(v)	((v) & ~0x7U)

LOCAL UINT msi_next = 0;		/* the next number to hand out */
LOCAL UINT msi_end = 0;

LOCAL ER msi_range( void )
{
	UW	typer;

	if ( msi_end != 0 ) {
		return E_OK;
	}
	typer = *(volatile UW *)(GICV2M_BASE + V2M_MSI_TYPER);
	msi_next = V2M_TYPER_BASE(typer);
	msi_end  = msi_next + V2M_TYPER_NUM(typer);
	if ( msi_next == 0 || msi_end <= msi_next || msi_end > N_INTVEC ) {
		msi_end = 0;
		return E_NOEXS;			/* the frame says nothing usable */
	}

	return E_OK;
}

/*
 * Give a function one message interrupt and answer the number it will
 * raise. The entry is written while it is masked, as the standard
 * requires, and unmasked once it is whole.
 */
EXPORT ER ts_pcie_msi_alloc( CONST T_PCIDEV *dev, UINT *p_intno )
{
	UBINT	table;
	UINT	cap, bir;
	UW	ctrl, off;
	UINT	intno;

	if ( dev == NULL || p_intno == NULL ) {
		return E_PAR;
	}
	if ( msi_range() < E_OK ) {
		return E_NOEXS;
	}
	cap = ts_pcie_cap_find(dev, PCI_CAP_MSIX);
	if ( cap == 0 ) {
		return E_NOSPT;			/* it has no messages to give */
	}
	if ( msi_next >= msi_end ) {
		return E_LIMIT;			/* the frame has none left */
	}

	off = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_TABLE, 4);
	bir = MSIX_TABLE_BIR(off);
	table = ts_pcie_bar_base(dev, (INT)bir);
	if ( table == 0 ) {
		return E_NOEXS;			/* the window it names is not there */
	}
	table += MSIX_TABLE_OFF(off);

	intno = msi_next++;

	/*
	 * A message is an edge, not a level. The frame raises the number
	 * for an instant and lets it fall at once; a distributor that is
	 * told the number is a level only sees it while it is high, and
	 * by the time it looks it has fallen. Every message would be
	 * lost, and whatever waits on them would wait out its whole time
	 * each time instead.
	 */
	SetIntMode(intno, IM_EDGE);

	/* masked while it is written, as the standard asks */
	*(volatile UW *)(table + MSIX_ENT_CTRL) = MSIX_CTRL_MASK;
	Asm("dsb sy" ::: "memory");
	*(volatile UW *)(table + MSIX_ENT_ADDR_LO) =
			(UW)(GICV2M_PA + V2M_MSI_SETSPI_NS);
	*(volatile UW *)(table + MSIX_ENT_ADDR_HI) =
			(UW)((UD)(GICV2M_PA + V2M_MSI_SETSPI_NS) >> 32);
	*(volatile UW *)(table + MSIX_ENT_DATA) = (UW)intno;
	Asm("dsb sy" ::: "memory");
	*(volatile UW *)(table + MSIX_ENT_CTRL) = 0;
	Asm("dsb sy" ::: "memory");

	/* and the capability turned on, with nothing held back */
	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_CTRL, 2);
	ctrl |= MSIX_CTRL_ENABLE;
	ctrl &= ~MSIX_CTRL_FUNCMASK;
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_CTRL, ctrl, 2);

	/* the line is no use once messages are on */
	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_COMMAND, 2);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, PCI_COMMAND,
			  ctrl | PCI_CMD_INTX_OFF, 2);

	*p_intno = intno;

	return E_OK;
}

#endif /* GICV2M_BASE */

/* ---------------------------------------------------------------- start-up */

/*
 * Walk the bus, handing out windows from the range given. A board that
 * had to bring its own controller up calls this once the link is
 * trained; a machine whose firmware left a flat window uses the call
 * below, which passes what the board set aside.
 */
EXPORT INT knl_pcie_scan( UD mmio_base, UD mmio_size )
{
	INT	i;

	if ( pci_ndev > 0 ) {
		return pci_ndev;		/* walked once is enough */
	}
	mmio_next = mmio_base;
	mmio_end  = mmio_base + mmio_size;
	bus_next  = 0;

	/* a bridge that is not there reads as all ones */
	if ( ts_pcie_cfg_read(0, 0, 0, PCI_VENDOR_ID, 4) == 0xFFFFFFFFU ) {
		return 0;			/* the machine has no bus */
	}
	bus_scan(0);
	bars_place();

	for ( i = 0; i < pci_ndev; i++ ) {
		T_PCIDEV	*d = &pci_dev[i];

		tm_printf((UB *)"TessronOS: pci %d:%d.%d %04x:%04x class %02x.%02x.%02x\n",
			  d->bus, d->dev, d->fn, d->vendor, d->device,
			  d->class_code, d->subclass, d->prog_if);
	}

	return pci_ndev;
}

#ifdef PCIE_MMIO_PA
EXPORT INT knl_pcie_init( void )
{
	return knl_pcie_scan(PCIE_MMIO_PA, PCIE_MMIO_SIZE);
}
#endif

#endif /* PCIE_ECAM_PA || PCIE_CFG_BOARD */
