/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ob.h
 *	Real objects of every kind and their basic operations (design 18)
 *
 *	Storage (files and volatile memory), devices, processes, channels
 *	and windows with their parts are all real objects: named by a UUID, opened into a key,
 *	read and written record by record, protected the same way. Which
 *	part of the peripheral kernel serves an object -- its manager -- is
 *	decided by the object's type and subtype; the name manager finds it
 *	and hands the call on, so a new kind of object is a new manager and
 *	not new system calls.
 *
 *	A process reaches these through the SVC gateway (class 7); code in
 *	the kernel calls them directly.
 */

#ifndef __TS_OB_H__
#define __TS_OB_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

/* ---------------------------------------------------------------- kinds */

/* Types */
#define OB_T_STORAGE	1		/* records kept somewhere */
#define OB_T_DEVICE	2
#define OB_T_PROCESS	3
#define OB_T_CHANNEL	4		/* messages between processes */
#define OB_T_WINDOW	5		/* a window, a panel, a menu or a part */

/* Subtypes */
#define OB_S_FILE	1		/* storage on a volume (TSFS) */
#define OB_S_MEMORY	2		/* storage in memory: shared memory, global names */
#define OB_S_DISK	1		/* device: blocks */
#define OB_S_CHAR	2		/* device: a stream */
#define OB_S_CLOCK	3		/* device: the clock */
#define OB_S_SOUND	4		/* device: sound, playing and recording */
#define OB_S_DEVLIST	5		/* device: the list of the devices */
#define OB_S_SYSTEM	6		/* device: the system itself */
#define OB_S_INPUT	7		/* device: a keyboard or a pointer, or the input they share */
#define OB_S_DISPLAY	8		/* device: a screen */
#define OB_S_GPIO	9		/* device: a bank of general purpose pins */
#define OB_S_USB	10		/* device: a device on a USB bus, hubs included */
#define OB_S_QUEUE	1		/* channel: a queue of messages */
#define OB_S_SOCKET	2		/* channel: a socket of the network */
#define OB_S_WINDOW	1		/* window: drawn in freely */
#define OB_S_PANEL	2		/* window: holds parts */
#define OB_S_MENU	3		/* window: a column of items */
#define OB_S_PART	4		/* window: a switch, a box, a selector */

/* What kind of object it is (T_OBREF.flags, T_OBCRE.flags) */
#define OB_F_VOLATILE	0x0001		/* gone when the system stops */
#define OB_F_VIRTUAL	0x0002		/* a device the manager makes itself */
#define OB_F_GLOBAL	0x0004		/* found by its name (ob_fnd_nam) */
#define OB_F_AUTOREF	0x0008		/* its store counts the links of its records */

/* ---------------------------------------------------------------- operations */

/*
 * What may be done to an object, one bit per basic operation. The
 * protection shows them as rwx; r, w and x stand for the groups below.
 */
#define OB_OP_READ	0x0001		/* read a data record */
#define OB_OP_ATRRD	0x0002		/* read the attributes, list the records */
#define OB_OP_WRITE	0x0004		/* write a data record, cut it short */
#define OB_OP_RECORD	0x0008		/* add or delete a record */
#define OB_OP_ATRWR	0x0010		/* write the attributes */
#define OB_OP_LINK	0x0020		/* count a virtual object in or out */
#define OB_OP_DELETE	0x0040		/* delete the object, end the process */
#define OB_OP_EXEC	0x0080		/* run the program, control the device */
#define OB_OP_PROT	0x0100		/* change the protection */

#define OB_OP_R		( OB_OP_READ | OB_OP_ATRRD )
#define OB_OP_W		( OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR \
			| OB_OP_LINK | OB_OP_DELETE )
#define OB_OP_X		OB_OP_EXEC
#define OB_OP_ALL	0x01FF

/* ob_opn_obj: do not wait (a channel that is full or empty) */
#define OB_O_NOWAIT	0x80000000

/*
 * The records of a sound device (design 18.7): record 0 says what it
 * is, record 1 is the PCM -- written, it plays; read, it gives what was
 * recorded -- and the rest are its attributes, each read or written
 * whole in the structures of include/ts/snd.h. Every opening is a
 * channel of its own, so several programs play at once.
 */
#define OB_SND_PCM	1		/* data number 0 */
#define OB_SND_INFO	2		/* SDPcmInfo, what each rate supports */
#define OB_SND_MODE	3		/* SDPcmMode */
#define OB_SND_CTL	4		/* SDPcmCtl: start, stop, record */
#define OB_SND_CNT	5		/* bytes processed since the start */
#define OB_SND_ENDCNT	6		/* stop after this many bytes */
#define OB_SND_BUFSZ	7		/* the PCM buffer's size */
#define OB_SND_STAT	8		/* SDStat */
#define OB_SND_SELIN	9		/* SDSel: the input */
#define OB_SND_SELOUT	10		/* SDSel: the output */
#define OB_SND_VOL	11		/* SDVol, the first of the levels: the rest follow
					   in the order of the data numbers, gain in and out,
					   main, treble, bass, PCM, beep, mic, line, CD,
					   music, aux 1, aux 2 */
