/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	arc_pack.c
 *	書庫管理: the objects of a book found and packed (design 11.20)
 *
 *	The objects are the root, those its record 0 links to, and those
 *	these link to, each once, the root first; the archive object being
 *	written to is left out, and a link to it points at nothing in the
 *	book. Each is numbered by its place, from 0, and a link record names
 *	its target by that number.
 *
 *	An object's records in the book, in order:
 *	  - a link record for each <link> of record 0, in its order, so that
 *	    the n-th virtual object of the TAD takes the n-th of them;
 *	  - record 0 as binary TAD (lib/libbtbk), its pictures as picture
 *	    segments;
 *	  - a 実行機能付箋 (RT 8) for each program of its applist BTRON
 *	    knows, the one that opens it first: the window it opens in, the
 *	    window's colour and マイクロスクリプト's $SV, or the object's own
 *	    record of that type;
 *	  - every other record as it is, an xmlTAD record as bytes (RT 15).
 *	An archive object is kept as BTRON keeps one: its book is its TAD
 *	record and its 実行機能付箋 names the archive's application; an empty
 *	one has a figure of nothing.
 *
 *	The local header keeps the name (20 TRON characters), whether it may
 *	be written and deleted, the application type and the times of the
 *	metadata (makeDate, updateDate, accessDate, periodDate).
 */

#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/btbk.h>
#include <ts/vobjsz.h>

#define LINK_GROW	16
#define OBJ_GROW	32
#define NO_TIME		0xFFFFFFFFU
/* bytes written to a record at a time: each write is made durable when it
   returns, so the fewer they are the less the medium waits (design 7.3) */
#define CHUNK		( 4 * 1024 * 1024 )
#define UNCHANGED_SECS	600		/* made and its records written: not changed since */

/* A figure of nothing: 管理情報, the figure's start and its end */
LOCAL const UB empty_fig[] = {
	0xE0, 0xFF, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x21, 0x01,
	0xE3, 0xFF, 0x18, 0x00, 0, 0, 0, 0, 0xC8, 0x00, 0x64, 0x00, 0, 0, 0, 0, 0xC8, 0x00, 0x64, 0x00,
	0x88, 0xFF, 0x88, 0xFF, 0, 0, 0, 0,
	0xE4, 0xFF, 0x00, 0x00
};

/* A text of nothing, for an object whose record 0 is no xmlTAD */
LOCAL const UB empty_text[] = {
	0xE0, 0xFF, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x22, 0x01,
	0xE1, 0xFF, 0x18, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0xB8, 0xFF, 0xB8, 0xFF, 0x21, 0x00, 0, 0,
	0x0A, 0x00, 0xE2, 0xFF, 0x00, 0x00
};

LOCAL void seterr( ARCJOB *j, const char *s, ER er )
{
	snprintf(j->err, sizeof(j->err), "%s (%d)", s, (INT)er);
}

LOCAL BOOL same_uuid( const TS_UUID *a, const TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

LOCAL INT find_obj( ARCJOB *j, const TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < j->nobj; i++ ) {
		if ( same_uuid(&j->obj[i].uuid, u) ) return i;
	}
	return -1;
}

LOCAL INT add_obj( ARCJOB *j, const TS_UUID *u )
{
	if ( j->nobj >= j->maxobj ) {
		INT	m = j->maxobj + OBJ_GROW;
		ARCOBJ	*no = realloc(j->obj, sizeof(ARCOBJ) * (size_t)m);

		if ( no == NULL ) return -1;
		j->obj = no;
		j->maxobj = m;
	}
	memset(&j->obj[j->nobj], 0, sizeof(ARCOBJ));
	j->obj[j->nobj].uuid = *u;
	return j->nobj++;
}

/* ---------------------------------------------------------------- the metadata */

/* The key of a member whose value starts at v, in the text before it */
LOCAL BOOL key_of( const UB *text, const UB *v, const UB **p_k, INT *p_n )
{
	const UB *p = v - 1, *e;

	while ( p > text && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) ) p--;
	if ( *p != ':' ) return FALSE;
	p--;
	while ( p > text && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) ) p--;
	if ( *p != '"' ) return FALSE;
	e = p;
	p--;
	while ( p > text && *p != '"' ) p--;
	if ( *p != '"' ) return FALSE;
	*p_k = p + 1;
	*p_n = (INT)( e - p - 1 );
	return TRUE;
}

