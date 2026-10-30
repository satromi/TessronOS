/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cpu_cntl.c (x86-64)
 *	CPU control: task register access, the x87/SSE/AVX state, the way to
 *	the user pages, the user stacks of tasks.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include <tm/tmonitor.h>
#include "../../../sysdepend.h"
#include "cpu_task.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include "../../../../tkernel/task.h"

/* ------------------------------------------------------------------------ */
/*
 * The x87, SSE and AVX state. With XSAVE (CPUID 1:ECX bit 26) the
 * components the kernel lets tasks use are x87, SSE and AVX (XCR0); the
 * area of a task is as large as CPUID 0Dh says they take. Without it,
 * the 512 bytes of FXSAVE. The kernel's code uses none of the registers
 * (-mgeneral-regs-only), so only TA_FPU tasks keep a copy.
 */
EXPORT UW	knl_xsave_size = 512;
EXPORT BOOL	knl_xsave_on = FALSE;
EXPORT UD	knl_xcr0 = 0;

EXPORT void knl_fpu_cpu_init( void )
{
	UW	a, b, c, d;
	UD	cr0, cr4;

	Asm("mov %%cr0, %0" : "=r"(cr0));
	cr0 = ( cr0 & ~(UD)( CR0_EM | CR0_TS ) ) | CR0_MP | CR0_NE;
	Asm("mov %0, %%cr0" :: "r"(cr0));
	Asm("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

	cpuid(1, 0, &a, &b, &c, &d);
	if ( ( c & ( 1U << 26 ) ) != 0 ) {
		cr4 |= CR4_OSXSAVE;
		Asm("mov %0, %%cr4" :: "r"(cr4));
		cpuid(0xd, 0, &a, &b, &c, &d);
		knl_xcr0 = ( (UD)d << 32 | a ) & ( XCR0_X87 | XCR0_SSE | XCR0_AVX );
		Asm("xsetbv" :: "c"(0), "a"((UW)knl_xcr0), "d"((UW)( knl_xcr0 >> 32 )));
		cpuid(0xd, 0, &a, &b, &c, &d);	/* ebx: the size for what XCR0 now enables */
		knl_xsave_size = ( b + 63 ) & ~63U;
		knl_xsave_on = TRUE;
	} else {
		Asm("mov %0, %%cr4" :: "r"(cr4));
	}
	Asm("fninit");
	{
		UW	mx = MXCSR_INIT;

		Asm("ldmxcsr %0" :: "m"(mx));
	}
}

/* ------------------------------------------------------------------------ */
/*
 * Supervisor mode access prevention (cpu_uaccess.h) and execution
 * prevention: SMEP keeps the kernel from running a user page, SMAP from
 * reading or writing one while RFLAGS.AC is clear. The boot processor
 * decides from CPUID before any task is made, and each processor sets
 * itself up as it starts; the processors of a machine are all of a kind.
 */
EXPORT	BOOL	knl_pan_on = FALSE;
EXPORT	UW	knl_pan_faults = 0;

EXPORT void knl_pan_setup( void )
{
	UW	a, b, c, d;
	UD	cr4;

	cpuid(7, 0, &a, &b, &c, &d);
	Asm("mov %%cr4, %0" : "=r"(cr4));
	if ( ( b & ( 1U << 7 ) ) != 0 ) {
		cr4 |= CR4_SMEP;
	}
	if ( ( b & ( 1U << 20 ) ) != 0 ) {
		Asm("clac" ::: "memory");
		cr4 |= CR4_SMAP;
		knl_pan_on = TRUE;
	}
	Asm("mov %0, %%cr4" :: "r"(cr4));
}

EXPORT void knl_ua_get( void *dst, CONST void *usrc, SZ len )
{
	BOOL	o = knl_ua_open();

	knl_memcpy(dst, usrc, len);
	knl_ua_close(o);
}

EXPORT void knl_ua_put( void *udst, CONST void *src, SZ len )
{
	BOOL	o = knl_ua_open();

	knl_memcpy(udst, src, len);
	knl_ua_close(o);
}

/*
 * The start a task of protection level 2/3 is given when it has no user
 * stack: it runs none of its own code in the kernel's mode, but says so
 * and ends. tk_sta_tsk refuses such a task, so this is reached only if
 * that check is ever passed by.
 */
EXPORT void knl_task_no_ustack( INT stacd, void *exinf )
{
	TCB	*tcb = knl_self_ctxtsk();

	tm_printf((UB*)"\n!USTACK! task=%d of protection level %d has no user stack: it does not run\n",
		  ( tcb != NULL ) ? tcb->tskid : 0,
		  ( tcb != NULL ) ? (INT)( tcb->tskatr & TA_RNG3 ) >> 8 : 0);
	tk_ext_tsk();
}

#ifdef USE_KTEST
/* For the tests: the next user task made finds no memory for its user stack */
EXPORT BOOL knl_ustack_refuse = FALSE;
#endif

/*
 * TCB system dependent part: user tasks (TA_RNG2/3) get a user stack in
 * the system process space at creation and give it back at deletion. It
 * stays allocated across termination and restart of the task.
 */
EXPORT ER knl_tcb_sysdep_cre( TCB *tcb, CONST T_CTSK *pk_ctsk )
{
	tcb->tskctxb.ustk = NULL;
	tcb->tskctxb.usp = NULL;
	tcb->tskctxb.ustk_npages = 0;
	tcb->tskctxb.ttbr0 = knl_ttbr0_null;
	tcb->tskctxb.fsbase = 0;

	if ( (tcb->tskatr & TA_RNG3) >= TA_RNG2 ) {
		UD	size = ( knl_ustack_size != 0 ) ? knl_ustack_size : CNF_USER_STKSZ;
		UD	npages = size >> PAGE_SHIFT;
#ifdef USE_KTEST
		if ( knl_ustack_refuse ) {
			knl_ustack_refuse = FALSE;
			return E_NOMEM;
		}
#endif
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
 * User stack size of the next user task created. Process creation sets
 * it around tk_cre_tsk under the process mutex; 0 means the default.
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
			knl_ustack_free_l0(tcb->tskctxb.ttbr0 & MMU_ROOT_PA,
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
 * with interrupts disabled). STI lets the next instruction run before
 * any interrupt is taken, so one that is already pending wakes the HLT;
 * it is taken there, and the loop goes on with interrupts disabled again.
 */
EXPORT void low_pow( void )
{
	Asm("sti; hlt; cli" ::: "memory");
}

/*
 * Suspend: not supported yet, behaves like low_pow()
 */
EXPORT void off_pow( void )
{
	Asm("sti; hlt; cli" ::: "memory");
}

/*
 * Set task register contents (tk_set_reg)
 *	The segments and the mode of the frame stay as they are, whatever
 *	eit->rflags says: only the arithmetic flags are taken from it (and
 *	the interrupt flag where the frame is one of the kernel's mode). A
 *	task that was in user mode goes back to it with interrupts enabled;
 *	no value given here takes a task to the kernel's mode, or opens the
 *	way to the user pages.
 */
EXPORT void knl_set_reg( TCB *tcb, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs )
{
	SStackFrame	*ssp;

	ssp = tcb->tskctxb.ssp;

	if ( cregs != NULL ) {
		ssp = cregs->ssp;
		tcb->tskctxb.ssp = ssp;
		if ( ( ssp->cs & SEL_RPL3 ) != 0 ) {
			ssp->rsp = (UD)(UBINT)cregs->usp;
		}
	}

	if ( regs != NULL ) {
		ssp->rax = (UD)regs->rax;
		ssp->rbx = (UD)regs->rbx;
		ssp->rcx = (UD)regs->rcx;
		ssp->rdx = (UD)regs->rdx;
		ssp->rsi = (UD)regs->rsi;
		ssp->rdi = (UD)regs->rdi;
		ssp->rbp = (UD)regs->rbp;
		ssp->r8  = (UD)regs->r8;
		ssp->r9  = (UD)regs->r9;
		ssp->r10 = (UD)regs->r10;
		ssp->r11 = (UD)regs->r11;
		ssp->r12 = (UD)regs->r12;
		ssp->r13 = (UD)regs->r13;
		ssp->r14 = (UD)regs->r14;
		ssp->r15 = (UD)regs->r15;
	}

	if ( eit != NULL ) {
		ssp->rip = (UD)(UBINT)eit->pc;
		if ( ( ssp->cs & SEL_RPL3 ) != 0 ) {
			ssp->rflags = ( eit->rflags & RFL_ARITH ) | RFL_FIXED | RFL_IF;
		} else {
			ssp->rflags = ( eit->rflags & ( RFL_ARITH | RFL_IF ) )
				    | ( ssp->rflags & ~(UD)( RFL_ARITH | RFL_IF ) );
		}
	}
}

/*
 * Get task register contents (tk_get_reg)
 */
EXPORT void knl_get_reg( TCB *tcb, T_REGS *regs, T_EIT *eit, T_CREGS *cregs )
{
	SStackFrame	*ssp;

	ssp = tcb->tskctxb.ssp;

	if ( regs != NULL ) {
		regs->rax = (VD)ssp->rax;
		regs->rbx = (VD)ssp->rbx;
		regs->rcx = (VD)ssp->rcx;
		regs->rdx = (VD)ssp->rdx;
		regs->rsi = (VD)ssp->rsi;
		regs->rdi = (VD)ssp->rdi;
		regs->rbp = (VD)ssp->rbp;
		regs->r8  = (VD)ssp->r8;
		regs->r9  = (VD)ssp->r9;
		regs->r10 = (VD)ssp->r10;
		regs->r11 = (VD)ssp->r11;
		regs->r12 = (VD)ssp->r12;
		regs->r13 = (VD)ssp->r13;
		regs->r14 = (VD)ssp->r14;
		regs->r15 = (VD)ssp->r15;
	}

	if ( eit != NULL ) {
		eit->pc       = (void *)(UBINT)ssp->rip;
		eit->rflags   = ssp->rflags;
		eit->taskmode = 0;
	}

	if ( cregs != NULL ) {
		cregs->ssp = tcb->tskctxb.ssp;
		cregs->usp = ( ( ssp->cs & SEL_RPL3 ) != 0 ) ? (void *)(UBINT)ssp->rsp : NULL;
	}
}

#if USE_FPU
#ifdef USE_FUNC_TK_SET_CPR
EXPORT ER knl_set_cpr( TCB *tcb, INT copno, CONST T_COPREGS *copregs )
{
	return E_NOSPT;
}
#endif
#ifdef USE_FUNC_TK_GET_CPR
EXPORT ER knl_get_cpr( TCB *tcb, INT copno, T_COPREGS *copregs )
{
	return E_NOSPT;
}
#endif
#endif /* USE_FPU */

#endif /* CPU_CORE_X86_64 */
