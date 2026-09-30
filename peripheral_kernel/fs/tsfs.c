/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfs.c
 *	Real objects, stage one: TSFS-on-FAT (design 11.9).
 *
 *	The store is a directory of a mounted file system holding the same
 *	files the TADjs Desktop writes, so the card can be read by either.
 *	The files are the record of truth: how many records an object has
 *	and what type each one is, is decided by which files exist, not by
 *	a list inside the metadata. That keeps the two in step without
 *	rewriting the metadata on every change.
 *
 *	The metadata is kept verbatim. Only the two items the store itself
 *	needs, the name and the reference count, are read out of it and
 *	written back, with a scanner that looks at the keys of the outer
 *	object only.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tsfsobj.h>
#include <ts/json.h>
#include <ts/proc.h>
#include <tk/smem.h>

#define TSFS_MAX_VOL	4
#define TSFS_MAX_OD	32
#define TSFS_JSON_MAX	16384		/* bytes of metadata handled at a time */
#define TSFS_PATH_MAX	256		/* the store's own paths: its root and a UUID's names */

/*
 * Each volume has a lock of its own, taken shared by what only reads a
 * native volume and whole by everything else (design 11.10). Readers
 * count themselves under `mtxid`; the first to come takes `wsem`, which
 * a writer holds alone, and the last to go gives it back.
 */
typedef struct {
	ID	mtxid;		/* guards `readers` */
	ID	wsem;		/* the volume: held by a writer, or by the readers */
	INT	readers;
	ID	bvol;		/* the native volume, 0 on the FAT store */
	UB	root[TSFS_PATH_MAX];	/* "/boot/TSFS" */
	INT	rootlen;
	UINT	flags;
	BOOL	used;
} TSFSVOL;

typedef struct {
	TSFSVOL	*vol;
	TS_UUID	uuid;
	UINT	omode;
	BOOL	used;
} TSFSOBJ;

LOCAL TSFSVOL	tsfs_vol[TSFS_MAX_VOL];
LOCAL TSFSOBJ	tsfs_od[TSFS_MAX_OD];
LOCAL ID	tsfs_mtxid = 0;		/* the two tables */

LOCAL void vol_lock( TSFSVOL *v )
{
	tk_wai_sem(v->wsem, 1, TMO_FEVR);
}

LOCAL void vol_unlock( TSFSVOL *v )
{
	tk_sig_sem(v->wsem, 1);
}

/* Shared on a native volume; the FAT store is taken whole even to read */
LOCAL void vol_rlock( TSFSVOL *v )
{
	if ( v->bvol <= 0 ) {
		vol_lock(v);
		return;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( v->readers++ == 0 ) {
		tk_wai_sem(v->wsem, 1, TMO_FEVR);
	}
	tk_unl_mtx(v->mtxid);
}

LOCAL void vol_runlock( TSFSVOL *v )
{
	if ( v->bvol <= 0 ) {
		vol_unlock(v);
		return;
	}
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( --v->readers == 0 ) {
		tk_sig_sem(v->wsem, 1);
	}
	tk_unl_mtx(v->mtxid);
}

LOCAL CONST char tsfs_default_json[] =
	"{\"name\":\"\",\"refCount\":0,\"recordCount\":0}";

/* ---------------------------------------------------------------- strings */

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;
	while ( s[n] != '\0' ) n++;
	return n;
}

LOCAL INT s_put( UB *dst, INT pos, INT max, CONST char *src )
{
	while ( *src != '\0' && pos < max - 1 ) {
		dst[pos++] = (UB)*src++;
	}
	dst[pos] = '\0';
	return pos;
}

LOCAL INT s_put_num( UB *dst, INT pos, INT max, INT v )
{
	char	tmp[12];
	INT	i = 0, neg = 0;

	if ( v < 0 ) { neg = 1; v = -v; }
	do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while ( v > 0 );
	if ( neg && pos < max - 1 ) dst[pos++] = '-';
	while ( i > 0 && pos < max - 1 ) dst[pos++] = (UB)tmp[--i];
	dst[pos] = '\0';
	return pos;
}

/*
 * Path of one file of an object: <root>/<uuid><suffix>
 */
LOCAL ER obj_path( TSFSVOL *v, CONST TS_UUID *uuid, CONST char *suffix, UB *out, INT max )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n;
	ER	er;

	er = ts_uuid_to_str(uuid, us, sizeof(us));
	if ( er < E_OK ) {
		return EX_INVAL;
	}
	n = s_put(out, 0, max, (CONST char *)v->root);
	n = s_put(out, n, max, "/");
	n = s_put(out, n, max, us);
	n = s_put(out, n, max, suffix);

	return ( n < max - 1 ) ? EX_OK : EX_NAMETOOLONG;
}

/* "_3.xtad" and the like */
LOCAL ER rec_path( TSFSVOL *v, CONST TS_UUID *uuid, INT recno, UINT rectype, UB *out, INT max )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n;
	ER	er;

	er = ts_uuid_to_str(uuid, us, sizeof(us));
	if ( er < E_OK ) {
		return EX_INVAL;
	}
	n = s_put(out, 0, max, (CONST char *)v->root);
	n = s_put(out, n, max, "/");
	n = s_put(out, n, max, us);
	n = s_put(out, n, max, "_");
	n = s_put_num(out, n, max, recno);
	n = s_put(out, n, max, ( rectype == TSFS_REC_BIN ) ? ".bin" : ".xtad");

	return ( n < max - 1 ) ? EX_OK : EX_NAMETOOLONG;
}

/* ---------------------------------------------------------------- metadata */

/*
 * Find a key of the outer object. Returns the offset of its value, or -1.
 * Only the outer object's own members are looked at, so a key of the
 * same name inside a nested object is not taken by mistake.
 */
EXPORT INT knl_json_find( CONST UB *j, INT len, CONST char *key )
{
	T_JSON	root, v;
	INT	at = 0;

	while ( at < len && j[at] != '{' ) {
		at++;
	}
	if ( js_span(j, len, at, &root) < E_OK || js_get(&root, key, &v) < E_OK ) {
		return -1;
	}
	return (INT)( v.s - j );
}

/*
 * The value of a key that holds an object: its offset and length. The
 * range starts at the opening brace, so the same lookup can be run on
 * it to reach a key one level further in.
 */
LOCAL BOOL json_sub( CONST UB *j, INT len, CONST char *key, INT *p_off, INT *p_len )
{
	INT	p = knl_json_find(j, len, key);
	T_JSON	v;

	if ( p < 0 || j[p] != '{' || js_span(j, len, p, &v) < E_OK ) {
		return FALSE;
	}
	*p_off = p;
	*p_len = v.len;
	return TRUE;
}

EXPORT INT knl_json_get_num( CONST UB *j, INT len, CONST char *key, INT dflt )
{
	INT	p = knl_json_find(j, len, key);
	INT	v = 0, neg = 0;

	if ( p < 0 || p >= len ) {
		return dflt;
	}
	if ( j[p] == '-' ) { neg = 1; p++; }
	if ( p >= len || j[p] < '0' || j[p] > '9' ) {
		return dflt;
	}
	while ( p < len && j[p] >= '0' && j[p] <= '9' ) {
		v = v * 10 + (j[p] - '0');
		p++;
	}

	return neg ? -v : v;
}

EXPORT void knl_json_get_str( CONST UB *j, INT len, CONST char *key, UB *out, INT max )
{
	INT	p = knl_json_find(j, len, key);
	T_JSON	v;

	out[0] = '\0';
	if ( p < 0 || j[p] != '"' || js_span(j, len, p, &v) < E_OK ) {
		return;
	}
	(void)js_str(&v, out, max);		/* what fits, when it does not all fit */
}

/*
 * Replace the value of a key with a number, or add the key when it is
 * not there. Returns the new length, or a negative error.
 */
EXPORT INT knl_json_set_num( UB *j, INT len, INT max, CONST char *key, INT value )
{
	UB	num[16];
	INT	p = knl_json_find(j, len, key);
	INT	end, nlen, klen, i, shift;

	nlen = s_put_num(num, 0, sizeof(num), value);

	if ( p < 0 ) {
		/* add it just after the opening brace */
		klen = s_len(key);
		shift = klen + 3 + nlen + ( len > 2 ? 1 : 0 );	/* "key":n, */
		if ( len + shift >= max ) return EX_NOSPC;
		for ( i = len; i > 1; i-- ) j[i + shift - 1] = j[i - 1];
		p = 1;
		j[p++] = '"';
		for ( i = 0; i < klen; i++ ) j[p++] = (UB)key[i];
		j[p++] = '"';
		j[p++] = ':';
		for ( i = 0; i < nlen; i++ ) j[p++] = num[i];
		if ( len > 2 ) j[p++] = ',';
		return len + shift;
	}

	/* the old value runs to the next comma or closing brace */
	end = p;
	if ( end < len && j[end] == '-' ) end++;
	while ( end < len && j[end] >= '0' && j[end] <= '9' ) end++;
	if ( end == p ) {
		return EX_INVAL;		/* not a number */
	}
	shift = nlen - (end - p);
	if ( len + shift >= max ) return EX_NOSPC;
	if ( shift > 0 ) {
		for ( i = len; i > end; i-- ) j[i + shift - 1] = j[i - 1];
	} else if ( shift < 0 ) {
		for ( i = end; i < len; i++ ) j[i + shift] = j[i];
	}
	for ( i = 0; i < nlen; i++ ) j[p + i] = num[i];

	return len + shift;
}

