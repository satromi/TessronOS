/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu.h
 *	Trees of files taken in as real objects, and real objects given out
 *	as trees of files (design 18.20)
 *
 *	The rules of include/ts/xf.h, for a whole tree and for a program as
 *	well as the kernel. The files are reached through a T_XFUTREE -- a
 *	directory of the file layer (xfu_fs_tree), a server of FTP
 *	(xfu_ftp_tree), or anything else that can open, read, write, list
 *	and make directories -- and the objects through the ob_ calls alone,
 *	which a process makes through the SVC gateway and the kernel makes
 *	directly. So one library serves the ファイル変換 accessory and the
 *	kernel's tests.
 *
 *	Taken in: a file becomes an object of the form include/ts/xf.h
 *	gives (record 1 its bytes, record 0 xmlTAD that shows it,
 *	"tessronos.file" in the metadata);
 *	a text in any of the encodings of include/ts/txc.h is shown as a
 *	document in UTF-8; a picture as a figure holding it; a directory as
 *	a figure of links to what it holds, like a cabinet; the files of a
 *	TADjs object ({uuid}.json, {uuid}_N.xtad ...) as that object, under
 *	its own UUID.
 *
 *	Given out: an object taken in and not changed as the file it was,
 *	byte for byte; a text document as text; a figure of links as a
 *	directory of what it links to; anything else as its xmlTAD. Or, as
 *	TADjs file sets, the object and everything it links to.
 *
 *	What the library needs of the system besides ob_ -- memory and the
 *	decoding of a picture -- are the xfu_sys_ calls at the end, given by
 *	one small file for the kernel and one for a process.
 */

#ifndef __TS_XFU_H__
#define __TS_XFU_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/ftp.h>

#define XFU_NAME_MAX	768		/* bytes of a file's name, UTF-8, with its nought */
#define XFU_PATH_MAX	1024		/* bytes of a path in a tree, with its nought */
#define XFU_NAME_U16	255		/* UTF-16 units a name may take */
#define XFU_DEPTH	16		/* directories followed, when not said */
#define XFU_OBJ_MAX	4096		/* objects one call goes through at most */

/* ---------------------------------------------------------------- a tree of files */

/* What an entry of a tree is */
#define XFU_K_FILE	1
#define XFU_K_DIR	2

typedef struct {
	UB	name[XFU_NAME_MAX];	/* UTF-8 */
	UINT	kind;			/* XFU_K_* */
	D	size;			/* bytes; -1 when not known */
	D	mtime;			/* TS_TIME, UTC; 0 when not known */
} T_XFUENT;

/* Called for each entry of a directory; below 0 stops the listing */
typedef INT (*XFU_ENTCB)( void *arg, CONST T_XFUENT *e );

#define XFU_O_READ	1
#define XFU_O_WRITE	2		/* made, or cut to nothing */

/*
 * A tree: paths are UTF-8, '/' between the names, relative to the
 * tree's own root ("" the root itself). open answers a handle (0 or
 * more) that read, write and close take; read answers 0 at the end.
 * utime may be NULL when the tree keeps no times. namemax is the most
 * bytes one name may take there (0: XFU_NAME_U16 UTF-16 units).
 */
typedef struct {
	void	*ctx;
	INT	(*open)( void *ctx, CONST UB *path, UINT mode );
	INT	(*read)( void *ctx, INT h, void *buf, SZ n );
	INT	(*write)( void *ctx, INT h, CONST void *buf, SZ n );
	ER	(*close)( void *ctx, INT h );
	ER	(*list)( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg );
	ER	(*stat)( void *ctx, CONST UB *path, T_XFUENT *e );
	ER	(*mkdir)( void *ctx, CONST UB *path );
	ER	(*utime)( void *ctx, CONST UB *path, D mtime );
	INT	namemax;
} T_XFUTREE;

/* ---------------------------------------------------------------- how */

#define XFU_F_TADJS	0x0001		/* given out as TADjs file sets */
#define XFU_F_LAYOUT	0x0002		/* a figure given out as a directory keeps its
					   own xmlTAD in it, as <name>.xtad */
#define XFU_F_TAD	0x0004		/* a text document given out as xmlTAD, not text */
#define XFU_F_ONE	0x0008		/* the object alone: a figure as xmlTAD, not a
					   directory */
