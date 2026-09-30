/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	jpeg.c
 *	Reading a JPEG into pixels
 *
 *	The sequential kind with Huffman coding and eight-bit samples:
 *	grey, or three components taken as YCbCr, with any sampling of
 *	one or two in each direction and restart intervals. A progressive
 *	or arithmetic-coded file, or one of four components, is refused
 *	with E_NOSPT.
 *
 *	Each component is decoded into a plane of its own, whole blocks
 *	and all; the colour is made at the end, a component sampled less
 *	often than the most taken at the nearest sample.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/img.h>

#define JPG_MAX_SIDE	8192

/* Where each coefficient in the order the file has them goes in the block */
LOCAL CONST UB	dezig[64 + 16] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	/* a run past the end lands here, harmlessly */
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

typedef struct {
	BOOL	ok;
	UB	val[256];
	INT	mincode[17], maxcode[18], valptr[17];
} HUFF;

typedef struct {
	INT	id, h, v, tq, td, ta;
	INT	bw, bh;			/* blocks across and down in its plane */
	UB	*plane;
	INT	pred;
} COMP;

typedef struct {
	CONST UB *p, *end;
	UW	acc;
	INT	n;
	BOOL	marker;			/* a marker reached: zeros from here */
} BITS;

typedef struct {
	UH	q[4][64];
	HUFF	dc[4], ac[4];
	COMP	c[3];
	INT	nc, w, h, hmax, vmax, mcux, mcuy, restart;
	BOOL	sof;
} JPG;

LOCAL UINT be16( CONST UB *p )
{
	return ( (UINT)p[0] << 8 ) | p[1];
}

/* ---------------------------------------------------------------- tables */

LOCAL ER huff_build( HUFF *t, CONST UB *counts, CONST UB *vals, INT nvals )
{
	INT	len, i, k = 0, code = 0;

	for ( i = 0; i < nvals && i < 256; i++ ) {
		t->val[i] = vals[i];
	}
	for ( len = 1; len <= 16; len++ ) {
		t->valptr[len] = k;
		t->mincode[len] = code;
		code += counts[len - 1];
		k += counts[len - 1];
		t->maxcode[len] = ( counts[len - 1] > 0 ) ? code - 1 : -1;
		code <<= 1;
	}
	t->maxcode[17] = 0x7FFFFFFF;
	t->ok = (BOOL)( k <= nvals );

	return t->ok ? E_OK : E_PAR;
}

/* ---------------------------------------------------------------- the coded data */

LOCAL INT get_bit( BITS *b )
{
	if ( b->n == 0 ) {
		UW	c = 0;

		if ( !b->marker && b->p < b->end ) {
			c = *b->p;
			if ( c == 0xFF ) {
				if ( b->p + 1 < b->end && b->p[1] == 0x00 ) {
					b->p += 2;		/* a stuffed 0xFF */
				} else {
					b->marker = TRUE;	/* left where it is */
					c = 0;
				}
			} else {
				b->p++;
			}
		}
		b->acc = c;
		b->n = 8;
	}
	b->n--;

	return (INT)( ( b->acc >> b->n ) & 1 );
}

LOCAL INT get_bits( BITS *b, INT s )
{
	INT	v = 0;

	while ( s-- > 0 ) {
		v = ( v << 1 ) | get_bit(b);
	}
	return v;
}

/* A value of s bits as the coding means it: the lower half negative */
LOCAL INT extend( INT v, INT s )
{
	return ( s > 0 && v < ( 1 << ( s - 1 ) ) ) ? v - ( 1 << s ) + 1 : v;
}

LOCAL INT decode( BITS *b, CONST HUFF *t )
{
	INT	code = 0, len;

	for ( len = 1; len <= 16; len++ ) {
		code = ( code << 1 ) | get_bit(b);
		if ( t->maxcode[len] >= 0 && code <= t->maxcode[len] ) {
			return t->val[t->valptr[len] + code - t->mincode[len]];
		}
	}
	return -1;
}

/* After a restart interval: the RSTn marker passed and the reading begun afresh */
LOCAL void restart( BITS *b )
{
	b->n = 0;
	if ( !b->marker ) {
		while ( b->p + 1 < b->end && !( b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7 ) ) {
			b->p++;
		}
	}
	if ( b->p + 1 < b->end && b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7 ) {
		b->p += 2;
	}
	b->marker = FALSE;
}

