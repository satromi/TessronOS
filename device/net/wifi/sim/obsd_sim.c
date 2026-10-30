/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_sim.c (the Wi-Fi simulator over OpenBSD's IEEE 802.11 layer,
 *	WIFI_SIM=1 WIFI_NET80211=obsd)
 *	Two simulated radios on one simulated air (design 9.6.12), in the
 *	layer of device/net/wifi/obsd in the place of iwm and its PCI bus.
 *	Compiled with OpenBSD's kernel headers into the layer's object
 *	(tools/openbsd/build.sh, WIFI_SIM=1).
 *
 *	- The station is iwm0, the name the card has, so that the Wi-Fi
 *	  manager (wifi.c) and the layer's component (tswifi_obsd.c) are the
 *	  ones the card has. It moves net80211 as iwm does: a start of the
 *	  interface begins a scan, a scan goes over the channels, a
 *	  background scan (ic_bgscan_start, which net80211 begins by itself
 *	  when the access point's signal is below its threshold) leaves the
 *	  access point's channel for each of the others in turn, and beacons
 *	  missed are answered with a directed probe request. The signal is
 *	  given as iwm gives it, the dBm less IWM_MIN_DBM, with the same
 *	  ic_max_rssi.
 *	- The access point is wsimap0, net80211 in its HOSTAP mode on
 *	  channel 6, with the layer's own authenticator for WPA2-Personal
 *	  (the PSK set in ic_psk): no hostapd. The Ethernet frames it takes
 *	  out go to the simulated access point (wifisim_ap.c) instead of a
 *	  stack (obsd_net.c's if_input), and the EAPOL ones stay with the
 *	  layer's authenticator (tswifi_obsd.c's wrap of
 *	  ieee80211_eapol_key_input).
 *
 *	The air is a timeout of the layer every 10 ms, holding the layer's
 *	lock as all of it runs: the frames each radio sent are handed to the
 *	other's net80211 (ieee80211_input, the crypto in software), when
 *	both run, the access point is on the air and the station's radio is
 *	on the access point's channel (a background scan takes it away);
 *	the access point's beacons go out every 100 ms; a scan moves to the
 *	next channel after 60 ms on one (110 ms for a background scan, which
 *	listens for the beacons only); and the station's beacons are counted
 *	as missed after 100 ms each.
 */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/timeout.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>

#include "obsd_glue.h"

#define WSIM_AP_CHAN	6
#define WSIM_CHANS	11		/* 2.4 GHz, channels 1 to 11 */
#define WSIM_FRAME	2300
#define WSIM_QMAX	256
#define WSIM_TICK_MS	10
#define WSIM_BEACON_MS	100
#define WSIM_DWELL_MS	60
#define WSIM_BGDWELL_MS	110
#define WSIM_ETHER	1514
#define WSIM_MIN_DBM	(-100)		/* as iwm gives the signal: dBm less this */
#define WSIM_MAX_DBM	(-33)

/* The hypercalls (wifi_user_sim.c) */
void	rumpcomp_tswifisim_ap_input(const void *, unsigned long long);

struct wsim_softc {
	struct ieee80211com	sc_ic;		/* first: the interface is in it */
	int			(*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	int			sc_ap;
	struct wsim_softc	*sc_peer;
	struct mbuf_list	sc_q;		/* frames for this radio, on the air */
	uint64_t		sc_scan_at;	/* ms: the scan came to this channel */
	int			sc_scan;	/* 1 a scan, 2 a background one, 0 none */
	int			sc_scanchan;	/* the channel it is on */
	uint64_t		sc_heard_at;	/* ms: the last beacon of the station's access point */
	uint64_t		sc_bmiss_at;
	uint32_t		sc_tx, sc_rx, sc_drop;
};

static struct wsim_softc	*wsim_sta, *wsim_ap;
static struct timeout		wsim_to;
static int			wsim_ap_on = 1;
static int			wsim_rssi = 45;		/* the signal's size in dBm */
static uint32_t			wsim_beacons, wsim_lost_off, wsim_lost_chan, wsim_lost_scan, wsim_bgscans;
static uint32_t			wsim_join_gen;

/*
 * An access point that keeps one record of the sequence numbers it has
 * received from a station for its data frames without the QoS field and
 * for TID 0, as some do, and drops a QoS data frame of TID 0 whose
 * number is not after the last of those (wsim_seqshare, "seqshare on"):
 * the station's numbers are to be continuous for it. Off by default.
 */
static int			wsim_seqshare;
static int			wsim_plain_seen;
static uint16_t			wsim_plain_seq;
static uint32_t			wsim_lost_seqshare;

#define WSIM_JOINS	8
static struct {
	uint8_t		mac[ETHER_ADDR_LEN];
	uint32_t	gen;
} wsim_joins[WSIM_JOINS];

static const uint8_t wsim_sta_mac[ETHER_ADDR_LEN] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x02 };
static const uint8_t wsim_ap_mac[ETHER_ADDR_LEN] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x01 };

