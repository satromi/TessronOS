/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	compose_simd.c (x86-64)
 *	The inner loops of the stages (compose.c), as outer_kernel/wm/
 *	compose_simd.c has them for the vector unit of AArch64: the same sums
 *	in the same 16 bit lanes, rounded the same way, so that every one
 *	writes what the plain loop in compose.c writes, to the bit.
 *
 *	Copying, laying and darkening take four pixels at a time in the SSE
 *	registers (x86-64-v2 has them all), or eight in the AVX2 ones where
 *	the processor has those (use_avx2, asked once); the spread
 *	(halving, smoothing, doubling) goes a channel at a time. They run
 *	only in the tasks the compositor names (cp_simd_allow), which keep
 *	the SIMD registers of their own (TA_FPU): this file alone of the
 *	kernel is translated with the SIMD registers allowed (x64.mk).
 *
 *	The loops are written with the compiler's vector types, without the
 *	intrinsics' headers, which want a C library the kernel has not.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "../compose.h"

typedef UB	v16b __attribute__((vector_size(16)));
typedef UH	v16h __attribute__((vector_size(32)));
typedef UW	v4w  __attribute__((vector_size(16), aligned(4), may_alias));
typedef UB	v32b __attribute__((vector_size(32)));
typedef UH	v32h __attribute__((vector_size(64)));
typedef UW	v8w  __attribute__((vector_size(32), aligned(4), may_alias));
typedef INT	v4i  __attribute__((vector_size(16)));
typedef INT	v8i  __attribute__((vector_size(32)));

/* Whether the processor has AVX2 and the system keeps its registers (XCR0) */
LOCAL INT	cps_avx2 = -1;

LOCAL BOOL use_avx2( void )
{
	if ( cps_avx2 < 0 ) {
		UW	a, b, c, d;
		IMPORT UD	knl_xcr0;

		cpuid(7, 0, &a, &b, &c, &d);
		cps_avx2 = ( ( b & ( 1U << 5 ) ) != 0 && ( knl_xcr0 & 0x6 ) == 0x6 ) ? 1 : 0;
	}
	return cps_avx2 > 0;
}

/* ---------------------------------------------------------------- copying */

__attribute__((target("avx2")))
LOCAL void copy8( UW *d, CONST UW *s, INT n )
{
	INT	i = 0;

	for ( ; i + 8 <= n; i += 8 ) {
		*(v8w *)( d + i ) = *(CONST v8w *)( s + i );
	}
	for ( ; i < n; i++ ) {
		d[i] = s[i];
	}
}

EXPORT void cps_copy( UW *d, CONST UW *s, INT n )
{
	INT	i = 0;

	if ( use_avx2() ) {
		copy8(d, s, n);
		return;
	}
	for ( ; i + 4 <= n; i += 4 ) {
		*(v4w *)( d + i ) = *(CONST v4w *)( s + i );
	}
	for ( ; i < n; i++ ) {
		d[i] = s[i];
	}
}

/* ---------------------------------------------------------------- mixing */

/* The same for one pixel, for what is left over at the end of a row */
LOCAL UW over1( UW under, UW over, UINT a )
{
	UINT	b = 255U - a;
	UW	r, g, bl;

	r  = ( ( ( over >> 16 ) & 0xFFU ) * a + ( ( under >> 16 ) & 0xFFU ) * b + 127U ) / 255U;
	g  = ( ( ( over >> 8 ) & 0xFFU ) * a + ( ( under >> 8 ) & 0xFFU ) * b + 127U ) / 255U;
	bl = ( ( over & 0xFFU ) * a + ( under & 0xFFU ) * b + 127U ) / 255U;

	return ( r << 16 ) | ( g << 8 ) | bl;
}

/*
 * Pixels of 'over' laid at a over those of 'under' (b = 255 - a), channel
 * by channel in 16 bit lanes, rounded: t / 255 is (t + 1 + (t >> 8)) >> 8
 * for every t below 65536 that these sums reach. The top byte comes out 0.
 */
#define OVER_BODY(VB, VH, VW, N)							\
	VH	t = __builtin_convertvector((VB)over, VH) * a			\
		  + __builtin_convertvector((VB)under, VH) * b + 127;		\
	VW	r;									\
										\
	t = ( t + 1 + ( t >> 8 ) ) >> 8;					\
	r = (VW)__builtin_convertvector(t, VB);					\
	return r & 0x00FFFFFFU;

