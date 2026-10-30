/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cam_isp.h
 *	A camera's raw Bayer pictures made into the camera device's formats
 *	in software (cam_isp.c): a sensor that gives 10-bit raw through a
 *	receiver of its own (device/cam/x86_64/cio2.c) has no image processor
 *	in front of the device as a USB camera has.
 *
 *	Steps, in the order a pixel meets them: the receiver's packing undone
 *	(IPU3's: 25 pixels in 32 bytes, the bits of each pixel one after
 *	another from the lowest bit of the first byte); the colour of each
 *	pixel completed from its neighbours (bilinear, or one pixel of each
 *	colour from a 2 x 2 cell at half the size); the black level taken
 *	off, the white balance's gains and a gamma of 0.5 applied through one
 *	table a colour; the colours made Y'CbCr (BT.601, studio range) as
 *	NV12 or YUY2. While a frame is made, its statistics are gathered (the
 *	mean of each colour and a histogram of green), from which the next
 *	frame's white balance (the grey world) and exposure come.
 *
 *	Integers only: it runs in the kernel, whose code uses no floating
 *	point registers.
 */

#ifndef _DEVICE_CAM_CAM_ISP_
#define _DEVICE_CAM_CAM_ISP_

/* Where red is in a 2 x 2 cell: bit 0 its column, bit 1 its row */
#define ISP_RGGB	0
#define ISP_GRBG	1
#define ISP_GBRG	2
#define ISP_BGGR	3

#define ISP_MAX_W	4224		/* the widest line the work rows hold */
#define ISP_HBINS	64		/* the histogram of green: 10-bit values in 16s */

typedef struct {
	INT	bayer;			/* ISP_* */
	UW	black;			/* the black level (10 bits) */
	UW	black_max;		/* the sensor's: the black level is never put above it */
	UW	wb[3];			/* red, green and blue gains, 256 for 1 */
	UB	lut[3][1024];		/* a raw value of each colour to its 8-bit output */
	/* statistics of the frame made last */
	UD	sum[3];			/* the colours of the cells looked at, the black level off */
	UW	ncell;
	UW	hist[ISP_HBINS];	/* green, before the black level is taken off */
	UW	nsat;			/* cells with a colour at the top */
	/* the rows a frame is made from, unpacked */
	UH	row[3][ISP_MAX_W + 2];
	UB	pix[3 * ISP_MAX_W + 6];	/* an output row as red, green and blue bytes */
} T_CAMISP;

/* CAMFMT_NV12 and CAMFMT_YUY2 (include/ts/cam.h) are the formats made */

/* The bytes of a line of IPU3's packed 10-bit raw of width pixels (the receiver writes them so) */
IMPORT INT	cam_isp_ipu3_bpl( INT width );
/* n pixels of a line unpacked, from its start */
IMPORT void	cam_isp_ipu3_unpack( CONST UB *src, UH *dst, INT n );
/* The settings to start with: the sensor's black level, gains of 1, the tables made */
IMPORT void	cam_isp_init( T_CAMISP *s, INT bayer, UW black );
/* The tables made again from the black level and the gains */
IMPORT void	cam_isp_tables( T_CAMISP *s );
/*
 * A frame of w x h raw pixels, lines bpl bytes apart, made into an ow x
 * oh picture of the format fmt at out: the middle of the frame at its
 * size, or, when the frame is at least twice as wide and high, the
 * middle 2*ow x 2*oh of it at half the size. The statistics are those of
 * the part used. The bytes made, or E_PAR when the sizes do not fit.
 */
IMPORT INT	cam_isp_frame( T_CAMISP *s, CONST UB *raw, INT bpl, INT w, INT h, UB *out, INT fmt, INT ow, INT oh );
/*
 * From the statistics: the white balance moved toward the grey world,
 * the black level brought down to the darkest values seen, the tables
 * made again; and how much more light the next frames want, 256 for as
 * much as now (from 64 to 1024, half the way toward what the mean asks)
 */
IMPORT UW	cam_isp_auto( T_CAMISP *s );

#endif /* _DEVICE_CAM_CAM_ISP_ */
