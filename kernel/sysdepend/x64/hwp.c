/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hwp.c
 *	The processors' clock chosen by the processor itself to the load:
 *	Hardware P-states (HWP) turned on, with the range and the energy
 *	preference of the "powersave" policy, and the energy bias (design
 *	03-platform 3.10.14)
 *
 *	Only where CPUID 6 says HWP is there, with an energy preference (EPP)
 *	or Dynamic Efficiency Control (MSR_IA32_POWER_CTL bit 27), not on a
 *	virtual machine, and not on a hybrid processor (its two kinds of core
 *	count performance on different scales, which this does not convert).
 *	A processor without HWP is left at the clock steps
 *	the firmware left (IA32_PERF_CTL), as before. The start-up setting
 *	"intel_pstate=no_hwp" leaves HWP and the bias as the firmware set
 *	them.
 *
 *	Each processor sets its own registers, in its idle loop (cpuidle.c
 *	asks knl_hwp_cpu_apply once a setting is made, and again after a
 *	sleep), in this order:
 *	 1. the energy bias (IA32_ENERGY_PERF_BIAS): 0 ("performance") is
 *	    taken as never set by the firmware and made "normal" (6, or 7 on
 *	    the processors whose normal is the power saving one);
 *	 2. the HWP notification interrupt off (MSR_HWP_INTERRUPT 0), HWP on
 *	    (IA32_PM_ENABLE 1), which cannot be turned off again until reset;
 *	 3. the default EPP: what the request holds once HWP is on, unless
 *	    the processor's model has its own balance_performance value, which
 *	    is then written; when the firmware had turned HWP on with an EPP
 *	    between performance and 0x80, that value is kept;
 *	 4. the request (IA32_HWP_REQUEST): its minimum the most efficient
 *	    ratio (MSR_PLATFORM_INFO 47:40, or the percentage of the highest
 *	    it is, rounded up, when that is more), its maximum the highest
 *	    performance (the guaranteed one when turbo is off in
 *	    IA32_MISC_ENABLE), the desired performance and the EPP as they are.
 *	The request is the same on every processor: they are of one kind.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <ts/board.h>
#include "sysdepend.h"

#define MSR_PLATFORM_INFO	0x0ce
#define MSR_MISC_ENABLE		0x1a0
#define MSR_ENERGY_PERF_BIAS	0x1b0
#define MSR_POWER_CTL		0x1fc
#define MSR_PM_ENABLE		0x770
#define MSR_HWP_CAPABILITIES	0x771
#define MSR_HWP_INTERRUPT	0x773
#define MSR_HWP_REQUEST		0x774

#define CPUID6_HWP		( 1U << 7 )
#define CPUID6_HWP_NOTIFY	( 1U << 8 )
#define CPUID6_HWP_EPP		( 1U << 10 )
#define CPUID6_ECX_EPB		( 1U << 3 )
#define POWER_CTL_DEC		( 1ULL << 27 )
#define MISC_TURBO_OFF		( 1ULL << 38 )

#define EPB_PERFORMANCE		0
#define EPB_NORMAL		6
#define EPB_NORMAL_POWERSAVE	7
#define EPP_PERFORMANCE		0x00
#define EPP_BALANCE_PERFORMANCE	0x80

#define HWP_HIGHEST(cap)	( (UW)( (cap) & 0xff ) )
#define HWP_GUARANTEED(cap)	( (UW)( ( (cap) >> 8 ) & 0xff ) )

LOCAL BOOL	hwp_on = FALSE;		/* the processors are to have HWP on */
LOCAL BOOL	hwp_forced = FALSE;	/* the firmware had turned it on */
LOCAL BOOL	has_notify = FALSE, has_epb = FALSE;
LOCAL UW	epb_normal = EPB_NORMAL;
LOCAL INT	epp_balance_perf = EPP_BALANCE_PERFORMANCE;

