/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Copyright (c) 2026 satromi
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 *	athx_pci.c (x64 PC, WIFI_NET80211=obsd)
 *	The QCA6174 on PCI (design 9.6.9): its window of registers, kept
 *	awake while the driver runs; the copy engines, rings of descriptors
 *	in host memory that move every message and frame between the host
 *	and the chip; the diagnostic window, a copy engine the host points at
 *	the chip's memory; the bootloader's message exchange on engines 0 and
 *	1; the cold and warm resets the chip needs before each start, and its
 *	one message interrupt.
 *
 *	The engines' configuration is the chip's: what the host programs
 *	(host_ce_config below) and what it writes into the target for the
 *	firmware (target_ce_config, the map of services to pipes). On this
 *	chip engine 5 is not the one HTT's frames come up on: that is engine
 *	1, with HTC's own messages, and engine 5 is the host's out.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/endian.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include "athxreg.h"
#include "athxvar.h"

static void	athx_pci_htc_tx_cb(struct athx_softc *, struct athx_ce *);
static void	athx_pci_htc_rx_cb(struct athx_softc *, struct athx_ce *);
static void	athx_pci_htt_htc_rx_cb(struct athx_softc *, struct athx_ce *);
static void	athx_pci_pktlog_rx_cb(struct athx_softc *, struct athx_ce *);

/* The host's configuration of the engines */
static const struct athx_ce_attr host_ce_config[CE_COUNT] = {
	/* CE0: host->target HTC control and raw streams */
	{ CE_ATTR_FLAGS, 16, 256, 0, athx_pci_htc_tx_cb, NULL },
	/* CE1: target->host HTT + HTC control */
	{ CE_ATTR_FLAGS, 0, 2048, 512, NULL, athx_pci_htt_htc_rx_cb },
	/* CE2: target->host WMI */
	{ CE_ATTR_FLAGS, 0, 2048, 128, NULL, athx_pci_htc_rx_cb },
	/* CE3: host->target WMI */
	{ CE_ATTR_FLAGS, 32, 2048, 0, athx_pci_htc_tx_cb, NULL },
	/* CE4: host->target HTT */
	{ CE_ATTR_FLAGS | CE_ATTR_DIS_INTR, CE_HTT_H2T_MSG_SRC_NENTRIES, 256, 0, athx_htt_tx_cb, NULL },
	/* CE5: not used by the host on this chip (its target side is the host's out) */
	{ CE_ATTR_FLAGS, 0, 0, 0, NULL, NULL },
	/* CE6: target autonomous hif_memcpy */
	{ CE_ATTR_FLAGS, 0, 0, 0, NULL, NULL },
	/* CE7: the diagnostic window */
	{ CE_ATTR_FLAGS | CE_ATTR_POLL, 2, DIAG_TRANSFER_LIMIT, 2, NULL, NULL },
};

/*
 * The host's engine 8 (target->host pktlog) is not among the eight this
 * chip has (CE_COUNT): its target configuration is written, its host side
 * is not set up, as on every chip of this family with eight engines
 */
static void athx_pci_unused(void) { (void)athx_pci_pktlog_rx_cb; }

/* The target firmware's configuration of the engines */
static const struct ce_pipe_config target_ce_config[NUM_TARGET_CE_CONFIG_WLAN] = {
	/* CE0: host->target HTC control and raw streams */
	{ htole32(0), htole32(PIPEDIR_OUT), htole32(32), htole32(256), htole32(CE_ATTR_FLAGS), 0 },
	/* CE1: target->host HTT + HTC control */
	{ htole32(1), htole32(PIPEDIR_IN), htole32(32), htole32(2048), htole32(CE_ATTR_FLAGS), 0 },
	/* CE2: target->host WMI */
	{ htole32(2), htole32(PIPEDIR_IN), htole32(64), htole32(2048), htole32(CE_ATTR_FLAGS), 0 },
	/* CE3: host->target WMI */
	{ htole32(3), htole32(PIPEDIR_OUT), htole32(32), htole32(2048), htole32(CE_ATTR_FLAGS), 0 },
	/* CE4: host->target HTT */
	{ htole32(4), htole32(PIPEDIR_OUT), htole32(256), htole32(256), htole32(CE_ATTR_FLAGS), 0 },
	/* CE5: on this chip, host->target */
	{ htole32(5), htole32(PIPEDIR_OUT), htole32(32), htole32(2048), htole32(CE_ATTR_FLAGS), 0 },
	/* CE6: reserved for target autonomous hif_memcpy */
	{ htole32(6), htole32(PIPEDIR_INOUT), htole32(32), htole32(4096), htole32(CE_ATTR_FLAGS), 0 },
};

/* Which pipe each service's messages take, both ways (the last entry ends the map) */
static const struct ce_service_to_pipe target_service_to_ce_map[] = {
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_VO), htole32(PIPEDIR_OUT), htole32(3) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_VO), htole32(PIPEDIR_IN), htole32(2) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_BK), htole32(PIPEDIR_OUT), htole32(3) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_BK), htole32(PIPEDIR_IN), htole32(2) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_BE), htole32(PIPEDIR_OUT), htole32(3) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_BE), htole32(PIPEDIR_IN), htole32(2) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_VI), htole32(PIPEDIR_OUT), htole32(3) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_DATA_VI), htole32(PIPEDIR_IN), htole32(2) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_CONTROL), htole32(PIPEDIR_OUT), htole32(3) },
	{ htole32(ATH10K_HTC_SVC_ID_WMI_CONTROL), htole32(PIPEDIR_IN), htole32(2) },
	{ htole32(ATH10K_HTC_SVC_ID_RSVD_CTRL), htole32(PIPEDIR_OUT), htole32(0) },
	{ htole32(ATH10K_HTC_SVC_ID_RSVD_CTRL), htole32(PIPEDIR_IN), htole32(1) },
	{ htole32(ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS), htole32(PIPEDIR_OUT), htole32(0) },
	{ htole32(ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS), htole32(PIPEDIR_IN), htole32(1) },
	{ htole32(ATH10K_HTC_SVC_ID_HTT_DATA_MSG), htole32(PIPEDIR_OUT), htole32(4) },
	/* on this chip HTT's frames come up on pipe 1, not 5 */
	{ htole32(ATH10K_HTC_SVC_ID_HTT_DATA_MSG), htole32(PIPEDIR_IN), htole32(1) },
	{ 0, 0, 0 },
};

/* ---------------------------------------------------------------- registers */

static int
athx_device_gone(struct athx_softc *sc)
{
	return bus_space_read_4(sc->sc_st, sc->sc_sh, FW_INDICATOR_ADDRESS) == 0xffffffff;
}

/*
 * The chip is kept awake as long as the driver runs (it is woken once,
 * after each reset): its registers are read and written as they are
 */
uint32_t
athx_read32(struct athx_softc *sc, uint32_t off)
{
	if (off + 4 > sc->sc_sz)
		return 0;
	return bus_space_read_4(sc->sc_st, sc->sc_sh, off);
}

void
athx_write32(struct athx_softc *sc, uint32_t off, uint32_t v)
{
	if (off + 4 > sc->sc_sz)
		return;
	bus_space_write_4(sc->sc_st, sc->sc_sh, off, v);
}

uint32_t
athx_soc_read32(struct athx_softc *sc, uint32_t addr)
{
	return athx_read32(sc, RTC_SOC_BASE_ADDRESS + addr);
}

