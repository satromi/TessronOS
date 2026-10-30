/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_rcompose.c
 *	Compositing the windows on the render engine (docs/tessronos/
 *	13-gpu.md 13.11.14): a part of the screen planned as rectangles
 *	(igt_rplan.c), each a compute shader of mesa/igt_cp_shaders.h, and
 *	the rectangles turned into one batch of the GPGPU pipeline, run and
 *	waited for. igt_compose.c chooses this or the copy engine, and checks
 *	either against the processor before it uses it.
 *
 *	The batch: the caches written back and the pipe idle, the pipeline
 *	turned to GPGPU, the bases of the state (the instructions at the
 *	code, the dynamic state at the memory of the descriptors and the
 *	push constants), the read-only caches invalidated, MEDIA_VFE_STATE
 *	(threads for every subslice, push constants of 8 registers), the
 *	interface descriptors of all the shaders loaded once; then for each
 *	rectangle, after a barrier when it begins a group, its push
 *	constants loaded (MEDIA_CURBE_LOAD), GPGPU_WALKER over its thread
 *	groups with its shader's descriptor, MEDIA_STATE_FLUSH; last the
 *	caches written back again. Every command is a packed one of the
 *	generated header, with only the fields it marks filled in.
 *
 *	Memory. The code and the descriptors are written once; the push
 *	constants and the batch for each part. They are the kernel's pages,
 *	entered in the kernel's space of the GPU, and written back from the
 *	processor's caches before the job (the command streamer and the
 *	fixed functions that read them need not see those). The back buffer,
 *	the ground and the windows' pixels are entered as the copy engine's
 *	are (igt_compose.c); the shaders reach them through the last level
 *	cache the processor shares.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"
#include "igt.h"
#define IGT_CP_WANT_CODE
#include "igt_rplan.h"
#include "igt_cp.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../v3d/v3d_cp_split.h"
#pragma GCC diagnostic pop

#define DYN_IDD		0			/* the interface descriptors */
#define DYN_SYNC	512			/* the word an end of pipe sync writes */
#define DYN_COV		1024			/* the corners' cover, each radius */
#define DYN_CURBE	( 64 * 1024 )		/* the push constants of each rectangle */
#define CURBE_BYTES	( IGT_CP_NPUSH * 4 )
#define DYN_BYTES	( DYN_CURBE + IGT_RD_MAX * CURBE_BYTES )
#define BAT_BYTES	( 256 * 1024 )
#define RECT_WORDS	( IGT_CP_LEN_PIPE_CONTROL + IGT_CP_LEN_MEDIA_CURBE_LOAD + IGT_CP_LEN_GPGPU_WALKER \
			  + IGT_CP_LEN_MEDIA_STATE_FLUSH )

/* The kernel's memory, entered in the GPU's space as one BO */
typedef struct {
	UB		*p;
	UD		npages;
	T_GPUBO		*bo;
	UW		gpu;
} KMEM;

LOCAL struct {
	BOOL		ready;
	KMEM		code, dyn, bat;
	T_GPUBO		*t1, *t2;
	UD		t_bytes;
	IGT_RD		*rd;
	IGT_RENV	env;
	UW		threads;
	T_GPUBO		*bos[IGT_CP_MAXOPS + 16];
	INT		nbo;
	INT		nrd, ngroup;		/* of the last part */
} rc;

/* ---------------------------------------------------------------- memory */

