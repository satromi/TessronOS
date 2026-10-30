/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svc.h
 *	System call gateway function codes (design 9.7).
 *	  svc #0, x8 = function code, x0-x5 = arguments, x0 = return value
 *	  function code: bits 31..16 class, bits 15..0 function number
 *	Used by both the kernel dispatch table and the user side stubs.
 */

#ifndef __TS_SVC_H__
#define __TS_SVC_H__

#define TSN_CLASS_SHIFT		16
#define TSN_CLASS(fncd)		((fncd) >> TSN_CLASS_SHIFT)
#define TSN_NUMBER(fncd)	((fncd) & 0xffff)
#define TSN(cls, no)		(((cls) << TSN_CLASS_SHIFT) | (no))

#define TSN_CLASS_TK		0	/* T-Kernel/OS */
#define TSN_CLASS_TF		1	/* TessronOS process / memory / IPC */
#define TSN_CLASS_FS		2
#define TSN_CLASS_DT		3
#define TSN_CLASS_PM		4
#define TSN_CLASS_SO		5
#define TSN_CLASS_TM		6	/* T-Monitor (debug output) */
#define TSN_CLASS_OB		7	/* real objects (design 18.5) */
#define TSN_CLASS_DP		8	/* drawing in a window of one's own (design 16.4.3) */
#define TSN_CLASS_WM		9	/* what of the window layer the objects do not cover */
#define TSN_CLASS_GP		10	/* the GPU (docs/tessronos/13-gpu.md 13.5) */
#define TSN_CLASS_SR		11	/* the serial ports (design 6.2) */
#define TSN_CLASS_US		12	/* USB devices of a program's own (design 6.3.1) */

/*
 * Rights (docs/tessronos/A-svc.md, 権限の表). A call that acts on the whole
 * system -- the time and the zone, the network interface, the volumes,
 * the look of the windows, the key layouts -- is a process's to make
 * only with the right the table of rights in svc_gate.c gives it, and
 * the gateway looks at that before the call is made (E_OACV). An
 * administrator has every right. The numbers below that need one say
 * which; a call that says none acts only on what is the caller's own,
 * or on what a key, an object's protection or a process's object lets
 * it at. The system process's tasks are not asked.
 */
#define TSN_RIGHT_NONE		0
#define TSN_RIGHT_ADMIN		1	/* an administrator */
#define TSN_RIGHT_CLOCK		2	/* who may write the clock device (時計) */
#define TSN_RIGHT_ZONE		3	/* who may write the attributes of the calendar (暦) */
#define TSN_RIGHT_CONF_DEV	4	/* who may write the machine's settings (機器の設定) */
#define TSN_RIGHT_CONF_USER	5	/* who may write the user information (ユーザ情報) */

/*
 * gp_ (class 10): the GPU for a process (include/ts/gpuapp.h). A
 * context holds the BOs the process made or took in, by number; the
 * process names a BO only by its number, and what its contexts hold
 * goes when it ends.
 */
#define TSN_GP_OPN_GPU		TSN(10, 1)
#define TSN_GP_CLS_GPU		TSN(10, 2)
#define TSN_GP_REF_GPU		TSN(10, 3)
#define TSN_GP_CRE_BO		TSN(10, 4)
#define TSN_GP_DEL_BO		TSN(10, 5)
#define TSN_GP_MAP_BO		TSN(10, 6)
#define TSN_GP_SUB_JOB		TSN(10, 7)
#define TSN_GP_WAI_FEN		TSN(10, 8)
#define TSN_GP_WAI_BO		TSN(10, 9)
#define TSN_GP_SHR_BO		TSN(10, 10)
#define TSN_GP_IMP_BO		TSN(10, 11)
#define TSN_GP_REF_BO		TSN(10, 12)
#ifdef __x86_64__
#define TSN_GP_BND_BO		TSN(10, 13)	/* a BO placed where the process chooses (Intel) */
#define TSN_GP_MAX		13
#else
#define TSN_GP_MAX		12
#endif

