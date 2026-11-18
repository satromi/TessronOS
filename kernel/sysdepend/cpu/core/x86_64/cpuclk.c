/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpuclk.c
 *	The processors' clock: the limit of the machine's settings and the
 *	fan's slowing, given to the firmware (include/ts/cpuclk.h, design
 *	3.7)
 *
 *	Three things say how fast the processors may run: the firmware, the
 *	most and the least it allows; the machine's setting CPU_MHZ, which
 *	the person sets with システム環境設定; and the fan's task, which slows
 *	them when it cannot keep them cool. The clock asked for is the least
 *	of the first two's most and the fan's, and not below the firmware's
 *	least, so the fan's slowing never lifts the clock above the limit,
 *	and its end brings it back to the limit, not to the most.
 *
 *	Until the settings are read the limit is CPU_MHZ_DEFAULT: the start
 *	never runs the processors faster than that, and a machine whose
 *	settings say more is brought up to it as they are read.
 *
 *	On a PC the clock is the processor's to choose: where it has HWP the
 *	kernel turns it on with the range of the powersave policy and the
 *	energy bias (kernel/sysdepend/x64/hwp.c), and the DPTF passive table
 *	may set the package's PL1 (acpi_therm.c); nothing here writes the
 *	registers that set the clock (IA32_PERF_CTL, the speed step and turbo
 *	bits of IA32_MISC_ENABLE), and the machine's setting CPU_MHZ is not
 *	used (システム環境設定 does not offer it on a PC). What HWP is set to
 *	is read once, on a machine that is not a virtual one, and told on
 *	the console. Calls are one at a time, the
 *	limit and the fan coming from different tasks: the clock asked for
 *	and what the firmware took belong together.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <ts/cpuclk.h>

#define MHZ		1000000U
#define LIMIT_MHZ_MAX	4000		/* above any clock the firmware allows; UW keeps Hz below 4.29GHz */

/* No firmware to ask */
WEAK_FUNC EXPORT BOOL knl_cpu_hw_range( UW *p_min, UW *p_max )
{
	(void)p_min;
	(void)p_max;
	return FALSE;
}

WEAK_FUNC EXPORT ER knl_cpu_hw_set( UW hz, UW *p_got )
{
	(void)hz;
	(void)p_got;
	return E_NOSPT;
}

WEAK_FUNC EXPORT ER knl_cpu_hw_now( UW *p_hz )
{
	(void)p_hz;
	return E_NOSPT;
}

WEAK_FUNC EXPORT ER knl_cpu_hw_throttled( UW *p_val )
{
	(void)p_val;
	return E_NOSPT;
}

LOCAL struct {
	UW	limit_mhz;
	UW	heat_hz;
	UW	want_hz;		/* what was last asked; 0 nothing yet */
	UW	got_hz;			/* what the firmware answered it */
	UW	min_hz, max_hz;		/* asked once */
	BOOL	ranged;
	BOOL	told;			/* a limit of the settings told on the console */
	UW	taken, applied;
} clk = { CPU_MHZ_DEFAULT, 0, 0, 0, 0, 0, FALSE, FALSE, 0, 0 };

LOCAL T_SPLOCK	clk_lock;

LOCAL void clk_take( void )
{
	while ( !SpinTryLock(&clk_lock) ) {
		if ( tk_dly_tsk(1) != E_OK ) {
			CpuRelax();		/* not in a task: no one else holds it */
		}
	}
}

EXPORT UW knl_cpu_want( UW limit_mhz, UW heat_hz, UW min_hz, UW max_hz )
{
	UW	want = max_hz;

	if ( limit_mhz > LIMIT_MHZ_MAX ) {
		limit_mhz = LIMIT_MHZ_MAX;
	}
	if ( limit_mhz != 0 && ( want == 0 || limit_mhz * MHZ < want ) ) {
		want = limit_mhz * MHZ;
	}
	if ( heat_hz != 0 && ( want == 0 || heat_hz < want ) ) {
		want = heat_hz;
	}
	if ( min_hz != 0 && want != 0 && want < min_hz ) {
		want = min_hz;
	}
	return want;
}

