/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpuidle.c
 *	The idle states of the processors: the deep sleep of an idle
 *	processor through MWAIT, the state chosen by how long it is expected
 *	to stay idle (design 03-platform 3.10.14)
 *
 *	The states: the processor's model's table of MWAIT hints with their
 *	exit latency and target residency (the time in the state that pays
 *	for entering it), each kept only when CPUID 5 lists sub-states of its
 *	C-state. A state deeper than C1 is kept only where the Local APIC's
 *	timer runs in every C-state (ARAT, CPUID 6 EAX bit 2), the timer being
 *	the only one that wakes a processor, and a state deeper than C2 only
 *	where the time stamp counter does not stop (CPUID 80000007h EDX bit
 *	8), the counter being the monotonic clock. Before them all, state 0
 *	polls: interrupts open, until a task is given or the time limit.
 *
 *	Where there is no MWAIT with break on interrupt, the model is not in
 *	the table, on a virtual machine, or the start-up setting says
 *	"intel_idle.max_cstate=0", the processor waits in HLT as before.
 *	"intel_idle.max_cstate=N" keeps the first N states of the table.
 *
 *	The choice (each time the idle loop would wait): the time to this
 *	processor's next timer deadline, made smaller by a correction factor
 *	learned from how long it really slept for deadlines of that order
 *	(6 buckets), and the typical interval of the last 8 sleeps when they
 *	are regular; the deepest state whose target residency is within the
 *	prediction. The kernel runs without a periodic tick, and the choice is
 *	made as for a kernel whose tick of TICK_NS is kept while the
 *	prediction is short: then a deadline at the next tick (apic.c) wakes
 *	the processor to choose again, so that it does not stay long in a
 *	shallow state. When the choice says the tick can stop, it stays
 *	stopped until the processor runs a task.
 *
 *	Each processor applies what is set once, in its own idle loop: the
 *	promotion of C1 to C1E off (MSR_IA32_POWER_CTL bit 1) where the table
 *	says so, and the clock's settings (hwp.c); again after a sleep.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <ts/board.h>
#include "sysdepend.h"
#include "../cpu/core/x86_64/sys_timer.h"
#include "tkernel/task.h"
#include "tkernel/wait.h"
#include "tkernel/time_calls.h"
#include <ts/pcie.h>

IMPORT void	(*knl_timer_fire_hook)( CBACK cb, void *arg );	/* timer.c */
IMPORT INT	knl_nvme_power_text( char *t );			/* device/nvme/nvme.c */
IMPORT INT	knl_usb_pm_text( char *t );			/* device/usb/usbman.c */
IMPORT INT	knl_igfx_wm_text( char *t );			/* device/disp/x86_64/disp_igfx.c */
IMPORT void	knl_igfx_dc_pause( BOOL pause );		/* device/disp/x86_64/disp_igfx_psr.c */
IMPORT UW	knl_igfx_dc_setting( void );
IMPORT UD	knl_apic_next_event( void );		/* apic.c */
IMPORT void	knl_apic_idle_tick( UD tsc );
IMPORT BOOL	knl_apic_any_pending( void );
IMPORT UD	knl_get_mono_ns( void );			/* sys_timer.c */
IMPORT volatile UW	knl_idle_irq[256];		/* apic.c */
IMPORT volatile UW	knl_idle_tmr[3];
IMPORT BOOL	knl_hwp_init( void );			/* hwp.c */
IMPORT void	knl_hwp_cpu_apply( void );

#define MSR_POWER_CTL		0x1fc
#define MSR_PKG_CST_CONFIG	0x0e2

#define NS_US		1000ULL
#define TICK_NS		1000000ULL		/* HZ 1000 */
#define RESOLUTION	1024U
#define DECAY		8U
#define MAX_INTERESTING	( 50000ULL * NS_US )
#define RESIDENCY_THRESHOLD_NS	( 15ULL * NS_US )
#define SAFE_TIMER_RANGE_NS	( 2ULL * TICK_NS )
#define POLL_MIN_NS	( 10ULL * NS_US )
#define POLL_MAX_NS	( TICK_NS / 16 )
#define POLL_RELAX_COUNT	200
#define NS_MAX		( (UD)0x7fffffffffffffffULL )
#define UINT_MAXV	0xffffffffU

typedef struct {
	CONST char	*name;
	UB		hint;		/* MWAIT's EAX */
	UH		exit_us;
	UH		target_us;
} CSTATE;

/* Skylake and Kaby Lake (the client models) */
LOCAL CONST CSTATE	skl_cstates[] = {
	{ "C1",  0x00, 2, 2 },
	{ "C1E", 0x01, 10, 20 },
	{ "C3",  0x10, 70, 100 },
	{ "C6",  0x20, 85, 200 },
	{ "C7s", 0x33, 124, 800 },
	{ "C8",  0x40, 200, 800 },
	{ "C9",  0x50, 480, 5000 },
	{ "C10", 0x60, 890, 5000 },
};

LOCAL CONST struct {
	UB		model;
	CONST CSTATE	*table;
	UB		n;
	BOOL		c1e_off;	/* the promotion of C1 to C1E turned off */
	BOOL		pkg_res;	/* the package's C-state residency counters of the client models */
} idle_models[] = {
	{ 0x4e, skl_cstates, 8, TRUE, TRUE },	/* Skylake L */
	{ 0x5e, skl_cstates, 8, TRUE, TRUE },	/* Skylake */
	{ 0x8e, skl_cstates, 8, TRUE, TRUE },	/* Kaby Lake L, Amber Lake, Whiskey Lake */
	{ 0x9e, skl_cstates, 8, TRUE, TRUE },	/* Kaby Lake, Coffee Lake */
};

typedef struct {
	CONST char	*name;
	UW		hint;
	UD		exit_ns, target_ns;
} STATE;

LOCAL STATE		st[IDLE_ST_N];		/* st[0] polls */
LOCAL INT		st_n = 0;		/* states in use, 0 none (HLT) */
LOCAL BOOL		c1e_off = FALSE;
LOCAL BOOL		pkg_res = FALSE;
LOCAL UD		poll_limit_ns = POLL_MAX_NS;
LOCAL volatile UW	idle_gen = 0;		/* 0: nothing to apply yet */
LOCAL UD		tick_cyc = 0;
LOCAL volatile UW	wake_none = 0, wake_task = 0;	/* sleeps that ended with no interrupt pending */
LOCAL BOOL		idlelog = FALSE;		/* "idlelog=1": the records of the platform's power too */

/* ---------------------------------------------------------------- the choice */

LOCAL UW which_bucket( UD ns )
{
	if ( ns < 10ULL * NS_US ) return 0;
	if ( ns < 100ULL * NS_US ) return 1;
	if ( ns < 1000ULL * NS_US ) return 2;
	if ( ns < 10000ULL * NS_US ) return 3;
	if ( ns < 100000ULL * NS_US ) return 4;
	return 5;
}

LOCAL void update_intervals( T_IDLECPU *d, UW us )
{
	d->intervals[d->interval_ptr++] = us;
	if ( d->interval_ptr >= IDLE_INTERVALS ) d->interval_ptr = 0;
}

/*
 * The average of the last intervals when they are regular (standard
 * deviation within 20us, or within a sixth of the average with three
 * quarters of them), outliers left out one at a time; UINT_MAXV when not
 */
