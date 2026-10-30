/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_camisp.c (x64 PC)
 *	The raw Bayer pictures of the notebook's own camera made into the
 *	camera device's formats (device/cam/cam_isp.c), with frames made
 *	here, as the IPU3's receiver would write them: the packing of 25
 *	pixels in 32 bytes undone exactly (test_unpack); a grey scene grey
 *	at both sizes and in both formats, black at the black level and
 *	near white at the top (test_grey); a red scene red, and blue when
 *	the colours are taken in the other order (test_colour); sizes that
 *	do not fit refused (test_sizes); the white balance and the exposure
 *	moving the right way (test_auto).
 */

#include "../ktest.h"
#include "tstdlib.h"
#include <ts/cam.h>
#include "../../../device/cam/cam_isp.h"

#define RW		64		/* the raw frame */
#define RH		48
#define BPL		128		/* cam_isp_ipu3_bpl(64) */

LOCAL UB	raw[BPL * RH];
LOCAL UB	out[RW * RH * 2];
LOCAL T_CAMISP	*isp;

/* A pixel put into a packed line, its bits after those of the pixels before it in its block */
LOCAL void pack( UB *line, INT i, UW v )
{
	UB	*blk = line + ( i / 25 ) * 32;
	INT	bit = ( i % 25 ) * 10, k;

	for ( k = 0; k < 10; k++, bit++ ) {
		if ( v & ( 1U << k ) ) blk[bit >> 3] |= (UB)( 1U << ( bit & 7 ) );
		else blk[bit >> 3] &= (UB)~( 1U << ( bit & 7 ) );
	}
}

/* A frame whose pixel of each colour (red, green, blue in the order bayer) has the value of that colour */
LOCAL void make_frame( INT bayer, UW r, UW g, UW b )
{
	INT	x, y, rx = bayer & 1, ry = ( bayer >> 1 ) & 1;

	knl_memset(raw, 0, sizeof(raw));
	for ( y = 0; y < RH; y++ ) {
		for ( x = 0; x < RW; x++ ) {
			BOOL	xr = ( ( x & 1 ) == rx ), yr = ( ( y & 1 ) == ry );

			pack(raw + y * BPL, x, ( xr && yr ) ? r : ( !xr && !yr ) ? b : g);
		}
	}
}

LOCAL void test_unpack( void )
{
	UB	line[128];
	UH	px[50];
	INT	i;

	KT_ASSERT_EQ(cam_isp_ipu3_bpl(1296), 1664);
	KT_ASSERT_EQ(cam_isp_ipu3_bpl(50), 64);
	KT_ASSERT_EQ(cam_isp_ipu3_bpl(51), 128);
	knl_memset(line, 0, sizeof(line));
	for ( i = 0; i < 50; i++ ) pack(line, i, (UW)( ( i * 97 + 13 ) & 0x3ff ));
	pack(line, 49, 0x3ff);
	pack(line, 25, 0x201);
	cam_isp_ipu3_unpack(line, px, 50);
	for ( i = 0; i < 50; i++ ) {
		UW	want = ( i == 49 ) ? 0x3ff : ( i == 25 ) ? 0x201 : (UW)( ( i * 97 + 13 ) & 0x3ff );

		if ( px[i] != want ) {
			KT_ASSERT_EQ(px[i], want);
			break;
		}
	}
	KT_ASSERT_EQ(i, 50);
	/* the first block's last pixel ends at bit 250: the 6 bits after it are not a pixel */
	knl_memset(line, 0, sizeof(line));
	line[31] = 0xfc;
	cam_isp_ipu3_unpack(line, px, 25);
	KT_ASSERT_EQ(px[24], 0);
}

/* Every Y, U and V of a picture within lo..hi, lo..hi and clo..chi */
LOCAL BOOL all_in( INT fmt, INT ow, INT oh, INT lo, INT hi, INT clo, INT chi )
{
	INT	i, n = ow * oh;

	if ( fmt == CAMFMT_YUY2 ) {
		for ( i = 0; i < n * 2; i++ ) {
			INT	v = out[i];

			if ( ( i & 1 ) == 0 ? ( v < lo || v > hi ) : ( v < clo || v > chi ) ) return FALSE;
		}
		return TRUE;
	}
	for ( i = 0; i < n; i++ ) if ( out[i] < lo || out[i] > hi ) return FALSE;
	for ( i = n; i < n + n / 2; i++ ) if ( out[i] < clo || out[i] > chi ) return FALSE;
	return TRUE;
}

LOCAL void test_grey( void )
{
	INT	f, k;
	static CONST INT fmts[2] = { CAMFMT_NV12, CAMFMT_YUY2 };
	static CONST INT sz[2][2] = { { 32, 24 }, { 60, 40 } };	/* half the size, and the middle at its size */

	for ( f = 0; f < 2; f++ ) {
		for ( k = 0; k < 2; k++ ) {
			INT	ow = sz[k][0], oh = sz[k][1], n;

			cam_isp_init(isp, ISP_BGGR, 64);
			make_frame(ISP_BGGR, 64, 64, 64);
			n = cam_isp_frame(isp, raw, BPL, RW, RH, out, fmts[f], ow, oh);
			KT_ASSERT_EQ(n, ( fmts[f] == CAMFMT_YUY2 ) ? ow * oh * 2 : ow * oh * 3 / 2);
			KT_ASSERT(all_in(fmts[f], ow, oh, 16, 16, 128, 128));
			make_frame(ISP_BGGR, 1023, 1023, 1023);
			(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, fmts[f], ow, oh);
			KT_ASSERT(all_in(fmts[f], ow, oh, 233, 236, 127, 129));
			make_frame(ISP_BGGR, 300, 300, 300);
			(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, fmts[f], ow, oh);
			KT_ASSERT(all_in(fmts[f], ow, oh, 100, 180, 127, 129));
		}
	}
}

