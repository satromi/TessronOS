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
 *	athx_htt.c (x64 PC, WIFI_NET80211=obsd)
 *	The data frames (design 9.6.9), by HTT on HTC's HTT endpoint:
 *
 *	- received: the chip's MAC writes each MSDU into a buffer of the host's
 *	  receive ring (an address per entry, an index the host moves), a
 *	  descriptor in front of the frame; an RX_IND message says how many
 *	  MPDUs are ready. The firmware takes the 802.11 header off (native
 *	  Wi-Fi decapsulation: a short 802.11 header in place of the original,
 *	  no QoS control, no IV): the header as received is in the
 *	  descriptor, and the frame is put back together from it. A frame the
 *	  firmware decrypted reaches net80211 without its IV and with the
 *	  protected bit off, marked as decrypted by the hardware;
 *	- sent: an HTT TX_FRM descriptor with the HTC header in front, a list of
 *	  fragments (one: the frame's copy), and the frame's first bytes for
 *	  the firmware's classification; data in native Wi-Fi form (the QoS
 *	  control taken out, the TID in the descriptor), management frames as
 *	  they are. The firmware encrypts a frame with the key of its peer
 *	  unless it is told not to (a frame without the protected bit);
 *	- the firmware's own messages: its version, the peers it made (peer
 *	  map), the keys it installed (security indication), the frames sent
 *	  (TX_COMPL_IND).
 *
 *	The receive ring's buffers are the host's for good: a frame is copied
 *	into an mbuf and its buffer goes back on the ring at once.
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

/* What of the frame the firmware's classification engine reads: 802.11 + QoS + HT, 802.1Q, LLC/SNAP, DSCP */
#define ATHX_HTT_PREFETCH_LEN	(36 + 4 + 8 + 2)

#define RX_DESC_SIZE		((int)sizeof(struct htt_rx_desc_v1))
#define RX_MSDU_SIZE		(HTT_RX_BUF_SIZE - RX_DESC_SIZE)

static void	athx_htt_t2h(struct athx_softc *, uint8_t *, int);

/* ---------------------------------------------------------------- setting up */

int
athx_htt_connect(struct athx_softc *sc)
{
	return athx_htc_connect(sc, ATH10K_HTC_SVC_ID_HTT_DATA_MSG, athx_htt_t2h, &sc->htt.eid);
}

/* The receive ring, the frames' descriptors and copies, HTT's own buffers: once */
int
athx_htt_alloc(struct athx_softc *sc)
{
	struct athx_htt *htt = &sc->htt;
	int i;

	if (htt->bufs.n != 0)
		goto reset;
	htt->size = HTT_RX_RING_SIZE;
	htt->size_mask = htt->size - 1;
	htt->fill_level = HTT_RX_RING_FILL_LEVEL;
	if (athx_dma_alloc(sc, &htt->ring, htt->size * sizeof(uint32_t), PAGE_SIZE) != 0 ||
	    athx_dma_alloc(sc, &htt->alloc_idx, sizeof(uint32_t), 8) != 0 ||
	    athx_bufs_alloc(sc, &htt->bufs, htt->fill_level + 1, HTT_RX_BUF_SIZE) != 0 ||
	    athx_dma_alloc(sc, &htt->txbuf, ATHX_TX_NBUF * sizeof(struct ath10k_htt_txbuf_32), PAGE_SIZE) != 0 ||
	    athx_bufs_alloc(sc, &htt->txdata, ATHX_TX_NBUF, ATHX_TX_BUFSZ) != 0 ||
	    athx_bufs_alloc(sc, &htt->ctl, ATHX_HTT_CTL_N, ATHX_HTT_CTL_SZ) != 0)
		return ENOMEM;
	htt->ring_buf = mallocarray(htt->size, sizeof(int), M_DEVBUF, M_NOWAIT);
	htt->free_buf = mallocarray(htt->bufs.n, sizeof(int), M_DEVBUF, M_NOWAIT);
	if (htt->ring_buf == NULL || htt->free_buf == NULL)
		return ENOMEM;
 reset:
	for (i = 0; i < htt->size; i++)
		htt->ring_buf[i] = -1;
	for (i = 0; i < htt->bufs.n; i++)
		htt->free_buf[i] = i;
	htt->nfree = htt->bufs.n;
	memset(htt->ring.kva, 0, htt->ring.size);
	*(volatile uint32_t *)htt->alloc_idx.kva = 0;
	htt->fill_cnt = 0;
	htt->sw_rd_idx = 0;
	htt->mpdus_ready = 0;
	htt->confused = 0;
	memset(htt->tx_busy, 0, sizeof(htt->tx_busy));
	memset(htt->ctl_busy, 0, sizeof(htt->ctl_busy));
	htt->tx_pending = 0;
	htt->ver_received = 0;
	return 0;
}

