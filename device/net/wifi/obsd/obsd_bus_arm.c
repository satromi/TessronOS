/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_bus_arm.c (Raspberry Pi, WIFI_NET80211=obsd)
 *	The machine's side of the layer where there is no PCI card (design
 *	9.6.8, 9.6.9): the registers of the Wi-Fi's SDIO host, DMA with the
 *	cache work the Pi's bus masters need, the firmware, and what sdmmc(4)
 *	and bwfm(4) ask of the kernel's autoconfiguration and threads.
 *	Compiled with OpenBSD's kernel headers. The host, its interrupt and
 *	the firmware are the hypercalls of wifi_user_rpi5.c (rumpcomp_*).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/device.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/pool.h>
#include <sys/task.h>
#include <sys/socket.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_var.h>

#include <dev/ofw/openfirm.h>

#include <dev/sdmmc/sdmmcvar.h>
#include <dev/sdmmc/sdhcreg.h>

#include "obsd_glue.h"
#include "obsd_arm.h"

/* ---------------------------------------------------------------- the registers */

/*
 * Accesses of device memory, ordered. On a host that loses a register's
 * write made within two of the card's clocks of the one before it
 * (obsd_bus_write_gap), each write but those to the data port is kept
 * apart from the next: two clocks at the slowest, 400 kHz, and the part
 * of a microsecond rounded up. Spun, not slept: sdhc holds its lock.
 */
#define WRITE_GAP_US	6

int	obsd_bus_write_gap;

static void
wr_gap(bus_size_t o)
{
	if (obsd_bus_write_gap && o != SDHC_DATA) {
		__asm volatile("dsb osh" ::: "memory");
		obsd_hc_delay(WRITE_GAP_US);
	}
}

#define RD(type, h, o, v) do {						\
	(v) = *(volatile type *)((h) + (o));				\
	__asm volatile("dmb oshld" ::: "memory");			\
} while (0)
#define WR(type, h, o, v) do {						\
	__asm volatile("dmb oshst" ::: "memory");			\
	*(volatile type *)((h) + (o)) = (v);				\
	wr_gap(o);							\
} while (0)

static u_int8_t
rd_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	u_int8_t v;

	RD(u_int8_t, h, o, v);
	return v;
}

static u_int16_t
rd_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	u_int16_t v;

	RD(u_int16_t, h, o, v);
	return v;
}

static u_int32_t
rd_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	u_int32_t v;

	RD(u_int32_t, h, o, v);
	return v;
}

static void
wr_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int8_t v)
{
	WR(u_int8_t, h, o, v);
}

static void
wr_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int16_t v)
{
	WR(u_int16_t, h, o, v);
}

static void
wr_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int32_t v)
{
	WR(u_int32_t, h, o, v);
}

static int
sp_subregion(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, bus_size_t s,
    bus_space_handle_t *n)
{
	*n = h + o;
	return 0;
}

struct bus_space obsd_host_space = {
	._space_read_1 = rd_1,
	._space_write_1 = wr_1,
	._space_read_2 = rd_2,
	._space_write_2 = wr_2,
	._space_read_4 = rd_4,
	._space_write_4 = wr_4,
	._space_subregion = sp_subregion,
};

/* ---------------------------------------------------------------- DMA */

/* The Wi-Fi's host runs by PIO; DMA is what the layer's test maps, on pages below 4GB */
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
			seg->_ds_vaddr = va;
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
 * The Pi's bus masters do not see the caches: written out before the
 * device reads, the CPU's copies dropped before and after the device writes
 */
#define SYNC_CLEAN	1
#define SYNC_INVAL	2

static void
dm_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	int i, how = 0;

	if (ops & (BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD))
		how |= SYNC_CLEAN;
	if (ops & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_POSTREAD))
		how |= SYNC_INVAL;
	if (how == 0)
		return;
	for (i = 0; i < map->dm_nsegs && len > 0; i++) {
		bus_dma_segment_t *ds = &map->dm_segs[i];
		bus_size_t n;

		if (off >= ds->ds_len) {
			off -= ds->ds_len;
			continue;
		}
		n = ds->ds_len - off;
		if (n > len)
			n = len;
		rumpcomp_tswifi_dmasync((void *)(ds->_ds_vaddr + off), n, how);
		off = 0;
		len -= n;
	}
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
	segs[0]._ds_vaddr = va;
	*rsegs = 1;
	return 0;
}

static void
dm_mem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		rumpcomp_pci_dmafree(segs[i]._ds_vaddr, segs[i].ds_len);
}

