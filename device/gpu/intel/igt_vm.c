/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_vm.c
 *	Address spaces of the render and copy engines: page tables of the
 *	GPU's own (the per-process GTT, PPGTT) of four levels, 48 bits of
 *	address, 4KB pages (docs/tessronos/13-gpu.md 13.11.3).
 *
 *	A table is a page of 512 entries of 64 bits: the physical address of
 *	the next level (or of the page), present, writable, and in a page's
 *	entry the private PAT index that says how the GPU caches it. Pages
 *	are entered write-back in the last level cache, which the processor
 *	shares: what one writes the other reads, without cache maintenance.
 *	The tables are written through the processor's cache too, and their
 *	lines are written back all the same before the GPU may walk them.
 *
 *	Nowhere does a space have a hole: an entry that maps nothing points
 *	at a scratch table of the level below, down to one scratch page of
 *	zeroes. A stray access of a job reaches the scratch page, never
 *	memory that is not the space's, and does not fault (the engines of
 *	this generation stop on a fault). The scratch tables are shared by
 *	every space and never change; tables of a space's own are made as
 *	pages are entered and are freed with the space.
 *
 *	The kernel's space (the compositing and the tests) holds the BOs of
 *	the common layer at 32 bit addresses handed out here; a process's
 *	space holds its BOs where the process places them (gp_bnd_bo: the
 *	user side driver chooses its addresses, as on other systems).
 *
 *	Changes are made under the driver's mutex, from tasks only.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "igt.h"

#define NENT		512
#define IDX(va, lvl)	( (UINT)( ( (va) >> ( 12 + 9 * (lvl) ) ) & ( NENT - 1 ) ) )
#define VA_BITS		48
#define VA_TOP		( 1ULL << VA_BITS )
#define PA_TOP		( 1ULL << 39 )

#define KVA_PAGES	( IGT_KVA_TOP >> 12 )
#define KVA_WORDS	( KVA_PAGES / 64 )

LOCAL UD	*kva_map;		/* a bit a page of the kernel's 32 bit range, set when taken */
LOCAL UD	kva_next;
LOCAL UD	kva_used;

Inline UD *tbl( UD pa )
{
	return (UD *)PA2VA(pa);
}

/* A table filled with one entry (a scratch one of the level below); its physical address, 0 when none */
LOCAL UD pt_new( UD fill )
{
	PFRAME	*pf;
	UD	pa, *t;
	INT	i;

	pf = knl_alloc_pages(0, ZONE_NORMAL, 0);
	if ( pf == NULL ) pf = knl_alloc_pages(0, ZONE_DMA32, KAF_DMA32);
	if ( pf == NULL ) return 0;
	pa = knl_pf_to_pa(pf);
	if ( pa + PAGE_SIZE > PA_TOP ) {
		knl_free_pages(pf, 0);
		return 0;
	}
	t = tbl(pa);
	for ( i = 0; i < NENT; i++ ) t[i] = fill;
	igt_clflush(t, PAGE_SIZE);
	return pa;
}

LOCAL void pt_free( UD pa )
{
	knl_free_pages(knl_pa_to_pf(pa), 0);
}

Inline UD pde( UD pa )
{
	return ( pa & GPTE_ADDR ) | GPTE_PRESENT | GPTE_RW | GPDE_CACHED;
}

Inline UD pte( UD pa )
{
	return ( pa & GPTE_ADDR ) | GPTE_PRESENT | GPTE_RW | GPTE_CACHED;
}

/* The scratch table that an entry of level 'lvl' (3: the top) points at when empty */
LOCAL UD scratch_of( INT lvl )
{
	return ( lvl == 3 ) ? knl_igt.scr_pdp : ( lvl == 2 ) ? knl_igt.scr_pd : knl_igt.scr_pt;
}

