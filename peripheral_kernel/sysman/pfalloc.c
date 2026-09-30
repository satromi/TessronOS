/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pfalloc.c
 *	Page frame database and buddy allocator (design 6.4)
 *
 *	One PFRAME per 4KB page of RAM, covering [knl_pf_base, knl_pf_base +
 *	knl_pf_npages * 4KB). The database itself is placed right after the
 *	kernel image. Two zones (below/above 4GB), buddy orders 0..PF_MAX_ORDER.
 *	Reserved pages (kernel image, database, DTB, firmware areas) are never
 *	handed out. Protection is interrupt disable until spin locks arrive
 *	with SMP (design 8.6).
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "pfalloc.h"
#include <tk/tkernel.h>

/*
 * Taken with interrupts disabled around the free lists of the page frames: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	pf_lock;

EXPORT PFRAME	*knl_pf_db;
EXPORT UD	knl_pf_base;
EXPORT UD	knl_pf_npages;

typedef struct {
	QUEUE	free[PF_MAX_ORDER + 1];	/* free lists per order */
	UD	nfree;			/* free pages */
	UD	ntotal;			/* pages managed (not reserved) */
} ZONE;

LOCAL ZONE	zones[ZONE_NUM];

#define ZONE_DMA32_LIMIT	(U64(1) << 32)

#define PF_INDEX(pf)		((UD)((pf) - knl_pf_db))
#define PF_OF(idx)		(&knl_pf_db[idx])

EXPORT UD knl_pf_to_pa( CONST PFRAME *pf )
{
	return knl_pf_base + (PF_INDEX(pf) << PAGE_SHIFT);
}

EXPORT PFRAME *knl_pa_to_pf( UD pa )
{
	UD	idx;

	if ( pa < knl_pf_base ) return NULL;
	idx = (pa - knl_pf_base) >> PAGE_SHIFT;
	return ( idx < knl_pf_npages ) ? PF_OF(idx) : NULL;
}

LOCAL INT zone_of_pa( UD pa )
{
	return ( pa < ZONE_DMA32_LIMIT ) ? ZONE_DMA32 : ZONE_NORMAL;
}

/*
 * Put the block starting at pf (order) into its free list.
 */
LOCAL void free_list_add( ZONE *z, PFRAME *pf, UINT order )
{
	pf->flags = PF_FREE;
	pf->order = (UB)order;
	QueInsert(&pf->link, &z->free[order]);
}

LOCAL void free_list_remove( PFRAME *pf )
{
	QueRemove(&pf->link);
	pf->flags = 0;
}

/*
 * Free a block and merge it with its buddy while possible.
 */
LOCAL void free_block( PFRAME *pf, UINT order )
{
	ZONE	*z = &zones[pf->zone];
	UD	idx = PF_INDEX(pf);

	while ( order < PF_MAX_ORDER ) {
		UD	bidx = idx ^ ((UD)1 << order);
		PFRAME	*buddy;

		if ( bidx >= knl_pf_npages ) break;
		buddy = PF_OF(bidx);
		if ( (buddy->flags & PF_FREE) == 0 || buddy->order != order || buddy->zone != pf->zone ) {
			break;
		}
		free_list_remove(buddy);
		if ( bidx < idx ) {
			idx = bidx;
			pf = buddy;
		}
		order++;
	}
	free_list_add(z, pf, order);
}

/*
 * Take a block of 'order' from zone z, splitting larger blocks as needed.
 */
LOCAL PFRAME *alloc_block( ZONE *z, UINT order )
{
	UINT	o;
	PFRAME	*pf;

	for ( o = order; o <= PF_MAX_ORDER; o++ ) {
		if ( !isQueEmpty(&z->free[o]) ) break;
	}
	if ( o > PF_MAX_ORDER ) return NULL;

	pf = (PFRAME *)((UB *)z->free[o].next - offsetof(PFRAME, link));
	free_list_remove(pf);
	while ( o > order ) {
		o--;
		free_list_add(z, PF_OF(PF_INDEX(pf) + ((UD)1 << o)), o);	/* upper half back to the list */
	}
	return pf;
}

