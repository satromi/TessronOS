/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_regs.h
 *	The registers and commands of the render and copy engines of
 *	Intel's integrated graphics of the 9th generation (Skylake, Kaby
 *	Lake, Amber Lake), as the driver (igt.c) uses them. The display
 *	engine's registers are not here: they belong to the display side
 *	(docs/private/x64_surface_go2_study.md 5.13, 5.14).
 *
 *	Offsets are from the start of the first BAR (GTTMMADR). A "masked"
 *	register takes in its upper half which of the lower bits a write
 *	changes.
 */

#ifndef _DEVICE_GPU_INTEL_IGT_REGS_H_
#define _DEVICE_GPU_INTEL_IGT_REGS_H_

#define MASK_EN(b)		( ( (UW)(b) << 16 ) | (UW)(b) )
#define MASK_DIS(b)		( (UW)(b) << 16 )
#define MASK_FIELD(m, v)	( ( (UW)(m) << 16 ) | (UW)(v) )

/* ---------------------------------------------------------------- PCI configuration */
#define IGT_PCI_GGC		0x50	/* GMCH graphics control: GGMS 7:6, GMS 15:8 */
#define IGT_PCI_BDSM		0x5c	/* base of the stolen memory */
#define IGT_PCI_ASLS		0xfc	/* the OpRegion */

#define IGT_GTT_OFFSET		( 8UL * 1024 * 1024 )	/* the global GTT's entries in the first BAR */

/* ---------------------------------------------------------------- forcewake and power */
#define FW_RENDER		0xa278
#define FW_RENDER_ACK		0x0d84
#define FW_GT			0xa188	/* the blitter and the rest of the GT */
#define FW_GT_ACK		0x130044
#define FW_MEDIA		0xa270
#define FW_MEDIA_ACK		0x0d88
#define  FW_KERNEL		0x0001
#define  FW_FALLBACK		0x8000

#define RC_CONTROL		0xa090
#define  RC_CTL_RC6_ENABLE	0x00040000
#define  RC_CTL_EI_MODE(x)	( (UW)(x) << 27 )
#define  RC_CTL_HW_ENABLE	0x80000000
#define RC_STATE		0xa094
#define  RC_SW_TARGET(s)	( (UW)(s) << 16 )	/* 4: RC6 */
#define RC6_WAKE_RATE_LIMIT	0xa09c
#define RC_EVALUATION_INTERVAL	0xa0a8
#define RC_IDLE_HYSTERSIS	0xa0ac
#define RC_SLEEP		0xa0b0
#define RC6_THRESHOLD		0xa0b8
#define MEDIA_PG_IDLE_HYSTERESIS 0xa0c4
#define RENDER_PG_IDLE_HYSTERESIS 0xa0c8
#define PG_ENABLE		0xa210
#define  RENDER_PG_EN		0x0001
#define  MEDIA_PG_EN		0x0002
#define GUC_MAX_IDLE_COUNT	0xc3e4
#define RC6_CTX_INFO		0x8504	/* 0: the context of RC6 was found corrupted */
#define GT_CORE_STATUS		0x138060	/* 2:0 the state now: 0 RC0, 3 RC6 */
#define GT_GFX_RC6		0x138108	/* time in RC6, units of 1.28 us */

#define RPNSWREQ		0xa008	/* the frequency asked for, units of 50/3 MHz in 31:23 */
#define  FREQ_REQ(v)		( (UW)(v) << 23 )
#define RC_VIDEO_FREQ		0xa00c
#define RP_INTERRUPT_LIMITS	0xa014	/* the highest 31:23, the lowest 22:14 */
#define RPSTAT1			0xa01c	/* the frequency now, 31:23 */
#define RP_CONTROL		0xa024
#define  RP_MEDIA_TURBO		0x0800
#define  RP_MEDIA_HW_NORMAL_MODE ( 2 << 9 )
#define  RP_MEDIA_IS_GFX	0x0100
#define  RP_ENABLE		0x0080
#define  RP_UP_BUSY_AVG		( 2 << 3 )
#define  RP_DOWN_IDLE_AVG	( 2 << 0 )
#define RP_UP_THRESHOLD		0xa02c
#define RP_DOWN_THRESHOLD	0xa030
#define RP_UP_EI		0xa068
#define RP_DOWN_EI		0xa06c
#define RP_IDLE_HYSTERSIS	0xa070
#define PMINTRMSK		0xa168
#define  PMINTR_REDIRECT_TO_GUC_DIS	0x80000000	/* must be 0 on this generation */
#define  PM_RP_DOWN_TIMEOUT	0x0040
#define  PM_RP_UP_THRESHOLD	0x0020
#define  PM_RP_DOWN_THRESHOLD	0x0010
#define  PM_RP_UP_EI_EXPIRED	0x0004
#define RP_STATE_CAP		0x145998	/* RP0 7:0, RP1 15:8, RPn 23:16, units of 50 MHz */
#define FREQ_SCALE		3	/* 50 MHz in units of 50/3 MHz */