static uint64_t
wsim_ms(void)
{
	return obsd_hc_nsec() / 1000000ULL;
}

/* ---------------------------------------------------------------- the air */

/* The channel a radio hears now: a background scan's, or its network's */
static int
wsim_chan(struct wsim_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	if (sc->sc_scan != 0)
		return sc->sc_scanchan;
	if (ic->ic_bss == NULL || ic->ic_bss->ni_chan == NULL ||
	    ic->ic_bss->ni_chan == IEEE80211_CHAN_ANYC)
		return -1;
	return ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
}

static void
wsim_air_send(struct wsim_softc *sc, struct mbuf *m)
{
	struct wsim_softc *to = sc->sc_peer;
	struct mbuf *c;
	int len = m->m_pkthdr.len;

	sc->sc_tx++;
	if (to == NULL)
		return;
	if (!wsim_ap_on || (to->sc_ic.ic_if.if_flags & IFF_RUNNING) == 0) {
		wsim_lost_off++;
		return;
	}
	if (wsim_chan(sc) != wsim_chan(to)) {
		if (sc->sc_scan == 2 || to->sc_scan == 2)
			wsim_lost_scan++;
		else
			wsim_lost_chan++;
		return;
	}
	if (len > WSIM_FRAME || len > MCLBYTES || ml_len(&to->sc_q) >= WSIM_QMAX) {
		sc->sc_drop++;
		return;
	}
	c = m_gethdr(M_DONTWAIT, MT_DATA);
	if (c != NULL && len > MHLEN) {
		MCLGET(c, M_DONTWAIT);
		if (!(c->m_flags & M_EXT)) {
			m_freem(c);
			c = NULL;
		}
	}
	if (c == NULL) {
		sc->sc_drop++;
		return;
	}
	m_copydata(m, 0, len, mtod(c, void *));
	c->m_pkthdr.len = c->m_len = len;
	ml_enqueue(&to->sc_q, c);
}

static void
wsim_air_recv(struct wsim_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct ieee80211_rxinfo rxi;

	if ((ic->ic_if.if_flags & IFF_RUNNING) == 0 || !wsim_ap_on ||
	    m->m_pkthdr.len < (int)sizeof(struct ieee80211_frame_min)) {
		m_freem(m);
		return;
	}
	wh = mtod(m, struct ieee80211_frame *);
	if (!sc->sc_ap && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL &&
	    IEEE80211_ADDR_EQ(wh->i_addr2, ic->ic_bss->ni_bssid) &&
	    (wh->i_fc[0] & (IEEE80211_FC0_TYPE_MASK | IEEE80211_FC0_SUBTYPE_MASK)) ==
	    (IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_BEACON))
		sc->sc_heard_at = wsim_ms();
	if (sc->sc_ap && wsim_seqshare &&
	    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_TODS &&
	    m->m_pkthdr.len >= (int)sizeof(struct ieee80211_qosframe)) {
		uint16_t seq = letoh16(*(const uint16_t *)wh->i_seq) >> IEEE80211_SEQ_SEQ_SHIFT;

		if (!ieee80211_has_qos(wh)) {
			wsim_plain_seq = seq;
			wsim_plain_seen = 1;
		} else if ((ieee80211_get_qos(wh) & IEEE80211_QOS_TID) == 0 && wsim_plain_seen &&
		    ((wsim_plain_seq - seq) & 0xfff) < 2048) {
			wsim_lost_seqshare++;
			m_freem(m);
			return;
		}
	}
	sc->sc_rx++;
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = -wsim_rssi - WSIM_MIN_DBM;	/* as iwm gives it */
	rxi.rxi_chan = 0;		/* as iwm's scans give it: the channel of the frame's own element */
	ni = ieee80211_find_rxnode(ic, wh);
	ieee80211_input(&ic->ic_if, m, ni, &rxi);
	ieee80211_release_node(ic, ni);
}

