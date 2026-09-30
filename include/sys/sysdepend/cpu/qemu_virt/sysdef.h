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
 *	System dependencies definition (QEMU virt SoC model)
 *	Included also from assembler program.
 *
 *	Peripheral addresses are given as physical (_PA). The _BASE names are
 *	the kernel virtual addresses in the device window (DEV_BASE) and are
 *	what the drivers use once the MMU is on.
 */

#ifndef __SYS_SYSDEF_DEPEND_CPU_H__
#define __SYS_SYSDEF_DEPEND_CPU_H__

#include "../core/armv8a/sysdef.h"

/* ------------------------------------------------------------------------ */
/*
 * Physical memory map of the QEMU "virt" machine
 *	RAM starts at 0x4000_0000. The actual size comes from the DTB (-m option).
 */
#define RAM_START		0x40000000
#define RAM_END_DEFAULT		0x80000000		/* -m 1G の場合。DTB で上書きする */

#define INTERNAL_RAM_START	RAM_START
#define INTERNAL_RAM_END	RAM_END_DEFAULT

/* ------------------------------------------------------------------------ */
/*
 * Peripherals (virt machine memory map, physical)
 */
#define GICD_PA			0x08000000		/* GICv2 Distributor */
#define GICC_PA			0x08010000		/* GICv2 CPU interface */
#define GICH_PA			0x08030000
#define GICV_PA			0x08040000

#define PL011_UART0_PA		0x09000000		/* PL011 UART (console) */
#define PL011_UART0_INTNO	(32 + 1)		/* SPI 1 */

#define PL031_RTC_PA		0x09010000		/* PL031 RTC */
#define PL061_GPIO_PA		0x09030000

#define VIRTIO_MMIO_PA		0x0a000000		/* virtio-mmio ×32, 0x200 stride */
#define VIRTIO_MMIO_STRIDE	0x200
#define VIRTIO_MMIO_NUM		32
#define VIRTIO_MMIO_INTNO(n)	(32 + 16 + (n))		/* SPI 16..47 */

/*
 * PCI Express host bridge. The numbers are the ones the machine puts in
 * its device tree; a machine started with highmem-ecam=off moves the
 * configuration space to 0x3f00_0000 instead.
 */
#define PCIE_ECAM_PA		0x4010000000	/* configuration space, one bus per MB */
#define PCIE_ECAM_SIZE		0x10000000	/* 256 buses */
#define PCIE_MMIO_PA		0x10000000	/* what BARs below 4GB are given */
#define PCIE_MMIO_SIZE		0x2eff0000
#define PCIE_MMIO64_PA		0x8000000000
#define PCIE_MMIO64_SIZE	0x8000000000
#define PCIE_IO_PA		0x3eff0000
#define PCIE_IO_SIZE		0x10000
#define PCIE_INTA_SPI		3		/* pin A of slot 0; the four rotate */
/*
 * The frame that turns a message into an interrupt. A card writes the
 * number it wants to one register of this, and the distributor raises
 * that number. Which numbers it may use, the frame itself says.
 */
#define GICV2M_PA		0x08020000
#define GICV2M_SIZE		0x1000


#define DBG_UART_PA		PL011_UART0_PA		/* console used by boot.S (before the MMU) */

/* Kernel virtual addresses (device window) */
#define GICD_BASE		DEV_BASE(GICD_PA)
#define GICC_BASE		DEV_BASE(GICC_PA)
#define PL011_UART0_BASE	DEV_BASE(PL011_UART0_PA)
#define PL031_RTC_BASE		DEV_BASE(PL031_RTC_PA)
#define VIRTIO_MMIO_BASE	DEV_BASE(VIRTIO_MMIO_PA)
#define PCIE_ECAM_BASE		DEV_BASE(PCIE_ECAM_PA)
#define GICV2M_BASE		DEV_BASE(GICV2M_PA)
#define DBG_UART_BASE		DEV_BASE(DBG_UART_PA)

/* ------------------------------------------------------------------------ */
/*
 * Interrupts
 */
#define N_INTVEC		320			/* INTID 0..319 (16 SGI + 16 PPI + 288 SPI) */
#define TIMER_INTNO		30			/* Generic Timer, non-secure EL1 physical (PPI 14) */
#define VTIMER_INTNO		27			/* Generic Timer, virtual (PPI 11) */

/* Generic Timer frequency: read CNTFRQ_EL0 at boot (QEMU default 62.5MHz) */
#define CNTFRQ_DEFAULT		62500000

#endif /* __SYS_SYSDEF_DEPEND_CPU_H__ */
