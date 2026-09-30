/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	net.h
 *	Network link layer (design 10.4, phase 8)
 *
 *	Whole Ethernet frames go in and out; the protocol stack sits above
 *	this. The address and the counters come from the card.
 */

#ifndef __TS_NET_H__
#define __TS_NET_H__

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAME_MIN	60		/* without the frame check sequence */
#define NET_FRAME_MAX	1514		/* 1500 of payload plus the header */
#define NET_MAC_LEN	6

typedef struct {
	UD	sent;
	UD	recv;
	UD	drop;			/* arrived but did not fit the caller */
	BOOL	up;
} T_NETSTAT;

/*
 * Attribute data of the "neta" device (design 10.4). The numbers of
 * T-Kernel itself stop at -4, so these start clear of them.
 */
#define TDN_NETADDR	(-16)	/* R-: the address of the card */
#define TDN_NETSTAT	(-17)	/* R-: T_NETSTAT */
#define TDN_NETMTU		(-18)	/* R-: the largest payload (W) */
#define TDN_NETLINK	(-19)	/* R-: does the link carry (BOOL) */

IMPORT ER  net_get_mac( UB *mac );			/* NET_MAC_LEN bytes */
IMPORT ER  net_send( CONST void *frame, SZ len );
IMPORT INT net_recv( void *frame, SZ size, TMO tmout );	/* bytes, or an error */
IMPORT ER  net_stat( T_NETSTAT *st );

IMPORT INT knl_vnet_init( void );	/* virtio-net (device/vnet/vnet.c) */
IMPORT INT knl_eth_rp1_init( void );	/* RP1 Ethernet (device/net/eth_rp1.c) */
IMPORT INT knl_netdev_init( void );	/* the card as "neta" (device/net/netdev.c) */

#ifdef __cplusplus
}
#endif

#endif /* __TS_NET_H__ */
