/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tswifi_obsd.c (x64 PC, WIFI_NET80211=obsd)
 *	The Wi-Fi interface between OpenBSD's IEEE 802.11 layer and
 *	TessronOS (design 9.6.9): the entry points the Wi-Fi manager
 *	(device/net/wifi/wifi.c) drives the card with, as the rump kernel's
 *	tswifi_if.c has them for NetBSD's layer. Compiled with OpenBSD's
 *	kernel headers (tools/openbsd/build.sh); each entry point takes the
 *	layer's lock for the time it runs.
 *
 *	The card's driver is the one of OpenBSD's that knows its PCI ID:
 *	iwm(4) for Intel's 7260 to 8265 (iwm0, tsw_iwm.c), iwx(4) for the
 *	Wi-Fi 6 AX200 and later (iwx0, tsw_iwx.c). What this file needs of
 *	the driver beyond net80211's interface is asked through tsw_drv.h;
 *	the rest is the same for both.
 *
 *	How the card is driven differs from NetBSD's: the manager says which
 *	network (SSID, its security and its key) and the layer itself
 *	chooses the access point of it, joins it, scans in the background
 *	while joined (the driver's ic_bgscan_start) and moves to a better one
 *	(OpenBSD's ifconfig join). While nothing is asked the layer scans on,
 *	as OpenBSD's does, and finds nothing to join.
 *
 *	The handshakes stay the manager's (wpa_supplicant's state machine,
 *	wpa/wpa_glue.c), as with NetBSD's layer:
 *
 *	- the EAPOL frames the layer takes out of what it receives go to
 *	  TessronOS as any frame does: ieee80211_eapol_key_input, which the
 *	  input path calls for them, is wrapped at the link (--wrap) by one
 *	  of this file's, and OpenBSD's own handshake is not run;
 *	- the authentication: once the layer has chosen an access point and
 *	  is about to authenticate (ic_send_mgmt, AUTH, sequence 1), the
 *	  frame is held and the manager told (an event). The manager decides
 *	  the security of that access point (knl_wpa_choose), writes the RSN
 *	  element of the association request (rump_tswifi_optie), and either
 *	  lets the open authentication go (rump_tswifi_auth_go) or runs SAE
 *	  (authentication algorithm 3) with frames sent through
 *	  rump_tswifi_auth_send and received as events (ic_recv_mgmt is
 *	  wrapped), and on its success moves the layer on to the association
 *	  (rump_tswifi_auth_done);
 *	- the association request the layer builds gets the manager's RSN
 *	  element in place of its own, and the RSNXE (SAE-H2E) after the
 *	  rest: the layer has no SAE and no RSNXE, and the element of the
 *	  4-way handshake's message 2 must be the same bytes;
 *	- the keys are set into the layer as its own handshake sets them
 *	  (rump_tswifi_setkey), the integrity group key of protected
 *	  management frames (BIP) among them, and the port made valid once
 *	  the handshakes are done (rump_tswifi_port).
 *
 *	The elements of the beacons and probe responses the layer does not
 *	keep (the RSNXE, the WPA one) are kept here for each access point,
 *	from the same wrapped ic_recv_mgmt, and given with the scan's
 *	results.
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

#include "obsd_glue.h"
#include "tsw_drv.h"

#define TSW_FRAME	1514
#define TSW_IE_MAX	256
#define TSW_EV_BODY	512
#define TSW_NEV		8
#define TSW_NAPS	128
#define RXS_SEC		10	/* how often the counts of what was received are looked at */

#ifndef IEEE80211_AUTH_ALG_SAE
#define IEEE80211_AUTH_ALG_SAE	3
#endif

/* What the manager reads of the card (wifi.c has the same layout) */
struct tswifi_status {
	int32_t		state;		/* IEEE80211_S_*: 0 init, 1 scan, 2 auth, 3 assoc, 4 run */
	int32_t		scanning;	/* a scan is going on */
	int32_t		freq;		/* MHz of the channel */
	int32_t		rssi;		/* dBm */
	int32_t		rate;		/* the rate sent at, in 500 kb/s */
	int32_t		ifflags;	/* 1 the interface is up, 2 the card runs */
	uint8_t		bssid[6];
	uint8_t		ssid_len;
	uint8_t		ssid[32];
	uint8_t		pad;
	/* the layer's own */
	int32_t		phy;		/* 0 none, 1 802.11a/b/g, 2 802.11n, 3 802.11ac */
	int32_t		mcs;		/* the MCS sent at (802.11n and ac) */
	int32_t		nss;		/* spatial streams */
	int32_t		width;		/* MHz of the channel's width: 20, 40, 80 */
	int32_t		sgi;		/* the short guard interval */
	int32_t		pmf;		/* management frames protected */
	uint32_t	scans;		/* scans ended, background ones too */
	uint32_t	bgscans;	/* background scans ended */
	uint32_t	roams;		/* moves to another access point while joined */
	int32_t		port;		/* data may pass */
};

/* One network a scan found (wifi.c has the same layout) */
struct tswifi_bss {
	uint8_t		bssid[6];
	uint8_t		ssid_len;
	uint8_t		ssid[32];
	uint8_t		pad;
	uint16_t	freq;
	uint16_t	capinfo;
	int32_t		rssi;
	uint16_t	ie_len;
	uint8_t		ie[TSW_IE_MAX];	/* its RSN, RSNXE and WPA elements */
};

/* What waits for the manager (wifi.c has the same layout) */
#define TSW_EV_AUTH	1		/* the layer is to authenticate with bssid */
#define TSW_EV_FRAME	2		/* an authentication frame of SAE came from bssid */
struct tswifi_ev {
	int32_t		type;
	uint8_t		bssid[6];
	uint16_t	alg;
	uint16_t	seq;
	uint16_t	status;
	int32_t		len;
	uint8_t		body[TSW_EV_BODY];
};

/* The security a network is joined with (rump_tswifi_join) */
#define TSW_AKM_PSK		0x01
#define TSW_AKM_PSK_SHA256	0x02
#define TSW_AKM_SAE		0x04

#if TSWIFI_SIM
struct ieee80211com	*obsd_sim_attach(void);	/* device/net/wifi/sim/obsd_sim.c */
int	obsd_sim_is_ap(struct ifnet *);
int	obsd_sim_scanning(void);
#endif
extern struct bus_dma_tag obsd_dma_tag;
pcitag_t obsd_pci_tag(int, int, int);

#if !TSWIFI_SIM
/* The drivers, in the order they are asked whether they know the card */
static const struct tsw_drv *const tsw_drvs[] = { &tsw_iwm_drv, &tsw_iwx_drv };
#endif

struct mbuf *ieee80211_get_assoc_req(struct ieee80211com *, struct ieee80211_node *, int);
struct mbuf *ieee80211_getmgmt(int, int, u_int);
int	ieee80211_mgmt_output(struct ifnet *, struct ieee80211_node *, struct mbuf *, int);
void	__real_ieee80211_end_scan(struct ifnet *);
void	__real_ieee80211_eapol_key_input(struct ieee80211com *, struct mbuf *,
	    struct ieee80211_node *);

static const struct tsw_drv *tsw_drv;	/* the card's driver; NULL for the Wi-Fi simulator's station */
struct ieee80211com	*tsw_ic;
static struct ifnet	*tsw_ifp;
static int		tsw_min_dbm = -100;	/* the signal net80211 has is dBm less this (the driver's) */

static int	(*orig_send_mgmt)(struct ieee80211com *, struct ieee80211_node *, int, int, int);
static void	(*orig_recv_mgmt)(struct ieee80211com *, struct mbuf *, struct ieee80211_node *,
		    struct ieee80211_rxinfo *, int);
static int	(*orig_newstate)(struct ieee80211com *, enum ieee80211_state, int);
static int	(*orig_bgscan_start)(struct ieee80211com *);
static int	tsw_bgscan_start(struct ieee80211com *);
static void	rxs_tick(void *);
static struct timeout rxs_to;
void	tsw_dhcp_frame(int, const uint8_t *, int);
void	tsw_rx_frame(const uint8_t *, int);
void	tsw_arp_frame(int, const uint8_t *, int);
static uint8_t	dhcp_yi[4];	/* the address DHCP last offered or acknowledged */
static int	tx_said, arp_said;
static uint64_t	port_ns;	/* when the port opened; 0 while it is not */
static int	txr_hs_said;	/* the card's answers said during the handshake of this join */
static int	bg_asked;	/* the manager's scan is being begun */
static int	bg_said;	/* the reason last said for putting a background scan off */
static int	tr_on;		/* a join is being timed */
static char	tr_buf[96];
#if !TSWIFI_SIM
extern void	(*obsd_rx_peek)(bus_dmamap_t, bus_addr_t, bus_size_t, int);
static uint32_t	pk_any, pk_bad_any, pk_one, pk_one_prot, pk_one_ccm, pk_one_dec,
		pk_one_err, pk_one_nokey;
static uint32_t	rp_all, rp_retry, rp_seen, rp_neither;	/* the replays seen ahead (rp_look) */
#endif
static void	tr_say(const char *);

static uint8_t	own_ie[TSW_IE_MAX];	/* the manager's RSN element and RSNXE */
static int	own_ie_len;
static uint32_t	n_scans, n_bgscans, n_roams;
static uint8_t	run_bssid[IEEE80211_ADDR_LEN];	/* the access point last joined */
static int	run_seen;

/* ---------------------------------------------------------------- the events */

static struct tswifi_ev	*evq;
static unsigned int	ev_head, ev_tail;

static struct tswifi_ev *
ev_put(int type, const uint8_t *bssid)
{
	struct tswifi_ev *ev;

	if (evq == NULL || ev_tail - ev_head >= TSW_NEV)
		return NULL;
	ev = &evq[ev_tail % TSW_NEV];
	memset(ev, 0, sizeof(*ev));
	ev->type = type;
	memcpy(ev->bssid, bssid, IEEE80211_ADDR_LEN);
	return ev;
}

static void
ev_done(void)
{
	ev_tail++;
	obsd_hc_kick();
}

/* The next thing that waits for the manager: 1 and it, or 0 */
int
rump_tswifi_event(struct tswifi_ev *out)
{
	int got = 0;

	obsd_lock();
	if (evq != NULL && ev_head != ev_tail) {
		*out = evq[ev_head % TSW_NEV];
		ev_head++;
		got = 1;
	}
	obsd_unlock();
	return got;
}

/* ---------------------------------------------------------------- the access points' elements */

struct ap_ies {
	uint8_t		bssid[IEEE80211_ADDR_LEN];
	uint64_t	seen;		/* when last heard, ns */
	uint16_t	len;
	uint8_t		ie[TSW_IE_MAX];
};

static struct ap_ies	*aps;

static void
aps_take(const uint8_t *bssid, const uint8_t *frm, const uint8_t *efrm)
{
	struct ap_ies *a, *old = NULL;
	uint8_t buf[TSW_IE_MAX];
	int n = 0, i;

	if (aps == NULL)
		return;
	while (frm + 2 <= efrm && frm + 2 + frm[1] <= efrm) {
		int keep = 0;

		switch (frm[0]) {
		case IEEE80211_ELEMID_RSN:
		case 244:		/* RSNXE */
			keep = 1;
			break;
		case IEEE80211_ELEMID_VENDOR:	/* WPA: 00:50:F2 type 1 */
			keep = frm[1] >= 4 && frm[2] == 0x00 && frm[3] == 0x50 &&
			    frm[4] == 0xf2 && frm[5] == 0x01;
			break;
		}
		if (keep && n + 2 + frm[1] <= (int)sizeof(buf)) {
			memcpy(buf + n, frm, 2 + frm[1]);
			n += 2 + frm[1];
		}
		frm += 2 + frm[1];
	}
	for (i = 0; i < TSW_NAPS; i++) {
		a = &aps[i];
		if (IEEE80211_ADDR_EQ(a->bssid, bssid))
			break;
		if (old == NULL || a->seen < old->seen)
			old = a;
	}
	if (i == TSW_NAPS)
		a = old;	/* the one heard longest ago gives its place */
	memcpy(a->bssid, bssid, IEEE80211_ADDR_LEN);
	a->seen = obsd_hc_nsec();
	memcpy(a->ie, buf, n);
	a->len = n;
}

static const struct ap_ies *
aps_find(const uint8_t *bssid)
{
	int i;

	if (aps == NULL)
		return NULL;
	for (i = 0; i < TSW_NAPS; i++) {
		if (aps[i].seen != 0 && IEEE80211_ADDR_EQ(aps[i].bssid, bssid))
			return &aps[i];
	}
	return NULL;
}

/*
 * The elements an access point sent: RSN, RSNXE and WPA, into ie (up
 * to max); their length. The node's own RSN element when the beacons
 * were not seen here.
 */
static int
ap_ies(struct ieee80211_node *ni, uint8_t *ie, int max)
{
	const struct ap_ies *a = aps_find(ni->ni_bssid);

	if (a != NULL && a->len <= max) {
		memcpy(ie, a->ie, a->len);
		return a->len;
	}
	if (ni->ni_rsnie != NULL && 2 + ni->ni_rsnie[1] <= max) {
		memcpy(ie, ni->ni_rsnie, 2 + ni->ni_rsnie[1]);
		return 2 + ni->ni_rsnie[1];
	}
	return 0;
}

/* An access point's RSN element (48) or RSNXE (244) into buf: its length, or 0 */
int
rump_tswifi_bss_ie(const uint8_t *bssid, int id, uint8_t *buf, int max)
{
	const struct ap_ies *a;
	const uint8_t *p;
	int n = 0;

	obsd_lock();
	a = aps_find(bssid);
	if (a != NULL) {
		for (p = a->ie; p + 2 <= a->ie + a->len; p += 2 + p[1]) {
			if (p[0] == id && 2 + p[1] <= max) {
				memcpy(buf, p, 2 + p[1]);
				n = 2 + p[1];
				break;
			}
		}
	}
	obsd_unlock();
	return n;
}

/* ---------------------------------------------------------------- the layer's hooks */

/* A scan ended (the driver's endscan): counted, and the manager told */
void
__wrap_ieee80211_end_scan(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;

	n_scans++;
	if ((ic->ic_flags & IEEE80211_F_BGSCAN) && ic->ic_state == IEEE80211_S_RUN)
		n_bgscans++;
	__real_ieee80211_end_scan(ifp);
	obsd_hc_kick();
}

/*
 * An EAPOL-Key frame the input path took aside for the layer's own
 * handshake: to TessronOS instead, as the frames for the stack go
 * (the manager gives it to the handshake)
 */
void
__wrap_ieee80211_eapol_key_input(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni)
{
	static uint8_t frame[TSW_FRAME];
	int len = m->m_pkthdr.len;

#if TSWIFI_SIM
	if (obsd_sim_is_ap(&ic->ic_if)) {
		/* the simulator's access point: the layer's own authenticator's */
		__real_ieee80211_eapol_key_input(ic, m, ni);
		return;
	}
#endif
	if (len >= (int)sizeof(struct ether_header) && len <= TSW_FRAME) {
		m_copydata(m, 0, len, frame);
		ic->ic_if.if_ipackets++;
		ic->ic_if.if_ibytes += len;
		rumpcomp_tswifi_input(frame, (unsigned long long)len);
	}
	m_freem(m);
}

/*
 * The power save as the access point sees it (design 9.6.9): the TIM
 * element of its beacons while joined, the bit of the station's
 * association ID set when the access point keeps frames for it (it
 * takes the station to be asleep), the bit 0 of the bitmap control when
 * it keeps frames to all for after the DTIM beacon. The first beacons
 * after the port opened are said, and those with the station's bit set.
 */
static uint32_t	tim_beacons, tim_ours;
static int	tim_said;

static void
tim_look(struct ieee80211com *ic, const uint8_t *p, const uint8_t *end)
{
	int aid = ic->ic_bss->ni_associd & 0x3fff, n1, idx, ours = 0;

	for (; p + 2 <= end && p + 2 + p[1] <= end; p += 2 + p[1]) {
		if (p[0] != IEEE80211_ELEMID_TIM || p[1] < 4)
			continue;
		tim_beacons++;
		n1 = p[4] & 0xfe;			/* the first octet of the partial bitmap */
		idx = aid / 8;
		if (aid != 0 && idx >= n1 && idx < n1 + p[1] - 3)
			ours = (p[5 + idx - n1] >> (aid % 8)) & 1;
		if (ours)
			tim_ours++;
		if (port_ns != 0 && (tim_said < 5 || (ours && tim_said < 25))) {
			tim_said++;
			printf("wifi: beacon of %s: DTIM count %u of %u, frames to all kept %d,"
			    " frames for the station (AID %d) kept %d; beacons %u, with the"
			    " station's bit %u\n", ether_sprintf(ic->ic_bss->ni_bssid), p[2], p[3],
			    p[4] & 1, aid, ours, tim_beacons, tim_ours);
		}
		return;
	}
}

/*
 * What the layer receives of management frames: the elements of each
 * beacon and probe response kept; SAE's authentication frames given to
 * the manager, not to the layer (which has no SAE)
 */
static void
tsw_recv_mgmt(struct ieee80211com *ic, struct mbuf *m, struct ieee80211_node *ni,
    struct ieee80211_rxinfo *rxi, int subtype)
{
	const struct ieee80211_frame *wh = mtod(m, const struct ieee80211_frame *);
	const uint8_t *frm = (const uint8_t *)&wh[1];
	const uint8_t *efrm = mtod(m, const uint8_t *) + m->m_len;
	struct tswifi_ev *ev;
	int len;

	switch (subtype) {
	case IEEE80211_FC0_SUBTYPE_BEACON:
		if (efrm - frm > 12 && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL &&
		    IEEE80211_ADDR_EQ(wh->i_addr3, ic->ic_bss->ni_bssid))
			tim_look(ic, frm + 12, efrm);
		/* FALLTHROUGH */
	case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
		if (efrm - frm > 12)
			aps_take(wh->i_addr3, frm + 12, efrm);
		break;
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
		/* the layer's move on it (AUTH -> AUTH while authenticating) says nothing of who sent it */
		printf("wifi: %s from %s (BSSID %s), reason %d, in %s\n",
		    subtype == IEEE80211_FC0_SUBTYPE_DEAUTH ? "deauthentication" : "disassociation",
		    ether_sprintf((uint8_t *)wh->i_addr2), ether_sprintf((uint8_t *)wh->i_addr3),
		    efrm - frm >= 2 ? LE_READ_2(frm) : -1, ieee80211_state_name[ic->ic_state]);
		break;
	case IEEE80211_FC0_SUBTYPE_ACTION:
		/* the block-ack agreements the access point asks for or answers, and their ends */
		if (efrm - frm >= 2 && frm[0] == IEEE80211_CATEG_BA)
			printf("wifi: block ack: %s from %s\n",
			    frm[1] == IEEE80211_ACTION_ADDBA_REQ ? "an agreement asked (ADDBA request)" :
			    frm[1] == IEEE80211_ACTION_ADDBA_RESP ? "an agreement answered (ADDBA response)" :
			    frm[1] == IEEE80211_ACTION_DELBA ? "an agreement ended (DELBA)" : "?",
			    ether_sprintf((uint8_t *)wh->i_addr2));
		/*
		 * The answer to the station's own request: the TID, the status,
		 * the agreement's starting sequence number as asked (SSN) and
		 * the TID's next sequence number, which are to be the same
		 */
		if (efrm - frm >= 7 && frm[0] == IEEE80211_CATEG_BA &&
		    frm[1] == IEEE80211_ACTION_ADDBA_RESP && ni != NULL) {
			int tid = (LE_READ_2(frm + 5) & IEEE80211_ADDBA_TID_MASK) >> IEEE80211_ADDBA_TID_SHIFT;

			printf("wifi: block ack: the agreement for what the station sends on TID %d:"
			    " status %d, starting at %u, the TID's next sequence number %u\n", tid,
			    LE_READ_2(frm + 3), ni->ni_tx_ba[tid].ba_winstart, ni->ni_qos_txseqs[tid]);
		}
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
		if (tr_on && efrm - frm >= 4) {
			snprintf(tr_buf, sizeof(tr_buf), "the association answered (status %d)",
			    LE_READ_2(frm + 2));
			tr_say(tr_buf);
		}
		break;
	case IEEE80211_FC0_SUBTYPE_AUTH:
		if (tr_on && efrm - frm >= 6) {
			snprintf(tr_buf, sizeof(tr_buf),
			    "the authentication answered (algorithm %d, sequence %d, status %d)",
			    LE_READ_2(frm), LE_READ_2(frm + 2), LE_READ_2(frm + 4));
			tr_say(tr_buf);
		}
		if (efrm - frm < 6 || LE_READ_2(frm) != IEEE80211_AUTH_ALG_SAE)
			break;
		len = efrm - frm - 6;
		if (ic->ic_state == IEEE80211_S_AUTH && len <= TSW_EV_BODY &&
		    IEEE80211_ADDR_EQ(wh->i_addr3, ic->ic_bss->ni_bssid) &&
		    (ev = ev_put(TSW_EV_FRAME, wh->i_addr3)) != NULL) {
			ev->alg = IEEE80211_AUTH_ALG_SAE;
			ev->seq = LE_READ_2(frm + 2);
			ev->status = LE_READ_2(frm + 4);
			ev->len = len;
			memcpy(ev->body, frm + 6, len);
			ev_done();
		}
		return;		/* the layer would drop it as of an algorithm it does not know */
	}
	(*orig_recv_mgmt)(ic, m, ni, rxi, subtype);
}

/*
 * The association request with the manager's elements: the RSN element
 * in place of the layer's, the RSNXE after the rest
 */
static int
send_assoc(struct ieee80211com *ic, struct ieee80211_node *ni, int type)
{
	struct mbuf *m0, *m;
	const uint8_t *p, *e, *q;
	uint8_t *d;
	int fixed, rsn_len, rest, error;

	ieee80211_ref_node(ni);
	m0 = ieee80211_get_assoc_req(ic, ni, type);
	if (m0 == NULL) {
		ieee80211_release_node(ic, ni);
		return ENOMEM;
	}
	m = ieee80211_getmgmt(M_DONTWAIT, MT_DATA, m0->m_len + own_ie_len);
	if (m == NULL) {
		m_freem(m0);
		ieee80211_release_node(ic, ni);
		return ENOMEM;
	}
	p = mtod(m0, const uint8_t *);
	e = p + m0->m_len;
	d = mtod(m, uint8_t *);
	fixed = 4 + (type == IEEE80211_FC0_SUBTYPE_REASSOC_REQ ? IEEE80211_ADDR_LEN : 0);
	memcpy(d, p, fixed);
	d += fixed;
	p += fixed;
	/* the manager's RSN element is its first; what follows it is the RSNXE */
	rsn_len = (own_ie_len >= 2 && own_ie[0] == IEEE80211_ELEMID_RSN) ? 2 + own_ie[1] : 0;
	if (rsn_len > own_ie_len)
		rsn_len = own_ie_len;
	rest = own_ie_len - rsn_len;
	for (q = p; q + 2 <= e && q + 2 + q[1] <= e; q += 2 + q[1]) {
		if (q[0] == IEEE80211_ELEMID_RSN) {
			memcpy(d, own_ie, rsn_len);
			d += rsn_len;
			rsn_len = 0;
			continue;
		}
		if (q[0] == 244)
			continue;
		memcpy(d, q, 2 + q[1]);
		d += 2 + q[1];
	}
	if (rsn_len > 0) {		/* the layer wrote none: the manager's anyway */
		memcpy(d, own_ie, rsn_len);
		d += rsn_len;
	}
	memcpy(d, own_ie + (own_ie_len - rest), rest);
	d += rest;
	m->m_pkthdr.len = m->m_len = d - mtod(m, uint8_t *);
	m_freem(m0);
	error = ieee80211_mgmt_output(&ic->ic_if, ni, m, type);
	if (error == 0)
		ic->ic_mgt_timer = IEEE80211_TRANS_WAIT;
	else
		ieee80211_release_node(ic, ni);
	return error;
}

static int	auth_pass;	/* the open authentication is let go: sent as the layer sends it */

/*
 * One join, timed (the records of a machine, design 9.6.9): from the
 * open authentication the layer would send, held for the manager, to
 * RUN or to the join's end. Each line is the milliseconds since the
 * hold, with whether the driver's time event that keeps the card on the
 * channel (iwm_protect_session in iwm_auth: twice the beacon interval;
 * iwx's session protection; started as the layer moves to AUTH) is on:
 * the hold, the manager's
 * letting go of it, the access point's answers as the layer's moves,
 * and the time event's end as the manager's loop sees it
 * (rump_tswifi_trace). Only while a join is under way.
 */
static uint64_t		tr_hold;	/* ns: the authentication held */
static int		tr_te;		/* the time event as last said */

/*
 * What became of the management frame sent last (the authentication,
 * the association request), as the driver counts it from the card's
 * answers to what it sent: those not acknowledged by the access point
 * after all the card's tries (its send errors), those its rate control
 * saw tried more than once (iwm's; iwx leaves the rate control to the
 * card's firmware), and what still waits in the queue of the management
 * frames (not yet sent by the card). Acknowledged and nothing queued:
 * the access point heard it and did not answer; not acknowledged: it
 * did not hear it, or the card sent it elsewhere; still queued: the
 * card did not send it (its channel, its contexts).
 */
static uint64_t		tr_oerr;
static u_int		tr_amn_tx, tr_amn_retry;

static int
tr_te_on(void)
{
	return tsw_drv != NULL && (*tsw_drv->te_on)();
}

static int
tr_ms(void)
{
	return (int)((getnsecuptime() - tr_hold) / 1000000ULL);
}

static void
tr_tx_mark(void)
{
	if (tsw_drv == NULL || tsw_ic->ic_bss == NULL)
		return;
	tr_oerr = tsw_ifp->if_oerrors;
	if (tsw_drv->tx_counts != NULL)
		(*tsw_drv->tx_counts)(&tr_amn_tx, &tr_amn_retry);
}

static void
tr_say(const char *what)
{
	u_int tx, retry;

	tr_te = tr_te_on();
	if (tsw_drv == NULL || tsw_ic->ic_bss == NULL) {
		printf("wifi: join timed: %s at %d ms, the time event %s\n", what, tr_ms(),
		    tr_te ? "on" : "off");
		return;
	}
	if (tsw_drv->tx_counts == NULL) {
		/* the card's firmware has the rate control, and the counts of its tries */
		printf("wifi: join timed: %s at %d ms, the time event %s; since the frame sent last:"
		    " not acknowledged %d, %d in the queue\n", what, tr_ms(), tr_te ? "on" : "off",
		    (int)(tsw_ifp->if_oerrors - tr_oerr), (*tsw_drv->mgmt_queued)());
		return;
	}
	(*tsw_drv->tx_counts)(&tx, &retry);
	printf("wifi: join timed: %s at %d ms, the time event %s; since the frame sent last:"
	    " not acknowledged %d, tried again %d of %d, %d in the queue\n", what, tr_ms(),
	    tr_te ? "on" : "off", (int)(tsw_ifp->if_oerrors - tr_oerr),
	    (int)(retry - tr_amn_retry), (int)(tx - tr_amn_tx), (*tsw_drv->mgmt_queued)());
}

/* From the manager's loop: the time event's end while a join is timed */
void	rump_tswifi_trace(void);

void
rump_tswifi_trace(void)
{
	obsd_lock();
	if (tr_on && tr_te_on() != tr_te)
		tr_say(tr_te_on() ? "the time event began" : "the time event ended");
	obsd_unlock();
}

static int
tsw_send_mgmt(struct ieee80211com *ic, struct ieee80211_node *ni, int type,
    int arg1, int arg2)
{
	struct tswifi_ev *ev;

	if (ic->ic_opmode != IEEE80211_M_STA || !(ic->ic_flags & IEEE80211_F_RSNON))
		return (*orig_send_mgmt)(ic, ni, type, arg1, arg2);
	switch (type) {
	case IEEE80211_FC0_SUBTYPE_AUTH:
		if (auth_pass || arg1 != IEEE80211_AUTH_OPEN_REQUEST)
			break;
		/* held: the manager decides the security of this access point first */
		ev = ev_put(TSW_EV_AUTH, ni->ni_bssid);
		if (ev == NULL)
			break;
		ev_done();
		tr_on = 1;
		tr_hold = getnsecuptime();
		printf("wifi: join timed: the authentication with %s held for the manager, the time event %s"
		    " (twice the beacon interval: %d TU)\n", ether_sprintf(ni->ni_bssid),
		    tr_te_on() ? "on" : "off", 2 * ni->ni_intval);
		tr_te = tr_te_on();
		/* the layer's timeout of the authentication runs from the watchdog, as for a frame sent */
		ic->ic_mgt_timer = IEEE80211_TRANS_WAIT;
		ic->ic_if.if_timer = 1;
		if (run_seen && !IEEE80211_ADDR_EQ(run_bssid, ni->ni_bssid))
			n_roams++;
		return 0;
	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
	case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
		if (tr_on) {
			tr_tx_mark();
			tr_say("the association request sent");
		}
		if (own_ie_len > 0)
			return send_assoc(ic, ni, type);
		break;
	}
	return (*orig_send_mgmt)(ic, ni, type, arg1, arg2);
}

static int
tsw_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	static const char *const name[] = { "INIT", "SCAN", "AUTH", "ASSOC", "RUN" };
	char what[40];

	if (tr_on && nstate != ic->ic_state && nstate <= IEEE80211_S_RUN) {
		snprintf(what, sizeof(what), "%s -> %s", name[ic->ic_state], name[nstate]);
		tr_say(what);
		if (nstate == IEEE80211_S_RUN || nstate <= IEEE80211_S_SCAN)
			tr_on = 0;
	}
	if (nstate == IEEE80211_S_RUN && ic->ic_bss != NULL) {
		memcpy(run_bssid, ic->ic_bss->ni_bssid, IEEE80211_ADDR_LEN);
		run_seen = 1;
	}
	if (nstate < IEEE80211_S_RUN) {
		port_ns = 0;		/* the next access point's port opens anew */
		txr_hs_said = 0;
	}
	return (*orig_newstate)(ic, nstate, arg);
}

