/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	smp.c (ARMv8-A / AArch64)
 *	Multiprocessor support (design chapter 8, first stage):
 *	per-CPU data, the big kernel lock, the global scheduler that
 *	assigns ready tasks to processors, IPIs, secondary core start-up
 *	through PSCI, tk_get_prc.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "../../../../tkernel/task.h"
#include "../../../../tkernel/ready_queue.h"
#include "sysman/space.h"
#include <tm/tmonitor.h>

/*
 * Per-CPU data. TPIDR_EL1 of each processor points at its own element.
 */
EXPORT T_PCPU	knl_pcpu_tbl[CNF_MAX_PRCID];
EXPORT INT	knl_num_prc = 1;		/* processors online (PRC 1..n) */
EXPORT UD	knl_secondary_sp[CNF_MAX_PRCID];	/* initial SP of each secondary (boot.S) */

LOCAL UB	knl_pcpu_tmp_stack[CNF_MAX_PRCID][CNF_TMP_STACK_SIZE] __attribute__((aligned(16)));
LOCAL UB	knl_pcpu_irq_stack[CNF_MAX_PRCID][CNF_EXC_STACK_SIZE] __attribute__((aligned(16)));

IMPORT UD	__tmp_stack_start;		/* linker: boot stack of the primary */
IMPORT UD	knl_secondary_entry_pa;		/* boot.S: physical entry of a secondary core */

/* ------------------------------------------------------------------------ */
/*
 * Big kernel lock
 *	One ticket spin lock for the whole kernel, taken with interrupts
 *	disabled. It is re-entrant on the holding processor so that a
 *	handler called from inside a critical section (cyclic handler,
 *	SVC from an interrupt) can call kernel APIs.
 */
LOCAL T_SPLOCK		knl_klock;
LOCAL volatile ID	knl_klock_holder = 0;	/* PRC ID, 0 = free */
LOCAL UW		knl_klock_depth = 0;

#if CNF_KLOCK_STAT
/*
 * What the lock costs, place by place (design 8.6.2, include/ts/klstat.h).
 * Kept under the lock itself, so nothing here needs to be atomic.
 */
#include <ts/klstat.h>

#define KL_SITES	96

LOCAL T_KLSITE	kl_site[KL_SITES];
LOCAL T_KLSITE	kl_all;
LOCAL T_KLSITE	kl_rest;		/* places that found the table full */
LOCAL INT	kl_nsite = 0;
LOCAL T_KLSITE	*kl_held;		/* where the lock was taken from now */
LOCAL UD	kl_since;		/* and when */