/*
 * sr_ (class 11): the serial ports for a process (include/ts/ser.h). A
 * process names a port it opened only by the number it was given, and
 * what it opened closes when it ends.
 */
#define TSN_SR_LST_PRT		TSN(11, 1)
#define TSN_SR_OPN_PRT		TSN(11, 2)
#define TSN_SR_CLS_PRT		TSN(11, 3)
#define TSN_SR_REA_PRT		TSN(11, 4)
#define TSN_SR_WRI_PRT		TSN(11, 5)
#define TSN_SR_SET_SPD		TSN(11, 6)
#define TSN_SR_MAX		6

#define TSN_US_LST_DEV		TSN(12, 1)
#define TSN_US_GET_DSC		TSN(12, 2)
#define TSN_US_OPN_DEV		TSN(12, 3)
#define TSN_US_CLS_DEV		TSN(12, 4)
#define TSN_US_CTL		TSN(12, 5)
#define TSN_US_SET_ALT		TSN(12, 6)
#define TSN_US_CLR_HLT		TSN(12, 7)
#define TSN_US_XFER		TSN(12, 8)
#define TSN_US_RST_DEV		TSN(12, 9)
#define TSN_US_MAX		9

/*
 * dp_ (class 8): drawing, for a process, into the drawing environment of
 * a window it has made. The environment is had from the window's key
 * (wm_obj_gid) and is the process's alone.
 */
#define TSN_DP_FILL_RECT	TSN(8, 1)
#define TSN_DP_FRAME_RECT	TSN(8, 2)
#define TSN_DP_LINE		TSN(8, 3)
#define TSN_DP_PUT_ARGB		TSN(8, 4)
#define TSN_DP_TEXT		TSN(8, 5)
#define TSN_DP_TEXT_WIDTH	TSN(8, 6)
#define TSN_DP_DRAW_TAD		TSN(8, 7)
#define TSN_DP_IMG_DECODE	TSN(8, 8)
#define TSN_DP_MAX		8

/*
 * wm_ (class 9), and the menus of mn_ for a program running as a
 * process (design 18.15): a menu made from a definition, set, shown on
 * the program's window and answered.
 */
#define TSN_WM_OBJ_GID		TSN(9, 1)
#define TSN_WM_OBJ_FLUSH	TSN(9, 2)
#define TSN_WM_KEY_CHAR		TSN(9, 3)
#define TSN_MN_CRE_MEN		TSN(9, 4)
#define TSN_MN_DEL_MEN		TSN(9, 5)
#define TSN_MN_CHG_ATR		TSN(9, 6)
#define TSN_MN_SET_LST		TSN(9, 7)
#define TSN_MN_CHG_IDX		TSN(9, 8)
#define TSN_MN_POP_MEN		TSN(9, 9)
#define TSN_KC_OPEN		TSN(9, 10)
#define TSN_KC_CLOSE		TSN(9, 11)
#define TSN_KC_HID_KEY		TSN(9, 12)
#define TSN_KC_CHOOSE		TSN(9, 13)
#define TSN_KC_LIST		TSN(9, 14)
#define TSN_KC_MODE		TSN(9, 15)
#define TSN_WM_MSG_PUT		TSN(9, 16)
#define TSN_WM_LOOK		TSN(9, 17)	/* an entry of the look table */
#define TSN_WM_SET_LOOK		TSN(9, 18)	/* and setting one: TSN_RIGHT_CONF_USER, and more by the entry */
#define TSN_WM_DRAW_BAR		TSN(9, 19)	/* a scroll bar in a window of one's own */
#define TSN_TR_PSH_DAT		TSN(9, 20)	/* the tray (include/ts/tray.h) */
#define TSN_TR_ADD_DAT		TSN(9, 21)
#define TSN_TR_SET_DAT		TSN(9, 22)
#define TSN_TR_GET_STS		TSN(9, 23)
#define TSN_TR_REF_REC		TSN(9, 24)
#define TSN_TR_REA_REC		TSN(9, 25)
#define TSN_TR_SEL_DAT		TSN(9, 26)
#define TSN_TR_MOV_DAT		TSN(9, 27)
#define TSN_TR_DEL_DAT		TSN(9, 28)
#define TSN_TR_CLR_TRA		TSN(9, 29)
#define TSN_WM_OBJ_SURF		TSN(9, 30)	/* a surface to draw the work area in */
#define TSN_WM_OBJ_PUT		TSN(9, 31)	/* and rectangles of it shown */
#define TSN_WM_KEY_PUT		TSN(9, 32)	/* a key typed by a screen keyboard */
#define TSN_KM_GET		TSN(9, 33)	/* the layouts and the keys given others */
#define TSN_KM_SET		TSN(9, 34)	/* and setting them: TSN_RIGHT_CONF_DEV */
#define TSN_KM_LABEL		TSN(9, 35)	/* the words a key is written with */
#define TSN_WM_SCREEN		TSN(9, 36)	/* the size of the screen */
#define TSN_WM_HIT_SLOP		TSN(9, 37)	/* how much farther a finger reaches a handle */
#define TSN_WM_GRAB_SCREEN	TSN(9, 38)	/* a rectangle of the screen copied (a page's screen sharing) */
#define TSN_WM_MAX		38

