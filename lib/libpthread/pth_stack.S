/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_stack.S
 *	void *__ts_call_on_stack( void *sp, void *(*fn)( void * ), void *arg )
 *
 *	Runs fn(arg) on the stack whose top is sp (16 byte aligned) and
 *	comes back to the caller's stack with fn's result. The caller's
 *	stack pointer is kept in x19, which fn keeps as the calling
 *	convention says. The frame chain on the new stack starts empty.
 */

	.text
	.balign	4
	.global	__ts_call_on_stack
	.type	__ts_call_on_stack, %function
__ts_call_on_stack:
	stp	x29, x30, [sp, #-32]!
	str	x19, [sp, #16]
	mov	x29, sp
	mov	x19, sp
	and	x0, x0, #~15
	mov	sp, x0
	mov	x0, x2
	mov	x29, #0
	blr	x1
	mov	sp, x19
	ldr	x19, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret
	.size	__ts_call_on_stack, . - __ts_call_on_stack