static void
wsim_beacon(struct wsim_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m;

	if ((ic->ic_if.if_flags & IFF_RUNNING) == 0 || ic->ic_state != IEEE80211_S_RUN ||
	    ic->ic_bss == NULL)
		return;
	m = ieee80211_beacon_alloc(ic, ic->ic_bss);
	if (m == NULL)
		return;
	wsim_beacons++;
	wsim_air_send(sc, m);
	m_freem(m);
}

/*
 * A scan's next channel, or its end, as iwm's card scans all the
 * channels by itself (IEEE80211_C_SCANALL) and tells the end
 * (iwm_endscan): a scan in SCAN (sc_scan 1) with a probe request on
 * each channel, a background one in RUN (sc_scan 2) listening only;
 * ieee80211_end_scan is called from here, as iwm calls it
 */
static void
wsim_scan_step(struct wsim_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	if (ic->ic_state != (sc->sc_scan == 2 ? IEEE80211_S_RUN : IEEE80211_S_SCAN)) {
		sc->sc_scan = 0;
		return;
	}
	if (sc->sc_scanchan >= WSIM_CHANS) {
		sc->sc_scan = 0;
		ieee80211_end_scan(&ic->ic_if);
		return;
	}
	sc->sc_scanchan++;
	sc->sc_scan_at = wsim_ms();
	if (sc->sc_scan == 1) {
		ic->ic_bss->ni_chan = &ic->ic_channels[sc->sc_scanchan];
		if (ic->ic_flags & IEEE80211_F_ASCAN)
			IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_PROBE_REQ, 0);
	}
}

static void
wsim_air(void *arg)
{
	struct wsim_softc *sc;
	struct ieee80211com *ic;
	struct mbuf_list q;
	struct mbuf *m;
	static uint64_t t_beacon;
	uint64_t now;
	int i;

	for (i = 0; i < 2; i++) {
		sc = (i == 0) ? wsim_sta : wsim_ap;
		q = sc->sc_q;
		ml_init(&sc->sc_q);
		while ((m = ml_dequeue(&q)) != NULL)
			wsim_air_recv(sc, m);
	}
	now = wsim_ms();
	if (now - t_beacon >= WSIM_BEACON_MS) {
		t_beacon = now;
		if (wsim_ap_on)
			wsim_beacon(wsim_ap);
	}
	sc = wsim_sta;
	ic = &sc->sc_ic;
	if (sc->sc_scan != 0 && (ic->ic_if.if_flags & IFF_RUNNING) &&
	    now - sc->sc_scan_at >= (sc->sc_scan == 2 ? WSIM_BGDWELL_MS : WSIM_DWELL_MS))
		wsim_scan_step(sc);
	/* beacons missed: a directed probe request, as iwm answers the card's notification */
	if (sc->sc_scan == 0 && ic->ic_state == IEEE80211_S_RUN &&
	    now - sc->sc_heard_at > (uint64_t)WSIM_BEACON_MS * (ic->ic_bmissthres + 1) &&
	    now - sc->sc_bmiss_at >= 1000 && ic->ic_mgt_timer == 0) {
		sc->sc_bmiss_at = now;
		if (ic->ic_if.if_flags & IFF_DEBUG)
			printf("%s: receiving no beacons from %s; checking if this AP is still"
			    " responding to probe requests\n", ic->ic_if.if_xname,
			    ether_sprintf(ic->ic_bss->ni_macaddr));
		IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_PROBE_REQ, 0);
	}
	/* the air as it is, every 30 s, for the records */
	{
		static uint64_t t_say;

		if (now - t_say >= 30000) {
			t_say = now;
			printf("wifisim: air: station state %d channel %d, access point state %d channel %d;"
			    " beacons %u, frames lost: off the air %u, another channel %u, a background scan %u;"
			    " the station sent %u, received %u (of another channel %u, of a bad channel %u),"
			    " the access point sent %u, received %u\n",
			    wsim_sta->sc_ic.ic_state, wsim_chan(wsim_sta), wsim_ap->sc_ic.ic_state,
			    wsim_chan(wsim_ap), wsim_beacons, wsim_lost_off, wsim_lost_chan, wsim_lost_scan,
			    wsim_sta->sc_tx, wsim_sta->sc_rx, wsim_sta->sc_ic.ic_stats.is_rx_chanmismatch,
			    wsim_sta->sc_ic.ic_stats.is_rx_badchan, wsim_ap->sc_tx, wsim_ap->sc_rx);
		}
	}
	timeout_add_msec(&wsim_to, WSIM_TICK_MS);
}