LOCAL void range_once( void )
{
	if ( !clk.ranged ) {
		clk.ranged = knl_cpu_hw_range(&clk.min_hz, &clk.max_hz);
	}
}

/*
 * The clock set as the limit and the fan say, with the lock held. The
 * firmware is asked only when what is asked for changes: the settings
 * are taken again whenever any line of them is written. FALSE when
 * there is no firmware.
 */
LOCAL BOOL clk_apply( void )
{
	UW	want, got = 0;

	range_once();
	if ( !clk.ranged ) {
		return FALSE;
	}
	want = knl_cpu_want(clk.limit_mhz, clk.heat_hz, clk.min_hz, clk.max_hz);
	if ( want == 0 || want == clk.want_hz ) {
		return TRUE;
	}
	if ( knl_cpu_hw_set(want, &got) >= E_OK ) {
		clk.want_hz = want;
		clk.got_hz = got;
		clk.applied++;
	}
	return TRUE;
}

EXPORT void knl_cpu_start( void )
{
	UW	was = 0, now = 0;

	(void)knl_cpu_hw_now(&was);
	clk_take();
	clk.taken++;
	if ( !clk_apply() ) {
		SpinUnlock(&clk_lock);
		return;
	}
	SpinUnlock(&clk_lock);
	(void)knl_cpu_hw_now(&now);
	tm_printf((UB *)"TessronOS: processors at %d MHz (were %d, the firmware allows %d to %d, limit %d until the settings are read)\n",
		  (INT)( now / MHZ ), (INT)( was / MHZ ), (INT)( clk.min_hz / MHZ ),
		  (INT)( clk.max_hz / MHZ ), CPU_MHZ_DEFAULT);
}

#define MSR_PM_ENABLE		0x770
#define MSR_HWP_CAPABILITIES	0x771
#define MSR_HWP_REQUEST		0x774

/*
 * What HWP is set to, read and told: nothing is written here. Not
 * read on a virtual machine (CPUID.1:ECX bit 31), whose hypervisor may
 * not let these registers be read, nor without HWP (CPUID.6:EAX bit 7).
 */
LOCAL void hwp_tell( void )
{
	UW	a, b, c, d;
	UD	en, cap, req;

	cpuid(1, 0, &a, &b, &c, &d);
	if ( c & 0x80000000U ) {
		tm_printf((UB *)"cpu: a virtual machine: the clock is the host's\n");
		return;
	}
	cpuid(0, 0, &a, &b, &c, &d);
	if ( a < 6 ) {
		return;
	}
	cpuid(6, 0, &a, &b, &c, &d);
	if ( ( a & 0x80U ) == 0 ) {
		tm_printf((UB *)"cpu: no HWP: the firmware's own clock steps\n");
		return;
	}
	/* for the line only: read where they may be refused */
	en = 0;
	cap = 0;
	req = 0;
	(void)knl_rdmsr_safe(MSR_PM_ENABLE, &en);
	if ( ( en & 1 ) == 0 ) {
		tm_printf((UB *)"cpu: HWP there but not on (IA32_PM_ENABLE %lx)\n", en);
		return;
	}
	(void)knl_rdmsr_safe(MSR_HWP_CAPABILITIES, &cap);
	(void)knl_rdmsr_safe(MSR_HWP_REQUEST, &req);
	tm_printf((UB *)"cpu: HWP on: performance %d to %d (guaranteed %d), asked %d to %d, desired %d, energy %d%s\n",
		  (INT)( ( cap >> 24 ) & 0xff ), (INT)( cap & 0xff ), (INT)( ( cap >> 8 ) & 0xff ),
		  (INT)( req & 0xff ), (INT)( ( req >> 8 ) & 0xff ), (INT)( ( req >> 16 ) & 0xff ),
		  (INT)( ( req >> 24 ) & 0xff ), ( a & 0x400U ) ? "" : " (no energy preference)");
}

