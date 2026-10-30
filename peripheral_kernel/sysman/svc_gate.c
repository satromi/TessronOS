/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svc_gate.c
 *	System call gateway (design 9.7): dispatches 'svc #0' from EL0 tasks
 *	to the kernel functions. Runs on the task's system stack as a quasi
 *	task part with interrupts enabled, so the callee may block.
 *
 *	Every call of a process that takes a pointer -- of every class: tk_,
 *	ts_, dt_, tm_, ob_, fs_, so_, gp_, the drawing and window calls --
 *	goes through a wrapper of its class that first makes sure the
 *	memory lies in the calling process's own pages, writable when
 *	something is to come back in it (knl_prc_user_ok), and answers
 *	E_MACV (EX_FAULT for fs_) otherwise. What the kernel acts on is
 *	copied into its own memory first; a buffer that is only carried
 *	is used where it is, once checked. The calls of the core from a
 *	process are served by svc_tk.c, the objects by obsvc.c, the
 *	sockets by so_prc.c and the GPU by gpu_svc.c.
 *
 *	The kernel reaches a process's pages only with the way to them
 *	open (PAN, kernel/sysdepend/cpu/core/armv8a/sysdepend.h): the
 *	wrappers open it with UACCESS, knl_ua_get and knl_ua_put for what
 *	they have made sure of, and nowhere else. A task of the system
 *	process at EL0 is the system's own; its calls run with the way
 *	open, and what it hands is taken as it is.
 *
 *	A call that acts on the whole system needs the right the table of
 *	rights gives it (svc_rights, include/ts/svc.h TSN_RIGHT_*), looked
 *	at here before the call is made.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "sysdepend/cpu_task.h"
#include <tm/tmonitor.h>
#include <ts/svc.h>
#include <ts/time.h>
#include <ts/proc.h>
#include <ts/ob.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include "obj/obj.h"
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/fn.h>
#include <ts/mn.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/img.h>
#include <ts/kconv.h>
#include <ts/hid.h>
#include <ts/conf.h>
#include <ts/umem.h>
#include <ts/tray.h>
#include <ts/keymap.h>
#include <ts/sysdef.h>
#include <ts/psx.h>

typedef UBINT (*SVCFN)( UBINT, UBINT, UBINT, UBINT, UBINT, UBINT, UBINT, UBINT );

#ifdef USE_UITEST
EXPORT volatile UD	knl_svc_calls = 0;	/* calls of processes made, for the UI tests (svcstat) */
#endif

#define F(fn)	((FP)(fn))

LOCAL ID svc_sys_cre_tsk( CONST T_CTSK *pk );

LOCAL const FP tk_svc_tbl[TSN_TK_MAX + 1] = {
	[TSN_NUMBER(TSN_TK_CRE_TSK)]	= F(svc_sys_cre_tsk),
	[TSN_NUMBER(TSN_TK_DEL_TSK)]	= F(tk_del_tsk),
	[TSN_NUMBER(TSN_TK_STA_TSK)]	= F(tk_sta_tsk),
	[TSN_NUMBER(TSN_TK_EXT_TSK)]	= F(tk_ext_tsk),
	[TSN_NUMBER(TSN_TK_EXD_TSK)]	= F(tk_exd_tsk),
	[TSN_NUMBER(TSN_TK_TER_TSK)]	= F(tk_ter_tsk),
	[TSN_NUMBER(TSN_TK_DIS_DSP)]	= F(tk_dis_dsp),
	[TSN_NUMBER(TSN_TK_ENA_DSP)]	= F(tk_ena_dsp),
	[TSN_NUMBER(TSN_TK_CHG_PRI)]	= F(tk_chg_pri),
	[TSN_NUMBER(TSN_TK_ROT_RDQ)]	= F(tk_rot_rdq),
	[TSN_NUMBER(TSN_TK_REL_WAI)]	= F(tk_rel_wai),
	[TSN_NUMBER(TSN_TK_GET_TID)]	= F(tk_get_tid),
	[TSN_NUMBER(TSN_TK_REF_TSK)]	= F(tk_ref_tsk),
	[TSN_NUMBER(TSN_TK_SUS_TSK)]	= F(tk_sus_tsk),
	[TSN_NUMBER(TSN_TK_RSM_TSK)]	= F(tk_rsm_tsk),
	[TSN_NUMBER(TSN_TK_FRSM_TSK)]	= F(tk_frsm_tsk),
	[TSN_NUMBER(TSN_TK_SLP_TSK)]	= F(tk_slp_tsk),
	[TSN_NUMBER(TSN_TK_WUP_TSK)]	= F(tk_wup_tsk),
	[TSN_NUMBER(TSN_TK_CAN_WUP)]	= F(tk_can_wup),
	[TSN_NUMBER(TSN_TK_CRE_SEM)]	= F(tk_cre_sem),
	[TSN_NUMBER(TSN_TK_DEL_SEM)]	= F(tk_del_sem),
	[TSN_NUMBER(TSN_TK_SIG_SEM)]	= F(tk_sig_sem),
	[TSN_NUMBER(TSN_TK_WAI_SEM)]	= F(tk_wai_sem),
	[TSN_NUMBER(TSN_TK_REF_SEM)]	= F(tk_ref_sem),
	[TSN_NUMBER(TSN_TK_CRE_FLG)]	= F(tk_cre_flg),
	[TSN_NUMBER(TSN_TK_DEL_FLG)]	= F(tk_del_flg),
	[TSN_NUMBER(TSN_TK_SET_FLG)]	= F(tk_set_flg),
	[TSN_NUMBER(TSN_TK_CLR_FLG)]	= F(tk_clr_flg),
	[TSN_NUMBER(TSN_TK_WAI_FLG)]	= F(tk_wai_flg),
	[TSN_NUMBER(TSN_TK_REF_FLG)]	= F(tk_ref_flg),
	[TSN_NUMBER(TSN_TK_CRE_MBX)]	= F(tk_cre_mbx),
	[TSN_NUMBER(TSN_TK_DEL_MBX)]	= F(tk_del_mbx),
	[TSN_NUMBER(TSN_TK_SND_MBX)]	= F(tk_snd_mbx),
	[TSN_NUMBER(TSN_TK_RCV_MBX)]	= F(tk_rcv_mbx),
	[TSN_NUMBER(TSN_TK_REF_MBX)]	= F(tk_ref_mbx),
	[TSN_NUMBER(TSN_TK_CRE_MTX)]	= F(tk_cre_mtx),
	[TSN_NUMBER(TSN_TK_DEL_MTX)]	= F(tk_del_mtx),
	[TSN_NUMBER(TSN_TK_LOC_MTX)]	= F(tk_loc_mtx),
	[TSN_NUMBER(TSN_TK_UNL_MTX)]	= F(tk_unl_mtx),
	[TSN_NUMBER(TSN_TK_REF_MTX)]	= F(tk_ref_mtx),
	[TSN_NUMBER(TSN_TK_CRE_MBF)]	= F(tk_cre_mbf),
	[TSN_NUMBER(TSN_TK_DEL_MBF)]	= F(tk_del_mbf),
	[TSN_NUMBER(TSN_TK_SND_MBF)]	= F(tk_snd_mbf),
	[TSN_NUMBER(TSN_TK_RCV_MBF)]	= F(tk_rcv_mbf),
	[TSN_NUMBER(TSN_TK_REF_MBF)]	= F(tk_ref_mbf),
	[TSN_NUMBER(TSN_TK_DLY_TSK)]	= F(tk_dly_tsk),
	[TSN_NUMBER(TSN_TK_GET_TIM)]	= F(tk_get_tim),
	[TSN_NUMBER(TSN_TK_SET_TIM)]	= F(tk_set_tim),
	[TSN_NUMBER(TSN_TK_GET_OTM)]	= F(tk_get_otm),
	[TSN_NUMBER(TSN_TK_CRE_CYC)]	= F(tk_cre_cyc),
	[TSN_NUMBER(TSN_TK_DEL_CYC)]	= F(tk_del_cyc),
	[TSN_NUMBER(TSN_TK_STA_CYC)]	= F(tk_sta_cyc),
	[TSN_NUMBER(TSN_TK_STP_CYC)]	= F(tk_stp_cyc),
	[TSN_NUMBER(TSN_TK_REF_CYC)]	= F(tk_ref_cyc),
	[TSN_NUMBER(TSN_TK_CRE_ALM)]	= F(tk_cre_alm),
	[TSN_NUMBER(TSN_TK_DEL_ALM)]	= F(tk_del_alm),
	[TSN_NUMBER(TSN_TK_STA_ALM)]	= F(tk_sta_alm),
	[TSN_NUMBER(TSN_TK_STP_ALM)]	= F(tk_stp_alm),
	[TSN_NUMBER(TSN_TK_REF_ALM)]	= F(tk_ref_alm),
#if USE_TIME_US_API
	[TSN_NUMBER(TSN_TK_SLP_TSK_U)]	= F(tk_slp_tsk_u),
	[TSN_NUMBER(TSN_TK_DLY_TSK_U)]	= F(tk_dly_tsk_u),
	[TSN_NUMBER(TSN_TK_WAI_SEM_U)]	= F(tk_wai_sem_u),
	[TSN_NUMBER(TSN_TK_WAI_FLG_U)]	= F(tk_wai_flg_u),
	[TSN_NUMBER(TSN_TK_RCV_MBX_U)]	= F(tk_rcv_mbx_u),
	[TSN_NUMBER(TSN_TK_LOC_MTX_U)]	= F(tk_loc_mtx_u),
	[TSN_NUMBER(TSN_TK_SND_MBF_U)]	= F(tk_snd_mbf_u),
	[TSN_NUMBER(TSN_TK_RCV_MBF_U)]	= F(tk_rcv_mbf_u),
	[TSN_NUMBER(TSN_TK_CRE_CYC_U)]	= F(tk_cre_cyc_u),
	[TSN_NUMBER(TSN_TK_REF_CYC_U)]	= F(tk_ref_cyc_u),
	[TSN_NUMBER(TSN_TK_STA_ALM_U)]	= F(tk_sta_alm_u),
	[TSN_NUMBER(TSN_TK_REF_ALM_U)]	= F(tk_ref_alm_u),
	[TSN_NUMBER(TSN_TK_SET_TIM_U)]	= F(tk_set_tim_u),
	[TSN_NUMBER(TSN_TK_GET_TIM_U)]	= F(tk_get_tim_u),
	[TSN_NUMBER(TSN_TK_GET_OTM_U)]	= F(tk_get_otm_u),
#endif
	[TSN_NUMBER(TSN_TK_SET_UTC)]	= F(tk_set_utc),
	[TSN_NUMBER(TSN_TK_GET_UTC)]	= F(tk_get_utc),
	[TSN_NUMBER(TSN_TK_GET_PRC)]	= F(tk_get_prc),
	[TSN_NUMBER(TSN_TK_CHG_SLT)]	= F(tk_chg_slt),
	[TSN_NUMBER(TSN_TK_SET_ASP)]	= F(tk_set_asp),
	[TSN_NUMBER(TSN_TK_REF_ASP)]	= F(tk_ref_asp),
};

/* Whether the calling process may use its memory at p..p+len for this */
LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

LOCAL SZ svc_ts_get_arg( void *buf, SZ size )
{
	if ( size < 0 || ( size > 0 && !knl_prc_user_ok(ts_get_pid(), buf, size, TRUE) ) ) {
		return E_MACV;
	}
	return UACCESS(ts_get_arg(buf, size));
}

LOCAL ER svc_ts_gen_uuid( TS_UUID *p_uuid )
{
	TS_UUID	u;
	ER	er;

	if ( !user_ok(p_uuid, sizeof(*p_uuid), TRUE) ) {
		return E_MACV;
	}
	er = ts_gen_uuid(&u);
	if ( er >= E_OK ) {
		knl_ua_put(p_uuid, &u, sizeof(u));
	}
	return er;
}

/*
 * Random bytes, as programs built before the random source was an
 * object asked for them: now a read of that object (乱数), as the
 * caller, through its protection. The libraries read the object
 * themselves and no longer call this.
 */
LOCAL ER svc_ts_get_random( void *buf, SZ len )
{
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( len <= 0 ) {
		return ( len == 0 ) ? E_OK : E_PAR;
	}
	if ( !user_ok(buf, len, TRUE) ) {
		return E_MACV;
	}
	key = ob_opn_obj(&ob_uuid_random, OB_OP_READ);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = UACCESS(ob_rea_rec(key, OB_RND_DATA, 0, buf, len, &asz));
	(void)ob_cls_obj(key);
	return ( er < E_OK ) ? er : ( asz == len ) ? E_OK : E_IO;
}

/*
 * What a process does to another goes through the other's process
 * object (design 18.8, knl_obprc_may): ending it takes the right to
 * delete it, sending it a message the right to write it, looking at it
 * the right to read it. A parent may end, look at and send to its
 * children, and a child send to and look at its parent, whoever they
 * act for. Collecting a process is its parent's alone: another would
 * take its exit code from it. The system process's tasks (pid 0) are
 * not asked. A packet is made sure of and passes through the kernel.
 */
LOCAL ER svc_ts_ter_prc( ID pid, INT abortcd )
{
	ER	er;

	if ( ts_get_pid() > 0 && ( er = knl_obprc_may(pid, OB_OP_DELETE, OBP_PARENT) ) < E_OK ) {
		return er;
	}
	return ts_ter_prc(pid, abortcd);
}

LOCAL ER svc_ts_wai_prc( ID pid, T_PSTS *pk_psts, TMO tmout )
{
	T_PSTS	s;
	T_RPRC	r;
	ID	me = ts_get_pid();
	ER	er;

	if ( pk_psts != NULL && !user_ok(pk_psts, sizeof(*pk_psts), TRUE) ) {
		return E_MACV;
	}
	if ( me > 0 && pid != TS_PRC_ANY ) {
		if ( ts_ref_prc(pid, &r) < E_OK ) {
			return E_NOEXS;
		}
		if ( r.ppid != me ) {
			return E_OACV;
		}
	}
	er = ts_wai_prc(pid, &s, tmout);
	if ( er >= E_OK && pk_psts != NULL ) {
		knl_ua_put(pk_psts, &s, sizeof(s));
	}
	return er;
}

LOCAL ER svc_ts_ref_prc( ID pid, T_RPRC *pk_rprc )
{
	T_RPRC	r;
	ER	er;

	if ( !user_ok(pk_rprc, sizeof(*pk_rprc), TRUE) ) {
		return E_MACV;
	}
	if ( pid != 0 && ts_get_pid() > 0
	  && ( er = knl_obprc_may(pid, OB_OP_READ, OBP_PARENT | OBP_CHILD) ) < E_OK ) {
		return er;
	}
	er = ts_ref_prc(pid, &r);
	if ( er >= E_OK ) {
		knl_ua_put(pk_rprc, &r, sizeof(r));
	}
	return er;
}

