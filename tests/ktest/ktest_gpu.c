/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_gpu.c
 *	The GPU layer without a GPU (docs/tessronos/13-gpu.md 13.7).
 *
 *	What does not need the hardware is tested on any machine: the V3D's
 *	page table and address allocator, buffer objects and their mapping
 *	into a process space, and the queue of jobs and its fences, driven
 *	by a backend the test puts in. That backend keeps its own V3D page
 *	table, so a BO bound through the common layer is checked entry by
 *	entry, and it ends jobs when the test says so, so that a job still
 *	running, one that fails and one that never ends can each be seen.
 *
 *	On a machine where the real GPU is registered already, the part that
 *	needs a backend of its own is skipped.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "ktest.h"
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "sysman/space.h"
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/gpuapp.h>
#include "../../device/gpu/gpudev.h"
#include "../../device/gpu/v3d/v3d.h"
#include "../../device/gpu/v3d/v3d_regs.h"

IMPORT BOOL knl_pt_reclaim( void );
#if TS_NETSTACK_NETBSD
IMPORT UD knl_rump_pages( void );
#endif

/*
 * The free pages with what comes and goes for nothing the test did left
 * out: the page tables taken out of spaces earlier, given back only once
 * every other processor has been seen past them (knl_pt_reclaim, waited
 * for here), and the pages the network stack keeps for its pools, which
 * grow now and then (counted as free, as ktest_proc does).
 */
LOCAL UD free_quiet( void )
{
	INT	i;
	UD	f;
#if TS_NETSTACK_NETBSD
	UD	r;
#endif

	UD	last = 0;
	INT	same = 0;

	/*
	 * Counted once the count stops moving: pages let go of late by
	 * what ran before (page tables reclaimed after the processors have
	 * passed, a job's buffers given back) come home within a few tenths
	 * of a second; the count is taken once it holds still that long.
	 */
	for ( i = 0; i < 50; i++ ) {
		INT	k;

		for ( k = 0; k < 50 && knl_pt_reclaim(); k++ ) tk_dly_tsk(20);
#if TS_NETSTACK_NETBSD
		do {
			r = knl_rump_pages();
			f = knl_pf_free_count(-1);
		} while ( knl_rump_pages() != r );
		f += r;
#else
		f = knl_pf_free_count(-1);
#endif
		same = ( i > 0 && f == last ) ? same + 1 : 0;
		if ( same >= 3 ) break;
		last = f;
		tk_dly_tsk(100);
	}
	return f;
}

#define TEST_VA		( 64UL * 1024 * 1024 )	/* a small GPU address space */
#define MS		1000			/* TMO_U of a millisecond */

/* ---------------------------------------------------------------- the page table */

LOCAL INT	flushes;

LOCAL void count_flush( void *arg )
{
	flushes++;
}

