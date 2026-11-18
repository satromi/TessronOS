/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpu.c
 *	The common layer of the GPU (docs/tessronos/13-gpu.md 13.4).
 *
 *	Buffer objects. A BO is an array of pages taken one at a time from
 *	the page allocator, so that a large one never needs a large run of
 *	contiguous memory: the GPU's own MMU makes them contiguous in its
 *	address space. The pages are zeroed through the processor's cache
 *	and then cleaned out of it once, because the GPU reads memory
 *	without looking at the caches; after that the processor only
 *	reaches them through uncached mappings.
 *
 *	A process sees a BO in the shared window. The address is taken
 *	once, when the BO is first mapped, and is the same in every process
 *	that maps it: a window's surface drawn by one process is read at
 *	the same address by the window system. Pages in the shared window
 *	belong to whoever shared them, so taking a process space down does
 *	not free them; the BO does, when its last reference goes.
 *
 *	Jobs and fences. Jobs go into one queue in the order they are
 *	submitted, each with the next fence number, and one task takes them
 *	out and runs them on the backend one at a time. That order is what
 *	makes a fence mean "this job and all before it are over", and it is
 *	also what the V3D needs from jobs that depend on each other. Running
 *	the binner of one job beside the renderer of the one before, as the
 *	hardware could, is left for later.
 *
 *	Address spaces. A backend may give page tables of its own besides
 *	the kernel's (T_GPUOPS as_create). A job submitted in one holds a
 *	reference to it, so that the table outlives every queued job that
 *	runs on it; the backend switches its MMU to the job's table when the
 *	job starts.
 *
 *	A waiter clears the "a fence is over" bit, looks at the last fence
 *	that is over, and only then waits for the bit. The task sets the
 *	number before the bit, so a fence that ends between the look and the
 *	wait still wakes the waiter. Every waiter looks again after it
 *	wakes, so one waiter clearing the bit cannot make another miss it.
 *
 *	A waiter that is about to sleep on a job still in the queue (one
 *	that has not started: it waits behind others) marks the job and has
 *	the backend raise its clock until that job is over (T_GPUOPS boost).
 *	The GPU's own measure of how busy it is takes tens of milliseconds
 *	to answer, while the waiter, stalled, adds nothing to it. A job that
 *	runs already is not marked: it is expected to end soon, and raising
 *	the clock on every short wait would keep it high under a light load.
 *	A look that does not sleep (TMO_POL) marks nothing.
 *
 *	The screen marks jobs the same way when a BO it was to show is still
 *	being drawn a build later (knl_gpu_bo_boost): the frame is late, and
 *	the queued jobs that draw it are hurried as a waiter's would be. The
 *	window manager also tells each build (knl_gpu_interactive); the
 *	backend may then answer the load more quickly while frames come
 *	(T_GPUOPS interactive).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smem.h>
#include <ts/time.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include "gpudev.h"

#define GPU_TASK_PRI		9
#define GPU_TASK_STKSZ		16384

#define FLG_JOB			0x0001	/* a job was queued */
#define FLG_END			0x0002	/* the backend says the running job is over */
#define FLG_DONE		0x0004	/* a fence is over */

#define FAILED_FIRST		16	/* the first room for the runs of failed fences */

/*
 * A process's view of a BO: uncached, read and write, never executed
 * (made here where the architecture's sysdef.h does not name it)
 */
#ifndef PTE_PAGE_UDATA_NC
#define PTE_PAGE_UDATA_NC	(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_NC) | PTE_AP_RW_ALL | \
				 PTE_SH_INNER | PTE_AF | PTE_nG | PTE_PXN | PTE_UXN)
#endif

struct gpubo {
	UD		npages;
	UD		*pa;		/* physical address of each page */
	UW		offset;		/* GPU address, 0 when not bound */
	UINT		flags;		/* GPU_BO_* */
	UBINT		uva;		/* in the shared window, 0 before the first map */
	UINT		refcnt;
	UINT		busy;		/* queued or running jobs that use it */
	CONST T_GPUOPS	*bound;		/* the backend whose MMU holds it */
};

struct gpuas {
	void		*priv;		/* the backend's */
	UINT		refcnt;
};

typedef struct qjob {
	struct qjob	*next;
	D		fence;
	T_GPUAS		*as;		/* NULL: the kernel's space */
	T_GPUJOB	job;
	BOOL		boost;		/* waited for before it started: the backend told */
	INT		nbo;
	T_GPUBO		*bo[1];		/* nbo of them */
} QJOB;

/*
 * Taken with interrupts disabled around everything below that is not
 * the task's alone: disabling interrupts alone keeps out only this
 * processor.
 */
