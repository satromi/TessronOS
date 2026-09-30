/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ser.h
 *	Serial ports as devices (design 10.5)
 *
 *	Each port is a device of its own, "sera", "serb", ..., with no
 *	subunits. Data (start 0) is a stream: a read gives what has come in,
 *	waiting up to the receive time-out for the first byte when nothing
 *	has, and a write sends every byte before it returns. The console's
 *	port is one of them; what the monitor writes goes out on it too.
 */

#ifndef __TS_SER_H__
#define __TS_SER_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TDK_SERIAL	0x0090U		/* the kind a serial port registers as */

#define SER_MAX_PORT	4
#define SER_LABEL_MAX	32

/* Attribute data */
#define TDN_SER_SPEED	(-101)		/* UW: bits per second (RW) */
#define TDN_SER_RCVTMO	(-103)		/* TMO: how long a read waits for its first byte (RW) */
#define TDN_SER_INFO	(-110)		/* T_SERINFO (R) */

typedef struct {
	UB	label[SER_LABEL_MAX];	/* what the port is, UTF-8 */
	BOOL	console;		/* the console's port */
	BOOL	polled;			/* no interrupt: read when asked */
	UW	speed;			/* bits per second */
	UD	lost;			/* bytes that came when the buffer was full */
} T_SERINFO;

IMPORT INT knl_ser_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SER_H__ */
