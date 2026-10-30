/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_pcode.c (x64 PC)
 *	The mailbox to the power controller (pcode) of Intel's integrated
 *	graphics of the 9th generation, shared by the display and the
 *	drawing drivers: both reach it only through knl_igt_pcode_rw, which
 *	keeps one request at a time across every processor.
 *
 *	A request is the command and its parameter written to the mailbox
 *	(0x138124, its bit 31 saying the controller is busy with it) and
 *	its two data registers (0x138128, 0x13812c), and the answer read
 *	back from the first data register once bit 31 has fallen. The low
 *	byte of the mailbox then holds an error code, 0 when it went well.
 *
 *	It works whether or not either driver has started: the registers are
 *	reached through the device window over the first BAR of the
 *	graphics function, found here the first time.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>

#define PCODE_MAILBOX	0x138124
#define PCODE_DATA	0x138128
#define PCODE_DATA1	0x13812c
#define PCODE_BUSY	0x80000000U
#define PCODE_WAIT_US	20000		/* the longest a request is waited for */

LOCAL T_SPLOCK	pc_lock;		/* zero is a free ticket lock */
LOCAL UBINT	pc_mmio = 0;

LOCAL UBINT mmio_of( void )
{
	T_PCIDEV	d;
	INT		i;

	if ( pc_mmio != 0 ) {
		return pc_mmio;
	}
	for ( i = 0; ts_pcie_find(0x03, 0x00, 0x00, i, &d) >= E_OK; i++ ) {
		if ( d.vendor == 0x8086 && d.bar[0] != 0 && d.bar_size[0] >= 16 * 1024 * 1024 ) {
			pc_mmio = (UBINT)DEV_BASE(d.bar[0]);
			break;
		}
	}
	return pc_mmio;
}

LOCAL UW rd( UBINT m, UW off )
{
	return *(volatile UW *)( m + off );
}

LOCAL void wr( UBINT m, UW off, UW v )
{
	*(volatile UW *)( m + off ) = v;
	DeviceBarrier();
}

/* Bit 31 of the mailbox fallen within the limit */
LOCAL BOOL idle( UBINT m )
{
	UW	us;

	for ( us = 0; us < PCODE_WAIT_US; us += 10 ) {
		if ( ( rd(m, PCODE_MAILBOX) & PCODE_BUSY ) == 0 ) {
			return TRUE;
		}
		WaitUsec(10);
	}
	return FALSE;
}

/*
 * One request: cmd (the mailbox's low bits, the command and what it
 * takes), in written to the first data register and 0 to the second;
 * the first data register after it in *out. E_OK, E_NOEXS without an
 * Intel graphics function, E_TMOUT when the controller stayed busy
 * before or after, E_IO with the error code it answered.
 */
EXPORT ER knl_igt_pcode_rw( UW cmd, UW in, UW *out )
{
	UBINT	m = mmio_of();
	UINT	intsts;
	UW	box;
	ER	er = E_OK;

	if ( m == 0 ) {
		return E_NOEXS;
	}
	ISpinLock(&pc_lock, &intsts);
	if ( !idle(m) ) {
		er = E_TMOUT;
	} else {
		wr(m, PCODE_DATA, in);
		wr(m, PCODE_DATA1, 0);
		wr(m, PCODE_MAILBOX, PCODE_BUSY | ( cmd & 0x7fffffffU ));
		if ( !idle(m) ) {
			er = E_TMOUT;
		} else {
			box = rd(m, PCODE_MAILBOX);
			if ( out != NULL ) *out = rd(m, PCODE_DATA);
			if ( ( box & 0xff ) != 0 ) er = E_IO;
		}
	}
	ISpinUnlock(&pc_lock, &intsts);
	return er;
}

#endif /* X64 */
