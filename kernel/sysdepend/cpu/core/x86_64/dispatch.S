/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dispatch.S (x86-64)
 *	Task dispatcher, per processor.
 *
 *	The GS base points at the T_PCPU of the running processor. The
 *	dispatcher switches from pcpu->ctxtsk to pcpu->schedtsk; while
 *	schedtsk is NULL it idles on the processor's temporary stack with
 *	interrupts enabled around HLT.
 *
 *	It is entered with the frame of the task (T_EXCFRAME) on the task's
 *	system stack: by the software interrupt SVC_DISPATCH, or from the
 *	interrupt return path. The frame holds every register, so a task's
 *	context is its frame, the FS base (its TLS) and, for a TA_FPU task,
 *	the x87/SSE/AVX state in the area at the top of its system stack.
 *
 *	TCB.runprc marks the processor that holds a task's register context.
 *	A task is switched in only after the processor that ran it last has
 *	saved its context and cleared runprc (a store, which x86 keeps in
 *	order after the stores of the context); a processor that finds
 *	runprc set keeps the interrupt window open so that a pending IPI can
 *	change its schedtsk.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#define	_in_asm_source_
#include <config.h>
#include <sys/sysdef.h>
#include "offset.h"

#define TA_FPU_BIT	0x1000

	/* rdi = the task's area at the top of its system stack (cpu_task.h) */
	.macro	FPU_AREA tcb
	mov	TCB_isstack(\tcb), %rdi
	and	$~63, %rdi
	mov	knl_xsave_size(%rip), %eax
	sub	%rax, %rdi
	.endm

	.macro	SAVE_FPU tcb
#if USE_FPU
	testl	$TA_FPU_BIT, TCB_tskatr(\tcb)
	jz	99f
	FPU_AREA \tcb
	cmpl	$0, knl_xsave_on(%rip)
	je	98f
	mov	$-1, %eax
	mov	$-1, %edx
	xsave64	(%rdi)
	jmp	99f
98:	fxsave64 (%rdi)
99:
#endif
	.endm

	.macro	LOAD_FPU tcb
#if USE_FPU
	testl	$TA_FPU_BIT, TCB_tskatr(\tcb)
	jz	99f
	FPU_AREA \tcb
	cmpl	$0, knl_xsave_on(%rip)
	je	98f
	mov	$-1, %eax
	mov	$-1, %edx
	xrstor64 (%rdi)
	jmp	99f
98:	fxrstor64 (%rdi)
99:
#endif
	.endm

	.text
	.code64

/*
 * knl_dispatch_to_schedtsk
 *	Throw away the current context (task exit / system start) and
 *	dispatch to schedtsk. Entered through SVC_FORCE_DISPATCH with
 *	interrupts masked.
 */
	.global	knl_dispatch_to_schedtsk
	.type	knl_dispatch_to_schedtsk, @function
knl_dispatch_to_schedtsk:
	movl	$1, %gs:PCPU_dispatch_disabled	/* dispatch disable */
	mov	%gs:PCPU_ctxtsk, %r8
	test	%r8, %r8
	jz	l_dispatch0
	movw	$0, TCB_runprc(%r8)		/* context released (RUNPRC, kernel.h) */
	movq	$0, %gs:PCPU_ctxtsk		/* ctxtsk = NULL */
	mov	%gs:PCPU_tmp_stack, %rsp	/* off the stack of the task that ended */
	call	knl_acct_leave			/* charged before its control block may go */
	call	knl_reap_task			/* one that deleted itself is given back now (task_manage.c) */
	jmp	l_dispatch0
	.size	knl_dispatch_to_schedtsk, . - knl_dispatch_to_schedtsk

/*
 * knl_dispatch_entry
 *	Save the current task's context and dispatch to schedtsk.
 *	Entered through SVC_DISPATCH (or from the interrupt return path)
 *	with the frame on the task's system stack and interrupts masked.
 */
	.global	knl_dispatch_entry
	.type	knl_dispatch_entry, @function
