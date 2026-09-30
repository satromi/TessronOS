/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk.h
 *	バックアップ: the accessory's own parts (design 17.18)
 *
 *	Saving (bk_save.c) walks the objects linked from a root and writes
 *	them as volumes of a BTRON backup archive with lib/libbtbk; restoring
 *	(bk_rest.c) reads such volumes and makes the objects again, their
 *	links joined once every object is there. Where the volumes are --
 *	files on a medium or objects of their own -- is bk_media.c's.
 */

#ifndef __BK_H__
#define __BK_H__

#include <ts/uapp.h>
#include <ts/btbk.h>
#include <ts/bpk.h>
#include <ts/time.h>

#define BK_PROG_ID	"btron-backup"
#define BK_NAME_MAX	128		/* bytes of a name, UTF-8 */
#define BK_ERR_MAX	200
#define BK_OBJ_CAP	0x60000000ULL	/* what one volume of an object holds */
#define BK_OBJ_KEEP	16		/* blocks of the store a volume object leaves free */
#define BK_ROOT_MAX	64		/* roots saved together */
#define BK_EXCL_MAX	64		/* objects left out of a save */

/*
 * A root to save: the object, the one whose record links to it and
 * where that link is (a figure's box), kept in the archive so that a
 * restore can put it back there.
 */
typedef struct {
	TS_UUID	uuid;
	TS_UUID	parent;			/* 0 when it is not known */
	BOOL	hasbox;
	INT	box[4];			/* left, top, right, bottom */
} BKROOT;

/*
 * The record TessronOS keeps its own of an object in, beside those BTRON
 * reads: a type of an application's own (16 to 31), which BTRON keeps
 * as it is and restores as it is. "TFBK", the lengths, then the
 * metadata, the icon, the xmlTAD of record 0 with each link as its
 * number among the link records, and the resources.
 */
#define BK_RT_TESSRONOS	28
#define BK_SUB_TESSRONOS	0x5446		/* "TF" */
#define BK_TS_MAGIC	"TFBK"

/* ---------------------------------------------------------------- media */

/*
 * A place volumes go to or come from: a directory of the file layer
 * (a medium mounted through its device object, or /boot), or real
 * objects of their own. bk_media_list says which there are, by the
 * disks the system has.
 */
#define BK_MED_OBJECT	0		/* each volume an object */
#define BK_MED_DIR	1		/* each volume a file in a directory */
#define BK_MED_MAX	16

typedef struct {
	INT	kind;			/* BK_MED_* */
	char	label[40];		/* as the window shows it */
	char	dev[FS_DEVNM_MAX];	/* the disk's name; "" for /boot */
	TS_UUID	obj;			/* the disk's object */
	char	dir[FS_PATH_MAX];	/* where its files are, once mounted */
	ID	key;			/* the disk opened to mount it; 0 not */
	BOOL	mounted;		/* mounted here, to be let go */
	TS_UUID	into;			/* objects: the cabinet they are linked into, and on
					   the volume of; 0 the first cabinet */
} BKMEDIA;

IMPORT INT  bk_media_list( BKMEDIA *m, INT max );
IMPORT ER   bk_media_open( BKMEDIA *m, BOOL write );
IMPORT void bk_media_close( BKMEDIA *m );
IMPORT UD   bk_media_free( BKMEDIA *m );

/* The link to u in the record of parent: its box in a figure, FALSE when there is none */
IMPORT BOOL bk_link_box( const TS_UUID *parent, const TS_UUID *u, INT box[4] );

/* The files of a directory whose names end in .tad, into names[][64]; how many */
IMPORT INT  bk_media_files( BKMEDIA *m, char (*names)[FS_NAME_MAX], INT max );

/* ---------------------------------------------------------------- names */

/* "N・base", N in full width digits, 20 characters at most as TRON code */
IMPORT void bk_vol_name( const char *base, INT volno, char *out, INT max );
/* The number a volume's name starts with, -1 when it has none */
IMPORT INT  bk_vol_number( const char *name );
/* The file a volume is written to */
IMPORT void bk_vol_file( const char *name, char *out, INT max );

