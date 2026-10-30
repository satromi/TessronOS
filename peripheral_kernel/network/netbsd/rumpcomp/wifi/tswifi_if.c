/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tswifi_if.c
 *	The Wi-Fi interface bwfm0 between net80211 and TessronOS (design
 *	9.6, 9.6.8).
 *
 *	This file is compiled with the NetBSD kernel headers into the rump
 *	kernel when the Raspberry Pi 5's build has the Wi-Fi
 *	(tools/netbsd/build.sh, WIFI=1), and when the Arm build has the
 *	Wi-Fi simulator (WIFI_SIM=1, design 9.6.12), whose station has the
 *	chip's name. Its entry points (rump_tswifi_*) are what the Wi-Fi
 *	manager (device/net/wifi/wifi.c) drives the card with; each enters
 *	the rump kernel as a thread of it for the time it runs. (The x64
 *	PC's Wi-Fi is OpenBSD's layer, device/net/wifi/obsd, design 9.6.9.)
 *
 *	The card is driven as a station through net80211's ioctls: roaming is left to
 *	the manager (IEEE80211_ROAMING_MANUAL), a scan is begun as net80211
 *	begins one (ieee80211_begin_scan) and read from its table,
 *	the WPA information element goes into the association request with
 *	IEEE80211_IOC_OPTIE, a network is joined with IEEE80211_IOC_MLME,
 *	and the keys the 4-way handshake gives are set with
 *	IEEE80211_IOC_WPAKEY: all through the interface ioctls SIOCS80211
 *	and SIOCG80211 on a socket of the kernel's.
 *
 *	bwfm0 carries no address of its own. The Ethernet frames net80211
 *	takes out of what the card receives are not given to the stack on
 *	bwfm0: they go to TessronOS (rumpcomp_tswifi_input), which hands
 *	them, as the card the device "neta" has, to the stack's interface
 *	neta0, or the EAPOL ones to the handshake. Frames from neta0 come
 *	back in through rump_tswifi_send and leave on bwfm0.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/lwp.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>

#include <rump-sys/kern.h>

/* The interface: Intel's card on the x64 PC, Broadcom's chip on the Raspberry Pi 5 */
#define TSWIFI_IF	"bwfm0"
#define TSWIFI_FRAME	1514

/* The hypercalls (wifi_user.c) */
void	rumpcomp_tswifi_input(const void *, size_t);
void	rumpcomp_tswifi_say(const char *);

/* What the manager reads of the card (wifi.c has the same layout) */
struct tswifi_status {
	int32_t		state;		/* IEEE80211_S_*: 0 init, 1 scan, 2 auth, 3 assoc, 4 run */
	int32_t		scanning;	/* a scan is going on */
	int32_t		freq;		/* MHz of the channel */
	int32_t		rssi;		/* as the driver gives it (bwfm: dBm, a signed byte) */
	int32_t		rate;		/* the rate sent at, in 500 kb/s */
	int32_t		ifflags;	/* 1 the interface is up, 2 the card runs */
	uint8_t		bssid[6];
	uint8_t		ssid_len;
	uint8_t		ssid[32];
	uint8_t		pad;
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
	uint8_t		ie[256];	/* its information elements (RSN, WPA) */
};

int	rump_tswifi_start(uint8_t *);
int	rump_tswifi_param(int, int);
int	rump_tswifi_optie(const uint8_t *, int);
int	rump_tswifi_scan(void);
int	rump_tswifi_results(struct tswifi_bss *, int);
int	rump_tswifi_assoc(const uint8_t *, const uint8_t *, int);
int	rump_tswifi_deauth(const uint8_t *, int);
int	rump_tswifi_setkey(int, const uint8_t *, int, int, const uint8_t *, int,
	    const uint8_t *, int);
int	rump_tswifi_delkey(const uint8_t *, int);
int	rump_tswifi_status(struct tswifi_status *);
int	rump_tswifi_send(const void *, int);
void	rump_tswifi_debug(int, uint32_t);

static struct ifnet	*tswifi_ifp;
static struct ieee80211com *tswifi_ic;

/* The frames through here, for the manager's records (rump_tswifi_stats) */
static volatile uint32_t tswifi_in, tswifi_in_bad, tswifi_out, tswifi_out_err;

