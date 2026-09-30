/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pool.c
 *	Small blocks, taken and given back quickly (design 6.5)
 *
 *	Twelve sizes, each with its own list of blocks that have been
 *	given back. Taking a block is: round the size up to a class, take
 *	the head of that class's list, or carve a new one off the current
 *	chunk. Giving one back is: put it on the head of its class's list.
 *	Neither walks anything, so neither gets slower as more blocks are
 *	out.
 *
 *	Every block carries a head of one word saying which class it
 *	belongs to, which is how giving one back needs nothing but the
 *	pointer. A block too large for any class is a page allocation of
 *	its own and its head says so.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/pool.h>

#define P_CLASSES	12
#define P_MIN_SHIFT	4			/* the smallest class is 16 */
#define P_MAX_SIZE	( 1U << (P_MIN_SHIFT + P_CLASSES - 1) )	/* 32768 */
#define P_CHUNK		( 128 * 1024 )		/* taken from the pages at once */
#define P_PAGE		4096

/*
 * The head of a block. 'cls' is its class, or P_CLASSES for a block
 * that has pages of its own; 'pages' is how many those are. The head is
 * as wide as the widest thing that may follow it, so that what follows
 * is aligned for anything.
 */
typedef struct {
	UW	cls;
	UW	pages;
	UD	pad;
} PHEAD;

typedef struct free_block {
	struct free_block	*next;
} FREEB;

LOCAL FREEB	*p_free[P_CLASSES];
LOCAL UB	*p_at = NULL;		/* the chunk being carved */
LOCAL SZ	p_left = 0;
LOCAL ID	p_mtx = 0;
LOCAL BOOL	p_ready = FALSE;
LOCAL T_POOLSTAT p_stat;

LOCAL SZ class_size( UINT cls )
{
	return (SZ)1 << ( P_MIN_SHIFT + cls );
}

LOCAL UINT class_of( SZ size )
{
	UINT	cls = 0;
	SZ	want = 1 << P_MIN_SHIFT;

	while ( want < size && cls < P_CLASSES - 1 ) {
		want <<= 1;
		cls++;
	}

	return cls;
}

LOCAL void pool_start( void )
{
	T_CMTX	cmtx;

	if ( p_ready ) {
		return;
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	cmtx.ceilpri = 0;
	p_mtx = tk_cre_mtx(&cmtx);
	p_ready = TRUE;
}

LOCAL void pool_lock( void )
{
	pool_start();
	if ( p_mtx > 0 ) {
		tk_loc_mtx(p_mtx, TMO_FEVR);
	}
}

LOCAL void pool_unlock( void )
{
	if ( p_mtx > 0 ) {
		tk_unl_mtx(p_mtx);
	}
}

EXPORT void *ts_pool_alloc( SZ size )
{
	UINT	cls;
	SZ	want;
	PHEAD	*h;

	if ( size <= 0 ) {
		return NULL;
	}
	/*
	 * The head has to fit in the block with what was asked for. A
	 * block of exactly the largest class would otherwise be rounded
	 * to that class with the head still to go in it, and hand back
	 * sixteen bytes less than was asked for.
	 */
	if ( size + (SZ)sizeof(PHEAD) > (SZ)P_MAX_SIZE ) {
		/* its own pages: too large to be worth a class */
		UD	pages = (UD)( ( size + sizeof(PHEAD) + P_PAGE - 1 )
				      / P_PAGE );

		h = (PHEAD *)knl_vmap(pages, 0);
		if ( h == NULL ) {
			return NULL;
		}
		h->cls = P_CLASSES;
		h->pages = (UW)pages;
		pool_lock();
		p_stat.allocs++;
		p_stat.taken += (SZ)pages * P_PAGE;
		pool_unlock();

		return (void *)( h + 1 );
	}
	cls = class_of(size + sizeof(PHEAD));
	want = class_size(cls);

	pool_lock();
	if ( p_free[cls] != NULL ) {
		h = (PHEAD *)(void *)p_free[cls];
		p_free[cls] = p_free[cls]->next;
	} else {
		if ( p_left < want ) {
			UD	pages = P_CHUNK / P_PAGE;
			UB	*chunk = (UB *)knl_vmap(pages, 0);

			if ( chunk == NULL ) {
				pool_unlock();
				return NULL;
			}
			p_at = chunk;
			p_left = P_CHUNK;
			p_stat.chunks += P_CHUNK;
		}
		h = (PHEAD *)(void *)p_at;
		p_at += want;
		p_left -= want;
	}
	h->cls = cls;
	h->pages = 0;
	p_stat.allocs++;
	p_stat.taken += want;
	pool_unlock();

	return (void *)( h + 1 );
}

EXPORT void ts_pool_free( void *p )
{
	PHEAD	*h;

	if ( p == NULL ) {
		return;
	}
	h = (PHEAD *)p - 1;
	if ( h->cls >= P_CLASSES ) {
		UD	pages = h->pages;

		pool_lock();
		p_stat.frees++;
		p_stat.taken -= (SZ)pages * P_PAGE;
		pool_unlock();
		knl_vunmap((UB *)h, pages);
		return;
	}
	pool_lock();
	p_stat.frees++;
	p_stat.taken -= class_size(h->cls);
	((FREEB *)(void *)h)->next = p_free[h->cls];
	p_free[h->cls] = (FREEB *)(void *)h;
	pool_unlock();
}

EXPORT void *ts_pool_realloc( void *p, SZ size )
{
	PHEAD	*h;
	void	*fresh;
	SZ	had;

	if ( p == NULL ) {
		return ts_pool_alloc(size);
	}
	if ( size <= 0 ) {
		ts_pool_free(p);
		return NULL;
	}
	h = (PHEAD *)p - 1;
	had = ( h->cls >= P_CLASSES )
	      ? (SZ)h->pages * P_PAGE - sizeof(PHEAD)
	      : class_size(h->cls) - sizeof(PHEAD);
	if ( size <= had ) {
		return p;			/* it already fits */
	}
	fresh = ts_pool_alloc(size);
	if ( fresh == NULL ) {
		return NULL;
	}
	knl_memcpy(fresh, p, had);
	ts_pool_free(p);

	return fresh;
}

EXPORT void ts_pool_stat( T_POOLSTAT *st )
{
	if ( st == NULL ) {
		return;
	}
	pool_lock();
	*st = p_stat;
	pool_unlock();
}
