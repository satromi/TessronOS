/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_spawn.c
 *	Starting programs (lib/libposix, design 5.8.2)
 *
 *	There is no fork: a child is started from its program at once,
 *	with ts_spn_prc, and given what it is to have -- its arguments, its
 *	environment, its place, its umask and its descriptors -- in the
 *	start-up argument the form of psx_start.c, the descriptors
 *	themselves handed over by the kernel. posix_spawn's file actions
 *	are worked out on a copy of the table first: dup2 and close change
 *	only the copy, open opens in the caller and is closed there once the
 *	child has it.
 *
 *	A program is an object of the object world with an executable
 *	record. A file that starts with "#!" is run by the program it names
 *	(one argument after it), with its own path after that. Anything else
 *	cannot be run (ENOEXEC): a shell then reads it as a script.
 *
 *	execve does not replace the caller: it starts the program as a child
 *	with the caller's descriptors, passes on to it the signals the caller
 *	gets, waits for it, and ends as it ended.
 */

#include "psx_int.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <spawn.h>
#include <paths.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PSX_AF_TCSETPGRP 0x0001	/* the child makes its group the terminal's foreground first */

#define PSX_FDM_CONOUT	0x00010000
#define PSX_FDM_CONIN	0x00020000

typedef struct {
	UW	size;
	UW	argc;
	UW	envc;
	UW	nfd;
	UW	umask;
	UW	flags;
	UW	fdmap;
	UW	strs;
} PSXARG;

/* ---------------------------------------------------------------- the attributes and actions */

struct __posix_spawnattr {
	short		flags;
	pid_t		pgroup;
	sigset_t	sigdef;
	sigset_t	sigmask;
	sigset_t	sigign;		/* ignored in the child besides what the caller ignores */
};

#define FA_OPEN		1
#define FA_DUP2		2
#define FA_CLOSE	3
#define FA_CHDIR	4

typedef struct {
	int	op;
	int	fd, fd2;
	int	oflag;
	mode_t	mode;
	char	*path;
} FACT;

struct __posix_spawn_file_actions {
	int	n, cap;
	FACT	*a;
};

int posix_spawnattr_init( posix_spawnattr_t *a )
{
	*a = (posix_spawnattr_t)calloc(1, sizeof(struct __posix_spawnattr));
	return ( *a == NULL ) ? ENOMEM : 0;
}

int posix_spawnattr_destroy( posix_spawnattr_t *a )
{
	free(*a);
	*a = NULL;
	return 0;
}

int posix_spawnattr_setflags( posix_spawnattr_t *a, short f )	{ (*a)->flags = f; return 0; }
int posix_spawnattr_getflags( const posix_spawnattr_t *a, short *f ) { *f = (*a)->flags; return 0; }
int posix_spawnattr_setpgroup( posix_spawnattr_t *a, pid_t p )	{ (*a)->pgroup = p; return 0; }
int posix_spawnattr_getpgroup( const posix_spawnattr_t *a, pid_t *p ) { *p = (*a)->pgroup; return 0; }
int posix_spawnattr_setsigdefault( posix_spawnattr_t *a, const sigset_t *s ) { (*a)->sigdef = *s; return 0; }
int posix_spawnattr_getsigdefault( const posix_spawnattr_t *a, sigset_t *s ) { *s = (*a)->sigdef; return 0; }
int posix_spawnattr_setsigmask( posix_spawnattr_t *a, const sigset_t *s ) { (*a)->sigmask = *s; return 0; }
int posix_spawnattr_getsigmask( const posix_spawnattr_t *a, sigset_t *s ) { *s = (*a)->sigmask; return 0; }
int posix_spawnattr_setsigignore_np( posix_spawnattr_t *a, const sigset_t *s ) { (*a)->sigign = *s; return 0; }
int posix_spawnattr_getsigignore_np( const posix_spawnattr_t *a, sigset_t *s ) { *s = (*a)->sigign; return 0; }
int posix_spawnattr_setschedparam( posix_spawnattr_t *a, const struct sched_param *p ) { (void)a; (void)p; return 0; }
int posix_spawnattr_getschedparam( const posix_spawnattr_t *a, struct sched_param *p ) { (void)a; (void)p; return 0; }
int posix_spawnattr_setschedpolicy( posix_spawnattr_t *a, int p ) { (void)a; (void)p; return 0; }
int posix_spawnattr_getschedpolicy( const posix_spawnattr_t *a, int *p ) { (void)a; *p = 0; return 0; }

