/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_compose.c
 *	Compositing the windows on the GPU (docs/tessronos/13-gpu.md 13.11.7
 *	and 13.11.14), in the place of the processor's stages (compose.h
 *	T_CPGPU): on the render engine, every stage by compute shaders
 *	(igt_rcompose.c), or on the copy engine what it can do, whichever the
 *	check at start-up finds the same as the processor and quicker. The
 *	render engine is checked and taken first; the copy engine stays for
 *	when its check fails.
 *
 *	The copy engine copies and fills rectangles and does no more: no
 *	mixing, no coverage of corners. So it lays what the processor's
 *	stages would only copy: the ground of the part, and from the back
 *	forward each window that is solid, square, without shadow and
 *	without wash or tint, whose pixels the processor's stages copy as
 *	they are (compose.c lay_piece's plain rows). At the first window with
 *	a look of its own it stops and answers how many it laid; the
 *	processor lays the rest over them as always. The screen comes out
 *	the same to the bit, which the check at start-up makes sure of
 *	before the copy engine is used at all.
 *
 *	What to copy is planned first (igt_cp_plan), as copies between the
 *	kernel's memory, and only then turned into commands with the
 *	memory's GPU addresses: the plan is what the tests on a machine
 *	without this GPU check against the processor's stages (the copies
 *	done by the processor there), and the commands are what the check at
 *	start-up checks on the GPU.
 *
 *	Memory. The back buffer, the ground and the windows' pixels are the
 *	kernel's pages wherever they are; their physical pages are entered
 *	in the kernel's space of the GPU as they are (knl_gpu_bo_wrap) the
 *	first time a part uses them, and kept there until the kernel lets
 *	the range go (knl_vunmap_hook). The GPU and the processor share the
 *	last level cache, so nothing of the caches is done, unless the check
 *	of coherence at start finds otherwise.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smem.h>
#include <ts/disp.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"
#include "../../../outer_kernel/wm/compose.h"
#include "igt.h"
#include "igt_cp.h"

#define NWRAP		96		/* memory entered in the GPU's space at once */
#define ENG_NONE	0		/* the processor composites */
#define ENG_BCS		1		/* the copy engine lays what it can */
#define ENG_RCS		2		/* the render engine lays it all */
#define BATCH_BYTES	( 64 * 1024 )
#define STATS_MS	60000

typedef struct {
	UBINT	va;			/* the first page, 0 when the slot is free */
	UD	npages;
	T_GPUBO	*bo;
	UW	gpu;
	UD	used;			/* the part it was last used in */
	BOOL	gone;			/* the kernel let the range go */
} WRAP;

LOCAL struct {
	BOOL		ready, passed, failed;
	BOOL		flush;		/* the caches have to be done: not coherent */
	ID		mtx;
	T_SPLOCK	wrap_lock;
	WRAP		wrap[NWRAP];
	UD		part;
	T_GPUBO		*batch;
	UW		batch_gpu;
	UD		thr;		/* parts of this many pixels and more on the copy engine */
	UD		thr_plain, thr_fx;	/* and on the render engine, without and with a look */
	INT		engine;		/* ENG_*: the one in use */
	BOOL		rc_passed;
	CP_OP		ops[IGT_CP_MAXOPS];
	/* the record */
	UD		st_t0, st_gpu, st_cpu, st_gpu_px, st_cpu_px, st_gpu_us, st_cpu_us, st_fallback, st_partial;
} cg;

/* ---------------------------------------------------------------- the plan */

/* Whether the processor's stages only copy the window's pixels, all of it */
EXPORT BOOL igt_cp_plain( CONST T_CPWIN *w )
{
	return (BOOL)( !CP_MOVING(w) && w->a_frame >= 255 && w->a_work >= 255 && !w->keyed && w->round == 0
		    && w->shape == NULL && w->wash_str == 0 && w->bar_str == 0 && w->tint_str == 0
		    && w->shd_dark == 0 && w->pixels != NULL );
}

LOCAL BOOL clip( T_DPRECT *r, CONST T_DPRECT *a, CONST T_DPRECT *b )
{
	r->left = ( a->left > b->left ) ? a->left : b->left;
	r->top = ( a->top > b->top ) ? a->top : b->top;
	r->right = ( a->right < b->right ) ? a->right : b->right;
	r->bottom = ( a->bottom < b->bottom ) ? a->bottom : b->bottom;
	return (BOOL)( r->left < r->right && r->top < r->bottom );
}

/*
 * The copies that lay the ground of the part and the windows w[0..n-1]
 * up to the first that is not plain, into ops (max of them): how many
 * windows they lay, the number of copies in *p_nops
 */