/* The message is held on the task's own stack: a task ended while it waits leaves nothing behind */
LOCAL ER svc_ts_snd_msg( ID pid, CONST T_TSMSG *msg, TMO tmout )
{
	T_TSMSG	m;
	ER	er;

	if ( !user_ok(msg, sizeof(*msg), FALSE) ) {
		return E_MACV;
	}
	if ( ts_get_pid() > 0 && ( er = knl_obprc_may(pid, OB_OP_WRITE, OBP_PARENT | OBP_CHILD) ) < E_OK ) {
		return er;
	}
	knl_ua_get(&m, msg, sizeof(m));
	return ts_snd_msg(pid, &m, tmout);
}

LOCAL ER svc_ts_rcv_msg( T_TSMSG *buf, TMO tmout )
{
	T_TSMSG	m;
	ER	er;

	if ( !user_ok(buf, sizeof(*buf), TRUE) ) {
		return E_MACV;
	}
	er = ts_rcv_msg(&m, tmout);
	if ( er >= E_OK ) {
		knl_ua_put(buf, &m, sizeof(m));
	}
	return er;
}

/* As many processors as the caller's size holds, and no more than there are */
IMPORT INT	knl_num_prc;

LOCAL INT svc_ts_ref_cpu( T_TSCPU *pk, SZ size )
{
	T_TSCPU	*c;
	SZ	want = size;
	INT	er;

	if ( size < (SZ)sizeof(T_TSCPU) ) {
		return E_PAR;
	}
	if ( !user_ok(pk, size, TRUE) ) {
		return E_MACV;
	}
	if ( want > TS_TSCPU_SIZE(knl_num_prc) ) {
		want = TS_TSCPU_SIZE(knl_num_prc);
	}
	c = (T_TSCPU *)Kmalloc((size_t)want);
	if ( c == NULL ) {
		return E_NOMEM;
	}
	er = ts_ref_cpu(c, want);
	if ( er >= E_OK ) {
		knl_ua_put(pk, c, want);
	}
	Kfree(c);
	return er;
}

LOCAL ER svc_ts_get_mono( UD *p_ns )
{
	UD	ns = 0;
	ER	er;

	if ( !user_ok(p_ns, sizeof(*p_ns), TRUE) ) {
		return E_MACV;
	}
	er = ts_get_mono(&ns);
	if ( er >= E_OK ) {
		knl_ua_put(p_ns, &ns, sizeof(ns));
	}
	return er;
}

/* The address wanted comes in and the one given goes out through *p_adr */
LOCAL ER svc_ts_map_mem( void **p_adr, SZ size, SZ align, UINT prot )
{
	void	*a;
	ER	er;

	if ( !user_ok(p_adr, sizeof(void *), TRUE) ) {
		return E_MACV;
	}
	knl_ua_get(&a, p_adr, sizeof(a));
	er = ts_map_mem(&a, size, align, prot);
	if ( er == E_OK ) {
		knl_ua_put(p_adr, &a, sizeof(a));
	}
	return er;
}

/* ---------------------------------------------------------------- POSIX processes (design 5.8) */

LOCAL ER user_cstr( CONST char *src, char *dst, INT max );

/*
 * What a spawn is given -- the T_SPAWN, its start-up argument and its
 * descriptors -- copied into the kernel's memory before anything is
 * looked at; spn_free lets the copies go.
 */
