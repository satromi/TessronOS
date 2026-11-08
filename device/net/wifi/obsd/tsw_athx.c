/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_athx.c (x64 PC, WIFI_NET80211=obsd)
 *	The Wi-Fi interface's driver for Qualcomm's QCA6174 hw3.x (PCI
 *	168c:003e, the first Surface Go's; design 9.6.9): athx (athx*.c), as
 *	tswifi_obsd.c asks of it (tsw_drv.h).
 *
 *	What differs from iwm and iwx as the interface sees it:
 *
 *	- the firmware runs from the attach on (the driver brings the chip up
 *	  there, its firmware and board data read from their objects): the
 *	  interface's up only makes the station's vdev;
 *	- the card reaches only 32 bits of address: every buffer it reads or
 *	  writes is the driver's DMA memory, below 4GB, frames copied in and
 *	  out of it; no mbuf is given to the card;
 *	- the signal the driver gives net80211 is the dBm itself, a negative
 *	  number kept in net80211's unsigned byte (min_dbm below);
 *	- the card scans by its firmware, keeps the pairwise and group keys as
 *	  iwx's does (ic_set_key answers EBUSY and a task of the driver sets
 *	  them), and does its own rate control and aggregation;
 *	- its own counts: the frames of HTT received and dropped, sent and
 *	  completed, the events of WMI.
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
#include "athxreg.h"
#include "athxvar.h"

static int
athx_t_match(struct pci_attach_args *pa)
{
	return athx_match(pa);
}

static struct ieee80211com *
athx_t_attach(struct pci_attach_args *pa, const char *xname)
{
	struct ieee80211com *ic;

	ic = athx_attach(pa, xname);
	if (ic == NULL) {
		printf("obsd: %s did not attach\n", xname);
		return NULL;
	}
	obsd_mountroot_run();
	return ic;
}

/* The card's firmware keeps it on the channel by itself: no time event */
static int
athx_t_te_on(void)
{
	return 0;
}

/* Management frames wait with the data frames for the driver's buffers */
static int
athx_t_mgmt_queued(void)
{
	return athx_sc != NULL ? athx_sc->htt.tx_pending : 0;
}

/* Its scans are IEEE80211_S_SCAN only (no background scan), which the interface looks at itself */
static int
athx_t_scanning(void)
{
	return 0;
}

static void
athx_t_card_stats(void)
{
	struct athx_softc *sc = athx_sc;

	if (sc == NULL)
		return;
	printf("athx: rx: %u indications, %u MSDUs, %u dropped, %u ready, ring %d of %d; tx: %d in flight, "
	    "%u completed, %u not acknowledged, %u discarded; WMI: %u events, %u commands, %u management "
	    "frames; block-ack %u up %u down%s\n", sc->htt.n_rx_ind, sc->htt.n_rx_msdu, sc->htt.n_rx_drop,
	    sc->htt.mpdus_ready, sc->htt.fill_cnt, sc->htt.fill_level, sc->htt.tx_pending, sc->htt.n_tx_compl,
	    sc->htt.n_tx_noack, sc->htt.n_tx_discard, sc->n_wmi_ev, sc->n_wmi_cmd, sc->n_mgmt_rx, sc->htt.n_addba,
	    sc->htt.n_delba, sc->wedged ? "; the firmware has crashed" : "");
}

static void
athx_t_debug(int level)
{
	if (athx_sc != NULL)
		athx_sc->debug = level;
}

const struct tsw_drv tsw_athx_drv = {
	.name = "athx",
	.min_dbm = -256,
	.match = athx_t_match,
	.attach = athx_t_attach,
	.te_on = athx_t_te_on,
	.tx_counts = NULL,
	.mgmt_queued = athx_t_mgmt_queued,
	.scanning = athx_t_scanning,
	.ptk_to_card = NULL,
	.rx_peek = NULL,
	.card_stats = athx_t_card_stats,
	.port_say = NULL,
	.debug = athx_t_debug,
};
