/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_posix.cc
 *	The POSIX gaps of newlib, filled for the conversion engine
 *
 *	The engine runs in one task. Threads are therefore one thread: a
 *	mutex is always free, a key has one value, a new thread cannot be
 *	made. Time and sleeping, and making a directory, are asked of the
 *	system through the tsmozc_host_* functions that application/kconv
 *	supplies; the files themselves go through newlib's system calls,
 *	which it supplies too.
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>

#include <sys/time.h>
#include <sys/times.h>
#include "ts/tsmozc.h"

extern "C" {

/* ---------------------------------------------------------------- newlib's system calls */

/*
 * The descriptors 0 to 2 are the console; a file's descriptor is the
 * system's, moved past them.
 */
#define TS_FD_BASE	3

void *_sbrk(ptrdiff_t incr)
{
	void *p = tsmozc_host_sbrk(static_cast<long>(incr));

	if ( p == reinterpret_cast<void *>(-1) ) {
		errno = ENOMEM;
	}
	return p;
}

int _open(const char *path, int flags, int)
{
	int	f = 0, fd;

	if ( ( flags & O_ACCMODE ) != O_RDONLY ) f |= TSMOZC_O_WRITE;
	if ( flags & O_CREAT )	f |= TSMOZC_O_CREATE;
	if ( flags & O_TRUNC )	f |= TSMOZC_O_TRUNC;
	if ( flags & O_APPEND )	f |= TSMOZC_O_APPEND;
	if ( flags & O_EXCL )	f |= TSMOZC_O_EXCL;
	fd = tsmozc_host_open(path, f);
	if ( fd < 0 ) {
		errno = ENOENT;
		return -1;
	}
	return fd + TS_FD_BASE;
}

int _close(int fd)
{
	return ( fd < TS_FD_BASE ) ? 0 : ( tsmozc_host_close(fd - TS_FD_BASE) < 0 ? -1 : 0 );
}

int _read(int fd, void *buf, size_t len)
{
	int	n;

	if ( fd < TS_FD_BASE ) {
		return 0;
	}
	n = tsmozc_host_read(fd - TS_FD_BASE, buf, static_cast<int>(len));
	if ( n < 0 ) {
		errno = EIO;
		return -1;
	}
	return n;
}

int _write(int fd, const void *buf, size_t len)
{
	int	n;

	if ( fd < TS_FD_BASE ) {
		tsmozc_host_log(static_cast<const char *>(buf), static_cast<int>(len));
		return static_cast<int>(len);
	}
	n = tsmozc_host_write(fd - TS_FD_BASE, buf, static_cast<int>(len));
	if ( n < 0 ) {
		errno = EIO;
		return -1;
	}
	return n;
}

off_t _lseek(int fd, off_t off, int whence)
{
	long long r;

	if ( fd < TS_FD_BASE ) {
		return 0;
	}
	r = tsmozc_host_lseek(fd - TS_FD_BASE, off, whence);
	if ( r < 0 ) {
		errno = EINVAL;
		return -1;
	}
	return static_cast<off_t>(r);
}

static void ts_fill_stat(struct stat *st, long long size, int dir, long long mtime)
{
	std::memset(st, 0, sizeof(*st));
	st->st_mode = dir ? ( S_IFDIR | 0755 ) : ( S_IFREG | 0644 );
	st->st_size = static_cast<off_t>(size);
	st->st_nlink = 1;
	st->st_blksize = 4096;
	st->st_mtim.tv_sec = static_cast<time_t>(mtime);
	st->st_atim = st->st_mtim;
	st->st_ctim = st->st_mtim;
}

int _fstat(int fd, struct stat *st)
{
	long long	size, mtime;
	int		dir;

	if ( fd < TS_FD_BASE ) {
		std::memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR;
		return 0;
	}
	if ( tsmozc_host_fstat(fd - TS_FD_BASE, &size, &dir, &mtime) < 0 ) {
		errno = EBADF;
		return -1;
	}
	ts_fill_stat(st, size, dir, mtime);
	return 0;
}

int _stat(const char *path, struct stat *st)
{
	long long	size, mtime;
	int		dir;

	if ( tsmozc_host_stat(path, &size, &dir, &mtime) < 0 ) {
		errno = ENOENT;
		return -1;
	}
	ts_fill_stat(st, size, dir, mtime);
	return 0;
}

int _isatty(int fd)
{
	return fd < TS_FD_BASE;
}

int _unlink(const char *path)
{
	if ( tsmozc_host_unlink(path) < 0 ) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

int _link(const char *, const char *)
{
	errno = EMLINK;
	return -1;
}

int rename(const char *from, const char *to)
{
	if ( tsmozc_host_rename(from, to) < 0 ) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

int _getpid(void)
{
	return 1;
}

int _kill(int, int)
{
	tsmozc_host_exit(134);
	return -1;
}

void _exit(int code)
{
	tsmozc_host_exit(code);
	for ( ;; ) {
	}
}

int _gettimeofday(struct timeval *tv, void *)
{
	long long ns = tsmozc_host_clock(0);

	tv->tv_sec = static_cast<time_t>(ns / 1000000000LL);
	tv->tv_usec = static_cast<suseconds_t>(( ns % 1000000000LL ) / 1000);
	return 0;
}

clock_t _times(struct tms *buf)
{
	long long ns = tsmozc_host_clock(1);

	if ( buf != nullptr ) {
		std::memset(buf, 0, sizeof(*buf));
		buf->tms_utime = static_cast<clock_t>(ns / 1000000LL);
	}
	return static_cast<clock_t>(ns / 1000000LL);
}

/* ---------------------------------------------------------------- threads: one */

pthread_t pthread_self(void)
{
	return 1;
}

int pthread_equal(pthread_t a, pthread_t b)
{
	return a == b;
}

int pthread_once(pthread_once_t *once, void (*init)(void))
{
	if ( once->init_executed == 0 ) {
		once->init_executed = 1;
		init();
	}
	return 0;
}

#define TS_KEYS	64

static const void	*ts_key_value[TS_KEYS];
static unsigned char	ts_key_used[TS_KEYS];

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	(void)destructor;
	for ( int i = 0; i < TS_KEYS; i++ ) {
		if ( !ts_key_used[i] ) {
			ts_key_used[i] = 1;
			ts_key_value[i] = nullptr;
			*key = static_cast<pthread_key_t>(i);
			return 0;
		}
	}
	return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
	if ( key >= TS_KEYS ) {
		return EINVAL;
	}
	ts_key_used[key] = 0;
	return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
	return ( key < TS_KEYS ) ? const_cast<void *>(ts_key_value[key]) : nullptr;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
	if ( key >= TS_KEYS ) {
		return EINVAL;
	}
	ts_key_value[key] = value;
	return 0;
}

int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *) { return 0; }
int pthread_mutex_destroy(pthread_mutex_t *) { return 0; }
int pthread_mutex_lock(pthread_mutex_t *) { return 0; }
int pthread_mutex_trylock(pthread_mutex_t *) { return 0; }
int pthread_mutex_unlock(pthread_mutex_t *) { return 0; }
int pthread_mutexattr_init(pthread_mutexattr_t *) { return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *) { return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *, int) { return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *type)
{
	*type = 0;
	return 0;
}

int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *) { return 0; }
int pthread_cond_destroy(pthread_cond_t *) { return 0; }
int pthread_cond_signal(pthread_cond_t *) { return 0; }
int pthread_cond_broadcast(pthread_cond_t *) { return 0; }
int pthread_condattr_init(pthread_condattr_t *) { return 0; }
int pthread_condattr_destroy(pthread_condattr_t *) { return 0; }
int pthread_condattr_setclock(pthread_condattr_t *, clockid_t) { return 0; }

