/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	con_job.c
 *	コンソール: a program run from the command line (design 16.5.19)
 *
 *	A program of the POSIX layer -- one whose metadata says
 *	tessronos.program.posix, tsh and the standard commands of bin -- is
 *	started on a pseudo terminal of its own (design 5.8.4): the console
 *	keeps the master, the program has the slave as its standard input,
 *	output and error and as the terminal of a session it leads, and is
 *	started with ts_spn_obj and the start-up argument of the POSIX layer
 *	(lib/libposix: argv, an environment, the place it starts in, the
 *	descriptors). What it writes is read from the master and shown as a
 *	terminal shows it (lterm/lt_vt.c); the keys typed are written to the
 *	master, so the terminal's own line editing, ^C and the rest work as
 *	on any other terminal.
 *
 *	Any other program object is made a process (ob_cre_obj of
 *	OB_T_PROCESS); the words after the command go to it as its start-up
 *	argument: one line of UTF-8, the words a space apart, ended with 0
 *	-- the argument ts_get_arg gives it -- and no argument at all when
 *	there are none, as the desktop starts a program. What it writes on
 *	its standard output and its standard error goes to the system's
 *	console as every process's does (tm_putstring), and the console's
 *	record brings it into the window with the rest.
 *
 *	The console is the parent of either: it waits for it (ts_wai_prc)
 *	and may end it.
 */

#include "console.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/psx.h>
#include <ts/json.h>

#define PSX_MAGIC	"\0TSPX1\0"	/* lib/libposix/psx_start.c: the spawn's form */
#define PSX_AF_SETCTTY	0x0002		/* the slave on descriptor 0 the session's terminal */
#define PSX_HOME	"/home"		/* the object home: the cabinet the desktop opens with is in it */
#define PLACE_MAX	1024
#define BLOB_MAX	4096

LOCAL ID	pid;			/* the program running, 0 none */
LOCAL TS_UUID	proc;
LOCAL UB	pname[OB_NAME_MAX];
LOCAL BOOL	stopping;
LOCAL UB	arg[CON_LINE_MAX];
LOCAL INT	ptm = -1;		/* the pty's master, a POSIX program running on it; -1 none */
LOCAL UB	blob[BLOB_MAX];

/*
 * Where the console waits (its window's channel), and the keys whose
 * notices come there: the end of the program running (OB_E_EXIT of its
 * process object) and what its pseudo terminal has for the master
 * (OB_E_CHANGE of record 1 of ptsN), so that the console sleeps until
 * one of them, or its window, has something
 */
LOCAL ID	wport = 0;
LOCAL ID	kexit = 0, kpts = 0;

EXPORT void job_port( ID port )
{
	wport = port;
}

/* Told to the console's channel of events ev of record recno of the object u: the key, or 0 */
LOCAL ID watch( CONST TS_UUID *u, INT recno, UINT ev )
{
	T_OBNTF	req;
	ID	k;

	if ( wport <= 0 ) return 0;
	k = ob_opn_obj(u, OB_OP_ATRRD);
	if ( k <= 0 ) return 0;
	memset(&req, 0, sizeof(req));
	req.events = ev;
	if ( ob_ntf_evt(k, recno, &req, wport) <= 0 ) {
		ob_cls_obj(k);
		return 0;
	}
	return k;
}

LOCAL void unwatch( ID *p_key )
{
	if ( *p_key > 0 ) ob_cls_obj(*p_key);
	*p_key = 0;
}

/* The object of the device of that name ("pts3"): found among the device objects */
LOCAL BOOL dev_named( CONST UB *name, TS_UUID *u )
{
	TS_UUID	ids[16], last;
	T_OBREF	r;
	INT	cnt, i, k, all = 0;

	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_DEVICE, 0, ( all > 0 ) ? &last : NULL, ids, 16, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&ids[i], &r) < E_OK ) continue;
			for ( k = 0; name[k] != 0 && r.name[k] == name[k]; k++ ) ;
			if ( name[k] == 0 && r.name[k] == 0 ) {
				*u = ids[i];
				return TRUE;
			}
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == 16 );
	return FALSE;
}

LOCAL ID pid_of( CONST TS_UUID *pu );

