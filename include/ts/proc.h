/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	proc.h
 *	Process management (design 9.4, 9.5)
 *
 *	First stage: a process is created from an ELF64 program held in a
 *	file of the file system. When TSFS is in place the same call takes
 *	the UUID of a real object instead, and this path stays as the way
 *	the loader reads a program out of /boot.
 */

#ifndef __TS_PROC_H__
#define __TS_PROC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

#define TS_PRC_ANY		(-1)		/* ts_wai_prc: any child */

/*
 * How many objects of the core one process may have (design 9.7). Its
 * main task counts among its tasks. Past these, or when the tables have
 * no more room than the kernel keeps for itself, a call answers E_LIMIT.
 */
#define TS_PRC_TSK_MAX		256
#define TS_PRC_SEM_MAX		1024
#define TS_PRC_FLG_MAX		256
#define TS_PRC_MTX_MAX		1024
#define TS_PRC_MBF_MAX		64

/* Process state */
#define PS_NONEXIST		0
#define PS_RUNNING		1
#define PS_EXITING		2
#define PS_ZOMBIE		3

/* Abnormal exit codes (design 9.11) */
#define TS_ABORT_FAULT		(-256)		/* memory access fault */
#define TS_ABORT_ILL		(-257)		/* illegal instruction */
#define TS_ABORT_TERM		(-258)		/* terminated by ts_ter_prc */

/*
 * Process attributes (T_CPRC.prcatr). TS_PRC_SYSTEM: the system needs
 * the process -- init is started so -- and it is not ended from the
 * keyboard (Shift+Pause, knl_prc_force_end). Taken only from a creator
 * that is not itself a process, so that no program can shield its own
 * children with it.
 */
#define TS_PRC_SYSTEM		0x0001

/*
 * Process creation information
 */
typedef struct {
	PRI	pri;			/* priority of the main task (0: the caller's) */
	ATR	prcatr;
	SZ	stksz;			/* user stack of the main task (0: default) */
	CONST void *arg;		/* start-up argument, copied into the space */
	SZ	argsz;
} T_CPRC;

/*
 * Process state information
 */
typedef struct {
	ID	pid;
	ID	ppid;
	UINT	state;			/* PS_* */
	INT	exitcd;
	INT	ntsk;
	UD	memsz;			/* bytes mapped in the space */
	ID	maintsk;
} T_RPRC;

typedef struct {
	INT	exitcd;
} T_PSTS;

/*
 * Process API. The kernel side implements them; a process reaches them
 * through the SVC gateway (class 1).
 */
IMPORT ID  ts_cre_prc( CONST char *path, CONST T_CPRC *pk_cprc );
IMPORT ID  ts_cre_prc_obj( ID vol, CONST TS_UUID *uuid, CONST T_CPRC *pk_cprc );
IMPORT void ts_ext_prc( INT exitcd );			/* no return */
IMPORT ER  ts_ter_prc( ID pid, INT abortcd );
IMPORT ER  ts_wai_prc( ID pid, T_PSTS *pk_psts, TMO tmout );
IMPORT ER  ts_ref_prc( ID pid, T_RPRC *pk_rprc );
IMPORT ID  ts_get_pid( void );
IMPORT SZ  ts_get_arg( void *buf, SZ size );	/* the start-up argument; its size */

/*
 * Process message queue (design 9.9). Each process has one queue; a
 * message is copied, so the sender keeps its own buffer. The low half of
 * 'type' is for the program, the high half for the system.
 */
#define TS_MSG_MAX	256		/* bytes of one message */
#define TS_MSG_QLEN	8		/* messages held at a time */

#define TS_MSG_CHILD_EXIT	0x00010000	/* a child ended */
#define TS_MSG_TERM_REQ		0x00020000	/* please end */

typedef struct {
	UINT	type;
	ID	from;			/* sender, 0 for the kernel */
	SZ	size;
	UB	body[TS_MSG_MAX];
} T_TSMSG;

IMPORT ER  ts_snd_msg( ID pid, CONST T_TSMSG *msg, TMO tmout );
IMPORT ER  ts_rcv_msg( T_TSMSG *buf, TMO tmout );

/* The first program the system starts by itself (design 9.14) */
IMPORT ID  knl_start_init( CONST char *path );
IMPORT ID  knl_init_pid( void );

/*
 * Program loader (kernel/sysman/elf.c)
 */
IMPORT ER knl_elf_load( CONST char *path, void *space, UD *p_entry, UD *p_brk );
IMPORT ER knl_elf_load_rec( ID vol, CONST TS_UUID *uuid, INT recno, void *space,
			    UD *p_entry, UD *p_brk );

IMPORT ER knl_proc_init( void );

/* A process as a real object (design 18.8) */
IMPORT ER  knl_prc_uuid( ID pid, TS_UUID *p_uuid, TS_UUID *p_prog );

/*
 * Pages mapped into a process at an address of the shared window, and
 * taken away again; the physical address a process has at an address
 * (~0 for none). Done under the process table's lock, so a process
 * that is ending is not mapped into while its space goes.
 */