LOCAL void gpu_mmu_table( void )
{
	T_V3DMMU	m;
	UD		pa[4];
	UW		va, va2, va3;
	UD		i, before;

	KT_ASSERT_ER(v3d_mmu_init(&m, 3 * 1024 * 1024), E_PAR);		/* not a power of two */
	KT_ASSERT_ER(v3d_mmu_init(&m, TEST_VA), E_OK);
	m.flush = count_flush;
	flushes = 0;

	KT_ASSERT_EQ(m.npte, TEST_VA >> 12);
	KT_ASSERT_EQ(m.pt_pa & ( ( TEST_VA >> 10 ) - 1 ), 0);		/* the table is aligned to its size */
	KT_ASSERT_EQ(v3d_mmu_lookup(&m, 0x10000), 0);
	KT_ASSERT_EQ(v3d_mmu_lookup(&m, (UW)( TEST_VA - 4096 )), 0);
	before = m.nfree;

	/* never below the reserved bottom, and aligned when asked */
	KT_ASSERT_ER(v3d_mmu_alloc(&m, 4, 1, &va), E_OK);
	KT_ASSERT(va >= V3D_VA_RESERVED);
	KT_ASSERT_ER(v3d_mmu_alloc(&m, 16, 16, &va2), E_OK);
	KT_ASSERT_EQ(va2 & ( 16 * 4096 - 1 ), 0);
	KT_ASSERT(va2 >= va + 4 * 4096);
	KT_ASSERT_EQ(m.nfree, before - 20);

	/* entries: the page number, valid, writable or not */
	for ( i = 0; i < 4; i++ ) pa[i] = 0x123450000ULL + i * 0x3000;
	v3d_mmu_insert(&m, va, pa, 4, TRUE);
	KT_ASSERT_EQ(flushes, 1);
	for ( i = 0; i < 4; i++ ) {
		UW	e = v3d_mmu_lookup(&m, va + (UW)( i * 4096 ));

		KT_ASSERT_EQ(e & V3D_PTE_PFN_MASK, pa[i] >> 12);
		KT_ASSERT(( e & V3D_PTE_VALID ) != 0);
		KT_ASSERT(( e & V3D_PTE_WRITEABLE ) != 0);
	}
	KT_ASSERT_EQ(v3d_mmu_pte(0xFFFFF000ULL, FALSE), 0x000FFFFFU | V3D_PTE_VALID);
	KT_ASSERT_EQ(v3d_mmu_pte(0xFFFFFFF000ULL, TRUE) & V3D_PTE_PFN_MASK, 0x0FFFFFFFU);	/* 40 bits */
	v3d_mmu_remove(&m, va, 4);
	KT_ASSERT_EQ(flushes, 2);
	KT_ASSERT_EQ(v3d_mmu_lookup(&m, va), 0);

	/* what is given back is not given out again at once: the search goes on */
	v3d_mmu_free(&m, va, 4);
	KT_ASSERT_ER(v3d_mmu_alloc(&m, 4, 1, &va3), E_OK);
	KT_ASSERT(va3 != va && va3 > va2);
	v3d_mmu_free(&m, va3, 4);
	v3d_mmu_free(&m, va2, 16);
	KT_ASSERT_EQ(m.nfree, before);

	/* more than there is */
	KT_ASSERT_ER(v3d_mmu_alloc(&m, m.nfree + 1, 1, &va), E_NOMEM);
	KT_ASSERT_ER(v3d_mmu_alloc(&m, m.nfree, 1, &va), E_OK);
	KT_ASSERT_ER(v3d_mmu_alloc(&m, 1, 1, &va2), E_NOMEM);
	v3d_mmu_free(&m, va, before);
	KT_ASSERT_EQ(m.nfree, before);

	v3d_mmu_fini(&m);
}

/* ---------------------------------------------------------------- buffer objects */

LOCAL void gpu_bo_pages( void )
{
	T_GPUBO		*bo;
	T_GPUBOINFO	bi;
	UD		free0 = free_quiet();
	UD		i, j;
	ER		er;

	KT_ASSERT(knl_gpu_bo_create(0, GPU_BO_NOGPU, &er) == NULL);
	KT_ASSERT_ER(er, E_PAR);
	KT_ASSERT(knl_gpu_bo_create(GPU_BO_MAXSIZE + 1, GPU_BO_NOGPU, &er) == NULL);

	bo = knl_gpu_bo_create(10000, GPU_BO_NOGPU, &er);
	KT_ASSERT_ER(er, E_OK);
	if ( bo == NULL ) return;
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT_EQ(bi.size, 3 * 4096);
	KT_ASSERT_EQ(bi.offset, 0);
	KT_ASSERT_EQ(bi.uva, 0);
	KT_ASSERT_EQ(bi.refcnt, 1);
	KT_ASSERT_EQ(knl_gpu_bo_npages(bo), 3);
	for ( i = 0; i < 3; i++ ) {
		CONST UD	*w = (CONST UD *)PA2VA(knl_gpu_bo_page(bo, i));
		BOOL		zero = TRUE;

		KT_ASSERT(knl_gpu_bo_page(bo, i) != 0);
		for ( j = 0; j < i; j++ ) KT_ASSERT(knl_gpu_bo_page(bo, i) != knl_gpu_bo_page(bo, j));
		for ( j = 0; j < 512; j++ ) if ( w[j] != 0 ) zero = FALSE;
		KT_ASSERT(zero);
	}
	KT_ASSERT_EQ(knl_gpu_bo_page(bo, 3), 0);
	KT_ASSERT(free_quiet() + 3 <= free0);

	knl_gpu_bo_get(bo);
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT_EQ(bi.refcnt, 2);
	knl_gpu_bo_put(bo);
	knl_gpu_bo_put(bo);
	KT_ASSERT_EQ(free_quiet(), free0);
}

