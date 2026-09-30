/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	boot.S (ARMv8-A / AArch64)
 *	Entry point from the firmware (Raspberry Pi 5: EL2, QEMU virt: EL1 or EL2).
 *
 *	x0 = physical address of the DTB, x1..x3 = 0.
 *	Only the primary core (Aff0 == 0) continues; the others park in WFE
 *	until they are started through PSCI CPU_ON.
 *
 *	This section runs at its physical load address with the MMU off, so
 *	it is position independent (adr only; VA symbols are taken from
 *	literals and converted with KVA_BASE). It builds the initial page
 *	tables (design 6.2/6.3), turns the MMU on and jumps to the kernel at
 *	its linked virtual address (KVA_BASE + PA).
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#define _in_asm_source_
#include <config.h>
#include <sys/sysdef.h>

	.section .boot, "ax"
	.balign	4

	.global	_start
	.type	_start, %function
_start:
	mov	x19, x0			/* keep the DTB address */
	msr	daifset, #(DAIF_D|DAIF_A|DAIF_I|DAIF_F)

	/* Secondary cores park here until PSCI CPU_ON brings them up */
	mrs	x1, mpidr_el1
	and	x1, x1, #0xffffff		/* Aff0..Aff2 */
	cbz	x1, primary_boot
secondary_park:
	wfe
	b	secondary_park

primary_boot:
	/*
	 * The Raspberry Pi firmware may ignore kernel_address for an image
	 * without the arm64 header and load it at 0x80000 instead. Everything
	 * after this point assumes the linked physical address, so copy the
	 * image there first (backwards when the destination is above the
	 * source, as the ranges overlap) and continue in the copy. The MMU and
	 * the data cache are off, so only the instruction cache is invalidated.
	 * x24 = the address the image was loaded at (0 if not moved).
	 */
	mov	x24, #0
	adr	x1, _start
	ldr	x2, =__boot_start
	cmp	x1, x2
	b.eq	4f
	mov	x24, x1
	ldr	x3, =__image_end_pa
	sub	x3, x3, x2
	add	x3, x3, #15
	bic	x3, x3, #15			/* x3 = image size, 16-byte units */
	cmp	x2, x1
	b.lo	6f
	add	x7, x1, x3			/* dest above src: from the end */
	add	x8, x2, x3
