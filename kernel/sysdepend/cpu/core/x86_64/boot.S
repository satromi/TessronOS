/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	boot.S (x86-64)
 *	Entry of the kernel from the UEFI loader (tools/x64/uefi).
 *
 *	The loader jumps to _start in 64 bit mode with interrupts off, on
 *	its page tables (bootinfo.h), with rdi the physical address of the
 *	T_BOOTINFO. Everything of the firmware's (its GDT, IDT and stack)
 *	is in memory the kernel takes as free, so the first thing is to
 *	have a stack, a GDT and segments of the kernel's own.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#define	_in_asm_source_
#include <config.h>
#include <sys/sysdef.h>
#include "bootinfo.h"

/*
 * The head of the image (T_KHDR): what the loader is to allocate after
 * the image. The page frame database takes 32 bytes for each page of
 * RAM (sysman/pfalloc.h); the Imalloc area follows it.
 */
	.section .khdr, "a"
	.balign	8
	.global	knl_khdr
knl_khdr:
	.quad	KHDR_MAGIC
	.quad	CNF_IMALLOC_SIZE + 0x400000
	.quad	32
	.quad	0

	.text
	.code64

	.global	_start
	.type	_start, @function
_start:
	cli
	cld
	mov	%rdi, %r12			/* the T_BOOTINFO, physically */
	lea	__tmp_stack_start(%rip), %rsp
	xor	%ebp, %ebp

	lgdt	knl_gdt_desc(%rip)
	pushq	$SEL_KCODE
	lea	1f(%rip), %rax
	pushq	%rax
	lretq
1:	mov	$SEL_KDATA, %eax
	mov	%eax, %ds
	mov	%eax, %es
	mov	%eax, %ss
	xor	%eax, %eax
	mov	%eax, %fs
	mov	%eax, %gs

	mov	%r12, %rdi
	call	reset_main			/* no return */
2:	hlt
	jmp	2b
	.size	_start, . - _start

/*
 * The GDT (sysdef.h): the kernel's and the user's segments, then a TSS
 * descriptor of 16 bytes for each processor, which smp.c fills
 */
	.data
	.balign	16
	.global	knl_gdt
knl_gdt:
	.quad	0				/* 0x00 */
	.quad	0x00af9a000000ffff		/* 0x08 kernel code, 64 bit */
	.quad	0x00cf92000000ffff		/* 0x10 kernel data */
	.quad	0x00cffa000000ffff		/* 0x18 user code, 32 bit (SYSRET's base) */
	.quad	0x00cff2000000ffff		/* 0x20 user data */
	.quad	0x00affa000000ffff		/* 0x28 user code, 64 bit */
	.fill	2, 8, 0			/* 0x30 a TSS: each processor has a GDT of its own (smp.c) */
knl_gdt_end:

	.balign	16
	.global	knl_gdt_desc
knl_gdt_desc:
	.word	knl_gdt_end - knl_gdt - 1
	.quad	knl_gdt

/* Where the user section of the image sits, as linked (the linker script) */
	.balign	8
	.global	knl_user_start_va, knl_user_text_end_va, knl_user_end_va, knl_user_lma
knl_user_start_va:	.quad	__user_start
knl_user_text_end_va:	.quad	__user_text_end
knl_user_end_va:	.quad	__user_end
knl_user_lma:		.quad	__user_lma		/* reset_main makes it the real physical address */

	.section .note.GNU-stack, "", @progbits

#endif /* CPU_CORE_X86_64 */