LOCAL ER spn_copy( CONST T_SPAWN *usp, T_SPAWN *sp )
{
	void	*arg = NULL;
	INT	*fds = NULL;

	if ( !user_ok(usp, sizeof(*sp), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(sp, usp, sizeof(*sp));
	if ( sp->argsz < 0 || sp->argsz > TS_SPN_ARG_MAX || sp->nfd < 0 || sp->nfd > TS_SPN_FD_MAX ) {
		return E_PAR;
	}
	if ( sp->argsz > 0 ) {
		if ( sp->arg == NULL || !user_ok(sp->arg, sp->argsz, FALSE) ) {
			return E_MACV;
		}
		arg = Kmalloc((size_t)sp->argsz);
		if ( arg == NULL ) {
			return E_NOMEM;
		}
		knl_ua_get(arg, sp->arg, (SZ)sp->argsz);
	}
	if ( sp->nfd > 0 ) {
		if ( sp->fds == NULL || !user_ok(sp->fds, (SZ)sp->nfd * (SZ)sizeof(INT), FALSE) ) {
			if ( arg != NULL ) Kfree(arg);
			return E_MACV;
		}
		fds = (INT *)Kmalloc(sizeof(INT) * (SZ)sp->nfd);
		if ( fds == NULL ) {
			if ( arg != NULL ) Kfree(arg);
			return E_NOMEM;
		}
		knl_ua_get(fds, sp->fds, (SZ)sp->nfd * (SZ)sizeof(INT));
	}
	sp->arg = arg;
	sp->fds = fds;
	return E_OK;
}

LOCAL void spn_free( T_SPAWN *sp )
{
	if ( sp->arg != NULL ) Kfree((void *)sp->arg);
	if ( sp->fds != NULL ) Kfree((void *)sp->fds);
}

/* A spawn by a path: the path copied too, and asked whether the caller may name it */
LOCAL ID svc_ts_spn_prc( CONST char *path, CONST T_SPAWN *usp )
{
	T_SPAWN	sp;
	char	*p;
	ID	pid;
	ER	er;

	p = (char *)Kmalloc(FS_PATH_MAX);
	if ( p == NULL ) {
		return E_NOMEM;
	}
	er = user_cstr(path, p, FS_PATH_MAX);
	if ( er >= EX_OK && !knl_fs_path_let(p, knl_ob_crd_of(ts_get_pid())) ) {
		er = EX_ACCES;
	}
	if ( er < EX_OK ) {
		Kfree(p);
		return ( er == EX_FAULT ) ? E_MACV : E_PAR;
	}
	er = spn_copy(usp, &sp);
	if ( er < E_OK ) {
		Kfree(p);
		return er;
	}
	pid = ts_spn_prc(p, &sp);
	Kfree(p);
	spn_free(&sp);
	return pid;
}

/* The object a path of the object world names */
LOCAL ER svc_ts_lkp_obj( CONST char *path, TS_UUID *p_uuid )
{
	TS_UUID	u;
	char	*p;
	ER	er;

	if ( !user_ok(p_uuid, sizeof(u), TRUE) ) {
		return E_MACV;
	}
	p = (char *)Kmalloc(FS_PATH_MAX);
	if ( p == NULL ) {
		return E_NOMEM;
	}
	er = user_cstr(path, p, FS_PATH_MAX);
	if ( er >= EX_OK && !knl_fs_path_let(p, knl_ob_crd_of(ts_get_pid())) ) {
		er = EX_ACCES;
	}
	if ( er >= EX_OK ) {
		er = knl_objfs_lookup(p, &u);
	}
	Kfree(p);
	if ( er < EX_OK ) {
		return ( er == EX_FAULT ) ? E_MACV : ( er == EX_ACCES ) ? E_OACV
		     : ( er == EX_NOENT || er == EX_NOTDIR ) ? E_NOEXS : E_PAR;
	}
	knl_ua_put(p_uuid, &u, sizeof(u));
	return E_OK;
}

/* A spawn of the program object a link points at */
LOCAL ID svc_ts_spn_obj( CONST TS_UUID *uprog, CONST T_SPAWN *usp )
{
	T_SPAWN	sp;
	TS_UUID	prog;
	ID	pid;
	ER	er;

	if ( !user_ok(uprog, sizeof(prog), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(&prog, uprog, sizeof(prog));
	er = spn_copy(usp, &sp);
	if ( er < E_OK ) {
		return er;
	}
	pid = ts_spn_obj(&prog, &sp);
	spn_free(&sp);
	return pid;
}

LOCAL INT svc_ts_get_inh( INT *fds, INT max )
{
	INT	*k, n;

	if ( max < 0 || max > TS_SPN_FD_MAX ) {
		return E_PAR;
	}
	if ( max == 0 ) {
		return ts_get_inh(NULL, 0);
	}
	if ( !user_ok(fds, (SZ)max * (SZ)sizeof(INT), TRUE) ) {
		return E_MACV;
	}
	k = (INT *)Kmalloc(sizeof(INT) * (SZ)max);
	if ( k == NULL ) {
		return E_NOMEM;
	}
	n = ts_get_inh(k, max);
	if ( n > 0 ) {
		knl_ua_put(fds, k, (SZ)( ( n < max ) ? n : max ) * (SZ)sizeof(INT));
	}
	Kfree(k);
	return n;
}

LOCAL ID svc_ts_wai_psx( ID pid, UINT options, INT *p_status )
{
	INT	st = 0;
	ID	r;

	if ( p_status != NULL && !user_ok(p_status, sizeof(INT), TRUE) ) {
		return E_MACV;
	}
	r = ts_wai_psx(pid, options, &st);
	if ( r > 0 && p_status != NULL ) {
		knl_ua_put(p_status, &st, sizeof(st));
	}
	return r;
}

LOCAL ER svc_ts_sig_set( UINT what, CONST T_TSSIG *in, T_TSSIG *old )
{
	T_TSSIG	i, o;
	ER	er;

	if ( ( in != NULL && !user_ok(in, sizeof(i), FALSE) )
	  || ( old != NULL && !user_ok(old, sizeof(o), TRUE) ) ) {
		return E_MACV;
	}
	if ( in != NULL ) knl_ua_get(&i, in, sizeof(i));
	er = ts_sig_set(what, ( in != NULL ) ? &i : NULL, ( old != NULL ) ? &o : NULL);
	if ( er >= E_OK && old != NULL ) {
		knl_ua_put(old, &o, sizeof(o));
	}
	return er;
}

LOCAL ER svc_ts_slp_psx( UD ns, UD *p_left )
{
	UD	left = 0;
	ER	er;

	if ( p_left != NULL && !user_ok(p_left, sizeof(UD), TRUE) ) {
		return E_MACV;
	}
	er = ts_slp_psx(ns, &left);
	if ( p_left != NULL ) {
		knl_ua_put(p_left, &left, sizeof(left));
	}
	return er;
}

LOCAL ER svc_ts_ref_psx( ID pid, T_PSXREF *pk )
{
	T_PSXREF r;
	ER	er;

	if ( !user_ok(pk, sizeof(r), TRUE) ) {
		return E_MACV;
	}
	er = ts_ref_psx(pid, &r);
	if ( er >= E_OK ) {
		knl_ua_put(pk, &r, sizeof(r));
	}
	return er;
}

#define LST_PRC_MAX	4096

LOCAL INT svc_ts_lst_prc( ID *pids, INT max )
{
	ID	*k;
	INT	n;

	if ( max <= 0 ) return 0;
	if ( max > LST_PRC_MAX ) max = LST_PRC_MAX;
	if ( !user_ok(pids, sizeof(ID) * (SZ)max, TRUE) ) {
		return E_MACV;
	}
	k = (ID *)Kmalloc(sizeof(ID) * (SZ)max);
	if ( k == NULL ) return E_NOMEM;
	n = ts_lst_prc(k, max);
	if ( n > 0 ) {
		knl_ua_put(pids, k, sizeof(ID) * (SZ)n);
	}
	Kfree(k);
	return n;
}

LOCAL const FP ts_svc_tbl[TSN_TS_MAX + 1] = {
	[TSN_NUMBER(TSN_TS_SPN_PRC)]	= F(svc_ts_spn_prc),
	[TSN_NUMBER(TSN_TS_GET_INH)]	= F(svc_ts_get_inh),
	[TSN_NUMBER(TSN_TS_WAI_PSX)]	= F(svc_ts_wai_psx),
	[TSN_NUMBER(TSN_TS_KIL_PRC)]	= F(ts_kil_prc),
	[TSN_NUMBER(TSN_TS_SIG_SET)]	= F(svc_ts_sig_set),
	[TSN_NUMBER(TSN_TS_SIG_RET)]	= F(ts_sig_ret),	/* taken by the gateway itself */
	[TSN_NUMBER(TSN_TS_SIG_SUS)]	= F(ts_sig_sus),
	[TSN_NUMBER(TSN_TS_SLP_PSX)]	= F(svc_ts_slp_psx),
	[TSN_NUMBER(TSN_TS_SIG_ALM)]	= F(ts_sig_alm),
	[TSN_NUMBER(TSN_TS_SET_PGID)]	= F(ts_set_pgid),
	[TSN_NUMBER(TSN_TS_GET_PGID)]	= F(ts_get_pgid),
	[TSN_NUMBER(TSN_TS_SET_SID)]	= F(ts_set_sid),
	[TSN_NUMBER(TSN_TS_GET_SID)]	= F(ts_get_sid),
	[TSN_NUMBER(TSN_TS_GET_PPID)]	= F(ts_get_ppid),
	[TSN_NUMBER(TSN_TS_REF_PSX)]	= F(svc_ts_ref_psx),
	[TSN_NUMBER(TSN_TS_LST_PRC)]	= F(svc_ts_lst_prc),
	[TSN_NUMBER(TSN_TS_SPN_OBJ)]	= F(svc_ts_spn_obj),
	[TSN_NUMBER(TSN_TS_LKP_OBJ)]	= F(svc_ts_lkp_obj),
	[TSN_NUMBER(TSN_TS_GET_MONO)]	= F(svc_ts_get_mono),
	[TSN_NUMBER(TSN_TS_REF_CPU)]	= F(svc_ts_ref_cpu),
	[TSN_NUMBER(TSN_TS_DEF_FLT)]	= F(ts_def_flt),
	[TSN_NUMBER(TSN_TS_FLT_RET)]	= F(ts_flt_ret),	/* taken by the gateway itself */
	[TSN_NUMBER(TSN_TS_EXT_PRC)]	= F(ts_ext_prc),
	[TSN_NUMBER(TSN_TS_TER_PRC)]	= F(svc_ts_ter_prc),
	[TSN_NUMBER(TSN_TS_WAI_PRC)]	= F(svc_ts_wai_prc),
	[TSN_NUMBER(TSN_TS_REF_PRC)]	= F(svc_ts_ref_prc),
	[TSN_NUMBER(TSN_TS_GET_PID)]	= F(ts_get_pid),
	[TSN_NUMBER(TSN_TS_GEN_UUID)]	= F(svc_ts_gen_uuid),
	[TSN_NUMBER(TSN_TS_GET_RANDOM)]	= F(svc_ts_get_random),
	[TSN_NUMBER(TSN_TS_SND_MSG)]	= F(svc_ts_snd_msg),
	[TSN_NUMBER(TSN_TS_RCV_MSG)]	= F(svc_ts_rcv_msg),
	[TSN_NUMBER(TSN_TS_GET_ARG)]	= F(svc_ts_get_arg),
	[TSN_NUMBER(TSN_TS_MAP_MEM)]	= F(svc_ts_map_mem),
	[TSN_NUMBER(TSN_TS_UNM_MEM)]	= F(ts_unm_mem),
	[TSN_NUMBER(TSN_TS_CTL_MEM)]	= F(ts_ctl_mem),
};

/*
 * dt_ from a process: what it hands by pointer is made sure of and
 * copied through the kernel's own, as for the other classes. The
 * system process's tasks (pid 0) call the calendar as it is.
 */
LOCAL ER svc_dt_gettime( TS_TIME *p_t )
{
	TS_TIME	t = 0;
	ER	er;

	if ( !user_ok(p_t, sizeof(*p_t), TRUE) ) {
		return E_MACV;
	}
	er = dt_gettime(&t);
	if ( er >= E_OK ) {
		knl_ua_put(p_t, &t, sizeof(t));
	}
	return er;
}

/* A time turned into its fields, in UTC or in the system's zone */
LOCAL ER dt_fields( CONST TS_TIME *t, TS_TM *tm, BOOL local )
{
	TS_TIME	v;
	TS_TM	f;
	ER	er;

	if ( !user_ok(t, sizeof(*t), FALSE) || !user_ok(tm, sizeof(*tm), TRUE) ) {
		return E_MACV;
	}
	knl_ua_get(&v, t, sizeof(v));
	er = local ? dt_localtime(&v, &f) : dt_gmtime(&v, &f);
	if ( er >= E_OK ) {
		knl_ua_put(tm, &f, sizeof(f));
	}
	return er;
}

LOCAL ER svc_dt_gmtime( CONST TS_TIME *t, TS_TM *tm )
{
	return dt_fields(t, tm, FALSE);
}

LOCAL ER svc_dt_localtime( CONST TS_TIME *t, TS_TM *tm )
{
	return dt_fields(t, tm, TRUE);
}

LOCAL ER svc_dt_mktime( CONST TS_TM *tm, TS_TIME *p_t )
{
	TS_TM	f;
	TS_TIME	t = 0;
	ER	er;

	if ( !user_ok(tm, sizeof(*tm), FALSE) || !user_ok(p_t, sizeof(*p_t), TRUE) ) {
		return E_MACV;
	}
	knl_ua_get(&f, tm, sizeof(f));
	er = dt_mktime(&f, &t);
	if ( er >= E_OK ) {
		knl_ua_put(p_t, &t, sizeof(t));
	}
	return er;
}

LOCAL ER svc_dt_getsystz( INT *p_minutes )
{
	INT	z = 0;
	ER	er;

	if ( !user_ok(p_minutes, sizeof(*p_minutes), TRUE) ) {
		return E_MACV;
	}
	er = dt_getsystz(&z);
	if ( er >= E_OK ) {
		knl_ua_put(p_minutes, &z, sizeof(z));
	}
	return er;
}

LOCAL INT svc_dt_getsyszone( char *name, INT max )
{
	char	z[DT_ZONE_MAX];
	INT	n;

	if ( max <= 0 || !user_ok(name, (SZ)max, TRUE) ) {
		return E_MACV;
	}
	n = dt_getsyszone(z, sizeof(z));
	if ( n >= 0 ) {
		if ( n >= max ) return E_PAR;
		knl_ua_put(name, z, n + 1);
	}
	return n;
}

LOCAL ER svc_dt_setsyszone( CONST char *name, INT minutes )
{
	char	z[DT_ZONE_MAX];
	INT	i;

	for ( i = 0; i < DT_ZONE_MAX; i++ ) {
		if ( !user_ok(name + i, 1, FALSE) ) return E_MACV;
		knl_ua_get(&z[i], name + i, 1);
		if ( z[i] == '\0' ) return dt_setsyszone(z, minutes);
	}
	return E_PAR;
}

#ifdef TSN_DT_GETRTC
LOCAL ER svc_dt_getrtc( INT *p_mode )
{
	INT	m = 0;
	ER	er;

	if ( !user_ok(p_mode, sizeof(*p_mode), TRUE) ) {
		return E_MACV;
	}
	er = dt_getrtc(&m);
	if ( er >= E_OK ) {
		knl_ua_put(p_mode, &m, sizeof(m));
	}
	return er;
}
#endif

LOCAL const FP dt_prc_tbl[TSN_DT_MAX + 1] = {
	[TSN_NUMBER(TSN_DT_GETTIME)]	= F(svc_dt_gettime),
	[TSN_NUMBER(TSN_DT_SETTIME)]	= F(dt_settime),
	[TSN_NUMBER(TSN_DT_GMTIME)]	= F(svc_dt_gmtime),
	[TSN_NUMBER(TSN_DT_LOCALTIME)]	= F(svc_dt_localtime),
	[TSN_NUMBER(TSN_DT_MKTIME)]	= F(svc_dt_mktime),
	[TSN_NUMBER(TSN_DT_GETSYSTZ)]	= F(svc_dt_getsystz),
	[TSN_NUMBER(TSN_DT_SETSYSTZ)]	= F(dt_setsystz),
	[TSN_NUMBER(TSN_DT_GETSYSZONE)]	= F(svc_dt_getsyszone),
	[TSN_NUMBER(TSN_DT_SETSYSZONE)]	= F(svc_dt_setsyszone),
#ifdef TSN_DT_GETRTC
	[TSN_NUMBER(TSN_DT_GETRTC)]	= F(svc_dt_getrtc),
	[TSN_NUMBER(TSN_DT_SETRTC)]	= F(dt_setrtc),
#endif
};

LOCAL const FP dt_svc_tbl[TSN_DT_MAX + 1] = {
	[TSN_NUMBER(TSN_DT_GETTIME)]	= F(dt_gettime),
	[TSN_NUMBER(TSN_DT_SETTIME)]	= F(dt_settime),
	[TSN_NUMBER(TSN_DT_GMTIME)]	= F(dt_gmtime),
	[TSN_NUMBER(TSN_DT_LOCALTIME)]	= F(dt_localtime),
	[TSN_NUMBER(TSN_DT_MKTIME)]	= F(dt_mktime),
	[TSN_NUMBER(TSN_DT_GETSYSTZ)]	= F(dt_getsystz),
	[TSN_NUMBER(TSN_DT_SETSYSTZ)]	= F(dt_setsystz),
	[TSN_NUMBER(TSN_DT_GETSYSZONE)]	= F(dt_getsyszone),
	[TSN_NUMBER(TSN_DT_SETSYSZONE)]	= F(dt_setsyszone),
#ifdef TSN_DT_GETRTC
	[TSN_NUMBER(TSN_DT_GETRTC)]	= F(dt_getrtc),
	[TSN_NUMBER(TSN_DT_SETRTC)]	= F(dt_setrtc),
#endif
};

/* ob_ (class 7): peripheral_kernel/obj/obsvc.c, which makes sure of every pointer */
IMPORT const FP knl_ob_svc_tbl[TSN_OB_MAX + 1];

/*
 * fs_ from a process. A path is taken only in its plain form -- no "."
 * or ".." and no empty part -- so that what it names is what it says,
 * and the files an object store keeps its objects in are closed to all
 * but administrators: a process reaches objects through ob_, where
 * their protection is (design 18.9). The kernel's own use of fs_ does
 * not come this way.
 */
EXPORT BOOL knl_fs_path_let( CONST char *path, CONST T_OBCRD *crd )
{
	INT	i, part = 0;

	if ( path == NULL || path[0] != '/' ) {
		return ( path != NULL );	/* the file layer answers for these */
	}
	for ( i = 1; ; i++ ) {
		if ( path[i] == '/' || path[i] == 0 ) {
			CONST char *p = path + i - part;

			if ( part == 0 && path[i] == '/' ) return FALSE;		/* "//" */
			if ( part == 1 && p[0] == '.' ) return FALSE;
			if ( part == 2 && p[0] == '.' && p[1] == '.' ) return FALSE;
			if ( path[i] == 0 ) break;
			part = 0;
		} else {
			part++;
		}
	}
	return ( !knl_tsfs_guards(path) || knl_ob_is_admin(crd) );
}

/*
 * A string of the process's taken into dst (max bytes, the terminator
 * included): EX_FAULT when it is not the process's to read, and
 * EX_NAMETOOLONG when it does not end in time -- a path cut short
 * would name something else.
 */
LOCAL ER user_cstr( CONST char *src, char *dst, INT max )
{
	INT	i;

	if ( src == NULL ) {
		return EX_INVAL;
	}
	for ( i = 0; i < max; i++ ) {
		if ( ( i == 0 || ( ( (UBINT)( src + i ) ) & ( PAGE_SIZE - 1 ) ) == 0 )
		  && !user_ok(src + i, 1, FALSE) ) {
			return EX_FAULT;
		}
		dst[i] = UACCESS(src[i]);
		if ( dst[i] == '\0' ) {
			return EX_OK;
		}
	}
	return EX_NAMETOOLONG;
}

/* A path of the process's, taken in and let through knl_fs_path_let */
LOCAL ER user_path( CONST char *src, char *dst )
{
	ER	er = user_cstr(src, dst, FS_PATH_MAX);

	if ( er < EX_OK ) {
		return er;
	}
	return knl_fs_path_let(dst, knl_ob_crd_of(ts_get_pid())) ? EX_OK : EX_ACCES;
}

/* A descriptor the calling process opened itself */
LOCAL BOOL fd_mine( INT fd )
{
	ID	pid = ts_get_pid();

	return ( pid > 0 && knl_fs_mine(fd, pid) );
}

LOCAL INT svc_fs_open( CONST char *path, UINT oflags )
{
	char	p[FS_PATH_MAX];
	ID	pid = ts_get_pid();
	ER	er = user_path(path, p);

	if ( er < EX_OK ) return er;
	if ( pid <= 0 ) return EX_PERM;		/* not a process: nobody would own it */
	return knl_fs_open_as(p, oflags, pid);
}

LOCAL ER svc_fs_close( INT fd )
{
	return fd_mine(fd) ? fs_close(fd) : EX_BADF;
}

/*
 * A process's read or write goes to the file layer in pieces of up to
 * FS_PRC_PIECE. Between them it looks whether the process is to end
 * (ts_ter_prc while it is inside, knl_prc_call_enter), and if so stops
 * with what it has moved: a process ended in the middle of a large
 * transfer is out of the file layer after one piece, and the volume is
 * as a short read or write leaves it.
 */
#define FS_PRC_PIECE	( 256 * 1024 )

LOCAL INT fs_pieces( INT fd, UB *buf, SZ len, BOOL wr )
{
	SZ	done = 0, want;
	INT	n;

	do {
		want = ( len - done > FS_PRC_PIECE ) ? FS_PRC_PIECE : len - done;
		n = UACCESS(wr ? fs_write(fd, buf + done, want) : fs_read(fd, buf + done, want));
		if ( n <= 0 ) {
			return ( done > 0 ) ? (INT)done : n;
		}
		done += n;
	} while ( n == (INT)want && done < len && !knl_prc_ending() );

	return (INT)done;
}

LOCAL INT svc_fs_read( INT fd, void *buf, SZ len )
{
	if ( !fd_mine(fd) ) return EX_BADF;
	if ( len < 0 ) return EX_INVAL;
	if ( len > 0 && !user_ok(buf, len, TRUE) ) return EX_FAULT;
	return fs_pieces(fd, (UB *)buf, len, FALSE);
}

LOCAL INT svc_fs_write( INT fd, CONST void *buf, SZ len )
{
	if ( !fd_mine(fd) ) return EX_BADF;
	if ( len < 0 ) return EX_INVAL;
	if ( len > 0 && !user_ok(buf, len, FALSE) ) return EX_FAULT;
	return fs_pieces(fd, (UB *)buf, len, TRUE);
}

LOCAL D svc_fs_lseek( INT fd, D offset, INT whence )
{
	return fd_mine(fd) ? fs_lseek(fd, offset, whence) : EX_BADF;
}

LOCAL ER svc_fs_stat( CONST char *path, T_FSTAT *st )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	if ( er < EX_OK ) return er;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return UACCESS(fs_stat(p, st));
}

LOCAL ER svc_fs_fstat( INT fd, T_FSTAT *st )
{
	if ( !fd_mine(fd) ) return EX_BADF;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return UACCESS(fs_fstat(fd, st));
}

LOCAL ER svc_fs_ftruncate( INT fd, UD len )
{
	return fd_mine(fd) ? fs_ftruncate(fd, len) : EX_BADF;
}

LOCAL ER svc_fs_truncate( CONST char *path, UD len )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_truncate(p, len);
}

#define GETDENTS_MAX	1024

LOCAL INT svc_fs_getdents( INT fd, T_DIRENT *buf, INT nent )
{
	if ( !fd_mine(fd) ) return EX_BADF;
	if ( nent <= 0 || nent > GETDENTS_MAX ) return EX_INVAL;
	if ( !user_ok(buf, (SZ)nent * (SZ)sizeof(T_DIRENT), TRUE) ) return EX_FAULT;
	return UACCESS(fs_getdents(fd, buf, nent));
}

LOCAL ER svc_fs_mkdir( CONST char *path )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_mkdir(p);
}

LOCAL ER svc_fs_rmdir( CONST char *path )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_rmdir(p);
}

LOCAL ER svc_fs_unlink( CONST char *path )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_unlink(p);
}

LOCAL ER svc_fs_rename( CONST char *from, CONST char *to )
{
	char	f[FS_PATH_MAX], t[FS_PATH_MAX];
	ER	er = user_path(from, f);

	if ( er < EX_OK ) return er;
	er = user_path(to, t);
	return ( er < EX_OK ) ? er : fs_rename(f, t);
}

LOCAL ER svc_fs_statvfs( CONST char *path, T_FSSTAT *st )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	if ( er < EX_OK ) return er;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return UACCESS(fs_statvfs(p, st));
}

/*
 * Mounting through a device object's key (design 12.2.3): the key and
 * its rights say everything, so nothing more is looked at here than
 * the fimp's name, copied in. No name: the kind on the disk says.
 */
LOCAL ER svc_fs_attach_dev( ID devkey, CONST char *fimpnm, UINT flags )
{
	char	f[FS_FIMPNM_MAX];
	ER	er;

	if ( fimpnm == NULL ) return fs_attach_dev(devkey, NULL, flags);
	if ( ( er = user_cstr(fimpnm, f, sizeof(f)) ) < EX_OK ) return er;
	return fs_attach_dev(devkey, f, flags);
}

LOCAL ER svc_fs_utime( CONST char *path, UD mtime )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_utime(p, mtime);
}

LOCAL INT svc_fs_mounts( T_FSMNT *buf, INT max )
{
	if ( max < 0 || max > FS_MAX_MOUNT * 4 ) return EX_INVAL;
	if ( max > 0 && !user_ok(buf, (SZ)max * (SZ)sizeof(T_FSMNT), TRUE) ) return EX_FAULT;
	return UACCESS(fs_mounts(buf, max));
}

/* ---------------------------------------------------------------- the POSIX layer's (design 5.8) */

LOCAL INT svc_fs_dup( INT fd )
{
	return fd_mine(fd) ? knl_fs_dup_as(fd, ts_get_pid()) : EX_BADF;
}

LOCAL ER svc_fs_pipe( INT *ufd )
{
	INT	fd[2];
	ER	er;

	if ( !user_ok(ufd, sizeof(fd), TRUE) ) return EX_FAULT;
	er = knl_fs_pipe_as(fd, ts_get_pid());
	if ( er >= EX_OK ) {
		knl_ua_put(ufd, fd, sizeof(fd));
	}
	return er;
}

