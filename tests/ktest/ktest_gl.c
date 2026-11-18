/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_gl.c
 *	The GPU for processes and OpenGL ES (docs/tessronos/13-gpu.md 13.5,
 *	13.6), through two programs started as processes:
 *
 *	/boot/GPPROG.ELF (tests/uprog/gpprog.c, always built) reaches SVC
 *	class 10 itself: its contexts and BOs, their mappings and names,
 *	jobs on a backend of the test's own, and BOs it leaves held when it
 *	ends, which the kernel lets go of. The backend gives each context
 *	an address space of its own (a V3D page table, as the board's does):
 *	the test looks, as each job is started, at what the job's table has
 *	at the addresses the job names -- the context's own pages, never
 *	another context's, never a BO of the kernel's.
 *
 *	The same program gives a window of its own two BOs to show its work
 *	area from (gl_gsurf, OB_WR_GSURF), and the test looks at the screen.
 *
 *	/boot/GLPROG.ELF (tests/uprog/glprog.c, made with make GL=1) draws
 *	with Mesa -- softpipe, as the machine has no GPU -- and reads what it
 *	drew back: a triangle, the depth test, textures with mipmaps, GLSL
 *	ES 3.00, a framebuffer object with no surface, a window of its own;
 *	and times frames. Against the DRM shim's pretended V3D, it checks
 *	that the platform chooses the driver v3d and makes its screen.
 *	Skipped when the program is not on the volume.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/gpu.h>
#include <ts/ob.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <ts/wm.h>
#include "../../device/gpu/gpudev.h"
#include "../../device/gpu/v3d/v3d.h"

IMPORT UD knl_pf_free_count( INT zone );
#if TS_NETSTACK_NETBSD
IMPORT UD knl_rump_pages( void );
#endif

#define GP_PROG		"/boot/GPPROG.ELF"
#define GL_PROG		"/boot/GLPROG.ELF"
#define GP_TMO		60000		/* ms */
#define GL_TMO		600000		/* ms: softpipe under TCG */

IMPORT BOOL knl_pt_reclaim( void );

/*
 * The free pages, the network stack's own counted as free (its pools grow
 * now and then for nothing the test did), once the page tables taken out
 * of spaces before are given back (knl_pt_reclaim gives them back only
 * when every other processor has been seen past them)
 */
LOCAL UD pages_free( void )
{
	INT	i;
#if TS_NETSTACK_NETBSD
	UD	r, f;
#endif

	for ( i = 0; i < 50 && knl_pt_reclaim(); i++ ) tk_dly_tsk(20);
#if TS_NETSTACK_NETBSD
	do {
		r = knl_rump_pages();
		f = knl_pf_free_count(-1);
	} while ( knl_rump_pages() != r );
	return f + r;
#else
	return knl_pf_free_count(-1);
#endif
}

/*
 * The free pages once what an ended process held is all back: the reaper
 * gives back its space after ts_wai_prc has answered, and the page tables
 * taken out of a space are given back only when every other processor
 * has been seen past them (knl_pt_reclaim). Waited for up to 5 s until
 * the count is 'before' again; the count then, and how long it took.
 */
LOCAL UD pages_settled( UD before )
{
	UD	now = 0;
	INT	t;

	for ( t = 0; t < 50; t++ ) {
		now = pages_free();
		if ( now == before ) break;
		tk_dly_tsk(100);
	}
	tm_printf((UB*)"  free pages %d -> %d, after %d ms\n", (INT)before, (INT)now, t * 100);
	return now;
}

LOCAL BOOL present( CONST char *path )
{
	T_FSTAT	st;

	if ( fs_stat(path, &st) < EX_OK ) return FALSE;
	tm_printf((UB*)"  %s: %d bytes\n", path, (INT)st.size);
	return TRUE;
}

/* The program run to its end with the argument; its exit code, or -1 */
LOCAL INT run( CONST char *prog, CONST char *mode, TMO tmo )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	SZ	n = 0;
	ID	pid;
	ER	er;

	while ( mode[n] != '\0' ) n++;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = (void *)mode;
	cprc.argsz  = n;
	pid = ts_cre_prc(prog, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return -1;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, tmo);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -1;
	}
	return psts.exitcd;
}

/* ---------------------------------------------------------------- gp_ */