/* ---------------------------------------------------------------- the start */

/*
 * The card found, the driver that knows its PCI ID (iwm, then iwx)
 * attached to it, and what the driver left until the root file system
 * was there run: the firmware read, the card started once to read its
 * NVM. 0 and the card's address, or an errno value.
 */
int
rump_tswifi_start(uint8_t *mac)
{
#if TSWIFI_SIM
	struct ieee80211com *ic;

	/* the Wi-Fi simulator's station in the card's place (design 9.6.12) */
	obsd_init();
	obsd_lock();
	evq = malloc(sizeof(*evq) * TSW_NEV, M_DEVBUF, M_WAITOK | M_ZERO);
	aps = malloc(sizeof(*aps) * TSW_NAPS, M_DEVBUF, M_WAITOK | M_ZERO);
	ic = obsd_sim_attach();
	if (evq == NULL || aps == NULL || ic == NULL) {
		obsd_unlock();
		return ENOMEM;
	}
	tsw_drv = NULL;
#else
	struct pci_attach_args pa;
	struct ieee80211com *ic;
	const struct tsw_drv *d = NULL;
	char xname[16];
	int bus, dev, fn, i;
	unsigned int id;

	if (obsd_hc_card(&bus, &dev, &fn, &id) != 0)
		return ENXIO;
	obsd_init();
	obsd_lock();
	evq = malloc(sizeof(*evq) * TSW_NEV, M_DEVBUF, M_WAITOK | M_ZERO);
	aps = malloc(sizeof(*aps) * TSW_NAPS, M_DEVBUF, M_WAITOK | M_ZERO);
	memset(&pa, 0, sizeof(pa));
	pa.pa_dmat = &obsd_dma_tag;
	pa.pa_bus = bus;
	pa.pa_device = dev;
	pa.pa_function = fn;
	pa.pa_tag = obsd_pci_tag(bus, dev, fn);
	pa.pa_id = pci_conf_read(NULL, pa.pa_tag, PCI_ID_REG);
	pa.pa_class = pci_conf_read(NULL, pa.pa_tag, PCI_CLASS_REG);
	pa.pa_intrline = PCI_INTERRUPT_LINE(pci_conf_read(NULL, pa.pa_tag, PCI_INTERRUPT_REG));
	if (evq == NULL || aps == NULL) {
		obsd_unlock();
		return ENOMEM;
	}
	for (i = 0; i < (int)nitems(tsw_drvs) && d == NULL; i++) {
		if ((*tsw_drvs[i]->match)(&pa))
			d = tsw_drvs[i];
	}
	if (d == NULL) {
		printf("obsd: neither iwm nor iwx knows the card %08x\n", id);
		obsd_unlock();
		return ENXIO;
	}
	printf("obsd: the card %08x is %s's\n", id, d->name);
	snprintf(xname, sizeof(xname), "%s0", d->name);
	ic = (*d->attach)(&pa, xname);
	if (ic == NULL) {
		obsd_unlock();
		return ENXIO;
	}
	tsw_drv = d;
	tsw_min_dbm = d->min_dbm;
	obsd_rx_peek = d->rx_peek;
#endif
	tsw_ic = ic;
	tsw_ifp = &ic->ic_if;
	orig_send_mgmt = ic->ic_send_mgmt;
	ic->ic_send_mgmt = tsw_send_mgmt;
	orig_recv_mgmt = ic->ic_recv_mgmt;
	ic->ic_recv_mgmt = tsw_recv_mgmt;
	orig_newstate = ic->ic_newstate;
	ic->ic_newstate = tsw_newstate;
	orig_bgscan_start = ic->ic_bgscan_start;
	if (orig_bgscan_start != NULL)
		ic->ic_bgscan_start = tsw_bgscan_start;
	timeout_set(&rxs_to, rxs_tick, NULL);
	timeout_add_sec(&rxs_to, RXS_SEC);
	memcpy(mac, ic->ic_myaddr, IEEE80211_ADDR_LEN);
	obsd_unlock();
	return 0;
}

