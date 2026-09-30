/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svc_stub.S
 *	System call stubs of the user library (design 9.7).
 *
 *	Every call is "svc #0" with the function code in x8 and up to six
 *	arguments in x0-x5. The full libts will be generated from the one
 *	table in include/ts/svc.h; these are the calls this program needs.
 */

#include <ts/svc.h>

	.text
	.balign	4

	.macro	SVCSTUB name, fncd
	.global	\name
	.type	\name, %function
\name:
	mov	x8, #(\fncd & 0xffff)
	movk	x8, #(\fncd >> 16), lsl #16
	svc	#0
	ret
	.size	\name, . - \name
	.endm

	SVCSTUB	ts_ext_prc,	TSN_TS_EXT_PRC
	SVCSTUB	ts_get_pid,	TSN_TS_GET_PID
	SVCSTUB	ts_get_mono,	TSN_TS_GET_MONO
	SVCSTUB	tk_get_tid,	TSN_TK_GET_TID
	SVCSTUB	tk_dly_tsk,	TSN_TK_DLY_TSK
	SVCSTUB	tm_putstring,	TSN_TM_PUTSTRING
	SVCSTUB	ts_rcv_msg,	TSN_TS_RCV_MSG
	SVCSTUB	ts_snd_msg,	TSN_TS_SND_MSG
