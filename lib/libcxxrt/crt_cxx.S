/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crt_cxx.S
 *	Entry of a TessronOS program in C++, or in C with threads
 *	(etc/linker/user/program_cxx.ld, design 17.19).
 *
 *	The kernel starts the main task at _start on the small stack of the
 *	process (design 9.5). .bss is cleared here, and __ts_start (ts_start.c)
 *	makes the main thread's TLS block and a stack of its own before it
 *	runs the constructors and main. It does not come back.
 */

	.text
	.balign	4
	.global	_start
	.type	_start, %function
_start:
	mov	x29, #0			/* end the frame chain */
	mov	x30, #0

	adrp	x0, __bss_start
	add	x0, x0, :lo12:__bss_start
	adrp	x1, __bss_end
	add	x1, x1, :lo12:__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:
	bl	__ts_start
3:	b	3b
	.size	_start, . - _start
