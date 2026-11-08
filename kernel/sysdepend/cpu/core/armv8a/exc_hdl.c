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
#include <ts/lockup.h>

/*
 * What is said of an exception goes to the UART at once, without the
 * console's lock (tm_printf_urgent): the processor that holds that lock
 * may be this one, stopped in the middle of a line, or another that
 * will never let go of it. Nothing said here is lost in a buffer.
 */
#if USE_EXCEPTION_DBG_MSG
#define EXCEPTION_DBG_MSG(...)	tm_printf_urgent((UB*)__VA_ARGS__)
#else
#define EXCEPTION_DBG_MSG(...)
#endif

/*
 * This processor stops. The others go on; they find it taking no
 * interrupts and say so, and the watchdog resets the machine after the
 * time set (lockup.c).
 */
LOCAL void halt( void )
{
	disint();
	knl_lockup_halted();
	while (1) {
		Asm("wfi");
	}
}

/* A report of a stopping exception under way on each processor: one met while saying it stops at once */
LOCAL volatile BOOL	exc_saying[CNF_MAX_PRCID];

LOCAL INT exc_cpu( void )
{
	UD	p;

	Asm("mrs %0, tpidr_el1" : "=r"(p));
	return ( p != 0 ) ? ((T_PCPU *)p)->prcid - 1 : 0;
}

LOCAL void exc_begin( void )
{
	INT	c = exc_cpu();

	if ( c >= 0 && c < CNF_MAX_PRCID ) {
		if ( exc_saying[c] ) {
			tm_urgent_puts((UB*)"\n!EXCEPTION! another one while the last was being told: stopped\n");
			halt();
		}
		exc_saying[c] = TRUE;
	}
}

/*
 * A task wrote past the foot of its stack (kernel.h). Whatever lay below
 * the stack is gone with it, so this says which task and how large its
 * stack was, and goes no further.
 */
EXPORT void knl_stack_broken( TCB *tcb )
{
	disint();
	tm_printf_urgent((UB*)"\n!STACK! CPU%d: task=%d ran past the foot of its %d byte stack\n",
		exc_cpu(), (tcb != NULL) ? tcb->tskid : 0,
		(tcb != NULL) ? (INT)tcb->sstksz : 0);
	knl_lockup_say_task(" ", tcb);
	halt();
}

#if CNF_STK_WATCH
/*
 * make STKWATCH=1 (smp.c): a return address in the stack of a task that
 * was not running was written over. What is said: the word and its two
 * values, the words around it, every task whose wait result would be
 * written near it, and what each processor runs. The others are
 * stopped too, so that the machine stays as it was for a debugger.
 */
IMPORT TCB	knl_tcb_table[];

EXPORT void knl_stack_overwritten( TCB *tcb, UD *at, UD was, UB prc, UD since )
{
	UD	now, *p;
	INT	i;

	disint();
	if ( knl_num_prc > 1 ) {
		knl_send_ipi(( ( 1U << knl_num_prc ) - 1 ) & ~( 1U << exc_cpu() ), SGI_STOP);
	}
	Asm("mrs %0, cntpct_el0" : "=r"(now));
	tm_printf_urgent((UB*)"\n!STKWATCH! CPU%d: task=%d: the return address at %lx was %lx"
			 " when CPU%d gave its context up %d us ago, and is %lx\n",
			 exc_cpu(), tcb->tskid, (UD)(UBINT)at, was, prc,
			 (INT)( ( now - since ) * 1000000 / ( knl_cntfrq != 0 ? knl_cntfrq : 1 ) ), *at);
	knl_lockup_say_task(" ", tcb);
	for ( p = at - 8; p < at + 8; p += 2 ) {
		tm_printf_urgent((UB*)"  %lx: %016lx %016lx\n", (UD)(UBINT)p, p[0], p[1]);
	}
	for ( i = 0; i < NUM_TSKID; i++ ) {
		TCB	*t = &knl_tcb_table[i];
		UBINT	w = (UBINT)t->wercd;

		if ( t->state != 0 && w != 0
		  && w + 0x200 > (UBINT)at && w < (UBINT)at + 0x200 ) {
			tm_printf_urgent((UB*)"  task %d state %d wid %d: its wait result goes to %lx\n",
					 t->tskid, t->state, t->wid, (UD)w);
		}
	}
	for ( i = 0; i < knl_num_prc; i++ ) {
		TCB	*t = knl_pcpu_tbl[i].ctxtsk;

		tm_printf_urgent((UB*)"  CPU%d runs task %d\n", i, ( t != NULL ) ? t->tskid : 0);
	}
	halt();
}
#endif