/* A program of the applist, as BTRON knows it, put after those already there (once) */
LOCAL void appl_add( ARCOBJ *o, const UB *k, INT n, BOOL first )
{
	char	id[48];
	BPKAPPL	a;
	INT	i;

	if ( n <= 0 || n >= (INT)sizeof(id) ) return;
	memcpy(id, k, (size_t)n);
	id[n] = 0;
	if ( !bpk_prog_appl(id, &a) ) return;
	for ( i = 0; i < o->nappl; i++ ) {
		if ( o->appl[i].appl[1] == a.appl[1] ) return;
	}
	if ( o->nappl >= ARC_APPL_MAX ) return;
	if ( first ) {
		memmove(&o->appl[1], &o->appl[0], sizeof(BPKAPPL) * (size_t)o->nappl);
		o->appl[0] = a;
	} else {
		o->appl[o->nappl] = a;
	}
	o->nappl++;
}

LOCAL UINT colour_of( const char *s )
{
	return ( s[0] == '#' ) ? 0x10000000U | ( (UINT)strtoul(s + 1, NULL, 16) & 0xFFFFFF ) : 0x10FFFFFFU;
}

LOCAL UINT time_of( const T_JSON *root, const char *key, UINT none )
{
	UB	t[48];

	if ( js_get_str(root, key, t, sizeof(t)) <= 0 ) return none;
	return bpk_iso_stime((const char *)t);
}

/* What the metadata says of an object: its name, programs, window, times, protection */
LOCAL void read_meta( ARCOBJ *o, const UB *meta, SZ size, const char *refname )
{
	T_JSON	root, al, it, win, pos, sv, ts, bt;
	const UB *k;
	INT	n, i;
	UB	col[16];

	strncpy(o->name, refname, sizeof(o->name) - 1);
	o->ftype = BPK_F_FILE;
	o->ltime = NO_TIME;
	if ( meta == NULL || js_parse(meta, (INT)size, &root) < E_OK ) return;
	(void)js_get_str(&root, "name", (UB *)o->name, sizeof(o->name));
	if ( js_get(&root, "applist", &al) >= E_OK ) {
		for ( it.s = NULL; js_next(&al, &it); ) {
			if ( key_of(meta, it.s, &k, &n) ) appl_add(o, k, n, js_get_bool(&it, "defaultOpen", FALSE));
		}
	}
	if ( js_get(&root, "window", &win) >= E_OK ) {
		INT	x = 100, y = 100, w, h;

		if ( js_get(&win, "pos", &pos) >= E_OK ) {
			x = (INT)js_get_num(&pos, "x", 100);
			y = (INT)js_get_num(&pos, "y", 100);
		}
		w = (INT)js_get_num(&win, "width", 0);
		h = (INT)js_get_num(&win, "height", 0);
		if ( w > 0 && h > 0 ) {
			o->win[0] = x;
			o->win[1] = y;
			o->win[2] = x + w;
			o->win[3] = y + h;
			o->haswin = TRUE;
		}
		if ( js_get_str(&win, "backgroundColor", col, sizeof(col)) > 0 ) {
			o->bg = colour_of((const char *)col);
			o->hasbg = TRUE;
		}
	}
	if ( js_get(&root, "microscriptSV", &sv) >= E_OK && js_type(&sv) == JS_ARRAY ) {
		D	v;

		for ( i = 0, it.s = NULL; i < 50 && js_next(&sv, &it); i++ ) o->sv[i] = ( js_num(&it, &v) >= E_OK ) ? (INT)v : 0;
		o->hassv = TRUE;
	}
	if ( !js_get_bool(&root, "editable", TRUE) ) o->ftype |= BPK_F_RONLY;
	if ( !js_get_bool(&root, "deletable", TRUE) ) o->ftype |= BPK_F_PERM;
	o->ctime = time_of(&root, "makeDate", 0);
	o->mtime = time_of(&root, "updateDate", 0);
	o->atime = time_of(&root, "accessDate", 0);
	o->ltime = time_of(&root, "periodDate", NO_TIME);
	if ( o->ltime == 0 ) o->ltime = NO_TIME;

	/*
	 * An object taken out of a book or a backup keeps the times it had
	 * there beside the ones the store gave it. Its making is that one;
	 * its changing and looking at are too, unless it was changed after
	 * it was made here.
	 */
	if ( js_get(&root, "tessronos", &ts) >= E_OK && js_get(&ts, "btron", &bt) >= E_OK ) {
		UINT	c = time_of(&bt, "ctime", 0), m = time_of(&bt, "mtime", 0), a = time_of(&bt, "atime", 0);
		BOOL	unchanged = (BOOL)( o->mtime <= o->ctime + UNCHANGED_SECS );

		if ( c != 0 ) o->ctime = c;
		if ( unchanged && m != 0 ) o->mtime = m;
		if ( unchanged && a != 0 ) o->atime = a;
	}
}

