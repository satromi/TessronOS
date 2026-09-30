/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	timer.c
 *	Timer Control: tickless high-resolution timer (design 7.4)
 */

#include "kernel.h"
#include "timer.h"
#include "time_calls.h"
#include "../sysdepend/sys_timer.h"

/*
 * Time base
 *	The monotonic clock is computed from the Generic Timer counter on
 *	every read (knl_get_mono_ns); nothing is accumulated per tick.
 *	knl_real_time_ofs is the difference between the system clock (UTC)
 *	and the monotonic clock. tk_set_tim() changes only the offset, so
 *	the monotonic clock never jumps.
 */
Noinit(EXPORT D	knl_real_time_ofs);		/* UTC ns - monotonic ns */

/*
 * Timer event queue (sorted by expiry, FIFO among equal expiries)
 */
Noinit(EXPORT QUEUE	knl_timer_queue);

EXPORT UD	knl_timer_irq_count = 0;

LOCAL ABSTIM	knl_timer_programmed;		/* expiry set in the comparator */
LOCAL BOOL	knl_timer_armed;		/* comparator is running */

/*
 * Program the comparator for the head of the queue (or stop it)
 *	Called with interrupts disabled.
 */
LOCAL void knl_timer_program( void )
{
	ABSTIM	next;

#if USE_SMP
	/* The Generic Timer is per processor: the comparator of PRC 1 serves
	   the queue. Another processor asks PRC 1 to reprogram it. */
	if ( knl_pcpu()->prcid != 1 ) {
		knl_send_ipi(1U << 0, SGI_TIMER);
		return;
	}
#endif
	if ( isQueEmpty(&knl_timer_queue) ) {
		/* Always stop: an expiry that already fired, or one whose event
		   was deleted, would otherwise keep the interrupt line asserted */
		knl_stop_hw_timer();
		knl_timer_armed = FALSE;
		return;
	}

	next = ((TMEB*)knl_timer_queue.next)->time;
	if ( !knl_timer_armed || next != knl_timer_programmed ) {
		knl_set_hw_timer(next);
		knl_timer_programmed = next;
		knl_timer_armed = TRUE;
	}
}

/*
 * Start system timer
 */
EXPORT ER knl_timer_startup( void )
{
	UD	sec;

	knl_real_time_ofs = 0;
	knl_timer_irq_count = 0;
	knl_timer_armed = FALSE;
	QueInit(&knl_timer_queue);

	/* Clock base and interrupt line; the comparator stays idle */
	knl_start_hw_timer();

	/*
	 * System clock from the RTC (UNIX seconds). Without one, or with an
	 * RTC that was never set and counts from 1970 (a Raspberry Pi 5
	 * without its battery), the clock starts at 1985-01-01, the origin
	 * of TRON time, and is taken as not set (design 11.4).
	 */
	knl_real_time_ofs = (D)DIFF_TRON_UTC * (D)NS_PER_MS - (D)knl_get_mono_ns();
	if ( knl_rtc_read(&sec) == E_OK && (D)sec * 1000 >= (D)DIFF_TRON_UTC ) {
		knl_real_time_ofs = (D)(sec * NS_PER_SEC) - (D)knl_get_mono_ns();
	}

	return E_OK;
}

#if USE_SHUTDOWN
/*
 * Stop system timer
 */
EXPORT void knl_timer_shutdown( void )
{
	knl_terminate_hw_timer();
}
#endif /* USE_SHUTDOWN */

/*
 * Insert timer event to timer event queue
 */
LOCAL void knl_enqueue_tmeb( TMEB *event )
{
	QUEUE	*q;

	for ( q = knl_timer_queue.next; q != &knl_timer_queue; q = q->next ) {
		if ( event->time < ((TMEB*)q)->time ) {
			break;
		}
	}
	QueInsert(&event->queue, q);

	if ( knl_timer_queue.next == &event->queue ) {
		knl_timer_program();		/* new earliest expiry */
	}
}

/*
 * Relative timeout -> absolute expiry
 *	Rounded up to the CNF_TIMER_SLACK_NS grid: expiries that fall into
 *	the same grid cell fire on one interrupt, and a timeout is never
 *	shortened.
 */
LOCAL ABSTIM knl_timer_expiry( UD tmout_ns )
{
	ABSTIM	t = knl_get_mono_ns() + tmout_ns;

#if CNF_TIMER_SLACK_NS > 0
	t = (t + (CNF_TIMER_SLACK_NS - 1)) / CNF_TIMER_SLACK_NS * CNF_TIMER_SLACK_NS;
#endif
	return t;
}

