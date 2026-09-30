/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xf.c
 *	Files from outside taken in as real objects, and given out again
 *	(design 18.20)
 *
 *	A file is taken in as it is read: the first bytes say what kind of
 *	file it is (with the name's extension), the object is made, and the
 *	rest goes straight into record 1. What the file was -- its name,
 *	type, size, time and where it came from -- is written last, when the
 *	size is known, into the object's management information (its
 *	metadata, "tessronos.file"); record 0 is xmlTAD that shows the file,
 *	its lines for text and its name otherwise. An xmlTAD file is its own
 *	record 0, unchanged.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/fs.h>
#include <ts/dt.h>
#include <ts/xf.h>
#include <ts/json.h>
#include <tm/tmonitor.h>

#define XF_CHUNK	( 64 * 1024 )
#define XF_TAD_MAX	( 8 * 1024 * 1024 )	/* bytes of an xmlTAD file taken whole */
#define XF_TEXT_MAX	( 256 * 1024 )		/* bytes of text shown line by line */
#define XF_REC0_MAX	( XF_TEXT_MAX * 2 + 4096 )
#define XF_JSON_MAX	OB_ATR_MAX

/*
 * What taking in does to the new object. Not its deletion: an object
 * the system owns may forbid that.
 */
#define XF_OPS		( OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR )

/* ---------------------------------------------------------------- kinds */

#define K_TAD		1
#define K_ELF		2
#define K_FONT		3
#define K_DICT		4
#define K_IMAGE		5
#define K_TEXT		6
#define K_BINARY	7
#define K_ARCHIVE	8		/* a BTRON archive (書庫), opened by 書庫解凍 */

typedef struct {
	CONST char	*ext;		/* lower case, with the dot */
	UINT		kind;
	CONST char	*mediatype;
} XFEXT;

LOCAL CONST XFEXT xf_ext[] = {
	{ ".xtad", K_TAD,	"application/x-xmltad" },
	{ ".elf",  K_ELF,	"application/x-elf" },
	{ ".ttf",  K_FONT,	"font/ttf" },
	{ ".otf",  K_FONT,	"font/otf" },
	{ ".ttc",  K_FONT,	"font/collection" },
	{ ".png",  K_IMAGE,	"image/png" },
	{ ".jpg",  K_IMAGE,	"image/jpeg" },
	{ ".jpeg", K_IMAGE,	"image/jpeg" },
	{ ".pic",  K_IMAGE,	"image/x-tessronos-picture" },
	{ ".txt",  K_TEXT,	"text/plain" },
	{ ".md",   K_TEXT,	"text/markdown" },
	{ ".csv",  K_TEXT,	"text/csv" },
	{ ".json", K_TEXT,	"application/json" },
	{ ".xml",  K_TEXT,	"application/xml" },
	{ ".html", K_TEXT,	"text/html" },
	{ ".c",    K_TEXT,	"text/x-c" },
	{ ".h",    K_TEXT,	"text/x-c" },
	{ ".py",   K_TEXT,	"text/x-python" },
	{ ".sh",   K_TEXT,	"text/x-shellscript" },
	{ ".log",  K_TEXT,	"text/plain" },
	{ ".dic",  K_DICT,	"application/x-dictionary" },
	{ ".bpk",  K_ARCHIVE,	"application/x-btron-archive" },
};

#define NXFEXT	( (INT)( sizeof(xf_ext) / sizeof(xf_ext[0]) ) )

/* The record type of each kind's record 1 */
LOCAL UINT kind_rt( UINT kind )
{
	switch ( kind ) {
	case K_ELF:	return OB_RT_PROG;
	case K_FONT:	return OB_RT_FONT;
	case K_DICT:	return OB_RT_DICT;
	default:	return OB_RT_SYSDATA;
	}
}

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

