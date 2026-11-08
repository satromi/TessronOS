/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07.
 *
 *----------------------------------------------------------------------
 */

/*
 *	kernel.h
 *	micro T-Kernel Common Definition
 */

#ifndef _KERNEL_
#define _KERNEL_

#include <sys/machine.h>
#include <sys/queue.h>

#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <tk/dbgspt.h>

#include "tstdlib.h"

/*
 * Task control block (TCB)
 */
typedef struct task_control_block	TCB;

/* Task System Dependent definition
	There is no definition for this in the standard.
	It is redefined in the system-dependent section as necessary.
*/
#define TCB_SYSDEPEND_INFO		/* None */

#include "klk.h"
#include "../tkernel/timer.h"
#include "../tkernel/winfo.h"
#include "../tkernel/mutex.h"

#include "prcset.h"

/* The processor holding a task's context (TCB.runprc): PRC 1..CNF_MAX_PRCID, 0 none */
#if CNF_MAX_PRCID > 255
typedef UH	RUNPRC;
#else
typedef UB	RUNPRC;
#endif

#include "../sysdepend/sys_msg.h"
#include "../sysdepend/cpu_status.h"
#include "../sysdepend/sysdepend.h"

#define SYSCALL		EXPORT		/* Definition of system call */
#if USE_TIME_US_API
#define SYSCALL_U	SYSCALL		/* Microsecond system call (design 7.6) */
#else
#define SYSCALL_U	LOCAL		/* only the ms wrapper is public */
#endif

/* User defined handler ( Sub-system calls, time-event handler ) */
# define CallUserHandlerP1(   p1,         hdr, cb)	(*(void(*)(UBINT))(hdr))((UBINT)(p1))
# define CallUserHandlerP2(   p1, p2,     hdr, cb)	(*(void(*)(UBINT,UBINT))(hdr))((UBINT)(p1), (UBINT)(p2))
# define CallUserHandlerP3(   p1, p2, p3, hdr, cb)	(*(void(*)(UBINT,UBINT,UBINT))(hdr))((UBINT)(p1), (UBINT)(p2), (UBINT)(p3))

/*
 * Task control block (TCB)
 */
struct task_control_block {
	QUEUE	tskque;		/* Task queue */
	ID	tskid;		/* Task ID */
	void	*exinf;		/* Extended information */
	ATR	tskatr;		/* Task attribute */
	FP	task;		/* Task startup address */
	CTXB	tskctxb;	/* Task context block */
	W	sstksz;		/* stack size */

	B	isysmode;	/* Task operation mode initial value */
	H	sysmode;	/* Task operation mode, quasi task part call level */

	UB	ipriority;	/* Priority at task startup */
	UB	bpriority;	/* Base priority */
	UB	priority;	/* Current priority */

	UB /*TSTAT*/	state;	/* Task state (Int. expression) */
	RUNPRC	runprc;		/* Processor holding the register context (0: none), dispatch.S */
	UW	sch_round;	/* the scheduler's pass that last chose it (smp.c): one look, however many processors */

	BOOL	klockwait:1;	/* TRUE at wait kernel lock */
	BOOL	klocked:1;	/* TRUE at hold kernel lock */

	CONST WSPEC *wspec;	/* Wait specification */
	ID	wid;		/* Wait object ID */
	INT	wupcnt;		/* Number of wakeup requests queuing */
	INT	suscnt;		/* Number of SUSPEND request nests */
	ER	*wercd;		/* Wait error code set area */
	WINFO	winfo;		/* Wait information */
	TMEB	wtmeb;		/* Wait timer event block */

	void	*isstack;	/* stack pointer initial value */
	T_PRCSET assprc;	/* Processors allowed (bit n: PRC n+1), empty: any */

#if USE_LEGACY_API && USE_RENDEZVOUS
	RNO	wrdvno;		/* For creating rendezvous number */
#endif
#if USE_MUTEX == 1
	MTXCB	*mtxlist;	/* List of hold mutexes */
#endif
#if KLK_FINE_OBJ
	/*
	 * The task's priority is to be worked out again before the task
	 * control lock is let go (mutex.c, design 4.3.3): the next of the
	 * list, and whether it is in it. Under that lock.
	 */
	struct task_control_block *pri_next;
	BOOL	pri_queued;
#endif

#if USE_DBGSPT && defined(USE_FUNC_TD_INF_TSK)
	UW	stime;		/* System execution time (ms) */
	UW	utime;		/* User execution time (ms) */
#endif

#if USE_OBJECT_NAME
	UB	name[OBJECT_NAME_LENGTH];	/* name */
#endif

