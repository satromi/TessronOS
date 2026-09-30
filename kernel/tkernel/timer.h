/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	timer.h
 *	Timer control: tickless high-resolution timer (design 7.4)
 *
 *	Every internal time is a nanosecond count of the monotonic clock,
 *	which is read from the Arm Generic Timer counter. Timer events are
 *	kept in one queue sorted by expiry; the hardware comparator is
 *	programmed with the earliest expiry only, so there is no periodic
 *	tick. Relative timeouts are rounded up to a CNF_TIMER_SLACK_NS grid
 *	so that neighbouring expiries share one interrupt.
 */

#ifndef _TIMER_
#define _TIMER_

#include "longlong.h"

/*
 * SYSTIM internal expression (milliseconds, 64 bit)
 */
typedef	D	LSYSTIM;

Inline LSYSTIM knl_toLSYSTIM( CONST SYSTIM *time )
{
	LSYSTIM		ltime;

	hilo_ll(ltime, time->hi, time->lo);

	return ltime;
}

Inline SYSTIM knl_toSYSTIM( LSYSTIM ltime )
{
	SYSTIM		time;

	ll_hilo(time.hi, time.lo, ltime);

	return time;
}

/*
 * Absolute time: monotonic clock in nanoseconds (0 at timer start)
 */
typedef	UD	ABSTIM;

#define NS_PER_US	(1000ULL)
#define NS_PER_MS	(1000000ULL)
#define NS_PER_SEC	(1000000000ULL)

/* Relative timeouts longer than this are clamped (about 146 years) */
#define KNL_TMOUT_NS_MAX	(0x3FFFFFFFFFFFFFFFULL)

Inline BOOL knl_abstim_reached( ABSTIM curtim, ABSTIM evttim )
{
	return curtim >= evttim;
}

/* TMO (ms) -> TMO_U (us), keeping TMO_FEVR */
#define knl_tmo_ms2us(t)	( ((t) == TMO_FEVR) ? (TMO_U)TMO_FEVR : (TMO_U)(t) * 1000 )

/* Milliseconds / microseconds to nanoseconds with clamping */
Inline UD knl_ms_to_ns( UD ms )
{
	return ( ms > KNL_TMOUT_NS_MAX / NS_PER_MS ) ? KNL_TMOUT_NS_MAX : ms * NS_PER_MS;
}

Inline UD knl_us_to_ns( UD us )
{
	return ( us > KNL_TMOUT_NS_MAX / NS_PER_US ) ? KNL_TMOUT_NS_MAX : us * NS_PER_US;
}

/*
 * Timer event block
 */
typedef void	(*CBACK)(void *);	/* Type of callback function */

typedef struct timer_event_block {
	QUEUE	queue;		/* Timer event queue */
	ABSTIM	time;		/* Expiry (monotonic ns) */
	CBACK	callback;	/* Callback function */
	void	*arg;		/* Argument to be sent to callback function */
} TMEB;

IMPORT D	knl_real_time_ofs;	/* System time (UTC ns) - monotonic ns */
IMPORT QUEUE	knl_timer_queue;
IMPORT UD	knl_timer_irq_count;	/* Timer interrupts taken (diagnostics) */

IMPORT UD	knl_get_mono_ns( void );	/* Monotonic clock (ns) */

/*
 * Register a timer event
 *	tmout: TMO (ms) / TMO_U (us); TMO_FEVR does not register the event
 *	but initializes it so that knl_timer_delete() can be called later.
 *	reltim: RELTIM (ms) / RELTIM_U (us), always registered.
 *	rel_ns: relative nanoseconds, rounded up to the slack grid.
 *	abs: absolute expiry, registered as given (no rounding).
 */
IMPORT void knl_timer_insert( TMEB *evt, TMO tmout, CBACK cback, void *arg );
IMPORT void knl_timer_insert_u( TMEB *evt, TMO_U tmout_u, CBACK cback, void *arg );
IMPORT void knl_timer_insert_reltim( TMEB *evt, RELTIM tmout, CBACK cback, void *arg );
IMPORT void knl_timer_insert_reltim_u( TMEB *evt, RELTIM_U tmout_u, CBACK cback, void *arg );
IMPORT void knl_timer_insert_rel_ns( TMEB *evt, UD tmout_ns, CBACK cback, void *arg );
IMPORT void knl_timer_insert_abs( TMEB *evt, ABSTIM time, CBACK cback, void *arg );

/*
 * Delete a timer event. A stale hardware expiry only causes one extra
 * interrupt that finds nothing due, so the comparator is left alone.
 */
Inline void knl_timer_delete( TMEB *event )
{
	QueRemove(&event->queue);
}

/*
 * Remaining time of a registered event (0 if already due)
 */
Inline UD knl_timer_left_ns( ABSTIM time, ABSTIM cur )
{
	return ( time > cur ) ? time - cur : 0;
}

#endif /* _TIMER_ */
