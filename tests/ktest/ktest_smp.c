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
 *	Multiprocessor: processor IDs, parallel execution, the processors'
 *	load (ts_ref_cpu), TA_ASSPRC, cross-processor wake-up, and a
 *	synchronisation stress run.
 *	Tests that need more than one processor are skipped on a single
 *	core.
 */

#include "ktest.h"
#include "kernel.h"			/* T_PCPU statistics */
#include <ts/time.h>
#include <ts/ob.h>

#define MAX_PRC		64		/* spinners and pinned tasks at most */
#ifdef CPU_CORE_X86_64
#define KT_PCPU(i)	knl_pcpu_of(i)		/* taken as each processor is found */
#else
#define KT_PCPU(i)	( &knl_pcpu_tbl[i] )
#endif
#define SPIN_NS		(50 * 1000000ULL)

LOCAL volatile ID	seen_prc[MAX_PRC];	/* the processor spinner i last ran on */
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
	ID	prc = 0;

	while ( mono() - t0 < SPIN_NS ) {
		prc = tk_get_prc();
	}
	seen_prc[stacd] = prc;
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

/* spinners below the test task's priority run on the other processors at the same time */
LOCAL void test_parallel( void )
{
	ID	id[MAX_PRC];
	INT	n = knl_num_prc, i, differ = 0;
	UD	t0;

	if ( n < 2 ) KT_SKIP("single processor");
	if ( n > MAX_PRC ) n = MAX_PRC;

	done_cnt = 0;
	for ( i = 0; i < n; i++ ) {
		seen_prc[i] = 0;
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
		KT_ASSERT(seen_prc[i] >= 1 && seen_prc[i] <= knl_num_prc);
		if ( seen_prc[i] != seen_prc[0] ) differ++;
		KT_ASSERT_ER(tk_del_tsk(id[i]), E_OK);
	}
	tm_printf((UB*)"  spinners on another processor than the first's: %d of %d\n", differ, n - 1);
	KT_ASSERT(differ > 0);				/* at least two processors */
}

/*
 * ts_ref_cpu: the processors online, the timer's frequency, and the time
 * each spends running tasks, which grows on every processor while
 * spinners keep them all busy and never by more than the time that went
 * by
 */
LOCAL void test_ref_cpu( void )
{
	T_TSCPU	*a, *b, h;
	ID	id[MAX_PRC];
	INT	n = knl_num_prc, i, loaded = 0;
	SZ	sz = TS_TSCPU_SIZE(knl_num_prc);
	UB	*guard;

	/* the number alone, nothing written past the header */
	guard = (UB *)Kmalloc((size_t)sz + 16);
	KT_ASSERT(guard != NULL);
	if ( guard == NULL ) return;
	knl_memset(guard, 0xa5, (SZ)sz + 16);
	KT_ASSERT_EQ(ts_ref_cpu((T_TSCPU *)guard, (SZ)sizeof(T_TSCPU)), knl_num_prc);
	KT_ASSERT(guard[sizeof(T_TSCPU)] == 0xa5 && guard[sz + 15] == 0xa5);
	KT_ASSERT_EQ(ts_ref_cpu(&h, (SZ)sizeof(h) - 1), E_PAR);
	Kfree(guard);

	a = (T_TSCPU *)Kmalloc((size_t)sz);
	b = (T_TSCPU *)Kmalloc((size_t)sz);
	KT_ASSERT(a != NULL && b != NULL);
	if ( a == NULL || b == NULL ) {
		if ( a != NULL ) Kfree(a);
		if ( b != NULL ) Kfree(b);
		return;
	}
	KT_ASSERT_EQ(ts_ref_cpu(a, sz), knl_num_prc);
	KT_ASSERT_EQ(a->ncpu, knl_num_prc);
	KT_ASSERT(a->freq > 0);
	if ( n > MAX_PRC ) n = MAX_PRC;
	done_cnt = 0;
	for ( i = 0; i < n; i++ ) {
		id[i] = kt_cre_tsk_any((FP)spinner, KT_PRI_LOW, NULL);
		KT_ASSERT(id[i] > 0);
		KT_ASSERT_ER(tk_sta_tsk(id[i], i), E_OK);
	}
	for ( i = 0; i < 400 && done_cnt < (UW)n; i++ ) {
		KT_ASSERT_ER(tk_dly_tsk(1), E_OK);
	}
	KT_ASSERT_EQ(ts_ref_cpu(b, sz), knl_num_prc);
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(tk_del_tsk(id[i]), E_OK);
	}
	KT_ASSERT(b->now > a->now);
	for ( i = 0; i < b->ncpu; i++ ) {
		UD	grew = b->busy[i] - a->busy[i];

		if ( i < 16 || i == b->ncpu - 1 ) {
			tm_printf((UB*)"  processor %d: %d%% busy\n", i + 1, (INT)( grew * 100 / ( b->now - a->now ) ));
		}
		KT_ASSERT(b->busy[i] >= a->busy[i]);
		KT_ASSERT(grew <= b->now - a->now + b->freq / 1000);
		if ( grew * 4 >= b->now - a->now ) loaded++;
	}
	KT_ASSERT(loaded >= ( n < 2 ? 1 : 2 ));
	Kfree(a);
	Kfree(b);
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
		ctsk.assprc  = (UBINT)1 << i;
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
#if CNF_MAX_PRCID < 64
	ctsk.assprc = (UBINT)1 << CNF_MAX_PRCID;
	KT_ASSERT_ER(tk_cre_tsk(&ctsk), E_PAR);
