/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tswifi_sim.c (the Wi-Fi simulator, WIFI_SIM=1)
 *	Two simulated radios on one simulated air (design 9.6.12): a
 *	station, which stands where the card is, and an access point.
 *
 *	This file is compiled with the NetBSD kernel headers into the rump
 *	kernel of an Arm build with the simulator (tools/netbsd/build.sh,
 *	WIFI_SIM=1) in the place of the card's
 *	driver and its bus. Both radios are net80211 interfaces of their
 *	own, as a driver attaches one, with the 802.11 layer doing all of
 *	what it does for a card that leaves the crypto and the management
 *	frames to it:
 *
 *	- the station has the name of the Raspberry Pi 5's chip, bwfm0, so
 *	  that the Wi-Fi manager (device/net/wifi/wifi.c) and the component
 *	  it drives the card with (rumpcomp/wifi/tswifi_if.c) are the ones
 *	  the chip has, unchanged. It moves net80211 as bwfm does: a start
 *	  begins no scan (the manager asks for the first); the signal is
 *	  given as a signed dBm. (The x64 PC's simulator is in OpenBSD's
 *	  layer, device/net/wifi/sim/obsd_sim.c);
 *	- the access point, wsimap0, is net80211 in its HOSTAP mode on
 *	  channel 6. The simulated access point outside the rump kernel
 *	  (device/net/wifi/sim/wifisim_ap.c) sets its network up, gets the
 *	  Ethernet frames it receives (rumpcomp_tswifisim_ap_input) instead
 *	  of a stack, sends frames through it, sets its keys and opens a
 *	  station's port, as hostapd does through the same ioctls.
 *
 *	The air is a thread of the rump kernel: what one radio sends is
 *	copied and queued for the other, and the thread hands it to the
 *	other's net80211 (ieee80211_input) with the kernel lock taken, as a
 *	driver's receive interrupt would. A frame reaches the other radio
 *	only while both run, both are on the same channel and the access
 *	point is on the air. The same thread sends the access point's
 *	beacons every 100 ms, moves the station's scan to the next channel
 *	after 60 ms on one, tells net80211 of a beacon miss when the
 *	station has heard nothing from its access point for 300 ms (which
 *	net80211 answers with a probe and then a scan), and runs net80211's
 *	watchdog every second.
 *
 *	Taking the access point off the air drops all it would send or
 *	receive, and starts its network again, as one that was switched
 *	off: its stations are forgotten. A station that was joined loses
 *	its beacons and leaves as from a network out of reach.
 */

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>

#include <rump-sys/kern.h>

#define WSIM_STA_NAME	"bwfm"
#define WSIM_AP_IF	"wsimap0"
#define WSIM_AP_CHAN	6
#define WSIM_CHANS	11		/* 2.4 GHz, channels 1 to 11 */
#define WSIM_FRAME	2300		/* the longest 802.11 frame carried */
#define WSIM_QMAX	256		/* frames queued for one radio */
#define WSIM_BEACON_MS	100
#define WSIM_DWELL_MS	60
#define WSIM_BMISS_MS	300
#define WSIM_ETHER	1514

/* The hypercalls (wifi_user_sim.c) */
void	rumpcomp_tswifisim_ap_input(const void *, size_t);

struct wsim_softc {
	struct ethercom		sc_ec;
	struct ieee80211com	sc_ic;
	kmutex_t		sc_media_mtx;
	int			(*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);
	int			sc_ap;		/* 1 the access point */
	struct wsim_softc	*sc_peer;
	struct mbuf		*sc_qhead, *sc_qtail;	/* frames for this radio, on the air */
	int			sc_qlen;
	uint64_t		sc_scan_at;	/* ms: the scan came to this channel */
	uint64_t		sc_heard_at;	/* ms: the last frame from the station's access point */
	uint64_t		sc_bmiss_at;	/* ms: the last beacon miss told */
	uint32_t		sc_tx, sc_rx, sc_drop;
};

