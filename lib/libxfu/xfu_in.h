/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_in.h
 *	What the files of lib/libxfu share
 */

#ifndef __XFU_IN_H__
#define __XFU_IN_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/json.h>
#include <ts/txc.h>
#include <ts/xfu.h>

#define XFU_CHUNK	( 64 * 1024 )		/* bytes read or written at a time */
#define XFU_TAD_MAX	( 8 * 1024 * 1024 )	/* an xmlTAD file taken whole */
#define XFU_TEXT_MAX	( 256 * 1024 )		/* bytes of text shown in record 0 */
#define XFU_IMG_MAX	( 16 * 1024 * 1024 )	/* a picture taken whole to show it */
#define XFU_PIX_MAX	( 16 * 1024 * 1024 )	/* pixels of a picture made a PNG */

/* The media type of a directory taken in */
#define XFU_MT_DIR	"inode/directory"
#define XFU_MT_TADJS	"application/x-tadjs"
#define XFU_MT_TAD	"application/x-xmltad"

/* The programs an object taken in opens with */
#define XFU_APP_TEXT	0		/* basic-text-editor */
#define XFU_APP_FIG	1		/* basic-figure-editor */
#define XFU_APP_LIST	2		/* virtual-object-list */
#define XFU_APP_UNPACK	3		/* unpack-file: a BTRON archive */
#define XFU_NAPP	4
#define XFU_APP_NONE	(-1)

/* ---------------------------------------------------------------- bytes and text */

IMPORT void xfu_mcpy( void *d, CONST void *s, SZ n );
IMPORT void xfu_mset( void *d, INT c, SZ n );
IMPORT INT  xfu_slen( CONST UB *s );
IMPORT INT  xfu_scpy( UB *d, INT max, CONST UB *s );
IMPORT BOOL xfu_same( CONST UB *a, CONST char *b );
IMPORT BOOL xfu_same_ci( CONST UB *a, CONST UB *b );
IMPORT INT  xfu_find( CONST UB *s, SZ n, CONST char *w, SZ from );

/* A buffer that grows */
typedef struct {
	UB	*b;
	SZ	at, max;
	BOOL	bad;			/* memory ran out: what was asked for is not there */
} XBUF;

IMPORT void xb_init( XBUF *w );
IMPORT void xb_free( XBUF *w );
IMPORT void xb_bytes( XBUF *w, CONST void *s, SZ n );
IMPORT void xb_put( XBUF *w, CONST char *s );
IMPORT void xb_num( XBUF *w, D v );
IMPORT void xb_xml( XBUF *w, CONST UB *s, SZ n );	/* escaped for XML */
IMPORT void xb_json( XBUF *w, CONST UB *s, SZ n );	/* escaped for a JSON string */

/* ---------------------------------------------------------------- identities */

IMPORT void xfu_uuid_str( CONST TS_UUID *u, char *out );	/* 37 bytes */
IMPORT BOOL xfu_str_uuid( CONST UB *s, TS_UUID *u );		/* the first 36 bytes */
IMPORT BOOL xfu_uuid_eq( CONST TS_UUID *a, CONST TS_UUID *b );

/* ---------------------------------------------------------------- objects */

/* A record read whole (a nought after it), the caller's to xfu_sys_free */
IMPORT UB  *xfu_rec_all( ID key, INT recno, SZ *p_size );

/* The metadata read whole, the same */
IMPORT UB  *xfu_meta_all( ID key, SZ *p_size );

/* "tessronos.file" of the metadata replaced by the JSON object given, the rest kept */
IMPORT ER   xfu_meta_file( ID key, CONST UB *file, SZ n );

/* Whether the store of an object counts the links of its records itself */
IMPORT BOOL xfu_counts( CONST TS_UUID *u );

/* The links of an xmlTAD text, each target given to cb in order */
typedef void (*XFU_LINKCB)( void *arg, CONST TS_UUID *target );
IMPORT INT  xfu_links( CONST UB *s, SZ n, XFU_LINKCB cb, void *arg );

/* One <link> of a figure, at a place, into w */
IMPORT void xfu_link_xml( XBUF *w, CONST TS_UUID *target, INT l, INT t, INT r, INT b, INT z );

/* A link to an object put in a box's figure, and counted */
IMPORT ER   xfu_link_add( CONST TS_UUID *box, CONST TS_UUID *u );

/* ---------------------------------------------------------------- a call's state */

typedef struct {
	BOOL	have;
	UB	name[OB_NAME_MAX];	/* what the program is called */
	UB	*icon;			/* its template's icon, or NULL */
	SZ	iconsz;
} XFUAPP;

typedef struct {
	CONST T_XFUTREE	*t;
	CONST T_XFUOPT	*opt;
	T_XFUSTAT	st;
	TS_UUID		near;			/* objects are made on this one's volume */
	BOOL		hasnear;
	INT		depth;
	UB		*chunk;			/* XFU_CHUNK bytes */
	UB		path[XFU_PATH_MAX];	/* the path of the entry at work */
	T_XFUENT	ent;
	XFUAPP		app[XFU_NAPP];
	BOOL		apps;			/* the programs have been looked up */
	TS_UUID		*seen;			/* given out: objects met, XFU_OBJ_MAX */
	INT		nseen;
	TS_UUID		*stack;			/* the objects being given out, outermost first */
	INT		nstack;
	TS_UUID		*made;			/* taken in: the objects made, in order */
	INT		nmade;
	BOOL		stop;			/* the caller asked to stop */
	BOOL		zen;			/* the text at work is made full width */
} XFU;

IMPORT ER   xfu_begin( XFU *x, CONST T_XFUTREE *t, CONST T_XFUOPT *opt );
IMPORT void xfu_finish( XFU *x );
IMPORT void xfu_note( XFU *x, CONST UB *name, ER er );
IMPORT void xfu_tick( XFU *x, D bytes );

/* An object made by the call noted, forgotten, and all of them thrown away again */
IMPORT void xfu_made( XFU *x, CONST TS_UUID *u );
IMPORT void xfu_unmade( XFU *x, CONST TS_UUID *u );
IMPORT void xfu_undo( XFU *x );

/* The program's name and template icon, looked up once in the program box */
IMPORT XFUAPP *xfu_app( XFU *x, INT app );
IMPORT CONST char *xfu_app_id( INT app );

/* A path joined: dir, '/', name into out */
IMPORT ER   xfu_join( UB *out, INT max, CONST UB *dir, CONST UB *name );

/* The width of a name in UTF-16 units */
IMPORT INT  xfu_u16( CONST UB *s, INT n );

#endif /* __XFU_IN_H__ */
