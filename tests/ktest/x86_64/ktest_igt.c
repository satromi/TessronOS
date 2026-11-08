/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_igt.c (x64 PC)
 *	What of the driver of Intel's render and copy engines
 *	(device/gpu/intel) and of the shared global GTT
 *	(kernel/sysdepend/x64/igt_ggtt.c) does not need the device, on a
 *	machine without it (docs/tessronos/13-gpu.md 13.11.8):
 *
 *	  igt_ggtt	ranges of the global GTT over a table in memory: what
 *			the firmware left, taken as it is; mappable and not;
 *			claimed places; names; the entries written and cleared
 *	  igt_ggtt_scattered  a table whose memory held stray entries
 *			(thousands of short runs): kept as a few ranges, the
 *			GOP's screen taken over, a mappable surface placed
 *			over the rest, the render engine's ranges above
 *	  igt_ppgtt	page tables of four levels: entries, the scratch
 *			table where nothing is, canonical addresses, tables
 *			made and freed with the space
 *	  igt_kva	the kernel's 32 bit addresses of the GPU
 *	  igt_layout	the register state of a new context: its commands
 *			against the fixed places of the values
 *	  igt_emit	the commands of a job, decoded: their lengths, the
 *			batch, the sequence number, the ring going round
 *	  igt_plan	compositing on the copy engine planned as copies and
 *			done by the processor, against the processor's stages
 *			over random scenes, to the bit
 *	  igt_place	a process places its BOs where it chooses (gp_bnd_bo)
 *			on a GPU of the test's whose spaces take them so: what
 *			reaches the space, moved, deleted, at the process's end
 *	  igt_irispath	a process's OpenGL ES (make GL=1): the platform
 *			takes Mesa's iris for the DRM shim's pretended Kaby
 *			Lake, makes its screen and goes back to softpipe when
 *			the frame of its check is not drawn
 *	  igt_softpipe	and softpipe draws a triangle, as before
 *	  igt_i915sync	the DRM shim's waits on synchronisation objects
 *			(WAIT_FOR_SUBMIT, WAIT_ALL, the first signalled) and
 *			its reading of the time stamp (REG_READ)
 *	  igt_rcmd	a rectangle of the render engine's compositing as
 *			commands: each command's header and the fields filled
 *			in, against the PRM's layout (not the generated one)
 *	  igt_rplan	the render engine's plan of a few scenes: the ground
 *			first, every rectangle in its part or its level, the
 *			rectangles of a group apart, a barrier at each group
 *	  igt_pm	the GT's power without the GPU: a park when idle,
 *			forcewake let go of and asked for again, the steps of
 *			the frequency on the GT's word, against registers of
 *			the test's
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include <ts/fs.h>
#include <ts/proc.h>
#include "../ktest.h"
#include "../../../device/gpu/intel/igt.h"
#include "../../../device/gpu/intel/igt_cp.h"
#include "../../../device/gpu/intel/igt_rplan.h"
#include "../../../device/gpu/intel/igt12.h"

IMPORT void	knl_igt_ggtt_fake( UBINT mmio, UD ggtt_size, UD mappable );
IMPORT void	knl_igt_ggtt_unfake( void );
IMPORT UD	knl_igt_ggtt_scratch( void );
IMPORT INT	knl_igt_ggtt_count( UINT owner );
IMPORT void	igt_test_layout( INT eng, UW *page );
IMPORT UW	igt_test_emit( INT eng, BOOL first, UD start, UB *ring, UW tail, UW hwsp_ggtt, UW *p_seq );
IMPORT void	knl_wm_hold( BOOL hold );
IMPORT UW	*igt_rc_rect( UW *b, CONST IGT_RD *d, UW curbe );

#define MB	( 1024UL * 1024 )

/* ---------------------------------------------------------------- the global GTT */

LOCAL void test_igt_ggtt( void )
{
	UD	npages = 16 * MB / PAGE_SIZE, i, off, size, off2, pa[3];
	UB	*mm = (UB *)knl_vmap(npages, 0);
	volatile UD *pte;
	ER	er;

	KT_ASSERT(mm != NULL);
	if ( mm == NULL ) return;
	knl_memset(mm, 0, 16 * MB);
	pte = (volatile UD *)( mm + 8 * MB );
	/* what the firmware left: its screen of 1875 pages from 0, 4 pages at 16MB */
	for ( i = 0; i < 1875; i++ ) pte[i] = 0x5d800000ULL + i * PAGE_SIZE + 1;
	for ( i = 0; i < 4; i++ ) pte[4096 + i] = 0x5f000000ULL + i * PAGE_SIZE + 1;
	knl_igt_ggtt_fake((UBINT)mm, 4096 * MB, 256 * MB);

	er = knl_igt_ggtt_find("firmware", &off, &size);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 0);
	KT_ASSERT_EQ(size, 1875 * PAGE_SIZE);

	/* mappable, 16MB on 1MB: past both of the firmware's */
	er = knl_igt_ggtt_alloc(16 * MB, MB, IGT_GGTT_MAPPABLE, IGT_GGTT_DISPLAY, "surface", &off);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 17 * MB);
	/* not mappable: above the mappable part */
	er = knl_igt_ggtt_alloc(64 * 1024, PAGE_SIZE, 0, IGT_GGTT_RENDER, "ring", &off2);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off2, 256 * MB);
	er = knl_igt_ggtt_find("ring", &off, &size);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 256 * MB);
	KT_ASSERT_EQ(size, 64 * 1024);
	KT_ASSERT_ER(knl_igt_ggtt_find("nothing", NULL, NULL), E_NOEXS);
	/* bad sizes and alignments */
	KT_ASSERT_ER(knl_igt_ggtt_alloc(100, PAGE_SIZE, 0, IGT_GGTT_RENDER, NULL, &off), E_PAR);
	KT_ASSERT_ER(knl_igt_ggtt_alloc(PAGE_SIZE, 3 * PAGE_SIZE, 0, IGT_GGTT_RENDER, NULL, &off), E_PAR);
	/* larger than the mappable part */
	KT_ASSERT_ER(knl_igt_ggtt_alloc(512 * MB, PAGE_SIZE, IGT_GGTT_MAPPABLE, IGT_GGTT_DISPLAY, NULL, &off),
		     E_NOMEM);

	/* the display takes the GOP's screen over: the first of the firmware's goes, the other stays */
	er = knl_igt_ggtt_claim(0, 16 * MB, IGT_GGTT_DISPLAY, "gop screen");
	KT_ASSERT_ER(er, E_OK);
	er = knl_igt_ggtt_find("firmware", &off, &size);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 16 * MB);
	KT_ASSERT_ER(knl_igt_ggtt_claim(255 * MB, 2 * MB, IGT_GGTT_DISPLAY, NULL), E_OBJ);	/* the ring's */
	/* a claim in the middle of the firmware's splits it */
	er = knl_igt_ggtt_claim(16 * MB + PAGE_SIZE, PAGE_SIZE, IGT_GGTT_DISPLAY, "mid");
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(knl_igt_ggtt_find("firmware", &off, &size), E_OK);
	KT_ASSERT_EQ(size, PAGE_SIZE);

	/* the entries: written as addresses with bit 0, the cache of the table flushed; cleared to the scratch */
	pa[0] = 0x123456000ULL;
	pa[1] = 0x7ff000ULL;
	pa[2] = 0x1000ULL;
	*(volatile UW *)( mm + 0x101008 ) = 0;
	knl_igt_ggtt_set(off2, pa, 3, FALSE);
	KT_ASSERT_EQ(pte[off2 / PAGE_SIZE], 0x123456001ULL);
	KT_ASSERT_EQ(pte[off2 / PAGE_SIZE + 1], 0x7ff001ULL);
	KT_ASSERT_EQ(pte[off2 / PAGE_SIZE + 2], 0x1001ULL);
	KT_ASSERT_EQ(*(volatile UW *)( mm + 0x101008 ), 1);
	knl_igt_ggtt_set(off2, pa, 2, TRUE);
	KT_ASSERT_EQ(pte[off2 / PAGE_SIZE + 1], 0x123457001ULL);
	KT_ASSERT_EQ(knl_igt_ggtt_pte(off2 + PAGE_SIZE), 0x123457001ULL);
	knl_igt_ggtt_clear(off2, 3);
	KT_ASSERT_EQ(pte[off2 / PAGE_SIZE + 2], knl_igt_ggtt_scratch() | 1);
	/* past the end: nothing written */
	knl_igt_ggtt_set(4096 * MB - PAGE_SIZE, pa, 2, FALSE);

	KT_ASSERT_ER(knl_igt_ggtt_free(off2), E_OK);
	KT_ASSERT_ER(knl_igt_ggtt_free(off2), E_NOEXS);
	KT_ASSERT_ER(knl_igt_ggtt_free(16 * MB), E_NOEXS);		/* the firmware's are not given back */
	/* given back, taken again at the same place */
	er = knl_igt_ggtt_alloc(64 * 1024, PAGE_SIZE, 0, IGT_GGTT_RENDER, "again", &off);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 256 * MB);
	/* the table of ranges fills up */
	for ( i = 0; i < 200; i++ ) {
		er = knl_igt_ggtt_alloc(PAGE_SIZE, PAGE_SIZE, 0, IGT_GGTT_RENDER, NULL, &off);
		if ( er < E_OK ) break;
	}
	KT_ASSERT_ER(er, E_LIMIT);
	tm_printf((UB *)"KTEST igt_ggtt: %ld ranges taken before the table was full\n", i);

	knl_igt_ggtt_unfake();
	knl_vunmap(mm, npages);
}