void
athx_soc_write32(struct athx_softc *sc, uint32_t addr, uint32_t v)
{
	athx_write32(sc, RTC_SOC_BASE_ADDRESS + addr, v);
}

static uint32_t
athx_reg_read32(struct athx_softc *sc, uint32_t addr)
{
	return athx_read32(sc, PCIE_LOCAL_BASE_ADDRESS + addr);
}

static void
athx_reg_write32(struct athx_softc *sc, uint32_t addr, uint32_t v)
{
	athx_write32(sc, PCIE_LOCAL_BASE_ADDRESS + addr, v);
}

static int
athx_is_awake(struct athx_softc *sc)
{
	uint32_t v = athx_read32(sc, PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);

	return ((v & RTC_STATE_V_MASK) >> RTC_STATE_V_LSB) == RTC_STATE_V_ON;
}

/* The SoC woken: 0, or ETIMEDOUT after PCIE_WAKE_TIMEOUT us */
static int
athx_force_wake(struct athx_softc *sc)
{
	int tot = 0, cur = 5;

	athx_write32(sc, PCIE_LOCAL_BASE_ADDRESS + PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
	while (tot < PCIE_WAKE_TIMEOUT) {
		if (athx_is_awake(sc)) {
			if (tot > PCIE_WAKE_LATE_US)
				printf("%s: device wakeup took %d ms\n", sc->sc_dev.dv_xname, tot / 1000);
			return 0;
		}
		delay(cur);
		tot += cur;
		if (cur < 50)
			cur += 5;
	}
	return ETIMEDOUT;
}

/* ---------------------------------------------------------------- memory */

int
athx_dma_alloc(struct athx_softc *sc, struct athx_dma *d, bus_size_t size, bus_size_t align)
{
	int nsegs;

	memset(d, 0, sizeof(*d));
	d->size = size;
	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0, BUS_DMA_NOWAIT, &d->map) != 0)
		return ENOMEM;
	if (bus_dmamem_alloc(sc->sc_dmat, size, align, 0, &d->seg, 1, &nsegs,
	    BUS_DMA_NOWAIT | BUS_DMA_ZERO) != 0)
		goto destroy;
	if (bus_dmamem_map(sc->sc_dmat, &d->seg, nsegs, size, &d->kva,
	    BUS_DMA_NOWAIT | BUS_DMA_COHERENT) != 0)
		goto free;
	if (bus_dmamap_load_raw(sc->sc_dmat, d->map, &d->seg, nsegs, size, BUS_DMA_NOWAIT) != 0)
		goto unmap;
	memset(d->kva, 0, size);
	d->paddr = d->map->dm_segs[0].ds_addr;
	if (d->paddr + size > 0x100000000ULL) {
		printf("%s: DMA memory at %llx is beyond the chip's 32 bits\n", sc->sc_dev.dv_xname,
		    (unsigned long long)d->paddr);
		goto unload;
	}
	return 0;
 unload:
	bus_dmamap_unload(sc->sc_dmat, d->map);
 unmap:
	bus_dmamem_unmap(sc->sc_dmat, d->kva, size);
 free:
	bus_dmamem_free(sc->sc_dmat, &d->seg, 1);
 destroy:
	bus_dmamap_destroy(sc->sc_dmat, d->map);
	memset(d, 0, sizeof(*d));
	return ENOMEM;
}

void
athx_dma_free(struct athx_softc *sc, struct athx_dma *d)
{
	if (d->map == NULL)
		return;
	bus_dmamap_unload(sc->sc_dmat, d->map);
	bus_dmamem_unmap(sc->sc_dmat, d->kva, d->size);
	bus_dmamem_free(sc->sc_dmat, &d->seg, 1);
	bus_dmamap_destroy(sc->sc_dmat, d->map);
	memset(d, 0, sizeof(*d));
}

