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
#include "cpu_msi.h"

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
	DeviceBarrier();
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
#ifdef PCIE_KEEP_FW_BARS
			d->bar[i] = (UD)hi_orig << 32;
#endif
		}
	}
	d->bar_size[i] = size;
#ifdef PCIE_KEEP_FW_BARS
	/*
	 * The firmware gave every window its address, and keeps using some
	 * of them (the screen it set up): they stay where they are
	 */
	d->bar[i] |= ( d->bar_is_io[i] != 0 ) ? (UD)( orig & ~0x3U ) : (UD)( orig & ~0xFU );
#endif

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
#ifdef PCIE_KEEP_FW_BARS
		/* the firmware numbered the buses too: the one behind it is walked as it is */
		UINT	sub = ts_pcie_cfg_read(bus, dev, fn, PCI_SECONDARY_BUS, 1);

		pci_ndev++;
		if ( sub > bus && sub <= 0xff ) {
			bus_scan(sub);
		}
		return;
#else
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
#endif
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
 * Every bridge between the host and a card made to pass the card's
 * memory reads and writes upstream. A bridge whose bus master bit is off
 * stops both the card's DMA and its message interrupts, which are writes
 * too; firmware leaves it off on ports it did not use itself. Only the
 * path to a card a driver enables is opened, so that a port with nothing
 * in use behind it (a Thunderbolt port) passes no DMA. Says what it
 * changed.
 */
LOCAL void upstream( CONST T_PCIDEV *dev )
{
	INT	i, depth;
	UINT	bus, sec;
	UW	cmd, want;
	BOOL	found;

	if ( dev == NULL ) {
		return;
	}
	bus = dev->bus;
	for ( depth = 0; bus != 0 && depth < 8; depth++ ) {
		found = FALSE;
		for ( i = 0; i < pci_ndev; i++ ) {
			T_PCIDEV	*b = &pci_dev[i];

			if ( b->header != PCI_HDR_BRIDGE ) continue;
			sec = ts_pcie_cfg_read(b->bus, b->dev, b->fn, PCI_SECONDARY_BUS, 1);
			if ( sec != bus ) continue;
			cmd = ts_pcie_cfg_read(b->bus, b->dev, b->fn, PCI_COMMAND, 2);
			want = cmd | PCI_CMD_MEMORY | PCI_CMD_MASTER;
			if ( want != cmd ) {
				ts_pcie_cfg_write(b->bus, b->dev, b->fn, PCI_COMMAND, want, 2);
			}
			tm_printf((UB *)"pcie: bridge %02x:%02x.%x above %02x:%02x.%x: command %04x%s%04x\n",
				  b->bus, b->dev, b->fn, dev->bus, dev->dev, dev->fn, (UINT)cmd,
				  ( want != cmd ) ? " -> " : ", kept as ",
				  (UINT)ts_pcie_cfg_read(b->bus, b->dev, b->fn, PCI_COMMAND, 2));
			bus = b->bus;
			found = TRUE;
			break;
		}
		if ( !found ) break;
	}
}

/*
 * Let it answer on its windows and fetch memory for itself, and the
 * bridges above it pass what it fetches. The line is left on: firmware
 * that meant to use messages can leave it switched off, and then nothing
 * the card raises ever arrives.
 */