knl_dispatch_entry:
	movl	$1, %gs:PCPU_dispatch_disabled	/* dispatch disable */
	mov	%gs:PCPU_ctxtsk, %rbx		/* rbx = ctxtsk */
	test	%rbx, %rbx
	jz	l_dispatch0
	mov	%rsp, (TCB_tskctxb + CTXB_ssp)(%rbx)	/* save SP to TCB */
	SAVE_FPU %rbx				/* before another processor may take it */
	mov	$MSR_FS_BASE, %ecx		/* the task's TLS pointer */
	rdmsr
	shl	$32, %rdx
	or	%rdx, %rax
	mov	%rax, (TCB_tskctxb + CTXB_fsbase)(%rbx)
#if CNF_STK_WATCH
	mov	%rbx, %rdi
	call	knl_stk_snap			/* its frames' return addresses noted (smp.c) */
#endif
	movw	$0, TCB_runprc(%rbx)		/* context saved: release the task */
	movq	$0, %gs:PCPU_ctxtsk		/* ctxtsk = NULL */

l_dispatch0:
	mov	%gs:PCPU_tmp_stack, %rsp	/* temporary stack of this processor */
	call	knl_acct_leave			/* the task that ran is charged its time (smp.c) */

l_dispatch1:
	cli
	mov	%gs:PCPU_schedtsk, %rax
	test	%rax, %rax
	jnz	l_dispatch2
	cmpl	$0, knl_lowpow_discnt(%rip)
	jnz	1f
	call	low_pow				/* HLT: wakes on any interrupt, which is taken there */
1:	sti					/* pending interrupts are taken here */
	nop
	cli
	jmp	l_dispatch1

l_dispatch2:
	/*
	 * Take the task under the kernel lock (smp.c): schedtsk is then
	 * consistent with the ready queue and every value the task will read
	 * on return has been written. NULL when the processor that ran the
	 * task last has not saved its context yet: open the interrupt window
	 * so that an IPI can change schedtsk, then retry.
	 */
	call	knl_dispatch_pick
	test	%rax, %rax
	jnz	3f
	sti
	nop
	cli
	jmp	l_dispatch1
3:
	mov	%rax, %rbx			/* rbx = task to run (ctxtsk, runprc set) */
	LOAD_FPU %rbx
	mov	$MSR_FS_BASE, %ecx
	mov	(TCB_tskctxb + CTXB_fsbase)(%rbx), %rax
	mov	%rax, %rdx
	shr	$32, %rdx
	wrmsr

	/* Process space (CR3 with the PCID): switch only when it changes */
	mov	(TCB_tskctxb + CTXB_ttbr0)(%rbx), %rax
	cmp	%gs:PCPU_cur_ttbr0, %rax
	je	2f
	mov	%rax, %gs:PCPU_cur_ttbr0
	cmpl	$0, knl_pcid_on(%rip)
	jne	1f
	movabs	$0x000FFFFFFFFFF000, %rcx	/* no PCIDs: the PML4 alone, and the TLB emptied */
	and	%rcx, %rax
	mov	%rax, %cr3
	jmp	2f
1:	bts	$63, %rax			/* the space's entries kept under its PCID */
	mov	%rax, %cr3
2:
	/* Its system stack for the next entry from user mode */
	mov	TCB_isstack(%rbx), %rax
	and	$~63, %rax
#if USE_FPU
	testl	$TA_FPU_BIT, TCB_tskatr(%rbx)
	jz	4f
	mov	knl_xsave_size(%rip), %ecx
	sub	%rcx, %rax
4:
#endif
	mov	%rax, %gs:PCPU_kstack
	mov	%rax, %gs:PCPU_tss_rsp0

	mov	(TCB_tskctxb + CTXB_ssp)(%rbx), %rsp	/* restore SP from TCB */
	movl	$0, %gs:PCPU_dispatch_disabled	/* dispatch enable */
	jmp	knl_int_restore			/* return to the task (vector.S) */
	.size	knl_dispatch_entry, . - knl_dispatch_entry

	.section .note.GNU-stack, "", @progbits

#endif /* CPU_CORE_X86_64 */
