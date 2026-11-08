/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07.
 *
 *----------------------------------------------------------------------
 */

/*
 *	task_manage.c
 *	Task Management Function
 */

#include "kernel.h"
#include "wait.h"
#include "check.h"
#include <tm/tmonitor.h>

#include "../sysdepend/cpu_task.h"

/*
 * Create task
 */
#if CNF_STK_USAGE
#define STK_KEPT	256		/* starting places of tasks deleted that are kept */

typedef struct {
	FP	task;
	W	size, used;
	UW	n;			/* how many of them were deleted */
} STKREC;

LOCAL STKREC	stk_rec[STK_KEPT];

LOCAL void stk_fill( void *stack, W sz )
{
	UD	*p = (UD *)stack, *e = (UD *)( (VB *)stack + sz );

	while ( p < e ) *p++ = STK_FILL;
}

EXPORT W knl_stk_used( TCB *tcb )
{
	UD	*p = TaskStackFoot(tcb) + 1, *e = (UD *)tcb->isstack;

	while ( p < e && *p == STK_FILL ) p++;
	return (W)( (VB *)e - (VB *)p ) + (W)sizeof(UD);
}

/* How deep the calling task's own stack has been used */
EXPORT W knl_stk_used_self( void )
{
	return knl_stk_used(knl_ctxtsk);
}

/* What a task used, kept under where it starts and how large its stack is */
LOCAL void stk_note( TCB *tcb )
{
	W	used = knl_stk_used(tcb);
	INT	i, free = -1;

	for ( i = 0; i < STK_KEPT; i++ ) {
		if ( stk_rec[i].task == tcb->task && stk_rec[i].size == tcb->sstksz ) break;
		if ( stk_rec[i].task == NULL && free < 0 ) free = i;
	}
	if ( i == STK_KEPT ) {
		if ( free < 0 ) return;
		i = free;
		stk_rec[i].task = tcb->task;
		stk_rec[i].size = tcb->sstksz;
		stk_rec[i].used = 0;
		stk_rec[i].n = 0;
	}
	if ( used > stk_rec[i].used ) stk_rec[i].used = used;
	stk_rec[i].n++;
}

LOCAL void stk_say( CONST char *what, INT id, FP task, W size, W used, UW n )
{
	INT	pc = (INT)( (D)used * 100 / ( ( size > 0 ) ? size : 1 ) );

	tm_printf((UB *)"stkuse: %s %d entry %p size %d used %d %d%%%s\n", what, id, (void *)task,
		  (INT)size, (INT)used, pc, ( pc > 75 ) ? " LOW" : "");
	(void)n;
}

/*
 * Every task there is, then every kind of task deleted: the size of the
 * stack, the most of it used, and LOW where less than a quarter is left
 */
EXPORT void knl_stk_usage_say( void )
{
	INT	i, low = 0, n = 0;

	for ( i = 0; i < NUM_TSKID; i++ ) {
		TCB	*tcb = &knl_tcb_table[i];
		W	used;

		if ( tcb->state == TS_NONEXIST || tcb->isstack == NULL ) continue;
		used = knl_stk_used(tcb);
		stk_say("task", tcb->tskid, tcb->task, tcb->sstksz, used, 1);
		if ( (D)used * 4 > (D)tcb->sstksz * 3 ) low++;
		n++;
	}
	for ( i = 0; i < STK_KEPT; i++ ) {
		if ( stk_rec[i].task == NULL ) continue;
		stk_say("gone", (INT)stk_rec[i].n, stk_rec[i].task, stk_rec[i].size, stk_rec[i].used, stk_rec[i].n);
		if ( (D)stk_rec[i].used * 4 > (D)stk_rec[i].size * 3 ) low++;
		n++;
	}
	tm_printf((UB *)"stkuse: %d stacks, %d with less than a quarter left\n", n, low);
}
#endif

