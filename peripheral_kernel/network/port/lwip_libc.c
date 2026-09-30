/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lwip_libc.c
 *	What lwIP asks of its C library besides the string functions
 *	(design 12.6): errno and random numbers. The string functions are
 *	the kernel's (peripheral_kernel/libc/libc.c), shared with the other
 *	vendored code.
 */

#include <tk/tkernel.h>
#include "tstdlib.h"
#include <ts/uuid.h>
#include <string.h>
#include <stdlib.h>

/*
 * lwIP follows BSD inside the socket layer and leaves the reason for a
 * failure here. so_api.c reads it back right after the call that set
 * it, so one for the whole system is enough until there is a per-task
 * place to put it.
 */
EXPORT int errno = 0;

/*
 * The stack asks for randomness when it picks port numbers and initial
 * sequence numbers.
 */
EXPORT unsigned int ts_lwip_rand( void )
{
	UW	v = 0;

	ts_get_random(&v, sizeof(v));

	return (unsigned int)v;
}
