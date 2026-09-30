/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	v3d_mmu.c
 *	The V3D's address space (docs/tessronos/13-gpu.md 13.3.3).
 *
 *	The V3D reads memory through a one level page table: one 32 bit
 *	entry per 4KB page of its 32 bit address space, found by the
 *	address >> 12. The whole 4GB takes a 4MB table, which has to be
 *	physically contiguous because the hardware is given only its start;
 *	that is one block of the buddy allocator's largest order, taken
 *	when the driver starts, before memory is broken up.
 *
 *	The GPU does not look at the processor's caches, so the table is
 *	written only through an uncached mapping, after its lines in the
 *	linear mapping were cleaned out once.
 *
 *	GPU addresses are handed out first fit from a bitmap of pages.
 *	Every client shares the one address space, as the hardware has one
 *	table; what keeps processes apart is that a process can only name
 *	BOs it holds (13.5), and that the kernel starts the lists.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "../gpudev.h"
#include "v3d.h"
#include "v3d_regs.h"

LOCAL BOOL used_test( T_V3DMMU *m, UD i )
{
	return (BOOL)( ( m->used[i >> 6] >> ( i & 63 ) ) & 1 );
}

LOCAL void used_set( T_V3DMMU *m, UD i, UD n, BOOL on )
{
	for ( ; n > 0; n--, i++ ) {
		if ( on ) {
			m->used[i >> 6] |= (UD)1 << ( i & 63 );
		} else {
			m->used[i >> 6] &= ~( (UD)1 << ( i & 63 ) );
		}
	}
}

EXPORT UW v3d_mmu_pte( UD pa, BOOL writable )
{
	return (UW)( ( pa >> V3D_PAGE_SHIFT ) & V3D_PTE_PFN_MASK )
		| V3D_PTE_VALID | ( writable ? V3D_PTE_WRITEABLE : 0 );
}

EXPORT ER v3d_mmu_init( T_V3DMMU *m, UD va_size )
{
	UD	bytes, words;
	UINT	order;
	PFRAME	*pf;

	knl_memset(m, 0, sizeof(T_V3DMMU));
	if ( va_size < ( (UD)1 << 22 ) || va_size > V3D_VA_FULL
	  || ( va_size & ( va_size - 1 ) ) != 0 ) {
		return E_PAR;
	}
	m->va_size = va_size;
	m->npte = va_size >> V3D_PAGE_SHIFT;
	bytes = m->npte * sizeof(UW);
	for ( order = 0; ( PAGE_SIZE << order ) < bytes; order++ ) ;
	if ( order > PF_MAX_ORDER ) {
		return E_NOMEM;
	}

	pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return E_NOMEM;
	}
	m->pt_pa = knl_pf_to_pa(pf);
	m->pt_order = order;
	knl_gpu_dcache_clean_inval((UBINT)PA2VA(m->pt_pa), bytes);
	m->pt = (UW *)knl_vmap_pa(m->pt_pa, (UD)1 << order, VMAP_NOCACHE);
	if ( m->pt == NULL ) {
		knl_free_pages(pf, order);
		return E_NOMEM;
	}

	words = ( m->npte + 63 ) / 64;
	m->used = (UD *)Kmalloc(words * sizeof(UD));
	if ( m->used == NULL ) {
		knl_vunmap_pa(m->pt, (UD)1 << order);
		knl_free_pages(pf, order);
		return E_NOMEM;
	}
	knl_memset(m->used, 0, words * sizeof(UD));
	used_set(m, 0, V3D_VA_RESERVED >> V3D_PAGE_SHIFT, TRUE);
	m->hint = V3D_VA_RESERVED >> V3D_PAGE_SHIFT;
	m->nfree = m->npte - m->hint;
	return E_OK;
}

EXPORT void v3d_mmu_fini( T_V3DMMU *m )
{
	if ( m->pt != NULL ) {
		knl_vunmap_pa(m->pt, (UD)1 << m->pt_order);
		knl_free_pages(knl_pa_to_pf(m->pt_pa), m->pt_order);
	}
	if ( m->used != NULL ) {
		Kfree(m->used);
	}
	knl_memset(m, 0, sizeof(T_V3DMMU));
}

EXPORT ER v3d_mmu_alloc( T_V3DMMU *m, UD npages, UD align, UW *p_va )
{
	UD	i, run;
	UINT	imask;
	BOOL	wrapped = FALSE;

	if ( npages == 0 || npages > m->npte || align == 0 || ( align & ( align - 1 ) ) != 0 ) {
		return E_PAR;
	}
	ISpinLock(&m->lock, &imask);
	if ( npages > m->nfree ) {
		ISpinUnlock(&m->lock, &imask);
		return E_NOMEM;
	}
	i = ( m->hint + align - 1 ) & ~( align - 1 );
	for (;;) {
		if ( i + npages > m->npte ) {
			if ( wrapped ) break;
			wrapped = TRUE;
			i = V3D_VA_RESERVED >> V3D_PAGE_SHIFT;
			i = ( i + align - 1 ) & ~( align - 1 );
			continue;
		}
		for ( run = 0; run < npages && !used_test(m, i + run); run++ ) ;
		if ( run == npages ) {
			used_set(m, i, npages, TRUE);
			m->nfree -= npages;
			m->hint = i + npages;
			ISpinUnlock(&m->lock, &imask);
			*p_va = (UW)( i << V3D_PAGE_SHIFT );
			return E_OK;
		}
		i = ( i + run + 1 + align - 1 ) & ~( align - 1 );
	}
	ISpinUnlock(&m->lock, &imask);
	return E_NOMEM;
}

EXPORT void v3d_mmu_free( T_V3DMMU *m, UW va, UD npages )
{
	UD	i = (UD)va >> V3D_PAGE_SHIFT;
	UINT	imask;

	if ( i < ( V3D_VA_RESERVED >> V3D_PAGE_SHIFT ) || i + npages > m->npte ) {
		return;
	}
	ISpinLock(&m->lock, &imask);
	used_set(m, i, npages, FALSE);
	m->nfree += npages;
	if ( i < m->hint ) m->hint = i;
	ISpinUnlock(&m->lock, &imask);
}

EXPORT void v3d_mmu_insert( T_V3DMMU *m, UW va, CONST UD *pa, UD npages, BOOL writable )
{
	UD	i, at = (UD)va >> V3D_PAGE_SHIFT;

	for ( i = 0; i < npages && at + i < m->npte; i++ ) {
		m->pt[at + i] = v3d_mmu_pte(pa[i], writable);
	}
	Asm("dsb st" ::: "memory");
	if ( m->flush != NULL ) {
		m->flush(m->flush_arg);
	}
}

EXPORT void v3d_mmu_remove( T_V3DMMU *m, UW va, UD npages )
{
	UD	i, at = (UD)va >> V3D_PAGE_SHIFT;

	for ( i = 0; i < npages && at + i < m->npte; i++ ) {
		m->pt[at + i] = 0;
	}
	Asm("dsb st" ::: "memory");
	if ( m->flush != NULL ) {
		m->flush(m->flush_arg);
	}
}

EXPORT UW v3d_mmu_lookup( T_V3DMMU *m, UW va )
{
	UD	at = (UD)va >> V3D_PAGE_SHIFT;

	return ( at < m->npte ) ? m->pt[at] : 0;
}