EXPORT ER ts_pcie_enable( CONST T_PCIDEV *dev )
{
	UW	cmd;

	if ( dev == NULL ) {
		return E_PAR;
	}
	upstream(dev);
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

#ifdef PCIE_BAR_CPU_OFFSET
	/* the outbound window puts the bus's addresses somewhere else for the processor */
	return DEV_BASE(dev->bar[bar] + PCIE_BAR_CPU_OFFSET);
#else
	return DEV_BASE(dev->bar[bar]);
#endif
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

#if KNL_HAS_MSI

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
#define MSIX_CTRL_SIZE		0x07ff	/* entries of the table, less one */
#define MSIX_ENT_SIZE		16
#define MSIX_TABLE_BIR(v)	((v) & 0x7)
#define MSIX_TABLE_OFF(v)	((v) & ~0x7U)

LOCAL UINT msi_next = 0;		/* the next number to hand out */
LOCAL UINT msi_end = 0;

LOCAL ER msi_range( void )
{
	UINT	first, num;

	if ( msi_end != 0 ) {
		return E_OK;
	}
	knl_msi_span(&first, &num);
	msi_next = first;
	msi_end  = first + num;
	if ( msi_next == 0 || msi_end <= msi_next || msi_end > N_INTVEC ) {
		msi_end = 0;
		return E_NOEXS;			/* the frame says nothing usable */
	}

	return E_OK;
}

#ifdef __x86_64__
/*
 * Where each message was written, so that it can be written again with
 * another processor's address (SetIntTarget, kernel/tkernel/inttarget.c):
 * an entry of a function's MSI-X table, or its plain MSI capability
 */
#define MSG_NONE	0
#define MSG_MSIX	1
#define MSG_PLAIN	2
#define MSI_CAP_ADDR_LO_W	0x04	/* the plain capability's address, low word */

LOCAL struct {
	UB	kind;
	UB	is64;
	UB	bus, dev, fn;
	UH	cap;
	UBINT	entry;			/* MSG_MSIX: the table entry */
} msg_at[N_INTVEC];

IMPORT ER	(*knl_msi_rewrite_hook)( UINT intno, UD addr );

LOCAL ER msg_rewrite( UINT intno, UD addr )
{
	UW	ctrl;

	if ( intno >= N_INTVEC ) {
		return E_PAR;
	}
	switch ( msg_at[intno].kind ) {
	case MSG_MSIX: {
		UBINT	t = msg_at[intno].entry;

		ctrl = *(volatile UW *)(t + MSIX_ENT_CTRL);
		*(volatile UW *)(t + MSIX_ENT_CTRL) = ctrl | MSIX_CTRL_MASK;
		DeviceBarrier();
		*(volatile UW *)(t + MSIX_ENT_ADDR_LO) = (UW)addr;
		*(volatile UW *)(t + MSIX_ENT_ADDR_HI) = (UW)( addr >> 32 );
		DeviceBarrier();
		*(volatile UW *)(t + MSIX_ENT_CTRL) = ctrl;
		DeviceBarrier();
		return E_OK;
	}
	case MSG_PLAIN:
		/* the processor is in the low word only: one write, whole */
		ts_pcie_cfg_write(msg_at[intno].bus, msg_at[intno].dev, msg_at[intno].fn,
				  msg_at[intno].cap + MSI_CAP_ADDR_LO_W, (UW)addr, 4);
		return E_OK;
	default:
		return E_OK;		/* not made yet: made with the new address */
	}
}

LOCAL void msg_note( UINT intno, UB kind, CONST T_PCIDEV *dev, UINT cap, UBINT entry )
{
	if ( intno >= N_INTVEC ) {
		return;
	}
	msg_at[intno].bus = (UB)dev->bus;
	msg_at[intno].dev = (UB)dev->dev;
	msg_at[intno].fn = (UB)dev->fn;
	msg_at[intno].cap = (UH)cap;
	msg_at[intno].entry = entry;
	msg_at[intno].kind = kind;
	knl_msi_rewrite_hook = msg_rewrite;
}


/* the capability of plain messages (MSI) */
#define MSI_CAP_CTRL		0x02
#define MSI_CAP_ADDR_LO		0x04
#define MSI_CTRL_ENABLE		0x0001
#define MSI_CTRL_MME		0x0070	/* messages enabled: one */
#define MSI_CTRL_64		0x0080
#define MSI_CTRL_PVM		0x0100	/* a mask bit of its own */

/*
 * A function with plain messages (Intel's integrated graphics has no
 * others): one message, its address and data written while the
 * capability is off, and then turned on. Asked for by name, or through
 * ts_pcie_irq_alloc when the function has no MSI-X.
 */
EXPORT ER ts_pcie_msi_plain_alloc( CONST T_PCIDEV *dev, UINT *p_intno )
{
	UW	ctrl, data_off;
	UINT	intno, cap;

	if ( dev == NULL || p_intno == NULL ) {
		return E_PAR;
	}
	if ( msi_range() < E_OK ) {
		return E_NOEXS;
	}
	cap = ts_pcie_cap_find(dev, PCI_CAP_MSI);
	if ( cap == 0 ) {
		return E_NOSPT;
	}
	if ( msi_next >= msi_end ) {
		return E_LIMIT;
	}
	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, cap + MSI_CAP_CTRL, 2);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSI_CAP_CTRL,
			  ctrl & ~( MSI_CTRL_ENABLE | MSI_CTRL_MME ), 2);
	intno = msi_next++;
	knl_msi_prepare(intno);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSI_CAP_ADDR_LO, (UW)knl_msi_addr(intno), 4);
	if ( ctrl & MSI_CTRL_64 ) {
		ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSI_CAP_ADDR_LO + 4,
				  (UW)( knl_msi_addr(intno) >> 32 ), 4);
		data_off = 0x0c;
	} else {
		data_off = 0x08;
	}
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + data_off, knl_msi_data(intno), 2);
	if ( ctrl & MSI_CTRL_PVM ) {
		ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + data_off + 4, 0, 4);	/* not masked */
	}
	ctrl = ( ctrl & ~MSI_CTRL_MME ) | MSI_CTRL_ENABLE;
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSI_CAP_CTRL, ctrl, 2);
	msg_note(intno, MSG_PLAIN, dev, cap, 0);

	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_COMMAND, 2);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, PCI_COMMAND, ctrl | PCI_CMD_INTX_OFF, 2);
	*p_intno = intno;
	return E_OK;
}
#endif

