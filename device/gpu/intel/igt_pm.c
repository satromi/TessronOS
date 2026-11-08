/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_pm.c
 *	The power of the GT of Intel's integrated graphics of the 9th
 *	generation: forcewake, RC6 and the frequency as the load asks (RPS)
 *	(docs/tessronos/13-gpu.md 13.11.13).
 *
 *	Forcewake. The registers of the engines and of the GT's power are
 *	reached only while the GT is awake; a forcewake request (one masked
 *	bit in each of the render, GT and media wells) keeps it so, and the
 *	acknowledgement says it is. Who reaches them takes a reference
 *	(igt_fw_get) and lets it go (igt_fw_put); a job holds one from
 *	before it is submitted until it is over. When nobody has held one
 *	for IDLE_US, the look every millisecond parks the GT and lets the
 *	wells go, and the GT may sleep in RC6. The next igt_fw_get asks for
 *	the wells again, which wakes the GT out of RC6, waits for the
 *	acknowledgements and unparks it before the registers are touched.
 *	With RC6 off (cmdline.txt "igtrc6=0") the wells are never let go.
 *	The GT is not parked while an engine holds a process's context:
 *	the task first queues a job of the kernel's context on that engine
 *	(igt_to_kernel), so that what RC6 saves and brings back is never
 *	the context of a process that may end meanwhile.
 *
 *	RC6. The timers and thresholds of RC6 and of the render and media
 *	power gating are set as the 9th generation wants them. On this
 *	generation the context RC6 saves may be corrupted when the hardware
 *	enters RC6 on its own while busy, so the automatic entry stays off
 *	(RC_CONTROL 0 while unparked) and RC6 is asked for by hand when the
 *	GT is parked (RC_CONTROL with RC6 enabled, the target state RC6).
 *	Before each such request, and when RC6 is set up, the saved context
 *	is looked at (RC6_CTX_INFO); found corrupted, RC6 is turned off for
 *	good and the wells kept awake.
 *
 *	RPS. The frequency is chosen between RPn and RP0 (RP_STATE_CAP) by
 *	the GT's own measure of how busy it was: over an evaluation interval
 *	it raises an interrupt when busy more than the up threshold or less
 *	than the down threshold. A task steps the frequency up or down on
 *	them, the step doubling while they come the same way; the intervals
 *	follow how high it is (three modes: low, between, high). Parked, the
 *	frequency goes to RPn and the interrupts are off; unparked, it starts
 *	again where it was, a little lower, not below the efficient one. The
 *	efficient frequency is asked of the power controller (pcode), and
 *	the table that ties the ring's (the LLC's) frequency to the GT's is
 *	written to it, both through knl_igt_pcode_rw. With RPS off
 *	("igtrps=0") none of it is done and the frequency stays at RP1.
 *
 *	Each step of setting up is told on the console, and a line of the
 *	state every 60 s while the GPU works.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"
#include "igt.h"

#define FW_ACK_US	50000		/* forcewake acknowledged within */
#define IDLE_US		10000		/* nobody near the registers this long: parked */
#define UP_PCT		95		/* busy more than this of an interval: up */
#define DOWN_PCT	85		/* busy less than this: down */
#define REPORT_MS	60000

#define PM_EVENTS	( PM_RP_UP_THRESHOLD | PM_RP_DOWN_THRESHOLD )

/* The power controller's mailbox (kernel/sysdepend/x64/igt_pcode.c) */
IMPORT ER	knl_igt_pcode_rw( UW cmd, UW in, UW *out );
#define PCODE_WRITE_MIN_FREQ_TABLE	0x08	/* the ring's frequency for each of the GT's */
#define PCODE_DYNAMIC_DUTY_CYCLE	0x1a	/* 15:8 the efficient frequency, units of 50 MHz */

LOCAL BOOL		rc6_bad;	/* the saved context found corrupted: told by the task */

/* ---------------------------------------------------------------- the settings */

EXPORT INT igt_setting( CONST char *word, INT dflt )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k, v;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; word[k] != '\0' && c[k] == word[k]; k++ ) ;
		if ( word[k] != '\0' || c[k] != '=' || c[k + 1] < '0' || c[k + 1] > '9' ) continue;
		for ( v = 0, k++; c[k] >= '0' && c[k] <= '9'; k++ ) v = v * 10 + ( c[k] - '0' );
		return v;
	}
	return dflt;
}

