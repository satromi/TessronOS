/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_smp.c
 *	Multiprocessor: processor IDs, parallel execution, TA_ASSPRC,
 *	cross-processor wake-up, and a synchronisation stress run.
 *	Tests that need more than one processor are skipped on a single
 *	core.
 */

#include "ktest.h"
#include "kernel.h"			/* T_PCPU statistics */

#define MAX_PRC		8
#define SPIN_NS		(50 * 1000000ULL)

LOCAL volatile UW	seen_mask[MAX_PRC];	/* processors seen by spinner i */
LOCAL volatile UW	done_cnt;
LOCAL volatile ID	pinned_prc[MAX_PRC];
LOCAL volatile UD	wake_time;

LOCAL UD mono( void )
{
	UD	ns = 0;

	ts_get_mono(&ns);
	return ns;
}

/* every processor reports a valid ID */
LOCAL void test_get_prc( void )
{
	ID	prc = tk_get_prc();

	tm_printf((UB*)"  processors online: %d\n", knl_num_prc);
	KT_ASSERT(prc >= 1 && prc <= knl_num_prc);
}

LOCAL void spinner( INT stacd, void *exinf )
{
	UD	t0 = mono();
	UW	mask = 0;

	while ( mono() - t0 < SPIN_NS ) {
		mask |= 1U << tk_get_prc();
	}
	seen_mask[stacd] = mask;
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

/* spinners below the test task's priority run on the other processors at the same time */
LOCAL void test_parallel( void )
{
	ID	id[MAX_PRC];
	INT	n = knl_num_prc, i;
	UW	all = 0;
	UD	t0;

	if ( n < 2 ) KT_SKIP("single processor");
	if ( n > MAX_PRC ) n = MAX_PRC;

	done_cnt = 0;
	for ( i = 0; i < n; i++ ) {
		seen_mask[i] = 0;
		id[i] = kt_cre_tsk_any((FP)spinner, KT_PRI_LOW, NULL);
		KT_ASSERT(id[i] > 0);
	}
	t0 = mono();
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(tk_sta_tsk(id[i], i), E_OK);
	}
	/* Wait for all of them, then compare with the time they would need
	   one after another (n x SPIN_NS). The spinners are below the test
	   task's priority, so they also use this processor while it sleeps. */
	for ( i = 0; i < 400 && done_cnt < (UW)n; i++ ) {
		KT_ASSERT_ER(tk_dly_tsk(1), E_OK);
	}
	KT_ASSERT_EQ(done_cnt, n);
	KT_ASSERT(mono() - t0 < (UD)n * SPIN_NS);
	for ( i = 0; i < n; i++ ) {
		all |= seen_mask[i];
		KT_ASSERT_ER(tk_del_tsk(id[i]), E_OK);
	}
	tm_printf((UB*)"  processors used: mask=0x%x\n", all);
	KT_ASSERT(all != 0 && (all & (all - 1)) != 0);	/* at least two processors */
}

LOCAL void pinned( INT stacd, void *exinf )
{
	UD	t0 = mono();
	ID	prc = tk_get_prc();

	while ( mono() - t0 < 5000000ULL ) {		/* 5ms, must not migrate */
		if ( tk_get_prc() != prc ) prc = -1;
	}
	pinned_prc[stacd] = prc;
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

/* TA_ASSPRC pins a task to one processor */
LOCAL void test_assprc( void )
{
	T_CTSK	ctsk;
	ID	id[MAX_PRC];
	INT	n = knl_num_prc, i;

	if ( n > MAX_PRC ) n = MAX_PRC;
	done_cnt = 0;
	for ( i = 0; i < n; i++ ) {
		pinned_prc[i] = 0;
		ctsk.exinf   = NULL;
		ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC;
		ctsk.task    = (FP)pinned;
		ctsk.itskpri = KT_PRI_HIGH;
		ctsk.stksz   = 4096;
		ctsk.assprc  = 1U << i;
		id[i] = tk_cre_tsk(&ctsk);
		KT_ASSERT(id[i] > 0);
		KT_ASSERT_ER(tk_sta_tsk(id[i], i), E_OK);
	}
	KT_ASSERT_ER(tk_dly_tsk(100), E_OK);
	KT_ASSERT_EQ(done_cnt, n);
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_EQ(pinned_prc[i], i + 1);
		KT_ASSERT_ER(tk_del_tsk(id[i]), E_OK);
	}

	/* an invalid processor is rejected */
	ctsk.assprc = 1U << CNF_MAX_PRCID;
	KT_ASSERT_ER(tk_cre_tsk(&ctsk), E_PAR);
}

