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
 *	Each port is a device of its own, "sera", "serb", ... "serz", "seraa"
 *	on, as many as the machine has, with no
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

/*
 * The serial ports for a process (SVC class 11, device/ser/ser_svc.c).
 * sr_lst_prt lists the ports, up to max of them into list, and gives
 * their count. sr_opn_prt opens a port by its name and gives the
 * process's number for it; what a process opened closes when it ends.
 * sr_rea_prt gives what has come in, up to size bytes, waiting up to tmo
 * for the first (0 when none came); sr_wri_prt sends every byte before
 * it returns.
 */
typedef struct {
	UB	name[8];		/* the device's name: "sera", ... */
	UB	label[SER_LABEL_MAX];	/* what the port is, UTF-8 */
	BOOL	console;		/* the console's port */
	UW	speed;			/* bits per second */
} T_SRPORT;

IMPORT INT sr_lst_prt( T_SRPORT *list, INT max );
IMPORT INT sr_opn_prt( CONST UB *name );
IMPORT ER  sr_cls_prt( INT pd );
IMPORT INT sr_rea_prt( INT pd, void *buf, INT size, TMO tmo );
IMPORT INT sr_wri_prt( INT pd, CONST void *buf, INT size );
IMPORT ER  sr_set_spd( INT pd, UW speed );

/* The ports a process opened, closed as it ends (proc.c) */
IMPORT void knl_sr_prc_end( ID pid );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SER_H__ */
