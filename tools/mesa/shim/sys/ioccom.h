/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/ioccom.h (probe)
 *	The encoding of request numbers: bits 7:0 the number, 15:8 the
 *	type, 29:16 the size of the argument, 31:30 its direction.
 */

#ifndef TS_PROBE_SYS_IOCCOM_H
#define TS_PROBE_SYS_IOCCOM_H

#define _IOC_NONE	0U
#define _IOC_WRITE	1U
#define _IOC_READ	2U

#define _IOC(dir, type, nr, size) \
	( ( (unsigned long)(dir) << 30 ) | ( (unsigned long)(size) << 16 ) | \
	  ( (unsigned long)(type) << 8 ) | (unsigned long)(nr) )

#define _IO(type, nr)		_IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t)	_IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t)	_IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t)	_IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))

#define _IOC_NR(r)		( (r) & 0xFF )
#define _IOC_TYPE(r)		( ( (r) >> 8 ) & 0xFF )
#define _IOC_SIZE(r)		( ( (r) >> 16 ) & 0x3FFF )
#define _IOC_DIR(r)		( ( (r) >> 30 ) & 3 )

#endif /* TS_PROBE_SYS_IOCCOM_H */
