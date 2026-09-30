/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc.h
 *	ファイル変換: the accessory's own parts (design 17.17)
 */

#ifndef __XFC_H__
#define __XFC_H__

#include <ts/uapp.h>
#include <ts/fs.h>
#include <ts/txc.h>
#include <ts/ftp.h>
#include <ts/xfu.h>
#include <ts/ui.h>

#define XFC_ENT_MAX	2048		/* entries a list holds */
#define XFC_CONN_MAX	8		/* FTP connections kept */
#define XFC_FIELD	128		/* bytes of a field of the FTP form */
#define XFC_WIN_MAX	8		/* windows at once: the media and the file lists */
#define XFC_HIST	8		/* directories a file list remembers */

/* What an entry of a list is */
#define E_DISK		1		/* a disk device object */
#define E_FTP		2		/* an FTP connection kept */
#define E_NEWFTP	3		/* a new FTP connection: ネットワーク(FTP) */
#define E_VOL		4		/* the volume shown, at the top of a file list */
#define E_UP		5		/* the directory above */
#define E_DIR		6
#define E_FILE		7

typedef struct {
	UINT	kind;			/* E_* */
	UB	name[256];		/* what is shown and, for a file, its name */
	D	size;			/* bytes, -1 unknown */
	D	mtime;
	TS_UUID	uuid;			/* E_DISK: the device object */
	UB	mount[64];		/* E_DISK: where it is mounted, empty for nowhere */
	BOOL	ro;			/* E_DISK: mounted for reading only */
	INT	conn;			/* E_FTP: which connection */
	BOOL	marked;			/* chosen */
	BOOL	hidden;			/* a name that starts with a dot */
	BOOL	faint;			/* cannot be used now: no medium */
	INT	vw;			/* the width of its 仮身, as last drawn */
} XFCENT;

/* An FTP connection as the panel and the settings have it */
typedef struct {
	UB	host[XFC_FIELD];
	UB	port[8];
	UB	user[XFC_FIELD];
	UB	pass[XFC_FIELD];
	UB	name[XFC_FIELD];	/* what the connection is called, "" the host */
	INT	enc;			/* TXC_UTF8, TXC_SJIS, TXC_EUCJP */
	INT	mode;			/* how data goes: XFC_DM_* */
	UB	lport[8];		/* the active way: the port listened on, "" any */
	BOOL	keep;			/* the password is kept with the settings */
	BOOL	ssh;			/* by SSH (SFTP) */
} XFCCONN;

/* How the data of FTP goes */
#define XFC_DM_EPSV	0		/* EPSV, PASV when it is refused */
#define XFC_DM_PASV	1		/* PASV only */
#define XFC_DM_ACTIVE	2		/* the server connects: EPRT, PORT when it is refused */

/* The connections over sockets (xfc_net.c) */
#define XNET_MAX	4
typedef struct {
	INT	s[XNET_MAX];		/* the socket of each connection, -1 none */
	UINT	addr[XNET_MAX];		/* the host it reached, network order */
	UINT	tmo;			/* ms a read waits */
	UINT	lport;			/* the active way: the port to listen on, 0 any */
} XFCNET;

/* What a window is */
#define W_MEDIA		1		/* the disks and the FTP connections */
#define W_FILES		2		/* the folders and files of one volume or server */

/*
 * A window of the accessory. The main one lists the media; each medium
 * opened has a window of its own listing one directory of it, the
 * entries drawn as 仮身.
 */
typedef struct {
	BOOL		used;
	INT		kind;			/* W_* */
	TS_UUID		win;
	ID		kw;
	INT		gid;
	BOOL		dirty;
	XFCENT		*ent;
	INT		nent;
	INT		cur;			/* the entry the keys are on */
	INT		anchor;			/* where a run chosen with Shift starts */
	INT		top;			/* the first row shown */
	INT		press_row;		/* the last press, for a double one */
	UD		press_at;
	char		typed[32];		/* what was typed to find a name */
	UD		typed_at;
	BOOL		band;			/* a rectangle is being pulled out */
	INT		bx0, by0, bx1, by1;
	BOOL		thumb;			/* the scroll bar's knob is held */
	char		status[300];
	BOOL		status_err;
	UINT		sum;			/* what the list said last */

	/* W_FILES */
	T_XFUTREE	tree;
	T_XFUFS		*fsx;
	T_XFUFTP	*ftx;
	T_FTP		*ftp;
	XFCNET		xnet;
	T_FTPNET	fnet;
	BOOL		is_ftp;
	TS_UUID		disk;			/* the disk shown */
	BOOL		mounted;		/* mounted by this window: taken off when it closes */
	BOOL		removable;
	BOOL		ro;			/* mounted for reading only */
	UB		dev[64];		/* the disk's name, or the connection's */
	char		label[160];		/* "/media/uda0" or "ftp://host" */
	char		source[8];		/* tessronos.file.source */
	UB		dir[XFU_PATH_MAX];	/* the directory shown, from the root */
	UB		(*hist)[XFU_PATH_MAX];	/* directories opened before, the latest first */
	INT		nhist;
} XWIN;

