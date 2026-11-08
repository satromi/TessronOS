/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vmd.c (x64 PC)
 *	Intel Volume Management Device: the PCI function under which many
 *	notebooks of the 11th generation and later hide their NVMe disks
 *	(docs/tessronos/06-device.md 6.2.2).
 *
 *	With it on in the firmware's setup, the disks are on no bus of the
 *	machine's. The function has three windows: the first is the
 *	configuration space of a bus segment of its own, a megabyte per bus;
 *	the other two are the memory the functions on that segment answer in.
 *	A function there writes its message interrupts into the VMD, which
 *	reads the entry number from the destination bits of the address and
 *	raises its own MSI-X entry of that number instead. So one number of
 *	ours per function behind it: its entry in the VMD's table gets the real
 *	address and data, and the function's own table the entry number.
 *
 *	The segment comes to the rest of the system as functions in the bus's
 *	table (knl_pcie_domain_add), with windows and bus numbers given here
 *	whatever the firmware's own driver left. device/nvme/nvme.c finds its
 *	disks there as any other.
 *
 *	A function that gets no entry of its own, when the table runs out,
 *	gets no interrupt: ts_pcie_irq_alloc says so and the NVMe driver polls.
 *
 *	No machine at hand has this device: the console tells enough here, on
 *	a machine that has, to see where it stopped. With the start-up setting
 *	vmd=off it is left as the firmware left it.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "vmd.h"

#define VID_INTEL		0x8086

/* The function's configuration registers */
#define VMCAP			0x40
#define VMCAP_BUS_RESTRICT	0x00000001
#define VMCONFIG		0x44
#define VMCONFIG_NO_REMAP	0x00000002	/* its functions' messages go straight to the processors */
#define VMLOCK			0x70
#define VMLOCK_SHADOW		0x00000002

#define BAR_CFG			0
#define BAR_MEM1		2
#define BAR_MEM2		4
#define MEM2_MSIX_ROOM		0x2000		/* the VMD's own MSI-X table is at the start of the window */
#define MEM2_SHADOW_ROOM	0x10		/* where the host addresses of the windows are kept */

#define MSG_ADDR_BASE		U64(0xfee00000)

/* What differs between the kinds */
#define F_RESTRICT	0x01	/* VMCAP and VMCONFIG can move its buses' numbers */
#define F_VEC1		0x02	/* MSI-X entry 0 is not for its functions */
#define F_BYPASS	0x04	/* VMCONFIG_NO_REMAP works */
#define F_SHADOW_REG	0x08	/* a virtual machine's view is told in the MEM2 window */
#define F_SHADOW_CAP	0x10	/* the same, told in a vendor capability */
#define F_ORDER		0x20	/* a message can pass what the function wrote: read it first */
#define F_LTR		0x40	/* the firmware may leave the disk's LTR zero, which keeps the chip from sleeping */

#define F_CLIENT	( F_RESTRICT | F_VEC1 | F_SHADOW_CAP | F_LTR )

LOCAL CONST struct {
	UH	device;
	UB	flags;
} vmd_ids[] = {
	{ 0x201d, F_SHADOW_CAP },
	{ 0x28c0, F_RESTRICT | F_BYPASS | F_SHADOW_REG },
	{ 0x467f, F_CLIENT },
	{ 0x4c3d, F_CLIENT },
	{ 0x9a0b, F_CLIENT },
	{ 0xa77f, F_CLIENT },
	{ 0x7d0b, F_CLIENT | F_ORDER },
	{ 0xad0b, F_CLIENT | F_ORDER },
	{ 0xb06f, F_CLIENT },
	{ 0xb07f, F_CLIENT },
	{ 0xb60b, F_CLIENT },
	{ 0xd70b, F_CLIENT },
	{ 0xd73b, F_CLIENT }
};
#define VMD_NIDS	(INT)( sizeof(vmd_ids) / sizeof(vmd_ids[0]) )

#define VMD_MAX		4

typedef struct {
	T_PCIDOMAIN	dom;
	T_PCIDEV	host;
	UBINT		cfg;		/* the first window as the kernel sees it */
	UD		cfg_size;
	UINT		first_bus;
	UINT		vec_next;	/* the next MSI-X entry of the VMD to give */
	UINT		vec_n;
	BOOL		bypass;
} VMD;