/* The card's interface's name, iwm0 or iwx0 (the simulator's station's iwm0) */
const char *
rump_tswifi_ifname(void)
{
	return tsw_ifp != NULL ? tsw_ifp->if_xname : "wifi0";
}

/* The interface up (the card started: the driver's init, which scans) or down; 0 or an errno value */
int
rump_tswifi_ifup(int up)
{
	struct ifnet *ifp = tsw_ifp;
	int error;

	if (ifp == NULL)
		return ENXIO;
	obsd_lock();
	if (up)
		ifp->if_flags |= IFF_UP;
	else
		ifp->if_flags &= ~IFF_UP;
	error = (*ifp->if_ioctl)(ifp, SIOCSIFFLAGS, NULL);
	obsd_unlock();
	return error;
}

static int	tsw_dbg;	/* the manager wants the layer's words */

/*
 * The layer's own words (IFF_DEBUG: its changes of state, the frames of
 * management it sends, the candidates of each scan, its background scans
 * and roams) while a network is named, joined or being joined; not while
 * it scans on with nothing named, when each scan would list every
 * network
 */
static void
dbg_set(void)
{
	if (tsw_dbg && tsw_ic->ic_des_esslen != 0)
		tsw_ifp->if_flags |= IFF_DEBUG;
	else
		tsw_ifp->if_flags &= ~IFF_DEBUG;
}