/* ---------------------------------------------------------------- forcewake */

LOCAL CONST struct {
	UW		set, ack;
	CONST char	*name;
} wells[3] = {
	{ FW_RENDER, FW_RENDER_ACK, "render" },
	{ FW_GT, FW_GT_ACK, "GT" },
	{ FW_MEDIA, FW_MEDIA_ACK, "media" },
};

LOCAL void udelay( UD us )
{
	UD	t0 = igt_now_us();

	while ( igt_now_us() - t0 < us ) CpuRelax();
}

LOCAL BOOL ack_is( UW ack, UW mask, UW val, UD us )
{
	UD	t0 = igt_now_us();

	for ( ;; ) {
		if ( ( igt_rd(ack) & mask ) == val ) return TRUE;
		if ( igt_now_us() - t0 > us ) return FALSE;
		CpuRelax();
	}
}

/*
 * A well's acknowledgement waited for. It may be lost when the hardware
 * wakes the well on its own at the same moment: a second bit is toggled
 * to kick it, as many times as it takes. The kicks needed, or -1.
 */
LOCAL INT fw_ack( INT k )
{
	INT	pass;

	if ( ack_is(wells[k].ack, FW_KERNEL, FW_KERNEL, FW_ACK_US) ) return 0;
	for ( pass = 1; pass <= 10; pass++ ) {
		BOOL	ok;

		(void)ack_is(wells[k].ack, FW_FALLBACK, 0, FW_ACK_US);
		igt_wr(wells[k].set, MASK_EN(FW_FALLBACK));
		udelay(10 * pass);
		(void)ack_is(wells[k].ack, FW_FALLBACK, FW_FALLBACK, FW_ACK_US);
		ok = ( igt_rd(wells[k].ack) & FW_KERNEL ) != 0;
		igt_wr(wells[k].set, MASK_DIS(FW_FALLBACK));
		if ( ok ) return pass;
	}
	return -1;
}

/* The three wells asked for, the last let go of having had time to fall */
LOCAL void fw_wake( void )
{
	INT	k;

	for ( k = 0; k < 3; k++ ) {
		if ( !ack_is(wells[k].ack, FW_KERNEL, 0, FW_ACK_US) ) knl_igt.n_fw_stuck++;
		igt_wr(wells[k].set, MASK_EN(FW_KERNEL));
	}
	for ( k = 0; k < 3; k++ ) {
		if ( fw_ack(k) != 0 ) knl_igt.n_fw_kick++;
	}
}

EXPORT BOOL igt_fw_init( void )
{
	BOOL	all = TRUE;
	UINT	imask;
	INT	k, r;

	for ( k = 0; k < 3; k++ ) {
		igt_wr(wells[k].set, MASK_DIS(0xffff));
		if ( !ack_is(wells[k].ack, FW_KERNEL, 0, FW_ACK_US) ) {
			tm_printf((UB *)"igt: forcewake %s: the acknowledgement stays set (%08x)\n", wells[k].name,
				  igt_rd(wells[k].ack));
		}
		igt_wr(wells[k].set, MASK_EN(FW_KERNEL));
		r = fw_ack(k);
		if ( r < 0 ) {
			tm_printf((UB *)"igt: forcewake %s: NOT acknowledged (%08x)\n", wells[k].name,
				  igt_rd(wells[k].ack));
			all = FALSE;
		} else if ( r > 0 ) {
			tm_printf((UB *)"igt: forcewake %s: acknowledged after %d kick(s)\n", wells[k].name, r);
		}
	}
	ISpinLock(&knl_igt.lock, &imask);
	knl_igt.fw_held = TRUE;
	knl_igt.fw_users = 1;			/* the start-up's, let go of when it is over */
	knl_igt.fw_always = TRUE;		/* until RC6 is set up */
	knl_igt.parked = FALSE;
	ISpinUnlock(&knl_igt.lock, &imask);
	if ( knl_igt.fw_mtx <= 0 ) {
		T_CMTX	cm;

		cm.exinf = NULL;
		cm.mtxatr = TA_INHERIT;
		knl_igt.fw_mtx = tk_cre_mtx(&cm);
		if ( knl_igt.fw_mtx <= 0 ) all = FALSE;
	}
	return all;
}