LOCAL VMD	vmd[VMD_MAX];
LOCAL INT	vmd_n = 0;

EXPORT INT knl_vmd_first_bus( UW vmcap, UW vmconfig, BOOL restricts )
{
	if ( !restricts || ( vmcap & VMCAP_BUS_RESTRICT ) == 0 ) {
		return 0;
	}
	switch ( ( vmconfig >> 8 ) & 3 ) {
	case 0:		return 0;
	case 1:		return 128;
	default:	return 224;
	}
}

EXPORT void knl_vmd_child_msg( UINT entry, UD *p_addr, UW *p_data )
{
	*p_addr = MSG_ADDR_BASE | ( (UD)entry << 12 );
	*p_data = 0;
}

LOCAL UBINT vmd_reg( VMD *v, UINT bus, UINT dev, UINT fn, UINT off )
{
	UD	o = ( (UD)( bus - v->first_bus ) << 20 ) | ( (UD)dev << 15 ) | ( (UD)fn << 12 ) | ( off & 0xfff );

	if ( bus < v->first_bus || o + 4 > v->cfg_size ) {
		return 0;
	}
	return v->cfg + (UBINT)o;
}

LOCAL UW vmd_cfg_rd( void *arg, UINT bus, UINT dev, UINT fn, UINT off, INT width )
{
	UBINT	a = vmd_reg((VMD *)arg, bus, dev, fn, off);

	if ( a == 0 ) {
		return 0xffffffffU;
	}
	switch ( width ) {
	case 1:		return *(volatile UB *)a;
	case 2:		return *(volatile UH *)a;
	default:	return *(volatile UW *)a;
	}
}

/* A write is posted: read back, so that the next step finds it done */
LOCAL void vmd_cfg_wr( void *arg, UINT bus, UINT dev, UINT fn, UINT off, UW val, INT width )
{
	UBINT	a = vmd_reg((VMD *)arg, bus, dev, fn, off);

	if ( a == 0 ) {
		return;
	}
	switch ( width ) {
	case 1:		*(volatile UB *)a = (UB)val; (void)*(volatile UB *)a; break;
	case 2:		*(volatile UH *)a = (UH)val; (void)*(volatile UH *)a; break;
	default:	*(volatile UW *)a = val; (void)*(volatile UW *)a; break;
	}
	DeviceBarrier();
}

LOCAL ER vmd_msg( void *arg, CONST T_PCIDEV *child, UINT *p_intno, UD *p_addr, UW *p_data, BOOL *p_routed )
{
	VMD	*v = (VMD *)arg;
	UINT	e;
	ER	er;

	if ( v->bypass ) {
		*p_routed = FALSE;
		return knl_pcie_msg_direct(p_intno, p_addr, p_data);
	}
	if ( v->vec_next >= v->vec_n ) {
		tm_printf((UB *)"vmd: %x:%02x.%x: all %d MSI-X entries given: no interrupt for it\n",
			  child->bus, child->dev, child->fn, v->vec_n);
		return E_LIMIT;
	}
	e = v->vec_next;
	er = knl_pcie_msix_at(&v->host, e, p_intno);
	if ( er < E_OK ) {
		tm_printf((UB *)"vmd: its MSI-X entry %d not set (%d)\n", e, er);
		return er;
	}
	v->vec_next = e + 1;
	knl_vmd_child_msg(e, p_addr, p_data);
	*p_routed = TRUE;
	tm_printf((UB *)"vmd: %x:%02x.%x interrupts by the VMD's entry %d, interrupt %d\n",
		  child->bus, child->dev, child->fn, e, *p_intno);
	return E_OK;
}

/*
 * A virtual machine's view: the addresses the functions decode (shadow
 * registers or capability) differ from where the windows are to the
 * processor. Answers the difference, 0 as on a real machine; nothing is
 * taken on a view that differs.
 */
