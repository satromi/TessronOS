/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	app_main.c
 *	Phase 1 bring-up application: two tasks, delays, semaphore handshake.
 *	usermain() returns after the test, which shuts the system down.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

LOCAL ID	sem_id;
LOCAL ID	tskA_id, tskB_id;

LOCAL void tskA( INT stacd, void *exinf )
{
	INT	i;

	for ( i = 0; i < 3; i++ ) {
		tm_printf((UB*)"tskA: %d\n", i);
		tk_dly_tsk(200);
	}
	tk_sig_sem(sem_id, 1);
	tk_ext_tsk();
}

LOCAL void tskB( INT stacd, void *exinf )
{
	INT	i;

	for ( i = 0; i < 3; i++ ) {
		tm_printf((UB*)"tskB: %d\n", i);
		tk_dly_tsk(300);
	}
	tk_sig_sem(sem_id, 1);
	tk_ext_tsk();
}

#if USE_KTEST
IMPORT INT ktest_main( void );
#endif
#ifdef USE_DESKTOP
IMPORT INT ts_desktop( void );
#endif

EXPORT INT usermain( void )
{
#if USE_KTEST
	return ktest_main();
#elif defined(USE_DESKTOP)
	/*
	 * What the machine comes up as: the ground, the opening cabinet,
	 * and a loop that reads what the person does. It does not return
	 * until the desktop is done with.
	 */
	return ts_desktop();
#else
	T_CTSK	ctsk;
	T_CSEM	csem;
	SYSTIM	tim;
	ER	err;

	tm_printf((UB*)"TessronOS usermain: start\n");

	csem.exinf  = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem = 2;
	sem_id = tk_cre_sem(&csem);

	ctsk.exinf  = NULL;
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.stksz  = 4096;
	ctsk.itskpri = 10;
	ctsk.task   = (FP)tskA;
	tskA_id = tk_cre_tsk(&ctsk);
	ctsk.task   = (FP)tskB;
	ctsk.itskpri = 11;
	tskB_id = tk_cre_tsk(&ctsk);

	tk_sta_tsk(tskA_id, 0);
	tk_sta_tsk(tskB_id, 0);

	err = tk_wai_sem(sem_id, 2, 5000);
	tk_get_tim(&tim);
	tm_printf((UB*)"TessronOS usermain: done (err=%d, systim=%x:%08x ms)\n", err, tim.hi, tim.lo);

	return 0;
#endif /* USE_KTEST */
}