int posix_spawn_file_actions_init( posix_spawn_file_actions_t *fa )
{
	*fa = (posix_spawn_file_actions_t)calloc(1, sizeof(struct __posix_spawn_file_actions));
	return ( *fa == NULL ) ? ENOMEM : 0;
}

int posix_spawn_file_actions_destroy( posix_spawn_file_actions_t *fa )
{
	int	i;

	if ( *fa == NULL ) return 0;
	for ( i = 0; i < (*fa)->n; i++ ) free((*fa)->a[i].path);
	free((*fa)->a);
	free(*fa);
	*fa = NULL;
	return 0;
}

static FACT *fa_add( posix_spawn_file_actions_t *fa )
{
	struct __posix_spawn_file_actions *f = *fa;

	if ( f->n == f->cap ) {
		FACT	*na;

		f->cap = ( f->cap == 0 ) ? 8 : f->cap * 2;
		na = (FACT *)realloc(f->a, sizeof(FACT) * (size_t)f->cap);
		if ( na == NULL ) return NULL;
		f->a = na;
	}
	memset(&f->a[f->n], 0, sizeof(FACT));
	return &f->a[f->n++];
}

int posix_spawn_file_actions_addopen( posix_spawn_file_actions_t *fa, int fd, const char *path,
				      int oflag, mode_t mode )
{
	FACT	*a = fa_add(fa);

	if ( a == NULL ) return ENOMEM;
	a->op = FA_OPEN;
	a->fd = fd;
	a->oflag = oflag;
	a->mode = mode;
	a->path = strdup(path);
	return ( a->path == NULL ) ? ENOMEM : 0;
}

int posix_spawn_file_actions_adddup2( posix_spawn_file_actions_t *fa, int fd, int to )
{
	FACT	*a = fa_add(fa);

	if ( a == NULL ) return ENOMEM;
	a->op = FA_DUP2;
	a->fd = fd;
	a->fd2 = to;
	return 0;
}

int posix_spawn_file_actions_addclose( posix_spawn_file_actions_t *fa, int fd )
{
	FACT	*a = fa_add(fa);

	if ( a == NULL ) return ENOMEM;
	a->op = FA_CLOSE;
	a->fd = fd;
	return 0;
}

int posix_spawn_file_actions_addchdir( posix_spawn_file_actions_t *fa, const char *path )
{
	FACT	*a = fa_add(fa);

	if ( a == NULL ) return ENOMEM;
	a->op = FA_CHDIR;
	a->path = strdup(path);
	return ( a->path == NULL ) ? ENOMEM : 0;
}

int posix_spawn_file_actions_addchdir_np( posix_spawn_file_actions_t *fa, const char *path )
{
	return posix_spawn_file_actions_addchdir(fa, path);
}

int posix_spawn_file_actions_addfchdir( posix_spawn_file_actions_t *fa, int fd )
{
	(void)fa;
	(void)fd;
	return ENOTSUP;
}

int posix_spawn_file_actions_addfchdir_np( posix_spawn_file_actions_t *fa, int fd )
{
	return posix_spawn_file_actions_addfchdir(fa, fd);
}

/* ---------------------------------------------------------------- the start-up argument */

typedef struct {
	char	*b;
	size_t	n, cap;
} ABUF;

static int ab_put( ABUF *a, const void *p, size_t n )
{
	if ( a->n + n > a->cap ) {
		size_t	cap = ( a->cap == 0 ) ? 4096 : a->cap;
		char	*nb;

		while ( cap < a->n + n ) cap *= 2;
		if ( cap > TS_SPN_ARG_MAX ) {
			errno = E2BIG;
			return -1;
		}
		nb = (char *)realloc(a->b, cap);
		if ( nb == NULL ) {
			errno = ENOMEM;
			return -1;
		}
		a->b = nb;
		a->cap = cap;
	}
	memcpy(a->b + a->n, p, n);
	a->n += n;
	return 0;
}

/* ---------------------------------------------------------------- finding the program */