/*
 * Set timeout event
 *	Register the timer event onto the timer queue to start after the
 *	timeout. At timeout, the callback is called with the argument.
 *	When the timeout is TMO_FEVR, do not register onto the timer queue,
 *	but initialize the queue area in case knl_timer_delete is called
 *	later.
 */
EXPORT void knl_timer_insert( TMEB *event, TMO tmout, CBACK callback, void *arg )
{
	event->callback = callback;
	event->arg = arg;

	if ( tmout == TMO_FEVR ) {
		QueInit(&event->queue);
	} else {
		event->time = knl_timer_expiry(knl_ms_to_ns((UD)(UW)tmout));
		knl_enqueue_tmeb(event);
	}
}

EXPORT void knl_timer_insert_u( TMEB *event, TMO_U tmout_u, CBACK callback, void *arg )
{
	event->callback = callback;
	event->arg = arg;

	if ( tmout_u == TMO_FEVR ) {
		QueInit(&event->queue);
	} else {
		event->time = knl_timer_expiry(knl_us_to_ns((UD)tmout_u));
		knl_enqueue_tmeb(event);
	}
}

EXPORT void knl_timer_insert_reltim( TMEB *event, RELTIM tmout, CBACK callback, void *arg )
{
	knl_timer_insert_rel_ns(event, knl_ms_to_ns((UD)tmout), callback, arg);
}

EXPORT void knl_timer_insert_reltim_u( TMEB *event, RELTIM_U tmout_u, CBACK callback, void *arg )
{
	knl_timer_insert_rel_ns(event, knl_us_to_ns((UD)tmout_u), callback, arg);
}

EXPORT void knl_timer_insert_rel_ns( TMEB *event, UD tmout_ns, CBACK callback, void *arg )
{
	event->callback = callback;
	event->arg = arg;
	event->time = knl_timer_expiry(tmout_ns);
	knl_enqueue_tmeb(event);
}

/*
 * Set time specified event
 *	Register the timer event onto the timer queue to start at the
 *	absolute monotonic time. No rounding is applied, so chained cyclic
 *	expiries do not drift.
 */
EXPORT void knl_timer_insert_abs( TMEB *evt, ABSTIM time, CBACK cback, void *arg )
{
	evt->callback = cback;
	evt->arg = arg;
	evt->time = time;
	knl_enqueue_tmeb(evt);
}

/* ------------------------------------------------------------------------ */

/*
 * System timer interrupt handler
 *	Runs when the comparator expires. Executes every event whose expiry
 *	has been reached, then programs the comparator for the next one or
 *	stops it when the queue is empty.
 */
EXPORT void knl_timer_handler( void )
{
	TMEB	*event;
	ABSTIM	cur;

	knl_timer_irq_count++;

	BEGIN_CRITICAL_SECTION;
	for (;;) {
		cur = knl_get_mono_ns();

		while ( !isQueEmpty(&knl_timer_queue) ) {
			event = (TMEB*)knl_timer_queue.next;

			if ( !knl_abstim_reached(cur, event->time) ) {
				break;
			}

			QueRemove(&event->queue);
			if ( event->callback != NULL ) {
				(*event->callback)(event->arg);
			}
		}

		/* Re-arm unconditionally: the fired expiry must not stay in the
		   comparator, and a callback may have queued a nearer event. */
		knl_timer_armed = FALSE;
		knl_timer_program();

		/* An event that became due while re-arming is run now instead
		   of waiting for the minimum-delta interrupt. */
		if ( isQueEmpty(&knl_timer_queue)
		  || !knl_abstim_reached(knl_get_mono_ns(), ((TMEB*)knl_timer_queue.next)->time) ) {
			break;
		}
	}
	END_CRITICAL_SECTION;

	knl_end_of_hw_timer_interrupt();		/* EOI */
}

/*
 * SGI_TIMER handler (PRC 1): another processor changed the head of the
 * queue. Reprogram the comparator; an expiry already reached fires at once.
 */
EXPORT void knl_timer_ipi_handler( UINT intno, UW iar )
{
	BEGIN_CRITICAL_SECTION;
	knl_timer_armed = FALSE;
	knl_timer_program();
	END_CRITICAL_SECTION;

	out_w(GICC_EOIR, iar);
}
