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

	/* Start a job. Its end is told with knl_gpu_job_end() */
	ER	(*kick)( void *priv, CONST T_GPUJOB *job );

	/* A job ran out of time: stop the GPU and make it ready again */
	void	(*reset)( void *priv );
} T_GPUOPS;

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
