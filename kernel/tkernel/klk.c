/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	klk.c
 *	The kernel's locks (klk.h, design 4.3.3): ticket locks taken with
 *	the interrupts masked, the record of what each processor holds,
 *	the checks of their order, what they cost (make KLOCKSTAT=1), and
 *	the report of one waited for too long.
 */

#include "kernel.h"
#include <tm/tmonitor.h>

#if KLK_FINE

#if CNF_KLOCK_STAT
#include <ts/klstat.h>
#endif

EXPORT KLOCK	knl_sched_klk;
EXPORT KLOCK	knl_tmr_klk;
EXPORT KLOCK	knl_tctl_klk;
EXPORT KLOCK	knl_por_klk;
EXPORT KLOCK	knl_tev_klk;
EXPORT KLOCK	knl_misc_klk;

EXPORT volatile BOOL	knl_sched_dirty = FALSE;
EXPORT volatile UW	knl_klk_kick_owed = 0;
EXPORT void		(*knl_klk_kick_hook)( void ) = NULL;

#if USE_KTEST
EXPORT volatile UW	knl_kt_klk_spin[KLK_RANKS];
EXPORT volatile UW	knl_kt_waitof_spin = 0;
EXPORT volatile UW	knl_kt_fire_spin = 0;
#endif

/*
 * A processor that holds no lock and came into its critical section with
 * the interrupts enabled waits with them enabled: it tries the lock,
 * and while it is held watches it with the interrupts let in, this many
 * times. Then it takes a ticket, with the interrupts masked, so that a
 * processor that keeps losing the race is still served in its turn.
 */
#define KLK_OPEN_TRIES	64

LOCAL CONST char *klk_name( KLOCK *l )
{
	if ( l == &knl_sched_klk ) return "scheduler";
	if ( l == &knl_tmr_klk )   return "timer";
	if ( l == &knl_tctl_klk )  return "task control";
	if ( l == &knl_por_klk )   return "rendezvous";
	if ( l == &knl_tev_klk )   return "time event";
	if ( l == &knl_misc_klk )  return "misc";
	return "object";
}

Inline UD klk_now( void )
{
	return ReadSysCounter();
}

/* ------------------------------------------------------------------------ */
/*
 * What it costs, by rank (make KLOCKSTAT=1). The objects' locks are one
 * rank together, so the counts are added atomically.
 */
#if CNF_KLOCK_STAT
LOCAL T_KLSITE	klk_cls[KLK_RANKS];
LOCAL UD	klk_edge[KL_HIST_N - 1];

LOCAL INT klk_bin( UD ticks )
{
	INT	k;

	if ( klk_edge[0] == 0 ) {
		for ( k = 0; k < KL_HIST_N - 1; k++ ) {
			UD	e = (UD)knl_cntfrq * ( (UD)1 << k ) / 1000000U;

			klk_edge[k] = ( e == 0 ) ? 1 : e;
		}
	}
	for ( k = 0; k < KL_HIST_N - 1 && ticks >= klk_edge[k]; k++ ) ;
	return k;
}

LOCAL void klk_max( UD *at, UD v )
{
	UD	cur = __atomic_load_n(at, __ATOMIC_RELAXED);

	while ( v > cur && !__atomic_compare_exchange_n(at, &cur, v, FALSE,
							 __ATOMIC_RELAXED, __ATOMIC_RELAXED) ) ;
}

LOCAL void klk_note_acq( INT rank, BOOL waited, UD wait )
{
	T_KLSITE	*s = &klk_cls[rank];

	__atomic_fetch_add(&s->acq, 1, __ATOMIC_RELAXED);
	if ( waited ) {
		__atomic_fetch_add(&s->cont, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s->wait, wait, __ATOMIC_RELAXED);
		klk_max(&s->wait_max, wait);
	}
}

