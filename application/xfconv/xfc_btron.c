/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_btron.c
 *	ファイル変換: a disk of the BTRON file system (design 11.9)
 *
 *	A BTRON CD-ROM -- an ISO 9660 disc whose volume partition descriptor
 *	names the image of a BTRON hard disk partition -- or a disk or
 *	partition that is that image itself, is read here through the disk's
 *	object, its blocks being record 1 (lib/libbtfs). Nothing is mounted
 *	and nothing is written to it.
 *
 *	Its window shows the 実身 that a 実身 links to, the root file's
 *	first, in the order of its link records: a 実身 that links to others
 *	is shown as one to go into. A 実身 taken in brings every 実身 it
 *	links to, in turn, with it: they are made one stored BTRON archive
 *	(btfs_book) and taken out by the code that takes an archive out
 *	(application/archive/unp_obj.c), so that their TAD records become
 *	xmlTAD, the links join the new 実身 together, the 実行機能付箋
 *	choose the programs that open them, and the records of other types
 *	are kept as they are.
 */

#include "xfc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/btfs.h>
#include <ts/sysdef.h>
#include "../archive/unp.h"

#define CHUNK		32768		/* a read of the disk: a multiple of any block size */
#define NCHUNK		8
#define PROBE_MAX	16		/* disks whose answer is kept */

struct xfcbt {
	ID		key;		/* the disk's object, for reading its blocks */
	D		dsize;		/* the disk's bytes */
	UB		*buf[NCHUNK];	/* the chunks read last */
	D		at[NCHUNK];
	INT		next;
	BTFS		fs;
	BOOL		cd;
};

/* ---------------------------------------------------------------- the disk's bytes */

LOCAL ER chunk_read( XFCBT *b, D at, UB *buf )
{
	SZ	asz = 0;
	SZ	n = ( b->dsize - at < CHUNK ) ? (SZ)( b->dsize - at ) : CHUNK;
	ER	er;

	if ( at < 0 || at >= b->dsize ) return E_PAR;
	er = ob_rea_rec(b->key, 1, at, buf, n, &asz);
	if ( er < E_OK ) return er;
	if ( asz != n ) return E_IO;
	if ( n < CHUNK ) memset(buf + n, 0, CHUNK - n);
	return E_OK;
}

LOCAL ER disk_read( void *ctx, D off, void *buf, UINT len )
{
	XFCBT	*b = (XFCBT *)ctx;
	UB	*o = (UB *)buf;

	if ( off < 0 || off + len > b->dsize ) return E_PAR;
	while ( len > 0 ) {
		D	base = off & ~(D)( CHUNK - 1 );
		UINT	in = (UINT)( off - base ), n;
		INT	i;

		for ( i = 0; i < NCHUNK; i++ ) {
			if ( b->buf[i] != NULL && b->at[i] == base ) break;
		}
		if ( i == NCHUNK ) {
			i = b->next;
			b->next = ( b->next + 1 ) % NCHUNK;
			if ( b->buf[i] == NULL ) b->buf[i] = (UB *)malloc(CHUNK);
			if ( b->buf[i] == NULL ) return E_NOMEM;
			b->at[i] = -1;
			if ( chunk_read(b, base, b->buf[i]) < E_OK ) return E_IO;
			b->at[i] = base;
		}
		n = CHUNK - in;
		if ( n > len ) n = len;
		memcpy(o, b->buf[i] + in, n);
		o += n;
		off += n;
		len -= n;
	}
	return E_OK;
}

LOCAL void bt_free( XFCBT *b )
{
	INT	i;

	if ( b == NULL ) return;
	btfs_close(&b->fs);
	for ( i = 0; i < NCHUNK; i++ ) free(b->buf[i]);
	if ( b->key > 0 ) ob_cls_obj(b->key);
	free(b);
}

/* The disk opened and its file system found: on a CD-ROM's partition, or from its start */
LOCAL XFCBT *bt_new( CONST TS_UUID *disk, ER *p_er )
{
	XFCBT	*b = (XFCBT *)calloc(1, sizeof(XFCBT));
	T_OBREC	rc[2];
	INT	nr = 0;
	D	off = 0, len = 0;
	ER	er;

	if ( b == NULL ) {
		*p_er = E_NOMEM;
		return NULL;
	}
	b->key = ob_opn_obj(disk, OB_OP_R);
	if ( b->key <= 0 ) {
		*p_er = (ER)b->key;
		b->key = 0;
		bt_free(b);
		return NULL;
	}
	if ( ob_lst_rec(b->key, rc, 2, &nr) >= E_OK && nr >= 2 ) b->dsize = (D)rc[1].size;
	if ( b->dsize <= 0 ) {
		*p_er = E_NOMDA;
		bt_free(b);
		return NULL;
	}
	if ( btfs_cd_find(disk_read, b, &off, &len, NULL) >= E_OK ) {
		b->cd = TRUE;
		if ( off + len > b->dsize ) len = b->dsize - off;
	} else {
		off = 0;
		len = b->dsize;
	}
	er = btfs_open(&b->fs, disk_read, b, off, len);
	if ( er < E_OK ) {
		*p_er = er;
		bt_free(b);
		return NULL;
	}
	*p_er = E_OK;
	return b;
}

