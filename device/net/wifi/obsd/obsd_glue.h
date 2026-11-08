/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_glue.h (x64 PC, WIFI_NET80211=obsd)
 *	What the files of the layer (obsd_kern.c, obsd_net.c, obsd_pci.c,
 *	tswifi_obsd.c) share among themselves. Compiled with OpenBSD's
 *	kernel headers.
 *
 *	Every function of OpenBSD's that runs here runs holding the one
 *	lock of the layer (obsd_lock), as OpenBSD's kernel lock is held by
 *	what is not MP-safe: the manager's calls, the card's interrupt, the
 *	timeouts and the task queues each take it, each in a task of its
 *	own. A sleep (tsleep_nsec) lets it go while it sleeps and takes it
 *	back. The levels of the interrupts (splnet) do nothing: nothing runs
 *	without the lock.
 */

#ifndef __OBSD_GLUE_H__
#define __OBSD_GLUE_H__

#include "obsd_hc.h"

/* The lock of the layer, which the one holding it may take again */
void	obsd_lock( void );
void	obsd_unlock( void );
int	obsd_lock_drop( void );		/* let go however deep: the depth */
void	obsd_lock_take( int depth );	/* taken back as deep */

/* The layer started: its tasks, the mbufs; once */
void	obsd_init( void );

/* What a task of the layer runs, holding the lock (obsd_kern.c) */
int	obsd_task( void (*fn)( void * ), void *arg, int pri );

/* The interfaces' watchdog, once a second (obsd_net.c) */
void	obsd_net_init( void );

/* The hook iwm left until the root file system was there (config_mountroot): run once */
void	obsd_mountroot_run( void );
int	obsd_mountroot_pending( void );

/* The console */
int	printf( const char *, ... );

/* The pools' allocations that failed (obsd_kern.c: mbufs and clusters among them; a frame lost) */
extern unsigned int	obsd_pool_fails;

/*
 * The card reaches 32 bits of address only: the mbufs themselves below 4GB
 * too (obsd_kern.c), set before the driver makes any
 */
extern int	obsd_mbuf_low;

/* The priorities of the layer's tasks (T-Kernel's: smaller is sooner); the manager's is 26 */
#define OBSD_PRI_INTR	20
#define OBSD_PRI_TIMER	22
#define OBSD_PRI_TASKQ	24
#define OBSD_STKSZ	( 32 * 1024 )

#endif /* __OBSD_GLUE_H__ */