/* ---------------------------------------------------------------- the inverse transform */

#define C_0541	2217
#define C_1847	( -7567 )
#define C_0765	3135
#define C_1175	4816
#define C_0298	1223
#define C_2053	8410
#define C_3072	12586
#define C_1501	6149
#define C_0899	( -3685 )
#define C_2562	( -10497 )
#define C_1961	( -8034 )
#define C_0390	( -1597 )

/* One row or column: the even part and the odd part, scaled by 4096 */
#define IDCT_1D( s0, s1, s2, s3, s4, s5, s6, s7 )			\
	{								\
		INT	p1, p2, p3, p4, p5;				\
		p2 = (s2); p3 = (s6);					\
		p1 = ( p2 + p3 ) * C_0541;				\
		t2 = p1 + p3 * C_1847;					\
		t3 = p1 + p2 * C_0765;					\
		p2 = (s0); p3 = (s4);					\
		t0 = ( p2 + p3 ) * 4096;				\
		t1 = ( p2 - p3 ) * 4096;				\
		x0 = t0 + t3; x3 = t0 - t3;				\
		x1 = t1 + t2; x2 = t1 - t2;				\
		t0 = (s7); t1 = (s5); t2 = (s3); t3 = (s1);		\
		p3 = t0 + t2; p4 = t1 + t3;				\
		p1 = t0 + t3; p2 = t1 + t2;				\
		p5 = ( p3 + p4 ) * C_1175;				\
		t0 = t0 * C_0298; t1 = t1 * C_2053;			\
		t2 = t2 * C_3072; t3 = t3 * C_1501;			\
		p1 = p5 + p1 * C_0899; p2 = p5 + p2 * C_2562;		\
		p3 = p3 * C_1961; p4 = p4 * C_0390;			\
		t3 += p1 + p4; t2 += p2 + p3;				\
		t1 += p2 + p4; t0 += p1 + p3;				\
	}

LOCAL UB clamp8( INT v )
{
	return (UB)( ( v < 0 ) ? 0 : ( v > 255 ) ? 255 : v );
}

/* The coefficients of a block turned into its 8 by 8 samples */
LOCAL void idct( CONST INT *in, UB *out, INT stride )
{
	INT	tmp[64], i;
	INT	t0, t1, t2, t3, x0, x1, x2, x3;

	for ( i = 0; i < 8; i++ ) {
		CONST INT	*d = in + i;
		INT		*v = tmp + i;

		if ( d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0
		  && d[40] == 0 && d[48] == 0 && d[56] == 0 ) {
			INT	dc = d[0] * 4;

			v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
			continue;
		}
		IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56]);
		x0 += 512; x1 += 512; x2 += 512; x3 += 512;
		v[0]  = ( x0 + t3 ) >> 10;
		v[56] = ( x0 - t3 ) >> 10;
		v[8]  = ( x1 + t2 ) >> 10;
		v[48] = ( x1 - t2 ) >> 10;
		v[16] = ( x2 + t1 ) >> 10;
		v[40] = ( x2 - t1 ) >> 10;
		v[24] = ( x3 + t0 ) >> 10;
		v[32] = ( x3 - t0 ) >> 10;
	}
	for ( i = 0; i < 8; i++ ) {
		CONST INT	*d = tmp + i * 8;
		UB		*o = out + i * stride;

		IDCT_1D(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
		/* the rounding and the level shift of 128 at once */
		x0 += 65536 + ( 128 << 17 );
		x1 += 65536 + ( 128 << 17 );
		x2 += 65536 + ( 128 << 17 );
		x3 += 65536 + ( 128 << 17 );
		o[0] = clamp8(( x0 + t3 ) >> 17);
		o[7] = clamp8(( x0 - t3 ) >> 17);
		o[1] = clamp8(( x1 + t2 ) >> 17);
		o[6] = clamp8(( x1 - t2 ) >> 17);
		o[2] = clamp8(( x2 + t1 ) >> 17);
		o[5] = clamp8(( x2 - t1 ) >> 17);
		o[3] = clamp8(( x3 + t0 ) >> 17);
		o[4] = clamp8(( x3 - t0 ) >> 17);
	}
}

/* ---------------------------------------------------------------- the file */

