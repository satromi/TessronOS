/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	stdio.h
 *	What wpa_supplicant takes of <stdio.h>: formatting into a buffer (the kernel's), and nothing of files
 */

#ifndef __TSWPA_STDIO_H__
#define __TSWPA_STDIO_H__

#include <stddef.h>
#include <stdarg.h>

typedef struct __tswpa_file FILE;

int	snprintf( char *s, size_t n, const char *fmt, ... );
int	vsnprintf( char *s, size_t n, const char *fmt, va_list ap );
int	printf( const char *fmt, ... );

#endif