/* Contexts, BOs, mappings and names; nothing left open after */
LOCAL void gl_gp_bo( void )
{
	INT	bos = -1;
	UD	before;
	INT	code;

	if ( !present(GP_PROG) ) KT_SKIP("no " GP_PROG);
	before = pages_free();
	code = run(GP_PROG, "bo", GP_TMO);
	tm_printf((UB*)"  exit code %d\n", code);
	KT_ASSERT_EQ(code, 0);
	KT_ASSERT_EQ(knl_gpu_ctx_count(&bos), 0);
	KT_ASSERT_EQ(bos, 0);
	KT_ASSERT_EQ(pages_settled(before), before);
}

/*
 * A backend that runs every job at once, gives the kernel's BOs addresses
 * one after another, and gives each context a page table of its own
 * (v3d_mmu.c, 64MB of addresses)
 */
#define JB_AS_VA	( 64UL * 1024 * 1024 )
#define JB_SEEN		8

typedef struct {
	T_V3DMMU	mmu;
} JBAS;

LOCAL UW	jb_next;
LOCAL INT	jb_kicked;
LOCAL INT	jb_as_live;
LOCAL struct {
	BOOL	own;		/* kicked in a space of its own */
	UW	bcl;		/* the entry at its binner list's address */
	UW	rcl;		/* and at its renderer list's */
} jb_seen[JB_SEEN];

LOCAL ER jb_info( void *priv, T_GPUINFO *info )
{
	info->kind = GPU_KIND_TEST;
	info->feature = GPU_FEAT_CL | GPU_FEAT_CACHE;
	info->va_size = 0x40000000;
	return E_OK;
}

LOCAL ER jb_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset )
{
	*p_offset = jb_next;
	jb_next += (UW)( npages << 12 );
	return E_OK;
}

LOCAL void jb_unbind( void *priv, UW offset, UD npages )
{
}

LOCAL ER jb_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	JBAS	*a = (JBAS *)as;

	if ( jb_kicked < JB_SEEN ) {
		jb_seen[jb_kicked].own = ( a != NULL );
		jb_seen[jb_kicked].bcl = ( a != NULL ) ? v3d_mmu_lookup(&a->mmu, job->u.cl.bcl_start) : 0;
		jb_seen[jb_kicked].rcl = ( a != NULL ) ? v3d_mmu_lookup(&a->mmu, job->u.cl.rcl_start) : 0;
	}
	jb_kicked++;
	knl_gpu_job_end(E_OK);
	return E_OK;
}

LOCAL ER jb_as_create( void *priv, void **p_as )
{
	JBAS	*a = (JBAS *)Kmalloc(sizeof(JBAS));
	ER	er;

	if ( a == NULL ) return E_NOMEM;
	knl_memset(a, 0, sizeof(*a));
	er = v3d_mmu_init(&a->mmu, JB_AS_VA);
	if ( er < E_OK ) {
		Kfree(a);
		return er;
	}
	jb_as_live++;
	*p_as = a;
	return E_OK;
}

LOCAL void jb_as_destroy( void *priv, void *as )
{
	v3d_mmu_fini(&((JBAS *)as)->mmu);
	Kfree(as);
	jb_as_live--;
}

LOCAL ER jb_as_bind( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va )
{
	JBAS	*a = (JBAS *)as;
	ER	er = v3d_mmu_alloc(&a->mmu, npages, 1, p_va);

	if ( er < E_OK ) return er;
	v3d_mmu_insert(&a->mmu, *p_va, knl_gpu_bo_pages(bo), npages, TRUE);
	return E_OK;
}

LOCAL void jb_as_unbind( void *priv, void *as, UW va, UD npages )
{
	JBAS	*a = (JBAS *)as;

	v3d_mmu_remove(&a->mmu, va, npages);
	v3d_mmu_free(&a->mmu, va, npages);
}

LOCAL void jb_reset( void *priv )
{
}

LOCAL CONST T_GPUOPS	jb_ops = {
	GPU_KIND_TEST, GPU_FEAT_CL | GPU_FEAT_CACHE, 0,
	jb_info, jb_bind, jb_unbind, jb_kick, jb_reset,
	jb_as_create, jb_as_destroy, jb_as_bind, jb_as_unbind
};

/* Hex digits of v at the end of the string */
LOCAL void put_hex( char *d, UW v )
{
	INT	i;

	for ( i = 7; i >= 0; i-- ) {
		d[i] = "0123456789abcdef"[v & 0xF];
		v >>= 4;
	}
	d[8] = '\0';
}

