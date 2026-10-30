/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	proc.c
 *	Process management (design 9.4, 9.5, 9.11).
 *
 *	A process owns an address space, a main task that runs at
 *	protection level 3 in that space, the tasks it makes there, the
 *	objects of the core it makes (semaphores, event flags, mutexes,
 *	message buffers; design 9.7) and the resources the space holds.
 *	When the process ends, its tasks are stopped, its objects deleted,
 *	the space is taken down and every page it mapped is returned; the
 *	control block stays behind holding the exit code until a parent
 *	collects it.
 *
 *	A task of the process inside a call of the files, the sockets, the
 *	objects, drawing or the windows is not stopped there
 *	(knl_prc_call_enter): the process
 *	is marked to end, and the last of its tasks to come out of such a
 *	call finishes it (prc_finish). Whichever way it ends -- by its own
 *	ts_ext_prc, by ts_ter_prc, by a fault -- it ends there.
 *
 *	A process that does not come out of such a call -- its task waits
 *	in a driver for a device that never answers, or for a lock nobody
 *	gives back -- is ended in two more steps when it is ended from the
 *	keyboard a second time (knl_prc_kill, design 5.5.4): the waits that
 *	only wait for something to happen are cut, and what still has not
 *	come out after that is stopped where it stands.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/ob.h>
#include <ts/so.h>
#include <ts/wm.h>
#include <ts/umem.h>
#include <ts/gpu.h>
#include <ts/ser.h>
#include <ts/usb.h>
#include <ts/psx.h>
#include "pfalloc.h"
#include "space.h"
#include "umem.h"
#include "../obj/obj.h"
#include "cpu_ufault.h"

#define PRC_DEFAULT_STKSZ	(64 * 1024)
#define PRC_TSK_STKMAX		(1024 * 1024)	/* the most user stack a task of a process asks for */
#define PRC_ARG_VA		0x0000000000300000ULL	/* start-up argument page */
#define TSK_STOP_TRIES		200		/* ms to wait for a task to leave another processor */
#define PRC_KILL_WAIT		1000		/* ms its tasks are given to come out once their waits are cut */
#define PRC_KILL_STEP		50

typedef struct {
	ID	pid;
	ID	ppid;
	UINT	state;
	INT	exitcd;
	ID	maintsk;
	PRI	basepri;		/* the main task's first: no task of it goes above */
	T_SPACE	space;
	UD	entry;
	UD	brk;
	UD	memsz;			/* bytes mapped, for ts_ref_prc */
	void	*ustk_base;
	UD	ustk_npages;
	ID	flgid;			/* set when the process ends */
	TS_UUID	uuid;			/* the process as a real object (design 18.8) */
	TS_UUID	prog;			/* the program object it was started from, or 0 */
	UB	file[TS_FLT_FILE];	/* the file it was started from, its last part */
	SZ	argsz;			/* bytes of the start-up argument, 0 for none */
	BOOL	sys;			/* the system needs it (TS_PRC_SYSTEM) */

	/* message queue (design 9.9), made on first use */
	T_TSMSG	*msgq;
	INT	msg_head, msg_tail;
	ID	msg_free;		/* room in the queue */
	ID	msg_used;		/* messages waiting */

	/*
	 * Calls of its tasks under way into the file layer, the objects,
	 * the network stack, drawing or the windows (each task's own count is its TCB's svcin);
	 * whether the process is to end, which it does when the last of
	 * them has come out; and whether a task is finishing it
	 * (knl_prc_call_enter, prc_mark, prc_finish).
	 *
	 * incall and ending are read and written with atomic operations,
	 * in sequential consistency, without the table's lock: a call
	 * counts itself in and then looks at ending, and prc_mark sets
	 * ending and then looks at incall, so that of a call coming in and
	 * the process being marked at once, at least one sees the other
	 * (the call goes out again, or the marking leaves the finishing to
	 * the call). Everything else of the end is decided under the lock.
	 */
	INT	incall;
	BOOL	ending;
	BOOL	finishing;
	UD	end_at;			/* ns (monotonic) it was marked to end */

	/*
	 * Its end told (prc_finish), and its tasks and space given back
	 * (prc_release, by prc_reaper or the collector): from then on it is
	 * PS_ZOMBIE, the control block with the exit code waiting for
	 * ts_wai_prc.
	 */
	BOOL	told;
	UD	told_at;		/* ns (monotonic) its end was told */
	BOOL	released;
	UD	zombie_at;		/* ns (monotonic) it became PS_ZOMBIE */
	INT	waiters;		/* ts_wai_prc waiting for it now */

	/*
	 * Ended by force (knl_prc_kill): its tasks were stopped where they
	 * stood. keep_space when one of them was inside a call: a transfer
	 * the call had started may still write into its pages, which are
	 * then not given back.
	 */
	BOOL	killed;
	BOOL	keep_space;

	/*
	 * The kernel met an address of the process that was not there
	 * while it worked for it (knl_prc_kfault), at kfault_va, or
	 * touched a page of it there without opening the way (kfault_pan,
	 * knl_prc_kpan); the process ends as the call returns.
	 */
	volatile BOOL	kfault;
	BOOL	kfault_pan;
	UBINT	kfault_va;
	UD	kfault_esr, kfault_elr, kfault_lr;	/* the exception, and where the kernel was */

	/* the reservations and pages it asked for (design 9.8.1, umem.c) */
	T_UMEM	*umem;

	/* the shared image of its program its space maps (elf.c), NULL for none */
	void	*img;

	/* where a fault of its own goes (ts_def_flt), 0 for none: the process ends */
	UD	flt_entry;

	/*
	 * What a POSIX program has of a process (design 5.8, include/ts/psx.h):
	 * its group and session, the terminal its session has (-1 none), the
	 * signals it catches, ignores and holds back and those come and not
	 * yet handed over, where they are handed over, whether it is stopped
	 * (and whether its parent has heard so), the descriptors it was given
	 * when it was started, and when it is to be sent SIGALRM.
	 */
	ID	pgid, sid;
	INT	ctty;
	volatile UD sig_pend;
	UD	sig_catch, sig_ign, sig_mask;
	UD	sig_entry;
	BOOL	sus_restore;		/* ts_sig_sus: the mask to put back after the handler */
	UD	sus_mask;
	ID	sig_from[TS_NSIG];	/* who sent each pending one, 0 the kernel */
	volatile BOOL stopping;		/* to stop: its tasks stop as they come out of calls */
	BOOL	stopped;
	INT	stopsig;
	BOOL	stop_new, cont_new;	/* not yet told to a waiter */
	INT	*inh;			/* descriptors it was given (ts_spn_prc) */
	INT	ninh;
	UD	alarm_at;		/* ns (monotonic), 0 none */

	BOOL	used;
	UW	gen;			/* which process under this number: a waiter looks again by it */
	QUEUE	link;			/* in prc_all */
} T_PCB;

/*
 * The processes there are. A control block is made when its process is
 * and freed when the process is collected; it is found by its number,
 * by its ASID (the space the running task is on), or by walking them
 * all. The two tables are written under prc_mtxid; the one by ASID is
 * also read without it, by the exception handler, which finds there
 * only a block that stays while a task of its process runs.
 */
LOCAL T_PCB	*prc_by_pid[CNF_PID_MAX];
LOCAL T_PCB	*prc_by_asid[ASID_MAX + 1];
LOCAL QUEUE	prc_all;
LOCAL INT	prc_count = 0;
LOCAL UW	prc_gen = 0;
LOCAL ID	prc_mtxid = 0;
LOCAL ID	prc_next_pid = 1;
LOCAL ID	prc_init_pid = 0;	/* the init process, 0 while there is none */
LOCAL ID	prc_reaper_tid = 0;	/* gives back what an ended process held (prc_reaper) */

/*
 * The process table's lock taken. In the UI tests' builds the takes are
 * counted, and those that had to wait for it (knl_prc_lock_stat).
 */
#ifdef USE_UITEST
LOCAL volatile UD	prc_lock_takes = 0, prc_lock_waits = 0;

EXPORT void knl_prc_lock_stat( UD *p_takes, UD *p_waits )
{
	*p_takes = __atomic_load_n(&prc_lock_takes, __ATOMIC_RELAXED);
	*p_waits = __atomic_load_n(&prc_lock_waits, __ATOMIC_RELAXED);
}

LOCAL void prc_lock( void )
{
	__atomic_fetch_add(&prc_lock_takes, 1, __ATOMIC_RELAXED);
	if ( tk_loc_mtx(prc_mtxid, TMO_POL) < E_OK ) {
		__atomic_fetch_add(&prc_lock_waits, 1, __ATOMIC_RELAXED);
		tk_loc_mtx(prc_mtxid, TMO_FEVR);
	}
}
#else
#define prc_lock()	tk_loc_mtx(prc_mtxid, TMO_FEVR)
#endif

#define PCB_OF(q)	( (T_PCB *)( (UB *)(q) - offsetof(T_PCB, link) ) )

#define INCALL(p)	__atomic_load_n(&(p)->incall, __ATOMIC_SEQ_CST)
#define ENDING(p)	__atomic_load_n(&(p)->ending, __ATOMIC_SEQ_CST)
#define FOR_EACH_PCB(p, q) \
	for ( (q) = prc_all.next; (q) != &prc_all && ( ( (p) = PCB_OF(q) ) != NULL ); (q) = (q)->next )

#define PRC_FLG_EXIT	0x0001
#define PRC_FLG_CHILD	0x0002		/* a child ended, stopped or went on */
#define PRC_FLG_SIG	0x0004		/* a signal came to be handed over */

/* ---------------------------------------------------------------- helpers */

LOCAL T_PCB *pid_to_pcb( ID pid )
{
	return ( pid > 0 && pid < CNF_PID_MAX ) ? prc_by_pid[pid] : NULL;
}

/* The process whose space ttbr0 is */
LOCAL T_PCB *ttbr0_to_pcb( UD ttbr0 )
{
	T_PCB	*p = prc_by_asid[knl_mmu_root_asid(ttbr0)];

	return ( p != NULL && p->used && p->space.ttbr0 == ttbr0 ) ? p : NULL;
}

/*
 * The process the calling task belongs to, found through the space its
 * TTBR0 names. Kernel tasks and the system process give NULL. The task
 * is read once, as the processor's own (knl_self_ctxtsk): with
 * interrupts enabled, two reads could each be of another processor's.
 */
LOCAL T_PCB *cur_pcb( void )
{
	TCB	*me = knl_self_ctxtsk();

	if ( me == NULL ) {
		return NULL;
	}
	return ttbr0_to_pcb(me->tskctxb.ttbr0);
}

#define PRC_REAPER_PRI		12	/* above the programs, so that what ended goes back soon */
#define PRC_REAP_WAIT		2000	/* ms its last task is given to become dormant */
#define PRC_REAP_BATCH		32	/* ended processes looked at in one round */
#define PRC_REAP_SOON		5	/* ms to the next round while some has not ended itself yet */
#define PRC_ORPHAN_KEEP_MS	60000	/* an ended process nobody can collect is kept this long */
#define PRC_ORPHAN_MAX		512	/* and no more of them than this, the oldest going first */
#define PRC_ORPHAN_MIN_MS	2000	/* but none sooner than this, however many there are */
#define PRC_REAP_TICK		1000	/* ms between looks while any is kept */

/*
 * How long an ended process that nobody can collect is kept for one who
 * still asks for its exit code (ts_wai_prc, ts_ref_prc); the tests make
 * it shorter.
 */
EXPORT UW	knl_prc_orphan_ms = PRC_ORPHAN_KEEP_MS;

LOCAL void prc_reaper( INT stacd, void *exinf );
LOCAL void prc_free( T_PCB *p );

EXPORT ER knl_proc_init( void )
{
	T_CMTX	cmtx;

	if ( prc_mtxid > 0 ) {
		return E_OK;
	}
	QueInit(&prc_all);

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	prc_mtxid = tk_cre_mtx(&cmtx);
	if ( prc_mtxid <= 0 ) {
		return (ER)prc_mtxid;
	}

	{
		T_CTSK	ctsk;

		ctsk.exinf   = NULL;
		ctsk.tskatr  = TA_HLNG | TA_RNG0;
		ctsk.task    = (FP)prc_reaper;
		ctsk.itskpri = PRC_REAPER_PRI;
		ctsk.stksz   = 8 * 1024;
		prc_reaper_tid = tk_cre_tsk(&ctsk);
		if ( prc_reaper_tid <= 0 || tk_sta_tsk(prc_reaper_tid, 0) < E_OK ) {
			tm_printf((UB*)"TessronOS: no task to give back what ended processes held (%d)\n",
				  prc_reaper_tid);
			prc_reaper_tid = 0;	/* their collectors do it, as before */
		}
	}
	if ( knl_psx_init() < E_OK ) {
		tm_printf((UB*)"TessronOS: no task to send SIGALRM\n");
	}

	return E_OK;
}

/*
 * The objects of the core a process made, deleted: as the process,
 * so that the core deletes only what is still its own, whatever became
 * of an ID since it was listed.
 */
#define OWN_BATCH	16

LOCAL void tk_objs_end( ID pid )
{
	ID	ids[OWN_BATCH];
	INT	kind, n, i, round;

	for ( kind = TK_OWN_SEM; kind <= TK_OWN_MBF; kind++ ) {
		for ( round = 0; round < TS_PRC_SEM_MAX / OWN_BATCH + 8; round++ ) {
			n = knl_own_list(kind, pid, ids, OWN_BATCH);
			if ( n <= 0 ) {
				break;
			}
			knl_svc_own(pid);
			for ( i = 0; i < n && i < OWN_BATCH; i++ ) {
				switch ( kind ) {
				  case TK_OWN_SEM:	(void)tk_del_sem(ids[i]);	break;
				  case TK_OWN_FLG:	(void)tk_del_flg(ids[i]);	break;
				  case TK_OWN_MTX:	(void)tk_del_mtx(ids[i]);	break;
				  default:		(void)tk_del_mbf(ids[i]);	break;
				}
			}
			knl_svc_own(0);
		}
	}
}

/*
 * What a process has open in the layers above -- its keys to objects,
 * the windows it made, its files, its sockets and its BOs of the GPU --
 * and the objects of the core it made.
 * Closed as it ends, on whichever path it ends by; closing twice is
 * harmless.
 */
LOCAL void prc_close_all( ID pid )
{
	if ( pid <= 0 ) {
		return;
	}
	knl_ob_prc_end(pid);
	knl_wmobj_prc_end(pid);
	knl_fs_prc_end(pid);
	knl_so_prc_end(pid);
	knl_gpu_prc_end(pid);
	knl_sr_prc_end(pid);
	knl_us_prc_end(pid);
	tk_objs_end(pid);
}

/*
 * A task of a process made dormant, wherever it stands: waiting, ready,
 * or running on another processor, which it is taken off first
 * (tk_ter_tsk does not reach a task there). Not the calling task.
 */
LOCAL void tsk_stop( ID tid )
{
	T_RTSK	r;
	INT	n;

	for ( n = 0; n < TSK_STOP_TRIES; n++ ) {
		if ( tk_ref_tsk(tid, &r) < E_OK || r.tskstat == TTS_DMT ) {
			return;
		}
		if ( tk_ter_tsk(tid) >= E_OK ) {
			return;
		}
		(void)tk_sus_tsk(tid);
		tk_dly_tsk(1);
	}
}

/*
 * Release everything a process holds except the control block itself:
 * its tasks, then the space. A task that could not be deleted keeps
 * the space: its stack is in it, and it would be given back into
 * tables that were gone.
 */
LOCAL void prc_release( T_PCB *p )
{
	ID	tids[TS_PRC_TSK_MAX];
	INT	n, i, left = 0;

	if ( p->released ) {
		return;
	}
	p->released = TRUE;
	n = knl_own_list(TK_OWN_TSK, p->pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		tsk_stop(tids[i]);
		if ( tk_del_tsk(tids[i]) < E_OK ) {
			left++;
		}
	}
	p->maintsk = 0;
	/* The stacks and the program pages all live in the space, so taking
	   the space down returns them together. */
	if ( left == 0 && !p->keep_space ) {
		knl_space_delete(&p->space);
		knl_umem_free(&p->umem);
		knl_elf_img_put(p->img);	/* after the space: the pages it shares are mapped no more */
		p->img = NULL;
	} else if ( left == 0 ) {
		tm_printf((UB*)"TessronOS: pid %d was stopped inside a call: its pages are kept\n",
			  p->pid);
	} else {
		tm_printf((UB*)"TessronOS: pid %d left %d task(s) behind: its space is kept\n",
			  p->pid, left);
		knl_umem_free(&p->umem);
	}
	p->ustk_base = NULL;
	p->memsz = 0;
}