/* What the start of a file says: an ELF, "#!" and what follows, or neither */
static int head_of( const char *path, char *interp, size_t imax, char *iarg, size_t amax )
{
	char	h[256];
	int	fd, n, i, k;

	interp[0] = iarg[0] = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if ( fd < 0 ) return -1;
	n = (int)read(fd, h, sizeof(h) - 1);
	(void)close(fd);
	if ( n >= 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' ) {
		return 1;
	}
	if ( n < 3 || h[0] != '#' || h[1] != '!' ) {
		return 0;
	}
	h[n] = 0;
	for ( i = 2; h[i] == ' ' || h[i] == '\t'; i++ ) ;
	for ( k = 0; h[i] != 0 && h[i] != ' ' && h[i] != '\t' && h[i] != '\n' && (size_t)k < imax - 1; i++ ) {
		interp[k++] = h[i];
	}
	interp[k] = 0;
	while ( h[i] == ' ' || h[i] == '\t' ) i++;
	for ( k = 0; h[i] != 0 && h[i] != '\n' && h[i] != '\r' && (size_t)k < amax - 1; i++ ) {
		iarg[k++] = h[i];
	}
	while ( k > 0 && ( iarg[k - 1] == ' ' || iarg[k - 1] == '\t' ) ) k--;
	iarg[k] = 0;
	return ( interp[0] != 0 ) ? 2 : 0;
}

/* ---------------------------------------------------------------- spawning */

/*
 * The descriptor table the child is to have, the caller's with the actions
 * done on it. Close-on-exec takes effect only after the actions: a dup2 may
 * name a close-on-exec descriptor as its source, and the copy it makes stays
 * open in the child.
 */
static int child_table( PSXFD *t, const posix_spawn_file_actions_t *fa, int *opened, int *nopened,
			char *cwd )
{
	int	i, n = ( fa == NULL || *fa == NULL ) ? 0 : (*fa)->n;

	*nopened = 0;
	for ( i = 0; i < PSX_FD_MAX; i++ ) t[i] = __psx_fd[i];
	for ( i = 0; i < n; i++ ) {
		FACT	*a = &(*fa)->a[i];
		int	fd;

		switch ( a->op ) {
		  case FA_DUP2:
			if ( a->fd < 0 || a->fd >= PSX_FD_MAX || t[a->fd].kind == PK_NONE
			  || a->fd2 < 0 || a->fd2 >= PSX_FD_MAX ) {
				return EBADF;
			}
			t[a->fd2] = t[a->fd];
			t[a->fd2].cloexec = 0;
			break;
		  case FA_CLOSE:
			if ( a->fd >= 0 && a->fd < PSX_FD_MAX ) t[a->fd].kind = PK_NONE;
			break;
		  case FA_OPEN:
			{
				char	p[PATH_MAX], save[PATH_MAX];

				strcpy(save, __psx_cwd);
				strcpy(__psx_cwd, cwd);
				fd = ( __psx_path(a->path, p) == 0 ) ? open(p, a->oflag | O_CLOEXEC, a->mode) : -1;
				strcpy(__psx_cwd, save);
			}
			if ( fd < 0 ) return errno;
			if ( a->fd < 0 || a->fd >= PSX_FD_MAX ) {
				(void)close(fd);
				return EBADF;
			}
			opened[(*nopened)++] = fd;
			t[a->fd] = __psx_fd[fd];
			t[a->fd].cloexec = 0;
			break;
		  case FA_CHDIR:
			{
				char		p[PATH_MAX], save[PATH_MAX];
				struct stat	st;

				strcpy(save, __psx_cwd);
				strcpy(__psx_cwd, cwd);
				if ( __psx_path(a->path, p) < 0 || stat(p, &st) < 0 ) {
					strcpy(__psx_cwd, save);
					return errno;
				}
				strcpy(__psx_cwd, save);
				if ( !S_ISDIR(st.st_mode) ) return ENOTDIR;
				strcpy(cwd, p);
			}
			break;
		  default:
			break;
		}
	}
	for ( i = 0; i < PSX_FD_MAX; i++ ) {
		if ( t[i].cloexec ) t[i].kind = PK_NONE;
		t[i].cloexec = 0;
	}
	return 0;
}