LOCAL void klk_note_hold( INT rank, UD held )
{
	T_KLSITE	*s = &klk_cls[rank];

	__atomic_fetch_add(&s->hold, held, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s->hold_hist[klk_bin(held)], 1, __ATOMIC_RELAXED);
	klk_max(&s->hold_max, held);
}

/* The counts of each rank into cls[1..n-1] (cls[r].pc is r); answers the ranks there are */
EXPORT INT knl_klk_stat( T_KLSITE *cls, INT n )
{
	INT	r;

	for ( r = 0; r < KLK_RANKS && r < n; r++ ) {
		cls[r] = klk_cls[r];
		cls[r].pc = (UBINT)r;
	}
	return KLK_RANKS;
}

EXPORT CONST char *knl_klk_rank_name( INT r )
{
	static CONST char *const nm[KLK_RANKS] = {
		"-", "tctl", "por", "obj", "sched", "tmr", "misc"
	};
	return ( r >= 0 && r < KLK_RANKS ) ? nm[r] : "?";
}

EXPORT void knl_klk_stat_clear( void )
{
	knl_memset(klk_cls, 0, sizeof(klk_cls));
}
#endif /* CNF_KLOCK_STAT */

/* ------------------------------------------------------------------------ */
/*
 * The checks (CNF_KLOCK_CHECK). A fault is said once for each place,
 * up to a number of places, and counted always.
 */
#if CNF_KLOCK_CHECK
EXPORT volatile UW	knl_klk_bad = 0;
EXPORT volatile BOOL	knl_klk_quiet = FALSE;	/* the tests' own faults, counted unsaid */

#define KLK_SAID_N	32
LOCAL UBINT	klk_said[KLK_SAID_N];

LOCAL void klk_fault( CONST char *what, KLOCK *l, UBINT pc )
{
	T_KLKCPU	*k = &knl_pcpu()->klk;
	INT		i, j;

	__atomic_fetch_add(&knl_klk_bad, 1, __ATOMIC_RELAXED);
	if ( knl_klk_quiet ) {
		return;
	}
	for ( i = 0; i < KLK_SAID_N; i++ ) {
		UBINT	cur = klk_said[i];

		if ( cur == pc ) {
			return;
		}
		if ( cur == 0 ) {
			if ( __atomic_compare_exchange_n(&klk_said[i], &cur, pc, FALSE,
							 __ATOMIC_RELAXED, __ATOMIC_RELAXED) ) {
				break;
			}
			if ( cur == pc ) {
				return;
			}
		}
	}
	if ( i == KLK_SAID_N ) {
		return;
	}
	tm_printf((UB *)"!KLOCK! PRC%d: %s: the %s lock %lx, at %lx; holding:",
		  knl_pcpu()->prcid, what, ( l != NULL ) ? klk_name(l) : "-", (UD)(UBINT)l, (UD)pc);
	for ( j = 0; j < k->n && j < KLK_HELD_N; j++ ) {
		tm_printf((UB *)" %s(%d) from %lx", klk_name(k->held[j].l), k->held[j].rank,
			  (UD)k->held[j].pc);
	}
	tm_printf((UB *)"\n");
}

EXPORT void knl_klk_assert( KLOCK *l, CONST char *what )
{
	if ( !knl_klk_mine(l) ) {
		klk_fault(what, l, (UBINT)__builtin_return_address(0));
	}
}

EXPORT void knl_klk_assert_none( CONST char *what )
{
	if ( knl_pcpu()->klk.n != 0 ) {
		klk_fault(what, NULL, (UBINT)__builtin_return_address(0));
	}
}

EXPORT void knl_klk_leave_check( INT n )
{
	if ( knl_pcpu()->klk.n > n ) {
		klk_fault("a critical section left holding", knl_pcpu()->klk.held[n].l,
			  (UBINT)__builtin_return_address(0));
	}
}
#endif /* CNF_KLOCK_CHECK */

