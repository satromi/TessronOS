/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ov5693.c (x64 PC)
 *	The OmniVision OV5693, the Surface Go 2's front camera (ACPI
 *	INT33BE): a 5-megapixel Bayer sensor (2592 x 1944 active pixels)
 *	spoken to on I2C, two CSI-2 lanes out (camx.h).
 *
 *	Its registers have 16-bit addresses and 8-bit values; a value of
 *	more bytes is at consecutive addresses, the highest byte first. Its
 *	ID is at 0x300A (0x5690). The one mode is the whole array binned 2 x
 *	2 to 1296 x 972, 10-bit raw, with the input clock of 19.2 MHz:
 *
 *	- the system PLL and the MIPI PLL: 19.2 MHz x 131 / 3 gives 838.4
 *	  Mbit/s a lane, a clock lane of 419.2 MHz, a pixel rate of 167.68 MHz;
 *	- the window: from column 16, row 6, 2592 x 1944 (the active pixels),
 *	  every other pixel read across and down (increments 3 and 1 for the
 *	  odd and even ones) and pairs binned, 1296 x 972 out, no mirror
 *	  and no flip (the pixels then come blue first:
 *	  BGGR);
 *	- a line is 2688 pixel clocks, a frame 2079 lines: 30 frames a second;
 *	- the exposure (0x3500, 3 bytes: lines in bits 19:4) and the analogue
 *	  gain (0x350A, 2 bytes: bits 10:4, 8 for 1x) set by hand (0x3503),
 *	  the white balance's gains of the sensor (0x3400, 0x3402, 0x3404,
 *	  1024 for 1x) left at 1x, the black level kept at 64 by the
 *	  sensor's own correction;
 *	- streaming started and stopped at 0x0100; 0x0103 resets it.
 *
 *	The rest of the set-up (the analogue part, the black level
 *	correction, the MIPI timing) is the sensor's reference settings for
 *	this clock. The colour bars of the sensor (0x5E00) can be put in
 *	place of the picture, to tell a fault of the sensor's picture from
 *	one of the path behind it.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "camx.h"

#define R_STREAM		0x0100
#define R_RESET			0x0103
#define R_CHIP_ID		0x300a
#define R_SUB_ID		0x302a
#define R_MWB_RED		0x3400
#define R_MWB_GREEN		0x3402
#define R_MWB_BLUE		0x3404
#define R_EXPOSURE		0x3500
#define R_AEC_MANUAL		0x3503
#define R_GAIN			0x350a
#define R_X_START		0x3800
#define R_Y_START		0x3802
#define R_X_END			0x3804
#define R_Y_END			0x3806
#define R_X_OUT			0x3808
#define R_Y_OUT			0x380a
#define R_HTS			0x380c
#define R_VTS			0x380e
#define R_X_OFFSET		0x3810
#define R_Y_OFFSET		0x3812
#define R_X_INC			0x3814
#define R_Y_INC			0x3815
#define R_FORMAT1		0x3820
#define R_FORMAT2		0x3821
#define R_TEST_PATTERN		0x5e00

#define FORMAT1_VBIN		0x01
#define FORMAT1_VFLIP		0x42
#define FORMAT2_HBIN		0x01
#define FORMAT2_HFLIP		0x06
#define TEST_BARS		0x80

#define HTS			2688
#define VTS_30FPS		2079
#define EXPOSURE_MARGIN		8
#define GAIN_MIN		1
#define GAIN_MAX		127
#define GAIN_1X			8

#define ACTIVE_LEFT		16
#define ACTIVE_TOP		6
#define ACTIVE_W		2592
#define ACTIVE_H		1944

typedef struct {
	UH	reg;
	UB	val;
} REGVAL;