static struct htt_rx_desc_v1 *
rx_desc_of(struct athx_htt *htt, int b)
{
	return (struct htt_rx_desc_v1 *)athx_buf_va(&htt->bufs, b);
}

/* num buffers more on the ring, the index the chip reads moved after them */
static int
rx_ring_fill_n(struct athx_softc *sc, int num)
{
	struct athx_htt *htt = &sc->htt;
	uint32_t *ring = (uint32_t *)htt->ring.kva;
	int idx, b;

	idx = le32toh(*(volatile uint32_t *)htt->alloc_idx.kva);
	if (idx < 0 || idx >= htt->size) {
		printf("%s: rx ring index is not valid, firmware malfunctioning?\n", sc->sc_dev.dv_xname);
		return ENOMEM;
	}
	while (num > 0 && htt->nfree > 0) {
		b = htt->free_buf[--htt->nfree];
		/* the attention word cleared before the buffer is given */
		rx_desc_of(htt, b)->attention.flags = 0;
		ring[idx] = htole32(athx_buf_pa(&htt->bufs, b));
		htt->ring_buf[idx] = b;
		htt->fill_cnt++;
		num--;
		idx = (idx + 1) & htt->size_mask;
	}
	membar_producer();
	bus_dmamap_sync(sc->sc_dmat, htt->ring.map, 0, htt->ring.size, BUS_DMASYNC_PREWRITE);
	*(volatile uint32_t *)htt->alloc_idx.kva = htole32(idx);
	return num > 0 ? ENOMEM : 0;
}

int
athx_htt_rx_ring_refill(struct athx_softc *sc)
{
	return rx_ring_fill_n(sc, sc->htt.fill_level - sc->htt.fill_cnt);
}

/* An HTT message of the host's (version, ring, aggregation): its body after the message type */
static int
htt_send(struct athx_softc *sc, uint8_t msg_type, const void *body, int len)
{
	uint8_t buf[ATHX_HTT_CTL_SZ - sizeof(struct ath10k_htc_hdr)];

	if (len + 1 > (int)sizeof(buf))
		return EMSGSIZE;
	memset(buf, 0, sizeof(buf));
	buf[0] = msg_type;
	memcpy(buf + 1, body, len);
	return athx_htc_send(sc, sc->htt.eid, buf, roundup(1 + len, 4));
}

/*
 * HTT set up: its version asked for and checked (2 or 3), the receive
 * ring given to the firmware with the offsets of the descriptor's parts,
 * the sizes of A-MPDUs and A-MSDUs
 */