/* Whether every task the process pid still has is dormant */
LOCAL BOOL prc_quiet( ID pid )
{
	ID	tids[TS_PRC_TSK_MAX];
	T_RTSK	r;
	INT	n, i;

	n = knl_own_list(TK_OWN_TSK, pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		if ( tk_ref_tsk(tids[i], &r) >= E_OK && r.tskstat != TTS_DMT ) {
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * What a process held once its end has been told -- its tasks and its
 * space -- given back without waiting for its collector, which may be
 * long in coming, or never come for one the kernel started. The task
 * that finished it goes on only to end itself (tk_ext_tsk); it is given
 * a moment to do so, so that it is deleted dormant rather than stopped.
 * The process is PS_ZOMBIE from then on, and ts_wai_prc frees its
 * control block and hands over the exit code as before (design 5.5).
 */
LOCAL BOOL prc_reap_ended( void )
{
	T_PCB	*p;
	QUEUE	*q;
	ID	pids[PRC_REAP_BATCH];
	UW	gens[PRC_REAP_BATCH];
	BOOL	force[PRC_REAP_BATCH], more = FALSE;
	UD	now = 0;
	INT	n = 0, i;

	(void)ts_get_mono(&now);
	prc_lock();
	FOR_EACH_PCB(p, q) {
		if ( p->used && p->told && !p->released ) {
			if ( n == PRC_REAP_BATCH ) {
				more = TRUE;
				break;
			}
			pids[n] = p->pid;
			gens[n] = p->gen;
			force[n] = (BOOL)( now > p->told_at && now - p->told_at >= (UD)PRC_REAP_WAIT * 1000000ULL );
			n++;
		}
	}
	tk_unl_mtx(prc_mtxid);

	/* one whose last task has not ended itself yet is looked at again; none is waited for here */
	for ( i = 0; i < n; i++ ) {
		if ( !force[i] && !prc_quiet(pids[i]) ) {
			more = TRUE;
			continue;
		}
		prc_lock();
		p = pid_to_pcb(pids[i]);
		if ( p != NULL && p->gen == gens[i] && p->told && !p->released ) {
			prc_release(p);
			p->state = PS_ZOMBIE;
			p->zombie_at = now;
		}
		tk_unl_mtx(prc_mtxid);
	}
	return more;
}

/*
 * Whether anyone may still collect p: a process parent that is running
 * and is not init, which collects nothing. The kernel waits for those
 * it started only while it is in ts_wai_prc for them (waiters).
 */
LOCAL BOOL prc_has_collector( T_PCB *p )
{
	T_PCB	*pp;

	if ( p->waiters > 0 ) {
		return TRUE;
	}
	if ( p->ppid <= 0 || p->ppid == prc_init_pid ) {
		return FALSE;
	}
	pp = pid_to_pcb(p->ppid);
	return (BOOL)( pp != NULL && pp->used && pp->state == PS_RUNNING );
}

/*
 * The control blocks of ended processes that nobody can collect, freed:
 * each once it has been PS_ZOMBIE for knl_prc_orphan_ms, and the oldest
 * of them sooner when there are more than PRC_ORPHAN_MAX (but none
 * before PRC_ORPHAN_MIN_MS). Its children are init's from then, as when
 * it is collected. TRUE while any such block is still kept.
 */
LOCAL BOOL prc_free_orphans( void )
{
	T_PCB	*p, *c, *old;
	QUEUE	*q, *q2;
	UD	now, age, keep;
	INT	n;
	ID	gone;
	BOOL	more;

	for ( ;; ) {
		now = 0;
		(void)ts_get_mono(&now);
		keep = (UD)knl_prc_orphan_ms * 1000000ULL;
		n = 0;
		old = NULL;
		gone = 0;
		prc_lock();
		FOR_EACH_PCB(p, q) {
			if ( !p->used || p->state != PS_ZOMBIE || !p->released || prc_has_collector(p) ) {
				continue;
			}
			n++;
			if ( old == NULL || p->zombie_at < old->zombie_at ) {
				old = p;
			}
		}
		if ( old != NULL ) {
			age = ( now > old->zombie_at ) ? now - old->zombie_at : 0;
			if ( age >= keep
			  || ( n > PRC_ORPHAN_MAX && age >= (UD)PRC_ORPHAN_MIN_MS * 1000000ULL ) ) {
				FOR_EACH_PCB(c, q2) {
					if ( c->ppid == old->pid ) {
						c->ppid = prc_init_pid;
					}
				}
				gone = old->pid;
				prc_free(old);
				n--;
			}
		}
		more = (BOOL)( n > 0 );
		tk_unl_mtx(prc_mtxid);
		if ( gone == 0 ) {
			return more;
		}
		prc_close_all(gone);
		knl_ob_prc_gone(gone);
	}
}

/* Woken by prc_finish; looks again now and then while ended processes are kept */
LOCAL void prc_reaper( INT stacd, void *exinf )
{
	BOOL	kept = FALSE, ending = FALSE;

	for ( ;; ) {
		(void)tk_slp_tsk(( ending ) ? PRC_REAP_SOON : ( kept ) ? PRC_REAP_TICK : TMO_FEVR);
		ending = prc_reap_ended();
		kept = prc_free_orphans();
		if ( knl_pt_reclaim() ) {	/* page tables taken out of the spaces, once it is safe */
			kept = TRUE;
		}
	}
}

/*
 * The queue of a process, made when it is first used. Returns E_OK once
 * the process has one.
 */
LOCAL ER msgq_ready( T_PCB *p )
{
	T_CSEM	csem;

	if ( p->msgq != NULL ) {
		return E_OK;
	}
	p->msgq = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG) * TS_MSG_QLEN);
	if ( p->msgq == NULL ) {
		return E_NOMEM;
	}
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = TS_MSG_QLEN;
	csem.maxsem = TS_MSG_QLEN;
	p->msg_free = tk_cre_sem(&csem);
	csem.isemcnt = 0;
	p->msg_used = tk_cre_sem(&csem);
	if ( p->msg_free <= 0 || p->msg_used <= 0 ) {
		if ( p->msg_free > 0 ) tk_del_sem(p->msg_free);
		if ( p->msg_used > 0 ) tk_del_sem(p->msg_used);
		Kfree(p->msgq);
		p->msgq = NULL;
		return E_LIMIT;
	}
	p->msg_head = p->msg_tail = 0;

	return E_OK;
}

LOCAL void msgq_free( T_PCB *p )
{
	if ( p->msgq == NULL ) {
		return;
	}
	tk_del_sem(p->msg_free);
	tk_del_sem(p->msg_used);
	Kfree(p->msgq);
	p->msgq = NULL;
	p->msg_free = p->msg_used = 0;
}

/* The control block let go, with what it still holds. Called with the lock held. */
LOCAL void prc_free( T_PCB *p )
{
	msgq_free(p);
	knl_umem_free(&p->umem);
	if ( p->inh != NULL ) {
		Kfree(p->inh);
		p->inh = NULL;
	}
	if ( p->flgid > 0 ) {
		tk_del_flg(p->flgid);
		p->flgid = 0;
	}
	p->used = FALSE;
	p->state = PS_NONEXIST;
	if ( prc_by_pid[p->pid] == p ) {
		prc_by_pid[p->pid] = NULL;
	}
	if ( p->space.asid != 0 && prc_by_asid[p->space.asid] == p ) {
		prc_by_asid[p->space.asid] = NULL;
	}
	QueRemove(&p->link);
	prc_count--;
	Kfree(p);
}

/* A number no process has, the next in turn; 0 when there is none */
LOCAL ID pid_new( void )
{
	INT	n;
	ID	pid = prc_next_pid;

	for ( n = 1; n < CNF_PID_MAX; n++, pid++ ) {
		if ( pid >= CNF_PID_MAX ) {
			pid = 1;
		}
		if ( prc_by_pid[pid] == NULL ) {
			prc_next_pid = pid + 1;
			return pid;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- creation */

/* Where a program's image comes from: a file, or a record of an object */
typedef struct {
	CONST char	*path;
	ID		vol;
	CONST TS_UUID	*uuid;
	INT		recno;
} PRCIMG;

LOCAL ID cre_prc( CONST PRCIMG *img, CONST T_CPRC *pk_cprc, CONST T_SPAWN *sp );

EXPORT ID ts_cre_prc( CONST char *path, CONST T_CPRC *pk_cprc )
{
	PRCIMG	img;

	if ( path == NULL ) {
		return E_PAR;
	}
	img.path = path;
	img.vol = 0;
	img.uuid = NULL;
	img.recno = 0;
	return cre_prc(&img, pk_cprc, NULL);
}

/* The pages of the start-up argument: as many as it takes, at most TS_SPN_ARG_MAX */
#define ARG_PAGES(n)	( ( (UD)(n) + PAGE_SIZE - 1 ) / PAGE_SIZE )

/*
 * The program read into a new space, with its start-up argument, before
 * anything else knows of the space. This is where a process takes its
 * time: a program of a hundred megabytes is seconds of reading. It is
 * done without prc_mtxid, which every call of a process into the files,
 * the objects, drawing and the windows takes on its way in and out
 * (knl_prc_call_enter), and which ts_get_pid takes for every check of
 * who is asking: held here, it would stop every program and the desktop
 * until the reading is over. The space is the caller's alone until
 * cre_prc hands it to the control block.
 */
LOCAL ER load_image( CONST PRCIMG *img, CONST T_CPRC *pk_cprc, T_SPACE *sp, UD *p_entry, UD *p_brk,
		       void **p_shared )
{
	ER	er;

	*p_shared = NULL;
	er = knl_space_create(sp);
	if ( er < E_OK ) {
		return er;
	}
	er = ( img->path != NULL ) ? knl_elf_load(img->path, sp, p_entry, p_brk)
				   : knl_elf_load_rec(img->vol, img->uuid, img->recno, sp,
						      p_entry, p_brk, p_shared);
	if ( er < E_OK ) goto err;

	/* the start-up argument, as read only pages in the space */
	if ( pk_cprc->arg != NULL && pk_cprc->argsz > 0 ) {
		UD	np = ARG_PAGES(pk_cprc->argsz), i;
		void	*page;

		er = knl_space_map_zero(sp, PRC_ARG_VA, np * PAGE_SIZE, PTE_PAGE_UDATA);
		if ( er < E_OK ) goto err;
		for ( i = 0; i < np; i++ ) {
			UD	k = (UD)pk_cprc->argsz - i * PAGE_SIZE;

			page = knl_space_page(sp, PRC_ARG_VA + i * PAGE_SIZE);
			if ( page == NULL ) { er = E_SYS; goto err; }
			knl_memcpy(page, (CONST UB *)pk_cprc->arg + i * PAGE_SIZE,
				   (INT)( ( k > PAGE_SIZE ) ? PAGE_SIZE : k ));
		}
		er = knl_space_protect(sp, PRC_ARG_VA, np * PAGE_SIZE, PTE_PAGE_URO);
		if ( er < E_OK ) goto err;
	}
	return E_OK;

    err:
	knl_space_delete(sp);
	knl_elf_img_put(*p_shared);	/* after the space, as when a process ends */
	*p_shared = NULL;
	return er;
}

LOCAL ER psx_child( T_PCB *p, T_PCB *parent, CONST T_SPAWN *sp );

LOCAL ID cre_prc( CONST PRCIMG *img, CONST T_CPRC *pk_cprc, CONST T_SPAWN *sp )
{
	CONST char *path = img->path;
	T_PCB	*p = NULL;
	T_CTSK	ctsk;
	T_CFLG	cflg;
	T_PCB	*parent;
	T_SPACE	space;
	void	*shared = NULL;
	UD	stksz, entry = 0, brk = 0;
	PRI	pri;
	ER	er;
	ID	tid, npid = 0, nppid;
	BOOL	room;

	if ( pk_cprc == NULL ) {
		return E_PAR;
	}
	if ( prc_mtxid <= 0 ) {
		return E_OBJ;
	}
	if ( pk_cprc->arg != NULL
	  && (UD)pk_cprc->argsz > ( ( sp != NULL ) ? (UD)TS_SPN_ARG_MAX : (UD)PAGE_SIZE ) ) {
		return E_PAR;
	}

	/* no process could be made: not worth reading the program for */
	prc_lock();
	room = (BOOL)( prc_count < CNF_MAX_PRC );
	tk_unl_mtx(prc_mtxid);
	if ( !room ) {
		return E_LIMIT;
	}

	knl_memset(&space, 0, sizeof(space));
	er = load_image(img, pk_cprc, &space, &entry, &brk, &shared);
	if ( er < E_OK ) {
		return er;
	}

	prc_lock();

	if ( prc_count >= CNF_MAX_PRC || ( npid = pid_new() ) <= 0 ) {
		er = E_LIMIT;
		goto err0;
	}
	p = (T_PCB *)Kmalloc(sizeof(T_PCB));
	if ( p == NULL ) {
		er = E_NOMEM;
		goto err0;
	}
	parent = cur_pcb();

	/*
	 * A process starts another no higher than itself, and in the
	 * real-time band (1..CNF_RT_PRI_LAST) only when it is the
	 * administrator's or the system's: a program in that band holds
	 * back everything below it, the desktop and the programs, and the
	 * time-shared band gives them turns (design 4.3.1). The kernel's
	 * callers choose freely.
	 */
	if ( parent != NULL && pk_cprc->pri > 0 ) {
		BOOL	rt_ok = (BOOL)( parent->sys || knl_ob_is_admin(knl_ob_crd_of(parent->pid)) );

		if ( pk_cprc->pri < parent->basepri
		  || ( !rt_ok && pk_cprc->pri <= CNF_RT_PRI_LAST ) ) {
			Kfree(p);
			er = E_OACV;
			goto err0;
		}
	}

	knl_memset(p, 0, sizeof(T_PCB));
	p->used    = TRUE;
	p->state   = PS_RUNNING;
	p->pid     = npid;
	p->gen     = ++prc_gen;
	p->ppid    = ( parent != NULL ) ? parent->pid : 0;
	prc_by_pid[npid] = p;
	QueInsert(&p->link, &prc_all);
	prc_count++;
	p->maintsk = 0;
	if ( ts_gen_uuid(&p->uuid) < E_OK ) {
		knl_memset(&p->uuid, 0, sizeof(p->uuid));	/* no clock yet: not an object */
	}

	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	p->flgid = tk_cre_flg(&cflg);
	if ( p->flgid <= 0 ) { er = (ER)p->flgid; goto err1; }

	/* the space is the process's from here, and goes with it, and so is the image it shares */
	p->space = space;
	space.l0_pa = 0;
	p->img = shared;
	shared = NULL;
	prc_by_asid[p->space.asid] = p;
	p->entry = entry;
	p->brk = brk;

	if ( path != NULL ) {
		CONST char	*b = path;
		INT		i;

		for ( i = 0; path[i] != '\0'; i++ ) {
			if ( path[i] == '/' ) b = &path[i + 1];
		}
		for ( i = 0; b[i] != '\0' && i < TS_FLT_FILE - 1; i++ ) {
			p->file[i] = (UB)b[i];
		}
		p->file[i] = 0;
	}
	if ( img->uuid != NULL ) {
		p->prog = *img->uuid;		/* known before its first instruction */
	}
	/* only the kernel says a process is the system's: a program cannot say it of its child */
	p->sys = (BOOL)( ( pk_cprc->prcatr & TS_PRC_SYSTEM ) != 0 && parent == NULL );
	if ( pk_cprc->arg != NULL && pk_cprc->argsz > 0 ) {
		p->argsz = pk_cprc->argsz;
	}

	/*
	 * The main task. knl_new_task_space makes tk_cre_tsk put the user
	 * stack of this level 3 task into the process's own space; it is
	 * held only across the call, under this mutex.
	 */
	stksz = ( pk_cprc->stksz > 0 ) ? (UD)pk_cprc->stksz : PRC_DEFAULT_STKSZ;
	/*
	 * CNF_PRC_PRI when the caller does not choose: in the time-shared
	 * band, beside the desktop, so that it takes turns with it (design
	 * 4.3.1); no higher than its parent
	 */
	pri   = ( pk_cprc->pri > 0 ) ? pk_cprc->pri : (PRI)CNF_PRC_PRI;
	if ( parent != NULL && pri < parent->basepri ) pri = parent->basepri;
	if ( pri > CNF_MAX_TSKPRI ) pri = CNF_MAX_TSKPRI;

	/* a program may compute in floating point: its registers go with it */
	ctsk.exinf   = p;
	ctsk.tskatr  = TA_HLNG | TA_RNG3 | TA_FPU;
	ctsk.task    = (FP)(UBINT)p->entry;
	ctsk.itskpri = pri;
	ctsk.stksz   = 0;			/* system stack (EL1): the least a process task has (CNF_SVC_STKSZ) */
	ctsk.assprc  = 0;

	knl_new_task_space = &p->space;
	knl_set_ustack_size(stksz);
	tid = tk_cre_tsk(&ctsk);
	knl_set_ustack_size(0);
	knl_new_task_space = NULL;

	if ( tid <= 0 ) { er = (ER)tid; goto err2; }
	p->maintsk = tid;
	p->basepri = pri;
	knl_tsk_set_owner(tid, p->pid);

	/* the task runs in the process space, not in the system process */
	er = knl_tsk_set_space(tid, p->space.ttbr0);
	if ( er < E_OK ) goto err3;

	p->memsz = p->brk - USER_TEXT_VA + stksz;

	/*
	 * It acts as its parent did (design 18.10) -- one the kernel
	 * starts as the system, or for the person at the machine when it
	 * says TS_PRC_USER -- from its first instruction: the credentials are there before the task runs, or
	 * its first calls are judged as nobody's. The object layer's lock
	 * is taken under this one; the object layer never takes this one
	 * holding its own, so the order is always this way round.
	 */
	npid = p->pid;
	nppid = p->ppid;
	knl_ob_prc_start(npid, nppid, (BOOL)( parent == NULL && !p->sys
					     && ( pk_cprc->prcatr & TS_PRC_USER ) != 0 ));

	/* its group, session and signals, and the descriptors it is given, before it runs */
	er = psx_child(p, parent, sp);
	if ( er < E_OK ) {
		knl_ob_prc_gone(npid);
		goto err3;
	}

	er = tk_sta_tsk(tid, 0);
	if ( er < E_OK ) {
		knl_ob_prc_gone(npid);
		goto err3;
	}
	tk_unl_mtx(prc_mtxid);

	return npid;

    err3:
	tk_del_tsk(p->maintsk);
	p->maintsk = 0;
    err2:
	prc_by_asid[p->space.asid] = NULL;
	knl_space_delete(&p->space);
	p->space.asid = 0;
	knl_elf_img_put(p->img);
	p->img = NULL;
    err1:
	npid = ( p->inh != NULL ) ? p->pid : 0;
	prc_free(p);
    err0:
	tk_unl_mtx(prc_mtxid);
	knl_space_delete(&space);	/* a space not handed to a process */
	knl_elf_img_put(shared);	/* and the image it shared */
	if ( sp != NULL && npid > 0 ) {
		knl_fs_prc_end(npid);		/* what it was given, never to be used */
	}

	return er;
}

/*
 * Start a program held in a real object (design 9.5, 9.6). The metadata
 * says which record holds the image. On the FAT store that record is a
 * file, and the loader is given its path; on the native store the loader
 * reads the record itself.
 */
LOCAL ID cre_prc_obj( ID vol, CONST TS_UUID *uuid, CONST T_CPRC *pk_cprc, CONST T_SPAWN *sp );

EXPORT ID ts_cre_prc_obj( ID vol, CONST TS_UUID *uuid, CONST T_CPRC *pk_cprc )
{
	return cre_prc_obj(vol, uuid, pk_cprc, NULL);
}

LOCAL ID cre_prc_obj( ID vol, CONST TS_UUID *uuid, CONST T_CPRC *pk_cprc, CONST T_SPAWN *sp )
{
	PRCIMG	img;
	UB	*path;
	INT	recno;
	ID	pid;
	ER	er;

	if ( uuid == NULL || pk_cprc == NULL ) {
		return E_PAR;
	}
	recno = knl_tsfs_exec_rec(vol, uuid);
	if ( recno < 0 ) {
		return (ER)recno;		/* not a program */
	}
	path = (UB *)Kmalloc(FS_PATH_MAX);
	if ( path == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_rec_path(vol, uuid, recno, path, FS_PATH_MAX);
	img.path = ( er >= E_OK ) ? (CONST char *)path : NULL;
	img.vol = vol;
	img.uuid = uuid;
	img.recno = recno;
	if ( er < E_OK && er != E_NOSPT ) {
		Kfree(path);
		return er;
	}
	pid = cre_prc(&img, pk_cprc, sp);
	Kfree(path);

	return pid;
}

/* ---------------------------------------------------------------- exit */

LOCAL ER snd_msg_as( ID pid, CONST T_TSMSG *msg, TMO tmout, ID from );

/*
 * p is to end with code; the code first given stays. TRUE when the
 * caller is to finish it now: none of its tasks is inside a bracketed
 * call (knl_prc_call_enter), and no one else is finishing it.
 * Otherwise the last of its tasks to come out of such a call does
 * (knl_prc_call_leave). From here on none of its tasks is let into one
 * (E_DISWAI), and the long calls it is in stop at their next piece
 * (knl_prc_ending). Called with the lock held.
 */
LOCAL BOOL prc_mark( T_PCB *p, INT code )
{
	if ( !p->ending ) {
		p->exitcd = code;
		p->state  = PS_EXITING;
		p->end_at = 0;
		(void)ts_get_mono(&p->end_at);
		__atomic_store_n(&p->ending, TRUE, __ATOMIC_SEQ_CST);
	}
	if ( INCALL(p) > 0 || p->finishing ) {
		return FALSE;
	}
	p->finishing = TRUE;
	return TRUE;
}

/*
 * The end of a process, on the task that finishes it (prc_mark): its
 * other tasks stopped and deleted, what it had open closed, the objects
 * of the core it made deleted, and only then its end told -- to whoever
 * waits for it and to its parent -- so that whoever collects it finds
 * all that done. A task of the process that finishes it goes on only to
 * end itself; the collector (ts_wai_prc) deletes it with the space.
 * Called without the lock.
 */
LOCAL void prc_finish( T_PCB *p )
{
	ID	tids[TS_PRC_TSK_MAX];
	ID	self = tk_get_tid(), pid, ppid;
	INT	n, i, code;

	prc_lock();
	pid = p->pid;
	ppid = p->ppid;
	code = p->exitcd;
	n = knl_own_list(TK_OWN_TSK, pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		if ( tids[i] == self ) {
			continue;
		}
		tsk_stop(tids[i]);
		if ( tk_del_tsk(tids[i]) >= E_OK && tids[i] == p->maintsk ) {
			p->maintsk = 0;
		}
	}
	tk_unl_mtx(prc_mtxid);

	prc_close_all(pid);

	/* the parent told, when it is a process that is listening */
	if ( ppid > 0 ) {
		T_TSMSG	m;

		m.type = TS_MSG_CHILD_EXIT;
		m.from = pid;
		m.size = sizeof(INT);
		knl_memcpy(m.body, &code, sizeof(INT));
		(void)snd_msg_as(ppid, &m, TMO_POL, pid);
	}

	prc_lock();
	tk_set_flg(p->flgid, PRC_FLG_EXIT);
	p->told = TRUE;
	p->told_at = 0;
	(void)ts_get_mono(&p->told_at);
	{
		T_PCB	*pp = pid_to_pcb(ppid);

		if ( pp != NULL && pp->used && pp->flgid > 0 ) {
			tk_set_flg(pp->flgid, PRC_FLG_CHILD);	/* ts_wai_psx looks again */
		}
	}
	tk_unl_mtx(prc_mtxid);
	if ( ppid > 0 ) {
		(void)knl_psx_kill(ppid, TS_SIGCHLD);
	}
	if ( prc_reaper_tid > 0 ) {
		(void)tk_wup_tsk(prc_reaper_tid);	/* what it held given back without waiting for its collector */
	}
}

/*
 * End the calling process, from any of its tasks. The task cannot take
 * its own space down while it is running on it, so the space is
 * released by the collector (ts_wai_prc) once the task has left the
 * processor.
 */
EXPORT void ts_ext_prc( INT exitcd )
{
	T_PCB	*p;
	BOOL	fin = FALSE;

	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		fin = prc_mark(p, exitcd);
	}
	tk_unl_mtx(prc_mtxid);

	if ( fin ) {
		prc_finish(p);
	}
	tk_ext_tsk();			/* no return */
}

#ifdef USE_KTEST
EXPORT UW	knl_prc_ter_inside = 0;		/* ts_ter_prc that found the process inside a call */
#endif

/*
 * End a process. Its tasks are stopped where they stand, unless one is
 * in a call of the file layer, the objects, the network stack, drawing
 * or the windows: taken down there, it would leave a volume, a store,
 * the stack, a drawing environment or the font engine half changed, a
 * lock of theirs taken, or memory the call took never given back. The process is then only marked; its tasks look between the
 * pieces of a long call and the slices of a wait whether it is to end,
 * come out, and the last one out finishes it (knl_prc_call_leave).
 * Called by a task of the process itself, it does not come back.
 */
EXPORT ER ts_ter_prc( ID pid, INT abortcd )
{
	T_PCB	*p;
	BOOL	fin = FALSE, mine = FALSE;
	ER	er = E_OK;

	prc_lock();

	p = pid_to_pcb(pid);
	if ( p == NULL ) {
		er = E_NOEXS;
	} else if ( p->ending ) {
		er = E_OBJ;
	} else {
		fin = prc_mark(p, abortcd);
		mine = ( cur_pcb() == p );
#ifdef USE_KTEST
		if ( !fin ) {
			knl_prc_ter_inside++;
		}
#endif
	}

	tk_unl_mtx(prc_mtxid);

	if ( fin ) {
		prc_finish(p);
	}
	if ( mine ) {
		tk_ext_tsk();		/* no return */
	}
	return er;
}

/*
 * Processes with a fault met in the kernel on their behalf that has not
 * ended them yet: while there is one, every call from a process goes
 * out through knl_prc_call_leave. Changed under kfault_lock.
 */
EXPORT volatile INT	knl_prc_kfaults = 0;
LOCAL T_SPLOCK		kfault_lock;

LOCAL void kfault_count( INT d )
{
	UINT	imask;

	ISpinLock(&kfault_lock, &imask);
	knl_prc_kfaults += d;
	ISpinUnlock(&kfault_lock, &imask);
}

/*
 * The processes ended by an exception, for the desktop to show: a few
 * kept, the oldest let go when there is no room, so that faults one
 * after another cannot pile up without bound. Written from the
 * exception handler's side, under a spin lock only.
 */
#define FLT_QLEN	4

LOCAL T_PRCFLT		flt_q[FLT_QLEN];
LOCAL INT		flt_head = 0, flt_n = 0;
LOCAL UW		flt_seq = 0, flt_lost = 0;
LOCAL T_SPLOCK		flt_lock;

/* The record of p's end by an exception: f's kind and registers, p's own. Called with prc_mtxid held. */
LOCAL void flt_note( T_PCB *p, INT code, CONST T_PRCFLT *f, T_PRCFLT *out )
{
	*out = *f;
	out->pid = p->pid;
	out->exitcd = code;
	out->prog = p->prog;
	knl_memcpy(out->file, p->file, TS_FLT_FILE);
}

LOCAL void flt_put( T_PRCFLT *f )
{
	UINT	imask;

	ISpinLock(&flt_lock, &imask);
	f->seq = ++flt_seq;
	if ( flt_n == FLT_QLEN ) {
		flt_head = ( flt_head + 1 ) % FLT_QLEN;	/* the oldest makes room */
		flt_n--;
		flt_lost++;
	}
	flt_q[( flt_head + flt_n ) % FLT_QLEN] = *f;
	flt_n++;
	ISpinUnlock(&flt_lock, &imask);
}

EXPORT BOOL knl_prc_fault_take( T_PRCFLT *f, UW *p_lost )
{
	UINT	imask;
	BOOL	got = FALSE;

	if ( f == NULL ) {
		return FALSE;
	}
	ISpinLock(&flt_lock, &imask);
	if ( flt_n > 0 ) {
		*f = flt_q[flt_head];
		flt_head = ( flt_head + 1 ) % FLT_QLEN;
		flt_n--;
		got = TRUE;
	}
	if ( p_lost != NULL ) {
		*p_lost = flt_lost;
	}
	flt_lost = 0;
	ISpinUnlock(&flt_lock, &imask);

	return got;
}

/* The running task, when it is one of p's */
LOCAL TCB *prc_task( T_PCB *p )
{
	TCB	*t = knl_tsk_self();

	return ( p != NULL && t != NULL && t->owner == p->pid ) ? t : NULL;
}

/*
 * A call of a task of a process into the file layer, the objects, the
 * network stack, drawing or the windows begins. Every task of the process is counted, on
 * its own and for the process: none is taken down while it is inside.
 * A process that is to end lets no task in (E_DISWAI).
 *
 * No lock is taken on the way in: the calling task's process and its
 * control block stay while the task runs, the task's own count is
 * written only by the task, and the process's is counted atomically
 * before ending is looked at (prc_mark looks the other way round). A
 * call that finds the process marked goes out again; when the marking
 * left the finishing to it, it finishes the process and ends.
 */
EXPORT ER knl_prc_call_enter( void )
{
	T_PCB	*p = cur_pcb();
	TCB	*t = prc_task(p);
	BOOL	fin = FALSE;

	if ( t == NULL ) {
		return E_OK;
	}
	t->svcin++;
	(void)__atomic_add_fetch(&p->incall, 1, __ATOMIC_SEQ_CST);
	if ( !ENDING(p) ) {
		return E_OK;
	}
	t->svcin--;
	if ( __atomic_sub_fetch(&p->incall, 1, __ATOMIC_SEQ_CST) == 0 ) {
		prc_lock();
		fin = prc_mark(p, p->exitcd);	/* marked already: only who finishes it is decided */
		tk_unl_mtx(prc_mtxid);
	}
	if ( fin ) {
		prc_finish(p);
		tk_ext_tsk();			/* no return */
	}
	return E_DISWAI;
}

/*
 * A call of a task of a process ends: 'entered' when it began with
 * knl_prc_call_enter. When the process is to end -- marked while the
 * task was inside, or the kernel met an address of the process that
 * was not there while it worked for it -- the task does not go back to
 * the program: it ends, and the last task of the process to come out
 * finishes the process on the way.
 *
 * The way out takes no lock unless there is something to do under it:
 * the process is to end, the kernel met a bad address of it, or this
 * was the last call out while pages it gave back are held for the calls
 * (umem.c). The count goes down before ending is looked at, as on the
 * way in.
 */
EXPORT void knl_prc_call_leave( BOOL entered )
{
	T_PCB	*p;
	TCB	*t;
	BOOL	end = FALSE, fin = FALSE, fault = FALSE, note = FALSE, pan = FALSE;
	UBINT	va = 0;
	ID	pid = 0;
	INT	left;
	T_PRCFLT info;

	p = cur_pcb();
	t = prc_task(p);
	if ( t == NULL ) {
		return;
	}
	if ( entered && t->svcin > 0 ) {
		t->svcin--;
		left = __atomic_sub_fetch(&p->incall, 1, __ATOMIC_SEQ_CST);
	} else {
		left = INCALL(p);
	}
	if ( !p->kfault && !ENDING(p) && !( left == 0 && knl_umem_held(p->umem) ) ) {
		return;
	}

	prc_lock();
	{
		if ( INCALL(p) == 0 ) {
			knl_umem_flush(p->umem);	/* no call holds its pages now */
		}
		if ( p->kfault ) {
			p->kfault = FALSE;
			kfault_count(-1);
			fault = TRUE;
			pan = p->kfault_pan;
			p->kfault_pan = FALSE;
			va = p->kfault_va;
			pid = p->pid;
			if ( !p->ending ) {
				T_PRCFLT	f;

				knl_memset(&f, 0, sizeof(f));
				f.kind = pan ? TS_FLT_KPAN : TS_FLT_KCALL;
				f.tskid = t->tskid;
				f.esr = p->kfault_esr;
				f.far = (UD)va;
				f.elr = p->kfault_elr;
				f.lr = p->kfault_lr;
				flt_note(p, TS_ABORT_FAULT, &f, &info);
				note = TRUE;
				p->exitcd = TS_ABORT_FAULT;
				p->state  = PS_EXITING;
				p->end_at = 0;
				(void)ts_get_mono(&p->end_at);
				__atomic_store_n(&p->ending, TRUE, __ATOMIC_SEQ_CST);
			}
		}
		if ( p->ending ) {
			end = TRUE;
			fin = prc_mark(p, p->exitcd);	/* marked: only who finishes it is decided */
		}
	}
	tk_unl_mtx(prc_mtxid);

	if ( fault && pan ) {
		tm_printf((UB*)"TessronOS: the kernel touched %lx of pid %d without making sure of it: it ends\n",
			  va, pid);
	} else if ( fault ) {
		tm_printf((UB*)"TessronOS: pid %d handed the kernel %lx, not its own: it ends\n",
			  pid, va);
	}
	if ( note ) {
		flt_put(&info);
	}
	if ( end ) {
		if ( fin ) {
			prc_finish(p);
		}
		tk_ext_tsk();			/* no return */
	}
}

/*
 * The kernel met an address below the kernel half that was not mapped,
 * or not to be written, while the task on the space ttbr0 was in a call
 * (the exception handler, interrupts disabled). When the address is in
 * a process's half and the task is one of that process's, the page
 * is given a page of nothing (knl_space_sink) so that the call can run
 * to its end without leaving a volume, a store or a lock half done, and
 * the process is marked to end as the call returns. FALSE leaves the
 * fault to stop the system. No lock is taken: the faulting code may
 * hold any of them. The control blocks of the running task and of its
 * process stay while it runs.
 */
EXPORT BOOL knl_prc_kfault( UD ttbr0, ID tskid, UBINT va, UD esr, UD elr, UD lr )
{
	T_PCB	*p;
	TCB	*t = knl_tsk_tcb(tskid);

	if ( va < USER_TEXT_VA || va >= USER_STACK_TOP ) {
		return FALSE;
	}
	p = ttbr0_to_pcb(ttbr0);
	if ( p == NULL || t == NULL || t->owner != p->pid || p->space.l0_pa == 0 ) {
		return FALSE;
	}
	if ( knl_space_sink(&p->space, va) < E_OK ) {
		return FALSE;
	}
	if ( !p->kfault ) {
		p->kfault_va = va;
		p->kfault_esr = esr;
		p->kfault_elr = elr;
		p->kfault_lr = lr;
		p->kfault_pan = FALSE;
		p->kfault = TRUE;
		kfault_count(1);
	}
	return TRUE;
}

/*
 * The kernel touched a page of the process on the space ttbr0, one EL0
 * may reach, without opening the way to it (PAN; the exception handler,
 * interrupts disabled): it had not made sure of the address, or forgot
 * to say it had. The exception handler lets the call go on to its end
 * with the way open, as a call cut short would leave a volume, a store
 * or a lock half done; the process, which can no longer be trusted to
 * have handed only what it meant to, is marked to end as the call
 * returns. FALSE when the task is not one of the process's. No lock is
 * taken, as in knl_prc_kfault.
 */
EXPORT BOOL knl_prc_kpan( UD ttbr0, ID tskid, UBINT va, UD esr, UD elr, UD lr )
{
	T_PCB	*p = ttbr0_to_pcb(ttbr0);
	TCB	*t = knl_tsk_tcb(tskid);

	if ( p == NULL || t == NULL || t->owner != p->pid ) {
		return FALSE;
	}
	if ( !p->kfault ) {
		p->kfault_va = va;
		p->kfault_esr = esr;
		p->kfault_elr = elr;
		p->kfault_lr = lr;
		p->kfault_pan = TRUE;
		p->kfault = TRUE;
		kfault_count(1);
	}
	return TRUE;
}

/*
 * A translation or permission fault of the calling task at EL0 (the
 * exception handler, with interrupts enabled; esr and va as the
 * processor gave them). TRUE when the access is to be made again: the
 * page's entry allows it by now (another task of the process changed
 * it while the access was under way: a page changed is unmapped for a
 * moment), or it is a page of a stretch written and run by turns,
 * turned for the access (knl_umem_fault). FALSE leaves the fault to end
 * the process.
 */
EXPORT BOOL knl_prc_ufault( UBINT va, UD esr )
{
	T_PCB	*p;
	UD	ent;
	UINT	acc;
	BOOL	ok = FALSE;

	if ( knl_ufault_unfixable(esr) || va < USER_TEXT_VA || va >= USER_STACK_TOP ) {
		return FALSE;
	}
	if ( knl_ufault_is_exec(esr) ) {
		acc = UM_ACC_EXEC;
	} else if ( knl_ufault_is_data(esr) ) {
		acc = knl_ufault_is_write(esr) ? UM_ACC_WRITE : UM_ACC_READ;
	} else {
		return FALSE;
	}
	prc_lock();
	p = cur_pcb();
	if ( p != NULL && p->space.l0_pa != 0 ) {
		ent = knl_pt_entry(p->space.l0_pa, va & ~(UBINT)( PAGE_SIZE - 1 ));
		if ( ent != 0 && PTE_USER_R(ent)
		  && ( acc != UM_ACC_WRITE || PTE_USER_W(ent) )
		  && ( acc != UM_ACC_EXEC || PTE_USER_X(ent) ) ) {
			knl_tlb_flush_va(va);
			ok = TRUE;
		} else {
			ok = knl_umem_fault(p->umem, &p->space, va, acc);
		}
	}
	tk_unl_mtx(prc_mtxid);

	return ok;
}

/* ---------------------------------------------------------------- a fault handed back (ts_def_flt) */

EXPORT ER ts_def_flt( void (*f)( T_TSFLT *flt ) )
{
	T_PCB	*p;
	UBINT	va = (UBINT)f;
	ER	er = E_OK;

	if ( f != NULL && ( va < USER_TEXT_VA || va >= USER_STACK_TOP || ( va & UFLT_PC_MASK ) != 0 ) ) {
		return E_PAR;
	}
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		p->flt_entry = (UD)va;
	} else {
		er = E_CTX;			/* a task of the system's own has no such entry */
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

/* Calls only from the SVC gateway, which hands it the frame (knl_prc_flt_return) */
EXPORT ER ts_flt_ret( CONST T_TSFLT *flt )
{
	(void)flt;
	return E_CTX;
}

EXPORT BOOL knl_prc_flt_give( void *frame, UD esr, UD far )
{
	UD	*f = (UD *)frame;
	T_PCB	*p;
	T_TSFLT	c;
	UD	entry = 0;
	UBINT	sp;
	ID	pid = 0;

	if ( !knl_ufault_is_data(esr) ) {
		return FALSE;
	}
	prc_lock();
	p = cur_pcb();
	if ( p != NULL && !p->ending ) {
		entry = p->flt_entry;
		pid = p->pid;
	}
	tk_unl_mtx(prc_mtxid);
	if ( entry == 0 ) {
		return FALSE;
	}

	knl_memset(&c, 0, sizeof(c));
	knl_uflt_from_frame(&c, f);
	knl_uflt_set_fault(&c, far, esr);
	knl_uflt_save_fp(&c);

	/* the picture goes on the task's own stack, below where it was */
	sp = ( (UBINT)knl_uframe_sp(f) - sizeof(c) - 64 ) & ~(UBINT)UFLT_SP_MASK;
	if ( sp >= (UBINT)knl_uframe_sp(f) || !knl_prc_user_ok(pid, (CONST void *)sp, sizeof(c), TRUE) ) {
		return FALSE;
	}
	knl_ua_put((void *)sp, &c, sizeof(c));
	knl_uframe_arg(f, 0, sp);
	knl_uframe_call(f, entry, sp);		/* the entry does not return: it calls ts_flt_ret */

	return TRUE;
}

EXPORT ER knl_prc_flt_return( void *frame, CONST T_TSFLT *uflt )
{
	UD	*f = (UD *)frame;
	T_TSFLT	c;

	if ( !knl_prc_user_ok(ts_get_pid(), uflt, sizeof(c), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(&c, uflt, sizeof(c));
	if ( c.pc < USER_TEXT_VA || c.pc >= USER_STACK_TOP || ( c.pc & UFLT_PC_MASK ) != 0
	  || c.sp < USER_TEXT_VA || c.sp > USER_STACK_TOP || ( c.sp & UFLT_SP_MASK ) != 0 ) {
		return E_PAR;
	}
	knl_uflt_to_frame(f, &c);
	knl_uflt_load_fp(&c);

	return E_OK;
}

/*
 * What the pages of process pid's reservations allow (knl_umem_wx):
 * pages there, writable, executable, both, and the turns of the
 * stretches written and run by turns. For the tests.
 */
EXPORT ER knl_prc_mem_wx( ID pid, UD out[6] )
{
	T_PCB	*p;
	ER	er = E_NOEXS;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->used && p->space.l0_pa != 0 ) {
		knl_umem_wx(p->umem, &p->space, out);
		er = E_OK;
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

/*
 * The kernel's view of a process's memory for a transfer by a device
 * (design 10.8): the linear map address of the memory at uaddr in the
 * calling process, and how many bytes from there up to len lie in pages
 * that follow one another in memory. 'into' is TRUE when the device
 * writes the memory: the pages must then be the process's to write.
 * A process's pages outside the shared window go only when the space
 * is taken down, which does not happen while one of its tasks is inside
 * a call (knl_prc_call_enter), so the answer holds until the call
 * returns; the shared window's pages may be taken out by another
 * process and are not given, nor are the tasks' stacks above it, which
 * go with their tasks. E_PAR when
 * the caller is not a task of a process inside such a call, or the
 * first page is not there. Nothing is locked: the caller holds the
 * file layer's locks, and the only tables read are of the caller's own
 * space.
 */
EXPORT INT knl_prc_dma( CONST void *uaddr, INT len, BOOL into, void **p_kaddr )
{
	T_PCB	*p;
	TCB	*t;
	UBINT	va = (UBINT)uaddr, end, pg;
	UD	e, pa, first = 0, next = 0;

	if ( len <= 0 || p_kaddr == NULL || knl_self_ctxtsk() == NULL ) {
		return E_PAR;
	}
	p = cur_pcb();
	t = prc_task(p);
	if ( t == NULL || t->svcin <= 0 || p->space.l0_pa == 0 ) {
		return E_PAR;
	}
	end = va + (UBINT)len;
	if ( end < va || va < USER_TEXT_VA || va >= SHM_WINDOW_BASE ) {
		return E_PAR;
	}
	if ( end > SHM_WINDOW_BASE ) {
		end = SHM_WINDOW_BASE;
	}
	for ( pg = va & PAGE_MASK; pg < end; pg += PAGE_SIZE ) {
		e = knl_pt_entry(p->space.l0_pa, pg);

		/* the process may read it, and write it when it is to be written into */
		if ( e == 0 || !PTE_USER_OK(e, into) ) {
			break;
		}
		pa = e & PTE_ADDR_MASK;
		if ( pg == ( va & PAGE_MASK ) ) {
			first = pa;
		} else if ( pa != next ) {
			break;
		}
		next = pa + PAGE_SIZE;
	}
	if ( pg == ( va & PAGE_MASK ) ) {
		return E_PAR;			/* not even the first page */
	}
	*p_kaddr = PA2VA(first + ( va & ~PAGE_MASK ));

	return (INT)( ( ( pg < end ) ? pg : end ) - va );
}

/* Whether the calling task's process is to end as soon as it is out of the layer it is in */
EXPORT BOOL knl_prc_ending( void )
{
	T_PCB	*p = cur_pcb();		/* the caller's own: it stays while the caller runs */

	return (BOOL)( p != NULL && ENDING(p) );
}

/*
 * Called from the kernel when a task of a process faults at EL0 (design
 * 9.11), on that task, which ends when this returns. The process ends
 * with it: finished here, or by the last of its other tasks to come
 * out of a bracketed call. The fault is recorded for the desktop when
 * it is what ends the process: not for a process already ending, nor
 * for a second task of it faulting as well.
 */
/*
 * The code around where a task of the process at EL0 met its exception,
 * as the process's pages hold it, into the record: read through the
 * process's own mapping (the faulting task's space is the one in use),
 * four bytes at a time, those on a page not mapped left out. Set beside
 * the instruction the processor reported, it tells a program that is
 * not the one thought apart from pages that changed under it. Put on
 * the console too.
 */
LOCAL void flt_code( UD ttbr0, T_PRCFLT *f )
{
	UD	l0 = ttbr0 & PTE_ADDR_MASK;
	UBINT	va = (UBINT)f->elr - TS_FLT_CODE_BACK, w;
	UW	*code = (UW *)f->code;
	INT	i;

	f->codeok = 0;
	f->codepa = knl_pt_lookup(l0, (UBINT)f->elr & PAGE_MASK);
	if ( f->codepa != ~(UD)0 ) {
		f->codepa += (UBINT)f->elr & ~PAGE_MASK;
	}
	for ( i = 0; i < TS_FLT_CODE / 4; i++ ) {
		w = va + (UBINT)i * 4;
		code[i] = 0;
		if ( w < USER_TEXT_VA || w + 4 > USER_STACK_TOP
		  || knl_pt_lookup(l0, w & PAGE_MASK) == ~(UD)0
		  || knl_pt_lookup(l0, ( w + 3 ) & PAGE_MASK) == ~(UD)0 ) {
			continue;
		}
		knl_ua_get(&code[i], (CONST void *)w, 4);
		f->codeok |= 1U << i;
	}
	tm_printf((UB*)"TessronOS: task %d code at %lx (pa %lx) from -%d:"
		  " %08x %08x %08x %08x [%08x] %08x %08x %08x (read %02x)\n",
		  f->tskid, f->elr, f->codepa, TS_FLT_CODE_BACK,
		  code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7], f->codeok);
}

EXPORT void knl_prc_abort( UD ttbr0, INT abortcd, CONST T_PRCFLT *f )
{
	T_PCB	*p;
	BOOL	fin = FALSE, note = FALSE;
	T_PRCFLT info, fc;

	if ( f != NULL && f->kind == TS_FLT_EL0 ) {
		fc = *f;
		flt_code(ttbr0, &fc);
		f = &fc;
	}
	prc_lock();
	p = ttbr0_to_pcb(ttbr0);
	if ( p != NULL ) {
		if ( !p->ending && f != NULL ) {
			flt_note(p, abortcd, f, &info);
			note = TRUE;
		}
		fin = prc_mark(p, abortcd);
	}
	tk_unl_mtx(prc_mtxid);

	if ( note ) {
		flt_put(&info);
	}
	if ( fin ) {
		prc_finish(p);
	}
}

/*
 * A name a person knows the program by: the name of the program object
 * it was started from, or else the file, or else its number. max is at
 * least 16.
 */
LOCAL INT name_of( ID pid, CONST TS_UUID *prog, CONST UB *file, UB *buf, INT max )
{
	T_OBREF	*r;
	INT	n = 0;

	if ( buf == NULL || max < 16 ) {
		return 0;
	}
	buf[0] = 0;
	if ( prog != NULL && !( prog->d.hi == 0 && prog->d.lo == 0 )
	  && ( r = (T_OBREF *)Kmalloc(sizeof(T_OBREF)) ) != NULL ) {
		if ( ob_ref_obj(prog, r) >= E_OK ) {
			while ( n < max - 1 && n < OB_NAME_MAX && r->name[n] != 0 ) {
				buf[n] = r->name[n];
				n++;
			}
			/* never half a character */
			while ( n > 0 && n < OB_NAME_MAX && r->name[n] != 0 && ( r->name[n] & 0xC0 ) == 0x80 ) {
				n--;
			}
		}
		Kfree(r);
	}
	if ( n == 0 && file != NULL ) {
		while ( n < max - 1 && n < TS_FLT_FILE && file[n] != 0 ) {
			buf[n] = file[n];
			n++;
		}
	}
	if ( n == 0 ) {
		n = tm_sprintf(buf, (CONST UB *)"pid %d", pid);
	}
	buf[n] = 0;
	return n;
}

EXPORT INT knl_prc_name( ID pid, UB *buf, INT max )
{
	T_PCB	*p;
	TS_UUID	prog;
	UB	file[TS_FLT_FILE];

	knl_memset(&prog, 0, sizeof(prog));
	file[0] = 0;
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL ) {
		prog = p->prog;
		knl_memcpy(file, p->file, TS_FLT_FILE);
	}
	tk_unl_mtx(prc_mtxid);

	return name_of(pid, &prog, file, buf, max);
}

/*
 * The file a process was started from, read without the table's lock
 * and without allocating: for what is said as the system stops
 * (lockup.c), where neither can be had. The name may be one being
 * written; at worst it is wrong, never out of the table. Answers the
 * length put in buf, 0 when there is no such process.
 */
EXPORT INT knl_prc_file_peek( ID pid, UB *buf, INT max )
{
	T_PCB	*p = pid_to_pcb(pid);
	INT	n = 0;

	if ( p != NULL && p->used ) {
		while ( n < max - 1 && n < TS_FLT_FILE && p->file[n] != 0 ) {
			buf[n] = p->file[n];
			n++;
		}
	}
	if ( max > 0 ) {
		buf[n] = 0;
	}
	return n;
}

EXPORT INT knl_prc_flt_name( CONST T_PRCFLT *f, UB *buf, INT max )
{
	return ( f != NULL ) ? name_of(f->pid, &f->prog, f->file, buf, max) : 0;
}

/*
 * End a process from the keyboard. A process the system needs is left
 * alone: init, and whatever the kernel started with TS_PRC_SYSTEM.
 */
EXPORT ER knl_prc_force_end( ID pid )
{
	T_PCB	*p;
	ER	er = E_OK;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->state == PS_ZOMBIE || p->state == PS_NONEXIST ) {
		er = E_NOEXS;
	} else if ( p->sys || pid == prc_init_pid ) {
		er = E_OACV;
	} else if ( p->ending ) {
		er = E_OBJ;
	}
	tk_unl_mtx(prc_mtxid);
	if ( er < E_OK ) {
		return er;
	}
	tm_printf((UB*)"TessronOS: pid %d ended from the keyboard (Shift+Pause)\n", pid);

	return ts_ter_prc(pid, TS_ABORT_TERM);
}

/* Whether the process pid of generation gen has finished: gone, or its end told */
LOCAL BOOL prc_done( ID pid, UW gen )
{
	T_PCB	*p;
	T_RFLG	rf;
	BOOL	done;

	prc_lock();
	p = pid_to_pcb(pid);
	done = (BOOL)( p == NULL || !p->used || p->gen != gen
		    || ( p->flgid > 0 && tk_ref_flg(p->flgid, &rf) >= E_OK
		      && ( rf.flgptn & PRC_FLG_EXIT ) != 0 ) );
	tk_unl_mtx(prc_mtxid);

	return done;
}

/*
 * The waits of a process's tasks that only wait for something to
 * happen -- an event flag, a message buffer, a mailbox, a sleep or a
 * delay -- cut (tk_rel_wai): the call they are in sees E_RLWAI and
 * comes out. A wait for a semaphore or a mutex is left: some of the
 * kernel's layers keep their locks in semaphores and go on as if they
 * had the lock when the wait ends, so cutting it would let two into
 * what the lock guards. Answers how many were cut.
 */
#define TTW_CUT	( TTW_SLP | TTW_DLY | TTW_FLG | TTW_MBX | TTW_SMBF | TTW_RMBF )

LOCAL INT prc_cut_waits( ID pid )
{
	ID	tids[TS_PRC_TSK_MAX];
	T_RTSK	r;
	INT	n, i, cut = 0;

	prc_lock();
	n = knl_own_list(TK_OWN_TSK, pid, tids, TS_PRC_TSK_MAX);
	tk_unl_mtx(prc_mtxid);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		if ( tk_ref_tsk(tids[i], &r) < E_OK || ( r.tskstat & TTS_WAI ) == 0 ) {
			continue;
		}
		if ( ( r.tskwait & TTW_CUT ) != 0 && ( r.tskwait & ~TTW_CUT ) == 0
		  && tk_rel_wai(tids[i]) >= E_OK ) {
			cut++;
		}
	}
	return cut;
}

/*
 * End a process that did not end when it was asked to (design 5.5.4).
 * First as ts_ter_prc would, then its waits that are not for a lock are
 * cut and it is given PRC_KILL_WAIT to come out of its calls by itself.
 * Whatever has not come out by then is stopped where it stands: its
 * tasks are made dormant inside the call (a mutex they hold is let go
 * by the core as they are), what it has open is closed, and its end is
 * told as any end is. The pages of one stopped inside a call are not
 * given back (keep_space). What the mutexes it held guard may be left
 * half changed; that is the price of this step, which is why it is
 * taken only when asked for a second time.
 */
EXPORT ER knl_prc_kill( ID pid )
{
	ID	tids[TS_PRC_TSK_MAX];
	T_PCB	*p;
	UW	gen = 0;
	INT	n, i, k, inside = 0, held = 0, cut = 0;
	BOOL	fin = FALSE;
	ER	er = E_OK;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || !p->used || p->state == PS_ZOMBIE || p->state == PS_NONEXIST ) {
		er = E_NOEXS;
	} else if ( p->sys || pid == prc_init_pid ) {
		er = E_OACV;
	} else if ( p->killed ) {
		er = E_OBJ;			/* done once: only collecting it is left */
	} else {
		gen = p->gen;
		fin = prc_mark(p, TS_ABORT_TERM);
	}
	tk_unl_mtx(prc_mtxid);
	if ( er < E_OK ) {
		return er;
	}
	if ( fin ) {
		prc_finish(p);			/* it was in no call after all */
		return E_OK;
	}
	tm_printf((UB*)"TessronOS: pid %d is ended by force: the waits of its tasks are cut\n", pid);

	for ( k = 0; k < PRC_KILL_WAIT / PRC_KILL_STEP; k++ ) {
		if ( prc_done(pid, gen) ) {
			return E_OK;
		}
		if ( k % 4 == 0 ) {
			cut += prc_cut_waits(pid);
		}
		tk_dly_tsk(PRC_KILL_STEP);
	}
	if ( prc_done(pid, gen) ) {
		return E_OK;
	}

	/* stopped where they stand */
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->gen != gen ) {
		tk_unl_mtx(prc_mtxid);
		return E_OK;
	}
	n = knl_own_list(TK_OWN_TSK, pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		TCB	*t = knl_tsk_tcb(tids[i]);

		/* looked at before it stops: the core lets its mutexes go as it does */
		if ( t != NULL && t->owner == pid ) {
			if ( t->svcin > 0 || t->svcdepth > 0 ) {
				inside++;
			}
			if ( t->mtxlist != NULL ) {
				held++;
			}
		}
		tsk_stop(tids[i]);
		if ( t != NULL && t->owner == pid ) {
			t->svcin = 0;
			t->svcdepth = 0;
		}
	}
	__atomic_store_n(&p->incall, 0, __ATOMIC_SEQ_CST);
	p->killed = TRUE;
	if ( inside > 0 ) {
		p->keep_space = TRUE;
	}
	p->finishing = TRUE;
	tk_unl_mtx(prc_mtxid);

	tm_printf((UB*)"TessronOS: pid %d: %d wait(s) cut, %d task(s) stopped inside a call, "
		  "%d of them holding a mutex\n", pid, cut, inside, held);
	prc_finish(p);

	return E_OK;
}

/* What the list of 強制終了 shows of a process */
LOCAL UD cyc_ms( UD cyc )
{
	UD	f;

	f = SysCounterFreq();
	return ( f != 0 ) ? cyc / ( f / 1000 ) : 0;
}

EXPORT ER knl_prc_info( ID pid, T_PRCINFO *pi )
{
	ID	tids[TS_PRC_TSK_MAX];
	T_PCB	*p;
	T_RFLG	rf;
	UD	now = 0, cyc = 0;
	INT	n, i;

	if ( pi == NULL ) {
		return E_PAR;
	}
	knl_memset(pi, 0, sizeof(*pi));
	(void)ts_get_mono(&now);
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || !p->used ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;
	}
	pi->pid = p->pid;
	pi->ppid = p->ppid;
	pi->state = p->state;
	pi->sys = (BOOL)( p->sys || pid == prc_init_pid );
	pi->ending = p->ending;
	pi->killed = p->killed;
	pi->ending_ms = ( p->ending && p->end_at != 0 && now > p->end_at )
		      ? (UW)( ( now - p->end_at ) / 1000000ULL ) : 0;
	pi->incall = INCALL(p);
	pi->done = (BOOL)( p->flgid > 0 && tk_ref_flg(p->flgid, &rf) >= E_OK
			&& ( rf.flgptn & PRC_FLG_EXIT ) != 0 );
	pi->memsz = p->memsz + knl_umem_bytes(p->umem);
	n = knl_own_list(TK_OWN_TSK, pid, tids, TS_PRC_TSK_MAX);
	pi->ntsk = n;
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		TCB	*t = knl_tsk_tcb(tids[i]);

		if ( t != NULL && t->owner == pid ) {
			cyc += t->runcyc;
		}
	}
	tk_unl_mtx(prc_mtxid);
	pi->cpu_ms = cyc_ms(cyc);

	return E_OK;
}

