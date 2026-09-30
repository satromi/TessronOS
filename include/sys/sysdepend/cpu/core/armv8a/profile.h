/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	profile.h
 *	Service Profile (ARMv8-A / AArch64 core)
 */

#ifndef __SYS_PROFILE_CORE_H__
#define __SYS_PROFILE_CORE_H__

/*
 **** CPU-depended profile (ARMv8-A)
 */

#define TK_ALLOW_MISALIGN	(ALLOW_MISALIGN)	/* Memory misalign access is permitted */
#define TK_BIGENDIAN		(BIGENDIAN)		/* Is Big Endian (Must be defined) */

#define TK_SUPPORT_FPU		(USE_FPU)		/* Support of FPU/SIMD */
#define TK_SUPPORT_COP0		FALSE			/* Support of co-processor-0 */
#define TK_SUPPORT_COP1		FALSE			/* Support of co-processor-1 */
#define TK_SUPPORT_COP2		FALSE			/* Support of co-processor-2 */
#define TK_SUPPORT_COP3		FALSE			/* Support of co-processor-3 */

#define TK_SUPPORT_REGOPS	TRUE			/* Support of get/set register operation */
#define TK_SUPPORT_ASM		FALSE			/* Support of assembly language function entry/exit */

#define TK_SUPPORT_INTCTRL	TRUE			/* Support of interrupt controller management. */
#define TK_HAS_ENAINTLEVEL	TRUE 			/* Can specify interrupt priority level */
#define TK_SUPPORT_CPUINTLEVEL	FALSE			/* Support of get/set of CPU interrupt mask level */
#define TK_SUPPORT_CTRLINTLEVEL	TRUE			/* Support of get/set of interrupt controller interrupt mask level */
#define TK_SUPPORT_INTMODE	TRUE			/* Supoprt of interrupt mode setting */

#define TK_SUPPORT_CACHECTRL	TRUE			/* Support of cache control */
#define TK_SUPPORT_SETCACHEMODE	FALSE			/* Support of set cache mode */
#define TK_SUPPORT_WBCACHE	TRUE			/* Support of write-back cache */
#define TK_SUPPORT_WTCACHE	FALSE			/* Support of write-through cache */

#define TK_MEM_RNG0		0
#define TK_MEM_RNG1		0
#define TK_MEM_RNG2		0
#define TK_MEM_RNG3		0

#define TK_SUPPORT_MICROWAIT	TRUE			/* Support of micro wait */

#endif /* __SYS_PROFILE_CORE_H__ */
