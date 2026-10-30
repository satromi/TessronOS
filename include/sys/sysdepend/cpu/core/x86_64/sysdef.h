/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	System dependencies definition (x86-64 core)
 *	Included also from assembler program.
 */

#ifndef __SYS_SYSDEF_DEPEND_CORE_H__
#define __SYS_SYSDEF_DEPEND_CORE_H__

/* 64 bit constants usable from both C and the assembler */
#ifdef _in_asm_source_
#define U64(x)			x
#else
#define U64(x)			x##ULL
#endif

/* ------------------------------------------------------------------------ */
/*
 * RFLAGS
 */
#define RFL_CF			0x00000001
#define RFL_FIXED		0x00000002	/* always 1 */
#define RFL_PF			0x00000004
#define RFL_AF			0x00000010
#define RFL_ZF			0x00000040
#define RFL_SF			0x00000080
#define RFL_TF			0x00000100	/* single step */
#define RFL_IF			0x00000200	/* interrupts enabled */
#define RFL_DF			0x00000400
#define RFL_OF			0x00000800
#define RFL_IOPL		0x00003000
#define RFL_NT			0x00004000
#define RFL_RF			0x00010000
#define RFL_AC			0x00040000	/* SMAP: the kernel may reach user pages */
#define RFL_ID			0x00200000
#define RFL_ARITH		(RFL_CF|RFL_PF|RFL_AF|RFL_ZF|RFL_SF|RFL_OF)

/*
 * Control registers, EFER and the model specific registers the kernel uses
 */
#define CR0_PE			0x00000001
#define CR0_MP			0x00000002
#define CR0_EM			0x00000004
#define CR0_TS			0x00000008
#define CR0_NE			0x00000020
#define CR0_WP			0x00010000
#define CR0_AM			0x00040000
#define CR0_NW			0x20000000
#define CR0_CD			0x40000000
#define CR0_PG			0x80000000

#define CR4_PSE			0x00000010
#define CR4_PAE			0x00000020
#define CR4_PGE			0x00000080
#define CR4_OSFXSR		0x00000200
#define CR4_OSXMMEXCPT		0x00000400
#define CR4_UMIP		0x00000800
#define CR4_FSGSBASE		0x00010000
#define CR4_PCIDE		0x00020000
#define CR4_OSXSAVE		0x00040000
#define CR4_SMEP		0x00100000
#define CR4_SMAP		0x00200000

#define XCR0_X87		0x00000001
#define XCR0_SSE		0x00000002
#define XCR0_AVX		0x00000004

#define MSR_APIC_BASE		0x0000001b
#define MSR_TSC_ADJUST		0x0000003b
#define MSR_TSC_DEADLINE	0x000006e0
#define MSR_PAT			0x00000277
#define MSR_X2APIC_BASE		0x00000800	/* x2APIC registers: 0x800 + (MMIO offset >> 4) */
#define MSR_EFER		0xc0000080
#define MSR_STAR		0xc0000081
#define MSR_LSTAR		0xc0000082
#define MSR_CSTAR		0xc0000083
#define MSR_SFMASK		0xc0000084
#define MSR_FS_BASE		0xc0000100
#define MSR_GS_BASE		0xc0000101
#define MSR_KERNEL_GS_BASE	0xc0000102
#define MSR_TSC_AUX		0xc0000103

#define APIC_BASE_BSP		0x00000100
#define APIC_BASE_EXTD		0x00000400	/* x2APIC mode */
#define APIC_BASE_EN		0x00000800

#define EFER_SCE		0x00000001	/* SYSCALL/SYSRET */
#define EFER_LME		0x00000100
#define EFER_LMA		0x00000400
#define EFER_NXE		0x00000800

/*
 * The page attribute table the kernel sets: entry 0 write-back, 1
 * write-combining, 2 uncached-minus, 3 uncached, then the same again.
 * A page entry picks one with PWT (1), PCD (2) and PAT (4).
 */
#define PAT_VALUE		U64(0x0007010600070106)

