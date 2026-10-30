/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tswifi_sdio.c
 *	The Wi-Fi's SDIO host inside the rump kernel (Raspberry Pi 5).
 *
 *	This file is compiled with the NetBSD kernel headers into the rump
 *	kernel when the aarch64 build has the Wi-Fi (tools/netbsd/build.sh,
 *	WIFI=1). The Wi-Fi chip (CYW43455) is on an SD host of its own, apart
 *	from the boot card's; NetBSD's SD Host Controller driver (sdhc) runs
 *	that host here, its SD/SDIO layer (sdmmc) brings the card up, and
 *	bwfm(4) attaches to the card's functions. This file gives them what a
 *	NetBSD kernel would around them:
 *
 *	- the host's device ("tssdhc") under the rump kernel's root bus, with
 *	  its registers mapped by the hypercalls (wifi_user_rpi5.c). A machine
 *	  whose hypercalls know no such host has no device at all.
 *	- bus_space over the mapping, and bus_dma for memory the host may
 *	  reach. The Pi 5's bus masters do not snoop the caches, so a map's
 *	  synchronisation cleans and invalidates them through a hypercall.
 *	  The host starts without DMA (PIO through the data port).
 *	- the host's interrupt: the hypercalls take the level interrupt of
 *	  the GIC and keep it masked until a thread of the rump kernel has run
 *	  sdhc's handler at the network's level.
 *	- the firmware loader (dev/firmload.h), reading the firmware, the
 *	  regulatory data and the board's settings from the objects of the
 *	  system volume.
 *	- the chip's own supplicant, the protection of management frames and
 *	  WPA3-Personal (SAE), and the chip's roaming: bwfm calls
 *	  tswifi_bwfm_join just before a join and tswifi_bwfm_event for each
 *	  of the chip's events (tools/netbsd/patches/bwfm-join-hooks.patch),
 *	  and the manager asks through rump_tswifi_fw_* (09-network.md 9.6.8).
 */

#include <sys/param.h>
#include <sys/atomic.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/pool.h>
#include <sys/socket.h>
#include <sys/workqueue.h>

#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net80211/ieee80211_var.h>

#include <dev/ic/bwfmreg.h>
#include <dev/ic/bwfmvar.h>
#include <dev/sdmmc/sdhcreg.h>
#include <dev/sdmmc/sdhcvar.h>
#include <dev/sdmmc/sdmmcvar.h>
#include <dev/firmload.h>

#include "ioconf.c"

#include <rump-sys/kern.h>
#include <rump-sys/dev.h>

/* The hypercalls of the host (wifi_user_rpi5.c) */
int	rumpcomp_tswifi_host(unsigned long *, unsigned long *, unsigned int *, unsigned int *);
void	*rumpcomp_tswifi_map(unsigned long, unsigned long);
int	rumpcomp_tswifi_intr_wait(unsigned int);
void	rumpcomp_tswifi_intr_done(unsigned int);
void	*rumpcomp_tswifi_dmalloc(size_t, size_t);
void	rumpcomp_tswifi_dmafree(void *, size_t);
unsigned long rumpcomp_tswifi_va2pa(void *);
void	rumpcomp_tswifi_dmasync(void *, size_t, int);
int	rumpcomp_tswifi_fw_open(const char *, void **, size_t *);
void	rumpcomp_tswifi_fw_close(void *, size_t);
void	rumpcomp_tswifi_say(const char *);

/* The kinds of cache work rumpcomp_tswifi_dmasync is asked for */
#define TSWIFI_SYNC_CLEAN	1	/* the CPU's writes out to memory */
#define TSWIFI_SYNC_INVAL	2	/* the CPU's copies dropped */

/* ---------------------------------------------------------------- the host */

struct tssdhc_softc {
	struct sdhc_softc	sc;
	struct sdhc_host	*sc_hosts[1];
	unsigned int		sc_intr;	/* the hypercalls' cookie of the interrupt */
	struct lwp		*sc_ilwp;
};

static int	tssdhc_match(device_t, cfdata_t, void *);
static void	tssdhc_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(tssdhc, sizeof(struct tssdhc_softc),
    tssdhc_match, tssdhc_attach, NULL, NULL);

/* What the hypercalls say of the host, given to the attachment */
struct tssdhc_attach_args {
	unsigned long	ta_pa;
	unsigned long	ta_size;
	unsigned int	ta_clk_khz;
	unsigned int	ta_intr;
};

RUMP_COMPONENT(RUMP_COMPONENT_DEV)
{

	config_init_component(cfdriver_ioconf_tssdhc,
	    cfattach_ioconf_tssdhc, cfdata_ioconf_tssdhc);
	/* what the drivers say of each step of the bring-up (aprint_verbose) */
	boothowto |= AB_VERBOSE;
}

RUMP_COMPONENT(RUMP_COMPONENT_DEV_AFTERMAINBUS)
{
	struct tssdhc_attach_args ta;
	device_t mainbus;

	memset(&ta, 0, sizeof(ta));
	if (rumpcomp_tswifi_host(&ta.ta_pa, &ta.ta_size, &ta.ta_clk_khz,
	    &ta.ta_intr) != 0)
		return;			/* no Wi-Fi host on this machine */

	mainbus = device_find_by_driver_unit("mainbus", 0);
	if (mainbus == NULL) {
		rumpcomp_tswifi_say("wifi: no root bus to put the SDIO host under\n");
		return;
	}
	config_found(mainbus, &ta, NULL, CFARGS(.iattr = "tssdhcbus"));
}