SYSCALL ID tk_cre_tsk( CONST T_CTSK *pk_ctsk )
{
#if CHK_RSATR
	const ATR VALID_TSKATR = {	/* Valid value of task attribute */
		 TA_HLNG
		|TA_RNG3
		|TA_USERBUF
		|TA_ASSPRC
		|TA_COPS
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
		|TA_EXTEND
	};
#endif
	TCB	*tcb;
	W	sstksz;
	void	*stack;
	ER	ercd;

	CHECK_RSATR(pk_ctsk->tskatr, VALID_TSKATR);
#if CNF_MAX_PRCID < 64
	CHECK_PAR((pk_ctsk->tskatr & TA_ASSPRC) == 0
		|| (pk_ctsk->assprc != 0 && (pk_ctsk->assprc & ( ~(UBINT)0 << CNF_MAX_PRCID )) == 0));
#else
	CHECK_PAR((pk_ctsk->tskatr & TA_ASSPRC) == 0 || pk_ctsk->assprc != 0);
#endif
#if !USE_IMALLOC
	/* TA_USERBUF must be specified if configured in no Imalloc */
	CHECK_PAR((pk_ctsk->tskatr & TA_USERBUF) != 0);
#endif
	CHECK_PAR(pk_ctsk->stksz >= 0);
	CHECK_PRI(pk_ctsk->itskpri);

	if ( (pk_ctsk->tskatr & TA_USERBUF) != 0 ) {
		/* Use user buffer */
		sstksz = pk_ctsk->stksz;
		CHECK_PAR(sstksz >= MIN_SYS_STACK_SIZE);
		stack = pk_ctsk->bufptr;
	} else {
#if USE_IMALLOC
		/* Allocate system stack area */
		sstksz = pk_ctsk->stksz + DEFAULT_SYS_STKSZ;
#ifdef CNF_SVC_STKSZ
		/*
		 * A task of a process makes its system calls on this stack,
		 * and a call may draw letters: the outline interpreter alone
		 * keeps some twenty kilobytes on it. However little the
		 * process asked for, that much is there.
		 */
		if ( (pk_ctsk->tskatr & TA_RNG3) >= TA_RNG2 && sstksz < CNF_SVC_STKSZ ) {
			sstksz = CNF_SVC_STKSZ;
		}
#endif
		sstksz  = (sstksz  + 7) / 8 * 8;	/* Align to a multiple of 8 */
		sstksz  = knl_sstk_round(sstksz, pk_ctsk->tskatr);
		stack = knl_sstk_alloc(sstksz, pk_ctsk->tskatr);
		if ( stack == NULL ) {
			return E_NOMEM;
		}
#endif
	}

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	KLK_MISC();
	tcb = (TCB*)QueRemoveNext(&knl_free_tcb);
	KLK_UNMISC();
	if ( tcb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* Initialize control block */
	tcb->exinf     = pk_ctsk->exinf;
	tcb->tskatr    = pk_ctsk->tskatr;
	tcb->task      = pk_ctsk->task;
	tcb->ipriority = (UB)int_priority(pk_ctsk->itskpri);
	tcb->sstksz    = sstksz;
	tcb->runprc    = 0;
	prcset_zero(&tcb->assprc);
	if ( (pk_ctsk->tskatr & TA_ASSPRC) != 0 ) {
		tcb->assprc.w[0] = pk_ctsk->assprc;	/* PRC 1..64; the rest with tk_set_asp */
	}
	tcb->owner     = knl_svc_owner();
	tcb->svcown    = 0;
#if USE_OBJECT_NAME
	if ( (pk_ctsk->tskatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)tcb->name, (char*)pk_ctsk->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	/* Set stack pointer */
	tcb->isstack = (VB*)stack + sstksz;

#if CNF_STK_USAGE
	stk_fill(stack, sstksz);
#endif
	/* The value the dispatcher looks for at the foot of the stack */
	*TaskStackFoot(tcb) = TASK_STK_MAGIC;

	/* Set initial value of task operation mode */
	tcb->isysmode = 1;
	tcb->sysmode  = 1;

#ifdef DEFINE_TSK_SYSDEPEND
	ercd = knl_tcb_sysdep_cre(tcb, pk_ctsk);	// TCB system dependent initialization
	if(ercd < E_OK) {
		tcb->state = TS_NONEXIST;
		KLK_MISC();
		QueInsert(&tcb->tskque, &knl_free_tcb);
		KLK_UNMISC();
		goto error_exit;
	}
#endif

	/* make it to DORMANT state */
	KLK_SCHED();
	knl_make_dormant(tcb);
	knl_own_note(TK_OWN_TSK, tcb->owner, 1);
	KLK_UNSCHED();

	ercd = tcb->tskid;

    error_exit:
	END_CRITICAL_SECTION;

#if USE_IMALLOC
	if ( (ercd < E_OK) && ((pk_ctsk->tskatr & TA_USERBUF) == 0) ) {
		knl_sstk_free(stack, sstksz, pk_ctsk->tskatr);
	}
#endif

	return ercd;
}

/*
 * Task deletion
 *	Call from critical section
 *
 *	A task's stack is given back only once its processor has left it.
 *	One that has ended goes on running on its stack for a moment after
 *	letting go of the kernel lock, until it has pushed its frame for the
 *	dispatcher and moved to the dispatcher's own stack; until then the
 *	processor holds its context (runprc). tk_del_tsk waits for that, and
 *	a task that deletes itself (tk_exd_tsk) has its stack and control
 *	block given back by the dispatcher from its own stack
 *	(knl_reap_task). Memory handed out again while the task was still on
 *	it would be written over by the frame it pushes (design 4.3).
 */

/* All of the deletion but giving back the stack and the control block */
LOCAL ER knl_del_tsk_keep( TCB *tcb )
{
	ER ercd = E_OK;

#ifdef DEFINE_TSK_SYSDEPEND
	ercd = knl_tcb_sysdep_del(tcb);	// TCB system dependent finalization
	if ( ercd < E_OK ) {
		return ercd;
	}
#endif

	/*
	 * The stack is about to go back to the allocator, so this is the
	 * last moment a task that overran it can still be named.
	 */
	if ( *TaskStackFoot(tcb) != TASK_STK_MAGIC ) {
		knl_stack_broken(tcb);
	}
#if CNF_STK_USAGE
	stk_note(tcb);
#endif

	knl_own_note(TK_OWN_TSK, tcb->owner, -1);
	tcb->state = TS_NONEXIST;

	return ercd;
}

/* The stack and the control block given back */
LOCAL void knl_del_tsk_give( TCB *tcb )
{
#if USE_IMALLOC
	if ( (tcb->tskatr & TA_USERBUF) == 0 ) {
		/* User buffer is not used */
		/* Free system stack */
		void *stack = (VB*)tcb->isstack - tcb->sstksz;
		knl_sstk_free(stack, tcb->sstksz, tcb->tskatr);
	}
#endif

	/* Return control block to FreeQue */
	KLK_MISC_DI();
	QueInsert(&tcb->tskque, &knl_free_tcb);
	KLK_UNMISC();
}

/* A task that deleted itself, on each processor, until the dispatcher gives it back */
LOCAL TCB	*knl_dead_tsk[CNF_MAX_PRCID];

/*
 * Called by the dispatcher (dispatch.S) on its own stack with interrupts
 * disabled, after the processor has let go of the context of the task
 * that ended
 */
EXPORT void knl_reap_task( void )
{
	INT	i = knl_pcpu()->prcid - 1;
	TCB	*tcb;
	UINT	s;

	if ( i < 0 || i >= CNF_MAX_PRCID || ( tcb = knl_dead_tsk[i] ) == NULL ) {
		return;
	}
	knl_dead_tsk[i] = NULL;
#if KLK_GIANT
	s = knl_lock_kernel();
	knl_del_tsk_give(tcb);
	knl_unlock_kernel(s);
#else
	(void)s;
	knl_del_tsk_give(tcb);
#endif
}

#ifdef USE_FUNC_TK_DEL_TSK
/*
 * Delete task 
 */
SYSCALL ER tk_del_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	for ( ;; ) {
		BOOL	busy = FALSE, give = FALSE;

		BEGIN_CRITICAL_SECTION;
		KLK_SCHED();
		state = (TSTAT)tcb->state;
		if ( state != TS_NONEXIST && knl_not_owner(tcb->owner) ) {
			ercd = E_ID;
		} else if ( state != TS_DORMANT ) {
			ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
		} else if ( tcb->runprc != 0 ) {
			busy = TRUE;	/* ended, its processor not yet off its stack */
		} else {
			ercd = knl_del_tsk_keep(tcb);
			give = ( ercd >= E_OK );
		}
		KLK_UNSCHED();
		if ( give ) {
			knl_del_tsk_give(tcb);
		}
		END_CRITICAL_SECTION;
		if ( !busy ) {
			break;
		}
		CpuRelax();
	}
#if USE_IMALLOC
	knl_sstk_trim();
#endif

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_TSK */

/* ------------------------------------------------------------------------ */

/*
 * Start task
 */
SYSCALL ER tk_sta_tsk( ID tskid, INT stacd )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_SCHED();
	state = (TSTAT)tcb->state;
	if ( state != TS_NONEXIST && knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else if ( state != TS_DORMANT ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
#ifdef knl_task_startable
	} else if ( !knl_task_startable(tcb) ) {
		ercd = E_SYS;		/* its context could not be made as it must be */
#endif
	} else {
		knl_setup_stacd(tcb, stacd);
		knl_make_ready(tcb);
	}
	KLK_UNSCHED();
	END_CRITICAL_SECTION;

	return ercd;
}

/*
 * Task finalization
 *	Call from critical section
 */
LOCAL void knl_ter_tsk( TCB *tcb )
{
	TSTAT	state;

	state = (TSTAT)tcb->state;
	if ( state == TS_READY ) {
		knl_make_non_ready(tcb);

	} else if ( (state & TS_WAIT) != 0 ) {
		knl_wait_cancel(tcb);
		if ( tcb->wspec->rel_wai_hook != NULL ) {
			(*tcb->wspec->rel_wai_hook)(tcb);
		}
	}

#if USE_MUTEX == 1
	/* signal mutex */
	knl_signal_all_mutex(tcb);
#endif

	knl_cleanup_context(tcb);
}

#ifdef USE_FUNC_TK_EXT_TSK
/*
 * End its own task
 */
SYSCALL void tk_ext_tsk( void )
{
#ifdef DORMANT_STACK_SIZE
	/* To avoid destroying stack used in 'knl_make_dormant', 
	   allocate the dummy area on the stack. */
	volatile VB _dummy[DORMANT_STACK_SIZE];
#endif

	/* Check context error */
#if CHK_CTX2
	if ( in_indp() ) {
		SYSTEM_MESSAGE("tk_ext_tsk was called in the task independent\n");
		while(1);
		return;
	}
#endif
#if CHK_CTX1
	if ( in_ddsp() ) {
		SYSTEM_MESSAGE("tk_ext_tsk was called in the dispatch disabled\n");
	}
#endif

	DISABLE_INTERRUPT;
#if KLK_GIANT
	knl_lock_kernel();
#endif
	KLK_TCTL_DI();
	KLK_SCHED_DI();
	knl_ter_tsk(knl_ctxtsk);
	knl_make_dormant(knl_ctxtsk);
	KLK_UNSCHED();
	KLK_UNTCTL();
#if KLK_GIANT
	knl_unlock_kernel_keep();
#endif
	KT_SPIN(knl_kt_exit_spin)

	knl_force_dispatch();
	/* No return */

#ifdef DORMANT_STACK_SIZE
	/* Avoid WARNING (This code does not execute) */
	_dummy[0] = _dummy[_dummy[0]];
#endif
}
#endif /* USE_FUNC_TK_EXT_TSK */

#ifdef USE_FUNC_TK_EXD_TSK
/*
 * End and delete its own task
 */
SYSCALL void tk_exd_tsk( void )
{
	/* Check context error */
#if CHK_CTX2
	if ( in_indp() ) {
		SYSTEM_MESSAGE("tk_exd_tsk was called in the task independent\n");
		return;
	}
#endif
#if CHK_CTX1
	if ( in_ddsp() ) {
		SYSTEM_MESSAGE("tk_exd_tsk was called in the dispatch disabled\n");
	}
#endif

	DISABLE_INTERRUPT;
#if KLK_GIANT
	knl_lock_kernel();
#endif
	KLK_TCTL_DI();
	KLK_SCHED_DI();
	knl_ter_tsk(knl_ctxtsk);
	if ( knl_del_tsk_keep(knl_ctxtsk) >= E_OK ) {
		knl_dead_tsk[knl_pcpu()->prcid - 1] = knl_ctxtsk;	/* given back by the dispatcher */
	}
	KLK_UNSCHED();
	KLK_UNTCTL();
#if KLK_GIANT
	knl_unlock_kernel_keep();
#endif
	KT_SPIN(knl_kt_exit_spin)

	knl_force_dispatch();
	/* No return */
}
#endif /* USE_FUNC_TK_EXD_TSK */

#ifdef USE_FUNC_TK_TER_TSK
/*
 * Termination of other task
 */
SYSCALL ER tk_ter_tsk( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;
	KLOCK	*wl;

	CHECK_TSKID(tskid);
	CHECK_NONSELF(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_TCTL();
	wl = knl_lock_wait_of(tcb);
	state = (TSTAT)tcb->state;
	if ( state != TS_NONEXIST && knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else if ( !knl_task_alive(state) ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else if ( tcb->runprc != 0 ) {
		ercd = E_OBJ;		/* running on another processor (not supported yet) */
	} else if ( tcb->klocked ) {
		/* Normally, it does not become this state.
		 * When the state is page-in wait in the virtual memory
		 * system and when trying to terminate any task,
		 * it becomes this state.
		 */
		ercd = E_OBJ;
	} else {
		knl_ter_tsk(tcb);
		knl_make_dormant(tcb);
	}
	knl_unlock_wait_of(wl);
	KLK_UNTCTL();
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_TER_TSK */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_TK_CHG_PRI
/*
 * Change task priority
 */
SYSCALL ER tk_chg_pri( ID tskid, PRI tskpri )
{
	TCB	*tcb;
	INT	priority;
	ER	ercd;
	KLOCK	*wl;

	CHECK_TSKID_SELF(tskid);
	CHECK_PRI_INI(tskpri);

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_TCTL();
	wl = knl_lock_wait_of(tcb);
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
		goto error_exit;
	}

	/* Conversion priority to internal expression */
	if ( tskpri == TPRI_INI ) {
		priority = tcb->ipriority;
	} else {
		priority = int_priority(tskpri);
	}

#if USE_MUTEX == 1
	/* Mutex priority change limit */
	ercd = knl_chg_pri_mutex(tcb, priority);
	if ( ercd < E_OK ) {
		goto error_exit;
	}

	tcb->bpriority = (UB)priority;
	priority = ercd;
#else
	tcb->bpriority = priority;
#endif

	/* Change priority */
	knl_change_task_priority(tcb, priority);

	ercd = E_OK;
    error_exit:
	knl_unlock_wait_of(wl);
	KLK_UNTCTL();
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_CHG_PRI */

/*
 * Change slice time (T-Kernel 2.0): how long the task runs before it
 * gives way to another of its priority that waits, in ms (design
 * 4.3.1). 0 makes it give way only to a higher priority, as all tasks
 * of the real-time band do unless given a time; in the band above,
 * where tasks of processes and of the desktop take turns, 0 is taken
 * as CNF_SLICE_MS. Only those tasks are made to give way at all.
 */
#define SLICE_MS_MAX	1000

SYSCALL ER tk_chg_slt( ID tskid, RELTIM slicetime )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);
	CHECK_PAR(slicetime <= SLICE_MS_MAX);

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_SCHED();
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else if ( knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else {
		tcb->slicetime = (UW)slicetime;
		tcb->slice_set = TRUE;
		knl_reschedule_lazy();	/* turns that are due now, or no longer */
	}
	KLK_UNSCHED();
	END_CRITICAL_SECTION;

	return ercd;
}

/*
 * The processors a task may run on, any number of them (design 4.3.2):
 * 'size' bytes of words of UBINT, bit n of the array for PRC n+1, as
 * the SMP T-Kernel's assignment. What the array does not reach is 0; an
 * empty set lets the task run on any processor. A set that names a
 * processor beyond CNF_MAX_PRCID, or that names some but no processor
 * online, is E_PAR. The task is placed again at once.
 */
SYSCALL ER tk_set_asp( ID tskid, CONST UBINT *assprc, SZ size )
{
	TCB		*tcb;
	T_PRCSET	s;
	INT		k, n = (INT)( size / (SZ)sizeof(UBINT) ), i;
	ER		ercd = E_OK;

	CHECK_TSKID_SELF(tskid);
	CHECK_PAR(size >= 0 && ( size % (SZ)sizeof(UBINT) ) == 0 && ( n == 0 || assprc != NULL ));

	prcset_zero(&s);
	for ( k = 0; k < n; k++ ) {
		UBINT	w = assprc[k];

		if ( k >= PRCSET_WORDS ) {
			CHECK_PAR(w == 0);
			continue;
		}
		if ( k == PRCSET_WORDS - 1 && CNF_MAX_PRCID % PRCSET_BITS != 0 ) {
			CHECK_PAR(( w >> ( CNF_MAX_PRCID % PRCSET_BITS ) ) == 0);
		}
		s.w[k] = w;
	}
	if ( !prcset_empty(&s) ) {
		i = prcset_next(&s, 0, knl_num_prc);
		CHECK_PAR(i >= 0);		/* none of them online */
	}

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_SCHED();
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else if ( knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else {
		tcb->assprc = s;
		knl_reschedule_lazy();	/* off a processor it may no longer run on */
	}
	KLK_UNSCHED();
	END_CRITICAL_SECTION;

	return ercd;
}

/*
 * The processors a task may run on, into 'size' bytes of words of
 * UBINT: the words written. A task that may run on any has every
 * processor online in it.
 */
SYSCALL INT tk_ref_asp( ID tskid, UBINT *assprc, SZ size )
{
	TCB		*tcb;
	T_PRCSET	s;
	INT		k, n = (INT)( size / (SZ)sizeof(UBINT) );
	ER		ercd = E_OK;

	CHECK_TSKID_SELF(tskid);
	CHECK_PAR(size >= 0 && ( size % (SZ)sizeof(UBINT) ) == 0 && ( n == 0 || assprc != NULL ));

	tcb = get_tcb_self(tskid);

	BEGIN_CRITICAL_SECTION;
	KLK_SCHED();
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		s = tcb->assprc;
	}
	KLK_UNSCHED();
	END_CRITICAL_SECTION;
	if ( ercd < E_OK ) {
		return ercd;
	}
	if ( prcset_empty(&s) ) {
		for ( k = 0; k < knl_num_prc; k++ ) prcset_add(&s, k);
	}
	for ( k = 0; k < n; k++ ) {
		assprc[k] = ( k < PRCSET_WORDS ) ? s.w[k] : 0;
	}
	return n;
}

#ifdef USE_FUNC_TK_ROT_RDQ
/*
 * Rotate ready queue
 */
SYSCALL ER tk_rot_rdq( PRI tskpri )
{
	CHECK_PRI_RUN(tskpri);

	BEGIN_CRITICAL_SECTION;
	if ( tskpri == TPRI_RUN ) {
		if ( in_indp() ) {
			knl_rotate_ready_queue_run();
		} else {
			knl_rotate_ready_queue(knl_ctxtsk->priority);
		}
	} else {
		knl_rotate_ready_queue(int_priority(tskpri));
	}
	END_CRITICAL_SECTION;

	return E_OK;
}
#endif /* USE_FUNC_TK_ROT_RDQ */

/* ------------------------------------------------------------------------ */

#ifdef USE_FUNC_TK_GET_TID
/*
 * Refer task ID at execution
 *	The task is read once, as the processor's own (knl_self_ctxtsk): read
 *	in two steps, a task moved to another processor in between would get
 *	the ID of the task running where it was, or 0.
 */
SYSCALL ID tk_get_tid( void )
{
	TCB	*tcb = knl_self_ctxtsk();

	return ( tcb == NULL )? 0: tcb->tskid;
}
#endif /* USE_FUNC_TK_GET_TID */

#ifdef USE_FUNC_TK_REF_TSK
/*
 * Refer task state
 */
SYSCALL ER tk_ref_tsk( ID tskid, T_RTSK *pk_rtsk )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	knl_memset(pk_rtsk, 0, sizeof(*pk_rtsk));

	BEGIN_CRITICAL_SECTION;
	KLK_SCHED();
	state = (TSTAT)tcb->state;
	if ( state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else if ( knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else {
		if ( ( state == TS_READY ) && ( tcb->runprc != 0 ) ) {
			pk_rtsk->tskstat = TTS_RUN;
		} else {
			pk_rtsk->tskstat = (UINT)state << 1;
		}
		if ( (state & TS_WAIT) != 0 ) {
			pk_rtsk->tskwait = tcb->wspec->tskwait;
			pk_rtsk->wid     = tcb->wid;
		}
		pk_rtsk->exinf     = tcb->exinf;
		pk_rtsk->tskpri    = ext_tskpri(tcb->priority);
		pk_rtsk->tskbpri   = ext_tskpri(tcb->bpriority);
		pk_rtsk->wupcnt    = tcb->wupcnt;
		pk_rtsk->suscnt    = tcb->suscnt;
		pk_rtsk->prcid     = tcb->runprc;
	}
	KLK_UNSCHED();
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REF_TSK */

/* ------------------------------------------------------------------------ */


#ifdef USE_FUNC_TK_REL_WAI
/*
 * Release wait
 */
SYSCALL ER tk_rel_wai( ID tskid )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;
	KLOCK	*wl;

	CHECK_TSKID(tskid);

	tcb = get_tcb(tskid);

	BEGIN_CRITICAL_SECTION;
	wl = knl_lock_wait_of(tcb);
	state = (TSTAT)tcb->state;
	if ( state != TS_NONEXIST && knl_not_owner(tcb->owner) ) {
		ercd = E_ID;
	} else if ( (state & TS_WAIT) == 0 ) {
		ercd = ( state == TS_NONEXIST )? E_NOEXS: E_OBJ;
	} else {
		knl_wait_release_ng(tcb, E_RLWAI);
	}
	knl_unlock_wait_of(wl);
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_REL_WAI */

/* ------------------------------------------------------------------------ */
/*
 *	Debug support function
 */
#if USE_DBGSPT

#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_task_getname(ID id, UB **name)
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(id);

	BEGIN_DISABLE_INTERRUPT;
	tcb = get_tcb_self(id);
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (tcb->tskatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = tcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_TSK
/*
 * Refer task usage state
 */
SYSCALL INT td_lst_tsk( ID list[], INT nent )
{
	TCB	*tcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_tcb_table + NUM_TSKID;
	for ( tcb = knl_tcb_table; tcb < end; tcb++ ) {
		if ( tcb->state == TS_NONEXIST ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = tcb->tskid;
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_TSK */

#ifdef USE_FUNC_TD_REF_TSK
/*
 * Refer task state
 */
SYSCALL ER td_ref_tsk( ID tskid, TD_RTSK *pk_rtsk )
{
	TCB	*tcb;
	TSTAT	state;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	knl_memset(pk_rtsk, 0, sizeof(*pk_rtsk));

	BEGIN_DISABLE_INTERRUPT;
	state = (TSTAT)tcb->state;
	if ( state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		if ( ( state == TS_READY ) && ( tcb->runprc != 0 ) ) {
			pk_rtsk->tskstat = TTS_RUN;
		} else {
			pk_rtsk->tskstat = (UINT)state << 1;
		}
		if ( (state & TS_WAIT) != 0 ) {
			pk_rtsk->tskwait = tcb->wspec->tskwait;
			pk_rtsk->wid     = tcb->wid;
		}
		pk_rtsk->exinf     = tcb->exinf;
		pk_rtsk->tskpri    = ext_tskpri(tcb->priority);
		pk_rtsk->tskbpri   = ext_tskpri(tcb->bpriority);
		pk_rtsk->wupcnt    = tcb->wupcnt;
		pk_rtsk->suscnt    = tcb->suscnt;

		pk_rtsk->task      = tcb->task;
		pk_rtsk->stksz     = tcb->sstksz;
		pk_rtsk->istack    = tcb->isstack;
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_REF_TSK */

#ifdef USE_FUNC_TD_INF_TSK
/*
 * Get task statistic information
 */
SYSCALL ER td_inf_tsk( ID tskid, TD_ITSK *pk_itsk, BOOL clr )
{
	TCB	*tcb;
	ER	ercd = E_OK;

	CHECK_TSKID_SELF(tskid);

	tcb = get_tcb_self(tskid);

	BEGIN_DISABLE_INTERRUPT;
	if ( tcb->state == TS_NONEXIST ) {
		ercd = E_NOEXS;
	} else {
		pk_itsk->stime = tcb->stime;
		pk_itsk->utime = tcb->utime;
		if ( clr ) {
			tcb->stime = 0;
			tcb->utime = 0;
		}
	}
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_FUNC_TD_INF_TSK */

#endif /* USE_DBGSPT */