static struct wsim_softc	*wsim_sta, *wsim_ap;
static kmutex_t			wsim_mtx;
static kcondvar_t		wsim_cv;
static struct lwp		*wsim_lwp;
static volatile int		wsim_ap_on = 1;		/* the access point is on the air */
static volatile int		wsim_rssi = 45;		/* the signal's size in dBm, the sign dropped */
static uint32_t			wsim_beacons, wsim_lost_off, wsim_lost_chan;
static uint32_t			wsim_join_gen;

/* A station's joins, numbered: a join again of the same station is told apart */
#define WSIM_JOINS	8
static struct {
	uint8_t		mac[ETHER_ADDR_LEN];
	uint32_t	gen;
} wsim_joins[WSIM_JOINS];

static const uint8_t wsim_sta_mac[ETHER_ADDR_LEN] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x02 };
static const uint8_t wsim_ap_mac[ETHER_ADDR_LEN] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0x01 };
static const uint8_t wsim_ap_ifmac[ETHER_ADDR_LEN] = { 0x02, 0x77, 0x73, 0x00, 0x00, 0xff };

static uint64_t
wsim_ms(void)
{
	struct timeval tv;

	getmicrouptime(&tv);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------------------------------------------------------------- the air */

static int
wsim_chan(struct wsim_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	if (ic->ic_curchan == NULL || ic->ic_curchan == IEEE80211_CHAN_ANYC)
		return -1;
	return ieee80211_chan2ieee(ic, ic->ic_curchan);
}

/*
 * One frame sent by sc: a copy queued for the other radio, when it can
 * hear it. The frame is the sender's to free.
 */
static void
wsim_air_send(struct wsim_softc *sc, struct mbuf *m)
{
	struct wsim_softc *to = sc->sc_peer;
	struct ifnet *toifp;
	struct mbuf *c;
	int len = m->m_pkthdr.len;

	sc->sc_tx++;
	if (to == NULL)
		return;
	toifp = &to->sc_ec.ec_if;
	if (!wsim_ap_on || (toifp->if_flags & IFF_RUNNING) == 0) {
		wsim_lost_off++;
		return;
	}
	if (wsim_chan(sc) != wsim_chan(to)) {
		wsim_lost_chan++;
		return;
	}
	if (len > WSIM_FRAME || len > MCLBYTES) {
		sc->sc_drop++;
		return;
	}
	MGETHDR(c, M_DONTWAIT, MT_DATA);
	if (c == NULL) {
		sc->sc_drop++;
		return;
	}
	if (len > MHLEN) {
		MCLGET(c, M_DONTWAIT);
		if ((c->m_flags & M_EXT) == 0) {
			m_freem(c);
			sc->sc_drop++;
			return;
		}
	}
	m_copydata(m, 0, len, mtod(c, void *));
	c->m_len = c->m_pkthdr.len = len;
	m_set_rcvif(c, toifp);
	mutex_enter(&wsim_mtx);
	if (to->sc_qlen >= WSIM_QMAX) {
		mutex_exit(&wsim_mtx);
		m_freem(c);
		to->sc_drop++;
		return;
	}
	c->m_nextpkt = NULL;
	if (to->sc_qtail != NULL)
		to->sc_qtail->m_nextpkt = c;
	else
		to->sc_qhead = c;
	to->sc_qtail = c;
	to->sc_qlen++;
	cv_signal(&wsim_cv);
	mutex_exit(&wsim_mtx);
}

/* A frame off the air into sc's net80211; the kernel lock held */
static void
wsim_air_recv(struct wsim_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	int rssi;

	if ((sc->sc_ec.ec_if.if_flags & IFF_RUNNING) == 0 || !wsim_ap_on
	    || m->m_pkthdr.len < (int)sizeof(struct ieee80211_frame_min)) {
		m_freem(m);
		return;
	}
	wh = mtod(m, struct ieee80211_frame *);
	if (!sc->sc_ap && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL
	    && IEEE80211_ADDR_EQ(wh->i_addr2, ic->ic_bss->ni_bssid))
		sc->sc_heard_at = wsim_ms();
	sc->sc_rx++;
	rssi = -wsim_rssi;		/* dBm, signed, as bwfm gives it */
	ni = ieee80211_find_rxnode(ic, (struct ieee80211_frame_min *)wh);
	ieee80211_input(ic, m, ni, rssi, 0);
	ieee80211_free_node(ni);
}

/* The access point's beacon: made anew each time */
static void
wsim_beacon(struct wsim_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_beacon_offsets bo;
	struct mbuf *m;

	if ((sc->sc_ec.ec_if.if_flags & IFF_RUNNING) == 0 || ic->ic_state != IEEE80211_S_RUN
	    || ic->ic_bss == NULL)
		return;
	m = ieee80211_beacon_alloc(ic, ic->ic_bss, &bo);
	if (m == NULL)
		return;
	wsim_beacons++;
	wsim_air_send(sc, m);
	m_freem(m);
}

static void
wsim_air(void *arg)
{
	struct wsim_softc *sc;
	struct ieee80211com *ic;
	struct mbuf *q[2], *m, *n;
	uint64_t now, t_beacon = 0, t_dog = 0;
	int i, s;

	for (;;) {
		mutex_enter(&wsim_mtx);
		if (wsim_sta->sc_qhead == NULL && wsim_ap->sc_qhead == NULL)
			(void)cv_timedwait(&wsim_cv, &wsim_mtx, MAX(1, mstohz(10)));
		q[0] = wsim_sta->sc_qhead;
		q[1] = wsim_ap->sc_qhead;
		wsim_sta->sc_qhead = wsim_sta->sc_qtail = NULL;
		wsim_ap->sc_qhead = wsim_ap->sc_qtail = NULL;
		wsim_sta->sc_qlen = wsim_ap->sc_qlen = 0;
		mutex_exit(&wsim_mtx);

		KERNEL_LOCK(1, NULL);
		s = splnet();
		for (i = 0; i < 2; i++) {
			sc = (i == 0) ? wsim_sta : wsim_ap;
			for (m = q[i]; m != NULL; m = n) {
				n = m->m_nextpkt;
				m->m_nextpkt = NULL;
				wsim_air_recv(sc, m);
			}
		}

		now = wsim_ms();
		if (now - t_beacon >= WSIM_BEACON_MS) {
			t_beacon = now;
			if (wsim_ap_on)
				wsim_beacon(wsim_ap);
		}
		ic = &wsim_sta->sc_ic;
		if ((wsim_sta->sc_ec.ec_if.if_flags & IFF_RUNNING) != 0
		    && ic->ic_state == IEEE80211_S_SCAN && (ic->ic_flags & IEEE80211_F_SCAN) != 0
		    && now - wsim_sta->sc_scan_at >= WSIM_DWELL_MS) {
			wsim_sta->sc_scan_at = now;
			(void)ieee80211_next_scan(ic);
		}
		if (ic->ic_state == IEEE80211_S_RUN && now - wsim_sta->sc_heard_at >= WSIM_BMISS_MS
		    && now - wsim_sta->sc_bmiss_at >= WSIM_BEACON_MS) {
			wsim_sta->sc_bmiss_at = now;
			ieee80211_beacon_miss(ic);
		}
		if (now - t_dog >= 1000) {
			t_dog = now;
			ieee80211_watchdog(&wsim_sta->sc_ic);
			ieee80211_watchdog(&wsim_ap->sc_ic);
		}
		splx(s);
		KERNEL_UNLOCK_ONE(NULL);
	}
}

/* ---------------------------------------------------------------- the radios */

static void
wsim_start(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ieee80211_frame *wh;
	struct ether_header *eh;
	struct mbuf *m;

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return;
	for (;;) {
		IF_DEQUEUE(&ic->ic_mgtq, m);
		if (m != NULL) {
			ni = M_GETCTX(m, struct ieee80211_node *);
			M_CLEARCTX(m);
		} else {
			if (ic->ic_state != IEEE80211_S_RUN)
				break;
			IFQ_DEQUEUE(&ifp->if_snd, m);
			if (m == NULL)
				break;
			if (m->m_len < (int)sizeof(*eh) && (m = m_pullup(m, sizeof(*eh))) == NULL) {
				if_statinc(ifp, if_oerrors);
				continue;
			}
			eh = mtod(m, struct ether_header *);
			ni = ieee80211_find_txnode(ic, eh->ether_dhost);
			if (ni == NULL) {
				m_freem(m);
				if_statinc(ifp, if_oerrors);
				continue;
			}
			if (ieee80211_classify(ic, m, ni) != 0) {
				m_freem(m);
				ieee80211_free_node(ni);
				if_statinc(ifp, if_oerrors);
				continue;
			}
			if ((m = ieee80211_encap(ic, m, ni)) == NULL) {
				ieee80211_free_node(ni);
				if_statinc(ifp, if_oerrors);
				continue;
			}
		}
		wh = mtod(m, struct ieee80211_frame *);
		if (wh->i_fc[1] & IEEE80211_FC1_WEP) {
			if (ni == NULL || ieee80211_crypto_encap(ic, ni, m) == NULL) {
				m_freem(m);
				if (ni != NULL)
					ieee80211_free_node(ni);
				if_statinc(ifp, if_oerrors);
				continue;
			}
		}
		if_statinc(ifp, if_opackets);
		wsim_air_send(sc, m);
		m_freem(m);
		if (ni != NULL)
			ieee80211_free_node(ni);
	}
}

static int
wsim_init(struct ifnet *ifp)
{
	struct wsim_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	ifp->if_flags |= IFF_RUNNING;
	/* the access point on its channel at once (ieee80211_create_ibss), and RUN; the station waits */
	if (sc->sc_ap)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	return 0;
}

static void
wsim_stop(struct ifnet *ifp, int disable)
{
	struct wsim_softc *sc = ifp->if_softc;

	ifp->if_flags &= ~IFF_RUNNING;
	if (sc->sc_ic.ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(&sc->sc_ic, IEEE80211_S_INIT, -1);
}

static int
wsim_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct wsim_softc *sc = ifp->if_softc;
	const struct sockaddr *sa;
	int s, err = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		err = ifioctl_common(ifp, cmd, data);
		if (err)
			break;
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_flags & IFF_RUNNING) == 0)
				err = wsim_init(ifp);
		} else if (ifp->if_flags & IFF_RUNNING) {
			wsim_stop(ifp, 1);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		sa = ifreq_getaddr(SIOCADDMULTI, (struct ifreq *)data);
		err = (cmd == SIOCADDMULTI) ? ether_addmulti(sa, &sc->sc_ec)
		    : ether_delmulti(sa, &sc->sc_ec);
		if (err == ENETRESET)
			err = 0;
		break;
	default:
		err = ieee80211_ioctl(&sc->sc_ic, cmd, data);
		break;
	}
	if (err == ENETRESET) {
		/* a change of the security: the radio started again, as a card is */
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) {
			wsim_stop(ifp, 0);
			err = wsim_init(ifp);
		}
	}
	splx(s);
	return err;
}