/*
 * Replace the value of a string key.
 */
EXPORT INT knl_json_set_str( UB *j, INT len, INT max, CONST char *key, CONST UB *value )
{
	INT	p = knl_json_find(j, len, key);
	INT	end, vlen = 0, shift, i;

	while ( value[vlen] != '\0' ) vlen++;
	if ( p < 0 || p >= len || j[p] != '"' ) {
		return EX_INVAL;
	}
	end = p + 1;
	while ( end < len && j[end] != '"' ) {
		if ( j[end] == '\\' ) end++;
		end++;
	}
	if ( end >= len ) return EX_INVAL;
	end++;					/* past the closing quote */

	shift = (vlen + 2) - (end - p);
	if ( len + shift >= max ) return EX_NOSPC;
	if ( shift > 0 ) {
		for ( i = len; i > end; i-- ) j[i + shift - 1] = j[i - 1];
	} else if ( shift < 0 ) {
		for ( i = end; i < len; i++ ) j[i + shift] = j[i];
	}
	j[p] = '"';
	for ( i = 0; i < vlen; i++ ) j[p + 1 + i] = value[i];
	j[p + 1 + vlen] = '"';

	return len + shift;
}

/* ---------------------------------------------------------------- files */

/*
 * A file read for a caller whose buffer may be a process's memory. The
 * FAT driver hands a process's pages to the disk itself (knl_prc_dma),
 * in requests as large as for the kernel's own buffers, so the buffer
 * goes to fs_read and fs_write as it is. A process's read goes in
 * pieces of PRC_PIECE, and between them looks whether the process is
 * to end (knl_prc_ending): ended in the middle of reading a large
 * record, it is out of the store after one piece. A write goes whole,
 * so that no record is left half rewritten by a process that was ended.
 */
#define PRC_PIECE	( 256 * 1024 )

/* Whether the buffer is the kernel's own memory */
LOCAL BOOL kernel_buf( CONST void *buf )
{
	void	*pa = NULL;

	return ( ConvPhysicalAddress(buf, 1, &pa) > 0 );
}

LOCAL INT piece_read( INT fd, void *buf, SZ size )
{
	SZ	done = 0, want;
	INT	n;

	if ( size <= 0 || kernel_buf(buf) ) {
		return fs_read(fd, buf, size);
	}
	do {
		want = ( size - done > PRC_PIECE ) ? PRC_PIECE : size - done;
		n = fs_read(fd, (UB *)buf + done, want);
		if ( n <= 0 ) {
			return ( done > 0 ) ? (INT)done : n;
		}
		done += n;
	} while ( n == (INT)want && done < size && !knl_prc_ending() );

	return (INT)done;
}

LOCAL INT read_file( CONST UB *path, UB *buf, INT max )
{
	INT	fd, n;

	fd = fs_open((CONST char *)path, O_RDONLY);
	if ( fd < 0 ) {
		return fd;
	}
	n = piece_read(fd, buf, max);
	fs_close(fd);

	return n;
}

LOCAL ER write_file( CONST UB *path, CONST UB *buf, INT len )
{
	INT	fd, n;

	fd = fs_open((CONST char *)path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return (ER)fd;
	}
	n = fs_write(fd, buf, len);
	fs_close(fd);

	return ( n == len ) ? EX_OK : EX_IO;
}

/*
 * Read the metadata of an object into a buffer the caller owns.
 */
LOCAL INT read_meta( TSFSVOL *v, CONST TS_UUID *uuid, UB *buf, INT max )
{
	UB	path[TSFS_PATH_MAX];
	ER	er;

	er = obj_path(v, uuid, ".json", path, sizeof(path));
	if ( er < EX_OK ) return er;

	return read_file(path, buf, max);
}

LOCAL ER write_meta( TSFSVOL *v, CONST TS_UUID *uuid, CONST UB *buf, INT len )
{
	UB	path[TSFS_PATH_MAX];
	ER	er;

	er = obj_path(v, uuid, ".json", path, sizeof(path));
	if ( er < EX_OK ) return er;

	return write_file(path, buf, len);
}

/*
 * The icon on the FAT store: the file {uuid}.ico beside the metadata.
 * Its size in *p_asize, at most `size` bytes of it into buf.
 */