/*
 * Start the init process (design 9.14). It is the first program the
 * system runs by itself, and it takes over the children of any process
 * that ends before them. Returns its pid, or an error when there is no
 * program to start; a system without one keeps running usermain() alone.
 */
EXPORT ID knl_start_init( CONST char *path )
{
	T_CPRC	cprc;
	T_FSTAT	st;
	UW	arg[2];
	ID	pid;

	if ( path == NULL ) {
		return E_PAR;
	}
	if ( fs_stat(path, &st) < EX_OK ) {
		return E_NOEXS;
	}
	/* init is given an argument block of zeros, so a program that reads
	   one finds a mapped page rather than faulting */
	arg[0] = 0;
	arg[1] = 0;

	cprc.pri    = CNF_INIT_PRI;
	cprc.prcatr = TS_PRC_SYSTEM;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);

	pid = ts_cre_prc(path, &cprc);
	if ( pid > 0 ) {
		prc_lock();
		prc_init_pid = pid;
		tk_unl_mtx(prc_mtxid);
		tm_printf((UB*)"TessronOS: init started as pid %d from %s\n", pid, path);
	}

	return pid;
}

/* The control blocks there are: processes running, ending, and ended and not yet freed */
EXPORT ID knl_prc_reaper_tid( void )
{
	return prc_reaper_tid;
}