	/*
	 * TessronOS: the process the task belongs to (0: the kernel), the
	 * process whose objects a call of the core made for a process may
	 * name (0: any; see knl_not_owner), and the calls of the task
	 * through the gateway under way: those that are more than a wait
	 * of the core (svcdepth), and of them the calls into the files,
	 * the sockets and the objects (svcin). design 9.7
	 */
	ID	owner;
	ID	svcown;
	volatile INT	svcdepth;
	INT	svcin;

	/*
	 * The time the task has run, in counts of the system counter
	 * (CNTPCT_EL0): from the dispatcher taking it until its processor
	 * gives its context up again (smp.c, knl_acct_leave). Interrupts
	 * taken while it runs are counted in it.
	 */
	UD	runcyc;

	/*
	 * Turns among the tasks of one priority (design 4.3.1): how long
	 * the task may run while another of its priority waits (ms; 0: it
	 * is not made to give way), whether that was set by tk_chg_slt
	 * rather than taken from its band, and when it last began to run
	 * (counts of the system counter, set as a processor takes it).
	 */
	UW	slicetime;
	BOOL	slice_set;
	UD	slice_at;

#if CNF_STK_WATCH
	/*
	 * make STKWATCH=1: the return addresses of the task's frames when
	 * its processor gave its context up, where each one lies and what
	 * it was, and the processor and time of that; the dispatcher reads
	 * them again before the task runs (smp.c, design 4.7.6).
	 */
#define STK_WATCH_N	12
	UD	*swaddr[STK_WATCH_N];
	UD	swval[STK_WATCH_N];
	INT	swn;
	UB	swprc;
	UD	swat;
#endif

/* TCB System Dependent definition */
	TCB_SYSDEPEND_INFO
};

/*
 * Stack guard
 *	The lowest eight bytes of a task's stack hold a value nothing else
 *	writes. A task that runs out of stack writes past them into
 *	whatever the allocator handed out below, and the damage shows up
 *	somewhere else entirely; the dispatcher checks the value before it
 *	lets a task run again, so the task that overran is named.
 */
#define TASK_STK_MAGIC		0x5354414b47554152ULL
#define TaskStackFoot(tcb)	( (UD *)((VB *)(tcb)->isstack - (tcb)->sstksz) )

/*
 * The stack of one of the kernel's tasks has this many unmapped pages
 * under it (knl_vmap_stack), so that running out of it faults there and
 * then. The value at the foot still catches a foot written over from
 * above, by whatever means.
 */
#define TASK_STK_GUARD_PAGES	8
IMPORT BOOL knl_stack_guard_hit( TCB *tcb, UBINT addr );

IMPORT void knl_stack_broken( TCB *tcb );

#if CNF_STK_USAGE
/*
 * The deepest use of each stack (design 4.7.7): the stack is filled with
 * STK_FILL when the task is made, and the words above the foot that
 * still hold it were never reached. What a task used is noted when it is
 * deleted, by the place it starts at and the size of its stack, so that
 * a task that came and went is in the account as well.
 */
#define STK_FILL		0x4c4c49464b545355ULL
IMPORT W knl_stk_used( TCB *tcb );
IMPORT W knl_stk_used_self( void );
IMPORT void knl_stk_usage_say( void );
#endif
#if CNF_STK_WATCH
IMPORT void knl_stk_snap( TCB *tcb );
IMPORT void knl_stack_overwritten( TCB *tcb, UD *at, UD was, UB prc, UD since );
#endif

/*
 * Task dispatch disable state
 *	0 = DDS_ENABLE		 : ENABLE
 *	1 = DDS_DISABLE_IMPLICIT : DISABLE with implicit process
 *	2 = DDS_DISABLE		 : DISABLE with tk_dis_dsp()
 *	|	|
 *	|	use in *.c
 *	use in *.S
 *	  --> Do NOT change these literals, because using in assembler code
 *
 *	'dispatch_disabled' records dispatch disable status set by tk_dis_dsp()
 *	for some CPU, that accepts delayed interrupt.
 *	In this case, you can NOT refer the dispatch disabled status
 *	only by 'dispatch_disabled'.
 *	Use 'in_ddsp()' to refer the task dispatch status.
 *	'in_ddsp()' is a macro definition in CPU-dependent definition files.
 */
#define DDS_ENABLE		(0)
#define DDS_DISABLE_IMPLICIT	(1)	/* set with implicit process */
#define DDS_DISABLE		(2)	/* set by tk_dis_dsp() */
#define knl_dispatch_disabled	(knl_pcpu()->dispatch_disabled)	/* per processor (design 8.4) */

/*
 * Task in execution
 *	ctxtsk is a variable that indicates TCB task in execution
 *	(= the task that CPU holds context). During system call processing,
 *	when checking information about the task that requested system call,
 *	use 'ctxtsk'. Only task dispatcher changes 'ctxtsk'.
 */
