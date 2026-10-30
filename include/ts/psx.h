/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx.h
 *	What a POSIX program needs of a process beyond include/ts/proc.h
 *	(design 5.8): starting a program with descriptors handed to it,
 *	process groups and sessions, signals, and waiting for children
 *	that end, stop or go on.
 *
 *	There is no fork. A program is started by ts_spn_prc from a
 *	program object named by its path in the object world (objfs), with
 *	the start-up argument the C library builds -- arguments, environment,
 *	where it is -- and the descriptors it is to have, which the child
 *	takes with ts_get_inh. lib/libposix builds posix_spawn, execve,
 *	waitpid, kill, sigaction and the rest of them on these.
 *
 *	Signals are numbered as in NetBSD (TS_SIG*). Each process has the
 *	signals it catches, ignores and holds back (its mask) and those
 *	that came and are not yet handed over. One it neither catches nor
 *	ignores does what it does by default at once: ends the process
 *	(with TS_ABORT_SIG(n)), stops its tasks, lets them go on, or
 *	nothing. One it catches is handed to it as one of its tasks comes
 *	back from a system call: the task goes on at the entry named with
 *	ts_sig_set, on its own stack below where it was, with x0 the
 *	address of a T_TSSIGFR there -- the picture of where it was, as a
 *	fault's (include/ts/proc.h T_TSFLT), x0 in it what the call
 *	answered -- and x1 the signal. The entry runs the program's
 *	handler and goes back with ts_sig_ret. A call that waits (a pipe, a
 *	terminal, ts_wai_psx, ts_sig_sus, ts_slp_psx) gives up waiting with
 *	E_RLWAI (EX_INTR from fs_) when a signal comes that is to be
 *	handed over, so that it is handed over at once.
 */

#ifndef __TS_PSX_H__
#define __TS_PSX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/proc.h>

/* ---------------------------------------------------------------- signals */

#define TS_SIGHUP	1
#define TS_SIGINT	2
#define TS_SIGQUIT	3
#define TS_SIGILL	4
#define TS_SIGTRAP	5
#define TS_SIGABRT	6
#define TS_SIGEMT	7
#define TS_SIGFPE	8
#define TS_SIGKILL	9
#define TS_SIGBUS	10
#define TS_SIGSEGV	11
#define TS_SIGSYS	12
#define TS_SIGPIPE	13
#define TS_SIGALRM	14
#define TS_SIGTERM	15
#define TS_SIGURG	16
#define TS_SIGSTOP	17
#define TS_SIGTSTP	18
#define TS_SIGCONT	19
#define TS_SIGCHLD	20
#define TS_SIGTTIN	21
#define TS_SIGTTOU	22
#define TS_SIGIO	23
#define TS_SIGXCPU	24
#define TS_SIGXFSZ	25
#define TS_SIGVTALRM	26
#define TS_SIGPROF	27
#define TS_SIGWINCH	28
#define TS_SIGINFO	29
#define TS_SIGUSR1	30
#define TS_SIGUSR2	31
#define TS_NSIG		32

#define TS_SIGBIT(n)	( 1UL << (n) )

/* The exit code of a process a signal ended (as ts_ter_prc and T_PSTS.exitcd have it) */
#define TS_ABORT_SIG(n)		( -512 - (n) )
#define TS_ABORT_IS_SIG(c)	( (c) <= -513 && (c) >= -512 - TS_NSIG )
#define TS_ABORT_SIGNO(c)	( -512 - (c) )

/* ts_sig_set: which members of T_TSSIG to take */
#define TS_SS_CATCH	0x0001
#define TS_SS_IGN	0x0002
#define TS_SS_MASK	0x0004
#define TS_SS_ENTRY	0x0008
#define TS_SS_BLOCK	0x0010		/* mask |= the mask given */
#define TS_SS_UNBLOCK	0x0020		/* mask &= ~the mask given */