static int
tssdhc_match(device_t parent, cfdata_t cf, void *aux)
{

	return 1;
}

/*
 * The thread of the host's interrupt: it waits outside the rump kernel,
 * runs sdhc's handler as the interrupt would, holding the big lock at
 * the network's level, and then lets the line be taken again.
 */
static void
tssdhc_intr_thread(void *v)
{
	struct tssdhc_softc *tsc = v;
	int s;

	for (;;) {
		if (rumpcomp_tswifi_intr_wait(tsc->sc_intr) != 0)
			continue;
		s = splnet();
		KERNEL_LOCK(1, NULL);
		(void)sdhc_intr(&tsc->sc);
		KERNEL_UNLOCK_ONE(NULL);
		splx(s);
		rumpcomp_tswifi_intr_done(tsc->sc_intr);
	}
}

static void
tssdhc_attach(device_t parent, device_t self, void *aux)
{
	struct tssdhc_softc *tsc = device_private(self);
	struct sdhc_softc *sc = &tsc->sc;
	struct tssdhc_attach_args *ta = aux;
	bus_space_handle_t bsh;

	aprint_naive("\n");
	aprint_normal(": SD host of the Wi-Fi, %u kHz\n", ta->ta_clk_khz);

	sc->sc_dev = self;
	sc->sc_host = tsc->sc_hosts;
	sc->sc_dmat = (bus_dma_tag_t)0x20;
	/*
	 * 32-bit accesses only (the BCM2712's host takes no narrower
	 * ones), a card that is always there, the base clock the device
	 * tree gives rather than the capabilities' field, the signals kept
	 * at 3.3V, and no DMA to begin with
	 */
	sc->sc_flags = SDHC_FLAG_32BIT_ACCESS | SDHC_FLAG_NON_REMOVABLE |
	    SDHC_FLAG_NO_CLKBASE | SDHC_FLAG_NO_1_8_V;
	sc->sc_clkbase = ta->ta_clk_khz;
	tsc->sc_intr = ta->ta_intr;

	if (bus_space_map((bus_space_tag_t)1, ta->ta_pa, ta->ta_size, 0, &bsh) != 0) {
		aprint_error_dev(self, "the registers could not be mapped\n");
		return;
	}
	if (kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, tssdhc_intr_thread,
	    tsc, &tsc->sc_ilwp, "%s intr", device_xname(self)) != 0) {
		aprint_error_dev(self, "no thread for the interrupt\n");
		return;
	}
	if (sdhc_host_found(sc, (bus_space_tag_t)1, bsh, ta->ta_size) != 0) {
		aprint_error_dev(self, "the host did not come up\n");
		return;
	}
}

/* ---------------------------------------------------------------- bus_space */

/*
 * One space: the memory the hypercalls map. The handle is the virtual
 * address of the mapping, the accesses are of device memory, ordered by
 * the barriers of the accessors.
 */
int
bus_space_map(bus_space_tag_t bst, bus_addr_t address, bus_size_t size,
    int flags, bus_space_handle_t *handlep)
{
	void *va;

	va = rumpcomp_tswifi_map(address, size);
	if (va == NULL)
		return EINVAL;
	*handlep = (bus_space_handle_t)va;
	return 0;
}

void
bus_space_unmap(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t size)
{

	/* the host's registers stay mapped for as long as the machine runs */
}

int
bus_space_subregion(bus_space_tag_t bst, bus_space_handle_t bsh,
    bus_size_t offset, bus_size_t size, bus_space_handle_t *nbshp)
{

	*nbshp = bsh + offset;
	return 0;
}

/* A read completes before what follows it; a write is made after what came before */
#define TSWIFI_RD(v, bsh, offset, type) do {				\
	(v) = *(volatile type *)((bsh) + (offset));			\
	__asm volatile("dmb oshld" ::: "memory");			\
} while (0)
#define TSWIFI_WR(bsh, offset, type, v) do {				\
	__asm volatile("dmb oshst" ::: "memory");			\
	*(volatile type *)((bsh) + (offset)) = (v);			\
} while (0)

uint8_t
bus_space_read_1(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset)
{
	uint8_t v;

	TSWIFI_RD(v, bsh, offset, uint8_t);
	return v;
}

uint16_t
bus_space_read_2(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset)
{
	uint16_t v;

	TSWIFI_RD(v, bsh, offset, uint16_t);
	return v;
}

uint32_t
bus_space_read_4(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset)
{
	uint32_t v;

	TSWIFI_RD(v, bsh, offset, uint32_t);
	return v;
}

void
bus_space_write_1(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset,
    uint8_t v)
{

	TSWIFI_WR(bsh, offset, uint8_t, v);
}

void
bus_space_write_2(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset,
    uint16_t v)
{

	TSWIFI_WR(bsh, offset, uint16_t, v);
}

void
bus_space_write_4(bus_space_tag_t bst, bus_space_handle_t bsh, bus_size_t offset,
    uint32_t v)
{

	TSWIFI_WR(bsh, offset, uint32_t, v);
}