/* ---------------------------------------------------------------- the radios */

static void
wsim_start(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ieee80211_frame *wh;
	struct ieee80211_key *k;
	struct ether_header *eh;
	struct mbuf *m;

	if (!(ifp->if_flags & IFF_RUNNING))
		return;
	for (;;) {
		m = mq_dequeue(&ic->ic_mgtq);
		if (m != NULL) {
			ni = m->m_pkthdr.ph_cookie;
		} else {
			if (ic->ic_state != IEEE80211_S_RUN ||
			    (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY))
				break;
			m = ifq_dequeue(&ifp->if_snd);
			if (m == NULL)
				break;
			if (m->m_len < sizeof(*eh) && (m = m_pullup(m, sizeof(*eh))) == NULL) {
				ifp->if_oerrors++;
				continue;
			}
			if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
				ifp->if_oerrors++;
				continue;
			}
		}
		wh = mtod(m, struct ieee80211_frame *);
		if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
			k = ieee80211_get_txkey(ic, wh, ni);
			if ((m = ieee80211_encrypt(ic, m, k)) == NULL) {
				ieee80211_release_node(ic, ni);
				ifp->if_oerrors++;
				continue;
			}
		}
		ifp->if_opackets++;
		wsim_air_send(sc, m);
		m_freem(m);
		ieee80211_release_node(ic, ni);
	}
}

static int
wsim_init(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	ifp->if_flags |= IFF_RUNNING;
	if (sc->sc_ap) {
		/*
		 * on its channel at once (ieee80211_create_ibss on ic_des_chan),
		 * and RUN; the BSS node on that channel first, as net80211's
		 * move to SCAN reads its channel for the rates
		 */
		ic->ic_bss->ni_chan = ic->ic_des_chan;
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	} else {
		/* the station scans as iwm's start does */
		ieee80211_begin_scan(ifp);
	}
	return 0;
}

static void
wsim_stop(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;

	sc->sc_scan = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	if (sc->sc_ic.ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(&sc->sc_ic, IEEE80211_S_INIT, -1);
}

static int
wsim_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int err = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING))
				err = wsim_init(ifp);
		} else if (ifp->if_flags & IFF_RUNNING) {
			wsim_stop(ifp);
		}
		break;
	default:
		err = ieee80211_ioctl(ifp, cmd, data);
		break;
	}
	if (err == ENETRESET) {
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) {
			wsim_stop(ifp);
			err = wsim_init(ifp);
		}
	}
	return err;
}

static void
wsim_watchdog(struct ifnet *ifp)
{
	ifp->if_timer = 0;
	ieee80211_watchdog(ifp);
}

static int
wsim_media_change(struct ifnet *ifp)
{
	int err = ieee80211_media_change(ifp);

	return err == ENETRESET ? 0 : err;
}