typedef struct {
	UD	catch_;			/* caught: handed over at the entry */
	UD	ign;			/* ignored: dropped as they come */
	UD	mask;			/* held back until let through */
	UD	entry;			/* where a caught one is handed over, 0 none */
	UD	pend;			/* (out) come and not yet handed over */
} T_TSSIG;

/* What the entry is given: the picture, and the mask to put back with ts_sig_ret */
typedef struct {
	T_TSFLT	flt;
	UD	oldmask;
	INT	sig;
	INT	code;			/* TS_SI_* */
	ID	from;			/* the process that sent it, 0 the kernel */
	INT	status;			/* SIGCHLD: the child's, as ts_wai_psx says it */
} T_TSSIGFR;

#define TS_SI_USER	0		/* sent by a process (ts_kil_prc) */
#define TS_SI_KERNEL	1		/* by the system: a terminal, a pipe, a child */

IMPORT ER  ts_sig_set( UINT what, CONST T_TSSIG *in, T_TSSIG *old );
IMPORT ER  ts_sig_ret( CONST T_TSSIGFR *fr );		/* no return when it is good */

/*
 * Sending: pid > 0 that process, 0 every process of the caller's group,
 * below -1 every process of group -pid. sig 0 only asks whether there
 * is one it may send to. Allowed to the caller's own children, to the
 * processes that act for the same user, and to administrators.
 */
IMPORT ER  ts_kil_prc( ID pid, INT sig );

/*
 * Wait with the mask set to 'mask' until a signal it lets through is
 * handed over (E_RLWAI); the old mask is what the handler's return
 * puts back.
 */
IMPORT ER  ts_sig_sus( UD mask );

/* Sleep up to ns; E_RLWAI when a signal cut it short, the time left in *p_left */
IMPORT ER  ts_slp_psx( UD ns, UD *p_left );

/* SIGALRM after ms (0: none); the ms that were left of the one before */
IMPORT INT ts_sig_alm( UINT ms );

/* ---------------------------------------------------------------- groups */

IMPORT ER  ts_set_pgid( ID pid, ID pgid );	/* pid 0: the caller; pgid 0: pid */
IMPORT ID  ts_get_pgid( ID pid );
IMPORT ID  ts_set_sid( void );			/* a new session and group, led by the caller */
IMPORT ID  ts_get_sid( ID pid );
IMPORT ID  ts_get_ppid( void );

/* ---------------------------------------------------------------- starting */

#define TS_SPN_ARG_MAX	( 256 * 1024 )	/* bytes of the start-up argument */
#define TS_SPN_FD_MAX	256		/* descriptors handed to a child */

/* T_SPAWN.flags */
#define TS_SPN_PGID	0x0001		/* put it in group pgid (0: a group of its own) */
#define TS_SPN_SETSID	0x0002		/* a session of its own */
#define TS_SPN_SIGDEF	0x0004		/* sigdef: those back to what they do by default */

typedef struct {
	CONST void *arg;		/* the start-up argument, at most TS_SPN_ARG_MAX */
	SZ	argsz;
	INT	nfd;			/* descriptors of the file layer the child is given */
	CONST INT *fds;			/* the caller's, in the order the child takes them */
	UINT	flags;			/* TS_SPN_* */
	ID	pgid;
	UD	sigign;			/* ignored in the child: what the caller ignores and these */
	UD	sigmask;		/* the child's mask */
	UD	sigdef;			/* TS_SPN_SIGDEF: ignored no longer */
	PRI	pri;			/* 0: the caller's */
	SZ	stksz;			/* the main task's stack, 0 the default */
} T_SPAWN;

/*
 * A program object started by its path in the object world (objfs, or
 * a path the caller may run a file by): the caller must be allowed to
 * run it (OB_OP_EXEC). The child acts as the caller does, is in the
 * caller's group and session unless the flags say other, and ignores
 * what the caller ignores. E_NOEXS: no such program; E_PAR: not one
 * that can be run (ENOEXEC).
 */
IMPORT ID  ts_spn_prc( CONST char *path, CONST T_SPAWN *sp );

