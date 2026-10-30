/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_rplan.c
 *	The plan of compositing on the render engine (docs/tessronos/
 *	13-gpu.md 13.11.14): a part of the screen as rectangles, each a
 *	compute shader of mesa/igt_cp_shaders.h run over it, in the order
 *	compose.c lays the stages.
 *
 *	  1. The ground: copied from the ground's memory over the part.
 *	  2. Each window from the back: its shadow, in the rectangles round
 *	     it and in its round corners (v3d_cp_split.h cp_split_shadow);
 *	     what shows through it spread, when it spreads (a rectangle for
 *	     each piece at each step: halved level by level into T1,
 *	     smoothed at the deepest into T2 below the screen's rows, doubled
 *	     back into T1 and the last into T2, as compose.c's
 *	     cp_spread_plan works it out); its pieces mixed over what is
 *	     behind, and where a piece is solid and plain its pixels copied
 *	     (cp_split_piece). A window opening or closing is one rectangle
 *	     of 'moving'.
 *
 *	A shader reads what is behind its pixel and writes the pixel, and
 *	reads nothing any other rectangle of its group writes: the
 *	rectangles of one stage of one window do not overlap, and a step of
 *	the spread reads only the level the step before made. So the
 *	rectangles of a group run in any order, and only the first of each
 *	group waits for what came before (a barrier).
 *
 *	The parameters are those of the V3D's shaders (v3d_compose.c), with
 *	the frame's origin 0: the pixel the shader works out is the
 *	destination's own.
 */

#ifdef IGT_RPLAN_HOST
#include "kernel.h"
#include <ts/dp.h>
#define X64
#else
#include <sys/machine.h>
#endif

#ifdef X64

#ifndef IGT_RPLAN_HOST
#include "kernel.h"
#include <tk/tkernel.h>
#endif
#include "igt_rplan.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../v3d/v3d_cp_split.h"
#pragma GCC diagnostic pop

typedef struct {
	CONST T_CPDST	*d;
	CONST IGT_RENV	*env;
	IGT_RD		*rd;
	INT		n, max;
	BOOL		over;
	BOOL		group;		/* the next rectangle begins a group */
	UD		back;		/* the GPU address of the back buffer's pixel (0, 0) */
} PLAN;

LOCAL BOOL cut( T_DPRECT *r, CONST T_DPRECT *c )
{
	if ( r->left < c->left ) r->left = c->left;
	if ( r->top < c->top ) r->top = c->top;
	if ( r->right > c->right ) r->right = c->right;
	if ( r->bottom > c->bottom ) r->bottom = c->bottom;
	return (BOOL)( r->left < r->right && r->top < r->bottom );
}

LOCAL void put64( UW *par, INT at, UD v )
{
	par[at] = (UW)v;
	par[at + 1] = (UW)( v >> 32 );
}

/*
 * A rectangle of shader fs over (l, t, r, b) of the destination at dst
 * (pitch bytes a row), with the parameters par and buffers buf
 */
LOCAL void rect( PLAN *p, INT fs, INT l, INT t, INT r, INT b, UD dst, UINT pitch, CONST UW *par, CONST UD *buf )
{
	IGT_RD	*d;
	INT	i;

	if ( l >= r || t >= b ) return;
	if ( p->n >= p->max ) {
		p->over = TRUE;
		return;
	}
	d = &p->rd[p->n++];
	knl_memset(d, 0, sizeof(*d));
	d->fs = (UB)fs;
	d->barrier = (UB)p->group;
	p->group = FALSE;
	d->l = l;
	d->t = t;
	d->r = r;
	d->b = b;
	for ( i = 0; i < CP_NPARAM; i++ ) d->par[i] = par[i];
	d->par[CP_P_ORGX] = 0;
	d->par[CP_P_ORGY] = 0;
	put64(d->par, IGT_CP_P_DST, dst);
	d->par[IGT_CP_P_DPITCH] = pitch;
	d->par[IGT_CP_P_RL] = (UW)l;
	d->par[IGT_CP_P_RT] = (UW)t;
	d->par[IGT_CP_P_RR] = (UW)r;
	d->par[IGT_CP_P_RB] = (UW)b;
	for ( i = 0; i < CP_NBUF; i++ ) put64(d->par, IGT_CP_P_BUF + 2 * i, buf[i]);
}

/* A rectangle of the screen, cut to the part */
LOCAL void lay( PLAN *p, INT fs, INT l, INT t, INT r, INT b, CONST UW *par, CONST UD *buf )
{
	T_DPRECT	c;

	c.left = l;  c.top = t;  c.right = r;  c.bottom = b;
	if ( !cut(&c, &p->d->box) ) return;
	rect(p, fs, c.left, c.top, c.right, c.bottom, p->back, p->d->pitch, par, buf);
}

