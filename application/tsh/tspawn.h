/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tspawn.h
 *	tsh without fork (design 5.8.8): a program is started with
 *	posix_spawn, and a subshell -- ( list ), an element of a pipeline
 *	that is not a program, $( ) of a builtin, a function in the
 *	background -- is another tsh started with the shell's state and
 *	the tree it is to evaluate, written into a pipe it reads first.
 */

#ifndef TSH_SPAWN_H
#define TSH_SPAWN_H

#include <sys/types.h>
#include <signal.h>
#include <stddef.h>

union node;
struct job;

/* A buffer the state is written into, and the reading of it back */
struct shbuf {
	char	*p;
	size_t	n, cap;
};

struct shrd {
	const char *p;
	size_t	n, at;
};

void	sb_int(struct shbuf *, int);
void	sb_str(struct shbuf *, const char *);
int	sr_int(struct shrd *);
char	*sr_str(struct shrd *);
void	*sr_alloc(struct shrd *, size_t);

/* nodes.c (made by mknodes.sh): a tree written out and read back */
void	sernode(struct shbuf *, union node *);
union node *desnode(struct shrd *);

/* var.c, exec.c, alias.c: what of them a subshell has */
void	servars(struct shbuf *);
void	restorevar(char *, int);
void	serfuncs(struct shbuf *);
void	seraliases(struct shbuf *);
void	restorealias(char *, char *);

/* trap.c: the signals a child takes back to their default, and ignores */
void	spawn_sigsets(sigset_t *, sigset_t *, int);

/* The word on the command line of a tsh started as a subshell, and its program */
#define TSH_SUBSHELL_ARG	"--tsh-subshell"
#define TSH_PATH		"/bin/tsh"
#define TSH_STATE_FD		255	/* where the subshell reads its state from */

/*
 * A program started as a fork would have run it: in, out (-1: as the
 * shell has it) become its standard input and output before the
 * redirections the shell has already made of its own descriptors.
 * -1 when no program of that name could be started: the caller then
 * starts a subshell, which tells why as the shell would.
 */
pid_t	spawn_command(char **, char **, const char *, int, struct job *, union node *, int, int, int);

/* A subshell evaluating the tree, its standard input and output in and out */
pid_t	spawn_subshell(union node *, struct job *, int, int, int);

/* A here-document longer than a pipe holds, written into it by a tsh of its own */
#define TSH_PIPE_ROOM		65536
#define TSH_HEREDOC_ARG		"--tsh-heredoc"
void	spawn_heredoc(const char *, int);

/* The subshell's side: the state read from the descriptor, the tree evaluated */
void	subshell_main(int) __attribute__((__noreturn__));

/* Descriptors 0 and 1 made in and out for a while, and put back */
/*
 * Descriptors 0 and 1 as they were before a spawnio_push. The caller keeps
 * it, because the pushes nest: a program of a pipeline has its pipe ends
 * pushed around the push of the spawn itself.
 */
struct spawnio {
	int fd[2];
	int had[2];
};

void	spawnio_push(struct spawnio *, int, int);
void	spawnio_pop(struct spawnio *);

#endif /* TSH_SPAWN_H */
