/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	System-dependent definition (ARMv8-A / AArch64 core)
 */

#ifndef _SYSDEPEND_CPU_CORE_SYSDEPEND_
#define _SYSDEPEND_CPU_CORE_SYSDEPEND_

/* Exception and interrupt entries (vector.S, int_asm.S, dispatch.S) */
IMPORT void knl_vector_table(void);		/* exception vector table (VBAR_EL1) */
IMPORT void knl_dispatch_entry(void);		/* dispatch entry (SVC 8) */
IMPORT void knl_dispatch_to_schedtsk(void);	/* force dispatch (SVC 7) */
IMPORT void knl_hll_inthdr(void);		/* high level language interrupt handler entry */

IMPORT FP knl_intvec_tbl[N_INTVEC];		/* Interrupt vector table */
IMPORT FP knl_hll_inthdr_tbl[N_INTVEC];		/* High level language interrupt handler table */
IMPORT const FP knl_svcvec_tbl[N_SVCHDR];	/* SVC handler table */

/* Default handlers (exc_hdl.c) */
IMPORT void Default_Handler(UINT intno);	/* unregistered interrupt */
IMPORT void SVC_default_Handler(UINT svcno);	/* unregistered SVC */
IMPORT void FIQ_Handler(void *frame);		/* frame: the entry frame (vector.S) */
IMPORT void SError_Handler(void *frame);
IMPORT void knl_exception_handler(void *frame, UD esr, UD far);	/* synchronous exception other than SVC */
IMPORT void knl_kernel_fault(void *frame, UD esr, UD far);	/* the same taken in the kernel */

/* Boot information and timer (cpu_cntl.c, reset_main.c) */
IMPORT UBINT	knl_dtb_addr;			/* physical address of the DTB (0 if none) */
IMPORT UW	knl_boot_el;			/* exception level at entry: 2 = firmware/EL2, 1 = EL1 */
IMPORT UD	knl_cntfrq;			/* Generic Timer frequency (Hz) */

/* Real time clock (target rtc.c): UNIX seconds. E_NOSPT/E_IO/E_TMOUT when unavailable */
IMPORT ER	knl_rtc_read( UD *p_sec );
IMPORT ER	knl_rtc_write( UD sec );

/* Device tree (dtb.c) */
#define KNL_MAX_MEMRANGE	8
#define KNL_MAX_CPU		8
#define KNL_BOOTARGS_LEN	1024	/* the Raspberry Pi firmware puts its own words before cmdline.txt's */
#define KNL_STDOUT_LEN		64
#define KNL_COMPAT_LEN		128	/* the root node's compatible, a list of strings */
#define KNL_MODEL_LEN		64
#define KNL_MAX_PL011		4	/* PL011 UARTs the device tree may name */
#define PSCI_METHOD_UNKNOWN	0
#define PSCI_METHOD_HVC		1
#define PSCI_METHOD_SMC		2

typedef struct {
	UD	start;		/* physical start address */
	UD	size;		/* bytes */
} T_MEMRANGE;

IMPORT T_MEMRANGE knl_mem_range[KNL_MAX_MEMRANGE];	/* RAM ranges from /memory */
IMPORT INT	knl_mem_range_n;
IMPORT INT	knl_num_cpu;				/* number of /cpus/cpu@* nodes */
IMPORT UD	knl_cpu_mpidr[KNL_MAX_CPU];		/* their reg (MPIDR affinity) */
IMPORT UW	knl_psci_method;			/* PSCI_METHOD_* */
IMPORT UB	knl_bootargs[KNL_BOOTARGS_LEN];		/* /chosen/bootargs */
IMPORT UB	knl_stdout_path[KNL_STDOUT_LEN];	/* /chosen/stdout-path */
IMPORT UD	knl_dtb_size;				/* total size of the DTB */
IMPORT UB	knl_dtb_compat[KNL_COMPAT_LEN];		/* the root node's compatible */
IMPORT INT	knl_dtb_compat_len;			/* its bytes, the last string's NUL included */
IMPORT UB	knl_dtb_model[KNL_MODEL_LEN];		/* the root node's model */
IMPORT ER	knl_dtb_init( UBINT dtb );