void
bus_space_barrier(bus_space_tag_t bst, bus_space_handle_t bsh,
    bus_size_t offset, bus_size_t len, int flags)
{

	__asm volatile("dsb osh" ::: "memory");
}

/* ---------------------------------------------------------------- bus_dma */

/*
 * Memory the host may reach comes from the hypercalls (the kernel's
 * pages below 4 GB); a map gives the physical address of each page of a
 * buffer of the kernel's, and its synchronisation does the cache work
 * the direction asks for: out to memory before the device reads, the
 * CPU's copies dropped before and after the device writes.
 */
int
bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamp)
{
	bus_dmamap_t map;
	size_t mapsize;

	mapsize = sizeof(*map) + sizeof(bus_dma_segment_t) * (nsegments - 1);
	map = kmem_intr_zalloc(mapsize,
	    (flags & BUS_DMA_NOWAIT) ? KM_NOSLEEP : KM_SLEEP);
	if (map == NULL)
		return ENOMEM;
	map->_dm_size = size;
	map->_dm_segcnt = nsegments;
	map->_dm_maxmaxsegsz = maxsegsz;
	map->_dm_boundary = boundary;
	map->_dm_flags = flags & ~(BUS_DMA_WAITOK | BUS_DMA_NOWAIT);
	map->dm_maxsegsz = maxsegsz;
	*dmamp = map;
	return 0;
}

void
bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{

	kmem_intr_free(map,
	    sizeof(*map) + sizeof(bus_dma_segment_t) * (map->_dm_segcnt - 1));
}

/* One buffer of the kernel's into the map, a segment for each run of pages */
static int
tswifi_load(bus_dmamap_t map, void *buf, bus_size_t len, int *segp, bool first)
{
	vaddr_t va = (vaddr_t)buf;
	bus_addr_t pa, last = 0;
	bus_size_t n;
	int seg = *segp;

	if (!first && seg >= 0) {
		last = map->dm_segs[seg].ds_addr + map->dm_segs[seg].ds_len;
	}
	while (len > 0) {
		pa = rumpcomp_tswifi_va2pa((void *)va);
		n = PAGE_SIZE - (va & PAGE_MASK);
		if (n > len)
			n = len;
		if (seg >= 0 && pa == last
		    && map->dm_segs[seg].ds_len + n <= map->dm_maxsegsz) {
			map->dm_segs[seg].ds_len += n;
		} else {
			if (++seg >= map->_dm_segcnt)
				return EFBIG;
			map->dm_segs[seg].ds_addr = pa;
			map->dm_segs[seg].ds_len = n;
			map->dm_segs[seg]._ds_vacookie = va;
		}
		last = pa + n;
		va += n;
		len -= n;
	}
	*segp = seg;
	return 0;
}

int
bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t buflen,
    struct proc *p, int flags)
{
	int seg = -1, error;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	if (buflen > map->_dm_size || p != NULL)
		return EINVAL;
	error = tswifi_load(map, buf, buflen, &seg, true);
	if (error != 0)
		return error;
	map->dm_mapsize = buflen;
	map->dm_nsegs = seg + 1;
	map->_dm_cookie = buf;
	return 0;
}

int
bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m0, int flags)
{
	struct mbuf *m;
	int seg = -1, error = 0;
	bool first = true;

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
	if (m0->m_pkthdr.len > map->_dm_size)
		return EINVAL;
	for (m = m0; m != NULL && error == 0; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		error = tswifi_load(map, m->m_data, m->m_len, &seg, first);
		first = false;
	}
	if (error != 0)
		return error;
	map->dm_mapsize = m0->m_pkthdr.len;
	map->dm_nsegs = seg + 1;
	return 0;
}

int
bus_dmamap_load_uio(bus_dma_tag_t t, bus_dmamap_t map, struct uio *uio, int flags)
{

	return EOPNOTSUPP;
}

int
bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs,
    int nsegs, bus_size_t size, int flags)
{
	int i;

	if (nsegs > map->_dm_segcnt || size > map->_dm_size)
		return EINVAL;
	for (i = 0; i < nsegs; i++)
		map->dm_segs[i] = segs[i];
	map->dm_nsegs = nsegs;
	map->dm_mapsize = size;
	return 0;
}

void
bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{

	map->dm_mapsize = 0;
	map->dm_nsegs = 0;
}

void
bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t offset,
    bus_size_t len, int ops)
{
	int i, how = 0;

	if (ops & (BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD))
		how |= TSWIFI_SYNC_CLEAN;
	if (ops & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_POSTREAD))
		how |= TSWIFI_SYNC_INVAL;
	if (how == 0)
		return;
	for (i = 0; i < map->dm_nsegs && len > 0; i++) {
		bus_dma_segment_t *ds = &map->dm_segs[i];
		bus_size_t n;

		if (offset >= ds->ds_len) {
			offset -= ds->ds_len;
			continue;
		}
		n = ds->ds_len - offset;
		if (n > len)
			n = len;
		rumpcomp_tswifi_dmasync((void *)(ds->_ds_vacookie + offset), n, how);
		offset = 0;
		len -= n;
	}
}

