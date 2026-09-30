/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_prc.c
 *	What lib/libxfu needs of the system, in a process
 *
 *	Memory from the C library (newlib's malloc), pictures read by the
 *	kernel's decoders through dp_img_decode, identities from
 *	ts_gen_uuid. The kernel writes each pixel in 32 bits, its own UW,
 *	whatever width UW has in the program: the buffer is UINTs.
 */

#include <stdlib.h>
#include <ts/uapp.h>
#include "xfu_in.h"

EXPORT void *xfu_sys_alloc( SZ size )
{
	return malloc(( size > 0 ) ? (size_t)size : 1);
}

EXPORT void xfu_sys_free( void *p )
{
	free(p);
}

EXPORT ER xfu_sys_decode( CONST UB *data, SZ size, UINT **p_px, INT *p_w, INT *p_h )
{
	UINT	*px;
	INT	w = 0, h = 0;
	SZ	n;
	ER	er;

	er = dp_img_decode(data, size, NULL, 0, &w, &h);
	if ( er < E_OK ) {
		return ( er == E_PAR ) ? E_NOSPT : er;
	}
	n = (SZ)w * h;
	if ( w <= 0 || h <= 0 || n > XFU_PIX_MAX ) {
		return E_LIMIT;
	}
	px = (UINT *)malloc((size_t)n * sizeof(UINT));
	if ( px == NULL ) {
		return E_NOMEM;
	}
	er = dp_img_decode(data, size, (UW *)px, n, &w, &h);
	if ( er < E_OK ) {
		free(px);
		return er;
	}
	*p_px = px;
	*p_w = w;
	*p_h = h;
	return E_OK;
}

EXPORT ER xfu_sys_uuid( TS_UUID *p_uuid )
{
	return ts_gen_uuid(p_uuid);
}
