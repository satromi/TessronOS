/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sdhci.c
 *	SD host controller of the Raspberry Pi 5 and the Compute Module 5
 *	(design 10.6, 4.2; docs/tessronos/03-platform.md 3.5).
 *
 *	The registers follow the SD Host Controller specification; the
 *	block sits at 0x10_00ff_f000 with SPI 273. The bus is four bits
 *	wide; the clock is raised from the identification speed to the
 *	default speed's 25MHz. On the Raspberry Pi 5's card slot, whose
 *	signal level is switched by an always-on GPIO and whose card power
 *	is another, a card that accepts 1.8V in its operating conditions is
 *	switched to it (CMD11) and then to the fastest UHS-I bus speed both
 *	sides have, SDR104, DDR50 or SDR50, with the sampling point found by
 *	the tuning command (sd_uhs_card). Anything that fails on the way
 *	powers the card off and on again and starts over at 3.3V, where a
 *	card that has the High Speed function is switched to it at 50MHz
 *	(sd_high_speed); one that fails that stays at the default speed.
 *	A card whose configuration register says it takes SET_BLOCK_COUNT
 *	(CMD23) is told the length of every multiple block transfer before
 *	it, by the controller itself (Auto-CMD23) where it can, so that no
 *	CMD12 has to stop it.
 *
 *	What is on the controller depends on the board (include/ts/board.h):
 *	the Raspberry Pi 5's card slot, whose detect switch is an always-on
 *	GPIO; the Compute Module 5's eMMC, soldered on, eight data lines at
 *	a fixed 1.8V and no detect line; the Compute Module 5 Lite's carrier
 *	slot, which has no detect line either, so that a card is simply
 *	looked for. A device that does not answer as an SD card is tried as
 *	an MMC (JEDEC eMMC): its own start-up (CMD1, an address given to
 *	it), its size from the extended CSD, its bus widened by a switch
 *	command and checked by reading the extended CSD again. The clock
 *	stays at 25MHz, inside the 26MHz of the eMMC's backward compatible
 *	timing.
 *
 *	Data moves one of three ways. ADMA2 walks a table of descriptors,
 *	each naming a length and an address, so one command carries as much
 *	as the table can describe. The single address mode takes one address
 *	and asks for the next at every buffer boundary. The programmed path
 *	moves words through the data port and needs no memory the controller
 *	can reach at all. Which one is used comes from the capability
 *	register, which says whether the controller has the descriptor mode
 *	and whether its addresses are 64 bits wide; CNF_SD_DMA_MODE forces
 *	the choice. The programmed path stays and stays selectable because
 *	it is the only one the others can be compared against when the board
 *	is first tried, and a transfer no descriptor table can describe - a
 *	buffer outside the linear map of memory, or one needing more
 *	descriptors than the table holds - falls back to it on its own.
 *
 *	The controller masters the SoC's own bus rather than sitting behind
 *	the PCIe bridge, so an address written into a descriptor is the
 *	physical address with nothing added to it. Nothing on this board is
 *	cache coherent with a bus master, so the table and the data are
 *	cleaned out of the cache before a transfer and invalidated after it.
 *
 *	The Raspberry Pi 4 family has the same kind of controller in its
 *	EMMC2 block (0xFE34_0000): the card slot of the Pi 4 and the Pi 400,
 *	the Compute Module 4's eMMC with eight data lines, or the Lite's
 *	carrier slot. None of them has a detect line the controller can use,
 *	so a card is looked for. The base clock is the firmware's EMMC2
 *	clock, asked for through the property mailbox, because the capability
 *	register does not say it. The controller does not reach memory at its
 *	physical address: the device tree's dma-ranges of /emmc2bus says what
 *	it sees, which on the first stepping of the chip (B0) is the low 1GB
 *	at bus address 0xC000_0000 and on the later one (C0) everything below
 *	the peripherals at the address itself. A buffer it cannot reach goes
 *	through the bounce buffer, which is taken from memory every bus
 *	master reaches.
 *
 *	The card is registered as the disk device "sda" with its partitions
 *	as subunits, the same shape the virtio block driver uses, so the
 *	file system layer above does not know which one it is talking to.
 *
 *	The same driver runs under QEMU on the SD host controller the
 *	machine offers as a PCI function (sdhci-pci, class 08 05), whose
 *	registers are the same standard ones behind the function's first
 *	window. That is where the commands, the three ways of moving data,
 *	CMD23 sent as a command of its own, the partition offsets and the
 *	ways back from a UHS-I switch that fails are tested against a card
 *	image (tests/ktest/ktest_sd.c). The 1.8V switch, the tuning and
 *	Auto-CMD23 exist on the board only (docs/tessronos/03-platform.md
 *	3.5.5).
 */

#include <sys/machine.h>

#if defined(RPI5) || defined(RPI4) || defined(QEMU_VIRT)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/blk.h>
#include <ts/board.h>
#if defined(RPI5)
#include <ts/gpio.h>
#elif defined(QEMU_VIRT)
#include <ts/pcie.h>
#endif
#include "sysman/pfalloc.h"
#include "sysdepend.h"
#ifdef RPI4
#include "sysdepend/sysdepend.h"
#endif

#ifdef QEMU_VIRT
/* the registers are wherever the bus walk put the function's window */
LOCAL UBINT	sd_base;
#define SDHCI_BASE		sd_base
#define PCI_CLASS_SDHCI		0x08		/* base system peripheral */
#define PCI_SUBCLASS_SDHCI	0x05		/* SD host controller */
#endif

#ifdef RPI4
/*
 * EMMC2, or with tessronos.sd=emmc on the command line the older EMMC
 * controller at 0xFE30_0000. On the board that one is wired to the
 * Wi-Fi; QEMU's raspi4b puts its SD card there and leaves EMMC2 empty,
 * so the same image is tried under QEMU with it.
 */
LOCAL UBINT	sd_base = SDHCI_BASE;
#undef SDHCI_BASE
#define SDHCI_BASE		sd_base
#define EMMC1_PA		0xFE300000
#endif

/* ---------------------------------------------------------------- registers */
#define SDHCI_ARGUMENT2		(SDHCI_BASE + 0x00)
#define SDHCI_SDMA_ADDRESS	(SDHCI_BASE + 0x00)	/* the same word, in the single address mode */
#define SDHCI_BLOCK_SIZE	(SDHCI_BASE + 0x04)	/* 16 bit */
#define SDHCI_BLOCK_COUNT	(SDHCI_BASE + 0x06)	/* 16 bit */
#define SDHCI_ARGUMENT		(SDHCI_BASE + 0x08)
#define SDHCI_TRANSFER_MODE	(SDHCI_BASE + 0x0c)	/* 16 bit */
#define SDHCI_COMMAND		(SDHCI_BASE + 0x0e)	/* 16 bit */
#define SDHCI_RESPONSE(i)	(SDHCI_BASE + 0x10 + (i) * 4)
#define SDHCI_BUFFER		(SDHCI_BASE + 0x20)
#define SDHCI_PRESENT_STATE	(SDHCI_BASE + 0x24)
#define SDHCI_HOST_CONTROL	(SDHCI_BASE + 0x28)	/* 8 bit */
#define SDHCI_POWER_CONTROL	(SDHCI_BASE + 0x29)	/* 8 bit */
#define SDHCI_CLOCK_CONTROL	(SDHCI_BASE + 0x2c)	/* 16 bit */
#define SDHCI_TIMEOUT_CONTROL	(SDHCI_BASE + 0x2e)	/* 8 bit */
#define SDHCI_SOFTWARE_RESET	(SDHCI_BASE + 0x2f)	/* 8 bit */
#define SDHCI_INT_STATUS	(SDHCI_BASE + 0x30)
#define SDHCI_INT_ENABLE	(SDHCI_BASE + 0x34)
#define SDHCI_SIGNAL_ENABLE	(SDHCI_BASE + 0x38)
#define SDHCI_AUTO_CMD_STATUS	(SDHCI_BASE + 0x3c)	/* 16 bit: why an Auto-CMD12 or Auto-CMD23 failed */
#define SDHCI_HOST_CONTROL2	(SDHCI_BASE + 0x3e)	/* 16 bit */
#define SDHCI_CAPABILITIES	(SDHCI_BASE + 0x40)
#define SDHCI_CAPABILITIES1	(SDHCI_BASE + 0x44)
#define SDHCI_MAX_CURRENT	(SDHCI_BASE + 0x48)	/* bits 7:0: the most the 3.3V supply gives, in 4mA */
#define SDHCI_ADMA_ERROR	(SDHCI_BASE + 0x54)	/* 8 bit */
#define SDHCI_ADMA_ADDRESS	(SDHCI_BASE + 0x58)	/* 64 bit; the low word alone when addresses are 32 bit */

/*
 * The preset values, 16 bit each from 0x66 (SDR12) to 0x6e (DDR50): the
 * clock divider and the driver strength of a bus speed (sd_speeds).
 */
#define PRESET_DIV(v)		( (v) & 0x3ff )
#define PRESET_CLKGEN		0x0400		/* the programmable clock instead of the divided one */

/*
 * The BCM2712's own configuration registers of the block (0x10_00ff_f400).
 * The pin select says whether the pads run the SD or the eMMC timing;
 * with the 50MHz limit mode off, the clock comes through the delay line
 * whose sampling point the tuning moves, which the bus speeds above
 * 50MHz need.
 */
#ifdef RPI5
#define SDIO_CFG_SD_PIN_SEL	(SDHCI_CFG_BASE + 0x44)
#define SD_PIN_SEL_MASK		0x3
#define SD_PIN_SEL_SD		0x2
#define SDIO_CFG_MAX_50MHZ	(SDHCI_CFG_BASE + 0x1ac)
#define MAX_50MHZ_ENABLE	0x00000001
#define MAX_50MHZ_OVERRIDE	0x80000000	/* this register, not the strap, decides */
#endif

/* Present state */
#define STATE_CMD_INHIBIT	0x00000001
#define STATE_DAT_INHIBIT	0x00000002
#define STATE_CARD_INSERTED	0x00010000
#define STATE_BUF_WRITE_EN	0x00000400
#define STATE_BUF_READ_EN	0x00000800
#define STATE_DAT_LEVEL(s)	( ( (s) >> 20 ) & 0xf )	/* the levels of DAT[3:0] */

/* Interrupt status (the low half is normal, the high half is error) */
#define INT_CMD_COMPLETE	0x00000001
#define INT_XFER_COMPLETE	0x00000002
#define INT_DMA			0x00000008	/* a buffer boundary was reached */
#define INT_BUF_WRITE_READY	0x00000010
#define INT_BUF_READ_READY	0x00000020
#define INT_CARD_INSERT		0x00000040
#define INT_ERROR		0x00008000
#define INT_ERROR_MASK		0xFFFF0000
#define INT_CMD_CRC		0x00020000
#define INT_CMD_END_BIT		0x00040000
#define INT_CMD_INDEX		0x00080000
#define INT_DATA_CRC		0x00200000
#define INT_DATA_END_BIT	0x00400000
#define INT_AUTO_CMD		0x01000000	/* Auto-CMD12 or Auto-CMD23 failed */
#define INT_ADMA_ERROR		0x02000000	/* the descriptor table was refused */
/* what a sampling point drifted off the data shows as: worth tuning again */
#define INT_SAMPLING_ERRORS	( INT_CMD_CRC | INT_CMD_END_BIT | INT_CMD_INDEX | INT_DATA_CRC | INT_DATA_END_BIT )

/* Software reset */
#define RESET_ALL		0x01
#define RESET_CMD		0x02
#define RESET_DATA		0x04

/* Clock control */
#define CLOCK_INTERNAL_EN	0x0001
#define CLOCK_INTERNAL_STABLE	0x0002
#define CLOCK_CARD_EN		0x0004
#define CLOCK_PROG_MODE		0x0020		/* the programmable clock generator */

/* Host control 1 */
#define CTRL_4BITBUS		0x02
#define CTRL_HISPD		0x04		/* High Speed Enable: the timing of the High Speed mode */
#define CTRL_8BITBUS		0x20		/* eight data lines, for an embedded device */
#define CTRL_CD_TEST		0xC0		/* card detect from the test level, which says inserted */
#define CTRL_DMA_MASK		0x18		/* which kind of transfer the controller runs */
#define CTRL_DMA_SDMA		0x00
#define CTRL_DMA_ADMA2_32	0x10
#define CTRL_DMA_ADMA2_64	0x18

/* Host control 2 */
#define CTRL2_UHS_MASK		0x0007		/* the UHS mode select: none of them in the High Speed mode */
#define CTRL2_UHS_SDR12		0x0000
#define CTRL2_UHS_SDR25		0x0001
#define CTRL2_UHS_SDR50		0x0002
#define CTRL2_UHS_SDR104	0x0003
#define CTRL2_UHS_DDR50		0x0004
#define CTRL2_1V8		0x0008		/* the lines signal at 1.8V */
#define CTRL2_DRV_MASK		0x0030		/* the driver strength select: 0 is type B, the default */
#define CTRL2_EXEC_TUNING	0x0040		/* the tuning runs; the controller clears it at the end */
#define CTRL2_TUNED_CLK		0x0080		/* the sampling point the tuning found is in use */
#define CTRL2_PRESET_EN		0x8000		/* clock and driver strength from the preset values */
#define SDHCI_HOST_VERSION	(SDHCI_BASE + 0xfe)	/* 16 bit: the specification version in bits 7:0 */
#define SDHCI_SPEC_300		2		/* version 3.00 of the specification */

/*
 * Capabilities, the second word: the UHS-I bus speeds, whether SDR50
 * needs the tuning too, the re-tuning timer, the clock multiplier.
 */
#define CAP1_SDR50		0x00000001
#define CAP1_SDR104		0x00000002
#define CAP1_DDR50		0x00000004
#define CAP1_SDR50_TUNING	0x00002000
#define CAP1_RETUNE_COUNT(c)	( ( (c) >> 8 ) & 0xf )		/* re-tune every 2^(n-1) seconds; 0 never */
#define CAP1_RETUNE_MODE(c)	( ( (c) >> 14 ) & 0x3 )		/* 0: the driver re-tunes when the timer says */
#define CAP1_CLK_MUL(c)		( ( (c) >> 16 ) & 0xff )	/* the programmable clock's multiplier less one */

/*
 * Capabilities. The controller says here which transfer modes it has and
 * how wide the addresses it masters with are.
 */
#define CAP_8BIT		0x00040000	/* eight data lines for an embedded device */
#define CAP_ADMA2		0x00080000	/* the descriptor table mode */
#define CAP_HISPD		0x00200000	/* the High Speed mode */
#define CAP_SDMA		0x00400000	/* the single address mode */
#define CAP_64BIT		0x10000000	/* addresses are 64 bits wide */

/* Transfer mode */
#define XFER_DMA_EN		0x0001
#define XFER_BLKCNT_EN		0x0002
#define XFER_AUTO_CMD12		0x0004
#define XFER_AUTO_CMD23		0x0008		/* CMD23 with ARGUMENT2 before the command, and no CMD12 */
#define XFER_READ		0x0010
#define XFER_MULTI		0x0020

/* Command flags */
#define CMD_RESP_NONE		0x0000
#define CMD_RESP_136		0x0001
#define CMD_RESP_48		0x0002
#define CMD_RESP_48_BUSY	0x0003
#define CMD_CRC_CHECK		0x0008
#define CMD_INDEX_CHECK		0x0010
#define CMD_DATA_PRESENT	0x0020

#define CMD(index, flags)	(((index) << 8) | (flags))

/* SD commands */
#define CMD_GO_IDLE		CMD(0,  CMD_RESP_NONE)
#define CMD_ALL_SEND_CID	CMD(2,  CMD_RESP_136 | CMD_CRC_CHECK)
#define CMD_SEND_REL_ADDR	CMD(3,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SELECT_CARD		CMD(7,  CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SEND_IF_COND	CMD(8,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SEND_CSD		CMD(9,  CMD_RESP_136 | CMD_CRC_CHECK)
#define CMD_STOP_XFER		CMD(12, CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SET_BLOCKLEN	CMD(16, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_READ_SINGLE		CMD(17, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_READ_MULTI		CMD(18, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_WRITE_SINGLE	CMD(24, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_WRITE_MULTI		CMD(25, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_APP_CMD		CMD(55, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define ACMD_SET_BUS_WIDTH	CMD(6,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define ACMD_SEND_OP_COND	CMD(41, CMD_RESP_48)
#define CMD_VOLTAGE_SWITCH	CMD(11, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_SEND_TUNING		CMD(19, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define CMD_SET_BLOCK_COUNT	CMD(23, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_DESELECT		CMD(7,  CMD_RESP_NONE)

#define OCR_VOLTAGE		0x00FF8000	/* 2.7 to 3.6 volts */
#define OCR_S18R		0x01000000	/* asks for 1.8V; S18A in the answer when the card takes it */
#define OCR_XPC			0x10000000	/* the host gives more than 150mA: the card may draw it */
#define OCR_HCS			0x40000000	/* high capacity supported */
#define OCR_BUSY		0x80000000	/* the card has finished */

