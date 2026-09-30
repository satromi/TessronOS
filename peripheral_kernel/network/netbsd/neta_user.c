/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	neta_user.c
 *	The hypercalls of the interface neta0 (design 10.4, 12.6).
 *
 *	The interface inside the rump kernel (rumpcomp/if_tsneta.c) reaches
 *	the card through these: the device "neta", one whole Ethernet frame
 *	per read or write.
 *
 *	Frames out go into a ring, and a task of their own writes them to
 *	the card: the device makes a writer wait until the card has taken
 *	the frame, and a thread of the rump kernel waiting there would hold
 *	up the rest of the stack for every frame. Only when the ring is full
 *	does a sender wait, having given up its virtual processor.
 *
 *	Frames in are read by a thread of the rump kernel; while it waits on
 *	the card it gives up its virtual processor. What else has arrived by
 *	the time the first frame comes is read at once with it, so that a
 *	burst enters the rump kernel once.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/net.h>
#include "tstdlib.h"
#include "rumpglue.h"

#define NETA_RETRY_MS		1000	/* after a request the card refused */
#define NETA_TX_SLOTS		128	/* frames waiting to go out */
#define NETA_TX_PRI		11	/* below the stack's threads: they are not taken off a processor for it */
#define NETA_TX_STKSZ		(8 * 1024)

LOCAL ID	neta_dd = 0;

typedef struct {
	UH	len;
	UB	frame[NET_FRAME_MAX];
} NETA_SLOT;

LOCAL NETA_SLOT		*tx_ring;
LOCAL T_SPLOCK		tx_lock;
LOCAL UINT		tx_head, tx_tail;	/* taken from head, put at tail */
LOCAL ID		tx_sem;			/* one count for each frame in the ring */
LOCAL ID		tx_room;		/* signalled when a full ring has room again */
LOCAL volatile INT	tx_waiting;		/* senders waiting for room */
LOCAL UD		tx_dropped;

/* The task that writes the ring's frames to the card, in order */
LOCAL void neta_tx_task( INT stacd, void *exinf )
{
	NETA_SLOT	*sl;
	SZ		asize;
	UINT		imask;
	INT		wake;

	for ( ;; ) {
		if ( tk_wai_sem(tx_sem, 1, TMO_FEVR) < E_OK ) {
			continue;
		}
		sl = &tx_ring[tx_head % NETA_TX_SLOTS];
		if ( tk_swri_dev(neta_dd, 0, sl->frame, (SZ)sl->len, &asize) < E_OK ) {
			tx_dropped++;
		}
		ISpinLock(&tx_lock, &imask);
		tx_head++;
		wake = tx_waiting;
		tx_waiting = 0;
		ISpinUnlock(&tx_lock, &imask);
		if ( wake > 0 ) {
			(void)tk_sig_sem(tx_room, wake);
		}
	}
}

/* The card opened, its address and largest payload; 0 or ENXIO (6) */
EXPORT int rumpcomp_neta_open( UB *mac, int *mtu )
{
	T_CSEM	csem;
	T_CTSK	ctsk;
	W	m = 1500;
	SZ	asize;
	ID	tskid;

	neta_dd = tk_opn_dev((UB *)"neta", TD_UPDATE);
	if ( neta_dd <= 0 ) {
		neta_dd = 0;
		return 6;
	}
	if ( tk_srea_dev(neta_dd, TDN_NETADDR, mac, NET_MAC_LEN, &asize) < E_OK ) {
		tk_cls_dev(neta_dd, 0);
		neta_dd = 0;
		return 6;
	}
	if ( tk_srea_dev(neta_dd, TDN_NETMTU, &m, sizeof(m), &asize) >= E_OK ) {
		*mtu = (int)m;
	}

	tx_ring = (NETA_SLOT *)Kmalloc(sizeof(NETA_SLOT) * NETA_TX_SLOTS);
	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem  = NETA_TX_SLOTS;
	tx_sem = tk_cre_sem(&csem);
	csem.maxsem  = 0x7fffffff;
	tx_room = tk_cre_sem(&csem);
	InitSpinLock(&tx_lock);
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)neta_tx_task;
	ctsk.itskpri = NETA_TX_PRI;
	ctsk.stksz   = NETA_TX_STKSZ;
	tskid = ( tx_ring != NULL && tx_sem > 0 && tx_room > 0 ) ? tk_cre_tsk(&ctsk) : E_NOMEM;
	if ( tskid <= 0 || tk_sta_tsk(tskid, 0) < E_OK ) {
		tm_printf((UB *)"neta: no sender for the card (%d)\n", tskid);
		tk_cls_dev(neta_dd, 0);
		neta_dd = 0;
		return 6;
	}
	return 0;
}

/*
 * One frame out, into the ring; 0. A full ring makes the sender wait
 * for room, outside the rump kernel.
 */
EXPORT int rumpcomp_neta_send( CONST void *frame, UD len )
{
	NETA_SLOT	*sl;
	UINT		imask;
	INT		n;
	BOOL		put;

	if ( len > NET_FRAME_MAX ) {
		return 5;
	}
	for ( ;; ) {
		ISpinLock(&tx_lock, &imask);
		put = ( tx_tail - tx_head < NETA_TX_SLOTS );
		if ( put ) {
			sl = &tx_ring[tx_tail % NETA_TX_SLOTS];
			knl_memcpy(sl->frame, frame, (SZ)len);
			sl->len = (UH)len;
			tx_tail++;
		} else {
			tx_waiting++;
		}
		ISpinUnlock(&tx_lock, &imask);
		if ( put ) {
			(void)tk_sig_sem(tx_sem, 1);
			return 0;
		}
		knl_rump_unsched(&n);
		(void)tk_wai_sem(tx_room, 1, 100);
		knl_rump_sched(n);
	}
}

/*
 * The frames that have come in, up to max of them into bufs (each size
 * bytes), their lengths into lens: how many. The first is waited for
 * with no limit, as the card wakes the reader when a frame lands; the
 * rest are only those already there.
 */
EXPORT int rumpcomp_neta_recv( void **bufs, int *lens, int max, UD size )
{
	SZ	asize;
	ER	ioer;
	ID	reqid;
	INT	n, got = 0;

	knl_rump_unsched(&n);
	while ( got < max ) {
		reqid = tk_rea_dev(neta_dd, 0, bufs[got], (SZ)size, ( got == 0 ) ? TMO_FEVR : TMO_POL);
		if ( reqid < E_OK ) {
			if ( got > 0 ) break;
			tk_dly_tsk(NETA_RETRY_MS);
			continue;
		}
		asize = 0;
		if ( tk_wai_dev(neta_dd, reqid, &asize, &ioer, TMO_FEVR) >= E_OK
		  && ioer >= E_OK && asize > 0 ) {
			lens[got++] = (int)asize;
		} else if ( got > 0 ) {
			break;			/* nothing more waiting */
		}
	}
	knl_rump_sched(n);
	return got;
}