#define MCR_SELECTOR		0xfdc
#define  MCR_SLICE(s)		( ( (UW)(s) & 3 ) << 26 )
#define  MCR_SUBSLICE(ss)	( ( (UW)(ss) & 3 ) << 24 )

#define FUSE2			0x9120
#define  F2_S_ENA(v)		( ( (v) >> 25 ) & 7 )
#define  F2_SS_DIS(v)		( ( (v) >> 20 ) & 0xf )
#define EU_DISABLE(s)		( 0x9134 + (s) * 4 )

#define GDRST			0x941c
#define  GRDOM_RENDER		0x0002
#define  GRDOM_BLT		0x0008

#define GFX_FLSH_CNTL		0x101008

#define PPAT_LO			0x40e0
#define PPAT_HI			0x40e4
#define  PPAT_UC		0
#define  PPAT_WC		1
#define  PPAT_WT		2
#define  PPAT_WB		3
#define  PPAT_ELLC_OVERRIDE	( 0 << 2 )
#define  PPAT_LLC		( 1 << 2 )
#define  PPAT_LLCELLC		( 2 << 2 )
#define  PPAT_AGE(a)		( (a) << 4 )

#define GFX_MOCS(i)		( 0xc800 + (i) * 4 )
#define BLT_MOCS(i)		( 0xcc00 + (i) * 4 )
#define LNCFCMOCS(i)		( 0xb020 + (i) * 4 )
#define  LE_PAGETABLE		0
#define  LE_UC			1
#define  LE_WB			3
#define  LE_TC_PAGETABLE	( 0 << 2 )
#define  LE_TC_LLC		( 1 << 2 )
#define  LE_TC_LLC_ELLC		( 2 << 2 )
#define  LE_LRUM(v)		( (v) << 4 )
#define  L3_UC			( 1 << 4 )
#define  L3_WB			( 3 << 4 )

/* Faults and errors, read when something goes wrong */
#define RING_FAULT_REG		0x4094
#define ERROR_GEN6		0x40a0
#define FAULT_TLB_DATA0		0x4b10
#define FAULT_TLB_DATA1		0x4b14

/* ---------------------------------------------------------------- interrupts */
#define MASTER_IRQ		0x44200
#define  MASTER_IRQ_CONTROL	0x80000000
#define  MASTER_GT0		0x00000003	/* GT interrupt register 0: the render (0) and copy (1) engines */
#define  MASTER_GT1		0x0000000c	/* 1: the video engines */
#define  MASTER_GT2		0x00000030	/* 2: power management (4) and the GuC (5) */
#define  MASTER_GT3		0x00000040	/* 3: the video enhancement engine */
#define  MASTER_DISPLAY		0x007f0000	/* the display's: pipes, ports, the rest */
#define  MASTER_PCU		0x40000000
#define GT_ISR(n)		( 0x44300 + (n) * 0x10 )
#define GT_IMR(n)		( 0x44304 + (n) * 0x10 )
#define GT_IIR(n)		( 0x44308 + (n) * 0x10 )
#define GT_IER(n)		( 0x4430c + (n) * 0x10 )
#define  GT_RCS_SHIFT		0
#define  GT_BCS_SHIFT		16
#define  GT_USER		0x0001	/* MI_USER_INTERRUPT */
#define  GT_CS_ERROR		0x0008
#define  GT_CTX_SWITCH		0x0100	/* a context status was written */
/* read only, for the record: what the display asked for */
#define DE_PIPE_IER(p)		( 0x4440c + (p) * 0x10 )
#define DE_PORT_IER		0x4444c
#define DE_MISC_IER		0x4446c
#define PCU_IER			0x444ec

