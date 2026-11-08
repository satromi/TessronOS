/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lockup.c (ARMv8-A / AArch64)
 *	Finding the machine stopped, and the watchdog (design 14.10,
 *	include/ts/lockup.h)
 *
 *	Each processor runs its own virtual timer (CNTV, PPI 27), which the
 *	kernel uses for nothing else, at LK_PERIOD_MS. Its interrupt marks
 *	the processor alive, notes where the interrupt found it, and looks
 *	at the others: one that has taken no such interrupt for LK_STOP_MS
 *	has had its interrupts masked that long, or is stalled on an access
 *	that does not complete. What is known of it goes to the console at
 *	once (tm_printf_urgent): its task, where it was last seen, the spin
 *	locks it holds or waits for, the kernel lock. It is then sent an
 *	interrupt of its own (SGI_LOCKUP); a processor that takes it says
 *	where it is itself, and one that does not take it is said not to.
 *
 *	A processor that waits LK_STOP_MS for a spin lock says so as it
 *	waits (spinlock.c calls lk_spin_stall), naming the processor that
 *	holds the lock and where that one took it. The kernel lock held as
 *	long by one taking, a circle of tasks waiting for each other's
 *	mutexes, and the desktop's loop not coming back for LK_SILENT_S are
 *	told as well, the last with where the desktop's task waits.
 *
 *	The hardware watchdog (the board's, kernel/sysdepend/rpi/wdog.c) is
 *	fed from these interrupts. Once a processor stopped, the kernel lock
 *	held too long, a processor stopped by an exception or the desktop's
 *	kicks stopped has stood for the time set (knl_wdog_set), it is no
 *	longer fed, and resets the machine a few seconds later. When every
 *	processor stops, nobody feeds it, and the machine is reset without
 *	a word.
 *
 *	Nothing here takes a lock: what is read of the other processors and
 *	of the tasks may be changing as it is read, and is said as it was
 *	found. The checking itself is done by one processor at a time.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "../../../../tkernel/task.h"
#include "../../../../tkernel/mutex.h"
#include <tm/tmonitor.h>
#include <ts/lockup.h>
#include <ts/proc.h>

#define LK_PERIOD_MS	500		/* the timer of each processor */
#define LK_STOP_MS	5000		/* no interrupt, a spin lock, the kernel lock: this long is stopped */
#define LK_SCAN_CHUNK	2048		/* tasks looked at for circles of mutex waits, a check at a time */
#define LK_HW_MS	10000		/* the hardware watchdog, from its last feed to the reset */
#define LK_SGI_MS	500		/* a processor sent SGI_LOCKUP has this long to answer */
#define LK_SPIN_AGAIN	6		/* a waiter for a spin lock says it again every this many periods */

#define NPRC		CNF_MAX_PRCID
#define LKB(i)		( lk[i].beats )

/* The board's watchdog: none unless the board has one (kernel/sysdepend/rpi/wdog.c) */
WEAK_FUNC EXPORT BOOL knl_wdog_hw_feed( UW ms )
{
	return FALSE;
}

WEAK_FUNC EXPORT void knl_wdog_hw_stop( void )
{
}

IMPORT T_SPLOCK		knl_klock;
IMPORT volatile ID	knl_klock_holder;
IMPORT volatile UBINT	knl_klock_pc;
IMPORT volatile UD	knl_klock_since;
IMPORT volatile UD	knl_klock_count;

/* ------------------------------------------------------------------------ */
/*
 * What is kept of each processor
 */
typedef struct {
	volatile UD	beat;		/* the counter at its last timer interrupt (0: not started) */
	volatile UD	beats;
	/* where the last interrupt found it */
	volatile UD	s_pc, s_lr, s_sp, s_spsr, s_at;
	volatile ID	s_tsk;
	/* the spin lock it waits for too long (lk_spin_stall) */
	T_SPLOCK * volatile w_lock;
	volatile UBINT	w_pc;
	volatile UD	w_since;
	UW		w_told;
	BOOL		reported;	/* told it stopped: until it beats again */
	UD		rep_at;
	volatile BOOL	dump_req;	/* SGI_LOCKUP: say where you are */
	volatile BOOL	dump_done;
	volatile BOOL	halted;		/* stopped by an exception in the kernel */
	volatile BOOL	in_hook;
	volatile BOOL	mute;		/* the tests: looks stopped */
} LKCPU;

LOCAL LKCPU	lk[NPRC];
LOCAL T_LKSTAT	lk_stat;

LOCAL UD	lk_frq = 0;		/* counts a second */
LOCAL UD	lk_period;		/* counts of the timer period */
LOCAL UD	lk_stop;		/* counts of LK_STOP_MS (or the tests') */
LOCAL UD	lk_kick_lim, lk_silent_lim;
LOCAL volatile BOOL	lk_armed = FALSE;
LOCAL volatile UW	lk_busy = 0;	/* the processor checking (PRC ID), 0 none */

/* the kernel lock */
LOCAL UD	lk_kl_count = 0;	/* the taking last seen */
LOCAL BOOL	lk_kl_told = FALSE;

/* the watchdog */
LOCAL volatile INT	lk_reset_s = LK_RESET_S_DEFAULT;
LOCAL BOOL	lk_hw_on = FALSE;	/* the hardware watchdog running */
LOCAL BOOL	lk_hw_told = FALSE;
LOCAL UD	lk_fail_at = 0;		/* something has stood since (0: nothing) */
LOCAL BOOL	lk_starve_told = FALSE;

/* the kicks and the watched loop */
LOCAL volatile UD	lk_kick_at = 0;	/* 0: nobody kicks */
LOCAL volatile ID	lk_kick_tsk = 0;
LOCAL volatile ID	lk_watch_tsk = 0;
LOCAL volatile UD	lk_alive_at = 0;
LOCAL volatile BOOL	lk_watch_idle = FALSE;	/* the watched task waits for input */
LOCAL BOOL	lk_silent_told = FALSE;

/* the circles of waits already told */
#define LK_CIRCLES	8
LOCAL ID	lk_circle[LK_CIRCLES];	/* found in the last round over the tasks */
LOCAL ID	lk_found[LK_CIRCLES];	/* found so far in this one */
LOCAL INT	lk_nfound = 0;
LOCAL INT	lk_scan_pos = 0;

Inline UD lk_now( void )
{
	UD	t;

	Asm("isb; mrs %0, cntpct_el0" : "=r"(t) :: "memory");
	return t;
}

Inline UW lk_ms( UD cycles )
{
	return ( lk_frq != 0 ) ? (UW)( cycles * 1000 / lk_frq ) : 0;
}

/*
 * How long ago t was. Another processor may write t after now was
 * read, putting it a little after now: that is no time ago, not the
 * whole range of the counter.
 */
Inline UW lk_ago( UD now, UD t )
{
	return ( now > t ) ? lk_ms(now - t) : 0;
}

Inline UD lk_cyc( UW ms )
{
	return lk_frq * ms / 1000;
}

Inline INT lk_me( void )
{
	return knl_pcpu()->prcid - 1;
}

LOCAL TCB *lk_tcb_ok( TCB *t )
{
	if ( t == NULL || t < &knl_tcb_table[0] || t >= &knl_tcb_table[NUM_TSKID]
	  || ( (UBINT)t - (UBINT)&knl_tcb_table[0] ) % sizeof(TCB) != 0 ) {
		return NULL;
	}
	return t;
}

/* ------------------------------------------------------------------------ */
/*
 * What is said
 */

/* A task: its number, where it starts, and its process */
LOCAL void lk_say_task( CONST char *pre, TCB *t )
{
	UB	file[TS_FLT_FILE + 1];

	t = lk_tcb_ok(t);
	if ( t == NULL ) {
		tm_printf_urgent((UB *)"%sno task (idle)\n", pre);
		return;
	}
	file[0] = 0;
	if ( t->owner > 0 ) {
		(void)knl_prc_file_peek(t->owner, file, sizeof(file));
	}
	tm_printf_urgent((UB *)"%stask %d entry %lx pri %d state %d process %d %s\n", pre,
			 t->tskid, (UD)(UBINT)t->task, t->priority, t->state, t->owner,
			 ( file[0] != 0 ) ? (char *)file : ( t->owner > 0 ? "?" : "(kernel)" ));
}

/*
 * The frames from fp on, each the place it returns to. A frame is
 * followed only while it is in [lo, hi), aligned, and further up than
 * the last one.
 */
LOCAL void lk_say_frames( CONST char *pre, UBINT fp, UBINT lo, UBINT hi )
{
	INT	i;
	UBINT	next;

	for ( i = 0; i < 10; i++ ) {
		if ( fp == 0 || fp < lo || fp + 16 > hi || ( fp & 0xf ) != 0 ) {
			break;
		}
		if ( ((UD *)fp)[1] == 0 ) {
			break;			/* the first frame of the task or the handler */
		}
		tm_printf_urgent((UB *)"%s  called from %lx\n", pre, ((UD *)fp)[1]);
		next = (UBINT)((UD *)fp)[0];
		if ( next <= fp ) {
			break;
		}
		fp = next;
	}
}

/* A frame is followed within 64KB steps of the last */
LOCAL void lk_say_own_frames( CONST char *pre, UBINT fp )
{
	lk_say_frames(pre, fp, ( fp > 0x10000 ) ? fp - 0x10 : 0, fp + 0x10000);
}

/* Where a task that is not running was stopped: its saved context, and its frames */
LOCAL void lk_say_waiting_task( CONST char *pre, TCB *t )
{
	UD	*ssp, *f;
	UBINT	lo, hi;

	t = lk_tcb_ok(t);
	if ( t == NULL ) {
		return;
	}
	lk_say_task(pre, t);
	if ( ( t->state & TS_WAIT ) != 0 && t->wspec != NULL ) {
		tm_printf_urgent((UB *)"%s  waits: factor %x object %d\n", pre,
				 (UW)t->wspec->tskwait, t->wid);
	}
	if ( t->runprc != 0 ) {
		tm_printf_urgent((UB *)"%s  running on CPU%d\n", pre, t->runprc);
		return;
	}
	hi = (UBINT)t->isstack;
	lo = hi - (UBINT)t->sstksz;
	ssp = (UD *)t->tskctxb.ssp;
	if ( (UBINT)ssp < lo || (UBINT)ssp + 80 + 208 > hi ) {
		return;
	}
	f = ssp + 10;				/* the callee registers, then the exception frame */
	tm_printf_urgent((UB *)"%s  at %lx lr %lx\n", pre, f[21], f[20]);
	lk_say_frames(pre, (UBINT)f[19], lo, hi);
}

/* The mutex a task waits for, and who holds it, as far as it goes */
LOCAL void lk_say_chain( CONST char *pre, TCB *t )
{
	INT	n;
	TCB	*first = t;

	for ( n = 0; n < 8; n++ ) {
		MTXCB	*m;
		TCB	*o;

		t = lk_tcb_ok(t);
		if ( t == NULL || ( t->state & TS_WAIT ) == 0 || t->wspec == NULL
		  || t->wspec->tskwait != TTW_MTX || !CHK_MTXID(t->wid) ) {
			return;
		}
		m = get_mtxcb(t->wid);
		o = lk_tcb_ok(m->mtxtsk);
		tm_printf_urgent((UB *)"%s  task %d waits for mutex %d, held by task %d\n", pre,
				 t->tskid, t->wid, ( o != NULL ) ? o->tskid : 0);
		t = o;
		if ( t == first ) {
			return;		/* round to where it began: a circle */
		}
	}
}

/* What is known of a processor from outside it */
LOCAL void lk_say_cpu( INT i, UD now )
{
	T_PCPU	*p = &knl_pcpu_tbl[i];
	LKCPU	*c = &lk[i];
	char	pre[8];
	INT	k;

	(void)tm_sprintf((UB *)pre, (UB *)" CPU%d ", i);
	if ( p->state != PC_ONLINE ) {
		tm_printf_urgent((UB *)"%soffline (%d)\n", pre, p->state);
		return;
	}
	tm_printf_urgent((UB *)"%slast interrupt %d ms ago; %s, dispatch %s, interrupt nesting %d%s\n", pre,
			 ( c->beat != 0 ) ? (INT)lk_ago(now, c->beat) : -1,
			 c->halted ? "stopped by an exception" : "running",
			 ( p->dispatch_disabled != 0 ) ? "disabled" : "enabled", p->taskindp,
			 c->mute ? " (made to look stopped by the tests)" : "");
	lk_say_task(pre, p->ctxtsk);
	if ( c->s_at != 0 ) {
		tm_printf_urgent((UB *)"%slast seen %d ms ago at pc %lx lr %lx sp %lx spsr %lx, task %d\n", pre,
				 lk_ago(now, c->s_at), c->s_pc, c->s_lr, c->s_sp, c->s_spsr, c->s_tsk);
	}
	if ( knl_klock_holder == i + 1 ) {
		tm_printf_urgent((UB *)"%sholds the kernel lock, taken at %lx %d ms ago\n", pre,
				 (UD)knl_klock_pc, lk_ago(now, knl_klock_since));
	}
#if KLK_FINE
	for ( k = 0; k < p->klk.n && k < KLK_HELD_N; k++ ) {
		tm_printf_urgent((UB *)"%sholds the kernel's lock %lx (rank %d), taken at %lx\n", pre,
				 (UD)(UBINT)p->klk.held[k].l, p->klk.held[k].rank, (UD)p->klk.held[k].pc);
	}
#endif
	for ( k = 0; i < SPIN_HELD_PRC && k < SPIN_HELD_N; k++ ) {
		T_SPHELD	*h = &knl_spin_held[i][k];
		T_SPLOCK	*l = h->lock;

		if ( l != NULL && l != &knl_klock ) {
			tm_printf_urgent((UB *)"%sholds spin lock %lx, taken at %lx %d ms ago\n", pre,
					 (UD)(UBINT)l, h->pc, lk_ago(now, h->since));
		}
	}
	if ( c->w_lock != NULL ) {
		tm_printf_urgent((UB *)"%swaits for spin lock %lx from %lx, for %d ms\n", pre,
				 (UD)(UBINT)c->w_lock, (UD)c->w_pc, lk_ago(now, c->w_since));
	}
}

LOCAL void lk_say_all_cpus( UD now )
{
	INT	i;

	for ( i = 0; i < knl_num_prc && i < NPRC; i++ ) {
		lk_say_cpu(i, now);
	}
}

/* Who holds a spin lock, from the tables of what each processor holds: -1 when not known */
LOCAL INT lk_holder_of( T_SPLOCK *l, T_SPHELD **ph )
{
	INT	i, k;

	if ( l == &knl_klock ) {
		*ph = NULL;
		return ( knl_klock_holder > 0 ) ? knl_klock_holder - 1 : -1;
	}
	for ( i = 0; i < SPIN_HELD_PRC && i < NPRC; i++ ) {
		for ( k = 0; k < SPIN_HELD_N; k++ ) {
			if ( knl_spin_held[i][k].lock == l ) {
				*ph = &knl_spin_held[i][k];
				return i;
			}
		}
	}
	return -1;
}

/*
 * A task named by its caller said as a stopped one is (where it runs or
 * waits, its frames, the mutexes it waits for and who holds them), with
 * the reason first: for a caller that finds a step of its own not done
 * in time, before the watch of the desktop's loop would tell it
 */
EXPORT void knl_lockup_tell_task( ID tskid, CONST char *why )
{
	TCB	*t;
	UINT	s;

	if ( !CHK_TSKID(tskid) ) {
		return;
	}
	t = get_tcb(tskid);
	s = tm_urgent_begin();
	tm_printf_urgent((UB *)"%s\n", ( why != NULL ) ? why : "");
	lk_say_waiting_task(" ", t);
	lk_say_chain(" ", t);
	tm_urgent_end(s);
}

/* The same for the exception reports (exc_hdl.c) */
EXPORT void knl_lockup_say_task( CONST char *pre, TCB *t )
{
	lk_say_task(pre, t);
}

EXPORT void knl_lockup_say_frames( CONST char *pre, UBINT fp )
{
	lk_say_own_frames(pre, fp);
}

/* ------------------------------------------------------------------------ */
/*
 * Circles of tasks waiting for each other's mutexes. Each circle is
 * told once, by the lowest task number in it, for as long as it stands.
 */
LOCAL BOOL lk_circle_of( TCB *t0, ID *p_low )
{
	TCB	*t = t0;
	ID	low = t0->tskid;
	INT	n;

	for ( n = 0; n < 16; n++ ) {
		MTXCB	*m;

		if ( ( t->state & TS_WAIT ) == 0 || t->wspec == NULL || t->wspec->tskwait != TTW_MTX
		  || !CHK_MTXID(t->wid) ) {
			return FALSE;
		}
		m = get_mtxcb(t->wid);
		t = lk_tcb_ok(m->mtxtsk);
		if ( t == NULL ) {
			return FALSE;
		}
		if ( t->tskid < low ) low = t->tskid;
		if ( t == t0 ) {
			*p_low = low;
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL BOOL lk_circle_told( ID low )
{
	INT	k;

	for ( k = 0; k < LK_CIRCLES; k++ ) {
		if ( lk_circle[k] == low ) return TRUE;
	}
	for ( k = 0; k < lk_nfound; k++ ) {
		if ( lk_found[k] == low ) return TRUE;
	}
	return FALSE;
}

/*
 * n of the tasks looked at, from where the last look stopped; a round
 * over all of them is spread over many checks, so that no check holds
 * its processor long with interrupts disabled.
 */
LOCAL void lk_scan_waits( INT n )
{
	ID	low;
	INT	k;

	while ( n-- > 0 ) {
		TCB	*t;

		if ( lk_scan_pos >= NUM_TSKID ) {
			/* a round over: what it found is what has been told */
			for ( k = 0; k < LK_CIRCLES; k++ ) {
				lk_circle[k] = ( k < lk_nfound ) ? lk_found[k] : 0;
			}
			lk_nfound = 0;
			lk_scan_pos = 0;
		}
		t = &knl_tcb_table[lk_scan_pos++];
		if ( ( t->state & TS_WAIT ) == 0 || t->wspec == NULL || t->wspec->tskwait != TTW_MTX ) {
			continue;
		}
		if ( !lk_circle_of(t, &low) || low != t->tskid ) {
			continue;
		}
		if ( !lk_circle_told(low) ) {
			UINT	s = tm_urgent_begin();

			lk_stat.deadlock++;
			tm_printf_urgent((UB *)"\n!LOCKUP! tasks wait for each other's mutexes in a circle:\n");
			lk_say_chain("", t);
			for ( k = 0; k < 8; k++ ) {
				lk_say_waiting_task(" ", t);
				if ( !CHK_MTXID(t->wid) ) break;
				t = lk_tcb_ok(get_mtxcb(t->wid)->mtxtsk);
				if ( t == NULL || t->tskid == low ) break;
			}
			tm_urgent_end(s);
		}
		if ( lk_nfound < LK_CIRCLES ) {
			lk_found[lk_nfound++] = low;
		}
	}
}

/* ------------------------------------------------------------------------ */
/*
 * A processor that has taken no interrupt for lk_stop
 */
LOCAL void lk_report_stopped( INT i, INT me, UD now )
{
	LKCPU	*c = &lk[i];
	UINT	s;
	UD	t0;

	lk_stat.hard++;
	lk_stat.last_prc = i + 1;
	s = tm_urgent_begin();
	tm_printf_urgent((UB *)"\n!LOCKUP! CPU%d has taken no interrupt for %d ms (seen from CPU%d)%s\n",
			 i, lk_ago(now, c->beat), me,
			 c->halted ? ": stopped by an exception in the kernel, told above" : "");
	lk_say_cpu(i, now);
	tm_urgent_end(s);
	if ( c->halted ) {
		return;
	}

	/* its own interrupt: taken only if its interrupts are not masked after all */
	c->dump_done = FALSE;
	c->dump_req = TRUE;
	knl_send_ipi(1U << i, SGI_LOCKUP);
	t0 = lk_now();
	while ( !c->dump_done && lk_now() - t0 < lk_cyc(LK_SGI_MS) ) {
		;
	}
	s = tm_urgent_begin();
	if ( c->dump_done ) {
		lk_stat.sgi_taken++;
	} else {
		c->dump_req = FALSE;
		lk_stat.sgi_lost++;
		tm_printf_urgent((UB *)" CPU%d did not take the interrupt sent to it in %d ms: "
				 "its interrupts are masked, or it is stalled on an access that does not complete\n",
				 i, LK_SGI_MS);
	}
	tm_printf_urgent((UB *)" the other processors:\n");
	for ( INT j = 0; j < knl_num_prc && j < NPRC; j++ ) {
		if ( j != i ) lk_say_cpu(j, now);
	}
	tm_urgent_end(s);
}

/* SGI_LOCKUP: say where this processor is, from the frame the interrupt saved */
LOCAL void lk_sgi_handler( UINT intno, UW iar )
{
	INT	me = lk_me();
	T_PCPU	*p = knl_pcpu();
	LKCPU	*c = &lk[me];

	out_w(GICC_EOIR, iar);
	if ( !c->dump_req ) {
		return;
	}
	c->dump_req = FALSE;
	{
		UINT	s = tm_urgent_begin();
		UD	*f = NULL;

		tm_printf_urgent((UB *)" CPU%d took the interrupt sent to it: its interrupts were not masked\n", me);
		if ( p->taskindp == 1 ) {
			f = *(UD **)( (UB *)p->irq_stack - 16 );
		}
		if ( f != NULL ) {
			tm_printf_urgent((UB *)" CPU%d was at pc %lx lr %lx sp %lx spsr %lx x29 %lx\n", me,
					 f[21], f[20], (UD)(UBINT)( f + 26 ), f[22], f[19]);
			if ( ( f[22] & 0xf ) != 0 ) {
				lk_say_own_frames(" ", (UBINT)f[19]);
			}
		}
		lk_say_task(" ", p->ctxtsk);
		tm_urgent_end(s);
	}
	c->dump_done = TRUE;
}

/*
 * A spin lock waited for too long (spinlock.c), said by the waiter:
 * waited is how long so far, 0 when the lock came at last.
 */
LOCAL void lk_spin_stall( T_SPLOCK *l, UBINT pc, UD waited )
{
	INT	me = lk_me(), h;
	LKCPU	*c;
	T_SPHELD *held = NULL;
	UD	now = lk_now();
	UINT	s;

	if ( me < 0 || me >= NPRC ) {
		return;
	}
	c = &lk[me];
	if ( c->in_hook ) {
		return;				/* the console's lock itself, waited for as this says */
	}
	c->in_hook = TRUE;
	if ( waited == 0 ) {
		if ( c->w_lock != NULL ) {
			tm_printf_urgent((UB *)"!LOCKUP! CPU%d has spin lock %lx at last, after %d ms\n",
					 me, (UD)(UBINT)l, lk_ago(now, c->w_since));
		}
		c->w_lock = NULL;
		c->in_hook = FALSE;
		return;
	}
	if ( c->w_lock != l ) {
		c->w_lock = l;
		c->w_pc = pc;
		c->w_since = now - waited;
		c->w_told = 0;
	} else if ( ++c->w_told % LK_SPIN_AGAIN != 0 ) {
		c->in_hook = FALSE;
		return;
	}
	lk_stat.spin++;
	if ( l == &knl_klock ) {
		lk_stat.klock++;		/* the kernel lock held too long, told by a waiter */
	}
	lk_stat.last_prc = me + 1;
	s = tm_urgent_begin();
	tm_printf_urgent((UB *)"\n!LOCKUP! CPU%d has waited %d ms for %s %lx, from %lx (ticket %d, serving %d)\n",
			 me, lk_ms(waited), ( l == &knl_klock ) ? "the kernel lock" : "spin lock",
			 (UD)(UBINT)l, (UD)pc, l->next, l->owner);
	lk_say_task(" ", knl_pcpu()->ctxtsk);
	lk_say_own_frames(" ", (UBINT)__builtin_frame_address(0));
	h = lk_holder_of(l, &held);
	if ( h < 0 ) {
		tm_printf_urgent((UB *)" the holder is not known\n");
	} else if ( held != NULL ) {
		tm_printf_urgent((UB *)" held by CPU%d, taken at %lx %d ms ago\n", h, held->pc,
				 lk_ago(now, held->since));
	} else {
		tm_printf_urgent((UB *)" held by CPU%d, taken at %lx %d ms ago\n", h,
				 (UD)knl_klock_pc, lk_ago(now, knl_klock_since));
	}
	lk_say_all_cpus(now);
	tm_urgent_end(s);
	c->in_hook = FALSE;
}

/* ------------------------------------------------------------------------ */
/*
 * The checks, by one processor at a time
 */
LOCAL void lk_check( INT me, UD now )
{
	INT	i;
	BOOL	failing = FALSE;
	UW	exp = 0;

	if ( !__atomic_compare_exchange_n(&lk_busy, &exp, (UW)( me + 1 ), FALSE,
					  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ) {
		return;
	}

	/* processors that take no interrupt */
	for ( i = 0; i < knl_num_prc && i < NPRC; i++ ) {
		LKCPU	*c = &lk[i];

		UD	beat = c->beat;

		if ( i == me || knl_pcpu_tbl[i].state != PC_ONLINE || beat == 0 ) {
			continue;
		}
		if ( beat < now && now - beat >= lk_stop ) {
			if ( !c->reported ) {
				c->reported = TRUE;
				c->rep_at = now;
				lk_report_stopped(i, me, now);
			}
			failing = TRUE;
		} else if ( c->reported ) {
			c->reported = FALSE;
			lk_stat.back++;
			tm_printf_urgent((UB *)"!LOCKUP! CPU%d takes interrupts again, after %d ms\n",
					 i, lk_ago(now, c->rep_at) + lk_ms(lk_stop));
		}
		if ( c->halted ) {
			failing = TRUE;
		}
	}
	if ( lk[me].halted ) {
		failing = TRUE;
	}

	/* the kernel lock held by one taking for lk_stop */
	{
		ID	h = knl_klock_holder;
		UD	cnt = knl_klock_count, since = knl_klock_since;

		if ( h != 0 && cnt == lk_kl_count && now - since >= lk_stop && now > since ) {
			if ( !lk_kl_told ) {
				UINT	s = tm_urgent_begin();

				lk_kl_told = TRUE;
				lk_stat.klock++;
				lk_stat.last_prc = h;
				tm_printf_urgent((UB *)"\n!LOCKUP! CPU%d has held the kernel lock for %d ms, taken at %lx\n",
						 h - 1, lk_ago(now, since), (UD)knl_klock_pc);
				lk_say_all_cpus(now);
				tm_urgent_end(s);
			}
			failing = TRUE;
		} else if ( cnt != lk_kl_count || h == 0 ) {
			lk_kl_count = cnt;
			lk_kl_told = FALSE;
		}
	}

	/* the desktop's kicks */
	if ( lk_kick_at != 0 && lk_kick_at < now && now - lk_kick_at >= lk_kick_lim ) {
		if ( !lk_starve_told ) {
			UINT	s = tm_urgent_begin();

			lk_starve_told = TRUE;
			lk_stat.starved++;
			tm_printf_urgent((UB *)"\n!LOCKUP! the watchdog has not been kicked for %d ms "
					 "(it is kicked by task %d, which waits or does not get to run)\n",
					 lk_ago(now, lk_kick_at), lk_kick_tsk);
			if ( CHK_TSKID(lk_kick_tsk) ) {
				lk_say_waiting_task(" ", get_tcb(lk_kick_tsk));
				lk_say_chain(" ", get_tcb(lk_kick_tsk));
			}
			lk_say_all_cpus(now);
			tm_urgent_end(s);
		}
		failing = TRUE;
	} else {
		lk_starve_told = FALSE;
	}

	/*
	 * The desktop's loop: told, the machine is not reset for it. A loop
	 * that waits for input is not away, however long nobody presses
	 * anything (knl_wdog_idle).
	 */
	if ( lk_watch_idle ) {
		lk_alive_at = now;
	}
	if ( lk_watch_tsk > 0 && lk_alive_at != 0 && lk_alive_at < now && now - lk_alive_at >= lk_silent_lim ) {
		if ( !lk_silent_told && CHK_TSKID(lk_watch_tsk) ) {
			UINT	s = tm_urgent_begin();

			lk_silent_told = TRUE;
			lk_stat.silent++;
			tm_printf_urgent((UB *)"\n!LOCKUP! the desktop's loop (task %d) has not come round for %d ms\n",
					 lk_watch_tsk, lk_ago(now, lk_alive_at));
			lk_say_waiting_task(" ", get_tcb(lk_watch_tsk));
			lk_say_chain(" ", get_tcb(lk_watch_tsk));
			lk_say_all_cpus(now);
			tm_printf_urgent((UB *)"!LOCKUP! Shift+Pause ends the program in front; with no program "
					 "there, pressed twice it starts the desktop again (design 5.5.5)\n");
			tm_urgent_end(s);
		}
	} else {
		lk_silent_told = FALSE;
	}

	/* circles of mutex waits */
	lk_scan_waits(LK_SCAN_CHUNK);

	/* the hardware watchdog: fed while nothing stands, or for the time set after */
	if ( !failing ) {
		lk_fail_at = 0;
	} else if ( lk_fail_at == 0 ) {
		lk_fail_at = now;
	}
	lk_stat.failing = ( lk_fail_at != 0 );
	if ( lk_reset_s > 0 ) {
		UD	keep = ( lk_reset_s * 1000 > LK_HW_MS ) ? lk_cyc(lk_reset_s * 1000 - LK_HW_MS) : 0;

		if ( lk_fail_at == 0 || now - lk_fail_at < keep ) {
			if ( knl_wdog_hw_feed(LK_HW_MS) ) {
				lk_hw_on = TRUE;
				lk_stat.feeds++;
			}
			lk_hw_told = FALSE;
		} else if ( !lk_hw_told ) {
			lk_hw_told = TRUE;
			tm_printf_urgent((UB *)"!LOCKUP! %s\n", lk_hw_on
				? "the watchdog is no longer fed: the machine is reset in about 10 seconds"
				: "no hardware watchdog on this machine: it stays as it is");
		}
	} else if ( lk_hw_on ) {
		knl_wdog_hw_stop();
		lk_hw_on = FALSE;
	}

	__atomic_store_n(&lk_busy, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------ */
/*
 * The timer of each processor
 */
LOCAL void lk_arm_timer( void )
{
	Asm("msr cntv_tval_el0, %0" :: "r"(lk_period));
	Asm("msr cntv_ctl_el0, %0" :: "r"((UD)1));	/* enabled, not masked */
	Asm("isb");
}

LOCAL void lk_tick( UINT intno, UW iar )
{
	INT	me = lk_me();
	T_PCPU	*p = knl_pcpu();
	LKCPU	*c = &lk[me];
	UD	now = lk_now();

	lk_arm_timer();				/* the line goes low with the new expiry */
	out_w(GICC_EOIR, iar);
	if ( !c->mute ) {
		c->beat = now;
		c->beats++;
		if ( p->taskindp == 1 ) {
			UD	*f = *(UD **)( (UB *)p->irq_stack - 16 );

			c->s_pc = f[21];
			c->s_lr = f[20];
			c->s_spsr = f[22];
			c->s_sp = ( ( f[22] & 0xf ) == 0 ) ? f[23] : (UD)(UBINT)( f + 26 );
		}
		c->s_tsk = ( p->ctxtsk != NULL ) ? p->ctxtsk->tskid : 0;
		c->s_at = now;
	}
	if ( lk_armed ) {
		lk_check(me, now);
	}
}

/* This processor's timer and its interrupts (each processor, knl_gic_cpu_init) */
EXPORT void knl_lockup_cpu_init( void )
{
	if ( lk_frq == 0 ) {
		UD	f;

		Asm("mrs %0, cntfrq_el0" : "=r"(f));
		lk_frq = ( f != 0 ) ? f : 62500000;
		lk_period = lk_cyc(LK_PERIOD_MS);
		lk_stop = lk_cyc(LK_STOP_MS);
		lk_kick_lim = lk_cyc(LK_KICK_S * 1000);
		lk_silent_lim = lk_cyc(LK_SILENT_S * 1000);
	}
	EnableInt(SGI_LOCKUP, INTPRI_HIGHEST);
	EnableInt(VTIMER_INTNO, INTPRI_SYSTICK);
	lk_arm_timer();
}

/* The handlers (knl_smp_init_interrupt) */
EXPORT void knl_lockup_init_interrupt( void )
{
	knl_define_inthdr(VTIMER_INTNO, TA_HLNG, (FP)lk_tick);
	knl_define_inthdr(SGI_LOCKUP, TA_HLNG, (FP)lk_sgi_handler);
}

/*
 * The checks begin (the initial task, before usermain): every
 * processor has come up and is marked from now on.
 */
EXPORT void knl_lockup_arm( void )
{
	UD	now = lk_now();
	INT	i;

	for ( i = 0; i < NPRC; i++ ) {
		lk[i].beat = ( knl_pcpu_tbl[i].state == PC_ONLINE ) ? now : 0;
		lk[i].reported = FALSE;
	}
	knl_spin_stall_cycles = lk_stop;
	knl_spin_stall_hook = lk_spin_stall;
	lk_armed = TRUE;
	if ( lk_reset_s > 0 && knl_wdog_hw_feed(LK_HW_MS) ) {
		lk_hw_on = TRUE;
		tm_printf((UB *)"TessronOS: watchdog on: the machine is reset %d s after it is found stopped\n",
			  lk_reset_s);
	}
}

EXPORT void knl_lockup_stop( void )
{
	lk_armed = FALSE;
	knl_spin_stall_hook = NULL;
	knl_spin_stall_cycles = 0;
	lk_kick_at = 0;
	lk_watch_tsk = 0;
	if ( lk_hw_on ) {
		knl_wdog_hw_stop();
		lk_hw_on = FALSE;
	}
}

EXPORT void knl_lockup_halted( void )
{
	INT	me = lk_me();

	if ( me >= 0 && me < NPRC ) {
		lk[me].halted = TRUE;
	}
}

/* ------------------------------------------------------------------------ */
/*
 * The desktop's part
 */
EXPORT void knl_wdog_kick( void )
{
	lk_kick_tsk = tk_get_tid();
	lk_kick_at = lk_now();
}

EXPORT void knl_wdog_kick_stop( void )
{
	lk_kick_at = 0;
	lk_watch_tsk = 0;
}

EXPORT void knl_wdog_watch( ID tskid )
{
	lk_alive_at = lk_now();
	lk_watch_idle = FALSE;
	lk_watch_tsk = tskid;
}

EXPORT void knl_wdog_alive( void )
{
	lk_alive_at = lk_now();
}

EXPORT void knl_wdog_idle( BOOL on )
{
	if ( lk_watch_tsk <= 0 || tk_get_tid() != lk_watch_tsk ) {
		return;
	}
	lk_alive_at = lk_now();
	lk_watch_idle = on;
}

EXPORT BOOL knl_wdog_stuck( UW *p_ms, ID *p_tsk )
{
	UD	now = lk_now(), at = lk_alive_at;
	ID	tsk = lk_watch_tsk;
	UW	ms;

	if ( tsk <= 0 || at == 0 || lk_watch_idle || at >= now ) {
		return FALSE;
	}
	ms = lk_ms(now - at);
	if ( now - at < lk_silent_lim ) {
		return FALSE;
	}
	if ( p_ms != NULL ) *p_ms = ms;
	if ( p_tsk != NULL ) *p_tsk = tsk;
	return TRUE;
}

EXPORT ID knl_wdog_watched( void )
{
	return lk_watch_tsk;
}

EXPORT void knl_wdog_set( INT reset_s )
{
	if ( reset_s < 0 ) reset_s = LK_RESET_S_DEFAULT;
	if ( reset_s > 3600 ) reset_s = 3600;
	if ( reset_s != lk_reset_s && reset_s > 0 ) {
		tm_printf((UB *)"TessronOS: watchdog on: reset %d s after the machine is found stopped\n", reset_s);
	} else if ( reset_s != lk_reset_s ) {
		tm_printf((UB *)"TessronOS: watchdog off\n");
	}
	lk_reset_s = reset_s;
}

EXPORT INT knl_wdog_get( BOOL *p_hw )
{
	if ( p_hw != NULL ) {
		*p_hw = lk_hw_on;
	}
	return lk_reset_s;
}

EXPORT void knl_lockup_stat( T_LKSTAT *st )
{
	*st = lk_stat;
	st->hw = lk_hw_on;
}

EXPORT INT knl_lockup_beats( UD *beats, INT n )
{
	INT	i;

	for ( i = 0; i < n && i < knl_num_prc && i < NPRC; i++ ) {
		beats[i] = LKB(i);
	}
	return i;
}

#if USE_KTEST
EXPORT void knl_lockup_tune( UW stop_ms, UW kick_ms, UW silent_ms )
{
	lk_stop = lk_cyc(( stop_ms != 0 ) ? stop_ms : LK_STOP_MS);
	lk_kick_lim = lk_cyc(( kick_ms != 0 ) ? kick_ms : LK_KICK_S * 1000);
	lk_silent_lim = lk_cyc(( silent_ms != 0 ) ? silent_ms : LK_SILENT_S * 1000);
	if ( lk_armed ) {
		knl_spin_stall_cycles = lk_stop;
	}
}

EXPORT void knl_lockup_mute( ID prc, BOOL mute )
{
	if ( prc >= 1 && prc <= NPRC ) {
		lk[prc - 1].mute = mute;
		if ( !mute ) {
			lk[prc - 1].beat = lk_now();
		}
	}
}

EXPORT void knl_lockup_scan( void )
{
	UW	exp = 0;
	UINT	s = disint();

	while ( !__atomic_compare_exchange_n(&lk_busy, &exp, (UW)( lk_me() + 1 ), FALSE,
					     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ) {
		exp = 0;
	}
	lk_scan_waits(NUM_TSKID);
	__atomic_store_n(&lk_busy, 0, __ATOMIC_RELEASE);
	enaint(s);
}
#endif

#endif /* CPU_CORE_ARMV8A */
