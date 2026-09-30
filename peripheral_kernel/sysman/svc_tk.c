/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svc_tk.c
 *	The calls of the core (class 0) from a task of a process (design 9.7)
 *
 *	The objects of the core a process makes -- tasks, semaphores, event
 *	flags, mutexes and message buffers -- are its own, and it can name
 *	no others: not another process's, not the kernel's. Their IDs are
 *	names private to the process, as descriptors are (design 18.19, R2):
 *	the core looks at the owner of the object inside each call made
 *	here (knl_not_owner), and anyone else's is E_ID. When the process
 *	ends, they go with it (proc.c).
 *
 *	What is more:
 *	- A process has room for so many objects of each kind, and all
 *	  processes together for so many (knl_prc_tk_cre): E_LIMIT.
 *	- Its main task is ended only by the process ending: tk_ter_tsk,
 *	  tk_del_tsk and tk_sta_tsk on it are E_OACV, and tk_ext_tsk or
 *	  tk_exd_tsk of the main task ends the process (ts_ext_prc). Its
 *	  other tasks are its own to end.
 *	- A task is not ended, held still, released from a wait or woken by
 *	  another while it is inside a call of the kernel's layers (its
 *	  svcdepth: anything but a wait of the core): stopped there it would
 *	  keep a lock or leave a layer half done. E_OBJ; the caller may try
 *	  again when the call is over.
 *	- Tasks run at protection level 3 in the process's space, and at no
 *	  higher priority than the process was started with (E_PAR).
 *	- Mailboxes pass pointers the core follows, and cyclic and alarm
 *	  handlers run in the kernel: a process has none of them (E_NOSPT),
 *	  nor tk_dis_dsp and tk_ena_dsp.
 *	- Every pointer is made sure of (E_MACV), and what the core reads or
 *	  writes goes through the kernel's own memory.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/dt.h>

#define F(fn)		((FP)(fn))

#define MBF_BUF_MAX	( 16 * 1024 )	/* bytes of a process's message buffer */
#define MBF_MSG_MAX	1024		/* bytes of one of its messages */
#define FREEZE_TRIES	200		/* ms to wait for a task to leave another processor */

IMPORT ID  knl_prc_self( ID *p_main, PRI *p_base );
IMPORT ID  knl_prc_tk_cre( INT kind, CONST void *pk );
IMPORT ID  knl_prc_cre_tsk( CONST T_CTSK *pk );

/*
 * The calling task, and work of the kernel's done for it under the
 * process table's lock: while it is, the task is not to be held still.
 */
#define ME()		knl_tsk_self()
#define HELD(t, stmt)	do { (t)->svcdepth++; stmt; (t)->svcdepth--; } while ( 0 )

/* A call of the core made as the process: it reaches the process's own objects only */
#define CORE(t, er, call) \
	do { knl_svc_own((t)->owner); er = (call); knl_svc_own(0); } while ( 0 )

LOCAL BOOL uok( TCB *t, CONST void *p, SZ len, BOOL write )
{
	BOOL	ok;

	HELD(t, ok = knl_prc_user_ok(t->owner, p, len, write));
	return ok;
}

LOCAL ID main_task( TCB *t, PRI *p_base )
{
	ID	main = 0;

	HELD(t, (void)knl_prc_self(&main, p_base));
	return main;
}

/*
 * Another task of the process held still: suspended as the process
 * (anyone else's is E_ID), and off every processor. E_OBJ, and it goes
 * on as it was, when it is inside a call of the kernel's layers; E_OK
 * with it suspended once more than it was otherwise, for the caller to
 * resume when it has done what it came for.
 */
