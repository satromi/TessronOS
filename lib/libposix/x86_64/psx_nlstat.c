/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_nlstat.c (x86-64)
 *	The status calls of newlib's own stdio, to the narrow struct stat of
 *	newlib's header (_fstat and _stat, psx_fd.c), and its clock.
 *
 *	newlib of x86_64-elf is built with MISSING_SYSCALL_NAMES: its
 *	_fstat_r and _stat_r call fstat and stat, which here are the POSIX
 *	calls with the wide struct stat of include/posix, and would write
 *	past the narrow one newlib's stdio hands them (its buffer's size,
 *	whether a descriptor is a terminal). These take their place in the
 *	link, before newlib's library, and call the narrow ones.
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/time.h>

extern int	_fstat( int fd, void *st );
extern int	_stat( const char *path, void *st );

int _fstat_r( struct _reent *r, int fd, void *st )
{
	int	ret;

	errno = 0;
	ret = _fstat(fd, st);
	if ( ret == -1 && errno != 0 ) {
		r->_errno = errno;
	}
	return ret;
}

int _stat_r( struct _reent *r, const char *path, void *st )
{
	int	ret;

	errno = 0;
	ret = _stat(path, st);
	if ( ret == -1 && errno != 0 ) {
		r->_errno = errno;
	}
	return ret;
}

/*
 * gettimeofday, which the same newlib reaches by its plain name and libposix
 * gives by the underscored one (psx_time.c).
 */
extern int	_gettimeofday( struct timeval *tv, void *tz );

int gettimeofday( struct timeval *tv, void *tz )
{
	return _gettimeofday(tv, tz);
}