#define OB_SND_NREC	24

/*
 * The records of the system object (ob_uuid_system, design 18.7): each
 * is "key value" lines of text (include/ts/conf.h), read to learn and,
 * where it may be, written to change.
 */
#define OB_SYS_INFO	1		/* the system, its processors and memory */
#define OB_SYS_POWER	2		/* write OFF or RESTART */
#define OB_SYS_DISPLAY	3		/* the screen */
#define OB_SYS_USB	4		/* the devices on the USB buses */
#define OB_SYS_SCHEME	5		/* the colour scheme; write its number */
#define OB_SYS_WALL	6		/* the wallpaper; write its object's UUID */
#define OB_SYS_NET	7		/* the network card and interface */
#define OB_SYS_SOUND	8		/* what plays sound */
#define OB_SYS_HW	9		/* the state of the hardware: temperature and clocks */
#define OB_SYS_NREC	10

/*
 * The records of an input device (OB_S_INPUT, design 18.7): each
 * keyboard and pointer on the bus, and 入力, the input they all feed,
 * which is there without any of them. Reading or writing the events
 * needs a key that may also control the device (OB_OP_EXEC, through
 * the record's own limit): what a person types is not everyone's to
 * read, nor to make up.
 *
 * Reading OB_IN_EVENTS gives, as T_HIDEV (include/ts/hid.h), what the
 * device reported since this key last read -- on 入力, every device's
 * and what was put in -- as a copy: the window manager still receives
 * all of it. Writing T_HIDEV puts them in as if the device had
 * reported them (how a test or a remote input drives the layers above).
 */
#define OB_IN_EVENTS	1
#define OB_IN_STATE	2		/* text: the kind, where the pointer is, the counts */
#define OB_IN_ATTR	3		/* 入力 only: T_HIDATTR, how the input is taken */
#define OB_IN_NREC	3		/* a device's records; 入力 has one more */

/*
 * The records of the screen (OB_S_DISPLAY): record 1 is "key value"
 * text -- SIZE, DEPTH, MODES, DRIVER -- and written "WIDTHxHEIGHT" it
 * sets the screen to that size where the hardware can (E_NOSPT where
 * it cannot). Record 2 is the pixels, rows of four bytes a pixel
 * (XRGB8888), read only, and only with a key that may also control the
 * device (OB_OP_EXEC).
 */
#define OB_DSP_MODE	1
#define OB_DSP_PIXELS	2
#define OB_DSP_NREC	3

/*
 * The record of a bank of general purpose pins (OB_S_GPIO): a line
 * "pin direction value pull" for each pin -- direction in, out, or
 * alt<n> for a pin given to one of the chip's functions; pull up, down,
 * none, or - where the bank cannot say. Writing such lines sets the
 * pins, a field of - leaving that part as it is. A pin a driver of the
 * kernel uses has "kernel" after it and is not set (E_BUSY). A change
 * of an input pin is told as OB_E_CHANGE on the record, the pin in the
 * notice's code and its value in x.
 */
#define OB_GPIO_PINS	1
#define OB_GPIO_NREC	2

/* The record of a device on the USB bus (OB_S_USB): "key value" text of what it is */
#define OB_USB_INFO	1
#define OB_USB_NREC	2

/* The random source (乱数, OB_S_CHAR): read, random bytes; written, stirred in */
#define OB_RND_DATA	1
#define OB_RND_NREC	2

/* A channel: writing record 0 sends one message, reading it takes one */
#define OB_CH_MSG_MAX	1024		/* bytes of one message */
#define OB_CH_QLEN	16		/* messages it holds */

/*
 * A socket (OB_S_SOCKET, design 18.8): there from so_socket or so_accept
 * until so_close or the end of the process that made it, and owned by
 * that process's user. Record 0 says what it is, as xmlTAD, with a link
 * to the process. Record 1 is the data, a stream as a channel's record
 * 0 is a queue: reading it receives, writing it sends, and each waits
 * as the channel does unless the key was opened with OB_O_NOWAIT
 * (E_TMOUT: nothing there, or no room). Its size is what has come in
 * and not been read. The metadata's "socket" member tells its number,
 * process, family, type, protocol, the two ends and its state.
 * Deleting the object closes the socket.
 */
#define OB_SK_DATA	1
#define OB_SK_NREC	2

/* ---------------------------------------------------------------- records */

/* Record types: the BTRON numbers */
#define OB_RT_LINK	0
#define OB_RT_TAD	1		/* xmlTAD */
#define OB_RT_FFUSEN	7
#define OB_RT_MFUSEN	8
#define OB_RT_PROG	9		/* an executable */
#define OB_RT_DATABOX	10
#define OB_RT_FONT	11
#define OB_RT_DICT	12
#define OB_RT_SYSDATA	15		/* bytes with no type declared */

typedef struct {
	INT	recno;
	UINT	rt;			/* OB_RT_* */
	UINT	sub;
	UD	size;
} T_OBREC;

#define OB_REC_ANY	(-1)		/* ob_ntf_evt: every record */
#define OB_REC_END	(-1)		/* ob_trs_rec: added after the last */