#define XFU_F_TEXT	0x0010		/* a text document given out as text in outenc
					   even when it is a file taken in unchanged */

/*
 * Told of each file or object as it is done: the name, and E_OK or what
 * went wrong. Answering below E_OK stops the call, which then answers
 * E_ABORT.
 */
typedef ER (*XFU_NOTE)( void *arg, CONST UB *name, ER er );

/* Told now and then while a long file is read or written: bytes so far; the same to stop */
typedef ER (*XFU_TICK)( void *arg, D bytes );

/* What a call did */
typedef struct {
	INT	files;			/* files read or written */
	INT	dirs;			/* directories */
	INT	objects;		/* objects made or given out */
	INT	skipped;		/* entries left out: a fault, too deep, a cycle */
	INT	geta;			/* characters of text that became 〓 */
	ER	last;			/* the last fault met, E_OK for none */
} T_XFUSTAT;

typedef struct {
	UINT		flags;		/* XFU_F_* */
	CONST char	*source;	/* taken in: "tessronos.file.source" (fat, ftp, usb, sd) */
	INT		enc;		/* taken in: the texts' encoding; TXC_AUTO to find out */
	INT		outenc;		/* given out: texts in this encoding (TXC_UTF8,
					   TXC_SJIS); TXC_AUTO: the one the text came in,
					   UTF-8 for one made here */
	INT		depth;		/* directories followed; 0: XFU_DEPTH */
	CONST char	*vol;		/* taken in: made on the volume of this path;
					   NULL: on the box's */
	CONST T_OBPRT	*prt;		/* taken in: NULL for the maker's default */
	XFU_NOTE	note;
	XFU_TICK	tick;		/* NULL for none */
	void		*arg;
	T_XFUSTAT	*stat;		/* what was done, or NULL */
	UINT		as;		/* taken in: what a file named by the call itself
					   is made, XFU_AS_*; the files of a directory
					   are always told by what they are */
	INT		shrink;		/* taken in: a picture shown at 1/shrink of its
					   size (2, 4, 8); 0 or 1 as it is */
	CONST TS_UUID	*near;		/* taken in with no box: made on this object's volume */
	CONST UB	*name;		/* given out: the name of what the object becomes,
					   in place of its own; NULL its own */
} T_XFUOPT;

/* What a file is made when it is taken in */
#define XFU_AS_AUTO	0		/* told by its first bytes and its name */
#define XFU_AS_TEXT	1		/* a text, shown as a document */
#define XFU_AS_ZEN	2		/* a text whose letters and digits become full width */
#define XFU_AS_IMAGE	3		/* a picture, shown as a figure */
#define XFU_AS_RAW	4		/* bytes kept in record 1, record 0 its name alone */
#define XFU_AS_ARCHIVE	5		/* a BTRON archive, opened by 書庫解凍 */

/*
 * A file or a directory of a tree taken in. With a box, a link to what
 * was made is added to the box's record 0 (a figure) and counted; with
 * none, nothing links to it yet and the caller links it. When it fails
 * or is stopped, every object it made is thrown away again.
 */
IMPORT ER  xfu_import( CONST T_XFUTREE *t, CONST UB *path, CONST T_XFUOPT *opt,
		       CONST TS_UUID *box, TS_UUID *p_uuid );

/*
 * An object given out into a directory of a tree, under a name no
 * other entry there has; that name comes back in name (may be NULL).
 */
IMPORT ER  xfu_export( CONST TS_UUID *uuid, CONST T_XFUTREE *t, CONST UB *dir,
		       CONST T_XFUOPT *opt, UB *name, INT max );

/*
 * A new box: an empty figure of links that opens as a list (仮身一覧),
 * on the volume of 'near' (NULL: the first), linked to by nothing yet.
 */
IMPORT ER  xfu_make_box( CONST UB *name, CONST TS_UUID *near, TS_UUID *p_uuid );

/* A link to an object put at the foot of a box's figure, and counted */
IMPORT ER  xfu_box_link( CONST TS_UUID *box, CONST TS_UUID *uuid );

/* ---------------------------------------------------------------- names */