LOCAL T_SPLOCK		gpu_lock;

LOCAL CONST T_GPUOPS	*gpu_ops;
LOCAL void		*gpu_priv;
LOCAL ID		gpu_tsk;
LOCAL ID		gpu_flg;
LOCAL QJOB		*q_head, *q_tail;
LOCAL BOOL		q_running;	/* the task has a job out of the queue */
LOCAL D			fence_run;	/* that job's, while q_running */
LOCAL UD		n_wait_run, n_wait_queued;	/* waits that slept, on a running job, on queued ones */
LOCAL D			fence_next;	/* last fence handed out */
LOCAL volatile D	fence_done;	/* last fence that is over */
/*
 * The fences whose jobs failed, every one, as runs of fences in a row
 * (a reset fails the jobs queued with it together), in the order of the
 * fences. Added to by the task alone; the table is made larger outside
 * the lock and put in place under it.
 */
typedef struct {
	D	first, last;
} FRUN;

LOCAL FRUN		failed_first[FAILED_FIRST];	/* the first room, so that a few cost no memory */
LOCAL FRUN		*failed = failed_first;
LOCAL INT		nfailed = 0, failed_cap = FAILED_FIRST;
LOCAL volatile ER	end_result;	/* what the backend said of the running job */
LOCAL UINT		bound_bos;	/* live BOs in the backend's MMU */
LOCAL UINT		live_as;	/* address spaces of the backend's that are alive */

EXPORT void		(*knl_gpu_end_hook)( void ) = NULL;

/* ---------------------------------------------------------------- caches */

EXPORT void knl_gpu_dcache_clean_inval( UBINT va, UBINT len )
{
#ifdef CPU_CORE_ARMV8A
	UBINT	end = va + len;

	Asm("dsb sy" ::: "memory");
	for ( va &= ~63ULL; va < end; va += 64 ) {
		Asm("dc civac, %0" :: "r"(va) : "memory");
	}
	Asm("dsb sy" ::: "memory");
#else
	DCacheFlushRange((CONST void *)va, len);
#endif
}

/* ---------------------------------------------------------------- buffer objects */

LOCAL void bo_free( T_GPUBO *bo )
{
	UD	i;
	UINT	imask;

	if ( bo->bound != NULL ) {
		bo->bound->bo_unbind(gpu_priv, bo->offset, bo->npages);
		ISpinLock(&gpu_lock, &imask);
		bound_bos--;
		ISpinUnlock(&gpu_lock, &imask);
	}
	for ( i = 0; i < bo->npages && ( bo->flags & GPU_BO_WRAPPED ) == 0; i++ ) {
		if ( bo->pa[i] != 0 ) {
			knl_free_pages(knl_pa_to_pf(bo->pa[i]), 0);
		}
	}
	Kfree(bo->pa);
	Kfree(bo);
}