int
athx_bufs_alloc(struct athx_softc *sc, struct athx_bufs *b, int n, int size)
{
	int i, sz = 4;

	memset(b, 0, sizeof(*b));
	while (sz < size)
		sz <<= 1;
	if (sz > ATHX_CHUNK || n <= 0)
		return EINVAL;
	b->n = n;
	b->size = sz;
	b->per = ATHX_CHUNK / sz;
	b->npieces = (n + b->per - 1) / b->per;
	b->piece = mallocarray(b->npieces, sizeof(*b->piece), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (b->piece == NULL)
		return ENOMEM;
	for (i = 0; i < b->npieces; i++) {
		int last = (i == b->npieces - 1) ? n - i * b->per : b->per;

		if (athx_dma_alloc(sc, &b->piece[i], last * sz, PAGE_SIZE) != 0) {
			athx_bufs_free(sc, b);
			return ENOMEM;
		}
	}
	return 0;
}

void
athx_bufs_free(struct athx_softc *sc, struct athx_bufs *b)
{
	int i;

	if (b->piece == NULL)
		return;
	for (i = 0; i < b->npieces; i++)
		athx_dma_free(sc, &b->piece[i]);
	free(b->piece, M_DEVBUF, b->npieces * sizeof(*b->piece));
	memset(b, 0, sizeof(*b));
}

/* ---------------------------------------------------------------- the copy engines */

static uint32_t
ce_base(int id)
{
	return CE0_BASE_ADDRESS + (CE1_BASE_ADDRESS - CE0_BASE_ADDRESS) * id;
}

static void
ce_rmw(struct athx_softc *sc, uint32_t addr, uint32_t mask, uint32_t val)
{
	athx_write32(sc, addr, (athx_read32(sc, addr) & ~mask) | (val & mask));
}

static struct athx_ce_ring *
ce_ring_alloc(struct athx_softc *sc, unsigned int n)
{
	struct athx_ce_ring *r;
	unsigned int nentries = 1;

	while (nentries < n)
		nentries <<= 1;
	r = malloc(sizeof(*r), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (r == NULL)
		return NULL;
	r->nentries = nentries;
	r->nentries_mask = nentries - 1;
	r->ctx = mallocarray(nentries, sizeof(void *), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (r->ctx == NULL ||
	    athx_dma_alloc(sc, &r->mem, nentries * sizeof(struct ce_desc) + CE_DESC_RING_ALIGN,
	    CE_DESC_RING_ALIGN) != 0) {
		free(r->ctx, M_DEVBUF, nentries * sizeof(void *));
		free(r, M_DEVBUF, sizeof(*r));
		return NULL;
	}
	r->base = (struct ce_desc *)roundup((uintptr_t)r->mem.kva, CE_DESC_RING_ALIGN);
	r->base_paddr = roundup((uint32_t)r->mem.paddr, CE_DESC_RING_ALIGN);
	return r;
}

/* The engines' rings and buffers, once (the rings stay through every restart of the chip) */
static int
athx_ce_alloc(struct athx_softc *sc)
{
	struct athx_ce *ce;
	struct athx_ce_attr *a;
	int i;

	for (i = 0; i < CE_COUNT; i++) {
		ce = &sc->ce[i];
		a = &sc->attr[i];
		ce->id = i;
		ce->ctrl_addr = ce_base(i);
		ce->attr_flags = a->flags;
		ce->src_sz_max = a->src_sz_max;
		if (a->src_nentries) {
			ce->send_cb = a->send_cb;
			ce->src = ce_ring_alloc(sc, a->src_nentries);
			if (ce->src == NULL)
				return ENOMEM;
			/* engines 0 and 3: a buffer of their largest message per entry */
			if ((i == 0 || i == 3) &&
			    athx_bufs_alloc(sc, &ce->src->bufs, ce->src->nentries, a->src_sz_max) != 0)
				return ENOMEM;
		}
		if (a->dest_nentries) {
			ce->recv_cb = a->recv_cb;
			ce->dest = ce_ring_alloc(sc, a->dest_nentries);
			if (ce->dest == NULL)
				return ENOMEM;
			if (i != CE_DIAG_PIPE) {
				ce->buf_sz = a->src_sz_max;
				if (athx_bufs_alloc(sc, &ce->dest->bufs, ce->dest->nentries, ce->buf_sz) != 0)
					return ENOMEM;
			}
		}
	}
	return athx_dma_alloc(sc, &sc->diag_buf, DIAG_TRANSFER_LIMIT, 8);
}

static void
ce_init_src_ring(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->src;
	uint32_t ctrl = ce->ctrl_addr;
	uint32_t n = r->nentries;

	memset(r->base, 0, n * sizeof(struct ce_desc));
	r->sw_index = athx_read32(sc, ctrl + CE_CURRENT_SRRI) & r->nentries_mask;
	r->hw_index = r->sw_index;
	r->write_index = athx_read32(sc, ctrl + CE_SR_WR_INDEX) & r->nentries_mask;
	athx_write32(sc, ctrl + CE_SR_BASE_ADDR_LO, r->base_paddr);
	athx_write32(sc, ctrl + CE_SR_SIZE, n);
	ce_rmw(sc, ctrl + CE_CTRL1, CE_CTRL1_DMAX_MASK, SM(ce->src_sz_max, CE_CTRL1_DMAX));
	ce_rmw(sc, ctrl + CE_CTRL1, CE_CTRL1_SRC_RING_BYTE_SWAP_MASK, 0);
	ce_rmw(sc, ctrl + CE_SRC_WATERMARK, CE_SRC_WATERMARK_LOW_MASK, SM(0, CE_SRC_WATERMARK_LOW));
	ce_rmw(sc, ctrl + CE_SRC_WATERMARK, CE_SRC_WATERMARK_HIGH_MASK, SM(n, CE_SRC_WATERMARK_HIGH));
}

static void
ce_init_dest_ring(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->dest;
	uint32_t ctrl = ce->ctrl_addr;
	uint32_t n = r->nentries;

	memset(r->base, 0, n * sizeof(struct ce_desc));
	memset(r->ctx, 0, n * sizeof(void *));
	r->sw_index = athx_read32(sc, ctrl + CE_CURRENT_DRRI) & r->nentries_mask;
	r->write_index = athx_read32(sc, ctrl + CE_DST_WR_INDEX) & r->nentries_mask;
	athx_write32(sc, ctrl + CE_DR_BASE_ADDR_LO, r->base_paddr);
	athx_write32(sc, ctrl + CE_DR_SIZE, n);
	ce_rmw(sc, ctrl + CE_CTRL1, CE_CTRL1_DST_RING_BYTE_SWAP_MASK, 0);
	ce_rmw(sc, ctrl + CE_DST_WATERMARK, CE_DST_WATERMARK_LOW_MASK, SM(0, CE_DST_WATERMARK_LOW));
	ce_rmw(sc, ctrl + CE_DST_WATERMARK, CE_DST_WATERMARK_HIGH_MASK, SM(n, CE_DST_WATERMARK_HIGH));
}

/* Every engine's rings given to the chip again (after each reset) */
static void
athx_init_pipes(struct athx_softc *sc)
{
	int i;

	for (i = 0; i < CE_COUNT; i++) {
		if (sc->ce[i].src != NULL)
			ce_init_src_ring(sc, &sc->ce[i]);
		if (sc->ce[i].dest != NULL)
			ce_init_dest_ring(sc, &sc->ce[i]);
	}
}

/* One entry of a source ring: 0, or ENOSPC when the ring is full */
static int
ce_send_nolock(struct athx_softc *sc, struct athx_ce *ce, void *ctx, uint32_t paddr,
    unsigned int nbytes, unsigned int transfer_id, unsigned int flags)
{
	struct athx_ce_ring *r = ce->src;
	unsigned int mask = r->nentries_mask, wi = r->write_index;
	struct ce_desc *desc;
	uint16_t dflags = 0;

	if (nbytes > ce->src_sz_max)
		printf("%s: CE%d: sending more than it can (%u > %u)\n", sc->sc_dev.dv_xname, ce->id,
		    nbytes, ce->src_sz_max);
	if (CE_RING_DELTA(mask, wi, r->sw_index - 1) <= 0)
		return ENOSPC;
	desc = &r->base[wi];
	dflags |= SM(transfer_id, CE_DESC_FLAGS_META_DATA);
	if (flags & CE_SEND_FLAG_GATHER)
		dflags |= CE_DESC_FLAGS_GATHER;
	desc->addr = htole32(paddr);
	desc->nbytes = htole16(nbytes);
	desc->flags = htole16(dflags);
	r->ctx[wi] = ctx;
	wi = CE_RING_IDX_INCR(mask, wi);
	bus_dmamap_sync(sc->sc_dmat, r->mem.map, 0, r->mem.size, BUS_DMASYNC_PREWRITE);
	if (!(flags & CE_SEND_FLAG_GATHER))
		athx_write32(sc, ce->ctrl_addr + CE_SR_WR_INDEX, wi);
	r->write_index = wi;
	return 0;
}

static void
ce_send_revert(struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->src;

	if (r->write_index == r->sw_index)
		return;
	r->write_index = (r->write_index - 1) & r->nentries_mask;
	r->ctx[r->write_index] = NULL;
}

/*
 * A message in pieces (the last ends it), all on one engine: 0, or ENOBUFS
 * when there is no room for them all
 */
int
athx_ce_send_sg(struct athx_softc *sc, int pipe, const uint32_t *paddr, const uint16_t *len,
    int n, void *ctx, unsigned int transfer_id)
{
	struct athx_ce *ce = &sc->ce[pipe];
	struct athx_ce_ring *r = ce->src;
	int i, err;

	if (r == NULL)
		return ENXIO;
	if (CE_RING_DELTA(r->nentries_mask, r->write_index, r->sw_index - 1) < n)
		return ENOBUFS;
	for (i = 0; i < n; i++) {
		err = ce_send_nolock(sc, ce, (i == n - 1) ? ctx : NULL, paddr[i], len[i], transfer_id,
		    (i == n - 1) ? 0 : CE_SEND_FLAG_GATHER);
		if (err) {
			for (; i > 0; i--)
				ce_send_revert(ce);
			return err;
		}
	}
	return 0;
}

/* The buffer of the next entry of engine 0's or 3's source ring, to copy a message into */
void *
athx_ce_src_buf(struct athx_softc *sc, int pipe, uint32_t *paddr)
{
	struct athx_ce_ring *r = sc->ce[pipe].src;

	if (r == NULL || r->bufs.n == 0)
		return NULL;
	*paddr = athx_buf_pa(&r->bufs, r->write_index);
	return athx_buf_va(&r->bufs, r->write_index);
}

/* How many entries a source ring has free */
int
athx_ce_free_src(struct athx_softc *sc, int pipe)
{
	struct athx_ce_ring *r = sc->ce[pipe].src;

	if (r == NULL)
		return 0;
	return CE_RING_DELTA(r->nentries_mask, r->write_index, r->sw_index - 1);
}

static int
ce_rx_post(struct athx_softc *sc, struct athx_ce *ce, void *ctx, uint32_t paddr)
{
	struct athx_ce_ring *r = ce->dest;
	unsigned int mask = r->nentries_mask, wi = r->write_index;

	if (CE_RING_DELTA(mask, wi, r->sw_index - 1) == 0)
		return ENOSPC;
	r->base[wi].addr = htole32(paddr);
	r->base[wi].nbytes = 0;
	r->ctx[wi] = ctx;
	wi = CE_RING_IDX_INCR(mask, wi);
	bus_dmamap_sync(sc->sc_dmat, r->mem.map, 0, r->mem.size, BUS_DMASYNC_PREWRITE);
	athx_write32(sc, ce->ctrl_addr + CE_DST_WR_INDEX, wi);
	r->write_index = wi;
	return 0;
}

/* A completed entry of a destination ring: 0 and what it held, or EIO when there is none */
static int
ce_completed_recv(struct athx_softc *sc, struct athx_ce *ce, void **ctx, unsigned int *nbytes,
    unsigned int *index)
{
	struct athx_ce_ring *r = ce->dest;
	unsigned int si = r->sw_index;
	struct ce_desc *desc = &r->base[si];
	uint16_t n;

	bus_dmamap_sync(sc->sc_dmat, r->mem.map, 0, r->mem.size, BUS_DMASYNC_POSTREAD);
	n = le16toh(desc->nbytes);
	if (n == 0)
		return EIO;		/* the chip's index ran ahead of the descriptor's write */
	desc->nbytes = 0;
	*nbytes = n;
	if (ctx != NULL)
		*ctx = r->ctx[si];
	r->ctx[si] = NULL;
	if (index != NULL)
		*index = si;
	r->sw_index = CE_RING_IDX_INCR(r->nentries_mask, si);
	return 0;
}

static int
ce_revoke_recv(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->dest;

	if (r->write_index == r->sw_index)
		return EIO;
	r->ctx[r->sw_index] = NULL;
	r->base[r->sw_index].nbytes = 0;
	r->sw_index = CE_RING_IDX_INCR(r->nentries_mask, r->sw_index);
	return 0;
}

/* A completed entry of a source ring: 0 and what it carried, EIO when there is none */
static int
ce_completed_send(struct athx_softc *sc, struct athx_ce *ce, void **ctx)
{
	struct athx_ce_ring *r = ce->src;
	unsigned int si = r->sw_index;
	uint32_t ri;

	if (r->hw_index == si) {
		ri = athx_read32(sc, ce->ctrl_addr + CE_CURRENT_SRRI);
		if (ri == 0xffffffff)
			return ENODEV;
		r->hw_index = ri & r->nentries_mask;
	}
	if (r->hw_index == si)
		return EIO;
	if (ctx != NULL)
		*ctx = r->ctx[si];
	r->ctx[si] = NULL;
	r->base[si].nbytes = 0;
	r->sw_index = CE_RING_IDX_INCR(r->nentries_mask, si);
	return 0;
}

static int
ce_cancel_send(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->src;

	if (r->write_index == r->sw_index)
		return EIO;
	r->ctx[r->sw_index] = NULL;
	r->sw_index = CE_RING_IDX_INCR(r->nentries_mask, r->sw_index);
	return 0;
}

/* An engine's completions handled, its interrupt's causes cleared first */
void
athx_ce_service(struct athx_softc *sc, int id)
{
	struct athx_ce *ce = &sc->ce[id];

	athx_write32(sc, ce->ctrl_addr + CE_HOST_IS, CE_HOST_IS_CC_MASK | CE_HOST_IS_WM_MASK);
	if (ce->recv_cb != NULL)
		(*ce->recv_cb)(sc, ce);
	if (ce->send_cb != NULL)
		(*ce->send_cb)(sc, ce);
}

static uint32_t
athx_ce_interrupt_summary(struct athx_softc *sc)
{
	uint32_t v = athx_read32(sc, CE_WRAPPER_BASE_ADDRESS + CE_WRAPPER_INTERRUPT_SUMMARY_ADDRESS);

	return (v & CE_WRAPPER_INTERRUPT_SUMMARY_HOST_MSI_MASK) >> CE_WRAPPER_INTERRUPT_SUMMARY_HOST_MSI_LSB;
}

static void
athx_ce_service_any(struct athx_softc *sc)
{
	uint32_t sum = athx_ce_interrupt_summary(sc);
	int id;

	for (id = 0; sum && id < CE_COUNT; id++) {
		if (!(sum & (1 << id)))
			continue;
		sum &= ~(1 << id);
		athx_ce_service(sc, id);
	}
}

/* Every engine looked at whether it interrupted or not */
void
athx_ce_poll_all(struct athx_softc *sc)
{
	int id;

	for (id = 0; id < CE_COUNT; id++)
		if (id != CE_DIAG_PIPE)
			athx_ce_service(sc, id);
}

static void
ce_disable_interrupts(struct athx_softc *sc)
{
	int id;
	uint32_t c;

	for (id = 0; id < CE_COUNT; id++) {
		if (sc->ce[id].attr_flags & CE_ATTR_POLL)
			continue;
		c = ce_base(id);
		ce_rmw(sc, c + CE_HOST_IE, CE_HOST_IE_COPY_COMPLETE_MASK, 0);
		ce_rmw(sc, c + CE_MISC_IE, CE_MISC_IS_ERR_MASK, 0);
		ce_rmw(sc, c + CE_HOST_IE, CE_HOST_IS_WM_MASK, 0);
	}
}

static void
ce_enable_interrupts(struct athx_softc *sc)
{
	struct athx_ce *ce;
	int id;

	for (id = 0; id < CE_COUNT; id++) {
		ce = &sc->ce[id];
		if (ce->attr_flags & CE_ATTR_POLL)
			continue;
		if (!(ce->attr_flags & CE_ATTR_DIS_INTR) && (ce->send_cb != NULL || ce->recv_cb != NULL))
			ce_rmw(sc, ce->ctrl_addr + CE_HOST_IE, CE_HOST_IE_COPY_COMPLETE_MASK,
			    CE_HOST_IE_COPY_COMPLETE_MASK);
		else
			ce_rmw(sc, ce->ctrl_addr + CE_HOST_IE, CE_HOST_IE_COPY_COMPLETE_MASK, 0);
		ce_rmw(sc, ce->ctrl_addr + CE_HOST_IE, CE_HOST_IS_WM_MASK, 0);
	}
}

static void
ce_deinit(struct athx_softc *sc)
{
	uint32_t c;
	int id;

	for (id = 0; id < CE_COUNT; id++) {
		c = ce_base(id);
		athx_write32(sc, c + CE_SR_BASE_ADDR_LO, 0);
		athx_write32(sc, c + CE_SR_SIZE, 0);
		ce_rmw(sc, c + CE_CTRL1, CE_CTRL1_DMAX_MASK, 0);
		ce_rmw(sc, c + CE_SRC_WATERMARK, CE_SRC_WATERMARK_HIGH_MASK, 0);
		athx_write32(sc, c + CE_DR_BASE_ADDR_LO, 0);
		athx_write32(sc, c + CE_DR_SIZE, 0);
		ce_rmw(sc, c + CE_DST_WATERMARK, CE_DST_WATERMARK_HIGH_MASK, 0);
	}
}

/* The receive buffers of an engine posted, as many as its ring has room for */
static void
athx_rx_post_pipe(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->dest;
	unsigned int wi;

	if (r == NULL || ce->buf_sz == 0)
		return;
	for (;;) {
		wi = r->write_index;
		if (ce_rx_post(sc, ce, (void *)(uintptr_t)(wi + 1), athx_buf_pa(&r->bufs, wi)) != 0)
			break;
	}
}

static void
athx_rx_post(struct athx_softc *sc)
{
	int i;

	for (i = 0; i < CE_COUNT; i++)
		if (i != CE_DIAG_PIPE)
			athx_rx_post_pipe(sc, &sc->ce[i]);
}

/* ---------------------------------------------------------------- what the engines carry */

static void
athx_pci_htc_tx_cb(struct athx_softc *sc, struct athx_ce *ce)
{
	while (ce_completed_send(sc, ce, NULL) == 0)
		;
}

/* Each message an engine received, to HTC; its buffer posted again */
static void
athx_pci_process_rx(struct athx_softc *sc, struct athx_ce *ce, int to_htc)
{
	struct athx_ce_ring *r = ce->dest;
	unsigned int nbytes, idx;
	void *ctx;

	while (ce_completed_recv(sc, ce, &ctx, &nbytes, &idx) == 0) {
		uint8_t *buf;

		if (ctx == NULL)
			continue;
		idx = (unsigned int)(uintptr_t)ctx - 1;
		buf = athx_buf_va(&r->bufs, idx);
		bus_dmamap_sync(sc->sc_dmat, r->bufs.piece[idx / r->bufs.per].map, 0,
		    r->bufs.piece[idx / r->bufs.per].size, BUS_DMASYNC_POSTREAD);
		if (nbytes > ce->buf_sz) {
			printf("%s: CE%d: received more than expected (%u > %u)\n", sc->sc_dev.dv_xname,
			    ce->id, nbytes, ce->buf_sz);
		} else if (to_htc) {
			athx_htc_rx(sc, ce, buf, nbytes);
		}
	}
	athx_rx_post_pipe(sc, ce);
}

static void
athx_pci_htc_rx_cb(struct athx_softc *sc, struct athx_ce *ce)
{
	athx_pci_process_rx(sc, ce, 1);
}

/* Engine 4 sends without interrupting: its completions are looked at whenever HTT's frames come */
static void
athx_pci_htt_htc_rx_cb(struct athx_softc *sc, struct athx_ce *ce)
{
	athx_ce_service(sc, 4);
	athx_pci_process_rx(sc, ce, 1);
}

static void
athx_pci_pktlog_rx_cb(struct athx_softc *sc, struct athx_ce *ce)
{
	athx_pci_process_rx(sc, ce, 0);
}

/* ---------------------------------------------------------------- the diagnostic window */

static uint32_t
targ_cpu_to_ce_addr(struct athx_softc *sc, uint32_t addr)
{
	uint32_t val, region = addr & 0xfffff;

	val = (athx_read32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS) & 0x7ff) << 21;
	val |= ((addr >= 0x100000) ? 0x100000 : 0) | region;
	return val;
}

static int
diag_wait(struct athx_softc *sc, struct athx_ce *ce, int recv, void **ctx, unsigned int *nbytes)
{
	int i = 0;

	for (;;) {
		if (recv ? ce_completed_recv(sc, ce, ctx, nbytes, NULL) == 0 :
		    ce_completed_send(sc, ce, NULL) == 0)
			return 0;
		delay(DIAG_ACCESS_CE_WAIT_US);
		i += DIAG_ACCESS_CE_WAIT_US;
		if (i > DIAG_ACCESS_CE_TIMEOUT_US)
			return EBUSY;
	}
}

/* The chip's memory at address read through the diagnostic window: 0 or an errno value */
int
athx_diag_read_mem(struct athx_softc *sc, uint32_t address, void *data, int nbytes)
{
	struct athx_ce *ce = &sc->ce[CE_DIAG_PIPE];
	uint32_t ce_data = (uint32_t)sc->diag_buf.paddr;
	unsigned int done;
	int n, ret;
	void *ctx;

	address = targ_cpu_to_ce_addr(sc, address);
	while (nbytes > 0) {
		n = MIN(nbytes, DIAG_TRANSFER_LIMIT);
		ret = ce_rx_post(sc, ce, (void *)(uintptr_t)ce_data, ce_data);
		if (ret)
			return ret;
		ret = ce_send_nolock(sc, ce, NULL, address, n, 0, 0);
		if (ret)
			return ret;
		if (diag_wait(sc, ce, 0, NULL, NULL) != 0 || diag_wait(sc, ce, 1, &ctx, &done) != 0)
			return EBUSY;
		if (done != (unsigned int)n || (uint32_t)(uintptr_t)ctx != ce_data)
			return EIO;
		bus_dmamap_sync(sc->sc_dmat, sc->diag_buf.map, 0, sc->diag_buf.size, BUS_DMASYNC_POSTREAD);
		memcpy(data, sc->diag_buf.kva, n);
		nbytes -= n;
		address += n;
		data = (uint8_t *)data + n;
	}
	return 0;
}

int
athx_diag_read32(struct athx_softc *sc, uint32_t address, uint32_t *value)
{
	uint32_t v = 0;
	int ret;

	ret = athx_diag_read_mem(sc, address, &v, sizeof(v));
	*value = le32toh(v);
	return ret;
}

/* Bytes written into the chip's memory through the diagnostic window: 0 or an errno value */
int
athx_diag_write_mem(struct athx_softc *sc, uint32_t address, const void *data, int nbytes)
{
	struct athx_ce *ce = &sc->ce[CE_DIAG_PIPE];
	unsigned int done;
	int n, ret;
	void *ctx;

	address = targ_cpu_to_ce_addr(sc, address);
	while (nbytes > 0) {
		n = MIN(nbytes, DIAG_TRANSFER_LIMIT);
		memcpy(sc->diag_buf.kva, data, n);
		bus_dmamap_sync(sc->sc_dmat, sc->diag_buf.map, 0, sc->diag_buf.size, BUS_DMASYNC_PREWRITE);
		ret = ce_rx_post(sc, ce, (void *)(uintptr_t)address, address);
		if (ret)
			return ret;
		ret = ce_send_nolock(sc, ce, NULL, (uint32_t)sc->diag_buf.paddr, n, 0, 0);
		if (ret)
			return ret;
		if (diag_wait(sc, ce, 0, NULL, NULL) != 0 || diag_wait(sc, ce, 1, &ctx, &done) != 0) {
			printf("%s: diag write at %x timed out\n", sc->sc_dev.dv_xname, address);
			return EBUSY;
		}
		if (done != (unsigned int)n || (uint32_t)(uintptr_t)ctx != address)
			return EIO;
		nbytes -= n;
		address += n;
		data = (const uint8_t *)data + n;
	}
	return 0;
}

static int
athx_diag_write32(struct athx_softc *sc, uint32_t address, uint32_t value)
{
	uint32_t v = htole32(value);

	return athx_diag_write_mem(sc, address, &v, sizeof(v));
}

/* ---------------------------------------------------------------- the bootloader's exchange */

/*
 * A request to the bootloader on engine 0 and, when one is expected, its
 * response on engine 1, both waited for by polling the engines (the
 * interrupt is not on yet): 0, ETIMEDOUT after 3 s
 */
int
athx_bmi_exchange(struct athx_softc *sc, const void *req, uint32_t req_len, void *resp,
    uint32_t *resp_len)
{
	struct athx_ce *tx = &sc->ce[BMI_CE_NUM_TO_TARG];
	struct athx_ce *rx = &sc->ce[BMI_CE_NUM_TO_HOST];
	uint64_t until = getnsecuptime() + 3000000000ULL;
	int tx_done = 0, rx_done = 0, ret;
	unsigned int got = 0;
	void *ctx;

	if (resp != NULL && (resp_len == NULL || *resp_len == 0))
		return EINVAL;
	if (req_len > sc->bmi_req.size || (resp != NULL && *resp_len > sc->bmi_resp.size))
		return EINVAL;
	memcpy(sc->bmi_req.kva, req, req_len);
	bus_dmamap_sync(sc->sc_dmat, sc->bmi_req.map, 0, sc->bmi_req.size, BUS_DMASYNC_PREWRITE);
	if (resp != NULL) {
		memset(sc->bmi_resp.kva, 0, *resp_len);
		ce_rx_post(sc, rx, (void *)&sc->bmi_resp, (uint32_t)sc->bmi_resp.paddr);
	}
	ret = ce_send_nolock(sc, tx, (void *)&sc->bmi_req, (uint32_t)sc->bmi_req.paddr, req_len, 0, 0);
	if (ret)
		goto out;
	ret = ETIMEDOUT;
	while (getnsecuptime() < until) {
		if (!tx_done && ce_completed_send(sc, tx, NULL) == 0)
			tx_done = 1;
		if (resp != NULL && !rx_done && ce_completed_recv(sc, rx, &ctx, &got, NULL) == 0)
			rx_done = 1;
		if (tx_done && (resp == NULL || rx_done)) {
			ret = 0;
			break;
		}
		delay(5);
	}
	if (ret)
		ce_cancel_send(sc, tx);
 out:
	if (resp != NULL) {
		if (!rx_done)
			ce_revoke_recv(sc, rx);
		if (ret == 0) {
			bus_dmamap_sync(sc->sc_dmat, sc->bmi_resp.map, 0, sc->bmi_resp.size, BUS_DMASYNC_POSTREAD);
			*resp_len = MIN(*resp_len, got);
			memcpy(resp, sc->bmi_resp.kva, *resp_len);
		}
	}
	return ret;
}

/* ---------------------------------------------------------------- the interrupt */

static void
athx_disable_and_clear_intx(struct athx_softc *sc)
{
	athx_write32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS, 0);
	athx_write32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_CLR_ADDRESS,
	    PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
	(void)athx_read32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS);
}

