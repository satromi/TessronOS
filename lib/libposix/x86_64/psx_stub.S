/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_stub.S (x86-64)
 *	The system calls of a POSIX program (design 5.8)
 *
 *	Every call is SYSCALL with the function code in rax and the first
 *	six arguments where the C calling convention leaves them, but the
 *	fourth, which goes from rcx to r10 (SYSCALL keeps the return
 *	address in rcx). A function of eight arguments has the 7th and 8th
 *	on the stack, where its caller put them; the kernel reads them there
 *	(kernel/sysdepend/cpu/core/x86_64/syscall.c). The result comes back
 *	in rax. The list of calls is lib/libposix/psx_stub.S's.
 */

#include <ts/svc.h>

	.text
	.balign	16

	.macro	SVCSTUB name, fncd
	.global	\name
	.type	\name, @function
\name:
	mov	$\fncd, %eax
	mov	%rcx, %r10
	syscall
	ret
	.size	\name, . - \name
	.endm

#define SVCSTUB_ARCH
#include "../psx_stub.S"

	.section .note.GNU-stack, "", @progbits
