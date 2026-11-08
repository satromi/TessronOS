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
 * TS_PRC_USER: the process acts for the person at the machine -- the
 * user logged in at the desktop, or the ordinary user 利用者 when no one
 * is (design 18.10) -- rather than as the system its creator is. Taken
 * only from a creator that is not itself a process, as a process's
 * child acts as its parent does whatever it says; a program the desktop
 * starts is started so (ob_cre_obj of a process object by the kernel).
 */
#define TS_PRC_USER		0x0002

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
			    UD *p_entry, UD *p_brk, void **p_img );
/* The shared image of a program a space maps, let go when the space is taken down (design 5.1) */
IMPORT void knl_elf_img_put( void *img );
/* The images of an object (uuid NULL: of the whole volume) not to be used again: it is to be written */
IMPORT void knl_elf_forget( ID vol, CONST TS_UUID *uuid );
/* Loads of a program from a record that mapped a shared image, and those that read the program */
IMPORT void knl_elf_counts( UW *p_hits, UW *p_reads );

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
 * Pages the kernel holds at kva (knl_vmap: each page wherever it lies)
 * mapped for the process to write, one after another at a new address
 * of the shared window, which *p_va is given; and pages there put back
 * on the page of nothing, so that what the process still writes there
 * goes nowhere rather than into pages given to something else.
 */
IMPORT ER  knl_prc_map_kernel( ID pid, CONST void *kva, UD size, UBINT *p_va );
IMPORT ER  knl_prc_sink_shared( ID pid, UBINT va, UD size );

/*
 * fn called with the process's space (sysman/space.h T_SPACE) and arg,
 * under the process table's lock, so that the space does not go while
 * fn maps into it or takes a mapping out (the GPU's BOs, 13-gpu.md
 * 13.5). Its answer, or E_NOEXS when the process has no space.
 */
IMPORT ER  knl_prc_space_call( ID pid, ER (*fn)( void *space, void *arg ), void *arg );

/*
 * Whether len bytes at p are the process's own to hand the kernel: in
 * its half of the address space, every page of them mapped for it, and
 * writable by it when the kernel is to write there. What a system call
 * takes by pointer is looked at this way before the kernel touches it,
 * and touched only with the way to the process's pages open (PAN,
 * kernel/sysdepend/cpu/core/armv8a/sysdepend.h).
 */
IMPORT BOOL knl_prc_user_ok( ID pid, CONST void *p, SZ len, BOOL write );

/*
 * The exception handler: the kernel touched a page of the space ttbr0
 * that EL0 may reach without opening the way (PAN). When the running
 * task is one of that process's, the process is marked to end as the
 * call returns, and the fault is recorded as TS_FLT_KPAN; TRUE then.
 */
IMPORT BOOL knl_prc_kpan( UD ttbr0, ID tskid, UBINT va, UD esr, UD elr, UD lr );
IMPORT ID  knl_prc_of_uuid( CONST TS_UUID *uuid );
/* The calling process: its number, main task and base priority; 0 when the caller is not one */
IMPORT ID  knl_prc_self( ID *p_main, PRI *p_base );
IMPORT INT knl_prc_list( ID *pids, INT n );
/* All of them, however many, in memory to Kfree: how many */
IMPORT INT knl_prc_list_all( ID **p_pids );
/* The machine going off: every process but the system's told to end, then ended by force: how many are left */
IMPORT INT knl_prc_end_all( UW term_ms, void (*tick)( void ) );

/*
 * A process ended by a processor exception (design 9.11), as the
 * exception handler saw it: kept for the desktop, which shows it in a
 * dialog. The handler only records it; the desktop's own task takes it
 * with knl_prc_fault_take. Only the exception that ends a process is
 * recorded, once for the process.
 */
#define TS_FLT_EL0		1	/* an instruction of the program's own (EL0) */
#define TS_FLT_KCALL		2	/* the kernel, at an address the program handed it in a call */
#define TS_FLT_KPAN		3	/* the kernel, at a page of the program's it had not made sure of (PAN) */

#define TS_FLT_FILE		32	/* bytes of the file name kept */
#define TS_FLT_CODE		32	/* bytes of the program's code kept, from elr - TS_FLT_CODE_BACK */
#define TS_FLT_CODE_BACK	16

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
	/*
	 * TS_FLT_EL0: the code around elr as the program's pages hold it
	 * when the process ends, read through its own mapping, to be set
	 * beside the instruction the processor took the exception at
	 */
	UB	code[TS_FLT_CODE];	/* from elr - TS_FLT_CODE_BACK */
	UW	codeok;			/* bit i: code[i * 4] to code[i * 4 + 3] were read */
	UD	codepa;			/* the physical address elr maps to, ~0 when none */
} T_PRCFLT;

