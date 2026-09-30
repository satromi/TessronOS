/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_task.c
 *	Task management: create/start/exit/delete, priority, sleep/wakeup,
 *	release wait, suspend/resume, terminate, reference.
 *
 *	Runs in the test task (priority KT_PRI_MAIN). Helpers at KT_PRI_HIGH
 *	preempt it at once; helpers at KT_PRI_LOW run only while it waits.
 */

#include "ktest.h"

LOCAL volatile INT	step;
LOCAL volatile INT	got_stacd;
LOCAL volatile void	*got_exinf;
LOCAL volatile ER	wait_ercd;

LOCAL void t_simple( INT stacd, void *exinf )
{
	got_stacd = stacd;
	got_exinf = exinf;
	step++;
	tk_ext_tsk();
}

LOCAL void t_sleeper( INT stacd, void *exinf )
{
	step = 1;
	wait_ercd = tk_slp_tsk(TMO_FEVR);
	step = 2;
	tk_ext_tsk();
}

LOCAL void t_delay( INT stacd, void *exinf )
{
	step = 1;
	wait_ercd = tk_dly_tsk(50);
	step = 2;
	tk_ext_tsk();
}

LOCAL void t_spin( INT stacd, void *exinf )
{
	while (1) {
		step++;
		tk_dly_tsk(1);
	}
}

LOCAL void t_wakeup_other( INT stacd, void *exinf )
{
	wait_ercd = tk_wup_tsk((ID)stacd);
	tk_ext_tsk();
}

/* create, start with stacd/exinf, exit, state transitions, delete */
LOCAL void test_create_start_exit( void )
{
	ID	id;
	T_RTSK	rtsk;

	step = 0;
	id = kt_cre_tsk((FP)t_simple, KT_PRI_HIGH, (void *)0x1234);
	KT_ASSERT(id > 0);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);

	KT_ASSERT_ER(tk_sta_tsk(id, 77), E_OK);		/* higher priority: runs immediately */
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_EQ(got_stacd, 77);
	KT_ASSERT_EQ((BINT)got_exinf, 0x1234);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);		/* back to dormant after tk_ext_tsk */

	KT_ASSERT_ER(tk_sta_tsk(id, 1), E_OK);		/* can be started again */
	KT_ASSERT_EQ(got_stacd, 1);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_NOEXS);
	KT_ASSERT_ER(tk_del_tsk(id), E_NOEXS);
	KT_ASSERT_ER(tk_sta_tsk(CNF_MAX_TSKID + 1, 0), E_ID);
}

/* lower priority task does not preempt; tk_get_tid; tk_chg_pri */
LOCAL void test_priority( void )
{
	ID	id, self;
	T_RTSK	rtsk;

	self = tk_get_tid();
	KT_ASSERT(self > 0);
	KT_ASSERT_ER(tk_ref_tsk(self, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_RUN);
	KT_ASSERT_EQ(rtsk.tskpri, KT_PRI_MAIN);

	step = 0;
	id = kt_cre_tsk((FP)t_simple, KT_PRI_LOW, NULL);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_EQ(step, 0);				/* not run yet */
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_RDY);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);		/* let it run */
	KT_ASSERT_EQ(step, 1);

	/* a priority changed while dormant applies to the next start (T-Kernel 3.0 behaviour) */
	KT_ASSERT_ER(tk_chg_pri(id, 200), E_PAR);
	KT_ASSERT_ER(tk_chg_pri(id, 3), E_OK);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);		/* now higher than us: runs at once */
	KT_ASSERT_EQ(step, 2);

	/* TPRI_INI restores the initial priority */
	KT_ASSERT_ER(tk_chg_pri(id, TPRI_INI), E_OK);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_EQ(step, 2);				/* back at KT_PRI_LOW: not run yet */
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskpri, KT_PRI_LOW);
	KT_ASSERT_ER(tk_chg_pri(id, 4), E_OK);		/* raised while ready: preempts */
	KT_ASSERT_EQ(step, 3);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

