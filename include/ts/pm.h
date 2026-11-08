/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pm.h
 *	The machine asleep: what a driver gives so that its device stops
 *	before the machine sleeps and starts again after it wakes
 *	(docs/tessronos/03-platform.md 3.10.11). Only the x64 PC sleeps; on
 *	the other machines registering does nothing.
 *
 *	Suspending runs by rising order in one task, resuming by falling
 *	order in the same task, so that a mutex taken in suspend (the way to
 *	hold requests off until the device is back) is let go in resume.
 *	Suspend answers E_OK with the device silent: no DMA, no interrupt.
 *	how says what the device's power does meanwhile: with PM_S3 the
 *	function comes back as from a reset, with PM_S0IDLE it was in D3hot.
 *	The PCI configuration of every function (the header, MSI and MSI-X,
 *	the Express controls) is saved and restored around the sleep by the
 *	framework: before resume runs, the function is in D0 with its
 *	configuration back, and its own registers are the driver's to set.
 */

#ifndef __TS_PM_H__
#define __TS_PM_H__

#include <tk/typedef.h>

#define PM_S3		3	/* suspend to RAM: the processors and the devices lose their state */
#define PM_S0IDLE	0	/* low power S0 idle: the system stays powered, the devices in D3hot */

/* The order of suspending: lower first. Disks are last, so that what the others do can reach them */
#define PM_ORD_CAMERA	10
#define PM_ORD_SOUND	20
#define PM_ORD_NET	30
#define PM_ORD_GPU	40
#define PM_ORD_DISPLAY	50
#define PM_ORD_INPUT	60
#define PM_ORD_USB	70
#define PM_ORD_DISK	80

typedef struct {
	CONST char	*name;
	INT		order;
	ER		(*suspend)( void *arg, INT how );
	void		(*resume)( void *arg, INT how );
	void		*arg;
} T_PMDEV;

#ifdef X64
/* ops stays the caller's: the framework keeps the pointer. E_LIMIT when the table is full */
IMPORT ER	ts_pm_register( CONST T_PMDEV *ops );
#else
#define ts_pm_register( ops )	( (void)(ops), (ER)E_OK )
#endif

#endif /* __TS_PM_H__ */