/* One run of the hypercalls' memory, in one segment */
int
bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags)
{
	void *va;

	if (nsegs < 1)
		return EINVAL;
	size = round_page(size);
	va = rumpcomp_tswifi_dmalloc(size, alignment > PAGE_SIZE ? alignment : PAGE_SIZE);
	if (va == NULL)
		return ENOMEM;
	segs[0].ds_addr = rumpcomp_tswifi_va2pa(va);
	segs[0].ds_len = size;
	segs[0]._ds_vacookie = (vaddr_t)va;
	segs[0]._ds_sizecookie = size;
	*rsegs = 1;
	return 0;
}

void
bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		rumpcomp_tswifi_dmafree((void *)segs[i]._ds_vacookie, segs[i]._ds_sizecookie);
}

int
bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size,
    void **kvap, int flags)
{

	if (nsegs != 1)
		return EINVAL;
	*kvap = (void *)segs[0]._ds_vacookie;
	return 0;
}

void
bus_dmamem_unmap(bus_dma_tag_t t, void *kva, size_t size)
{
}

paddr_t
bus_dmamem_mmap(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, off_t off,
    int prot, int flags)
{

	return (paddr_t)-1;
}

/* ---------------------------------------------------------------- firmware */

struct firmware_handle {
	void	*fh_data;
	size_t	fh_size;
};

int
firmware_open(const char *drvname, const char *imgname,
    firmware_handle_t *fhp)
{
	struct firmware_handle *fh;
	void *data;
	size_t size;
	int error;

	error = rumpcomp_tswifi_fw_open(imgname, &data, &size);
	if (error != 0)
		return error;
	fh = kmem_zalloc(sizeof(*fh), KM_SLEEP);
	fh->fh_data = data;
	fh->fh_size = size;
	*fhp = fh;
	return 0;
}

int
firmware_close(firmware_handle_t fh)
{

	rumpcomp_tswifi_fw_close(fh->fh_data, fh->fh_size);
	kmem_free(fh, sizeof(*fh));
	return 0;
}

off_t
firmware_get_size(firmware_handle_t fh)
{

	return (off_t)fh->fh_size;
}

int
firmware_read(firmware_handle_t fh, off_t offset, void *buf, size_t len)
{

	if (offset < 0 || (size_t)offset > fh->fh_size
	    || len > fh->fh_size - (size_t)offset)
		return EINVAL;
	memcpy(buf, (char *)fh->fh_data + offset, len);
	return 0;
}

void *
firmware_malloc(size_t size)
{

	return kmem_alloc(size, KM_SLEEP);
}

void
firmware_free(void *v, size_t size)
{

	kmem_free(v, size);
}

/* ---------------------------------------------------------------- the chip's supplicant, PMF, SAE, roaming */

/*
 * The CYW43455's firmware (7.45.234) has a supplicant of its own
 * ("idsup"), the protection of management frames ("mfp") and WPA3's SAE
 * ("sae"), as its capabilities say. Asked to (rump_tswifi_fw_keys), the
 * join is set up for them just before bwfm joins: the chip then makes
 * the SAE exchange, the 4-way and group key handshakes and the keys
 * (BIP's integrity key with them) itself, and makes them again when it
 * roams to another access point of the network. Otherwise the join is
 * as bwfm makes it, the handshakes left to the host, and the chip's
 * roaming off when there are keys, as the host's keys would not follow
 * a roam.
 *
 * The choices are taken from the access point's RSN element (the node
 * of the scan): SAE when the access point offers it and the manager
 * gave a password, else PSK (or PSK with SHA-256) with the PMK; the
 * protection of management frames required when the access point
 * requires it or the key management is SAE, offered when the access
 * point offers it.
 */

/* The chip's capabilities, as rump_tswifi_fw_caps tells them */
#define TSWIFI_FW_IDSUP		1
#define TSWIFI_FW_SAE		2
#define TSWIFI_FW_MFP		4

/* The firmware's words of a join: its "wpa_auth" and "auth" values, a band, the SAE password's room */
#define TSWIFI_WPA2_AUTH_PSK		0x00000080
#define TSWIFI_WPA2_AUTH_PSK_SHA256	0x00008000
#define TSWIFI_WPA3_AUTH_SAE_PSK	0x00040000
#define TSWIFI_AUTH_SAE			3
#define TSWIFI_BAND_ALL			3
#define TSWIFI_SAE_PW_MAX		128

/* The chip roams when the signal is below -75 dBm, to an access point 20 dB stronger */
#define TSWIFI_ROAM_TRIGGER_DBM		(-75)
#define TSWIFI_ROAM_DELTA_DB		20

/* RSN's suites (IEEE 802.11 9.4.2.24): 00-0F-AC and the type */
#define RSN_AKM_PSK		2
#define RSN_AKM_PSK_SHA256	6
#define RSN_AKM_SAE		8
#define RSN_CIPHER_TKIP		2
#define RSN_CIPHER_CCMP		4
#define RSN_CAP_MFPR		0x0040
#define RSN_CAP_MFPC		0x0080

