/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_task.h
 *	CPU-dependent task control (ARMv8-A / AArch64 core)
 */

#ifndef _SYSDEPEND_CPU_CORE_CPUTASK_
#define _SYSDEPEND_CPU_CORE_CPUTASK_

#include "sysman/pfalloc.h"
#include "sysman/space.h"

/*
 * System stack frame of a task that is not running.
 *
 *	high address
 *	  +-------------------+
 *	  | tpidr_el0, pad    |  \
 *	  | sp_el0            |   |
 *	  | spsr  (SPSR_EL1)  |   | pushed by the exception entry (vector.S)
 *	  | elr   (ELR_EL1)   |   |
 *	  | x30, x29          |   |
 *	  | x18 .. x0         |  /
 *	  +-------------------+
 *	  | x28 .. x19        |  pushed by the dispatcher (dispatch.S)
 *	  +-------------------+ <- tskctxb.ssp
 *	low address
 *
 *	Both parts are multiples of 16 bytes so that SP stays 16-byte aligned.
 */
typedef struct {
	UD	x19_28[10];	/* callee-saved registers x19-x28 */
	/* exception entry frame */
	UD	x[19];		/* x0-x18 */
	UD	fp;		/* x29 */
	UD	lr;		/* x30 */
	UD	elr;		/* return address */
	UD	spsr;		/* saved PSTATE */
	UD	sp_el0;		/* user stack pointer (EL0 tasks) */
	UD	tpidr_el0;	/* thread pointer (TLS) */
	UD	pad;
} SStackFrame;

/*
 * Exception entry frame alone (what vector.S pushes; SP points at x[0]).
 * Handlers that receive the frame pointer (SVC gateway, fault handlers)
 * use this view.
 */
typedef struct {
	UD	x[19];		/* x0-x18 */
	UD	fp;		/* x29 */
	UD	lr;		/* x30 */
	UD	elr;
	UD	spsr;
	UD	sp_el0;
	UD	tpidr_el0;
	UD	pad;
} T_EXCFRAME;

#define SSF_EXC_SIZE		(26 * 8)	/* size of the exception entry frame (208) */
#define SSF_CALLEE_SIZE		(10 * 8)	/* size of the callee-saved area (80) */

#define DORMANT_STACK_SIZE	( sizeof(SStackFrame) + 0x10 )

#if USE_FPU
/*
 * FP/SIMD context, placed at the top of the task system stack.
 */
typedef struct {
	UD	q[32][2];	/* v0-v31 */
	UD	fpsr;
	UD	fpcr;
} FPUContext;

#define knl_fpu_ctx	(knl_pcpu()->fpu_ctx)	/* Task that owns the FP/SIMD registers (per processor) */
#endif /* USE_FPU */

/*
 * Create stack frame for task startup
 *	Protection level 0/1: runs at EL1h on the system stack.
 *	Protection level 2/3: runs at EL0t on a user stack in the system
 *	process space; the system stack takes exceptions and system calls.
 *	The task starts with interrupts enabled.
 */
Inline void knl_setup_context( TCB *tcb )
{
	SStackFrame	*ssp;
	UINT		rng = tcb->tskatr & TA_RNG3;

	ssp = (SStackFrame *)((UBINT)tcb->isstack & ~(UBINT)0xf);
#if USE_FPU
	if ( (tcb->tskatr & TA_FPU) != 0 ) {
		FPUContext *fpu = (FPUContext *)ssp;
		(--fpu)->fpcr	= FPCR_INIT;
		fpu->fpsr	= 0;
		ssp = (SStackFrame *)fpu;
	}
#endif /* USE_FPU */
	ssp--;

	ssp->fp		= 0;
	ssp->lr		= 0;
	ssp->tpidr_el0	= 0;
	ssp->elr	= (UD)(UBINT)tcb->task;		/* Task startup address */

	if ( rng >= TA_RNG2 && tcb->tskctxb.ustk != NULL ) {
		/* EL0 task: user stack allocated by knl_tcb_sysdep_cre() */
		ssp->spsr	= PSR_M_EL0t;
		ssp->sp_el0	= (UD)(UBINT)tcb->tskctxb.usp;
		if ( tcb->tskctxb.ttbr0 == knl_ttbr0_null ) {
			/* not created for a process: the system process space */
			tcb->tskctxb.ttbr0 = knl_sysprc_ttbr0;
		}
	} else {
		ssp->spsr	= PSR_M_EL1h;			/* EL1, SP_EL1, DAIF clear */
		ssp->sp_el0	= 0;
		tcb->tskctxb.ttbr0 = knl_ttbr0_null;
	}
	tcb->tskctxb.ssp = ssp;				/* System stack */
}

/*
 * Set task startup code
 *	x0 = stacd, x1 = exinf
 */
Inline void knl_setup_stacd( TCB *tcb, INT stacd )
{
	SStackFrame	*ssp = tcb->tskctxb.ssp;

	ssp->x[0] = (UD)(BINT)stacd;
	ssp->x[1] = (UD)(UBINT)tcb->exinf;
}

/*
 * Delete task context
 */
Inline void knl_cleanup_context( TCB *tcb )
{
#if	USE_FPU
	if ( (tcb->tskatr & TA_FPU) != 0 ) {
		if ( knl_fpu_ctx == tcb ) {
			knl_fpu_ctx = NULL;
		}
	}
#endif	/* USE_FPU */
	/* the user stack is kept for a restart; knl_tcb_sysdep_del() releases it */
}

#endif /* _SYSDEPEND_CPU_CORE_CPUTASK_ */