LOCAL ER freeze( TCB *t, ID tid )
{
	T_RTSK	r;
	TCB	*o;
	ER	er;
	INT	n;

	CORE(t, er, tk_sus_tsk(tid));
	if ( er < E_OK ) {
		return er;
	}
	r.prcid = 0;
	for ( n = 0; n < FREEZE_TRIES; n++ ) {
		if ( tk_ref_tsk(tid, &r) < E_OK || r.prcid == 0 ) {
			break;
		}
		tk_dly_tsk(1);
	}
	o = knl_tsk_tcb(tid);
	if ( r.prcid != 0 || o == NULL || o->svcdepth > 0 ) {
		(void)tk_rsm_tsk(tid);
		return E_OBJ;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- tasks */

LOCAL ID svc_tk_cre_tsk( CONST T_CTSK *pk )
{
	TCB	*t = ME();
	T_CTSK	c;
	ID	tid;

	if ( !uok(t, pk, sizeof(*pk), FALSE) ) return E_MACV;
	c = *pk;
	HELD(t, tid = knl_prc_cre_tsk(&c));
	return tid;
}

LOCAL ER svc_tk_del_tsk( ID tid )
{
	TCB	*t = ME();
	ER	er;

	if ( tid != TSK_SELF && tid == main_task(t, NULL) ) return E_OACV;
	CORE(t, er, tk_del_tsk(tid));
	return er;
}

LOCAL ER svc_tk_sta_tsk( ID tid, INT stacd )
{
	TCB	*t = ME();
	ER	er;

	if ( tid != TSK_SELF && tid == main_task(t, NULL) ) return E_OACV;
	CORE(t, er, tk_sta_tsk(tid, stacd));
	return er;
}

/* The main task ending ends the process */
LOCAL void svc_tk_ext_tsk( void )
{
	TCB	*t = ME();

	if ( t->tskid == main_task(t, NULL) ) {
		ts_ext_prc(0);
	}
	tk_ext_tsk();
}

LOCAL void svc_tk_exd_tsk( void )
{
	TCB	*t = ME();

	if ( t->tskid == main_task(t, NULL) ) {
		ts_ext_prc(0);
	}
	tk_exd_tsk();
}

LOCAL ER svc_tk_ter_tsk( ID tid )
{
	TCB	*t = ME();
	ER	er;

	if ( tid != TSK_SELF && tid == main_task(t, NULL) ) return E_OACV;
	er = freeze(t, tid);
	if ( er < E_OK ) return er;
	er = tk_ter_tsk(tid);
	if ( er < E_OK ) {
		(void)tk_rsm_tsk(tid);
	}
	return er;
}

LOCAL ER svc_tk_sus_tsk( ID tid )
{
	return freeze(ME(), tid);
}

LOCAL ER svc_tk_rsm_tsk( ID tid )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_rsm_tsk(tid));
	return er;
}

LOCAL ER svc_tk_frsm_tsk( ID tid )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_frsm_tsk(tid));
	return er;
}

LOCAL ER svc_tk_rel_wai( ID tid )
{
	TCB	*t = ME();
	ER	er;

	er = freeze(t, tid);
	if ( er < E_OK ) return er;
	er = tk_rel_wai(tid);
	(void)tk_rsm_tsk(tid);
	return er;
}

LOCAL ER svc_tk_wup_tsk( ID tid )
{
	TCB	*t = ME();
	ER	er;

	er = freeze(t, tid);
	if ( er < E_OK ) return er;
	er = tk_wup_tsk(tid);
	(void)tk_rsm_tsk(tid);
	return er;
}

LOCAL INT svc_tk_can_wup( ID tid )
{
	TCB	*t = ME();
	INT	n;

	if ( tid == TSK_SELF || tid == t->tskid ) {
		CORE(t, n, tk_can_wup(tid));
		return n;
	}
	n = freeze(t, tid);
	if ( n < E_OK ) return n;
	n = tk_can_wup(tid);
	(void)tk_rsm_tsk(tid);
	return n;
}

LOCAL ER svc_tk_chg_pri( ID tid, PRI pri )
{
	TCB	*t = ME();
	PRI	base = 0;
	ER	er;

	(void)main_task(t, &base);
	if ( pri != TPRI_INI && pri < base ) return E_PAR;
	CORE(t, er, tk_chg_pri(tid, pri));
	return er;
}

LOCAL ER svc_tk_rot_rdq( PRI pri )
{
	TCB	*t = ME();
	PRI	base = 0;

	(void)main_task(t, &base);
	if ( pri != TPRI_RUN && pri < base ) return E_PAR;
	return tk_rot_rdq(pri);
}