/*
 * Jobs from a process: fences, waits, numbers not its own refused; each
 * context's jobs in its own space, which holds its BOs and nothing of
 * the kernel's or of the other context's
 */
LOCAL void gl_gp_job( void )
{
	T_GPUBO		*kbo;
	T_GPUBOINFO	ki;
	char		arg[16] = "job ";
	INT		code, bos = -1, i;
	ER		er;

	if ( !present(GP_PROG) ) KT_SKIP("no " GP_PROG);
	jb_next = 0x100000;
	jb_kicked = 0;
	jb_as_live = 0;
	knl_memset(jb_seen, 0, sizeof(jb_seen));
	er = knl_gpu_register(&jb_ops, NULL);
	if ( er == E_OBJ ) KT_SKIP("a GPU is registered already");
	KT_ASSERT_ER(er, E_OK);

	/* a BO of the kernel's, whose address the program names in its jobs */
	kbo = knl_gpu_bo_create(4096, 0, &er);
	KT_ASSERT(kbo != NULL);
	if ( kbo == NULL ) goto out;
	knl_gpu_bo_ref(kbo, &ki);
	put_hex(arg + 4, ki.offset);
	code = run(GP_PROG, arg, GP_TMO);
	tm_printf((UB*)"  exit code %d, %d jobs run, the kernel's BO at %08x\n", code, jb_kicked, ki.offset);
	for ( i = 0; i < jb_kicked && i < JB_SEEN; i++ ) {
		tm_printf((UB*)"  job %d: own space %d, entries %08x at its lists, %08x at the kernel's BO\n",
			  i, jb_seen[i].own, jb_seen[i].bcl, jb_seen[i].rcl);
	}
	KT_ASSERT_EQ(code, 0);
	KT_ASSERT_EQ(jb_kicked, 3);			/* the jobs of control lists */
	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT(jb_seen[i].own);		/* never in the kernel's space */
		KT_ASSERT(jb_seen[i].bcl != 0);		/* its own BO is there */
	}
	/* the two contexts' BOs at the same address are different pages */
	KT_ASSERT(jb_seen[1].bcl != jb_seen[2].bcl);
	/* the kernel's BO is in neither */
	KT_ASSERT_EQ(jb_seen[1].rcl, 0);
	KT_ASSERT_EQ(jb_seen[2].rcl, 0);
	knl_gpu_bo_put(kbo);
out:
	KT_ASSERT_EQ(knl_gpu_ctx_count(&bos), 0);
	KT_ASSERT_EQ(jb_as_live, 0);			/* the spaces went with the contexts */
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
}

/* A process that ends holding BOs in two contexts: all of it let go of */
LOCAL void gl_gp_hold( void )
{
	INT	code, bos = -1;
	UD	before;

	if ( !present(GP_PROG) ) KT_SKIP("no " GP_PROG);
	before = pages_free();
	code = run(GP_PROG, "hold", GP_TMO);
	KT_ASSERT_EQ(code, 0);
	KT_ASSERT_EQ(knl_gpu_ctx_count(&bos), 0);
	KT_ASSERT_EQ(bos, 0);
	KT_ASSERT_EQ(pages_settled(before), before);
}

/* ---------------------------------------------------------------- a work area shown from BOs */

/*
 * A window of a process's shown from two BOs (OB_WR_GSURF; GPPROG.ELF
 * gsurf): the BO itself laid over the work area, its pixels not copied
 * into the window; another BO not shown while a job of the GPU uses it,
 * shown once the job is over, by the job's end alone; the picture going
 * with the window when it moves, under a window in front of it, copied
 * into the window while the window is seen through; the last picture
 * kept when the BOs are let go of; and nothing left -- BOs, pages, the
 * window -- when the window is closed or the process killed. The jobs
 * are on a backend of the test's own, which holds a job until the test
 * lets it end.
 */
#define GS_BLUE		0x0000C0U
#define GS_GREEN	0x00C000U
#define GS_RED		0xC00000U

LOCAL volatile BOOL	hj_hold;
LOCAL volatile INT	hj_kicked;

LOCAL ER hj_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	hj_kicked++;
	if ( !hj_hold ) {
		knl_gpu_job_end(E_OK);
	}
	return E_OK;
}