EXPORT ER igt_vm_init( void )
{
	PFRAME	*pf;

	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) return E_NOMEM;
	knl_igt.scratch_pa = knl_pf_to_pa(pf);
	igt_clflush(PA2VA(knl_igt.scratch_pa), PAGE_SIZE);
	knl_igt.scr_pt = pt_new(pte(knl_igt.scratch_pa));
	if ( knl_igt.scr_pt == 0 ) return E_NOMEM;
	knl_igt.scr_pd = pt_new(pde(knl_igt.scr_pt));
	if ( knl_igt.scr_pd == 0 ) return E_NOMEM;
	knl_igt.scr_pdp = pt_new(pde(knl_igt.scr_pd));
	if ( knl_igt.scr_pdp == 0 ) return E_NOMEM;

	kva_map = (UD *)Kmalloc(KVA_WORDS * sizeof(UD));
	if ( kva_map == NULL ) return E_NOMEM;
	knl_memset(kva_map, 0, KVA_WORDS * sizeof(UD));
	kva_next = IGT_KVA_LOW >> 12;
	return E_OK;
}

EXPORT IGTVM *igt_vm_create( BOOL kernel )
{
	IGTVM	*vm;

	vm = (IGTVM *)Kmalloc(sizeof(IGTVM));
	if ( vm == NULL ) return NULL;
	knl_memset(vm, 0, sizeof(IGTVM));
	vm->kernel = kernel;
	vm->pml4_pa = pt_new(pde(knl_igt.scr_pdp));
	if ( vm->pml4_pa == 0 ) {
		Kfree(vm);
		return NULL;
	}
	return vm;
}

/* The tables of a space below 'pa' (a table of level 'lvl'), freed */
LOCAL void free_below( UD pa, INT lvl )
{
	UD	*t = tbl(pa);
	INT	i;

	if ( lvl == 0 ) return;
	for ( i = 0; i < NENT; i++ ) {
		UD	child = t[i] & GPTE_ADDR;

		if ( child != scratch_of(lvl) ) {
			free_below(child, lvl - 1);
			pt_free(child);
		}
	}
}

/* A space, its contexts and its tables freed */
LOCAL void vm_free( IGTVM *vm )
{
	INT	h, e;

	for ( h = 0; h < GPU_HWC_MAX; h++ ) {
		for ( e = 0; e < IGT_NENG; e++ ) {
			if ( vm->ctx[h][e] != NULL ) igt_ctx_free(vm->ctx[h][e]);
		}
	}
	free_below(vm->pml4_pa, 3);
	pt_free(vm->pml4_pa);
	Kfree(vm);
}

/*
 * The space of a process that has ended. An engine that ran one of its
 * contexts last still holds that context: the engine writes it back to
 * its image as it goes on to another context, and RC6 saves it and
 * brings it back. Freed now, its image, its ring and its tables would be
 * pages the next process gets (a context's image or a BO of its), and
 * the engine would write the old state over them. So such a space is
 * kept, the kernel's context is asked for on that engine, and the space
 * is freed once no engine holds it (igt_vm_reap, at the next job and
 * from the power task).
 */
EXPORT void igt_vm_destroy( IGTVM *vm )
{
	BOOL	held;

	if ( vm == NULL ) return;
	held = ( !vm->kernel && igt_holds_vm(vm) );
	if ( !held ) {
		vm_free(vm);
		igt_vm_reap();
		return;
	}
	vm->dead_next = knl_igt.dead;
	knl_igt.dead = vm;
	knl_igt.n_held++;
	igt_to_kernel();
}

EXPORT void igt_vm_reap( void )
{
	IGTVM	**pp = &knl_igt.dead;

	while ( *pp != NULL ) {
		IGTVM	*vm = *pp;

		if ( igt_holds_vm(vm) ) {
			pp = &vm->dead_next;
			continue;
		}
		*pp = vm->dead_next;
		vm_free(vm);
		knl_igt.n_reaped++;
	}
}