/* The machine's setting CPU_MHZ: kept, not used */
EXPORT ER knl_cpu_limit( UW mhz )
{
	BOOL	first;

	if ( mhz > LIMIT_MHZ_MAX ) {
		mhz = LIMIT_MHZ_MAX;
	}
	clk_take();
	clk.taken++;
	first = !clk.told;
	clk.limit_mhz = mhz;
	clk.told = TRUE;
	SpinUnlock(&clk_lock);
	if ( first ) {
		tm_printf((UB *)"cpu: the setting CPU_MHZ %d is not used: on a PC the processor and its firmware choose the clock\n",
			  (INT)mhz);
		hwp_tell();
	}
	return E_OK;
}

EXPORT ER knl_cpu_heat( UW hz, UW *p_got )
{
	ER	er = E_OK;

	clk_take();
	clk.heat_hz = hz;
	if ( !clk_apply() ) {
		er = E_NOSPT;
	} else if ( p_got != NULL ) {
		*p_got = clk.got_hz;
	}
	SpinUnlock(&clk_lock);
	return er;
}

/*
 * The clock the processors run at, as /proc/cpuinfo's "cpu MHz" tells
 * it: each processor's APERF (counting at its actual clock) and MPERF
 * (at the TSC's fixed rate) sampled by the processor itself in its own
 * timer interrupt (knl_cpu_freq_tick, every LK_PERIOD_MS), and the
 * clock over the last period the TSC's rate times the ratio of their
 * increments. Both count only while the processor runs (C0), so a
 * processor that slept most of the period is told at the clock it ran
 * at, not slowed by its sleep. Reading costs no wait: the last periods'
 * values are taken as they are. A processor whose sample is older than
 * FREQ_STALE_MS (gone offline, its timer stopped) is left out. Only read.
 * Not on
 * a virtual machine (CPUID.1:ECX bit 31), whose hypervisor may fault
 * the reads, nor without the two counters (CPUID.6:ECX bit 0).
 */
#define MSR_MPERF		0xe7
#define MSR_APERF		0xe8
#define MSR_THERM_STATUS	0x19c
#define MSR_TEMPERATURE_TARGET	0x1a2
#define MSR_PKG_THERM_STATUS	0x1b1
#define FREQ_NPRC		64		/* the processors sampled; the rest are not told */
#define FREQ_STALE_MS		1500		/* three periods of the processors' timer */
#define TJMAX_DEFAULT		100		/* the most the processor's temperature may be, when it does not say */

typedef struct {
	UD		aperf, mperf, at;	/* the last sample, and when (the TSC) */
	volatile UD	mhz_at;			/* when mhz was made */
	volatile UW	mhz;			/* the clock over the period before it */
} FREQ;

LOCAL FREQ		freq[FREQ_NPRC];
LOCAL volatile INT	freq_can = -1;		/* -1 not yet asked of CPUID */
LOCAL volatile INT	temp_can = -1;		/* 0 none, 1 the core's sensor, 2 the package's */
LOCAL INT		temp_tj = TJMAX_DEFAULT;	/* TjMax, C */

LOCAL BOOL on_vm( void )
{
	UW	a, b, c, d;

	cpuid(1, 0, &a, &b, &c, &d);
	return (BOOL)( ( c & 0x80000000U ) != 0 );
}

LOCAL UW max_leaf( void )
{
	UW	a, b, c, d;

	cpuid(0, 0, &a, &b, &c, &d);
	return a;
}

