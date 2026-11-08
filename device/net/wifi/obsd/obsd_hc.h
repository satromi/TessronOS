/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_hc.h (x64 PC, WIFI_NET80211=obsd)
 *	The calls between OpenBSD's IEEE 802.11 layer and iwm(4), as the
 *	layer of device/net/wifi/obsd runs them (design 9.6.9), and the
 *	TessronOS kernel around them.
 *
 *	The layer is compiled with OpenBSD's kernel headers
 *	(tools/openbsd/build.sh) and the kernel with T-Kernel's, which
 *	cannot meet; what crosses is in plain C types. The kernel's side is
 *	obsd_user.c, and the card's bus, DMA memory, interrupt and firmware
 *	are the hypercalls the Wi-Fi of the rump kernel has as well
 *	(peripheral_kernel/network/netbsd/wifi_user.c, rumpcomp_*).
 */

#ifndef __OBSD_HC_H__
#define __OBSD_HC_H__

/* ---------------------------------------------------------------- obsd_user.c */

/* A lock (a mutex of T-Kernel, not recursive); its number, or a negative one */
extern int	obsd_hc_mtx_new( void );
extern void	obsd_hc_mtx_lock( int id );
extern void	obsd_hc_mtx_unlock( int id );

/* A counting semaphore: its number, or a negative one */
extern int	obsd_hc_sem_new( void );
/* Waits for one count: 0 taken, 1 the time (ms; negative: no end) ran out */
extern int	obsd_hc_sem_wait( int id, long long ms );
extern void	obsd_hc_sem_post( int id );

/* The calling task, as a number that is never 0 */
extern int	obsd_hc_self( void );

/* A task running fn(arg) at priority pri with a stack of stksz bytes; 0, or -1 */
extern int	obsd_hc_thread( void (*fn)( void * ), void *arg, int pri, int stksz );

/* Nanoseconds since the start, and seconds and microseconds of the wall clock */
extern unsigned long long obsd_hc_nsec( void );
extern void	obsd_hc_walltime( long long *sec, long *usec );

/* A busy wait of us microseconds */
extern void	obsd_hc_delay( unsigned int us );

/* Memory of the kernel's heap (its direct map): NULL when there is none */
extern void	*obsd_hc_malloc( unsigned long size );
extern void	obsd_hc_free( void *p );

/* Random bytes */
extern void	obsd_hc_random( void *buf, unsigned long len );

/* A line, or part of one, to the console */
extern void	obsd_hc_puts( const char *s );

/* Where the card is: 0 with its bus, device, function, vendor and device ID, or -1 (a PCI card's) */
extern int	obsd_hc_card( int *bus, int *dev, int *fn, unsigned int *id );

/* The manager (wifi.c) is told that something of the card's waits for it */
extern void	obsd_hc_kick( void );

/* ---------------------------------------------------------------- wifi_user.c (rumpcomp_*) */

extern int	rumpcomp_tswifi_card( int *bus, unsigned int *id );
extern void	*rumpcomp_pci_map( unsigned long addr, unsigned long len );
extern int	rumpcomp_pci_confread( unsigned bus, unsigned dev, unsigned fun, int reg, unsigned int *v );
extern int	rumpcomp_pci_confwrite( unsigned bus, unsigned dev, unsigned fun, int reg, unsigned int v );
extern int	rumpcomp_pci_irq_map( unsigned bus, unsigned dev, unsigned fun, int intrline, unsigned cookie );
extern int	rumpcomp_tswifi_intr_wait( unsigned int cookie );
extern int	rumpcomp_pci_dmalloc( unsigned long size, unsigned long align, unsigned long *pap,
				      unsigned long *vap );
extern void	rumpcomp_pci_dmafree( unsigned long va, unsigned long size );
extern unsigned long rumpcomp_pci_virt_to_mach( void *va );
extern int	rumpcomp_tswifi_fw_open( const char *name, void **data, unsigned long long *size );
extern void	rumpcomp_tswifi_fw_close( void *data, unsigned long long size );
extern void	rumpcomp_tswifi_input( const void *frame, unsigned long long len );

#endif /* __OBSD_HC_H__ */
