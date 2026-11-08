/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07
 *
 *----------------------------------------------------------------------
 */

/*
 *	inittask.c
 *	Initial task definition
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <sys/inittask.h>

#include "kernel.h"
#include <ts/lockup.h>

#if !USE_IMALLOC
INT	init_task_stack[INITTASK_STKSZ/sizeof(INT)];
#endif

typedef INT	(*MAIN_FP)(INT, UB **);

/*
 * Initial task creation parameter
 */

LOCAL void init_task_main(void);

EXPORT const T_CTSK knl_init_ctsk = {
	.exinf		= (void *)INITTASK_EXINF,
	.tskatr		= (ATR)INITTASK_TSKATR,
	.task		= (FP)&init_task_main,
	.itskpri	= (PRI)INITTASK_ITSKPRI,
	.stksz		= (SZ)INITTASK_STKSZ,

#if TRUSTZONE_SCALL
	.tzstksz	= (SZ)INITTASK_STKSZ_SEC,
#endif

#if USE_OBJECT_NAME
	.dsname		= INITTASK_DSNAME,
#endif
	.bufptr		= (void*)INITTASK_STACK,
};

/* --------------------------------------------------------------- */
/*
 * Start System
 *	Start each subsystem and each device driver.
 *	Return from function after starting.
 */
LOCAL ER start_system( void )
{
	ER	ercd;

#if USE_DEVICE
	/* Initialize Device manager */
	ercd = knl_initialize_devmgr();
	if ( ercd < E_OK ) return ercd;
#endif

	/* Start system dependent sequence */
	ercd = knl_start_device();

	return ercd;
}

/*
 * Stop System
 *	Never return from this function.
 *
 *	fin  =	 0 : Power off
 *		-1 : reset and re-start	(Reset -> Boot -> Start)
 *		-2 : fast re-start		(Start)
 *		-3 : Normal re-start		(Boot -> Start)
 *
 *	fin are not always supported.
 */
#if USE_SHUTDOWN
/*
 * The machine going off is not left to what it waits for: a disk that no
 * longer answers, a lock never given back. A task above the others looks
 * at the time; past it, it says so and turns the machine off or starts it
 * again as asked, without the steps that remain. What was committed is in
 * the volumes' logs, and the next start puts it back as after a cut in the
 * power.
 */
IMPORT void knl_bootlog_soon( void ) __attribute__((weak));

LOCAL volatile UW	off_at = 0;
LOCAL volatile INT	off_fin = 0;
LOCAL BOOL		off_started = FALSE;

LOCAL UW off_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

LOCAL void off_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	while ( off_at == 0 || (INT)( off_now() - off_at ) < 0 ) {
		tk_dly_tsk(200);
	}
	tm_printf((UB *)"shutdown: NOT FINISHED IN TIME: %s without the steps that remain\n",
		  ( off_fin < 0 ) ? "starting again" : "turning off");
	if ( knl_bootlog_soon != NULL ) {
		knl_bootlog_soon();
		tk_dly_tsk(1000);
	}
	if ( off_fin < 0 ) {
		knl_restart_hw(off_fin);
	}
	knl_tkernel_exit();
}

EXPORT void knl_off_limit( UW ms, INT fin )
{
	off_fin = fin;
	if ( ms == 0 ) {
		off_at = 0;		/* the going off was cancelled */
		return;
	}
	off_at = off_now() + ms;
	if ( !off_started ) {
		T_CTSK	c;
		ID	t;

		knl_memset(&c, 0, sizeof(c));
		c.tskatr = TA_HLNG | TA_RNG0;
		c.task = (FP)off_task;
		c.itskpri = 2;
		c.stksz = 64 * 1024;
		t = tk_cre_tsk(&c);
		if ( t > 0 && tk_sta_tsk(t, 0) >= E_OK ) {
			off_started = TRUE;
		}
	}
}
#else
EXPORT void knl_off_limit( UW ms, INT fin )
{
	(void)ms;
	(void)fin;
}
#endif /* USE_SHUTDOWN */

/* From the last steps of the machine on: time for the volumes and the devices */
#define OFF_LAST_MS	90000

LOCAL void shutdown_system( INT fin )
{
	/* no more looking for a stopped machine, and no watchdog to reset it while it goes down */
	knl_lockup_stop();
	knl_off_limit(OFF_LAST_MS, fin);

#if USE_SHUTDOWN
	/* Platform dependent finalize sequence */
	knl_finish_device();

	/* Shutdown message output */
	if ( fin >= 0 ) {
		SYSTEM_MESSAGE("\n<< SYSTEM SHUTDOWN >>\n");
	}

	if ( fin < 0 ) {
		/* Re-start sequence (platform dependent) */
		knl_restart_hw(fin);
	}

	knl_tkernel_exit();		/* Stop system */
#else
	DISABLE_INTERRUPT;
	for(;;) {
		;
	}
#endif /* USE_SHUTDOWN */
}


/*
 * Initial task main
 */
LOCAL void init_task_main(void)
{
	INT	fin = 1;
	ER	ercd;

	ercd = start_system();		/* Start Sub-system & device driver */
	if(ercd  >= E_OK) {

#if (USE_SYSTEM_MESSAGE && USE_TMONITOR)
		tm_printf((UB*)"\n\nmicroT-Kernel Version %x.%02x\n\n", VER_MAJOR, VER_MINOR);
#endif

#if USE_USERINIT
		/* Perform user defined initialization sequence */
		fin = (*(MAIN_FP)RI_USERINIT)(0, NULL);
#endif
		if ( fin > 0 ) {
			knl_lockup_arm();	/* every processor watched from now on (lockup.c) */
			fin = usermain();	/* User Main Program */
		}
#if USE_USERINIT
		/* Perform user defined finalization sequence */
		(*(MAIN_FP)RI_USERINIT)(-1, NULL);
#endif

	} else {
		SYSTEM_MESSAGE("!ERROR! Init Task start\n");	/* Start message */
	}

	shutdown_system(fin);	/* Never return */
}