static int
wsim_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct wsim_softc *sc = ic->ic_if.if_softc;

	if (nstate == IEEE80211_S_RUN)
		sc->sc_heard_at = sc->sc_bmiss_at = wsim_ms();
	if (nstate != IEEE80211_S_SCAN && !(nstate == IEEE80211_S_RUN && sc->sc_scan == 2))
		sc->sc_scan = 0;
	if (!sc->sc_ap && nstate == IEEE80211_S_SCAN) {
		/*
		 * The station's scan is its own walk over the channels, as
		 * iwm's card scans (iwm_scan): begun once, a move to SCAN
		 * while it goes on being nothing more
		 */
		if (sc->sc_scan == 1)
			return 0;
		sc->sc_scan = 1;
		sc->sc_scanchan = 1;
		sc->sc_scan_at = wsim_ms();
		ic->ic_bss->ni_chan = &ic->ic_channels[1];
	}
	return sc->sc_newstate(ic, nstate, arg);
}

/* A background scan begun by net80211: the channels in turn, the access point's left for each */
static int
wsim_bgscan(struct ieee80211com *ic)
{
	struct wsim_softc *sc = ic->ic_if.if_softc;

	if (sc->sc_scan != 0)
		return EBUSY;
	sc->sc_scan = 2;
	sc->sc_scanchan = 1;
	sc->sc_scan_at = wsim_ms();
	wsim_bgscans++;
	return 0;
}

/* A station (re)associated with the access point: its join numbered */
static void
wsim_newassoc(struct ieee80211com *ic, struct ieee80211_node *ni, int isnew)
{
	struct wsim_softc *sc = ic->ic_if.if_softc;
	int i, k = -1;

	if (!sc->sc_ap)
		return;
	for (i = 0; i < WSIM_JOINS; i++) {
		if (IEEE80211_ADDR_EQ(wsim_joins[i].mac, ni->ni_macaddr)) {
			k = i;
			break;
		}
		if (k < 0 && wsim_joins[i].gen == 0)
			k = i;
	}
	if (k < 0)
		k = 0;
	memcpy(wsim_joins[k].mac, ni->ni_macaddr, ETHER_ADDR_LEN);
	wsim_joins[k].gen = ++wsim_join_gen;
}

static struct wsim_softc *
wsim_attach(const char *name, int ap, const uint8_t *mac)
{
	struct wsim_softc *sc;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int i;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	ic = &sc->sc_ic;
	ifp = &ic->ic_if;
	sc->sc_ap = ap;
	ml_init(&sc->sc_q);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = ap ? IEEE80211_M_HOSTAP : IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	/*
	 * Both radios 802.11n with QoS and sending A-MPDUs, as iwm and the
	 * access points are: the station asks for block-ack agreements for
	 * what it sends, and the access point's net80211 reorders what it
	 * receives under one, dropping what is below its window
	 * (ieee80211_input_ba), as an access point does
	 */
	ic->ic_caps = IEEE80211_C_WEP | IEEE80211_C_RSN | IEEE80211_C_SHSLOT |
	    IEEE80211_C_SHPREAMBLE | IEEE80211_C_QOS | IEEE80211_C_TX_AMPDU |
	    (ap ? IEEE80211_C_HOSTAP : IEEE80211_C_SCANALL);
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	ic->ic_htcaps = IEEE80211_HTCAP_SGI20;
	ic->ic_htxcaps = 0;
	ic->ic_txbfcaps = 0;
	ic->ic_aselcaps = 0;
	ic->ic_ampdu_params = (IEEE80211_AMPDU_PARAM_SS_4 | 0x3 /* 64k */);
	memset(ic->ic_sup_mcs, 0, sizeof(ic->ic_sup_mcs));
	ic->ic_sup_mcs[0] = 0xff;		/* MCS 0-7 */
	for (i = 1; i <= WSIM_CHANS; i++) {
		ic->ic_channels[i].ic_freq = ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_HT;
	}
	ic->ic_ibss_chan = &ic->ic_channels[WSIM_AP_CHAN];
	ic->ic_max_rssi = WSIM_MAX_DBM - WSIM_MIN_DBM;
	memcpy(ic->ic_myaddr, mac, ETHER_ADDR_LEN);

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = wsim_ioctl;
	ifp->if_start = wsim_start;
	ifp->if_watchdog = wsim_watchdog;
	strlcpy(ifp->if_xname, name, IFNAMSIZ);
	if_attach(ifp);
	ieee80211_ifattach(ifp);
	ieee80211_media_init(ifp, wsim_media_change, ieee80211_media_status);
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = wsim_newstate;
	ic->ic_newassoc = wsim_newassoc;
	if (!ap)
		ic->ic_bgscan_start = wsim_bgscan;
	else
		ic->ic_des_chan = &ic->ic_channels[WSIM_AP_CHAN];
	return sc;
}