/* ---------------------------------------------------------------- RPS */

/*
 * An interval of the GT's power management of 'ns': units of 16 ticks
 * of its 12 MHz time stamp, rounded up
 */
LOCAL UW pm_interval( UD ns )
{
	UD	ticks = ( ns * 12 + 999 ) / 1000;

	return (UW)( ( ticks + 15 ) / 16 );
}

/* The evaluation intervals and thresholds for how high the frequency is; under the lock, awake */
LOCAL void set_power( INT mode )
{
	UD	ei_up, ei_down = 32000;		/* units of 1 us */

	if ( mode == knl_igt.power_mode ) return;
	ei_up = ( mode == 0 ) ? 16000 : ( mode == 1 ) ? 13000 : 10000;
	igt_wr(RP_UP_EI, pm_interval(ei_up * 1000));
	igt_wr(RP_UP_THRESHOLD, pm_interval(ei_up * UP_PCT * 10));
	igt_wr(RP_DOWN_EI, pm_interval(ei_down * 1000));
	igt_wr(RP_DOWN_THRESHOLD, pm_interval(ei_down * DOWN_PCT * 10));
	igt_wr(RP_CONTROL, RP_MEDIA_TURBO | RP_MEDIA_HW_NORMAL_MODE | RP_MEDIA_IS_GFX | RP_ENABLE
			   | RP_UP_BUSY_AVG | RP_DOWN_IDLE_AVG);
	knl_igt.power_mode = mode;
}

LOCAL void set_thresholds( UINT val )
{
	INT	mode = knl_igt.power_mode;

	switch ( mode ) {
	case 0:
		if ( val > knl_igt.f_eff + 1 && val > knl_igt.f_cur ) mode = 1;
		break;
	case 1:
		if ( val <= knl_igt.f_eff && val < knl_igt.f_cur ) mode = 0;
		else if ( val >= knl_igt.f_max && val > knl_igt.f_cur ) mode = 2;
		break;
	case 2:
		if ( val < ( knl_igt.f_rp1 + knl_igt.f_max ) / 2 && val < knl_igt.f_cur ) mode = 1;
		break;
	default:
		break;
	}
	if ( val <= knl_igt.f_min ) mode = 0;
	if ( val >= knl_igt.f_max ) mode = 2;
	set_power(mode);
}

/* The frequency asked for; under the lock, awake */
LOCAL void freq_req( UINT val, BOOL update )
{
	if ( val == knl_igt.f_last ) return;
	igt_wr(RPNSWREQ, FREQ_REQ(val));
	if ( update ) set_thresholds(val);
	knl_igt.f_last = val;
}

/* The interrupts that can move the frequency from 'val'; the others masked */
LOCAL UW pm_mask( UINT val )
{
	UW	m = 0;

	if ( val > knl_igt.f_min ) m |= PM_RP_UP_EI_EXPIRED | PM_RP_DOWN_THRESHOLD | PM_RP_DOWN_TIMEOUT;
	if ( val < knl_igt.f_max ) m |= PM_RP_UP_EI_EXPIRED | PM_RP_UP_THRESHOLD;
	m &= PM_EVENTS;
	return ~m & ~PMINTR_REDIRECT_TO_GUC_DIS;
}

/*
 * A new frequency while unparked, with the limits of the interrupts:
 * the lowest only once there, lest a fall below it coming out of RC6
 * hide the interrupt. Under the lock, awake.
 */
LOCAL void freq_set( UINT val )
{
	UW	limits = knl_igt.f_max << 23;

	if ( val <= knl_igt.f_min ) limits |= knl_igt.f_min << 14;
	freq_req(val, TRUE);
	igt_wr(RP_INTERRUPT_LIMITS, limits);
	igt_wr(PMINTRMSK, pm_mask(val));
	knl_igt.f_cur = val;
}

/* ---------------------------------------------------------------- parking */

