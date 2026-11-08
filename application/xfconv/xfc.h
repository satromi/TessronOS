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
#include <ts/vobjsz.h>

#define XFC_ENT_MAX	2048		/* entries a list holds */
#define XFC_DISKS	2048		/* disks listed: every disk and partition the device manager knows */
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

typedef struct xfcbt XFCBT;		/* a disk of the BTRON file system (xfc_btron.c) */

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
	INT	fid;			/* a 実身 of a BTRON disk: its file ID */
	char	btron[64];		/* E_DISK: the name of its BTRON file system, "" none */
	char	fmt[16];		/* E_DISK: what is on it (tessronos.format.kind), "" not known */
	BOOL	nofs;			/* E_DISK: a kind no file system here mounts */
	BOOL	iso;			/* of the ISO 9660 side of a BTRON CD-ROM: a file or a
					   directory there, not a 実身 */
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
	UB	hostkey[65];		/* SSH: the SHA-256 of the server's key once accepted, in hex */
	UB	newkey[65];		/* one shown to the user and not yet accepted */
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
	BOOL		mounted;		/* its disk opened by this window (vd): let go when it closes */
	INT		vd;			/* the disk's root, from its device object (fs_opn_vol) */
	ID		vkey;			/* the device object, opened for it */
	BOOL		removable;
	BOOL		ro;			/* mounted for reading only */
	UB		dev[64];		/* the disk's name, or the connection's */
	char		label[160];		/* the disk's device object's name, or "ftp://host" */
	char		source[8];		/* tessronos.file.source */
	UB		dir[XFU_PATH_MAX];	/* the directory shown, from the root */
	UB		(*hist)[XFU_PATH_MAX];	/* directories opened before, the latest first */
	INT		nhist;

	/* a disk of the BTRON file system: dir is then the file IDs gone into, "3/17" */
	XFCBT		*bt;
	INT		niso;			/* entries of the ISO 9660 side's root, in tree; 0 none.
						   Its directories are "@iso/..." in dir */
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
#define XFC_TEXT_PX	16		/* the letters of an entry */
#define XFC_VOBJ_H	VOBJ_CLOSED_H(XFC_TEXT_PX, 1, 1)	/* a 仮身 in it: closed, at those letters */
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

/* ---------------------------------------------------------------- BTRON disks (xfc_btron.c) */

/* Whether a disk holds the BTRON file system (a CD-ROM's partition, or from its start); its name */
IMPORT BOOL xfc_bt_probe( CONST TS_UUID *disk, D size, char *name, INT max );
IMPORT void xfc_bt_forget( CONST TS_UUID *disk );	/* what the probe kept of a disk let go */
IMPORT ER   xfc_bt_open( XWIN *w, CONST TS_UUID *disk );
IMPORT void xfc_bt_close( XWIN *w );
IMPORT CONST char *xfc_bt_name( XWIN *w );
IMPORT INT  xfc_bt_fid( CONST UB *dir );
IMPORT void xfc_bt_names( XWIN *w, CONST UB *dir, char *out, INT max );

/* Each 実身 a 実身 links to: its name, file ID, whether it links to others, its bytes */
typedef INT (*XFC_BTPUT)( void *arg, CONST UB *name, INT fid, BOOL links, D size );
IMPORT ER   xfc_bt_list( XWIN *w, INT fid, XFC_BTPUT put, void *arg );

/* A 実身 and what it links to made 実身 on the volume of into; the first made, and how many */
/*
 * The files of a BTRON CD-ROM's ISO 9660 side, beside its partition, as
 * a tree to read (the partition's image left out): how many entries its
 * root has, 0 when there is no such side (t is then of no use).
 */
IMPORT INT  xfc_bt_iso_tree( XWIN *w, T_XFUTREE *t );

typedef void (*XFC_BTPROG)( INT done, INT total );
IMPORT ER   xfc_bt_import( XWIN *w, INT fid, CONST TS_UUID *into, TS_UUID *p_top, INT *p_objs, XFC_BTPROG prog,
			   char *err, INT errmax );

#endif /* __XFC_H__ */
