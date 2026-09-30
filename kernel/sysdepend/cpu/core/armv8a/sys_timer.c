/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.c (ARMv8-A / AArch64)
 *	Monotonic clock from the Generic Timer counter.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "sys_timer.h"

EXPORT UD	knl_hrt_base = 0;
EXPORT UD	knl_hrt_min_delta = 64;

/*
 * Monotonic clock: nanoseconds since the timer was started
 */
EXPORT UD knl_get_mono_ns( void )
{
	return knl_cycles_to_ns(knl_read_cntpct() - knl_hrt_base);
}

#endif /* CPU_CORE_ARMV8A */