/* From the processor's timer interrupt (apic.c), on the processor sampled */
EXPORT void knl_cpu_freq_tick( ID prcid )
{
	FREQ	*f;
	UD	now, a, m, da, dm;
	UW	ea, eb, ec, ed;

	if ( freq_can < 0 ) {
		ec = 0;
		if ( !on_vm() && max_leaf() >= 6 ) {
			cpuid(6, 0, &ea, &eb, &ec, &ed);
		}
		freq_can = ( ec & 1U ) ? 1 : 0;
	}
	if ( freq_can == 0 || prcid < 1 || prcid > FREQ_NPRC || knl_cntfrq == 0 ) {
		return;
	}
	f = &freq[prcid - 1];
	now = ReadSysCounterSync();
	a = rdmsr(MSR_APERF);
	m = rdmsr(MSR_MPERF);
	if ( f->at != 0 && now - f->at < knl_cntfrq / 1000 * FREQ_STALE_MS ) {
		da = a - f->aperf;
		dm = m - f->mperf;
		if ( dm != 0 ) {
			/* in kHz first: the TSC's rate times APERF's increment stays within 64 bits */
			f->mhz = (UW)( knl_cntfrq / 1000 * da / dm / 1000 );
			f->mhz_at = now;
		}
	}
	f->aperf = a;
	f->mperf = m;
	f->at = now;
}

/*
 * The clock in MHz, the mean of the processors sampled within the last
 * FREQ_STALE_MS and the fastest of them; E_NOSPT when none is
 */
EXPORT ER knl_cpu_x64_clock( UW *p_mean, UW *p_most )
{
	UD	now = ReadSysCounterSync(), sum = 0, at;
	UW	most = 0, v;
	INT	i, n = 0;

	if ( freq_can <= 0 || knl_cntfrq == 0 ) {
		return E_NOSPT;
	}
	for ( i = 0; i < knl_num_prc && i < FREQ_NPRC; i++ ) {
		at = freq[i].mhz_at;
		v = freq[i].mhz;
		if ( at == 0 || now - at >= knl_cntfrq / 1000 * FREQ_STALE_MS || v == 0 ) {
			continue;
		}
		sum += v;
		if ( v > most ) most = v;
		n++;
	}
	if ( n == 0 ) {
		return E_NOSPT;
	}
	*p_mean = (UW)( sum / (UD)n );
	*p_most = most;
	return E_OK;
}

/* The base and the most (turbo) clock as the processor names them (CPUID.16h), in MHz */
EXPORT ER knl_cpu_x64_rated( UW *p_base, UW *p_most )
{
	UW	a, b, c, d;

	if ( max_leaf() < 0x16 ) {
		return E_NOSPT;
	}
	cpuid(0x16, 0, &a, &b, &c, &d);
	if ( ( a & 0xffff ) == 0 && ( b & 0xffff ) == 0 ) {
		return E_NOSPT;
	}
	*p_base = a & 0xffff;
	*p_most = b & 0xffff;
	return E_OK;
}

/* ---------------------------------------------------------------- the registers of power there are */

/*
 * Which registers of power and of the limits' reasons the processor has
 * (syslib.h X64M_*). CPUID does not tell them: the family 6 model says
 * where a model is known to have a register and where it is known not
 * to, and each one taken is read once with knl_rdmsr_safe before it is
 * used, so that a model the tables do not know, or one whose firmware
 * hides a register, is answered with none instead of a fault later.
 *
 *   RAPL's package registers (0x606 units, 0x610 limits, 0x611 energy):
 *	Sandy Bridge and later; looked at on any other model
 *   the cores' limit reasons: 0x690 on Haswell and Broadwell, 0x64f on
 *	Skylake to Comet Lake, none on Sandy Bridge and Ivy Bridge; 0x64f
 *	then 0x690 looked at on the others
 *   the GT's limit reasons (0x6b0): none on Sandy Bridge, Ivy Bridge and
 *	the server parts without a GT; looked at on the others
 *   MSR_PKG_POWER_INFO (0x614) and MSR_PP0_CURRENT_CONFIG (0x601):
 *	looked at where RAPL is there
 *   the Atom cores: no RAPL on Bonnell, Saltwell and Airmont (Braswell,
 *	Cherry Trail), and none taken on Silvermont, whose units are of
 *	another form; RAPL as on the others on Goldmont, Goldmont Plus and
 *	Tremont; none of the limits' reasons on any of them
 */
LOCAL BOOL	msr_decided = FALSE;
LOCAL UW	msr_at[X64M_N];

