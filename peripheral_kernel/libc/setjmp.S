/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	setjmp.S
 *	Leaving a deep call in one step (AArch64)
 *
 *	Saves exactly what the procedure call standard says a function must
 *	preserve for its caller: x19-x28, the frame pointer, the link
 *	register, and the stack pointer. Nothing else can be observed to
 *	have changed by the code that jumps back.
 *
 *	The floating point callee-saved registers (d8-d15) are not saved.
 *	The kernel is built with -mgeneral-regs-only, so no code that can
 *	reach here holds a value in them.
 */

	.text
	.global	setjmp
	.type	setjmp, %function
setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x1, sp
	str	x1, [x0, #96]
	mov	w0, #0
	ret
	.size	setjmp, . - setjmp

	.global	longjmp
	.type	longjmp, %function
longjmp:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldr	x2, [x0, #96]
	mov	sp, x2
	/* setjmp answers 0, so a jump asking for 0 has to answer 1 */
	cmp	w1, #0
	csinc	w0, w1, wzr, ne
	ret
	.size	longjmp, . - longjmp
