/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys_timer.c (x86-64)
 *	The monotonic clock and the rate of the time stamp counter.
 *
 *	The rate is what CPUID leaf 15h says when it says it all (the
 *	crystal's rate and the ratio to it); otherwise it is measured, over
 *	50ms, against the HPET, else the ACPI PM timer, else channel 2 of
 *	the 8254. A chipset may have any of them stopped or gated (a counter
 *	that does not move, a line that never rises): each is given up after
 *	far more than its 50ms by the time stamp counter itself, and the next
 *	is asked. When none answers, CPUID leaf 16h's base clock is taken,
 *	which on a processor without leaf 15h's rate is at least near it, so
 *	that the start goes on and says so.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include <tm/tmonitor.h>
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

#define CAL_MS		50
#define CAL_GIVE_UP	( (UD)1 << 32 )		/* counts of the TSC: about a second, whatever its rate */

/* The HPET's main counter: its period (fs) from its capabilities */
LOCAL UD cal_hpet( void )
{
	UBINT	h = DEV_BASE(knl_acpi.hpet_addr);
	UD	period = in_w(h + 0x04), ticks, t0, t1;
	UW	c0, c1, done = 0;

	if ( period == 0 || period > 100000000ULL ) {
		return 0;
	}
	/*
	 * Only the low half of the counter, a word at a time: the chipsets of
	 * AMD's processors have a counter of 32 bits, and it is enough for
	 * the 50 ms whichever the width. The subtraction takes a wrap.
	 */
	out_w(h + 0x10, in_w(h + 0x10) | 1);		/* the counter running */
	ticks = (UD)CAL_MS * 1000000000000ULL / period;
	c0 = in_w(h + 0xf0);
	t0 = ReadSysCounterSync();
	do {
		c1 = in_w(h + 0xf0);
		done = c1 - c0;
		t1 = ReadSysCounterSync();
		if ( t1 - t0 > CAL_GIVE_UP ) {
			return 0;
		}
	} while ( done < ticks );
	t1 = ReadSysCounterSync();
	return (UD)( (unsigned __int128)( t1 - t0 ) * 1000000000000000ULL / ( (UD)done * period ) );
}

/* The ACPI PM timer: 3.579545 MHz, 24 or 32 bits */
LOCAL UD cal_pmtmr( void )
{
	UH	port = (UH)knl_acpi.pm_tmr;
	UW	mask = knl_acpi.pm_tmr32 ? 0xffffffffU : 0x00ffffffU;
	UW	c0, c, last;
	UD	done = 0, want = 3579545ULL * CAL_MS / 1000, t0, t1;

	c0 = last = in_port_w(port) & mask;
	t0 = ReadSysCounterSync();
	while ( done < want ) {
		c = in_port_w(port) & mask;
		done += ( c - last ) & mask;
		last = c;
		if ( ReadSysCounterSync() - t0 > CAL_GIVE_UP ) {
			return 0;
		}
	}
	t1 = ReadSysCounterSync();
	(void)c0;
	return ( t1 - t0 ) * 3579545ULL / done;
}

/* Channel 2 of the 8254, gated by port 61h, counting down from 1.193182 MHz */
LOCAL UD cal_pit( void )
{
	UW	n = 1193182 * CAL_MS / 1000;
	UD	t0, t1;
	UB	g;

	g = in_port_b(0x61);
	out_port_b(0x61, ( g & ~0x02 ) | 0x01);		/* gate on, speaker off */
	out_port_b(0x43, 0xb0);				/* channel 2, lo/hi, mode 0 */
	out_port_b(0x42, (UB)n);
	out_port_b(0x42, (UB)( n >> 8 ));
	t0 = ReadSysCounterSync();
	while ( ( in_port_b(0x61) & 0x20 ) == 0 ) {	/* OUT2 goes high at the count's end */
		if ( ReadSysCounterSync() - t0 > CAL_GIVE_UP ) {
			out_port_b(0x61, g);
			return 0;
		}
	}
	t1 = ReadSysCounterSync();
	out_port_b(0x61, g);
	return ( t1 - t0 ) * 1000 / CAL_MS;
}

EXPORT void knl_tsc_calibrate( void )
{
	UW		a, b, c, d;
	UD		f = 0;
	CONST char	*how = "CPUID";

	cpuid(0, 0, &a, &b, &c, &d);
	if ( a >= 0x15 ) {
		cpuid(0x15, 0, &a, &b, &c, &d);
		if ( a != 0 && b != 0 && c != 0 ) {
			f = (UD)c * b / a;
		}
	}
	if ( f == 0 && knl_acpi.hpet_addr != 0 ) {
		f = cal_hpet();
		how = "HPET";
	}
	if ( f == 0 && knl_acpi.pm_tmr != 0 ) {
		f = cal_pmtmr();
		how = "PM timer";
	}
	if ( f == 0 ) {
		f = cal_pit();
		how = "8254";
	}
	if ( f == 0 ) {
		cpuid(0, 0, &a, &b, &c, &d);
		if ( a >= 0x16 ) {
			cpuid(0x16, 0, &a, &b, &c, &d);
			f = (UD)( a & 0xffff ) * 1000000ULL;
			how = "CPUID 16h, none of the timers answered";
		}
	}
	if ( f == 0 ) {
		f = 1000000000ULL;
		how = "a guess, nothing answered";
	}
	knl_cntfrq = f;
	cpuid(0x80000000, 0, &a, &b, &c, &d);
	if ( a >= 0x80000007 ) {
		cpuid(0x80000007, 0, &a, &b, &c, &d);
	} else {
		d = 0;
	}
	tm_printf((UB *)"TessronOS x64: TSC %d kHz (%s)%s\n", (UW)( f / 1000 ), how,
		  ( d & ( 1U << 8 ) ) ? ", invariant" : ", NOT invariant");
	{
		/* The processor, for the records of a machine that has not been tried */
		UW	v[3], sig, c1, c7b, x;

		cpuid(0, 0, &a, &v[0], &v[2], &v[1]);
		cpuid(1, 0, &sig, &b, &c1, &d);
		cpuid(7, 0, &a, &c7b, &c, &d);
		cpuid(0x80000000, 0, &x, &b, &c, &d);
		tm_printf((UB *)"TessronOS x64: CPU %.12s, signature %08x (family %x model %x), CPUID 1 ECX %08x, "
			  "7 EBX %08x, last extended leaf %08x\n", (UB *)v, sig,
			  ( ( sig >> 8 ) & 0xf ) + ( ( sig >> 20 ) & 0xff ),
			  ( ( sig >> 4 ) & 0xf ) | ( ( sig >> 12 ) & 0xf0 ), c1, c7b, x);
	}
}

#endif /* CPU_CORE_X86_64 */