LOCAL UW typical_interval( T_IDLECPU *d )
{
	D	value, min_thresh = -1, max_thresh = UINT_MAXV;
	UW	max, min, divisor;
	UD	avg, variance, avg_sq;
	INT	i;

	for ( ;; ) {
		max = 0;
		min = UINT_MAXV;
		avg = 0;
		variance = 0;
		divisor = 0;
		for ( i = 0; i < IDLE_INTERVALS; i++ ) {
			value = (D)d->intervals[i];
			if ( value <= min_thresh || value >= max_thresh ) continue;
			divisor++;
			avg += (UD)value;
			variance += (UD)value * (UD)value;
			if ( (UW)value > max ) max = (UW)value;
			if ( (UW)value < min ) min = (UW)value;
		}
		if ( max == 0 ) return UINT_MAXV;
		avg /= divisor;
		variance /= divisor;
		avg_sq = avg * avg;
		variance -= avg_sq;
		if ( variance <= 0xffffffffffffffffULL / 36 ) {
			if ( ( avg_sq > variance * 36 && divisor * 4 >= IDLE_INTERVALS * 3 ) || variance <= 400 ) {
				return (UW)avg;
			}
		}
		if ( divisor * 4 <= IDLE_INTERVALS * 3 ) return UINT_MAXV;
		if ( avg - min > max - avg ) {
			min_thresh = min;
		} else {
			max_thresh = max;
		}
	}
}

/* What the last sleep says of the prediction (its state, its length, whether the tick woke it) */
LOCAL void menu_update( T_IDLECPU *d, BOOL tick_wakeup )
{
	STATE	*target = &st[d->last_idx];
	UD	measured;
	UW	nf;

	if ( tick_wakeup && d->next_timer_ns > TICK_NS ) {
		measured = 9 * MAX_INTERESTING / 10;
	} else if ( d->last_idx == 0 && d->poll_limited ) {
		measured = d->next_timer_ns;
	} else {
		measured = d->last_res_ns;
		if ( measured > 2 * target->exit_ns ) {
			measured -= target->exit_ns;
		} else {
			measured /= 2;
		}
	}
	if ( measured > d->next_timer_ns ) measured = d->next_timer_ns;
	nf = d->correction[d->bucket];
	nf -= nf / DECAY;
	if ( d->next_timer_ns > 0 && measured < MAX_INTERESTING ) {
		nf += (UW)( (UD)RESOLUTION * measured / d->next_timer_ns );
	} else {
		nf += RESOLUTION;
	}
	d->correction[d->bucket] = nf;
	update_intervals(d, (UW)( measured / NS_US ));
}

/*
 * The state to enter: now and the next deadline in counts of the time
 * stamp counter (0 none). *stop_tick FALSE when the tick is to be kept.
 */
LOCAL INT menu_select( T_IDLECPU *d, UD now, UD next_dl, BOOL *stop_tick )
{
	UD	predicted, delta, delta_tick, timer_us;
	INT	i, idx;

	*stop_tick = TRUE;
	if ( d->needs_update ) {
		menu_update(d, d->tick_wakeup);
		d->needs_update = FALSE;
	} else if ( d->last_res_ns == 0 ) {
		update_intervals(d, UINT_MAXV);
	}

	predicted = (UD)typical_interval(d) * NS_US;
	if ( predicted > RESIDENCY_THRESHOLD_NS || d->tick_stopped ) {
		delta = ( next_dl == 0 ) ? NS_MAX : ( next_dl > now ) ? knl_cycles_to_ns(next_dl - now) : 0;
		if ( d->tick_stopped ) {
			delta_tick = delta;
		} else {
			UD	to_tick = tick_cyc - now % tick_cyc;

			delta_tick = knl_cycles_to_ns(to_tick);
			if ( delta_tick > delta ) delta_tick = delta;
		}
		d->next_timer_ns = delta;
		d->bucket = which_bucket(delta);
		timer_us = (UD)( ( (unsigned __int128)RESOLUTION * DECAY * NS_US / 2
				 + (unsigned __int128)delta * d->correction[d->bucket] )
				 / ( RESOLUTION * DECAY * NS_US ) );
		if ( timer_us * NS_US < predicted ) predicted = timer_us * NS_US;
		if ( d->tick_stopped && predicted < TICK_NS && d->next_timer_ns > SAFE_TIMER_RANGE_NS ) {
			predicted = d->next_timer_ns;
		}
	} else {
		d->next_timer_ns = NS_MAX;
		delta_tick = TICK_NS / 2;
		d->bucket = IDLE_BUCKETS - 1;
	}

	/* no latency limit is asked (PM QoS's default) */
	if ( st_n > 1 && d->next_timer_ns < st[1].target_ns ) {
		*stop_tick = FALSE;		/* state 0 polls */
		return 0;
	}

	idx = -1;
	for ( i = 0; i < st_n; i++ ) {
		STATE	*s = &st[i];

		if ( idx == -1 ) idx = i;
		if ( s->target_ns <= predicted ) {
			idx = i;
			continue;
		}
		if ( idx == 0 && s->target_ns < RESIDENCY_THRESHOLD_NS && s->target_ns <= d->next_timer_ns
		  && s->exit_ns <= predicted ) {
			predicted = s->target_ns;
			idx = i;
			break;
		}
		if ( predicted < TICK_NS ) break;
		if ( !d->tick_stopped ) {
			predicted = st[idx].target_ns;
			break;
		}
		if ( st[idx].target_ns < TICK_NS && s->target_ns <= delta_tick ) idx = i;
		return idx;
	}
	if ( idx == -1 ) idx = 0;

	if ( ( idx == 0 || predicted < TICK_NS ) && !d->tick_stopped ) {
		*stop_tick = FALSE;
		if ( idx > 0 && st[idx].target_ns > delta_tick ) {
			for ( i = idx - 1; i >= 0; i-- ) {
				idx = i;
				if ( st[i].target_ns <= delta_tick ) break;
			}
		}
	}
	return idx;
}

/* ---------------------------------------------------------------- entering a state */

/* The settings, on this processor (interrupts disabled) */
LOCAL void cpu_apply( void )
{
	if ( st_n > 0 && c1e_off ) {
		wrmsr(MSR_POWER_CTL, rdmsr(MSR_POWER_CTL) & ~2ULL);
	}
	knl_hwp_cpu_apply();
}

/*
 * From the idle loop (low_pow, cpu_cntl.c) with interrupts disabled:
 * FALSE when the idle states are not in use and the loop is to HLT
 */
