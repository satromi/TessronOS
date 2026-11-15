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
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include <ts/proc.h>

/*
 * Taken with interrupts disabled around the free lists of the page frames: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	pf_lock;

EXPORT PFRAME	*knl_pf_db;
EXPORT UD	knl_pf_base;
EXPORT UD	knl_pf_npages;
EXPORT UD	knl_fb_page2 = 0;	/* a second screen page kept from the start, 0: none */

typedef struct {
	QUEUE	free[PF_MAX_ORDER + 1];	/* free lists per order */
	UD	nfree;			/* free pages */
	UD	ntotal;			/* pages managed (not reserved) */
} ZONE;

LOCAL ZONE	zones[ZONE_NUM];

/*
 * Where the zone that bus masters reach ends: 4GB, or lower on a board
 * whose DMA sees less (the Raspberry Pi 4: 1GB, sysdef.h)
 */
#ifdef DMA_ZONE_LIMIT
#define ZONE_DMA32_LIMIT	((UD)DMA_ZONE_LIMIT)
#else
#define ZONE_DMA32_LIMIT	(U64(1) << 32)
#endif

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

#ifdef PF_FREE_CHECK
/*
 * Pages written after they were given back (make PGCHECK=1). Each page
 * given back is filled with a pattern of its own frame number, and the
 * address the free was called from is kept for it; a page handed out
 * again is first compared with its pattern. A difference is a write
 * into a page that was free -- through a mapping or a translation left
 * behind, by a device, or by code that still held it -- and a page given
 * back while it is free is a second free, which is kept out of the
 * lists. The first ones are kept and said by knl_pf_check_tell, outside
 * the lock: for a page written, with the process and address it was
 * given up from (knl_pf_check_owner), how long it had been free, and
 * who took it. What it costs is a write of each page given back and a
 * read of each page handed out.
 */
#define PFC_BLK_ENT	( ( (UD)PAGE_SIZE << PF_MAX_ORDER ) / sizeof(UD) )
#define PFC_BLKS	16
#define PFC_PAT(pfn)	( 0xF1EEF1EE00000000ULL | ( (UD)(pfn) & 0xFFFFFFFFULL ) )
#define PFC_REC		24		/* pages written while free said one by one */

/*
 * For each page watched, three words: where it was freed from (0 while
 * it is in use), whose it was (the process and its address, when a
 * process gave it up: knl_pf_check_owner), and when it was freed
 */
LOCAL UD	*pfc_blk[PFC_BLKS], *pfc_own[PFC_BLKS], *pfc_tim[PFC_BLKS];
LOCAL UD	pfc_npages = 0;		/* pages watched, from frame 0 */
LOCAL UW	pfc_written = 0, pfc_twice = 0, pfc_told_w = 0, pfc_told_t = 0;
LOCAL struct {
	UD	pa, off, nbad, freer, taker, own, age_us, taker_pid, cpu;
	UD	word[8];
}		pfc_w[PFC_REC];
LOCAL struct {
	UD	pa, freer, again;
}		pfc_t1;

IMPORT UD knl_get_mono_ns( void );

/* The owner word: the process (16 bits), the processor that freed it (8), the page's address in the process */
#define PFC_OWN(pid, cpu, va)	( ( (UD)(pid) << 48 ) | ( ( (UD)(cpu) & 0xFF ) << 40 ) \
				| ( ( (UD)(va) >> PAGE_SHIFT ) & 0xFFFFFFFFFFULL ) )
#define PFC_OWN_PID(o)		( (UD)(o) >> 48 )
#define PFC_OWN_VA(o)		( ( (UD)(o) & 0xFFFFFFFFFFULL ) << PAGE_SHIFT )

LOCAL UD *pfc_ent( UD idx )
{
	return &pfc_blk[idx / PFC_BLK_ENT][idx % PFC_BLK_ENT];
}

LOCAL UD *pfc_owner( UD idx )
{
	return &pfc_own[idx / PFC_BLK_ENT][idx % PFC_BLK_ENT];
}

LOCAL UD *pfc_time( UD idx )
{
	return &pfc_tim[idx / PFC_BLK_ENT][idx % PFC_BLK_ENT];
}