LOCAL CONST T_GPUOPS	hj_ops = {
	GPU_KIND_TEST, GPU_FEAT_CL | GPU_FEAT_CACHE, 0,
	jb_info, jb_bind, jb_unbind, hj_kick, jb_reset,
	jb_as_create, jb_as_destroy, jb_as_bind, jb_as_unbind
};

LOCAL ID gs_start( void )
{
	T_CPRC	cprc;

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = (void *)"gsurf";
	cprc.argsz  = 5;
	return ts_cre_prc(GP_PROG, &cprc);
}

/* Until the program has made so many semaphores (its steps done); FALSE if it ended first */
LOCAL BOOL gs_steps( ID pid, INT n )
{
	T_RPRC	r;
	INT	t;

	for ( t = 0; t < 400; t++ ) {
		if ( knl_own_list(TK_OWN_SEM, pid, NULL, 0) >= n ) {
			return TRUE;
		}
		if ( ts_ref_prc(pid, &r) < E_OK || r.state != PS_RUNNING ) {
			return FALSE;
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL ER gs_send( ID pid, UINT type )
{
	T_TSMSG	*m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	ER	er;

	if ( m == NULL ) return E_NOMEM;
	knl_memset(m, 0, sizeof(T_TSMSG));
	m->type = type;
	er = ts_snd_msg(pid, m, 1000);
	Kfree(m);
	return er;
}

/* The window the program made */
LOCAL INT gs_window( void )
{
	TS_UUID	*list;
	T_OBREF	r;
	INT	cnt, i, wid = 0;

	if ( ( cnt = kt_lst_obj(OB_T_WINDOW, 0, &list) ) < 0 ) return 0;
	for ( i = 0; i < cnt && wid == 0; i++ ) {
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW
		  && kt_name_is(&list[i], "ktgsurf") ) {
			wid = wm_obj_wid(&list[i]);
		}
	}
	Kfree(list);
	return wid;
}

/* The screen's pixel at (x, y) of a window's work area, as built */
LOCAL UW gs_px( INT wid, INT x, INT y )
{
	T_WMWIN		ref;
	T_DISPSPEC	spec;
	UB		*fb;

	if ( wm_ref(wid, &ref) < E_OK || ts_disp_ref(&spec) < E_OK
	  || ( fb = (UB *)ts_disp_buffer() ) == NULL ) return 0xFFFFFFFF;
	return ((UW *)( fb + (UD)( ref.work.top + y ) * spec.pitch ))[ref.work.left + x] & 0x00FFFFFF;
}

LOCAL UW gs_own( INT wid, INT x, INT y )
{
	UW	px = 0xFFFFFFFF;

	(void)knl_wm_gsurf_own_px(wid, x, y, &px);
	return px;
}

/* Until the window shows BO idx under serial, up to 5 s */
LOCAL BOOL gs_shown( INT wid, INT idx, UW serial )
{
	T_OBWGSTAT	st;
	INT		t;

	for ( t = 0; t < 100; t++ ) {
		if ( wm_gsurf_ref(wid, &st) >= E_OK && st.shown == idx && st.shown_serial == serial ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL void gl_gsurf( void )
{
	T_OBWGSTAT	st;
	T_WMWIN		ref;
	T_DPRECT	o;
	T_PSTS		psts;
	T_GPUJOB	job;
	T_GPUBO		*bo1 = NULL;
	UD		before;
	INT		n0, wid, cw, mx, my, t, bos = -1, lk_look, lk_whole;
	D		fence;
	ID		pid;
	ER		er;

	if ( !present(GP_PROG) ) KT_SKIP("no " GP_PROG);
	hj_hold = FALSE;
	hj_kicked = 0;
	jb_next = 0x100000;
	jb_as_live = 0;
	er = knl_gpu_register(&hj_ops, NULL);
	if ( er == E_OBJ ) KT_SKIP("a GPU is registered already");
	KT_ASSERT_ER(er, E_OK);
	before = pages_free();
	n0 = knl_wm_gsurf_count();

	/* given, and BO 0 shown: laid itself, not copied into the window */
	pid = gs_start();
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) goto out;
	KT_ASSERT(gs_steps(pid, 1));
	wid = gs_window();
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) goto kill;
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0 + 1);
	KT_ASSERT_ER(wm_ref(wid, &ref), E_OK);
	mx = ( ref.work.right - ref.work.left ) / 2;
	my = ( ref.work.bottom - ref.work.top ) / 2;
	KT_ASSERT_ER(wm_gsurf_ref(wid, &st), E_OK);
	KT_ASSERT(st.set && st.shown == 0 && st.laid);
	KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);
	KT_ASSERT_EQ(gs_px(wid, 0, 0), GS_BLUE);
	KT_ASSERT(gs_own(wid, mx, my) != GS_BLUE);	/* nothing was copied */

	/* BO 1 shown while a job of the GPU uses it: not until the job is over */
	KT_ASSERT_ER(knl_wm_gsurf_bo(wid, 1, &bo1), E_OK);
	knl_memset(&job, 0, sizeof(job));
	job.kind = GPU_JOB_CL;
	hj_hold = TRUE;
	cw = hj_kicked;
	fence = knl_gpu_submit(&job, &bo1, 1);
	KT_ASSERT(fence > 0);
	for ( t = 0; t < 100 && hj_kicked == cw; t++ ) tk_dly_tsk(10);
	KT_ASSERT(hj_kicked > cw);
	KT_ASSERT_ER(gs_send(pid, 1), E_OK);
	for ( t = 0; t < 100; t++ ) {
		if ( wm_gsurf_ref(wid, &st) >= E_OK && st.waiting == 1 ) break;
		tk_dly_tsk(20);
	}
	KT_ASSERT_EQ(st.waiting, 1);
	(void)wm_update();
	tk_dly_tsk(300);
	KT_ASSERT_ER(wm_gsurf_ref(wid, &st), E_OK);
	KT_ASSERT_EQ(st.shown, 0);
	KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_BLUE);	/* the BO being drawn is not seen */
	hj_hold = FALSE;
	knl_gpu_job_end(E_OK);				/* the job's end alone shows it */
	KT_ASSERT_ER(knl_gpu_wait(fence, 2000000), E_OK);
	KT_ASSERT(gs_shown(wid, 1, 2));
	KT_ASSERT(gs_steps(pid, 2));			/* and the program was told */
	KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_GREEN);
	KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);
	tm_printf((UB*)"  BO 1 shown after its job: (20,20) %06x, middle %06x\n",
		  gs_px(wid, 20, 20), gs_px(wid, mx, my));

	/* moved: the picture goes with the window */
	o = ref.outer;
	o.left += 40;  o.right += 40;  o.top += 30;  o.bottom += 30;
	KT_ASSERT_ER(wm_move(wid, &o), E_OK);
	(void)wm_update();
	KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_GREEN);
	KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);
	KT_ASSERT_ER(wm_gsurf_ref(wid, &st), E_OK);
	KT_ASSERT(st.laid);

	/* under a window in front of it: the front window shows where it stands */
	{
		INT	fw;
		T_DPRECT	fo, all;

		KT_ASSERT_ER(wm_ref(wid, &ref), E_OK);
		fo.left = ref.work.left - 20;
		fo.top = ref.work.top - 20;
		fo.right = ref.work.left + 30;
		fo.bottom = ref.work.top + 30;
		fw = wm_open(&fo, 0, "ktgsfront");
		KT_ASSERT(fw > 0);
		if ( fw > 0 ) {
			all.left = 0;  all.top = 0;
			all.right = fo.right - fo.left;  all.bottom = fo.bottom - fo.top;
			(void)dp_fill_rect(wm_gid(fw), &all, GS_RED);
			(void)wm_update();
			KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_RED);
			KT_ASSERT_EQ(gs_px(wid, 40, 35), GS_GREEN);
			KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);
			KT_ASSERT_ER(wm_close(fw), E_OK);
			(void)wm_update();
			KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_GREEN);
		}
	}

	/* seen through: the BO copied into the window, which is laid as before */
	lk_look = wm_get(LK_LOOK);
	lk_whole = wm_get(LK_OPA_WHOLE);
	KT_ASSERT_ER(wm_set(LK_LOOK, 1), E_OK);
	KT_ASSERT_ER(wm_set(LK_OPA_WHOLE, 1), E_OK);
	KT_ASSERT_ER(wm_set_opacity(wid, 128), E_OK);
	(void)wm_composite();
	KT_ASSERT_ER(wm_gsurf_ref(wid, &st), E_OK);
	KT_ASSERT(!st.laid);
	KT_ASSERT_EQ(gs_own(wid, mx, my), GS_BLUE);
	KT_ASSERT_EQ(gs_own(wid, 20, 20), GS_GREEN);
	KT_ASSERT(gs_px(wid, mx, my) != GS_BLUE);
	KT_ASSERT_ER(wm_set_opacity(wid, 255), E_OK);
	(void)wm_set(LK_OPA_WHOLE, ( lk_whole < 0 ) ? 0 : lk_whole);
	(void)wm_set(LK_LOOK, ( lk_look < 0 ) ? 1 : lk_look);
	(void)wm_composite();
	KT_ASSERT_ER(wm_gsurf_ref(wid, &st), E_OK);
	KT_ASSERT(st.laid);
	KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);

	/* let go of: the last picture stays */
	KT_ASSERT_ER(gs_send(pid, 2), E_OK);
	KT_ASSERT(gs_steps(pid, 3));
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0);
	(void)wm_update();
	KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_GREEN);
	KT_ASSERT_EQ(gs_px(wid, mx, my), GS_BLUE);
	KT_ASSERT_EQ(gs_own(wid, mx, my), GS_BLUE);

	/* given again; the window closed under them: they go */
	KT_ASSERT_ER(gs_send(pid, 3), E_OK);
	KT_ASSERT(gs_steps(pid, 4));
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0 + 1);
	KT_ASSERT_EQ(gs_px(wid, 20, 20), GS_BLUE);
	KT_ASSERT_ER(wm_close(wid), E_OK);
	(void)wm_update();
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0);
	KT_ASSERT_ER(gs_send(pid, 4), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(pages_settled(before), before);

	/* killed while it shows them: nothing is left */
	pid = gs_start();
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) goto out;
	KT_ASSERT(gs_steps(pid, 1));
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0 + 1);
kill:
	KT_ASSERT_ER(ts_ter_prc(pid, -7), E_OK);
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	KT_ASSERT_EQ(knl_wm_gsurf_count(), n0);
	KT_ASSERT_EQ(gs_window(), 0);
	KT_ASSERT_EQ(pages_settled(before), before);
	KT_ASSERT_EQ(wm_self_check(), 0);