/* ---------------------------------------------------------------- frames */

/*
 * What net80211 gives up on bwfm0, instead of the stack: one Ethernet
 * frame, flattened and handed over. Called from the interface's soft
 * interrupt, so the hypercall does not wait.
 */
static void
tswifi_input(struct ifnet *ifp, struct mbuf *m)
{
	uint8_t frame[TSWIFI_FRAME];
	int len;

	len = m->m_pkthdr.len;
	if (len >= (int)sizeof(struct ether_header) && len <= TSWIFI_FRAME) {
		m_copydata(m, 0, len, frame);
		if_statadd2(ifp, if_ipackets, 1, if_ibytes, len);
		tswifi_in++;
		rumpcomp_tswifi_input(frame, (size_t)len);
	} else {
		if_statinc(ifp, if_ierrors);
		tswifi_in_bad++;
	}
	m_freem(m);
}

/* One Ethernet frame out on bwfm0; 0 or an errno value */
int
rump_tswifi_send(const void *frame, int len)
{
	struct ifnet *ifp = tswifi_ifp;
	struct mbuf *m;
	int error;

	if (ifp == NULL)
		return ENXIO;
	if (len < (int)sizeof(struct ether_header) || len > TSWIFI_FRAME)
		return EINVAL;
	rump_schedule();
	m = m_devget(__UNCONST(frame), len, 0, ifp);
	if (m == NULL) {
		rump_unschedule();
		return ENOBUFS;
	}
	/*
	 * Sent, not received: no interface it came in on. m_set_rcvif takes
	 * an interface and reads its index (NULL would be read through);
	 * m_reset_rcvif is the clearing
	 */
	m_reset_rcvif(m);
	error = if_transmit_lock(ifp, m);
	rump_unschedule();
	if (error == 0)
		tswifi_out++;
	else
		tswifi_out_err++;
	return error;
}

/*
 * What the frames came to, for the records: v[0] frames given to the
 * manager, v[1] those too long or short, v[2] frames handed to bwfm0 to
 * send, v[3] those it refused; then net80211's: v[4] frames it encrypted
 * or decrypted with CCMP, v[5] CCMP MIC failures, v[6] CCMP replays,
 * v[7] CCMP frames of a bad form, v[8] frames of a key index it has no
 * key for, v[9] unencrypted frames dropped, v[10] decryption failures,
 * v[11] decapsulation failures, v[12] frames sent with no key, v[13]
 * frames of another BSS, v[14] duplicates, v[15] frames while not
 * associated, v[16] data frames dropped for the port of the access
 * point's node not being open (is_rx_unauth), v[17] 1 when that port is
 * open. As many as n; the count filled.
 */
int	rump_tswifi_stats(uint32_t *, int);

int
rump_tswifi_stats(uint32_t *v, int n)
{
	struct ieee80211com *ic = tswifi_ic;
	uint32_t a[18];
	int i;

	memset(a, 0, sizeof(a));
	a[0] = tswifi_in;
	a[1] = tswifi_in_bad;
	a[2] = tswifi_out;
	a[3] = tswifi_out_err;
	if (ic != NULL) {
		a[4] = ic->ic_stats.is_crypto_ccmp;
		a[5] = ic->ic_stats.is_rx_ccmpmic;
		a[6] = ic->ic_stats.is_rx_ccmpreplay;
		a[7] = ic->ic_stats.is_rx_ccmpformat;
		a[8] = ic->ic_stats.is_rx_badkeyid;
		a[9] = ic->ic_stats.is_rx_unencrypted;
		a[10] = ic->ic_stats.is_rx_wepfail;
		a[11] = ic->ic_stats.is_rx_decap;
		a[12] = ic->ic_stats.is_tx_nodefkey;
		a[13] = ic->ic_stats.is_rx_wrongbss;
		a[14] = ic->ic_stats.is_rx_dup;
		a[15] = ic->ic_stats.is_rx_notassoc;
		a[16] = ic->ic_stats.is_rx_unauth;
		a[17] = (ic->ic_bss != NULL && ieee80211_node_is_authorized(ic->ic_bss)) ? 1 : 0;
	}
	for (i = 0; i < n && i < 18; i++)
		v[i] = a[i];
	return i;
}