/*
 * The mappable part as a machine left it whose table's memory was not
 * cleared: the GOP's screen from 0, then a stray entry every few pages
 * up to its end
 */
LOCAL void test_igt_ggtt_scattered( void )
{
	UD	npages = 16 * MB / PAGE_SIZE, i, off, off2, size;
	UB	*mm = (UB *)knl_vmap(npages, 0);
	volatile UD *pte;
	ER	er;

	KT_ASSERT(mm != NULL);
	if ( mm == NULL ) return;
	knl_memset(mm, 0, 16 * MB);
	pte = (volatile UD *)( mm + 8 * MB );
	for ( i = 0; i < 1875; i++ ) pte[i] = 0x5d800000ULL + i * PAGE_SIZE + 1;
	for ( i = 2048; i < 65536; i += 3 ) pte[i] = 0x123456000ULL + i * PAGE_SIZE + 1;
	knl_igt_ggtt_fake((UBINT)mm, 4096 * MB, 256 * MB);

	/* far more runs than the array holds, kept as a few ranges */
	KT_ASSERT(knl_igt_ggtt_count(IGT_GGTT_FIRMWARE) >= 1);
	KT_ASSERT(knl_igt_ggtt_count(IGT_GGTT_FIRMWARE) <= 16);
	er = knl_igt_ggtt_find("firmware", &off, &size);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off, 0);

	/* the display takes the GOP's screen over, and a second place where no free room is left */
	KT_ASSERT_ER(knl_igt_ggtt_claim(0, 16 * MB, IGT_GGTT_DISPLAY, "display 0"), E_OK);
	er = knl_igt_ggtt_alloc(16 * MB, 256 * 1024, IGT_GGTT_MAPPABLE, IGT_GGTT_DISPLAY, "display 1", &off);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT(off >= 16 * MB);
	KT_ASSERT(off + 16 * MB <= 256 * MB);
	KT_ASSERT_EQ(off & ( 256 * 1024 - 1 ), 0);
	/* the two places do not meet, nor does anything else come into them */
	KT_ASSERT_ER(knl_igt_ggtt_claim(off, PAGE_SIZE, IGT_GGTT_RENDER, NULL), E_OBJ);
	KT_ASSERT_ER(knl_igt_ggtt_claim(0, PAGE_SIZE, IGT_GGTT_RENDER, NULL), E_OBJ);

	/* the render engine's ranges above the mappable part, as many as before */
	er = knl_igt_ggtt_alloc(64 * 1024, PAGE_SIZE, 0, IGT_GGTT_RENDER, "ring", &off2);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(off2, 256 * MB);
	for ( i = 0; i < 64; i++ ) {
		er = knl_igt_ggtt_alloc(PAGE_SIZE, PAGE_SIZE, 0, IGT_GGTT_RENDER, NULL, &off2);
		if ( er < E_OK ) break;
	}
	KT_ASSERT_ER(er, E_OK);
	tm_printf((UB *)"KTEST igt_ggtt_scattered: %d firmware ranges kept, the second place at %lx\n",
		  knl_igt_ggtt_count(IGT_GGTT_FIRMWARE), off);

	knl_igt_ggtt_unfake();
	knl_vunmap(mm, npages);
}

/* ---------------------------------------------------------------- page tables */

LOCAL BOOL vm_ready( void )
{
	if ( knl_igt.scr_pt != 0 ) return TRUE;
	return igt_vm_init() >= E_OK;
}

LOCAL void test_igt_ppgtt( void )
{
	IGTVM	*vm;
	PFRAME	*pf[3];
	UD	pa[3], free0, scr, e;
	INT	i;

	KT_ASSERT(vm_ready());
	if ( knl_igt.scr_pt == 0 ) return;
	scr = ( knl_igt.scratch_pa & GPTE_ADDR ) | GPTE_PRESENT | GPTE_RW | GPTE_CACHED;
	for ( i = 0; i < 3; i++ ) {
		pf[i] = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);
		if ( pf[i] == NULL ) pf[i] = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
		KT_ASSERT(pf[i] != NULL);
		if ( pf[i] == NULL ) return;
		pa[i] = knl_pf_to_pa(pf[i]);
	}
	free0 = knl_pf_free_count(-1);
	vm = igt_vm_create(FALSE);
	KT_ASSERT(vm != NULL);
	if ( vm == NULL ) return;
	KT_ASSERT(knl_pf_free_count(-1) < free0);
	KT_ASSERT_EQ(vm->npt, 0);

	/* nothing there: the scratch page, at any level */
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x1000), scr);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x7ffffffff000ULL), scr);

	KT_ASSERT_ER(igt_vm_map(vm, 0x1000, &pa[0], 1), E_OK);
	KT_ASSERT_EQ(vm->npt, 3);				/* a table at each level below the top */
	KT_ASSERT_ER(igt_vm_map(vm, 0x7ffffffff000ULL, &pa[1], 1), E_OK);
	KT_ASSERT_EQ(vm->npt, 6);
	KT_ASSERT_ER(igt_vm_map(vm, 0x123456789000ULL, &pa[2], 1), E_OK);
	KT_ASSERT_EQ(vm->npt, 9);
	KT_ASSERT_ER(igt_vm_map(vm, 0x2000, &pa[1], 1), E_OK);	/* the same tables as at 0x1000 */
	KT_ASSERT_EQ(vm->npt, 9);
	e = igt_vm_lookup(vm, 0x1000);
	KT_ASSERT_EQ(e, pa[0] | GPTE_PRESENT | GPTE_RW | GPTE_CACHED);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x7ffffffff000ULL), pa[1] | GPTE_PRESENT | GPTE_RW | GPTE_CACHED);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x123456789000ULL), pa[2] | GPTE_PRESENT | GPTE_RW | GPTE_CACHED);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x3000), scr);		/* beside, in a table made: still the scratch */
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x123456788000ULL), scr);

	/* not on a page, past 48 bits */
	KT_ASSERT_ER(igt_vm_map(vm, 0x1800, &pa[0], 1), E_PAR);
	KT_ASSERT_ER(igt_vm_map(vm, 0x1000000000000ULL, &pa[0], 1), E_PAR);
	KT_ASSERT_ER(igt_vm_map(vm, 0xfffffffff000ULL, pa, 2), E_PAR);

	/* the canonical form of a high address: its 48 bits */
	{
		T_GPUBO	*bo = knl_gpu_bo_create(PAGE_SIZE, GPU_BO_NOGPU, NULL);

		KT_ASSERT(bo != NULL);
		if ( bo != NULL ) {
			KT_ASSERT_ER(igt_vm_map(vm, 0x0000900000000000ULL, knl_gpu_bo_pages(bo), 1), E_OK);
			KT_ASSERT_EQ(igt_vm_lookup(vm, 0x0000900000000000ULL) & GPTE_ADDR, knl_gpu_bo_page(bo, 0));
			igt_vm_unmap(vm, 0x0000900000000000ULL, 1);
			knl_gpu_bo_put(bo);
		}
	}

	igt_vm_unmap(vm, 0x1000, 2);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x1000), scr);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x2000), scr);
	KT_ASSERT_EQ(igt_vm_lookup(vm, 0x7ffffffff000ULL), pa[1] | GPTE_PRESENT | GPTE_RW | GPTE_CACHED);

	free0 = knl_pf_free_count(-1);
	e = vm->npt;
	igt_vm_destroy(vm);
	KT_ASSERT(knl_pf_free_count(-1) >= free0 + e + 1);	/* every table given back */
	for ( i = 0; i < 3; i++ ) knl_free_pages(pf[i], 0);
}

/*
 * The space of an ended process whose context an engine still holds:
 * kept until the engine has gone on to another context and that job is
 * over; one no engine holds, freed at once
 */
LOCAL void test_igt_held( void )
{
	IGTVM	*vm, *other;
	IGTCTX	c, d;
	IGTENG	*e = &knl_igt.eng[IGT_RCS];
	UD	held0 = knl_igt.n_held, reaped0 = knl_igt.n_reaped, free0;

	KT_ASSERT(vm_ready());
	if ( knl_igt.scr_pt == 0 ) return;
	vm = igt_vm_create(FALSE);
	other = igt_vm_create(FALSE);
	KT_ASSERT(vm != NULL && other != NULL);
	if ( vm == NULL || other == NULL ) return;
	knl_memset(&c, 0, sizeof(c));
	knl_memset(&d, 0, sizeof(d));
	c.vm = vm;
	d.vm = other;

	/* the engine ran one of its contexts last: kept, not freed */
	e->last = &c;
	e->prev = NULL;
	e->cur = NULL;
	free0 = knl_pf_free_count(-1);
	igt_vm_destroy(vm);
	KT_ASSERT(knl_igt.dead == vm);
	KT_ASSERT_EQ(knl_igt.n_held, held0 + 1);
	KT_ASSERT_EQ(knl_pf_free_count(-1), free0);
	igt_vm_reap();
	KT_ASSERT(knl_igt.dead == vm);

	/* a job of another context started: the engine may still be writing it back */
	e->prev = &c;
	e->last = &d;
	e->cur = &d;
	igt_vm_reap();
	KT_ASSERT(knl_igt.dead == vm);

	/* that job over: freed */
	e->cur = NULL;
	igt_vm_reap();
	KT_ASSERT(knl_igt.dead == NULL);
	KT_ASSERT_EQ(knl_igt.n_reaped, reaped0 + 1);
	KT_ASSERT(knl_pf_free_count(-1) > free0);

	/* a space no engine holds: freed at once */
	e->last = NULL;
	e->prev = NULL;
	igt_vm_destroy(other);
	KT_ASSERT(knl_igt.dead == NULL);
	KT_ASSERT_EQ(knl_igt.n_held, held0 + 1);
}