/*
 * The two radios and the air, from the layer's start (tswifi_obsd.c's
 * rump_tswifi_start, in the place of iwm's attachment): the station's
 * net80211, which the component drives as the card's
 */
struct ieee80211com	*obsd_sim_attach(void);

struct ieee80211com *
obsd_sim_attach(void)
{
	if (wsim_sta != NULL)
		return &wsim_sta->sc_ic;
	wsim_sta = wsim_attach("iwm0", 0, wsim_sta_mac);
	wsim_ap = wsim_attach("wsimap0", 1, wsim_ap_mac);
	wsim_sta->sc_peer = wsim_ap;
	wsim_ap->sc_peer = wsim_sta;
	timeout_set(&wsim_to, wsim_air, NULL);
	timeout_add_msec(&wsim_to, WSIM_TICK_MS);
	printf("wifisim: iwm0 (station) and wsimap0 (access point) on the simulated air\n");
	return &wsim_sta->sc_ic;
}

/* Whether the interface is the access point's (obsd_net.c, tswifi_obsd.c) */
int	obsd_sim_is_ap(struct ifnet *);

int
obsd_sim_is_ap(struct ifnet *ifp)
{
	return wsim_ap != NULL && ifp == &wsim_ap->sc_ic.ic_if;
}

/* Whether the station scans (rump_tswifi_status): a scan or a background one */
int	obsd_sim_scanning(void);

int
obsd_sim_scanning(void)
{
	return wsim_sta != NULL && wsim_sta->sc_scan != 0;
}

/* An Ethernet frame the access point took out for its stack: to the simulated access point */
void	obsd_sim_ap_input(const uint8_t *, int);

void
obsd_sim_ap_input(const uint8_t *frame, int len)
{
	rumpcomp_tswifisim_ap_input(frame, (unsigned long long)len);
}

/* ---------------------------------------------------------------- the access point's entry points */

/* One station of the access point's, as rump_tswifisim_ap_stations tells it (tswifi_sim.c's layout) */
struct tswifisim_sta {
	uint8_t		mac[6];
	uint8_t		authorized;
	uint8_t		ie_len;
	uint32_t	gen;
	uint8_t		ie[64];
};

int	rump_tswifisim_ap_config(const uint8_t *, int, int, const uint8_t *);
int	rump_tswifisim_ap_send(const void *, int);
int	rump_tswifisim_ap_setkey(int, const uint8_t *, int, const uint8_t *, int);
int	rump_tswifisim_ap_port(const uint8_t *, int);
int	rump_tswifisim_ap_kick(const uint8_t *, int);
int	rump_tswifisim_ap_mac(uint8_t *);
int	rump_tswifisim_ap_stations(struct tswifisim_sta *, int);
int	rump_tswifisim_stats(uint32_t *, int);
void	rump_tswifisim_air(int, int);

/* A station of the access point sent away (its network is changed) */
static void
wsim_kick(void *arg, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = arg;

	if (ni != ic->ic_bss && ni->ni_state >= IEEE80211_STA_AUTH)
		IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_DEAUTH,
		    IEEE80211_REASON_AUTH_LEAVE);
}

/*
 * The access point's network: its SSID, and wpa 0 (open) or 2 (WPA2-
 * Personal with the PSK psk, CCMP; the layer's own authenticator does
 * the handshakes); its radio started again on channel 6
 */
