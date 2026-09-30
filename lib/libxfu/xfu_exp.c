/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_exp.c
 *	Real objects given out as a tree of files (design 18.20)
 *
 *	What an object becomes is decided in this order:
 *
 *	  - taken in from a file, and record 1 still the file's bytes (its
 *	    size and checksum as "tessronos.file" says): those bytes, under the
 *	    file's name -- unless it was a text and record 0 has been changed
 *	    since, when the text of record 0 is written in the encoding and
 *	    with the line ends the file had;
 *	  - taken in from an xmlTAD file and not changed: that file;
 *	  - a figure with links: a directory, and in it each object it links
 *	    to, given out the same way, as deep as the depth allows; an
 *	    object already being given out further up is not gone into again;
 *	  - a document: its text, in UTF-8 or the encoding asked for;
 *	  - anything else: its record 0 as <name>.xtad.
 *
 *	Or, as TADjs file sets, the object and everything it links to:
 *	{uuid}.json, {uuid}_N.xtad or _N.bin for each record, {uuid}.ico,
 *	and {uuid}_N_... for each resource: the layout tools/tsfs_py reads
 *	and writes.
 */

#include "xfu_in.h"

#define M_RAW		1		/* a record's bytes as they are */
#define M_TEXT		2		/* the text of record 0 */
#define M_DIR		3
#define M_XTAD		4		/* record 0 as xmlTAD */

/* What an object's "tessronos.file" says */
typedef struct {
	BOOL	have;
	UB	name[XFU_NAME_MAX];
	UB	type[64];
	D	size;
	D	crc;			/* -1 when not said */
	D	rec0;			/* -1 when not said */
	INT	enc;
	BOOL	crlf;
	D	mtime;
} FFACT;

LOCAL void read_fact( CONST UB *meta, SZ len, FFACT *f, D *p_updated )
{
	T_JSON	root, tf, fl;
	UB	t[40];

	xfu_mset(f, 0, sizeof(*f));
	f->crc = -1;
	f->rec0 = -1;
	f->size = -1;
	*p_updated = 0;
	if ( meta == NULL || js_parse(meta, (INT)len, &root) < E_OK ) {
		return;
	}
	if ( js_get_str(&root, "updateDate", t, sizeof(t)) > 0 ) {
		*p_updated = xfu_time_parse(t);
	}
	if ( js_get(&root, "tessronos", &tf) < E_OK || js_get(&tf, "file", &fl) < E_OK ) {
		return;
	}
	f->have = TRUE;
	(void)js_get_str(&fl, "name", f->name, sizeof(f->name));
	(void)js_get_str(&fl, "mediatype", f->type, sizeof(f->type));
	f->size = js_get_num(&fl, "size", -1);
	f->crc = js_get_num(&fl, "crc32", -1);
	f->rec0 = js_get_num(&fl, "rec0", -1);
	if ( js_get_str(&fl, "encoding", t, sizeof(t)) > 0 ) f->enc = txc_by_name(t);
	f->crlf = (BOOL)( js_get_str(&fl, "eol", t, sizeof(t)) > 0 && xfu_same(t, "crlf") );
	if ( js_get_str(&fl, "mtime", t, sizeof(t)) > 0 ) f->mtime = xfu_time_parse(t);
}

/* A record's CRC and size, read through */
LOCAL ER rec_crc( XFU *x, ID key, INT recno, UINT *p_crc, D *p_size )
{
	SZ	asz;
	D	at = 0;
	UINT	crc = 0;
	ER	er;

	for (;;) {
		asz = 0;
		er = ob_rea_rec(key, recno, at, x->chunk, XFU_CHUNK, &asz);
		if ( er < E_OK || asz <= 0 ) break;
		crc = xfu_crc32(crc, x->chunk, asz);
		at += asz;
		if ( asz < XFU_CHUNK ) break;
	}
	if ( er == E_NOEXS && at > 0 ) er = E_OK;
	*p_crc = crc;
	*p_size = at;
	return ( er < E_OK && at == 0 && er != E_NOEXS ) ? er : E_OK;
}

/* Whether a name is one of a TADjs record: {uuid}_N.xtad */
LOCAL BOOL tadjs_name( CONST UB *s )
{
	TS_UUID	u;

	return (BOOL)( xfu_slen(s) > 37 && xfu_str_uuid(s, &u) && s[36] == '_' );
}