/* Each channel of a scan timed; the time of the join, for the beacons missed */
static int
wsim_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct wsim_softc *sc = ic->ic_ifp->if_softc;

	if (nstate == IEEE80211_S_SCAN)
		sc->sc_scan_at = wsim_ms();
	if (nstate == IEEE80211_S_RUN)
		sc->sc_heard_at = sc->sc_bmiss_at = wsim_ms();
	return sc->sc_newstate(ic, nstate, arg);
}

/* A station (re)associated with the access point, or the station with its access point */
static void
wsim_newassoc(struct ieee80211_node *ni, int isnew)
{
	struct ieee80211com *ic = ni->ni_ic;
	struct wsim_softc *sc = ic->ic_ifp->if_softc;
	int i, k = -1;

	if (ni->ni_rates.rs_nrates > 0)
		ni->ni_txrate = ni->ni_rates.rs_nrates - 1;
	if (!sc->sc_ap)
		return;
	for (i = 0; i < WSIM_JOINS; i++) {
		if (IEEE80211_ADDR_EQ(wsim_joins[i].mac, ni->ni_macaddr) || (k < 0 && wsim_joins[i].gen == 0))
			k = i;
		if (IEEE80211_ADDR_EQ(wsim_joins[i].mac, ni->ni_macaddr))
			break;
	}
	if (k < 0)
		k = 0;
	memcpy(wsim_joins[k].mac, ni->ni_macaddr, ETHER_ADDR_LEN);
	wsim_joins[k].gen = ++wsim_join_gen;
}