/* tk_slp_tsk / tk_wup_tsk, wakeup queuing, tk_rel_wai, tk_can_wup */
LOCAL void test_sleep_wakeup( void )
{
	ID	id, helper;
	T_RTSK	rtsk;

	step = 0;
	id = kt_cre_tsk((FP)t_sleeper, KT_PRI_HIGH, NULL);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_EQ(step, 1);				/* sleeping */
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_WAI);
	KT_ASSERT_EQ(rtsk.tskwait, TTW_SLP);

	KT_ASSERT_ER(tk_wup_tsk(id), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_OK);
	KT_ASSERT_ER(tk_wup_tsk(id), E_OBJ);		/* dormant */

	/* forced release of the wait */
	step = 0;
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_ER(tk_rel_wai(id), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_RLWAI);
	KT_ASSERT_ER(tk_rel_wai(id), E_OBJ);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);

	/* self: polling and timeout */
	KT_ASSERT_ER(tk_slp_tsk(0), E_TMOUT);
	KT_ASSERT_ER(tk_slp_tsk(20), E_TMOUT);
	KT_ASSERT_ER(tk_wup_tsk(tk_get_tid()), E_OBJ);	/* cannot wake up self */

	/* a wakeup request queued while running is consumed by the next sleep */
	helper = kt_cre_tsk((FP)t_wakeup_other, KT_PRI_HIGH, NULL);
	KT_ASSERT_ER(tk_sta_tsk(helper, tk_get_tid()), E_OK);
	KT_ASSERT_EQ(wait_ercd, E_OK);
	KT_ASSERT_ER(tk_slp_tsk(TMO_FEVR), E_OK);	/* returns at once */
	KT_ASSERT_ER(tk_slp_tsk(0), E_TMOUT);		/* request consumed */

	/* tk_can_wup cancels the queued requests and returns their number */
	KT_ASSERT_ER(tk_sta_tsk(helper, tk_get_tid()), E_OK);
	KT_ASSERT_ER(tk_sta_tsk(helper, tk_get_tid()), E_OK);
	KT_ASSERT_EQ(tk_can_wup(TSK_SELF), 2);
	KT_ASSERT_EQ(tk_can_wup(TSK_SELF), 0);
	KT_ASSERT_ER(tk_slp_tsk(0), E_TMOUT);
	KT_ASSERT_ER(tk_del_tsk(helper), E_OK);
}

/* tk_dly_tsk timing at tick resolution, release of a delay */
LOCAL void test_delay( void )
{
	ID	id;
	T_RTSK	rtsk;

	step = 0;
	id = kt_cre_tsk((FP)t_delay, KT_PRI_HIGH, NULL);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskwait, TTW_DLY);
	KT_ASSERT_ER(tk_dly_tsk(100), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_OK);

	step = 0;
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_ER(tk_rel_wai(id), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_RLWAI);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

