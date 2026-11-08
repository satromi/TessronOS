/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ui_libc.c
 *	What the C library (newlib) needs from the system, for the programs
 *	drawn with lib/libui
 *
 *	Memory comes from _sbrk: one reservation of addresses made on first
 *	use, whose pages are made as the break goes up, so that a program
 *	costs what it keeps and is limited only by the memory there is. Output to the
 *	standard streams goes to the console.
 */

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/umem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define HEAP_MAX	( 4ULL * 1024 * 1024 * 1024 )
#define HEAP_STEP	( 256 * 1024 )

static char	*heap_base, *heap_brk, *heap_made;

void *_sbrk( ptrdiff_t incr )
{
	char	*old;

	if ( heap_base == NULL ) {
		void	*p = NULL;

		if ( ts_map_mem(&p, (SZ)HEAP_MAX, 0, TS_MEM_NONE) < E_OK ) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap_base = heap_brk = heap_made = (char *)p;
	}
	old = heap_brk;
	if ( incr < 0 ? (size_t)( -incr ) > (size_t)( heap_brk - heap_base )
		      : (size_t)incr > (size_t)( heap_base + HEAP_MAX - heap_brk ) ) {
		errno = ENOMEM;
		return (void *)-1;
	}
	if ( incr > 0 && heap_brk + incr > heap_made ) {
		size_t	need = ( (size_t)( heap_brk + incr - heap_made ) + HEAP_STEP - 1 ) & ~(size_t)( HEAP_STEP - 1 );

		if ( ts_ctl_mem(heap_made, (SZ)need, TS_MEM_RW) < E_OK ) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap_made += need;
	}
	heap_brk += incr;
	return old;
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