/* What the access point's net80211 takes out for its stack: to the simulated access point */
static void
wsim_ap_input(struct ifnet *ifp, struct mbuf *m)
{
	uint8_t frame[WSIM_ETHER];
	int len = m->m_pkthdr.len;

	if (len >= (int)sizeof(struct ether_header) && len <= WSIM_ETHER) {
		m_copydata(m, 0, len, frame);
		rumpcomp_tswifisim_ap_input(frame, (size_t)len);
	}
	m_freem(m);
}

static struct wsim_softc *
wsim_attach(const char *name, int unit, int ap, const uint8_t *mac)
{
	struct wsim_softc *sc;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int i;

	sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
	ic = &sc->sc_ic;
	ifp = &sc->sc_ec.ec_if;
	sc->sc_ap = ap;
	ic->ic_ifp = ifp;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = ap ? IEEE80211_M_HOSTAP : IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_WEP | IEEE80211_C_WPA | IEEE80211_C_SHSLOT
	    | IEEE80211_C_SHPREAMBLE | (ap ? IEEE80211_C_HOSTAP : 0);
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 1; i <= WSIM_CHANS; i++) {
		ic->ic_channels[i].ic_freq = ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM
		    | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}
	ic->ic_ibss_chan = &ic->ic_channels[WSIM_AP_CHAN];
	memcpy(ic->ic_myaddr, mac, ETHER_ADDR_LEN);

	if_initname(ifp, name, unit);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = wsim_init;
	ifp->if_stop = wsim_stop;
	ifp->if_ioctl = wsim_ioctl;
	ifp->if_start = wsim_start;
	IFQ_SET_READY(&ifp->if_snd);

	if_initialize(ifp);
	ieee80211_ifattach(ic);
	ifp->if_percpuq = if_percpuq_create(ifp);
	if_register(ifp);
	ic->ic_newassoc = wsim_newassoc;
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = wsim_newstate;
	mutex_init(&sc->sc_media_mtx, MUTEX_DEFAULT, IPL_SOFTNET);
	ieee80211_media_init_with_lock(ic, ieee80211_media_change, ieee80211_media_status,
	    &sc->sc_media_mtx);
	if (ap) {
		ic->ic_des_chan = &ic->ic_channels[WSIM_AP_CHAN];
		ic->ic_curchan = ic->ic_des_chan;
		ifp->_if_input = wsim_ap_input;
		/*
		 * The interface's own link-level address is not the access
		 * point's (its BSSID, ic_myaddr, which net80211 sends with):
		 * the stack of this kernel takes an ARP packet from an address
		 * one of its interfaces has as its own and drops it
		 * (in_arpinput, if_get_bylla), and the station's stack is in
		 * the same kernel
		 */
		if_set_sadl(ifp, wsim_ap_ifmac, ETHER_ADDR_LEN, false);
	}
	return sc;
}

