/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_fd.c
 *	Descriptors (lib/libposix, design 5.8.2): the table of POSIX
 *	numbers over the file layer's descriptors, and the calls on them
 *	newlib and the commands make.
 */

#include "psx_int.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

IMPORT ER tm_putstring( CONST UB *s );

/* ---------------------------------------------------------------- errors */

int __psx_errno_fs( int er )
{
	switch ( er ) {
	  case EX_PERM:		return EPERM;
	  case EX_NOENT:	return ENOENT;
	  case EX_INTR:		return EINTR;
	  case EX_IO:		return EIO;
	  case EX_NXIO:		return ENXIO;
	  case EX_NOEXEC:	return ENOEXEC;
	  case EX_BADF:		return EBADF;
	  case EX_AGAIN:	return EAGAIN;
	  case EX_NOMEM:	return ENOMEM;
	  case EX_ACCES:	return EACCES;
	  case EX_FAULT:	return EFAULT;
	  case EX_BUSY:		return EBUSY;
	  case EX_EXIST:	return EEXIST;
	  case EX_NOTDIR:	return ENOTDIR;
	  case EX_ISDIR:	return EISDIR;
	  case EX_INVAL:	return EINVAL;
	  case EX_MFILE:	return EMFILE;
	  case EX_NOTTY:	return ENOTTY;
	  case EX_FBIG:		return EFBIG;
	  case EX_NOSPC:	return ENOSPC;
	  case EX_ROFS:		return EROFS;
	  case EX_PIPE:		return EPIPE;
	  case EX_NAMETOOLONG:	return ENAMETOOLONG;
	  case EX_NOTEMPTY:	return ENOTEMPTY;
	  case EX_NOTSUP:	return ENOTSUP;
	  default:		return EIO;
	}
}

int __psx_errno_er( int er )
{
	switch ( er ) {
	  case E_NOEXS:	return ENOENT;
	  case E_OACV:	return EACCES;
	  case E_PAR:	return EINVAL;
	  case E_NOMEM:	return ENOMEM;
	  case E_LIMIT:	return EAGAIN;
	  case E_RLWAI:
	  case E_DISWAI:return EINTR;
	  case E_TMOUT:	return EAGAIN;
	  case E_OBJ:	return EPERM;
	  case E_MACV:	return EFAULT;
	  case E_ID:	return EBADF;
	  case E_NOSPT:	return ENOTSUP;
	  case E_RONLY:	return EROFS;
	  case E_BUSY:	return EBUSY;
	  case E_IO:	return EIO;
	  default:	return ( er > -200 && er < 0 ) ? __psx_errno_fs(er) : EIO;
	}
}

int __psx_fail_fs( int er )
{
	errno = __psx_errno_fs(er);
	return -1;
}

int __psx_fail_er( int er )
{
	errno = __psx_errno_er(er);
	return -1;
}

/* ---------------------------------------------------------------- the table */

PSXFD *__psx_fd_get( int fd )
{
	if ( fd < 0 || fd >= PSX_FD_MAX || __psx_fd[fd].kind == PK_NONE ) {
		errno = EBADF;
		return NULL;
	}
	return &__psx_fd[fd];
}

int __psx_fd_alloc( int from )
{
	int	i;

	for ( i = ( from < 0 ) ? 0 : from; i < PSX_FD_MAX; i++ ) {
		if ( __psx_fd[i].kind == PK_NONE ) return i;
	}
	errno = EMFILE;
	return -1;
}

int __psx_fd_new( int kfd, int from, int cloexec )
{
	int	fd = __psx_fd_alloc(from);

	if ( fd < 0 ) {
		(void)fs_close(kfd);
		return -1;
	}
	__psx_fd[fd].kind = PK_FS;
	__psx_fd[fd].kfd = kfd;
	__psx_fd[fd].cloexec = (unsigned char)( cloexec ? 1 : 0 );
	__psx_fd[fd].path = NULL;
	return fd;
}

/* ---------------------------------------------------------------- newlib's calls */