__attribute__((target("avx2")))
static inline v8w over8( v8w under, v8w over, UH a, UH b )
{
	OVER_BODY(v32b, v32h, v8w, 32)
}

static inline v4w over4( v4w under, v4w over, UH a, UH b )
{
	OVER_BODY(v16b, v16h, v4w, 16)
}

/*
 * A run of n pixels of a window laid where nothing but its own solidity
 * a varies across it: each pixel tinted by tstr, washed by wstr, and
 * mixed at a over what shows through (und, or what is there when it is
 * NULL); with 'keyed' a pixel with 0xFF on top is not laid.
 */
#define LAY_BODY(VW, VI, OVER, N)							\
	for ( ; i + N <= n; i += N ) {							\
		VW	s = *(CONST VW *)( src + i ), was = *(CONST VW *)( dst + i );	\
		VW	c = s, out;							\
										\
		if ( tstr != 0 ) {							\
			c = OVER(c, (VW){} + tint, (UH)tstr, (UH)( 255U - tstr ));	\
		}									\
		if ( wstr != 0 ) {							\
			c = OVER(c, (VW){} + wcol, (UH)wstr, (UH)( 255U - wstr ));	\
		}									\
		if ( a >= 255U ) {							\
			out = c;							\
		} else {								\
			out = OVER(( und != NULL ) ? *(CONST VW *)( und + i ) : was,	\
				   c, (UH)a, (UH)( 255U - a ));				\
		}									\
		if ( keyed ) {								\
			VW	m = (VW)( ( s >> 24 ) == 0xFFU );			\
										\
			out = ( was & m ) | ( out & ~m );				\
		}									\
		*(VW *)( dst + i ) = out;						\
	}

__attribute__((target("avx2")))
LOCAL INT lay8( UW *dst, CONST UW *src, CONST UW *und, INT n, UINT a, BOOL keyed,
		UW tint, UINT tstr, UW wcol, UINT wstr )
{
	INT	i = 0;

	LAY_BODY(v8w, v8i, over8, 8)
	return i;
}

EXPORT void cps_lay( UW *dst, CONST UW *src, CONST UW *und, INT n, UINT a, BOOL keyed,
		     UW tint, UINT tstr, UW wcol, UINT wstr )
{
	INT	i = 0;

	if ( a > 255U ) {
		a = 255U;
	}
	if ( use_avx2() ) {
		i = lay8(dst, src, und, n, a, keyed, tint, tstr, wcol, wstr);
	} else {
		LAY_BODY(v4w, v4i, over4, 4)
	}
	for ( ; i < n; i++ ) {
		UW	c = src[i];

		if ( keyed && ( c >> 24 ) == 0xFF ) {
			continue;
		}
		if ( tstr != 0 ) {
			c = over1(c, tint, tstr);
		}
		if ( wstr != 0 ) {
			c = over1(c, wcol, wstr);
		}
		dst[i] = ( a >= 255 ) ? c : over1(( und != NULL ) ? und[i] : dst[i], c, a);
	}
}

/*
 * A run of n pixels taken toward black, each by its own a of 255 (a 0
 * leaves the pixel as it is, top byte and all; any other clears it).
 */
#define DARKEN_BODY(VB, VH, VW, N, IDX)							\
	for ( ; i + N <= n; i += N ) {							\
		VW	c = *(CONST VW *)( row + i ), m, nw;				\
		VW	av;								\
		VH	k, t;								\
		INT	j;								\
										\
		for ( j = 0; j < N; j++ ) {						\
			av[j] = a[i + j];						\
		}									\
		/* each pixel's 255 - a in its four lanes */				\
		k = __builtin_convertvector((VB)( ( av * 0x01010101U ) ^ 0xFFFFFFFFU ), VH); \
		t = __builtin_convertvector((VB)c, VH) * k + 127;			\
		t = ( t + 1 + ( t >> 8 ) ) >> 8;					\
		nw = (VW)__builtin_convertvector(t, VB) & 0x00FFFFFFU;		\
		m = (VW)( av != 0 );							\
		*(VW *)( row + i ) = ( nw & m ) | ( c & ~m );				\
	}

__attribute__((target("avx2")))
LOCAL INT darken8( UW *row, CONST UB *a, INT n )
{
	INT	i = 0;

	DARKEN_BODY(v32b, v32h, v8w, 8, 0)
	return i;
}