RUMP_COMPONENT(RUMP_COMPONENT_NET_IF)
{
	mutex_init(&wsim_mtx, MUTEX_DEFAULT, IPL_NET);
	cv_init(&wsim_cv, "wsimair");
	wsim_sta = wsim_attach(WSIM_STA_NAME, 0, 0, wsim_sta_mac);
	wsim_ap = wsim_attach("wsimap", 0, 1, wsim_ap_mac);
	wsim_sta->sc_peer = wsim_ap;
	wsim_ap->sc_peer = wsim_sta;
	if (kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, wsim_air, NULL, &wsim_lwp, "wsimair") != 0)
		panic("wifisim: the air's thread could not be made");
	aprint_normal("wifisim: %s0 (station) and " WSIM_AP_IF " (access point) on the simulated air\n",
	    WSIM_STA_NAME);
}

/*
 * What tswifi_if.c asks of bwfm's chip to leave a network: the
 * simulated station's leave is net80211's own (the disassociation of
 * its move to INIT)
 */
void	tswifi_bwfm_leave(const uint8_t *, int);

void
tswifi_bwfm_leave(const uint8_t *bssid, int reason)
{
}

/* ---------------------------------------------------------------- the access point's entry points */

static int
wsim_ap_set(int type, int val, const void *data, int len)
{
	struct ieee80211req ireq;
	struct socket *so;
	int error;

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, WSIM_AP_IF, sizeof(ireq.i_name));
	ireq.i_type = type;
	ireq.i_val = val;
	ireq.i_data = __UNCONST(data);
	ireq.i_len = len;
	error = socreate(AF_INET, &so, SOCK_DGRAM, 0, curlwp, NULL);
	if (error)
		return error;
	error = ifioctl(so, SIOCS80211, &ireq, curlwp);
	soclose(so);
	if (error != 0 && error != ENETRESET)
		printf("wifisim: the access point refused request %d value %d: error %d\n", type, val, error);
	return error == ENETRESET ? 0 : error;
}