/* The reference settings for a 19.2 MHz input clock and two lanes at 838.4 Mbit/s */
LOCAL CONST REGVAL	setup_regs[] = {
	/* the outputs: the MIPI lanes and the clock on, the parallel port off */
	{ 0x3016, 0xf0 }, { 0x3017, 0xf0 }, { 0x3018, 0xf0 }, { 0x3022, 0x01 }, { 0x3028, 0x44 },
	/* the system PLL and the MIPI PLL */
	{ 0x3098, 0x02 }, { 0x3099, 0x19 }, { 0x309a, 0x02 }, { 0x309b, 0x01 }, { 0x309c, 0x00 },
	{ 0x30a0, 0xd2 }, { 0x30a2, 0x01 }, { 0x30b2, 0x00 }, { 0x30b3, 0x83 }, { 0x30b4, 0x03 },
	{ 0x30b5, 0x04 }, { 0x30b6, 0x01 }, { 0x3080, 0x01 }, { 0x3104, 0x21 }, { 0x3106, 0x00 },
	/* white balance, exposure and gain by hand */
	{ 0x3406, 0x01 }, { 0x3503, 0x07 }, { 0x350b, 0x40 },
	/* the analogue part */
	{ 0x3601, 0x0a }, { 0x3602, 0x38 }, { 0x3612, 0x80 }, { 0x3620, 0x54 }, { 0x3621, 0xc7 },
	{ 0x3622, 0x0f }, { 0x3625, 0x10 }, { 0x3630, 0x55 }, { 0x3631, 0xf4 }, { 0x3632, 0x00 },
	{ 0x3633, 0x34 }, { 0x3634, 0x02 }, { 0x364d, 0x0d }, { 0x364f, 0xdd }, { 0x3660, 0x04 },
	{ 0x3662, 0x10 }, { 0x3663, 0xf1 }, { 0x3665, 0x00 }, { 0x3666, 0x20 }, { 0x3667, 0x00 },
	{ 0x366a, 0x80 }, { 0x3680, 0xe0 }, { 0x3681, 0x00 },
	/* the sensor's timing of the pixel array */
	{ 0x3700, 0x42 }, { 0x3701, 0x14 }, { 0x3702, 0xa0 }, { 0x3703, 0xd8 }, { 0x3704, 0x78 },
	{ 0x3705, 0x02 }, { 0x370a, 0x00 }, { 0x370b, 0x20 }, { 0x370c, 0x0c }, { 0x370d, 0x11 },
	{ 0x370e, 0x00 }, { 0x370f, 0x40 }, { 0x3710, 0x00 }, { 0x371a, 0x1c }, { 0x371b, 0x05 },
	{ 0x371c, 0x01 }, { 0x371e, 0xa1 }, { 0x371f, 0x0c },
	{ 0x3721, 0x00 }, { 0x3724, 0x10 }, { 0x3726, 0x00 }, { 0x372a, 0x01 }, { 0x3730, 0x10 },
	{ 0x3738, 0x22 }, { 0x3739, 0xe5 }, { 0x373a, 0x50 }, { 0x373b, 0x02 }, { 0x373c, 0x41 },
	{ 0x373f, 0x02 }, { 0x3740, 0x42 }, { 0x3741, 0x02 }, { 0x3742, 0x18 }, { 0x3743, 0x01 },
	{ 0x3744, 0x02 }, { 0x3747, 0x10 }, { 0x374c, 0x04 }, { 0x3751, 0xf0 }, { 0x3752, 0x00 },
	{ 0x3753, 0x00 }, { 0x3754, 0xc0 }, { 0x3755, 0x00 }, { 0x3756, 0x1a }, { 0x3758, 0x00 },
	{ 0x3759, 0x0f }, { 0x376b, 0x44 }, { 0x375c, 0x04 }, { 0x3774, 0x10 }, { 0x3776, 0x00 },
	{ 0x377f, 0x08 }, { 0x3780, 0x22 }, { 0x3781, 0x0c }, { 0x3784, 0x2c }, { 0x3785, 0x1e },
	{ 0x378f, 0xf5 }, { 0x3791, 0xb0 }, { 0x3795, 0x00 }, { 0x3796, 0x64 }, { 0x3797, 0x11 },
	{ 0x3798, 0x30 }, { 0x3799, 0x41 }, { 0x379a, 0x07 }, { 0x379b, 0xb0 }, { 0x379c, 0x0c },
	{ 0x3a04, 0x06 }, { 0x3a05, 0x14 }, { 0x3e07, 0x20 },
	/* the black level correction: a target of 64 at 10 bits */
	{ 0x4000, 0x08 }, { 0x4001, 0x04 }, { 0x4004, 0x08 }, { 0x4006, 0x20 }, { 0x4008, 0x24 },
	{ 0x4009, 0x10 }, { 0x4058, 0x00 }, { 0x4101, 0xb2 },
	/* the output's format and the MIPI timing (0x4837: the period of the pixel clock for the lanes' rate) */
	{ 0x4307, 0x31 }, { 0x4511, 0x05 }, { 0x4512, 0x01 }, { 0x481f, 0x30 }, { 0x4826, 0x2c },
	{ 0x4d02, 0xfd }, { 0x4d03, 0xf5 }, { 0x4d04, 0x0c }, { 0x4d05, 0xcc }, { 0x4837, 0x0a },
	/* the ISP of the sensor: its corrections of defects on, its own colour processing off */
	{ 0x5003, 0x20 }, { 0x5013, 0x00 }, { 0x5842, 0x01 }, { 0x5843, 0x2b }, { 0x5844, 0x01 },
	{ 0x5845, 0x92 }, { 0x5846, 0x01 }, { 0x5847, 0x8f }, { 0x5848, 0x01 }, { 0x5849, 0x0c },
	{ 0x5e10, 0x0c }, { 0x5041, 0x14 },
	/* the readout's format: mirror and flip are taken off after, with the binning */
	{ 0x3820, 0x00 }, { 0x3821, 0x1e },
};