EXPORT BOOL knl_cpuidle_enter( void )
{
	T_PCPU		*p = knl_pcpu();
	T_IDLECPU	*d = &p->idle;
	UW		g = idle_gen;
	UD		t0, t1, diff;
	BOOL		stop_tick;
	INT		idx, i;

	if ( g == 0 ) return FALSE;
	if ( d->gen != g ) {
		cpu_apply();
		if ( d->correction[0] == 0 ) {
			for ( i = 0; i < IDLE_BUCKETS; i++ ) d->correction[i] = RESOLUTION * DECAY;
		}
		d->gen = g;
	}
	if ( st_n == 0 ) return FALSE;

	/* the tick runs again once the processor has run a task */
	d->tick_wakeup = p->apic_idle_got;
	if ( p->ctxsw_cnt != d->ctxsw_at ) {
		d->ctxsw_at = p->ctxsw_cnt;
		d->tick_stopped = FALSE;
		d->tick_wakeup = FALSE;		/* a tick while a task ran did not end a sleep */
	}
	p->apic_idle_got = FALSE;
	t0 = ReadSysCounterSync();
	idx = menu_select(d, t0, knl_apic_next_event(), &stop_tick);
	if ( stop_tick || d->tick_stopped ) {
		d->tick_stopped = TRUE;
		knl_apic_idle_tick(0);
	} else {
		knl_apic_idle_tick(t0 - t0 % tick_cyc + tick_cyc);
	}

	d->poll_limited = FALSE;
	t0 = ReadSysCounterSync();
	if ( idx == 0 ) {
		UD	limit = t0 + knl_ns_to_cycles_up(poll_limit_ns);
		UW	n = 0;

		Asm("sti" ::: "memory");
		while ( __atomic_load_n(&p->schedtsk, __ATOMIC_ACQUIRE) == NULL ) {
			CpuRelax();
			if ( n++ < POLL_RELAX_COUNT ) continue;
			n = 0;
			if ( ReadSysCounterSync() > limit ) {
				d->poll_limited = TRUE;
				break;
			}
		}
		Asm("cli" ::: "memory");
	} else {
		Asm("monitor" :: "a"(&p->schedtsk), "c"(0), "d"(0));
		if ( __atomic_load_n(&p->schedtsk, __ATOMIC_ACQUIRE) == NULL ) {
			Asm("mwait" :: "a"(st[idx].hint), "c"(1) : "memory");
		}
	}
	t1 = ReadSysCounterSync();
	if ( idx > 0 && !knl_apic_any_pending() ) {
		/* woken with no interrupt: a task given through the monitored line, or an NMI, an SMI */
		__atomic_add_fetch(( p->schedtsk != NULL ) ? &wake_task : &wake_none, 1, __ATOMIC_RELAXED);
		if ( idlelog && p->schedtsk == NULL ) d->wake_none++;
	}

	/* the time in the state, against its target residency */
	diff = knl_cycles_to_ns(t1 - t0);
	d->last_res_ns = diff;
	d->time_ns[idx] += diff;
	d->usage[idx]++;
	if ( diff < st[idx].target_ns ) {
		if ( idx > 0 ) d->above[idx]++;
	} else if ( diff > st[idx].exit_ns && idx + 1 < st_n ) {
		if ( diff - st[idx].exit_ns >= st[idx + 1].target_ns ) d->below[idx]++;
	}
	d->last_idx = idx;
	d->needs_update = TRUE;
	return TRUE;
}

/* ---------------------------------------------------------------- the records */

LOCAL UD	last_usage[IDLE_ST_N], last_time[IDLE_ST_N], last_above[IDLE_ST_N], last_below[IDLE_ST_N];
LOCAL UD	last_pc[7], last_tsc = 0;
LOCAL CONST UW	pc_msr[7] = { 0x60d, 0x3f8, 0x3f9, 0x3fa, 0x630, 0x631, 0x632 };
LOCAL CONST char * CONST pc_name[7] = { "PC2", "PC3", "PC6", "PC7", "PC8", "PC9", "PC10" };

/*
 * Since the last call, for every processor together: each state's
 * entries, time (ms), entries shorter than its target residency (above)
 * and long enough for the next (below); with the package's C-state
 * residency (% of the time) where the model has the counters
 */
EXPORT INT knl_cpuidle_text( char *t )
{
	UD	u, tm, ab, be, tsc, pc;
	INT	i, j, n = 0;

	if ( st_n == 0 ) return tm_sprintf((UB *)t, (CONST UB *)"; idle HLT");
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; idle");
	for ( i = 0; i < st_n; i++ ) {
		u = tm = ab = be = 0;
		for ( j = 0; j < knl_num_prc; j++ ) {
			T_IDLECPU	*d = &knl_pcpu_of(j)->idle;

			u += d->usage[i];
			tm += d->time_ns[i];
			ab += d->above[i];
			be += d->below[i];
		}
		n += tm_sprintf((UB *)t + n, (CONST UB *)" %s %d/%dms/%d/%d", st[i].name, (INT)( u - last_usage[i] ),
				(INT)( ( tm - last_time[i] ) / 1000000ULL ), (INT)( ab - last_above[i] ), (INT)( be - last_below[i] ));
		last_usage[i] = u;
		last_time[i] = tm;
		last_above[i] = ab;
		last_below[i] = be;
	}
	if ( pkg_res ) {
		tsc = ReadSysCounterSync();
		n += tm_sprintf((UB *)t + n, (CONST UB *)"; package");
		for ( i = 0; i < 7; i++ ) {
			pc = rdmsr(pc_msr[i]);
			if ( last_tsc != 0 && tsc > last_tsc ) {
				n += tm_sprintf((UB *)t + n, (CONST UB *)" %s %d%%", pc_name[i],
						(INT)( ( pc - last_pc[i] ) * 100 / ( tsc - last_tsc ) ));
			}
			last_pc[i] = pc;
		}
		last_tsc = tsc;
	}
	return n;
}

/* ---------------------------------------------------------------- what wakes them, and what runs */

#define WK_TICK		256		/* the timer: the idle states' tick */
#define WK_LOCKUP	257		/* the timer: the lockup checks' period */
#define WK_SYSTIMER	258		/* the timer: the system timer */
#define WK_NONE		259		/* no interrupt: an NMI, an SMI */
#define WK_TASK		260		/* no interrupt: a task given through the monitored line */
#define WK_N		261
#define WK_TOP		10
#define TSK_TOP		6
#define CPU_LOG		16		/* the processors told one by one */

LOCAL UW	wk_last[WK_N];
LOCAL UD	busy_last[CPU_LOG], idle_last[CPU_LOG], wall_last = 0;
LOCAL UD	run_last[CNF_MAX_TSKID];

LOCAL UW wk_now( INT k )
{
	if ( k < 256 ) return knl_idle_irq[k];
	if ( k <= WK_SYSTIMER ) return knl_idle_tmr[k - WK_TICK];
	return ( k == WK_NONE ) ? wake_none : wake_task;
}

LOCAL INT wk_name( char *t, INT k )
{
	switch ( k ) {
	case WK_TICK:		return tm_sprintf((UB *)t, (CONST UB *)"tick");
	case WK_LOCKUP:		return tm_sprintf((UB *)t, (CONST UB *)"lockup-period");
	case WK_SYSTIMER:	return tm_sprintf((UB *)t, (CONST UB *)"systimer");
	case WK_NONE:		return tm_sprintf((UB *)t, (CONST UB *)"no-interrupt");
	case WK_TASK:		return tm_sprintf((UB *)t, (CONST UB *)"monitor");
	}
	if ( k == INTVEC_TIMER ) return tm_sprintf((UB *)t, (CONST UB *)"timer");
	if ( k >= INTVEC_IPI(0) && k < INTVEC_IPI(SGI_NUM) ) return tm_sprintf((UB *)t, (CONST UB *)"ipi%d", k - INTVEC_IPI(0));
	if ( k >= INTVEC_GSI(0) && k < INTVEC_GSI(INTVEC_GSI_NUM) ) return tm_sprintf((UB *)t, (CONST UB *)"gsi%d", k - INTVEC_GSI(0));
	return tm_sprintf((UB *)t, (CONST UB *)"v%02x", k);
}

/*
 * Since the last call: what woke the processors while they had no task
 * (the most frequent first; the timer's interrupts split by the deadline
 * due), each processor's time asleep in the idle states, running tasks
 * (interrupts taken meanwhile included) and the rest (interrupts taken
 * while idle, the idle loop, HLT where the states are not used), and
 * the tasks that ran the most (ID, process, entry, % of one processor)
 */
