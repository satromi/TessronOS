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
#include "pfalloc.h"
#include "space.h"
#include "umem.h"

#define PRC_DEFAULT_STKSZ	(64 * 1024)
#define PRC_TSK_STKMAX		(1024 * 1024)	/* the most user stack a task of a process asks for */
#define PRC_ARG_VA		0x0000000000300000ULL	/* start-up argument page */
#define TSK_STOP_TRIES		200		/* ms to wait for a task to leave another processor */

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
	 */
	INT	incall;
	BOOL	ending;
	BOOL	finishing;

	/*
	 * The kernel met an address of the process that was not there
	 * while it worked for it (knl_prc_kfault), at kfault_va; the
	 * process ends as the call returns.
	 */
	volatile BOOL	kfault;
	UBINT	kfault_va;
	UD	kfault_esr, kfault_elr, kfault_lr;	/* the exception, and where the kernel was */

	/* the reservations and pages it asked for (design 9.8.1, umem.c) */
	T_UMEM	*umem;

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

#define PCB_OF(q)	( (T_PCB *)( (UB *)(q) - offsetof(T_PCB, link) ) )
#define FOR_EACH_PCB(p, q) \
	for ( (q) = prc_all.next; (q) != &prc_all && ( ( (p) = PCB_OF(q) ) != NULL ); (q) = (q)->next )

#define PRC_FLG_EXIT	0x0001

/* ---------------------------------------------------------------- helpers */

LOCAL T_PCB *pid_to_pcb( ID pid )
{
	return ( pid > 0 && pid < CNF_PID_MAX ) ? prc_by_pid[pid] : NULL;
}

/* The process whose space ttbr0 is */
LOCAL T_PCB *ttbr0_to_pcb( UD ttbr0 )
{
	T_PCB	*p = prc_by_asid[( ttbr0 >> 48 ) & ASID_MAX];

	return ( p != NULL && p->used && p->space.ttbr0 == ttbr0 ) ? p : NULL;
}

/*
 * The process the calling task belongs to, found through the space its
 * TTBR0 names. Kernel tasks and the system process give NULL.
 */
LOCAL T_PCB *cur_pcb( void )
{
	if ( knl_ctxtsk == NULL ) {
		return NULL;
	}
	return ttbr0_to_pcb(knl_ctxtsk->tskctxb.ttbr0);
}

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

	return ( prc_mtxid > 0 ) ? E_OK : (ER)prc_mtxid;
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
 * the windows it made, its files and its sockets -- and the objects of
 * the core it made.
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
	if ( left == 0 ) {
		knl_space_delete(&p->space);
	} else {
		tm_printf((UB*)"TessronOS: pid %d left %d task(s) behind: its space is kept\n",
			  p->pid, left);
	}
	p->ustk_base = NULL;
	p->memsz = 0;
	knl_umem_free(&p->umem);
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

LOCAL ID cre_prc( CONST PRCIMG *img, CONST T_CPRC *pk_cprc );

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
	return cre_prc(&img, pk_cprc);
}

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
LOCAL ER load_image( CONST PRCIMG *img, CONST T_CPRC *pk_cprc, T_SPACE *sp, UD *p_entry, UD *p_brk )
{
	ER	er;

	er = knl_space_create(sp);
	if ( er < E_OK ) {
		return er;
	}
	er = ( img->path != NULL ) ? knl_elf_load(img->path, sp, p_entry, p_brk)
				   : knl_elf_load_rec(img->vol, img->uuid, img->recno, sp,
						      p_entry, p_brk);
	if ( er < E_OK ) goto err;

	/* the start-up argument, as one read only page in the space */
	if ( pk_cprc->arg != NULL && pk_cprc->argsz > 0 ) {
		void	*page;

		er = knl_space_map_zero(sp, PRC_ARG_VA, PAGE_SIZE, PTE_PAGE_UDATA);
		if ( er < E_OK ) goto err;
		page = knl_space_page(sp, PRC_ARG_VA);
		if ( page == NULL ) { er = E_SYS; goto err; }
		knl_memcpy(page, pk_cprc->arg, (INT)pk_cprc->argsz);
		er = knl_space_protect(sp, PRC_ARG_VA, PAGE_SIZE, PTE_PAGE_URO);
		if ( er < E_OK ) goto err;
	}
	return E_OK;

    err:
	knl_space_delete(sp);
	return er;
}