/*
 * MMC (JEDEC eMMC). The device answers CMD1 with its operating
 * conditions and is given its relative address by the host. Its size
 * past 2GB and its bus width live in the extended CSD, 512 bytes read
 * with CMD8 and written a byte at a time with CMD6.
 */
#define CMD_MMC_SEND_OP_COND	CMD(1,  CMD_RESP_48)
#define CMD_MMC_SET_REL_ADDR	CMD(3,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_MMC_SWITCH		CMD(6,  CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_MMC_SEND_EXT_CSD	CMD(8,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)

#define MMC_OCR_ARG		0x40FF8080	/* sector addressing, 2.7 to 3.6V and 1.70 to 1.95V */
#define MMC_OCR_ACCESS		0x60000000	/* the access mode the device answers */
#define MMC_OCR_SECTOR		0x40000000	/* addressed by sector: larger than 2GB */
#define MMC_RCA			0x00010000	/* the address given to it, in the high half */

#define MMC_SWITCH_WRITE	0x03000000	/* CMD6 access: write the byte */
#define MMC_SWITCH(idx, val)	( MMC_SWITCH_WRITE | ( (UW)(idx) << 16 ) | ( (UW)(val) << 8 ) )
#define R1_SWITCH_ERROR		0x00000080

#define EXT_CSD_BUS_WIDTH	183		/* 0: 1 bit, 1: 4 bits, 2: 8 bits */
#define EXT_CSD_SEC_COUNT	212		/* four bytes, least significant first */
#define EXT_CSD_PROPS		192		/* from here on the bytes only describe the device */
#define EXT_CSD_SIZE		512

/* Card status (R1) and the write protection the card data holds */
#define R1_CARD_IS_LOCKED	0x02000000	/* a password is set and not given */
#define CSD_PERM_WP		0x00002000	/* bit 13: for good */
#define CSD_TMP_WP		0x00001000	/* bit 12: until cleared */
#define SD_WP_TMP		0x01
#define SD_WP_PERM		0x02
#define SD_WP_LOCKED		0x04

#define SD_SPIN			1000000		/* polling attempts */

/*
 * How long a transfer may take. The waits spin a little and then sleep a
 * millisecond at a time, so that a slow card is waited for by time, not
 * by how fast the processor reads a register: a card may take a quarter
 * of a second over one block it is programming, and much longer over a
 * large write when it has to erase first.
 */
#define SD_QUICK_SPIN		20000		/* reads before the first sleep */
#define SD_INT_MS		1000		/* one command or one block */
#define SD_XFER_MS(n)		( 1000 + (INT)(n) * 2 )	/* a whole transfer of n blocks */

#define CMD_SEND_STATUS		CMD(13, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define R1_STATE(r)		( ( (r) >> 9 ) & 0xf )
#define R1_STATE_TRAN		4
#define R1_READY_FOR_DATA	0x00000100
#define R1_ERRORS		0xFDF90008	/* the error bits of an R1 status (not the lock bit 25) */

/* SD only: the switch function command and the configuration register */
#define CMD_SD_SWITCH		CMD(6,  CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define ACMD_SEND_SCR		CMD(51, CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK | CMD_DATA_PRESENT)
#define SD_SWITCH_CHECK		0x00FFFFF0	/* mode 0: what group 1 (the access mode) can be */
#define SD_SWITCH_HS		0x80FFFFF1	/* mode 1: group 1 set to function 1, the High Speed mode */
#define SD_SWITCH_DEFAULT	0x80FFFFF0	/* mode 1: group 1 set back to function 0, the default speed */
#define SD_STATUS_LEN		64		/* the switch function status */
#define SD_SCR_LEN		8
#define CSD_CCC_SWITCH		0x400		/* command class 10 (switch) of the CSD's CCC */
#define SD_HS_HZ		50000000
#define SD_DS_HZ		25000000
#define SD_HS_CHECK_SECT	64		/* 32KB read at both speeds and compared */

/*
 * The configuration register: the bytes as the card sends them, bits
 * 63:56 first. SD_SPEC 2 with SD_SPEC3 set is version 3.0x, which
 * has the UHS-I bus speeds in the switch status and the command
 * support bits; SD_SPEC4 widens those from two bits to four.
 */
#define SCR_SD_SPEC(s)		( (s)[0] & 0x0f )
#define SCR_SPEC3(s)		( SCR_SD_SPEC(s) == 2 && ( (s)[2] & 0x80 ) != 0 )
#define SCR_SPEC4(s)		( SCR_SPEC3(s) && ( (s)[2] & 0x04 ) != 0 )
#define SCR_CMD_SUPPORT(s)	( SCR_SPEC4(s) ? ( (s)[3] & 0x0f ) : SCR_SPEC3(s) ? ( (s)[3] & 0x03 ) : 0 )
#define SCR_CMD23		0x02		/* SET_BLOCK_COUNT */

/*
 * The switch status of mode 0: what function group 1 (the bus speed)
 * and group 4 (the current limit) offer. At 1.8V, functions 0 to 4 of
 * group 1 are SDR12, SDR25, SDR50, SDR104 and DDR50; at 3.3V only the
 * first two exist, the default speed and the High Speed mode.
 */
#define SW_BUS_MODES(w)		( (w)[13] )
#define SW_CURRENTS(w)		( (UW)(w)[7] | ( (UW)(w)[6] << 8 ) )
#define SW_FUNC_SDR50		0x04
#define SW_FUNC_SDR104		0x08
#define SW_FUNC_DDR50		0x10
#define SW_GROUP_BUS		0		/* group 1, as the shift of its four bits in the argument */
#define SW_GROUP_CURRENT	3		/* group 4 */
#define SW_SET			0x80000000	/* mode 1: switch */

/*
 * The bus speeds, in the order of SD_SPEED_* (include/ts/blk.h): the
 * function of group 1 that selects one on the card, the UHS mode of the
 * host, the preset register, the fastest clock the bus speed allows,
 * and whether the sampling point is tuned (2: when the controller says
 * SDR50 needs it).
 */
typedef struct {
	CONST char	*name;
	UB		func;
	UH		uhs;
	UBINT		preset;
	UW		max_hz;
	UB		tune;
} SDSPEED;

LOCAL CONST SDSPEED sd_speeds[] = {
	{ "default speed", 0, 0,                0,                   25000000,  0 },
	{ "High Speed",    1, 0,                0,                   50000000,  0 },
	{ "UHS-I SDR12",   0, CTRL2_UHS_SDR12,  0x66,                25000000,  0 },
	{ "UHS-I SDR25",   1, CTRL2_UHS_SDR25,  0x68,                50000000,  0 },
	{ "UHS-I SDR50",   2, CTRL2_UHS_SDR50,  0x6a,                100000000, 2 },
	{ "UHS-I DDR50",   4, CTRL2_UHS_DDR50,  0x6e,                50000000,  1 },
	{ "UHS-I SDR104",  3, CTRL2_UHS_SDR104, 0x6c,                208000000, 1 },
};

/* The UHS-I bus speeds the board offers on the slot */
#define HC_SDR50		0x01
#define HC_SDR104		0x02
#define HC_DDR50		0x04

/*
 * How the switch to 1.8V went (sd_uhs_voltage): not asked for, done,
 * failed in a way worth another try after the card's power is cycled,
 * or failed so that the card is taken at 3.3V.
 */
#define VS_NONE			0
#define VS_OK			1
#define VS_AGAIN		2
#define VS_FAIL			3

#define SD_S18R_TRIES		10		/* 1.8V asked for again after a failed switch */
#define SD_TUNING_LOOPS		40		/* CMD19 at most per tuning */
#define SD_TUNING_LEN		64		/* the tuning block of a four bit bus */
#define SD_TUNING_MS		50		/* the wait for one tuning block */
#define SD_POWER_UP_MS		10		/* after the supply is on, and after the clock is */

/*
 * Block size register: the high bits say how far the single address mode
 * runs before it stops and asks for the address of the rest. The largest
 * step there is keeps the interruptions rare.
 */
#define BLKSZ_SDMA_BOUND_512K	0x7000
#define SDMA_BOUND		0x80000

/*
 * ADMA2 descriptors. A descriptor is a set of attribute bits, a length
 * and an address: two words when addresses are 32 bits wide, three when
 * they are 64. They are written word by word rather than through a
 * structure so that no padding can get between the fields.
 */
#define ADMA_ATTR_VALID		0x0001
#define ADMA_ATTR_END		0x0002		/* the last one of the table */
#define ADMA_ATTR_INT		0x0004
#define ADMA_ATTR_TRAN		0x0020		/* move data, as against link or skip */

/*
 * One descriptor carries at most 32KB. The field could say 64KB (as 0),
 * but not every controller reads a zero length that way, and the table
 * has room enough either way.
 */
#define ADMA_MAX_LEN		0x8000
#define ADMA_DESC_MAX		256		/* 8MB in one transfer, 3KB of table */

/*
 * A buffer the controller moves data into or out of starts on a cache
 * line. A read into one that shares its first or last line with other
 * data loses: whatever is written to that other data while the transfer
 * runs brings the line back into the cache, and the invalidate after the
 * transfer writes the line, stale half and all, over what the controller
 * put there. Such a buffer goes through the driver's own bounce buffer,
 * whose lines are its alone.
 */
#define DMA_ALIGN		64
#ifdef RPI4
#define BOUNCE_ORDER		6		/* 256KB: memory past what EMMC2 reaches goes through it */
#else
#define BOUNCE_ORDER		4		/* 64KB */
#endif
#define BOUNCE_SECT		( ( PAGE_SIZE << BOUNCE_ORDER ) / BLK_SECTOR_SIZE )

/*
 * Sectors in one command. The block count register is 16 bits wide; a
 * larger request is sent as several commands.
 */
#define SD_XFER_MAX		8192

/*
 * The bytes the file system is told to ask for at once (TDN_IOMAX):
 * 1280KB (design 7.3 says how it was chosen). One such request is one
 * command and 40 descriptors of ADMA_MAX_LEN.
 */
#define SD_IO_MAX		( 1280 * 1024 )

/* How data moves */
#define SD_DMA_NONE		0		/* through the data port */
#define SD_DMA_SDMA		1		/* one address, renewed at each boundary */
#define SD_DMA_ADMA2_32		2		/* descriptor table, 32 bit addresses */
#define SD_DMA_ADMA2_64		3		/* descriptor table, 64 bit addresses */

/*
 * Which of them to use. A negative value takes what the capability
 * register offers; 0 to 3 force one, which is how the programmed path is
 * kept selectable for comparison on a board.
 */
#ifndef CNF_SD_DMA_MODE
#define CNF_SD_DMA_MODE		(-1)
#endif

typedef struct {
	UD	nsect;			/* sectors of the card */
	UW	rca;			/* relative address, in the high half */
	BOOL	hc;			/* block addressed (SDHC and up, eMMC past 2GB) */
	BOOL	mmc;			/* an MMC (eMMC), not an SD card */
	UB	width;			/* data lines in use: 1, 4 or 8 */
	BOOL	present;
	UB	wp;			/* SD_WP_*: why the card takes no writes, 0 when it does */
	INT	dma_mode;		/* SD_DMA_* */
	UW	ccc;			/* the command classes, from the CSD */
	UW	clock_hz;		/* the bus clock set last */
	INT	speed;			/* the bus speed, SD_SPEED_* */
	BOOL	v18;			/* the lines signal at 1.8V */
	BOOL	tuned;			/* the sampling point the tuning found is in use */
	UW	retune_ms;		/* how often the re-tuning timer asks for it again, 0 never */
	UW	tuned_at;		/* when it was tuned last, in ms */
	BOOL	pv_used;		/* the clock comes from the preset value pv */
	UH	pv;
	UB	scr[SD_SCR_LEN];	/* the configuration register */
	BOOL	sw_ok;			/* sw holds the switch status of mode 0 */
	UB	sw[SD_STATUS_LEN];
	BOOL	cmd23_card;		/* the card takes SET_BLOCK_COUNT */
	INT	cmd23_force;		/* SD_CMD23_* set by a test, negative to decide */
	UW	*desc;			/* the descriptor table, when there is one */
	UD	desc_pa;
	UB	*bounce;		/* for buffers off a cache line, NULL without */
	ID	mtxid;
	ID	devid;
	T_PARTTBL tbl;
	UB	devnm[8];
} SDCARD;

LOCAL SDCARD	sd;
LOCAL INT	sd_step;	/* how far sd_init_card came, for the message when it fails */
LOCAL UW	sd_err_status;	/* the interrupt status of the last transfer that failed */
LOCAL BOOL	sd_in_bringup;	/* the card is being brought up: multiple blocks end with CMD12 */
LOCAL UW	sd_test;	/* SD_TEST_* of knl_sd_reinit, while it runs */

/*
 * The card slot's supplies, from the device tree: the signal level is
 * a GPIO regulator (sd-io-1v8-reg) and the card's power a fixed one
 * switched by a GPIO (sd-vcc-reg), both on the always-on GPIO block.
 * With both there, and the bus speeds the node of the controller lists,
 * the slot can run UHS-I.
 */
#ifdef RPI5
#define SDIO1_PATH		"/soc/mmc@fff000"
#define SD_IO_REG_PATH		"/sd-io-1v8-reg"
#define SD_VCC_REG_PATH		"/sd-vcc-reg"

LOCAL struct {
	BOOL	reg;			/* both supplies are known */
	UW	caps;			/* HC_*: the bus speeds the node lists */
	UW	f_max;			/* the node's max-frequency, 0 without */
	UINT	io_pin;			/* the signal level: the pin, and its level for each voltage */
	UINT	io_1v8, io_3v3;
	UW	settle_us;		/* how long the level takes to settle */
	UINT	vcc_pin;		/* the card's power: the pin and its level when on */
	UINT	vcc_on;
} sd_slot;
#endif

/*
 * What the controller sees of memory: physical sd_dma_cpu and up, for
 * sd_dma_size bytes, at bus address sd_dma_bus and up. Everything at its
 * own address unless the board says otherwise (the Raspberry Pi 4's
 * /emmc2bus dma-ranges, read in knl_sd_init).
 */
LOCAL UD	sd_dma_bus = 0;
LOCAL UD	sd_dma_cpu = 0;
LOCAL UD	sd_dma_size = ~(UD)0;
LOCAL UW	sd_base_hz = 0;	/* the base clock, when the board says it */

/* The bus address of [pa, pa + len), FALSE when the controller does not reach all of it */
LOCAL BOOL sd_bus_addr( UD pa, UD len, UD *p_bus )
{
	if ( pa < sd_dma_cpu || pa + len < pa || pa + len - sd_dma_cpu > sd_dma_size ) {
		return FALSE;
	}
	*p_bus = sd_dma_bus + ( pa - sd_dma_cpu );
	return TRUE;
}

/* ---------------------------------------------------------------- low level */

/*
 * Every register is reached with 32 bit accesses only. The narrower
 * registers of the specification share words; one is written by reading
 * its word, changing its bytes and writing the word back. Two words need
 * care. The transfer mode and the command share one, and writing the
 * command starts it, so the mode is kept aside and goes out in the same
 * write as the command. The word of the clock control also holds the
 * software reset bits, which are written back as zero so that changing
 * the clock never starts a reset.
 */
LOCAL UH	sd_xfer_mode;		/* the transfer mode for the next command */

#define SD_WORD(r)		((r) & ~(UBINT)3)
#define SD_SHIFT(r)		(((r) & 3) * 8)
#define SD_RESET_BITS		0xFF000000	/* byte 3 of the word at 0x2c */

LOCAL UW sd_in8( UBINT r )
{
	return ( in_w(SD_WORD(r)) >> SD_SHIFT(r) ) & 0xff;
}

LOCAL UW sd_in16( UBINT r )
{
	return ( in_w(SD_WORD(r)) >> SD_SHIFT(r) ) & 0xffff;
}

LOCAL UW sd_keep( UBINT r, UW w )
{
	return ( SD_WORD(r) == SD_WORD(SDHCI_SOFTWARE_RESET) ) ? ( w & ~(UW)SD_RESET_BITS ) : w;
}

LOCAL void sd_out8( UBINT r, UW v )
{
	UW	w = sd_keep(r, in_w(SD_WORD(r)));

	w &= ~( (UW)0xff << SD_SHIFT(r) );
	out_w(SD_WORD(r), w | ( ( v & 0xff ) << SD_SHIFT(r) ));
}

LOCAL void sd_out16( UBINT r, UW v )
{
	UW	w;

	if ( r == SDHCI_TRANSFER_MODE ) {
		sd_xfer_mode = (UH)v;		/* goes out with the command */
		return;
	}
	if ( r == SDHCI_COMMAND ) {
		out_w(SDHCI_TRANSFER_MODE, ( ( v & 0xffff ) << 16 ) | sd_xfer_mode);
		sd_xfer_mode = 0;
		return;
	}
	w = sd_keep(r, in_w(SD_WORD(r)));
	w &= ~( (UW)0xffff << SD_SHIFT(r) );
	out_w(SD_WORD(r), w | ( ( v & 0xffff ) << SD_SHIFT(r) ));
}

LOCAL void sd_out64( UBINT r, UD v )
{
	out_w(r, (UW)v);
	out_w(r + 4, (UW)( v >> 32 ));
}