/* The processor running the caller (0 when it cannot be told) */
LOCAL UD pfc_cpu( void )
{
	return (UD)knl_pcpu()->prcid;
}

/* The pages of a block given back filled with their pattern, when they are watched */
LOCAL void pfc_fill( PFRAME *pf, UINT order )
{
	UD	idx = PF_INDEX(pf), n = (UD)1 << order, k, i;

	if ( idx + n > __atomic_load_n(&pfc_npages, __ATOMIC_ACQUIRE) ) return;
	for ( k = 0; k < n; k++ ) {
		UD	*w = (UD *)PA2VA(knl_pf_base + ( ( idx + k ) << PAGE_SHIFT ));
		UD	pat = PFC_PAT(idx + k);

		for ( i = 0; i < PAGE_SIZE / sizeof(UD); i++ ) w[i] = pat;
	}
}

/* Under pf_lock: the block noted free, or TRUE when a page of it is free already */
LOCAL BOOL pfc_mark( PFRAME *pf, UINT order, UD freer )
{
	UD	idx = PF_INDEX(pf), n = (UD)1 << order, k;

	if ( idx + n > pfc_npages ) return FALSE;
	for ( k = 0; k < n; k++ ) {
		UD	was = *pfc_ent(idx + k);

		if ( was != 0 ) {
			if ( pfc_twice++ == 0 ) {
				pfc_t1.pa = knl_pf_base + ( ( idx + k ) << PAGE_SHIFT );
				pfc_t1.freer = was;
				pfc_t1.again = freer;
			}
			return TRUE;
		}
	}
	{
		UD	now = knl_get_mono_ns();

		for ( k = 0; k < n; k++ ) {
			*pfc_ent(idx + k) = freer;
			*pfc_time(idx + k) = now;
		}
	}
	return FALSE;
}

/* The pages of a block just handed out compared with their pattern, and noted in use */
LOCAL void pfc_check( PFRAME *pf, UINT order, UD taker )
{
	UD	idx = PF_INDEX(pf), n = (UD)1 << order, k, i;
	UINT	imask;

	if ( idx + n > __atomic_load_n(&pfc_npages, __ATOMIC_ACQUIRE) ) return;
	for ( k = 0; k < n; k++ ) {
		UD	*e = pfc_ent(idx + k), freer = *e, pat = PFC_PAT(idx + k), bad = 0, first = 0;
		UD	*w = (UD *)PA2VA(knl_pf_base + ( ( idx + k ) << PAGE_SHIFT ));
		UD	own = *pfc_owner(idx + k), at = *pfc_time(idx + k);

		*pfc_owner(idx + k) = 0;
		if ( freer == 0 ) continue;
		__atomic_store_n(e, 0, __ATOMIC_RELAXED);
		for ( i = 0; i < PAGE_SIZE / sizeof(UD); i++ ) {
			if ( w[i] != pat && bad++ == 0 ) first = i;
		}
		if ( bad == 0 ) continue;
		ISpinLock(&pf_lock, &imask);
		if ( pfc_written < PFC_REC ) {
			UD	r = pfc_written;

			pfc_w[r].off = first * sizeof(UD);
			if ( first > PAGE_SIZE / sizeof(UD) - 8 ) first = PAGE_SIZE / sizeof(UD) - 8;
			pfc_w[r].pa = knl_pf_base + ( ( idx + k ) << PAGE_SHIFT );
			pfc_w[r].nbad = bad;
			pfc_w[r].freer = freer;
			pfc_w[r].taker = taker;
			pfc_w[r].own = own;
			pfc_w[r].age_us = ( knl_get_mono_ns() - at ) / 1000;
			pfc_w[r].taker_pid = (UD)ts_get_pid();
			pfc_w[r].cpu = pfc_cpu();
			for ( i = 0; i < 8; i++ ) pfc_w[r].word[i] = w[first + i];
		}
		pfc_written++;
		ISpinUnlock(&pf_lock, &imask);
	}
}