/* Whether the object opens with a program of the archive's application */
LOCAL BOOL opens_archive( const ARCOBJ *o )
{
	return (BOOL)( o->nappl > 0 && o->appl[0].appl[1] == BPK_APPL_ARCHIVE );
}

/* ---------------------------------------------------------------- the book of an archive object */

EXPORT ER arc_book_rec( const TS_UUID *archive, INT *p_recno, UB **p_raw, UINT *p_len )
{
	T_OBREC	*r;
	INT	cnt = 0, i;
	ID	k;

	*p_recno = -1;
	if ( p_raw != NULL ) *p_raw = NULL;
	if ( p_len != NULL ) *p_len = 0;
	k = ob_opn_obj(archive, OB_OP_R);
	if ( k <= 0 ) return E_NOEXS;
	r = ob_rec_read(k, &cnt);
	for ( i = 0; i < cnt && *p_recno < 0; i++ ) {
		UB	head[2];
		SZ	asz = 0;
		UINT	len = 0;
		UB	*raw;

		if ( r[i].rt != OB_RT_SYSDATA || r[i].size < 2 ) continue;
		if ( ob_rea_rec(k, r[i].recno, 0, head, 2, &asz) < E_OK || asz != 2 || head[0] != 0xE0 || head[1] != 0xFF ) continue;
		raw = unp_read_rec(k, r[i].recno, &len);
		if ( raw == NULL ) continue;
		if ( !bpk_is_archive(raw, len) ) {
			free(raw);
			continue;
		}
		*p_recno = r[i].recno;
		if ( p_raw != NULL ) {
			*p_raw = raw;
			*p_len = len;
		} else {
			free(raw);
		}
	}
	free(r);
	ob_cls_obj(k);
	return E_OK;
}

EXPORT ER arc_book_put( const TS_UUID *archive, const UB *raw, UINT len )
{
	INT	recno = -1;
	UINT	at = 0;
	SZ	asz = 0;
	ER	er;
	ID	k;

	er = arc_book_rec(archive, &recno, NULL, NULL);
	if ( er < E_OK ) return er;
	/* the record written piece by piece: nobody reads it half written */
	k = ob_opn_obj(archive, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	if ( k <= 0 ) return ( k < 0 ) ? (ER)k : E_NOEXS;
	if ( raw == NULL ) {
		er = ( recno >= 0 ) ? ob_del_rec(k, recno) : E_OK;
		ob_cls_obj(k);
		return er;
	}
	if ( recno < 0 ) er = ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno);
	while ( er >= E_OK && at < len ) {
		SZ	n = ( len - at > CHUNK ) ? CHUNK : (SZ)( len - at );

		er = ob_wri_rec(k, recno, (D)at, raw + at, n, &asz);
		if ( er >= E_OK && asz != n ) er = E_IO;
		at += (UINT)n;
	}
	if ( er >= E_OK ) er = ob_trn_rec(k, recno, (UD)len);
	ob_cls_obj(k);
	return er;
}

/* ---------------------------------------------------------------- finding the objects */

typedef struct {
	ARCJOB	*j;
	INT	at;			/* the object whose links are told */
} WALK;

LOCAL ER walk_link( void *arg, CONST BK_TADLINK *lk )
{
	WALK	*w = arg;
	ARCOBJ	*o = &w->j->obj[w->at];
	TS_UUID	u;

	if ( o->nlink >= o->maxlink ) {
		INT	m = o->maxlink * 2 + LINK_GROW;
		TS_UUID	*nt = realloc(o->target, sizeof(TS_UUID) * (size_t)m);
		UH	*na;

		if ( nt == NULL ) return E_NOMEM;
		o->target = nt;
		na = realloc(o->latr, sizeof(UH) * (size_t)m);
		if ( na == NULL ) return E_NOMEM;
		o->latr = na;
		o->maxlink = m;
	}
	memset(&u, 0, sizeof(u));
	(void)unp_uuid_parse((const char *)lk->target, &u);
	o->target[o->nlink] = u;
	o->latr[o->nlink] = lk->attr;
	o->nlink++;
	return E_OK;
}

