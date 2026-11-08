/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lc3_fmath.c
 *	The few functions of floating point the LC3 codec (lib/liblc3) calls
 *	that the compiler does not make into instructions on every processor:
 *	on x86-64 fmaxf, fminf and roundf stay calls, and the kernel links no
 *	library of mathematics. Built like the codec, with the floating point
 *	registers, and called only from the task that encodes (bt_lea.c).
 */

float fmaxf( float a, float b );
float fminf( float a, float b );
float roundf( float x );

/* The larger; a NaN gives way to the other */
float fmaxf( float a, float b )
{
	if ( a != a ) return b;
	if ( b != b ) return a;
	return ( a > b ) ? a : b;
}

float fminf( float a, float b )
{
	if ( a != a ) return b;
	if ( b != b ) return a;
	return ( a < b ) ? a : b;
}

/*
 * To the nearest whole number, a half away from zero. What truncation
 * leaves, x - t, is exact: below 2^23 it fits the mantissa, and from there
 * on x has no fraction.
 */
float roundf( float x )
{
	float	t = __builtin_truncf(x);
	float	r = x - t;

	if ( r >= 0.5f ) t += 1.0f;
	else if ( r <= -0.5f ) t -= 1.0f;
	return t;
}