LOCAL void sleeper( INT stacd, void *exinf )
{
	tk_slp_tsk(TMO_FEVR);
	wake_time = mono();
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

/* a task on another processor is woken through an IPI */
LOCAL void test_cross_wakeup( void )
{
	T_CTSK	ctsk;
	ID	id;
	ID	me = tk_get_prc();
	INT	other = ( me == 1 ) ? 2 : 1;
	UD	t0;

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC;
	ctsk.task    = (FP)sleeper;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;
	ctsk.assprc  = 1U << (other - 1);
	id = tk_cre_tsk(&ctsk);
	KT_ASSERT(id > 0);
	done_cnt = 0;
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);		/* it is asleep on the other processor */
	KT_ASSERT_EQ(done_cnt, 0);

	t0 = mono();
	KT_ASSERT_ER(tk_wup_tsk(id), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	KT_ASSERT_EQ(done_cnt, 1);
	tm_printf((UB*)"  cross-processor wake-up latency: %d ns\n", (INT)(wake_time - t0));
	KT_ASSERT(wake_time - t0 < 5000000ULL);		/* 5ms */
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

LOCAL volatile UD	dly_worst;

LOCAL void delayer( INT stacd, void *exinf )
{
	INT	i;
	UD	t0, d, worst = 0;

	for ( i = 0; i < 20; i++ ) {
		t0 = mono();
		tk_dly_tsk_u(100);
		d = mono() - t0;
		if ( d > worst ) worst = d;
	}
	dly_worst = worst;
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

/* timeouts requested on another processor are served by the timer of PRC 1 */
LOCAL void test_timer_other_prc( void )
{
	T_CTSK	ctsk;
	ID	id;

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC;
	ctsk.task    = (FP)delayer;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;
	ctsk.assprc  = 1U << 1;			/* PRC 2 */
	id = tk_cre_tsk(&ctsk);
	KT_ASSERT(id > 0);
	done_cnt = 0;
	dly_worst = 0;
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(200), E_OK);
	KT_ASSERT_EQ(done_cnt, 1);
	tm_printf((UB*)"  tk_dly_tsk_u(100) on PRC2: worst %d ns\n", (INT)dly_worst);
	/* Never early is the part that is the kernel's doing. How late
	   it runs depends on the machine underneath, which is an
	   emulator here, so the far bound is wide and the measured
	   value is printed above. */
	KT_ASSERT(dly_worst >= 100000ULL && dly_worst < 50000000ULL);
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
}

/* stress: semaphore ping-pong pairs plus short delays on all processors */
#define NPAIR	4
LOCAL ID		st_sem[NPAIR][2];
LOCAL volatile UD	st_count[NPAIR][2];
LOCAL volatile BOOL	st_stop;
LOCAL volatile UW	st_tmo;			/* waits that timed out (lost wake-ups) */

LOCAL void pinger( INT stacd, void *exinf )
{
	INT	p = stacd >> 1, side = stacd & 1;

	while ( !st_stop ) {
		/*
		 * The partner answers in microseconds, so a wait that reaches
		 * the timeout means the signal was lost. One second is far
		 * above the stalls a vCPU sees under an emulator.
		 */
		ER	er = tk_wai_sem(st_sem[p][side], 1, 1000);
		if ( er == E_OK ) {
			atomic64_add((UD *)&st_count[p][side], 1);
			tk_sig_sem(st_sem[p][side ^ 1], 1);
		} else if ( er == E_TMOUT ) {
			atomic_inc((UW *)&st_tmo);
		}
		if ( (st_count[p][side] & 63) == 0 ) {
			tk_dly_tsk_u(100);
		}
	}
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

LOCAL void test_stress( void )
{
	T_CSEM	csem;
	ID	id[NPAIR * 2];
	INT	i;
	UD	total = 0;

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.maxsem = 1;
	for ( i = 0; i < NPAIR; i++ ) {
		csem.isemcnt = 1;
		st_sem[i][0] = tk_cre_sem(&csem);
		csem.isemcnt = 0;
		st_sem[i][1] = tk_cre_sem(&csem);
		KT_ASSERT(st_sem[i][0] > 0 && st_sem[i][1] > 0);
		st_count[i][0] = st_count[i][1] = 0;
	}
	st_stop = FALSE;
	st_tmo = 0;
	done_cnt = 0;
	for ( i = 0; i < NPAIR * 2; i++ ) {
		id[i] = kt_cre_tsk_any((FP)pinger, KT_PRI_LOW, NULL);
		KT_ASSERT(id[i] > 0);
	}
	for ( i = 0; i < NPAIR * 2; i++ ) {
		KT_ASSERT_ER(tk_sta_tsk(id[i], i), E_OK);
	}
	KT_ASSERT_ER(tk_dly_tsk(1000), E_OK);

	/* Stop them: the ping-pong breaks as soon as one side leaves, so the
	   other side is released here instead of waiting out its timeout. */
	st_stop = TRUE;
	for ( i = 0; i < NPAIR; i++ ) {
		tk_sig_sem(st_sem[i][0], 1);
		tk_sig_sem(st_sem[i][1], 1);
	}
	for ( i = 0; i < 100 && done_cnt < NPAIR * 2; i++ ) {
		KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	}
	KT_ASSERT_EQ(done_cnt, NPAIR * 2);
	for ( i = 0; i < NPAIR; i++ ) {
		total += st_count[i][0] + st_count[i][1];
		/* The two sides alternate, so their counts differ by at most one
		   (plus the one release each side may take from the stop above). */
		KT_ASSERT(st_count[i][0] + 1 >= st_count[i][1] && st_count[i][0] <= st_count[i][1] + 2);
		KT_ASSERT_ER(tk_del_sem(st_sem[i][0]), E_OK);
		KT_ASSERT_ER(tk_del_sem(st_sem[i][1]), E_OK);
	}
	for ( i = 0; i < NPAIR * 2; i++ ) {
		KT_ASSERT_ER(tk_del_tsk(id[i]), E_OK);
	}
	tm_printf((UB*)"  semaphore exchanges in 1s: %d, timeouts: %d\n", (INT)total, (INT)st_tmo);
	for ( i = 0; i < knl_num_prc; i++ ) {
		tm_printf((UB*)"  PRC%d: ctxsw=%d ipi=%d\n", i + 1,
			(INT)knl_pcpu_tbl[i].ctxsw_cnt, (INT)knl_pcpu_tbl[i].ipi_cnt);
	}
	KT_ASSERT_EQ(st_tmo, 0);			/* every signal woke its waiter */
	KT_ASSERT(total > 1000);
}

EXPORT void ktest_smp( void )
{
	KT_RUN(test_get_prc);
	KT_RUN(test_parallel);
	KT_RUN(test_assprc);
	KT_RUN(test_cross_wakeup);
	KT_RUN(test_timer_other_prc);
	KT_RUN(test_stress);
}
