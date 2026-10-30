/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_bsd.c
 *	What the BSD commands call that newlib does not have (lib/libposix,
 *	design 5.8.3): a line read with fgetln and fgetwln, the C locale as
 *	an object, sleeping on a clock, the few names of sysctl a command
 *	asks, and kqueue, which answers that no event can be watched so that
 *	a command falls back to looking again now and then. Also getline,
 *	timegm, the system's log (written only when LOG_PERROR asks for
 *	it) and the calls on extended attributes and whiteouts, which
 *	objects do not have.
 */

#include "psx_int.h"
#include <ts/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <nl_types.h>
#include <paths.h>
#include <stdarg.h>
#include <syslog.h>
#include <util.h>
#include <utmpx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* ---------------------------------------------------------------- lines */

/*
 * The line of fgetln is kept per stream until its next call on the same
 * stream: a few streams' buffers at a time, the oldest given up.
 */
#define LN_KEEP	8

typedef struct {
	void	*fp;
	void	*buf;
	size_t	cap;
} LNBUF;

static LNBUF	ln_c[LN_KEEP], ln_w[LN_KEEP];

static LNBUF *ln_of( LNBUF *t, void *fp )
{
	int	i;

	for ( i = 0; i < LN_KEEP; i++ ) {
		if ( t[i].fp == fp ) return &t[i];
	}
	for ( i = 0; i < LN_KEEP; i++ ) {
		if ( t[i].fp == NULL ) break;
	}
	if ( i == LN_KEEP ) {
		i = (int)( (unsigned long)fp % LN_KEEP );
	}
	t[i].fp = fp;
	return &t[i];
}

char *fgetln( FILE *fp, size_t *len )
{
	LNBUF	*b = ln_of(ln_c, fp);
	size_t	n = 0;
	int	c;

	while ( ( c = getc(fp) ) != EOF ) {
		if ( n + 1 >= b->cap ) {
			size_t	cap = ( b->cap == 0 ) ? 256 : b->cap * 2;
			void	*nb = realloc(b->buf, cap);

			if ( nb == NULL ) return NULL;
			b->buf = nb;
			b->cap = cap;
		}
		((char *)b->buf)[n++] = (char)c;
		if ( c == '\n' ) break;
	}
	if ( n == 0 ) return NULL;
	*len = n;
	return (char *)b->buf;
}

wchar_t *fgetwln( FILE *fp, size_t *len )
{
	LNBUF	*b = ln_of(ln_w, fp);
	size_t	n = 0;
	wint_t	c;

	while ( ( c = getwc(fp) ) != WEOF ) {
		if ( ( n + 1 ) * sizeof(wchar_t) >= b->cap ) {
			size_t	cap = ( b->cap == 0 ) ? 1024 : b->cap * 2;
			void	*nb = realloc(b->buf, cap);

			if ( nb == NULL ) return NULL;
			b->buf = nb;
			b->cap = cap;
		}
		((wchar_t *)b->buf)[n++] = (wchar_t)c;
		if ( c == L'\n' ) break;
	}
	if ( n == 0 ) return NULL;
	*len = n;
	return (wchar_t *)b->buf;
}

/* ---------------------------------------------------------------- the C locale */

locale_t __psx_c_locale( void )
{
	static locale_t	c;

	if ( c == (locale_t)0 ) {
		c = newlocale(LC_ALL_MASK, "C", (locale_t)0);
	}
	return c;
}

/* ---------------------------------------------------------------- clocks */

int clock_nanosleep( clockid_t clk, int flags, const struct timespec *req, struct timespec *rem )
{
	struct timespec	now, d;

	if ( ( flags & TIMER_ABSTIME ) != 0 ) {
		clock_gettime(clk, &now);
		d.tv_sec = req->tv_sec - now.tv_sec;
		d.tv_nsec = req->tv_nsec - now.tv_nsec;
		if ( d.tv_nsec < 0 ) {
			d.tv_nsec += 1000000000L;
			d.tv_sec--;
		}
		if ( d.tv_sec < 0 ) return 0;
		return ( nanosleep(&d, NULL) < 0 ) ? errno : 0;
	}
	return ( nanosleep(req, rem) < 0 ) ? errno : 0;
}

/* ---------------------------------------------------------------- sysctl */

