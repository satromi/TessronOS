/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_mwx.c (x64 PC, WIFI_NET80211=obsd)
 *	The Wi-Fi interface's driver for MediaTek's MT7921 and MT7922 (design
 *	9.6.9): OpenBSD's mwx(4), as tswifi_obsd.c asks of it (tsw_drv.h).
 *
 *	What differs from iwm and iwx as the interface sees it:
 *
 *	- mwx is 802.11a/b/g only so far: it does not turn on 802.11n or ac,
 *	  so the rate stays at 54 Mb/s or below, whatever the card can do;
 *	- the card reaches only 32 bits of address, in its rings and in the
 *	  frames' pieces. The rings and clusters are below 4GB for every
 *	  card; the mbufs themselves are made so too (obsd_mbuf_low);
 *	- the signal the driver gives net80211 is the dBm itself, a negative
 *	  number kept in net80211's unsigned byte (min_dbm below);
 *	- the card scans by its firmware, and keeps the keys as iwx's does:
 *	  ic_set_key answers EBUSY and a task of the driver sets them;
 *	- the driver's softc is private to its file, and has no counts of
 *	  the card's: what the card wrote is not looked at here.
 */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include "obsd_glue.h"
#include "tsw_drv.h"

extern struct cfattach mwx_ca;

/* The two members the driver's softc starts with; the rest is its own */
struct mwx_head {
	struct device		sc_dev;
	struct ieee80211com	sc_ic;
};

static struct ieee80211com	*mwx_ic;

static int
mwx_t_match(struct pci_attach_args *pa)
{
	return (*mwx_ca.ca_match)(NULL, NULL, pa);
}

static struct ieee80211com *
mwx_t_attach(struct pci_attach_args *pa, const char *xname)
{
	struct mwx_head *sc;
	struct ieee80211com *ic;
	static const uint8_t none[IEEE80211_ADDR_LEN];

	obsd_mbuf_low = 1;
	sc = malloc(mwx_ca.ca_devsize, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		return NULL;
	strlcpy(sc->sc_dev.dv_xname, xname, sizeof(sc->sc_dev.dv_xname));
	printf("%s at pci%d dev %d function %d", xname, pa->pa_bus, pa->pa_device, pa->pa_function);
	(*mwx_ca.ca_attach)(NULL, &sc->sc_dev, pa);
	ic = &sc->sc_ic;
	if (ic->ic_if.if_softc != sc) {
		printf("obsd: %s did not attach\n", xname);
		return NULL;
	}
	obsd_mountroot_run();
	/* the address is read from the card once its firmware runs, and nothing else tells of that failing */
	if (memcmp(ic->ic_myaddr, none, sizeof(none)) == 0) {
		printf("obsd: %s could not read its firmware and address\n", xname);
		return NULL;
	}
	printf("obsd: %s: the firmware runs, address %s; 802.11a/b/g only (mwx has not 802.11n or ac)\n",
	    xname, ether_sprintf(ic->ic_myaddr));
	mwx_ic = ic;
	return ic;
}

/* The card's firmware keeps it on the channel by itself: no time event */
static int
mwx_t_te_on(void)
{
	return 0;
}

/* Its queues are its softc's, not to be seen here */
static int
mwx_t_mgmt_queued(void)
{
	return 0;
}

/* A scan of its own is IEEE80211_S_SCAN, which the interface looks at itself */
static int
mwx_t_scanning(void)
{
	return mwx_ic != NULL && (mwx_ic->ic_flags & IEEE80211_F_BGSCAN) != 0;
}

static void
mwx_t_card_stats(void)
{
}

static void
mwx_t_debug(int level)
{
}

const struct tsw_drv tsw_mwx_drv = {
	.name = "mwx",
	.min_dbm = -256,
	.match = mwx_t_match,
	.attach = mwx_t_attach,
	.te_on = mwx_t_te_on,
	.tx_counts = NULL,
	.mgmt_queued = mwx_t_mgmt_queued,
	.scanning = mwx_t_scanning,
	.ptk_to_card = NULL,
	.rx_peek = NULL,
	.card_stats = mwx_t_card_stats,
	.port_say = NULL,
	.debug = mwx_t_debug,
};
