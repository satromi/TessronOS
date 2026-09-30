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
 *	The user pages of a process are readable and writable from EL1 (PAN
 *	is not used). The calls of the classes below that take a buffer --
 *	tk_, ts_, ob_, fs_, so_, the drawing and window calls -- first make
 *	sure it lies in the calling process's own pages (knl_prc_user_ok);
 *	dt_ and tm_putstring still use pointer arguments as they are. The calls of the core
 *	from a process are served by svc_tk.c. The protection level 3
 *	permission table is added with TessronOS/PM.
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include "sysdepend/cpu/core/armv8a/cpu_task.h"
#include <tm/tmonitor.h>
#include <ts/svc.h>
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

typedef UBINT (*SVCFN)( UBINT, UBINT, UBINT, UBINT, UBINT, UBINT, UBINT, UBINT );

#define F(fn)	((FP)(fn))

LOCAL const FP tk_svc_tbl[TSN_TK_MAX + 1] = {
	[TSN_NUMBER(TSN_TK_CRE_TSK)]	= F(tk_cre_tsk),
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
	return ts_get_arg(buf, size);
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
		*p_uuid = u;
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
	er = ob_rea_rec(key, OB_RND_DATA, 0, buf, len, &asz);
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
		*pk_psts = s;
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
		*pk_rprc = r;
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
	m = *msg;
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
		*buf = m;
	}
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
		*p_ns = ns;
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
	a = *p_adr;
	er = ts_map_mem(&a, size, align, prot);
	if ( er == E_OK ) {
		*p_adr = a;
	}
	return er;
}