LOCAL UD sd_in64( UBINT r )
{
	return (UD)in_w(r) | ( (UD)in_w(r + 4) << 32 );
}

LOCAL ER wait_state( UW mask, INT spin )
{
	INT	i;

	for ( i = 0; i < spin; i++ ) {
		if ( (in_w(SDHCI_PRESENT_STATE) & mask) == 0 ) return E_OK;
	}
	return E_TMOUT;
}

/* a millisecond's sleep, or a moment's pause where the caller cannot sleep */
LOCAL void sd_nap( void )
{
	if ( tk_dly_tsk(1) != E_OK ) {
		CpuRelax();
	}
}

LOCAL ER wait_int_ms( UW mask, UW *p_status, INT ms )
{
	UW	st;
	INT	i, slept = 0;

	for (;;) {
		for ( i = 0; i < SD_QUICK_SPIN; i++ ) {
			st = in_w(SDHCI_INT_STATUS);
			if ( (st & INT_ERROR_MASK) != 0 ) {
				out_w(SDHCI_INT_STATUS, st);	/* clear and report */
				if ( p_status != NULL ) *p_status = st;
				sd_err_status |= st;
				return E_IO;
			}
			if ( (st & mask) != 0 ) {
				out_w(SDHCI_INT_STATUS, st & mask);
				if ( p_status != NULL ) *p_status = st;
				return E_OK;
			}
		}
		if ( slept++ >= ms ) {
			return E_TMOUT;
		}
		sd_nap();
	}
}

LOCAL ER wait_int( UW mask, UW *p_status )
{
	return wait_int_ms(mask, p_status, SD_INT_MS);
}

/*
 * Wait until the controller has finished with the last transfer and the
 * card with the last write. The block size and count, the transfer mode
 * and the DMA addresses may not be written before that: while the data
 * lines are in use the controller keeps its old values, and the next
 * command would run with the last one's direction and length. A card
 * may take a quarter of a second over programming what it was sent.
 */
LOCAL ER sd_wait_idle( void )
{
	INT	i;

	for ( i = 0; i < 1000; i++ ) {
		if ( wait_state(STATE_CMD_INHIBIT | STATE_DAT_INHIBIT, 10000) == E_OK ) {
			return E_OK;
		}
		if ( tk_dly_tsk(1) != E_OK ) {
			CpuRelax();
		}
	}
	return E_TMOUT;
}

/*
 * One command. 'flags' carries the response kind and whether data follows.
 */
LOCAL ER sd_cmd( UH cmd, UW arg, UW *resp )
{
	ER	er;

	er = wait_state(STATE_CMD_INHIBIT | (( (cmd & CMD_DATA_PRESENT) != 0 ) ? STATE_DAT_INHIBIT : 0),
			SD_SPIN);
	if ( er < E_OK ) {
		return er;
	}
	/*
	 * Nothing left over from an earlier command may be taken for this
	 * one's: a command that holds the card busy (R1b) ends with the
	 * same transfer complete bit a data transfer ends with, and a wait
	 * that found it already set would return before its own transfer
	 * had moved anything.
	 */
	out_w(SDHCI_INT_STATUS, 0xFFFFFFFF);
	out_w(SDHCI_ARGUMENT, arg);
	sd_out16(SDHCI_COMMAND, cmd);

	er = wait_int(INT_CMD_COMPLETE, NULL);
	if ( er < E_OK ) {
		return er;
	}
	if ( (cmd & 3) == CMD_RESP_48_BUSY && (cmd & CMD_DATA_PRESENT) == 0 ) {
		/* the end of the busy time, which not every controller reports */
		(void)wait_int(INT_XFER_COMPLETE, NULL);
		(void)wait_state(STATE_DAT_INHIBIT, SD_SPIN);
	}
	if ( resp == NULL ) {
		/* nothing to read */
	} else if ( (cmd & 3) == CMD_RESP_136 ) {
		/*
		 * The controller keeps bits 127:8 of a long response in bits
		 * 119:0 of the four registers (the CRC is not kept). Shifted
		 * back up a byte, resp[3] holds bits 127:96 and resp[0]
		 * bits 31:0, with the CRC's byte zero.
		 */
		UW	r[4];
		INT	i;

		for ( i = 0; i < 4; i++ ) {
			r[i] = in_w(SDHCI_RESPONSE(i));
		}
		resp[3] = (r[3] << 8) | (r[2] >> 24);
		resp[2] = (r[2] << 8) | (r[1] >> 24);
		resp[1] = (r[1] << 8) | (r[0] >> 24);
		resp[0] = r[0] << 8;
	} else {
		resp[0] = in_w(SDHCI_RESPONSE(0));
	}

	return E_OK;
}

/* An application command is a CMD55 followed by the command itself */
LOCAL ER sd_acmd( UH cmd, UW arg, UW *resp )
{
	ER	er = sd_cmd(CMD_APP_CMD, sd.rca, NULL);

	if ( er < E_OK ) {
		return er;
	}
	return sd_cmd(cmd, arg, resp);
}

LOCAL ER sd_reset( UB what )
{
	INT	i;

	out_w(SD_WORD(SDHCI_SOFTWARE_RESET), ( in_w(SD_WORD(SDHCI_SOFTWARE_RESET)) & ~(UW)SD_RESET_BITS ) | ( (UW)what << 24 ));
	for ( i = 0; i < SD_SPIN; i++ ) {
		if ( (sd_in8(SDHCI_SOFTWARE_RESET) & what) == 0 ) return E_OK;
	}
	return E_TMOUT;
}

/* The base clock, as the board says it or the capability register does */
LOCAL UW sd_base_clock( void )
{
	UW	base;

	if ( sd_base_hz != 0 ) {
		return sd_base_hz;			/* as the board told it */
	}
	base = (in_w(SDHCI_CAPABILITIES) >> 8) & 0xff;	/* MHz */
	if ( base == 0 ) base = 100;		/* the field may read zero */
	return base * 1000000;
}

/*
 * Write the clock: the card's clock and the internal one stopped, the
 * BCM2712's pads given the SD timing, then the frequency select 'sel'
 * with the internal clock on and, once that is stable, the card's
 * clock. 'sel' 0 with 'hz' 0 leaves the clock stopped.
 */
LOCAL ER sd_clock_write( UW sel, UW hz )
{
	UH	val;
	INT	i;

	sd_out16(SDHCI_CLOCK_CONTROL, 0);
	sd.clock_hz = 0;
#ifdef RPI5
	out_w(SDIO_CFG_SD_PIN_SEL, ( in_w(SDIO_CFG_SD_PIN_SEL) & ~(UW)SD_PIN_SEL_MASK ) | SD_PIN_SEL_SD);
#endif
	if ( hz == 0 ) {
		return E_OK;
	}
	val = (UH)( sel | CLOCK_INTERNAL_EN );
	sd_out16(SDHCI_CLOCK_CONTROL, val);

	for ( i = 0; i < SD_SPIN; i++ ) {
		if ( (sd_in16(SDHCI_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE) != 0 ) break;
	}
	if ( i >= SD_SPIN ) {
		return E_TMOUT;
	}
	sd_out16(SDHCI_CLOCK_CONTROL, (UH)(val | CLOCK_CARD_EN));
	sd.clock_hz = hz;

	return E_OK;
}

/* The frequency select of the divided clock for divider register value 'div' (the divisor over two) */
#define CLOCK_DIV_SEL(div)	( ( ( (div) & 0xff ) << 8 ) | ( ( (div) & 0x300 ) >> 2 ) )

/*
 * The divider of the base clock that gives at most 'hz'. The 8 bit
 * divided clock mode of the specification is used. 0 stops the clock.
 */
LOCAL ER sd_set_clock( UW hz )
{
	UW	base, div;

	if ( hz == 0 ) {
		return sd_clock_write(0, 0);
	}
	base = sd_base_clock();
	for ( div = 1; div < 2048; div <<= 1 ) {
		if ( base / div <= hz ) break;
	}
	div >>= 1;					/* the register holds div/2 */

	return sd_clock_write(CLOCK_DIV_SEL(div), ( div == 0 ) ? base : base / ( div * 2 ));
}

/* a pause of at least 'us' microseconds */
LOCAL void sd_delay_us( UW us )
{
	if ( us >= 1000 ) {
		if ( tk_dly_tsk((RELTIM)( ( us + 999 ) / 1000 )) == E_OK ) return;
	}
	WaitUsec(us);
}

LOCAL void sd_delay_ms( UW ms )
{
	sd_delay_us(ms * 1000);
}

/*
 * After a transfer that failed or ran out of time: the controller's
 * command and data sides are reset, the card is told to stop whatever
 * transfer it is still in (CMD12; a card that is in none refuses it,
 * which does no harm), and it is waited for until it is back in the
 * transfer state and ready for data. Without this, a card left in the
 * middle of a write refuses every command after it.
 */
LOCAL void sd_recover( ER why )
{
	UW	resp[4];
	INT	i;

	sd_reset(RESET_CMD);
	sd_reset(RESET_DATA);
	(void)sd_cmd(CMD_STOP_XFER, 0, NULL);
	for ( i = 0; i < SD_INT_MS; i++ ) {
		resp[0] = 0;
		if ( sd_cmd(CMD_SEND_STATUS, sd.rca, resp) >= E_OK
		  && R1_STATE(resp[0]) == R1_STATE_TRAN
		  && (resp[0] & R1_READY_FOR_DATA) != 0 ) {
			break;
		}
		sd_nap();
	}
	tm_printf((UB*)"TessronOS: sd a transfer failed (%d); card status %08x after %d ms\n",
		  (INT)why, resp[0], i);
}

/* ---------------------------------------------------------------- transfer */

/*
 * Clean and invalidate a range, so that the controller sees what was
 * written and the processor keeps no stale line of what the controller
 * wrote. Nothing on this board is coherent with a bus master.
 */
LOCAL void dcache_flush( CONST void *p, UD len )
{
	DCacheFlushRange(p, len);
}

/*
 * Only a buffer in the linear map of memory has a physical address that
 * subtraction finds, and only such a buffer is one unbroken run of
 * physical memory. Anything else has to go through the data port.
 */
LOCAL BOOL buf_is_linear( CONST void *p, UD len )
{
	UBINT	a = (UBINT)p;

	return ( a >= KVA_BASE && a + len > a && a + len <= DEV_VA_BASE );
}

/*
 * Set the block size and count, say what the transfer does, and send the
 * command that starts it. 'extra_mode' carries the transfer mode bits
 * that belong to the way the data moves, 'extra_blksz' the buffer
 * boundary of the single address mode.
 */
/*
 * How a multiple block transfer is ended: by CMD12 after it, or by
 * SET_BLOCK_COUNT (CMD23) before it, which the controller sends itself
 * (Auto-CMD23) or which is sent as a command of its own. A card whose
 * configuration register says it takes CMD23 is given it; a controller
 * of version 3.00 or later sends it itself, except in the single
 * address mode, whose address register is the one Auto-CMD23 takes its
 * argument from. While the card is being brought up, CMD12.
 */
LOCAL INT sd_cmd23_way( void )
{
	INT	way;

	if ( sd_in_bringup ) {
		return SD_CMD23_NONE;
	}
	if ( sd.cmd23_force >= 0 ) {
		way = sd.cmd23_force;
	} else if ( !sd.cmd23_card ) {
		return SD_CMD23_NONE;
	} else {
		way = ( ( sd_in16(SDHCI_HOST_VERSION) & 0xff ) >= SDHCI_SPEC_300 ) ? SD_CMD23_AUTO : SD_CMD23_MANUAL;
	}
	if ( way == SD_CMD23_AUTO && sd.dma_mode == SD_DMA_SDMA ) {
		way = SD_CMD23_MANUAL;
	}
	return way;
}

LOCAL ER sd_xfer_begin( UD start, UD nsect, BOOL write, UH extra_mode, UH extra_blksz )
{
	UW	arg = (UW)(( sd.hc ) ? start : start * BLK_SECTOR_SIZE);
	UW	resp[4];
	INT	way = ( nsect > 1 ) ? sd_cmd23_way() : SD_CMD23_NONE;
	UH	mode;
	ER	er;

	if ( way == SD_CMD23_MANUAL ) {
		resp[0] = 0;
		er = sd_cmd(CMD_SET_BLOCK_COUNT, (UW)nsect, resp);
		if ( er >= E_OK && ( resp[0] & R1_ERRORS ) != 0 ) {
			er = E_IO;
		}
		if ( er < E_OK ) {
			return er;
		}
	}

	out_w(SDHCI_BLOCK_SIZE, (UW)(BLK_SECTOR_SIZE | extra_blksz) | ( (UW)nsect << 16 ));

	/*
	 * The block count runs down on a single block too, so that no
	 * transfer leaves a count behind: some controllers take a write of
	 * the single address register with blocks outstanding as the
	 * go-ahead for another transfer.
	 */
	mode = (( write ) ? 0 : XFER_READ) | XFER_BLKCNT_EN;
	if ( nsect > 1 ) {
		mode |= XFER_MULTI;
		if ( way == SD_CMD23_AUTO ) {
			mode |= XFER_AUTO_CMD23;
			out_w(SDHCI_ARGUMENT2, (UW)nsect);
		} else if ( way == SD_CMD23_NONE ) {
			mode |= XFER_AUTO_CMD12;
		}
	}
	sd_out16(SDHCI_TRANSFER_MODE, (UH)(mode | extra_mode));

	return sd_cmd(( write ) ? (( nsect > 1 ) ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE)
			        : (( nsect > 1 ) ? CMD_READ_MULTI : CMD_READ_SINGLE),
		      arg, NULL);
}

/*
 * Move whole blocks through the data port, one word at a time. This path
 * asks nothing of the controller but the data register, which is what
 * makes it the one the others are measured against.
 */
LOCAL ER sd_xfer_pio( UD start, void *buf, UD nsect, BOOL write )
{
	UB	*p = (UB *)buf;
	UW	st, w;
	UD	i;
	ER	er;

	er = sd_xfer_begin(start, nsect, write, 0, 0);
	if ( er < E_OK ) {
		return er;
	}

	for ( i = 0; i < nsect; i++ ) {
		UD	k;

		er = wait_int(( write ) ? INT_BUF_WRITE_READY : INT_BUF_READ_READY, &st);
		if ( er < E_OK ) {
			sd_recover(er);
			return er;
		}
		/* byte by byte in memory: the buffer need not be word aligned */
		for ( k = 0; k < BLK_SECTOR_SIZE / 4; k++, p += 4 ) {
			if ( write ) {
				w = (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
				out_w(SDHCI_BUFFER, w);
			} else {
				w = in_w(SDHCI_BUFFER);
				p[0] = (UB)w;
				p[1] = (UB)(w >> 8);
				p[2] = (UB)(w >> 16);
				p[3] = (UB)(w >> 24);
			}
		}
	}

	er = wait_int_ms(INT_XFER_COMPLETE, NULL, SD_XFER_MS(nsect));
	if ( er < E_OK ) {
		sd_recover(er);
	}

	return er;
}

/*
 * Describe one unbroken run of physical memory as a descriptor table.
 * Each entry carries at most 65536 bytes, which the 16 bit length field
 * writes as zero; the last one is marked so the controller stops there.
 * Answers how many entries were used, or E_NOSPT when the table is too
 * small for the transfer.
 */
LOCAL ER adma_build( UD pa, UD len )
{
	UW	*d = sd.desc, *last = NULL;
	INT	n = 0;
	INT	step = ( sd.dma_mode == SD_DMA_ADMA2_64 ) ? 3 : 2;

	while ( len > 0 ) {
		UD	chunk = ( len > ADMA_MAX_LEN ) ? ADMA_MAX_LEN : len;

		if ( n >= ADMA_DESC_MAX ) {
			return E_NOSPT;
		}
		d[0] = (UW)(ADMA_ATTR_VALID | ADMA_ATTR_TRAN)
		     | ((UW)(chunk & 0xffff) << 16);
		d[1] = (UW)pa;
		if ( step == 3 ) {
			d[2] = (UW)(pa >> 32);
		}
		last = d;
		d  += step;
		pa += chunk;
		len -= chunk;
		n++;
	}
	if ( last == NULL ) {
		return E_PAR;
	}
	last[0] |= ADMA_ATTR_END;
	dcache_flush(sd.desc, (UD)n * (UD)step * 4);

	return (ER)n;
}

/*
 * Wait for a transfer the controller runs by itself. In the single
 * address mode it stops at every buffer boundary and wants the address
 * of the rest, which is what its own register then holds. A refused
 * descriptor table is reported separately from a refused transfer, and
 * the controller leaves the address of the descriptor it stopped on
 * where it can be read out.
 */
LOCAL ER sd_dma_wait( UD nsect )
{
	UW	st;
	INT	i = 0, slept = 0;

	for ( ;; i++ ) {
		if ( i >= SD_QUICK_SPIN ) {
			if ( slept++ >= SD_XFER_MS(nsect) ) {
				break;
			}
			sd_nap();
			i = 0;
		}
		st = in_w(SDHCI_INT_STATUS);

		if ( (st & INT_ERROR_MASK) != 0 ) {
			out_w(SDHCI_INT_STATUS, st);
			sd_err_status |= st;
			if ( (st & INT_ADMA_ERROR) != 0 ) {
				UD	at = sd_in64(SDHCI_ADMA_ADDRESS);

				tm_printf((UB*)"TessronOS: sd ADMA error %02x, descriptor at %08x%08x\n",
					  (UW)sd_in8(SDHCI_ADMA_ERROR),
					  (UW)(at >> 32), (UW)at);
			}
			return E_IO;
		}
		if ( (st & INT_DMA) != 0 ) {
			out_w(SDHCI_INT_STATUS, INT_DMA);
			out_w(SDHCI_SDMA_ADDRESS, in_w(SDHCI_SDMA_ADDRESS));
			continue;
		}
		if ( (st & INT_XFER_COMPLETE) != 0 ) {
			out_w(SDHCI_INT_STATUS, st & INT_XFER_COMPLETE);
			return E_OK;
		}
	}

	return E_TMOUT;
}

/*
 * Let the controller fetch the data itself. Answers E_NOSPT when this
 * buffer cannot be reached the chosen way, which sends the transfer
 * through the data port instead.
 */
LOCAL ER sd_xfer_dma( UD start, void *buf, UD nsect, BOOL write )
{
	UD	len = nsect * BLK_SECTOR_SIZE;
	UD	pa, bus, desc;
	UB	ctrl;
	UH	blksz = 0;
	ER	er;

	if ( !buf_is_linear(buf, len) || ((UBINT)buf & (DMA_ALIGN - 1)) != 0 ) {
		return E_NOSPT;
	}
	pa = (UD)VA2PA((UBINT)buf);
	if ( !sd_bus_addr(pa, len, &bus) ) {
		return E_NOSPT;			/* the controller does not reach it */
	}
	ctrl = (UB)(sd_in8(SDHCI_HOST_CONTROL) & ~CTRL_DMA_MASK);

	switch ( sd.dma_mode ) {
	case SD_DMA_ADMA2_64:
		if ( !sd_bus_addr(sd.desc_pa, PAGE_SIZE, &desc) ) {
			return E_NOSPT;
		}
		er = adma_build(bus, len);
		if ( er < E_OK ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_ADMA2_64));
		sd_out64(SDHCI_ADMA_ADDRESS, desc);
		break;

	case SD_DMA_ADMA2_32:
		if ( (bus + len) > 0x100000000ULL || !sd_bus_addr(sd.desc_pa, PAGE_SIZE, &desc)
		  || desc >= 0x100000000ULL ) {
			return E_NOSPT;		/* the addresses do not reach it */
		}
		er = adma_build(bus, len);
		if ( er < E_OK ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_ADMA2_32));
		out_w(SDHCI_ADMA_ADDRESS, (UW)desc);
		break;

	case SD_DMA_SDMA:
		/* the single address register is 32 bits wide */
		if ( (bus + len) > 0x100000000ULL ) {
			return E_NOSPT;
		}
		sd_out8(SDHCI_HOST_CONTROL, (UB)(ctrl | CTRL_DMA_SDMA));
		out_w(SDHCI_SDMA_ADDRESS, (UW)bus);
		blksz = BLKSZ_SDMA_BOUND_512K;
		break;

	default:
		return E_NOSPT;
	}

	dcache_flush(buf, len);

	er = sd_xfer_begin(start, nsect, write, XFER_DMA_EN, blksz);
	if ( er < E_OK ) {
		sd_recover(er);
		return er;
	}
	er = sd_dma_wait(nsect);
	if ( er < E_OK ) {
		sd_recover(er);
		return er;
	}
	if ( !write ) {
		dcache_flush(buf, len);	/* drop what was read ahead of the transfer */
	}

	return E_OK;
}