LOCAL D vmd_view_offset( CONST T_PCIDEV *h, UB flags )
{
	UD	phys = 0;
	UINT	cap;

	if ( ( flags & F_SHADOW_CAP ) != 0 ) {
		cap = ts_pcie_cap_find(h, 0x09);	/* vendor specific */
		if ( cap != 0 && ts_pcie_cfg_read(h->bus, h->dev, h->fn, cap + 4, 4) == 0x53484457U ) {	/* "SHDW" */
			phys = ts_pcie_cfg_read(h->bus, h->dev, h->fn, cap + 8, 4)
			     | ( (UD)ts_pcie_cfg_read(h->bus, h->dev, h->fn, cap + 12, 4) << 32 );
		}
	} else if ( ( flags & F_SHADOW_REG ) != 0
		 && ( ts_pcie_cfg_read(h->bus, h->dev, h->fn, VMLOCK, 4) & VMLOCK_SHADOW ) != 0 ) {
		phys = *(volatile UD *)( ts_pcie_bar_base(h, BAR_MEM2) + MEM2_MSIX_ROOM );
	}
	phys &= ~(UD)0xf;
	return ( phys == 0 ) ? 0 : (D)h->bar[BAR_MEM1] - (D)phys;
}

/* What a port tells of the link below it, for the records when a disk is not found */
LOCAL void vmd_ports( INT from, INT to )
{
	T_PCIDEV	d;
	INT		i;
	UINT		e;

	for ( i = from; i < to; i++ ) {
		if ( ts_pcie_get(i, &d) < E_OK || d.header != PCI_HDR_BRIDGE ) continue;
		e = ts_pcie_cap_find(&d, PCI_CAP_EXPRESS);
		tm_printf((UB *)"vmd: port %x:%02x.%x %04x:%04x, buses %x-%x, link status %04x, slot status %04x\n",
			  d.bus, d.dev, d.fn, d.vendor, d.device,
			  ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_SECONDARY_BUS, 1),
			  ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_SUBORDINATE_BUS, 1),
			  ( e != 0 ) ? ts_pcie_cfg_read(d.bus, d.dev, d.fn, e + 0x12, 2) : 0,
			  ( e != 0 ) ? ts_pcie_cfg_read(d.bus, d.dev, d.fn, e + 0x1a, 2) : 0);
	}
}

/*
 * The firmware leaves the snoop and no-snoop latencies of a disk behind
 * this device at zero on some machines, which says the disk can wait for
 * nothing, and the chip does not go to its deep states. Told the same
 * 3 ms as the firmware gives other disks.
 */
LOCAL void vmd_ltr( CONST T_PCIDEV *d )
{
	UINT	off = 0x100;
	UW	h;
	INT	guard;

	for ( guard = 0; guard < 48 && off >= 0x100; guard++ ) {
		h = ts_pcie_cfg_read(d->bus, d->dev, d->fn, off, 4);
		if ( h == 0 || h == 0xffffffffU ) return;
		if ( ( h & 0xffff ) == 0x18 ) {		/* latency tolerance reporting */
			if ( ts_pcie_cfg_read(d->bus, d->dev, d->fn, off + 4, 4) == 0 ) {
				ts_pcie_cfg_write(d->bus, d->dev, d->fn, off + 4, 0x10031003U, 4);
				tm_printf((UB *)"vmd: %x:%02x.%x its LTR was zero: 3 ms told\n", d->bus, d->dev, d->fn);
			}
			return;
		}
		off = h >> 20;
	}
}

