/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpi_therm.c
 *	The record of what holds the processors and the GT down: the
 *	processor's thermal and power registers, the firmware's thermal
 *	zones and DPTF participants, and the embedded controller's
 *	temperatures, on the console (design 3.10.13)
 *
 *	Only on an Intel processor that is not a virtual machine (the MSRs
 *	are not there on others). Once at the start, the settings the
 *	firmware left: TjMax and the TCC offset (MSR_TEMPERATURE_TARGET),
 *	the bidirectional PROCHOT (MSR_POWER_CTL bit 0), the package's
 *	power limits with their time windows and lock (MSR_PKG_POWER_LIMIT),
 *	the ratios (MSR_PLATFORM_INFO), IA32_PERF_CTL and the status, the
 *	energy bias, SpeedStep and turbo of IA32_MISC_ENABLE, HWP, the
 *	configurable TDP level and the cores' current limit; then each
 *	thermal zone's _TMP, _PSV, _CRT, _HOT, _TC1, _TC2, _TSP, the DPTF
 *	manager (INT3400), the trip points of its participants (INT3403
 *	and the processor's, \_SB.PCI0.B0D4) and the embedded controller's
 *	temperature and DPTF fields where the firmware names them.
 *
 *	Every second, read only: whether PROCHOT# is driven from outside
 *	the processor (IA32_PACKAGE_THERM_STATUS bit 2), whether the
 *	processor's own thermal control is on (bit 0), and the PROCHOT
 *	reason of the cores' and the GT's limit registers (bit 0 of
 *	MSR_CORE_PERF_LIMIT_REASONS and MSR_GFX_PERF_LIMIT_REASONS). When
 *	they change, a line at once (at most CHG_MAX in ten seconds); every
 *	ten seconds a line of how many seconds each was on, the package's
 *	temperature and power, the thermal status registers (whose logs
 *	are cleared then), the zones' and the controller's temperatures,
 *	the battery's _BST and the adapter's _PSR.
 *
 *	The DPTF passive table (design 3.10.13): where the DPTF manager
 *	(INT3400) lists the passive policy 2 among its UUIDs (IDSP) and has
 *	no _TRT or _ART for the passive policy 1, each row of its PSVT whose
 *	participant is the processor (B0D4, B0DB, TCPU) is a passive trip:
 *	the row's sensor (an INT3403) is read every POLL_S seconds through
 *	its _TMP; at or above the row's temperature the package's PL1
 *	(MSR_PKG_POWER_LIMIT) is set to the row's limit, at most once in the
 *	row's sample period and not again within DEBOUNCE_S, the lowest limit
 *	of the rows that are on winning; below it the row is off, and with no
 *	row on PL1 is the most again (the PL1 the firmware set: it is never
 *	written above that), with the enable bit
 *	and the time window it had. A limit below the most is set with a time
 *	window of a second and PL1 on, and never below half the most. The
 *	table's step and coefficients are not used. Nothing is written when
 *	PL1 is locked. The manager's _OSC is not called. All this, the
 *	sensors' _TMP included, is done only when the start-up setting says
 *	"thermdptf=1": a sensor's _TMP may tell the embedded controller that
 *	a DPTF host is there, and the Surface Go 2's controller then stops
 *	driving PROCHOT# at its own trips, which leaves the processor to PL1
 *	and its own thermal control alone.
 *
 *	Written besides: the logs of IA32_PACKAGE_THERM_STATUS, and PL1.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/board.h>
#include "sysdepend.h"
#include "acpi.h"
#include "acpica/tsacpi_os.h"

IMPORT ER	knl_cpu_x64_temp( INT *p_deg );		/* cpuclk.c */

#define MSR_PLATFORM_INFO	0x0ce
#define MSR_PERF_STATUS		0x198
#define MSR_PERF_CTL		0x199
#define MSR_THERM_STATUS	0x19c
#define MSR_MISC_ENABLE		0x1a0
#define MSR_TEMPERATURE_TARGET	0x1a2
#define MSR_ENERGY_PERF_BIAS	0x1b0
#define MSR_PKG_THERM_STATUS	0x1b1
#define MSR_POWER_CTL		0x1fc
#define MSR_PP0_CURRENT_CONFIG	0x601
#define MSR_RAPL_POWER_UNIT	0x606
#define MSR_PKG_POWER_LIMIT	0x610
#define MSR_PKG_ENERGY_STATUS	0x611
#define MSR_CONFIG_TDP_CONTROL	0x64b
#define MSR_PM_ENABLE		0x770
#define MSR_HWP_CAPABILITIES	0x771
#define MSR_HWP_REQUEST		0x774

#define ST_OUT		0x01		/* PROCHOT# driven from outside */
#define ST_TCC		0x02		/* the processor's own thermal control */
#define ST_CORE		0x04		/* the cores held down by PROCHOT */
#define ST_GT		0x08		/* the GT held down by PROCHOT */
#define ST_N		4

#define MSR_PKG_POWER_INFO	0x614

/*
 * What this processor has (decided at the start): the package's thermal
 * status (CPUID 6 EAX bit 6) and this core's (CPUID 1 EDX bit 22), each
 * read once with knl_rdmsr_safe before it is used, the energy bias
 * (CPUID 6 ECX bit 3),
 * RAPL's package registers and the limits' reasons of the cores and the
 * GT (knl_x64_msr: by the model, and a look where it does not settle
 * it). What is not there is told as none, and the power limit is not
 * used without RAPL: the temperature is then watched through PROCHOT
 * and the processor's own thermal control alone.
 */
LOCAL BOOL	has_ptm = FALSE, has_therm = FALSE, has_epb = FALSE, has_rapl = FALSE, has_info = FALSE;
LOCAL UW	msr_plr_core = 0, msr_plr_gfx = 0, msr_pp0 = 0;

/* A register that may be absent, for the lines: its value, or 0 when there is none */
LOCAL UD rd_if( UW msr )
{
	return ( msr != 0 ) ? rdmsr(msr) : 0;
}

#define PERIOD_S	10
#define CHG_MAX		6
#define TZ_MAX		8
#define SEN_MAX		6
#define LINE		1536

#define POLL_S		4		/* the sensors read */
#define DEBOUNCE_S	2		/* PL1 not set again within */
#define ROW_MAX		8
#define PL1_WINDOW_US	1000000ULL	/* the time window of a limit below the most */
#define PL_SANE_UW	1000000000ULL	/* more is not a power the processor can take */

IMPORT INT	knl_cpuidle_text( char *t );	/* kernel/sysdepend/x64/cpuidle.c */
IMPORT INT	knl_cpuidle_wake_text( char *t );
IMPORT INT	knl_hwp_text( char *t );	/* kernel/sysdepend/x64/hwp.c */

LOCAL BOOL	dptf_on = FALSE;	/* "thermdptf=1" turns it on */
LOCAL void	*ec_dev = NULL;
LOCAL void	*bat_dev = NULL, *ac_dev = NULL;
LOCAL INT	eu = 14;		/* the energy unit, 1 / 2^eu J */

LOCAL UD now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UW)t.lo;
}