/* Whether the controller reaches n sectors at p where they are */
LOCAL BOOL sd_reaches( CONST void *p, UD n )
{
	UD	len = n * BLK_SECTOR_SIZE, bus;

	if ( !buf_is_linear(p, len) ) {
		return TRUE;		/* not a question of reach: the data port takes it */
	}
	return sd_bus_addr((UD)VA2PA((UBINT)p), len, &bus);
}

LOCAL ER sd_execute_tuning( void );
LOCAL ER sd_give_up_uhs( ER why );
LOCAL UW sd_now_ms( void );

/*
 * A transfer, in pieces as large as one command carries. At 1.8V a
 * piece that fails with a CRC or end bit error is sent again after the
 * sampling point is tuned anew, as is every piece once the controller's
 * re-tuning timer has run out; one that still fails, or a tuning that
 * fails, takes the card back to 3.3V (sd_give_up_uhs) and is sent once
 * more there.
 */
LOCAL ER sd_xfer( UD start, void *buf, UD nsect, BOOL write )
{
	UB	*p = (UB *)buf;
	UD	n;
	ER	er;
	BOOL	retuned = FALSE, given_up = FALSE;

	while ( nsect > 0 ) {
		BOOL	pio = ( sd.dma_mode == SD_DMA_NONE );

		er = sd_wait_idle();
		if ( er < E_OK ) {
			return er;
		}
		if ( sd.tuned && sd.retune_ms != 0 && sd_now_ms() - sd.tuned_at >= sd.retune_ms ) {
			if ( sd_execute_tuning() < E_OK ) {
				if ( given_up ) return E_IO;
				given_up = TRUE;
				er = sd_give_up_uhs(E_IO);
				if ( er < E_OK ) return er;
				continue;
			}
		}
		sd_err_status = 0;

		n = ( nsect > SD_XFER_MAX ) ? SD_XFER_MAX : nsect;
		if ( sd.dma_mode == SD_DMA_SDMA && buf_is_linear(p, BLK_SECTOR_SIZE) ) {
			/*
			 * The single address mode is given runs that end at or
			 * before the next buffer boundary, so it never stops
			 * half way to ask for the rest (a pause not every
			 * controller resumes from). A sector that would straddle
			 * the boundary goes through the data port.
			 */
			UD	pa = (UD)VA2PA((UBINT)p);
			UD	fit = ( SDMA_BOUND - ( pa & ( SDMA_BOUND - 1 ) ) ) / BLK_SECTOR_SIZE;

			if ( fit == 0 ) {
				n = 1;
				pio = TRUE;
			} else if ( n > fit ) {
				n = fit;
			}
		}
		er = E_NOSPT;
		if ( !pio && sd.bounce != NULL
		  && ( ((UBINT)p & (DMA_ALIGN - 1)) != 0 || !sd_reaches(p, n) ) ) {
			/* off a cache line, or where the controller does not reach: through the bounce buffer */
			if ( n > BOUNCE_SECT ) n = BOUNCE_SECT;
			if ( write ) {
				knl_memcpy(sd.bounce, p, (SZ)( n * BLK_SECTOR_SIZE ));
			}
			er = sd_xfer_dma(start, sd.bounce, n, write);
			if ( er >= E_OK && !write ) {
				knl_memcpy(p, sd.bounce, (SZ)( n * BLK_SECTOR_SIZE ));
			}
		} else if ( !pio ) {
			er = sd_xfer_dma(start, p, n, write);
		}
		if ( er == E_NOSPT ) {
			er = sd_xfer_pio(start, p, n, write);
		}
		if ( er < E_OK ) {
			if ( sd.tuned && !retuned && ( sd_err_status & INT_SAMPLING_ERRORS ) != 0 ) {
				UW	why = sd_err_status;

				retuned = TRUE;
				if ( sd_execute_tuning() >= E_OK ) {
					tm_printf((UB*)"TessronOS: sd tuned again after a transfer failed (interrupts %08x)\n", why);
					continue;
				}
			}
			if ( sd.v18 && !given_up ) {
				given_up = TRUE;
				if ( sd_give_up_uhs(er) >= E_OK ) {
					continue;
				}
			}
			return er;
		}
		start += n;
		p += n * BLK_SECTOR_SIZE;
		nsect -= n;
	}
	return E_OK;
}

/*
 * Pick how data moves. The capability register says whether the
 * controller has the descriptor mode and whether its addresses are 64
 * bits wide. The table itself has to be memory the controller can reach,
 * and one page of it describes more than any request the layers above
 * make; without that page the single address mode is taken instead.
 */
LOCAL void sd_bounce_alloc( void )
{
	PFRAME	*pf = knl_alloc_pages(BOUNCE_ORDER, ZONE_DMA32, KAF_DMA32);

	sd.bounce = ( pf != NULL ) ? (UB *)PA2VA(knl_pf_to_pa(pf)) : NULL;
}

LOCAL void sd_dma_select( void )
{
	UW	caps = in_w(SDHCI_CAPABILITIES);

#if CNF_SD_DMA_MODE < 0
	if ( (caps & CAP_ADMA2) != 0 ) {
		sd.dma_mode = ( (caps & CAP_64BIT) != 0 )
			    ? SD_DMA_ADMA2_64 : SD_DMA_ADMA2_32;
	} else if ( (caps & CAP_SDMA) != 0 ) {
		sd.dma_mode = SD_DMA_SDMA;
	} else {
		sd.dma_mode = SD_DMA_NONE;
	}
#else
	sd.dma_mode = CNF_SD_DMA_MODE;
#endif

	if ( sd.dma_mode == SD_DMA_ADMA2_32 || sd.dma_mode == SD_DMA_ADMA2_64 ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

		if ( pf == NULL ) {
			sd.dma_mode = ( (caps & CAP_SDMA) != 0 )
				    ? SD_DMA_SDMA : SD_DMA_NONE;
		} else {
			sd.desc_pa = knl_pf_to_pa(pf);
			sd.desc    = (UW *)PA2VA(sd.desc_pa);
		}
	}
}

/*
 * Force the way data moves, for a test that runs every one of them over
 * the same card. A way the controller does not have, or the descriptor
 * modes without their table, answer E_NOSPT and change nothing. Answers
 * the way that was in use.
 */
EXPORT INT knl_sd_speed( UW *p_hz )
{
	if ( p_hz != NULL ) *p_hz = sd.clock_hz;
	if ( !sd.present ) return E_NOEXS;
	return sd.speed;
}

EXPORT INT knl_sd_dma_mode( INT mode )
{
	UW	caps;
	INT	old = sd.dma_mode;

	if ( mode < 0 ) {
		return old;
	}
	caps = in_w(SDHCI_CAPABILITIES);
	switch ( mode ) {
	case SD_DMA_NONE:
		break;
	case SD_DMA_SDMA:
		if ( (caps & CAP_SDMA) == 0 ) return E_NOSPT;
		break;
	case SD_DMA_ADMA2_32:
		if ( (caps & CAP_ADMA2) == 0 || sd.desc == NULL ) return E_NOSPT;
		break;
	case SD_DMA_ADMA2_64:
		if ( (caps & CAP_ADMA2) == 0 || (caps & CAP_64BIT) == 0
		  || sd.desc == NULL ) return E_NOSPT;
		break;
	default:
		return E_PAR;
	}
	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	sd.dma_mode = mode;
	tk_unl_mtx(sd.mtxid);
	return old;
}

LOCAL CONST UB *sd_dma_name( void )
{
	switch ( sd.dma_mode ) {
	case SD_DMA_SDMA:	return (CONST UB*)"SDMA";
	case SD_DMA_ADMA2_32:	return (CONST UB*)"ADMA2/32";
	case SD_DMA_ADMA2_64:	return (CONST UB*)"ADMA2/64";
	default:		return (CONST UB*)"PIO";
	}
}

/* ---------------------------------------------------------------- card */

/*
 * The size and the write protection from the card specific data. An SD
 * card of version 2 counts its size in 512KB units; an SD card of
 * version 1 and every MMC use the older form, which an MMC past 2GB
 * leaves at its largest and says its true size in the extended CSD.
 */
LOCAL void sd_csd( CONST UW *resp, BOOL mmc )
{
	UW	csd_ver = (resp[3] >> 30) & 3;

	sd.ccc = (resp[2] >> 20) & 0xfff;		/* bits 95:84 */

	if ( !mmc && csd_ver == 1 ) {
		/* version 2: (C_SIZE + 1) * 512KB */
		UD c_size = ((UD)(resp[1] >> 16) | ((UD)(resp[2] & 0x3f) << 16));
		sd.nsect = (c_size + 1) * 1024;
	} else {
		UD c_size = (((UD)(resp[2] & 0x3ff) << 2) | (resp[1] >> 30));
		UD mult   = 1UL << (((resp[1] >> 15) & 7) + 2);
		UD rdblk  = 1UL << ((resp[2] >> 16) & 0x0f);
		sd.nsect = (c_size + 1) * mult * rdblk / BLK_SECTOR_SIZE;
	}
	/*
	 * A card that says it is write protected is written to by no
	 * one: the protection is its own, set by a command, and a
	 * write it refused would come back as an error half way
	 * through what a file system meant to write. The two bits sit
	 * in the same place in both kinds of CSD.
	 */
	sd.wp = 0;
	if ( (resp[0] & CSD_TMP_WP) != 0 )  sd.wp |= SD_WP_TMP;
	if ( (resp[0] & CSD_PERM_WP) != 0 ) sd.wp |= SD_WP_PERM;
}

LOCAL void sd_wp_note( void )
{
	if ( sd.wp != 0 ) {
		tm_printf((UB*)"TessronOS: sd the card is%s%s%s: nothing is written to it\n",
			  ( sd.wp & SD_WP_TMP ) ? " write protected (temporary)" : "",
			  ( sd.wp & SD_WP_PERM ) ? " write protected (permanent)" : "",
			  ( sd.wp & SD_WP_LOCKED ) ? " locked by a password" : "");
	}
}

/*
 * The extended CSD of an MMC: one block through the data port. The
 * controller's DMA is not chosen yet when this runs, and the block is
 * read twice at most, so the simplest path is the one taken.
 */
LOCAL ER mmc_ext_csd( UB *buf )
{
	UW	w;
	INT	k;
	ER	er;

	er = sd_wait_idle();
	if ( er < E_OK ) return er;

	out_w(SDHCI_BLOCK_SIZE, (UW)EXT_CSD_SIZE | ( 1UL << 16 ));
	sd_out16(SDHCI_TRANSFER_MODE, XFER_READ | XFER_BLKCNT_EN);
	er = sd_cmd(CMD_MMC_SEND_EXT_CSD, 0, NULL);
	if ( er >= E_OK ) {
		er = wait_int(INT_BUF_READ_READY, NULL);
	}
	if ( er >= E_OK ) {
		for ( k = 0; k < EXT_CSD_SIZE / 4; k++ ) {
			w = in_w(SDHCI_BUFFER);
			buf[k * 4 + 0] = (UB)w;
			buf[k * 4 + 1] = (UB)(w >> 8);
			buf[k * 4 + 2] = (UB)(w >> 16);
			buf[k * 4 + 3] = (UB)(w >> 24);
		}
		er = wait_int(INT_XFER_COMPLETE, NULL);
	}
	if ( er < E_OK ) {
		sd_reset(RESET_CMD);
		sd_reset(RESET_DATA);
	}
	return er;
}

/*
 * Widen the bus. The device is told the new width by a switch command
 * and the controller by its host control register; whether the lines
 * really carry the data is seen by reading the extended CSD again and
 * comparing the part of it that only describes the device, which a
 * width the board has not wired garbles. Eight lines are tried where
 * the board wires them and the controller has them, then four; a width
 * that fails is switched back to one line.
 */
LOCAL UB	mmc_ext[2][EXT_CSD_SIZE];

LOCAL INT mmc_bus_width( void )
{
	UW	caps = in_w(SDHCI_CAPABILITIES);
	UW	resp[4];
	INT	width, i;
	UB	ctrl;
	ER	er;

	width = ( knl_board.sd_width >= 8 && (caps & CAP_8BIT) != 0 ) ? 8 : 4;
	for ( ; width >= 4; width -= 4 ) {
		er = sd_cmd(CMD_MMC_SWITCH, MMC_SWITCH(EXT_CSD_BUS_WIDTH, ( width == 8 ) ? 2 : 1), resp);
		if ( er >= E_OK ) {
			er = sd_cmd(CMD_SEND_STATUS, sd.rca, resp);
		}
		if ( er < E_OK || (resp[0] & R1_SWITCH_ERROR) != 0 ) {
			continue;			/* refused: nothing changed on the device */
		}
		ctrl = (UB)( sd_in8(SDHCI_HOST_CONTROL) & ~( CTRL_4BITBUS | CTRL_8BITBUS ) );
		sd_out8(SDHCI_HOST_CONTROL, ctrl | ( ( width == 8 ) ? CTRL_8BITBUS : CTRL_4BITBUS ));

		er = mmc_ext_csd(mmc_ext[1]);
		for ( i = EXT_CSD_PROPS; er >= E_OK && i < EXT_CSD_SIZE && mmc_ext[0][i] == mmc_ext[1][i]; i++ ) ;
		if ( er >= E_OK && i >= EXT_CSD_SIZE ) {
			return width;
		}
		tm_printf((UB*)"TessronOS: sd eMMC %d data lines do not read back (%d at byte %d)\n",
			  width, (INT)er, i);
		(void)sd_cmd(CMD_MMC_SWITCH, MMC_SWITCH(EXT_CSD_BUS_WIDTH, 0), resp);
		sd_out8(SDHCI_HOST_CONTROL, ctrl);
	}
	return 1;
}

