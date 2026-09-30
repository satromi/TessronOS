/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vmap.c
 *	Kernel variable area (design 6.5): non-contiguous physical pages
 *	mapped at a contiguous virtual address in [VMAP_VA_BASE, +CNF_VMAP_SIZE).
 *	Each allocation is followed by one unmapped guard page.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "pfalloc.h"
#include <tk/smem.h>

/*
 * Taken with interrupts disabled around the addresses of the vmap window: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	vmap_lock;

#define VMAP_PAGES		(CNF_VMAP_SIZE / PAGE_SIZE)
#define VMAP_WORDS		(VMAP_PAGES / 64)

LOCAL UD	vmap_bitmap[VMAP_WORDS];	/* 1 = in use */
LOCAL UD	vmap_hint;			/* first page that may be free */

IMPORT UBINT	knl_pt_l0_k_pa;

LOCAL BOOL bit_test( UD i )
{
	return (vmap_bitmap[i >> 6] >> (i & 63)) & 1;
}
LOCAL void bit_set_range( UD i, UD n )
{
	for ( ; n > 0; n--, i++ ) vmap_bitmap[i >> 6] |= (UD)1 << (i & 63);
}
LOCAL void bit_clr_range( UD i, UD n )
{
	for ( ; n > 0; n--, i++ ) vmap_bitmap[i >> 6] &= ~((UD)1 << (i & 63));
}

/*
 * Reserve npages + 1 guard page of virtual space (first fit)
 */
LOCAL UBINT vmap_alloc_va( UD npages )
{
	UD	need = npages + 1, i, run = 0, start = 0;

	for ( i = vmap_hint; i < VMAP_PAGES; i++ ) {
		if ( bit_test(i) ) {
			run = 0;
			continue;
		}
		if ( run == 0 ) start = i;
		if ( ++run == need ) {
			bit_set_range(start, need);
			vmap_hint = start + need;
			return VMAP_VA_BASE + (start << PAGE_SHIFT);
		}
	}
	return 0;
}

LOCAL void vmap_free_va( UBINT va, UD npages )
{
	UD	start = (va - VMAP_VA_BASE) >> PAGE_SHIFT;

	bit_clr_range(start, npages + 1);
	if ( start < vmap_hint ) vmap_hint = start;
}

/*
 * Allocate npages and map them contiguously. NULL on failure.
 */
EXPORT void *knl_vmap( UD npages, UINT flags )
{
	UBINT	va, v;
	UD	attr = ( (flags & VMAP_NOCACHE) != 0 ) ? PTE_PAGE_KDATA_NC : PTE_PAGE_KDATA;
	UINT	aflags = ( (flags & VMAP_DMA32) != 0 ) ? KAF_DMA32 : 0;
	UINT	imask;
	UD	i;

	if ( npages == 0 || npages >= VMAP_PAGES ) return NULL;

	ISpinLock(&vmap_lock, &imask);
	va = vmap_alloc_va(npages);
	ISpinUnlock(&vmap_lock, &imask);
	if ( va == 0 ) return NULL;

	for ( i = 0, v = va; i < npages; i++, v += PAGE_SIZE ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, aflags);
		if ( pf == NULL || knl_pt_map(knl_pt_l0_k_pa, v, knl_pf_to_pa(pf), PAGE_SIZE, attr) != E_OK ) {
			if ( pf != NULL ) knl_free_pages(pf, 0);
			knl_vunmap((void *)va, i);
			ISpinLock(&vmap_lock, &imask);
			vmap_free_va(va, npages);	/* knl_vunmap freed only i pages of VA */
			ISpinUnlock(&vmap_lock, &imask);
			return NULL;
		}
	}
	return (void *)va;
}

/*
 * Map a range of memory that already exists, rather than taking pages
 * from the allocator. A framebuffer is the case this is for: the pixels
 * live where the display controller put them, and what is needed is a
 * way to reach them with the right attributes.
 *
 * With VMAP_NOCACHE the mapping is Normal Non-cacheable, which lets
 * writes merge and reorder on their way out. That is what a framebuffer
 * wants; the device window's Device attribute forbids both, so a write
 * of one word goes to the bus as one word.
 */
EXPORT void *knl_vmap_pa( UD pa, UD npages, UINT flags )
{
	UBINT	va, v;
	UD	attr = ( (flags & VMAP_NOCACHE) != 0 ) ? PTE_PAGE_KDATA_NC
						      : PTE_PAGE_KDATA;
	UINT	imask;
	UD	i;

	if ( npages == 0 || npages >= VMAP_PAGES ) {
		return NULL;
	}
	if ( (pa & (PAGE_SIZE - 1)) != 0 ) {
		return NULL;			/* it has to start on a page */
	}

	ISpinLock(&vmap_lock, &imask);
	va = vmap_alloc_va(npages);
	ISpinUnlock(&vmap_lock, &imask);
	if ( va == 0 ) {
		return NULL;
	}
	for ( i = 0, v = va; i < npages; i++, v += PAGE_SIZE ) {
		if ( knl_pt_map(knl_pt_l0_k_pa, v, pa + i * PAGE_SIZE,
				PAGE_SIZE, attr) != E_OK ) {
			knl_vunmap_pa((void *)va, i);
			ISpinLock(&vmap_lock, &imask);
			vmap_free_va(va, npages);
			ISpinUnlock(&vmap_lock, &imask);
			return NULL;
		}
	}

	return (void *)va;
}

/*
 * Take such a mapping away again. The memory itself belongs to whoever
 * it belonged to before, so nothing is given back to the allocator.
 */
EXPORT void knl_vunmap_pa( void *va, UD npages )
{
	UBINT	v = (UBINT)va;
	UINT	imask;
	UD	i;

	if ( v < VMAP_VA_BASE || v >= VMAP_VA_BASE + CNF_VMAP_SIZE ) {
		return;
	}
	for ( i = 0; i < npages; i++, v += PAGE_SIZE ) {
		knl_pt_unmap(knl_pt_l0_k_pa, v, PAGE_SIZE);
	}
	ISpinLock(&vmap_lock, &imask);
	vmap_free_va((UBINT)va, npages);
	ISpinUnlock(&vmap_lock, &imask);
}

/*
 * Unmap and free the pages of a knl_vmap() area
 */
EXPORT void knl_vunmap( void *va, UD npages )
{
	UBINT	v = (UBINT)va;
	UINT	imask;
	UD	i;

	if ( v < VMAP_VA_BASE || v >= VMAP_VA_BASE + CNF_VMAP_SIZE ) return;

	for ( i = 0; i < npages; i++, v += PAGE_SIZE ) {
		UD	pa = knl_pt_lookup(knl_pt_l0_k_pa, v);
		if ( pa != ~(UD)0 ) {
			knl_pt_unmap(knl_pt_l0_k_pa, v, PAGE_SIZE);
			knl_free_pages(knl_pa_to_pf(pa), 0);
		}
	}
	ISpinLock(&vmap_lock, &imask);
	vmap_free_va((UBINT)va, npages);
	ISpinUnlock(&vmap_lock, &imask);
}