/* A temperature of ACPI's (tenths of a kelvin) as degrees C with a tenth, "-" when there is none */
LOCAL INT dk_put( char *t, unsigned long long v, BOOL had )
{
	INT	c;

	if ( !had || v == 0xffffffffULL || v > 0xffff ) return tm_sprintf((UB *)t, (CONST UB *)"-");
	c = (INT)v - 2732;
	if ( c < 0 ) return tm_sprintf((UB *)t, (CONST UB *)"-%d.%d", -c / 10, -c % 10);
	return tm_sprintf((UB *)t, (CONST UB *)"%d.%d", c / 10, c % 10);
}

/* An integer method or field of a device, as " NAME v" (temperatures in C when dk), nothing when it has none */
LOCAL INT item( char *t, void *dev, CONST char *name, BOOL dk )
{
	unsigned long long	v = 0;
	INT			n;

	if ( dev == NULL || knl_acpica_int(dev, name, &v) != 0 ) return 0;
	n = tm_sprintf((UB *)t, (CONST UB *)" %s ", name);
	if ( dk ) return n + dk_put(t + n, v, TRUE);
	return n + tm_sprintf((UB *)t + n, (CONST UB *)"%d", (INT)v);
}

/* The embedded controller's fields of temperatures (degrees C) and of DPTF, where the firmware names them */
LOCAL INT ec_text( char *t )
{
	static CONST char * CONST	names[] = { "CTMP", "S1TP", "S2TP", "S3TP", "DPTF", "HOTN", "ECMS", "HFON", "ACST" };
	INT				i, n = 0;

	if ( ec_dev == NULL ) return 0;
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; EC");
	for ( i = 0; i < (INT)( sizeof(names) / sizeof(names[0]) ); i++ ) {
		n += item(t + n, ec_dev, names[i], FALSE);
	}
	return n;
}

/* ---------------------------------------------------------------- the start */

LOCAL UW ms_window( UD field, INT tu )
{
	UW	y = (UW)( field & 0x1f ), z = (UW)( ( field >> 5 ) & 3 );

	/* 2^Y * (1 + Z / 4) time units of 1 / 2^tu s */
	return (UW)( ( ( (UD)1000 * ( 4 + z ) ) << y ) >> ( tu + 2 ) );
}

/*
 * A register for the start's lines only: read with knl_rdmsr_safe, its
 * value as hex in b, or "none" when the processor refuses it (then 0).
 * TRUE when it was read.
 */
LOCAL BOOL shown( UW msr, UD *p_v, char *b )
{
	if ( knl_rdmsr_safe(msr, p_v) == E_OK ) {
		tm_sprintf((UB *)b, (CONST UB *)"%08x", (UW)*p_v);
		return TRUE;
	}
	*p_v = 0;
	tm_sprintf((UB *)b, (CONST UB *)"none");
	return FALSE;
}