static void
athx_enable_intx(struct athx_softc *sc)
{
	athx_write32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS,
	    PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
	(void)athx_read32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS);
}

static void
athx_msi_fw_mask(struct athx_softc *sc)
{
	uint32_t v = athx_read32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);

	athx_write32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS, v & ~CORE_CTRL_PCIE_REG_31_MASK);
}

static void
athx_msi_fw_unmask(struct athx_softc *sc)
{
	uint32_t v = athx_read32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);

	athx_write32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS, v | CORE_CTRL_PCIE_REG_31_MASK);
}

static void
athx_irq_disable(struct athx_softc *sc)
{
	ce_disable_interrupts(sc);
	athx_disable_and_clear_intx(sc);
	athx_msi_fw_mask(sc);
}

static void
athx_irq_enable(struct athx_softc *sc)
{
	ce_enable_interrupts(sc);
	athx_enable_intx(sc);
	athx_msi_fw_unmask(sc);
}

static int
athx_fw_crashed(struct athx_softc *sc)
{
	return (athx_read32(sc, FW_INDICATOR_ADDRESS) & FW_IND_EVENT_PENDING) != 0;
}

/*
 * The message interrupt: masked, every engine that interrupted served,
 * HTT's frames and completions handled, the engines looked at again until
 * none is pending, unmasked
 */
