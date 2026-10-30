/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	arc.h
 *	書庫管理: the program's own parts (design 11.20)
 *
 *	An archive object (書庫実身) holds at most one book (書籍): the
 *	objects reachable through the links from one root, packed into an
 *	archive file (lib/libbpk) kept as a record of plain data
 *	(OB_RT_SYSDATA) of the object. An archive object without that
 *	record is empty (空書庫). arc_pack.c finds the objects and writes
 *	the book; unp_obj.c takes it out again, and opens an archive object
 *	that was taken in from a .bpk file the same way.
 */

#ifndef __ARC_H__
#define __ARC_H__

#include <ts/bpk.h>
#include <ts/uapp.h>
#include <ts/dtreq.h>
#include "unp.h"
#include "arc_arg.h"

#define ARC_NAME_MAX	128		/* bytes of a name, UTF-8 */
#define ARC_ERR_MAX	200
#define ARC_APPL_MAX	4		/* programs of one object's applist written */

/* What kind of object it is to the archive */
#define K_PLAIN		0		/* records of its own: record 0 becomes binary TAD */
#define K_ARCHIVE	1		/* an archive object with a book: the book is its TAD */
#define K_EMPTY		2		/* an archive object with none */

/* One object found from the root */
typedef struct {
	TS_UUID	uuid;
	char	name[ARC_NAME_MAX];
	INT	kind;			/* K_* */
	BPKAPPL	appl[ARC_APPL_MAX];	/* the programs that open it, the first first */
	INT	nappl;
	INT	win[4];			/* its window: left, top, right, bottom */
	BOOL	haswin;
	UINT	bg;			/* its window's colour, a COLOR */
	BOOL	hasbg;
	INT	sv[50];			/* マイクロスクリプト's $SV */
	BOOL	hassv;
	UINT	ctime, mtime, atime, ltime;	/* STIME; ltime 0xFFFFFFFF none */
	UINT	ftype;			/* BPK_F_* */
	TS_UUID	*target;		/* what each link of record 0 points at */
	UH	*latr;			/* and how it looks (BPK_V_*) */
	INT	nlink, maxlink;
	UD	size;			/* its records' bytes */
} ARCOBJ;

/* The look of the root's link the book keeps */
typedef struct {
	BOOL	has;
	INT	box[4];
	UINT	attr;			/* BPK_V_* */
	INT	chsz;			/* points */
	UINT	col[4];			/* 0x00rrggbb: frame, letters, band, ground */
} ARCLOOK;

typedef struct arcjob ARCJOB;

/* How far the work is: done of total, and what it is doing */
typedef void (*ARC_TELL)( ARCJOB *j, INT done, INT total, const char *what );

struct arcjob {
	TS_UUID	archive;		/* the archive object */
	TS_UUID	root;
	ARCOBJ	*obj;			/* the objects found, the root first */
	INT	nobj, maxobj;
	UD	total;			/* their records' bytes */
	ARC_TELL tell;
	BPKWSTAT st;			/* what the book written came to */
	UINT	time;			/* when */
	char	err[ARC_ERR_MAX];
};

/* ---------------------------------------------------------------- arc_pack.c */

/* The objects reachable from root, the archive itself left out; E_OK or why not in err */
IMPORT ER   arc_find( ARCJOB *j, const TS_UUID *archive, const TS_UUID *root );

/* The objects found packed into the archive object, compressed with method; look may be NULL */
IMPORT ER   arc_pack( ARCJOB *j, INT method, const ARCLOOK *look );
IMPORT void arc_free( ARCJOB *j );

/*
 * The archive object's book: the record that holds it, -1 when the
 * object is empty, and its bytes (malloc'd) in *p_raw when p_raw is
 * not NULL. E_NOEXS when the object cannot be read.
 */
IMPORT ER   arc_book_rec( const TS_UUID *archive, INT *p_recno, UB **p_raw, UINT *p_len );

/* An archive file put into the archive object as its book, or the book taken away (raw NULL) */
IMPORT ER   arc_book_put( const TS_UUID *archive, const UB *raw, UINT len );

/* The time now as STIME, and the offset of local time in seconds */
IMPORT UINT arc_now( INT *p_offset );

/* A link to target put at the end of the figure (or text) of record 0 of into */
IMPORT ER   arc_link_into( const TS_UUID *into, const TS_UUID *target, const char *name );

#endif /* __ARC_H__ */
