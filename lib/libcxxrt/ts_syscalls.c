/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_syscalls.c
 *	What the C library (newlib) is built on, for a program with
 *	lib/libcxxrt (design 9.13, 17.19).
 *
 *	The heap (_sbrk) is one reservation of TS_HEAP_MAX bytes of
 *	addresses made on first use; pages are made in steps of HEAP_STEP as
 *	the break goes up. Descriptors 0 to 2 are the console: what is
 *	written there goes out with tm_putstring, reading gives end of file.
 *	Sockets have descriptors of their own range (ts_socket.c). Any
 *	other descriptor is a file of the file layer (fs_), numbered
 *	FD_BASE above the layer's own. Time comes from the monotonic clock
 *	(ts_get_mono) and the calendar (tk_get_utc).
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/umem.h>
#include <ts/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <reent.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>

IMPORT void ts_ext_prc( INT exitcd );
IMPORT ID   ts_get_pid( void );
IMPORT ER   tm_putstring( CONST UB *s );

/*
 * The file layer's calls (include/ts/fs.h), declared here with its own
 * flags: that header's O_ names are those of newlib's <fcntl.h> with
 * other values.
 */
typedef struct {
	UINT	mode;
	UD	size;
	UD	mtime;			/* seconds since 1985-01-01 UTC */
	UD	ino;
	UB	dev[16];
} FS_STAT;

IMPORT INT fs_open( CONST char *path, UINT oflags );
IMPORT ER  fs_close( INT fd );
IMPORT INT fs_read( INT fd, void *buf, SZ len );
IMPORT INT fs_write( INT fd, CONST void *buf, SZ len );
IMPORT D   fs_lseek( INT fd, D offset, INT whence );
IMPORT ER  fs_stat( CONST char *path, FS_STAT *st );
IMPORT ER  fs_fstat( INT fd, FS_STAT *st );
IMPORT ER  fs_unlink( CONST char *path );
IMPORT ER  fs_rename( CONST char *from, CONST char *to );
IMPORT ER  fs_mkdir( CONST char *path );
IMPORT ER  fs_ftruncate( INT fd, UD len );

#define FS_O_WRONLY	0x0001
#define FS_O_RDWR	0x0002
#define FS_O_CREAT	0x0100
#define FS_O_EXCL	0x0200
#define FS_O_TRUNC	0x0400
#define FS_O_APPEND	0x0800
#define FS_IFDIR	0x4000

#define FD_BASE		3
#define SOCK_BASE	0x4000		/* TS_SOCK_FD_BASE of include/tsposix/sys/socket.h */

/* ts_socket.c */
extern int	ts_sock_close( int fd );
extern int	ts_evfd_is( int fd );
extern long	ts_evfd_read( int fd, void *buf, size_t n );
extern long	ts_evfd_write( int fd, const void *buf, size_t n );
extern int	ts_evfd_close( int fd );
extern int	ts_sock_fcntl( int fd, int cmd, int val );
extern long	send( int fd, const void *buf, size_t len, int flags );
extern long	recv( int fd, void *buf, size_t len, int flags );

/* ts_urandom.c: /dev/urandom and /dev/random, the random source object */
extern int	ts_rnd_open( const char *path );
extern int	ts_rnd_is( int fd );
extern long	ts_rnd_read( int fd, void *buf, size_t n );
extern long	ts_rnd_write( int fd, const void *buf, size_t n );
extern int	ts_rnd_close( int fd );
#define TRON_EPOCH	473385600LL	/* 1985-01-01 as seconds since 1970-01-01 */

/* ---------------------------------------------------------------- memory */

#define TS_HEAP_MAX	( 64ULL * 1024 * 1024 * 1024 )
#define HEAP_STEP	( 1024 * 1024 )

static char	*heap_base, *heap_brk, *heap_made;

void *_sbrk( ptrdiff_t incr )
{
	char	*old;

	if ( heap_base == NULL ) {
		void	*p = NULL;

		if ( ts_map_mem(&p, (SZ)TS_HEAP_MAX, 0, TS_MEM_NONE) < E_OK ) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap_base = heap_brk = heap_made = (char *)p;
	}
	old = heap_brk;
	if ( incr < 0 ) {
		if ( (size_t)( -incr ) > (size_t)( heap_brk - heap_base ) ) {
			errno = EINVAL;
			return (void *)-1;
		}
		heap_brk += incr;
		return old;
	}
	if ( (size_t)incr > (size_t)( heap_base + TS_HEAP_MAX - heap_brk ) ) {
		errno = ENOMEM;
		return (void *)-1;
	}
	if ( heap_brk + incr > heap_made ) {
		size_t	need = (size_t)( heap_brk + incr - heap_made );

		need = ( need + HEAP_STEP - 1 ) & ~(size_t)( HEAP_STEP - 1 );
		if ( ts_ctl_mem(heap_made, (SZ)need, TS_MEM_RW) < E_OK ) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap_made += need;
	}
	heap_brk += incr;
	return old;
}