int	rump_tswifisim_ap_config(const uint8_t *, int, int, const uint8_t *);
int	rump_tswifisim_ap_send(const void *, int);
int	rump_tswifisim_ap_setkey(int, const uint8_t *, int, const uint8_t *, int);
int	rump_tswifisim_ap_port(const uint8_t *, int);
int	rump_tswifisim_ap_kick(const uint8_t *, int);
int	rump_tswifisim_ap_mac(uint8_t *);
void	rump_tswifisim_air(int, int);

/* One station of the access point's, as rump_tswifisim_ap_stations tells it */
struct tswifisim_sta {
	uint8_t		mac[6];
	uint8_t		authorized;
	uint8_t		ie_len;
	uint32_t	gen;		/* its join, numbered */
	uint8_t		ie[64];		/* its RSN element of the association request */
};
int	rump_tswifisim_ap_stations(struct tswifisim_sta *, int);
int	rump_tswifisim_stats(uint32_t *, int);

/*
 * The access point's network: its SSID, and wpa 0 (open) or 2 (WPA2-
 * Personal, CCMP, the frames without encryption dropped), as hostapd
 * sets net80211 up for it; its radio (re)started on channel 6. The PSK
 * is hostapd's authenticator's here (wsap_glue.c), not net80211's: psk
 * is not used. 0 or an errno value.
 */