LOCAL void gpu_bo_map( void )
{
	T_SPACE		sp;
	T_GPUBO		*bo;
	T_GPUBOINFO	bi;
	UBINT		va = 0, va2 = 0;
	UD		i, e;
	UD		free0 = free_quiet();
	ER		er;

	bo = knl_gpu_bo_create(4 * 4096, GPU_BO_NOGPU, &er);
	KT_ASSERT(bo != NULL);
	if ( bo == NULL ) return;
	KT_ASSERT_ER(knl_space_create(&sp), E_OK);

	KT_ASSERT_ER(knl_gpu_bo_map(bo, &sp, &va), E_OK);
	KT_ASSERT(va >= SHM_WINDOW_BASE && va < SHM_WINDOW_END);
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_EQ(knl_pt_lookup(sp.l0_pa, va + i * 4096), knl_gpu_bo_page(bo, i));
	}
	e = knl_pt_entry(sp.l0_pa, va);
#if defined(__x86_64__)
	KT_ASSERT_EQ(e & ( PTE_PWT | PTE_PCD ), PTE_CACHE_WC);		/* write combined */
	KT_ASSERT_EQ(e & ( PTE_US | PTE_RW ), PTE_US | PTE_RW);		/* the process writes it */
	KT_ASSERT(( e & PTE_NX ) != 0);					/* and never runs it */
#else
	KT_ASSERT_EQ(e & PTE_ATTR(7), PTE_ATTR(MAIR_IDX_NORMAL_NC));	/* uncached */
	KT_ASSERT_EQ(e & PTE_AP_RO_ALL, PTE_AP_RW_ALL);			/* the process writes it */
	KT_ASSERT(( e & PTE_UXN ) != 0);				/* and never runs it */
#endif
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT_EQ(bi.uva, va);
	KT_ASSERT_EQ(bi.refcnt, 2);					/* the mapping holds one */

	KT_ASSERT_ER(knl_gpu_bo_map(bo, &sp, &va2), E_OBJ);		/* there already */
	KT_ASSERT_ER(knl_gpu_bo_unmap(bo, &sp), E_OK);
	KT_ASSERT_EQ(knl_pt_lookup(sp.l0_pa, va), ~(UD)0);
	KT_ASSERT_ER(knl_gpu_bo_unmap(bo, &sp), E_NOEXS);

	/* mapped again, at the same address; the space goes with it mapped */
	KT_ASSERT_ER(knl_gpu_bo_map(bo, &sp, &va2), E_OK);
	KT_ASSERT_EQ(va2, va);
	knl_space_delete(&sp);
	knl_gpu_bo_put(bo);		/* the mapping's reference, which the process exit gives back */
	for ( i = 0; i < 4; i++ ) {
		CONST UW	*w = (CONST UW *)PA2VA(knl_gpu_bo_page(bo, i));

		KT_ASSERT_EQ(w[0], 0);	/* deleting the space left the pages alone */
	}
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT_EQ(bi.refcnt, 1);
	knl_gpu_bo_put(bo);
	KT_ASSERT_EQ(free_quiet(), free0);
}

/* ---------------------------------------------------------------- a backend of the test's own */

typedef struct {
	T_V3DMMU	mmu;
	BOOL		hold;		/* leave the job running */
	INT		kicked;
	UINT		last_kind;
	UINT		resets;
	INT		boost;		/* jobs waited for before they started, not over */
	INT		boosted;	/* told of so many */
} TB;

LOCAL TB	tb;
LOCAL T_SPLOCK	tb_lock;

/* From the waiter and from the common layer's task, on any processor */
LOCAL void tb_boost( void *priv, INT delta )
{
	UINT	imask;

	ISpinLock(&tb_lock, &imask);
	tb.boost += delta;
	if ( delta > 0 ) tb.boosted += delta;
	ISpinUnlock(&tb_lock, &imask);
}

LOCAL ER tb_info( void *priv, T_GPUINFO *info )
{
	info->kind = GPU_KIND_TEST;
	info->feature = GPU_FEAT_CL | GPU_FEAT_TFU | GPU_FEAT_CACHE | GPU_FEAT_MMU;
	info->va_size = tb.mmu.va_size;
	return E_OK;
}