/* The process object of the process numbered pid: found among the process objects */
LOCAL BOOL proc_of( ID n, TS_UUID *u )
{
	TS_UUID	ids[16], last;
	INT	cnt, i, all = 0;

	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_PROCESS, 0, ( all > 0 ) ? &last : NULL, ids, 16, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt; i++ ) {
			if ( pid_of(&ids[i]) == n ) {
				*u = ids[i];
				return TRUE;
			}
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == 16 );
	return FALSE;
}

/* A process's number, as its object says it */
LOCAL ID pid_of( CONST TS_UUID *pu )
{
	UB	b[16];
	SZ	asz = 0;
	ID	key, n = 0;
	INT	i;

	/* the number its metadata says (tessronos.process.pid), not its name */
	key = ob_opn_obj(pu, OB_OP_ATRRD);
	if ( key <= 0 ) return 0;
	if ( ob_get_atv(key, (CONST UB *)"tessronos.process.pid", b, sizeof(b) - 1, &asz) < E_OK ) asz = 0;
	ob_cls_obj(key);
	for ( i = 0; i < (INT)asz && b[i] >= '0' && b[i] <= '9'; i++ ) n = n * 10 + ( b[i] - '0' );
	return n;
}

/* ---------------------------------------------------------------- a POSIX program on a pty */

/* Whether its metadata says it is a program of the POSIX layer */
LOCAL BOOL is_posix( CONST TS_UUID *prog )
{
	UB		b[8];
	SZ		asz = 0;
	ID		key;
	BOOL		yes = FALSE;

	/* the one member asked for, whatever the length of the rest (no memory to read it whole) */
	key = ob_opn_obj(prog, OB_OP_ATRRD);
	if ( key <= 0 ) return FALSE;
	if ( ob_get_atv(key, (CONST UB *)"tessronos.program.posix", b, sizeof(b), &asz) >= E_OK
	  && asz == 4 && b[0] == 't' && b[1] == 'r' && b[2] == 'u' && b[3] == 'e' ) {
		yes = TRUE;
	}
	ob_cls_obj(key);
	return yes;
}

LOCAL INT put_str( INT at, CONST UB *s, BOOL unescape )
{
	for ( ; *s != 0 && at < BLOB_MAX - 2; s++ ) {
		if ( unescape && *s == '\\' && s[1] != 0 ) s++;
		blob[at++] = *s;
	}
	blob[at++] = 0;
	return at;
}

/*
 * Where the program starts: the object the console is in, by the path
 * that names it from "/", when that path reaches that very object (the
 * console's path is made of the objects' names, the file layer's of
 * the names it shows); otherwise home.
 */
LOCAL CONST UB *start_place( void )
{
	static UB	p[PLACE_MAX];
	TS_UUID		here, at;
	INT		i;

	box_where(p, sizeof(p));
	box_here_uuid(&here);
	if ( p[0] != '/' || ts_lkp_obj((CONST char *)p, &at) < E_OK ) return (CONST UB *)PSX_HOME;
	for ( i = 0; i < (INT)sizeof(at); i++ ) {
		if ( at.b[i] != here.b[i] ) return (CONST UB *)PSX_HOME;
	}
	return p;
}

/*
 * The start-up argument of the POSIX layer: a magic, its numbers, the
 * POSIX number of each descriptor handed over (0, 1, 2: the slave),
 * then argv, the environment and the place it starts in
 */
LOCAL INT make_blob( CONST UB *name, INT argc, UB **argv, INT rows, INT cols )
{
	static CONST char *const env[] = {
		"PATH=/bin", "HOME=" PSX_HOME, "TERM=tessron", "LANG=ja_JP.UTF-8",
		"SHELL=/bin/tsh", NULL
	};
	UW	*h = (UW *)( blob + 8 );
	UB	num[16];
	INT	at, i, envc = 0;

	for ( i = 0; i < 8; i++ ) blob[i] = (UB)PSX_MAGIC[i];
	at = 8 + 8 * 4;
	h[1] = (UW)argc;
	h[3] = 3;
	h[4] = 022;
	h[5] = PSX_AF_SETCTTY;
	h[6] = (UW)at;
	for ( i = 0; i < 3; i++ ) ((UW *)( blob + at ))[i] = (UW)i;
	at += 3 * 4;
	h[7] = (UW)at;
	at = put_str(at, name, FALSE);
	for ( i = 1; i < argc; i++ ) at = put_str(at, argv[i], TRUE);
	for ( i = 0; env[i] != NULL; i++, envc++ ) at = put_str(at, (CONST UB *)env[i], FALSE);
	(void)lt_num(num, rows);
	at = put_str(at, (CONST UB *)"LINES=", FALSE) - 1;
	at = put_str(at, num, FALSE);
	envc++;
	(void)lt_num(num, cols);
	at = put_str(at, (CONST UB *)"COLUMNS=", FALSE) - 1;
	at = put_str(at, num, FALSE);
	envc++;
	at = put_str(at, start_place(), FALSE);
	h[2] = (UW)envc;
	h[0] = (UW)at;
	return at;
}

