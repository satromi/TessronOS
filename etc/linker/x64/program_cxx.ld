/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	program_cxx.ld
 *	Link script of a TessronOS program in C++, or in C with threads
 *	(lib/libcxxrt, lib/libpthread; design 9.6, 17.19).
 *
 *	As program.ld, with what the C++ runtime needs besides: the tables
 *	of constructors and destructors, the unwinding tables and the
 *	exception tables (kept, not thrown away), and the thread local
 *	image (.tdata, .tbss) as a PT_TLS segment whose place, size and
 *	alignment the thread library reads through __tls_*.
 *
 *	The link is started with crti.o and crtbegin.o before the program
 *	and crtend.o and crtn.o after it, so that .init/.fini are whole and
 *	the unwinding tables begin and end where the runtime looks.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PROGRAM_BASE = 0x400000;
PAGE = 0x1000;

PHDRS
{
	text	PT_LOAD FLAGS(5);	/* R + X */
	rodata	PT_LOAD FLAGS(4);	/* R */
	data	PT_LOAD FLAGS(6);	/* R + W */
	tls	PT_TLS FLAGS(4);
	stack	PT_GNU_STACK FLAGS(6);	/* R + W, never executable */
}

SECTIONS
{
	. = PROGRAM_BASE;

	.text : {
		*(.text.startup .text.startup.*)
		*(.text.hot .text.hot.*)
		*(.text .text.* .gnu.linkonce.t.*)
		KEEP(*(SORT_NONE(.init)))
		KEEP(*(SORT_NONE(.fini)))
	} :text

	. = ALIGN(PAGE);
	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	} :rodata
	.eh_frame_hdr : {
		*(.eh_frame_hdr)
	} :rodata
	.eh_frame : {
		KEEP(*(.eh_frame))
		*(.eh_frame.*)
	} :rodata
	.gcc_except_table : {
		*(.gcc_except_table .gcc_except_table.*)
	} :rodata

	. = ALIGN(PAGE);
	.tdata : {
		*(.tdata .tdata.* .gnu.linkonce.td.*)
	} :data :tls
	.tbss : {
		*(.tbss .tbss.* .gnu.linkonce.tb.*)
		*(.tcommon)
	} :data :tls

	.preinit_array : {
		PROVIDE_HIDDEN(__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN(__preinit_array_end = .);
	} :data
	.init_array : {
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN(__init_array_end = .);
	} :data
	.fini_array : {
		PROVIDE_HIDDEN(__fini_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
		KEEP(*(.fini_array))
		PROVIDE_HIDDEN(__fini_array_end = .);
	} :data
	/*
	 * The lists crtbegin.o and crtend.o of x86_64-elf still keep, with
	 * -1 before them and 0 after, which their own code walks from the
	 * .init and .fini sections: crtbegin.o's part first, crtend.o's last,
	 * as the GNU linker's own script lays them, and kept out of the
	 * arrays above, where -1 and 0 would be called
	 */
	.ctors : {
		KEEP(*crtbegin.o(.ctors))
		KEEP(*crtbegin?.o(.ctors))
		KEEP(*(EXCLUDE_FILE(*crtend.o *crtend?.o) .ctors))
		KEEP(*(SORT(.ctors.*)))
		KEEP(*(.ctors))
	} :data
	.dtors : {
		KEEP(*crtbegin.o(.dtors))
		KEEP(*crtbegin?.o(.dtors))
		KEEP(*(EXCLUDE_FILE(*crtend.o *crtend?.o) .dtors))
		KEEP(*(SORT(.dtors.*)))
		KEEP(*(.dtors))
	} :data

	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.got .got.plt)
	} :data

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.* .gnu.linkonce.b.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	} :data

	_end = .;
	end = .;

	/* The thread local image: its first byte, what is in the file, all of it */
	__tls_start  = ADDR(.tdata);
	__tls_filesz = SIZEOF(.tdata);
	__tls_memsz  = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata);
	__tls_align  = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
	}
}