/* ------------------------------------------------------------------------ */
/*
 * Segments. One GDT for all processors; each has its own TSS
 * descriptor, two entries from GDT_TSS on. The order of the user ones
 * is what SYSRET takes from STAR: 32 bit code, data, 64 bit code.
 */
#define GDT_NULL		0x00
#define GDT_KCODE		0x08
#define GDT_KDATA		0x10
#define GDT_UCODE32		0x18
#define GDT_UDATA		0x20
#define GDT_UCODE		0x28
#define GDT_TSS			0x30
#define GDT_TSS_SEL(n)		( GDT_TSS + (n) * 16 )

#define SEL_RPL3		3
#define SEL_KCODE		GDT_KCODE
#define SEL_KDATA		GDT_KDATA
#define SEL_UDATA		( GDT_UDATA | SEL_RPL3 )
#define SEL_UCODE		( GDT_UCODE | SEL_RPL3 )

/* What the interrupt stack table of each TSS is for */
#define IST_DF			1		/* double fault */
#define IST_NMI			2		/* NMI */
#define IST_MC			3		/* machine check */
#define IST_STACK_SIZE		8192

/* ------------------------------------------------------------------------ */
/*
 * Virtual memory layout. 48 bit addresses, 4 level paging; the low half
 * belongs to the process spaces, the high half to the kernel.
 *	KVA_BASE	linear map of RAM: VA = PA + KVA_BASE
 *	DEV_VA_BASE	device window: every physical address is visible at
 *			DEV_VA_BASE + PA as uncached memory
 *	KIMG_VA_BASE	the kernel image (-mcmodel=kernel: the top 2GB),
 *			placed wherever the loader found room: VA2PA takes
 *			the image's addresses too
 */
#define KVA_BASE		U64(0xFFFF800000000000)
#define DEV_VA_BASE		U64(0xFFFFC00000000000)
#define VMAP_VA_BASE		U64(0xFFFFD00000000000)
#define PCPU_VA_BASE		U64(0xFFFFE00000000000)
#define FIXMAP_VA_BASE		U64(0xFFFFE80000000000)
#define KIMG_VA_BASE		U64(0xFFFFFFFF80000000)
#define KIMG_LINK_VA		U64(0xFFFFFFFF80200000)	/* where the image is linked */

#ifndef _in_asm_source_
/*
 * The window holds the physical addresses below DEV_NEAR_SIZE (15TB) at
 * DEV_VA_BASE + PA. A machine with more address bits puts its 64 bit
 * BARs far above (QEMU at the top of what the host processor can
 * address): such a gigabyte is given a slot of the window's last
 * terabyte as it is first asked for (knl_dev_far, mmu.c)
 */
#define DEV_NEAR_SIZE		U64(0x00000F0000000000)
extern unsigned long long	knl_dev_far( unsigned long long pa );
#define DEV_BASE(pa)		( (unsigned long long)(pa) < DEV_NEAR_SIZE			\
				  ? DEV_VA_BASE + (unsigned long long)(pa)		\
				  : knl_dev_far((unsigned long long)(pa)) )
extern unsigned long long	knl_kimg_off;		/* image VA - image PA (reset_main.c) */
#define PA2VA(pa)		((void *)((UBINT)(pa) + KVA_BASE))
#define VA2PA(va)		( (UBINT)(va) >= KIMG_VA_BASE				\
				  ? (UBINT)(va) - (UBINT)knl_kimg_off			\
				  : (UBINT)(va) - KVA_BASE )
#endif

/* ------------------------------------------------------------------------ */
/*
 * Page table entries (4KB pages; 2MB and 1GB ones at the upper levels
 * with PTE_PS)
 */
#define PTE_P			U64(0x0000000000000001)	/* present */
#define PTE_RW			U64(0x0000000000000002)	/* writable */
#define PTE_US			U64(0x0000000000000004)	/* user */
#define PTE_PWT			U64(0x0000000000000008)
#define PTE_PCD			U64(0x0000000000000010)
#define PTE_A			U64(0x0000000000000020)	/* accessed */
#define PTE_D			U64(0x0000000000000040)	/* dirty */
#define PTE_PS			U64(0x0000000000000080)	/* large page (PDPT, PD); PAT in a PT */
#define PTE_G			U64(0x0000000000000100)	/* global */
#define PTE_PAT_LARGE		U64(0x0000000000001000)	/* PAT of a large page */
#define PTE_NX			U64(0x8000000000000000)	/* never executed */

