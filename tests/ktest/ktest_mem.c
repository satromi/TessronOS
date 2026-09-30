/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_mem.c
 *	Memory management: page frame allocator (buddy) and Imalloc.
 */

#include "ktest.h"
#include "tstdlib.h"
#include "sysman/pfalloc.h"

/* allocate and free at several orders: free count returns to the start value */
LOCAL void test_pages_symmetry( void )
{
	UD	before = knl_pf_free_count(-1);
	PFRAME	*pf[16];
	INT	i;

	KT_ASSERT(before > 64);
	for ( i = 0; i < 16; i++ ) {
		pf[i] = knl_alloc_pages((UINT)(i % 4), ZONE_NORMAL, 0);
		KT_ASSERT(pf[i] != NULL);
		KT_ASSERT((pf[i]->flags & PF_HEAD) != 0);
		KT_ASSERT_EQ(pf[i]->order, i % 4);
	}
	KT_ASSERT_EQ(knl_pf_free_count(-1), before - (4 * (1 + 2 + 4 + 8)));
	for ( i = 15; i >= 0; i-- ) {
		knl_free_pages(pf[i], (UINT)(i % 4));
	}
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
}

/* a large block is split and merged back; pages are distinct and page aligned */
LOCAL void test_pages_split_merge( void )
{
	UD	before = knl_pf_free_count(-1);
	PFRAME	*big, *small[8];
	UD	pa0, pa;
	INT	i, j;

	big = knl_alloc_pages(3, ZONE_NORMAL, 0);		/* 8 pages */
	KT_ASSERT(big != NULL);
	pa0 = knl_pf_to_pa(big);
	KT_ASSERT_EQ(pa0 & (PAGE_SIZE * 8 - 1), 0);		/* aligned to its size */
	knl_free_pages(big, 3);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);

	for ( i = 0; i < 8; i++ ) {
		small[i] = knl_alloc_pages(0, ZONE_NORMAL, 0);
		KT_ASSERT(small[i] != NULL);
		pa = knl_pf_to_pa(small[i]);
		KT_ASSERT_EQ(pa & (PAGE_SIZE - 1), 0);
		KT_ASSERT(knl_pa_to_pf(pa) == small[i]);
		for ( j = 0; j < i; j++ ) {
			KT_ASSERT(small[j] != small[i]);
		}
	}
	for ( i = 0; i < 8; i++ ) {
		knl_free_pages(small[i], 0);
	}
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
	big = knl_alloc_pages(3, ZONE_NORMAL, 0);		/* merged back: an order-3 block exists again */
	KT_ASSERT(big != NULL);
	knl_free_pages(big, 3);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
}

/* KAF_ZERO, double free is ignored, reserved pages are never returned */
LOCAL void test_pages_flags( void )
{
	UD	before = knl_pf_free_count(-1);
	PFRAME	*pf;
	UD	*p;
	INT	i;

	pf = knl_alloc_pages(1, ZONE_NORMAL, 0);
	KT_ASSERT(pf != NULL);
	p = (UD *)knl_pf_to_va(pf);
	for ( i = 0; i < 1024; i++ ) p[i] = 0x5a5a5a5a5a5a5a5aULL;
	knl_free_pages(pf, 1);
	pf = knl_alloc_pages(1, ZONE_NORMAL, KAF_ZERO);	/* likely the same block */
	KT_ASSERT(pf != NULL);
	p = (UD *)knl_pf_to_va(pf);
	for ( i = 0; i < 1024; i++ ) {
		if ( p[i] != 0 ) break;
	}
	KT_ASSERT_EQ(i, 1024);
	knl_free_pages(pf, 1);
	knl_free_pages(pf, 1);					/* double free: ignored */
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);

	KT_ASSERT(knl_alloc_pages(PF_MAX_ORDER + 1, ZONE_NORMAL, 0) == NULL);
	KT_ASSERT(knl_pa_to_pf(knl_pf_base + (knl_pf_npages << PAGE_SHIFT)) == NULL);

	/* the kernel image is reserved */
	pf = knl_pa_to_pf(LOAD_PA_VALUE);
	KT_ASSERT(pf != NULL);
	KT_ASSERT((pf->flags & PF_RESERVED) != 0);
	knl_free_pages(pf, 0);					/* ignored */
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
	KT_ASSERT(knl_pf_free_count(-1) <= knl_pf_total_count(-1));
}

/* DMA32 zone: pages below 4GB */
LOCAL void test_pages_dma32( void )
{
	PFRAME	*pf;

	pf = knl_alloc_pages(0, ZONE_NORMAL, KAF_DMA32);
	KT_ASSERT(pf != NULL);
	KT_ASSERT(knl_pf_to_pa(pf) < 0x100000000ULL);
	KT_ASSERT_EQ(pf->zone, ZONE_DMA32);
	knl_free_pages(pf, 0);
}

/* Imalloc still works on top (kernel objects use it) */
LOCAL void test_imalloc( void )
{
	void	*a, *b, *c;

	a = Kmalloc(100);
	b = Kmalloc(4000);
	c = Kmalloc(70000);
	KT_ASSERT(a != NULL && b != NULL && c != NULL);
	KT_ASSERT(a != b && b != c);
	KT_ASSERT_EQ((UBINT)a & 7, 0);
	knl_memset(c, 0x11, 70000);
	Kfree(b);
	Kfree(a);
	Kfree(c);
	a = Kmalloc(100);
	KT_ASSERT(a != NULL);
	Kfree(a);
}