/* fs_ (class 2): the file management of T2EX, for a process */
#define TSN_FS_OPEN		TSN(2, 1)
#define TSN_FS_CLOSE		TSN(2, 2)
#define TSN_FS_READ		TSN(2, 3)
#define TSN_FS_WRITE		TSN(2, 4)
#define TSN_FS_LSEEK		TSN(2, 5)
#define TSN_FS_STAT		TSN(2, 6)
#define TSN_FS_FSTAT		TSN(2, 7)
#define TSN_FS_FTRUNCATE	TSN(2, 8)
#define TSN_FS_TRUNCATE		TSN(2, 9)
#define TSN_FS_GETDENTS		TSN(2, 10)
#define TSN_FS_MKDIR		TSN(2, 11)
#define TSN_FS_RMDIR		TSN(2, 12)
#define TSN_FS_UNLINK		TSN(2, 13)
#define TSN_FS_RENAME		TSN(2, 14)
#define TSN_FS_STATVFS		TSN(2, 15)
#define TSN_FS_SYNC		TSN(2, 16)
#define TSN_FS_ATTACH_DEV	TSN(2, 17)	/* by a disk's device object key, at FS_MEDIA_DIR */
#define TSN_FS_DETACH_DEV	TSN(2, 18)
#define TSN_FS_MOUNTS		TSN(2, 19)
#define TSN_FS_UTIME		TSN(2, 20)
#define TSN_FS_DUP		TSN(2, 21)	/* the POSIX layer's (design 5.8) */
#define TSN_FS_PIPE		TSN(2, 22)
#define TSN_FS_XSTAT		TSN(2, 23)
#define TSN_FS_FXSTAT		TSN(2, 24)
#define TSN_FS_IOCTL		TSN(2, 25)
#define TSN_FS_FCNTL		TSN(2, 26)
#define TSN_FS_POLL		TSN(2, 27)
#define TSN_FS_LINK		TSN(2, 28)
#define TSN_FS_CHMOD		TSN(2, 29)
#define TSN_FS_OPN_VOL		TSN(2, 30)	/* a disk's root by its device object key, no path (7.2) */
#define TSN_FS_OPENAT		TSN(2, 31)
#define TSN_FS_STATAT		TSN(2, 32)
#define TSN_FS_MKDIRAT		TSN(2, 33)
#define TSN_FS_RMDIRAT		TSN(2, 34)
#define TSN_FS_UNLINKAT		TSN(2, 35)
#define TSN_FS_UTIMEAT		TSN(2, 36)
#define TSN_FS_RENAMEAT		TSN(2, 37)
#define TSN_FS_FSTATVFS		TSN(2, 38)
#define TSN_FS_FSYNC		TSN(2, 39)
#define TSN_FS_MAX		39

/*
 * so_ (class 5): the sockets of a process, which are its own and are
 * closed when it ends; and the interface, set by administrators only.
 */