/* How much the layer says on the console: the driver's steps and net80211's (IFF_DEBUG) */
void
rump_tswifi_debug(int drv, uint32_t net80211)
{
	if (tsw_drv != NULL)
		(*tsw_drv->debug)(drv);
	if (tsw_ifp == NULL)
		return;
	obsd_lock();
	tsw_dbg = net80211 != 0;
	dbg_set();
#ifdef IEEE80211_DEBUG
	{
		extern int ieee80211_debug;

		ieee80211_debug = (net80211 & 0x00001000) ? 1 : 0;	/* while joining */
	}
#endif
	obsd_unlock();
}

/* ---------------------------------------------------------------- frames */

/*
 * A block-ack agreement for what the station sends, which net80211 asks
 * for at a TID's data frames (ieee80211_encap): asked for only once the
 * port is open and the TID has carried AGG_START frames within a second,
 * as iwlwifi starts one (rs_tl_turn_on_agg: the station authorized,
 * IWL_MVM_RS_AGG_START_THRESHOLD of 10 frames a second), so not for
 * DHCP's few frames right after a join, nor for EAPOL (never asked for,
 * ieee80211_output's patch).
 */
#define AGG_START	10
static uint64_t	agg_win_ns[IEEE80211_NUM_TID];
static int	agg_cnt[IEEE80211_NUM_TID];

void	__real_ieee80211_node_trigger_addba_req(struct ieee80211_node *, int);

void
__wrap_ieee80211_node_trigger_addba_req(struct ieee80211_node *ni, int tid)
{
	uint64_t now = getnsecuptime();

	if (tid < 0 || tid >= IEEE80211_NUM_TID)
		return;
	if (ni->ni_ic != tsw_ic) {
		/* the Wi-Fi simulator's access point (design 9.6.12): as OpenBSD asks */
		__real_ieee80211_node_trigger_addba_req(ni, tid);
		return;
	}
	if (!ni->ni_port_valid) {
		agg_cnt[tid] = 0;
		return;
	}
	if (now - agg_win_ns[tid] >= 1000000000ULL) {
		agg_win_ns[tid] = now;
		agg_cnt[tid] = 0;
	}
	if (++agg_cnt[tid] < AGG_START)
		return;
	if (ni->ni_tx_ba[tid].ba_state == IEEE80211_BA_INIT &&
	    !timeout_pending(&ni->ni_addba_req_to[tid]))
		printf("wifi: block ack: an agreement for TID %d to be asked for, after %d frames"
		    " in a second; the TID's next sequence number %u\n", tid, agg_cnt[tid],
		    ni->ni_qos_txseqs[tid]);
	__real_ieee80211_node_trigger_addba_req(ni, tid);
}

/*
 * The frames of a TID whose agreement has been asked for and not yet
 * answered are held, and sent once it has been answered or has failed,
 * in their order, the TID's frames waiting until the agreement is
 * operational. Sent meanwhile, each would take a sequence number of the
 * TID, and the agreement starts at the number asked for (net80211 sets
 * the TID's counter back to it, iwm's aggregation queue starts there):
 * the numbers would be used twice, and the access point would take the
 * second frames for ones it has already had. With iwx the card's
 * firmware asks for the agreement, and it is made as the driver's task
 * enables the TID's queue; until then the TID's frames go without the
 * QoS field on the queue of the management frames
 * (tools/openbsd/patches/ieee80211_output-qos-data.patch), and they wait
 * for that task so as to go out in their order on the TID's queue.
 */
#define AGG_HOLD_MAX	64
static struct mbuf_list	agg_hold = MBUF_LIST_INITIALIZER();
static struct timeout	agg_hold_to;
static int		agg_hold_tid = -1;
static uint64_t		agg_hold_ns;
static int		agg_hold_inited;

static int
frame_tid(struct ieee80211com *ic, struct mbuf *m)
{
	const struct ether_header *eh = mtod(m, const struct ether_header *);

	if (eh->ether_type == htons(ETHERTYPE_EAPOL))
		return 7;
	return ieee80211_classify(ic, m);
}

/* The TID's agreement asked for and not yet answered: its frames are to wait */
static int
agg_pending(struct ieee80211com *ic, int tid)
{
	return ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL &&
	    ic->ic_bss->ni_tx_ba[tid].ba_state == IEEE80211_BA_REQUESTED;
}

/* The frames held sent, in their order, once the agreement is answered (obsd_lock held) */
static void
agg_release(void)
{
	struct ifnet *ifp = tsw_ifp;
	struct ieee80211com *ic = tsw_ic;
	struct mbuf *m;
	int n = 0, tid = agg_hold_tid;

	agg_hold_tid = -1;
	while ((m = ml_dequeue(&agg_hold)) != NULL) {
		n++;
		if (ifp == NULL)
			m_freem(m);
		else
			(void)if_enqueue(ifp, m);
	}
	if (n > 0 && ic != NULL && ic->ic_bss != NULL && tid >= 0)
		printf("wifi: block ack: %d frame%s of TID %d held %d ms while the agreement was"
		    " asked for, sent (the agreement %s, the TID's next sequence number %u)\n", n,
		    n == 1 ? "" : "s", tid, (int)((getnsecuptime() - agg_hold_ns) / 1000000ULL),
		    ic->ic_bss->ni_tx_ba[tid].ba_state == IEEE80211_BA_AGREED ? "made" : "not made",
		    ic->ic_bss->ni_qos_txseqs[tid]);
}

static void
agg_hold_tick(void *arg)
{
	struct ieee80211com *ic = tsw_ic;

	if (agg_hold_tid < 0)
		return;
	if (ic == NULL || !agg_pending(ic, agg_hold_tid) ||
	    getnsecuptime() - agg_hold_ns > 2000000000ULL)
		agg_release();
	else
		timeout_add_msec(&agg_hold_to, 5);
}

/* One Ethernet frame out on the card's interface; 0 or an errno value */
int
rump_tswifi_send(const void *frame, int len)
{
	struct ifnet *ifp = tsw_ifp;
	struct mbuf *m;
	int error;

	if (ifp == NULL)
		return ENXIO;
	if (len < (int)sizeof(struct ether_header) || len > TSW_FRAME)
		return EINVAL;
	obsd_lock();
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m != NULL && len > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			m_freem(m);
			m = NULL;
		}
	}
	if (m == NULL) {
		obsd_unlock();
		return ENOBUFS;
	}
	memcpy(mtod(m, void *), frame, len);
	m->m_pkthdr.len = m->m_len = len;
	tsw_dhcp_frame(0, frame, len);
	tsw_arp_frame(0, frame, len);
	if (!agg_hold_inited) {
		timeout_set(&agg_hold_to, agg_hold_tick, NULL);
		agg_hold_inited = 1;
	}
	if (agg_hold_tid >= 0 && !agg_pending(tsw_ic, agg_hold_tid))
		agg_release();		/* answered meanwhile: the held ones first */
	if (agg_hold_tid < 0 && tsw_ic != NULL && agg_pending(tsw_ic, frame_tid(tsw_ic, m))) {
		agg_hold_tid = frame_tid(tsw_ic, m);
		agg_hold_ns = getnsecuptime();
		timeout_add_msec(&agg_hold_to, 5);
	}
	if (agg_hold_tid >= 0 && agg_hold_tid == frame_tid(tsw_ic, m)) {
		if (ml_len(&agg_hold) >= AGG_HOLD_MAX) {
			m_freem(m);
			error = ENOBUFS;
		} else {
			ml_enqueue(&agg_hold, m);
			error = 0;
		}
	} else
		error = if_enqueue(ifp, m);
	obsd_unlock();
	return error;
}

/* ---------------------------------------------------------------- what goes through */

