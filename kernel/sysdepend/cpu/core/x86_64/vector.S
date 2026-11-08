/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vector.S (x86-64)
 *	The entries of the IDT, the interrupt return path and SYSCALL.
 *
 *	Every entry to the kernel makes the same frame on the system stack
 *	(T_EXCFRAME, cpu_task.h): what the processor pushes (ss, rsp,
 *	rflags, cs, rip, and an error code for some exceptions, 0 pushed
 *	for the rest), the vector, then every general register. An entry
 *	from user mode swaps in the kernel's GS base, whose T_PCPU names the
 *	processor; RFLAGS.AC is cleared, so that SMAP holds in the kernel
 *	whatever the code interrupted was doing. All gates are interrupt
 *	gates: interrupts are masked on entry.
 *
 *	Exceptions (vectors 0-31) go to knl_exception_entry (exc_hdl.c) on
 *	the stack they came in on. NMI, the double fault and the machine
 *	check come in on stacks of their own (the TSS's IST), and with the
 *	GS base as it was, which may be the user's: knl_nmi_common sets the
 *	kernel's for the handler from the T_PCPU kept at the top of the
 *	stack, and puts the old one back. The dispatcher's two software
 *	interrupts go to dispatch.S. The rest are interrupts: the C handler
 *	of knl_intvec_tbl runs on the processor's interrupt stack (first
 *	level only) as a task independent part, then knl_return_inthdr
 *	dispatches when needed.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#define	_in_asm_source_
#include <config.h>
#include <sys/sysdef.h>
#include "offset.h"

#define NGPR		15
#define GPR_SIZE	(NGPR * 8)

	.macro	SAVE_GPRS
	sub	$GPR_SIZE, %rsp
	mov	%rax, 0(%rsp)
	mov	%rbx, 8(%rsp)
	mov	%rcx, 16(%rsp)
	mov	%rdx, 24(%rsp)
	mov	%rsi, 32(%rsp)
	mov	%rdi, 40(%rsp)
	mov	%rbp, 48(%rsp)
	mov	%r8, 56(%rsp)
	mov	%r9, 64(%rsp)
	mov	%r10, 72(%rsp)
	mov	%r11, 80(%rsp)
	mov	%r12, 88(%rsp)
	mov	%r13, 96(%rsp)
	mov	%r14, 104(%rsp)
	mov	%r15, 112(%rsp)
	.endm

	.macro	RESTORE_GPRS
	mov	0(%rsp), %rax
	mov	8(%rsp), %rbx
	mov	16(%rsp), %rcx
	mov	24(%rsp), %rdx
	mov	32(%rsp), %rsi
	mov	40(%rsp), %rdi
	mov	48(%rsp), %rbp
	mov	56(%rsp), %r8
	mov	64(%rsp), %r9
	mov	72(%rsp), %r10
	mov	80(%rsp), %r11
	mov	88(%rsp), %r12
	mov	96(%rsp), %r13
	mov	104(%rsp), %r14
	mov	112(%rsp), %r15
	.endm

	/* SMAP: the way to the user pages shut on every entry */
	.macro	CLAC_IF_SMAP
	cmpl	$0, knl_pan_on(%rip)
	je	91f
	clac
91:
	.endm

/* ------------------------------------------------------------------------ */
/*
 * The 256 entries, 16 bytes apart; knl_int_stubs[] has their addresses
 * for the IDT (interrupt.c)
 */
	.macro	STUB vec
	.balign	16
knl_stub_\vec:
	.if (\vec == 8) || (\vec == 10) || (\vec == 11) || (\vec == 12) || (\vec == 13) || (\vec == 14) || (\vec == 17) || (\vec == 21) || (\vec == 29) || (\vec == 30)
	.else
	pushq	$0				/* no error code: 0 in its place */
	.endif
	pushq	$\vec
	.if (\vec == 2) || (\vec == 8) || (\vec == 18)
	jmp	knl_nmi_common
	.else
	jmp	knl_int_common
	.endif
	.endm

	.altmacro
	.text
	.code64
	.balign	16
	.global	knl_int_stub_base