LOCAL void tell_msrs( void )
{
	UW	a, b, c, d;
	UD	tt, pc, pl, unit, pi, misc, cur, v, ct;
	INT	pu, tu, tj, off, k;
	BOOL	have_tt, have_misc;
	char	t[LINE], s_pc[12], s_pi[12], s_ctl[12], s_st[12], s_epb[12], s_ts[12], s_pts[12], s_x[12];

	have_tt = shown(MSR_TEMPERATURE_TARGET, &tt, s_x);
	(void)shown(MSR_POWER_CTL, &pc, s_pc);
	(void)shown(MSR_PLATFORM_INFO, &pi, s_pi);
	have_misc = shown(MSR_MISC_ENABLE, &misc, s_x);
	(void)shown(MSR_PERF_CTL, &v, s_ctl);
	(void)shown(MSR_PERF_STATUS, &v, s_st);
	(void)shown(MSR_ENERGY_PERF_BIAS, &v, s_epb);
	(void)shown(MSR_THERM_STATUS, &v, s_ts);
	(void)shown(MSR_PKG_THERM_STATUS, &v, s_pts);
	unit = has_rapl ? rdmsr(MSR_RAPL_POWER_UNIT) : 0;
	pl = has_rapl ? rdmsr(MSR_PKG_POWER_LIMIT) : 0;
	cur = rd_if(msr_pp0);
	pu = (INT)( unit & 0xf );
	eu = (INT)( ( unit >> 8 ) & 0x1f );
	tu = (INT)( ( unit >> 16 ) & 0xf );
	tj = (INT)( ( tt >> 16 ) & 0xff );
	off = (INT)( ( tt >> 24 ) & 0x3f );
	if ( have_tt ) {
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: start: TjMax %d C, TCC offset %d (the processor's own control from %d C); ",
			       tj, off, tj - off);
	} else {
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: start: TjMax none (no TEMPERATURE_TARGET); ");
	}
	k += tm_sprintf((UB *)t + k, (CONST UB *)"bidirectional PROCHOT %s (POWER_CTL %s); ", ( pc & 1 ) ? "on" : "off", s_pc);
	if ( !has_rapl ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"no RAPL (no power limits nor energy); ");
	} else {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"PL1 %d mW %s clamp %d window %d ms, PL2 %d mW %s clamp %d window %d ms, "
				"%slocked (PKG_POWER_LIMIT %08x%08x); ",
				(INT)( ( ( pl & 0x7fff ) * 1000 ) >> pu ), ( pl & 0x8000 ) ? "on" : "off", (INT)( ( pl >> 16 ) & 1 ),
				(INT)ms_window(pl >> 17, tu),
				(INT)( ( ( ( pl >> 32 ) & 0x7fff ) * 1000 ) >> pu ), ( pl & ( 1ULL << 47 ) ) ? "on" : "off",
				(INT)( ( pl >> 48 ) & 1 ), (INT)ms_window(pl >> 49, tu), ( pl & ( 1ULL << 63 ) ) ? "" : "not ",
				(UW)( pl >> 32 ), (UW)pl);
	}
	k += tm_sprintf((UB *)t + k, (CONST UB *)"ratios least %d, most efficient %d, most without turbo %d (PLATFORM_INFO %s); "
			"PERF_CTL %s, PERF_STATUS %s; energy bias %s%s; SpeedStep %s, turbo %s; ",
			(INT)( ( pi >> 48 ) & 0xff ), (INT)( ( pi >> 40 ) & 0xff ), (INT)( ( pi >> 8 ) & 0xff ), s_pi, s_ctl, s_st,
			s_epb, has_epb ? "" : " (CPUID: none)", !have_misc ? "none" : ( misc & ( 1ULL << 16 ) ) ? "on" : "off",
			!have_misc ? "none" : ( misc & ( 1ULL << 38 ) ) ? "off" : "on");
	if ( msr_pp0 != 0 ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"the cores' current limit %d A %slocked; ", (INT)( ( cur & 0x1fff ) / 8 ),
				( cur & 0x80000000ULL ) ? "" : "not ");
	} else {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"the cores' current limit none; ");
	}
	k += tm_sprintf((UB *)t + k, (CONST UB *)"the limits' reasons: cores %x, GT %x (0: none)\n", msr_plr_core, msr_plr_gfx);
	tm_printf((UB *)"%s", t);

	cpuid(6, 0, &a, &b, &c, &d);
	k = tm_sprintf((UB *)t, (CONST UB *)"therm: start: thermal status of this core %s%s, of the package %s%s", s_ts,
		       has_therm ? "" : " (not used)", s_pts, has_ptm ? "" : " (not used)");
	if ( ( pi >> 33 ) & 3 ) {
		char	s_ct[12];

		if ( shown(MSR_CONFIG_TDP_CONTROL, &ct, s_ct) ) {
			k += tm_sprintf((UB *)t + k, (CONST UB *)"; configurable TDP level %d of %d, %slocked",
					(INT)( ct & 3 ), (INT)( ( pi >> 33 ) & 3 ), ( ct & 0x80000000ULL ) ? "" : "not ");
		} else {
			k += tm_sprintf((UB *)t + k, (CONST UB *)"; configurable TDP none");
		}
	}
	if ( ( a & 0x80U ) == 0 ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; no HWP");
	} else if ( knl_rdmsr_safe(MSR_PM_ENABLE, &v) != E_OK || ( v & 1 ) == 0 ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; HWP off");
	} else {
		char	s_cap[12], s_req[12];

		(void)shown(MSR_HWP_CAPABILITIES, &v, s_cap);
		(void)shown(MSR_HWP_REQUEST, &v, s_req);
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; HWP on, capabilities %s, request %08x%s", s_cap, (UW)( v >> 32 ), s_req);
	}
	tm_printf((UB *)"%s\n", t);
}