LOCAL ER svc_tk_ref_tsk( ID tid, T_RTSK *pk )
{
	TCB	*t = ME();
	T_RTSK	r;
	ER	er;

	if ( !uok(t, pk, sizeof(*pk), TRUE) ) return E_MACV;
	CORE(t, er, tk_ref_tsk(tid, &r));
	if ( er >= E_OK ) *pk = r;
	return er;
}

LOCAL ER svc_tk_nospt( void )
{
	return E_NOSPT;
}

/* ---------------------------------------------------------------- semaphores */

LOCAL ID svc_tk_cre_sem( CONST T_CSEM *pk )
{
	TCB	*t = ME();
	T_CSEM	c;
	ID	id;

	if ( !uok(t, pk, sizeof(*pk), FALSE) ) return E_MACV;
	c = *pk;
	HELD(t, id = knl_prc_tk_cre(TK_OWN_SEM, &c));
	return id;
}

LOCAL ER svc_tk_del_sem( ID id )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_del_sem(id));
	return er;
}

LOCAL ER svc_tk_sig_sem( ID id, INT cnt )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_sig_sem(id, cnt));
	return er;
}

LOCAL ER svc_tk_wai_sem( ID id, INT cnt, TMO tmout )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_wai_sem(id, cnt, tmout));
	return er;
}

LOCAL ER svc_tk_ref_sem( ID id, T_RSEM *pk )
{
	TCB	*t = ME();
	T_RSEM	r;
	ER	er;

	if ( !uok(t, pk, sizeof(*pk), TRUE) ) return E_MACV;
	CORE(t, er, tk_ref_sem(id, &r));
	if ( er >= E_OK ) *pk = r;
	return er;
}

/* ---------------------------------------------------------------- event flags */

LOCAL ID svc_tk_cre_flg( CONST T_CFLG *pk )
{
	TCB	*t = ME();
	T_CFLG	c;
	ID	id;

	if ( !uok(t, pk, sizeof(*pk), FALSE) ) return E_MACV;
	c = *pk;
	HELD(t, id = knl_prc_tk_cre(TK_OWN_FLG, &c));
	return id;
}

LOCAL ER svc_tk_del_flg( ID id )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_del_flg(id));
	return er;
}

LOCAL ER svc_tk_set_flg( ID id, UINT ptn )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_set_flg(id, ptn));
	return er;
}

LOCAL ER svc_tk_clr_flg( ID id, UINT ptn )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_clr_flg(id, ptn));
	return er;
}

/* The pattern comes back in the kernel's memory: the task that sets it writes it there */
LOCAL ER svc_tk_wai_flg( ID id, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO tmout )
{
	TCB	*t = ME();
	UINT	ptn = 0;
	ER	er;

	if ( !uok(t, p_flgptn, sizeof(*p_flgptn), TRUE) ) return E_MACV;
	CORE(t, er, tk_wai_flg(id, waiptn, wfmode, &ptn, tmout));
	if ( er >= E_OK ) *p_flgptn = ptn;
	return er;
}

LOCAL ER svc_tk_ref_flg( ID id, T_RFLG *pk )
{
	TCB	*t = ME();
	T_RFLG	r;
	ER	er;

	if ( !uok(t, pk, sizeof(*pk), TRUE) ) return E_MACV;
	CORE(t, er, tk_ref_flg(id, &r));
	if ( er >= E_OK ) *pk = r;
	return er;
}

/* ---------------------------------------------------------------- mutexes */

/* A ceiling above the process's priority would lift it there */
LOCAL ID svc_tk_cre_mtx( CONST T_CMTX *pk )
{
	TCB	*t = ME();
	T_CMTX	c;
	PRI	base = 0;
	ID	id;

	if ( !uok(t, pk, sizeof(*pk), FALSE) ) return E_MACV;
	c = *pk;
	(void)main_task(t, &base);
	if ( ( c.mtxatr & TA_CEILING ) == TA_CEILING && c.ceilpri < base ) return E_PAR;
	HELD(t, id = knl_prc_tk_cre(TK_OWN_MTX, &c));
	return id;
}