/* What the chip says of the link (wifi.c has the same layout) */
struct tswifi_fwlink {
	int32_t		authorized;	/* the access point's port is open: the keys are in */
	int32_t		rssi;		/* dBm */
	int32_t		tx_kbps;	/* the rate of the last frame sent */
	int32_t		rx_kbps;	/* and of the last received */
	int32_t		mode;		/* 0 802.11a/b/g, 1 802.11n, 2 802.11ac */
	int32_t		freq;		/* MHz */
	int32_t		sup_status;	/* the chip's supplicant's last state (E_PSK_SUP), -1 none */
	int32_t		roams;		/* how many times the chip roamed */
	uint8_t		bssid[6];
	uint8_t		pad[2];
};

int	rump_tswifi_fw_caps(void);
int	rump_tswifi_fw_keys(int, const uint8_t *, const uint8_t *, int);
int	rump_tswifi_fw_link(struct tswifi_fwlink *);
void	tswifi_bwfm_join(struct bwfm_softc *);
void	tswifi_bwfm_event(struct bwfm_softc *, struct bwfm_event *);

int	bwfm_fwvar_cmd_get_data(struct bwfm_softc *, int, void *, size_t);
int	bwfm_fwvar_cmd_set_data(struct bwfm_softc *, int, void *, size_t);
int	bwfm_fwvar_cmd_get_int(struct bwfm_softc *, int, uint32_t *);
int	bwfm_fwvar_var_get_data(struct bwfm_softc *, const char *, void *, size_t);
int	bwfm_fwvar_var_set_data(struct bwfm_softc *, const char *, void *, size_t);
int	bwfm_fwvar_var_set_int(struct bwfm_softc *, const char *, uint32_t);

/* What the manager asked for the next join; the secrets are cleared once given to the chip */
static struct {
	volatile int	use;		/* the chip's supplicant makes the handshakes */
	int		has_pmk;
	uint8_t		pmk[32];
	int		pw_len;
	uint8_t		pw[TSWIFI_SAE_PW_MAX];
} tswifi_fwk;

static struct {
	int		known;		/* the capabilities have been read */
	int		caps;
	volatile int	sup_status;
	volatile int	roams;
} tswifi_fws = { 0, 0, -1, 0 };

static struct bwfm_softc *
tswifi_bwfm(void)
{
	struct ifnet *ifp = ifunit("bwfm0");

	return ifp != NULL ? ifp->if_softc : NULL;
}

/* A word of the chip's capabilities, a list of words with blanks between */
static int
tswifi_has_word(const char *list, const char *w)
{
	size_t n = strlen(w);
	const char *p = list;

	while ((p = strstr(p, w)) != NULL) {
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\n' || p[n] == '\0'))
			return 1;
		p += n;
	}
	return 0;
}

/* The capabilities, read once and said on the console with the firmware's version */
static int
tswifi_caps_of(struct bwfm_softc *sc)
{
	char buf[512];
	size_t n;

	if (tswifi_fws.known)
		return tswifi_fws.caps;
	memset(buf, 0, sizeof(buf));
	if (bwfm_fwvar_var_get_data(sc, "ver", buf, sizeof(buf) - 1) == 0) {
		buf[sizeof(buf) - 1] = '\0';
		n = strlen(buf);
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		printf("bwfm: firmware %s\n", buf);
	}
	memset(buf, 0, sizeof(buf));
	if (bwfm_fwvar_var_get_data(sc, "cap", buf, sizeof(buf) - 1) != 0) {
		printf("bwfm: the chip's capabilities could not be read\n");
		return 0;
	}
	buf[sizeof(buf) - 1] = '\0';
	tswifi_fws.caps = (tswifi_has_word(buf, "idsup") ? TSWIFI_FW_IDSUP : 0)
	    | (tswifi_has_word(buf, "sae") ? TSWIFI_FW_SAE : 0)
	    | (tswifi_has_word(buf, "mfp") ? TSWIFI_FW_MFP : 0);
	tswifi_fws.known = 1;
	printf("bwfm: capabilities: %s\n", buf);
	printf("bwfm: the chip's supplicant %s, SAE %s, PMF %s\n",
	    (tswifi_fws.caps & TSWIFI_FW_IDSUP) ? "yes" : "no",
	    (tswifi_fws.caps & TSWIFI_FW_SAE) ? "yes" : "no",
	    (tswifi_fws.caps & TSWIFI_FW_MFP) ? "yes" : "no");
	return tswifi_fws.caps;
}

/* The chip's roaming on (off 0) or off, with the trigger and the step it roams by */
static void
tswifi_roam(struct bwfm_softc *sc, int off)
{
	uint32_t v[2];
	int e1, e2 = 0, e3 = 0;

	e1 = bwfm_fwvar_var_set_int(sc, "roam_off", off);
	if (!off) {
		v[0] = htole32((uint32_t)TSWIFI_ROAM_TRIGGER_DBM);
		v[1] = htole32(TSWIFI_BAND_ALL);
		e2 = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_ROAM_TRIGGER, v, sizeof(v));
		v[0] = htole32(TSWIFI_ROAM_DELTA_DB);
		v[1] = htole32(TSWIFI_BAND_ALL);
		e3 = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_ROAM_DELTA, v, sizeof(v));
	}
	/* each setting's answer, 0 taken */
	if (off)
		printf("bwfm: the chip's roaming off (%d)\n", e1);
	else
		printf("bwfm: the chip's roaming on, below %d dBm to one %d dB stronger (%d %d %d)\n",
		    TSWIFI_ROAM_TRIGGER_DBM, TSWIFI_ROAM_DELTA_DB, e1, e2, e3);
}

