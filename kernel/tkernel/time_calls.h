/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08 / TessronOS
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	time_calls.h
 *	Time Management Function
 *	TessronOS: cyclic and alarm handlers keep their times in nanoseconds
 *	of the monotonic clock (design 7.4.2).
 */

#ifndef _TIME_CALLS_H
#define _TIME_CALLS_H

#define	DIFF_TRON_UTC		(473385600000LL)		/* ms from 1970-01-01 to 1985-01-01 */
#define	DIFF_TRON_UTC_NS	(DIFF_TRON_UTC * 1000000LL)

/*
 * Cyclic handler control block
 */
typedef struct cyclic_handler_control_block {
	void	*exinf;		/* Extended information */
	ATR	cycatr;		/* Cyclic handler attribute */
	FP	cychdr;		/* Cyclic handler address */
	UINT	cycstat;	/* Cyclic handler state */
	UD	cyctim;		/* Cyclic time (ns) */
	TMEB	cyctmeb;	/* Timer event block */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* name */
#endif
} CYCCB;

IMPORT CYCCB	knl_cyccb_table[];	/* Cyclic handler control block */
IMPORT QUEUE	knl_free_cyccb;	/* FreeQue */

#define get_cyccb(id)	( &knl_cyccb_table[INDEX_CYC(id)] )

/*
 * Next startup time: the previous expiry plus the period, moved past the
 * current time by whole periods (no drift).
 */
Inline ABSTIM knl_cyc_next_time( CYCCB *cyccb )
{
	ABSTIM		tm, cur;

	cur = knl_get_mono_ns();
	tm = cyccb->cyctmeb.time + cyccb->cyctim;
	if ( knl_abstim_reached(cur, tm) ) {
		tm = ((cur - cyccb->cyctmeb.time) / cyccb->cyctim + 1) * cyccb->cyctim + cyccb->cyctmeb.time;
	}

	return tm;
}

IMPORT void knl_call_cychdr( CYCCB* cyccb );

/*
 * Register timer event queue
 */
Inline void knl_cyc_timer_insert( CYCCB *cyccb, ABSTIM tm )
{
	knl_timer_insert_abs(&cyccb->cyctmeb, tm, (CBACK)knl_call_cychdr, cyccb);
}

/*
 * Alarm handler control block
 */
typedef struct alarm_handler_control_block {
	void	*exinf;		/* Extended information */
	ATR	almatr;		/* Alarm handler attribute */
	FP	almhdr;		/* Alarm handler address */
	UINT	almstat;	/* Alarm handler state */
	TMEB	almtmeb;	/* Timer event block */
#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* name */
#endif
} ALMCB;

IMPORT ALMCB	knl_almcb_table[];	/* Alarm handler control block */
IMPORT QUEUE	knl_free_almcb;	/* FreeQue */

#define get_almcb(id)	( &knl_almcb_table[INDEX_ALM(id)] )

IMPORT void knl_call_almhdr( ALMCB *almcb );

/*
 * Register onto timer event queue (relative time in ns)
 */
Inline void knl_alm_timer_insert( ALMCB *almcb, UD reltim_ns )
{
	knl_timer_insert_rel_ns(&almcb->almtmeb, reltim_ns, (CBACK)knl_call_almhdr, almcb);
}

#endif /* _TIME_CALLS_H */