/*
 * A fault of the program's own handed back to it (design 9.11.1): a
 * data abort at an address the process has nothing at, or may not
 * touch so, which its memory cannot put right. When the program has
 * named an entry (ts_def_flt), the task that met it goes on there, on
 * its own stack below where it was, with x0 the address of this
 * picture of where it was. The entry decides; with ts_flt_ret it goes
 * back to what the picture then says (a program that turns the fault
 * into something of its own moves pc), and the access is made again
 * if pc was left as it was. With no entry the process ends, as before.
 *
 * x19 to x28 are not in the picture: the entry keeps them, as a
 * function does, and they are what they were when it returns. The
 * picture's FP/SIMD registers are the task's at the fault and are put
 * back by ts_flt_ret.
 */
#if defined(__x86_64__)
/*
 * x86-64: the general registers but rsp in the order rax, rbx, rcx,
 * rdx, rsi, rdi, rbp, r8-r15 (rbx and r12-r15 left 0: the entry
 * keeps them, as a function does), rsp, rip and the arithmetic flags of
 * rflags; far is the address met (CR2) and esr the exception's vector
 * in bits 39:32 and its error code below; fpsr is MXCSR, fpcr the x87
 * control and status words; vregs the FXSAVE image of x87 and
 * xmm0-xmm15 (the upper halves of the ymm registers are not in it)
 */
typedef struct {
	UD	regs[16];		/* rax-r15 but rsp; regs[15] 0 */
	UD	sp, pc, pstate;		/* rsp, rip, the arithmetic flags */
	UD	far, esr;		/* the address met, the vector and error code */
	UD	fpsr, fpcr;		/* MXCSR, FCW | FSW << 16 */
	UD	vregs[64] __attribute__((aligned(16)));	/* the FXSAVE image */
} T_TSFLT;
#else
typedef struct {
	UD	regs[31];		/* x0-x30 (x19-x28 left 0) */
	UD	sp, pc, pstate;		/* SP_EL0, where it was, its NZCV */
	UD	far, esr;		/* the address met, the syndrome */
	UD	fpsr, fpcr;
	UD	vregs[64] __attribute__((aligned(16)));	/* q0-q31, the low half of each first */
} T_TSFLT;
#endif

/* The entry: f is called with the picture; NULL takes it away */
IMPORT ER ts_def_flt( void (*f)( T_TSFLT *flt ) );
/* Back to where the picture says: no return when it is good */
IMPORT ER ts_flt_ret( CONST T_TSFLT *flt );

/*
 * The exception handler: a fault of a task of a process at EL0 the
 * process's memory did not put right. When the process named an entry
 * and its stack has room, the frame is turned to it and TRUE comes
 * back, the task to go on there.
 */
IMPORT BOOL knl_prc_flt_give( void *frame, UD esr, UD far );
/* ts_flt_ret, from the SVC gateway with the call's own frame */
IMPORT ER   knl_prc_flt_return( void *frame, CONST T_TSFLT *uflt );

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
/* The file it was started from, without a lock or memory: for the lockup report */
IMPORT INT  knl_prc_file_peek( ID pid, UB *buf, INT max );

/*
 * End a process from the keyboard (Shift+Pause): as ts_ter_prc with
 * TS_ABORT_TERM. E_OACV for a process the system needs (TS_PRC_SYSTEM,
 * init among them), E_NOEXS for one that is not running, E_OBJ for one
 * already ending.
 */
IMPORT ER   knl_prc_force_end( ID pid );

/*
 * End a process that has not ended since it was asked to, its tasks
 * still inside calls of the kernel (design 5.5.4): the waits of them
 * that only wait for something to happen are cut, and what has not
 * come out a second later is stopped where it stands. E_OACV for a
 * process the system needs, E_NOEXS for none, E_OBJ for one already
 * ended this way and not yet collected.
 */
IMPORT ER   knl_prc_kill( ID pid );

/* What the list of 強制終了 shows of a process (knl_prc_info) */
typedef struct {
	ID	pid;
	ID	ppid;
	UINT	state;			/* PS_* */
	BOOL	sys;			/* the system needs it: not ended from the keyboard */
	BOOL	ending;			/* asked to end */
	BOOL	killed;			/* ended by force (knl_prc_kill) */
	BOOL	done;			/* its end told: only collecting it is left */
	UW	ending_ms;		/* since it was asked to end */
	INT	incall;			/* calls of its tasks under way in the kernel's layers */
	INT	ntsk;
	UD	memsz;			/* bytes mapped */
	UD	cpu_ms;			/* the time its tasks have run */
} T_PRCINFO;

IMPORT ER   knl_prc_info( ID pid, T_PRCINFO *pi );

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
