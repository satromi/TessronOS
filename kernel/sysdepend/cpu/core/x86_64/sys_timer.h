/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.h (x86-64)
 *	Hardware-dependent timer processing.
 *
 *	Tickless operation: the invariant time stamp counter is the
 *	monotonic clock, and the Local APIC timer is set to the next expiry
 *	only: in TSC deadline mode the expiry itself (IA32_TSC_DEADLINE),
 *	else a one shot count to it (apic.c). It is stopped while no event
 *	is pending.
 */

#ifndef _SYSDEPEND_CPU_CORE_SYSTIMER_
#define _SYSDEPEND_CPU_CORE_SYSTIMER_

IMPORT UD	knl_hrt_base;		/* counter value at timer start (monotonic 0) */
IMPORT UD	knl_hrt_min_delta;	/* minimum distance of a new expiry (cycles) */
IMPORT void	knl_tsc_calibrate( void );	/* knl_cntfrq measured (sys_timer.c) */

/*
 * Read the counter. LFENCE keeps the read behind earlier instructions so
 * that a time stamp is not taken speculatively early.
 */
Inline UD knl_read_cntpct( void )
{
	return ReadSysCounterSync();
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
 * The timer stays stopped until an event is queued.
 */
Inline void knl_start_hw_timer( void )
{
	UINT	imask;

	DI(imask);

	knl_hrt_base = knl_read_cntpct();
	knl_hrt_min_delta = knl_cntfrq / 1000000ULL + 1;	/* about 1us */
	knl_apic_timer_deadline(0);

	EnableInt(INTNO_SYSTICK, INTPRI_SYSTICK);

	EI(imask);
}

/*
 * Program the timer for an absolute monotonic expiry (ns).
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
	knl_apic_timer_deadline(now_c + delta_c);
}

/*
 * Stop the timer (no pending event)
 */
Inline void knl_stop_hw_timer( void )
{
	knl_apic_timer_deadline(0);
}

/*
 * End of the timer interrupt (EOI to the Local APIC)
 */
Inline void knl_end_of_hw_timer_interrupt( void )
{
	disint();	/* keep interrupts masked across the EOI */
	EndOfInt(INTNO_SYSTICK);
}

/*
 * The Local APIC timer is per processor and PRC 1's serves the queue:
 * another processor that changed the head of the queue asks PRC 1 to
 * program it again (SGI_TIMER), and PRC 1's handler ends that interrupt
 */
Inline void knl_timer_ipi_request( void )
{
	knl_send_ipi_one(0, SGI_TIMER);
}

Inline void knl_end_of_timer_ipi( UW iar )
{
	(void)iar;
	knl_apic_eoi();
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