LOCAL ER svc_tk_del_mtx( ID id )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_del_mtx(id));
	return er;
}

LOCAL ER svc_tk_loc_mtx( ID id, TMO tmout )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_loc_mtx(id, tmout));
	return er;
}

LOCAL ER svc_tk_unl_mtx( ID id )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_unl_mtx(id));
	return er;
}

LOCAL ER svc_tk_ref_mtx( ID id, T_RMTX *pk )
{
	TCB	*t = ME();
	T_RMTX	r;
	ER	er;

	if ( !uok(t, pk, sizeof(*pk), TRUE) ) return E_MACV;
	CORE(t, er, tk_ref_mtx(id, &r));
	if ( er >= E_OK ) *pk = r;
	return er;
}

/* ---------------------------------------------------------------- message buffers */

/*
 * The buffer is the kernel's, of a size a process may have; a message
 * passes through the kernel's memory both ways (on the task's own
 * stack, which stays while the task waits), so the core never reads or
 * writes a process's page, and a task ended while it waits leaves
 * nothing behind.
 */
LOCAL ID svc_tk_cre_mbf( CONST T_CMBF *pk )
{
	TCB	*t = ME();
	T_CMBF	c;
	ID	id;

	if ( !uok(t, pk, sizeof(*pk), FALSE) ) return E_MACV;
	c = *pk;
	if ( ( c.mbfatr & TA_USERBUF ) != 0 ) return E_RSATR;
	if ( c.bufsz < 0 || c.bufsz > MBF_BUF_MAX || c.maxmsz <= 0 || c.maxmsz > MBF_MSG_MAX ) {
		return E_PAR;
	}
	c.bufptr = NULL;
	HELD(t, id = knl_prc_tk_cre(TK_OWN_MBF, &c));
	return id;
}

LOCAL ER svc_tk_del_mbf( ID id )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_del_mbf(id));
	return er;
}

LOCAL ER svc_tk_snd_mbf( ID id, CONST void *msg, INT msgsz, TMO tmout )
{
	TCB	*t = ME();
	UB	m[MBF_MSG_MAX];
	ER	er;

	if ( msgsz <= 0 || msgsz > MBF_MSG_MAX ) return E_PAR;
	if ( !uok(t, msg, msgsz, FALSE) ) return E_MACV;
	knl_memcpy(m, msg, msgsz);
	CORE(t, er, tk_snd_mbf(id, m, msgsz, tmout));
	return er;
}

LOCAL INT svc_tk_rcv_mbf( ID id, void *msg, TMO tmout )
{
	TCB	*t = ME();
	UB	m[MBF_MSG_MAX];
	T_RMBF	r;
	INT	n;

	CORE(t, n, tk_ref_mbf(id, &r));
	if ( n < E_OK ) return n;
	if ( !uok(t, msg, r.maxmsz, TRUE) ) return E_MACV;
	CORE(t, n, tk_rcv_mbf(id, m, tmout));
	if ( n > 0 ) knl_memcpy(msg, m, n);
	return n;
}

LOCAL ER svc_tk_ref_mbf( ID id, T_RMBF *pk )
{
	TCB	*t = ME();
	T_RMBF	r;
	ER	er;

	if ( !uok(t, pk, sizeof(*pk), TRUE) ) return E_MACV;
	CORE(t, er, tk_ref_mbf(id, &r));
	if ( er >= E_OK ) *pk = r;
	return er;
}

/* ---------------------------------------------------------------- time */

/*
 * The time set by a process: only by one that may write the clock
 * device, and told on it (design 12.3.1).
 */
LOCAL ER set_told( ER er )
{
	if ( er >= E_OK ) knl_dt_time_set();
	return er;
}

LOCAL ER svc_tk_set_tim( CONST SYSTIM *pk )
{
	SYSTIM	s;
	ER	er;

	if ( !uok(ME(), pk, sizeof(*pk), FALSE) ) return E_MACV;
	s = *pk;
	if ( ( er = knl_dt_may_set() ) < E_OK ) return er;
	return set_told(tk_set_tim(&s));
}

