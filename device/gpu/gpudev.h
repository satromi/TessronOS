/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpudev.h
 *	What a GPU backend gives the common layer (device/gpu/gpu.c) and
 *	what it may call back (docs/tessronos/13-gpu.md 13.4).
 *
 *	The common layer runs one job at a time: it starts a job with
 *	kick() and waits until the backend says it is over with
 *	knl_gpu_job_end(), which may be called from an interrupt handler or
 *	from within kick() itself. A job that is not over in GPU_JOB_TMO_US
 *	has the GPU reset() and its fence marked as failed.
 *
 *	A backend may keep address spaces of its own besides the kernel's
 *	(as_create and the rest): a page table that holds only the BOs
 *	entered in it. A job submitted with one (knl_gpu_submit_as) is
 *	kicked with it, and the backend runs the job with that table in its
 *	MMU, so that the job reaches nothing else. A process's jobs are
 *	run so (13-gpu.md 13.5). A backend without them leaves the four
 *	NULL, and the process's BOs go into the one space there is.
 */

#ifndef _DEVICE_GPU_GPUDEV_H_
#define _DEVICE_GPU_GPUDEV_H_

#include <ts/gpu.h>

#define GPU_JOB_TMO_US		(2 * 1000 * 1000)

typedef struct {
	UINT	kind;			/* GPU_KIND_* */
	UINT	feature;		/* GPU_FEAT_* the backend runs */
	/*
	 * Pages the GPU can reach lie below this physical address
	 * (0: anywhere). A limit of 4GB or less takes BO pages from the
	 * 32 bit zone.
	 */
	UD	pa_limit;

	ER	(*info)( void *priv, T_GPUINFO *info );

	/*
	 * Enter the 'npages' pages of a new BO (knl_gpu_bo_page) into the
	 * GPU's MMU and answer the GPU address they start at. unbind
	 * takes them out again once the BO is gone.
	 */
	ER	(*bo_bind)( void *priv, T_GPUBO *bo, UD npages, UW *p_offset );
	void	(*bo_unbind)( void *priv, UW offset, UD npages );

	/*
	 * Start a job, in the address space 'as' (as_create's), or in the
	 * kernel's when it is NULL. Its end is told with knl_gpu_job_end()
	 */
	ER	(*kick)( void *priv, CONST T_GPUJOB *job, void *as );

	/* A job ran out of time: stop the GPU and make it ready again */
	void	(*reset)( void *priv );

	/*
	 * An address space of the backend's own, with nothing in it: E_NOSPT
	 * when the backend cannot or must not give one (its own check at
	 * start failed). Destroyed when no job uses it any more and every
	 * BO has been taken out of it.
	 */
	ER	(*as_create)( void *priv, void **p_as );
	void	(*as_destroy)( void *priv, void *as );

	/* A BO's pages entered in it at a new address, and taken out again */
	ER	(*as_bind)( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va );
	void	(*as_unbind)( void *priv, void *as, UW va, UD npages );
#ifdef __x86_64__
	/*
	 * A BO's pages entered in a space at the address given (the
	 * space's user chose it), and taken out from there. NULL when the
	 * backend's spaces have no such place.
	 */
	ER	(*as_bind_at)( void *priv, void *as, T_GPUBO *bo, UD npages, UD va );
	void	(*as_unbind_at)( void *priv, void *as, UD va, UD npages );
	/*
	 * The GPU sees memory through the processor's caches: processes
	 * map BOs cached
	 */
	BOOL	coherent;
#endif
} T_GPUOPS;

/* The backend's own address space of one of the common layer's */
IMPORT void	*knl_gpu_as_priv( T_GPUAS *as );

/*
 * Put a backend in. E_OBJ when one is in already. Starts the common
 * layer if it is not running.
 */
IMPORT ER	knl_gpu_register( CONST T_GPUOPS *ops, void *priv );

/*
 * Take it out again, which only a test does: E_BUSY while a job is
 * queued or running, or a BO it holds in its MMU is still alive.
 */
IMPORT ER	knl_gpu_unregister( void );

/* The job started last is over, with this result (E_OK or an error) */
IMPORT void	knl_gpu_job_end( ER result );

/*
 * A BO over pages that are someone else's (knl_gpu_bo_wrap): their
 * physical addresses, page aligned and within the backend's reach. The
 * pages are not freed with the BO.
 */
#define GPU_BO_WRAPPED		0x8000
IMPORT T_GPUBO	*knl_gpu_bo_wrap( CONST UD *pa, UD npages, ER *p_er );

/* Pages of a BO, and the physical address of each */
IMPORT UD	knl_gpu_bo_npages( T_GPUBO *bo );
IMPORT CONST UD	*knl_gpu_bo_pages( T_GPUBO *bo );

/*
 * Make memory the processor wrote through its caches visible to a
 * device that does not look at them, and drop the lines so that what
 * the device writes is read from memory. 'va' is a kernel address.
 */
IMPORT void	knl_gpu_dcache_clean_inval( UBINT va, UBINT len );

#endif /* _DEVICE_GPU_GPUDEV_H_ */
