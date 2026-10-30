/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_user.c
 *	Who the program acts for, and the machine (lib/libposix, design
 *	5.8.1)
 *
 *	A user and a group are objects (design 18.10); the numbers POSIX
 *	shows for them come from include/ts/psxid.h, as the kernel's stat
 *	gives them. The fixed ones have names a command can print (system,
 *	admin, user, users, wheel); another is named by its object when it
 *	is the one the program acts for, by its number otherwise. Every
 *	user's home is /home and its shell /bin/tsh.
 */

#include "psx_int.h"
#include <ts/ob.h>
#include <ts/psxid.h>
#include <ts/sysdef.h>
#include <ts/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <pwd.h>
#include <grp.h>
#include <paths.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static T_OBCRD	crd;
static int	crd_ok;

static const T_OBCRD *me( void )
{
	if ( !crd_ok && ob_get_crd(&crd) >= E_OK ) {
		crd_ok = 1;
	}
	return crd_ok ? &crd : NULL;
}

uid_t getuid( void )
{
	const T_OBCRD *c = me();

	return ( c != NULL ) ? (uid_t)psx_id_of(&c->user) : 0;
}

uid_t geteuid( void )	{ return getuid(); }

gid_t getgid( void )
{
	const T_OBCRD *c = me();

	return ( c != NULL && c->ngrp > 0 ) ? (gid_t)psx_id_of(&c->grp[0]) : 0;
}

gid_t getegid( void )	{ return getgid(); }

int getgroups( int n, gid_t *list )
{
	const T_OBCRD *c = me();
	int	i, k;

	if ( c == NULL ) return 0;
	k = c->ngrp;
	if ( n == 0 ) return k;
	if ( n < k ) {
		errno = EINVAL;
		return -1;
	}
	for ( i = 0; i < k; i++ ) list[i] = (gid_t)psx_id_of(&c->grp[i]);
	return k;
}

int setuid( uid_t u )	{ if ( u == getuid() ) return 0; errno = EPERM; return -1; }
int seteuid( uid_t u )	{ return setuid(u); }
int setgid( gid_t g )	{ if ( g == getgid() ) return 0; errno = EPERM; return -1; }
int setegid( gid_t g )	{ return setgid(g); }
int setgroups( int n, const gid_t *g ) { (void)n; (void)g; errno = EPERM; return -1; }

int setreuid( uid_t r, uid_t e )
{
	if ( ( r == (uid_t)-1 || r == getuid() ) && ( e == (uid_t)-1 || e == getuid() ) ) return 0;
	errno = EPERM;
	return -1;
}

int setregid( gid_t r, gid_t e )
{
	if ( ( r == (gid_t)-1 || r == getgid() ) && ( e == (gid_t)-1 || e == getgid() ) ) return 0;
	errno = EPERM;
	return -1;
}
int issetugid( void )	{ return 0; }

/* ---------------------------------------------------------------- names */

static const struct {
	unsigned int	id;
	const char	*name;
	const char	*gecos;
} users[] = {
	{ PSX_UID_SYSTEM,	"system",	"システム" },
	{ PSX_UID_ADMIN,	"admin",	"管理者" },
	{ PSX_UID_PERSON,	"user",		"利用者" },
}, groups[] = {
	{ PSX_GID_ADMIN,	"wheel",	"管理者" },
	{ PSX_GID_USERS,	"users",	"利用者" },
};

#define NUSERS	( sizeof(users) / sizeof(users[0]) )
#define NGROUPS_	( sizeof(groups) / sizeof(groups[0]) )

static struct passwd	pw;
static char		pw_name[OB_NAME_MAX + 8], pw_gecos[OB_NAME_MAX + 8];
static struct group	gr;
static char		gr_name[OB_NAME_MAX + 8];
static char		*gr_mem[2];

/* The name of the user the program acts for, as its object has it */
static int own_name( char *buf, size_t max )
{
	const T_OBCRD	*c = me();
	T_OBREF		r;

	if ( c == NULL || ob_ref_obj(&c->user, &r) < E_OK || r.name[0] == 0 ) return -1;
	strlcpy(buf, (const char *)r.name, max);
	return 0;
}

static struct passwd *pw_fill( uid_t uid, const char *name, const char *gecos )
{
	strlcpy(pw_name, name, sizeof(pw_name));
	strlcpy(pw_gecos, gecos, sizeof(pw_gecos));
	memset(&pw, 0, sizeof(pw));
	pw.pw_name = pw_name;
	pw.pw_passwd = (char *)"*";
	pw.pw_uid = uid;
	pw.pw_gid = ( uid == PSX_UID_SYSTEM || uid == PSX_UID_ADMIN ) ? PSX_GID_ADMIN : PSX_GID_USERS;
	pw.pw_gecos = pw_gecos;
	pw.pw_dir = (char *)_PATH_HOME;
	pw.pw_shell = (char *)"/bin/tsh";
	return &pw;
}

