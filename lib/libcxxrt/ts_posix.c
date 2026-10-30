/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_posix.c
 *	POSIX calls a C++ program reaches for beyond newlib's stubs, answered
 *	on the file layer and the process (design 17.19).
 *
 *	The *at calls take only AT_FDCWD for their directory: the file layer
 *	has absolute paths and no descriptor stands for a directory to be
 *	resolved against. A process has one user, the one it runs as (0), and
 *	no working directory other than the root. A descriptor is not
 *	duplicated (the file layer keeps one offset per open), so dup fails
 *	with EMFILE, as when the table is full.
 */

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef AT_FDCWD
#define AT_FDCWD		-100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR		0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW	0x100
#endif

extern int	_open( const char *path, int flags, ... );
extern int	_stat( const char *path, struct stat *st );
extern int	_unlink( const char *path );
extern int	fs_rmdir( const char *path );
extern int	fs_sync( void );
extern int	fs_fsync( int fd );

int access( const char *path, int mode )
{
	struct stat	st;

	(void)mode;
	return ( _stat(path, &st) == 0 ) ? 0 : -1;
}

int creat( const char *path, mode_t mode )
{
	(void)mode;
	return _open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

extern int	ts_memfd_is( int fd );
extern int	ts_memfd_dup( int fd );

/* Only a memory object's descriptor (memfd_create) can be duplicated */
int dup( int fd )
{
	if ( ts_memfd_is(fd) ) {
		return ts_memfd_dup(fd);
	}
	errno = EMFILE;
	return -1;
}

/* The file's own writes on the medium (design 7.3) */
int fdatasync( int fd )
{
	return ( fs_fsync(fd) < 0 ) ? ( errno = EIO, -1 ) : 0;
}

int openat( int dirfd, const char *path, int flags, ... )
{
	if ( dirfd != AT_FDCWD && path[0] != '/' ) {
		errno = ENOTSUP;
		return -1;
	}
	return _open(path, flags);
}

int fstatat( int dirfd, const char *path, struct stat *st, int flags )
{
	(void)flags;
	if ( dirfd != AT_FDCWD && path[0] != '/' ) {
		errno = ENOTSUP;
		return -1;
	}
	return _stat(path, st);
}

int unlinkat( int dirfd, const char *path, int flags )
{
	int	er;

	if ( dirfd != AT_FDCWD && path[0] != '/' ) {
		errno = ENOTSUP;
		return -1;
	}
	if ( ( flags & AT_REMOVEDIR ) == 0 ) return _unlink(path);
	er = fs_rmdir(path);
	if ( er < 0 ) {
		errno = ( er > -200 ) ? -er : EIO;
		return -1;
	}
	return 0;
}

char *getcwd( char *buf, size_t size )
{
	if ( buf == NULL || size < 2 ) {
		errno = ( buf == NULL ) ? EINVAL : ERANGE;
		return NULL;
	}
	strcpy(buf, "/");
	return buf;
}

uid_t geteuid( void )
{
	return 0;
}

/* The user and group as POSIX names them: one of each, the same as the effective ones */
uid_t getuid( void )
{
	return 0;
}

gid_t getgid( void )
{
	return 0;
}

gid_t getegid( void )
{
	return 0;
}

/*
 * The calling thread's policy, as pthread_setschedparam maps it onto the
 * kernel's two bands (lib/libpthread, design 4.3.1); pid 0 or the
 * process's own
 */
int sched_setscheduler( pid_t pid, int policy, const struct sched_param *param )
{
	int	e;

	if ( pid != 0 && pid != getpid() ) {
		errno = ESRCH;
		return -1;
	}
	e = pthread_setschedparam(pthread_self(), policy, param);
	if ( e != 0 ) {
		errno = e;
		return -1;
	}
	return 0;
}

/* Interfaces have no names on TessronOS's sockets (include/tsposix/net/if.h) */
unsigned int if_nametoindex( const char *name )
{
	(void)name;
	errno = ENODEV;
	return 0;
}

char *if_indextoname( unsigned int index, char *name )
{
	(void)index; (void)name;
	errno = ENXIO;
	return NULL;
}