/* A device's trip points: the methods there are, side effects none (_TMP is not among them) */
LOCAL INT trips( char *t, void *dev )
{
	INT	n = 0;

	n += item(t + n, dev, "_PSV", TRUE);
	n += item(t + n, dev, "_CRT", TRUE);
	n += item(t + n, dev, "_HOT", TRUE);
	n += item(t + n, dev, "_AC0", TRUE);
	return n;
}

LOCAL void tell_acpi( void )
{
	char	t[LINE], path[64];
	void	*d;
	INT	i, k;

	for ( i = 0; i < TZ_MAX && ( d = knl_acpica_thermal_zone(i) ) != NULL; i++ ) {
		if ( knl_acpica_path(d, path, sizeof(path)) != 0 ) path[0] = '\0';
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: zone %s:", path);
		k += item(t + k, d, "_TMP", TRUE);
		k += trips(t + k, d);
		k += item(t + k, d, "_TC1", FALSE);
		k += item(t + k, d, "_TC2", FALSE);
		k += item(t + k, d, "_TSP", FALSE);
		tm_printf((UB *)"%s\n", t);
	}
	if ( i == 0 ) tm_printf((UB *)"therm: no thermal zone\n");

	d = knl_acpica_device("INT3400", 0);
	if ( d != NULL && knl_acpica_path(d, path, sizeof(path)) == 0 ) {
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: DPTF: the manager %s there (INT3400)", path);
	} else {
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: DPTF: no manager (INT3400)");
	}
	{
		void	*root = knl_acpica_handle("\\");

		if ( root != NULL ) k += item(t + k, root, "DPTF", FALSE);
	}
	if ( ( d = knl_acpica_handle("\\_SB.PCI0.B0D4") ) != NULL ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; the processor's B0D4:");
		k += trips(t + k, d);
	}
	for ( i = 0; i < SEN_MAX && ( d = knl_acpica_device("INT3403", i) ) != NULL; i++ ) {
		if ( knl_acpica_path(d, path, sizeof(path)) != 0 ) path[0] = '\0';
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; %s:", path);
		k += trips(t + k, d);
		if ( k > LINE - 120 ) break;
	}
	k += ec_text(t + k);
	tm_printf((UB *)"%s\n", t);
}

/* ---------------------------------------------------------------- the DPTF passive table */

typedef struct {
	void	*sensor;
	char	name[64];
	INT	temp_mc;		/* the trip, m°C */
	UW	period_s;		/* the sample period, 0 none */
	UD	limit_uw;		/* PL1 while on */
	BOOL	on;
	UD	last_op_s;
	BOOL	limited;		/* among the limits that hold */
	INT	read_mc;		/* the sensor's last reading, INT_MIN none */
} PSVROW;

LOCAL PSVROW	row[ROW_MAX];
LOCAL INT	nrow = 0;
LOCAL UW	n_tmp = 0, n_set = 0;	/* _TMP read, PL1 written */

/* PL1 (package power limit 1) as one power, in µW */
LOCAL struct {
	INT	pu;			/* the power unit, 1 / 2^pu W */
	INT	tu;			/* the time unit, 1 / 2^tu s */
	UD	min_state;		/* the most PL1 (no limit) */
	UD	max_state;		/* the least PL1 a limit gives */
	UD	curr;
	BOOL	locked;
	BOOL	constrained;
	BOOL	en_at_start;
	UW	win_at_start;		/* the time window field as it was */
	UD	last_action_s;
	BOOL	last_state;
} pl;

LOCAL UD uw_of( UD field )
{
	return ( ( field & 0x7fff ) * 1000000ULL ) >> pl.pu;
}

/* A time window in µs as the field (2^Y * (1 + F/4) time units) */
LOCAL UW window_field( UD us )
{
	UD	unit = 1000000ULL >> pl.tu, v, y = 0, f;

	if ( unit == 0 || us < unit ) return 0;
	v = us / unit;
	while ( ( v >> ( y + 1 ) ) != 0 ) y++;
	if ( y > 0x1f ) return 0x7f;
	f = 4 * ( v - ( 1ULL << y ) ) / ( 1ULL << y );
	return (UW)( ( y & 0x1f ) | ( ( f & 3 ) << 5 ) );
}

LOCAL void pl1_write( UD uw, INT en, INT win )
{
	UD	v = rdmsr(MSR_PKG_POWER_LIMIT), raw = ( uw << pl.pu ) / 1000000ULL;

	if ( v & ( 1ULL << 63 ) ) {
		pl.locked = TRUE;
		return;
	}
	if ( raw > 0x7fff ) raw = 0x7fff;
	v = ( v & ~0x7fffULL ) | raw;
	if ( en >= 0 ) v = ( v & ~( 1ULL << 15 ) ) | ( (UD)( en != 0 ) << 15 );
	if ( win >= 0 ) v = ( v & ~( 0x7fULL << 17 ) ) | ( (UD)( win & 0x7f ) << 17 );
	wrmsr(MSR_PKG_POWER_LIMIT, v);
	n_set++;
}

/* The power limit set to a state: at or above min_state the limit as it was at the start */
LOCAL void pl_set_curr( UD state, BOOL control )
{
	UD	n = state;

	if ( pl.locked ) {
		pl.curr = ( state >= pl.min_state ) ? pl.min_state : pl.max_state;
		return;
	}
	if ( n < pl.max_state ) n = pl.max_state;
	if ( n >= pl.min_state ) {
		n = pl.min_state;
		pl1_write(n, pl.en_at_start ? -1 : 0, (INT)pl.win_at_start);
		pl.constrained = FALSE;
	} else if ( control && !pl.constrained ) {
		pl1_write(n, pl.en_at_start ? -1 : 1, (INT)window_field(PL1_WINDOW_US));
		pl.constrained = TRUE;
	} else {
		pl1_write(n, -1, -1);
	}
	pl.curr = n;
}

