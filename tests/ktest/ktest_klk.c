/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_klk.c
 *	The kernel's locks (design 4.3.3): the check of their order finds a
 *	lock taken out of order, and calls that act on several objects and
 *	tasks at once, run together on every processor, give the right
 *	results -- counts that add up, priorities that come back, waits
 *	ended once -- with the moments where processors meet made long by
 *	the kernel's hooks for the tests. Run in every lock mode: in mode 0
 *	the same calls run under the big lock.
 */

#include "ktest.h"
#include "kernel.h"

#define NTSK		12		/* tasks of a load */
#define RUN_MS		1200		/* how long a load runs */

LOCAL volatile BOOL	k_stop;
LOCAL volatile UW	k_done;
LOCAL volatile UW	k_bad;			/* a result that cannot be */
LOCAL volatile ER	k_bad_er;

LOCAL UW k_rand( UW *s )
{
	*s = *s * 1103515245U + 12345U;
	return *s >> 16;
}

LOCAL void k_fault( ER er )
{
	k_bad_er = er;
	atomic_inc((UW *)&k_bad);
}

/* The hooks that make the moments long (klk.h), all at once or none */
LOCAL void k_spins( BOOL on )
{
#if KLK_FINE
	knl_kt_klk_spin[KLK_R_TCTL]  = on ? 300 : 0;
	knl_kt_klk_spin[KLK_R_OBJ]   = on ? 500 : 0;
	knl_kt_klk_spin[KLK_R_SCHED] = on ? 200 : 0;
	knl_kt_waitof_spin = on ? 3000 : 0;
	knl_kt_fire_spin = on ? 5000 : 0;
#else
	(void)on;
#endif
	knl_kt_release_spin = on ? 2000 : 0;
	knl_kt_unlock_spin = on ? 1000 : 0;
}

LOCAL void k_wait_done( UW n )
{
	INT	i;

	for ( i = 0; i < 500 && k_done < n; i++ ) {
		tk_dly_tsk(10);
	}
}

/* A task that has counted itself done is deleted once it has ended */
LOCAL ER k_del( ID tid )
{
	ER	er;
	INT	i;

	for ( i = 0; i < 1000 && ( er = tk_del_tsk(tid) ) == E_OBJ; i++ ) {
		tk_dly_tsk(1);
	}
	return er;
}

LOCAL ID k_task( FP fn, PRI pri, INT stacd )
{
	ID	id = kt_cre_tsk_any(fn, pri, NULL);

	if ( id > 0 && tk_sta_tsk(id, stacd) < E_OK ) {
		tk_del_tsk(id);
		return E_SYS;
	}
	return id;
}

/* ------------------------------------------------------------------------ */
/*
 * The check of the order: a lock of an object taken under the
 * scheduler's is found and counted (said nothing of: it is the test's)
 */
LOCAL void test_klk_check( void )
{
#if CNF_KLOCK_CHECK && KLK_FINE_OBJ
	IMPORT volatile BOOL	knl_klk_quiet;
	KLOCK	l;
	UW	before;
	UINT	s;

	knl_klk_init(&l);
	knl_klk_quiet = TRUE;
	before = knl_klk_bad;
	s = disint();
	KLK_SCHED_DI();
	KLK_LOCK_DI(&l);		/* the wrong way round */
	KLK_UNLOCK(&l);
	KLK_UNSCHED();
	enaint(s);
	KT_ASSERT_EQ(knl_klk_bad, before + 1);

	before = knl_klk_bad;
	s = disint();
	KLK_LOCK_DI(&l);
	KLK_SCHED_DI();			/* the right way */
	KLK_UNSCHED();
	KLK_UNLOCK(&l);
	enaint(s);
	KT_ASSERT_EQ(knl_klk_bad, before);
	knl_klk_bad -= 1;		/* the one made here on purpose */
	knl_klk_quiet = FALSE;
#else
	KT_SKIP("the order of the locks is not checked in this build");
#endif
}

/* ------------------------------------------------------------------------ */
/*
 * A semaphore given and taken by many tasks at once, with short limits,
 * waits ended by tk_rel_wai: every count given is taken once or is
 * still there at the end
 */
LOCAL ID		s_sem;
LOCAL volatile UW	s_given, s_taken, s_rlwai, s_tmout;
LOCAL ID		s_ids[NTSK];