LOCAL ER wr( T_OV5693 *s, UH reg, CONST UB *v, INT n )
{
	UB	b[2 + 4];
	INT	i;

	b[0] = (UB)( reg >> 8 );
	b[1] = (UB)reg;
	for ( i = 0; i < n && i < 4; i++ ) b[2 + i] = v[i];
	return s->bus->xfer(s->bus->ctx, s->addr, b, 2 + n, NULL, 0);
}

LOCAL ER wr8( T_OV5693 *s, UH reg, UW v )
{
	UB	b = (UB)v;

	return wr(s, reg, &b, 1);
}

LOCAL ER wr16( T_OV5693 *s, UH reg, UW v )
{
	UB	b[2];

	b[0] = (UB)( v >> 8 );
	b[1] = (UB)v;
	return wr(s, reg, b, 2);
}

LOCAL ER wr24( T_OV5693 *s, UH reg, UW v )
{
	UB	b[3];

	b[0] = (UB)( v >> 16 );
	b[1] = (UB)( v >> 8 );
	b[2] = (UB)v;
	return wr(s, reg, b, 3);
}

/* n bytes from reg, as one value (the highest byte first); -1 when it did not answer */
LOCAL INT rd( T_OV5693 *s, UH reg, INT n )
{
	UB	a[2], b[4];
	INT	i, v = 0;

	a[0] = (UB)( reg >> 8 );
	a[1] = (UB)reg;
	if ( n > 4 ) n = 4;
	if ( s->bus->xfer(s->bus->ctx, s->addr, a, 2, b, n) < E_OK ) return -1;
	for ( i = 0; i < n; i++ ) v = ( v << 8 ) | b[i];
	return v;
}

EXPORT ER knl_ov5693_probe( T_OV5693 *s )
{
	INT	id = rd(s, R_CHIP_ID, 2), sub = rd(s, R_SUB_ID, 1);

	tm_printf((UB *)"ov5693: on %s at %02x: chip ID %04x (an OV5693 says %04x), sub ID %02x\n",
		  s->bus->name, s->addr, id < 0 ? 0xffff : id, OV5693_CHIP_ID, sub < 0 ? 0xff : sub);
	if ( id < 0 ) return E_IO;
	s->chip = (UW)id;
	s->rev = (UB)( sub < 0 ? 0 : sub );
	return ( id == OV5693_CHIP_ID ) ? E_OK : E_NOEXS;
}

/* bits of an 8-bit register set to v */
LOCAL ER update( T_OV5693 *s, UH reg, UB mask, UB v )
{
	INT	o = rd(s, reg, 1);

	if ( o < 0 ) return E_IO;
	return wr8(s, reg, ( (UW)o & ~(UW)mask ) | ( v & mask ));
}

EXPORT UW knl_ov5693_max_exposure( T_OV5693 *s )
{
	return s->vts - EXPOSURE_MARGIN;
}

EXPORT ER knl_ov5693_exposure( T_OV5693 *s, UW exposure, UW gain )
{
	ER	er;

	if ( exposure < 1 ) exposure = 1;
	if ( exposure > knl_ov5693_max_exposure(s) ) exposure = knl_ov5693_max_exposure(s);
	if ( gain < GAIN_MIN ) gain = GAIN_MIN;
	if ( gain > GAIN_MAX ) gain = GAIN_MAX;
	er = wr24(s, R_EXPOSURE, ( exposure << 4 ) & 0xffff0);
	if ( er >= E_OK ) er = wr16(s, R_GAIN, ( gain << 4 ) & 0x7f0);
	if ( er >= E_OK ) {
		s->exposure = exposure;
		s->gain = gain;
	}
	return er;
}