EXPORT INT knl_cpuidle_wake_text( char *t )
{
	UW	d[WK_N], cnt;
	INT	i, j, k, n = 0, top[WK_TOP], ttop[TSK_TOP];
	UD	tval[TSK_TOP], now = ReadSysCounterSync(), wall;
	T_PCPU	*p;

	for ( k = 0; k < WK_N; k++ ) {
		cnt = wk_now(k);
		d[k] = cnt - wk_last[k];
		wk_last[k] = cnt;
	}
	for ( i = 0; i < WK_TOP; i++ ) {
		top[i] = -1;
		for ( k = 0; k < WK_N; k++ ) {
			if ( d[k] == 0 || ( k == INTVEC_TIMER && d[k] <= d[WK_TICK] + d[WK_LOCKUP] + d[WK_SYSTIMER] ) ) continue;
			for ( j = 0; j < i && top[j] != k; j++ ) ;
			if ( j < i ) continue;
			if ( top[i] < 0 || d[k] > d[top[i]] ) top[i] = k;
		}
		if ( top[i] < 0 ) break;
		n += tm_sprintf((UB *)t + n, (CONST UB *)( ( i == 0 ) ? "; woken by" : "," ));
		n += tm_sprintf((UB *)t + n, (CONST UB *)" ");
		n += wk_name(t + n, top[i]);
		n += tm_sprintf((UB *)t + n, (CONST UB *)" %d", (INT)d[top[i]]);
	}

	wall = ( wall_last != 0 && now > wall_last ) ? now - wall_last : 0;
	for ( i = 0; i < knl_num_prc && i < CPU_LOG; i++ ) {
		UD	busy, idle = 0, b, s;

		p = knl_pcpu_of(i);
		busy = __atomic_load_n(&p->acct_busy, __ATOMIC_RELAXED);
		if ( p->acct_tsk != NULL && now > p->acct_at ) busy += now - p->acct_at;
		for ( k = 0; k < IDLE_ST_N; k++ ) idle += p->idle.time_ns[k];
		if ( wall != 0 ) {
			UW	w[3];

			for ( k = 0; k < 3; k++ ) {
				w[k] = p->idle.wake_tmr[k] - p->idle.wake_tmr_last[k];
				p->idle.wake_tmr_last[k] = p->idle.wake_tmr[k];
			}
			b = ( busy - busy_last[i] ) * 100 / wall;
			s = knl_ns_to_cycles_up(idle - idle_last[i]) * 100 / wall;
			/* the timer's wakes of each kind, and where its last lockup period found it (task, RIP) */
			n += tm_sprintf((UB *)t + n, (CONST UB *)"%s PRC%d asleep %d%% tasks %d%% rest %d%% (tick %d, systimer %d, "
					"lockup %d; at task %d %lx)", ( i == 0 ) ? ";" : ",", i + 1, (INT)s, (INT)b,
					(INT)( ( s + b < 100 ) ? 100 - s - b : 0 ), (INT)w[0], (INT)w[2], (INT)w[1],
					(INT)p->lk.s_tsk, (UD)p->lk.s_pc);
		}
		busy_last[i] = busy;
		idle_last[i] = idle;
	}

	/* the tasks, the running ones counted up to now */
	for ( i = 0; i < TSK_TOP; i++ ) {
		ttop[i] = -1;
		tval[i] = 0;
	}
	for ( k = 0; k < CNF_MAX_TSKID; k++ ) {
		TCB	*c = &knl_tcb_table[k];
		UD	r, dr;

		if ( c->state == TS_NONEXIST ) {
			run_last[k] = 0;
			continue;
		}
		r = c->runcyc;
		for ( i = 0; i < knl_num_prc; i++ ) {
			p = knl_pcpu_of(i);
			if ( p->acct_tsk == c && now > p->acct_at ) r += now - p->acct_at;
		}
		dr = ( r > run_last[k] ) ? r - run_last[k] : 0;
		run_last[k] = r;
		if ( wall == 0 || dr == 0 ) continue;
		for ( i = 0; i < TSK_TOP && dr <= tval[i]; i++ ) ;
		if ( i == TSK_TOP ) continue;
		for ( j = TSK_TOP - 1; j > i; j-- ) {
			ttop[j] = ttop[j - 1];
			tval[j] = tval[j - 1];
		}
		ttop[i] = k;
		tval[i] = dr;
	}
	for ( i = 0; i < TSK_TOP && ttop[i] >= 0; i++ ) {
		TCB	*c = &knl_tcb_table[ttop[i]];

		n += tm_sprintf((UB *)t + n, (CONST UB *)"%s task %d (process %d, at %lx) %d%%",
				( i == 0 ) ? "; tasks:" : ",", (INT)c->tskid, (INT)c->owner, (UD)(UBINT)c->task,
				(INT)( tval[i] * 100 / wall ));
	}
	wall_last = now;
	return n;
}

/* ---------------------------------------------------------------- the platform's power, "idlelog=1" only */

/*
 * The timer's events as they fire (timer.c's hook), counted by what they
 * call: a task's timed wait by the task, a cyclic or alarm handler by
 * the handler, anything else by its callback. Only PRC 1 runs the queue.
 */
#define TF_N		32
#define TF_TOP		8
#define TF_TASK		1
#define TF_CYC		2
#define TF_ALM		3
#define TF_CB		4

typedef struct {
	UB	kind;
	UH	tskid;
	UD	addr;
	UW	n, last;
} TFIRE;

LOCAL TFIRE	tf[TF_N];
LOCAL UW	tf_other = 0, tf_other_last = 0;

LOCAL void timer_fire_note( CBACK cb, void *arg )
{
	UB	kind = TF_CB;
	UH	tid = 0;
	UD	a = (UD)(UBINT)cb;
	INT	i;

	if ( cb == (CBACK)knl_wait_tmout || cb == (CBACK)knl_wait_release_tmout ) {
		kind = TF_TASK;
		tid = (UH)( (TCB *)arg )->tskid;
		a = (UD)(UBINT)( (TCB *)arg )->task;
	} else if ( cb == (CBACK)knl_call_cychdr ) {
		kind = TF_CYC;
		a = (UD)(UBINT)( (CYCCB *)arg )->cychdr;
	} else if ( cb == (CBACK)knl_call_almhdr ) {
		kind = TF_ALM;
		a = (UD)(UBINT)( (ALMCB *)arg )->almhdr;
	}
	for ( i = 0; i < TF_N; i++ ) {
		if ( tf[i].kind == 0 ) {
			tf[i].kind = kind;
			tf[i].tskid = tid;
			tf[i].addr = a;
		}
		if ( tf[i].kind == kind && tf[i].tskid == tid && tf[i].addr == a ) {
			tf[i].n++;
			return;
		}
	}
	tf_other++;
}

LOCAL INT timer_fire_text( char *t )
{
	UW	d[TF_N];
	INT	i, j, k, n = 0, top[TF_TOP];

	for ( i = 0; i < TF_N; i++ ) {
		UW	c = tf[i].n;

		d[i] = c - tf[i].last;
		tf[i].last = c;
	}
	for ( k = 0; k < TF_TOP; k++ ) {
		top[k] = -1;
		for ( i = 0; i < TF_N; i++ ) {
			if ( d[i] == 0 ) continue;
			for ( j = 0; j < k && top[j] != i; j++ ) ;
			if ( j < k ) continue;
			if ( top[k] < 0 || d[i] > d[top[k]] ) top[k] = i;
		}
		if ( top[k] < 0 ) break;
		i = top[k];
		n += tm_sprintf((UB *)t + n, (CONST UB *)"%s ", ( k == 0 ) ? "; timer events" : ",");
		switch ( tf[i].kind ) {
		case TF_TASK:	n += tm_sprintf((UB *)t + n, (CONST UB *)"task %d (at %lx)", (INT)tf[i].tskid, tf[i].addr); break;
		case TF_CYC:	n += tm_sprintf((UB *)t + n, (CONST UB *)"cyclic %lx", tf[i].addr); break;
		case TF_ALM:	n += tm_sprintf((UB *)t + n, (CONST UB *)"alarm %lx", tf[i].addr); break;
		default:	n += tm_sprintf((UB *)t + n, (CONST UB *)"callback %lx", tf[i].addr); break;
		}
		n += tm_sprintf((UB *)t + n, (CONST UB *)" %d", (INT)d[i]);
	}
	if ( tf_other != tf_other_last ) {
		n += tm_sprintf((UB *)t + n, (CONST UB *)", others %d", (INT)( tf_other - tf_other_last ));
		tf_other_last = tf_other;
	}
	return n;
}