/* ---------------------------------------------------------------- text */

/* An entity of XML at s: the character, and in *p_n the bytes it took; -1 for none */
LOCAL INT entity( CONST UB *s, SZ n, SZ *p_n )
{
	SZ	i;
	INT	c = 0;

	if ( n >= 5 && s[1] == 'a' && s[2] == 'm' && s[3] == 'p' && s[4] == ';' ) { *p_n = 5; return '&'; }
	if ( n >= 4 && s[1] == 'l' && s[2] == 't' && s[3] == ';' ) { *p_n = 4; return '<'; }
	if ( n >= 4 && s[1] == 'g' && s[2] == 't' && s[3] == ';' ) { *p_n = 4; return '>'; }
	if ( n >= 6 && s[1] == 'q' && s[2] == 'u' && s[3] == 'o' && s[4] == 't' && s[5] == ';' ) { *p_n = 6; return '"'; }
	if ( n >= 6 && s[1] == 'a' && s[2] == 'p' && s[3] == 'o' && s[4] == 's' && s[5] == ';' ) { *p_n = 6; return '\''; }
	if ( n >= 4 && s[1] == '#' ) {
		BOOL	hex = (BOOL)( s[2] == 'x' || s[2] == 'X' );

		for ( i = hex ? 3 : 2; i < n && i < 12 && s[i] != ';'; i++ ) {
			UB	d = s[i];

			if ( d >= '0' && d <= '9' ) c = c * ( hex ? 16 : 10 ) + ( d - '0' );
			else if ( hex && ( d | 0x20 ) >= 'a' && ( d | 0x20 ) <= 'f' ) c = c * 16 + ( ( d | 0x20 ) - 'a' + 10 );
			else return -1;
		}
		if ( i < n && s[i] == ';' && c > 0 && c <= 0x10FFFF ) {
			*p_n = i + 1;
			return c;
		}
	}
	return -1;
}

/*
 * The text of a document: the characters between the tags, a line end
 * where a paragraph ends and at each <br/>, a tab at <tab/>. Line ends
 * in the xmlTAD itself are how it was laid out, not text, and go.
 */
LOCAL void doc_text( CONST UB *s, SZ n, XBUF *w, BOOL crlf )
{
	SZ	i, k;
	INT	c;
	UB	u[4];
	INT	b = xfu_find(s, n, "<document", 0), e;

	i = ( b >= 0 ) ? (SZ)b : 0;
	e = xfu_find(s, n, "</document>", i);
	if ( e >= 0 ) n = (SZ)e;
	if ( b >= 0 ) {
		while ( i < n && s[i] != '>' ) i++;
		i++;
	}
	while ( i < n ) {
		if ( s[i] == '<' ) {
			SZ	t = i + 1;
			BOOL	close = FALSE;

			if ( t < n && s[t] == '/' ) {
				close = TRUE;
				t++;
			}
			for ( k = i; k < n && s[k] != '>'; k++ ) ;
			if ( close && t + 1 < n && s[t] == 'p' && ( s[t + 1] == '>' || s[t + 1] == ' ' ) ) {
				xb_put(w, crlf ? "\r\n" : "\n");
			} else if ( !close && t + 2 < n && s[t] == 'b' && s[t + 1] == 'r'
				    && ( s[t + 2] == '/' || s[t + 2] == '>' || s[t + 2] == ' ' ) ) {
				xb_put(w, crlf ? "\r\n" : "\n");
			} else if ( !close && t + 3 < n && s[t] == 't' && s[t + 1] == 'a' && s[t + 2] == 'b'
				    && ( s[t + 3] == '/' || s[t + 3] == '>' || s[t + 3] == ' ' ) ) {
				xb_put(w, "\t");
			}
			i = k + 1;
		} else if ( s[i] == '&' && ( c = entity(s + i, n - i, &k) ) >= 0 ) {
			xb_bytes(w, u, txc_utf8_put(c, u));
			i += k;
		} else if ( s[i] == '\r' || s[i] == '\n' ) {
			i++;
		} else {
			xb_bytes(w, s + i, 1);
			i++;
		}
	}
}

/* ---------------------------------------------------------------- writing a file */

