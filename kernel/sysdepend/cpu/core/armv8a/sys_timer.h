/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h
 *	System timer: Arm Generic Timer, EL1 physical timer (CNTP_*), PPI 30
 *
 *	Tickless operation: the counter (CNTPCT_EL0) is the monotonic clock
 *	and the comparator (CNTP_CVAL_EL0) is set to the next expiry only.
 *	The comparator is disabled while no event is pending.
 */

#ifndef _SYSDEPEND_CPU_CORE_SYSTIMER_
#define _SYSDEPEND_CPU_CORE_SYSTIMER_

#define CNTP_CTL_ENABLE		(1 << 0)
#define CNTP_CTL_IMASK		(1 << 1)
#define CNTP_CTL_ISTATUS	(1 << 2)

IMPORT UD	knl_hrt_base;		/* counter value at timer start (monotonic 0) */
IMPORT UD	knl_hrt_min_delta;	/* minimum distance of a new expiry (cycles) */

/*
 * Read the counter. The ISB keeps the read behind earlier instructions so
 * that a time stamp is not taken speculatively early.
 */
Inline UD knl_read_cntpct( void )
{
	UD	c;

	Asm("isb\n\tmrs %0, cntpct_el0" : "=r"(c) :: "memory");
	return c;
}

/*
 * Exact conversions (two 64 bit divisions, no overflow for any input)
 */
Inline UD knl_cycles_to_ns( UD cycles )
{
	UD	frq = knl_cntfrq;

	return (cycles / frq) * 1000000000ULL + ((cycles % frq) * 1000000000ULL) / frq;
}

Inline UD knl_ns_to_cycles_up( UD ns )
{
	UD	frq = knl_cntfrq;

	return (ns / 1000000000ULL) * frq + ((ns % 1000000000ULL) * frq + 999999999ULL) / 1000000000ULL;
}

/*
 * Start the system timer: clock base, minimum delta, interrupt line.
 * The comparator stays disabled until an event is queued.
 */
Inline void knl_start_hw_timer( void )
{
	UINT	imask;
	UD	frq;

	DI(imask);

	Asm("mrs %0, cntfrq_el0" : "=r"(frq));
	knl_cntfrq = frq;
	knl_hrt_base = knl_read_cntpct();
	knl_hrt_min_delta = frq / 1000000ULL + 1;	/* about 1us */

	Asm("msr cntp_ctl_el0, %0" :: "r"((UD)0));
	Asm("isb");

	EnableInt(INTNO_SYSTICK, INTPRI_SYSTICK);

	EI(imask);
}

/*
 * Program the comparator for an absolute monotonic expiry (ns).
 *	The distance is computed from one counter read so that the
 *	conversion error is relative to the distance, not to the uptime.
 *	A past or too near expiry is moved to now + minimum delta.
 */
Inline void knl_set_hw_timer( UD expire_ns )
{
	UD	now_c, now_ns, delta_c;

	now_c = knl_read_cntpct();
	now_ns = knl_cycles_to_ns(now_c - knl_hrt_base);
	delta_c = ( expire_ns > now_ns ) ? knl_ns_to_cycles_up(expire_ns - now_ns) : 0;
	if ( delta_c < knl_hrt_min_delta ) {
		delta_c = knl_hrt_min_delta;
	}

	Asm("msr cntp_cval_el0, %0" :: "r"(now_c + delta_c));
	Asm("msr cntp_ctl_el0, %0" :: "r"((UD)CNTP_CTL_ENABLE));
	Asm("isb");
}

/*
 * Stop the comparator (no pending event)
 */
Inline void knl_stop_hw_timer( void )
{
	Asm("msr cntp_ctl_el0, %0" :: "r"((UD)0));
	Asm("isb");
}

/*
 * End of the timer interrupt (EOI to the GIC)
 */
Inline void knl_end_of_hw_timer_interrupt( void )
{
	disint();	/* keep interrupts masked across the EOI */
	EndOfInt(INTNO_SYSTICK);
}

/*
 * Stop the system timer
 */
Inline void knl_terminate_hw_timer( void )
{
	knl_stop_hw_timer();
	DisableInt(INTNO_SYSTICK);
}

#endif /* _SYSDEPEND_CPU_CORE_SYSTIMER_ */