LOCAL void s_giver( INT stacd, void *exinf )
{
	UW	r = (UW)stacd * 7919U + 1;

	while ( !k_stop ) {
		if ( tk_sig_sem(s_sem, 1) == E_OK ) {
			atomic_inc((UW *)&s_given);
		}
		if ( ( k_rand(&r) & 7 ) == 0 ) {
			tk_dly_tsk(1);
		}
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void s_taker( INT stacd, void *exinf )
{
	UW	r = (UW)stacd * 104729U + 3;
	ER	er;

	while ( !k_stop ) {
		er = tk_wai_sem_u(s_sem, 1, (TMO_U)( k_rand(&r) % 2000 ));
		if ( er == E_OK ) {
			atomic_inc((UW *)&s_taken);
		} else if ( er == E_RLWAI ) {
			atomic_inc((UW *)&s_rlwai);
		} else if ( er == E_TMOUT ) {
			atomic_inc((UW *)&s_tmout);
		} else {
			k_fault(er);
		}
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void s_breaker( INT stacd, void *exinf )
{
	UW	r = 77;

	while ( !k_stop ) {
		ER er = tk_rel_wai(s_ids[NTSK / 2 + (INT)( k_rand(&r) % ( NTSK / 2 ) )]);

		if ( er != E_OK && er != E_OBJ ) {
			k_fault(er);
		}
		tk_dly_tsk(1);
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void k_sem_run( BOOL spin )
{
	T_CSEM	cs;
	T_RSEM	rs;
	ID	brk;
	INT	i;
	ER	er;

	cs.exinf = NULL;
	cs.sematr = TA_TPRI | TA_CNT;
	cs.isemcnt = 0;
	cs.maxsem = 0x7fffffff;
	s_sem = tk_cre_sem(&cs);
	KT_ASSERT(s_sem > 0);
	s_given = s_taken = s_rlwai = s_tmout = 0;
	k_stop = FALSE;
	k_done = 0;
	k_spins(spin);
	for ( i = 0; i < NTSK; i++ ) {
		s_ids[i] = k_task(( i < NTSK / 2 ) ? (FP)s_giver : (FP)s_taker,
				  (PRI)( KT_PRI_LOW - ( i % 3 ) ), i);
		KT_ASSERT(s_ids[i] > 0);
	}
	brk = k_task((FP)s_breaker, KT_PRI_LOW, 0);
	KT_ASSERT(brk > 0);
	tk_dly_tsk(RUN_MS);
	k_stop = TRUE;
	k_wait_done(NTSK + 1);
	k_spins(FALSE);
	KT_ASSERT_EQ(k_done, NTSK + 1);

	/* what is left is taken here */
	while ( ( er = tk_wai_sem(s_sem, 1, TMO_POL) ) == E_OK ) {
		s_taken++;
	}
	KT_ASSERT_ER(er, E_TMOUT);
	KT_ASSERT_ER(tk_ref_sem(s_sem, &rs), E_OK);
	tm_printf((UB *)"  semaphore%s: given %d, taken %d, ended %d, timed out %d\n",
		  spin ? " (moments made long)" : "", s_given, s_taken, s_rlwai, s_tmout);
	KT_ASSERT_EQ(s_given, s_taken);
	KT_ASSERT_EQ(rs.wtsk, 0);
	for ( i = 0; i < NTSK; i++ ) {
		KT_ASSERT_ER(k_del(s_ids[i]), E_OK);
	}
	KT_ASSERT_ER(k_del(brk), E_OK);
	KT_ASSERT_ER(tk_del_sem(s_sem), E_OK);
}

LOCAL void test_klk_sem( void )
{
	k_bad = 0;
	k_sem_run(FALSE);
	k_sem_run(TRUE);
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */
/*
 * A chain of inheritance: C holds M3 and waits on a semaphore, B holds
 * M2 and waits for M3, A waits for M2. A's priority goes down the chain
 * to C; a change of A's priority goes down it again; A's limit and the
 * end of B take it back, one link at a time.
 */
LOCAL ID		c_m2, c_m3, c_sem, c_go;
LOCAL volatile ER	c_er_a, c_er_b;

LOCAL void c_task_c( INT stacd, void *exinf )
{
	if ( tk_loc_mtx(c_m3, TMO_FEVR) != E_OK ) k_fault(E_SYS);
	tk_sig_sem(c_go, 1);
	if ( tk_wai_sem(c_sem, 1, TMO_FEVR) != E_OK ) k_fault(E_SYS);
	if ( tk_unl_mtx(c_m3) != E_OK ) k_fault(E_SYS);
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void c_task_b( INT stacd, void *exinf )
{
	if ( tk_loc_mtx(c_m2, TMO_FEVR) != E_OK ) k_fault(E_SYS);
	tk_sig_sem(c_go, 1);
	c_er_b = tk_loc_mtx(c_m3, TMO_FEVR);
	tk_unl_mtx(c_m3);
	tk_unl_mtx(c_m2);
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void c_task_a( INT stacd, void *exinf )
{
	c_er_a = tk_loc_mtx(c_m2, (TMO)stacd);
	if ( c_er_a == E_OK ) tk_unl_mtx(c_m2);
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL PRI k_pri( ID tid )
{
	T_RTSK	r;

	return ( tk_ref_tsk(tid, &r) == E_OK ) ? r.tskpri : 0;
}

LOCAL BOOL k_waits( ID tid )
{
	T_RTSK	r;

	return ( tk_ref_tsk(tid, &r) == E_OK && ( r.tskstat & TTS_WAI ) != 0 );
}

/* PRI p reached by tid within a while */
LOCAL BOOL k_pri_is( ID tid, PRI p )
{
	INT	i;

	for ( i = 0; i < 200; i++ ) {
		if ( k_pri(tid) == p ) return TRUE;
		tk_dly_tsk(1);
	}
	return FALSE;
}

LOCAL BOOL k_until_waits( ID tid )
{
	INT	i;

	for ( i = 0; i < 500; i++ ) {
		if ( k_waits(tid) ) return TRUE;
		tk_dly_tsk(1);
	}
	return FALSE;
}

#define C_PRI_A		10
#define C_PRI_B		24
#define C_PRI_C		28

LOCAL void k_chain_run( BOOL spin )
{
	T_CMTX	cm;
	T_CSEM	cs;
	ID	a, b, c;
	INT	i;

	cm.exinf = NULL;
	cm.mtxatr = TA_INHERIT;
	cm.ceilpri = 0;
	c_m2 = tk_cre_mtx(&cm);
	c_m3 = tk_cre_mtx(&cm);
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 0;
	cs.maxsem = 10;
	c_sem = tk_cre_sem(&cs);
	c_go = tk_cre_sem(&cs);
	KT_ASSERT(c_m2 > 0 && c_m3 > 0 && c_sem > 0 && c_go > 0);
	k_done = 0;
	k_spins(spin);

	c = k_task((FP)c_task_c, C_PRI_C, 0);
	KT_ASSERT_ER(tk_wai_sem(c_go, 1, 2000), E_OK);
	KT_ASSERT(k_until_waits(c));			/* on the semaphore, holding M3 */
	b = k_task((FP)c_task_b, C_PRI_B, 0);
	KT_ASSERT_ER(tk_wai_sem(c_go, 1, 2000), E_OK);
	KT_ASSERT(k_until_waits(b));			/* for M3 */
	KT_ASSERT(k_pri_is(c, C_PRI_B));		/* B's, through M3 */

	c_er_a = 1;
	a = k_task((FP)c_task_a, C_PRI_A, 400);
	KT_ASSERT(k_until_waits(a));			/* for M2, 400ms */
	KT_ASSERT(k_pri_is(b, C_PRI_A));
	KT_ASSERT(k_pri_is(c, C_PRI_A));		/* down the chain */

	KT_ASSERT_ER(tk_chg_pri(a, C_PRI_A + 2), E_OK);
	KT_ASSERT(k_pri_is(b, C_PRI_A + 2));
	KT_ASSERT(k_pri_is(c, C_PRI_A + 2));

	/* A's limit: back to B's */
	for ( i = 0; i < 1000 && c_er_a == 1; i++ ) tk_dly_tsk(1);
	KT_ASSERT_ER(c_er_a, E_TMOUT);
	KT_ASSERT(k_pri_is(b, C_PRI_B));
	KT_ASSERT(k_pri_is(c, C_PRI_B));

	/* B ended while it waits for M3 and holds M2: C back to its own */
	KT_ASSERT_ER(tk_ter_tsk(b), E_OK);
	KT_ASSERT(k_pri_is(c, C_PRI_C));
	KT_ASSERT_ER(tk_sig_sem(c_sem, 1), E_OK);
	k_wait_done(2);
	k_spins(FALSE);
	KT_ASSERT_EQ(k_done, 2);			/* A and C ended themselves */
	KT_ASSERT_ER(k_del(a), E_OK);
	KT_ASSERT_ER(k_del(b), E_OK);
	KT_ASSERT_ER(k_del(c), E_OK);
	KT_ASSERT_ER(tk_del_mtx(c_m2), E_OK);
	KT_ASSERT_ER(tk_del_mtx(c_m3), E_OK);
	KT_ASSERT_ER(tk_del_sem(c_sem), E_OK);
	KT_ASSERT_ER(tk_del_sem(c_go), E_OK);
}

LOCAL void test_klk_chain( void )
{
	INT	i;

	if ( knl_num_prc < 2 ) KT_SKIP("single processor");
	k_bad = 0;
	for ( i = 0; i < 4; i++ ) {
		k_chain_run(( i & 1 ) != 0);
	}
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */
/*
 * Mutexes that inherit, taken in one order by many tasks with short
 * limits while their priorities are changed and their waits ended:
 * every mutex free and every priority its base one at the end
 */
#define NMTX	3

LOCAL ID		m_mtx[NMTX];
LOCAL ID		m_ids[NTSK];
LOCAL volatile UW	m_got;

LOCAL void m_worker( INT stacd, void *exinf )
{
	UW	r = (UW)stacd * 31337U + 5;
	INT	i, k, n;
	ER	er;
	BOOL	held[NMTX];

	while ( !k_stop ) {
		n = 0;
		for ( i = 0; i < NMTX; i++ ) {
			held[i] = FALSE;
			if ( ( k_rand(&r) & 1 ) == 0 ) continue;
			er = tk_loc_mtx_u(m_mtx[i], (TMO_U)( k_rand(&r) % 1500 ));
			if ( er == E_OK ) {
				held[i] = TRUE;
				n++;
			} else if ( er != E_TMOUT && er != E_RLWAI ) {
				k_fault(er);
			}
		}
		for ( k = 0; k < (INT)( k_rand(&r) % 200 ); k++ ) Asm("nop");
		for ( i = NMTX - 1; i >= 0; i-- ) {
			if ( held[i] && tk_unl_mtx(m_mtx[i]) != E_OK ) k_fault(E_SYS);
		}
		if ( n > 0 ) atomic_inc((UW *)&m_got);
	}
	atomic_inc((UW *)&k_done);
	tk_slp_tsk(TMO_FEVR);		/* alive, holding nothing, till it is ended */
	tk_ext_tsk();
}

LOCAL void m_shaker( INT stacd, void *exinf )
{
	UW	r = 4242;
	ER	er;

	while ( !k_stop ) {
		ID	t = m_ids[k_rand(&r) % NTSK];

		if ( ( k_rand(&r) & 3 ) == 0 ) {
			er = tk_rel_wai(t);
			if ( er != E_OK && er != E_OBJ ) k_fault(er);
		} else {
			er = tk_chg_pri(t, (PRI)( KT_PRI_LOW - 4 + (INT)( k_rand(&r) % 5 ) ));
			if ( er != E_OK ) k_fault(er);
		}
		tk_dly_tsk(1);
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void k_mtx_run( BOOL spin )
{
	T_CMTX	cm;
	T_RMTX	rm;
	T_RTSK	rt;
	ID	sh;
	INT	i;

	cm.exinf = NULL;
	cm.mtxatr = TA_INHERIT;
	cm.ceilpri = 0;
	for ( i = 0; i < NMTX; i++ ) {
		m_mtx[i] = tk_cre_mtx(&cm);
		KT_ASSERT(m_mtx[i] > 0);
	}
	m_got = 0;
	k_stop = FALSE;
	k_done = 0;
	k_spins(spin);
	for ( i = 0; i < NTSK; i++ ) {
		m_ids[i] = k_task((FP)m_worker, (PRI)( KT_PRI_LOW - ( i % 4 ) ), i);
		KT_ASSERT(m_ids[i] > 0);
	}
	sh = k_task((FP)m_shaker, KT_PRI_LOW - 5, 0);
	KT_ASSERT(sh > 0);
	tk_dly_tsk(RUN_MS);
	k_stop = TRUE;
	k_wait_done(NTSK + 1);
	k_spins(FALSE);
	KT_ASSERT_EQ(k_done, NTSK + 1);
	tm_printf((UB *)"  mutexes%s: rounds with one or more held %d\n", spin ? " (moments made long)" : "", m_got);
	for ( i = 0; i < NMTX; i++ ) {
		KT_ASSERT_ER(tk_ref_mtx(m_mtx[i], &rm), E_OK);
		KT_ASSERT_EQ(rm.htsk, 0);
		KT_ASSERT_EQ(rm.wtsk, 0);
		KT_ASSERT_ER(tk_del_mtx(m_mtx[i]), E_OK);
	}
	for ( i = 0; i < NTSK; i++ ) {
		KT_ASSERT_ER(tk_ref_tsk(m_ids[i], &rt), E_OK);	/* holding nothing: its base priority */
		KT_ASSERT_EQ(rt.tskpri, rt.tskbpri);
		KT_ASSERT_ER(tk_ter_tsk(m_ids[i]), E_OK);
		KT_ASSERT_ER(k_del(m_ids[i]), E_OK);
	}
	KT_ASSERT_ER(k_del(sh), E_OK);
	KT_ASSERT(m_got > 100);
}

LOCAL void test_klk_mtx( void )
{
	k_bad = 0;
	k_mtx_run(FALSE);
	k_mtx_run(TRUE);
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */
/*
 * A semaphore deleted while tasks wait on it, some without a limit and
 * some with a short one: each wait ends once, E_DLT or E_TMOUT
 */
#define D_ROUNDS	40
#define D_WAITERS	8

LOCAL ID		d_sem;
LOCAL volatile UW	d_dlt, d_tmo, d_late, d_dlt_all;

LOCAL void d_waiter( INT stacd, void *exinf )
{
	ER	er = tk_wai_sem_u(d_sem, 1, ( stacd & 1 ) ? (TMO_U)( 300 + stacd * 50 ) : TMO_FEVR);

	if ( er == E_DLT ) {
		atomic_inc((UW *)&d_dlt);
	} else if ( er == E_TMOUT ) {
		atomic_inc((UW *)&d_tmo);
	} else if ( er == E_NOEXS ) {
		atomic_inc((UW *)&d_late);	/* came after the deletion */
	} else {
		k_fault(er);
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL BOOL k_all_wait( ID *ids, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( !k_waits(ids[i]) ) return FALSE;
	}
	return TRUE;
}

LOCAL void test_klk_del( void )
{
	T_CSEM	cs;
	ID	ids[D_WAITERS];
	INT	r, i;

	k_bad = 0;
	d_dlt_all = 0;
	cs.exinf = NULL;
	cs.sematr = TA_TPRI;
	cs.isemcnt = 0;
	cs.maxsem = 1;
	for ( r = 0; r < D_ROUNDS; r++ ) {
		d_sem = tk_cre_sem(&cs);
		KT_ASSERT(d_sem > 0);
		d_dlt = d_tmo = d_late = 0;
		k_done = 0;
		k_spins(( r & 1 ) != 0);
		for ( i = 0; i < D_WAITERS; i++ ) {
			ids[i] = k_task((FP)d_waiter, (PRI)( KT_PRI_LOW - ( i % 3 ) ), i);
		}
		if ( ( r % 3 ) != 0 ) {
			/* mostly once all of them wait, now and then while they come */
			for ( i = 0; i < 200 && !k_all_wait(ids, D_WAITERS); i++ ) tk_dly_tsk(1);
		}
		KT_ASSERT_ER(tk_del_sem(d_sem), E_OK);
		k_wait_done(D_WAITERS);
		k_spins(FALSE);
		KT_ASSERT_EQ(k_done, D_WAITERS);
		KT_ASSERT_EQ(d_dlt + d_tmo + d_late, D_WAITERS);
		d_dlt_all += d_dlt;
		for ( i = 0; i < D_WAITERS; i++ ) {
			KT_ASSERT_ER(k_del(ids[i]), E_OK);
		}
	}
	tm_printf((UB *)"  waits ended by the deletion: %d of %d\n", d_dlt_all, D_ROUNDS * D_WAITERS);
	KT_ASSERT(d_dlt_all > D_ROUNDS * D_WAITERS / 3);
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */
/*
 * A message buffer between senders and receivers on all processors with
 * short limits: every message that was sent is received whole, once
 */
#define B_MSGSZ		32

LOCAL ID		b_mbf;
LOCAL volatile UW	b_sent, b_recv;

LOCAL void b_sender( INT stacd, void *exinf )
{
	UW	msg[B_MSGSZ / 4], r = (UW)stacd + 11, seq = 0;
	INT	i;
	ER	er;

	while ( !k_stop ) {
		msg[0] = (UW)stacd;
		msg[1] = seq;
		for ( i = 2; i < B_MSGSZ / 4; i++ ) msg[i] = msg[0] * 0x9E3779B1U + seq * (UW)i;
		er = tk_snd_mbf_u(b_mbf, msg, (INT)( 8 + ( k_rand(&r) % ( B_MSGSZ - 8 ) ) / 4 * 4 ), (TMO_U)( k_rand(&r) % 1500 ));
		if ( er == E_OK ) {
			atomic_inc((UW *)&b_sent);
			seq++;
		} else if ( er != E_TMOUT ) {
			k_fault(er);
		}
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void b_receiver( INT stacd, void *exinf )
{
	UW	msg[B_MSGSZ / 4], r = (UW)stacd + 99;
	INT	sz, i;

	while ( !k_stop ) {
		sz = tk_rcv_mbf_u(b_mbf, msg, (TMO_U)( k_rand(&r) % 1500 ));
		if ( sz >= 8 ) {
			for ( i = 2; i < sz / 4; i++ ) {
				if ( msg[i] != msg[0] * 0x9E3779B1U + msg[1] * (UW)i ) {
					k_fault(E_SYS);
					break;
				}
			}
			atomic_inc((UW *)&b_recv);
		} else if ( sz != E_TMOUT ) {
			k_fault(sz);
		}
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void k_mbf_run( BOOL spin )
{
	T_CMBF	cb;
	UW	msg[B_MSGSZ / 4];
	ID	ids[NTSK];
	INT	i, sz;

	cb.exinf = NULL;
	cb.mbfatr = TA_TPRI;
	cb.bufsz = 256;
	cb.maxmsz = B_MSGSZ;
	b_mbf = tk_cre_mbf(&cb);
	KT_ASSERT(b_mbf > 0);
	b_sent = b_recv = 0;
	k_stop = FALSE;
	k_done = 0;
	k_spins(spin);
	for ( i = 0; i < NTSK; i++ ) {
		ids[i] = k_task(( i & 1 ) ? (FP)b_receiver : (FP)b_sender, (PRI)( KT_PRI_LOW - ( i % 3 ) ), i);
		KT_ASSERT(ids[i] > 0);
	}
	tk_dly_tsk(RUN_MS);
	k_stop = TRUE;
	k_wait_done(NTSK);
	k_spins(FALSE);
	KT_ASSERT_EQ(k_done, NTSK);
	while ( ( sz = tk_rcv_mbf(b_mbf, msg, TMO_POL) ) >= 0 ) {
		b_recv++;
	}
	tm_printf((UB *)"  message buffer%s: sent %d, received %d\n", spin ? " (moments made long)" : "", b_sent, b_recv);
	KT_ASSERT_EQ(b_sent, b_recv);
	for ( i = 0; i < NTSK; i++ ) {
		KT_ASSERT_ER(k_del(ids[i]), E_OK);
	}
	KT_ASSERT_ER(tk_del_mbf(b_mbf), E_OK);
}

LOCAL void test_klk_mbf( void )
{
	k_bad = 0;
	k_mbf_run(FALSE);
	k_mbf_run(TRUE);
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */
/*
 * Cyclic and alarm handlers made, started, stopped and deleted on the
 * other processors while they fire: none is called after its deletion
 * has returned
 */
#define T_HDR		4

LOCAL volatile UW	t_calls[T_HDR], t_late;
LOCAL volatile BOOL	t_gone[T_HDR];

LOCAL void t_handler( void *exinf )
{
	INT	i = (INT)(UBINT)exinf;

	if ( t_gone[i] ) {
		atomic_inc((UW *)&t_late);
	}
	atomic_inc((UW *)&t_calls[i]);
}

LOCAL void t_churner( INT stacd, void *exinf )
{
	T_CCYC	cc;
	T_CALM	ca;
	UW	r = (UW)stacd + 5;
	ID	id;

	while ( !k_stop ) {
		t_gone[stacd] = FALSE;
		if ( stacd & 1 ) {
			cc.exinf = (void *)(UBINT)stacd;
			cc.cycatr = TA_HLNG | ( ( k_rand(&r) & 1 ) ? TA_STA : 0 );
			cc.cychdr = (FP)t_handler;
			cc.cyctim = 1;
			cc.cycphs = k_rand(&r) % 2;
			id = tk_cre_cyc(&cc);
			if ( id <= 0 ) { k_fault(id); break; }
			if ( k_rand(&r) & 1 ) tk_sta_cyc(id);
			tk_dly_tsk(k_rand(&r) % 3);
			if ( k_rand(&r) & 1 ) tk_stp_cyc(id);
			if ( tk_del_cyc(id) != E_OK ) k_fault(E_SYS);
		} else {
			ca.exinf = (void *)(UBINT)stacd;
			ca.almatr = TA_HLNG;
			ca.almhdr = (FP)t_handler;
			id = tk_cre_alm(&ca);
			if ( id <= 0 ) { k_fault(id); break; }
			tk_sta_alm(id, k_rand(&r) % 2);
			tk_dly_tsk(k_rand(&r) % 3);
			if ( k_rand(&r) & 1 ) tk_stp_alm(id);
			if ( tk_del_alm(id) != E_OK ) k_fault(E_SYS);
		}
		t_gone[stacd] = TRUE;
		tk_dly_tsk(1);			/* a call after the deletion would come now */
	}
	atomic_inc((UW *)&k_done);
	tk_ext_tsk();
}

LOCAL void test_klk_tev( void )
{
	ID	ids[T_HDR];
	INT	i;
	UW	calls = 0;

	k_bad = 0;
	t_late = 0;
	k_stop = FALSE;
	k_done = 0;
	for ( i = 0; i < T_HDR; i++ ) {
		t_calls[i] = 0;
		ids[i] = k_task((FP)t_churner, KT_PRI_LOW, i);
		KT_ASSERT(ids[i] > 0);
	}
	k_spins(TRUE);
	tk_dly_tsk(RUN_MS);
	k_stop = TRUE;
	k_wait_done(T_HDR);
	k_spins(FALSE);
	KT_ASSERT_EQ(k_done, T_HDR);
	for ( i = 0; i < T_HDR; i++ ) {
		calls += t_calls[i];
		KT_ASSERT_ER(k_del(ids[i]), E_OK);
	}
	tm_printf((UB *)"  time event handlers called %d, after their deletion %d\n", calls, t_late);
	KT_ASSERT_EQ(t_late, 0);
	KT_ASSERT(calls > 50);
	KT_ASSERT_EQ(k_bad, 0);
}

/* ------------------------------------------------------------------------ */

LOCAL void test_klk_faults( void )
{
#if CNF_KLOCK_CHECK
	tm_printf((UB *)"  faults of the order found in all: %d\n", knl_klk_bad);
	KT_ASSERT_EQ(knl_klk_bad, 0);
#else
	KT_SKIP("the order of the locks is not checked in this build");
#endif
}

EXPORT void ktest_klk( void )
{
	tm_printf((UB *)"  lock mode %d, %d processors\n", CNF_KLOCK_MODE, knl_num_prc);
	KT_RUN(test_klk_check);
	KT_RUN(test_klk_sem);
	KT_RUN(test_klk_chain);
	KT_RUN(test_klk_mtx);
	KT_RUN(test_klk_del);
	KT_RUN(test_klk_mbf);
	KT_RUN(test_klk_tev);
	KT_RUN(test_klk_faults);
}
