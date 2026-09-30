/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_rest.c
 *	バックアップ: objects made again from a backup (design 17.18)
 *
 *	First pass, volume by volume: each object is made when its first
 *	piece comes, with a record 0 that says its name, and its records are
 *	written as they come -- a link record kept in memory as the objid it
 *	points at, a TAD record held as bytes in a record of its own, TessronOS's
 *	own record likewise. Second pass, when every volume has been read:
 *	every object is there, so each link can name its target; record 0 is
 *	made from the TAD (or from TessronOS's own record, which gives back the
 *	record exactly as it was, with its metadata, icon and resources), the
 *	metadata is written, and the records held for the work taken away.
 */

#include "bk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/json.h>
#include <ts/sysdef.h>

#define SUB_TAD		0xBAD1		/* a TAD record held as bytes */
#define SUB_PRIV	0xBAD2		/* TessronOS's own record held */
#define TAD_KEEP	8		/* TAD records of one object */
#define PRIV_HEAD	24
#define CHUNK		( 64 * 1024 )

typedef struct {
	UINT	objid;
	TS_UUID	uuid;
	char	name[BK_NAME_MAX];
	BK_FSTATE st;
	INT	nlink, maxlink;
	UINT	*links;			/* the objid each link record points at */
	UH	*latr;
	INT	tadrec[TAD_KEEP];	/* the TAD records held */
	INT	ntad;
	INT	privrec;		/* TessronOS's own record held, -1 none */
	BOOL	tf;			/* it came with TessronOS's own record */
	UH	*privrt;		/* then the types of its other records */
	INT	nprivrt, nother;
	INT	execrec;		/* its 実行機能付箋, -1 none */
	INT	currec;			/* the record the last piece went to, -1 none */
} RSOBJ;

typedef struct {
	BK_RD	rd;
	BK_FRAG	*frag;
	INT	nfrag;
	RSOBJ	*obj;
	INT	nobj, maxobj;
	BOOL	autoref;
	BKROOT	root[BK_ROOT_MAX];	/* the roots TessronOS's records say, by their number */
	BOOL	isroot[BK_ROOT_MAX];
	UB	buf[CHUNK];
} RSTATE;

LOCAL void seterr( BKJOB *j, const char *s, ER er )
{
	snprintf(j->err, sizeof(j->err), "%s (%d)", s, (INT)er);
}

LOCAL RSOBJ *by_objid( RSTATE *r, UINT objid )
{
	INT	i;

	if ( objid == 0 ) return NULL;
	for ( i = 0; i < r->nobj; i++ ) {
		if ( r->obj[i].objid == objid ) return &r->obj[i];
	}
	return NULL;
}

/* ---------------------------------------------------------------- the first pass */