/*
 * Give a function one message interrupt and answer the number it will
 * raise. The capability is turned on first with the whole function
 * masked (MSI-X Enable and Function Mask together), as some functions
 * do not answer their table until MSI-X is on; the first time, every
 * entry is masked then, so that none left by the firmware fires. The
 * entry is written while it is masked, as the standard requires, and
 * unmasked once it is whole; the function is unmasked last.
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

	/* the interrupt readied for messages (cpu_msi.h) */
	knl_msi_prepare(intno);

	/* the capability on, the function masked as a whole */
	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_CTRL, 2);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_CTRL,
			  ctrl | MSIX_CTRL_ENABLE | MSIX_CTRL_FUNCMASK, 2);
	if ( ( ctrl & MSIX_CTRL_ENABLE ) == 0 ) {
		UINT	n = ( ctrl & MSIX_CTRL_SIZE ) + 1, i;

		for ( i = 1; i < n; i++ ) {
			*(volatile UW *)(table + i * MSIX_ENT_SIZE + MSIX_ENT_CTRL) = MSIX_CTRL_MASK;
		}
	}

	/* the entry masked while it is written, as the standard asks */
	*(volatile UW *)(table + MSIX_ENT_CTRL) = MSIX_CTRL_MASK;
	DeviceBarrier();
	*(volatile UW *)(table + MSIX_ENT_ADDR_LO) = (UW)knl_msi_addr(intno);
	*(volatile UW *)(table + MSIX_ENT_ADDR_HI) = (UW)( knl_msi_addr(intno) >> 32 );
	*(volatile UW *)(table + MSIX_ENT_DATA) = knl_msi_data(intno);
	DeviceBarrier();
	*(volatile UW *)(table + MSIX_ENT_CTRL) = 0;
	DeviceBarrier();
#ifdef __x86_64__
	msg_note(intno, MSG_MSIX, dev, cap, table);
#endif

	/* and the function unmasked, with nothing held back */
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, cap + MSIX_CAP_CTRL,
			  ( ctrl | MSIX_CTRL_ENABLE ) & ~MSIX_CTRL_FUNCMASK, 2);

	/* the line is no use once messages are on */
	ctrl = ts_pcie_cfg_read(dev->bus, dev->dev, dev->fn, PCI_COMMAND, 2);
	ts_pcie_cfg_write(dev->bus, dev->dev, dev->fn, PCI_COMMAND,
			  ctrl | PCI_CMD_INTX_OFF, 2);

	*p_intno = intno;

	return E_OK;
}

#else /* KNL_HAS_MSI */

/* No frame to send messages to: the functions use their lines */
EXPORT ER ts_pcie_msi_alloc( CONST T_PCIDEV *dev, UINT *p_intno )
{
	(void)dev;
	(void)p_intno;
	return E_NOEXS;
}

#endif /* KNL_HAS_MSI */

/* ---------------------------------------------------------------- the interrupt of a function */

/* What each function was given, for the drivers' records and the tests */
#define IRQ_REC_N	32
LOCAL struct {
	UB	used, bus, dev, fn, kind;
	UINT	intno;
} irq_rec[IRQ_REC_N];