LOCAL INT slen( CONST UB *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same_nocase( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( lower(a[i]) != lower(b[i]) ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* Where the extension of a name starts (its dot), or the name's end */
LOCAL INT ext_at( CONST UB *name )
{
	INT	n = slen(name), i;

	for ( i = n - 1; i > 0; i-- ) {
		if ( name[i] == '.' ) return i;
		if ( name[i] == '/' ) break;
	}
	return n;
}

LOCAL CONST XFEXT *ext_of( CONST UB *name )
{
	INT	e = ext_at(name), i, k;

	for ( i = 0; i < NXFEXT; i++ ) {
		CONST char	*x = xf_ext[i].ext;

		for ( k = 0; x[k] != 0 && name[e + k] != 0 && lower(name[e + k]) == (UB)x[k]; k++ ) ;
		if ( x[k] == 0 && name[e + k] == 0 ) return &xf_ext[i];
	}
	return NULL;
}

/* What a file is, from its first bytes and then its name */
LOCAL UINT kind_of( CONST UB *name, CONST UB *head, INT n, CONST char **p_type )
{
	CONST XFEXT	*x = ext_of(name);
	INT		i = 0;

	/* xmlTAD: <tad, after a byte order mark, blanks or an XML declaration */
	if ( n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF ) i = 3;
	while ( i < n && ( head[i] == ' ' || head[i] == '\n' || head[i] == '\r' || head[i] == '\t' ) ) i++;
	if ( i + 5 < n && head[i] == '<' && head[i + 1] == '?' ) {
		while ( i + 1 < n && !( head[i] == '?' && head[i + 1] == '>' ) ) i++;
		for ( i += 2; i < n && ( head[i] == ' ' || head[i] == '\n' || head[i] == '\r' ); i++ ) ;
	}
	if ( i + 4 <= n && head[i] == '<' && head[i + 1] == 't' && head[i + 2] == 'a' && head[i + 3] == 'd' ) {
		*p_type = "application/x-xmltad";
		return K_TAD;
	}
	if ( n >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F' ) {
		*p_type = "application/x-elf";
		return K_ELF;
	}
	if ( n >= 4 && ( ( head[0] == 0 && head[1] == 1 && head[2] == 0 && head[3] == 0 )
		      || ( head[0] == 'O' && head[1] == 'T' && head[2] == 'T' && head[3] == 'O' )
		      || ( head[0] == 't' && head[1] == 't' && head[2] == 'c' && head[3] == 'f' ) ) ) {
		*p_type = ( x != NULL && x->kind == K_FONT ) ? x->mediatype : "font/ttf";
		return K_FONT;
	}
	if ( n >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G' ) {
		*p_type = "image/png";
		return K_IMAGE;
	}
	if ( n >= 2 && head[0] == 0xFF && head[1] == 0xD8 ) {
		*p_type = "image/jpeg";
		return K_IMAGE;
	}
	if ( x != NULL && x->kind != K_TAD && x->kind != K_ELF && x->kind != K_FONT ) {
		*p_type = x->mediatype;
		return x->kind;
	}
	*p_type = "application/octet-stream";
	return K_BINARY;
}

/* ---------------------------------------------------------------- text */

typedef struct {
	UB	*b;
	INT	at, max;
} BUF;

LOCAL void put( BUF *w, CONST char *s )
{
	while ( *s != 0 && w->at < w->max - 1 ) w->b[w->at++] = (UB)*s++;
	w->b[w->at] = 0;
}

LOCAL void put_n( BUF *w, UD v )
{
	char	d[24];
	INT	n = 0;

	do {
		d[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( n > 0 && w->at < w->max - 1 ) w->b[w->at++] = (UB)d[--n];
	w->b[w->at] = 0;
}

/* Text inside an XML attribute or element */
LOCAL void put_xml( BUF *w, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && s[i] != 0 && w->at < w->max - 8; i++ ) {
		switch ( s[i] ) {
		case '&':	put(w, "&amp;");	break;
		case '<':	put(w, "&lt;");		break;
		case '>':	put(w, "&gt;");		break;
		case '"':	put(w, "&quot;");	break;
		case '\r':	break;
		default:	w->b[w->at++] = s[i];	break;
		}
	}
	w->b[w->at] = 0;
}

/* Text inside a JSON string */
LOCAL void put_json( BUF *w, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && s[i] != 0 && w->at < w->max - 8; i++ ) {
		if ( s[i] == '"' || s[i] == '\\' ) {
			w->b[w->at++] = '\\';
			w->b[w->at++] = s[i];
		} else if ( s[i] >= 0x20 ) {
			w->b[w->at++] = s[i];
		}
	}
	w->b[w->at] = 0;
}

/* The time as ISO 8601 in UTC */
LOCAL void put_time( BUF *w, D t )
{
	TS_TM	tm;
	char	s[32];
	TS_TIME	tt = t;

	if ( dt_gmtime(&tt, &tm) >= E_OK
	  && dt_strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0 ) {
		put(w, s);
	}
}

/* ---------------------------------------------------------------- taking in */

/* "tessronos.file": what the file was */
LOCAL void put_file( BUF *w, CONST T_XFSRC *src, CONST char *type, UD size )
{
	put(w, "{\"name\":\"");
	put_json(w, src->filename, slen(src->filename));
	put(w, "\",\"mediatype\":\"");
	put(w, type);
	put(w, "\",\"size\":");
	put_n(w, size);
	if ( src->mtime > 0 ) {
		put(w, ",\"mtime\":\"");
		put_time(w, src->mtime);
		put(w, "\"");
	}
	put(w, ",\"source\":\"");
	put_json(w, (CONST UB *)( ( src->source != NULL ) ? src->source : "" ), XF_NAME_MAX);
	put(w, "\"}");
}

/*
 * The object's metadata when it is made: its name, the records it has,
 * and "tessronos.file". The size is written again once the whole file has
 * been read (meta_again).
 */
LOCAL INT meta_of( BUF *w, CONST T_XFSRC *src, UINT kind, CONST char *type, BOOL has1,
		   UD size )
{
	INT	e = ext_at(src->filename);

	w->at = 0;
	put(w, "{\"name\":\"");
	put_json(w, src->filename, e);
	put(w, "\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":");
	put(w, has1 ? "2" : "1");
	put(w, ",\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":");
	/* an archive opens in 書庫解凍, which reads it from record 1 */
	put(w, ( kind == K_TAD || kind == K_TEXT ) ? "{\"basic-text-editor\":{\"defaultOpen\":true}}"
	       : ( kind == K_ARCHIVE ) ? "{\"unpack-file\":{\"name\":\"書庫解凍\",\"defaultOpen\":true}}" : "{}");
	put(w, ",\"tessronos\":{\"file\":");
	put_file(w, src, type, size);
	put(w, ",\"records\":[{\"n\":0,\"rt\":1}");
	if ( has1 ) {
		put(w, ",{\"n\":1,\"rt\":");
		put_n(w, kind_rt(kind));
		put(w, "}");
	}
	put(w, "]");
	if ( kind == K_ELF ) {
		put(w, ",\"exec\":{\"record\":1,\"type\":\"application/x-elf\","
		       "\"arch\":\"aarch64\",\"abi\":\"tessronos-1\"}");
	}
	if ( src->tessronos != NULL && src->tessronos[0] != 0 ) {
		put(w, ",");
		put(w, src->tessronos);
	}
	put(w, "}}");
	return ( w->at < w->max - 1 ) ? w->at : -1;
}

LOCAL ER make_object( CONST T_XFSRC *src, UINT kind, CONST char *type, BOOL has1,
		      UD size, TS_UUID *p_uuid )
{
	T_OBCRE		c;
	BUF		w;
	INT		n;
	ER		er;
	CONST char	*app;
	UB		*ico;
	SZ		isz = 0;

	w.b = (UB *)Kmalloc(XF_JSON_MAX);
	w.max = XF_JSON_MAX;
	if ( w.b == NULL ) {
		return E_NOMEM;
	}
	n = meta_of(&w, src, kind, type, has1, size);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = w.b;
	c.jsonsz = ( n > 0 ) ? n : 0;
	c.prt = src->prt;

	/* the icon of the template of the program it opens in, as one made from that template has */
	app = ( kind == K_TAD || kind == K_TEXT ) ? "basic-text-editor"
	    : ( kind == K_ARCHIVE ) ? "unpack-file" : NULL;
	ico = ( app != NULL ) ? om_store_app_icon((CONST UB *)app, &isz) : NULL;
	c.icon = ico;
	c.iconsz = ( ico != NULL ) ? isz : 0;

	er = ( n > 0 ) ? ob_cre_obj(&c, p_uuid) : E_LIMIT;
	if ( er == E_PAR && ico != NULL ) {
		c.icon = NULL;			/* an icon the store refuses does not keep the file out */
		c.iconsz = 0;
		er = ob_cre_obj(&c, p_uuid);
	}
	if ( ico != NULL ) {
		Kfree(ico);
	}
	Kfree(w.b);
	return er;
}

/*
 * "tessronos.file" written again, the size now known. Only that member is
 * replaced: what the store added when the object was made (its dates)
 * is kept as it is.
 */
LOCAL ER meta_again( ID key, CONST T_XFSRC *src, CONST char *type, UD size )
{
	T_JSON	root, tf, f;
	UB	*old;
	BUF	w;
	SZ	olen = 0;
	INT	at;
	ER	er;

	old = (UB *)Kmalloc(XF_JSON_MAX);
	w.b = (UB *)Kmalloc(XF_JSON_MAX);
	w.at = 0;
	w.max = XF_JSON_MAX;
	er = ( old != NULL && w.b != NULL ) ? ob_get_atr(key, old, XF_JSON_MAX, &olen) : E_NOMEM;
	if ( er >= E_OK && ( olen >= XF_JSON_MAX || js_parse(old, (INT)olen, &root) < E_OK
			  || js_get(&root, "tessronos", &tf) < E_OK || js_get(&tf, "file", &f) < E_OK ) ) {
		er = E_OBJ;
	}
	if ( er >= E_OK ) {
		at = (INT)( f.s - old );
		knl_memcpy(w.b, old, at);
		w.at = at;
		put_file(&w, src, type, size);
		for ( at += f.len; at < (INT)olen && w.at < w.max - 1; at++ ) w.b[w.at++] = old[at];
		er = ( w.at < w.max - 1 ) ? ob_set_atr(key, w.b, w.at) : E_LIMIT;
	}
	if ( old != NULL ) Kfree(old);
	if ( w.b != NULL ) Kfree(w.b);
	return er;
}

/* An xmlTAD file: gathered whole, and made record 0 as it is */
LOCAL ER take_tad( CONST T_XFSRC *src, UB *first, INT nfirst, TS_UUID *p_uuid )
{
	UB	*all;
	INT	n = nfirst, got, recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	all = (UB *)Kmalloc(XF_TAD_MAX);
	if ( all == NULL ) {
		return E_NOMEM;
	}
	knl_memcpy(all, first, nfirst);
	while ( n < XF_TAD_MAX && ( got = src->read(src->ctx, all + n, XF_TAD_MAX - n) ) > 0 ) {
		n += got;
	}
	if ( n >= XF_TAD_MAX ) {
		Kfree(all);
		return E_LIMIT;
	}
	er = make_object(src, K_TAD, "application/x-xmltad", FALSE, (UD)n, p_uuid);
	if ( er >= E_OK ) {
		key = ob_opn_obj(p_uuid, XF_OPS);
		er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &recno) : (ER)key;
		if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, all, n, &asz);
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < E_OK ) (void)ob_del_obj(p_uuid);
	}
	Kfree(all);
	return er;
}

/* Record 0 of a file kept in record 1: its lines, or its name */
LOCAL ER write_rec0( ID key, CONST T_XFSRC *src, UINT kind, CONST UB *text, INT ntext )
{
	BUF	w;
	SZ	asz = 0;
	INT	i, from;
	ER	er;

	w.max = XF_REC0_MAX;
	w.b = (UB *)Kmalloc(w.max);
	w.at = 0;
	if ( w.b == NULL ) {
		return E_NOMEM;
	}
	put(&w, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	put_xml(&w, src->filename, ext_at(src->filename));
	put(&w, "\"><document>");
	if ( kind == K_TEXT && text != NULL ) {
		for ( i = 0, from = 0; i <= ntext; i++ ) {
			if ( i == ntext || text[i] == '\n' ) {
				put(&w, "<p>");
				put_xml(&w, text + from, i - from);
				put(&w, "</p>");
				from = i + 1;
			}
		}
	} else {
		put(&w, "<p>");
		put_xml(&w, src->filename, slen(src->filename));
		put(&w, "</p>");
	}
	put(&w, "</document></tad>");
	er = ob_wri_rec(key, 0, 0, w.b, w.at, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(key, 0, (UD)w.at);
	Kfree(w.b);
	return er;
}

EXPORT ER xf_import( CONST T_XFSRC *src, CONST TS_UUID *box, TS_UUID *p_uuid )
{
	UB		*buf, *text = NULL;
	CONST char	*type = NULL;
	UINT		kind;
	INT		got, ntext = 0, rec = -1;
	UD		total = 0;
	SZ		asz = 0;
	ID		key;
	ER		er;

	if ( src == NULL || src->filename == NULL || src->read == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(XF_CHUNK);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	got = src->read(src->ctx, buf, XF_CHUNK);
	if ( got < 0 ) {
		Kfree(buf);
		return (ER)got;
	}
	kind = kind_of(src->filename, buf, got, &type);
	if ( kind == K_TAD ) {
		er = take_tad(src, buf, got, p_uuid);
		Kfree(buf);
		if ( er >= E_OK && box != NULL ) er = om_obj_link_add(box, p_uuid);
		return er;
	}

	er = make_object(src, kind, type, TRUE, 0, p_uuid);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	key = ob_opn_obj(p_uuid, XF_OPS);
	er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &rec) : (ER)key;
	if ( er >= E_OK ) er = ob_apd_rec(key, kind_rt(kind), 0, &rec);
	if ( er >= E_OK && kind == K_TEXT ) {
		text = (UB *)Kmalloc(XF_TEXT_MAX);
	}
	/* the bytes, unchanged, into record 1 */
	while ( er >= E_OK && got > 0 ) {
		if ( text != NULL ) {
			INT	k = ( ntext + got <= XF_TEXT_MAX ) ? got : XF_TEXT_MAX - ntext;

			knl_memcpy(text + ntext, buf, k);
			ntext += k;
		}
		er = ob_wri_rec(key, 1, (D)total, buf, got, &asz);
		total += (UD)got;
		if ( er >= E_OK ) got = src->read(src->ctx, buf, XF_CHUNK);
		if ( got < 0 ) er = (ER)got;
	}
	if ( er >= E_OK ) {
		er = write_rec0(key, src, kind, text, ntext);
	}
	if ( er >= E_OK ) {
		er = meta_again(key, src, type, total);
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( text != NULL ) Kfree(text);
	Kfree(buf);
	if ( er < E_OK ) {
		(void)ob_del_obj(p_uuid);		/* nothing half taken in is kept */
		return er;
	}
	if ( box != NULL ) {
		er = om_obj_link_add(box, p_uuid);
	}
	return er;
}

typedef struct {
	INT	fd;
} XFFD;

LOCAL INT fd_read( void *ctx, void *buf, SZ n )
{
	return fs_read(((XFFD *)ctx)->fd, buf, n);
}

EXPORT ER xf_import_path( CONST char *path, CONST char *source, CONST char *tessronos,
			  CONST T_OBPRT *prt, CONST TS_UUID *box, TS_UUID *p_uuid )
{
	T_XFSRC	src;
	T_FSTAT	st;
	XFFD	f;
	INT	i, base = 0;
	ER	er;

	if ( path == NULL ) {
		return E_PAR;
	}
	for ( i = 0; path[i] != 0; i++ ) {
		if ( path[i] == '/' ) base = i + 1;
	}
	f.fd = fs_open(path, O_RDONLY);
	if ( f.fd < 0 ) {
		return E_NOEXS;
	}
	knl_memset(&src, 0, sizeof(src));
	src.filename = (CONST UB *)path + base;
	src.source = source;
	src.mtime = ( fs_fstat(f.fd, &st) >= E_OK ) ? (D)st.mtime : 0;
	src.read = fd_read;
	src.ctx = &f;
	src.tessronos = tessronos;
	src.prt = prt;
	er = xf_import(&src, box, p_uuid);
	fs_close(f.fd);
	return er;
}

/* ---------------------------------------------------------------- a TADjs object */

/* A whole file of the file layer, in memory the caller frees; NULL when it is not there */
LOCAL UB *file_all( CONST char *path, SZ *p_size )
{
	T_FSTAT	st;
	UB	*b;
	INT	fd, n;
	SZ	at = 0;

	if ( fs_stat(path, &st) < EX_OK || st.size > XF_TAD_MAX ) {
		return NULL;
	}
	b = (UB *)Kmalloc((SZ)st.size + 1);
	fd = ( b != NULL ) ? fs_open(path, O_RDONLY) : -1;
	if ( fd < 0 ) {
		if ( b != NULL ) Kfree(b);
		return NULL;
	}
	while ( at < (SZ)st.size && ( n = fs_read(fd, b + at, (SZ)st.size - at) ) > 0 ) {
		at += n;
	}
	fs_close(fd);
	b[at] = 0;
	*p_size = at;
	return b;
}

/* <dir>/<uuid><tail> */
LOCAL void tadjs_path( char *out, INT max, CONST char *dir, CONST char *us, CONST char *tail )
{
	BUF	w;

	w.b = (UB *)out;
	w.at = 0;
	w.max = max;
	put(&w, dir);
	put(&w, "/");
	put(&w, us);
	put(&w, tail);
}

/*
 * The metadata a TADjs object brings, with "tessronos.file" put in: into
 * its "tessronos" when it has one, as a new "tessronos" when it has none. The
 * rest is kept as it came.
 */
LOCAL INT tadjs_meta( CONST UB *j, INT len, CONST char *us, CONST char *source, UD size,
		       UB *out, INT max )
{
	T_JSON	root, tf;
	BUF	w, f;
	UB	fb[512];
	INT	at, i;
	BOOL	has_tf, empty;

	if ( js_parse(j, len, &root) < E_OK || js_type(&root) != JS_OBJECT ) {
		return -1;
	}
	f.b = fb;
	f.at = 0;
	f.max = sizeof(fb);
	put(&f, "{\"name\":\"");
	put(&f, us);
	put(&f, "_0.xtad\",\"mediatype\":\"application/x-xmltad\",\"size\":");
	put_n(&f, size);
	put(&f, ",\"source\":\"");
	put_json(&f, (CONST UB *)source, XF_NAME_MAX);
	put(&f, "\"}");

	has_tf = (BOOL)( js_get(&root, "tessronos", &tf) >= E_OK && js_type(&tf) == JS_OBJECT );
	if ( has_tf ) {
		at = (INT)( tf.s - j ) + 1;			/* just inside its '{' */
		for ( i = 1; i < tf.len - 1 && ( tf.s[i] == ' ' || tf.s[i] == '\n'
						|| tf.s[i] == '\r' || tf.s[i] == '\t' ); i++ ) ;
		empty = (BOOL)( tf.s[i] == '}' );
	} else {
		at = (INT)( root.s - j ) + root.len - 1;	/* at the root's '}' */
		for ( i = root.len - 2; i > 0 && ( root.s[i] == ' ' || root.s[i] == '\n'
						|| root.s[i] == '\r' || root.s[i] == '\t' ); i-- ) ;
		empty = (BOOL)( root.s[i] == '{' );
	}
	w.b = out;
	w.at = 0;
	w.max = max;
	if ( at >= max ) {
		return -1;
	}
	knl_memcpy(out, j, at);
	w.at = at;
	if ( has_tf ) {
		put(&w, "\"file\":");
		put(&w, (CONST char *)fb);
		if ( !empty ) put(&w, ",");
	} else {
		if ( !empty ) put(&w, ",");
		put(&w, "\"tessronos\":{\"file\":");
		put(&w, (CONST char *)fb);
		put(&w, "}");
	}
	for ( ; at < len && w.at < w.max - 1; at++ ) out[w.at++] = j[at];
	out[w.at] = 0;
	return ( w.at < w.max - 1 ) ? w.at : -1;
}

/* {uuid}.ico of a file set, read whole; NULL when there is none or it is no icon */
LOCAL UB *tadjs_icon( CONST char *dir, CONST char *us, SZ *p_len )
{
	char	path[FS_PATH_MAX];
	UB	*ico;

	*p_len = 0;
	tadjs_path(path, sizeof(path), dir, us, ".ico");
	ico = file_all(path, p_len);
	if ( ico != NULL && ( *p_len > OB_ICO_MAX || *p_len < 8
			      || !( ( ico[0] == 0 && ico[1] == 0 && ico[2] == 1 && ico[3] == 0 )
				 || ( ico[0] == 0x89 && ico[1] == 'P' && ico[2] == 'N' && ico[3] == 'G' ) ) ) ) {
		tm_printf((UB *)"xf: the icon of %s is not an icon; left out\n", us);
		Kfree(ico);
		ico = NULL;
		*p_len = 0;
	}
	return ico;
}

EXPORT ER xf_import_tadjs( CONST char *dir, CONST TS_UUID *uuid, CONST char *source,
			   CONST T_OBPRT *prt, CONST TS_UUID *box )
{
	char	us[TS_UUID_STRLEN + 1], path[FS_PATH_MAX], tail[16];
	T_OBCRE	c;
	T_OBREF	r;
	T_FSTAT	st;
	TS_UUID	got;
	UB	*j, *meta, *rec;
	UD	size0;
	SZ	jl = 0, rl = 0;
	INT	ml, n;
	ER	er;

	if ( dir == NULL || uuid == NULL || ts_uuid_to_str(uuid, us, sizeof(us)) < E_OK ) {
		return E_PAR;
	}
	if ( ob_ref_obj(uuid, &r) >= E_OK ) {
		/* taken in before; its icon given now when it was taken in without one */
		er = E_OK;
		meta = om_obj_icon(uuid, &rl);
		if ( meta != NULL ) {
			Kfree(meta);
		} else if ( ( rec = tadjs_icon(dir, us, &rl) ) != NULL ) {
			if ( om_obj_icon_put(uuid, rec, rl) < E_OK ) {
				tm_printf((UB *)"xf: the icon of %s could not be given\n", us);
			}
			Kfree(rec);
		}
		goto boxed;
	}
	tadjs_path(path, sizeof(path), dir, us, "_0.xtad");
	size0 = ( fs_stat(path, &st) >= EX_OK ) ? (UD)st.size : 0;
	tadjs_path(path, sizeof(path), dir, us, ".json");
	j = file_all(path, &jl);
	if ( j == NULL ) {
		return E_NOEXS;
	}
	meta = (UB *)Kmalloc(XF_JSON_MAX);
	ml = ( meta != NULL ) ? tadjs_meta(j, (INT)jl, us, ( source != NULL ) ? source : "",
					   size0, meta, XF_JSON_MAX) : -1;
	Kfree(j);
	if ( ml <= 0 ) {
		if ( meta != NULL ) Kfree(meta);
		return ( meta != NULL ) ? E_PAR : E_NOMEM;
	}
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = meta;
	c.jsonsz = ml;
	c.prt = prt;
	c.uuid = *uuid;
	/* its icon, {uuid}.ico, made with it as its metadata is */
	rec = tadjs_icon(dir, us, &rl);
	c.icon = rec;
	c.iconsz = ( rec != NULL ) ? (SZ)rl : 0;
	er = ob_cre_obj(&c, &got);
	Kfree(meta);
	if ( rec != NULL ) {
		Kfree(rec);
	}

	/* its records, {uuid}_0.xtad, _1.xtad, ... as long as there are more */
	for ( n = 0; er >= E_OK; n++ ) {
		BUF	t;

		t.b = (UB *)tail;
		t.at = 0;
		t.max = sizeof(tail);
		put(&t, "_");
		put_n(&t, (UD)n);
		put(&t, ".xtad");
		tadjs_path(path, sizeof(path), dir, us, tail);
		rec = file_all(path, &rl);
		if ( rec == NULL ) break;
		er = om_obj_record_put(uuid, n, OB_RT_TAD, 0, rec, rl);
		Kfree(rec);
	}
	if ( er < E_OK ) {
		(void)ob_del_obj(uuid);
		return er;
	}
    boxed:
	return ( box != NULL ) ? om_obj_link_add(box, uuid) : E_OK;
}

/* ---------------------------------------------------------------- what was taken in */

/* The file's name the object keeps in "tessronos.file.name", or an empty name */
LOCAL INT file_name( CONST TS_UUID *uuid, UB *out, INT max )
{
	T_JSON	root, tf, f;
	UB	*meta;
	SZ	size = 0;
	INT	n = 0;

	out[0] = 0;
	meta = om_obj_meta(uuid, &size);
	if ( meta == NULL ) {
		return 0;
	}
	if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "file", &f) >= E_OK ) {
		n = js_get_str(&f, "name", out, max);
		if ( n < 0 ) n = 0;
	}
	Kfree(meta);
	return n;
}

EXPORT INT xf_data_rec( CONST TS_UUID *uuid )
{
	T_OBREF	r;

	if ( ob_ref_obj(uuid, &r) < E_OK ) {
		return E_NOEXS;
	}
	return ( r.nrec >= 2 ) ? 1 : 0;
}

EXPORT ER xf_name( CONST TS_UUID *uuid, UB *name, INT max )
{
	T_OBREF	r;
	INT	n;

	if ( name == NULL || max <= 0 ) {
		return E_PAR;
	}
	n = file_name(uuid, name, max);
	if ( n == 0 && ob_ref_obj(uuid, &r) >= E_OK ) {
		/* made here, never a file: its name, as xmlTAD */
		CONST char	*x = ".xtad";
		INT		k;

		for ( ; r.name[n] != 0 && n < max - 6; n++ ) name[n] = r.name[n];
		for ( k = 0; x[k] != 0; k++ ) name[n++] = (UB)x[k];
		name[n] = 0;
	}
	return ( n > 0 ) ? E_OK : E_NOEXS;
}

EXPORT ER xf_find( CONST TS_UUID *box, CONST UB *filename, TS_UUID *p_uuid )
{
	TS_UUID	*ids;
	UB	name[XF_NAME_MAX];
	INT	n = 0, i;
	ER	er = E_NOEXS;

	if ( box == NULL || filename == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	if ( ids != NULL && ob_lst_lnk(box, NULL, ids, 256, &n) >= E_OK ) {
		for ( i = 0; i < n && i < 256 && er < E_OK; i++ ) {
			if ( xf_name(&ids[i], name, sizeof(name)) >= E_OK && same_nocase(name, filename) ) {
				*p_uuid = ids[i];
				er = E_OK;
			}
		}
	}
	if ( ids != NULL ) Kfree(ids);
	return er;
}

EXPORT ER xf_rename( CONST TS_UUID *uuid, CONST UB *filename )
{
	T_JSON	root, tf, f, nv;
	UB	*old, stem[OB_NAME_MAX];
	BUF	w;
	SZ	olen = 0;
	INT	e, i, at;
	ID	key;
	ER	er;

	if ( uuid == NULL || filename == NULL || filename[0] == 0 ) {
		return E_PAR;
	}
	e = ext_at(filename);
	for ( i = 0; i < e && i < OB_NAME_MAX - 1; i++ ) stem[i] = filename[i];
	stem[i] = 0;
	er = om_store_rename(uuid, ( i > 0 ) ? stem : filename);
	if ( er < E_OK ) {
		return er;
	}

	/* "tessronos.file.name", when it was taken in from a file */
	key = ob_opn_obj(uuid, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key <= 0 ) {
		return (ER)key;
	}
	old = (UB *)Kmalloc(XF_JSON_MAX);
	w.b = (UB *)Kmalloc(XF_JSON_MAX);
	w.at = 0;
	w.max = XF_JSON_MAX;
	er = ( old != NULL && w.b != NULL ) ? ob_get_atr(key, old, XF_JSON_MAX, &olen) : E_NOMEM;
	if ( er >= E_OK && olen < XF_JSON_MAX && js_parse(old, (INT)olen, &root) >= E_OK
	  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "file", &f) >= E_OK
	  && js_get(&f, "name", &nv) >= E_OK ) {
		at = (INT)( nv.s - old );
		knl_memcpy(w.b, old, at);
		w.at = at;
		put(&w, "\"");
		put_json(&w, filename, slen(filename));
		put(&w, "\"");
		for ( at += nv.len; at < (INT)olen && w.at < w.max - 1; at++ ) w.b[w.at++] = old[at];
		er = ( w.at < w.max - 1 ) ? ob_set_atr(key, w.b, w.at) : E_LIMIT;
	}
	ob_cls_obj(key);
	if ( old != NULL ) Kfree(old);
	if ( w.b != NULL ) Kfree(w.b);
	return er;
}

/* ---------------------------------------------------------------- giving out */

EXPORT ER xf_export( CONST TS_UUID *uuid, XF_WRITE write, void *ctx, UB *name, INT max )
{
	UB	*buf;
	SZ	asz = 0;
	D	at = 0;
	INT	rec;
	ID	key;
	ER	er;

	if ( uuid == NULL || write == NULL ) {
		return E_PAR;
	}
	if ( name != NULL && ( er = xf_name(uuid, name, max) ) < E_OK ) {
		return er;
	}
	rec = xf_data_rec(uuid);
	if ( rec < 0 ) {
		return (ER)rec;
	}
	key = ob_opn_obj(uuid, OB_OP_R);
	if ( key <= 0 ) {
		return (ER)key;
	}
	buf = (UB *)Kmalloc(XF_CHUNK);
	er = ( buf != NULL ) ? E_OK : E_NOMEM;
	while ( er >= E_OK ) {
		er = ob_rea_rec(key, rec, at, buf, XF_CHUNK, &asz);
		if ( er < E_OK || asz <= 0 ) break;
		if ( write(ctx, buf, asz) < 0 ) {
			er = E_IO;
			break;
		}
		at += asz;
		if ( asz < XF_CHUNK ) break;
	}
	if ( er == E_NOEXS && at > 0 ) er = E_OK;	/* read past the end */
	if ( buf != NULL ) Kfree(buf);
	ob_cls_obj(key);
	return ( er >= E_OK ) ? E_OK : er;
}