int
athx_htt_setup(struct athx_softc *sc)
{
	struct athx_htt *htt = &sc->htt;
	struct {
		struct htt_rx_ring_setup_hdr hdr;
		struct htt_rx_ring_setup_ring32 ring;
	} __packed rs;
	struct htt_aggr_conf ac;
	struct htt_ver_req vr;
	uint16_t flags;
	int ret;

	memset(&vr, 0, sizeof(vr));
	htt->ver_received = 0;
	ret = htt_send(sc, HTT_H2T_MSG_TYPE_VERSION_REQ, &vr, sizeof(vr));
	if (ret)
		return ret;
	ret = athx_wait(sc, &htt->ver_received, "athxhtt", 3000);
	if (ret) {
		printf("%s: htt version request timed out\n", sc->sc_dev.dv_xname);
		return ret;
	}
	printf("%s: htt target version %d.%d\n", sc->sc_dev.dv_xname, htt->ver_major, htt->ver_minor);
	if (htt->ver_major != 2 && htt->ver_major != 3) {
		printf("%s: unsupported htt major version %d\n", sc->sc_dev.dv_xname, htt->ver_major);
		return EOPNOTSUPP;
	}

	memset(&rs, 0, sizeof(rs));
	rs.hdr.num_rings = 1;
	flags = HTT_RX_RING_FLAGS_MAC80211_HDR | HTT_RX_RING_FLAGS_MSDU_PAYLOAD | HTT_RX_RING_FLAGS_PPDU_START |
	    HTT_RX_RING_FLAGS_PPDU_END | HTT_RX_RING_FLAGS_MPDU_START | HTT_RX_RING_FLAGS_MPDU_END |
	    HTT_RX_RING_FLAGS_MSDU_START | HTT_RX_RING_FLAGS_MSDU_END | HTT_RX_RING_FLAGS_RX_ATTENTION |
	    HTT_RX_RING_FLAGS_FRAG_INFO | HTT_RX_RING_FLAGS_UNICAST_RX | HTT_RX_RING_FLAGS_MULTICAST_RX |
	    HTT_RX_RING_FLAGS_CTRL_RX | HTT_RX_RING_FLAGS_MGMT_RX | HTT_RX_RING_FLAGS_NULL_RX |
	    HTT_RX_RING_FLAGS_PHY_DATA_RX;
	rs.ring.fw_idx_shadow_reg_paddr = htole32((uint32_t)htt->alloc_idx.paddr);
	rs.ring.rx_ring_base_paddr = htole32((uint32_t)htt->ring.paddr);
	rs.ring.rx_ring_len = htole16(htt->size);
	rs.ring.rx_ring_bufsize = htole16(HTT_RX_BUF_SIZE);
	rs.ring.flags = htole16(flags);
	rs.ring.fw_idx_init_val = htole16(le32toh(*(volatile uint32_t *)htt->alloc_idx.kva));
#define desc_offset(x) htole16(offsetof(struct htt_rx_desc_v1, x) / 4)
	rs.ring.offsets.mac80211_hdr_offset = desc_offset(rx_hdr_status);
	rs.ring.offsets.msdu_payload_offset = desc_offset(msdu_payload);
	rs.ring.offsets.ppdu_start_offset = desc_offset(ppdu_start);
	rs.ring.offsets.ppdu_end_offset = desc_offset(ppdu_end);
	rs.ring.offsets.mpdu_start_offset = desc_offset(mpdu_start);
	rs.ring.offsets.mpdu_end_offset = desc_offset(mpdu_end);
	rs.ring.offsets.msdu_start_offset = desc_offset(msdu_start);
	rs.ring.offsets.msdu_end_offset = desc_offset(msdu_end);
	rs.ring.offsets.rx_attention_offset = desc_offset(attention);
	rs.ring.offsets.frag_info_offset = desc_offset(frag_info);
#undef desc_offset
	ret = htt_send(sc, HTT_H2T_MSG_TYPE_RX_RING_CFG, &rs, sizeof(rs));
	if (ret) {
		printf("%s: failed to setup rx ring: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}

	ac.max_num_ampdu_subframes = ATH10K_HTT_MAX_NUM_AMPDU_DEFAULT;
	ac.max_num_amsdu_subframes = ATH10K_HTT_MAX_NUM_AMSDU_DEFAULT;
	ret = htt_send(sc, HTT_H2T_MSG_TYPE_AGGR_CFG, &ac, sizeof(ac));
	if (ret)
		printf("%s: failed to setup amsdu/ampdu limit: %d\n", sc->sc_dev.dv_xname, ret);
	return ret;
}

/* ---------------------------------------------------------------- sending */

/* Engine 4's completions: HTT's own buffers given back (a frame's are given back by TX_COMPL_IND) */
void
athx_htt_tx_cb(struct athx_softc *sc, struct athx_ce *ce)
{
	struct athx_ce_ring *r = ce->src;
	unsigned int si;
	uint32_t ri;
	uint8_t *busy;

	for (;;) {
		si = r->sw_index;
		if (r->hw_index == si) {
			ri = athx_read32(sc, ce->ctrl_addr + CE_CURRENT_SRRI);
			if (ri == 0xffffffff)
				return;
			r->hw_index = ri & r->nentries_mask;
			if (r->hw_index == si)
				return;
		}
		busy = r->ctx[si];
		r->ctx[si] = NULL;
		r->base[si].nbytes = 0;
		r->sw_index = CE_RING_IDX_INCR(r->nentries_mask, si);
		if (busy != NULL)
			*busy = 0;
	}
}

static int
htt_frm_has_freq(struct athx_softc *sc)
{
	return sc->htt.ver_major >= 3 && sc->htt.ver_minor >= 4;
}

/*
 * A frame to the firmware: copied into its buffer, its descriptor and
 * HTC header written, both sent on engine 4 in two pieces (the
 * descriptor, then the frame's first bytes; the firmware fetches the rest
 * by the fragment list). txmode: ATH10K_HW_TXRX_NATIVE_WIFI or _MGMT.
 * 0, ENOBUFS when every buffer is in flight
 */
int
athx_htt_tx(struct athx_softc *sc, int vdev, struct mbuf *m, int txmode, int no_encrypt)
{
	struct athx_htt *htt = &sc->htt;
	struct ath10k_htt_txbuf_32 *txbuf;
	struct ieee80211_frame *wh;
	uint32_t paddr[2], txbuf_paddr, data_paddr;
	uint16_t len[2], flags1 = 0;
	uint8_t flags0 = 0, *data, tid;
	int id, flen, hdrlen, prefetch, ret;

	for (id = 0; id < ATHX_TX_NBUF && htt->tx_busy[id]; id++)
		;
	if (id == ATHX_TX_NBUF)
		return ENOBUFS;
	flen = m->m_pkthdr.len;
	if (flen > ATHX_TX_BUFSZ || flen < (int)sizeof(*wh))
		return EMSGSIZE;
	data = athx_buf_va(&htt->txdata, id);
	data_paddr = athx_buf_pa(&htt->txdata, id);
	m_copydata(m, 0, flen, data);
	wh = (struct ieee80211_frame *)data;
	if (txmode == ATH10K_HW_TXRX_MGMT) {
		tid = HTT_DATA_TX_EXT_TID_MGMT;
	} else if (ieee80211_has_qos(wh)) {
		/* native Wi-Fi: the QoS control out of the header, its TID into the descriptor */
		hdrlen = ieee80211_get_hdrlen(wh);
		tid = ((uint8_t *)wh)[hdrlen - 2] & IEEE80211_QOS_TID;
		if ((wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) == (IEEE80211_FC0_SUBTYPE_QOS | IEEE80211_FC0_SUBTYPE_NODATA))
			tid = HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST;
		memmove(data + 2, data, hdrlen - 2);
		data += 2;
		data_paddr += 2;
		flen -= 2;
		wh = (struct ieee80211_frame *)data;
		wh->i_fc[0] &= ~IEEE80211_FC0_SUBTYPE_QOS;
	} else {
		tid = HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST;
	}
	bus_dmamap_sync(sc->sc_dmat, htt->txdata.piece[id / htt->txdata.per].map, 0,
	    htt->txdata.piece[id / htt->txdata.per].size, BUS_DMASYNC_PREWRITE);

	txbuf = (struct ath10k_htt_txbuf_32 *)htt->txbuf.kva + id;
	txbuf_paddr = (uint32_t)htt->txbuf.paddr + sizeof(*txbuf) * id;
	memset(txbuf, 0, sizeof(*txbuf));
	prefetch = roundup(MIN(ATHX_HTT_PREFETCH_LEN, flen), 4);
	flags0 |= HTT_DATA_TX_DESC_FLAGS0_MAC_HDR_PRESENT;
	flags0 |= SM(txmode, HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE);
	if (txmode == ATH10K_HW_TXRX_MGMT) {
		txbuf->cmd_tx.frags_paddr = htole32(data_paddr);
	} else {
		txbuf->frags[0].dword_addr.paddr = htole32(data_paddr);
		txbuf->frags[0].dword_addr.len = htole32(flen);
		txbuf->frags[1].dword_addr.paddr = 0;
		txbuf->frags[1].dword_addr.len = 0;
		txbuf->cmd_tx.frags_paddr = htole32(txbuf_paddr);
	}
	if (no_encrypt)
		flags0 |= HTT_DATA_TX_DESC_FLAGS0_NO_ENCRYPT;
	flags1 |= SM((uint16_t)vdev, HTT_DATA_TX_DESC_FLAGS1_VDEV_ID);
	flags1 |= SM((uint16_t)tid, HTT_DATA_TX_DESC_FLAGS1_EXT_TID);
	/* the firmware is not to send the host frames for inspection: there is nothing to do with them */
	flags1 |= HTT_DATA_TX_DESC_FLAGS1_POSTPONED;
	txbuf->htc_hdr.eid = htt->eid;
	txbuf->htc_hdr.len = htole16(sizeof(txbuf->cmd_hdr) + sizeof(txbuf->cmd_tx) + prefetch);
	txbuf->htc_hdr.flags = 0;
	txbuf->cmd_hdr.msg_type = HTT_H2T_MSG_TYPE_TX_FRM;
	txbuf->cmd_tx.flags0 = flags0;
	txbuf->cmd_tx.flags1 = htole16(flags1);
	txbuf->cmd_tx.len = htole16(flen);
	txbuf->cmd_tx.id = htole16(id);
	if (htt_frm_has_freq(sc)) {
		txbuf->cmd_tx.offchan_tx.peerid = htole16(HTT_INVALID_PEERID);
		txbuf->cmd_tx.offchan_tx.freq = 0;
	} else {
		txbuf->cmd_tx.peerid = htole32(HTT_INVALID_PEERID);
	}
	bus_dmamap_sync(sc->sc_dmat, htt->txbuf.map, 0, htt->txbuf.size, BUS_DMASYNC_PREWRITE);
	paddr[0] = txbuf_paddr + sizeof(txbuf->frags);
	len[0] = sizeof(txbuf->htc_hdr) + sizeof(txbuf->cmd_hdr) + sizeof(txbuf->cmd_tx);
	paddr[1] = data_paddr;
	len[1] = prefetch;
	ret = athx_ce_send_sg(sc, sc->ep[htt->eid].ul_pipe, paddr, len, 2, NULL, 0);
	if (ret)
		return ENOBUFS;
	htt->tx_busy[id] = 1;
	htt->tx_pending++;
	return 0;
}

/* ---------------------------------------------------------------- receiving */

/* The next buffer of the ring, taken off it */
static int
rx_netbuf_pop(struct athx_softc *sc)
{
	struct athx_htt *htt = &sc->htt;
	uint32_t *ring = (uint32_t *)htt->ring.kva;
	int idx = htt->sw_rd_idx, b;

	if (htt->fill_cnt == 0) {
		printf("%s: tried to pop a buffer from an empty rx ring\n", sc->sc_dev.dv_xname);
		return -1;
	}
	b = htt->ring_buf[idx];
	htt->ring_buf[idx] = -1;
	ring[idx] = 0;
	htt->sw_rd_idx = (idx + 1) & htt->size_mask;
	htt->fill_cnt--;
	if (b >= 0)
		bus_dmamap_sync(sc->sc_dmat, htt->bufs.piece[b / htt->bufs.per].map, 0,
		    htt->bufs.piece[b / htt->bufs.per].size, BUS_DMASYNC_POSTREAD);
	return b;
}

static void
rx_buf_free(struct athx_htt *htt, int b)
{
	if (b >= 0 && htt->nfree < htt->bufs.n)
		htt->free_buf[htt->nfree++] = b;
}

#define ATHX_AMSDU_MAX	32

struct athx_msdu {
	int	buf;		/* its first buffer (the descriptor and the frame) */
	int	len;		/* of the frame in that buffer */
	int	chained;	/* buffers after it, without descriptors */
};

/*
 * The MSDUs of one MPDU off the ring: <0 the ring is out of step, 0 none
 * chained, 1 some chained (a frame longer than one buffer)
 */
static int
rx_amsdu_pop(struct athx_softc *sc, struct athx_msdu *ms, int *nms, int *bufs, int *nbufs)
{
	struct athx_htt *htt = &sc->htt;
	struct htt_rx_desc_v1 *rxd;
	int chaining = 0, b, msdu_len, chained, last;

	*nms = *nbufs = 0;
	for (;;) {
		b = rx_netbuf_pop(sc);
		if (b < 0)
			return -ENOENT;
		bufs[(*nbufs)++] = b;
		rxd = rx_desc_of(htt, b);
		if (!(le32toh(rxd->attention.flags) & RX_ATTENTION_FLAGS_MSDU_DONE))
			return -EIO;
		msdu_len = MS(le32toh(rxd->msdu_start.common.info0), RX_MSDU_START_INFO0_MSDU_LENGTH);
		if (le32toh(rxd->attention.flags) &
		    (RX_ATTENTION_FLAGS_MPDU_LENGTH_ERR | RX_ATTENTION_FLAGS_MSDU_LENGTH_ERR))
			msdu_len = 0;
		chained = rxd->frag_info.common.ring2_more_count;
		if (*nms < ATHX_AMSDU_MAX) {
			ms[*nms].buf = b;
			ms[*nms].len = MIN(msdu_len, RX_MSDU_SIZE);
			ms[*nms].chained = chained;
			(*nms)++;
		}
		while (chained--) {
			b = rx_netbuf_pop(sc);
			if (b < 0)
				return -ENOENT;
			if (*nbufs < ATHX_AMSDU_MAX * 4)
				bufs[(*nbufs)++] = b;
			chaining = 1;
		}
		last = le32toh(rxd->msdu_end.common.info0) & RX_MSDU_END_INFO0_LAST_MSDU;
		if (last || *nbufs >= ATHX_AMSDU_MAX * 4)
			break;
	}
	return chaining;
}

/* The receiver and sender of an 802.11 header, by its DS bits */
static uint8_t *
wh_da(struct ieee80211_frame *wh)
{
	return (wh->i_fc[1] & IEEE80211_FC1_DIR_TODS) ? wh->i_addr3 : wh->i_addr1;
}

static uint8_t *
wh_sa(struct ieee80211_frame *wh)
{
	if (!(wh->i_fc[1] & IEEE80211_FC1_DIR_FROMDS))
		return wh->i_addr2;
	if (wh->i_fc[1] & IEEE80211_FC1_DIR_TODS)
		return ((struct ieee80211_frame_addr4 *)wh)->i_addr4;
	return wh->i_addr3;
}

/*
 * One MPDU taken off the ring: its MSDUs put back together as 802.11
 * frames (the header as received, with each MSDU's DA and SA, without the
 * IV of a decrypted frame) and given to net80211; the buffers back on the
 * ring
 */
static int
rx_handle_amsdu(struct athx_softc *sc)
{
	struct athx_htt *htt = &sc->htt;
	struct athx_msdu ms[ATHX_AMSDU_MAX];
	int bufs[ATHX_AMSDU_MAX * 4], nms, nbufs, ret, i, hdr_len, nw_len, plen, decrypted, mgmt;
	struct htt_rx_desc_v1 *first, *last;
	struct ieee80211_frame *hdr, *nw;
	uint8_t first_hdr[RX_HTT_HDR_STATUS_LEN], *payload, da[ETH_ALEN], sa[ETH_ALEN];
	uint32_t attention, enctype, decap;
	static int rssi = -95;
	struct mbuf *m;

	if (htt->confused)
		return EIO;
	ret = rx_amsdu_pop(sc, ms, &nms, bufs, &nbufs);
	if (ret < 0) {
		printf("%s: rx ring became corrupted: %d\n", sc->sc_dev.dv_xname, ret);
		htt->confused = 1;
		for (i = 0; i < nbufs; i++)
			rx_buf_free(htt, bufs[i]);
		return EIO;
	}
	if (nms == 0)
		goto out;
	first = rx_desc_of(htt, ms[0].buf);
	last = rx_desc_of(htt, ms[nms - 1].buf);
	if (le32toh(first->attention.flags) & RX_ATTENTION_FLAGS_FIRST_MPDU)
		rssi = ATHX_NOISE_FLOOR + first->ppdu_start.rssi_comb;
	htt->n_rx_msdu += nms;
	if (ret > 0) {
		/* chained frames are put back together only when they are raw, which these are not */
		htt->n_rx_drop += nms;
		goto out;
	}
	attention = le32toh(last->attention.flags);
	mgmt = (le32toh(first->attention.flags) & RX_ATTENTION_FLAGS_MGMT_TYPE) != 0;
	enctype = MS(le32toh(first->mpdu_start.info0), RX_MPDU_START_INFO0_ENCRYPT_TYPE);
	if (mgmt || (attention & (RX_ATTENTION_FLAGS_FCS_ERR | RX_ATTENTION_FLAGS_TKIP_MIC_ERR))) {
		/* management frames come by WMI */
		if (attention & RX_ATTENTION_FLAGS_TKIP_MIC_ERR)
			sc->sc_ic.ic_stats.is_rx_locmicfail++;
		htt->n_rx_drop += nms;
		goto out;
	}
	decrypted = enctype != HTT_RX_MPDU_ENCRYPT_NONE && !(attention & RX_ATTENTION_FLAGS_DECRYPT_ERR) &&
	    !(attention & RX_ATTENTION_FLAGS_PEER_IDX_INVALID);
	memcpy(first_hdr, first->rx_hdr_status, sizeof(first_hdr));
	hdr = (struct ieee80211_frame *)first_hdr;
	hdr_len = ieee80211_get_hdrlen(hdr);
	if (hdr_len > (int)sizeof(first_hdr)) {
		htt->n_rx_drop += nms;
		goto out;
	}
	/* each A-MSDU subframe goes up as an MPDU of its own: the A-MSDU bit off */
	if (ieee80211_has_qos(hdr))
		first_hdr[hdr_len - 2] &= ~IEEE80211_QOS_AMSDU;
	if (decrypted)
		hdr->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;
	for (i = 0; i < nms; i++) {
		struct htt_rx_desc_v1 *rxd = rx_desc_of(htt, ms[i].buf);

		decap = MS(le32toh(rxd->msdu_start.common.info1), RX_MSDU_START_INFO1_DECAP_FORMAT);
		if (decap != RX_MSDU_DECAP_NATIVE_WIFI) {
			htt->n_rx_drop++;
			continue;
		}
		payload = rxd->msdu_payload;
		if (ms[i].len < (int)sizeof(struct ieee80211_frame)) {
			htt->n_rx_drop++;
			continue;
		}
		nw = (struct ieee80211_frame *)payload;
		nw_len = ieee80211_get_hdrlen(nw);
		if (!fw_feature_no_nwifi_pad(sc))
			nw_len = roundup(nw_len, 4);
		if (nw_len > ms[i].len) {
			htt->n_rx_drop++;
			continue;
		}
		memcpy(da, wh_da(nw), ETH_ALEN);
		memcpy(sa, wh_sa(nw), ETH_ALEN);
		plen = ms[i].len - nw_len;
		MGETHDR(m, M_DONTWAIT, MT_DATA);
		if (m == NULL) {
			htt->n_rx_drop++;
			continue;
		}
		if (hdr_len + plen > MHLEN) {
			MCLGETL(m, M_DONTWAIT, hdr_len + plen);
			if (!(m->m_flags & M_EXT)) {
				m_freem(m);
				htt->n_rx_drop++;
				continue;
			}
		}
		memcpy(mtod(m, uint8_t *), first_hdr, hdr_len);
		memcpy(mtod(m, uint8_t *) + hdr_len, payload + nw_len, plen);
		m->m_len = m->m_pkthdr.len = hdr_len + plen;
		memcpy(wh_da(mtod(m, struct ieee80211_frame *)), da, ETH_ALEN);
		memcpy(wh_sa(mtod(m, struct ieee80211_frame *)), sa, ETH_ALEN);
		athx_data_rx(sc, m, rssi, (decrypted ? IEEE80211_RXI_HWDEC : 0) |
		    (i > 0 ? IEEE80211_RXI_SAME_SEQ : 0));
	}
 out:
	for (i = 0; i < nbufs; i++)
		rx_buf_free(htt, bufs[i]);
	return 0;
}

/* What the interrupt left: the MPDUs RX_IND said were ready, the ring filled again */
void
athx_htt_txrx_compl_task(struct athx_softc *sc)
{
	struct athx_htt *htt = &sc->htt;
	int n = 0;

	while (htt->mpdus_ready > 0 && n++ < 512) {
		if (rx_handle_amsdu(sc) != 0)
			break;
		htt->mpdus_ready--;
	}
	if (htt->bufs.n != 0 && !htt->confused && htt->fill_cnt < htt->fill_level)
		(void)rx_ring_fill_n(sc, MIN(ATH10K_HTT_MAX_NUM_REFILL, htt->fill_level - htt->fill_cnt));
}

/* ---------------------------------------------------------------- the firmware's messages */

static void
htt_tx_compl_ind(struct athx_softc *sc, const uint8_t *body, int len)
{
	struct athx_htt *htt = &sc->htt;
	const struct htt_data_tx_completion *c = (const struct htt_data_tx_completion *)body;
	int status, i, n, freed = 0;
	uint16_t id;

	if (len < (int)sizeof(*c))
		return;
	status = MS(c->flags, HTT_DATA_TX_STATUS);
	n = c->num_msdus;
	if (len < (int)sizeof(*c) + n * 2)
		return;
	for (i = 0; i < n; i++) {
		id = le16toh(c->msdus[i]);
		if (id >= ATHX_TX_NBUF || htt->tx_busy[id] != 1)
			continue;
		htt->tx_busy[id] = 0;
		htt->tx_pending--;
		freed++;
		htt->n_tx_compl++;
		if (status == HTT_DATA_TX_STATUS_NO_ACK)
			htt->n_tx_noack++;
		else if (status != HTT_DATA_TX_STATUS_OK)
			htt->n_tx_discard++;
	}
	if (freed)
		athx_tx_done(sc);
}

/* A message of the firmware's on the HTT endpoint, by its type */
static void
athx_htt_t2h(struct athx_softc *sc, uint8_t *data, int len)
{
	struct athx_htt *htt = &sc->htt;
	const uint8_t *body = data + 1;
	int blen = len - 1;

	if (len < 1)
		return;
	switch (data[0]) {
	case HTT_TLV_T2H_MSG_TYPE_VERSION_CONF:
		if (blen >= (int)sizeof(struct htt_ver_resp)) {
			const struct htt_ver_resp *v = (const struct htt_ver_resp *)body;

			htt->ver_major = v->major;
			htt->ver_minor = v->minor;
			htt->ver_received = 1;
			wakeup(&htt->ver_received);
		}
		break;
	case HTT_TLV_T2H_MSG_TYPE_RX_IND: {
		const struct htt_rx_indication *rx = (const struct htt_rx_indication *)body;
		const struct htt_rx_indication_mpdu_range *r;
		int i, nr, off, cnt = 0;

		if (blen < (int)(sizeof(rx->hdr) + sizeof(rx->ppdu) + sizeof(rx->prefix)))
			break;
		nr = MS(le32toh(rx->hdr.info1), HTT_RX_INDICATION_INFO1_NUM_MPDU_RANGES);
		off = sizeof(rx->hdr) + sizeof(rx->ppdu) + sizeof(rx->prefix) +
		    roundup(le16toh(rx->prefix.fw_rx_desc_bytes), 4);
		if (off + nr * (int)sizeof(*r) > blen)
			break;
		r = (const struct htt_rx_indication_mpdu_range *)(body + off);
		for (i = 0; i < nr; i++)
			cnt += r[i].mpdu_count;
		htt->mpdus_ready += cnt;
		htt->n_rx_ind++;
		break;
	}
	case HTT_TLV_T2H_MSG_TYPE_RX_FRAG_IND:
		htt->mpdus_ready++;
		break;
	case HTT_TLV_T2H_MSG_TYPE_PEER_MAP:
		if (blen >= (int)sizeof(struct htt_rx_peer_map)) {
			const struct htt_rx_peer_map *pm = (const struct htt_rx_peer_map *)body;

			htt->n_peer_map++;
			printf("%s: htt peer map vdev %d peer %s id %d\n", sc->sc_dev.dv_xname, pm->vdev_id,
			    ether_sprintf((uint8_t *)pm->addr), le16toh(pm->peer_id));
			if (memcmp(pm->addr, sc->peer_wait_addr, ETH_ALEN) == 0) {
				sc->peer_mapped = 1;
				wakeup(&sc->peer_mapped);
			}
		}
		break;
	case HTT_TLV_T2H_MSG_TYPE_PEER_UNMAP:
		break;
	case HTT_TLV_T2H_MSG_TYPE_TX_COMPL_IND:
		htt_tx_compl_ind(sc, body, blen);
		break;
	case HTT_TLV_T2H_MSG_TYPE_SEC_IND:
		sc->install_key_done = 1;
		wakeup(&sc->install_key_done);
		break;
	case HTT_TLV_T2H_MSG_TYPE_RX_ADDBA:
		htt->n_addba++;
		break;
	case HTT_TLV_T2H_MSG_TYPE_RX_DELBA:
		htt->n_delba++;
		break;
	case HTT_TLV_T2H_MSG_TYPE_MGMT_TX_COMPL_IND:
		if (blen >= (int)sizeof(struct htt_mgmt_tx_completion)) {
			const struct htt_mgmt_tx_completion *mc = (const struct htt_mgmt_tx_completion *)body;
			uint32_t id = le32toh(mc->desc_id);

			if (id < ATHX_TX_NBUF && htt->tx_busy[id] == 1) {
				htt->tx_busy[id] = 0;
				htt->tx_pending--;
				athx_tx_done(sc);
			}
		}
		break;
	case HTT_TLV_T2H_MSG_TYPE_RX_FLUSH:
	case HTT_TLV_T2H_MSG_TYPE_STATS_CONF:
	case HTT_TLV_T2H_MSG_TYPE_TX_CREDIT_UPDATE_IND:
	case HTT_TLV_T2H_MSG_TYPE_CHAN_CHANGE:
	case HTT_TLV_T2H_MSG_TYPE_PKTLOG:
	case HTT_TLV_T2H_MSG_TYPE_TEST:
		break;
	case HTT_TLV_T2H_MSG_TYPE_TX_INSPECT_IND:
		printf("%s: received an unexpected htt tx inspect event\n", sc->sc_dev.dv_xname);
		break;
	default:
		ATHX_DPRINTF(sc, "%s: htt event (%d) not handled\n", sc->sc_dev.dv_xname, data[0]);
		break;
	}
}