/* Idle: the frequency to its lowest, RC6 asked for; under the lock, awake */
LOCAL void park( void )
{
	if ( knl_igt.rps_on ) {
		INT	adj, cur;

		igt_wr(PMINTRMSK, ~PMINTR_REDIRECT_TO_GUC_DIS);
		igt_wr(GT_IER(2), 0);
		igt_wr(GT_IMR(2), 0xffffffff);
		knl_igt.pm_iir = 0;
		if ( knl_igt.f_last > knl_igt.f_min ) {
			freq_req(knl_igt.f_min, FALSE);
			/*
			 * Started again from a little lower, as though the GT had
			 * asked to go down: parked more often than it can ask, the
			 * frequency would never move otherwise
			 */
			adj = ( knl_igt.f_adj < 0 ) ? knl_igt.f_adj * 2 : -2;
			knl_igt.f_adj = adj;
			cur = (INT)knl_igt.f_cur + adj;
			knl_igt.f_cur = ( cur > (INT)knl_igt.f_min ) ? (UINT)cur : knl_igt.f_min;
			if ( knl_igt.f_cur < knl_igt.f_eff ) {
				knl_igt.f_cur = knl_igt.f_eff;
				knl_igt.f_adj = 0;
			}
		}
	}
	if ( knl_igt.rc6_on ) {
		if ( igt_rd(RC6_CTX_INFO) == 0 ) {
			igt_wr(PG_ENABLE, 0);
			igt_wr(RC_CONTROL, 0);
			igt_wr(RC_STATE, 0);
			knl_igt.rc6_on = FALSE;
			knl_igt.fw_always = TRUE;
			rc6_bad = TRUE;
		} else {
			igt_wr(RC_CONTROL, RC_CTL_RC6_ENABLE);
			igt_wr(RC_STATE, RC_SW_TARGET(4));
		}
	}
	knl_igt.parked = TRUE;
	knl_igt.n_park++;
}

/* Busy again: RC6 by hand off, the frequency where it was, its interrupts; under the lock, awake */
LOCAL void unpark( void )
{
	UINT	f;

	if ( knl_igt.rc6_on ) igt_wr(RC_CONTROL, 0);
	if ( knl_igt.rps_on ) {
		f = knl_igt.f_cur;
		if ( f < knl_igt.f_min ) f = knl_igt.f_min;
		if ( f > knl_igt.f_max ) f = knl_igt.f_max;
		freq_set(f);
		knl_igt.pm_iir = 0;
		igt_wr(GT_IIR(2), PM_EVENTS);
		igt_wr(GT_IIR(2), PM_EVENTS);
		igt_wr(GT_IER(2), PM_EVENTS);
		igt_wr(GT_IMR(2), ~PM_EVENTS);
		igt_wr(PMINTRMSK, pm_mask(knl_igt.f_last));
	}
	knl_igt.n_unpark++;
}

/*
 * A task's: whoever finds the wells let go of wakes them, and the others
 * wait on the mutex for it (its priority raised to theirs) rather than
 * spin on a processor it may need
 */
EXPORT void igt_fw_get( void )
{
	UINT	imask;
	BOOL	wake = FALSE;

	(void)tk_loc_mtx(knl_igt.fw_mtx, TMO_FEVR);
	ISpinLock(&knl_igt.lock, &imask);
	knl_igt.fw_users++;
	if ( !knl_igt.fw_held ) {
		knl_igt.fw_held = TRUE;
		wake = TRUE;
		knl_igt.n_wake++;
	}
	ISpinUnlock(&knl_igt.lock, &imask);

	if ( wake ) fw_wake();

	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.parked ) {
		knl_igt.parked = FALSE;
		unpark();
	}
	ISpinUnlock(&knl_igt.lock, &imask);
	(void)tk_unl_mtx(knl_igt.fw_mtx);
}

EXPORT void igt_fw_put( void )
{
	UINT	imask;

	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.fw_users > 0 && --knl_igt.fw_users == 0 ) knl_igt.fw_idle_since = igt_now_us();
	ISpinUnlock(&knl_igt.lock, &imask);
}

/* ---------------------------------------------------------------- the interrupts and the task */