LOCAL ER svc_fs_xstat( CONST char *path, T_FSXSTAT *st )
{
	char		p[FS_PATH_MAX];
	T_FSXSTAT	x;
	ER		er = user_path(path, p);

	if ( er < EX_OK ) return er;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	er = fs_xstat(p, &x);
	if ( er >= EX_OK ) knl_ua_put(st, &x, sizeof(x));
	return er;
}

LOCAL ER svc_fs_fxstat( INT fd, T_FSXSTAT *st )
{
	T_FSXSTAT	x;
	ER		er;

	if ( !fd_mine(fd) ) return EX_BADF;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	er = fs_fxstat(fd, &x);
	if ( er >= EX_OK ) knl_ua_put(st, &x, sizeof(x));
	return er;
}

/* What a request takes in and gives out through its argument */
LOCAL INT ioc_size( UINT cmd, BOOL *p_in, BOOL *p_out )
{
	*p_in = *p_out = FALSE;
	switch ( cmd ) {
	  case FS_IOC_TCGETS:	*p_out = TRUE; return sizeof(T_FSTERMIOS);
	  case FS_IOC_TCSETS:
	  case FS_IOC_TCSETSW:
	  case FS_IOC_TCSETSF:	*p_in = TRUE; return sizeof(T_FSTERMIOS);
	  case FS_IOC_GWINSZ:	*p_out = TRUE; return sizeof(T_FSWINSZ);
	  case FS_IOC_SWINSZ:	*p_in = TRUE; return sizeof(T_FSWINSZ);
	  case FS_IOC_GPGRP:
	  case FS_IOC_GPTN:
	  case FS_IOC_NREAD:
	  case FS_IOC_GSID:	*p_out = TRUE; return sizeof(INT);
	  case FS_IOC_SPGRP:
	  case FS_IOC_FLUSH:	*p_in = TRUE; return sizeof(INT);
	  case FS_IOC_SCTTY:
	  case FS_IOC_NOTTY:
	  case FS_IOC_DRAIN:
	  case FS_IOC_ISTTY:	return 0;
	  default:		return -1;
	}
}

LOCAL ER svc_fs_ioctl( INT fd, UINT cmd, void *arg )
{
	UB	k[sizeof(T_FSTERMIOS) + sizeof(T_FSWINSZ) + 16];
	BOOL	in, out;
	INT	n;
	ER	er;

	if ( !fd_mine(fd) ) return EX_BADF;
	n = ioc_size(cmd, &in, &out);
	if ( n < 0 ) return EX_NOTTY;
	if ( n > 0 && !user_ok(arg, n, out) ) return EX_FAULT;
	knl_memset(k, 0, sizeof(k));
	if ( in ) knl_ua_get(k, arg, n);
	er = fs_ioctl(fd, cmd, k);
	if ( er >= EX_OK && out ) knl_ua_put(arg, k, n);
	return er;
}

LOCAL INT svc_fs_fcntl( INT fd, INT cmd, INT arg )
{
	return fd_mine(fd) ? fs_fcntl(fd, cmd, arg) : EX_BADF;
}

#define POLL_MAX	256

LOCAL INT svc_fs_poll( T_FSPOLL *ufds, INT n, TMO tmout )
{
	T_FSPOLL	*k;
	BOOL		*bad;
	INT		i, r;
	ID		pid = ts_get_pid();

	if ( n < 0 || n > POLL_MAX ) return EX_INVAL;
	if ( n > 0 && !user_ok(ufds, (SZ)n * (SZ)sizeof(T_FSPOLL), TRUE) ) return EX_FAULT;
	k = (T_FSPOLL *)Kmalloc(sizeof(T_FSPOLL) * (SZ)( n + 1 ) + sizeof(BOOL) * (SZ)( n + 1 ));
	if ( k == NULL ) return EX_NOMEM;
	bad = (BOOL *)( k + n + 1 );
	if ( n > 0 ) knl_ua_get(k, ufds, (SZ)n * (SZ)sizeof(T_FSPOLL));
	for ( i = 0; i < n; i++ ) {
		bad[i] = ( k[i].fd >= 0 && !knl_fs_mine(k[i].fd, pid) );
		if ( bad[i] ) k[i].fd = -1;
	}
	r = fs_poll(k, n, tmout);
	if ( r >= 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( bad[i] ) {
				k[i].revents = FS_POLL_NVAL;
				r++;
			}
		}
		if ( n > 0 ) {
			for ( i = 0; i < n; i++ ) {
				knl_ua_put(&ufds[i].revents, &k[i].revents, sizeof(k[i].revents));
			}
		}
	}
	Kfree(k);
	return r;
}

LOCAL ER svc_fs_link( CONST char *from, CONST char *to )
{
	char	f[FS_PATH_MAX], t[FS_PATH_MAX];
	ER	er = user_path(from, f);

	if ( er < EX_OK ) return er;
	er = user_path(to, t);
	return ( er < EX_OK ) ? er : fs_link(f, t);
}

LOCAL ER svc_fs_chmod( CONST char *path, UINT mode )
{
	char	p[FS_PATH_MAX];
	ER	er = user_path(path, p);

	return ( er < EX_OK ) ? er : fs_chmod(p, mode);
}

/*
 * A disk reached from its device object's key (design 7.2): the key's
 * rights are all there is to it, so the paths below the disk's root are
 * not looked at as paths of the tree are (knl_fs_path_let); they are
 * copied in and the layer keeps them below the root.
 */
LOCAL INT svc_fs_opn_vol( ID devkey, CONST char *fimpnm, UINT flags )
{
	char	f[FS_FIMPNM_MAX];
	ID	pid = ts_get_pid();
	ER	er;

	if ( pid <= 0 ) return EX_PERM;
	if ( fimpnm == NULL ) return knl_fs_opn_vol_as(devkey, NULL, flags, pid);
	if ( ( er = user_cstr(fimpnm, f, sizeof(f)) ) < EX_OK ) return er;
	return knl_fs_opn_vol_as(devkey, f, flags, pid);
}

LOCAL INT svc_fs_openat( INT vd, CONST char *path, UINT oflags )
{
	char	p[FS_PATH_MAX];
	ER	er;

	if ( !fd_mine(vd) ) return EX_BADF;
	if ( ( er = user_cstr(path, p, FS_PATH_MAX) ) < EX_OK ) return er;
	return knl_fs_openat_as(vd, p, oflags, ts_get_pid());
}

LOCAL ER svc_fs_statat( INT vd, CONST char *path, T_FSTAT *st )
{
	char	p[FS_PATH_MAX];
	ER	er;

	if ( !fd_mine(vd) ) return EX_BADF;
	if ( ( er = user_cstr(path, p, FS_PATH_MAX) ) < EX_OK ) return er;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return UACCESS(fs_statat(vd, p, st));
}

/* mkdirat, rmdirat and unlinkat: one path, the change of the disk */
LOCAL ER at_path( INT vd, CONST char *path, ER (*fn)( INT vd, CONST char *path ) )
{
	char	p[FS_PATH_MAX];
	ER	er;

	if ( !fd_mine(vd) ) return EX_BADF;
	if ( ( er = user_cstr(path, p, FS_PATH_MAX) ) < EX_OK ) return er;
	return fn(vd, p);
}

LOCAL ER svc_fs_mkdirat( INT vd, CONST char *path ) { return at_path(vd, path, fs_mkdirat); }
LOCAL ER svc_fs_rmdirat( INT vd, CONST char *path ) { return at_path(vd, path, fs_rmdirat); }
LOCAL ER svc_fs_unlinkat( INT vd, CONST char *path ) { return at_path(vd, path, fs_unlinkat); }

LOCAL ER svc_fs_utimeat( INT vd, CONST char *path, UD mtime )
{
	char	p[FS_PATH_MAX];
	ER	er;

	if ( !fd_mine(vd) ) return EX_BADF;
	if ( ( er = user_cstr(path, p, FS_PATH_MAX) ) < EX_OK ) return er;
	return fs_utimeat(vd, p, mtime);
}

LOCAL ER svc_fs_renameat( INT vd, CONST char *from, CONST char *to )
{
	char	f[FS_PATH_MAX], t[FS_PATH_MAX];
	ER	er;

	if ( !fd_mine(vd) ) return EX_BADF;
	if ( ( er = user_cstr(from, f, FS_PATH_MAX) ) < EX_OK ) return er;
	if ( ( er = user_cstr(to, t, FS_PATH_MAX) ) < EX_OK ) return er;
	return fs_renameat(vd, f, t);
}

LOCAL ER svc_fs_fstatvfs( INT vd, T_FSSTAT *st )
{
	if ( !fd_mine(vd) ) return EX_BADF;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return UACCESS(fs_fstatvfs(vd, st));
}

LOCAL ER svc_fs_fsync( INT fd )
{
	return fd_mine(fd) ? fs_fsync(fd) : EX_BADF;
}

LOCAL const FP fs_svc_tbl[TSN_FS_MAX + 1] = {
	[TSN_NUMBER(TSN_FS_DUP)]	= F(svc_fs_dup),
	[TSN_NUMBER(TSN_FS_PIPE)]	= F(svc_fs_pipe),
	[TSN_NUMBER(TSN_FS_XSTAT)]	= F(svc_fs_xstat),
	[TSN_NUMBER(TSN_FS_FXSTAT)]	= F(svc_fs_fxstat),
	[TSN_NUMBER(TSN_FS_IOCTL)]	= F(svc_fs_ioctl),
	[TSN_NUMBER(TSN_FS_FCNTL)]	= F(svc_fs_fcntl),
	[TSN_NUMBER(TSN_FS_POLL)]	= F(svc_fs_poll),
	[TSN_NUMBER(TSN_FS_LINK)]	= F(svc_fs_link),
	[TSN_NUMBER(TSN_FS_CHMOD)]	= F(svc_fs_chmod),
	[TSN_NUMBER(TSN_FS_OPEN)]	= F(svc_fs_open),
	[TSN_NUMBER(TSN_FS_CLOSE)]	= F(svc_fs_close),
	[TSN_NUMBER(TSN_FS_READ)]	= F(svc_fs_read),
	[TSN_NUMBER(TSN_FS_WRITE)]	= F(svc_fs_write),
	[TSN_NUMBER(TSN_FS_LSEEK)]	= F(svc_fs_lseek),
	[TSN_NUMBER(TSN_FS_STAT)]	= F(svc_fs_stat),
	[TSN_NUMBER(TSN_FS_FSTAT)]	= F(svc_fs_fstat),
	[TSN_NUMBER(TSN_FS_FTRUNCATE)]	= F(svc_fs_ftruncate),
	[TSN_NUMBER(TSN_FS_TRUNCATE)]	= F(svc_fs_truncate),
	[TSN_NUMBER(TSN_FS_GETDENTS)]	= F(svc_fs_getdents),
	[TSN_NUMBER(TSN_FS_MKDIR)]	= F(svc_fs_mkdir),
	[TSN_NUMBER(TSN_FS_RMDIR)]	= F(svc_fs_rmdir),
	[TSN_NUMBER(TSN_FS_UNLINK)]	= F(svc_fs_unlink),
	[TSN_NUMBER(TSN_FS_RENAME)]	= F(svc_fs_rename),
	[TSN_NUMBER(TSN_FS_STATVFS)]	= F(svc_fs_statvfs),
	[TSN_NUMBER(TSN_FS_SYNC)]	= F(fs_sync),
	[TSN_NUMBER(TSN_FS_ATTACH_DEV)]	= F(svc_fs_attach_dev),
	[TSN_NUMBER(TSN_FS_DETACH_DEV)]	= F(fs_detach_dev),
	[TSN_NUMBER(TSN_FS_MOUNTS)]	= F(svc_fs_mounts),
	[TSN_NUMBER(TSN_FS_UTIME)]	= F(svc_fs_utime),
	[TSN_NUMBER(TSN_FS_OPN_VOL)]	= F(svc_fs_opn_vol),
	[TSN_NUMBER(TSN_FS_OPENAT)]	= F(svc_fs_openat),
	[TSN_NUMBER(TSN_FS_STATAT)]	= F(svc_fs_statat),
	[TSN_NUMBER(TSN_FS_MKDIRAT)]	= F(svc_fs_mkdirat),
	[TSN_NUMBER(TSN_FS_RMDIRAT)]	= F(svc_fs_rmdirat),
	[TSN_NUMBER(TSN_FS_UNLINKAT)]	= F(svc_fs_unlinkat),
	[TSN_NUMBER(TSN_FS_UTIMEAT)]	= F(svc_fs_utimeat),
	[TSN_NUMBER(TSN_FS_RENAMEAT)]	= F(svc_fs_renameat),
	[TSN_NUMBER(TSN_FS_FSTATVFS)]	= F(svc_fs_fstatvfs),
	[TSN_NUMBER(TSN_FS_FSYNC)]	= F(svc_fs_fsync),
};

/* so_ (class 5): peripheral_kernel/network/so_prc.c */
IMPORT const FP knl_so_svc_tbl[TSN_SO_MAX + 1];

/* ---------------------------------------------------------------- drawing, for a process */

/*
 * A drawing environment is handed to a process only through the key of
 * a window it may write (wm_obj_gid), and is its own from then on: what
 * it draws with is looked up here on every call, and a window that has
 * closed takes its environment's use with it.
 */
#define GRANT_MAX	32

typedef struct {
	INT	gid;
	ID	pid;
	INT	wid;
	TS_UUID	win;			/* the window object it was given for */
} GRANT;

LOCAL GRANT	grants[GRANT_MAX];

/*
 * Whether a grant still stands: the window object it was given for is
 * there, under the number and with the environment it had. A number
 * and an environment come back for the next window opened, perhaps a
 * window of another process; the object never does.
 */
LOCAL BOOL grant_live( CONST GRANT *g )
{
	return (BOOL)( g->pid != 0 && wm_obj_wid(&g->win) == g->wid && wm_gid(g->wid) == g->gid );
}

LOCAL BOOL granted( INT gid )
{
	ID	pid = ts_get_pid();
	INT	i;

	if ( pid <= 0 || gid < 0 ) {
		return FALSE;
	}
	for ( i = 0; i < GRANT_MAX; i++ ) {
		if ( grants[i].pid == pid && grants[i].gid == gid && grant_live(&grants[i]) ) {
			return TRUE;
		}
	}
	return FALSE;
}

/* A string of the process's, copied in with its terminator; FALSE when it cannot be */
LOCAL BOOL user_str( CONST UB *src, UB *dst, INT max )
{
	INT	i;

	for ( i = 0; i < max; i++ ) {
		if ( ( i == 0 || ( ( (UBINT)( src + i ) ) & ( PAGE_SIZE - 1 ) ) == 0 )
		  && !user_ok(src + i, 1, FALSE) ) {
			return FALSE;
		}
		dst[i] = UACCESS(src[i]);
		if ( dst[i] == 0 ) {
			return TRUE;
		}
	}
	dst[max - 1] = 0;
	return TRUE;
}