int
athx_intr(void *arg)
{
	struct athx_softc *sc = arg;
	int loops;

	if (athx_device_gone(sc))
		return 0;
	athx_disable_and_clear_intx(sc);
	athx_msi_fw_mask(sc);
	if (athx_fw_crashed(sc)) {
		uint32_t v = athx_read32(sc, FW_INDICATOR_ADDRESS);

		athx_write32(sc, FW_INDICATOR_ADDRESS, v & ~FW_IND_EVENT_PENDING);
		if (!sc->wedged)
			printf("%s: FIRMWARE CRASHED (indicator %08x)\n", sc->sc_dev.dv_xname, v);
		sc->wedged = 1;
		wakeup(&sc->ctl_resp_done);
		return 1;
	}
	for (loops = 0; loops < 16; loops++) {
		athx_ce_service_any(sc);
		athx_htt_txrx_compl_task(sc);
		if (!athx_ce_interrupt_summary(sc))
			break;
	}
	athx_enable_intx(sc);
	athx_msi_fw_unmask(sc);
	return 1;
}

/* ---------------------------------------------------------------- resets */

static int
athx_wait_for_target_init(struct athx_softc *sc)
{
	uint64_t until = getnsecuptime() + 3000ULL * 1000000ULL;	/* 3000 ms */
	uint32_t val;

	do {
		val = athx_read32(sc, FW_INDICATOR_ADDRESS);
		if (val == 0xffffffff)
			continue;
		if (val & (FW_IND_EVENT_PENDING | FW_IND_INITIALIZED))
			break;
		delay(10000);
	} while (getnsecuptime() < until);
	athx_disable_and_clear_intx(sc);
	athx_msi_fw_mask(sc);
	if (val == 0xffffffff) {
		printf("%s: failed to read device register, device is gone\n", sc->sc_dev.dv_xname);
		return EIO;
	}
	if (val & FW_IND_EVENT_PENDING) {
		printf("%s: device has crashed during init\n", sc->sc_dev.dv_xname);
		return EIO;
	}
	if (!(val & FW_IND_INITIALIZED)) {
		printf("%s: failed to receive initialized event from target: %08x\n",
		    sc->sc_dev.dv_xname, val);
		return ETIMEDOUT;
	}
	return 0;
}