/* ------------------------------------------------------------------------ */
/*
 * A lock waited for too long: said once when the lockup watch's time
 * (lockup.c) has gone by, and again each time as long has gone by
 */
LOCAL void klk_stall( KLOCK *l, UBINT pc, UD waited )
{
	INT	h = l->holder, j;
	T_PCPU	*hp;
	UBINT	at = 0;

	if ( h > 0 && h <= knl_num_prc && ( hp = knl_pcpu_of(h - 1) ) != NULL ) {
		for ( j = 0; j < hp->klk.n && j < KLK_HELD_N; j++ ) {
			if ( hp->klk.held[j].l == l ) {
				at = hp->klk.held[j].pc;
			}
		}
	}
	tm_printf_urgent((UB *)"!LOCKUP! PRC%d has waited %d ms for the kernel's %s lock %lx, from %lx;"
			 " held by PRC%d, taken at %lx (ticket %d, serving %d)\n",
			 knl_pcpu()->prcid, (INT)( waited * 1000 / ( knl_cntfrq ? knl_cntfrq : 1 ) ),
			 klk_name(l), (UD)(UBINT)l, (UD)pc, h, (UD)at, l->next, l->owner);
}

LOCAL BOOL klk_try( KLOCK *l )
{
	UW	n = __atomic_load_n(&l->next, __ATOMIC_RELAXED);

	if ( __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != n ) {
		return FALSE;
	}
	return __atomic_compare_exchange_n(&l->next, &n, n + 1, FALSE,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Watching for the lock, and for the time it has taken */
Inline void klk_watch( KLOCK *l, UBINT pc, UD *t0, UD *last )
{
	UD	lim = knl_spin_stall_cycles, now;

	CpuRelax();
	if ( lim == 0 ) {
		return;
	}
	now = klk_now();
	if ( *t0 == 0 ) {
		*t0 = *last = now;
	} else if ( now - *last >= lim ) {
		*last = now;
		klk_stall(l, pc, now - *t0);
	}
}

LOCAL void klk_wait( KLOCK *l, BOOL open, UBINT pc )
{
	T_PCPU	*p = knl_pcpu();
	UD	t0 = 0, last = 0;
	UW	ticket;
	INT	k, dd;

	if ( open ) {
		for ( k = 0; k < KLK_OPEN_TRIES; k++ ) {
			/*
			 * The interrupts are taken while the lock is held
			 * elsewhere; the task is not switched meanwhile: it is
			 * in its critical section
			 */
			dd = p->dispatch_disabled;
			if ( dd == DDS_ENABLE ) {
				p->dispatch_disabled = DDS_DISABLE_IMPLICIT;
			}
			enaint(0);
			while ( __atomic_load_n(&l->owner, __ATOMIC_RELAXED)
				!= __atomic_load_n(&l->next, __ATOMIC_RELAXED) ) {
				klk_watch(l, pc, &t0, &last);
			}
			(void)disint();
			if ( dd == DDS_ENABLE ) {
				p->dispatch_disabled = DDS_ENABLE;
			}
			if ( klk_try(l) ) {
				return;
			}
		}
	}
	ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
	while ( __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket ) {
		klk_watch(l, pc, &t0, &last);
	}
}

EXPORT INT knl_klk_nheld( void )
{
	return knl_pcpu()->klk.n;
}

EXPORT BOOL knl_klk_mine( KLOCK *l )
{
	return ( l->holder != 0 && l->holder == (UH)knl_pcpu()->prcid );
}

EXPORT void knl_klk_lock( KLOCK *l, INT rank, BOOL open )
{
	T_PCPU		*p = knl_pcpu();
	T_KLKCPU	*k = &p->klk;
	UH		me = (UH)p->prcid;
	UBINT		pc = (UBINT)__builtin_return_address(0);
	BOOL		waited = FALSE;
#if CNF_KLOCK_STAT
	UD		t0 = 0;
#endif
#if CNF_KLOCK_CHECK
	INT		j;

	if ( !knl_int_masked() ) {
		klk_fault("taken with the interrupts enabled", l, pc);
	}
#endif

	if ( l->holder == me ) {
#if CNF_KLOCK_CHECK
		if ( rank != KLK_R_SCHED ) {
			klk_fault("taken again by its holder", l, pc);
		}
#endif
		l->depth++;
		return;
	}
#if CNF_KLOCK_CHECK
	for ( j = 0; j < k->n && j < KLK_HELD_N; j++ ) {
		if ( k->held[j].rank >= rank ) {
			klk_fault("taken out of order", l, pc);
			break;
		}
	}
#endif

	if ( !klk_try(l) ) {
#if CNF_KLOCK_STAT
		t0 = klk_now();
#endif
		waited = TRUE;
		/* under the big lock (modes 1 and 2) the interrupts stay masked: it is held */
		klk_wait(l, ( !KLK_GIANT && open && k->n == 0 && !knl_isTaskIndependent() ), pc);
	}
	l->holder = me;
	l->depth = 1;
	if ( k->n < KLK_HELD_N ) {
		k->held[k->n].l = l;
		k->held[k->n].rank = (UB)rank;
		k->held[k->n].pc = pc;
	} else {
		k->lost++;
	}
	k->n++;
#if CNF_KLOCK_STAT
	klk_note_acq(rank, waited, waited ? klk_now() - t0 : 0);
	if ( k->n <= KLK_HELD_N ) {
		k->since[k->n - 1] = klk_now();
	}
#else
	(void)waited;
#endif
#if USE_KTEST
	KLK_KT_SPIN(knl_kt_klk_spin[rank])
#endif
}

EXPORT void knl_klk_unlock( KLOCK *l )
{
	T_PCPU		*p = knl_pcpu();
	T_KLKCPU	*k = &p->klk;
	INT		j, rank = 0;

#if CNF_KLOCK_CHECK
	if ( l->holder != (UH)p->prcid ) {
		klk_fault("let go by another than its holder", l, (UBINT)__builtin_return_address(0));
		return;
	}
#endif
	if ( --l->depth > 0 ) {
		return;
	}

	/* The scheduler assigns the processors as its lock is let go */
	if ( l == &knl_sched_klk && knl_sched_dirty ) {
		l->depth = 1;
		while ( knl_sched_dirty ) {
			knl_sched_dirty = FALSE;
			knl_reschedule();
		}
		l->depth = 0;
	}

	for ( j = ( k->n <= KLK_HELD_N ) ? k->n - 1 : KLK_HELD_N - 1; j >= 0; j-- ) {
		if ( k->held[j].l == l ) {
			break;
		}
	}
	if ( j >= 0 ) {
		rank = k->held[j].rank;
#if CNF_KLOCK_STAT
		klk_note_hold(rank, klk_now() - k->since[j]);
#endif
		for ( ; j + 1 < k->n && j + 1 < KLK_HELD_N; j++ ) {
			k->held[j] = k->held[j + 1];
#if CNF_KLOCK_STAT
			k->since[j] = k->since[j + 1];
#endif
		}
	} else if ( k->lost > 0 ) {
		k->lost--;
	}
	k->n--;
	(void)rank;

	l->holder = 0;
	__atomic_store_n(&l->owner, l->owner + 1, __ATOMIC_RELEASE);

	/* the processors the scheduler gave other tasks, told now that the lock is free */
	if ( l == &knl_sched_klk ) {
		knl_sched_ipi_flush();
	}

	/* a wake-up owed while a lock was held, given now that none is */
	if ( k->n == 0 && knl_klk_kick_owed != 0 && knl_klk_kick_hook != NULL
	  && __atomic_exchange_n(&knl_klk_kick_owed, 0, __ATOMIC_SEQ_CST) != 0 ) {
		(*knl_klk_kick_hook)();
	}
}

#endif /* KLK_FINE */