#define PTE_CACHE_WB		U64(0)
#define PTE_CACHE_WC		PTE_PWT
#define PTE_CACHE_UC		(PTE_PWT|PTE_PCD)

#define PTE_VALID		PTE_P
/*
 * Software bit (ignored by the walk): a page of a process's space that
 * the space maps but does not own (design 5.1). The same bit as on the
 * other architecture.
 */
#define PTE_SW_SHARED		U64(0x0080000000000000)	/* bit 55 */
#define PTE_ADDR_MASK		U64(0x000FFFFFFFFFF000)

/* What a page entry lets a process do: read it, write it, run it */
#define PTE_USER_R(e)		( ( (e) & PTE_US ) != 0 )
#define PTE_USER_W(e)		( ( (e) & ( PTE_US | PTE_RW ) ) == ( PTE_US | PTE_RW ) )
#define PTE_USER_X(e)		( ( (e) & PTE_US ) != 0 && ( (e) & PTE_NX ) == 0 )
/* the process may read it, and write it when w */
#define PTE_USER_OK(e, w)	( PTE_USER_R(e) && ( !(w) || PTE_USER_W(e) ) )

/* Entry of a table that points to the next level: the leaves decide the rights */
#define PTE_TABLE_DESC		(PTE_P | PTE_RW | PTE_US)

/*
 * Page entries. "Not cached" (KDATA_NC, the vmap's VMAP_NOCACHE) is what
 * the screens ask for, memory whose writes may merge and reorder on
 * their way out: write-combining. DMA needs nothing of the kind on a PC,
 * whose bus masters see the caches.
 */
#define PTE_PAGE		(PTE_P | PTE_A)
#define PTE_PAGE_KDATA		(PTE_PAGE | PTE_RW | PTE_D | PTE_G | PTE_NX)
#define PTE_PAGE_KDATA_NC	(PTE_PAGE_KDATA | PTE_CACHE_WC)
#define PTE_PAGE_KTEXT		(PTE_PAGE | PTE_G)
#define PTE_PAGE_KRO		(PTE_PAGE | PTE_G | PTE_NX)
#define PTE_PAGE_KDEV		(PTE_PAGE | PTE_RW | PTE_D | PTE_G | PTE_NX | PTE_CACHE_UC)
#define PTE_PAGE_UDATA		(PTE_PAGE | PTE_US | PTE_RW | PTE_D | PTE_NX)
#define PTE_PAGE_UDATA_NC	(PTE_PAGE_UDATA | PTE_CACHE_WC)
#define PTE_PAGE_UTEXT		(PTE_PAGE | PTE_US)
#define PTE_PAGE_URO		(PTE_PAGE | PTE_US | PTE_NX)

/* Large pages of the linear map and the device window */
#define PTE_LARGE_RAM		(PTE_P | PTE_RW | PTE_A | PTE_D | PTE_PS | PTE_G | PTE_NX)
#define PTE_LARGE_DEV		(PTE_LARGE_RAM | PTE_CACHE_UC)

#define L0_SHIFT		39		/* PML4 */
#define L1_SHIFT		30		/* PDPT */
#define L2_SHIFT		21		/* PD */
#define L3_SHIFT		12		/* PT */
#define PT_ENTRIES		512
#define PT_USER_ENTRIES		256		/* entries of a PML4 that belong to the process */

#define KERNEL_BLOCK_SIZE	0x200000

/* ------------------------------------------------------------------------ */
/*
 * Interrupts. An interrupt number is the vector the processor takes it
 * at; its priority is the vector's upper four bits (the Local APIC's
 * priority class), so a level of the API stands for a class and the
 * vector already says it.
 *	0x00 .. 0x1f	exceptions
 *	0x20 .. 0x2f	the 8259 PICs, masked (should one still fire)
 *	0x30 .. 0x4f	IOAPIC inputs (GSI 0 .. 31)
 *	0x50 .. 0xdf	message signalled interrupts (MSI, MSI-X)
 *	0xe8, 0xe9	the dispatcher, raised by the kernel itself (int)
 *	0xf0 .. 0xf7	the system timer and the inter-processor interrupts
 *	0xfe, 0xff	Local APIC error, spurious
 */