#define TSN_SO_SOCKET		TSN(5, 1)
#define TSN_SO_CLOSE		TSN(5, 2)
#define TSN_SO_BIND		TSN(5, 3)
#define TSN_SO_CONNECT		TSN(5, 4)
#define TSN_SO_LISTEN		TSN(5, 5)
#define TSN_SO_ACCEPT		TSN(5, 6)
#define TSN_SO_SHUTDOWN		TSN(5, 7)
#define TSN_SO_SEND		TSN(5, 8)
#define TSN_SO_RECV		TSN(5, 9)
#define TSN_SO_SENDTO		TSN(5, 10)
#define TSN_SO_RECVFROM		TSN(5, 11)
#define TSN_SO_GETSOCKOPT	TSN(5, 12)
#define TSN_SO_SETSOCKOPT	TSN(5, 13)
#define TSN_SO_GETSOCKNAME	TSN(5, 14)
#define TSN_SO_GETPEERNAME	TSN(5, 15)
#define TSN_SO_POLL		TSN(5, 16)
#define TSN_SO_FCNTL		TSN(5, 17)
#define TSN_SO_RESOLVE		TSN(5, 18)
#define TSN_SO_GETIFADDR	TSN(5, 19)
#define TSN_SO_GETDNS		TSN(5, 20)
#define TSN_SO_SETIFADDR	TSN(5, 21)	/* TSN_RIGHT_ADMIN */
#define TSN_SO_DHCP_START	TSN(5, 22)	/* TSN_RIGHT_ADMIN */
#define TSN_SO_GETOBJ		TSN(5, 23)
#define TSN_SO_RESOLVEALL	TSN(5, 24)
#define TSN_SO_RECVTOS		TSN(5, 25)
#define TSN_SO_MAX		25