/*
 * The records of a window (design 18.13). What follows the state
 * depends on the subtype: a switch's two looks, a menu's items (one a
 * record, the item's state in the record's subtype), a panel's links
 * to the parts placed on it.
 */
#define OB_WR_DRAW	0		/* xmlTAD: what the window shows */
#define OB_WR_SHAPE	1		/* xmlTAD figure: its outline */
#define OB_WR_PLACE	2		/* T_OBWPOS */
#define OB_WR_STATE	3		/* a part's value: an INT, or a box's text */
#define OB_WR_KIND	4		/* the first of the subtype's own */

#define OB_WR_OFF	4		/* a switch: how it looks released */
#define OB_WR_ON	5		/* and pressed */

/* A menu item's state, in its record's subtype */
#define OB_MI_GREY	0x0001		/* shown, but cannot be chosen */
#define OB_MI_TICK	0x0002		/* shown with a mark */
#define OB_MI_LINE	0x0004		/* a line between items */
#define OB_MI_SUB	0x0008		/* opens a menu beside it */

/* Where a window is: record OB_WR_PLACE */
#define OB_WP_SHOWN	0x0001
#define OB_WP_OFF	0x0002		/* a part: shown, but it cannot be worked */
typedef struct {
	TS_UUID	parent;			/* the window it is in; 0 the screen */
	INT	left, top, right, bottom;	/* its outer edge */
	INT	z;			/* 0 nearest the viewer */
	UINT	flags;			/* OB_WP_* */
	/* its work area, as the screen has it; read only: a write moves the outer edge */
	INT	wleft, wtop, wright, wbottom;
	INT	sw, sh;			/* the screen's size; read only */
} T_OBWPOS;

/*
 * Links dropped on a window (design 16.5.21, 18.14): record OB_WR_DROP
 * of a plain window, the latest drop, told with OB_E_DROP. Only a
 * window someone has asked OB_E_DROP of takes drops; the first process
 * that asked is the taker. The taker is granted, for each object
 * dropped, what the one who dropped it may do to it, but never to
 * delete it, run it or change its protection (OB_DROP_OPS): it opens the
 * object by its UUID with those rights whatever its own credentials
 * say, as long as it runs. What the objects link to further is opened
 * with the taker's own credentials.
 *
 * Read, the record is a T_OBDROP whose v[] holds n entries (its length
 * says so too). Written with a T_OBDRANS, it is answered: the one who
 * dropped is told whether it was taken, on the message line; a drop
 * refused takes its grants back.
 */
#define OB_WR_DROP	4		/* a plain window: the latest drop */
#define OB_WR_BARS	5		/* a plain window: its scroll bars, T_OBWBARS */
#define OB_DROP_MAX	16		/* links in one drop */
#define OB_DROP_NAME	64		/* bytes of a link's name, UTF-8 */
#define OB_DROP_MSG	128		/* bytes of what an answer says */
#define OB_DROP_OPS	( OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR | OB_OP_LINK )

typedef struct {
	TS_UUID	target;			/* the object the link points at */
	TS_UUID	vobjid;			/* the link itself */
	UINT	ops;			/* what the taker was granted: OB_OP_*, 0 nothing */
	UB	name[OB_DROP_NAME];	/* the name the link shows */
	INT	left, top, right, bottom;	/* its box where it was */
	UINT	frcol, chcol, tbcol, bgcol;	/* 0x00rrggbb */
	INT	chsz;
	UINT	disp;			/* what it shows: TAD_D_* */
} T_OBDROPV;

typedef struct {
	UINT	seq;			/* counts the drops on this window */
	INT	n;			/* entries of v[] */
	INT	x, y;			/* where it was let go, in the work area */
	UINT	mods;			/* the keys held then */
	TS_UUID	from;			/* the window it came from; 0 none said */
	TS_UUID	fromobj;		/* the object that window shows (the record the
					   links are in); 0 none said */
	T_OBDROPV v[OB_DROP_MAX];
} T_OBDROP;

#define OB_DR_ACCEPT	1
#define OB_DR_REFUSE	2
typedef struct {
	UINT	seq;			/* the drop answered */
	UINT	answer;			/* OB_DR_* */
	UB	msg[OB_DROP_MSG];	/* said on the message line; "" the usual words */
} T_OBDRANS;

/*
 * The scroll bars of a plain window: record OB_WR_BARS, one T_OBWBAR
 * for each side in OB_BAR_* order. A window has the bars its "attr"
 * asked for (WM_ATTR_RBAR, BBAR, LBAR); what its owner writes here is
 * what they show: everything there is (lo..hi) and the part of it the
 * work area shows (clo..chi), in whatever unit the owner chooses. A
 * write shorter than the whole sets the first bars only.
 *
 * The bars are worked by the user, not by the owner: a press on the
 * track before or after the knob, and the knob carried, are told with
 * OB_E_SCROLL, code the bar, x where its start (clo) is asked to go,
 * already kept inside lo..hi, y how (OB_SCR_*). The owner scrolls to x
 * and writes the record again; the knob itself moves while it is
 * carried.
 */