LOCAL INT round_of( CONST T_CPWIN *w, CONST T_DPRECT *r )
{
	INT	rad = ( w->shape == NULL ) ? cp_round_of(r, w->round) : 0;

	return ( cp_corner_cov(rad) == NULL ) ? 0 : rad;
}

LOCAL UD cov_of( PLAN *p, INT rad )
{
	return ( rad > 0 && rad <= CP_ROUND_MAX && p->env->cov[rad] != 0 ) ? p->env->cov[rad] : p->env->cov[0];
}

/* Level k (1 .. CP_BLUR_DEPTH) of a spread, in T1 */
LOCAL UD lv_at( PLAN *p, INT k )
{
	return p->env->t1 + (UD)cp_lv_row(p->d->rows, k) * p->d->pitch;
}

/* One step of a spread: fs over the rectangles r[0..n-1] of a level at 'to', reading the level at 'from' */
LOCAL void spread_step( PLAN *p, INT fs, UD from, UD to, CONST T_DPRECT *v, CONST T_DPRECT *r, INT n, UW *par,
			UD *buf )
{
	INT	i;

	par[CP_PL_VL] = (UW)v->left;
	par[CP_PL_VT] = (UW)v->top;
	par[CP_PL_VR] = (UW)v->right;
	par[CP_PL_VB] = (UW)v->bottom;
	buf[CP_B_SRC] = from;
	p->group = TRUE;
	for ( i = 0; i < n; i++ ) {
		rect(p, fs, r[i].left, r[i].top, r[i].right, r[i].bottom, to, p->d->pitch, par, buf);
	}
}

/* What shows through the parts of a window that are not solid, spread into T2 */
LOCAL void spread( PLAN *p, CONST T_CPPIECE *pc, INT npc, INT rad )
{
	CONST T_CPDST	*d = p->d;
	T_CPSPREAD	sp[CP_NPIECE];
	T_DPRECT	r[CP_NPIECE];
	UW		par[CP_NPARAM];
	UD		buf[CP_NBUF], below = p->env->t2 + (UD)d->rows * d->pitch;
	INT		i, k, n = 0, depth;

	for ( i = 0; i < npc; i++ ) {
		if ( pc[i].a < 255 && cp_spread_plan(&d->box, &pc[i].r, rad, &sp[n]) ) n++;
	}
	if ( n == 0 ) return;
	depth = sp[0].depth;
	knl_memset(par, 0, sizeof(par));
	par[CP_PL_PITCH] = d->pitch;
	par[CP_PL_A] = (UW)sp[0].w[0];
	par[CP_PL_B] = (UW)sp[0].w[1];
	par[CP_PL_M] = (UW)sp[0].w[2];
	buf[CP_B_AUX] = p->back;
	buf[CP_B_COV] = p->env->cov[0];

	for ( k = 1; k <= depth; k++ ) {
		for ( i = 0; i < n; i++ ) r[i] = sp[i].down[k];
		spread_step(p, CP_FS_HALVE, ( k == 1 ) ? p->back : lv_at(p, k - 1), lv_at(p, k), &sp[0].valid[k - 1], r, n,
			    par, buf);
	}
	for ( i = 0; i < n; i++ ) r[i] = sp[i].up[depth];
	spread_step(p, ( sp[0].w[0] != 0 ) ? CP_FS_SMOOTH5 : CP_FS_SMOOTH3, ( depth == 0 ) ? p->back : lv_at(p, depth),
		    ( depth == 0 ) ? p->env->t2 : below, &sp[0].valid[depth], r, n, par, buf);
	for ( k = depth; k >= 1; k-- ) {
		for ( i = 0; i < n; i++ ) r[i] = sp[i].up[k - 1];
		spread_step(p, CP_FS_DOUBLE, ( k == depth ) ? below : lv_at(p, k), ( k == 1 ) ? p->env->t2 : lv_at(p, k - 1),
			    &sp[0].valid[k], r, n, par, buf);
	}
}

