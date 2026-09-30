/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdepend.h
 *	System-dependent definition (rpi5)
 */

#ifndef _SYSDEPEND_TARGET_SYSDEPEND_
#define _SYSDEPEND_TARGET_SYSDEPEND_

#include "../cpu/bcm2712/sysdepend.h"

/*
 * The PCI Express root complex the RP1 hangs on (kernel/sysdepend/rpi5/
 * pcie_rc.c). Brings the link up when the firmware left it in reset and
 * finds what is on it; answers how many functions were found, or 0 when
 * the link is down.
 */
IMPORT INT knl_pcie_rc_init( void );

/*
 * One call of the firmware's property interface (kernel/sysdepend/rpi5/
 * mbox.c). 'buf' is the whole message, its size in bytes in word 0; it
 * lies below 1GB physical and is 16 byte aligned. Answers E_OK when the
 * firmware accepted the message, E_TMOUT when it did not answer.
 */
IMPORT ER knl_mbox_property( UW *buf );

/* One value the firmware tells for a tag asked with one word (mbox.c) */
IMPORT ER knl_fw_value( UW tag, UW id, UW *p_val );

#endif /* _SYSDEPEND_TARGET_SYSDEPEND_ */
