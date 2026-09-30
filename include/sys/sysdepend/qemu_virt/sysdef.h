/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	System dependencies definition (QEMU virt board)
 *	Included also from assembler program.
 */

#ifndef __SYS_SYSDEF_DEPEND_H__
#define __SYS_SYSDEF_DEPEND_H__

#include "../cpu/qemu_virt/sysdef.h"

/* Low-power mode: number of consecutive idle counts before entering (unused) */
#define LOWPOW_LIMIT		0x7fff

#endif /* __SYS_SYSDEF_DEPEND_H__ */
