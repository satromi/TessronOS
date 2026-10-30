/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	host_rplan.c (host)
 *	The kernel's plan of compositing on Intel's render engine
 *	(device/gpu/intel/igt_rplan.c) called with the plain types of
 *	hr.h, for the host test of the shaders, which is built with Mesa's
 *	headers and does not see the kernel's.
 */

#include "kernel.h"
#include <ts/dp.h>
#include "../../../../outer_kernel/wm/compose.h"
#include "../../../../device/gpu/intel/igt_rplan.h"
#include "hr.h"

LOCAL T_DPRECT rect_of( HC_RECT r )
{
	T_DPRECT	d;

	d.left = r.left;
	d.top = r.top;
	d.right = r.right;
	d.bottom = r.bottom;
	return d;
}

LOCAL void win_of( CONST HC_WIN *h, T_CPWIN *w )
{
	knl_memset(w, 0, sizeof(*w));
	w->outer = rect_of(h->outer);
	w->work = rect_of(h->work);
	w->band = rect_of(h->band);
	w->pixels = h->pixels;
	w->pitch = h->pitch;
	w->a_frame = h->a_frame;
	w->a_work = h->a_work;
	w->keyed = (BOOL)h->keyed;
	w->round = h->round;
	w->shape = NULL;
	w->wash = h->wash;
	w->wash_str = h->wash_str;
	w->bar_wash = h->bar_wash;
	w->bar_str = h->bar_str;
	w->tint = h->tint;
	w->tint_str = h->tint_str;
	w->blur = h->blur;
	w->shd_dx = h->shd_dx;
	w->shd_dy = h->shd_dy;
	w->shd_size = h->shd_size;
	w->shd_dark = h->shd_dark;
	w->shown = rect_of(h->shown);
	w->fade = h->fade;
}

LOCAL HR_GPU	*gpu_fn;

LOCAL UD gpu_of( void *ctx, CONST void *p, UD len )
{
	(void)ctx;
	return gpu_fn(p, len);
}

int hr_plan( uint8_t *back, unsigned pitch, HC_RECT box, int rows, const uint32_t *ground, unsigned gpitch,
	     const HC_WIN *hw, int n, HR_GPU *gpu, uint64_t t1, uint64_t t2, const uint64_t *cov, HR_RD *out, int max,
	     int *p_nrd )
{
	static T_CPWIN	w[CP_LAY_MAX];
	static IGT_RD	rd[IGT_RD_MAX];
	T_CPDST		d;
	IGT_RENV	env;
	INT		i, k, r;

	if ( n > CP_LAY_MAX ) n = CP_LAY_MAX;
	if ( max > IGT_RD_MAX ) max = IGT_RD_MAX;
	d.back = back;
	d.pitch = pitch;
	d.box = rect_of(box);
	d.rows = rows;
	for ( i = 0; i < n; i++ ) win_of(&hw[i], &w[i]);
	knl_memset(&env, 0, sizeof(env));
	gpu_fn = gpu;
	env.gpu = gpu_of;
	env.t1 = t1;
	env.t2 = t2;
	for ( i = 0; i <= CP_ROUND_MAX; i++ ) env.cov[i] = cov[i];
	r = igt_rplan(&d, ground, gpitch, w, n, &env, rd, max, p_nrd);
	for ( i = 0; i < *p_nrd; i++ ) {
		out[i].fs = rd[i].fs;
		out[i].barrier = rd[i].barrier;
		out[i].l = rd[i].l;
		out[i].t = rd[i].t;
		out[i].r = rd[i].r;
		out[i].b = rd[i].b;
		for ( k = 0; k < IGT_CP_NPUSH; k++ ) out[i].par[k] = rd[i].par[k];
	}
	return r;
}