/* ---------------------------------------------------------------- an engine, from its base */
#define RCS_BASE		0x02000
#define BCS_BASE		0x22000
#define VCS_BASE		0x12000	/* the video engine: not driven, its idle time for RC6 only */
#define VECS_BASE		0x1a000	/* the video enhancement engine: likewise */

#define RING_TAIL		0x030
#define RING_HEAD		0x034
#define RING_START		0x038
#define RING_CTL		0x03c
#define  RING_VALID		0x0001
#define RING_MAX_IDLE		0x054
#define RING_ACTHD_UDW		0x05c
#define RING_IPEIR		0x064
#define RING_IPEHR		0x068
#define RING_INSTDONE		0x06c
#define RING_ACTHD		0x074
#define RING_HWS_PGA		0x080
#define RING_NOPID		0x094
#define RING_HWSTAM		0x098
#define RING_MI_MODE		0x09c
#define  STOP_RING		0x0100
#define  MODE_IDLE		0x0200
#define RING_IMR		0x0a8
#define RING_EIR		0x0b0
#define RING_EMR		0x0b4
#define RING_ESR		0x0b8
#define RING_RESET_CTL		0x0d0
#define  RESET_REQUEST		0x0001
#define  RESET_READY		0x0002
#define  RESET_CAT_ERROR	0x0004
#define RING_BBADDR		0x140
#define RING_BBADDR_UDW		0x168
#define RING_ELSP		0x230
#define RING_EXECLIST_LO	0x234
#define RING_EXECLIST_HI	0x238
#define RING_CONTEXT_CONTROL	0x244
#define RING_MODE		0x29c
#define  RUN_LIST_ENABLE	0x8000
#define RING_TIMESTAMP		0x358	/* and its upper half at 0x35c */
#define RING_CSB_LO(i)		( 0x370 + (i) * 8 )
#define RING_CSB_HI(i)		( 0x374 + (i) * 8 )
#define RING_CSB_PTR		0x3a0	/* write pointer 2:0, read pointer 10:8 */
#define RING_FORCE_TO_NONPRIV(i) ( 0x4d0 + (i) * 4 )
#define  NONPRIV_SLOTS		12

#define CSB_ENTRIES		6
#define  CSB_IDLE_ACTIVE	0x0001
#define  CSB_PREEMPTED		0x0002
#define  CSB_ELEMENT_SWITCH	0x0004
#define  CSB_ACTIVE_IDLE	0x0008
#define  CSB_COMPLETE		0x0010
#define  CSB_LITE_RESTORE	0x8000

