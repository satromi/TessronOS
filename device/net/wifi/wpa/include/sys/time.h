/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/time.h
 *	<sys/time.h>: struct timeval
 */

#ifndef __TSWPA_SYS_TIME_H__
#define __TSWPA_SYS_TIME_H__

#include <sys/types.h>

struct timeval {
	time_t		tv_sec;
	suseconds_t	tv_usec;
};

#endif
