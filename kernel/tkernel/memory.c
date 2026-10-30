/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07 .
 *
 *----------------------------------------------------------------------
 */

/*
 *	memory.c
 *	In-kernel dynamic memory management
 */

#include "kernel.h"
#include "memory.h"
#include <tk/smem.h>

/*
 * Taken with interrupts disabled around the kernel heap: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	knl_imalloc_lock;

/*
 * FreeQue search
 *	Search the free area whose size is equal to 'blksz',
 *	or larger than
 *      'blksz' but closest.
 *	If it does not exist, return '&imacb->freeque'.
 */
EXPORT QUEUE* knl_searchFreeArea( IMACB *imacb, W blksz )
{
	QUEUE	*q = &imacb->freeque;

	/* For area whose memory pool size is less than 1/4,
	   search from smaller size.
	   Otherwise, search from larger size. */
	if ( blksz > imacb->memsz / 4 ) {
		/* Search from larger size. */
		W fsz = 0;
		while ( (q = q->prev) != &imacb->freeque ) {
			fsz = FreeSize(q);
			if ( fsz <= blksz ) {
				return ( fsz < blksz )? q->next: q;
			}
		}
		return ( fsz >= blksz )? q->next: q;
	} else {
		/* Search from smaller size. */
		while ( (q = q->next) != &imacb->freeque ) {
			if ( FreeSize(q) >= blksz ) {
				break;
			}
		}
		return q;
	}
}


/*
 * Registration of free area on FreeQue
 *	FreeQue is composed of 2 types: Queue that links the
 *	different size of areas by size and queue that links the
 *	same size of areas.
 *
 *	freeque
 *	|
 *	|   +-----------------------+	    +-----------------------+
 *	|   | AreaQue		    |	    | AreaQue		    |
 *	|   +-----------------------+	    +-----------------------+
 *	*---> FreeQue Size order    |	    | EmptyQue		    |
 *	|   | FreeQue Same size   --------->| FreeQue Same size   ----->
 *	|   |			    |	    |			    |
 *	|   |			    |	    |			    |
 *	|   +-----------------------+	    +-----------------------+
 *	|   | AreaQue		    |	    | AreaQue		    |
 *	v   +-----------------------+	    +-----------------------+
 */
EXPORT void knl_appendFreeArea( IMACB *imacb, QUEUE *aq )
{
	QUEUE	*fq;
	W	size = AreaSize(aq);

	/* Registration position search */
	/*  Search the free area whose size is equal to 'blksz',
	 *  or larger than 'blksz' but closest.
	 *  If it does not exist, return '&imacb->freeque'.
	 */
	fq = knl_searchFreeArea(imacb, size);

	/* Register */
	clrAreaFlag(aq, AREA_USE);
	if ( fq != &imacb->freeque && FreeSize(fq) == size ) {
		/* FreeQue Same size */
		(aq + 2)->next = (fq + 1)->next;
		(fq + 1)->next = aq + 2;
		(aq + 2)->prev = fq + 1;
		if( (aq + 2)->next != NULL ) {
			(aq + 2)->next->prev = aq + 2;
		}
		(aq + 1)->next = NULL;
	} else {
		/* FreeQue Size order */
		QueInsert(aq + 1, fq);
		(aq + 2)->next = NULL;
		(aq + 2)->prev = (QUEUE*)(UBINT)size;
	}
}

/*
 * Delete from FreeQue
 */