/* ---------------------------------------------------------------- descriptors */

static void put_console( const char *buf, size_t n )
{
	char	tmp[256];
	size_t	k, done = 0;

	while ( done < n ) {
		k = n - done;
		if ( k > sizeof(tmp) - 1 ) k = sizeof(tmp) - 1;
		memcpy(tmp, buf + done, k);
		tmp[k] = 0;
		tm_putstring((CONST UB *)tmp);
		done += k;
	}
}

static int fs_errno( int er )
{
	return ( er < 0 && er > -200 ) ? -er : EIO;
}

int _write( int fd, const void *buf, size_t n )
{
	INT	r;

	if ( fd == 1 || fd == 2 ) {
		put_console((const char *)buf, n);
		return (int)n;
	}
	if ( fd >= SOCK_BASE ) {
		return (int)send(fd, buf, n, 0);
	}
	if ( ts_evfd_is(fd) ) {
		return (int)ts_evfd_write(fd, buf, n);
	}
	if ( ts_rnd_is(fd) ) {
		return (int)ts_rnd_write(fd, buf, n);
	}
	if ( fd < FD_BASE ) {
		errno = EBADF;
		return -1;
	}
	r = fs_write(fd - FD_BASE, buf, (SZ)n);
	if ( r < 0 ) {
		errno = fs_errno(r);
		return -1;
	}
	return r;
}

int _read( int fd, void *buf, size_t n )
{
	INT	r;

	if ( fd < FD_BASE ) {
		return 0;
	}
	if ( fd >= SOCK_BASE ) {
		return (int)recv(fd, buf, n, 0);
	}
	if ( ts_evfd_is(fd) ) {
		return (int)ts_evfd_read(fd, buf, n);
	}
	if ( ts_rnd_is(fd) ) {
		return (int)ts_rnd_read(fd, buf, n);
	}
	r = fs_read(fd - FD_BASE, buf, (SZ)n);
	if ( r < 0 ) {
		errno = fs_errno(r);
		return -1;
	}
	return r;
}

int _open( const char *path, int flags, ... )
{
	UINT	f = 0;
	INT	fd;

	fd = ts_rnd_open(path);
	if ( fd != -2 ) {
		return fd;			/* /dev/urandom, /dev/random, or why not */
	}
	switch ( flags & O_ACCMODE ) {
	case O_WRONLY:	f = FS_O_WRONLY; break;
	case O_RDWR:	f = FS_O_RDWR; break;
	default:	f = 0; break;
	}
	if ( flags & O_CREAT )	f |= FS_O_CREAT;
	if ( flags & O_EXCL )	f |= FS_O_EXCL;
	if ( flags & O_TRUNC )	f |= FS_O_TRUNC;
	if ( flags & O_APPEND )	f |= FS_O_APPEND;
	fd = fs_open(path, f);
	if ( fd < 0 ) {
		errno = fs_errno(fd);
		return -1;
	}
	return fd + FD_BASE;
}

int _close( int fd )
{
	ER	er;

	if ( fd < FD_BASE ) {
		return 0;
	}
	if ( fd >= SOCK_BASE ) {
		return ts_sock_close(fd);
	}
	if ( ts_evfd_is(fd) ) {
		return ts_evfd_close(fd);
	}
	if ( ts_rnd_is(fd) ) {
		return ts_rnd_close(fd);
	}
	er = fs_close(fd - FD_BASE);
	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	return 0;
}

off_t _lseek( int fd, off_t off, int whence )
{
	D	r;

	if ( fd < FD_BASE || ts_rnd_is(fd) ) {
		return 0;
	}
	r = fs_lseek(fd - FD_BASE, (D)off, whence);
	if ( r < 0 ) {
		errno = fs_errno((int)r);
		return -1;
	}
	return (off_t)r;
}

static void to_stat( const FS_STAT *f, struct stat *st )
{
	memset(st, 0, sizeof(*st));
	st->st_mode = ( f->mode & FS_IFDIR ) ? ( S_IFDIR | 0755 ) : ( S_IFREG | 0644 );
	st->st_size = (off_t)f->size;
	st->st_ino = (ino_t)f->ino;
	st->st_nlink = 1;
	st->st_blksize = 4096;
	st->st_blocks = (blkcnt_t)( ( f->size + 511 ) / 512 );
	st->st_mtime = (time_t)( (long long)f->mtime + TRON_EPOCH );
	st->st_atime = st->st_mtime;
	st->st_ctime = st->st_mtime;
}

