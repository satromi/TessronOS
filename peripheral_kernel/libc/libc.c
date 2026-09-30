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
 *	The part of the C library that vendored code reaches for, and that
 *	nothing else in TessronOS provides.
 *
 *	The string functions here serve lwIP and FreeType alike; what only
 *	lwIP needs (errno, its random numbers) is in
 *	peripheral_kernel/network/port/lwip_libc.c. A name is implemented in
 *	one place, never both.
 *
 *	The allocator here is the kernel's. FreeType is given an allocator
 *	of its own by the font layer, so malloc is only reached by parts of
 *	it that are not built in; it is provided so that the link does not
 *	depend on which parts those are.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <stddef.h>
#include "tstdlib.h"

/* ---------------------------------------------------------------- text */

void *memchr( const void *s, int c, size_t n )
{
	CONST UB	*p = (CONST UB *)s;
	size_t		i;

	for ( i = 0; i < n; i++ ) {
		if ( p[i] == (UB)c ) {
			return (void *)(p + i);
		}
	}

	return NULL;
}

size_t strlen( const char *s )
{
	return (size_t)knl_strlen(s);
}

char *strcpy( char *dst, const char *src )
{
	return knl_strcpy(dst, src);
}

char *strncpy( char *dst, const char *src, size_t n )
{
	return knl_strncpy(dst, src, (SZ)n);
}

int strcmp( const char *s1, const char *s2 )
{
	return knl_strcmp(s1, s2);
}

int strncmp( const char *s1, const char *s2, size_t n )
{
	size_t	i;

	for ( i = 0; i < n; i++ ) {
		UB	a = (UB)s1[i], b = (UB)s2[i];

		if ( a != b ) {
			return ( a < b ) ? -1 : 1;
		}
		if ( a == 0 ) {
			break;
		}
	}

	return 0;
}

/* The terminator counts as part of the string, as the standard has it */
char *strchr( const char *s, int c )
{
	for ( ;; s++ ) {
		if ( *s == (char)c ) {
			return (char *)s;
		}
		if ( *s == '\0' ) {
			return NULL;
		}
	}
}

char *strstr( const char *haystack, const char *needle )
{
	SZ	n = knl_strlen(needle);
	SZ	i, j;

	if ( n == 0 ) {
		return (char *)haystack;
	}
	for ( i = 0; haystack[i] != '\0'; i++ ) {
		for ( j = 0; j < n && haystack[i + j] == needle[j]; j++ ) {
			;
		}
		if ( j == n ) {
			return (char *)(haystack + i);
		}
		if ( haystack[i + j] == '\0' ) {
			break;
		}
	}

	return NULL;
}

/*
 * Leading space and a sign, then digits, and nothing else: which is all
 * that is asked of it (the number at the end of an interface name).
 */
int atoi( const char *s )
{
	int	sign = 1, v = 0;

	while ( *s == ' ' || *s == '\t' ) {
		s++;
	}
	if ( *s == '-' ) {
		sign = -1;
		s++;
	} else if ( *s == '+' ) {
		s++;
	}
	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + (*s - '0');
		s++;
	}

	return v * sign;
}

char *strrchr( const char *s, int c )
{
	CONST char	*last = NULL;

	for ( ;; ) {
		if ( *s == (char)c ) {
			last = s;
		}
		if ( *s == '\0' ) {
			break;
		}
		s++;
	}

	return (char *)last;
}

long atol( const char *s )
{
	long	v = 0;
	int	neg = 0;

	while ( *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' ) s++;
	if ( *s == '-' ) { neg = 1; s++; }
	else if ( *s == '+' ) { s++; }
	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + ( *s - '0' );
		s++;
	}

	return neg ? -v : v;
}

long labs( long v )
{
	return ( v < 0 ) ? -v : v;
}

char *getenv( const char *name )
{
	(void)name;

	return NULL;			/* there is no environment to look in */
}

/* ---------------------------------------------------------------- memory */

/*
 * The size is kept in front of the block: the kernel's allocator is not
 * asked how large a block is, and realloc has to know how much to copy.
 */
#define HDR	( (SZ)sizeof(SZ) )

void *malloc( size_t n )
{
	UB	*p;

	if ( n == 0 ) {
		return NULL;
	}
	p = (UB *)Kmalloc((SZ)n + HDR);
	if ( p == NULL ) {
		return NULL;
	}
	*(SZ *)p = (SZ)n;

	return p + HDR;
}

void free( void *p )
{
	if ( p != NULL ) {
		Kfree((UB *)p - HDR);
	}
}

void *calloc( size_t n, size_t size )
{
	SZ	total = (SZ)n * (SZ)size;
	UB	*p;
	SZ	i;

	if ( n != 0 && total / (SZ)n != (SZ)size ) {
		return NULL;			/* the count times the size overflowed */
	}
	p = (UB *)malloc((size_t)total);
	if ( p == NULL ) {
		return NULL;
	}
	for ( i = 0; i < total; i++ ) {
		p[i] = 0;
	}

	return p;
}

void *realloc( void *p, size_t n )
{
	UB	*fresh;
	SZ	keep;

	if ( p == NULL ) {
		return malloc(n);
	}
	if ( n == 0 ) {
		free(p);
		return NULL;
	}
	fresh = (UB *)malloc(n);
	if ( fresh == NULL ) {
		return NULL;
	}
	keep = *(SZ *)((UB *)p - HDR);
	if ( keep > (SZ)n ) {
		keep = (SZ)n;
	}
	knl_memcpy(fresh, p, keep);
	free(p);

	return fresh;
}

/* ---------------------------------------------------------------- sorting */

LOCAL void swap_bytes( UB *a, UB *b, SZ size )
{
	SZ	i;

	for ( i = 0; i < size; i++ ) {
		UB t = a[i];

		a[i] = b[i];
		b[i] = t;
	}
}

/*
 * Insertion sort. What is sorted here is a handful of glyph indices or
 * table entries, never a large array, and a small predictable amount of
 * code is worth more in a kernel than an asymptotically better sort
 * that needs a stack of its own.
 */
void qsort( void *base, size_t n, size_t size,
	    int (*cmp)( const void *, const void * ) )
{
	UB	*a = (UB *)base;
	SZ	i, j;

	if ( a == NULL || cmp == NULL || size == 0 ) {
		return;
	}
	for ( i = 1; i < (SZ)n; i++ ) {
		for ( j = i; j > 0; j-- ) {
			UB *cur = a + j * size;
			UB *prv = cur - size;

			if ( cmp(prv, cur) <= 0 ) {
				break;
			}
			swap_bytes(prv, cur, (SZ)size);
		}
	}
}