LOCAL ID cre_prc( CONST PRCIMG *img, CONST T_CPRC *pk_cprc )
{
	CONST char *path = img->path;
	T_PCB	*p = NULL;
	T_CTSK	ctsk;
	T_CFLG	cflg;
	T_PCB	*parent;
	T_SPACE	space;
	UD	stksz, entry = 0, brk = 0;
	PRI	pri;
	ER	er;
	ID	tid, npid, nppid;
	BOOL	room;

	if ( pk_cprc == NULL ) {
		return E_PAR;
	}
	if ( prc_mtxid <= 0 ) {
		return E_OBJ;
	}
	if ( pk_cprc->arg != NULL && (UD)pk_cprc->argsz > PAGE_SIZE ) {
		return E_PAR;
	}

	/* no process could be made: not worth reading the program for */
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	room = (BOOL)( prc_count < CNF_MAX_PRC );
	tk_unl_mtx(prc_mtxid);
	if ( !room ) {
		return E_LIMIT;
	}

	knl_memset(&space, 0, sizeof(space));
	er = load_image(img, pk_cprc, &space, &entry, &brk);
	if ( er < E_OK ) {
		return er;
	}

	tk_loc_mtx(prc_mtxid, TMO_FEVR);

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

	/* the space is the process's from here, and goes with it */
	p->space = space;
	space.l0_pa = 0;
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
	/* the middle of the range when the caller does not choose */
	pri   = ( pk_cprc->pri > 0 ) ? pk_cprc->pri : (PRI)(CNF_MAX_TSKPRI / 2);
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
	 * It acts as its parent did (design 18.10), from its first
	 * instruction: the credentials are there before the task runs, or
	 * its first calls are judged as nobody's. The object layer's lock
	 * is taken under this one; the object layer never takes this one
	 * holding its own, so the order is always this way round.
	 */
	npid = p->pid;
	nppid = p->ppid;
	knl_ob_prc_start(npid, nppid);

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
    err1:
	prc_free(p);
    err0:
	tk_unl_mtx(prc_mtxid);
	knl_space_delete(&space);	/* a space not handed to a process */

	return er;
}

/*
 * Start a program held in a real object (design 9.5, 9.6). The metadata
 * says which record holds the image. On the FAT store that record is a
 * file, and the loader is given its path; on the native store the loader
 * reads the record itself.
 */
EXPORT ID ts_cre_prc_obj( ID vol, CONST TS_UUID *uuid, CONST T_CPRC *pk_cprc )
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
	pid = cre_prc(&img, pk_cprc);
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
		p->ending = TRUE;
	}
	if ( p->incall > 0 || p->finishing ) {
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	tk_set_flg(p->flgid, PRC_FLG_EXIT);
	tk_unl_mtx(prc_mtxid);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);

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
 */