EXPORT BOOL igt_pm_irq( UW iir )
{
	UW	ev = iir & PM_EVENTS;

	if ( ev == 0 || !knl_igt.rps_on || knl_igt.parked ) return FALSE;
	/* masked until the task has done with them */
	igt_wr(GT_IMR(2), igt_rd(GT_IMR(2)) | ev);
	knl_igt.pm_iir |= ev;
	knl_igt.n_pm_irq++;
	return TRUE;
}

EXPORT void igt_pm_wake( void )
{
	if ( knl_igt.pm_tsk > 0 ) (void)tk_wup_tsk(knl_igt.pm_tsk);
}

/* From the look every millisecond (a cyclic handler) */
EXPORT void igt_pm_tick( void )
{
	UINT	imask;
	BOOL	wake = FALSE;
	INT	k;

	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.pm_ready && ( knl_igt.intno == 0 || knl_igt.irq_off ) && !knl_igt.parked ) {
		/* no interrupt comes: the GT's word read here */
		UW	iir = igt_rd(GT_IIR(2)) & PM_EVENTS;

		if ( iir != 0 ) {
			igt_wr(GT_IIR(2), iir);
			wake = igt_pm_irq(iir);
		}
	}
	if ( knl_igt.pm_ready && !knl_igt.parked && knl_igt.fw_users == 0 && knl_igt.active < 0
	  && igt_now_us() - knl_igt.fw_idle_since >= IDLE_US && igt_holds_user() ) {
		/*
		 * Not parked on a process's context: RC6 would save it and bring
		 * it back, after its process may have ended. The task gives the
		 * engine the kernel's context first, and the GT is parked once
		 * that job is over and the GT idle again.
		 */
		if ( !knl_igt.want_kernel ) {
			knl_igt.want_kernel = TRUE;
			wake = TRUE;
		}
	} else if ( knl_igt.pm_ready && !knl_igt.parked && knl_igt.fw_users == 0 && knl_igt.active < 0
	  && igt_now_us() - knl_igt.fw_idle_since >= IDLE_US ) {
		park();
		if ( !knl_igt.fw_always ) {
			for ( k = 0; k < 3; k++ ) igt_wr(wells[k].set, MASK_DIS(FW_KERNEL));
			knl_igt.fw_held = FALSE;
		}
	}
	ISpinUnlock(&knl_igt.lock, &imask);
	if ( wake ) igt_pm_wake();
}

/* A step on the GT's word: up doubling while up comes, down likewise */
LOCAL void rps_step( UW ev )
{
	UINT	imask;
	INT	adj, f;

	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.parked ) {
		/* idle since: the word no longer holds, and parking set the frequency */
		ISpinUnlock(&knl_igt.lock, &imask);
		return;
	}
	knl_igt.fw_users++;			/* not parked: the wells are awake */
	adj = knl_igt.f_adj;
	f = (INT)knl_igt.f_cur;
	if ( ev & PM_RP_UP_THRESHOLD ) {
		adj = ( adj > 0 ) ? adj * 2 : 1;
		if ( f >= (INT)knl_igt.f_max ) adj = 0;
		knl_igt.n_up++;
	} else if ( ev & PM_RP_DOWN_THRESHOLD ) {
		adj = ( adj < 0 ) ? adj * 2 : -1;
		if ( f <= (INT)knl_igt.f_min ) adj = 0;
		knl_igt.n_down++;
	} else {
		adj = 0;
	}
	f += adj;
	if ( f < (INT)knl_igt.f_min ) f = (INT)knl_igt.f_min;
	if ( f > (INT)knl_igt.f_max ) f = (INT)knl_igt.f_max;
	freq_set((UINT)f);
	knl_igt.f_adj = adj;
	igt_wr(GT_IMR(2), igt_rd(GT_IMR(2)) & ~PM_EVENTS);
	if ( --knl_igt.fw_users == 0 ) knl_igt.fw_idle_since = igt_now_us();
	ISpinUnlock(&knl_igt.lock, &imask);
}

/* MHz of a frequency in units of 50/3 MHz */
Inline INT mhz( UINT f )
{
	return (INT)( f * 50 / FREQ_SCALE );
}

