/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysnames.S (x86-64)
 *	The names newlib calls the system by on x86-64, and those of
 *	AArch64. The x86_64-elf newlib reaches the system by the plain names
 *	(write, sbrk ...), the aarch64 one by the names with an underscore
 *	(_write, _sbrk ...), which is what the C library shims of the
 *	programs define (lib/libui/ui_libc.c, lib/libcxxrt/ts_syscalls.c
 *	...). Each plain name here goes on to the underscored one. Both are
 *	weak: a program that defines the plain name itself keeps its own, and
 *	one that defines neither does not call it.
 */

	.text

	.macro	ALIAS name
	.weak	\name
	.weak	_\name
	.type	\name, @function
\name:
	jmp	_\name
	.size	\name, . - \name
	.endm

	ALIAS	close
	ALIAS	execve
	ALIAS	fcntl
	ALIAS	fork
	ALIAS	fstat
	ALIAS	getpid
	ALIAS	gettimeofday
	ALIAS	isatty
	ALIAS	kill
	ALIAS	link
	ALIAS	lseek
	ALIAS	open
	ALIAS	read
	ALIAS	rename
	ALIAS	sbrk
	ALIAS	stat
	ALIAS	times
	ALIAS	unlink
	ALIAS	wait
	ALIAS	write

	.section .note.GNU-stack, "", @progbits
