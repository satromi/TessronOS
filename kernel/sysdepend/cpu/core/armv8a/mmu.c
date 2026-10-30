/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mmu.c (ARMv8-A / AArch64)
 *	Page table management (4KB granule, 4 levels, design 6.3).
 *	  - linear map of RAM and the device window (initial tables from boot.S)
 *	  - generic 4KB page mapping for any translation table (kernel vmap,
 *	    process spaces): tables are allocated from the page frame allocator
 *	  - MapMemory / UnmapMemory / ConvPhysicalAddress
 */

#include <sys/machine.h>

#ifdef CPU_CORE_ARMV8A

#include "kernel.h"
#include "../../../sysdepend.h"
#include "sysman/pfalloc.h"
#include <tk/smem.h>

IMPORT UBINT	knl_pt_l0_k_pa;		/* TTBR1 L0 table (boot.S) */
IMPORT UBINT	knl_pt_l1_ram_pa;	/* L1 table of the linear map (boot.S) */

#define PT_IDX(va, shift)	(((UD)(va) >> (shift)) & (PT_ENTRIES - 1))
#define PTE_IS_TABLE(e)		(((e) & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE))
#define PTE_PA(e)		((e) & PTE_ADDR_MASK)

LOCAL void tlb_flush_all( void )
{
	Asm("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

EXPORT void knl_tlb_flush_va( UBINT va )
{
	Asm("dsb ishst; tlbi vaale1is, %0; dsb ish; isb" :: "r"(va >> PAGE_SHIFT) : "memory");
}

/*
 * Instructions written as data in [va, va + size) made visible to the
 * instruction fetch of every processor of the inner shareable domain:
 * the data cache lines cleaned to the point of unification, then the
 * instruction cache lines of the range invalidated. 'va' may be any
 * mapping of the memory (the linear map). CTR_EL0 says what is needed:
 * IDC and DIC drop the clean and the invalidation, and the clean is not
 * needed either when CLIDR_EL1 puts the point of unification before
 * every cache level (LoUIS and LoUU 0) or names no level to the point
 * of coherence (LoC 0) -- the Cortex-A76 has IDC clear and LoUU 0; an instruction cache
 * that is not physically indexed and tagged may hold the lines under
 * another address than the one given, and is invalidated whole. Both
 * kinds of maintenance by address reach the caches of all processors;
 * a processor that may have fetched the old instructions still needs a
 * context synchronization event of its own (knl_sync_cores).
 */
#define CTR_IDC			( 1UL << 28 )
#define CTR_DIC			( 1UL << 29 )
#define CTR_L1IP(c)		( ( (c) >> 14 ) & 3 )
#define CTR_L1IP_PIPT		3
#define CLIDR_LOUIS(c)		( ( (c) >> 21 ) & 7 )
#define CLIDR_LOC(c)		( ( (c) >> 24 ) & 7 )
#define CLIDR_LOUU(c)		( ( (c) >> 27 ) & 7 )

/* CTR_EL0, IDC set when CLIDR_EL1 says the data cache need not be cleaned for the instruction side */
LOCAL UD ctr_effective( void )
{
	UD	ctr, clidr;

	Asm("mrs %0, ctr_el0" : "=r"(ctr));
	if ( ( ctr & CTR_IDC ) == 0 ) {
		Asm("mrs %0, clidr_el1" : "=r"(clidr));
		if ( CLIDR_LOC(clidr) == 0 || ( CLIDR_LOUIS(clidr) == 0 && CLIDR_LOUU(clidr) == 0 ) ) {
			ctr |= CTR_IDC;
		}
	}
	return ctr;
}

/*
 * Written instructions cleaned out of the data cache to the point of
 * unification, where the instruction side fetches from, when CTR_EL0
 * and CLIDR_EL1 say that is needed (ctr_effective: the Cortex-A72; not
 * the Cortex-A76). The instruction caches are invalidated afterwards,
 * for all of a program at once (knl_icache_sync_all).
 */
EXPORT void knl_dcache_clean_pou( UBINT va, UD size )
{
	UD	ctr, dline;
	UBINT	p, end = va + size;

	ctr = ctr_effective();
	if ( ( ctr & CTR_IDC ) != 0 ) {
		return;
	}
	dline = 4UL << ( ( ctr >> 16 ) & 0xf );
	for ( p = va & ~( dline - 1 ); p < end; p += dline ) {
		Asm("dc cvau, %0" :: "r"(p) : "memory");
	}
}

EXPORT void knl_icache_sync( UBINT va, UD size )
{
	UD	ctr, dline, iline;
	UBINT	p, end = va + size;

	ctr = ctr_effective();
	dline = 4UL << ( ( ctr >> 16 ) & 0xf );
	iline = 4UL << ( ctr & 0xf );
	if ( ( ctr & CTR_IDC ) == 0 ) {
		for ( p = va & ~( dline - 1 ); p < end; p += dline ) {
			Asm("dc cvau, %0" :: "r"(p) : "memory");
		}
	}
	Asm("dsb ish" ::: "memory");
	if ( ( ctr & CTR_DIC ) == 0 ) {
		if ( CTR_L1IP(ctr) == CTR_L1IP_PIPT ) {
			for ( p = va & ~( iline - 1 ); p < end; p += iline ) {
				Asm("ic ivau, %0" :: "r"(p) : "memory");
			}
		} else {
			Asm("ic ialluis" ::: "memory");
		}
	}
	Asm("dsb ish; isb" ::: "memory");
}

/*
 * Tables of 2MB blocks for the gigabytes that RAM fills only in part. A
 * 1GB block there would map what follows RAM in that gigabyte as memory
 * too -- on the Raspberry Pi 4 the peripherals from 0xFC00_0000 -- and
 * the processor may read ahead into memory it holds as such, registers
 * included. A range has at most two such ends. They are taken before
 * the page allocator exists, so they are part of the image.
 */
#define EDGE_TABLES		(2 * KNL_MAX_MEMRANGE)
#define L1_BLOCK		(1ULL << L1_SHIFT)
#define L2_BLOCK		(1ULL << L2_SHIFT)

LOCAL UD	edge_l2[EDGE_TABLES][PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
LOCAL INT	edge_n = 0;

/*
 * Map every RAM range from the DTB into the linear map: 1GB blocks where
 * a range fills the whole gigabyte, 2MB blocks where it does not. Blocks
 * are Normal write-back memory. Only RAM is mapped this way: peripherals
 * are reached through the device window (DEV_BASE).
 */
EXPORT void knl_mmu_map_ram( void )
{
	UD	*l1 = (UD *)PA2VA(knl_pt_l1_ram_pa);
	INT	i;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	s   = knl_mem_range[i].start;
		UD	end = knl_mem_range[i].start + knl_mem_range[i].size;
		UD	pa  = s & ~(L1_BLOCK - 1);

		for ( ; pa < end; pa += L1_BLOCK ) {
			UD	idx = pa >> L1_SHIFT;
			UD	lo = ( s > pa ) ? s : pa;
			UD	hi = ( end < pa + L1_BLOCK ) ? end : pa + L1_BLOCK;
			UD	*l2, a;

			if ( idx >= PT_ENTRIES ) break;		/* linear map covers 512GB */
			if ( lo == pa && hi == pa + L1_BLOCK ) {
				if ( l1[idx] == 0 ) {
					l1[idx] = pa | PTE_BLOCK_NORMAL_XN;	/* data only: never executed */
				}
				continue;
			}
			if ( l1[idx] == 0 ) {
				if ( edge_n >= EDGE_TABLES ) continue;
				l2 = edge_l2[edge_n++];
				l1[idx] = VA2PA(l2) | PTE_TABLE_DESC;
			} else if ( PTE_IS_TABLE(l1[idx]) ) {
				/* the kernel's own gigabyte, or an end another range made */
				l2 = (UD *)PA2VA(PTE_PA(l1[idx]));
			} else {
				continue;			/* already one block */
			}
			for ( a = lo & ~(L2_BLOCK - 1); a < hi; a += L2_BLOCK ) {
				UD	j = ( a >> L2_SHIFT ) & ( PT_ENTRIES - 1 );

				if ( l2[j] == 0 ) {
					l2[j] = a | PTE_BLOCK_NORMAL_XN;
				}
			}
		}
	}
	Asm("dsb ishst" ::: "memory");
	tlb_flush_all();
}

/*
 * Allocate a zeroed page table page
 */
LOCAL UD *pt_alloc( void )
{
	PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_ZERO);

	if ( pf == NULL ) return NULL;
	pf->flags = PF_PTABLE | PF_HEAD;
	return (UD *)knl_pf_to_va(pf);
}

/*
 * Taken with interrupts disabled while the tables are changed: two
 * processors making the same missing table would each put in their own,
 * and the mappings made in the one that lost would be gone.
 */
LOCAL T_SPLOCK	pt_lock;

/*
 * Return the L3 table for 'va' under the L0 table at l0_pa, creating the
 * intermediate tables when 'create' is set. NULL if absent or if a block
 * mapping is in the way.
 */
LOCAL UD *pt_l3_of( UD l0_pa, UBINT va, BOOL create )
{
	UD	*tbl = (UD *)PA2VA(l0_pa);
	UINT	shift;

	for ( shift = L0_SHIFT; shift > L3_SHIFT; shift -= 9 ) {
		UD	*ent = &tbl[PT_IDX(va, shift)];
		UD	e = *ent;

		if ( (e & PTE_VALID) == 0 ) {
			UD	*nt;
			if ( !create ) return NULL;
			nt = pt_alloc();
			if ( nt == NULL ) return NULL;
			*ent = VA2PA(nt) | PTE_TABLE_DESC;
			Asm("dsb ishst" ::: "memory");
			tbl = nt;
		} else if ( PTE_IS_TABLE(e) ) {
			tbl = (UD *)PA2VA(PTE_PA(e));
		} else {
			return NULL;		/* block mapping: cannot hold 4KB pages */
		}
	}
	return tbl;
}

/*
 * Map [va, va + size) to [pa, pa + size) with 4KB pages and descriptor
 * bits 'attr' (PTE_PAGE_*). Existing entries are overwritten with
 * break-before-make.
 */
EXPORT ER knl_pt_map( UD l0_pa, UBINT va, UD pa, UD size, UD attr )
{
	UBINT	end = va + size;
	UINT	imask;
	ER	er = E_OK;

	if ( (va | pa | size) & (PAGE_SIZE - 1) ) return E_PAR;

	ISpinLock(&pt_lock, &imask);
	for ( ; va < end; va += PAGE_SIZE, pa += PAGE_SIZE ) {
		UD	*l3 = pt_l3_of(l0_pa, va, TRUE);
		UD	*ent;

		if ( l3 == NULL ) {
			er = E_NOMEM;
			break;
		}
		ent = &l3[PT_IDX(va, L3_SHIFT)];
		if ( (*ent & PTE_VALID) != 0 ) {
			*ent = 0;
			knl_tlb_flush_va(va);
		}
		*ent = (pa & PTE_ADDR_MASK) | attr;
	}
	Asm("dsb ishst; isb" ::: "memory");
	ISpinUnlock(&pt_lock, &imask);
	return er;
}

/*
 * Remove the 4KB mappings of [va, va + size). Tables are kept.
 */
EXPORT ER knl_pt_unmap( UD l0_pa, UBINT va, UD size )
{
	UBINT	end = va + size;
	UINT	imask;

	if ( (va | size) & (PAGE_SIZE - 1) ) return E_PAR;

	ISpinLock(&pt_lock, &imask);
	for ( ; va < end; va += PAGE_SIZE ) {
		UD	*l3 = pt_l3_of(l0_pa, va, FALSE);

		if ( l3 == NULL ) continue;
		l3[PT_IDX(va, L3_SHIFT)] = 0;
		knl_tlb_flush_va(va);
	}
	ISpinUnlock(&pt_lock, &imask);
	return E_OK;
}

/*
 * The tables under [va, va + size) of the user half that map nothing
 * any more taken out of the space at l0_pa: an L3 table that has no
 * entry left, and the L2 table above it when that has none left either
 * (design 4.6.2). A stack given back leaves them so, and without this the
 * tables of a space that makes and ends tasks would only grow. Each one
 * is unhooked and its translations dropped from every processor's TLB
 * here, under the lock; the page itself is given back later
 * (knl_pt_reclaim), once every other processor has been seen past any
 * walk of the tables it may have begun without the lock before.
 */
#define PT_DEAD_MAX	64

LOCAL struct {
	UD	pa;			/* the table's page */
	UD	seen[CNF_MAX_PRCID];	/* each processor's ctxsw_cnt when it was unhooked */
}		pt_dead[PT_DEAD_MAX];
LOCAL INT	pt_ndead = 0;
LOCAL UW	pt_pruned = 0;		/* tables taken out, for the tests */

LOCAL BOOL pt_empty( CONST UD *tbl )
{
	INT	i;

	for ( i = 0; i < PT_ENTRIES; i++ ) {
		if ( tbl[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* Under pt_lock: the table at pa to be given back once no walk can be in it */
LOCAL BOOL pt_bury( UD pa )
{
	INT	i;

	if ( pt_ndead >= PT_DEAD_MAX ) {
		return FALSE;
	}
	pt_dead[pt_ndead].pa = pa;
	for ( i = 0; i < CNF_MAX_PRCID; i++ ) {
		pt_dead[pt_ndead].seen[i] = knl_pcpu_tbl[i].ctxsw_cnt;
	}
	pt_ndead++;
	return TRUE;
}

EXPORT void knl_pt_prune( UD l0_pa, UBINT va, UD size )
{
	UBINT	end = va + size, r;
	UINT	imask;

	if ( size == 0 || va >= ( (UBINT)1 << 48 ) ) return;
	ISpinLock(&pt_lock, &imask);
	for ( r = va & ~(((UBINT)1 << L2_SHIFT) - 1); r < end; r += (UBINT)1 << L2_SHIFT ) {
		UD	*l0 = (UD *)PA2VA(l0_pa), *l1, *l2, *l3;
		UD	*e0, *e1, *e2;

		e0 = &l0[PT_IDX(r, L0_SHIFT)];
		if ( !PTE_IS_TABLE(*e0) ) continue;
		l1 = (UD *)PA2VA(PTE_PA(*e0));
		e1 = &l1[PT_IDX(r, L1_SHIFT)];
		if ( !PTE_IS_TABLE(*e1) ) continue;
		l2 = (UD *)PA2VA(PTE_PA(*e1));
		e2 = &l2[PT_IDX(r, L2_SHIFT)];
		if ( !PTE_IS_TABLE(*e2) ) continue;
		l3 = (UD *)PA2VA(PTE_PA(*e2));
		if ( !pt_empty(l3) || pt_ndead >= PT_DEAD_MAX - 1 ) continue;

		*e2 = 0;
		Asm("dsb ishst; tlbi vaae1is, %0; dsb ish" :: "r"(r >> PAGE_SHIFT) : "memory");
		(void)pt_bury(VA2PA(l3));
		pt_pruned++;
		if ( pt_empty(l2) ) {
			*e1 = 0;
			Asm("dsb ishst; tlbi vaae1is, %0; dsb ish" :: "r"(r >> PAGE_SHIFT) : "memory");
			(void)pt_bury(VA2PA(l2));
			pt_pruned++;
		}
	}
	Asm("isb" ::: "memory");
	ISpinUnlock(&pt_lock, &imask);
}

/*
 * The tables knl_pt_prune took out given back, those that every other
 * online processor has gone through the dispatcher since, or is idle
 * in it: a walk without the lock is made with interrupts disabled, so
 * one that had begun before has ended by then. TRUE while some still
 * wait. Called wherever tables come and go, and by the reaper of the
 * processes (proc.c).
 */
EXPORT BOOL knl_pt_reclaim( void )
{
	UD	free_pa[PT_DEAD_MAX];
	INT	nfree = 0, i, k, me = knl_pcpu()->prcid - 1;
	UINT	imask;
	BOOL	left;

	ISpinLock(&pt_lock, &imask);
	for ( i = 0; i < pt_ndead; ) {
		BOOL	past = TRUE;

		for ( k = 0; k < knl_num_prc && k < CNF_MAX_PRCID; k++ ) {
			T_PCPU	*p = &knl_pcpu_tbl[k];

			if ( k == me || p->state != PC_ONLINE ) continue;
			if ( p->ctxsw_cnt == pt_dead[i].seen[k] && p->ctxtsk != NULL ) {
				past = FALSE;
				break;
			}
		}
		if ( past ) {
			free_pa[nfree++] = pt_dead[i].pa;
			pt_dead[i] = pt_dead[--pt_ndead];
		} else {
			i++;
		}
	}
	left = (BOOL)( pt_ndead > 0 );
	ISpinUnlock(&pt_lock, &imask);
	for ( i = 0; i < nfree; i++ ) {
		knl_free_pages(knl_pa_to_pf(free_pa[i]), 0);
	}
	return left;
}

/* Tables taken out so far, and those not yet given back (the tests) */
EXPORT UW knl_pt_pruned( INT *p_waiting )
{
	if ( p_waiting != NULL ) *p_waiting = pt_ndead;
	return pt_pruned;
}

/*
 * Physical address of a 4KB-mapped 'va' (page granularity). ~0 if unmapped
 * or mapped by a block.
 */
EXPORT UD knl_pt_lookup( UD l0_pa, UBINT va )
{
	UINT	s = disint();			/* a walk without the lock (knl_pt_prune) */
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e = ( l3 != NULL ) ? l3[PT_IDX(va, L3_SHIFT)] : 0;

	enaint(s);
	if ( (e & PTE_VALID) == 0 ) return ~(UD)0;
	return PTE_PA(e) | (va & (PAGE_SIZE - 1));
}

/* The entry that maps a page, with its access bits; 0 when there is none */
EXPORT UD knl_pt_entry( UD l0_pa, UBINT va )
{
	UINT	s = disint();			/* a walk without the lock (knl_pt_prune) */
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e = ( l3 != NULL ) ? l3[PT_IDX(va, L3_SHIFT)] : 0;

	enaint(s);
	return ( (e & PTE_VALID) != 0 ) ? e : 0;
}

/*
 * The first page address in [va, end) that the tables at l0_pa map, or
 * end when none does. A table that is not there is stepped over whole,
 * so a large stretch that was only reserved (design 9.8.1) is crossed
 * in a few steps. The tables are read without the lock, each walk with
 * interrupts disabled, so that a table knl_pt_prune takes out is not
 * given back while it is being read; the caller keeps the space.
 */
EXPORT UBINT knl_pt_next( UD l0_pa, UBINT va, UBINT end )
{
	UINT	s;
	BOOL	found = FALSE;

	va &= ~(UBINT)(PAGE_SIZE - 1);
	while ( va < end && !found ) {
		UD	*tbl = (UD *)PA2VA(l0_pa);
		UINT	shift;

		s = disint();
		for ( shift = L0_SHIFT; ; shift -= 9 ) {
			UD	e = tbl[PT_IDX(va, shift)];

			if ( (e & PTE_VALID) == 0 ) {
				UBINT	next = ( va | ( ((UBINT)1 << shift) - 1 ) ) + 1;

				va = ( next <= va ) ? end : next;
				break;
			}
			if ( shift == L3_SHIFT || !PTE_IS_TABLE(e) ) {
				found = TRUE;
				break;
			}
			tbl = (UD *)PA2VA(PTE_PA(e));
		}
		enaint(s);
	}
	return ( va < end ) ? va : end;
}

/*
 * Kernel virtual address of a peripheral register block
 */
EXPORT void *knl_dev_va( UBINT pa )
{
	return (void *)DEV_BASE(pa);
}

/* ------------------------------------------------------------------------ */
/*
 * T-Kernel/SM address space services
 */

/*
 * MapMemory: physical range -> kernel virtual address.
 *	Device memory (MM_CDIS or outside RAM) comes from the device window,
 *	RAM from the linear map; nothing is allocated, so UnmapMemory is a no-op.
 *	MM_USER (process space) is a later phase.
 */
EXPORT ER MapMemory( CONST void *paddr, SZ len, UINT attr, void **laddr )
{
	UD	pa = (UD)(UBINT)paddr;
	INT	i;
	BOOL	ram = FALSE;

	if ( laddr == NULL || len <= 0 ) return E_PAR;
	if ( (attr & MM_USER) != 0 ) return E_NOSPT;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	s = knl_mem_range[i].start, e = s + knl_mem_range[i].size;
		if ( pa >= s && pa + (UD)len <= e ) {
			ram = TRUE;
			break;
		}
	}
	if ( ram && (attr & MM_CDIS) == 0 ) {
		*laddr = PA2VA(pa);
	} else {
		if ( pa + (UD)len > (U64(1) << L0_SHIFT) ) return E_PAR;	/* device window: 512GB */
		*laddr = (void *)DEV_BASE(pa);
	}
	return E_OK;
}

EXPORT ER UnmapMemory( CONST void *laddr )
{
	return E_OK;
}

/*
 * ConvPhysicalAddress: kernel virtual -> physical. Returns the length of
 * the physically contiguous range starting at laddr (up to len).
 */
EXPORT INT ConvPhysicalAddress( CONST void *laddr, INT len, void **paddr )
{
	UBINT	va = (UBINT)laddr;
	UD	pa;

	if ( paddr == NULL || len <= 0 ) return E_PAR;
	if ( va < KVA_BASE ) return E_PAR;	/* a process's address: its own tables map it (knl_prc_dma) */

	if ( va >= KVA_BASE && va < DEV_VA_BASE ) {		/* linear map */
		*paddr = (void *)VA2PA(va);
		return len;
	}
	if ( va >= DEV_VA_BASE && va < VMAP_VA_BASE ) {		/* device window */
		*paddr = (void *)(va - DEV_VA_BASE);
		return len;
	}
	pa = knl_pt_lookup(knl_pt_l0_k_pa, va);
	if ( pa == ~(UD)0 ) return E_PAR;
	*paddr = (void *)(UBINT)pa;
	{
		/* count the contiguous pages */
		INT	n = (INT)(PAGE_SIZE - (va & (PAGE_SIZE - 1)));
		UD	next = (pa & PAGE_MASK) + PAGE_SIZE;
		UBINT	v = (va & PAGE_MASK) + PAGE_SIZE;
		while ( n < len && knl_pt_lookup(knl_pt_l0_k_pa, v) == next ) {
			n += PAGE_SIZE;
			next += PAGE_SIZE;
			v += PAGE_SIZE;
		}
		return ( n < len ) ? n : len;
	}
}

#endif /* CPU_CORE_ARMV8A */