static int
athx_cold_reset(struct athx_softc *sc)
{
	uint32_t val;

	/* the target, PCIe with it, into reset; its PCIe is not to be touched for 20 ms after */
	val = athx_reg_read32(sc, SOC_GLOBAL_RESET_ADDRESS);
	val |= 1;
	athx_reg_write32(sc, SOC_GLOBAL_RESET_ADDRESS, val);
	delay(20000);
	val &= ~1;
	athx_reg_write32(sc, SOC_GLOBAL_RESET_ADDRESS, val);
	delay(20000);
	return 0;
}

/* Clears the target memory controller's assert line */
static void
athx_warm_reset_si0(struct athx_softc *sc)
{
	uint32_t val;

	val = athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	athx_soc_write32(sc, SOC_RESET_CONTROL_ADDRESS, val | SOC_RESET_CONTROL_SI0_RST_MASK);
	(void)athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	delay(10000);
	val = athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	athx_soc_write32(sc, SOC_RESET_CONTROL_ADDRESS, val & ~SOC_RESET_CONTROL_SI0_RST_MASK);
	(void)athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	delay(10000);
}

static void
athx_warm_reset_cpu(struct athx_softc *sc)
{
	uint32_t val;

	athx_write32(sc, FW_INDICATOR_ADDRESS, 0);
	val = athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	athx_soc_write32(sc, SOC_RESET_CONTROL_ADDRESS, val | SOC_RESET_CONTROL_CPU_WARM_RST_MASK);
}