EXPORT INT knl_prc_count( void )
{
	return prc_count;
}

EXPORT ID knl_init_pid( void )
{
	return prc_init_pid;
}

/* ---------------------------------------------------------------- messages */

/*
 * Put a message in the queue of another process. Waits while the queue
 * is full, up to the timeout.
 */
EXPORT ER ts_snd_msg( ID pid, CONST T_TSMSG *msg, TMO tmout )
{
	T_PCB	*self;
	ID	from;

	prc_lock();
	self = cur_pcb();
	from = ( self != NULL ) ? self->pid : 0;
	tk_unl_mtx(prc_mtxid);

	return snd_msg_as(pid, msg, tmout, from);
}

/* The same, the message said to be from 'from' */
LOCAL ER snd_msg_as( ID pid, CONST T_TSMSG *msg, TMO tmout, ID from )
{
	T_PCB	*p;
	ID	sem;
	UW	gen;
	ER	er;

	if ( msg == NULL || msg->size > TS_MSG_MAX ) {
		return E_PAR;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->state != PS_RUNNING ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;
	}
	er = msgq_ready(p);
	sem = p->msg_free;
	gen = p->gen;
	tk_unl_mtx(prc_mtxid);
	if ( er < E_OK ) {
		return er;
	}

	er = tk_wai_sem(sem, 1, tmout);
	if ( er < E_OK ) {
		return er;			/* the queue stayed full */
	}

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->gen != gen || p->msgq == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;			/* it ended while we waited */
	}
	p->msgq[p->msg_tail] = *msg;
	p->msgq[p->msg_tail].from = from;
	p->msg_tail = (p->msg_tail + 1) % TS_MSG_QLEN;
	sem = p->msg_used;
	tk_unl_mtx(prc_mtxid);

	tk_sig_sem(sem, 1);

	return E_OK;
}