int
rump_tswifisim_ap_config(const uint8_t *ssid, int ssid_len, int wpa, const uint8_t *psk)
{
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int error;

	if (wsim_ap == NULL)
		return ENXIO;
	if (ssid_len <= 0 || ssid_len > IEEE80211_NWID_LEN || (wpa && psk == NULL))
		return EINVAL;
	ic = &wsim_ap->sc_ic;
	ifp = &ic->ic_if;
	obsd_lock();
	if (ifp->if_flags & IFF_RUNNING) {
		/* its stations told it leaves, as an access point started again tells them */
		ieee80211_iterate_nodes(ic, wsim_kick, ic);
		wsim_start(ifp);
		wsim_stop(ifp);
	}
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	memcpy(ic->ic_des_essid, ssid, ssid_len);
	ic->ic_des_esslen = ssid_len;
	if (wpa) {
		ic->ic_rsnprotos = IEEE80211_PROTO_RSN;
		ic->ic_rsnakms = IEEE80211_AKM_PSK;
		ic->ic_rsnciphers = IEEE80211_CIPHER_CCMP;
		ic->ic_rsngroupcipher = IEEE80211_CIPHER_CCMP;
		memcpy(ic->ic_psk, psk, IEEE80211_PMK_LEN);
		ic->ic_flags |= IEEE80211_F_RSNON | IEEE80211_F_PSK;
	} else {
		ic->ic_flags &= ~(IEEE80211_F_RSNON | IEEE80211_F_PSK);
		memset(ic->ic_psk, 0, sizeof(ic->ic_psk));
	}
	ifp->if_flags |= IFF_UP | IFF_DEBUG;
	error = wsim_init(ifp);
	obsd_unlock();
	printf("wifisim: the access point's network %s, channel %d (%d)\n",
	    wpa ? "WPA2-Personal" : "open", WSIM_AP_CHAN, error);
	return error;
}

int
rump_tswifisim_ap_mac(uint8_t *mac)
{
	memcpy(mac, wsim_ap_mac, ETHER_ADDR_LEN);
	return 0;
}

int
rump_tswifisim_ap_send(const void *frame, int len)
{
	struct ifnet *ifp;
	struct mbuf *m;
	int error;

	if (wsim_ap == NULL)
		return ENXIO;
	if (len < (int)sizeof(struct ether_header) || len > WSIM_ETHER)
		return EINVAL;
	ifp = &wsim_ap->sc_ic.ic_if;
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
	error = if_enqueue(ifp, m);
	obsd_unlock();
	return error;
}

/* The layer's authenticator sets the keys and opens the ports itself: nothing to do */
int
rump_tswifisim_ap_setkey(int cipher, const uint8_t *addr, int idx, const uint8_t *key, int key_len)
{
	return 0;
}

int
rump_tswifisim_ap_port(const uint8_t *addr, int on)
{
	return 0;
}

/* A station sent away with the reason */
int
rump_tswifisim_ap_kick(const uint8_t *addr, int reason)
{
	struct ieee80211com *ic;
	struct ieee80211_node *ni;

	if (wsim_ap == NULL)
		return ENXIO;
	ic = &wsim_ap->sc_ic;
	obsd_lock();
	ni = ieee80211_find_node(ic, addr);
	if (ni != NULL) {
		IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_DEAUTH, reason);
		ieee80211_node_leave(ic, ni);
	}
	obsd_unlock();
	return ni != NULL ? 0 : ENOENT;
}

/* The stations associated with the access point: authorized is the port open (the handshake done) */
int
rump_tswifisim_ap_stations(struct tswifisim_sta *list, int max)
{
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	struct tswifisim_sta *t;
	int n = 0, i;

	if (wsim_ap == NULL)
		return -ENXIO;
	ic = &wsim_ap->sc_ic;
	obsd_lock();
	RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
		if (ni == ic->ic_bss || ni->ni_associd == 0)
			continue;
		if (n < max) {
			t = &list[n];
			memset(t, 0, sizeof(*t));
			memcpy(t->mac, ni->ni_macaddr, 6);
			t->authorized = (ic->ic_flags & IEEE80211_F_RSNON) ? ni->ni_port_valid : 1;
			for (i = 0; i < WSIM_JOINS; i++) {
				if (IEEE80211_ADDR_EQ(wsim_joins[i].mac, ni->ni_macaddr))
					t->gen = wsim_joins[i].gen;
			}
		}
		n++;
	}
	obsd_unlock();
	return n;
}

