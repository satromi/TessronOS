/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crt_cxx.S (x86-64)
 *	Entry of a TessronOS program in C++, or in C with threads
 *	(etc/linker/x64/program_cxx.ld, design 17.19).
 *
 *	The kernel starts the main task at _start on the small stack of the
 *	process (design 9.5). .bss is cleared here, and __ts_start (ts_start.c)
 *	makes the main thread's TLS block and a stack of its own before it
 *	runs the constructors and main. It does not come back.
 */

	.text
	.global	_start
	.type	_start, @function
_start:
	xor	%ebp, %ebp			/* end the frame chain */
	lea	__bss_start(%rip), %rdi
	lea	__bss_end(%rip), %rcx
	sub	%rdi, %rcx
	xor	%eax, %eax
	rep stosb
	and	$-16, %rsp
	call	__ts_start
1:	jmp	1b
	.size	_start, . - _start

	.section .note.GNU-stack, "", @progbits
