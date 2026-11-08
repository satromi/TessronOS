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
 *	Each allocation is followed by one unmapped guard page. A task's
 *	stack (knl_vmap_stack) has TASK_STK_GUARD_PAGES more below it, the
 *	side it grows towards.
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

EXPORT void	(*knl_vunmap_hook)( UBINT va, UD npages ) = NULL;

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
 * The pages of [va, va + n pages) unmapped and given back; the addresses
 * are left reserved for the caller to give back
 */
LOCAL void vmap_drop_pages( UBINT va, UD n )
{
	UBINT	v = va;
	UD	i;

	if ( n == 0 ) return;
	if ( knl_vunmap_hook != NULL ) {
		knl_vunmap_hook(va, n);
	}
	for ( i = 0; i < n; i++, v += PAGE_SIZE ) {
		UD	pa = knl_pt_lookup(knl_pt_l0_k_pa, v);
		if ( pa != ~(UD)0 ) {
			knl_pt_unmap(knl_pt_l0_k_pa, v, PAGE_SIZE);
			knl_free_pages(knl_pa_to_pf(pa), 0);
		}
	}
}

/*
 * An allocation that failed, said for the first few: callers take NULL
 * quietly, and what then goes missing (a screen drawn without its
 * wallpaper, a window not opened) says nothing of why. The area's pages
 * in use and its longest free run are counted then.
 */
#define VMAP_TELL_MAX		8
LOCAL INT	vmap_told = 0;

LOCAL void vmap_failed( UD npages, CONST char *why )
{
	UD	i, used = 0, run = 0, longest = 0;
	UINT	imask;

	if ( vmap_told >= VMAP_TELL_MAX ) return;
	vmap_told++;
	ISpinLock(&vmap_lock, &imask);
	for ( i = 0; i < VMAP_PAGES; i++ ) {
		if ( bit_test(i) ) {
			used++;
			run = 0;
		} else if ( ++run > longest ) {
			longest = run;
		}
	}
	ISpinUnlock(&vmap_lock, &imask);
	tm_printf((UB *)"vmap: %ld pages not had (%s); the area: %ld of %ld pages in use, the longest free run %ld\n",
		  (D)npages, why, (D)used, (D)VMAP_PAGES, (D)longest);
}

/*
 * Pages taken and mapped at [va, va + npages pages). On failure the ones
 * mapped so far are given back and what went wrong is returned, and the
 * addresses are left for the caller to give back, all of them at once:
 * given back in two parts, another processor could take the first part
 * before the rest followed it.
 */
LOCAL CONST char *vmap_fill( UBINT va, UD npages, UD attr, UINT aflags )
{
	UBINT	v;
	UD	i;

	for ( i = 0, v = va; i < npages; i++, v += PAGE_SIZE ) {
		PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, aflags);
		if ( pf == NULL || knl_pt_map(knl_pt_l0_k_pa, v, knl_pf_to_pa(pf), PAGE_SIZE, attr) != E_OK ) {
			if ( pf != NULL ) knl_free_pages(pf, 0);
			vmap_drop_pages(va, i);
			return ( pf == NULL ) ? "no free page" : "the page table";
		}
	}
	return NULL;
}

/*
 * Allocate npages and map them contiguously. NULL on failure.
 */
EXPORT void *knl_vmap( UD npages, UINT flags )
{
	UBINT	va;
	UD	attr = ( (flags & VMAP_NOCACHE) != 0 ) ? PTE_PAGE_KDATA_NC : PTE_PAGE_KDATA;
	UINT	aflags = ( (flags & VMAP_DMA32) != 0 ) ? KAF_DMA32 : 0;
	UINT	imask;
	CONST char *why;

	if ( npages == 0 || npages >= VMAP_PAGES ) return NULL;

	ISpinLock(&vmap_lock, &imask);
	va = vmap_alloc_va(npages);
	ISpinUnlock(&vmap_lock, &imask);
	if ( va == 0 ) {
		vmap_failed(npages, "no run of addresses that long");
		return NULL;
	}

	why = vmap_fill(va, npages, attr, aflags);
	if ( why != NULL ) {
		ISpinLock(&vmap_lock, &imask);
		vmap_free_va(va, npages);
		ISpinUnlock(&vmap_lock, &imask);
		vmap_failed(npages, why);
		return NULL;
	}
	return (void *)va;
}

/*
 * The stack of a task: npages mapped, with TASK_STK_GUARD_PAGES below
 * them that stay unmapped. A stack that runs out then faults at the
 * instruction that went too far, instead of writing over its neighbour.
 * The guard is wider than one page because a frame need not touch its
 * first page: the kernel's frames are kept to 4KB, those of the outline
 * rasterizers reach 25KB.
 */
EXPORT void *knl_vmap_stack( UD npages )
{
	UD	need = npages + TASK_STK_GUARD_PAGES;
	UBINT	va;
	UINT	imask;
	CONST char *why;

	if ( npages == 0 || need >= VMAP_PAGES ) return NULL;

	ISpinLock(&vmap_lock, &imask);
	va = vmap_alloc_va(need);
	ISpinUnlock(&vmap_lock, &imask);
	if ( va == 0 ) {
		vmap_failed(need, "no run of addresses that long");
		return NULL;
	}

	why = vmap_fill(va + ( (UBINT)TASK_STK_GUARD_PAGES << PAGE_SHIFT ), npages, PTE_PAGE_KDATA, 0);
	if ( why != NULL ) {
		ISpinLock(&vmap_lock, &imask);
		vmap_free_va(va, need);
		ISpinUnlock(&vmap_lock, &imask);
		vmap_failed(need, why);
		return NULL;
	}
	return (void *)( va + ( (UBINT)TASK_STK_GUARD_PAGES << PAGE_SHIFT ) );
}

EXPORT void knl_vunmap_stack( void *stack, UD npages )
{
	UBINT	va = (UBINT)stack - ( (UBINT)TASK_STK_GUARD_PAGES << PAGE_SHIFT );
	UINT	imask;

	vmap_drop_pages((UBINT)stack, npages);
	ISpinLock(&vmap_lock, &imask);
	vmap_free_va(va, npages + TASK_STK_GUARD_PAGES);
	ISpinUnlock(&vmap_lock, &imask);
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
	if ( knl_vunmap_hook != NULL ) {
		knl_vunmap_hook(v, npages);
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

	if ( knl_vunmap_hook != NULL ) {
		knl_vunmap_hook(v, npages);
	}
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