Inline UD kl_now( void )
{
	UD	t;

	Asm("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

LOCAL T_KLSITE *kl_site_of( UBINT pc )
{
	INT	i;

	for ( i = 0; i < kl_nsite; i++ ) {
		if ( kl_site[i].pc == pc ) return &kl_site[i];
	}
	if ( kl_nsite == KL_SITES ) {
		return &kl_rest;
	}
	kl_site[kl_nsite].pc = pc;
	return &kl_site[kl_nsite++];
}

EXPORT void knl_klock_stat_clear( void )
{
	UINT	s = knl_lock_kernel();

	knl_memset(kl_site, 0, sizeof(kl_site));
	knl_memset(&kl_all, 0, sizeof(kl_all));
	knl_memset(&kl_rest, 0, sizeof(kl_rest));
	kl_nsite = 0;
	kl_held = &kl_rest;			/* this taking is not counted */
	knl_unlock_kernel(s);
}

EXPORT INT knl_klock_stat( T_KLSITE *all, T_KLSITE *sites, INT n )
{
	UINT	s = knl_lock_kernel();
	INT	i, k, got, used[KL_SITES];

	if ( all != NULL ) *all = kl_all;
	got = kl_nsite;
	for ( i = 0; i < kl_nsite; i++ ) used[i] = 0;
	for ( k = 0; sites != NULL && k < n && k < kl_nsite; k++ ) {
		INT	best = -1;

		for ( i = 0; i < kl_nsite; i++ ) {
			if ( !used[i] && ( best < 0 || kl_site[i].wait + kl_site[i].hold
						       > kl_site[best].wait + kl_site[best].hold ) ) {
				best = i;
			}
		}
		used[best] = 1;
		sites[k] = kl_site[best];
	}
	knl_unlock_kernel(s);
	return got;
}
#endif

EXPORT UINT knl_lock_kernel( void )
{
	UINT	intsts = disint();
	ID	me = knl_pcpu()->prcid;

	if ( knl_klock_holder == me ) {
		knl_klock_depth++;
		return intsts;
	}
#if CNF_KLOCK_STAT
	{
		UD		t0 = kl_now(), t1;
		BOOL		waited = !SpinTryLock(&knl_klock);
		T_KLSITE	*s;

		if ( waited ) {
			SpinLock(&knl_klock);
		}
		t1 = kl_now();
		s = kl_site_of((UBINT)__builtin_return_address(0));
		s->acq++;
		kl_all.acq++;
		if ( waited ) {
			s->cont++;
			s->wait += t1 - t0;
			kl_all.cont++;
			kl_all.wait += t1 - t0;
		}
		kl_held = s;
		kl_since = t1;
	}
#else
	SpinLock(&knl_klock);
#endif
	knl_klock_holder = me;
	knl_klock_depth = 1;
	return intsts;
}

EXPORT void knl_unlock_kernel_keep( void )
{
	if ( --knl_klock_depth == 0 ) {
#if CNF_KLOCK_STAT
		UD	held = kl_now() - kl_since;

		if ( kl_held != NULL ) kl_held->hold += held;
		kl_all.hold += held;
#endif
		knl_klock_holder = 0;
		SpinUnlock(&knl_klock);
	}
}

/*
 * Dispatcher: take the task this processor should run (dispatch.S)
 *	Done under the kernel lock so that schedtsk is consistent with the
 *	ready queue and the critical section that made the task ready has
 *	finished writing what the task reads on return (wait result, message
 *	pointer ...). Returns NULL when there is nothing to run yet or the
 *	task's context is still held by the processor that ran it last.
 *	Called with interrupts disabled on the temporary stack.
 */
EXPORT TCB *knl_dispatch_pick( void )
{
	T_PCPU	*p = knl_pcpu();
	TCB	*tcb;

	(void)knl_lock_kernel();
	tcb = p->schedtsk;
	if ( tcb != NULL ) {
		if ( __atomic_load_n(&tcb->runprc, __ATOMIC_ACQUIRE) != 0 ) {
			tcb = NULL;
		} else {
			tcb->runprc = (UB)p->prcid;
			p->ctxtsk = tcb;
			p->ctxsw_cnt++;
		}
	}
	knl_unlock_kernel_keep();

	/*
	 * A task that overran its stack wrote over whatever the allocator
	 * handed out below it. Nothing else can be trusted after that, so
	 * say which task it was and stop here rather than let the damage
	 * surface somewhere unrelated.
	 */
	if ( tcb != NULL && *TaskStackFoot(tcb) != TASK_STK_MAGIC ) {
		knl_stack_broken(tcb);
	}

	return tcb;
}

EXPORT void knl_unlock_kernel( UINT intsts )
{
	knl_unlock_kernel_keep();
	enaint(intsts);
}

EXPORT BOOL knl_kernel_locked( void )
{
	return ( knl_klock_holder == knl_pcpu()->prcid );
}

/* ------------------------------------------------------------------------ */
/*
 * IPI
 */
EXPORT void knl_send_ipi( UW prcmask, UINT sgino )
{
	UW	targets = prcmask & ((1U << CNF_MAX_PRCID) - 1);

	if ( targets == 0 ) return;
	Asm("dsb ishst");			/* data written before the IPI is visible first */
	out_w(GICD_SGIR, (targets << 16) | (sgino & 0xF));
}

/*
 * SGI_DISPATCH: nothing to do here. The interrupt return path compares
 * ctxtsk with schedtsk and dispatches (int_asm.S).
 */
LOCAL void knl_ipi_dispatch_handler( UINT intno, UW iar )
{
	knl_pcpu()->ipi_cnt++;
	out_w(GICC_EOIR, iar);			/* SGI EOI carries the source CPU ID */
}

/*
 * SGI_STOP: park this processor (panic, shutdown)
 */
LOCAL void knl_ipi_stop_handler( UINT intno, UW iar )
{
	out_w(GICC_EOIR, iar);
	disint();
	for (;;) {
		Asm("wfi");
	}
}

/* ------------------------------------------------------------------------ */
/*
 * Scheduler: assign the ready tasks to the online processors (design 8.3)
 *	Called inside the kernel lock after every change of the ready queue.
 *	A processor with dispatch disabled keeps its current task. A task
 *	that is running stays on its processor when it is allowed to; the
 *	other processors take the remaining tasks in priority order. Every
 *	processor whose schedtsk changed, except the caller, gets an IPI.
 */
LOCAL BOOL knl_task_assigned( TCB *tcb, TCB **assign, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( assign[i] == tcb ) return TRUE;
	}
	return FALSE;
}

LOCAL INT knl_pick_prc( TCB *tcb, UW cand )
{
	INT	i;

	if ( tcb->runprc != 0 && (cand & (1U << (tcb->runprc - 1))) != 0 ) {
		return tcb->runprc - 1;
	}
	for ( i = 0; i < CNF_MAX_PRCID; i++ ) {
		if ( cand & (1U << i) ) return i;
	}
	return -1;
}

EXPORT void knl_reschedule( void )
{
	TCB	*assign[CNF_MAX_PRCID];
	T_PCPU	*me = knl_pcpu();
	T_PCPU	*p;
	RDYQUE	*rq = &knl_ready_queue;
	UW	free = 0, ipi = 0;
	INT	n_free = 0, n = knl_num_prc, i, pri, prc;
	QUEUE	*q;
	TCB	*tcb;

	for ( i = 0; i < n; i++ ) {
		p = &knl_pcpu_tbl[i];
		assign[i] = NULL;
		if ( p->state != PC_ONLINE ) continue;
		if ( p->dispatch_disabled >= DDS_DISABLE && p->ctxtsk != NULL ) {
			assign[i] = p->ctxtsk;		/* held by tk_dis_dsp */
		} else {
			free |= 1U << i;
			n_free++;
		}
	}

	if ( n_free > 0 ) {
		/* The task holding the object lock goes first */
		tcb = rq->klocktsk;
		if ( tcb != NULL && !knl_task_assigned(tcb, assign, n) ) {
			prc = knl_pick_prc(tcb, ( tcb->assprc != 0 ? tcb->assprc : ~0U ) & free);
			if ( prc >= 0 ) {
				assign[prc] = tcb;
				free &= ~(1U << prc);
				n_free--;
			}
		}
		for ( pri = rq->top_priority; pri < NUM_TSKPRI && n_free > 0; pri++ ) {
			for ( q = rq->tskque[pri].next; q != &rq->tskque[pri] && n_free > 0; q = q->next ) {
				tcb = (TCB*)q;
				if ( knl_task_assigned(tcb, assign, n) ) continue;
				prc = knl_pick_prc(tcb, ( tcb->assprc != 0 ? tcb->assprc : ~0U ) & free);
				if ( prc < 0 ) continue;	/* its processors are all taken */
				assign[prc] = tcb;
				free &= ~(1U << prc);
				n_free--;
			}
		}
	}

	for ( i = 0; i < n; i++ ) {
		p = &knl_pcpu_tbl[i];
		if ( p->state != PC_ONLINE ) continue;
		if ( p->schedtsk != assign[i] ) {
			p->schedtsk = assign[i];
			if ( p != me ) ipi |= 1U << i;
		}
	}
	if ( ipi != 0 ) {
		knl_send_ipi(ipi, SGI_DISPATCH);
	}
}

/* ------------------------------------------------------------------------ */
/*
 * Per-CPU initialization
 */
LOCAL void knl_pcpu_setup( INT i, void *tmp_stack_top )
{
	T_PCPU	*p = &knl_pcpu_tbl[i];
	UD	mpidr;

	Asm("mrs %0, mpidr_el1" : "=r"(mpidr));
	p->prcid = i + 1;
	p->mpidr = mpidr;
	p->ctxtsk = NULL;
	p->schedtsk = NULL;
	p->dispatch_disabled = DDS_ENABLE;
	p->taskindp = 0;
	p->cur_ttbr0 = 0;
	p->fpu_ctx = NULL;
	p->tmp_stack = tmp_stack_top;
	p->irq_stack = &knl_pcpu_irq_stack[i][CNF_EXC_STACK_SIZE];
	p->ctxsw_cnt = 0;
	p->ipi_cnt = 0;

	Asm("msr tpidr_el1, %0" :: "r"((UD)p));
	Asm("isb");
}

/*
 * Primary processor: called first thing in reset_main()
 */
EXPORT void knl_smp_init_primary( void )
{
	INT	i;

	for ( i = 0; i < CNF_MAX_PRCID; i++ ) {
		knl_pcpu_tbl[i].state = PC_OFFLINE;
		knl_pcpu_tbl[i].prcid = i + 1;
	}
	knl_pcpu_setup(0, &__tmp_stack_start);
	knl_pcpu_tbl[0].state = PC_ONLINE;
	knl_num_prc = 1;
}

/*
 * GIC CPU interface and the banked interrupts (SGI 0-15, PPI 16-31)
 * of the calling processor. The distributor is set up once by
 * knl_init_interrupt().
 */
EXPORT void knl_gic_cpu_init( void )
{
	INT	i;

	out_w(GICD_ICENABLER(0), 0xFFFFFFFFU);		/* disable SGI/PPI */
	out_w(GICD_ICPENDR(0), 0xFFFFFFFFU);
	out_w(GICD_IGROUPR(0), 0x00000000U);		/* group 0 */
	for ( i = 0; i < 8; i++ ) {
		out_w(GICD_IPRIORITYR(i), 0xF0F0F0F0U);	/* lowest priority */
	}

	out_w(GICC_PMR, 0xFF);				/* accept all priorities */
	out_w(GICC_BPR, 0);
	out_w(GICC_CTLR, 0x00000001U);			/* enable group 0 signalling */

	EnableInt(SGI_DISPATCH, INTPRI_HIGHEST);
	EnableInt(SGI_STOP, INTPRI_HIGHEST);
	EnableInt(SGI_TIMER, INTPRI_SYSTICK);
}

/*
 * Register the IPI handlers (called by knl_init_interrupt)
 */
EXPORT void knl_smp_init_interrupt( void )
{
	knl_define_inthdr(SGI_DISPATCH, TA_HLNG, (FP)knl_ipi_dispatch_handler);
	knl_define_inthdr(SGI_STOP, TA_HLNG, (FP)knl_ipi_stop_handler);
	knl_define_inthdr(SGI_TIMER, TA_HLNG, (FP)knl_timer_ipi_handler);
}

/* ------------------------------------------------------------------------ */
/*
 * Secondary processors
 */

/*
 * C entry of a secondary processor (boot.S knl_secondary_start_va), running
 * at the virtual address with the MMU on, interrupts disabled, on the
 * temporary stack of its slot.
 */
EXPORT void knl_secondary_main( INT i )
{
	T_PCPU	*p = &knl_pcpu_tbl[i];

	knl_pcpu_setup(i, &knl_pcpu_tmp_stack[i][CNF_TMP_STACK_SIZE]);

	/* Process space: the empty table, like an idle primary */
	Asm("msr ttbr0_el1, %0" :: "r"(knl_ttbr0_null));
	Asm("isb");
	Asm("tlbi vmalle1");
	Asm("dsb ish");
	Asm("isb");
	p->cur_ttbr0 = knl_ttbr0_null;

	knl_gic_cpu_init();
	Asm("msr cntp_ctl_el0, xzr");		/* no timer on this processor */

	__atomic_store_n(&p->state, PC_ONLINE, __ATOMIC_RELEASE);
	Asm("dsb ish");
	Asm("sev");

	/* Pick up work that was queued before this processor came online */
	{
		UINT	s = knl_lock_kernel();
		knl_reschedule();
		knl_unlock_kernel(s);
	}

	knl_force_dispatch();			/* idle loop / first task, no return */
	for (;;) {
		Asm("wfi");
	}
}

/*
 * Start the secondary processors (called from the kernel initialization
 * after the timer is running, interrupts disabled)
 */
EXPORT void knl_smp_start_secondaries( void )
{
#if USE_SMP
	INT	i, n = knl_num_cpu;
	UD	t0;
	ER	er;

	if ( n > CNF_MAX_PRCID ) n = CNF_MAX_PRCID;

	for ( i = 1; i < n; i++ ) {
		T_PCPU	*p = &knl_pcpu_tbl[i];

		knl_secondary_sp[i] = (UD)(UBINT)&knl_pcpu_tmp_stack[i][CNF_TMP_STACK_SIZE];
		p->state = PC_BOOTING;
		Asm("dsb ish");

		er = knl_psci_cpu_on(knl_cpu_mpidr[i], knl_secondary_entry_pa, (UD)i);
		if ( er != E_OK ) {
			tm_printf((UB*)"TessronOS: CPU%d: PSCI CPU_ON failed (%d)\n", i, er);
			p->state = PC_OFFLINE;
			continue;
		}
		t0 = knl_get_mono_ns();
		while ( __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != PC_ONLINE ) {
			if ( knl_get_mono_ns() - t0 > 100000000ULL ) break;	/* 100ms */
			Asm("wfe");
		}
		if ( p->state != PC_ONLINE ) {
			tm_printf((UB*)"TessronOS: CPU%d did not come online\n", i);
			p->state = PC_OFFLINE;
			continue;
		}
		knl_num_prc = i + 1;
	}
	tm_printf((UB*)"TessronOS: %d of %d CPUs online\n", knl_num_prc, knl_num_cpu);
#endif /* USE_SMP */
}

/* ------------------------------------------------------------------------ */
/*
 * tk_get_prc: ID of the processor executing the caller
 */
SYSCALL ID tk_get_prc( void )
{
	return knl_pcpu()->prcid;
}

#endif /* CPU_CORE_ARMV8A */