#endif
	ctsk.assprc = 0;				/* TA_ASSPRC with none */
	KT_ASSERT_ER(tk_cre_tsk(&ctsk), E_PAR);
}

/*
 * tk_set_asp and tk_ref_asp: a set of the caller's size, which can name
 * any processor (the last one online, beyond PRC 64 on a large machine),
 * read back as set; an empty set is every processor online; a set of no
 * processor online, of one beyond the limit, or of a size that is not a
 * whole number of words is refused
 */
LOCAL void test_set_asp( void )
{
	T_CTSK	ctsk;
	UBINT	set[( CNF_MAX_PRCID + 63 ) / 64 + 1], got[( CNF_MAX_PRCID + 63 ) / 64 + 1];
	INT	nw = ( CNF_MAX_PRCID + 63 ) / 64, k, cnt;
	ID	id, last = knl_num_prc;

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)pinned;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;
	id = tk_cre_tsk(&ctsk);
	KT_ASSERT(id > 0);
	if ( id <= 0 ) return;

	/* not set: every processor online */
	KT_ASSERT_EQ(tk_ref_asp(id, got, (SZ)( sizeof(UBINT) * nw )), nw);
	for ( cnt = 0, k = 0; k < knl_num_prc + 64; k++ ) {
		if ( k / 64 < nw && ( got[k / 64] >> ( k % 64 ) & 1 ) != 0 ) cnt++;
	}
	KT_ASSERT_EQ(cnt, knl_num_prc);

	/* the last processor alone, and it runs there */
	for ( k = 0; k <= nw; k++ ) set[k] = 0;
	set[( last - 1 ) / 64] = (UBINT)1 << ( ( last - 1 ) % 64 );
	KT_ASSERT_ER(tk_set_asp(id, set, (SZ)( sizeof(UBINT) * nw )), E_OK);
	KT_ASSERT_EQ(tk_ref_asp(id, got, (SZ)( sizeof(UBINT) * ( nw + 1 ) )), nw + 1);
	for ( k = 0; k < nw; k++ ) KT_ASSERT(got[k] == set[k]);
	KT_ASSERT(got[nw] == 0);
	done_cnt = 0;
	pinned_prc[0] = 0;
	KT_ASSERT_ER(tk_sta_tsk(id, 0), E_OK);
	for ( k = 0; k < 200 && done_cnt == 0; k++ ) {
		KT_ASSERT_ER(tk_dly_tsk(1), E_OK);
	}
	tm_printf((UB *)"  pinned to PRC %d with tk_set_asp: ran on %d\n", last, (INT)pinned_prc[0]);
	KT_ASSERT_EQ(pinned_prc[0], last);

	/* refused */
	KT_ASSERT_ER(tk_set_asp(TSK_SELF, set, 7), E_PAR);			/* not a whole word */
	set[nw] = 1;								/* beyond the limit */
	KT_ASSERT_ER(tk_set_asp(TSK_SELF, set, (SZ)( sizeof(UBINT) * ( nw + 1 ) )), E_PAR);
	set[nw] = 0;
	if ( knl_num_prc < CNF_MAX_PRCID ) {					/* none online */
		for ( k = 0; k < nw; k++ ) set[k] = 0;
		set[knl_num_prc / 64] = (UBINT)1 << ( knl_num_prc % 64 );
		KT_ASSERT_ER(tk_set_asp(TSK_SELF, set, (SZ)( sizeof(UBINT) * nw )), E_PAR);
	}
	KT_ASSERT_ER(tk_del_tsk(id), E_OK);
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
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)sleeper;
	ctsk.itskpri = KT_PRI_HIGH;
	ctsk.stksz   = 4096;
	id = kt_cre_on(&ctsk, other);
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
			(INT)KT_PCPU(i)->ctxsw_cnt, (INT)KT_PCPU(i)->ipi_cnt);
	}
	KT_ASSERT_EQ(st_tmo, 0);			/* every signal woke its waiter */
	KT_ASSERT(total > 1000);
}

