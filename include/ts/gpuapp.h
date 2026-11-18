/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpuapp.h
 *	The GPU for a program running as a process (docs/tessronos/13-gpu.md
 *	13.5): SVC class 10, gp_.
 *
 *	The calls follow the requests a user side DRM driver makes of its
 *	kernel one to one, so that the DRM shim of OpenGL ES (lib/libgl/
 *	gl_drm.c) only renames them. A process opens a context; the BOs it
 *	makes there, or takes in from another process, have numbers in that
 *	context, and only those numbers name them in later calls -- a
 *	number of another context, or one already deleted, is E_ID. A BO is
 *	mapped into the process at its address of the shared window, the
 *	same in every process that maps it, uncached. A job names the BOs
 *	it uses by their numbers and is answered its fence; the BOs stay
 *	until it is over, even when the process deletes them or ends.
 *
 *	When the process ends, its contexts are closed: every BO it held is
 *	let go of, the mappings with it, and the names it shared with
 *	gp_shr_bo go too. There is no fixed number of contexts or of names:
 *	the kernel's tables grow as they are used.
 *
 *	Errors: E_NOEXS when there is no GPU (gp_ref_gpu, gp_sub_job), E_ID
 *	for a context or a number that is not the caller's, E_LIMIT when a
 *	context holds as many BOs as it may, E_NOMEM when the kernel has no
 *	memory for another context, number or name, E_IO when a job failed
 *	or the GPU had to be reset, E_TMOUT when a wait ran out of time.
 */

#ifndef __TS_GPUAPP_H__
#define __TS_GPUAPP_H__

#include <tk/typedef.h>
#include <ts/gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GP_JOB_BO_MAX	1024		/* BOs one job names */

typedef struct {
	UINT	handle;		/* the BO's number in the context */
	UW	offset;		/* its GPU address (0: not in the GPU's MMU) */
	SZ	size;		/* bytes, a whole number of pages */
} T_GPBO;

/* A context of the calling process: its number (> 0) */
IMPORT INT gp_opn_gpu( void );
IMPORT ER  gp_cls_gpu( INT ctx );

/* What the GPU is; E_NOEXS when the machine has none */
IMPORT ER  gp_ref_gpu( T_GPUINFO *info );

/*
 * Knowing that a GPU has been registered, when it comes after the
 * program started (the Intel GPU of an x64 PC is ready some time after
 * the boot). The GPU is a device object, "gpu0" (OB_S_GPU), found at
 * OB_UUID_GPU_INIT on every machine, GPU or not; its record OB_GP_STATE
 * (include/ts/ob.h) is "key value" lines whose first, BACKEND, is
 * "registered" or "none". OB_E_CHANGE of that record is told whenever
 * the backend comes or goes and as the backend's own lines change, so a
 * notice means "look again", not "registered". In an order that loses
 * nothing:
 *
 *	key = ob_opn_obj(&gpu, OB_OP_ATRRD);
 *	ob_ntf_evt(key, OB_GP_STATE, &req, port);	req.events = OB_E_CHANGE
 *	if ( gp_ref_gpu(&info) >= E_OK ) -> the GPU is there
 *	else wait for a notice at port, and ask gp_ref_gpu again
 *
 * (a program that opens it after the registration need not wait: the
 * first gp_ref_gpu already answers).
 */
#define GPU_OBJ_NAME		"gpu0"
#define OB_UUID_GPU_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xad, 0x7e, 0x2b, \
				    0x91, 0x4c, 0x07, 0xd5, 0xa3, 0x68, 0xf1, 0x2c } }

/* A BO of at least size bytes, zeroed, in the context (flags: GPU_BO_*) */
IMPORT ER  gp_cre_bo( INT ctx, SZ size, UINT flags, T_GPBO *bo );
IMPORT ER  gp_del_bo( INT ctx, UINT handle );
IMPORT ER  gp_ref_bo( INT ctx, UINT handle, T_GPBO *bo );

/* The BO mapped into the process; where it is (the same each time) */
IMPORT ER  gp_map_bo( INT ctx, UINT handle, void **addr );

/*
 * A job (include/ts/gpu.h T_GPUJOB) that uses the n BOs named; its fence
 * (> 0) or an error. job->after is a fence it waits for, or 0.
 */
IMPORT D   gp_sub_job( INT ctx, CONST T_GPUJOB *job, CONST UINT *handles, INT n );

/* A fence over; the jobs that use a BO over */
IMPORT ER  gp_wai_fen( INT ctx, D fence, TMO_U tmout );
IMPORT ER  gp_wai_bo( INT ctx, UINT handle, TMO_U tmout );

/*
 * A BO given a name (> 0) another process takes it in by, with its own
 * number. The name holds while the number it was made from does.
 */
IMPORT INT gp_shr_bo( INT ctx, UINT handle );
IMPORT ER  gp_imp_bo( INT ctx, INT name, T_GPBO *bo );

/*
 * A BO as a real object (OB_T_STORAGE, OB_S_GPUBO of include/ts/ob.h):
 * ob_cre_obj with arg a T_OBGBMAKE, which names a BO of the caller's by
 * its context and number, or by a name of gp_shr_bo (name above 0). The
 * object holds the BO for as long as a key to it is open, whoever holds
 * the key, so a key carried by a stream (OB_ST_MSG) takes the BO to
 * another process without a name. Never opened, it goes when the process
 * that made it ends. Record OB_GB_IMPORT read with a T_OBGBIMP enters the
 * BO in the reader's context ctx under a number of its own, as gp_imp_bo
 * does, and answers it in bo. The protection is the maker's, rw-------.
 */
#define OB_GB_IMPORT	1
#define OB_GB_NREC	2

typedef struct {
	INT	ctx;
	UINT	handle;
	INT	name;			/* above 0: the BO of this name instead */
} T_OBGBMAKE;

typedef struct {
	INT	ctx;			/* the reader's context */
	T_GPBO	bo;			/* out */
} T_OBGBIMP;

#ifdef __x86_64__
/*
 * The BO placed at 'va' (48 bits, page aligned; the upper bits of the
 * canonical form are let go of) in the context's space, and taken out
 * of where it was. For a GPU whose user side driver chooses its
 * addresses (Intel); E_NOSPT where the GPU chooses them. The BO's job
 * reaches it there once placed; deleting the number takes it out.
 */
IMPORT ER  gp_bnd_bo( INT ctx, UINT handle, UD va );
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TS_GPUAPP_H__ */
