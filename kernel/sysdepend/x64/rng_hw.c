/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rng_hw.c (x64 PC)
 *	The processor's random number generator: RDSEED where there is one
 *	(the entropy source itself), else RDRAND, else none. A failed read
 *	(the generator had nothing ready) is tried a few times.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <ts/uuid.h>

LOCAL INT	rng_kind = -1;		/* -1 not looked yet, 0 none, 1 RDRAND, 2 RDSEED */

EXPORT ER knl_hwrng_read( UW *p_val )
{
	UW	a, b, c, d, v;
	UB	ok;
	INT	i;

	if ( rng_kind < 0 ) {
		cpuid(7, 0, &a, &b, &c, &d);
		rng_kind = ( b & ( 1U << 18 ) ) ? 2 : 0;
		if ( rng_kind == 0 ) {
			cpuid(1, 0, &a, &b, &c, &d);
			rng_kind = ( c & ( 1U << 30 ) ) ? 1 : 0;
		}
	}
	for ( i = 0; i < 10 && rng_kind > 0; i++ ) {
		if ( rng_kind == 2 ) {
			Asm("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
		} else {
			Asm("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		}
		/*
		 * All ones or all zeros with success said is a fault of the
		 * instruction, known on some of AMD's processors: taken as no value.
		 */
		if ( ok && v != 0 && v != 0xffffffffU ) {
			*p_val = v;
			return E_OK;
		}
		if ( i == 4 && rng_kind == 2 ) {
			/* its seed gives nothing: the generator, if it has one */
			cpuid(1, 0, &a, &b, &c, &d);
			rng_kind = ( c & ( 1U << 30 ) ) ? 1 : 0;
		}
	}
	return E_NOSPT;
}

#endif /* X64 */
