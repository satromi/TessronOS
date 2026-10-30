/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_net.c (x64 PC, WIFI_NET80211=obsd)
 *	The network interface around iwm0 (design 9.6.9): what OpenBSD's
 *	if.c, ifq.c and if_ethersubr.c give a driver and the IEEE 802.11
 *	layer. Compiled with OpenBSD's kernel headers.
 *
 *	iwm0 is in no network stack. Its transmit queue holds the Ethernet
 *	frames of neta0 (tswifi_obsd.c) and of the handshake, and the frames
 *	the layer takes out of what the card receives go to TessronOS
 *	(rumpcomp_tswifi_input, the Wi-Fi manager's knl_wifi_rx), which
 *	gives them to neta0 or to the handshake. The watchdog of each
 *	interface (if_timer, if_watchdog) runs once a second, as OpenBSD's
 *	if_slowtimo runs it.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/timeout.h>
#include <sys/task.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include "obsd_glue.h"

#define TSW_FRAME	1514
#define TSW_SNDQ	256		/* frames the transmit queue holds */

struct ifnet_head	ifnetlist = TAILQ_HEAD_INITIALIZER(ifnetlist);

u_int8_t etherbroadcastaddr[ETHER_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
u_int8_t etheranyaddr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

/* ---------------------------------------------------------------- the transmit queue */

int
ifq_enqueue(struct ifqueue *ifq, struct mbuf *m)
{
	struct mbuf_list *ml = ifq->ifq_q;

	if (ml == NULL || ifq->ifq_len >= ifq->ifq_maxlen) {
		ifq->ifq_qdrops++;
		m_freem(m);
		return ENOBUFS;
	}
	ml_enqueue(ml, m);
	ifq->ifq_len++;
	ifq->ifq_packets++;
	ifq->ifq_bytes += m->m_pkthdr.len;
	return 0;
}

struct mbuf *
ifq_dequeue(struct ifqueue *ifq)
{
	struct mbuf_list *ml = ifq->ifq_q;
	struct mbuf *m;

	if (ml == NULL)
		return NULL;
	m = ml_dequeue(ml);
	if (m != NULL)
		ifq->ifq_len--;
	return m;
}

unsigned int
ifq_purge(struct ifqueue *ifq)
{
	struct mbuf_list *ml = ifq->ifq_q;
	unsigned int n = 0;

	if (ml != NULL) {
		n = ml_purge(ml);
		ifq->ifq_len = 0;
	}
	return n;
}

void
ifq_set_oactive(struct ifqueue *ifq)
{
	if (!ifq->ifq_oactive)
		ifq->ifq_oactives++;
	ifq->ifq_oactive = 1;
}

/* ---------------------------------------------------------------- the interface */

void
if_attach(struct ifnet *ifp)
{
	struct mbuf_list *ml;

	ml = malloc(sizeof(*ml), M_DEVBUF, M_WAITOK | M_ZERO);
	ml_init(ml);
	ifp->if_snd.ifq_if = ifp;
	ifp->if_snd.ifq_q = ml;
	ifp->if_snd.ifq_maxlen = TSW_SNDQ;
	ifp->if_snd.ifq_len = 0;
	ifp->if_snd.ifq_oactive = 0;
	ifp->if_link_state = LINK_STATE_DOWN;
	TAILQ_INSERT_TAIL(&ifnetlist, ifp, if_list);
}

void
if_deactivate(struct ifnet *ifp)
{
}

int
if_addgroup(struct ifnet *ifp, const char *group)
{
	return 0;
}

/* What the driver's start runs: at once, holding the lock, as for a driver that is not MP-safe */
void
if_start(struct ifnet *ifp)
{
	if (ifp->if_start != NULL)
		(*ifp->if_start)(ifp);
}

int
if_enqueue(struct ifnet *ifp, struct mbuf *m)
{
	int error;

	error = ifq_enqueue(&ifp->if_snd, m);
	if (error) {
		ifp->if_oerrors++;
		return error;
	}
	if_start(ifp);
	return 0;
}

void	tsw_rx_frame(const uint8_t *, int);		/* tswifi_obsd.c: counted, DHCP's said */

#if TSWIFI_SIM
int	obsd_sim_is_ap(struct ifnet *);			/* device/net/wifi/sim/obsd_sim.c */
void	obsd_sim_ap_input(const uint8_t *, int);
#endif

/*
 * Frames for the stack: flattened and handed to TessronOS, one by one
 * (the Wi-Fi simulator's access point's to the simulated access point)
 */
void
if_input(struct ifnet *ifp, struct mbuf_list *ml)
{
	static uint8_t frame[TSW_FRAME];
	struct mbuf *m;
	int len;

	while ((m = ml_dequeue(ml)) != NULL) {
		len = m->m_pkthdr.len;
		if (len >= (int)sizeof(struct ether_header) && len <= TSW_FRAME) {
			m_copydata(m, 0, len, frame);
			ifp->if_ipackets++;
			ifp->if_ibytes += len;
#if TSWIFI_SIM
			if (obsd_sim_is_ap(ifp))
				obsd_sim_ap_input(frame, len);
			else
#endif
			{
				tsw_rx_frame(frame, len);
				rumpcomp_tswifi_input(frame, (unsigned long long)len);
			}
		} else {
			ifp->if_ierrors++;
		}
		m_freem(m);
	}
}

/* The manager reads the state itself (tswifi_obsd.c): nothing to tell */
void
if_link_state_change(struct ifnet *ifp)
{
}

int
if_setlladdr(struct ifnet *ifp, const uint8_t *lladdr)
{
	memcpy(((struct arpcom *)ifp)->ac_enaddr, lladdr, ETHER_ADDR_LEN);
	return 0;
}

void
rtm_80211info(struct ifnet *ifp, struct if_ieee80211_data *ifie)
{
}

/* ---------------------------------------------------------------- Ethernet */

void
ether_ifattach(struct ifnet *ifp)
{
	ifp->if_type = IFT_ETHER;
	ifp->if_addrlen = ETHER_ADDR_LEN;
	ifp->if_hdrlen = ETHER_HDR_LEN;
	ifp->if_mtu = ETHERMTU;
}

void
ether_ifdetach(struct ifnet *ifp)
{
}

int
ether_ioctl(struct ifnet *ifp, struct arpcom *arp, u_long cmd, caddr_t data)
{
	switch (cmd) {
	case SIOCSIFMTU:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return ENOTTY;
	}
}

int
ether_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
    struct rtentry *rt)
{
	m_freem(m);
	return EOPNOTSUPP;
}

/* Every multicast frame is taken (the stack on neta0 sorts them) */
int
ether_addmulti(struct ifreq *ifr, struct arpcom *ac)
{
	return 0;
}

int
ether_delmulti(struct ifreq *ifr, struct arpcom *ac)
{
	return 0;
}

char *
ether_sprintf(u_char *ap)
{
	static char buf[4][18];
	static int k;
	static const char digits[] = "0123456789abcdef";
	char *cp;
	int i;

	k = (k + 1) % 4;
	cp = buf[k];
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		*cp++ = digits[*ap >> 4];
		*cp++ = digits[*ap++ & 0xf];
		*cp++ = ':';
	}
	*--cp = '\0';
	return buf[k];
}

u_int32_t
ether_crc32_le_update(u_int32_t crc, const u_int8_t *buf, size_t len)
{
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
	}
	return crc;
}

/* ---------------------------------------------------------------- the watchdog */

static struct timeout	slowtimo;

static void
if_slowtimo(void *arg)
{
	struct ifnet *ifp;

	TAILQ_FOREACH(ifp, &ifnetlist, if_list) {
		if (ifp->if_timer > 0 && --ifp->if_timer == 0 &&
		    ifp->if_watchdog != NULL)
			(*ifp->if_watchdog)(ifp);
	}
	timeout_add_sec(&slowtimo, 1);
}

void
obsd_net_init(void)
{
	timeout_set(&slowtimo, if_slowtimo, NULL);
	timeout_add_sec(&slowtimo, 1);
}
