/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obrand.c
 *	The random source as an object, 乱数 (design 18.7)
 *
 *	A stream device of fixed UUID (ob_uuid_random). Record 1 read gives
 *	as many random bytes as were asked for, from the generator of the
 *	kernel (sysman/rng.c); written, the bytes are stirred into it. It
 *	is everyone's to read and the system's and the administrators' to
 *	stir (rw-r--r--). A program keeps a key open to it and reads it
 *	whenever it needs bytes: the libraries' ts_get_random, getrandom and
 *	/dev/urandom, and the entropy of TLS all come here.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/uuid.h>
#include "obj.h"

LOCAL INT rand_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>Record 1: read, random bytes, as many as are asked for; "
			  "written, what is written is stirred into the generator.</p>");
}

LOCAL INT rand_attr( void *ctx, UB *j, INT n, INT max )
{
	UW	w;

	(void)ctx;
	n = knl_oj_put(j, n, max, ",\"virtual\":true,\"hardware\":");
	return knl_oj_put(j, n, max, ( knl_hwrng_read(&w) == E_OK ) ? "true" : "false");
}

LOCAL ER rand_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	ER	er;

	(void)ctx;
	(void)pos;
	(void)off;				/* a stream: every place gives new bytes */
	if ( recno != OB_RND_DATA ) {
		return E_NOEXS;
	}
	if ( size <= 0 ) {
		if ( p_asize != NULL ) *p_asize = 0;
		return ( size == 0 ) ? E_OK : E_PAR;
	}
	er = ts_get_random(buf, size);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

LOCAL ER rand_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	(void)ctx;
	(void)pos;
	(void)off;
	if ( recno != OB_RND_DATA ) {
		return E_NOEXS;
	}
	if ( size < 0 ) {
		return E_PAR;
	}
	knl_rng_stir(buf, size);
	if ( p_asize != NULL ) *p_asize = size;
	return E_OK;
}

LOCAL CONST T_OBDVOPS rand_ops = {
	OB_S_CHAR, "random", OB_RND_NREC, 0644, 0, 0,
	rand_text, rand_attr, rand_rea, rand_wri, NULL, NULL
};

EXPORT void knl_obrand_start( void )
{
	ER	er = knl_obdev_add((CONST UB *)"乱数", &ob_uuid_random, &rand_ops, NULL, NULL);

	if ( er < E_OK ) {
		tm_printf((UB *)"ob: no random source object (%d)\n", (INT)er);
	}
}