/* A record's bytes into a file of the tree */
LOCAL ER put_rec_file( XFU *x, ID key, INT recno, CONST UB *path )
{
	CONST T_XFUTREE	*t = x->t;
	SZ		asz;
	D		at = 0;
	INT		h;
	ER		er = E_OK, e2;

	h = t->open(t->ctx, path, XFU_O_WRITE);
	if ( h < 0 ) {
		return (ER)h;
	}
	for (;;) {
		asz = 0;
		er = ob_rea_rec(key, recno, at, x->chunk, XFU_CHUNK, &asz);
		if ( er < E_OK || asz <= 0 ) break;
		if ( t->write(t->ctx, h, x->chunk, asz) != (INT)asz ) {
			er = E_IO;
			break;
		}
		at += asz;
		xfu_tick(x, at);
		if ( x->stop ) {
			er = E_ABORT;
			break;
		}
		if ( asz < XFU_CHUNK ) break;
	}
	if ( er == E_NOEXS ) er = E_OK;		/* read to the end */
	e2 = t->close(t->ctx, h);
	return ( er >= E_OK ) ? e2 : er;
}

/* Bytes into a file of the tree */
LOCAL ER put_file( XFU *x, CONST UB *path, CONST UB *b, SZ n )
{
	CONST T_XFUTREE	*t = x->t;
	INT		h, k;
	SZ		done = 0;
	ER		er = E_OK, e2;

	h = t->open(t->ctx, path, XFU_O_WRITE);
	if ( h < 0 ) {
		return (ER)h;
	}
	while ( done < n ) {
		SZ	part = ( n - done > XFU_CHUNK ) ? XFU_CHUNK : n - done;

		k = t->write(t->ctx, h, b + done, part);
		if ( k <= 0 ) {
			er = ( k < 0 ) ? (ER)k : E_IO;
			break;
		}
		done += k;
	}
	e2 = t->close(t->ctx, h);
	return ( er >= E_OK ) ? e2 : er;
}

/* The names a directory of the tree has */
LOCAL INT names_put( void *arg, CONST T_XFUENT *e )
{
	return ( xfu_names_add((T_XFUNAMES *)arg, e->name) >= E_OK ) ? 0 : -1;
}

/* A name made one the tree takes, and new in its directory */
LOCAL ER fresh_name( XFU *x, T_XFUNAMES *names, CONST UB *want, UB *out )
{
	INT	lim = ( x->t->namemax > 0 && x->t->namemax + 1 < XFU_NAME_MAX )
		      ? x->t->namemax + 1 : XFU_NAME_MAX;
	INT	n = xfu_name_out(want, out, lim);

	if ( n < 0 ) {
		return (ER)n;
	}
	return xfu_names_unique(names, out, XFU_NAME_MAX, x->t->namemax);
}

/* ---------------------------------------------------------------- an object */

LOCAL BOOL on_stack( XFU *x, CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < x->nstack; i++ ) {
		if ( xfu_uuid_eq(&x->stack[i], u) ) return TRUE;
	}
	return FALSE;
}

typedef struct {
	TS_UUID	*ids;
	INT	n, max;
} TLIST;

/* Each target once, in the order of the links */
LOCAL void tl_put( void *arg, CONST TS_UUID *u )
{
	TLIST	*l = (TLIST *)arg;
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( xfu_uuid_eq(&l->ids[i], u) ) return;
	}
	if ( l->n < l->max ) l->ids[l->n++] = *u;
}

LOCAL ER exp_obj( XFU *x, CONST TS_UUID *u, CONST UB *dir, T_XFUNAMES *names, INT level,
		  UB *outname, INT max );

/* A figure's links, each given out into the directory made for it */
LOCAL ER exp_links( XFU *x, CONST UB *rec0, SZ n0, CONST UB *path, INT level )
{
	T_XFUNAMES	sub;
	TLIST		l;
	INT		i;
	ER		er = E_OK, e;

	l.max = 1024;
	l.n = 0;
	l.ids = (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * l.max);
	if ( l.ids == NULL ) {
		return E_NOMEM;
	}
	(void)xfu_links(rec0, n0, tl_put, &l);
	xfu_names_init(&sub);
	(void)x->t->list(x->t->ctx, path, names_put, &sub);
	for ( i = 0; i < l.n && er != E_NOMEM; i++ ) {
		if ( x->stop ) {
			er = E_ABORT;
			break;
		}
		if ( on_stack(x, &l.ids[i]) ) {
			xfu_note(x, path, E_OBJ);	/* a cycle: not gone into again */
			continue;
		}
		if ( level + 1 > x->depth ) {
			xfu_note(x, path, E_LIMIT);
			continue;
		}
		e = exp_obj(x, &l.ids[i], path, &sub, level + 1, NULL, 0);
		if ( e == E_NOMEM || e == E_ABORT ) er = e;
	}
	xfu_names_free(&sub);
	xfu_sys_free(l.ids);
	return er;
}