IMPORT ER  knl_prc_map_shared( ID pid, UBINT va, UD pa, UD size, BOOL write );
IMPORT ER  knl_prc_unmap_shared( ID pid, UBINT va, UD size );
IMPORT UD  knl_prc_lookup( ID pid, UBINT va );

/*
 * Whether len bytes at p are the process's own to hand the kernel: in
 * its half of the address space, every page of them mapped for it, and
 * writable by it when the kernel is to write there. What a system call
 * takes by pointer is looked at this way before the kernel touches it.
 */
IMPORT BOOL knl_prc_user_ok( ID pid, CONST void *p, SZ len, BOOL write );
IMPORT ID  knl_prc_of_uuid( CONST TS_UUID *uuid );
/* The calling process: its number, main task and base priority; 0 when the caller is not one */
IMPORT ID  knl_prc_self( ID *p_main, PRI *p_base );
IMPORT INT knl_prc_list( ID *pids, INT n );

/*
 * A process ended by a processor exception (design 9.11), as the
 * exception handler saw it: kept for the desktop, which shows it in a
 * dialog. The handler only records it; the desktop's own task takes it
 * with knl_prc_fault_take. Only the exception that ends a process is
 * recorded, once for the process.
 */
#define TS_FLT_EL0		1	/* an instruction of the program's own (EL0) */
#define TS_FLT_KCALL		2	/* the kernel, at an address the program handed it in a call */

#define TS_FLT_FILE		32	/* bytes of the file name kept */

typedef struct {
	UW	seq;			/* counted from 1, in the order they happened */
	UINT	kind;			/* TS_FLT_* */
	ID	pid;
	ID	tskid;			/* the task that met it */
	INT	exitcd;			/* TS_ABORT_FAULT, TS_ABORT_ILL */
	TS_UUID	prog;			/* the program object it was started from, 0 none */
	UB	file[TS_FLT_FILE];	/* the file it was started from, the last part of the path */
	UD	esr;			/* the exception syndrome (ESR_EL1) */
	UD	far;			/* the address it met (FAR_EL1) */
	UD	elr;			/* where it was (ELR_EL1): the program's PC, or the kernel's */
	UD	lr;			/* x30 at that moment */
} T_PRCFLT;

/*
 * The exception handler: a task of the process on ttbr0 faulted at EL0
 * and the process ends with abortcd. f carries the kind and the
 * registers; the rest is filled in here.
 */
IMPORT void knl_prc_abort( UD ttbr0, INT abortcd, CONST T_PRCFLT *f );

/*
 * The oldest exception not yet taken: TRUE and *f filled, FALSE when
 * none waits. Only a few are kept; *p_lost (when not NULL) is how many
 * older ones had to be let go since the last call to make room.
 */
IMPORT BOOL knl_prc_fault_take( T_PRCFLT *f, UW *p_lost );

/*
 * A process's name for a person: its program object's name, or the
 * file it was started from, or "pid N". The bytes written, UTF-8.
 * knl_prc_flt_name says the same of a recorded exception.
 */
IMPORT INT  knl_prc_name( ID pid, UB *buf, INT max );
IMPORT INT  knl_prc_flt_name( CONST T_PRCFLT *f, UB *buf, INT max );

/*
 * End a process from the keyboard (Shift+Pause): as ts_ter_prc with
 * TS_ABORT_TERM. E_OACV for a process the system needs (TS_PRC_SYSTEM,
 * init among them), E_NOEXS for one that is not running, E_OBJ for one
 * already ending.
 */
IMPORT ER   knl_prc_force_end( ID pid );

/*
 * Calls of a process into the layers whose state a task taken down in
 * the middle would leave half changed: files (class 2), sockets
 * (class 5), objects (class 7), drawing (class 8) and windows
 * (class 9), bracketed by the SVC gateway.
 * Between enter and leave the main task is not taken down: ts_ter_prc
 * marks the process instead, the long calls and the waits in them see
 * knl_prc_ending between their pieces and slices and return, and leave
 * ends the task there.
 */
IMPORT ER   knl_prc_call_enter( void );
IMPORT void knl_prc_call_leave( BOOL entered );
IMPORT BOOL knl_prc_ending( void );

/*
 * A fault the kernel met at an address of the running process's half
 * while it worked for the process (the exception handler, with the
 * syndrome and where the kernel was): TRUE when it was put right for
 * the call to go on, the process ending as the call returns.
 * knl_prc_kfaults counts the processes marked so and not yet ended.
 */
IMPORT BOOL knl_prc_kfault( UD ttbr0, ID tskid, UBINT va, UD esr, UD elr, UD lr );
IMPORT volatile INT knl_prc_kfaults;

/*
 * The kernel's view of the calling process's memory at uaddr for a
 * device to move data to or from ('into': the device writes it), and
 * how many of len bytes from there lie in one run of memory. Only for
 * the main task inside a call bracketed as above, whose pages stay
 * until it returns; E_PAR otherwise.
 */
IMPORT INT  knl_prc_dma( CONST void *uaddr, INT len, BOOL into, void **p_kaddr );

#ifdef __cplusplus
}
#endif

#endif /* __TS_PROC_H__ */