LOCAL ER tb_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset )
{
	UW	va;
	ER	er = v3d_mmu_alloc(&tb.mmu, npages, 1, &va);

	if ( er < E_OK ) return er;
	v3d_mmu_insert(&tb.mmu, va, knl_gpu_bo_pages(bo), npages, TRUE);
	*p_offset = va;
	return E_OK;
}

LOCAL void tb_unbind( void *priv, UW offset, UD npages )
{
	v3d_mmu_remove(&tb.mmu, offset, npages);
	v3d_mmu_free(&tb.mmu, offset, npages);
}

/* A TFU job fails; the others end at once unless the test holds them */
LOCAL ER tb_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	tb.kicked++;
	tb.last_kind = job->kind;
	if ( job->kind == GPU_JOB_TFU ) {
		knl_gpu_job_end(E_IO);
	} else if ( !tb.hold ) {
		knl_gpu_job_end(E_OK);
	}
	return E_OK;
}

LOCAL void tb_reset( void *priv )
{
	tb.resets++;
}

LOCAL CONST T_GPUOPS	tb_ops = {
	GPU_KIND_TEST,
	GPU_FEAT_CL | GPU_FEAT_TFU | GPU_FEAT_CACHE | GPU_FEAT_MMU,
	0,
	tb_info, tb_bind, tb_unbind, tb_kick, tb_reset,
	.boost = tb_boost
};