/* tk_get_smb / tk_rel_smb / tk_ref_smb: contiguous and vmap (non-cacheable) blocks */
LOCAL void test_smb( void )
{
	UD	before;
	T_RSMB	rsmb;
	void	*a, *b, *c;
	UD	*p;
	INT	i;

	/* warm up: the page tables of the vmap region stay allocated afterwards */
	c = tk_get_smb(3000, TA_RNG0 | TA_NOCACHE);
	KT_ASSERT(c != NULL);
	KT_ASSERT_ER(tk_rel_smb(c), E_OK);
	before = knl_pf_free_count(-1);

	KT_ASSERT_ER(tk_ref_smb(&rsmb), E_OK);
	KT_ASSERT_EQ(rsmb.blksz, PAGE_SIZE);
	KT_ASSERT(rsmb.total > 0 && rsmb.free <= rsmb.total);

	a = tk_get_smb(3, TA_RNG0);				/* contiguous, order 2 (4 pages) */
	KT_ASSERT(a != NULL);
	KT_ASSERT_EQ((UBINT)a & (PAGE_SIZE - 1), 0);
	KT_ASSERT((UBINT)a >= KVA_BASE && (UBINT)a < DEV_VA_BASE);
	p = (UD *)a;
	for ( i = 0; i < 3 * 512; i++ ) p[i] = (UD)i;
	KT_ASSERT_EQ(p[3 * 512 - 1], 3 * 512 - 1);

	b = tk_get_smb(2, TA_RNG0 | TA_NOCACHE);		/* vmap, non-cacheable */
	KT_ASSERT(b != NULL);
	KT_ASSERT((UBINT)b >= VMAP_VA_BASE && (UBINT)b < VMAP_VA_BASE + CNF_VMAP_SIZE);
	p = (UD *)b;
	for ( i = 0; i < 2 * 512; i++ ) p[i] = ~(UD)i;
	KT_ASSERT_EQ(p[2 * 512 - 1], ~(UD)(2 * 512 - 1));

	c = tk_get_smb(3000, TA_RNG0);				/* too big for a buddy block: vmap */
	KT_ASSERT(c != NULL);
	KT_ASSERT((UBINT)c >= VMAP_VA_BASE);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before - 4 - 2 - 3000);

	KT_ASSERT_ER(tk_rel_smb(b), E_OK);
	KT_ASSERT_ER(tk_rel_smb(b), E_PAR);			/* already released */
	KT_ASSERT_ER(tk_rel_smb(a), E_OK);
	KT_ASSERT_ER(tk_rel_smb(c), E_OK);
	KT_ASSERT_ER(tk_rel_smb(NULL), E_PAR);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
	KT_ASSERT(tk_get_smb(0, TA_RNG0) == NULL);
}

/* knl_vmap: guard page between areas, unmap frees everything, lookup */
LOCAL void test_vmap( void )
{
	UD	before = knl_pf_free_count(-1);
	UB	*v1, *v2;
	void	*pa;
	INT	n;

	v1 = knl_vmap(5, 0);
	v2 = knl_vmap(1, 0);
	KT_ASSERT(v1 != NULL && v2 != NULL);
	KT_ASSERT(v2 >= v1 + 6 * PAGE_SIZE);			/* 5 pages + guard */
	v1[0] = 1;
	v1[5 * PAGE_SIZE - 1] = 2;
	KT_ASSERT_EQ(v1[5 * PAGE_SIZE - 1], 2);
	n = ConvPhysicalAddress(v1, 5 * PAGE_SIZE, &pa);
	KT_ASSERT(n >= (INT)PAGE_SIZE && n <= 5 * (INT)PAGE_SIZE);
	KT_ASSERT(knl_pa_to_pf((UD)(UBINT)pa) != NULL);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before - 6);
	knl_vunmap(v2, 1);
	knl_vunmap(v1, 5);
	KT_ASSERT_EQ(knl_pf_free_count(-1), before);
	KT_ASSERT_EQ(ConvPhysicalAddress(v1, PAGE_SIZE, &pa), E_PAR);	/* unmapped */

	/* MapMemory: RAM -> linear map, device -> device window */
	KT_ASSERT_ER(MapMemory((void *)(UBINT)knl_pf_base, PAGE_SIZE, MM_SYSTEM, &pa), E_OK);
	KT_ASSERT_EQ((UBINT)pa, (UBINT)PA2VA(knl_pf_base));
	KT_ASSERT_ER(MapMemory((void *)(UBINT)DBG_UART_PA, PAGE_SIZE, MM_CDIS, &pa), E_OK);
	KT_ASSERT_EQ((UBINT)pa, DEV_BASE(DBG_UART_PA));
	KT_ASSERT_ER(UnmapMemory(pa), E_OK);
}

EXPORT void ktest_mem( void )
{
	KT_RUN(test_pages_symmetry);
	KT_RUN(test_pages_split_merge);
	KT_RUN(test_pages_flags);
	KT_RUN(test_pages_dma32);
	KT_RUN(test_imalloc);
	KT_RUN(test_smb);
	KT_RUN(test_vmap);
}
