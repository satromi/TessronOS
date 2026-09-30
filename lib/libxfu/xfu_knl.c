/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_knl.c
 *	What lib/libxfu needs of the system, in the kernel
 *
 *	Memory from the kernel's allocator, pictures read by the kernel's
 *	decoders (include/ts/img.h), identities from ts_gen_uuid. A process
 *	links a file of its own in place of this one.
 */

#include <tk/tkernel.h>
#include <ts/img.h>
#include "xfu_in.h"

EXPORT void *xfu_sys_alloc( SZ size )
{
	return Kmalloc(( size > 0 ) ? size : 1);
}

EXPORT void xfu_sys_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

EXPORT ER xfu_sys_decode( CONST UB *data, SZ size, UINT **p_px, INT *p_w, INT *p_h )
{
	UW	*px = NULL;
	ER	er;

	er = img_decode(data, size, &px, p_w, p_h);
	if ( er < E_OK ) {
		return ( er == E_PAR ) ? E_NOSPT : er;
	}
	*p_px = (UINT *)px;			/* 32 bits a pixel in the kernel */
	return E_OK;
}

EXPORT ER xfu_sys_uuid( TS_UUID *p_uuid )
{
	return ts_gen_uuid(p_uuid);
}
