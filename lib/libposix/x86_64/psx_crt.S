/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_crt.S (x86-64)
 *	Entry of a POSIX program (lib/libposix, design 5.8.3)
 *
 *	_start clears .bss and hands over to __psx_start (psx_start.c),
 *	which never returns. __psx_on_stack(sp, fn, arg) calls fn(arg) on
 *	another stack: the program's own, larger than the one the kernel
 *	gives the main task.
 */

	.text
	.global	_start
	.type	_start, @function
_start:
	xor	%ebp, %ebp
	lea	__bss_start(%rip), %rdi
	lea	__bss_end(%rip), %rcx
	sub	%rdi, %rcx
	xor	%eax, %eax
	rep stosb
	and	$-16, %rsp
	call	__psx_start
1:	jmp	1b
	.size	_start, . - _start

/*
 * The entry a caught signal is handed to (include/ts/psx.h): rdi the
 * picture of where the task was, rsi the signal, the stack just below
 * the picture as a function's is when it has just been called. The
 * handler is a C function and gives back rbx, rbp and r12-r15 as it
 * found them, which are those of the moment of the signal: ts_sig_ret
 * goes back with them, and the picture's other registers. The kernel
 * takes an entry only at an address of a multiple of 4 (ts_sig_set).
 */
	.balign	16
	.global	__psx_sigentry
	.type	__psx_sigentry, @function
__psx_sigentry:
	push	%rdi				/* the picture (the stack 16 byte aligned again) */
	call	__psx_sighandle
	pop	%rdi
	jmp	ts_sig_ret
	.size	__psx_sigentry, . - __psx_sigentry

	.global	__psx_on_stack
	.type	__psx_on_stack, @function
__psx_on_stack:
	mov	%rdi, %rsp
	and	$-16, %rsp
	xor	%ebp, %ebp
	mov	%rdx, %rdi
	call	*%rsi
2:	jmp	2b
	.size	__psx_on_stack, . - __psx_on_stack

	.section .note.GNU-stack, "", @progbits