/*
 * The PCH's power management controller (Sunrise Point LP, 8086:9D21 at
 * 0:1F.2), read only, at the offsets of its PWRMBASE window: each IP's
 * latency tolerance (LTR, snoop and non-snoop in ns, 0 when it asks for
 * none), the power gating of the IPs (PPFEAR, a bit set when the IP is
 * off), and the time in SLP_S0 (counts of 104 us)
 */
#define PMC_DEV_SPT	0x9d21
#define PMC_PWRMBASE	0x48
#define PMC_SLP_S0	0x13c
#define PMC_LTR_IGNORE	0x30c
#define PMC_PPFEAR0	0x590
#define PMC_PPFEAR_N	5
#define PMC_SLP_S0_US	104

LOCAL CONST struct {
	CONST char	*name;
	UH		off;
} pmc_ltr[] = {
	{ "SPA", 0x360 }, { "SPB", 0x364 }, { "SATA", 0x368 }, { "GBE", 0x36c }, { "XHCI", 0x370 },
	{ "ME", 0x378 }, { "EVA", 0x37c }, { "SPC", 0x380 }, { "AZ", 0x384 }, { "LPSS", 0x38c },
	{ "CAM", 0x390 }, { "SPD", 0x394 }, { "SPE", 0x398 }, { "ESPI", 0x39c }, { "SCC", 0x3a0 },
	{ "ISH", 0x3a4 }, { "PLATFORM", 0x350 }, { "AGGREGATE", 0x354 },
};

LOCAL CONST char * CONST pmc_pfear[PMC_PPFEAR_N * 8] = {
	"PMC", "OPI-DMI", "SPI", "XHCI", "SPA", "SPB", "SPC", "GBE",
	"SATA", "HDA-PGD0", "HDA-PGD1", "HDA-PGD2", "HDA-PGD3", "RSVD", "LPSS", "LPC",
	"SMB", "ISH", "P2SB", "DFX", "SCC", "RSVD", "FUSE", "CAMERA",
	"RSVD", "USB3-OTG", "EXI", "CSE", "CSME_KVM", "CSME_PMT", "CSME_CLINK", "CSME_PTIO",
	"CSME_USBR", "CSME_SUSRAM", "CSME_SMT", "RSVD", "CSME_SMS2", "CSME_SMS1", "CSME_RTC", "CSME_PSF",
};

LOCAL UBINT	pmc_base = 0;
LOCAL INT	pmc_state = 0;		/* 0 not looked for, 1 there, -1 not this PCH */
LOCAL UW	slp_s0_last = 0;

LOCAL BOOL pmc_find( void )
{
	UW	id;
	UD	bar;

	if ( pmc_state != 0 ) return (BOOL)( pmc_state > 0 );
	pmc_state = -1;
	id = ts_pcie_cfg_read(0, 0x1f, 2, PCI_VENDOR_ID, 4);
	if ( ( id & 0xffff ) != 0x8086 || ( id >> 16 ) != PMC_DEV_SPT ) return FALSE;
	bar = ts_pcie_cfg_read(0, 0x1f, 2, PMC_PWRMBASE, 4) & ~0xfffU;
	if ( bar == 0 ) return FALSE;
	pmc_base = (UBINT)DEV_BASE(bar);
	pmc_state = 1;
	return TRUE;
}

/* The LTR as ns: 10 bits of value, 3 of scale (2^(5 * scale) ns), and whether it is asked for */
LOCAL UD ltr_ns( UH v, BOOL req )
{
	UW	scale = ( v >> 10 ) & 7;

	if ( !req || scale > 5 ) return 0;
	return (UD)( v & 0x3ff ) << ( 5 * scale );
}

LOCAL INT pmc_text( char *t )
{
	INT	i, b, n = 0;
	UW	r, s0;
	UB	pf;

	if ( !pmc_find() ) {
		return tm_sprintf((UB *)t, (CONST UB *)"; PMC %08x not known", ts_pcie_cfg_read(0, 0x1f, 2, PCI_VENDOR_ID, 4));
	}
	s0 = in_w(pmc_base + PMC_SLP_S0);
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; SLP_S0 %d ms; LTR ignore %08x; LTR (non-snoop/snoop us)",
			(INT)( (UD)( s0 - slp_s0_last ) * PMC_SLP_S0_US / 1000 ), in_w(pmc_base + PMC_LTR_IGNORE));
	slp_s0_last = s0;
	for ( i = 0; i < (INT)( sizeof(pmc_ltr) / sizeof(pmc_ltr[0]) ); i++ ) {
		r = in_w(pmc_base + pmc_ltr[i].off);
		n += tm_sprintf((UB *)t + n, (CONST UB *)" %s %d/%d", pmc_ltr[i].name,
				(INT)( ltr_ns((UH)( r >> 16 ), (BOOL)( ( r >> 31 ) & 1 )) / 1000 ),
				(INT)( ltr_ns((UH)r, (BOOL)( ( r >> 15 ) & 1 )) / 1000 ));
	}
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; IPs on:");
	for ( i = 0; i < PMC_PPFEAR_N; i++ ) {
		pf = in_b(pmc_base + PMC_PPFEAR0 + i);
		for ( b = 0; b < 8; b++ ) {
			if ( ( pf & ( 1U << b ) ) == 0 ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %s", pmc_pfear[i * 8 + b]);
		}
	}
	return n;
}

/* SMIs since the last call (MSR_SMI_COUNT), and each processor's sleeps that ended with no interrupt pending */
#define MSR_SMI_COUNT	0x34
LOCAL UW	smi_last = 0;

LOCAL INT smi_text( char *t )
{
	UW	s = (UW)rdmsr(MSR_SMI_COUNT);
	INT	i, n;

	n = tm_sprintf((UB *)t, (CONST UB *)"; SMI %d; no-interrupt", (INT)( s - smi_last ));
	smi_last = s;
	for ( i = 0; i < knl_num_prc && i < CPU_LOG; i++ ) {
		T_IDLECPU	*d = &knl_pcpu_of(i)->idle;

		n += tm_sprintf((UB *)t + n, (CONST UB *)" PRC%d %d", i + 1, (INT)( d->wake_none - d->wake_none_last ));
		d->wake_none_last = d->wake_none;
	}
	return n;
}

/*
 * Once: the display's DC states and PSR (the GPU at 0:2.0, its first
 * window), and each PCI function's power and link: the PCI power state
 * (PMCSR), ASPM supported and on (Link Capabilities, Link Control), LTR
 * supported and on (Device Capabilities 2, Device Control 2), the L1
 * substates supported and on (the L1 PM Substates capability); then the
 * NVMe controllers' autonomous power states. Read only.
 */