static void
athx_warm_reset_ce(struct athx_softc *sc)
{
	uint32_t val;

	val = athx_soc_read32(sc, SOC_RESET_CONTROL_ADDRESS);
	athx_soc_write32(sc, SOC_RESET_CONTROL_ADDRESS, val | SOC_RESET_CONTROL_CE_RST_MASK);
	delay(10000);
	athx_soc_write32(sc, SOC_RESET_CONTROL_ADDRESS, val & ~SOC_RESET_CONTROL_CE_RST_MASK);
}

static void
athx_warm_reset_clear_lf(struct athx_softc *sc)
{
	uint32_t val;

	val = athx_soc_read32(sc, SOC_LF_TIMER_CONTROL0_ADDRESS);
	athx_soc_write32(sc, SOC_LF_TIMER_CONTROL0_ADDRESS, val & ~SOC_LF_TIMER_CONTROL0_ENABLE_MASK);
}

/*
 * The warm reset: the target's CPU kept from touching the copy engines
 * while they are reset (a CE reset under the CPU's access can stop the
 * host's PCIe altogether)
 */
static int
athx_warm_reset(struct athx_softc *sc)
{
	int ret;

	athx_irq_disable(sc);
	athx_warm_reset_si0(sc);
	athx_warm_reset_cpu(sc);
	athx_init_pipes(sc);
	(void)athx_wait_for_target_init(sc);
	athx_warm_reset_clear_lf(sc);
	athx_warm_reset_ce(sc);
	athx_warm_reset_cpu(sc);
	athx_init_pipes(sc);
	ret = athx_wait_for_target_init(sc);
	if (ret)
		printf("%s: failed to wait for target init: %d\n", sc->sc_dev.dv_xname, ret);
	return ret;
}

