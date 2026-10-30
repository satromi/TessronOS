/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lockup.c (x86-64)
 *	Finding the machine stopped, and the watchdog (design 14.10,
 *	include/ts/lockup.h)
 *
 *	Each processor sets its Local APIC timer to go off every
 *	LK_PERIOD_MS as well (apic.c, which runs the system timer of PRC 1 on
 *	the same timer). Its interrupt marks the processor alive, notes
 *	where the interrupt found it, and looks at the others: one that has
 *	taken no such interrupt for LK_STOP_MS has had its interrupts masked
 *	that long, or is stalled on an access that does not complete. What
 *	is known of it goes to the console at once (tm_printf_urgent): its
 *	task, where it was last seen, the spin locks it holds or waits for,
 *	the kernel lock. It is then sent an interrupt of its own
 *	(SGI_LOCKUP); a processor that takes it says where it is itself.
 *	One that does not take it is sent an NMI, which it takes with its
 *	interrupts masked, and says from there where it stands.
 *
 *	A processor that waits LK_STOP_MS for a spin lock says so as it
 *	waits (spinlock.c calls lk_spin_stall), naming the processor that
 *	holds the lock and where that one took it. The kernel lock held as
 *	long by one taking, a circle of tasks waiting for each other's
 *	mutexes, and the desktop's loop not coming back for LK_SILENT_S are
 *	told as well, the last with where the desktop's task waits.
 *
 *	A hardware watchdog (none yet on a PC: the TCO timer of the chipset
 *	is for later) is fed from these interrupts. Once a processor stopped, the kernel lock
 *	held too long, a processor stopped by an exception or the desktop's
 *	kicks stopped has stood for the time set (knl_wdog_set), it is no
 *	longer fed, and resets the machine a few seconds later. When every
 *	processor stops, nobody feeds it, and the machine is reset without
 *	a word.
 *
 *	The checks begin when the start-up is done (knl_lockup_arm). Before
 *	that, the same interrupts keep the start-up's watch on the screen
 *	(knl_fbcon_watch): a step that stands five seconds is named at the
 *	foot of the screen with how long it has stood and how many
 *	processors still take this interrupt.
 *
 *	Nothing here takes a lock: what is read of the other processors and
 *	of the tasks may be changing as it is read, and is said as it was
 *	found. The checking itself is done by one processor at a time.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include "../../../sysdepend.h"
#include "../../../../tkernel/task.h"
#include "../../../../tkernel/mutex.h"
#include <tm/tmonitor.h>
#include <ts/lockup.h>
#include <ts/proc.h>
#include "cpu_task.h"
#include "cpu_ufault.h"

IMPORT void	knl_bootlog_soon( void );	/* bootlog.c */
IMPORT void	knl_fbcon_watch( UD now, UD frq, INT me, INT ticking, INT online, CONST UB *quiet );	/* fbcon.c */

#define LK_PERIOD_MS	500		/* the timer of each processor */
#define LK_STOP_MS	5000		/* no interrupt, a spin lock, the kernel lock: this long is stopped */
#define LK_SCAN_CHUNK	2048		/* tasks looked at for circles of mutex waits, a check at a time */
#define LK_HW_MS	10000		/* the hardware watchdog, from its last feed to the reset */
#define LK_SGI_MS	500		/* a processor sent SGI_LOCKUP has this long to answer */
#define LK_SPIN_AGAIN	6		/* a waiter for a spin lock says it again every this many periods */

#define NPRC		knl_num_prc
#define LKC(i)		( &knl_pcpu_of(i)->lk )
#define LKB(i)		( LKC(i)->beats )

/* The board's watchdog: none unless the board has one */
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
 * What is kept of each processor: in its own data (T_PCPU.lk), so that
 * only the processors found take memory
 */
typedef T_LKCPU LKCPU;

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
LOCAL UD	lk_silent_from = 0;	/* when the loop told of had last come round */

/* the circles of waits already told */
#define LK_CIRCLES	8
LOCAL ID	lk_circle[LK_CIRCLES];	/* found in the last round over the tasks */
LOCAL ID	lk_found[LK_CIRCLES];	/* found so far in this one */
LOCAL INT	lk_nfound = 0;
LOCAL INT	lk_scan_pos = 0;

Inline UD lk_now( void )
{
	return ReadSysCounterSync();
}