LOCAL ER read_sof( JPG *j, CONST UB *s, INT len )
{
	INT	i;

	if ( len < 6 || s[0] != 8 ) {
		return E_NOSPT;			/* not eight bits a sample */
	}
	j->h = (INT)be16(s + 1);
	j->w = (INT)be16(s + 3);
	j->nc = s[5];
	if ( j->w <= 0 || j->h <= 0 || j->w > JPG_MAX_SIDE || j->h > JPG_MAX_SIDE ) {
		return E_NOSPT;
	}
	if ( ( j->nc != 1 && j->nc != 3 ) || len < 6 + j->nc * 3 ) {
		return E_NOSPT;
	}
	j->hmax = j->vmax = 1;
	for ( i = 0; i < j->nc; i++ ) {
		COMP	*c = &j->c[i];

		c->id = s[6 + i * 3];
		c->h = s[7 + i * 3] >> 4;
		c->v = s[7 + i * 3] & 15;
		c->tq = s[8 + i * 3] & 3;
		if ( c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2 ) {
			return E_NOSPT;
		}
		if ( c->h > j->hmax ) j->hmax = c->h;
		if ( c->v > j->vmax ) j->vmax = c->v;
	}
	j->mcux = ( j->w + j->hmax * 8 - 1 ) / ( j->hmax * 8 );
	j->mcuy = ( j->h + j->vmax * 8 - 1 ) / ( j->vmax * 8 );
	j->sof = TRUE;

	return E_OK;
}

LOCAL ER read_dqt( JPG *j, CONST UB *s, INT len )
{
	INT	at = 0, i;

	while ( at < len ) {
		INT	pq = s[at] >> 4, tq = s[at] & 3;

		at++;
		if ( at + 64 * ( pq + 1 ) > len ) {
			return E_PAR;
		}
		for ( i = 0; i < 64; i++ ) {
			j->q[tq][i] = (UH)( pq ? be16(s + at + i * 2) : s[at + i] );
		}
		at += 64 * ( pq + 1 );
	}
	return E_OK;
}

LOCAL ER read_dht( JPG *j, CONST UB *s, INT len )
{
	INT	at = 0, i, n;

	while ( at + 17 <= len ) {
		INT	tc = s[at] >> 4, th = s[at] & 3;
		CONST UB *counts = s + at + 1;

		for ( n = 0, i = 0; i < 16; i++ ) {
			n += counts[i];
		}
		if ( n > 256 || at + 17 + n > len ) {
			return E_PAR;
		}
		if ( huff_build(( tc == 0 ) ? &j->dc[th] : &j->ac[th], counts, s + at + 17, n) < E_OK ) {
			return E_PAR;
		}
		at += 17 + n;
	}
	return E_OK;
}

/* One block of a component decoded into its plane at block (bx, by) */
LOCAL ER block( JPG *j, BITS *b, COMP *c, INT bx, INT by )
{
	INT	coef[64], k, t;
	CONST UH *q = j->q[c->tq];

	knl_memset(coef, 0, sizeof(coef));
	t = decode(b, &j->dc[c->td]);
	if ( t < 0 || t > 16 ) {
		return E_PAR;
	}
	c->pred += ( t > 0 ) ? extend(get_bits(b, t), t) : 0;
	coef[0] = c->pred * q[0];
	for ( k = 1; k < 64; ) {
		INT	rs = decode(b, &j->ac[c->ta]), r, s;

		if ( rs < 0 ) {
			return E_PAR;
		}
		r = rs >> 4;
		s = rs & 15;
		if ( s == 0 ) {
			if ( r != 15 ) {
				break;			/* the rest are zero */
			}
			k += 16;
			continue;
		}
		k += r;
		if ( k > 63 ) {
			return E_PAR;
		}
		coef[dezig[k]] = extend(get_bits(b, s), s) * q[k];
		k++;
	}
	if ( bx < c->bw && by < c->bh ) {
		idct(coef, c->plane + (SZ)by * 8 * c->bw * 8 + bx * 8, c->bw * 8);
	}
	return E_OK;
}