EXPORT ER bk_rest_begin( BKJOB *j )
{
	RSTATE	*r = calloc(1, sizeof(RSTATE));

	if ( r == NULL ) {
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	j->rest = r;
	j->nvol = 0;
	j->more = FALSE;
	j->nmade = j->nlinked = j->nlost = j->nconv = 0;
	return E_OK;
}

LOCAL void btron_of( BPKBUF *b, const RSOBJ *o );

/* The metadata of an object BTRON made, from its F_STATE and name */
LOCAL void meta_btron( BPKBUF *b, const RSOBJ *o, const char *app, const char *appname, const char *win )
{
	char	t[32];

	bpk_buf_puts(b, "{\"name\":\"");
	bpk_buf_json(b, o->name);
	bpk_buf_printf(b, "\",\"relationship\":[],\"linktype\":false,\"editable\":%s,\"deletable\":%s,"
		       "\"readable\":true,\"maker\":\"バックアップ\",\"periodDate\":",
		       ( o->st.f_type & 0x0010 ) ? "false" : "true", ( o->st.f_type & 0x0020 ) ? "false" : "true");
	if ( o->st.f_ltime != 0xFFFFFFFFU && bk_iso_of(o->st.f_ltime, t) > 0 ) bpk_buf_printf(b, "\"%s\"", t);
	else bpk_buf_puts(b, "null");
	if ( win != NULL ) bpk_buf_printf(b, ",%s", win);
	if ( app != NULL ) {
		bpk_buf_printf(b, ",\"applist\":{\"%s\":{\"name\":\"%s\",\"defaultOpen\":true}}", app, appname);
	}
	/* what BTRON said of it that the store keeps otherwise */
	bpk_buf_puts(b, ",\"tessronos\":{");
	btron_of(b, o);
	bpk_buf_puts(b, "}}");
}

/* What the archive said of an object: "btron":{...} */
LOCAL void btron_of( BPKBUF *b, const RSOBJ *o )
{
	char	t[32];

	bpk_buf_printf(b, "\"btron\":{\"objid\":%u,\"atype\":%u,\"ftype\":%u", o->objid, o->st.f_atype, o->st.f_type);
	if ( bk_iso_of(o->st.f_ctime, t) > 0 ) bpk_buf_printf(b, ",\"ctime\":\"%s\"", t);
	if ( bk_iso_of(o->st.f_mtime, t) > 0 ) bpk_buf_printf(b, ",\"mtime\":\"%s\"", t);
	if ( bk_iso_of(o->st.f_atime, t) > 0 ) bpk_buf_printf(b, ",\"atime\":\"%s\"", t);
	bpk_buf_putc(b, '}');
}

/*
 * The metadata TessronOS's own record carried, with what the archive said
 * of the object as "tessronos.btron": the store gives a restored object its
 * own dates, so the ones it had are kept there.
 */
LOCAL void meta_tf( BPKBUF *b, const RSOBJ *o, const char *meta, INT n )
{
	const char *tf = NULL;
	INT	i, e;

	for ( i = 0; i + 10 <= n; i++ ) {
		if ( memcmp(meta + i, "\"tessronos\":{", 10) == 0 ) {
			tf = meta + i + 10;
			break;
		}
	}
	if ( tf != NULL ) {
		bpk_buf_putn(b, meta, (INT)( tf - meta ));
		btron_of(b, o);
		if ( *tf != '}' ) bpk_buf_putc(b, ',');
		bpk_buf_putn(b, tf, n - (INT)( tf - meta ));
		return;
	}
	for ( e = n; e > 0 && meta[e - 1] != '}'; e-- ) ;
	if ( e == 0 ) {
		bpk_buf_putn(b, meta, n);
		return;
	}
	e--;
	bpk_buf_putn(b, meta, e);
	for ( i = e - 1; i > 0 && ( meta[i] == ' ' || meta[i] == '\n' || meta[i] == '\r' || meta[i] == '\t' ); i-- ) ;
	bpk_buf_puts(b, ( i > 0 && meta[i] != '{' ) ? ",\"tessronos\":{" : "\"tessronos\":{");
	btron_of(b, o);
	bpk_buf_puts(b, "}}");
}

/* The object of a first piece made, with a record 0 that says its name */
LOCAL ER make_obj( BKJOB *j, RSTATE *r, const BK_OBJHEAD *oh, RSOBJ **p_o )
{
	RSOBJ	*o;
	BPKBUF	b;
	T_OBCRE	c;
	INT	rec = -1;
	ID	k;
	ER	er;

	if ( r->nobj >= r->maxobj ) {
		INT	m = r->maxobj * 2 + 64;
		RSOBJ	*no = realloc(r->obj, sizeof(RSOBJ) * (size_t)m);

		if ( no == NULL ) return E_NOMEM;
		r->obj = no;
		r->maxobj = m;
	}
	o = &r->obj[r->nobj];
	memset(o, 0, sizeof(*o));
	o->objid = oh->objid;
	o->st = oh->st;
	o->privrec = o->execrec = o->currec = -1;
	bpk_tron_utf8(oh->name, BK_NAME_TC, o->name, sizeof(o->name));
	if ( o->name[0] == 0 ) strcpy(o->name, "名前なし");

	bpk_buf_init(&b);
	meta_btron(&b, o, NULL, NULL, NULL);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (const UB *)b.s;
	c.jsonsz = b.n;
	c.near = j->near;
	er = b.fail ? E_NOMEM : ob_cre_obj(&c, &o->uuid);
	bpk_buf_free(&b);
	if ( er < E_OK ) return er;
	r->nobj++;
	j->nmade++;
	if ( j->nmade == 1 ) j->root = o->uuid;

	k = ob_opn_obj(&o->uuid, OB_OP_R | OB_OP_W);
	if ( k <= 0 ) return ( k < 0 ) ? k : E_NOEXS;
	bpk_buf_init(&b);
	bpk_buf_puts(&b, "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>");
	bpk_buf_xml(&b, o->name);
	bpk_buf_puts(&b, "</p></document></tad>\n");
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = bk_write_all(k, rec, b.s, (UINT)b.n);
	bpk_buf_free(&b);
	ob_cls_obj(k);
	*p_o = o;
	return er;
}

/* TessronOS's own record, read back once whole: the types of the other records */
LOCAL void priv_read( RSOBJ *o, ID k )
{
	UB	h[PRIV_HEAD];
	SZ	asz = 0;
	UINT	ml, il, xl, nr, i;

	if ( o->privrec < 0 || o->tf ) return;
	if ( ob_rea_rec(k, o->privrec, 0, h, PRIV_HEAD, &asz) < E_OK || asz < PRIV_HEAD
	  || memcmp(h, BK_TS_MAGIC, 4) != 0 ) return;
	ml = h[4] | ( h[5] << 8 ) | ( h[6] << 16 ) | ( (UINT)h[7] << 24 );
	il = h[8] | ( h[9] << 8 ) | ( h[10] << 16 ) | ( (UINT)h[11] << 24 );
	xl = h[12] | ( h[13] << 8 ) | ( h[14] << 16 ) | ( (UINT)h[15] << 24 );
	nr = h[16] | ( h[17] << 8 ) | ( h[18] << 16 ) | ( (UINT)h[19] << 24 );
	if ( nr > 4096 ) return;
	o->privrt = calloc(nr + 1, 2 * sizeof(UH));
	if ( o->privrt == NULL ) return;
	if ( nr > 0 && ( ob_rea_rec(k, o->privrec, (D)PRIV_HEAD + ml + il + xl, o->privrt, (SZ)nr * 4, &asz) < E_OK
			 || asz != (SZ)nr * 4 ) ) {
		free(o->privrt);
		o->privrt = NULL;
		return;
	}
	for ( i = 0; i < nr * 2; i++ ) {
		UB	*p = (UB *)&o->privrt[i];

		o->privrt[i] = (UH)( p[0] | ( p[1] << 8 ) );
	}
	o->nprivrt = (INT)nr;
	o->tf = TRUE;
}

EXPORT ER bk_rest_volume( BKJOB *j, BK_SRC src, void *arg )
{
	RSTATE	*r = j->rest;
	BK_OBJHEAD oh;
	BK_FRAG	*f;
	BK_RECHEAD rh;
	INT	res;
	ER	er;

	if ( r == NULL ) return E_OBJ;
	if ( j->nvol == 0 ) bk_rd_init(&r->rd, NULL, 0);
	er = bk_rd_open(&r->rd, src, arg, &j->vh);
	if ( er < E_OK ) {
		seterr(j, ( er == E_OBJ ) ? "この巻は続きの巻ではありません" : "巻を読めません", er);
		return er;
	}
	if ( j->nvol == 0 ) {
		/* the table of objects: the head says how many the set has */
		r->nfrag = (INT)j->vh.nobj + 16;
		r->frag = calloc((size_t)r->nfrag, sizeof(BK_FRAG));
		if ( r->frag == NULL ) {
			seterr(j, "作業の場所がありません", E_NOMEM);
			return E_NOMEM;
		}
		r->rd.frag = r->frag;
		r->rd.nfrag = r->nfrag;
	}
	j->nvol++;
	j->more = j->vh.more;

	while ( ( res = bk_rd_object(&r->rd, &oh, &f) ) > 0 ) {
		RSOBJ	*o;
		ID	k;
		INT	cur = -1;		/* the record being written, -1 none */
		const char *what = "巻を読めません";
		UINT	rtype = 0;

		if ( j->stop ) {
			seterr(j, "中止しました", E_ABORT);
			return E_ABORT;
		}
		if ( ( oh.seg & 0x7FFF ) == 0 ) {
			er = make_obj(j, r, &oh, &o);
			if ( er < E_OK ) {
				seterr(j, "実身を作れません", er);
				return er;
			}
			f->user = (void *)(UBINT)( o - r->obj + 1 );
		} else {
			o = &r->obj[(UBINT)f->user - 1];
		}
		k = ob_opn_obj(&o->uuid, OB_OP_R | OB_OP_W);
		if ( k <= 0 ) {
			seterr(j, "作った実身を開けません", k);
			return ( k < 0 ) ? k : E_NOEXS;
		}
		while ( ( res = bk_rd_record(&r->rd, &rh) ) > 0 ) {
			INT	n;
			D	at = rh.offset;

			rtype = rh.type;
			if ( rh.offset == 0 ) {
				priv_read(o, k);	/* the record before may have been it */
				cur = -1;
				what = "レコードを足せません";
				if ( rh.type == 0 ) {
					BK_LINKDESC ld;

					er = bk_rd_link(&r->rd, &ld);
					if ( er < E_OK ) break;
					if ( o->nlink >= o->maxlink ) {
						INT	m = o->maxlink * 2 + 8;
						UINT	*nl = realloc(o->links, sizeof(UINT) * (size_t)m);
						UH	*na = realloc(o->latr, sizeof(UH) * (size_t)m);

						if ( nl != NULL ) o->links = nl;
						if ( na != NULL ) o->latr = na;
						if ( nl == NULL || na == NULL ) {
							er = E_NOMEM;
							break;
						}
						o->maxlink = m;
					}
					o->links[o->nlink] = ld.objid;
					o->latr[o->nlink] = ld.atr[0];
					o->nlink++;
					continue;
				}
				if ( rh.type == BK_RT_TESSRONOS && rh.subtype == BK_SUB_TESSRONOS && o->privrec < 0 ) {
					er = ob_apd_rec(k, OB_RT_SYSDATA, SUB_PRIV, &cur);
					if ( er >= E_OK ) o->privrec = cur;
				} else if ( rh.type == OB_RT_TAD && o->tf ) {
					cur = -1;	/* TessronOS's own record has it already */
				} else if ( rh.type == OB_RT_TAD && o->ntad < TAD_KEEP ) {
					er = ob_apd_rec(k, OB_RT_SYSDATA, SUB_TAD, &cur);
					if ( er >= E_OK ) o->tadrec[o->ntad++] = cur;
				} else if ( rh.type == OB_RT_MFUSEN && o->tf ) {
					cur = -1;	/* TessronOS keeps the application in the metadata */
				} else {
					UINT	rt = rh.type, sub = rh.subtype;

					if ( o->tf && o->nother < o->nprivrt ) {
						rt = o->privrt[2 * o->nother];
						sub = o->privrt[2 * o->nother + 1];
					}
					o->nother++;
					er = ob_apd_rec(k, rt, sub, &cur);
					if ( er < E_OK ) er = ob_apd_rec(k, OB_RT_SYSDATA, sub, &cur);
					if ( er >= E_OK && rh.type == OB_RT_MFUSEN && o->execrec < 0 ) o->execrec = cur;
				}
				if ( er < E_OK ) break;
				o->currec = cur;
			} else if ( cur < 0 ) {
				/* a record begun in the volume before */
				cur = o->currec;
			}
			/* the bytes of this piece */
			what = "レコードに書けません";
			while ( ( n = bk_rd_read(&r->rd, r->buf, CHUNK) ) > 0 ) {
				if ( cur >= 0 ) {
					SZ	asz = 0;

					er = ob_wri_rec(k, cur, at, r->buf, n, &asz);
					if ( er < E_OK ) break;
				}
				at += n;
			}
			if ( n < 0 ) {
				er = n;
				what = "巻を読めません";
			}
			if ( er < E_OK ) break;
			what = "巻を読めません";
		}
		priv_read(o, k);
		ob_cls_obj(k);
		if ( er < E_OK || res < 0 ) {
			if ( er >= E_OK ) er = res;
			snprintf(j->err, sizeof(j->err), "%s：%s の種別 %u のレコード (%d)",
				 ( er == E_OBJ ) ? "巻の中身が壊れています" : what, o->name, rtype, (INT)er);
			return er;
		}
		if ( j->tell != NULL ) {
			char	s[BK_NAME_MAX + 32];

			snprintf(s, sizeof(s), "実身を作っています：%s", o->name);
			j->tell(j, s, (UD)j->nmade, (UD)j->vh.nobj);
		}
	}
	if ( res < 0 ) {
		seterr(j, ( res == E_NOEXS ) ? "前の巻がありません" : "巻の中身が壊れています", res);
		return res;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the second pass */

typedef struct {
	BKJOB	*j;
	RSTATE	*r;
	RSOBJ	*o;
	ID	key;
	char	ids[40];		/* the target told last */
	char	self[40];		/* the object being written */
} CONV;

LOCAL const char *conv_target( void *ctx, INT n )
{
	CONV	*c = ctx;
	RSOBJ	*t = ( n >= 0 && n < c->o->nlink ) ? by_objid(c->r, c->o->links[n]) : NULL;

	if ( t == NULL ) {
		c->j->nlost++;
		return NULL;
	}
	bk_uuid_str(&t->uuid, c->ids);
	return c->ids;
}

LOCAL void conv_vobjid( void *ctx, char *out )
{
	TS_UUID	u;

	memset(&u, 0, sizeof(u));
	(void)ts_gen_uuid(&u);
	bk_uuid_str(&u, out);
}

LOCAL ER conv_picture( void *ctx, INT recno, INT n, const UB *png, INT len )
{
	CONV	*c = ctx;
	char	name[OB_RES_NAME];

	snprintf(name, sizeof(name), "_%d_%d.png", recno, n);
	return ob_wri_res(c->key, (const UB *)name, png, len);
}

/* An xmlTAD written as record rec, the record cut to its length */
LOCAL ER put_xml( ID k, INT rec, const char *s, INT n )
{
	ER	er = bk_write_all(k, rec, s, (UINT)n);

	if ( er >= E_OK ) er = ob_trn_rec(k, rec, (UD)n);
	return er;
}

/* The element of the tag at s, whose id is at i, left out: where it ends */
LOCAL INT elem_end( const char *x, INT n, INT i )
{
	INT	e;

	for ( e = i; e < n && x[e] != '>'; e++ ) ;
	if ( e > 0 && x[e - 1] != '/' ) {
		const char *end = strstr(x + e, "</link>");

		if ( end != NULL ) e = (INT)( end - x ) + 6;
	}
	return e;
}

/*
 * Each "@n" link of TessronOS's own xmlTAD given the UUID it names now, a
 * link to nothing left out; "@self" the object's own UUID. A link that
 * kept its UUID -- to an object left out of the save -- stays when that
 * object is still there, and goes when it is not.
 */
LOCAL void relink( CONV *c, const char *x, INT n, BPKBUF *b )
{
	INT	at = 0, i;

	for ( i = 0; i + 9 < n; i++ ) {
		INT	s, e, num = 0, q;
		RSOBJ	*t;

		if ( memcmp(x + i, "@self", 5) == 0 ) {
			/* the object's own UUID */
			bpk_buf_putn(b, x + at, i - at);
			bpk_buf_puts(b, c->self);
			at = i + 5;
			i += 4;
			continue;
		}
		if ( memcmp(x + i, "id=\"", 4) == 0 && x[i + 4] != '@' && i + 40 < n
		  && ( i == 0 || x[i - 1] == ' ' || x[i - 1] == '\t' || x[i - 1] == '\n' || x[i - 1] == '\r' ) ) {
			char	us[37];
			TS_UUID	u;
			T_OBREF	ref;

			for ( s = i; s > 0 && x[s] != '<'; s-- ) ;
			if ( strncmp(x + s, "<link", 5) != 0 ) continue;
			memcpy(us, x + i + 4, 36);
			us[36] = 0;
			if ( !bk_uuid_parse(us, &u) || ob_ref_obj(&u, &ref) >= E_OK ) continue;
			e = elem_end(x, n, i);
			bpk_buf_putn(b, x + at, s - at);
			at = e + 1;
			i = e;
			c->j->nlost++;
			continue;
		}
		if ( memcmp(x + i, "id=\"@", 5) != 0 ) continue;
		/* the tag it is in */
		for ( s = i; s > 0 && x[s] != '<'; s-- ) ;
		if ( strncmp(x + s, "<link", 5) != 0 ) continue;
		for ( q = i + 5; q < n && x[q] >= '0' && x[q] <= '9'; q++ ) num = num * 10 + ( x[q] - '0' );
		t = ( num < c->o->nlink ) ? by_objid(c->r, c->o->links[num]) : NULL;
		if ( t != NULL ) {
			char	id[40];

			bpk_buf_putn(b, x + at, i + 4 - at);
			bk_uuid_str(&t->uuid, id);
			bpk_buf_puts(b, id);
			at = q;
		} else {
			/* the whole element goes */
			e = elem_end(x, n, i);
			bpk_buf_putn(b, x + at, s - at);
			at = e + 1;
			c->j->nlost++;
		}
		i = q - 1;
	}
	bpk_buf_putn(b, x + at, n - at);
}

/* The links of an xmlTAD counted in their targets, for a store that does not */
LOCAL ER count_links( const char *xml )
{
	const char *p;
	ER	er;

	for ( p = xml; ( p = strstr(p, "<link id=\"") ) != NULL; p++ ) {
		TS_UUID	u;

		if ( !bk_uuid_parse(p + 10, &u) ) continue;
		er = ob_lnk_obj(&u);
		if ( er < E_OK ) return er;
	}
	return E_OK;
}

LOCAL UINT le32( const UB *p )
{
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (UINT)p[3] << 24 );
}

/*
 * Whether the metadata says the object was a root of the save, which
 * of them, and where it was linked from ("tessronos":{"backup":{...}}):
 * kept in r->root, and the member taken out of the metadata, which the
 * object is not to keep. *p_n is the metadata's new length.
 */
LOCAL void root_take( RSTATE *r, const RSOBJ *o, char *meta, INT *p_n )
{
	T_JSON	root, tf, bk, v, it;
	INT	n = *p_n, idx, k, a, e;
	UB	us[40];
	const char *key;

	if ( js_parse((const UB *)meta, n, &root) < E_OK || js_get(&root, "tessronos", &tf) < E_OK
	  || js_get(&tf, "backup", &bk) < E_OK || js_type(&bk) != JS_OBJECT ) return;
	idx = (INT)js_get_num(&bk, "root", -1);
	if ( idx >= 0 && idx < BK_ROOT_MAX ) {
		BKROOT	*rt = &r->root[idx];

		memset(rt, 0, sizeof(*rt));
		rt->uuid = o->uuid;
		if ( js_get_str(&bk, "parent", us, sizeof(us)) > 0 ) (void)bk_uuid_parse((const char *)us, &rt->parent);
		if ( js_get(&bk, "box", &v) >= E_OK && js_type(&v) == JS_ARRAY ) {
			for ( k = 0, it.s = NULL; k < 4 && js_next(&v, &it); k++ ) {
				D	d = 0;

				(void)js_num(&it, &d);
				rt->box[k] = (INT)d;
			}
			rt->hasbox = (BOOL)( k == 4 && rt->box[2] > rt->box[0] && rt->box[3] > rt->box[1] );
		}
		r->isroot[idx] = TRUE;
	}

	/* "backup":{...} out, with the comma before or after it */
	e = (INT)( (const char *)bk.s - meta ) + bk.len;
	for ( key = (const char *)bk.s - 1; key > meta && *key != '"'; key-- ) ;	/* the key's end */
	for ( key--; key > meta && *key != '"'; key-- ) ;				/* its start */
	a = (INT)( key - meta );
	for ( k = a - 1; k > 0 && ( meta[k] == ' ' || meta[k] == '\t' || meta[k] == '\r' || meta[k] == '\n' ); k-- ) ;
	if ( meta[k] == ',' ) {
		a = k;
	} else {
		for ( k = e; k < n && ( meta[k] == ' ' || meta[k] == '\t' || meta[k] == '\r' || meta[k] == '\n' ); k++ ) ;
		if ( k < n && meta[k] == ',' ) e = k + 1;
	}
	memmove(meta + a, meta + e, (size_t)( n - e ));
	*p_n = n - ( e - a );
}

/* An object TessronOS saved: its metadata, icon, resources and record 0 as they were */
LOCAL ER finish_tf( BKJOB *j, CONV *c, BPKBUF *xml )
{
	UB	*p;
	UINT	len = 0, ml, il, xl, nr, nres, at, i;
	ER	er = E_OK;

	p = bk_read_rec(c->key, c->o->privrec, &len);
	if ( p == NULL || len < PRIV_HEAD ) {
		free(p);
		return E_OBJ;
	}
	ml = le32(p + 4);
	il = le32(p + 8);
	xl = le32(p + 12);
	nr = le32(p + 16);
	nres = le32(p + 20);
	at = PRIV_HEAD;
	if ( (UD)at + ml + il + xl + (UD)nr * 4 > len ) {
		free(p);
		return E_OBJ;
	}
	if ( ml > 2 ) {
		BPKBUF	m;
		INT	mn = (INT)ml;

		root_take(c->r, c->o, (char *)p + at, &mn);
		bpk_buf_init(&m);
		meta_tf(&m, c->o, (const char *)p + at, mn);
		er = m.fail ? E_NOMEM : ob_set_atr(c->key, (const UB *)m.s, m.n);
		bpk_buf_free(&m);
	}
	at += ml;
	if ( er >= E_OK && il > 0 ) er = ob_set_ico(c->key, p + at, il);
	at += il;
	relink(c, (const char *)p + at, (INT)xl, xml);
	at += xl + nr * 4;
	for ( i = 0; er >= E_OK && i < nres && at + 2 <= len; i++ ) {
		UINT	nl = p[at] | ( p[at + 1] << 8 ), dl;
		char	name[OB_RES_NAME];

		at += 2;
		if ( at + nl + 4 > len || nl >= OB_RES_NAME ) break;
		memcpy(name, p + at, nl);
		name[nl] = 0;
		at += nl;
		dl = le32(p + at);
		at += 4;
		if ( at + dl > len ) break;
		er = ob_wri_res(c->key, (const UB *)name, p + at, dl);
		at += dl;
	}
	free(p);
	(void)j;
	return er;
}

/* The application a 実行機能付箋 names, and its window */
LOCAL void exec_of( ID k, INT rec, const char **p_id, const char **p_name, char *win, INT max )
{
	UB	e[160];
	SZ	asz = 0;
	UH	appl[3];

	*p_id = NULL;
	win[0] = 0;
	if ( rec < 0 || ob_rea_rec(k, rec, 0, e, sizeof(e), &asz) < E_OK || asz < 96 ) return;
	appl[0] = (UH)( e[24] | ( e[25] << 8 ) );
	appl[1] = (UH)( e[26] | ( e[27] << 8 ) );
	appl[2] = (UH)( e[28] | ( e[29] << 8 ) );
	*p_id = bpk_appl_prog(appl, p_name);
	if ( asz >= 96 + 12 ) {
		INT	l = (H)( e[100] | ( e[101] << 8 ) ), t = (H)( e[102] | ( e[103] << 8 ) );
		INT	r = (H)( e[104] | ( e[105] << 8 ) ), b = (H)( e[106] | ( e[107] << 8 ) );

		if ( r > l && b > t ) {
			snprintf(win, (size_t)max, "\"window\":{\"pos\":{\"x\":%d,\"y\":%d},\"width\":%d,\"height\":%d}",
				 l, t, r - l, b - t);
		}
	}
}

/* An object BTRON saved: record 0 from its first TAD, the rest of its TAD after */
LOCAL ER finish_btron( BKJOB *j, CONV *c, BPKBUF *xml, BPKCONV *cv )
{
	RSOBJ		*o = c->o;
	const char	*app = NULL, *appname = NULL;
	char		win[160];
	BPKBUF		m;
	INT		t;
	ER		er = E_OK;

	exec_of(c->key, o->execrec, &app, &appname, win, sizeof(win));
	if ( app == NULL ) {
		app = ( o->st.f_atype == 6 ) ? "virtual-object-list"
		    : ( o->st.f_atype == 2 ) ? "basic-figure-editor" : "basic-text-editor";
		appname = ( o->st.f_atype == 6 ) ? "仮身一覧" : ( o->st.f_atype == 2 ) ? "基本図形編集" : "基本文章編集";
	}
	bpk_conv_object(cv);
	for ( t = 0; t < o->ntad && er >= E_OK; t++ ) {
		UB	*tad;
		UINT	len = 0;

		tad = bk_read_rec(c->key, o->tadrec[t], &len);
		if ( tad == NULL ) {
			er = E_IO;
			break;
		}
		if ( t == 0 ) {
			if ( bpk_tad_is_fig(tad, len) && o->st.f_atype != 6 && o->execrec < 0 ) {
				app = "basic-figure-editor";
				appname = "基本図形編集";
			}
			er = bpk_tad_to_xml(cv, o->name, c->self, 0, tad, len, xml);
		} else {
			BPKBUF	more;
			INT	rec = -1;

			bpk_buf_init(&more);
			er = bpk_tad_to_xml(cv, o->name, c->self, t, tad, len, &more);
			if ( er >= E_OK ) er = ob_apd_rec(c->key, OB_RT_TAD, 0, &rec);
			if ( er >= E_OK ) er = put_xml(c->key, rec, more.s, more.n);
			if ( er >= E_OK && !((RSTATE *)j->rest)->autoref ) er = count_links(more.s);
			bpk_buf_free(&more);
		}
		free(tad);
		j->nconv++;
	}
	bpk_buf_init(&m);
	meta_btron(&m, o, app, appname, win[0] ? win : NULL);
	if ( er >= E_OK ) er = m.fail ? E_NOMEM : ob_set_atr(c->key, (const UB *)m.s, m.n);
	bpk_buf_free(&m);
	return er;
}

EXPORT ER bk_rest_end( BKJOB *j )
{
	RSTATE	*r = j->rest;
	BPKCONV	*cv;
	BPKHOOK	hook;
	CONV	c;
	INT	i;
	ER	er = E_OK;

	if ( r == NULL ) return E_OBJ;
	if ( bk_rd_close(&r->rd) < E_OK ) {
		seterr(j, "巻がそろっていません", E_OBJ);
		return E_OBJ;
	}
	{
		T_OBREF	ref;

		r->autoref = ( r->nobj > 0 && ob_ref_obj(&r->obj[0].uuid, &ref) >= E_OK
			       && ( ref.flags & OB_F_AUTOREF ) != 0 );
	}
	cv = malloc(sizeof(BPKCONV));
	if ( cv == NULL ) {
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	memset(&c, 0, sizeof(c));
	c.j = j;
	c.r = r;
	hook.link_target = conv_target;
	hook.new_vobjid = conv_vobjid;
	hook.picture = conv_picture;
	hook.ctx = &c;
	bpk_conv_init(cv, &hook);

	for ( i = 0; i < r->nobj && er >= E_OK; i++ ) {
		RSOBJ	*o = &r->obj[i];
		BPKBUF	xml;
		INT	q, del[TAD_KEEP + 1], ndel = 0;

		if ( j->stop ) {
			er = E_ABORT;
			seterr(j, "中止しました", er);
			break;
		}
		c.o = o;
		c.key = ob_opn_obj(&o->uuid, OB_OP_ALL);
		if ( c.key <= 0 ) {
			er = ( c.key < 0 ) ? c.key : E_NOEXS;
			seterr(j, "作った実身を開けません", er);
			break;
		}
		bk_uuid_str(&o->uuid, c.self);
		bpk_buf_init(&xml);
		er = o->tf ? finish_tf(j, &c, &xml) : finish_btron(j, &c, &xml, cv);
		if ( er >= E_OK && xml.n > 0 ) {
			er = xml.fail ? E_NOMEM : put_xml(c.key, 0, xml.s, xml.n);
			if ( er >= E_OK && !r->autoref ) er = count_links(xml.s);
		}
		if ( er >= E_OK ) {
			const char *p;

			for ( p = xml.s; p != NULL && ( p = strstr(p, "<link ") ) != NULL; p++ ) j->nlinked++;
		}
		bpk_buf_free(&xml);

		/* the records held for the work go, the last first */
		for ( q = 0; q < o->ntad; q++ ) del[ndel++] = o->tadrec[q];
		if ( o->privrec >= 0 ) del[ndel++] = o->privrec;
		while ( er >= E_OK && ndel > 0 ) {
			INT	mx = 0, t;

			for ( t = 1; t < ndel; t++ ) if ( del[t] > del[mx] ) mx = t;
			er = ob_del_rec(c.key, del[mx]);
			del[mx] = del[--ndel];
		}
		ob_cls_obj(c.key);
		if ( er < E_OK && j->err[0] == 0 ) seterr(j, "記録を書けません", er);
		if ( j->tell != NULL ) {
			char	s[BK_NAME_MAX + 40];

			snprintf(s, sizeof(s), "仮身をつないでいます：%s", o->name);
			j->tell(j, s, (UD)i + 1, (UD)r->nobj);
		}
	}
	if ( cv->er < E_OK && er >= E_OK ) {
		er = cv->er;
		seterr(j, "画像を書けません", er);
	}
	free(cv);

	/* the roots: those TessronOS's records say, in their order; else the first object */
	j->nrroot = 0;
	for ( i = 0; i < BK_ROOT_MAX; i++ ) {
		if ( r->isroot[i] ) j->rroot[j->nrroot++] = r->root[i];
	}
	if ( j->nrroot == 0 && r->nobj > 0 ) {
		memset(&j->rroot[0], 0, sizeof(BKROOT));
		j->rroot[0].uuid = r->obj[0].uuid;
		j->nrroot = 1;
	}
	if ( j->nrroot > 0 ) j->root = j->rroot[0].uuid;
	return er;
}

EXPORT void bk_rest_undo( BKJOB *j )
{
	RSTATE	*r = j->rest;
	INT	i;

	if ( r == NULL ) return;
	for ( i = r->nobj - 1; i >= 0; i-- ) (void)ob_del_obj(&r->obj[i].uuid);
	r->nobj = 0;
	j->nmade = 0;
}

EXPORT void bk_rest_free( BKJOB *j )
{
	RSTATE	*r = j->rest;
	INT	i;

	if ( r == NULL ) return;
	for ( i = 0; i < r->nobj; i++ ) {
		free(r->obj[i].links);
		free(r->obj[i].latr);
		free(r->obj[i].privrt);
	}
	free(r->obj);
	free(r->frag);
	free(r);
	j->rest = NULL;
}

/* ---------------------------------------------------------------- looking in */

LOCAL ER read_n( BK_SRC src, void *arg, UB *buf, INT n )
{
	while ( n > 0 ) {
		INT	k = src(arg, buf, n);

		if ( k <= 0 ) return ( k < 0 ) ? k : E_NOEXS;
		buf += k;
		n -= k;
	}
	return E_OK;
}

EXPORT ER bk_rest_peek( BK_SRC src, void *arg, BK_VOLHEAD *vh, char (*names)[BK_NAME_MAX],
			INT max, INT *p_n )
{
	static UB	skip[CHUNK];
	BK_RD	*rd = malloc(sizeof(BK_RD));
	BK_FRAG	fr[1];
	UB	h[16 + BK_META_SIZE];
	INT	n = 0;
	ER	er;

	*p_n = 0;
	if ( rd == NULL ) return E_NOMEM;
	bk_rd_init(rd, fr, 1);
	rd->loose = TRUE;
	er = bk_rd_open(rd, src, arg, vh);
	free(rd);
	if ( er < E_OK ) return er;
	for ( ;; ) {
		UINT	llen, rest;
		UH	name[BK_NAME_TC];
		BK_FSTATE st;

		er = read_n(src, arg, h, 4);
		if ( er == E_NOEXS ) break;
		if ( er < E_OK ) return er;
		if ( h[0] != 0xFD || h[1] != 0xFF ) return E_OBJ;
		if ( h[2] == 0xFF && h[3] == 0xFF ) {
			er = read_n(src, arg, h, 4);
			if ( er < E_OK ) return er;
			llen = le32(h);
		} else {
			llen = (UINT)( h[2] | ( h[3] << 8 ) );
		}
		if ( llen < BK_OBJ_META ) return E_OBJ;
		er = read_n(src, arg, h, BK_OBJ_META);
		if ( er < E_OK ) return er;
		bk_meta_get(h + 8, name, &st);
		if ( n < max && ( h[10] | ( h[11] << 8 ) ) == 0 ) {
			bpk_tron_utf8(name, BK_NAME_TC, names[n], BK_NAME_MAX);
			n++;
		}
		for ( rest = llen - BK_OBJ_META; rest > 0; ) {
			INT	k = ( rest > CHUNK ) ? CHUNK : (INT)rest;

			er = read_n(src, arg, skip, k);
			if ( er < E_OK ) return E_OBJ;
			rest -= (UINT)k;
		}
	}
	*p_n = n;
	return E_OK;
}