LOCAL void vmd_take( CONST T_PCIDEV *p, UB flags )
{
	VMD		*v;
	T_PCIDOMAIN	*g;
	UINT		cap, msix = 0, i, first;
	UW		vmcap = 0, vmconf = 0;
	D		ofs;
	INT		from, n, before;
	UD		m2off;
	T_PCIDEV	d;

	if ( vmd_n >= VMD_MAX ) {
		return;
	}
	if ( p->bar[BAR_CFG] == 0 || p->bar[BAR_MEM1] == 0 || p->bar[BAR_MEM2] == 0 || p->bar_size[BAR_CFG] < 0x100000 ) {
		tm_printf((UB *)"vmd: %02x:%02x.%x: windows %lx+%lx %lx+%lx %lx+%lx: not usable\n", p->bus, p->dev, p->fn,
			  p->bar[0], p->bar_size[0], p->bar[2], p->bar_size[2], p->bar[4], p->bar_size[4]);
		return;
	}
	ts_pcie_enable(p);
	ofs = vmd_view_offset(p, flags);
	if ( ofs != 0 ) {
		tm_printf((UB *)"vmd: %02x:%02x.%x: a virtual machine's view of it (%ld apart): not taken\n",
			  p->bus, p->dev, p->fn, ofs);
		return;
	}
	if ( ( flags & F_RESTRICT ) != 0 ) {
		vmcap = ts_pcie_cfg_read(p->bus, p->dev, p->fn, VMCAP, 4);
		vmconf = ts_pcie_cfg_read(p->bus, p->dev, p->fn, VMCONFIG, 4);
		if ( vmconf == 0xffffffffU ) {
			tm_printf((UB *)"vmd: %02x:%02x.%x: VMCONFIG reads all ones\n", p->bus, p->dev, p->fn);
			return;
		}
	}
	first = (UINT)knl_vmd_first_bus(vmcap, vmconf, ( flags & F_RESTRICT ) != 0);

	v = &vmd[vmd_n];
	knl_memset(v, 0, sizeof(*v));
	v->host = *p;
	v->cfg = ts_pcie_bar_base(p, BAR_CFG);
	v->cfg_size = p->bar_size[BAR_CFG];
	v->first_bus = first;

	cap = ts_pcie_cap_find(p, PCI_CAP_MSIX);
	if ( cap != 0 ) {
		msix = ( ts_pcie_cfg_read(p->bus, p->dev, p->fn, cap + 2, 2) & 0x7ff ) + 1;
	}
	/*
	 * Where it can, its functions' messages go straight to the processors.
	 * Else entry by entry; the first (of a kind that keeps entry 0 for
	 * itself, the second) is left to what needs sharing, and nothing here
	 * shares, so that the disks start after it when there are enough.
	 */
	v->bypass = ( ( flags & F_BYPASS ) != 0 );
	if ( ( flags & F_BYPASS ) != 0 ) {
		ts_pcie_cfg_write(p->bus, p->dev, p->fn, VMCONFIG, vmconf | VMCONFIG_NO_REMAP, 4);
	} else if ( ( flags & F_RESTRICT ) != 0 ) {
		ts_pcie_cfg_write(p->bus, p->dev, p->fn, VMCONFIG, vmconf & ~(UW)VMCONFIG_NO_REMAP, 4);
	}
	i = ( ( flags & F_VEC1 ) != 0 ) ? 1 : 0;
	v->vec_next = ( msix > i + 1 ) ? i + 1 : i;
	v->vec_n = msix;

	g = &v->dom;
	g->arg = v;
	g->cfg_rd = vmd_cfg_rd;
	g->cfg_wr = vmd_cfg_wr;
	g->msg = vmd_msg;
	g->bus_first = first;
	n = (INT)( p->bar_size[BAR_CFG] >> 20 );
	g->bus_last = ( first + (UINT)n - 1 > 0xff ) ? 0xff : first + (UINT)n - 1;
	g->nrange = 2;
	g->range_next[0] = p->bar[BAR_MEM1];
	g->range_end[0] = p->bar[BAR_MEM1] + p->bar_size[BAR_MEM1];
	m2off = MEM2_MSIX_ROOM + ( ( ( flags & F_SHADOW_REG ) != 0 ) ? MEM2_SHADOW_ROOM : 0 );
	g->range_next[1] = p->bar[BAR_MEM2] + m2off;
	g->range_end[1] = p->bar[BAR_MEM2] + p->bar_size[BAR_MEM2];
	g->host_bus = p->bus;
	g->host_dev = p->dev;
	g->host_fn = p->fn;
	g->read_before_handler = ( ( flags & F_ORDER ) != 0 );

	tm_printf((UB *)"vmd: %04x:%04x at %02x:%02x.%x: config %lx+%lx, memory %lx+%lx and %lx+%lx, "
		  "VMCAP %08x VMCONFIG %08x VMLOCK %08x, buses %x-%x, MSI-X %d from entry %d%s\n",
		  p->vendor, p->device, p->bus, p->dev, p->fn,
		  p->bar[BAR_CFG], p->bar_size[BAR_CFG], p->bar[BAR_MEM1], p->bar_size[BAR_MEM1],
		  p->bar[BAR_MEM2], p->bar_size[BAR_MEM2], vmcap, vmconf,
		  ts_pcie_cfg_read(p->bus, p->dev, p->fn, VMLOCK, 4), g->bus_first, g->bus_last,
		  msix, v->vec_next, v->bypass ? ", its messages not taken in" : "");

	vmd_n++;
	from = ts_pcie_count();
	n = knl_pcie_domain_add(g);
	if ( n < E_OK ) {
		tm_printf((UB *)"vmd: not taken (%d)\n", n);
		return;
	}
	before = from + n;
	vmd_ports(from, before);
	if ( ( flags & F_LTR ) != 0 ) {
		INT	k;

		for ( k = from; k < before; k++ ) {
			if ( ts_pcie_get(k, &d) >= E_OK && d.class_code == 0x01 && d.subclass == 0x08 ) vmd_ltr(&d);
		}
	}
	tm_printf((UB *)"vmd: %d functions behind it\n", n);
}