EXPORT INT igt_cp_plan( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n,
			CP_OP *ops, INT max, INT *p_nops )
{
	INT	k, nops = 0;
	T_DPRECT r;

	if ( max < 1 || d->box.left >= d->box.right || d->box.top >= d->box.bottom ) {
		*p_nops = 0;
		return 0;
	}
	ops[nops].src = (CONST UB *)ground + (UBINT)d->box.top * gpitch + (UBINT)d->box.left * 4;
	ops[nops].spitch = gpitch;
	ops[nops].dst = d->back + (UBINT)d->box.top * d->pitch + (UBINT)d->box.left * 4;
	ops[nops].dpitch = d->pitch;
	ops[nops].w = d->box.right - d->box.left;
	ops[nops].h = d->box.bottom - d->box.top;
	nops++;
	for ( k = 0; k < n; k++ ) {
		if ( !igt_cp_plain(&w[k]) ) break;
		if ( !clip(&r, &w[k].outer, &d->box) ) continue;
		if ( nops >= max ) break;
		ops[nops].src = w[k].pixels + (UBINT)( r.top - w[k].outer.top ) * w[k].pitch
				+ (UBINT)( r.left - w[k].outer.left ) * 4;
		ops[nops].spitch = w[k].pitch;
		ops[nops].dst = d->back + (UBINT)r.top * d->pitch + (UBINT)r.left * 4;
		ops[nops].dpitch = d->pitch;
		ops[nops].w = r.right - r.left;
		ops[nops].h = r.bottom - r.top;
		nops++;
	}
	*p_nops = nops;
	return k;
}

/* The plan carried out by the processor (the tests on a machine without the GPU) */
EXPORT void igt_cp_run_cpu( CONST CP_OP *ops, INT nops )
{
	INT	i, y;

	for ( i = 0; i < nops; i++ ) {
		for ( y = 0; y < ops[i].h; y++ ) {
			knl_memcpy(ops[i].dst + (UBINT)y * ops[i].dpitch, ops[i].src + (UBINT)y * ops[i].spitch,
				   (SZ)ops[i].w * 4);
		}
	}
}

/* ---------------------------------------------------------------- memory in the GPU's space */

LOCAL void vunmap_seen( UBINT va, UD npages )
{
	UBINT	end = va + ( npages << PAGE_SHIFT );
	UINT	imask;
	INT	i;

	ISpinLock(&cg.wrap_lock, &imask);
	for ( i = 0; i < NWRAP; i++ ) {
		WRAP	*w = &cg.wrap[i];

		if ( w->va != 0 && w->va < end && va < w->va + ( w->npages << PAGE_SHIFT ) ) w->gone = TRUE;
	}
	ISpinUnlock(&cg.wrap_lock, &imask);
}

LOCAL void wrap_drop( WRAP *w )
{
	T_GPUBO	*bo = w->bo;
	UINT	imask;

	ISpinLock(&cg.wrap_lock, &imask);
	w->va = 0;
	w->bo = NULL;
	w->gone = FALSE;
	ISpinUnlock(&cg.wrap_lock, &imask);
	if ( bo != NULL ) knl_gpu_bo_put(bo);
}

LOCAL void wraps_sweep( void )
{
	INT	i;

	for ( i = 0; i < NWRAP; i++ ) {
		if ( cg.wrap[i].va != 0 && cg.wrap[i].gone ) wrap_drop(&cg.wrap[i]);
	}
}

/*
 * The GPU address of the page that holds 'p' (the kernel's memory of
 * 'len' bytes from p), entering the pages when they are not in, and the
 * BO they are in; 0 when they cannot be
 */