/*
 * The same for the program object prog itself, as a link points at it:
 * what found the program by its link starts that one, whatever names
 * stand on the way to it.
 */
IMPORT ID  ts_spn_obj( CONST TS_UUID *prog, CONST T_SPAWN *sp );

/*
 * The object a path of the object world (objfs, from "/") names, looked
 * up with the caller's rights: what holds a place as an object asks
 * whether a path it made reaches that very object. E_NOEXS: no such
 * path; E_OACV: a box on the way the caller may not read.
 */
IMPORT ER  ts_lkp_obj( CONST char *path, TS_UUID *p_uuid );

/* The descriptors the caller was given when it was started, in order; how many */
IMPORT INT ts_get_inh( INT *fds, INT max );

/*
 * Waiting for a child (POSIX waitpid): pid > 0 that one, -1 any, 0
 * those of the caller's group, below -1 those of group -pid. The
 * status in *p_status as POSIX's macros read it: an exit (code << 8), a
 * signal (n), stopped ((n << 8) | 0x7f), gone on (0xffff). Answers the
 * child's pid, 0 with TS_WP_NOHANG when none is ready, E_NOEXS when
 * there is no such child, E_RLWAI when a signal came.
 */
#define TS_WP_NOHANG	0x0001
#define TS_WP_UNTRACED	0x0002
#define TS_WP_CONTINUED	0x0010

IMPORT ID  ts_wai_psx( ID pid, UINT options, INT *p_status );

/*
 * A process's POSIX state for ps (and for the tests): its group,
 * session, the terminal it belongs to (-1 none), whether it is
 * stopped, and the signals as T_TSSIG has them.
 */
typedef struct {
	ID	pid, ppid, pgid, sid;
	INT	tty;			/* number of the pty, -1 none */
	BOOL	stopped;
	UD	catch_, ign, mask, pend;
	UINT	state;			/* PS_* of include/ts/proc.h */
	UD	memsz;			/* bytes mapped */
	UD	cpu_ms;			/* the time its tasks have run */
	UB	name[32];		/* its program object's name or file, UTF-8 */
} T_PSXREF;

IMPORT ER  ts_ref_psx( ID pid, T_PSXREF *pk );

/* The numbers of the processes there are, at most max of them; how many */
IMPORT INT ts_lst_prc( ID *pids, INT max );

/* ---------------------------------------------------------------- the kernel's */

/* Sent by the kernel: a terminal's ^C, a pipe nobody reads, a child */
IMPORT ER   knl_psx_kill( ID pid, INT sig );
IMPORT ER   knl_psx_killpg( ID pgid, INT sig );

/*
 * Whether the calling task's process has a signal to be handed over
 * (caught and not held back) or is to end: what a call that waits
 * looks at between its slices.
 */
IMPORT BOOL knl_psx_intr( void );

/* The caller's group and session, 0 when it is not a process */
IMPORT ID   knl_psx_pgid( void );
IMPORT ID   knl_psx_sid( void );
IMPORT BOOL knl_psx_pg_in_sid( ID pgid, ID sid );
IMPORT BOOL knl_psx_sig_blocked( INT sig );	/* the caller ignores or holds back sig */

/* The terminal the caller's session has (-1 none), and setting it (the session's leader) */
IMPORT INT  knl_psx_ctty( void );
IMPORT ER   knl_psx_set_ctty( INT tty );
IMPORT void knl_psx_tty_gone( INT tty );	/* a terminal closed: no session has it */

/*
 * SVC gateway: a caught signal handed over on the frame of the call it
 * returns from (TRUE: the frame was turned to the entry), and the
 * return from its handler (ts_sig_ret) on the frame of that call.
 */
IMPORT BOOL knl_psx_deliver( void *frame, UD ret );
IMPORT ER   knl_psx_sig_return( void *frame, CONST T_TSSIGFR *ufr );

IMPORT ER   knl_psx_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_PSX_H__ */