/*
 * The last level entry of 'va', the tables on the way made when 'make';
 * NULL when there is none (or no memory for one)
 */
LOCAL UD *walk( IGTVM *vm, UD va, BOOL make )
{
	UD	pa = vm->pml4_pa;
	INT	lvl;

	for ( lvl = 3; lvl >= 1; lvl-- ) {
		UD	*t = tbl(pa);
		UINT	i = IDX(va, lvl);
		UD	child = t[i] & GPTE_ADDR;

		if ( child == scratch_of(lvl) ) {
			if ( !make ) return NULL;
			child = pt_new(( lvl == 1 ) ? pte(knl_igt.scratch_pa) : pde(scratch_of(lvl - 1)));
			if ( child == 0 ) return NULL;
			t[i] = pde(child);
			igt_clflush(&t[i], sizeof(UD));
			vm->npt++;
		}
		pa = child;
	}
	return &tbl(pa)[IDX(va, 0)];
}

EXPORT ER igt_vm_map( IGTVM *vm, UD va, CONST UD *pa, UD npages )
{
	UD	i, *e, *run = NULL;
	UD	nrun = 0;

	if ( ( va & 0xfff ) != 0 || va + ( npages << 12 ) > VA_TOP ) return E_PAR;
	for ( i = 0; i < npages; i++ ) {
		if ( pa[i] + PAGE_SIZE > PA_TOP || ( pa[i] & 0xfff ) != 0 ) {
			igt_vm_unmap(vm, va, i);
			return E_PAR;
		}
		e = walk(vm, va + ( i << 12 ), TRUE);
		if ( e == NULL ) {
			if ( run != NULL ) igt_clflush(run, nrun * sizeof(UD));
			igt_vm_unmap(vm, va, i);
			return E_NOMEM;
		}
		*e = pte(pa[i]);
		/* write the lines back a table's run at a time */
		if ( run != NULL && e == run + nrun ) {
			nrun++;
		} else {
			if ( run != NULL ) igt_clflush(run, nrun * sizeof(UD));
			run = e;
			nrun = 1;
		}
	}
	if ( run != NULL ) igt_clflush(run, nrun * sizeof(UD));
	return E_OK;
}

EXPORT void igt_vm_unmap( IGTVM *vm, UD va, UD npages )
{
	UD	i, *e;

	for ( i = 0; i < npages; i++ ) {
		e = walk(vm, va + ( i << 12 ), FALSE);
		if ( e != NULL ) {
			*e = pte(knl_igt.scratch_pa);
			igt_clflush(e, sizeof(UD));
		}
	}
}

EXPORT UD igt_vm_lookup( IGTVM *vm, UD va )
{
	UD	*e = walk(vm, va, FALSE);

	return ( e != NULL ) ? *e : pte(knl_igt.scratch_pa);
}

/* ---------------------------------------------------------------- the kernel's 32 bit addresses */

Inline BOOL kva_taken( UD p )
{
	return ( kva_map[p >> 6] >> ( p & 63 ) ) & 1;
}

Inline void kva_set( UD p, BOOL on )
{
	if ( on ) kva_map[p >> 6] |= 1ULL << ( p & 63 );
	else kva_map[p >> 6] &= ~( 1ULL << ( p & 63 ) );
}

/*
 * Pages taken from where the last search ended, round to the start once:
 * an address given back is not handed out again soon after
 */
EXPORT ER igt_kva_alloc( UD npages, UW *p_va )
{
	UD	low = IGT_KVA_LOW >> 12, p, run = 0, start = 0, looked = 0;

	if ( npages == 0 || npages > KVA_PAGES - low ) return E_PAR;
	p = kva_next;
	while ( looked < ( KVA_PAGES - low ) + npages ) {
		if ( p >= KVA_PAGES ) {
			p = low;
			run = 0;
		}
		if ( kva_taken(p) ) {
			run = 0;
		} else {
			if ( run == 0 ) start = p;
			if ( ++run == npages ) {
				UD	i;

				for ( i = 0; i < npages; i++ ) kva_set(start + i, TRUE);
				kva_next = start + npages;
				kva_used += npages;
				*p_va = (UW)( start << 12 );
				return E_OK;
			}
		}
		p++;
		looked++;
	}
	return E_NOMEM;
}

