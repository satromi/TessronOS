/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_user.c (x64 PC, Raspberry Pi, WIFI_NET80211=obsd)
 *	The kernel's side of obsd_hc.h (design 9.6.9): what the layer of
 *	OpenBSD's IEEE 802.11 code and iwm(4) (tools/openbsd/build.sh,
 *	~/tessronos-obsd-x64/obsdwifi.o) has of T-Kernel: locks, semaphores,
 *	tasks, time, memory, random bytes, the console, the card's place, and
 *	the Wi-Fi manager woken. Compiled by the kernel's build with
 *	T-Kernel's headers.
 */

#include <sys/machine.h>

#if defined(WIFI_OBSD)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#ifdef X64
#include <ts/pcie.h>
#endif
#include <ts/time.h>
#include "tstdlib.h"
#include "obsd_hc.h"

#ifdef X64
IMPORT BOOL	knl_wifi_pci_find( T_PCIDEV *d );	/* wifi_user.c */
#endif
IMPORT void	knl_wifi_kick( void );			/* wifi.c */
IMPORT ER	ts_get_random( void *buf, SZ len );

EXPORT int obsd_hc_mtx_new( void )
{
	T_CMTX	cm;

	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	return (int)tk_cre_mtx(&cm);
}

EXPORT void obsd_hc_mtx_lock( int id )
{
	(void)tk_loc_mtx((ID)id, TMO_FEVR);
}

EXPORT void obsd_hc_mtx_unlock( int id )
{
	(void)tk_unl_mtx((ID)id);
}

EXPORT int obsd_hc_sem_new( void )
{
	T_CSEM	cs;

	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 0x7fffffff;
	return (int)tk_cre_sem(&cs);
}

EXPORT int obsd_hc_sem_wait( int id, long long ms )
{
	TMO	tmo;
	ER	er;

	if ( ms < 0 ) {
		tmo = TMO_FEVR;
	} else if ( ms == 0 ) {
		tmo = TMO_POL;
	} else {
		tmo = ( ms > 0x7fffffffLL ) ? 0x7fffffff : (TMO)ms;
	}
	er = tk_wai_sem((ID)id, 1, tmo);
	return ( er == E_OK ) ? 0 : 1;
}

EXPORT void obsd_hc_sem_post( int id )
{
	(void)tk_sig_sem((ID)id, 1);
}

EXPORT int obsd_hc_self( void )
{
	return (int)tk_get_tid();
}

typedef struct {
	void	(*fn)( void * );
	void	*arg;
} OBSD_START;

LOCAL void obsd_task_entry( INT stacd, void *exinf )
{
	OBSD_START	st = *(OBSD_START *)exinf;

	(void)stacd;
	Kfree(exinf);
	(*st.fn)(st.arg);
	tk_exd_tsk();
}

EXPORT int obsd_hc_thread( void (*fn)( void * ), void *arg, int pri, int stksz )
{
	OBSD_START	*st;
	T_CTSK		ct;
	ID		t;

	st = (OBSD_START *)Kmalloc(sizeof(*st));
	if ( st == NULL ) return -1;
	st->fn = fn;
	st->arg = arg;
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = st;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)obsd_task_entry;
	ct.itskpri = (PRI)pri;
	ct.stksz = (SZ)stksz;
	t = tk_cre_tsk(&ct);
	if ( t <= 0 || tk_sta_tsk(t, 0) < E_OK ) {
		Kfree(st);
		return -1;
	}
	return 0;
}

EXPORT unsigned long long obsd_hc_nsec( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (unsigned long long)ns;
}

EXPORT void obsd_hc_walltime( long long *sec, long *usec )
{
	SYSTIM	t;
	D	ms;

	if ( tk_get_utc(&t) < E_OK ) {
		*sec = 0;
		*usec = 0;
		return;
	}
	ms = ( (D)t.hi << 32 ) | t.lo;
	*sec = (long long)( ms / 1000 );
	*usec = (long)( ms % 1000 ) * 1000;
}

EXPORT void obsd_hc_delay( unsigned int us )
{
	WaitUsec((UW)us);
}

EXPORT void *obsd_hc_malloc( unsigned long size )
{
	return Kmalloc((size_t)size);
}

EXPORT void obsd_hc_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

EXPORT void obsd_hc_random( void *buf, unsigned long len )
{
	(void)ts_get_random(buf, (SZ)len);
}

IMPORT void	knl_net_stamp_line( CONST UB *line );	/* so_api.c */

/* A line of the layer's (iwm0: ..., net80211's): with the time since the start, as the manager's */
EXPORT void obsd_hc_puts( const char *s )
{
	knl_net_stamp_line((CONST UB *)s);
}

#ifdef X64
/* Where the card iwm drives is, its path enabled (rumpcomp_tswifi_card); 0, or -1 */
EXPORT int obsd_hc_card( int *bus, int *dev, int *fn, unsigned int *id )
{
	T_PCIDEV	d;
	int		b;

	if ( rumpcomp_tswifi_card(&b, id) != 0 ) return -1;
	if ( !knl_wifi_pci_find(&d) ) return -1;
	*bus = (int)d.bus;
	*dev = (int)d.dev;
	*fn = (int)d.fn;
	return 0;
}
#endif /* X64 */

EXPORT void obsd_hc_kick( void )
{
	knl_wifi_kick();
}

#endif /* WIFI_OBSD */
