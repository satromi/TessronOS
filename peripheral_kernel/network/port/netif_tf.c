/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netif_tf.c
 *	The card as an lwIP interface (design 10.4, 12.6).
 *
 *	The device "neta" carries one whole Ethernet frame per read or
 *	write, which is what lwIP wants of a link layer, so the interface is
 *	thin: a frame out is the buffer chain flattened and written, a frame
 *	in is a read handed to the stack.
 *
 *	Reading blocks, so it gets a task of its own. It hands what arrives
 *	to tcpip_input, which is the entry that may be called from another
 *	thread; the stack itself runs in the tcpip task.
 */

#include <tk/tkernel.h>
#include <ts/net.h>
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "netif/ethernet.h"
#include "ts_netif.h"

#define NETIF_TASK_PRI		11	/* just under the tcpip task */
#define NETIF_TASK_STKSZ	(8 * 1024)
#define NETIF_RETRY_MS		1000	/* ms: after a request the card refused */

LOCAL ID	neta_dd = 0;		/* the open device */
LOCAL BOOL	link_up = FALSE;	/* read before the reader takes over */
LOCAL UB	rx_frame[NET_FRAME_MAX];
LOCAL UB	tx_frame[NET_FRAME_MAX];

/*
 * One frame out. The chain is flattened into a buffer of its own because
 * the device takes a single run of bytes.
 */
LOCAL err_t ts_netif_output( struct netif *netif, struct pbuf *p )
{
	SZ	asize;
	u16_t	len;

	if ( neta_dd <= 0 ) {
		return ERR_IF;
	}
	len = pbuf_copy_partial(p, tx_frame, sizeof(tx_frame), 0);
	if ( len == 0 ) {
		return ERR_BUF;
	}
	/* the card fills a short frame out itself, but not every one does */
	while ( len < NET_FRAME_MIN ) {
		tx_frame[len++] = 0;
	}
	if ( tk_swri_dev(neta_dd, 0, tx_frame, (SZ)len, &asize) < E_OK ) {
		LINK_STATS_INC(link.drop);
		return ERR_IF;
	}
	LINK_STATS_INC(link.xmit);

	return ERR_OK;
}

/*
 * The task that waits on the card. Everything that arrives becomes a
 * buffer and goes to the stack; a frame that will not fit in one is
 * dropped rather than split, since the card never sends one that long.
 */
LOCAL void ts_netif_rx_task( INT stacd, void *exinf )
{
	struct netif	*netif = (struct netif *)exinf;
	struct pbuf	*p;
	SZ		asize;
	ER		ioer;
	ID		reqid;

	for (;;) {
		/* No limit: the card wakes this when a frame lands, and
		   waking on a timer instead would cost an interrupt every
		   time round on a machine that has nothing to do. Nothing
		   else reads the card while this waits. */
		reqid = tk_rea_dev(neta_dd, 0, rx_frame, sizeof(rx_frame),
				   TMO_FEVR);
		if ( reqid < E_OK ) {
			tk_dly_tsk(NETIF_RETRY_MS);
			continue;
		}
		if ( tk_wai_dev(neta_dd, reqid, &asize, &ioer,
				TMO_FEVR) < E_OK
		  || ioer < E_OK || asize <= 0 ) {
			continue;		/* nothing usable arrived */
		}

		p = pbuf_alloc(PBUF_RAW, (u16_t)asize, PBUF_POOL);
		if ( p == NULL ) {
			LINK_STATS_INC(link.memerr);
			LINK_STATS_INC(link.drop);
			continue;
		}
		if ( pbuf_take(p, rx_frame, (u16_t)asize) != ERR_OK ) {
			pbuf_free(p);
			LINK_STATS_INC(link.drop);
			continue;
		}
		LINK_STATS_INC(link.recv);

		if ( tcpip_input(p, netif) != ERR_OK ) {
			pbuf_free(p);
		}
	}
}

/*
 * Called by lwIP once, to fill the interface in. The address and the
 * largest payload come from the device.
 */
EXPORT err_t ts_netif_init( struct netif *netif )
{
	T_CTSK	ctsk;
	UB	mac[NET_MAC_LEN];
	W	mtu = 1500;
	SZ	asize;
	ID	tskid;
	INT	i;

	if ( netif == NULL ) {
		return ERR_ARG;
	}
	neta_dd = tk_opn_dev((UB *)"neta", TD_UPDATE);
	if ( neta_dd <= 0 ) {
		return ERR_IF;			/* the machine has no card */
	}
	if ( tk_srea_dev(neta_dd, TDN_NETADDR, mac, NET_MAC_LEN, &asize) < E_OK ) {
		tk_cls_dev(neta_dd, 0);
		neta_dd = 0;
		return ERR_IF;
	}
	tk_srea_dev(neta_dd, TDN_NETMTU, &mtu, sizeof(mtu), &asize);
	tk_srea_dev(neta_dd, TDN_NETLINK, &link_up, sizeof(link_up), &asize);

	netif->name[0] = 'e';
	netif->name[1] = 'n';
	netif->hwaddr_len = ETH_HWADDR_LEN;
	for ( i = 0; i < ETH_HWADDR_LEN; i++ ) {
		netif->hwaddr[i] = mac[i];
	}
	netif->mtu = (u16_t)mtu;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
		     | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
	netif->output = etharp_output;
	netif->linkoutput = ts_netif_output;
#if LWIP_NETIF_HOSTNAME
	netif->hostname = "tessronos";
#endif

	/* the reader can only start once the interface is filled in */
	ctsk.exinf   = netif;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)ts_netif_rx_task;
	ctsk.itskpri = NETIF_TASK_PRI;
	ctsk.stksz   = NETIF_TASK_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		tk_cls_dev(neta_dd, 0);
		neta_dd = 0;
		return ERR_MEM;
	}
	if ( tk_sta_tsk(tskid, 0) < E_OK ) {
		tk_del_tsk(tskid);
		tk_cls_dev(neta_dd, 0);
		neta_dd = 0;
		return ERR_MEM;
	}

	return ERR_OK;
}

/*
 * Whether the card reported a carrier when the interface was set up.
 * It is read there rather than here because the reader below holds the
 * card from the moment it starts.
 */
EXPORT BOOL ts_netif_link_up( void )
{
	return link_up;
}