LOCAL void test_igt_kva( void )
{
	UW	a, b, c;
	UD	before;

	KT_ASSERT(vm_ready());
	if ( knl_igt.scr_pt == 0 ) return;
	before = igt_kva_free_pages();
	KT_ASSERT_ER(igt_kva_alloc(4, &a), E_OK);
	KT_ASSERT(a >= IGT_KVA_LOW && ( a & 0xfff ) == 0);
	KT_ASSERT_ER(igt_kva_alloc(1, &b), E_OK);
	KT_ASSERT_EQ(b, a + 4 * PAGE_SIZE);
	igt_kva_free(a, 4);
	KT_ASSERT_ER(igt_kva_alloc(2, &c), E_OK);
	KT_ASSERT(c > b);				/* not handed out again straight away */
	KT_ASSERT_EQ(igt_kva_free_pages(), before - 3);
	KT_ASSERT_ER(igt_kva_alloc(0, &a), E_PAR);
	KT_ASSERT_ER(igt_kva_alloc(( IGT_KVA_TOP >> 12 ) + 1, &a), E_PAR);
	igt_kva_free(b, 1);
	igt_kva_free(c, 2);
	KT_ASSERT_EQ(igt_kva_free_pages(), before);
}

/* ---------------------------------------------------------------- a context's image */

LOCAL void test_igt_layout( void )
{
	UW	*pg = (UW *)Kmalloc(PAGE_SIZE);
	INT	eng;

	KT_ASSERT(pg != NULL);
	if ( pg == NULL ) return;
	for ( eng = 0; eng < IGT_NENG; eng++ ) {
		UW	b = ( eng == IGT_RCS ) ? RCS_BASE : BCS_BASE;

		igt_test_layout(eng, pg);
		KT_ASSERT_EQ(pg[0], MI_NOOP);
		KT_ASSERT_EQ(pg[1], MI_LRI(14) | MI_LRI_FORCE_POSTED);
		KT_ASSERT_EQ(pg[CTX_CONTEXT_CONTROL - 1], b + RING_CONTEXT_CONTROL);
		KT_ASSERT_EQ(pg[CTX_RING_HEAD - 1], b + RING_HEAD);
		KT_ASSERT_EQ(pg[CTX_RING_TAIL - 1], b + RING_TAIL);
		KT_ASSERT_EQ(pg[CTX_RING_START - 1], b + RING_START);
		KT_ASSERT_EQ(pg[CTX_RING_CTL - 1], b + RING_CTL);
		KT_ASSERT_EQ(pg[0x0c], b + RING_BBADDR_UDW);
		KT_ASSERT_EQ(pg[0x0e], b + RING_BBADDR);
		KT_ASSERT_EQ(pg[CTX_BB_STATE - 1], b + 0x110);
		KT_ASSERT_EQ(pg[0x1e], 0);				/* the no-ops between */
		KT_ASSERT_EQ(pg[0x21], MI_LRI(9) | MI_LRI_FORCE_POSTED);
		KT_ASSERT_EQ(pg[CTX_TIMESTAMP - 1], b + 0x3a8);
		KT_ASSERT_EQ(pg[CTX_PDP0_UDW - 1], b + 0x274);
		KT_ASSERT_EQ(pg[CTX_PDP0_LDW - 1], b + 0x270);
		KT_ASSERT_EQ(pg[0x41], MI_LRI(1) | ( ( eng == IGT_RCS ) ? 0 : MI_LRI_FORCE_POSTED ));
		KT_ASSERT_EQ(pg[0x42], b + ( ( eng == IGT_RCS ) ? 0x0c8 : 0x200 ));
		KT_ASSERT_EQ(pg[0x51], MI_LRI(44) | MI_LRI_FORCE_POSTED);
		KT_ASSERT_EQ(pg[CTX_RING_MI_MODE], b + RING_MI_MODE);
		KT_ASSERT_EQ(pg[0x52 + 2 * 11], b + 0x600);
		KT_ASSERT_EQ(pg[0x52 + 2 * 43], b + 0x068);
		KT_ASSERT_EQ(pg[0x52 + 2 * 44], 0);			/* nothing after */
	}
	Kfree(pg);
}

/* ---------------------------------------------------------------- the commands of a job */

/* Dwords of the command at 'dw' (its first) */
LOCAL INT cmd_len( UW dw )
{
	UW	type = dw >> 29;

	if ( type == 0 ) {
		switch ( ( dw >> 23 ) & 0x3f ) {
		case 0x00: case 0x02: case 0x05: case 0x08: case 0x0a:
			return 1;
		default:
			return (INT)( dw & 0xff ) + 2;
		}
	}
	return (INT)( dw & 0xff ) + 2;
}

LOCAL void test_igt_emit( void )
{
	UB	*ring = (UB *)Kmalloc(IGT_RING_BYTES);
	UW	cmd[256];
	INT	eng, first;

	KT_ASSERT(ring != NULL);
	if ( ring == NULL ) return;
	for ( eng = 0; eng < IGT_NENG; eng++ ) {
		for ( first = 0; first < 2; first++ ) {
			UW	from = IGT_RING_BYTES - 40, tail, seq, n, i, k;
			UD	start = ( first == 0 ) ? 0x0000123456789000ULL : 0;
			INT	bbs = 0, seqw = 0, lri_regs = 0, last = -1;

			knl_memset(ring, 0xee, IGT_RING_BYTES);
			tail = igt_test_emit(eng, (BOOL)first, start, ring, from, 0x10000000, &seq);
			KT_ASSERT_EQ(seq, 0xffffffff);
			KT_ASSERT_EQ(tail & 7, 0);
			KT_ASSERT(tail < from);				/* it went round */
			n = ( ( tail + IGT_RING_BYTES - from ) & ( IGT_RING_BYTES - 1 ) ) / 4;
			KT_ASSERT(n > 8 && n < 256);
			for ( i = 0; i < n && i < 256; i++ ) {
				cmd[i] = *(UW *)( ring + ( ( from + i * 4 ) & ( IGT_RING_BYTES - 1 ) ) );
			}
			for ( i = 0; i < n; i += (UW)cmd_len(cmd[i]) ) {
				UW	c = cmd[i];

				last = (INT)i;
				if ( ( c >> 23 ) == 0x31 ) {
					bbs++;
					KT_ASSERT_EQ(c, MI_BATCH_BUFFER_START | MI_BATCH_PPGTT);
					KT_ASSERT_EQ(cmd[i + 1], (UW)start);
					KT_ASSERT_EQ(cmd[i + 2], (UW)( start >> 32 ));
				}
				if ( ( c >> 23 ) == 0x22 ) {
					lri_regs += (INT)( ( c & 0xff ) + 1 ) / 2;
					for ( k = 0; k < ( ( c & 0xff ) + 1 ) / 2; k++ ) {
						UW	reg = cmd[i + 1 + 2 * k];

						KT_ASSERT(reg > 0x2000 && reg < 0x10000);
					}
				}
				if ( c == PIPE_CONTROL && ( cmd[i + 1] & PC_GLOBAL_GTT ) ) {
					KT_ASSERT_EQ(cmd[i + 1] & ( PC_QW_WRITE | PC_CS_STALL ), PC_QW_WRITE | PC_CS_STALL);
					KT_ASSERT_EQ(cmd[i + 2], 0x10000000 + HWS_SEQNO * 4);
					KT_ASSERT_EQ(cmd[i + 4], seq);
					seqw++;
				}
				if ( ( c & ~0x3fffffU ) == ( MI_FLUSH_DW & ~0x3fffffU ) && ( cmd[i + 1] & FLUSH_DW_USE_GTT ) ) {
					KT_ASSERT_EQ(c & 0xff, 2);
					KT_ASSERT_EQ(cmd[i + 1], ( 0x10000000 + HWS_SEQNO * 4 ) | FLUSH_DW_USE_GTT);
					KT_ASSERT_EQ(cmd[i + 3], seq);
					seqw++;
				}
			}
			KT_ASSERT_EQ(i, n);					/* the lengths add up to the tail */
			KT_ASSERT_EQ(bbs, ( start != 0 ) ? 1 : 0);
			KT_ASSERT_EQ(seqw, 1);
			if ( eng == IGT_RCS ) {
				KT_ASSERT_EQ(lri_regs, 3 + ( first ? 10 : 0 ));	/* the context's and the workarounds' */
			} else {
				KT_ASSERT_EQ(lri_regs, 0);
			}
			/* it ends: the interrupt, arbitration on, and a no-op to an even dword when needed */
			KT_ASSERT(last >= 0 && ( cmd[last] == MI_NOOP || cmd[last] == ( MI_ARB_ON_OFF | MI_ARB_ENABLE ) ));
		}
	}
	Kfree(ring);
}

/* ---------------------------------------------------------------- compositing planned */

#define PW	640
#define PH	480
#define PNB	3

LOCAL UW	pseed;

LOCAL UW prnd( void )
{
	pseed = pseed * 1103515245U + 12345U;
	return ( pseed >> 8 ) ^ ( pseed << 21 );
}