EXPORT INT igt_pm_text( char *t, INT max )
{
	char	b[256];
	INT	n;

	n = tm_sprintf((UB *)b, (CONST UB *)"RC6 %s, RPS %s, %s, asked %d MHz (RPn %d, RPe %d, RP1 %d, RP0 %d); "
		       "parked %ld times, woken %ld, steps up %ld down %ld",
		       knl_igt.rc6_on ? "on" : "off", knl_igt.rps_on ? "on" : "off",
		       knl_igt.parked ? "parked" : "busy", mhz(knl_igt.f_last), mhz(knl_igt.f_min),
		       mhz(knl_igt.f_eff), mhz(knl_igt.f_rp1), mhz(knl_igt.f_max), knl_igt.n_park,
		       knl_igt.n_wake, knl_igt.n_up, knl_igt.n_down);
	if ( n >= max ) n = max - 1;
	knl_memcpy(t, b, n);
	t[n] = '\0';
	return n;
}

LOCAL void pm_task( INT stacd, void *exinf )
{
	UD	jobs_seen = 0, t_last = igt_now_us();
	UW	rc6_last = igt_rd(GT_GFX_RC6);
	char	line[256];

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		UINT	imask;
		UW	ev, rc6;
		UD	jobs, now;
		BOOL	want;

		(void)tk_slp_tsk(1000);
		ISpinLock(&knl_igt.lock, &imask);
		ev = knl_igt.pm_iir;
		knl_igt.pm_iir = 0;
		want = knl_igt.want_kernel;
		knl_igt.want_kernel = FALSE;
		ISpinUnlock(&knl_igt.lock, &imask);
		if ( ev != 0 ) rps_step(ev);
		if ( want ) igt_to_kernel();
		if ( knl_igt.dead != NULL ) {
			tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
			igt_vm_reap();
			tk_unl_mtx(knl_igt.mtx);
		}
		if ( rc6_bad ) {
			rc6_bad = FALSE;
			tm_printf((UB *)"igt: power: the context RC6 saved was found corrupted (RC6_CTX_INFO 0): "
				  "RC6 off from now on, the wells kept awake\n");
		}
		now = igt_now_us();
		jobs = knl_igt.eng[IGT_RCS].jobs + knl_igt.eng[IGT_BCS].jobs;
		if ( now - t_last >= (UD)REPORT_MS * 1000 && jobs != jobs_seen ) {
			rc6 = igt_rd(GT_GFX_RC6);
			(void)igt_pm_text(line, sizeof(line));
			tm_printf((UB *)"igt: power: %s; %ld jobs, in RC6 %ld%% of the last %ld s, %ld PM interrupts, "
				  "%ld forcewake kicks, %ld stuck\n", line, jobs - jobs_seen,
				  (UD)(UW)( rc6 - rc6_last ) * 128 / 1000 / ( ( now - t_last ) / 1000 ),
				  ( now - t_last ) / 1000000, knl_igt.n_pm_irq, knl_igt.n_fw_kick, knl_igt.n_fw_stuck);
			tm_printf((UB *)"igt: contexts: %ld switches to the kernel's context; %ld spaces of ended processes "
				  "kept while an engine held them, %ld freed after it left them; %ld images read whole\n",
				  knl_igt.n_to_kernel, knl_igt.n_held, knl_igt.n_reaped, knl_igt.n_force);
			rc6_last = rc6;
			jobs_seen = jobs;
			t_last = now;
		}
	}
}

/* ---------------------------------------------------------------- setting up */

/*
 * Before anything else, the wells held: both off, the frequencies the
 * GT may run at read, RP1 asked for
 */