/*
 * Take the oldest message of the calling process.
 */
EXPORT ER ts_rcv_msg( T_TSMSG *buf, TMO tmout )
{
	T_PCB	*p;
	ID	sem;
	ER	er;

	if ( buf == NULL ) {
		return E_PAR;
	}
	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;			/* not a process */
	}
	er = msgq_ready(p);
	sem = p->msg_used;
	tk_unl_mtx(prc_mtxid);
	if ( er < E_OK ) {
		return er;
	}

	er = tk_wai_sem(sem, 1, tmout);
	if ( er < E_OK ) {
		return er;			/* nothing arrived */
	}

	prc_lock();
	*buf = p->msgq[p->msg_head];
	p->msg_head = (p->msg_head + 1) % TS_MSG_QLEN;
	sem = p->msg_free;
	tk_unl_mtx(prc_mtxid);

	tk_sig_sem(sem, 1);

	return E_OK;
}

/* ---------------------------------------------------------------- wait */

EXPORT ER ts_wai_prc( ID pid, T_PSTS *pk_psts, TMO tmout )
{
	T_PCB	*p = NULL, *c;
	T_PCB	*self;
	QUEUE	*q;
	UINT	ptn;
	ID	flgid, gone;
	UW	gen;
	ER	er;

	prc_lock();
	self = cur_pcb();

	if ( pid == TS_PRC_ANY ) {
		ID	myid = ( self != NULL ) ? self->pid : 0;

		FOR_EACH_PCB(c, q) {
			if ( c->ppid == myid ) { p = c; break; }
		}
	} else {
		p = pid_to_pcb(pid);
	}
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;
	}
	flgid = p->flgid;
	pid = p->pid;
	gen = p->gen;
	p->waiters++;			/* not freed for want of a collector meanwhile */
	tk_unl_mtx(prc_mtxid);

	er = tk_wai_flg(flgid, PRC_FLG_EXIT, TWF_ORW, &ptn, tmout);

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->gen == gen ) {
		p->waiters--;
	}
	if ( er < E_OK ) {
		tk_unl_mtx(prc_mtxid);
		return er;
	}
	if ( p == NULL || p->gen != gen ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;			/* someone else collected it */
	}
	if ( pk_psts != NULL ) {
		pk_psts->exitcd = p->exitcd;
	}
	/* its children lose their parent: init takes them (design 9.5) */
	FOR_EACH_PCB(c, q) {
		if ( c->ppid == p->pid ) {
			c->ppid = prc_init_pid;
		}
	}
	/* collect: its tasks are leaving the space or have left it, so it can be freed */
	gone = p->pid;
	prc_release(p);			/* unless prc_reaper has done it already */
	p->state = PS_ZOMBIE;
	prc_free(p);			/* p is gone from here */
	tk_unl_mtx(prc_mtxid);

	/* a process that faulted closed nothing itself; its credentials go now */
	prc_close_all(gone);
	knl_ob_prc_gone(gone);

	return E_OK;
}

EXPORT ER ts_ref_prc( ID pid, T_RPRC *pk_rprc )
{
	T_PCB	*p;
	ER	er = E_OK;

	if ( pk_rprc == NULL ) {
		return E_PAR;
	}
	prc_lock();

	p = ( pid == 0 ) ? cur_pcb() : pid_to_pcb(pid);
	if ( p == NULL ) {
		er = E_NOEXS;
	} else {
		pk_rprc->pid     = p->pid;
		pk_rprc->ppid    = p->ppid;
		pk_rprc->state   = p->state;
		pk_rprc->exitcd  = p->exitcd;
		pk_rprc->ntsk    = knl_own_list(TK_OWN_TSK, p->pid, NULL, 0);
		pk_rprc->memsz   = p->memsz + knl_umem_bytes(p->umem);
		pk_rprc->maintsk = p->maintsk;
	}

	tk_unl_mtx(prc_mtxid);

	return er;
}

/* ---------------------------------------------------------------- objects of the core */

/*
 * How many objects of each kind one process may have, and all
 * processes together (design 9.7): a process does not use up a table
 * the kernel and the others draw on. Its main task counts among its
 * tasks.
 */
LOCAL CONST struct {
	INT	one;
	INT	all;
} own_quota[TK_OWN_MBF + 1] = {
	{ TS_PRC_TSK_MAX, CNF_MAX_TSKID - CNF_KNL_TSK_ROOM },
	{ TS_PRC_SEM_MAX, CNF_MAX_SEMID - CNF_KNL_TSK_ROOM },
	{ TS_PRC_FLG_MAX, CNF_MAX_FLGID - CNF_KNL_TSK_ROOM },
	{ TS_PRC_MTX_MAX, CNF_MAX_MTXID - CNF_KNL_TSK_ROOM },
	{ TS_PRC_MBF_MAX, CNF_MAX_MBFID - CNF_KNL_TSK_ROOM },
};

/* The calling process: its number, main task and base priority; 0 when the caller is not one */
EXPORT ID knl_prc_self( ID *p_main, PRI *p_base )
{
	T_PCB	*p;
	ID	pid = 0;

	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		pid = p->pid;
		if ( p_main != NULL ) *p_main = p->maintsk;
		if ( p_base != NULL ) *p_base = p->basepri;
	}
	tk_unl_mtx(prc_mtxid);

	return pid;
}

LOCAL ER own_room( INT kind, ID pid )
{
	if ( knl_own_list(kind, pid, NULL, 0) >= own_quota[kind].one
	  || knl_own_list(kind, TK_OWN_ANY, NULL, 0) >= own_quota[kind].all ) {
		return E_LIMIT;
	}
	return E_OK;
}

/*
 * An object of the core made for the calling process, when it has room
 * for one more of the kind: the packet (in the kernel) as tk_cre_sem,
 * tk_cre_flg, tk_cre_mtx or tk_cre_mbf take it. The object is the
 * process's from the moment it is made. A process that is to end makes
 * nothing more (E_DISWAI).
 */