LOCAL INT prange( INT lo, INT hi )
{
	return lo + (INT)( prnd() % (UW)( hi - lo + 1 ) );
}

LOCAL void test_igt_plan( void )
{
	UD	pages = ( (UD)PW * PH * 4 + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	UB	*bc = (UB *)knl_vmap(pages, 0), *bg = (UB *)knl_vmap(pages, 0);
	UW	*ground = (UW *)knl_vmap(pages, 0);
	UB	*pix[PNB];
	CP_OP	*ops = (CP_OP *)Kmalloc(sizeof(CP_OP) * IGT_CP_MAXOPS);
	T_CPWIN	w[12];
	INT	s, i, k, bad = 0, laid_total = 0, cut = 0;

	for ( i = 0; i < PNB; i++ ) pix[i] = (UB *)knl_vmap(pages, 0);
	KT_ASSERT(bc != NULL && bg != NULL && ground != NULL && pix[PNB - 1] != NULL && ops != NULL);
	if ( bc == NULL || bg == NULL || ground == NULL || pix[PNB - 1] == NULL || ops == NULL ) return;
	pseed = 0x2468ace;
	for ( i = 0; i < PW * PH; i++ ) ground[i] = prnd();
	for ( k = 0; k < PNB; k++ ) {
		for ( i = 0; i < PW * PH; i++ ) ((UW *)pix[k])[i] = prnd();
	}

	for ( s = 0; s < 300; s++ ) {
		T_CPDST	dc, dg;
		INT	n = prange(0, 12), nops, laid, want;
		UD	j;

		dc.back = bc;
		dc.pitch = PW * 4;
		dc.rows = PH;
		if ( prnd() & 1 ) {
			dc.box.left = 0;
			dc.box.top = 0;
			dc.box.right = PW;
			dc.box.bottom = PH;
		} else {
			dc.box.left = prange(0, PW - 2);
			dc.box.top = prange(0, PH - 2);
			dc.box.right = prange(dc.box.left + 1, PW);
			dc.box.bottom = prange(dc.box.top + 1, PH);
		}
		dg = dc;
		dg.back = bg;
		for ( i = 0; i < n; i++ ) {
			INT	l = prange(-80, PW - 20), t = prange(-60, PH - 20), ww = prange(40, 400), hh = prange(40, 300);

			knl_memset(&w[i], 0, sizeof(w[i]));
			w[i].outer.left = l;
			w[i].outer.top = t;
			w[i].outer.right = l + ww;
			w[i].outer.bottom = t + hh;
			w[i].work.left = l + 3;
			w[i].work.top = t + 22;
			w[i].work.right = l + ww - 4;
			w[i].work.bottom = t + hh - 4;
			w[i].band = w[i].outer;
			w[i].band.bottom = w[i].work.top;
			w[i].pixels = pix[i % PNB];
			w[i].pitch = PW * 4;
			w[i].a_frame = 255;
			w[i].a_work = 255;
			w[i].shown = w[i].outer;
			w[i].fade = 255;
			switch ( prnd() % 10 ) {
			case 0:	w[i].round = 6;				break;
			case 1:	w[i].a_frame = 180;			break;
			case 2:	w[i].wash = 0x203040; w[i].wash_str = 80;	break;
			case 3:	w[i].shd_dark = 100; w[i].shd_dx = 4; w[i].shd_dy = 6; w[i].shd_size = 8;	break;
			default: break;					/* plain */
			}
		}
		for ( want = 0; want < n && igt_cp_plain(&w[want]); want++ ) ;
		for ( j = 0; j < (UD)PW * PH; j++ ) ((UW *)bc)[j] = ((UW *)bg)[j] = 0xa5a5a5a5U ^ (UW)j;

		knl_wm_hold(TRUE);
		/* the processor's stages */
		for ( i = dc.box.top; i < dc.box.bottom; i++ ) {
			knl_memcpy(bc + (UBINT)i * PW * 4 + (UBINT)dc.box.left * 4, ground + (SZ)i * PW + dc.box.left,
				   (SZ)( dc.box.right - dc.box.left ) * 4);
		}
		for ( i = 0; i < n; i++ ) cp_window(&dc, &w[i]);
		/* the plan, done as copies, and the processor's stages for the rest */
		laid = igt_cp_plan(&dg, ground, PW * 4, w, n, ops, IGT_CP_MAXOPS, &nops);
		igt_cp_run_cpu(ops, nops);
		for ( i = laid; i < n; i++ ) cp_window(&dg, &w[i]);
		knl_wm_hold(FALSE);

		laid_total += laid;
		if ( laid < n ) cut++;
		for ( j = 0; j < (UD)PW * PH && ((UW *)bc)[j] == ((UW *)bg)[j]; j++ ) ;
		if ( laid != want || j != (UD)PW * PH ) {
			if ( bad < 3 ) {
				tm_printf((UB *)"KTEST igt_plan: scene %d: %d windows, laid %d of %d wanted, box (%d,%d)-(%d,%d)\n",
					  s, n, laid, want, dc.box.left, dc.box.top, dc.box.right, dc.box.bottom);
			}
			bad++;
		}
	}
	tm_printf((UB *)"KTEST igt_plan: 300 scenes, %d windows laid by the plan, %d scenes ended by a look, %d different\n",
		  laid_total, cut, bad);
	KT_ASSERT_EQ(bad, 0);
	knl_vunmap(bc, pages);
	knl_vunmap(bg, pages);
	knl_vunmap(ground, pages);
	for ( i = 0; i < PNB; i++ ) knl_vunmap(pix[i], pages);
	Kfree(ops);
}

/* ---------------------------------------------------------------- a process's OpenGL ES */

#define GL_PROG		"/boot/GLPROG.ELF"

LOCAL INT run_gl( CONST char *mode )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_FSTAT	st;
	SZ	n = 0;
	ID	pid;
	ER	er;

	if ( fs_stat(GL_PROG, &st) < EX_OK ) return -2;
	while ( mode[n] != '\0' ) n++;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = (void *)mode;
	cprc.argsz  = n;
	pid = ts_cre_prc(GL_PROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return -1;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 600000);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return -1;
	}
	return psts.exitcd;
}

LOCAL void test_igt_irispath( void )
{
	INT	code = run_gl("irispath");

	if ( code == -2 ) KT_SKIP("no " GL_PROG " (make GL=1)");
	KT_ASSERT_EQ(code, 0);
}

LOCAL void test_igt_softpipe( void )
{
	INT	code = run_gl("tri");

	if ( code == -2 ) KT_SKIP("no " GL_PROG " (make GL=1)");
	KT_ASSERT_EQ(code, 0);
}

LOCAL void test_igt_i915sync( void )
{
	INT	code = run_gl("i915sync");

	if ( code == -2 ) KT_SKIP("no " GL_PROG " (make GL=1)");
	KT_ASSERT_EQ(code, 0);
}

/* ---------------------------------------------------------------- compositing on the render engine */

LOCAL void test_igt_rcmd( void )
{
	IGT_RD	d;
	UW	b[64], *e;

	knl_memset(&d, 0, sizeof(d));
	d.fs = CP_FS_SHADOW;
	d.barrier = 1;
	d.l = 37;
	d.t = 5;
	d.r = 100;
	d.b = 25;
	knl_memset(b, 0xee, sizeof(b));
	e = igt_rc_rect(b, &d, 0x12340);
	/* PIPE_CONTROL: 3D, opcode 2, sub-opcode 0, 6 words; the command streamer's stall and the data cache */
	KT_ASSERT_EQ(b[0], 0x7a000004U);
	KT_ASSERT(( b[1] & ( 1U << 20 ) ) != 0);
	KT_ASSERT(( b[1] & ( 1U << 5 ) ) != 0);
	/* MEDIA_CURBE_LOAD: media pipeline, opcode 0, sub-opcode 1, 4 words; the length and the start */
	KT_ASSERT_EQ(b[6], 0x70010002U);
	KT_ASSERT_EQ(b[8] & 0x1ffff, (UW)IGT_CP_NPUSH * 4);
	KT_ASSERT_EQ(b[9], 0x12340U);
	/* GPGPU_WALKER: media pipeline, opcode 1, sub-opcode 5, 15 words */
	KT_ASSERT_EQ(b[10], 0x7105000dU);
	KT_ASSERT_EQ(b[11] & 0x3f, (UW)CP_FS_SHADOW);		/* the interface descriptor */
	KT_ASSERT_EQ(b[14] >> 30, 1U);				/* SIMD16 */
	KT_ASSERT_EQ(b[14] & 0x3f, 0U);			/* one thread to a group */
	KT_ASSERT_EQ(b[15], 0U);				/* from group 0 across */
	KT_ASSERT_EQ(b[17], 5U);				/* pixels 32 .. 111: five groups across */
	KT_ASSERT_EQ(b[18], 0U);
	KT_ASSERT_EQ(b[20], 20U);				/* rows 5 .. 24 */
	KT_ASSERT_EQ(b[22], 1U);				/* one deep */
	KT_ASSERT_EQ(b[23], 0xffffU);
	KT_ASSERT_EQ(b[24], 0xffffffffU);
	/* MEDIA_STATE_FLUSH */
	KT_ASSERT_EQ(b[25], 0x70040000U);
	KT_ASSERT(e == b + 27);
	KT_ASSERT_EQ(b[27], 0xeeeeeeeeU);

	/* without a barrier: the push constants first; a rectangle on 16 pixels */
	d.barrier = 0;
	d.fs = CP_FS_COPY;
	d.l = 16;
	d.r = 32;
	e = igt_rc_rect(b, &d, 0x40);
	KT_ASSERT_EQ(b[0], 0x70010002U);
	KT_ASSERT_EQ(b[3], 0x40U);
	KT_ASSERT_EQ(b[5] & 0x3f, (UW)CP_FS_COPY);
	KT_ASSERT_EQ(b[11], 1U);
	KT_ASSERT(e == b + 21);
}