#define knl_ctxtsk	(knl_pcpu()->ctxtsk)		/* task in execution on this processor */

/*
 * Task which should be executed
 *	'schedtsk' is a variable that indicates the task TCB to be executed.
 *	If a dispatch is delayed by the delayed dispatch or dispatch disable, 
 *	it does not match with 'ctxtsk.' 
 */
#define knl_schedtsk	(knl_pcpu()->schedtsk)	/* task this processor should execute */

/*
 * TessronOS: objects of processes (design 9.7). A task, semaphore, event
 * flag, mutex or message buffer made by a call of the core on behalf of
 * a process is that process's (its control block's owner). While such a
 * call is served, the running task's svcown names the process, and an
 * object of anyone else is not there for it: the check is made inside
 * the call's critical section, so an ID given back and taken again by
 * someone else between a look and the call cannot be reached. Calls the
 * kernel makes for itself leave svcown 0 and are not checked, nor are
 * those of a handler that interrupts such a call.
 */
#define knl_svc_owner()		( in_indp() ? 0 : knl_ctxtsk->svcown )
#define knl_not_owner(o)	( knl_svc_owner() != 0 && (o) != knl_svc_owner() )

#define TK_OWN_TSK	0
#define TK_OWN_SEM	1
#define TK_OWN_FLG	2
#define TK_OWN_MTX	3
#define TK_OWN_MBF	4
#define TK_OWN_ANY	(-1)		/* knl_own_list: of any process */

IMPORT INT  knl_own_list( INT kind, ID owner, ID *ids, INT max );	/* how many there are */
/*
 * The system stack of a task: a task at protection level 2 or 3 has its
 * own pages from the page allocator, so that as many tasks as memory
 * holds can be made rather than as many as the Imalloc area holds; the
 * kernel's tasks have theirs mapped in the variable area, with unmapped
 * pages below it. knl_sstk_round says how large a stack of that size
 * really is. The stacks given back are unmapped later (knl_sstk_trim),
 * outside the critical section.
 */
IMPORT W knl_sstk_round( W size, ATR tskatr );
IMPORT void *knl_sstk_alloc( W size, ATR tskatr );
IMPORT void knl_sstk_free( void *stack, W size, ATR tskatr );
IMPORT void knl_sstk_trim( void );
IMPORT void knl_own_note( INT kind, ID owner, INT d );	/* one more (d 1) or one fewer (-1) is owner's; in the critical section */
IMPORT void knl_svc_own( ID owner );		/* svcown of the running task */
IMPORT TCB  *knl_tsk_tcb( ID tskid );		/* NULL for no such number */
IMPORT TCB  *knl_tsk_self( void );
IMPORT void knl_tsk_set_owner( ID tskid, ID owner );

/*
 * The processor that takes each interrupt of a device and the roles of
 * the processors (inttarget.c, design 4.5)
 */
IMPORT T_PRCSET	knl_prc_chore;		/* for the system's chores: the devices' interrupts, the timer */
IMPORT T_PRCSET	knl_prc_rt;		/* kept for real time */
IMPORT ER   knl_cpu_roles( CONST T_PRCSET *chore, CONST T_PRCSET *rt );
IMPORT void knl_cpu_roles_get( T_PRCSET *chore, T_PRCSET *rt );
IMPORT ER   knl_cpu_roles_text( CONST char *chore, CONST char *rt );	/* "1,3-4" */
IMPORT void knl_prcset_parse( CONST char *s, INT len, INT nprc, T_PRCSET *set );	/* len bytes of "1,3-4" */
IMPORT INT  knl_prcset_text( UB *t, INT n, INT max, CONST T_PRCSET *s, INT k );	/* the first k as "1,3-4" (obsys.c) */
IMPORT void knl_int_owner( UINT intno, CONST UB *devnm );
IMPORT ER   knl_int_dev_target( CONST UB *devnm, ID prcid );
IMPORT INT  knl_int_dev_info( CONST UB *devnm, ID *p_want, UINT *intno, ID *prc, INT n );
IMPORT INT  knl_int_target_of( UINT intno );
IMPORT INT  knl_int_dev_names( UB (*names)[L_DEVNM + 1], INT n );
/* the architecture's (x64 apic.c, Arm interrupt.c) */
IMPORT BOOL knl_int_is_dev( UINT intno );	/* an interrupt of a device, that can be sent to a processor */
IMPORT BOOL knl_int_is_msi( UINT intno );	/* one by message */
IMPORT BOOL knl_int_in_use( UINT intno );	/* with a handler */
IMPORT ER   knl_int_route( UINT intno, INT prc );	/* sent to PRC prc from now on */

/*
 * Kernel-object initialization (each object)
 */