LOCAL ER kmem_alloc( KMEM *m, UD bytes )
{
	UD	*pa, i;
	ER	er = E_NOMEM;

	m->npages = ( bytes + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	m->p = (UB *)knl_vmap(m->npages, 0);
	if ( m->p == NULL ) return E_NOMEM;
	knl_memset(m->p, 0, (SZ)( m->npages << PAGE_SHIFT ));
	pa = (UD *)Kmalloc(sizeof(UD) * m->npages);
	if ( pa == NULL ) return E_NOMEM;
	for ( i = 0; i < m->npages; i++ ) {
		pa[i] = knl_pt_lookup(knl_pt_l0_k_pa, (UBINT)m->p + ( i << PAGE_SHIFT ));
		if ( pa[i] == ~(UD)0 ) {
			Kfree(pa);
			return E_SYS;
		}
	}
	m->bo = knl_gpu_bo_wrap(pa, m->npages, &er);
	Kfree(pa);
	if ( m->bo == NULL ) return er;
	{
		T_GPUBOINFO	bi;

		(void)knl_gpu_bo_ref(m->bo, &bi);
		m->gpu = bi.offset;
	}
	return E_OK;
}

/* The levels of a spread for a back buffer of 'rows' rows of 'pitch' bytes; FALSE when there is no room */
LOCAL BOOL spread_room( UINT pitch, INT rows )
{
	UD		want = cp_spread_bytes(pitch, rows) + PAGE_SIZE;
	T_GPUBOINFO	bi;
	ER		er = E_OK;

	if ( rc.t1 != NULL && rc.t_bytes >= want ) return TRUE;
	if ( rc.t1 != NULL ) knl_gpu_bo_put(rc.t1);
	if ( rc.t2 != NULL ) knl_gpu_bo_put(rc.t2);
	rc.t2 = NULL;
	rc.t1 = knl_gpu_bo_create((SZ)want, 0, &er);
	if ( rc.t1 != NULL ) rc.t2 = knl_gpu_bo_create((SZ)want, 0, &er);
	if ( rc.t2 == NULL ) {
		if ( rc.t1 != NULL ) knl_gpu_bo_put(rc.t1);
		rc.t1 = NULL;
		rc.t_bytes = 0;
		rc.env.t1 = rc.env.t2 = 0;
		tm_printf((UB *)"igt rcompose: no memory to spread what shows through (%d)\n", er);
		return FALSE;
	}
	rc.t_bytes = want;
	(void)knl_gpu_bo_ref(rc.t1, &bi);
	rc.env.t1 = bi.offset;
	(void)knl_gpu_bo_ref(rc.t2, &bi);
	rc.env.t2 = bi.offset;
	return TRUE;
}

LOCAL void bo_use( T_GPUBO *bo )
{
	INT	i;

	if ( bo == NULL ) return;
	for ( i = 0; i < rc.nbo; i++ ) {
		if ( rc.bos[i] == bo ) return;
	}
	if ( rc.nbo < (INT)( sizeof(rc.bos) / sizeof(rc.bos[0]) ) ) rc.bos[rc.nbo++] = bo;
}

/* The plan's way to the GPU address of memory: entered as the copy engine's, its BO used by the job */
LOCAL UD gpu_of( void *ctx, CONST void *p, UD len )
{
	T_GPUBO	*bo = NULL;
	UW	g = igt_cp_wrap(p, len, &bo);

	(void)ctx;
	if ( g == 0 ) return 0;
	bo_use(bo);
	return (UD)g + ( (UBINT)p & ( PAGE_SIZE - 1 ) );
}

/* ---------------------------------------------------------------- the batch */

LOCAL UW *put( UW *b, CONST UW *t, INT n )
{
	knl_memcpy(b, t, (SZ)n * 4);
	return b + n;
}

Inline void field( UW *c, INT dw, INT shift, UW mask, UW v )
{
	c[dw] = ( c[dw] & ~mask ) | ( ( v << shift ) & mask );
}

Inline void address( UW *c, INT dw, UD a )
{
	c[dw] += (UW)a;
	c[dw + 1] += (UW)( a >> 32 );
}

/* An end of pipe sync: the caches written back, the pipe idle */
LOCAL UW *sync( UW *b )
{
	UW	*c = b;

	b = put(b, igt_cp_sync_flush, IGT_CP_LEN_PIPE_CONTROL);
	address(c, IGT_CP_SYNC_ADDR_DW, (UD)rc.dyn.gpu + DYN_SYNC);
	return b;
}

/* The state every part's batch begins with */
LOCAL UW *prologue( UW *b )
{
	UW	*c;

	b = sync(b);
	b = put(b, igt_cp_invalidate, IGT_CP_LEN_PIPE_CONTROL);
	b = put(b, igt_cp_cc_pointers, IGT_CP_LEN_3DSTATE_CC_STATE_POINTERS);
	b = put(b, igt_cp_select_gpgpu, IGT_CP_LEN_PIPELINE_SELECT);
	b = sync(b);
	c = b;
	b = put(b, igt_cp_base, IGT_CP_LEN_STATE_BASE_ADDRESS);
	address(c, IGT_CP_BASE_SURFACE_DW, rc.dyn.gpu);
	address(c, IGT_CP_BASE_DYNAMIC_DW, rc.dyn.gpu);
	address(c, IGT_CP_BASE_INSTRUCTION_DW, rc.code.gpu);
	address(c, IGT_CP_BASE_BINDLESS_DW, rc.dyn.gpu);
	b = put(b, igt_cp_invalidate, IGT_CP_LEN_PIPE_CONTROL);
	c = b;
	b = put(b, igt_cp_vfe, IGT_CP_LEN_MEDIA_VFE_STATE);
	field(c, IGT_CP_VFE_THREADS_DW, IGT_CP_VFE_THREADS_SHIFT, IGT_CP_VFE_THREADS_MASK, rc.threads - 1);
	c = b;
	b = put(b, igt_cp_idl, IGT_CP_LEN_MEDIA_INTERFACE_DESCRIPTOR_LOAD);
	field(c, IGT_CP_IDL_LENGTH_DW, IGT_CP_IDL_LENGTH_SHIFT, IGT_CP_IDL_LENGTH_MASK, CP_NFS * IGT_CP_IDD_LEN * 4);
	field(c, IGT_CP_IDL_START_DW, IGT_CP_IDL_START_SHIFT, IGT_CP_IDL_START_MASK, DYN_IDD);
	return b;
}

/* One rectangle: its push constants at curbe (from the dynamic state's base), its walk */
EXPORT UW *igt_rc_rect( UW *b, CONST IGT_RD *d, UW curbe )
{
	UW	*c;

	if ( d->barrier ) b = put(b, igt_cp_barrier, IGT_CP_LEN_PIPE_CONTROL);
	c = b;
	b = put(b, igt_cp_curbe, IGT_CP_LEN_MEDIA_CURBE_LOAD);
	field(c, IGT_CP_CURBE_LENGTH_DW, IGT_CP_CURBE_LENGTH_SHIFT, IGT_CP_CURBE_LENGTH_MASK, CURBE_BYTES);
	field(c, IGT_CP_CURBE_START_DW, IGT_CP_CURBE_START_SHIFT, IGT_CP_CURBE_START_MASK, curbe);
	c = b;
	b = put(b, igt_cp_walker, IGT_CP_LEN_GPGPU_WALKER);
	field(c, IGT_CP_WALKER_IDO_DW, IGT_CP_WALKER_IDO_SHIFT, IGT_CP_WALKER_IDO_MASK, d->fs);
	field(c, IGT_CP_WALKER_XDIM_DW, IGT_CP_WALKER_XDIM_SHIFT, IGT_CP_WALKER_XDIM_MASK, igt_rd_groups_x(d));
	field(c, IGT_CP_WALKER_YDIM_DW, IGT_CP_WALKER_YDIM_SHIFT, IGT_CP_WALKER_YDIM_MASK, igt_rd_groups_y(d));
	return put(b, igt_cp_msf, IGT_CP_LEN_MEDIA_STATE_FLUSH);
}

/* The rectangles rc.rd[0..n-1] as a batch: its length in bytes */
LOCAL UW batch( INT n )
{
	UW	*b0 = (UW *)rc.bat.p, *b = b0;
	INT	i;

	b = prologue(b);
	for ( i = 0; i < n; i++ ) {
		knl_memcpy(rc.dyn.p + DYN_CURBE + (UD)i * CURBE_BYTES, rc.rd[i].par, CURBE_BYTES);
		b = igt_rc_rect(b, &rc.rd[i], DYN_CURBE + (UW)i * CURBE_BYTES);
	}
	b = sync(b);
	*b++ = MI_BATCH_BUFFER_END;
	if ( ( b - b0 ) & 1 ) *b++ = MI_NOOP;
	igt_clflush(rc.dyn.p + DYN_CURBE, (UD)n * CURBE_BYTES);
	igt_clflush(b0, (UD)( b - b0 ) * 4);
	return (UW)( ( b - b0 ) * 4 );
}

/* ---------------------------------------------------------------- the part */

/* The caches written back over a rectangle of rows of 'pitch' bytes from p */
LOCAL void rows_flush( CONST UB *p, UINT pitch, INT l, INT t, INT r, INT b )
{
	INT	y;

	for ( y = t; y < b; y++ ) igt_clflush(p + (UBINT)y * pitch + (UBINT)l * 4, (UD)( r - l ) * 4);
}

/*
 * The ground and windows w[0..n-1] of the part d->box laid on the
 * render engine, run and waited for: how many windows were laid, or an
 * error when nothing was (E_LIMIT, E_NOMEM: the plan does not fit; E_IO,
 * E_TMOUT: the job failed). 'flush' when the GPU does not see the
 * processor's caches: they are written back around the job.
 */
EXPORT INT igt_rc_lay( CONST T_CPDST *d, CONST UW *ground, UINT gpitch, CONST T_CPWIN *w, INT n, BOOL flush )
{
	INT	laid, nrd, i, k;
	UW	len;
	ER	er;

	if ( !rc.ready ) return E_NOSPT;
	igt_cp_new_part();
	rc.nbo = 0;
	for ( i = 0; i < n; i++ ) {
		if ( !CP_MOVING(&w[i]) && w[i].blur > 0 && ( w[i].a_frame < 255 || w[i].a_work < 255 ) ) {
			(void)spread_room(d->pitch, d->rows);
			break;
		}
	}
	laid = igt_rplan(d, ground, gpitch, w, n, &rc.env, rc.rd, IGT_RD_MAX, &nrd);
	if ( laid < 0 ) return laid;
	if ( (UD)nrd * RECT_WORDS * 4 + 1024 > BAT_BYTES ) return E_LIMIT;
	rc.nrd = nrd;
	rc.ngroup = 0;
	for ( i = 0; i < nrd; i++ ) rc.ngroup += rc.rd[i].barrier;
	len = batch(nrd);
	bo_use(rc.code.bo);
	bo_use(rc.dyn.bo);
	bo_use(rc.bat.bo);
	bo_use(rc.t1);
	bo_use(rc.t2);
	if ( flush ) {
		rows_flush(d->back, d->pitch, d->box.left, d->box.top, d->box.right, d->box.bottom);
		rows_flush((CONST UB *)ground, gpitch, d->box.left, d->box.top, d->box.right, d->box.bottom);
		for ( k = 0; k < laid; k++ ) {
			igt_clflush(w[k].pixels, (UD)w[k].pitch * (UD)( w[k].outer.bottom - w[k].outer.top ));
		}
	}
	er = igt_kjob(IGT_RCS, rc.bat.gpu, len, rc.bos, rc.nbo);
	if ( flush ) rows_flush(d->back, d->pitch, d->box.left, d->box.top, d->box.right, d->box.bottom);
	return ( er < E_OK ) ? er : laid;
}

/* Rectangles and groups of the last part, for the record */
EXPORT void igt_rc_last( INT *p_nrd, INT *p_ngroup )
{
	*p_nrd = rc.nrd;
	*p_ngroup = rc.ngroup;
}

/* ---------------------------------------------------------------- setting up */

EXPORT ER igt_rc_init( void )
{
	UD	off = DYN_COV;
	INT	rad, ss;
	ER	er;

	if ( rc.ready ) return E_OK;
	rc.rd = (IGT_RD *)knl_vmap(( sizeof(IGT_RD) * IGT_RD_MAX + PAGE_SIZE - 1 ) >> PAGE_SHIFT, 0);
	if ( rc.rd == NULL ) return E_NOMEM;
	er = kmem_alloc(&rc.code, IGT_CP_CODE_BYTES);
	if ( er >= E_OK ) er = kmem_alloc(&rc.dyn, DYN_BYTES);
	if ( er >= E_OK ) er = kmem_alloc(&rc.bat, BAT_BYTES);
	if ( er < E_OK ) {
		tm_printf((UB *)"igt rcompose: no memory for the code and the batch (%d)\n", er);
		return er;
	}

	/* the code, the descriptors (the code's place from the instructions' base), each radius's cover */
	knl_memcpy(rc.code.p, igt_cp_code, IGT_CP_CODE_BYTES);
	knl_memcpy(rc.dyn.p + DYN_IDD, igt_cp_idd, CP_NFS * IGT_CP_IDD_LEN * 4);
	rc.env.cov[0] = (UD)rc.dyn.gpu + DYN_COV;
	for ( rad = 1; rad <= CP_ROUND_MAX; rad++ ) {
		CONST UB	*t = cp_corner_cov(rad);

		if ( t == NULL ) continue;
		knl_memcpy(rc.dyn.p + off, t, (SZ)( rad * rad ));
		rc.env.cov[rad] = (UD)rc.dyn.gpu + off;
		off = ( off + (UD)( rad * rad ) + 4 + 15 ) & ~15UL;	/* a word read past the last byte stays inside */
	}
	if ( off > DYN_CURBE ) {
		tm_printf((UB *)"igt rcompose: the covers of the corners do not fit\n");
		return E_SYS;
	}
	igt_clflush(rc.code.p, IGT_CP_CODE_BYTES);
	igt_clflush(rc.dyn.p, DYN_CURBE);
	rc.env.gpu = gpu_of;
	rc.env.ctx = NULL;

	/* threads for every subslice the fuses leave */
	ss = 0;
	{
		UW	m;

		for ( m = knl_igt.ss_mask; m != 0; m &= m - 1 ) ss++;
	}
	if ( ss == 0 ) ss = 1;
	rc.threads = (UW)ss * IGT_CP_THREADS_PER_SS;
	rc.ready = TRUE;
	tm_printf((UB *)"igt rcompose: %d shaders, %d bytes of code at %08x, descriptors and push constants at %08x, "
		  "%d threads\n", CP_NFS, IGT_CP_CODE_BYTES, rc.code.gpu, rc.dyn.gpu, rc.threads);
	return E_OK;
}

#endif /* X64 */
