/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	psx_stub.S
 *	The system calls of a POSIX program (include/ts/psx.h and the POSIX
 *	part of include/ts/fs.h, design 5.8), beside lib/libts/svc_stub.S:
 *	starting a program with descriptors, groups, signals, waiting for
 *	children, and the descriptors' own calls. A program that does not
 *	use lib/libposix (the console) may link this file alone.
 */

#include <ts/svc.h>

/*
 * The stub of one call. Another architecture's file (lib/libposix/
 * x86_64/psx_stub.S) defines its own SVCSTUB and SVCSTUB_ARCH and
 * includes this one for the list.
 */
#ifndef SVCSTUB_ARCH
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
#endif

	SVCSTUB	ts_spn_prc,	TSN_TS_SPN_PRC
	SVCSTUB	ts_get_inh,	TSN_TS_GET_INH
	SVCSTUB	ts_wai_psx,	TSN_TS_WAI_PSX
	SVCSTUB	ts_kil_prc,	TSN_TS_KIL_PRC
	SVCSTUB	ts_sig_set,	TSN_TS_SIG_SET
	SVCSTUB	ts_sig_ret,	TSN_TS_SIG_RET
	SVCSTUB	ts_sig_sus,	TSN_TS_SIG_SUS
	SVCSTUB	ts_slp_psx,	TSN_TS_SLP_PSX
	SVCSTUB	ts_sig_alm,	TSN_TS_SIG_ALM
	SVCSTUB	ts_set_pgid,	TSN_TS_SET_PGID
	SVCSTUB	ts_get_pgid,	TSN_TS_GET_PGID
	SVCSTUB	ts_set_sid,	TSN_TS_SET_SID
	SVCSTUB	ts_get_sid,	TSN_TS_GET_SID
	SVCSTUB	ts_get_ppid,	TSN_TS_GET_PPID
	SVCSTUB	ts_ref_psx,	TSN_TS_REF_PSX
	SVCSTUB	ts_lst_prc,	TSN_TS_LST_PRC
	SVCSTUB	ts_spn_obj,	TSN_TS_SPN_OBJ
	SVCSTUB	ts_lkp_obj,	TSN_TS_LKP_OBJ

	SVCSTUB	fs_dup,		TSN_FS_DUP
	SVCSTUB	fs_pipe,	TSN_FS_PIPE
	SVCSTUB	fs_xstat,	TSN_FS_XSTAT
	SVCSTUB	fs_fxstat,	TSN_FS_FXSTAT
	SVCSTUB	fs_ioctl,	TSN_FS_IOCTL
	SVCSTUB	fs_fcntl,	TSN_FS_FCNTL
	SVCSTUB	fs_poll,	TSN_FS_POLL
	SVCSTUB	fs_link,	TSN_FS_LINK
	SVCSTUB	fs_chmod,	TSN_FS_CHMOD