EXPORT ER knl_ov5693_setup( T_OV5693 *s )
{
	INT	i, bad = 0;
	ER	er;

	er = wr8(s, R_STREAM, 0);
	if ( er >= E_OK ) er = wr8(s, R_RESET, 1);
	if ( er < E_OK ) return er;
	tk_dly_tsk(2);
	(void)wr8(s, R_STREAM, 0);
	for ( i = 0; i < (INT)( sizeof(setup_regs) / sizeof(setup_regs[0]) ); i++ ) {
		if ( wr8(s, setup_regs[i].reg, setup_regs[i].val) < E_OK ) bad++;
	}

	/* the window, the binning and the frame's timing */
	s->vts = VTS_30FPS;
	(void)wr16(s, R_X_START, ACTIVE_LEFT);
	(void)wr16(s, R_Y_START, ACTIVE_TOP);
	(void)wr16(s, R_X_END, ACTIVE_LEFT + ACTIVE_W);
	(void)wr16(s, R_Y_END, ACTIVE_TOP + ACTIVE_H);
	(void)wr16(s, R_X_OUT, OV5693_WIDTH);
	(void)wr16(s, R_Y_OUT, OV5693_HEIGHT);
	(void)wr16(s, R_HTS, HTS);
	(void)wr16(s, R_VTS, s->vts);
	(void)wr16(s, R_X_OFFSET, 0);
	(void)wr16(s, R_Y_OFFSET, 0);
	(void)wr8(s, R_X_INC, 0x31);
	(void)wr8(s, R_Y_INC, 0x31);
	(void)update(s, R_FORMAT1, FORMAT1_VBIN | FORMAT1_VFLIP, FORMAT1_VBIN);
	(void)update(s, R_FORMAT2, FORMAT2_HBIN | FORMAT2_HFLIP, FORMAT2_HBIN);

	/* the sensor's white balance at 1x, a start of a quarter of the frame's time at 1x gain */
	(void)wr16(s, R_MWB_RED, 0x400);
	(void)wr16(s, R_MWB_GREEN, 0x400);
	(void)wr16(s, R_MWB_BLUE, 0x400);
	er = knl_ov5693_exposure(s, s->vts / 4, GAIN_1X);
	(void)wr8(s, R_TEST_PATTERN, s->bars ? TEST_BARS : 0);
	if ( bad > 0 ) tm_printf((UB *)"ov5693: %d of the set-up's writes not answered\n", bad);
	return ( bad > 0 ) ? E_IO : er;
}

EXPORT ER knl_ov5693_stream( T_OV5693 *s, BOOL on )
{
	return wr8(s, R_STREAM, on ? 1 : 0);
}

EXPORT void knl_ov5693_say( T_OV5693 *s, CONST char *when )
{
	tm_printf((UB *)"ov5693: %s: stream %x, PLL %02x %02x / MIPI %02x %02x, window %d,%d-%d,%d out %dx%d, "
		  "HTS %d VTS %d, inc %02x %02x, format %02x %02x, exposure %06x gain %04x, AEC %02x, test %02x\n",
		  when, rd(s, R_STREAM, 1), rd(s, 0x3098, 1), rd(s, 0x3099, 1), rd(s, 0x30b3, 1), rd(s, 0x30b4, 1),
		  rd(s, R_X_START, 2), rd(s, R_Y_START, 2), rd(s, R_X_END, 2), rd(s, R_Y_END, 2),
		  rd(s, R_X_OUT, 2), rd(s, R_Y_OUT, 2), rd(s, R_HTS, 2), rd(s, R_VTS, 2), rd(s, R_X_INC, 1),
		  rd(s, R_Y_INC, 1), rd(s, R_FORMAT1, 1), rd(s, R_FORMAT2, 1), rd(s, R_EXPOSURE, 3), rd(s, R_GAIN, 2),
		  rd(s, R_AEC_MANUAL, 1), rd(s, R_TEST_PATTERN, 1));
}

#endif /* X64 */
