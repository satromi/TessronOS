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
 *	Machine type definition (QEMU virt board, AArch64)
 */

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

/*
 * [TYPE]_[CPU]		TARGET SYSTEM
 * QEMU_VIRT		QEMU "virt" machine (gic-version=2, cortex-a76)
 */

#define QEMU_VIRT		1		/* Target system : QEMU virt */
#define CPU_QEMU_VIRT		1		/* Target SoC : QEMU virt (PL011, GICv2, virtio-mmio) */
#define CPU_CORE_ARMV8A		1		/* Target CPU-Core type : ARMv8-A (AArch64) */
#define CPU_CORE_ACA76		1		/* Target CPU-Core : Arm Cortex-A76 */

#define TARGET_DIR		qemu_virt	/* Sysdepend-Directory name */
#define KNL_SYSDEP_PATH		qemu_virt	/* Kernel sysdepend path */

#include "../cpu/qemu_virt/machine.h"

#endif /* __SYS_SYSDEPEND_MACHINE_H__ */
