/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	memfunc.c
 *	Standard memory functions the compiler may call implicitly
 *	(structure copies, array initialization). The kernel itself uses
 *	knl_memcpy() etc. from tstdlib.
 */

#include <tk/tkernel.h>
#include "tstdlib.h"

void *memcpy( void *dst, const void *src, size_t n )
{
	return knl_memcpy(dst, src, (SZ)n);
}

void *memmove( void *dst, const void *src, size_t n )
{
	UB		*d = dst;
	const UB	*s = src;

	if ( d == s || n == 0 ) return dst;
	if ( d < s || d >= s + n ) {
		return knl_memcpy(dst, src, (SZ)n);
	}
	d += n;
	s += n;
	while ( n-- > 0 ) {
		*--d = *--s;
	}
	return dst;
}

void *memset( void *dst, int c, size_t n )
{
	return knl_memset(dst, c, (SZ)n);
}

int memcmp( const void *a, const void *b, size_t n )
{
	const UB	*p = a;
	const UB	*q = b;

	while ( n-- > 0 ) {
		if ( *p != *q ) {
			return (int)*p - (int)*q;
		}
		p++;
		q++;
	}
	return 0;
}