/* The models whose "normal" bias is the power saving one */
LOCAL CONST UB	epb_normal_powersave[] = { 0x9a, 0xbe, 0xba };	/* Alder Lake L, Gracemont, Raptor Lake P */

/* The models with their own balance_performance EPP */
LOCAL CONST struct {
	UB	model;
	UB	epp;
} epp_default[] = {
	{ 0x9a, 102 },		/* Alder Lake L */
	{ 0x8f, 32 },		/* Sapphire Rapids X */
	{ 0xcf, 32 },		/* Emerald Rapids X */
	{ 0xad, 32 },		/* Granite Rapids X */
	{ 0xae, 32 },		/* Granite Rapids D */
	{ 0xaa, 64 },		/* Meteor Lake L */
	{ 0xc6, 64 },		/* Arrow Lake */
};

LOCAL UW cpu_model( void )
{
	UW	a, b, c, d;

	cpuid(1, 0, &a, &b, &c, &d);
	if ( ( ( a >> 8 ) & 0xf ) != 6 ) return 0;
	return ( ( a >> 4 ) & 0xf ) | ( ( a >> 12 ) & 0xf0 );
}

/* The request this processor's capabilities give */
LOCAL UD hwp_request( UD req, UD cap )
{
	UD	pi = rdmsr(MSR_PLATFORM_INFO);
	INT	min_pstate = (INT)( ( pi >> 40 ) & 0xff );
	INT	turbo = (INT)HWP_HIGHEST(cap), max, min, pct, gmin;

	max = ( rdmsr(MSR_MISC_ENABLE) & MISC_TURBO_OFF ) ? (INT)HWP_GUARANTEED(cap) : turbo;
	pct = ( turbo != 0 ) ? min_pstate * 100 / turbo : 0;
	gmin = ( turbo * pct + 99 ) / 100;
	if ( gmin > turbo ) gmin = turbo;
	min = ( min_pstate > gmin ) ? min_pstate : gmin;
	if ( min > max ) min = max;
	req &= ~0xffffULL;
	return req | (UD)(UW)min | ( (UD)(UW)max << 8 );
}

/* This processor, from its idle loop with interrupts disabled */
EXPORT void knl_hwp_cpu_apply( void )
{
	UD	epb, req, cap;
	INT	epp;

	if ( !hwp_on ) return;
	if ( has_epb ) {
		epb = rdmsr(MSR_ENERGY_PERF_BIAS);
		if ( ( epb & 0xf ) == EPB_PERFORMANCE ) {
			wrmsr(MSR_ENERGY_PERF_BIAS, ( epb & ~0xfULL ) | epb_normal);
		}
	}
	if ( has_notify ) wrmsr(MSR_HWP_INTERRUPT, 0);
	wrmsr(MSR_PM_ENABLE, 1);
	req = rdmsr(MSR_HWP_REQUEST);
	epp = (INT)( ( req >> 24 ) & 0xff );
	if ( !( hwp_forced && epp <= EPP_BALANCE_PERFORMANCE && epp > EPP_PERFORMANCE )
	  && epp_balance_perf != EPP_BALANCE_PERFORMANCE ) {
		req = ( req & ~( 0xffULL << 24 ) ) | ( (UD)(UW)epp_balance_perf << 24 );
	}
	cap = rdmsr(MSR_HWP_CAPABILITIES);
	wrmsr(MSR_HWP_REQUEST, hwp_request(req, cap));
}

/* What this processor's HWP is set to, for the records: its request and capabilities, or why there are none */
EXPORT INT knl_hwp_text( char *t )
{
	if ( !hwp_on ) return tm_sprintf((UB *)t, (CONST UB *)"; HWP not set by the kernel");
	return tm_sprintf((UB *)t, (CONST UB *)"; HWP request %08x%08x capabilities %08x, energy bias %d",
			  (UW)( rdmsr(MSR_HWP_REQUEST) >> 32 ), (UW)rdmsr(MSR_HWP_REQUEST),
			  (UW)rdmsr(MSR_HWP_CAPABILITIES), (INT)( rdmsr(MSR_ENERGY_PERF_BIAS) & 0xf ));
}