/*
 * An object's name made a name a FAT volume takes: \ / : * ? " < > |
 * turned into their full width forms, a control character into its
 * picture (U+2400 on), the dots and blanks at the end taken off, a
 * device's name (CON, NUL, COM1 ...) given a '_', and the whole kept
 * within XFU_NAME_U16 units and max - 1 bytes, the extension (from
 * the last dot) kept whole. An empty name becomes "_". Answers the
 * length.
 */
IMPORT INT xfu_name_out( CONST UB *name, UB *out, INT max );

/* A file's name made an object's name again: the full width forms back */
IMPORT INT xfu_name_in( CONST UB *name, UB *out, INT max );

/*
 * The names of one directory, so that a new one does not meet an old:
 * compared with the case of A to Z ignored, as FAT does.
 */
typedef struct {
	UB	*buf;
	INT	used, max;
} T_XFUNAMES;

IMPORT void xfu_names_init( T_XFUNAMES *s );
IMPORT void xfu_names_free( T_XFUNAMES *s );
IMPORT BOOL xfu_names_has( CONST T_XFUNAMES *s, CONST UB *name );
IMPORT ER   xfu_names_add( T_XFUNAMES *s, CONST UB *name );

/*
 * A name made one the set does not have yet, " (2)", " (3)" ... put
 * before its extension, and added to the set. namemax as T_XFUTREE's.
 */
IMPORT ER   xfu_names_unique( T_XFUNAMES *s, UB *name, INT max, INT namemax );

/* ---------------------------------------------------------------- trees */

/* A directory of the file layer as a tree */
#define XFU_FS_OPEN	8		/* files open at once */
typedef struct {
	char	root[XFU_PATH_MAX];
	INT	fd[XFU_FS_OPEN];
	char	path[XFU_PATH_MAX];
} T_XFUFS;

IMPORT ER  xfu_fs_tree( T_XFUFS *fs, CONST char *root, T_XFUTREE *t );

/* A directory of an FTP server as a tree (include/ts/ftp.h); one file open at a time */
typedef struct {
	T_FTP	*ftp;			/* logged in */
	UB	root[XFU_PATH_MAX];
	UB	path[XFU_PATH_MAX];
	BOOL	busy;			/* a file is open */
	BOOL	eof;			/* and the end of it was read */
	UB	want[XFU_NAME_MAX];	/* the name a stat looks for */
} T_XFUFTP;

IMPORT ER  xfu_ftp_tree( T_XFUFTP *ft, T_FTP *ftp, CONST UB *root, T_XFUTREE *t );

/* ---------------------------------------------------------------- helpers */

/* The CRC-32 of ISO 3309 (as PNG and ZIP), carried on from crc (0 to start) */
IMPORT UINT xfu_crc32( UINT crc, CONST UB *b, SZ n );

/*
 * Pixels (0x00rrggbb, row after row; 0xFFFFFFFF clear) made a PNG:
 * stored, not compressed, which any decoder reads. RGB, or RGBA when a
 * pixel is clear. The PNG is xfu_sys_alloc's.
 */
IMPORT ER  xfu_png_encode( CONST UINT *px, INT w, INT h, UB **p_out, SZ *p_len );

/* The width and height a PNG says it has */
IMPORT ER  xfu_png_size( CONST UB *data, SZ size, INT *p_w, INT *p_h );

/* A TS_TIME as ISO 8601 in UTC ("2026-09-27T10:00:00Z", 21 bytes), and back */
IMPORT INT xfu_time_iso( D t, UB *out );
IMPORT D   xfu_time_parse( CONST UB *s );

/* ---------------------------------------------------------------- what the system gives */

IMPORT void *xfu_sys_alloc( SZ size );
IMPORT void  xfu_sys_free( void *p );

/*
 * A picture file (PNG, JPEG, BMP ...) as 0x00rrggbb pixels in memory of
 * xfu_sys_alloc; E_NOSPT for a kind that cannot be read.
 */
IMPORT ER    xfu_sys_decode( CONST UB *data, SZ size, UINT **p_px, INT *p_w, INT *p_h );

/* A new UUID for a link's own identity */
IMPORT ER    xfu_sys_uuid( TS_UUID *p_uuid );

#ifdef __cplusplus
}
#endif

#endif /* __TS_XFU_H__ */