LOCAL ER svc_tk_get_tim( SYSTIM *pk )
{
	SYSTIM	s;
	ER	er;

	if ( !uok(ME(), pk, sizeof(*pk), TRUE) ) return E_MACV;
	er = tk_get_tim(&s);
	if ( er >= E_OK ) *pk = s;
	return er;
}

LOCAL ER svc_tk_get_otm( SYSTIM *pk )
{
	SYSTIM	s;
	ER	er;

	if ( !uok(ME(), pk, sizeof(*pk), TRUE) ) return E_MACV;
	er = tk_get_otm(&s);
	if ( er >= E_OK ) *pk = s;
	return er;
}

LOCAL ER svc_tk_set_utc( CONST SYSTIM *pk )
{
	SYSTIM	s;
	ER	er;

	if ( !uok(ME(), pk, sizeof(*pk), FALSE) ) return E_MACV;
	s = *pk;
	if ( ( er = knl_dt_may_set() ) < E_OK ) return er;
	return set_told(tk_set_utc(&s));
}

LOCAL ER svc_tk_get_utc( SYSTIM *pk )
{
	SYSTIM	s;
	ER	er;

	if ( !uok(ME(), pk, sizeof(*pk), TRUE) ) return E_MACV;
	er = tk_get_utc(&s);
	if ( er >= E_OK ) *pk = s;
	return er;
}

#if USE_TIME_US_API
LOCAL ER svc_tk_set_tim_u( SYSTIM_U tim )
{
	ER	er = knl_dt_may_set();

	return ( er < E_OK ) ? er : set_told(tk_set_tim_u(tim));
}

LOCAL ER svc_tk_wai_sem_u( ID id, INT cnt, TMO_U tmout )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_wai_sem_u(id, cnt, tmout));
	return er;
}

LOCAL ER svc_tk_wai_flg_u( ID id, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO_U tmout )
{
	TCB	*t = ME();
	UINT	ptn = 0;
	ER	er;

	if ( !uok(t, p_flgptn, sizeof(*p_flgptn), TRUE) ) return E_MACV;
	CORE(t, er, tk_wai_flg_u(id, waiptn, wfmode, &ptn, tmout));
	if ( er >= E_OK ) *p_flgptn = ptn;
	return er;
}

LOCAL ER svc_tk_loc_mtx_u( ID id, TMO_U tmout )
{
	TCB	*t = ME();
	ER	er;

	CORE(t, er, tk_loc_mtx_u(id, tmout));
	return er;
}

LOCAL ER svc_tk_snd_mbf_u( ID id, CONST void *msg, INT msgsz, TMO_U tmout )
{
	TCB	*t = ME();
	UB	m[MBF_MSG_MAX];
	ER	er;

	if ( msgsz <= 0 || msgsz > MBF_MSG_MAX ) return E_PAR;
	if ( !uok(t, msg, msgsz, FALSE) ) return E_MACV;
	knl_memcpy(m, msg, msgsz);
	CORE(t, er, tk_snd_mbf_u(id, m, msgsz, tmout));
	return er;
}

LOCAL INT svc_tk_rcv_mbf_u( ID id, void *msg, TMO_U tmout )
{
	TCB	*t = ME();
	UB	m[MBF_MSG_MAX];
	T_RMBF	r;
	INT	n;

	CORE(t, n, tk_ref_mbf(id, &r));
	if ( n < E_OK ) return n;
	if ( !uok(t, msg, r.maxmsz, TRUE) ) return E_MACV;
	CORE(t, n, tk_rcv_mbf_u(id, m, tmout));
	if ( n > 0 ) knl_memcpy(msg, m, n);
	return n;
}

LOCAL ER svc_tk_get_tim_u( SYSTIM_U *tim, UW *ofs )
{
	TCB	*t = ME();
	SYSTIM_U s;
	UW	o = 0;
	ER	er;

	if ( !uok(t, tim, sizeof(*tim), TRUE) ) return E_MACV;
	if ( ofs != NULL && !uok(t, ofs, sizeof(*ofs), TRUE) ) return E_MACV;
	er = tk_get_tim_u(&s, &o);
	if ( er >= E_OK ) {
		*tim = s;
		if ( ofs != NULL ) *ofs = o;
	}
	return er;
}