EXPORT void igt_pm_init( void )
{
	UW	cap, ctl, st, pg;

	ctl = igt_rd(RC_CONTROL);
	st = igt_rd(RC_STATE);
	pg = igt_rd(PG_ENABLE);
	igt_wr(PG_ENABLE, 0);
	igt_wr(RC_CONTROL, 0);
	igt_wr(RC_STATE, 0);
	igt_wr(RP_CONTROL, 0);

	cap = igt_rd(RP_STATE_CAP);
	knl_igt.rp0 = ( cap & 0xff ) * 50;
	knl_igt.rp1 = ( ( cap >> 8 ) & 0xff ) * 50;
	knl_igt.rpn = ( ( cap >> 16 ) & 0xff ) * 50;
	knl_igt.f_max = ( cap & 0xff ) * FREQ_SCALE;
	knl_igt.f_rp1 = ( ( cap >> 8 ) & 0xff ) * FREQ_SCALE;
	knl_igt.f_min = ( ( cap >> 16 ) & 0xff ) * FREQ_SCALE;
	knl_igt.f_eff = knl_igt.f_rp1;
	igt_wr(RPNSWREQ, FREQ_REQ(knl_igt.f_rp1));
	knl_igt.f_last = knl_igt.f_rp1;
	knl_igt.f_cur = knl_igt.f_rp1;
	udelay(2000);
	tm_printf((UB *)"igt: power: as found: RC_CONTROL %08x, RC_STATE %08x, PG_ENABLE %08x; both off now; "
		  "RP0 %d MHz, RP1 %d MHz, RPn %d MHz (cap %08x); RP1 asked for, running at %d MHz\n", ctl, st, pg,
		  knl_igt.rp0, knl_igt.rp1, knl_igt.rpn, cap, ( ( igt_rd(RPSTAT1) >> 23 ) & 0x1ff ) * 50 / FREQ_SCALE);
}

/* RC6 set up; the wells held */
LOCAL void rc6_start( void )
{
	static CONST UW	bases[] = { RCS_BASE, BCS_BASE, VCS_BASE, VECS_BASE };
	UW	ctx;
	INT	k;

	igt_wr(RC6_WAKE_RATE_LIMIT, 54 << 16);
	igt_wr(RC_EVALUATION_INTERVAL, 125000);
	igt_wr(RC_IDLE_HYSTERSIS, 25);
	for ( k = 0; k < 4; k++ ) igt_wr(bases[k] + RING_MAX_IDLE, 10);
	igt_wr(GUC_MAX_IDLE_COUNT, 0xa);
	igt_wr(RC_SLEEP, 0);
	igt_wr(MEDIA_PG_IDLE_HYSTERESIS, 250);
	igt_wr(RENDER_PG_IDLE_HYSTERESIS, 250);
	igt_wr(RC6_THRESHOLD, 37500);
	igt_wr(PG_ENABLE, RENDER_PG_EN | MEDIA_PG_EN);
	/* the automatic entry left off (RC_CONTROL 0): RC6 by hand when parked */

	ctx = igt_rd(RC6_CTX_INFO);
	if ( ctx == 0 ) {
		igt_wr(PG_ENABLE, 0);
		knl_igt.rc6_on = FALSE;
		tm_printf((UB *)"igt: power: RC6 NOT used: the context RC6 saves is corrupted (RC6_CTX_INFO 0)\n");
		return;
	}
	tm_printf((UB *)"igt: power: RC6 set up (RC6_CTX_INFO %08x, in RC6 so far %ld ms); asked for by hand after "
		  "%d ms idle, the wells let go then; render and media power gating on\n", ctx,
		  (UD)igt_rd(GT_GFX_RC6) * 128 / 100000, IDLE_US / 1000);
}

