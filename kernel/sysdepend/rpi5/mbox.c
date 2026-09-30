/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mbox.c (Raspberry Pi 5)
 *	The VideoCore firmware's property interface (design 4.10.2, 16.5.1).
 *
 *	One call hands the firmware a buffer of tags and waits until it has
 *	written its answers into the same buffer. The buffer is given by its
 *	address on the VideoCore's bus, which sees ARM physical 0..1GB at
 *	0xC000_0000, so it must lie below 1GB; it is 16 byte aligned because
 *	the low four bits of the message carry the channel. The firmware
 *	reads and writes memory without looking at this processor's caches:
 *	the buffer is cleaned before the call and invalidated after it.
 *
 *	The real time clock and the screen both use it. Calls are one at a
 *	time: the mailbox has one reply register, and an answer taken by the
 *	wrong caller would be lost to the right one.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include "sysdepend.h"

#define MBOX_READ	(VC_MAILBOX_BASE + 0x00)
#define MBOX_STATUS	(VC_MAILBOX_BASE + 0x18)
#define MBOX_WRITE	(VC_MAILBOX_BASE + 0x20)
#define MBOX_FULL	0x80000000U
#define MBOX_EMPTY	0x40000000U
#define MBOX_CH_PROP	8

#define VC_BUS_BASE	0xC0000000U		/* VC view of ARM physical 0..1GB */
#define VC_BUS_LIMIT	0x40000000ULL

/*
 * Polling iterations. Setting a display mode keeps the firmware busy
 * for far longer than reading the clock does, so the wait for the
 * answer is given more room than the wait to send.
 */
#define MBOX_SEND_TMO	1000000
#define MBOX_REPLY_TMO	100000000

LOCAL T_SPLOCK	mbox_lock;

LOCAL void dcache_clean_inval( void *p, UD len )
{
	UBINT	a = (UBINT)p & ~63ULL, end = (UBINT)p + len;

	for ( ; a < end; a += 64 ) {
		Asm("dc civac, %0" :: "r"(a) : "memory");
	}
	Asm("dsb sy");
}

/*
 * Take the mailbox. A task that finds it in use gives up the processor
 * rather than spinning against a caller that may be waiting for the
 * firmware for some milliseconds; where it cannot wait (before the
 * kernel runs tasks), no one else can hold it.
 */
LOCAL void mbox_take( void )
{
	while ( !SpinTryLock(&mbox_lock) ) {
		if ( tk_dly_tsk(1) != E_OK ) {
			Asm("yield");
		}
	}
}

/*
 * One property call on 'buf', whose first word is its size in bytes.
 * Answers E_OK when the firmware answered with the success code
 * (0x80000000) in word 1; whether each tag was answered is in its own
 * response word, which the caller looks at.
 */
EXPORT ER knl_mbox_property( UW *buf )
{
	UD	pa = VA2PA((UBINT)buf);
	UW	msg;
	INT	i;
	ER	er = E_OK;

	if ( pa >= VC_BUS_LIMIT || ( pa & 15 ) != 0 ) {
		return E_NOSPT;
	}
	msg = (UW)pa | VC_BUS_BASE | MBOX_CH_PROP;

	mbox_take();
	dcache_clean_inval(buf, buf[0]);

	for ( i = 0; in_w(MBOX_STATUS) & MBOX_FULL; i++ ) {
		if ( i >= MBOX_SEND_TMO ) {
			er = E_TMOUT;
			goto out;
		}
	}
	out_w(MBOX_WRITE, msg);

	for (;;) {
		for ( i = 0; in_w(MBOX_STATUS) & MBOX_EMPTY; i++ ) {
			if ( i >= MBOX_REPLY_TMO ) {
				er = E_TMOUT;
				goto out;
			}
		}
		if ( in_w(MBOX_READ) == msg ) break;
	}

	dcache_clean_inval(buf, buf[0]);
	er = ( buf[1] == 0x80000000U ) ? E_OK : E_IO;
out:
	SpinUnlock(&mbox_lock);
	return er;
}

/*
 * One value the firmware tells: a tag whose request is one word (which
 * sensor, which clock) and whose answer is that word and the value --
 * the temperature (0x00030006, thousandths of a degree Celsius), a
 * clock's rate (0x00030002, Hz). E_IO when the tag was not answered.
 * The message is one of this file's, taken one caller at a time.
 */
LOCAL UW	fw_buf[16] __attribute__((aligned(64)));
LOCAL T_SPLOCK	fw_lock;

EXPORT ER knl_fw_value( UW tag, UW id, UW *p_val )
{
	ER	er;

	while ( !SpinTryLock(&fw_lock) ) {
		if ( tk_dly_tsk(1) != E_OK ) {
			Asm("yield");
		}
	}
	knl_memset(fw_buf, 0, sizeof(fw_buf));
	fw_buf[0] = 8 * 4;
	fw_buf[1] = 0;			/* a request */
	fw_buf[2] = tag;
	fw_buf[3] = 8;			/* room for two words */
	fw_buf[4] = 4;			/* one word asked with */
	fw_buf[5] = id;
	fw_buf[6] = 0;
	fw_buf[7] = 0;			/* the end tag */
	er = knl_mbox_property(fw_buf);
	if ( er >= E_OK ) {
		if ( ( fw_buf[4] & 0x80000000U ) == 0 ) {
			er = E_IO;
		} else if ( p_val != NULL ) {
			*p_val = fw_buf[6];
		}
	}
	SpinUnlock(&fw_lock);
	return er;
}

#endif /* RPI5 */