static int
dm_mem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size,
    caddr_t *kvap, int flags)
{
	if (nsegs != 1)
		return EINVAL;
	*kvap = (caddr_t)segs[0]._ds_vaddr;
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

/* A file read whole from its object on the system volume; the lock let go while the disk is read */
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

/* ---------------------------------------------------------------- what the drivers ask of the kernel */

/* No device tree in the layer: the board's name and address are not looked for there */
int
OF_peer(int node)
{
	return 0;
}

int
OF_getprop(int node, char *prop, void *buf, int len)
{
	return -1;
}

/* A thread of the kernel: a task of the layer's, holding the lock as they do */
static struct proc	kthread_proc;

int
kthread_create(void (*func)(void *), void *arg, struct proc **newpp, const char *name)
{
	if (obsd_task(func, arg, OBSD_PRI_TASKQ) != 0)
		return ENOMEM;
	if (newpp != NULL)
		*newpp = &kthread_proc;
	return 0;
}

void
kthread_create_deferred(void (*func)(void *), void *arg)
{
	(*func)(arg);
}

/* These threads run for as long as the machine does */
void
kthread_exit(int status)
{
	for (;;)
		tsleep_nsec(&kthread_proc, 0, "kthexit", INFSLP);
}

void
config_pending_incr(void)
{
}

void
config_pending_decr(void)
{
}

int
config_activate_children(struct device *parent, int act)
{
	return 0;
}

int
config_detach(struct device *dev, int flags)
{
	return 0;
}

/*
 * The one tree of devices here: the host's device gets sdmmc0 and each
 * SDIO function is offered to bwfm. The functions are said with the
 * card's vendor and product, for what a start shows on the console.
 */
extern struct cfattach sdmmc_ca, bwfm_sdio_ca;

struct device	*tsw_host_dev;		/* tsw_bwfm.c */
struct device	*tsw_chip_dev;

static struct device *
attach_as(struct cfattach *ca, struct device *parent, const char *xname, void *aux)
{
	struct device *d;

	d = malloc(ca->ca_devsize, M_DEVBUF, M_WAITOK | M_ZERO);
	if (d == NULL)
		return NULL;
	strlcpy(d->dv_xname, xname, sizeof(d->dv_xname));
	d->dv_parent = parent;
	printf("%s at %s\n", xname, parent->dv_xname);
	(*ca->ca_attach)(parent, d, aux);
	return d;
}

struct device *
config_found_sm(struct device *parent, void *aux, cfprint_t print, cfmatch_t submatch)
{
	struct sdmmc_attach_args *saa = aux;
	struct sdmmc_softc *sc;

	if (parent == tsw_host_dev)
		return attach_as(&sdmmc_ca, parent, "sdmmc0", aux);
	sc = saa->sf != NULL ? saa->sf->sc : NULL;
	if (sc != NULL && sc->sc_fn0 != NULL)
		printf("obsd: SDIO function %d of %d functions, CIS vendor %04x product %04x\n",
		    saa->sf->number, sc->sc_function_count,
		    sc->sc_fn0->cis.manufacturer, sc->sc_fn0->cis.product);
	if (tsw_chip_dev != NULL || !(*bwfm_sdio_ca.ca_match)(parent, NULL, aux))
		return NULL;
	tsw_chip_dev = attach_as(&bwfm_sdio_ca, parent, "bwfm0", aux);
	return tsw_chip_dev;
}

/* Small things of the kernel's the drivers use */
struct device	*rootdv;	/* no booting from the Wi-Fi's host */
int		nowake;
int		tick = 10000;	/* the microseconds below which sdmmc spins rather than sleeps */

void
delay(u_int us)
{
	obsd_hc_delay(us);
}

char *
strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (*s == '\0')
			return NULL;
	}
}

/* Buffers the host reads by PIO: the driver wants them aligned to 4 bytes, as the heap's are */
void *
dma_alloc(size_t size, int flags)
{
	return malloc(size, M_DEVBUF, ((flags & PR_WAITOK) ? M_WAITOK : M_NOWAIT) |
	    ((flags & PR_ZERO) ? M_ZERO : 0));
}

void
dma_free(void *p, size_t size)
{
	free(p, M_DEVBUF, size);
}

/* The chip stays for as long as the machine runs; and nothing runs without the lock */
void
if_detach(struct ifnet *ifp)
{
}

void
ifq_serialize(struct ifqueue *ifq, struct task *t)
{
	(*t->t_func)(t->t_arg);
}

void
taskq_barrier(struct taskq *tq)
{
}

/* The chip's out-of-band interrupt line is not used: the device tree's node is not given to bwfm */
void *
arm_intr_establish_fdt(int node, int level, int (*func)(void *),
    void *arg, char *name)
{
	return NULL;
}

void
arm_intr_enable(void *cookie)
{
}

void
arm_intr_disable(void *cookie)
{
}

/* The SCSI side of sdmmc is for memory cards: an SDIO card has none */
void
sdmmc_scsi_attach(struct sdmmc_softc *sc)
{
	printf("obsd: a memory card on the Wi-Fi's host: left alone\n");
}

void
sdmmc_scsi_detach(struct sdmmc_softc *sc)
{
}
