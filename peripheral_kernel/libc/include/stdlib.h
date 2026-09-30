/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	stdlib.h
 *	The part of the C library that vendored code reaches for.
 *
 *	lwIP wants one function from here (an interface name carries a
 *	number at the end). FreeType wants sorting, a few conversions, and
 *	the allocator -- although the font layer gives it an allocator of
 *	its own, so malloc is only ever reached by parts that are not built
 *	in (design 16.6).
 */

#ifndef __TS_LIBC_STDLIB_H__
#define __TS_LIBC_STDLIB_H__

#include <stddef.h>

int	atoi( const char *s );
long	atol( const char *s );
long	labs( long v );

void	qsort( void *base, size_t n, size_t size,
	       int (*cmp)( const void *, const void * ) );

void	*malloc( size_t n );
void	*calloc( size_t n, size_t size );
void	*realloc( void *p, size_t n );
void	free( void *p );

char	*getenv( const char *name );

#endif /* __TS_LIBC_STDLIB_H__ */