/* tk_sus_tsk / tk_rsm_tsk / tk_frsm_tsk and tk_ter_tsk */
LOCAL void test_suspend_terminate( void )
{
	ID	id;
	T_RTSK	rtsk;
	INT	s1, s2;

	step = 0;
	id = kt_cre_tsk((FP)t_spin, KT_PRI_HIGH, NULL);
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	s1 = step;
	KT_ASSERT(s1 > 0);

	KT_ASSERT_ER(tk_sus_tsk(id), E_OK);
	KT_ASSERT_ER(tk_sus_tsk(id), E_OK);		/* nested */
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.suscnt, 2);
	KT_ASSERT(rtsk.tskstat == TTS_WAS || rtsk.tskstat == TTS_SUS);
	s1 = step;
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	KT_ASSERT_EQ(step, s1);				/* frozen while suspended */

	KT_ASSERT_ER(tk_rsm_tsk(id), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	KT_ASSERT_EQ(step, s1);				/* still suspended (count 1) */
	KT_ASSERT_ER(tk_frsm_tsk(id), E_OK);		/* forced resume clears the count */
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	s2 = step;
	KT_ASSERT(s2 > s1);
	KT_ASSERT_ER(tk_rsm_tsk(id), E_OBJ);

	KT_ASSERT_ER(tk_ter_tsk(id), E_OK);
	KT_ASSERT_ER(tk_ref_tsk(id, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);
	s1 = step;
	KT_ASSERT_ER(tk_dly_tsk(30), E_OK);
	KT_ASSERT_EQ(step, s1);
	KT_ASSERT_ER(tk_ter_tsk(id), E_OBJ);
	KT_ASSERT_ER(tk_ter_tsk(tk_get_tid()), E_OBJ);	/* cannot terminate self */
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

/* many tasks: creation up to the limit and clean deletion */
/*
 * FP/SIMD registers kept across switches: two tasks created with TA_FPU,
 * on one processor, each keeping its own value in d8 while the other
 * runs. Without the save a switch hands the second task's value to the
 * first.
 */
LOCAL volatile INT	fpu_bad, fpu_done;

LOCAL void t_fpu( INT stacd, void *exinf )
{
	UD	mine = (UD)(UBINT)exinf, got;
	INT	i;

	Asm("fmov d8, %0" : : "r"(mine));
	for ( i = 0; i < 50; i++ ) {
		tk_dly_tsk(1);			/* the other one runs */
		Asm("fmov %0, d8" : "=r"(got));
		if ( got != mine ) {
			fpu_bad++;
		}
	}
	fpu_done++;
	tk_ext_tsk();
}

LOCAL ID fpu_task( UD value )
{
	T_CTSK	ctsk;

	ctsk.exinf   = (void *)(UBINT)value;
	ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC | TA_FPU;
	ctsk.task    = (FP)t_fpu;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;
	ctsk.assprc  = 1U << ( tk_get_prc() - 1 );
	return tk_cre_tsk(&ctsk);
}

LOCAL void test_fpu_kept( void )
{
	ID	a, b;
	INT	wait;

	fpu_bad = 0;
	fpu_done = 0;
	a = fpu_task(0x1111222233334444ULL);
	b = fpu_task(0x5555666677778888ULL);
	KT_ASSERT(a > 0);
	KT_ASSERT(b > 0);
	KT_ASSERT_ER(tk_sta_tsk(a, 0), E_OK);
	KT_ASSERT_ER(tk_sta_tsk(b, 0), E_OK);
	for ( wait = 0; wait < 500 && fpu_done < 2; wait++ ) {
		tk_dly_tsk(2);
	}
	KT_ASSERT_EQ(fpu_done, 2);
	KT_ASSERT_EQ(fpu_bad, 0);
	tk_del_tsk(a);
	tk_del_tsk(b);
}

/*
 * Tasks made until the kernel has no room for another: the table or the
 * memory for their stacks, whichever runs out first. Many more than any
 * test otherwise has, and the one after the last refused.
 */
#define MANY_TASKS	4096

LOCAL void test_many_tasks( void )
{
	ID	*ids = (ID *)Kmalloc(sizeof(ID) * MANY_TASKS);
	INT	n = 0, i;
	ER	er;

	KT_ASSERT(ids != NULL);
	if ( ids == NULL ) {
		return;
	}
	while ( n < MANY_TASKS ) {
		ids[n] = kt_cre_tsk((FP)t_simple, KT_PRI_LOW, NULL);
		if ( ids[n] <= 0 ) break;
		n++;
	}
	tm_printf((UB*)"  many tasks: %d made\n", n);
	KT_ASSERT(n >= 1024);
	if ( n < MANY_TASKS ) {
		er = kt_cre_tsk((FP)t_simple, KT_PRI_LOW, NULL);
		KT_ASSERT(er == E_LIMIT || er == E_NOMEM);
	}
	step = 0;
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(tk_sta_tsk(ids[i], i), E_OK);
	}
	for ( i = 0; i < 500 && step < n; i++ ) {
		tk_dly_tsk(10);
	}
	KT_ASSERT_EQ(step, n);
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(tk_del_tsk(ids[i]), E_OK);
	}
	Kfree(ids);
}

EXPORT void ktest_task( void )
{
	KT_RUN(test_create_start_exit);
	KT_RUN(test_priority);
	KT_RUN(test_sleep_wakeup);
	KT_RUN(test_delay);
	KT_RUN(test_suspend_terminate);
	KT_RUN(test_many_tasks);
	KT_RUN(test_fpu_kept);
}