#define GPU_DC_STATE_EN		0x45504
#define GPU_DMC_DC3_DC5		0x80030
#define GPU_DMC_DC5_DC6		0x8002c
#define GPU_PSR_CTL		0x6f800		/* the eDP transcoder's */
#define GPU_PSR_STATUS		0x6f840
#define PCI_CAP_PM		0x01
#define PCI_EXT_L1SS		0x1e
#define PCI_EXT_LTR		0x18

LOCAL UINT ext_cap_find( UINT bus, UINT dev, UINT fn, UINT id )
{
	UINT	off = 0x100;
	INT	i;

	for ( i = 0; i < 48 && off >= 0x100; i++ ) {
		UW	h = ts_pcie_cfg_read(bus, dev, fn, off, 4);

		if ( h == 0 || h == 0xffffffffU ) return 0;
		if ( ( h & 0xffff ) == id ) return off;
		off = ( h >> 20 ) & 0xffc;
	}
	return 0;
}

/* The Intel GPU's first window (0:2.0), 0 when there is none */
LOCAL UBINT gpu_base( void )
{
	UD	bar;

	if ( ( ts_pcie_cfg_read(0, 2, 0, PCI_VENDOR_ID, 4) & 0xffff ) != 0x8086
	  || ( ts_pcie_cfg_read(0, 2, 0, 8, 4) >> 24 ) != 0x03 ) return 0;
	bar = ( (UD)ts_pcie_cfg_read(0, 2, 0, PCI_BAR0 + 4, 4) << 32 ) | ( ts_pcie_cfg_read(0, 2, 0, PCI_BAR0, 4) & ~0xfU );
	return ( bar == 0 ) ? 0 : (UBINT)DEV_BASE(bar);
}

/*
 * The display's FIFO watermarks of the pipes that are on (PIPECONF bit
 * 31), as the firmware or the driver left them: plane 1's and the
 * cursor's levels 0 to 7 and their transition level (bit 31 on, the
 * blocks and lines below), with what they are worked out from: the pipe's
 * source size, plane 1's control (format), size and DDB allocation, the
 * timings of the transcoders (A's and eDP's), the link M/N of each, the
 * DPLLs' link rates and CDCLK. SAGV and the memory latencies are the
 * power controller's (a mailbox request): not read.
 */
#define GPU_PIPECONF(p)		( 0x70008 + 0x1000 * (p) )
#define GPU_PIPESRC(p)		( 0x6001c + 0x1000 * (p) )
#define GPU_PIPECONF_EDP	0x7f008
#define GPU_TRANS_DDI_EDP	0x6f400
#define GPU_PLANE_CTL(p)	( 0x70180 + 0x1000 * (p) )
#define GPU_PLANE_SIZE(p)	( 0x70190 + 0x1000 * (p) )
#define GPU_PLANE_BUF(p)	( 0x7027c + 0x1000 * (p) )
#define GPU_PLANE_WM(p, l)	( 0x70240 + 0x1000 * (p) + 4 * (l) )
#define GPU_PLANE_WM_TRANS(p)	( 0x70268 + 0x1000 * (p) )
#define GPU_CUR_WM(p, l)	( 0x70140 + 0x1000 * (p) + 4 * (l) )
#define GPU_CUR_WM_TRANS(p)	( 0x70168 + 0x1000 * (p) )
#define GPU_TRANS_A		0x60000
#define GPU_TRANS_EDP		0x6f000
#define GPU_DPLL_CTRL1		0x6c058
#define GPU_CDCLK_CTL		0x46000

LOCAL void watermarks_tell( UBINT g )
{
	static char	t[768];
	INT		p, l, k;

	UW	ddi = in_w(g + GPU_TRANS_DDI_EDP), sel = ( ddi >> 12 ) & 7;
	INT	edp_pipe = ( ( ddi & 0x80000000U ) == 0 ) ? -1 : ( sel == 0 || sel == 4 ) ? 0 : ( sel == 5 ) ? 1 : ( sel == 6 ) ? 2 : -1;

	for ( p = 0; p < 3; p++ ) {
		UW	cf = in_w(g + GPU_PIPECONF(p));

		/* a pipe the eDP transcoder takes is on by the eDP transcoder's PIPECONF */
		if ( p == edp_pipe && ( in_w(g + GPU_PIPECONF_EDP) & 0x80000000U ) != 0 ) cf = in_w(g + GPU_PIPECONF_EDP);
		if ( ( cf & 0x80000000U ) == 0 ) continue;
		k = tm_sprintf((UB *)t, (CONST UB *)"idle: display pipe %c on (PIPECONF %08x), source %08x; plane 1 control %08x "
			       "size %08x DDB %08x; WM", 'A' + p, cf, in_w(g + GPU_PIPESRC(p)), in_w(g + GPU_PLANE_CTL(p)),
			       in_w(g + GPU_PLANE_SIZE(p)), in_w(g + GPU_PLANE_BUF(p)));
		for ( l = 0; l < 8; l++ ) k += tm_sprintf((UB *)t + k, (CONST UB *)" %08x", in_w(g + GPU_PLANE_WM(p, l)));
		k += tm_sprintf((UB *)t + k, (CONST UB *)" trans %08x; cursor WM", in_w(g + GPU_PLANE_WM_TRANS(p)));
		for ( l = 0; l < 8; l++ ) k += tm_sprintf((UB *)t + k, (CONST UB *)" %08x", in_w(g + GPU_CUR_WM(p, l)));
		k += tm_sprintf((UB *)t + k, (CONST UB *)" trans %08x", in_w(g + GPU_CUR_WM_TRANS(p)));
		tm_printf((UB *)"%s\n", t);
	}
	tm_printf((UB *)"idle: display timings: transcoder A htotal %08x hsync %08x vtotal %08x link M %08x N %08x; "
		  "eDP htotal %08x hsync %08x vtotal %08x link M %08x N %08x; DPLL_CTRL1 %08x, CDCLK_CTL %08x\n",
		  in_w(g + GPU_TRANS_A), in_w(g + GPU_TRANS_A + 8), in_w(g + GPU_TRANS_A + 0x0c),
		  in_w(g + GPU_TRANS_A + 0x40), in_w(g + GPU_TRANS_A + 0x44), in_w(g + GPU_TRANS_EDP),
		  in_w(g + GPU_TRANS_EDP + 8), in_w(g + GPU_TRANS_EDP + 0x0c), in_w(g + GPU_TRANS_EDP + 0x40),
		  in_w(g + GPU_TRANS_EDP + 0x44), in_w(g + GPU_DPLL_CTRL1), in_w(g + GPU_CDCLK_CTL));
}

/* The GT's time in RC6 since the last call (ms; units of 1.28 us); nothing without the GPU */
#define GPU_GT_RC6		0x138108
LOCAL UW	rc6_last = 0;

LOCAL INT rc6_text( char *t, BOOL intel )
{
	UBINT	g = intel ? gpu_base() : 0;
	UW	r;
	INT	n;

	if ( g == 0 ) return 0;
	r = in_w(g + GPU_GT_RC6);
	n = tm_sprintf((UB *)t, (CONST UB *)"; GT in RC6 %d ms", (INT)( (UD)( r - rc6_last ) * 128 / 100000 ));
	rc6_last = r;
	return n;
}