EXPORT PFRAME *knl_alloc_pages( UINT order, UINT zone, UINT flags )
{
	PFRAME	*pf = NULL;
	UINT	imask;
	INT	zi;

	if ( order > PF_MAX_ORDER ) return NULL;
	if ( (flags & KAF_DMA32) != 0 ) zone = ZONE_DMA32;

	ISpinLock(&pf_lock, &imask);
	if ( zone == ZONE_NORMAL ) {
		pf = alloc_block(&zones[ZONE_NORMAL], order);
	}
	if ( pf == NULL ) {
		pf = alloc_block(&zones[ZONE_DMA32], order);	/* NORMAL falls back to DMA32 */
	}
	if ( pf != NULL ) {
		zi = pf->zone;
		zones[zi].nfree -= (UD)1 << order;
		pf->flags = PF_KERNEL | PF_HEAD;
		pf->order = (UB)order;
		pf->refcnt = 1;
		pf->owner = 0;
	}
	ISpinUnlock(&pf_lock, &imask);

	if ( pf != NULL && (flags & KAF_ZERO) != 0 ) {
		knl_memset(knl_pf_to_va(pf), 0, (SZ)(PAGE_SIZE << order));
	}
	return pf;
}

EXPORT void knl_free_pages( PFRAME *pf, UINT order )
{
	UINT	imask;

	if ( pf == NULL || order > PF_MAX_ORDER ) return;
	if ( (pf->flags & (PF_FREE | PF_RESERVED)) != 0 ) return;	/* double free or reserved */

	ISpinLock(&pf_lock, &imask);
	zones[pf->zone].nfree += (UD)1 << order;
	free_block(pf, order);
	ISpinUnlock(&pf_lock, &imask);
}

EXPORT UD knl_pf_free_count( INT zone )
{
	if ( zone >= 0 ) return zones[zone].nfree;
	return zones[ZONE_DMA32].nfree + zones[ZONE_NORMAL].nfree;
}

EXPORT UD knl_pf_total_count( INT zone )
{
	if ( zone >= 0 ) return zones[zone].ntotal;
	return zones[ZONE_DMA32].ntotal + zones[ZONE_NORMAL].ntotal;
}

/*
 * Mark [pa, pa + size) reserved (clipped to the database range).
 */
LOCAL void reserve_range( UD pa, UD size )
{
	UD	s = pa & PAGE_MASK;
	UD	e = (pa + size + PAGE_SIZE - 1) & PAGE_MASK;

	for ( ; s < e; s += PAGE_SIZE ) {
		PFRAME	*pf = knl_pa_to_pf(s);
		if ( pf != NULL ) pf->flags = PF_RESERVED;
	}
}

/*
 * Release the pages of a RAM range that are not reserved to the buddy
 * lists, using the largest aligned blocks.
 */
LOCAL void release_range( UD start, UD end )
{
	UD	pa = (start + PAGE_SIZE - 1) & PAGE_MASK;

	end &= PAGE_MASK;
	while ( pa < end ) {
		PFRAME	*pf = knl_pa_to_pf(pa);
		UINT	order;
		UD	idx;

		if ( pf == NULL ) break;
		if ( (pf->flags & PF_RESERVED) != 0 ) {
			pa += PAGE_SIZE;
			continue;
		}
		/* largest order such that the block is aligned, fits and holds no reserved page */
		idx = PF_INDEX(pf);
		for ( order = PF_MAX_ORDER; order > 0; order-- ) {
			UD	n = (UD)1 << order, k;
			if ( (idx & (n - 1)) != 0 || pa + (n << PAGE_SHIFT) > end ) continue;
			if ( zone_of_pa(pa) != zone_of_pa(pa + (n << PAGE_SHIFT) - 1) ) continue;
			for ( k = 0; k < n; k++ ) {
				if ( (PF_OF(idx + k)->flags & PF_RESERVED) != 0 ) break;
			}
			if ( k == n ) break;
		}
		{
			UD	n = (UD)1 << order, k;
			INT	zi = zone_of_pa(pa);
			for ( k = 0; k < n; k++ ) {
				PF_OF(idx + k)->zone = (UB)zi;
			}
			free_list_add(&zones[zi], pf, order);
			zones[zi].nfree += n;
			zones[zi].ntotal += n;
			pa += n << PAGE_SHIFT;
		}
	}
}

/*
 * Initialize the database and the free lists.
 *	*p_kernel_end_pa: physical end of the kernel image on entry; on return
 *	the end of the database (page aligned). The 'extra_reserve' bytes that
 *	follow the database are reserved too (the caller uses them, e.g. for
 *	Imalloc). Everything from the image start to there is reserved, as are
 *	the DTB and the firmware areas.
 */
