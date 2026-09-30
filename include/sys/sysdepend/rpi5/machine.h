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
 *	Machine type definition (Raspberry Pi 5)
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/*
 * [TYPE]_[CPU]		TARGET SYSTEM
 * RPI5			Raspberry Pi 5 (Broadcom BCM2712, Cortex-A76 ×4)
 */

#define RPI5			1		/* Target system : Raspberry Pi 5 */
#define CPU_BCM2712		1		/* Target SoC : Broadcom BCM2712 */
#define CPU_CORE_ARMV8A		1		/* Target CPU-Core type : ARMv8-A (AArch64) */
#define CPU_CORE_ACA76		1		/* Target CPU-Core : Arm Cortex-A76 */

#define TARGET_DIR		rpi5		/* Sysdepend-Directory name */
#define KNL_SYSDEP_PATH		rpi5		/* Kernel sysdepend path */

#include "../cpu/bcm2712/machine.h"

#endif /* __SYS_SYSDEPEND_MACHINE_H__ */