int
rump_tswifisim_ap_config(const uint8_t *ssid, int ssid_len, int wpa, const uint8_t *psk)
{
	struct wsim_softc *sc = wsim_ap;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int error = 0, s;

	if (sc == NULL)
		return ENXIO;
	if (ssid_len <= 0 || ssid_len > IEEE80211_NWID_LEN)
		return EINVAL;
	ic = &sc->sc_ic;
	ifp = &sc->sc_ec.ec_if;
	rump_schedule();
	KERNEL_LOCK(1, NULL);
	s = splnet();
	if (ifp->if_flags & IFF_RUNNING)
		wsim_stop(ifp, 0);
	ifp->if_flags &= ~IFF_UP;
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	memcpy(ic->ic_des_essid, ssid, ssid_len);
	ic->ic_des_esslen = ssid_len;
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	if (wpa) {
		error = wsim_ap_set(IEEE80211_IOC_WPA, 2, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_MCASTCIPHER, IEEE80211_CIPHER_AES_CCM, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_UCASTCIPHERS, 1 << IEEE80211_CIPHER_AES_CCM, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_KEYMGTALGS, WPA_ASE_8021X_PSK, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_RSNCAPS, 0, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_AUTHMODE, IEEE80211_AUTH_WPA, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_DROPUNENCRYPTED, 1, NULL, 0);
	} else {
		error = wsim_ap_set(IEEE80211_IOC_AUTHMODE, IEEE80211_AUTH_OPEN, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_WPA, 0, NULL, 0);
		if (error == 0)
			error = wsim_ap_set(IEEE80211_IOC_DROPUNENCRYPTED, 0, NULL, 0);
	}
	KERNEL_LOCK(1, NULL);
	s = splnet();
	if (error == 0) {
		ifp->if_flags |= IFF_UP;
		error = wsim_init(ifp);
	}
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	printf("wifisim: the access point's network %s, channel %d (%d)\n",
	    wpa ? "WPA2-Personal" : "open", WSIM_AP_CHAN, error);
	rump_unschedule();
	return error;
}

/* The access point's address */
int
rump_tswifisim_ap_mac(uint8_t *mac)
{
	memcpy(mac, wsim_ap_mac, ETHER_ADDR_LEN);
	return 0;
}

/* One Ethernet frame out of the access point; 0 or an errno value */
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
	ifp = &wsim_ap->sc_ec.ec_if;
	rump_schedule();
	m = m_devget(__UNCONST(frame), len, 0, ifp);
	if (m == NULL) {
		rump_unschedule();
		return ENOBUFS;
	}
	m_reset_rcvif(m);
	error = if_transmit_lock(ifp, m);
	rump_unschedule();
	return error;
}

/* A key into the access point's net80211: a station's pairwise key, or the group's (addr all ones) */
int
rump_tswifisim_ap_setkey(int cipher, const uint8_t *addr, int idx, const uint8_t *key, int key_len)
{
	struct ieee80211req_key wk;
	int error;

	if (key_len > (int)sizeof(wk.ik_keydata))
		return EINVAL;
	memset(&wk, 0, sizeof(wk));
	wk.ik_type = cipher;
	wk.ik_flags = IEEE80211_KEY_RECV | IEEE80211_KEY_XMIT;
	memcpy(wk.ik_macaddr, addr, IEEE80211_ADDR_LEN);
	if (ETHER_IS_MULTICAST(addr)) {
		wk.ik_flags |= IEEE80211_KEY_GROUP | IEEE80211_KEY_DEFAULT;
		wk.ik_keyix = idx;
	} else {
		wk.ik_keyix = IEEE80211_KEYIX_NONE;
	}
	wk.ik_keylen = key_len;
	memcpy(wk.ik_keydata, key, key_len);
	rump_schedule();
	error = wsim_ap_set(IEEE80211_IOC_WPAKEY, 0, &wk, sizeof(wk));
	rump_unschedule();
	return error;
}

/* A station's port opened (the 4-way handshake done) or closed */
int
rump_tswifisim_ap_port(const uint8_t *addr, int on)
{
	struct ieee80211req_mlme mlme;
	int error;

	memset(&mlme, 0, sizeof(mlme));
	mlme.im_op = on ? IEEE80211_MLME_AUTHORIZE : IEEE80211_MLME_UNAUTHORIZE;
	memcpy(mlme.im_macaddr, addr, IEEE80211_ADDR_LEN);
	rump_schedule();
	error = wsim_ap_set(IEEE80211_IOC_MLME, 0, &mlme, sizeof(mlme));
	rump_unschedule();
	return error;
}

