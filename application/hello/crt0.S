/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crt0.S
 *	Entry of a TessronOS program (design 9.6).
 *
 *	The kernel starts the main task at _start with an empty stack in
 *	the process's own space. main() runs and its return value ends the
 *	process, so _start never returns.
 */

	.text
	.balign	4
	.global	_start
	.type	_start, %function
_start:
	mov	x29, #0			/* end the frame chain */
	mov	x30, #0

	/* clear .bss */
	adrp	x0, __bss_start
	add	x0, x0, :lo12:__bss_start
	adrp	x1, __bss_end
	add	x1, x1, :lo12:__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:
	bl	main
	bl	ts_ext_prc		/* x0 = exit code, no return */
3:	b	3b
	.size	_start, . - _start