LOCAL ER user_rect( CONST T_DPRECT *r, T_DPRECT *out )
{
	if ( !user_ok(r, sizeof(*r), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(out, r, sizeof(*out));
	return E_OK;
}

LOCAL ER svc_dp_fill_rect( INT gid, CONST T_DPRECT *r, UW colour )
{
	T_DPRECT	k;

	if ( !granted(gid) ) return E_ID;
	if ( user_rect(r, &k) < E_OK ) return E_MACV;
	return dp_fill_rect(gid, &k, colour);
}

LOCAL ER svc_dp_frame_rect( INT gid, CONST T_DPRECT *r, UW colour, INT width )
{
	T_DPRECT	k;

	if ( !granted(gid) ) return E_ID;
	if ( user_rect(r, &k) < E_OK ) return E_MACV;
	return dp_frame_rect(gid, &k, colour, width);
}

LOCAL ER svc_dp_line( INT gid, INT x0, INT y0, INT x1, INT y1, UW colour )
{
	if ( !granted(gid) ) return E_ID;
	return dp_line(gid, x0, y0, x1, y1, colour);
}

LOCAL ER svc_dp_put_argb( INT gid, INT x, INT y, CONST UW *pixels, INT pitch, INT w, INT h,
			  UW clear )
{
	if ( !granted(gid) ) return E_ID;
	if ( w <= 0 || h <= 0 || pitch < w ) return E_PAR;
	if ( !user_ok(pixels, (SZ)pitch * ( h - 1 ) * 4 + (SZ)w * 4, FALSE) ) return E_MACV;
	return UACCESS(dp_put_argb(gid, x, y, pixels, pitch, w, h, clear));
}

#define TEXT_MAX	512

/*
 * A process's letters: the face set to their size and put back as it
 * was, all while the font layer is held, so that the kernel's own
 * drawing never finds the face set to a process's size. gid below zero
 * measures.
 */
LOCAL INT text_as( INT gid, ID fid, INT x, INT y, CONST UB *t, UW colour, INT px )
{
	T_FNSTATE	st;
	INT		r;

	fn_hold();
	(void)fn_state_get(fid, &st);
	if ( px > 0 ) (void)fn_set_size(fid, px);
	r = ( gid >= 0 ) ? fn_draw(gid, fid, x, y, t, colour) : fn_width(fid, t);
	(void)fn_state_set(fid, &st);
	fn_release();
	return r;
}

/* Letters in the system's face, px tall; how far they reached */
LOCAL INT svc_dp_text( INT gid, INT x, INT y, CONST UB *utf8, UW colour, INT px )
{
	UB	t[TEXT_MAX];
	ID	fid = fn_system();

	if ( !granted(gid) ) return E_ID;
	if ( !user_str(utf8, t, sizeof(t)) ) return E_MACV;
	if ( fid <= 0 ) return E_NOEXS;
	return text_as(gid, fid, x, y, t, colour, px);
}

LOCAL INT svc_dp_text_width( CONST UB *utf8, INT px )
{
	UB	t[TEXT_MAX];
	ID	fid = fn_system();

	if ( !user_str(utf8, t, sizeof(t)) ) return E_MACV;
	if ( fid <= 0 ) return E_NOEXS;
	return text_as(-1, fid, 0, 0, t, 0, px);
}

#define TAD_DRAW_MAX	( 1024 * 1024 )

/*
 * A figure drawn at (ox, oy) inside `clip`, the paper left as it is. The
 * bytes are taken into the kernel first: the process may change its own
 * while they are read.
 */
LOCAL ER svc_dp_draw_tad( INT gid, CONST T_DPRECT *clip, INT ox, INT oy,
			  CONST UB *xml, SZ len )
{
	T_DPRECT	r;
	T_TAD		*doc;
	T_TVFIG		*f;
	UB		*copy;
	ER		er;

	if ( !granted(gid) ) return E_ID;
	if ( len <= 0 || len > TAD_DRAW_MAX ) return E_PAR;
	if ( !user_ok(clip, sizeof(*clip), FALSE) || !user_ok(xml, len, FALSE) ) return E_MACV;
	knl_ua_get(&r, clip, sizeof(r));
	if ( r.right <= r.left || r.bottom <= r.top ) return E_OK;
	copy = (UB *)Kmalloc(len);
	if ( copy == NULL ) return E_NOMEM;
	knl_ua_get(copy, xml, len);
	er = tad_parse(copy, len, NULL, &doc);
	if ( er >= E_OK ) {
		if ( tv_kind(doc) == TV_KIND_FIG && ( er = tv_fig(doc, &f) ) >= E_OK ) {
			T_FNSTATE	st;
			ID		fid = fn_system();

			/* the faces the figure's words set, put back for the kernel's own drawing */
			fn_hold();
			(void)fn_state_get(fid, &st);
			er = tv_fig_draw(gid, f, &r, r.left - ox, r.top - oy, doc, TAD_COL_NONE);
			(void)fn_state_set(fid, &st);
			fn_release();
			tv_fig_free(f);
		} else if ( er >= E_OK ) {
			er = E_PAR;		/* not a figure */
		}
		tad_free(doc);
	}
	Kfree(copy);
	return er;
}

/* The drawing environment of a window, from a key that may write it */
LOCAL INT svc_wm_obj_gid( ID key )
{
	TS_UUID	u;
	ID	pid = ts_get_pid();
	INT	wid, gid, i, free = -1;
	ER	er = knl_ob_key_uuid(key, OB_OP_WRITE, &u);

	if ( er < E_OK ) return er;
	if ( pid <= 0 ) return E_OBJ;
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;		/* not a window: nothing to draw in */
	gid = wm_gid(wid);
	if ( gid < 0 ) return gid;
	for ( i = 0; i < GRANT_MAX; i++ ) {
		if ( grants[i].pid == pid && grants[i].gid == gid && grants[i].wid == wid
		  && ts_uuid_cmp(&grants[i].win, &u) == 0 ) {
			return gid;
		}
		/* a slot whose window has gone may be taken again */
		if ( free < 0 && !grant_live(&grants[i]) ) {
			free = i;
		}
	}
	if ( free < 0 ) return E_LIMIT;
	grants[free].gid = gid;
	grants[free].pid = pid;
	grants[free].wid = wid;
	grants[free].win = u;
	return gid;
}

/* What was drawn in a window, shown: the rectangle of its work area, or all of it */
LOCAL ER svc_wm_obj_flush( ID key, CONST T_DPRECT *r )
{
	TS_UUID		u;
	T_DPRECT	k;
	T_WMWIN		w;
	INT		wid;
	ER		er = knl_ob_key_uuid(key, OB_OP_WRITE, &u);

	if ( er < E_OK ) return er;
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;
	if ( r != NULL ) {
		if ( user_rect(r, &k) < E_OK ) return E_MACV;
	} else if ( wm_ref(wid, &w) >= E_OK ) {
		k.left = 0;
		k.top = 0;
		k.right = w.work.right - w.work.left;
		k.bottom = w.work.bottom - w.work.top;
	} else {
		return E_OBJ;
	}
	(void)wm_damage(wid, &k);
	return wm_update();
}

/*
 * A surface the process draws its window's work area in, mapped into the
 * process (wm_surf_get); from a key that may write the window
 */
LOCAL ER svc_wm_obj_surf( ID key, T_WMSURF *s )
{
	TS_UUID		u;
	T_WMSURF	k;
	ID		pid = ts_get_pid();
	INT		wid;
	ER		er = knl_ob_key_uuid(key, OB_OP_WRITE, &u);

	if ( er < E_OK ) return er;
	if ( pid <= 0 ) return E_OBJ;
	if ( !user_ok(s, sizeof(*s), TRUE) ) return E_MACV;
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;
	er = wm_surf_get(wid, pid, &k);
	if ( er >= E_OK ) knl_ua_put(s, &k, sizeof(k));
	return er;
}

/* Rectangles of the work area drawn in the surface, put in the window and shown */
LOCAL ER svc_wm_obj_put( ID key, CONST T_DPRECT *r, INT n )
{
	TS_UUID		u;
	T_DPRECT	k[WM_PUT_MAX];
	ID		pid = ts_get_pid();
	INT		wid;
	ER		er = knl_ob_key_uuid(key, OB_OP_WRITE, &u);

	if ( er < E_OK ) return er;
	if ( pid <= 0 ) return E_OBJ;
	if ( n <= 0 || n > WM_PUT_MAX ) return E_PAR;
	if ( !user_ok(r, (SZ)n * (SZ)sizeof(T_DPRECT), FALSE) ) return E_MACV;
	knl_ua_get(k, r, (SZ)n * (SZ)sizeof(T_DPRECT));
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;
	er = wm_surf_put(wid, pid, k, n);
	if ( er < E_OK ) return er;
	return wm_update();
}

#define IMG_FILE_MAX	( 32 * 1024 * 1024 )

/*
 * A picture file (PNG, JPEG or BMP) turned into 0x00rrggbb pixels in the
 * process's buffer, row after row. With no buffer, or one of fewer than
 * w * h pixels, only the size is said; the second is E_LIMIT.
 */
LOCAL ER svc_dp_img_decode( CONST UB *data, SZ size, UW *px, SZ maxpx, INT *p_w, INT *p_h )
{
	UB	*copy;
	UW	*pix = NULL;
	INT	w = 0, h = 0;
	ER	er;

	if ( size <= 0 || size > IMG_FILE_MAX || maxpx < 0 ) return E_PAR;
	if ( !user_ok(data, size, FALSE) || !user_ok(p_w, sizeof(INT), TRUE)
	  || !user_ok(p_h, sizeof(INT), TRUE) ) return E_MACV;
	if ( px != NULL && !user_ok(px, maxpx * (SZ)sizeof(UW), TRUE) ) return E_MACV;
	copy = (UB *)Kmalloc(size);
	if ( copy == NULL ) return E_NOMEM;
	knl_ua_get(copy, data, size);
	er = img_size(copy, size, &w, &h);
	if ( er >= E_OK && px != NULL ) {
		if ( (SZ)w * h > maxpx ) {
			er = E_LIMIT;
		} else if ( ( er = img_decode(copy, size, &pix, &w, &h) ) >= E_OK ) {
			knl_ua_put(px, pix, (SZ)w * h * (SZ)sizeof(UW));
			Kfree(pix);
		}
	}
	Kfree(copy);
	if ( er >= E_OK || er == E_LIMIT ) {
		knl_ua_put(p_w, &w, sizeof(w));
		knl_ua_put(p_h, &h, sizeof(h));
	}
	return er;
}

LOCAL const FP dp_svc_tbl[TSN_DP_MAX + 1] = {
	[TSN_NUMBER(TSN_DP_FILL_RECT)]	= F(svc_dp_fill_rect),
	[TSN_NUMBER(TSN_DP_FRAME_RECT)]	= F(svc_dp_frame_rect),
	[TSN_NUMBER(TSN_DP_LINE)]	= F(svc_dp_line),
	[TSN_NUMBER(TSN_DP_PUT_ARGB)]	= F(svc_dp_put_argb),
	[TSN_NUMBER(TSN_DP_TEXT)]	= F(svc_dp_text),
	[TSN_NUMBER(TSN_DP_TEXT_WIDTH)]	= F(svc_dp_text_width),
	[TSN_NUMBER(TSN_DP_DRAW_TAD)]	= F(svc_dp_draw_tad),
	[TSN_NUMBER(TSN_DP_IMG_DECODE)]	= F(svc_dp_img_decode),
};

LOCAL UB svc_wm_key_char( UINT code, UINT mods )
{
	return wm_key_char(code, mods);
}

/*
 * Menus for a process. A menu is the process's that made it (the menu
 * keeps it, knl_mn_owner); one whose process has ended is let go of when
 * the next is made.
 */
IMPORT ID   knl_mn_owner( ID mid );		/* outer_kernel/wm/mn.c */
IMPORT void knl_mn_set_owner( ID mid, ID pid );
IMPORT void knl_mn_reap( BOOL (*gone)( ID pid ) );

LOCAL BOOL mn_mine( ID mid )
{
	ID	pid = knl_mn_owner(mid);

	return (BOOL)( pid > 0 && pid == ts_get_pid() );
}

LOCAL BOOL mn_prc_gone( ID pid )
{
	T_RPRC	r;

	return (BOOL)( ts_ref_prc(pid, &r) < E_OK );
}

LOCAL ER svc_mn_cre_men( CONST TS_UUID *def, ID *p_mid )
{
	TS_UUID	d;
	ID	mid;
	ER	er;

	if ( !user_ok(def, sizeof(*def), FALSE) || !user_ok(p_mid, sizeof(*p_mid), TRUE) ) {
		return E_MACV;
	}
	knl_mn_reap(mn_prc_gone);		/* those whose process has gone */
	knl_ua_get(&d, def, sizeof(d));
	er = mn_cre_men(&d, &mid);
	if ( er < E_OK ) {
		return er;
	}
	knl_mn_set_owner(mid, ts_get_pid());
	knl_ua_put(p_mid, &mid, sizeof(mid));
	return E_OK;
}

LOCAL ER svc_mn_del_men( ID mid )
{
	if ( !mn_mine(mid) ) return E_ID;
	return mn_del_men(mid);
}

/* A definition named by the process, or none */
LOCAL ER user_def( CONST TS_UUID *def, TS_UUID *out, CONST TS_UUID **p )
{
	*p = NULL;
	if ( def != NULL ) {
		if ( !user_ok(def, sizeof(*def), FALSE) ) return E_MACV;
		knl_ua_get(out, def, sizeof(*out));
		*p = out;
	}
	return E_OK;
}

LOCAL INT svc_mn_chg_atr( ID mid, CONST TS_UUID *def, CONST char *code, UINT atr )
{
	TS_UUID		d;
	CONST TS_UUID	*pd;
	UB		c[MN_CODE_MAX];

	if ( !mn_mine(mid) ) return E_ID;
	if ( user_def(def, &d, &pd) < E_OK || !user_str((CONST UB *)code, c, sizeof(c)) ) {
		return E_MACV;
	}
	return mn_chg_atr(mid, pd, (CONST char *)c, atr);
}

LOCAL ER svc_mn_set_lst( ID mid, CONST TS_UUID *def, CONST char *code,
			 CONST UB *labels, INT stride, INT n )
{
	TS_UUID		d;
	CONST TS_UUID	*pd;
	UB		c[MN_CODE_MAX], *l;
	ER		er;

	if ( !mn_mine(mid) ) return E_ID;
	if ( n < 0 || stride <= 0 || stride > 4096 ) return E_PAR;
	if ( user_def(def, &d, &pd) < E_OK || !user_str((CONST UB *)code, c, sizeof(c))
	  || ( n > 0 && !user_ok(labels, (SZ)stride * n, FALSE) ) ) {
		return E_MACV;
	}
	l = (UB *)Kmalloc((SZ)stride * ( n > 0 ? n : 1 ));
	if ( l == NULL ) return E_NOMEM;
	if ( n > 0 ) knl_ua_get(l, labels, (SZ)stride * n);
	er = mn_set_lst(mid, pd, (CONST char *)c, l, stride, n);
	Kfree(l);
	return er;
}

LOCAL ER svc_mn_chg_idx( ID mid, CONST TS_UUID *def, CONST char *code, INT index, UINT atr )
{
	TS_UUID		d;
	CONST TS_UUID	*pd;
	UB		c[MN_CODE_MAX];

	if ( !mn_mine(mid) ) return E_ID;
	if ( user_def(def, &d, &pd) < E_OK || !user_str((CONST UB *)code, c, sizeof(c)) ) {
		return E_MACV;
	}
	return mn_chg_idx(mid, pd, (CONST char *)c, index, atr);
}

/* Shown on the process's window and waited for; what was chosen */
LOCAL ER svc_mn_pop_men( ID mid, ID wkey, INT x, INT y, UD when, T_MNSEL *sel )
{
	T_MNSEL	s;
	TS_UUID	u;
	INT	wid, pid, cmd;
	ER	er;

	if ( !mn_mine(mid) ) return E_ID;
	if ( !user_ok(sel, sizeof(*sel), TRUE) ) return E_MACV;
	er = knl_ob_key_uuid(wkey, OB_OP_WRITE, &u);
	if ( er < E_OK ) return er;
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;
	mn_winlist_fill(mid);			/* ウインドウ, when the menu has it */
	mn_tail_fill(mid);			/* 小物 and 実行 */
	pid = mn_opn_men(mid, wid, x, y);
	if ( pid < 0 ) return (ER)pid;
	wm_panel_draw(pid);
	wm_update();
	cmd = knl_wmobj_menu_run(pid, when);
	wm_panel_close(pid);
	wm_update();
	if ( cmd <= 0 ) {
		return ( cmd < 0 ) ? (ER)cmd : E_NOEXS;
	}
	er = mn_get_sel(mid, cmd, &s);
	if ( er >= E_OK && ( mn_winlist_do(&s) || mn_tail_do(&s) ) ) {
		return E_NOEXS;			/* the system's: nothing for the program to do */
	}
	if ( er >= E_OK ) {
		knl_ua_put(sel, &s, sizeof(s));
	}
	return er;
}

/*
 * かな漢字変換 for a process: a session of the converter that is the
 * process's, keys given as the keyboard gives them and turned into the
 * converter's in the input mode each key came in. A session whose
 * process has ended is closed when the next is opened.
 */
#define KC_SESS_MAX	16

LOCAL ID	kc_owner[KC_SESS_MAX];
LOCAL INT	kc_kid[KC_SESS_MAX];
LOCAL INT	kc_roman[KC_SESS_MAX];	/* how its letters were last typed: 1 romaji, 0 kana, -1 not yet */

LOCAL INT kc_slot( INT kid )
{
	INT	i;
	ID	pid = ts_get_pid();

	for ( i = 0; i < KC_SESS_MAX; i++ ) {
		if ( kc_owner[i] == pid && kc_kid[i] == kid && kid > 0 ) return i;
	}
	return -1;
}

LOCAL INT svc_kc_open( void )
{
	T_RPRC	r;
	INT	i, slot = -1, kid;

	for ( i = 0; i < KC_SESS_MAX; i++ ) {
		if ( kc_owner[i] > 0 && ts_ref_prc(kc_owner[i], &r) < E_OK ) {
			kc_close(kc_kid[i]);		/* its process has gone */
			kc_owner[i] = 0;
		}
		if ( kc_owner[i] == 0 && slot < 0 ) slot = i;
	}
	if ( slot < 0 ) return E_LIMIT;
	kid = kc_open();
	if ( kid <= 0 ) return ( kid < 0 ) ? kid : E_LIMIT;
	kc_owner[slot] = ts_get_pid();
	kc_kid[slot] = kid;
	kc_roman[slot] = -1;
	return kid;
}

LOCAL ER svc_kc_close( INT kid )
{
	INT	i = kc_slot(kid);

	if ( i < 0 ) return E_ID;
	kc_close(kid);
	kc_owner[i] = 0;
	return E_OK;
}

LOCAL UINT svc_kc_mode( void )
{
	return wm_msg_get_mode();
}

/* The composition, handed back to the process when the call has made it */
LOCAL INT kc_answer( INT r, CONST T_KCOUT *o, T_KCOUT *out )
{
	if ( r >= 0 && r != KC_NOTMINE ) {
		knl_ua_put(out, o, sizeof(T_KCOUT));
	}
	return r;
}

/*
 * A key as the keyboard gave it, in the input mode it came in (T_OBNTM
 * imode: the mode once the key was worked, in the keys' order). In 日本語
 * a key that is not a letter is the converter's while it is composing,
 * and a letter always: romaji as the letter, kana keys as the kana on
 * the key. In 英語 no key is the converter's. The mode is the caller's,
 * never the one the message line shows now: a mode key pressed after
 * this key may have changed that already.
 */
LOCAL INT svc_kc_hid_key( INT kid, UINT key, UINT mods, UINT mode, T_KCOUT *out )
{
	INT	i = kc_slot(kid), r, roman;
	UINT	code, stat = 0;
	BOOL	shift = (BOOL)( ( mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 );
	UB	ch;
	T_KCOUT	*o;

	if ( i < 0 ) return E_ID;
	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	if ( ( mode & WM_MODE_ALPH ) != 0 ) {
		return KC_NOTMINE;
	}
	roman = ( ( mode & WM_MODE_ROMAN ) != 0 ) ? 1 : 0;
	if ( roman != kc_roman[i] ) {
		(void)kc_input(kid, roman ? TSMOZC_M_ROMAN : 0);
		kc_roman[i] = roman;
	}
	if ( shift ) stat |= TSMOZC_S_SHIFT;
	if ( ( mode & WM_MODE_KANA ) != 0 ) stat |= TSMOZC_S_KANA;
	code = kc_code_key(key, mods);
	if ( code == 0 ) {
		ch = wm_key_char(key, mods);
		code = roman ? ( ( ch > 0x20 && ch < 0x7F ) ? ch : 0 ) : kc_kana_key_m(key, mods);
		if ( code == 0 ) return KC_NOTMINE;
	}
	o = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
	if ( o == NULL ) return E_NOMEM;
	r = kc_answer(kc_key(kid, code, stat, o), o, out);
	Kfree(o);
	return r;
}

LOCAL INT svc_kc_choose( INT kid, INT n, T_KCOUT *out )
{
	T_KCOUT	*o;
	INT	r;

	if ( kc_slot(kid) < 0 ) return E_ID;
	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	o = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
	if ( o == NULL ) return E_NOMEM;
	r = kc_answer(kc_choose(kid, n, o), o, out);
	Kfree(o);
	return r;
}

LOCAL INT svc_kc_list( INT kid, UB *buf, INT size, INT *p_count, INT *p_first, INT *p_total )
{
	UB	*b;
	INT	r, count = 0, first = 0, total = 0;

	if ( kc_slot(kid) < 0 ) return E_ID;
	if ( size <= 0 || size > 65536 ) return E_PAR;
	if ( !user_ok(buf, size, TRUE) || !user_ok(p_count, sizeof(INT), TRUE)
	  || !user_ok(p_first, sizeof(INT), TRUE) || !user_ok(p_total, sizeof(INT), TRUE) ) return E_MACV;
	b = (UB *)Kmalloc(size);
	if ( b == NULL ) return E_NOMEM;
	r = kc_list(kid, b, size, &count, &first, &total);
	if ( r >= 0 ) {
		knl_ua_put(buf, b, size);
		knl_ua_put(p_count, &count, sizeof(count));
		knl_ua_put(p_first, &first, sizeof(first));
		knl_ua_put(p_total, &total, sizeof(total));
	}
	Kfree(b);
	return r;
}

/*
 * A message of a process on the system's message line (the band at the
 * foot of the screen): a line of UTF-8, taken into the kernel first, or
 * NULL to take the one there away. It stays until the next press or key.
 */
#define MSG_PUT_MAX	256

LOCAL ER svc_wm_msg_put( CONST UB *utf8 )
{
	UB	line[MSG_PUT_MAX];
	INT	n = 0;

	if ( utf8 == NULL ) {
		return wm_msg_put(NULL);
	}
	while ( n < MSG_PUT_MAX - 1 ) {
		/* each page of it asked about as it is reached */
		if ( ( n == 0 || ( ( (UBINT)( utf8 + n ) ) & 0xFFF ) == 0 )
		  && !user_ok(utf8 + n, 1, FALSE) ) {
			return E_MACV;
		}
		line[n] = UACCESS(utf8[n]);
		if ( line[n] == 0 || line[n] == '\n' ) {
			break;
		}
		n++;
	}
	line[n] = 0;
	return wm_msg_put((CONST char *)line);
}

/*
 * The look table: read by anyone, since everything drawn on the screen
 * shows it anyway. An entry ユーザ環境設定 sets (the pointer, the
 * keyboard, the sizes and times, the sound) is written by whoever may
 * write the user information it is kept in; the windows' look by
 * whoever may write the machine's settings; any other only by an
 * administrator, since it is what every window is drawn with.
 */
LOCAL UW svc_wm_look( UINT num )
{
	return wm_look(num);
}

/* Whether the credentials let ops on the system's object of role (fixed by default) */
LOCAL BOOL may_on( CONST char *role, CONST char *fixed, CONST T_OBCRD *crd, UINT ops );

LOCAL ER svc_wm_set_look( UINT num, UW value )
{
	CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());
	BOOL		may;

	if ( cf_user_look(num) != NULL ) {
		may = may_on(SYSROLE_CONF_USER, SYSDEF_CONF_USER, crd, OB_OP_WRITE);
	} else if ( cf_look_look(num) != NULL ) {
		may = may_on(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, crd, OB_OP_WRITE);
	} else {
		may = knl_ob_is_admin(crd);
	}
	if ( !may ) {
		knl_ob_deny_note("wm_set_look", NULL, num, 0);
		return E_OACV;
	}
	if ( cf_user_look(num) != NULL ) {
		CONST CF_ITEM	*it = cf_user_look(num);

		if ( (INT)value < it->lo || (INT)value > it->hi ) {
			return E_PAR;
		}
		return knl_look_setting(num, value);	/* a person's setting, not the scheme's */
	}
	if ( cf_look_look(num) != NULL ) {
		/* the windows' look: on every window with a frame, at once */
		ER	er = knl_wm_look(num, (INT)value);

		if ( er > 0 ) {
			(void)wm_composite();
		}
		return ( er < E_OK ) ? er : E_OK;
	}
	return wm_set_look(num, value);
}

/* A key typed by a screen keyboard: its window, by a key that may write it */
LOCAL ER svc_wm_key_put( ID key, UINT code, UINT mods )
{
	TS_UUID	u;
	INT	wid;
	ER	er = knl_ob_key_uuid(key, OB_OP_WRITE, &u);

	if ( er < E_OK ) return er;
	wid = wm_obj_wid(&u);
	if ( wid <= 0 ) return E_OBJ;
	return wm_obj_key_put(wid, code, mods);
}

LOCAL ER svc_km_get( T_KMSTATE *st )
{
	T_KMSTATE	s;
	ER		er;

	if ( !user_ok(st, sizeof(*st), TRUE) ) {
		return E_MACV;
	}
	er = km_get(&s);
	if ( er >= E_OK ) {
		knl_ua_put(st, &s, sizeof(s));
	}
	return er;
}

/*
 * The layouts and the keys given others are the machine's settings: set
 * only by a process that may write them (SYSDEF_CONF_DEV), as システム
 * 環境設定 writes them, and asked here before anything is set.
 */
LOCAL ER svc_km_set( CONST T_KMSTATE *st, UINT what )
{
	T_KMSTATE	s;
	TS_UUID		u;
	ID		k;
	INT		n;

	if ( !user_ok(st, sizeof(*st), FALSE) ) {
		return E_MACV;
	}
	knl_ua_get(&s, st, sizeof(s));
	if ( ob_fnd_sys(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u) < E_OK ) {
		return E_SYS;
	}
	/* whether the caller may write them: readers not kept out, a writer met waited for (up to a second) */
	for ( n = 0; ( k = ob_opn_obj(&u, OB_OP_WRITE | OB_O_WEXCL) ) == E_BUSY && n < 100; n++ ) {
		tk_dly_tsk(10);
	}
	if ( k < E_OK ) {
		return ( k == E_NOEXS ) ? E_NOEXS : E_OACV;
	}
	(void)ob_cls_obj(k);
	return km_set(&s, what);
}

LOCAL INT svc_km_label( UINT what, UINT code, UINT mods, char *buf, INT max )
{
	char	s[64];
	INT	n;

	if ( max <= 0 || !user_ok(buf, (SZ)max, TRUE) ) {
		return E_MACV;
	}
	n = km_label(what, code, mods, s, ( max < (INT)sizeof(s) ) ? max : (INT)sizeof(s));
	if ( n >= 0 ) {
		knl_ua_put(buf, s, n + 1);
	}
	return n;
}

/* The size of the screen (a page's screen.width and height, among others) */
LOCAL ER svc_wm_screen( INT *p_w, INT *p_h )
{
	INT	w = 0, h = 0;

	if ( !user_ok(p_w, sizeof(*p_w), TRUE) || !user_ok(p_h, sizeof(*p_h), TRUE) ) {
		return E_MACV;
	}
	wm_screen(&w, &h);
	knl_ua_put(p_w, &w, sizeof(w));
	knl_ua_put(p_h, &h, sizeof(h));
	return E_OK;
}

/* How much farther a handle is reached by the press made at when: a number, nothing to copy */
LOCAL INT svc_wm_hit_slop( UD when )
{
	return wm_hit_slop(when);
}

/*
 * A rectangle of the screen copied into the process's memory (a program
 * that shares the screen the person chose to share, as the browser's
 * getDisplayMedia does), 'pitch' pixels to a row. The rows pass through
 * a band of the kernel's memory, GRAB_BAND pixels at most, each band
 * copied under the manager's lock as wm_grab_screen copies it.
 */
#define GRAB_BAND	( 64 * 1024 )

LOCAL ER svc_wm_grab_screen( UW *dst, INT pitch, CONST T_DPRECT *r, BOOL pointer )
{
	T_DPRECT	k, band;
	UW		*buf;
	INT		w, h, y, i, rows;
	ER		er = E_OK;

	if ( user_rect(r, &k) < E_OK ) return E_MACV;
	w = k.right - k.left;
	h = k.bottom - k.top;
	if ( w <= 0 || h <= 0 || pitch < w ) return E_PAR;
	if ( !user_ok(dst, ( (SZ)( h - 1 ) * pitch + w ) * (SZ)sizeof(UW), TRUE) ) return E_MACV;
	rows = GRAB_BAND / w;
	if ( rows < 1 ) rows = 1;
	if ( rows > h ) rows = h;
	buf = (UW *)Kmalloc((size_t)rows * (size_t)w * sizeof(UW));
	if ( buf == NULL ) return E_NOMEM;
	for ( y = 0; y < h && er >= E_OK; y += rows ) {
		band = k;
		band.top = k.top + y;
		band.bottom = ( band.top + rows < k.bottom ) ? band.top + rows : k.bottom;
		er = wm_grab_screen(buf, w, &band, pointer);
		for ( i = 0; er >= E_OK && i < band.bottom - band.top; i++ ) {
			knl_ua_put(dst + (SZ)( y + i ) * pitch, buf + (SZ)i * w, (SZ)w * (SZ)sizeof(UW));
		}
	}
	Kfree(buf);
	return er;
}

/* A scroll bar as the windows' own are drawn, in a window of the process's */
LOCAL ER svc_wm_draw_bar( INT gid, CONST T_DPRECT *r, CONST T_WMBAR *b, BOOL across, BOOL active )
{
	T_DPRECT	k;
	T_WMBAR		kb;

	if ( !granted(gid) ) return E_ID;
	if ( user_rect(r, &k) < E_OK || !user_ok(b, sizeof(*b), FALSE) ) return E_MACV;
	knl_ua_get(&kb, b, sizeof(kb));
	return wm_draw_bar(gid, &k, &kb, across, active);
}

/*
 * The tray for a process (design 18.16). The tray is an object of the
 * system's, kept open with a key of the kernel's, and a key answers
 * only the one that opened it; so a process's call is done by a task
 * of the kernel's, one call at a time, and what goes in and comes out
 * passes through memory of the kernel's. What a process puts in and
 * takes out is the person's, as the desktop's windows' is.
 */
#define TJ_PSH		1
#define TJ_ADD		2
#define TJ_SET		3
#define TJ_STS		4
#define TJ_REF		5
#define TJ_REA		6
#define TJ_SEL		7
#define TJ_MOV		8
#define TJ_DEL		9
#define TJ_CLR		10
#define TJ_BYTES_MAX	( 16 * 1024 * 1024 )	/* bytes of one set going in or one record coming out */
#define TJ_PRI		12

typedef struct {
	INT	op;
	INT	level, rec, to, n;
	T_TRREC	r[TR_REC_MAX];
	UB	name[TR_NAME_MAX];
	BOOL	named;
	UB	*buf;
	SZ	size, asz;
	T_TRSTS	sts;
	T_TRINF	inf;
	ER	er;
} TRJOB;

LOCAL ID	tj_mtx = 0;		/* one call at a time */
LOCAL ID	tj_go = 0, tj_done = 0;
LOCAL ID	tj_tsk = 0;
LOCAL TRJOB	tj;

LOCAL void tj_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		if ( tk_wai_sem(tj_go, 1, TMO_FEVR) < E_OK ) {
			continue;
		}
		switch ( tj.op ) {
		case TJ_PSH:	tj.er = tr_psh_dat(tj.r, tj.n, tj.named ? tj.name : NULL);	break;
		case TJ_ADD:	tj.er = tr_add_dat(tj.r, tj.n);				break;
		case TJ_SET:	tj.er = tr_set_dat(( tj.n > 0 ) ? tj.r : NULL, tj.n);		break;
		case TJ_STS:	tj.er = tr_get_sts(&tj.sts);					break;
		case TJ_REF:	tj.er = tr_ref_rec(tj.level, tj.rec, &tj.inf);			break;
		case TJ_REA:	tj.er = tr_rea_rec(tj.level, tj.rec, tj.buf, tj.size, &tj.asz);	break;
		case TJ_SEL:	tj.er = tr_sel_dat(tj.level);					break;
		case TJ_MOV:	tj.er = tr_mov_dat(tj.level, tj.to);				break;
		case TJ_DEL:	tj.er = tr_del_dat(tj.level);					break;
		case TJ_CLR:	tj.er = tr_clr_tra();						break;
		default:	tj.er = E_PAR;							break;
		}
		(void)tk_sig_sem(tj_done, 1);
	}
}

