/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ftsystem.c
 *	Memory and streams for FreeType inside TessronOS (design 16.6)
 *
 *	This takes the place of the upstream file of the same name, which
 *	uses malloc and fopen. Memory comes from the small-block pool
 *	rather than the kernel's own allocator: making one letter takes
 *	and gives back hundreds of small blocks, and the kernel's
 *	allocator searches one list from the front with interrupts off,
 *	which turns that into a walk down every block the system has out; streams are refused, because a font is only ever opened
 *	from a block of bytes somebody else has read
 *	(FT_New_Memory_Face), and a call that cannot happen should say so
 *	rather than appear to work.
 *
 *	The size of a block is kept in front of it, because the kernel's
 *	allocator does not answer how large a block is and reallocating
 *	needs to know how much to copy.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>

#include <ft2build.h>
#include <freetype/internal/ftobjs.h>
#include <freetype/internal/ftstream.h>
#include <freetype/ftsystem.h>
#include <freetype/fterrors.h>
#include <freetype/fttypes.h>
#include <ts/pool.h>

/* How much is put in front of every block to remember its size */
#define HDR	( (SZ)sizeof(SZ) )

LOCAL void *ts_ft_alloc( FT_Memory memory, long size )
{
	UB	*p;

	(void)memory;
	if ( size <= 0 ) {
		return NULL;
	}
	p = (UB *)ts_pool_alloc((SZ)size + HDR);
	if ( p == NULL ) {
		return NULL;
	}
	*(SZ *)p = (SZ)size;

	return p + HDR;
}

LOCAL void ts_ft_free( FT_Memory memory, void *block )
{
	(void)memory;
	if ( block != NULL ) {
		ts_pool_free((UB *)block - HDR);
	}
}

LOCAL void *ts_ft_realloc( FT_Memory memory, long cur_size, long new_size,
			   void *block )
{
	UB	*fresh;
	SZ	keep;

	(void)cur_size;
	if ( block == NULL ) {
		return ts_ft_alloc(memory, new_size);
	}
	if ( new_size <= 0 ) {
		ts_ft_free(memory, block);
		return NULL;
	}
	fresh = (UB *)ts_ft_alloc(memory, new_size);
	if ( fresh == NULL ) {
		return NULL;
	}
	keep = *(SZ *)((UB *)block - HDR);
	if ( keep > (SZ)new_size ) {
		keep = (SZ)new_size;
	}
	knl_memcpy(fresh, block, keep);
	ts_ft_free(memory, block);

	return fresh;
}

FT_BASE_DEF( FT_Memory ) FT_New_Memory( void )
{
	FT_Memory	memory;

	memory = (FT_Memory)Kmalloc(sizeof(*memory));
	if ( memory == NULL ) {
		return NULL;
	}
	memory->user    = NULL;
	memory->alloc   = ts_ft_alloc;
	memory->realloc = ts_ft_realloc;
	memory->free    = ts_ft_free;

	return memory;
}

FT_BASE_DEF( void ) FT_Done_Memory( FT_Memory memory )
{
	if ( memory != NULL ) {
		Kfree(memory);
	}
}

/*
 * Streams. A font is opened from memory, so this is never reached on the
 * live path; it answers "cannot be opened" rather than pretending.
 */
FT_BASE_DEF( FT_Error ) FT_Stream_Open( FT_Stream stream, const char *filepathname )
{
	(void)stream;
	(void)filepathname;

	return FT_Err_Cannot_Open_Resource;
}
