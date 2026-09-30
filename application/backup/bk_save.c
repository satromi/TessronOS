/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_save.c
 *	バックアップ: objects written as a backup (design 17.18)
 *
 *	The objects are the roots, those they link to, and those these link
 *	to, each once, the roots first. An object left out is not saved, and
 *	neither is what only it links to; a link to it stays a link to it.
 *	Every object is numbered from 1, and its number with the file
 *	system's (1) is the objid a link record gives for it.
 *
 *	A root's TessronOS record says it is one, and which object's record
 *	linked to it and where ("tessronos":{"backup":{...}} in its metadata),
 *	for a restore to put it back there. BTRON has no place for that.
 *
 *	An object's records in the archive, in order:
 *	  - TessronOS's own record (BK_RT_TESSRONOS): the metadata, the icon, the
 *	    xmlTAD of record 0 with each link as its number, the resources and
 *	    the types of the other records. BTRON keeps it as it is; restored
 *	    here, it gives back what BTRON has no place for;
 *	  - the 実行機能付箋 (RT 8): the application that opens the object and
 *	    its window, or the object's own record of that type;
 *	  - a link record for each <link> of record 0, in its order;
 *	  - record 0 as binary TAD;
 *	  - every other record as it is (an xmlTAD record as bytes, RT 15).
 *	The link records come before the TAD, so the n-th TS_VOBJ takes the
 *	n-th of them. The totals of the volume head are what these add up to,
 *	measured before anything is written.
 */

#include "bk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/json.h>

#define FSIDX		1		/* the file system's number in an objid */
#define BSIZE		32768		/* the block the totals count in */
#define EXEC_DLEN	40		/* data of the 実行機能付箋 */
#define EXEC_SIZE	( 96 + EXEC_DLEN )
#define LINK_GROW	16

/* ---------------------------------------------------------------- what each object is */

typedef struct {
	UH	appl;
	const char *id;
	const char *name;
	UH	atype;			/* BTRON's application type */
	UINT	bgoff;			/* where the data keeps the window's colour */
} APPL;

LOCAL const APPL appls[] = {
	{ 0x0001, "virtual-object-list", "仮身一覧", 6, 0 },
	{ 0x0002, "basic-figure-editor", "基本図形編集", 2, 0x1E },
	{ 0x0003, "basic-text-editor", "基本文章編集", 1, 0x16 },
	{ 0x0009, "basic-calc-editor", "基本表計算", 1, 0x16 },
	{ 0x000B, "microscript", "マイクロスクリプト", 2, 0x1E },
	{ BPK_APPL_ARCHIVE, "unpack-file", "書庫解凍", 1, 0 },
};
#define NAPPL	( (INT)( sizeof(appls) / sizeof(appls[0]) ) )

/* What the walk and the writing keep of an object besides BKOBJ */
typedef struct {
	TS_UUID	*target;		/* the UUID each link names */
	INT	maxlink;
	const APPL *appl;
	UINT	winx, winy, winw, winh, bg;
	BOOL	haswin, hasbg;
	INT	execrec;		/* its own 実行機能付箋, -1 none */
	UINT	privlen;		/* bytes of TessronOS's own record */
	INT	nother;			/* records beside 0 and the 実行機能付箋 */
	INT	rootof;			/* which of the roots it is, -1 none */
} EXTRA;

LOCAL EXTRA	*extra;

LOCAL void seterr( BKJOB *j, const char *s, ER er )
{
	snprintf(j->err, sizeof(j->err), "%s (%d)", s, (INT)er);
}