LOCAL void set_size( INT fd, INT rows, INT cols )
{
	T_FSWINSZ	ws;

	ws.ws_row = (UH)rows;
	ws.ws_col = (UH)cols;
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;
	(void)fs_ioctl(fd, FS_IOC_SWINSZ, &ws);
}

/* The program started on a new pty; E_OK, or an error and nothing started */
LOCAL ER pty_start( CONST TS_UUID *prog, CONST UB *name, INT argc, UB **argv, INT rows, INT cols )
{
	char	path[16] = "/dev/pts";
	T_SPAWN	sp;
	INT	m, s, no = -1, k = 8, fds[3];
	ID	id;

	m = fs_open("/dev/ptmx", 0x0002 | FS_O_NONBLOCK | FS_O_NOCTTY);
	if ( m < 0 ) return E_NOEXS;
	if ( fs_ioctl(m, FS_IOC_GPTN, &no) < EX_OK || no < 0 ) {
		fs_close(m);
		return E_NOEXS;
	}
	if ( no >= 10 ) path[k++] = (char)( '0' + no / 10 % 10 );
	path[k++] = (char)( '0' + no % 10 );
	path[k] = 0;
	s = fs_open(path, 0x0002 | FS_O_NOCTTY);
	if ( s < 0 ) {
		fs_close(m);
		return E_NOEXS;
	}
	set_size(m, rows, cols);

	memset(&sp, 0, sizeof(sp));
	sp.arg = blob;
	sp.argsz = make_blob(name, argc, argv, rows, cols);
	fds[0] = fds[1] = fds[2] = s;
	sp.nfd = 3;
	sp.fds = fds;
	sp.flags = TS_SPN_SETSID;
	/* the program the link found, wherever it was found and whatever names stand on the way */
	id = ts_spn_obj(prog, &sp);
	fs_close(s);
	if ( id <= 0 ) {
		fs_close(m);
		return ( id < 0 ) ? (ER)id : E_SYS;
	}
	pid = id;
	ptm = m;
	/* told when the program writes and when it ends; whatever came before is read as the loop comes round */
	{
		UB	pts[8] = "pts";
		TS_UUID	u;
		INT	j = 3;

		if ( no >= 10 ) pts[j++] = (UB)( '0' + no / 10 % 10 );
		pts[j++] = (UB)( '0' + no % 10 );
		pts[j] = 0;
		if ( dev_named(pts, &u) ) kpts = watch(&u, 1, OB_E_CHANGE);
		if ( proc_of(pid, &u) ) kexit = watch(&u, -1, OB_E_EXIT);
	}
	return E_OK;
}

EXPORT BOOL job_on_pty( void )
{
	return (BOOL)( ptm >= 0 );
}

EXPORT INT job_pty_read( UB *buf, INT max )
{
	INT	n;

	if ( ptm < 0 ) return 0;
	n = fs_read(ptm, buf, max);
	return ( n > 0 ) ? n : 0;
}

EXPORT void job_pty_write( CONST UB *s, INT n )
{
	INT	k;

	while ( ptm >= 0 && n > 0 ) {
		k = fs_write(ptm, s, n);
		if ( k == EX_AGAIN ) {
			tk_dly_tsk(10);
			continue;
		}
		if ( k <= 0 ) break;
		s += k;
		n -= k;
	}
}

