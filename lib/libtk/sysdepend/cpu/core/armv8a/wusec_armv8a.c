/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wusec_armv8a.c
 *	Busy wait using the Generic Timer counter (CNTVCT_EL0, CNTVOFF = 0)
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include <tk/tkernel.h>

LOCAL UD read_cnt( void )
{
	UD	cnt;
	Asm("isb; mrs %0, cntvct_el0" : "=r"(cnt));
	return cnt;
}

LOCAL void wait_ticks( UD ticks )
{
	UD	start = read_cnt();
	while ( (read_cnt() - start) < ticks ) {
		;
	}
}

EXPORT void WaitUsec( UW usec )
{
	UD	frq;
	Asm("mrs %0, cntfrq_el0" : "=r"(frq));
	wait_ticks(((UD)usec * frq) / 1000000ULL);
}

EXPORT void WaitNsec( UW nsec )
{
	UD	frq;
	Asm("mrs %0, cntfrq_el0" : "=r"(frq));
	wait_ticks(((UD)nsec * frq) / 1000000000ULL + 1);
}

#endif /* CPU_CORE_ARMV8A */