IMPORT void xfc_net_init( XFCNET *n, T_FTPNET *net, UINT tmo, UINT lport );
IMPORT ER   xfc_net_up( void );		/* an address for the interface, by DHCP when it has none */

/* The settings kept in the program object's metadata (tessronos.xfconv) */
IMPORT INT  xfc_conf_load( XFCCONN *c, INT max );
IMPORT ER   xfc_conf_save( CONST XFCCONN *c, INT n );

IMPORT void xfc_uuid_str( CONST TS_UUID *u, char *out );
IMPORT BOOL xfc_uuid_parse( CONST char *s, TS_UUID *u );

/* ---------------------------------------------------------------- drawing (xfc_draw.c) */

#define XFC_ROW_H	32		/* a row of the list */
#define XFC_VOBJ_H	25		/* a 仮身 in it: a closed one at 14 points */
#define XFC_TOP		10		/* above the first row */
#define XFC_LEFT	12		/* left of the 仮身 */
#define XFC_SB_W	14		/* the scroll bar */
#define XFC_STATUS_H	24		/* the line at the foot */

IMPORT void xfc_work_size( XWIN *w, INT *p_w, INT *p_h );
IMPORT INT  xfc_rows( XWIN *w );		/* rows the list shows */
IMPORT void xfc_paint( XWIN *w );
IMPORT void xfc_size_text( D v, char *out, INT max );

/* Where the 仮身 of entry i is drawn, in the work area; FALSE when it is not shown */
IMPORT BOOL xfc_vobj_rect( XWIN *w, INT i, T_DPRECT *r );

/* The entry whose 仮身 is at (x, y), or -1 */
IMPORT INT  xfc_hit( XWIN *w, INT x, INT y );

/* The width a 仮身 of that entry takes */
IMPORT INT  xfc_vobj_w( XWIN *w, CONST XFCENT *e );

/* ---------------------------------------------------------------- panels (xfc_dlg.c) */

/* How the files are made 実身, as the panel was answered */
typedef struct {
	UINT	as;			/* XFU_AS_* */
	INT	enc;			/* TXC_* for a text, TXC_AUTO found out */
	INT	shrink;			/* 1, 2, 4, 8 */
} XFCIMP;

/* How 実身 are made files */
typedef struct {
	UB	name[XFU_NAME_MAX];	/* the name of the one written, "" its own */
	UINT	flags;			/* XFU_F_* */
	INT	outenc;
} XFCEXP;

IMPORT void xfc_dlg_init( ID port );
IMPORT BOOL xfc_ask_import( XWIN *w, INT n, CONST UB *first, XFCIMP *how );
IMPORT BOOL xfc_ask_export( XWIN *w, INT n, CONST UB *first, CONST UB *dir, XFCEXP *how,
			    BOOL tadjs, BOOL layout, BOOL sjis );
IMPORT void xfc_tell( XWIN *w, CONST char *l1, CONST char *l2, CONST char *l3 );

/*
 * The FTP panel: the connection filled in and answered with 接続, TRUE;
 * try is called for each 接続 and answers E_OK when it connected, or
 * what to say on the panel.
 */
typedef ER (*XFC_TRY)( XFCCONN *c, char *why, INT max );
IMPORT BOOL xfc_ask_ftp( XWIN *w, XFCCONN *c, XFC_TRY try_connect );

/* Something that came to the channel while a panel was up (xfc_main.c) */
IMPORT void xfc_meanwhile( CONST T_OBNTM *m );

IMPORT void xfc_log( CONST char *s );

#endif /* __XFC_H__ */