IMPORT ER knl_task_initialize( void );
IMPORT ER knl_semaphore_initialize( void );
IMPORT ER knl_eventflag_initialize( void );
IMPORT ER knl_mailbox_initialize( void );
IMPORT ER knl_messagebuffer_initialize( void );
IMPORT ER knl_rendezvous_initialize( void );
IMPORT ER knl_mutex_initialize( void );
IMPORT ER knl_memorypool_initialize( void );
IMPORT ER knl_fix_memorypool_initialize( void );
IMPORT ER knl_cyclichandler_initialize( void );
IMPORT ER knl_alarmhandler_initialize( void );
IMPORT ER knl_subsystem_initialize( void );

/*
 * Kernel-object initialization (each object) (tkinit.c)
 */
IMPORT ER knl_init_object(void);

/*
 * Initialization of Device management (device.c)
 */
IMPORT ER knl_initialize_devmgr( void );

/*
 * System timer control (timer.c)
 */
IMPORT ER   knl_timer_startup( void );
IMPORT void knl_timer_shutdown( void );
IMPORT void knl_timer_handler( void );
IMPORT void knl_timer_ipi_handler( UINT intno, UW iar );	/* SGI_TIMER: reprogram on PRC 1 (TessronOS) */

/*
 * Mutex control (mutex.c)
 */
IMPORT void knl_signal_all_mutex( TCB *tcb );
#if KLK_FINE_OBJ
IMPORT void knl_tctl_defer( TCB *tcb );	/* its priority worked out again before the task control lock goes */
#endif
IMPORT INT knl_chg_pri_mutex( TCB *tcb, INT priority );

/*
 * Internal memory allocation (Imalloc) (memory.c)
 */
IMPORT ER knl_init_Imalloc( void );
IMPORT void* knl_Imalloc( SZ size );
IMPORT void* knl_Icalloc( SZ nmemb, SZ size );
IMPORT void* knl_Irealloc( void *ptr, SZ size );
IMPORT void  knl_Ifree( void *ptr );

/*
 * Initial task creation parameter (inittask.c)
 */
IMPORT const T_CTSK knl_init_ctsk;

/*
 * User main program (usermain.c)
 */
IMPORT INT usermain( void );

/*
 * power-saving function (power.c)
 */
IMPORT UINT	knl_lowpow_discnt;


/* ----------------------------------------------------------------------- */
/*
 * Target system-dependent routine (/sysdepend)
 */

/* Low-level memory management information (reset_hdl.c) */
IMPORT	void	*knl_lowmem_top, *knl_lowmem_limit;

/*
 * Startup / Re-start / Shutdown Hardware (hw_setting.c)
 */
IMPORT void knl_startup_hw(void);
IMPORT void knl_shutdown_hw( void );
IMPORT ER knl_restart_hw( W mode );

/*
 * CPU control (cpu_cntl.c)
 */
#if TK_SUPPORT_REGOPS
IMPORT void knl_set_reg( TCB *tcb, CONST T_REGS *regs, CONST T_EIT *eit, CONST T_CREGS *cregs );
IMPORT void knl_get_reg( TCB *tcb, T_REGS *regs, T_EIT *eit, T_CREGS *cregs );
#endif /* TK_SUPPORT_REGOPS */

#if NUM_COPROCESSOR > 0
IMPORT ER knl_get_cpr( TCB *tcb, INT copno, T_COPREGS *copregs);
IMPORT ER knl_set_cpr( TCB *tcb, INT copno, CONST T_COPREGS *copregs);
#endif

/*
 *	Task dispatcher (cpu_cntl.c)
 */
IMPORT void knl_force_dispatch( void );
IMPORT void knl_dispatch( void );

/*
 * Interrupt control (interrupt.c)
 */
IMPORT ER knl_init_interrupt( void );
IMPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr );
IMPORT void knl_return_inthdr(void);

/*
 * Device Driver Startup / Finalization (devinit.c)
 */
IMPORT ER knl_init_device( void );
IMPORT ER knl_start_device( void );
IMPORT ER knl_finish_device( void );

/*
 * micro T-Kernel Startup / Finalization (sysinit.c)
 */
#if !ADD_PREFIX_MAIN_FUNC
IMPORT INT main(void);
#else
IMPORT INT knl_main(void);
#endif	/* ADD_PREFIX_MAIN_FUNC */

IMPORT void knl_tkernel_exit( void );

/*
 * System Call entry
 */
IMPORT void knl_call_entry( void );

/*
 *	Power-Saving Function (power_save.c)
 */
IMPORT void low_pow( void );		/* Switch to power-saving mode */
IMPORT void off_pow( void );		/* Move to suspend mode */

#endif /* _KERNEL_ */