/*
 * Bring an MMC up: idle, operating conditions, the address it is given,
 * the size, select, the bus width. Tried when the device did not answer
 * as an SD card, which leaves the command line to be reset first.
 */
LOCAL ER mmc_init_card( void )
{
	UW	resp[4], ocr = 0;
	INT	i;
	ER	er;

	sd_reset(RESET_CMD);
	sd.mmc = TRUE;
	sd.rca = 0;

	sd_step = 11;
	er = sd_cmd(CMD_GO_IDLE, 0, NULL);
	if ( er < E_OK ) return er;
	for ( i = 0; i < 1000; i++ ) {
		er = sd_cmd(CMD_MMC_SEND_OP_COND, MMC_OCR_ARG, resp);
		if ( er < E_OK ) return er;
		ocr = resp[0];
		if ( (ocr & OCR_BUSY) != 0 ) break;
		tk_dly_tsk(1);
	}
	if ( (ocr & OCR_BUSY) == 0 ) {
		return E_TMOUT;
	}
	sd.hc = ( (ocr & MMC_OCR_ACCESS) == MMC_OCR_SECTOR );

	sd_step = 12;
	er = sd_cmd(CMD_ALL_SEND_CID, 0, resp);
	if ( er < E_OK ) return er;
	sd.rca = MMC_RCA;
	er = sd_cmd(CMD_MMC_SET_REL_ADDR, sd.rca, resp);
	if ( er < E_OK ) return er;
	er = sd_cmd(CMD_SEND_CSD, sd.rca, resp);
	if ( er < E_OK ) return er;
	sd_csd(resp, TRUE);

	sd_step = 13;
	er = sd_cmd(CMD_SELECT_CARD, sd.rca, resp);
	if ( er < E_OK ) return er;
	if ( (resp[0] & R1_CARD_IS_LOCKED) != 0 ) {
		sd.wp |= SD_WP_LOCKED;
	}
	sd_wp_note();
	er = sd_cmd(CMD_SET_BLOCKLEN, BLK_SECTOR_SIZE, NULL);
	if ( er < E_OK ) return er;

	sd_step = 14;
	er = sd_set_clock(25000000);			/* the backward compatible timing */
	if ( er < E_OK ) return er;
	er = mmc_ext_csd(mmc_ext[0]);
	if ( er < E_OK ) return er;
	if ( sd.hc ) {
		CONST UB *c = &mmc_ext[0][EXT_CSD_SEC_COUNT];
		UD	n = (UD)c[0] | ((UD)c[1] << 8) | ((UD)c[2] << 16) | ((UD)c[3] << 24);

		if ( n == 0 ) return E_IO;
		sd.nsect = n;
	}

	sd_step = 15;
	sd.width = (UB)mmc_bus_width();
	sd.present = TRUE;

	return E_OK;
}

/* ---------------------------------------------------------------- the supplies of the slot */

LOCAL UW sd_now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return t.lo;
}

/*
 * The signal level, through the GPIO regulator: the pin driven to the
 * level of the voltage asked for and, when that changed it, the
 * regulator's settling time waited. Nothing on a machine without one.
 */
LOCAL void sd_vqmmc( BOOL v18 )
{
#ifndef RPI5
	(void)v18;
#else
	UINT	lv;

	if ( !sd_slot.reg ) return;
	lv = ( v18 ) ? sd_slot.io_1v8 : sd_slot.io_3v3;
	if ( gpio_get_dir(0, sd_slot.io_pin) == GPIO_DIR_OUT && (UINT)gpio_read(0, sd_slot.io_pin) == lv ) {
		return;
	}
	gpio_write(0, sd_slot.io_pin, lv);
	gpio_set_dir(0, sd_slot.io_pin, GPIO_DIR_OUT);
	sd_delay_us(sd_slot.settle_us);
#endif
}

/* The card's power, through the GPIO of the fixed regulator */
LOCAL void sd_vmmc( BOOL on )
{
#ifndef RPI5
	(void)on;
#else
	if ( !sd_slot.reg ) return;
	gpio_write(0, sd_slot.vcc_pin, ( on ) ? sd_slot.vcc_on : !sd_slot.vcc_on);
	gpio_set_dir(0, sd_slot.vcc_pin, GPIO_DIR_OUT);
#endif
}

/*
 * The lines at 3.3V: the host's 1.8V signalling bit cleared, the
 * regulator set, 5ms waited, and the bit seen still clear.
 */
LOCAL ER sd_signal_330( void )
{
	sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) & ~(UW)CTRL2_1V8);
	sd_vqmmc(FALSE);
	sd_delay_ms(5);
	sd.v18 = FALSE;
	return ( ( sd_in16(SDHCI_HOST_CONTROL2) & CTRL2_1V8 ) == 0 ) ? E_OK : E_IO;
}

/*
 * Power off: no interrupt signalled, the controller reset, its clock
 * stopped, the card's supply and the controller's bus power off, and a
 * millisecond for the supply to fall.
 */
LOCAL void sd_power_off( void )
{
	out_w(SDHCI_SIGNAL_ENABLE, 0);
	(void)sd_reset(RESET_ALL);
	out_w(SDHCI_INT_ENABLE, 0xFFFFFFFF);
	(void)sd_clock_write(0, 0);
	sd_vmmc(FALSE);
	sd_out8(SDHCI_POWER_CONTROL, 0);
	sd.tuned = FALSE;
	sd.pv_used = FALSE;
	sd_delay_ms(1);
}

/*
 * Power the card off and on again, which is the only way back to 3.3V
 * for a card that went to 1.8V: off, another millisecond, the supply on
 * with the controller's bus power at 3.3V, the lines at 3.3V, and the
 * time for the supply to come up. sd_init_card then gives the clock and
 * waits again before the first command.
 */
LOCAL void sd_power_cycle( void )
{
	sd_power_off();
	sd_delay_ms(1);
	sd_vmmc(TRUE);
	sd_out8(SDHCI_POWER_CONTROL, 0);
	sd_out8(SDHCI_POWER_CONTROL, 0x0f);		/* 3.3 volts, on */
	(void)sd_signal_330();
	sd_delay_ms(SD_POWER_UP_MS);
}

/*
 * The host's side of the switch to 1.8V: the clock stopped, the
 * regulator at 1.8V, the host's 1.8V signalling bit set and seen to
 * stay set, the clock kept stopped 10ms (the card asks for 5), and given
 * again at the speed it had.
 */
LOCAL ER sd_host_1v8( void )
{
	UW	hz = sd.clock_hz;

	(void)sd_clock_write(0, 0);
	sd_vqmmc(TRUE);
	sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) | CTRL2_1V8);
	if ( ( sd_in16(SDHCI_HOST_CONTROL2) & CTRL2_1V8 ) == 0
	  && ( sd_test & SD_TEST_NO_CMD11 ) == 0 ) {	/* a test's host may not have the bit */
		return E_IO;
	}
	sd.v18 = TRUE;
	sd_delay_ms(10);
	return sd_set_clock(hz);
}

/*
 * The switch to 1.8V, once the card has accepted it in its operating
 * conditions (S18A) and before CMD2. CMD11; the card answers and pulls
 * DAT[3:0] low within a millisecond; the host's side (sd_host_1v8); a
 * millisecond later the card has DAT[3:0] high again. A card that
 * refuses CMD11 has stayed at 3.3V. Any other failure leaves the card
 * in between, and its power is cycled.
 */
LOCAL INT sd_uhs_voltage( CONST char **why )
{
	UW	resp[4], lv;
	ER	er;

	if ( ( sd_test & SD_TEST_NO_CMD11 ) == 0 ) {
		resp[0] = 0;
		er = sd_cmd(CMD_VOLTAGE_SWITCH, 0, resp);
		if ( er < E_OK ) {
			*why = "the card did not answer the voltage switch (CMD11)";
			sd_power_cycle();
			return VS_FAIL;
		}
		if ( ( resp[0] & R1_ERRORS ) != 0 ) {
			*why = "the card refused the voltage switch (CMD11)";
			return VS_FAIL;
		}
		sd_delay_ms(1);
		lv = STATE_DAT_LEVEL(in_w(SDHCI_PRESENT_STATE));
		if ( lv != 0 ) {
			tm_printf((UB*)"TessronOS: sd after CMD11 DAT[3:0] %x, not 0\n", lv);
			*why = "the data lines did not go low after CMD11";
			sd_power_cycle();
			return VS_AGAIN;
		}
	}
	if ( sd_host_1v8() < E_OK ) {
		*why = "the host's 1.8V signalling did not become stable";
		sd_power_cycle();
		return VS_AGAIN;
	}
	sd_delay_ms(1);
	lv = STATE_DAT_LEVEL(in_w(SDHCI_PRESENT_STATE));
	if ( ( sd_test & SD_TEST_NO_CMD11 ) == 0 && lv != 0xf ) {
		tm_printf((UB*)"TessronOS: sd at 1.8V DAT[3:0] %x, not f\n", lv);
		*why = "the data lines did not come back high at 1.8V";
		sd_power_cycle();
		return VS_AGAIN;
	}
	return VS_OK;
}

/*
 * The UHS-I bus speeds the host offers on this slot: those the device
 * tree lists for the controller, on a controller of version 3.00 or
 * later, when the slot's supplies are known. For a test (SD_TEST_UHS)
 * all of them, on any machine.
 */
LOCAL UW sd_uhs_caps( void )
{
	if ( ( sd_test & SD_TEST_UHS ) != 0 ) {
		return HC_SDR50 | HC_SDR104 | HC_DDR50;
	}
#ifdef RPI5
	if ( sd_slot.reg && ( sd_in16(SDHCI_HOST_VERSION) & 0xff ) >= SDHCI_SPEC_300 ) {
		return sd_slot.caps;
	}
#endif
	return 0;
}

/* XPC: the 3.3V supply gives more than 150mA, by the controller's own word */
LOCAL BOOL sd_xpc( void )
{
	return ( ( in_w(SDHCI_MAX_CURRENT) & 0xff ) * 4 > 150 );
}

/*
 * Bring the card up: idle, voltage, address, select, four bit bus.
 * With 's18r' 1.8V is asked for in the operating conditions, and a card
 * that accepts it is switched before CMD2; *p_vs says how that went,
 * and when it failed nothing more is done here, so that the caller
 * starts again.
 */
LOCAL ER sd_init_card( BOOL s18r, INT *p_vs, CONST char **p_why )
{
	UW	resp[4], ocr = 0, arg;
	INT	i;
	ER	er;

	*p_vs = VS_NONE;

	/*
	 * The card detect switch of the Raspberry Pi 5's socket is wired to
	 * an always-on GPIO (active low), not necessarily to the controller,
	 * whose own bit may then say there is no card. The GPIO decides; the
	 * controller is told the card is in through its detect test level,
	 * as it may refuse to power the bus otherwise. The Compute Module 5
	 * has no such line: its eMMC is always there, and the Lite's carrier
	 * slot is found out by trying a card. There the same pin selects the
	 * Wi-Fi antenna and is not read at all.
	 */
#if defined(RPI5)
	if ( knl_board.sd == BOARD_SD_CD
	  && gpio_read(0, GIO_AON_SD_CD) == 1
	  && (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		return E_NOEXS;
	}
#elif defined(RPI4)
	/* the Raspberry Pi 4 family: no detect line the controller uses (broken-cd) */
#else
	if ( (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		return E_NOEXS;
	}
#endif
	sd_step = 1;
	er = sd_reset(RESET_ALL);
	if ( er < E_OK ) return er;
	if ( (in_w(SDHCI_PRESENT_STATE) & STATE_CARD_INSERTED) == 0 ) {
		sd_out8(SDHCI_HOST_CONTROL, sd_in8(SDHCI_HOST_CONTROL) | CTRL_CD_TEST);
	}

	sd_out8(SDHCI_POWER_CONTROL, 0x0e);		/* 3.3 volts */
	sd_out8(SDHCI_POWER_CONTROL, 0x0f);		/* and on */
	/*
	 * The Compute Module 5's eMMC lines are held at 1.8V by the module
	 * itself (its supply is 3.3V all the same); the controller is told
	 * the level the lines are at.
	 */
	if ( knl_board.sd_1v8 ) {
		sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) | CTRL2_1V8);
	}
	sd_out8(SDHCI_TIMEOUT_CONTROL, 0x0e);		/* the longest timeout */
	out_w(SDHCI_INT_ENABLE, 0xFFFFFFFF);		/* status bits, not signals */
	out_w(SDHCI_SIGNAL_ENABLE, 0);			/* polled, no interrupt yet */

	sd_step = 2;
	er = sd_set_clock(400000);			/* identification speed */
	if ( er < E_OK ) return er;
	sd_delay_ms(SD_POWER_UP_MS);			/* more than 74 clocks before the first command */

	sd_step = 3;
	sd.rca = 0;
	sd.mmc = FALSE;
	sd.width = 1;
	er = sd_cmd(CMD_GO_IDLE, 0, NULL);
	if ( er < E_OK ) return er;

	/*
	 * 2.7 to 3.6 volts, check pattern 0xAA. An eMMC ignores the command
	 * in its idle state (its CMD8 is another one, taken only once it is
	 * selected), which is how it is told from an SD card.
	 */
	sd_step = 4;
	er = sd_cmd(CMD_SEND_IF_COND, 0x1AA, resp);
	if ( er < E_OK || (resp[0] & 0xff) != 0xAA ) {
		return mmc_init_card();			/* not an SD 2.0 card: an MMC? */
	}

	/*
	 * The operating conditions: 2.7 to 3.6 volts, high capacity, XPC
	 * when the host gives more than 150mA, and S18R when 1.8V is asked
	 * for. The card's answer has S18A when it takes it, and then the
	 * switch comes before anything else.
	 */
	sd_step = 5;
	arg = OCR_VOLTAGE | OCR_HCS | ( sd_xpc() ? OCR_XPC : 0 ) | ( s18r ? OCR_S18R : 0 );
	for ( i = 0; i < 1000; i++ ) {
		er = sd_acmd(ACMD_SEND_OP_COND, arg, resp);
		if ( er < E_OK ) return er;
		ocr = resp[0];
		if ( (ocr & OCR_BUSY) != 0 ) break;
		tk_dly_tsk(1);
	}
	if ( (ocr & OCR_BUSY) == 0 ) {
		return E_TMOUT;
	}
	sd.hc = ( (ocr & OCR_HCS) != 0 );
	if ( s18r && ( ( ocr & OCR_S18R ) != 0 || ( sd_test & SD_TEST_S18A ) != 0 ) ) {
		sd_step = 9;
		*p_vs = sd_uhs_voltage(p_why);
		if ( *p_vs != VS_OK ) {
			return E_OK;
		}
	}

	sd_step = 6;
	er = sd_cmd(CMD_ALL_SEND_CID, 0, resp);
	if ( er < E_OK ) return er;

	er = sd_cmd(CMD_SEND_REL_ADDR, 0, resp);
	if ( er < E_OK ) return er;
	sd.rca = resp[0] & 0xFFFF0000;

	/* the size, from the card specific data */
	er = sd_cmd(CMD_SEND_CSD, sd.rca, resp);
	if ( er < E_OK ) return er;
	sd_csd(resp, FALSE);

	sd_step = 7;
	er = sd_cmd(CMD_SELECT_CARD, sd.rca, resp);
	if ( er < E_OK ) return er;
	if ( (resp[0] & R1_CARD_IS_LOCKED) != 0 ) {
		sd.wp |= SD_WP_LOCKED;
	}
	sd_wp_note();

	er = sd_acmd(ACMD_SET_BUS_WIDTH, 2, NULL);	/* four bits */
	if ( er >= E_OK ) {
		sd_out8(SDHCI_HOST_CONTROL, sd_in8(SDHCI_HOST_CONTROL) | CTRL_4BITBUS);
		sd.width = 4;
	}
	er = sd_cmd(CMD_SET_BLOCKLEN, BLK_SECTOR_SIZE, NULL);
	if ( er < E_OK ) return er;

	sd_step = 8;
	er = sd_set_clock(25000000);			/* full speed */
	if ( er < E_OK ) return er;

	sd.present = TRUE;

	return E_OK;
}

/* ---------------------------------------------------------------- the High Speed mode */

/*
 * A command that reads one short block of data through the data port:
 * the configuration register (ACMD51, 8 bytes) or the switch function
 * status (CMD6, 64 bytes). The block size, the count and the transfer
 * mode are written after the CMD55 of an application command, as the
 * transfer mode goes out with the next command written. The R1 status
 * of the command comes back in *p_r1.
 */