int sysctl( const int *name, unsigned int namelen, void *oldp, size_t *oldlenp,
	    const void *newp, size_t newlen )
{
	const char	*s = NULL;
	long		v = 0;
	int		isnum = 0;

	(void)newlen;
	if ( newp != NULL ) {
		errno = EPERM;
		return -1;
	}
	if ( namelen == 2 && name[0] == CTL_HW ) {
		switch ( name[1] ) {
		  case HW_MACHINE:
#if defined(__x86_64__)
		  case HW_MACHINE_ARCH:	s = "x86_64"; break;
#else
		  case HW_MACHINE_ARCH:	s = "aarch64"; break;
#endif
		  case HW_MODEL:	s = "TessronOS"; break;
		  case HW_NCPU:		v = sysconf(_SC_NPROCESSORS_ONLN); isnum = 1; break;
		  case HW_PAGESIZE:	v = 4096; isnum = 1; break;
		  default:		break;
		}
	} else if ( namelen == 2 && name[0] == CTL_KERN ) {
		switch ( name[1] ) {
		  case KERN_OSTYPE:	s = "TessronOS"; break;
		  case KERN_OSRELEASE:	s = "1.0"; break;
		  case KERN_HOSTNAME:	s = "tessronos"; break;
		  case KERN_ARGMAX:	v = TS_SPN_ARG_MAX; isnum = 1; break;
		  default:		break;
		}
	} else if ( namelen == 2 && name[0] == CTL_USER && name[1] == USER_CS_PATH ) {
		s = _PATH_STDPATH;
	}
	if ( s == NULL && !isnum ) {
		errno = ENOENT;
		return -1;
	}
	if ( isnum ) {
		if ( oldp != NULL ) {
			if ( *oldlenp == sizeof(int) ) {
				*(int *)oldp = (int)v;
			} else if ( *oldlenp >= sizeof(long) ) {
				*(long *)oldp = v;
				*oldlenp = sizeof(long);
			} else {
				errno = ENOMEM;
				return -1;
			}
		} else {
			*oldlenp = sizeof(long);
		}
		return 0;
	}
	if ( oldp != NULL ) {
		if ( *oldlenp < strlen(s) + 1 ) {
			errno = ENOMEM;
			return -1;
		}
		strcpy((char *)oldp, s);
	}
	*oldlenp = strlen(s) + 1;
	return 0;
}

int sysctlbyname( const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen )
{
	int	mib[2];

	if ( strcmp(name, "hw.machine_arch") == 0 ) { mib[0] = CTL_HW; mib[1] = HW_MACHINE_ARCH; }
	else if ( strcmp(name, "hw.machine") == 0 ) { mib[0] = CTL_HW; mib[1] = HW_MACHINE; }
	else if ( strcmp(name, "hw.ncpu") == 0 ) { mib[0] = CTL_HW; mib[1] = HW_NCPU; }
	else if ( strcmp(name, "kern.ostype") == 0 ) { mib[0] = CTL_KERN; mib[1] = KERN_OSTYPE; }
	else if ( strcmp(name, "kern.hostname") == 0 ) { mib[0] = CTL_KERN; mib[1] = KERN_HOSTNAME; }
	else if ( strcmp(name, "user.cs_path") == 0 ) { mib[0] = CTL_USER; mib[1] = USER_CS_PATH; }
	else {
		errno = ENOENT;
		return -1;
	}
	return sysctl(mib, 2, oldp, oldlenp, newp, newlen);
}

/* ---------------------------------------------------------------- kqueue */

