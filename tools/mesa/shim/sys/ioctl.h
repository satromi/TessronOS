/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/ioctl.h (probe)
 *	Request numbers in the usual encoding (direction, size, type,
 *	number), which the DRM headers build their requests from. There is
 *	no ioctl on TessronOS: the port turns each request into a GPU SVC
 *	(docs/tessronos/13-gpu.md 13.6.2).
 */

#ifndef TS_PROBE_SYS_IOCTL_H
#define TS_PROBE_SYS_IOCTL_H

#include <sys/ioccom.h>

int	ioctl( int fd, unsigned long request, ... );

#endif /* TS_PROBE_SYS_IOCTL_H */