/* The task and what it waits on, made the first time a process asks; the lock taken */
LOCAL ER tj_begin( void )
{
	if ( tj_mtx == 0 ) {
		T_CMTX	c;
		ID	id;

		c.exinf = NULL;
		c.mtxatr = TA_INHERIT;
		c.ceilpri = 0;
		id = tk_cre_mtx(&c);
		if ( id <= 0 ) return E_NOMEM;
		/* the first to set it wins; tasks on other processors race here */
		if ( atomic_cmpxchg((UW *)&tj_mtx, (UW)id, 0) == 0 ) {
			id = 0;
		}
		if ( id > 0 ) tk_del_mtx(id);
	}
	if ( tk_loc_mtx(tj_mtx, TMO_FEVR) < E_OK ) return E_SYS;
	if ( tj_tsk <= 0 ) {
		T_CSEM	cs;
		T_CTSK	ct;

		knl_memset(&cs, 0, sizeof(cs));
		cs.sematr = TA_TFIFO | TA_FIRST;
		cs.maxsem = 1;
		if ( tj_go <= 0 ) tj_go = tk_cre_sem(&cs);
		if ( tj_done <= 0 ) tj_done = tk_cre_sem(&cs);
		knl_memset(&ct, 0, sizeof(ct));
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)tj_task;
		ct.itskpri = TJ_PRI;
		ct.stksz = 16 * 1024;
		if ( tj_go > 0 && tj_done > 0 ) {
			tj_tsk = tk_cre_tsk(&ct);
			if ( tj_tsk > 0 && tk_sta_tsk(tj_tsk, 0) < E_OK ) {
				(void)tk_del_tsk(tj_tsk);
				tj_tsk = 0;
			}
		}
		if ( tj_tsk <= 0 ) {
			tj_tsk = 0;
			(void)tk_unl_mtx(tj_mtx);
			return E_NOMEM;
		}
	}
	knl_memset(&tj, 0, sizeof(tj));
	return E_OK;
}