/* The events of the supplicant and of a roam, added to those bwfm asked for */
static void
tswifi_events(struct bwfm_softc *sc)
{
	uint8_t mask[BWFM_EVENT_MASK_LEN];

	memset(mask, 0, sizeof(mask));
	if (bwfm_fwvar_var_get_data(sc, "event_msgs", mask, sizeof(mask)) != 0)
		return;
	mask[BWFM_E_PSK_SUP / 8] |= 1 << (BWFM_E_PSK_SUP % 8);
	mask[BWFM_E_ROAM / 8] |= 1 << (BWFM_E_ROAM % 8);
	(void)bwfm_fwvar_var_set_data(sc, "event_msgs", mask, sizeof(mask));
}

/*
 * The access point's RSN element (the scan's node): the group cipher,
 * the key managements offered (a bit for each type) and the
 * capabilities. 0 when there is none to read.
 */
static int
tswifi_ap_rsn(const uint8_t *ie, int *group, int *akms, int *caps)
{
	const uint8_t *p, *end;
	int n, i;

	if (ie == NULL || ie[0] != IEEE80211_ELEMID_RSN || ie[1] < 2 + 4)
		return 0;
	p = ie + 2;
	end = p + ie[1];
	p += 2;					/* the version */
	*group = p[3];
	p += 4;
	*akms = 0;
	*caps = 0;
	if (end - p < 2)
		return 1;
	n = p[0] | (p[1] << 8);			/* the pairwise ciphers, passed over */
	if (end - p < 2 + 4 * n)
		return 1;
	p += 2 + 4 * n;
	if (end - p < 2)
		return 1;
	n = p[0] | (p[1] << 8);
	p += 2;
	for (i = 0; i < n && end - p >= 4; i++, p += 4) {
		if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] < 32)
			*akms |= 1 << p[3];
	}
	if (end - p >= 2)
		*caps = p[0] | (p[1] << 8);
	return 1;
}

/* The station's RSN element for the chip's supplicant: its length */
static int
tswifi_sta_rsn(uint8_t *ie, int group, int akm, int mfp)
{
	int n = 0, caps = (mfp >= BWFM_MFP_CAPABLE ? RSN_CAP_MFPC : 0)
	    | (mfp == BWFM_MFP_REQUIRED ? RSN_CAP_MFPR : 0);

	ie[n++] = IEEE80211_ELEMID_RSN;
	ie[n++] = 0;
	ie[n++] = 1;				/* version 1 */
	ie[n++] = 0;
	ie[n++] = 0x00;				/* the group cipher, the access point's */
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = group;
	ie[n++] = 1;				/* one pairwise cipher: CCMP */
	ie[n++] = 0;
	ie[n++] = 0x00;
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = RSN_CIPHER_CCMP;
	ie[n++] = 1;				/* one key management */
	ie[n++] = 0;
	ie[n++] = 0x00;
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = akm;
	ie[n++] = caps & 0xff;
	ie[n++] = caps >> 8;
	ie[1] = n - 2;
	return n;
}

static const char *
tswifi_akm_name(int akm)
{
	return akm == RSN_AKM_SAE ? "SAE (WPA3-Personal)"
	    : akm == RSN_AKM_PSK_SHA256 ? "PSK-SHA256" : "PSK (WPA2-Personal)";
}

/*
 * Just before bwfm joins (bwfm_connect, after it has written "wpa_auth",
 * "wsec", "auth" and "mfp"): the rest of the join's security written
 */