out:
	hj_hold = FALSE;
	KT_ASSERT_EQ(knl_gpu_ctx_count(&bos), 0);
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
}

/* ---------------------------------------------------------------- OpenGL ES */

LOCAL BOOL	have_gl;

LOCAL void gl_present( void )
{
	have_gl = present(GL_PROG);
	if ( !have_gl ) KT_SKIP("no " GL_PROG " (make GL=1)");
}

LOCAL void gl_mode( CONST char *mode )
{
	INT	code;

	if ( !have_gl ) KT_SKIP("no " GL_PROG);
	code = run(GL_PROG, mode, GL_TMO);
	KT_ASSERT_EQ(code, 0);
}

LOCAL void gl_info( void )	{ gl_mode("info"); }
LOCAL void gl_depth( void )	{ gl_mode("depth"); }
LOCAL void gl_tex( void )	{ gl_mode("tex"); }
LOCAL void gl_glsl( void )	{ gl_mode("glsl"); }
LOCAL void gl_fbo( void )	{ gl_mode("fbo"); }
LOCAL void gl_speed( void )	{ gl_mode("speed"); }

/* The driver v3d chosen and its screen made, against the DRM shim's pretended V3D */
LOCAL void gl_v3dpath( void )	{ gl_mode("v3dpath"); }

/* A triangle; and the pages of the run all come back */
LOCAL void gl_tri( void )
{
	UD	before;

	if ( !have_gl ) KT_SKIP("no " GL_PROG);
	before = pages_free();
	gl_mode("tri");
	KT_ASSERT_EQ(pages_settled(before), before);
}

/* A window of its own: the program reads its pixels back */
LOCAL void gl_win( void )
{
	if ( !kt_screen() ) KT_SKIP(KT_NO_SCREEN);
	gl_mode("win");
}

EXPORT void ktest_gl( void )
{
	KT_RUN(gl_gp_bo);
	KT_RUN(gl_gp_job);
	KT_RUN(gl_gp_hold);
	KT_RUN_SCREEN(gl_gsurf);
	KT_RUN(gl_present);
	KT_RUN(gl_info);
	KT_RUN(gl_tri);
	KT_RUN(gl_depth);
	KT_RUN(gl_tex);
	KT_RUN(gl_glsl);
	KT_RUN(gl_fbo);
	KT_RUN(gl_win);
	KT_RUN(gl_v3dpath);
	KT_RUN(gl_speed);
}