LOCAL void devices_tell( CONST char *when )
{
	static char	t[1024];
	T_PCIDEV	d;
	INT		i, k, cnt = ts_pcie_count();
	UBINT		g = gpu_base();

	k = tm_sprintf((UB *)t, (CONST UB *)"idle: devices %s", when);
	if ( g != 0 ) {
		/*
		 * Read as they are while the DC states may be on (the first
		 * read brings the display out of DC5 by itself): the PLL of
		 * DPLL1, which DPLL feeds DDI A, the backlight's PWM on the
		 * processor's side and the PCH's, the four requests for the
		 * power wells (the BIOS's, the driver's, KVMR, debug)
		 */
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; display as in DC: LCPLL2_CTL %08x DPLL_CTRL2 %08x BLC_PWM_CPU_CTL2 %08x "
				"BLC_PWM_PCH_CTL1 %08x PWR_WELL_CTL1 %08x CTL2 %08x CTL3 %08x CTL4 %08x",
				in_w(g + 0x46014), in_w(g + 0x6c05c), in_w(g + 0x48250), in_w(g + 0xc8250), in_w(g + 0x45400),
				in_w(g + 0x45404), in_w(g + 0x45408), in_w(g + 0x4540c));
	}
	knl_igfx_dc_pause(TRUE);		/* the display's registers read with its power wells on */
	if ( g != 0 ) {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; display DC_STATE_EN set %08x read %08x (the DC states held off "
				"to read), DMC DC3-DC5 %d DC5-DC6 %d, PSR_CTL %08x PSR_STATUS %08x", knl_igfx_dc_setting(),
				in_w(g + GPU_DC_STATE_EN), in_w(g + GPU_DMC_DC3_DC5), in_w(g + GPU_DMC_DC5_DC6),
				in_w(g + GPU_PSR_CTL), in_w(g + GPU_PSR_STATUS));
		/* the display's clock and power wells: LCPLL, the wells the BIOS and the driver ask for and have, DBUF */
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; LCPLL1_CTL %08x, PWR_WELL_CTL1 %08x CTL2 %08x, DBUF_CTL %08x",
				in_w(g + 0x46010), in_w(g + 0x45400), in_w(g + 0x45404), in_w(g + 0x45008));
	} else {
		k += tm_sprintf((UB *)t + k, (CONST UB *)"; no Intel display at 0:2.0");
	}
	k += knl_nvme_power_text(t + k);
	tm_printf((UB *)"%s\n", t);
	if ( g != 0 ) watermarks_tell(g);
	knl_igfx_dc_pause(FALSE);

	for ( i = 0; i < cnt; i++ ) {
		UINT	pm, ex, l1;
		UINT	bus, dv, fn;

		if ( ts_pcie_get(i, &d) < E_OK ) continue;
		bus = d.bus;
		dv = d.dev;
		fn = d.fn;
		k = tm_sprintf((UB *)t, (CONST UB *)"idle: pci %02x:%02x.%d %04x:%04x class %02x%02x", bus & 0xff, dv, fn,
			       d.vendor, d.device, d.class_code, d.subclass);
		pm = ts_pcie_cap_find(&d, PCI_CAP_PM);
		if ( pm != 0 ) {
			k += tm_sprintf((UB *)t + k, (CONST UB *)" D%d", (INT)( ts_pcie_cfg_read(bus, dv, fn, pm + 4, 4) & 3 ));
		}
		ex = ts_pcie_cap_find(&d, PCI_CAP_EXPRESS);
		if ( ex != 0 ) {
			UW	lcap = ts_pcie_cfg_read(bus, dv, fn, ex + 0x0c, 4);
			UW	lctl = ts_pcie_cfg_read(bus, dv, fn, ex + 0x10, 2);
			UW	dcap2 = ts_pcie_cfg_read(bus, dv, fn, ex + 0x24, 4);
			UW	dctl2 = ts_pcie_cfg_read(bus, dv, fn, ex + 0x28, 2);

			k += tm_sprintf((UB *)t + k, (CONST UB *)" ASPM supported %d on %d, LTR supported %d on %d",
					(INT)( ( lcap >> 10 ) & 3 ), (INT)( lctl & 3 ), (INT)( ( dcap2 >> 11 ) & 1 ),
					(INT)( ( dctl2 >> 10 ) & 1 ));
			l1 = ext_cap_find(bus, dv, fn, PCI_EXT_L1SS);
			if ( l1 != 0 ) {
				UW	c1 = ts_pcie_cfg_read(bus, dv, fn, l1 + 8, 4);
				UINT	tv = ( c1 >> 16 ) & 0x3ff, ts = ( c1 >> 29 ) & 7;

				/* the L1.2 threshold of LTR: value in bits 25:16, scale (1, 32, 1024... ns) in 31:29 */
				k += tm_sprintf((UB *)t + k, (CONST UB *)", L1 substates supported %x on %x (control 1 %08x, "
						"L1.2 LTR threshold %d x %d ns)",
						(UW)( ts_pcie_cfg_read(bus, dv, fn, l1 + 4, 4) & 0xf ), (UW)( c1 & 0xf ), c1,
						(INT)tv, ( ts <= 5 ) ? (INT)( 1U << ( 5 * ts ) ) : 0);
			}
			l1 = ext_cap_find(bus, dv, fn, PCI_EXT_LTR);
			if ( l1 != 0 ) {
				UW	m = ts_pcie_cfg_read(bus, dv, fn, l1 + 4, 4);

				/* the most latency the device may report, snoop (15:0) and no-snoop (31:16) */
				k += tm_sprintf((UB *)t + k, (CONST UB *)", LTR max snoop %d x %d ns no-snoop %d x %d ns",
						(INT)( m & 0x3ff ), (INT)( 1U << ( 5 * ( ( m >> 10 ) & 7 ) ) ),
						(INT)( ( m >> 16 ) & 0x3ff ), (INT)( 1U << ( 5 * ( ( m >> 26 ) & 7 ) ) ));
			}
		}
		tm_printf((UB *)"%s\n", t);
	}
}

/*
 * "idlelog=1": the line every ten seconds on any machine (the record of
 * the temperatures has it on Intel's own), with the platform's power on
 * Intel's; the devices told at the start and once the machine has been
 * idle (each processor asleep for nine tenths of the time) for 60 s
 */
#define IDLE_TELL_PERIODS	6

LOCAL void idlelog_task( INT stacd, void *exinf )
{
	static char	t[2048];
	INT		k, i, quiet = 0;
	BOOL		intel = FALSE, told_idle = FALSE;
	UD		slept_last = 0, at_last = 0;
	UW		a, b, c, d;

	(void)stacd;
	(void)exinf;
	cpuid(0, 0, &a, &b, &c, &d);
	intel = (BOOL)( b == 0x756e6547U && d == 0x49656e69U && c == 0x6c65746eU );	/* "GenuineIntel" */
	cpuid(1, 0, &a, &b, &c, &d);
	if ( c & 0x80000000U ) intel = FALSE;	/* the MSRs and the PCH's registers: not on a virtual machine */
	(void)tk_dly_tsk(10000);
	devices_tell("at the start");
	for ( ;; ) {
		UD	now = ReadSysCounterSync(), slept = 0;

		k = tm_sprintf((UB *)t, (CONST UB *)"idle: at %d s", (INT)( knl_get_mono_ns() / 1000000000ULL ));
		k += knl_cpuidle_wake_text(t + k);
		tm_printf((UB *)"%s\n", t);
		k = tm_sprintf((UB *)t, (CONST UB *)"idle: platform at %d s", (INT)( knl_get_mono_ns() / 1000000000ULL ));
		if ( intel ) {
			k += smi_text(t + k);
			k += pmc_text(t + k);
			k += rc6_text(t + k, intel);
			k += knl_igfx_wm_text(t + k);
		}
		k += timer_fire_text(t + k);
		k += knl_usb_pm_text(t + k);
		tm_printf((UB *)"%s\n", t);

		/* idle for IDLE_TELL_PERIODS periods in a row: the devices told once more */
		for ( i = 0; i < knl_num_prc; i++ ) {
			INT	j;

			for ( j = 0; j < IDLE_ST_N; j++ ) slept += knl_pcpu_of(i)->idle.time_ns[j];
		}
		if ( at_last != 0 && now > at_last && st_n > 0 ) {
			UD	span = knl_cycles_to_ns(now - at_last) * (UD)knl_num_prc;

			quiet = ( ( slept - slept_last ) * 10 >= span * 9 ) ? quiet + 1 : 0;
			if ( quiet >= IDLE_TELL_PERIODS && !told_idle ) {
				devices_tell("idle for 60 s");
				told_idle = TRUE;
			}
		}
		slept_last = slept;
		at_last = now;
		(void)tk_dly_tsk(10000);
	}
}

