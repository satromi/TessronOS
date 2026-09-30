/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svc_stub.S
 *	System calls of a TessronOS program (design 9.7)
 *
 *	Every call is "svc #0" with the function code in x8 and up to eight
 *	arguments in x0-x7, just as the C calling convention leaves them;
 *	the result comes back in x0. The names are the kernel's, so a
 *	program is written against the same headers (include/ts/ob.h,
 *	include/ts/dp.h, include/ts/fs.h, include/ts/uapp.h), and the
 *	sockets against include/ts/soapp.h.
 */

#include <ts/svc.h>

	.text
	.balign	4

	.macro	SVCSTUB name, fncd
	.global	\name
	.type	\name, %function
\name:
	mov	x8, #(\fncd & 0xffff)
	movk	x8, #(\fncd >> 16), lsl #16
	svc	#0
	ret
	.size	\name, . - \name
	.endm

	/* processes and time */
	SVCSTUB	ts_ext_prc,	TSN_TS_EXT_PRC
	SVCSTUB	ts_get_pid,	TSN_TS_GET_PID
	SVCSTUB	ts_get_mono,	TSN_TS_GET_MONO
	SVCSTUB	ts_rcv_msg,	TSN_TS_RCV_MSG
	SVCSTUB	ts_get_arg,	TSN_TS_GET_ARG
	SVCSTUB	ts_wai_prc,	TSN_TS_WAI_PRC
	SVCSTUB	ts_gen_uuid,	TSN_TS_GEN_UUID
	SVCSTUB	ts_map_mem,	TSN_TS_MAP_MEM
	SVCSTUB	ts_unm_mem,	TSN_TS_UNM_MEM
	SVCSTUB	ts_ctl_mem,	TSN_TS_CTL_MEM
	SVCSTUB	tk_dly_tsk_u,	TSN_TK_DLY_TSK_U
	SVCSTUB	tk_slp_tsk_u,	TSN_TK_SLP_TSK_U
	SVCSTUB	tk_wai_sem_u,	TSN_TK_WAI_SEM_U

	/* class 3: the calendar */
	SVCSTUB	dt_gettime,	TSN_DT_GETTIME
	SVCSTUB	dt_localtime,	TSN_DT_LOCALTIME
	SVCSTUB	dt_mktime,	TSN_DT_MKTIME
	SVCSTUB	dt_getsystz,	TSN_DT_GETSYSTZ
	SVCSTUB	dt_gmtime,	TSN_DT_GMTIME
	SVCSTUB	dt_settime,	TSN_DT_SETTIME
	SVCSTUB	dt_setsystz,	TSN_DT_SETSYSTZ
	SVCSTUB	ts_snd_msg,	TSN_TS_SND_MSG
	SVCSTUB	ts_ter_prc,	TSN_TS_TER_PRC
	SVCSTUB	ts_ref_prc,	TSN_TS_REF_PRC
	SVCSTUB	tk_get_tid,	TSN_TK_GET_TID
	SVCSTUB	tk_dly_tsk,	TSN_TK_DLY_TSK
	SVCSTUB	tm_putstring,	TSN_TM_PUTSTRING

	/* objects of the core, the process's own (class 0, design 9.7) */
	SVCSTUB	tk_cre_tsk,	TSN_TK_CRE_TSK
	SVCSTUB	tk_del_tsk,	TSN_TK_DEL_TSK
	SVCSTUB	tk_sta_tsk,	TSN_TK_STA_TSK
	SVCSTUB	tk_ext_tsk,	TSN_TK_EXT_TSK
	SVCSTUB	tk_exd_tsk,	TSN_TK_EXD_TSK
	SVCSTUB	tk_ter_tsk,	TSN_TK_TER_TSK
	SVCSTUB	tk_dis_dsp,	TSN_TK_DIS_DSP
	SVCSTUB	tk_chg_pri,	TSN_TK_CHG_PRI
	SVCSTUB	tk_rot_rdq,	TSN_TK_ROT_RDQ
	SVCSTUB	tk_rel_wai,	TSN_TK_REL_WAI
	SVCSTUB	tk_ref_tsk,	TSN_TK_REF_TSK
	SVCSTUB	tk_sus_tsk,	TSN_TK_SUS_TSK
	SVCSTUB	tk_rsm_tsk,	TSN_TK_RSM_TSK
	SVCSTUB	tk_frsm_tsk,	TSN_TK_FRSM_TSK
	SVCSTUB	tk_slp_tsk,	TSN_TK_SLP_TSK
	SVCSTUB	tk_wup_tsk,	TSN_TK_WUP_TSK
	SVCSTUB	tk_can_wup,	TSN_TK_CAN_WUP
	SVCSTUB	tk_cre_sem,	TSN_TK_CRE_SEM
	SVCSTUB	tk_del_sem,	TSN_TK_DEL_SEM
	SVCSTUB	tk_sig_sem,	TSN_TK_SIG_SEM
	SVCSTUB	tk_wai_sem,	TSN_TK_WAI_SEM
	SVCSTUB	tk_ref_sem,	TSN_TK_REF_SEM
	SVCSTUB	tk_cre_flg,	TSN_TK_CRE_FLG
	SVCSTUB	tk_del_flg,	TSN_TK_DEL_FLG
	SVCSTUB	tk_set_flg,	TSN_TK_SET_FLG
	SVCSTUB	tk_clr_flg,	TSN_TK_CLR_FLG
	SVCSTUB	tk_wai_flg,	TSN_TK_WAI_FLG
	SVCSTUB	tk_ref_flg,	TSN_TK_REF_FLG
	SVCSTUB	tk_cre_mbx,	TSN_TK_CRE_MBX
	SVCSTUB	tk_cre_mtx,	TSN_TK_CRE_MTX
	SVCSTUB	tk_del_mtx,	TSN_TK_DEL_MTX
	SVCSTUB	tk_loc_mtx,	TSN_TK_LOC_MTX
	SVCSTUB	tk_unl_mtx,	TSN_TK_UNL_MTX
	SVCSTUB	tk_ref_mtx,	TSN_TK_REF_MTX
	SVCSTUB	tk_cre_mbf,	TSN_TK_CRE_MBF
	SVCSTUB	tk_del_mbf,	TSN_TK_DEL_MBF
	SVCSTUB	tk_snd_mbf,	TSN_TK_SND_MBF
	SVCSTUB	tk_rcv_mbf,	TSN_TK_RCV_MBF
	SVCSTUB	tk_ref_mbf,	TSN_TK_REF_MBF
	SVCSTUB	tk_get_tim,	TSN_TK_GET_TIM
	SVCSTUB	tk_set_tim,	TSN_TK_SET_TIM
	SVCSTUB	tk_get_utc,	TSN_TK_GET_UTC
	SVCSTUB	tk_set_utc,	TSN_TK_SET_UTC
	SVCSTUB	tk_get_otm,	TSN_TK_GET_OTM
	SVCSTUB	tk_cre_cyc,	TSN_TK_CRE_CYC
	SVCSTUB	tm_log_read,	TSN_TM_LOG_READ

	/* real objects (class 7) */
	SVCSTUB	ob_opn_obj,	TSN_OB_OPN_OBJ
	SVCSTUB	ob_cls_obj,	TSN_OB_CLS_OBJ
	SVCSTUB	ob_cre_obj,	TSN_OB_CRE_OBJ
	SVCSTUB	ob_del_obj,	TSN_OB_DEL_OBJ
	SVCSTUB	ob_ref_obj,	TSN_OB_REF_OBJ
	SVCSTUB	ob_lst_obj,	TSN_OB_LST_OBJ
	SVCSTUB	ob_lnk_obj,	TSN_OB_LNK_OBJ
	SVCSTUB	ob_unl_obj,	TSN_OB_UNL_OBJ
	SVCSTUB	ob_rea_rec,	TSN_OB_REA_REC
	SVCSTUB	ob_wri_rec,	TSN_OB_WRI_REC
	SVCSTUB	ob_apd_rec,	TSN_OB_APD_REC
	SVCSTUB	ob_trn_rec,	TSN_OB_TRN_REC
	SVCSTUB	ob_del_rec,	TSN_OB_DEL_REC
	SVCSTUB	ob_lst_rec,	TSN_OB_LST_REC
	SVCSTUB	ob_get_atr,	TSN_OB_GET_ATR
	SVCSTUB	ob_set_atr,	TSN_OB_SET_ATR
	SVCSTUB	ob_get_ico,	TSN_OB_GET_ICO
	SVCSTUB	ob_set_ico,	TSN_OB_SET_ICO
	SVCSTUB	ob_ref_vol,	TSN_OB_REF_VOL
	SVCSTUB	ob_get_prt,	TSN_OB_GET_PRT
	SVCSTUB	ob_set_prt,	TSN_OB_SET_PRT
	SVCSTUB	ob_dup_key,	TSN_OB_DUP_KEY
	SVCSTUB	ob_fnd_nam,	TSN_OB_FND_NAM
	SVCSTUB	ob_fnd_lnk,	TSN_OB_FND_LNK
	SVCSTUB	ob_lst_lnk,	TSN_OB_LST_LNK
	SVCSTUB	ob_get_crd,	TSN_OB_GET_CRD
	SVCSTUB	ob_login,	TSN_OB_LOGIN
	SVCSTUB	ob_set_pwd,	TSN_OB_SET_PWD
	SVCSTUB	ob_rea_res,	TSN_OB_REA_RES
	SVCSTUB	ob_wri_res,	TSN_OB_WRI_RES
	SVCSTUB	ob_del_res,	TSN_OB_DEL_RES
	SVCSTUB	ob_lst_res,	TSN_OB_LST_RES
	SVCSTUB	ob_beg_trx,	TSN_OB_BEG_TRX
	SVCSTUB	ob_end_trx,	TSN_OB_END_TRX
	SVCSTUB	ob_sch_rec,	TSN_OB_SCH_REC
	SVCSTUB	ob_trs_rec,	TSN_OB_TRS_REC
	SVCSTUB	ob_cpy_obj,	TSN_OB_CPY_OBJ
	SVCSTUB	ob_ntf_evt,	TSN_OB_NTF_EVT
	SVCSTUB	ob_can_evt,	TSN_OB_CAN_EVT
	SVCSTUB	ob_map_rec,	TSN_OB_MAP_REC
	SVCSTUB	ob_unm_rec,	TSN_OB_UNM_REC
	SVCSTUB	ob_get_dom,	TSN_OB_GET_DOM
	SVCSTUB	ob_att_vol,	TSN_OB_ATT_VOL
	SVCSTUB	ob_det_vol,	TSN_OB_DET_VOL

	/* files (class 2) */
	SVCSTUB	fs_open,	TSN_FS_OPEN
	SVCSTUB	fs_close,	TSN_FS_CLOSE
	SVCSTUB	fs_read,	TSN_FS_READ
	SVCSTUB	fs_write,	TSN_FS_WRITE
	SVCSTUB	fs_lseek,	TSN_FS_LSEEK
	SVCSTUB	fs_stat,	TSN_FS_STAT
	SVCSTUB	fs_fstat,	TSN_FS_FSTAT
	SVCSTUB	fs_ftruncate,	TSN_FS_FTRUNCATE
	SVCSTUB	fs_truncate,	TSN_FS_TRUNCATE
	SVCSTUB	fs_getdents,	TSN_FS_GETDENTS
	SVCSTUB	fs_mkdir,	TSN_FS_MKDIR
	SVCSTUB	fs_rmdir,	TSN_FS_RMDIR
	SVCSTUB	fs_unlink,	TSN_FS_UNLINK
	SVCSTUB	fs_rename,	TSN_FS_RENAME
	SVCSTUB	fs_statvfs,	TSN_FS_STATVFS
	SVCSTUB	fs_sync,	TSN_FS_SYNC
	SVCSTUB	fs_attach_dev,	TSN_FS_ATTACH_DEV
	SVCSTUB	fs_detach_dev,	TSN_FS_DETACH_DEV
	SVCSTUB	fs_mounts,	TSN_FS_MOUNTS
	SVCSTUB	fs_utime,	TSN_FS_UTIME

	/* sockets and the interface (class 5) */
	SVCSTUB	so_socket,	TSN_SO_SOCKET
	SVCSTUB	so_close,	TSN_SO_CLOSE
	SVCSTUB	so_bind,	TSN_SO_BIND
	SVCSTUB	so_connect,	TSN_SO_CONNECT
	SVCSTUB	so_listen,	TSN_SO_LISTEN
	SVCSTUB	so_accept,	TSN_SO_ACCEPT
	SVCSTUB	so_shutdown,	TSN_SO_SHUTDOWN
	SVCSTUB	so_send,	TSN_SO_SEND
	SVCSTUB	so_recv,	TSN_SO_RECV
	SVCSTUB	so_sendto,	TSN_SO_SENDTO
	SVCSTUB	so_recvfrom,	TSN_SO_RECVFROM
	SVCSTUB	so_getsockopt,	TSN_SO_GETSOCKOPT
	SVCSTUB	so_setsockopt,	TSN_SO_SETSOCKOPT
	SVCSTUB	so_getsockname,	TSN_SO_GETSOCKNAME
	SVCSTUB	so_getpeername,	TSN_SO_GETPEERNAME
	SVCSTUB	so_poll,	TSN_SO_POLL
	SVCSTUB	so_fcntl,	TSN_SO_FCNTL
	SVCSTUB	so_resolve,	TSN_SO_RESOLVE
	SVCSTUB	so_getifaddr,	TSN_SO_GETIFADDR
	SVCSTUB	so_getdns,	TSN_SO_GETDNS
	SVCSTUB	so_setifaddr,	TSN_SO_SETIFADDR
	SVCSTUB	so_dhcp_start,	TSN_SO_DHCP_START
	SVCSTUB	so_getobj,	TSN_SO_GETOBJ

	/* drawing in a window of one's own (class 8) */
	SVCSTUB	dp_fill_rect,	TSN_DP_FILL_RECT
	SVCSTUB	dp_frame_rect,	TSN_DP_FRAME_RECT
	SVCSTUB	dp_line,	TSN_DP_LINE
	SVCSTUB	dp_put_argb,	TSN_DP_PUT_ARGB
	SVCSTUB	dp_text,	TSN_DP_TEXT
	SVCSTUB	dp_text_width,	TSN_DP_TEXT_WIDTH
	SVCSTUB	dp_draw_tad,	TSN_DP_DRAW_TAD
	SVCSTUB	dp_img_decode,	TSN_DP_IMG_DECODE

	/* the window layer (class 9) */
	SVCSTUB	wm_obj_gid,	TSN_WM_OBJ_GID
	SVCSTUB	wm_obj_flush,	TSN_WM_OBJ_FLUSH
	SVCSTUB	wm_key_char,	TSN_WM_KEY_CHAR
	SVCSTUB	mn_cre_men,	TSN_MN_CRE_MEN
	SVCSTUB	mn_del_men,	TSN_MN_DEL_MEN
	SVCSTUB	mn_chg_atr,	TSN_MN_CHG_ATR
	SVCSTUB	mn_set_lst,	TSN_MN_SET_LST
	SVCSTUB	mn_chg_idx,	TSN_MN_CHG_IDX
	SVCSTUB	mn_pop_men,	TSN_MN_POP_MEN
	SVCSTUB	kc_open,	TSN_KC_OPEN
	SVCSTUB	kc_close,	TSN_KC_CLOSE
	SVCSTUB	kc_hid_key,	TSN_KC_HID_KEY
	SVCSTUB	kc_choose,	TSN_KC_CHOOSE
	SVCSTUB	kc_list,	TSN_KC_LIST
	SVCSTUB	kc_mode,	TSN_KC_MODE
	SVCSTUB	wm_msg_put,	TSN_WM_MSG_PUT
	SVCSTUB	wm_look,	TSN_WM_LOOK
	SVCSTUB	wm_set_look,	TSN_WM_SET_LOOK
	SVCSTUB	wm_draw_bar,	TSN_WM_DRAW_BAR
