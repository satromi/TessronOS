/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_psx.c
 *	The POSIX layer's foundation (design 5.8.6): pipes, descriptors
 *	handed to a child, the pseudo terminals and their line discipline,
 *	the object world as paths, signals, and a POSIX program
 *	(tests/uprog/psxtest.c, /boot/PSXTEST.ELF) started with all of them.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/fs.h>
#include <ts/psx.h>
#include "ktest.h"
#include "tstdlib.h"

#define PSXTEST		"/boot/PSXTEST.ELF"

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return ( b[n] == 0 );
}

/* Whether a contains b */
LOCAL BOOL s_has( CONST UB *a, INT n, CONST char *b )
{
	INT	i, k = s_len(b);

	for ( i = 0; i + k <= n; i++ ) {
		if ( s_eq(a + i, b, k) ) return TRUE;
	}
	return FALSE;
}

/* Everything readable from fd until its end or ms, into buf */
LOCAL INT read_all( INT fd, UB *buf, INT max, INT ms )
{
	T_FSPOLL p;
	INT	n = 0, r;

	while ( n < max - 1 ) {
		p.fd = fd;
		p.events = FS_POLL_IN;
		if ( fs_poll(&p, 1, ms) <= 0 ) break;
		r = fs_read(fd, buf + n, max - 1 - n);
		if ( r <= 0 ) break;
		n += r;
	}
	buf[n] = 0;
	return n;
}

/* ---------------------------------------------------------------- pipes */

LOCAL void test_pipe( void )
{
	INT	fd[2], n;
	UB	b[16];

	KT_ASSERT_ER(fs_pipe(fd), EX_OK);
	KT_ASSERT_EQ(fs_write(fd[1], "abc", 3), 3);
	n = fs_read(fd[0], b, sizeof(b));
	KT_ASSERT_EQ(n, 3);
	KT_ASSERT(s_eq(b, "abc", 3));
	KT_ASSERT_ER(fs_close(fd[1]), EX_OK);
	KT_ASSERT_EQ(fs_read(fd[0], b, sizeof(b)), 0);	/* no end writes: the end of the file */
	KT_ASSERT_ER(fs_close(fd[0]), EX_OK);
}

/* A second descriptor shares the open file: the pipe ends only when both are closed */
LOCAL void test_dup( void )
{
	INT	fd[2], w2;
	UB	b[8];

	KT_ASSERT_ER(fs_pipe(fd), EX_OK);
	w2 = fs_dup(fd[1]);
	KT_ASSERT(w2 >= 0);
	KT_ASSERT_ER(fs_close(fd[1]), EX_OK);
	KT_ASSERT_EQ(fs_write(w2, "x", 1), 1);
	KT_ASSERT_EQ(fs_read(fd[0], b, sizeof(b)), 1);
	KT_ASSERT_ER(fs_close(w2), EX_OK);
	KT_ASSERT_EQ(fs_read(fd[0], b, sizeof(b)), 0);
	fs_close(fd[0]);
}

/* ---------------------------------------------------------------- the object world */