LOCAL ER sd_short_read( UH cmd, UW arg, BOOL app, UB *buf, INT len, UW *p_r1 )
{
	UW	w, resp[4];
	INT	k;
	ER	er;

	*p_r1 = 0;
	er = sd_wait_idle();
	if ( er < E_OK ) return er;
	if ( app ) {
		er = sd_cmd(CMD_APP_CMD, sd.rca, NULL);
		if ( er < E_OK ) return er;
	}
	out_w(SDHCI_BLOCK_SIZE, (UW)len | ( 1UL << 16 ));
	sd_out16(SDHCI_TRANSFER_MODE, XFER_READ | XFER_BLKCNT_EN);
	er = sd_cmd(cmd, arg, resp);
	if ( er >= E_OK ) {
		*p_r1 = resp[0];
		er = wait_int(INT_BUF_READ_READY, NULL);
	}
	if ( er >= E_OK ) {
		for ( k = 0; k < len / 4; k++ ) {
			w = in_w(SDHCI_BUFFER);
			buf[k * 4 + 0] = (UB)w;
			buf[k * 4 + 1] = (UB)(w >> 8);
			buf[k * 4 + 2] = (UB)(w >> 16);
			buf[k * 4 + 3] = (UB)(w >> 24);
		}
		er = wait_int(INT_XFER_COMPLETE, NULL);
	}
	if ( er >= E_OK && ( *p_r1 & R1_ERRORS ) != 0 ) {
		er = E_IO;
	}
	if ( er < E_OK ) {
		sd_reset(RESET_CMD);
		sd_reset(RESET_DATA);
	}
	return er;
}

/* The card in the transfer state and ready for data: E_OK, with its status in *p_r1 */
LOCAL ER sd_tran_ready( UW *p_r1 )
{
	UW	resp[4];
	INT	i;
	ER	er = E_TMOUT;

	for ( i = 0; i < SD_INT_MS; i++ ) {
		resp[0] = 0;
		er = sd_cmd(CMD_SEND_STATUS, sd.rca, resp);
		*p_r1 = resp[0];
		if ( er >= E_OK && ( resp[0] & R1_ERRORS ) == 0 && R1_STATE(resp[0]) == R1_STATE_TRAN
		  && ( resp[0] & R1_READY_FOR_DATA ) != 0 ) {
			return E_OK;
		}
		sd_nap();
	}
	return ( er < E_OK ) ? er : E_IO;
}

/*
 * The preset value of a UHS-I bus speed, and the clock it gives: the
 * divided clock's divisor is twice the value (the base clock itself for
 * 0), the programmable clock's the value plus one over the multiplied
 * base. FALSE when there is none to use, or it would run the bus faster
 * than the bus speed allows.
 */
LOCAL BOOL sd_preset( INT speed, UH *p_pv, UW *p_sel, UW *p_hz )
{
	CONST SDSPEED	*s = &sd_speeds[speed];
	UW	pv, div, mul, hz, base = sd_base_clock();

	if ( s->preset == 0 ) {
		return FALSE;
	}
	pv = sd_in16(SDHCI_BASE + s->preset);
	div = PRESET_DIV(pv);
	mul = CAP1_CLK_MUL(in_w(SDHCI_CAPABILITIES1));
	if ( ( pv & PRESET_CLKGEN ) != 0 && mul != 0 ) {
		hz = (UW)( (UD)base * ( mul + 1 ) / ( div + 1 ) );
		*p_sel = CLOCK_PROG_MODE | CLOCK_DIV_SEL(div);
	} else {
		hz = ( div == 0 ) ? base : base / ( div * 2 );
		*p_sel = CLOCK_DIV_SEL(div);
	}
	*p_pv = (UH)pv;
	*p_hz = hz;
	return ( hz != 0 && hz <= s->max_hz );
}

/*
 * The host's side of a bus speed. The card's clock is stopped while
 * the timing changes, as the specification asks of a controller whose
 * preset values may be on. The High Speed Enable bit of the host
 * control register is set for every bus speed above 25MHz. On a
 * controller of version 3.00 or later, the driver strength is type B
 * and the UHS mode select is the bus speed's (none for the default
 * speed and the High Speed mode); a UHS-I bus speed takes its clock and
 * driver strength from its preset value, which the controller then
 * applies itself. Without a preset value to use, the clock is 'hz'.
 */
LOCAL ER sd_host_timing( INT speed, UW hz )
{
	UW	clk, ctrl2, sel = 0, phz = 0;
	UH	pv = 0;
	BOOL	hisp = ( speed != SD_SPEED_DEFAULT && speed != SD_SPEED_SDR12 );

	clk = sd_in16(SDHCI_CLOCK_CONTROL);
	if ( ( clk & CLOCK_CARD_EN ) != 0 ) {
		sd_out16(SDHCI_CLOCK_CONTROL, clk & ~(UW)CLOCK_CARD_EN);
	}
	sd_out8(SDHCI_HOST_CONTROL, hisp ? ( sd_in8(SDHCI_HOST_CONTROL) | CTRL_HISPD )
					 : ( sd_in8(SDHCI_HOST_CONTROL) & ~(UW)CTRL_HISPD ));
	sd.pv_used = FALSE;
	if ( ( sd_in16(SDHCI_HOST_VERSION) & 0xff ) >= SDHCI_SPEC_300 ) {
		ctrl2 = sd_in16(SDHCI_HOST_CONTROL2) & ~(UW)( CTRL2_UHS_MASK | CTRL2_DRV_MASK | CTRL2_PRESET_EN );
		if ( speed >= SD_SPEED_SDR12 ) {
			ctrl2 |= sd_speeds[speed].uhs;
		}
		sd_out16(SDHCI_HOST_CONTROL2, ctrl2);
		if ( speed >= SD_SPEED_SDR12 && sd_preset(speed, &pv, &sel, &phz) ) {
			sd_out16(SDHCI_HOST_CONTROL2, ctrl2 | CTRL2_PRESET_EN);
			sd.pv_used = TRUE;
			sd.pv = pv;
			return sd_clock_write(sel, phz);
		}
		sd.pv = pv;
	}
	return sd_set_clock(hz);
}

/*
 * The configuration register (ACMD51) and the switch status of mode 0
 * (CMD6), kept in sd.scr and sd.sw: a card of version 1.10 or later
 * (SD_SPEC 1 or more) whose CSD lists command class 10 has the switch
 * function. While the High Speed function is busy (bit 1 of byte 29)
 * the status is asked again, four times at most. Answers why there is
 * no switch status, NULL when there is.
 */
LOCAL CONST char *sd_switch_info( void )
{
	UW	r1 = 0;
	INT	k;
	ER	er;

	sd.sw_ok = FALSE;
	er = sd_short_read(ACMD_SEND_SCR, 0, TRUE, sd.scr, SD_SCR_LEN, &r1);
	if ( er < E_OK ) {
		knl_memset(sd.scr, 0, sizeof(sd.scr));
		return "the configuration register (ACMD51) was not read";
	}
	if ( SCR_SD_SPEC(sd.scr) < 1 || ( sd.ccc & CSD_CCC_SWITCH ) == 0 ) {
		return "the card has no switch function (SD_SPEC 1.0, or no command class 10)";
	}
	for ( k = 0; k < 4; k++ ) {
		er = sd_short_read(CMD_SD_SWITCH, SD_SWITCH_CHECK, FALSE, sd.sw, SD_STATUS_LEN, &r1);
		if ( er < E_OK || ( sd.sw[29] & 0x02 ) == 0 ) break;
	}
	if ( er < E_OK ) {
		return "the switch function (CMD6 mode 0) failed";
	}
	sd.sw_ok = TRUE;
	return NULL;
}

/* CMD6 in mode 1: function group 'group' (the shift of its four bits / 4) set to 'func' */
LOCAL ER sd_switch_set( INT group, UW func, UB *st, UW *p_r1 )
{
	UW	arg = ( SW_SET | 0x00FFFFFF ) & ~( (UW)0xf << ( group * 4 ) );

	return sd_short_read(CMD_SD_SWITCH, arg | ( func << ( group * 4 ) ), FALSE, st, SD_STATUS_LEN, p_r1);
}

/*
 * The SD card switched to the High Speed mode (50MHz at 3.3V), after the
 * card is up in the default speed with its bus four lines wide and its
 * switch status read (sd_switch_info). The steps are those of the SD
 * specification's switch function:
 *
 *	1. Bit 1 of byte 13 of the switch status of mode 0 is the High
 *	   Speed function.
 *	2. 32KB from the start of the card are read through the data port
 *	   in the default speed, to compare with.
 *	3. CMD6 in mode 1 switches group 1 to function 1; the function the
 *	   card took is the low four bits of byte 16 of the status.
 *	4. The host: the High Speed Enable bit, then the clock at 50MHz.
 *	5. The same 32KB are read again and compared, and the card must be
 *	   in the transfer state.
 *
 * A step that fails leaves the card in the default speed: the host's
 * bit cleared and the clock at 25MHz again and, when the card had been
 * switched, the card switched back to function 0. 'why' says where a
 * switch did not happen; 'info_why' is why there is no switch status.
 */
LOCAL BOOL sd_high_speed( CONST char *info_why, CONST char **why )
{
	UB	*st, *ref, *now;
	PFRAME	*pf;
	UW	r1 = 0;
	UD	i;
	BOOL	card_hs = FALSE, ok = FALSE;
	ER	er = E_OK;

	*why = NULL;
	sd.speed = SD_SPEED_DEFAULT;
	if ( ( in_w(SDHCI_CAPABILITIES) & CAP_HISPD ) == 0 ) {
		*why = "the controller has no High Speed mode";
		return FALSE;
	}
	if ( !sd.sw_ok ) {
		*why = info_why;
		return FALSE;
	}
	if ( ( SW_BUS_MODES(sd.sw) & 0x02 ) == 0 ) {
		*why = "the card has no High Speed function";
		return FALSE;
	}
	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_DMA32);	/* 64KB: two blocks of 32KB */
	if ( pf == NULL ) {
		*why = "no memory to check the switch";
		return FALSE;
	}
	ref = (UB *)PA2VA(knl_pf_to_pa(pf));
	now = ref + SD_HS_CHECK_SECT * BLK_SECTOR_SIZE;
	st = now;					/* the status, before the second read needs the room */

	/* 2. the start of the card in the default speed, to compare with */
	er = sd_xfer_pio(0, ref, SD_HS_CHECK_SECT, FALSE);
	if ( er < E_OK ) {
		*why = "the card could not be read in the default speed";
		goto out;
	}

	/* 3. the switch; from here the card is set back if anything fails */
	card_hs = TRUE;
	er = sd_short_read(CMD_SD_SWITCH, SD_SWITCH_HS, FALSE, st, SD_STATUS_LEN, &r1);
	if ( er < E_OK ) {
		*why = "the switch function (CMD6 mode 1) failed";
		goto out;
	}
	if ( ( st[16] & 0x0f ) != 1 ) {
		*why = "the card did not take the High Speed function";
		goto out;
	}

	/* 4. the host */
	er = sd_host_timing(SD_SPEED_HS, SD_DS_HZ);
	if ( er >= E_OK ) er = sd_set_clock(SD_HS_HZ);
	if ( er < E_OK ) {
		*why = "the clock did not become stable at 50MHz";
		goto out;
	}

	/* 5. the same bytes again, and the card ready */
	for ( i = 0; i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE; i++ ) now[i] = (UB)~ref[i];
	er = sd_xfer_pio(0, now, SD_HS_CHECK_SECT, FALSE);
	if ( er < E_OK ) {
		*why = "the card could not be read in the High Speed mode";
		goto out;
	}
	for ( i = 0; i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE && now[i] == ref[i]; i++ ) ;
	if ( i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE ) {
		*why = "what was read in the High Speed mode differed";
		goto out;
	}
	er = sd_tran_ready(&r1);
	if ( er < E_OK ) {
		*why = "the card was not ready after the switch";
		goto out;
	}
	ok = TRUE;

    out:
	if ( !ok ) {
		UW	intst = in_w(SDHCI_INT_STATUS), state = in_w(SDHCI_PRESENT_STATE);

		/* the default speed again, the host first so that the card can be told */
		(void)sd_host_timing(SD_SPEED_DEFAULT, SD_DS_HZ);
		if ( card_hs ) {
			UW	r1b = 0;

			(void)sd_short_read(CMD_SD_SWITCH, SD_SWITCH_DEFAULT, FALSE, st, SD_STATUS_LEN, &r1b);
			(void)sd_tran_ready(&r1b);
		}
		tm_printf((UB*)"TessronOS: sd the High Speed mode not used (%d): R1 %08x, state %08x, interrupts %08x\n",
			  (INT)er, r1, state, intst);
	}
	knl_free_pages(pf, 4);
	sd.speed = ( ok ) ? SD_SPEED_HS : SD_SPEED_DEFAULT;
	return ok;
}

/* ---------------------------------------------------------------- UHS-I */

/*
 * The tuning: the controller's Execute Tuning bit set, and CMD19 sent
 * again and again, each bringing a 64 byte block the controller samples
 * at another point, until the controller clears the bit. It is done
 * when the Sampling Clock Select bit is then set. The block is not read
 * out; only the buffer read ready status is waited for, the one the
 * controller gives during the tuning, 50ms at most, and a millisecond
 * passes between blocks. No block, or no sampling point after 40,
 * clears both bits, and the controller keeps sampling at the fixed
 * point.
 */
LOCAL ER sd_execute_tuning( void )
{
	UW	ctrl2;
	INT	i;
	ER	er = E_IO;

	sd.tuned = FALSE;
	sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) | CTRL2_EXEC_TUNING);
	out_w(SDHCI_INT_ENABLE, INT_BUF_READ_READY);
	for ( i = 0; i < SD_TUNING_LOOPS; i++ ) {
		er = wait_state(STATE_CMD_INHIBIT, SD_SPIN);
		if ( er >= E_OK ) {
			out_w(SDHCI_BLOCK_SIZE, (UW)SD_TUNING_LEN | ( 1UL << 16 ));
			out_w(SDHCI_INT_STATUS, 0xFFFFFFFF);
			out_w(SDHCI_ARGUMENT, 0);
			sd_out16(SDHCI_TRANSFER_MODE, XFER_READ);
			sd_out16(SDHCI_COMMAND, CMD_SEND_TUNING);
			er = wait_int_ms(INT_BUF_READ_READY, NULL, SD_TUNING_MS);
		}
		if ( er < E_OK ) {
			/* no block: the tuning is abandoned and the lines reset */
			sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) & ~(UW)( CTRL2_EXEC_TUNING | CTRL2_TUNED_CLK ));
			sd_reset(RESET_CMD);
			sd_reset(RESET_DATA);
			break;
		}
		sd_delay_ms(1);
		ctrl2 = sd_in16(SDHCI_HOST_CONTROL2);
		if ( ( ctrl2 & CTRL2_EXEC_TUNING ) == 0 ) {
			er = ( ( ctrl2 & CTRL2_TUNED_CLK ) != 0 ) ? E_OK : E_IO;
			break;
		}
		er = E_IO;
	}
	if ( er < E_OK ) {
		sd_out16(SDHCI_HOST_CONTROL2, sd_in16(SDHCI_HOST_CONTROL2) & ~(UW)( CTRL2_EXEC_TUNING | CTRL2_TUNED_CLK ));
	}
	out_w(SDHCI_INT_ENABLE, 0xFFFFFFFF);
	out_w(SDHCI_INT_STATUS, 0xFFFFFFFF);
	if ( er >= E_OK ) {
		sd.tuned = TRUE;
		sd.tuned_at = sd_now_ms();
	} else {
		tm_printf((UB*)"TessronOS: sd the tuning failed (%d) after %d blocks, host control 2 %04x\n",
			  (INT)er, i + 1, sd_in16(SDHCI_HOST_CONTROL2));
	}
	return er;
}

/*
 * The fastest UHS-I bus speed both the host and the card have: SDR104,
 * then DDR50, then SDR50, then SDR25. The card's bus speeds are those
 * of its switch status at 1.8V, which a card of version 3.0x reports.
 * SDR12 alone is no faster than the default speed: none then.
 */
LOCAL INT sd_uhs_pick( UW host )
{
	UW	modes = SCR_SPEC3(sd.scr) ? SW_BUS_MODES(sd.sw) : 0;

	if ( ( host & HC_SDR104 ) != 0 && ( modes & SW_FUNC_SDR104 ) != 0 ) return SD_SPEED_SDR104;
	if ( ( host & HC_DDR50 ) != 0 && ( modes & SW_FUNC_DDR50 ) != 0 ) return SD_SPEED_DDR50;
	if ( ( host & ( HC_SDR104 | HC_SDR50 ) ) != 0 && ( modes & SW_FUNC_SDR50 ) != 0 ) return SD_SPEED_SDR50;
	if ( ( modes & 0x02 ) != 0 ) return SD_SPEED_SDR25;
	return SD_SPEED_DEFAULT;
}

/*
 * A card at 1.8V brought to a UHS-I bus speed, after it is up in SDR12
 * (25MHz) with its bus four lines wide and its switch status read:
 *
 *	1. The bus speed (sd_uhs_pick).
 *	2. 32KB from the start of the card are read through the data port
 *	   at SDR12, to compare with.
 *	3. For SDR50, SDR104 and DDR50, the current limit (CMD6 group 4):
 *	   the largest of 800, 600, 400 and 200mA that both the host's 3.3V
 *	   supply (its maximum current register) and the card have. Nothing
 *	   is sent when the host does not say.
 *	4. The driver strength stays type B: nothing is sent.
 *	5. CMD6 in mode 1 sets group 1 to the bus speed, which byte 16 of the
 *	   status must show.
 *	6. The host: the bus speed's timing, preset value and clock
 *	   (sd_host_timing), the clock at the bus speed's fastest or the
 *	   device tree's max-frequency, whichever is less.
 *	7. The tuning for SDR104 and DDR50, and for SDR50 when the controller
 *	   says SDR50 needs it. DDR50 goes on without it when it fails.
 *	8. The same 32KB read again and compared, and the card in the
 *	   transfer state.
 *
 * A step that fails answers FALSE with 'why'; the caller cycles the
 * card's power and takes it at 3.3V.
 */