/* The counter these checks read, for others that note when something began (usbman.c) */
EXPORT UD knl_lockup_now( void )
{
	return lk_now();
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
		if ( fp < lo || fp + 16 > hi || ( fp & 0xf ) != 0 ) {
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
	if ( (UBINT)ssp < lo || (UBINT)ssp + SSF_EXC_SIZE > hi ) {
		return;
	}
	f = ssp;				/* the frame (T_EXCFRAME) */
	tm_printf_urgent((UB *)"%s  at %lx rbp %lx\n", pre, f[FR_RIP], f[FR_RBP]);
	if ( ( f[FR_CS] & SEL_RPL3 ) == 0 ) {
		lk_say_frames(pre, (UBINT)f[FR_RBP], lo, hi);
	}
}

/*
 * Semaphores used as locks (a count of one), whose holder is noted as
 * they are taken and given back (knl_lockup_sem_held): a task waiting
 * for one is followed to the holder as for a mutex
 */
#define LK_SEMS		16

LOCAL struct {
	volatile ID	sem;		/* 0: a free slot */
	volatile ID	tsk;		/* the holder, 0 none */
	volatile UD	at;		/* since */
} lk_sem[LK_SEMS];

EXPORT void knl_lockup_sem_held( ID semid, BOOL held )
{
	INT	i, free = -1;

	for ( i = 0; i < LK_SEMS; i++ ) {
		if ( lk_sem[i].sem == semid ) break;
		if ( lk_sem[i].sem == 0 && free < 0 ) free = i;
	}
	if ( i == LK_SEMS ) {
		ID	none = 0;

		if ( !held || free < 0 ) return;
		if ( !__atomic_compare_exchange_n(&lk_sem[free].sem, &none, semid, FALSE,
						  __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ) {
			return;			/* taken meanwhile: this one is not noted */
		}
		i = free;
	}
	if ( held ) {
		lk_sem[i].at = lk_now();
		lk_sem[i].tsk = tk_get_tid();
	} else {
		lk_sem[i].tsk = 0;
	}
}

LOCAL INT lk_sem_slot( ID semid )
{
	INT	i;

	for ( i = 0; i < LK_SEMS; i++ ) {
		if ( lk_sem[i].sem == semid ) return i;
	}
	return -1;
}

/*
 * The mutex or noted semaphore a task waits for, and who holds it, as
 * far as it goes; with 'holders', what each holder is doing too: its
 * state, what it waits for, and where it stopped
 */
LOCAL void lk_say_chain( CONST char *pre, TCB *t, BOOL holders )
{
	INT	n;
	TCB	*first = t;

	for ( n = 0; n < 8; n++ ) {
		MTXCB	*m;
		TCB	*o;

		t = lk_tcb_ok(t);
		if ( t == NULL || ( t->state & TS_WAIT ) == 0 || t->wspec == NULL ) {
			return;
		}
		if ( t->wspec->tskwait == TTW_SEM ) {
			INT	k = lk_sem_slot(t->wid);
			ID	h = ( k >= 0 ) ? lk_sem[k].tsk : 0;

			if ( k < 0 ) {
				tm_printf_urgent((UB *)"%s  task %d waits for semaphore %d, not one whose holder is noted\n",
						 pre, t->tskid, t->wid);
				return;
			}
			if ( h <= 0 || !CHK_TSKID(h) ) {
				tm_printf_urgent((UB *)"%s  task %d waits for semaphore %d, noted as free\n", pre,
						 t->tskid, t->wid);
				return;
			}
			o = get_tcb(h);
			tm_printf_urgent((UB *)"%s  task %d waits for semaphore %d, held by task %d for %d ms\n", pre,
					 t->tskid, t->wid, h, lk_ago(lk_now(), lk_sem[k].at));
		} else if ( t->wspec->tskwait == TTW_MTX && CHK_MTXID(t->wid) ) {
			m = get_mtxcb(t->wid);
			o = lk_tcb_ok(m->mtxtsk);
			tm_printf_urgent((UB *)"%s  task %d waits for mutex %d, held by task %d\n", pre,
					 t->tskid, t->wid, ( o != NULL ) ? o->tskid : 0);
		} else {
			return;
		}
		if ( holders && o != NULL && o != first ) {
			tm_printf_urgent((UB *)"%s  the holder, task %d (state %d: 1 ready, 2 waiting, 4 suspended):\n",
					 pre, o->tskid, o->state);
			lk_say_waiting_task(pre, o);
		}
		t = o;
		if ( t == first ) {
			return;		/* round to where it began: a circle */
		}
	}
}

/* The USB manager's lock (device/usb/usbman.c): who holds it, and since when */
IMPORT volatile ID	knl_usb_lock_tsk __attribute__((weak));
IMPORT volatile UD	knl_usb_lock_at __attribute__((weak));

LOCAL void lk_say_usb_lock( CONST char *pre, UD now )
{
	ID	t;

	if ( &knl_usb_lock_tsk == NULL || &knl_usb_lock_at == NULL ) {
		return;
	}
	t = knl_usb_lock_tsk;
	if ( t > 0 ) {
		tm_printf_urgent((UB *)"%sthe USB manager's lock is held by task %d, for %d ms\n", pre, t,
				 lk_ago(now, knl_usb_lock_at));
		if ( CHK_TSKID(t) ) {
			lk_say_waiting_task(pre, get_tcb(t));
		}
	} else {
		tm_printf_urgent((UB *)"%sthe USB manager's lock is free\n", pre);
	}
}

/* What is known of a processor from outside it */
LOCAL void lk_say_cpu( INT i, UD now )
{
	T_PCPU	*p = knl_pcpu_of(i);
	LKCPU	*c = LKC(i);
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
		tm_printf_urgent((UB *)"%slast seen %d ms ago at pc %lx rbp %lx sp %lx rflags %lx, task %d\n", pre,
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
	for ( k = 0; i < knl_spin_held_n && knl_spin_held_p[i] != NULL && k < SPIN_HELD_N; k++ ) {
		T_SPHELD	*h = &knl_spin_held_p[i][k];
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
	for ( i = 0; i < knl_spin_held_n && i < NPRC; i++ ) {
		if ( knl_spin_held_p[i] == NULL ) continue;
		for ( k = 0; k < SPIN_HELD_N; k++ ) {
			if ( knl_spin_held_p[i][k].lock == l ) {
				*ph = &knl_spin_held_p[i][k];
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
	lk_say_chain(" ", t, TRUE);
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
			lk_say_chain("", t, FALSE);
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
	LKCPU	*c = LKC(i);
	UINT	s;
	UD	t0;

	lk_stat.hard++;
	lk_stat.last_prc = i + 1;
	s = tm_urgent_begin();
	tm_printf_urgent((UB *)"\n!LOCKUP! CPU%d has taken no interrupt for %d ms (seen from CPU%d)%s\n",
			 i, lk_ago(now, c->beat), me,
			 c->halted ? ": stopped by an exception in the kernel, kept under the band at the top" : "");
	lk_say_cpu(i, now);
	tm_urgent_end(s);
	knl_bootlog_stopped("stopped: a processor takes no interrupt");
	if ( c->halted ) {
		return;
	}

	/* its own interrupt: taken only if its interrupts are not masked after all */
	c->dump_done = FALSE;
	c->dump_req = TRUE;
	knl_send_ipi_one(i, SGI_LOCKUP);
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
		tm_urgent_end(s);
		/* an NMI is taken all the same */
		c->nmi_done = FALSE;
		c->nmi_req = TRUE;
		knl_apic_send(knl_pcpu_of(i)->apic_id, 0, APIC_DM_NMI);
		t0 = lk_now();
		while ( !c->nmi_done && lk_now() - t0 < lk_cyc(LK_SGI_MS) ) {
			;
		}
		s = tm_urgent_begin();
		if ( !c->nmi_done ) {
			c->nmi_req = FALSE;
			tm_printf_urgent((UB *)" CPU%d did not take an NMI either: it is stalled\n", i);
		}
	}
	tm_printf_urgent((UB *)" the other processors:\n");
	for ( INT j = 0; j < knl_num_prc && j < NPRC; j++ ) {
		if ( j != i ) lk_say_cpu(j, now);
	}
	tm_urgent_end(s);
}

/* Where a frame says a processor was */
LOCAL void lk_say_frame( INT me, CONST UD *f )
{
	tm_printf_urgent((UB *)" CPU%d was at pc %lx sp %lx rflags %lx rbp %lx%s\n", me,
			 f[FR_RIP], f[FR_SP], f[FR_RFLAGS], f[FR_RBP],
			 ( f[FR_CS] & SEL_RPL3 ) ? " (user mode)" : "");
	if ( ( f[FR_CS] & SEL_RPL3 ) == 0 ) {
		lk_say_own_frames(" ", (UBINT)f[FR_RBP]);
	}
}

/* SGI_LOCKUP: say where this processor is, from the frame the interrupt saved */
LOCAL void lk_sgi_handler( UINT intno, UW iar )
{
	INT	me = lk_me();
	T_PCPU	*p = knl_pcpu();
	LKCPU	*c = LKC(me);

	knl_apic_eoi();
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
			lk_say_frame(me, f);
		}
		lk_say_task(" ", p->ctxtsk);
		tm_urgent_end(s);
	}
	c->dump_done = TRUE;
}

/* NMI (exc_hdl.c): the same, asked of a processor that did not take SGI_LOCKUP */
EXPORT BOOL knl_lockup_nmi( T_EXCFRAME *f )
{
	INT	me = lk_me();
	LKCPU	*c;

	if ( me < 0 || me >= NPRC || !LKC(me)->nmi_req ) {
		return FALSE;
	}
	c = LKC(me);
	c->nmi_req = FALSE;
	{
		UINT	s = tm_urgent_begin();

		tm_printf_urgent((UB *)" CPU%d took the NMI sent to it: its interrupts were masked\n", me);
		lk_say_frame(me, (CONST UD *)f);
		lk_say_task(" ", knl_pcpu()->ctxtsk);
		tm_urgent_end(s);
	}
	c->nmi_done = TRUE;
	return TRUE;
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
	c = LKC(me);
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
		LKCPU	*c = LKC(i);

		UD	beat = c->beat;

		if ( i == me || knl_pcpu_of(i)->state != PC_ONLINE || beat == 0 ) {
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
	if ( LKC(me)->halted ) {
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
				knl_bootlog_stopped("stopped: the kernel lock is not let go");
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
				lk_say_chain(" ", get_tcb(lk_kick_tsk), TRUE);
			}
			lk_say_usb_lock(" ", now);
			lk_say_all_cpus(now);
			tm_urgent_end(s);
			knl_bootlog_stopped("stopped: the watchdog is not kicked");
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
			lk_say_chain(" ", get_tcb(lk_watch_tsk), TRUE);
			lk_say_usb_lock(" ", now);
			lk_say_all_cpus(now);
			tm_printf_urgent((UB *)"!LOCKUP! Shift+Pause ends the program in front; with no program "
					 "there, pressed twice it starts the desktop again (design 5.5.5)\n");
			tm_urgent_end(s);
			knl_bootlog_soon();	/* it may come round again: the records written in 2 s */
			lk_silent_from = lk_alive_at;
		}
	} else {
		if ( lk_silent_told && lk_alive_at > lk_silent_from ) {
			tm_printf_urgent((UB *)"!LOCKUP! the desktop's loop came round again, %d ms after it last had\n",
					 lk_ms(lk_alive_at - lk_silent_from));
		}
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
IMPORT void	knl_apic_lk_arm( UD period );	/* apic.c */
IMPORT void	(*knl_apic_lk_hook)( void );

LOCAL void lk_arm_timer( void )
{
	knl_apic_lk_arm(lk_period);
}

/*
 * The start-up's watch on the screen (fbcon.c), checked before the
 * checks above are armed as well: the processors online, those of them
 * whose timer's interrupt came within the last second, and the first
 * that did not: whether an exception stopped it or how long it has
 * taken no timer's interrupt, and where it was last seen (the
 * instruction and the task, from its last timer's interrupt or from the
 * interrupt it stopped in), with how many more there are like it
 */
LOCAL void lk_startup_watch( INT me, UD now )
{
	INT	i, online = 0, ticking = 0, nquiet = 0, q = -1;
	UB	quiet[96];

	for ( i = 0; i < knl_num_prc && i < NPRC; i++ ) {
		UD	beat = LKC(i)->beat;

		if ( knl_pcpu_of(i)->state != PC_ONLINE ) {
			continue;
		}
		online++;
		if ( beat != 0 && beat <= now && now - beat < lk_frq ) {
			ticking++;
		} else {
			if ( q < 0 ) q = i;
			nquiet++;
		}
	}
	quiet[0] = '\0';
	if ( q >= 0 ) {
		LKCPU	*c = LKC(q);
		UD	beat = c->beat;
		INT	n;

		if ( c->halted ) {
			n = tm_sprintf(quiet, (CONST UB *)"CPU%d stopped by an exception", q);
			if ( c->s_at != 0 ) {
				n += tm_sprintf(quiet + n, (CONST UB *)", last at %lx task %d", c->s_pc, c->s_tsk);
			}
		} else if ( beat == 0 ) {
			n = tm_sprintf(quiet, (CONST UB *)"CPU%d has not ticked", q);
		} else if ( beat > now ) {
			/* its counter ahead of this processor's: the two are not in step */
			n = tm_sprintf(quiet, (CONST UB *)"CPU%d's counter %ld ms ahead of CPU%d's", q,
				       (D)lk_ms(beat - now), me);
		} else {
			n = tm_sprintf(quiet, (CONST UB *)"CPU%d silent %ld s, last at %lx task %d", q,
				       (D)( ( now - beat ) / lk_frq ), c->s_pc, c->s_tsk);
		}
		if ( nquiet > 1 ) {
			(void)tm_sprintf(quiet + n, (CONST UB *)" (+%d)", nquiet - 1);
		}
	}
	knl_fbcon_watch(now, lk_frq, me, ticking, online, quiet);
}

/* From the Local APIC timer's interrupt (apic.c), which ends the interrupt itself */
LOCAL void lk_tick( void )
{
	INT	me = lk_me();
	T_PCPU	*p = knl_pcpu();
	LKCPU	*c = LKC(me);
	UD	now = lk_now();

	lk_arm_timer();
	if ( !c->mute ) {
		c->beat = now;
		c->beats++;
		if ( p->taskindp == 1 ) {
			UD	*f = *(UD **)( (UB *)p->irq_stack - 16 );

			c->s_pc = f[FR_RIP];
			c->s_lr = f[FR_RBP];
			c->s_spsr = f[FR_RFLAGS];
			c->s_sp = f[FR_SP];
		}
		c->s_tsk = ( p->ctxtsk != NULL ) ? p->ctxtsk->tskid : 0;
		c->s_at = now;
	}
	lk_startup_watch(me, now);
	if ( lk_armed ) {
		lk_check(me, now);
	}
}

/* This processor's part of the timer (each processor, after its Local APIC is set up) */
EXPORT void knl_lockup_cpu_init( void )
{
	if ( lk_frq == 0 ) {
		UD	f = knl_cntfrq;

		lk_frq = ( f != 0 ) ? f : 1000000000;
		lk_period = lk_cyc(LK_PERIOD_MS);
		lk_stop = lk_cyc(LK_STOP_MS);
		lk_kick_lim = lk_cyc(LK_KICK_S * 1000);
		lk_silent_lim = lk_cyc(LK_SILENT_S * 1000);
	}
	knl_apic_lk_hook = lk_tick;
	lk_arm_timer();
}

/* The handlers (knl_smp_init_interrupt) */
EXPORT void knl_lockup_init_interrupt( void )
{
	knl_define_inthdr(INTVEC_IPI(SGI_LOCKUP), TA_HLNG, (FP)lk_sgi_handler);
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
		LKC(i)->beat = ( knl_pcpu_of(i)->state == PC_ONLINE ) ? now : 0;
		LKC(i)->reported = FALSE;
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

/*
 * This processor stops (exc_hdl.c). Stopped inside an interrupt, where
 * that interrupt found it is kept as the last place it was seen, for the
 * start-up's watch and the lockup reports.
 */
EXPORT void knl_lockup_halted( void )
{
	INT	me = lk_me();
	T_PCPU	*p = knl_pcpu();
	LKCPU	*c;

	if ( me < 0 || me >= NPRC ) {
		return;
	}
	c = LKC(me);
	if ( p->taskindp == 1 ) {
		UD	*f = *(UD **)( (UB *)p->irq_stack - 16 );

		c->s_pc = f[FR_RIP];
		c->s_lr = f[FR_RBP];
		c->s_spsr = f[FR_RFLAGS];
		c->s_sp = f[FR_SP];
		c->s_tsk = ( p->ctxtsk != NULL ) ? p->ctxtsk->tskid : 0;
		c->s_at = lk_now();
	}
	c->halted = TRUE;
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
		LKC(prc - 1)->mute = mute;
		if ( !mute ) {
			LKC(prc - 1)->beat = lk_now();
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

#endif /* CPU_CORE_X86_64 */