/* class 0: tk_ */
#define TSN_TK_CRE_TSK		TSN(0, 1)
#define TSN_TK_DEL_TSK		TSN(0, 2)
#define TSN_TK_STA_TSK		TSN(0, 3)
#define TSN_TK_EXT_TSK		TSN(0, 4)
#define TSN_TK_EXD_TSK		TSN(0, 5)
#define TSN_TK_TER_TSK		TSN(0, 6)
#define TSN_TK_DIS_DSP		TSN(0, 7)
#define TSN_TK_ENA_DSP		TSN(0, 8)
#define TSN_TK_CHG_PRI		TSN(0, 9)
#define TSN_TK_ROT_RDQ		TSN(0, 10)
#define TSN_TK_REL_WAI		TSN(0, 11)
#define TSN_TK_GET_TID		TSN(0, 12)
#define TSN_TK_REF_TSK		TSN(0, 13)
#define TSN_TK_SUS_TSK		TSN(0, 14)
#define TSN_TK_RSM_TSK		TSN(0, 15)
#define TSN_TK_FRSM_TSK		TSN(0, 16)
#define TSN_TK_SLP_TSK		TSN(0, 17)
#define TSN_TK_WUP_TSK		TSN(0, 18)
#define TSN_TK_CAN_WUP		TSN(0, 19)
#define TSN_TK_CRE_SEM		TSN(0, 20)
#define TSN_TK_DEL_SEM		TSN(0, 21)
#define TSN_TK_SIG_SEM		TSN(0, 22)
#define TSN_TK_WAI_SEM		TSN(0, 23)
#define TSN_TK_REF_SEM		TSN(0, 24)
#define TSN_TK_CRE_FLG		TSN(0, 25)
#define TSN_TK_DEL_FLG		TSN(0, 26)
#define TSN_TK_SET_FLG		TSN(0, 27)
#define TSN_TK_CLR_FLG		TSN(0, 28)
#define TSN_TK_WAI_FLG		TSN(0, 29)
#define TSN_TK_REF_FLG		TSN(0, 30)
#define TSN_TK_CRE_MBX		TSN(0, 31)
#define TSN_TK_DEL_MBX		TSN(0, 32)
#define TSN_TK_SND_MBX		TSN(0, 33)
#define TSN_TK_RCV_MBX		TSN(0, 34)
#define TSN_TK_REF_MBX		TSN(0, 35)
#define TSN_TK_CRE_MTX		TSN(0, 36)
#define TSN_TK_DEL_MTX		TSN(0, 37)
#define TSN_TK_LOC_MTX		TSN(0, 38)
#define TSN_TK_UNL_MTX		TSN(0, 39)
#define TSN_TK_REF_MTX		TSN(0, 40)
#define TSN_TK_CRE_MBF		TSN(0, 41)
#define TSN_TK_DEL_MBF		TSN(0, 42)
#define TSN_TK_SND_MBF		TSN(0, 43)
#define TSN_TK_RCV_MBF		TSN(0, 44)
#define TSN_TK_REF_MBF		TSN(0, 45)
#define TSN_TK_DLY_TSK		TSN(0, 46)
#define TSN_TK_GET_TIM		TSN(0, 47)
#define TSN_TK_SET_TIM		TSN(0, 48)	/* TSN_RIGHT_CLOCK */
#define TSN_TK_GET_OTM		TSN(0, 49)
#define TSN_TK_CRE_CYC		TSN(0, 50)
#define TSN_TK_DEL_CYC		TSN(0, 51)
#define TSN_TK_STA_CYC		TSN(0, 52)
#define TSN_TK_STP_CYC		TSN(0, 53)
#define TSN_TK_REF_CYC		TSN(0, 54)
#define TSN_TK_CRE_ALM		TSN(0, 55)
#define TSN_TK_DEL_ALM		TSN(0, 56)
#define TSN_TK_STA_ALM		TSN(0, 57)
#define TSN_TK_STP_ALM		TSN(0, 58)
#define TSN_TK_REF_ALM		TSN(0, 59)
#define TSN_TK_SLP_TSK_U	TSN(0, 60)
#define TSN_TK_DLY_TSK_U	TSN(0, 61)
#define TSN_TK_WAI_SEM_U	TSN(0, 62)
#define TSN_TK_WAI_FLG_U	TSN(0, 63)
#define TSN_TK_RCV_MBX_U	TSN(0, 64)
#define TSN_TK_LOC_MTX_U	TSN(0, 65)
#define TSN_TK_SND_MBF_U	TSN(0, 66)
#define TSN_TK_RCV_MBF_U	TSN(0, 67)
#define TSN_TK_CRE_CYC_U	TSN(0, 68)
#define TSN_TK_REF_CYC_U	TSN(0, 69)
#define TSN_TK_STA_ALM_U	TSN(0, 70)
#define TSN_TK_REF_ALM_U	TSN(0, 71)
#define TSN_TK_SET_TIM_U	TSN(0, 72)	/* TSN_RIGHT_CLOCK */
#define TSN_TK_GET_TIM_U	TSN(0, 73)
#define TSN_TK_GET_OTM_U	TSN(0, 74)
#define TSN_TK_SET_UTC		TSN(0, 75)	/* TSN_RIGHT_CLOCK */
#define TSN_TK_GET_UTC		TSN(0, 76)
#define TSN_TK_GET_PRC		TSN(0, 77)
#define TSN_TK_CHG_SLT		TSN(0, 78)
#define TSN_TK_SET_ASP		TSN(0, 79)	/* the processors a task may run on, of the caller's size */
#define TSN_TK_REF_ASP		TSN(0, 80)
#define TSN_TK_MAX		80