EXPORT T_GPUBO *knl_gpu_bo_create( SZ size, UINT flags, ER *p_er )
{
	T_GPUBO		*bo;
	CONST T_GPUOPS	*ops;
	UD		i;
	UINT		aflags = KAF_ZERO;
	UINT		imask;
	ER		er = E_OK;

	if ( size <= 0 || (UD)size > GPU_BO_MAXSIZE ) {
		er = E_PAR;
		goto out;
	}
	ISpinLock(&gpu_lock, &imask);
	ops = gpu_ops;
	ISpinUnlock(&gpu_lock, &imask);
	if ( ( flags & GPU_BO_NOGPU ) == 0 && ops == NULL ) {
		er = E_NOEXS;
		goto out;
	}
	if ( ops != NULL && ops->pa_limit != 0 && ops->pa_limit <= 0x100000000ULL ) {
		aflags |= KAF_DMA32;
	}

	bo = (T_GPUBO *)Kmalloc(sizeof(T_GPUBO));
	if ( bo == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	knl_memset(bo, 0, sizeof(T_GPUBO));
	bo->npages = ( (UD)size + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	bo->flags = flags;
	bo->refcnt = 1;
	bo->pa = (UD *)Kmalloc(sizeof(UD) * bo->npages);
	if ( bo->pa == NULL ) {
		Kfree(bo);
		er = E_NOMEM;
		goto out;
	}
	knl_memset(bo->pa, 0, sizeof(UD) * bo->npages);

	for ( i = 0; i < bo->npages; i++ ) {
		PFRAME	*pf = knl_alloc_pages(0, ( aflags & KAF_DMA32 ) ? ZONE_DMA32 : ZONE_NORMAL,
					      aflags);

		if ( pf == NULL
		  || ( ops != NULL && ops->pa_limit != 0 && knl_pf_to_pa(pf) + PAGE_SIZE > ops->pa_limit ) ) {
			if ( pf != NULL ) knl_free_pages(pf, 0);
			bo_free(bo);
			er = E_NOMEM;
			goto out;
		}
		bo->pa[i] = knl_pf_to_pa(pf);
		/* the zeroes went through the cache: push them out and drop the lines */
		knl_gpu_dcache_clean_inval((UBINT)PA2VA(bo->pa[i]), PAGE_SIZE);
	}

	if ( ( flags & GPU_BO_NOGPU ) == 0 ) {
		er = ops->bo_bind(gpu_priv, bo, bo->npages, &bo->offset);
		if ( er < E_OK ) {
			bo_free(bo);
			goto out;
		}
		bo->bound = ops;
		ISpinLock(&gpu_lock, &imask);
		bound_bos++;
		ISpinUnlock(&gpu_lock, &imask);
	}
	if ( p_er != NULL ) *p_er = E_OK;
	return bo;

out:
	if ( p_er != NULL ) *p_er = er;
	return NULL;
}

/*
 * Memory that is someone else's -- the screen's back buffer, a window's
 * pixels -- entered in the GPU's MMU as it is. The pages stay where and
 * whose they are: letting the BO go takes them out of the MMU and frees
 * nothing. The owner keeps them alive while a job can use them, and has
 * to make what it wrote through the processor's caches reach memory
 * before the GPU reads it.
 */
EXPORT T_GPUBO *knl_gpu_bo_wrap( CONST UD *pa, UD npages, ER *p_er )
{
	T_GPUBO		*bo;
	CONST T_GPUOPS	*ops;
	UD		i;
	UINT		imask;
	ER		er = E_OK;

	ISpinLock(&gpu_lock, &imask);
	ops = gpu_ops;
	ISpinUnlock(&gpu_lock, &imask);
	if ( ops == NULL ) {
		er = E_NOEXS;
		goto out;
	}
	if ( npages == 0 || ( npages << PAGE_SHIFT ) > GPU_BO_MAXSIZE ) {
		er = E_PAR;
		goto out;
	}
	for ( i = 0; i < npages; i++ ) {
		if ( ( pa[i] & ( PAGE_SIZE - 1 ) ) != 0
		  || ( ops->pa_limit != 0 && pa[i] + PAGE_SIZE > ops->pa_limit ) ) {
			er = E_PAR;		/* where the GPU cannot reach */
			goto out;
		}
	}
	bo = (T_GPUBO *)Kmalloc(sizeof(T_GPUBO));
	if ( bo == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	knl_memset(bo, 0, sizeof(T_GPUBO));
	bo->npages = npages;
	bo->flags = GPU_BO_WRAPPED;
	bo->refcnt = 1;
	bo->pa = (UD *)Kmalloc(sizeof(UD) * npages);
	if ( bo->pa == NULL ) {
		Kfree(bo);
		er = E_NOMEM;
		goto out;
	}
	knl_memcpy(bo->pa, pa, sizeof(UD) * npages);
	er = ops->bo_bind(gpu_priv, bo, npages, &bo->offset);
	if ( er < E_OK ) {
		bo_free(bo);
		goto out;
	}
	bo->bound = ops;
	ISpinLock(&gpu_lock, &imask);
	bound_bos++;
	ISpinUnlock(&gpu_lock, &imask);
	if ( p_er != NULL ) *p_er = E_OK;
	return bo;

out:
	if ( p_er != NULL ) *p_er = er;
	return NULL;
}

EXPORT void knl_gpu_bo_get( T_GPUBO *bo )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	bo->refcnt++;
	ISpinUnlock(&gpu_lock, &imask);
}

EXPORT void knl_gpu_bo_put( T_GPUBO *bo )
{
	UINT	imask;
	BOOL	last;

	if ( bo == NULL ) return;
	ISpinLock(&gpu_lock, &imask);
	last = ( --bo->refcnt == 0 );
	ISpinUnlock(&gpu_lock, &imask);
	if ( last ) {
		bo_free(bo);	/* a job holds a reference, so none uses it */
	}
}

EXPORT ER knl_gpu_bo_ref( T_GPUBO *bo, T_GPUBOINFO *info )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	info->size   = (SZ)( bo->npages << PAGE_SHIFT );
	info->offset = bo->offset;
	info->uva    = bo->uva;
	info->refcnt = bo->refcnt;
	info->busy   = bo->busy;
	ISpinUnlock(&gpu_lock, &imask);
	return E_OK;
}

EXPORT UD knl_gpu_bo_npages( T_GPUBO *bo )
{
	return bo->npages;
}

EXPORT CONST UD *knl_gpu_bo_pages( T_GPUBO *bo )
{
	return bo->pa;
}

EXPORT UD knl_gpu_bo_page( T_GPUBO *bo, UD i )
{
	return ( i < bo->npages ) ? bo->pa[i] : 0;
}

/*
 * A process's view of a BO: uncached, but cached where the GPU sees
 * memory through the processor's caches (it shares the last level cache)
 */
#ifdef __x86_64__
#define BO_PTE	( ( gpu_ops != NULL && gpu_ops->coherent ) ? PTE_PAGE_UDATA : PTE_PAGE_UDATA_NC )
#else
#define BO_PTE	PTE_PAGE_UDATA_NC
#endif

/*
 * A mapping holds a reference, so that the pages cannot go while a
 * process can still reach them; unmapping gives it back.
 */
EXPORT ER knl_gpu_bo_map( T_GPUBO *bo, void *space, UBINT *p_va )
{
	T_SPACE	*sp = (T_SPACE *)space;
	UBINT	va;
	UD	i;
	UINT	imask;

	if ( sp == NULL || sp->l0_pa == 0 ) return E_PAR;

	ISpinLock(&gpu_lock, &imask);
	va = bo->uva;
	ISpinUnlock(&gpu_lock, &imask);
	if ( va == 0 ) {
		UBINT	nva = knl_shm_va_alloc(bo->npages << PAGE_SHIFT);

		if ( nva == 0 ) return E_NOMEM;
		ISpinLock(&gpu_lock, &imask);
		if ( bo->uva == 0 ) bo->uva = nva;	/* another map may have come first */
		va = bo->uva;
		ISpinUnlock(&gpu_lock, &imask);
	}
	if ( knl_pt_next(sp->l0_pa, va, va + ( bo->npages << PAGE_SHIFT )) != va + ( bo->npages << PAGE_SHIFT ) ) {
		return E_OBJ;				/* mapped into this space already */
	}
	for ( i = 0; i < bo->npages; i++ ) {
		if ( knl_pt_map(sp->l0_pa, va + ( i << PAGE_SHIFT ), bo->pa[i], PAGE_SIZE,
				BO_PTE) != E_OK ) {
			knl_pt_unmap(sp->l0_pa, va, i << PAGE_SHIFT);
			return E_NOMEM;
		}
	}
	knl_gpu_bo_get(bo);
	if ( p_va != NULL ) *p_va = va;
	return E_OK;
}

EXPORT ER knl_gpu_bo_unmap( T_GPUBO *bo, void *space )
{
	T_SPACE	*sp = (T_SPACE *)space;

	if ( sp == NULL || sp->l0_pa == 0 || bo->uva == 0 ) return E_PAR;
	if ( knl_pt_lookup(sp->l0_pa, bo->uva) != bo->pa[0] ) return E_NOEXS;

	knl_pt_unmap(sp->l0_pa, bo->uva, bo->npages << PAGE_SHIFT);
	knl_gpu_bo_put(bo);
	return E_OK;
}

/*
 * The kernel's view of a BO: the same attribute as a process's (BO_PTE),
 * so that no page is seen through two kinds of mapping that disagree on
 * the cache.
 */
EXPORT ER knl_gpu_bo_kmap( T_GPUBO *bo, void **p_va )
{
	UINT	flags = VMAP_NOCACHE;
	void	*va;

	if ( bo == NULL || p_va == NULL || bo->npages == 0 ) return E_PAR;
#ifdef __x86_64__
	if ( gpu_ops != NULL && gpu_ops->coherent ) flags = 0;
#endif
	va = knl_vmap_pages(bo->pa, bo->npages, flags);
	if ( va == NULL ) return E_NOMEM;
	knl_gpu_bo_get(bo);
	*p_va = va;
	return E_OK;
}

EXPORT void knl_gpu_bo_kunmap( T_GPUBO *bo, void *va )
{
	if ( bo == NULL || va == NULL ) return;
	knl_vunmap_pa(va, bo->npages);
	knl_gpu_bo_put(bo);
}

/* ---------------------------------------------------------------- fences */

/* Whether the job of a fence failed; under the lock */
LOCAL BOOL fence_failed( D fence )
{
	INT	lo = 0, hi = nfailed - 1, mid;

	while ( lo <= hi ) {
		mid = ( lo + hi ) / 2;
		if ( fence < failed[mid].first ) {
			hi = mid - 1;
		} else if ( fence > failed[mid].last ) {
			lo = mid + 1;
		} else {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Room for one more run of failed fences, made by the task before it
 * takes the lock (it alone adds to the table); FALSE when there is no
 * memory, said on the console: a wait for that fence then sees it over
 */
LOCAL BOOL failed_room( void )
{
	FRUN	*t, *old;
	UINT	imask;
	INT	cap;

	if ( nfailed < failed_cap ) {
		return TRUE;
	}
	cap = failed_cap * 2;
	t = (FRUN *)Kmalloc(sizeof(FRUN) * (SZ)cap);
	if ( t == NULL ) {
		tm_printf((UB *)"gpu: no memory to remember %d failed jobs: a wait may see one as done\n", cap);
		return FALSE;
	}
	ISpinLock(&gpu_lock, &imask);
	if ( nfailed > 0 ) knl_memcpy(t, failed, sizeof(FRUN) * (SZ)nfailed);
	old = failed;
	failed = t;
	failed_cap = cap;
	ISpinUnlock(&gpu_lock, &imask);
	if ( old != failed_first ) Kfree(old);
	return TRUE;
}

EXPORT D knl_gpu_done( void )
{
	return fence_done;
}

/*
 * Wait until 'cond' holds of the state under the lock, a fence at a
 * time. The deadline is kept in monotonic time so that many wakeups do
 * not stretch the wait.
 */
LOCAL ER wait_until( BOOL (*cond)( void *arg ), void *arg, TMO_U tmout )
{
	UD	now, end = 0;
	UINT	ptn;
	ER	er;

	if ( tmout > 0 ) {
		ts_get_mono(&now);
		end = now + (UD)tmout * 1000;
	}
	for (;;) {
		TMO_U	left = tmout;

		tk_clr_flg(gpu_flg, ~FLG_DONE);
		if ( cond(arg) ) return E_OK;
		if ( tmout == TMO_POL ) return E_TMOUT;
		if ( tmout > 0 ) {
			ts_get_mono(&now);
			if ( now >= end ) return E_TMOUT;
			left = (TMO_U)( ( end - now + 999 ) / 1000 );
		}
		er = tk_wai_flg_u(gpu_flg, FLG_DONE, TWF_ORW, &ptn, left);
		if ( er == E_TMOUT ) {
			if ( cond(arg) ) return E_OK;
			return E_TMOUT;
		}
		if ( er < E_OK ) return er;
	}
}

LOCAL BOOL fence_reached( void *arg )
{
	return (BOOL)( fence_done >= *(D *)arg );
}

/* Whether a queued job uses 'bo'; under the lock */
LOCAL BOOL uses( CONST QJOB *q, CONST T_GPUBO *bo )
{
	INT	i;

	for ( i = 0; i < q->nbo; i++ ) {
		if ( q->bo[i] == bo ) return TRUE;
	}
	return FALSE;
}

/*
 * About to sleep on the job of 'fence', or on every job that uses 'bo'
 * (or, not 'wait', the screen still waiting for 'bo'): those still
 * queued marked, and the backend told of the new ones; a wait counted
 */
LOCAL void boost_queued( D fence, CONST T_GPUBO *bo, BOOL wait )
{
	CONST T_GPUOPS	*ops;
	void		*priv;
	QJOB		*q;
	UINT		imask;
	INT		n = 0;
	BOOL		queued = FALSE, running = FALSE;

	ISpinLock(&gpu_lock, &imask);
	ops = gpu_ops;
	priv = gpu_priv;
	if ( bo == NULL ) {
		if ( fence > fence_done ) {
			for ( q = q_head; q != NULL && q->fence != fence; q = q->next ) ;
			if ( q != NULL ) {
				queued = TRUE;
				if ( !q->boost ) {
					q->boost = TRUE;
					n++;
				}
			} else {
				running = q_running;
			}
		}
	} else if ( bo->busy > 0 ) {
		for ( q = q_head; q != NULL; q = q->next ) {
			if ( !uses(q, bo) ) continue;
			queued = TRUE;
			if ( !q->boost ) {
				q->boost = TRUE;
				n++;
			}
		}
		running = !queued;
	}
	if ( wait && queued ) n_wait_queued++;
	if ( wait && running ) n_wait_run++;
	ISpinUnlock(&gpu_lock, &imask);
	/* the backend cannot leave while those jobs are queued (knl_gpu_unregister) */
	if ( n > 0 && ops != NULL && ops->boost != NULL ) ops->boost(priv, n);
}

EXPORT void knl_gpu_wait_counts( UD *p_running, UD *p_queued )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	*p_running = n_wait_run;
	*p_queued = n_wait_queued;
	ISpinUnlock(&gpu_lock, &imask);
}

EXPORT ER knl_gpu_wait( D fence, TMO_U tmout )
{
	UINT	imask;
	BOOL	bad;
	ER	er;

	if ( fence <= 0 || fence > fence_next || gpu_flg <= 0 ) return E_PAR;
	if ( tmout != TMO_POL ) boost_queued(fence, NULL, TRUE);
	er = wait_until(fence_reached, &fence, tmout);
	if ( er < E_OK ) return er;
	ISpinLock(&gpu_lock, &imask);
	bad = fence_failed(fence);
	ISpinUnlock(&gpu_lock, &imask);
	return bad ? E_IO : E_OK;
}

LOCAL BOOL bo_idle( void *arg )
{
	return (BOOL)( ((T_GPUBO *)arg)->busy == 0 );
}

EXPORT ER knl_gpu_bo_wait( T_GPUBO *bo, TMO_U tmout )
{
	if ( gpu_flg <= 0 ) return ( bo->busy == 0 ) ? E_OK : E_SYS;
	if ( tmout != TMO_POL ) boost_queued(0, bo, TRUE);
	return wait_until(bo_idle, bo, tmout);
}

/*
 * The screen missed a build for 'bo': as a wait on it would, without
 * sleeping and without being counted as a wait. A job that runs already
 * is expected to be over by the next build.
 */
EXPORT void knl_gpu_bo_boost( T_GPUBO *bo )
{
	if ( gpu_flg <= 0 || bo == NULL ) return;
	boost_queued(0, bo, FALSE);
}

/* A frame of the screen built: told to the backend that keeps a pace for it */
EXPORT void knl_gpu_interactive( void )
{
	CONST T_GPUOPS	*ops;
	void		*priv;
	UINT		imask;

	ISpinLock(&gpu_lock, &imask);
	ops = gpu_ops;
	priv = gpu_priv;
	ISpinUnlock(&gpu_lock, &imask);
	/* only a test takes a backend out again (knl_gpu_unregister), and its backends keep no pace */
	if ( ops != NULL && ops->interactive != NULL ) ops->interactive(priv);
}

/* ---------------------------------------------------------------- address spaces */

EXPORT T_GPUAS *knl_gpu_as_create( ER *p_er )
{
	CONST T_GPUOPS	*ops;
	T_GPUAS		*as;
	UINT		imask;
	ER		er;

	ISpinLock(&gpu_lock, &imask);
	ops = gpu_ops;
	ISpinUnlock(&gpu_lock, &imask);
	if ( ops == NULL || ops->as_create == NULL ) {
		er = ( ops == NULL ) ? E_NOEXS : E_NOSPT;
		goto out;
	}
	as = (T_GPUAS *)Kmalloc(sizeof(T_GPUAS));
	if ( as == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	as->refcnt = 1;
	er = ops->as_create(gpu_priv, &as->priv);
	if ( er < E_OK ) {
		Kfree(as);
		goto out;
	}
	ISpinLock(&gpu_lock, &imask);
	live_as++;
	ISpinUnlock(&gpu_lock, &imask);
	if ( p_er != NULL ) *p_er = E_OK;
	return as;

out:
	if ( p_er != NULL ) *p_er = er;
	return NULL;
}

EXPORT void knl_gpu_as_get( T_GPUAS *as )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	as->refcnt++;
	ISpinUnlock(&gpu_lock, &imask);
}

EXPORT void knl_gpu_as_put( T_GPUAS *as )
{
	UINT	imask;
	BOOL	last;

	if ( as == NULL ) return;
	ISpinLock(&gpu_lock, &imask);
	last = ( --as->refcnt == 0 );
	if ( last ) live_as--;
	ISpinUnlock(&gpu_lock, &imask);
	if ( last ) {
		gpu_ops->as_destroy(gpu_priv, as->priv);
		Kfree(as);
	}
}

EXPORT void *knl_gpu_as_priv( T_GPUAS *as )
{
	return ( as != NULL ) ? as->priv : NULL;
}

EXPORT ER knl_gpu_as_bind( T_GPUAS *as, T_GPUBO *bo, UW *p_va )
{
	if ( as == NULL || bo == NULL || p_va == NULL || gpu_ops == NULL ) return E_PAR;
	return gpu_ops->as_bind(gpu_priv, as->priv, bo, bo->npages, p_va);
}

EXPORT void knl_gpu_as_unbind( T_GPUAS *as, T_GPUBO *bo, UW va )
{
	if ( as == NULL || bo == NULL || va == 0 || gpu_ops == NULL ) return;
	gpu_ops->as_unbind(gpu_priv, as->priv, va, bo->npages);
}

#ifdef __x86_64__
EXPORT ER knl_gpu_as_bind_at( T_GPUAS *as, T_GPUBO *bo, UD va )
{
	if ( as == NULL || bo == NULL || va == 0 || gpu_ops == NULL ) return E_PAR;
	if ( gpu_ops->as_bind_at == NULL ) return E_NOSPT;
	return gpu_ops->as_bind_at(gpu_priv, as->priv, bo, bo->npages, va);
}

EXPORT void knl_gpu_as_unbind_at( T_GPUAS *as, T_GPUBO *bo, UD va )
{
	if ( as == NULL || bo == NULL || va == 0 || gpu_ops == NULL || gpu_ops->as_unbind_at == NULL ) return;
	gpu_ops->as_unbind_at(gpu_priv, as->priv, va, bo->npages);
}
#endif

/* ---------------------------------------------------------------- jobs */

EXPORT D knl_gpu_submit( CONST T_GPUJOB *job, T_GPUBO *CONST *bo, INT nbo )
{
	return knl_gpu_submit_as(job, bo, nbo, NULL);
}

EXPORT D knl_gpu_submit_as( CONST T_GPUJOB *job, T_GPUBO *CONST *bo, INT nbo, T_GPUAS *as )
{
	QJOB	*q;
	INT	i;
	UINT	imask;
	UINT	need;
	D	fence;

	if ( job == NULL || nbo < 0 || ( nbo > 0 && bo == NULL ) ) return E_PAR;
	switch ( job->kind ) {
	case GPU_JOB_NOP:	need = 0;		break;
	case GPU_JOB_CL:	need = GPU_FEAT_CL;	break;
	case GPU_JOB_TFU:	need = GPU_FEAT_TFU;	break;
	case GPU_JOB_CSD:	need = GPU_FEAT_CSD;	break;
	case GPU_JOB_CACHE:	need = GPU_FEAT_CACHE;	break;
#ifdef __x86_64__
	case GPU_JOB_BATCH:	need = GPU_FEAT_BATCH;	break;
#endif
	default:		return E_PAR;
	}
	if ( gpu_ops == NULL || gpu_flg <= 0 ) return E_NOEXS;
	if ( ( gpu_ops->feature & need ) != need ) return E_NOSPT;

	q = (QJOB *)Kmalloc(sizeof(QJOB) + sizeof(T_GPUBO *) * (UINT)nbo);
	if ( q == NULL ) return E_NOMEM;
	q->next = NULL;
	q->job = *job;
	q->boost = FALSE;
	q->as = as;
	q->nbo = nbo;
	if ( as != NULL ) knl_gpu_as_get(as);
	for ( i = 0; i < nbo; i++ ) {
		q->bo[i] = bo[i];
		knl_gpu_bo_get(bo[i]);
	}

	ISpinLock(&gpu_lock, &imask);
	if ( job->after < 0 || job->after > fence_next ) {
		ISpinUnlock(&gpu_lock, &imask);
		for ( i = 0; i < nbo; i++ ) knl_gpu_bo_put(bo[i]);
		knl_gpu_as_put(as);
		Kfree(q);
		return E_PAR;
	}
	fence = q->fence = ++fence_next;
	for ( i = 0; i < nbo; i++ ) bo[i]->busy++;
	if ( q_tail == NULL ) {
		q_head = q;
	} else {
		q_tail->next = q;
	}
	q_tail = q;
	ISpinUnlock(&gpu_lock, &imask);

	tk_set_flg(gpu_flg, FLG_JOB);
	return fence;
}

EXPORT void knl_gpu_job_end( ER result )
{
	end_result = result;
	tk_set_flg(gpu_flg, FLG_END);
}

/* Run one job on the backend and answer how it went */
LOCAL ER run_job( QJOB *q )
{
	UINT	ptn;
	ER	er;

	if ( q->job.kind == GPU_JOB_NOP ) {
		return E_OK;
	}
	tk_clr_flg(gpu_flg, ~FLG_END);
	end_result = E_OK;
	er = gpu_ops->kick(gpu_priv, &q->job, ( q->as != NULL ) ? q->as->priv : NULL);
	if ( er < E_OK ) {
		return er;
	}
	er = tk_wai_flg_u(gpu_flg, FLG_END, TWF_ORW | TWF_BITCLR, &ptn, GPU_JOB_TMO_US);
	if ( er == E_TMOUT ) {
		tm_printf((UB *)"gpu: job of fence %ld did not end; resetting the GPU\n", q->fence);
		gpu_ops->reset(gpu_priv);
		return E_IO;
	}
	return ( er < E_OK ) ? er : end_result;
}

LOCAL void gpu_task( INT stacd, void *exinf )
{
	for (;;) {
		QJOB	*q;
		UINT	ptn, imask;
		INT	i;
		BOOL	room;
		ER	er;

		tk_wai_flg(gpu_flg, FLG_JOB, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
		for (;;) {
			ISpinLock(&gpu_lock, &imask);
			q = q_head;
			if ( q != NULL ) {
				q_head = q->next;
				if ( q_head == NULL ) q_tail = NULL;
				q_running = TRUE;
				fence_run = q->fence;
			}
			ISpinUnlock(&gpu_lock, &imask);
			if ( q == NULL ) break;

			er = run_job(q);
			room = ( er < E_OK ) ? failed_room() : FALSE;

			ISpinLock(&gpu_lock, &imask);
			if ( er < E_OK ) {
				if ( nfailed > 0 && failed[nfailed - 1].last + 1 == q->fence ) {
					failed[nfailed - 1].last = q->fence;
				} else if ( room ) {
					failed[nfailed].first = failed[nfailed].last = q->fence;
					nfailed++;
				}
			}
			for ( i = 0; i < q->nbo; i++ ) q->bo[i]->busy--;
			ISpinUnlock(&gpu_lock, &imask);

			/*
			 * The job's hold on its buffers and its address space is
			 * given back before the fence is said to be over: one
			 * who waited for the fence may then count on a buffer it
			 * let go of being gone.
			 */
			for ( i = 0; i < q->nbo; i++ ) knl_gpu_bo_put(q->bo[i]);
			knl_gpu_as_put(q->as);

			/* a boosted job over: its part of the boost given back before anyone sees it over */
			if ( q->boost && gpu_ops->boost != NULL ) gpu_ops->boost(gpu_priv, -1);

			ISpinLock(&gpu_lock, &imask);
			fence_done = q->fence;
			q_running = FALSE;
			ISpinUnlock(&gpu_lock, &imask);

			Kfree(q);
			tk_set_flg(gpu_flg, FLG_DONE);
			if ( knl_gpu_end_hook != NULL ) {
				knl_gpu_end_hook();
			}
		}
	}
}

/* ---------------------------------------------------------------- the layer */

EXPORT ER knl_gpu_init( void )
{
	T_CFLG	cflg;
	T_CTSK	ctsk;

	if ( gpu_tsk > 0 ) {
		return E_OK;
	}
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	gpu_flg = tk_cre_flg(&cflg);
	if ( gpu_flg <= 0 ) {
		return E_LIMIT;
	}
	ctsk.exinf = NULL;
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)gpu_task;
	ctsk.itskpri = GPU_TASK_PRI;
	ctsk.stksz = GPU_TASK_STKSZ;
	gpu_tsk = tk_cre_tsk(&ctsk);
	if ( gpu_tsk <= 0 || tk_sta_tsk(gpu_tsk, 0) < E_OK ) {
		return E_LIMIT;
	}
	return E_OK;
}

EXPORT ER knl_gpu_register( CONST T_GPUOPS *ops, void *priv )
{
	UINT	imask;
	ER	er;

	er = knl_gpu_init();
	if ( er < E_OK ) {
		return er;
	}
	ISpinLock(&gpu_lock, &imask);
	if ( gpu_ops != NULL ) {
		ISpinUnlock(&gpu_lock, &imask);
		return E_OBJ;
	}
	gpu_priv = priv;
	gpu_ops = ops;
	ISpinUnlock(&gpu_lock, &imask);
	knl_gpu_obj_told();		/* told after it is in: a waiter that looks again finds it */
	return E_OK;
}

EXPORT ER knl_gpu_unregister( void )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	if ( q_head != NULL || q_running || bound_bos != 0 || live_as != 0 ) {
		ISpinUnlock(&gpu_lock, &imask);
		return E_BUSY;
	}
	gpu_ops = NULL;
	gpu_priv = NULL;
	ISpinUnlock(&gpu_lock, &imask);
	knl_gpu_obj_told();
	return E_OK;
}

EXPORT ER knl_gpu_ref( T_GPUINFO *info )
{
	if ( gpu_ops == NULL ) {
		return E_NOEXS;
	}
	knl_memset(info, 0, sizeof(T_GPUINFO));
	return gpu_ops->info(gpu_priv, info);
}
