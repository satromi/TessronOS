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
 *	for each device paired or found, one after the other, each as long as
 *	its name makes it (BT_LINE_NEXT); record OB_BT_CMD takes a T_BTCMD.
 */

#ifndef __TS_BT_H__
#define __TS_BT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define BT_NAME_MAX	248		/* a device's name: Core 5.4 Vol 4 Part E 7.3.12, Vol 3 Part C 8.1.2 */

#define BT_DEV_UUID	"01a0d8c4-5502-7a01-8b12-3c4d5e6f7b80"
#define OB_BT_LIST	1
#define OB_BT_CMD	2
#define OB_BT_STATE	3		/* T_BTSTATE: what the adapter is doing */
#define OB_BT_NREC	4

#define BT_C_SCAN	1		/* a search for some seconds */
#define BT_C_PAIR	2		/* the device connected, paired and kept */
#define BT_C_FORGET	3		/* disconnected, and its object deleted */

/* What a device's connection is doing (T_BTLINE.use) */
#define BT_USE_NONE	0		/* not connected */
#define BT_USE_SETUP	1		/* connected, its services still being found or set up */
#define BT_USE_INPUT	2		/* its keys or pointer taken */
#define BT_USE_SOUND	3		/* a sound output */

/* What became of a try with a device found (T_BTLINE.note) */
#define BT_NOTE_NONE	0
#define BT_NOTE_LEA	1		/* LE Audio, which this adapter cannot carry, and no BR/EDR side known */
#define BT_NOTE_NOUSE	2		/* nothing it serves is used here: let go */
#define BT_NOTE_BREDR	3		/* its BR/EDR side paired in its place */

/* Where a device was found (T_BTLINE.bearer) */
#define BT_BEARER_LE	0x01
#define BT_BEARER_BREDR	0x02

/*
 * A line of the list: its fields, then its name, namelen bytes of UTF-8
 * and a 0. size is the whole line's bytes, to a four-byte boundary; the
 * next line begins there. The record is the lines one after the other.
 */
typedef struct {
	UH	size;			/* BT_LINE_SIZE(namelen) */
	UH	namelen;		/* 0 when it has told none */
	UB	addr[6];
	UB	paired;
	UB	connected;		/* in use: BT_USE_INPUT or BT_USE_SOUND */
	UW	cod;
	UB	bearer;			/* BT_BEARER_*, 0 for a paired device not seen in this search */
	UB	use;			/* BT_USE_* */
	UB	note;			/* BT_NOTE_* */
	UB	pad;
	UB	name[4];		/* as long as namelen and its 0 */
} T_BTLINE;
#define BT_LINE_HEAD		20	/* the bytes before name */
#define BT_LINE_SIZE(nlen)	( ( BT_LINE_HEAD + (nlen) + 1 + 3 ) & ~3 )
#define BT_LINE_NEXT(l)		( (T_BTLINE *)( (UB *)(l) + (l)->size ) )

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
	UB	name[BT_NAME_MAX + 1];	/* when it told: UTF-8 and a 0; empty when not */
} T_BTFOUND;

IMPORT BOOL knl_bt_ready( void );
IMPORT ER   knl_bt_scan( INT seconds );		/* found devices are listed by knl_bt_found as they come */
IMPORT INT  knl_bt_found( T_BTFOUND *buf, INT max );	/* up to max into buf; how many there are */
IMPORT INT  knl_bt_lines( UB *buf, INT size );	/* what OB_BT_LIST reads: the whole lines size takes; the bytes of all */
IMPORT ER   knl_bt_pair( CONST UB *addr );	/* connected, paired and kept, its keys or buttons taken */

#ifdef __cplusplus
}
#endif

#endif /* __TS_BT_H__ */
