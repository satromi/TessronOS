/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	v3d.h
 *	Inside the V3D driver (docs/tessronos/13-gpu.md 13.3).
 *
 *	The GPU's address space is kept apart from the hardware
 *	(v3d_mmu.c): the page table is ordinary memory and the allocator of
 *	GPU addresses is plain bookkeeping, so both run and are tested on a
 *	machine without a V3D. Only telling the hardware that the table
 *	changed (flush) needs the registers, and it is a callback.
 */

#ifndef _DEVICE_GPU_V3D_V3D_H_
#define _DEVICE_GPU_V3D_V3D_H_

#define V3D_PAGE_SHIFT		12
#define V3D_VA_FULL		0x100000000ULL		/* 4GB: every 32 bit GPU address */

/*
 * GPU addresses below this are never handed out, so that 0 means "no
 * address" and a list that runs off the bottom meets an invalid page.
 */
#define V3D_VA_RESERVED		0x10000UL

typedef struct {
	UD	va_size;	/* bytes the table covers */
	UD	npte;		/* its entries */
	UD	pt_pa;		/* the table, physically contiguous */
	UINT	pt_order;	/* its buddy order */
	UW	*pt;		/* the table, mapped uncached */
	UD	*used;		/* one bit per page of GPU address space */
	UD	hint;		/* first page that may be free */
	UD	nfree;		/* pages not handed out */
	T_SPLOCK lock;
	/* the hardware was told nothing of the table's change yet */
	void	(*flush)( void *arg );
	void	*flush_arg;
} T_V3DMMU;

/* A table for 'va_size' bytes (a power of two, 4GB at most), all entries invalid */
IMPORT ER	v3d_mmu_init( T_V3DMMU *m, UD va_size );
IMPORT void	v3d_mmu_fini( T_V3DMMU *m );

/* 'npages' of GPU address space, aligned to 'align' pages (a power of two) */
IMPORT ER	v3d_mmu_alloc( T_V3DMMU *m, UD npages, UD align, UW *p_va );
IMPORT void	v3d_mmu_free( T_V3DMMU *m, UW va, UD npages );

/* Enter pages at 'va' / take them out; both flush */
IMPORT void	v3d_mmu_insert( T_V3DMMU *m, UW va, CONST UD *pa, UD npages, BOOL writable );
IMPORT void	v3d_mmu_remove( T_V3DMMU *m, UW va, UD npages );

/* The entry for a physical page */
IMPORT UW	v3d_mmu_pte( UD pa, BOOL writable );

/* The entry at a GPU address, as the hardware would read it */
IMPORT UW	v3d_mmu_lookup( T_V3DMMU *m, UW va );

/*
 * What the driver counted since it started (v3d.c), for the tests on
 * the board (v3d_test.c, 13-gpu.md 13.8.2).
 */
typedef struct {
	UW	core_ints;	/* interrupts of the core */
	UW	hub_ints;	/* interrupts of the hub */
	UW	frdone;		/* frames the renderer finished */
	UW	fldone;		/* flushes the binner finished */
	UW	outomem;	/* the binner ran out of tile memory */
	UW	faults;		/* MMU faults */
	UW	storms;		/* interrupts turned off for coming on and on */
	UW	core_bits;	/* every core interrupt bit seen */
	UW	fldone_count;	/* binner ends seen in the flush counter first */
	UW	frdone_count;	/* renderer ends seen in the frame counter first */
	UW	fault_sts;	/* the last fault: hub status, address, client */
	UW	fault_addr;
	UW	fault_id;
	UD	scratch_pa;	/* the page invalid accesses go to */
} T_V3DSTAT;

IMPORT void	v3d_stat( T_V3DSTAT *st );

/* Start the tests on the board in a task of their own (v3d_test.c) */
IMPORT void	v3d_test_start( void );

#endif /* _DEVICE_GPU_V3D_V3D_H_ */
