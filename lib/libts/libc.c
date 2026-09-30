/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	libc.c
 *	The few C library functions a TessronOS program needs without a C
 *	library: the compiler itself calls memset and memcpy to clear and
 *	copy structures, so they must be there however small the program.
 */

typedef unsigned long	size_t;

void *memset( void *d, int c, size_t n )
{
	unsigned char	*p = (unsigned char *)d;

	while ( n-- > 0 ) *p++ = (unsigned char)c;
	return d;
}

void *memcpy( void *d, const void *s, size_t n )
{
	unsigned char		*p = (unsigned char *)d;
	const unsigned char	*q = (const unsigned char *)s;

	while ( n-- > 0 ) *p++ = *q++;
	return d;
}

void *memmove( void *d, const void *s, size_t n )
{
	unsigned char		*p = (unsigned char *)d;
	const unsigned char	*q = (const unsigned char *)s;

	if ( p < q ) {
		while ( n-- > 0 ) *p++ = *q++;
	} else {
		while ( n-- > 0 ) p[n] = q[n];
	}
	return d;
}

size_t strlen( const char *s )
{
	size_t	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}