/* RPS set up; the wells held */
LOCAL void rps_start( void )
{
	UW	ddcc = 0, f;
	ER	er;
	INT	n = 0, bad = 0;

	if ( knl_igt.f_min == 0 || knl_igt.f_max < knl_igt.f_min ) {
		knl_igt.rps_on = FALSE;
		tm_printf((UB *)"igt: power: RPS NOT used: RPn %d MHz, RP0 %d MHz\n", knl_igt.rpn, knl_igt.rp0);
		return;
	}

	/* the efficient frequency, as the power controller has it */
	er = knl_igt_pcode_rw(PCODE_DYNAMIC_DUTY_CYCLE, 0, &ddcc);
	if ( er >= E_OK ) {
		f = ( ( ddcc >> 8 ) & 0xff ) * FREQ_SCALE;
		if ( f < knl_igt.f_min ) f = knl_igt.f_min;
		if ( f > knl_igt.f_max ) f = knl_igt.f_max;
		knl_igt.f_eff = f;
	}
	tm_printf((UB *)"igt: power: efficient frequency %d MHz (pcode %d, %08x)\n", mhz(knl_igt.f_eff), er, ddcc);

	/*
	 * The ring's frequency for each of the GT's, from the highest down,
	 * in units of 50 MHz both: as high as the GT's, no floor
	 */
	for ( f = knl_igt.f_max / FREQ_SCALE; er != E_NOEXS && f >= knl_igt.f_min / FREQ_SCALE; f-- ) {
		if ( knl_igt_pcode_rw(PCODE_WRITE_MIN_FREQ_TABLE, ( f << 16 ) | f, NULL) < E_OK ) bad++;
		n++;
		if ( f == 0 ) break;
	}
	tm_printf((UB *)"igt: power: the ring's frequency table: %d entries written, %d refused\n", n, bad);

	igt_wr(RC_VIDEO_FREQ, FREQ_REQ(knl_igt.f_rp1));
	igt_wr(RP_IDLE_HYSTERSIS, 0xa);
	/* from the lowest, the thresholds afresh */
	knl_igt.power_mode = -1;
	knl_igt.f_last = 0;
	freq_req(knl_igt.f_min, TRUE);
	knl_igt.f_cur = knl_igt.f_min;
	knl_igt.f_adj = 0;
}

/*
 * RC6 and RPS as cmdline.txt says, once the engines run; the wells
 * held by the start-up
 */
EXPORT void igt_pm_start( void )
{
	UINT	imask;
	T_CTSK	ct;

	knl_igt.rc6_on = ( igt_setting("igtrc6", 1) != 0 );
	knl_igt.rps_on = ( igt_setting("igtrps", 1) != 0 );
	tm_printf((UB *)"igt: power: RC6 %s, RPS %s (cmdline.txt igtrc6=0, igtrps=0 turn them off)\n",
		  knl_igt.rc6_on ? "asked for" : "OFF", knl_igt.rps_on ? "asked for" : "OFF");
	if ( knl_igt.rc6_on ) rc6_start();
	if ( knl_igt.rps_on ) rps_start();

	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.rps_on ) {
		knl_igt.parked = FALSE;
		unpark();
		knl_igt.n_unpark = 0;
	}
	knl_igt.fw_always = !knl_igt.rc6_on;
	knl_igt.fw_idle_since = igt_now_us();
	ISpinUnlock(&knl_igt.lock, &imask);

	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)pm_task;
	ct.itskpri = 12;
	ct.stksz = 8192;
	knl_igt.pm_tsk = tk_cre_tsk(&ct);
	if ( knl_igt.pm_tsk > 0 && tk_sta_tsk(knl_igt.pm_tsk, 0) < E_OK ) knl_igt.pm_tsk = 0;

	udelay(2000);
	tm_printf((UB *)"igt: power: RPS %s: asked %d MHz, running at %d MHz (RPSTAT1 %08x), RP_CONTROL %08x, "
		  "PMINTRMSK %08x, GT IER/IMR 2 %08x %08x; task %d\n", knl_igt.rps_on ? "on" : "off",
		  mhz(knl_igt.f_last), ( ( igt_rd(RPSTAT1) >> 23 ) & 0x1ff ) * 50 / FREQ_SCALE, igt_rd(RPSTAT1),
		  igt_rd(RP_CONTROL), igt_rd(PMINTRMSK), igt_rd(GT_IER(2)), igt_rd(GT_IMR(2)), knl_igt.pm_tsk);
	knl_igt.pm_ready = TRUE;
}

/*
 * After S3: what igt_pm_start set up, set up again; its task goes on
 * running. The caller holds the wells by igt_fw_init's reference.
 */
EXPORT void igt_pm_again( void )
{
	UINT	imask;

	if ( knl_igt.rc6_on ) rc6_start();
	if ( knl_igt.rps_on ) rps_start();
	ISpinLock(&knl_igt.lock, &imask);
	if ( knl_igt.rps_on ) {
		knl_igt.parked = FALSE;
		unpark();
	}
	knl_igt.fw_always = !knl_igt.rc6_on;
	knl_igt.fw_idle_since = igt_now_us();
	ISpinUnlock(&knl_igt.lock, &imask);
}

#endif /* X64 */
