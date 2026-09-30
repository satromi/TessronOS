/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	klstat.h
 *	What the kernel lock costs, measured (design 8.6.2)
 *
 *	Built with make KLOCKSTAT=1 only (CNF_KLOCK_STAT). Every outermost
 *	taking of the kernel lock is counted against the place it was taken
 *	from: how often, how often it had to wait for another processor, how
 *	long it waited, and how long it was held. Times are in ticks of the
 *	generic timer (CNTFRQ_EL0 of them in a second).
 */

#ifndef __TS_KLSTAT_H__
#define __TS_KLSTAT_H__

#include <tk/typedef.h>

typedef struct {
	UBINT	pc;			/* where it was taken from; 0 for all together */
	UD	acq;			/* times taken */
	UD	cont;			/* of those, times it had to wait */
	UD	wait;			/* ticks spent waiting */
	UD	hold;			/* ticks it was held */
} T_KLSITE;

/*
 * The totals in *all, and up to n places in sites, the most waited for
 * first; answers how many places there are.
 */
IMPORT INT  knl_klock_stat( T_KLSITE *all, T_KLSITE *sites, INT n );
IMPORT void knl_klock_stat_clear( void );

#endif /* __TS_KLSTAT_H__ */