/* The call done by the task; its answer, the lock still held */
LOCAL ER tj_run( INT op )
{
	tj.op = op;
	(void)tk_sig_sem(tj_go, 1);
	(void)tk_wai_sem(tj_done, 1, TMO_FEVR);
	return tj.er;
}

LOCAL void tj_end( void )
{
	INT	i;

	for ( i = 0; i < tj.n && i < TR_REC_MAX; i++ ) {
		if ( tj.r[i].data != NULL ) Kfree((void *)tj.r[i].data);
	}
	if ( tj.buf != NULL ) Kfree(tj.buf);
	tj.buf = NULL;
	tj.n = 0;
	(void)tk_unl_mtx(tj_mtx);
}

/* The tray records of a process's set, each one's bytes copied in */
LOCAL ER tj_recs( CONST T_TRREC *recs, INT n )
{
	SZ	total = 0;
	INT	i;

	if ( n <= 0 || n > TR_REC_MAX ) return E_PAR;
	if ( !user_ok(recs, sizeof(T_TRREC) * (SZ)n, FALSE) ) return E_MACV;
	for ( i = 0; i < n; i++ ) {
		T_TRREC	r;

		knl_ua_get(&r, &recs[i], sizeof(r));

		tj.r[i] = r;
		tj.r[i].data = NULL;
		tj.n = i + 1;
		if ( r.size < 0 || r.size > TJ_BYTES_MAX - total ) return E_LIMIT;
		total += r.size;
		if ( r.size > 0 ) {
			if ( !user_ok(r.data, r.size, FALSE) ) return E_MACV;
			tj.r[i].data = Kmalloc(r.size);
			if ( tj.r[i].data == NULL ) return E_NOMEM;
			knl_ua_get((void *)tj.r[i].data, r.data, r.size);
		}
	}
	return E_OK;
}

LOCAL ER svc_tr_put( INT op, CONST T_TRREC *recs, INT n, CONST UB *name )
{
	ER	er = tj_begin();

	if ( er < E_OK ) return er;
	if ( op == TJ_SET && n == 0 ) {
		er = E_OK;
	} else {
		er = tj_recs(recs, n);
	}
	if ( er >= E_OK && name != NULL ) {
		tj.named = TRUE;
		if ( !user_str(name, tj.name, sizeof(tj.name)) ) er = E_MACV;
	}
	if ( er >= E_OK ) er = tj_run(op);
	tj_end();
	return er;
}

LOCAL ER svc_tr_psh_dat( CONST T_TRREC *recs, INT n, CONST UB *name )
{
	return svc_tr_put(TJ_PSH, recs, n, name);
}

LOCAL ER svc_tr_add_dat( CONST T_TRREC *recs, INT n )
{
	return svc_tr_put(TJ_ADD, recs, n, NULL);
}

LOCAL ER svc_tr_set_dat( CONST T_TRREC *recs, INT n )
{
	return svc_tr_put(TJ_SET, recs, n, NULL);
}

LOCAL ER svc_tr_get_sts( T_TRSTS *sts )
{
	ER	er;

	if ( !user_ok(sts, sizeof(*sts), TRUE) ) return E_MACV;
	if ( ( er = tj_begin() ) < E_OK ) return er;
	er = tj_run(TJ_STS);
	if ( er >= E_OK ) knl_ua_put(sts, &tj.sts, sizeof(tj.sts));
	tj_end();
	return er;
}

LOCAL ER svc_tr_ref_rec( INT level, INT rec, T_TRINF *inf )
{
	ER	er;

	if ( !user_ok(inf, sizeof(*inf), TRUE) ) return E_MACV;
	if ( ( er = tj_begin() ) < E_OK ) return er;
	tj.level = level;
	tj.rec = rec;
	er = tj_run(TJ_REF);
	if ( er >= E_OK ) knl_ua_put(inf, &tj.inf, sizeof(tj.inf));
	tj_end();
	return er;
}

LOCAL ER svc_tr_rea_rec( INT level, INT rec, void *buf, SZ size, SZ *p_asize )
{
	ER	er;

	if ( size < 0 || ( buf != NULL && size > 0 && !user_ok(buf, size, TRUE) )
	  || ( p_asize != NULL && !user_ok(p_asize, sizeof(*p_asize), TRUE) ) ) {
		return E_MACV;
	}
	if ( size > TJ_BYTES_MAX ) size = TJ_BYTES_MAX;
	if ( ( er = tj_begin() ) < E_OK ) return er;
	tj.level = level;
	tj.rec = rec;
	if ( buf != NULL && size > 0 ) {
		tj.buf = (UB *)Kmalloc(size);
		tj.size = ( tj.buf != NULL ) ? size : 0;
	}
	er = ( buf != NULL && size > 0 && tj.buf == NULL ) ? E_NOMEM : tj_run(TJ_REA);
	if ( er >= E_OK ) {
		if ( tj.buf != NULL ) {
			knl_ua_put(buf, tj.buf, ( tj.asz < size ) ? tj.asz : size);
		}
		if ( p_asize != NULL ) knl_ua_put(p_asize, &tj.asz, sizeof(tj.asz));
	}
	tj_end();
	return er;
}

LOCAL ER svc_tr_one( INT op, INT level, INT to )
{
	ER	er = tj_begin();

	if ( er < E_OK ) return er;
	tj.level = level;
	tj.to = to;
	er = tj_run(op);
	tj_end();
	return er;
}

LOCAL ER svc_tr_sel_dat( INT level )		{ return svc_tr_one(TJ_SEL, level, 0); }
LOCAL ER svc_tr_mov_dat( INT level, INT to )	{ return svc_tr_one(TJ_MOV, level, to); }
LOCAL ER svc_tr_del_dat( INT level )		{ return svc_tr_one(TJ_DEL, level, 0); }
LOCAL ER svc_tr_clr_tra( void )			{ return svc_tr_one(TJ_CLR, 0, 0); }

LOCAL const FP wm_svc_tbl[TSN_WM_MAX + 1] = {
	[TSN_NUMBER(TSN_WM_OBJ_GID)]	= F(svc_wm_obj_gid),
	[TSN_NUMBER(TSN_WM_OBJ_FLUSH)]	= F(svc_wm_obj_flush),
	[TSN_NUMBER(TSN_WM_KEY_CHAR)]	= F(svc_wm_key_char),
	[TSN_NUMBER(TSN_MN_CRE_MEN)]	= F(svc_mn_cre_men),
	[TSN_NUMBER(TSN_MN_DEL_MEN)]	= F(svc_mn_del_men),
	[TSN_NUMBER(TSN_MN_CHG_ATR)]	= F(svc_mn_chg_atr),
	[TSN_NUMBER(TSN_MN_SET_LST)]	= F(svc_mn_set_lst),
	[TSN_NUMBER(TSN_MN_CHG_IDX)]	= F(svc_mn_chg_idx),
	[TSN_NUMBER(TSN_MN_POP_MEN)]	= F(svc_mn_pop_men),
	[TSN_NUMBER(TSN_KC_OPEN)]	= F(svc_kc_open),
	[TSN_NUMBER(TSN_KC_CLOSE)]	= F(svc_kc_close),
	[TSN_NUMBER(TSN_KC_HID_KEY)]	= F(svc_kc_hid_key),
	[TSN_NUMBER(TSN_KC_CHOOSE)]	= F(svc_kc_choose),
	[TSN_NUMBER(TSN_KC_LIST)]	= F(svc_kc_list),
	[TSN_NUMBER(TSN_KC_MODE)]	= F(svc_kc_mode),
	[TSN_NUMBER(TSN_WM_MSG_PUT)]	= F(svc_wm_msg_put),
	[TSN_NUMBER(TSN_WM_LOOK)]	= F(svc_wm_look),
	[TSN_NUMBER(TSN_WM_SET_LOOK)]	= F(svc_wm_set_look),
	[TSN_NUMBER(TSN_WM_DRAW_BAR)]	= F(svc_wm_draw_bar),
	[TSN_NUMBER(TSN_TR_PSH_DAT)]	= F(svc_tr_psh_dat),
	[TSN_NUMBER(TSN_TR_ADD_DAT)]	= F(svc_tr_add_dat),
	[TSN_NUMBER(TSN_TR_SET_DAT)]	= F(svc_tr_set_dat),
	[TSN_NUMBER(TSN_TR_GET_STS)]	= F(svc_tr_get_sts),
	[TSN_NUMBER(TSN_TR_REF_REC)]	= F(svc_tr_ref_rec),
	[TSN_NUMBER(TSN_TR_REA_REC)]	= F(svc_tr_rea_rec),
	[TSN_NUMBER(TSN_TR_SEL_DAT)]	= F(svc_tr_sel_dat),
	[TSN_NUMBER(TSN_TR_MOV_DAT)]	= F(svc_tr_mov_dat),
	[TSN_NUMBER(TSN_TR_DEL_DAT)]	= F(svc_tr_del_dat),
	[TSN_NUMBER(TSN_TR_CLR_TRA)]	= F(svc_tr_clr_tra),
	[TSN_NUMBER(TSN_WM_OBJ_SURF)]	= F(svc_wm_obj_surf),
	[TSN_NUMBER(TSN_WM_OBJ_PUT)]	= F(svc_wm_obj_put),
	[TSN_NUMBER(TSN_WM_KEY_PUT)]	= F(svc_wm_key_put),
	[TSN_NUMBER(TSN_KM_GET)]	= F(svc_km_get),
	[TSN_NUMBER(TSN_KM_SET)]	= F(svc_km_set),
	[TSN_NUMBER(TSN_KM_LABEL)]	= F(svc_km_label),
	[TSN_NUMBER(TSN_WM_SCREEN)]	= F(svc_wm_screen),
	[TSN_NUMBER(TSN_WM_HIT_SLOP)]	= F(svc_wm_hit_slop),
	[TSN_NUMBER(TSN_WM_GRAB_SCREEN)]	= F(svc_wm_grab_screen),
};

