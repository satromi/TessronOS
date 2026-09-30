/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	v3d_regs.h
 *	Registers of the V3D (docs/tessronos/13-gpu.md 13.2).
 *
 *	The V3D has a hub (MMU, texture formatting unit, the interrupts of
 *	both) and one or more cores (control list executors, caches, the
 *	core's interrupts). On the BCM2712 the hub, core 0 and the power
 *	state manager (SMS) are three windows the device tree names "hub",
 *	"core0" and "sms" (include/sys/sysdepend/cpu/bcm2712/sysdef.h).
 *
 *	Every offset and bit here carries one of two marks:
 *	  [doc]	a published layout (13-gpu.md 13.2 says which).
 *	  [hw]	no published layout; to be read back on the board before
 *		anything depends on it (13-gpu.md 13.8). The driver writes
 *		these only at CNF_V3D >= 2.
 *	Nothing is written to the SMS window.
 */

#ifndef _DEVICE_GPU_V3D_REGS_H_
#define _DEVICE_GPU_V3D_REGS_H_

/* ---------------------------------------------------------------- core */

/* [doc] identification. IDENT0 bits 23:0 read "V3D", 31:24 the major version */
#define V3D_CTL_IDENT0		0x0000
#define V3D_CTL_IDENT1		0x0004	/* 3:0 minor, 7:4 slices, 11:8 QPUs a slice, 31:28 VPM in 8KB */
#define V3D_CTL_IDENT2		0x0008
#define V3D_IDENT0_MAGIC	0x00443356U	/* 'V' '3' 'D' from the lowest byte */
#define V3D_IDENT0_MAGIC_MASK	0x00FFFFFFU

/* [hw] caches of the core */
#define V3D_CTL_L2CACTL		0x0020
#define V3D_CTL_SLCACTL		0x0024	/* writing ones invalidates the slice caches */
#define V3D_CTL_L2TCACTL	0x0030
#define   V3D_L2TCACTL_L2TFLS	0x00000001U	/* start; reads 1 until done */
#define   V3D_L2TCACTL_FLM_SHIFT 1
#define   V3D_L2TCACTL_FLM_FLUSH 0		/* invalidate */
#define   V3D_L2TCACTL_FLM_CLEAR 1		/* write back and invalidate */
#define   V3D_L2TCACTL_FLM_CLEAN 2		/* write back */
#define   V3D_L2TCACTL_TMUWCF	0x00000100U	/* flush the TMU write combiner; reads 1 until done */
#define V3D_CTL_L2TFLSTA	0x0034
#define V3D_CTL_L2TFLEND	0x0038

/* [hw] interrupts of the core */
#define V3D_CTL_INT_STS		0x0050
#define V3D_CTL_INT_SET		0x0054
#define V3D_CTL_INT_CLR		0x0058
#define V3D_CTL_INT_MSK_STS	0x005c
#define V3D_CTL_INT_MSK_SET	0x0060	/* ones mask */
#define V3D_CTL_INT_MSK_CLR	0x0064	/* ones unmask */
#define   V3D_INT_FRDONE	0x00000001U	/* renderer frame done */
#define   V3D_INT_FLDONE	0x00000002U	/* binner flush done */
#define   V3D_INT_OUTOMEM	0x00000004U	/* binner out of tile memory */
#define   V3D_INT_CSDDONE_71	0x00000040U	/* compute dispatch done (7.1) */

/* [doc] control list executors: thread 0 is the binner, thread 1 the renderer */
#define V3D_CLE_CT0CS		0x0100
#define V3D_CLE_CT1CS		0x0104
#define   V3D_CLE_CTCS_CTERR	0x00000008U
#define   V3D_CLE_CTCS_CTRUN	0x00000020U
#define   V3D_CLE_CTCS_CTRSTA	0x00008000U	/* reset the thread */
#define V3D_CLE_CT0EA		0x0108
#define V3D_CLE_CT1EA		0x010c
#define V3D_CLE_CT0CA		0x0110
#define V3D_CLE_CT1CA		0x0114
#define V3D_CLE_PCS		0x0130
#define V3D_CLE_BFC		0x0134	/* 7:0 binner flushes counted */
#define V3D_CLE_RFC		0x0138	/* 7:0 frames rendered counted */

/* [hw] the queued form the later generations start a list with */
#define V3D_CLE_CT0QTS		0x015c
#define   V3D_CLE_CT0QTS_ENABLE	0x00000002U
#define V3D_CLE_CT0QBA		0x0160	/* start address; writing the end starts it */
#define V3D_CLE_CT1QBA		0x0164
#define V3D_CLE_CT0QEA		0x0168
#define V3D_CLE_CT1QEA		0x016c
#define V3D_CLE_CT0QMA		0x0170	/* tile allocation memory */
#define V3D_CLE_CT0QMS		0x0174

/* [doc] primitive tile binner overflow memory */
#define V3D_PTB_BPCA		0x0300
#define V3D_PTB_BPCS		0x0304
#define V3D_PTB_BPOA		0x0308
#define V3D_PTB_BPOS		0x030c

/* ---------------------------------------------------------------- hub */

/*
 * [hw] identification of the hub. Read on the Raspberry Pi 5 (13-gpu.md
 * 13.8.1): IDENT0 42554856 ("VHUB"), IDENT1 00081117, IDENT2 00001900,
 * IDENT3 00000700. IDENT1 3:0 and 7:4 repeat the major and minor version
 * of the core (7 and 1), so 11:8 is the number of cores (1).
 */
#define V3D_HUB_IDENT0		0x0008
#define   V3D_HUB_IDENT0_MAGIC	0x42554856U	/* 'V' 'H' 'U' 'B' from the lowest byte */
#define V3D_HUB_IDENT1		0x000c	/* 3:0 major, 7:4 minor, 11:8 cores */
#define   V3D_HUB_IDENT1_NCORES(v)	( ( (v) >> 8 ) & 0xF )
#define V3D_HUB_IDENT2		0x0010	/* 8: has an MMU */
#define   V3D_HUB_IDENT2_WITH_MMU	0x00000100U
#define V3D_HUB_IDENT3		0x0014	/* 15:8 revision, 23:16 compatible revision [doc] */

/* [hw] interrupts of the hub */
#define V3D_HUB_INT_STS		0x0050
#define V3D_HUB_INT_SET		0x0054
#define V3D_HUB_INT_CLR		0x0058
#define V3D_HUB_INT_MSK_STS	0x005c
#define V3D_HUB_INT_MSK_SET	0x0060
#define V3D_HUB_INT_MSK_CLR	0x0064
#define   V3D_HUB_INT_TFUC	0x00000002U	/* texture formatting done */
#define   V3D_HUB_INT_MMU_CAP	0x00000008U	/* address beyond the cap */
#define   V3D_HUB_INT_MMU_PTI	0x00000010U	/* page table entry not valid */
#define   V3D_HUB_INT_MMU_WRV	0x00000020U	/* write to a page not writable */
#define   V3D_HUB_INT_GMPV_71	0x00000040U	/* memory protection violation (7.1) */

/* [hw] the MMU and its cache */
#define V3D_MMUC_CONTROL	0x1000
#define   V3D_MMUC_ENABLE	0x00000001U
#define   V3D_MMUC_FLUSH	0x00000004U
#define   V3D_MMUC_FLUSHING	0x00000008U
#define V3D_MMU_CTL		0x1200
#define   V3D_MMU_CTL_ENABLE	0x00000001U
#define   V3D_MMU_CTL_TLB_CLEAR	0x00000004U
#define   V3D_MMU_CTL_TLB_CLEARING 0x00000080U
#define V3D_MMU_PT_PA_BASE	0x1204	/* physical address of the table >> 12 */
#define V3D_MMU_ADDR_CAP	0x1214
#define V3D_MMU_VIO_ID		0x122c
#define V3D_MMU_ILLEGAL_ADDR	0x1230	/* page that invalid accesses go to >> 12, 31: enable */
#define   V3D_MMU_ILLEGAL_ENABLE 0x80000000U
#define V3D_MMU_VIO_ADDR	0x1234
#define V3D_MMU_DEBUG_INFO	0x1238	/* 7:4 and 11:8: address widths less 30 */
#define   V3D_MMU_VA_WIDTH(v)	( 30 + ( ( (v) >> 4 ) & 0xF ) )
#define   V3D_MMU_PA_WIDTH(v)	( 30 + ( ( (v) >> 8 ) & 0xF ) )

/* ---------------------------------------------------------------- page table */

/*
 * [hw] One 32 bit entry per 4KB page of GPU address space, indexed by
 * the address >> 12; 1M entries (4MB) for the whole 4GB. The low 28
 * bits are the physical page number, which reaches 1TB.
 */
#define V3D_PTE_VALID		0x10000000U
#define V3D_PTE_WRITEABLE	0x20000000U
#define V3D_PTE_BIGPAGE		0x40000000U
#define V3D_PTE_SUPERPAGE	0x80000000U
#define V3D_PTE_PFN_MASK	0x0FFFFFFFU

#endif /* _DEVICE_GPU_V3D_REGS_H_ */