/* The access point off the air (its network started again, no station) or back; the signal */
void
rump_tswifisim_air(int on, int rssi)
{
	struct ifnet *ifp;

	if (wsim_ap == NULL)
		return;
	obsd_lock();
	if (rssi >= 0)
		wsim_rssi = rssi;
	if (on >= 0 && (on != 0) != (wsim_ap_on != 0)) {
		ifp = &wsim_ap->sc_ic.ic_if;
		if (!on) {
			wsim_ap_on = 0;
			if (ifp->if_flags & IFF_RUNNING) {
				wsim_stop(ifp);
				(void)wsim_init(ifp);
			}
		} else {
			wsim_ap_on = 1;
		}
		printf("wifisim: the access point %s the air\n", on ? "back on" : "off");
	}
	obsd_unlock();
}

int	rump_tswifisim_seqshare(int);

/* The access point's one record of the numbers (wsim_seqshare) on or off, forgotten */
int
rump_tswifisim_seqshare(int on)
{
	obsd_lock();
	wsim_seqshare = on;
	wsim_plain_seen = 0;
	wsim_lost_seqshare = 0;
	obsd_unlock();
	return 0;
}

/*
 * The air's counts, as tswifi_sim.c's: v[0..2] the station's frames
 * sent, received, dropped; v[3..5] the access point's; v[6] beacons;
 * v[7] frames lost with the access point off the air, v[8] on another
 * channel; v[9] the station's data frames dropped for its port not
 * open, v[10] the access point's; v[11] the station's unencrypted
 * frames dropped, v[12] its decryption failures; v[13] the joins;
 * v[14] frames lost while a background scan had the station's radio on
 * another channel; v[15] the background scans; v[16] the block-ack
 * agreements for what the station sends, v[17] as the access point
 * counts them; v[18] the station's frames the access point dropped as
 * below the agreement's window or as duplicates; v[19] those dropped by
 * its one record of the numbers (wsim_seqshare)
 */
int
rump_tswifisim_stats(uint32_t *v, int n)
{
	uint32_t a[20];
	int i;

	memset(a, 0, sizeof(a));
	obsd_lock();
	if (wsim_sta != NULL && wsim_ap != NULL) {
		a[0] = wsim_sta->sc_tx;
		a[1] = wsim_sta->sc_rx;
		a[2] = wsim_sta->sc_drop;
		a[3] = wsim_ap->sc_tx;
		a[4] = wsim_ap->sc_rx;
		a[5] = wsim_ap->sc_drop;
		a[6] = wsim_beacons;
		a[7] = wsim_lost_off;
		a[8] = wsim_lost_chan;
		a[9] = wsim_sta->sc_ic.ic_stats.is_rx_unauth;
		a[10] = wsim_ap->sc_ic.ic_stats.is_rx_unauth;
		a[11] = wsim_sta->sc_ic.ic_stats.is_rx_unencrypted;
		a[12] = wsim_sta->sc_ic.ic_stats.is_rx_decryptcrc + wsim_sta->sc_ic.ic_stats.is_ccmp_dec_errs;
		a[13] = wsim_join_gen;
		a[14] = wsim_lost_scan;
		a[15] = wsim_bgscans;
		a[16] = wsim_sta->sc_ic.ic_stats.is_ht_tx_ba_agreements;
		a[17] = wsim_ap->sc_ic.ic_stats.is_ht_rx_ba_agreements;
		a[18] = wsim_ap->sc_ic.ic_stats.is_ht_rx_frame_below_ba_winstart +
		    wsim_ap->sc_ic.ic_stats.is_rx_dup;
		a[19] = wsim_lost_seqshare;
	}
	obsd_unlock();
	for (i = 0; i < n && i < 20; i++)
		v[i] = a[i];
	return i;
}