/* class 1: TessronOS process, memory and IPC */
#define TSN_TS_GET_MONO		TSN(1, 1)
#define TSN_TS_CRE_PRC		TSN(1, 2)
#define TSN_TS_EXT_PRC		TSN(1, 3)
#define TSN_TS_TER_PRC		TSN(1, 4)
#define TSN_TS_WAI_PRC		TSN(1, 5)
#define TSN_TS_REF_PRC		TSN(1, 6)
#define TSN_TS_GET_PID		TSN(1, 7)
#define TSN_TS_GEN_UUID		TSN(1, 8)
#define TSN_TS_GET_RANDOM	TSN(1, 9)	/* kept for programs built before: the library reads 乱数 */
#define TSN_TS_SND_MSG		TSN(1, 10)
#define TSN_TS_RCV_MSG		TSN(1, 11)
#define TSN_TS_GET_ARG		TSN(1, 12)
#define TSN_TS_MAP_MEM		TSN(1, 13)	/* memory a process asks for (include/ts/umem.h) */
#define TSN_TS_UNM_MEM		TSN(1, 14)
#define TSN_TS_CTL_MEM		TSN(1, 15)
#define TSN_TS_REF_CPU		TSN(1, 16)	/* the processors and their load (include/ts/time.h) */
#define TSN_TS_DEF_FLT		TSN(1, 17)	/* a fault handed back to the program (include/ts/proc.h) */
#define TSN_TS_FLT_RET		TSN(1, 18)
#define TSN_TS_SPN_PRC		TSN(1, 19)	/* a POSIX process (include/ts/psx.h, design 5.8) */
#define TSN_TS_GET_INH		TSN(1, 20)
#define TSN_TS_WAI_PSX		TSN(1, 21)
#define TSN_TS_KIL_PRC		TSN(1, 22)
#define TSN_TS_SIG_SET		TSN(1, 23)
#define TSN_TS_SIG_RET		TSN(1, 24)
#define TSN_TS_SIG_SUS		TSN(1, 25)
#define TSN_TS_SLP_PSX		TSN(1, 26)
#define TSN_TS_SIG_ALM		TSN(1, 27)
#define TSN_TS_SET_PGID		TSN(1, 28)
#define TSN_TS_GET_PGID		TSN(1, 29)
#define TSN_TS_SET_SID		TSN(1, 30)
#define TSN_TS_GET_SID		TSN(1, 31)
#define TSN_TS_GET_PPID		TSN(1, 32)
#define TSN_TS_REF_PSX		TSN(1, 33)
#define TSN_TS_LST_PRC		TSN(1, 34)
#define TSN_TS_SPN_OBJ		TSN(1, 35)	/* a POSIX process of a program object (include/ts/psx.h) */
#define TSN_TS_LKP_OBJ		TSN(1, 36)	/* the object a path of the object world names */
#define TSN_TS_MAX		36

/* class 3: calendar */
#define TSN_DT_GETTIME		TSN(3, 1)
#define TSN_DT_SETTIME		TSN(3, 2)	/* TSN_RIGHT_CLOCK */
#define TSN_DT_GMTIME		TSN(3, 3)
#define TSN_DT_LOCALTIME	TSN(3, 4)
#define TSN_DT_MKTIME		TSN(3, 5)
#define TSN_DT_GETSYSTZ		TSN(3, 6)
#define TSN_DT_SETSYSTZ		TSN(3, 7)	/* TSN_RIGHT_ZONE */
#define TSN_DT_GETSYSZONE	TSN(3, 8)
#define TSN_DT_SETSYSZONE	TSN(3, 9)	/* TSN_RIGHT_ZONE */
#ifdef __x86_64__
#define TSN_DT_GETRTC		TSN(3, 10)	/* how the PC's RTC is kept */
#define TSN_DT_SETRTC		TSN(3, 11)	/* TSN_RIGHT_ZONE */
#define TSN_DT_MAX		11
#else
#define TSN_DT_MAX		9
#endif