EXPORT CONST char *ts_pcie_irq_name( INT kind )
{
	switch ( kind ) {
	case PCI_IRQ_MSIX:	return "MSI-X";
	case PCI_IRQ_MSI:	return "MSI";
	case PCI_IRQ_INTX:	return "INTx";
	default:		return "polled";
	}
}

EXPORT INT ts_pcie_irq_kind( CONST T_PCIDEV *dev, UINT *p_intno )
{
	INT	i;

	for ( i = 0; dev != NULL && i < IRQ_REC_N; i++ ) {
		if ( irq_rec[i].used && irq_rec[i].bus == dev->bus && irq_rec[i].dev == dev->dev
		  && irq_rec[i].fn == dev->fn ) {
			if ( p_intno != NULL ) *p_intno = irq_rec[i].intno;
			return irq_rec[i].kind;
		}
	}
	return E_NOEXS;
}

LOCAL void irq_note( CONST T_PCIDEV *dev, INT kind, UINT intno )
{
	INT	i, f = -1;

	for ( i = 0; i < IRQ_REC_N; i++ ) {
		if ( !irq_rec[i].used ) {
			if ( f < 0 ) f = i;
			continue;
		}
		if ( irq_rec[i].bus == dev->bus && irq_rec[i].dev == dev->dev && irq_rec[i].fn == dev->fn ) {
			f = i;
			break;
		}
	}
	if ( f < 0 ) return;
	irq_rec[f].used = 1;
	irq_rec[f].bus = dev->bus;
	irq_rec[f].dev = dev->dev;
	irq_rec[f].fn = dev->fn;
	irq_rec[f].kind = (UB)kind;
	irq_rec[f].intno = intno;
}

#ifdef __x86_64__
IMPORT int	knl_acpica_ready( void );
IMPORT int	knl_acpica_pci_gsi( unsigned int root_bus, const unsigned char *dev, const unsigned char *fn, int n,
				    unsigned int pin, char *how, int max );
IMPORT BOOL	knl_int_in_use( UINT intno );

/*
 * The line of a function on a PC: the GSI the _PRT tables name for it,
 * found by the path of bridges from the root bus down to it. A line that
 * a handler or another function already has is not taken: the drivers'
 * handlers each serve their own card only.
 */
LOCAL UINT intx_line( CONST T_PCIDEV *d, char *how, INT max )
{
	UB	dv[8], fn[8], rdv[8], rfn[8];
	UINT	bus = d->bus, v;
	INT	n = 0, i, k;
	int	gsi;

	if ( d->int_pin < 1 || d->int_pin > 4 ) {
		tm_sprintf((UB *)how, (UB *)"no line pin");
		return 0;
	}
	if ( !knl_acpica_ready() ) {
		tm_sprintf((UB *)how, (UB *)"no ACPI interpreter");
		return 0;
	}
	rdv[n] = d->dev;
	rfn[n] = d->fn;
	n++;
	while ( n < 8 ) {
		for ( i = 0; i < pci_ndev; i++ ) {
			if ( pci_dev[i].header == PCI_HDR_BRIDGE
			  && ts_pcie_cfg_read(pci_dev[i].bus, pci_dev[i].dev, pci_dev[i].fn, PCI_SECONDARY_BUS, 1) == bus ) {
				break;
			}
		}
		if ( i >= pci_ndev ) break;		/* bus is a root bus */
		rdv[n] = pci_dev[i].dev;
		rfn[n] = pci_dev[i].fn;
		n++;
		bus = pci_dev[i].bus;
	}
	for ( k = 0; k < n; k++ ) {
		dv[k] = rdv[n - 1 - k];
		fn[k] = rfn[n - 1 - k];
	}
	gsi = knl_acpica_pci_gsi(bus, dv, fn, n, d->int_pin, how, max);
	if ( gsi < 0 ) return 0;
	if ( gsi >= INTVEC_GSI_NUM ) {
		tm_sprintf((UB *)how + knl_strlen((char *)how), (UB *)", GSI %d beyond those served", gsi);
		return 0;
	}
	v = INTVEC_GSI(gsi);
	for ( i = 0; i < IRQ_REC_N; i++ ) {
		if ( irq_rec[i].used && irq_rec[i].kind == PCI_IRQ_INTX && irq_rec[i].intno == v ) break;
	}
	if ( i < IRQ_REC_N || knl_int_in_use(v) ) {
		tm_sprintf((UB *)how + knl_strlen((char *)how), (UB *)", GSI %d already in use", gsi);
		return 0;
	}
	tm_sprintf((UB *)how + knl_strlen((char *)how), (UB *)", GSI %d", gsi);
	SetIntMode(v, IM_LEVEL | IM_LOW);		/* a PCI line: level, active low */
	return v;
}
#endif

