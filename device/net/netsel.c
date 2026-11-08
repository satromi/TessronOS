/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netsel.c (x64 PC, and the Raspberry Pi 5 with the Wi-Fi)
 *	The network card the protocol stack reaches (net_get_mac, net_send,
 *	net_recv, net_stat of <ts/net.h>, the device "neta") on a machine
 *	that may have one of several. A PC: virtio-net in QEMU (device/vnet/vnet.c,
 *	translated on x64 with its functions named knl_vnet_*, build_make/
 *	x64.mk), Intel's e1000e family (the I219 of a notebook's chipset,
 *	e1k.c), a USB adapter of the communications class
 *	(device/usb/x86_64/cdcnet.c), and the Wi-Fi (wifi.c, when the build
 *	has it). The other wired cards (x86_64/re.c, rge.c, igc.c and
 *	device/usb/x86_64/usbven.c) register themselves when their device is
 *	found (knl_netsel_add) and are reached through what they gave.
 *
 *	Until the adapters' manager (netadp.c) has chosen, the first there is
 *	of virtio-net, the Intel card, the USB adapter and the Wi-Fi is taken.
 *	The manager then chooses as the person said (the machine's setting
 *	NET_ADAPTER), or, when they left it to the system, the wired card
 *	whose link is up before the Wi-Fi that is connected; it changes the
 *	address of the stack's interface to the new card's before it tells
 *	this file (knl_netsel_set).
 *
 *	A reader waiting with no limit waits a slice at a time, so that a
 *	change of card is seen by the reader already waiting on the old one.
 *
 *	On the Arm machines this is built only with the Wi-Fi (USE_NETSEL,
 *	build_make/wifi_arm.mk): the board's Ethernet (the RP1's on the
 *	Raspberry Pi 5, virtio-net under QEMU), whose net_* are translated
 *	with the names knl_eth_*, and the Wi-Fi. The Ethernet takes the
 *	place of the first card (NETSEL_VNET); the other two are not there.
 */

#include <sys/machine.h>

#if defined(X64) || defined(USE_NETSEL)

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/net.h>
#include "netsel.h"

#ifndef X64
/* The board's Ethernet in the place of the PC's first card; no Intel card, no USB adapter */
#define knl_vnet_get_mac	knl_eth_get_mac
#define knl_vnet_send		knl_eth_send
#define knl_vnet_recv		knl_eth_recv
#define knl_vnet_stat		knl_eth_stat
#define knl_e1k_get_mac(m)	E_NOEXS
#define knl_e1k_send(f, l)	E_NOEXS
#define knl_e1k_recv(f, s, t)	E_NOEXS
#define knl_e1k_stat(s)		E_NOEXS
#define knl_e1k_told()		TRUE
#define knl_cdcnet_present()	FALSE
#define knl_cdcnet_get_mac(m)	E_NOEXS
#define knl_cdcnet_send(f, l)	E_NOEXS
#define knl_cdcnet_recv(f, s, t) E_NOEXS
#define knl_cdcnet_stat(s)	E_NOEXS
#endif

IMPORT ER	knl_vnet_get_mac( UB *mac );
IMPORT ER	knl_vnet_send( CONST void *frame, SZ len );
IMPORT INT	knl_vnet_recv( void *frame, SZ size, TMO tmout );
IMPORT ER	knl_vnet_stat( T_NETSTAT *st );
#ifdef X64
IMPORT ER	knl_e1k_get_mac( UB *mac );
IMPORT ER	knl_e1k_send( CONST void *frame, SZ len );
IMPORT INT	knl_e1k_recv( void *frame, SZ size, TMO tmout );
IMPORT ER	knl_e1k_stat( T_NETSTAT *st );
IMPORT BOOL	knl_e1k_told( void );
IMPORT BOOL	knl_cdcnet_present( void );
IMPORT ER	knl_cdcnet_get_mac( UB *mac );
IMPORT ER	knl_cdcnet_send( CONST void *frame, SZ len );
IMPORT INT	knl_cdcnet_recv( void *frame, SZ size, TMO tmout );
IMPORT ER	knl_cdcnet_stat( T_NETSTAT *st );
#endif

#if USE_WIFI
IMPORT BOOL	knl_wifi_present( void );
IMPORT ER	knl_wifi_get_mac( UB *mac );
IMPORT ER	knl_wifi_send( CONST void *frame, SZ len );
IMPORT INT	knl_wifi_recv( void *frame, SZ size, TMO tmout );
IMPORT ER	knl_wifi_stat( T_NETSTAT *st );
#endif

#define SLICE_MS	200		/* a reader's wait with no limit, a slice at a time */

LOCAL volatile INT	chosen = -1;	/* the manager's choice, NETSEL_*; -1 none yet */
LOCAL BOOL		mgr_started = FALSE;
LOCAL CONST T_NETDRV	*drv[NETSEL_N];	/* the cards that registered themselves */

EXPORT void knl_netsel_add( INT c, CONST T_NETDRV *d )
{
	if ( c >= 0 && c < NETSEL_N ) drv[c] = d;
}

EXPORT CONST T_NETDRV *knl_netsel_drv( INT c )
{
	return ( c >= 0 && c < NETSEL_N ) ? drv[c] : NULL;
}