IMPORT void bk_uuid_str( const TS_UUID *u, char *out );
IMPORT BOOL bk_uuid_parse( const char *s, TS_UUID *u );
IMPORT UB  *bk_read_rec( ID key, INT recno, UINT *p_len );
IMPORT ER   bk_write_all( ID key, INT recno, const void *data, UINT len );
IMPORT INT  bk_iso_of( UINT stime, char *out );	/* STIME as "2025-10-09T07:30:36Z" */
IMPORT UINT bk_stime_of( const char *iso );	/* and back, 0 when it is not one */

/* ---------------------------------------------------------------- saving */

typedef struct bkjob BKJOB;

/* What the work says as it goes: a line, and how far (done of total) */
typedef void (*BK_TELL)( BKJOB *j, const char *line, UD done, UD total );

typedef struct {
	TS_UUID	uuid;
	UINT	objid;
	UINT	fid;			/* its number among the objects, from 1 */
	char	name[BK_NAME_MAX];
	BK_FSTATE st;
	INT	nlink;
	UINT	*links;			/* objid each link record points at, 0 none */
	UH	*latr;			/* its attr */
	UINT	taglen;			/* bytes of binary TAD record 0 becomes */
	INT	nres;
} BKOBJ;

struct bkjob {
	BK_TELL	tell;
	void	*ctx;
	volatile BOOL stop;		/* 中止 was asked for */
	char	err[BK_ERR_MAX];

	/* saving: the first nroot objects are the roots, root[] says of them */
	BKOBJ	*obj;
	INT	nobj, maxobj;
	const BKROOT *sroot;
	INT	nroot;
	const TS_UUID *excl;		/* what is left out: links to it stay links to it */
	INT	nexcl;
	UD	total, src;
	char	base[BK_NAME_MAX];	/* the volumes' name after "N・" */
	UD	done;

	/* restoring */
	BK_VOLHEAD vh;
	INT	nvol;			/* volumes read */
	BOOL	more;			/* the set goes on */
	TS_UUID	near;			/* the volume the objects are made on */
	INT	nmade, nlinked, nlost, nconv;
	TS_UUID	root;			/* the first root */
	INT	nrroot;			/* the roots, as the archive says them */
	BKROOT	rroot[BK_ROOT_MAX];
	void	*rest;			/* bk_rest.c's state */
};

/*
 * The objects linked from the roots, the roots first, with what each
 * will write: the totals of the volume head. The objects of j->excl are
 * not saved, nor what only they link to. Then the volumes written: each
 * opened with open(ctx, volno, name, &capacity) giving where its bytes
 * go, and closed with close(ctx, more). E_OK, or why not in j->err.
 */
typedef struct {
	ER	(*open)( void *ctx, INT volno, const char *name, BK_OUT *p_out, void **p_arg, UD *p_cap );
	ER	(*close)( void *ctx, INT volno, BOOL more );
	void	*ctx;
} BKDEST;

IMPORT ER   bk_save_walk( BKJOB *j, const BKROOT *roots, INT nroot );
IMPORT ER   bk_save_write( BKJOB *j, const BKDEST *d );
IMPORT void bk_save_free( BKJOB *j );

/* ---------------------------------------------------------------- restoring */

/*
 * A volume read: its head and each object in it, the objects made on the
 * volume of j->near, their records written. More volumes of the set may
 * follow (j->more); bk_rest_end then joins the links, writes record 0 of
 * each object and says which the roots are and where the archive says
 * they were linked from (j->rroot; an archive BTRON wrote has one root,
 * its first object, with no place). src reads the volume's bytes.
 */
IMPORT ER   bk_rest_begin( BKJOB *j );
IMPORT ER   bk_rest_volume( BKJOB *j, BK_SRC src, void *arg );
IMPORT ER   bk_rest_end( BKJOB *j );
IMPORT void bk_rest_undo( BKJOB *j );		/* what was made taken away */
IMPORT void bk_rest_free( BKJOB *j );

/* The head of a volume and the names of its objects, without making anything */
IMPORT ER   bk_rest_peek( BK_SRC src, void *arg, BK_VOLHEAD *vh, char (*names)[BK_NAME_MAX],
			  INT max, INT *p_n );

#endif /* __BK_H__ */