EXPORT ID knl_prc_tk_cre( INT kind, CONST void *pk )
{
	T_PCB	*p;
	ID	id;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		id = E_OBJ;
	} else if ( p->ending ) {
		id = E_DISWAI;
	} else if ( ( id = own_room(kind, p->pid) ) >= E_OK ) {
		knl_svc_own(p->pid);
		switch ( kind ) {
		  case TK_OWN_SEM:	id = tk_cre_sem((CONST T_CSEM *)pk);	break;
		  case TK_OWN_FLG:	id = tk_cre_flg((CONST T_CFLG *)pk);	break;
		  case TK_OWN_MTX:	id = tk_cre_mtx((CONST T_CMTX *)pk);	break;
		  case TK_OWN_MBF:	id = tk_cre_mbf((CONST T_CMBF *)pk);	break;
		  default:		id = E_PAR;				break;
		}
		knl_svc_own(0);
	}
	tk_unl_mtx(prc_mtxid);

	return id;
}

/*
 * A task made for the calling process, in its space: at protection
 * level 3, with the FPU, a user stack of ustksz bytes in the process's
 * space (0: the default), and no higher priority than the process was
 * started with (E_PAR). What else the packet may ask for is the
 * process's to choose: the processors it runs on and its name. The
 * task is the process's from the moment it is made.
 */
EXPORT ID knl_prc_cre_tsk( CONST T_CTSK *pk )
{
	T_PCB	*p;
	T_CTSK	c;
	UD	ustk;
	ID	tid;

	ustk = ( pk->stksz > 0 ) ? (UD)pk->stksz : PRC_DEFAULT_STKSZ;
	ustk = ( ustk + PAGE_SIZE - 1 ) & ~(UD)( PAGE_SIZE - 1 );
	if ( pk->stksz < 0 || ustk > PRC_TSK_STKMAX ) {
		return E_PAR;
	}
	c = *pk;
	c.tskatr = TA_HLNG | TA_RNG3 | TA_FPU | ( pk->tskatr & ( TA_ASSPRC | TA_DSNAME ) );
	c.stksz  = 0;			/* the system stack: the least a task of a process has */
	c.bufptr = NULL;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tid = E_OBJ;
	} else if ( p->ending ) {
		tid = E_DISWAI;
	} else if ( c.itskpri < p->basepri || c.itskpri > CNF_MAX_TSKPRI ) {
		tid = E_PAR;
	} else if ( ( tid = own_room(TK_OWN_TSK, p->pid) ) >= E_OK ) {
		knl_new_task_space = &p->space;
		knl_set_ustack_size(ustk);
		knl_svc_own(p->pid);
		tid = tk_cre_tsk(&c);
		knl_svc_own(0);
		knl_set_ustack_size(0);
		knl_new_task_space = NULL;
		if ( tid > 0 && knl_tsk_set_space(tid, p->space.ttbr0) < E_OK ) {
			(void)tk_del_tsk(tid);
			tid = E_SYS;
		}
	}
	tk_unl_mtx(prc_mtxid);

	return tid;
}

/* ---------------------------------------------------------------- as objects */

/* The UUID of a process and of the program it came from (design 18.8) */
/* ---------------------------------------------------------------- the shared window */