/* The test's memory in a GPU space of its own: the back buffer, the ground, the windows */
#define RP_W		400
#define RP_H		300
#define RP_PITCH	( RP_W * 4 + 64 )
#define RP_BACK		0x10000000ULL
#define RP_GROUND	0x20000000ULL
#define RP_WIN		0x30000000ULL
#define RP_T1		0x50000000ULL
#define RP_T2		0x60000000ULL

LOCAL UB	*rp_back, *rp_ground, *rp_pix[3];

LOCAL UD rp_gpu( void *ctx, CONST void *p, UD len )
{
	UD	size = (UD)RP_PITCH * RP_H;
	INT	k;

	(void)ctx;
	if ( (CONST UB *)p >= rp_back && (CONST UB *)p + len <= rp_back + size ) return RP_BACK + (UD)( (CONST UB *)p - rp_back );
	if ( (CONST UB *)p >= rp_ground && (CONST UB *)p + len <= rp_ground + size ) {
		return RP_GROUND + (UD)( (CONST UB *)p - rp_ground );
	}
	for ( k = 0; k < 3; k++ ) {
		if ( (CONST UB *)p >= rp_pix[k] && (CONST UB *)p + len <= rp_pix[k] + size ) {
			return RP_WIN + (UD)k * 0x1000000 + (UD)( (CONST UB *)p - rp_pix[k] );
		}
	}
	return 0;
}

LOCAL void rp_win( T_CPWIN *w, INT k, INT l, INT t, INT ww, INT hh )
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
	w->pixels = rp_pix[k];
	w->pitch = RP_PITCH;
	w->a_frame = 255;
	w->a_work = 255;
	w->shown = w->outer;
	w->fade = 255;
}

LOCAL void rp_look( T_CPWIN *w, UINT a_frame, INT round, INT blur, UINT dark )
{
	w->a_frame = a_frame;
	w->round = round;
	w->blur = blur;
	w->wash = 0x202838;
	w->wash_str = 90;
	w->tint = 0x336699;
	w->tint_str = 50;
	if ( dark != 0 ) {
		w->shd_dx = 6;
		w->shd_dy = 8;
		w->shd_size = 10;
		w->shd_dark = dark;
	}
}

/* One scene planned and looked over: the number of rectangles of each shader added to cnt */
LOCAL void rp_scene( CONST T_DPRECT *box, CONST T_CPWIN *w, INT n, IGT_RD *rd, CONST IGT_RENV *env, INT *cnt )
{
	T_CPDST	d;
	INT	laid, nrd = 0, i, k, g0 = 0;
	UD	tb = (UD)RP_PITCH * ( RP_H + ( ( RP_H - 1 ) >> 1 ) + 1 );

	d.back = rp_back;
	d.pitch = RP_PITCH;
	d.box = *box;
	d.rows = RP_H;
	laid = igt_rplan(&d, (CONST UW *)rp_ground, RP_PITCH, w, n, env, rd, IGT_RD_MAX, &nrd);
	KT_ASSERT_EQ(laid, n);
	KT_ASSERT(nrd >= 1);
	if ( nrd < 1 ) return;
	/* the ground first, over the part, from the ground into the back buffer */
	KT_ASSERT_EQ(rd[0].fs, CP_FS_COPY);
	KT_ASSERT(rd[0].l == box->left && rd[0].t == box->top && rd[0].r == box->right && rd[0].b == box->bottom);
	KT_ASSERT_EQ(rd[0].par[IGT_CP_P_BUF + 2 * CP_B_SRC], (UW)RP_GROUND);
	for ( i = 0; i < nrd; i++ ) {
		CONST IGT_RD	*r = &rd[i];
		UD		dst = (UD)r->par[IGT_CP_P_DST] | ( (UD)r->par[IGT_CP_P_DST + 1] << 32 );

		cnt[r->fs]++;
		KT_ASSERT(r->fs < CP_NFS);
		KT_ASSERT(r->l < r->r && r->t < r->b);
		KT_ASSERT(r->par[IGT_CP_P_RL] == (UW)r->l && r->par[IGT_CP_P_RT] == (UW)r->t
			  && r->par[IGT_CP_P_RR] == (UW)r->r && r->par[IGT_CP_P_RB] == (UW)r->b);
		KT_ASSERT_EQ(r->par[IGT_CP_P_DPITCH], (UW)RP_PITCH);
		KT_ASSERT_EQ(r->par[CP_P_ORGX], 0U);
		if ( dst == RP_BACK ) {
			/* on the screen: within the part */
			KT_ASSERT(r->l >= box->left && r->t >= box->top && r->r <= box->right && r->b <= box->bottom);
		} else {
			/* a level of the spread, within T1 or T2 */
			KT_ASSERT(r->fs == CP_FS_HALVE || r->fs == CP_FS_SMOOTH3 || r->fs == CP_FS_SMOOTH5
				  || r->fs == CP_FS_DOUBLE);
			KT_ASSERT(( dst >= RP_T1 && dst + (UD)r->b * RP_PITCH <= RP_T1 + tb )
				  || ( dst >= RP_T2 && dst + (UD)r->b * RP_PITCH <= RP_T2 + tb ));
			KT_ASSERT(r->l >= 0 && r->t >= 0 && r->r <= RP_W && r->b <= RP_H);
		}
		for ( k = 0; k < CP_NBUF; k++ ) {
			KT_ASSERT(r->par[IGT_CP_P_BUF + 2 * k] != 0 || r->par[IGT_CP_P_BUF + 2 * k + 1] != 0);
		}
		/* a group's rectangles on the screen apart from each other */
		if ( r->barrier ) g0 = i;
		for ( k = g0; k < i && dst == RP_BACK; k++ ) {
			CONST IGT_RD	*q = &rd[k];
			UD		qd = (UD)q->par[IGT_CP_P_DST] | ( (UD)q->par[IGT_CP_P_DST + 1] << 32 );

			if ( qd != RP_BACK ) continue;
			KT_ASSERT(q->r <= r->l || r->r <= q->l || q->b <= r->t || r->b <= q->t);
		}
	}
	/* the first rectangle of each window's stage after the ground begins a group */
	KT_ASSERT(nrd == 1 || rd[1].barrier);
}

LOCAL void test_igt_rplan( void )
{
	static CONST T_DPRECT	all = { 0, 0, RP_W, RP_H }, part = { 37, 21, 311, 233 };
	UD		pages = ( (UD)RP_PITCH * RP_H + PAGE_SIZE - 1 ) >> PAGE_SHIFT;
	IGT_RD		*rd = (IGT_RD *)knl_vmap(( sizeof(IGT_RD) * IGT_RD_MAX + PAGE_SIZE - 1 ) >> PAGE_SHIFT, 0);
	IGT_RENV	env;
	T_CPWIN		w[3];
	INT		cnt[CP_NFS], k, nlook;

	rp_back = (UB *)knl_vmap(pages, 0);
	rp_ground = (UB *)knl_vmap(pages, 0);
	for ( k = 0; k < 3; k++ ) rp_pix[k] = (UB *)knl_vmap(pages, 0);
	KT_ASSERT(rd != NULL && rp_back != NULL && rp_ground != NULL && rp_pix[2] != NULL);
	if ( rd == NULL || rp_back == NULL || rp_ground == NULL || rp_pix[2] == NULL ) return;
	knl_memset(&env, 0, sizeof(env));
	env.gpu = rp_gpu;
	env.t1 = RP_T1;
	env.t2 = RP_T2;
	for ( k = 0; k <= CP_ROUND_MAX; k++ ) env.cov[k] = 0x40000000ULL + (UD)k * 0x1000;
	knl_memset(cnt, 0, sizeof(cnt));

	rp_scene(&all, w, 0, rd, &env, cnt);
	rp_scene(&part, w, 0, rd, &env, cnt);
	rp_win(&w[0], 0, 40, 30, 250, 180);
	rp_scene(&all, w, 1, rd, &env, cnt);
	KT_ASSERT_EQ(cnt[CP_FS_PIECE], 0);			/* solid and square: copied */
	rp_win(&w[1], 1, 150, 100, 230, 170);
	rp_look(&w[0], 255, 12, 0, 150);
	rp_look(&w[1], 170, 12, 0, 150);
	rp_scene(&all, w, 2, rd, &env, cnt);
	KT_ASSERT(cnt[CP_FS_SHADOW] > 0 && cnt[CP_FS_PIECE] > 0);
	rp_win(&w[2], 2, 90, 60, 200, 150);
	for ( k = 0; k < 3; k++ ) rp_look(&w[k], 140, 8, 6, 120);
	w[2].a_work = 190;
	rp_scene(&all, w, 3, rd, &env, cnt);
	rp_scene(&part, w, 3, rd, &env, cnt);
	KT_ASSERT(cnt[CP_FS_HALVE] > 0 && cnt[CP_FS_DOUBLE] > 0 && cnt[CP_FS_SMOOTH3] + cnt[CP_FS_SMOOTH5] > 0);
	w[1].shown.left += 20;
	w[1].shown.right -= 30;
	w[1].fade = 120;
	rp_scene(&all, w, 2, rd, &env, cnt);
	KT_ASSERT(cnt[CP_FS_MOVING] > 0);

	/* without the levels, the plan stops at the first window that spreads */
	env.t1 = env.t2 = 0;
	{
		T_CPDST	d;
		INT	nrd = 0;

		d.back = rp_back;
		d.pitch = RP_PITCH;
		d.box = all;
		d.rows = RP_H;
		nlook = igt_rplan(&d, (CONST UW *)rp_ground, RP_PITCH, w, 3, &env, rd, IGT_RD_MAX, &nrd);
		KT_ASSERT_EQ(nlook, 0);
		/* more rectangles than room: none */
		env.t1 = RP_T1;
		env.t2 = RP_T2;
		KT_ASSERT_EQ(igt_rplan(&d, (CONST UW *)rp_ground, RP_PITCH, w, 3, &env, rd, 3, &nrd), E_LIMIT);
	}
	tm_printf((UB *)"KTEST igt_rplan: rectangles: piece %d, shadow %d, halve %d, smooth %d, double %d, moving %d, copy %d\n",
		  cnt[CP_FS_PIECE], cnt[CP_FS_SHADOW], cnt[CP_FS_HALVE], cnt[CP_FS_SMOOTH3] + cnt[CP_FS_SMOOTH5],
		  cnt[CP_FS_DOUBLE], cnt[CP_FS_MOVING], cnt[CP_FS_COPY]);

	knl_vunmap(rd, ( sizeof(IGT_RD) * IGT_RD_MAX + PAGE_SIZE - 1 ) >> PAGE_SHIFT);
	knl_vunmap(rp_back, pages);
	knl_vunmap(rp_ground, pages);
	for ( k = 0; k < 3; k++ ) knl_vunmap(rp_pix[k], pages);
}

