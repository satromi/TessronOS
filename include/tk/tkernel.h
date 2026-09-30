/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

/*
 *	tkernel.h
 *
 *	T-Kernel Definition
 */

#ifndef __TK_TKERNEL_H__
#define __TK_TKERNEL_H__

#include <sys/machine.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <config.h>

#if USE_STDINC_STDDEF
#include <stddef.h>
#define PROHIBIT_DEF_SIZE_T
#endif

#if USE_STDINC_STDINT
#include <stdint.h>
#endif

#include <tk/typedef.h>
#include <tk/errno.h>

#include <sys/sysdef.h>
#include <sys/profile.h>

#include <tk/cpudef.h>
#include <tk/syscall.h>
#include <tk/syslib.h>
#include <tk/smem.h>
#include <ts/time.h>
#include <ts/blk.h>
#include <ts/fs.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/dt.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/net.h>
#include <ts/gpio.h>

#ifndef offsetof
#define offsetof(type, member) ((SZ)(&((type *)0)->member))
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TK_TKERNEL_H__ */