/*
 * Before any driver looks at the bus. A machine without this device goes
 * through the table once and nothing else happens. A function of Intel's
 * of the RAID class that is not in the list is told: its disks are hidden
 * here, and the identity is what is needed to add it.
 */
EXPORT void knl_vmd_init( BOOL off )
{
	T_PCIDEV	p;
	INT		i, k, n = ts_pcie_count();

	for ( i = 0; i < n; i++ ) {
		if ( ts_pcie_get(i, &p) < E_OK || p.vendor != VID_INTEL ) continue;
		for ( k = 0; k < VMD_NIDS && vmd_ids[k].device != p.device; k++ ) ;
		if ( k >= VMD_NIDS ) {
			if ( p.class_code == 0x01 && p.subclass == 0x04 ) {
				tm_printf((UB *)"vmd: 8086:%04x at %02x:%02x.%x of the RAID class is not a kind known here: "
					  "disks behind it are not seen\n", p.device, p.bus, p.dev, p.fn);
			}
			continue;
		}
		if ( off ) {
			tm_printf((UB *)"vmd: 8086:%04x at %02x:%02x.%x left alone (vmd=off): disks behind it are not seen\n",
				  p.device, p.bus, p.dev, p.fn);
			continue;
		}
		vmd_take(&p, vmd_ids[k].flags);
	}
}

#ifdef USE_KTEST
/*
 * For the tests (the start-up setting vmdsim): the machine has no VMD, so
 * the last NVMe controller on its bus is shown behind a made-up one. A
 * root port kept in memory stands at the top of the segment, the controller
 * under it through its real configuration space; its messages go straight to
 * the processors as those of a VMD that does not take them in. Everything else
 * is what a real one goes through: the numbering, the windows, the table,
 * and the NVMe driver on all that. The window of the controller moves to a
 * range nothing else uses.
 */
#define SIM_RANGE	U64(0x4000000000)
#define SIM_RANGE_SIZE	U64(0x4000000)

LOCAL UB		sim_port[256];
LOCAL T_PCIDEV		sim_real;
LOCAL T_PCIDOMAIN	sim_dom;
EXPORT INT		knl_vmd_sim_on = 0;

LOCAL UW sim_rd( void *arg, UINT bus, UINT dev, UINT fn, UINT off, INT width )
{
	UW	v = 0xffffffffU;
	INT	i;

	(void)arg;
	if ( bus == sim_dom.bus_first ) {
		if ( dev != 0 || fn != 0 ) return v;
		v = 0;
		for ( i = 0; i < width; i++ ) v |= (UW)sim_port[( off + (UINT)i ) & 0xff] << ( i * 8 );
		return v;
	}
	if ( bus == sim_port[PCI_SECONDARY_BUS] && bus != 0 && dev == 0 && fn == 0 ) {
		return ts_pcie_cfg_read(sim_real.bus, sim_real.dev, sim_real.fn, off, width);
	}
	return ( width == 1 ) ? 0xff : ( width == 2 ) ? 0xffff : v;
}

