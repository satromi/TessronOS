/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tkernel_map.ld
 *	Linker script for QEMU virt (AArch64)
 *
 *	LOAD_PA is passed from the makefile (-Wl,--defsym,LOAD_PA=...) and is
 *	2MB aligned. .boot sits at its physical address; the rest is linked
 *	into the linear map (KVA_BASE + PA) and loaded at PA (AT).
 *
 *	W^X (design 5.11): the image is mapped with 2MB blocks. .boot, .text
 *	and .rodata share the first block(s) (RO+X); .data starts on a 2MB
 *	boundary so that data, bss, stacks and the initial page tables are in
 *	blocks mapped RW+XN.
 */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

KVA_BASE = 0xFFFF000000000000;
BOOT_STACK_SIZE = 0x4000;

SECTIONS
{
	. = LOAD_PA;
	__boot_start = .;

	.boot : {
		KEEP(*(.boot))
	}
	__boot_end_pa = .;

	. = KVA_BASE + ALIGN(__boot_end_pa, 0x1000);
	__kernel_start = .;

	.text : AT(ADDR(.text) - KVA_BASE) ALIGN(0x1000) {
		*(.text .text.*)
	}

	.rodata : AT(ADDR(.rodata) - KVA_BASE) ALIGN(0x1000) {
		*(.rodata .rodata.*)
		*(.gcc_except_table .gcc_except_table.*)
	}
	__text_end = .;

	/* user section of the image (EL0 code/data, mapped into the system process space) */
	__user_lma = ALIGN(LOADADDR(.rodata) + SIZEOF(.rodata), 0x1000);
	. = 0x400000;
	.user : AT(__user_lma) ALIGN(0x1000) {
		__user_start = .;
		*(.user.text .user.text.*)
		. = ALIGN(0x1000);
		__user_text_end = .;
		*(.user.data .user.data.*)
		. = ALIGN(0x1000);
		__user_end = .;
	}
	. = KVA_BASE + __user_lma + SIZEOF(.user);

	.data : AT(ADDR(.data) - KVA_BASE) ALIGN(0x200000) {
		__data_start = .;
		*(.data .data.*)
		. = ALIGN(16);
	}

	.noinit (NOLOAD) : AT(ADDR(.noinit) - KVA_BASE) ALIGN(16) {
		*(.noinit)
	}

	.bss (NOLOAD) : AT(ADDR(.bss) - KVA_BASE) ALIGN(16) {
		_bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		_bss_end = .;
	}

	.stack (NOLOAD) : AT(ADDR(.stack) - KVA_BASE) ALIGN(16) {
		__tmp_stack_end = .;
		. += BOOT_STACK_SIZE;
		__tmp_stack_start = .;
	}

	.boot.pt (NOLOAD) : AT(ADDR(.boot.pt) - KVA_BASE) ALIGN(0x1000) {
		__boot_pt_start = .;
		*(.boot.pt)
		__boot_pt_end = .;
	}

	. = ALIGN(0x1000);
	__kernel_end = .;
	_HeapStart = .;

	/DISCARD/ : {
		*(.note.GNU-stack)
		*(.comment)
	}

	_text_start	= ADDR(.text);
	_text_end	= _text_start + SIZEOF(.text);
	_data_org	= LOADADDR(.data);
	_data_start	= ADDR(.data);
	_data_end	= _data_start + SIZEOF(.data);
	__image_end_pa	= LOADADDR(.data) + SIZEOF(.data);	/* end of the flat image */
	_bss_size	= _bss_end - _bss_start;
}