/* What went to the console, from where the process had got to */
LOCAL INT svc_tm_log_read( UD *p_pos, UB *buf, INT max )
{
	UD	pos;
	INT	n;

	if ( max <= 0 ) {
		return 0;
	}
	if ( !knl_prc_user_ok(ts_get_pid(), p_pos, sizeof(*p_pos), TRUE)
	  || !knl_prc_user_ok(ts_get_pid(), buf, (SZ)max, TRUE) ) {
		return E_MACV;
	}
	knl_ua_get(&pos, p_pos, sizeof(pos));
	n = UACCESS(tm_log_read(&pos, buf, max));
	knl_ua_put(p_pos, &pos, sizeof(pos));
	return n;
}

/*
 * A process's string on the console, taken in through the kernel a
 * piece at a time: each page it runs into is made sure of before it is
 * read. E_MACV when it runs into one that is not the process's, after
 * what came before it was put out.
 */
#define PUT_PIECE	256

LOCAL INT svc_tm_putstring( CONST UB *s )
{
	UB	t[PUT_PIECE];
	INT	i;
	BOOL	end = FALSE;

	while ( !end ) {
		for ( i = 0; i < PUT_PIECE - 1; i++ ) {
			if ( ( i == 0 || ( ( (UBINT)( s + i ) ) & ( PAGE_SIZE - 1 ) ) == 0 )
			  && !user_ok(s + i, 1, FALSE) ) {
				t[i] = 0;
				(void)tm_putstring(t);
				return E_MACV;
			}
			t[i] = UACCESS(s[i]);
			if ( t[i] == 0 ) {
				end = TRUE;
				break;
			}
		}
		t[i] = 0;
		(void)tm_putstring(t);
		s += i;
	}
	return E_OK;
}

LOCAL const FP tm_prc_tbl[TSN_TM_MAX + 1] = {
	[TSN_NUMBER(TSN_TM_PUTSTRING)]	= F(svc_tm_putstring),
	[TSN_NUMBER(TSN_TM_PUTCHAR)]	= F(tm_putchar),
	[TSN_NUMBER(TSN_TM_LOG_READ)]	= F(svc_tm_log_read),
};

LOCAL const FP tm_svc_tbl[TSN_TM_MAX + 1] = {
	[TSN_NUMBER(TSN_TM_PUTSTRING)]	= F(tm_putstring),
	[TSN_NUMBER(TSN_TM_PUTCHAR)]	= F(tm_putchar),
	[TSN_NUMBER(TSN_TM_LOG_READ)]	= F(svc_tm_log_read),
};

/* tk_ (class 0) for a task of a process: svc_tk.c */
IMPORT const FP knl_tk_prc_tbl[TSN_TK_MAX + 1];
/* gp_ (class 10), the GPU: device/gpu/gpu_svc.c */
IMPORT const FP knl_gp_svc_tbl[TSN_GP_MAX + 1];
/* sr_ (class 11), the serial ports: device/ser/ser_svc.c */
IMPORT const FP knl_sr_svc_tbl[TSN_SR_MAX + 1];
/* us_ (class 12), USB devices of a program's own: device/usb/uusb.c */
IMPORT const FP knl_us_svc_tbl[TSN_US_MAX + 1];

/*
 * The table of rights (include/ts/svc.h, docs/tessronos/A-svc.md, 権限の表):
 * the calls that act on the whole system and the right each takes. The
 * gateway asks before the call is made; the layers behind ask again
 * where they always did, for the kernel's own callers as well.
 */
typedef struct {
	UW	fncd;
	UB	right;		/* TSN_RIGHT_* */
} SVCRIGHT;

LOCAL const SVCRIGHT svc_rights[] = {
	{ TSN_TK_SET_TIM,	TSN_RIGHT_CLOCK },
	{ TSN_TK_SET_TIM_U,	TSN_RIGHT_CLOCK },
	{ TSN_TK_SET_UTC,	TSN_RIGHT_CLOCK },
	{ TSN_DT_SETTIME,	TSN_RIGHT_CLOCK },
	{ TSN_DT_SETSYSTZ,	TSN_RIGHT_ZONE },
	{ TSN_DT_SETSYSZONE,	TSN_RIGHT_ZONE },
#ifdef TSN_DT_SETRTC
	{ TSN_DT_SETRTC,	TSN_RIGHT_ZONE },
#endif
	{ TSN_SO_SETIFADDR,	TSN_RIGHT_ADMIN },
	{ TSN_SO_DHCP_START,	TSN_RIGHT_ADMIN },
	{ TSN_OB_ATT_VOL,	TSN_RIGHT_ADMIN },
	{ TSN_OB_DET_VOL,	TSN_RIGHT_ADMIN },
	{ TSN_OB_SET_DOM,	TSN_RIGHT_ADMIN },
	{ TSN_WM_SET_LOOK,	TSN_RIGHT_CONF_USER },
	{ TSN_KM_SET,		TSN_RIGHT_CONF_DEV },
};

/* Whether the credentials let ops on the object named by the string s */
LOCAL BOOL may_on( CONST char *role, CONST char *fixed, CONST T_OBCRD *crd, UINT ops )
{
	TS_UUID	u;

	return ( ob_fnd_sys(role, fixed, &u) >= E_OK && ( knl_ob_permit_on(&u, crd) & ops ) == ops );
}

/* E_OK when process pid has the right call fncd takes, E_OACV when not */
LOCAL ER svc_right( UINT fncd, ID pid )
{
	CONST T_OBCRD	*crd;
	BOOL		ok = FALSE;
	UINT		i, right = TSN_RIGHT_NONE;

	for ( i = 0; i < sizeof(svc_rights) / sizeof(svc_rights[0]); i++ ) {
		if ( svc_rights[i].fncd == fncd ) {
			right = svc_rights[i].right;
			break;
		}
	}
	if ( right == TSN_RIGHT_NONE ) {
		return E_OK;
	}
	crd = knl_ob_crd_of(pid);
	if ( knl_ob_is_admin(crd) ) {
		return E_OK;
	}
	switch ( right ) {
	  case TSN_RIGHT_CLOCK:
		ok = ( ( knl_ob_permit_on(&ob_uuid_clock, crd) & OB_OP_WRITE ) != 0 );
		break;
	  case TSN_RIGHT_ZONE:
		ok = may_on(SYSROLE_CALENDAR, SYSDEF_CALENDAR, crd, OB_OP_ATRRD | OB_OP_ATRWR);
		break;
	  case TSN_RIGHT_CONF_DEV:
		ok = may_on(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, crd, OB_OP_WRITE);
		break;
	  case TSN_RIGHT_CONF_USER:
		ok = may_on(SYSROLE_CONF_USER, SYSDEF_CONF_USER, crd, OB_OP_WRITE);
		break;
	  default:				/* TSN_RIGHT_ADMIN */
		break;
	}
	if ( !ok ) {
		knl_ob_deny_note("svc", NULL, fncd, right);
	}
	return ok ? E_OK : E_OACV;
}

/*
 * tk_cre_tsk of a task of the system process at EL0: it makes tasks of
 * protection level 2/3, as it is itself, and none that runs at EL1.
 */
LOCAL ID svc_sys_cre_tsk( CONST T_CTSK *pk )
{
	if ( pk == NULL ) {
		return E_PAR;
	}
	if ( ( pk->tskatr & TA_RNG3 ) < TA_RNG2 ) {
		return E_OACV;
	}
	return tk_cre_tsk(pk);
}

#ifdef USE_KTEST
/*
 * For the tests (TSN_TM_PAN_PROBE): a word of the process's read by the
 * kernel, with the way opened once it has made sure of it, or straight,
 * as a kernel that forgot to open it would.
 */
LOCAL UBINT svc_pan_probe( CONST UD *p, BOOL opened )
{
	if ( ts_get_pid() > 0 && !user_ok(p, sizeof(*p), FALSE) ) {
		return (UBINT)(BINT)E_MACV;
	}
	return opened ? UACCESS(*p) : *(volatile CONST UD *)p;
}
#endif

/*
 * Called from vector.S with the exception frame; returns the value for
 * the register the call's value goes back in (KNL_SVC_RET).
 *
 * A task of a process (its TCB's owner) has class 0 through
 * knl_tk_prc_tbl, where it names only its process's objects. While it
 * is in a call of any other class, which reaches the kernel's layers
 * and their locks, its svcdepth says so, and no other task of the
 * process holds it still or ends it there (svc_tk.c).
 */
EXPORT UBINT knl_svc_gateway( T_EXCFRAME *frame )
{
	UINT	fncd = (UINT)KNL_SVC_FNCD(frame);
	UINT	cls = TSN_CLASS(fncd), no = TSN_NUMBER(fncd);
	FP	fn = NULL;
	UBINT	ret;
	BOOL	inside, busy, opened;
	TCB	*me = knl_ctxtsk;		/* interrupts are still disabled here */
	ID	prc = ( me != NULL ) ? me->owner : 0;

	if ( cls == TSN_CLASS_TK && no <= TSN_TK_MAX ) {
		fn = ( prc > 0 ) ? knl_tk_prc_tbl[no] : tk_svc_tbl[no];
	} else if ( cls == TSN_CLASS_TF && no <= TSN_TS_MAX ) {
		fn = ts_svc_tbl[no];
	} else if ( cls == TSN_CLASS_DT && no <= TSN_DT_MAX ) {
		fn = ( prc > 0 ) ? dt_prc_tbl[no] : dt_svc_tbl[no];
	} else if ( cls == TSN_CLASS_TM && no <= TSN_TM_MAX ) {
		fn = ( prc > 0 ) ? tm_prc_tbl[no] : tm_svc_tbl[no];
	} else if ( cls == TSN_CLASS_OB && no <= TSN_OB_MAX ) {
		fn = knl_ob_svc_tbl[no];
	} else if ( cls == TSN_CLASS_FS && no <= TSN_FS_MAX ) {
		fn = fs_svc_tbl[no];
	} else if ( cls == TSN_CLASS_SO && no <= TSN_SO_MAX ) {
		fn = knl_so_svc_tbl[no];
	} else if ( cls == TSN_CLASS_DP && no <= TSN_DP_MAX ) {
		fn = dp_svc_tbl[no];
	} else if ( cls == TSN_CLASS_WM && no <= TSN_WM_MAX ) {
		fn = wm_svc_tbl[no];
	} else if ( cls == TSN_CLASS_GP && no <= TSN_GP_MAX ) {
		fn = knl_gp_svc_tbl[no];
	} else if ( cls == TSN_CLASS_SR && no <= TSN_SR_MAX ) {
		fn = knl_sr_svc_tbl[no];
	} else if ( cls == TSN_CLASS_US && no <= TSN_US_MAX ) {
		fn = knl_us_svc_tbl[no];
	}
#ifdef USE_KTEST
	if ( fncd == TSN_TM_PAN_PROBE ) {
		fn = F(svc_pan_probe);
	}
#endif
	if ( fn == NULL ) {
		return (UBINT)(BINT)E_RSFN;
	}

	busy = ( prc > 0 && cls != TSN_CLASS_TK );
	if ( busy ) {
		me->svcdepth++;
	}
#ifdef USE_UITEST
	if ( prc > 0 ) {
		__atomic_fetch_add(&knl_svc_calls, 1, __ATOMIC_RELAXED);
	}
#endif
	enaint(0);				/* the system call may block or be preempted */

	/*
	 * A call that acts on the whole system is a process's to make only
	 * with the right the table of rights asks for it (svc_right). The
	 * system process's tasks are the system's, and are not asked.
	 */
	if ( prc > 0 && ( ret = (UBINT)(BINT)svc_right(fncd, prc) ) != (UBINT)E_OK ) {
		if ( busy ) {
			me->svcdepth--;
		}
		disint();
		return ret;
	}

	/*
	 * A task of the system process at EL0 is part of the system, and
	 * the kernel takes what it hands by pointer as it is: its call runs
	 * with the way to the user pages open from end to end. A process's
	 * call keeps it shut, and opens it only for what has been made sure
	 * of (UACCESS).
	 */
	opened = ( prc == 0 ) ? knl_ua_open() : FALSE;

	/*
	 * A call into the files, the sockets, the objects, drawing or the
	 * windows is not cut short by the end of its process: see
	 * knl_prc_call_enter. Drawing and the windows hold the drawing
	 * environments, the font engine, the screen's order and what the
	 * call allocated; a task taken down there would leave them half
	 * changed. A call of any class goes out through knl_prc_call_leave
	 * while the kernel has met an address of some process's that was
	 * not there.
	 */
	inside = ( cls == TSN_CLASS_FS || cls == TSN_CLASS_SO || cls == TSN_CLASS_OB
		|| cls == TSN_CLASS_DP || cls == TSN_CLASS_WM || cls == TSN_CLASS_GP
		|| fncd == TSN_TS_SPN_PRC || fncd == TSN_TS_SPN_OBJ
		|| fncd == TSN_TS_LKP_OBJ );
	if ( inside && knl_prc_call_enter() < E_OK ) {
		ret = (UBINT)(BINT)( ( cls == TSN_CLASS_FS ) ? EX_INTR : E_DISWAI );
	} else if ( fncd == TSN_TS_FLT_RET ) {
		/* back to where a fault's picture says: the call's own frame is turned to it */
		ret = (UBINT)(BINT)knl_prc_flt_return(frame, (CONST T_TSFLT *)KNL_SVC_ARG(frame, 0));
		if ( (BINT)ret >= E_OK ) {
			ret = KNL_SVC_RET(frame);
		}
	} else if ( fncd == TSN_TS_SIG_RET && prc > 0 ) {
		/* back from a signal's handler, its mask put back */
		ret = (UBINT)(BINT)knl_psx_sig_return(frame, (CONST T_TSSIGFR *)KNL_SVC_ARG(frame, 0));
		if ( (BINT)ret >= E_OK ) {
			ret = KNL_SVC_RET(frame);
		}
	} else {
		ret = ((SVCFN)fn)(KNL_SVC_ARG(frame, 0), KNL_SVC_ARG(frame, 1), KNL_SVC_ARG(frame, 2),
				  KNL_SVC_ARG(frame, 3), KNL_SVC_ARG(frame, 4), KNL_SVC_ARG(frame, 5),
				  KNL_SVC_ARG(frame, 6), KNL_SVC_ARG(frame, 7));
		if ( inside || knl_prc_kfaults > 0 ) {
			knl_prc_call_leave(inside);
		}
	}
	knl_ua_close(opened);

	/*
	 * A signal the process catches is handed over as the call returns
	 * (include/ts/psx.h), and a process that was stopped stays here
	 * until it goes on.
	 */
	if ( prc > 0 ) {
		if ( knl_psx_deliver(frame, (UD)ret) ) {
			ret = KNL_SVC_RET(frame);	/* turned to the signal's entry */
		}
	}
	if ( busy ) {
		me->svcdepth--;
	}
	disint();
	return ret;
}