/* The most restrictive limit of the rows that hold one, 0 none */
LOCAL UD pl_lowest( void )
{
	UD	low = 0;
	INT	i;

	for ( i = 0; i < nrow; i++ ) {
		if ( row[i].limited && ( low == 0 || row[i].limit_uw < low ) ) low = row[i].limit_uw;
	}
	return low;
}

/* A row turned on (state TRUE) or off, as the processor's power limit takes it */
LOCAL void pl_trip( INT r, BOOL state, UD now_s )
{
	UD	target;
	BOOL	first;
	INT	i, n_lim = 0;

	for ( i = 0; i < nrow; i++ ) {
		if ( row[i].limited ) n_lim++;
	}
	if ( !state && pl.curr >= pl.min_state && n_lim == 0 ) return;
	if ( state ) {
		first = (BOOL)( n_lim == 0 );
		row[r].limited = TRUE;
		target = pl_lowest();
		/* a limit no lower than the one that holds changes nothing */
		if ( !first && target >= pl.curr ) return;
		if ( pl.last_state && now_s - pl.last_action_s <= DEBOUNCE_S ) return;
	} else {
		/* only the row whose limit holds is taken out; one with a higher limit stays among them */
		if ( !row[r].limited || row[r].limit_uw != pl_lowest() ) return;
		row[r].limited = FALSE;
		target = pl_lowest();
		if ( target == 0 ) target = pl.min_state;
	}
	pl.last_action_s = now_s;
	pl.last_state = state;
	pl_set_curr(target, state);
}

/* Every POLL_S: each row's sensor read and its trip checked */
LOCAL void dptf_poll( UD now_s )
{
	INT	i, j;

	for ( i = 0; i < nrow; i++ ) {
		unsigned long long	v = 0;

		/* a sensor of several rows is read once */
		for ( j = 0; j < i && row[j].sensor != row[i].sensor; j++ ) ;
		if ( j < i ) {
			row[i].read_mc = row[j].read_mc;
			continue;
		}
		n_tmp++;
		if ( knl_acpica_int(row[i].sensor, "_TMP", &v) != 0 || v > 0xffff ) {
			row[i].read_mc = (INT)0x80000000;
			continue;
		}
		row[i].read_mc = ( (INT)v - 2732 ) * 100;
	}
	for ( i = 0; i < nrow; i++ ) {
		PSVROW	*w = &row[i];

		if ( w->read_mc == (INT)0x80000000 ) continue;
		if ( w->read_mc >= w->temp_mc ) {
			w->on = TRUE;
			if ( w->period_s != 0 ) {
				if ( now_s - w->last_op_s < w->period_s ) continue;
				w->last_op_s = now_s;
			}
			if ( pl.curr <= pl.max_state ) continue;	/* no further to go */
			pl_trip(i, TRUE, now_s);
		} else {
			w->on = FALSE;
			if ( pl.curr >= pl.min_state ) continue;	/* nothing to take back */
			pl_trip(i, FALSE, now_s);
		}
	}
}

LOCAL CONST UB	uuid_passive2[16] = {	/* 9E04115A-AE87-4D1C-9500-0F3E340BFE75, as ACPI lays it */
	0x5a, 0x11, 0x04, 0x9e, 0x87, 0xae, 0x1c, 0x4d, 0x95, 0x00, 0x0f, 0x3e, 0x34, 0x0b, 0xfe, 0x75
};
LOCAL CONST UB	uuid_passive1[16] = {	/* 42A441D6-AE6A-462b-A84B-4A8CE79027D3 */
	0xd6, 0x41, 0xa4, 0x42, 0x6a, 0xae, 0x2b, 0x46, 0xa8, 0x4b, 0x4a, 0x8c, 0xe7, 0x90, 0x27, 0xd3
};

LOCAL BOOL uuid_listed( UB (*u)[16], INT n, CONST UB *want )
{
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; k < 16 && u[i][k] == want[k]; k++ ) ;
		if ( k == 16 ) return TRUE;
	}
	return FALSE;
}

/* The last name of a path ("\_SB_.PCI0.B0D4" → "B0D4") */
LOCAL CONST char *last_name( CONST char *path )
{
	CONST char	*s = path, *p;

	for ( p = path; *p != '\0'; p++ ) {
		if ( *p == '.' || *p == '\\' ) s = p + 1;
	}
	return s;
}