#define OB_BAR_R	0		/* down the right side */
#define OB_BAR_B	1		/* along the foot */
#define OB_BAR_L	2		/* down the left side */
#define OB_BAR_MAX	3

#define OB_SCR_PAGE	1		/* the track pressed: a page back or on */
#define OB_SCR_KNOB	2		/* the knob is being carried */
#define OB_SCR_DONE	3		/* and was let go */

typedef struct {
	INT	lo, hi;			/* everything there is */
	INT	clo, chi;		/* what is shown of it */
} T_OBWBAR;

typedef struct {
	T_OBWBAR bar[OB_BAR_MAX];
} T_OBWBARS;

/* ---------------------------------------------------------------- objects */

#define OB_NAME_MAX	64		/* bytes of a name, UTF-8 */

typedef struct {
	TS_UUID	uuid;
	UINT	type;			/* OB_T_* */
	UINT	sub;			/* OB_S_* */
	UINT	flags;			/* OB_F_* */
	UB	name[OB_NAME_MAX];
	INT	refcnt;
	INT	nrec;
	UD	size;			/* bytes of every record together */
} T_OBREF;

/* ---------------------------------------------------------------- protection */

#define OB_ACL_MAX	16
#define OB_RMASK_MAX	16

/* Attributes */
#define OB_A_RONLY	0x0001		/* no writing */
#define OB_A_PERM	0x0002		/* no deleting */

/* One entry of the access control list */
#define OB_ACE_USER	1
#define OB_ACE_GROUP	2
typedef struct {
	UINT	kind;			/* OB_ACE_* */
	TS_UUID	who;
	UINT	ops;			/* OB_OP_* given */
} T_OBACE;

/*
 * A record's own limit: only what it lets through is allowed on it.
 * A limit that names OB_OP_EXEC lets it through only to a key that may
 * also control the object (run it, control the device): a device's
 * record that shows or makes up what a person sees or does.
 */
typedef struct {
	INT	recno;
	UINT	ops;
} T_OBRMASK;

/*
 * The protection of an object. The mode is Unix's nine bits, 0400 the
 * owner's r down to 0001 the others' x. An owner of all zeroes is no
 * owner, as data written by the TADjs Desktop has.
 */
typedef struct {
	TS_UUID	owner;
	TS_UUID	group;
	UINT	mode;
	UINT	attr;			/* OB_A_* */
	INT	nacl;
	T_OBACE	acl[OB_ACL_MAX];
	INT	nrmask;
	T_OBRMASK rmask[OB_RMASK_MAX];
} T_OBPRT;

/* ---------------------------------------------------------------- credentials */

#ifndef CNF_OB_MAX_GROUPS
#define CNF_OB_MAX_GROUPS	64
#endif

/* Who a process acts as: a user and the groups it belongs to, the first of them its own */
typedef struct {
	TS_UUID	user;
	INT	ngrp;
	TS_UUID	grp[CNF_OB_MAX_GROUPS];
} T_OBCRD;

/* The fixed objects (design 18.4, 18.10) */
#define OB_USER_SYSTEM_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa0, 0x75, 0xb3, \
				    0xbd, 0x7d, 0x07, 0x5e, 0x14, 0x21, 0x7c, 0xd2 } }
#define OB_GROUP_ADMIN_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa1, 0x79, 0x22, \
				    0xae, 0x79, 0xba, 0x5c, 0xbb, 0xaa, 0x20, 0x37 } }
#define OB_UUID_CLOCK_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa2, 0x7e, 0x93, \
				    0x94, 0xde, 0xcc, 0xe6, 0x17, 0x97, 0xda, 0xd4 } }
#define OB_UUID_DEVBOX_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa3, 0x73, 0xe6, \
				    0x9a, 0x3e, 0xfe, 0x2e, 0x66, 0xff, 0x8f, 0xb1 } }
#define OB_UUID_DOMAIN_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa4, 0x70, 0x9d, \
				    0x92, 0x96, 0xd8, 0x36, 0x41, 0x59, 0x3c, 0x9c } }
#define OB_UUID_DEVLIST_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa5, 0x7b, 0x41, \
				    0x8c, 0x5e, 0x2d, 0x61, 0x9f, 0x07, 0x43, 0xa8 } }
#define OB_UUID_SYSTEM_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa6, 0x7c, 0x52, \
				    0x9d, 0x6f, 0x3e, 0x72, 0xa0, 0xb1, 0x8f, 0x14 } }

#define OB_UUID_RANDOM_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa7, 0x71, 0x3c, \
				    0x8e, 0x52, 0x6b, 0x0d, 0x4f, 0x93, 0x27, 0xe1 } }
#define OB_UUID_INPUT_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa8, 0x76, 0x1f, \
				    0xb4, 0x08, 0x5d, 0xc2, 0x39, 0x7a, 0xe6, 0x50 } }
#define OB_UUID_DISPLAY_INIT	{ { 0x01, 0xa0, 0xd6, 0x80, 0x3a, 0xa9, 0x7d, 0x84, \
				    0x93, 0x6e, 0x21, 0xfa, 0x0c, 0x57, 0xb8, 0x4d } }