LOCAL BOOL sd_uhs_card( UW host, CONST char *info_why, CONST char **why )
{
	UB	st[SD_STATUS_LEN];
	UB	*ref, *now;
	PFRAME	*pf;
	UW	r1 = 0, maxc, cur, hz, caps1 = in_w(SDHCI_CAPABILITIES1);
	INT	speed, limit = -1;
	UD	i;
	BOOL	ok = FALSE;
	ER	er = E_OK;

	*why = NULL;
	if ( !sd.sw_ok ) {
		*why = info_why;
		return FALSE;
	}
	speed = sd_uhs_pick(host);
	if ( speed == SD_SPEED_DEFAULT ) {
		*why = "the card offers no UHS-I bus speed above SDR12";
		return FALSE;
	}
	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_DMA32);	/* 64KB: two blocks of 32KB */
	if ( pf == NULL ) {
		*why = "no memory to check the switch";
		return FALSE;
	}
	ref = (UB *)PA2VA(knl_pf_to_pa(pf));
	now = ref + SD_HS_CHECK_SECT * BLK_SECTOR_SIZE;

	er = sd_xfer_pio(0, ref, SD_HS_CHECK_SECT, FALSE);
	if ( er < E_OK ) {
		*why = "the card could not be read at SDR12";
		goto out;
	}

	if ( speed == SD_SPEED_SDR50 || speed == SD_SPEED_SDR104 || speed == SD_SPEED_DDR50 ) {
		maxc = ( in_w(SDHCI_MAX_CURRENT) & 0xff ) * 4;
		cur = SW_CURRENTS(sd.sw);
		if ( maxc >= 800 && ( cur & 0x08 ) != 0 ) limit = 3;
		else if ( maxc >= 600 && ( cur & 0x04 ) != 0 ) limit = 2;
		else if ( maxc >= 400 && ( cur & 0x02 ) != 0 ) limit = 1;
		else if ( maxc >= 200 && ( cur & 0x01 ) != 0 ) limit = 0;
		if ( limit >= 0 ) {
			er = sd_switch_set(SW_GROUP_CURRENT, (UW)limit, st, &r1);
			if ( er < E_OK ) {
				*why = "the current limit (CMD6 group 4) failed";
				goto out;
			}
			if ( ( ( st[15] >> 4 ) & 0x0f ) != (UW)limit ) {
				tm_printf((UB*)"TessronOS: sd the card did not take the current limit %d (%02x)\n",
					  limit, st[15]);
			}
		}
	}

	er = sd_switch_set(SW_GROUP_BUS, sd_speeds[speed].func, st, &r1);
	if ( er < E_OK ) {
		*why = "the switch function (CMD6 mode 1) failed";
		goto out;
	}
	if ( ( st[16] & 0x0f ) != sd_speeds[speed].func ) {
		*why = "the card did not take the bus speed";
		er = E_IO;
		goto out;
	}

	hz = sd_speeds[speed].max_hz;
#ifdef RPI5
	if ( sd_slot.f_max != 0 && hz > sd_slot.f_max ) hz = sd_slot.f_max;
#endif
	er = sd_host_timing(speed, SD_DS_HZ);
	if ( er >= E_OK && !sd.pv_used ) er = sd_set_clock(hz);
	if ( er < E_OK ) {
		*why = "the clock did not become stable";
		goto out;
	}
	sd.speed = speed;

	if ( sd_speeds[speed].tune == 1
	  || ( sd_speeds[speed].tune == 2 && ( caps1 & CAP1_SDR50_TUNING ) != 0 ) ) {
		er = sd_execute_tuning();
		if ( er < E_OK && speed != SD_SPEED_DDR50 ) {
			*why = "the tuning (CMD19) failed";
			goto out;
		}
		er = E_OK;
		if ( sd.tuned && CAP1_RETUNE_MODE(caps1) == 0 && CAP1_RETUNE_COUNT(caps1) != 0 ) {
			sd.retune_ms = ( 1UL << ( CAP1_RETUNE_COUNT(caps1) - 1 ) ) * 1000;
		}
	}

	for ( i = 0; i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE; i++ ) now[i] = (UB)~ref[i];
	er = sd_xfer_pio(0, now, SD_HS_CHECK_SECT, FALSE);
	if ( er < E_OK ) {
		*why = "the card could not be read at the bus speed";
		goto out;
	}
	if ( ( sd_test & SD_TEST_BAD_READ ) != 0 ) {
		now[100] ^= 0x01;
	}
	for ( i = 0; i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE && now[i] == ref[i]; i++ ) ;
	if ( i < SD_HS_CHECK_SECT * BLK_SECTOR_SIZE ) {
		*why = "what was read at the bus speed differed";
		er = E_IO;
		goto out;
	}
	er = sd_tran_ready(&r1);
	if ( er < E_OK ) {
		*why = "the card was not ready after the switch";
		goto out;
	}
	ok = TRUE;

    out:
	if ( !ok ) {
		tm_printf((UB*)"TessronOS: sd %s not used (%d): R1 %08x, state %08x, interrupts %08x, host control 2 %04x\n",
			  sd_speeds[speed].name, (INT)er, r1, in_w(SDHCI_PRESENT_STATE), sd_err_status,
			  sd_in16(SDHCI_HOST_CONTROL2));
		sd.speed = SD_SPEED_DEFAULT;
		sd.tuned = FALSE;
		sd.retune_ms = 0;
	}
	knl_free_pages(pf, 4);
	return ok;
}

/*
 * Whether a card at 3.3V reports bus speeds only a card at 1.8V has:
 * then it was left at 1.8V by whoever had it before, with its power on.
 */
LOCAL BOOL sd_card_left_v18( void )
{
	return sd.sw_ok && SCR_SPEC3(sd.scr)
	    && ( SW_BUS_MODES(sd.sw) & ( SW_FUNC_SDR50 | SW_FUNC_SDR104 | SW_FUNC_DDR50 ) ) != 0;
}

/*
 * Bring the card up at the fastest bus speed that works, from whatever
 * state it is in.
 *
 *	1. A card whose lines are at 1.8V from an earlier round has its
 *	   power cycled first.
 *	2. sd_init_card, asking for 1.8V when the host offers UHS-I on this
 *	   slot ('allow_uhs'). A switch that failed in a way worth another
 *	   try is tried again, ten times at most, each after the card's
 *	   power was cycled; any other failure gives 1.8V up.
 *	3. An eMMC is done.
 *	4. The configuration register and the switch status.
 *	5. At 1.8V, the UHS-I bus speed (sd_uhs_card). When that fails, the
 *	   card's power is cycled and the card is taken at 3.3V.
 *	6. At 3.3V, a card that reports 1.8V bus speeds was left at 1.8V:
 *	   the host's side is switched to it (sd_host_1v8) and the UHS-I bus
 *	   speed tried, once; when that fails, as in 5, but the next round
 *	   asks for 1.8V again.
 *	7. Otherwise the High Speed mode, or the default speed.
 *
 * *p_uhs says why UHS-I is not used where the host offers it, *p_hs why
 * the High Speed mode is not.
 */
LOCAL ER sd_bringup( BOOL allow_uhs, CONST char **p_uhs, CONST char **p_hs )
{
	UW	host = sd_uhs_caps();
	INT	tries = SD_S18R_TRIES, vs, round;
	BOOL	uhs_off = !allow_uhs, left_tried = FALSE, cycle = FALSE, done = FALSE, s18r;
	CONST char *why = NULL, *info_why;
	ER	er = E_IO;

	*p_uhs = NULL;
	*p_hs = NULL;
	sd_in_bringup = TRUE;
	sd.cmd23_card = FALSE;
	for ( round = 0; round < SD_S18R_TRIES + 4 && !done; round++ ) {
		if ( cycle || sd.v18 ) {
			sd_power_cycle();
			cycle = FALSE;
		}
		sd.speed = SD_SPEED_DEFAULT;
		sd.tuned = FALSE;
		sd.retune_ms = 0;
		s18r = ( host != 0 && !uhs_off && tries > 0 );
		er = sd_init_card(s18r, &vs, &why);
		if ( er < E_OK ) {
			break;
		}
		if ( vs == VS_AGAIN ) {
			tries--;
			*p_uhs = why;
			continue;
		}
		if ( vs == VS_FAIL ) {
			uhs_off = TRUE;
			*p_uhs = why;
			continue;
		}
		if ( sd.mmc ) {
			done = TRUE;
			break;
		}
		if ( s18r && vs == VS_NONE ) {
			*p_uhs = "the card did not accept 1.8V (no S18A)";
		}
		info_why = sd_switch_info();
		if ( sd.v18 ) {
			if ( sd_uhs_card(host, info_why, &why) ) {
				*p_uhs = NULL;
				done = TRUE;
				break;
			}
			*p_uhs = why;
			uhs_off = TRUE;
			continue;
		}
		if ( host != 0 && !uhs_off && !left_tried && sd_card_left_v18() ) {
			left_tried = TRUE;
			why = "the host could not follow the card to 1.8V";
			if ( sd_host_1v8() >= E_OK && sd_uhs_card(host, info_why, &why) ) {
				*p_uhs = NULL;
				done = TRUE;
				break;
			}
			*p_uhs = why;
			cycle = TRUE;
			continue;
		}
		if ( host != 0 && *p_uhs == NULL ) {
			*p_uhs = ( !allow_uhs ) ? "not tried" : "the card was not switched to 1.8V";
		}
		(void)sd_high_speed(info_why, p_hs);
		done = TRUE;
	}
	sd_in_bringup = FALSE;
	if ( er >= E_OK && ( !done || !sd.present ) ) {
		er = E_IO;
	}
	if ( er < E_OK ) {
		sd.present = FALSE;
		return er;
	}
	/* SET_BLOCK_COUNT, when the configuration register says the card takes it (SD only) */
	sd.cmd23_card = ( !sd.mmc && ( SCR_CMD_SUPPORT(sd.scr) & SCR_CMD23 ) != 0 );
	return E_OK;
}

/* The bus speed in one line, and why a faster one is not used */
LOCAL void sd_speed_note( CONST char *uhs_why, CONST char *hs_why )
{
	INT	mhz = (INT)( sd.clock_hz / 1000000 );

	if ( sd.mmc ) {
		return;
	}
	if ( sd.speed >= SD_SPEED_SDR12 ) {
		tm_printf((UB*)"sda: SD card, %s %d MHz%s\n", sd_speeds[sd.speed].name, mhz,
			  ( sd.tuned ) ? ", tuned" : "");
	} else if ( sd.speed == SD_SPEED_HS ) {
		tm_printf((UB*)"sda: SD card, High Speed %d MHz%s%s\n", mhz,
			  ( uhs_why != NULL ) ? "; UHS-I not used: " : "", ( uhs_why != NULL ) ? uhs_why : "");
	} else {
		tm_printf((UB*)"sda: SD card, default speed %d MHz: %s%s%s\n", mhz,
			  ( hs_why != NULL ) ? hs_why : "",
			  ( uhs_why != NULL ) ? "; UHS-I not used: " : "", ( uhs_why != NULL ) ? uhs_why : "");
	}
}

/*
 * A transfer failed at 1.8V and tuning anew did not help: the card's
 * power is cycled and it is brought up at 3.3V for good.
 */
LOCAL ER sd_give_up_uhs( ER why )
{
	CONST char	*uhs_why, *hs_why;
	ER		er;

	tm_printf((UB*)"TessronOS: sd %s fails (%d, interrupts %08x); back to 3.3V\n",
		  sd_speeds[sd.speed].name, (INT)why, sd_err_status);
	er = sd_bringup(FALSE, &uhs_why, &hs_why);
	if ( er < E_OK ) {
		tm_printf((UB*)"TessronOS: sd the card did not come up again (%d at step %d)\n", (INT)er, sd_step);
		return er;
	}
	sd_speed_note("given up after a transfer failed", hs_why);
	return E_OK;
}

/*
 * Before anything is written: read the start of the card through the
 * data port ended by CMD12, and through the way chosen ended the way
 * chosen (sd_cmd23_way), and compare. Reading changes nothing on the
 * card. When the two differ and CMD23 was in use, the way chosen is
 * read again ended by CMD12: if that agrees, CMD23 is not used; if not,
 * the way chosen is not, so that it never gets to write.
 */
#define SD_CHECK_SECT		64		/* 32KB: the partition table and more */

LOCAL CONST UB *sd_cmd23_name( INT way )
{
	switch ( way ) {
	case SD_CMD23_MANUAL:	return (CONST UB*)"CMD23";
	case SD_CMD23_AUTO:	return (CONST UB*)"Auto-CMD23";
	default:		return (CONST UB*)"CMD12";
	}
}

LOCAL void sd_check_read( UB *buf, INT mode, INT way, ER *p_er )
{
	INT	old_mode = sd.dma_mode, old_force = sd.cmd23_force;
	UD	i;

	for ( i = 0; i < SD_CHECK_SECT * BLK_SECTOR_SIZE; i++ ) {
		buf[i] = (UB)( 0x5A ^ ( mode * 0x11 + way ) );
	}
	sd.dma_mode = mode;
	sd.cmd23_force = way;
	*p_er = sd_xfer(0, buf, SD_CHECK_SECT, FALSE);
	sd.dma_mode = old_mode;
	sd.cmd23_force = old_force;
}

LOCAL void sd_dma_check( void )
{
	PFRAME	*pf;
	UB	*a, *b;
	UD	i;
	ER	er1, er2;
	INT	mode = sd.dma_mode, way = sd_cmd23_way();

	if ( mode == SD_DMA_NONE && way == SD_CMD23_NONE ) {
		return;
	}
	pf = knl_alloc_pages(4, ZONE_DMA32, KAF_DMA32);	/* 64KB: two buffers of 32KB */
	if ( pf == NULL ) {
		sd.dma_mode = SD_DMA_NONE;
		sd.cmd23_card = FALSE;
		tm_printf((UB*)"TessronOS: sd no memory to check %s and %s; using the data port and CMD12\n",
			  sd_dma_name(), sd_cmd23_name(way));
		return;
	}
	a = (UB *)PA2VA(knl_pf_to_pa(pf));
	b = a + SD_CHECK_SECT * BLK_SECTOR_SIZE;

	sd_check_read(a, SD_DMA_NONE, SD_CMD23_NONE, &er1);
	sd_check_read(b, mode, way, &er2);
	for ( i = 0; i < SD_CHECK_SECT * BLK_SECTOR_SIZE && a[i] == b[i]; i++ ) ;

	if ( ( er1 < E_OK || er2 < E_OK || i < SD_CHECK_SECT * BLK_SECTOR_SIZE ) && way != SD_CMD23_NONE ) {
		tm_printf((UB*)"TessronOS: sd %s with %s reads differ from the data port "
			  "(%d, %d, at byte %d); CMD12 instead\n",
			  sd_dma_name(), sd_cmd23_name(way), (INT)er1, (INT)er2, (INT)i);
		sd.cmd23_card = FALSE;
		sd.cmd23_force = -1;
		way = SD_CMD23_NONE;
		sd_check_read(b, mode, way, &er2);
		for ( i = 0; i < SD_CHECK_SECT * BLK_SECTOR_SIZE && a[i] == b[i]; i++ ) ;
	}
	if ( er1 < E_OK || er2 < E_OK || i < SD_CHECK_SECT * BLK_SECTOR_SIZE ) {
		tm_printf((UB*)"TessronOS: sd %s reads differ from the data port "
			  "(%d, %d, at byte %d); using the data port\n",
			  sd_dma_name(), (INT)er1, (INT)er2, (INT)i);
		sd.dma_mode = SD_DMA_NONE;
	}
	knl_free_pages(pf, 4);
}

/* ---------------------------------------------------------------- driver */

LOCAL ER sd_readfn( void *exinf, UD start, UD nsect, void *buf )
{
	ER	er;

	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	er = sd_xfer(start, buf, nsect, FALSE);
	tk_unl_mtx(sd.mtxid);

	return er;
}

LOCAL ER sd_unit_range( INT unitno, UD *p_start, UD *p_nsect )
{
	if ( unitno == 0 ) {
		*p_start = 0;
		*p_nsect = sd.nsect;
		return E_OK;
	}
	if ( unitno > sd.tbl.n || !sd.tbl.part[unitno - 1].valid ) {
		return E_NOEXS;
	}
	*p_start = sd.tbl.part[unitno - 1].start;
	*p_nsect = sd.tbl.part[unitno - 1].nsect;

	return E_OK;
}

LOCAL ER sd_open( ID devid, UINT omode, void *exinf )
{
	return ( sd.present ) ? E_OK : E_NOMDA;
}

