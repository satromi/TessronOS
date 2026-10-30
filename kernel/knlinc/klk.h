/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	klk.h
 *	The kernel's locks (design 4.3.3).
 *
 *	How the kernel's data is kept from two processors at once is chosen
 *	by CNF_KLOCK_MODE (config.h, make KLOCK=<n>):
 *
 *	  0  the big kernel lock alone: every critical section takes it
 *	  1  the big lock, and under it the scheduler's and the timer's
 *	     locks, which the dispatcher and the timer take without the big
 *	     lock
 *	  2  the big lock, and under it every lock of the kernel
 *	  3  no big lock: a critical section masks the interrupts only, and
 *	     takes the locks of what it touches
 *
 *	The code of the T-Kernel's functions is the same in every mode. It
 *	names the lock of what it touches with the macros below, which do
 *	nothing where the mode does not have that lock; in mode 0 a KLOCK
 *	takes no room at all.
 *
 *	The order the locks are taken in, outermost first, is their rank.
 *	A processor holds at most one lock of a rank; the scheduler's lock
 *	alone may be taken again by the processor that holds it.
 *
 *	  1 KLK_R_TCTL	task control: every mutex, the mutexes each task
 *			holds, base priorities, and the changes of another
 *			task's priority and the ends of tasks
 *	  2 KLK_R_POR	every rendezvous port and the waits on them
 *	  3 KLK_R_OBJ	one object: its state and its wait queue (and the
 *			cyclic and alarm handlers, all under one)
 *	  4 KLK_R_SCHED	the ready queue, the state and priority of tasks,
 *			each processor's schedtsk, waits on no object
 *	  5 KLK_R_TMR	the timer queue and the comparator
 *	  6 KLK_R_MISC	free lists of control blocks, counts of the
 *			objects of processes, the system time's offset
 */

#ifndef _KLK_H_
#define _KLK_H_

#include <config.h>

#ifndef CNF_KLOCK_MODE
#define CNF_KLOCK_MODE	0
#endif

#define KLK_FINE_SCHED	( CNF_KLOCK_MODE >= 1 )	/* the scheduler's and the timer's locks */
#define KLK_FINE_OBJ	( CNF_KLOCK_MODE >= 2 )	/* every other lock as well */
#define KLK_GIANT	( CNF_KLOCK_MODE <= 2 )	/* the big lock is the critical section's */
#define KLK_FINE	KLK_FINE_SCHED

/* The order of the locks and its checks (klk.c): in the tests' builds */
#ifndef CNF_KLOCK_CHECK
#if KLK_FINE && defined(USE_KTEST) && USE_KTEST
#define CNF_KLOCK_CHECK	1
#else
#define CNF_KLOCK_CHECK	0
#endif
#endif

#define KLK_R_TCTL	1
#define KLK_R_POR	2
#define KLK_R_OBJ	3
#define KLK_R_SCHED	4
#define KLK_R_TMR	5
#define KLK_R_MISC	6
#define KLK_RANKS	7

/*
 * A lock: a ticket lock, the processor that holds it and how many times
 * it took it. It starts all zero.
 */
typedef struct {
#if KLK_FINE
	volatile UW	next;		/* the next ticket */
	volatile UW	owner;		/* the ticket served */
	volatile UH	holder;		/* PRC holding it, 0: none */
	UH		depth;		/* times the holder took it */
#endif
} KLOCK;

/*
 * What a processor holds (in its T_PCPU)
 */
#define KLK_HELD_N	8

typedef struct {
	UB	n;			/* locks held */
	UB	lost;			/* taken beyond the record */
	struct {
		KLOCK	*l;
		UB	rank;
		UBINT	pc;		/* where it was taken */
	} held[KLK_HELD_N];
#if CNF_KLOCK_STAT
	UD	since[KLK_HELD_N];	/* when each was taken */
#endif
} T_KLKCPU;

#if KLK_FINE

IMPORT KLOCK	knl_sched_klk;		/* rank 4 */
IMPORT KLOCK	knl_tmr_klk;		/* rank 5 */
IMPORT KLOCK	knl_tctl_klk;		/* rank 1 */
IMPORT KLOCK	knl_por_klk;		/* rank 2 */
IMPORT KLOCK	knl_tev_klk;		/* rank 3: the cyclic and the alarm handlers */
IMPORT KLOCK	knl_misc_klk;		/* rank 6 */

