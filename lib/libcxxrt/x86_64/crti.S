/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	crti.S (x86-64)
 *	The beginning of _init and _fini, which crtn.S ends: the C library
 *	calls them as it starts and ends (newlib's __libc_init_array), and
 *	what other objects put in .init and .fini runs between. The
 *	toolchain for x86_64-elf has none of its own.
 */

	.section .init, "ax", @progbits
	.global	_init
	.type	_init, @function
_init:
	push	%rbp
	mov	%rsp, %rbp

	.section .fini, "ax", @progbits
	.global	_fini
	.type	_fini, @function
_fini:
	push	%rbp
	mov	%rsp, %rbp

	.section .note.GNU-stack, "", @progbits