/* The mean U and V of an NV12 picture */
LOCAL void mean_uv( INT ow, INT oh, INT *u, INT *v )
{
	INT	i, n = ow * oh / 4, su = 0, sv = 0;
	CONST UB *c = out + ow * oh;

	for ( i = 0; i < n; i++ ) {
		su += c[2 * i];
		sv += c[2 * i + 1];
	}
	*u = su / n;
	*v = sv / n;
}

LOCAL void test_colour( void )
{
	INT	k, u, v;
	static CONST INT sz[2][2] = { { 32, 24 }, { 60, 40 } };

	for ( k = 0; k < 2; k++ ) {
		/* red: V well above the middle, U below */
		cam_isp_init(isp, ISP_BGGR, 64);
		make_frame(ISP_BGGR, 900, 64, 64);
		(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, sz[k][0], sz[k][1]);
		mean_uv(sz[k][0], sz[k][1], &u, &v);
		KT_ASSERT(v > 200);
		KT_ASSERT(u < 110);
		/* the same frame read as RGGB: the red pixels taken for blue */
		cam_isp_init(isp, ISP_RGGB, 64);
		(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, sz[k][0], sz[k][1]);
		mean_uv(sz[k][0], sz[k][1], &u, &v);
		KT_ASSERT(u > 200);
		KT_ASSERT(v < 128);
		/* green, in GRBG: both below the middle */
		cam_isp_init(isp, ISP_GRBG, 64);
		make_frame(ISP_GRBG, 64, 900, 64);
		(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, sz[k][0], sz[k][1]);
		mean_uv(sz[k][0], sz[k][1], &u, &v);
		KT_ASSERT(u < 90);
		KT_ASSERT(v < 90);
	}
}

LOCAL void test_sizes( void )
{
	cam_isp_init(isp, ISP_BGGR, 64);
	make_frame(ISP_BGGR, 200, 200, 200);
	KT_ASSERT_ER(cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_MJPEG, 32, 24), E_PAR);
	KT_ASSERT_ER(cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 31, 24), E_PAR);
	KT_ASSERT_ER(cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 66, 40), E_PAR);
	KT_ASSERT_ER(cam_isp_frame(isp, raw, 64, RW, RH, out, CAMFMT_NV12, 32, 24), E_PAR);
	KT_ASSERT_EQ(cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, RW, RH), RW * RH * 3 / 2);
}

LOCAL void test_auto( void )
{
	INT	i;
	UW	scale = 0;

	/* red twice green, blue half of it: red's gain goes down, blue's up */
	cam_isp_init(isp, ISP_BGGR, 64);
	make_frame(ISP_BGGR, 64 + 400, 64 + 200, 64 + 100);
	for ( i = 0; i < 20; i++ ) {
		(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 32, 24);
		scale = cam_isp_auto(isp);
	}
	KT_ASSERT(isp->wb[0] >= 120 && isp->wb[0] <= 140);
	KT_ASSERT(isp->wb[2] >= 480 && isp->wb[2] <= 540);
	KT_ASSERT_EQ(isp->wb[1], 256);
	tm_printf((UB *)"camisp: white balance %d %d %d, exposure %d/256\n", isp->wb[0], isp->wb[1], isp->wb[2], scale);
	/* dark: more light wanted; bright and saturated: less */
	make_frame(ISP_BGGR, 70, 70, 70);
	(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 32, 24);
	KT_ASSERT(cam_isp_auto(isp) > 256);
	make_frame(ISP_BGGR, 1023, 1023, 1023);
	(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 32, 24);
	KT_ASSERT(cam_isp_auto(isp) < 256);
	/* a scene darker than the black level brings it down, never up again */
	make_frame(ISP_BGGR, 40, 40, 40);
	(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 32, 24);
	(void)cam_isp_auto(isp);
	KT_ASSERT(isp->black <= 40);
	make_frame(ISP_BGGR, 300, 300, 300);
	(void)cam_isp_frame(isp, raw, BPL, RW, RH, out, CAMFMT_NV12, 32, 24);
	(void)cam_isp_auto(isp);
	KT_ASSERT(isp->black <= 40);
}

EXPORT void ktest_camisp( void )
{
	isp = (T_CAMISP *)Kmalloc(sizeof(T_CAMISP));
	if ( isp == NULL ) {
		kt_skip("camisp", "no memory");
		return;
	}
	KT_RUN(test_unpack);
	KT_RUN(test_grey);
	KT_RUN(test_colour);
	KT_RUN(test_sizes);
	KT_RUN(test_auto);
	Kfree(isp);
}