/* The scheduler is to assign the processors again when its lock is let go */
IMPORT volatile BOOL	knl_sched_dirty;
IMPORT void knl_sched_ipi_flush( void );	/* and to tell those it gave other tasks (smp.c) */

/*
 * A wake-up owed by a processor that held a lock of the kernel when it
 * was to give it: setting an event flag takes the flag's lock (rank 3)
 * and the scheduler's (rank 4), which may not be taken under a lock of
 * the same or a later rank. It is given by the processor that lets go
 * of the last lock it holds, as it does (knl_klk_unlock): the hook runs
 * with no lock of the kernel held.
 */
IMPORT volatile UW	knl_klk_kick_owed;
IMPORT void		(*knl_klk_kick_hook)( void );

/*
 * Take a lock, interrupts masked. 'open': the interrupts were enabled
 * before the critical section began, so that, holding no lock yet, the
 * processor may take its interrupts while it waits.
 */
IMPORT void knl_klk_lock( KLOCK *l, INT rank, BOOL open );
IMPORT void knl_klk_unlock( KLOCK *l );
IMPORT BOOL knl_klk_mine( KLOCK *l );
IMPORT INT  knl_klk_nheld( void );
Inline void knl_klk_init( KLOCK *l )
{
	l->next = 0;
	l->owner = 0;
	l->holder = 0;
	l->depth = 0;
}

#else /* KLK_FINE */

#define knl_klk_init(l)		((void)(l))

#endif /* KLK_FINE */

/*
 * The macros of the code that takes the locks. Those of a critical
 * section's own (KLK_SCHED, KLK_LOCK, ...) read its _intsts_; the ones
 * ending in _DI are for where the interrupts are masked otherwise.
 */
#if KLK_FINE_SCHED
#define KLK_SCHED()		knl_klk_lock(&knl_sched_klk, KLK_R_SCHED, !isDI(_intsts_))
#define KLK_SCHED_DI()		knl_klk_lock(&knl_sched_klk, KLK_R_SCHED, FALSE)
#define KLK_UNSCHED()		knl_klk_unlock(&knl_sched_klk)
#define KLK_TMR_DI()		knl_klk_lock(&knl_tmr_klk, KLK_R_TMR, FALSE)
#define KLK_UNTMR()		knl_klk_unlock(&knl_tmr_klk)
#define knl_reschedule_lazy()	( knl_sched_dirty = TRUE )
#else
#define KLK_SCHED()
#define KLK_SCHED_DI()
#define KLK_UNSCHED()
#define KLK_TMR_DI()
#define KLK_UNTMR()
#define knl_reschedule_lazy()	knl_reschedule()
#endif

#if KLK_FINE_OBJ
#define KLK_LOCK(l)		knl_klk_lock((l), KLK_R_OBJ, !isDI(_intsts_))
#define KLK_LOCK_DI(l)		knl_klk_lock((l), KLK_R_OBJ, FALSE)
#define KLK_UNLOCK(l)		knl_klk_unlock(l)
#define KLK_TCTL()		knl_klk_lock(&knl_tctl_klk, KLK_R_TCTL, !isDI(_intsts_))
#define KLK_TCTL_DI()		knl_klk_lock(&knl_tctl_klk, KLK_R_TCTL, FALSE)
#define KLK_UNTCTL()		knl_tctl_unlock()
#define KLK_POR()		knl_klk_lock(&knl_por_klk, KLK_R_POR, !isDI(_intsts_))
#define KLK_UNPOR()		knl_klk_unlock(&knl_por_klk)
#define KLK_TEV()		knl_klk_lock(&knl_tev_klk, KLK_R_OBJ, !isDI(_intsts_))
#define KLK_TEV_DI()		knl_klk_lock(&knl_tev_klk, KLK_R_OBJ, FALSE)
#define KLK_UNTEV()		knl_klk_unlock(&knl_tev_klk)
#define KLK_MISC()		knl_klk_lock(&knl_misc_klk, KLK_R_MISC, !isDI(_intsts_))
#define KLK_MISC_DI()		knl_klk_lock(&knl_misc_klk, KLK_R_MISC, FALSE)
#define KLK_UNMISC()		knl_klk_unlock(&knl_misc_klk)
IMPORT void knl_tctl_unlock( void );	/* priorities to be worked out again, then the lock (mutex.c) */
#else
#define KLK_LOCK(l)
#define KLK_LOCK_DI(l)
#define KLK_UNLOCK(l)
#define KLK_TCTL()
#define KLK_TCTL_DI()
#define KLK_UNTCTL()
#define KLK_POR()
#define KLK_UNPOR()
#define KLK_TEV()
#define KLK_TEV_DI()
#define KLK_UNTEV()
#define KLK_MISC()
#define KLK_MISC_DI()
#define KLK_UNMISC()
#endif

