/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_cp.h
 *	The plan of compositing on the copy engine (igt_compose.c): the
 *	copies between the kernel's memory that lay a part, which the tests
 *	on a machine without the GPU check against the processor's stages;
 *	and what igt_compose.c and the render engine's compositing
 *	(igt_rcompose.c) call of each other.
 */

#ifndef _DEVICE_GPU_INTEL_IGT_CP_H_
#define _DEVICE_GPU_INTEL_IGT_CP_H_

#include <ts/gpu.h>
#include "../../../outer_kernel/wm/compose.h"

#define IGT_CP_MAXOPS	( CP_GPU_MAX + 1 )	/* the ground and a copy a window */

/* w x h pixels of 32 bits from src (rows spitch bytes apart) to dst */
typedef struct {
	CONST UB	*src;
	UINT		spitch;
	UB		*dst;
	UINT		dpitch;
	INT		w, h;
} CP_OP;

/* The memory of the parts in the GPU's space (igt_compose.c), for the render engine's plan too */
IMPORT UW	igt_cp_wrap( CONST void *p, UD len, T_GPUBO **p_bo );
IMPORT void	igt_cp_new_part( void );

/* The render engine (igt_rcompose.c) */
IMPORT ER	igt_rc_init( void );
IMPORT INT	igt_rc_lay( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n, BOOL flush );
IMPORT void	igt_rc_last( INT *p_nrd, INT *p_ngroup );

IMPORT BOOL	igt_cp_plain( CONST T_CPWIN *w );
IMPORT INT	igt_cp_plan( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n,
			     CP_OP *ops, INT max, INT *p_nops );
IMPORT void	igt_cp_run_cpu( CONST CP_OP *ops, INT nops );

#endif /* _DEVICE_GPU_INTEL_IGT_CP_H_ */
