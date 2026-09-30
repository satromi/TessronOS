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
 * Map every RAM range from the DTB into the linear map with 1GB blocks.
 *	Blocks are Normal write-back memory. Only RAM is mapped this way:
 *	peripherals are reached through the device window (DEV_BASE).
 */
EXPORT void knl_mmu_map_ram( void )
{
	UD	*l1 = (UD *)PA2VA(knl_pt_l1_ram_pa);
	INT	i;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		UD	pa  = knl_mem_range[i].start & ~((1ULL << L1_SHIFT) - 1);
		UD	end = knl_mem_range[i].start + knl_mem_range[i].size;

		for ( ; pa < end; pa += (1ULL << L1_SHIFT) ) {
			UD	idx = pa >> L1_SHIFT;
			if ( idx >= PT_ENTRIES ) break;		/* linear map covers 512GB */
			if ( l1[idx] == 0 ) {
				l1[idx] = pa | PTE_BLOCK_NORMAL_XN;	/* data only: never executed */
			}
		}
	}
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
 * Physical address of a 4KB-mapped 'va' (page granularity). ~0 if unmapped
 * or mapped by a block.
 */
EXPORT UD knl_pt_lookup( UD l0_pa, UBINT va )
{
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e;

	if ( l3 == NULL ) return ~(UD)0;
	e = l3[PT_IDX(va, L3_SHIFT)];
	if ( (e & PTE_VALID) == 0 ) return ~(UD)0;
	return PTE_PA(e) | (va & (PAGE_SIZE - 1));
}

/* The entry that maps a page, with its access bits; 0 when there is none */
EXPORT UD knl_pt_entry( UD l0_pa, UBINT va )
{
	UD	*l3 = pt_l3_of(l0_pa, va, FALSE);
	UD	e;

	if ( l3 == NULL ) return 0;
	e = l3[PT_IDX(va, L3_SHIFT)];
	return ( (e & PTE_VALID) != 0 ) ? e : 0;
}

/*
 * The first page address in [va, end) that the tables at l0_pa map, or
 * end when none does. A table that is not there is stepped over whole,
 * so a large stretch that was only reserved (design 9.8.1) is crossed
 * in a few steps. Tables are only ever added while a space lives, so
 * they are read here without the lock; the caller keeps the space.
 */
EXPORT UBINT knl_pt_next( UD l0_pa, UBINT va, UBINT end )
{
	va &= ~(UBINT)(PAGE_SIZE - 1);
	while ( va < end ) {
		UD	*tbl = (UD *)PA2VA(l0_pa);
		UINT	shift;

		for ( shift = L0_SHIFT; ; shift -= 9 ) {
			UD	e = tbl[PT_IDX(va, shift)];

			if ( (e & PTE_VALID) == 0 ) {
				UBINT	next = ( va | ( ((UBINT)1 << shift) - 1 ) ) + 1;

				if ( next <= va ) return end;
				va = next;
				break;
			}
			if ( shift == L3_SHIFT || !PTE_IS_TABLE(e) ) {
				return ( va < end ) ? va : end;
			}
			tbl = (UD *)PA2VA(PTE_PA(e));
		}
	}
	return end;
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