LOCAL ER svc_tk_get_otm_u( SYSTIM_U *tim, UW *ofs )
{
	TCB	*t = ME();
	SYSTIM_U s;
	UW	o = 0;
	ER	er;

	if ( !uok(t, tim, sizeof(*tim), TRUE) ) return E_MACV;
	if ( ofs != NULL && !uok(t, ofs, sizeof(*ofs), TRUE) ) return E_MACV;
	er = tk_get_otm_u(&s, &o);
	if ( er >= E_OK ) {
		*tim = s;
		if ( ofs != NULL ) *ofs = o;
	}
	return er;
}
#endif /* USE_TIME_US_API */

/*
 * Class 0 for a task of a process. A call that names nothing and
 * touches no memory of the caller's goes to the core as it is.
 */
EXPORT const FP knl_tk_prc_tbl[TSN_TK_MAX + 1] = {
	[TSN_NUMBER(TSN_TK_CRE_TSK)]	= F(svc_tk_cre_tsk),
	[TSN_NUMBER(TSN_TK_DEL_TSK)]	= F(svc_tk_del_tsk),
	[TSN_NUMBER(TSN_TK_STA_TSK)]	= F(svc_tk_sta_tsk),
	[TSN_NUMBER(TSN_TK_EXT_TSK)]	= F(svc_tk_ext_tsk),
	[TSN_NUMBER(TSN_TK_EXD_TSK)]	= F(svc_tk_exd_tsk),
	[TSN_NUMBER(TSN_TK_TER_TSK)]	= F(svc_tk_ter_tsk),
	[TSN_NUMBER(TSN_TK_DIS_DSP)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_ENA_DSP)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_CHG_PRI)]	= F(svc_tk_chg_pri),
	[TSN_NUMBER(TSN_TK_ROT_RDQ)]	= F(svc_tk_rot_rdq),
	[TSN_NUMBER(TSN_TK_REL_WAI)]	= F(svc_tk_rel_wai),
	[TSN_NUMBER(TSN_TK_GET_TID)]	= F(tk_get_tid),
	[TSN_NUMBER(TSN_TK_REF_TSK)]	= F(svc_tk_ref_tsk),
	[TSN_NUMBER(TSN_TK_SUS_TSK)]	= F(svc_tk_sus_tsk),
	[TSN_NUMBER(TSN_TK_RSM_TSK)]	= F(svc_tk_rsm_tsk),
	[TSN_NUMBER(TSN_TK_FRSM_TSK)]	= F(svc_tk_frsm_tsk),
	[TSN_NUMBER(TSN_TK_SLP_TSK)]	= F(tk_slp_tsk),
	[TSN_NUMBER(TSN_TK_WUP_TSK)]	= F(svc_tk_wup_tsk),
	[TSN_NUMBER(TSN_TK_CAN_WUP)]	= F(svc_tk_can_wup),
	[TSN_NUMBER(TSN_TK_CRE_SEM)]	= F(svc_tk_cre_sem),
	[TSN_NUMBER(TSN_TK_DEL_SEM)]	= F(svc_tk_del_sem),
	[TSN_NUMBER(TSN_TK_SIG_SEM)]	= F(svc_tk_sig_sem),
	[TSN_NUMBER(TSN_TK_WAI_SEM)]	= F(svc_tk_wai_sem),
	[TSN_NUMBER(TSN_TK_REF_SEM)]	= F(svc_tk_ref_sem),
	[TSN_NUMBER(TSN_TK_CRE_FLG)]	= F(svc_tk_cre_flg),
	[TSN_NUMBER(TSN_TK_DEL_FLG)]	= F(svc_tk_del_flg),
	[TSN_NUMBER(TSN_TK_SET_FLG)]	= F(svc_tk_set_flg),
	[TSN_NUMBER(TSN_TK_CLR_FLG)]	= F(svc_tk_clr_flg),
	[TSN_NUMBER(TSN_TK_WAI_FLG)]	= F(svc_tk_wai_flg),
	[TSN_NUMBER(TSN_TK_REF_FLG)]	= F(svc_tk_ref_flg),
	[TSN_NUMBER(TSN_TK_CRE_MBX)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_DEL_MBX)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_SND_MBX)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_RCV_MBX)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_REF_MBX)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_CRE_MTX)]	= F(svc_tk_cre_mtx),
	[TSN_NUMBER(TSN_TK_DEL_MTX)]	= F(svc_tk_del_mtx),
	[TSN_NUMBER(TSN_TK_LOC_MTX)]	= F(svc_tk_loc_mtx),
	[TSN_NUMBER(TSN_TK_UNL_MTX)]	= F(svc_tk_unl_mtx),
	[TSN_NUMBER(TSN_TK_REF_MTX)]	= F(svc_tk_ref_mtx),
	[TSN_NUMBER(TSN_TK_CRE_MBF)]	= F(svc_tk_cre_mbf),
	[TSN_NUMBER(TSN_TK_DEL_MBF)]	= F(svc_tk_del_mbf),
	[TSN_NUMBER(TSN_TK_SND_MBF)]	= F(svc_tk_snd_mbf),
	[TSN_NUMBER(TSN_TK_RCV_MBF)]	= F(svc_tk_rcv_mbf),
	[TSN_NUMBER(TSN_TK_REF_MBF)]	= F(svc_tk_ref_mbf),
	[TSN_NUMBER(TSN_TK_DLY_TSK)]	= F(tk_dly_tsk),
	[TSN_NUMBER(TSN_TK_GET_TIM)]	= F(svc_tk_get_tim),
	[TSN_NUMBER(TSN_TK_SET_TIM)]	= F(svc_tk_set_tim),
	[TSN_NUMBER(TSN_TK_GET_OTM)]	= F(svc_tk_get_otm),
	[TSN_NUMBER(TSN_TK_CRE_CYC)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_DEL_CYC)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_STA_CYC)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_STP_CYC)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_REF_CYC)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_CRE_ALM)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_DEL_ALM)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_STA_ALM)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_STP_ALM)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_REF_ALM)]	= F(svc_tk_nospt),
