/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpu.h
 *	The GPU layer (docs/tessronos/13-gpu.md).
 *
 *	One GPU at a time is driven by a backend (the V3D of the Raspberry
 *	Pi 5, later virtio-gpu on the emulated machine) under a common layer
 *	that owns what does not depend on the hardware: buffer objects,
 *	the order in which jobs run, and the fences that say a job is over.
 *
 *	A buffer object (BO) is memory the GPU reads and writes through its
 *	own MMU. Its pages need not be physically contiguous. The GPU sees
 *	it at a 32 bit GPU address; a process sees it at an address of the
 *	shared window, the same in every process that maps it, so a buffer
 *	drawn by one process can be read by another (the window system)
 *	without a copy.
 *
 *	A job is a unit of work for the GPU. It names the BOs it uses, so
 *	that they stay alive until it is over, and may name a fence it has
 *	to wait for before it starts. Submitting it answers the fence that
 *	is signalled when it is over. Fences are numbers that only grow:
 *	fence n being over means every job before it is over too.
 *
 *	The job descriptions carry the same fields as the requests the
 *	user side's V3D driver makes of a kernel, so that the user side
 *	translates its calls one to one instead of being rewritten.
 */

#ifndef __TS_GPU_H__
#define __TS_GPU_H__

#ifdef __cplusplus
extern "C" {
#endif

/* What drives the GPU */
#define GPU_KIND_NONE		0
#define GPU_KIND_V3D		1	/* Broadcom V3D (Raspberry Pi 5: V3D 7.1) */
#define GPU_KIND_VIRTIO		2	/* virtio-gpu with virgl (the emulated machine; planned) */
#define GPU_KIND_TEST		15	/* a backend the in-kernel tests put in */

/* What the GPU can run (T_GPUINFO.feature) */
#define GPU_FEAT_CL		0x0001	/* binner and renderer control lists */
#define GPU_FEAT_TFU		0x0002	/* texture formatting unit */
#define GPU_FEAT_CSD		0x0004	/* compute shader dispatch */
#define GPU_FEAT_CACHE		0x0008	/* cache clean jobs */
#define GPU_FEAT_MMU		0x0010	/* its own MMU: BOs need not be contiguous */

typedef struct {
	UINT	kind;		/* GPU_KIND_* */
	UINT	ver;		/* V3D: major * 10 + minor (71 for 7.1) */
	UINT	rev;
	UINT	ncores;
	UINT	nqpu;		/* shader processors in all */
	UINT	feature;	/* GPU_FEAT_* */
	UD	va_size;	/* bytes of GPU address space */
	UW	hub_ident[4];	/* V3D: the identification registers as read */
	UW	core_ident[3];
} T_GPUINFO;

/* ------------------------------------------------------------------------ */
/*
 * Buffer objects
 */
#define GPU_BO_MAXSIZE		(512UL * 1024 * 1024)	/* one BO */

/* Flags of knl_gpu_bo_create */
#define GPU_BO_NOGPU		0x0001	/* not entered in the GPU's MMU (tests, staging) */

typedef struct gpubo	T_GPUBO;

typedef struct {
	SZ	size;		/* bytes, a whole number of pages */
	UW	offset;		/* GPU address (0: not in the GPU's MMU) */
	UBINT	uva;		/* where a process sees it (shared window), 0 before the first map */
	UINT	refcnt;
	UINT	busy;		/* jobs that use it and are not over */
} T_GPUBOINFO;

/* ------------------------------------------------------------------------ */
/*
 * Jobs
 */
#define GPU_JOB_NOP		0	/* nothing: a fence that follows the jobs before it */
#define GPU_JOB_CL		1	/* binner then renderer (T_GPUCL) */
#define GPU_JOB_TFU		2	/* texture formatting (T_GPUTFU) */
#define GPU_JOB_CSD		3	/* compute dispatch (T_GPUCSD) */
#define GPU_JOB_CACHE		4	/* clean the GPU's caches to memory */

/* T_GPUCL.flags */
#define GPU_CL_FLUSH_CACHE	0x0001	/* clean the caches when the renderer is done */

/*
 * A binner control list and a renderer control list. The binner runs
 * first and writes the tile lists into the tile allocation memory; the
 * renderer then walks them. All addresses are GPU addresses.
 */
typedef struct {
	UW	bcl_start;	/* binner control list */
	UW	bcl_end;	/* first byte past it */
	UW	rcl_start;	/* renderer control list */
	UW	rcl_end;
	UW	qma;		/* tile allocation memory */
	UW	qms;		/* its size */
	UW	qts;		/* tile state data (0: the list sets it) */
	UW	flags;		/* GPU_CL_* */
} T_GPUCL;

/* The texture formatting unit's registers, as the user side computed them */
typedef struct {
	UW	icfg;
	UW	iia;
	UW	iis;
	UW	ica;
	UW	iua;
	UW	ioa;
	UW	ioc;		/* 7.1 and later */
	UW	ios;
	UW	coef[4];
} T_GPUTFU;

/* One compute dispatch: the queued configuration registers */
typedef struct {
	UW	cfg[7];
	UW	coef[4];
} T_GPUCSD;

typedef struct {
	UINT	kind;		/* GPU_JOB_* */
	D	after;		/* fence to wait for before it starts; 0: none */
	union {
		T_GPUCL		cl;
		T_GPUTFU	tfu;
		T_GPUCSD	csd;
	} u;
} T_GPUJOB;

/* ------------------------------------------------------------------------ */
/*
 * The kernel's interface. Processes are to reach the same through SVC
 * class 10 (gp_, not yet in svc.h; 13-gpu.md 13.5), which keeps a table of the BOs
 * each process holds and gives the process numbers instead of pointers.
 */

/* The common layer: its task and objects. Answers E_OK once it runs */
IMPORT ER	knl_gpu_init( void );

/*
 * The V3D of the Raspberry Pi 5. Powers it, reads what it is, sets its
 * MMU and interrupts and registers it as the backend. Answers E_NOSPT
 * when the build did not ask for it (CNF_V3D) or the machine has none.
 */
IMPORT ER	knl_v3d_init( void );

/* What the GPU is. E_NOEXS when no backend is registered */
IMPORT ER	knl_gpu_ref( T_GPUINFO *info );

/*
 * A BO of at least 'size' bytes, zeroed. It holds one reference, which
 * knl_gpu_bo_put gives back; the memory goes when the last reference
 * does and no job uses it any more.
 */
IMPORT T_GPUBO	*knl_gpu_bo_create( SZ size, UINT flags, ER *p_er );
IMPORT void	knl_gpu_bo_get( T_GPUBO *bo );
IMPORT void	knl_gpu_bo_put( T_GPUBO *bo );
IMPORT ER	knl_gpu_bo_ref( T_GPUBO *bo, T_GPUBOINFO *info );

/* Physical address of page 'i' of the BO, for the backends and the tests */
IMPORT UD	knl_gpu_bo_page( T_GPUBO *bo, UD i );

/*
 * Map the BO into a process space (sysman/space.h T_SPACE) at its
 * address in the shared window, uncached, readable and writable, never
 * executable. The address is chosen on the first map and kept.
 */
IMPORT ER	knl_gpu_bo_map( T_GPUBO *bo, void *space, UBINT *p_va );
IMPORT ER	knl_gpu_bo_unmap( T_GPUBO *bo, void *space );

/*
 * Submit a job that uses the 'nbo' BOs in 'bo'. Answers the fence that
 * is signalled when it is over (> 0), or an error.
 */
IMPORT D	knl_gpu_submit( CONST T_GPUJOB *job, T_GPUBO *CONST *bo, INT nbo );

/*
 * Wait until 'fence' is over. E_OK when it went well, E_IO when the job
 * failed or the GPU had to be reset, E_TMOUT when time ran out.
 */
IMPORT ER	knl_gpu_wait( D fence, TMO_U tmout );

/* The last fence that is over */
IMPORT D	knl_gpu_done( void );

/* Wait until no job uses the BO */
IMPORT ER	knl_gpu_bo_wait( T_GPUBO *bo, TMO_U tmout );

#ifdef __cplusplus
}
#endif

#endif /* __TS_GPU_H__ */