/* This chip needs a cold reset and a warm one, in that order */
static int
athx_chip_reset(struct athx_softc *sc)
{
	int ret;

	ret = athx_cold_reset(sc);
	if (ret)
		return ret;
	(void)athx_force_wake(sc);
	ret = athx_wait_for_target_init(sc);
	if (ret) {
		printf("%s: failed to wait for target after cold reset: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_warm_reset(sc);
	if (ret)
		printf("%s: failed to warm reset: %d\n", sc->sc_dev.dv_xname, ret);
	return ret;
}

/* ---------------------------------------------------------------- the target's configuration */

/*
 * The engines' configuration and the map of services written into the
 * target, its PCIe L1 turned off, its first IRAM banks switched in, and
 * the target told to go on with its initialisation
 */
static int
athx_init_config(struct athx_softc *sc)
{
	uint32_t interconnect, pcie_state = 0, pipe_cfg = 0, svc_map = 0, flags = 0, ealloc, flag2;
	uint32_t a;
	int ret;

	interconnect = QCA988X_HOST_INTEREST_ADDRESS + HI_ITEM(hi_interconnect_state);
	ret = athx_diag_read32(sc, interconnect, &pcie_state);
	if (ret || pcie_state == 0) {
		printf("%s: failed to get pcie state addr: %d %x\n", sc->sc_dev.dv_xname, ret, pcie_state);
		return ret ? ret : EIO;
	}
	ret = athx_diag_read32(sc, pcie_state + offsetof(struct pcie_state, pipe_cfg_addr), &pipe_cfg);
	if (ret || pipe_cfg == 0) {
		printf("%s: failed to get pipe cfg addr: %d\n", sc->sc_dev.dv_xname, ret);
		return ret ? ret : EIO;
	}
	ret = athx_diag_write_mem(sc, pipe_cfg, sc->pipe_config, sizeof(sc->pipe_config));
	if (ret) {
		printf("%s: failed to write pipe cfg: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_diag_read32(sc, pcie_state + offsetof(struct pcie_state, svc_to_pipe_map), &svc_map);
	if (ret || svc_map == 0) {
		printf("%s: failed to get svc/pipe map: %d\n", sc->sc_dev.dv_xname, ret);
		return ret ? ret : EIO;
	}
	ret = athx_diag_write_mem(sc, svc_map, sc->serv_to_pipe, sizeof(target_service_to_ce_map));
	if (ret) {
		printf("%s: failed to write svc/pipe map: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	a = pcie_state + offsetof(struct pcie_state, config_flags);
	ret = athx_diag_read32(sc, a, &flags);
	if (ret == 0)
		ret = athx_diag_write32(sc, a, flags & ~PCIE_CONFIG_FLAG_ENABLE_L1);
	if (ret) {
		printf("%s: failed to set pcie config_flags: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	a = QCA988X_HOST_INTEREST_ADDRESS + HI_ITEM(hi_early_alloc);
	ret = athx_diag_read32(sc, a, &ealloc);
	if (ret == 0) {
		ealloc |= (HI_EARLY_ALLOC_MAGIC << HI_EARLY_ALLOC_MAGIC_SHIFT) & HI_EARLY_ALLOC_MAGIC_MASK;
		ealloc |= (QCA6174_HW_3_IRAM_BANKS << HI_EARLY_ALLOC_IRAM_BANKS_SHIFT) &
		    HI_EARLY_ALLOC_IRAM_BANKS_MASK;
		ret = athx_diag_write32(sc, a, ealloc);
	}
	if (ret) {
		printf("%s: failed to set early alloc val: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	a = QCA988X_HOST_INTEREST_ADDRESS + HI_ITEM(hi_option_flag2);
	ret = athx_diag_read32(sc, a, &flag2);
	if (ret == 0)
		ret = athx_diag_write32(sc, a, flag2 | HI_OPTION_EARLY_CFG_DONE);
	if (ret)
		printf("%s: failed to set option val: %d\n", sc->sc_dev.dv_xname, ret);
	return ret;
}

/* The target's CPU interrupted so that it notices what changed */
static void
athx_wake_target_cpu(struct athx_softc *sc)
{
	uint32_t a = SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS;

	athx_write32(sc, a, athx_read32(sc, a) | CORE_CTRL_CPU_INTR_MASK);
}

/* The link's ASPM, as it was found (the link is kept from L0s and L1 while the chip starts) */
static pcireg_t athx_lnkctl;
static int athx_pcie_off;

static void
athx_aspm_off(struct athx_softc *sc)
{
	pcireg_t v;

	if (athx_pcie_off == 0)
		return;
	v = pci_conf_read(sc->sc_pc, sc->sc_tag, athx_pcie_off + PCI_PCIE_LCSR);
	athx_lnkctl = v;
	pci_conf_write(sc->sc_pc, sc->sc_tag, athx_pcie_off + PCI_PCIE_LCSR,
	    v & ~(PCI_PCIE_LCSR_ASPM_L0S | PCI_PCIE_LCSR_ASPM_L1));
}

static void
athx_aspm_restore(struct athx_softc *sc)
{
	pcireg_t v;

	if (athx_pcie_off == 0)
		return;
	v = pci_conf_read(sc->sc_pc, sc->sc_tag, athx_pcie_off + PCI_PCIE_LCSR);
	v &= ~(PCI_PCIE_LCSR_ASPM_L0S | PCI_PCIE_LCSR_ASPM_L1);
	v |= athx_lnkctl & (PCI_PCIE_LCSR_ASPM_L0S | PCI_PCIE_LCSR_ASPM_L1);
	pci_conf_write(sc->sc_pc, sc->sc_tag, athx_pcie_off + PCI_PCIE_LCSR, v);
}

/*
 * The chip brought up cleanly: reset, its engines given to it, their
 * configuration written into the target, its CPU woken. 0 or an errno
 * value; the stage reached says where it stopped
 */
int
athx_pci_power_up(struct athx_softc *sc)
{
	int ret;

	athx_aspm_off(sc);
	ret = athx_chip_reset(sc);
	if (ret) {
		if (athx_fw_crashed(sc))
			printf("%s: firmware crashed during chip reset\n", sc->sc_dev.dv_xname);
		printf("%s: failed to reset chip: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	sc->chip_id = athx_soc_read32(sc, SOC_CHIP_ID_ADDRESS);
	sc->stage = ATHX_ST_RESET;
	athx_init_pipes(sc);
	ret = athx_init_config(sc);
	if (ret) {
		printf("%s: failed to setup init config: %d\n", sc->sc_dev.dv_xname, ret);
		ce_deinit(sc);
		return ret;
	}
	athx_wake_target_cpu(sc);
	sc->stage = ATHX_ST_PIPES;
	return 0;
}

/* The engines' interrupts and the message interrupt on, the receive buffers posted */
int
athx_pci_start(struct athx_softc *sc)
{
	athx_irq_enable(sc);
	athx_rx_post(sc);
	athx_aspm_restore(sc);
	return 0;
}

/* The chip stopped: its interrupt off, and a reset so that it touches host memory no more */
void
athx_pci_stop(struct athx_softc *sc)
{
	athx_irq_disable(sc);
	(void)athx_warm_reset(sc);
}

/* The service's pipes, both ways: 0, or ENOENT when the map has none */
int
athx_map_service_to_pipe(struct athx_softc *sc, uint16_t service_id, int *ul, int *dl)
{
	const struct ce_service_to_pipe *e;
	int i, ul_set = 0, dl_set = 0;

	for (i = 0; i < sc->nserv_to_pipe; i++) {
		e = &sc->serv_to_pipe[i];
		if (le32toh(e->service_id) != service_id)
			continue;
		switch (le32toh(e->pipedir)) {
		case PIPEDIR_IN:
			*dl = le32toh(e->pipenum);
			dl_set = 1;
			break;
		case PIPEDIR_OUT:
			*ul = le32toh(e->pipenum);
			ul_set = 1;
			break;
		case PIPEDIR_INOUT:
			*dl = *ul = le32toh(e->pipenum);
			dl_set = ul_set = 1;
			break;
		}
	}
	return (ul_set && dl_set) ? 0 : ENOENT;
}

/* The chip's registers as they stand, for KERNEL.TXT */
void
athx_say_regs(struct athx_softc *sc, const char *why)
{
	int i;

	printf("%s: regs (%s): stage %d, FW_IND %08x, RTC_STATE %08x, CORE_CTRL %08x, INTR_EN %08x "
	    "CAUSE %08x, CE summary %02x, chip %08x\n", sc->sc_dev.dv_xname, why, sc->stage,
	    athx_read32(sc, FW_INDICATOR_ADDRESS),
	    athx_read32(sc, PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS),
	    athx_read32(sc, SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS),
	    athx_read32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS),
	    athx_read32(sc, SOC_CORE_BASE_ADDRESS + PCIE_INTR_CAUSE_ADDRESS),
	    athx_ce_interrupt_summary(sc), sc->chip_id);
	for (i = 0; i < CE_COUNT; i++) {
		uint32_t c = ce_base(i);

		if (sc->ce[i].src == NULL && sc->ce[i].dest == NULL)
			continue;
		printf("%s: regs:   CE%d SR wr %u rd %u (host %u/%u) DR wr %u rd %u (host %u/%u) IE %08x IS %08x\n",
		    sc->sc_dev.dv_xname, i, athx_read32(sc, c + CE_SR_WR_INDEX),
		    athx_read32(sc, c + CE_CURRENT_SRRI),
		    sc->ce[i].src ? sc->ce[i].src->write_index : 0, sc->ce[i].src ? sc->ce[i].src->sw_index : 0,
		    athx_read32(sc, c + CE_DST_WR_INDEX), athx_read32(sc, c + CE_CURRENT_DRRI),
		    sc->ce[i].dest ? sc->ce[i].dest->write_index : 0,
		    sc->ce[i].dest ? sc->ce[i].dest->sw_index : 0,
		    athx_read32(sc, c + CE_HOST_IE), athx_read32(sc, c + CE_HOST_IS));
	}
}

/* ---------------------------------------------------------------- attaching */

/*
 * The card claimed: its window mapped, its message interrupt set up, the
 * engines' rings and buffers allocated, the chip woken and its engines
 * quiet. 0 or an errno value
 */
int
athx_pci_attach(struct athx_softc *sc, struct pci_attach_args *pa)
{
	pci_intr_handle_t ih;
	pcireg_t memtype, reg;
	int off;

	athx_pci_unused();
	sc->sc_dmat = pa->pa_dmat;
	sc->sc_pc = pa->pa_pc;
	sc->sc_tag = pa->pa_tag;
	sc->sc_vendor = PCI_VENDOR(pa->pa_id);
	sc->sc_device = PCI_PRODUCT(pa->pa_id);
	reg = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_SUBSYS_ID_REG);
	sc->sc_subvendor = PCI_VENDOR(reg);
	sc->sc_subdevice = PCI_PRODUCT(reg);

	if (pci_get_capability(pa->pa_pc, pa->pa_tag, PCI_CAP_PCIEXPRESS, &off, NULL))
		athx_pcie_off = off;
	reg = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	reg |= PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_MASTER_ENABLE;
	pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, reg);

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);
	if (pci_mapreg_map(pa, PCI_MAPREG_START, memtype, 0, &sc->sc_st, &sc->sc_sh, NULL,
	    &sc->sc_sz, 0)) {
		printf(": can't map mem space\n");
		return ENXIO;
	}
	if (pci_intr_map_msi(pa, &ih) && pci_intr_map(pa, &ih)) {
		printf(": can't map interrupt\n");
		return ENXIO;
	}
	sc->sc_ih = pci_intr_establish(pa->pa_pc, ih, IPL_NET, athx_intr, sc, sc->sc_dev.dv_xname);
	if (sc->sc_ih == NULL) {
		printf(": can't establish interrupt\n");
		return ENXIO;
	}
	printf(", %s\n", pci_intr_string(pa->pa_pc, ih));

	memcpy(sc->attr, host_ce_config, sizeof(sc->attr));
	memcpy(sc->pipe_config, target_ce_config, sizeof(sc->pipe_config));
	memcpy(sc->serv_to_pipe, target_service_to_ce_map, sizeof(target_service_to_ce_map));
	sc->nserv_to_pipe = nitems(target_service_to_ce_map);
	if (athx_ce_alloc(sc) != 0 ||
	    athx_dma_alloc(sc, &sc->bmi_req, BMI_MAX_CMDBUF_SIZE, 8) != 0 ||
	    athx_dma_alloc(sc, &sc->bmi_resp, BMI_MAX_CMDBUF_SIZE, 8) != 0) {
		printf("%s: no DMA memory for the copy engines\n", sc->sc_dev.dv_xname);
		return ENOMEM;
	}
	sc->stage = ATHX_ST_PCI;
	if (athx_force_wake(sc) != 0) {
		printf("%s: failed to wake up device\n", sc->sc_dev.dv_xname);
		return EIO;
	}
	ce_deinit(sc);
	athx_irq_disable(sc);
	return 0;
}
