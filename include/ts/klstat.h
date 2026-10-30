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

/*
 * How long each holding lasted, counted in bins of doubling length:
 * bin 0 below 1us, bin k from 2^(k-1) to 2^k us, the last bin 2^14us
 * (16ms) and more
 */
#define KL_HIST_N	16

typedef struct {
	UBINT	pc;			/* where it was taken from; 0 for all together */
	UD	acq;			/* times taken */
	UD	cont;			/* of those, times it had to wait */
	UD	wait;			/* ticks spent waiting */
	UD	hold;			/* ticks it was held */
	UD	wait_max;		/* the longest one wait, ticks */
	UD	hold_max;		/* the longest one holding, ticks */
	UD	hold_hist[KL_HIST_N];	/* the holdings by their length */
} T_KLSITE;

/*
 * The totals in *all, and up to n places in sites, the most waited for
 * first; answers how many places there are.
 */
IMPORT INT  knl_klock_stat( T_KLSITE *all, T_KLSITE *sites, INT n );
IMPORT void knl_klock_stat_clear( void );

/*
 * Where the big lock is not the critical section's, or not alone
 * (CNF_KLOCK_MODE 1 to 3, design 4.3.3): the same counts for each rank of
 * the kernel's locks, into cls[0..n-1] (cls[r].pc is the rank r, 1 to 6;
 * the objects' locks are one rank together). Answers the ranks there are.
 */
IMPORT INT  knl_klk_stat( T_KLSITE *cls, INT n );
IMPORT CONST char *knl_klk_rank_name( INT r );
IMPORT void knl_klk_stat_clear( void );

#endif /* __TS_KLSTAT_H__ */