LOCAL void test_paths( void )
{
	T_FSXSTAT x;
	INT	fd, n;
	UB	b[32];

	KT_ASSERT_ER(fs_xstat("/", &x), EX_OK);
	KT_ASSERT_EQ(x.st.mode & FS_IFMT, FS_IFDIR);
	KT_ASSERT_ER(fs_xstat("/bin", &x), EX_OK);
	KT_ASSERT_EQ(x.st.mode & FS_IFMT, FS_IFDIR);
	KT_ASSERT_ER(fs_xstat("/home/BTRON", &x), EX_OK);
	KT_ASSERT_EQ(x.st.mode & FS_IFMT, FS_IFDIR);
	KT_ASSERT_ER(fs_xstat("/tmp", &x), EX_OK);
	KT_ASSERT(fs_xstat("/no such name", &x) == EX_NOENT);

	/* a file made in tmp, read back, renamed and removed */
	fd = fs_open("/tmp/ktest psx.txt", O_RDWR | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	KT_ASSERT_EQ(fs_write(fd, "hello\n", 6), 6);
	KT_ASSERT_EQ(fs_lseek(fd, 0, SEEK_SET_), 0);
	n = fs_read(fd, b, sizeof(b));
	KT_ASSERT_EQ(n, 6);
	KT_ASSERT(s_eq(b, "hello\n", 6));
	fs_close(fd);
	KT_ASSERT_ER(fs_xstat("/tmp/ktest psx.txt", &x), EX_OK);
	KT_ASSERT_EQ(x.st.size, 6);
	KT_ASSERT_ER(fs_rename("/tmp/ktest psx.txt", "/tmp/改名した.txt"), EX_OK);
	KT_ASSERT_ER(fs_xstat("/tmp/改名した.txt", &x), EX_OK);
	KT_ASSERT_ER(fs_unlink("/tmp/改名した.txt"), EX_OK);
	KT_ASSERT(fs_xstat("/tmp/改名した.txt", &x) == EX_NOENT);
}

/* ---------------------------------------------------------------- terminals */

/* /dev comes once the task that keeps it has run */
LOCAL BOOL dev_ready( void )
{
	T_FSXSTAT x;
	INT	i;

	for ( i = 0; i < 50; i++ ) {
		if ( fs_xstat("/dev/ptmx", &x) >= EX_OK ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/* The slave of a new pty, its master in *p_m */
LOCAL INT pty_open( INT *p_m )
{
	char	path[16] = "/dev/pts";
	INT	m, s, no = -1, k = 8;

	m = fs_open("/dev/ptmx", O_RDWR);
	if ( m < 0 ) {
		tm_printf((UB *)"psx: /dev/ptmx does not open (%d)\n", m);
		return m;
	}
	if ( fs_ioctl(m, FS_IOC_GPTN, &no) < EX_OK ) {
		fs_close(m);
		return EX_IO;
	}
	if ( no >= 10 ) path[k++] = (char)( '0' + no / 10 );
	path[k++] = (char)( '0' + no % 10 );
	path[k] = 0;
	s = fs_open(path, O_RDWR);
	if ( s < 0 ) {
		tm_printf((UB *)"psx: %s does not open (%d)\n", path, s);
		fs_close(m);
		return s;
	}
	*p_m = m;
	return s;
}

LOCAL void test_pty( void )
{
	INT		m = -1, s, n;
	UB		b[64];
	T_FSTERMIOS	t;

	if ( !dev_ready() ) KT_SKIP("no /dev/ptmx");
	s = pty_open(&m);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	KT_ASSERT_ER(fs_ioctl(s, FS_IOC_ISTTY, NULL), EX_OK);

	/* a line edited: the 'b' taken back */
	KT_ASSERT_EQ(fs_write(m, "ab\177c\r", 5), 5);
	n = fs_read(s, b, sizeof(b));
	KT_ASSERT_EQ(n, 3);
	KT_ASSERT(s_eq(b, "ac\n", 3));
	n = read_all(m, b, sizeof(b), 100);
	KT_ASSERT(s_has(b, n, "ab\b \bc\r\n"));		/* the echo */

	/* a newline written goes out as carriage return and newline */
	KT_ASSERT_EQ(fs_write(s, "x\n", 2), 2);
	n = read_all(m, b, sizeof(b), 100);
	KT_ASSERT(s_eq(b, "x\r\n", 3) && n == 3);

	/* raw: each byte as it comes, no echo */
	KT_ASSERT_ER(fs_ioctl(s, FS_IOC_TCGETS, &t), EX_OK);
	t.c_lflag &= ~( 0x100 | 0x8 );		/* ICANON, ECHO */
	KT_ASSERT_ER(fs_ioctl(s, FS_IOC_TCSETS, &t), EX_OK);
	KT_ASSERT_EQ(fs_write(m, "q", 1), 1);
	KT_ASSERT_EQ(fs_read(s, b, sizeof(b)), 1);
	KT_ASSERT_EQ(b[0], 'q');
	KT_ASSERT_EQ(read_all(m, b, sizeof(b), 50), 0);

	fs_close(s);
	fs_close(m);
}

/* ---------------------------------------------------------------- a POSIX program */

/* The start-up argument of lib/libposix's spawn: words, no environment, fds 0..nfd-1 */
LOCAL INT arg_make( UB *a, INT max, CONST char *const *words, INT nw, INT nfd )
{
	UW	*h;
	INT	at, i, k;

	for ( i = 0; i < 8; i++ ) a[i] = (UB)"\0TSPX1\0"[i];
	h = (UW *)( a + 8 );
	at = 8 + 8 * 4;
	h[1] = (UW)nw;			/* argc */
	h[2] = 0;			/* envc */
	h[3] = (UW)nfd;
	h[4] = 022;
	h[5] = 0;
	h[6] = (UW)at;			/* the map */
	for ( i = 0; i < nfd; i++ ) ((UW *)( a + at ))[i] = (UW)i;
	at += 4 * nfd;
	h[7] = (UW)at;			/* the strings */
	for ( i = 0; i < nw; i++ ) {
		for ( k = 0; words[i][k] != 0 && at < max - 2; k++ ) a[at++] = (UB)words[i][k];
		a[at++] = 0;
	}
	for ( k = 0; "/tmp"[k] != 0; k++ ) a[at++] = (UB)"/tmp"[k];
	a[at++] = 0;
	h[0] = (UW)at;
	return at;
}

/* A program started with the words, its 0, 1 and 2 the descriptors given */
LOCAL ID start_prog( CONST char *path, CONST char *const *words, INT nw, INT in, INT out )
{
	UB	*a = (UB *)Kmalloc(4096);
	T_SPAWN	sp;
	INT	fds[3];
	ID	pid;

	if ( a == NULL ) return E_NOMEM;
	knl_memset(&sp, 0, sizeof(sp));
	fds[0] = in;
	fds[1] = out;
	fds[2] = out;
	sp.argsz = arg_make(a, 4096, words, nw, 3);
	sp.arg = a;
	sp.nfd = 3;
	sp.fds = fds;
	sp.pri = 20;
	pid = ts_spn_prc(path, &sp);
	Kfree(a);
	return pid;
}

LOCAL ID start( CONST char *const *words, INT nw, INT in, INT out )
{
	return start_prog(PSXTEST, words, nw, in, out);
}

LOCAL INT run( CONST char *const *words, INT nw, UB *out, INT max, INT *p_code )
{
	T_PSTS	ps;
	INT	p[2], in[2], n;
	ID	pid;

	if ( fs_pipe(p) < EX_OK || fs_pipe(in) < EX_OK ) return -1;
	pid = start(words, nw, in[0], p[1]);
	fs_close(p[1]);
	fs_close(in[0]);
	fs_close(in[1]);
	if ( pid <= 0 ) {
		fs_close(p[0]);
		return (INT)pid;
	}
	n = read_all(p[0], out, max, 5000);
	fs_close(p[0]);
	*p_code = ( ts_wai_prc(pid, &ps, 5000) >= E_OK ) ? ps.exitcd : -9999;
	return n;
}

LOCAL void test_spawn( void )
{
	static CONST char *const w[] = { "psxtest", "args", "hello", "日本語の 語" };
	UB	b[256];
	INT	n, code = -1;
	T_FSTAT	st;

	if ( fs_stat(PSXTEST, &st) < EX_OK ) KT_SKIP("no " PSXTEST);
	n = run(w, 4, b, sizeof(b), &code);
	tm_printf((UB *)"psx: spawn said [%s] code %d\n", b, code);
	KT_ASSERT(n > 0);
	KT_ASSERT(s_has(b, n, "arg hello\narg 日本語の 語\n"));
	KT_ASSERT_EQ(code, 0);
}

LOCAL void test_exit_code( void )
{
	static CONST char *const w[] = { "psxtest", "exit", "42" };
	UB	b[64];
	INT	code = -1;
	T_FSTAT	st;

	if ( fs_stat(PSXTEST, &st) < EX_OK ) KT_SKIP("no " PSXTEST);
	(void)run(w, 3, b, sizeof(b), &code);
	KT_ASSERT_EQ(code, 42);
}

LOCAL void test_signals( void )
{
	static CONST char *const w[] = { "psxtest", "sig" };
	UB	b[256];
	INT	n, code = -1;
	T_FSTAT	st;

	if ( fs_stat(PSXTEST, &st) < EX_OK ) KT_SKIP("no " PSXTEST);
	n = run(w, 2, b, sizeof(b), &code);
	tm_printf((UB *)"psx: sig said [%s] code %d\n", b, code);
	KT_ASSERT(s_has(b, n, "ok sig"));
	KT_ASSERT_EQ(code, 0);
}

/* A signal it does not catch ends it, and its parent hears which */
LOCAL void test_kill( void )
{
	static CONST char *const w[] = { "psxtest", "kill" };
	T_PSTS	ps;
	UB	b[64];
	INT	p[2], in[2], n;
	ID	pid;
	T_FSTAT	st;

	if ( fs_stat(PSXTEST, &st) < EX_OK ) KT_SKIP("no " PSXTEST);
	KT_ASSERT_ER(fs_pipe(p), EX_OK);
	KT_ASSERT_ER(fs_pipe(in), EX_OK);
	pid = start(w, 2, in[0], p[1]);
	fs_close(p[1]);
	fs_close(in[0]);
	KT_ASSERT(pid > 0);
	n = read_all(p[0], b, sizeof(b), 300);
	KT_ASSERT(s_has(b, n, "ready"));
	KT_ASSERT_ER(knl_psx_kill(pid, TS_SIGTERM), E_OK);
	KT_ASSERT_ER(ts_wai_prc(pid, &ps, 5000), E_OK);
	KT_ASSERT_EQ(ps.exitcd, TS_ABORT_SIG(TS_SIGTERM));
	fs_close(p[0]);
	fs_close(in[1]);
}

/* A line typed at a terminal reaches the program reading it */
LOCAL void test_tty_program( void )
{
	static CONST char *const w[] = { "psxtest", "tty" };
	T_PSTS	ps;
	UB	b[256];
	INT	m = -1, s, n;
	ID	pid;
	T_FSTAT	st;

	if ( fs_stat(PSXTEST, &st) < EX_OK ) KT_SKIP("no " PSXTEST);
	if ( !dev_ready() ) KT_SKIP("no /dev/ptmx");
	s = pty_open(&m);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	pid = start(w, 2, s, s);
	fs_close(s);
	KT_ASSERT(pid > 0);
	n = read_all(m, b, sizeof(b), 300);
	KT_ASSERT(s_has(b, n, "tty 1"));
	KT_ASSERT_EQ(fs_write(m, "かな漢字\r", 13), 13);
	n = read_all(m, b, sizeof(b), 1000);
	tm_printf((UB *)"psx: tty said [%s]\n", b);
	KT_ASSERT(s_has(b, n, "line [かな漢字\r\n]"));	/* its newline written out as CR LF */
	KT_ASSERT_ER(ts_wai_prc(pid, &ps, 5000), E_OK);
	KT_ASSERT_EQ(ps.exitcd, 0);
	fs_close(m);
}

/* ---------------------------------------------------------------- the commands (make CMDS=1) */

/*
 * ls /tmp | grep 日本語 | wc -l: three commands of bin, joined by pipes,
 * find the one object of /tmp with a Japanese name
 */
LOCAL void test_cmd_pipeline( void )
{
	static CONST char *const w_ls[] = { "ls", "/tmp" };
	static CONST char *const w_grep[] = { "grep", "日本語" };
	static CONST char *const w_wc[] = { "wc", "-l" };
	T_FSTAT	st;
	T_PSTS	ps;
	UB	b[256];
	INT	a[2], c[2], d[2], f, n, i;
	ID	pid[3];

	if ( fs_stat("/bin/ls", &st) < EX_OK ) KT_SKIP("no /bin/ls (make CMDS=1)");
	f = fs_open("/tmp/日本語の名前", 0x0001 | 0x0100 | 0x0400);
	KT_ASSERT(f >= 0);
	if ( f < 0 ) return;
	(void)fs_write(f, "x\n", 2);
	fs_close(f);

	KT_ASSERT_ER(fs_pipe(a), EX_OK);
	KT_ASSERT_ER(fs_pipe(c), EX_OK);
	KT_ASSERT_ER(fs_pipe(d), EX_OK);
	pid[0] = start_prog("/bin/ls", w_ls, 2, d[0], a[1]);
	fs_close(a[1]);
	fs_close(d[0]);
	pid[1] = start_prog("/bin/grep", w_grep, 2, a[0], c[1]);
	fs_close(a[0]);
	fs_close(c[1]);
	KT_ASSERT_ER(fs_pipe(a), EX_OK);
	pid[2] = start_prog("/bin/wc", w_wc, 2, c[0], a[1]);
	fs_close(c[0]);
	fs_close(a[1]);
	fs_close(d[1]);
	n = read_all(a[0], b, sizeof(b), 10000);
	fs_close(a[0]);
	tm_printf((UB *)"psx: ls | grep | wc said [%s]\n", b);
	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT(pid[i] > 0);
		if ( pid[i] > 0 ) {
			KT_ASSERT_ER(ts_wai_prc(pid[i], &ps, 10000), E_OK);
			KT_ASSERT_EQ(ps.exitcd, 0);
		}
	}
	for ( i = 0; i < n && ( b[i] == ' ' || b[i] == '\t' ); i++ ) ;
	KT_ASSERT(i < n && b[i] == '1' && ( i + 1 == n || b[i + 1] == '\n' ));
	(void)fs_unlink("/tmp/日本語の名前");
}

/* ps lists the processes, itself among them, with what the kernel tells of them */
LOCAL void test_cmd_ps( void )
{
	static CONST char *const w[] = { "ps", "ax" };
	T_FSTAT	st;
	UB	b[2048];
	INT	n, code = -1;

	if ( fs_stat("/bin/ps", &st) < EX_OK ) KT_SKIP("no /bin/ps (make CMDS=1)");
	{
		T_PSTS	ps;
		INT	p[2], in[2];
		ID	pid;

		KT_ASSERT_ER(fs_pipe(p), EX_OK);
		KT_ASSERT_ER(fs_pipe(in), EX_OK);
		pid = start_prog("/bin/ps", w, 2, in[0], p[1]);
		fs_close(p[1]);
		fs_close(in[0]);
		fs_close(in[1]);
		KT_ASSERT(pid > 0);
		n = read_all(p[0], b, sizeof(b), 10000);
		fs_close(p[0]);
		if ( pid > 0 && ts_wai_prc(pid, &ps, 10000) >= E_OK ) code = ps.exitcd;
	}
	tm_printf((UB *)"psx: ps said [%s] code %d\n", b, code);
	KT_ASSERT(s_has(b, n, "PID"));
	KT_ASSERT(s_has(b, n, " ps"));
	KT_ASSERT_EQ(code, 0);
}

/*
 * tsh with a script: subshells, $( ), a function in a pipeline, all of
 * them processes started without fork, and a pipeline of programs
 * finding a Japanese name
 */
LOCAL void test_tsh_script( void )
{
	static CONST char *const w[] = { "tsh", "-c",
		"x=1; f() { echo \"f:$1:$x\"; }; "
		"echo \"a:$(echo sub)\"; "
		"( x=2; echo \"paren:$x\" ); echo \"after:$x\"; "
		"f arg | cat; "
		"for i in 1 2 3; do echo $i; done | tr -d '\\n'; echo; "
		"echo \"n=$(ls /tmp | grep 日本語 | wc -l | tr -d ' ')\"; "
		"y=$(f z); echo \"y=$y\"; "
		"false || echo or-ok; "
		"case abc in a*) echo case-ok;; esac; "
		"echo \"m=$((3*4))\"; "
		"cd /tmp && echo \"pwd=$(pwd)\"; "
		"ls | cat; "
		"echo \"c=$(ls | wc -l | tr -d ' ')\"; "
		"( echo a ); "
		"exit 7" };
	T_FSTAT	st;
	T_PSTS	ps;
	UB	b[1024];
	INT	p[2], in[2], f, n, code = -1;
	ID	pid;

	if ( fs_stat("/bin/tsh", &st) < EX_OK ) KT_SKIP("no /bin/tsh (make CMDS=1)");
	f = fs_open("/tmp/日本語の名前", 0x0001 | 0x0100 | 0x0400);
	if ( f >= 0 ) fs_close(f);
	KT_ASSERT_ER(fs_pipe(p), EX_OK);
	KT_ASSERT_ER(fs_pipe(in), EX_OK);
	pid = start_prog("/bin/tsh", w, 3, in[0], p[1]);
	fs_close(p[1]);
	fs_close(in[0]);
	fs_close(in[1]);
	KT_ASSERT(pid > 0);
	n = read_all(p[0], b, sizeof(b), 20000);
	fs_close(p[0]);
	if ( pid > 0 && ts_wai_prc(pid, &ps, 20000) >= E_OK ) code = ps.exitcd;
	tm_printf((UB *)"psx: tsh said [%s] code %d\n", b, code);
	KT_ASSERT(s_has(b, n, "a:sub\n"));
	KT_ASSERT(s_has(b, n, "paren:2\nafter:1\n"));
	KT_ASSERT(s_has(b, n, "f:arg:1\n"));
	KT_ASSERT(s_has(b, n, "123\n"));
	KT_ASSERT(s_has(b, n, "n=1\n"));
	KT_ASSERT(s_has(b, n, "y=f:z:1\n"));
	KT_ASSERT(s_has(b, n, "or-ok\ncase-ok\nm=12\n"));
	KT_ASSERT(s_has(b, n, "pwd=/tmp\n"));
	KT_ASSERT(s_has(b, n, "日本語の名前\n"));
	KT_ASSERT(s_has(b, n, "c="));
	KT_ASSERT(!s_has(b, n, "c=0\n"));
	KT_ASSERT(s_has(b, n, "\na\n"));
	KT_ASSERT_EQ(code, 7);
	(void)fs_unlink("/tmp/日本語の名前");
}

#ifdef KT_SSH_PORT
IMPORT ER  so_dhcp_start( void );
IMPORT ER  so_getifaddr( UW *p_addr, UW *p_mask, UW *p_gw );

/* What the terminal's master gives, until the text has come or the time is out */
LOCAL BOOL pty_until( INT m, CONST char *text, UB *b, INT max, INT ms )
{
	INT	have = 0, waited = 0;

	while ( waited < ms ) {
		T_FSPOLL	p;

		p.fd = m;
		p.events = FS_POLL_IN;
		p.revents = 0;
		if ( fs_poll(&p, 1, 200) > 0 && ( p.revents & FS_POLL_IN ) != 0 ) {
			INT	n = fs_read(m, b + have, max - have - 1);

			/* the program gone: its terminal stays readable with nothing to read */
			if ( n <= 0 ) break;
			if ( n > 0 ) {
				have += n;
				if ( s_has(b, have, text) ) {
					b[have] = 0;
					return TRUE;
				}
				if ( have > max / 2 ) {
					knl_memcpy(b, b + have - 64, 64);
					have = 64;
				}
			}
		} else {
			waited += 200;
		}
	}
	b[have] = 0;
	tm_printf((UB *)"psx: ssh: waited in vain for [%s]; it said [%s]\n", text, b);
	return FALSE;
}

LOCAL void pty_say( INT m, CONST char *text )
{
	(void)fs_write(m, text, knl_strlen(text));
}

/*
 * make SSH=1 CMDS=1 NET=user: the command ssh, on a terminal, to the sshd
 * on the host (tools/ssh/sshd_host.sh). The key is asked about; a shell
 * there answers what is typed; it ends with the shell. Then one command,
 * and a wrong password that lets nothing in. Last the person's key: made
 * and kept in the objects, not known to the server so the password is
 * still asked (which puts its public line in authorized_keys), then let
 * in with no password.
 */
LOCAL void test_ssh( void )
{
	CONST char *const w[] = { "ssh", "-p", "2222", "kts@10.0.2.2" };
	CONST char *const w2[] = { "ssh", "-p", "2222", "kts@10.0.2.2", "echo", "from-the-server" };
	static UB	b[4096];
	T_FSTAT		st;
	T_PSTS		ps;
	UW		addr = 0;
	INT		m = -1, s, t, code = -1;
	ID		pid;

	if ( fs_stat("/bin/ssh", &st) < EX_OK ) KT_SKIP("no /bin/ssh (make SSH=1 CMDS=1)");
	(void)so_dhcp_start();
	for ( t = 0; t < 150 && ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ); t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(addr != 0);

	s = pty_open(&m);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	pid = start_prog("/bin/ssh", w, 4, s, s);
	fs_close(s);
	KT_ASSERT(pid > 0);
	KT_ASSERT(pty_until(m, "Type yes", b, sizeof(b), 60000));
	pty_say(m, "yes\n");
	KT_ASSERT(pty_until(m, "Password:", b, sizeof(b), 30000));
	pty_say(m, "ktpass\n");
	tk_dly_tsk(2000);
	pty_say(m, "echo marker$((40+2))\n");
	KT_ASSERT(pty_until(m, "marker42", b, sizeof(b), 30000));
	pty_say(m, "exit\n");
	if ( pid > 0 && ts_wai_prc(pid, &ps, 30000) >= E_OK ) code = ps.exitcd;
	KT_ASSERT_EQ(code, 0);
	fs_close(m);

	/* the key known by now; a command, its output, and the wrong password first */
	m = -1;
	code = -1;
	s = pty_open(&m);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	pid = start_prog("/bin/ssh", w2, 6, s, s);
	fs_close(s);
	KT_ASSERT(pty_until(m, "Password:", b, sizeof(b), 60000));
	KT_ASSERT(!s_has(b, knl_strlen((char *)b), "Type yes"));
	pty_say(m, "wrongpass\n");
	KT_ASSERT(pty_until(m, "Not right. Password:", b, sizeof(b), 30000));
	pty_say(m, "ktpass\n");
	KT_ASSERT(pty_until(m, "from-the-server", b, sizeof(b), 30000));
	if ( pid > 0 && ts_wai_prc(pid, &ps, 30000) >= E_OK ) code = ps.exitcd;
	KT_ASSERT_EQ(code, 0);
	fs_close(m);

	{
		CONST char *const kw[] = { "ssh", "-K", "-b", "2048", "-f" };
		CONST char *const yw[] = { "ssh", "-Y" };
		static char	inst[1800];
		CONST char	*w3[] = { "ssh", "-p", "2222", "kts@10.0.2.2", inst };
		CONST char *const w4[] = { "ssh", "-p", "2222", "kts@10.0.2.2", "echo", "by-the-key" };
		char		*l, *e;

		m = -1;
		code = -1;
		s = pty_open(&m);
		KT_ASSERT(s >= 0);
		if ( s < 0 ) return;
		pid = start_prog("/bin/ssh", kw, 5, s, s);
		fs_close(s);
		KT_ASSERT(pty_until(m, "The key is kept", b, sizeof(b), 300000));
		tm_printf((UB *)"psx: ssh: the key made and kept\n");
		if ( pid > 0 && ts_wai_prc(pid, &ps, 30000) >= E_OK ) code = ps.exitcd;
		KT_ASSERT_EQ(code, 0);
		fs_close(m);

		/* the line as -Y gives it from the objects, which proves them read back */
		m = -1;
		code = -1;
		s = pty_open(&m);
		KT_ASSERT(s >= 0);
		if ( s < 0 ) return;
		pid = start_prog("/bin/ssh", yw, 2, s, s);
		fs_close(s);
		b[0] = 0;
		if ( pid > 0 && ts_wai_prc(pid, &ps, 60000) >= E_OK ) code = ps.exitcd;
		KT_ASSERT_EQ(code, 0);
		KT_ASSERT(pty_until(m, "ssh-rsa ", b, sizeof(b), 10000));
		{
			INT		have = (INT)knl_strlen((char *)b);
			T_FSPOLL	p;

			/* the rest of the line, if any: the program has ended, and nothing more comes */
			p.fd = m;
			p.events = FS_POLL_IN;
			p.revents = 0;
			if ( fs_poll(&p, 1, 500) > 0 && ( p.revents & FS_POLL_IN ) != 0 ) {
				INT	g = fs_read(m, b + have, sizeof(b) - have - 1);

				if ( g > 0 ) b[have + g] = 0;
			}
		}
		fs_close(m);
		for ( l = (char *)b; *l != 0 && ( l[0] != 's' || l[3] != '-' || l[7] != ' ' ); l++ ) {
		}
		for ( e = l; *e != 0 && *e != '\r' && *e != '\n'; e++ ) {
		}
		KT_ASSERT(e > l + 200);
		*e = 0;
		tm_printf((UB *)"psx: ssh: the key read back: %s\n", l);
		tm_sprintf((UB *)inst, (UB *)"mkdir -p .ssh; echo '%s' >> .ssh/authorized_keys; echo stored-it", l);

		m = -1;
		code = -1;
		s = pty_open(&m);
		KT_ASSERT(s >= 0);
		if ( s < 0 ) return;
		pid = start_prog("/bin/ssh", w3, 5, s, s);
		fs_close(s);
		KT_ASSERT(pty_until(m, "Password:", b, sizeof(b), 60000));
		pty_say(m, "ktpass\n");
		KT_ASSERT(pty_until(m, "stored-it", b, sizeof(b), 30000));
		if ( pid > 0 && ts_wai_prc(pid, &ps, 30000) >= E_OK ) code = ps.exitcd;
		KT_ASSERT_EQ(code, 0);
		fs_close(m);

		m = -1;
		code = -1;
		s = pty_open(&m);
		KT_ASSERT(s >= 0);
		if ( s < 0 ) return;
		pid = start_prog("/bin/ssh", w4, 6, s, s);
		fs_close(s);
		KT_ASSERT(pty_until(m, "by-the-key", b, sizeof(b), 60000));
		KT_ASSERT(!s_has(b, knl_strlen((char *)b), "Password:"));
		if ( pid > 0 && ts_wai_prc(pid, &ps, 30000) >= E_OK ) code = ps.exitcd;
		KT_ASSERT_EQ(code, 0);
		fs_close(m);
	}
}
#endif

EXPORT void ktest_psx( void )
{
	KT_RUN(test_pipe);
	KT_RUN(test_dup);
	KT_RUN(test_paths);
	KT_RUN(test_pty);
	KT_RUN(test_spawn);
	KT_RUN(test_exit_code);
	KT_RUN(test_signals);
	KT_RUN(test_kill);
	KT_RUN(test_tty_program);
	KT_RUN(test_cmd_pipeline);
	KT_RUN(test_cmd_ps);
	KT_RUN(test_tsh_script);
#ifdef KT_SSH_PORT
	KT_RUN(test_ssh);
#endif
}
