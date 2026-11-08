/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ov8865.c (x64 PC)
 *	The OmniVision OV8865, the Surface Go 2's camera at the back (ACPI
 *	INT347A): an 8-megapixel Bayer sensor spoken to on I2C, behind a
 *	TPS68470 for its power and clock (int3472.c; camx.h).
 *
 *	Its registers have 16-bit addresses and 8-bit values; a value of more
 *	bytes is at consecutive addresses, the highest byte first. Its ID is
 *	three bytes at 0x300A (0x008865). The one mode used is the array read
 *	every other line and column and binned to 1632 x 1224, 10-bit raw,
 *	with the input clock of 19.2 MHz:
 *
 *	- PLL1: 19.2 MHz / 2 x 75 = 720 MHz, a clock lane of 360 MHz (720
 *	  Mbit/s a lane) and the system clock; PLL2 from the same input;
 *	- the window worked out by the sensor from the output's size, with a
 *	  border of 8 pixels; a line is 1923 clocks, a frame 1248 lines;
 *	- the exposure (0x3500, 3 bytes, sixteenths of a line) and the gain
 *	  (0x3508, 2 bytes, 128 for 1x) set by hand; the black level kept by
 *	  the sensor's own correction;
 *	- streaming started and stopped at 0x0100; 0x0103 resets it.
 *
 *	The rest of the set-up (the analogue part, the black level correction,
 *	the lanes) is the sensor's reference settings. What each step reads
 *	back is said on the console: this can be checked only on the machine.
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
#define R_PUMP			0x3015
#define R_MIPI_CTRL		0x3018
#define R_PCLK_SEL		0x3020
#define R_MIPI_CTRL2		0x3022
#define R_MIPI_BITS		0x3031
#define R_CLK_SEL0		0x3032
#define R_CLK_SEL1		0x3033
#define R_SCLK			0x3106
#define R_EXPOSURE		0x3500
#define R_GAIN			0x3508
#define R_X_OUT			0x3808
#define R_Y_OUT			0x380a
#define R_HTS			0x380c
#define R_VTS			0x380e
#define R_X_INC_ODD		0x3814
#define R_X_INC_EVEN		0x3815
#define R_FORMAT1		0x3820
#define R_FORMAT2		0x3821
#define R_Y_INC_ODD		0x382a
#define R_Y_INC_EVEN		0x382b
#define R_ABLC_NUM		0x3830
#define R_ZLINE_NUM		0x3836
#define R_AUTO_SIZE		0x3841
#define R_AUTO_BORDER		0x3846
#define R_BLC_CTRL0		0x4000
#define R_BLC_CTRL1		0x4001
#define R_BLC_TRIG		0x400d
#define R_BLC_CTRL1F		0x401f
#define R_BLC_LIMIT		0x4034
#define R_VFIFO_START		0x4600
#define R_MIPI_LANE01		0x4850
#define R_MIPI_LANE23		0x4851
#define R_PCLK_PERIOD		0x4837
#define R_ISP0			0x5000
#define R_ISP1			0x5001
#define R_ISP2			0x5002
#define R_VAP			0x5901
#define R_TEST_PATTERN		0x5e00

#define FORMAT_FLIP		0x06		/* both of the bits for mirror (0x3821) or flip (0x3820) */
#define TEST_BARS		0x80

#define HTS			1923
#define VTS			1248
#define EXPOSURE_MARGIN		8
#define GAIN_MIN		128
#define GAIN_MAX		2048

typedef struct {
	UH	reg;
	UB	val;
} REGVAL;

