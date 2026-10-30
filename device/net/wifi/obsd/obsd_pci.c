/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_pci.c (x64 PC, WIFI_NET80211=obsd)
 *	The card's bus as its driver (iwm(4) or iwx(4)) sees it (design
 *	9.6.9): the configuration space, the window of its registers, DMA,
 *	the message interrupt and the firmware. Compiled with OpenBSD's kernel headers; the card is
 *	reached through the hypercalls the rump kernel's Wi-Fi uses as well
 *	(wifi_user.c, rumpcomp_*).
 *
 *	The kernel's memory is in its direct map, where an address is the
 *	physical one plus a constant: DMA addresses are what
 *	rumpcomp_pci_virt_to_mach makes of each page of a buffer, the pages
 *	physically next to each other joined into one segment. Memory the
 *	driver asks for by bus_dmamem_alloc is whole pages below 4GB, cleared,
 *	from the page allocator (rumpcomp_pci_dmalloc).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/device.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "obsd_glue.h"

/* ---------------------------------------------------------------- configuration space */

#define TAG_BUS(t)	(((t) >> 16) & 0xff)
#define TAG_DEV(t)	(((t) >> 8) & 0x1f)
#define TAG_FN(t)	((t) & 0x7)

pcitag_t
obsd_pci_tag(int bus, int dev, int fn)
{
	return ((pcitag_t)bus << 16) | ((pcitag_t)dev << 8) | (pcitag_t)fn;
}

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	unsigned int v = 0xffffffff;

	(void)rumpcomp_pci_confread(TAG_BUS(tag), TAG_DEV(tag), TAG_FN(tag), reg, &v);
	return v;
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t data)
{
	(void)rumpcomp_pci_confwrite(TAG_BUS(tag), TAG_DEV(tag), TAG_FN(tag), reg, data);
}

int
pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid,
    int *offset, pcireg_t *value)
{
	pcireg_t reg;
	unsigned int ofs;
	int n = 0;

	reg = pci_conf_read(pc, tag, PCI_COMMAND_STATUS_REG);
	if (!(reg & PCI_STATUS_CAPLIST_SUPPORT))
		return 0;
	ofs = PCI_CAPLIST_PTR(pci_conf_read(pc, tag, PCI_CAPLISTPTR_REG));
	while (ofs != 0 && n++ < 48) {
		reg = pci_conf_read(pc, tag, ofs);
		if (PCI_CAPLIST_CAP(reg) == capid) {
			if (offset)
				*offset = ofs;
			if (value)
				*value = reg;
			return 1;
		}
		ofs = PCI_CAPLIST_NEXT(reg);
	}
	return 0;
}