struct passwd *getpwuid( uid_t uid )
{
	char	b[OB_NAME_MAX + 8];
	size_t	i;

	for ( i = 0; i < NUSERS; i++ ) {
		if ( users[i].id == uid ) return pw_fill(uid, users[i].name, users[i].gecos);
	}
	if ( uid == getuid() && own_name(b, sizeof(b)) == 0 ) {
		return pw_fill(uid, b, b);
	}
	snprintf(b, sizeof(b), "u%u", (unsigned int)uid);
	return pw_fill(uid, b, b);
}

struct passwd *getpwnam( const char *name )
{
	char	b[OB_NAME_MAX + 8];
	size_t	i;

	for ( i = 0; i < NUSERS; i++ ) {
		if ( strcmp(users[i].name, name) == 0 ) return pw_fill(users[i].id, users[i].name, users[i].gecos);
	}
	if ( own_name(b, sizeof(b)) == 0 && strcmp(b, name) == 0 ) {
		return pw_fill(getuid(), b, b);
	}
	if ( name[0] == 'u' && name[1] >= '0' && name[1] <= '9' ) {
		return getpwuid((uid_t)strtoul(name + 1, NULL, 10));
	}
	return NULL;
}

int getpwuid_r( uid_t uid, struct passwd *p, char *buf, size_t len, struct passwd **res )
{
	struct passwd *r = getpwuid(uid);

	(void)buf;
	(void)len;
	*p = *r;
	*res = p;
	return 0;
}

int getpwnam_r( const char *name, struct passwd *p, char *buf, size_t len, struct passwd **res )
{
	struct passwd *r = getpwnam(name);

	(void)buf;
	(void)len;
	*res = NULL;
	if ( r == NULL ) return 0;
	*p = *r;
	*res = p;
	return 0;
}

static int pw_at;
void setpwent( void )		{ pw_at = 0; }
void endpwent( void )		{ pw_at = 0; }
struct passwd *getpwent( void )
{
	if ( pw_at >= (int)NUSERS ) return NULL;
	pw_at++;
	return pw_fill(users[pw_at - 1].id, users[pw_at - 1].name, users[pw_at - 1].gecos);
}

static struct group *gr_fill( gid_t gid, const char *name )
{
	strlcpy(gr_name, name, sizeof(gr_name));
	memset(&gr, 0, sizeof(gr));
	gr.gr_name = gr_name;
	gr.gr_passwd = (char *)"*";
	gr.gr_gid = gid;
	gr_mem[0] = NULL;
	gr.gr_mem = gr_mem;
	return &gr;
}

struct group *getgrgid( gid_t gid )
{
	char	b[24];
	size_t	i;

	for ( i = 0; i < NGROUPS_; i++ ) {
		if ( groups[i].id == gid ) return gr_fill(gid, groups[i].name);
	}
	snprintf(b, sizeof(b), "g%u", (unsigned int)gid);
	return gr_fill(gid, b);
}

struct group *getgrnam( const char *name )
{
	size_t	i;

	for ( i = 0; i < NGROUPS_; i++ ) {
		if ( strcmp(groups[i].name, name) == 0 ) return gr_fill(groups[i].id, groups[i].name);
	}
	if ( name[0] == 'g' && name[1] >= '0' && name[1] <= '9' ) {
		return getgrgid((gid_t)strtoul(name + 1, NULL, 10));
	}
	return NULL;
}

int getgrgid_r( gid_t gid, struct group *g, char *buf, size_t len, struct group **res )
{
	(void)buf;
	(void)len;
	*g = *getgrgid(gid);
	*res = g;
	return 0;
}

int getgrnam_r( const char *name, struct group *g, char *buf, size_t len, struct group **res )
{
	struct group *r = getgrnam(name);

	(void)buf;
	(void)len;
	*res = NULL;
	if ( r == NULL ) return 0;
	*g = *r;
	*res = g;
	return 0;
}

static int gr_at;
void setgrent( void )		{ gr_at = 0; }
void endgrent( void )		{ gr_at = 0; }
struct group *getgrent( void )
{
	if ( gr_at >= (int)NGROUPS_ ) return NULL;
	gr_at++;
	return gr_fill(groups[gr_at - 1].id, groups[gr_at - 1].name);
}