LOCAL ER read_icon( TSFSVOL *v, CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	UB	path[TSFS_PATH_MAX];
	T_FSTAT	st;
	INT	n;

	if ( obj_path(v, uuid, ".ico", path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	if ( fs_stat((CONST char *)path, &st) < EX_OK ) {
		return E_NOEXS;
	}
	if ( p_asize != NULL ) *p_asize = (SZ)st.size;
	if ( size <= 0 ) {
		return E_OK;
	}
	n = read_file(path, buf, (INT)size);
	return ( n >= 0 ) ? E_OK : E_IO;
}

LOCAL ER write_icon( TSFSVOL *v, CONST TS_UUID *uuid, CONST UB *buf, SZ size )
{
	UB	path[TSFS_PATH_MAX];

	if ( obj_path(v, uuid, ".ico", path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	if ( size == 0 ) {
		(void)fs_unlink((CONST char *)path);
		return E_OK;
	}
	return ( write_file(path, buf, (INT)size) >= EX_OK ) ? E_OK : E_IO;
}

/*
 * The type of record 'recno', or a negative error when it is not there.
 */
LOCAL INT rec_type( TSFSVOL *v, CONST TS_UUID *uuid, INT recno, UD *p_size )
{
	UB	path[TSFS_PATH_MAX];
	T_FSTAT	st;
	UINT	t;

	for ( t = TSFS_REC_XTAD; t <= TSFS_REC_BIN; t++ ) {
		if ( rec_path(v, uuid, recno, t, path, sizeof(path)) < EX_OK ) continue;
		if ( fs_stat((CONST char *)path, &st) >= EX_OK ) {
			if ( p_size != NULL ) *p_size = st.size;
			return (INT)t;
		}
	}

	return EX_NOENT;
}

/* How many records an object has: the files decide */
LOCAL INT rec_count( TSFSVOL *v, CONST TS_UUID *uuid )
{
	INT	n = 0;

	while ( n < TSFS_MAX_REC && rec_type(v, uuid, n, NULL) >= 0 ) {
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------- volumes */

LOCAL TSFSVOL *vol_of( ID vol )
{
	if ( vol < 1 || vol > TSFS_MAX_VOL || !tsfs_vol[vol - 1].used ) {
		return NULL;
	}
	return &tsfs_vol[vol - 1];
}

LOCAL TSFSOBJ *od_of( ID od )
{
	if ( od < 1 || od > TSFS_MAX_OD || !tsfs_od[od - 1].used ) {
		return NULL;
	}
	return &tsfs_od[od - 1];
}

EXPORT ID ts_opn_vol( CONST char *path, UINT flags )
{
	T_CMTX	cmtx;
	T_CSEM	csem;
	TSFSVOL	*v = NULL;
	T_FSTAT	st;
	ID	bvol = 0;
	INT	i, len;

	if ( path == NULL ) {
		return E_PAR;
	}
	len = s_len(path);
	if ( len >= TSFS_PATH_MAX - TS_UUID_STRLEN - 16 ) {
		return E_PAR;			/* no room for the file names */
	}
	if ( tsfs_mtxid <= 0 ) {
		cmtx.exinf = NULL;
		cmtx.mtxatr = TA_TFIFO;
		tsfs_mtxid = tk_cre_mtx(&cmtx);
		if ( tsfs_mtxid <= 0 ) return (ER)tsfs_mtxid;
	}

	if ( (flags & TSFS_STORE_BLK) != 0 ) {
		/* the path names a block device, not a directory */
		bvol = ts_obj_mount(path);
		if ( bvol <= 0 ) {
			return bvol;
		}
	} else {
		/* the directory must be there; it is made when it is not */
		if ( fs_stat(path, &st) < EX_OK ) {
			if ( fs_mkdir(path) < EX_OK ) return E_NOEXS;
		} else if ( (st.mode & FS_IFMT) != FS_IFDIR ) {
			return E_OBJ;
		}
	}

	tk_loc_mtx(tsfs_mtxid, TMO_FEVR);
	for ( i = 0; i < TSFS_MAX_VOL; i++ ) {
		if ( !tsfs_vol[i].used ) { v = &tsfs_vol[i]; break; }
	}
	if ( v != NULL ) {
		cmtx.exinf = NULL;
		cmtx.mtxatr = TA_TFIFO;
		csem.exinf = NULL;
		csem.sematr = TA_TFIFO | TA_FIRST;
		csem.isemcnt = 1;
		csem.maxsem = 1;
		v->mtxid = tk_cre_mtx(&cmtx);
		v->wsem = ( v->mtxid > 0 ) ? tk_cre_sem(&csem) : E_NOMEM;
		if ( v->wsem <= 0 ) {
			if ( v->mtxid > 0 ) tk_del_mtx(v->mtxid);
			v = NULL;
		}
	}
	if ( v == NULL ) {
		tk_unl_mtx(tsfs_mtxid);
		if ( bvol > 0 ) ts_obj_unmount(bvol);
		return E_LIMIT;
	}
	v->readers = 0;
	s_put(v->root, 0, TSFS_PATH_MAX, path);
	v->rootlen = len;
	v->flags   = flags;
	v->bvol    = bvol;
	v->used    = TRUE;
	tk_unl_mtx(tsfs_mtxid);

	return (ID)(v - tsfs_vol) + 1;
}

EXPORT ER ts_cls_vol( ID vol )
{
	TSFSVOL	*v = vol_of(vol);
	ID	bvol;
	INT	i;

	if ( v == NULL ) {
		return E_ID;
	}
	tk_loc_mtx(tsfs_mtxid, TMO_FEVR);
	for ( i = 0; i < TSFS_MAX_OD; i++ ) {
		if ( tsfs_od[i].used && tsfs_od[i].vol == v ) {
			tk_unl_mtx(tsfs_mtxid);
			return E_OBJ;		/* an object is still open */
		}
	}
	bvol = v->bvol;
	v->bvol = 0;
	v->used = FALSE;
	tk_del_sem(v->wsem);
	tk_del_mtx(v->mtxid);
	tk_unl_mtx(tsfs_mtxid);

	return ( bvol > 0 ) ? ts_obj_unmount(bvol) : E_OK;
}

EXPORT ER ts_ref_vol( ID vol, T_RVOL *pk_rvol )
{
	TSFSVOL		*v = vol_of(vol);
	T_FSSTAT	vfs;
	TS_UUID		uuid;
	SYSTIM		tim;
	INT		cnt = 0, total = 0;
	ER		er;

	if ( v == NULL || pk_rvol == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return ts_obj_ref_vol(v->bvol, pk_rvol);
	}
	er = fs_statvfs((CONST char *)v->root, &vfs);
	if ( er < EX_OK ) {
		return E_IO;
	}
	/* count the objects */
	for (;;) {
		TS_UUID	buf[8];
		INT	got = 0;

		er = ts_lst_obj(vol, ( total == 0 ) ? NULL : &uuid, buf, 8, &got);
		if ( er < E_OK || got == 0 ) break;
		uuid = buf[got - 1];
		total += got;
		cnt = got;
		if ( cnt < 8 ) break;
	}

	pk_rvol->nobj   = total;
	pk_rvol->blocks = vfs.blocks;
	pk_rvol->bfree  = vfs.bfree;
	pk_rvol->bsize  = vfs.bsize;
	pk_rvol->time_valid = ( tk_get_tim(&tim) >= E_OK
			     && ((((UD)(UW)tim.hi << 32) | tim.lo) > 1000000000000ULL) );

	return E_OK;
}

/* ---------------------------------------------------------------- objects */

EXPORT ER ts_cre_obj( ID vol, CONST T_COBJ *pk_cobj, TS_UUID *p_uuid )
{
	TSFSVOL	*v = vol_of(vol);
	TS_UUID	uuid;
	UB	*json;
	INT	len;
	ER	er;

	if ( v == NULL || p_uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		CONST UB *j  = ( pk_cobj != NULL ) ? pk_cobj->json : NULL;
		INT	 jlen = ( pk_cobj != NULL ) ? (INT)pk_cobj->jsonsz : 0;

		vol_lock(v);
		er = ts_obj_create(v->bvol, j, jlen, p_uuid);
		vol_unlock(v);
		return er;
	}
	er = ts_gen_uuid(&uuid);
	if ( er < E_OK ) {
		return er;			/* the clock has not been set */
	}

	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) {
		return E_NOMEM;
	}
	if ( pk_cobj != NULL && pk_cobj->json != NULL && pk_cobj->jsonsz > 0 ) {
		if ( pk_cobj->jsonsz >= TSFS_JSON_MAX ) { Kfree(json); return E_PAR; }
		knl_memcpy(json, pk_cobj->json, (INT)pk_cobj->jsonsz);
		len = (INT)pk_cobj->jsonsz;
	} else {
		len = s_put(json, 0, TSFS_JSON_MAX, tsfs_default_json);
	}

	vol_lock(v);
	er = write_meta(v, &uuid, json, len);
	vol_unlock(v);

	Kfree(json);
	if ( er < EX_OK ) {
		return E_IO;
	}
	*p_uuid = uuid;

	return E_OK;
}

/* How many have the object open (the volume's lock, then the tables') */
LOCAL INT open_count( TSFSVOL *v, CONST TS_UUID *uuid )
{
	INT	i, n = 0;

	tk_loc_mtx(tsfs_mtxid, TMO_FEVR);
	for ( i = 0; i < TSFS_MAX_OD; i++ ) {
		if ( tsfs_od[i].used && tsfs_od[i].vol == v
		  && ts_uuid_cmp(&tsfs_od[i].uuid, uuid) == 0 ) {
			n++;
		}
	}
	tk_unl_mtx(tsfs_mtxid);
	return n;
}

EXPORT ER ts_del_obj( ID vol, CONST TS_UUID *uuid )
{
	TSFSVOL	*v = vol_of(vol);
	UB	path[TSFS_PATH_MAX];
	UB	*json;
	INT	i, len, refcnt;
	ER	er;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		/* one that is open goes when the last one closes it */
		vol_lock(v);
		er = ( open_count(v, uuid) > 0 ) ? ts_obj_orphan(v->bvol, uuid)
						  : ts_obj_delete(v->bvol, uuid);
		vol_unlock(v);
		return er;
	}
	vol_lock(v);

	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) { er = E_NOMEM; goto exit; }

	len = read_meta(v, uuid, json, TSFS_JSON_MAX);
	if ( len < 0 ) { er = E_NOEXS; goto exit; }
	refcnt = knl_json_get_num(json, len, "refCount", 0);
	if ( refcnt > 0 ) { er = E_OBJ; goto exit; }	/* still referred to */

	/* every record first, then the metadata */
	for ( i = 0; i < TSFS_MAX_REC; i++ ) {
		INT t = rec_type(v, uuid, i, NULL);
		if ( t < 0 ) break;
		if ( rec_path(v, uuid, i, (UINT)t, path, sizeof(path)) >= EX_OK ) {
			fs_unlink((CONST char *)path);
		}
	}
	if ( obj_path(v, uuid, ".ico", path, sizeof(path)) >= EX_OK ) {
		fs_unlink((CONST char *)path);
	}
	/* and the pictures and the rest beside the records */
	{
		UB	*names = (UB *)Kmalloc(2048);
		INT	cnt = 0, k, at = 0;

		if ( names != NULL && knl_tsfs_res_lst(vol, uuid, names, 2048, &cnt) >= E_OK ) {
			for ( k = 0; k < cnt; k++ ) {
				INT	l = 0;

				while ( names[at + l] != 0 ) l++;
				if ( obj_path(v, uuid, (CONST char *)names + at, path, sizeof(path)) >= EX_OK ) {
					fs_unlink((CONST char *)path);
				}
				at += l + 1;
			}
		}
		if ( names != NULL ) Kfree(names);
	}
	if ( obj_path(v, uuid, ".json", path, sizeof(path)) >= EX_OK ) {
		er = ( fs_unlink((CONST char *)path) >= EX_OK ) ? E_OK : E_IO;
	} else {
		er = E_IO;
	}

    exit:
	if ( json != NULL ) Kfree(json);
	vol_unlock(v);

	return er;
}

EXPORT ID ts_opn_obj( ID vol, CONST TS_UUID *uuid, UINT omode )
{
	TSFSVOL	*v = vol_of(vol);
	TSFSOBJ	*o = NULL;
	UB	path[TSFS_PATH_MAX];
	T_FSTAT	st;
	INT	i;
	ER	er;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		if ( ts_obj_exists(v->bvol, uuid) < E_OK ) {
			return E_NOEXS;
		}
	} else {
		er = obj_path(v, uuid, ".json", path, sizeof(path));
		if ( er < EX_OK ) return E_PAR;
		if ( fs_stat((CONST char *)path, &st) < EX_OK ) {
			return E_NOEXS;
		}
	}

	tk_loc_mtx(tsfs_mtxid, TMO_FEVR);
	for ( i = 0; i < TSFS_MAX_OD; i++ ) {
		TSFSOBJ *t = &tsfs_od[i];

		if ( !t->used ) { if ( o == NULL ) o = t; continue; }
		if ( t->vol != v || ts_uuid_cmp(&t->uuid, uuid) != 0 ) continue;
		/* writing is exclusive */
		if ( ((omode | t->omode) & TFO_WRITE) != 0
		  || ((omode | t->omode) & TFO_EXCL) != 0 ) {
			tk_unl_mtx(tsfs_mtxid);
			return E_BUSY;
		}
	}
	if ( o == NULL ) {
		tk_unl_mtx(tsfs_mtxid);
		return E_LIMIT;
	}
	o->vol   = v;
	o->uuid  = *uuid;
	o->omode = omode;
	o->used  = TRUE;
	tk_unl_mtx(tsfs_mtxid);

	return (ID)(o - tsfs_od) + 1;
}

EXPORT ER ts_od_uuid( ID od, TS_UUID *p_uuid )
{
	TSFSOBJ	*o = od_of(od);

	if ( o == NULL || p_uuid == NULL ) {
		return E_ID;
	}
	*p_uuid = o->uuid;

	return E_OK;
}

/*
 * On the native store the last to close an object finishes it: one that
 * was written has its link table made again, one deleted while open
 * goes (design 11.7).
 */
EXPORT ER ts_cls_obj( ID od )
{
	TSFSOBJ	*o = od_of(od);
	TSFSVOL	*v;
	TS_UUID	uuid;
	BOOL	wrote;

	if ( o == NULL ) {
		return E_ID;
	}
	v = o->vol;
	uuid = o->uuid;
	wrote = (BOOL)( ( o->omode & TFO_WRITE ) != 0 );
	o->used = FALSE;

	if ( v->bvol > 0 ) {
		vol_lock(v);
		if ( open_count(v, &uuid) == 0 ) {
			if ( ts_obj_reap(v->bvol, &uuid) < E_OK && wrote ) {
				(void)ts_obj_relink(v->bvol, &uuid);
			}
		}
		vol_unlock(v);
	}

	return E_OK;
}

EXPORT ER ts_ref_obj( ID vol, CONST TS_UUID *uuid, T_ROBJ *pk_robj )
{
	TSFSVOL	*v = vol_of(vol);
	UB	*json;
	INT	len, i;
	UD	size;
	ER	er = E_OK;

	if ( v == NULL || uuid == NULL || pk_robj == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		vol_rlock(v);
		er = ts_obj_ref(v->bvol, uuid, pk_robj);
		vol_runlock(v);
		return er;
	}
	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) {
		return E_NOMEM;
	}
	vol_lock(v);

	len = read_meta(v, uuid, json, TSFS_JSON_MAX);
	if ( len < 0 ) {
		er = E_NOEXS;
	} else {
		pk_robj->uuid   = *uuid;
		pk_robj->refcnt = knl_json_get_num(json, len, "refCount", 0);
		pk_robj->flags  = 0;		/* the FAT store holds none */
		knl_json_get_str(json, len, "name", pk_robj->name, TSFS_NAME_MAX);
		pk_robj->nrec = 0;
		pk_robj->size = 0;
		for ( i = 0; i < TSFS_MAX_REC; i++ ) {
			if ( rec_type(v, uuid, i, &size) < 0 ) break;
			pk_robj->nrec++;
			pk_robj->size += size;
		}
	}

	vol_unlock(v);
	Kfree(json);

	return er;
}

/*
 * Objects in UUID order, which is creation order. 'from' continues a
 * listing after that UUID; NULL starts at the beginning.
 */
EXPORT ER ts_lst_obj( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	TSFSVOL		*v = vol_of(vol);
	T_DIRENT	*de;
	TS_UUID		u;
	INT		fd, got = 0, k, cnt;
	ER		er = E_OK;

	if ( v == NULL || buf == NULL || n <= 0 || p_cnt == NULL ) {
		return E_PAR;
	}
	if ( v->bvol > 0 ) {
		vol_rlock(v);
		er = ts_obj_list(v->bvol, from, buf, n, p_cnt);
		vol_runlock(v);
		return er;
	}
	de = (T_DIRENT *)Kmalloc(sizeof(T_DIRENT) * 8);
	if ( de == NULL ) {
		return E_NOMEM;
	}
	vol_lock(v);

	fd = fs_open((CONST char *)v->root, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) { er = E_NOEXS; goto exit; }

	while ( (cnt = fs_getdents(fd, de, 8)) > 0 ) {
		for ( k = 0; k < cnt; k++ ) {
			INT	l = 0;

			while ( de[k].name[l] != '\0' ) l++;
			/* only "{uuid}.json" names an object */
			if ( l != TS_UUID_STRLEN + 5 ) continue;
			if ( de[k].name[TS_UUID_STRLEN] != '.' ) continue;
			if ( ts_str_to_uuid((CONST char *)de[k].name, &u) < E_OK ) continue;
			if ( from != NULL && ts_uuid_cmp(&u, from) <= 0 ) continue;

			/* keep the list sorted while it is built */
			if ( got < n ) {
				buf[got++] = u;
			} else if ( ts_uuid_cmp(&u, &buf[got - 1]) >= 0 ) {
				continue;
			} else {
				buf[got - 1] = u;
			}
			for ( l = got - 1; l > 0 && ts_uuid_cmp(&buf[l - 1], &buf[l]) > 0; l-- ) {
				TS_UUID t = buf[l - 1];
				buf[l - 1] = buf[l];
				buf[l] = t;
			}
		}
	}
	fs_close(fd);
	*p_cnt = got;

    exit:
	vol_unlock(v);
	Kfree(de);

	return er;
}

/*
 * Adjust the reference count held in the metadata.
 */
LOCAL ER refcnt_add( ID vol, CONST TS_UUID *uuid, INT delta )
{
	TSFSVOL	*v = vol_of(vol);
	UB	*json;
	INT	len, cur;
	ER	er = E_OK;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		vol_lock(v);
		er = ( delta > 0 ) ? ts_obj_link(v->bvol, uuid)
				   : ts_obj_unlink(v->bvol, uuid);
		vol_unlock(v);
		return er;
	}
	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) {
		return E_NOMEM;
	}
	vol_lock(v);

	len = read_meta(v, uuid, json, TSFS_JSON_MAX);
	if ( len < 0 ) { er = E_NOEXS; goto exit; }

	cur = knl_json_get_num(json, len, "refCount", 0) + delta;
	if ( cur < 0 ) { er = E_OBJ; goto exit; }

	len = knl_json_set_num(json, len, TSFS_JSON_MAX, "refCount", cur);
	if ( len < 0 ) { er = E_SYS; goto exit; }

	er = ( write_meta(v, uuid, json, len) >= EX_OK ) ? E_OK : E_IO;

    exit:
	vol_unlock(v);
	Kfree(json);

	return er;
}