int
pci_matchbyid(struct pci_attach_args *pa, const struct pci_matchid *ids, int nent)
{
	for (; nent-- > 0; ids++) {
		if (PCI_VENDOR(pa->pa_id) == ids->pm_vid &&
		    PCI_PRODUCT(pa->pa_id) == ids->pm_pid)
			return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- the registers' window */

static u_int8_t
mem_read_1(bus_space_handle_t h, bus_size_t o)
{
	return *(volatile u_int8_t *)(h + o);
}

static u_int16_t
mem_read_2(bus_space_handle_t h, bus_size_t o)
{
	return *(volatile u_int16_t *)(h + o);
}

static u_int32_t
mem_read_4(bus_space_handle_t h, bus_size_t o)
{
	return *(volatile u_int32_t *)(h + o);
}

static u_int64_t
mem_read_8(bus_space_handle_t h, bus_size_t o)
{
	return *(volatile u_int64_t *)(h + o);
}

static void
mem_write_1(bus_space_handle_t h, bus_size_t o, u_int8_t v)
{
	*(volatile u_int8_t *)(h + o) = v;
}

static void
mem_write_2(bus_space_handle_t h, bus_size_t o, u_int16_t v)
{
	*(volatile u_int16_t *)(h + o) = v;
}

static void
mem_write_4(bus_space_handle_t h, bus_size_t o, u_int32_t v)
{
	*(volatile u_int32_t *)(h + o) = v;
}

static void
mem_write_8(bus_space_handle_t h, bus_size_t o, u_int64_t v)
{
	*(volatile u_int64_t *)(h + o) = v;
}

static const struct x86_bus_space_ops obsd_mem_ops = {
	.read_1 = mem_read_1,
	.read_2 = mem_read_2,
	.read_4 = mem_read_4,
	.read_8 = mem_read_8,
	.write_1 = mem_write_1,
	.write_2 = mem_write_2,
	.write_4 = mem_write_4,
	.write_8 = mem_write_8,
};

pcireg_t
pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	return PCI_MAPREG_TYPE(pci_conf_read(pc, tag, reg)) |
	    (PCI_MAPREG_MEM_TYPE(pci_conf_read(pc, tag, reg)));
}

/*
 * The window of a memory BAR: its address as the firmware set it, its
 * size as the BAR answers all ones, the window of the kernel's that maps
 * the device's addresses
 */
int
pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags,
    bus_space_tag_t *tagp, bus_space_handle_t *handlep, bus_addr_t *basep,
    bus_size_t *sizep, bus_size_t maxsize)
{
	pcireg_t lo, hi = 0, slo, shi = 0xffffffff;
	uint64_t base, mask, size;
	int is64;

	lo = pci_conf_read(pa->pa_pc, pa->pa_tag, reg);
	if (PCI_MAPREG_TYPE(lo) != PCI_MAPREG_TYPE_MEM)
		return EINVAL;
	is64 = PCI_MAPREG_MEM_TYPE(lo) == PCI_MAPREG_MEM_TYPE_64BIT;
	if (is64)
		hi = pci_conf_read(pa->pa_pc, pa->pa_tag, reg + 4);
	pci_conf_write(pa->pa_pc, pa->pa_tag, reg, 0xffffffff);
	slo = pci_conf_read(pa->pa_pc, pa->pa_tag, reg);
	pci_conf_write(pa->pa_pc, pa->pa_tag, reg, lo);
	if (is64) {
		pci_conf_write(pa->pa_pc, pa->pa_tag, reg + 4, 0xffffffff);
		shi = pci_conf_read(pa->pa_pc, pa->pa_tag, reg + 4);
		pci_conf_write(pa->pa_pc, pa->pa_tag, reg + 4, hi);
	}
	base = ((uint64_t)hi << 32) | (lo & PCI_MAPREG_MEM_ADDR_MASK);
	mask = ((uint64_t)shi << 32) | (slo & PCI_MAPREG_MEM_ADDR_MASK);
	if (!is64)
		mask |= 0xffffffff00000000ULL;
	size = ~mask + 1;
	if (base == 0 || size == 0)
		return EINVAL;
	if (maxsize != 0 && size > maxsize)
		size = maxsize;
	*tagp = &obsd_mem_ops;
	*handlep = (bus_space_handle_t)rumpcomp_pci_map(base, size);
	if (basep)
		*basep = base;
	if (sizep)
		*sizep = size;
	printf("obsd: BAR 0x%x at 0x%llx, %llu bytes\n", reg,
	    (unsigned long long)base, (unsigned long long)size);
	return 0;
}

/* The window stays: it is the kernel's map of the device's addresses, which nothing takes away */
void
bus_space_unmap(bus_space_tag_t t, bus_space_handle_t bsh, bus_size_t size)
{
}

/* ---------------------------------------------------------------- the interrupt */

struct obsd_ih {
	unsigned int	cookie;
	int		(*func)(void *);
	void		*arg;
};

/* No MSI-X, and no line: the card's one message (MSI); iwx too takes it when it has no MSI-X */
int
pci_intr_map_msix(struct pci_attach_args *pa, int vec, pci_intr_handle_t *ihp)
{
	return 1;
}