/* What the firmware keeps for itself (/memreserve/ of the header) */
#define KNL_MAX_MEMRSV		8
IMPORT T_MEMRANGE knl_mem_rsv[KNL_MAX_MEMRSV];
IMPORT INT	knl_mem_rsv_n;

/*
 * How each processor is started: PSCI CPU_ON, or a spin table, where it
 * waits until its cpu-release-addr holds the physical address of where
 * to go (the Raspberry Pi 4's firmware)
 */
#define CPU_ENABLE_PSCI		0
#define CPU_ENABLE_SPIN		1
IMPORT UB	knl_cpu_enable[KNL_MAX_CPU];
IMPORT UD	knl_cpu_release[KNL_MAX_CPU];

/*
 * One property of the node at 'path' (dtb.c), once the kernel runs: its
 * bytes and length, NULL when there is none. knl_dtb_u32 answers the
 * first cell, or dflt. knl_dtb_ranges decodes entry 'index' of a ranges
 * or dma-ranges property: E_NOEXS when there is none.
 */
IMPORT CONST UB	*knl_dtb_prop( CONST char *path, CONST char *prop, INT *p_len );
IMPORT UW	knl_dtb_u32( CONST char *path, CONST char *prop, UW dflt );
IMPORT BOOL	knl_dtb_okay( CONST char *path );	/* there, and not disabled */
IMPORT ER	knl_dtb_ranges( CONST char *path, CONST char *prop, INT index,
				UD *p_child, UD *p_parent, UD *p_size );

/*
 * The root node's compatible and model from a device tree in memory,
 * read without touching anything else (dtb.c). E_NOEXS when it is not
 * one; compat_len is 0 and model empty when the node lacks them.
 */
IMPORT ER	knl_dtb_root( CONST UB *fdt, UB *compat, INT cmax, INT *p_clen, UB *model, INT mmax );

/*
 * A screen the firmware set up before this kernel started and described
 * in the device tree: the first enabled node compatible with
 * "simple-framebuffer". size is 0 when there was none. The page
 * allocator keeps its pages out of use.
 */
#define KNL_FB_FORMAT_LEN	16

typedef struct {
	UD	pa;		/* where the pixels are, physically */
	UD	size;		/* bytes */
	UW	width;
	UW	height;
	UW	stride;		/* bytes from one row to the next */
	UB	format[KNL_FB_FORMAT_LEN];	/* "a8r8g8b8", "r5g6b5" ... */
} T_DTBFB;

IMPORT T_DTBFB	knl_dtb_fb;

/* MMU (mmu.c) */
IMPORT void	knl_mmu_map_ram( void );		/* extend the linear map to all RAM ranges */
IMPORT void	*knl_dev_va( UBINT pa );		/* device window address of a physical register block */
IMPORT ER	knl_pt_map( UD l0_pa, UBINT va, UD pa, UD size, UD attr );	/* 4KB page mappings */
IMPORT ER	knl_pt_unmap( UD l0_pa, UBINT va, UD size );
IMPORT UD	knl_pt_lookup( UD l0_pa, UBINT va );	/* PA of a page mapping, ~0 if none */
IMPORT UD	knl_pt_entry( UD l0_pa, UBINT va );	/* the page's entry, 0 if none */
IMPORT UBINT	knl_pt_next( UD l0_pa, UBINT va, UBINT end );	/* first mapped page in [va, end), or end */
IMPORT void	knl_tlb_flush_va( UBINT va );
IMPORT void	knl_pt_prune( UD l0_pa, UBINT va, UD size );	/* tables left empty under it taken out (user half) */
IMPORT BOOL	knl_pt_reclaim( void );			/* those taken out given back once safe; TRUE: some wait */
IMPORT UW	knl_pt_pruned( INT *p_waiting );		/* tables taken out so far (the tests) */
IMPORT void	knl_icache_sync( UBINT va, UD size );	/* written instructions made fetchable */
IMPORT void	knl_dcache_clean_pou( UBINT va, UD size );	/* written instructions out to where they are fetched from */

/*
 * Every processor's instruction cache made to fetch what was written
 * through the linear map (the program loader, after writing a
 * program's pages and cleaning them to the point of unification with
 * knl_dcache_clean_pou)
 */