void
tswifi_bwfm_join(struct bwfm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct bwfm_wsec_pmk pmk;
	struct {
		uint16_t	len;
		uint8_t		key[TSWIFI_SAE_PW_MAX];
	} __packed sae;
	uint8_t ie[32];
	int caps, group = RSN_CIPHER_CCMP, akms = 1 << RSN_AKM_PSK, rcaps = 0;
	int akm, mfp, len, e[7], i, wpa = (ic->ic_flags & IEEE80211_F_WPA) != 0;

	caps = tswifi_caps_of(sc);
	tswifi_events(sc);
	tswifi_fws.sup_status = -1;
	if (!tswifi_fwk.use || !wpa || !(caps & TSWIFI_FW_IDSUP)) {
		/* as bwfm makes it: the host's handshakes, and no roaming of the chip's under them */
		(void)bwfm_fwvar_var_set_int(sc, "sup_wpa", 0);
		printf("bwfm: join: %s\n", wpa ? "the handshakes by the host" : "no security");
		tswifi_roam(sc, wpa);
		goto clear;
	}
	if (ni == NULL || !tswifi_ap_rsn(ni->ni_wpa_ie, &group, &akms, &rcaps))
		printf("bwfm: join: the access point's RSN element is not at hand: CCMP and PSK taken\n");
	if (tswifi_fwk.pw_len > 0 && (akms & (1 << RSN_AKM_SAE)) && (caps & TSWIFI_FW_SAE))
		akm = RSN_AKM_SAE;
	else if (tswifi_fwk.has_pmk && (akms & (1 << RSN_AKM_PSK)))
		akm = RSN_AKM_PSK;
	else if (tswifi_fwk.has_pmk && (akms & (1 << RSN_AKM_PSK_SHA256)))
		akm = RSN_AKM_PSK_SHA256;
	else {
		printf("bwfm: join: the access point offers no key management there is a key for"
		    " (offered %#x, a PMK %s, a password %s)\n", akms,
		    tswifi_fwk.has_pmk ? "given" : "not given",
		    tswifi_fwk.pw_len > 0 ? "given" : "not given");
		akm = RSN_AKM_PSK;
	}
	mfp = (akm == RSN_AKM_SAE || (rcaps & RSN_CAP_MFPR)) ? BWFM_MFP_REQUIRED
	    : (rcaps & RSN_CAP_MFPC) ? BWFM_MFP_CAPABLE : BWFM_MFP_NONE;
	if (mfp != BWFM_MFP_NONE && !(caps & TSWIFI_FW_MFP)) {
		printf("bwfm: join: the chip has no PMF\n");
		mfp = BWFM_MFP_NONE;
	}

	for (i = 0; i < 7; i++)
		e[i] = 0;
	len = tswifi_sta_rsn(ie, group, akm, mfp);
	e[0] = bwfm_fwvar_var_set_data(sc, "wpaie", ie, len);
	e[1] = bwfm_fwvar_var_set_int(sc, "wpa_auth",
	    akm == RSN_AKM_SAE ? TSWIFI_WPA3_AUTH_SAE_PSK
	    : akm == RSN_AKM_PSK_SHA256 ? TSWIFI_WPA2_AUTH_PSK_SHA256 : TSWIFI_WPA2_AUTH_PSK);
	e[2] = bwfm_fwvar_var_set_int(sc, "wsec",
	    BWFM_WSEC_AES | (group == RSN_CIPHER_TKIP ? BWFM_WSEC_TKIP : 0));
	e[3] = bwfm_fwvar_var_set_int(sc, "auth",
	    akm == RSN_AKM_SAE ? TSWIFI_AUTH_SAE : BWFM_AUTH_OPEN);
	e[4] = bwfm_fwvar_var_set_int(sc, "mfp", mfp);
	if (akm == RSN_AKM_SAE) {
		memset(&sae, 0, sizeof(sae));
		sae.len = htole16(tswifi_fwk.pw_len);
		memcpy(sae.key, tswifi_fwk.pw, tswifi_fwk.pw_len);
		e[5] = bwfm_fwvar_var_set_data(sc, "sae_password", &sae, sizeof(sae));
		explicit_memset(&sae, 0, sizeof(sae));
	} else {
		/* the PMK as it is, not a passphrase (no BWFM_WSEC_PASSPHRASE) */
		memset(&pmk, 0, sizeof(pmk));
		pmk.key_len = htole16(sizeof(tswifi_fwk.pmk));
		pmk.flags = 0;
		memcpy(pmk.key, tswifi_fwk.pmk, sizeof(tswifi_fwk.pmk));
		e[5] = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_WSEC_PMK, &pmk, sizeof(pmk));
		explicit_memset(&pmk, 0, sizeof(pmk));
	}
	e[6] = bwfm_fwvar_var_set_int(sc, "sup_wpa", 1);
	/* each setting's answer (0 taken), in the order written: wpaie wpa_auth wsec auth mfp key sup_wpa */
	printf("bwfm: join: the chip's supplicant, %s, group %s, PMF %s (%d %d %d %d %d %d %d)\n",
	    tswifi_akm_name(akm), group == RSN_CIPHER_TKIP ? "TKIP" : "CCMP",
	    mfp == BWFM_MFP_REQUIRED ? "required" : mfp == BWFM_MFP_CAPABLE ? "offered" : "none",
	    e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
	tswifi_roam(sc, 0);
clear:
	explicit_memset(&tswifi_fwk, 0, sizeof(tswifi_fwk));
}

/* The chip's events: those of a join, of a roam and of the supplicant said, and noted */
void
tswifi_bwfm_event(struct bwfm_softc *sc, struct bwfm_event *e)
{
	static const struct {
		uint32_t	type;
		const char	*name;
	} names[] = {
		{ BWFM_E_SET_SSID, "set_ssid" }, { BWFM_E_AUTH, "auth" },
		{ BWFM_E_DEAUTH, "deauth" }, { BWFM_E_DEAUTH_IND, "deauth_ind" },
		{ BWFM_E_ASSOC, "assoc" }, { BWFM_E_REASSOC, "reassoc" },
		{ BWFM_E_DISASSOC, "disassoc" }, { BWFM_E_DISASSOC_IND, "disassoc_ind" },
		{ BWFM_E_LINK, "link" }, { BWFM_E_ROAM, "roam" },
		{ BWFM_E_PSK_SUP, "psk_sup" },
	};
	uint32_t type = ntohl(e->msg.event_type), status = ntohl(e->msg.status);
	const uint8_t *a = (const uint8_t *)&e->msg.addr;
	size_t i;

	for (i = 0; i < __arraycount(names) && names[i].type != type; i++)
		continue;
	if (i == __arraycount(names))
		return;
	/*
	 * The reason of a deauthentication or a disassociation is IEEE
	 * 802.11's; the status of psk_sup is the chip's supplicant's state
	 * (6 the keys are in)
	 */
	printf("bwfm: event %s, status %u, reason %u, flags %#x, %02x:%02x:%02x:%02x:%02x:%02x\n",
	    names[i].name, status, ntohl(e->msg.reason), ntohs(e->msg.flags),
	    a[0], a[1], a[2], a[3], a[4], a[5]);
	if (type == BWFM_E_PSK_SUP)
		tswifi_fws.sup_status = status;
	if (type == BWFM_E_ROAM && status == 0)
		tswifi_fws.roams++;
}

