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
 *	System dependencies definition (ARMv8-A / AArch64 core)
 *	Included also from assembler program.
 */

#ifndef __SYS_SYSDEF_DEPEND_CORE_H__
#define __SYS_SYSDEF_DEPEND_CORE_H__

/* ------------------------------------------------------------------------ */
/*
 * PSTATE / SPSR
 */
#define PSR_N		0x80000000		/* negative */
#define PSR_Z		0x40000000		/* zero */
#define PSR_C		0x20000000		/* carry */
#define PSR_V		0x10000000		/* overflow */
#define PSR_SS		0x00200000		/* software step */
#define PSR_IL		0x00100000		/* illegal execution state */
#define PSR_D		0x00000200		/* debug exception mask */
#define PSR_A		0x00000100		/* SError mask */
#define PSR_I		0x00000080		/* IRQ mask */
#define PSR_F		0x00000040		/* FIQ mask */
#define PSR_DAIF	(PSR_D|PSR_A|PSR_I|PSR_F)
#define PSR_M_EL0t	0x00000000
#define PSR_M_EL1t	0x00000004
#define PSR_M_EL1h	0x00000005
#define PSR_M_EL2t	0x00000008
#define PSR_M_EL2h	0x00000009
#define PSR_M_MASK	0x0000000f

#define PSR_DI		(PSR_I|PSR_F)		/* disable (ordinary) interrupt */

/* DAIF immediate bit positions for msr daifset / daifclr */
#define DAIF_F		1
#define DAIF_I		2
#define DAIF_A		4
#define DAIF_D		8

/* ------------------------------------------------------------------------ */
/*
 * System registers (bit definitions used by the boot code)
 */
#define SCTLR_M		(1 << 0)		/* MMU enable */
#define SCTLR_A		(1 << 1)		/* alignment check */
#define SCTLR_C		(1 << 2)		/* data cache enable */
#define SCTLR_SA	(1 << 3)		/* SP alignment check (EL1) */
#define SCTLR_SA0	(1 << 4)		/* SP alignment check (EL0) */
#define SCTLR_I		(1 << 12)		/* instruction cache enable */
#define SCTLR_DZE	(1 << 14)		/* DC ZVA at EL0 */
#define SCTLR_UCT	(1 << 15)		/* CTR_EL0 access at EL0 */
#define SCTLR_nTWI	(1 << 16)		/* do not trap WFI from EL0 */
#define SCTLR_nTWE	(1 << 18)		/* do not trap WFE from EL0 */
#define SCTLR_WXN	(1 << 19)		/* write permission implies XN */
#define SCTLR_UCI	(1 << 26)		/* cache maintenance at EL0 */
#define SCTLR_EL1_RES1	(0x30d00800)		/* RES1 bits: 11,20,22,23,28,29 */

#define HCR_RW		(1UL << 31)		/* EL1 is AArch64 */
#define HCR_SWIO	(1UL << 1)
#define CNTHCTL_EL1PCTEN (1 << 0)		/* EL1 may read CNTPCT */
#define CNTHCTL_EL1PCEN	(1 << 1)		/* EL1 may access CNTP_* */
#define CPTR_EL2_RES1	0x000033ff
#define CPTR_EL2_TFP	(1 << 10)
#define CPACR_FPEN	(3 << 20)		/* FP/SIMD not trapped at EL0/EL1 */
#define CNTKCTL_EL0VCTEN (1 << 1)		/* EL0 reads the virtual counter (cntvct_el0) */

/* ------------------------------------------------------------------------ */
/*
 * Virtual memory layout (design 6.2). 48 bit VA, 4KB granule, 4 levels.
 *	KVA_BASE	linear map of RAM: VA = PA + KVA_BASE (kernel image is inside)
 *	DEV_VA_BASE	device window: every physical address below 512GB is
 *			visible at DEV_VA_BASE + PA as Device-nGnRE memory
 */
#define KVA_BASE		0xFFFF000000000000
#define DEV_VA_BASE		0xFFFF800000000000
#define VMAP_VA_BASE		0xFFFF900000000000
#define PCPU_VA_BASE		0xFFFFA00000000000
#define FIXMAP_VA_BASE		0xFFFFB00000000000

#define DEV_BASE(pa)		(DEV_VA_BASE + (pa))

#ifndef _in_asm_source_
#define PA2VA(pa)		((void *)((UBINT)(pa) + KVA_BASE))
#define VA2PA(va)		((UBINT)(va) - KVA_BASE)
#endif

/* Translation control */
#define TCR_EL1_VALUE		0x12B5103510	/* T0SZ=T1SZ=16, 4KB, WBWA, inner shareable, IPS=40bit, AS=1 */
#define MAIR_EL1_VALUE		0x000000BBFF440400	/* idx0 nGnRnE, 1 nGnRE, 2 Normal NC, 3 Normal WB, 4 Normal WT */
#define MAIR_IDX_DEV_nGnRnE	0
#define MAIR_IDX_DEV_nGnRE	1
#define MAIR_IDX_NORMAL_NC	2
#define MAIR_IDX_NORMAL_WB	3
#define MAIR_IDX_NORMAL_WT	4