/*
 * A wait ended from another processor returns what that processor gave
 * it, and nothing writes into the waiter's stack after it has returned
 * (design 4.3). The waiter waits without a limit, so anything but E_OK
 * is wrong; E_TMOUT is what the wait had been given to begin with. The
 * other side signals just as the waiter goes into its wait, at a moment
 * that moves a little each round, so that the signal comes while the
 * waiter is still on its processor after letting go of the kernel lock.
 *
 * As the processors run, the moment in which that goes wrong is a few
 * instructions long and is met once in a great many rounds. The second
 * run makes it long with the kernel's hooks for the tests (smp.c): a
 * pause after each critical section lets go of the lock, and a longer
 * one between a wait being ended and its result being written. A waiter
 * that decided after letting go of the lock whether to give up its
 * processor would find itself made ready again in the first pause, and
 * read its result in the second, before it was written.
 */
#define WR_PAIRS	2
#define WR_MS		2000
#define WR_UNLOCK_SPIN	4000
#define WR_RELEASE_SPIN	40000

LOCAL ID		wr_sem[WR_PAIRS];
LOCAL volatile UD	wr_at[WR_PAIRS];	/* the round the waiter is in */
LOCAL volatile UD	wr_rounds[WR_PAIRS];
LOCAL volatile UW	wr_bad;
LOCAL volatile ER	wr_bad_er;
LOCAL volatile BOOL	wr_stop;