static UINT fs_flags( int flags )
{
	UINT	f;

	switch ( flags & O_ACCMODE ) {
	  case O_WRONLY:	f = 0x0001; break;
	  case O_RDWR:		f = 0x0002; break;
	  default:		f = 0; break;
	}
	if ( flags & O_CREAT )		f |= 0x0100;
	if ( flags & O_EXCL )		f |= 0x0200;
	if ( flags & O_TRUNC )		f |= 0x0400;
	if ( flags & O_APPEND )		f |= 0x0800;
	if ( flags & O_DIRECTORY )	f |= 0x1000;
	if ( flags & O_NONBLOCK )	f |= 0x2000;
	if ( flags & O_NOCTTY )		f |= 0x4000;
	return f;
}

static int open_path( const char *path, int flags, mode_t mode )
{
	char	p[PATH_MAX];
	INT	kfd;
	int	fd;

	if ( __psx_path(path, p) < 0 ) {
		return -1;
	}
	/* the descriptors a program already has, by their names */
	if ( strncmp(p, "/dev/fd/", 8) == 0 || strcmp(p, "/dev/stdin") == 0
	  || strcmp(p, "/dev/stdout") == 0 || strcmp(p, "/dev/stderr") == 0 ) {
		int	n = ( p[5] == 'f' ) ? atoi(p + 8) : ( p[8] == 'i' ) ? 0 : ( p[9] == 'o' ) ? 1 : 2;

		return fcntl(n, F_DUPFD, 0);
	}
	kfd = fs_open(p, fs_flags(flags));
	if ( kfd < 0 ) {
		return __psx_fail_fs(kfd);
	}
	if ( ( flags & O_CREAT ) != 0 ) {
		(void)fs_chmod(p, (UINT)( mode & ~__psx_umask & 07777 ));
	}
	fd = __psx_fd_new(kfd, 0, ( flags & O_CLOEXEC ) != 0);
	if ( fd >= 0 ) __psx_fd[fd].path = strdup(p);	/* for fchdir and the calls named "at" */
	return fd;
}

int _open( const char *path, int flags, ... )
{
	va_list	ap;
	mode_t	mode = 0;

	if ( flags & O_CREAT ) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return open_path(path, flags, mode);
}

int open( const char *path, int flags, ... ) __attribute__((alias("_open")));

int openat( int dirfd, const char *path, int flags, ... )
{
	va_list	ap;
	mode_t	mode = 0;

	if ( flags & O_CREAT ) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	{
		char	p[PATH_MAX];

		if ( __psx_path_at(dirfd, path, p) < 0 ) return -1;
		return open_path(p, flags, mode);
	}
}

