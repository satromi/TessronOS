/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_iwm.c (x64 PC, WIFI_NET80211=obsd)
 *	The Wi-Fi interface's driver for Intel's 7260 to 8265 and 9260/9560
 *	(design 9.6.9): OpenBSD's iwm(4), as tswifi_obsd.c asks of it
 *	(tsw_drv.h). Compiled with OpenBSD's kernel headers and iwm's.
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

/* as if_iwm.c has it before its registers' header */
#define le32_to_cpup(_a_) (le32toh(*(const uint32_t *)(_a_)))

#include <dev/pci/if_iwmreg.h>
#include <dev/pci/if_iwmvar.h>

#include "obsd_glue.h"
#include "tsw_drv.h"

extern const struct cfattach iwm_ca;

int	iwm_rx_pkt_valid(struct iwm_rx_packet *);
int	iwm_send_cmd_pdu_status(struct iwm_softc *, uint32_t, uint16_t, const void *, uint32_t *);

static struct iwm_softc	*iwm_sc;

static int
iwm_t_match(struct pci_attach_args *pa)
{
	return (*iwm_ca.ca_match)(NULL, NULL, pa);
}

static struct ieee80211com *
iwm_t_attach(struct pci_attach_args *pa, const char *xname)
{
	struct iwm_softc *sc;

	sc = malloc(iwm_ca.ca_devsize, M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		return NULL;
	strlcpy(sc->sc_dev.dv_xname, xname, sizeof(sc->sc_dev.dv_xname));
	printf("%s at pci%d dev %d function %d", xname, pa->pa_bus, pa->pa_device, pa->pa_function);
	(*iwm_ca.ca_attach)(NULL, &sc->sc_dev, pa);
	if (sc->sc_ic.ic_if.if_softc != sc) {
		printf("obsd: %s did not attach\n", xname);
		return NULL;
	}
	obsd_mountroot_run();
	if (!sc->attached) {
		printf("obsd: %s could not read its firmware and NVM\n", xname);
		return NULL;
	}
	iwm_sc = sc;
	return &sc->sc_ic;
}

/* iwm_protect_session in iwm_auth: twice the beacon interval, from the move to AUTH */
static int
iwm_t_te_on(void)
{
	return iwm_sc != NULL && (iwm_sc->sc_flags & IWM_FLAG_TE_ACTIVE) != 0;
}

static void
iwm_t_tx_counts(u_int *tx, u_int *retry)
{
	struct iwm_node *in;

	*tx = *retry = 0;
	if (iwm_sc == NULL || tsw_ic->ic_bss == NULL)
		return;
	in = (struct iwm_node *)tsw_ic->ic_bss;
	*tx = in->in_amn.amn_txcnt;
	*retry = in->in_amn.amn_retrycnt;
}

static int
iwm_t_mgmt_queued(void)
{
	int qid;

	if (iwm_sc == NULL)
		return 0;
	qid = (isset(iwm_sc->sc_enabled_capa, IWM_UCODE_TLV_CAPA_DQA_SUPPORT) ?
	    IWM_DQA_MIN_MGMT_QUEUE : 0) + EDCA_AC_BE;
	return iwm_sc->txq[qid].queued;
}

static int
iwm_t_scanning(void)
{
	return iwm_sc != NULL && (iwm_sc->sc_flags & (IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN)) != 0;
}

/*
 * The pairwise CCMP key into the card, with the card's answer: the same
 * ADD_STA_KEY command iwm_set_key sends (the key for the access point's
 * station, IWM_STATION_ID, key offset 0, the key's index, key mapping),
 * sent waiting for its status instead of without (IWM_CMD_ASYNC), so
 * that the key the card decrypts the frames to the station with is
 * known to be set, and said. The card decrypts what is sent to the
 * station alone with the keys it holds; what the station sends is
 * encrypted with the key given in each frame's command (iwm_tx), so a
 * key the card did not take shows only in what is received. The group
 * key stays in the layer (iwm_set_key: software).
 */
static int
iwm_t_ptk_to_card(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	struct iwm_softc *sc = iwm_sc;
	struct iwm_add_sta_key_cmd cmd;
	struct iwm_add_sta_key_cmd_v1 cmd_v1;
	struct iwm_add_sta_key_common *c;
	uint32_t status = 0;
	uint64_t t0;
	int v1, size, error;

	v1 = !isset(sc->sc_ucode_api, IWM_UCODE_TLV_API_TKIP_MIC_KEYS);
	memset(&cmd, 0, sizeof(cmd));
	memset(&cmd_v1, 0, sizeof(cmd_v1));
	c = v1 ? &cmd_v1.common : &cmd.common;
	c->key_flags = htole16(IWM_STA_KEY_FLG_CCM | IWM_STA_KEY_FLG_WEP_KEY_MAP |
	    ((k->k_id << IWM_STA_KEY_FLG_KEYID_POS) & IWM_STA_KEY_FLG_KEYID_MSK));
	if (ni->ni_flags & IEEE80211_NODE_MFP)
		c->key_flags |= htole16(IWM_STA_KEY_MFP);
	memcpy(c->key, k->k_key, MIN(sizeof(c->key), k->k_len));
	c->key_offset = 0;
	c->sta_id = IWM_STATION_ID;
	if (!v1)
		cmd.transmit_seq_cnt = htole64(k->k_tsc);
	size = v1 ? sizeof(cmd_v1) : sizeof(cmd);
	t0 = getnsecuptime();
	error = iwm_send_cmd_pdu_status(sc, IWM_ADD_STA_KEY, size, v1 ? (void *)&cmd_v1 : (void *)&cmd,
	    &status);
	printf("wifi: the card's pairwise key: ADD_STA_KEY (%s, %d bytes), station %u, key offset %u,"
	    " flags %04x, index %d: error %d, status %x (%s) in %d ms; the card's station flags %x\n",
	    v1 ? "version 1" : "version 2", size, c->sta_id, c->key_offset, letoh16(c->key_flags),
	    k->k_id, error, status,
	    error == 0 && (status & IWM_ADD_STA_STATUS_MASK) == IWM_ADD_STA_SUCCESS ? "taken" : "refused",
	    (int)((getnsecuptime() - t0) / 1000000ULL), sc->sc_flags & IWM_FLAG_STA_ACTIVE);
	explicit_bzero(&cmd, sizeof(cmd));
	explicit_bzero(&cmd_v1, sizeof(cmd_v1));
	if (error == 0 && (status & IWM_ADD_STA_STATUS_MASK) != IWM_ADD_STA_SUCCESS)
		error = EIO;
	return error;
}

/*
 * The frames the card received, read from the receive buffer as iwm
 * begins to read it (iwm_rx_pkt syncs the buffer first), before iwm
 * drops any: each MPDU the card wrote, with the rate of the last PHY
 * information before it, and the card's answers to what was sent (each
 * frame's or aggregate's status, and the access point's block-ack
 * answers). A buffer is looked at once, as iwm reads it (the ring's
 * index it is at).
 */
static int	pk_last = -1;
static uint32_t	pk_rate;	/* rate_n_flags of the last PHY information */

static void
iwm_t_rx_peek(bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	struct iwm_softc *sc = iwm_sc;
	struct iwm_rx_data *data;
	struct iwm_rx_packet *pkt;
	struct iwm_rx_mpdu_res_start *res;
	const size_t minsz = sizeof(pkt->len_n_flags) + sizeof(pkt->hdr);
	uint32_t offset = 0, plen, flen;
	uint64_t port_ns = tsw_port_ns();
	uint8_t *base;
	int cur;

	if (sc == NULL || off != 0 || len != IWM_RBUF_SIZE || sc->sc_mqrx_supported)
		return;
	cur = sc->rxq.cur;
	data = &sc->rxq.data[cur];
	if (data->map != map || data->m == NULL || cur == pk_last)
		return;
	pk_last = cur;
	base = mtod(data->m, uint8_t *);
	while (offset + minsz < IWM_RBUF_SIZE) {
		pkt = (struct iwm_rx_packet *)(base + offset);
		if (!iwm_rx_pkt_valid(pkt))
			break;
		plen = sizeof(pkt->len_n_flags) + iwm_rx_packet_len(pkt);
		if (plen < minsz || plen > IWM_RBUF_SIZE - offset)
			break;
		if (pkt->hdr.code == IWM_STATISTICS_NOTIFICATION)
			tsw_stats_size("iwm", plen - (uint32_t)minsz,
			    sizeof(struct iwm_notif_statistics), "IWM_STATISTICS_NTFY_API_S_VER_8");
		if (pkt->hdr.code == IWM_TX_CMD && plen >= minsz + sizeof(struct iwm_tx_resp) &&
		    tsw_txr_say()) {
			const struct iwm_tx_resp *tr = (const struct iwm_tx_resp *)pkt->data;
			uint32_t ts = le16toh(tr->status.status) & IWM_TX_STATUS_MSK;

			printf("wifi: the card sent: queue %u, %u frame%s, status %02x (%s), tried again %u,"
			    " RTS failed %u, rate %08x, TID %u, sequence %u, %u ms after the port opened\n",
			    pkt->hdr.qid & 0x7f, tr->frame_count, tr->frame_count == 1 ? "" : "s", ts,
			    ts == IWM_TX_STATUS_SUCCESS || ts == IWM_TX_STATUS_DIRECT_DONE ? "acknowledged" :
			    ts == IWM_TX_STATUS_FAIL_SHORT_LIMIT || ts == IWM_TX_STATUS_FAIL_LONG_LIMIT ?
			    "not acknowledged: the retry limit" : "not sent", tr->failure_frame, tr->failure_rts,
			    le32toh(tr->initial_rate), tr->ra_tid & 0x0f, le16toh(tr->seq_ctl) >> 4,
			    (int)((getnsecuptime() - port_ns) / 1000000ULL));
		} else if (pkt->hdr.code == IWM_BA_NOTIF && plen >= minsz + sizeof(struct iwm_ba_notif) &&
		    tsw_txr_say()) {
			const struct iwm_ba_notif *ba = (const struct iwm_ba_notif *)pkt->data;

			printf("wifi: the card sent: block-ack answer of %s, TID %u, %u of %u frames"
			    " acknowledged, the window from %u, queue %u\n", ether_sprintf((uint8_t *)ba->sta_addr),
			    ba->tid, ba->txed_2_done, ba->txed, le16toh(ba->seq_ctl) >> 4, le16toh(ba->scd_flow));
		} else if (pkt->hdr.code == IWM_REPLY_RX_PHY_CMD && plen >= minsz + sizeof(struct iwm_rx_phy_info)) {
			pk_rate = le32toh(((struct iwm_rx_phy_info *)pkt->data)->rate_n_flags);
		} else if (pkt->hdr.code == IWM_REPLY_RX_MPDU_CMD && plen >= minsz + sizeof(*res)) {
			res = (struct iwm_rx_mpdu_res_start *)pkt->data;
			flen = le16toh(res->byte_count);
			if (flen >= sizeof(struct ieee80211_frame) && minsz + sizeof(*res) + flen + 4 <= plen)
				tsw_rx_mpdu((const struct ieee80211_frame *)(pkt->data + sizeof(*res)), flen,
				    le32toh(*(uint32_t *)(pkt->data + sizeof(*res) + flen)), pk_rate);
		}
		offset += roundup(plen, IWM_FH_RSCSR_FRAME_ALIGN);
	}
}

/*
 * The card's own counts, as its last statistics notification gave them
 * (iwm keeps it in sc_stats): what its receiver decoded well and badly
 * at each kind of rate, the data it took as the station's and what its
 * filter dropped, and the acknowledgements it sent. A frame lost below
 * iwm shows here.
 */
static void
iwm_t_card_stats(void)
{
	const struct iwm_statistics_rx *r;

	if (iwm_sc == NULL)
		return;
	r = &iwm_sc->sc_stats.rx;
	printf("wifi: receive: the card: OFDM good %u, CRC errors %u, PLCP errors %u;"
	    " CCK good %u, CRC errors %u; HT good %u, CRC errors %u, PLCP errors %u,"
	    " MCS not supported %u, aggregates %u (good %u)\n",
	    letoh32(r->ofdm.crc32_good), letoh32(r->ofdm.crc32_err),
	    letoh32(r->ofdm.plcp_err), letoh32(r->cck.crc32_good),
	    letoh32(r->cck.crc32_err), letoh32(r->ofdm_ht.crc32_good),
	    letoh32(r->ofdm_ht.crc32_err), letoh32(r->ofdm_ht.plcp_err),
	    letoh32(r->ofdm_ht.unsupport_mcs), letoh32(r->ofdm_ht.agg_cnt),
	    letoh32(r->ofdm_ht.agg_crc32_good));
	printf("wifi: receive: the card: data to the station %u, dropped by its filter %u,"
	    " of another BSSID %u, beacons %u (missed %u); acknowledgements sent %u"
	    " (CCK %u), block-ack answers %u\n",
	    letoh32(r->general.directed_data_mpdu), letoh32(r->general.filtered_frames),
	    letoh32(r->general.non_bssid_frames), letoh32(r->general.channel_beacons),
	    letoh32(r->general.num_missed_bcon), letoh32(r->ofdm.sent_ack_cnt),
	    letoh32(r->cck.sent_ack_cnt), letoh32(r->ofdm.sent_ba_rsp_cnt));
}

static void
iwm_t_debug(int level)
{
#ifdef IWM_DEBUG
	extern int iwm_debug;

	iwm_debug = level;
#endif
}

const struct tsw_drv tsw_iwm_drv = {
	.name = "iwm",
	.min_dbm = IWM_MIN_DBM,
	.match = iwm_t_match,
	.attach = iwm_t_attach,
	.te_on = iwm_t_te_on,
	.tx_counts = iwm_t_tx_counts,
	.mgmt_queued = iwm_t_mgmt_queued,
	.scanning = iwm_t_scanning,
	.ptk_to_card = iwm_t_ptk_to_card,
	.rx_peek = iwm_t_rx_peek,
	.card_stats = iwm_t_card_stats,
	.port_say = NULL,
	.debug = iwm_t_debug,
};
