/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tspawn.c
 *	tsh without fork (design 5.8.8). TessronOS has no fork: where the
 *	shell would fork, the parent does what the child would have done
 *	to itself before running a program, and starts the program with
 *	posix_spawn:
 *
 *	- its group (and, in the foreground, the terminal's foreground
 *	  group, which the child takes before main) by the attributes;
 *	- the signals the shell ignores for its own sake back to their
 *	  default, those of a job in the background without job control
 *	  ignored;
 *	- its standard input and output, the pipe of a pipeline or of
 *	  $( ), put on descriptors 0 and 1 for a while, and its
 *	  redirections made on the shell's descriptors with REDIR_PUSH,
 *	  in the order the child would have made them; all of these are
 *	  put back when it has started.
 *
 *	A subshell, which a fork would have made a copy of the shell for,
 *	is a tsh of its own: the variables, the functions, the aliases,
 *	the options, the positional parameters, $$, $? and $! are written
 *	into a pipe with the tree it is to evaluate (nodes.c's sernode),
 *	and it reads them back before anything else (subshell_main).
 *	What a subshell does not have of its parent: the traps, which a
 *	subshell sets back anyway, and the memory of the jobs.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"
#include "nodes.h"
#include "alias.h"
#include "eval.h"
#include "exec.h"
#include "jobs.h"
#include "main.h"
#include "memalloc.h"
#include "mystring.h"
#include "options.h"
#include "output.h"
#include "error.h"
#include "redir.h"
#include "trap.h"
#include "var.h"
#include "tspawn.h"
#include "bashext.h"

#define STATE_MAGIC	0x54534831	/* "TSH1" */
#define NOPTIONS	((int)(sizeof_optlist / sizeof(optlist[0])) - 1)

extern int jobctl;			/* jobs.c */
extern int rootpid;

/* ---------------------------------------------------------------- the buffer */

static void
sb_put(struct shbuf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = (b->cap == 0) ? 4096 : b->cap;

		while (cap < b->n + n)
			cap *= 2;
		b->p = ckrealloc(b->p, cap);
		b->cap = cap;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

void
sb_int(struct shbuf *b, int v)
{
	sb_put(b, &v, sizeof(v));
}

void
sb_str(struct shbuf *b, const char *s)
{
	int n;

	if (s == NULL) {
		sb_int(b, -1);
		return;
	}
	n = (int)strlen(s);
	sb_int(b, n);
	sb_put(b, s, (size_t)n);
}

static void
sr_bad(void)
{
	error("subshell: the state from the parent is broken");
}

int
sr_int(struct shrd *r)
{
	int v;

	if (r->at + sizeof(v) > r->n)
		sr_bad();
	memcpy(&v, r->p + r->at, sizeof(v));
	r->at += sizeof(v);
	return v;
}

char *
sr_str(struct shrd *r)
{
	int n = sr_int(r);
	char *s;

	if (n < 0)
		return NULL;
	if (r->at + (size_t)n > r->n)
		sr_bad();
	s = ckmalloc((size_t)n + 1);
	memcpy(s, r->p + r->at, (size_t)n);
	s[n] = '\0';
	r->at += (size_t)n;
	return s;
}

void *
sr_alloc(struct shrd *r, size_t n)
{
	void *p = ckmalloc(n);

	(void)r;
	memset(p, 0, n);
	return p;
}

/* ---------------------------------------------------------------- descriptors 0 and 1 for a while */

static void
io_put(struct spawnio *sv, int which, int fd)
{
	if (fd < 0 || fd == which)
		return;
	sv->fd[which] = fcntl(which, F_DUPFD_CLOEXEC, 10);
	sv->had[which] = 1;		/* -1 there: it was closed */
	if (dup2(fd, which) < 0)
		error("cannot make descriptor %d: %s", which, strerror(errno));
}

static void
io_back(struct spawnio *sv, int which)
{
	if (!sv->had[which])
		return;
	if (sv->fd[which] >= 0) {
		(void)dup2(sv->fd[which], which);
		(void)close(sv->fd[which]);
	} else
		(void)close(which);
	sv->had[which] = 0;
}

void
spawnio_push(struct spawnio *sv, int in, int out)
{
	sv->had[0] = sv->had[1] = 0;
	io_put(sv, 0, in);
	io_put(sv, 1, out);
}

void
spawnio_pop(struct spawnio *sv)
{
	io_back(sv, 1);
	io_back(sv, 0);
}

/* ---------------------------------------------------------------- starting */

/* The attributes a forked child of the job would have given itself */
static void
child_attr(posix_spawnattr_t *at, struct job *jp, int mode)
{
	sigset_t def, ign;
	short flags = POSIX_SPAWN_SETSIGDEF;
	int bg_nojob = 0;

	posix_spawnattr_init(at);
#if JOBS
	if (rootshell && mode != FORK_NOJOB && mflag) {
		pid_t pgrp = (jp == NULL || jp->nprocs == 0) ? 0 : jp->ps[0].pid;

		flags |= POSIX_SPAWN_SETPGROUP;
		posix_spawnattr_setpgroup(at, pgrp);
		if (mode == FORK_FG && pgrp == 0)
			flags |= POSIX_SPAWN_TCSETPGROUP;
	} else
#endif
	if (mode == FORK_BG)
		bg_nojob = 1;
	spawn_sigsets(&def, &ign, bg_nojob);
	posix_spawnattr_setsigdefault(at, &def);
	posix_spawnattr_setsigignore_np(at, &ign);
	posix_spawnattr_setflags(at, flags);
}

/* A background job without job control reads nothing of the terminal: /dev/null instead */
static int
bg_stdin(struct job *jp, int mode, int in)
{
#if JOBS
	if (rootshell && mode != FORK_NOJOB && mflag)
		return in;
#endif
	if (mode == FORK_BG && in < 0 && (jp == NULL || jp->nprocs == 0) &&
	    !fd0_redirected_p())
		return open(_PATH_DEVNULL, O_RDONLY | O_CLOEXEC);
	return in;
}

static int
try_spawn(pid_t *pid, const char *file, char **argv, char **envp, posix_spawnattr_t *at)
{
	int e;

	e = posix_spawn(pid, file, NULL, at, argv, envp);
	if (e == ENOEXEC) {
		/* a script without #!: the shell runs it */
		int n;
		char **av;

		for (n = 0; argv[n] != NULL; n++)
			continue;
		av = ckmalloc(sizeof(char *) * (size_t)(n + 2));
		av[0] = __UNCONST("sh");
		av[1] = __UNCONST(file);
		memcpy(av + 2, argv + 1, sizeof(char *) * (size_t)n);
		e = posix_spawn(pid, TSH_PATH, NULL, at, av, envp);
		ckfree(av);
	}
	return e;
}

pid_t
spawn_command(char **argv, char **envp, const char *path, int idx, struct job *jp,
	      union node *n, int mode, int in, int out)
{
	posix_spawnattr_t at;
	struct spawnio sv;
	pid_t pid = -1;
	char *cmdname;
	int e = ENOENT, nullfd;

	nullfd = bg_stdin(jp, mode, in);
	child_attr(&at, jp, mode);
	INTOFF;
	spawnio_push(&sv, nullfd, out);
	if (strchr(argv[0], '/') != NULL) {
		e = try_spawn(&pid, argv[0], argv, envp, &at);
	} else {
		while ((cmdname = padvance(&path, argv[0], 1)) != NULL) {
			if (--idx < 0 && pathopt == NULL) {
				e = try_spawn(&pid, cmdname, argv, envp, &at);
				stunalloc(cmdname);
				if (e != ENOENT && e != ENOTDIR)
					break;
			} else
				stunalloc(cmdname);
		}
	}
	spawnio_pop(&sv);
	posix_spawnattr_destroy(&at);
	if (nullfd >= 0 && nullfd != in)
		(void)close(nullfd);
	INTON;
	if (e != 0)
		return -1;
	forkparent(jp, n, mode, pid);
	return pid;
}

/* ---------------------------------------------------------------- a subshell */

static void
serparams(struct shbuf *b)
{
	int i;

	sb_int(b, 'P');
	sb_str(b, arg0);
	sb_int(b, shellparam.nparam);
	for (i = 0; i < shellparam.nparam; i++)
		sb_str(b, shellparam.p[i]);
}

static void
seroptions(struct shbuf *b)
{
	int i;

	sb_int(b, 'O');
	sb_int(b, NOPTIONS);
	for (i = 0; i < NOPTIONS; i++)
		sb_int(b, optlist[i].val);
}

static void
write_all(int fd, const char *p, size_t n)
{
	ssize_t k;

	while (n > 0) {
		k = write(fd, p, n);
		if (k < 0) {
			if (errno == EINTR)
				continue;
			return;		/* it ended before reading: its status tells */
		}
		p += k;
		n -= (size_t)k;
	}
}

pid_t
spawn_subshell(union node *n, struct job *jp, int mode, int in, int out)
{
	posix_spawnattr_t at;
	posix_spawn_file_actions_t fa;
	struct shbuf b;
	char *argv[4], fdword[8];
	int pip[2], e, nullfd;
	pid_t pid;
	sig_t opipe;

	memset(&b, 0, sizeof(b));
	sb_int(&b, STATE_MAGIC);
	servars(&b);
	serarrays(&b);
	serfuncs(&b);
	seraliases(&b);
	seroptions(&b);
	serparams(&b);
	sb_int(&b, 'M');
	sb_int(&b, rootpid);
	sb_int(&b, exitstatus);
	sb_int(&b, (int)backgndpid);
	sb_int(&b, 'X');
	sb_int(&b, tshext);
	sb_int(&b, sh_nullglob);
	sb_int(&b, sh_dotglob);
	sb_int(&b, sh_nocaseglob);
	sb_int(&b, sh_nocasematch);
	sb_int(&b, sh_extglob);
	sb_int(&b, 'C');
	sernode(&b, n);
	sb_int(&b, 'E');

	if (pipe2(pip, O_CLOEXEC) < 0) {
		ckfree(b.p);
		error("Pipe call failed: %s", strerror(errno));
	}
	nullfd = bg_stdin(jp, mode, in);
	child_attr(&at, jp, mode);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pip[0], TSH_STATE_FD);
	if (nullfd >= 0)
		posix_spawn_file_actions_adddup2(&fa, nullfd, 0);
	if (out >= 0)
		posix_spawn_file_actions_adddup2(&fa, out, 1);
	fmtstr(fdword, sizeof(fdword), "%d", TSH_STATE_FD);
	argv[0] = __UNCONST("tsh");
	argv[1] = __UNCONST(TSH_SUBSHELL_ARG);
	argv[2] = fdword;
	argv[3] = NULL;

	INTOFF;
	e = posix_spawn(&pid, TSH_PATH, &fa, &at, argv, environment());
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	if (nullfd >= 0 && nullfd != in)
		(void)close(nullfd);
	(void)close(pip[0]);
	if (e != 0) {
		(void)close(pip[1]);
		ckfree(b.p);
		INTON;
		error("Cannot start a subshell (%s)", strerror(e));
	}
	opipe = signal(SIGPIPE, SIG_IGN);
	write_all(pip[1], b.p, b.n);
	(void)signal(SIGPIPE, opipe);
	(void)close(pip[1]);
	ckfree(b.p);
	forkparent(jp, n, mode, pid);
	INTON;
	return pid;
}

/*
 * The writer of a long here-document: a tsh given the text as its
 * argument writes it on its standard output, the pipe, and ends. The
 * shell does not wait for it, as it does not for the child it would
 * have forked; the process ends as soon as the pipe has taken the text.
 */
void
spawn_heredoc(const char *text, int out)
{
	posix_spawn_file_actions_t fa;
	char *argv[4];
	pid_t pid;
	int e;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out, 1);
	argv[0] = __UNCONST("tsh");
	argv[1] = __UNCONST(TSH_HEREDOC_ARG);
	argv[2] = __UNCONST(text);
	argv[3] = NULL;
	e = posix_spawn(&pid, TSH_PATH, &fa, NULL, argv, environment());
	posix_spawn_file_actions_destroy(&fa);
	if (e != 0)
		error("cannot write the here-document (%s)", strerror(e));
}

