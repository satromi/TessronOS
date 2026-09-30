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
 *	System dependencies definition (Broadcom BCM2712 / Raspberry Pi 5)
 *	Included also from assembler program.
 *
 *	番地と割込み番号は設計書 第4章(DTS と Linux ドライバで照合済み)による。
 *	要実機の項目(付録C HW-05, 06, 09〜11)は実機で確認したら更新する。
 *	_PA は物理番地、_BASE はデバイス窓(DEV_BASE)上のカーネル仮想番地。
 */

#ifndef __SYS_SYSDEF_DEPEND_CPU_H__
#define __SYS_SYSDEF_DEPEND_CPU_H__

#include "../core/armv8a/sysdef.h"

/* ------------------------------------------------------------------------ */
/*
 * Physical memory map
 *	RAM starts at 0. There is a firmware hole just below 1GB, so /memory in
 *	the DTB has two ranges. TF-A BL31 (PSCI runtime) lives in 0x1000-0x7ffff.
 */
#define RAM_START		0x00000000
#define RAM_END_DEFAULT		0x3f700000		/* 第 1 区間の終端(8GB 版の実測)。DTB で上書きする */

#define TFA_BL31_START		0x00001000		/* 予約: PSCI 実行時サービス */
#define TFA_BL31_END		0x00080000
#define BOOT_SCRATCH_START	0x00080000		/* 起動時スクラッチ(初期ページテーブル等) */
#define BOOT_SCRATCH_END	0x00200000

#define INTERNAL_RAM_START	RAM_START
#define INTERNAL_RAM_END	RAM_END_DEFAULT

/* ------------------------------------------------------------------------ */
/*
 * SoC peripherals (physical: 0x10_0000_0000 + soc offset)
 */
#define SOC_PERIPH_PA		0x1000000000

#define PCIE0_PA		0x1000100000		/* pcie0 (unused) */
#define PCIE1_PA		0x1000110000		/* pcie1: external x1 connector */
#define PCIE2_PA		0x1000120000		/* pcie2: RP1 (x4) */
#define MIP0_PA			0x1000130000		/* MSI-X interrupt peripheral for pcie2 */
#define MIP1_PA			0x1000131000		/* MSI-X interrupt peripheral for pcie1 */
#define MIP0_DOORBELL_PCIE	0xfffffff000		/* PCIe-side address written by MSI-X */

/*
 * The windows of pcie2, the link the RP1 sits on (design 4.7).
 *
 * Outbound: the processor reaches PCIe address PCIE2_OUTBOUND_BUS_PA and
 * upwards at physical PCIE2_OUTBOUND_CPU_PA and upwards. The RP1's
 * peripheral window is the first thing in it, which is what decides
 * where RP1_WINDOW_PA is.
 *
 * Inbound: a device below the link reaches physical address P at PCIe
 * address PCIE2_INBOUND_BUS_PA + P. That difference is what every bus
 * master below the RP1 has to add to an address it is handed, so
 * RP1_DMA_BUS_OFFSET is this window rather than a number of its own.
 */
#define PCIE2_OUTBOUND_CPU_PA	0x1f00000000
#define PCIE2_OUTBOUND_BUS_PA	0x00000000
#define PCIE2_OUTBOUND_SIZE	0x100000000		/* 4GB, 32 bit, non-prefetchable */
#define PCIE2_INBOUND_BUS_PA	0x1000000000
#define PCIE2_INBOUND_SIZE	0x1000000000		/* 64GB: all of system memory */
#define PCIE2_INTA_SPI		229			/* INTx of pcie2 is SPI 229 to 232 */
#define PCIE2_INTNO		(32 + 233)		/* the controller's own interrupt */

#define SDHCI_PA		0x1000fff000		/* sdio1 host registers (0x260) */
#define SDHCI_CFG_PA		0x1000fff400		/* sdio1 cfg registers (0x200) */
#define SDHCI_INTNO		(32 + 273)		/* SPI 273 */

#define SYSTIMER_PA		0x107c003000		/* BCM2835-compatible 1MHz system timer */
#define VC_MAILBOX_PA		0x107c013880		/* VideoCore mailbox (property interface, RTC) */
#define VC_MAILBOX_INTNO	(32 + 33)

/*
 * The V3D (GPU, docs/tessronos/13-gpu.md): hub, core 0 and the power
 * state manager, and the interrupts of the hub and of core 0 (level).
 */
#define V3D_HUB_PA		0x1002000000		/* 16KB */
#define V3D_CORE0_PA		0x1002008000		/* 24KB */
#define V3D_SMS_PA		0x1002030800
#define V3D_HUB_INTNO		(32 + 250)		/* SPI 250 */
#define V3D_CORE0_INTNO		(32 + 249)		/* SPI 249 */

#define PL011_UART10_PA		0x107d001000		/* debug UART (3-pin connector) */
#define PL011_UART10_INTNO	(32 + 121)		/* SPI 121 */
#define PL011_UART10_CLOCK	44000000

