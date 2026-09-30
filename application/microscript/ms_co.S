/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_co.S
 *	Threads of マイクロスクリプト that take turns within the one task
 *
 *	A switch keeps what a call must keep (x19-x30, sp, d8-d15) in the
 *	context being left and takes it from the one being entered. A new
 *	context starts in ms_co_entry, which calls its function with its
 *	argument and, when that returns, calls ms_co_done, which does not.
 *
 *	The context: x19..x30 at 0, sp at 96, d8..d15 at 104.
 */

	.text
	.balign	4

	/* void ms_co_switch( MSCTX *from, MSCTX *to ) */
	.global	ms_co_switch
	.type	ms_co_switch, %function
ms_co_switch:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x2, sp
	str	x2, [x0, #96]
	stp	d8, d9, [x0, #104]
	stp	d10, d11, [x0, #120]
	stp	d12, d13, [x0, #136]
	stp	d14, d15, [x0, #152]

	ldp	x19, x20, [x1, #0]
	ldp	x21, x22, [x1, #16]
	ldp	x23, x24, [x1, #32]
	ldp	x25, x26, [x1, #48]
	ldp	x27, x28, [x1, #64]
	ldp	x29, x30, [x1, #80]
	ldr	x2, [x1, #96]
	mov	sp, x2
	ldp	d8, d9, [x1, #104]
	ldp	d10, d11, [x1, #120]
	ldp	d12, d13, [x1, #136]
	ldp	d14, d15, [x1, #152]
	ret
	.size	ms_co_switch, . - ms_co_switch

	/* x19 = the function, x20 = its argument */
	.global	ms_co_entry
	.type	ms_co_entry, %function
ms_co_entry:
	mov	x29, #0
	mov	x0, x20
	blr	x19
	bl	ms_co_done
1:	b	1b
	.size	ms_co_entry, . - ms_co_entry