int _fstat( int fd, struct stat *st )
{
	FS_STAT	f;
	ER	er;

	if ( fd < FD_BASE || fd >= SOCK_BASE || ts_rnd_is(fd) ) {
		memset(st, 0, sizeof(*st));
		st->st_mode = ( fd >= SOCK_BASE ) ? S_IFSOCK : S_IFCHR;
		return 0;
	}
	er = fs_fstat(fd - FD_BASE, &f);
	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	to_stat(&f, st);
	return 0;
}

int _stat( const char *path, struct stat *st )
{
	FS_STAT	f;
	ER	er;

	if ( strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0 ) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0666;
		return 0;
	}
	er = fs_stat(path, &f);

	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	to_stat(&f, st);
	return 0;
}

/* The file layer has no symbolic links */
int lstat( const char *path, struct stat *st )
{
	return _stat(path, st);
}

/* fcntl: whether a socket blocks; a file's flags are not kept */
int _fcntl( int fd, int cmd, int arg )
{
	if ( fd >= SOCK_BASE ) {
		return ts_sock_fcntl(fd, cmd, arg);
	}
	switch ( cmd ) {
	case F_GETFL:	return O_RDWR;
	case F_GETFD:	return 0;
	default:	return 0;
	}
}

ssize_t readv( int fd, const struct iovec *iov, int n )
{
	ssize_t	done = 0;
	int	i;

	for ( i = 0; i < n; i++ ) {
		int r = _read(fd, iov[i].iov_base, iov[i].iov_len);

		if ( r < 0 ) return ( done > 0 ) ? done : -1;
		done += r;
		if ( (size_t)r < iov[i].iov_len ) break;
	}
	return done;
}

ssize_t writev( int fd, const struct iovec *iov, int n )
{
	ssize_t	done = 0;
	int	i;

	for ( i = 0; i < n; i++ ) {
		int r = _write(fd, iov[i].iov_base, iov[i].iov_len);

		if ( r < 0 ) return ( done > 0 ) ? done : -1;
		done += r;
		if ( (size_t)r < iov[i].iov_len ) break;
	}
	return done;
}

int _isatty( int fd )
{
	return ( fd >= 0 && fd < FD_BASE );
}

int _unlink( const char *path )
{
	ER	er = fs_unlink(path);

	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	return 0;
}

int _rename( const char *from, const char *to )
{
	ER	er = fs_rename(from, to);

	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	return 0;
}

int mkdir( const char *path, mode_t mode )
{
	ER	er;

	(void)mode;
	er = fs_mkdir(path);
	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	return 0;
}

long sysconf( int name )
{
	switch ( name ) {
	case _SC_PAGESIZE:		return 4096;
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:	return 2;
	case _SC_CLK_TCK:		return 100;
	case _SC_OPEN_MAX:		return 16;
	case _SC_PHYS_PAGES:		return 256 * 1024;
	case _SC_THREAD_STACK_MIN:	return 16384;
	default:
		errno = EINVAL;
		return -1;
	}
}

int getpagesize( void )
{
	return 4096;
}

int posix_memalign( void **p, size_t align, size_t size )
{
	void	*q = memalign(align, size);

	if ( q == NULL ) {
		return ENOMEM;
	}
	*p = q;
	return 0;
}

/* No links on the file layer: nothing is a symbolic link */
ssize_t readlink( const char *path, char *buf, size_t len )
{
	(void)path; (void)buf; (void)len;
	errno = EINVAL;
	return -1;
}

char *realpath( const char *path, char *resolved )
{
	size_t	n = strlen(path);

	if ( resolved == NULL ) {
		resolved = (char *)malloc(n + 1);
		if ( resolved == NULL ) {
			errno = ENOMEM;
			return NULL;
		}
	}
	memcpy(resolved, path, n + 1);
	return resolved;
}

/* At an offset, leaving the descriptor's own where it was */
ssize_t pread( int fd, void *buf, size_t n, off_t off )
{
	off_t	was = _lseek(fd, 0, SEEK_CUR);
	int	r;

	if ( was < 0 || _lseek(fd, off, SEEK_SET) < 0 ) return -1;
	r = _read(fd, buf, n);
	(void)_lseek(fd, was, SEEK_SET);
	return r;
}

ssize_t pwrite( int fd, const void *buf, size_t n, off_t off )
{
	off_t	was = _lseek(fd, 0, SEEK_CUR);
	int	r;

	if ( was < 0 || _lseek(fd, off, SEEK_SET) < 0 ) return -1;
	r = _write(fd, buf, n);
	(void)_lseek(fd, was, SEEK_SET);
	return r;
}

int ftruncate( int fd, off_t len )
{
	ER	er;

	if ( fd < FD_BASE || fd >= SOCK_BASE ) {
		errno = EBADF;
		return -1;
	}
	er = fs_ftruncate(fd - FD_BASE, (UD)len);
	if ( er < 0 ) {
		errno = fs_errno(er);
		return -1;
	}
	return 0;
}