EXPORT void knl_pfalloc_init( UBINT *p_kernel_end_pa, UD extra_reserve )
{
	UD	lo = ~(UD)0, hi = 0, db_pa, db_size, i;
	INT	z, o;

	for ( z = 0; z < ZONE_NUM; z++ ) {
		for ( o = 0; o <= PF_MAX_ORDER; o++ ) QueInit(&zones[z].free[o]);
		zones[z].nfree = zones[z].ntotal = 0;
	}

	for ( i = 0; i < (UD)knl_mem_range_n; i++ ) {
		UD	s = knl_mem_range[i].start, e = s + knl_mem_range[i].size;
		if ( s < lo ) lo = s;
		if ( e > hi ) hi = e;
	}
	if ( knl_mem_range_n == 0 ) {			/* no DTB: single default range */
		lo = INTERNAL_RAM_START;
		hi = INTERNAL_RAM_END;
	}
	knl_pf_base = lo & PAGE_MASK;
	knl_pf_npages = (hi - knl_pf_base) >> PAGE_SHIFT;

	/* database right after the kernel image */
	db_pa = (*p_kernel_end_pa + PAGE_SIZE - 1) & PAGE_MASK;
	db_size = (knl_pf_npages * sizeof(PFRAME) + PAGE_SIZE - 1) & PAGE_MASK;
	knl_pf_db = (PFRAME *)PA2VA(db_pa);
	knl_memset(knl_pf_db, 0, (SZ)db_size);
	*p_kernel_end_pa = db_pa + db_size;

	/* everything is reserved until a RAM range releases it */
	for ( i = 0; i < knl_pf_npages; i++ ) knl_pf_db[i].flags = PF_RESERVED;
	for ( i = 0; i < (UD)knl_mem_range_n; i++ ) {
		UD	s = knl_mem_range[i].start, e = s + knl_mem_range[i].size, pa;
		for ( pa = (s + PAGE_SIZE - 1) & PAGE_MASK; pa + PAGE_SIZE <= e; pa += PAGE_SIZE ) {
			knl_pa_to_pf(pa)->flags = 0;
		}
	}
	if ( knl_mem_range_n == 0 ) {
		for ( i = 0; i < knl_pf_npages; i++ ) knl_pf_db[i].flags = 0;
	}

	/* reservations: kernel image + database (+ caller's area), DTB, firmware */
	reserve_range(LOAD_PA_VALUE, *p_kernel_end_pa + extra_reserve - LOAD_PA_VALUE);
	if ( knl_dtb_addr != 0 ) reserve_range(knl_dtb_addr, knl_dtb_size);
	/* a screen the firmware left running: its pixels are scanned out from there */
	if ( knl_dtb_fb.size != 0 ) reserve_range(knl_dtb_fb.pa, knl_dtb_fb.size);
#ifdef TFA_BL31_END
	reserve_range(0, TFA_BL31_END);
#endif

	if ( knl_mem_range_n == 0 ) {
		release_range(lo, hi);
	} else {
		for ( i = 0; i < (UD)knl_mem_range_n; i++ ) {
			release_range(knl_mem_range[i].start, knl_mem_range[i].start + knl_mem_range[i].size);
		}
	}
}

/*
 * The system stack of a task (kernel.h). One of protection level 2 or 3
 * is a run of whole pages, as many as its size rounds up to in a power
 * of two; the kernel's own come from Imalloc as they always did.
 */
LOCAL UINT sstk_order( W size )
{
	UINT	order = 0;

	while ( ( (UD)PAGE_SIZE << order ) < (UD)size && order < PF_MAX_ORDER ) {
		order++;
	}
	return order;
}

LOCAL BOOL sstk_paged( W size, ATR tskatr )
{
	return (BOOL)( ( tskatr & TA_RNG3 ) >= TA_RNG2 && knl_pf_db != NULL
		    && (UD)size <= ( (UD)PAGE_SIZE << PF_MAX_ORDER ) );
}

EXPORT void *knl_sstk_alloc( W size, ATR tskatr )
{
	PFRAME	*pf;

	if ( !sstk_paged(size, tskatr) ) {
		return knl_Imalloc((UW)size);
	}
	pf = knl_alloc_pages(sstk_order(size), ZONE_NORMAL, 0);

	return ( pf != NULL ) ? knl_pf_to_va(pf) : NULL;
}

EXPORT void knl_sstk_free( void *stack, W size, ATR tskatr )
{
	if ( stack == NULL ) {
		return;
	}
	if ( !sstk_paged(size, tskatr) ) {
		knl_Ifree(stack);
		return;
	}
	knl_free_pages(knl_pa_to_pf(VA2PA(stack)), sstk_order(size));
}
