/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pfalloc.h
 *	Page frame database and buddy allocator (design 6.4)
 */

#ifndef _SYSMAN_PFALLOC_H_
#define _SYSMAN_PFALLOC_H_

#include <sys/queue.h>

#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE - 1))
#define PF_MAX_ORDER		10		/* 4KB .. 4MB */

/* PFRAME.flags */
#define PF_FREE			0x0001		/* in a buddy free list (head page) */
#define PF_KERNEL		0x0002		/* allocated for the kernel */
#define PF_USER			0x0004		/* allocated for a process */
#define PF_PTABLE		0x0008		/* page table */
#define PF_RESERVED		0x0010		/* never allocated (image, DTB, firmware) */
#define PF_HEAD			0x0020		/* first page of an allocated block */

/* zones */
#define ZONE_DMA32		0		/* PA < 4GB */
#define ZONE_NORMAL		1		/* PA >= 4GB */
#define ZONE_NUM		2

/* knl_alloc_pages flags */
#define KAF_ZERO		0x01		/* zero-fill */
#define KAF_DMA32		0x02		/* must be below 4GB */
#define KAF_NOWAIT		0x04		/* never blocks (always the case for now) */

typedef struct pframe {
	UH	flags;		/* PF_* */
	UB	order;		/* buddy order (head page of a free or allocated block) */
	UB	zone;		/* ZONE_* */
	UW	refcnt;		/* references (shared memory, mappings) */
	UD	owner;		/* owning process ID or kernel usage code */
	QUEUE	link;		/* free list / owner list */
} PFRAME;			/* 32 bytes */

IMPORT PFRAME	*knl_pf_db;		/* page frame database */
IMPORT UD	knl_pf_base;		/* PA of page 0 of the database */
IMPORT UD	knl_pf_npages;		/* number of entries */

IMPORT void	knl_pfalloc_init( UBINT *p_kernel_end_pa, UD extra_reserve );
IMPORT PFRAME	*knl_alloc_pages( UINT order, UINT zone, UINT flags );
IMPORT void	knl_free_pages( PFRAME *pf, UINT order );
IMPORT UD	knl_pf_to_pa( CONST PFRAME *pf );
IMPORT PFRAME	*knl_pa_to_pf( UD pa );
IMPORT UD	knl_pf_free_count( INT zone );	/* zone < 0: all zones */
IMPORT UD	knl_pf_total_count( INT zone );

Inline void *knl_pf_to_va( CONST PFRAME *pf )
{
	return PA2VA(knl_pf_to_pa(pf));
}

#endif /* _SYSMAN_PFALLOC_H_ */