/*
 * Synchronous exception other than SVC (data/instruction abort, illegal
 * instruction, alignment fault ...). frame points at the entry frame.
 */
EXPORT void knl_exception_handler( void *frame, UD esr, UD far )
{
	UD	*f = (UD *)frame;
	INT	i;
	UINT	s;

	disint();
	exc_begin();
	s = tm_urgent_begin();
	EXCEPTION_DBG_MSG("\n!EXCEPTION! CPU%d ESR=%08x%08x FAR=%08x%08x ELR=%08x%08x SPSR=%08x\n",
		exc_cpu(), (UW)(esr >> 32), (UW)esr, (UW)(far >> 32), (UW)far,
		(UW)(f[21] >> 32), (UW)f[21], (UW)f[22]);
	EXCEPTION_DBG_MSG(" EC=%02x ISS=%06x  task=%d  indp=%d\n",
		(UW)(esr >> 26) & 0x3f, (UW)esr & 0x1ffffff,
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0, knl_taskindp);
	knl_lockup_say_task(" ", knl_ctxtsk);
	for ( i = 0; i < 19; i += 2 ) {
		EXCEPTION_DBG_MSG(" x%02d=%08x%08x x%02d=%08x%08x\n",
			i, (UW)(f[i] >> 32), (UW)f[i],
			i + 1, (UW)(f[i + 1] >> 32), (UW)f[i + 1]);
	}
	EXCEPTION_DBG_MSG(" x29=%08x%08x x30=%08x%08x sp=%08x%08x\n",
		(UW)(f[19] >> 32), (UW)f[19], (UW)(f[20] >> 32), (UW)f[20],
		(UW)((UBINT)(f + 26) >> 32), (UW)(UBINT)(f + 26));
	if ( ( f[22] & 0xf ) != 0 ) {
		knl_lockup_say_frames(" ", (UBINT)f[19]);	/* taken in the kernel: where it was called from */
	}
	tm_urgent_end(s);
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
 * PAN refusing the kernel a page of the process's is taken first
 * (pan_fault). Anything else stops the system. Returns only when put
 * right.
 */
#define EC_DABT_EL1	0x25
#define ESR_FNV		( 1UL << 10 )		/* FAR is not valid */
#define FSC_PERM(f)	( (f) >= 0x0c && (f) <= 0x0f )
#define USER_HALF_END	0x0001000000000000ULL	/* addresses below go through TTBR0 */
#define PTE_AP_EL0	( 1UL << 6 )		/* AP[1]: EL0 may reach the page */

/* The last access PAN refused, for the tests */
EXPORT UD	knl_pan_last_far = 0;
EXPORT UD	knl_pan_last_elr = 0;

/*
 * Whether a data abort is PAN refusing the kernel a page EL0 may reach:
 * a permission fault, taken with PSTATE.PAN set, at an address the
 * running space maps for EL0.
 */
LOCAL BOOL pan_refused( UD *f, UD esr, UD far )
{
	UD	ttbr0, ent;

	if ( !knl_pan_on || ( ( esr >> 26 ) & 0x3f ) != EC_DABT_EL1 || ( esr & ESR_FNV ) != 0
	  || !FSC_PERM(esr & 0x3f) || ( f[22] & PSR_PAN ) == 0 || far >= USER_HALF_END ) {
		return FALSE;
	}
	Asm("mrs %0, ttbr0_el1" : "=r"(ttbr0));
	ent = knl_pt_entry(ttbr0 & 0x0000FFFFFFFFFFFEULL, (UBINT)far & ~(UBINT)0xfff);
	return ( ent != 0 && ( ent & PTE_AP_EL0 ) != 0 );
}

/*
 * PAN refused the kernel a page EL0 may reach: the kernel touched it
 * without opening the way, which it does only after it has made sure of
 * what it was handed. Recorded on the console. In an interrupt handler
 * it stops the system. In a task, the call goes on to its end with the
 * way open -- cut short there it would leave a volume, a store or a lock
 * half done -- and when the task is a process's, the process ends as
 * the call returns, recorded for the desktop (knl_prc_kpan).
 */
LOCAL BOOL pan_fault( UD *f, UD esr, UD far )
{
	TCB	*t = knl_ctxtsk;

	knl_pan_faults++;
	knl_pan_last_far = far;
	knl_pan_last_elr = f[21];
	tm_printf((UB*)"\n!PAN! the kernel touched a user page without making sure of it:"
		  " FAR=%lx ELR=%lx LR=%lx task=%d indp=%d\n",
		  far, f[21], f[20], ( t != NULL ) ? t->tskid : 0, knl_taskindp);
	if ( t == NULL || knl_taskindp > 0 ) {
		return FALSE;
	}
	(void)knl_prc_kpan(t->tskctxb.ttbr0, t->tskid, (UBINT)far, esr, f[21], f[20]);
	f[22] &= ~(UD)PSR_PAN;		/* the way open until the call is over */
	return TRUE;
}

/*
 * A task reached the unmapped pages below its stack (knl_vmap_stack).
 * The summary of it, the rest being told as for any exception.
 */
EXPORT volatile UW	knl_stack_out_count = 0;	/* for the tests */
EXPORT volatile UD	knl_stack_out_addr = 0;

LOCAL void stack_out_say( UD *f, UD far, UD sp )
{
	knl_stack_out_addr = far;
	tm_printf_urgent((UB*)"\n!STACK! CPU%d: task=%d ran out of its %d byte stack: %lx, at ELR %lx, LR %lx, SP %lx\n",
		exc_cpu(), ( knl_ctxtsk != NULL ) ? knl_ctxtsk->tskid : 0,
		( knl_ctxtsk != NULL ) ? (INT)knl_ctxtsk->sstksz : 0, far, f[21], f[20], sp);
	__atomic_add_fetch(&knl_stack_out_count, 1, __ATOMIC_SEQ_CST);
}

/*
 * The entry of the exception (vector.S) found no room for its frame on
 * the stack, and came here on the interrupt stack with the stack pointer
 * as it was. No return.
 */
EXPORT void knl_stack_out( void *frame, UD esr, UD far, UD sp )
{
	stack_out_say((UD *)frame, far, sp);
	knl_exception_handler(frame, esr, far);
	halt();
}

EXPORT void knl_kernel_fault( void *frame, UD esr, UD far )
{
	UD	dfsc = esr & 0x3f;

	if ( knl_stack_guard_hit(knl_ctxtsk, (UBINT)far) ) {
		stack_out_say((UD *)frame, far, (UD)(UBINT)( (UD *)frame + 26 ));
		knl_exception_handler(frame, esr, far);
	}

	if ( pan_refused((UD *)frame, esr, far) ) {
		if ( pan_fault((UD *)frame, esr, far) ) {
			return;
		}
		knl_exception_handler(frame, esr, far);
	}

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
 *
 * An abort the process's memory can put right first goes to
 * knl_prc_ufault, as a call of the task would (interrupts enabled, the
 * task marked as inside the kernel so that it is not held still or
 * ended while it holds the process table's lock): a page of a stretch
 * written and run by turns is turned for the access, or a page another
 * task was changing is there by now. Then this returns, and the access
 * is made again. A data abort it does not put right goes to the
 * program's entry for faults when it named one (knl_prc_flt_give): the
 * frame is turned to the entry, and this returns there.
 */
#define EC_UNKNOWN	0x00
#define EC_ILLSTATE	0x0e
#define EC_IABT_EL0	0x20
#define EC_DABT_EL0	0x24

IMPORT BOOL knl_prc_ufault( UBINT va, UD esr );

EXPORT void knl_el0_fault( void *frame, UD esr, UD far )
{
	T_EXCFRAME	*f = (T_EXCFRAME *)frame;
	T_PRCFLT	info;
	UW		ec = (UW)(esr >> 26) & 0x3f;
	TCB		*me = knl_ctxtsk;
	BOOL		fixed;

	if ( ( ec == EC_IABT_EL0 || ec == EC_DABT_EL0 ) && me != NULL && knl_taskindp == 0 ) {
		me->svcdepth++;
		enaint(0);
		fixed = knl_prc_ufault((UBINT)far, esr);
		if ( !fixed ) {
			/* the program's own entry for it (ts_def_flt) takes it */
			fixed = knl_prc_flt_give(frame, esr, far);
		}
		disint();
		me->svcdepth--;
		if ( fixed ) {
			return;
		}
	}

	EXCEPTION_DBG_MSG("\n!EL0 fault! task=%d EC=%02x ISS=%06x FAR=%lx ELR=%lx LR=%lx SP_EL0=%lx\n",
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0,
		ec, (UW)esr & 0x1ffffff, far, f->elr, f->lr, f->sp_el0);
	{
		/* what the caches need for written instructions to be fetched (knl_icache_sync) */
		UD	ctr, clidr;

		Asm("mrs %0, ctr_el0" : "=r"(ctr));
		Asm("mrs %0, clidr_el1" : "=r"(clidr));
		EXCEPTION_DBG_MSG(" CTR_EL0=%lx CLIDR_EL1=%lx\n", ctr, clidr);
	}
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

/* Where an asynchronous exception found the processor: its entry frame (vector.S) */
LOCAL void exc_say_frame( UD *f )
{
	EXCEPTION_DBG_MSG(" ELR=%lx SPSR=%lx LR=%lx x29=%lx SP=%lx indp=%d\n",
		f[21], f[22], f[20], f[19],
		( ( f[22] & 0xf ) == 0 ) ? f[23] : (UD)(UBINT)( f + 26 ), knl_taskindp);
	knl_lockup_say_task(" ", knl_ctxtsk);
	if ( ( f[22] & 0xf ) != 0 ) {
		knl_lockup_say_frames(" ", (UBINT)f[19]);
	}
}

WEAK_FUNC EXPORT void FIQ_Handler( void *frame )
{
	UINT	s;

	exc_begin();
	s = tm_urgent_begin();
	EXCEPTION_DBG_MSG("\n!FIQ! CPU%d\n", exc_cpu());
	exc_say_frame((UD *)frame);
	tm_urgent_end(s);
	halt();
}

WEAK_FUNC EXPORT void SError_Handler( void *frame )
{
	UD	esr, elr;
	UINT	s;

	Asm("mrs %0, esr_el1" : "=r"(esr));
	Asm("mrs %0, elr_el1" : "=r"(elr));
	exc_begin();
	s = tm_urgent_begin();
	/* asynchronous: ELR is where it was taken, at or after the access that failed */
	EXCEPTION_DBG_MSG("\n!SError! CPU%d ESR=%08x%08x ELR=%08x%08x task=%d\n",
		exc_cpu(), (UW)(esr >> 32), (UW)esr, (UW)(elr >> 32), (UW)elr,
		(knl_ctxtsk != NULL) ? knl_ctxtsk->tskid : 0);
	exc_say_frame((UD *)frame);
	tm_urgent_end(s);
	halt();
}

WEAK_FUNC EXPORT void Default_Handler( UINT intno )
{
	EXCEPTION_DBG_MSG("\n!Undefined interrupt! CPU%d interrupt %d\n", exc_cpu(), intno);
	halt();
}

WEAK_FUNC EXPORT void SVC_default_Handler( UINT svcno )
{
	EXCEPTION_DBG_MSG("\n!Undefined SVC %d!\n", svcno);
	halt();
}

#endif	/* CPU_CORE_ARMV8A */