#if KNL_HAS_MSI && defined(__x86_64__)
/*
 * For trying out a driver's order of setting up its interrupt: with
 * "msiearly" in the start-up settings (cmdline.txt), each message
 * interrupt is raised once on the processor it goes to as soon as it is
 * enabled, as a function the firmware left with an interrupt pending
 * does, before the driver has defined the handler. The interrupt is
 * expected to be dropped (exc_hdl.c, Default_Handler) and the device
 * to work all the same.
 */
LOCAL BOOL msi_early_asked( void )
{
	CONST char	*c = knl_bootinfo->cmdline, *w = "msiearly";
	INT		k;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; w[k] != '\0' && c[k] == w[k]; k++ ) ;
		if ( w[k] == '\0' && ( c[k] == '\0' || c[k] == ' ' ) ) return TRUE;
	}
	return FALSE;
}

LOCAL void msi_early( UINT intno )
{
	UW	apic = knl_msi_apic(intno);
	UINT	s;

	tm_printf((UB *)"pcie: interrupt %d raised before its handler is defined, on the APIC ID %d (msiearly)\n",
		  intno, apic);
	s = disint();			/* the two halves of the command register written together */
	knl_apic_send(apic, intno, APIC_DM_FIXED);
	enaint(s);
}
#endif

/*
 * One interrupt for a function, tried in the order MSI-X, MSI, its line
 * (INTx, only when line is TRUE); the kind it got, *p_intno 0 when
 * there was none (PCI_IRQ_POLL: the driver polls). What was tried is
 * said on the console.
 */
EXPORT INT ts_pcie_irq_alloc( CONST T_PCIDEV *dev, UINT *p_intno, BOOL line )
{
	char	how[200];
	ER	ex, em = E_NOSPT;
	UINT	intno = 0;
	INT	kind = PCI_IRQ_POLL;

	if ( dev == NULL || p_intno == NULL ) {
		return E_PAR;
	}
	how[0] = '\0';
	ex = ts_pcie_msi_alloc(dev, &intno);
	if ( ex >= E_OK ) {
		kind = PCI_IRQ_MSIX;
	}
#if KNL_HAS_MSI && defined(__x86_64__)
	if ( kind == PCI_IRQ_POLL ) {
		em = ts_pcie_msi_plain_alloc(dev, &intno);
		if ( em >= E_OK ) kind = PCI_IRQ_MSI;
	}
#endif
	if ( kind == PCI_IRQ_POLL ) {
		intno = 0;
		if ( !line ) {
			tm_sprintf((UB *)how, (UB *)"the line not used");
		} else {
#ifdef __x86_64__
			intno = intx_line(dev, how, (INT)sizeof(how));
#else
			intno = dev->intno;
			if ( intno == 0 ) tm_sprintf((UB *)how, (UB *)"no line");
#endif
			if ( intno != 0 ) kind = PCI_IRQ_INTX;
		}
	}
	irq_note(dev, kind, intno);
	if ( kind == PCI_IRQ_MSIX ) {
		tm_printf((UB *)"pcie: %02x:%02x.%d %04x:%04x: interrupt by MSI-X (%d)\n",
			  dev->bus, dev->dev, dev->fn, dev->vendor, dev->device, intno);
	} else {
		/* why the earlier ones were not had: E_NOSPT the function has no such capability */
		tm_printf((UB *)"pcie: %02x:%02x.%d %04x:%04x: interrupt by %s (%d); MSI-X %d, MSI %d%s%s\n",
			  dev->bus, dev->dev, dev->fn, dev->vendor, dev->device, ts_pcie_irq_name(kind), intno,
			  (INT)ex, ( kind == PCI_IRQ_MSI ) ? E_OK : (INT)em, ( how[0] != '\0' ) ? ", line: " : "", how);
	}
#if KNL_HAS_MSI && defined(__x86_64__)
	if ( ( kind == PCI_IRQ_MSIX || kind == PCI_IRQ_MSI ) && msi_early_asked() ) {
		msi_early(intno);
	}
#endif
	*p_intno = intno;
	return kind;
}

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