static int spawn_path( pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
		       const posix_spawnattr_t *at, char *const argv[], char *const envp[], int depth )
{
	char		p[PATH_MAX], interp[PATH_MAX], iarg[256], cwd[PATH_MAX];
	PSXFD		*t;
	ABUF		ab = { NULL, 0, 0 };
	PSXARG		h;
	T_SPAWN		sp;
	INT		kfd[TS_SPN_FD_MAX];
	UW		map[TS_SPN_FD_MAX];
	int		opened[64], nopened = 0, nfd = 0, argc = 0, envc = 0, i, r, kind;
	ID		id;
	struct stat	st;

	if ( __psx_path(path, p) < 0 ) return errno;
	if ( stat(p, &st) < 0 ) return errno;
	if ( S_ISDIR(st.st_mode) ) return EACCES;
	kind = head_of(p, interp, sizeof(interp), iarg, sizeof(iarg));
	if ( kind == 2 && depth < 4 ) {
		/* a script: its program, the word after it, its path, the rest */
		char	**nv;
		int	n, k = 0;

		for ( n = 0; argv != NULL && argv[n] != NULL; n++ ) ;
		nv = (char **)malloc(sizeof(char *) * (size_t)( n + 4 ));
		if ( nv == NULL ) return ENOMEM;
		nv[k++] = interp;
		if ( iarg[0] != 0 ) nv[k++] = iarg;
		nv[k++] = p;
		for ( i = 1; i < n; i++ ) nv[k++] = argv[i];
		nv[k] = NULL;
		r = spawn_path(pid, interp, fa, at, nv, envp, depth + 1);
		free(nv);
		return r;
	}
	if ( kind == 0 ) {
		return ENOEXEC;
	}

	t = (PSXFD *)malloc(sizeof(PSXFD) * PSX_FD_MAX);
	if ( t == NULL ) return ENOMEM;
	strcpy(cwd, __psx_cwd);
	r = child_table(t, fa, opened, &nopened, cwd);
	if ( r != 0 ) goto out;

	for ( i = 0; i < PSX_FD_MAX && nfd < TS_SPN_FD_MAX; i++ ) {
		if ( t[i].kind == PK_FS ) {
			kfd[nfd] = t[i].kfd;
			map[nfd++] = (UW)i;
		} else if ( t[i].kind == PK_CONOUT || t[i].kind == PK_CONIN ) {
			kfd[nfd] = -1;
			map[nfd++] = (UW)i | ( ( t[i].kind == PK_CONOUT ) ? PSX_FDM_CONOUT : PSX_FDM_CONIN );
		}
	}
	/* the consoles go only in the map; the kernel hands over the real ones */
	{
		INT	k[TS_SPN_FD_MAX];
		UW	m[TS_SPN_FD_MAX];
		int	a = 0, b;

		for ( b = 0; b < nfd; b++ ) {
			if ( kfd[b] >= 0 ) {
				k[a] = kfd[b];
				m[a++] = map[b];
			}
		}
		for ( b = 0; b < nfd; b++ ) {
			if ( kfd[b] < 0 ) m[a++] = map[b];
		}
		memcpy(kfd, k, sizeof(INT) * (size_t)nfd);
		memcpy(map, m, sizeof(UW) * (size_t)nfd);
	}

	for ( argc = 0; argv != NULL && argv[argc] != NULL; argc++ ) ;
	for ( envc = 0; envp != NULL && envp[envc] != NULL; envc++ ) ;
	memset(&h, 0, sizeof(h));
	h.argc = (UW)argc;
	h.envc = (UW)envc;
	h.nfd = (UW)nfd;
	h.umask = __psx_umask;
	if ( at != NULL && *at != NULL && ( (*at)->flags & POSIX_SPAWN_TCSETPGROUP ) != 0 ) {
		h.flags |= PSX_AF_TCSETPGRP;
	}
	h.fdmap = 8 + sizeof(h);
	h.strs = h.fdmap + (UW)( sizeof(UW) * (size_t)nfd );
	if ( ab_put(&ab, "\0TSPX1\0", 8) < 0 || ab_put(&ab, &h, sizeof(h)) < 0
	  || ab_put(&ab, map, sizeof(UW) * (size_t)nfd) < 0 ) {
		r = errno;
		goto out;
	}
	for ( i = 0; i < argc; i++ ) {
		if ( ab_put(&ab, argv[i], strlen(argv[i]) + 1) < 0 ) { r = errno; goto out; }
	}
	for ( i = 0; i < envc; i++ ) {
		if ( ab_put(&ab, envp[i], strlen(envp[i]) + 1) < 0 ) { r = errno; goto out; }
	}
	if ( ab_put(&ab, cwd, strlen(cwd) + 1) < 0 ) { r = errno; goto out; }
	((PSXARG *)( ab.b + 8 ))->size = (UW)ab.n;

	memset(&sp, 0, sizeof(sp));
	sp.arg = ab.b;
	sp.argsz = (SZ)ab.n;
	for ( i = 0; i < nfd && map[i] < PSX_FDM_CONOUT; i++ ) ;
	sp.nfd = i;				/* the kernel's: the consoles are not */
	sp.fds = kfd;
	if ( at != NULL && *at != NULL ) {
		if ( (*at)->flags & POSIX_SPAWN_SETPGROUP ) {
			sp.flags |= TS_SPN_PGID;
			sp.pgid = (*at)->pgroup;
		}
		if ( (*at)->flags & POSIX_SPAWN_SETSID ) {
			sp.flags |= TS_SPN_SETSID;
		}
		if ( (*at)->flags & POSIX_SPAWN_SETSIGMASK ) {
			sp.sigmask = (UD)(*at)->sigmask;
		} else {
			sigset_t m;

			(void)sigprocmask(SIG_BLOCK, NULL, &m);
			sp.sigmask = (UD)m;
		}
		if ( (*at)->flags & POSIX_SPAWN_SETSIGDEF ) {
			sp.flags |= TS_SPN_SIGDEF;
			sp.sigdef = (UD)(*at)->sigdef;
		}
		sp.sigign = (UD)(*at)->sigign;
	} else {
		sigset_t m;

		(void)sigprocmask(SIG_BLOCK, NULL, &m);
		sp.sigmask = (UD)m;
	}
	id = ts_spn_prc(p, &sp);
	if ( id < 0 ) {
		r = ( id == E_PAR ) ? ENOEXEC : ( id == E_OBJ ) ? EACCES : __psx_errno_er(id);
		goto out;
	}
	if ( pid != NULL ) *pid = (pid_t)id;
	r = 0;
    out:
	for ( i = 0; i < nopened; i++ ) (void)close(opened[i]);
	free(t);
	free(ab.b);
	return r;
}