/* The table of the pages watched, made once the allocator is up and every page is in the linear map */
LOCAL void pfc_init( void )
{
	UD	need = knl_pf_npages, have = 0;
	INT	i;

	for ( i = 0; i < PFC_BLKS && have < need; i++ ) {
		PFRAME	*pf = knl_alloc_pages(PF_MAX_ORDER, ZONE_NORMAL, KAF_ZERO);
		PFRAME	*po = knl_alloc_pages(PF_MAX_ORDER, ZONE_NORMAL, KAF_ZERO);
		PFRAME	*pt = knl_alloc_pages(PF_MAX_ORDER, ZONE_NORMAL, KAF_ZERO);

		if ( pf == NULL || po == NULL || pt == NULL ) {
			if ( pf != NULL ) knl_free_pages(pf, PF_MAX_ORDER);
			if ( po != NULL ) knl_free_pages(po, PF_MAX_ORDER);
			if ( pt != NULL ) knl_free_pages(pt, PF_MAX_ORDER);
			break;
		}
		pfc_blk[i] = (UD *)knl_pf_to_va(pf);
		pfc_own[i] = (UD *)knl_pf_to_va(po);
		pfc_tim[i] = (UD *)knl_pf_to_va(pt);
		have += PFC_BLK_ENT;
	}
	if ( have > need ) have = need;
	__atomic_store_n(&pfc_npages, have, __ATOMIC_RELEASE);
	tm_printf((UB *)"pgcheck: on, %ld of %ld pages watched\n", (long)have, (long)need);
}
#endif

/*
 * A page a process gives up (umem.c): whose it was and where, said with
 * it should it be written after it is freed. Its frame is the process's
 * until then, so the word is the caller's alone.
 */
EXPORT void knl_pf_check_owner( UD pa, UBINT va )
{
#ifdef PF_FREE_CHECK
	UD	idx;

	if ( pa < knl_pf_base ) return;
	idx = ( pa - knl_pf_base ) >> PAGE_SHIFT;
	if ( idx >= __atomic_load_n(&pfc_npages, __ATOMIC_ACQUIRE) ) return;
	*pfc_owner(idx) = PFC_OWN(ts_get_pid(), pfc_cpu(), va);
#else
	(void)pa;
	(void)va;
#endif
}

/* Pages written while free and pages given back twice, so far (the tests); FALSE when not watched */
EXPORT BOOL knl_pf_check_counts( UW *p_written, UW *p_twice )
{
#ifdef PF_FREE_CHECK
	if ( p_written != NULL ) *p_written = __atomic_load_n(&pfc_written, __ATOMIC_RELAXED);
	if ( p_twice != NULL ) *p_twice = __atomic_load_n(&pfc_twice, __ATOMIC_RELAXED);
	return (BOOL)( __atomic_load_n(&pfc_npages, __ATOMIC_ACQUIRE) != 0 );
#else
	if ( p_written != NULL ) *p_written = 0;
	if ( p_twice != NULL ) *p_twice = 0;
	return FALSE;
#endif
}

/*
 * What PF_FREE_CHECK found since it was last said; the first call makes
 * its table. Called by the process reaper (proc.c), as processes end.
 */