/*
 * DHCP's messages each way, for the records (design 9.6.9): the type,
 * the transaction, the broadcast flag, the addresses in the message and
 * in the frame, and whether the frame came to the station alone
 * (unicast, protected with the pairwise key) or to all (the group key).
 * What is asked and what came back are thereby seen where the card's
 * frames are, below the stack. While a DISCOVER or a REQUEST waits for
 * its answer, no background scan is begun (tsw_bgscan_start).
 */
/*
 * The client's longest wait for the answer to a REQUEST before it sends
 * it again (so_dhcp.c: 4, 8, 16, 32 s, each up to 1 s longer)
 */
#define DHCP_WAIT_NS	(33ULL * 1000000000ULL)
#define DHCP_HOLD_NS	(60ULL * 1000000000ULL)	/* the most one round of asking holds the scans */

static uint64_t	dhcp_out_ns;	/* the last DISCOVER or REQUEST sent */
static uint64_t	dhcp_round_ns;	/* the first of the round still unanswered */
static int	dhcp_waiting;	/* a DISCOVER or REQUEST is unanswered */
static uint32_t	rx_unicast, rx_group;	/* frames given to the stack, by their destination */

static const char *const dhcp_type[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE",
    "ACK", "NAK", "RELEASE", "INFORM" };

static void
ip4_str(char *b, size_t n, const uint8_t *a)
{
	snprintf(b, n, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

void
tsw_dhcp_frame(int in, const uint8_t *f, int len)
{
	const uint8_t *ip = f + 14, *u, *b, *o, *end;
	const uint8_t *req = NULL, *srv = NULL;
	char ci[16], yi[16], rq[16], sv[16], src[20], dst[20];
	int hl, sport, dport, type = 0;
	uint64_t now;

	if (len < 14 + 20 || f[12] != 0x08 || f[13] != 0x00 || (ip[0] >> 4) != 4 || ip[9] != 17)
		return;
	hl = (ip[0] & 15) * 4;
	u = ip + hl;
	if (len < 14 + hl + 8 + 240)
		return;
	sport = (u[0] << 8) | u[1];
	dport = (u[2] << 8) | u[3];
	if (!((sport == 68 && dport == 67) || (sport == 67 && dport == 68)))
		return;
	b = u + 8;
	if (b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99)
		return;
	end = f + len;
	for (o = b + 240; o < end && *o != 255; ) {
		if (*o == 0) {
			o++;
			continue;
		}
		if (o + 2 > end || o + 2 + o[1] > end)
			break;
		if (o[0] == 53 && o[1] >= 1)
			type = o[2];
		else if (o[0] == 50 && o[1] >= 4)
			req = o + 2;
		else if (o[0] == 54 && o[1] >= 4)
			srv = o + 2;
		o += 2 + o[1];
	}
	ip4_str(ci, sizeof(ci), b + 12);
	ip4_str(yi, sizeof(yi), b + 16);
	if (req != NULL)
		ip4_str(rq, sizeof(rq), req);
	else
		strlcpy(rq, "none", sizeof(rq));
	if (srv != NULL)
		ip4_str(sv, sizeof(sv), srv);
	else
		strlcpy(sv, "none", sizeof(sv));
	strlcpy(src, ether_sprintf((uint8_t *)(f + 6)), sizeof(src));
	strlcpy(dst, ether_sprintf((uint8_t *)f), sizeof(dst));
	printf("wifi: DHCP %s: %s xid %02x%02x%02x%02x, flags %02x%02x, ciaddr %s, yiaddr %s,"
	    " chaddr %s, requested %s, server %s; frame %s -> %s (%s)\n",
	    in ? "in" : "out", (type >= 0 && type <= 8) ? dhcp_type[type] : "?",
	    b[4], b[5], b[6], b[7], b[10], b[11], ci, yi, ether_sprintf((uint8_t *)(b + 28)),
	    rq, sv, src, dst, (f[0] & 1) ? "to all" : "to one");
	now = getnsecuptime();
	if (!in && (type == 1 || type == 3)) {
		if (!dhcp_waiting)
			dhcp_round_ns = now;
		dhcp_waiting = 1;
		dhcp_out_ns = now;
	} else if (in && (type == 5 || type == 6)) {
		dhcp_waiting = 0;
	}
	if (in && (type == 2 || type == 5))
		memcpy(dhcp_yi, b + 16, 4);
}

/*
 * ARP each way while the address is asked for and after, for the
 * records: what the station sends, and of what it receives the answers
 * and the requests for the address DHCP offered (a server that checks
 * the address before it acknowledges, or the address already taken).
 * At most ARP_SAID lines from the port's opening.
 */
#define ARP_SAID	24

void
tsw_arp_frame(int in, const uint8_t *f, int len)
{
	int op;

	if (len < 42 || f[12] != 0x08 || f[13] != 0x06 || arp_said >= ARP_SAID)
		return;
	op = (f[20] << 8) | f[21];
	if (in && op != 2 && memcmp(f + 38, dhcp_yi, 4) != 0)
		return;
	arp_said++;
	printf("wifi: ARP %s: %s, %u.%u.%u.%u (%s) asks or tells about %u.%u.%u.%u;"
	    " frame from %s (%s)\n", in ? "in" : "out", op == 1 ? "request" : op == 2 ? "reply" : "?",
	    f[28], f[29], f[30], f[31], ether_sprintf((uint8_t *)(f + 22)),
	    f[38], f[39], f[40], f[41], ether_sprintf((uint8_t *)(f + 6)),
	    (f[0] & 1) ? "to all" : "to one");
}

/*
 * The data frames as the layer builds them (the driver's start, iwm_start
 * or iwx_start, calls ieee80211_encap, wrapped at the link): the first
 * after the port opened are said with their power management bit. The
 * card's firmware sets that bit itself in what it sends while it
 * sleeps; with the power management iwm gives it (the MAC's power table
 * with POWER_SAVE_ENA alone, iwm_power_build_cmd) it stays awake.
 */
struct mbuf	*__real_ieee80211_encap(struct ifnet *, struct mbuf *, struct ieee80211_node **);

struct mbuf *
__wrap_ieee80211_encap(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node **pni)
{
	const struct ieee80211_frame *wh;

	m = __real_ieee80211_encap(ifp, m, pni);
	if (m != NULL && ifp == tsw_ifp && port_ns != 0 && tx_said < 4 &&
	    m->m_len >= (int)sizeof(*wh)) {
		wh = mtod(m, const struct ieee80211_frame *);
		tx_said++;
		printf("wifi: sent: a data frame to %s through %s, power management bit %d,"
		    " protected %d\n", ether_sprintf((uint8_t *)wh->i_addr3),
		    ether_sprintf((uint8_t *)wh->i_addr1), !!(wh->i_fc[1] & IEEE80211_FC1_PWR_MGT),
		    !!(wh->i_fc[1] & IEEE80211_FC1_PROTECTED));
	}
	return m;
}

/* A frame for the stack (obsd_net.c's if_input): counted, and said if it is DHCP's */
void
tsw_rx_frame(const uint8_t *f, int len)
{
	if (f[0] & 1)
		rx_group++;
	else
		rx_unicast++;
	tsw_dhcp_frame(1, f, len);
	tsw_arp_frame(1, f, len);
}

/*
 * What the layer and the driver dropped of the frames received, by the
 * reason each counts it under, with what reached the stack: said every
 * RXS_SEC while joined, when it has changed. A frame for the station
 * that does not reach the stack is counted in one of them: decryption
 * (the card's status of the CCMP decryption), replay (the packet number
 * not above the last), port (the 802.1X port not open), the block-ack
 * reordering (below the window, lost), duplicate, another BSS,
 * unencrypted while the keys are set, decapsulation, and the driver's
 * own count.
 *
 * What the driver handed up to the layer (ieee80211_inputm, wrapped at
 * the link) is counted as well, before any of the layer's checks: data
 * frames to the station alone, with how many of them were protected and
 * decrypted by the card (IEEE80211_RXI_HWDEC), data frames to all, and
 * management frames to the station alone. A frame for the station that
 * is not among them was dropped below the layer: by the card's firmware
 * (its address filter, its decryption), or by the driver before it
 * hands a frame up (iwm_rx_mpdu, iwx_rx_mpdu_mq: the card's CRC or
 * overrun status, a frame too short or too long, the last two counted
 * in the driver's receive errors). With iwx the card reorders what
 * comes under a block-ack agreement itself and the driver drops the
 * duplicates before the layer (iwx_detect_duplicate).
 */
#define RXS_N		21

static uint32_t	rxs_was[RXS_N];
static uint32_t	drv_data_one, drv_data_one_prot, drv_data_one_hwdec, drv_data_all, drv_mgmt_one;
static int	drv_said;	/* data frames to the station alone said since the port opened */

void	__real_ieee80211_inputm(struct ifnet *, struct mbuf *, struct ieee80211_node *,
	    struct ieee80211_rxinfo *, struct mbuf_list *);

void
__wrap_ieee80211_inputm(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node *ni,
    struct ieee80211_rxinfo *rxi, struct mbuf_list *ml)
{
	const struct ieee80211_frame *wh;
	int type;

	if (m->m_len >= (int)sizeof(struct ieee80211_frame)) {
		wh = mtod(m, const struct ieee80211_frame *);
		type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		if (type == IEEE80211_FC0_TYPE_DATA) {
			if (IEEE80211_IS_MULTICAST(wh->i_addr1)) {
				drv_data_all++;
			} else {
				drv_data_one++;
				if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
					drv_data_one_prot++;
				if (rxi->rxi_flags & IEEE80211_RXI_HWDEC)
					drv_data_one_hwdec++;
				if (port_ns != 0 && drv_said < 4) {
					drv_said++;
					printf("wifi: from the card: a data frame to %s from %s,"
					    " %s, %s, receive flags %x, sequence %u\n",
					    ether_sprintf((uint8_t *)wh->i_addr1),
					    ether_sprintf((uint8_t *)wh->i_addr2),
					    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) ? "protected" : "not protected",
					    (rxi->rxi_flags & IEEE80211_RXI_HWDEC) ? "decrypted by the card" :
					    "not decrypted by the card", rxi->rxi_flags,
					    letoh16(*(const uint16_t *)wh->i_seq) >> IEEE80211_SEQ_SEQ_SHIFT);
				}
			}
		} else if (type == IEEE80211_FC0_TYPE_MGT && !IEEE80211_IS_MULTICAST(wh->i_addr1)) {
			drv_mgmt_one++;
		}
	}
	__real_ieee80211_inputm(ifp, m, ni, rxi, ml);
}

static void
rxs_tick(void *arg)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_stats *s;
	uint32_t v[RXS_N];

	if (ic == NULL)
		return;
	if (ic->ic_state == IEEE80211_S_RUN) {
		s = &ic->ic_stats;
		v[0] = rx_unicast;
		v[1] = rx_group;
		v[2] = s->is_ccmp_dec_errs;
		v[3] = s->is_ccmp_replays;
		v[4] = s->is_rx_unauth;
		v[5] = s->is_ht_rx_frame_below_ba_winstart;
		v[6] = s->is_ht_rx_ba_frame_lost;
		v[7] = s->is_rx_dup;
		v[8] = s->is_rx_wrongbss;
		v[9] = s->is_rx_unencrypted;
		v[10] = s->is_rx_decap;
		v[11] = (uint32_t)tsw_ifp->if_ierrors;
		v[12] = (uint32_t)tsw_ifp->if_oerrors;
		v[13] = s->is_ht_rx_ba_agreements;
		v[14] = s->is_ht_rx_ba_window_gap_timeout;
		v[15] = s->is_rx_nombuf;
		v[16] = drv_data_one;
		v[17] = drv_data_one_prot;
		v[18] = drv_data_one_hwdec;
		v[19] = drv_data_all;
		v[20] = drv_mgmt_one;
		if (memcmp(v, rxs_was, sizeof(v)) != 0) {
			memcpy(rxs_was, v, sizeof(v));
			/* lines each well within the layer's line of 510 characters (obsd_kern.c) */
			printf("wifi: receive: from the card: data to the station %u (protected %u,"
			    " decrypted by the card %u), data to all %u, management to the station %u;"
			    " to the stack %u unicast, %u to all\n", v[16], v[17], v[18], v[19], v[20],
			    v[0], v[1]);
			printf("wifi: receive: dropped for decryption %u, replay %u (frames sent again among them), port %u,"
			    " below the block-ack window %u, block-ack lost %u (gaps timed out %u,"
			    " agreements %u), duplicate %u\n", v[2], v[3], v[4], v[5], v[6], v[14], v[13],
			    v[7]);
			printf("wifi: receive: dropped for another BSS %u, unencrypted %u, decapsulation %u,"
			    " no buffer %u; %s's receive errors %u (the replays among them), send failures %u\n",
			    v[8], v[9], v[10], v[15], tsw_drv != NULL ? tsw_drv->name : "the station",
			    v[11], v[12]);
#if !TSWIFI_SIM
			printf("wifi: receive: the replays as seen ahead %u: with the retry bit %u, the"
			    " sequence received before %u, neither %u; too short %u; allocations failed %u\n",
			    rp_all, rp_retry, rp_seen, rp_neither, s->is_rx_tooshort, obsd_pool_fails);
			/*
			 * The card's own counts (the driver's card_stats), and what
			 * it wrote of the frames it received (tsw_rx_mpdu). A frame
			 * lost below the driver shows here.
			 */
			if (tsw_drv != NULL) {
				(*tsw_drv->card_stats)();
				printf("wifi: receive: the card wrote %u frames (%u with a bad CRC or overrun);"
				    " to the station %u: protected %u, CCMP %u, decrypted with the MIC good %u,"
				    " could not be decrypted %u, protected with no valid key %u\n",
				    pk_any, pk_bad_any, pk_one, pk_one_prot, pk_one_ccm, pk_one_dec,
				    pk_one_err, pk_one_nokey);
			}
#endif
		}
	}
	timeout_add_sec(&rxs_to, RXS_SEC);
}

