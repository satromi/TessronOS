/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	if_tsneta.c
 *	The card as an Ethernet interface of the NetBSD stack (design 12.6).
 *
 *	This file is compiled with the NetBSD kernel headers into the rump
 *	kernel (tools/netbsd/build.sh); it reaches the card only through the
 *	hypercalls rumpcomp_neta_* of neta_user.c, which speak to the device
 *	"neta" with tk_rea_dev and tk_wri_dev.
 *
 *	The device carries one whole frame per read or write. A frame out is
 *	the mbuf chain flattened into one buffer and handed over, to be
 *	written by a task outside the rump kernel. Frames in are read by a
 *	kernel thread of this interface, which waits on the card outside
 *	the rump kernel and hands what came to if_input, a burst at a time.
 *
 *	The link is reported as unknown, which the stack takes as up: the
 *	card is used as soon as the interface is, as it always was.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mbuf.h>
#include <sys/socket.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_types.h>

#include <rump-sys/kern.h>
#include <rump-sys/net.h>

#define TSNETA_FRAME_MAX	1514	/* 1500 of payload and the header */
#define TSNETA_FRAME_MIN	60	/* the card pads nothing shorter */
#define TSNETA_RX_BATCH		16	/* frames taken in at once */

/* The hypercalls (neta_user.c) */
int	rumpcomp_neta_open(uint8_t *, int *);
int	rumpcomp_neta_send(const void *, size_t);
int	rumpcomp_neta_recv(void **, int *, int, size_t);

struct tsneta_softc {
	struct ethercom	sc_ec;
	struct lwp	*sc_rx;
};

static struct tsneta_softc	*tsneta_sc;

static int
tsneta_init(struct ifnet *ifp)
{

	ifp->if_flags |= IFF_RUNNING;
	return 0;
}

static void
tsneta_stop(struct ifnet *ifp, int disable)
{

	ifp->if_flags &= ~IFF_RUNNING;
}

static int
tsneta_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	int	error, s;

	s = splnet();
	error = ether_ioctl(ifp, cmd, data);
	if (error == ENETRESET)
		error = 0;		/* no multicast filter to program */
	splx(s);
	return error;
}

/*
 * Every frame queued goes out in the caller's thread. The frame is built
 * on the stack, so two threads sending at once do not share a buffer;
 * the device keeps its own writes in order.
 */
static void
tsneta_start(struct ifnet *ifp)
{
	uint8_t		frame[TSNETA_FRAME_MAX];
	struct mbuf	*m;
	int		len;

	for (;;) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		len = m->m_pkthdr.len;
		if (len > TSNETA_FRAME_MAX) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			continue;
		}
		m_copydata(m, 0, len, frame);
		bpf_mtap(ifp, m, BPF_D_OUT);
		m_freem(m);
		while (len < TSNETA_FRAME_MIN)
			frame[len++] = 0;
		if (rumpcomp_neta_send(frame, (size_t)len) != 0) {
			if_statinc(ifp, if_oerrors);
		} else {
			if_statadd2(ifp, if_opackets, 1, if_obytes, len);
		}
	}
}

/*
 * The reader: each frame from the card becomes one mbuf (a cluster when
 * it does not fit in a header mbuf), with the IP header after the
 * Ethernet one on a four byte boundary. The frames that came together
 * go up together.
 */
static void
tsneta_rx(void *arg)
{
	struct tsneta_softc	*sc = arg;
	struct ifnet		*ifp = &sc->sc_ec.ec_if;
	void			*bufs[TSNETA_RX_BATCH];
	int			lens[TSNETA_RX_BATCH];
	struct mbuf		*m;
	int			i, n, len, bound;

	for (i = 0; i < TSNETA_RX_BATCH; i++)
		bufs[i] = kmem_alloc(TSNETA_FRAME_MAX, KM_SLEEP);
	for (;;) {
		n = rumpcomp_neta_recv(bufs, lens, TSNETA_RX_BATCH,
		    TSNETA_FRAME_MAX);
		if ((ifp->if_flags & IFF_RUNNING) == 0)
			continue;

		KERNEL_LOCK(1, NULL);
		bound = curlwp_bind();
		for (i = 0; i < n; i++) {
			len = lens[i];
			if (len < (int)sizeof(struct ether_header))
				continue;
			MGETHDR(m, M_DONTWAIT, MT_DATA);
			if (m == NULL) {
				if_statinc(ifp, if_ierrors);
				continue;
			}
			if (len + ETHER_ALIGN > MHLEN) {
				MCLGET(m, M_DONTWAIT);
				if ((m->m_flags & M_EXT) == 0) {
					m_freem(m);
					if_statinc(ifp, if_ierrors);
					continue;
				}
			}
			m->m_data += ETHER_ALIGN;
			memcpy(mtod(m, void *), bufs[i], len);
			m->m_len = m->m_pkthdr.len = len;
			m_set_rcvif(m, ifp);
			if_input(ifp, m);
		}
		curlwp_bindx(bound);
		KERNEL_UNLOCK_LAST(NULL);
	}
}

/*
 * The interface is made when the stack starts, if there is a card. Its
 * name is neta0, after the device.
 */
RUMP_COMPONENT(RUMP_COMPONENT_NET_IF)
{
	struct tsneta_softc	*sc;
	struct ifnet		*ifp;
	uint8_t			mac[ETHER_ADDR_LEN];
	int			mtu = ETHERMTU;

	if (rumpcomp_neta_open(mac, &mtu) != 0) {
		aprint_normal("neta: no card\n");
		return;
	}

	sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
	ifp = &sc->sc_ec.ec_if;
	if_initname(ifp, "neta", 0);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = tsneta_init;
	ifp->if_ioctl = tsneta_ioctl;
	ifp->if_start = tsneta_start;
	ifp->if_stop = tsneta_stop;
	ifp->if_dlt = DLT_EN10MB;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	IFQ_SET_READY(&ifp->if_snd);

	if_initialize(ifp);
	ether_ifattach(ifp, mac);
	if (mtu > 0 && mtu <= ETHERMTU)
		ifp->if_mtu = mtu;
	if_register(ifp);
	tsneta_sc = sc;

	if (kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, tsneta_rx, sc,
	    &sc->sc_rx, "neta0") != 0)
		panic("neta: the reader thread could not be made");

	aprint_normal("neta0: Ethernet address %s\n", ether_sprintf(mac));
}