/* ---------------------------------------------------------------- the ioctls */

static int
tswifi_ioctl(u_long cmd, void *data)
{
	struct socket *so;
	int error;

	error = socreate(AF_INET, &so, SOCK_DGRAM, 0, curlwp, NULL);
	if (error)
		return error;
	error = ifioctl(so, cmd, data, curlwp);
	soclose(so);
	return error;
}

static int
tswifi_set(int type, int val, const void *data, int len)
{
	struct ieee80211req ireq;
	int error;

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, TSWIFI_IF, sizeof(ireq.i_name));
	ireq.i_type = type;
	ireq.i_val = val;
	ireq.i_data = __UNCONST(data);
	ireq.i_len = len;
	error = tswifi_ioctl(SIOCS80211, &ireq);
	if (error != 0) {
		/* which request net80211 or the driver refused, for the record */
		printf("tswifi: SIOCS80211 type %d value %d length %d: error %d\n", type, val, len, error);
	}
	return error;
}

/*
 * The configuration a driver left until the root file system was
 * there run, and the interface taken over: the frames bwfm0 takes are
 * turned to TessronOS, and the card left to the manager's roaming. 0 and
 * its address, or an errno value (ENXIO: no interface yet, a chip the
 * SD/SDIO layer finds later, rump_tswifi_take).
 */
int
rump_tswifi_start(uint8_t *mac)
{
	struct ifnet *ifp;
	int error, s;

	rump_schedule();
	config_create_mountrootthreads();
	config_finalize_mountroot();

	ifp = ifunit(TSWIFI_IF);
	if (ifp == NULL) {
		rump_unschedule();
		return ENXIO;
	}
	s = splnet();
	ifp->_if_input = tswifi_input;
	splx(s);
	memcpy(mac, CLLADDR(ifp->if_sadl), ETHER_ADDR_LEN);
	tswifi_ic = ieee80211_find_vap(mac);
	tswifi_ifp = ifp;
	error = tswifi_set(IEEE80211_IOC_ROAMING, IEEE80211_ROAMING_MANUAL, NULL, 0);
	rump_unschedule();
	return error;
}

/*
 * The interface taken over as rump_tswifi_start does, for a chip the
 * SD/SDIO layer found only after that: the configuration that waited for
 * the root file system has run by then, and a driver that attaches later
 * runs its own at once. ENXIO while there is no interface yet.
 */
int	rump_tswifi_take(uint8_t *);

int
rump_tswifi_take(uint8_t *mac)
{
	struct ifnet *ifp;
	int error, s;

	rump_schedule();
	ifp = ifunit(TSWIFI_IF);
	if (ifp == NULL) {
		rump_unschedule();
		return ENXIO;
	}
	s = splnet();
	ifp->_if_input = tswifi_input;
	splx(s);
	memcpy(mac, CLLADDR(ifp->if_sadl), ETHER_ADDR_LEN);
	tswifi_ic = ieee80211_find_vap(mac);
	tswifi_ifp = ifp;
	error = tswifi_set(IEEE80211_IOC_ROAMING, IEEE80211_ROAMING_MANUAL, NULL, 0);
	rump_unschedule();
	return error;
}

/*
 * The security of the network to join: wpa 0 none, 2 WPA2 (RSN); and
 * whether frames without encryption are dropped. Only what differs from
 * what the card has is set: net80211 answers a change of the
 * authentication or of WPA by starting the card again (ENETRESET). 1
 * when that happened, 0 when nothing had to change, or an errno value.
 *
 * A station's 802.11 authentication is open in every case, WPA2 too;
 * WPA2 is IEEE80211_IOC_WPA and the privacy bit, as wpa_supplicant's
 * layer for the BSDs sets a station up (driver_bsd.c,
 * wpa_driver_bsd_associate and wpa_driver_bsd_set_auth_alg). The
 * authentication mode IEEE80211_AUTH_WPA is an access point's (hostapd's
 * bsd_set_ieee8021x): net80211 keeps it as IEEE80211_AUTH_8021X, and a
 * station joined with that does not have the port of its access point's
 * node opened when it comes to RUN (ieee80211_newstate), nor is there a
 * call to open it on a station (IEEE80211_MLME_AUTHORIZE is an access
 * point's). Every data frame from the access point but EAPOL's was then
 * dropped as from a port not open (is_rx_unauth): the 4-way handshake
 * ended and DHCP had no answer. Open, the port is opened at RUN; the
 * frames without encryption are still dropped (drop) until the keys are
 * in. A change of the authentication mode clears the WPA and privacy
 * flags in net80211, so these are read again after it.
 */