int posix_spawn( pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
		 const posix_spawnattr_t *at, char *const argv[], char *const envp[] )
{
	return spawn_path(pid, path, fa, at, argv, envp, 0);
}

/* The program a name finds along PATH: copied into out */
static int path_find( const char *name, char *out, size_t max )
{
	const char	*path = getenv("PATH"), *s, *e;
	struct stat	st;
	int		err = ENOENT;

	if ( strchr(name, '/') != NULL ) {
		strlcpy(out, name, max);
		return 0;
	}
	if ( path == NULL ) path = "/bin";
	for ( s = path; ; s = e + 1 ) {
		size_t	n;

		e = strchr(s, ':');
		if ( e == NULL ) e = s + strlen(s);
		n = (size_t)( e - s );
		if ( n == 0 ) {
			snprintf(out, max, "%s", name);
		} else {
			snprintf(out, max, "%.*s/%s", (int)n, s, name);
		}
		if ( stat(out, &st) == 0 ) {
			if ( !S_ISDIR(st.st_mode) ) return 0;
			err = EACCES;
		}
		if ( *e == 0 ) break;
	}
	return err;
}

int posix_spawnp( pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
		  const posix_spawnattr_t *at, char *const argv[], char *const envp[] )
{
	char	p[PATH_MAX];
	int	r = path_find(file, p, sizeof(p));

	if ( r != 0 ) return r;
	return spawn_path(pid, p, fa, at, argv, envp, 0);
}

/* ---------------------------------------------------------------- exec */

static pid_t	exec_child;

static void exec_forward( int sig )
{
	if ( exec_child > 0 ) (void)kill(exec_child, sig);
}

static __attribute__((noreturn)) void exec_wait( pid_t child )
{
	struct sigaction sa;
	int		st, sig;

	exec_child = child;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = exec_forward;
	for ( sig = 1; sig < NSIG; sig++ ) {
		struct sigaction old;

		if ( sig == SIGKILL || sig == SIGSTOP || sig == SIGCHLD || sig == SIGTSTP
		  || sig == SIGTTIN || sig == SIGTTOU || sig == SIGCONT ) continue;
		if ( sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_IGN ) continue;
		(void)sigaction(sig, &sa, NULL);
	}
	while ( waitpid(child, &st, 0) < 0 ) {
		if ( errno != EINTR ) _exit(127);
	}
	if ( WIFSIGNALED(st) ) {
		sa.sa_handler = SIG_DFL;
		(void)sigaction(WTERMSIG(st), &sa, NULL);
		(void)kill(getpid(), WTERMSIG(st));
	}
	_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
}

