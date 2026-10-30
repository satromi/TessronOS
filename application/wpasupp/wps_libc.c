/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wps_libc.c
 *	What the C library (newlib) and OpenSSL's libcrypto need from the
 *	system, for wpasupp. The process has one task, keeps no file and
 *	has no directory: the heap is a range of addresses reserved once
 *	and committed as the break moves up (as the other programs have
 *	it), what is written to the standard output and error goes to the
 *	console, a file cannot be opened, and the time of day is the
 *	system's clock since it started (OpenSSL takes it for the intervals
 *	of its reseeding only).
 */

#include <config.h>
#include <stddef.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/uapp.h>
#include <ts/umem.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>

#define HEAP_MAX	( (SZ)64 * 1024 * 1024 )
#define PAGE		4096

static char	*heap_base = NULL;
static size_t	heap_brk = 0;
static size_t	heap_top = 0;

void wps_libc_init( void )
{
}

void *_sbrk( ptrdiff_t incr )
{
	char	*p;

	if ( heap_base == NULL ) {
		void	*a = NULL;

		if ( ts_map_mem(&a, HEAP_MAX, PAGE, TS_MEM_NONE) < E_OK ) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap_base = (char *)a;
	}
	if ( incr < 0 ? (size_t)( -incr ) > heap_brk : heap_brk + (size_t)incr > (size_t)HEAP_MAX ) {
		errno = ENOMEM;
		return (void *)-1;
	}
	p = heap_base + heap_brk;
	if ( incr > 0 ) {
		size_t	need = ( heap_brk + (size_t)incr + PAGE - 1 ) & ~(size_t)( PAGE - 1 );

		if ( need > heap_top ) {
			if ( ts_ctl_mem(heap_base + heap_top, (SZ)( need - heap_top ), TS_MEM_RW) < E_OK ) {
				errno = ENOMEM;
				return (void *)-1;
			}
			heap_top = need;
		}
	}
	heap_brk += (size_t)incr;
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

int _read( int fd, void *buf, size_t n ) { (void)fd; (void)buf; (void)n; return 0; }
int _close( int fd ) { (void)fd; errno = EBADF; return -1; }
int _fstat( int fd, struct stat *st ) { (void)fd; memset(st, 0, sizeof(*st)); st->st_mode = S_IFCHR; return 0; }
int _stat( const char *path, struct stat *st ) { (void)path; (void)st; errno = ENOENT; return -1; }
int _isatty( int fd ) { return fd <= 2; }
off_t _lseek( int fd, off_t off, int whence ) { (void)fd; (void)off; (void)whence; return 0; }
int _open( const char *path, int flags, ... ) { (void)path; (void)flags; errno = ENOENT; return -1; }
void _exit( int code ) { ts_ext_prc(code); for ( ;; ) ; }
int _kill( int pid, int sig ) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid( void ) { return (int)ts_get_pid(); }

int _gettimeofday( struct timeval *tv, void *tz )
{
	SYSTIM	t;
	unsigned long long ms;

	(void)tz;
	if ( tv == NULL ) return 0;
	ms = ( tk_get_otm(&t) >= E_OK ) ? ( ( (unsigned long long)(UW)t.hi << 32 ) | (UW)t.lo ) : 0;
	tv->tv_sec = (time_t)( ms / 1000 );
	tv->tv_usec = (suseconds_t)( ms % 1000 ) * 1000;
	return 0;
}

/* The process is the system's: no user of its own to ask after */
uid_t getuid( void ) { return 0; }
uid_t geteuid( void ) { return 0; }
gid_t getgid( void ) { return 0; }
gid_t getegid( void ) { return 0; }

unsigned int sleep( unsigned int s )
{
	(void)tk_dly_tsk((TMO)s * 1000);
	return 0;
}

DIR *opendir( const char *name ) { (void)name; errno = ENOENT; return NULL; }
struct dirent *readdir( DIR *dir ) { (void)dir; errno = EBADF; return NULL; }
int closedir( DIR *dir ) { (void)dir; errno = EBADF; return -1; }

/* newlib has memalign only */
extern void	*memalign( size_t align, size_t n );

int posix_memalign( void **p, size_t align, size_t n )
{
	void	*m = memalign(align, n);

	if ( m == NULL ) return ENOMEM;
	*p = m;
	return 0;
}