LOCAL ER exp_obj( XFU *x, CONST TS_UUID *u, CONST UB *dir, T_XFUNAMES *names, INT level,
		  UB *outname, INT max )
{
	CONST T_XFUOPT	*opt = x->opt;
	UINT		flags = ( opt != NULL ) ? opt->flags : 0;
	T_OBREF		ref;
	T_OBREC		rec[16];
	FFACT		*f = NULL;
	UB		*meta = NULL, *r0 = NULL, *name = NULL, *path = NULL;
	SZ		mlen = 0, n0 = 0;
	INT		mode = 0, nrec = 0, raw = 1, i, enc;
	D		updated = 0, when = 0;
	UINT		crc;
	D		sz;
	XBUF		w;
	ID		key;
	ER		er;

	if ( x->nseen >= XFU_OBJ_MAX ) {
		return E_LIMIT;
	}
	x->seen[x->nseen++] = *u;
	er = ob_ref_obj(u, &ref);
	if ( er < E_OK ) {
		return er;
	}
	key = ob_opn_obj(u, OB_OP_R);
	if ( key < E_OK ) {
		return (ER)key;
	}
	f = (FFACT *)xfu_sys_alloc(sizeof(FFACT));
	name = (UB *)xfu_sys_alloc(XFU_NAME_MAX * 2);
	path = (UB *)xfu_sys_alloc(XFU_PATH_MAX);
	er = ( f != NULL && name != NULL && path != NULL ) ? E_OK : E_NOMEM;
	if ( er >= E_OK ) {
		meta = xfu_meta_all(key, &mlen);
		read_fact(meta, mlen, f, &updated);
		if ( ob_lst_rec(key, rec, 16, &nrec) < E_OK ) nrec = 0;
		r0 = ( nrec > 0 ) ? xfu_rec_all(key, 0, &n0) : NULL;
	}

	/* what it becomes */
	if ( er >= E_OK && f->have && !xfu_same(f->type, XFU_MT_TADJS) && !xfu_same(f->type, XFU_MT_DIR) ) {
		if ( nrec >= 2 && rec_crc(x, key, 1, &crc, &sz) >= E_OK && sz == f->size
		  && ( f->crc < 0 || (UINT)f->crc == crc ) ) {
			if ( f->rec0 >= 0 && r0 != NULL && (UINT)f->rec0 != xfu_crc32(0, r0, n0) ) {
				mode = M_TEXT;		/* the text was changed here */
			} else {
				mode = M_RAW;
			}
		} else if ( nrec == 1 && r0 != NULL && xfu_same(f->type, XFU_MT_TAD) && !tadjs_name(f->name)
			    && (D)n0 == f->size && ( f->crc < 0 || (UINT)f->crc == xfu_crc32(0, r0, n0) ) ) {
			mode = M_RAW;
			raw = 0;
		}
		if ( mode == M_RAW && ( flags & XFU_F_TEXT ) != 0 && r0 != NULL
		  && xfu_find(r0, n0, "<document", 0) >= 0 ) {
			mode = M_TEXT;			/* the text asked for in the encoding asked for */
		}
		if ( mode != 0 ) {
			(void)xfu_scpy(name, XFU_NAME_MAX, f->name);
			when = f->mtime;
		}
	}
	if ( er >= E_OK && mode == 0 ) {
		INT	fig = ( r0 != NULL ) ? xfu_find(r0, n0, "<figure", 0) : -1;
		INT	doc = ( r0 != NULL ) ? xfu_find(r0, n0, "<document", 0) : -1;

		if ( r0 == NULL && nrec >= 2 ) {
			mode = M_RAW;
		} else if ( fig >= 0 && ( doc < 0 || fig < doc ) && ( flags & XFU_F_ONE ) == 0
			    && xfu_links(r0, n0, NULL, NULL) > 0 ) {
			mode = M_DIR;
		} else if ( doc >= 0 && ( fig < 0 || doc < fig ) && ( flags & XFU_F_TAD ) == 0 ) {
			mode = M_TEXT;
		} else {
			mode = M_XTAD;
		}
		when = updated;
		if ( mode == M_DIR && f->have && xfu_same(f->type, XFU_MT_DIR) && f->name[0] != 0 ) {
			(void)xfu_scpy(name, XFU_NAME_MAX, f->name);
		} else if ( mode == M_TEXT && f->have && f->rec0 >= 0 ) {
			(void)xfu_scpy(name, XFU_NAME_MAX, f->name);
		} else {
			i = xfu_scpy(name, XFU_NAME_MAX - 8, ref.name);
			if ( i == 0 ) i = xfu_scpy(name, XFU_NAME_MAX - 8, (CONST UB *)"_");
			(void)xfu_scpy(name + i, 8, ( mode == M_TEXT ) ? (CONST UB *)".txt"
					    : ( mode == M_XTAD ) ? (CONST UB *)".xtad"
					    : ( mode == M_RAW ) ? (CONST UB *)".bin" : (CONST UB *)"");
		}
	}

	/* the name the caller gave the object asked for, in place of its own */
	if ( er >= E_OK && level == 0 && opt != NULL && opt->name != NULL && opt->name[0] != 0 ) {
		(void)xfu_scpy(name, XFU_NAME_MAX, opt->name);
	}

	/* its name in the directory, and it written */
	if ( er >= E_OK ) {
		UB	*fresh = name + XFU_NAME_MAX;

		er = fresh_name(x, names, name, fresh);
		if ( er >= E_OK ) er = xfu_join(path, XFU_PATH_MAX, dir, fresh);
		if ( er >= E_OK && outname != NULL ) (void)xfu_scpy(outname, max, fresh);
	}
	if ( er >= E_OK ) {
		x->stack[x->nstack++] = *u;
		switch ( mode ) {
		case M_RAW:
			er = put_rec_file(x, key, raw, path);
			x->st.files++;
			break;

		case M_TEXT:
			xb_init(&w);
			doc_text(r0, n0, &w, f->have && f->crlf);
			enc = ( opt != NULL && opt->outenc != TXC_AUTO ) ? opt->outenc
			    : ( f->have && f->enc != TXC_AUTO ) ? f->enc : TXC_UTF8;
			if ( w.bad ) {
				er = E_NOMEM;
			} else if ( enc == TXC_UTF8 ) {
				er = put_file(x, path, w.b, w.at);
			} else {
				SZ	need = txc_from_utf8(enc, w.b, w.at, NULL, 0, NULL);
				UB	*o = (UB *)xfu_sys_alloc(need + 1);
				INT	bad = 0;

				er = ( o != NULL ) ? E_OK : E_NOMEM;
				if ( er >= E_OK ) {
					(void)txc_from_utf8(enc, w.b, w.at, o, need + 1, &bad);
					x->st.geta += bad;
					er = put_file(x, path, o, need);
					xfu_sys_free(o);
				}
			}
			xb_free(&w);
			x->st.files++;
			break;

		case M_DIR:
			er = ( x->t->mkdir != NULL ) ? x->t->mkdir(x->t->ctx, path) : E_NOSPT;
			if ( er >= E_OK ) {
				x->st.dirs++;
				er = exp_links(x, r0, n0, path, level);
			}
			if ( er >= E_OK && ( flags & XFU_F_LAYOUT ) != 0 ) {
				T_XFUNAMES	own;
				UB		*lay = name;
				INT		k = xfu_scpy(lay, XFU_NAME_MAX - 8, name + XFU_NAME_MAX);

				(void)xfu_scpy(lay + k, 8, (CONST UB *)".xtad");
				xfu_names_init(&own);
				(void)x->t->list(x->t->ctx, path, names_put, &own);
				if ( xfu_names_unique(&own, lay, XFU_NAME_MAX, x->t->namemax) >= E_OK
				  && xfu_join(name + XFU_NAME_MAX, XFU_NAME_MAX, path, lay) >= E_OK ) {
					er = put_file(x, name + XFU_NAME_MAX, r0, n0);
				}
				xfu_names_free(&own);
			}
			break;

		default:
			er = put_file(x, path, ( r0 != NULL ) ? r0 : (CONST UB *)"", n0);
			x->st.files++;
			break;
		}
		x->nstack--;
		if ( er >= E_OK ) {
			x->st.objects++;
			if ( when > 0 && x->t->utime != NULL && mode != M_DIR ) {
				(void)x->t->utime(x->t->ctx, path, when);
			}
		}
	}
	xfu_note(x, ( name != NULL ) ? name : (CONST UB *)"", er);

	ob_cls_obj(key);
	if ( meta != NULL ) xfu_sys_free(meta);
	if ( r0 != NULL ) xfu_sys_free(r0);
	if ( f != NULL ) xfu_sys_free(f);
	if ( name != NULL ) xfu_sys_free(name);
	if ( path != NULL ) xfu_sys_free(path);
	return er;
}