EXPORT ER ts_lnk_obj( ID vol, CONST TS_UUID *uuid )
{
	return refcnt_add(vol, uuid, 1);
}

EXPORT ER ts_unl_obj( ID vol, CONST TS_UUID *uuid )
{
	return refcnt_add(vol, uuid, -1);
}

/* ---------------------------------------------------------------- garbage */

/*
 * The garbage list and the collecting of it belong to the native store:
 * the FAT store has no list of its own, and would have to walk every
 * object to build one (design 11.9).
 */
EXPORT ER ts_lst_gc( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_rlock(v);
	er = ts_obj_gc_list(v->bvol, from, buf, n, p_cnt);
	vol_runlock(v);

	return er;
}

EXPORT ER ts_gc_obj( ID vol, CONST TS_UUID *uuid )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(v);
	er = ts_obj_gc_one(v->bvol, uuid);
	vol_unlock(v);

	return er;
}

EXPORT ER ts_gc_vol( ID vol, INT *p_cnt )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(v);
	er = ts_obj_gc_all(v->bvol, p_cnt);
	vol_unlock(v);

	return er;
}

/* ---------------------------------------------------------------- transactions */

EXPORT ER ts_beg_trx( ID vol )
{
	TSFSVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;			/* the FAT store has no log */
	}

	return ts_obj_begin(v->bvol);
}