/* ---------------------------------------------------------------- the GT's power */

/*
 * igt_pm.c against registers of the test's (memory): the acknowledgements
 * of forcewake always set, RP_STATE_CAP 1100/700/300 MHz, no power
 * controller (the efficient frequency stays RP1)
 */
LOCAL void test_igt_pm( void )
{
	UD	npages = 2 * MB / PAGE_SIZE;
	UB	*mm;
	IGT	*save;
	UW	ev;
	UINT	imask;
	BOOL	wake;

#define R(off)	( *(volatile UW *)( mm + (off) ) )
	if ( knl_igt.mmio != 0 ) KT_SKIP("the GPU's driver runs");
	mm = (UB *)knl_vmap(npages, 0);
	save = (IGT *)Kmalloc(sizeof(IGT));
	KT_ASSERT(mm != NULL && save != NULL);
	if ( mm == NULL || save == NULL ) return;
	knl_memcpy(save, &knl_igt, sizeof(IGT));
	knl_memset(mm, 0, 2 * MB);
	R(FW_RENDER_ACK) = FW_KERNEL;
	R(FW_MEDIA_ACK) = FW_KERNEL;
	R(FW_GT_ACK) = FW_KERNEL;
	R(RP_STATE_CAP) = ( 6 << 16 ) | ( 14 << 8 ) | 22;
	R(RC6_CTX_INFO) = 1;
	R(RC_CONTROL) = RC_CTL_HW_ENABLE | RC_CTL_RC6_ENABLE;
	knl_igt.mmio = (UBINT)mm;
	knl_igt.active = -1;

	/* start: the wells held, both off, RP1 */
	KT_ASSERT(igt_fw_init());
	KT_ASSERT_EQ(R(FW_RENDER), MASK_EN(FW_KERNEL));
	igt_pm_init();
	KT_ASSERT_EQ(R(RC_CONTROL), 0);
	KT_ASSERT_EQ(knl_igt.f_min, 18);
	KT_ASSERT_EQ(knl_igt.f_rp1, 42);
	KT_ASSERT_EQ(knl_igt.f_max, 66);
	KT_ASSERT_EQ(R(RPNSWREQ), FREQ_REQ(42));

	/* RC6 and RPS: the automatic entry off, power gating on, from the lowest, only up can come */
	igt_pm_start();
	KT_ASSERT(knl_igt.rc6_on && knl_igt.rps_on && knl_igt.pm_tsk > 0);
	KT_ASSERT_EQ(R(RC_CONTROL), 0);
	KT_ASSERT_EQ(R(PG_ENABLE), RENDER_PG_EN | MEDIA_PG_EN);
	KT_ASSERT_EQ(R(RC6_THRESHOLD), 37500);
	KT_ASSERT_EQ(R(RPNSWREQ), FREQ_REQ(18));
	KT_ASSERT(( R(RP_CONTROL) & RP_ENABLE ) != 0);
	KT_ASSERT_EQ(R(RP_UP_EI), 12000);		/* 16 ms in units of 16 ticks of 12 MHz */
	KT_ASSERT_EQ(R(RP_UP_THRESHOLD), 11400);
	KT_ASSERT_EQ(R(RP_INTERRUPT_LIMITS), ( 66U << 23 ) | ( 18U << 14 ));
	KT_ASSERT_EQ(R(PMINTRMSK), ~PM_RP_UP_THRESHOLD & ~PMINTR_REDIRECT_TO_GUC_DIS);
	KT_ASSERT_EQ(R(GT_IER(2)), PM_RP_UP_THRESHOLD | PM_RP_DOWN_THRESHOLD);

	/* the GT's word: up by 1, then 2 (doubling), down by 1; masked until the task is done */
	for ( ev = 0; ev < 3; ev++ ) {
		ISpinLock(&knl_igt.lock, &imask);
		wake = igt_pm_irq(( ev < 2 ) ? PM_RP_UP_THRESHOLD : PM_RP_DOWN_THRESHOLD);
		KT_ASSERT(( R(GT_IMR(2)) & ( ( ev < 2 ) ? PM_RP_UP_THRESHOLD : PM_RP_DOWN_THRESHOLD ) ) != 0);
		ISpinUnlock(&knl_igt.lock, &imask);
		KT_ASSERT(wake);
		igt_pm_wake();
		tk_dly_tsk(30);
	}
	KT_ASSERT_EQ(knl_igt.f_cur, 20);
	KT_ASSERT_EQ(R(RPNSWREQ), FREQ_REQ(20));
	KT_ASSERT_EQ(R(GT_IMR(2)) & ( PM_RP_UP_THRESHOLD | PM_RP_DOWN_THRESHOLD ), 0);
	KT_ASSERT_EQ(knl_igt.power_mode, 0);

	/* idle: parked after 10 ms, RC6 asked for, the wells let go, the lowest frequency */
	igt_fw_put();
	igt_pm_tick();
	KT_ASSERT(!knl_igt.parked);
	tk_dly_tsk(20);
	/* not while an engine holds a process's context: the kernel's asked for first */
	{
		IGTVM	uvm;
		IGTCTX	uctx;

		knl_memset(&uvm, 0, sizeof(uvm));
		knl_memset(&uctx, 0, sizeof(uctx));
		uctx.vm = &uvm;
		knl_igt.eng[IGT_RCS].last = &uctx;
		igt_pm_tick();
		KT_ASSERT(!knl_igt.parked);
		KT_ASSERT(knl_igt.fw_held);
		knl_igt.eng[IGT_RCS].last = NULL;
	}
	igt_pm_tick();
	KT_ASSERT(knl_igt.parked);
	KT_ASSERT(!knl_igt.fw_held);
	KT_ASSERT_EQ(R(FW_RENDER), MASK_DIS(FW_KERNEL));
	KT_ASSERT_EQ(R(FW_GT), MASK_DIS(FW_KERNEL));
	KT_ASSERT_EQ(R(RC_CONTROL), RC_CTL_RC6_ENABLE);
	KT_ASSERT_EQ(R(RC_STATE), RC_SW_TARGET(4));
	KT_ASSERT_EQ(R(PG_ENABLE), RENDER_PG_EN | MEDIA_PG_EN);
	KT_ASSERT_EQ(R(RPNSWREQ), FREQ_REQ(18));
	KT_ASSERT_EQ(R(GT_IER(2)), 0);
	KT_ASSERT_EQ(knl_igt.f_cur, 42);		/* not below the efficient one */
	/* a word of the GT while parked is not taken */
	ISpinLock(&knl_igt.lock, &imask);
	KT_ASSERT(!igt_pm_irq(PM_RP_UP_THRESHOLD));
	ISpinUnlock(&knl_igt.lock, &imask);

	/* busy again: woken (the acknowledgement here never falls: told as stuck), unparked */
	igt_fw_get();
	KT_ASSERT(knl_igt.fw_held && !knl_igt.parked);
	KT_ASSERT_EQ(R(FW_MEDIA), MASK_EN(FW_KERNEL));
	KT_ASSERT_EQ(knl_igt.n_wake, 1);
	KT_ASSERT_EQ(knl_igt.n_fw_stuck, 3);
	KT_ASSERT_EQ(R(RC_CONTROL), 0);
	KT_ASSERT_EQ(R(RPNSWREQ), FREQ_REQ(42));
	KT_ASSERT_EQ(R(GT_IER(2)), PM_RP_UP_THRESHOLD | PM_RP_DOWN_THRESHOLD);

	/* the context RC6 saves found corrupted at the next park: RC6 off for good, the wells kept */
	R(RC6_CTX_INFO) = 0;
	igt_fw_put();
	tk_dly_tsk(20);
	igt_pm_tick();
	KT_ASSERT(knl_igt.parked && !knl_igt.rc6_on && knl_igt.fw_always && knl_igt.fw_held);
	KT_ASSERT_EQ(R(RC_CONTROL), 0);
	KT_ASSERT_EQ(R(PG_ENABLE), 0);
	KT_ASSERT_EQ(R(FW_RENDER), MASK_EN(FW_KERNEL));
	tk_dly_tsk(1100);				/* the task tells it */

	(void)tk_ter_tsk(knl_igt.pm_tsk);
	(void)tk_del_tsk(knl_igt.pm_tsk);
	if ( knl_igt.fw_mtx > 0 ) (void)tk_del_mtx(knl_igt.fw_mtx);
	knl_memcpy(&knl_igt, save, sizeof(IGT));
	Kfree(save);
	knl_vunmap(mm, npages);
#undef R
}

