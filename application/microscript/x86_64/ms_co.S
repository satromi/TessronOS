/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_co.S (x86-64)
 *	Threads of マイクロスクリプト that take turns within the one task
 *
 *	A switch keeps what a call must keep (rbx, rbp, r12-r15, rsp, and
 *	the x87 and SSE control words) in the context being left and takes
 *	it from the one being entered, then goes on where the entered one
 *	was. A new context starts in ms_co_entry, which calls its function
 *	with its argument and, when that returns, calls ms_co_done, which
 *	does not.
 *
 *	The context (MSCTX, ms_run.h, laid out for the other architecture):
 *	x[0] rbx, x[1] r12, x[2] r13, x[3] r14, x[4] r15, x[5] rbp, x[11]
 *	where it goes on, sp at 96, MXCSR and the x87 control word at 104
 *	(0 in a context not yet entered: they are left as they are).
 */

	.text

	/* void ms_co_switch( MSCTX *from, MSCTX *to ) */
	.global	ms_co_switch
	.type	ms_co_switch, @function
ms_co_switch:
	mov	%rbx, 0(%rdi)
	mov	%r12, 8(%rdi)
	mov	%r13, 16(%rdi)
	mov	%r14, 24(%rdi)
	mov	%r15, 32(%rdi)
	mov	%rbp, 40(%rdi)
	mov	(%rsp), %rax			/* the return address: where it goes on */
	mov	%rax, 88(%rdi)
	lea	8(%rsp), %rax
	mov	%rax, 96(%rdi)
	stmxcsr	104(%rdi)
	fnstcw	108(%rdi)

	mov	0(%rsi), %rbx
	mov	8(%rsi), %r12
	mov	16(%rsi), %r13
	mov	24(%rsi), %r14
	mov	32(%rsi), %r15
	mov	40(%rsi), %rbp
	mov	96(%rsi), %rsp
	cmpl	$0, 104(%rsi)
	je	1f
	ldmxcsr	104(%rsi)
	fldcw	108(%rsi)
1:	jmp	*88(%rsi)
	.size	ms_co_switch, . - ms_co_switch

	/* rbx = the function, r12 = its argument; the stack 16 byte aligned */
	.global	ms_co_entry
	.type	ms_co_entry, @function
ms_co_entry:
	xor	%ebp, %ebp
	mov	%r12, %rdi
	call	*%rbx
	call	ms_co_done
2:	jmp	2b
	.size	ms_co_entry, . - ms_co_entry

	.section .note.GNU-stack, "", @progbits