/* A registered card with its address: it is there now */
LOCAL BOOL drv_there( INT c )
{
	UB	m[NET_MAC_LEN];

	return drv[c] != NULL && drv[c]->get_mac(drv[c]->ctx, m) >= E_OK;
}

/* The first card there is: what is used before the manager has chosen */
EXPORT INT knl_netsel_first( void )
{
	UB	m[NET_MAC_LEN];

	if ( knl_vnet_get_mac(m) >= E_OK ) return NETSEL_VNET;
	if ( knl_e1k_get_mac(m) >= E_OK ) return NETSEL_E1K;
	if ( drv_there(NETSEL_RE) ) return NETSEL_RE;
	if ( drv_there(NETSEL_RGE) ) return NETSEL_RGE;
	if ( drv_there(NETSEL_IGC) ) return NETSEL_IGC;
	if ( knl_cdcnet_present() ) return NETSEL_CDC;
	if ( drv_there(NETSEL_USBV) ) return NETSEL_USBV;
#if USE_WIFI
	if ( knl_wifi_present() ) return NETSEL_WIFI;
#endif
	return NETSEL_CDC;
}

LOCAL INT card( void )
{
	INT	c = chosen;

	return ( c >= 0 ) ? c : knl_netsel_first();
}

EXPORT INT knl_netsel_card( void )
{
	return card();
}

EXPORT void knl_netsel_set( INT c )
{
	chosen = c;
}

/* What one card answers: its address, its counters and its link */
EXPORT ER knl_netsel_mac( INT c, UB *mac )
{
	switch ( c ) {
	case NETSEL_VNET:	return knl_vnet_get_mac(mac);
	case NETSEL_E1K:	return knl_e1k_get_mac(mac);
	case NETSEL_CDC:	return knl_cdcnet_present() ? knl_cdcnet_get_mac(mac) : E_NOEXS;
#if USE_WIFI
	case NETSEL_WIFI:	return knl_wifi_get_mac(mac);
#endif
	default:		return ( c >= 0 && c < NETSEL_N && drv[c] != NULL )
					? drv[c]->get_mac(drv[c]->ctx, mac) : E_NOEXS;
	}
}

EXPORT ER knl_netsel_stat( INT c, T_NETSTAT *st )
{
	switch ( c ) {
	case NETSEL_VNET:	return knl_vnet_stat(st);
	case NETSEL_E1K:	return knl_e1k_stat(st);
	case NETSEL_CDC:	return knl_cdcnet_present() ? knl_cdcnet_stat(st) : E_NOEXS;
#if USE_WIFI
	case NETSEL_WIFI:	return knl_wifi_stat(st);
#endif
	default:		return ( c >= 0 && c < NETSEL_N && drv[c] != NULL )
					? drv[c]->stat(drv[c]->ctx, st) : E_NOEXS;
	}
}

EXPORT BOOL knl_netsel_silent( void )
{
	UB	m[NET_MAC_LEN];

#ifdef X64
	INT	c;

	for ( c = NETSEL_RE; c < NETSEL_N; c++ ) {
		if ( drv_there(c) && !drv[c]->told(drv[c]->ctx) ) return TRUE;
	}
	return knl_e1k_get_mac(m) >= E_OK && !knl_e1k_told();
#else
	return knl_vnet_get_mac(m) >= E_OK;	/* the board's Ethernet */
#endif
}

EXPORT ER net_get_mac( UB *mac )
{
	if ( !mgr_started ) {
		/* the first to ask is the start-up's (knl_netdev_init): the manager starts behind it */
		mgr_started = TRUE;
		knl_netadp_start();
	}
	return knl_netsel_mac(card(), mac);
}

EXPORT ER net_send( CONST void *frame, SZ len )
{
	INT	c = card();

	if ( c >= NETSEL_RE && drv[c] != NULL ) return drv[c]->send(drv[c]->ctx, frame, len);
	switch ( c ) {
	case NETSEL_VNET:	return knl_vnet_send(frame, len);
	case NETSEL_E1K:	return knl_e1k_send(frame, len);
#if USE_WIFI
	case NETSEL_WIFI:	return knl_wifi_send(frame, len);
#endif
	default:		return knl_cdcnet_send(frame, len);
	}
}

LOCAL INT recv_on( INT c, void *frame, SZ size, TMO tmout )
{
	if ( c >= NETSEL_RE && drv[c] != NULL ) return drv[c]->recv(drv[c]->ctx, frame, size, tmout);
	switch ( c ) {
	case NETSEL_VNET:	return knl_vnet_recv(frame, size, tmout);
	case NETSEL_E1K:	return knl_e1k_recv(frame, size, tmout);
#if USE_WIFI
	case NETSEL_WIFI:	return knl_wifi_recv(frame, size, tmout);
#endif
	default:		return knl_cdcnet_recv(frame, size, tmout);
	}
}

EXPORT INT net_recv( void *frame, SZ size, TMO tmout )
{
	INT	n;

	if ( tmout != TMO_FEVR ) {
		return recv_on(card(), frame, size, tmout);
	}
	for ( ;; ) {
		n = recv_on(card(), frame, size, SLICE_MS);
		if ( n != E_TMOUT ) return n;
	}
}

EXPORT ER net_stat( T_NETSTAT *st )
{
	return knl_netsel_stat(card(), st);
}

#endif /* X64 || USE_NETSEL */
