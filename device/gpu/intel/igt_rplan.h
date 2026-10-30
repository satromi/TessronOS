/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_rplan.h
 *	The plan of compositing on the render engine (igt_rplan.c): the
 *	rectangles each shader of mesa/igt_cp_shaders.h is run over, in
 *	order, with their push constants (docs/tessronos/13-gpu.md 13.11.14).
 *	Built into the kernel and, with tools/mesa/hostshim, into the host
 *	test of the shaders (tools/x64/mesa/test_igt_shaders.c).
 */

#ifndef _DEVICE_GPU_INTEL_IGT_RPLAN_H_
#define _DEVICE_GPU_INTEL_IGT_RPLAN_H_

#include "../../../outer_kernel/wm/compose.h"
#include "mesa/igt_cp_shaders.h"

#define IGT_RD_MAX	2048		/* rectangles of a part */

/*
 * One rectangle: a shader run over it, a thread group for every 16
 * pixels of a row from its left edge rounded down to 16. 'barrier': what
 * the rectangles before it wrote is to be finished and seen first.
 */
typedef struct {
	UB	fs;			/* CP_FS_* */
	UB	barrier;
	INT	l, t, r, b;		/* in the pixels of its destination */
	UW	par[IGT_CP_NPUSH];	/* its push constants */
} IGT_RD;

/* What the plan needs of where memory is in the GPU's space */
typedef struct {
	/* the GPU address of 'len' bytes of the kernel's memory from p; 0 when they cannot be entered */
	UD	(*gpu)( void *ctx, CONST void *p, UD len );
	void	*ctx;
	UD	t1, t2;				/* the levels of the spread, cp_spread_bytes each; 0 none */
	UD	cov[CP_ROUND_MAX + 1];		/* each radius's cover of the corners; [0] memory any may read */
} IGT_RENV;

/*
 * The rectangles that lay the ground of the part d->box and windows
 * w[0..n-1] from the back, up to the first the shaders do not lay (an
 * outline of its own, a key, pixels that cannot be entered; one that
 * spreads what shows through without the levels), into rd (max of them):
 * how many windows they lay, the number of rectangles in *p_nrd; E_LIMIT
 * when more than max are needed, E_NOMEM when the part's memory cannot be
 * entered
 */
IMPORT INT igt_rplan( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n,
		      CONST IGT_RENV *env, IGT_RD *rd, INT max, INT *p_nrd );

/* Thread groups of a rectangle across and down */
Inline UW igt_rd_groups_x( CONST IGT_RD *d )
{
	return (UW)( ( d->r - ( d->l & ~( IGT_CP_LANES - 1 ) ) + IGT_CP_LANES - 1 ) / IGT_CP_LANES );
}

Inline UW igt_rd_groups_y( CONST IGT_RD *d )
{
	return (UW)( d->b - d->t );
}

#endif /* _DEVICE_GPU_INTEL_IGT_RPLAN_H_ */
