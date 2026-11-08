/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netsel.h (x64 PC)
 *	The network cards of a PC and the one the stack uses (netsel.c), and
 *	the adapters' manager that chooses it (netadp.c).
 */

#ifndef __NETSEL_H__
#define __NETSEL_H__

#include <ts/net.h>

/* The cards, as netsel.c and the machine's setting NET_ADAPTER name them */
#define NETSEL_VNET	0		/* virtio-net (QEMU), "vnet" */
#define NETSEL_E1K	1		/* Intel's e1000e family (the I219), "e1k" */
#define NETSEL_CDC	2		/* a USB adapter of the communications class, "cdcnet" */
#define NETSEL_WIFI	3		/* the Wi-Fi, "wifi" */
#define NETSEL_RE	4		/* Realtek's RTL8111/8168, "re" */
#define NETSEL_RGE	5		/* Realtek's RTL8125, "rge" */
#define NETSEL_IGC	6		/* Intel's I225/I226, "igc" */
#define NETSEL_USBV	7		/* a USB adapter by its maker's protocol (RTL8152/8153, AX88179), "usblan" */
#define NETSEL_N	8

/*
 * A card that registers itself (knl_netsel_add): the machine's other cards
 * stay as they are, and a build or a machine without it knows nothing of it.
 * label: what it is, for the adapters' list; its length.
 */
typedef struct {
	ER	(*get_mac)( void *ctx, UB *mac );
	ER	(*send)( void *ctx, CONST void *frame, SZ len );
	INT	(*recv)( void *ctx, void *frame, SZ size, TMO tmout );
	ER	(*stat)( void *ctx, T_NETSTAT *st );
	BOOL	(*told)( void *ctx );		/* something says when its link changes */
	INT	(*speed)( void *ctx );		/* in Mb/s, 0 while its link is down */
	INT	(*label)( void *ctx, UB *t, INT max );
	void	*ctx;
} T_NETDRV;

IMPORT void	knl_netsel_add( INT c, CONST T_NETDRV *d );
IMPORT CONST T_NETDRV *knl_netsel_drv( INT c );	/* NULL when card c registered nothing */

IMPORT INT	knl_netsel_first( void );		/* the first card there is */
IMPORT INT	knl_netsel_card( void );		/* the card in use */
IMPORT void	knl_netsel_set( INT c );		/* the card chosen */
IMPORT ER	knl_netsel_mac( INT c, UB *mac );
IMPORT ER	knl_netsel_stat( INT c, T_NETSTAT *st );

/*
 * Whether there is a card whose link nothing tells of (knl_netadp_link):
 * the board's Ethernet, whose PHY has no interrupt, and the Intel
 * Ethernet without its interrupt. The adapters' manager looks at such a
 * card every second.
 */
IMPORT BOOL	knl_netsel_silent( void );

/* The adapters' manager (netadp.c), started by the first ask for the card's address */
IMPORT void	knl_netadp_start( void );

/* The Wi-Fi manager (wifi.c): started by the adapters' manager once the object layer is up */
IMPORT void	knl_wifi_start( void );
IMPORT BOOL	knl_wifi_connected( void );
IMPORT BOOL	knl_wifi_mac_known( void );	/* the card's own address read: the card started */
/* The adapters' list's lines of it; without measured, its signal and rate left out (what is told of) */
IMPORT INT	knl_wifi_line( UB *t, INT n, INT max, BOOL measured );
IMPORT INT	knl_wifi_name( UB *t, INT n, INT max );	/* its name as the adapters are shown */

#endif /* __NETSEL_H__ */