/* ---------------------------------------------------------------- workaround registers */
#define FF_SLICE_CHICKEN	0x2088
#define  FF_CL_PROVOKING_FIX	0x0002
#define CHICKEN3_3D		0x2090
#define  SF_PROVOKING_FIX	0x1000
#define CSFE_CHICKEN1_RCS	0x20d4
#define  PREEMPT_GPGPU_SYNC_SWITCH_DIS	0x0004
#define FF_SLICE_CS_CHICKEN1	0x20e0
#define  FFSC_PERCTX_PREEMPT_CTRL	0x4000
#define CTX_PREEMPT_REG		0x2248
#define CS_CHICKEN1		0x2580
#define  PREEMPT_3D_OBJECT_LEVEL	0x0001
#define  PREEMPT_GPGPU_LEVEL_MASK	0x0006
#define  PREEMPT_GPGPU_COMMAND_LEVEL	0x0004
#define GAM_ECOCHK		0x4090
#define  ECOCHK_DIS_TLB		0x00000100
#define  DISABLE_HDC_INVALIDATION	0x02000000
#define GAMT_ECO_REG_RW_IA	0x4ab0
#define  GAMT_IN_PLACE_DECOMPRESS	0x00040000
#define GAMT_CHKN_BIT_REG	0x4ab8
#define  GAMT_DYNAMIC_CREDIT_SHARING_DIS	0x10000000
#define MMCD_MISC_CTRL		0x4ddc
#define  MMCD_PCLA		0x80000000
#define  MMCD_HOTSPOT_EN	0x08000000
#define CACHE_MODE_1		0x7004
#define  STC_4X4_OPT_DIS	0x0040
#define  PARTIAL_RESOLVE_IN_VC_DIS	0x0002
#define COMMON_SLICE_CHICKEN2	0x7014
#define  PBE_COMPRESSED_HASH_SEL	0x2000
#define  GATHER_AT_SET_SHADER_DIS	0x1000
#define  SBE_REPLAY_BUF_OPT_DIS		0x0100
#define HDC_CHICKEN0		0x7300
#define  HDC_FORCE_CSR_NON_COHERENT_OVR_DIS	0x8000
#define  HDC_FORCE_CTX_SR_NON_COHERENT		0x0020
#define  HDC_FORCE_NON_COHERENT			0x0010
#define HDC_CHICKEN1		0x7304
#define UCGCTL1			0x9400
#define  GAMUNIT_CLOCK_GATE_DIS	0x00400000
#define UCGCTL4			0x940c
#define  EU_GAUNIT_CLOCK_GATE_DIS	0x00004000
#define UCGCTL6			0x9430
#define  SDEUNIT_CLOCK_GATE_DIS	0x00004000
#define GARBCNTL		0xb004
#define  GAPS_TSV_CREDIT_DIS	0x00000080
#define SCRATCH_LNCF1		0xb008
#define  LNCF_NONIA_COHERENT_ATOMICS	0x00000001
#define L3SQCREG4		0xb118
#define  LQSC_FLUSH_COHERENT_LINES	0x00200000
#define  LQSQ_NONIA_COHERENT_ATOMICS	0x00400000
#define SCRATCH1		0xb11c
#define  LBS_SLA_RETRY_TIMER_DEC	0x00000004
#define  EVICTION_PERF_FIX		0x00000100
#define HALF_SLICE_CHICKEN1	0xe100
#define  SBE_SS_CACHE_DISPATCH_PORT_SHARING_DIS	0x0010
#define HALF_SLICE_CHICKEN2	0xe180
#define  ST_PO_DIS		0x2000
#define HALF_SLICE_CHICKEN3	0xe184
#define  SAMPLER_POWER_BYPASS_DIS	0x0002
#define HALF_SLICE_CHICKEN5	0xe188
#define  CCS_TLB_PREFETCH_EN	0x0008
#define HALF_SLICE_CHICKEN7	0xe194
#define  SAMPLER_HASH_COMPRESSED_READ_ADDR	0x0100
#define  YV12_BUGFIX		0x0010
#define  GPGPU_PREEMPTION	0x0004
#define ROW_CHICKEN		0xe4f0
#define  FLOW_CONTROL_EN	0x8000
#define  PARTIAL_INST_SHOOTDOWN_DIS	0x0100

/* ---------------------------------------------------------------- the logical ring context */
/*
 * The image of a context: a page the engine writes its own status to
 * (the per-process status page), then the registers the engine loads
 * when the context comes in and saves when it goes out, laid as a list
 * of MI_LOAD_REGISTER_IMM. The dword indices of the values are fixed.
 */
#define LRC_STATE_PAGE		1
#define CTX_CONTEXT_CONTROL	( 0x02 + 1 )
#define CTX_RING_HEAD		( 0x04 + 1 )
#define CTX_RING_TAIL		( 0x06 + 1 )
#define CTX_RING_START		( 0x08 + 1 )
#define CTX_RING_CTL		( 0x0a + 1 )
#define CTX_BB_STATE		( 0x10 + 1 )
#define CTX_TIMESTAMP		( 0x22 + 1 )
#define CTX_PDP0_UDW		( 0x30 + 1 )
#define CTX_PDP0_LDW		( 0x32 + 1 )
#define CTX_RING_MI_MODE	0x54	/* the render engine's */
#define  CTX_CTRL_RESTORE_INHIBIT	0x0001
#define  CTX_CTRL_RS_CTX_ENABLE		0x0002
#define  CTX_CTRL_SAVE_INHIBIT		0x0004
#define  CTX_CTRL_INHIBIT_SYN_SWITCH	0x0008
#define  BB_STATE_PPGTT		0x0020
#define LRC_SCRATCH		0x34	/* a dword of the status page commands may write to */

#define CTX_DESC_VALID		0x001
#define CTX_DESC_FORCE_PD	0x002
#define CTX_DESC_FORCE_RESTORE	0x004
#define CTX_DESC_64B		( 3 << 3 )	/* 48 bit, four level page tables */
#define CTX_DESC_PRIVILEGE	0x100

/* The engine's own status page (RING_HWS_PGA): dword indices */
#define HWS_CSB0		0x10
#define HWS_CSB_WRITE		0x1f
#define HWS_SEQNO		0x40

