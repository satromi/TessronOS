/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	time.h
 *	TessronOS time services (design 7.3)
 */

#ifndef __TS_TIME_H__
#define __TS_TIME_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Monotonic clock: nanoseconds since the kernel started. Never set back,
 * unaffected by tk_set_tim.
 */
IMPORT ER ts_get_mono( UD *p_ns );

/*
 * The processors: how many are online, and the time each has spent
 * running tasks since the kernel started, in counts of the generic
 * timer (freq a second). A processor's load over a while is what its
 * busy count grew by against what now grew by.
 *
 * The caller gives the size of what pk points at, so that a machine of
 * any number of processors can be told: busy[] has as many processors
 * as that size holds after the header (TS_TSCPU_SIZE(n) for n of them;
 * sizeof(T_TSCPU) for none, to learn the number alone). The answer is
 * the number of processors online, or an error.
 */
typedef struct {
	INT	ncpu;			/* processors online */
	UW	freq;			/* the counts a second */
	UD	now;			/* the timer's count now */
	UD	busy[];			/* each processor's counts spent running tasks, as many as fit */
} T_TSCPU;

#define TS_TSCPU_SIZE(n)	( (SZ)sizeof(T_TSCPU) + (SZ)(n) * (SZ)sizeof(UD) )

IMPORT INT ts_ref_cpu( T_TSCPU *pk, SZ size );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TIME_H__ */