int getgrouplist( const char *name, gid_t base, gid_t *list, int *n )
{
	(void)name;
	if ( *n < 1 ) {
		*n = 1;
		return -1;
	}
	list[0] = base;
	*n = 1;
	return 0;
}

int initgroups( const char *name, gid_t base )
{
	(void)name;
	(void)base;
	return 0;
}

const char *user_from_uid( uid_t uid, int nouser )
{
	static char	b[OB_NAME_MAX + 8];
	struct passwd	*p = getpwuid(uid);

	if ( p == NULL || nouser ) {
		snprintf(b, sizeof(b), "%u", (unsigned int)uid);
		return ( p == NULL && nouser ) ? NULL : b;
	}
	strlcpy(b, p->pw_name, sizeof(b));
	return b;
}

const char *group_from_gid( gid_t gid, int nogroup )
{
	static char	b[OB_NAME_MAX + 8];
	struct group	*g = getgrgid(gid);

	if ( g == NULL ) {
		if ( nogroup ) return NULL;
		snprintf(b, sizeof(b), "%u", (unsigned int)gid);
		return b;
	}
	strlcpy(b, g->gr_name, sizeof(b));
	return b;
}

int uid_from_user( const char *name, uid_t *uid )
{
	struct passwd *p = getpwnam(name);

	if ( p == NULL ) return -1;
	*uid = p->pw_uid;
	return 0;
}

int gid_from_group( const char *name, gid_t *gid )
{
	struct group *g = getgrnam(name);

	if ( g == NULL ) return -1;
	*gid = g->gr_gid;
	return 0;
}

char *getlogin( void )
{
	struct passwd *p = getpwuid(getuid());

	return ( p != NULL ) ? p->pw_name : NULL;
}

int getlogin_r( char *buf, size_t len )
{
	char	*n = getlogin();

	if ( n == NULL ) return ENOENT;
	if ( strlen(n) >= len ) return ERANGE;
	strcpy(buf, n);
	return 0;
}

int setlogin( const char *name )
{
	(void)name;
	errno = EPERM;
	return -1;
}

static int shell_at;
char *getusershell( void )
{
	static const char *const sh[] = { "/bin/tsh", "/bin/sh", NULL };

	return ( sh[shell_at] != NULL ) ? (char *)sh[shell_at++] : NULL;
}
void setusershell( void )	{ shell_at = 0; }
void endusershell( void )	{ shell_at = 0; }

/* ---------------------------------------------------------------- the machine */

/*
 * What the machine is, as the system object (SYSDEF_SYSTEM) says it:
 * its records are lines of a key, a tab and the value. The value of key
 * in record recno into out; -1 when the object cannot be read or has no
 * such line.
 */
