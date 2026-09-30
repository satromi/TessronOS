/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rng_hw.c (Raspberry Pi 5)
 *	Hardware random generator, iproc-rng200 family (design 4.10.3).
 *
 *	Not yet verified on hardware (HW-13).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <ts/uuid.h>
#include "sysdepend.h"

#define RNG_CTRL		(RNG_BASE + 0x00)
#define RNG_SOFT_RESET		(RNG_BASE + 0x04)
#define RNG_RBG_SOFT_RESET	(RNG_BASE + 0x08)
#define RNG_INT_STATUS		(RNG_BASE + 0x18)
#define RNG_FIFO_DATA		(RNG_BASE + 0x20)
#define RNG_FIFO_COUNT		(RNG_BASE + 0x24)

#define RNG_CTRL_RBGEN		0x00000001
#define RNG_CTRL_MASK		0x00001FFF

#define INT_MASTER_FAIL		0x80000000
#define INT_NIST_FAIL		0x00000020

#define RNG_SPIN		10000		/* polling attempts */

LOCAL BOOL	rng_on = FALSE;

LOCAL void rng_restart( void )
{
	out_w(RNG_SOFT_RESET, 1);
	out_w(RNG_SOFT_RESET, 0);
	out_w(RNG_RBG_SOFT_RESET, 1);
	out_w(RNG_RBG_SOFT_RESET, 0);
	out_w(RNG_INT_STATUS, INT_MASTER_FAIL | INT_NIST_FAIL);
	out_w(RNG_CTRL, (in_w(RNG_CTRL) & ~RNG_CTRL_MASK) | RNG_CTRL_RBGEN);
}

EXPORT ER knl_hwrng_read( UW *p_val )
{
	INT	i;

	if ( p_val == NULL ) {
		return E_PAR;
	}
	if ( !rng_on ) {
		rng_restart();
		rng_on = TRUE;
	}
	if ( (in_w(RNG_INT_STATUS) & (INT_MASTER_FAIL | INT_NIST_FAIL)) != 0 ) {
		rng_restart();
		return E_IO;
	}
	for ( i = 0; i < RNG_SPIN; i++ ) {
		if ( (in_w(RNG_FIFO_COUNT) & 0xff) != 0 ) {
			*p_val = in_w(RNG_FIFO_DATA);
			return E_OK;
		}
	}

	return E_TMOUT;
}

#endif /* RPI5 */
