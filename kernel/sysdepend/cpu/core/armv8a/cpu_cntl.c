/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c (ARMv8-A / AArch64)
 *	CPU control: task register access, kernel state variables.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "cpu_task.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include "../../../../tkernel/task.h"

EXPORT	UBINT	knl_dtb_addr = 0;		/* DTB physical address */
EXPORT	UW	knl_boot_el = 0;		/* exception level at entry (1 or 2) */
EXPORT	UD	knl_cntfrq = CNTFRQ_DEFAULT;	/* Generic Timer frequency */

/*
 * TCB system dependent part: EL0 tasks (TA_RNG2/3) get a user stack in the
 * system process space at creation and give it back at deletion. It stays
 * allocated across termination and restart of the task.
 */
EXPORT ER knl_tcb_sysdep_cre( TCB *tcb, CONST T_CTSK *pk_ctsk )
{
	tcb->tskctxb.ustk = NULL;
	tcb->tskctxb.usp = NULL;
	tcb->tskctxb.ustk_npages = 0;
	tcb->tskctxb.ttbr0 = knl_ttbr0_null;

	if ( (tcb->tskatr & TA_RNG3) >= TA_RNG2 ) {
		UD	size = ( knl_ustack_size != 0 ) ? knl_ustack_size : CNF_USER_STKSZ;
		UD	npages = size >> PAGE_SHIFT;
		tcb->tskctxb.ustk = knl_ustack_alloc(npages, &tcb->tskctxb.usp);
		if ( tcb->tskctxb.ustk == NULL ) {
			return E_NOMEM;
		}
		tcb->tskctxb.ustk_npages = npages;
		if ( knl_new_task_space != NULL ) {
			tcb->tskctxb.ttbr0 = knl_new_task_space->ttbr0;
		}
	}
	return E_OK;
}

/*
 * User stack size of the next EL0 task created. Process creation sets it
 * around tk_cre_tsk under the process mutex; 0 means the default.
 */
EXPORT UD knl_ustack_size = 0;

EXPORT void knl_set_ustack_size( UD size )
{
	knl_ustack_size = size;
}

/*
 * Install a process space on a dormant task. The context was built for
 * the system process when the task was created, so the value saved in
 * the task's frame is replaced here as well.
 */
EXPORT ER knl_tsk_set_space( ID tskid, UD ttbr0 )
{
	TCB	*tcb;

	if ( tskid <= 0 || tskid > NUM_TSKID ) {
		return E_ID;
	}
	tcb = &knl_tcb_table[tskid - 1];
	if ( tcb->state != TS_DORMANT ) {
		return E_OBJ;
	}
	tcb->tskctxb.ttbr0 = ttbr0;

	return E_OK;
}

EXPORT ER knl_tcb_sysdep_del( TCB *tcb )
{
	/*
	 * A task of a process keeps its stack in that process's space. It
	 * goes back as the task goes, so that a process making and
	 * deleting tasks does not pile stacks up in its space; the task's
	 * space is still there, for its tasks go before it is taken down.
	 */
	if ( tcb->tskctxb.ttbr0 != knl_sysprc_ttbr0 ) {
		if ( tcb->tskctxb.ustk != NULL && tcb->tskctxb.ttbr0 != knl_ttbr0_null ) {
			knl_ustack_free_l0(tcb->tskctxb.ttbr0 & 0x0000FFFFFFFFFFFEULL,
					   tcb->tskctxb.ustk, tcb->tskctxb.ustk_npages);
		}
		tcb->tskctxb.ustk = NULL;
		tcb->tskctxb.usp = NULL;
		tcb->tskctxb.ustk_npages = 0;
		return E_OK;
	}
	if ( tcb->tskctxb.ustk != NULL ) {
		knl_ustack_free(tcb->tskctxb.ustk, tcb->tskctxb.ustk_npages);
		tcb->tskctxb.ustk = NULL;
		tcb->tskctxb.usp = NULL;
		tcb->tskctxb.ustk_npages = 0;
	}
	return E_OK;
}

/*
 * Low power: wait for an interrupt (called from the dispatcher idle loop
 * with IRQ masked; WFI still wakes on a pending interrupt).
 */
EXPORT void low_pow( void )
{
	Asm("wfi");
}

/*
 * Suspend: not supported yet, behaves like low_pow()
 */
EXPORT void off_pow( void )
{
	Asm("wfi");
}

/*
 * Set task register contents (tk_set_reg)
 */
EXPORT void knl_set_reg( TCB *tcb, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs )
{
	SStackFrame	*ssp;
	INT		i;

	ssp = tcb->tskctxb.ssp;

	if ( cregs != NULL ) {
		ssp = cregs->ssp;
		tcb->tskctxb.ssp = ssp;
	}

	if ( regs != NULL ) {
		for ( i = 0; i < 19; ++i ) {
			ssp->x[i] = (UD)regs->x[i];
		}
		for ( i = 19; i < 29; ++i ) {
			ssp->x19_28[i - 19] = (UD)regs->x[i];
		}
		ssp->fp = (UD)(UBINT)regs->fp;
		ssp->lr = (UD)(UBINT)regs->lr;
	}

	if ( eit != NULL ) {
		ssp->elr  = (UD)(UBINT)eit->pc;
		ssp->spsr = eit->spsr;
	}
}

/*
 * Get task register contents (tk_get_reg)
 */
EXPORT void knl_get_reg( TCB *tcb, T_REGS *regs, T_EIT *eit, T_CREGS *cregs )
{
	SStackFrame	*ssp;
	INT		i;

	ssp = tcb->tskctxb.ssp;

	if ( regs != NULL ) {
		for ( i = 0; i < 19; ++i ) {
			regs->x[i] = (VD)ssp->x[i];
		}
		for ( i = 19; i < 29; ++i ) {
			regs->x[i] = (VD)ssp->x19_28[i - 19];
		}
		regs->fp = (void *)(UBINT)ssp->fp;
		regs->lr = (void *)(UBINT)ssp->lr;
	}

	if ( eit != NULL ) {
		eit->pc       = (void *)(UBINT)ssp->elr;
		eit->spsr     = ssp->spsr;
		eit->taskmode = 0;
	}

	if ( cregs != NULL ) {
		cregs->ssp = tcb->tskctxb.ssp;
	}
}

#if USE_FPU
#ifdef USE_FUNC_TK_SET_CPR
EXPORT ER knl_set_cpr( TCB *tcb, INT copno, CONST T_COPREGS *copregs )
{
	return E_NOSPT;		/* FP/SIMD context switching: later phase */
}
#endif
#ifdef USE_FUNC_TK_GET_CPR
EXPORT ER knl_get_cpr( TCB *tcb, INT copno, T_COPREGS *copregs )
{
	return E_NOSPT;
}
#endif
#endif /* USE_FPU */

#endif /* CPU_CORE_ARMV8A */