EXPORT void job_pty_size( INT rows, INT cols )
{
	if ( ptm >= 0 ) set_size(ptm, rows, cols);
}

EXPORT void job_pty_close( void )
{
	unwatch(&kpts);
	if ( ptm >= 0 ) {
		fs_close(ptm);
		ptm = -1;
	}
}

/* ---------------------------------------------------------------- starting */

EXPORT ER job_start( CONST TS_UUID *prog, CONST UB *name, INT argc, UB **argv, INT rows, INT cols )
{
	T_OBCRE	c;
	INT	i, n = 0;
	ER	er;

	for ( i = 0; name[i] != 0 && i < OB_NAME_MAX - 1; i++ ) pname[i] = name[i];
	pname[i] = 0;
	stopping = FALSE;

	if ( is_posix(prog) && pty_start(prog, name, argc, argv, rows, cols) >= E_OK ) {
		return E_OK;
	}

	/* the words, their escapes taken off, a space apart */
	for ( i = 1; i < argc; i++ ) {
		CONST UB *w = argv[i];

		if ( i > 1 && n < CON_LINE_MAX - 1 ) arg[n++] = ' ';
		for ( ; *w != 0 && n < CON_LINE_MAX - 1; w++ ) {
			if ( *w == '\\' && w[1] != 0 ) w++;
			arg[n++] = *w;
		}
	}
	arg[n] = 0;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = *prog;
	if ( argc > 1 ) {
		c.arg = arg;
		c.argsz = n + 1;
	}
	er = ob_cre_obj(&c, &proc);
	if ( er < E_OK ) {
		say_u(name);
		say(": 起動できませんでした (");
		say_num(er);
		say(")\n");
		return er;
	}
	pid = pid_of(&proc);
	if ( pid <= 0 ) {
		say_u(name);
		say(": 起動したプロセスが見つかりません\n");
		pid = 0;
		return E_SYS;
	}
	kexit = watch(&proc, -1, OB_E_EXIT);	/* an end before this is found as the loop comes round */
	return E_OK;
}

EXPORT BOOL job_running( void )
{
	return (BOOL)( pid > 0 );
}

/* A child's end told to the process's queue: taken off, as the wait answers it */
LOCAL void drain( void )
{
	static T_TSMSG	m;
	INT		k;

	for ( k = 0; k < TS_MSG_QLEN && ts_rcv_msg(&m, TMO_POL) >= E_OK; k++ ) ;
}

EXPORT BOOL job_poll( void )
{
	T_PSTS	ps;
	ER	er;
	BOOL	pty = (BOOL)( ptm >= 0 );

	if ( pid <= 0 ) return FALSE;
	er = ts_wai_prc(pid, &ps, TMO_POL);
	if ( er == E_TMOUT ) return FALSE;
	drain();
	pid = 0;
	unwatch(&kexit);
	if ( pty && er >= E_OK && ps.exitcd == 0 ) {
		return TRUE;			/* a shell or a command that ended well: nothing to say */
	}
	say_u(pname);
	if ( er < E_OK ) {
		say(": 終わりを待てませんでした (");
		say_num(er);
		say(")\n");
	} else if ( ps.exitcd == TS_ABORT_TERM ) {
		say(": 止めました\n");
	} else if ( ps.exitcd == TS_ABORT_FAULT ) {
		say(": メモリの例外で終わりました\n");
	} else if ( ps.exitcd == TS_ABORT_ILL ) {
		say(": 不正な命令で終わりました\n");
	} else {
		say(": 終了コード ");
		say_num(ps.exitcd);
		say("\n");
	}
	return TRUE;
}

EXPORT void job_stop( void )
{
	ER	er;

	if ( pid <= 0 || stopping ) return;
	if ( ptm >= 0 ) {
		/* the terminal hung up: its session is told SIGHUP, as a closed terminal's is */
		job_pty_close();
		(void)ts_kil_prc(pid, TS_SIGHUP);
		(void)ts_kil_prc(pid, TS_SIGCONT);
		stopping = TRUE;
		return;
	}
	er = ts_ter_prc(pid, TS_ABORT_TERM);
	if ( er < E_OK ) {
		say_u(pname);
		say(": 止められません (");
		say_num(er);
		say(")\n");
		return;
	}
	stopping = TRUE;
}