Inline void knl_icache_sync_all( void )
{
	Asm("dsb ish" ::: "memory");
	Asm("ic ialluis" ::: "memory");
	Asm("dsb ish; isb" ::: "memory");
}

/*
 * The root of a process space as the MMU takes it (TTBR0_EL1): the
 * physical address of its L0 table and its ASID in bits 63:48. The
 * kernel keeps that value for each space and each task (ttbr0) and
 * finds the space's ASID again in it. MMU_ASID_MAX is the largest ASID
 * TCR_EL1.AS gives; knl_mmu_asid_top is the largest the processor has
 * (ID_AA64MMFR0_EL1.ASIDBits: 16 bits, else 8).
 */
#define MMU_ASID_MAX	65535

Inline UD knl_mmu_space_root( UD l0_pa, UINT asid )
{
	return l0_pa | ((UD)asid << 48);
}

Inline UINT knl_mmu_root_asid( UD root )
{
	return (UINT)( ( root >> 48 ) & MMU_ASID_MAX );
}

Inline UINT knl_mmu_asid_top( void )
{
	UD	mmfr0;

	Asm("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
	return ( ( ( mmfr0 >> 4 ) & 0xF ) == 2 ) ? MMU_ASID_MAX : 255;
}

/*
 * A new L0 table of a process space or of the kernel's tasks made ready:
 * nothing to do, the kernel's half is TTBR1's
 */
Inline void knl_mmu_space_init_root( UD l0_pa )
{
	(void)l0_pa;
}

/*
 * A process space about to be freed, taken off every processor that
 * still has it installed: nothing to do, a TTBR0 left behind is never
 * walked for the kernel's own addresses
 */
Inline void knl_mmu_space_drop( UD ttbr0 )
{
	(void)ttbr0;
}

/* Every processor's TLB entries of the ASID dropped (the space's tables are gone) */
Inline void knl_tlb_flush_asid( UINT asid )
{
	Asm("dsb ishst" ::: "memory");
	Asm("tlbi aside1is, %0" :: "r"((UD)asid << 48) : "memory");
	Asm("dsb ish; isb" ::: "memory");
}

/* The machine of the programs the loader takes (ELF e_machine: EM_AARCH64) */
#define KNL_ELF_MACHINE	183

/*
 * Task context block
 */
typedef struct {
	void	*ssp;		/* System stack pointer (SP_EL1) */
	UD	ttbr0;		/* process space (TTBR0 value with ASID); knl_ttbr0_null for kernel tasks */
	void	*usp;		/* initial user stack pointer (EL0 tasks) */
	void	*ustk;		/* user stack area base (NULL: none) */
	UD	ustk_npages;
} CTXB;

/*
 * Per-processor data (design 8.2). TPIDR_EL1 of each processor points at
 * its own element; the fields replace the single-processor globals
 * knl_ctxtsk, knl_schedtsk, knl_dispatch_disabled, knl_taskindp,
 * knl_cur_ttbr0 and knl_fpu_ctx (macros in kernel.h / cpu_status.h).
 */
#define PC_OFFLINE	0
#define PC_BOOTING	1
#define PC_ONLINE	2
#define PC_STOPPED	3

typedef struct pcpu {
	ID	prcid;			/* 1..CNF_MAX_PRCID */
	UINT	state;			/* PC_* */
	UD	mpidr;
	struct task_control_block *ctxtsk;	/* task in execution (NULL: idle) */
	struct task_control_block *schedtsk;	/* task to execute */
	INT	dispatch_disabled;	/* DDS_* */
	W	taskindp;		/* task independent part nesting */
	UD	cur_ttbr0;		/* TTBR0 installed (0: boot identity map) */
	struct task_control_block *fpu_ctx;	/* task owning the FP/SIMD registers */
	void	*tmp_stack;		/* dispatcher/idle stack (top) */
	void	*irq_stack;		/* interrupt stack (top) */
	UD	ctxsw_cnt;		/* context switches */
	UD	ipi_cnt;		/* SGI_DISPATCH received */
#if KLK_FINE
	T_KLKCPU klk;			/* the kernel's locks it holds (klk.c) */
	UW	ipi_due;		/* the processors to be told, once the scheduler's lock is let go */
#endif
} T_PCPU;

IMPORT T_PCPU	knl_pcpu_tbl[];
IMPORT INT	knl_num_prc;			/* processors online */

#define knl_pcpu_of(i)	( &knl_pcpu_tbl[i] )

Inline T_PCPU *knl_pcpu( void )
{
	UD	p;
	Asm("mrs %0, tpidr_el1" : "=r"(p));
	return (T_PCPU *)p;
}

/*
 * The task this processor runs, read with interrupts masked: between
 * reading TPIDR_EL1 and reading its ctxtsk the task could otherwise be
 * moved to another processor and read that one's task
 */
Inline struct task_control_block *knl_self_ctxtsk( void )
{
	UD	daif, p;
	struct task_control_block *t;

	Asm( "mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) :: "memory" );
	Asm( "mrs %0, tpidr_el1" : "=r"(p) );
	t = ((T_PCPU *)p)->ctxtsk;
	Asm( "msr daif, %0" :: "r"(daif) : "memory" );
	return t;
}

/* Multiprocessor support (smp.c) */
IMPORT UINT	knl_lock_kernel( void );		/* big kernel lock: DI + lock, re-entrant per processor */
IMPORT void	knl_unlock_kernel( UINT intsts );	/* unlock + restore interrupts */
IMPORT void	knl_unlock_kernel_keep( void );		/* unlock, interrupts stay as they are */
IMPORT BOOL	knl_kernel_locked( void );
IMPORT void	knl_reschedule( void );			/* assign ready tasks to processors */
IMPORT struct task_control_block *knl_dispatch_pick( void );	/* dispatcher: take schedtsk under the lock */
IMPORT void	knl_acct_leave( void );			/* dispatcher: the task that ran charged its time */
IMPORT void	knl_send_ipi( UW prcmask, UINT sgino );
IMPORT void	knl_sync_cores( UD ttbr0 );		/* the others running in a space take an exception */
IMPORT void	knl_smp_init_primary( void );
IMPORT void	knl_smp_init_interrupt( void );
IMPORT void	knl_gic_cpu_init( void );
IMPORT void	knl_gic_cpuif_note( void );	/* this processor's bit in the distributor's targets (interrupt.c) */
IMPORT void	knl_smp_start_secondaries( void );
IMPORT ER	knl_psci_cpu_on( UD mpidr, UD entry_pa, UD ctx );	/* target hw_setting.c */
IMPORT UD	knl_psci_call( UD fid, UD a1, UD a2, UD a3 );

/* Finding the machine stopped (lockup.c, design 14.10) */
IMPORT void	knl_lockup_cpu_init( void );		/* this processor's timer and SGI_LOCKUP */
IMPORT void	knl_lockup_init_interrupt( void );
IMPORT void	knl_lockup_arm( void );			/* the checks begin */
IMPORT BOOL	knl_wdog_hw_feed( UW ms );		/* the board's watchdog: reset ms from now; FALSE: none */
IMPORT void	knl_wdog_hw_stop( void );
IMPORT void	knl_lockup_say_task( CONST char *pre, struct task_control_block *t );	/* urgent output */
IMPORT void	knl_lockup_say_frames( CONST char *pre, UBINT fp );

/* TCB system dependent initialization/finalization (called by tk_cre_tsk / task deletion) */
#define DEFINE_TSK_SYSDEPEND
IMPORT ER	knl_tcb_sysdep_cre( TCB *tcb, CONST T_CTSK *pk_ctsk );
IMPORT ER	knl_tcb_sysdep_del( TCB *tcb );

/* User stack size and process space of the next EL0 task (cpu_cntl.c) */
IMPORT UD	knl_ustack_size;
IMPORT void	knl_set_ustack_size( UD size );
IMPORT ER	knl_tsk_set_space( ID tskid, UD ttbr0 );

/* Privileged Access Never: the way to the user pages (cpu_uaccess.h) */
#include "cpu_uaccess.h"

#endif /* _SYSDEPEND_CPU_CORE_SYSDEPEND_ */