int
pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	return 1;
}

int
pci_intr_map_msi(struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	static unsigned int next;
	unsigned int cookie = ++next;

	if (rumpcomp_pci_irq_map(pa->pa_bus, pa->pa_device, pa->pa_function,
	    pa->pa_intrline, cookie) != 0)
		return 1;
	memset(ihp, 0, sizeof(*ihp));
	ihp->tag = pa->pa_tag;
	ihp->line = (int)cookie;
	return 0;
}

const char *
pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{
	static char buf[16];

	snprintf(buf, sizeof(buf), "msi %d", ih.line);
	return buf;
}

/* The task of the interrupt: it waits for the card's message, then runs the handler holding the lock */
static void
intr_main(void *v)
{
	struct obsd_ih *ih = v;
	int depth;

	for (;;) {
		depth = obsd_lock_drop();
		(void)rumpcomp_tswifi_intr_wait(ih->cookie);
		obsd_lock_take(depth);
		(void)(*ih->func)(ih->arg);
	}
}

void *
pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int level,
    int (*func)(void *), void *arg, const char *what)
{
	struct obsd_ih *t = malloc(sizeof(*t), M_DEVBUF, M_WAITOK | M_ZERO);

	if (t == NULL)
		return NULL;
	t->cookie = (unsigned int)ih.line;
	t->func = func;
	t->arg = arg;
	if (obsd_task(intr_main, t, OBSD_PRI_INTR) != 0) {
		free(t, M_DEVBUF, sizeof(*t));
		return NULL;
	}
	return t;
}

void
pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{
	/* the card stays for as long as the machine runs */
}

/* ---------------------------------------------------------------- DMA */

static int
dm_create(bus_dma_tag_t t, bus_size_t size, int nsegments, bus_size_t maxsegsz,
    bus_size_t boundary, int flags, bus_dmamap_t *dmamp)
{
	struct bus_dmamap *map;
	size_t len;

	len = sizeof(*map) + sizeof(bus_dma_segment_t) * (nsegments - 1);
	map = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (map == NULL)
		return ENOMEM;
	map->_dm_size = size;
	map->_dm_segcnt = nsegments;
	map->_dm_maxsegsz = maxsegsz;
	map->_dm_boundary = boundary;
	map->_dm_flags = flags;
	*dmamp = map;
	return 0;
}

static void
dm_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{
	free(map, M_DEVBUF, 0);
}

/* A run of virtual addresses added to the map's segments, page by page */
static int
dm_add(bus_dmamap_t map, vaddr_t va, bus_size_t len)
{
	bus_dma_segment_t *seg;
	bus_size_t chunk;
	unsigned long pa;

	while (len > 0) {
		chunk = PAGE_SIZE - (va & PAGE_MASK);
		if (chunk > len)
			chunk = len;
		pa = rumpcomp_pci_virt_to_mach((void *)va);
		seg = map->dm_nsegs > 0 ? &map->dm_segs[map->dm_nsegs - 1] : NULL;
		if (seg != NULL && seg->ds_addr + seg->ds_len == pa &&
		    seg->ds_len + chunk <= map->_dm_maxsegsz &&
		    (map->_dm_boundary == 0 ||
		     (seg->ds_addr & ~(map->_dm_boundary - 1)) ==
		     ((pa + chunk - 1) & ~(map->_dm_boundary - 1)))) {
			seg->ds_len += chunk;
		} else {
			if (map->dm_nsegs >= map->_dm_segcnt)
				return EFBIG;
			seg = &map->dm_segs[map->dm_nsegs++];
			seg->ds_addr = pa;
			seg->ds_len = chunk;
			seg->_ds_va = va;
		}
		map->dm_mapsize += chunk;
		va += chunk;
		len -= chunk;
	}
	return 0;
}