EXPORT ER ts_end_trx( ID vol, BOOL commit )
{
	TSFSVOL	*v = vol_of(vol);

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;
	}

	return ts_obj_end(v->bvol, commit);
}

/* ---------------------------------------------------------------- metadata */

EXPORT ER ts_get_met( ID od, UB *json, SZ size, SZ *p_asize )
{
	TSFSOBJ	*o = od_of(od);
	INT	n;

	if ( o == NULL || json == NULL ) {
		return E_ID;
	}
	if ( o->vol->bvol > 0 ) {
		ER er;

		vol_rlock(o->vol);
		er = ts_obj_get_meta(o->vol->bvol, &o->uuid, json, size, p_asize);
		vol_runlock(o->vol);
		return er;
	}
	vol_lock(o->vol);
	n = read_meta(o->vol, &o->uuid, json, (INT)size);
	vol_unlock(o->vol);

	if ( n < 0 ) {
		return E_IO;
	}
	if ( p_asize != NULL ) *p_asize = n;

	return E_OK;
}

EXPORT ER ts_set_met( ID od, CONST UB *json, SZ size )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL || json == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_set_meta(o->vol->bvol, &o->uuid, json, size);
		vol_unlock(o->vol);
		return er;
	}
	vol_lock(o->vol);
	er = write_meta(o->vol, &o->uuid, json, (INT)size);
	vol_unlock(o->vol);

	return ( er >= EX_OK ) ? E_OK : E_IO;
}

/* ---------------------------------------------------------------- the icon */

EXPORT ER ts_get_ico( ID od, UB *buf, SZ size, SZ *p_asize )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	if ( o->vol->bvol > 0 ) {
		vol_rlock(o->vol);
		er = ts_obj_get_icon(o->vol->bvol, &o->uuid, buf, size, p_asize);
		vol_runlock(o->vol);
		return er;
	}
	vol_lock(o->vol);
	er = read_icon(o->vol, &o->uuid, buf, size, p_asize);
	vol_unlock(o->vol);
	return er;
}

EXPORT ER ts_set_ico( ID od, CONST UB *buf, SZ size )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( size < 0 || size > TSFS_ICON_MAX || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	vol_lock(o->vol);
	er = ( o->vol->bvol > 0 ) ? ts_obj_set_icon(o->vol->bvol, &o->uuid, buf, size)
				 : write_icon(o->vol, &o->uuid, buf, size);
	vol_unlock(o->vol);
	return er;
}

/*
 * The flags of an object (design 11.5): editable, deletable, readable.
 * Only the native store holds them.
 */
EXPORT ER ts_set_flg_obj( ID od, UINT flags, UINT mask )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(o->vol);
	er = ts_obj_set_flags(o->vol->bvol, &o->uuid, flags, mask);
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_set_nam( ID od, CONST UB *name )
{
	TSFSOBJ	*o = od_of(od);
	UB	*json;
	INT	len;
	ER	er = E_OK;

	if ( o == NULL || name == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_set_name(o->vol->bvol, &o->uuid, name);
		vol_unlock(o->vol);
		return er;
	}
	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) {
		return E_NOMEM;
	}
	vol_lock(o->vol);

	len = read_meta(o->vol, &o->uuid, json, TSFS_JSON_MAX);
	if ( len < 0 ) { er = E_NOEXS; goto exit; }

	len = knl_json_set_str(json, len, TSFS_JSON_MAX, "name", name);
	if ( len < 0 ) { er = E_PAR; goto exit; }

	er = ( write_meta(o->vol, &o->uuid, json, len) >= EX_OK ) ? E_OK : E_IO;

    exit:
	vol_unlock(o->vol);
	Kfree(json);

	return er;
}

/* ---------------------------------------------------------------- records */