/* The analogue part and the array's timing, for every mode */
LOCAL CONST REGVAL	setup_regs[] = {
	{ 0x3604, 0x04 }, { 0x3602, 0x30 }, { 0x3605, 0x00 }, { 0x3607, 0x20 }, { 0x3608, 0x11 },
	{ 0x3609, 0x68 }, { 0x360a, 0x40 }, { 0x360c, 0xdd }, { 0x360e, 0x0c }, { 0x3610, 0x07 },
	{ 0x3612, 0x86 }, { 0x3613, 0x58 }, { 0x3614, 0x28 }, { 0x3617, 0x40 }, { 0x3618, 0x5a },
	{ 0x3619, 0x9b }, { 0x361c, 0x00 }, { 0x361d, 0x60 }, { 0x3631, 0x60 }, { 0x3633, 0x10 },
	{ 0x3634, 0x10 }, { 0x3635, 0x10 }, { 0x3636, 0x10 }, { 0x3638, 0xff }, { 0x3641, 0x55 },
	{ 0x3646, 0x86 }, { 0x3647, 0x27 }, { 0x364a, 0x1b }, { 0x3700, 0x24 }, { 0x3701, 0x0c },
	{ 0x3702, 0x28 }, { 0x3703, 0x19 }, { 0x3704, 0x14 }, { 0x3705, 0x00 }, { 0x3706, 0x38 },
	{ 0x3707, 0x04 }, { 0x3708, 0x24 }, { 0x3709, 0x40 }, { 0x370a, 0x00 }, { 0x370b, 0xb8 },
	{ 0x370c, 0x04 }, { 0x3718, 0x12 }, { 0x3719, 0x31 }, { 0x3712, 0x42 }, { 0x3714, 0x12 },
	{ 0x371e, 0x19 }, { 0x371f, 0x40 }, { 0x3720, 0x05 }, { 0x3721, 0x05 }, { 0x3724, 0x02 },
	{ 0x3725, 0x02 }, { 0x3726, 0x06 }, { 0x3728, 0x05 }, { 0x3729, 0x02 }, { 0x372a, 0x03 },
	{ 0x372b, 0x53 }, { 0x372c, 0xa3 }, { 0x372d, 0x53 }, { 0x372e, 0x06 }, { 0x372f, 0x10 },
	{ 0x3730, 0x01 }, { 0x3731, 0x06 }, { 0x3732, 0x14 }, { 0x3733, 0x10 }, { 0x3734, 0x40 },
	{ 0x3736, 0x20 }, { 0x373a, 0x02 }, { 0x373b, 0x0c }, { 0x373c, 0x0a }, { 0x373e, 0x03 },
	{ 0x3755, 0x40 }, { 0x3758, 0x00 }, { 0x3759, 0x4c }, { 0x375a, 0x06 }, { 0x375b, 0x13 },
	{ 0x375c, 0x40 }, { 0x375d, 0x02 }, { 0x375e, 0x00 }, { 0x375f, 0x14 }, { 0x3767, 0x1c },
	{ 0x3768, 0x04 }, { 0x3769, 0x20 }, { 0x376c, 0xc0 }, { 0x376d, 0xc0 }, { 0x376a, 0x08 },
	{ 0x3761, 0x00 }, { 0x3762, 0x00 }, { 0x3763, 0x00 }, { 0x3766, 0xff }, { 0x376b, 0x42 },
	{ 0x3772, 0x23 }, { 0x3773, 0x02 }, { 0x3774, 0x16 }, { 0x3775, 0x12 }, { 0x3776, 0x08 },
	{ 0x37a0, 0x44 }, { 0x37a1, 0x3d }, { 0x37a2, 0x3d }, { 0x37a3, 0x01 }, { 0x37a4, 0x00 },
	{ 0x37a5, 0x08 }, { 0x37a6, 0x00 }, { 0x37a7, 0x44 }, { 0x37a8, 0x58 }, { 0x37a9, 0x58 },
	{ 0x3760, 0x00 }, { 0x376f, 0x01 }, { 0x37aa, 0x44 }, { 0x37ab, 0x2e }, { 0x37ac, 0x2e },
	{ 0x37ad, 0x33 }, { 0x37ae, 0x0d }, { 0x37af, 0x0d }, { 0x37b0, 0x00 }, { 0x37b1, 0x00 },
	{ 0x37b2, 0x00 }, { 0x37b3, 0x42 }, { 0x37b4, 0x42 }, { 0x37b5, 0x33 }, { 0x37b6, 0x00 },
	{ 0x37b7, 0x00 }, { 0x37b8, 0x00 }, { 0x37b9, 0xff }, { 0x4503, 0x10 },
};

/* The clocks of the mode: PLL1, PLL2 and the system clock, for an input of 19.2 MHz */
LOCAL CONST REGVAL	clock_regs[] = {
	{ R_MIPI_BITS, 0x0a },			/* 10 bits a pixel */
	{ 0x030a, 0x00 }, { 0x0300, 0x02 }, { 0x0301, 0x00 }, { 0x0302, 75 }, { 0x0303, 0x00 },
	{ 0x0304, 0x03 }, { 0x0305, 0x01 }, { 0x0306, 0x01 },
	{ 0x0312, 0x01 }, { 0x030b, 0x02 }, { 0x030c, 0x00 }, { 0x030d, 75 }, { 0x030f, 0x09 }, { 0x030e, 0x00 },
	{ R_CLK_SEL0, 0x80 }, { R_SCLK, 0x01 },
};

