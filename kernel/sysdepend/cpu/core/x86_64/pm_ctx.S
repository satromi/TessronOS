/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pm_ctx.S (x86-64)
 *	Where a processor stood as the machine went to sleep: the registers a
 *	C call keeps, with the stack and the return address. The other
 *	registers, the control registers and the tables are set up again by
 *	kernel/sysdepend/x64/pm.c. knl_pm_ctx_save answers 0; when
 *	knl_pm_ctx_load comes back to the same place it answers 1.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

	.text
	.code64

	.global	knl_pm_ctx_save
	.type	knl_pm_ctx_save, @function
knl_pm_ctx_save:
	mov	%rbx, 0(%rdi)
	mov	%rbp, 8(%rdi)
	mov	%r12, 16(%rdi)
	mov	%r13, 24(%rdi)
	mov	%r14, 32(%rdi)
	mov	%r15, 40(%rdi)
	mov	%rsp, 48(%rdi)
	mov	(%rsp), %rax
	mov	%rax, 56(%rdi)
	xor	%eax, %eax
	ret
	.size	knl_pm_ctx_save, . - knl_pm_ctx_save

	.global	knl_pm_ctx_load
	.type	knl_pm_ctx_load, @function
knl_pm_ctx_load:
	mov	0(%rdi), %rbx
	mov	8(%rdi), %rbp
	mov	16(%rdi), %r12
	mov	24(%rdi), %r13
	mov	32(%rdi), %r14
	mov	40(%rdi), %r15
	mov	48(%rdi), %rsp
	mov	56(%rdi), %rax
	mov	%rax, (%rsp)
	mov	$1, %eax
	ret
	.size	knl_pm_ctx_load, . - knl_pm_ctx_load

	/*
	 * After the kernel's GDT is loaded on a processor that came through
	 * the trampoline: CS and the data segments still hold the trampoline's
	 * selectors, and a frame pushed with them could not be returned from.
	 * FS and GS stay as they are, their bases being in the MSRs.
	 */
	.global	knl_pm_segs_load
	.type	knl_pm_segs_load, @function
knl_pm_segs_load:
	mov	$0x10, %eax
	mov	%eax, %ds
	mov	%eax, %es
	mov	%eax, %ss
	pop	%rax
	pushq	$0x08
	push	%rax
	lretq
	.size	knl_pm_segs_load, . - knl_pm_segs_load

	.section .note.GNU-stack, "", @progbits

#endif /* CPU_CORE_X86_64 */