/* ---------------------------------------------------------------- what the list of the media asks */

typedef struct {
	TS_UUID	disk;
	D	size;
	BOOL	is;
	char	name[64];
} PROBED;

LOCAL PROBED	probed[PROBE_MAX];
LOCAL INT	nprobed;

EXPORT BOOL xfc_bt_probe( CONST TS_UUID *disk, D size, char *name, INT max )
{
	XFCBT	*b;
	PROBED	*p = NULL;
	INT	i;
	ER	er;

	name[0] = 0;
	for ( i = 0; i < nprobed; i++ ) {
		if ( memcmp(&probed[i].disk, disk, sizeof(*disk)) == 0 ) {
			p = &probed[i];
			if ( p->size == size ) {
				snprintf(name, max, "%s", p->name);
				return p->is;
			}
		}
	}
	if ( p == NULL ) {
		p = &probed[( nprobed < PROBE_MAX ) ? nprobed++ : PROBE_MAX - 1];
		p->disk = *disk;
	}
	p->size = size;
	p->is = FALSE;
	p->name[0] = 0;
	if ( size <= 0 ) return FALSE;
	b = bt_new(disk, &er);
	if ( b != NULL ) {
		p->is = TRUE;
		snprintf(p->name, sizeof(p->name), "%s", b->fs.uname);
		snprintf(name, max, "%s", p->name);
		bt_free(b);
	}
	return p->is;
}

EXPORT void xfc_bt_forget( CONST TS_UUID *disk )
{
	INT	i;

	for ( i = 0; i < nprobed; i++ ) {
		if ( memcmp(&probed[i].disk, disk, sizeof(*disk)) == 0 ) probed[i].size = -1;
	}
}

/* ---------------------------------------------------------------- a window on it */

EXPORT ER xfc_bt_open( XWIN *w, CONST TS_UUID *disk )
{
	ER	er;
	char	s[200];

	w->bt = bt_new(disk, &er);
	if ( w->bt == NULL ) return er;
	snprintf(s, sizeof(s), "btron %s %s block %u %s%s", w->bt->fs.uname, w->bt->cd ? "cdrom" : "disk",
		 w->bt->fs.bsize, w->bt->fs.le ? "semi-standard" : "standard",
		 ( w->bt->fs.format == BTFS_FMT_STD ) ? "" : " extended");
	xfc_log(s);
	return E_OK;
}

EXPORT void xfc_bt_close( XWIN *w )
{
	bt_free(w->bt);
	w->bt = NULL;
}

EXPORT CONST char *xfc_bt_name( XWIN *w )
{
	return ( w->bt != NULL ) ? w->bt->fs.uname : "";
}

/* The 実身 a path of file IDs ("3/17") ends at; the root for an empty one */
EXPORT INT xfc_bt_fid( CONST UB *dir )
{
	CONST char	*p = (CONST char *)dir, *last = p;

	for ( ; *p != 0; p++ ) {
		if ( *p == '/' ) last = p + 1;
	}
	return ( *last != 0 ) ? atoi(last) : 0;
}

/* The names of the 実身 a path of file IDs goes through, "資料/大きな実身" */
EXPORT void xfc_bt_names( XWIN *w, CONST UB *dir, char *out, INT max )
{
	CONST char	*p = (CONST char *)dir;
	INT		n = 0;

	out[0] = 0;
	while ( w->bt != NULL && *p != 0 && n < max - 1 ) {
		BTFSFILE	f;
		INT		fid = atoi(p);

		if ( btfs_file(&w->bt->fs, (UINT)fid, &f) >= E_OK ) {
			n += snprintf(out + n, max - n, "%s%s", n > 0 ? "/" : "", f.uname);
			if ( n >= max ) n = max - 1;	/* a long path: it ends where the room does */
			btfs_file_free(&f);
		}
		while ( *p != 0 && *p != '/' ) p++;
		if ( *p == '/' ) p++;
	}
}