static int hex_of( char c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0'
	     : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

static int sys_uuid( TS_UUID *u )
{
	const char	*s = SYSDEF_SYSTEM;
	int		i = 0, h, l;

	while ( *s != 0 && i < 16 ) {
		if ( *s == '-' ) {
			s++;
			continue;
		}
		h = hex_of(s[0]);
		l = ( h >= 0 ) ? hex_of(s[1]) : -1;
		if ( l < 0 ) return -1;
		u->b[i++] = (UB)( h * 16 + l );
		s += 2;
	}
	return ( i == 16 ) ? 0 : -1;
}

static int sys_line( int recno, const char *key, char *out, size_t max )
{
	TS_UUID	u;
	ID	k;
	char	*b = NULL, *nb, *p, *e;
	SZ	room = 1024, got = 0;
	size_t	kl = strlen(key), n;
	int	found = -1;

	if ( max == 0 || sys_uuid(&u) < 0 || ( k = ob_opn_obj(&u, OB_OP_READ) ) <= 0 ) return -1;
	for ( ;; ) {
		nb = (char *)realloc(b, (size_t)room + 1);
		if ( nb == NULL ) break;
		b = nb;
		got = 0;
		if ( ob_rea_rec(k, recno, 0, b, room, &got) < E_OK ) {
			got = -1;
			break;
		}
		if ( got < room ) break;
		room *= 2;			/* the whole record, however long */
	}
	ob_cls_obj(k);
	if ( b != NULL && got >= 0 && got <= room ) {
		b[got] = 0;
		for ( p = b; *p != 0 && found < 0; p = ( *e != 0 ) ? e + 1 : e ) {
			for ( e = p; *e != 0 && *e != '\n'; e++ ) ;
			if ( (size_t)( e - p ) > kl && strncmp(p, key, kl) == 0 && p[kl] == '\t' ) {
				n = (size_t)( e - ( p + kl + 1 ) );
				if ( n >= max ) n = max - 1;
				memcpy(out, p + kl + 1, n);
				out[n] = 0;
				found = 0;
			}
		}
	}
	free(b);
	return found;
}

/* The network's name for the machine; the stack's own default when the system object says none */
int gethostname( char *name, size_t len )
{
	if ( sys_line(OB_SYS_NET, "HOSTNAME", name, len) < 0 || name[0] == 0 ) {
		strlcpy(name, "tessronos", len);
	}
	return 0;
}

int sethostname( const char *name, size_t len )
{
	(void)name;
	(void)len;
	errno = EPERM;
	return -1;
}

int uname( struct utsname *u )
{
	memset(u, 0, sizeof(*u));
	strcpy(u->sysname, "TessronOS");
	(void)gethostname(u->nodename, sizeof(u->nodename));
	{
		char	ver[sizeof(u->release)], knl[sizeof(u->version) / 2];

		if ( sys_line(OB_SYS_INFO, "VERSION", ver, sizeof(ver)) < 0 ) strcpy(ver, "unknown");
		if ( sys_line(OB_SYS_INFO, "KERNEL", knl, sizeof(knl)) < 0 ) strcpy(knl, "microT-Kernel");
		strlcpy(u->release, ver, sizeof(u->release));
		snprintf(u->version, sizeof(u->version), "TessronOS %s (%s, NetBSD 11.0 commands)", ver, knl);
	}
#if defined(__x86_64__)
	strcpy(u->machine, "x86_64");
#else
	strcpy(u->machine, "aarch64");
#endif
	return 0;
}

long sysconf( int name )
{
	switch ( name ) {
	  case _SC_ARG_MAX:		return TS_SPN_ARG_MAX;
	  case _SC_CHILD_MAX:		return 1000;
	  case _SC_CLK_TCK:		return 100;
	  case _SC_NGROUPS_MAX:		return 64;
	  case _SC_OPEN_MAX:		return PSX_FD_MAX;
	  case _SC_PAGESIZE:		return 4096;
	  case _SC_LINE_MAX:		return 2048;
	  case _SC_TTY_NAME_MAX:	return 32;
	  case _SC_HOST_NAME_MAX:	return 255;
	  case _SC_LOGIN_NAME_MAX:	return 64;
	  case _SC_SYMLOOP_MAX:		return 32;
	  case _SC_NPROCESSORS_CONF:
	  case _SC_NPROCESSORS_ONLN: {
		T_TSCPU	c;

		return ( ts_ref_cpu(&c, (SZ)sizeof(c)) > 0 && c.ncpu > 0 ) ? c.ncpu : 1;	/* the number alone */
	  }
	  case _SC_PHYS_PAGES:		return 256 * 1024;
	  case _SC_JOB_CONTROL:		return 1;
	  case _SC_SAVED_IDS:		return 0;
	  case _SC_VERSION:		return 200809L;
	  default:
		errno = EINVAL;
		return -1;
	}
}

long pathconf( const char *path, int name )
{
	(void)path;
	switch ( name ) {
	  case _PC_LINK_MAX:		return 32767;
	  case _PC_NAME_MAX:		return 255;
	  case _PC_PATH_MAX:		return PATH_MAX;
	  case _PC_PIPE_BUF:		return 512;
	  case _PC_MAX_CANON:		return 1023;
	  case _PC_MAX_INPUT:		return 4096;
	  case _PC_NO_TRUNC:		return 1;
	  case _PC_CHOWN_RESTRICTED:	return 1;
	  case _PC_VDISABLE:		return 0xff;
	  default:
		errno = EINVAL;
		return -1;
	}
}

long fpathconf( int fd, int name )
{
	(void)fd;
	return pathconf("/", name);
}

int getrlimit( int res, struct rlimit *r )
{
	r->rlim_cur = r->rlim_max = RLIM_INFINITY;
	if ( res == RLIMIT_NOFILE ) r->rlim_cur = r->rlim_max = PSX_FD_MAX;
	if ( res == RLIMIT_STACK ) r->rlim_cur = r->rlim_max = 1024 * 1024;
	return 0;
}

int setrlimit( int res, const struct rlimit *r )
{
	(void)res;
	(void)r;
	return 0;
}

int getrusage( int who, struct rusage *ru )
{
	(void)who;
	memset(ru, 0, sizeof(*ru));
	return 0;
}

int getpriority( int which, id_t who )
{
	(void)which;
	(void)who;
	errno = 0;
	return 0;
}

int setpriority( int which, id_t who, int prio )
{
	(void)which;
	(void)who;
	(void)prio;
	return 0;
}

int nice( int incr )
{
	(void)incr;
	return 0;
}