/*
 * A wait with one thread is a wait for nobody. It is only reached when
 * the engine blocks on itself, which is a fault to be seen, not hidden.
 */
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *)
{
	abort();
}

int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
			   const struct timespec *)
{
	return ETIMEDOUT;
}

int pthread_attr_init(pthread_attr_t *) { return 0; }
int pthread_attr_destroy(pthread_attr_t *) { return 0; }
int pthread_attr_setstacksize(pthread_attr_t *, size_t) { return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *, int) { return 0; }

int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *)
{
	return EAGAIN;
}

int pthread_join(pthread_t, void **) { return ESRCH; }
int pthread_detach(pthread_t) { return ESRCH; }

int pthread_getschedparam(pthread_t, int *policy, struct sched_param *param)
{
	*policy = 0;
	std::memset(param, 0, sizeof(*param));
	return 0;
}

int pthread_sigmask(int, const sigset_t *, sigset_t *oldset)
{
	if ( oldset != nullptr ) {
		*oldset = 0;
	}
	return 0;
}

int sched_yield(void)
{
	return 0;
}

/* ---------------------------------------------------------------- semaphores */

int sem_init(sem_t *sem, int, unsigned int value)
{
	sem->count = static_cast<int>(value);
	return 0;
}

int sem_destroy(sem_t *) { return 0; }

int sem_post(sem_t *sem)
{
	sem->count++;
	return 0;
}

int sem_trywait(sem_t *sem)
{
	if ( sem->count <= 0 ) {
		errno = EAGAIN;
		return -1;
	}
	sem->count--;
	return 0;
}

int sem_wait(sem_t *sem)
{
	if ( sem_trywait(sem) != 0 ) {
		abort();		/* nobody else will post */
	}
	return 0;
}

int sem_timedwait(sem_t *sem, const struct timespec *)
{
	if ( sem_trywait(sem) != 0 ) {
		errno = ETIMEDOUT;
		return -1;
	}
	return 0;
}

int sem_getvalue(sem_t *sem, int *sval)
{
	*sval = sem->count;
	return 0;
}

/* ---------------------------------------------------------------- time */

int clock_gettime(clockid_t id, struct timespec *tp)
{
	long long ns = tsmozc_host_clock(id != CLOCK_REALTIME);

	tp->tv_sec = static_cast<time_t>(ns / 1000000000LL);
	tp->tv_nsec = static_cast<long>(ns % 1000000000LL);
	return 0;
}