/* ---------------------------------------------------------------- the subshell's side */

void
subshell_main(int fd)
{
	struct shbuf b;
	struct shrd r;
	union node *n = NULL;
	char buf[4096], **av;
	ssize_t k;
	int tag, i, cnt, flags;
	char *s, *v;

	memset(&b, 0, sizeof(b));
	for (;;) {
		k = read(fd, buf, sizeof(buf));
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			break;
		sb_put(&b, buf, (size_t)k);
	}
	(void)close(fd);

	rootshell = 0;
	jobctl = 0;
	r.p = b.p;
	r.n = b.n;
	r.at = 0;
	if (sr_int(&r) != STATE_MAGIC)
		sr_bad();
	for (;;) {
		tag = sr_int(&r);
		if (tag == 'E')
			break;
		switch (tag) {
		case 'V':
			flags = sr_int(&r);
			s = sr_str(&r);
			restorevar(s, flags);
			break;
		case 'F':
			s = sr_str(&r);
			i = sr_int(&r);
			defun(s, desnode(&r), i);
			ckfree(s);
			break;
		case 'A':
			s = sr_str(&r);
			v = sr_str(&r);
			restorealias(s, v);
			ckfree(s);
			ckfree(v);
			break;
		case 'O':
			cnt = sr_int(&r);
			for (i = 0; i < cnt; i++) {
				int val = sr_int(&r);

				if (i < NOPTIONS)
					optlist[i].val = (unsigned char)val;
			}
			optschanged();
			break;
		case 'P':
			arg0 = sr_str(&r);
			if (arg0 == NULL)
				arg0 = __UNCONST("tsh");
			cnt = sr_int(&r);
			av = ckmalloc(sizeof(char *) * (size_t)(cnt + 1));
			for (i = 0; i < cnt; i++)
				av[i] = sr_str(&r);
			av[cnt] = NULL;
			setparam(av);
			for (i = 0; i < cnt; i++)
				ckfree(av[i]);
			ckfree(av);
			break;
		case 'M':
			rootpid = sr_int(&r);
			exitstatus = sr_int(&r);
			backgndpid = (pid_t)sr_int(&r);
			break;
		case 'R':
			restorearray(&r);
			break;
		case 'X':
			tshext = sr_int(&r);
			sh_nullglob = sr_int(&r);
			sh_dotglob = sr_int(&r);
			sh_nocaseglob = sr_int(&r);
			sh_nocasematch = sr_int(&r);
			sh_extglob = sr_int(&r);
			break;
		case 'C':
			n = desnode(&r);
			break;
		default:
			sr_bad();
		}
	}
	/* a job in the background is one here already: it is evaluated as a subshell is */
	if (n != NULL && n->type == NBACKGND)
		n->type = NSUBSHELL;
	if (n != NULL && n->type == NCMD)
		n->ncmd.backgnd = 0;
	if (n != NULL && n->type == NPIPE)
		n->npipe.backgnd = 0;
	if (n != NULL)
		evaltree(n, EV_EXIT);
	exitshell(exitstatus);
	/* NOTREACHED */
}