/* The table taken, when the manager offers the passive policy 2 */
LOCAL void dptf_setup( void )
{
	static TSACPI_PSVT	t[ROW_MAX];
	UB			u[16][16];
	char			path[64], src[64];
	void			*ietm;
	UD			v, info, spec;
	INT			n, i;

	ietm = knl_acpica_device("INT3400", 0);
	if ( ietm == NULL || knl_acpica_path(ietm, path, sizeof(path)) != 0 ) {
		tm_printf((UB *)"therm: DPTF policy: no manager (INT3400)\n");
		return;
	}
	n = knl_acpica_uuids(ietm, "IDSP", u, 16);
	if ( n > 16 ) n = 16;
	if ( n < 0 || !uuid_listed(u, n, uuid_passive2) ) {
		tm_printf((UB *)"therm: DPTF policy: %s does not list the passive policy 2 (IDSP, %d UUIDs): not used\n", path, n);
		return;
	}
	if ( uuid_listed(u, n, uuid_passive1) ) {
		unsigned long long	dummy[1];
		char			ds[1][32];

		if ( knl_acpica_pkg(ietm, "_TRT", dummy, ds, 0) >= 0 || knl_acpica_pkg(ietm, "_ART", dummy, ds, 0) >= 0 ) {
			tm_printf((UB *)"therm: DPTF policy: %s has _TRT or _ART for the passive policy 1: not used\n", path);
			return;
		}
	}
	n = knl_acpica_psvt(ietm, t, ROW_MAX);
	if ( n <= 0 ) {
		tm_printf((UB *)"therm: DPTF policy: %s has no PSVT of revision 2: not used\n", path);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		CONST char	*s;
		PSVROW		*w = &row[nrow];
		INT		c;

		if ( knl_acpica_path(t[i].source, src, sizeof(src)) != 0 ) src[0] = '\0';
		if ( knl_acpica_path(t[i].target, w->name, sizeof(w->name)) != 0 ) w->name[0] = '\0';
		s = last_name(src);
		if ( knl_strcmp(s, "B0D4") != 0 && knl_strcmp(s, "B0DB") != 0 && knl_strcmp(s, "TCPU") != 0 ) {
			tm_printf((UB *)"therm: DPTF policy: PSVT row %d of %s: not the processor, left out\n", i + 1, src);
			continue;
		}
		if ( t[i].limit_str[0] != '\0' || t[i].limit == 0 ) {
			tm_printf((UB *)"therm: DPTF policy: PSVT row %d: a limit of \"%s\" (no power), left out\n", i + 1, t[i].limit_str);
			continue;
		}
		c = (INT)t[i].temp - 2732;
		c = ( c >= 0 ) ? ( c + 5 ) / 10 : ( c - 5 ) / 10;
		w->sensor = t[i].target;
		w->temp_mc = c * 1000;
		w->period_s = (UW)( t[i].sample_period / 10 );
		w->limit_uw = t[i].limit * 1000ULL;
		w->read_mc = (INT)0x80000000;
		tm_printf((UB *)"therm: DPTF policy: PSVT row %d: %s at %d C or more: PL1 %d mW (sample period %d s; "
			  "priority %d, step %d and coefficients %d/%d not used)\n", i + 1, w->name, c,
			  (INT)t[i].limit, (INT)w->period_s, (INT)t[i].priority, (INT)t[i].step,
			  (INT)t[i].limit_coeff, (INT)t[i].unlimit_coeff);
		nrow++;
	}
	if ( nrow == 0 ) return;
	if ( !has_rapl ) {
		tm_printf((UB *)"therm: DPTF policy: no RAPL on this processor: the table not used\n");
		nrow = 0;
		return;
	}

	v = rdmsr(MSR_RAPL_POWER_UNIT);
	pl.pu = (INT)( v & 0xf );
	pl.tu = (INT)( ( v >> 16 ) & 0xf );
	v = rdmsr(MSR_PKG_POWER_LIMIT);
	info = rd_if(has_info ? MSR_PKG_POWER_INFO : 0);
	spec = uw_of(info);
	/* the most is the PL1 the firmware set: never written above it, even where the design power is more */
	pl.min_state = uw_of(v);
	if ( pl.min_state > PL_SANE_UW ) pl.min_state = PL_SANE_UW;
	pl.max_state = pl.min_state - pl.min_state * 50 / 100;
	pl.curr = pl.min_state;
	pl.locked = (BOOL)( ( v & ( 1ULL << 63 ) ) != 0 );
	pl.en_at_start = (BOOL)( ( v & ( 1ULL << 15 ) ) != 0 );
	pl.win_at_start = (UW)( ( v >> 17 ) & 0x7f );
	tm_printf((UB *)"therm: DPTF policy: %d rows, the sensors read every %d s; PL1 most %d mW (thermal design %d mW, "
		      "PL1 %d mW), least %d mW%s\n", nrow, POLL_S, (INT)( pl.min_state / 1000 ), (INT)( spec / 1000 ),
		      (INT)( uw_of(v) / 1000 ), (INT)( pl.max_state / 1000 ), pl.locked ? "; PL1 locked: nothing written" : "");
}

