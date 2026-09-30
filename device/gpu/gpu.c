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
 *	A waiter clears the "a fence is over" bit, looks at the last fence
 *	that is over, and only then waits for the bit. The task sets the
 *	number before the bit, so a fence that ends between the look and the
 *	wait still wakes the waiter. Every waiter looks again after it
 *	wakes, so one waiter clearing the bit cannot make another miss it.
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

#define NFAILED			16	/* failed fences remembered */

/* A process's view of a BO: uncached, read and write, never executed */
#define PTE_PAGE_UDATA_NC	(PTE_PAGE | PTE_ATTR(MAIR_IDX_NORMAL_NC) | PTE_AP_RW_ALL | \
				 PTE_SH_INNER | PTE_AF | PTE_nG | PTE_PXN | PTE_UXN)

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

typedef struct qjob {
	struct qjob	*next;
	D		fence;
	T_GPUJOB	job;
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
LOCAL D			fence_next;	/* last fence handed out */
LOCAL volatile D	fence_done;	/* last fence that is over */
LOCAL D			failed[NFAILED];
LOCAL INT		failed_at;
LOCAL volatile ER	end_result;	/* what the backend said of the running job */
LOCAL UINT		bound_bos;	/* live BOs in the backend's MMU */

/* ---------------------------------------------------------------- caches */

EXPORT void knl_gpu_dcache_clean_inval( UBINT va, UBINT len )
{
	UBINT	end = va + len;

	Asm("dsb sy" ::: "memory");
	for ( va &= ~63ULL; va < end; va += 64 ) {
		Asm("dc civac, %0" :: "r"(va) : "memory");
	}
	Asm("dsb sy" ::: "memory");
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
	for ( i = 0; i < bo->npages; i++ ) {
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
				PTE_PAGE_UDATA_NC) != E_OK ) {
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

/* ---------------------------------------------------------------- fences */

LOCAL BOOL fence_failed( D fence )
{
	INT	i;

	for ( i = 0; i < NFAILED; i++ ) {
		if ( failed[i] == fence ) return TRUE;
	}
	return FALSE;
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

EXPORT ER knl_gpu_wait( D fence, TMO_U tmout )
{
	ER	er;

	if ( fence <= 0 || fence > fence_next || gpu_flg <= 0 ) return E_PAR;
	er = wait_until(fence_reached, &fence, tmout);
	if ( er < E_OK ) return er;
	return fence_failed(fence) ? E_IO : E_OK;
}

LOCAL BOOL bo_idle( void *arg )
{
	return (BOOL)( ((T_GPUBO *)arg)->busy == 0 );
}

EXPORT ER knl_gpu_bo_wait( T_GPUBO *bo, TMO_U tmout )
{
	if ( gpu_flg <= 0 ) return ( bo->busy == 0 ) ? E_OK : E_SYS;
	return wait_until(bo_idle, bo, tmout);
}

/* ---------------------------------------------------------------- jobs */

EXPORT D knl_gpu_submit( CONST T_GPUJOB *job, T_GPUBO *CONST *bo, INT nbo )
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
	default:		return E_PAR;
	}
	if ( gpu_ops == NULL || gpu_flg <= 0 ) return E_NOEXS;
	if ( ( gpu_ops->feature & need ) != need ) return E_NOSPT;

	q = (QJOB *)Kmalloc(sizeof(QJOB) + sizeof(T_GPUBO *) * (UINT)nbo);
	if ( q == NULL ) return E_NOMEM;
	q->next = NULL;
	q->job = *job;
	q->nbo = nbo;
	for ( i = 0; i < nbo; i++ ) {
		q->bo[i] = bo[i];
		knl_gpu_bo_get(bo[i]);
	}

	ISpinLock(&gpu_lock, &imask);
	if ( job->after < 0 || job->after > fence_next ) {
		ISpinUnlock(&gpu_lock, &imask);
		for ( i = 0; i < nbo; i++ ) knl_gpu_bo_put(bo[i]);
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
	er = gpu_ops->kick(gpu_priv, &q->job);
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
		ER	er;

		tk_wai_flg(gpu_flg, FLG_JOB, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
		for (;;) {
			ISpinLock(&gpu_lock, &imask);
			q = q_head;
			if ( q != NULL ) {
				q_head = q->next;
				if ( q_head == NULL ) q_tail = NULL;
				q_running = TRUE;
			}
			ISpinUnlock(&gpu_lock, &imask);
			if ( q == NULL ) break;

			er = run_job(q);

			ISpinLock(&gpu_lock, &imask);
			if ( er < E_OK ) {
				failed[failed_at] = q->fence;
				failed_at = ( failed_at + 1 ) % NFAILED;
			}
			for ( i = 0; i < q->nbo; i++ ) q->bo[i]->busy--;
			fence_done = q->fence;
			q_running = FALSE;
			ISpinUnlock(&gpu_lock, &imask);

			for ( i = 0; i < q->nbo; i++ ) knl_gpu_bo_put(q->bo[i]);
			Kfree(q);
			tk_set_flg(gpu_flg, FLG_DONE);
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
	return E_OK;
}

EXPORT ER knl_gpu_unregister( void )
{
	UINT	imask;

	ISpinLock(&gpu_lock, &imask);
	if ( q_head != NULL || q_running || bound_bos != 0 ) {
		ISpinUnlock(&gpu_lock, &imask);
		return E_BUSY;
	}
	gpu_ops = NULL;
	gpu_priv = NULL;
	ISpinUnlock(&gpu_lock, &imask);
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