LOCAL ER sd_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER sd_attr( INT unitno, T_DEVREQ *req )
{
	DiskInfo	*info;
	UD		start, nsect;
	ER		er;

	if ( req->start == TDN_IOMAX && req->cmd == TDC_READ && req->size >= (SZ)sizeof(UW) ) {
		*(UW *)req->buf = SD_IO_MAX;
		req->asize = sizeof(UW);
		return E_OK;
	}
	if ( req->start != TDN_DISKINFO || req->cmd != TDC_READ
	  || req->size < (SZ)sizeof(DiskInfo) ) {
		return E_PAR;
	}
	er = sd_unit_range(unitno, &start, &nsect);
	if ( er < E_OK ) return er;

	info = (DiskInfo *)req->buf;
	info->protect   = ( sd.wp != 0 );
	info->removable = !sd.mmc;		/* a card can be taken out, an eMMC not */
	info->blocksize = BLK_SECTOR_SIZE;
	/* the count in a W stops at 2^31 - 1 sectors (1TB); the whole count is in blockcont_d */
	info->blockcont = ( nsect > 0x7FFFFFFFULL ) ? 0x7FFFFFFF : (W)nsect;
	info->blockcont_d = (D)nsect;
	req->asize = sizeof(DiskInfo);

	return E_OK;
}

LOCAL ER sd_define( void );

LOCAL ER sd_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	INT	unitno = (INT)(req->devid & 0xff);
	UD	start, nsect, nreq;
	ER	er;

	req->asize = 0;
	req->error = E_OK;

	/*
	 * A write here completes when the card leaves its busy state, and
	 * the card's own cache (SD 6.0) is never turned on, so there is
	 * nothing more to put onto the medium.
	 */
	if ( req->start == TDN_FLUSH && req->cmd == TDC_WRITE ) {
		req->error = sd.present ? E_OK : E_NOMDA;
		return E_OK;
	}
	/* the partition table read again after it was written (gpt.c) */
	if ( req->start == TDN_PARTSCAN && req->cmd == TDC_WRITE ) {
		INT	n = ( unitno != 0 ) ? E_PAR : knl_part_rescan(sd_readfn, &sd, sd.nsect, &sd.tbl);

		req->error = ( n < E_OK ) ? n : sd_define();
		return E_OK;
	}
	if ( req->start < 0 ) {
		req->error = sd_attr(unitno, req);
		return E_OK;
	}
	if ( !sd.present ) {
		req->error = E_NOMDA;
		return E_OK;
	}
	er = sd_unit_range(unitno, &start, &nsect);
	if ( er < E_OK ) { req->error = er; return E_OK; }

	if ( (req->size % BLK_SECTOR_SIZE) != 0 ) {
		req->error = E_PAR;
		return E_OK;
	}
	nreq = (UD)req->size / BLK_SECTOR_SIZE;
	if ( (UD)req->start >= nsect || (UD)req->start + nreq > nsect ) {
		req->error = E_PAR;
		return E_OK;
	}

	if ( req->cmd == TDC_WRITE && sd.wp != 0 ) {
		req->error = E_RONLY;
		return E_OK;
	}

	er = tk_loc_mtx(sd.mtxid, tmout);
	if ( er < E_OK ) { req->error = er; return E_OK; }

	er = sd_xfer(start + (UD)req->start, req->buf, nreq, ( req->cmd == TDC_WRITE ));
	tk_unl_mtx(sd.mtxid);

	if ( er == E_OK ) req->asize = req->size;
	req->error = er;

	return E_OK;
}

LOCAL INT sd_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER sd_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT sd_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* The card under its name, the partitions as its subunits; again when they change */
LOCAL ER sd_define( void )
{
	T_DDEV	ddev;
	ID	id;

	ddev.exinf   = &sd;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_DISK | TDK_DISK_FLA;
	ddev.nsub    = sd.tbl.n;
	ddev.blksz   = BLK_SECTOR_SIZE;
	ddev.openfn  = (FP)sd_open;
	ddev.closefn = (FP)sd_close;
	ddev.execfn  = (FP)sd_exec;
	ddev.waitfn  = (FP)sd_wait;
	ddev.abortfn = (FP)sd_abort;
	ddev.eventfn = (FP)sd_event;

	id = tk_def_dev(sd.devnm, &ddev, NULL);
	if ( id > 0 ) {
		sd.devid = id;
	}
	return ( id > 0 ) ? E_OK : (ER)id;
}

#ifdef RPI5
LOCAL UW sd_be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | (UW)p[3];
}

LOCAL BOOL sd_str_is( CONST UB *v, INT len, CONST char *s )
{
	INT	i;

	for ( i = 0; i < len && s[i] != '\0' && v[i] == (UB)s[i]; i++ ) ;
	return ( i < len && s[i] == '\0' && v[i] == '\0' );
}

/*
 * The card slot from the device tree. The controller's node lists the
 * UHS-I bus speeds (sd-uhs-sdr50, -ddr50, -sdr104; SDR104 brings SDR50)
 * unless it says no-1-8-v, and may cap the clock (max-frequency). The
 * signal level's regulator-gpio gives its pin (the second cell of
 * gpios; the third's bit 0 inverts it), the pin's level for 1.8V and
 * for 3.3V (states: microvolts and level in pairs) and its settling
 * time; the fixed regulator of the card's power gives its pin, high for
 * on with enable-active-high and low without. Both pins are put where
 * a system starting up would have them: 3.3V, power on. An eMMC, and
 * a board without the two regulators, run no UHS-I.
 */
LOCAL void sd_slot_probe( void )
{
	CONST UB	*v;
	INT		len, i;
	UW		uv, inv;
	BOOL		got18 = FALSE, got33 = FALSE;

	knl_memset(&sd_slot, 0, sizeof(sd_slot));
	if ( knl_board.sd == BOARD_SD_EMMC || knl_board.sd_1v8 ) {
		return;
	}
	if ( knl_dtb_prop(SDIO1_PATH, "no-1-8-v", &len) != NULL ) {
		tm_printf((UB*)"TessronOS: sd the device tree says no-1-8-v: no UHS-I\n");
		return;
	}
	if ( knl_dtb_prop(SDIO1_PATH, "sd-uhs-sdr50", &len) != NULL )  sd_slot.caps |= HC_SDR50;
	if ( knl_dtb_prop(SDIO1_PATH, "sd-uhs-ddr50", &len) != NULL )  sd_slot.caps |= HC_DDR50;
	if ( knl_dtb_prop(SDIO1_PATH, "sd-uhs-sdr104", &len) != NULL ) sd_slot.caps |= HC_SDR104 | HC_SDR50;
	sd_slot.f_max = knl_dtb_u32(SDIO1_PATH, "max-frequency", 0);
	if ( sd_slot.caps == 0 ) {
		return;
	}

	v = knl_dtb_prop(SD_IO_REG_PATH, "compatible", &len);
	if ( v == NULL || !sd_str_is(v, len, "regulator-gpio") ) return;
	v = knl_dtb_prop(SD_IO_REG_PATH, "gpios", &len);
	if ( v == NULL || len < 12 ) return;
	sd_slot.io_pin = sd_be32(v + 4);
	inv = sd_be32(v + 8) & 1;
	v = knl_dtb_prop(SD_IO_REG_PATH, "states", &len);
	if ( v == NULL ) return;
	for ( i = 0; i + 8 <= len; i += 8 ) {
		uv = sd_be32(v + i);
		if ( uv == 1800000 ) { sd_slot.io_1v8 = ( sd_be32(v + i + 4) & 1 ) ^ inv; got18 = TRUE; }
		if ( uv == 3300000 ) { sd_slot.io_3v3 = ( sd_be32(v + i + 4) & 1 ) ^ inv; got33 = TRUE; }
	}
	if ( !got18 || !got33 || sd_slot.io_1v8 == sd_slot.io_3v3 ) return;
	sd_slot.settle_us = knl_dtb_u32(SD_IO_REG_PATH, "regulator-settling-time-us", 0);

	v = knl_dtb_prop(SD_VCC_REG_PATH, "gpios", &len);
	if ( v == NULL || len < 12 ) return;
	sd_slot.vcc_pin = sd_be32(v + 4);
	sd_slot.vcc_on = ( knl_dtb_prop(SD_VCC_REG_PATH, "enable-active-high", &len) != NULL ) ? 1 : 0;

	if ( gpio_bank_pins(0) <= (INT)sd_slot.io_pin || gpio_bank_pins(0) <= (INT)sd_slot.vcc_pin ) return;
	sd_slot.reg = TRUE;
	sd_vmmc(TRUE);
	sd_vqmmc(FALSE);
}
#endif

/*
 * Ending multiple block transfers by SET_BLOCK_COUNT, for a test: one of
 * SD_CMD23_NONE, _MANUAL, _AUTO forced, SD_CMD23_DECIDE back to what the
 * card and the controller say, negative only asks. Answers the way in
 * use before.
 */
EXPORT INT knl_sd_cmd23( INT way )
{
	INT	old;

	if ( !sd.present ) return E_NOEXS;
	old = sd_cmd23_way();
	if ( way < 0 ) return old;
	if ( way > SD_CMD23_DECIDE ) return E_PAR;
	if ( way == SD_CMD23_AUTO && ( sd_in16(SDHCI_HOST_VERSION) & 0xff ) < SDHCI_SPEC_300 ) return E_NOSPT;
	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	sd.cmd23_force = ( way == SD_CMD23_DECIDE ) ? -1 : way;
	tk_unl_mtx(sd.mtxid);
	return old;
}

/*
 * Bring the card up again from the start, as at start-up, with the
 * failures of SD_TEST_* made to happen: the ways back from a UHS-I
 * switch, on a machine whose card cannot switch. 0 is start-up's own
 * way. Answers the bus speed it ended at.
 */
EXPORT INT knl_sd_reinit( UW test )
{
	CONST char	*uhs_why, *hs_why;
	ER		er;

	if ( sd.mtxid <= 0 || sd.mmc ) return E_NOEXS;
	tk_loc_mtx(sd.mtxid, TMO_FEVR);
	(void)sd_wait_idle();
	sd_test = test;
	er = sd_bringup(TRUE, &uhs_why, &hs_why);
	if ( er >= E_OK ) {
		sd_speed_note(uhs_why, hs_why);
	}
	sd_test = 0;
	tk_unl_mtx(sd.mtxid);
	return ( er < E_OK ) ? er : sd.speed;
}

/*
 * At shutdown and before a restart: a card left at 1.8V is deselected
 * and its power turned off, and the lines put back at 3.3V, so that the
 * firmware starting next finds it as after a power on. A card at 3.3V
 * is left as it is, powered, as the firmware gave it.
 */
EXPORT void knl_sd_finish( void )
{
#ifdef RPI5
	ER	er;

	if ( !sd.present || sd.mmc || !sd_slot.reg || !sd.v18 || sd.mtxid <= 0 ) {
		return;
	}
	er = tk_loc_mtx(sd.mtxid, 2000);
	(void)sd_wait_idle();
	(void)sd_cmd(CMD_DESELECT, 0, NULL);
	sd_power_off();
	sd_vqmmc(FALSE);
	sd.v18 = FALSE;
	sd.present = FALSE;
	if ( er >= E_OK ) tk_unl_mtx(sd.mtxid);
	tm_printf((UB*)"TessronOS: sd the card's power is off\n");
#endif
}

EXPORT INT knl_sd_init( void )
{
	T_CMTX	cmtx;
	INT	i;
	ER	er;
	CONST char	*uhs_why = NULL, *hs_why = NULL;

	knl_memset(&sd, 0, sizeof(sd));
	sd.cmd23_force = -1;

#ifdef QEMU_VIRT
	{
		T_PCIDEV	d;

		if ( ts_pcie_find(PCI_CLASS_SDHCI, PCI_SUBCLASS_SDHCI, 0xff, 0, &d) < E_OK
		  || d.bar[0] == 0 ) {
			return 0;		/* the machine has no SD slot */
		}
		ts_pcie_enable(&d);
		sd_base = ts_pcie_bar_base(&d, 0);
		if ( sd_base == 0 ) {
			return 0;
		}
	}
#endif
#ifdef RPI4
	{
		UD	bus, cpu, size;
		UW	hz = 0;

		/*
		 * What EMMC2 sees of memory. Without a device tree that says, the
		 * first stepping's view is taken: it is the narrower one, and right
		 * on the later stepping too for memory below 1GB.
		 */
		if ( knl_dtb_ranges("/emmc2bus", "dma-ranges", 0, &bus, &cpu, &size) >= E_OK && size != 0 ) {
			sd_dma_bus = bus;
			sd_dma_cpu = cpu;
			sd_dma_size = size;
		} else {
			sd_dma_bus = 0xC0000000;
			sd_dma_cpu = 0;
			sd_dma_size = 0x40000000;
		}
		if ( knl_fw_value(TAG_GET_CLOCK_RATE, FW_CLOCK_EMMC2, &hz) >= E_OK
		  && hz >= 1000000 && hz <= 400000000 ) {
			sd_base_hz = hz;
		}
		{
			UB	v[8];

			if ( knl_bootarg(knl_bootargs, "tessronos.sd", v, sizeof(v))
			  && v[0] == 'e' && v[1] == 'm' && v[2] == 'm' && v[3] == 'c' && v[4] == '\0' ) {
				sd_base = DEV_BASE(EMMC1_PA);
				sd_base_hz = 0;
				tm_printf((UB*)"TessronOS: sd the EMMC controller at %08x instead (tessronos.sd=emmc)\n",
					  (UW)EMMC1_PA);
			}
		}
		tm_printf((UB*)"TessronOS: sd EMMC2 sees memory %08x%08x+%08x%08x at bus %08x%08x, clock %d Hz\n",
			  (UW)( sd_dma_cpu >> 32 ), (UW)sd_dma_cpu, (UW)( sd_dma_size >> 32 ), (UW)sd_dma_size,
			  (UW)( sd_dma_bus >> 32 ), (UW)sd_dma_bus, sd_base_hz);
	}
#endif

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	sd.mtxid = tk_cre_mtx(&cmtx);
	if ( sd.mtxid <= 0 ) {
		return (ER)sd.mtxid;
	}

#ifdef RPI5
	sd_slot_probe();
	if ( sd_uhs_caps() != 0 ) {
		/*
		 * The clock through the delay line, which the tuning moves:
		 * the 50MHz limit mode off, and this register rather than the
		 * strap deciding, before the controller is reset.
		 */
		UW	m50 = in_w(SDIO_CFG_MAX_50MHZ);

		out_w(SDIO_CFG_MAX_50MHZ, ( m50 & ~(UW)MAX_50MHZ_ENABLE ) | MAX_50MHZ_OVERRIDE);
		tm_printf((UB*)"TessronOS: sd UHS-I on the slot: bus speeds %x, level pin %d (1.8V %d), power pin %d, "
			  "caps %08x %08x, max current %08x, version %04x, 50MHz mode %08x -> %08x\n",
			  sd_slot.caps, sd_slot.io_pin, sd_slot.io_1v8, sd_slot.vcc_pin,
			  in_w(SDHCI_CAPABILITIES), in_w(SDHCI_CAPABILITIES1), in_w(SDHCI_MAX_CURRENT),
			  sd_in16(SDHCI_HOST_VERSION), m50, in_w(SDIO_CFG_MAX_50MHZ));
	}
#endif
	er = sd_bringup(TRUE, &uhs_why, &hs_why);
	if ( er < E_OK ) {
		tm_printf((UB*)"TessronOS: no SD card (%d at step %d, state %08x, caps %08x)\n",
			  er, sd_step, in_w(SDHCI_PRESENT_STATE), in_w(SDHCI_CAPABILITIES));
		return 0;
	}
	sd_speed_note(uhs_why, hs_why);
	if ( sd.pv_used ) {
		tm_printf((UB*)"TessronOS: sd the clock from the preset value %04x\n", sd.pv);
	}
	sd_dma_select();
	sd_bounce_alloc();
	sd_dma_check();

	knl_read_parttbl(sd_readfn, &sd, sd.nsect, &sd.tbl);

	sd.devnm[0] = 's'; sd.devnm[1] = 'd'; sd.devnm[2] = 'a'; sd.devnm[3] = '\0';

	er = sd_define();
	if ( er < E_OK ) {
		return er;
	}

	tm_printf((UB*)"TessronOS: sda %s %d sectors (%d MB)%s, %d data lines, %d partitions (%s), %s, %s, SCR %02x%02x%02x%02x\n",
		sd.mmc ? "eMMC" : "SD card",
		(UW)sd.nsect, (UW)(sd.nsect >> 11), sd.hc ? ", HC" : "", (INT)sd.width,
		sd.tbl.n, sd.tbl.gpt ? "GPT" : "MBR", sd_dma_name(), sd_cmd23_name(sd_cmd23_way()),
		sd.scr[0], sd.scr[1], sd.scr[2], sd.scr[3]);
	for ( i = 0; i < sd.tbl.n; i++ ) {
		tm_printf((UB*)"TessronOS:   sda%d %d+%d\n", i,
			(UW)sd.tbl.part[i].start, (UW)sd.tbl.part[i].nsect);
	}

	return 1;
}

#endif /* RPI5 || RPI4 || QEMU_VIRT */