5:	ldp	x4, x5, [x7, #-16]!
	stp	x4, x5, [x8, #-16]!
	cmp	x7, x1
	b.hi	5b
	b	7f
6:	mov	x7, x1				/* dest below src: from the start */
	mov	x8, x2
	add	x9, x1, x3
8:	ldp	x4, x5, [x7], #16
	stp	x4, x5, [x8], #16
	cmp	x7, x9
	b.lo	8b
7:	dsb	sy
	ic	iallu
	dsb	sy
	isb
	adr	x4, 4f
	sub	x4, x4, x1
	add	x4, x4, x2
	br	x4
4:
	/*
	 * x0 carries the DTB address only when the loader follows the Linux
	 * boot protocol. QEMU passes 0 for an ELF image but places the DTB at
	 * the start of RAM, so fall back to looking for the FDT magic there.
	 */
	cbnz	x19, 3f
	ldr	x1, =RAM_START
	ldr	w2, [x1]
	ldr	w3, =0xedfe0dd0		/* 0xd00dfeed read as little endian */
	cmp	w2, w3
	b.ne	3f
	mov	x19, x1
3:
	/* Banner: "TessronOS boot: ELn  DTB=..." */
	adr	x0, msg_banner
	bl	boot_puts
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	mov	x20, x0				/* x20 = boot EL */
	add	w0, w0, #'0'
	bl	boot_putc
	adr	x0, msg_dtb
	bl	boot_puts
	mov	x0, x19
	bl	boot_puthex
	adr	x0, msg_nl
	bl	boot_puts
	cbz	x24, 9f
	adr	x0, msg_moved
	bl	boot_puts
	mov	x0, x24
	bl	boot_puthex
	adr	x0, msg_nl
	bl	boot_puts
9:

	/*
	 * Drop from EL2 to EL1 if the firmware entered at EL2 (Raspberry Pi 5,
	 * QEMU with virtualization=on). EL1 gets: AArch64, access to the
	 * physical timer, no FP trap at EL2, virtual CPU ID = real CPU ID.
	 */
	cmp	x20, #2
	b.ne	el1_start
	ldr	x0, =SCTLR_EL1_RES1
	msr	sctlr_el1, x0			/* EL1: MMU and caches off */
	mov	x0, #0x80000000			/* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3			/* EL1PCEN | EL1PCTEN */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mov	x0, #0x33ff			/* CPTR_EL2 RES1, no traps */
	msr	cptr_el2, x0
	msr	hstr_el2, xzr
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0
	mov	x0, #(PSR_M_EL1h | PSR_DAIF)
	msr	spsr_el2, x0
	adr	x0, el1_start
	msr	elr_el2, x0
	eret

el1_start:
	/*
	 * Initial page tables (in .boot.pt at the end of the image, physical,
	 * zeroed here). x21 = KVA_BASE for VA -> PA conversion of literals.
	 *   pt_l0_k    TTBR1 L0: [0] -> pt_l1_ram, [256] -> pt_l1_dev
	 *   pt_l1_ram  linear map (VA = KVA_BASE + PA), 1GB blocks; the block that
	 *              holds the kernel points to pt_l2_kern instead. mmu.c adds
	 *              the rest of RAM after the DTB is parsed
	 *   pt_l2_kern 2MB blocks of the kernel's 1GB: image text RO+X, the rest
	 *              RW+XN (W^X, design 5.11)
	 *   pt_l1_dev  device window (VA = DEV_VA_BASE + PA), 512 x 1GB blocks
	 *   pt_l0_id   TTBR0 L0: [0] -> pt_l1_id (identity map of the kernel block,
	 *              used only to survive the MMU switch)
	 */
	ldr	x21, =KVA_BASE
	ldr	x0, =__boot_pt_start
	sub	x0, x0, x21
	ldr	x1, =__boot_pt_end
	sub	x1, x1, x21
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:
	ldr	x4, =pt_l0_k
	sub	x4, x4, x21
	ldr	x5, =pt_l1_ram
	sub	x5, x5, x21
	ldr	x6, =pt_l1_dev
	sub	x6, x6, x21
	ldr	x7, =pt_l0_id
	sub	x7, x7, x21
	ldr	x8, =pt_l1_id
	sub	x8, x8, x21
	ldr	x22, =pt_l2_kern
	sub	x22, x22, x21

	/* pt_l0_k[0] = pt_l1_ram, pt_l0_k[256] = pt_l1_dev */
	orr	x0, x5, #PTE_TABLE_DESC
	str	x0, [x4, #0]
	orr	x0, x6, #PTE_TABLE_DESC
	str	x0, [x4, #(256 * 8)]

	/* pt_l1_dev[i] = (i << 30) | PTE_BLOCK_DEVICE, i = 0..511 */
	ldr	x9, =PTE_BLOCK_DEVICE
	mov	x1, #0
3:	lsl	x0, x1, #L1_SHIFT
	orr	x0, x0, x9
	str	x0, [x6, x1, lsl #3]
	add	x1, x1, #1
	cmp	x1, #PT_ENTRIES
	b.lo	3b

	/*
	 * pt_l2_kern: the kernel's 1GB block in 2MB pieces.
	 *   [LOAD_PA, __data_start) -> RO+X   (boot, text, rodata)
	 *   everything else          -> RW+XN
	 */
	adr	x23, _start			/* x23 = LOAD_PA */
	lsr	x10, x23, #L1_SHIFT
	lsl	x10, x10, #L1_SHIFT		/* x10 = base PA of the kernel's 1GB block */
	ldr	x11, =__data_start
	sub	x11, x11, x21			/* x11 = PA where RW data starts */
	ldr	x12, =PTE_BLOCK_NORMAL_XN
	ldr	x13, =PTE_BLOCK_TEXT
	mov	x1, #0
4:	lsl	x0, x1, #L2_SHIFT
	add	x0, x0, x10			/* PA of this 2MB block */
	cmp	x0, x23
	b.lo	5f
	cmp	x0, x11
	b.hs	5f
	orr	x2, x0, x13			/* text block */
	b	6f
5:	orr	x2, x0, x12			/* data block */
6:	str	x2, [x22, x1, lsl #3]
	add	x1, x1, #1
	cmp	x1, #PT_ENTRIES
	b.lo	4b

	/* pt_l1_ram[k] -> pt_l2_kern (table); pt_l1_id[k] = 1GB block RO+X (identity; with WXN a
	   writable block would be non-executable and the code right after the MMU switch runs here) */
	lsr	x1, x23, #L1_SHIFT		/* k: kernel block index (< 512) */
	orr	x0, x22, #PTE_TABLE_DESC
	str	x0, [x5, x1, lsl #3]
	ldr	x9, =PTE_BLOCK_TEXT
	orr	x0, x10, x9
	str	x0, [x8, x1, lsl #3]

	/* DTB block in the linear map (1GB block, RW+XN) if it is outside the kernel block */
	cbz	x19, 7f
	lsr	x1, x19, #L1_SHIFT
	ldr	x0, [x5, x1, lsl #3]
	cbnz	x0, 7f
	lsl	x0, x1, #L1_SHIFT
	orr	x0, x0, x12
	str	x0, [x5, x1, lsl #3]
7:
	/* pt_l0_id[0] = pt_l1_id */
	orr	x0, x8, #PTE_TABLE_DESC
	str	x0, [x7, #0]

	/* Translation regime */
	ldr	x0, =MAIR_EL1_VALUE
	msr	mair_el1, x0
	ldr	x0, =TCR_EL1_VALUE
	msr	tcr_el1, x0
	msr	ttbr0_el1, x7
	msr	ttbr1_el1, x4
	dsb	ish
	isb
	tlbi	vmalle1
	dsb	ish
	ic	iallu
	dsb	ish
	isb

	/* MMU, caches, SP alignment check, WXN on */
	ldr	x0, =(SCTLR_EL1_RES1 | SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_SA | SCTLR_WXN)
	msr	sctlr_el1, x0
	isb

	/* Continue at the linked (virtual) address */
	ldr	x0, =knl_start_va
	br	x0
	.size	_start, . - _start

/*
 * knl_secondary_entry(x0 = processor slot index)
 *	Entry of a secondary core started by PSCI CPU_ON: physical address,
 *	MMU off, EL2 or EL1 (design 8.9). Joins the translation regime of
 *	the primary (shared TTBR1, identity map for the switch) and continues
 *	at knl_secondary_start_va.
 */
	.global	knl_secondary_entry
	.type	knl_secondary_entry, %function
knl_secondary_entry:
	msr	daifset, #(DAIF_D|DAIF_A|DAIF_I|DAIF_F)
	mov	x19, x0
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #2
	b.ne	sec_el1
	ldr	x0, =SCTLR_EL1_RES1
	msr	sctlr_el1, x0
	mov	x0, #0x80000000			/* HCR_EL2.RW */
	msr	hcr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mov	x0, #0x33ff
	msr	cptr_el2, x0
	msr	hstr_el2, xzr
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0
	mov	x0, #(PSR_M_EL1h | PSR_DAIF)
	msr	spsr_el2, x0
	adr	x0, sec_el1
	msr	elr_el2, x0
	eret
sec_el1:
	ldr	x21, =KVA_BASE
	ldr	x4, =pt_l0_k
	sub	x4, x4, x21
	ldr	x7, =pt_l0_id
	sub	x7, x7, x21
	ldr	x0, =MAIR_EL1_VALUE
	msr	mair_el1, x0
	ldr	x0, =TCR_EL1_VALUE
	msr	tcr_el1, x0
	msr	ttbr0_el1, x7
	msr	ttbr1_el1, x4
	dsb	ish
	isb
	tlbi	vmalle1
	dsb	ish
	ic	iallu
	dsb	ish
	isb
	ldr	x0, =(SCTLR_EL1_RES1 | SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_SA | SCTLR_WXN)
	msr	sctlr_el1, x0
	isb
	ldr	x0, =knl_secondary_start_va
	br	x0
	.size	knl_secondary_entry, . - knl_secondary_entry

/*
 * boot_putc(w0 = character)
 *	Polled output to the debug PL011 (physical address, MMU off).
 */
	.global	boot_putc
	.type	boot_putc, %function
boot_putc:
	ldr	x1, =DBG_UART_PA
1:	ldr	w2, [x1, #PL011_FR]
	tbnz	w2, #5, 1b		/* TXFF: transmit FIFO full */
	str	w0, [x1, #PL011_DR]
	ret
	.size	boot_putc, . - boot_putc

/*
 * boot_puts(x0 = NUL-terminated string)
 */
	.global	boot_puts
	.type	boot_puts, %function
boot_puts:
	mov	x9, x30
	mov	x10, x0
1:	ldrb	w0, [x10], #1
	cbz	w0, 2f
	bl	boot_putc
	b	1b
2:	mov	x30, x9
	ret
	.size	boot_puts, . - boot_puts

/*
 * boot_puthex(x0 = value)	prints 16 hex digits
 */
	.global	boot_puthex
	.type	boot_puthex, %function
boot_puthex:
	mov	x9, x30
	mov	x10, x0
	mov	w11, #16
1:	ror	x10, x10, #60		/* rotate the top nibble into the bottom */
	and	w0, w10, #0xf
	cmp	w0, #10
	b.lo	2f
	add	w0, w0, #('a' - 10 - '0')
2:	add	w0, w0, #'0'
	bl	boot_putc
	subs	w11, w11, #1
	b.ne	1b
	mov	x30, x9
	ret
	.size	boot_puthex, . - boot_puthex

msg_banner:	.asciz	"\r\nTessronOS boot: EL"
msg_dtb:	.asciz	"  DTB=0x"
msg_nl:		.asciz	"\r\n"
msg_moved:	.asciz	"TessronOS boot: moved from 0x"
	.balign	8
	.ltorg

/*
 * Initial page tables (physical, at the end of the image, cleared above)
 */
	.section .boot.pt, "aw", %nobits
	.balign	4096
	.global	pt_l0_k, pt_l1_ram, pt_l2_kern, pt_l1_dev, pt_l0_id, pt_l1_id
pt_l0_k:	.space	4096
pt_l1_ram:	.space	4096
pt_l2_kern:	.space	4096
pt_l1_dev:	.space	4096
pt_l0_id:	.space	4096
pt_l1_id:	.space	4096

/*
 * Physical addresses of the initial page tables for C code
 */
	.data
	.balign	8
	.global	knl_pt_l0_k_pa, knl_pt_l1_ram_pa, knl_pt_l2_kern_pa, knl_pt_l1_dev_pa, knl_pt_l0_id_pa
knl_pt_l0_k_pa:		.quad	pt_l0_k - KVA_BASE
knl_pt_l1_ram_pa:	.quad	pt_l1_ram - KVA_BASE
knl_pt_l2_kern_pa:	.quad	pt_l2_kern - KVA_BASE
knl_pt_l1_dev_pa:	.quad	pt_l1_dev - KVA_BASE
knl_pt_l0_id_pa:	.quad	pt_l0_id - KVA_BASE

/* physical entry address of the secondary cores (PSCI CPU_ON) */
	.global	knl_secondary_entry_pa
knl_secondary_entry_pa:	.quad	knl_secondary_entry

/* user section of the image (linker symbols, absolute) */
	.global	knl_user_start_va, knl_user_text_end_va, knl_user_end_va, knl_user_lma
knl_user_start_va:	.quad	__user_start
knl_user_text_end_va:	.quad	__user_text_end
knl_user_end_va:	.quad	__user_end
knl_user_lma:		.quad	__user_lma

/*
 * knl_start_va: first code executed at the virtual address (MMU on).
 *	x19 = DTB physical address, x20 = boot EL
 */
	.text
	.balign	4
	.global	knl_start_va
	.type	knl_start_va, %function
knl_start_va:
	adrp	x0, knl_vector_table
	add	x0, x0, :lo12:knl_vector_table
	msr	vbar_el1, x0

	adrp	x2, __tmp_stack_start
	add	x2, x2, :lo12:__tmp_stack_start
	mov	sp, x2

	/* Clear .bss */
	adrp	x3, _bss_start
	add	x3, x3, :lo12:_bss_start
	adrp	x4, _bss_end
	add	x4, x4, :lo12:_bss_end
1:	cmp	x3, x4
	b.hs	2f
	str	xzr, [x3], #8
	b	1b
2:
	adrp	x1, knl_boot_el
	str	w20, [x1, :lo12:knl_boot_el]	/* remembered for the PSCI conduit choice */

	mov	x0, #CPACR_FPEN			/* FP/SIMD usable */
	msr	cpacr_el1, x0
	mov	x0, #CNTKCTL_EL0VCTEN		/* a process reads the time as a count, as on Linux */
	msr	cntkctl_el1, x0
	isb
	msr	daifclr, #DAIF_A		/* take SErrors; IRQ/FIQ stay masked until the kernel starts */

	mov	x0, x19				/* DTB physical address */
	bl	reset_main			/* no return */
halt:
	wfi
	b	halt
	.size	knl_start_va, . - knl_start_va

/*
 * knl_secondary_start_va: a secondary core at the virtual address.
 *	x19 = processor slot index; SP comes from knl_secondary_sp[] (smp.c)
 */
	.global	knl_secondary_start_va
	.type	knl_secondary_start_va, %function
knl_secondary_start_va:
	adrp	x0, knl_vector_table
	add	x0, x0, :lo12:knl_vector_table
	msr	vbar_el1, x0
	adrp	x2, knl_secondary_sp
	add	x2, x2, :lo12:knl_secondary_sp
	ldr	x2, [x2, x19, lsl #3]
	mov	sp, x2
	mov	x0, #CPACR_FPEN
	msr	cpacr_el1, x0
	mov	x0, #CNTKCTL_EL0VCTEN
	msr	cntkctl_el1, x0
	isb
	msr	daifclr, #DAIF_A
	mov	x0, x19
	bl	knl_secondary_main		/* no return */
1:	wfi
	b	1b
	.size	knl_secondary_start_va, . - knl_secondary_start_va

#endif /* CPU_CORE_ARMV8A */