/*
 * The background scans while joined: the layer begins one 500 ms after
 * a frame of a signal below its threshold (60 % of the card's range on
 * 2.4 GHz, 50 % on 5 GHz) and less often after each that found nothing
 * better. The card leaves the channel for at most 120 ms at a time,
 * coming back for 120 ms between, until all channels are done (about
 * 10 s). Begun while DHCP's answer is on its way, the answer may be
 * missed: an answer to all goes out after the access point's next DTIM
 * beacon whether the station is on the channel or not. So none is
 * begun:
 *
 * - in the first BG_FIRST_NS after the port opened: the scan that
 *   chose the access point is that recent, and the address is asked
 *   for then;
 * - sooner than BG_EVERY_NS after the last;
 * - while a DISCOVER or REQUEST waits for its answer (DHCP_WAIT_NS,
 *   one round of asking at most DHCP_HOLD_NS).
 *
 * A scan the manager asks for (rump_tswifi_scan, the user's) is begun
 * as asked.
 */
#define BG_FIRST_NS	(30ULL * 1000000000ULL)
#define BG_EVERY_NS	(30ULL * 1000000000ULL)

static uint64_t	bg_ns;		/* when the last background scan began */

static int
tsw_bgscan_start(struct ieee80211com *ic)
{
	uint64_t now = getnsecuptime();
	int why = 0, error;

	if (!bg_asked) {
		if (port_ns == 0 || now - port_ns < BG_FIRST_NS)
			why = 1;
		else if (bg_ns != 0 && now - bg_ns < BG_EVERY_NS)
			why = 2;
		else if (dhcp_waiting && now - dhcp_out_ns < DHCP_WAIT_NS &&
		    now - dhcp_round_ns < DHCP_HOLD_NS)
			why = 3;
	}
	if (why != 0) {
		if (why != bg_said)
			printf("wifi: background scan put off: %s\n",
			    why == 1 ? "the port opened less than 30 s ago" :
			    why == 2 ? "the last began less than 30 s ago" :
			    "DHCP's answer is awaited");
		bg_said = why;
		return EBUSY;
	}
	bg_said = 0;
	error = (*orig_bgscan_start)(ic);
	if (error == 0)
		bg_ns = now;
	return error;
}

/* ---------------------------------------------------------------- scanning */

/*
 * A scan: one is going on all the time while nothing is joined (the
 * layer scans on until it finds what it was asked for), and is taken as
 * the one asked for; while joined, a background scan. The scan ends
 * when the count of ended scans (status) moves on.
 */
int
rump_tswifi_scan(void)
{
	struct ieee80211com *ic = tsw_ic;
	int error = 0;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	if ((tsw_ifp->if_flags & IFF_RUNNING) == 0) {
		error = ENETDOWN;
	} else if (ic->ic_state == IEEE80211_S_RUN) {
		bg_asked = 1;
		ieee80211_begin_bgscan(tsw_ifp);
		bg_asked = 0;
	} else if (ic->ic_state == IEEE80211_S_INIT) {
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	} else if (ic->ic_state != IEEE80211_S_SCAN) {
		error = EBUSY;
	}
	obsd_unlock();
	return error;
}

/*
 * What the scans found, up to max networks into list: how many there
 * are, those that did not fit counted too. The signal is in dBm (the
 * driver gives it as dBm less its MIN_DBM, -100 for iwm and iwx).
 */
int
rump_tswifi_results(struct tswifi_bss *list, int max)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;
	struct tswifi_bss *b;
	int n = 0;

	if (ic == NULL)
		return -ENXIO;
	obsd_lock();
	RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
		if (ni->ni_chan == IEEE80211_CHAN_ANYC || ni->ni_chan == NULL)
			continue;
		if (n >= max) {
			n++;			/* counted, not copied */
			continue;
		}
		b = &list[n];
		memset(b, 0, sizeof(*b));
		memcpy(b->bssid, ni->ni_bssid, 6);
		b->ssid_len = MIN(ni->ni_esslen, sizeof(b->ssid));
		memcpy(b->ssid, ni->ni_essid, b->ssid_len);
		b->freq = ni->ni_chan->ic_freq;
		b->capinfo = ni->ni_capinfo;
		b->rssi = (int)ni->ni_rssi + tsw_min_dbm;
		b->ie_len = ap_ies(ni, b->ie, sizeof(b->ie));
		n++;
	}
	obsd_unlock();
	return n;
}

/* ---------------------------------------------------------------- joining */

/*
 * The network to join: its SSID, whether it has security (RSN, with the
 * key managements TSW_AKM_* it may be joined with, management frames
 * protected always or when the access point can, and the PMK of
 * WPA2-Personal), or none. The layer chooses the access point of it
 * after its scan, and moves among them. 0 or an errno value.
 */
int
rump_tswifi_join(const uint8_t *ssid, int ssid_len, int rsn, int akms, int mfp_required,
    const uint8_t *pmk)
{
	struct ieee80211com *ic = tsw_ic;
	int same;

	if (ic == NULL)
		return ENXIO;
	if (ssid_len <= 0 || ssid_len > IEEE80211_NWID_LEN)
		return EINVAL;
	obsd_lock();
	same = ic->ic_des_esslen == ssid_len && memcmp(ic->ic_des_essid, ssid, ssid_len) == 0;
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	memcpy(ic->ic_des_essid, ssid, ssid_len);
	ic->ic_des_esslen = ssid_len;
	ic->ic_flags &= ~(IEEE80211_F_RSNON | IEEE80211_F_PSK | IEEE80211_F_MFPR |
	    IEEE80211_F_WEPON | IEEE80211_F_DESBSSID);
	explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
	own_ie_len = 0;
	if (rsn) {
		ic->ic_flags |= IEEE80211_F_RSNON;
		ic->ic_rsnprotos = IEEE80211_PROTO_RSN;
		ic->ic_rsnakms = 0;
		if (akms & TSW_AKM_PSK)
			ic->ic_rsnakms |= IEEE80211_AKM_PSK;
		if (akms & TSW_AKM_PSK_SHA256)
			ic->ic_rsnakms |= IEEE80211_AKM_SHA256_PSK;
		if (akms & TSW_AKM_SAE)
			ic->ic_rsnakms |= IEEE80211_AKM_SAE;
		ic->ic_rsnciphers = IEEE80211_CIPHER_CCMP | IEEE80211_CIPHER_TKIP;
		ic->ic_rsngroupcipher = IEEE80211_CIPHER_CCMP;
		ic->ic_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
		if (mfp_required)
			ic->ic_flags |= IEEE80211_F_MFPR;
		/* the layer accepts an access point of PSK only with a key set */
		ic->ic_flags |= IEEE80211_F_PSK;
		if (pmk != NULL)
			memcpy(ic->ic_psk, pmk, IEEE80211_PMK_LEN);
	}
	dbg_set();
	if (!same && ic->ic_state > IEEE80211_S_SCAN)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	obsd_unlock();
	return 0;
}

/* The network left, telling the access point why, and nothing joined until asked again */
int
rump_tswifi_deauth(const uint8_t *bssid, int reason)
{
	struct ieee80211com *ic = tsw_ic;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	if (ic->ic_state >= IEEE80211_S_ASSOC && ic->ic_bss != NULL)
		IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, reason);
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	ic->ic_des_esslen = 0;
	ic->ic_flags &= ~(IEEE80211_F_RSNON | IEEE80211_F_PSK | IEEE80211_F_MFPR);
	explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
	own_ie_len = 0;
	run_seen = 0;
	dbg_set();
	if (ic->ic_state > IEEE80211_S_SCAN)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	obsd_unlock();
	return 0;
}

/* The RSN element (and RSNXE) the manager made for the association request */
int
rump_tswifi_optie(const uint8_t *ie, int len)
{
	if (len < 0 || len > (int)sizeof(own_ie))
		return EINVAL;
	obsd_lock();
	if (len > 0)
		memcpy(own_ie, ie, len);
	own_ie_len = len;
	obsd_unlock();
	return 0;
}