EXPORT void knl_pf_check_tell( void )
{
#ifdef PF_FREE_CHECK
	static BOOL	up = FALSE;
	UW		w, t;
	UINT		imask;

	if ( !up ) {
		up = TRUE;
		pfc_init();
		return;
	}
	ISpinLock(&pf_lock, &imask);
	w = pfc_written;
	t = pfc_twice;
	ISpinUnlock(&pf_lock, &imask);
	if ( w != pfc_told_w ) {
		UW	r;

		tm_printf((UB *)"pgcheck: %d pages written while free (%d said before)\n", (int)w, (int)pfc_told_w);
		for ( r = pfc_told_w; r < w && r < PFC_REC; r++ ) {
			tm_printf((UB *)"pgcheck:   %lx offset %lx, %ld words; freed from %lx %ld us before by pid %ld "
				  "(its %lx, processor %ld); taken by %lx for pid %ld on processor %ld:\n",
				  pfc_w[r].pa, pfc_w[r].off, (long)pfc_w[r].nbad, pfc_w[r].freer, (long)pfc_w[r].age_us,
				  (long)PFC_OWN_PID(pfc_w[r].own), PFC_OWN_VA(pfc_w[r].own),
				  (long)( ( pfc_w[r].own >> 40 ) & 0xFF ), pfc_w[r].taker, (long)pfc_w[r].taker_pid,
				  (long)pfc_w[r].cpu);
			tm_printf((UB *)"pgcheck:     %016lx %016lx %016lx %016lx %016lx %016lx %016lx %016lx\n",
				  pfc_w[r].word[0], pfc_w[r].word[1], pfc_w[r].word[2], pfc_w[r].word[3],
				  pfc_w[r].word[4], pfc_w[r].word[5], pfc_w[r].word[6], pfc_w[r].word[7]);
		}
		pfc_told_w = w;
	}
	if ( t != pfc_told_t ) {
		pfc_told_t = t;
		tm_printf((UB *)"pgcheck: %d pages given back while free; the first at %lx, freed from %lx, again from %lx\n",
			  (int)t, pfc_t1.pa, pfc_t1.freer, pfc_t1.again);
	}
#endif
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

#ifdef PF_FREE_CHECK
	if ( pf != NULL ) pfc_check(pf, order, (UD)(UBINT)__builtin_return_address(0));
#endif
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

#ifdef PF_FREE_CHECK
	pfc_fill(pf, order);
	ISpinLock(&pf_lock, &imask);
	if ( pfc_mark(pf, order, (UD)(UBINT)__builtin_return_address(0)) ) {
		ISpinUnlock(&pf_lock, &imask);
		return;			/* free already: kept out of the lists */
	}
#else
	ISpinLock(&pf_lock, &imask);
#endif
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
#if defined(RPI5) && CNF_DISP_FLIP
	/*
	 * A second page for the screen, which the display's scaler is pointed
	 * at in turn with the first (disp_rpi5.c): as large as the firmware's,
	 * below 4GB, and contiguous, more than the allocator hands out in one
	 * block, so taken now from the start of the first RAM range past it
	 */
	if ( knl_dtb_fb.size != 0 ) {
		UD	best = 0;

		for ( i = 0; i < (UD)knl_mem_range_n; i++ ) {
			UD	s = ( knl_mem_range[i].start + PAGE_SIZE - 1 ) & PAGE_MASK;

			if ( s >= knl_dtb_fb.pa + knl_dtb_fb.size && s + knl_dtb_fb.size <= 0x100000000ULL
			  && knl_mem_range[i].start + knl_mem_range[i].size >= s + knl_dtb_fb.size
			  && ( best == 0 || s < best ) ) {
				best = s;
			}
		}
		if ( best != 0 ) {
			reserve_range(best, knl_dtb_fb.size);
			knl_fb_page2 = best;
		}
	}
#endif
#ifdef TFA_BL31_END
	reserve_range(0, TFA_BL31_END);
#endif
#ifdef LOW_RESERVED_END
	reserve_range(0, LOW_RESERVED_END);
#endif
	/* what the device tree's header says the firmware keeps (/memreserve/) */
	for ( i = 0; i < (UD)knl_mem_rsv_n; i++ ) {
		reserve_range(knl_mem_rsv[i].start, knl_mem_rsv[i].size);
	}

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
 * of two. One of the kernel's own is mapped in the variable area with
 * unmapped pages below it (knl_vmap_stack), so that running out of it
 * faults at once; only the tasks made before the page allocator is up
 * have theirs from Imalloc.
 *
 * Such a stack is not physically contiguous and has no address in the
 * linear map: nothing on it can be handed to a device by subtraction
 * (ConvPhysicalAddress finds where it lies).
 *
 * A stack cannot be unmapped where it is given back: a task that ends
 * has it given back by the dispatcher, with interrupts off and the
 * kernel's lock held, and taking pages out costs a translation drop on
 * every processor for each. Stacks given back wait on a list, kept
 * mapped; the next task of the same size takes one as it is, and those
 * over SSTK_KEEP go when a task is next made or deleted.
 */
#define SSTK_KEEP	16

typedef struct sstk_dead {
	struct sstk_dead *next;
	UD	npages;
} SSTK_DEAD;

LOCAL T_SPLOCK	sstk_lock;
LOCAL SSTK_DEAD	*sstk_dead_list = NULL;
LOCAL INT	sstk_dead_n = 0;

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

LOCAL BOOL sstk_mapped( ATR tskatr )
{
	return (BOOL)( ( tskatr & TA_RNG3 ) < TA_RNG2 && knl_pf_db != NULL && !in_indp() );
}

/*
 * Whether a stack given back is one of the mapped ones. Imalloc also
 * hands out addresses of the variable area, for a large block, but never
 * one on a page's boundary (its head comes first).
 */
LOCAL BOOL sstk_is_mapped( CONST void *stack )
{
	UBINT	v = (UBINT)stack;

	return (BOOL)( v >= (UBINT)VMAP_VA_BASE && v < (UBINT)VMAP_VA_BASE + CNF_VMAP_SIZE
		    && ( v & ( PAGE_SIZE - 1 ) ) == 0 );
}

/*
 * The size a stack really has: a mapped one fills its pages, so that its
 * foot is right above the unmapped ones.
 */
EXPORT W knl_sstk_round( W size, ATR tskatr )
{
	if ( sstk_mapped(tskatr) ) {
		return (W)( ( (UD)size + PAGE_SIZE - 1 ) & PAGE_MASK );
	}
	return size;
}

/* The stacks over the number kept (all of them for keep 0) unmapped and given back */
LOCAL void sstk_let_go( INT keep )
{
	for ( ;; ) {
		SSTK_DEAD	*d = NULL;
		UINT		imask;

		ISpinLock(&sstk_lock, &imask);
		if ( sstk_dead_n > keep ) {
			d = sstk_dead_list;
			sstk_dead_list = d->next;
			sstk_dead_n--;
		}
		ISpinUnlock(&sstk_lock, &imask);
		if ( d == NULL ) {
			return;
		}
		knl_vunmap_stack(d, d->npages);
	}
}

EXPORT void knl_sstk_trim( void )
{
	sstk_let_go(SSTK_KEEP);
}

LOCAL void *sstk_map( W size )
{
	UD		npages = (UD)size >> PAGE_SHIFT;
	SSTK_DEAD	*d, **pp;
	void		*stack = NULL;
	UINT		imask;

	ISpinLock(&sstk_lock, &imask);
	for ( pp = &sstk_dead_list; ( d = *pp ) != NULL; pp = &d->next ) {
		if ( d->npages == npages ) {
			*pp = d->next;
			sstk_dead_n--;
			stack = d;
			break;
		}
	}
	ISpinUnlock(&sstk_lock, &imask);
	knl_sstk_trim();
	if ( stack == NULL ) {
		stack = knl_vmap_stack(npages);
	}
	if ( stack == NULL ) {
		sstk_let_go(0);		/* the pages the stacks waiting hold */
		stack = knl_vmap_stack(npages);
	}
	return stack;
}

#if USE_KTEST
/* For the tests: whether a mapped stack has been given back and waits on the list */
EXPORT BOOL knl_sstk_waiting( CONST void *stack )
{
	SSTK_DEAD	*d;
	BOOL		found = FALSE;
	UINT		imask;

	ISpinLock(&sstk_lock, &imask);
	for ( d = sstk_dead_list; d != NULL && !found; d = d->next ) {
		found = (BOOL)( (CONST void *)d == stack );
	}
	ISpinUnlock(&sstk_lock, &imask);
	return found;
}
#endif

EXPORT void *knl_sstk_alloc( W size, ATR tskatr )
{
	PFRAME	*pf;

	if ( sstk_mapped(tskatr) ) {
		return sstk_map(size);
	}
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
	if ( sstk_is_mapped(stack) ) {
		SSTK_DEAD	*d = (SSTK_DEAD *)stack;
		UINT		imask;

		d->npages = (UD)size >> PAGE_SHIFT;
		ISpinLock(&sstk_lock, &imask);
		d->next = sstk_dead_list;
		sstk_dead_list = d;
		sstk_dead_n++;
		ISpinUnlock(&sstk_lock, &imask);
		return;
	}
	if ( !sstk_paged(size, tskatr) ) {
		knl_Ifree(stack);
		return;
	}
	knl_free_pages(knl_pa_to_pf(VA2PA(stack)), sstk_order(size));
}
