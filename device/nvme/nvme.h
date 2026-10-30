/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	nvme.h
 *	What the NVMe driver (nvme.c) gives the rest of the kernel: the
 *	shutdown of the controllers before the machine goes off or starts
 *	again, their counts for the records, and for the tests a command
 *	that is made to be lost.
 */

#ifndef __DEVICE_NVME_H__
#define __DEVICE_NVME_H__

/* The state of a controller (T_NVSTAT.state) */
#define NV_INIT		0	/* being set up: the admin queue's commands only */
#define NV_LIVE		1
#define NV_FAILED	2	/* a command not answered, or the controller failed: being reset */
#define NV_DEAD		3	/* gone from the bus, or not back after a reset */
#define NV_OFF		4	/* shut down (the machine goes off or starts again) */

typedef struct {
	INT	state;		/* NV_* */
	INT	irqkind;	/* PCI_IRQ_* */
	UINT	intno;		/* 0: polled */
	UW	nint;		/* interrupts that arrived */
	UW	ncmd;		/* commands answered */
	UW	nlate;		/* of them, found by the look without their interrupt */
	UW	ntmo;		/* commands not answered in time */
	UW	nreset;		/* resets after a command not answered or a failure */
	UW	blksz;		/* bytes of a logical block (LBADS) */
	UD	capacity;	/* logical blocks of namespace 1 */
	UD	hmb_bytes;	/* the host memory buffer given to the controller, 0 none */
	BOOL	hmb_on;		/* and the controller uses it now */
	UW	shst_ms;	/* the shutdown: how long it took, 0 before it */
} T_NVSTAT;

/* The counts and the state of the controller registered as devnm ("nvmea") */
IMPORT ER	knl_nvme_stat( CONST UB *devnm, T_NVSTAT *st );

/* Each controller's counts said on the console, for the records (before knl_bootlog_final) */
IMPORT void	knl_nvme_report( void );

/*
 * Every controller shut down (NVMe 1.4 7.6.2): no request taken after it,
 * the volatile cache flushed, the I/O queues deleted, the host memory
 * buffer taken back, CC.SHN set to a normal shutdown and CSTS.SHST waited
 * for. After the records are written (knl_finish_device).
 */
IMPORT void	knl_nvme_shutdown( void );

#if USE_KTEST
/*
 * For the tests: the next command on the I/O queue of devnm is written
 * to the queue but its doorbell not rung, so that it is never answered,
 * as a command the controller lost
 */
IMPORT ER	knl_nvme_test_drop( CONST UB *devnm );
#endif

#endif /* __DEVICE_NVME_H__ */