int creat( const char *path, mode_t mode )
{
	return open_path(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

static void con_write( const void *buf, size_t n )
{
	char	tmp[256];
	size_t	k, done = 0;

	while ( done < n ) {
		k = n - done;
		if ( k > sizeof(tmp) - 1 ) k = sizeof(tmp) - 1;
		memcpy(tmp, (const char *)buf + done, k);
		tmp[k] = 0;
		tm_putstring((CONST UB *)tmp);
		done += k;
	}
}

int _write( int fd, const void *buf, size_t n )
{
	PSXFD	*e = __psx_fd_get(fd);
	INT	r;

	if ( e == NULL ) return -1;
	if ( e->kind == PK_CONOUT ) {
		con_write(buf, n);
		return (int)n;
	}
	if ( e->kind != PK_FS ) {
		errno = EBADF;
		return -1;
	}
	do {
		r = fs_write(e->kfd, buf, (SZ)n);
	} while ( r == EX_INTR && __psx_restart() );
	return ( r < 0 ) ? __psx_fail_fs(r) : r;
}

_READ_WRITE_RETURN_TYPE write( int fd, const void *buf, size_t n )
{
	return _write(fd, buf, n);
}

int _read( int fd, void *buf, size_t n )
{
	PSXFD	*e = __psx_fd_get(fd);
	INT	r;

	if ( e == NULL ) return -1;
	if ( e->kind == PK_CONIN ) return 0;
	if ( e->kind != PK_FS ) {
		errno = EBADF;
		return -1;
	}
	do {
		r = fs_read(e->kfd, buf, (SZ)n);
	} while ( r == EX_INTR && __psx_restart() );
	return ( r < 0 ) ? __psx_fail_fs(r) : r;
}

_READ_WRITE_RETURN_TYPE read( int fd, void *buf, size_t n )
{
	return _read(fd, buf, n);
}

/* Whether any other entry names the same kernel descriptor */
static int shared( int fd, int kfd )
{
	int	i;

	for ( i = 0; i < PSX_FD_MAX; i++ ) {
		if ( i != fd && __psx_fd[i].kind == PK_FS && __psx_fd[i].kfd == kfd ) return 1;
	}
	return 0;
}

int _close( int fd )
{
	PSXFD	*e = __psx_fd_get(fd);
	int	kind, kfd;
	ER	er = EX_OK;

	if ( e == NULL ) return -1;
	kind = e->kind;
	kfd = e->kfd;
	e->kind = PK_NONE;
	e->cloexec = 0;
	e->kfd = -1;
	if ( e->path != NULL ) {
		free(e->path);
		e->path = NULL;
	}
	if ( kind == PK_FS && !shared(fd, kfd) ) {
		er = fs_close(kfd);
	}
	return ( er < 0 && er != EX_IO ) ? __psx_fail_fs(er) : 0;
}

int close( int fd ) __attribute__((alias("_close")));

off_t _lseek( int fd, off_t off, int whence )
{
	PSXFD	*e = __psx_fd_get(fd);
	D	r;

	if ( e == NULL ) return -1;
	if ( e->kind != PK_FS ) {
		errno = ESPIPE;
		return -1;
	}
	r = fs_lseek(e->kfd, (D)off, whence);
	if ( r == EX_INVAL ) {
		T_FSXSTAT x;

		if ( fs_fxstat(e->kfd, &x) >= 0 && ( x.st.mode & FS_IFMT ) == FS_IFIFO ) {
			errno = ESPIPE;
			return -1;
		}
	}
	return ( r < 0 ) ? (off_t)__psx_fail_fs((int)r) : (off_t)r;
}

off_t lseek( int fd, off_t off, int whence ) __attribute__((alias("_lseek")));

/*
 * newlib's own struct stat, as its stdio asks _fstat of a descriptor
 * (whether it is a terminal, how big its blocks are): the layout of
 * newlib's header, with its narrow numbers.
 */
struct nl_stat {
	short		st_dev;
	unsigned short	st_ino;
	__uint32_t	st_mode;
	unsigned short	st_nlink;
	unsigned short	st_uid;
	unsigned short	st_gid;
	short		st_rdev;
	long		st_size;
	struct timespec	st_atim, st_mtim, st_ctim;
	long		st_blksize;
	long		st_blocks;
	long		st_spare4[2];
};

static void nl_of( const struct stat *s, struct nl_stat *n )
{
	memset(n, 0, sizeof(*n));
	n->st_mode = s->st_mode;
	n->st_nlink = (unsigned short)s->st_nlink;
	n->st_size = (long)s->st_size;
	n->st_atim = s->st_atim;
	n->st_mtim = s->st_mtim;
	n->st_ctim = s->st_ctim;
	n->st_blksize = s->st_blksize;
	n->st_blocks = s->st_blocks;
	n->st_ino = (unsigned short)s->st_ino;
}

int _fstat( int fd, struct nl_stat *n )
{
	struct stat	s;

	if ( fstat(fd, &s) < 0 ) return -1;
	nl_of(&s, n);
	return 0;
}

int _stat( const char *path, struct nl_stat *n )
{
	struct stat	s;

	if ( stat(path, &s) < 0 ) return -1;
	nl_of(&s, n);
	return 0;
}

int fstat( int fd, struct stat *st )
{
	PSXFD		*e = __psx_fd_get(fd);
	T_FSXSTAT	x;
	ER		er;

	if ( e == NULL ) return -1;
	if ( e->kind != PK_FS ) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0620;
		st->st_blksize = 1024;
		st->st_nlink = 1;
		return 0;
	}
	er = fs_fxstat(e->kfd, &x);
	if ( er < 0 ) return __psx_fail_fs(er);
	__psx_stat_of(&x, st);
	return 0;
}

int isatty( int fd )
{
	PSXFD	*e = __psx_fd_get(fd);

	if ( e == NULL ) return 0;
	if ( e->kind == PK_CONOUT || e->kind == PK_CONIN ) return 0;
	if ( fs_ioctl(e->kfd, FS_IOC_ISTTY, NULL) < 0 ) {
		errno = ENOTTY;
		return 0;
	}
	return 1;
}

int _isatty( int fd ) __attribute__((alias("isatty")));

/* ---------------------------------------------------------------- duplicates */

static int dup_to( int fd, int to, int cloexec, int at_least )
{
	PSXFD	*e = __psx_fd_get(fd);
	int	nfd;

	if ( e == NULL ) return -1;
	if ( to < 0 ) {
		nfd = __psx_fd_alloc(at_least);
		if ( nfd < 0 ) return -1;
	} else {
		if ( to >= PSX_FD_MAX ) {
			errno = EBADF;
			return -1;
		}
		if ( to == fd ) return to;
		if ( __psx_fd[to].kind != PK_NONE ) (void)close(to);
		nfd = to;
	}
	__psx_fd[nfd] = *e;
	__psx_fd[nfd].cloexec = (unsigned char)( cloexec ? 1 : 0 );
	__psx_fd[nfd].path = ( e->path != NULL ) ? strdup(e->path) : NULL;
	return nfd;
}

int dup( int fd )
{
	return dup_to(fd, -1, 0, 0);
}

int dup2( int fd, int to )
{
	if ( __psx_fd_get(fd) == NULL ) return -1;
	return dup_to(fd, to, 0, 0);
}

int dup3( int fd, int to, int flags )
{
	if ( fd == to ) {
		errno = EINVAL;
		return -1;
	}
	return dup_to(fd, to, ( flags & O_CLOEXEC ) != 0, 0);
}

int _fcntl( int fd, int cmd, int arg )
{
	PSXFD	*e = __psx_fd_get(fd);
	INT	r;

	if ( e == NULL ) return -1;
	switch ( cmd ) {
	  case F_DUPFD:
		return dup_to(fd, -1, 0, arg);
	  case F_DUPFD_CLOEXEC:
		return dup_to(fd, -1, 1, arg);
	  case F_GETFD:
		return e->cloexec ? FD_CLOEXEC : 0;
	  case F_SETFD:
		e->cloexec = (unsigned char)( ( arg & FD_CLOEXEC ) != 0 );
		return 0;
	  case F_GETFL:
		if ( e->kind == PK_CONOUT ) return O_WRONLY;
		if ( e->kind == PK_CONIN ) return O_RDONLY;
		r = fs_fcntl(e->kfd, FS_F_GETFL, 0);
		if ( r < 0 ) return __psx_fail_fs(r);
		return ( ( r & 3 ) == 1 ? O_WRONLY : ( r & 3 ) == 2 ? O_RDWR : O_RDONLY )
		     | ( ( r & 0x0800 ) ? O_APPEND : 0 ) | ( ( r & 0x2000 ) ? O_NONBLOCK : 0 );
	  case F_SETFL:
		if ( e->kind != PK_FS ) return 0;
		r = fs_fcntl(e->kfd, FS_F_SETFL, ( ( arg & O_APPEND ) ? 0x0800 : 0 )
						| ( ( arg & O_NONBLOCK ) ? 0x2000 : 0 ));
		return ( r < 0 ) ? __psx_fail_fs(r) : 0;
	  case F_CLOSEM:
		closefrom(fd);
		return 0;
	  case F_MAXFD:
		{
			int	i, top = -1;

			for ( i = 0; i < PSX_FD_MAX; i++ ) {
				if ( __psx_fd[i].kind != PK_NONE ) top = i;
			}
			return top;
		}
	  case F_GETLK:
	  case F_SETLK:
	  case F_SETLKW:
		return 0;			/* no locks: every lock is had at once */
	  default:
		errno = EINVAL;
		return -1;
	}
}

int fcntl( int fd, int cmd, ... )
{
	va_list	ap;
	int	arg;

	va_start(ap, cmd);
	arg = va_arg(ap, int);
	va_end(ap);
	return _fcntl(fd, cmd, arg);
}

void closefrom( int from )
{
	int	i;

	for ( i = ( from < 0 ) ? 0 : from; i < PSX_FD_MAX; i++ ) {
		if ( __psx_fd[i].kind != PK_NONE ) (void)close(i);
	}
}

int pipe2( int fds[2], int flags )
{
	INT	k[2];
	ER	er;
	int	a, b;

	er = fs_pipe(k);
	if ( er < 0 ) return __psx_fail_fs(er);
	a = __psx_fd_new(k[0], 0, ( flags & O_CLOEXEC ) != 0);
	if ( a < 0 ) {
		(void)fs_close(k[1]);
		return -1;
	}
	b = __psx_fd_new(k[1], 0, ( flags & O_CLOEXEC ) != 0);
	if ( b < 0 ) {
		(void)close(a);
		return -1;
	}
	if ( flags & O_NONBLOCK ) {
		(void)fcntl(a, F_SETFL, O_NONBLOCK);
		(void)fcntl(b, F_SETFL, O_NONBLOCK);
	}
	fds[0] = a;
	fds[1] = b;
	return 0;
}

int pipe( int fds[2] )
{
	return pipe2(fds, 0);
}

/* ---------------------------------------------------------------- ioctl */

int ioctl( int fd, unsigned long req, ... )
{
	PSXFD	*e = __psx_fd_get(fd);
	va_list	ap;
	void	*arg;
	UINT	cmd;
	ER	er;
	INT	v;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if ( e == NULL ) return -1;
	if ( req == FIOCLEX || req == FIONCLEX ) {
		e->cloexec = (unsigned char)( req == FIOCLEX );
		return 0;
	}
	if ( e->kind != PK_FS ) {
		errno = ENOTTY;
		return -1;
	}
	switch ( req ) {
	  case TIOCGETA:	cmd = FS_IOC_TCGETS; break;
	  case TIOCSETA:	cmd = FS_IOC_TCSETS; break;
	  case TIOCSETAW:	cmd = FS_IOC_TCSETSW; break;
	  case TIOCSETAF:	cmd = FS_IOC_TCSETSF; break;
	  case TIOCGWINSZ:	cmd = FS_IOC_GWINSZ; break;
	  case TIOCSWINSZ:	cmd = FS_IOC_SWINSZ; break;
	  case TIOCGPGRP:	cmd = FS_IOC_GPGRP; break;
	  case TIOCSPGRP:	cmd = FS_IOC_SPGRP; break;
	  case TIOCSCTTY:	cmd = FS_IOC_SCTTY; break;
	  case TIOCNOTTY:	cmd = FS_IOC_NOTTY; break;
	  case TIOCGPTN:	cmd = FS_IOC_GPTN; break;
	  case TIOCGSID:	cmd = FS_IOC_GSID; break;
	  case TIOCDRAIN:	cmd = FS_IOC_DRAIN; break;
	  case FIONREAD:	cmd = FS_IOC_NREAD; break;
	  case TIOCFLUSH:
		v = ( arg == NULL ) ? 2 : ( *(int *)arg == FREAD ) ? 0 : ( *(int *)arg == FWRITE ) ? 1 : 2;
		er = fs_ioctl(e->kfd, FS_IOC_FLUSH, &v);
		return ( er < 0 ) ? __psx_fail_fs(er) : 0;
	  case FIONBIO:
		return fcntl(fd, F_SETFL, ( arg != NULL && *(int *)arg ) ? O_NONBLOCK : 0);
	  case TIOCEXCL:
	  case TIOCNXCL:
	  case TIOCSTART:
	  case TIOCSTOP:
		return 0;
	  default:
		errno = ENOTTY;
		return -1;
	}
	er = fs_ioctl(e->kfd, cmd, arg);
	return ( er < 0 ) ? __psx_fail_fs(er) : 0;
}

/* ---------------------------------------------------------------- waiting for several */

int poll( struct pollfd *fds, nfds_t n, int timeout )
{
	T_FSPOLL	k[64];
	nfds_t		i;
	int		r, ready = 0;

	if ( n > 64 ) {
		errno = EINVAL;
		return -1;
	}
	for ( i = 0; i < n; i++ ) {
		PSXFD *e = ( fds[i].fd >= 0 && fds[i].fd < PSX_FD_MAX ) ? &__psx_fd[fds[i].fd] : NULL;

		k[i].fd = -1;
		k[i].events = (UINT)fds[i].events & ( POLLIN | POLLPRI | POLLOUT );
		if ( fds[i].events & POLLRDNORM ) k[i].events |= FS_POLL_IN;
		k[i].revents = 0;
		fds[i].revents = 0;
		if ( fds[i].fd < 0 ) continue;
		if ( e == NULL || e->kind == PK_NONE ) {
			fds[i].revents = POLLNVAL;
		} else if ( e->kind == PK_CONOUT ) {
			fds[i].revents = fds[i].events & POLLOUT;
		} else if ( e->kind == PK_CONIN ) {
			fds[i].revents = ( fds[i].events & POLLIN ) | POLLHUP;
		} else {
			k[i].fd = e->kfd;
		}
		if ( fds[i].revents != 0 ) ready++;
	}
	r = fs_poll(k, (INT)n, ( ready > 0 ) ? TMO_POL : ( timeout < 0 ) ? TMO_FEVR : (TMO)timeout);
	if ( r < 0 ) {
		return __psx_fail_fs(r);
	}
	for ( i = 0; i < n; i++ ) {
		if ( k[i].fd >= 0 && k[i].revents != 0 ) {
			fds[i].revents = (short)k[i].revents;
			if ( ( k[i].revents & FS_POLL_IN ) && ( fds[i].events & POLLRDNORM ) ) {
				fds[i].revents |= POLLRDNORM;
			}
			ready++;
		}
	}
	return ready;
}

int select( int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv )
{
	struct pollfd	p[64];
	int		n = 0, i, r, ms;

	for ( i = 0; i < nfds && n < 64; i++ ) {
		short	ev = 0;

		if ( rd != NULL && FD_ISSET(i, rd) ) ev |= POLLIN;
		if ( wr != NULL && FD_ISSET(i, wr) ) ev |= POLLOUT;
		if ( ex != NULL && FD_ISSET(i, ex) ) ev |= POLLPRI;
		if ( ev != 0 ) {
			p[n].fd = i;
			p[n].events = ev;
			n++;
		}
	}
	ms = ( tv == NULL ) ? -1 : (int)( tv->tv_sec * 1000 + tv->tv_usec / 1000 );
	r = poll(p, (nfds_t)n, ms);
	if ( r < 0 ) return -1;
	if ( rd != NULL ) FD_ZERO(rd);
	if ( wr != NULL ) FD_ZERO(wr);
	if ( ex != NULL ) FD_ZERO(ex);
	r = 0;
	for ( i = 0; i < n; i++ ) {
		if ( rd != NULL && ( p[i].revents & ( POLLIN | POLLHUP | POLLERR ) ) && ( p[i].events & POLLIN ) ) {
			FD_SET(p[i].fd, rd);
			r++;
		}
		if ( wr != NULL && ( p[i].revents & ( POLLOUT | POLLERR ) ) && ( p[i].events & POLLOUT ) ) {
			FD_SET(p[i].fd, wr);
			r++;
		}
		if ( ex != NULL && ( p[i].revents & POLLPRI ) ) {
			FD_SET(p[i].fd, ex);
			r++;
		}
	}
	return r;
}

/* ---------------------------------------------------------------- the rest */

/* The file's own writes on the medium (design 7.3); sync(2) is every volume's */
int fsync( int fd )
{
	PSXFD	*e = __psx_fd_get(fd);

	if ( e == NULL ) return -1;
	if ( e->kind == PK_FS && fs_fsync(e->kfd) < 0 ) {
		errno = EIO;
		return -1;
	}
	return 0;
}

int fdatasync( int fd )
{
	return fsync(fd);
}

int ftruncate( int fd, off_t len )
{
	PSXFD	*e = __psx_fd_get(fd);
	ER	er;

	if ( e == NULL ) return -1;
	if ( e->kind != PK_FS ) {
		errno = EINVAL;
		return -1;
	}
	er = fs_ftruncate(e->kfd, (UD)len);
	return ( er < 0 ) ? __psx_fail_fs(er) : 0;
}

ssize_t pread( int fd, void *buf, size_t n, off_t off )
{
	off_t	was = lseek(fd, 0, SEEK_CUR);
	int	r;

	if ( was < 0 || lseek(fd, off, SEEK_SET) < 0 ) return -1;
	r = _read(fd, buf, n);
	(void)lseek(fd, was, SEEK_SET);
	return r;
}

ssize_t pwrite( int fd, const void *buf, size_t n, off_t off )
{
	off_t	was = lseek(fd, 0, SEEK_CUR);
	int	r;

	if ( was < 0 || lseek(fd, off, SEEK_SET) < 0 ) return -1;
	r = _write(fd, buf, n);
	(void)lseek(fd, was, SEEK_SET);
	return r;
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

int getdtablesize( void )
{
	return PSX_FD_MAX;
}