EXPORT void igt_kva_free( UW va, UD npages )
{
	UD	i, p = (UD)va >> 12;

	for ( i = 0; i < npages && p + i < KVA_PAGES; i++ ) {
		if ( kva_taken(p + i) ) {
			kva_set(p + i, FALSE);
			kva_used--;
		}
	}
}

EXPORT UD igt_kva_free_pages( void )
{
	return KVA_PAGES - ( IGT_KVA_LOW >> 12 ) - kva_used;
}

/* ---------------------------------------------------------------- the common layer's calls */

EXPORT ER igt_bo_bind( void *priv, T_GPUBO *bo, UD npages, UW *p_offset )
{
	UW	va;
	ER	er;

	(void)priv;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	er = igt_kva_alloc(npages, &va);
	if ( er >= E_OK ) {
		er = igt_vm_map(knl_igt.kvm, va, knl_gpu_bo_pages(bo), npages);
		if ( er < E_OK ) igt_kva_free(va, npages);
	}
	tk_unl_mtx(knl_igt.mtx);
	if ( er >= E_OK ) *p_offset = va;
	return er;
}

EXPORT void igt_bo_unbind( void *priv, UW offset, UD npages )
{
	(void)priv;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	igt_vm_unmap(knl_igt.kvm, offset, npages);
	igt_kva_free(offset, npages);
	tk_unl_mtx(knl_igt.mtx);
}

EXPORT ER igt_as_create( void *priv, void **p_as )
{
	IGTVM	*vm;

	(void)priv;
	if ( !knl_igt.ready ) return E_NOSPT;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	vm = igt_vm_create(FALSE);
	tk_unl_mtx(knl_igt.mtx);
	if ( vm == NULL ) return E_NOMEM;
	*p_as = vm;
	return E_OK;
}

EXPORT void igt_as_destroy( void *priv, void *as )
{
	(void)priv;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	igt_vm_destroy((IGTVM *)as);
	tk_unl_mtx(knl_igt.mtx);
}

/*
 * A process's BO is entered where the process places it (gp_bnd_bo),
 * not at an address chosen here: it has none until then
 */
EXPORT ER igt_as_bind( void *priv, void *as, T_GPUBO *bo, UD npages, UW *p_va )
{
	(void)priv;
	(void)as;
	(void)bo;
	(void)npages;
	*p_va = 0;
	return E_OK;
}

EXPORT void igt_as_unbind( void *priv, void *as, UW va, UD npages )
{
	igt_as_unbind_at(priv, as, va, npages);
}

/* An address as a process gives it: the 48 bits, its canonical upper bits dropped */
Inline UD va48( UD va )
{
	return va & ( VA_TOP - 1 );
}

EXPORT ER igt_as_bind_at( void *priv, void *as, T_GPUBO *bo, UD npages, UD va )
{
	ER	er;

	(void)priv;
	va = va48(va);
	if ( va == 0 || ( va & 0xfff ) != 0 || va + ( npages << 12 ) > VA_TOP ) return E_PAR;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	er = igt_vm_map((IGTVM *)as, va, knl_gpu_bo_pages(bo), npages);
	tk_unl_mtx(knl_igt.mtx);
	return er;
}

EXPORT void igt_as_unbind_at( void *priv, void *as, UD va, UD npages )
{
	(void)priv;
	va = va48(va);
	if ( va == 0 ) return;
	tk_loc_mtx(knl_igt.mtx, TMO_FEVR);
	igt_vm_unmap((IGTVM *)as, va, npages);
	tk_unl_mtx(knl_igt.mtx);
}