LOCAL void sim_wr( void *arg, UINT bus, UINT dev, UINT fn, UINT off, UW val, INT width )
{
	INT	i;

	(void)arg;
	if ( bus == sim_dom.bus_first ) {
		if ( dev != 0 || fn != 0 ) return;
		/* the 64 bit window is a read-only kind; the identity and the header stay */
		if ( off < 0x18 || off == 0x34 ) return;
		if ( off == 0x24 && width == 4 ) val = ( val & 0xfff0fff0U ) | 0x00010001;
		for ( i = 0; i < width; i++ ) sim_port[( off + (UINT)i ) & 0xff] = (UB)( val >> ( i * 8 ) );
		return;
	}
	if ( bus == sim_port[PCI_SECONDARY_BUS] && dev == 0 && fn == 0 ) {
		ts_pcie_cfg_write(sim_real.bus, sim_real.dev, sim_real.fn, off, val, width);
	}
}

LOCAL ER sim_msg( void *arg, CONST T_PCIDEV *child, UINT *p_intno, UD *p_addr, UW *p_data, BOOL *p_routed )
{
	(void)arg;
	(void)child;
	*p_routed = FALSE;
	return knl_pcie_msg_direct(p_intno, p_addr, p_data);
}

EXPORT void knl_vmd_sim( void )
{
	T_PCIDEV	p;
	INT		k, n = 0;

	for ( k = 0; ts_pcie_find(0x01, 0x08, 0x02, k, &p) == E_OK; k++ ) {
		sim_real = p;
		n++;
	}
	if ( n < 2 ) {
		tm_printf((UB *)"vmdsim: wants a second NVMe controller\n");
		return;
	}
	knl_pcie_hide(sim_real.bus, sim_real.dev, sim_real.fn);
	ts_pcie_cfg_write(sim_real.bus, sim_real.dev, sim_real.fn, PCI_COMMAND,
			  ts_pcie_cfg_read(sim_real.bus, sim_real.dev, sim_real.fn, PCI_COMMAND, 2)
			  & ~( PCI_CMD_MEMORY | PCI_CMD_MASTER ), 2);
	knl_memset(sim_port, 0, sizeof(sim_port));
	sim_port[0] = 0x86; sim_port[1] = 0x80; sim_port[2] = 0xff; sim_port[3] = 0xff;
	sim_port[0x06] = 0x10;		/* no capability list is needed: the list is empty */
	sim_port[0x0a] = 0x04; sim_port[0x0b] = 0x06;	/* a bridge */
	sim_port[0x0e] = PCI_HDR_BRIDGE;
	sim_port[0x24] = 0x01; sim_port[0x26] = 0x01;	/* a 64 bit prefetchable window */

	knl_memset(&sim_dom, 0, sizeof(sim_dom));
	sim_dom.cfg_rd = sim_rd;
	sim_dom.cfg_wr = sim_wr;
	sim_dom.msg = sim_msg;
	sim_dom.bus_first = 224;
	sim_dom.bus_last = 255;
	sim_dom.nrange = 1;
	sim_dom.range_next[0] = SIM_RANGE;
	sim_dom.range_end[0] = SIM_RANGE + SIM_RANGE_SIZE;
	sim_dom.host_bus = 0xffff;
	sim_dom.read_before_handler = TRUE;
	k = knl_pcie_domain_add(&sim_dom);
	tm_printf((UB *)"vmdsim: %d functions behind the made-up VMD; the real one at %02x:%02x.%x\n",
		  k, sim_real.bus, sim_real.dev, sim_real.fn);
	knl_vmd_sim_on = 1;
}

/* Secondary bus, and the port's 64 bit window as the walk set it */
EXPORT void knl_vmd_sim_port( UINT *p_sec, UD *p_base, UD *p_limit )
{
	*p_sec = sim_port[PCI_SECONDARY_BUS];
	*p_base = ( (UD)*(UW *)&sim_port[0x28] << 32 ) | ( (UD)( *(UH *)&sim_port[0x24] & 0xfff0 ) << 16 );
	*p_limit = ( (UD)*(UW *)&sim_port[0x2c] << 32 ) | ( (UD)( *(UH *)&sim_port[0x26] & 0xfff0 ) << 16 ) | 0xfffff;
}
#endif /* USE_KTEST */

#endif /* X64 */