/* A window opening or closing (compose.c cp_moving) */
LOCAL void lay_moving( PLAN *p, CONST T_CPWIN *w, UD src )
{
	T_DPRECT	v = w->shown;
	INT		ow = w->outer.right - w->outer.left, oh = w->outer.bottom - w->outer.top;
	INT		sw = w->shown.right - w->shown.left, sh = w->shown.bottom - w->shown.top;
	UW		par[CP_NPARAM];
	UD		buf[CP_NBUF];
	INT		rad;

	if ( sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0 || !cut(&v, &p->d->box) ) return;
	rad = cp_round_of(&w->shown, w->round);
	if ( cp_corner_cov(rad) == NULL ) rad = 0;
	knl_memset(par, 0, sizeof(par));
	par[CP_PM_PITCH] = w->pitch;
	par[CP_PM_SL] = (UW)w->shown.left;
	par[CP_PM_ST] = (UW)w->shown.top;
	par[CP_PM_SR] = (UW)w->shown.right;
	par[CP_PM_SB] = (UW)w->shown.bottom;
	par[CP_PM_STEPX] = ( (UW)ow << 16 ) / (UW)sw;
	par[CP_PM_STEPY] = ( (UW)oh << 16 ) / (UW)sh;
	par[CP_PM_OL] = (UW)w->outer.left;
	par[CP_PM_OT] = (UW)w->outer.top;
	par[CP_PM_OR] = (UW)w->outer.right;
	par[CP_PM_OB] = (UW)w->outer.bottom;
	par[CP_PM_WL] = (UW)w->work.left;
	par[CP_PM_WT] = (UW)w->work.top;
	par[CP_PM_WR] = (UW)w->work.right;
	par[CP_PM_WB] = (UW)w->work.bottom;
	par[CP_PM_AW] = w->a_work * w->fade / 255U;
	par[CP_PM_AF] = w->a_frame * w->fade / 255U;
	par[CP_PM_RAD] = (UW)rad;
	par[CP_PM_TINT] = w->tint;
	par[CP_PM_TSTR] = w->tint_str;
	par[CP_PM_WASH] = w->wash;
	par[CP_PM_WSTR] = w->wash_str;
	par[CP_PM_BWASH] = w->bar_wash;
	par[CP_PM_BSTR] = w->bar_str;
	par[CP_PM_BAND] = ( w->band.right > w->band.left ) ? 1 : 0;
	buf[CP_B_SRC] = src;
	buf[CP_B_AUX] = p->back;
	buf[CP_B_COV] = cov_of(p, rad);
	p->group = TRUE;
	lay(p, CP_FS_MOVING, v.left, v.top, v.right, v.bottom, par, buf);
}