EXPORT ER xfc_bt_list( XWIN *w, INT fid, XFC_BTPUT put, void *arg )
{
	BTFSFILE	f, t;
	INT		r, k;
	ER		er;

	if ( w->bt == NULL ) return E_OBJ;
	er = btfs_file(&w->bt->fs, (UINT)fid, &f);
	if ( er < E_OK ) return er;
	for ( r = 0; r < f.nrec; r++ ) {
		D	size = 0;
		INT	q;

		if ( f.rec[r].type != 0 ) continue;
		k = btfs_link_target(&w->bt->fs, &f.rec[r]);
		if ( k < 0 || btfs_file(&w->bt->fs, (UINT)k, &t) < E_OK ) continue;
		for ( q = 0; q < t.nrec; q++ ) size += t.rec[q].size;
		if ( put(arg, (CONST UB *)t.uname, k, (BOOL)( t.nlink > 0 ), size) < 0 ) {
			btfs_file_free(&t);
			break;
		}
		btfs_file_free(&t);
	}
	btfs_file_free(&f);
	return E_OK;
}

/* ---------------------------------------------------------------- the ISO 9660 side */

/*
 * The files of a CD-ROM beside its BTRON partition, as a tree that
 * libxfu reads like the files of a mounted disk: paths are from the
 * ISO 9660 root, the partition's own image file is left out, and
 * nothing can be written.
 */
#define ISO_OPEN	4		/* files open at once */

typedef struct {
	BOOL	used;
	UINT	extent, size, at;
} ISOFILE;

LOCAL ISOFILE	isof[ISO_OPEN];

/* Whether an entry is the partition's own image */
LOCAL BOOL iso_is_part( XFCBT *b, CONST BTFS_ISOENT *e )
{
	return (BOOL)( !e->dir && (D)e->extent * BTFS_CD_BLOCK == b->fs.part );
}

LOCAL D iso_time( CONST UB *d )
{
	UB	s[40];
	INT	q = (B)d[6], h, m;

	if ( d[1] < 1 || d[1] > 12 || d[2] < 1 ) return 0;
	h = ( q < 0 ? -q : q ) / 4;
	m = ( ( q < 0 ? -q : q ) % 4 ) * 15;
	snprintf((char *)s, sizeof(s), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", 1900 + d[0], d[1], d[2], d[3], d[4],
		 d[5], ( q < 0 ) ? '-' : '+', h, m);
	return xfu_time_parse(s);
}

LOCAL INT iso_open( void *ctx, CONST UB *path, UINT mode )
{
	XFCBT		*b = (XFCBT *)ctx;
	BTFS_ISOENT	e;
	INT		h;

	if ( mode != XFU_O_READ ) return E_RONLY;
	if ( btfs_iso_find(disk_read, b, (CONST char *)path, &e) < E_OK || e.dir || iso_is_part(b, &e) ) return E_NOEXS;
	for ( h = 0; h < ISO_OPEN && isof[h].used; h++ ) ;
	if ( h == ISO_OPEN ) return E_LIMIT;
	isof[h].used = TRUE;
	isof[h].extent = e.extent;
	isof[h].size = e.size;
	isof[h].at = 0;
	return h;
}

LOCAL INT iso_read( void *ctx, INT h, void *buf, SZ n )
{
	XFCBT	*b = (XFCBT *)ctx;
	ISOFILE	*f;

	if ( h < 0 || h >= ISO_OPEN || !isof[h].used ) return E_PAR;
	f = &isof[h];
	if ( n > (SZ)( f->size - f->at ) ) n = (SZ)( f->size - f->at );
	if ( n <= 0 ) return 0;
	if ( disk_read(b, (D)f->extent * BTFS_CD_BLOCK + f->at, buf, (UINT)n) < E_OK ) return E_IO;
	f->at += (UINT)n;
	return (INT)n;
}

LOCAL INT iso_write( void *ctx, INT h, CONST void *buf, SZ n )
{
	(void)ctx;
	(void)h;
	(void)buf;
	(void)n;
	return E_RONLY;
}

LOCAL ER iso_close( void *ctx, INT h )
{
	(void)ctx;
	if ( h < 0 || h >= ISO_OPEN ) return E_PAR;
	isof[h].used = FALSE;
	return E_OK;
}

typedef struct {
	XFCBT		*b;
	XFU_ENTCB	cb;
	void		*arg;
} ISOLIST;

LOCAL void iso_ent( CONST BTFS_ISOENT *e, T_XFUENT *x )
{
	memset(x, 0, sizeof(*x));
	snprintf((char *)x->name, sizeof(x->name), "%s", e->name);
	x->kind = e->dir ? XFU_K_DIR : XFU_K_FILE;
	x->size = e->dir ? -1 : (D)e->size;
	x->mtime = iso_time(e->date);
}