/* The keys and the protection of the authentication before, cleared as the layer clears them */
static void
auth_reset(struct ieee80211com *ic)
{
	struct ieee80211_node *ni = ic->ic_bss;

	ni->ni_flags &= ~(IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_RXMGMTPROT |
	    IEEE80211_NODE_TXMGMTPROT);
	ni->ni_port_valid = 0;
	ni->ni_replaycnt_ok = 0;
	(*ic->ic_delete_key)(ic, ni, &ni->ni_pairwise_key);
}

/* The open authentication held for the manager, let go */
int
rump_tswifi_auth_go(void)
{
	struct ieee80211com *ic = tsw_ic;
	int error;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	if (ic->ic_state != IEEE80211_S_AUTH) {
		obsd_unlock();
		return EINVAL;
	}
	auth_pass = 1;
	tr_tx_mark();
	error = IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_AUTH,
	    IEEE80211_AUTH_OPEN_REQUEST);
	auth_pass = 0;
	if (tr_on)
		tr_say(error == 0 ? "the authentication let go and sent" : "the authentication let go, not sent");
	obsd_unlock();
	return error;
}

/* An authentication frame of SAE to the access point being joined */
int
rump_tswifi_auth_send(int seq, int status, const uint8_t *body, int len)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;
	struct mbuf *m;
	uint8_t *frm;
	int error;

	if (ic == NULL)
		return ENXIO;
	if (len < 0 || len > TSW_EV_BODY)
		return EINVAL;
	obsd_lock();
	ni = ic->ic_bss;
	if (ic->ic_state != IEEE80211_S_AUTH || ni == NULL) {
		obsd_unlock();
		return EINVAL;
	}
	m = ieee80211_getmgmt(M_DONTWAIT, MT_DATA, 6 + len);
	if (m == NULL) {
		obsd_unlock();
		return ENOMEM;
	}
	frm = mtod(m, uint8_t *);
	LE_WRITE_2(frm, IEEE80211_AUTH_ALG_SAE);
	LE_WRITE_2(frm + 2, seq);
	LE_WRITE_2(frm + 4, status);
	memcpy(frm + 6, body, len);
	m->m_pkthdr.len = m->m_len = 6 + len;
	ieee80211_ref_node(ni);
	error = ieee80211_mgmt_output(&ic->ic_if, ni, m, IEEE80211_FC0_SUBTYPE_AUTH);
	if (error == 0)
		ic->ic_mgt_timer = IEEE80211_TRANS_WAIT;
	else
		ieee80211_release_node(ic, ni);
	obsd_unlock();
	return error;
}

/* SAE succeeded: the layer goes on to the association, as after an open authentication */
int
rump_tswifi_auth_done(void)
{
	struct ieee80211com *ic = tsw_ic;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	if (ic->ic_state != IEEE80211_S_AUTH) {
		obsd_unlock();
		return EINVAL;
	}
	auth_reset(ic);
	ieee80211_new_state(ic, IEEE80211_S_ASSOC, IEEE80211_FC0_SUBTYPE_AUTH);
	obsd_unlock();
	return 0;
}

/*
 * The access point being authenticated with is not one to join (its
 * security, or SAE failed): the layer scans again, the node marked as
 * failed, and tries another of the network if there is one
 */
int
rump_tswifi_auth_fail(void)
{
	struct ieee80211com *ic = tsw_ic;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	if (ic->ic_state == IEEE80211_S_AUTH)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	obsd_unlock();
	return 0;
}

/* ---------------------------------------------------------------- keys */

static uint64_t
le48(const uint8_t *seq, int seq_len)
{
	uint64_t v = 0;
	int i;

	for (i = (seq_len > 6 ? 6 : seq_len) - 1; i >= 0; i--)
		v = (v << 8) | seq[i];
	return v;
}

#if !TSWIFI_SIM
/*
 * The frames the card received, as the driver begins to read them
 * (its rx_peek, from obsd_pci.c's dm_sync), before the driver drops
 * any: each MPDU the card wrote, its 802.11 header and the card's
 * status after it (CRC, overrun, the security it found, decrypted,
 * MIC), and the rate it came at. Counted for those to the station; the
 * first protected ones after the port opened are said.
 */
static int	pk_said;

/*
 * The frames to the station that the driver will drop as replays, seen
 * ahead (the records of a machine): iwm and iwx check each frame's
 * packet number against the last one taken for its TID (iwm_ccmp_decap,
 * iwx_ccmp_decap, before net80211's duplicate check and block-ack
 * reordering, which follow in ieee80211_inputm), so a frame the access
 * point sent again, already received and taken, counts there as a
 * replay rather than as a duplicate or as below the reordering window.
 * Whether those dropped are such repeats is seen in the retry bit and
 * in whether the TID's sequence number was received before (the last
 * RP_RING of each TID).
 */
#define RP_RING		64
#define RP_SAID		8
static uint16_t	rp_ring[IEEE80211_NUM_TID][RP_RING];
static uint8_t	rp_ring_n[IEEE80211_NUM_TID];
static uint8_t	rp_pos[IEEE80211_NUM_TID];
static int	rp_said, st_said;

static void
rp_look(const struct ieee80211_frame *wh, uint32_t flen)
{
	struct ieee80211_node *ni = tsw_ic->ic_bss;
	const uint8_t *ivp;
	uint64_t pn, rsc;
	uint16_t seq;
	int hdrlen, tid, i, seen = 0, retry;

	if (ni == NULL || ni->ni_pairwise_key.k_cipher != IEEE80211_CIPHER_CCMP)
		return;
	hdrlen = ieee80211_get_hdrlen(wh);
	if (flen < (uint32_t)hdrlen + IEEE80211_CCMP_HDRLEN)
		return;
	ivp = (const uint8_t *)wh + hdrlen;
	if (!(ivp[3] & IEEE80211_WEP_EXTIV))
		return;
	tid = ieee80211_has_qos(wh) ? ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;
	pn = (uint64_t)ivp[0] | (uint64_t)ivp[1] << 8 | (uint64_t)ivp[4] << 16 |
	    (uint64_t)ivp[5] << 24 | (uint64_t)ivp[6] << 32 | (uint64_t)ivp[7] << 40;
	rsc = ni->ni_pairwise_key.k_rsc[tid];
	seq = letoh16(*(const uint16_t *)wh->i_seq) >> IEEE80211_SEQ_SEQ_SHIFT;
	retry = (wh->i_fc[1] & IEEE80211_FC1_RETRY) != 0;
	for (i = 0; i < rp_ring_n[tid]; i++)
		if (rp_ring[tid][i] == seq)
			seen = 1;
	if (pn <= rsc) {
		rp_all++;
		if (retry)
			rp_retry++;
		if (seen)
			rp_seen++;
		if (!retry && !seen)
			rp_neither++;
		if (rp_said < RP_SAID || (!retry && !seen && rp_said < 2 * RP_SAID)) {
			rp_said++;
			printf("wifi: a frame to the station %s drops as a replay: TID %d, sequence %u,"
			    " retry bit %d, the sequence received before %d, PN %llu, the last PN taken"
			    " for the TID %llu, the TID's block-ack agreement %s from %u\n", tsw_drv->name,
			    tid, seq, retry, seen, (unsigned long long)pn, (unsigned long long)rsc,
			    ni->ni_rx_ba[tid].ba_state == IEEE80211_BA_AGREED ? "made, its window" : "none,",
			    ni->ni_rx_ba[tid].ba_winstart);
		}
		return;
	}
	if (!seen) {
		rp_ring[tid][rp_pos[tid]] = seq;
		rp_pos[tid] = (rp_pos[tid] + 1) % RP_RING;
		if (rp_ring_n[tid] < RP_RING)
			rp_ring_n[tid]++;
	}
}

uint64_t
tsw_port_ns(void)
{
	return port_ns;
}

void
tsw_rx_mpdu(const struct ieee80211_frame *wh, uint32_t flen, uint32_t st, uint32_t rate)
{
	int sec;

	if (tsw_ic == NULL || tsw_drv == NULL)
		return;
	pk_any++;
	if ((st & (TSW_RXS_CRC_OK | TSW_RXS_OVERRUN_OK)) != (TSW_RXS_CRC_OK | TSW_RXS_OVERRUN_OK)) {
		pk_bad_any++;
		return;
	}
	if (!IEEE80211_ADDR_EQ(wh->i_addr1, tsw_ic->ic_myaddr))
		return;
	sec = st & TSW_RXS_SEC_MSK;
	pk_one++;
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
		pk_one_prot++;
	if (sec == TSW_RXS_SEC_CCM)
		pk_one_ccm++;
	if (sec == TSW_RXS_SEC_ERR)
		pk_one_err++;
	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) && !(st & TSW_RXS_KEY_VALID))
		pk_one_nokey++;
	if ((st & (TSW_RXS_DEC_DONE | TSW_RXS_MIC_OK)) == (TSW_RXS_DEC_DONE | TSW_RXS_MIC_OK)) {
		pk_one_dec++;
		if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) &&
		    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
		    !(ieee80211_has_qos(wh) && (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_NODATA)))
			rp_look(wh, flen);
	}
	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) && port_ns != 0 && pk_said < 6) {
		pk_said++;
		printf("wifi: the card received: type %02x to the station from %s,"
		    " %u bytes, status %08x (station found %d, key valid %d, key"
		    " parameters %d, ICV %d, MIC %d, security %d, decrypted %d,"
		    " station %u), rate %08x\n", wh->i_fc[0],
		    ether_sprintf((uint8_t *)wh->i_addr2), flen, st,
		    !!(st & TSW_RXS_SRC_STA_FOUND), !!(st & TSW_RXS_KEY_VALID),
		    !!(st & TSW_RXS_KEY_PARAM_OK), !!(st & TSW_RXS_ICV_OK),
		    !!(st & TSW_RXS_MIC_OK), sec >> 8, !!(st & TSW_RXS_DEC_DONE),
		    (st & TSW_RXS_STA_ID_MSK) >> 24, rate);
	}
}

/*
 * The card's answers to what was sent, the first TXR_SAID after the
 * port opened (the records of a machine): for each frame or aggregate
 * its queue, the frames, the status (acknowledged, or the retry limit
 * reached), the retries, the rate and the TID; and the block-ack answers
 * of the access point to the aggregates. Whether the data the station
 * sends reaches the access point at all is seen in them.
 */
#define TXR_SAID	16
static int	txr_said;

int
tsw_txr_say(void)
{
	if (port_ns == 0 || txr_said >= TXR_SAID)
		return 0;
	txr_said++;
	return 1;
}

/*
 * The card's answers to what was sent during the handshake, from the
 * association until the port opens (the EAPOL frames of the 4-way, sent
 * before any key is in the card), the first TXR_HS_SAID of a join:
 * whether the access point acknowledged the station's messages of the
 * handshake is seen in them.
 */
#define TXR_HS_SAID	8

