/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	reset_main.c (ARMv8-A / AArch64)
 *	C part of the boot sequence, called from boot.S in EL1 with the MMU
 *	off, .bss cleared and the boot stack set up.
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include <tm/tmonitor.h>

EXPORT	void	*knl_lowmem_top;		/* Head of the system memory area */
EXPORT	void	*knl_lowmem_limit;		/* End of the system memory area */

IMPORT	const void *_HeapStart;			/* end of the kernel image (linker script) */

LOCAL void print_boot_info( void )
{
	INT	i;
	UD	frq;

	Asm("mrs %0, cntfrq_el0" : "=r"(frq));
	tm_printf((UB*)"TessronOS: CNTFRQ=%d Hz  CPUs=%d  PSCI=%s  DTB=%x%08x (%d bytes)\n",
		(UW)frq, knl_num_cpu,
		(knl_psci_method == PSCI_METHOD_SMC) ? "smc" :
		(knl_psci_method == PSCI_METHOD_HVC) ? "hvc" : "?",
		(UW)(knl_dtb_addr >> 32), (UW)knl_dtb_addr, (UW)knl_dtb_size);
	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	e = knl_mem_range[i].start + knl_mem_range[i].size;
		tm_printf((UB*)"TessronOS: RAM[%d] %x%08x-%x%08x (%d MB)\n", i,
			(UW)(knl_mem_range[i].start >> 32), (UW)knl_mem_range[i].start,
			(UW)(e >> 32), (UW)e, (UW)(knl_mem_range[i].size >> 20));
	}
	tm_printf((UB*)"TessronOS: pages %d total, %d free (%d MB); Imalloc %x%08x-%x%08x\n",
		(UW)knl_pf_total_count(-1), (UW)knl_pf_free_count(-1), (UW)(knl_pf_free_count(-1) >> 8),
		(UW)((UBINT)knl_lowmem_top >> 32), (UW)(UBINT)knl_lowmem_top,
		(UW)((UBINT)knl_lowmem_limit >> 32), (UW)(UBINT)knl_lowmem_limit);
}

EXPORT void reset_main( UBINT dtb )
{
	UBINT	top, limit;		/* physical */
	INT	i;

	knl_smp_init_primary();		/* per-CPU pointer (TPIDR_EL1) of the boot processor */
	knl_dtb_addr = dtb;
	knl_dtb_init(dtb != 0 ? (UBINT)PA2VA(dtb) : 0);	/* E_NOEXS without a DTB: the defaults below apply */
	knl_mmu_map_ram();		/* linear map for all RAM ranges */

	/* Startup hardware (board dependent) */
	knl_startup_hw();

	/*
	 * Page frame database right after the kernel image, then the Imalloc
	 * area (CNF_IMALLOC_SIZE) for the kernel objects. The rest of RAM goes
	 * to the buddy allocator.
	 */
	top = VA2PA(&_HeapStart);
	knl_pfalloc_init(&top, CNF_IMALLOC_SIZE);
	knl_space_init();		/* TTBR0 spaces: empty (kernel tasks) and the system process */
	/*
	 * Leave the identity map the MMU was turned on through. Its blocks
	 * are global, so a translation of them cached while the kernel came
	 * up would stay in the TLB whatever TTBR0 holds next; where the
	 * kernel sits in the first 1GB of memory (Raspberry Pi 5) it covers
	 * the addresses user code runs at. The secondaries do the same when
	 * they come up (smp.c).
	 */
	Asm("msr ttbr0_el1, %0" :: "r"(knl_ttbr0_null));
	Asm("isb");
	Asm("tlbi vmalle1");
	Asm("dsb ish");
	Asm("isb");
	limit = top + CNF_IMALLOC_SIZE;
	(void)i;
#if USE_IMALLOC
	knl_lowmem_top = PA2VA(top);
	knl_lowmem_limit = PA2VA(limit);
#endif	/* USE_IMALLOC */

	print_boot_info();

	/* Startup Kernel */
	knl_main();		/**** No return ****/

	while (1) {
		Asm("wfi");
	}
}

#endif	/* CPU_CORE_ARMV8A */