LOCAL INT dptf_text( char *t )
{
	INT	i, n = 0, on = 0;

	if ( !has_rapl ) {
		return tm_sprintf((UB *)t, (CONST UB *)"; no RAPL: no PL1, DPTF policy not used");
	}
	if ( !dptf_on ) {
		return tm_sprintf((UB *)t, (CONST UB *)"; PL1 %d mW; DPTF policy off (thermdptf=1 turns it on)",
				  (INT)( uw_of(rdmsr(MSR_PKG_POWER_LIMIT)) / 1000 ));
	}
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; PL1 %d mW", (INT)( uw_of(rdmsr(MSR_PKG_POWER_LIMIT)) / 1000 ));
	if ( nrow == 0 ) return n + tm_sprintf((UB *)t + n, (CONST UB *)"; DPTF policy not used");
	for ( i = 0; i < nrow; i++ ) {
		if ( row[i].on ) on++;
	}
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; DPTF rows on %d of %d, PL1 %s %d mW, set %d times, _TMP read %d times",
			on, nrow, pl.locked ? "locked," : "asked", (INT)( pl.curr / 1000 ), (INT)n_set, (INT)n_tmp);
	for ( i = 0; i < nrow; i++ ) {
		INT	c = row[i].read_mc;

		if ( i > 0 && row[i].sensor == row[i - 1].sensor ) continue;
		if ( c == (INT)0x80000000 ) {
			n += tm_sprintf((UB *)t + n, (CONST UB *)"; %s -", last_name(row[i].name));
		} else {
			n += tm_sprintf((UB *)t + n, (CONST UB *)"; %s %d.%d", last_name(row[i].name), c / 1000,
					( ( c < 0 ) ? -c : c ) % 1000 / 100);
		}
	}
	return n;
}

/* ---------------------------------------------------------------- every second */

LOCAL UW state_now( UD *p_pkg )
{
	UD	pkg = has_ptm ? rdmsr(MSR_PKG_THERM_STATUS) : rd_if(has_therm ? MSR_THERM_STATUS : 0);
	UW	s = 0;

	/* the same bits in this core's status where there is no package's: 2 PROCHOT# from outside, 0 its own control */
	if ( pkg & 4 ) s |= ST_OUT;
	if ( pkg & 1 ) s |= ST_TCC;
	if ( rd_if(msr_plr_core) & 1 ) s |= ST_CORE;
	if ( rd_if(msr_plr_gfx) & 1 ) s |= ST_GT;
	*p_pkg = pkg;
	return s;
}

LOCAL INT temp_now( void )
{
	INT	deg = -1;

	if ( knl_cpu_x64_temp(&deg) < E_OK ) deg = -1;
	return deg;
}

/* The thermal zones' _TMP and the controller's fields */
LOCAL INT zones_text( char *t )
{
	char	path[64];
	void	*d;
	INT	i, n = 0;

	for ( i = 0; i < TZ_MAX && ( d = knl_acpica_thermal_zone(i) ) != NULL; i++ ) {
		unsigned long long	v = 0;
		BOOL			had;

		if ( knl_acpica_path(d, path, sizeof(path)) != 0 ) path[0] = '\0';
		had = (BOOL)( knl_acpica_int(d, "_TMP", &v) == 0 );
		n += tm_sprintf((UB *)t + n, (CONST UB *)"; %s ", path);
		n += dk_put(t + n, v, had);
	}
	return n + ec_text(t + n);
}

LOCAL INT power_text( char *t )
{
	unsigned long long	v[4];
	char			s[4][32];
	unsigned long long	on = 0;
	INT			n = 0;

	if ( bat_dev != NULL && knl_acpica_pkg(bat_dev, "_BST", v, s, 4) >= 4 ) {
		n += tm_sprintf((UB *)t + n, (CONST UB *)"; battery state %d rate %d remaining %d voltage %d",
				(INT)v[0], (INT)(UW)v[1], (INT)(UW)v[2], (INT)(UW)v[3]);
	}
	if ( ac_dev != NULL && knl_acpica_int(ac_dev, "_PSR", &on) == 0 ) {
		n += tm_sprintf((UB *)t + n, (CONST UB *)"; AC %d", (INT)on);
	}
	return n;
}

LOCAL void st_put( char *t, INT *k, UW s )
{
	*k += tm_sprintf((UB *)t + *k, (CONST UB *)"outside %d, own %d, cores %d, GT %d",
			 ( s & ST_OUT ) ? 1 : 0, ( s & ST_TCC ) ? 1 : 0, ( s & ST_CORE ) ? 1 : 0, ( s & ST_GT ) ? 1 : 0);
}