int kqueue( void )
{
	return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int kqueue1( int flags )
{
	(void)flags;
	return kqueue();
}

int kevent( int kq, const struct kevent *ch, size_t nch, struct kevent *ev, size_t nev,
	    const struct timespec *t )
{
	(void)kq;
	(void)ch;
	(void)nch;
	(void)ev;
	(void)nev;
	(void)t;
	errno = ENOSYS;
	return -1;
}

/* ---------------------------------------------------------------- lines of any length */

extern ssize_t __getdelim( char **, size_t *, int, FILE * );

ssize_t getdelim( char **buf, size_t *cap, int delim, FILE *fp )
{
	return __getdelim(buf, cap, delim, fp);
}

ssize_t getline( char **buf, size_t *cap, FILE *fp )
{
	return __getdelim(buf, cap, '\n', fp);
}

/*
 * Formatting for a signal handler: newlib's vsnprintf into a buffer
 * touches no stream and takes no lock, which is what a handler needs of
 * it for the integers and strings the commands write this way.
 */
int vsnprintf_ss( char *buf, size_t len, const char *fmt, va_list ap )
{
	return vsnprintf(buf, len, fmt, ap);
}

int snprintf_ss( char *buf, size_t len, const char *fmt, ... )
{
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	return n;
}

/* ---------------------------------------------------------------- streams of a program's own */

/* funopen2 on newlib's funopen: the functions and the cookie kept together */
typedef struct {
	void	*cookie;
	ssize_t	(*rd)( void *, void *, size_t );
	ssize_t	(*wr)( void *, const void *, size_t );
	off_t	(*sk)( void *, off_t, int );
	int	(*fl)( void * );
	int	(*cl)( void * );
} FUN2;

static int fun2_read( void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n )
{
	FUN2	*f = (FUN2 *)c;

	return (int)f->rd(f->cookie, buf, (size_t)n);
}

static int fun2_write( void *c, const char *buf, _READ_WRITE_BUFSIZE_TYPE n )
{
	FUN2	*f = (FUN2 *)c;

	return (int)f->wr(f->cookie, buf, (size_t)n);
}

static fpos_t fun2_seek( void *c, fpos_t off, int whence )
{
	FUN2	*f = (FUN2 *)c;

	return (fpos_t)f->sk(f->cookie, (off_t)off, whence);
}

static int fun2_close( void *c )
{
	FUN2	*f = (FUN2 *)c;
	int	r = 0;

	if ( f->fl != NULL ) (void)f->fl(f->cookie);
	if ( f->cl != NULL ) r = f->cl(f->cookie);
	free(f);
	return r;
}

FILE *funopen2( const void *cookie, ssize_t (*rd)( void *, void *, size_t ),
		ssize_t (*wr)( void *, const void *, size_t ), off_t (*sk)( void *, off_t, int ),
		int (*fl)( void * ), int (*cl)( void * ) )
{
	FUN2	*f = (FUN2 *)malloc(sizeof(FUN2));
	FILE	*fp;

	if ( f == NULL ) return NULL;
	f->cookie = (void *)(unsigned long)cookie;
	f->rd = rd;
	f->wr = wr;
	f->sk = sk;
	f->fl = fl;
	f->cl = cl;
	fp = funopen(f, ( rd != NULL ) ? fun2_read : NULL, ( wr != NULL ) ? fun2_write : NULL,
		     ( sk != NULL ) ? fun2_seek : NULL, fun2_close);
	if ( fp == NULL ) free(f);
	return fp;
}

/* ---------------------------------------------------------------- dates */

static int leap( long long y )
{
	return ( y % 4 == 0 && y % 100 != 0 ) || y % 400 == 0;
}

/* The seconds since 1970-01-01 00:00 UTC of a broken-down UTC date, normalized as mktime does */
time_t timegm( struct tm *tm )
{
	static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	long long	y, m, days, secs;
	struct tm	*n;
	time_t		t;
	int		i;

	m = tm->tm_mon;
	y = (long long)tm->tm_year + 1900 + m / 12;
	m %= 12;
	if ( m < 0 ) {
		m += 12;
		y--;
	}
	days = 0;
	if ( y >= 1970 ) {
		for ( i = 1970; i < y; i++ ) days += 365 + leap(i);
	} else {
		for ( i = (int)y; i < 1970; i++ ) days -= 365 + leap(i);
	}
	for ( i = 0; i < m; i++ ) days += mdays[i] + ( i == 1 && leap(y) );
	days += tm->tm_mday - 1;
	secs = days * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
	t = (time_t)secs;
	n = gmtime(&t);
	if ( n != NULL ) *tm = *n;
	return t;
}

time_t timelocal( struct tm *tm )
{
	return mktime(tm);
}

struct __psx_tz {
	int	utc;
};

timezone_t tzalloc( const char *name )
{
	struct __psx_tz *z = (struct __psx_tz *)malloc(sizeof(*z));

	if ( z == NULL ) return NULL;
	z->utc = ( name != NULL && ( strcmp(name, "UTC") == 0 || strcmp(name, "GMT") == 0
				 || strcmp(name, "UTC0") == 0 ) );
	return z;
}

void tzfree( timezone_t z )
{
	free(z);
}

struct tm *localtime_rz( timezone_t z, const time_t *t, struct tm *tm )
{
	return ( z == NULL || z->utc ) ? gmtime_r(t, tm) : localtime_r(t, tm);
}

time_t mktime_z( timezone_t z, struct tm *tm )
{
	return ( z == NULL || z->utc ) ? timegm(tm) : mktime(tm);
}

/* The broken-down time is the zone's already: only %Z and %z would tell it, as TZ's */
size_t strftime_z( timezone_t z, char *buf, size_t max, const char *fmt, const struct tm *tm )
{
	(void)z;
	return strftime(buf, max, fmt, tm);
}

/* ---------------------------------------------------------------- the times of a file */

int utimens( const char *path, const struct timespec ts[2] )
{
	return utimensat(AT_FDCWD, path, ts, 0);
}

int lutimens( const char *path, const struct timespec ts[2] )
{
	return utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

/* ---------------------------------------------------------------- what objects do not have */

int fcpxattr( int from, int to )
{
	(void)from;
	(void)to;
	return 0;
}

int undelete( const char *path )
{
	(void)path;
	errno = EOPNOTSUPP;
	return -1;
}

/* The top of the tree of a program is the system's "/" alone */
int chroot( const char *path )
{
	(void)path;
	errno = EPERM;
	return -1;
}

int fchroot( int fd )
{
	(void)fd;
	errno = EPERM;
	return -1;
}

void setproctitle( const char *fmt, ... )
{
	(void)fmt;
}

/* A program's promise of the calls and paths it will use: taken, not enforced */
int pledge( const char *promises, const char *execpromises )
{
	(void)promises;
	(void)execpromises;
	return 0;
}

int unveil( const char *path, const char *perm )
{
	(void)path;
	(void)perm;
	return 0;
}

/* ---------------------------------------------------------------- catalogues of messages */

nl_catd catopen( const char *name, int flag )
{
	(void)name;
	(void)flag;
	errno = ENOENT;
	return (nl_catd)-1;
}

char *catgets( nl_catd cat, int set, int num, const char *s )
{
	(void)cat;
	(void)set;
	(void)num;
	return (char *)s;
}

int catclose( nl_catd cat )
{
	(void)cat;
	return 0;
}

/* ---------------------------------------------------------------- the records of logins */

void setutxent( void )			{ }
void endutxent( void )			{ }
struct utmpx *getutxent( void )		{ return NULL; }
struct utmpx *getutxid( const struct utmpx *u )	{ (void)u; return NULL; }
struct utmpx *getutxline( const struct utmpx *u ) { (void)u; return NULL; }
struct utmpx *pututxline( const struct utmpx *u ) { return (struct utmpx *)u; }
int utmpxname( const char *name )	{ (void)name; return 0; }

void logwtmp( const char *line, const char *name, const char *host )
{
	(void)line;
	(void)name;
	(void)host;
}

int logout( const char *line )
{
	(void)line;
	return 0;
}

int logoutx( const char *line, int status, int type )
{
	(void)line;
	(void)status;
	(void)type;
	return 0;
}

/* ---------------------------------------------------------------- the system's log */

static const char *log_ident;
static int	log_opt;
static int	log_mask = 0xff;

void openlog( const char *ident, int opt, int facility )
{
	(void)facility;
	log_ident = ident;
	log_opt = opt;
}

void closelog( void )
{
	log_ident = NULL;
	log_opt = 0;
}

int setlogmask( int mask )
{
	int	old = log_mask;

	if ( mask != 0 ) log_mask = mask;
	return old;
}

void vsyslog( int pri, const char *fmt, va_list ap )
{
	if ( ( log_mask & LOG_MASK(pri & LOG_PRIMASK) ) == 0 ) return;
	if ( ( log_opt & LOG_PERROR ) == 0 ) return;
	fprintf(stderr, "%s: ", ( log_ident != NULL ) ? log_ident : getprogname());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void syslog( int pri, const char *fmt, ... )
{
	va_list	ap;

	va_start(ap, fmt);
	vsyslog(pri, fmt, ap);
	va_end(ap);
}