/* ---------------------------------------------------------------- the start */

LOCAL BOOL states_find( void )
{
	UW	a, b, c, d, model, subs, limit = IDLE_ST_N - 1;
	UB	v[8];
	BOOL	arat, nonstop;
	INT	i, k;
	CONST CSTATE	*tab = NULL;
	INT	tab_n = 0;

	cpuid(0, 0, &a, &b, &c, &d);
	/* "GenuineIntel" */
	if ( a < 6 || b != 0x756e6547U || d != 0x49656e69U || c != 0x6c65746eU ) return FALSE;
	cpuid(1, 0, &a, &b, &c, &d);
	if ( c & 0x80000000U ) {
		tm_printf((UB *)"cpuidle: a virtual machine: HLT\n");
		return FALSE;
	}
	if ( ( c & ( 1U << 3 ) ) == 0 ) {
		tm_printf((UB *)"cpuidle: no MWAIT: HLT\n");
		return FALSE;
	}
	model = ( ( ( a >> 8 ) & 0xf ) == 6 ) ? ( ( ( a >> 4 ) & 0xf ) | ( ( a >> 12 ) & 0xf0 ) ) : 0;
	for ( i = 0; i < (INT)( sizeof(idle_models) / sizeof(idle_models[0]) ); i++ ) {
		if ( idle_models[i].model == model ) {
			tab = idle_models[i].table;
			tab_n = idle_models[i].n;
			c1e_off = idle_models[i].c1e_off;
			pkg_res = idle_models[i].pkg_res;
		}
	}
	if ( tab == NULL ) {
		tm_printf((UB *)"cpuidle: model %02x not in the tables: HLT\n", model);
		return FALSE;
	}
	if ( knl_bootarg(knl_bootargs, "intel_idle.max_cstate", v, sizeof(v)) ) {
		limit = 0;
		for ( k = 0; v[k] >= '0' && v[k] <= '9'; k++ ) limit = limit * 10 + ( v[k] - '0' );
		if ( limit == 0 ) {
			tm_printf((UB *)"cpuidle: intel_idle.max_cstate=0: HLT\n");
			return FALSE;
		}
	}
	cpuid(5, 0, &a, &b, &c, &subs);
	if ( ( c & 3 ) != 3 || subs == 0 ) {
		tm_printf((UB *)"cpuidle: MWAIT without break on interrupt (CPUID 5 ECX %08x EDX %08x): HLT\n", c, subs);
		return FALSE;
	}
	cpuid(6, 0, &a, &b, &c, &d);
	arat = (BOOL)( ( a & 4 ) != 0 );
	cpuid(0x80000000, 0, &a, &b, &c, &d);
	d = 0;
	if ( a >= 0x80000007 ) cpuid(0x80000007, 0, &a, &b, &c, &d);
	nonstop = (BOOL)( ( d & ( 1U << 8 ) ) != 0 );

	st[0].name = "POLL";
	st[0].hint = 0;
	st[0].exit_ns = 0;
	st[0].target_ns = 0;
	st_n = 1;
	for ( i = 0; i < tab_n && (UW)i < limit && st_n < IDLE_ST_N; i++ ) {
		UW	cs = ( ( tab[i].hint >> 4 ) + 1 ) & 0xf;

		if ( ( ( subs >> ( cs * 4 ) ) & 0xf ) == 0 ) continue;
		if ( cs > 1 && !arat ) continue;		/* the timer would stop: no other wakes it */
		if ( cs > 2 && !nonstop ) continue;		/* the monotonic clock would stop */
		st[st_n].name = tab[i].name;
		st[st_n].hint = tab[i].hint;
		st[st_n].exit_ns = (UD)tab[i].exit_us * NS_US;
		st[st_n].target_ns = (UD)tab[i].target_us * NS_US;
		st_n++;
	}
	if ( st_n == 1 ) {
		st_n = 0;
		tm_printf((UB *)"cpuidle: none of the states is usable (CPUID 5 EDX %08x): HLT\n", subs);
		return FALSE;
	}
	/* how long state 0 polls: the first target residency of at least POLL_MIN_NS, at most POLL_MAX_NS */
	for ( i = 1; i < st_n; i++ ) {
		if ( st[i].target_ns < POLL_MIN_NS ) continue;
		poll_limit_ns = ( st[i].target_ns < POLL_MAX_NS ) ? st[i].target_ns : POLL_MAX_NS;
		break;
	}
	{
		char	t[256];
		INT	n = 0;

		for ( i = 0; i < st_n; i++ ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %s", st[i].name);
		tm_printf((UB *)"cpuidle: model %02x, states%s (CPUID 5 EDX %08x)%s%s; the promotion to C1E %s "
			  "(POWER_CTL %08x); package C-state limit (MSR E2h) %08x; the choice by the expected sleep, "
			  "tick %d ms\n", model, t, subs, arat ? "" : ", no ARAT: none deeper than C1",
			  nonstop ? "" : ", the TSC stops: none deeper than C2", c1e_off ? "turned off" : "as it is",
			  (UW)rdmsr(MSR_POWER_CTL), (UW)rdmsr(MSR_PKG_CST_CONFIG), (INT)( TICK_NS / 1000000ULL ));
	}
	return TRUE;
}

/*
 * After the processors are started (devinit.c): the idle states and the
 * clock's settings decided, then applied by each processor in its idle
 * loop
 */
EXPORT void knl_cpu_pm_start( void )
{
	BOOL	idle, hwp;
	UB	v[8];

	tick_cyc = knl_ns_to_cycles_up(TICK_NS);
	if ( knl_bootarg(knl_bootargs, "idlelog", v, sizeof(v)) && v[0] == '1' ) {
		T_CTSK	ct;
		ID	t;

		idlelog = TRUE;
		knl_timer_fire_hook = timer_fire_note;
		knl_memset(&ct, 0, sizeof(ct));
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)idlelog_task;
		ct.itskpri = 20;
		ct.stksz = 16 * 1024;
		t = tk_cre_tsk(&ct);
		if ( t > 0 ) tk_sta_tsk(t, 0);
	}
	idle = states_find();
	hwp = knl_hwp_init();
	if ( !idle && !hwp ) return;
	__atomic_store_n(&idle_gen, 1, __ATOMIC_RELEASE);
}

/* A processor back from a sleep (pm.c): what it lost applied again in its idle loop */
EXPORT void knl_cpu_pm_again( T_PCPU *p )
{
	p->idle.gen = 0;
	p->idle.tick_stopped = FALSE;
}

#endif /* X64 */
