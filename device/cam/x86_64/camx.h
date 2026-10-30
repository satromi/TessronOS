/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	camx.h (x64 PC)
 *	The notebook's own cameras behind Intel's IPU3 (the Surface Go 2):
 *	the CSI-2 receiver CIO2 and what is put together for each sensor
 *	from the ACPI namespace (cio2.c), the sensor's power controller of
 *	the ACPI device INT3472 (int3472.c) and the sensor OV5693 on I2C
 *	(ov5693.c).
 */

#ifndef _DEVICE_CAM_X86_64_CAMX_
#define _DEVICE_CAM_X86_64_CAMX_

#include "../../i2c/x86_64/i2c.h"

/* ---------------------------------------------------------------- the power controller (int3472.c) */

/* The kinds of a GPIO line the controller's _DSM names (its low byte) */
#define CAMGP_RESET		0x00
#define CAMGP_POWERDOWN		0x01
#define CAMGP_STROBE		0x02
#define CAMGP_POWER1		0x08
#define CAMGP_POWER_ENABLE	0x0b
#define CAMGP_CLK_ENABLE	0x0c
#define CAMGP_PRIVACY_LED	0x0d
#define CAMGP_DOVDD		0x10
#define CAMGP_HANDSHAKE		0x12

/* The kinds of controller its CLDB names (byte 1) */
#define CAMPWR_DISCRETE		1	/* GPIO lines of the chipset */
#define CAMPWR_TPS68470		2	/* a TPS68470 PMIC on I2C (2 and 3) */

#define CAMPWR_GPIO_MAX		8

typedef struct {
	UB	type;			/* CAMGP_* */
	UB	on;			/* the level that has the sensor on (out of reset, powered, clocked, lit) */
	UH	pin;			/* the controller's pin, as _CRS gives it */
	UW	dsm;			/* the _DSM's value for it, as it came */
	char	ctrl[40];		/* the GPIO controller's path */
} T_CAMGPIO;

typedef struct {
	void		*dev;
	char		path[48];
	UB		cldb[32];
	INT		kind;		/* CAMPWR_*, 0 none known */
	INT		ngpio;
	T_CAMGPIO	gpio[CAMPWR_GPIO_MAX];
	BOOL		swap_rst;	/* reset and powerdown driven the other way round (see knl_campwr_swap) */
	T_I2CBUS	*pmic;		/* a TPS68470's bus and address */
	UH		pmic_addr;
	BOOL		on;
} T_CAMPWR;

/* The power controller at path: its CLDB, its lines and their kinds said; E_OK when it is one this can drive */
IMPORT ER	knl_campwr_find( T_CAMPWR *p, CONST char *path );
/* The sensor powered as its data sheet asks: lines held in reset, clock, power, out of reset, the wait */
IMPORT ER	knl_campwr_on( T_CAMPWR *p );
IMPORT void	knl_campwr_off( T_CAMPWR *p );
/* The levels of reset and powerdown taken the other way from now on (said; for a sensor that did not answer) */
IMPORT void	knl_campwr_swap( T_CAMPWR *p );
/* Its lines said on the console, with how the pads are now */
IMPORT void	knl_campwr_say( T_CAMPWR *p, CONST char *when );

/* ---------------------------------------------------------------- the sensor (ov5693.c) */

/* The one mode used: 2 x 2 binned from the whole array */
#define OV5693_WIDTH		1296
#define OV5693_HEIGHT		972
#define OV5693_LINK_HZ		419200000U	/* the CSI-2 clock lane: 838.4 Mbit/s a data lane */
#define OV5693_LANES		2
#define OV5693_MCLK		19200000U
#define OV5693_INTERVAL		333333U		/* 100 ns: 30 frames a second */
#define OV5693_BLACK		64		/* the black level the sensor's own correction keeps (10 bits) */
#define OV5693_CHIP_ID		0x5690

typedef struct {
	T_I2CBUS	*bus;
	UH		addr;
	UW		chip;		/* the ID it gave */
	UB		rev;
	UW		vts;		/* lines a frame */
	UW		exposure;	/* lines */
	UW		gain;		/* the analogue gain's register value, 8 for 1 */
	BOOL		bars;		/* the colour bars of the sensor in place of the picture */
} T_OV5693;

/* Its ID read (the sensor powered): E_OK when it is an OV5693 */
IMPORT ER	knl_ov5693_probe( T_OV5693 *s );
/* Reset and set up for the mode, standing by */
IMPORT ER	knl_ov5693_setup( T_OV5693 *s );
IMPORT ER	knl_ov5693_stream( T_OV5693 *s, BOOL on );
/* The exposure (lines) and the analogue gain, within what the mode allows */
IMPORT ER	knl_ov5693_exposure( T_OV5693 *s, UW exposure, UW gain );
/* The largest exposure the mode allows */
IMPORT UW	knl_ov5693_max_exposure( T_OV5693 *s );
/* Registers that tell how it is set up, said on the console */
IMPORT void	knl_ov5693_say( T_OV5693 *s, CONST char *when );

/* ---------------------------------------------------------------- the receiver (cio2.c) */

/* The receiver and the sensors the ACPI namespace names found, each working one made a camera */
IMPORT INT	knl_cio2_init( void );

#endif /* _DEVICE_CAM_X86_64_CAMX_ */
