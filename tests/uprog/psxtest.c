/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psxtest.c
 *	A POSIX program the tests start (tests/ktest/ktest_psx.c, design
 *	5.8.6): what it was given -- its words, its environment, its place,
 *	its descriptors -- and the calls of lib/libposix it makes, written
 *	to its standard output one line each, "ok ..." or "ng ...".
 *
 *	psxtest args A B C	the words, a line each
 *	psxtest cat		its standard input to its standard output
 *	psxtest exit N		ends with N
 *	psxtest pipe		a child of its own through a pipe, and waited for
 *	psxtest sig		a signal caught, held back and let through, an alarm
 *	psxtest kill		waits for a signal that ends it by default
 *	psxtest tty		reads a line from its terminal, as given
 *	psxtest names		NAME_MAX as pathconf, limits.h and struct dirent
 *				have it; a name that long made, one longer refused
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

extern char	**environ;

static volatile int	got_usr1, got_alrm;

static void on_usr1( int s )	{ (void)s; got_usr1++; }
static void on_alrm( int s )	{ (void)s; got_alrm++; }

static int t_pipe( const char *self )
{
	posix_spawn_file_actions_t fa;
	char	*argv[] = { (char *)self, (char *)"args", (char *)"hello", (char *)"world", NULL };
	char	buf[256];
	int	fds[2], st = 0, n, got = 0;
	pid_t	pid;

	if ( pipe(fds) < 0 ) {
		printf("ng pipe %d\n", errno);
		return 1;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawn_file_actions_addclose(&fa, fds[1]);
	n = posix_spawn(&pid, self, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if ( n != 0 ) {
		printf("ng spawn %d\n", n);
		return 1;
	}
	while ( ( n = (int)read(fds[0], buf + got, sizeof(buf) - 1 - (size_t)got) ) > 0 ) got += n;
	buf[got] = 0;
	close(fds[0]);
	if ( waitpid(pid, &st, 0) != pid ) {
		printf("ng waitpid %d\n", errno);
		return 1;
	}
	if ( strcmp(buf, "arg hello\narg world\n") != 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0 ) {
		printf("ng child said [%s] status %x\n", buf, st);
		return 1;
	}
	printf("ok pipe\n");
	return 0;
}

static int t_sig( void )
{
	struct sigaction sa;
	sigset_t	m;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	kill(getpid(), SIGUSR1);
	if ( got_usr1 != 1 ) {
		printf("ng caught %d\n", got_usr1);
		return 1;
	}
	sigemptyset(&m);
	sigaddset(&m, SIGUSR1);
	sigprocmask(SIG_BLOCK, &m, NULL);
	kill(getpid(), SIGUSR1);
	if ( got_usr1 != 1 ) {
		printf("ng held %d\n", got_usr1);
		return 1;
	}
	sigprocmask(SIG_UNBLOCK, &m, NULL);
	getpid();
	if ( got_usr1 != 2 ) {
		printf("ng let through %d\n", got_usr1);
		return 1;
	}
	sa.sa_handler = on_alrm;
	sigaction(SIGALRM, &sa, NULL);
	alarm(1);
	if ( sleep(5) == 0 || got_alrm != 1 ) {
		printf("ng alarm %d\n", got_alrm);
		return 1;
	}
	printf("ok sig\n");
	return 0;
}

static int t_names( void )
{
	static char	path[NAME_MAX + 8];
	struct dirent	*e;
	DIR		*d;
	long		pc = pathconf("/tmp", _PC_NAME_MAX);
	int		fd, found = 0, i;

	printf("names pc %ld max %d dname %d\n", pc, (int)NAME_MAX, (int)sizeof(e->d_name));
	strcpy(path, "/tmp/");
	for ( i = 0; i < NAME_MAX; i++ ) path[5 + i] = (char)( 'a' + i % 26 );
	path[5 + NAME_MAX] = 0;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	printf("names make %s\n", ( fd >= 0 ) ? "ok" : strerror(errno));
	if ( fd >= 0 ) close(fd);
	d = opendir("/tmp");
	while ( d != NULL && ( e = readdir(d) ) != NULL ) {
		if ( strcmp(e->d_name, path + 5) == 0 ) found = 1;
	}
	if ( d != NULL ) closedir(d);
	printf("names listed %d\n", found);
	(void)unlink(path);

	path[5 + NAME_MAX] = 'z';			/* one byte more */
	path[6 + NAME_MAX] = 0;
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	printf("names long %s\n", ( fd < 0 && errno == ENAMETOOLONG ) ? "refused" : "taken");
	if ( fd >= 0 ) {
		close(fd);
		(void)unlink(path);
	}
	return 0;
}

int main( int argc, char **argv )
{
	int	i;

	if ( argc < 2 ) {
		printf("ng no words\n");
		return 2;
	}
	if ( strcmp(argv[1], "args") == 0 ) {
		for ( i = 2; i < argc; i++ ) printf("arg %s\n", argv[i]);
		return 0;
	}
	if ( strcmp(argv[1], "env") == 0 ) {
		char	cwd[256];

		printf("env HOME=%s cwd=%s\n", getenv("HOME") ? getenv("HOME") : "-",
		       getcwd(cwd, sizeof(cwd)) ? cwd : "-");
		return 0;
	}
	if ( strcmp(argv[1], "cat") == 0 ) {
		char	b[512];
		ssize_t	n;

		while ( ( n = read(0, b, sizeof(b)) ) > 0 ) write(1, b, (size_t)n);
		return 0;
	}
	if ( strcmp(argv[1], "exit") == 0 ) {
		return ( argc > 2 ) ? atoi(argv[2]) : 0;
	}
	if ( strcmp(argv[1], "pipe") == 0 ) {
		return t_pipe(argv[0]);
	}
	if ( strcmp(argv[1], "sig") == 0 ) {
		return t_sig();
	}
	if ( strcmp(argv[1], "kill") == 0 ) {
		printf("ready\n");
		fflush(stdout);
		for ( ;; ) pause();
	}
	if ( strcmp(argv[1], "tty") == 0 ) {
		char	b[128];
		ssize_t	n;

		printf("tty %d\n", isatty(0));
		fflush(stdout);
		n = read(0, b, sizeof(b) - 1);
		if ( n < 0 ) n = 0;
		b[n] = 0;
		printf("line [%s]\n", b);
		return 0;
	}
	if ( strcmp(argv[1], "names") == 0 ) {
		return t_names();
	}
	printf("ng what is %s\n", argv[1]);
	return 2;
}
