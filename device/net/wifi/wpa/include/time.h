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
 *	<time.h>: the time types
 */

#ifndef __TSWPA_TIME_H__
#define __TSWPA_TIME_H__

#include <sys/types.h>

struct timespec {
	time_t	tv_sec;
	long	tv_nsec;
};

#endif