int
rump_tswifi_param(int wpa, int drop)
{
	struct ieee80211com *ic = tswifi_ic;
	int error = 0, reset = 0, auth, flags, s;

	if (ic == NULL)
		return ENXIO;
	rump_schedule();
	s = splnet();
	auth = ic->ic_bss != NULL ? ic->ic_bss->ni_authmode : -1;
	flags = ic->ic_flags;
	splx(s);
	if (auth != IEEE80211_AUTH_OPEN) {
		error = tswifi_set(IEEE80211_IOC_AUTHMODE, IEEE80211_AUTH_OPEN, NULL, 0);
		reset = 1;
		s = splnet();
		flags = ic->ic_flags;
		splx(s);
	}
	if (error == 0 && ((flags & IEEE80211_F_PRIVACY) != 0) != (wpa != 0))
		error = tswifi_set(IEEE80211_IOC_PRIVACY, wpa ? 1 : 0, NULL, 0);
	if (error == 0 && (flags & IEEE80211_F_WPA)
	    != (wpa == 2 ? IEEE80211_F_WPA2 : 0)) {
		error = tswifi_set(IEEE80211_IOC_WPA, wpa, NULL, 0);
		reset = 1;
	}
	if (error == 0 && ((flags & IEEE80211_F_DROPUNENC) != 0) != (drop != 0))
		error = tswifi_set(IEEE80211_IOC_DROPUNENCRYPTED, drop, NULL, 0);
	rump_unschedule();
	return error != 0 ? error : reset;
}

/* The WPA or RSN information element sent with the association request */
int
rump_tswifi_optie(const uint8_t *ie, int len)
{
	int error;

	rump_schedule();
	error = tswifi_set(IEEE80211_IOC_OPTIE, 0, len > 0 ? ie : NULL, len);
	rump_unschedule();
	return error;
}

/*
 * A scan of every channel the card may use, begun as net80211 begins
 * one (ieee80211_begin_scan): the networks of the last scan are
 * forgotten, and IEEE80211_F_SCAN stays set until the card says the
 * scan is over (rump_tswifi_status). Not while a network is joined.
 *
 * A scan is begun only from INIT. With the roaming left to the manager,
 * net80211 stays in SCAN once a scan is over, and stays in AUTH or
 * ASSOC when the access point sent the station away (a deauthentication
 * or a disassociation): a scan asked for there would go from SCAN to
 * SCAN, which bwfm takes as no new scan, or would not be begun. The
 * card is put back to INIT first, and the caller asks again (EAGAIN)
 * once it is there; the drivers make that change in a task of their
 * own. A scan already going on is taken as the one asked for: 0.
 */
int
rump_tswifi_scan(void)
{
	struct ieee80211com *ic = tswifi_ic;
	int error = 0, s;

	if (ic == NULL || tswifi_ifp == NULL)
		return ENXIO;
	rump_schedule();
	KERNEL_LOCK(1, NULL);
	s = splnet();
	if ((tswifi_ifp->if_flags & IFF_RUNNING) == 0)
		error = ENETDOWN;
	else if (ic->ic_flags & IEEE80211_F_SCAN)
		error = 0;
	else if (ic->ic_state == IEEE80211_S_RUN)
		error = EBUSY;
	else if (ic->ic_state != IEEE80211_S_INIT) {
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
		error = EAGAIN;
	} else
		ieee80211_begin_scan(ic, 1);
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	rump_unschedule();
	return error;
}

/*
 * What the last scan found, up to max networks into list: how many there
 * are, those that did not fit counted too, or a negative errno value.
 * The scan's table of nodes is walked where it is, so no buffer (the
 * scan results ioctl's length is 16 bits) bounds how many are told; a
 * network's information elements are its WPA or RSN one and its WME
 * one.
 */