/* ---------------------------------------------------------------- page tables */
#define GPTE_PRESENT		0x001ULL
#define GPTE_RW			0x002ULL
#define GPTE_PWT			0x008ULL
#define GPTE_PCD			0x010ULL
#define GPTE_PAT			0x080ULL
#define GPTE_ADDR		0x0000007ffffff000ULL	/* 39 bits of physical address */
#define GPTE_CACHED		GPTE_PAT			/* PPAT 4: write back, LLC and eLLC */
#define GPTE_UNCACHED		( GPTE_PCD | GPTE_PWT )	/* PPAT 3 */
#define GPDE_CACHED		0			/* PPAT 0: write back, LLC */

/* ---------------------------------------------------------------- commands */
#define MI(op, len)		( ( (UW)(op) << 23 ) | (UW)(len) )
#define MI_NOOP			MI(0x00, 0)
#define MI_USER_INTERRUPT	MI(0x02, 0)
#define MI_ARB_CHECK		MI(0x05, 0)
#define MI_ARB_ON_OFF		MI(0x08, 0)
#define  MI_ARB_ENABLE		1
#define  MI_ARB_DISABLE		0
#define MI_BATCH_BUFFER_END	MI(0x0a, 0)
#define MI_STORE_DWORD_IMM	MI(0x20, 2)	/* + address low, high, value */
#define MI_STORE_QWORD_IMM	MI(0x20, 3)
#define  MI_USE_GGTT		( 1 << 22 )
#define MI_LRI(n)		MI(0x22, 2 * (n) - 1)
#define  MI_LRI_FORCE_POSTED	( 1 << 12 )
#define MI_STORE_REGISTER_MEM	MI(0x24, 2)	/* + register, address low, high */
#define MI_FLUSH_DW		MI(0x26, 2)	/* + address low, high, value */
#define  FLUSH_DW_STORE_INDEX	( 1 << 21 )
#define  FLUSH_DW_INVALIDATE_TLB ( 1 << 18 )
#define  FLUSH_DW_OP_STOREDW	( 1 << 14 )
#define  FLUSH_DW_USE_GTT	( 1 << 2 )	/* in the address */
#define MI_BATCH_BUFFER_START	MI(0x31, 1)	/* + address low, high */
#define  MI_BATCH_PPGTT		( 1 << 8 )

#define PIPE_CONTROL		( ( 3U << 29 ) | ( 3U << 27 ) | ( 2U << 24 ) | ( 6 - 2 ) )
#define  PC_FLUSH_L3		( 1 << 27 )
#define  PC_GLOBAL_GTT		( 1 << 24 )
#define  PC_STORE_DATA_INDEX	( 1 << 21 )
#define  PC_CS_STALL		( 1 << 20 )
#define  PC_TLB_INVALIDATE	( 1 << 18 )
#define  PC_QW_WRITE		( 1 << 14 )
#define  PC_DEPTH_STALL		( 1 << 13 )
#define  PC_RT_FLUSH		( 1 << 12 )
#define  PC_INST_INVALIDATE	( 1 << 11 )
#define  PC_TEX_INVALIDATE	( 1 << 10 )
#define  PC_FLUSH_ENABLE	( 1 << 7 )
#define  PC_DC_FLUSH		( 1 << 5 )
#define  PC_VF_INVALIDATE	( 1 << 4 )
#define  PC_CONST_INVALIDATE	( 1 << 3 )
#define  PC_STATE_INVALIDATE	( 1 << 2 )
#define  PC_STALL_AT_SCOREBOARD	( 1 << 1 )
#define  PC_DEPTH_FLUSH		( 1 << 0 )

/* The copy engine's (two dimensional, linear, 32 bits a pixel) */
#define XY_COLOR_BLT		( ( 2U << 29 ) | ( 0x50U << 22 ) | ( 7 - 2 ) )
#define XY_SRC_COPY_BLT		( ( 2U << 29 ) | ( 0x53U << 22 ) | ( 10 - 2 ) )
#define  BLT_WRITE_RGBA		( 3 << 20 )
#define  BLT_DEPTH_32		( 3 << 24 )
#define  BLT_ROP_SRC_COPY	( 0xcc << 16 )
#define  BLT_ROP_COLOR_COPY	( 0xf0 << 16 )
#define BLT_PITCH_MAX		32767	/* bytes */
#define BLT_COORD_MAX		32767

#endif /* _DEVICE_GPU_INTEL_IGT_REGS_H_ */
