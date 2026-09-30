/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pool.h
 *	Small blocks, taken and given back quickly (design 6.5)
 *
 *	The kernel's own allocator keeps one list of free areas, searches
 *	it from the front for the first area large enough, and does the
 *	whole search with interrupts off. That is the right shape for what
 *	it is for -- a few large, long-lived areas -- and the wrong shape
 *	for what a font library does, which is to take and give back
 *	hundreds of small blocks for every letter it makes. With a few
 *	thousand blocks outstanding, every one of those takes a walk down
 *	the list, and making one letter came to sixty milliseconds.
 *
 *	This is the other shape. Blocks are rounded up to one of a dozen
 *	sizes; each size keeps its own list of blocks already given back,
 *	so taking one and giving it back are both a handful of
 *	instructions and neither depends on how many blocks are out. The
 *	memory behind them is taken from the page allocator a chunk at a
 *	time and never given back, which is what makes it cheap; a pool is
 *	for what a running system uses continuously, not for what it uses
 *	once.
 *
 *	Blocks larger than the largest size go to the page allocator
 *	directly, so a caller need not know where the line is.
 */

#ifndef __TS_POOL_H__
#define __TS_POOL_H__

#ifdef __cplusplus
extern "C" {
#endif

/* A block of at least that many bytes, or NULL */
IMPORT void *ts_pool_alloc( SZ size );

/* Back to the pool. A NULL is nothing to do, as everywhere else. */
IMPORT void  ts_pool_free( void *p );

/* The same block at another size, keeping what is in it */
IMPORT void *ts_pool_realloc( void *p, SZ size );

/* What the pool holds and hands out, for whoever is counting */
typedef struct {
	SZ	taken;			/* bytes handed out just now */
	SZ	chunks;			/* bytes taken from the pages */
	UD	allocs, frees;
} T_POOLSTAT;

IMPORT void  ts_pool_stat( T_POOLSTAT *st );

#ifdef __cplusplus
}
#endif

#endif /* __TS_POOL_H__ */