LOCAL BOOL model_in( UW m, CONST UB *t, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( t[i] == m ) return TRUE;
	}
	return FALSE;
}

/* The address when the register answers a read, 0 when it is refused */
LOCAL UW msr_there( UW msr )
{
	UD	v;

	return ( msr != 0 && knl_rdmsr_safe(msr, &v) == E_OK ) ? msr : 0;
}

LOCAL void msr_decide( void )
{
	static CONST UB	rapl[] = { 0x2a, 0x2d, 0x3a, 0x3e, 0x3c, 0x3f, 0x45, 0x46, 0x3d, 0x47, 0x4f, 0x56,
				   0x4e, 0x5e, 0x55, 0x8e, 0x9e, 0xa5, 0xa6 };
	static CONST UB	snb_ivb[] = { 0x2a, 0x2d, 0x3a, 0x3e };
	static CONST UB	hsw_bdw[] = { 0x3c, 0x3f, 0x45, 0x46, 0x3d, 0x47, 0x4f, 0x56 };
	static CONST UB	skl_cml[] = { 0x4e, 0x5e, 0x8e, 0x9e, 0xa5, 0xa6 };
	static CONST UB	no_gt[] = { 0x3f, 0x4f, 0x56, 0x55 };
	static CONST UB	atom_no_rapl[] = { 0x1c, 0x26, 0x27, 0x35, 0x36, 0x37, 0x4a, 0x4c, 0x4d, 0x5a, 0x5d, 0x75 };
	static CONST UB	atom[] = { 0x1c, 0x26, 0x27, 0x35, 0x36, 0x37, 0x4a, 0x4c, 0x4d, 0x5a, 0x5d, 0x75,
				   0x5c, 0x5f, 0x7a, 0x86, 0x96, 0x9c };
	static CONST UB	atom_rapl[] = { 0x5c, 0x5f, 0x7a, 0x86, 0x96, 0x9c };
	UW		a, b, c, d, model;
	INT		i;

	msr_decided = TRUE;
	for ( i = 0; i < X64M_N; i++ ) msr_at[i] = 0;
	cpuid(0, 0, &a, &b, &c, &d);
	if ( on_vm() || b != 0x756e6547U || d != 0x49656e69U || c != 0x6c65746eU ) {	/* "GenuineIntel" */
		return;
	}
	cpuid(1, 0, &a, &b, &c, &d);
	if ( ( ( a >> 8 ) & 0xf ) != 6 ) {
		return;
	}
	model = ( ( a >> 4 ) & 0xf ) | ( ( a >> 12 ) & 0xf0 );
	if ( model_in(model, atom_no_rapl, (INT)sizeof(atom_no_rapl)) ) {
		;
	} else if ( model_in(model, rapl, (INT)sizeof(rapl)) || model_in(model, atom_rapl, (INT)sizeof(atom_rapl))
		 || ( msr_there(0x606) && msr_there(0x610) && msr_there(0x611) ) ) {
		if ( msr_there(0x610) && msr_there(0x611) ) msr_at[X64M_RAPL] = msr_there(0x606);
	}
	if ( msr_at[X64M_RAPL] != 0 ) {
		msr_at[X64M_PKG_INFO] = msr_there(0x614);
		msr_at[X64M_PP0_CUR] = msr_there(0x601);
	}
	if ( model_in(model, snb_ivb, (INT)sizeof(snb_ivb)) || model_in(model, atom, (INT)sizeof(atom)) ) {
		;
	} else if ( model_in(model, hsw_bdw, (INT)sizeof(hsw_bdw)) ) {
		msr_at[X64M_PLR_CORE] = msr_there(0x690);
	} else if ( model_in(model, skl_cml, (INT)sizeof(skl_cml)) ) {
		msr_at[X64M_PLR_CORE] = msr_there(0x64f);
	} else {
		msr_at[X64M_PLR_CORE] = msr_there(0x64f);
		if ( msr_at[X64M_PLR_CORE] == 0 ) msr_at[X64M_PLR_CORE] = msr_there(0x690);
	}
	if ( !model_in(model, snb_ivb, (INT)sizeof(snb_ivb)) && !model_in(model, no_gt, (INT)sizeof(no_gt))
	  && !model_in(model, atom, (INT)sizeof(atom)) ) {
		msr_at[X64M_PLR_GFX] = msr_there(0x6b0);
	}
	tm_printf((UB *)"cpu: model %02x: RAPL %x, power info %x, the cores' limit reasons %x, the GT's %x, "
		  "PP0 current %x (0: none)\n", model, msr_at[X64M_RAPL], msr_at[X64M_PKG_INFO], msr_at[X64M_PLR_CORE],
		  msr_at[X64M_PLR_GFX], msr_at[X64M_PP0_CUR]);
}