EXPORT void knl_removeFreeQue( QUEUE *fq )
{
	if ( fq->next == NULL ) {	/* FreeQue Same size */
		(fq + 1)->prev->next = (fq + 1)->next;
		if ( (fq + 1)->next != NULL ) {
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
	} else {			/* FreeQue Size order */
		if ( (fq + 1)->next != NULL ) {		/* having FreeQue Same size */
			QueInsert((fq + 1)->next - 1, fq);
			(fq + 1)->next->prev = (fq + 1)->prev;
		}
		QueRemove(fq);
	}
}

/*
 * Register area
 *	Insert 'ent' just after 'que.'
 */
EXPORT void knl_insertAreaQue( QUEUE *que, QUEUE *ent )
{
	ent->prev = que;
	ent->next = que->next;
	Assign(que->next->prev, ent);
	que->next = ent;
}

/*
 * Delete area
 */
EXPORT void knl_removeAreaQue( QUEUE *aq )
{
	Mask(aq->prev)->next = aq->next;
	Assign(aq->next->prev, Mask(aq->prev));
}

/* ------------------------------------------------------------------------ */

#if USE_IMALLOC
/* ------------------------------------------------------------------------ */

Noinit(EXPORT IMACB *knl_imacb);

/* The bytes of the heap's areas in use, kept as they are taken and given back */
LOCAL SZ	knl_imalloc_used = 0;

/* What of the heap is free, at once: for those who size what they keep by it */
EXPORT SZ knl_Ifree_bytes( void )
{
	return (SZ)knl_imacb->memsz - knl_imalloc_used;
}

/*
 * Large blocks. The kernel heap is an area of a fixed size, set aside
 * at start; what one document, picture or table needs may be larger
 * than all of it. A block of IM_BIG bytes or more is therefore made of
 * pages of their own, wherever they lie, mapped one after another in
 * the kernel variable area (knl_vmap), as much as the machine has. Its
 * head, IM_HEAD bytes before what is handed out, says how many pages
 * it is. Such a block is not physically contiguous: whoever gives
 * memory to a device asks where each piece is (ConvPhysicalAddress),
 * as for any memory of the variable area. Only a task asks for one; in
 * a handler, and before the tasks run, the heap gives it as before.
 */
#define IM_BIG		( 1024 * 1024 )
#define IM_HEAD		64
#define IM_MAGIC	0x42494742494D454DULL
#define IM_PAGE		4096		/* the pages knl_vmap maps */

typedef struct {
	UD	magic;
	UD	npages;
	SZ	size;		/* what was asked for */
} IMBIG;

/*
 * Mapping a block's pages and taking them away again costs a good part
 * of what is done with it: a document made again at each letter typed
 * frees and asks for blocks of the same few sizes over and over. A few
 * blocks freed are therefore kept mapped, IM_KEEP of them and no more
 * than IM_KEEP_BYTES together, and one asked for of about the same size
 * (no more than a quarter larger) is handed out again. When the pages
 * for a new block cannot be had, the kept ones are let go first.
 */
#define IM_KEEP		8
#define IM_KEEP_BYTES	( 64 * 1024 * 1024 )

LOCAL T_SPLOCK	im_keep_lock;
LOCAL void	*im_keep[IM_KEEP];	/* the heads of the blocks kept */
LOCAL UD	im_keep_pages = 0;

LOCAL BOOL im_is_big( CONST void *ptr )
{
	UBINT	v = (UBINT)ptr;

	return (BOOL)( v >= (UBINT)VMAP_VA_BASE && v < (UBINT)VMAP_VA_BASE + CNF_VMAP_SIZE );
}

LOCAL void *im_big_get( SZ size )
{
	UD	np = ( (UD)size + IM_HEAD + IM_PAGE - 1 ) / IM_PAGE;
	IMBIG	*h;

	UINT	imask;
	INT	i, best = -1;
	BOOL	kept;

	if ( in_indp() || knl_ctxtsk == NULL ) {
		return NULL;
	}
	/* a block kept that is about as large */
	ISpinLock(&im_keep_lock, &imask);
	for ( i = 0; i < IM_KEEP; i++ ) {
		IMBIG	*k = (IMBIG *)im_keep[i];

		if ( k != NULL && k->npages >= np && k->npages <= np + np / 4
		  && ( best < 0 || k->npages < ((IMBIG *)im_keep[best])->npages ) ) {
			best = i;
		}
	}
	h = NULL;
	kept = (BOOL)( best >= 0 );
	if ( best >= 0 ) {
		h = (IMBIG *)im_keep[best];
		im_keep[best] = NULL;
		im_keep_pages -= h->npages;
	}
	ISpinUnlock(&im_keep_lock, &imask);
	if ( h == NULL ) {
		h = (IMBIG *)knl_vmap(np, 0);
	}
	if ( h == NULL ) {
		/* the pages the kept blocks hold let go, and tried again */
		for ( i = 0; i < IM_KEEP; i++ ) {
			IMBIG	*k;

			ISpinLock(&im_keep_lock, &imask);
			k = (IMBIG *)im_keep[i];
			im_keep[i] = NULL;
			if ( k != NULL ) {
				im_keep_pages -= k->npages;
			}
			ISpinUnlock(&im_keep_lock, &imask);
			if ( k != NULL ) {
				knl_vunmap(k, k->npages);
			}
		}
		h = (IMBIG *)knl_vmap(np, 0);
	}
	if ( h == NULL ) {
		return NULL;
	}
	h->magic = IM_MAGIC;
	if ( !kept ) {
		h->npages = np;		/* a kept block keeps how many pages it has */
	}
	h->size = size;

	return (VB *)h + IM_HEAD;
}

LOCAL SZ im_big_size( CONST void *ptr )
{
	CONST IMBIG	*h = (CONST IMBIG *)( (CONST VB *)ptr - IM_HEAD );

	return ( h->magic == IM_MAGIC ) ? h->size : 0;
}

LOCAL void im_big_free( void *ptr )
{
	IMBIG	*h = (IMBIG *)( (VB *)ptr - IM_HEAD );

	if ( h->magic != IM_MAGIC ) {
		return;
	}
	h->magic = 0;
	/* kept mapped for the next one about as large, while there is room */
	if ( !in_indp() ) {
		UINT	imask;
		INT	i;

		ISpinLock(&im_keep_lock, &imask);
		for ( i = 0; i < IM_KEEP; i++ ) {
			if ( im_keep[i] == NULL
			  && ( im_keep_pages + h->npages ) * IM_PAGE <= IM_KEEP_BYTES ) {
				im_keep[i] = h;
				im_keep_pages += h->npages;
				h = NULL;
				break;
			}
		}
		ISpinUnlock(&im_keep_lock, &imask);
		if ( h == NULL ) {
			return;
		}
	}
	knl_vunmap(h, h->npages);
}

/* ------------------------------------------------------------------------ */

/*
 * Memory allocate
 */
EXPORT void* knl_Imalloc( SZ size )
{
	QUEUE	*q, *aq, *aq2;
	UINT	imask;

	/* If it is smaller than the minimum fragment size,
	   allocate the minimum size to it. */
	if( size <= 0 ) {
		return (void *)NULL;
	}
	if ( size >= (SZ)IM_BIG ) {
		void	*big = im_big_get(size);

		if ( big != NULL ) {
			return big;
		}
		/* no pages for it: from the heap, as far as it goes */
	}
	if ( size < (SZ)MIN_FRAGMENT ) {
		size = (SZ)MIN_FRAGMENT;
	} else {
		size = ROUND(size);
	}

	ISpinLock(&knl_imalloc_lock, &imask);

	/* Search FreeQue */
	q = knl_searchFreeArea(knl_imacb, size);
	if ( q == &(knl_imacb->freeque) ) {
		q = NULL; /* Insufficient memory */
		goto err_ret;
	}

	/* There is free area: Split from FreeQue once */
	knl_removeFreeQue(q);

	aq = q - 1;

	/* If there are fragments smaller than the minimum fragment size,
	   allocate them also */
	if ( FreeSize(q) - size >= (SZ)(MIN_FRAGMENT + sizeof(QUEUE)) ) {

		/* Divide area into 2 */
		aq2 = (QUEUE*)((VB*)(aq + 1) + size);
		knl_insertAreaQue(aq, aq2);

		/* Register remaining area to FreeQue */
		knl_appendFreeArea(knl_imacb, aq2);
	}
	setAreaFlag(aq, AREA_USE);
	knl_imalloc_used += (SZ)AreaSize(aq);

err_ret:
	ISpinUnlock(&knl_imalloc_lock, &imask);

	return (void *)q;
}

/*
 * Memory allocate  and clear
 */
EXPORT void* knl_Icalloc( SZ nmemb, SZ size )
{
	SZ	sz = nmemb * size;
	void	*mem;

	mem = knl_Imalloc(sz);
	if ( mem == NULL ) {
		return NULL;
	}

	knl_memset(mem, 0, sz);

	return mem;
}


/*
 * Memory allocation size change
 */
EXPORT void* knl_Irealloc( void *ptr, SZ size )
{
	void	*newptr;
	QUEUE	*aq;
	SZ	oldsz;

	if(size != 0) {
		newptr = knl_Imalloc(size);
		if(newptr == NULL) {
			return NULL;
		}
	} else {
		newptr = NULL;
	}

	if(ptr != NULL) {
		if(newptr != NULL) {
			if ( im_is_big(ptr) ) {
				oldsz = im_big_size(ptr);
			} else {
				aq = (QUEUE*)ptr - 1;
				oldsz = (SZ)AreaSize(aq);
			}
			knl_memcpy(newptr, ptr, (size > oldsz)?oldsz:size);
		}
		knl_Ifree(ptr);
	}

	return newptr;
}


/*
 * Free memory
 */
EXPORT void  knl_Ifree( void *ptr )
{
	QUEUE	*aq;
	UINT	imask;

	if ( im_is_big(ptr) ) {
		im_big_free(ptr);
		return;
	}
	ISpinLock(&knl_imalloc_lock, &imask);

	aq = (QUEUE*)ptr - 1;
	clrAreaFlag(aq, AREA_USE);
	knl_imalloc_used -= (SZ)AreaSize(aq);

	if ( !chkAreaFlag(aq->next, AREA_USE) ) {
		/* Merge with free area in after location */
		knl_removeFreeQue(aq->next + 1);
		knl_removeAreaQue(aq->next);
	}

	if ( !chkAreaFlag(aq->prev, AREA_USE) ) {
		/* Merge with free area in front location */
		aq = aq->prev;
		knl_removeFreeQue(aq + 1);
		knl_removeAreaQue(aq->next);
	}

	knl_appendFreeArea(knl_imacb, aq);

	ISpinUnlock(&knl_imalloc_lock, &imask);
}


#if USE_KTEST
/* For the tests: whether the block ptr that knl_Imalloc gave is still taken */
EXPORT BOOL knl_Iinuse( void *ptr )
{
	return chkAreaFlag((QUEUE*)ptr - 1, AREA_USE) ? TRUE : FALSE;
}
#endif

/*
 * Bytes the areas in use hold, and how many areas they are: what the
 * tests look at for memory not given back
 */
EXPORT SZ knl_Iused( INT *p_areas )
{
	QUEUE	*aq;
	SZ	used = 0;
	INT	n = 0;
	UINT	imask;

	ISpinLock(&knl_imalloc_lock, &imask);
	for ( aq = knl_imacb->areaque.next; aq != &knl_imacb->areaque; aq = aq->next ) {
		if ( aq->next == &knl_imacb->areaque ) break;	/* the end mark */
		if ( chkAreaFlag(aq, AREA_USE) ) {
			used += (SZ)AreaSize(aq);
			n++;
		}
	}
	ISpinUnlock(&knl_imalloc_lock, &imask);
	if ( p_areas != NULL ) *p_areas = n;
	return used;
}

/* ------------------------------------------------------------------------ */

/*
 * IMACB Initialization 
 */
LOCAL void initIMACB( void )
{
	QueInit(&(knl_imacb->areaque));
	QueInit(&(knl_imacb->freeque));
}

/*
 * Imalloc initial setting 
 */
EXPORT ER knl_init_Imalloc( void )
{
	QUEUE	*top, *end;

	/* Align top with 4 byte unit alignment for IMACB */
	knl_lowmem_top = (void *)(((UBINT)knl_lowmem_top + 3) & ~(UBINT)0x3);
	knl_imacb = (IMACB*)knl_lowmem_top;
	knl_lowmem_top = (void *)((UBINT)knl_lowmem_top + sizeof(IMACB));

	/* Align top with 8 byte unit alignment */
	knl_lowmem_top = (void *)(((UBINT)knl_lowmem_top + 15) & ~(UBINT)0xf);
	top = (QUEUE*)knl_lowmem_top;
	knl_imacb->memsz = (W)((UBINT)knl_lowmem_limit - (UBINT)knl_lowmem_top - sizeof(QUEUE)*2);

	knl_lowmem_top = knl_lowmem_limit;  /* Update memory free space */

	initIMACB();

	/* Register on AreaQue */
	end = (QUEUE*)((VB*)top + knl_imacb->memsz) + 1;
	knl_insertAreaQue(&knl_imacb->areaque, end);
	knl_insertAreaQue(&knl_imacb->areaque, top);
	setAreaFlag(end, AREA_USE);
	setAreaFlag(&knl_imacb->areaque, AREA_USE);

	knl_appendFreeArea(knl_imacb, top);

	return E_OK;
}

#endif /* USE_IMALLOC */
