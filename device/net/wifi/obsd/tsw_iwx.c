/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_iwx.c (x64 PC, WIFI_NET80211=obsd)
 *	The Wi-Fi interface's driver for Intel's Wi-Fi 6 AX200 (8086:2723,
 *	the Surface Go 2's) and the later cards of its family (design
 *	9.6.9): OpenBSD's iwx(4), as tswifi_obsd.c asks of it (tsw_drv.h).
 *	Compiled with OpenBSD's kernel headers and iwx's.
 *
 *	What differs from iwm as the interface sees it:
 *
 *	- the card's firmware chooses the rate (its own rate control,
 *	  TLC): there are no counts of tries in the driver;
 *	- the keys, the group's and the integrity group key among them, are
 *	  set into the card by a task of the driver (iwx_setkey_task,
 *	  waiting for the card's answer to each), the port made valid by it
 *	  once all of them are in; ic_set_key answers EBUSY at once;
 *	- the card's firmware makes and ends the block-ack agreements for
 *	  what the station sends (IEEE80211_C_ADDBA_OFFLOAD): the agreement
 *	  is made as the driver enables the TID's aggregation queue;
 *	- the card writes what it received to a ring of its own (multiple
 *	  queues), each MPDU after a descriptor with its status and rate.
 */

#include "bpfilter.h"

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/refcnt.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_amrr.h>
#include <net80211/ieee80211_ra.h>
#include <net80211/ieee80211_ra_vht.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_priv.h>

/* as if_iwx.c has it before its registers' header */
#define le32_to_cpup(_a_) (le32toh(*(const uint32_t *)(_a_)))

#include <dev/pci/if_iwxreg.h>
#include <dev/pci/if_iwxvar.h>

#include "obsd_glue.h"
#include "tsw_drv.h"

extern const struct cfattach iwx_ca;

int	iwx_rx_pkt_valid(struct iwx_rx_packet *);

static struct iwx_softc	*iwx_sc;

static int
iwx_t_match(struct pci_attach_args *pa)
{
	return (*iwx_ca.ca_match)(NULL, NULL, pa);
}

static struct ieee80211com *
iwx_t_attach(struct pci_attach_args *pa, const char *xname)
{
	struct iwx_softc *sc;

	sc = malloc(iwx_ca.ca_devsize, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		return NULL;
	strlcpy(sc->sc_dev.dv_xname, xname, sizeof(sc->sc_dev.dv_xname));
	printf("%s at pci%d dev %d function %d", xname, pa->pa_bus, pa->pa_device, pa->pa_function);
	(*iwx_ca.ca_attach)(NULL, &sc->sc_dev, pa);
	if (sc->sc_ic.ic_if.if_softc != sc) {
		printf("obsd: %s did not attach\n", xname);
		return NULL;
	}
	printf("obsd: %s: firmware %s, device family %d, HW_REV %08x, RF ID %08x, %s interrupt\n",
	    xname, sc->sc_fwname != NULL ? sc->sc_fwname : "(none)", sc->sc_device_family,
	    sc->sc_hw_rev, sc->sc_hw_rf_id, sc->sc_msix ? "MSI-X" : "message (MSI)");
	obsd_mountroot_run();
	if (!sc->attached) {
		printf("obsd: %s could not read its firmware and NVM\n", xname);
		return NULL;
	}
	printf("obsd: %s: firmware %s running its API (first data queue %d), the NVM read\n",
	    xname, sc->sc_fwver, sc->first_data_qid);
	iwx_sc = sc;
	return &sc->sc_ic;
}

/* The firmware's session protection (iwx_schedule_session_protection in iwx_auth) */
static int
iwx_t_te_on(void)
{
	return iwx_sc != NULL && (iwx_sc->sc_flags & IWX_FLAG_TE_ACTIVE) != 0;
}

/* Management frames go out on the first data queue (iwx_tx) */
static int
iwx_t_mgmt_queued(void)
{
	if (iwx_sc == NULL)
		return 0;
	return iwx_sc->txq[iwx_sc->first_data_qid].queued;
}

static int
iwx_t_scanning(void)
{
	return iwx_sc != NULL && (iwx_sc->sc_flags & (IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN)) != 0;
}

/*
 * The frames the card received, read from the receive buffer as iwx
 * begins to read it (iwx_rx_pkt syncs the buffer first), before iwx
 * drops any: each MPDU the card wrote, after its descriptor (the
 * status, the rate), and the card's answers to what was sent (each
 * frame's status, and the compressed block-ack answers). A buffer is
 * looked at once, as iwx reads it (the ring's index it is at).
 */
static int	pk_last = -1;

static void
iwx_t_rx_peek(bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	struct iwx_softc *sc = iwx_sc;
	struct iwx_rx_data *data;
	struct iwx_rx_packet *pkt;
	struct iwx_rx_mpdu_desc *desc;
	const size_t minsz = sizeof(pkt->len_n_flags) + sizeof(pkt->hdr);
	uint32_t offset = 0, plen, flen, code, rate;
	uint64_t port_ns = tsw_port_ns();
	size_t dsz;
	uint8_t *base;
	int cur;

	if (sc == NULL || off != 0 || len != IWX_RBUF_SIZE)
		return;
	cur = sc->rxq.cur;
	data = &sc->rxq.data[cur];
	if (data->map != map || data->m == NULL || cur == pk_last)
		return;
	pk_last = cur;
	base = mtod(data->m, uint8_t *);
	dsz = sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210 ? sizeof(*desc) : IWX_RX_DESC_SIZE_V1;
	while (offset + minsz < IWX_RBUF_SIZE) {
		pkt = (struct iwx_rx_packet *)(base + offset);
		if (!iwx_rx_pkt_valid(pkt))
			break;
		plen = sizeof(pkt->len_n_flags) + iwx_rx_packet_len(pkt);
		if (plen < minsz || plen > IWX_RBUF_SIZE - offset)
			break;
		code = IWX_WIDE_ID(pkt->hdr.flags, pkt->hdr.code);
		if (code == IWX_STATISTICS_NOTIFICATION)
			tsw_stats_size("iwx", plen - (uint32_t)minsz,
			    sizeof(struct iwx_notif_statistics), "STATISTICS_NTFY_API_S_VER_13");
		if (pkt->hdr.code == IWX_TX_CMD && plen >= minsz + sizeof(struct iwx_tx_resp) &&
		    (port_ns != 0 ? tsw_txr_say() : tsw_txr_hs_say())) {
			const struct iwx_tx_resp *tr = (const struct iwx_tx_resp *)pkt->data;
			uint32_t ts = le16toh(tr->status.status) & IWX_TX_STATUS_MSK;
			char when[40];

			/* after the port opened, the time since; before it, the handshake's frames (no key in the card) */
			if (port_ns != 0)
				snprintf(when, sizeof(when), "%u ms after the port opened",
				    (int)((getnsecuptime() - port_ns) / 1000000ULL));
			else
				strlcpy(when, "during the handshake", sizeof(when));
			printf("wifi: the card sent: queue %u, %u frame%s, status %02x (%s), tried again %u,"
			    " RTS failed %u, rate %08x, TID %u, sequence %u, %s\n",
			    pkt->hdr.qid & 0x7f, tr->frame_count, tr->frame_count == 1 ? "" : "s", ts,
			    ts == IWX_TX_STATUS_SUCCESS || ts == IWX_TX_STATUS_DIRECT_DONE ? "acknowledged" :
			    ts == IWX_TX_STATUS_FAIL_SHORT_LIMIT || ts == IWX_TX_STATUS_FAIL_LONG_LIMIT ?
			    "not acknowledged: the retry limit" : "not sent", tr->failure_frame, tr->failure_rts,
			    le32toh(tr->initial_rate), tr->ra_tid & 0x0f, le16toh(tr->seq_ctl) >> 4, when);
		} else if (pkt->hdr.code == IWX_BA_NOTIF &&
		    plen >= minsz + sizeof(struct iwx_compressed_ba_notif) && tsw_txr_say()) {
			const struct iwx_compressed_ba_notif *ba =
			    (const struct iwx_compressed_ba_notif *)pkt->data;
			const struct iwx_compressed_ba_ratid *rt = (const struct iwx_compressed_ba_ratid *)
			    ((const uint8_t *)ba + sizeof(*ba));
			int has_rt = le16toh(ba->ra_tid_cnt) > 0 &&
			    plen >= minsz + sizeof(*ba) + sizeof(*rt);

			printf("wifi: the card sent: block-ack answer for station %u, TID %d, %u of %u frames"
			    " acknowledged, the window from %d, queue %d, %u TFDs\n", ba->sta_id,
			    has_rt ? rt->tid : -1, le16toh(ba->done), le16toh(ba->txed),
			    has_rt ? (int)le16toh(rt->ssn) : -1, has_rt ? rt->q_num : -1,
			    le16toh(ba->tfd_cnt));
		} else if (pkt->hdr.code == IWX_REPLY_RX_MPDU_CMD && plen >= minsz + dsz) {
			desc = (struct iwx_rx_mpdu_desc *)pkt->data;
			flen = le16toh(desc->mpdu_len);
			rate = le32toh(sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210 ?
			    desc->v3.rate_n_flags : desc->v1.rate_n_flags);
			/* a header padded to four bytes (MFLG2_PAD) has its two bytes after the IV, not within it */
			if (flen >= sizeof(struct ieee80211_frame) && minsz + dsz + flen <= plen)
				tsw_rx_mpdu((const struct ieee80211_frame *)(pkt->data + dsz), flen,
				    le32toh(desc->status), rate);
		}
		offset += roundup(plen, IWX_FH_RSCSR_FRAME_ALIGN);
	}
}

/*
 * The card's own counts, as its last statistics notification gave them
 * (iwx keeps it in sc_stats), as for iwm
 */
static void
iwx_t_card_stats(void)
{
	const struct iwx_statistics_rx *r;

	if (iwx_sc == NULL)
		return;
	r = &iwx_sc->sc_stats.rx;
	printf("wifi: receive: the card: OFDM good %u, CRC errors %u, PLCP errors %u;"
	    " CCK good %u, CRC errors %u; HT good %u, CRC errors %u, PLCP errors %u,"
	    " MCS not supported %u, aggregates %u (good %u)\n",
	    letoh32(r->ofdm.crc32_good), letoh32(r->ofdm.crc32_err),
	    letoh32(r->ofdm.plcp_err), letoh32(r->cck.crc32_good),
	    letoh32(r->cck.crc32_err), letoh32(r->ofdm_ht.crc32_good),
	    letoh32(r->ofdm_ht.crc32_err), letoh32(r->ofdm_ht.plcp_err),
	    letoh32(r->ofdm_ht.unsupport_mcs), letoh32(r->ofdm_ht.agg_cnt),
	    letoh32(r->ofdm_ht.agg_crc32_good));
	printf("wifi: receive: the card: dropped by its filter %u, of another BSSID %u,"
	    " beacons %u (missed %u); acknowledgements sent %u (CCK %u), block-ack answers %u\n",
	    letoh32(r->general.filtered_frames), letoh32(r->general.non_bssid_frames),
	    letoh32(r->general.channel_beacons), letoh32(r->general.num_missed_bcon),
	    letoh32(r->ofdm.sent_ack_cnt), letoh32(r->cck.sent_ack_cnt),
	    letoh32(r->ofdm.sent_ba_rsp_cnt));
}

/*
 * The keys the card holds as the port opens: those iwx_setkey_task has
 * set (each with the card's answer), and those still waiting for it.
 * The port is valid in the driver's view once the pairwise key, the
 * group key and, with protected management frames, the integrity group
 * key are in.
 */
static void
iwx_t_port_say(void)
{
	struct iwx_node *in;

	if (iwx_sc == NULL || tsw_ic->ic_bss == NULL)
		return;
	in = (struct iwx_node *)tsw_ic->ic_bss;
	printf("wifi: the card's keys: pairwise %d, group %d, integrity group %d, %d still to be set;"
	    " the card's station flags %x\n", !!(in->in_flags & IWX_NODE_FLAG_HAVE_PAIRWISE_KEY),
	    !!(in->in_flags & IWX_NODE_FLAG_HAVE_GROUP_KEY),
	    !!(in->in_flags & IWX_NODE_FLAG_HAVE_INTEGRITY_GROUP_KEY), iwx_sc->setkey_nkeys,
	    iwx_sc->sc_flags & IWX_FLAG_STA_ACTIVE);
}

static void
iwx_t_debug(int level)
{
#ifdef IWX_DEBUG
	extern int iwx_debug;

	iwx_debug = level;
#endif
}

const struct tsw_drv tsw_iwx_drv = {
	.name = "iwx",
	.min_dbm = IWX_MIN_DBM,
	.match = iwx_t_match,
	.attach = iwx_t_attach,
	.te_on = iwx_t_te_on,
	.tx_counts = NULL,
	.mgmt_queued = iwx_t_mgmt_queued,
	.scanning = iwx_t_scanning,
	.ptk_to_card = NULL,
	.rx_peek = iwx_t_rx_peek,
	.card_stats = iwx_t_card_stats,
	.port_say = iwx_t_port_say,
	.debug = iwx_t_debug,
};
