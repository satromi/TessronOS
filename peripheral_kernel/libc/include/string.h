/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	string.h
 *	The part of the C library that vendored code reaches for.
 *
 *	The kernel has no C library. lwIP and FreeType are written against
 *	one, and neither is going to be changed to suit us (design 12.6,
 *	16.6): what they need is declared here with the standard names and
 *	shapes, and implemented in extension/libc/ and the ports.
 *
 *	Replaced by newlib when that is in place. Because the names are the
 *	standard ones, nothing above has to change when it is.
 */

#ifndef __TS_LIBC_STRING_H__
#define __TS_LIBC_STRING_H__

#include <stddef.h>

void	*memcpy( void *dst, const void *src, size_t n );
void	*memmove( void *dst, const void *src, size_t n );
void	*memset( void *s, int c, size_t n );
int	memcmp( const void *s1, const void *s2, size_t n );
void	*memchr( const void *s, int c, size_t n );

size_t	strlen( const char *s );
char	*strcpy( char *dst, const char *src );
char	*strncpy( char *dst, const char *src, size_t n );
int	strcmp( const char *s1, const char *s2 );
int	strncmp( const char *s1, const char *s2, size_t n );
char	*strchr( const char *s, int c );
char	*strrchr( const char *s, int c );
char	*strstr( const char *haystack, const char *needle );

#endif /* __TS_LIBC_STRING_H__ */