/* ---------------------------------------------------------------- BOs placed by a process */

/*
 * A GPU of the test's own whose spaces take BOs where their user places
 * them, as Intel's do: what is entered and taken out is recorded
 */
#define PL_MAX	16
#define GP_PROG	"/boot/GPPROG.ELF"

LOCAL struct {
	INT	n, spaces, destroyed;
	UD	va[PL_MAX];
	UD	npages[PL_MAX];
	BOOL	in[PL_MAX];
} pl;

LOCAL UW pl_space;

LOCAL ER pl_info( void *priv, T_GPUINFO *info )
{
	(void)priv;
	info->kind = GPU_KIND_TEST;
	info->feature = GPU_FEAT_BATCH;
	return E_OK;
}
LOCAL ER pl_bo_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset )
{
	(void)priv; (void)bo; (void)npages;
	*p_offset = 0x10000;
	return E_OK;
}
LOCAL void pl_bo_unbind( void *priv, UW offset, UD npages ) { (void)priv; (void)offset; (void)npages; }
LOCAL ER pl_kick( void *priv, CONST T_GPUJOB *job, void *as )
{
	(void)priv; (void)job; (void)as;
	knl_gpu_job_end(E_OK);
	return E_OK;
}
LOCAL void pl_reset( void *priv ) { (void)priv; }
LOCAL ER pl_as_create( void *priv, void **p_as )
{
	(void)priv;
	pl.spaces++;
	*p_as = &pl_space;
	return E_OK;
}
LOCAL void pl_as_destroy( void *priv, void *as ) { (void)priv; (void)as; pl.destroyed++; }
LOCAL ER pl_as_bind( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va )
{
	(void)priv; (void)as; (void)bo; (void)npages;
	*p_va = 0;
	return E_OK;
}
LOCAL void pl_as_unbind( void *priv, void *as, UW va, UD npages ) { (void)priv; (void)as; (void)va; (void)npages; }
LOCAL void pl_note( UD va, UD npages, BOOL in )
{
	if ( pl.n < PL_MAX ) {
		pl.va[pl.n] = va;
		pl.npages[pl.n] = npages;
		pl.in[pl.n] = in;
		pl.n++;
	}
}
LOCAL ER pl_as_bind_at( void *priv, void *as, T_GPUBO *bo, UD npages, UD va )
{
	(void)priv; (void)as; (void)bo;
	pl_note(va, npages, TRUE);
	return E_OK;
}
LOCAL void pl_as_unbind_at( void *priv, void *as, UD va, UD npages )
{
	(void)priv; (void)as;
	pl_note(va, npages, FALSE);
}

LOCAL CONST T_GPUOPS	pl_ops = {
	GPU_KIND_TEST, GPU_FEAT_BATCH, 0, pl_info, pl_bo_bind, pl_bo_unbind, pl_kick, pl_reset,
	pl_as_create, pl_as_destroy, pl_as_bind, pl_as_unbind, pl_as_bind_at, pl_as_unbind_at, TRUE
};