/* The mode's timing of the array and its black level lines, read binned */
LOCAL CONST REGVAL	mode_regs[] = {
	{ R_X_INC_ODD, 3 }, { R_X_INC_EVEN, 1 }, { R_Y_INC_ODD, 3 }, { R_Y_INC_EVEN, 1 },
	{ R_AUTO_SIZE, 0x3f }, { R_AUTO_BORDER, 0x88 },
	{ R_VFIFO_START, 0x00 }, { R_VFIFO_START + 1, 116 }, { R_ABLC_NUM, 8 }, { R_ZLINE_NUM, 2 },
	{ R_VAP, 0x00 },
	{ R_BLC_CTRL1, 0x04 },
	{ 0x4028, 0 }, { 0x4029, 2 }, { 0x402a, 4 }, { 0x402b, 4 }, { 0x402c, 2 }, { 0x402d, 2 }, { 0x402e, 8 }, { 0x402f, 2 },
	{ 0x4020, 0x01 }, { 0x4021, 0x20 }, { 0x4022, 0x01 }, { 0x4023, 0x9f },
	{ 0x4024, 0x03 }, { 0x4025, 0xe0 }, { 0x4026, 0x04 }, { 0x4027, 0x5f },
	{ 0x3f08, 0x0b }, { 0x4500, 0x40 },
};