LOCAL void gpu_jobs( void )
{
	T_GPUINFO	info;
	T_GPUBO		*bo, *bo2;
	T_GPUBOINFO	bi;
	T_GPUJOB	job;
	D		f1, f2, f3, f4;
	UD		free0;
	ER		er;
	INT		nb;

	KT_ASSERT_ER(knl_gpu_init(), E_OK);
	knl_memset(&tb, 0, sizeof(tb));
	if ( v3d_mmu_init(&tb.mmu, TEST_VA) < E_OK ) {
		KT_ASSERT(FALSE);
		return;
	}
	free0 = free_quiet();
	er = knl_gpu_register(&tb_ops, &tb);
	if ( er == E_OBJ ) {
		v3d_mmu_fini(&tb.mmu);
		KT_SKIP("a GPU is registered already");
	}
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(knl_gpu_register(&tb_ops, &tb), E_OBJ);
	KT_ASSERT_ER(knl_gpu_ref(&info), E_OK);
	KT_ASSERT_EQ(info.kind, GPU_KIND_TEST);

	/* a BO the backend entered in its table */
	bo = knl_gpu_bo_create(3 * 4096, 0, &er);
	KT_ASSERT(bo != NULL);
	if ( bo == NULL ) goto out;
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT(bi.offset >= V3D_VA_RESERVED);
	KT_ASSERT_EQ(v3d_mmu_lookup(&tb.mmu, bi.offset), v3d_mmu_pte(knl_gpu_bo_page(bo, 0), TRUE));
	KT_ASSERT_EQ(v3d_mmu_lookup(&tb.mmu, bi.offset + 2 * 4096),
		     v3d_mmu_pte(knl_gpu_bo_page(bo, 2), TRUE));

	/* jobs end in order and their fences grow */
	knl_memset(&job, 0, sizeof(job));
	job.kind = GPU_JOB_NOP;
	f1 = knl_gpu_submit(&job, &bo, 1);
	job.kind = GPU_JOB_CL;
	f2 = knl_gpu_submit(&job, &bo, 1);
	KT_ASSERT(f1 > 0 && f2 == f1 + 1);
	KT_ASSERT_ER(knl_gpu_wait(f2, 500 * MS), E_OK);
	KT_ASSERT_ER(knl_gpu_wait(f1, TMO_POL), E_OK);
	KT_ASSERT(knl_gpu_done() >= f2);
	KT_ASSERT_EQ(tb.kicked, 1);				/* the NOP did not reach it */
	KT_ASSERT_EQ(tb.last_kind, GPU_JOB_CL);

	/* what the backend does not run, and fences that do not exist */
	job.kind = GPU_JOB_CSD;
	KT_ASSERT_EQ(knl_gpu_submit(&job, &bo, 1), E_NOSPT);
	job.kind = 99;
	KT_ASSERT_EQ(knl_gpu_submit(&job, NULL, 0), E_PAR);
	job.kind = GPU_JOB_CL;
	job.after = f2 + 100;
	KT_ASSERT_EQ(knl_gpu_submit(&job, NULL, 0), E_PAR);
	job.after = 0;
	KT_ASSERT_ER(knl_gpu_wait(0, TMO_POL), E_PAR);
	KT_ASSERT_ER(knl_gpu_wait(knl_gpu_done() + 1000, TMO_POL), E_PAR);

	/* a job that fails says so at its fence, and the next is fine */
	job.kind = GPU_JOB_TFU;
	f3 = knl_gpu_submit(&job, NULL, 0);
	job.kind = GPU_JOB_CACHE;
	f4 = knl_gpu_submit(&job, NULL, 0);
	KT_ASSERT_ER(knl_gpu_wait(f3, 500 * MS), E_IO);
	KT_ASSERT_ER(knl_gpu_wait(f4, 500 * MS), E_OK);
	/* jobs waited for while still queued above were boosted: all given back once over */
	KT_ASSERT_EQ(tb.boost, 0);

	/* a job still running keeps its BOs busy and alive */
	bo2 = knl_gpu_bo_create(4096, 0, &er);
	KT_ASSERT(bo2 != NULL);
	if ( bo2 == NULL ) goto out_bo;
	tb.hold = TRUE;
	job.kind = GPU_JOB_CL;
	nb = tb.boosted;
	f1 = knl_gpu_submit(&job, &bo2, 1);
	f2 = knl_gpu_submit(&job, &bo2, 1);		/* queued behind it */
	KT_ASSERT_ER(knl_gpu_wait(f1, 20 * MS), E_TMOUT);
	{
		INT	b0 = tb.boosted;		/* f1 may have been queued at first */

		KT_ASSERT_ER(knl_gpu_wait(f1, 10 * MS), E_TMOUT);
		KT_ASSERT_EQ(tb.boosted, b0);		/* a wait on the running job boosts nothing */
		knl_gpu_bo_ref(bo2, &bi);
		KT_ASSERT_EQ(bi.busy, 2);
		KT_ASSERT_ER(knl_gpu_bo_wait(bo2, TMO_POL), E_TMOUT);
		KT_ASSERT_EQ(tb.boosted, b0);		/* nor a look that does not sleep */
		KT_ASSERT_ER(knl_gpu_bo_wait(bo2, 10 * MS), E_TMOUT);
		KT_ASSERT_EQ(tb.boosted, b0 + 1);	/* the queued one, once however often waited for */
		KT_ASSERT_ER(knl_gpu_wait(f2, 10 * MS), E_TMOUT);
		KT_ASSERT_EQ(tb.boosted, b0 + 1);
		KT_ASSERT_EQ(tb.boost, 1 + ( b0 - nb ));	/* f2, and f1 if it was boosted */
	}
	KT_ASSERT_ER(knl_gpu_unregister(), E_BUSY);
	knl_gpu_bo_put(bo2);				/* the jobs still hold it */
	KT_ASSERT(v3d_mmu_lookup(&tb.mmu, bi.offset) != 0);
	tb.hold = FALSE;
	knl_gpu_job_end(E_OK);				/* f1 ends, f2 runs and ends */
	KT_ASSERT_ER(knl_gpu_wait(f2, 500 * MS), E_OK);
	KT_ASSERT_ER(knl_gpu_wait(f1, TMO_POL), E_OK);
	KT_ASSERT_EQ(tb.boost, 0);			/* given back when it was over */
	KT_ASSERT_EQ(v3d_mmu_lookup(&tb.mmu, bi.offset), 0);	/* gone with its last reference */
	KT_ASSERT_EQ(tb.resets, 0);

out_bo:
	KT_ASSERT_ER(knl_gpu_bo_wait(bo, TMO_POL), E_OK);
	knl_gpu_bo_put(bo);
out:
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
	KT_ASSERT_EQ(free_quiet(), free0);
	KT_ASSERT_EQ(tb.mmu.nfree, tb.mmu.npte - ( V3D_VA_RESERVED >> 12 ));
	v3d_mmu_fini(&tb.mmu);
}