/* Descriptor bits */
#define PTE_VALID		(1 << 0)
#define PTE_TABLE		(1 << 1)	/* L0-L2: next level table; L3: page */
#define PTE_ATTR(idx)		((idx) << 2)
#define PTE_NS			(1 << 5)
#define PTE_AP_RW_EL1		(0 << 6)
#define PTE_AP_RW_ALL		(1 << 6)
#define PTE_AP_RO_EL1		(2 << 6)
#define PTE_AP_RO_ALL		(3 << 6)
#define PTE_SH_NONE		(0 << 8)
#define PTE_SH_OUTER		(2 << 8)
#define PTE_SH_INNER		(3 << 8)
#define PTE_AF			(1 << 10)
#define PTE_nG			(1 << 11)
/* 64 bit constants usable from both C and the assembler */
#ifdef _in_asm_source_
#define U64(x)			x
#else
#define U64(x)			x##ULL
#endif

#define PTE_PXN			(U64(1) << 53)
#define PTE_UXN			(U64(1) << 54)
#define PTE_ADDR_MASK		U64(0x0000FFFFFFFFF000)

/* Block descriptors (kernel only, EL0 never executes them). W^X: text is RO+X, everything else RW+XN */
#define PTE_BLOCK_NORMAL	(PTE_VALID | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RW_EL1 | PTE_SH_INNER | PTE_AF | PTE_UXN)
#define PTE_BLOCK_NORMAL_XN	(PTE_BLOCK_NORMAL | PTE_PXN)
#define PTE_BLOCK_TEXT		(PTE_VALID | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_SH_INNER | PTE_AF | PTE_UXN)
#define PTE_BLOCK_DEVICE	(PTE_VALID | PTE_ATTR(MAIR_IDX_DEV_nGnRE) | PTE_AP_RW_EL1 | PTE_SH_NONE | PTE_AF | PTE_PXN | PTE_UXN)
#define PTE_TABLE_DESC		(PTE_VALID | PTE_TABLE)

#define KERNEL_BLOCK_SIZE	0x200000	/* the kernel image is mapped with 2MB blocks (design 5.11) */

/* Page (L3) descriptors */
#define PTE_PAGE		(PTE_VALID | PTE_TABLE)
#define PTE_PAGE_KDATA		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RW_EL1 | PTE_SH_INNER | PTE_AF | PTE_PXN | PTE_UXN)
#define PTE_PAGE_KDATA_NC	(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_NC) | PTE_AP_RW_EL1 | PTE_SH_INNER | PTE_AF | PTE_PXN | PTE_UXN)
#define PTE_PAGE_KTEXT		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_SH_INNER | PTE_AF | PTE_UXN)
#define PTE_PAGE_KRO		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_EL1 | PTE_SH_INNER | PTE_AF | PTE_PXN | PTE_UXN)
#define PTE_PAGE_KDEV		(PTE_PAGE | PTE_ATTR(MAIR_IDX_DEV_nGnRE) | PTE_AP_RW_EL1 | PTE_SH_NONE | PTE_AF | PTE_PXN | PTE_UXN)
/* user (EL0) pages: nG, never executable at EL1 */
#define PTE_PAGE_UDATA		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RW_ALL | PTE_SH_INNER | PTE_AF | PTE_nG | PTE_PXN | PTE_UXN)
#define PTE_PAGE_UTEXT		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_ALL | PTE_SH_INNER | PTE_AF | PTE_nG | PTE_PXN)
#define PTE_PAGE_URO		(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_WB) | PTE_AP_RO_ALL | PTE_SH_INNER | PTE_AF | PTE_nG | PTE_PXN | PTE_UXN)

#define L0_SHIFT		39
#define L1_SHIFT		30
#define L2_SHIFT		21
#define L3_SHIFT		12
#define PT_ENTRIES		512

/* ------------------------------------------------------------------------ */
/*
 * PSCI (SMC64 function IDs)
 */
#define PSCI_VERSION		0x84000000
#define PSCI_CPU_OFF		0x84000002
#define PSCI_CPU_ON		0xC4000003
#define PSCI_SYSTEM_OFF		0x84000008
#define PSCI_SYSTEM_RESET	0x84000009

/* ------------------------------------------------------------------------ */
/*
 * GICv2 (GIC-400) register offsets. Base addresses come from the SoC sysdef.
 */