LOCAL ER scan( JPG *j, CONST UB *s, INT len, CONST UB *data, CONST UB *end, CONST UB **p_next )
{
	INT	ns, i, k, mx, my, left;
	COMP	*sc[3];
	BITS	b;
	ER	er = E_OK;

	if ( !j->sof || len < 1 ) {
		return E_PAR;
	}
	ns = s[0];
	if ( ns < 1 || ns > j->nc || len < 1 + ns * 2 ) {
		return E_PAR;
	}
	for ( i = 0; i < ns; i++ ) {
		sc[i] = NULL;
		for ( k = 0; k < j->nc; k++ ) {
			if ( j->c[k].id == s[1 + i * 2] ) {
				sc[i] = &j->c[k];
			}
		}
		if ( sc[i] == NULL ) {
			return E_PAR;
		}
		sc[i]->td = s[2 + i * 2] >> 4 & 3;
		sc[i]->ta = s[2 + i * 2] & 3;
		if ( !j->dc[sc[i]->td].ok || !j->ac[sc[i]->ta].ok ) {
			return E_PAR;
		}
		sc[i]->pred = 0;
	}
	b.p = data;
	b.end = end;
	b.n = 0;
	b.acc = 0;
	b.marker = FALSE;
	left = j->restart;
	if ( ns == 1 ) {
		/* one component alone: its blocks in order, as many as cover the picture */
		COMP	*c = sc[0];
		INT	bw = ( j->w * c->h / j->hmax + 7 ) / 8, bh = ( j->h * c->v / j->vmax + 7 ) / 8;

		for ( my = 0; my < bh && er >= E_OK; my++ ) {
			for ( mx = 0; mx < bw && er >= E_OK; mx++ ) {
				if ( j->restart > 0 && left-- == 0 ) {
					restart(&b);
					c->pred = 0;
					left = j->restart - 1;
				}
				er = block(j, &b, c, mx, my);
			}
		}
	} else {
		for ( my = 0; my < j->mcuy && er >= E_OK; my++ ) {
			for ( mx = 0; mx < j->mcux && er >= E_OK; mx++ ) {
				if ( j->restart > 0 && left-- == 0 ) {
					restart(&b);
					for ( i = 0; i < ns; i++ ) sc[i]->pred = 0;
					left = j->restart - 1;
				}
				for ( i = 0; i < ns && er >= E_OK; i++ ) {
					INT	u, v;

					for ( v = 0; v < sc[i]->v && er >= E_OK; v++ ) {
						for ( u = 0; u < sc[i]->h && er >= E_OK; u++ ) {
							er = block(j, &b, sc[i], mx * sc[i]->h + u, my * sc[i]->v + v);
						}
					}
				}
			}
		}
	}
	/* on to the next marker */
	while ( b.p + 1 < end && !( b.p[0] == 0xFF && b.p[1] != 0x00 && !( b.p[1] >= 0xD0 && b.p[1] <= 0xD7 ) ) ) {
		b.p++;
	}
	*p_next = b.p;

	return er;
}

LOCAL ER planes( JPG *j )
{
	INT	i;

	for ( i = 0; i < j->nc; i++ ) {
		COMP	*c = &j->c[i];

		c->bw = j->mcux * c->h;
		c->bh = j->mcuy * c->v;
		c->plane = (UB *)Kmalloc((SZ)c->bw * 8 * c->bh * 8);
		if ( c->plane == NULL ) {
			return E_NOMEM;
		}
		knl_memset(c->plane, 128, (INT)( (SZ)c->bw * 8 * c->bh * 8 ));
	}
	return E_OK;
}

LOCAL void planes_free( JPG *j )
{
	INT	i;

	for ( i = 0; i < 3; i++ ) {
		if ( j->c[i].plane != NULL ) {
			Kfree(j->c[i].plane);
			j->c[i].plane = NULL;
		}
	}
}

/*
 * The markers walked: the tables taken, and with `decode_too` the scans
 * decoded. Without it the walk stops at the frame's head.
 */
