/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt.h
 *	Bluetooth (design 6.8, device/bt/bt.c). A program reaches it through
 *	its device object, BT_DEV_UUID: record OB_BT_LIST reads T_BTLINE, one
 *	for each device paired or found, record OB_BT_CMD takes a T_BTCMD.
 */

#ifndef __TS_BT_H__
#define __TS_BT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define BT_FOUND_MAX	16

#define BT_DEV_UUID	"01a0d8c4-5502-7a01-8b12-3c4d5e6f7b80"
#define OB_BT_LIST	1
#define OB_BT_CMD	2
#define OB_BT_STATE	3		/* T_BTSTATE: what the adapter is doing */
#define OB_BT_NREC	4

#define BT_C_SCAN	1		/* a search for some seconds */
#define BT_C_PAIR	2		/* the device connected, paired and kept */
#define BT_C_FORGET	3		/* disconnected, and its object deleted */

typedef struct {
	UB	addr[6];
	UB	paired, connected;
	UW	cod;
	UB	name[32];
} T_BTLINE;

#define BT_ST_STARTING	0		/* the commands of the start are going */
#define BT_ST_READY	1
#define BT_ST_NOFIRMWARE 2		/* in its boot loader, with no firmware for it here */
#define BT_ST_FWFAILED	4		/* in its boot loader, and the firmware did not load */
#define BT_ST_NOANSWER	3		/* a command of the start got no answer, or failed */

typedef struct {
	UW	state;			/* BT_ST_* */
	UH	vid, pid;		/* the USB adapter */
	UH	op;			/* the command it stopped at, for BT_ST_NOANSWER */
	UH	status;			/* what that command answered, 0xffff nothing */
	UB	file[24];		/* the firmware's file, for BT_ST_NOFIRMWARE and BT_ST_FWFAILED */
	UB	scanning;		/* a search is going: its findings still come */
	UB	pad[3];
} T_BTSTATE;

typedef struct {
	UW	op;			/* BT_C_* */
	UB	addr[6];
	UB	pad[2];
} T_BTCMD;

/* A device a scan found */
typedef struct {
	UB	addr[6];		/* the lowest byte first, as HCI has it */
	UB	le;			/* found on LE, which has no Class of Device */
	UB	atype;			/* and its address's type */
	UW	cod;			/* its Class of Device */
	UB	name[32];		/* when it told: UTF-8; empty when not */
} T_BTFOUND;

IMPORT BOOL knl_bt_ready( void );
IMPORT ER   knl_bt_scan( INT seconds );		/* found devices are listed by knl_bt_found as they come */
IMPORT INT  knl_bt_found( T_BTFOUND *buf, INT max );
IMPORT ER   knl_bt_pair( CONST UB *addr );	/* connected, paired and kept, its keys or buttons taken */

#ifdef __cplusplus
}
#endif

#endif /* __TS_BT_H__ */
