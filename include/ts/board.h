/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	board.h
 *	Which board the system runs on, and what of it the drivers have to
 *	know (docs/tessronos/03-platform.md 3.5)
 *
 *	The Raspberry Pi 4 family (the Pi 4 Model B, the Pi 400 and the
 *	Compute Module 4) has an image of its own and is told apart the same
 *	way (docs/tessronos/03-platform.md 3.6).
 *
 *	One kernel image serves the Raspberry Pi 5 and the Compute Module 5,
 *	with its eMMC or without (Lite, which uses the carrier's SD slot).
 *	They share the SoC and the RP1 and differ in a few wires: what the
 *	SD host controller has on it and whether the card detect line is
 *	there, which always-on pins the board keeps for itself, at which
 *	address the Ethernet PHY answers, and where a serial lead can be
 *	put. The kind is told first by the root node of the device tree
 *	(its compatible and model) and then, on a Raspberry Pi, by the board
 *	revision the firmware gives, which says what the hardware is
 *	whichever device tree was loaded.
 */

#ifndef __TS_BOARD_H__
#define __TS_BOARD_H__

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_UNKNOWN		0
#define BOARD_QEMU_VIRT		1
#define BOARD_RPI5		2	/* Raspberry Pi 5 */
#define BOARD_CM5		3	/* Compute Module 5, eMMC on the module */
#define BOARD_CM5LITE		4	/* Compute Module 5 Lite, the carrier's SD slot */
#define BOARD_RPI4B		5	/* Raspberry Pi 4 Model B */
#define BOARD_RPI400		6	/* Raspberry Pi 400 */
#define BOARD_CM4		7	/* Compute Module 4: eMMC on the module, or (Lite) the carrier's SD slot */
#define BOARD_PC		8	/* an x64 PC with UEFI firmware, of no kind the kernel knows */
#define BOARD_QEMU_Q35		9	/* QEMU q35 with OVMF */
#define BOARD_SURFACE_GO2	10	/* Microsoft Surface Go 2 (model 1927) */
#define BOARD_X1YOGA3		11	/* Lenovo ThinkPad X1 Yoga, 3rd generation (machine types 20LD to 20LG) */

/* What the SD host controller of the board has on it */
#define BOARD_SD_CD		0	/* a card slot with its detect switch on always-on pin 5 */
#define BOARD_SD_NOCD		1	/* a card slot without a detect line: a card is looked for */
#define BOARD_SD_EMMC		2	/* eMMC soldered on: always there, never taken out */
#define BOARD_SD_PCI		3	/* QEMU's sdhci-pci, which reports its card itself */

typedef struct {
	UW	kind;		/* BOARD_* */
	UW	rev;		/* the firmware's board revision, 0 when not known */
	CONST char *name;	/* what people call it */
	UB	sd;		/* BOARD_SD_* */
	UB	sd_width;	/* data lines wired to an eMMC: 4 or 8 */
	BOOL	sd_1v8;		/* the SD lines are fixed at 1.8V */
	UB	eth_phy;	/* MDIO address of the Ethernet PHY */
	UW	aon_own;	/* always-on bank 0 pins the board uses itself, a bit per pin */
	BOOL	led_power;	/* the LED the kernel blinks is the board's power LED, left on */
	BOOL	console_rp1;	/* the console is (to be) on UART0 of the RP1 */
} T_BOARD;

IMPORT T_BOARD	knl_board;

/*
 * The kind a device tree's root node names: its compatible (a list of
 * strings, 'len' bytes) and its model. BOARD_UNKNOWN for anything else.
 */
IMPORT UW   knl_board_by_dt( CONST UB *compat, INT len, CONST UB *model );

/*
 * The kind a Raspberry Pi board revision names (the new style, with the
 * type in bits 4 to 11). BOARD_UNKNOWN for other types and the old style.
 */
IMPORT UW   knl_board_by_rev( UW rev );

/*
 * Fill knl_board for a kind: the table's values, and the device tree's
 * model as the name of a board the table does not know.
 */
IMPORT void knl_board_set( UW kind, UW rev );

/*
 * From the device tree read at boot, and the command line in it. An
 * image started with a device tree of another SoC says so and stops.
 */
IMPORT void knl_board_init( void );

/* Whether a compatible list (len bytes) holds the string 'want' */
IMPORT BOOL knl_board_has_compat( CONST UB *compat, INT len, CONST char *want );

/* The firmware's board revision, which decides when it names a kind */
IMPORT void knl_board_set_rev( UW rev );

/*
 * The value of "key=value" among the words of a command line, copied
 * into 'val' (at most max - 1 bytes). FALSE when the key is not there.
 */
IMPORT BOOL knl_bootarg( CONST UB *args, CONST char *key, UB *val, INT max );

#ifdef __cplusplus
}
#endif
#endif /* __TS_BOARD_H__ */
