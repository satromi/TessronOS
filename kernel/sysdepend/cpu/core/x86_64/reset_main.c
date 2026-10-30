/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	reset_main.c (x86-64)
 *	C part of the boot sequence, called from boot.S on the loader's
 *	page tables with the boot stack and the kernel's GDT, interrupts
 *	disabled.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include "../../../sysdepend.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include <tm/tmonitor.h>
#include <ts/board.h>
#include "sys_timer.h"

EXPORT	void	*knl_lowmem_top;		/* Head of the system memory area */
EXPORT	void	*knl_lowmem_limit;		/* End of the system memory area */

EXPORT unsigned long long	knl_kimg_off;		/* image VA - image PA (VA2PA) */
EXPORT UD			knl_kimg_pa;
EXPORT CONST T_BOOTINFO		*knl_bootinfo;
EXPORT UD			knl_cntfrq;

EXPORT T_MEMRANGE	knl_mem_range[KNL_MAX_MEMRANGE];
EXPORT INT		knl_mem_range_n = 0;
EXPORT T_MEMRANGE	knl_mem_rsv[KNL_MAX_MEMRSV];
EXPORT INT		knl_mem_rsv_n = 0;
EXPORT UB		knl_bootargs[KNL_BOOTARGS_LEN];

IMPORT	const void *_HeapStart;			/* end of the kernel image (linker script) */
IMPORT	UBINT	knl_user_lma;			/* boot.S */
IMPORT	void	knl_idt_init( void );		/* interrupt.c */

LOCAL CONST char *mem_name( UW t )
{
	switch ( t ) {
	case BI_MEM_FREE:	return "free";
	case BI_MEM_KERNEL:	return "kernel";
	case BI_MEM_ACPI:	return "ACPI";
	case BI_MEM_NVS:	return "ACPI NVS";
	case BI_MEM_RESERVED:	return "reserved";
	case BI_MEM_MMIO:	return "MMIO";
	case BI_MEM_BAD:	return "bad";
	default:		return "?";
	}
}

/*
 * The loader's memory map in the names the page allocator reads: the
 * RAM it may hand out and the kernel's own (knl_mem_range), and of that
 * what is not to be handed out -- the kernel's image and heap, its page
 * tables, the loader's information, the processors' start page
 * (knl_mem_rsv). The firmware's memory, ACPI's and the devices' are in
 * neither.
 */
LOCAL void take_memory_map( CONST T_BOOTINFO *bi )
{
	UW	i;

	for ( i = 0; i < bi->nmem; i++ ) {
		CONST T_BIMEM	*m = &bi->mem[i];

		if ( m->type != BI_MEM_FREE && m->type != BI_MEM_KERNEL ) {
			continue;
		}
		if ( knl_mem_range_n > 0
		  && knl_mem_range[knl_mem_range_n - 1].start + knl_mem_range[knl_mem_range_n - 1].size == m->start ) {
			knl_mem_range[knl_mem_range_n - 1].size += m->size;
		} else if ( knl_mem_range_n < KNL_MAX_MEMRANGE ) {
			knl_mem_range[knl_mem_range_n].start = m->start;
			knl_mem_range[knl_mem_range_n].size = m->size;
			knl_mem_range_n++;
		}
		if ( m->type == BI_MEM_KERNEL && knl_mem_rsv_n < KNL_MAX_MEMRSV ) {
			knl_mem_rsv[knl_mem_rsv_n].start = m->start;
			knl_mem_rsv[knl_mem_rsv_n].size = m->size;
			knl_mem_rsv_n++;
		}
	}
}

/* The loader's screen, which the page allocator leaves alone and the screen's driver takes */
LOCAL void take_screen( CONST T_BOOTINFO *bi )
{
	knl_memset(&knl_dtb_fb, 0, sizeof(knl_dtb_fb));
	if ( bi->fb.pa == 0 || bi->fb.format == BI_FB_NONE ) {
		return;
	}
	knl_dtb_fb.pa = bi->fb.pa;
	knl_dtb_fb.size = bi->fb.size;
	knl_dtb_fb.width = bi->fb.width;
	knl_dtb_fb.height = bi->fb.height;
	knl_dtb_fb.stride = bi->fb.stride;
	knl_memcpy(knl_dtb_fb.format, ( bi->fb.format == BI_FB_RGBX ) ? "x8b8g8r8" : "x8r8g8b8", 9);
}

LOCAL void print_boot_info( CONST T_BOOTINFO *bi )
{
	INT	i;
	UD	free = 0;

	for ( i = 0; i < (INT)bi->nmem; i++ ) {
		UD	s = bi->mem[i].start, e = s + bi->mem[i].size;

		if ( bi->mem[i].type == BI_MEM_FREE ) free += bi->mem[i].size;
		if ( bi->mem[i].type == BI_MEM_FREE && bi->mem[i].size < 0x100000 ) continue;
		tm_printf((UB*)"TessronOS: mem %x%08x-%x%08x %s\n",
			(UW)(s >> 32), (UW)s, (UW)(e >> 32), (UW)e, mem_name(bi->mem[i].type));
	}
	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	e = knl_mem_range[i].start + knl_mem_range[i].size;
		tm_printf((UB*)"TessronOS: RAM[%d] %x%08x-%x%08x (%d MB)\n", i,
			(UW)(knl_mem_range[i].start >> 32), (UW)knl_mem_range[i].start,
			(UW)(e >> 32), (UW)e, (UW)(knl_mem_range[i].size >> 20));
	}
	tm_printf((UB*)"TessronOS: pages %d total, %d free (%d MB, the firmware left %d MB); Imalloc %x%08x-%x%08x\n",
		(UW)knl_pf_total_count(-1), (UW)knl_pf_free_count(-1), (UW)(knl_pf_free_count(-1) >> 8),
		(UW)(free >> 20),
		(UW)((UBINT)knl_lowmem_top >> 32), (UW)(UBINT)knl_lowmem_top,
		(UW)((UBINT)knl_lowmem_limit >> 32), (UW)(UBINT)knl_lowmem_limit);
}