LOCAL void test_igt_place( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	T_FSTAT	st;
	ID	pid;
	ER	er;
	INT	i;
	static CONST UD	want_va[] = { 0xffff800000123000ULL, 0xffff800000123000ULL, 0x200000, 0x400000,
				      0x400000, 0x600000, 0x600000, 0x601000, 0x700000, 0x700000, 0x200000,
				      0x601000 };
	static CONST BOOL want_in[] = { TRUE, FALSE, TRUE, TRUE, FALSE, TRUE, FALSE, TRUE, TRUE, FALSE, FALSE,
					FALSE };
	static CONST UD	want_np[] = { 2, 2, 2, 1, 1, 2, 2, 1, 2, 2, 2, 1 };
	INT	nwant = (INT)( sizeof(want_va) / sizeof(want_va[0]) );

	if ( fs_stat(GP_PROG, &st) < EX_OK ) KT_SKIP("no " GP_PROG);
	knl_memset(&pl, 0, sizeof(pl));
	er = knl_gpu_register(&pl_ops, NULL);
	if ( er == E_OBJ ) KT_SKIP("a GPU is driven");
	KT_ASSERT_ER(er, E_OK);
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = (void *)"place";
	cprc.argsz  = 5;
	pid = ts_cre_prc(GP_PROG, &cprc);
	KT_ASSERT(pid > 0);
	psts.exitcd = -1;
	if ( pid > 0 ) {
		er = ts_wai_prc(pid, &psts, 60000);
		KT_ASSERT_ER(er, E_OK);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
	for ( i = 0; i < 50 && pl.destroyed == 0; i++ ) tk_dly_tsk(20);
	tm_printf((UB *)"KTEST igt_place: %d entered or taken out, %d spaces made, %d destroyed\n", pl.n,
		  pl.spaces, pl.destroyed);
	/*
	 * placed, moved (out of the first place, into the second), another
	 * placed and deleted; a third placed, a fourth placed over its second
	 * page (the third taken out whole first), the third moved (nothing
	 * taken out of the fourth's place) and deleted; the first and the
	 * fourth out at the end
	 */
	KT_ASSERT_EQ(pl.n, nwant);
	for ( i = 0; i < pl.n && i < nwant; i++ ) {
		KT_ASSERT_EQ(pl.va[i], want_va[i]);
		KT_ASSERT_EQ(pl.in[i], want_in[i]);
		KT_ASSERT_EQ(pl.npages[i], want_np[i]);
	}
	KT_ASSERT_EQ(pl.spaces, 1);
	KT_ASSERT_EQ(pl.destroyed, 1);
	KT_ASSERT_ER(knl_gpu_unregister(), E_OK);
}

/* ---------------------------------------------------------------- the 11th generation and later */

LOCAL void test_igt12_gen( void )
{
	CONST T_IGTGEN	*g;

	/* the 9th generation's are not in the table: such a machine goes on as before */
	KT_ASSERT(knl_igt_gen(0x5917) == NULL);
	KT_ASSERT(knl_igt_gen(0x591c) == NULL);
	KT_ASSERT(knl_igt_gen(0x87c0) == NULL);
	KT_ASSERT(knl_igt_gen(0x3e9b) == NULL);
	KT_ASSERT(knl_igt_gen(0x9b41) == NULL);
	KT_ASSERT(knl_igt_gen(0x0000) == NULL);
	KT_ASSERT(knl_igt_gen(0xffff) == NULL);

	/* the 11th generation: named, its registers not read, whatever the first and last of each */
	g = knl_igt_gen(0x8a50);
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO && g->uc == IGT_UC_NONE && !g->discrete);
	g = knl_igt_gen(0x8a52);
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x8a71);
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x4541);	/* Elkhart Lake */
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x4571);
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x4e51);	/* Jasper Lake */
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x4e71);
	KT_ASSERT(g != NULL && g->verx10 == 110 && g->read == IGT_READ_NO);

	g = knl_igt_gen(0x9a49);
	KT_ASSERT(g != NULL);
	if ( g != NULL ) {
		KT_ASSERT_EQ(g->verx10, 120);
		KT_ASSERT_EQ(g->uc, IGT_UC_NONE);
		KT_ASSERT_EQ(g->read, IGT_READ_PRM);
		KT_ASSERT(!g->discrete);
	}
	g = knl_igt_gen(0x4680);	/* Alder Lake-S */
	KT_ASSERT(g != NULL && g->uc == IGT_UC_HUC && g->read == IGT_READ_SAME);
	g = knl_igt_gen(0x46a6);	/* Alder Lake-P */
	KT_ASSERT(g != NULL && g->uc == IGT_UC_GUC && g->read == IGT_READ_SAME);
	g = knl_igt_gen(0x56a0);	/* DG2: its own memory, not read */
	KT_ASSERT(g != NULL && g->discrete && g->read == IGT_READ_NO);
	g = knl_igt_gen(0x7d55);	/* Meteor Lake: registers not confirmed */
	KT_ASSERT(g != NULL && g->verx10 == 127 && g->read == IGT_READ_NO);
	g = knl_igt_gen(0xb080);	/* Panther Lake */
	KT_ASSERT(g != NULL && g->verx10 == 300);
	g = knl_igt_gen(0x4626);
	KT_ASSERT(g != NULL);
	g = knl_igt_gen(0xfd81);
	KT_ASSERT(g != NULL);
}

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, UD n )
{
	UD	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

/* The state read from memory of the test's: the fields from their places, and not one byte written */
LOCAL void test_igt12_read( void )
{
	UD		npages = 2 * MB / PAGE_SIZE;
	UB		*mm, *copy;
	T_IGT12STATE	s;

#define R12(off)	( *(volatile UW *)( mm + (off) ) )
	mm = (UB *)knl_vmap(npages, 0);
	copy = (UB *)knl_vmap(npages, 0);
	KT_ASSERT(mm != NULL && copy != NULL);
	if ( mm == NULL || copy == NULL ) return;
	knl_memset(mm, 0xa5, 2 * MB);
	R12(0x09138) = 0x01;
	R12(0x0913c) = 0x3f;
	R12(0x09134) = 0x02;
	R12(0x09140) = 0x000c0003;
	R12(0x108040) = 0x1234;
	R12(0x1080c0) = 0x80000000;
	R12(0x1080c4) = 0x1;
	R12(0x00d84) = 0;		/* render asleep */
	R12(0x130044) = 0x0001;		/* the GT awake */
	R12(0x00d88) = 0;
	R12(0x0229c) = 0xdead0001;	/* must not be taken */
	R12(0x2229c) = 0x00008000;
	R12(0x22234) = 0x11;
	R12(0x22238) = 0x22;
	R12(0x0c000) = 0x33;
	R12(0x0c340) = 0x44;
	R12(0x45400) = 0x0f;
	R12(0x45404) = 0x05;
	R12(0x70008) = 0xc0000000;
	R12(0x60400) = 0x87000000;
	R12(0x70180) = 0x84000000;
	R12(0x7019c) = 0x00100000;
	R12(0x73008) = 0x11111111;
	knl_memcpy(copy, mm, 2 * MB);

	igt12_read((UBINT)mm, &s);
	KT_ASSERT(same_bytes(mm, copy, 2 * MB));
	KT_ASSERT(!s.dead);
	KT_ASSERT_EQ(s.slice_en, 1);
	KT_ASSERT_EQ(s.dss_en, 0x3f);
	KT_ASSERT_EQ(s.eu_dis, 2);
	KT_ASSERT_EQ(s.media_dis, 0x000c0003);
	KT_ASSERT_EQ(s.ggc, 0x1234);
	KT_ASSERT_EQ(s.dsm_hi, 1);
	KT_ASSERT(!s.rcs_read);
	KT_ASSERT(s.gt_read);
	KT_ASSERT_EQ(s.bcs_mode, 0x8000);
	KT_ASSERT_EQ(s.bcs_st_lo, 0x11);
	KT_ASSERT_EQ(s.bcs_st_hi, 0x22);
	KT_ASSERT_EQ(s.guc_status, 0x33);
	KT_ASSERT_EQ(s.guc_wopcm, 0x44);
	KT_ASSERT_EQ(s.pwr_bios, 0x0f);
	KT_ASSERT_EQ(s.pwr_driver, 5);
	KT_ASSERT_EQ(s.pipe[0].conf, 0xc0000000U);
	KT_ASSERT_EQ(s.pipe[0].ddi, 0x87000000U);
	KT_ASSERT_EQ(s.pipe[0].plane_surf, 0x00100000);
	KT_ASSERT_EQ(s.pipe[3].conf, 0x11111111);

	/* the other way round: the GT's asleep, nothing of it read */
	R12(0x00d84) = 1;
	R12(0x130044) = 0;
	knl_memcpy(copy, mm, 2 * MB);
	igt12_read((UBINT)mm, &s);
	KT_ASSERT(same_bytes(mm, copy, 2 * MB));
	KT_ASSERT(s.rcs_read && !s.gt_read);
	KT_ASSERT_EQ(s.rcs_mode, 0xdead0001U);

	/* a device that reads all ones: stopped at the first */
	R12(0x09138) = 0xffffffffU;
	igt12_read((UBINT)mm, &s);
	KT_ASSERT(s.dead);
#undef R12
	knl_vunmap(mm, npages);
	knl_vunmap(copy, npages);
}

#ifdef IGT12
/* The formats against the published layout of the fields (IGT12=1) */
LOCAL void test_igt12_fmt( void )
{
	UD		npages = 2 * MB / PAGE_SIZE, d;
	UB		*mm;
	T_IGT12CS	cs;
	INT		i;

	d = igt12_desc(0x00123000, 5, 2, 0x7fe, 63, 1, TRUE);
	KT_ASSERT_EQ((UW)d, 0x00123000 | 0x100 | 0x18 | 0x4 | 0x1 | ( 1 << 9 ));
	KT_ASSERT_EQ((UW)( d >> 32 ), ( 5U << 29 ) | ( 63U << 23 ) | ( 2U << 16 ) | ( 0x7feU << 5 ));
	d = igt12_desc(0x1000, 0, 0, 1, 0, -1, FALSE);
	KT_ASSERT_EQ((UW)d, 0x1000 | 0x100 | 0x18 | 0x1);		/* no priority where it is not programmed */
	KT_ASSERT_EQ(igt12_desc(0x1001, 0, 0, 1, 0, -1, FALSE), 0);	/* not on a page */
	KT_ASSERT_EQ(igt12_desc(0, 0, 0, 1, 0, -1, FALSE), 0);
	KT_ASSERT_EQ(igt12_desc(0x1000, 8, 0, 1, 0, -1, FALSE), 0);
	KT_ASSERT_EQ(igt12_desc(0x1000, 0, 64, 1, 0, -1, FALSE), 0);
	KT_ASSERT_EQ(igt12_desc(0x1000, 0, 0, IGT12_IDLE_CTX, 0, -1, FALSE), 0);	/* stands for idle */
	KT_ASSERT_EQ(igt12_desc(0x1000, 0, 0, 1, 64, -1, FALSE), 0);
	KT_ASSERT_EQ(igt12_desc(0x1000, 0, 0, 1, 0, 3, FALSE), 0);

#define CSQW(to, inst, cls, nq, away, det) \
	( (UD)( ( (UW)(to) << 15 ) | ( (UW)(inst) << 6 ) | ( (UW)(cls) << 3 ) | (UW)(nq) ) \
	  | ( (UD)( ( (UW)(away) << 15 ) | (UW)(det) ) << 32 ) )
	igt12_cs_decode(CSQW(0xab, 1, 3, 1, IGT12_IDLE_CTX, 0), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_IDLE_TO_ACTIVE);
	KT_ASSERT_EQ(cs.to, 0xab);
	KT_ASSERT_EQ(cs.instance, 1);
	KT_ASSERT_EQ(cs.eng_class, 3);
	igt12_cs_decode(CSQW(IGT12_IDLE_CTX, 0, 0, 0, 0xab, 0), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_ACTIVE_TO_IDLE);
	KT_ASSERT_EQ(cs.away, 0xab);
	KT_ASSERT_EQ(cs.detail, 0);
	igt12_cs_decode(CSQW(IGT12_IDLE_CTX, 0, 0, 1, 0xab, 5), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_PREEMPT_TO_IDLE);
	KT_ASSERT_EQ(cs.detail, 5);
	igt12_cs_decode(CSQW(IGT12_IDLE_CTX, 0, 0, 1, IGT12_IDLE_CTX, 0), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_IDLE_REPORT);
	igt12_cs_decode(CSQW(0x7bc, 0, 0, 1, 0xab, 0), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_NEW_QUEUE);
	igt12_cs_decode(CSQW(0x7ac, 0, 0, 0, 0xab, 0), &cs);
	KT_ASSERT_EQ(cs.kind, IGT12_CS_ELEMENT);
#undef CSQW

	/* on memory of the test's: element 0, the rest invalid, the load last */
	mm = (UB *)knl_vmap(npages, 0);
	KT_ASSERT(mm != NULL);
	if ( mm == NULL ) return;
	knl_memset(mm, 0xa5, 2 * MB);
	igt12_sq_load((UBINT)mm, 0x22000, 0x1122334455667789ULL);
	KT_ASSERT_EQ(*(UW *)( mm + 0x22510 ), 0x55667789);
	KT_ASSERT_EQ(*(UW *)( mm + 0x22514 ), 0x11223344);
	for ( i = 2; i < 16; i++ ) KT_ASSERT_EQ(*(UW *)( mm + 0x22510 + i * 4 ), 0);
	KT_ASSERT_EQ(*(UW *)( mm + 0x22550 ), 1);
	KT_ASSERT_EQ(*(UW *)( mm + 0x2250c ), 0xa5a5a5a5U);
	KT_ASSERT_EQ(*(UW *)( mm + 0x22554 ), 0xa5a5a5a5U);
	knl_vunmap(mm, npages);
}
#endif /* IGT12 */

EXPORT void ktest_igt( void )
{
	if ( knl_igt.ready ) {
		kt_skip("igt", "the device is driven: its tables are not to be faked");
		return;
	}
	KT_RUN(test_igt_ggtt);
	KT_RUN(test_igt_ggtt_scattered);
	KT_RUN(test_igt_ppgtt);
	KT_RUN(test_igt_held);
	KT_RUN(test_igt_kva);
	KT_RUN(test_igt_layout);
	KT_RUN(test_igt_emit);
	KT_RUN(test_igt_plan);
	KT_RUN(test_igt_place);
	KT_RUN(test_igt_irispath);
	KT_RUN(test_igt_softpipe);
	KT_RUN(test_igt_i915sync);
	KT_RUN(test_igt_rcmd);
	KT_RUN(test_igt_rplan);
	KT_RUN(test_igt_pm);
	KT_RUN(test_igt12_gen);
	KT_RUN(test_igt12_read);
#ifdef IGT12
	KT_RUN(test_igt12_fmt);
#endif
}

#endif /* X64 */