knl_int_stub_base:
	.set	v, 0
	.rept	256
	STUB	%v
	.set	v, v + 1
	.endr

	.macro	STUB_ADDR vec
	.quad	knl_stub_\vec
	.endm

	.section .rodata
	.balign	8
	.global	knl_int_stubs
knl_int_stubs:
	.set	v, 0
	.rept	256
	STUB_ADDR %v
	.set	v, v + 1
	.endr
	.noaltmacro

/* ------------------------------------------------------------------------ */
	.text

	.global	knl_int_common
	.type	knl_int_common, @function
knl_int_common:
	SAVE_GPRS
	cld
	testb	$3, FR_cs(%rsp)
	jz	1f
	swapgs					/* from user mode: the kernel's GS base */
1:	CLAC_IF_SMAP
	mov	FR_vec(%rsp), %rax
	cmp	$32, %rax
	jb	l_exception
	cmp	$SVC_DISPATCH, %rax
	je	knl_dispatch_entry
	cmp	$SVC_FORCE_DISPATCH, %rax
	je	knl_dispatch_to_schedtsk
	cmp	$INTVEC_SPURIOUS, %rax
	je	knl_int_restore			/* spurious: no EOI, nothing to do */

	/* an interrupt: the task independent part */
	incl	%gs:PCPU_taskindp
	cmpl	$1, %gs:PCPU_taskindp
	jne	2f				/* nested: stay on the interrupt stack */
	mov	%rsp, %rbx
	mov	%gs:PCPU_irq_stack, %rsp
	push	%rbx				/* the task's stack pointer (the frame) */
	push	%rbx				/* (twice: the stack stays 16 byte aligned) */
2:	mov	%rax, %rdi			/* intno */
	xor	%esi, %esi			/* (no acknowledge value on this architecture) */
	mov	knl_intvec_tbl(,%rax,8), %rcx
	test	%rcx, %rcx
	jz	3f
	call	*%rcx
	jmp	knl_return_inthdr
3:	call	Default_Handler			/* returns only for the APIC's error interrupt */
	jmp	knl_return_inthdr

l_exception:
	mov	%rsp, %rdi			/* the frame */
	call	knl_exception_entry		/* returns when the fault was put right */
	jmp	knl_int_restore
	.size	knl_int_common, . - knl_int_common

/*
 * knl_return_inthdr
 *	Leave the task independent part, dispatch if needed, return to the
 *	interrupted context.
 */
	.global	knl_return_inthdr
	.type	knl_return_inthdr, @function
knl_return_inthdr:
	cli
	decl	%gs:PCPU_taskindp
	jnz	knl_int_restore			/* nested interrupt: just return */
	pop	%rbx
	pop	%rbx
	mov	%rbx, %rsp			/* the task's stack again */
	cmpl	$0, %gs:PCPU_dispatch_disabled
	jnz	knl_int_restore
	mov	%gs:PCPU_ctxtsk, %rax
	cmp	%gs:PCPU_schedtsk, %rax
	jne	knl_dispatch_entry
	/* fall through */
	.size	knl_return_inthdr, . - knl_return_inthdr

/*
 * knl_int_restore
 *	Return to the context of the frame at rsp
 */
	.global	knl_int_restore
	.type	knl_int_restore, @function
knl_int_restore:
	testb	$3, FR_cs(%rsp)
	jz	1f
	swapgs					/* back to user mode: the user's GS base */
1:	RESTORE_GPRS
	add	$(GPR_SIZE + 16), %rsp		/* the registers, the vector and the error code */
	iretq
	.size	knl_int_restore, . - knl_int_restore

/*
 * NMI, double fault, machine check: on an IST stack, whose top holds
 * this processor's T_PCPU. The GS base may be anything here, the user's
 * included: the kernel's is set for the handler and the old one put
 * back, so that SWAPGS is never needed.
 */
	.global	knl_nmi_common
	.type	knl_nmi_common, @function