/* A window in its stages: shadow, spread, pieces; FALSE when it spreads and there are no levels */
LOCAL BOOL lay_window( PLAN *p, CONST T_CPWIN *w, UD src )
{
	CONST T_CPDST	*d = p->d;
	T_CPPIECE	pc[CP_NPIECE];
	T_DPRECT	e;
	UW		par[CP_NPARAM], cpar[CP_NPARAM];
	UD		buf[CP_NBUF];
	CP_SR		so, sp, sr[CP_SPLIT_PIECE_MAX + CP_SPLIT_SHADOW_MAX];
	INT		npc, rad, brad, i, k, n, sfs[CP_SPLIT_PIECE_MAX];
	BOOL		spreads = FALSE;

	rad = round_of(w, &w->outer);
	buf[CP_B_SRC] = src;
	buf[CP_B_AUX] = p->back;
	buf[CP_B_COV] = cov_of(p, rad);
	so.l = w->outer.left;   so.t = w->outer.top;
	so.r = w->outer.right;  so.b = w->outer.bottom;

	npc = cp_pieces(d, w, pc);
	brad = ( w->blur > CP_BLUR_MAX ) ? CP_BLUR_MAX : w->blur;
	if ( brad > 0 ) {
		for ( i = 0; i < npc; i++ ) {
			if ( pc[i].a < 255 ) spreads = TRUE;
		}
	}
	if ( spreads && ( p->env->t1 == 0 || p->env->t2 == 0 ) ) return FALSE;

	/* 1. the shadow, round the window */
	if ( cp_shadow_extent(w, &e) && cut(&e, &d->box) ) {
		CP_SR	se;

		knl_memset(par, 0, sizeof(par));
		par[CP_PS_OL] = (UW)w->outer.left;
		par[CP_PS_OT] = (UW)w->outer.top;
		par[CP_PS_OR] = (UW)w->outer.right;
		par[CP_PS_OB] = (UW)w->outer.bottom;
		par[CP_PS_DX] = (UW)w->shd_dx;
		par[CP_PS_DY] = (UW)w->shd_dy;
		par[CP_PS_SIZE] = (UW)w->shd_size;
		par[CP_PS_DARK] = w->shd_dark;
		par[CP_PS_RAD] = (UW)rad;
		se.l = e.left;   se.t = e.top;
		se.r = e.right;  se.b = e.bottom;
		n = cp_split_shadow(&se, &so, rad, sr);
		p->group = TRUE;
		for ( k = 0; k < n; k++ ) lay(p, CP_FS_SHADOW, sr[k].l, sr[k].t, sr[k].r, sr[k].b, par, buf);
	}

	/* 2. what shows through, spread from what is behind now */
	if ( spreads ) {
		spread(p, pc, npc, brad);
		buf[CP_B_AUX] = p->env->t2;
	}

	/* 3 and 4. the pieces, washed; where one is solid and plain, the window's pixels as they are */
	knl_memset(cpar, 0, sizeof(cpar));
	cpar[CP_PD_PITCH] = w->pitch;
	cpar[CP_PD_OL] = (UW)w->outer.left;
	cpar[CP_PD_OT] = (UW)w->outer.top;
	cpar[CP_PD_W] = (UW)( w->outer.right - w->outer.left );
	cpar[CP_PD_H] = (UW)( w->outer.bottom - w->outer.top );
	p->group = TRUE;
	for ( i = 0; i < npc; i++ ) {
		UINT	tstr = pc[i].frame ? w->tint_str : 0;
		BOOL	plain = ( pc[i].a >= 255 && pc[i].wstr == 0 && tstr == 0 );

		knl_memset(par, 0, sizeof(par));
		par[CP_PP_PITCH] = w->pitch;
		par[CP_PP_OL] = (UW)w->outer.left;
		par[CP_PP_OT] = (UW)w->outer.top;
		par[CP_PP_OR] = (UW)w->outer.right;
		par[CP_PP_OB] = (UW)w->outer.bottom;
		par[CP_PP_A] = pc[i].a;
		par[CP_PP_RAD] = (UW)rad;
		par[CP_PP_TINT] = w->tint;
		par[CP_PP_TSTR] = tstr;
		par[CP_PP_WASH] = pc[i].wcol;
		par[CP_PP_WSTR] = pc[i].wstr;
		par[CP_PP_UND] = ( spreads && pc[i].a < 255 ) ? 1 : 0;
		par[CP_PP_UPITCH] = d->pitch;
		par[CP_PP_SW] = d->pitch / 4;
		par[CP_PP_SH] = (UW)d->rows;
		sp.l = pc[i].r.left;   sp.t = pc[i].r.top;
		sp.r = pc[i].r.right;  sp.b = pc[i].r.bottom;
		n = cp_split_piece(&sp, plain, &so, rad, sr, sfs);
		for ( k = 0; k < n; k++ ) {
			lay(p, sfs[k], sr[k].l, sr[k].t, sr[k].r, sr[k].b, ( sfs[k] == CP_FS_COPY ) ? cpar : par, buf);
		}
	}
	return TRUE;
}

EXPORT INT igt_rplan( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n,
		      CONST IGT_RENV *env, IGT_RD *rd, INT max, INT *p_nrd )
{
	PLAN	p;
	UW	par[CP_NPARAM];
	UD	buf[CP_NBUF], src;
	INT	k;

	*p_nrd = 0;
	knl_memset(&p, 0, sizeof(p));
	p.d = d;
	p.env = env;
	p.rd = rd;
	p.max = max;
	p.back = env->gpu(env->ctx, d->back, (UD)d->pitch * (UD)d->rows);
	buf[CP_B_SRC] = env->gpu(env->ctx, ground, (UD)gpitch * (UD)d->rows);
	if ( p.back == 0 || buf[CP_B_SRC] == 0 ) return E_NOMEM;

	/* 1. the ground, its pixel (x, y) at y rows and 4 x bytes from its first */
	knl_memset(par, 0, sizeof(par));
	par[CP_PD_PITCH] = gpitch;
	par[CP_PD_W] = gpitch / 4;
	par[CP_PD_H] = (UW)d->rows;
	buf[CP_B_AUX] = p.back;
	buf[CP_B_COV] = env->cov[0];
	lay(&p, CP_FS_COPY, d->box.left, d->box.top, d->box.right, d->box.bottom, par, buf);

	/* 2. the windows, up to the first the shaders do not lay */
	for ( k = 0; k < n && !p.over; k++ ) {
		if ( w[k].shape != NULL || w[k].keyed || w[k].pixels == NULL ) break;
		src = env->gpu(env->ctx, w[k].pixels, (UD)w[k].pitch * (UD)( w[k].outer.bottom - w[k].outer.top ));
		if ( src == 0 ) break;
		if ( CP_MOVING(&w[k]) ) {
			lay_moving(&p, &w[k], src);
		} else if ( !lay_window(&p, &w[k], src) ) {
			break;
		}
	}
	if ( p.over ) return E_LIMIT;
	*p_nrd = p.n;
	return k;
}

#endif /* X64 */
