/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_ggtt.h (x64 PC)
 *	Intel's integrated graphics of the 9th generation as both its
 *	drivers share it: the display (planes, pipes) and the render and
 *	copy engines (device/gpu/intel). Neither owns the device; this part
 *	finds it, and keeps the one global graphics translation table
 *	(GGTT) both of them place what they need in
 *	(docs/private/x64_surface_go2_study.md 5.13).
 *
 *	The GGTT maps a 32 bit address space of the GPU's (its size from
 *	GGMS, 4GB on the machines seen) page by page onto memory. Its first
 *	part, as long as the second BAR (GMADR), is "mappable": the
 *	processor reaches it through that BAR. Every range in it is taken
 *	from here, by either driver, with what it is for: a range that has
 *	to be mappable is taken from the first part, any other from above
 *	it while there is room. What the firmware left mapped (the screen
 *	of the GOP, its stolen memory) is entered as taken when this part
 *	starts, as a few ranges; a range asked for that finds no free room
 *	is placed over them, never over the one at 0 (the GOP's screen).
 *
 *	A range is named, so that one side can find where the other placed
 *	something (the render side writing into the display's surface when
 *	it composites). Each driver writes the entries of its own ranges
 *	only, and the cache of the table is flushed after every write here.
 *
 *	Either driver may be the first to call: the device is found and
 *	the table read on the first call, once.
 */

#ifndef _SYSDEPEND_X64_IGT_GGTT_H_
#define _SYSDEPEND_X64_IGT_GGTT_H_

#include <ts/pcie.h>

typedef struct {
	T_PCIDEV	pci;
	UBINT		mmio;		/* the first BAR as the kernel sees it: registers, then the GGTT */
	UD		gmadr_pa;	/* the second BAR: the mappable part, through the processor */
	UD		mappable;	/* its size */
	UD		ggtt_size;	/* bytes of GPU address space the GGTT maps */
	UD		stolen_pa;	/* memory the firmware set aside for the GPU */
	UD		stolen_size;
	UW		ggc, bdsm;	/* as read */
	UINT		fw_pages;	/* entries the firmware left mapped in the mappable part */
} T_IGTDEV;

/*
 * The device, found on the first call. E_NOEXS when the machine has no
 * Intel display device or it is not of the kind this knows.
 */
IMPORT ER	knl_igt_dev( T_IGTDEV *dev );

/* Who a range is for */
#define IGT_GGTT_DISPLAY	1
#define IGT_GGTT_RENDER		2
#define IGT_GGTT_FIRMWARE	3

/* What a range has to be */
#define IGT_GGTT_MAPPABLE	0x0001	/* within the mappable part */

/*
 * A range of 'size' bytes (a whole number of pages), aligned to 'align'
 * (a power of two, at least a page), for 'owner', under 'name' (up to
 * 15 characters; an empty or NULL name is not looked for). Its GGTT
 * address goes to *p_off. The entries are not written.
 */
IMPORT ER	knl_igt_ggtt_alloc( UD size, UD align, UINT flags, UINT owner, CONST char *name, UD *p_off );
IMPORT ER	knl_igt_ggtt_free( UD off );

/*
 * A range at a given place taken: one that is free, or within what the
 * firmware left mapped (the display going on with the GOP's screen).
 * E_OBJ when another driver has some of it.
 */
IMPORT ER	knl_igt_ggtt_claim( UD off, UD size, UINT owner, CONST char *name );

/* Where a named range is, and how large: E_NOEXS when there is none */
IMPORT ER	knl_igt_ggtt_find( CONST char *name, UD *p_off, UD *p_size );

/*
 * The entries of 'npages' pages from 'off' made to map the pages at
 * pa[0..npages-1] (contiguous from pa[0] when 'contig'), or the scratch
 * page (clear), and the GGTT's cache flushed.
 */
IMPORT void	knl_igt_ggtt_set( UD off, CONST UD *pa, UD npages, BOOL contig );
IMPORT void	knl_igt_ggtt_clear( UD off, UD npages );

/* An entry as it is now (for the record) */
IMPORT UD	knl_igt_ggtt_pte( UD off );

/* Lines for KERNEL.TXT: the ranges taken */
IMPORT void	knl_igt_ggtt_report( void );

#endif /* _SYSDEPEND_X64_IGT_GGTT_H_ */
