/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rng_hw.c (QEMU virt)
 *	The machine has no always present hardware generator, so the
 *	software generator of rng.c is used on its own.
 */

#include <sys/machine.h>

#ifdef QEMU_VIRT

#include "kernel.h"
#include <ts/uuid.h>

EXPORT ER knl_hwrng_read( UW *p_val )
{
	return E_NOSPT;
}

#endif /* QEMU_VIRT */