int _link( const char *from, const char *to )
{
	(void)from; (void)to;
	errno = EMLINK;
	return -1;
}

/* ---------------------------------------------------------------- the process */

void _exit( int code )
{
	ts_ext_prc(code);
	for ( ;; ) ;
}

int _kill( int pid, int sig )
{
	if ( pid == (int)ts_get_pid() ) {
		_exit(128 + sig);
	}
	errno = EINVAL;
	return -1;
}

int _getpid( void )
{
	return (int)ts_get_pid();
}

int _fork( void )
{
	errno = ENOSYS;
	return -1;
}

int _wait( int *status )
{
	(void)status;
	errno = ECHILD;
	return -1;
}

int _execve( const char *path, char *const argv[], char *const envp[] )
{
	(void)path; (void)argv; (void)envp;
	errno = ENOSYS;
	return -1;
}

/* ---------------------------------------------------------------- time */

static int64_t	rt_offset;		/* CLOCK_REALTIME - CLOCK_MONOTONIC, nanoseconds */
static uint64_t	rt_taken;		/* when it was last read from the calendar */

static uint64_t mono_ns( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (uint64_t)ns;
}

static void rt_refresh( uint64_t now )
{
	SYSTIM	t;

	if ( tk_get_utc(&t) >= E_OK ) {
		int64_t	ms = ( (int64_t)t.hi << 32 ) | t.lo;

		rt_offset = ms * 1000000LL - (int64_t)now;
		rt_taken = now;
	}
}

void __ts_time_init( void )
{
	rt_refresh(mono_ns());
}

/* The calendar is read again once a second, so that a clock set meanwhile is seen */
int64_t __ts_realtime_offset( void )
{
	uint64_t now = mono_ns();

	if ( now - rt_taken > 1000000000ULL ) {
		rt_refresh(now);
	}
	return rt_offset;
}

int clock_gettime( clockid_t clk, struct timespec *ts )
{
	uint64_t now = mono_ns();
	int64_t	t = (int64_t)now;

	if ( clk == CLOCK_REALTIME ) {
		t += __ts_realtime_offset();
	}
	ts->tv_sec = (time_t)( t / 1000000000LL );
	ts->tv_nsec = (long)( t % 1000000000LL );
	return 0;
}

/* The calendar's time of a broken-down UTC time; days from the civil calendar */
time_t timegm( struct tm *tm )
{
	long long	y = tm->tm_year + 1900LL, m = tm->tm_mon, days;

	y += m / 12;
	m %= 12;
	if ( m < 0 ) {
		m += 12;
		y--;
	}
	if ( m < 2 ) y--;
	{
		long long era = ( y >= 0 ? y : y - 399 ) / 400;
		long long yoe = y - era * 400;
		long long mp = ( m + 9 ) % 12;
		long long doy = ( 153 * mp + 2 ) / 5 + tm->tm_mday - 1;
		long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

		days = era * 146097 + doe - 719468;
	}
	return (time_t)( days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec );
}

int uname( struct utsname *u )
{
	memset(u, 0, sizeof(*u));
	strcpy(u->sysname, "TessronOS");
	strcpy(u->nodename, "tessronos");
	strcpy(u->release, "1");
	strcpy(u->version, "1");
	strcpy(u->machine, "aarch64");
	return 0;
}

int clock_getres( clockid_t clk, struct timespec *ts )
{
	(void)clk;
	if ( ts != NULL ) {
		ts->tv_sec = 0;
		ts->tv_nsec = 1000;
	}
	return 0;
}

int _gettimeofday( struct timeval *tv, void *tz )
{
	struct timespec	ts;

	(void)tz;
	clock_gettime(CLOCK_REALTIME, &ts);
	if ( tv != NULL ) {
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	}
	return 0;
}

clock_t _times( struct tms *buf )
{
	clock_t	c = (clock_t)( mono_ns() / ( 1000000000ULL / CLOCKS_PER_SEC ) );

	if ( buf != NULL ) {
		buf->tms_utime = c;
		buf->tms_stime = 0;
		buf->tms_cutime = 0;
		buf->tms_cstime = 0;
	}
	return c;
}

int nanosleep( const struct timespec *req, struct timespec *rem )
{
	uint64_t us = (uint64_t)req->tv_sec * 1000000ULL + (uint64_t)req->tv_nsec / 1000;

	if ( req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L ) {
		errno = EINVAL;
		return -1;
	}
	if ( us > 0 ) {
		(void)tk_dly_tsk_u((RELTIM_U)us);
	}
	if ( rem != NULL ) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}

int usleep( useconds_t us )
{
	if ( us > 0 ) (void)tk_dly_tsk_u((RELTIM_U)us);
	return 0;
}

unsigned sleep( unsigned s )
{
	(void)tk_dly_tsk_u((RELTIM_U)s * 1000000ULL);
	return 0;
}
