/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cam_isp.c
 *	Raw Bayer pictures made into NV12 or YUY2 in software (cam_isp.h).
 *
 *	A frame is made a row of the output at a time. At its size, the
 *	three raw rows around the output row are kept unpacked (the row
 *	above, the row, the row below), each unpacked once as the window
 *	moves down; a pixel's missing colours are the means of its
 *	neighbours of those colours (the two or four nearest). At half the
 *	size, the two rows of a cell are unpacked and the cell gives one
 *	pixel: its red, its blue and the mean of its two greens. The values
 *	are 10-bit raw until the table of their colour turns them into 8
 *	bits with the black level off, the gain of the white balance on and
 *	the gamma applied, so the interpolation is done on what the sensor
 *	measured.
 *
 *	The statistics are taken from every fourth cell of every fourth
 *	cell row of the part of the frame used.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <ts/cam.h>
#include "cam_isp.h"

#define STAT_STEP	4		/* cells: every fourth across and down */
#define TOP10		1020		/* a raw value counted as saturated */

EXPORT INT cam_isp_ipu3_bpl( INT width )
{
	/* 25 pixels to 32 bytes, lines kept to 64 bytes */
	return ( ( width + 49 ) / 50 ) * 64;
}

EXPORT void cam_isp_ipu3_unpack( CONST UB *src, UH *dst, INT n )
{
	INT	i, k, bit;

	for ( i = 0; i < n; i += 25, src += 32 ) {
		for ( k = 0, bit = 0; k < 25 && i + k < n; k++, bit += 10 ) {
			UINT	b = (UINT)bit >> 3;

			dst[i + k] = (UH)( ( ( (UINT)src[b] | ( (UINT)src[b + 1] << 8 ) ) >> ( bit & 7 ) ) & 0x3ff );
		}
	}
}