/* A station sent away (a deauthentication with the reason) */
int
rump_tswifisim_ap_kick(const uint8_t *addr, int reason)
{
	struct ieee80211req_mlme mlme;
	int error;

	memset(&mlme, 0, sizeof(mlme));
	mlme.im_op = IEEE80211_MLME_DEAUTH;
	mlme.im_reason = reason;
	memcpy(mlme.im_macaddr, addr, IEEE80211_ADDR_LEN);
	rump_schedule();
	error = wsim_ap_set(IEEE80211_IOC_MLME, 0, &mlme, sizeof(mlme));
	rump_unschedule();
	return error;
}

/*
 * The stations associated with the access point, up to max into list:
 * how many there are, or a negative errno value
 */
int
rump_tswifisim_ap_stations(struct tswifisim_sta *list, int max)
{
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	struct tswifisim_sta *t;
	int n = 0, s, i;

	if (wsim_ap == NULL)
		return -ENXIO;
	ic = &wsim_ap->sc_ic;
	rump_schedule();
	KERNEL_LOCK(1, NULL);
	s = splnet();
	TAILQ_FOREACH(ni, &ic->ic_sta.nt_node, ni_list) {
		if (ni->ni_associd == 0 || ni == ic->ic_bss)
			continue;
		if (n < max) {
			t = &list[n];
			memset(t, 0, sizeof(*t));
			memcpy(t->mac, ni->ni_macaddr, 6);
			t->authorized = ieee80211_node_is_authorized(ni) ? 1 : 0;
			if (ni->ni_wpa_ie != NULL && 2 + ni->ni_wpa_ie[1] <= (int)sizeof(t->ie)) {
				t->ie_len = 2 + ni->ni_wpa_ie[1];
				memcpy(t->ie, ni->ni_wpa_ie, t->ie_len);
			}
			for (i = 0; i < WSIM_JOINS; i++) {
				if (IEEE80211_ADDR_EQ(wsim_joins[i].mac, ni->ni_macaddr))
					t->gen = wsim_joins[i].gen;
			}
		}
		n++;
	}
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	rump_unschedule();
	return n;
}

/*
 * The air: on 0/1 takes the access point off the air or puts it back
 * (off: its network started again with no station), rssi the signal's
 * size in dBm (-1 left as it is)
 */
void
rump_tswifisim_air(int on, int rssi)
{
	struct ifnet *ifp;
	int s;

	if (wsim_ap == NULL)
		return;
	if (rssi >= 0)
		wsim_rssi = rssi;
	if (on < 0 || (on != 0) == (wsim_ap_on != 0))
		return;
	ifp = &wsim_ap->sc_ec.ec_if;
	rump_schedule();
	KERNEL_LOCK(1, NULL);
	s = splnet();
	if (!on) {
		wsim_ap_on = 0;
		if (ifp->if_flags & IFF_RUNNING) {
			wsim_stop(ifp, 0);
			(void)wsim_init(ifp);
		}
	} else {
		wsim_ap_on = 1;
	}
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	printf("wifisim: the access point %s the air\n", on ? "back on" : "off");
	rump_unschedule();
}

/*
 * The air's counts: v[0..2] frames the station sent, received, dropped
 * (queue or size); v[3..5] the same of the access point; v[6] beacons;
 * v[7] frames lost with the access point off the air or a radio not
 * running, v[8] on another channel; v[9] data frames the station's
 * net80211 dropped for its port not being open (is_rx_unauth), v[10]
 * the access point's; v[11] the station's frames dropped unencrypted,
 * v[12] the station's decryption failures (CCMP MIC), v[13] the joins,
 * v[14] (a background scan's losses on OpenBSD's layer) 0
 */
int
rump_tswifisim_stats(uint32_t *v, int n)
{
	uint32_t a[15];
	int i;

	memset(a, 0, sizeof(a));
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
		a[12] = wsim_sta->sc_ic.ic_stats.is_rx_ccmpmic;
		a[13] = wsim_join_gen;
	}
	for (i = 0; i < n && i < 15; i++)
		v[i] = a[i];
	return i;
}
