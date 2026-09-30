/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_wait.c
 *	What every lock of lib/libpthread waits with.
 *
 *	A thread that must wait puts itself on the list of the thing it
 *	waits for and sleeps on its own semaphore of the kernel (made the
 *	first time it waits, count at most 1). Whoever lets it go takes it
 *	off the list and signals that semaphore. The signal is remembered by
 *	the semaphore, so a thread let go before it has begun to sleep does
 *	not sleep. A thread is on one list at a time and is signalled once
 *	for being taken off it; a thread whose sleep ends by time takes
 *	itself off, or, when someone took it off first, takes the signal
 *	that is on its way, so no signal is left over for its next wait.
 *
 *	The lists are changed under one mutex of the kernel for the whole
 *	process (priority inheritance), the only kernel mutex this library
 *	uses: a process has four (design 9.15).
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/time.h>
#include <errno.h>
#include "pth_int.h"

static ID	glock_id;

void pth_glock( void )
{
	if ( glock_id <= 0 ) {
		T_CMTX	c;
		ID	id;

		/* made at start-up by the main thread, before any other exists */
		c.exinf = NULL;
		c.mtxatr = TA_INHERIT;
		c.ceilpri = 0;
		id = tk_cre_mtx(&c);
		if ( id <= 0 ) {
			for ( ;; ) tk_dly_tsk(1000);	/* nothing can wait without it */
		}
		glock_id = id;
	}
	(void)tk_loc_mtx(glock_id, TMO_FEVR);
}

void pth_gunlock( void )
{
	(void)tk_unl_mtx(glock_id);
}

uint64_t pth_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (uint64_t)ns;
}

/* Nanoseconds between the monotonic clock and CLOCK_REALTIME (lib/libcxxrt) */
extern int64_t __ts_realtime_offset( void );

uint64_t pth_deadline( clockid_t clk, const struct timespec *abstime )
{
	int64_t	t;

	if ( abstime == NULL ) {
		return 0;
	}
	t = (int64_t)abstime->tv_sec * 1000000000LL + abstime->tv_nsec;
	if ( clk != CLOCK_MONOTONIC ) {
		t -= __ts_realtime_offset();
	}
	if ( t <= 0 ) {
		return 1;			/* already past */
	}
	return (uint64_t)t;
}

static int park_sem( PTH *me )
{
	if ( me->sem <= 0 ) {
		T_CSEM	c;
		ID	id;

		c.exinf = NULL;
		c.sematr = TA_TFIFO | TA_FIRST;
		c.isemcnt = 0;
		c.maxsem = 1;
		id = tk_cre_sem(&c);
		if ( id <= 0 ) {
			return -1;
		}
		me->sem = id;
	}
	return 0;
}

int pth_park( PTH *me, uint64_t dl )
{
	ER	er;

	if ( park_sem(me) < 0 ) {
		/* no semaphore to be had: wait by looking every millisecond */
		while ( me->queued ) {
			if ( dl != 0 && pth_now() >= dl ) return ETIMEDOUT;
			tk_dly_tsk(1);
		}
		return 0;
	}
	if ( dl == 0 ) {
		er = tk_wai_sem(me->sem, 1, TMO_FEVR);
	} else {
		uint64_t now = pth_now();
		TMO_U	us = ( dl > now ) ? (TMO_U)( ( dl - now + 999 ) / 1000 ) : 0;

		er = tk_wai_sem_u(me->sem, 1, us);
	}
	return ( er == E_TMOUT ) ? ETIMEDOUT : 0;
}

void pth_unpark( PTH *t )
{
	t->queued = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if ( t->sem > 0 ) {
		(void)tk_sig_sem(t->sem, 1);
	}
}

/*
 * Before a thread first sleeps it has no semaphore, and one that lets
 * it go would see none to signal: the semaphore is made as the thread
 * goes on a list, under the lists' lock, so that cannot happen.
 */
void pth_prepare( PTH *me )
{
	(void)park_sem(me);
	me->queued = 1;
}

void pth_enq( void **q, PTH *t )
{
	pth_prepare(t);
	t->qnext = NULL;
	if ( q[1] != NULL ) {
		((PTH *)q[1])->qnext = t;
	} else {
		q[0] = t;
	}
	q[1] = t;
}

PTH *pth_deq( void **q )
{
	PTH	*t = (PTH *)q[0];

	if ( t != NULL ) {
		q[0] = t->qnext;
		if ( q[0] == NULL ) q[1] = NULL;
		t->qnext = NULL;
	}
	return t;
}

int pth_remove( void **q, PTH *t )
{
	PTH	*p = (PTH *)q[0], *prev = NULL;

	for ( ; p != NULL; prev = p, p = p->qnext ) {
		if ( p != t ) continue;
		if ( prev != NULL ) prev->qnext = p->qnext; else q[0] = p->qnext;
		if ( q[1] == p ) q[1] = prev;
		p->qnext = NULL;
		return 1;
	}
	return 0;
}