/*
 * The checks (CNF_KLOCK_CHECK): that the lock that guards what is about
 * to be touched is held; that a critical section leaves holding no more
 * than it came in with; that the dispatcher is entered holding none.
 */
#if CNF_KLOCK_CHECK
IMPORT volatile UW	knl_klk_bad;		/* faults found */
IMPORT void knl_klk_assert( KLOCK *l, CONST char *what );
IMPORT void knl_klk_assert_none( CONST char *what );
IMPORT void knl_klk_leave_check( INT n );
#define KLK_ASSERT(l, what)	knl_klk_assert((l), (what))
#define KLK_ASSERT_NONE(what)	knl_klk_assert_none(what)
#define KLK_ENTER_DECL		INT _klkn_ = knl_klk_nheld();
#define KLK_LEAVE_CHECK		knl_klk_leave_check(_klkn_);
#else
#define KLK_ASSERT(l, what)
#define KLK_ASSERT_NONE(what)
#define KLK_ENTER_DECL
#define KLK_LEAVE_CHECK
#endif

/*
 * The critical section without the big lock (mode 3): the interrupts
 * masked, the locks taken inside it by what it touches. Whether to
 * dispatch is read from schedtsk with the order of a taking (acquire):
 * the scheduler sets it as its lock is let go, after whatever a task it
 * made ready is given has been written (design 4.3.3).
 */
#if !KLK_GIANT
#define knl_schedtsk_acq()	__atomic_load_n(&knl_pcpu()->schedtsk, __ATOMIC_ACQUIRE)

#define KLK_BEGIN_CS	{ UINT _intsts_ = disint(); KLK_ENTER_DECL
#define KLK_END_CS	{ BOOL _dsp_;					\
			  KLK_LEAVE_CHECK				\
			  _dsp_ = ( !isDI(_intsts_)			\
				  && knl_ctxtsk != knl_schedtsk_acq()	\
				  && !knl_isTaskIndependent()		\
				  && !knl_dispatch_disabled );		\
			  KT_SPIN(knl_kt_unlock_spin)			\
			  if ( _dsp_ ) {				\
				knl_dispatch();				\
			  }						\
			  enaint(_intsts_); } }
#endif

#if KLK_FINE_SCHED
#define KLK_ASSERT_SCHED(what)	KLK_ASSERT(&knl_sched_klk, (what))
#define KLK_ASSERT_TMR(what)	KLK_ASSERT(&knl_tmr_klk, (what))
#else
#define KLK_ASSERT_SCHED(what)
#define KLK_ASSERT_TMR(what)
#endif

/*
 * The tests' hooks (USE_KTEST): turns of a loop made, by rank, right
 * after a lock is taken; in knl_lock_wait_of before the locks are taken
 * again; and between the timer taking an event off its queue and calling
 * it. They make the moments in which processors meet long enough to be
 * met. 0 outside the tests that set them.
 */
#if KLK_FINE && defined(USE_KTEST) && USE_KTEST
IMPORT volatile UW	knl_kt_klk_spin[KLK_RANKS];
IMPORT volatile UW	knl_kt_waitof_spin;
IMPORT volatile UW	knl_kt_fire_spin;
#define KLK_KT_SPIN(n)	{ UW _k_ = (n); while ( _k_ != 0 ) { _k_--; Asm("nop"); } }
#else
#define KLK_KT_SPIN(n)
#endif

#endif /* _KLK_H_ */
