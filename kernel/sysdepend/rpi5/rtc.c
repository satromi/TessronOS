/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rtc.c (Raspberry Pi 5)
 *	Real time clock through the VideoCore firmware mailbox (design 4.10.2).
 *
 *	The RTC has no CPU-visible registers. The property interface
 *	(mailbox channel 8) carries a GET_RTC_REG / SET_RTC_REG tag whose
 *	value is { register, value }; register 0 (RTC_TIME) is UNIX seconds.
 *	The call itself, and what its buffer has to be, is mbox.c's.
 *
 *	Not yet verified on hardware (HW-12).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include "sysdepend.h"

#define TAG_GET_RTC_REG	0x00030087U
#define TAG_SET_RTC_REG	0x00038087U
#define RTC_REG_TIME	0

LOCAL UW	mbox_buf[16] __attribute__((aligned(64)));

LOCAL ER rtc_reg( UW tag, UW reg, UW *val )
{
	UW	*b = mbox_buf;
	ER	er;

	b[0] = 9 * 4;		/* total size */
	b[1] = 0;		/* request */
	b[2] = tag;
	b[3] = 8;		/* value buffer size */
	b[4] = 8;		/* request size */
	b[5] = reg;
	b[6] = *val;
	b[7] = 0;		/* end tag */
	b[8] = 0;

	er = knl_mbox_property(b);
	if ( er == E_OK ) {
		if ( (b[4] & 0x80000000U) == 0 ) {
			er = E_IO;	/* tag not answered */
		} else {
			*val = b[6];
		}
	}
	return er;
}

EXPORT ER knl_rtc_read( UD *p_sec )
{
	UW	val = 0;
	ER	er;

	er = rtc_reg(TAG_GET_RTC_REG, RTC_REG_TIME, &val);
	if ( er == E_OK ) {
		*p_sec = val;
	}
	return er;
}

EXPORT ER knl_rtc_write( UD sec )
{
	UW	val;

	if ( sec > 0xFFFFFFFFULL ) {
		return E_PAR;
	}
	val = (UW)sec;
	return rtc_reg(TAG_SET_RTC_REG, RTC_REG_TIME, &val);
}

#endif /* RPI5 */