/* ---------------------------------------------------------------- the GPU's object tells a backend that came */

/* A backend that only says what it is: the object tells of it, no job runs */
LOCAL ER tg_info( void *priv, T_GPUINFO *info )
{
	info->kind = GPU_KIND_TEST;
	info->feature = GPU_FEAT_CL;
	return E_OK;
}

LOCAL CONST T_GPUOPS	tg_ops = { GPU_KIND_TEST, GPU_FEAT_CL, 0, tg_info };

LOCAL void tg_late( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	tk_dly_tsk(300);
	(void)knl_gpu_register(&tg_ops, NULL);
	tk_ext_tsk();
}

LOCAL INT tg_read( CONST TS_UUID *u, UB *t, INT max )
{
	ID	k = ob_opn_obj(u, OB_OP_R);
	SZ	asz = 0;
	ER	er;

	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, OB_GP_STATE, 0, t, max - 1, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) return (INT)er;
	t[asz] = 0;
	return (INT)asz;
}

LOCAL BOOL tg_has( CONST UB *t, CONST char *w )
{
	INT	i, k, n = (INT)knl_strlen(w), len = (INT)knl_strlen((CONST char *)t);

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* Notices of OB_E_CHANGE on record OB_GP_STATE at the port, taken: how many */
LOCAL INT tg_notices( ID port, INT ms )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	n = 0, waited = 0;

	for (;;) {
		while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
			if ( m.event == OB_E_CHANGE && m.recno == OB_GP_STATE ) n++;
			asz = 0;
		}
		if ( n > 0 || waited >= ms ) break;
		tk_dly_tsk(20);
		waited += 20;
	}
	return n;
}

/*
 * The object is there before any backend: a program that opened it and
 * asked to be told is told when a backend registers later, finds the
 * backend by what the call that asks the GPU answers, and is told again
 * when it goes; one that opens it afterwards reads the state at once.
 */
LOCAL void gpu_obj_told( void )
{
	CONST TS_UUID	gu = OB_UUID_GPU_INIT;
	T_GPUINFO	info;
	T_OBCRE		c;
	T_OBNTF		req;
	TS_UUID		pu;
	UB		t[512];
	ID		port, kw, tid;
	T_CTSK		ct;
	ER		er;

	if ( knl_gpu_ref(&info) >= E_OK ) KT_SKIP("a GPU is registered already");
	KT_ASSERT_ER(knl_gpu_init(), E_OK);

	/* a key opened before the backend, with the request made before looking */
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	port = ob_opn_obj(&pu, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	kw = ob_opn_obj(&gu, OB_OP_ATRRD);
	KT_ASSERT(port > 0 && kw > 0);
	if ( port <= 0 || kw <= 0 ) goto out;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	KT_ASSERT(ob_ntf_evt(kw, OB_GP_STATE, &req, port) > 0);
	KT_ASSERT(tg_read(&gu, t, sizeof(t)) > 0 && tg_has(t, "BACKEND none"));
	KT_ASSERT_ER(knl_gpu_ref(&info), E_NOEXS);
	(void)tg_notices(port, 0);

	/* nothing is told while no one registers, and the task that does comes later */
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)tg_late;
	ct.itskpri = KT_PRI_HIGH;
	ct.stksz = 4096;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) goto out;
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	KT_ASSERT_EQ(tg_notices(port, 100), 0);
	KT_ASSERT_ER(knl_gpu_ref(&info), E_NOEXS);

	/* told: the backend is there, and the record says so */
	KT_ASSERT(tg_notices(port, 3000) >= 1);
	er = knl_gpu_ref(&info);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) goto out;
	KT_ASSERT_EQ(info.kind, GPU_KIND_TEST);
	KT_ASSERT(tg_read(&gu, t, sizeof(t)) > 0 && tg_has(t, "BACKEND registered") && tg_has(t, "KIND test"));

	/* opened after the registration: the state is there to read at once */
	(void)tg_notices(port, 0);
	KT_ASSERT(tg_read(&gu, t, sizeof(t)) > 0 && tg_has(t, "BACKEND registered"));
	KT_ASSERT_ER(knl_gpu_ref(&info), E_OK);

	/* a backend's own lines come with its text, and the change is told */
	knl_gpu_obj_detail((CONST UB *)"STATE running\n", 14);
	KT_ASSERT(tg_notices(port, 500) >= 1);
	KT_ASSERT(tg_read(&gu, t, sizeof(t)) > 0 && tg_has(t, "STATE running"));

	/* gone: told, and the record says none */
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
	KT_ASSERT(tg_notices(port, 500) >= 1);
	KT_ASSERT(tg_read(&gu, t, sizeof(t)) > 0 && tg_has(t, "BACKEND none"));
	knl_gpu_obj_detail((CONST UB *)"", 0);
