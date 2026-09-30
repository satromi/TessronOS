/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_sync.c
 *	Synchronization objects: semaphore, event flag, mailbox, mutex,
 *	message buffer. Rendezvous is skipped unless USE_LEGACY_API.
 */

#include "ktest.h"

LOCAL volatile INT	step;
LOCAL volatile ER	wait_ercd;
LOCAL ID		obj_id;
LOCAL volatile UINT	got_ptn;

/* ---- semaphore ---------------------------------------------------- */

LOCAL void t_sem_waiter( INT stacd, void *exinf )
{
	step = 1;
	wait_ercd = tk_wai_sem(obj_id, stacd, TMO_FEVR);
	step = 2;
	tk_ext_tsk();
}

LOCAL void test_semaphore( void )
{
	T_CSEM	csem;
	T_RSEM	rsem;
	ID	id, tid;

	csem.exinf = NULL;
	csem.sematr = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 1;
	csem.maxsem = 3;
	obj_id = id = tk_cre_sem(&csem);
	KT_ASSERT(id > 0);

	KT_ASSERT_ER(tk_wai_sem(id, 1, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_wai_sem(id, 1, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(tk_wai_sem(id, 1, 20), E_TMOUT);
	KT_ASSERT_ER(tk_sig_sem(id, 3), E_OK);
	KT_ASSERT_ER(tk_sig_sem(id, 1), E_QOVR);		/* over maxsem */
	KT_ASSERT_ER(tk_ref_sem(id, &rsem), E_OK);
	KT_ASSERT_EQ(rsem.semcnt, 3);
	KT_ASSERT_ER(tk_wai_sem(id, 3, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_wai_sem(id, 4, TMO_POL), E_PAR);	/* more than maxsem */
	KT_ASSERT_ER(tk_wai_sem(CNF_MAX_SEMID + 1, 1, TMO_POL), E_ID);

	/* a higher priority task blocks, then is released by tk_sig_sem */
	step = 0;
	tid = kt_cre_tsk((FP)t_sem_waiter, 5, NULL);
	KT_ASSERT_ER(tk_sta_tsk(tid, 2), E_OK);
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_ER(tk_ref_sem(id, &rsem), E_OK);
	KT_ASSERT_EQ(rsem.wtsk, tid);
	KT_ASSERT_ER(tk_sig_sem(id, 1), E_OK);
	KT_ASSERT_EQ(step, 1);				/* needs 2 */
	KT_ASSERT_ER(tk_sig_sem(id, 1), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_OK);

	/* deleting an object releases the waiter with E_DLT */
	step = 0;
	KT_ASSERT_ER(tk_sta_tsk(tid, 1), E_OK);
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_ER(tk_del_sem(id), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, E_DLT);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
	KT_ASSERT_ER(tk_ref_sem(id, &rsem), E_NOEXS);
}

/* ---- event flag ---------------------------------------------------- */

LOCAL void t_flg_waiter( INT stacd, void *exinf )
{
	UINT	ptn = 0;
	step = 1;
	wait_ercd = tk_wai_flg(obj_id, (UINT)stacd, TWF_ANDW | TWF_CLR, &ptn, TMO_FEVR);
	got_ptn = ptn;
	step = 2;
	tk_ext_tsk();
}

LOCAL void test_eventflag( void )
{
	T_CFLG	cflg;
	T_RFLG	rflg;
	ID	id, tid;
	UINT	ptn;

	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0x00000001;
	obj_id = id = tk_cre_flg(&cflg);
	KT_ASSERT(id > 0);

	KT_ASSERT_ER(tk_wai_flg(id, 0x1, TWF_ORW, &ptn, TMO_POL), E_OK);
	KT_ASSERT_EQ(ptn, 0x1);
	KT_ASSERT_ER(tk_wai_flg(id, 0x3, TWF_ANDW, &ptn, TMO_POL), E_TMOUT);
	KT_ASSERT_ER(tk_set_flg(id, 0x80000002), E_OK);	/* bit 31 must survive (32 bit pattern) */
	KT_ASSERT_ER(tk_ref_flg(id, &rflg), E_OK);
	KT_ASSERT_EQ(rflg.flgptn, 0x80000003);
	KT_ASSERT_ER(tk_wai_flg(id, 0x80000000, TWF_ANDW | TWF_CLR, &ptn, TMO_POL), E_OK);
	KT_ASSERT_EQ(ptn, 0x80000003);
	KT_ASSERT_ER(tk_ref_flg(id, &rflg), E_OK);
	KT_ASSERT_EQ(rflg.flgptn, 0);				/* TWF_CLR clears all */
	KT_ASSERT_ER(tk_set_flg(id, 0xff), E_OK);
	KT_ASSERT_ER(tk_wai_flg(id, 0x0f, TWF_ORW | TWF_BITCLR, &ptn, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_ref_flg(id, &rflg), E_OK);
	KT_ASSERT_EQ(rflg.flgptn, 0xf0);			/* TWF_BITCLR clears the waited bits */
	KT_ASSERT_ER(tk_clr_flg(id, 0x0f), E_OK);		/* AND with the mask */
	KT_ASSERT_ER(tk_ref_flg(id, &rflg), E_OK);
	KT_ASSERT_EQ(rflg.flgptn, 0);
	KT_ASSERT_ER(tk_wai_flg(id, 0, TWF_ORW, &ptn, TMO_POL), E_PAR);

	step = 0;
	tid = kt_cre_tsk((FP)t_flg_waiter, 5, NULL);
	KT_ASSERT_ER(tk_sta_tsk(tid, 0x30), E_OK);
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_ER(tk_set_flg(id, 0x10), E_OK);
	KT_ASSERT_EQ(step, 1);
	KT_ASSERT_ER(tk_set_flg(id, 0x20), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(got_ptn, 0x30);
	KT_ASSERT_EQ(wait_ercd, E_OK);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
	KT_ASSERT_ER(tk_del_flg(id), E_OK);
}

/* ---- mailbox ------------------------------------------------------- */

typedef struct {
	T_MSG	hdr;
	INT	value;
} KT_MSG;

LOCAL KT_MSG	msgs[4];

LOCAL void t_mbx_waiter( INT stacd, void *exinf )
{
	T_MSG	*m;
	step = 1;
	wait_ercd = tk_rcv_mbx(obj_id, &m, TMO_FEVR);
	got_ptn = (UINT)((KT_MSG *)m)->value;
	step = 2;
	tk_ext_tsk();
}

LOCAL void test_mailbox( void )
{
	T_CMBX	cmbx;
	T_MSG	*m;
	ID	id, tid;
	INT	i;

	cmbx.exinf = NULL;
	cmbx.mbxatr = TA_TFIFO | TA_MFIFO;
	obj_id = id = tk_cre_mbx(&cmbx);
	KT_ASSERT(id > 0);

	KT_ASSERT_ER(tk_rcv_mbx(id, &m, TMO_POL), E_TMOUT);
	for ( i = 0; i < 4; i++ ) {
		msgs[i].value = i + 100;
		KT_ASSERT_ER(tk_snd_mbx(id, &msgs[i].hdr), E_OK);
	}
	for ( i = 0; i < 4; i++ ) {				/* FIFO order */
		KT_ASSERT_ER(tk_rcv_mbx(id, &m, TMO_POL), E_OK);
		KT_ASSERT_EQ(((KT_MSG *)m)->value, i + 100);
	}
	KT_ASSERT_ER(tk_rcv_mbx(id, &m, 20), E_TMOUT);

	step = 0;
	tid = kt_cre_tsk((FP)t_mbx_waiter, 5, NULL);
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	KT_ASSERT_EQ(step, 1);
	msgs[0].value = 55;
	KT_ASSERT_ER(tk_snd_mbx(id, &msgs[0].hdr), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(got_ptn, 55);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
	KT_ASSERT_ER(tk_del_mbx(id), E_OK);
}

/* ---- mutex --------------------------------------------------------- */

LOCAL void t_mtx_holder( INT stacd, void *exinf )
{
	step = 1;
	tk_loc_mtx(obj_id, TMO_FEVR);
	step = 2;
	tk_slp_tsk(TMO_FEVR);			/* holds the mutex while sleeping */
	step = 3;
	tk_unl_mtx(obj_id);
	step = 4;
	tk_ext_tsk();
}

LOCAL void t_mtx_waiter( INT stacd, void *exinf )
{
	wait_ercd = tk_loc_mtx(obj_id, stacd);
	if ( wait_ercd == E_OK ) tk_unl_mtx(obj_id);
	step = 10;
	tk_ext_tsk();
}

LOCAL void test_mutex( void )
{
	T_CMTX	cmtx;
	T_RTSK	rtsk;
	ID	id, holder, waiter;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_INHERIT;
	cmtx.ceilpri = 0;
	obj_id = id = tk_cre_mtx(&cmtx);
	KT_ASSERT(id > 0);

	KT_ASSERT_ER(tk_loc_mtx(id, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_loc_mtx(id, TMO_POL), E_ILUSE);	/* already held by self */
	KT_ASSERT_ER(tk_unl_mtx(id), E_OK);
	KT_ASSERT_ER(tk_unl_mtx(id), E_ILUSE);		/* not held */

	/* priority inheritance: holder (pri 12) is raised to the waiter's (pri 6) */
	step = 0;
	holder = kt_cre_tsk((FP)t_mtx_holder, 12, NULL);
	waiter = kt_cre_tsk((FP)t_mtx_waiter, 6, NULL);
	KT_ASSERT_ER(tk_sta_tsk(holder, 0), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	KT_ASSERT_EQ(step, 2);				/* holder sleeps with the mutex */
	KT_ASSERT_ER(tk_sta_tsk(waiter, TMO_FEVR), E_OK);	/* blocks on the mutex */
	KT_ASSERT_ER(tk_ref_tsk(holder, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskpri, 6);			/* inherited */
	KT_ASSERT_EQ(rtsk.tskbpri, 12);
	KT_ASSERT_ER(tk_wup_tsk(holder), E_OK);		/* holder releases, waiter gets it */
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	KT_ASSERT_EQ(step, 4);				/* waiter (pri 6) ran before the holder's exit */
	KT_ASSERT_EQ(wait_ercd, E_OK);
	KT_ASSERT_ER(tk_ref_tsk(waiter, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);
	KT_ASSERT_ER(tk_ref_tsk(holder, &rtsk), E_OK);
	KT_ASSERT_EQ(rtsk.tskstat, TTS_DMT);
	KT_ASSERT_ER(tk_del_tsk(holder), E_OK);
	KT_ASSERT_ER(tk_del_tsk(waiter), E_OK);

	/* timeout while another task holds it */
	step = 0;
	holder = kt_cre_tsk((FP)t_mtx_holder, 12, NULL);
	waiter = kt_cre_tsk((FP)t_mtx_waiter, 6, NULL);
	KT_ASSERT_ER(tk_sta_tsk(holder, 0), E_OK);
	KT_ASSERT_ER(tk_dly_tsk(10), E_OK);
	KT_ASSERT_ER(tk_sta_tsk(waiter, 20), E_OK);	/* 20 ms timeout */
	KT_ASSERT_ER(tk_dly_tsk(50), E_OK);
	KT_ASSERT_EQ(step, 10);
	KT_ASSERT_EQ(wait_ercd, E_TMOUT);
	KT_ASSERT_ER(tk_ter_tsk(holder), E_OK);		/* mutex released on termination */
	KT_ASSERT_ER(tk_loc_mtx(id, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_unl_mtx(id), E_OK);
	KT_ASSERT_ER(tk_del_tsk(holder), E_OK);
	KT_ASSERT_ER(tk_del_tsk(waiter), E_OK);
	KT_ASSERT_ER(tk_del_mtx(id), E_OK);
}

/* ---- message buffer ------------------------------------------------ */

LOCAL void t_mbf_waiter( INT stacd, void *exinf )
{
	UB	buf[64];
	INT	n;
	step = 1;
	n = tk_rcv_mbf(obj_id, buf, TMO_FEVR);
	wait_ercd = n;
	got_ptn = (n > 0) ? buf[0] : 0;
	step = 2;
	tk_ext_tsk();
}

LOCAL void test_messagebuf( void )
{
	T_CMBF	cmbf;
	T_RMBF	rmbf;
	ID	id, tid;
	UB	buf[64];
	INT	i, n;

	cmbf.exinf = NULL;
	cmbf.mbfatr = TA_TFIFO;
	cmbf.bufsz = 64;
	cmbf.maxmsz = 24;
	obj_id = id = tk_cre_mbf(&cmbf);
	KT_ASSERT(id > 0);

	KT_ASSERT_ER(tk_snd_mbf(id, "hello", 5, TMO_POL), E_OK);
	KT_ASSERT_ER(tk_snd_mbf(id, buf, 25, TMO_POL), E_PAR);	/* over maxmsz */
	n = tk_rcv_mbf(id, buf, TMO_POL);
	KT_ASSERT_EQ(n, 5);
	KT_ASSERT_EQ(buf[0], 'h');
	KT_ASSERT_EQ(tk_rcv_mbf(id, buf, TMO_POL), E_TMOUT);

	/* fill until it is full, then drain (exercises wrap-around) */
	for ( i = 0; i < 20; i++ ) {
		buf[0] = (UB)i;
		if ( tk_snd_mbf(id, buf, 12, TMO_POL) != E_OK ) break;
	}
	KT_ASSERT(i >= 3 && i < 20);
	KT_ASSERT_ER(tk_ref_mbf(id, &rmbf), E_OK);
	KT_ASSERT(rmbf.frbufsz < 16);
	for ( n = 0; n < i; n++ ) {
		KT_ASSERT_EQ(tk_rcv_mbf(id, buf, TMO_POL), 12);
		KT_ASSERT_EQ(buf[0], n);
	}
	KT_ASSERT_EQ(tk_rcv_mbf(id, buf, TMO_POL), E_TMOUT);

	step = 0;
	tid = kt_cre_tsk((FP)t_mbf_waiter, 5, NULL);
	KT_ASSERT_ER(tk_sta_tsk(tid, 0), E_OK);
	KT_ASSERT_EQ(step, 1);
	buf[0] = 0x5a;
	KT_ASSERT_ER(tk_snd_mbf(id, buf, 3, TMO_POL), E_OK);
	KT_ASSERT_EQ(step, 2);
	KT_ASSERT_EQ(wait_ercd, 3);
	KT_ASSERT_EQ(got_ptn, 0x5a);
	KT_ASSERT_ER(tk_del_tsk(tid), E_OK);
	KT_ASSERT_ER(tk_del_mbf(id), E_OK);
}

LOCAL void test_rendezvous( void )
{
#if USE_LEGACY_API
	KT_SKIP("not implemented in ktest yet");
#else
	KT_SKIP("USE_LEGACY_API=0");
#endif
}

EXPORT void ktest_sync( void )
{
	KT_RUN(test_semaphore);
	KT_RUN(test_eventflag);
	KT_RUN(test_mailbox);
	KT_RUN(test_mutex);
	KT_RUN(test_messagebuf);
	KT_RUN(test_rendezvous);
}