LOCAL void therm_task( INT stacd, void *exinf )
{
	UD	pkg = 0, t0 = now_ms(), since = t0, e0, e, now;
	UW	s, last, chg_lost = 0;
	INT	on[ST_N], i, k, n_chg = 0, sec = 0, deg, most = -1, since_poll = 0;
	char	t[LINE];

	(void)stacd;
	(void)exinf;
	if ( nrow > 0 ) dptf_poll(t0 / 1000);
	last = state_now(&pkg);
	e0 = (UW)rd_if(has_rapl ? MSR_PKG_ENERGY_STATUS : 0);
	knl_memset(on, 0, sizeof(on));
	k = tm_sprintf((UB *)t, (CONST UB *)"therm: at %d s: ", (INT)( t0 / 1000 ));
	st_put(t, &k, last);
	k += tm_sprintf((UB *)t + k, (CONST UB *)"; package %d C, thermal status %08x", temp_now(), (UW)pkg);
	tm_printf((UB *)"%s\n", t);
	for ( ;; ) {
		(void)tk_dly_tsk(1000);
		s = state_now(&pkg);
		now = now_ms();
		deg = temp_now();
		if ( deg > most ) most = deg;
		for ( i = 0; i < ST_N; i++ ) {
			if ( s & ( 1U << i ) ) on[i]++;
		}
		sec++;
		if ( nrow > 0 && ++since_poll >= POLL_S ) {
			since_poll = 0;
			dptf_poll(now / 1000);
		}
		if ( s != last ) {
			if ( n_chg < CHG_MAX ) {
				k = tm_sprintf((UB *)t, (CONST UB *)"therm: change at %d s after %d s: ", (INT)( now / 1000 ),
					       (INT)( ( now - since ) / 1000 ));
				st_put(t, &k, s);
				k += tm_sprintf((UB *)t + k, (CONST UB *)" (were ");
				st_put(t, &k, last);
				k += tm_sprintf((UB *)t + k, (CONST UB *)"); package %d C, thermal status %08x; cores %04x, GT %04x",
						deg, (UW)pkg, (UW)( rd_if(msr_plr_core) & 0xffff ),
						(UW)( rd_if(msr_plr_gfx) & 0xffff ));
				k += ec_text(t + k);
				tm_printf((UB *)"%s\n", t);
				n_chg++;
			} else {
				chg_lost++;
			}
			last = s;
			since = now;
		}
		if ( sec < PERIOD_S ) continue;

		e = (UW)rd_if(has_rapl ? MSR_PKG_ENERGY_STATUS : 0);
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: %d s at %d s: seconds outside %d, own %d, cores %d, GT %d; changes %d "
			       "(%d not told); package %d C (most %d), %d mW; thermal status of the package %08x, of this core %08x",
			       sec, (INT)( now / 1000 ), on[0], on[1], on[2], on[3], n_chg + (INT)chg_lost, (INT)chg_lost,
			       deg, most, (INT)( ( (UD)(UW)( e - e0 ) * 1000 / (UD)sec ) >> eu ), (UW)pkg,
			       (UW)rd_if(has_therm ? MSR_THERM_STATUS : 0));
		/* the logs of the package's status, cleared: each line tells what happened in its own seconds */
		if ( has_ptm ) wrmsr(MSR_PKG_THERM_STATUS, 0);
		k += zones_text(t + k);
		k += power_text(t + k);
		tm_printf((UB *)"%s\n", t);
		/* the idle states, the clock and the power limit over the same seconds */
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: power at %d s", (INT)( now / 1000 ));
		k += knl_cpuidle_text(t + k);
		k += knl_hwp_text(t + k);
		k += dptf_text(t + k);
		tm_printf((UB *)"%s\n", t);
		/* what woke the processors, how much each slept, and the tasks that ran the most */
		k = tm_sprintf((UB *)t, (CONST UB *)"therm: wake at %d s", (INT)( now / 1000 ));
		k += knl_cpuidle_wake_text(t + k);
		tm_printf((UB *)"%s\n", t);
		e0 = e;
		knl_memset(on, 0, sizeof(on));
		sec = 0;
		n_chg = 0;
		chg_lost = 0;
		most = -1;
	}
}

/*
 * After the batteries and the adapter are made objects (devinit.c):
 * the start's lines and the task, on an Intel processor that is not
 * a virtual machine with the interpreter running
 */
EXPORT void knl_acpi_therm_start( void )
{
	T_CTSK	ct;
	ID	t;
	INT	deg;
	UB	v[8];

	if ( knl_cpu_x64_temp(&deg) == E_NOSPT || !knl_acpica_ready() ) {
		return;
	}
	if ( knl_bootarg(knl_bootargs, "thermdptf", v, sizeof(v)) && v[0] == '1' ) {
		dptf_on = TRUE;
	}
	{
		UW	a, b, c, d;

		UD	v;

		cpuid(1, 0, &a, &b, &c, &d);
		has_therm = (BOOL)( ( d & ( 1U << 22 ) ) != 0 && knl_rdmsr_safe(MSR_THERM_STATUS, &v) == E_OK );
		cpuid(6, 0, &a, &b, &c, &d);
		has_ptm = (BOOL)( ( a & 0x40U ) != 0 && knl_rdmsr_safe(MSR_PKG_THERM_STATUS, &v) == E_OK );
		has_epb = (BOOL)( ( c & 0x08U ) != 0 );
	}
	has_rapl = (BOOL)( knl_x64_msr(X64M_RAPL) != 0 );
	has_info = (BOOL)( knl_x64_msr(X64M_PKG_INFO) != 0 );
	msr_plr_core = knl_x64_msr(X64M_PLR_CORE);
	msr_plr_gfx = knl_x64_msr(X64M_PLR_GFX);
	msr_pp0 = knl_x64_msr(X64M_PP0_CUR);
	/* the power and time units, for telling PL1 whether or not the passive table is used */
	if ( has_rapl ) {
		pl.pu = (INT)( rdmsr(MSR_RAPL_POWER_UNIT) & 0xf );
		pl.tu = (INT)( ( rdmsr(MSR_RAPL_POWER_UNIT) >> 16 ) & 0xf );
	}
	ec_dev = knl_acpica_device("PNP0C09", 0);
	bat_dev = knl_acpica_device("PNP0C0A", 0);
	ac_dev = knl_acpica_device("ACPI0003", 0);
	tell_msrs();
	tell_acpi();
	if ( dptf_on ) {
		dptf_setup();
	} else {
		tm_printf((UB *)"therm: the DPTF passive table not used, the sensors' _TMP not read (thermdptf=1 turns it on)\n");
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)therm_task;
	ct.itskpri = 20;
	ct.stksz = 16 * 1024;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}

#endif /* X64 */