int
rump_tswifi_results(struct tswifi_bss *list, int max)
{
	struct ieee80211com *ic = tswifi_ic;
	struct ieee80211_node *ni;
	struct tswifi_bss *b;
	int n = 0, s, ielen;

	if (ic == NULL)
		return -ENXIO;
	rump_schedule();
	KERNEL_LOCK(1, NULL);
	s = splnet();
	TAILQ_FOREACH(ni, &ic->ic_scan.nt_node, ni_list) {
		/* NB: skip pre-scan node state */
		if (ni->ni_chan == IEEE80211_CHAN_ANYC)
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
		b->rssi = ic->ic_node_getrssi(ni);
		ielen = 0;
		if (ni->ni_wpa_ie != NULL)
			ielen += 2 + ni->ni_wpa_ie[1];
		if (ni->ni_wme_ie != NULL)
			ielen += 2 + ni->ni_wme_ie[1];
		if (ielen <= 255 && ielen <= (int)sizeof(b->ie)) {
			if (ni->ni_wpa_ie != NULL) {
				memcpy(b->ie, ni->ni_wpa_ie, 2 + ni->ni_wpa_ie[1]);
				b->ie_len = 2 + ni->ni_wpa_ie[1];
			}
			if (ni->ni_wme_ie != NULL) {
				memcpy(b->ie + b->ie_len, ni->ni_wme_ie, 2 + ni->ni_wme_ie[1]);
				b->ie_len += 2 + ni->ni_wme_ie[1];
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
 * Join the network bssid, found in the last scan by its BSSID and its SSID
 * (which tells apart those of one access point), with the MLME request
 * carrying the SSID and the BSSID, and nothing else. net80211 finds the node by the two it is
 * given (ieee80211_find_node_with_ssid) and joins it; the desired SSID
 * of SIOCS80211NWID is for net80211's own choosing after a scan, which
 * the manual roaming set at the start leaves to the manager. Setting it
 * would answer ENETRESET: the driver would stop and start the card
 * again, emptying the table of the scan, and the request would find
 * nothing (EINVAL).
 */
int
rump_tswifi_assoc(const uint8_t *bssid, const uint8_t *ssid, int ssid_len)
{
	struct ieee80211req_mlme mlme;
	int error;

	if (tswifi_ic == NULL)
		return ENXIO;
	if (ssid_len < 0 || ssid_len > IEEE80211_NWID_LEN)
		return EINVAL;
	rump_schedule();
	memset(&mlme, 0, sizeof(mlme));
	mlme.im_op = IEEE80211_MLME_ASSOC;
	memcpy(mlme.im_ssid, ssid, ssid_len);
	mlme.im_ssid_len = ssid_len;
	memcpy(mlme.im_macaddr, bssid, IEEE80211_ADDR_LEN);
	error = tswifi_set(IEEE80211_IOC_MLME, 0, &mlme, sizeof(mlme));
	rump_unschedule();
	return error;
}

/*
 * Leave the network (or the join under way), the card left running.
 * bwfm's chip keeps its association whatever net80211's state, so it is
 * told to leave (BWFM_C_DISASSOC) and net80211 goes to INIT. Below AUTH
 * there is nothing to leave: INIT, as before. 0 or an errno value.
 */
void	tswifi_bwfm_leave(const uint8_t *, int);	/* tswifi_sdio.c (the simulator: tswifi_sim.c) */

int
rump_tswifi_deauth(const uint8_t *bssid, int reason)
{
	struct ieee80211com *ic = tswifi_ic;
	int s, was;

	if (ic == NULL)
		return ENXIO;
	rump_schedule();
	was = ic->ic_state;
	if (was > IEEE80211_S_SCAN)
		tswifi_bwfm_leave(bssid, reason);	/* asks the chip: may sleep */
	KERNEL_LOCK(1, NULL);
	s = splnet();
	if (ic->ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	splx(s);
	KERNEL_UNLOCK_ONE(NULL);
	/* said while still on the rump kernel's processor: printf needs its CPU (pserialize) */
	printf("tswifi: left the network (reason %d, from state %d)\n", reason, was);
	rump_unschedule();
	return 0;
}

/*
 * A key from the handshake: cipher 1 TKIP, 3 CCMP (IEEE80211_CIPHER_*),
 * for addr (all ones: the group's), its index, whether frames are sent
 * with it, the receive sequence counter as it came (little endian)
 */
int
rump_tswifi_setkey(int cipher, const uint8_t *addr, int idx, int set_tx,
    const uint8_t *seq, int seq_len, const uint8_t *key, int key_len)
{
	struct ieee80211req_key wk;
	int error;

	if (key_len > (int)sizeof(wk.ik_keydata) || seq_len > 8)
		return EINVAL;
	memset(&wk, 0, sizeof(wk));
	wk.ik_type = cipher;
	wk.ik_flags = IEEE80211_KEY_RECV | (set_tx ? IEEE80211_KEY_XMIT : 0);
	memcpy(wk.ik_macaddr, addr, IEEE80211_ADDR_LEN);
	if (ETHER_IS_MULTICAST(addr)) {
		wk.ik_flags |= IEEE80211_KEY_GROUP;
		wk.ik_keyix = idx;
	} else {
		wk.ik_keyix = (idx == 0) ? IEEE80211_KEYIX_NONE : idx;
	}
	if (wk.ik_keyix != IEEE80211_KEYIX_NONE && set_tx)
		wk.ik_flags |= IEEE80211_KEY_DEFAULT;
	wk.ik_keylen = key_len;
	if (seq != NULL && seq_len > 0)
		memcpy(&wk.ik_keyrsc, seq, seq_len);
	memcpy(wk.ik_keydata, key, key_len);
	rump_schedule();
	error = tswifi_set(IEEE80211_IOC_WPAKEY, 0, &wk, sizeof(wk));
	rump_unschedule();
	return error;
}

/* A key taken away: the pairwise one of addr, or the group's of idx (addr NULL) */
int
rump_tswifi_delkey(const uint8_t *addr, int idx)
{
	struct ieee80211req_del_key dk;
	int error;

	memset(&dk, 0, sizeof(dk));
	if (addr == NULL) {
		dk.idk_keyix = idx;
	} else {
		memcpy(dk.idk_macaddr, addr, IEEE80211_ADDR_LEN);
		dk.idk_keyix = (uint8_t)IEEE80211_KEYIX_NONE;
	}
	rump_schedule();
	error = tswifi_set(IEEE80211_IOC_DELKEY, 0, &dk, sizeof(dk));
	rump_unschedule();
	return error;
}

/* Where the card is: its state and the network it is in */
int
rump_tswifi_status(struct tswifi_status *st)
{
	struct ieee80211com *ic = tswifi_ic;
	struct ieee80211_node *ni;
	int s, r;

	memset(st, 0, sizeof(*st));
	if (ic == NULL)
		return ENXIO;
	rump_schedule();
	s = splnet();
	st->state = ic->ic_state;
	st->scanning = (ic->ic_flags & IEEE80211_F_SCAN) != 0;
	if (tswifi_ifp != NULL)
		st->ifflags = ((tswifi_ifp->if_flags & IFF_UP) ? 1 : 0)
		    | ((tswifi_ifp->if_flags & IFF_RUNNING) ? 2 : 0);
	ni = ic->ic_bss;
	if (ni != NULL && ic->ic_state >= IEEE80211_S_AUTH) {
		memcpy(st->bssid, ni->ni_bssid, 6);
		st->ssid_len = MIN(ni->ni_esslen, sizeof(st->ssid));
		memcpy(st->ssid, ni->ni_essid, st->ssid_len);
		if (ni->ni_chan != NULL && ni->ni_chan != IEEE80211_CHAN_ANYC)
			st->freq = ni->ni_chan->ic_freq;
		st->rssi = ic->ic_node_getrssi(ni);
		r = ni->ni_txrate;
		if (r >= 0 && r < ni->ni_rates.rs_nrates)
			st->rate = ni->ni_rates.rs_rates[r] & IEEE80211_RATE_VAL;
	}
	splx(s);
	rump_unschedule();
	return 0;
}

/* How much net80211 says on the console (the first argument, the driver's, is not used) */
void
rump_tswifi_debug(int drv, uint32_t net80211)
{
	if (tswifi_ic != NULL)
		tswifi_ic->ic_debug = net80211;
}