int clock_getres(clockid_t, struct timespec *res)
{
	if ( res != nullptr ) {
		res->tv_sec = 0;
		res->tv_nsec = 1000000;
	}
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	tsmozc_host_sleep(static_cast<long long>(req->tv_sec) * 1000000000LL + req->tv_nsec);
	if ( rem != nullptr ) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}

int usleep(useconds_t usec)
{
	tsmozc_host_sleep(static_cast<long long>(usec) * 1000LL);
	return 0;
}

/* ---------------------------------------------------------------- the machine */

long sysconf(int name)
{
	switch ( name ) {
	case _SC_PAGESIZE:		return 4096;
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF:	return 1;
	case _SC_GETPW_R_SIZE_MAX:	return 1024;
	default:			errno = EINVAL; return -1;
	}
}

int getpagesize(void)
{
	return 4096;
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }

int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **result)
{
	*result = nullptr;
	return ENOENT;
}

/* What abseil seeds its generators with: the cycle counter, stirred */
int btron_os_entropy(void *buf, unsigned int len)
{
	unsigned char	*out = static_cast<unsigned char *>(buf);
	unsigned int	made = 0;

	for ( unsigned int i = 0; made < len; i++ ) {
		unsigned long	cnt;
		unsigned int	v;

		__asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
		v = static_cast<unsigned int>(cnt) ^ static_cast<unsigned int>(cnt >> 32)
		  ^ ( static_cast<unsigned int>(reinterpret_cast<unsigned long>(&cnt)) * 2246822519u )
		  ^ ( i * 668265263u );
		v ^= v >> 15;  v *= 2246822519u;
		v ^= v >> 13;  v *= 3266489917u;
		v ^= v >> 16;
		for ( unsigned int b = 0; b < sizeof(v) && made < len; b++ ) {
			out[made++] = static_cast<unsigned char>(v >> ( 8 * b ));
		}
	}
	return 0;
}

int ts_os_entropy(void *buf, unsigned int len)
{
	return btron_os_entropy(buf, len);
}

/* A romaji table kept in a real object: there are none to read here */
int btron_read_object(const void *, int, char *, int)
{
	return -1;
}

/* ---------------------------------------------------------------- mappings */

void *mmap(void *, size_t length, int, int, int fd, off_t offset)
{
	/*
	 * Memory read from the file. Anonymous memory arrives zeroed, as
	 * POSIX says; a file's is overwritten by what is read.
	 */
	void *p = ( fd < 0 ) ? std::calloc(1, length) : std::malloc(length);

	if ( p == nullptr ) {
		return MAP_FAILED;
	}
	if ( fd >= 0 ) {
		size_t	done = 0;
		char	*dst = static_cast<char *>(p);

		lseek(fd, offset, SEEK_SET);
		while ( done < length ) {
			ssize_t n = read(fd, dst + done, length - done);

			if ( n <= 0 ) {
				break;
			}
			done += static_cast<size_t>(n);
		}
	}
	return p;
}

int munmap(void *addr, size_t)
{
	std::free(addr);
	return 0;
}

int mprotect(void *, size_t, int) { return 0; }
int msync(void *, size_t, int) { return 0; }
int mlock(const void *, size_t) { return 0; }
int munlock(const void *, size_t) { return 0; }
int madvise(void *, size_t, int) { return 0; }

/* ---------------------------------------------------------------- files the engine does not list */

DIR *opendir(const char *)
{
	errno = ENOSYS;
	return nullptr;
}

struct dirent *readdir(DIR *)
{
	return nullptr;
}

int closedir(DIR *)
{
	return 0;
}

int mkdir(const char *path, mode_t)
{
	if ( tsmozc_host_mkdir(path) < 0 ) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

int rmdir(const char *)
{
	errno = ENOSYS;
	return -1;
}

int access(const char *path, int)
{
	struct stat st;

	return stat(path, &st);
}

int mkstemp(char *)
{
	errno = ENOSYS;
	return -1;
}

char *mkdtemp(char *)
{
	errno = ENOSYS;
	return nullptr;
}

int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *),
	 int, int)
{
	errno = ENOSYS;
	return -1;
}

char *strptime(const char *, const char *, struct tm *)
{
	return nullptr;
}

int chmod(const char *, mode_t)
{
	return 0;
}

int _getentropy(void *buf, size_t len)
{
	return btron_os_entropy(buf, static_cast<unsigned int>(len));
}

/* ---------------------------------------------------------------- the C++ runtime's ends */

/*
 * What crtbegin and crti would give a program: the handle its static
 * destructors are registered under, and the end of its life. The engine
 * is never unloaded, so nothing registered here is run.
 */
void *__dso_handle = &__dso_handle;

void _fini(void)
{
}

}  // extern "C"

/* abseil's fatal-error path prints a stack; there is no unwinder to walk it */
#include "absl/debugging/internal/examine_stack.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

void DumpStackTrace(int, int, bool, OutputWriter *, void *)
{
}

}  // namespace debugging_internal
ABSL_NAMESPACE_END
}  // namespace absl