EXPORT ER knl_prc_map_shared( ID pid, UBINT va, UD pa, UD size, BOOL write )
{
	T_PCB	*p;
	ER	er;

	if ( va < SHM_WINDOW_BASE || va + size > SHM_WINDOW_END || size == 0 ) {
		return E_PAR;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOEXS;
	} else {
		/* mapped afresh: a number used before may be a new process */
		(void)knl_pt_unmap(p->space.l0_pa, va, size);
		er = knl_pt_map(p->space.l0_pa, va, pa, size,
				write ? PTE_PAGE_UDATA : PTE_PAGE_URO);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT ER knl_prc_unmap_shared( ID pid, UBINT va, UD size )
{
	T_PCB	*p;
	ER	er = E_NOEXS;

	if ( va < SHM_WINDOW_BASE || va + size > SHM_WINDOW_END ) {
		return E_PAR;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->space.l0_pa != 0 ) {
		er = knl_pt_unmap(p->space.l0_pa, va, size);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT ER knl_prc_space_call( ID pid, ER (*fn)( void *space, void *arg ), void *arg )
{
	T_PCB	*p;
	ER	er = E_NOEXS;

	if ( fn == NULL ) {
		return E_PAR;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->space.l0_pa != 0 ) {
		er = fn(&p->space, arg);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

IMPORT UBINT	knl_pt_l0_k_pa;

/*
 * The pages of a knl_vmap area are not one run of memory: each is found
 * where the kernel's own tables put it and mapped by itself.
 */
EXPORT ER knl_prc_map_kernel( ID pid, CONST void *kva, UD size, UBINT *p_va )
{
	T_PCB	*p;
	UBINT	va;
	UD	off = 0, pa;
	ER	er = E_OK;

	if ( kva == NULL || size == 0 || ( ( (UBINT)kva | size ) & ( PAGE_SIZE - 1 ) ) != 0 || p_va == NULL ) {
		return E_PAR;
	}
	va = knl_shm_va_alloc(size);
	if ( va == 0 ) {
		return E_LIMIT;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOEXS;
	}
	for ( ; er >= E_OK && off < size; off += PAGE_SIZE ) {
		pa = knl_pt_lookup(knl_pt_l0_k_pa, (UBINT)kva + off);
		if ( pa == ~(UD)0 ) {
			er = E_PAR;		/* not the kernel's pages */
		} else {
			er = knl_pt_map(p->space.l0_pa, va + off, pa, PAGE_SIZE, PTE_PAGE_UDATA);
		}
	}
	if ( er < E_OK && p != NULL && p->space.l0_pa != 0 && off > 0 ) {
		(void)knl_pt_unmap(p->space.l0_pa, va, off);
	}
	tk_unl_mtx(prc_mtxid);
	if ( er >= E_OK ) {
		*p_va = va;
	}
	return er;
}

EXPORT ER knl_prc_sink_shared( ID pid, UBINT va, UD size )
{
	T_PCB	*p;
	UD	off;
	ER	er = E_NOEXS;

	if ( va < SHM_WINDOW_BASE || va + size > SHM_WINDOW_END || ( ( va | size ) & ( PAGE_SIZE - 1 ) ) != 0 ) {
		return E_PAR;
	}
	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->space.l0_pa != 0 ) {
		er = E_OK;
		for ( off = 0; off < size && er >= E_OK; off += PAGE_SIZE ) {
			er = knl_space_sink(&p->space, va + off);
		}
		if ( er < E_OK ) {
			/* no page of nothing to put there: the pages are taken away all the same */
			(void)knl_pt_unmap(p->space.l0_pa, va, size);
		}
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT BOOL knl_prc_user_ok( ID pid, CONST void *p, SZ len, BOOL write )
{
	T_PCB	*pc;
	UBINT	va = (UBINT)p, end, pg;
	BOOL	ok = TRUE, mine;

	if ( p == NULL || len < 0 ) {
		return FALSE;
	}
	end = va + (UBINT)len;
	if ( end < va || end > USER_STACK_TOP || va < USER_TEXT_VA ) {
		return FALSE;			/* not in the process's half at all */
	}
	/*
	 * The caller's own process -- every call asks of its own -- needs no
	 * lock: its control block and its space stay while the caller runs,
	 * and the tables are walked as knl_pt_entry walks them, without the
	 * lock of the tables. Another process's block may go meanwhile, and
	 * is looked at under the table's lock.
	 */
	pc = cur_pcb();
	mine = ( pc != NULL && pc->pid == pid );
	if ( !mine ) {
		prc_lock();
		pc = pid_to_pcb(pid);
	}
	if ( pc == NULL || pc->space.l0_pa == 0 ) {
		ok = FALSE;
	}
	for ( pg = va & PAGE_MASK; ok && pg < end; pg += PAGE_SIZE ) {
		UD	e = knl_pt_entry(pc->space.l0_pa, pg);

		/* the process may read it, and write it when it is to be written */
		ok = ( e != 0 && PTE_USER_OK(e, write) );
	}
	if ( !mine ) {
		tk_unl_mtx(prc_mtxid);
	}

	return ok;
}

EXPORT UD knl_prc_lookup( ID pid, UBINT va )
{
	T_PCB	*p;
	UD	pa = ~(UD)0;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL && p->space.l0_pa != 0 ) {
		pa = knl_pt_lookup(p->space.l0_pa, va & PAGE_MASK);
		if ( pa != ~(UD)0 ) {
			pa += va & ~PAGE_MASK;
		}
	}
	tk_unl_mtx(prc_mtxid);

	return pa;
}

EXPORT ER knl_prc_uuid( ID pid, TS_UUID *p_uuid, TS_UUID *p_prog )
{
	T_PCB	*p;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p != NULL ) {
		if ( p_uuid != NULL ) *p_uuid = p->uuid;
		if ( p_prog != NULL ) *p_prog = p->prog;
	}
	tk_unl_mtx(prc_mtxid);

	return ( p != NULL ) ? E_OK : E_NOEXS;
}

EXPORT ID knl_prc_of_uuid( CONST TS_UUID *uuid )
{
	ID	pid = E_NOEXS;
	T_PCB	*p;
	QUEUE	*q;

	if ( uuid == NULL || ( uuid->d.hi == 0 && uuid->d.lo == 0 ) ) {
		return E_NOEXS;
	}
	prc_lock();
	FOR_EACH_PCB(p, q) {
		if ( ts_uuid_cmp(&p->uuid, uuid) == 0 ) {
			pid = p->pid;
			break;
		}
	}
	tk_unl_mtx(prc_mtxid);

	return pid;
}

/* The processes there are: how many were put in pids */
EXPORT INT knl_prc_list( ID *pids, INT n )
{
	INT	k = 0;
	T_PCB	*p;
	QUEUE	*q;

	prc_lock();
	FOR_EACH_PCB(p, q) {
		if ( k >= n ) {
			break;
		}
		pids[k++] = p->pid;
	}
	tk_unl_mtx(prc_mtxid);

	return k;
}

/* ---------------------------------------------------------------- memory asked for */

/*
 * Pages a change of the reservations takes away are always held, and
 * given back at once only when no call of the process is inside (umem.c).
 * The count is looked at after the change, past a full barrier: a call
 * counts itself in before it reads the tables, so either it finds the
 * page gone or the count shows it, and the page then waits for the last
 * call out (knl_prc_call_leave).
 */
LOCAL void umem_settle( T_PCB *p )
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if ( INCALL(p) == 0 ) {
		knl_umem_flush(p->umem);
	}
}

/*
 * Reservations and pages in the calling process's own half (design
 * 9.8.1, include/ts/umem.h). A page taken away while another task of
 * the process is inside a call is held back until none is (umem.c).
 * The kernel's tasks have no such memory (E_NOSPT).
 */
EXPORT ER ts_map_mem( void **p_adr, SZ size, SZ align, UINT prot )
{
	T_PCB	*p;
	UBINT	va;
	ER	er;

	if ( p_adr == NULL || size <= 0 || align < 0 ) {
		return E_PAR;
	}
	prc_lock();
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		va = (UBINT)*p_adr;
		er = knl_umem_map(&p->umem, &p->space, &va, (UD)size, (UD)align, prot, TRUE);
		if ( er == E_OK ) {
			*p_adr = (void *)va;
		}
		umem_settle(p);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT ER ts_unm_mem( void *adr, SZ size )
{
	T_PCB	*p;
	ER	er;

	if ( size <= 0 ) {
		return E_PAR;
	}
	prc_lock();
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		er = knl_umem_unmap(&p->umem, &p->space, (UBINT)adr, (UD)size, TRUE);
		umem_settle(p);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT ER ts_ctl_mem( void *adr, SZ size, UINT prot )
{
	T_PCB	*p;
	ER	er;

	if ( size <= 0 ) {
		return E_PAR;
	}
	prc_lock();
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		er = knl_umem_ctl(&p->umem, &p->space, (UBINT)adr, (UD)size, prot, TRUE);
		umem_settle(p);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}


/*
 * The start-up argument of the calling process, as far as `size` holds
 * it; answers its whole size, 0 when it was started without one.
 */
EXPORT SZ ts_get_arg( void *buf, SZ size )
{
	T_PCB	*p;
	SZ	n = 0;
	void	*page;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;
	}
	n = p->argsz;
	if ( n > 0 && buf != NULL && size > 0 ) {
		SZ	want = ( size < n ) ? size : n, at;

		for ( at = 0; at < want; at += PAGE_SIZE ) {
			SZ	k = want - at;

			page = knl_space_page(&p->space, PRC_ARG_VA + (UBINT)at);
			if ( page == NULL ) break;
			knl_memcpy((UB *)buf + at, page, (INT)( ( k > (SZ)PAGE_SIZE ) ? (SZ)PAGE_SIZE : k ));
		}
	}
	tk_unl_mtx(prc_mtxid);

	return n;
}

/* The caller's process: its block stays while the caller runs, so no lock is taken */
EXPORT ID ts_get_pid( void )
{
	T_PCB	*p = cur_pcb();

	return ( p != NULL ) ? p->pid : 0;
}

/* ---------------------------------------------------------------- POSIX (design 5.8) */

#define SIG_BIT(n)	( 1ULL << (n) )
#define SIG_UNCATCH	( SIG_BIT(TS_SIGKILL) | SIG_BIT(TS_SIGSTOP) )
#define SIG_STOPS	( SIG_BIT(TS_SIGSTOP) | SIG_BIT(TS_SIGTSTP) | SIG_BIT(TS_SIGTTIN) | SIG_BIT(TS_SIGTTOU) )
#define SIG_DFL_IGN	( SIG_BIT(TS_SIGCHLD) | SIG_BIT(TS_SIGURG) | SIG_BIT(TS_SIGWINCH) \
			| SIG_BIT(TS_SIGINFO) | SIG_BIT(TS_SIGCONT) | SIG_BIT(TS_SIGIO) )
#define SIG_ALL		( ( SIG_BIT(TS_NSIG) - 1 ) & ~1ULL )

/* What a signal sent does at once, done once the lock is let go */
#define ACT_NONE	0
#define ACT_TERM	1
#define ACT_STOP	2
#define ACT_CONT	3
#define ACT_WAKE	4		/* caught: the waits of its calls are to look */

#define PSX_ALARM_PRI	12

LOCAL ID	psx_alarm_tid = 0;

/*
 * The child's group, session, terminal and signals, from its parent's
 * and what the spawn says, and the descriptors it is given. Under the
 * lock, before the child's main task starts.
 */
LOCAL ER psx_child( T_PCB *p, T_PCB *parent, CONST T_SPAWN *sp )
{
	T_PCB	*g;
	QUEUE	*q;
	BOOL	found;
	ER	er;

	if ( parent != NULL ) {
		p->pgid = parent->pgid;
		p->sid = parent->sid;
		p->ctty = parent->ctty;
		p->sig_ign = parent->sig_ign;
		p->sig_mask = parent->sig_mask;
	} else {
		p->pgid = p->sid = p->pid;	/* one the kernel starts leads a session of its own */
		p->ctty = -1;
	}
	p->sig_catch = 0;
	p->sig_entry = 0;
	p->sig_pend = 0;
	if ( sp == NULL ) {
		return E_OK;
	}
	if ( ( sp->flags & TS_SPN_SETSID ) != 0 ) {
		p->sid = p->pgid = p->pid;
		p->ctty = -1;
	} else if ( ( sp->flags & TS_SPN_PGID ) != 0 ) {
		if ( sp->pgid == 0 || sp->pgid == p->pid ) {
			p->pgid = p->pid;
		} else {
			found = FALSE;
			FOR_EACH_PCB(g, q) {
				if ( g->used && g->pgid == sp->pgid && g->sid == p->sid && g->state == PS_RUNNING ) {
					found = TRUE;
					break;
				}
			}
			if ( !found ) {
				return E_PAR;
			}
			p->pgid = sp->pgid;
		}
	}
	p->sig_ign |= sp->sigign & ~SIG_UNCATCH;
	if ( ( sp->flags & TS_SPN_SIGDEF ) != 0 ) {
		p->sig_ign &= ~sp->sigdef;
	}
	p->sig_mask = sp->sigmask & ~SIG_UNCATCH;
	if ( sp->nfd > 0 ) {
		if ( sp->nfd > TS_SPN_FD_MAX || sp->fds == NULL ) {
			return E_PAR;
		}
		p->inh = (INT *)Kmalloc(sizeof(INT) * (SZ)sp->nfd);
		if ( p->inh == NULL ) {
			return E_NOMEM;
		}
		er = knl_fs_give(sp->fds, sp->nfd, ( parent != NULL ) ? parent->pid : 0, p->pid, p->inh);
		if ( er < EX_OK ) {
			Kfree(p->inh);
			p->inh = NULL;
			return ( er == EX_MFILE ) ? E_LIMIT : E_PAR;
		}
		p->ninh = sp->nfd;
	}
	return E_OK;
}

EXPORT ID ts_spn_prc( CONST char *path, CONST T_SPAWN *sp )
{
	T_CPRC	cprc;
	TS_UUID	uuid;
	ER	er;

	if ( path == NULL || sp == NULL ) {
		return E_PAR;
	}
	if ( ts_get_pid() <= 0 && knl_objfs_lookup(path, &uuid) < EX_OK ) {
		PRCIMG	img;

		/* the kernel's own (the tests): a program file of the file layer */
		knl_memset(&cprc, 0, sizeof(cprc));
		cprc.pri = sp->pri;
		cprc.stksz = sp->stksz;
		cprc.arg = sp->arg;
		cprc.argsz = sp->argsz;
		img.path = path;
		img.vol = 0;
		img.uuid = NULL;
		img.recno = 0;
		return cre_prc(&img, &cprc, sp);
	}
	er = knl_objfs_lookup(path, &uuid);
	if ( er < EX_OK ) {
		return ( er == EX_NOENT || er == EX_NOTDIR ) ? E_NOEXS
		     : ( er == EX_ACCES ) ? E_OACV
		     : ( er == EX_ISDIR ) ? E_OBJ : E_PAR;
	}
	return ts_spn_obj(&uuid, sp);
}

EXPORT ID ts_spn_obj( CONST TS_UUID *prog, CONST T_SPAWN *sp )
{
	T_CPRC	cprc;
	TS_UUID	uuid;
	ID	vol, me;
	PRI	base = 0;

	if ( prog == NULL || sp == NULL ) {
		return E_PAR;
	}
	uuid = *prog;
	if ( ( knl_ob_permit_on(&uuid, knl_ob_crd_of(ts_get_pid())) & OB_OP_EXEC ) == 0 ) {
		knl_ob_deny_note("run", &uuid, OB_OP_EXEC, 0);
		return E_OACV;
	}
	vol = knl_obfile_vol_of(&uuid);
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	me = knl_prc_self(NULL, &base);
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.pri = ( sp->pri > 0 ) ? sp->pri : ( me > 0 ) ? base : 0;
	cprc.stksz = sp->stksz;
	cprc.arg = sp->arg;
	cprc.argsz = sp->argsz;
	return cre_prc_obj(vol, &uuid, &cprc, sp);
}

EXPORT INT ts_get_inh( INT *fds, INT max )
{
	T_PCB	*p;
	INT	n = 0, i;

	prc_lock();
	p = cur_pcb();
	if ( p != NULL && p->inh != NULL ) {
		n = p->ninh;
		for ( i = 0; i < n && i < max && fds != NULL; i++ ) {
			fds[i] = p->inh[i];
		}
	}
	tk_unl_mtx(prc_mtxid);
	return n;
}

/* The status ts_wai_psx gives for an exit code */
LOCAL INT exit_status( INT code )
{
	if ( TS_ABORT_IS_SIG(code) ) return TS_ABORT_SIGNO(code) & 0x7F;
	if ( code == TS_ABORT_TERM ) return TS_SIGKILL;
	if ( code == TS_ABORT_FAULT ) return TS_SIGSEGV;
	if ( code == TS_ABORT_ILL ) return TS_SIGILL;
	return ( code & 0xFF ) << 8;
}

/* p's parent told that p changed: its waiters look again, and it is sent SIGCHLD. Under the lock */
LOCAL ID tell_parent( T_PCB *p )
{
	T_PCB	*pp = pid_to_pcb(p->ppid);

	if ( pp != NULL && pp->used && pp->flgid > 0 ) {
		tk_set_flg(pp->flgid, PRC_FLG_CHILD);
		return pp->pid;
	}
	return 0;
}

/*
 * A signal come to p: what it does now, by p's dispositions. A stop
 * cancels a SIGCONT not yet handed over, and SIGCONT a stop. Under the
 * lock.
 */
LOCAL INT post( T_PCB *p, INT sig, ID from )
{
	UD	bit = SIG_BIT(sig);
	INT	act = ACT_NONE;

	if ( sig <= 0 || sig >= TS_NSIG || !p->used || p->ending ) {
		return ACT_NONE;
	}
	if ( sig == TS_SIGKILL ) {
		return ACT_TERM;
	}
	if ( sig == TS_SIGCONT ) {
		p->sig_pend &= ~SIG_STOPS;
		if ( p->stopped || p->stopping ) act = ACT_CONT;
	} else if ( ( bit & SIG_STOPS ) != 0 ) {
		p->sig_pend &= ~SIG_BIT(TS_SIGCONT);
	}
	if ( sig == TS_SIGSTOP ) {
		return ACT_STOP;
	}
	if ( ( p->sig_ign & bit ) != 0 ) {
		return act;
	}
	if ( ( p->sig_catch & bit ) != 0 && p->sig_entry != 0 ) {
		p->sig_pend |= bit;
		p->sig_from[sig] = from;
		if ( ( p->sig_mask & bit ) == 0 && p->flgid > 0 ) tk_set_flg(p->flgid, PRC_FLG_SIG);
		return ( act == ACT_CONT ) ? ACT_CONT : ACT_WAKE;
	}
	if ( ( p->sig_mask & bit ) != 0 ) {
		p->sig_pend |= bit;		/* its default, once it is let through */
		p->sig_from[sig] = from;
		return act;
	}
	if ( ( bit & SIG_DFL_IGN ) != 0 ) {
		return act;
	}
	if ( ( bit & SIG_STOPS ) != 0 ) {
		p->stopsig = sig;
		return ACT_STOP;
	}
	return ACT_TERM;
}

/* The tasks of p stopped, those in no call at once and the rest as they come out of theirs */
LOCAL void stop_tasks( T_PCB *p )
{
	ID	tids[TS_PRC_TSK_MAX], self = tk_get_tid();
	INT	n, i;

	n = knl_own_list(TK_OWN_TSK, p->pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		TCB	*t = knl_tsk_tcb(tids[i]);

		if ( tids[i] == self || t == NULL || t->svcdepth > 0 || t->svcin > 0 ) {
			continue;
		}
		if ( tk_sus_tsk(tids[i]) >= E_OK && ( t->svcdepth > 0 || t->svcin > 0 ) ) {
			(void)tk_rsm_tsk(tids[i]);	/* it had gone into a call: it stops coming out */
		}
	}
}

LOCAL void cont_tasks( T_PCB *p )
{
	ID	tids[TS_PRC_TSK_MAX];
	T_RTSK	r;
	INT	n, i;

	n = knl_own_list(TK_OWN_TSK, p->pid, tids, TS_PRC_TSK_MAX);
	for ( i = 0; i < n && i < TS_PRC_TSK_MAX; i++ ) {
		if ( tk_ref_tsk(tids[i], &r) >= E_OK && ( r.tskstat & TTS_SUS ) != 0 ) {
			(void)tk_frsm_tsk(tids[i]);
		}
	}
}

/* What post said, done; the caller holds no lock */
LOCAL void act_on( ID pid, INT act, INT sig )
{
	T_PCB	*p;
	ID	parent = 0;

	switch ( act ) {
	  case ACT_TERM:
		prc_lock();
		p = pid_to_pcb(pid);
		if ( p != NULL && ( p->stopped || p->stopping ) ) {
			p->stopped = p->stopping = FALSE;
			cont_tasks(p);
		}
		tk_unl_mtx(prc_mtxid);
		(void)ts_ter_prc(pid, TS_ABORT_SIG(sig));
		break;
	  case ACT_STOP:
		prc_lock();
		p = pid_to_pcb(pid);
		if ( p != NULL && p->used && !p->ending && !p->stopped ) {
			p->stopping = TRUE;
			p->stopped = TRUE;
			p->stopsig = sig;
			p->stop_new = TRUE;
			p->cont_new = FALSE;
			stop_tasks(p);
			parent = tell_parent(p);
		}
		tk_unl_mtx(prc_mtxid);
		if ( parent > 0 ) (void)knl_psx_kill(parent, TS_SIGCHLD);
		break;
	  case ACT_CONT:
		prc_lock();
		p = pid_to_pcb(pid);
		if ( p != NULL && p->used && ( p->stopped || p->stopping ) ) {
			p->stopping = FALSE;
			p->stopped = FALSE;
			p->stop_new = FALSE;
			p->cont_new = TRUE;
			cont_tasks(p);
			parent = tell_parent(p);
		}
		tk_unl_mtx(prc_mtxid);
		knl_fs_wake();
		if ( parent > 0 ) (void)knl_psx_kill(parent, TS_SIGCHLD);
		break;
	  case ACT_WAKE:
		knl_fs_wake();
		break;
	  default:
		break;
	}
}

LOCAL ER kill_one( ID pid, INT sig, ID from )
{
	T_PCB	*p;
	INT	act;

	prc_lock();
	p = pid_to_pcb(pid);
	if ( p == NULL || !p->used || p->state == PS_ZOMBIE ) {
		tk_unl_mtx(prc_mtxid);
		return E_NOEXS;
	}
	act = ( sig == 0 ) ? ACT_NONE : post(p, sig, from);
	tk_unl_mtx(prc_mtxid);
	act_on(pid, act, sig);
	return E_OK;
}

EXPORT ER knl_psx_kill( ID pid, INT sig )
{
	if ( pid <= 0 || prc_mtxid <= 0 ) {
		return E_NOEXS;
	}
	return kill_one(pid, sig, 0);
}

/* Each process of a group, as allowed (may NULL: all) */
LOCAL ER kill_group( ID pgid, INT sig, ID from, BOOL (*may)( ID pid ) )
{
	ID	*pids;
	T_PCB	*p;
	QUEUE	*q;
	INT	n = 0, i, sent = 0;
	BOOL	denied = FALSE;

	pids = (ID *)Kmalloc(sizeof(ID) * 1024);
	if ( pids == NULL ) {
		return E_NOMEM;
	}
	prc_lock();
	FOR_EACH_PCB(p, q) {
		if ( p->used && p->pgid == pgid && p->state == PS_RUNNING && n < 1024 ) {
			pids[n++] = p->pid;
		}
	}
	tk_unl_mtx(prc_mtxid);
	for ( i = 0; i < n; i++ ) {
		if ( may != NULL && !may(pids[i]) ) {
			denied = TRUE;
			continue;
		}
		if ( kill_one(pids[i], sig, from) >= E_OK ) sent++;
	}
	Kfree(pids);
	return ( sent > 0 ) ? E_OK : denied ? E_OACV : E_NOEXS;
}

EXPORT ER knl_psx_killpg( ID pgid, INT sig )
{
	if ( pgid <= 0 || prc_mtxid <= 0 ) {
		return E_NOEXS;
	}
	return kill_group(pgid, sig, 0, NULL);
}

/* A process may signal its children, those that act for the same user, and as an administrator any */
LOCAL BOOL may_signal( ID pid )
{
	ID	me = ts_get_pid();

	if ( me <= 0 || pid == me ) {
		return TRUE;
	}
	return ( knl_obprc_may(pid, OB_OP_DELETE, OBP_PARENT) >= E_OK );
}

EXPORT ER ts_kil_prc( ID pid, INT sig )
{
	ID	me = ts_get_pid(), pg;

	if ( sig < 0 || sig >= TS_NSIG ) {
		return E_PAR;
	}
	if ( pid > 0 ) {
		if ( !may_signal(pid) ) return E_OACV;
		return kill_one(pid, sig, me);
	}
	if ( pid == 0 ) {
		pg = knl_psx_pgid();
		return ( pg > 0 ) ? kill_group(pg, sig, me, may_signal) : E_NOEXS;
	}
	if ( pid < -1 ) {
		return kill_group(-pid, sig, me, may_signal);
	}
	return E_PAR;				/* -1, every process: not taken */
}

/* Signals let through that now do what they do by default (a mask taken away). Under the lock */
LOCAL INT unblocked( T_PCB *p, INT *p_sig )
{
	UD	now = p->sig_pend & ~p->sig_mask & ~p->sig_catch;
	INT	sig;

	for ( sig = 1; sig < TS_NSIG; sig++ ) {
		if ( ( now & SIG_BIT(sig) ) == 0 ) continue;
		p->sig_pend &= ~SIG_BIT(sig);
		if ( ( p->sig_ign & SIG_BIT(sig) ) != 0 || ( SIG_BIT(sig) & SIG_DFL_IGN ) != 0 ) {
			continue;
		}
		*p_sig = sig;
		return ( ( SIG_BIT(sig) & SIG_STOPS ) != 0 ) ? ACT_STOP : ACT_TERM;
	}
	return ACT_NONE;
}

EXPORT ER ts_sig_set( UINT what, CONST T_TSSIG *in, T_TSSIG *old )
{
	T_PCB	*p;
	INT	act = ACT_NONE, sig = 0;
	ID	pid;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;
	}
	if ( old != NULL ) {
		old->catch_ = p->sig_catch;
		old->ign = p->sig_ign;
		old->mask = p->sig_mask;
		old->entry = p->sig_entry;
		old->pend = p->sig_pend;
	}
	if ( in != NULL ) {
		if ( ( what & TS_SS_ENTRY ) != 0 ) {
			UD	va = in->entry;

			if ( va != 0 && ( va < USER_TEXT_VA || va >= USER_STACK_TOP || ( va & 3 ) != 0 ) ) {
				tk_unl_mtx(prc_mtxid);
				return E_PAR;
			}
			p->sig_entry = va;
		}
		if ( ( what & TS_SS_CATCH ) != 0 ) p->sig_catch = in->catch_ & SIG_ALL & ~SIG_UNCATCH;
		if ( ( what & TS_SS_IGN ) != 0 ) {
			p->sig_ign = in->ign & SIG_ALL & ~SIG_UNCATCH;
			p->sig_pend &= ~p->sig_ign;
		}
		if ( ( what & TS_SS_MASK ) != 0 ) p->sig_mask = in->mask;
		if ( ( what & TS_SS_BLOCK ) != 0 ) p->sig_mask |= in->mask;
		if ( ( what & TS_SS_UNBLOCK ) != 0 ) p->sig_mask &= ~in->mask;
		p->sig_mask &= SIG_ALL & ~SIG_UNCATCH;
		if ( ( p->sig_pend & p->sig_catch & ~p->sig_mask ) != 0 && p->flgid > 0 ) {
			tk_set_flg(p->flgid, PRC_FLG_SIG);
		}
		act = unblocked(p, &sig);
	}
	pid = p->pid;
	tk_unl_mtx(prc_mtxid);
	act_on(pid, act, sig);
	return E_OK;
}

EXPORT BOOL knl_psx_intr( void )
{
	T_PCB	*p;
	TCB	*me = knl_self_ctxtsk();
	BOOL	yes = FALSE;

	if ( prc_mtxid <= 0 || me == NULL || me->owner <= 0 ) {
		return FALSE;
	}
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		yes = (BOOL)( p->ending || ( p->sig_pend & p->sig_catch & ~p->sig_mask ) != 0 );
	}
	tk_unl_mtx(prc_mtxid);
	return yes;
}

/* Wait on the caller's flags for a signal to be handed over, up to ms; TRUE when one came or it ends */
LOCAL BOOL sig_wait( T_PCB *p, ID flgid, TMO ms )
{
	UINT	ptn;

	(void)p;
	(void)tk_wai_flg(flgid, PRC_FLG_SIG, TWF_ORW | TWF_BITCLR, &ptn, ms);
	return knl_psx_intr();
}

EXPORT ER ts_sig_sus( UD mask )
{
	T_PCB	*p;
	ID	flgid;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;
	}
	p->sus_mask = p->sig_mask;
	p->sus_restore = TRUE;
	p->sig_mask = mask & SIG_ALL & ~SIG_UNCATCH;
	flgid = p->flgid;
	tk_unl_mtx(prc_mtxid);

	while ( !sig_wait(p, flgid, 100) ) ;

	prc_lock();
	if ( p->ending ) {
		p->sig_mask = p->sus_mask;
		p->sus_restore = FALSE;
	}
	tk_unl_mtx(prc_mtxid);
	return E_RLWAI;
}

EXPORT ER ts_slp_psx( UD ns, UD *p_left )
{
	T_PCB	*p;
	ID	flgid;
	UD	start = 0, now = 0, left;

	prc_lock();
	p = cur_pcb();
	flgid = ( p != NULL ) ? p->flgid : 0;
	tk_unl_mtx(prc_mtxid);
	if ( p == NULL ) {
		if ( ns >= 1000 ) tk_dly_tsk_u((RELTIM_U)( ns / 1000 ));
		return E_OK;
	}
	(void)ts_get_mono(&start);
	for ( ;; ) {
		(void)ts_get_mono(&now);
		left = ( now - start < ns ) ? ns - ( now - start ) : 0;
		if ( knl_psx_intr() ) {
			if ( p_left != NULL ) *p_left = left;
			return E_RLWAI;
		}
		if ( left == 0 ) break;
		if ( left < 1000000ULL ) {
			tk_dly_tsk_u((RELTIM_U)( left / 1000 + 1 ));
			continue;
		}
		(void)sig_wait(p, flgid, (TMO)( ( left / 1000000ULL > 100 ) ? 100 : left / 1000000ULL ));
	}
	if ( p_left != NULL ) *p_left = 0;
	return E_OK;
}

/* SIGALRM: the task that sends it when a process's time comes */
LOCAL void psx_alarm( INT stacd, void *exinf )
{
	T_PCB	*p;
	QUEUE	*q;
	UD	now, next;
	ID	due[16];
	INT	n, i;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		now = 0;
		(void)ts_get_mono(&now);
		next = 0;
		n = 0;
		prc_lock();
		FOR_EACH_PCB(p, q) {
			if ( !p->used || p->alarm_at == 0 ) continue;
			if ( p->alarm_at <= now ) {
				if ( n < 16 ) {
					due[n++] = p->pid;
					p->alarm_at = 0;
				}
			} else if ( next == 0 || p->alarm_at < next ) {
				next = p->alarm_at;
			}
		}
		tk_unl_mtx(prc_mtxid);
		for ( i = 0; i < n; i++ ) {
			(void)knl_psx_kill(due[i], TS_SIGALRM);
		}
		if ( n > 0 ) continue;
		(void)tk_slp_tsk( ( next == 0 ) ? TMO_FEVR : (TMO)( ( next - now ) / 1000000ULL + 1 ) );
	}
}

