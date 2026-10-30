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
 *	What wpa_supplicant takes of <string.h>: the kernel's
 */

#ifndef __TSWPA_STRING_H__
#define __TSWPA_STRING_H__

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
char	*strdup( const char *s );
int	strcasecmp( const char *s1, const char *s2 );
int	strncasecmp( const char *s1, const char *s2, size_t n );
char	*strerror( int e );

#endif