/* class 7: ob_ */
#define TSN_OB_OPN_OBJ	TSN(7, 1)
#define TSN_OB_CLS_OBJ	TSN(7, 2)
#define TSN_OB_CRE_OBJ	TSN(7, 3)
#define TSN_OB_DEL_OBJ	TSN(7, 4)
#define TSN_OB_REF_OBJ	TSN(7, 5)
#define TSN_OB_LST_OBJ	TSN(7, 6)
#define TSN_OB_LNK_OBJ	TSN(7, 7)
#define TSN_OB_UNL_OBJ	TSN(7, 8)
#define TSN_OB_REA_REC	TSN(7, 9)
#define TSN_OB_WRI_REC	TSN(7, 10)
#define TSN_OB_APD_REC	TSN(7, 11)
#define TSN_OB_TRN_REC	TSN(7, 12)
#define TSN_OB_DEL_REC	TSN(7, 13)
#define TSN_OB_LST_REC	TSN(7, 14)
#define TSN_OB_GET_ATR	TSN(7, 15)
#define TSN_OB_SET_ATR	TSN(7, 16)
#define TSN_OB_GET_PRT	TSN(7, 17)
#define TSN_OB_SET_PRT	TSN(7, 18)
#define TSN_OB_DUP_KEY	TSN(7, 19)
#define TSN_OB_FND_NAM	TSN(7, 20)
#define TSN_OB_GET_CRD	TSN(7, 21)
#define TSN_OB_LOGIN		TSN(7, 22)
#define TSN_OB_SET_PWD	TSN(7, 23)
#define TSN_OB_ATT_VOL	TSN(7, 24)	/* TSN_RIGHT_ADMIN */
#define TSN_OB_DET_VOL	TSN(7, 25)	/* TSN_RIGHT_ADMIN */
#define TSN_OB_REA_RES	TSN(7, 26)
#define TSN_OB_WRI_RES	TSN(7, 27)
#define TSN_OB_DEL_RES	TSN(7, 28)
#define TSN_OB_LST_RES	TSN(7, 29)
#define TSN_OB_BEG_TRX	TSN(7, 30)
#define TSN_OB_END_TRX	TSN(7, 31)
#define TSN_OB_SCH_REC	TSN(7, 32)
#define TSN_OB_TRS_REC	TSN(7, 33)
#define TSN_OB_CPY_OBJ	TSN(7, 34)
#define TSN_OB_NTF_EVT	TSN(7, 35)
#define TSN_OB_CAN_EVT	TSN(7, 36)
#define TSN_OB_MAP_REC	TSN(7, 37)
#define TSN_OB_UNM_REC	TSN(7, 38)
#define TSN_OB_GET_DOM	TSN(7, 39)
#define TSN_OB_SET_DOM	TSN(7, 40)	/* TSN_RIGHT_ADMIN */
#define TSN_OB_GET_ICO	TSN(7, 41)
#define TSN_OB_SET_ICO	TSN(7, 42)
#define TSN_OB_REF_VOL	TSN(7, 43)
#define TSN_OB_FND_LNK	TSN(7, 44)
#define TSN_OB_LST_LNK	TSN(7, 45)
#define TSN_OB_END_ADM	TSN(7, 46)	/* the administrator's rights given back */
#define TSN_OB_WIP_VOL	TSN(7, 47)
#define TSN_OB_GET_ATV	TSN(7, 48)
#define TSN_OB_RPL_REC	TSN(7, 49)
#define TSN_OB_SYN_OBJ	TSN(7, 50)
#define TSN_OB_REA_TMO	TSN(7, 51)	/* ob_rea_rec waited for up to a time */
#define TSN_OB_FND_SYS	TSN(7, 52)	/* a system object, by the relationship of the root's link */
#define TSN_OB_ACC_OBJ	TSN(7, 53)	/* whether the caller may do the operations, without opening */
#define TSN_OB_MAX		53

/* class 6: tm_ */
#define TSN_TM_PUTSTRING	TSN(6, 1)
#define TSN_TM_PUTCHAR		TSN(6, 2)
#define TSN_TM_LOG_READ		TSN(6, 3)
#define TSN_TM_MAX		3

/*
 * A kernel built for its tests only (USE_KTEST): a word of the
 * process's at x0 read by the kernel, with the way to it opened after
 * the kernel has made sure of it (x1 = 1) or straight (x1 = 0), which
 * PAN refuses (docs/tessronos/04-kernel.md 4.6.1). Answers E_RSFN in
 * any other kernel.
 */
#define TSN_TM_PAN_PROBE	TSN(6, 0x7ff0)

#endif /* __TS_SVC_H__ */