LOCAL INT iso_each( void *arg, CONST BTFS_ISOENT *e )
{
	ISOLIST	*l = (ISOLIST *)arg;
	T_XFUENT x;

	if ( iso_is_part(l->b, e) ) return 0;
	iso_ent(e, &x);
	return l->cb(l->arg, &x);
}

LOCAL ER iso_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	XFCBT		*b = (XFCBT *)ctx;
	BTFS_ISOENT	d;
	ISOLIST		l;
	ER		er;

	er = btfs_iso_find(disk_read, b, (CONST char *)path, &d);
	if ( er < E_OK ) return er;
	if ( !d.dir ) return E_PAR;
	l.b = b;
	l.cb = cb;
	l.arg = arg;
	return btfs_iso_list(disk_read, b, &d, iso_each, &l);
}

LOCAL ER iso_stat( void *ctx, CONST UB *path, T_XFUENT *x )
{
	XFCBT		*b = (XFCBT *)ctx;
	BTFS_ISOENT	e;
	ER		er;

	er = btfs_iso_find(disk_read, b, (CONST char *)path, &e);
	if ( er < E_OK ) return er;
	if ( iso_is_part(b, &e) ) return E_NOEXS;
	iso_ent(&e, x);
	if ( path[0] == 0 ) x->name[0] = 0;
	return E_OK;
}

LOCAL ER iso_mkdir( void *ctx, CONST UB *path )
{
	(void)ctx;
	(void)path;
	return E_RONLY;
}

/* The entries of the root that are shown, counted */
typedef struct {
	XFCBT	*b;
	INT	n;
} ISOCOUNT;

LOCAL INT iso_count( void *arg, CONST BTFS_ISOENT *e )
{
	ISOCOUNT *c = (ISOCOUNT *)arg;

	if ( !iso_is_part(c->b, e) ) c->n++;
	return 0;
}

EXPORT INT xfc_bt_iso_tree( XWIN *w, T_XFUTREE *t )
{
	BTFS_ISOENT	root;
	ISOCOUNT	c;

	memset(t, 0, sizeof(*t));
	if ( w->bt == NULL || !w->bt->cd || btfs_iso_root(disk_read, w->bt, &root) < E_OK ) return 0;
	t->ctx = w->bt;
	t->open = iso_open;
	t->read = iso_read;
	t->write = iso_write;
	t->close = iso_close;
	t->list = iso_list;
	t->stat = iso_stat;
	t->mkdir = iso_mkdir;
	t->utime = NULL;
	c.b = w->bt;
	c.n = 0;
	(void)btfs_iso_list(disk_read, w->bt, &root, iso_count, &c);
	return c.n;
}

/* ---------------------------------------------------------------- taken in */

LOCAL XFC_BTPROG	prog_fn;

LOCAL void progress( UNPJOB *j, INT done, INT total )
{
	(void)j;
	if ( prog_fn != NULL ) prog_fn(done, total);
}

EXPORT ER xfc_bt_import( XWIN *w, INT fid, CONST TS_UUID *into, TS_UUID *p_top, INT *p_objs, XFC_BTPROG prog,
			 char *err, INT errmax )
{
	UNPJOB	*j;
	UB	*raw = NULL;
	UINT	len = 0;
	INT	n = 0;
	ER	er;

	*p_objs = 0;
	err[0] = 0;
	if ( w->bt == NULL ) return E_OBJ;
	j = (UNPJOB *)calloc(1, sizeof(UNPJOB));
	if ( j == NULL ) return E_NOMEM;
	er = btfs_book(&w->bt->fs, (UINT)fid, &raw, &len, &n, err, errmax);
	if ( er >= E_OK ) er = bpk_parse(&j->arc, raw, len, err, errmax);
	j->raw = raw;
	j->rawlen = len;
	if ( er >= E_OK ) {
		snprintf(j->name, sizeof(j->name), "%s", j->arc.file[0].name);
		snprintf(j->maker, sizeof(j->maker), "ファイル変換（%s「%s」）", w->bt->cd ? "CD-ROM" : "ディスク",
			 w->bt->fs.uname);
		prog_fn = prog;
		er = unp_job_run(j, into, progress);
		prog_fn = NULL;
		if ( er < E_OK ) snprintf(err, errmax, "%s", j->err);
	}
	if ( er >= E_OK ) {
		*p_top = j->root;
		*p_objs = (INT)j->arc.nfiles;
	}
	unp_job_free(j);
	free(j);
	return er;
}