/* ---------------------------------------------------------------- TADjs file sets */

/* The file of an object's set: <dir>/<uuid><tail> */
LOCAL ER set_path( UB *out, CONST UB *dir, CONST TS_UUID *u, CONST UB *tail )
{
	UB	nm[40 + OB_RES_NAME + 16];
	INT	n;

	xfu_uuid_str(u, (char *)nm);
	n = xfu_slen(nm);
	if ( xfu_scpy(nm + n, sizeof(nm) - n, tail) != xfu_slen(tail) ) {
		return E_LIMIT;
	}
	return xfu_join(out, XFU_PATH_MAX, dir, nm);
}

/* The number n as a record's part of a name: "_n.xtad" */
LOCAL void rec_tail( UB *out, INT n, BOOL tad )
{
	UB	d[12];
	INT	k = 0, at = 0;

	do {
		d[k++] = (UB)( '0' + n % 10 );
		n /= 10;
	} while ( n > 0 );
	out[at++] = '_';
	while ( k > 0 ) out[at++] = d[--k];
	(void)xfu_scpy(out + at, 8, tad ? (CONST UB *)".xtad" : (CONST UB *)".bin");
}

/* Objects linked to that have not been met, put in the list to go */
LOCAL void seen_put( void *arg, CONST TS_UUID *u )
{
	XFU	*x = (XFU *)arg;
	T_OBREF	r;
	INT	i;

	for ( i = 0; i < x->nseen; i++ ) {
		if ( xfu_uuid_eq(&x->seen[i], u) ) return;
	}
	if ( x->nseen < XFU_OBJ_MAX && ob_ref_obj(u, &r) >= E_OK ) {
		x->seen[x->nseen++] = *u;
	}
}