#define N_INTVEC		256
#define INTVEC_PIC		0x20
#define INTVEC_GSI(g)		( 0x30 + (g) )
#define INTVEC_GSI_NUM		32
#define INTVEC_MSI_FIRST	0x50
#define INTVEC_MSI_NUM		0x90
#define INTVEC_TIMER		0xf0
#define INTVEC_IPI(n)		( 0xf1 + (n) )
#define INTVEC_APIC_ERROR	0xfe
#define INTVEC_SPURIOUS		0xff

#define TIMER_INTNO		INTVEC_TIMER
#define INTNO_SYSTICK		TIMER_INTNO

/* Interrupt priority (API level 0 = highest .. 15 = lowest = class 15 - level) */
#define INTPRI_BITWIDTH		4
#define INTPRI_HIGHEST		0
#define INTPRI_LOWEST		15
#define INTPRI_SYSTICK		0		/* system timer (class 15) */
#define INTPRI_IPI		0		/* inter-processor interrupt (class 15) */
#define INTPRI_DEVICE		8		/* ordinary device drivers */
#define INTPRI_OF_VEC(v)	( 15 - ( (v) >> 4 ) )

/* Inter-processor interrupts (design 8.8): INTVEC_IPI(SGI_*) */
#define SGI_DISPATCH		0	/* reschedule request */
#define SGI_TIMER		1	/* reprogram the timer on PRC 1 */
#define SGI_CALL		2	/* function call request (reserved) */
#define SGI_STOP		3	/* stop request */
#define SGI_LOCKUP		4	/* say where you are (lockup.c) */
#define SGI_TLB			5	/* drop the translations asked for (mmu.c) */
#define SGI_SYNC		6	/* take an interrupt: a space's tables changed */
#define SGI_NUM			7

/* System timer period range (ms) */
#define MIN_TIMER_PERIOD	1
#define MAX_TIMER_PERIOD	50
#define TIMER_INTLEVEL		0

/* ------------------------------------------------------------------------ */
/*
 * Memory below 1MB stays out of use: the real mode remnants, and the
 * page the other processors start in
 */
#define LOW_RESERVED_END	0x100000
#define INTERNAL_RAM_START	0x00100000
#define INTERNAL_RAM_END	0x10000000	/* used only without a memory map */

/* ------------------------------------------------------------------------ */
/*
 * COM1 (16550): the console of the PC that has one
 */
#define COM1_PORT		0x3f8
#define COM1_GSI		4
#define UART_THR		0
#define UART_RBR		0
#define UART_IER		1
#define UART_FCR		2
#define UART_LCR		3
#define UART_MCR		4
#define UART_LSR		5
#define UART_LSR_DR		0x01
#define UART_LSR_THRE		0x20

/* ------------------------------------------------------------------------ */
/*
 * Task system stack: the interrupt frame (184) + the XSAVE area of a
 * TA_FPU task (up to 832 for x87, SSE and AVX) + C frames
 */
#define MIN_SYS_STACK_SIZE	4096
#define DEFAULT_SYS_STKSZ	8192

/* CPU features */
#define CPU_HAS_FPU		1
#define CPU_HAS_DSP		0
#define CPU_HAS_PTMR		0
#if USE_FPU
#define NUM_COPROCESSOR		1
#else
#define NUM_COPROCESSOR		0
#endif
#define MXCSR_INIT		0x00001f80	/* round to nearest, all exceptions masked */
#define FCW_INIT		0x037f

/* ------------------------------------------------------------------------ */
/*
 * Software interrupts the kernel raises itself (vectors)
 */
#define SVC_FORCE_DISPATCH	0xe8	/* throw the context away and dispatch */
#define SVC_DISPATCH		0xe9	/* save the context and dispatch */
#define N_SVCHDR		0

#endif /* __SYS_SYSDEF_DEPEND_CORE_H__ */