#if USE_TIME_US_API
	[TSN_NUMBER(TSN_TK_SLP_TSK_U)]	= F(tk_slp_tsk_u),
	[TSN_NUMBER(TSN_TK_DLY_TSK_U)]	= F(tk_dly_tsk_u),
	[TSN_NUMBER(TSN_TK_WAI_SEM_U)]	= F(svc_tk_wai_sem_u),
	[TSN_NUMBER(TSN_TK_WAI_FLG_U)]	= F(svc_tk_wai_flg_u),
	[TSN_NUMBER(TSN_TK_RCV_MBX_U)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_LOC_MTX_U)]	= F(svc_tk_loc_mtx_u),
	[TSN_NUMBER(TSN_TK_SND_MBF_U)]	= F(svc_tk_snd_mbf_u),
	[TSN_NUMBER(TSN_TK_RCV_MBF_U)]	= F(svc_tk_rcv_mbf_u),
	[TSN_NUMBER(TSN_TK_CRE_CYC_U)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_REF_CYC_U)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_STA_ALM_U)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_REF_ALM_U)]	= F(svc_tk_nospt),
	[TSN_NUMBER(TSN_TK_SET_TIM_U)]	= F(svc_tk_set_tim_u),
	[TSN_NUMBER(TSN_TK_GET_TIM_U)]	= F(svc_tk_get_tim_u),
	[TSN_NUMBER(TSN_TK_GET_OTM_U)]	= F(svc_tk_get_otm_u),
#endif
	[TSN_NUMBER(TSN_TK_SET_UTC)]	= F(svc_tk_set_utc),
	[TSN_NUMBER(TSN_TK_GET_UTC)]	= F(svc_tk_get_utc),
	[TSN_NUMBER(TSN_TK_GET_PRC)]	= F(tk_get_prc),
};