EXPORT ER knl_prc_call_enter( void )
{
	T_PCB	*p;
	TCB	*t;
	ER	er = E_OK;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	t = prc_task(p);
	if ( t != NULL ) {
		if ( p->ending ) {
			er = E_DISWAI;
		} else {
			p->incall++;
			t->svcin++;
		}
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

/*
 * A call of a task of a process ends: 'entered' when it began with
 * knl_prc_call_enter. When the process is to end -- marked while the
 * task was inside, or the kernel met an address of the process that
 * was not there while it worked for it -- the task does not go back to
 * the program: it ends, and the last task of the process to come out
 * finishes the process on the way.
 */
EXPORT void knl_prc_call_leave( BOOL entered )
{
	T_PCB	*p;
	TCB	*t;
	BOOL	end = FALSE, fin = FALSE, fault = FALSE, note = FALSE;
	UBINT	va = 0;
	ID	pid = 0;
	T_PRCFLT info;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	t = prc_task(p);
	if ( t != NULL ) {
		if ( entered && t->svcin > 0 ) {
			t->svcin--;
			p->incall--;
		}
		if ( p->incall == 0 ) {
			knl_umem_flush(p->umem);	/* no call holds its pages now */
		}
		if ( p->kfault ) {
			p->kfault = FALSE;
			kfault_count(-1);
			fault = TRUE;
			va = p->kfault_va;
			pid = p->pid;
			if ( !p->ending ) {
				T_PRCFLT	f;

				knl_memset(&f, 0, sizeof(f));
				f.kind = TS_FLT_KCALL;
				f.tskid = t->tskid;
				f.esr = p->kfault_esr;
				f.far = (UD)va;
				f.elr = p->kfault_elr;
				f.lr = p->kfault_lr;
				flt_note(p, TS_ABORT_FAULT, &f, &info);
				note = TRUE;
				p->exitcd = TS_ABORT_FAULT;
				p->state  = PS_EXITING;
				p->ending = TRUE;
			}
		}
		if ( p->ending ) {
			end = TRUE;
			fin = prc_mark(p, p->exitcd);	/* marked: only who finishes it is decided */
		}
	}
	tk_unl_mtx(prc_mtxid);

	if ( fault ) {
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
		p->kfault = TRUE;
		kfault_count(1);
	}
	return TRUE;
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
	UD	e, ap, pa, first = 0, next = 0;

	if ( len <= 0 || p_kaddr == NULL || knl_ctxtsk == NULL ) {
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
		ap = ( e >> 6 ) & 3;

		/* AP: 1 read and write for all, 3 read only for all */
		if ( e == 0 || !( ap == 1 || ( ap == 3 && !into ) ) ) {
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
	T_PCB	*p;
	BOOL	yes;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	yes = ( p != NULL && p->ending );
	tk_unl_mtx(prc_mtxid);

	return yes;
}

/*
 * Called from the kernel when a task of a process faults at EL0 (design
 * 9.11), on that task, which ends when this returns. The process ends
 * with it: finished here, or by the last of its other tasks to come
 * out of a bracketed call. The fault is recorded for the desktop when
 * it is what ends the process: not for a process already ending, nor
 * for a second task of it faulting as well.
 */
EXPORT void knl_prc_abort( UD ttbr0, INT abortcd, CONST T_PRCFLT *f )
{
	T_PCB	*p;
	BOOL	fin = FALSE, note = FALSE;
	T_PRCFLT info;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = pid_to_pcb(pid);
	if ( p != NULL ) {
		prog = p->prog;
		knl_memcpy(file, p->file, TS_FLT_FILE);
	}
	tk_unl_mtx(prc_mtxid);

	return name_of(pid, &prog, file, buf, max);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
		tk_loc_mtx(prc_mtxid, TMO_FEVR);
		prc_init_pid = pid;
		tk_unl_mtx(prc_mtxid);
		tm_printf((UB*)"TessronOS: init started as pid %d from %s\n", pid, path);
	}

	return pid;
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_unl_mtx(prc_mtxid);

	er = tk_wai_flg(flgid, PRC_FLG_EXIT, TWF_ORW, &ptn, tmout);
	if ( er < E_OK ) {
		return er;
	}

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = pid_to_pcb(pid);
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
	prc_release(p);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);

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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = pid_to_pcb(pid);
	if ( p != NULL && p->space.l0_pa != 0 ) {
		er = knl_pt_unmap(p->space.l0_pa, va, size);
	}
	tk_unl_mtx(prc_mtxid);

	return er;
}

EXPORT BOOL knl_prc_user_ok( ID pid, CONST void *p, SZ len, BOOL write )
{
	T_PCB	*pc;
	UBINT	va = (UBINT)p, end, pg;
	BOOL	ok = TRUE;

	if ( p == NULL || len < 0 ) {
		return FALSE;
	}
	end = va + (UBINT)len;
	if ( end < va || end > USER_STACK_TOP || va < USER_TEXT_VA ) {
		return FALSE;			/* not in the process's half at all */
	}
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	pc = pid_to_pcb(pid);
	if ( pc == NULL || pc->space.l0_pa == 0 ) {
		ok = FALSE;
	}
	for ( pg = va & PAGE_MASK; ok && pg < end; pg += PAGE_SIZE ) {
		UD	e = knl_pt_entry(pc->space.l0_pa, pg);
		UD	ap = ( e >> 6 ) & 3;

		/* AP: 1 read and write for all, 3 read only for all */
		ok = ( e != 0 && ( ap == 1 || ( ap == 3 && !write ) ) );
	}
	tk_unl_mtx(prc_mtxid);

	return ok;
}

EXPORT UD knl_prc_lookup( ID pid, UBINT va )
{
	T_PCB	*p;
	UD	pa = ~(UD)0;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		va = (UBINT)*p_adr;
		er = knl_umem_map(&p->umem, &p->space, &va, (UD)size, (UD)align, prot,
				  (BOOL)( p->incall > 0 ));
		if ( er == E_OK ) {
			*p_adr = (void *)va;
		}
		if ( p->incall == 0 ) knl_umem_flush(p->umem);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		er = knl_umem_unmap(&p->umem, &p->space, (UBINT)adr, (UD)size, (BOOL)( p->incall > 0 ));
		if ( p->incall == 0 ) knl_umem_flush(p->umem);
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
	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	if ( p == NULL || p->space.l0_pa == 0 ) {
		er = E_NOSPT;
	} else {
		er = knl_umem_ctl(&p->umem, &p->space, (UBINT)adr, (UD)size, prot,
				  (BOOL)( p->incall > 0 ));
		if ( p->incall == 0 ) knl_umem_flush(p->umem);
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

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	if ( p == NULL ) {
		tk_unl_mtx(prc_mtxid);
		return E_OBJ;
	}
	n = p->argsz;
	page = ( n > 0 ) ? knl_space_page(&p->space, PRC_ARG_VA) : NULL;
	if ( page != NULL && buf != NULL && size > 0 ) {
		knl_memcpy(buf, page, (INT)( ( size < n ) ? size : n ));
	}
	tk_unl_mtx(prc_mtxid);

	return n;
}

EXPORT ID ts_get_pid( void )
{
	T_PCB	*p;
	ID	pid;

	tk_loc_mtx(prc_mtxid, TMO_FEVR);
	p = cur_pcb();
	pid = ( p != NULL ) ? p->pid : 0;
	tk_unl_mtx(prc_mtxid);

	return pid;
}