EXPORT UW knl_x64_msr( INT what )
{
	if ( !msr_decided ) msr_decide();
	return ( what >= 0 && what < X64M_N ) ? msr_at[what] : 0;
}

/*
 * The processor's temperature in degrees, as coretemp and the package
 * sensor tell it: TjMax (MSR_TEMPERATURE_TARGET bits 23:16, 100 when
 * it says 0) less the sensor's distance from it (bits 22:16 of the
 * package's thermal status, or of this core's without a package sensor),
 * when bit 31 says the reading is valid. Intel's only: the registers
 * are not there on others. Read only.
 */
EXPORT ER knl_cpu_x64_temp( INT *p_deg )
{
	UW	a, b, c, d;
	UD	st, v;

	/*
	 * Decided once: the package's sensor (CPUID 6 EAX bit 6) or this
	 * core's (bit 0), each taken only when its status register answers a
	 * read; TjMax from MSR_TEMPERATURE_TARGET where it answers, else 100
	 */
	if ( temp_can < 0 ) {
		INT	can = 0;

		cpuid(0, 0, &a, &b, &c, &d);
		/* "GenuineIntel" */
		if ( !on_vm() && a >= 6 && b == 0x756e6547U && d == 0x49656e69U && c == 0x6c65746eU ) {
			cpuid(6, 0, &a, &b, &c, &d);
			if ( ( a & 0x40U ) != 0 && knl_rdmsr_safe(MSR_PKG_THERM_STATUS, &v) == E_OK ) {
				can = 2;
			} else if ( ( a & 0x1U ) != 0 && knl_rdmsr_safe(MSR_THERM_STATUS, &v) == E_OK ) {
				can = 1;
			}
			if ( can != 0 ) {
				temp_tj = ( knl_rdmsr_safe(MSR_TEMPERATURE_TARGET, &v) == E_OK ) ? (INT)( ( v >> 16 ) & 0xff ) : 0;
				if ( temp_tj == 0 ) temp_tj = TJMAX_DEFAULT;
			}
		}
		temp_can = can;
	}
	if ( temp_can == 0 ) {
		return E_NOSPT;
	}
	st = rdmsr(( temp_can == 2 ) ? MSR_PKG_THERM_STATUS : MSR_THERM_STATUS);
	if ( ( st & 0x80000000ULL ) == 0 ) {
		return E_NOEXS;
	}
	*p_deg = temp_tj - (INT)( ( st >> 16 ) & 0x7f );
	return E_OK;
}

EXPORT void knl_cpu_clock( T_CPUCLK *st )
{
	clk_take();
	range_once();
	st->limit_mhz = clk.limit_mhz;
	st->want_hz = clk.want_hz;
	st->min_hz = clk.min_hz;
	st->max_hz = clk.max_hz;
	st->heat_hz = clk.heat_hz;
	st->taken = clk.taken;
	st->applied = clk.applied;
	SpinUnlock(&clk_lock);
	st->hz = 0;
	if ( knl_cpu_hw_now(&st->hz) < E_OK ) {
		st->hz = 0;
	}
	if ( knl_cpu_hw_throttled(&st->throttled) < E_OK ) {
		st->throttled = 0xFFFFFFFFU;
	}
}

#endif /* CPU_CORE_X86_64 */