LOCAL UINT isqrt( UINT v )
{
	UINT	r = 0, bit = 1U << 30;

	while ( bit > v ) bit >>= 2;
	while ( bit != 0 ) {
		if ( v >= r + bit ) {
			v -= r + bit;
			r = ( r >> 1 ) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

EXPORT void cam_isp_tables( T_CAMISP *s )
{
	UINT	c, v, range = ( s->black < 1000 ) ? 1023 - s->black : 23;

	for ( c = 0; c < 3; c++ ) {
		for ( v = 0; v < 1024; v++ ) {
			UD	lin = ( v > s->black ) ? v - s->black : 0;
			UINT	o;

			/* to 12 bits of linear light, then the square root: 0..4095 gives 0..255 */
			lin = lin * s->wb[c] * 4095 / ( (UD)range * 256 );
			if ( lin > 4095 ) lin = 4095;
			o = isqrt((UINT)lin * 16);
			s->lut[c][v] = (UB)( o > 255 ? 255 : o );
		}
	}
}

EXPORT void cam_isp_init( T_CAMISP *s, INT bayer, UW black )
{
	s->bayer = bayer & 3;
	s->black = s->black_max = ( black < 512 ) ? black : 64;
	s->wb[0] = s->wb[1] = s->wb[2] = 256;
	s->ncell = 0;
	cam_isp_tables(s);
}

/* ---------------------------------------------------------------- statistics */

LOCAL void stats_clear( T_CAMISP *s )
{
	INT	i;

	s->sum[0] = s->sum[1] = s->sum[2] = 0;
	s->ncell = 0;
	s->nsat = 0;
	for ( i = 0; i < ISP_HBINS; i++ ) s->hist[i] = 0;
}

/* One cell: r, the two greens and b, raw */
LOCAL void stats_cell( T_CAMISP *s, UINT r, UINT g0, UINT g1, UINT b )
{
	UINT	g = ( g0 + g1 ) >> 1, bl = s->black;

	s->sum[0] += ( r > bl ) ? r - bl : 0;
	s->sum[1] += ( g > bl ) ? g - bl : 0;
	s->sum[2] += ( b > bl ) ? b - bl : 0;
	s->hist[g >> 4]++;
	if ( r >= TOP10 || g >= TOP10 || b >= TOP10 ) s->nsat++;
	s->ncell++;
}

/* The cells of a pair of rows a and b (a the even one), from column x0 for cw cells */
LOCAL void stats_rows( T_CAMISP *s, CONST UH *a, CONST UH *b, INT x0, INT cw )
{
	INT	rx = s->bayer & 1, ry = ( s->bayer >> 1 ) & 1, i;
	CONST UH *rrow = ry ? b : a, *brow = ry ? a : b;

	for ( i = 0; i < cw; i += STAT_STEP ) {
		INT	x = x0 + 2 * i;

		stats_cell(s, rrow[x + rx], rrow[x + 1 - rx], brow[x + rx], brow[x + 1 - rx]);
	}
}

/* ---------------------------------------------------------------- output */

#define Y_OF(r, g, b)	( (UB)( ( ( 66 * (INT)(r) + 129 * (INT)(g) + 25 * (INT)(b) + 128 ) >> 8 ) + 16 ) )
#define U_OF(r, g, b)	( (UB)( ( ( -38 * (INT)(r) - 74 * (INT)(g) + 112 * (INT)(b) + 128 ) >> 8 ) + 128 ) )
#define V_OF(r, g, b)	( (UB)( ( ( 112 * (INT)(r) - 94 * (INT)(g) - 18 * (INT)(b) + 128 ) >> 8 ) + 128 ) )

/*
 * One output row of RGB pairs: pix[] holds ow pixels as r, g, b bytes.
 * NV12's chroma is that of the even rows, of each pair of pixels.
 */
LOCAL void put_row( UB *out, INT fmt, INT ow, INT oh, INT y, CONST UB *pix )
{
	INT	x;

	if ( fmt == CAMFMT_YUY2 ) {
		UB	*d = out + (SZ)y * ow * 2;

		for ( x = 0; x < ow; x += 2, pix += 6, d += 4 ) {
			INT	r = ( pix[0] + pix[3] + 1 ) >> 1, g = ( pix[1] + pix[4] + 1 ) >> 1, b = ( pix[2] + pix[5] + 1 ) >> 1;

			d[0] = Y_OF(pix[0], pix[1], pix[2]);
			d[1] = U_OF(r, g, b);
			d[2] = Y_OF(pix[3], pix[4], pix[5]);
			d[3] = V_OF(r, g, b);
		}
		return;
	}
	{
		UB	*yp = out + (SZ)y * ow;
		UB	*uv = out + (SZ)ow * oh + (SZ)( y >> 1 ) * ow;
		BOOL	chroma = ( ( y & 1 ) == 0 );

		for ( x = 0; x < ow; x += 2, pix += 6 ) {
			yp[x] = Y_OF(pix[0], pix[1], pix[2]);
			yp[x + 1] = Y_OF(pix[3], pix[4], pix[5]);
			if ( chroma ) {
				INT	r = ( pix[0] + pix[3] + 1 ) >> 1, g = ( pix[1] + pix[4] + 1 ) >> 1;
				INT	b = ( pix[2] + pix[5] + 1 ) >> 1;

				uv[x] = U_OF(r, g, b);
				uv[x + 1] = V_OF(r, g, b);
			}
		}
	}
}

/* The kinds of a pixel of a row: red, green beside red, green beside blue, blue */
#define K_R	0
#define K_GR	1
#define K_GB	2
#define K_B	3

/* A pixel's three colours from the rows above (u), at (m) and below (d), as 8 bits */
LOCAL void demosaic( CONST T_CAMISP *s, INT kind, CONST UH *u, CONST UH *m, CONST UH *d, INT x, UB *p )
{
	UINT	r, g, b;

	switch ( kind ) {
	case K_R:
		r = m[x];
		g = ( (UINT)u[x] + d[x] + m[x - 1] + m[x + 1] + 2 ) >> 2;
		b = ( (UINT)u[x - 1] + u[x + 1] + d[x - 1] + d[x + 1] + 2 ) >> 2;
		break;
	case K_B:
		b = m[x];
		g = ( (UINT)u[x] + d[x] + m[x - 1] + m[x + 1] + 2 ) >> 2;
		r = ( (UINT)u[x - 1] + u[x + 1] + d[x - 1] + d[x + 1] + 2 ) >> 2;
		break;
	case K_GR:
		g = m[x];
		r = ( (UINT)m[x - 1] + m[x + 1] + 1 ) >> 1;
		b = ( (UINT)u[x] + d[x] + 1 ) >> 1;
		break;
	default:
		g = m[x];
		b = ( (UINT)m[x - 1] + m[x + 1] + 1 ) >> 1;
		r = ( (UINT)u[x] + d[x] + 1 ) >> 1;
		break;
	}
	p[0] = s->lut[0][r];
	p[1] = s->lut[1][g];
	p[2] = s->lut[2][b];
}

/* The row y of the frame (mirrored at the edges) unpacked */
LOCAL void unpack_row( CONST UB *raw, INT bpl, INT w, INT h, INT y, UH *dst )
{
	if ( y < 0 ) y = -y;
	if ( y >= h ) y = 2 * h - 2 - y;
	cam_isp_ipu3_unpack(raw + (SZ)y * bpl, dst, w);
}

EXPORT INT cam_isp_frame( T_CAMISP *s, CONST UB *raw, INT bpl, INT w, INT h, UB *out, INT fmt, INT ow, INT oh )
{
	INT	rx = s->bayer & 1, ry = ( s->bayer >> 1 ) & 1;
	INT	x0, y0, x, y;
	UB	*pix = s->pix;
	BOOL	half;

	if ( fmt != CAMFMT_NV12 && fmt != CAMFMT_YUY2 ) return E_PAR;
	if ( w > ISP_MAX_W || ow <= 0 || oh <= 0 || ( ow & 1 ) || ( oh & 1 ) || w < 4 || h < 4 ) return E_PAR;
	if ( bpl < cam_isp_ipu3_bpl(w) && bpl < ( ( w + 24 ) / 25 ) * 32 ) return E_PAR;
	half = ( 2 * ow <= w && 2 * oh <= h );
	if ( !half && ( ow > w || oh > h ) ) return E_PAR;
	stats_clear(s);

	if ( half ) {
		/* the middle 2*ow x 2*oh, from an even row and column so that the cells keep their order */
		x0 = ( ( w - 2 * ow ) / 2 ) & ~1;
		y0 = ( ( h - 2 * oh ) / 2 ) & ~1;
		for ( y = 0; y < oh; y++ ) {
			CONST UH *a = s->row[0], *b = s->row[1], *rrow, *brow;

			unpack_row(raw, bpl, w, h, y0 + 2 * y, s->row[0]);
			unpack_row(raw, bpl, w, h, y0 + 2 * y + 1, s->row[1]);
			rrow = ry ? b : a;
			brow = ry ? a : b;
			for ( x = 0; x < ow; x++ ) {
				INT	c = x0 + 2 * x;

				pix[3 * x] = s->lut[0][rrow[c + rx]];
				pix[3 * x + 1] = s->lut[1][( (UINT)rrow[c + 1 - rx] + brow[c + rx] + 1 ) >> 1];
				pix[3 * x + 2] = s->lut[2][brow[c + 1 - rx]];
			}
			put_row(out, fmt, ow, oh, y, pix);
			if ( ( y % STAT_STEP ) == 0 ) stats_rows(s, a, b, x0, ow);
		}
	} else {
		UH	*u, *m, *d, *t;

		x0 = ( ( w - ow ) / 2 ) & ~1;
		y0 = ( ( h - oh ) / 2 ) & ~1;
		u = s->row[0];
		m = s->row[1];
		d = s->row[2];
		unpack_row(raw, bpl, w, h, y0 - 1, u);
		unpack_row(raw, bpl, w, h, y0, m);
		for ( y = 0; y < oh; y++ ) {
			INT	fy = y0 + y, k0, k1;
			BOOL	rrow = ( ( fy & 1 ) == ry );

			unpack_row(raw, bpl, w, h, fy + 1, d);
			/* the left and right edges: their outer neighbour is the one inside, mirrored */
			if ( x0 == 0 ) {
				u[w] = u[w - 2]; m[w] = m[w - 2]; d[w] = d[w - 2];
			}
			/* the kinds of the even and the odd columns of this row */
			if ( rrow ) {
				k0 = ( rx == 0 ) ? K_R : K_GR;
				k1 = ( rx == 0 ) ? K_GR : K_R;
			} else {
				k0 = ( rx == 0 ) ? K_GB : K_B;
				k1 = ( rx == 0 ) ? K_B : K_GB;
			}
			for ( x = 0; x < ow; x += 2 ) {
				INT	fx = x0 + x;

				if ( fx == 0 ) {
					/* column -1 is column 1 */
					UH	su = u[1], sm = m[1], sd = d[1];
					UH	cu[3], cm[3], cd[3];

					cu[0] = su; cu[1] = u[0]; cu[2] = u[1];
					cm[0] = sm; cm[1] = m[0]; cm[2] = m[1];
					cd[0] = sd; cd[1] = d[0]; cd[2] = d[1];
					demosaic(s, k0, cu, cm, cd, 1, &pix[3 * x]);
				} else {
					demosaic(s, k0, u, m, d, fx, &pix[3 * x]);
				}
				demosaic(s, k1, u, m, d, fx + 1, &pix[3 * x + 3]);
			}
			put_row(out, fmt, ow, oh, y, pix);
			if ( ( fy & 1 ) == 1 && ( ( y >> 1 ) % STAT_STEP ) == 0 ) stats_rows(s, u, m, x0, ow / 2);
			t = u;
			u = m;
			m = d;
			d = t;
		}
	}
	return ( fmt == CAMFMT_YUY2 ) ? ow * oh * 2 : ow * oh * 3 / 2;
}

/* ---------------------------------------------------------------- white balance, black level, exposure */

#define WB_MIN		128		/* gains from a half */
#define WB_MAX		1024		/* to four */

EXPORT UW cam_isp_auto( T_CAMISP *s )
{
	UD	r, g, b, target, mean;
	UW	n = s->ncell, want, i, acc, lo;
	INT	c;

	if ( n == 0 ) return 256;
	r = s->sum[0] / n;
	g = s->sum[1] / n;
	b = s->sum[2] / n;

	/* the grey world: red and blue brought to the mean of green, a quarter of the way a frame */
	if ( r > 0 && b > 0 && g > 4 ) {
		UD	gr = g * 256 / r, gb = g * 256 / b;

		if ( gr < WB_MIN ) gr = WB_MIN;
		if ( gr > WB_MAX ) gr = WB_MAX;
		if ( gb < WB_MIN ) gb = WB_MIN;
		if ( gb > WB_MAX ) gb = WB_MAX;
		s->wb[0] = (UW)( ( 3 * (UD)s->wb[0] + gr ) / 4 );
		s->wb[2] = (UW)( ( 3 * (UD)s->wb[2] + gb ) / 4 );
	}
	s->wb[1] = 256;

	/* the black level: the value below which one cell in a thousand lies, when that is darker */
	for ( i = 0, acc = 0, lo = ISP_HBINS; i < ISP_HBINS; i++ ) {
		acc += s->hist[i];
		if ( acc * 1000 > n ) {
			lo = i;
			break;
		}
	}
	if ( lo < ISP_HBINS && lo * 16 < s->black ) s->black = lo * 16;
	if ( s->black > s->black_max ) s->black = s->black_max;
	cam_isp_tables(s);

	/*
	 * Exposure: the mean of green to a sixth of the range above black
	 * (with the gamma of 0.5, about 0.4 of the output's range); fewer
	 * than one cell in fifty saturated
	 */
	target = ( 1023 - s->black ) / 6;
	mean = g + 1;
	want = (UW)( target * 256 / mean );
	if ( s->nsat * 50 > n && want > 192 ) want = 192;
	if ( want < 64 ) want = 64;
	if ( want > 1024 ) want = 1024;
	c = ( 256 + (INT)want ) / 2;
	return (UW)c;
}