/*
 * The chip told to leave its network (tswifi_if.c, rump_tswifi_deauth):
 * it keeps its association whatever net80211's state, so a disconnection
 * asked of net80211 alone would leave it joined. The reason is IEEE
 * 802.11's, sent to the access point.
 */
void	tswifi_bwfm_leave(const uint8_t *, int);

void
tswifi_bwfm_leave(const uint8_t *bssid, int reason)
{
	struct bwfm_softc *sc = tswifi_bwfm();
	struct {
		uint32_t	val;
		uint8_t		ea[6];
	} __packed scb;
	int e;

	if (sc == NULL)
		return;
	memset(&scb, 0, sizeof(scb));
	scb.val = htole32((uint32_t)reason);
	memcpy(scb.ea, bssid, sizeof(scb.ea));
	e = bwfm_fwvar_cmd_set_data(sc, BWFM_C_DISASSOC, &scb, sizeof(scb));
	printf("bwfm: the chip told to leave the network (reason %d): %d\n", reason, e);
}

/* What the chip can do itself: TSWIFI_FW_*, 0 when there is no chip */
int
rump_tswifi_fw_caps(void)
{
	struct bwfm_softc *sc;
	int caps = 0;

	rump_schedule();
	sc = tswifi_bwfm();
	if (sc != NULL)
		caps = tswifi_caps_of(sc);
	rump_unschedule();
	return caps;
}

/*
 * The next join's handshakes by the chip's supplicant (use 1), with the
 * network's PMK (32 bytes, for PSK) and its password (for SAE; NULL or
 * length 0 none), or by the host (use 0). 0 or an errno value.
 */
int
rump_tswifi_fw_keys(int use, const uint8_t *pmk, const uint8_t *pw, int pw_len)
{

	if (pw_len < 0 || pw_len > TSWIFI_SAE_PW_MAX)
		return EINVAL;
	explicit_memset(&tswifi_fwk, 0, sizeof(tswifi_fwk));
	if (!use)
		return 0;
	if (pmk != NULL) {
		memcpy(tswifi_fwk.pmk, pmk, sizeof(tswifi_fwk.pmk));
		tswifi_fwk.has_pmk = 1;
	}
	if (pw != NULL && pw_len > 0) {
		memcpy(tswifi_fwk.pw, pw, pw_len);
		tswifi_fwk.pw_len = pw_len;
	}
	membar_producer();
	tswifi_fwk.use = 1;
	return 0;
}

/*
 * The link as the chip has it. It asks the chip several things over
 * SDIO: not more than about once a second. 0 or an errno value.
 */
int
rump_tswifi_fw_link(struct tswifi_fwlink *l)
{
	struct bwfm_softc *sc;
	struct bwfm_sta_info sta;
	uint32_t v, ch[3];
	int c;

	memset(l, 0, sizeof(*l));
	l->sup_status = tswifi_fws.sup_status;
	l->roams = tswifi_fws.roams;
	rump_schedule();
	sc = tswifi_bwfm();
	if (sc == NULL) {
		rump_unschedule();
		return ENXIO;
	}
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_BSSID, l->bssid, sizeof(l->bssid)) != 0)
		memset(l->bssid, 0, sizeof(l->bssid));
	if (bwfm_fwvar_cmd_get_int(sc, BWFM_C_GET_RSSI, &v) == 0)
		l->rssi = (int32_t)v;
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_CHANNEL, ch, sizeof(ch)) == 0) {
		c = (int)le32toh(ch[1]);		/* the network's channel */
		l->freq = c <= 0 ? 0 : c == 14 ? 2484 : c < 14 ? 2407 + 5 * c : 5000 + 5 * c;
	}
	memset(&sta, 0, sizeof(sta));
	memcpy(&sta, l->bssid, sizeof(l->bssid));	/* whose: the access point's */
	if (bwfm_fwvar_var_get_data(sc, "sta_info", &sta, sizeof(sta)) == 0
	    && memcmp(sta.ea, l->bssid, sizeof(l->bssid)) == 0) {
		v = le32toh(sta.flags);
		l->authorized = (v & BWFM_STA_AUTHO) != 0;
		l->mode = (v & BWFM_STA_VHT_CAP) ? 2 : (v & BWFM_STA_N_CAP) ? 1 : 0;
		if (le16toh(sta.ver) >= 3) {
			l->tx_kbps = (int32_t)le32toh(sta.tx_rate);
			l->rx_kbps = (int32_t)le32toh(sta.rx_rate);
		}
	}
	rump_unschedule();
	return 0;
}