EXPORT ER ts_rea_rec( ID od, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	TSFSOBJ	*o = od_of(od);
	UB	path[TSFS_PATH_MAX];
	INT	t, fd, n;
	ER	er = E_OK;

	if ( o == NULL || buf == NULL || off < 0 ) {
		return E_ID;
	}
	if ( o->vol->bvol > 0 ) {
		vol_rlock(o->vol);
		er = ts_obj_rea_rec(o->vol->bvol, &o->uuid, recno, off,
				     buf, size, p_asize);
		vol_runlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	t = rec_type(o->vol, &o->uuid, recno, NULL);
	if ( t < 0 ) { er = E_NOEXS; goto exit; }
	if ( rec_path(o->vol, &o->uuid, recno, (UINT)t, path, sizeof(path)) < EX_OK ) {
		er = E_PAR;
		goto exit;
	}
	fd = fs_open((CONST char *)path, O_RDONLY);
	if ( fd < 0 ) { er = E_IO; goto exit; }
	if ( fs_lseek(fd, off, SEEK_SET_) != off ) {
		fs_close(fd);
		er = E_PAR;
		goto exit;
	}
	n = piece_read(fd, buf, size);
	fs_close(fd);
	if ( n < 0 ) { er = E_IO; goto exit; }
	if ( p_asize != NULL ) *p_asize = n;

    exit:
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_wri_rec( ID od, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	TSFSOBJ	*o = od_of(od);
	UB	path[TSFS_PATH_MAX];
	INT	t, fd, n;
	ER	er = E_OK;

	if ( o == NULL || buf == NULL || off < 0 ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_wri_rec(o->vol->bvol, &o->uuid, recno, off,
				     buf, size, p_asize);
		vol_unlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	t = rec_type(o->vol, &o->uuid, recno, NULL);
	if ( t < 0 ) { er = E_NOEXS; goto exit; }
	if ( rec_path(o->vol, &o->uuid, recno, (UINT)t, path, sizeof(path)) < EX_OK ) {
		er = E_PAR;
		goto exit;
	}
	fd = fs_open((CONST char *)path, O_RDWR);
	if ( fd < 0 ) { er = E_IO; goto exit; }
	if ( fs_lseek(fd, off, SEEK_SET_) != off ) {
		fs_close(fd);
		er = E_PAR;
		goto exit;
	}
	n = fs_write(fd, buf, size);
	fs_close(fd);
	if ( n != (INT)size ) { er = E_IO; goto exit; }
	if ( p_asize != NULL ) *p_asize = n;

    exit:
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_rpl_rec( ID od, INT recno, CONST void *buf, SZ size )
{
	TSFSOBJ	*o = od_of(od);
	SZ	asize = 0;
	ER	er;

	if ( o == NULL || ( buf == NULL && size > 0 ) || size < 0 ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_rpl_rec(o->vol->bvol, &o->uuid, recno, buf, size);
		vol_unlock(o->vol);
		return er;
	}
	er = ( size > 0 ) ? ts_wri_rec(od, recno, 0, buf, size, &asize) : E_OK;
	if ( er >= E_OK ) {
		er = ts_trn_rec(od, recno, (UD)size);
	}
	return er;
}

EXPORT ER ts_apd_rec( ID od, UINT rectype, INT *p_recno )
{
	TSFSOBJ	*o = od_of(od);
	UB	path[TSFS_PATH_MAX];
	INT	n, fd;
	ER	er = E_OK;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( rectype > TSFS_REC_BIN ) {
		return E_PAR;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_apd_rec(o->vol->bvol, &o->uuid, rectype, p_recno);
		vol_unlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	n = rec_count(o->vol, &o->uuid);
	if ( n >= TSFS_MAX_REC ) { er = E_LIMIT; goto exit; }

	if ( rec_path(o->vol, &o->uuid, n, rectype, path, sizeof(path)) < EX_OK ) {
		er = E_PAR;
		goto exit;
	}
	fd = fs_open((CONST char *)path, O_WRONLY | O_CREAT | O_EXCL);
	if ( fd < 0 ) { er = E_IO; goto exit; }
	fs_close(fd);
	if ( p_recno != NULL ) *p_recno = n;

    exit:
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_trn_rec( ID od, INT recno, UD newsize )
{
	TSFSOBJ	*o = od_of(od);
	UB	path[TSFS_PATH_MAX];
	INT	t, fd;
	ER	er = E_OK;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_trn_rec(o->vol->bvol, &o->uuid, recno, newsize);
		vol_unlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	t = rec_type(o->vol, &o->uuid, recno, NULL);
	if ( t < 0 ) { er = E_NOEXS; goto exit; }
	if ( rec_path(o->vol, &o->uuid, recno, (UINT)t, path, sizeof(path)) < EX_OK ) {
		er = E_PAR;
		goto exit;
	}
	fd = fs_open((CONST char *)path, O_RDWR);
	if ( fd < 0 ) { er = E_IO; goto exit; }
	er = ( fs_ftruncate(fd, newsize) >= EX_OK ) ? E_OK : E_IO;
	fs_close(fd);

    exit:
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_del_rec( ID od, INT recno )
{
	TSFSOBJ	*o = od_of(od);
	UB	path[TSFS_PATH_MAX];
	INT	t, n;
	ER	er = E_OK;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol > 0 ) {
		vol_lock(o->vol);
		er = ts_obj_del_rec(o->vol->bvol, &o->uuid, recno);
		vol_unlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	n = rec_count(o->vol, &o->uuid);
	if ( recno != n - 1 ) {
		er = E_PAR;		/* only the last record may go */
		goto exit;
	}
	t = rec_type(o->vol, &o->uuid, recno, NULL);
	if ( t < 0 ) { er = E_NOEXS; goto exit; }
	if ( rec_path(o->vol, &o->uuid, recno, (UINT)t, path, sizeof(path)) < EX_OK ) {
		er = E_PAR;
		goto exit;
	}
	er = ( fs_unlink((CONST char *)path) >= EX_OK ) ? E_OK : E_IO;

    exit:
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_lst_lnk( ID od, T_RLNK *buf, INT n, INT *p_cnt )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->vol->bvol <= 0 ) {
		return E_NOSPT;			/* FAT keeps no link table */
	}
	vol_rlock(o->vol);
	er = ts_obj_lst_lnk(o->vol->bvol, &o->uuid, buf, n, p_cnt);
	vol_runlock(o->vol);
	return er;
}

EXPORT ER ts_scr_vol( ID vol, BOOL data, UD *p_checked, UD *p_bad )
{
	TSFSVOL		*v = vol_of(vol);
	T_TSFSSCRUB	sc;
	ER		er;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(v);			/* what is on the medium stays still meanwhile */
	er = ts_obj_scrub(v->bvol, data, &sc);
	vol_unlock(v);
	if ( er >= E_OK ) {
		if ( p_checked != NULL ) *p_checked = sc.checked;
		if ( p_bad != NULL ) *p_bad = sc.bad;
	}
	return er;
}

/* The calls on the order of the records, for the native store */
LOCAL TSFSOBJ *order_obj( ID od, ER *p_er )
{
	TSFSOBJ	*o = od_of(od);

	*p_er = E_OK;
	if ( o == NULL ) {
		*p_er = E_ID;
	} else if ( (o->omode & TFO_WRITE) == 0 ) {
		*p_er = E_OACV;
	} else if ( o->vol->bvol <= 0 ) {
		*p_er = E_NOSPT;
	}
	return ( *p_er < E_OK ) ? NULL : o;
}

EXPORT ER ts_ins_rec( ID od, INT pos, UINT rt, UINT sub, INT *p_pos )
{
	TSFSOBJ	*o;
	ER	er;

	o = order_obj(od, &er);
	if ( o != NULL ) {
		vol_lock(o->vol);
		er = ts_obj_ins_rec(o->vol->bvol, &o->uuid, pos, rt, sub, p_pos);
		vol_unlock(o->vol);
	}
	return er;
}

EXPORT ER ts_mov_rec( ID od, INT from, INT to )
{
	TSFSOBJ	*o;
	ER	er;

	o = order_obj(od, &er);
	if ( o != NULL ) {
		vol_lock(o->vol);
		er = ts_obj_mov_rec(o->vol->bvol, &o->uuid, from, to);
		vol_unlock(o->vol);
	}
	return er;
}

EXPORT ER ts_set_rtp( ID od, INT recno, UINT rt, UINT sub )
{
	TSFSOBJ	*o;
	ER	er;

	o = order_obj(od, &er);
	if ( o != NULL ) {
		vol_lock(o->vol);
		er = ts_obj_set_rtp(o->vol->bvol, &o->uuid, recno, rt, sub);
		vol_unlock(o->vol);
	}
	return er;
}

EXPORT ER ts_lst_rec( ID od, T_RREC *buf, INT n, INT *p_cnt )
{
	TSFSOBJ	*o = od_of(od);
	INT	i, got = 0;
	UD	size;

	if ( o == NULL || buf == NULL || n <= 0 || p_cnt == NULL ) {
		return E_PAR;
	}
	if ( o->vol->bvol > 0 ) {
		ER er;

		vol_rlock(o->vol);
		er = ts_obj_lst_rec(o->vol->bvol, &o->uuid, buf, n, p_cnt);
		vol_runlock(o->vol);
		return er;
	}
	vol_lock(o->vol);

	for ( i = 0; i < TSFS_MAX_REC && got < n; i++ ) {
		INT t = rec_type(o->vol, &o->uuid, i, &size);

		if ( t < 0 ) break;
		buf[got].recno   = i;
		buf[got].rectype = (UINT)t;
		buf[got].size    = size;
		buf[got].rt      = ( t == TSFS_REC_XTAD ) ? 1 : 15;
		buf[got].sub     = 0;
		got++;
	}
	*p_cnt = got;

	vol_unlock(o->vol);

	return E_OK;
}

/* ---------------------------------------------------------------- resources */

/*
 * Resources by an open object: the native store only. On the FAT store
 * they are the files "{uuid}_N_M.{ext}", reached by name through the
 * calls for the object layer (knl_tsfs_res_*); the icon through
 * ts_get_ico/ts_set_ico on either.
 */
EXPORT ER ts_rea_res( ID od, INT recno, INT resno, void *buf, SZ size,
		      SZ *p_asize, UB *ext )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( o->vol->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_rlock(o->vol);
	er = ts_obj_rea_res(o->vol->bvol, &o->uuid, recno, resno, buf, size,
			    p_asize, ext);
	vol_runlock(o->vol);

	return er;
}

EXPORT ER ts_wri_res( ID od, INT recno, INT resno, CONST UB *ext,
		      CONST void *buf, SZ size )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL || buf == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(o->vol);
	er = ts_obj_wri_res(o->vol->bvol, &o->uuid, recno, resno, ext, buf, size);
	vol_unlock(o->vol);

	return er;
}

EXPORT ER ts_del_res( ID od, INT recno, INT resno )
{
	TSFSOBJ	*o = od_of(od);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( (o->omode & TFO_WRITE) == 0 ) {
		return E_OACV;
	}
	if ( o->vol->bvol <= 0 ) {
		return E_NOSPT;
	}
	vol_lock(o->vol);
	er = ts_obj_del_res(o->vol->bvol, &o->uuid, recno, resno);
	vol_unlock(o->vol);

	return er;
}

/* ---------------------------------------------------------------- for the object layer */

/*
 * Whether an object is on the volume, and its metadata read or written
 * without an open object: the file manager of the object layer
 * (design 18.6) asks for these to find which volume an object is on
 * and what protection it has, before anything is opened.
 */
/*
 * Resources on the FAT store: files beside the records, named by the
 * UUID and what follows it ("_0_1.png", ".ico"), as a TADjs Desktop
 * names them. The name has been checked by the caller not to be a
 * record or the metadata.
 */
/* ---------------------------------------------------------------- resources on a native volume */

/*
 * A resource's name as the native store keys it: a record number, a
 * number and the extension. "_2_1.png" is (2, 1, "png"); ".ico" is the
 * icon's, (TSFSO_ICON_REC, 0, "ico"); a name whose second part is not a
 * number, "_0_bgm.mp3", keeps that part whole as its extension under a
 * number made from it, below 0 so it meets no numbered one.
 */
LOCAL ER res_key( CONST UB *name, INT *p_rec, INT *p_res, UB *ext )
{
	CONST UB *rest;
	INT	i, k, rec = 0, res = 0;

	knl_memset(ext, 0, TSFSO_EXT_LEN);
	if ( name[0] == '.' ) {
		/* the icon, and nothing else is named so */
		if ( !( name[1] == 'i' && name[2] == 'c' && name[3] == 'o' && name[4] == 0 ) ) {
			return E_PAR;
		}
		rest = name + 1;
		rec = TSFSO_ICON_REC;
	} else if ( name[0] == '_' ) {
		for ( i = 1; name[i] >= '0' && name[i] <= '9'; i++ ) {
			rec = rec * 10 + ( name[i] - '0' );
		}
		if ( i == 1 || name[i] != '_' ) {
			return E_PAR;
		}
		i++;
		for ( k = i; name[k] >= '0' && name[k] <= '9'; k++ ) {
			res = res * 10 + ( name[k] - '0' );
		}
		if ( k > i && name[k] == '.' ) {
			rest = name + k + 1;
		} else {
			UW	h = 2166136261U;

			for ( k = i; name[k] != 0; k++ ) {
				h = ( h ^ name[k] ) * 16777619U;
			}
			res = -2 - (INT)( h & 0x3FFFFFFF );
			rest = name + i;
		}
	} else {
		return E_PAR;
	}
	for ( k = 0; rest[k] != 0; k++ ) {
		if ( k >= TSFSO_EXT_LEN - 1 ) {
			return E_PAR;			/* no room to keep it */
		}
		ext[k] = rest[k];
	}
	*p_rec = rec;
	*p_res = res;
	return E_OK;
}

/* And back: the name a native resource is listed by */
LOCAL INT res_name( CONST T_TSFSRES *r, UB *out, INT max )
{
	INT	n;

	if ( r->recno == TSFSO_ICON_REC ) {
		n = s_put(out, 0, max, ".");
	} else {
		n = s_put(out, 0, max, "_");
		n = s_put_num(out, n, max, r->recno);
		n = s_put(out, n, max, "_");
		if ( r->resno >= 0 ) {
			n = s_put_num(out, n, max, r->resno);
			n = s_put(out, n, max, ".");
		}
	}
	return s_put(out, n, max, (CONST char *)r->ext);
}

LOCAL ER nres_rea( TSFSVOL *v, CONST TS_UUID *uuid, CONST UB *name, D off,
		   void *buf, SZ size, SZ *p_asize )
{
	UB	ext[TSFSO_EXT_LEN];
	INT	rec, res;
	ER	er = res_key(name, &rec, &res, ext);

	if ( er < E_OK ) return er;
	vol_rlock(v);
	er = ts_obj_rea_res_at(v->bvol, uuid, rec, res, off, buf, size, p_asize, NULL);
	vol_runlock(v);
	return er;
}

LOCAL ER nres_wri( TSFSVOL *v, CONST TS_UUID *uuid, CONST UB *name, CONST void *buf, SZ size )
{
	UB	ext[TSFSO_EXT_LEN];
	INT	rec, res;
	ER	er = res_key(name, &rec, &res, ext);

	if ( er < E_OK ) return er;
	vol_lock(v);
	er = ts_obj_wri_res(v->bvol, uuid, rec, res, ext, buf, size);
	vol_unlock(v);
	return er;
}

LOCAL ER nres_del( TSFSVOL *v, CONST TS_UUID *uuid, CONST UB *name )
{
	UB	ext[TSFSO_EXT_LEN];
	INT	rec, res;
	ER	er = res_key(name, &rec, &res, ext);

	if ( er < E_OK ) return er;
	vol_lock(v);
	er = ts_obj_del_res(v->bvol, uuid, rec, res);
	vol_unlock(v);
	return er;
}

LOCAL ER nres_lst( TSFSVOL *v, CONST TS_UUID *uuid, UB *buf, SZ size, INT *p_cnt )
{
	T_TSFSRES	*r;
	UB		nm[TSFSO_EXT_LEN + 32];
	INT		n = 0, i, got = 0, k;
	SZ		at = 0;
	ER		er;

	r = (T_TSFSRES *)Kmalloc(sizeof(T_TSFSRES) * TSFSO_MAX_RES);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	vol_rlock(v);
	er = ts_obj_lst_res(v->bvol, uuid, r, TSFSO_MAX_RES, &n);
	vol_runlock(v);
	for ( i = 0; er >= E_OK && i < n; i++ ) {
		k = res_name(&r[i], nm, sizeof(nm));
		if ( buf != NULL && at + k + 1 <= size ) {
			knl_memcpy(buf + at, nm, k + 1);
			at += k + 1;
			got++;
		}
	}
	Kfree(r);
	if ( er >= E_OK && p_cnt != NULL ) {
		*p_cnt = got;
	}
	return er;
}

EXPORT ER knl_tsfs_res_rea( ID vol, CONST TS_UUID *uuid, CONST UB *name, D off,
			   void *buf, SZ size, SZ *p_asize )
{
	TSFSVOL	*v = vol_of(vol);
	UB	path[TSFS_PATH_MAX];
	INT	fd, n;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return nres_rea(v, uuid, name, off, buf, size, p_asize);
	}
	if ( obj_path(v, uuid, (CONST char *)name, path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	fd = fs_open((CONST char *)path, O_RDONLY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	if ( fs_lseek(fd, off, SEEK_SET_) != off ) {
		fs_close(fd);
		return E_PAR;
	}
	n = piece_read(fd, buf, size);
	fs_close(fd);
	if ( n < 0 ) {
		return E_IO;
	}
	if ( p_asize != NULL ) *p_asize = n;

	return E_OK;
}

EXPORT ER knl_tsfs_res_wri( ID vol, CONST TS_UUID *uuid, CONST UB *name,
			   CONST void *buf, SZ size )
{
	TSFSVOL	*v = vol_of(vol);
	UB	path[TSFS_PATH_MAX];
	INT	fd, n;
	SZ	done = 0;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return nres_wri(v, uuid, name, buf, size);
	}
	if ( obj_path(v, uuid, (CONST char *)name, path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	fd = fs_open((CONST char *)path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return E_IO;
	}
	n = fs_write(fd, buf, size);
	fs_close(fd);
	done = ( n > 0 ) ? n : 0;

	return ( done == size ) ? E_OK : E_IO;
}

EXPORT ER knl_tsfs_res_del( ID vol, CONST TS_UUID *uuid, CONST UB *name )
{
	TSFSVOL	*v = vol_of(vol);
	UB	path[TSFS_PATH_MAX];

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return nres_del(v, uuid, name);
	}
	if ( obj_path(v, uuid, (CONST char *)name, path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	return ( fs_unlink((CONST char *)path) >= EX_OK ) ? E_OK : E_NOEXS;
}

/* Whether a file of the directory is one of the object's resources */
LOCAL BOOL res_file( CONST UB *nm, CONST char *us, CONST UB **p_rest )
{
	INT	k;
	CONST UB *r;

	for ( k = 0; k < TS_UUID_STRLEN; k++ ) {
		UB	a = nm[k], b = (UB)us[k];

		if ( a >= 'A' && a <= 'Z' ) a = (UB)( a - 'A' + 'a' );	/* the names do not keep case */
		if ( a != b ) return FALSE;
	}
	r = nm + TS_UUID_STRLEN;
	if ( r[0] != '_' && r[0] != '.' ) return FALSE;
	if ( r[0] == '.' && r[1] == 'j' && r[2] == 's' && r[3] == 'o' && r[4] == 'n' && r[5] == 0 ) {
		return FALSE;
	}
	if ( r[0] == '_' ) {
		for ( k = 1; r[k] >= '0' && r[k] <= '9'; k++ ) ;
		if ( k > 1 && r[k] == '.'
		  && ( ( r[k + 1] == 'x' && r[k + 2] == 't' && r[k + 3] == 'a' && r[k + 4] == 'd' && r[k + 5] == 0 )
		    || ( r[k + 1] == 'b' && r[k + 2] == 'i' && r[k + 3] == 'n' && r[k + 4] == 0 ) ) ) {
			return FALSE;		/* a record */
		}
	}
	*p_rest = r;
	return TRUE;
}

/* The object's resources, their names one after another, each ending in 0 */
EXPORT ER knl_tsfs_res_lst( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, INT *p_cnt )
{
	TSFSVOL		*v = vol_of(vol);
	T_DIRENT	*de;
	char		us[TS_UUID_STRLEN + 1];
	INT		fd, cnt, i, got = 0;
	SZ		at = 0;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return nres_lst(v, uuid, buf, size, p_cnt);
	}
	if ( ts_uuid_to_str(uuid, us, sizeof(us)) < E_OK ) {
		return E_PAR;
	}
	de = (T_DIRENT *)Kmalloc(sizeof(T_DIRENT) * 8);
	if ( de == NULL ) {
		return E_NOMEM;
	}
	fd = fs_open((CONST char *)v->root, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) {
		Kfree(de);
		return E_IO;
	}
	while ( ( cnt = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < cnt; i++ ) {
			CONST UB	*rest;
			INT		k;

			if ( !res_file(de[i].name, us, &rest) ) continue;
			for ( k = 0; rest[k] != 0; k++ ) ;
			if ( buf != NULL && at + k + 1 <= size ) {
				knl_memcpy(buf + at, rest, k + 1);
				at += k + 1;
				got++;
			}
		}
	}
	fs_close(fd);
	Kfree(de);
	if ( p_cnt != NULL ) *p_cnt = got;

	return E_OK;
}

/*
 * An object made with an identity given rather than a new one: an
 * object the system knows by a fixed UUID.
 */
EXPORT ER knl_tsfs_cre_as( ID vol, CONST TS_UUID *uuid, CONST UB *json, INT len )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL || uuid == NULL || json == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		vol_lock(v);
		er = ts_obj_create_as(v->bvol, uuid, json, len);
		vol_unlock(v);
		return er;
	}
	if ( knl_tsfs_exists(vol, uuid) >= E_OK ) {
		return E_OBJ;
	}
	vol_lock(v);
	er = ( write_meta(v, uuid, json, len) >= EX_OK ) ? E_OK : E_IO;
	vol_unlock(v);

	return er;
}

EXPORT ER knl_tsfs_exists( ID vol, CONST TS_UUID *uuid )
{
	TSFSVOL	*v = vol_of(vol);
	UB	path[TSFS_PATH_MAX];
	T_FSTAT	st;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return ( ts_obj_exists(v->bvol, uuid) >= E_OK ) ? E_OK : E_NOEXS;
	}
	if ( obj_path(v, uuid, ".json", path, sizeof(path)) < EX_OK ) {
		return E_PAR;
	}
	return ( fs_stat((CONST char *)path, &st) >= EX_OK ) ? E_OK : E_NOEXS;
}

EXPORT ER knl_tsfs_get_meta( ID vol, CONST TS_UUID *uuid, UB *buf, INT max, INT *p_len )
{
	TSFSVOL	*v = vol_of(vol);
	INT	n;
	ER	er = E_OK;

	if ( v == NULL || uuid == NULL || buf == NULL ) {
		return E_ID;
	}
	vol_rlock(v);
	if ( v->bvol > 0 ) {
		SZ	asize = 0;

		er = ts_obj_get_meta(v->bvol, uuid, buf, (SZ)max, &asize);
		n = (INT)asize;
	} else {
		n = read_meta(v, uuid, buf, max);
		if ( n < 0 ) er = E_NOEXS;
	}
	vol_runlock(v);
	if ( er >= E_OK && p_len != NULL ) {
		*p_len = n;
	}
	return er;
}

EXPORT ER knl_tsfs_get_icon( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	vol_rlock(v);
	er = ( v->bvol > 0 ) ? ts_obj_get_icon(v->bvol, uuid, buf, size, p_asize)
			     : read_icon(v, uuid, buf, size, p_asize);
	vol_runlock(v);
	return er;
}

EXPORT ER knl_tsfs_set_icon( ID vol, CONST TS_UUID *uuid, CONST UB *buf, SZ size )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	if ( size < 0 || size > TSFS_ICON_MAX || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	vol_lock(v);
	er = ( v->bvol > 0 ) ? ts_obj_set_icon(v->bvol, uuid, buf, size)
			     : write_icon(v, uuid, buf, size);
	vol_unlock(v);
	return er;
}

/*
 * An object made with its metadata and its icon: under the identity
 * given, or a new one when `as` is NULL. On the native store both go in
 * the one transaction.
 */
EXPORT ER knl_tsfs_cre_icon( ID vol, CONST TS_UUID *as, CONST UB *json, INT len,
			    CONST UB *icon, SZ iconlen, TS_UUID *p_uuid )
{
	TSFSVOL	*v = vol_of(vol);
	TS_UUID	uuid;
	ER	er;

	if ( v == NULL ) {
		return E_ID;
	}
	if ( iconlen < 0 || iconlen > TSFS_ICON_MAX || ( iconlen > 0 && icon == NULL ) ) {
		return E_PAR;
	}
	if ( v->bvol > 0 ) {
		vol_lock(v);
		er = ts_obj_create_icon(v->bvol, as, json, len, icon, iconlen, p_uuid);
		vol_unlock(v);
		return er;
	}
	if ( as != NULL ) {
		uuid = *as;
		er = knl_tsfs_cre_as(vol, &uuid, json, len);
	} else {
		T_COBJ	c;

		knl_memset(&c, 0, sizeof(c));
		c.json = json;
		c.jsonsz = len;
		er = ts_cre_obj(vol, &c, &uuid);
	}
	if ( er >= E_OK && iconlen > 0 ) {
		vol_lock(v);
		er = write_icon(v, &uuid, icon, iconlen);
		vol_unlock(v);
	}
	if ( er >= E_OK && p_uuid != NULL ) {
		*p_uuid = uuid;
	}
	return er;
}

EXPORT ER knl_tsfs_set_meta( ID vol, CONST TS_UUID *uuid, CONST UB *buf, INT len )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er;

	if ( v == NULL || uuid == NULL || buf == NULL ) {
		return E_ID;
	}
	vol_lock(v);
	if ( v->bvol > 0 ) {
		er = ts_obj_set_meta(v->bvol, uuid, buf, (SZ)len);
	} else {
		er = ( write_meta(v, uuid, buf, len) >= EX_OK ) ? E_OK : E_IO;
	}
	vol_unlock(v);

	return er;
}

EXPORT ER knl_tsfs_set_flags( ID vol, CONST TS_UUID *uuid, UINT flags, UINT mask )
{
	TSFSVOL	*v = vol_of(vol);
	ER	er = E_OK;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	vol_lock(v);
	if ( v->bvol > 0 ) {
		er = ts_obj_set_flags(v->bvol, uuid, flags, mask);
	}
	vol_unlock(v);

	return er;
}

/* ---------------------------------------------------------------- loader */

/*
 * The path of one record, on the FAT store. The native store has no
 * paths: E_NOSPT, and the loader reads the record through ts_rea_rec.
 */
EXPORT ER knl_tsfs_rec_path( ID vol, CONST TS_UUID *uuid, INT recno, UB *out, INT max )
{
	TSFSVOL	*v = vol_of(vol);
	INT	t;

	if ( v == NULL || uuid == NULL || out == NULL ) {
		return E_ID;
	}
	if ( v->bvol > 0 ) {
		return E_NOSPT;			/* the native store has no paths */
	}
	t = rec_type(v, uuid, recno, NULL);
	if ( t < 0 ) {
		return E_NOEXS;
	}
	return ( rec_path(v, uuid, recno, (UINT)t, out, max) >= EX_OK ) ? E_OK : E_PAR;
}

/*
 * The record holding the program, from "tessronos": { "exec": { "record": N } }
 * of the metadata (design 9.6). E_NOEXS when the object is not a program.
 */
EXPORT INT knl_tsfs_exec_rec( ID vol, CONST TS_UUID *uuid )
{
	TSFSVOL	*v = vol_of(vol);
	UB	*json;
	INT	len, off, sublen, off2, sublen2, rec = E_NOEXS;

	if ( v == NULL || uuid == NULL ) {
		return E_ID;
	}
	json = (UB *)Kmalloc(TSFS_JSON_MAX);
	if ( json == NULL ) {
		return E_NOMEM;
	}
	vol_lock(v);

	if ( v->bvol > 0 ) {
		SZ	asize = 0;

		len = ( ts_obj_get_meta(v->bvol, uuid, json, TSFS_JSON_MAX,
					&asize) >= E_OK ) ? (INT)asize : -1;
	} else {
		len = read_meta(v, uuid, json, TSFS_JSON_MAX);
	}
	if ( len > 0
	  && json_sub(json, len, "tessronos", &off, &sublen)
	  && json_sub(json + off, sublen, "exec", &off2, &sublen2) ) {
		rec = knl_json_get_num(json + off + off2, sublen2, "record", -1);
		if ( rec < 0 || rec >= TSFS_MAX_REC ) rec = E_NOEXS;
	}

	vol_unlock(v);
	Kfree(json);

	return rec;
}

/* ---------------------------------------------------------------- the store's directory */

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Whether a name begins with a UUID in its text form, as every file of an object does */
LOCAL BOOL uuid_named( CONST char *s )
{
	INT	i;

	for ( i = 0; i < TS_UUID_STRLEN; i++ ) {
		UB	c = lower((UB)s[i]);

		if ( i == 8 || i == 13 || i == 18 || i == 23 ) {
			if ( c != '-' ) return FALSE;
		} else if ( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) ) ) {
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * A store keeps its objects' files straight in its directory, beside
 * whatever else is there (the programs and faces in /boot), so what is
 * closed is exactly those files: a name in that directory that begins
 * with a UUID. The letters are compared without case, as FAT names
 * them.
 */
EXPORT BOOL knl_tsfs_guards( CONST char *path )
{
	INT	i, k;

	if ( path == NULL ) {
		return FALSE;
	}
	for ( i = 0; i < TSFS_MAX_VOL; i++ ) {
		TSFSVOL	*v = &tsfs_vol[i];
		INT	n = v->rootlen;

		if ( !v->used || v->bvol != 0 || n <= 0 ) {
			continue;
		}
		if ( n == 1 && v->root[0] == '/' ) {
			n = 0;				/* the root of everything */
		}
		for ( k = 0; k < n && path[k] != 0
			     && lower((UB)path[k]) == lower(v->root[k]); k++ ) ;
		if ( k != n || path[k] != '/' ) {
			continue;
		}
		for ( k = n + 1; path[k] != 0 && path[k] != '/'; k++ ) ;
		if ( path[k] == 0 && uuid_named(path + n + 1) ) {
			return TRUE;			/* one of its files */
		}
	}
	return FALSE;
}
