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
	UW	csddone;	/* compute dispatches the core told the end of */
	UW	csd_jobs;	/* compute dispatch jobs ended */
	UW	fault_sts;	/* the last fault: hub status, address, client */
	UW	fault_addr;
	UW	fault_id;
	UD	scratch_pa;	/* the page invalid accesses go to */
	UW	ovf_made;	/* overflow pieces made */
	UW	ovf_waits;	/* out-of-memory requests no piece was ready for */
	UW	ovf_most;	/* the most pieces one job's binner took */
	UW	resets;		/* resets of the block */
} T_V3DSTAT;

IMPORT void	v3d_stat( T_V3DSTAT *st );

/*
 * Where the time of the control list jobs went, as the driver sees it,
 * in ticks of the system counter (cntvct_el0) summed over the jobs that
 * ended since the last take. A job runs: the caches made ready and the
 * binner started (setup), the binner until its end is seen (bin), the
 * caches made ready again and the renderer started (mid), the renderer
 * until its end is seen (render), the caches cleaned when the job asked
 * for it (clean). The ends are seen in the interrupt handler, so bin
 * and render include the interrupt's latency.
 */
typedef struct {
	UW	njob;		/* jobs that ended */
	UD	first_kick;	/* the counter when the first of them started; 0 none */
	UD	last_end;	/* and when the last of them ended */
	UD	setup;
	UD	bin;
	UD	mid;
	UD	render;
	UD	clean;
} T_V3DJOBT;

/* What was summed since the last take, and the sums started again */
IMPORT void	v3d_jobt_take( T_V3DJOBT *t );

/* Start the tests on the board in a task of their own (v3d_test.c) */
IMPORT void	v3d_test_start( void );

/*
 * The address spaces of the processes (13-gpu.md 13.5): the table of
 * one (NULL: the kernel's), and the check at start that a job in one
 * reaches its own BOs and not the kernel's (v3d_test.c). Processes get
 * spaces only when it passed.
 */
IMPORT T_V3DMMU	*v3d_as_mmu( void *as );
IMPORT BOOL	v3d_as_check( void );

/*
 * Compositing the windows on the GPU (v3d_compose.c): set up, the check
 * that compares its screen with the processor's and turns it on, and
 * that check in a task of its own when the tests do not run it.
 */
IMPORT ER	v3d_compose_init( void );
IMPORT BOOL	v3d_compose_check( void );
IMPORT void	v3d_compose_start( void );

/*
 * Compute dispatch: the check at start (v3d_compose.c) runs a small
 * compute shader through the queue and compares what it wrote; the
 * backend offers dispatches (GPU_FEAT_CSD) only while 'on' (v3d.c).
 */
IMPORT BOOL	v3d_csd_check( void );
IMPORT void	v3d_csd_offer( BOOL on );

#endif /* _DEVICE_GPU_V3D_V3D_H_ */
