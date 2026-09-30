/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	exc_hdl.c (ARMv8-A / AArch64)
 *	Default exception handlers: report and halt.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <kernel.h>
#include "../../../sysdepend.h"
#include "cpu_task.h"
#include <ts/proc.h>

#if USE_EXCEPTION_DBG_MSG
#define EXCEPTION_DBG_MSG(...)	tm_printf((UB*)__VA_ARGS__)
#else
#define EXCEPTION_DBG_MSG(...)
#endif

LOCAL void halt( void )
{
	disint();
	while (1) {
		Asm("wfi");
	}
}

/*
 * A task wrote past the foot of its stack (kernel.h). Whatever lay below
 * the stack is gone with it, so this says which task and how large its
 * stack was, and goes no further.
 */
EXPORT void knl_stack_broken( TCB *tcb )
{
	tm_printf((UB*)"\n!STACK! task=%d ran past the foot of its %d byte stack\n",
		(tcb != NULL) ? tcb->tskid : 0,
		(tcb != NULL) ? (INT)tcb->sstksz : 0);
	halt();
}

/*
 * Synchronous exception other than SVC (data/instruction abort, illegal
 * instruction, alignment fault ...). frame points at the entry frame.
 */
EXPORT void knl_exception_handler( void *frame, UD esr, UD far )
{
	UD	*f = (UD *)frame;
	INT	i;

	EXCEPTION_DBG_MSG("\n!EXCEPTION! ESR=%08x%08x FAR=%08x%08x ELR=%08x%08x SPSR=%08x\n",
		(UW)(esr >> 32), (UW)esr, (UW)(far >> 32), (UW)far,
		(UW)(f[21] >> 32), (UW)f[21], (UW)f[22]);
	EXCEPTION_DBG_MSG(" EC=%02x ISS=%06x  task=%d  indp=%d\n",
		(UW)(esr >> 26) & 0x3f, (UW)esr & 0x1ffffff,
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0, knl_taskindp);
	for ( i = 0; i < 19; i += 2 ) {
		EXCEPTION_DBG_MSG(" x%02d=%08x%08x x%02d=%08x%08x\n",
			i, (UW)(f[i] >> 32), (UW)f[i],
			i + 1, (UW)(f[i + 1] >> 32), (UW)f[i + 1]);
	}
	EXCEPTION_DBG_MSG(" x29=%08x%08x x30=%08x%08x sp=%08x%08x\n",
		(UW)(f[19] >> 32), (UW)f[19], (UW)(f[20] >> 32), (UW)f[20],
		(UW)((UBINT)(f + 24) >> 32), (UW)(UBINT)(f + 24));
	halt();
}

/*
 * Synchronous exception taken in the kernel other than SVC. A data
 * abort at an address of the running process's half -- not mapped, or
 * not to be written -- met while the kernel works for the process in a
 * call, is the process's doing, not the kernel's: the address is given
 * a page of nothing and the access is made again, so the call runs to
 * its end without leaving a volume, a store or a lock half done, and
 * the process ends as the call returns (knl_prc_kfault, design 9.11).
 * Anything else stops the system. Returns only when put right.
 */
#define EC_DABT_EL1	0x25
#define ESR_FNV		( 1UL << 10 )		/* FAR is not valid */

EXPORT void knl_kernel_fault( void *frame, UD esr, UD far )
{
	UD	dfsc = esr & 0x3f;

	/* translation (0x04..0x07), access flag (0x08..0x0b) and permission (0x0c..0x0f) faults */
	if ( ( ( esr >> 26 ) & 0x3f ) == EC_DABT_EL1 && ( esr & ESR_FNV ) == 0
	  && dfsc >= 0x04 && dfsc <= 0x0f
	  && knl_ctxtsk != NULL && knl_taskindp == 0
	  && knl_prc_kfault(knl_ctxtsk->tskctxb.ttbr0, knl_ctxtsk->tskid, (UBINT)far,
			    esr, ((UD *)frame)[21], ((UD *)frame)[20]) ) {
		return;
	}
	knl_exception_handler(frame, esr, far);
}

/*
 * Fault of an EL0 task (data/instruction abort, illegal instruction ...):
 * report and terminate the task; the kernel and the other tasks go on.
 * Runs on the task's system stack with the exception frame at 'frame'.
 * An instruction that is not one (EC 0x00: UDF and the encodings not
 * allocated) and the illegal execution state (EC 0x0e) end the process
 * with TS_ABORT_ILL; every other exception with TS_ABORT_FAULT. What
 * happened is recorded with the process's end for the desktop, which
 * shows it (knl_prc_abort): nothing here builds anything on the screen.
 */
#define EC_UNKNOWN	0x00
#define EC_ILLSTATE	0x0e

EXPORT void knl_el0_fault( void *frame, UD esr, UD far )
{
	T_EXCFRAME	*f = (T_EXCFRAME *)frame;
	T_PRCFLT	info;
	UW		ec = (UW)(esr >> 26) & 0x3f;

	EXCEPTION_DBG_MSG("\n!EL0 fault! task=%d EC=%02x ISS=%06x FAR=%lx ELR=%lx LR=%lx SP_EL0=%lx\n",
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0,
		ec, (UW)esr & 0x1ffffff, far, f->elr, f->lr, f->sp_el0);
	if ( knl_ctxtsk == NULL || knl_taskindp > 0 ) {
		halt();
	}
	enaint(0);		/* tk_ext_tsk is a task-context call: run it with interrupts enabled */
	knl_memset(&info, 0, sizeof(info));
	info.kind = TS_FLT_EL0;
	info.tskid = knl_ctxtsk->tskid;
	info.esr = esr;
	info.far = far;
	info.elr = f->elr;
	info.lr = f->lr;
	/* a task of a process takes the whole process down (design 9.11) */
	knl_prc_abort(knl_ctxtsk->tskctxb.ttbr0,
		( ec == EC_UNKNOWN || ec == EC_ILLSTATE ) ? TS_ABORT_ILL : TS_ABORT_FAULT, &info);
	tk_ext_tsk();		/* no return */
	halt();
}

WEAK_FUNC EXPORT void FIQ_Handler( void )
{
	EXCEPTION_DBG_MSG("\n!FIQ!\n");
	halt();
}

WEAK_FUNC EXPORT void SError_Handler( void )
{
	UD	esr, elr;

	Asm("mrs %0, esr_el1" : "=r"(esr));
	Asm("mrs %0, elr_el1" : "=r"(elr));
	/* asynchronous: ELR is where it was taken, at or after the access that failed */
	EXCEPTION_DBG_MSG("\n!SError! ESR=%08x%08x ELR=%08x%08x task=%d\n",
		(UW)(esr >> 32), (UW)esr, (UW)(elr >> 32), (UW)elr,
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0);
	halt();
}

WEAK_FUNC EXPORT void Default_Handler( void )
{
	EXCEPTION_DBG_MSG("\n!Undefined interrupt!\n");
	halt();
}

WEAK_FUNC EXPORT void SVC_default_Handler( UINT svcno )
{
	EXCEPTION_DBG_MSG("\n!Undefined SVC %d!\n", svcno);
	halt();
}

#endif	/* CPU_CORE_ARMV8A */