#define PM_PA			0x107d200000		/* power management / watchdog */
#define RNG_PA			0x107d208000		/* iproc-rng200 */
#define GIO_AON_PA		0x107d517c00		/* always-on GPIO: bank0 17 bits, bank1 6 bits */
#define GIO_AON_LED_ACT		9			/* bank0 bit 9, active low */
#define GIO_AON_SD_CD		5			/* bank0 bit 5, active low (SD card detect) */

#define GICD_PA			0x107fff9000		/* GIC-400 Distributor */
#define GICC_PA			0x107fffa000		/* GIC-400 CPU interface */
#define GICH_PA			0x107fffc000
#define GICV_PA			0x107fffe000

/*
 * RP1 (through the pcie2 outbound window). Where its peripherals are,
 * and what a bus master below it adds to a physical address, depend on
 * how the windows and the RP1's BAR1 were set up, which is the
 * firmware's doing when the link is taken over from it
 * (pciex4_reset=0). kernel/sysdepend/rpi5/pcie_rc.c reads that at start
 * and sets these two; until then they hold the layout the constants
 * above describe.
 */
#ifndef _in_asm_source_
extern __UINT64_TYPE__	knl_rp1_window_pa;
extern __UINT64_TYPE__	knl_rp1_dma_offset;
#endif
#define RP1_WINDOW_PA		(knl_rp1_window_pa)	/* RP1 peripheral space (BAR1) */
#define RP1_WINDOW_SIZE		0x00400000
#define RP1_UART0_PA		(RP1_WINDOW_PA + 0x030000)
#define RP1_IO_BANK0_PA		(RP1_WINDOW_PA + 0x0d0000)
#define RP1_SYS_RIO0_PA		(RP1_WINDOW_PA + 0x0e0000)
#define RP1_PADS_BANK0_PA	(RP1_WINDOW_PA + 0x0f0000)
#define RP1_ETH_PA		(RP1_WINDOW_PA + 0x100000)
#define RP1_PCIE_APBS_PA	(RP1_WINDOW_PA + 0x108000)
#define RP1_USB0_PA		(RP1_WINDOW_PA + 0x200000)
#define RP1_USB1_PA		(RP1_WINDOW_PA + 0x300000)
#define RP1_DMA_BUS_OFFSET	(knl_rp1_dma_offset)	/* bus address = physical + this */
#define RP1_MSI_SPI_BASE	128			/* MSI data d -> SPI 128+d (INTID 160+d) */

#define DBG_UART_PA		PL011_UART10_PA		/* console used by boot.S (before the MMU) */

/* Kernel virtual addresses (device window) */
#define GICD_BASE		DEV_BASE(GICD_PA)
#define GICC_BASE		DEV_BASE(GICC_PA)
#define PCIE2_BASE		DEV_BASE(PCIE2_PA)
#define MIP0_BASE		DEV_BASE(MIP0_PA)
#define PL011_UART10_BASE	DEV_BASE(PL011_UART10_PA)
#define SDHCI_BASE		DEV_BASE(SDHCI_PA)
#define SDHCI_CFG_BASE		DEV_BASE(SDHCI_CFG_PA)
#define SYSTIMER_BASE		DEV_BASE(SYSTIMER_PA)
#define VC_MAILBOX_BASE		DEV_BASE(VC_MAILBOX_PA)
#define PM_BASE			DEV_BASE(PM_PA)
#define RNG_BASE		DEV_BASE(RNG_PA)
#define GIO_AON_BASE		DEV_BASE(GIO_AON_PA)
#define V3D_HUB_BASE		DEV_BASE(V3D_HUB_PA)
#define V3D_CORE0_BASE		DEV_BASE(V3D_CORE0_PA)

/* RP1 blocks. Reached through the device window because the firmware
   leaves the PCIe link and the peripheral window up (pciex4_reset=0) */
#define RP1_WINDOW_BASE		DEV_BASE(RP1_WINDOW_PA)
#define RP1_UART0_BASE		DEV_BASE(RP1_UART0_PA)
#define RP1_IO_BANK0_BASE	DEV_BASE(RP1_IO_BANK0_PA)
#define RP1_SYS_RIO0_BASE	DEV_BASE(RP1_SYS_RIO0_PA)
#define RP1_PADS_BANK0_BASE	DEV_BASE(RP1_PADS_BANK0_PA)
#define RP1_ETH_BASE		DEV_BASE(RP1_ETH_PA)
#define DBG_UART_BASE		DEV_BASE(DBG_UART_PA)

/* ------------------------------------------------------------------------ */
/*
 * Interrupts
 */
#define N_INTVEC		512			/* GIC-400: up to 480 SPIs */
#define TIMER_INTNO		30			/* Generic Timer, non-secure EL1 physical (PPI 14) */
#define VTIMER_INTNO		27			/* Generic Timer, virtual (PPI 11) */

/* Generic Timer frequency: read CNTFRQ_EL0 at boot (54MHz on Raspberry Pi 5) */
#define CNTFRQ_DEFAULT		54000000

#endif /* __SYS_SYSDEF_DEPEND_CPU_H__ */
