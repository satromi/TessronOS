/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	Machine type definition (ARMv8-A / AArch64 core)
 */

#ifndef __SYS_MACHINE_CORE_H__
#define __SYS_MACHINE_CORE_H__

/*
 * CPU_xxxx	CPU type
 * ALLOW_MISALIGN	1 if access to misalignment data is allowed
 * BIGENDIAN		1 if big endian
 * INT_BITWIDTH		bit width of INT/UINT (TessronOS keeps INT at 32 bit; see design 5.2)
 * PTR_BITWIDTH		bit width of pointers and BINT/UBINT/SZ
 */

#define ALLOW_MISALIGN		0
#define INT_BITWIDTH		32
#define PTR_BITWIDTH		64
#define BIGENDIAN		0			/* Little Endian */

/* Data cache line size of Cortex-A76 (L1/L2) */
#define CACHE_LINE_SIZE		64

#endif /* __SYS_MACHINE_CORE_H__ */