out:
	if ( kw > 0 ) (void)ob_cls_obj(kw);
	if ( port > 0 ) (void)ob_cls_obj(port);
	(void)ob_del_obj(&pu);
}

/*
 * More failed jobs than the first room of the failed fences, each one a
 * run of its own (a job that ends well between): every one of them is
 * still said to have failed, the first as well, and the others not
 */
#define MANY_FAILED	40

LOCAL void gpu_many_failed( void )
{
	T_GPUJOB	job;
	D		bad[MANY_FAILED], good[MANY_FAILED];
	INT		i;
	ER		er;

	KT_ASSERT_ER(knl_gpu_init(), E_OK);
	knl_memset(&tb, 0, sizeof(tb));
	if ( v3d_mmu_init(&tb.mmu, TEST_VA) < E_OK ) {
		KT_ASSERT(FALSE);
		return;
	}
	er = knl_gpu_register(&tb_ops, &tb);
	if ( er == E_OBJ ) {
		v3d_mmu_fini(&tb.mmu);
		KT_SKIP("a GPU is registered already");
	}
	KT_ASSERT_ER(er, E_OK);
	knl_memset(&job, 0, sizeof(job));
	for ( i = 0; i < MANY_FAILED; i++ ) {
		job.kind = GPU_JOB_TFU;
		bad[i] = knl_gpu_submit(&job, NULL, 0);
		job.kind = GPU_JOB_CACHE;
		good[i] = knl_gpu_submit(&job, NULL, 0);
		KT_ASSERT(bad[i] > 0 && good[i] == bad[i] + 1);
	}
	KT_ASSERT_ER(knl_gpu_wait(good[MANY_FAILED - 1], 2000 * MS), E_OK);
	for ( i = 0; i < MANY_FAILED; i++ ) {
		KT_ASSERT_ER(knl_gpu_wait(bad[i], TMO_POL), E_IO);
		KT_ASSERT_ER(knl_gpu_wait(good[i], TMO_POL), E_OK);
	}
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
	v3d_mmu_fini(&tb.mmu);
}

/* A backend's lines longer than the record's old room (768 bytes) come whole */
LOCAL void gpu_obj_long_detail( void )
{
	CONST TS_UUID	gu = OB_UUID_GPU_INIT;
	UB		*d, *t;
	INT		i, n, len = 3000;
	CONST INT	max = 8192;

	d = (UB *)Kmalloc((SZ)len + 1);
	t = (UB *)Kmalloc((SZ)max);
	KT_ASSERT(d != NULL && t != NULL);
	if ( d == NULL || t == NULL ) goto out;
	/* lines of x, the last one "LAST end" */
	for ( i = 0; i < len - 9; i++ ) d[i] = ( i % 60 == 59 ) ? '\n' : 'x';
	knl_memcpy(d + len - 9, "LAST end\n", 9);
	knl_gpu_obj_detail(d, len);
	n = tg_read(&gu, t, max);
	KT_ASSERT(n > len);
	KT_ASSERT(n > 0 && n < max && tg_has(t, "LAST end"));
	knl_gpu_obj_detail((CONST UB *)"", 0);
out:
	if ( d != NULL ) Kfree(d);
	if ( t != NULL ) Kfree(t);
}

EXPORT void ktest_gpu( void )
{
	KT_RUN(gpu_mmu_table);
	KT_RUN(gpu_bo_pages);
	KT_RUN(gpu_bo_map);
	KT_RUN(gpu_jobs);
	KT_RUN(gpu_many_failed);
	KT_RUN(gpu_obj_told);
	KT_RUN(gpu_obj_long_detail);
}