int execve( const char *path, char *const argv[], char *const envp[] )
{
	pid_t	child;
	int	r;

	fflush(NULL);
	r = posix_spawn(&child, path, NULL, NULL, argv, envp);
	if ( r != 0 ) {
		errno = r;
		return -1;
	}
	closefrom(0);
	exec_wait(child);
}

int _execve( const char *path, char *const argv[], char *const envp[] )
{
	return execve(path, argv, envp);
}

int execvpe( const char *file, char *const argv[], char *const envp[] )
{
	char	p[PATH_MAX];
	int	r = path_find(file, p, sizeof(p));

	if ( r != 0 ) {
		errno = r;
		return -1;
	}
	return execve(p, argv, envp);
}

int execvp( const char *file, char *const argv[] )
{
	return execvpe(file, argv, environ);
}

int execv( const char *path, char *const argv[] )
{
	return execve(path, argv, environ);
}

static char **va_args( const char *arg0, va_list ap, char ***envp )
{
	va_list	aq;
	char	**v;
	int	n = 1, i;

	va_copy(aq, ap);
	while ( va_arg(aq, char *) != NULL ) n++;
	va_end(aq);
	v = (char **)malloc(sizeof(char *) * (size_t)( n + 1 ));
	if ( v == NULL ) return NULL;
	v[0] = (char *)arg0;
	for ( i = 1; i < n; i++ ) v[i] = va_arg(ap, char *);
	v[n] = NULL;
	(void)va_arg(ap, char *);
	if ( envp != NULL ) *envp = va_arg(ap, char **);
	return v;
}

int execl( const char *path, const char *arg0, ... )
{
	va_list	ap;
	char	**v;

	va_start(ap, arg0);
	v = va_args(arg0, ap, NULL);
	va_end(ap);
	if ( v == NULL ) return -1;
	return execve(path, v, environ);
}

int execlp( const char *file, const char *arg0, ... )
{
	va_list	ap;
	char	**v;

	va_start(ap, arg0);
	v = va_args(arg0, ap, NULL);
	va_end(ap);
	if ( v == NULL ) return -1;
	return execvp(file, v);
}

int execle( const char *path, const char *arg0, ... )
{
	va_list	ap;
	char	**v, **e = NULL;

	va_start(ap, arg0);
	v = va_args(arg0, ap, &e);
	va_end(ap);
	if ( v == NULL ) return -1;
	return execve(path, v, e);
}

pid_t fork( void )
{
	errno = ENOSYS;
	return -1;
}

pid_t _fork( void )
{
	return fork();
}

pid_t vfork( void )
{
	errno = ENOSYS;
	return -1;
}

/* ---------------------------------------------------------------- the shell */

int system( const char *cmd )
{
	char	*argv[4];
	pid_t	pid;
	int	st = 0, r;
	struct sigaction ign, oint, oquit;

	if ( cmd == NULL ) return 1;
	argv[0] = (char *)"sh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)cmd;
	argv[3] = NULL;
	memset(&ign, 0, sizeof(ign));
	ign.sa_handler = SIG_IGN;
	(void)sigaction(SIGINT, &ign, &oint);
	(void)sigaction(SIGQUIT, &ign, &oquit);
	fflush(NULL);
	r = posix_spawn(&pid, _PATH_BSHELL, NULL, NULL, argv, environ);
	if ( r != 0 ) {
		st = 127 << 8;
	} else {
		while ( waitpid(pid, &st, 0) < 0 && errno == EINTR ) ;
	}
	(void)sigaction(SIGINT, &oint, NULL);
	(void)sigaction(SIGQUIT, &oquit, NULL);
	return st;
}

typedef struct pent {
	struct pent	*next;
	FILE		*fp;
	pid_t		pid;
} PENT;

static PENT	*popen_list;

