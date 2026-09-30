/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	program.ld
 *	Link script of a TessronOS program (design 6.2.2, 9.6).
 *
 *	The image is loaded at 0x40_0000, the base a process space gives to
 *	the program. Text and data are in separate pages so that the loader
 *	can make the text read only and executable and leave the data
 *	writable and non executable.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PROGRAM_BASE = 0x400000;
PAGE = 0x1000;

PHDRS
{
	text	PT_LOAD FLAGS(5);	/* R + X */
	rodata	PT_LOAD FLAGS(4);	/* R */
	data	PT_LOAD FLAGS(6);	/* R + W */
	stack	PT_GNU_STACK FLAGS(6);	/* R + W, never executable */
}

SECTIONS
{
	. = PROGRAM_BASE;

	.text : {
		*(.text.startup)
		*(.text .text.*)
	} :text

	. = ALIGN(PAGE);
	.rodata : {
		*(.rodata .rodata.*)
	} :rodata

	. = ALIGN(PAGE);
	.data : {
		*(.data .data.*)
		*(.got .got.plt)
	} :data

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	} :data

	_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
	}
}
