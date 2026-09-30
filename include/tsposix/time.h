/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	time.h
 *	The C library's time.h with what it leaves out on this machine: the
 *	clocks of processor time (the monotonic clock stands in, nothing is
 *	counted per task) and timegm (lib/libcxxrt/ts_syscalls.c).
 */

#ifndef __TSPOSIX_TIME_H__
#define __TSPOSIX_TIME_H__

#include_next <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID	((clockid_t)2)
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID		((clockid_t)3)
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME			((clockid_t)7)
#endif

time_t	timegm( struct tm *tm );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_TIME_H__ */