knl_nmi_common:
	SAVE_GPRS
	cld
	mov	$MSR_GS_BASE, %ecx
	rdmsr
	shl	$32, %rdx
	or	%rax, %rdx
	mov	%rdx, %r12			/* the GS base as it was */
	mov	SSF_SIZE(%rsp), %rax		/* the T_PCPU at the top of the IST stack */
	mov	%rax, %rdx
	shr	$32, %rdx
	wrmsr
	mov	%rsp, %rdi
	call	knl_nmi_entry			/* exc_hdl.c */
	mov	$MSR_GS_BASE, %ecx
	mov	%r12, %rax
	mov	%r12, %rdx
	shr	$32, %rdx
	wrmsr
	RESTORE_GPRS
	add	$(GPR_SIZE + 16), %rsp
	iretq
	.size	knl_nmi_common, . - knl_nmi_common

/* ------------------------------------------------------------------------ */
/*
 * SYSCALL from user mode (MSR_LSTAR). rcx holds the return address and
 * r11 RFLAGS; interrupts, AC, DF and TF were cleared by MSR_SFMASK. The
 * function code is in rax, the arguments in rdi, rsi, rdx, r10, r8, r9;
 * the 7th and 8th are on the user's stack, which knl_syscall_c reads.
 * The frame is the one an interrupt from user mode makes. The way back
 * is SYSRET when the frame still returns to 64 bit user code at a
 * canonical address, IRET otherwise (a frame the kernel turned
 * elsewhere, or an address SYSRET would fault on in the kernel's mode).
 */
	.global	knl_syscall_entry
	.type	knl_syscall_entry, @function
knl_syscall_entry:
	swapgs
	mov	%rsp, %gs:PCPU_user_rsp
	mov	%gs:PCPU_kstack, %rsp
	pushq	$SEL_UDATA			/* ss */
	pushq	%gs:PCPU_user_rsp		/* rsp */
	pushq	%r11				/* rflags */
	pushq	$SEL_UCODE			/* cs */
	pushq	%rcx				/* rip */
	pushq	$0				/* err: the 8th argument */
	pushq	$0				/* vec: the 7th argument */
	SAVE_GPRS
	mov	%rsp, %rdi
	call	knl_syscall_c			/* svc_x86_64.c: the value goes into the frame's rax */
	cli
	cmpq	$SEL_UCODE, FR_cs(%rsp)
	jne	knl_int_restore
	mov	FR_rip(%rsp), %rcx
	mov	%rcx, %r11
	sar	$47, %r11
	jnz	knl_int_restore			/* not a canonical address of the user half */
	mov	FR_rflags(%rsp), %r11
	test	$(RFL_TF | RFL_NT), %r11
	jnz	knl_int_restore
	mov	0(%rsp), %rax
	mov	8(%rsp), %rbx
	/*
	 * SS is null when this task was switched to from one that came in by
	 * an interrupt. AMD's SYSRET loads the selector but not the descriptor
	 * behind it, and the user would run with an unusable stack segment.
	 */
	mov	$GDT_KDATA, %edx
	mov	%edx, %ss
	mov	24(%rsp), %rdx
	mov	32(%rsp), %rsi
	mov	40(%rsp), %rdi
	mov	48(%rsp), %rbp
	mov	56(%rsp), %r8
	mov	64(%rsp), %r9
	mov	72(%rsp), %r10
	mov	88(%rsp), %r12
	mov	96(%rsp), %r13
	mov	104(%rsp), %r14
	mov	112(%rsp), %r15
	mov	FR_rsp(%rsp), %rsp
	swapgs
	sysretq
	.size	knl_syscall_entry, . - knl_syscall_entry

	.section .note.GNU-stack, "", @progbits

#endif /* CPU_CORE_X86_64 */