LOCAL const FP ts_svc_tbl[TSN_TS_MAX + 1] = {
	[TSN_NUMBER(TSN_TS_GET_MONO)]	= F(svc_ts_get_mono),
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
		*p_t = t;
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
	v = *t;
	er = local ? dt_localtime(&v, &f) : dt_gmtime(&v, &f);
	if ( er >= E_OK ) {
		*tm = f;
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
	f = *tm;
	er = dt_mktime(&f, &t);
	if ( er >= E_OK ) {
		*p_t = t;
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
		*p_minutes = z;
	}
	return er;
}

LOCAL const FP dt_prc_tbl[TSN_DT_MAX + 1] = {
	[TSN_NUMBER(TSN_DT_GETTIME)]	= F(svc_dt_gettime),
	[TSN_NUMBER(TSN_DT_SETTIME)]	= F(dt_settime),
	[TSN_NUMBER(TSN_DT_GMTIME)]	= F(svc_dt_gmtime),
	[TSN_NUMBER(TSN_DT_LOCALTIME)]	= F(svc_dt_localtime),
	[TSN_NUMBER(TSN_DT_MKTIME)]	= F(svc_dt_mktime),
	[TSN_NUMBER(TSN_DT_GETSYSTZ)]	= F(svc_dt_getsystz),
	[TSN_NUMBER(TSN_DT_SETSYSTZ)]	= F(dt_setsystz),
};

LOCAL const FP dt_svc_tbl[TSN_DT_MAX + 1] = {
	[TSN_NUMBER(TSN_DT_GETTIME)]	= F(dt_gettime),
	[TSN_NUMBER(TSN_DT_SETTIME)]	= F(dt_settime),
	[TSN_NUMBER(TSN_DT_GMTIME)]	= F(dt_gmtime),
	[TSN_NUMBER(TSN_DT_LOCALTIME)]	= F(dt_localtime),
	[TSN_NUMBER(TSN_DT_MKTIME)]	= F(dt_mktime),
	[TSN_NUMBER(TSN_DT_GETSYSTZ)]	= F(dt_getsystz),
	[TSN_NUMBER(TSN_DT_SETSYSTZ)]	= F(dt_setsystz),
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
		dst[i] = src[i];
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
		n = wr ? fs_write(fd, buf + done, want) : fs_read(fd, buf + done, want);
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
	return fs_stat(p, st);
}

LOCAL ER svc_fs_fstat( INT fd, T_FSTAT *st )
{
	if ( !fd_mine(fd) ) return EX_BADF;
	if ( !user_ok(st, sizeof(*st), TRUE) ) return EX_FAULT;
	return fs_fstat(fd, st);
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
	return fs_getdents(fd, buf, nent);
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
	return fs_statvfs(p, st);
}

/*
 * Mounting through a device object's key (design 12.2.3): the key and
 * its rights say everything, so nothing more is looked at here than
 * the fimp's name, copied in.
 */
LOCAL ER svc_fs_attach_dev( ID devkey, CONST char *fimpnm, UINT flags )
{
	char	f[FS_FIMPNM_MAX];
	ER	er;

	if ( fimpnm == NULL ) return EX_INVAL;
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
	return fs_mounts(buf, max);
}

LOCAL const FP fs_svc_tbl[TSN_FS_MAX + 1] = {
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
		dst[i] = src[i];
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
	*out = *r;
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
	return dp_put_argb(gid, x, y, pixels, pitch, w, h, clear);
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
	r = *clip;
	if ( r.right <= r.left || r.bottom <= r.top ) return E_OK;
	copy = (UB *)Kmalloc(len);
	if ( copy == NULL ) return E_NOMEM;
	knl_memcpy(copy, xml, (INT)len);
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
	knl_memcpy(copy, data, (INT)size);
	er = img_size(copy, size, &w, &h);
	if ( er >= E_OK && px != NULL ) {
		if ( (SZ)w * h > maxpx ) {
			er = E_LIMIT;
		} else if ( ( er = img_decode(copy, size, &pix, &w, &h) ) >= E_OK ) {
			knl_memcpy(px, pix, (INT)( (SZ)w * h * sizeof(UW) ));
			Kfree(pix);
		}
	}
	Kfree(copy);
	if ( er >= E_OK || er == E_LIMIT ) {
		*p_w = w;
		*p_h = h;
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
 * Menus for a process. A menu is the process's that made it; one whose
 * process has ended is let go of when the next is made.
 */
LOCAL ID	mn_owner[MN_MAX + 1];

LOCAL BOOL mn_mine( ID mid )
{
	return (BOOL)( mid >= 1 && mid <= MN_MAX && mn_owner[mid] > 0
		    && mn_owner[mid] == ts_get_pid() );
}

LOCAL ER svc_mn_cre_men( CONST TS_UUID *def, ID *p_mid )
{
	T_RPRC	r;
	TS_UUID	d;
	ID	mid, i;
	ER	er;

	if ( !user_ok(def, sizeof(*def), FALSE) || !user_ok(p_mid, sizeof(*p_mid), TRUE) ) {
		return E_MACV;
	}
	for ( i = 1; i <= MN_MAX; i++ ) {
		if ( mn_owner[i] > 0 && ts_ref_prc(mn_owner[i], &r) < E_OK ) {
			(void)mn_del_men(i);		/* its process has gone */
			mn_owner[i] = 0;
		}
	}
	d = *def;
	er = mn_cre_men(&d, &mid);
	if ( er < E_OK ) {
		return er;
	}
	if ( mid < 1 || mid > MN_MAX ) {
		(void)mn_del_men(mid);
		return E_LIMIT;
	}
	mn_owner[mid] = ts_get_pid();
	*p_mid = mid;
	return E_OK;
}

LOCAL ER svc_mn_del_men( ID mid )
{
	if ( !mn_mine(mid) ) return E_ID;
	mn_owner[mid] = 0;
	return mn_del_men(mid);
}

/* A definition named by the process, or none */
LOCAL ER user_def( CONST TS_UUID *def, TS_UUID *out, CONST TS_UUID **p )
{
	*p = NULL;
	if ( def != NULL ) {
		if ( !user_ok(def, sizeof(*def), FALSE) ) return E_MACV;
		*out = *def;
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
	if ( n < 0 || n > WM_ITEM_MAX || stride <= 0 || stride > 4096 ) return E_PAR;
	if ( user_def(def, &d, &pd) < E_OK || !user_str((CONST UB *)code, c, sizeof(c))
	  || ( n > 0 && !user_ok(labels, (SZ)stride * n, FALSE) ) ) {
		return E_MACV;
	}
	l = (UB *)Kmalloc((SZ)stride * ( n > 0 ? n : 1 ));
	if ( l == NULL ) return E_NOMEM;
	if ( n > 0 ) knl_memcpy(l, labels, stride * n);
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
	pid = mn_opn_men(mid, wid, x, y);
	if ( pid < 0 ) return (ER)pid;
	wm_panel_draw(pid);
	wm_composite();
	cmd = knl_wmobj_menu_run(pid, when);
	wm_panel_close(pid);
	wm_composite();
	if ( cmd <= 0 ) {
		return ( cmd < 0 ) ? (ER)cmd : E_NOEXS;
	}
	er = mn_get_sel(mid, cmd, &s);
	if ( er >= E_OK && mn_winlist_do(&s) ) {
		return E_NOEXS;			/* the system's: nothing for the program to do */
	}
	if ( er >= E_OK ) {
		*sel = s;
	}
	return er;
}

/*
 * かな漢字変換 for a process: a session of the converter that is the
 * process's, keys given as the keyboard gives them and turned into the
 * converter's in the input mode the message line shows. A session whose
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
		knl_memcpy(out, o, sizeof(T_KCOUT));
	}
	return r;
}

/*
 * A key as the keyboard gave it. In 英語 nothing is the converter's. In
 * 日本語 a key that is not a letter is the converter's while it is
 * composing, and a letter always: romaji as the letter, kana keys as
 * the kana on the key.
 */
LOCAL INT svc_kc_hid_key( INT kid, UINT key, UINT mods, T_KCOUT *out )
{
	INT	i = kc_slot(kid), r, roman;
	UINT	mode = wm_msg_get_mode(), code, stat = 0;
	BOOL	shift = (BOOL)( ( mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 );
	UB	ch;
	T_KCOUT	*o;

	if ( i < 0 ) return E_ID;
	if ( !user_ok(out, sizeof(*out), TRUE) ) return E_MACV;
	if ( ( mode & WM_MODE_ALPH ) != 0 ) return KC_NOTMINE;
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
		code = roman ? ( ( ch > 0x20 && ch < 0x7F ) ? ch : 0 ) : kc_kana_key(key, shift);
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
		knl_memcpy(buf, b, size);
		*p_count = count;
		*p_first = first;
		*p_total = total;
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
		if ( utf8[n] == 0 || utf8[n] == '\n' ) {
			break;
		}
		line[n] = utf8[n];
		n++;
	}
	line[n] = 0;
	return wm_msg_put((CONST char *)line);
}

/*
 * The look table: read by anyone, since everything drawn on the screen
 * shows it anyway; written only by an administrator, since it is what
 * every window of every person is drawn with.
 */
LOCAL UW svc_wm_look( UINT num )
{
	return wm_look(num);
}

LOCAL ER svc_wm_set_look( UINT num, UW value )
{
	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return E_OACV;
	}
	if ( cf_user_look(num) != NULL ) {
		CONST CF_ITEM	*it = cf_user_look(num);

		if ( (INT)value < it->lo || (INT)value > it->hi ) {
			return E_PAR;
		}
		return knl_look_setting(num, value);	/* a person's setting, not the scheme's */
	}
	return wm_set_look(num, value);
}

/* A scroll bar as the windows' own are drawn, in a window of the process's */
LOCAL ER svc_wm_draw_bar( INT gid, CONST T_DPRECT *r, CONST T_WMBAR *b, BOOL across, BOOL active )
{
	T_DPRECT	k;
	T_WMBAR		kb;

	if ( !granted(gid) ) return E_ID;
	if ( user_rect(r, &k) < E_OK || !user_ok(b, sizeof(*b), FALSE) ) return E_MACV;
	kb = *b;
	return wm_draw_bar(gid, &k, &kb, across, active);
}

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
	pos = *p_pos;
	n = tm_log_read(&pos, buf, max);
	*p_pos = pos;
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
			t[i] = s[i];
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

/*
 * Called from vector.S with the exception frame; returns the value for x0.
 *
 * A task of a process (its TCB's owner) has class 0 through
 * knl_tk_prc_tbl, where it names only its process's objects. While it
 * is in a call of any other class, which reaches the kernel's layers
 * and their locks, its svcdepth says so, and no other task of the
 * process holds it still or ends it there (svc_tk.c).
 */
EXPORT UBINT knl_svc_gateway( T_EXCFRAME *frame )
{
	UINT	fncd = (UINT)frame->x[8];
	UINT	cls = TSN_CLASS(fncd), no = TSN_NUMBER(fncd);
	FP	fn = NULL;
	UBINT	ret;
	BOOL	inside, busy;
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
	}
	if ( fn == NULL ) {
		return (UBINT)(BINT)E_RSFN;
	}

	busy = ( prc > 0 && cls != TSN_CLASS_TK );
	if ( busy ) {
		me->svcdepth++;
	}
	enaint(0);				/* the system call may block or be preempted */

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
		|| cls == TSN_CLASS_DP || cls == TSN_CLASS_WM );
	if ( inside && knl_prc_call_enter() < E_OK ) {
		ret = (UBINT)(BINT)( ( cls == TSN_CLASS_FS ) ? EX_INTR : E_DISWAI );
	} else {
		ret = ((SVCFN)fn)(frame->x[0], frame->x[1], frame->x[2], frame->x[3],
				  frame->x[4], frame->x[5], frame->x[6], frame->x[7]);
		if ( inside || knl_prc_kfaults > 0 ) {
			knl_prc_call_leave(inside);
		}
	}
	if ( busy ) {
		me->svcdepth--;
	}
	disint();
	return ret;
}