int
tsw_txr_hs_say(void)
{
	if (port_ns != 0 || tsw_ic == NULL || tsw_ic->ic_state != IEEE80211_S_RUN ||
	    txr_hs_said >= TXR_HS_SAID)
		return 0;
	txr_hs_said++;
	return 1;
}

/* The size the firmware sends, against the structure the driver reads it as */
void
tsw_stats_size(const char *drv, unsigned int got, unsigned int want, const char *api)
{
	if (st_said)
		return;
	st_said = 1;
	printf("wifi: the card's statistics notification: %u bytes, %s reads %u (%s)%s\n", got, drv,
	    want, api, got == want ? "" : ": another version, the card's counts below not to be trusted");
}
#endif

/*
 * A key from the handshake, set as the layer's own handshake sets it:
 * cipher 1 TKIP, 3 CCMP, 6 BIP (the integrity group key, index 4 or 5);
 * addr all ones for the group's; the receive counter as it came (little
 * endian). The pairwise one protects what is sent and received from
 * then on.
 */
int
rump_tswifi_setkey(int cipher, const uint8_t *addr, int idx, int set_tx,
    const uint8_t *seq, int seq_len, const uint8_t *key, int key_len)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;
	struct ieee80211_key *k;
	int error;

	if (ic == NULL)
		return ENXIO;
	if (key_len <= 0 || key_len > (int)sizeof(k->k_key))
		return EINVAL;
	obsd_lock();
	ni = ic->ic_bss;
	if (cipher == 6) {
		if (idx != 4 && idx != 5) {
			obsd_unlock();
			return EINVAL;
		}
		k = &ic->ic_nw_keys[idx];
		memset(k, 0, sizeof(*k));
		k->k_id = idx;
		k->k_cipher = IEEE80211_CIPHER_BIP;
		k->k_flags = IEEE80211_KEY_IGTK;
		k->k_mgmt_rsc = seq != NULL ? le48(seq, seq_len) : 0;
		k->k_len = key_len;
		memcpy(k->k_key, key, key_len);
		error = (*ic->ic_set_key)(ic, ni, k);
		if (error == 0 || error == EBUSY) {
			ni->ni_flags |= IEEE80211_NODE_TXMGMTPROT | IEEE80211_NODE_RXMGMTPROT;
			ic->ic_igtk_kid = idx;
			error = 0;
		}
	} else if (IEEE80211_IS_MULTICAST(addr)) {
		if (idx < 0 || idx > 3) {
			obsd_unlock();
			return EINVAL;
		}
		k = &ic->ic_nw_keys[idx];
		memset(k, 0, sizeof(*k));
		k->k_id = idx;
		k->k_cipher = (cipher == 1) ? IEEE80211_CIPHER_TKIP : IEEE80211_CIPHER_CCMP;
		k->k_flags = IEEE80211_KEY_GROUP | (set_tx ? IEEE80211_KEY_TX : 0);
		k->k_rsc[0] = seq != NULL ? le48(seq, seq_len) : 0;
		k->k_len = key_len;
		memcpy(k->k_key, key, key_len);
		error = (*ic->ic_set_key)(ic, ni, k);
		if (error == EBUSY)
			error = 0;
		if (error == 0)
			ic->ic_rsngroupcipher = ni->ni_rsngroupcipher = k->k_cipher;
	} else {
		k = &ni->ni_pairwise_key;
		memset(k, 0, sizeof(*k));
		k->k_cipher = (cipher == 1) ? IEEE80211_CIPHER_TKIP : IEEE80211_CIPHER_CCMP;
		k->k_rsc[0] = seq != NULL ? le48(seq, seq_len) : 0;
		k->k_len = key_len;
		memcpy(k->k_key, key, key_len);
		ni->ni_rsncipher = k->k_cipher;
#if !TSWIFI_SIM
		if (tsw_drv != NULL && tsw_drv->ptk_to_card != NULL && k->k_cipher == IEEE80211_CIPHER_CCMP)
			error = (*tsw_drv->ptk_to_card)(ic, ni, k);
		else
#endif
		error = (*ic->ic_set_key)(ic, ni, k);
		if (error == EBUSY)
			error = 0;
		if (error == 0) {
			ni->ni_flags &= ~IEEE80211_NODE_RSN_NEW_PTK;
			ni->ni_flags |= IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_RXPROT;
		}
	}
	obsd_unlock();
	return error;
}

/* A key taken away: the pairwise one of addr, or the group's of idx (addr NULL) */
int
rump_tswifi_delkey(const uint8_t *addr, int idx)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	ni = ic->ic_bss;
	if (addr == NULL) {
		if (idx >= 0 && idx < IEEE80211_GROUP_NKID && ic->ic_nw_keys[idx].k_cipher != 0)
			(*ic->ic_delete_key)(ic, ni, &ic->ic_nw_keys[idx]);
	} else if (ni != NULL) {
		(*ic->ic_delete_key)(ic, ni, &ni->ni_pairwise_key);
		ni->ni_flags &= ~(IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_RXPROT);
	}
	obsd_unlock();
	return 0;
}

/* The handshakes are done: data may pass, and the link is up */
int
rump_tswifi_port(int valid)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;

	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	ni = ic->ic_bss;
	if (ni != NULL) {
		ni->ni_port_valid = valid ? 1 : 0;
		port_ns = valid ? getnsecuptime() : 0;
		bg_said = 0;
		drv_said = 0;
		tim_said = 0;
		tx_said = 0;
		arp_said = 0;
		memset(agg_cnt, 0, sizeof(agg_cnt));
#if !TSWIFI_SIM
		pk_said = 0;
		txr_said = 0;
		if (valid && tsw_drv != NULL && tsw_drv->port_say != NULL)
			(*tsw_drv->port_say)();
#endif
		if (valid)
			printf("wifi: power: the layer's power management %s, listen interval %u,"
			    " the access point's beacon interval %u TU, DTIM period %u, AID %u\n",
			    (ic->ic_flags & IEEE80211_F_PMGTON) ? "on" : "off", ic->ic_lintval,
			    ni->ni_intval, ni->ni_dtimperiod, ni->ni_associd & 0x3fff);
		if (valid) {
			ni->ni_assoc_fail = 0;
			ic->ic_rsngroupcipher = ni->ni_rsngroupcipher;
			ieee80211_set_link_state(ic, LINK_STATE_UP);
		}
	}
	obsd_unlock();
	return 0;
}

/*
 * NetBSD's way of joining (the security set, then the access point
 * named), which the manager does not use with this layer
 */
int
rump_tswifi_param(int wpa, int drop)
{
	return 0;
}

int
rump_tswifi_assoc(const uint8_t *bssid, const uint8_t *ssid, int ssid_len)
{
	return EOPNOTSUPP;
}

/* ---------------------------------------------------------------- the state */

/* Mb/s times 10 of one stream at an MCS (0 to 9), for a width (20, 40, 80) */
static const uint16_t rate20[10] = { 65, 130, 195, 260, 390, 520, 585, 650, 780, 867 };
static const uint16_t rate40[10] = { 135, 270, 405, 540, 810, 1080, 1215, 1350, 1620, 1800 };
static const uint16_t rate80[10] = { 293, 585, 878, 1170, 1755, 2340, 2633, 2925, 3510, 3900 };

static int
rate_of(int mcs, int nss, int width, int sgi)
{
	const uint16_t *t = (width >= 80) ? rate80 : (width >= 40) ? rate40 : rate20;
	int r;

	if (mcs < 0 || mcs > 9)
		return 0;
	r = t[mcs] * (nss > 0 ? nss : 1);
	if (sgi)
		r = r * 10 / 9;
	return r * 2 / 10;		/* in 500 kb/s */
}

/* Where the card is: its state, the network it is in, and the rate */
int
rump_tswifi_status(struct tswifi_status *st)
{
	struct ieee80211com *ic = tsw_ic;
	struct ieee80211_node *ni;
	int r;

	memset(st, 0, sizeof(*st));
	if (ic == NULL)
		return ENXIO;
	obsd_lock();
	st->state = ic->ic_state;
	st->scanning = ic->ic_state == IEEE80211_S_SCAN ||
#if TSWIFI_SIM
	    obsd_sim_scanning();
#else
	    (tsw_drv != NULL && (*tsw_drv->scanning)());
#endif
	st->ifflags = ((tsw_ifp->if_flags & IFF_UP) ? 1 : 0) |
	    ((tsw_ifp->if_flags & IFF_RUNNING) ? 2 : 0);
	st->scans = n_scans;
	st->bgscans = n_bgscans;
	st->roams = n_roams;
	ni = ic->ic_bss;
	if (ni != NULL && ic->ic_state >= IEEE80211_S_AUTH) {
		memcpy(st->bssid, ni->ni_bssid, 6);
		st->ssid_len = MIN(ni->ni_esslen, sizeof(st->ssid));
		memcpy(st->ssid, ni->ni_essid, st->ssid_len);
		if (ni->ni_chan != NULL && ni->ni_chan != IEEE80211_CHAN_ANYC)
			st->freq = ni->ni_chan->ic_freq;
		st->rssi = (int)ni->ni_rssi + tsw_min_dbm;
		st->pmf = (ni->ni_flags & IEEE80211_NODE_MFP) != 0;
		st->port = ni->ni_port_valid;
		if (ni->ni_flags & IEEE80211_NODE_VHT) {
			st->phy = 3;
			st->mcs = ni->ni_txmcs;
			st->nss = ni->ni_vht_ss;
			st->width = ieee80211_node_vht_channel_width(ni) ==
			    IEEE80211_VHTOP0_CHAN_WIDTH_80 ? 80 :
			    ieee80211_node_supports_ht_chan40(ni) ? 40 : 20;
			st->sgi = st->width >= 80 ? (ni->ni_flags & IEEE80211_NODE_VHT_SGI80) != 0 :
			    st->width >= 40 ? (ni->ni_flags & IEEE80211_NODE_HT_SGI40) != 0 :
			    (ni->ni_flags & IEEE80211_NODE_HT_SGI20) != 0;
			st->rate = rate_of(st->mcs, st->nss, st->width, st->sgi);
		} else if (ni->ni_flags & IEEE80211_NODE_HT) {
			st->phy = 2;
			st->mcs = ni->ni_txmcs;
			st->nss = ni->ni_txmcs / 8 + 1;
			st->width = ieee80211_node_supports_ht_chan40(ni) ? 40 : 20;
			st->sgi = st->width >= 40 ? (ni->ni_flags & IEEE80211_NODE_HT_SGI40) != 0 :
			    (ni->ni_flags & IEEE80211_NODE_HT_SGI20) != 0;
			st->rate = rate_of(st->mcs % 8, st->nss, st->width, st->sgi);
		} else {
			st->phy = 1;
			st->width = 20;
			r = ni->ni_txrate;
			if (r >= 0 && r < ni->ni_rates.rs_nrates)
				st->rate = ni->ni_rates.rs_rates[r] & IEEE80211_RATE_VAL;
		}
	}
	obsd_unlock();
	return 0;
}