#define GICD_CTLR		(GICD_BASE + 0x0000)
#define GICD_TYPER		(GICD_BASE + 0x0004)
#define GICD_IIDR		(GICD_BASE + 0x0008)
#define GICD_IGROUPR(n)		(GICD_BASE + 0x0080 + (0x04*(n)))
#define GICD_ISENABLER(n)	(GICD_BASE + 0x0100 + (0x04*(n)))
#define GICD_ICENABLER(n)	(GICD_BASE + 0x0180 + (0x04*(n)))
#define GICD_ISPENDR(n)		(GICD_BASE + 0x0200 + (0x04*(n)))
#define GICD_ICPENDR(n)		(GICD_BASE + 0x0280 + (0x04*(n)))
#define GICD_ISACTIVER(n)	(GICD_BASE + 0x0300 + (0x04*(n)))
#define GICD_ICACTIVER(n)	(GICD_BASE + 0x0380 + (0x04*(n)))
#define GICD_IPRIORITYR(n)	(GICD_BASE + 0x0400 + (0x04*(n)))
#define GICD_ITARGETSR(n)	(GICD_BASE + 0x0800 + (0x04*(n)))
#define GICD_ICFGR(n)		(GICD_BASE + 0x0C00 + (0x04*(n)))
#define GICD_SGIR		(GICD_BASE + 0x0F00)
#define GICD_CPENDSGIR(n)	(GICD_BASE + 0x0F10 + (0x04*(n)))
#define GICD_SPENDSGIR(n)	(GICD_BASE + 0x0F20 + (0x04*(n)))

#define GICC_CTLR		(GICC_BASE + 0x0000)
#define GICC_PMR		(GICC_BASE + 0x0004)
#define GICC_BPR		(GICC_BASE + 0x0008)
#define GICC_IAR		(GICC_BASE + 0x000C)
#define GICC_EOIR		(GICC_BASE + 0x0010)
#define GICC_RPR		(GICC_BASE + 0x0014)
#define GICC_HPPIR		(GICC_BASE + 0x0018)
#define GICC_IIDR		(GICC_BASE + 0x00FC)

#define GICC_IAR_SPURIOUS	1023
#define GIC_PRI_SHIFT		3		/* GIC-400 implements the upper 5 bits of priority */

/* ------------------------------------------------------------------------ */
/*
 * PL011 UART register offsets (used by boot code and T-Monitor)
 */
#define PL011_DR		0x000
#define PL011_FR		0x018
#define PL011_IBRD		0x024
#define PL011_FBRD		0x028
#define PL011_LCR_H		0x02c
#define PL011_CR		0x030
#define PL011_IFLS		0x034
#define PL011_IMSC		0x038
#define PL011_RIS		0x03c
#define PL011_MIS		0x040
#define PL011_ICR		0x044
#define PL011_FR_TXFF		(1 << 5)
#define PL011_FR_RXFE		(1 << 4)
#define PL011_FR_BUSY		(1 << 3)

/* ------------------------------------------------------------------------ */
/*
 * Interrupt priority (API level 0 = highest .. 15 = lowest; GIC-400 uses bits 7:3)
 */
#define INTPRI_BITWIDTH		4
#define INTPRI_SHIFT		(8 - INTPRI_BITWIDTH)
#define INTPRI_HIGHEST		0
#define INTPRI_LOWEST		15
#define INTPRI_SYSTICK		2		/* system timer */
#define INTPRI_IPI		3		/* inter-processor interrupt */
#define INTPRI_DEVICE		8		/* ordinary device drivers */
#define INTNO_SYSTICK		TIMER_INTNO	/* TIMER_INTNO comes from the SoC sysdef */

/* GIC register array lengths for N_INTVEC interrupt IDs */
#define GICD_IGROUPR_N		(N_INTVEC / 32)
#define GICD_ICFGR_N		(N_INTVEC / 16)
#define GICD_IPRIORITYR_N	(N_INTVEC / 4)
#define GICD_ITARGETSR_N	(N_INTVEC / 4)
#define GICD_ICENABLER_N	(N_INTVEC / 32)

/* System timer period range (ms) */
#define MIN_TIMER_PERIOD	1
#define MAX_TIMER_PERIOD	50
#define TIMER_INTLEVEL		0

/* Inter-processor interrupts (SGI numbers, design 8.8) */
#define SGI_DISPATCH		0	/* reschedule request */
#define SGI_TIMER		1	/* per-CPU timer reprogramming (reserved) */
#define SGI_CALL		2	/* function call request (reserved) */
#define SGI_STOP		3	/* stop request */


/* Task system stack: the exception frame (272) + FP context (528) + C frames */
#define MIN_SYS_STACK_SIZE	2048
#define DEFAULT_SYS_STKSZ	4096

/* CPU features */
#define CPU_HAS_FPU		1
#define CPU_HAS_DSP		0
#define CPU_HAS_PTMR		0		/* physical timer API: later phase */
#if USE_FPU
#define NUM_COPROCESSOR		1
#else
#define NUM_COPROCESSOR		0
#endif
#define FPCR_INIT		0x00000000	/* FPCR initial value (round to nearest, no traps) */

/* ------------------------------------------------------------------------ */
/*
 * Exception vector numbers used inside the kernel
 */
#define SVC_SYSCALL		6		/* T-Kernel system call (SVC #6) */
#define SVC_FORCE_DISPATCH	7
#define SVC_DISPATCH		8
#define SVC_DEBUG_SUPPORT	9
#define SVC_EXTENDED_SVC	10
#define N_SVCHDR		(11)

#endif /* __SYS_SYSDEF_DEPEND_CORE_H__ */
