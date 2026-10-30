/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_stack.S (x86-64)
 *	void *__ts_call_on_stack( void *sp, void *(*fn)( void * ), void *arg )
 *
 *	Runs fn(arg) on the stack whose top is sp (16 byte aligned) and
 *	comes back to the caller's stack with fn's result. The caller's
 *	stack pointer is kept in rbx, which fn keeps as the calling
 *	convention says. The frame chain on the new stack starts empty.
 */

	.text
	.global	__ts_call_on_stack
	.type	__ts_call_on_stack, @function
__ts_call_on_stack:
	push	%rbp
	mov	%rsp, %rbp
	push	%rbx
	push	%rbx				/* (the stack 16 byte aligned) */
	mov	%rsp, %rbx
	and	$-16, %rdi
	mov	%rdi, %rsp
	mov	%rdx, %rdi
	xor	%ebp, %ebp
	call	*%rsi
	mov	%rbx, %rsp
	pop	%rbx
	pop	%rbx
	pop	%rbp
	ret
	.size	__ts_call_on_stack, . - __ts_call_on_stack

	.section .note.GNU-stack, "", @progbits