LOCAL ER walk( JPG *j, CONST UB *data, SZ size, BOOL decode_too )
{
	CONST UB	*p = data + 2, *end = data + size;
	ER		er;

	while ( p + 4 <= end ) {
		UINT	m;
		INT	len;

		if ( p[0] != 0xFF ) {
			p++;
			continue;
		}
		m = p[1];
		if ( m == 0xFF ) {
			p++;
			continue;
		}
		if ( m == 0xD9 ) {
			break;				/* the end */
		}
		if ( m == 0x01 || ( m >= 0xD0 && m <= 0xD7 ) ) {
			p += 2;
			continue;
		}
		len = (INT)be16(p + 2);
		if ( len < 2 || p + 2 + len > end ) {
			return E_PAR;
		}
		switch ( m ) {
		case 0xC0: case 0xC1:
			er = read_sof(j, p + 4, len - 2);
			if ( er < E_OK ) return er;
			if ( !decode_too ) return E_OK;
			er = planes(j);
			if ( er < E_OK ) return er;
			break;
		case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
		case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
			return E_NOSPT;			/* progressive, lossless or arithmetic */
		case 0xC4:
			if ( read_dht(j, p + 4, len - 2) < E_OK ) return E_PAR;
			break;
		case 0xDB:
			if ( read_dqt(j, p + 4, len - 2) < E_OK ) return E_PAR;
			break;
		case 0xDD:
			if ( len >= 4 ) j->restart = (INT)be16(p + 4);
			break;
		case 0xDA: {
			CONST UB	*next;

			er = scan(j, p + 4, len - 2, p + 2 + len, end, &next);
			if ( er < E_OK ) return er;
			p = next;
			continue;
		}
		default:
			break;
		}
		p += 2 + len;
	}
	return j->sof ? E_OK : E_PAR;
}

LOCAL BOOL is_jpeg( CONST UB *data, SZ size )
{
	return (BOOL)( size >= 4 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF );
}

EXPORT ER img_jpeg_size( CONST UB *data, SZ size, INT *p_w, INT *p_h )
{
	JPG	*j;
	ER	er;

	if ( data == NULL || !is_jpeg(data, size) ) {
		return E_PAR;
	}
	j = (JPG *)Kmalloc(sizeof(JPG));
	if ( j == NULL ) {
		return E_NOMEM;
	}
	knl_memset(j, 0, sizeof(JPG));
	er = walk(j, data, size, FALSE);
	if ( er >= E_OK ) {
		if ( p_w != NULL ) *p_w = j->w;
		if ( p_h != NULL ) *p_h = j->h;
	}
	Kfree(j);
	return er;
}

EXPORT ER img_jpeg_decode( CONST UB *data, SZ size, UW **p_pixels, INT *p_w, INT *p_h )
{
	JPG	*j;
	UW	*px;
	INT	x, y;
	ER	er;

	if ( data == NULL || p_pixels == NULL || !is_jpeg(data, size) ) {
		return E_PAR;
	}
	j = (JPG *)Kmalloc(sizeof(JPG));
	if ( j == NULL ) {
		return E_NOMEM;
	}
	knl_memset(j, 0, sizeof(JPG));
	er = walk(j, data, size, TRUE);
	if ( er < E_OK ) {
		planes_free(j);
		Kfree(j);
		return er;
	}
	px = (UW *)Kmalloc((SZ)j->w * j->h * sizeof(UW));
	if ( px == NULL ) {
		planes_free(j);
		Kfree(j);
		return E_NOMEM;
	}
	for ( y = 0; y < j->h; y++ ) {
		for ( x = 0; x < j->w; x++ ) {
			INT	s[3], i;

			for ( i = 0; i < j->nc; i++ ) {
				COMP	*c = &j->c[i];
				INT	cx = x * c->h / j->hmax, cy = y * c->v / j->vmax;

				s[i] = c->plane[(SZ)cy * c->bw * 8 + cx];
			}
			if ( j->nc == 1 ) {
				px[(SZ)y * j->w + x] = ( (UW)s[0] << 16 ) | ( (UW)s[0] << 8 ) | (UW)s[0];
			} else {
				INT	cb = s[1] - 128, cr = s[2] - 128;
				INT	r = s[0] + ( ( 91881 * cr + 32768 ) >> 16 );
				INT	g = s[0] - ( ( 22553 * cb + 46802 * cr - 32768 ) >> 16 );
				INT	b = s[0] + ( ( 116130 * cb + 32768 ) >> 16 );

				px[(SZ)y * j->w + x] = ( (UW)clamp8(r) << 16 ) | ( (UW)clamp8(g) << 8 ) | clamp8(b);
			}
		}
	}
	*p_pixels = px;
	if ( p_w != NULL ) *p_w = j->w;
	if ( p_h != NULL ) *p_h = j->h;
	planes_free(j);
	Kfree(j);
	return E_OK;
}