IMPORT void	knl_fbcon_mark( CONST T_BOOTINFO *bi, UW k, UW r, UW g, UW b );	/* fbcon.c */

EXPORT void reset_main( UD bi_pa )
{
	CONST T_BOOTINFO	*bi = (CONST T_BOOTINFO *)PA2VA(bi_pa);
	UBINT			top, limit;		/* physical */
	INT			i;

	/* the boot processor's T_PCPU from the first: knl_pcpu() reads it */
	knl_pcpu_boot.self = &knl_pcpu_boot;
	wrmsr(MSR_GS_BASE, (UD)(UBINT)&knl_pcpu_boot);
	knl_bootinfo = bi;
	/*
	 * The loader's PML4 from the first: the device window's pages that
	 * are not mapped yet (a screen above 4GB) are made on first use
	 * (knl_dev_fault, knl_dev_set_wc), which walks it
	 */
	knl_pt_l0_k_pa = bi->pml4_pa;
	knl_fbcon_mark(bi, BI_MK_KENTRY, 0xff, 0xff, 0xff);
	knl_kimg_pa = bi->kimg_pa;
	knl_kimg_off = bi->kimg_va - bi->kimg_pa;
	knl_user_lma = knl_user_lma - (UBINT)( bi->kimg_va - KIMG_VA_BASE ) + (UBINT)bi->kimg_pa;
	knl_idt_init();				/* an exception is told from here on */
	knl_fbcon_mark(bi, BI_MK_KIDT, 0x90, 0x90, 0x90);

	libtm_init();
	knl_fbcon_mark(bi, BI_MK_KTM, 0x90, 0x50, 0x10);
	knl_fbcon_init(bi);
	knl_fbcon_mark(bi, BI_MK_KFB, 0xff, 0x90, 0xc0);
	tm_printf((UB*)"\nTessronOS x64: kernel %x%08x at %x%08x, %d KB; firmware \"%s\"; settings \"%s\"\n",
		(UW)(bi->kimg_va >> 32), (UW)bi->kimg_va, (UW)(bi->kimg_pa >> 32), (UW)bi->kimg_pa,
		(UW)(bi->kimg_size >> 10), bi->fw_vendor, bi->cmdline);
	if ( bi->magic != BI_MAGIC || bi->version != BI_VERSION || bi->size != sizeof(T_BOOTINFO) ) {
		tm_printf((UB*)"TessronOS x64: the loader is of another version (%x %d %d). Stopped.\n",
			bi->magic, bi->version, bi->size);
		for ( ;; ) {
			Asm("hlt");
		}
	}

	knl_fbcon_stage("the processor");
	knl_smp_init_primary();		/* GDT, TSS, IDT, GS base, SYSCALL, paging, x87/SSE, SMAP */
	for ( i = 0; i < KNL_BOOTARGS_LEN - 1 && bi->cmdline[i] != '\0'; i++ ) {
		knl_bootargs[i] = (UB)bi->cmdline[i];
	}
	knl_bootargs[i] = '\0';
	take_memory_map(bi);
	take_screen(bi);
	knl_fbcon_stage("ACPI tables");
	(void)knl_acpi_init(bi->rsdp_pa);
	knl_board_init();		/* which machine, as SMBIOS names it (board.c) */
	knl_fbcon_stage("the processor's clock");
	knl_tsc_calibrate();
	knl_fbcon_stage("memory management");
	knl_mmu_init();

	/* Startup hardware (board dependent) */
	knl_startup_hw();

	/*
	 * Page frame database right after the kernel image, then the Imalloc
	 * area (CNF_IMALLOC_SIZE) for the kernel objects: the loader gave
	 * the run of pages after the image for them. The rest of RAM goes
	 * to the buddy allocator.
	 */
	top = VA2PA(&_HeapStart);
	knl_pfalloc_init(&top, CNF_IMALLOC_SIZE);
	knl_space_init();		/* spaces: empty (kernel tasks) and the system process */
	limit = top + CNF_IMALLOC_SIZE;
#if USE_IMALLOC
	knl_lowmem_top = PA2VA(top);
	knl_lowmem_limit = PA2VA(limit);
#endif	/* USE_IMALLOC */

	print_boot_info(bi);

	/* Startup Kernel */
	knl_fbcon_stage("the kernel (other processors, timers, the first task)");
	knl_main();		/**** No return ****/

	while (1) {
		Asm("hlt");
	}
}

#endif	/* CPU_CORE_X86_64 */