LOCAL ER exp_set( XFU *x, CONST TS_UUID *u, CONST UB *dir )
{
	CONST T_XFUTREE	*t = x->t;
	T_OBREC		rec[16];
	UB		*path, *names = NULL, *b;
	UB		tail[24];
	SZ		len = 0, asz;
	D		at;
	INT		nrec = 0, i, cnt = 0, h, k;
	ID		key;
	ER		er, e2;

	key = ob_opn_obj(u, OB_OP_R);
	if ( key < E_OK ) {
		return (ER)key;
	}
	path = (UB *)xfu_sys_alloc(XFU_PATH_MAX);
	er = ( path != NULL ) ? E_OK : E_NOMEM;

	/* the metadata */
	if ( er >= E_OK ) {
		b = xfu_meta_all(key, &len);
		er = ( b != NULL ) ? set_path(path, dir, u, (CONST UB *)".json") : E_NOEXS;
		if ( er >= E_OK ) er = put_file(x, path, b, len);
		if ( b != NULL ) xfu_sys_free(b);
	}

	/* each record; the links of an xmlTAD one followed */
	if ( er >= E_OK && ob_lst_rec(key, rec, 16, &nrec) < E_OK ) nrec = 0;
	for ( i = 0; er >= E_OK && i < nrec && i < 16; i++ ) {
		BOOL	tad = (BOOL)( rec[i].rt == OB_RT_TAD );

		rec_tail(tail, i, tad);
		er = set_path(path, dir, u, tail);
		if ( er < E_OK ) break;
		if ( tad ) {
			b = xfu_rec_all(key, rec[i].recno, &len);
			er = ( b != NULL ) ? put_file(x, path, b, len) : E_NOMEM;
			if ( b != NULL ) {
				(void)xfu_links(b, len, seen_put, x);
				xfu_sys_free(b);
			}
		} else {
			er = put_rec_file(x, key, rec[i].recno, path);
		}
		x->st.files++;
	}

	/* the icon */
	if ( er >= E_OK && ob_get_ico(key, NULL, 0, &len) >= E_OK && len > 0 ) {
		b = (UB *)xfu_sys_alloc(len);
		er = ( b != NULL ) ? ob_get_ico(key, b, len, &len) : E_NOMEM;
		if ( er >= E_OK ) er = set_path(path, dir, u, (CONST UB *)".ico");
		if ( er >= E_OK ) er = put_file(x, path, b, len);
		if ( b != NULL ) xfu_sys_free(b);
	}

	/* the resources */
	if ( er >= E_OK ) {
		names = (UB *)xfu_sys_alloc(4096);
		er = ( names != NULL ) ? E_OK : E_NOMEM;
		if ( er >= E_OK && ob_lst_res(key, names, 4096, &cnt) < E_OK ) cnt = 0;
	}
	for ( i = 0, k = 0; er >= E_OK && i < cnt; i++ ) {
		CONST UB	*nm = names + k;

		k += xfu_slen(nm) + 1;
		if ( xfu_same(nm, ".ico") ) continue;	/* the icon, written above */
		er = set_path(path, dir, u, nm);
		if ( er < E_OK ) break;
		h = t->open(t->ctx, path, XFU_O_WRITE);
		if ( h < 0 ) {
			er = (ER)h;
			break;
		}
		for ( at = 0;; at += asz ) {
			asz = 0;
			if ( ob_rea_res(key, nm, at, x->chunk, XFU_CHUNK, &asz) < E_OK || asz <= 0 ) break;
			if ( t->write(t->ctx, h, x->chunk, asz) != (INT)asz ) {
				er = E_IO;
				break;
			}
		}
		e2 = t->close(t->ctx, h);
		if ( er >= E_OK ) er = e2;
		x->st.files++;
	}
	if ( er >= E_OK ) x->st.objects++;
	ob_cls_obj(key);
	if ( names != NULL ) xfu_sys_free(names);
	if ( path != NULL ) xfu_sys_free(path);
	return er;
}

