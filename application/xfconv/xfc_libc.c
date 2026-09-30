/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_libc.c
 *	What the C library (newlib) needs from the system
 *
 *	Memory comes from _sbrk, which hands out a fixed area of the
 *	program's own: the xmlTAD of an object, a picture being turned into
 *	a PNG and the listing of a directory are all there. Output to the
 *	standard streams goes to the console; files are reached through the
 *	fs_ calls themselves, not through stdio.
 */

#include "xfc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define ARENA_SIZE	( 32 * 1024 * 1024 )

static char	arena[ARENA_SIZE] __attribute__((aligned(16)));
static size_t	arena_used;

void *_sbrk( ptrdiff_t incr )
{
	char	*p;

	if ( incr < 0 ? (size_t)( -incr ) > arena_used : arena_used + (size_t)incr > ARENA_SIZE ) {
		errno = ENOMEM;
		return (void *)-1;
	}
	p = arena + arena_used;
	arena_used += (size_t)incr;
	return p;
}

int _write( int fd, const void *buf, size_t n )
{
	char	tmp[128];
	size_t	k, done = 0;

	if ( fd != 1 && fd != 2 ) {
		errno = EBADF;
		return -1;
	}
	while ( done < n ) {
		k = n - done;
		if ( k > sizeof(tmp) - 1 ) k = sizeof(tmp) - 1;
		memcpy(tmp, (const char *)buf + done, k);
		tmp[k] = 0;
		tm_putstring((const UB *)tmp);
		done += k;
	}
	return (int)n;
}

int _read( int fd, void *buf, size_t n )
{
	(void)fd; (void)buf; (void)n;
	return 0;
}

int _close( int fd )
{
	(void)fd;
	return -1;
}

int _fstat( int fd, struct stat *st )
{
	(void)fd;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR;
	return 0;
}

int _isatty( int fd )
{
	return ( fd <= 2 );
}

off_t _lseek( int fd, off_t off, int whence )
{
	(void)fd; (void)off; (void)whence;
	return 0;
}

int _open( const char *path, int flags, ... )
{
	(void)path; (void)flags;
	errno = ENOENT;
	return -1;
}

void _exit( int code )
{
	ts_ext_prc(code);
	for ( ;; ) ;
}

int _kill( int pid, int sig )
{
	(void)pid; (void)sig;
	errno = EINVAL;
	return -1;
}

int _getpid( void )
{
	return (int)ts_get_pid();
}
