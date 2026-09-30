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
#include "../../device/gpu/gpudev.h"
#include "../../device/gpu/v3d/v3d.h"
#include "../../device/gpu/v3d/v3d_regs.h"

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

	/* what is given back is found again first */
	v3d_mmu_free(&m, va, 4);
	KT_ASSERT_ER(v3d_mmu_alloc(&m, 4, 1, &va3), E_OK);
	KT_ASSERT_EQ(va3, va);
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
	UD		free0 = knl_pf_free_count(-1);
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
	KT_ASSERT(knl_pf_free_count(-1) + 3 <= free0);

	knl_gpu_bo_get(bo);
	knl_gpu_bo_ref(bo, &bi);
	KT_ASSERT_EQ(bi.refcnt, 2);
	knl_gpu_bo_put(bo);
	knl_gpu_bo_put(bo);
	KT_ASSERT_EQ(knl_pf_free_count(-1), free0);
}

LOCAL void gpu_bo_map( void )
{
	T_SPACE		sp;
	T_GPUBO		*bo;
	T_GPUBOINFO	bi;
	UBINT		va = 0, va2 = 0;
	UD		i, e;
	UD		free0 = knl_pf_free_count(-1);
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
	KT_ASSERT_EQ(e & PTE_ATTR(7), PTE_ATTR(MAIR_IDX_NORMAL_NC));	/* uncached */
	KT_ASSERT_EQ(e & PTE_AP_RO_ALL, PTE_AP_RW_ALL);			/* the process writes it */
	KT_ASSERT(( e & PTE_UXN ) != 0);				/* and never runs it */
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
	KT_ASSERT_EQ(knl_pf_free_count(-1), free0);
}

/* ---------------------------------------------------------------- a backend of the test's own */

typedef struct {
	T_V3DMMU	mmu;
	BOOL		hold;		/* leave the job running */
	INT		kicked;
	UINT		last_kind;
	UINT		resets;
} TB;

LOCAL TB	tb;

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
LOCAL ER tb_kick( void *priv, CONST T_GPUJOB *job )
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
	tb_info, tb_bind, tb_unbind, tb_kick, tb_reset
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

	KT_ASSERT_ER(knl_gpu_init(), E_OK);
	knl_memset(&tb, 0, sizeof(tb));
	if ( v3d_mmu_init(&tb.mmu, TEST_VA) < E_OK ) {
		KT_ASSERT(FALSE);
		return;
	}
	free0 = knl_pf_free_count(-1);
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

	/* a job still running keeps its BOs busy and alive */
	bo2 = knl_gpu_bo_create(4096, 0, &er);
	KT_ASSERT(bo2 != NULL);
	if ( bo2 == NULL ) goto out_bo;
	tb.hold = TRUE;
	job.kind = GPU_JOB_CL;
	f1 = knl_gpu_submit(&job, &bo2, 1);
	f2 = knl_gpu_submit(&job, &bo2, 1);		/* queued behind it */
	KT_ASSERT_ER(knl_gpu_wait(f1, 20 * MS), E_TMOUT);
	knl_gpu_bo_ref(bo2, &bi);
	KT_ASSERT_EQ(bi.busy, 2);
	KT_ASSERT_ER(knl_gpu_bo_wait(bo2, 10 * MS), E_TMOUT);
	KT_ASSERT_ER(knl_gpu_unregister(), E_BUSY);
	knl_gpu_bo_put(bo2);				/* the jobs still hold it */
	KT_ASSERT(v3d_mmu_lookup(&tb.mmu, bi.offset) != 0);
	tb.hold = FALSE;
	knl_gpu_job_end(E_OK);				/* f1 ends, f2 runs and ends */
	KT_ASSERT_ER(knl_gpu_wait(f2, 500 * MS), E_OK);
	KT_ASSERT_ER(knl_gpu_wait(f1, TMO_POL), E_OK);
	KT_ASSERT_EQ(v3d_mmu_lookup(&tb.mmu, bi.offset), 0);	/* gone with its last reference */
	KT_ASSERT_EQ(tb.resets, 0);

out_bo:
	KT_ASSERT_ER(knl_gpu_bo_wait(bo, TMO_POL), E_OK);
	knl_gpu_bo_put(bo);
out:
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
	KT_ASSERT_EQ(knl_pf_free_count(-1), free0);
	KT_ASSERT_EQ(tb.mmu.nfree, tb.mmu.npte - ( V3D_VA_RESERVED >> 12 ));
	v3d_mmu_fini(&tb.mmu);
}

EXPORT void ktest_gpu( void )
{
	KT_RUN(gpu_mmu_table);
	KT_RUN(gpu_bo_pages);
	KT_RUN(gpu_bo_map);
	KT_RUN(gpu_jobs);
}
