/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rumpuser.c
 *	The hypercalls of the NetBSD rump kernel, on T-Kernel (design 12.6).
 *
 *	The rump kernel carrying the network stack asks its host for memory,
 *	threads, locks, clocks, random bytes and a console through the
 *	rumpuser_ calls (version 17 of the interface). Here the host is the
 *	TessronOS kernel itself:
 *
 *	memory		the page allocator for whole pages in powers of two,
 *			Kmalloc for the rest
 *	threads		T-Kernel tasks
 *	locks		a lock of its own for each of the rump kernel's
 *			mutexes, read/write locks and condition variables:
 *			a spin lock over a queue of waiters, each waiter
 *			sleeping on a semaphore taken from a pool while it
 *			waits. The rump kernel makes thousands of locks, far
 *			more than there are T-Kernel objects to spare, and
 *			holds them only briefly.
 *	clocks		the system time and the monotonic clock
 *	randomness	the kernel's random source
 *	console		T-Monitor's
 *
 *	A wait that is not the rump kernel's own gives up the virtual
 *	processor and the big lock of the thread first, and takes them back
 *	after (unsched/sched below), so that the rump kernel's other threads
 *	go on meanwhile; the _nowrap forms, which the rump kernel's scheduler
 *	itself uses, do not.
 *
 *	The rump kernel knows which of its threads a task is by the
 *	curlwp calls: a table indexed by task ID holds that, so a task that
 *	enters the rump kernel from outside (a process's system call) is a
 *	thread of it only while it is there.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/time.h>
#include <ts/uuid.h>
#include "tstdlib.h"
#include "sysman/pfalloc.h"
#include "rumpglue.h"

#define RUMPUSER_VERSION	17

#define RU_TASK_PRI		10		/* the stack's threads, as its task was */
#define RU_TASK_STKSZ		(64 * 1024)
#define RU_SPIN			200		/* tries at a held lock before sleeping */
#define RU_SEM_POOL		128		/* semaphores kept for waiters */
#define RU_TRON_UNIX_S		473385600LL	/* seconds from 1970 to 1985 */

/* NetBSD's errno values, which the rump kernel expects back */
#define NB_ENOENT		2
#define NB_ENOMEM		12
#define NB_EBUSY		16
#define NB_EINVAL		22
#define NB_ETIMEDOUT		60
#define NB_EOPNOTSUPP		45

IMPORT INT	knl_num_prc;

/* ---------------------------------------------------------------- the rump kernel's upcalls */

typedef struct {
	void	(*hyp_schedule)( void );
	void	(*hyp_unschedule)( void );
	void	(*hyp_backend_unschedule)( int, int *, void * );
	void	(*hyp_backend_schedule)( int, void * );
	void	(*hyp_lwproc_switch)( void * );
	void	(*hyp_lwproc_release)( void );
	int	(*hyp_lwproc_rfork)( void *, int, const char * );
	int	(*hyp_lwproc_newlwp)( int );
	void	*(*hyp_lwproc_curlwp)( void );
	int	(*hyp_syscall)( int, void *, long * );
	void	(*hyp_lwpexit)( void );
	void	(*hyp_execnotify)( const char * );
	int	(*hyp_getpid)( void );
	void	*hyp__extra[8];
} RU_HYPERUP;

LOCAL CONST RU_HYPERUP	*ru_hyp;

EXPORT void	rumpuser_thread_exit( void );
LOCAL void	page_start( void );

/* The thread of the rump kernel each task is while it is in there */
LOCAL void		**ru_curlwp;

/*
 * With RU_TRACE, what each task waits on in here is kept, and a task of
 * its own reports waits for a lock that go on for seconds, with the
 * place in the rump kernel that asked (a means of finding a deadlock).
 */
#ifndef RU_TRACE
#define RU_TRACE	0
#endif
#define TRK_LOCK	1
#define TRK_RW		2
#define TRK_CV		3

#if RU_TRACE
typedef struct {
	UB		kind;
	UB		told;
	void		*obj;
	void		*ra;
	UD		since;
} RU_TR;

LOCAL RU_TR		*ru_tr;
#define TR_RA()		do { if ( ru_tr != NULL ) ru_tr[tk_get_tid()].ra = __builtin_return_address(0); } while ( 0 )
LOCAL void ru_trace_start( void );
#else
#define TR_RA()		do { } while ( 0 )
#endif

/* Give up the virtual processor (and big lock) around a wait of the host's */
EXPORT void knl_rump_unsched( INT *p_nlocks )
{
	int	n = 0;

	ru_hyp->hyp_backend_unschedule(0, &n, NULL);
	*p_nlocks = (INT)n;
}

EXPORT void knl_rump_sched( INT nlocks )
{
	ru_hyp->hyp_backend_schedule((int)nlocks, NULL);
}

/* As above, with the lock being waited on named: the scheduler may hold it */
LOCAL void unsched_il( INT *p_nlocks, void *interlock )
{
	int	n = 0;

	ru_hyp->hyp_backend_unschedule(0, &n, interlock);
	*p_nlocks = (INT)n;
}

LOCAL void sched_il( INT nlocks, void *interlock )
{
	ru_hyp->hyp_backend_schedule((int)nlocks, interlock);
}

/* ---------------------------------------------------------------- init */

EXPORT int rumpuser_init( int version, CONST void *hyp )
{
	INT	order = 0;
	PFRAME	*pf;

	if ( version != RUMPUSER_VERSION ) {
		tm_printf((UB *)"rumpuser: interface version %d, not %d\n",
			  version, RUMPUSER_VERSION);
		return 1;
	}
	ru_hyp = (CONST RU_HYPERUP *)hyp;

	/* one pointer for every task ID there can be */
	while ( ( PAGE_SIZE << order ) < ( CNF_MAX_TSKID + 1 ) * sizeof(void *) ) {
		order++;
	}
	pf = knl_alloc_pages((UINT)order, ZONE_NORMAL, KAF_ZERO);
	if ( pf == NULL ) {
		return NB_ENOMEM;
	}
	ru_curlwp = (void **)knl_pf_to_va(pf);
	page_start();
#if RU_TRACE
	pf = knl_alloc_pages((UINT)order + 2, ZONE_NORMAL, KAF_ZERO);
	if ( pf != NULL ) {
		ru_tr = (RU_TR *)knl_pf_to_va(pf);
		ru_trace_start();
	}
#endif
	return 0;
}

EXPORT void rumpuser_curlwpop( int op, void *l )
{
	ID	tid = tk_get_tid();

	switch ( op ) {
	case 2:				/* RUMPUSER_LWP_SET */
		ru_curlwp[tid] = l;
		break;
	case 3:				/* RUMPUSER_LWP_CLEAR */
		ru_curlwp[tid] = NULL;
		break;
	default:			/* made and destroyed: nothing kept here */
		break;
	}
}

EXPORT void *rumpuser_curlwp( void )
{
	return ru_curlwp[tk_get_tid()];
}

/* ---------------------------------------------------------------- memory */

/*
 * Whole pages in a power of two come from the page allocator, aligned
 * to their size (or more, when more is asked); anything else from
 * Kmalloc, with the address of the block kept just before what is
 * handed out so that it can be aligned as asked. The two are told apart
 * by the length, which the rump kernel gives again when it frees; the
 * order a block of pages was taken with is in its page frame.
 *
 * Single pages -- what the rump kernel's pools grow by -- are kept once
 * taken: a page given back goes on a list of the rump kernel's own and
 * is handed out again from there. Some are taken at the start. So the
 * stack's memory comes to what it needs and stays there, and its pools
 * growing and shrinking as packets come and go do not move the count
 * of free pages the rest of the system sees.
 */
#define RU_PAGES_AT_START	1024	/* 4 MB */

LOCAL T_SPLOCK	page_lock;
LOCAL void	*page_list = NULL;	/* free single pages, linked through their first word */
LOCAL UD	page_count = 0;		/* pages the rump kernel holds, on the list or not */

LOCAL INT page_order( UD len )
{
	INT	o;

	if ( len == 0 || ( len & ( PAGE_SIZE - 1 ) ) != 0 ) {
		return -1;
	}
	for ( o = 0; o <= PF_MAX_ORDER; o++ ) {
		if ( ( (UD)PAGE_SIZE << o ) == len ) {
			return o;
		}
	}
	return -1;
}

LOCAL void *page_take( void )
{
	void	*p;
	UINT	imask;
	PFRAME	*pf;

	ISpinLock(&page_lock, &imask);
	p = page_list;
	if ( p != NULL ) {
		page_list = *(void **)p;
	}
	ISpinUnlock(&page_lock, &imask);
	if ( p != NULL ) {
		return p;
	}
	pf = knl_alloc_pages(0, ZONE_NORMAL, 0);
	if ( pf == NULL ) {
		return NULL;
	}
	ISpinLock(&page_lock, &imask);
	page_count++;
	ISpinUnlock(&page_lock, &imask);
	return knl_pf_to_va(pf);
}

LOCAL void page_give( void *p )
{
	UINT	imask;

	ISpinLock(&page_lock, &imask);
	*(void **)p = page_list;
	page_list = p;
	ISpinUnlock(&page_lock, &imask);
}

/* The pages the network stack holds (for a count of the system's memory) */
EXPORT UD knl_rump_pages( void )
{
	return page_count;
}

LOCAL void page_start( void )
{
	void	*p[64];
	INT	i, k;

	for ( i = 0; i < RU_PAGES_AT_START; i += 64 ) {
		for ( k = 0; k < 64; k++ ) {
			p[k] = page_take();
		}
		for ( k = 0; k < 64; k++ ) {
			if ( p[k] != NULL ) page_give(p[k]);
		}
	}
}

EXPORT int rumpuser_malloc( UD len, int alignment, void **memp )
{
	INT	o = page_order(len);
	UD	align = ( alignment > 16 ) ? (UD)alignment : 16;
	UB	*raw;
	UBINT	p;

	if ( o >= 0 ) {
		PFRAME	*pf;

		while ( ( (UD)PAGE_SIZE << o ) < align && o < PF_MAX_ORDER ) {
			o++;
		}
		if ( o == 0 ) {
			*memp = page_take();
			return ( *memp != NULL ) ? 0 : NB_ENOMEM;
		}
		pf = knl_alloc_pages((UINT)o, ZONE_NORMAL, 0);
		if ( pf == NULL ) {
			return NB_ENOMEM;
		}
		*memp = knl_pf_to_va(pf);
		return 0;
	}
	raw = (UB *)Kmalloc((size_t)( len + align + sizeof(void *) ));
	if ( raw == NULL ) {
		return NB_ENOMEM;
	}
	p = ( (UBINT)raw + sizeof(void *) + align - 1 ) & ~( align - 1 );
	((void **)p)[-1] = raw;
	*memp = (void *)p;
	return 0;
}

EXPORT void rumpuser_free( void *mem, UD len )
{
	PFRAME	*pf;

	if ( mem == NULL ) {
		return;
	}
	if ( page_order(len) >= 0 ) {
		pf = knl_pa_to_pf(VA2PA(mem));
		if ( pf->order == 0 ) {
			page_give(mem);
		} else {
			knl_free_pages(pf, pf->order);
		}
		return;
	}
	Kfree(((void **)mem)[-1]);
}

/* Memory for code is never asked for: modules are not loaded */
EXPORT int rumpuser_anonmmap( void *prefaddr, UD size, int alignbit, int exec, void **memp )
{
	if ( exec ) {
		return NB_EOPNOTSUPP;
	}
	return rumpuser_malloc(size, ( alignbit > 0 ) ? ( 1 << alignbit ) : 0, memp);
}

EXPORT void rumpuser_unmap( void *addr, UD len )
{
	rumpuser_free(addr, len);
}

/* ---------------------------------------------------------------- waiters */

/*
 * One task waiting on a lock or a condition. It lives on the waiter's
 * stack; whoever wakes it takes it off the queue, marks it and signals
 * its semaphore, and the waiter does not go before that signal has
 * been taken, so the entry is never used after it is gone.
 */
typedef struct ru_waiter {
	struct ru_waiter	*next;
	ID			semid;
	volatile BOOL		woken;
} RU_WAITER;

typedef struct {
	RU_WAITER	*head;
	RU_WAITER	*tail;
} RU_QUEUE;

LOCAL T_SPLOCK	sem_lock;
LOCAL ID	sem_pool[RU_SEM_POOL];
LOCAL INT	sem_free = 0;

/* A semaphore at 0 for one wait */
LOCAL ID sem_get( void )
{
	T_CSEM	csem;
	UINT	imask;
	ID	id = 0;

	ISpinLock(&sem_lock, &imask);
	if ( sem_free > 0 ) {
		id = sem_pool[--sem_free];
	}
	ISpinUnlock(&sem_lock, &imask);
	if ( id > 0 ) {
		return id;
	}
	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem  = 1;
	id = tk_cre_sem(&csem);
	if ( id <= 0 ) {
		tm_printf((UB *)"rumpuser: no semaphore for a waiter (%d)\n", id);
		for ( ;; ) tk_slp_tsk(TMO_FEVR);
	}
	return id;
}

/* Back to the pool, at 0 again: every signal to it has been taken */
LOCAL void sem_put( ID id )
{
	UINT	imask;
	BOOL	kept = FALSE;

	ISpinLock(&sem_lock, &imask);
	if ( sem_free < RU_SEM_POOL ) {
		sem_pool[sem_free++] = id;
		kept = TRUE;
	}
	ISpinUnlock(&sem_lock, &imask);
	if ( !kept ) {
		tk_del_sem(id);
	}
}

/*
 * A wait on a waiter's semaphore that only its signal, or the time
 * (tmo_us >= 0), ends: a wait released by someone else (tk_rel_wai on
 * a process's task) is taken up again, since the signal it is for has
 * not come.
 */
LOCAL ER sem_wait_( ID semid, D tmo_us )
{
	UD	now = 0, until = 0;
	ER	er;

	if ( tmo_us >= 0 ) {
		(void)ts_get_mono(&now);
		until = now + (UD)tmo_us * 1000;
	}
	for ( ;; ) {
		if ( tmo_us < 0 ) {
			er = tk_wai_sem(semid, 1, TMO_FEVR);
		} else {
			(void)ts_get_mono(&now);
			if ( now >= until ) {
				return E_TMOUT;
			}
			er = tk_wai_sem_u(semid, 1, (TMO_U)( ( until - now + 999 ) / 1000 ));
		}
		if ( er == E_OK || er == E_TMOUT ) {
			return er;
		}
	}
}

LOCAL ER sem_wait( ID semid, D tmo_us, INT kind, void *obj )
{
#if RU_TRACE
	RU_TR	*t = ( ru_tr != NULL ) ? &ru_tr[tk_get_tid()] : NULL;
	ER	er;

	if ( t != NULL ) {
		t->obj = obj;
		t->told = 0;
		(void)ts_get_mono(&t->since);
		t->kind = (UB)kind;
	}
	er = sem_wait_(semid, tmo_us);
	if ( t != NULL ) {
		t->kind = 0;
	}
	return er;
#else
	return sem_wait_(semid, tmo_us);
#endif
}

LOCAL void q_add( RU_QUEUE *q, RU_WAITER *w )
{
	w->next = NULL;
	if ( q->tail != NULL ) {
		q->tail->next = w;
	} else {
		q->head = w;
	}
	q->tail = w;
}

LOCAL RU_WAITER *q_take( RU_QUEUE *q )
{
	RU_WAITER	*w = q->head;

	if ( w != NULL ) {
		q->head = w->next;
		if ( q->head == NULL ) {
			q->tail = NULL;
		}
	}
	return w;
}

LOCAL BOOL q_remove( RU_QUEUE *q, RU_WAITER *w )
{
	RU_WAITER	*p, *prev = NULL;

	for ( p = q->head; p != NULL; prev = p, p = p->next ) {
		if ( p == w ) {
			if ( prev != NULL ) {
				prev->next = p->next;
			} else {
				q->head = p->next;
			}
			if ( q->tail == p ) {
				q->tail = prev;
			}
			return TRUE;
		}
	}
	return FALSE;
}

/* Mark the waiter and answer its semaphore, to be signalled once the spin lock is let go */
LOCAL ID q_wake( RU_WAITER *w )
{
	ID	id = w->semid;

	w->woken = TRUE;
	return id;
}

/* ---------------------------------------------------------------- a sleeping lock */

typedef struct {
	T_SPLOCK	sl;
	volatile BOOL	held;
	RU_QUEUE	q;
} RU_LOCK;

LOCAL void lk_init( RU_LOCK *l )
{
	InitSpinLock(&l->sl);
	l->held = FALSE;
	l->q.head = l->q.tail = NULL;
}

LOCAL BOOL lk_try( RU_LOCK *l )
{
	UINT	imask;
	BOOL	got = FALSE;

	ISpinLock(&l->sl, &imask);
	if ( !l->held ) {
		l->held = TRUE;
		got = TRUE;
	}
	ISpinUnlock(&l->sl, &imask);
	return got;
}

/*
 * A holder on another processor usually lets go at once, so the lock is
 * tried a while before sleeping. Letting go wakes the first sleeper,
 * which then tries for the lock again like anyone else: the lock is
 * never held by a task that is not running, however low its priority.
 */
LOCAL void lk_lock( RU_LOCK *l )
{
	RU_WAITER	w;
	UINT		imask;
	INT		i;

	for ( ;; ) {
		for ( i = 0; i < RU_SPIN; i++ ) {
			if ( !l->held && lk_try(l) ) {
				return;
			}
			Asm("yield");
		}
		w.semid = sem_get();
		w.woken = FALSE;
		ISpinLock(&l->sl, &imask);
		if ( !l->held ) {
			l->held = TRUE;
			ISpinUnlock(&l->sl, &imask);
			sem_put(w.semid);
			return;
		}
		q_add(&l->q, &w);
		ISpinUnlock(&l->sl, &imask);
		(void)sem_wait(w.semid, -1, TRK_LOCK, l);
		sem_put(w.semid);
	}
}

LOCAL void lk_unlock( RU_LOCK *l )
{
	RU_WAITER	*w;
	UINT		imask;
	ID		sem = 0;

	ISpinLock(&l->sl, &imask);
	l->held = FALSE;
	w = q_take(&l->q);
	if ( w != NULL ) {
		sem = q_wake(w);
	}
	ISpinUnlock(&l->sl, &imask);
	if ( sem > 0 ) {
		tk_sig_sem(sem, 1);
	}
}

/* ---------------------------------------------------------------- mutexes */

#define RUMPUSER_MTX_SPIN	0x01
#define RUMPUSER_MTX_KMUTEX	0x02

typedef struct rumpuser_mtx {
	RU_LOCK		lk;
	INT		flags;
	void		*owner;		/* the rump kernel's thread, for a kernel mutex */
} RU_MTX;

LOCAL void mtx_owned( RU_MTX *m )
{
	if ( ( m->flags & RUMPUSER_MTX_KMUTEX ) != 0 ) {
		m->owner = rumpuser_curlwp();
	}
}

EXPORT void rumpuser_mutex_init( RU_MTX **mp, int flags )
{
	RU_MTX	*m = (RU_MTX *)Kmalloc(sizeof(RU_MTX));

	if ( m == NULL ) {
		tm_printf((UB *)"rumpuser: no memory for a mutex\n");
		for ( ;; ) tk_slp_tsk(TMO_FEVR);
	}
	lk_init(&m->lk);
	m->flags = flags;
	m->owner = NULL;
	*mp = m;
}

EXPORT void rumpuser_mutex_enter_nowrap( RU_MTX *m )
{
	TR_RA();
	lk_lock(&m->lk);
	mtx_owned(m);
}

EXPORT void rumpuser_mutex_enter( RU_MTX *m )
{
	INT	n;

	TR_RA();
	if ( ( m->flags & RUMPUSER_MTX_SPIN ) != 0 ) {
		rumpuser_mutex_enter_nowrap(m);
		return;
	}
	if ( !lk_try(&m->lk) ) {
		knl_rump_unsched(&n);
		lk_lock(&m->lk);
		knl_rump_sched(n);
	}
	mtx_owned(m);
}

EXPORT int rumpuser_mutex_tryenter( RU_MTX *m )
{
	if ( !lk_try(&m->lk) ) {
		return NB_EBUSY;
	}
	mtx_owned(m);
	return 0;
}

EXPORT void rumpuser_mutex_exit( RU_MTX *m )
{
	m->owner = NULL;
	lk_unlock(&m->lk);
}

EXPORT void rumpuser_mutex_destroy( RU_MTX *m )
{
	Kfree(m);
}

EXPORT void rumpuser_mutex_owner( RU_MTX *m, void **lp )
{
	*lp = m->owner;
}

EXPORT int rumpuser_mutex_spin_p( RU_MTX *m )
{
	return ( m->flags & RUMPUSER_MTX_SPIN ) != 0;
}

/* ---------------------------------------------------------------- read/write locks */

#define RUMPUSER_RW_READER	0
#define RUMPUSER_RW_WRITER	1

/*
 * Readers share it, a writer has it alone. A reader coming while a
 * writer waits waits too, so that writers are not starved. Whenever the
 * lock comes free, everyone waiting is woken and tries for it again: a
 * writer that wins keeps the others waiting, readers that win share it.
 */
typedef struct rumpuser_rw {
	T_SPLOCK	sl;
	INT		readers;
	BOOL		wheld;
	void		*writer;	/* the rump kernel's thread holding it to write */
	RU_QUEUE	rq;
	RU_QUEUE	wq;
} RU_RW;

EXPORT void rumpuser_rw_init( RU_RW **rwp )
{
	RU_RW	*rw = (RU_RW *)Kcalloc(1, sizeof(RU_RW));

	if ( rw == NULL ) {
		tm_printf((UB *)"rumpuser: no memory for a lock\n");
		for ( ;; ) tk_slp_tsk(TMO_FEVR);
	}
	InitSpinLock(&rw->sl);
	*rwp = rw;
}

/* Take it at once if that can be done; under the spin lock */
LOCAL BOOL rw_take( RU_RW *rw, int lk )
{
	if ( lk == RUMPUSER_RW_WRITER ) {
		if ( !rw->wheld && rw->readers == 0 ) {
			rw->wheld = TRUE;
			return TRUE;
		}
	} else {
		if ( !rw->wheld && rw->wq.head == NULL ) {
			rw->readers++;
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT int rumpuser_rw_tryenter( int lk, RU_RW *rw )
{
	UINT	imask;
	BOOL	got;

	ISpinLock(&rw->sl, &imask);
	got = rw_take(rw, lk);
	ISpinUnlock(&rw->sl, &imask);
	if ( !got ) {
		return NB_EBUSY;
	}
	if ( lk == RUMPUSER_RW_WRITER ) {
		rw->writer = rumpuser_curlwp();
	}
	return 0;
}

EXPORT void rumpuser_rw_enter( int lk, RU_RW *rw )
{
	RU_WAITER	w;
	UINT		imask;
	BOOL		got;
	INT		n;

	TR_RA();
	if ( rumpuser_rw_tryenter(lk, rw) == 0 ) {
		return;
	}
	knl_rump_unsched(&n);
	for ( ;; ) {
		w.semid = sem_get();
		w.woken = FALSE;
		ISpinLock(&rw->sl, &imask);
		got = rw_take(rw, lk);
		if ( !got ) {
			q_add(( lk == RUMPUSER_RW_WRITER ) ? &rw->wq : &rw->rq, &w);
		}
		ISpinUnlock(&rw->sl, &imask);
		if ( !got ) {
			(void)sem_wait(w.semid, -1, TRK_RW, rw);
		}
		sem_put(w.semid);
		if ( got ) {
			break;
		}
	}
	knl_rump_sched(n);
	if ( lk == RUMPUSER_RW_WRITER ) {
		rw->writer = rumpuser_curlwp();
	}
}

/*
 * Under the spin lock: when the lock is free, every waiter is taken off
 * its queue and marked; the list of them is answered, to be signalled
 * once the spin lock is let go.
 */
LOCAL RU_WAITER *rw_free( RU_RW *rw )
{
	RU_WAITER	*list = NULL, *w;

	if ( rw->wheld || rw->readers > 0 ) {
		return NULL;
	}
	while ( ( w = q_take(&rw->wq) ) != NULL ) {
		w->woken = TRUE;
		w->next = list;
		list = w;
	}
	while ( ( w = q_take(&rw->rq) ) != NULL ) {
		w->woken = TRUE;
		w->next = list;
		list = w;
	}
	return list;
}

/* Each entry is read before its owner is let go */
LOCAL void rw_signal( RU_WAITER *w )
{
	RU_WAITER	*next;
	ID		sem;

	while ( w != NULL ) {
		next = w->next;
		sem = w->semid;
		tk_sig_sem(sem, 1);
		w = next;
	}
}

EXPORT void rumpuser_rw_exit( RU_RW *rw )
{
	RU_WAITER	*list;
	UINT		imask;

	ISpinLock(&rw->sl, &imask);
	if ( rw->wheld ) {
		rw->wheld = FALSE;
		rw->writer = NULL;
	} else if ( rw->readers > 0 ) {
		rw->readers--;
	}
	list = rw_free(rw);
	ISpinUnlock(&rw->sl, &imask);
	rw_signal(list);
}

EXPORT int rumpuser_rw_tryupgrade( RU_RW *rw )
{
	UINT	imask;
	BOOL	got = FALSE;

	ISpinLock(&rw->sl, &imask);
	if ( !rw->wheld && rw->readers == 1 ) {
		rw->readers = 0;
		rw->wheld = TRUE;
		got = TRUE;
	}
	ISpinUnlock(&rw->sl, &imask);
	if ( !got ) {
		return NB_EBUSY;
	}
	rw->writer = rumpuser_curlwp();
	return 0;
}

/* From writer to reader; the readers waiting are woken to come in too */
EXPORT void rumpuser_rw_downgrade( RU_RW *rw )
{
	RU_WAITER	*list = NULL, *w;
	UINT		imask;

	ISpinLock(&rw->sl, &imask);
	rw->wheld = FALSE;
	rw->writer = NULL;
	rw->readers = 1;
	if ( rw->wq.head == NULL ) {
		while ( ( w = q_take(&rw->rq) ) != NULL ) {
			w->woken = TRUE;
			w->next = list;
			list = w;
		}
	}
	ISpinUnlock(&rw->sl, &imask);
	rw_signal(list);
}

EXPORT void rumpuser_rw_destroy( RU_RW *rw )
{
	Kfree(rw);
}

/* Held to write by this thread; held to read by anyone */
EXPORT void rumpuser_rw_held( int lk, RU_RW *rw, int *rvp )
{
	if ( lk == RUMPUSER_RW_WRITER ) {
		*rvp = ( rw->wheld && rw->writer == rumpuser_curlwp() );
	} else {
		*rvp = ( rw->readers > 0 );
	}
}

/* ---------------------------------------------------------------- condition variables */

typedef struct rumpuser_cv {
	T_SPLOCK	sl;
	RU_QUEUE	q;
	INT		nwaiters;
} RU_CV;

EXPORT void rumpuser_cv_init( RU_CV **cvp )
{
	RU_CV	*cv = (RU_CV *)Kcalloc(1, sizeof(RU_CV));

	if ( cv == NULL ) {
		tm_printf((UB *)"rumpuser: no memory for a condition variable\n");
		for ( ;; ) tk_slp_tsk(TMO_FEVR);
	}
	InitSpinLock(&cv->sl);
	*cvp = cv;
}

EXPORT void rumpuser_cv_destroy( RU_CV *cv )
{
	Kfree(cv);
}

/*
 * Wait on cv with m held: the task goes on the queue before m is let go,
 * so a signal given under m once it is is not missed. With a limit
 * (tmo_us >= 0), E_TMOUT when it ran out first. m is held again on
 * return. 'wrap' gives up the virtual processor for the wait, with m
 * named as what is held.
 */
LOCAL ER cv_wait( RU_CV *cv, RU_MTX *m, D tmo_us, BOOL wrap )
{
	RU_WAITER	w;
	UINT		imask;
	INT		n = 0;
	ER		er;

	w.semid = sem_get();
	w.woken = FALSE;
	ISpinLock(&cv->sl, &imask);
	cv->nwaiters++;
	ISpinUnlock(&cv->sl, &imask);
	if ( wrap ) {
		unsched_il(&n, m);
	}

	ISpinLock(&cv->sl, &imask);
	q_add(&cv->q, &w);
	ISpinUnlock(&cv->sl, &imask);
	m->owner = NULL;
	lk_unlock(&m->lk);

	er = sem_wait(w.semid, ( tmo_us < 0 ) ? -1 : ( tmo_us > 0 ) ? tmo_us : 1, TRK_CV, cv);
	if ( er < E_OK ) {
		ISpinLock(&cv->sl, &imask);
		if ( !w.woken ) {
			(void)q_remove(&cv->q, &w);
			ISpinUnlock(&cv->sl, &imask);
			er = E_TMOUT;
		} else {
			/* woken as the time ran out: the signal is on its way */
			ISpinUnlock(&cv->sl, &imask);
			(void)sem_wait(w.semid, -1, TRK_CV, cv);
			er = E_OK;
		}
	}
	sem_put(w.semid);

	lk_lock(&m->lk);
	mtx_owned(m);
	if ( wrap ) {
		sched_il(n, m);
	}
	ISpinLock(&cv->sl, &imask);
	cv->nwaiters--;
	ISpinUnlock(&cv->sl, &imask);
	return er;
}

EXPORT void rumpuser_cv_wait( RU_CV *cv, RU_MTX *m )
{
	TR_RA();
	(void)cv_wait(cv, m, -1, TRUE);
}

EXPORT void rumpuser_cv_wait_nowrap( RU_CV *cv, RU_MTX *m )
{
	TR_RA();
	(void)cv_wait(cv, m, -1, FALSE);
}

/* The limit is relative */
EXPORT int rumpuser_cv_timedwait( RU_CV *cv, RU_MTX *m, D sec, D nsec )
{
	D	us = sec * 1000000 + ( nsec + 999 ) / 1000;

	TR_RA();

	return ( cv_wait(cv, m, ( us > 0 ) ? us : 0, TRUE) == E_TMOUT ) ? NB_ETIMEDOUT : 0;
}

EXPORT void rumpuser_cv_signal( RU_CV *cv )
{
	RU_WAITER	*w;
	UINT		imask;
	ID		sem = 0;

	ISpinLock(&cv->sl, &imask);
	w = q_take(&cv->q);
	if ( w != NULL ) {
		sem = q_wake(w);
	}
	ISpinUnlock(&cv->sl, &imask);
	if ( sem > 0 ) {
		tk_sig_sem(sem, 1);
	}
}

EXPORT void rumpuser_cv_broadcast( RU_CV *cv )
{
	RU_WAITER	*w, *next;
	UINT		imask;
	ID		sem;

	ISpinLock(&cv->sl, &imask);
	w = cv->q.head;
	cv->q.head = cv->q.tail = NULL;
	for ( next = w; next != NULL; next = next->next ) {
		next->woken = TRUE;
	}
	ISpinUnlock(&cv->sl, &imask);

	/* each entry is read before its owner is let go */
	while ( w != NULL ) {
		next = w->next;
		sem = w->semid;
		tk_sig_sem(sem, 1);
		w = next;
	}
}

EXPORT void rumpuser_cv_has_waiters( RU_CV *cv, int *rvp )
{
	*rvp = ( cv->nwaiters > 0 );
}

/* ---------------------------------------------------------------- threads */

typedef struct {
	void	*(*func)( void * );
	void	*arg;
	ID	joinsem;		/* 0: nobody waits for its end */
} RU_THREAD;

LOCAL void thread_entry( INT stacd, void *exinf )
{
	RU_THREAD	*t = (RU_THREAD *)exinf;

	(void)t->func(t->arg);
	rumpuser_thread_exit();
}

EXPORT int rumpuser_thread_create( void *(*func)( void * ), void *arg,
				   CONST char *name, int mustjoin, int priority,
				   int cpuidx, void **cookie )
{
	RU_THREAD	*t;
	T_CTSK		ctsk;
	T_CSEM		csem;
	ID		tskid;

	t = (RU_THREAD *)Kmalloc(sizeof(RU_THREAD));
	if ( t == NULL ) {
		return NB_ENOMEM;
	}
	t->func = func;
	t->arg = arg;
	t->joinsem = 0;
	if ( mustjoin ) {
		csem.exinf   = NULL;
		csem.sematr  = TA_TFIFO | TA_FIRST;
		csem.isemcnt = 0;
		csem.maxsem  = 1;
		t->joinsem = tk_cre_sem(&csem);
		if ( t->joinsem <= 0 ) {
			Kfree(t);
			return NB_ENOMEM;
		}
	}

	ctsk.exinf   = t;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)thread_entry;
	ctsk.itskpri = RU_TASK_PRI;
	ctsk.stksz   = RU_TASK_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		if ( t->joinsem > 0 ) tk_del_sem(t->joinsem);
		Kfree(t);
		return NB_ENOMEM;
	}
	if ( tk_sta_tsk(tskid, 0) < E_OK ) {
		tk_del_tsk(tskid);
		if ( t->joinsem > 0 ) tk_del_sem(t->joinsem);
		Kfree(t);
		return NB_ENOMEM;
	}
	if ( cookie != NULL ) {
		*cookie = t;
	}
	return 0;
}

EXPORT void rumpuser_thread_exit( void )
{
	T_RTSK		rtsk;
	RU_THREAD	*t = NULL;

	ru_curlwp[tk_get_tid()] = NULL;
	if ( tk_ref_tsk(TSK_SELF, &rtsk) >= E_OK ) {
		t = (RU_THREAD *)rtsk.exinf;
	}
	if ( t != NULL ) {
		if ( t->joinsem > 0 ) {
			tk_sig_sem(t->joinsem, 1);	/* the joiner frees it */
		} else {
			Kfree(t);
		}
	}
	tk_exd_tsk();
}

EXPORT int rumpuser_thread_join( void *cookie )
{
	RU_THREAD	*t = (RU_THREAD *)cookie;
	INT		n;

	if ( t == NULL || t->joinsem <= 0 ) {
		return NB_EINVAL;
	}
	knl_rump_unsched(&n);
	(void)sem_wait(t->joinsem, -1, TRK_CV, t);
	knl_rump_sched(n);
	tk_del_sem(t->joinsem);
	Kfree(t);
	return 0;
}

/* ---------------------------------------------------------------- clocks */

#define RUMPUSER_CLOCK_RELWALL	0
#define RUMPUSER_CLOCK_ABSMONO	1

EXPORT int rumpuser_clock_gettime( int clk, D *sec, long *nsec )
{
	SYSTIM_U	t;
	UD		ns;

	if ( clk == RUMPUSER_CLOCK_RELWALL ) {
		if ( tk_get_tim_u(&t, NULL) < E_OK ) {
			t = 0;
		}
		*sec = t / 1000000 + RU_TRON_UNIX_S;
		*nsec = (long)( ( t % 1000000 ) * 1000 );
	} else {
		if ( ts_get_mono(&ns) < E_OK ) {
			ns = 0;
		}
		*sec = (D)( ns / 1000000000ULL );
		*nsec = (long)( ns % 1000000000ULL );
	}
	return 0;
}

/* A sleep, relative or until a time of the monotonic clock */
EXPORT int rumpuser_clock_sleep( int clk, D sec, long nsec )
{
	UD	now = 0, until;
	D	us;
	INT	n;

	(void)ts_get_mono(&now);
	if ( clk == RUMPUSER_CLOCK_ABSMONO ) {
		until = (UD)sec * 1000000000ULL + (UD)nsec;
	} else {
		until = now + (UD)sec * 1000000000ULL + (UD)nsec;
	}
	knl_rump_unsched(&n);
	while ( now < until ) {
		us = (D)( ( until - now + 999 ) / 1000 );
		(void)tk_dly_tsk_u((RELTIM_U)us);
		(void)ts_get_mono(&now);
	}
	knl_rump_sched(n);
	return 0;
}

/* ---------------------------------------------------------------- the host */

/*
 * The parameters the rump kernel asks for. As many virtual processors
 * as there are processors; a limit on its memory, past which it gives
 * back what its pools keep.
 */
EXPORT int rumpuser_getparam( CONST char *name, void *buf, UD blen )
{
	CONST char	*v = NULL;
	char		ncpu[4];
	UD		i;

	if ( knl_strcmp(name, "_RUMPUSER_NCPU") == 0 ) {
		ncpu[0] = (char)( '0' + ( ( knl_num_prc > 0 && knl_num_prc < 10 ) ? knl_num_prc : 1 ) );
		ncpu[1] = '\0';
		v = ncpu;
	} else if ( knl_strcmp(name, "_RUMPUSER_HOSTNAME") == 0 ) {
		v = "tessronos";
	} else if ( knl_strcmp(name, "RUMP_MEMLIMIT") == 0 ) {
		v = "128m";
	}
	if ( v == NULL ) {
		return NB_ENOENT;
	}
	for ( i = 0; i + 1 < blen && v[i] != '\0'; i++ ) {
		((char *)buf)[i] = v[i];
	}
	if ( blen > 0 ) {
		((char *)buf)[i] = '\0';
	}
	return 0;
}

EXPORT int rumpuser_getrandom( void *buf, UD buflen, int flags, UD *retp )
{
	if ( ts_get_random(buf, (SZ)buflen) < E_OK ) {
		*retp = 0;
		return NB_EINVAL;
	}
	*retp = buflen;
	return 0;
}

/* The rump kernel's console: whole lines to T-Monitor's */
LOCAL T_SPLOCK	con_lock;
LOCAL char	con_line[160];
LOCAL INT	con_len = 0;

EXPORT void rumpuser_putchar( int c )
{
	char	out[sizeof(con_line) + 1];
	UINT	imask;
	INT	n = 0;

	ISpinLock(&con_lock, &imask);
	if ( c != '\n' && c != '\r' ) {
		con_line[con_len++] = (char)c;
	}
	if ( c == '\n' || con_len >= (INT)sizeof(con_line) - 1 ) {
		for ( n = 0; n < con_len; n++ ) {
			out[n] = con_line[n];
		}
		out[n] = '\0';
		con_len = 0;
		n = ( n > 0 ) ? n : -1;
	}
	ISpinUnlock(&con_lock, &imask);
	if ( n != 0 ) {
		tm_printf((UB *)"%s\n", ( n > 0 ) ? out : "");
	}
}

/*
 * The rump kernel's own messages of the host's: only what is needed to
 * read them -- strings, integers and pointers.
 */
EXPORT void rumpuser_dprintf( CONST char *fmt, ... )
{
	__builtin_va_list	ap;
	char			buf[200], num[24];
	CONST char		*s;
	INT			o = 0, k, lng;
	UD			v;
	BOOL			neg;

	__builtin_va_start(ap, fmt);
	for ( ; *fmt != '\0' && o < (INT)sizeof(buf) - 1; fmt++ ) {
		if ( *fmt != '%' ) {
			buf[o++] = *fmt;
			continue;
		}
		fmt++;
		lng = 0;
		while ( *fmt == 'l' || *fmt == 'z' || *fmt == 'j' ) {
			lng = 1;
			fmt++;
		}
		s = NULL;
		switch ( *fmt ) {
		case 's':
			s = __builtin_va_arg(ap, CONST char *);
			if ( s == NULL ) s = "(null)";
			break;
		case 'c':
			num[0] = (char)__builtin_va_arg(ap, int);
			num[1] = '\0';
			s = num;
			break;
		case 'd': case 'i': case 'u': case 'x': case 'p':
			if ( *fmt == 'p' || lng ) {
				v = __builtin_va_arg(ap, UD);
			} else if ( *fmt == 'd' || *fmt == 'i' ) {
				v = (UD)(D)__builtin_va_arg(ap, int);
			} else {
				v = __builtin_va_arg(ap, unsigned int);
			}
			neg = ( ( *fmt == 'd' || *fmt == 'i' ) && (D)v < 0 );
			if ( neg ) v = (UD)( -(D)v );
			k = sizeof(num) - 1;
			num[k] = '\0';
			do {
				UINT	d = (UINT)( v % ( ( *fmt == 'x' || *fmt == 'p' ) ? 16 : 10 ) );

				num[--k] = (char)( ( d < 10 ) ? '0' + d : 'a' + d - 10 );
				v /= ( *fmt == 'x' || *fmt == 'p' ) ? 16 : 10;
			} while ( v != 0 && k > 1 );
			if ( neg ) num[--k] = '-';
			s = &num[k];
			break;
		case '%':
			s = "%";
			break;
		default:
			s = "?";
			break;
		}
		while ( *s != '\0' && o < (INT)sizeof(buf) - 1 ) {
			buf[o++] = *s++;
		}
	}
	__builtin_va_end(ap);
	buf[o] = '\0';
	tm_printf((UB *)"%s", buf);
}

EXPORT void rumpuser_seterrno( int error )
{
	/* the calls here take the error from rump_syscall itself */
}

/*
 * The rump kernel stopped (a panic): it cannot go on, but the rest of
 * the system can, without a network. The task that found it sleeps.
 */
EXPORT void rumpuser_exit( int rv )
{
	tm_printf((UB *)"rumpuser: the network stack stopped (%d)\n", rv);
	for ( ;; ) {
		tk_slp_tsk(TMO_FEVR);
	}
}

EXPORT int rumpuser_kill( D pid, int sig )
{
	return NB_EOPNOTSUPP;
}

/* Components are found through the rump kernel's link sets, not loaded */
EXPORT void rumpuser_dl_bootstrap( void *domodinit, void *symload, void *compload,
				   void *evcntattach )
{
}

EXPORT int rumpuser_daemonize_begin( void )
{
	return 0;
}

EXPORT int rumpuser_daemonize_done( int error )
{
	return 0;
}

#if RU_TRACE
/* Every two seconds: the waits for a lock of more than five seconds */
LOCAL void ru_trace_task( INT stacd, void *exinf )
{
	UD	now;
	INT	tid;

	for ( ;; ) {
		tk_dly_tsk(2000);
		(void)ts_get_mono(&now);
		for ( tid = 1; tid <= CNF_MAX_TSKID; tid++ ) {
			RU_TR	*t = &ru_tr[tid];
			UD	age;

			if ( t->kind == 0 || t->told ) {
				continue;
			}
			age = ( now - t->since ) / 1000000ULL;
			if ( age < ( ( t->kind == TRK_CV ) ? 20000 : 5000 ) ) {
				continue;
			}
			t->told = 1;
			tm_printf((UB *)"rumpuser: task %d has waited %d ms on %s %p, from %p\n",
				  tid, (INT)age, ( t->kind == TRK_LOCK ) ? "lock" : ( t->kind == TRK_RW ) ? "rwlock" : "cv",
				  t->obj, t->ra);
		}
	}
}

LOCAL void ru_trace_start( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)ru_trace_task;
	ctsk.itskpri = 2;
	ctsk.stksz   = 8 * 1024;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid > 0 ) {
		(void)tk_sta_tsk(tskid, 0);
	}
}
#endif
