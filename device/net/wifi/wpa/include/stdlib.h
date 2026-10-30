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
 *	What wpa_supplicant takes of <stdlib.h>: the kernel's C library (peripheral_kernel/libc)
 */

#ifndef __TSWPA_STDLIB_H__
#define __TSWPA_STDLIB_H__

#include <stddef.h>

void	*malloc( size_t n );
void	*calloc( size_t n, size_t size );
void	*realloc( void *p, size_t n );
void	free( void *p );
int	atoi( const char *s );
long	atol( const char *s );
unsigned long strtoul( const char *s, char **end, int base );
long	strtol( const char *s, char **end, int base );
void	qsort( void *base, size_t n, size_t size, int (*cmp)( const void *, const void * ) );
void	abort( void );

/* the kernel's C library has labs only; SAE's dragonfly.c asks abs */
#define abs(x)	__builtin_abs(x)

#endif