LOCAL UW wrap_of( CONST void *p, UD len, T_GPUBO **p_bo )
{
	UBINT	va = (UBINT)p & ~(UBINT)( PAGE_SIZE - 1 );
	UD	npages = ( ( (UBINT)p - va ) + len + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	UD	*pa, i;
	WRAP	*w = NULL, *old = NULL;
	T_GPUBO	*bo;
	T_GPUBOINFO bi;
	UINT	imask;
	INT	k;
	ER	er;

	for ( k = 0; k < NWRAP; k++ ) {
		WRAP	*c = &cg.wrap[k];

		if ( c->va != 0 && c->va <= va && va + ( npages << PAGE_SHIFT ) <= c->va + ( c->npages << PAGE_SHIFT )
		  && !c->gone ) {
			c->used = cg.part;
			*p_bo = c->bo;
			return c->gpu + (UW)( va - c->va );
		}
	}
	for ( k = 0; k < NWRAP; k++ ) {
		WRAP	*c = &cg.wrap[k];

		if ( c->va == 0 ) {
			w = c;
			break;
		}
		if ( c->used != cg.part && ( old == NULL || c->used < old->used ) ) old = c;
	}
	if ( w == NULL ) w = old;
	if ( w == NULL ) return 0;
	if ( w->va != 0 ) wrap_drop(w);
	pa = (UD *)Kmalloc(sizeof(UD) * npages);
	if ( pa == NULL ) return 0;
	for ( i = 0; i < npages; i++ ) {
		pa[i] = knl_pt_lookup(knl_pt_l0_k_pa, va + ( i << PAGE_SHIFT ));
		if ( pa[i] == ~(UD)0 ) {
			Kfree(pa);
			return 0;
		}
	}
	bo = knl_gpu_bo_wrap(pa, npages, &er);
	Kfree(pa);
	if ( bo == NULL ) return 0;
	(void)knl_gpu_bo_ref(bo, &bi);
	ISpinLock(&cg.wrap_lock, &imask);
	w->va = va;
	w->npages = npages;
	w->bo = bo;
	w->gpu = bi.offset;
	w->used = cg.part;
	w->gone = FALSE;
	ISpinUnlock(&cg.wrap_lock, &imask);
	*p_bo = bo;
	return bi.offset;
}

/* The same for the render engine's plan (igt_rcompose.c) */
EXPORT UW igt_cp_wrap( CONST void *p, UD len, T_GPUBO **p_bo )
{
	return wrap_of(p, len, p_bo);
}

/* A part begun: the wraps of ranges the kernel let go of dropped */
EXPORT void igt_cp_new_part( void )
{
	cg.part++;
	wraps_sweep();
}

/* ---------------------------------------------------------------- the commands */

/*
 * One copy as XY_SRC_COPY_BLT. The engine addresses a surface by its
 * base, a pitch and coordinates; the base is taken as the page the
 * first pixel lies in, the pixel's place in it as coordinates.
 */
LOCAL BOOL op_cmd( UW *c, CONST CP_OP *op, T_GPUBO **bos, INT *p_nbo )
{
	T_GPUBO	*sb, *db;
	UD	slen = (UD)( op->h - 1 ) * op->spitch + (UD)op->w * 4;
	UD	dlen = (UD)( op->h - 1 ) * op->dpitch + (UD)op->w * 4;
	UW	sg = wrap_of(op->src, slen, &sb), dg = wrap_of(op->dst, dlen, &db);
	UW	so = (UW)( (UBINT)op->src & ( PAGE_SIZE - 1 ) ), dofs = (UW)( (UBINT)op->dst & ( PAGE_SIZE - 1 ) );
	INT	sx = (INT)( so % op->spitch ) / 4, sy = (INT)( so / op->spitch );
	INT	dx = (INT)( dofs % op->dpitch ) / 4, dy = (INT)( dofs / op->dpitch );

	if ( sg == 0 || dg == 0 || op->spitch > BLT_PITCH_MAX || op->dpitch > BLT_PITCH_MAX
	  || ( ( so | dofs | op->spitch | op->dpitch ) & 3 ) != 0
	  || dx + op->w > BLT_COORD_MAX || dy + op->h > BLT_COORD_MAX || sx + op->w > BLT_COORD_MAX
	  || sy + op->h > BLT_COORD_MAX ) {
		return FALSE;
	}
	c[0] = XY_SRC_COPY_BLT | BLT_WRITE_RGBA;
	c[1] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | op->dpitch;
	c[2] = ( (UW)dy << 16 ) | (UW)dx;
	c[3] = ( (UW)( dy + op->h ) << 16 ) | (UW)( dx + op->w );
	c[4] = dg;
	c[5] = 0;
	c[6] = ( (UW)sy << 16 ) | (UW)sx;
	c[7] = op->spitch;
	c[8] = sg;
	c[9] = 0;
	bos[(*p_nbo)++] = sb;
	bos[(*p_nbo)++] = db;
	return TRUE;
}

/*
 * The plan as a batch on the copy engine, run and waited for: E_OK, or
 * an error when it could not be built (nothing done) or the job failed
 */
LOCAL ER run_ops( CONST CP_OP *ops, INT nops )
{
	T_GPUBO	*bos[2 * IGT_CP_MAXOPS + 1];
	UW	*b = (UW *)PA2VA(knl_gpu_bo_page(cg.batch, 0));
	INT	i, nbo = 0, n = 0;

	cg.part++;
	wraps_sweep();
	for ( i = 0; i < nops; i++ ) {
		if ( !op_cmd(b + n, &ops[i], bos, &nbo) ) return E_NOMEM;
		n += 10;
		if ( cg.flush ) {
			INT	y;

			for ( y = 0; y < ops[i].h; y++ ) {
				igt_clflush(ops[i].src + (UBINT)y * ops[i].spitch, (UD)ops[i].w * 4);
				igt_clflush(ops[i].dst + (UBINT)y * ops[i].dpitch, (UD)ops[i].w * 4);
			}
		}
	}
	b[n++] = MI_BATCH_BUFFER_END;
	if ( n & 1 ) b[n++] = MI_NOOP;
	igt_clflush(b, (UD)n * 4);
	bos[nbo++] = cg.batch;
	return igt_kjob(IGT_BCS, cg.batch_gpu, (UW)( n * 4 ), bos, nbo);
}

/* ---------------------------------------------------------------- the window manager's calls */

LOCAL INT lay_run( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n )
{
	INT	nops, laid;
	ER	er;

	if ( cg.engine == ENG_RCS ) return igt_rc_lay(d, ground, gpitch, w, n, cg.flush);
	laid = igt_cp_plan(d, ground, gpitch, w, n, cg.ops, IGT_CP_MAXOPS, &nops);
	if ( nops == 0 ) return E_NOSPT;
	er = run_ops(cg.ops, nops);
	if ( er < E_OK ) return er;
	if ( cg.flush ) {
		INT	y;

		for ( y = d->box.top; y < d->box.bottom; y++ ) {
			igt_clflush(d->back + (UBINT)y * d->pitch + (UBINT)d->box.left * 4,
				    (UD)( d->box.right - d->box.left ) * 4);
		}
	}
	return laid;
}

LOCAL INT cg_lay( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, UINT gen, CONST T_CPWIN *w, INT n )
{
	INT	r;

	(void)gen;
	if ( cg.engine == ENG_NONE || cg.failed ) return E_NOSPT;
	tk_loc_mtx(cg.mtx, TMO_FEVR);
	r = lay_run(d, ground, gpitch, w, n);
	if ( r == E_IO || r == E_TMOUT ) {
		cg.failed = TRUE;
		tm_printf((UB *)"igt compose: a job failed (%d): compositing goes back to the processor\n", r);
	} else if ( r < E_OK ) {
		cg.st_fallback++;
	} else if ( r < n ) {
		cg.st_partial++;
	}
	tk_unl_mtx(cg.mtx);
	return r;
}

LOCAL BOOL cg_wants( CONST T_DPRECT *box, UINT fx )
{
	UD	px;

	if ( cg.engine == ENG_NONE || cg.failed ) return FALSE;
	px = (UD)( box->right - box->left ) * (UD)( box->bottom - box->top );
	if ( cg.engine == ENG_RCS ) return (BOOL)( px >= ( ( fx & ( CP_FX_LOOK | CP_FX_BLUR ) ) ? cg.thr_fx : cg.thr_plain ) );
	return (BOOL)( px >= cg.thr );
}

LOCAL void cg_note( BOOL gpu, CONST T_DPRECT *box, UD us )
{
	UD	px = (UD)( box->right - box->left ) * (UD)( box->bottom - box->top );
	UD	now = igt_now_us() / 1000;

	if ( gpu ) {
		cg.st_gpu++;
		cg.st_gpu_px += px;
		cg.st_gpu_us += us;
	} else {
		cg.st_cpu++;
		cg.st_cpu_px += px;
		cg.st_cpu_us += us;
	}
	if ( cg.st_t0 == 0 ) {
		cg.st_t0 = now;
	} else if ( now - cg.st_t0 >= STATS_MS ) {
		tm_printf((UB *)"igt compose: in %ld s: %ld parts on the %s engine (%ld Kpx, %ld us), %ld on the "
			  "processor (%ld Kpx, %ld us); fell back %ld, partly %ld\n", ( now - cg.st_t0 ) / 1000,
			  cg.st_gpu, ( cg.engine == ENG_RCS ) ? "render" : "copy", cg.st_gpu_px / 1000, cg.st_gpu_us,
			  cg.st_cpu, cg.st_cpu_px / 1000, cg.st_cpu_us, cg.st_fallback, cg.st_partial);
		cg.st_t0 = now;
		cg.st_gpu = cg.st_cpu = cg.st_gpu_px = cg.st_cpu_px = cg.st_gpu_us = cg.st_cpu_us = 0;
		cg.st_fallback = cg.st_partial = 0;
	}
}

LOCAL CONST T_CPGPU	cg_ops = {
	1,
	cg_wants,
	cg_lay,
	cg_note
};

/* ---------------------------------------------------------------- setting up */

EXPORT ER igt_compose_init( void )
{
	T_CMTX		cm;
	T_GPUBOINFO	bi;
	ER		er;

	if ( cg.ready ) return E_OK;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cg.mtx = tk_cre_mtx(&cm);
	if ( cg.mtx <= 0 ) return E_LIMIT;
	cg.batch = knl_gpu_bo_create(PAGE_SIZE, 0, &er);
	if ( cg.batch == NULL ) {
		tm_printf((UB *)"igt compose: no memory for the batch (%d)\n", er);
		return er;
	}
	(void)knl_gpu_bo_ref(cg.batch, &bi);
	cg.batch_gpu = bi.offset;
	cg.thr = (UD)-1;
	cg.ready = TRUE;
	knl_vunmap_hook = vunmap_seen;
	return E_OK;
}

/* Where the windows are composited: 0 the processor, 1 the copy engine, 2 the render engine */
EXPORT INT igt_compose_used( void )
{
	return cg.failed ? ENG_NONE : cg.engine;
}

/* ---------------------------------------------------------------- the check at start-up */

/*
 * Scenes built both ways over the same ground into two back buffers,
 * on the processor with its stages (cp_window) and with the copy engine
 * laying what it lays and the processor the rest, and compared pixel by
 * pixel. The copy engine is used only when every scene comes out the
 * same, to the top byte. Then the time either way takes for a small, a
 * middling and a whole part gives the size from which it is quicker.
 */
#define CK_W		1024
#define CK_H		768
#define CK_PITCH	( CK_W * 4 )
#define CK_NBUF		3

IMPORT void knl_wm_hold( BOOL hold );

LOCAL struct {
	UB	*back_c, *back_g;
	UW	*ground;
	UB	*pix[CK_NBUF];
	UW	seed;
	INT	nscene, npass;
} ck;

LOCAL UW ck_rnd( void )
{
	ck.seed = ck.seed * 1103515245U + 12345U;
	return ( ck.seed >> 8 ) ^ ( ck.seed << 20 );
}

LOCAL UD ck_pages( void )
{
	return ( (UD)CK_PITCH * CK_H + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
}

LOCAL void ck_free( void )
{
	INT	i;

	if ( ck.back_c != NULL ) knl_vunmap(ck.back_c, ck_pages());
	if ( ck.back_g != NULL ) knl_vunmap(ck.back_g, ck_pages());
	if ( ck.ground != NULL ) knl_vunmap(ck.ground, ck_pages());
	for ( i = 0; i < CK_NBUF; i++ ) {
		if ( ck.pix[i] != NULL ) knl_vunmap(ck.pix[i], ck_pages());
		ck.pix[i] = NULL;
	}
	ck.back_c = ck.back_g = NULL;
	ck.ground = NULL;
}

LOCAL BOOL ck_alloc( void )
{
	INT	i, x, y;

	ck.back_c = (UB *)knl_vmap(ck_pages(), 0);
	ck.back_g = (UB *)knl_vmap(ck_pages(), 0);
	ck.ground = (UW *)knl_vmap(ck_pages(), 0);
	for ( i = 0; i < CK_NBUF; i++ ) ck.pix[i] = (UB *)knl_vmap(ck_pages(), 0);
	if ( ck.back_c == NULL || ck.back_g == NULL || ck.ground == NULL || ck.pix[CK_NBUF - 1] == NULL ) {
		ck_free();
		return FALSE;
	}
	ck.seed = 0x1234567U;
	for ( y = 0; y < CK_H; y++ ) {
		for ( x = 0; x < CK_W; x++ ) {
			ck.ground[y * CK_W + x] = ( ( (UW)x * 255 / CK_W ) << 16 ) | ( ( (UW)y * 255 / CK_H ) << 8 )
						  | ( ( x ^ y ) & 0xFF ) | ( ck_rnd() & 0xFF000000U );
		}
	}
	for ( i = 0; i < CK_NBUF; i++ ) {
		UW	*p = (UW *)ck.pix[i];

		for ( y = 0; y < CK_H; y++ ) {
			for ( x = 0; x < CK_W; x++ ) p[y * CK_W + x] = ck_rnd();
		}
	}
	return TRUE;
}

LOCAL void ck_win( T_CPWIN *w, INT k, INT l, INT t, INT ww, INT hh )
{
	knl_memset(w, 0, sizeof(*w));
	w->outer.left = l;
	w->outer.top = t;
	w->outer.right = l + ww;
	w->outer.bottom = t + hh;
	w->work.left = l + 3;
	w->work.top = t + 22;
	w->work.right = l + ww - 4;
	w->work.bottom = t + hh - 4;
	w->band = w->outer;
	w->band.bottom = w->work.top;
	w->pixels = ck.pix[k % CK_NBUF];
	w->pitch = CK_PITCH;
	w->a_frame = 255;
	w->a_work = 255;
	w->shown = w->outer;
	w->fade = 255;
}

LOCAL void ck_cpu( CONST T_DPRECT *box, CONST T_CPWIN *w, INT n )
{
	T_CPDST	d;
	INT	y, i;

	d.back = ck.back_c;
	d.pitch = CK_PITCH;
	d.box = *box;
	d.rows = CK_H;
	for ( y = box->top; y < box->bottom; y++ ) {
		knl_memcpy(ck.back_c + (UBINT)y * CK_PITCH + (UBINT)box->left * 4,
			   ck.ground + (SZ)y * CK_W + box->left, (SZ)( box->right - box->left ) * 4);
	}
	for ( i = 0; i < n; i++ ) cp_window(&d, &w[i]);
}

LOCAL INT ck_gpu( CONST T_DPRECT *box, CONST T_CPWIN *w, INT n )
{
	T_CPDST	d;
	INT	r, i;

	d.back = ck.back_g;
	d.pitch = CK_PITCH;
	d.box = *box;
	d.rows = CK_H;
	tk_loc_mtx(cg.mtx, TMO_FEVR);
	r = lay_run(&d, ck.ground, CK_PITCH, w, n);
	tk_unl_mtx(cg.mtx);
	for ( i = ( r < 0 ) ? 0 : r; i < n; i++ ) cp_window(&d, &w[i]);
	return r;
}

LOCAL UW ck_junk_at( UD i )
{
	return 0xA5000000U ^ ( (UW)i * 0x9E3779B1U );
}

LOCAL void ck_junk( void )
{
	UW	*c = (UW *)ck.back_c, *g = (UW *)ck.back_g;
	UD	i, n = (UD)CK_W * CK_H;

	for ( i = 0; i < n; i++ ) c[i] = g[i] = ck_junk_at(i);
}

/* How many of the windows the engine on trial is to lay: the copy engine those before the first that is not plain */
LOCAL INT ck_want( CONST T_CPWIN *w, INT n )
{
	INT	k;

	if ( cg.engine == ENG_RCS ) return n;
	for ( k = 0; k < n && igt_cp_plain(&w[k]); k++ ) ;
	return k;
}

LOCAL BOOL ck_scene( CONST char *name, CONST T_DPRECT *box, CONST T_CPWIN *w, INT n )
{
	UW	*c = (UW *)ck.back_c, *g = (UW *)ck.back_g;
	UD	i, np = (UD)CK_W * CK_H, ndiff = 0, nold = 0, t0, cpu_us, gpu_us;
	INT	r, shown = 0, want = ck_want(w, n);
	BOOL	same;

	ck_junk();
	knl_wm_hold(TRUE);
	t0 = igt_now_us();
	ck_cpu(box, w, n);
	cpu_us = igt_now_us() - t0;
	t0 = igt_now_us();
	r = ck_gpu(box, w, n);
	gpu_us = igt_now_us() - t0;
	knl_wm_hold(FALSE);
	for ( i = 0; i < np; i++ ) {
		if ( c[i] == g[i] ) continue;
		ndiff++;
		if ( g[i] == ck_junk_at(i) ) nold++;
		if ( shown < 3 ) {
			tm_printf((UB *)"igt compose: check %s: at (%d,%d) GPU %08x, processor %08x\n", name,
				  (INT)( i % CK_W ), (INT)( i / CK_W ), g[i], c[i]);
			shown++;
		}
	}
	same = ( r == want && ndiff == 0 );
	if ( cg.engine == ENG_RCS ) {
		INT	nrd, ngroup;

		igt_rc_last(&nrd, &ngroup);
		tm_printf((UB *)"igt compose: check %s, %d windows in %dx%d at (%d,%d): %s; %ld of %ld pixels differ "
			  "(%ld left as before); the render engine laid %d of %d in %d rectangles, %d groups, %ld us, "
			  "processor %ld us\n", name, n, box->right - box->left, box->bottom - box->top, box->left,
			  box->top, same ? "same" : "DIFFERENT", ndiff, np, nold, r, want, nrd, ngroup, gpu_us, cpu_us);
	} else {
		tm_printf((UB *)"igt compose: check %s, %d windows in %dx%d at (%d,%d): %s; %ld of %ld pixels differ "
			  "(%ld left as before); the copy engine laid %d of %d, %ld us, processor %ld us\n", name, n,
			  box->right - box->left, box->bottom - box->top, box->left, box->top, same ? "same" : "DIFFERENT",
			  ndiff, np, nold, r, want, gpu_us, cpu_us);
	}
	ck.nscene++;
	if ( same ) ck.npass++;
	return same;
}

/* The least of three times of a part of 'side' x 'side' with two plain windows, either way */
LOCAL void ck_time( INT wd, INT ht, UD *p_gpu, UD *p_cpu )
{
	T_CPWIN	w[2];
	T_DPRECT box = { 0, 0, 0, 0 };
	UD	t0, g = (UD)-1, c = (UD)-1, t;
	INT	k;

	box.right = wd;
	box.bottom = ht;
	ck_win(&w[0], 0, 10, 10, wd / 2 + 20, ht / 2 + 20);
	ck_win(&w[1], 1, wd / 3, ht / 3, wd / 2, ht / 2);
	for ( k = 0; k < 3; k++ ) {
		knl_wm_hold(TRUE);
		t0 = igt_now_us();
		ck_cpu(&box, w, 2);
		t = igt_now_us() - t0;
		if ( t < c ) c = t;
		t0 = igt_now_us();
		(void)ck_gpu(&box, w, 2);
		t = igt_now_us() - t0;
		if ( t < g ) g = t;
		knl_wm_hold(FALSE);
	}
	*p_gpu = g;
	*p_cpu = c;
}

LOCAL BOOL check_scenes( void );
LOCAL BOOL check_render( void );

EXPORT BOOL igt_compose_check( void )
{
	BOOL	ok;

	if ( !cg.ready ) return FALSE;
	if ( !ck_alloc() ) {
		tm_printf((UB *)"igt compose: no memory for the check\n");
		return FALSE;
	}
	cg.engine = ENG_BCS;
	cg.flush = FALSE;
	ok = check_scenes();
	if ( !ok ) {
		/* the GPU may not see what the processor's caches hold: once more with them flushed */
		cg.flush = TRUE;
		tm_printf((UB *)"igt compose: once more, the processor's caches flushed around the jobs\n");
		ok = check_scenes();
	}
	if ( !ok ) cg.flush = FALSE;
	if ( igt_setting("igtrc", 1) == 0 ) {
		tm_printf((UB *)"igt compose: the render engine not tried (cmdline.txt igtrc=0)\n");
	} else if ( igt_rc_init() >= E_OK ) {
		cg.engine = ENG_RCS;
		cg.failed = FALSE;
		cg.rc_passed = check_render();
	}
	ck_free();

	/* the render engine when it passed and is quicker somewhere, else the copy engine likewise */
	if ( cg.rc_passed && ( cg.thr_plain != (UD)-1 || cg.thr_fx != (UD)-1 ) ) {
		cg.engine = ENG_RCS;
	} else if ( cg.passed && cg.thr != (UD)-1 ) {
		cg.engine = ENG_BCS;
	} else {
		cg.engine = ENG_NONE;
	}
	cg.failed = FALSE;
	tm_printf((UB *)"igt compose: the windows are composited by %s\n",
		  ( cg.engine == ENG_RCS ) ? "the render engine" : ( cg.engine == ENG_BCS ) ? "the copy engine"
		  : "the processor");
	if ( cg.engine != ENG_NONE ) cp_gpu_set(&cg_ops);
	return ok || cg.rc_passed;
}

LOCAL BOOL check_scenes( void )
{
	static CONST T_DPRECT	all = { 0, 0, CK_W, CK_H }, part = { 37, 21, 611, 433 };
	T_CPWIN		w[24];
	UD		g64, c64, gall, call;
	INT		i;
	BOOL		ok = TRUE;

	ck.nscene = ck.npass = 0;
	tm_printf((UB *)"igt compose: check start\n");

	ok &= ck_scene("ground", &all, w, 0);
	ok &= ck_scene("ground in a part", &part, w, 0);
	ck_win(&w[0], 0, 100, 80, 500, 400);
	ok &= ck_scene("one window", &all, w, 1);
	ck_win(&w[1], 1, 350, 250, 600, 480);			/* past the right and bottom edges of the screen */
	ok &= ck_scene("two, one past the edge", &all, w, 2);
	ok &= ck_scene("two in a part", &part, w, 2);
	ck_win(&w[0], 0, -40, -30, 300, 200);			/* past the left and top edges */
	ok &= ck_scene("past the left and top", &all, w, 2);
	for ( i = 0; i < 24; i++ ) ck_win(&w[i], i, ( i * 37 ) % 900, ( i * 53 ) % 700, 120 + i * 7, 90 + i * 5);
	ok &= ck_scene("many", &all, w, 24);
	ck_win(&w[0], 0, 100, 80, 500, 400);
	ck_win(&w[1], 1, 200, 150, 400, 300);
	ck_win(&w[2], 2, 300, 200, 300, 300);
	w[2].round = 8;
	w[2].shd_dark = 120;
	w[2].shd_dx = 6;
	w[2].shd_dy = 8;
	w[2].shd_size = 10;
	ck_win(&w[3], 0, 50, 400, 300, 200);
	ok &= ck_scene("plain, then a look", &all, w, 4);
	w[0].a_frame = 200;
	w[0].wash = 0x202838;
	w[0].wash_str = 90;
	ok &= ck_scene("a look first", &all, w, 4);

	if ( !ok ) {
		tm_printf((UB *)"igt compose: check FAILED (%d/%d scenes the same)%s\n", ck.npass, ck.nscene,
			  cg.flush ? ": compositing stays on the processor" : "");
		return FALSE;
	}

	ck_time(64, 64, &g64, &c64);
	ck_time(CK_W, CK_H, &gall, &call);
	if ( gall >= call ) {
		cg.thr = (UD)-1;
	} else if ( g64 <= c64 ) {
		cg.thr = 0;
	} else {
		/* where the two lines cross, from 64 x 64 to the whole */
		UD	px0 = 64 * 64, px1 = (UD)CK_W * CK_H;
		D	dg = (D)gall - (D)g64, dc = (D)call - (D)c64;

		cg.thr = ( dc - dg > 0 ) ? px0 + (UD)( ( (D)g64 - (D)c64 ) * (D)( px1 - px0 ) / ( dc - dg ) ) : (UD)-1;
	}
	cg.passed = TRUE;
	tm_printf((UB *)"igt compose: time: 64x64 copy engine %ld us / processor %ld us, %dx%d %ld / %ld us\n",
		  g64, c64, CK_W, CK_H, gall, call);
	tm_printf((UB *)"igt compose: check passed (%d/%d%s): %s\n", ck.npass, ck.nscene,
		  cg.flush ? ", the caches flushed" : "",
		  ( cg.thr == (UD)-1 ) ? "the processor is quicker than the copy engine" : "the copy engine may be used");
	if ( cg.thr != (UD)-1 ) {
		tm_printf((UB *)"igt compose: parts of %ld pixels and more on the copy engine\n", cg.thr);
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the render engine's check */

/* A look: frames seen through as 'a_frame' says, corners, spread, shadow */
LOCAL void ck_look( T_CPWIN *w, UINT a_frame, INT round, INT blur, UINT dark )
{
	w->a_frame = a_frame;
	w->round = round;
	w->blur = blur;
	w->wash = 0x202838;
	w->wash_str = 90;
	w->bar_wash = 0x405878;
	w->bar_str = 120;
	w->tint = 0x336699;
	w->tint_str = 50;
	if ( dark != 0 ) {
		w->shd_dx = 6;
		w->shd_dy = 8;
		w->shd_size = 10;
		w->shd_dark = dark;
	}
}

/* The least of three times of a part either way */
LOCAL void rk_time( CONST T_DPRECT *box, CONST T_CPWIN *w, INT n, UD *p_gpu, UD *p_cpu )
{
	UD	t0, g = (UD)-1, c = (UD)-1, t;
	INT	k;

	for ( k = 0; k < 3; k++ ) {
		knl_wm_hold(TRUE);
		t0 = igt_now_us();
		ck_cpu(box, w, n);
		t = igt_now_us() - t0;
		if ( t < c ) c = t;
		t0 = igt_now_us();
		(void)ck_gpu(box, w, n);
		t = igt_now_us() - t0;
		if ( t < g ) g = t;
		knl_wm_hold(FALSE);
	}
	*p_gpu = g;
	*p_cpu = c;
}

/* From the times of a small and a whole part, the size from which the render engine is quicker */
LOCAL UD rk_threshold( CONST char *what, CONST T_CPWIN *w, INT n )
{
	static CONST T_DPRECT	part[2] = { { 256, 192, 320, 256 }, { 0, 0, CK_W, CK_H } };
	UD	g[2], c[2], a[2], thr;
	INT	i;

	for ( i = 0; i < 2; i++ ) {
		rk_time(&part[i], w, n, &g[i], &c[i]);
		a[i] = (UD)( part[i].right - part[i].left ) * (UD)( part[i].bottom - part[i].top );
	}
	if ( g[1] >= c[1] ) {
		thr = (UD)-1;
	} else if ( g[0] <= c[0] ) {
		thr = a[0];
	} else {
		thr = a[0] + ( g[0] - c[0] ) * ( a[1] - a[0] ) / ( ( c[1] - g[1] ) + ( g[0] - c[0] ) );
	}
	tm_printf((UB *)"igt compose: time %s: 64x64 render engine %ld us / processor %ld us, %dx%d %ld / %ld us; %s",
		  what, g[0], c[0], CK_W, CK_H, g[1], c[1], ( thr == (UD)-1 ) ? "the processor at every size\n" : "");
	if ( thr != (UD)-1 ) tm_printf((UB *)"the render engine from %ld pixels\n", thr);
	return thr;
}

/*
 * The scenes of the render engine, each one built by the processor and
 * by the render engine and compared to the top byte: every stage, alone
 * and together, all of the screen and a part of it
 */
LOCAL BOOL check_render( void )
{
	static CONST T_DPRECT	all = { 0, 0, CK_W, CK_H }, part = { 113, 97, 689, 521 };
	T_CPWIN		w[16];
	INT		i;
	BOOL		ok = TRUE;

	ck.nscene = ck.npass = 0;
	tm_printf((UB *)"igt compose: check of the render engine start\n");

	ok &= ck_scene("r ground", &all, w, 0);
	ok &= ck_scene("r ground in a part", &part, w, 0);
	ck_win(&w[0], 0, 40, 30, 500, 380);
	ok &= ck_scene("r one window", &all, w, 1);
	ck_win(&w[1], 1, 300, 200, 560, 420);
	ok &= ck_scene("r plain", &all, w, 2);
	ck_look(&w[0], 255, 0, 0, 0);
	ck_look(&w[1], 255, 0, 0, 0);
	ok &= ck_scene("r washed", &all, w, 2);
	ck_look(&w[0], 150, 0, 0, 0);
	ck_look(&w[1], 150, 0, 0, 0);
	w[1].a_work = 200;
	ok &= ck_scene("r see-through", &all, w, 2);
	ck_win(&w[0], 0, 40, 30, 500, 380);
	ck_win(&w[1], 1, 300, 200, 560, 420);
	ck_look(&w[0], 255, 12, 0, 150);
	ck_look(&w[1], 170, 12, 0, 150);
	ok &= ck_scene("r corners", &all, w, 2);
	ck_win(&w[0], 0, 40, 30, 500, 380);
	ck_win(&w[1], 1, 300, 200, 560, 420);
	ck_win(&w[2], 2, 160, 120, 420, 300);
	for ( i = 0; i < 3; i++ ) ck_look(&w[i], 140, 8, 6, 120);
	w[2].a_work = 190;
	ok &= ck_scene("r spread", &all, w, 3);
	ok &= ck_scene("r spread in a part", &part, w, 3);
	ck_win(&w[0], 0, 40, 30, 500, 380);
	ck_look(&w[0], 255, 10, 0, 150);
	ck_win(&w[1], 1, 300, 200, 560, 420);
	ck_look(&w[1], 200, 10, 0, 0);
	w[1].shown.left = 328;
	w[1].shown.top = 221;
	w[1].shown.right = 832;
	w[1].shown.bottom = 599;
	w[1].fade = 160;
	ok &= ck_scene("r opening", &all, w, 2);
	for ( i = 0; i < 16; i++ ) {
		ck_win(&w[i], i, 20 + ( i * 70 ) % 640, 10 + ( i * 50 ) % 480, 300 + ( i % 3 ) * 40, 200 + ( i % 4 ) * 30);
		ck_look(&w[i], ( i % 2 ) ? 160 : 255, i % 13, ( i % 4 == 1 ) ? 4 : 0, ( i % 3 ) ? 100 : 0);
	}
	ok &= ck_scene("r many", &all, w, 16);
	if ( !ok || cg.failed ) {
		tm_printf((UB *)"igt compose: check of the render engine FAILED (%d/%d scenes the same)%s\n", ck.npass,
			  ck.nscene, cg.failed ? ", a job failed" : "");
		return FALSE;
	}

	ck_win(&w[0], 0, 40, 30, 500, 380);
	ck_win(&w[1], 1, 300, 200, 560, 420);
	cg.thr_plain = rk_threshold("plain", w, 2);
	ck_look(&w[0], 170, 12, 0, 150);
	ck_look(&w[1], 170, 12, 6, 150);
	cg.thr_fx = rk_threshold("with a look", w, 2);
	tm_printf((UB *)"igt compose: check of the render engine passed (%d/%d%s)\n", ck.npass, ck.nscene,
		  cg.flush ? ", the caches flushed" : "");
	return TRUE;
}

#endif /* X64 */