LOCAL void wr_waiter( INT p, void *exinf )
{
	UD	k;
	ER	er;

	for ( k = 1; !wr_stop; k++ ) {
		wr_at[p] = k;
		er = tk_wai_sem(wr_sem[p], 1, TMO_FEVR);
		if ( er != E_OK ) {
			wr_bad_er = er;
			atomic_inc((UW *)&wr_bad);
		}
		wr_rounds[p] = k;
	}
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

LOCAL void wr_signaller( INT p, void *exinf )
{
	UD	k;
	UW	d = (UW)p * 37;
	INT	i;

	for ( k = 1; !wr_stop; k++ ) {
		while ( wr_at[p] != k && !wr_stop ) ;
		for ( i = 0; i < (INT)d; i++ ) Asm("nop");
		d = ( d + 7 ) % 400;
		tk_sig_sem(wr_sem[p], 1);
	}
	atomic_inc((UW *)&done_cnt);
	tk_ext_tsk();
}

LOCAL ID wr_task( FP fn, PRI pri, INT prc )
{
	T_CTSK	ctsk;

	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = fn;
	ctsk.itskpri = pri;
	ctsk.stksz   = 4096;
	return kt_cre_on(&ctsk, prc);
}

/* One run of the pairs for ms, the hooks set as given; the waits ended */
LOCAL UD wr_run( INT ms, UW unlock_spin, UW release_spin )
{
	T_CSEM	csem;
	ID	wid[WR_PAIRS], sid[WR_PAIRS];
	INT	me = tk_get_prc(), other[3], n = 0, i, p;
	UD	total = 0;

	for ( i = 1; i <= knl_num_prc && n < 3; i++ ) {
		if ( i != me ) other[n++] = i;
	}
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem = 1;
	wr_stop = FALSE;
	done_cnt = 0;
	for ( i = 0; i < WR_PAIRS; i++ ) {
		wr_sem[i] = tk_cre_sem(&csem);
		KT_ASSERT(wr_sem[i] > 0);
		wr_at[i] = 0;
		wr_rounds[i] = 0;
	}
	/* the waiters on processors of their own, the signallers beside them; one shares the test's */
	wid[0] = wr_task((FP)wr_waiter, KT_PRI_HIGH, other[0]);
	sid[0] = wr_task((FP)wr_signaller, KT_PRI_LOW, other[1]);
	wid[1] = wr_task((FP)wr_waiter, KT_PRI_HIGH, other[2]);
	sid[1] = wr_task((FP)wr_signaller, KT_PRI_LOW, me);
	knl_kt_unlock_spin = unlock_spin;
	knl_kt_release_spin = release_spin;
	for ( i = 0; i < WR_PAIRS; i++ ) {
		KT_ASSERT(wid[i] > 0 && sid[i] > 0);
		KT_ASSERT_ER(tk_sta_tsk(wid[i], i), E_OK);
		KT_ASSERT_ER(tk_sta_tsk(sid[i], i), E_OK);
	}
	KT_ASSERT_ER(tk_dly_tsk(ms), E_OK);

	wr_stop = TRUE;
	knl_kt_unlock_spin = 0;
	knl_kt_release_spin = 0;
	for ( i = 0; i < 100 && done_cnt < WR_PAIRS * 2; i++ ) {
		for ( p = 0; p < WR_PAIRS; p++ ) (void)tk_sig_sem(wr_sem[p], 1);
		KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	}
	KT_ASSERT_EQ(done_cnt, WR_PAIRS * 2);
	for ( i = 0; i < WR_PAIRS; i++ ) {
		total += wr_rounds[i];
		KT_ASSERT_ER(tk_del_tsk(wid[i]), E_OK);
		KT_ASSERT_ER(tk_del_tsk(sid[i]), E_OK);
		KT_ASSERT_ER(tk_del_sem(wr_sem[i]), E_OK);
	}
	return total;
}

LOCAL void test_wait_result( void )
{
	UD	total;

	if ( knl_num_prc < 4 ) KT_SKIP("fewer than four processors");

	wr_bad = 0;
	wr_bad_er = E_OK;
	total = wr_run(WR_MS, 0, 0);
	tm_printf((UB*)"  waits ended from another processor: %d, wrong results: %d (last %d)\n",
		  (INT)total, (INT)wr_bad, (INT)wr_bad_er);
	KT_ASSERT_EQ(wr_bad, 0);
	KT_ASSERT(total > 1000);

	wr_bad = 0;
	wr_bad_er = E_OK;
	total = wr_run(WR_MS, WR_UNLOCK_SPIN, WR_RELEASE_SPIN);
	tm_printf((UB*)"  the same with the moments made long: %d, wrong results: %d (last %d)\n",
		  (INT)total, (INT)wr_bad, (INT)wr_bad_er);
	KT_ASSERT_EQ(wr_bad, 0);
	KT_ASSERT(total > 100);
}

/*
 * A task's stack is given back only once its processor has left it
 * (design 4.3). A task that ends goes on running on its stack for a
 * moment after it has let go of the kernel lock: it pushes its frame
 * for the dispatcher and only then moves to the dispatcher's own stack.
 * Another processor that deleted it meanwhile (tk_del_tsk of a dormant
 * task), or the allocator that had been given the stack back by the
 * task itself (tk_exd_tsk), would hand out memory that is still being
 * written. The hook of the tests (smp.c) makes that moment long; the
 * control block says whether a processor still holds the task's
 * context (runprc), the allocator whether the stack is free.
 */
#define EX_ROUNDS	20
#define EX_SPIN		200000

IMPORT BOOL	knl_Iinuse( void *ptr );
IMPORT TCB	knl_tcb_table[];

LOCAL volatile BOOL	ex_going;

LOCAL void ex_ext( INT stacd, void *exinf )
{
	ex_going = TRUE;
	tk_ext_tsk();
}

LOCAL void ex_exd( INT stacd, void *exinf )
{
	ex_going = TRUE;
	tk_exd_tsk();
}

LOCAL ID ex_start( FP fn, INT prc, void **p_stack )
{
	ID	tid = wr_task(fn, KT_PRI_HIGH, prc);
	TCB	*t;

	if ( tid <= 0 ) {
		return tid;
	}
	t = &knl_tcb_table[tid - 1];
	*p_stack = (VB *)t->isstack - t->sstksz;
	ex_going = FALSE;
	if ( tk_sta_tsk(tid, 0) < E_OK ) {
		tk_del_tsk(tid);
		return E_SYS;
	}
	while ( !ex_going ) ;
	return tid;
}

/* Deleted by another processor as soon as it is dormant */
LOCAL void test_del_ended( void )
{
	INT	me = tk_get_prc(), prc = ( me == 1 ) ? 2 : 1, i, held = 0;
	void	*stk;
	ER	er;
	ID	tid;

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");
	knl_kt_exit_spin = EX_SPIN;
	for ( i = 0; i < EX_ROUNDS; i++ ) {
		tid = ex_start((FP)ex_ext, prc, &stk);
		KT_ASSERT(tid > 0);
		if ( tid <= 0 ) break;
		while ( ( er = tk_del_tsk(tid) ) == E_OBJ ) ;	/* not dormant yet */
		KT_ASSERT_ER(er, E_OK);
		if ( knl_tcb_table[tid - 1].runprc != 0 ) {
			held++;
		}
	}
	knl_kt_exit_spin = 0;
	tm_printf((UB*)"  deleted while their processor still held them: %d of %d\n", held, EX_ROUNDS);
	KT_ASSERT_EQ(held, 0);
}

/* Deleting itself: its stack is not free while its processor is on it */
LOCAL void test_exd_stack( void )
{
	INT	me = tk_get_prc(), prc = ( me == 1 ) ? 2 : 1, i, early = 0;
	void	*stk;
	TCB	*t;
	ID	tid;

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");
	knl_kt_exit_spin = EX_SPIN;
	for ( i = 0; i < EX_ROUNDS; i++ ) {
		tid = ex_start((FP)ex_exd, prc, &stk);
		KT_ASSERT(tid > 0);
		if ( tid <= 0 ) break;
		t = &knl_tcb_table[tid - 1];
		while ( t->runprc != 0 ) {
			if ( !knl_Iinuse(stk) ) {
				/* free: wrong only if its processor still held it after that */
#if defined(__x86_64__)
				SmpMemBarrier();
#else
				Asm("dmb ish" ::: "memory");
#endif
				if ( t->runprc != 0 ) early++;
				break;
			}
		}
		while ( t->state != 0 ) tk_dly_tsk(1);	/* TS_NONEXIST */
	}
	knl_kt_exit_spin = 0;
	tm_printf((UB*)"  stacks free while their processor was still on them: %d of %d\n", early, EX_ROUNDS);
	KT_ASSERT_EQ(early, 0);
}

/*
 * The processor that takes a device's interrupt (design 4.5): the disk's
 * interrupt sent to PRC 2 is taken there and no longer on PRC 1, and
 * back on PRC 1 when the system chooses again; the same through the
 * disk's object (irq.cpu); a processor kept for real time is refused.
 * The disk's handler is wrapped for the test, to count where it runs.
 */
#ifdef CPU_CORE_X86_64
#define IT_TBL		knl_intvec_tbl
#else
IMPORT FP	knl_hll_inthdr_tbl[];
#define IT_TBL		knl_hll_inthdr_tbl
#endif

LOCAL FP		it_orig;
LOCAL volatile UW	it_on[MAX_PRC];

LOCAL void it_wrap( UINT intno, UW iar )
{
	ID	p = tk_get_prc();

	if ( p >= 1 && p <= MAX_PRC ) atomic_inc((UW *)&it_on[p - 1]);
	(*(void (*)(UINT, UW))it_orig)(intno, iar);
}

/* Reads of the disk, for its interrupts; the counts taken from zero */
LOCAL INT it_reads( ID dd, UB *buf )
{
	SZ	asize;
	INT	i, bad = 0;
	LOCAL INT	round = 0;

	for ( i = 0; i < MAX_PRC; i++ ) it_on[i] = 0;
	round++;
	for ( i = 0; i < 40; i++ ) {
		if ( tk_srea_dev(dd, (D)( ( i * 997 + round * 4099 ) % 65536 ), buf, 8 * 512, &asize) < E_OK ) bad++;
	}
	return bad;
}

/* The interrupts counted on processors other than PRC prc */
LOCAL UW it_elsewhere( INT prc )
{
	UW	n = 0;
	INT	i;

	for ( i = 0; i < MAX_PRC; i++ ) {
		if ( i != prc - 1 ) n += it_on[i];
	}
	return n;
}

LOCAL void test_int_target( void )
{
	UINT	intno[4];
	ID	prc[4], want;
	ID	dd;
	UB	*buf;
	INT	n, bad;
	TS_UUID	u;
	IMPORT ER knl_ob_meta_write( CONST TS_UUID *uuid, CONST UB *buf, INT len );

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");
	n = knl_int_dev_info((CONST UB *)KT_DISK, &want, intno, prc, 4);
	if ( n <= 0 ) KT_SKIP("the disk's driver names no interrupt");
	KT_ASSERT_EQ(GetIntTarget(intno[0]), 1);
	KT_ASSERT_ER(SetIntTarget(intno[0], knl_num_prc + 1), E_PAR);
	KT_ASSERT_ER(SetIntTarget(5, 2), E_PAR);	/* not a device's */

	dd = tk_opn_dev((UB *)KT_DISK, TD_READ);
	buf = (UB *)Kmalloc(8 * 512);
	KT_ASSERT(dd > 0 && buf != NULL);
	if ( dd <= 0 || buf == NULL ) return;
	it_orig = IT_TBL[intno[0]];
	IT_TBL[intno[0]] = (FP)it_wrap;

	/* where it comes now: PRC 1 */
	bad = it_reads(dd, buf);
	tm_printf((UB *)"  interrupt %d of %s as it was: on PRC1 %d, elsewhere %d (reads failed %d)\n",
		  intno[0], KT_DISK, it_on[0], it_elsewhere(1), bad);
	KT_ASSERT_EQ(bad, 0);
	if ( it_on[0] + it_elsewhere(1) == 0 ) {
		IT_TBL[intno[0]] = it_orig;
		tk_cls_dev(dd, 0);
		Kfree(buf);
		KT_SKIP("the disk's reads end without its interrupt");
	}
	KT_ASSERT_EQ(it_elsewhere(1), 0);

	/* sent to PRC 2 */
	KT_ASSERT_ER(SetIntTarget(intno[0], 2), E_OK);
	KT_ASSERT_EQ(GetIntTarget(intno[0]), 2);
	(void)it_reads(dd, buf);
	tm_printf((UB *)"  sent to PRC2: on PRC2 %d, elsewhere %d\n", it_on[1], it_elsewhere(2));
	KT_ASSERT(it_on[1] > 0);
	KT_ASSERT_EQ(it_elsewhere(2), 0);

	/* the system's choice again: PRC 1 */
	KT_ASSERT_ER(SetIntTarget(intno[0], 0), E_OK);
	KT_ASSERT_EQ(GetIntTarget(intno[0]), 1);
	(void)it_reads(dd, buf);
	tm_printf((UB *)"  back to the system's choice: on PRC1 %d, elsewhere %d\n", it_on[0], it_elsewhere(1));
	KT_ASSERT(it_on[0] > 0);
	KT_ASSERT_EQ(it_elsewhere(1), 0);

	/* through the disk's object */
	if ( knl_obdev_uuid((CONST UB *)KT_DISK, &u) >= E_OK ) {
		static CONST char	on2[] = "{\"irq\":{\"cpu\":2}}", off[] = "{\"irq\":{\"cpu\":0}}";

		KT_ASSERT_ER(knl_ob_meta_write(&u, (CONST UB *)on2, (INT)sizeof(on2) - 1), E_OK);
		KT_ASSERT_EQ(GetIntTarget(intno[0]), 2);
		(void)knl_int_dev_info((CONST UB *)KT_DISK, &want, intno, prc, 4);
		KT_ASSERT_EQ(want, 2);
		(void)it_reads(dd, buf);
		KT_ASSERT(it_on[1] > 0);
		KT_ASSERT_EQ(it_elsewhere(2), 0);
		KT_ASSERT_ER(knl_ob_meta_write(&u, (CONST UB *)off, (INT)sizeof(off) - 1), E_OK);
		KT_ASSERT_EQ(GetIntTarget(intno[0]), 1);
	}

	/* the processors for chores: one by message is spread among them */
	if ( knl_int_is_msi(intno[0]) && knl_num_prc >= 3 ) {
		KT_ASSERT_ER(knl_cpu_roles_text("1,3", ""), E_OK);
		tm_printf((UB *)"  with PRC1 and PRC3 for chores, interrupt %d goes to PRC%d\n",
			  intno[0], GetIntTarget(intno[0]));
		KT_ASSERT(GetIntTarget(intno[0]) == 1 || GetIntTarget(intno[0]) == 3);
		KT_ASSERT_ER(knl_cpu_roles_text("", ""), E_OK);
		KT_ASSERT_EQ(GetIntTarget(intno[0]), 1);
	}

#ifdef CPU_CORE_X86_64
	/* an IOAPIC's input in use (the serial port's): its entry sent to PRC 2 and back */
	{
		UINT	v;

		for ( v = INTVEC_GSI(0); v < INTVEC_GSI(INTVEC_GSI_NUM); v++ ) {
			if ( knl_int_in_use(v) ) break;
		}
		if ( v < INTVEC_GSI(INTVEC_GSI_NUM) ) {
			KT_ASSERT_ER(SetIntTarget(v, 2), E_OK);
			KT_ASSERT_EQ(GetIntTarget(v), 2);
			KT_ASSERT_ER(SetIntTarget(v, 0), E_OK);
			KT_ASSERT_EQ(GetIntTarget(v), 1);
			tm_printf((UB *)"  IOAPIC input of interrupt %d sent to PRC2 and back\n", v);
		}
	}
#endif

	/* a processor kept for real time takes no device's interrupt */
	KT_ASSERT_ER(knl_cpu_roles_text("1", "2"), E_OK);
	KT_ASSERT_ER(SetIntTarget(intno[0], 2), E_OBJ);
	KT_ASSERT_EQ(GetIntTarget(intno[0]), 1);
	KT_ASSERT_ER(knl_cpu_roles_text("", ""), E_OK);

	IT_TBL[intno[0]] = it_orig;
	tk_cls_dev(dd, 0);
	Kfree(buf);
}

EXPORT void ktest_smp( void )
{
	KT_RUN(test_get_prc);
	KT_RUN(test_parallel);
	KT_RUN(test_ref_cpu);
	KT_RUN(test_assprc);
	KT_RUN(test_set_asp);
	KT_RUN(test_cross_wakeup);
	KT_RUN(test_timer_other_prc);
	KT_RUN(test_stress);
	KT_RUN(test_wait_result);
	KT_RUN(test_del_ended);
	KT_RUN(test_exd_stack);
	KT_RUN(test_int_target);
}
