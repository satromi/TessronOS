/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tramp.S (x86-64)
 *	Where a secondary processor starts: copied by smp.c to the page
 *	below 1MB the loader set aside, which the start-up IPI names. The
 *	processor arrives in real mode with CS the page's paragraph and IP 0;
 *	it loads the small GDT of the page, goes to protected mode, turns on
 *	PAE, the PML4 of the page (the identity map of the first gigabyte
 *	and the kernel's half), long mode and paging, and jumps to
 *	knl_secondary_main with the stack and the slot smp.c wrote here.
 *	Everything is reached relative to the page, so the code runs at
 *	whatever address it was copied to; the few absolute addresses (the
 *	GDT's, the far pointers') are written in by smp.c.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#define	_in_asm_source_
#include <config.h>
#include <sys/sysdef.h>

#define OFF(sym)	( sym - knl_tramp_start )

	.section .rodata
	.balign	16
	.global	knl_tramp_start, knl_tramp_end
	.global	knl_tramp_gdt, knl_tramp_gdt_desc, knl_tramp_far32, knl_tramp_far64
	.global	knl_tramp_cr3, knl_tramp_stack, knl_tramp_entry, knl_tramp_arg
	.global	knl_tramp_pm32, knl_tramp_lm64

	.code16
knl_tramp_start:
	cli
	cld
	mov	%cs, %ax
	mov	%ax, %ds
	movzwl	%ax, %ebx
	shl	$4, %ebx			/* ebx = where the page is, kept to long mode */
	lgdtl	OFF(knl_tramp_gdt_desc)
	mov	%cr0, %eax
	and	$~(CR0_CD | CR0_NW), %eax	/* INIT leaves the caches off */
	or	$CR0_PE, %eax
	mov	%eax, %cr0
	ljmpl	*OFF(knl_tramp_far32)

	.code32
knl_tramp_pm32:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	%cr4, %eax
	or	$(CR4_PAE | CR4_PGE), %eax
	mov	%eax, %cr4
	mov	OFF(knl_tramp_cr3)(%ebx), %eax
	mov	%eax, %cr3
	mov	$MSR_EFER, %ecx
	rdmsr
	or	$(EFER_LME | EFER_NXE | EFER_SCE), %eax
	wrmsr
	mov	%cr0, %eax
	or	$(CR0_PG | CR0_WP | CR0_NE), %eax
	mov	%eax, %cr0
	ljmpl	*OFF(knl_tramp_far64)(%ebx)

	.code64
knl_tramp_lm64:
	mov	knl_tramp_stack(%rip), %rsp
	mov	knl_tramp_arg(%rip), %rdi
	mov	knl_tramp_entry(%rip), %rax
	xor	%ebp, %ebp
	jmp	*%rax

	.balign	8
knl_tramp_gdt:
	.quad	0
	.quad	0x00cf9a000000ffff		/* 0x08 code, 32 bit */
	.quad	0x00cf92000000ffff		/* 0x10 data */
	.quad	0x00af9a000000ffff		/* 0x18 code, 64 bit */
knl_tramp_gdt_desc:
	.word	4 * 8 - 1
	.long	0				/* the GDT's address (smp.c) */
	.balign	4
knl_tramp_far32:
	.long	0				/* knl_tramp_pm32's address (smp.c) */
	.word	0x08
	.balign	4
knl_tramp_far64:
	.long	0				/* knl_tramp_lm64's address (smp.c) */
	.word	0x18
	.balign	8
knl_tramp_cr3:
	.quad	0				/* the PML4 after this page */
knl_tramp_stack:
	.quad	0				/* the processor's temporary stack */
knl_tramp_entry:
	.quad	0				/* knl_secondary_main */
knl_tramp_arg:
	.quad	0				/* its slot */
knl_tramp_end:

	.section .note.GNU-stack, "", @progbits

#endif /* CPU_CORE_X86_64 */