LOCAL ER wr( T_OV5693 *s, UH reg, UW v, INT n )
{
	UB	b[2 + 4];
	INT	i;

	b[0] = (UB)( reg >> 8 );
	b[1] = (UB)reg;
	for ( i = 0; i < n && i < 4; i++ ) b[2 + i] = (UB)( v >> ( 8 * ( n - 1 - i ) ) );
	return s->bus->xfer(s->bus->ctx, s->addr, b, 2 + n, NULL, 0);
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

LOCAL INT table( T_OV5693 *s, CONST REGVAL *t, INT n )
{
	INT	i, bad = 0;

	for ( i = 0; i < n; i++ ) {
		if ( wr(s, t[i].reg, t[i].val, 1) < E_OK ) bad++;
	}
	return bad;
}

LOCAL ER probe( T_OV5693 *s )
{
	INT	id;

	/* a reset first: its ID is read as the sensor stands after one, on stand-by */
	(void)wr(s, R_RESET, 1, 1);
	tk_dly_tsk(2);
	(void)wr(s, R_STREAM, 0, 1);
	id = rd(s, R_CHIP_ID, 3);
	tm_printf((UB *)"ov8865: on %s at %02x: chip ID %06x (an OV8865 says %06x)\n", s->bus->name, s->addr,
		  id < 0 ? 0xffffff : id, OV8865_CHIP_ID);
	if ( id < 0 ) return E_IO;
	s->chip = (UW)id;
	return ( id == OV8865_CHIP_ID ) ? E_OK : E_NOEXS;
}

LOCAL UW max_exposure( T_OV5693 *s )
{
	return s->vts - EXPOSURE_MARGIN;
}

LOCAL ER exposure( T_OV5693 *s, UW e, UW g )
{
	ER	er;

	if ( e < 2 ) e = 2;
	if ( e > max_exposure(s) ) e = max_exposure(s);
	if ( g < GAIN_MIN ) g = GAIN_MIN;
	if ( g > GAIN_MAX ) g = GAIN_MAX;
	er = wr(s, R_EXPOSURE, e << 4, 3);
	if ( er >= E_OK ) er = wr(s, R_GAIN, g, 2);
	if ( er >= E_OK ) {
		s->exposure = e;
		s->gain = g;
	}
	return er;
}

LOCAL ER setup( T_OV5693 *s )
{
	INT	bad = 0, v;
	ER	er;

	er = wr(s, R_STREAM, 0, 1);
	if ( er >= E_OK ) er = wr(s, R_RESET, 1, 1);
	if ( er < E_OK ) return er;
	tk_dly_tsk(2);
	(void)wr(s, R_STREAM, 0, 1);
	bad += table(s, setup_regs, (INT)( sizeof(setup_regs) / sizeof(setup_regs[0]) ));
	(void)wr(s, R_PUMP, 0x01, 1);

	/* the lanes the board wires, each to itself */
	(void)wr(s, R_MIPI_CTRL, ( (UW)( s->lanes - 1 ) << 5 ) | 0x12, 1);
	(void)wr(s, R_MIPI_CTRL2, 0x01, 1);
	(void)wr(s, R_MIPI_LANE01, 0x10, 1);
	if ( s->lanes >= 4 ) (void)wr(s, R_MIPI_LANE23, 0x32, 1);
	v = rd(s, R_CLK_SEL1, 1);
	if ( v >= 0 ) (void)wr(s, R_CLK_SEL1, ( (UW)v | 0x20 ) & ~(UW)0x02, 1);
	(void)wr(s, R_PCLK_PERIOD, 0x16, 1);

	/* the sensor's corrections: defects and white balance's gains on, its lens correction off */
	(void)wr(s, R_ISP0, 0x16, 1);
	(void)wr(s, R_ISP1, 0x01, 1);
	(void)wr(s, R_BLC_CTRL0, 0xf1, 1);
	(void)wr(s, R_BLC_TRIG, 0x10, 1);
	(void)wr(s, R_BLC_CTRL1F, 0x00, 1);
	(void)wr(s, R_BLC_LIMIT, 0x3f, 1);

	s->vts = VTS;
	(void)wr(s, R_X_OUT, OV8865_WIDTH, 2);
	(void)wr(s, R_Y_OUT, OV8865_HEIGHT, 2);
	(void)wr(s, R_HTS, HTS, 2);
	(void)wr(s, R_VTS, s->vts, 2);
	/* turned half round in the case: mirrored and flipped, the same pixels first */
	(void)wr(s, R_FORMAT1, s->turned ? FORMAT_FLIP : 0, 1);
	(void)wr(s, R_FORMAT2, 0x40 | 0x20 | 0x01 | ( s->turned ? FORMAT_FLIP : 0 ), 1);
	v = rd(s, R_ISP2, 1);
	if ( v >= 0 ) (void)wr(s, R_ISP2, (UW)v & ~(UW)0x04, 1);
	bad += table(s, mode_regs, (INT)( sizeof(mode_regs) / sizeof(mode_regs[0]) ));
	bad += table(s, clock_regs, (INT)( sizeof(clock_regs) / sizeof(clock_regs[0]) ));
	v = rd(s, R_PCLK_SEL, 1);
	if ( v >= 0 ) (void)wr(s, R_PCLK_SEL, (UW)v & ~(UW)0x08, 1);
	v = rd(s, 0x031e, 1);
	if ( v >= 0 ) (void)wr(s, 0x031e, (UW)v | 0x08, 1);

	er = exposure(s, s->vts / 4, GAIN_MIN);
	(void)wr(s, R_TEST_PATTERN, s->bars ? TEST_BARS : 0, 1);
	if ( bad > 0 ) tm_printf((UB *)"ov8865: %d of the set-up's writes not answered\n", bad);
	return ( bad > 0 ) ? E_IO : er;
}

LOCAL ER stream( T_OV5693 *s, BOOL on )
{
	return wr(s, R_STREAM, on ? 1 : 0, 1);
}

LOCAL void say( T_OV5693 *s, CONST char *when )
{
	tm_printf((UB *)"ov8865: %s: stream %x, PLL1 %02x %02x%02x %02x %02x, MIPI %02x, out %dx%d, HTS %d VTS %d, "
		  "inc %02x %02x %02x %02x, format %02x %02x, exposure %06x gain %04x, test %02x\n",
		  when, rd(s, R_STREAM, 1), rd(s, 0x0300, 1), rd(s, 0x0301, 1), rd(s, 0x0302, 1), rd(s, 0x0303, 1),
		  rd(s, 0x0304, 1), rd(s, R_MIPI_CTRL, 1), rd(s, R_X_OUT, 2), rd(s, R_Y_OUT, 2), rd(s, R_HTS, 2),
		  rd(s, R_VTS, 2), rd(s, R_X_INC_ODD, 1), rd(s, R_X_INC_EVEN, 1), rd(s, R_Y_INC_ODD, 1),
		  rd(s, R_Y_INC_EVEN, 1), rd(s, R_FORMAT1, 1), rd(s, R_FORMAT2, 1), rd(s, R_EXPOSURE, 3),
		  rd(s, R_GAIN, 2), rd(s, R_TEST_PATTERN, 1));
}

EXPORT CONST T_SENSOPS	knl_ov8865_ops = {
	"OV8865", "Back camera (OV8865)", OV8865_WIDTH, OV8865_HEIGHT, OV8865_LINK_HZ, 2, 4,
	OV8865_BLACK, OV8865_INTERVAL, GAIN_MIN, OV8865_MCLK,
	probe, setup, stream, exposure, max_exposure, say
};

#endif /* X64 */