FILE *popen( const char *cmd, const char *mode )
{
	posix_spawn_file_actions_t fa;
	char	*argv[4];
	int	fds[2], r, rd = ( mode[0] == 'r' );
	pid_t	pid;
	PENT	*pe;
	FILE	*fp;

	if ( pipe2(fds, O_CLOEXEC) < 0 ) return NULL;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, rd ? fds[1] : fds[0], rd ? 1 : 0);
	argv[0] = (char *)"sh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)cmd;
	argv[3] = NULL;
	fflush(NULL);
	r = posix_spawn(&pid, _PATH_BSHELL, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	(void)close(rd ? fds[1] : fds[0]);
	if ( r != 0 ) {
		(void)close(rd ? fds[0] : fds[1]);
		errno = r;
		return NULL;
	}
	fp = fdopen(rd ? fds[0] : fds[1], rd ? "r" : "w");
	pe = (PENT *)malloc(sizeof(PENT));
	if ( fp == NULL || pe == NULL ) {
		if ( fp != NULL ) fclose(fp);
		free(pe);
		return NULL;
	}
	pe->fp = fp;
	pe->pid = pid;
	pe->next = popen_list;
	popen_list = pe;
	return fp;
}

int pclose( FILE *fp )
{
	PENT	**pp, *pe;
	int	st = 0;

	for ( pp = &popen_list; *pp != NULL && (*pp)->fp != fp; pp = &(*pp)->next ) ;
	if ( *pp == NULL ) {
		errno = ECHILD;
		return -1;
	}
	pe = *pp;
	*pp = pe->next;
	fclose(fp);
	while ( waitpid(pe->pid, &st, 0) < 0 ) {
		if ( errno != EINTR ) {
			st = -1;
			break;
		}
	}
	free(pe);
	return st;
}

/* ---------------------------------------------------------------- waiting */

pid_t wait4( pid_t pid, int *status, int options, struct rusage *ru )
{
	INT	st = 0;
	UINT	o = 0;
	ID	r;

	if ( ru != NULL ) memset(ru, 0, sizeof(*ru));
	if ( options & WNOHANG ) o |= TS_WP_NOHANG;
	if ( options & WUNTRACED ) o |= TS_WP_UNTRACED;
	if ( options & WCONTINUED ) o |= TS_WP_CONTINUED;
	r = ts_wai_psx((ID)pid, o, &st);
	if ( r < 0 ) {
		errno = ( r == E_NOEXS ) ? ECHILD : __psx_errno_er(r);
		return -1;
	}
	if ( r > 0 && status != NULL ) *status = st;
	return (pid_t)r;
}

pid_t waitpid( pid_t pid, int *status, int options )
{
	return wait4(pid, status, options, NULL);
}

pid_t wait3( int *status, int options, struct rusage *ru )
{
	return wait4(-1, status, options, ru);
}

pid_t wait( int *status )
{
	return wait4(-1, status, 0, NULL);
}

pid_t _wait( int *status ) __attribute__((alias("wait")));

/* ---------------------------------------------------------------- the process */

pid_t getpid( void )		{ return (pid_t)ts_get_pid(); }
pid_t _getpid( void ) __attribute__((alias("getpid")));
pid_t getppid( void )		{ return (pid_t)ts_get_ppid(); }

pid_t getpgrp( void )		{ ID g = ts_get_pgid(0); return ( g < 0 ) ? (pid_t)__psx_fail_er(g) : (pid_t)g; }

pid_t getpgid( pid_t pid )
{
	ID	g = ts_get_pgid((ID)pid);

	if ( g < 0 ) {
		errno = ESRCH;
		return -1;
	}
	return (pid_t)g;
}

int setpgid( pid_t pid, pid_t pgid )
{
	ER	er = ts_set_pgid((ID)pid, (ID)pgid);

	if ( er < 0 ) {
		errno = ( er == E_NOEXS ) ? ESRCH : EPERM;
		return -1;
	}
	return 0;
}

pid_t setsid( void )
{
	ID	s = ts_set_sid();

	if ( s < 0 ) {
		errno = EPERM;
		return -1;
	}
	return (pid_t)s;
}

pid_t getsid( pid_t pid )
{
	ID	s = ts_get_sid((ID)pid);

	if ( s < 0 ) {
		errno = ESRCH;
		return -1;
	}
	return (pid_t)s;
}

void _exit( int code )
{
	ts_ext_prc(code & 0xff);
	for ( ;; ) ;
}