/* ---------------------------------------------------------------- the call */

EXPORT ER xfu_export( CONST TS_UUID *uuid, CONST T_XFUTREE *t, CONST UB *dir,
		      CONST T_XFUOPT *opt, UB *name, INT max )
{
	XFU		*x;
	T_XFUNAMES	names;
	INT		i;
	ER		er, e;

	if ( uuid == NULL || t == NULL || t->open == NULL || t->write == NULL
	  || t->close == NULL || t->list == NULL ) {
		return E_PAR;
	}
	x = (XFU *)xfu_sys_alloc(sizeof(XFU));
	if ( x == NULL ) {
		return E_NOMEM;
	}
	er = xfu_begin(x, t, opt);
	if ( er >= E_OK ) {
		x->seen = (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * XFU_OBJ_MAX);
		x->stack = (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * ( XFU_DEPTH * 4 + 4 ));
		if ( x->seen == NULL || x->stack == NULL ) er = E_NOMEM;
		if ( x->depth > XFU_DEPTH * 4 ) x->depth = XFU_DEPTH * 4;
	}
	if ( dir == NULL ) {
		dir = (CONST UB *)"";
	}
	if ( er >= E_OK && opt != NULL && ( opt->flags & XFU_F_TADJS ) != 0 ) {
		/* the object, and every object met in the records of those before */
		x->seen[x->nseen++] = *uuid;
		for ( i = 0; i < x->nseen; i++ ) {
			if ( x->stop ) {
				er = E_ABORT;
				break;
			}
			e = exp_set(x, &x->seen[i], dir);
			xfu_note(x, dir, e);
			if ( i == 0 && e < E_OK ) er = e;
			if ( e == E_NOMEM ) {
				er = e;
				break;
			}
		}
		if ( er >= E_OK && name != NULL ) {
			char	us[40];

			xfu_uuid_str(uuid, us);
			(void)xfu_scpy(name, max, (CONST UB *)us);
		}
	} else if ( er >= E_OK ) {
		xfu_names_init(&names);
		(void)t->list(t->ctx, dir, names_put, &names);
		er = exp_obj(x, uuid, dir, &names, 0, name, max);
		xfu_names_free(&names);
	}
	if ( er >= E_OK && x->stop ) {
		er = E_ABORT;
	}
	xfu_finish(x);
	xfu_sys_free(x);
	return er;
}