/* Whether an object is one left out of the save */
LOCAL BOOL excluded( BKJOB *j, const TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < j->nexcl && j->excl != NULL; i++ ) {
		if ( memcmp(&j->excl[i], u, sizeof(TS_UUID)) == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL INT find_obj( BKJOB *j, const TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < j->nobj; i++ ) {
		if ( memcmp(&j->obj[i].uuid, u, sizeof(TS_UUID)) == 0 ) return i;
	}
	return -1;
}

LOCAL INT add_obj( BKJOB *j, const TS_UUID *u )
{
	BKOBJ	*o;

	if ( j->nobj >= j->maxobj ) {
		INT	m = j->maxobj * 2 + 32;
		BKOBJ	*no = realloc(j->obj, sizeof(BKOBJ) * (size_t)m);
		EXTRA	*ne = realloc(extra, sizeof(EXTRA) * (size_t)m);

		if ( no == NULL || ne == NULL ) {
			if ( no != NULL ) j->obj = no;
			if ( ne != NULL ) extra = ne;
			return -1;
		}
		j->obj = no;
		extra = ne;
		j->maxobj = m;
	}
	if ( j->nobj >= 0xFFFF ) return -1;
	o = &j->obj[j->nobj];
	memset(o, 0, sizeof(*o));
	memset(&extra[j->nobj], 0, sizeof(EXTRA));
	extra[j->nobj].rootof = -1;
	o->uuid = *u;
	o->fid = (UINT)j->nobj + 1;
	o->objid = ( o->fid << 16 ) | FSIDX;
	return j->nobj++;
}

/* ---------------------------------------------------------------- metadata */

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

LOCAL BOOL key_is( const UB *k, INT n, const char *w )
{
	return (BOOL)( (INT)strlen(w) == n && memcmp(k, w, (size_t)n) == 0 );
}

/*
 * The metadata to carry: every member but those the store keeps itself
 * and, of "tessronos", the order of the records and the protection, which
 * belong to the system the object is in. add (NULL: nothing) is put in
 * "tessronos" as a member more, "tessronos" made for it when there is none.
 */
LOCAL void meta_filter( BPKBUF *b, const UB *text, INT len, const char *add )
{
	T_JSON	root, it, it2;
	const UB *k;
	INT	n, first = 1;
	BOOL	added = FALSE;

	if ( js_parse(text, len, &root) < E_OK || js_type(&root) != JS_OBJECT ) {
		if ( add != NULL ) bpk_buf_printf(b, "{\"tessronos\":{%s}}", add);
		else bpk_buf_puts(b, "{}");
		return;
	}
	bpk_buf_putc(b, '{');
	for ( it.s = NULL; js_next(&root, &it); ) {
		if ( !key_of(text, it.s, &k, &n) ) continue;
		if ( key_is(k, n, "refCount") || key_is(k, n, "recordCount") || key_is(k, n, "makeDate")
		  || key_is(k, n, "updateDate") || key_is(k, n, "accessDate") ) continue;
		if ( !first ) bpk_buf_putc(b, ',');
		first = 0;
		bpk_buf_putc(b, '"');
		bpk_buf_putn(b, (const char *)k, n);
		bpk_buf_puts(b, "\":");
		if ( key_is(k, n, "tessronos") && js_type(&it) == JS_OBJECT ) {
			INT	f2 = 1;

			bpk_buf_putc(b, '{');
			for ( it2.s = NULL; js_next(&it, &it2); ) {
				const UB *k2;
				INT	n2;

				if ( !key_of(text, it2.s, &k2, &n2) ) continue;
				if ( key_is(k2, n2, "records") || key_is(k2, n2, "access") ) continue;
				if ( !f2 ) bpk_buf_putc(b, ',');
				f2 = 0;
				bpk_buf_putc(b, '"');
				bpk_buf_putn(b, (const char *)k2, n2);
				bpk_buf_puts(b, "\":");
				bpk_buf_putn(b, (const char *)it2.s, it2.len);
			}
			if ( add != NULL ) {
				if ( !f2 ) bpk_buf_putc(b, ',');
				bpk_buf_puts(b, add);
				added = TRUE;
			}
			bpk_buf_putc(b, '}');
		} else {
			bpk_buf_putn(b, (const char *)it.s, it.len);
		}
	}
	if ( add != NULL && !added ) {
		if ( !first ) bpk_buf_putc(b, ',');
		bpk_buf_printf(b, "\"tessronos\":{%s}", add);
	}
	bpk_buf_putc(b, '}');
}

/* The application that opens it first, by its applist; NULL none known */
LOCAL const APPL *appl_of( const UB *text, INT len )
{
	T_JSON	root, al, it;
	const UB *k;
	INT	n, i;
	const APPL *first = NULL;

	if ( js_parse(text, len, &root) < E_OK || js_get(&root, "applist", &al) < E_OK ) return NULL;
	for ( it.s = NULL; js_next(&al, &it); ) {
		if ( !key_of(text, it.s, &k, &n) ) continue;
		for ( i = 0; i < NAPPL; i++ ) {
			if ( !key_is(k, n, appls[i].id) ) continue;
			if ( js_get_bool(&it, "defaultOpen", FALSE) ) return &appls[i];
			if ( first == NULL ) first = &appls[i];
		}
	}
	return first;
}

LOCAL UINT colour_of( const char *s )
{
	UINT	v = 0;
	INT	i;

	if ( s[0] != '#' ) return 0x10FFFFFF;
	for ( i = 1; i <= 6; i++ ) {
		char	c = s[i];

		v = v * 16 + (UINT)( ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
				     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : 0 );
	}
	return 0x10000000U | v;
}

/* ---------------------------------------------------------------- the walk */

/* The pixels of a picture of the object being measured or written */
typedef struct {
	ID	key;
	UINT	*px;
} PICS;

/* What the hooks of a walk share: the pictures first, for pic_image */
typedef struct {
	PICS	pics;
	BKJOB	*j;
	INT	at;			/* the object whose links are told */
} WALK;

LOCAL ER walk_link( void *arg, CONST BK_TADLINK *lk )
{
	WALK	*w = arg;
	BKOBJ	*o = &w->j->obj[w->at];
	EXTRA	*x = &extra[w->at];
	TS_UUID	u;

	if ( o->nlink >= x->maxlink ) {
		INT	m = x->maxlink * 2 + LINK_GROW;
		TS_UUID	*nt = realloc(x->target, sizeof(TS_UUID) * (size_t)m);
		UH	*na = realloc(o->latr, sizeof(UH) * (size_t)m);

		if ( nt != NULL ) x->target = nt;
		if ( na != NULL ) o->latr = na;
		if ( nt == NULL || na == NULL ) return E_NOMEM;
		x->maxlink = m;
	}
	memset(&u, 0, sizeof(u));
	(void)bk_uuid_parse((const char *)lk->target, &u);
	x->target[o->nlink] = u;
	o->latr[o->nlink] = lk->attr;
	o->nlink++;
	return E_OK;
}


LOCAL ER pic_image( void *arg, CONST UB *href, CONST UINT **p_px, INT *p_w, INT *p_h )
{
	PICS	*p = arg;
	const char *u = strchr((const char *)href, '_');
	UB	*data;
	SZ	asz = 0, size = 0;
	INT	w = 0, h = 0;
	UB	probe[64];
	ER	er;

	if ( u == NULL || p->key <= 0 ) return E_NOEXS;
	/* the resource: its size first */
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

/* An object's metadata read: name, applist, window; FALSE when it is none to save */
LOCAL BOOL read_meta( BKJOB *j, INT i, ID k, UB *meta, SZ size )
{
	BKOBJ	*o = &j->obj[i];
	EXTRA	*x = &extra[i];
	T_JSON	root, win, pos;
	UB	col[16];

	if ( js_parse(meta, (INT)size, &root) < E_OK ) return FALSE;
	(void)js_get_str(&root, "name", (UB *)o->name, sizeof(o->name));
	x->appl = appl_of(meta, (INT)size);
	if ( js_get(&root, "window", &win) >= E_OK ) {
		if ( js_get(&win, "pos", &pos) >= E_OK ) {
			x->winx = (UINT)js_get_num(&pos, "x", 100);
			x->winy = (UINT)js_get_num(&pos, "y", 100);
			x->haswin = TRUE;
		}
		x->winw = (UINT)js_get_num(&win, "width", 600);
		x->winh = (UINT)js_get_num(&win, "height", 400);
		if ( js_get_str(&win, "backgroundColor", col, sizeof(col)) > 0 ) {
			x->bg = colour_of((const char *)col);
			x->hasbg = TRUE;
		}
	}
	/* F_STATE: the protection BTRON has, the times it keeps */
	o->st.f_type = 0x1000;
	if ( !js_get_bool(&root, "editable", TRUE) ) o->st.f_type |= 0x0010;	/* F_RONLY */
	if ( !js_get_bool(&root, "deletable", TRUE) ) o->st.f_type |= 0x0020;	/* F_PERM */
	o->st.f_pubacc = 0x0FFF;
	{
		UB	t[40];

		o->st.f_ltime = ( js_get_str(&root, "periodDate", t, sizeof(t)) > 0 )
				? bk_stime_of((const char *)t) : 0xFFFFFFFFU;
		if ( o->st.f_ltime == 0 ) o->st.f_ltime = 0xFFFFFFFFU;
		o->st.f_ctime = ( js_get_str(&root, "makeDate", t, sizeof(t)) > 0 ) ? bk_stime_of((const char *)t) : 0;
		o->st.f_mtime = ( js_get_str(&root, "updateDate", t, sizeof(t)) > 0 ) ? bk_stime_of((const char *)t) : 0;
		o->st.f_atime = ( js_get_str(&root, "accessDate", t, sizeof(t)) > 0 ) ? bk_stime_of((const char *)t) : 0;
	}
	(void)k;
	return TRUE;
}

/* ---------------------------------------------------------------- TessronOS's own record */

LOCAL void put32le( BPKBUF *b, UINT v )
{
	char	c[4];

	c[0] = (char)v;
	c[1] = (char)( v >> 8 );
	c[2] = (char)( v >> 16 );
	c[3] = (char)( v >> 24 );
	bpk_buf_putn(b, c, 4);
}

LOCAL void put16le( BPKBUF *b, UINT v )
{
	char	c[2];

	c[0] = (char)v;
	c[1] = (char)( v >> 8 );
	bpk_buf_putn(b, c, 2);
}

typedef struct {
	const UB *tag;
	INT	n;
	BOOL	keep;			/* it points at an object left out */
} TAGAT;

typedef struct {
	TAGAT	*at;
	INT	n, max;
	BKJOB	*j;
} TAGS;

LOCAL ER tag_link( void *arg, CONST BK_TADLINK *lk )
{
	TAGS	*t = arg;

	if ( t->n >= t->max ) {
		INT	m = t->max * 2 + 16;
		TAGAT	*na = realloc(t->at, sizeof(TAGAT) * (size_t)m);

		if ( na == NULL ) return E_NOMEM;
		t->at = na;
		t->max = m;
	}
	t->at[t->n].tag = lk->tag;
	t->at[t->n].n = lk->taglen;
	t->at[t->n].keep = FALSE;
	if ( t->j != NULL && t->j->nexcl > 0 ) {
		TS_UUID	u;

		memset(&u, 0, sizeof(u));
		t->at[t->n].keep = (BOOL)( bk_uuid_parse((const char *)lk->target, &u) && excluded(t->j, &u) );
	}
	t->n++;
	return E_OK;
}

/*
 * The xmlTAD with the id of each link written as "@n", n its link
 * record, and the object's own UUID (the pictures it keeps, "uuid_0_0.png")
 * as "@self": both name other objects once it is restored. A link to an
 * object left out keeps its UUID: restored, it points at that object if
 * it is still there.
 */
LOCAL void xml_numbered( BKJOB *j, BPKBUF *b, const UB *xml, UINT len, const char *self )
{
	BK_TADW	*w = malloc(sizeof(BK_TADW));
	TAGS	t = { NULL, 0, 0, NULL };
	BK_TADHOOK hk = { tag_link, NULL, NULL };
	INT	n = 0, i;
	UINT	at = 0;

	hk.arg = &t;
	t.j = j;
	if ( w == NULL || bk_tad_from_xml(w, xml, (INT)len, NULL, 0, &n, &hk, NULL) < E_OK ) t.n = 0;
	free(w);
	for ( i = 0; i < t.n; i++ ) {
		const UB *s = t.at[i].tag, *e = s + t.at[i].n, *p;

		if ( t.at[i].keep ) continue;		/* left out: named as it is */

		/* the value of id="..." in the tag */
		for ( p = s; p + 4 < e; p++ ) {
			if ( p[0] == 'i' && p[1] == 'd' && p[2] == '=' && ( p[3] == '"' || p[3] == '\'' )
			  && ( p == s || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r' ) ) break;
		}
		if ( p + 4 >= e ) continue;
		{
			const UB *v = p + 4, *ve = v;
			UB	q = p[3];
			char	num[16];

			while ( ve < e && *ve != q ) ve++;
			if ( ve - v > 36 ) {
				/* a UUID with more after it ("_0.xtad"): only the UUID is numbered */
				char	us[37];
				TS_UUID	u;

				memcpy(us, v, 36);
				us[36] = 0;
				if ( bk_uuid_parse(us, &u) ) ve = v + 36;
			}
			bpk_buf_putn(b, (const char *)xml + at, (INT)( v - xml - at ));
			snprintf(num, sizeof(num), "@%d", i);
			bpk_buf_puts(b, num);
			at = (UINT)( ve - xml );
		}
	}
	bpk_buf_putn(b, (const char *)xml + at, (INT)( len - at ));
	free(t.at);

	/* the object's own UUID */
	if ( !b->fail && b->n > 0 ) {
		INT	k = (INT)strlen(self), i, o = 0;

		for ( i = 0; i + k <= b->n; ) {
			if ( memcmp(b->s + i, self, (size_t)k) == 0 ) {
				memcpy(b->s + o, "@self", 5);
				o += 5;
				i += k;
			} else {
				b->s[o++] = b->s[i++];
			}
		}
		while ( i < b->n ) b->s[o++] = b->s[i++];
		b->n = o;
	}
}

/*
 * "TFBK", then the lengths of the metadata, the icon and the xmlTAD, the
 * number of the other records and of the resources; then those bytes;
 * the type and subtype of each other record; each resource as the
 * length of its name, the name, the length of its bytes and the bytes.
 */
LOCAL ER make_private( BKJOB *j, INT i, ID k, BPKBUF *b )
{
	static UB	meta[OB_ATR_MAX + 1];
	BPKBUF		m, x;
	UB		*ico = NULL, *xml = NULL, *names = NULL;
	SZ		asz = 0, icolen = 0;
	UINT		xlen = 0;
	T_OBREC		r[64];
	INT		cnt = 0, nres = 0, q, nother = 0;
	char		add[200];

	bpk_buf_init(&m);
	bpk_buf_init(&x);
	add[0] = 0;
	if ( extra[i].rootof >= 0 ) {
		/* a root: which, and where it was linked from */
		const BKROOT *r = &j->sroot[extra[i].rootof];
		char	pu[40];
		static const TS_UUID zero;

		snprintf(add, sizeof(add), "\"backup\":{\"root\":%d", extra[i].rootof);
		if ( memcmp(&r->parent, &zero, sizeof(zero)) != 0 ) {
			bk_uuid_str(&r->parent, pu);
			snprintf(add + strlen(add), sizeof(add) - strlen(add), ",\"parent\":\"%s\"", pu);
			if ( r->hasbox ) {
				snprintf(add + strlen(add), sizeof(add) - strlen(add), ",\"box\":[%d,%d,%d,%d]",
					 r->box[0], r->box[1], r->box[2], r->box[3]);
			}
		}
		strcat(add, "}");
	}
	if ( ob_get_atr(k, meta, OB_ATR_MAX, &asz) >= E_OK ) meta_filter(&m, meta, (INT)asz, add[0] ? add : NULL);
	else meta_filter(&m, (const UB *)"{}", 2, add[0] ? add : NULL);
	ico = malloc(OB_ICO_MAX);
	if ( ico != NULL && ( ob_get_ico(k, ico, OB_ICO_MAX, &icolen) < E_OK || icolen < 0 ) ) icolen = 0;
	xml = bk_read_rec(k, 0, &xlen);
	if ( xml != NULL ) {
		char	self[40];

		bk_uuid_str(&j->obj[i].uuid, self);
		xml_numbered(j, &x, xml, xlen, self);
	}
	(void)ob_lst_rec(k, r, 64, &cnt);
	for ( q = 0; q < cnt && q < 64; q++ ) {
		if ( r[q].recno != 0 && r[q].recno != extra[i].execrec ) nother++;
	}
	names = malloc(64 * OB_RES_NAME);
	if ( names != NULL && ob_lst_res(k, names, 64 * OB_RES_NAME, &nres) < E_OK ) nres = 0;

	bpk_buf_puts(b, BK_TS_MAGIC);
	put32le(b, (UINT)m.n);
	put32le(b, (UINT)icolen);
	put32le(b, (UINT)x.n);
	put32le(b, (UINT)nother);
	put32le(b, (UINT)nres);
	bpk_buf_putn(b, m.s, m.n);
	if ( icolen > 0 ) bpk_buf_putn(b, (const char *)ico, (INT)icolen);
	bpk_buf_putn(b, x.s, x.n);
	for ( q = 0; q < cnt && q < 64; q++ ) {
		if ( r[q].recno == 0 || r[q].recno == extra[i].execrec ) continue;
		put16le(b, r[q].rt);
		put16le(b, r[q].sub);
	}
	{
		const char *nm = (const char *)names;
		INT	t;

		for ( t = 0; t < nres && nm != NULL; t++ ) {
			INT	nl = (INT)strlen(nm);
			UB	*data;
			SZ	size;

			for ( size = 64 * 1024; ; size *= 2 ) {
				data = malloc((size_t)size);
				asz = 0;
				if ( data == NULL || ob_rea_res(k, (const UB *)nm, 0, data, size, &asz) < E_OK ) {
					asz = 0;
					break;
				}
				if ( asz < size || size >= 16 * 1024 * 1024 ) break;
				free(data);
			}
			put16le(b, (UINT)nl);
			bpk_buf_putn(b, nm, nl);
			put32le(b, (UINT)asz);
			if ( asz > 0 ) bpk_buf_putn(b, (const char *)data, (INT)asz);
			free(data);
			nm += nl + 1;
		}
	}
	extra[i].nother = nother;
	j->obj[i].nres = nres;
	free(ico);
	free(xml);
	free(names);
	bpk_buf_free(&m);
	bpk_buf_free(&x);
	return b->fail ? E_NOMEM : E_OK;
}

/* ---------------------------------------------------------------- the 実行機能付箋 */

LOCAL void make_exec( BKJOB *j, INT i, UB *e )
{
	EXTRA		*x = &extra[i];
	const APPL	*a = x->appl;
	UH		nm[16];
	INT		q;

	memset(e, 0, EXEC_SIZE);
	e[10] = 0x00; e[13] = 0x10;			/* frcol black */
	e[17] = 0x10;					/* chcol black */
	e[18] = 0xFF; e[19] = 0xFF; e[20] = 0xFF; e[21] = 0x10;	/* tbcol white */
	e[22] = 4;					/* pict */
	e[24] = 0x00; e[25] = 0x80;
	e[26] = (UB)a->appl; e[27] = (UB)( a->appl >> 8 );
	e[28] = 0x00; e[29] = 0x80;
	bpk_utf8_tron(a->name, nm, 16);
	for ( q = 0; q < 16; q++ ) {
		e[30 + 2 * q] = (UB)nm[q];
		e[31 + 2 * q] = (UB)( nm[q] >> 8 );
		e[62 + 2 * q] = (UB)nm[q];
		e[63 + 2 * q] = (UB)( nm[q] >> 8 );
	}
	e[94] = EXEC_DLEN;
	/* its data: the window, and for an editor the ground's colour */
	e[96] = 0x20;
	if ( x->haswin ) {
		INT	r[4] = { (INT)x->winx, (INT)x->winy, (INT)( x->winx + x->winw ), (INT)( x->winy + x->winh ) };

		for ( q = 0; q < 4; q++ ) {
			e[100 + 2 * q] = (UB)r[q];
			e[101 + 2 * q] = (UB)( r[q] >> 8 );
		}
	}
	if ( x->hasbg && a->bgoff != 0 && a->bgoff + 4 <= EXEC_DLEN ) {
		for ( q = 0; q < 4; q++ ) e[96 + a->bgoff + q] = (UB)( x->bg >> ( 8 * q ) );
	}
	(void)j;
}

/* ---------------------------------------------------------------- walking */

EXPORT ER bk_save_walk( BKJOB *j, const BKROOT *roots, INT nroot )
{
	static UB	meta[OB_ATR_MAX + 1];
	BK_TADW		*w;
	WALK		wk;
	BK_TADHOOK	hk = { walk_link, pic_image, NULL };
	INT		i, l, a;
	ER		er = E_OK;

	j->nobj = 0;
	j->total = j->src = 0;
	j->sroot = roots;
	j->nroot = 0;
	w = malloc(sizeof(BK_TADW));
	if ( w == NULL ) {
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	/* the roots first, each once; one left out is no root */
	for ( i = 0; i < nroot; i++ ) {
		if ( excluded(j, &roots[i].uuid) || find_obj(j, &roots[i].uuid) >= 0 ) continue;
		a = add_obj(j, &roots[i].uuid);
		if ( a < 0 ) {
			free(w);
			seterr(j, "作業の場所がありません", E_NOMEM);
			return E_NOMEM;
		}
		extra[a].rootof = i;
		j->nroot++;
	}
	if ( j->nroot == 0 ) {
		free(w);
		seterr(j, "保存する実身がありません", E_PAR);
		return E_PAR;
	}
	memset(&wk, 0, sizeof(wk));
	wk.j = j;
	hk.arg = &wk;
	for ( i = 0; i < j->nobj && er >= E_OK; i++ ) {
		BKOBJ	*o = &j->obj[i];
		EXTRA	*x = &extra[i];
		T_OBREF	ref;
		T_OBREC	r[64];
		SZ	asz = 0;
		ID	k;
		UB	*xml;
		UINT	xlen = 0;
		INT	cnt = 0, q, tlen = 0;
		BPKBUF	pb;

		if ( j->stop ) {
			er = E_ABORT;
			seterr(j, "中止しました", er);
			break;
		}
		k = ob_opn_obj(&o->uuid, OB_OP_R);
		if ( k <= 0 || ob_ref_obj(&o->uuid, &ref) < E_OK ) {
			er = ( k < 0 ) ? k : E_NOEXS;
			seterr(j, "実身を開けません", er);
			if ( k > 0 ) ob_cls_obj(k);
			break;
		}
		if ( ob_get_atr(k, meta, OB_ATR_MAX, &asz) < E_OK || !read_meta(j, i, k, meta, asz) ) {
			strncpy(o->name, (const char *)ref.name, sizeof(o->name) - 1);
		}
		if ( o->name[0] == 0 ) strncpy(o->name, (const char *)ref.name, sizeof(o->name) - 1);

		/* record 0: its links, the objects they name, and its size as binary TAD */
		x->execrec = -1;
		wk.at = i;
		o->nlink = 0;
		xml = bk_read_rec(k, 0, &xlen);
		if ( xml != NULL ) {
			wk.pics.key = k;
			er = bk_tad_from_xml(w, xml, (INT)xlen, NULL, 0, &tlen, &hk, NULL);
			free(xml);
			if ( er < E_OK ) {
				seterr(j, "記録を読めません", er);
				ob_cls_obj(k);
				break;
			}
		} else {
			/* a record 0 that says nothing: a text of its name */
			tlen = 44;
		}
		o->taglen = (UINT)tlen;
		if ( x->appl == NULL ) x->appl = &appls[2];		/* 基本文章編集 */
		for ( l = 0; l < o->nlink; l++ ) {
			T_OBREF	tr;

			if ( ob_ref_obj(&x->target[l], &tr) < E_OK || tr.type != OB_T_STORAGE ) continue;
			if ( excluded(j, &x->target[l]) ) continue;
			if ( find_obj(j, &x->target[l]) < 0 && add_obj(j, &x->target[l]) < 0 ) {
				er = E_LIMIT;
				seterr(j, "実身が多すぎます", er);
				break;
			}
			o = &j->obj[i];
			x = &extra[i];
		}

		/* the other records, and TessronOS's own */
		(void)ob_lst_rec(k, r, 64, &cnt);
		o->st.f_size = (INT)o->taglen;
		o->st.f_nrec = 2 + o->nlink;			/* TAD, 実行機能付箋 */
		for ( q = 0; q < cnt && q < 64; q++ ) {
			if ( r[q].recno == 0 ) continue;
			if ( r[q].rt == OB_RT_MFUSEN && x->execrec < 0 ) {
				x->execrec = r[q].recno;
				o->st.f_size += (INT)r[q].size;
				continue;
			}
			o->st.f_size += (INT)r[q].size;
			o->st.f_nrec++;
		}
		if ( x->execrec < 0 ) o->st.f_size += EXEC_SIZE;
		bpk_buf_init(&pb);
		er = make_private(j, i, k, &pb);
		x->privlen = (UINT)pb.n;
		bpk_buf_free(&pb);
		o->st.f_size += (INT)x->privlen;
		o->st.f_nrec++;
		o->st.f_nlink = (H)o->nlink;
		o->st.f_atype = x->appl->atype;
		o->st.f_nblk = 1 + ( o->st.f_size + BSIZE - 1 ) / BSIZE;
		ob_cls_obj(k);
		bk_estimate(&o->st, BSIZE, &j->total, &j->src);
		if ( j->tell != NULL ) {
			char	s[BK_NAME_MAX + 32];

			snprintf(s, sizeof(s), "実身を調べています：%s", o->name);
			j->tell(j, s, (UD)i + 1, (UD)j->nobj);
		}
	}
	free(wk.pics.px);
	free(w);
	if ( er < E_OK ) return er;

	/* each link: the objid of what it names when that is saved too */
	for ( i = 0; i < j->nobj; i++ ) {
		BKOBJ	*o = &j->obj[i];

		o->links = calloc((size_t)( o->nlink + 1 ), sizeof(UINT));
		if ( o->links == NULL ) {
			seterr(j, "作業の場所がありません", E_NOMEM);
			return E_NOMEM;
		}
		for ( l = 0; l < o->nlink; l++ ) {
			INT	t = find_obj(j, &extra[i].target[l]);

			o->links[l] = ( t >= 0 ) ? j->obj[t].objid : 0;
		}
	}
	return E_OK;
}

/* ---------------------------------------------------------------- writing */

/* The records of the object being written */
#define R_PRIV		0
#define R_EXEC		1
#define R_LINK		2
#define R_TAD		3
#define R_OTHER		4

typedef struct {
	BKJOB	*j;
	INT	i;			/* the object */
	ID	key;
	UB	*priv;			/* TessronOS's own record */
	UINT	privlen;
	UB	exec[EXEC_SIZE];
	UB	*tad;			/* record 0 as binary TAD */
	UINT	tadlen;
	T_OBREC	rec[64];		/* the other records */
	INT	nrec;
} SRC;

/* Which of the records rec is, and for the others which of them */
LOCAL INT rec_kind( SRC *s, INT rec, INT *p_k )
{
	BKOBJ	*o = &s->j->obj[s->i];

	if ( rec == 0 ) return R_PRIV;
	if ( rec == 1 ) return R_EXEC;
	if ( rec < 2 + o->nlink ) {
		*p_k = rec - 2;
		return R_LINK;
	}
	if ( rec == 2 + o->nlink ) return R_TAD;
	*p_k = rec - 3 - o->nlink;
	return R_OTHER;
}

/* The other records: those beside record 0 and the one 実行機能付箋 */
LOCAL T_OBREC *other( SRC *s, INT k )
{
	INT	q, n = 0;

	for ( q = 0; q < s->nrec; q++ ) {
		if ( s->rec[q].recno == 0 || s->rec[q].recno == extra[s->i].execrec ) continue;
		if ( n++ == k ) return &s->rec[q];
	}
	return NULL;
}

LOCAL ER src_info( void *arg, INT rec, UH *type, UH *subtype, INT *size )
{
	SRC	*s = arg;
	INT	k = 0;
	T_OBREC	*r;

	switch ( rec_kind(s, rec, &k) ) {
	case R_PRIV:
		*type = BK_RT_TESSRONOS;
		*subtype = BK_SUB_TESSRONOS;
		*size = (INT)s->privlen;
		return E_OK;
	case R_EXEC:
		*type = OB_RT_MFUSEN;
		*subtype = 0;
		if ( extra[s->i].execrec >= 0 ) {
			INT	q;

			for ( q = 0; q < s->nrec; q++ ) {
				if ( s->rec[q].recno == extra[s->i].execrec ) {
					*subtype = (UH)s->rec[q].sub;
					*size = (INT)s->rec[q].size;
				}
			}
			return E_OK;
		}
		*size = EXEC_SIZE;
		return E_OK;
	case R_LINK:
		*type = 0;
		*subtype = 0;
		*size = BK_LINK_SIZE;
		return E_OK;
	case R_TAD:
		*type = OB_RT_TAD;
		*subtype = 0;
		*size = (INT)s->tadlen;
		return E_OK;
	default:
		r = other(s, k);
		if ( r == NULL ) return E_PAR;
		/* an xmlTAD record is bytes to BTRON */
		*type = ( r->rt == OB_RT_TAD || r->rt == OB_RT_LINK ) ? OB_RT_SYSDATA : (UH)r->rt;
		*subtype = (UH)r->sub;
		*size = (INT)r->size;
		return E_OK;
	}
}

LOCAL INT src_read( void *arg, INT rec, INT off, UB *buf, INT len )
{
	SRC	*s = arg;
	INT	k = 0, n;
	SZ	asz = 0;
	T_OBREC	*r;

	switch ( rec_kind(s, rec, &k) ) {
	case R_PRIV:
		n = ( off + len <= (INT)s->privlen ) ? len : (INT)s->privlen - off;
		memcpy(buf, s->priv + off, (size_t)n);
		return n;
	case R_EXEC:
		if ( extra[s->i].execrec >= 0 ) {
			if ( ob_rea_rec(s->key, extra[s->i].execrec, off, buf, len, &asz) < E_OK ) return E_IO;
			return (INT)asz;
		}
		n = ( off + len <= EXEC_SIZE ) ? len : EXEC_SIZE - off;
		memcpy(buf, s->exec + off, (size_t)n);
		return n;
	case R_TAD:
		n = ( off + len <= (INT)s->tadlen ) ? len : (INT)s->tadlen - off;
		memcpy(buf, s->tad + off, (size_t)n);
		return n;
	case R_OTHER:
		r = other(s, k);
		if ( r == NULL || ob_rea_rec(s->key, r->recno, off, buf, len, &asz) < E_OK ) return E_IO;
		return (INT)asz;
	default:
		return E_PAR;
	}
}

LOCAL ER src_link( void *arg, INT rec, BK_LINKDESC *ld )
{
	SRC	*s = arg;
	BKOBJ	*o = &s->j->obj[s->i];
	INT	k = 0, t;
	UINT	id;

	(void)rec_kind(s, rec, &k);
	memset(ld, 0, sizeof(*ld));
	ld->atr[0] = o->latr[k];
	id = o->links[k];
	ld->objid = id;
	/* the F_LINK: the name and application type of what it names */
	t = (INT)( id >> 16 ) - 1;
	if ( id != 0 && t >= 0 && t < s->j->nobj ) {
		bpk_utf8_tron(s->j->obj[t].name, ld->f_name, 20);
		ld->f_atype = s->j->obj[t].st.f_atype;
	}
	return E_OK;
}

/* The object's records made ready: TessronOS's own, its TAD, its 実行機能付箋 */
LOCAL ER src_open( BKJOB *j, INT i, SRC *s )
{
	BK_TADW		*w;
	UB		*xml;
	UINT		xlen = 0;
	INT		n = 0;
	PICS		pics = { 0, NULL };
	BK_TADHOOK	hk = { NULL, pic_image, NULL };
	BPKBUF		pb;
	ER		er;

	memset(s, 0, sizeof(*s));
	s->j = j;
	s->i = i;
	s->key = ob_opn_obj(&j->obj[i].uuid, OB_OP_R);
	if ( s->key <= 0 ) return ( s->key < 0 ) ? s->key : E_NOEXS;
	(void)ob_lst_rec(s->key, s->rec, 64, &s->nrec);
	bpk_buf_init(&pb);
	er = make_private(j, i, s->key, &pb);
	if ( er < E_OK ) {
		bpk_buf_free(&pb);
		return er;
	}
	s->priv = (UB *)pb.s;			/* kept: freed with the source */
	s->privlen = (UINT)pb.n;
	make_exec(j, i, s->exec);

	xml = bk_read_rec(s->key, 0, &xlen);
	s->tad = malloc(j->obj[i].taglen + 64);
	if ( s->tad == NULL ) {
		free(xml);
		return E_NOMEM;
	}
	if ( xml == NULL ) {
		/* a text of its name */
		static const UB empty[] = { 0xE0, 0xFF, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x22, 0x01,
					    0xE1, 0xFF, 0x18, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					    0, 0, 0, 0, 0xB8, 0xFF, 0xB8, 0xFF, 0x21, 0x00, 0, 0,
					    0x0A, 0x00, 0xE2, 0xFF, 0x00, 0x00 };

		memcpy(s->tad, empty, sizeof(empty));
		s->tadlen = sizeof(empty);
		return E_OK;
	}
	w = malloc(sizeof(BK_TADW));
	pics.key = s->key;
	hk.arg = &pics;
	er = ( w != NULL ) ? bk_tad_from_xml(w, xml, (INT)xlen, s->tad, (INT)j->obj[i].taglen + 64, &n, &hk, NULL)
			   : E_NOMEM;
	free(w);
	free(xml);
	free(pics.px);
	s->tadlen = (UINT)n;
	return er;
}

LOCAL void src_close( SRC *s )
{
	if ( s->key > 0 ) ob_cls_obj(s->key);
	free(s->priv);
	free(s->tad);
	s->key = 0;
	s->priv = NULL;
	s->tad = NULL;
}

/*
 * The room a volume is written with. The writer weighs each piece
 * against what has gone out, while the compressor may still hold up to
 * its ring of bytes taken earlier; that much is kept back so a volume
 * never grows past the room it was given.
 */
#define HELD_BACK	( BK_ENC_RING + 0x400 )

LOCAL UD room_of( UD cap )
{
	return ( cap > 2 * HELD_BACK ) ? cap - HELD_BACK : cap / 2;
}

EXPORT ER bk_save_write( BKJOB *j, const BKDEST *d )
{
	BK_WR	*wr = malloc(sizeof(BK_WR));
	UH	memo[BK_MEMO_TC];
	char	name[BK_NAME_MAX];
	BK_OUT	out = NULL;
	void	*oarg = NULL;
	UD	cap = 0;
	INT	i, vol = 1;
	ER	er;

	if ( wr == NULL ) {
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}
	bpk_utf8_tron(j->base, memo, BK_MEMO_TC);
	bk_wr_init(wr, j->total, (UINT)j->nobj, j->src, memo);
	bk_vol_name(j->base, vol, name, sizeof(name));
	er = d->open(d->ctx, vol, name, &out, &oarg, &cap);
	if ( er >= E_OK ) er = bk_wr_begin(wr, out, oarg, room_of(cap));
	if ( er < E_OK ) {
		if ( j->err[0] == 0 ) seterr(j, "保存先に書けません", er);
		free(wr);
		return er;
	}
	for ( i = 0; i < j->nobj && er >= E_OK; i++ ) {
		SRC		s;
		BK_OBJSRC	os;
		UB		meta[BK_META_SIZE];
		UH		nm[BK_NAME_TC];
		BKOBJ		*o = &j->obj[i];

		if ( j->stop ) {
			er = E_ABORT;
			seterr(j, "中止しました", er);
			break;
		}
		er = src_open(j, i, &s);
		if ( er < E_OK ) {
			src_close(&s);
			seterr(j, "実身を読めません", er);
			break;
		}
		bpk_utf8_tron(o->name, nm, BK_NAME_TC);
		bk_meta_put(meta, nm, &o->st);
		os.nrec = o->st.f_nrec;
		os.info = src_info;
		os.read = src_read;
		os.link = src_link;
		os.arg = &s;
		while ( ( er = bk_wr_object(wr, o->objid, meta, &os) ) == BK_FULL ) {
			/* the volume is full: the next one goes on with this object */
			er = bk_wr_end(wr, TRUE);
			if ( er >= E_OK ) er = d->close(d->ctx, vol, TRUE);
			if ( er < E_OK ) break;
			vol++;
			bk_vol_name(j->base, vol, name, sizeof(name));
			if ( j->tell != NULL ) {
				char	line[BK_NAME_MAX + 64];

				snprintf(line, sizeof(line), "次の巻（%s）に書きます", name);
				j->tell(j, line, (UD)i, (UD)j->nobj);
			}
			er = d->open(d->ctx, vol, name, &out, &oarg, &cap);
			if ( er >= E_OK ) er = bk_wr_begin(wr, out, oarg, room_of(cap));
			if ( er < E_OK || j->stop ) {
				if ( er >= E_OK ) er = E_ABORT;
				break;
			}
		}
		src_close(&s);
		if ( er < E_OK ) {
			if ( j->err[0] == 0 ) seterr(j, ( er == E_LIMIT ) ? "巻に収まりません" : "保存先に書けません", er);
			break;
		}
		j->done = (UD)i + 1;
		if ( j->tell != NULL ) {
			char	line[BK_NAME_MAX + 32];

			snprintf(line, sizeof(line), "保存しています：%s", o->name);
			j->tell(j, line, (UD)i + 1, (UD)j->nobj);
		}
	}
	if ( er >= E_OK ) {
		er = bk_wr_end(wr, FALSE);
		if ( er >= E_OK ) er = d->close(d->ctx, vol, FALSE);
		if ( er < E_OK ) seterr(j, "保存先に書けません", er);
	} else {
		(void)d->close(d->ctx, vol, FALSE);
	}
	j->nvol = vol;
	free(wr);
	return er;
}

EXPORT void bk_save_free( BKJOB *j )
{
	INT	i;

	for ( i = 0; i < j->nobj; i++ ) {
		free(j->obj[i].links);
		free(j->obj[i].latr);
		if ( extra != NULL ) free(extra[i].target);
	}
	free(j->obj);
	free(extra);
	extra = NULL;
	j->obj = NULL;
	j->nobj = j->maxobj = 0;
}
