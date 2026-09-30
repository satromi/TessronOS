/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtc.c (QEMU virt)
 *	Real time clock: PL031 at PL031_RTC_PA. RTCDR holds UNIX seconds;
 *	RTCLR loads a new value.
 */

#include <sys/machine.h>

#ifdef QEMU_VIRT

#include "kernel.h"
#include "sysdepend.h"

#define PL031_RTCDR	(PL031_RTC_BASE + 0x00)		/* data (read) */
#define PL031_RTCLR	(PL031_RTC_BASE + 0x08)		/* load (write) */
#define PL031_RTCCR	(PL031_RTC_BASE + 0x0c)		/* control: bit0 start */

EXPORT ER knl_rtc_read( UD *p_sec )
{
	*p_sec = in_w(PL031_RTCDR);
	return E_OK;
}

EXPORT ER knl_rtc_write( UD sec )
{
	if ( sec > 0xFFFFFFFFULL ) {
		return E_PAR;
	}
	out_w(PL031_RTCLR, (UW)sec);
	out_w(PL031_RTCCR, 1);
	return E_OK;
}

#endif /* QEMU_VIRT */