IMPORT CONST TS_UUID	ob_user_system;		/* the system, and init */
IMPORT CONST TS_UUID	ob_group_admin;		/* administrators */
IMPORT CONST TS_UUID	ob_uuid_clock;		/* the clock device */
IMPORT CONST TS_UUID	ob_uuid_devlist;	/* the list of the devices */
IMPORT CONST TS_UUID	ob_uuid_system;		/* the system itself (obsys.c) */
IMPORT CONST TS_UUID	ob_uuid_devbox;		/* where device names meet UUIDs */
IMPORT CONST TS_UUID	ob_uuid_domain;		/* a volume's protection domain, on each volume */
IMPORT CONST TS_UUID	ob_uuid_random;		/* the random source, 乱数 */
IMPORT CONST TS_UUID	ob_uuid_input;		/* the input the keyboards and pointers feed, 入力 */
IMPORT CONST TS_UUID	ob_uuid_display;	/* the screen, 画面 */

/* ---------------------------------------------------------------- creation */

typedef struct {
	UINT	type;			/* OB_T_* */
	UINT	sub;			/* OB_S_* */
	UINT	flags;			/* OB_F_GLOBAL */
	CONST UB *name;			/* NULL for none */
	CONST UB *json;			/* storage: the metadata, NULL for a default */
	SZ	jsonsz;
	TS_UUID	near;			/* file: made on the volume of this object */
	CONST char *vol;		/* file: or on the volume attached with this path;
					   neither: the first volume */
	TS_UUID	prog;			/* process: the program to start */
	CONST void *arg;		/* process: the start-up argument */
	SZ	argsz;
	CONST T_OBPRT *prt;		/* NULL: the creator's default */
	TS_UUID	uuid;			/* file, memory: the identity to give it, for
					   an object the system knows by a fixed UUID
					   (an administrator only); 0: a new one */
	CONST void *icon;		/* storage: its icon (ob_set_ico), NULL for none */
	SZ	iconsz;
} T_OBCRE;

/* ---------------------------------------------------------------- events */

/*
 * What happened to an object, sent to another object -- the port -- as
 * one record (design 18.14). A channel takes it as one message; storage
 * adds it as a record at the end, which keeps a log of them.
 */
#define OB_E_PRESS	0x0001		/* a button went down */
#define OB_E_RELEASE	0x0002		/* and came up */
#define OB_E_MOVE	0x0004		/* the pointer moved */
#define OB_E_KEY	0x0008		/* a key */
#define OB_E_ENTER	0x0010		/* the pointer came in */
#define OB_E_LEAVE	0x0020		/* and went out */
#define OB_E_REDRAW	0x0040		/* a window drawn directly wants drawing again */
#define OB_E_CLOSE	0x0080		/* a window is asked to close */
#define OB_E_CHANGE	0x0100		/* a record or a part's value changed */
#define OB_E_DELETE	0x0200		/* the object is gone */
#define OB_E_ATTACH	0x0400		/* a medium came */
#define OB_E_DETACH	0x0800		/* and went */
#define OB_E_EXIT	0x1000		/* the process ended */
#define OB_E_DROP	0x2000		/* links were dropped on a window (OB_WR_DROP) */
#define OB_E_WHEEL	0x4000		/* a wheel turned over a window: code HID_WHEEL_*, dz */
#define OB_E_SCROLL	0x8000		/* a scroll bar was worked: code OB_BAR_*, x, y OB_SCR_* */
#define OB_E_ALL	0xFFFF

#define OB_E_INPUT	( OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY \
			| OB_E_ENTER | OB_E_LEAVE | OB_E_REDRAW | OB_E_CLOSE | OB_E_DROP \
			| OB_E_WHEEL | OB_E_SCROLL )

/* A request: which events, the number the caller knows them by */
#define OB_N_ONCE	0x0001		/* sent once, then the request goes */
typedef struct {
	UINT	events;			/* OB_E_* */
	UINT	id;			/* carried in every notice */
	UINT	flags;			/* OB_N_* */
} T_OBNTF;

/* A notice, as it arrives at the port */
typedef struct {
	UINT	id;			/* the requester's number */
	ID	nid;			/* the request */
	TS_UUID	uuid;			/* where it happened */
	INT	recno;			/* on which record, -1 the object */
	UINT	event;			/* one OB_E_* */
	UINT	code;			/* a key or a button */
	UINT	mods;
	INT	x, y;			/* in the window's work area */
	UINT	lost;			/* notices dropped before this one */
	UD	when;			/* ms since start */
	INT	dz;			/* a wheel: the notches it turned */
} T_OBNTM;

/* ---------------------------------------------------------------- calls */

IMPORT ID  ob_opn_obj( CONST TS_UUID *uuid, UINT ops );
IMPORT ER  ob_cls_obj( ID key );
IMPORT ER  ob_cre_obj( CONST T_OBCRE *pk_cre, TS_UUID *p_uuid );
IMPORT ER  ob_del_obj( CONST TS_UUID *uuid );
IMPORT ER  ob_ref_obj( CONST TS_UUID *uuid, T_OBREF *pk_ref );
IMPORT ER  ob_lst_obj( UINT type, UINT sub, CONST TS_UUID *from, TS_UUID *buf, INT n,
			INT *p_cnt );
