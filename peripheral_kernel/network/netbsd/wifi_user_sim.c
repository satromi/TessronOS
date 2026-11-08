/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifi_user_sim.c (the Wi-Fi simulator, WIFI_SIM=1)
 *	The hypercalls of the simulated radios inside the rump kernel
 *	(rumpcomp/wifisim/tswifi_sim.c, design 9.6.12), in the place of
 *	the card's (wifi_user.c on the x64 PC, wifi_user_rpi5.c on the
 *	Raspberry Pi 5).
 *
 *	The simulator's station is the card: it is always there, and the
 *	frames it receives go to the Wi-Fi manager (wifi.c) as the card's
 *	do. The frames the simulated access point receives go to it
 *	(device/net/wifi/sim/wifisim_ap.c). Neither waits: both come from
 *	the rump kernel's soft interrupt.
 */

#include <sys/machine.h>

#if WIFI_SIM

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

IMPORT void	knl_wifi_rx( CONST UB *frame, INT len );		/* wifi.c */
IMPORT void	knl_wifisim_ap_rx( CONST UB *frame, INT len );		/* wifisim_ap.c */

/* The card the Wi-Fi manager looks for: the simulator's station */
EXPORT BOOL knl_wifisim_present( void )
{
	return TRUE;
}

#if defined(X64) || WIFI_OBSD
/* What the manager says of the card's interrupt for the records: the simulator has none */
EXPORT void knl_wifi_irq_say( CONST char *why )
{
	(void)why;
}

#if WIFI_OBSD && defined(X64)
#include <ts/pcie.h>

/* The card OpenBSD's layer would look for (obsd_user.c): none, the simulator's station is in the layer */
EXPORT BOOL knl_wifi_pci_find( T_PCIDEV *out )
{
	(void)out;
	return FALSE;
}

EXPORT int rumpcomp_tswifi_card( int *bus, unsigned int *id )
{
	(void)bus;
	(void)id;
	return -1;
}
#endif

#if WIFI_OBSD
/*
 * The layer's pools of large items (mbuf clusters, obsd_kern.c's
 * pool_get) take memory a card could reach by DMA: no card here, so
 * the kernel's memory, aligned as asked. Never given back (the pools
 * keep what they take).
 */
EXPORT int rumpcomp_pci_dmalloc( unsigned long size, unsigned long align, unsigned long *pap,
				 unsigned long *vap )
{
	UB	*p;

	if ( align == 0 ) align = 1;
	p = (UB *)Kmalloc((SZ)( size + align ));
	if ( p == NULL ) return 12;			/* ENOMEM */
	*vap = ( (unsigned long)p + align - 1 ) & ~( align - 1 );
	*pap = 0;
	return 0;
}
#endif
#endif

/* A frame the station received: to the manager */
EXPORT void rumpcomp_tswifi_input( CONST void *frame, UD len )
{
	knl_wifi_rx((CONST UB *)frame, (INT)len);
}

EXPORT void rumpcomp_tswifi_say( CONST char *s )
{
	tm_putstring((CONST UB *)s);
}

/* A frame the access point received: to the simulated access point */
EXPORT void rumpcomp_tswifisim_ap_input( CONST void *frame, UD len )
{
	knl_wifisim_ap_rx((CONST UB *)frame, (INT)len);
}

#endif /* WIFI_SIM */