EXPORT void cps_darken( UW *row, CONST UB *a, INT n )
{
	INT	i = 0;

	if ( use_avx2() ) {
		i = darken8(row, a, n);
	} else {
		DARKEN_BODY(v16b, v16h, v4w, 4, 0)
	}
	for ( ; i < n; i++ ) {
		if ( a[i] != 0 ) {
			UW	c = row[i];
			UINT	k = 255U - a[i];

			row[i] = ( ( ( ( ( c >> 16 ) & 0xFFU ) * k + 127U ) / 255U ) << 16 )
				 | ( ( ( ( ( c >> 8 ) & 0xFFU ) * k + 127U ) / 255U ) << 8 )
				 | ( ( ( c & 0xFFU ) * k + 127U ) / 255U );
		}
	}
}

/* ---------------------------------------------------------------- the spread */

/*
 * A pixel is four 16 bit lanes, blue first, the top lane 0, held in one
 * vector of four (v4h); the sums wrap at sixteen bits as the AArch64
 * lanes do.
 */
typedef UH	v4h  __attribute__((vector_size(8), aligned(2), may_alias));
typedef UB	v4b  __attribute__((vector_size(4)));
typedef UW	v4u  __attribute__((vector_size(16)));

LOCAL INT within( INT v, INT n )
{
	return ( v < 0 ) ? 0 : ( v >= n ) ? n - 1 : v;
}

static inline v4h px16( UW v )
{
	v4b	b = (v4b)( v & 0x00FFFFFFU );

	return __builtin_convertvector(b, v4h);
}

#define LD(p)		( *(CONST v4h *)(p) )
#define ST(p, v)	( *(v4h *)(p) = (v) )

/*
 * Row y of a level as four lanes a pixel, from x = lo (0 or less) to hi
 * counted from its left edge, past its edges the pixel at the edge
 * again: pixel x lands at row + 4 (x - lo).
 */
LOCAL void expand( CONST T_CPLEVEL *s, INT y, INT lo, INT hi, UH *row )
{
	CONST UW	*p = s->p + (SZ)y * s->pitch;
	INT		n = s->r.right - s->r.left, x;
	UH		*e = row - (SZ)lo * 4;

	for ( x = lo; x <= hi; x++ ) {
		ST(e + (SZ)x * 4, px16(p[within(x, n)]));
	}
}

/* Four lanes, each rounded down by 2^s and cut to eight bits, as one pixel */
static inline UW put1( v4h v, INT s )
{
	v4u	w = ( __builtin_convertvector(v, v4u) + ( 1U << ( s - 1 ) ) ) >> s;

	return (UW)__builtin_convertvector(__builtin_convertvector(w, v4h), v4b);
}

EXPORT void cps_halve( CONST T_CPLEVEL *s, CONST T_DPRECT *d, UW *out, UH *row, UH *mid )
{
	INT	w = d->right - d->left, sh = s->r.bottom - s->r.top;
	INT	lo = 2 * d->left - 1 - s->r.left, hi = 2 * d->right - s->r.left;
	INT	y, i;

	/* along the rows: of four pixels 1 3 3 1 */
	for ( y = 0; y < sh; y++ ) {
		UH	*m = mid + (SZ)y * w * 4;

		expand(s, y, lo, hi, row);
		for ( i = 0; i < w; i++ ) {
			CONST UH	*q = row + (SZ)i * 8;

			ST(m + (SZ)i * 4, LD(q) + LD(q + 12) + ( LD(q + 4) + LD(q + 8) ) * 3);
		}
	}

	/* down the columns, the same, rounded to eight bits */
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		t = 2 * y - 1 - s->r.top;
		CONST UH	*m0 = mid + (SZ)within(t, sh) * w * 4, *m1 = mid + (SZ)within(t + 1, sh) * w * 4;
		CONST UH	*m2 = mid + (SZ)within(t + 2, sh) * w * 4, *m3 = mid + (SZ)within(t + 3, sh) * w * 4;
		UW		*o = out + (SZ)( y - d->top ) * w;

		for ( i = 0; i < w; i++ ) {
			SZ	j = (SZ)i * 4;

			o[i] = put1(LD(m0 + j) + LD(m3 + j) + ( LD(m1 + j) + LD(m2 + j) ) * 3, 6);
		}
	}
}