IMPORT ER  ob_lnk_obj( CONST TS_UUID *uuid );
IMPORT ER  ob_unl_obj( CONST TS_UUID *uuid );

IMPORT ER  ob_rea_rec( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize );
IMPORT ER  ob_wri_rec( ID key, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize );
IMPORT ER  ob_apd_rec( ID key, UINT rt, UINT sub, INT *p_recno );
IMPORT ER  ob_trn_rec( ID key, INT recno, UD size );
IMPORT ER  ob_del_rec( ID key, INT recno );
IMPORT ER  ob_lst_rec( ID key, T_OBREC *buf, INT n, INT *p_cnt );

/* The first record from 'from' on of type rt whose subtype & mask is sub */
IMPORT ER  ob_sch_rec( ID key, INT from, UINT rt, UINT sub, UINT mask, INT *p_recno );

/* n records copied, type and all, from src's srec on to dst's drec on */
IMPORT ER  ob_trs_rec( ID dst, INT drec, ID src, INT srec, INT n );

/*
 * A new object with the records, resources and metadata of another.
 * pk_cre says where it goes and may change its kind (a part's
 * definition becoming a window); NULL, or a type of 0, keeps the kind.
 */
IMPORT ER  ob_cpy_obj( CONST TS_UUID *uuid, CONST T_OBCRE *pk_cre, TS_UUID *p_uuid );

/*
 * A record of one object mapped onto another (design 18.5).
 *
 * Onto a window: a part placed on a panel, a panel or a menu on a
 * window, at (x, y) in the target (design 18.13). Record 0 is the
 * object as a whole.
 *
 * Onto a process: a record of storage in memory mapped into the
 * process's space, in the shared memory window, at the same address in
 * every process it is mapped into (design 6.8) -- shared memory. The
 * target is the process's object, or 0 for the caller's own process.
 * The record then stays in the pages it was given, at least 'size'
 * bytes of them, and cannot grow past them. The address comes back in
 * 'addr'.
 *
 * Unmapping takes it off again; the object stays.
 */
#define OB_M_WRITE	0x0001		/* mapped writable: the key must allow writing */

typedef struct {
	INT	x, y;			/* a window: where, in the target */
	UINT	flags;			/* OB_M_* */
	SZ	size;			/* a process: room to keep, 0 the record's size */
	void	*addr;			/* a process: where it was mapped (out) */
} T_OBMAP;

IMPORT ER  ob_map_rec( ID key, INT recno, ID target, T_OBMAP *pk_map );
IMPORT ER  ob_unm_rec( ID key, INT recno, ID target );

/* Notices of what happens on a record of key's object, sent to port */
IMPORT ID  ob_ntf_evt( ID key, INT recno, CONST T_OBNTF *pk_ntf, ID port );
IMPORT ER  ob_can_evt( ID nid );

/*
 * Resources: bytes kept beside the records -- a picture, a sound, the
 * icon -- named by what follows the object's UUID in the file name a
 * TADjs Desktop gives them: "_0_1.png", "_0_bgm.mp3", ".ico". A record
 * or the metadata is not a resource. ob_lst_res gives the names one
 * after another, each ending in 0.
 */
#define OB_RES_NAME	64		/* bytes of a resource's name */

IMPORT ER  ob_rea_res( ID key, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize );
IMPORT ER  ob_wri_res( ID key, CONST UB *name, CONST void *buf, SZ size );
IMPORT ER  ob_del_res( ID key, CONST UB *name );
IMPORT ER  ob_lst_res( ID key, UB *buf, SZ size, INT *p_cnt );

/* The attributes: the metadata, JSON */
#define OB_ATR_MAX	16384		/* bytes of metadata an object may have */

IMPORT ER  ob_get_atr( ID key, UB *json, SZ size, SZ *p_asize );
IMPORT ER  ob_set_atr( ID key, CONST UB *json, SZ size );

/*
 * The icon (ピクトグラム): a part of the object beside its attributes,
 * read and written the same way and under the same rights -- reading it
 * needs OB_OP_ATRRD, writing it OB_OP_ATRWR -- and told of by the same
 * notice (OB_E_CHANGE on record -1). It is an ICO or a PNG, whole, of at
 * most OB_ICO_MAX bytes. ob_get_ico answers its size in *p_asize (size 0
 * with buf NULL asks only that) and E_NOEXS when there is none;
 * ob_set_ico with size 0 takes it away. It may also be given when the
 * object is made (T_OBCRE.icon), and a copy carries it. The resource
 * name ".ico" names the same thing.
 */
#define OB_ICO_MAX	65536

IMPORT ER  ob_get_ico( ID key, UB *buf, SZ size, SZ *p_asize );
IMPORT ER  ob_set_ico( ID key, CONST UB *buf, SZ size );

IMPORT ER  ob_get_prt( CONST TS_UUID *uuid, T_OBPRT *pk_prt );
IMPORT ER  ob_set_prt( CONST TS_UUID *uuid, CONST T_OBPRT *pk_prt );