static int
dm_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t buflen,
    struct proc *p, int flags)
{
	int error;

	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	if (buflen > map->_dm_size)
		return EINVAL;
	error = dm_add(map, (vaddr_t)buf, buflen);
	if (error) {
		map->dm_nsegs = 0;
		map->dm_mapsize = 0;
	}
	return error;
}

static int
dm_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m0, int flags)
{
	struct mbuf *m;
	int error = 0;

	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	if (m0->m_pkthdr.len > map->_dm_size)
		return EINVAL;
	for (m = m0; m != NULL && error == 0; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		error = dm_add(map, (vaddr_t)m->m_data, m->m_len);
	}
	if (error) {
		map->dm_nsegs = 0;
		map->dm_mapsize = 0;
	}
	return error;
}

static int
dm_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs,
    int nsegs, bus_size_t size, int flags)
{
	int i;

	if (nsegs > map->_dm_segcnt)
		return EFBIG;
	for (i = 0; i < nsegs; i++)
		map->dm_segs[i] = segs[i];
	map->dm_nsegs = nsegs;
	map->dm_mapsize = size;
	return 0;
}

static void
dm_unload(bus_dma_tag_t t, bus_dmamap_t map)
{
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
}

/*
 * What the card wrote, looked at as the driver begins to read it
 * (tswifi_obsd.c's tsw_rx_peek: the frames the card received, counted
 * before iwm drops any)
 */
void	(*obsd_rx_peek)(bus_dmamap_t, bus_addr_t, bus_size_t, int);

/* The x64's DMA sees the caches: only the order of the stores and loads around it matters */
static void
dm_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	__asm volatile("mfence" ::: "memory");
	if (obsd_rx_peek != NULL && (ops & BUS_DMASYNC_POSTREAD))
		(*obsd_rx_peek)(map, off, len, ops);
}

static int
dm_mem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags)
{
	unsigned long pa, va;

	if (rumpcomp_pci_dmalloc(size, alignment, &pa, &va) != 0)
		return ENOMEM;
	memset(&segs[0], 0, sizeof(segs[0]));
	segs[0].ds_addr = pa;
	segs[0].ds_len = size;
	segs[0]._ds_va = va;
	*rsegs = 1;
	return 0;
}

static void
dm_mem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		rumpcomp_pci_dmafree(segs[i]._ds_va, segs[i].ds_len);
}

static int
dm_mem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size,
    caddr_t *kvap, int flags)
{
	if (nsegs != 1)
		return EINVAL;
	*kvap = (caddr_t)segs[0]._ds_va;
	return 0;
}

static void
dm_mem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
}

struct bus_dma_tag obsd_dma_tag = {
	._dmamap_create = dm_create,
	._dmamap_destroy = dm_destroy,
	._dmamap_load = dm_load,
	._dmamap_load_mbuf = dm_load_mbuf,
	._dmamap_load_raw = dm_load_raw,
	._dmamap_unload = dm_unload,
	._dmamap_sync = dm_sync,
	._dmamem_alloc = dm_mem_alloc,
	._dmamem_free = dm_mem_free,
	._dmamem_map = dm_mem_map,
	._dmamem_unmap = dm_mem_unmap,
};

/* ---------------------------------------------------------------- firmware */

/*
 * A firmware file read whole from its object on the system volume; the
 * lock is let go while the disk is read
 */
int
loadfirmware(const char *name, u_char **bufp, size_t *buflen)
{
	void *data;
	unsigned long long size;
	u_char *buf;
	int error, depth;

	depth = obsd_lock_drop();
	error = rumpcomp_tswifi_fw_open(name, &data, &size);
	obsd_lock_take(depth);
	if (error)
		return error;
	buf = malloc(size, M_DEVBUF, M_NOWAIT);
	if (buf == NULL) {
		rumpcomp_tswifi_fw_close(data, size);
		return ENOMEM;
	}
	memcpy(buf, data, size);
	rumpcomp_tswifi_fw_close(data, size);
	*bufp = buf;
	*buflen = size;
	return 0;
}