EXPORT void cps_smooth( CONST T_CPLEVEL *s, CONST T_DPRECT *d, CONST INT *wt, UW *out,
			UH *row, UH *mid )
{
	INT		w = d->right - d->left, sh = s->r.bottom - s->r.top;
	INT		taps = ( wt[0] != 0 ) ? 2 : 1;
	INT		lo = d->left - taps - s->r.left, hi = d->right - 1 + taps - s->r.left;
	UH		a = (UH)wt[0], b = (UH)wt[1], c = (UH)wt[2];
	INT		y, i;

	/* along the rows: a b m b a */
	for ( y = 0; y < sh; y++ ) {
		UH		*m = mid + (SZ)y * w * 4;
		CONST UH	*q = row + (SZ)taps * 4;	/* the pixel at d->left */

		expand(s, y, lo, hi, row);
		for ( i = 0; i < w; i++ ) {
			CONST UH	*p = q + (SZ)i * 4;
			v4h		v = LD(p) * c + ( LD(p - 4) + LD(p + 4) ) * b;

			if ( taps == 2 ) {
				v += ( LD(p - 8) + LD(p + 8) ) * a;
			}
			ST(m + (SZ)i * 4, v);
		}
	}

	/* down the columns: the sums reach past sixteen bits, so in 32 */
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		t = y - s->r.top;
		CONST UH	*m0 = mid + (SZ)within(t - 2, sh) * w * 4, *m1 = mid + (SZ)within(t - 1, sh) * w * 4;
		CONST UH	*m2 = mid + (SZ)within(t, sh) * w * 4;
		CONST UH	*m3 = mid + (SZ)within(t + 1, sh) * w * 4, *m4 = mid + (SZ)within(t + 2, sh) * w * 4;
		UW		*o = out + (SZ)( y - d->top ) * w;

		for ( i = 0; i < w; i++ ) {
			SZ	j = (SZ)i * 4;
			v4u	v = __builtin_convertvector(LD(m2 + j), v4u) * c
			  + __builtin_convertvector(LD(m1 + j) + LD(m3 + j), v4u) * b;
			v4h	r;

			if ( taps == 2 ) {
				v += __builtin_convertvector(LD(m0 + j) + LD(m4 + j), v4u) * a;
			}
			r = __builtin_convertvector(( v + ( 1U << 11 ) ) >> 12, v4h);
			o[i] = (UW)__builtin_convertvector(r, v4b);
		}
	}
}

EXPORT void cps_double( CONST T_CPLEVEL *s, CONST T_DPRECT *d, UW *out, UH *row, UH *mid )
{
	INT	w = d->right - d->left, sh = s->r.bottom - s->r.top;
	INT	lo = ( d->left >> 1 ) - 1 - s->r.left, hi = ( ( d->right - 1 ) >> 1 ) + 1 - s->r.left;
	INT	y, i, x;

	/*
	 * Along the rows: a pixel from the two of the level above nearest
	 * its centre, 3 and 1; an even pixel takes the one to its left as
	 * the 1, an odd one the one to its right.
	 */
	for ( y = 0; y < sh; y++ ) {
		UH	*m = mid + (SZ)y * w * 4;

		expand(s, y, lo, hi, row);
		for ( i = 0, x = d->left; i < w; i++, x++ ) {
			CONST UH	*q = row + (SZ)( ( x >> 1 ) - s->r.left - lo ) * 4;
			CONST UH	*o = ( ( x & 1 ) != 0 ) ? q + 4 : q - 4;

			ST(m + (SZ)i * 4, LD(o) + LD(q) * 3);
		}
	}

	/* down the columns, the same */
	for ( y = d->top; y < d->bottom; y++ ) {
		INT		c = ( y >> 1 ) - s->r.top;
		CONST UH	*ma, *mb;
		UW		*o = out + (SZ)( y - d->top ) * w;
		BOOL		odd = (BOOL)( ( y & 1 ) != 0 );

		ma = mid + (SZ)within(odd ? c : c - 1, sh) * w * 4;
		mb = mid + (SZ)within(odd ? c + 1 : c, sh) * w * 4;
		for ( i = 0; i < w; i++ ) {
			SZ	j = (SZ)i * 4;

			o[i] = put1(odd ? LD(mb + j) + LD(ma + j) * 3 : LD(ma + j) + LD(mb + j) * 3, 4);
		}
	}
}