/* A key with fewer operations, held by another process (0: the kernel) */
IMPORT ID  ob_dup_key( ID key, UINT ops, ID pid );

IMPORT ER  ob_fnd_nam( CONST UB *name, TS_UUID *p_uuid );

/*
 * The links of a box (a 書体箱, a 壁紙箱, the プログラム箱): the objects
 * the <link>s of its record 0 point at, in the order they are written,
 * one for every link. ob_lst_lnk gives those named 'name' -- every one
 * for NULL or "" -- as many as fit in buf (n 0 and buf NULL ask only
 * how many), and in *p_cnt how many there are; ob_fnd_lnk gives the
 * first of them, E_NOEXS when there is none. A link is named by its
 * object's name or by the file name its metadata keeps
 * (tessronos.file.name), without regard to case and with or without
 * the file name's extension. The box is read with the caller's rights
 * (OB_OP_READ); an object whose metadata the caller may not read is
 * named by its object name only.
 */
IMPORT ER  ob_fnd_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid );
IMPORT ER  ob_lst_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *buf, INT n, INT *p_cnt );

IMPORT ER  ob_get_crd( T_OBCRD *pk_crd );
IMPORT ER  ob_login( CONST TS_UUID *user, CONST UB *password );
IMPORT ER  ob_set_pwd( CONST TS_UUID *user, CONST UB *password );

/*
 * Several changes to objects on the volume 'on' is on, made real
 * together: a record written and the counts it changes. E_NOSPT on a
 * volume that keeps no log (the FAT store), where each change is made
 * as it comes.
 */
IMPORT ER  ob_beg_trx( CONST TS_UUID *on );
IMPORT ER  ob_end_trx( CONST TS_UUID *on, BOOL commit );

/*
 * The volume an object of the file manager is on (NULL, or a UUID of 0:
 * the first volume, where objects made with no place given go): how big
 * it is and how much of it is free, in blocks. Asking of an object needs
 * the right to read its attributes. E_NOSPT: the object is on no volume
 * (memory, a device, a window).
 */
typedef struct {
	UD	blocks;			/* all of it */
	UD	bfree;			/* not in use */
	UINT	bsize;			/* bytes of a block */
	INT	nobj;			/* objects on it */
} T_OBVOL;

IMPORT ER  ob_ref_vol( CONST TS_UUID *near, T_OBVOL *pk_vol );

/* Volumes of the file manager: a TSFS store (ts_opn_vol's path and flags) */
IMPORT ER  ob_att_vol( CONST char *path, UINT flags );
IMPORT ER  ob_det_vol( CONST char *path );

/*
 * The protection domain of a volume (design 18.9): protection that
 * every object on it is looked at through before its own. What the
 * domain allows is an upper bound on what the object's own protection
 * gives, except that the owner of an object may still change its
 * protection. A volume without one bounds nothing. Setting it is an
 * administrator's; NULL takes it away. It is kept on the volume, so it
 * goes where the medium goes.
 */
IMPORT ER  ob_get_dom( CONST char *vol, T_OBPRT *pk_prt );
IMPORT ER  ob_set_dom( CONST char *vol, CONST T_OBPRT *pk_prt );

/* ---------------------------------------------------------------- the kernel's */

IMPORT ER   knl_ob_init( void );
IMPORT void knl_ob_prc_start( ID pid, ID ppid );	/* a process was made */
IMPORT void knl_ob_prc_end( ID pid );		/* it is going: its keys close */
IMPORT void knl_ob_prc_gone( ID pid );		/* it is gone: its credentials go */
IMPORT UINT knl_ob_permit( CONST T_OBCRD *crd, CONST T_OBPRT *prt, INT recno );

/* Who wrote the message last read from a channel (0 the kernel), and whether a process is an administrator */
IMPORT ID   knl_obchan_last_from( CONST TS_UUID *uuid );
IMPORT BOOL knl_ob_pid_admin( ID pid );

/*
 * Something happened on an object: the notices asked for it are sent.
 * 'n' gives the details (code, place, when) and may be NULL. A manager
 * calls it holding none of its own locks, since a notice may go to an
 * object the same manager keeps.
 */
IMPORT void knl_ob_post( CONST TS_UUID *uuid, INT recno, UINT event, CONST T_OBNTM *n );

/*
 * The same, told to those who watch 'watched' -- an object that stands
 * for a whole class, such as the list of the devices -- with the notice
 * naming 'uuid', the object it happened on.
 */
IMPORT void knl_ob_post_as( CONST TS_UUID *watched, CONST TS_UUID *uuid, INT recno,
			    UINT event, CONST T_OBNTM *n );

/*
 * Whether anyone asked to be told 'event' on the object, and which
 * process asked first (0: the kernel): the one something handed to the
 * object with that event goes to (a drop, OB_E_DROP).
 */
IMPORT BOOL knl_ob_taker( CONST TS_UUID *uuid, UINT event, ID *p_pid );