/*
 * Whether HWP is to be turned on, decided once on PRC 1 before the
 * processors apply it (cpuidle.c). TRUE when it is.
 */
EXPORT BOOL knl_hwp_init( void )
{
	UW	a, b, c, d, model;
	UB	v[16];
	INT	i;
	BOOL	epp, dec;

	cpuid(0, 0, &a, &b, &c, &d);
	/* "GenuineIntel" */
	if ( a < 6 || b != 0x756e6547U || d != 0x49656e69U || c != 0x6c65746eU ) return FALSE;
	cpuid(1, 0, &a, &b, &c, &d);
	if ( c & 0x80000000U ) {
		tm_printf((UB *)"hwp: a virtual machine: the clock is the host's\n");
		return FALSE;
	}
	cpuid(6, 0, &a, &b, &c, &d);
	if ( ( a & CPUID6_HWP ) == 0 ) {
		tm_printf((UB *)"hwp: no HWP: the firmware's clock steps, as before\n");
		return FALSE;
	}
	cpuid(7, 0, &a, &b, &c, &d);
	if ( d & ( 1U << 15 ) ) {
		/* performance and ratios are of different scales on the two kinds of core */
		tm_printf((UB *)"hwp: a hybrid processor: HWP left as the firmware set it\n");
		return FALSE;
	}
	cpuid(6, 0, &a, &b, &c, &d);
	if ( knl_bootarg(knl_bootargs, "intel_pstate", v, sizeof(v)) && knl_strcmp((char *)v, "no_hwp") == 0 ) {
		tm_printf((UB *)"hwp: intel_pstate=no_hwp: HWP and the energy bias left as the firmware set them\n");
		return FALSE;
	}
	epp = (BOOL)( ( a & CPUID6_HWP_EPP ) != 0 );
	dec = (BOOL)( ( rdmsr(MSR_POWER_CTL) & POWER_CTL_DEC ) != 0 );
	hwp_forced = (BOOL)( ( rdmsr(MSR_PM_ENABLE) & 1 ) != 0 );
	if ( !hwp_forced && !epp && !dec ) {
		tm_printf((UB *)"hwp: HWP without an energy preference: not turned on\n");
		return FALSE;
	}
	has_notify = (BOOL)( ( a & CPUID6_HWP_NOTIFY ) != 0 );
	has_epb = (BOOL)( ( c & CPUID6_ECX_EPB ) != 0 );
	model = cpu_model();
	for ( i = 0; i < (INT)sizeof(epb_normal_powersave); i++ ) {
		if ( model == epb_normal_powersave[i] ) epb_normal = EPB_NORMAL_POWERSAVE;
	}
	for ( i = 0; i < (INT)( sizeof(epp_default) / sizeof(epp_default[0]) ); i++ ) {
		if ( model == epp_default[i].model ) epp_balance_perf = epp_default[i].epp;
	}
	hwp_on = TRUE;
	tm_printf((UB *)"hwp: HWP to be on (%s), the powersave policy: minimum the most efficient ratio %d, maximum %s; "
		  "energy bias %d when the firmware left 0%s; balance_performance EPP %d%s\n",
		  hwp_forced ? "the firmware had turned it on" : "turned on by the kernel",
		  (INT)( ( rdmsr(MSR_PLATFORM_INFO) >> 40 ) & 0xff ),
		  ( rdmsr(MSR_MISC_ENABLE) & MISC_TURBO_OFF ) ? "the guaranteed (turbo off)" : "the highest",
		  (INT)epb_normal, has_epb ? "" : " (no bias register)", epp_balance_perf,
		  epp ? "" : " (no energy preference)");
	return TRUE;
}

#endif /* X64 */