EXPORT ER arc_find( ARCJOB *j, const TS_UUID *archive, const TS_UUID *root )
{
	UB		*meta;
	BK_TADW		*w;
	WALK		wk;
	BK_TADHOOK	hk = { walk_link, NULL, NULL };
	INT		i, l;
	ER		er = E_OK;

	arc_free(j);
	j->archive = *archive;
	j->root = *root;
	j->err[0] = 0;
	if ( same_uuid(archive, root) ) {
		seterr(j, "書庫の実身そのものは書籍にできません", E_PAR);
		return E_PAR;
	}
	w = malloc(sizeof(BK_TADW));
	if ( w == NULL || add_obj(j, root) < 0 ) {
		free(w);
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	wk.j = j;
	hk.arg = &wk;
	for ( i = 0; i < j->nobj && er >= E_OK; i++ ) {
		ARCOBJ	*o = &j->obj[i];
		T_OBREF	ref;
		T_OBREC	*r;
		INT	asz, cnt = 0, q, recno = -1, tlen = 0;
		UB	*xml;
		UINT	xlen = 0;
		ID	k;

		k = ob_opn_obj(&o->uuid, OB_OP_R);
		if ( k <= 0 || ob_ref_obj(&o->uuid, &ref) < E_OK ) {
			er = ( k < 0 ) ? (ER)k : E_NOEXS;
			seterr(j, "実身を開けません", er);
			if ( k > 0 ) ob_cls_obj(k);
			break;
		}
		/* the metadata read first: the size it gives is an argument of the same call */
		asz = ob_atr_read(k, 0, &meta);
		read_meta(o, ( asz >= 0 ) ? meta : NULL, ( asz >= 0 ) ? asz : 0, (const char *)ref.name);
		free(meta);
		er = E_OK;
		r = ob_rec_read(k, &cnt);
		for ( q = 0; q < cnt; q++ ) o->size += r[q].size;
		free(r);
		j->total += o->size;
		ob_cls_obj(k);

		if ( opens_archive(o) ) {
			/* an archive object: its book, or nothing, and no links of its own */
			(void)arc_book_rec(&o->uuid, &recno, NULL, NULL);
			o->kind = ( recno >= 0 ) ? K_ARCHIVE : K_EMPTY;
		} else {
			o->kind = K_PLAIN;
			k = ob_opn_obj(&o->uuid, OB_OP_R);
			xml = ( k > 0 ) ? unp_read_rec(k, 0, &xlen) : NULL;
			if ( k > 0 ) ob_cls_obj(k);
			if ( xml != NULL ) {
				wk.at = i;
				er = bk_tad_from_xml(w, xml, (INT)xlen, NULL, 0, &tlen, &hk, NULL);
				free(xml);
				if ( er == E_PAR ) er = E_OK;		/* no xmlTAD: no links */
				if ( er < E_OK ) {
					seterr(j, "記録を読めません", er);
					break;
				}
			}
		}
		o = &j->obj[i];
		for ( l = 0; l < o->nlink; l++ ) {
			T_OBREF	tr;
			TS_UUID	t = o->target[l];

			if ( same_uuid(&t, archive) || find_obj(j, &t) >= 0 ) continue;
			if ( ob_ref_obj(&t, &tr) < E_OK || tr.type != OB_T_STORAGE ) continue;
			if ( j->nobj >= BPK_FILES_MAX ) {
				er = E_LIMIT;
				seterr(j, "書籍に入る実身は650までです", er);
				break;
			}
			if ( add_obj(j, &t) < 0 ) {
				er = E_NOMEM;
				seterr(j, "作業の場所がありません", er);
				break;
			}
			o = &j->obj[i];
		}
		if ( j->tell != NULL ) j->tell(j, i + 1, j->nobj, o->name);
	}
	free(w);
	return er;
}

/* ---------------------------------------------------------------- packing */

/* The pixels of a picture of the object being written, for the TAD writer */
typedef struct {
	ID	key;
	UINT	*px;
} PICS;

LOCAL ER pic_image( void *arg, CONST UB *href, CONST UINT **p_px, INT *p_w, INT *p_h )
{
	PICS	*p = arg;
	const char *u = strchr((const char *)href, '_');
	UB	*data, probe[8];
	SZ	asz = 0, size;
	INT	w = 0, h = 0;
	ER	er;

	if ( u == NULL || p->key <= 0 ) return E_NOEXS;
	if ( ob_rea_res(p->key, (const UB *)u, 0, probe, sizeof(probe), &asz) < E_OK || asz <= 0 ) return E_NOEXS;
	for ( size = 64 * 1024; ; size *= 2 ) {
		data = malloc((size_t)size);
		if ( data == NULL ) return E_NOEXS;
		if ( ob_rea_res(p->key, (const UB *)u, 0, data, size, &asz) < E_OK ) {
			free(data);
			return E_NOEXS;
		}
		if ( asz < size || size >= 16 * 1024 * 1024 ) break;
		free(data);
	}
	er = dp_img_decode(data, asz, NULL, 0, &w, &h);
	if ( er >= E_OK && w > 0 && h > 0 && (UD)w * (UD)h <= 4 * 1024 * 1024 ) {
		free(p->px);
		p->px = malloc(sizeof(UINT) * (size_t)w * (size_t)h);
		er = ( p->px != NULL ) ? dp_img_decode(data, asz, (UW *)p->px, (SZ)w * h, &w, &h) : E_NOMEM;
	} else {
		er = E_NOEXS;
	}
	free(data);
	if ( er < E_OK ) return E_NOEXS;
	*p_px = p->px;
	*p_w = w;
	*p_h = h;
	return E_OK;
}

/* What one object's records hold while the book is written */
typedef struct {
	BPKREC	*rec;
	INT	nrec, maxrec;
	UB	**owned;		/* the bytes made for it, freed after */
	INT	nowned, maxowned;
} RECS;

LOCAL BOOL recs_add( RECS *s, UINT type, UINT sub, const UB *data, UINT size, BOOL own )
{
	if ( s->nrec >= s->maxrec ) {
		INT	m = s->maxrec * 2 + 16;
		BPKREC	*nr = realloc(s->rec, sizeof(BPKREC) * (size_t)m);

		if ( nr == NULL ) return FALSE;
		s->rec = nr;
		s->maxrec = m;
	}
	if ( own ) {
		if ( s->nowned >= s->maxowned ) {
			INT	m = s->maxowned * 2 + 16;
			UB	**no = realloc(s->owned, sizeof(UB *) * (size_t)m);

			if ( no == NULL ) return FALSE;
			s->owned = no;
			s->maxowned = m;
		}
		s->owned[s->nowned++] = (UB *)data;
	}
	s->rec[s->nrec].type = (UH)type;
	s->rec[s->nrec].sub = (UH)sub;
	s->rec[s->nrec].size = size;
	s->rec[s->nrec].data = data;
	s->nrec++;
	return TRUE;
}

LOCAL void recs_free( RECS *s )
{
	INT	i;

	for ( i = 0; i < s->nowned; i++ ) free(s->owned[i]);
	free(s->owned);
	free(s->rec);
	memset(s, 0, sizeof(*s));
}

/* A copy of n bytes to keep */
LOCAL UB *dup_bytes( const UB *p, UINT n )
{
	UB	*d = malloc(n ? n : 1);

	if ( d != NULL && n != 0 ) memcpy(d, p, n);
	return d;
}

/* The records of object i, into s */
LOCAL ER object_recs( ARCJOB *j, INT i, RECS *s )
{
	ARCOBJ	*o = &j->obj[i];
	T_OBREC	*r;
	INT	cnt = 0, q, l, bookrec = -1, own_exec = 0;
	BOOL	rec0_tad = FALSE;		/* record 0 went as binary TAD */
	ID	k;
	ER	er = E_OK;

	k = ob_opn_obj(&o->uuid, OB_OP_R);
	if ( k <= 0 ) return ( k < 0 ) ? (ER)k : E_NOEXS;
	r = ob_rec_read(k, &cnt);

	if ( o->kind == K_PLAIN ) {
		/* the link records, then record 0 as binary TAD */
		for ( l = 0; l < o->nlink && er >= E_OK; l++ ) {
			UB	*lk = malloc(BPK_LINKREC);
			INT	t = find_obj(j, &o->target[l]);

			if ( lk == NULL ) {
				er = E_NOMEM;
				break;
			}
			bpk_link_put(lk, ( t >= 0 ) ? (UINT)t : 0xFFFF, o->latr[l], 0,
				     ( t >= 0 && j->obj[t].nappl > 0 ) ? j->obj[t].appl[0].appl : NULL);
			if ( !recs_add(s, BPK_RT_LINK, 0, lk, BPK_LINKREC, TRUE) ) {
				free(lk);
				er = E_NOMEM;
			}
		}
		if ( er >= E_OK ) {
			UINT		xlen = 0;
			UB		*xml = unp_read_rec(k, 0, &xlen), *tad = NULL;
			INT		tlen = 0;
			BK_TADW		*w = malloc(sizeof(BK_TADW));
			PICS		pics = { 0, NULL };
			BK_TADHOOK	hk = { NULL, pic_image, NULL };

			pics.key = k;
			hk.arg = &pics;
			if ( w == NULL ) er = E_NOMEM;
			if ( er >= E_OK && xml != NULL ) {
				er = bk_tad_from_xml(w, xml, (INT)xlen, NULL, 0, &tlen, &hk, NULL);
				if ( er >= E_OK ) {
					tad = malloc((size_t)tlen + 64);
					er = ( tad != NULL ) ? bk_tad_from_xml(w, xml, (INT)xlen, tad, tlen + 64, &tlen, &hk, NULL)
							     : E_NOMEM;
				}
				if ( er == E_PAR ) {
					/* record 0 is no xmlTAD: it goes as bytes with the rest */
					free(tad);
					tad = NULL;
					er = E_OK;
				}
			}
			free(w);
			free(xml);
			free(pics.px);
			rec0_tad = (BOOL)( tad != NULL );
			if ( er >= E_OK && tad == NULL ) {
				tad = dup_bytes(empty_text, sizeof(empty_text));
				tlen = sizeof(empty_text);
			}
			if ( er >= E_OK && ( tad == NULL || !recs_add(s, BPK_RT_TAD, 0, tad, (UINT)tlen, TRUE) ) ) {
				free(tad);
				er = E_NOMEM;
			}
		}
	} else {
		/* an archive object: its book is its TAD, or a figure of nothing */
		UB	*raw = NULL;
		UINT	len = 0;

		if ( o->kind == K_ARCHIVE ) (void)arc_book_rec(&o->uuid, &bookrec, &raw, &len);
		if ( raw == NULL ) {
			raw = dup_bytes(empty_fig, sizeof(empty_fig));
			len = sizeof(empty_fig);
		}
		if ( raw == NULL || !recs_add(s, BPK_RT_TAD, 0, raw, len, TRUE) ) {
			free(raw);
			er = E_NOMEM;
		}
	}

	/* the programs that open it */
	for ( q = 0; q < cnt; q++ ) {
		if ( r[q].rt == OB_RT_MFUSEN ) own_exec++;
	}
	for ( l = 0; er >= E_OK && own_exec == 0 && l < o->nappl; l++ ) {
		UB	*e = malloc(BPK_EXEC_MAX);
		INT	n;

		if ( e == NULL ) {
			er = E_NOMEM;
			break;
		}
		n = bpk_exec_put(e, &o->appl[l], o->haswin ? o->win : NULL, o->bg, o->hasbg,
				 ( o->hassv && o->appl[l].appl[1] == 0x000B ) ? o->sv : NULL);
		if ( !recs_add(s, BPK_RT_EXEC, 0, e, (UINT)n, TRUE) ) {
			free(e);
			er = E_NOMEM;
		}
	}

	/* the rest as they are */
	for ( q = 0; er >= E_OK && q < cnt; q++ ) {
		UINT	len = 0, rt = r[q].rt;
		UB	*b;

		if ( r[q].recno == 0 && rec0_tad ) continue;
		if ( r[q].recno == 0 && opens_archive(o) ) continue;	/* what the archive object says of itself */
		if ( r[q].recno == bookrec ) continue;
		b = unp_read_rec(k, r[q].recno, &len);
		if ( b == NULL && r[q].size != 0 ) {
			er = E_IO;
			break;
		}
		if ( b == NULL ) b = dup_bytes(NULL, 0);
		if ( rt == OB_RT_TAD || rt == OB_RT_LINK ) rt = OB_RT_SYSDATA;
		if ( b == NULL || !recs_add(s, rt, r[q].sub, b, len, TRUE) ) {
			free(b);
			er = E_NOMEM;
		}
	}
	free(r);
	ob_cls_obj(k);
	return er;
}

/* The local header of object i */
LOCAL void lhead_of( const ARCOBJ *o, INT i, BPKLHEAD *h )
{
	memset(h, 0, sizeof(*h));
	h->f_type = o->ftype;
	h->f_atype = ( o->nappl > 0 ) ? o->appl[0].atype : 1;
	(void)bpk_utf8_tron(o->name, h->name, 20);
	h->origid = i + 1;
	h->f_ltime = o->ltime;
	h->f_atime = o->atime;
	h->f_mtime = o->mtime;
	h->f_ctime = o->ctime;
}

/* What a link's frame shows, as the VLINK attr says it */
LOCAL UINT attr_of( const ARCLOOK *lk )
{
	return lk->attr;
}

EXPORT ER arc_pack( ARCJOB *j, INT method, const ARCLOOK *look )
{
	BPKWFILE	*f;
	RECS		*s;
	BPKWRITE	w;
	UB		*out = NULL;
	UINT		len = 0;
	INT		i, k;
	ER		er = E_OK;
	char		err[160];

	if ( j->nobj <= 0 ) {
		seterr(j, "書籍にする実身がありません", E_PAR);
		return E_PAR;
	}
	f = calloc((size_t)j->nobj, sizeof(BPKWFILE));
	s = calloc((size_t)j->nobj, sizeof(RECS));
	if ( f == NULL || s == NULL ) {
		free(f);
		free(s);
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	for ( i = 0; i < j->nobj && er >= E_OK; i++ ) {
		er = object_recs(j, i, &s[i]);
		if ( er < E_OK ) {
			char	m[ARC_NAME_MAX + 32];

			snprintf(m, sizeof(m), "%s を読めません", j->obj[i].name);
			seterr(j, m, er);
			break;
		}
		lhead_of(&j->obj[i], i, &f[i].head);
		f[i].nrec = s[i].nrec;
		f[i].rec = s[i].rec;
		if ( j->tell != NULL ) j->tell(j, i + 1, j->nobj + 1, j->obj[i].name);
	}
	if ( er >= E_OK ) {
		memset(&w, 0, sizeof(w));
		w.name = j->obj[0].name;
		w.time = j->time = arc_now(NULL);
		w.method = method;
		w.nfiles = j->nobj;
		w.file = f;
		w.hasroot = TRUE;
		if ( look != NULL && look->has ) {
			for ( k = 0; k < 4; k++ ) {
				w.rootview[k] = look->box[k];
				w.rootcol[k] = 0x10000000U | ( look->col[k] & 0xFFFFFF );
			}
			w.rootattr = attr_of(look);
			w.rootchsz = 0x8000U | (UINT)( ( look->chsz > 0 ? look->chsz : 14 ) * 20 );
		} else {
			INT	tw = dp_text_width((const UB *)j->obj[0].name, 14) + 40;

			w.rootview[0] = 0;
			w.rootview[1] = 0;
			w.rootview[2] = ( tw < 120 ) ? 120 : ( tw > 480 ) ? 480 : tw;
			w.rootview[3] = VOBJ_CLOSED_H(14, 1, 1);	/* closed, at 14 */
			w.rootcol[0] = w.rootcol[1] = 0x10000000U;
			w.rootcol[2] = w.rootcol[3] = 0x10FFFFFFU;
			w.rootattr = BPK_V_NORELN | BPK_V_NOTYPE | BPK_V_NOTIME;
			w.rootchsz = 0x8000U | 14 * 20;
		}
		if ( j->obj[0].nappl > 0 ) memcpy(w.rootappl, j->obj[0].appl[0].appl, sizeof(w.rootappl));
		if ( j->tell != NULL ) j->tell(j, j->nobj, j->nobj + 1, "圧縮しています");
		er = bpk_write(&w, &out, &len, &j->st, err, sizeof(err));
		if ( er < E_OK ) seterr(j, err, er);
	}
	for ( i = 0; i < j->nobj; i++ ) recs_free(&s[i]);
	free(s);
	free(f);
	if ( er >= E_OK ) {
		er = arc_book_put(&j->archive, out, len);
		if ( er < E_OK ) seterr(j, "書庫の実身に書けません", er);
	}
	free(out);
	if ( er >= E_OK && j->tell != NULL ) j->tell(j, j->nobj + 1, j->nobj + 1, "書籍を作りました");
	return er;
}

EXPORT void arc_free( ARCJOB *j )
{
	INT	i;

	for ( i = 0; i < j->nobj; i++ ) {
		free(j->obj[i].target);
		free(j->obj[i].latr);
	}
	free(j->obj);
	j->obj = NULL;
	j->nobj = j->maxobj = 0;
	j->total = 0;
}

/* ---------------------------------------------------------------- the clock and links */

EXPORT UINT arc_now( INT *p_offset )
{
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		t[48];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&clk, OB_OP_READ);
	UINT		now = 0;

	if ( p_offset != NULL ) *p_offset = 0;
	if ( k > 0 ) {
		if ( ob_rea_rec(k, 1, 0, t, sizeof(t) - 1, &asz) >= E_OK && asz >= 19 ) {
			t[asz] = 0;
			now = bpk_iso_stime((const char *)t);
			if ( p_offset != NULL && asz >= 25 && ( t[19] == '+' || t[19] == '-' ) ) {
				INT	off = ( ( t[20] - '0' ) * 10 + ( t[21] - '0' ) ) * 3600 + ( ( t[23] - '0' ) * 10 + ( t[24] - '0' ) ) * 60;

				*p_offset = ( t[19] == '-' ) ? -off : off;
			}
		}
		ob_cls_obj(k);
	}
	return now;
}

EXPORT ER arc_link_into( const TS_UUID *into, const TS_UUID *target, const char *name )
{
	T_OBREF	ref;
	UINT	len = 0;
	UB	*x;
	char	id[40], vid[40], *at, *link;
	TS_UUID	v;
	SZ	asz = 0;
	ID	k;
	ER	er;
	INT	n, tw = dp_text_width((const UB *)name, 14) + 40;

	if ( ob_ref_obj(into, &ref) < E_OK ) return E_NOEXS;
	k = ob_opn_obj(into, OB_OP_R | OB_OP_WRITE | OB_O_WEXCL);	/* read, a link added, written back */
	if ( k <= 0 ) return ( k < 0 ) ? (ER)k : E_NOEXS;
	x = unp_read_rec(k, 0, &len);
	at = ( x != NULL ) ? strstr((char *)x, "</figure>") : NULL;
	if ( at == NULL && x != NULL ) at = strstr((char *)x, "</document>");
	if ( at == NULL ) {
		free(x);
		ob_cls_obj(k);
		return E_OBJ;
	}
	memset(&v, 0, sizeof(v));
	if ( ts_gen_uuid(&v) < E_OK ) v = *target;
	unp_uuid_str(target, id);
	unp_uuid_str(&v, vid);
	link = malloc(len + 512);
	if ( link == NULL ) {
		free(x);
		ob_cls_obj(k);
		return E_NOMEM;
	}
	n = (INT)( at - (char *)x );
	memcpy(link, x, (size_t)n);
	n += snprintf(link + n, 512, "<link id=\"%s_0.xtad\" vobjid=\"%s\" vobjleft=\"8\" vobjtop=\"8\" vobjright=\"%d\""
		      " vobjbottom=\"%d\" height=\"%d\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\""
		      " bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>", id, vid, 8 + tw,
		      8 + VOBJ_CLOSED_H(14, 1, 1), VOBJ_CLOSED_H(14, 1, 1));	/* closed at 14 */
	memcpy(link + n, at, len - (UINT)( at - (char *)x ));
	n += (INT)( len - (UINT)( at - (char *)x ) );
	er = ob_rpl_rec(k, 0, link, (SZ)n);	/* whole: a reader sees the old record or the new one */
	(void)asz;
	if ( er >= E_OK && !( ref.flags & OB_F_AUTOREF ) ) er = ob_lnk_obj(target);
	free(link);
	free(x);
	ob_cls_obj(k);
	return er;
}