/*
 * A grant: process pid may open the object by its UUID with 'ops' as
 * well as with what its credentials allow, until it ends or the grant
 * is taken back (ops 0). The oldest grant gives way when there is no
 * room.
 */
IMPORT void knl_ob_grant( ID pid, CONST TS_UUID *uuid, UINT ops );

/* What process pid holds granted on the object, 0 nothing */
IMPORT UINT knl_ob_granted( ID pid, CONST TS_UUID *uuid );

/*
 * What a driver says to the device manager: a medium put in or taken
 * out of the device of that name (OB_E_ATTACH / OB_E_DETACH on its
 * object), and the list of devices changed -- one registered or taken
 * away -- which is told the same way for each that came or went.
 */
IMPORT void knl_obdev_media( CONST UB *devnm, BOOL in );
IMPORT void knl_obdev_changed( void );

/*
 * Whether a process with these credentials may name a path through
 * fs_: only a plain path, and not into an object store's directory
 * unless it is an administrator (design 18.9).
 */
IMPORT BOOL knl_fs_path_let( CONST char *path, CONST T_OBCRD *crd );

/*
 * The object a key names, when the key is the calling process's and
 * allows at least 'ops': what a layer outside the object layer uses to
 * take a key as the right to act on the object (a window drawn in).
 * knl_ob_key_rec asks it of one record, through the record's own limit
 * as reading or writing that record would (the blocks of a disk
 * mounted). E_ID: not a key; E_OACV: another's, or not allowed.
 */
IMPORT ER   knl_ob_key_uuid( ID key, UINT ops, TS_UUID *p_uuid );
IMPORT ER   knl_ob_key_rec( ID key, UINT ops, INT recno, TS_UUID *p_uuid );

/* The object of the device of that name (E_NOEXS: none) */
IMPORT ER   knl_obdev_uuid( CONST UB *devnm, TS_UUID *p_uuid );

/*
 * A device the device management does not have: a keyboard, the
 * screen, a bank of pins, a device on the USB bus, the random source.
 * Whoever serves it adds it under a name and says in T_OBDVOPS what its
 * records are; the device manager gives it a UUID (the fixed one given,
 * or one kept in the device box under the name), lists it, protects it
 * and tells of it coming and going as of any device.
 *
 * The ops are called holding none of the manager's locks. 'pos' is a
 * word of the key's own, zero when the key was opened, for a record
 * read as a stream; 'ops' are what the key may do.
 */
#define OB_DV_NAME	32		/* bytes of a name */

typedef struct {
	UINT	sub;			/* OB_S_* */
	CONST char *kind;		/* the attribute device.kind */
	INT	nrec;			/* records, the description included */
	UINT	mode;			/* the protection: owner the system, group the administrators */
	INT	xrec;			/* a record only a key that may control the device reaches, 0 none */
	UINT	xops;			/* what that record lets through then: OB_OP_READ, OB_OP_WRITE */
	INT	(*text)( void *ctx, UB *t, INT n, INT max );		/* record 0's paragraphs */
	INT	(*attr)( void *ctx, UB *j, INT n, INT max );		/* members of "device" */
	ER	(*rea)( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize );
	ER	(*wri)( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize );
	UD	(*size)( void *ctx, INT recno );
	void	(*opened)( void *ctx, UD *pos );		/* a key opened: where it starts */
} T_OBDVOPS;

IMPORT ER   knl_obdev_add( CONST UB *name, CONST TS_UUID *fixed, CONST T_OBDVOPS *ops, void *ctx,
			   TS_UUID *p_uuid );
IMPORT ER   knl_obdev_del( CONST UB *name );

/* Something happened on such a device: told on it and on the list of the devices */
IMPORT void knl_obdev_post( CONST TS_UUID *uuid, INT recno, UINT event, CONST T_OBNTM *n );

/* A paragraph of record 0 that links to an object */
IMPORT INT  knl_obdev_link( UB *t, INT n, INT max, CONST TS_UUID *u, CONST UB *name );

/*
 * The devices that are not in the device management are made objects
 * once the object layer is up (knl_obdev_start), and the ones on the
 * USB bus again whenever one comes or goes (knl_obusb_changed, called
 * by the USB manager holding none of its locks).
 */
IMPORT void knl_obdev_start( void );
IMPORT void knl_obusb_changed( void );
IMPORT ER   knl_obusb_uuid( INT usbdev, TS_UUID *p_uuid );	/* the object of a USB device */
IMPORT ER   knl_obinput_uuid( UINT hid, TS_UUID *p_uuid );	/* of a keyboard or pointer */
IMPORT INT  knl_obusb_text( UB *t, INT max );			/* the system's record 4 */
IMPORT INT  knl_obdisp_text( UB *t, INT max );			/* the system's record 3 */
IMPORT void knl_obdisp_changed( void );				/* the screen came to another size */
IMPORT INT  knl_obhw_text( UB *t, INT max );			/* the system's record 9 */

/* What a subject may do to an object: its own protection within its volume's domain */
IMPORT UINT knl_ob_permit_on( CONST TS_UUID *uuid, CONST T_OBCRD *crd );

#ifdef __cplusplus
}
#endif

#endif /* __TS_OB_H__ */