EXPORT INT ts_sig_alm( UINT ms )
{
	T_PCB	*p;
	UD	now = 0;
	INT	left = 0;

	(void)ts_get_mono(&now);
	prc_lock();
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;
	}
	if ( p->alarm_at > now ) {
		left = (INT)( ( p->alarm_at - now + 999999ULL ) / 1000000ULL );
	}
	p->alarm_at = ( ms > 0 ) ? now + (UD)ms * 1000000ULL : 0;
	tk_unl_mtx(prc_mtxid);
	if ( psx_alarm_tid > 0 ) {
		(void)tk_wup_tsk(psx_alarm_tid);
	}
	return left;
}

/* ---------------------------------------------------------------- handing a signal over */

EXPORT BOOL knl_psx_deliver( void *frame, UD ret )
{
	UD		*f = (UD *)frame;
	T_PCB		*p;
	T_TSSIGFR	*c;
	UD		deliv, entry;
	UBINT		sp;
	ID		pid;
	INT		sig;

	p = cur_pcb();
	if ( p == NULL || p->ending ) {
		return FALSE;
	}
	/* stopped: it waits here, out of every call, until it goes on */
	while ( p->stopping && !p->ending ) {
		tk_dly_tsk(20);
	}
	if ( ( p->sig_pend & p->sig_catch & ~p->sig_mask ) == 0 || p->sig_entry == 0 ) {
		return FALSE;
	}
	c = (T_TSSIGFR *)Kmalloc(sizeof(T_TSSIGFR));
	if ( c == NULL ) {
		return FALSE;
	}
	knl_memset(c, 0, sizeof(*c));

	prc_lock();
	deliv = p->sig_pend & p->sig_catch & ~p->sig_mask;
	entry = p->sig_entry;
	for ( sig = 1; sig < TS_NSIG && ( deliv & SIG_BIT(sig) ) == 0; sig++ ) ;
	if ( sig >= TS_NSIG || entry == 0 || p->ending ) {
		tk_unl_mtx(prc_mtxid);
		Kfree(c);
		return FALSE;
	}
	p->sig_pend &= ~SIG_BIT(sig);
	c->oldmask = p->sus_restore ? p->sus_mask : p->sig_mask;
	p->sus_restore = FALSE;
	p->sig_mask |= SIG_BIT(sig);
	c->sig = sig;
	c->from = p->sig_from[sig];
	c->code = ( c->from > 0 ) ? TS_SI_USER : TS_SI_KERNEL;
	pid = p->pid;
	tk_unl_mtx(prc_mtxid);

	knl_uflt_from_call(&c->flt, f, ret);
	knl_uflt_save_fp(&c->flt);

	/* the picture on the task's own stack, below where it was and below its red zone */
	sp = ( (UBINT)knl_uframe_sp(f) - sizeof(*c) - 256 ) & ~(UBINT)UFLT_SP_MASK;
	if ( sp >= (UBINT)knl_uframe_sp(f) || !knl_prc_user_ok(pid, (CONST void *)sp, sizeof(*c), TRUE) ) {
		Kfree(c);
		(void)knl_psx_kill(pid, TS_SIGKILL);	/* nowhere to hand it over */
		return FALSE;
	}
	knl_ua_put((void *)sp, c, sizeof(*c));
	Kfree(c);
	knl_uframe_arg(f, 0, sp);
	knl_uframe_arg(f, 1, (UD)sig);
	knl_uframe_call(f, entry, sp);		/* the entry does not return: it calls ts_sig_ret */
	return TRUE;
}

/* ts_sig_ret from the SVC gateway, with the call's own frame */
EXPORT ER knl_psx_sig_return( void *frame, CONST T_TSSIGFR *ufr )
{
	T_PCB	*p;
	UD	mask;
	INT	act = ACT_NONE, sig = 0;
	ID	pid;
	ER	er;

	if ( !knl_prc_user_ok(ts_get_pid(), ufr, sizeof(*ufr), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(&mask, &ufr->oldmask, sizeof(mask));
	er = knl_prc_flt_return(frame, &ufr->flt);
	if ( er < E_OK ) {
		return er;
	}
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		p->sig_mask = mask & SIG_ALL & ~SIG_UNCATCH;
		act = unblocked(p, &sig);
		pid = p->pid;
	} else {
		pid = 0;
	}
	tk_unl_mtx(prc_mtxid);
	if ( pid > 0 ) act_on(pid, act, sig);
	return E_OK;
}

/* Calls only from the SVC gateway, which hands it the frame (knl_psx_sig_return) */
EXPORT ER ts_sig_ret( CONST T_TSSIGFR *fr )
{
	(void)fr;
	return E_CTX;
}

/* ---------------------------------------------------------------- groups and sessions */

EXPORT ID knl_psx_pgid( void )
{
	T_PCB	*p;
	ID	g = 0;

	if ( prc_mtxid <= 0 ) return 0;
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) g = p->pgid;
	tk_unl_mtx(prc_mtxid);
	return g;
}

EXPORT ID knl_psx_sid( void )
{
	T_PCB	*p;
	ID	s = 0;

	if ( prc_mtxid <= 0 ) return 0;
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) s = p->sid;
	tk_unl_mtx(prc_mtxid);
	return s;
}

EXPORT BOOL knl_psx_pg_in_sid( ID pgid, ID sid )
{
	T_PCB	*p;
	QUEUE	*q;
	BOOL	yes = FALSE;

	prc_lock();
	FOR_EACH_PCB(p, q) {
		if ( p->used && p->pgid == pgid && p->sid == sid && p->state == PS_RUNNING ) {
			yes = TRUE;
			break;
		}
	}
	tk_unl_mtx(prc_mtxid);
	return yes;
}

EXPORT BOOL knl_psx_sig_blocked( INT sig )
{
	T_PCB	*p;
	BOOL	yes = FALSE;

	if ( sig <= 0 || sig >= TS_NSIG ) return FALSE;
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) yes = (BOOL)( ( ( p->sig_ign | p->sig_mask ) & SIG_BIT(sig) ) != 0 );
	tk_unl_mtx(prc_mtxid);
	return yes;
}

EXPORT INT knl_psx_ctty( void )
{
	T_PCB	*p;
	INT	t = -1;

	if ( prc_mtxid <= 0 ) return -1;
	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) t = p->ctty;
	tk_unl_mtx(prc_mtxid);
	return t;
}

/* The caller's session has the terminal tty from now on: each of its processes knows */
EXPORT ER knl_psx_set_ctty( INT tty )
{
	T_PCB	*p, *c;
	QUEUE	*q;
	ER	er = E_OK;

	prc_lock();
	p = cur_pcb();
	if ( p == NULL || p->sid != p->pid ) {
		er = E_OBJ;
	} else {
		FOR_EACH_PCB(c, q) {
			if ( c->used && c->sid == p->sid ) c->ctty = tty;
		}
	}
	tk_unl_mtx(prc_mtxid);
	return er;
}

EXPORT void knl_psx_tty_gone( INT tty )
{
	T_PCB	*c;
	QUEUE	*q;

	prc_lock();
	FOR_EACH_PCB(c, q) {
		if ( c->used && c->ctty == tty ) c->ctty = -1;
	}
	tk_unl_mtx(prc_mtxid);
}

EXPORT ER ts_set_pgid( ID pid, ID pgid )
{
	T_PCB	*me, *p, *g;
	QUEUE	*q;
	ER	er = E_OK;
	BOOL	found;

	if ( pid < 0 || pgid < 0 ) {
		return E_PAR;
	}
	prc_lock();
	me = cur_pcb();
	p = ( pid == 0 ) ? me : pid_to_pcb(pid);
	if ( me == NULL || p == NULL || !p->used ) {
		er = E_NOEXS;
	} else if ( p != me && ( p->ppid != me->pid || p->sid != me->sid ) ) {
		er = E_NOEXS;			/* ESRCH: not the caller nor its child in its session */
	} else if ( p->sid == p->pid ) {
		er = E_OBJ;			/* EPERM: a session's leader stays in its group */
	} else {
		if ( pgid == 0 ) pgid = p->pid;
		found = ( pgid == p->pid );
		FOR_EACH_PCB(g, q) {
			if ( found ) break;
			if ( g->used && g->pgid == pgid && g->sid == p->sid && g->state == PS_RUNNING ) found = TRUE;
		}
		if ( !found ) {
			er = E_OBJ;
		} else {
			p->pgid = pgid;
		}
	}
	tk_unl_mtx(prc_mtxid);
	return er;
}

EXPORT ID ts_get_pgid( ID pid )
{
	T_PCB	*p;
	ID	g;

	prc_lock();
	p = ( pid == 0 ) ? cur_pcb() : pid_to_pcb(pid);
	g = ( p != NULL && p->used ) ? p->pgid : E_NOEXS;
	tk_unl_mtx(prc_mtxid);
	return g;
}

EXPORT ID ts_set_sid( void )
{
	T_PCB	*p, *g;
	QUEUE	*q;
	ID	s = E_OBJ;

	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) {
		BOOL	leads = FALSE;

		FOR_EACH_PCB(g, q) {
			if ( g != p && g->used && g->pgid == p->pid && g->state == PS_RUNNING ) leads = TRUE;
		}
		if ( !leads && p->sid != p->pid ) {
			p->sid = p->pgid = p->pid;
			p->ctty = -1;
			s = p->pid;
		} else if ( !leads && p->sid == p->pid && p->pgid == p->pid ) {
			s = E_OBJ;		/* leads its own session already */
		}
	}
	tk_unl_mtx(prc_mtxid);
	return s;
}

EXPORT ID ts_get_sid( ID pid )
{
	T_PCB	*p;
	ID	s;

	prc_lock();
	p = ( pid == 0 ) ? cur_pcb() : pid_to_pcb(pid);
	s = ( p != NULL && p->used ) ? p->sid : E_NOEXS;
	tk_unl_mtx(prc_mtxid);
	return s;
}

EXPORT ID ts_get_ppid( void )
{
	T_PCB	*p;
	ID	pp = 0;

	prc_lock();
	p = cur_pcb();
	if ( p != NULL ) pp = p->ppid;
	tk_unl_mtx(prc_mtxid);
	return pp;
}

EXPORT ER ts_ref_psx( ID pid, T_PSXREF *pk )
{
	T_PCB	*p;
	ER	er = E_OK;

	prc_lock();
	p = ( pid == 0 ) ? cur_pcb() : pid_to_pcb(pid);
	if ( p == NULL || !p->used ) {
		er = E_NOEXS;
	} else {
		pk->pid = p->pid;
		pk->ppid = p->ppid;
		pk->pgid = p->pgid;
		pk->sid = p->sid;
		pk->tty = p->ctty;
		pk->stopped = p->stopped;
		pk->catch_ = p->sig_catch;
		pk->ign = p->sig_ign;
		pk->mask = p->sig_mask;
		pk->pend = p->sig_pend;
	}
	tk_unl_mtx(prc_mtxid);
	if ( er == E_OK ) {
		T_PRCINFO pi;

		/* what ps shows besides: taken with the table's lock of their own */
		pk->state = PS_NONEXIST;
		pk->memsz = 0;
		pk->cpu_ms = 0;
		if ( knl_prc_info(pk->pid, &pi) >= E_OK ) {
			pk->state = pi.state;
			pk->memsz = pi.memsz;
			pk->cpu_ms = pi.cpu_ms;
		}
		(void)knl_prc_name(pk->pid, pk->name, (INT)sizeof(pk->name));
	}
	return er;
}

EXPORT INT ts_lst_prc( ID *pids, INT max )
{
	return knl_prc_list(pids, max);
}

/* ---------------------------------------------------------------- waiting for children */

/* Whether child c is one the wait names */
LOCAL BOOL wait_names( T_PCB *me, T_PCB *c, ID pid )
{
	if ( !c->used || c->ppid != me->pid ) return FALSE;
	if ( pid > 0 ) return ( c->pid == pid );
	if ( pid == -1 ) return TRUE;
	if ( pid == 0 ) return ( c->pgid == me->pgid );
	return ( c->pgid == -pid );
}

EXPORT ID ts_wai_psx( ID pid, UINT options, INT *p_status )
{
	T_PCB	*me, *c, *cc;
	QUEUE	*q, *q2;
	ID	flgid, got, gone;
	INT	status;
	BOOL	have;

	for ( ;; ) {
		got = 0;
		gone = 0;
		have = FALSE;
		status = 0;
		prc_lock();
		me = cur_pcb();
		if ( me == NULL ) {
			tk_unl_mtx(prc_mtxid);
			return E_OBJ;
		}
		flgid = me->flgid;
		(void)tk_clr_flg(flgid, ~(UINT)( PRC_FLG_CHILD | PRC_FLG_SIG ));
		FOR_EACH_PCB(c, q) {
			if ( !wait_names(me, c, pid) ) continue;
			have = TRUE;
			if ( c->told ) {
				status = exit_status(c->exitcd);
				got = c->pid;
				/* collected: as ts_wai_prc does */
				FOR_EACH_PCB(cc, q2) {
					if ( cc->ppid == c->pid ) cc->ppid = prc_init_pid;
				}
				gone = c->pid;
				prc_release(c);
				c->state = PS_ZOMBIE;
				prc_free(c);
				break;
			}
			if ( ( options & TS_WP_UNTRACED ) != 0 && c->stop_new ) {
				c->stop_new = FALSE;
				status = ( ( c->stopsig & 0xFF ) << 8 ) | 0x7F;
				got = c->pid;
				break;
			}
			if ( ( options & TS_WP_CONTINUED ) != 0 && c->cont_new ) {
				c->cont_new = FALSE;
				status = 0xFFFF;
				got = c->pid;
				break;
			}
		}
		tk_unl_mtx(prc_mtxid);
		if ( gone > 0 ) {
			prc_close_all(gone);
			knl_ob_prc_gone(gone);
		}
		if ( got > 0 ) {
			if ( p_status != NULL ) *p_status = status;
			return got;
		}
		if ( !have ) {
			return E_NOEXS;
		}
		if ( ( options & TS_WP_NOHANG ) != 0 ) {
			return 0;
		}
		if ( knl_psx_intr() ) {
			return E_RLWAI;
		}
		{
			UINT	ptn;

			(void)tk_wai_flg(flgid, PRC_FLG_CHILD | PRC_FLG_SIG, TWF_ORW, &ptn, 100);
		}
		if ( knl_psx_intr() ) {
			return E_RLWAI;
		}
	}
}

EXPORT ER knl_psx_init( void )
{
	T_CTSK	ctsk;

	if ( psx_alarm_tid > 0 ) {
		return E_OK;
	}
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)psx_alarm;
	ctsk.itskpri = PSX_ALARM_PRI;
	ctsk.stksz   = 4 * 1024;
	psx_alarm_tid = tk_cre_tsk(&ctsk);
	if ( psx_alarm_tid <= 0 || tk_sta_tsk(psx_alarm_tid, 0) < E_OK ) {
		psx_alarm_tid = 0;
		return E_SYS;
	}
	return E_OK;
}
