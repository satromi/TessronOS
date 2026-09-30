/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_imp.c
 *	A tree of files taken in as real objects (design 18.20)
 *
 *	A file is read once, as it comes: the first bytes and the name's
 *	extension say what kind of file it is (the same table and the same
 *	marks as include/ts/xf.h, so a file is the same kind whichever
 *	takes it in), the object is made, and every byte goes unchanged into
 *	record 1. What the object shows in record 0 is made last: the lines
 *	of a text, decoded from whatever encoding it was in; a picture, kept
 *	beside the records as a PNG and placed in a figure; the file's name
 *	for anything else. The facts of the file go into "tessronos.file" when
 *	its size and checksum are known, with the checksum of record 0 as
 *	made, so that giving it out later can tell whether it was changed.
 *
 *	A directory becomes a figure of links to what it holds, made after
 *	them so that each link reaches an object that is there. The files of
 *	a TADjs object in it become that object under its UUID, and only the
 *	objects of those no other one links to are linked from the figure.
 */

#include "xfu_in.h"

/* What taking in does to a new object: not its deletion */
#define IMP_OPS		( OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR )

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
} XEXT;

/* The extensions include/ts/xf.h knows, and a few more of the same kinds */
LOCAL CONST XEXT ext_tab[] = {
	{ ".xtad", K_TAD,	XFU_MT_TAD },
	{ ".elf",  K_ELF,	"application/x-elf" },
	{ ".ttf",  K_FONT,	"font/ttf" },
	{ ".otf",  K_FONT,	"font/otf" },
	{ ".ttc",  K_FONT,	"font/collection" },
	{ ".png",  K_IMAGE,	"image/png" },
	{ ".jpg",  K_IMAGE,	"image/jpeg" },
	{ ".jpeg", K_IMAGE,	"image/jpeg" },
	{ ".bmp",  K_IMAGE,	"image/bmp" },
	{ ".gif",  K_IMAGE,	"image/gif" },
	{ ".pic",  K_IMAGE,	"image/x-tessronos-picture" },
	{ ".txt",  K_TEXT,	"text/plain" },
	{ ".md",   K_TEXT,	"text/markdown" },
	{ ".csv",  K_TEXT,	"text/csv" },
	{ ".tsv",  K_TEXT,	"text/tab-separated-values" },
	{ ".json", K_TEXT,	"application/json" },
	{ ".xml",  K_TEXT,	"application/xml" },
	{ ".html", K_TEXT,	"text/html" },
	{ ".htm",  K_TEXT,	"text/html" },
	{ ".css",  K_TEXT,	"text/css" },
	{ ".js",   K_TEXT,	"text/javascript" },
	{ ".c",    K_TEXT,	"text/x-c" },
	{ ".h",    K_TEXT,	"text/x-c" },
	{ ".py",   K_TEXT,	"text/x-python" },
	{ ".sh",   K_TEXT,	"text/x-shellscript" },
	{ ".ini",  K_TEXT,	"text/plain" },
	{ ".log",  K_TEXT,	"text/plain" },
	{ ".dic",  K_DICT,	"application/x-dictionary" },
	{ ".bpk",  K_ARCHIVE,	"application/x-btron-archive" },
	{ ".tpk",  K_BINARY,	"application/x-tessronos-package" },
};

#define NEXT	( (INT)( sizeof(ext_tab) / sizeof(ext_tab[0]) ) )

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Where the extension of a name starts (its dot), or the name's end */
LOCAL INT ext_at( CONST UB *name )
{
	INT	n = xfu_slen(name), i;

	for ( i = n - 1; i > 0; i-- ) {
		if ( name[i] == '.' ) return i;
	}
	return n;
}

LOCAL CONST XEXT *ext_of( CONST UB *name )
{
	INT	e = ext_at(name), i, k;

	for ( i = 0; i < NEXT; i++ ) {
		CONST char	*x = ext_tab[i].ext;

		for ( k = 0; x[k] != 0 && name[e + k] != 0 && lower(name[e + k]) == (UB)x[k]; k++ ) ;
		if ( x[k] == 0 && name[e + k] == 0 ) return &ext_tab[i];
	}
	return NULL;
}

/* What a file is, from its first bytes and then its name */
LOCAL UINT kind_of( CONST UB *name, CONST UB *head, INT n, CONST char **p_type )
{
	CONST XEXT	*x = ext_of(name);
	INT		i = 0;

	if ( n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF ) i = 3;
	while ( i < n && ( head[i] == ' ' || head[i] == '\n' || head[i] == '\r' || head[i] == '\t' ) ) i++;
	if ( i + 5 < n && head[i] == '<' && head[i + 1] == '?' ) {
		while ( i + 1 < n && !( head[i] == '?' && head[i + 1] == '>' ) ) i++;
		for ( i += 2; i < n && ( head[i] == ' ' || head[i] == '\n' || head[i] == '\r' ); i++ ) ;
	}
	if ( i + 4 <= n && head[i] == '<' && head[i + 1] == 't' && head[i + 2] == 'a' && head[i + 3] == 'd' ) {
		*p_type = XFU_MT_TAD;
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
	if ( n >= 4 && head[0] == 'G' && head[1] == 'I' && head[2] == 'F' && head[3] == '8' ) {
		*p_type = "image/gif";
		return K_IMAGE;
	}
	if ( x != NULL && x->kind != K_TAD && x->kind != K_ELF && x->kind != K_FONT ) {
		*p_type = x->mediatype;
		return x->kind;
	}
	*p_type = "application/octet-stream";
	return K_BINARY;
}

/*
 * What the caller said a file is, over what it looks like: a text, a
 * picture, bytes alone or an archive. A file said to be a picture that
 * cannot be read as one is shown by its name, as any other file.
 */
LOCAL UINT kind_as( UINT as, UINT kind, CONST char **p_type )
{
	switch ( as ) {
	case XFU_AS_TEXT:
	case XFU_AS_ZEN:
		if ( kind != K_TEXT ) *p_type = "text/plain";
		return K_TEXT;
	case XFU_AS_IMAGE:
		if ( kind != K_IMAGE ) *p_type = "application/octet-stream";
		return K_IMAGE;
	case XFU_AS_RAW:
		return K_BINARY;
	case XFU_AS_ARCHIVE:
		*p_type = "application/x-btron-archive";
		return K_ARCHIVE;
	default:
		return kind;
	}
}

LOCAL UINT kind_rt( UINT kind )
{
	switch ( kind ) {
	case K_ELF:	return OB_RT_PROG;
	case K_FONT:	return OB_RT_FONT;
	case K_DICT:	return OB_RT_DICT;
	default:	return OB_RT_SYSDATA;
	}
}

/* ---------------------------------------------------------------- the objects made */

/* An object's name from a file's: the full width forms back, cut to fit */
LOCAL void obj_name( CONST UB *file, INT n, UB *out )
{
	UB	tmp[XFU_NAME_MAX];
	INT	i, k, c, at = 0, len;

	for ( i = 0; i < n && i < XFU_NAME_MAX - 1; i++ ) tmp[i] = file[i];
	tmp[i] = 0;
	len = xfu_name_in(tmp, tmp, sizeof(tmp));
	for ( i = 0; i < len; i += k ) {
		k = txc_utf8_get(tmp + i, len - i, &c);
		if ( at + k >= OB_NAME_MAX ) break;
		xfu_mcpy(out + at, tmp + i, k);
		at += k;
	}
	out[at] = 0;
}

/* What "tessronos.file" says of a file */
typedef struct {
	CONST UB	*name;
	CONST char	*type;
	D		size;
	D		mtime;
	UINT		crc;
	BOOL		hascrc;
	INT		enc;		/* a text's, TXC_AUTO for none */
	BOOL		crlf;
	UINT		rec0;
	BOOL		hasrec0;
} FILEFACT;

LOCAL void put_file( XFU *x, XBUF *w, CONST FILEFACT *f )
{
	UB	t[24];

	xb_put(w, "{\"name\":\"");
	xb_json(w, f->name, xfu_slen(f->name));
	xb_put(w, "\",\"mediatype\":\"");
	xb_put(w, f->type);
	xb_put(w, "\"");
	if ( f->size >= 0 ) {
		xb_put(w, ",\"size\":");
		xb_num(w, f->size);
	}
	if ( f->mtime > 0 ) {
		xb_put(w, ",\"mtime\":\"");
		(void)xfu_time_iso(f->mtime, t);
		xb_put(w, (CONST char *)t);
		xb_put(w, "\"");
	}
	xb_put(w, ",\"source\":\"");
	if ( x->opt != NULL && x->opt->source != NULL ) {
		xb_json(w, (CONST UB *)x->opt->source, xfu_slen((CONST UB *)x->opt->source));
	}
	xb_put(w, "\"");
	if ( f->hascrc ) {
		xb_put(w, ",\"crc32\":");
		xb_num(w, f->crc);
	}
	if ( f->enc != TXC_AUTO ) {
		xb_put(w, ",\"encoding\":\"");
		xb_put(w, txc_name(f->enc));
		xb_put(w, "\"");
		if ( f->crlf ) xb_put(w, ",\"eol\":\"crlf\"");
	}
	if ( f->hasrec0 ) {
		xb_put(w, ",\"rec0\":");
		xb_num(w, f->rec0);
	}
	xb_put(w, "}");
}

/* The applist of a new object: the program it opens with, a list for a directory */
LOCAL void put_apps( XFU *x, XBUF *w, INT app )
{
	XFUAPP	*a;
	INT	i, n = 0, list[2];

	xb_put(w, "\"applist\":{");
	if ( app == XFU_APP_LIST ) {
		list[n++] = XFU_APP_LIST;
		list[n++] = XFU_APP_FIG;
	} else if ( app != XFU_APP_NONE ) {
		list[n++] = app;
	}
	for ( i = 0; i < n; i++ ) {
		if ( i > 0 ) xb_put(w, ",");
		xb_put(w, "\"");
		xb_put(w, xfu_app_id(list[i]));
		xb_put(w, "\":{");
		a = xfu_app(x, list[i]);
		if ( a != NULL && a->name[0] != 0 ) {
			xb_put(w, "\"name\":\"");
			xb_json(w, a->name, xfu_slen(a->name));
			xb_put(w, "\",");
		}
		xb_put(w, ( i == 0 ) ? "\"defaultOpen\":true}" : "\"defaultOpen\":false}");
	}
	xb_put(w, "}");
}

/*
 * A new object: its name, the program it opens with, "tessronos.file"
 * and the types of its records. Nothing links to it yet (refCount 0):
 * a link is counted when it is written.
 */
LOCAL ER make_obj( XFU *x, CONST UB *name, INT app, UINT kind, CONST FILEFACT *f, INT nrec,
		   TS_UUID *p_uuid )
{
	T_OBCRE	c;
	XBUF	w;
	XFUAPP	*a;
	ER	er;

	xb_init(&w);
	xb_put(&w, "{\"name\":\"");
	xb_json(&w, name, xfu_slen(name));
	xb_put(&w, "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,\"recordCount\":");
	xb_num(&w, nrec);
	xb_put(&w, ",\"editable\":true,\"deletable\":true,\"readable\":true,");
	put_apps(x, &w, app);
	xb_put(&w, ",\"tessronos\":{\"file\":");
	put_file(x, &w, f);
	xb_put(&w, ",\"records\":[{\"n\":0,\"rt\":1}");
	if ( nrec > 1 ) {
		xb_put(&w, ",{\"n\":1,\"rt\":");
		xb_num(&w, kind_rt(kind));
		xb_put(&w, "}");
	}
	xb_put(&w, "]");
	if ( kind == K_ELF ) {
		xb_put(&w, ",\"exec\":{\"record\":1,\"type\":\"application/x-elf\","
			   "\"arch\":\"aarch64\",\"abi\":\"tessronos-1\"}");
	}
	xb_put(&w, "}}");
	if ( w.bad || w.at > OB_ATR_MAX ) {
		xb_free(&w);
		return w.bad ? E_NOMEM : E_LIMIT;
	}
	xfu_mset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = w.b;
	c.jsonsz = w.at;
	c.prt = ( x->opt != NULL ) ? x->opt->prt : NULL;
	if ( x->opt != NULL && x->opt->vol != NULL ) {
		c.vol = x->opt->vol;
	} else if ( x->hasnear ) {
		c.near = x->near;
	}
	a = ( app != XFU_APP_NONE ) ? xfu_app(x, app) : NULL;
	if ( a != NULL && a->icon != NULL ) {
		c.icon = a->icon;
		c.iconsz = a->iconsz;
	}
	er = ob_cre_obj(&c, p_uuid);
	xb_free(&w);
	if ( er >= E_OK ) {
		x->st.objects++;
		xfu_made(x, p_uuid);
	}
	return er;
}

/* A record written whole, and cut to that */
LOCAL ER put_rec( ID key, INT recno, CONST UB *b, SZ n )
{
	SZ	done = 0, asz;
	ER	er = E_OK;

	while ( er >= E_OK && done < n ) {
		asz = 0;
		er = ob_wri_rec(key, recno, (D)done, b + done, n - done, &asz);
		if ( er >= E_OK && asz <= 0 ) er = E_IO;
		done += asz;
	}
	return ( er >= E_OK ) ? ob_trn_rec(key, recno, (UD)n) : er;
}

/* ---------------------------------------------------------------- record 0 */

/* <tad> and its body opened, with the file's name as the document's */
LOCAL void tad_head( XBUF *w, CONST UB *name, CONST char *body )
{
	xb_put(w, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	xb_xml(w, name, xfu_slen(name));
	xb_put(w, "\">\n<");
	xb_put(w, body);
	xb_put(w, ">\n");
}

/* A document of one paragraph: the file's name */
LOCAL void rec0_name( XBUF *w, CONST UB *stem, CONST UB *file )
{
	tad_head(w, stem, "document");
	xb_put(w, "<p>");
	xb_xml(w, file, xfu_slen(file));
	xb_put(w, "</p>\n</document>\n</tad>\n");
}

/*
 * The half width katakana U+FF61 to U+FF9F as the full width letters
 * they stand for, the voiced marks after them joined on where a letter
 * with the mark exists.
 */
LOCAL CONST UH kana_full[0x3F] = {
	0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3,
	0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,
	0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF,
	0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF,
	0x30C1, 0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD,
	0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB, 0x30DE, 0x30DF,
	0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, 0x30EA,
	0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C
};

/* The letter c with the voiced mark (U+FF9E) or the half voiced one (U+FF9F), or 0 */
LOCAL INT kana_voiced( INT c, INT mark )
{
	if ( mark == 0xFF9E ) {
		if ( c == 0x30A6 ) return 0x30F4;
		if ( ( c >= 0x30AB && c <= 0x30C1 && ( c & 1 ) == 1 ) || c == 0x30C4 || c == 0x30C6
		  || c == 0x30C8 ) return c + 1;
		if ( c >= 0x30CF && c <= 0x30DB && ( c - 0x30CF ) % 3 == 0 ) return c + 1;
	} else if ( mark == 0xFF9F ) {
		if ( c >= 0x30CF && c <= 0x30DB && ( c - 0x30CF ) % 3 == 0 ) return c + 2;
	}
	return 0;
}

/*
 * A text in UTF-8 made full width: the letters, digits and signs of
 * ASCII as their full width forms, a blank as the ideographic space,
 * the half width katakana as the full width ones. The control
 * characters (the line ends among them) stay. A new buffer of
 * xfu_sys_alloc, its length into *p_n.
 */
LOCAL UB *zen_utf8( CONST UB *s, SZ *p_n )
{
	SZ	n = *p_n, i, at = 0;
	UB	*o = (UB *)xfu_sys_alloc(n * 3 + 1);
	INT	c, k, v, m;

	if ( o == NULL ) {
		return NULL;
	}
	for ( i = 0; i < n; i += k ) {
		k = txc_utf8_get(s + i, n - i, &c);
		if ( k <= 0 ) {
			k = 1;
			c = 0xFFFD;
		}
		if ( c == 0x20 ) {
			c = 0x3000;
		} else if ( c > 0x20 && c < 0x7F ) {
			c += 0xFEE0;
		} else if ( c >= 0xFF61 && c <= 0xFF9F ) {
			c = kana_full[c - 0xFF61];
			if ( i + k < n ) {
				INT	k2 = txc_utf8_get(s + i + k, n - i - k, &m);

				v = ( k2 > 0 ) ? kana_voiced(c, m) : 0;
				if ( v != 0 ) {
					c = v;
					k += k2;
				}
			}
		}
		at += txc_utf8_put(c, o + at);
	}
	o[at] = 0;
	*p_n = at;
	return o;
}

/*
 * A text as a document: each line a paragraph, its end (LF or CRLF)
 * taken off; a last line with no end is a paragraph too. Answers
 * whether most of the line ends were CRLF.
 */
LOCAL BOOL rec0_text( XFU *x, XBUF *w, CONST UB *stem, CONST UB *s, SZ n, INT enc )
{
	UB	*u;
	SZ	un, i, from;
	INT	bad = 0, crlf = 0, lf = 0;

	un = txc_to_utf8(enc, s, n, NULL, 0, NULL);
	u = (UB *)xfu_sys_alloc(un + 1);
	if ( u == NULL ) {
		w->bad = TRUE;
		return FALSE;
	}
	(void)txc_to_utf8(enc, s, n, u, un + 1, &bad);
	x->st.geta += bad;
	if ( x->zen ) {
		UB	*z = zen_utf8(u, &un);

		xfu_sys_free(u);
		if ( z == NULL ) {
			w->bad = TRUE;
			return FALSE;
		}
		u = z;
	}
	tad_head(w, stem, "document");
	for ( i = 0, from = 0; i < un; i++ ) {
		if ( u[i] == '\n' ) {
			SZ	end = i;

			if ( end > from && u[end - 1] == '\r' ) {
				end--;
				crlf++;
			} else {
				lf++;
			}
			xb_put(w, "<p>");
			xb_xml(w, u + from, end - from);
			xb_put(w, "</p>\n");
			from = i + 1;
		}
	}
	if ( from < un ) {
		xb_put(w, "<p>");
		xb_xml(w, u + from, un - from);
		xb_put(w, "</p>\n");
	}
	xb_put(w, "</document>\n</tad>\n");
	xfu_sys_free(u);
	return (BOOL)( crlf > lf );
}

/*
 * Pixels made 1/k as wide and as tall, each new pixel the mean of the
 * k by k it stands for (the clear ones left out of it; clear when all
 * were). The old pixels are given back and the new ones put in their
 * place.
 */
LOCAL ER img_shrink( UINT **p_px, INT *p_w, INT *p_h, INT k )
{
	UINT	*px = *p_px, *o;
	INT	w = *p_w, h = *p_h, nw, nh, x, y, i, j, cnt;
	UINT	r, g, b, c;

	nw = ( w + k - 1 ) / k;
	nh = ( h + k - 1 ) / k;
	o = (UINT *)xfu_sys_alloc((SZ)nw * nh * sizeof(UINT));
	if ( o == NULL ) {
		return E_NOMEM;
	}
	for ( y = 0; y < nh; y++ ) {
		for ( x = 0; x < nw; x++ ) {
			r = g = b = 0;
			cnt = 0;
			for ( j = y * k; j < y * k + k && j < h; j++ ) {
				for ( i = x * k; i < x * k + k && i < w; i++ ) {
					c = px[(SZ)j * w + i];
					if ( c == 0xFFFFFFFFU ) continue;
					r += ( c >> 16 ) & 0xFF;
					g += ( c >> 8 ) & 0xFF;
					b += c & 0xFF;
					cnt++;
				}
			}
			o[(SZ)y * nw + x] = ( cnt == 0 ) ? 0xFFFFFFFFU
					  : ( ( r / cnt ) << 16 ) | ( ( g / cnt ) << 8 ) | ( b / cnt );
		}
	}
	xfu_sys_free(px);
	*p_px = o;
	*p_w = nw;
	*p_h = nh;
	return E_OK;
}

/*
 * A picture: kept beside the records as a PNG (as it came, or made one
 * from the pixels), and a figure the size of it that shows it.
 */
LOCAL ER rec0_image( ID key, CONST TS_UUID *u, XBUF *w, CONST UB *stem, CONST UB *data, SZ n, INT shrink )
{
	UB	*png = NULL;
	SZ	plen = 0;
	UINT	*px = NULL;
	INT	pw = 0, ph = 0;
	char	us[40];
	ER	er;

	if ( shrink <= 1 && xfu_png_size(data, n, &pw, &ph) >= E_OK ) {
		er = ob_wri_res(key, (CONST UB *)"_0_0.png", data, n);
	} else {
		er = xfu_sys_decode(data, n, &px, &pw, &ph);
		if ( er >= E_OK && shrink > 1 ) er = img_shrink(&px, &pw, &ph, shrink);
		if ( er >= E_OK ) {
			er = xfu_png_encode(px, pw, ph, &png, &plen);
			xfu_sys_free(px);
		}
		if ( er >= E_OK ) {
			er = ob_wri_res(key, (CONST UB *)"_0_0.png", png, plen);
			xfu_sys_free(png);
		}
	}
	if ( er < E_OK ) {
		return er;
	}
	tad_head(w, stem, "figure");
	xb_put(w, "<figView top=\"0\" left=\"0\" right=\"");
	xb_num(w, pw);
	xb_put(w, "\" bottom=\"");
	xb_num(w, ph);
	xb_put(w, "\"/>\n<figDraw top=\"0\" left=\"0\" right=\"");
	xb_num(w, pw);
	xb_put(w, "\" bottom=\"");
	xb_num(w, ph);
	xb_put(w, "\"/>\n<figScale hunit=\"-96\" vunit=\"-96\"/>\n");
	xb_put(w, "<image lineType=\"0\" lineWidth=\"0\" l_pat=\"0\" f_pat=\"0\" angle=\"0\""
		  " rotation=\"0\" flipH=\"false\" flipV=\"false\" left=\"0\" top=\"0\" right=\"");
	xb_num(w, pw);
	xb_put(w, "\" bottom=\"");
	xb_num(w, ph);
	xb_put(w, "\" href=\"");
	xfu_uuid_str(u, us);
	xb_put(w, us);
	xb_put(w, "_0_0.png\" zIndex=\"1\"/>\n</figure>\n</tad>\n");
	return E_OK;
}

/* ---------------------------------------------------------------- a file */

/*
 * As many bytes as there are, up to n. Fewer than n means the end was
 * met, and the file is not read again: a connection that has closed
 * may answer a further read with a fault.
 */
LOCAL INT fill( CONST T_XFUTREE *t, INT h, UB *b, INT n, BOOL *p_eof )
{
	INT	got = 0, k;

	if ( *p_eof ) {
		return 0;
	}
	while ( got < n ) {
		k = t->read(t->ctx, h, b + got, n - got);
		if ( k < 0 ) return k;
		if ( k == 0 ) {
			*p_eof = TRUE;
			break;
		}
		got += k;
	}
	return got;
}

/* An xmlTAD file: gathered whole and made record 0 as it is */
LOCAL ER take_tad( XFU *x, INT h, INT got, BOOL *p_eof, CONST UB *file, CONST UB *stem, D mtime,
		   TS_UUID *p_uuid )
{
	CONST T_XFUTREE	*t = x->t;
	FILEFACT	f;
	XBUF		all;
	INT		recno = -1;
	ID		key;
	ER		er = E_OK;

	xb_init(&all);
	while ( got > 0 && !all.bad ) {
		xb_bytes(&all, x->chunk, got);
		if ( all.at > XFU_TAD_MAX ) {
			er = E_LIMIT;
			break;
		}
		got = fill(t, h, x->chunk, XFU_CHUNK, p_eof);
		if ( got < 0 ) er = (ER)got;
	}
	if ( er >= E_OK && all.bad ) er = E_NOMEM;
	if ( er >= E_OK ) {
		xfu_mset(&f, 0, sizeof(f));
		f.name = file;
		f.type = XFU_MT_TAD;
		f.size = (D)all.at;
		f.mtime = mtime;
		f.crc = xfu_crc32(0, all.b, all.at);
		f.hascrc = TRUE;
		f.enc = TXC_AUTO;
		er = make_obj(x, stem, ( xfu_find(all.b, all.at, "<figure", 0) >= 0 )
				       ? XFU_APP_FIG : XFU_APP_TEXT, K_TAD, &f, 1, p_uuid);
	}
	if ( er >= E_OK ) {
		key = ob_opn_obj(p_uuid, IMP_OPS);
		er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &recno) : (ER)key;
		if ( er >= E_OK ) er = put_rec(key, recno, all.b, all.at);
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < E_OK ) {
			(void)ob_del_obj(p_uuid);
			xfu_unmade(x, p_uuid);
			x->st.objects--;
		}
	}
	xb_free(&all);
	return er;
}

LOCAL ER imp_file( XFU *x, CONST UB *path, CONST UB *file, D mtime, UINT as, TS_UUID *p_uuid )
{
	CONST T_XFUTREE	*t = x->t;
	CONST char	*type = NULL;
	UB		stem[OB_NAME_MAX];
	FILEFACT	f;
	XBUF		keep, r0;
	UINT		kind, crc = 0;
	INT		h, got, rec = -1, enc = TXC_AUTO;
	D		total = 0;
	BOOL		crlf = FALSE, keeping, eof = FALSE;
	SZ		asz;
	ID		key;
	ER		er, e2;

	h = t->open(t->ctx, path, XFU_O_READ);
	if ( h < 0 ) {
		return (ER)h;
	}
	got = fill(t, h, x->chunk, XFU_CHUNK, &eof);
	if ( got < 0 ) {
		(void)t->close(t->ctx, h);
		return (ER)got;
	}
	obj_name(file, ext_at(file), stem);
	if ( stem[0] == 0 ) obj_name(file, xfu_slen(file), stem);
	kind = kind_of(file, x->chunk, got, &type);
	kind = kind_as(as, kind, &type);
	x->zen = (BOOL)( as == XFU_AS_ZEN );
	if ( kind == K_TAD ) {
		er = take_tad(x, h, got, &eof, file, stem, mtime, p_uuid);
		e2 = t->close(t->ctx, h);
		return ( er >= E_OK ) ? e2 : er;
	}

	xfu_mset(&f, 0, sizeof(f));
	f.name = file;
	f.type = type;
	f.size = 0;
	f.mtime = mtime;
	f.enc = TXC_AUTO;
	er = make_obj(x, stem, ( kind == K_TEXT ) ? XFU_APP_TEXT : ( kind == K_IMAGE ) ? XFU_APP_FIG
			       : ( kind == K_ARCHIVE ) ? XFU_APP_UNPACK : XFU_APP_NONE, kind, &f, 2, p_uuid);
	if ( er < E_OK ) {
		(void)t->close(t->ctx, h);
		return er;
	}
	key = ob_opn_obj(p_uuid, IMP_OPS);
	er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &rec) : (ER)key;
	if ( er >= E_OK ) er = ob_apd_rec(key, kind_rt(kind), 0, &rec);

	/* the bytes, unchanged, into record 1; a text's and a picture's kept as well */
	xb_init(&keep);
	keeping = (BOOL)( kind == K_TEXT || kind == K_IMAGE );
	while ( er >= E_OK && got > 0 ) {
		if ( keeping ) {
			SZ	lim = ( kind == K_TEXT ) ? XFU_TEXT_MAX : XFU_IMG_MAX;
			SZ	k = ( keep.at + got <= lim ) ? (SZ)got : lim - keep.at;

			if ( kind == K_IMAGE && keep.at + got > lim ) {
				keeping = FALSE;	/* too large to show: a file like any other */
				kind = K_BINARY;
				xb_free(&keep);
			} else {
				xb_bytes(&keep, x->chunk, k);
			}
		}
		asz = 0;
		er = ob_wri_rec(key, 1, total, x->chunk, got, &asz);
		crc = xfu_crc32(crc, x->chunk, got);
		total += got;
		xfu_tick(x, total);
		if ( x->stop ) er = E_ABORT;
		if ( er >= E_OK ) got = fill(t, h, x->chunk, XFU_CHUNK, &eof);
		if ( got < 0 ) er = (ER)got;
	}
	e2 = t->close(t->ctx, h);
	if ( er >= E_OK ) er = e2;
	if ( er >= E_OK && keep.bad ) er = E_NOMEM;

	/* record 0 */
	xb_init(&r0);
	if ( er >= E_OK ) {
		if ( kind == K_TEXT ) {
			enc = ( x->opt != NULL && x->opt->enc != TXC_AUTO ) ? x->opt->enc
			    : txc_detect(keep.b, keep.at);
			crlf = rec0_text(x, &r0, stem, keep.b, keep.at, enc);
		} else if ( kind == K_IMAGE && rec0_image(key, p_uuid, &r0, stem, keep.b, keep.at,
								   ( x->opt != NULL ) ? x->opt->shrink : 0) >= E_OK ) {
			;
		} else {
			r0.at = 0;
			rec0_name(&r0, stem, file);
		}
		er = r0.bad ? E_NOMEM : put_rec(key, 0, r0.b, r0.at);
	}
	if ( er >= E_OK ) {
		XBUF	fj;

		f.size = total;
		f.crc = crc;
		f.hascrc = TRUE;
		f.enc = enc;
		f.crlf = crlf;
		if ( kind == K_TEXT ) {
			f.rec0 = xfu_crc32(0, r0.b, r0.at);
			f.hasrec0 = TRUE;
		}
		xb_init(&fj);
		put_file(x, &fj, &f);
		er = fj.bad ? E_NOMEM : xfu_meta_file(key, fj.b, fj.at);
		xb_free(&fj);
	}
	xb_free(&r0);
	xb_free(&keep);
	if ( key > 0 ) ob_cls_obj(key);
	if ( er < E_OK ) {
		(void)ob_del_obj(p_uuid);		/* nothing half taken in is kept */
		xfu_unmade(x, p_uuid);
		x->st.objects--;
		return er;
	}
	x->st.files++;
	return E_OK;
}

/* ---------------------------------------------------------------- a directory's entries */

typedef struct {
	INT	name;			/* where its name is in the pool */
	UINT	kind;
	D	size, mtime;
	INT	set;			/* the TADjs set it is a file of, -1 none */
	INT	part;			/* what of it: P_* */
	INT	pos;			/* a record's position */
} DENT;

#define P_NONE	0
#define P_JSON	1
#define P_ICO	2
#define P_XTAD	3
#define P_BIN	4
#define P_RES	5

typedef struct {
	DENT	*e;
	INT	n, max;
	XBUF	pool;
	BOOL	bad;
} DLIST;

LOCAL INT dl_put( void *arg, CONST T_XFUENT *en )
{
	DLIST	*d = (DLIST *)arg;
	DENT	*ne;

	if ( d->n == d->max ) {
		INT	cap = ( d->max == 0 ) ? 64 : d->max * 2;

		ne = (DENT *)xfu_sys_alloc(sizeof(DENT) * cap);
		if ( ne == NULL ) {
			d->bad = TRUE;
			return -1;
		}
		if ( d->e != NULL ) {
			xfu_mcpy(ne, d->e, sizeof(DENT) * d->n);
			xfu_sys_free(d->e);
		}
		d->e = ne;
		d->max = cap;
	}
	ne = &d->e[d->n++];
	ne->name = (INT)d->pool.at;
	ne->kind = en->kind;
	ne->size = en->size;
	ne->mtime = en->mtime;
	ne->set = -1;
	ne->part = P_NONE;
	ne->pos = 0;
	xb_bytes(&d->pool, en->name, xfu_slen(en->name) + 1);
	return d->pool.bad ? -1 : 0;
}

#define DNAME(d, i)	( (d)->pool.b + (d)->e[i].name )

LOCAL INT name_cmp( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (INT)a[i] - (INT)b[i];
}

/* The entries in the order of their names, so that a tree comes in the same way twice */
LOCAL void dl_sort( DLIST *d )
{
	INT	gap, i, j;
	DENT	t;

	for ( gap = d->n / 2; gap > 0; gap /= 2 ) {
		for ( i = gap; i < d->n; i++ ) {
			t = d->e[i];
			for ( j = i; j >= gap && name_cmp(d->pool.b + d->e[j - gap].name,
							  d->pool.b + t.name) > 0; j -= gap ) {
				d->e[j] = d->e[j - gap];
			}
			d->e[j] = t;
		}
	}
}

LOCAL void dl_free( DLIST *d )
{
	if ( d->e != NULL ) xfu_sys_free(d->e);
	xb_free(&d->pool);
}

/* ---------------------------------------------------------------- TADjs objects */

#define SET_REC		16		/* record types remembered of a set */

typedef struct {
	TS_UUID	u;
	INT	json;			/* its entry */
	BOOL	made;			/* made now, not there before */
	BOOL	linked;			/* another of the sets links to it */
	UB	rt[SET_REC];		/* the type of each byte record, 0 when not said */
} TSET;

/* What a name is of a TADjs object: its part, the UUID, the position */
LOCAL INT tadjs_part( CONST UB *s, TS_UUID *u, INT *p_pos )
{
	INT	i, n = xfu_slen(s), pos = 0, k;

	if ( n < 40 || !xfu_str_uuid(s, u) ) {
		return P_NONE;
	}
	s += 36;
	if ( xfu_same(s, ".json") ) return P_JSON;
	if ( xfu_same(s, ".ico") ) return P_ICO;
	if ( s[0] != '_' || s[1] < '0' || s[1] > '9' ) return P_NONE;
	for ( i = 1; s[i] >= '0' && s[i] <= '9'; i++ ) pos = pos * 10 + ( s[i] - '0' );
	*p_pos = pos;
	if ( xfu_same(s + i, ".xtad") ) return P_XTAD;
	if ( xfu_same(s + i, ".bin") ) return P_BIN;
	if ( s[i] == '_' && s[i + 1] != 0 ) {
		for ( k = i + 1; s[k] != 0; k++ ) ;
		return ( k - i < OB_RES_NAME - 4 ) ? P_RES : P_NONE;
	}
	return P_NONE;
}

/* A file of the tree read whole (at most max bytes) into w */
LOCAL ER read_all( XFU *x, CONST UB *path, XBUF *w, SZ max )
{
	CONST T_XFUTREE	*t = x->t;
	INT		h, got;
	ER		er = E_OK, e2;

	h = t->open(t->ctx, path, XFU_O_READ);
	if ( h < 0 ) {
		return (ER)h;
	}
	while ( ( got = t->read(t->ctx, h, x->chunk, XFU_CHUNK) ) > 0 ) {
		xb_bytes(w, x->chunk, got);
		if ( w->at > max ) {
			er = E_LIMIT;
			break;
		}
	}
	if ( got < 0 ) er = (ER)got;
	e2 = t->close(t->ctx, h);
	if ( er >= E_OK && w->bad ) er = E_NOMEM;
	return ( er >= E_OK ) ? e2 : er;
}

/* The path of an entry of the directory at work, in x->path after its own */
LOCAL ER sub_path( XFU *x, INT plen, CONST UB *name )
{
	x->path[plen] = 0;
	if ( plen > 0 ) {
		if ( plen + 1 >= XFU_PATH_MAX ) return E_LIMIT;
		x->path[plen] = '/';
		x->path[plen + 1] = 0;
		return ( xfu_scpy(x->path + plen + 1, XFU_PATH_MAX - plen - 1, name) == xfu_slen(name) )
		       ? E_OK : E_LIMIT;
	}
	return ( xfu_scpy(x->path, XFU_PATH_MAX, name) == xfu_slen(name) ) ? E_OK : E_LIMIT;
}

/*
 * The metadata a TADjs object brings, made ready: the count set to
 * nought (each link is counted as it is written), and "tessronos.file"
 * saying where it came from, put into its "tessronos" or into a new one.
 */
LOCAL ER tadjs_meta( XFU *x, XBUF *in, CONST UB *jsonname, D mtime, XBUF *out, TSET *s )
{
	T_JSON		root, tf, rc, recs, it, v;
	FILEFACT	f;
	XBUF		fb;
	INT		at, i, from;
	BOOL		has_tf, empty;
	CONST UB	*j = in->b;

	if ( js_parse(j, (INT)in->at, &root) < E_OK || js_type(&root) != JS_OBJECT ) {
		return E_PAR;
	}
	/* the types the byte records are said to have */
	xfu_mset(s->rt, 0, sizeof(s->rt));
	if ( js_get(&root, "records", &recs) >= E_OK ) {
		for ( it.s = NULL, i = 0; i < SET_REC && js_next(&recs, &it); i++ ) {
			if ( js_get(&it, "rt", &v) >= E_OK ) s->rt[i] = (UB)js_get_num(&it, "rt", 0);
		}
	}
	if ( js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "records", &recs) >= E_OK ) {
		for ( it.s = NULL; js_next(&recs, &it); ) {
			D	n = js_get_num(&it, "n", -1), rt = js_get_num(&it, "rt", 0);

			if ( n >= 0 && n < SET_REC && rt > 0 ) s->rt[n] = (UB)rt;
		}
	}

	xfu_mset(&f, 0, sizeof(f));
	f.name = jsonname;
	f.type = XFU_MT_TADJS;
	f.size = (D)in->at;
	f.mtime = mtime;
	f.enc = TXC_AUTO;
	xb_init(&fb);
	put_file(x, &fb, &f);

	has_tf = (BOOL)( js_get(&root, "tessronos", &tf) >= E_OK && js_type(&tf) == JS_OBJECT );
	if ( has_tf ) {
		at = (INT)( tf.s - j ) + 1;
		for ( i = 1; i < tf.len - 1 && ( tf.s[i] == ' ' || tf.s[i] == '\n'
						|| tf.s[i] == '\r' || tf.s[i] == '\t' ); i++ ) ;
		empty = (BOOL)( tf.s[i] == '}' );
	} else {
		at = (INT)( root.s - j ) + root.len - 1;
		for ( i = root.len - 2; i > 0 && ( root.s[i] == ' ' || root.s[i] == '\n'
						|| root.s[i] == '\r' || root.s[i] == '\t' ); i-- ) ;
		empty = (BOOL)( root.s[i] == '{' );
	}
	/* up to where the file goes in, with refCount made nought on the way */
	from = 0;
	if ( js_get(&root, "refCount", &rc) >= E_OK && (INT)( rc.s - j ) < at ) {
		xb_bytes(out, j, rc.s - j);
		xb_put(out, "0");
		from = (INT)( rc.s - j ) + rc.len;
	}
	xb_bytes(out, j + from, at - from);
	if ( has_tf ) {
		xb_put(out, "\"file\":");
		xb_bytes(out, fb.b, fb.at);
		if ( !empty ) xb_put(out, ",");
	} else {
		if ( !empty ) xb_put(out, ",");
		xb_put(out, "\"tessronos\":{\"file\":");
		xb_bytes(out, fb.b, fb.at);
		xb_put(out, "}");
	}
	from = at;
	if ( js_get(&root, "refCount", &rc) >= E_OK && (INT)( rc.s - j ) >= at ) {
		xb_bytes(out, j + from, ( rc.s - j ) - from);
		xb_put(out, "0");
		from = (INT)( rc.s - j ) + rc.len;
	}
	xb_bytes(out, j + from, in->at - from);
	xb_free(&fb);
	return out->bad ? E_NOMEM : ( out->at > OB_ATR_MAX ) ? E_LIMIT : E_OK;
}

/* Each link of the sets' records marks the set it reaches as linked to */
typedef struct {
	TSET	*s;
	INT	n;
	TS_UUID	*t;			/* the targets met, to be counted */
	INT	nt, maxt;
} LNKS;

LOCAL void set_linked( void *arg, CONST TS_UUID *u )
{
	LNKS	*l = (LNKS *)arg;
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( xfu_uuid_eq(&l->s[i].u, u) ) l->s[i].linked = TRUE;
	}
	if ( l->t != NULL && l->nt < l->maxt ) {
		l->t[l->nt++] = *u;
	}
}

/* The entry of a set's part, or -1 */
LOCAL INT set_entry( DLIST *d, INT set, INT part, INT pos )
{
	INT	i;

	for ( i = 0; i < d->n; i++ ) {
		if ( d->e[i].set == set && d->e[i].part == part && d->e[i].pos == pos ) return i;
	}
	return -1;
}

/*
 * The TADjs objects of a directory taken in: first every one made,
 * with its metadata and icon, then their records and resources, so
 * that the links between them are written to objects that are there;
 * last, on a store that does not count, each link counted.
 */
LOCAL ER imp_sets( XFU *x, INT plen, DLIST *d, TSET *s, INT ns )
{
	XBUF	in, meta;
	T_OBCRE	c;
	LNKS	l;
	INT	i, k, e, pos, recno;
	T_OBREF	r;
	ID	key;
	ER	er;

	/* every object made */
	for ( i = 0; i < ns; i++ ) {
		if ( ob_ref_obj(&s[i].u, &r) >= E_OK ) {
			continue;			/* taken in before: left as it is */
		}
		xb_init(&in);
		xb_init(&meta);
		er = sub_path(x, plen, DNAME(d, s[i].json));
		if ( er >= E_OK ) er = read_all(x, x->path, &in, OB_ATR_MAX);
		if ( er >= E_OK ) er = tadjs_meta(x, &in, DNAME(d, s[i].json), d->e[s[i].json].mtime,
						  &meta, &s[i]);
		xb_free(&in);
		if ( er >= E_OK ) {
			xfu_mset(&c, 0, sizeof(c));
			c.type = OB_T_STORAGE;
			c.sub = OB_S_FILE;
			c.json = meta.b;
			c.jsonsz = meta.at;
			c.prt = ( x->opt != NULL ) ? x->opt->prt : NULL;
			if ( x->opt != NULL && x->opt->vol != NULL ) {
				c.vol = x->opt->vol;
			} else if ( x->hasnear ) {
				c.near = x->near;
			}
			c.uuid = s[i].u;
			e = set_entry(d, i, P_ICO, 0);
			xb_init(&in);
			if ( e >= 0 && sub_path(x, plen, DNAME(d, e)) >= E_OK
			  && read_all(x, x->path, &in, OB_ICO_MAX) >= E_OK && in.at >= 8
			  && ( ( in.b[0] == 0 && in.b[1] == 0 && in.b[2] == 1 && in.b[3] == 0 )
			    || ( in.b[0] == 0x89 && in.b[1] == 'P' && in.b[2] == 'N' && in.b[3] == 'G' ) ) ) {
				c.icon = in.b;
				c.iconsz = in.at;
			}
			er = ob_cre_obj(&c, &s[i].u);
			xb_free(&in);
		}
		xb_free(&meta);
		if ( er >= E_OK ) {
			s[i].made = TRUE;
			x->st.objects++;
			xfu_made(x, &s[i].u);
		}
		xfu_note(x, DNAME(d, s[i].json), er);
	}

	/* their records and resources, the links in them noted */
	l.s = s;
	l.n = ns;
	l.nt = 0;
	l.maxt = XFU_OBJ_MAX;
	l.t = (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * l.maxt);
	if ( l.t == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < ns; i++ ) {
		if ( !s[i].made ) continue;
		key = ob_opn_obj(&s[i].u, IMP_OPS);
		er = ( key > 0 ) ? E_OK : (ER)key;
		for ( pos = 0; er >= E_OK; pos++ ) {
			BOOL	tad = TRUE;

			e = set_entry(d, i, P_XTAD, pos);
			if ( e < 0 ) {
				e = set_entry(d, i, P_BIN, pos);
				tad = FALSE;
			}
			if ( e < 0 ) break;
			xb_init(&in);
			er = sub_path(x, plen, DNAME(d, e));
			if ( er >= E_OK ) er = read_all(x, x->path, &in, XFU_TAD_MAX);
			if ( er >= E_OK ) {
				UINT	rt = tad ? OB_RT_TAD
					   : ( pos < SET_REC && s[i].rt[pos] != 0 ) ? s[i].rt[pos]
					   : OB_RT_SYSDATA;

				er = ob_apd_rec(key, rt, 0, &recno);
				if ( er >= E_OK ) er = put_rec(key, recno, in.b, in.at);
				if ( er >= E_OK && tad ) (void)xfu_links(in.b, in.at, set_linked, &l);
			}
			xb_free(&in);
			x->st.files++;
		}
		for ( k = 0; er >= E_OK && k < d->n; k++ ) {
			CONST UB	*nm = DNAME(d, k);

			if ( d->e[k].set != i || d->e[k].part != P_RES ) continue;
			xb_init(&in);
			er = sub_path(x, plen, nm);
			if ( er >= E_OK ) er = read_all(x, x->path, &in, XFU_IMG_MAX);
			if ( er >= E_OK ) er = ob_wri_res(key, nm + 36, in.b, in.at);
			xb_free(&in);
		}
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < E_OK ) {
			xfu_note(x, DNAME(d, s[i].json), er);
		}
	}

	/* each link counted where the store does not count them itself */
	for ( i = 0; i < l.nt; i++ ) {
		if ( ob_ref_obj(&l.t[i], &r) >= E_OK && !xfu_counts(&l.t[i]) ) {
			(void)ob_lnk_obj(&l.t[i]);
		}
	}
	xfu_sys_free(l.t);
	return E_OK;
}

/* ---------------------------------------------------------------- a directory */

LOCAL ER imp_dir( XFU *x, CONST UB *file, D mtime, INT level, TS_UUID *p_uuid );

/* An entry of a directory taken in */
LOCAL ER imp_entry( XFU *x, CONST UB *name, UINT kind, D mtime, INT level, TS_UUID *p_uuid )
{
	if ( kind == XFU_K_DIR ) {
		if ( level >= x->depth ) {
			return E_LIMIT;
		}
		return imp_dir(x, name, mtime, level + 1, p_uuid);
	}
	return imp_file(x, x->path, name, mtime, XFU_AS_AUTO, p_uuid);
}

#define DIR_ROWS	20		/* links in a column of a directory's figure */
#define DIR_W		256
#define DIR_H		33

LOCAL ER imp_dir( XFU *x, CONST UB *file, D mtime, INT level, TS_UUID *p_uuid )
{
	DLIST		d;
	TSET		*s = NULL;
	XBUF		kids, r0;
	FILEFACT	f;
	UB		stem[OB_NAME_MAX];
	INT		plen = xfu_slen(x->path), i, k, ns = 0, nk, pos, col, row;
	TS_UUID		u;
	T_OBREF		ref;
	ID		key;
	ER		er, ek;

	xfu_mset(&d, 0, sizeof(d));
	xb_init(&d.pool);
	er = x->t->list(x->t->ctx, x->path, dl_put, &d);
	if ( er >= E_OK && ( d.bad || d.pool.bad ) ) er = E_NOMEM;
	if ( er < E_OK ) {
		dl_free(&d);
		return er;
	}
	dl_sort(&d);
	x->st.dirs++;

	/* the TADjs objects here: each .json names one, its other files join it */
	for ( i = 0; i < d.n; i++ ) {
		if ( d.e[i].kind == XFU_K_FILE && tadjs_part(DNAME(&d, i), &u, &pos) == P_JSON ) ns++;
	}
	if ( ns > 0 ) {
		s = (TSET *)xfu_sys_alloc(sizeof(TSET) * ns);
		if ( s == NULL ) {
			dl_free(&d);
			return E_NOMEM;
		}
		xfu_mset(s, 0, sizeof(TSET) * ns);
		for ( i = 0, k = 0; i < d.n; i++ ) {
			if ( d.e[i].kind == XFU_K_FILE && tadjs_part(DNAME(&d, i), &u, &pos) == P_JSON ) {
				s[k].u = u;
				s[k].json = i;
				k++;
			}
		}
		for ( i = 0; i < d.n; i++ ) {
			INT	part;

			if ( d.e[i].kind != XFU_K_FILE ) continue;
			part = tadjs_part(DNAME(&d, i), &u, &pos);
			if ( part == P_NONE ) continue;
			for ( k = 0; k < ns && !xfu_uuid_eq(&s[k].u, &u); k++ ) ;
			if ( k < ns ) {
				d.e[i].set = k;
				d.e[i].part = part;
				d.e[i].pos = pos;
			}
		}
		er = imp_sets(x, plen, &d, s, ns);
	}

	/* what the directory holds, made before the figure that links to it */
	xb_init(&kids);
	for ( i = 0; i < ns; i++ ) {
		if ( !s[i].linked && ( s[i].made || ob_ref_obj(&s[i].u, &ref) >= E_OK ) ) {
			xb_bytes(&kids, &s[i].u, sizeof(TS_UUID));
		}
	}
	for ( i = 0; er >= E_OK && i < d.n; i++ ) {
		CONST UB	*nm = DNAME(&d, i);

		if ( x->stop ) {
			er = E_ABORT;
			break;
		}
		if ( d.e[i].set >= 0 || ( ns > 0 && xfu_same(nm, "tsfs-volume.json") ) ) {
			continue;
		}
		ek = sub_path(x, plen, nm);
		if ( ek >= E_OK ) ek = imp_entry(x, nm, d.e[i].kind, d.e[i].mtime, level, &u);
		x->path[plen] = 0;
		if ( ek >= E_OK ) {
			xb_bytes(&kids, &u, sizeof(TS_UUID));
		}
		xfu_note(x, nm, ek);
		if ( ek == E_NOMEM || ek == E_ABORT ) er = ek;
	}
	x->path[plen] = 0;
	if ( er >= E_OK && kids.bad ) er = E_NOMEM;
	nk = (INT)( kids.at / sizeof(TS_UUID) );

	/* the figure of links */
	xb_init(&r0);
	if ( er >= E_OK ) {
		obj_name(file, xfu_slen(file), stem);
		if ( stem[0] == 0 ) {
			stem[0] = '/';
			stem[1] = 0;
		}
		tad_head(&r0, stem, "figure");
		col = ( nk + DIR_ROWS - 1 ) / DIR_ROWS;
		row = ( nk < DIR_ROWS ) ? nk : DIR_ROWS;
		xb_put(&r0, "<figView top=\"0\" left=\"0\" right=\"");
		xb_num(&r0, col * DIR_W + 8);
		xb_put(&r0, "\" bottom=\"");
		xb_num(&r0, row * DIR_H + 8);
		xb_put(&r0, "\"/>\n<figDraw top=\"0\" left=\"0\" right=\"");
		xb_num(&r0, col * DIR_W + 8);
		xb_put(&r0, "\" bottom=\"");
		xb_num(&r0, row * DIR_H + 8);
		xb_put(&r0, "\"/>\n<figScale hunit=\"-96\" vunit=\"-96\"/>\n");
		for ( i = 0; i < nk; i++ ) {
			TS_UUID	t;
			INT	l = 8 + ( i / DIR_ROWS ) * DIR_W, tp = 8 + ( i % DIR_ROWS ) * DIR_H;

			xfu_mcpy(&t, kids.b + i * sizeof(TS_UUID), sizeof(TS_UUID));
			xfu_link_xml(&r0, &t, l, tp, l + DIR_W - 16, tp + 25, i + 1);
		}
		xb_put(&r0, "</figure>\n</tad>\n");
		if ( r0.bad ) er = E_NOMEM;
	}
	if ( er >= E_OK ) {
		xfu_mset(&f, 0, sizeof(f));
		f.name = ( file[0] != 0 ) ? file : (CONST UB *)"";
		f.type = XFU_MT_DIR;
		f.size = -1;
		f.mtime = mtime;
		f.enc = TXC_AUTO;
		er = make_obj(x, stem, XFU_APP_LIST, K_TAD, &f, 1, p_uuid);
	}
	if ( er >= E_OK ) {
		INT	recno = -1;

		key = ob_opn_obj(p_uuid, IMP_OPS);
		er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &recno) : (ER)key;
		if ( er >= E_OK ) er = put_rec(key, recno, r0.b, r0.at);
		if ( key > 0 ) ob_cls_obj(key);
		if ( er >= E_OK && !xfu_counts(p_uuid) ) {
			for ( i = 0; i < nk; i++ ) {
				TS_UUID	t;

				xfu_mcpy(&t, kids.b + i * sizeof(TS_UUID), sizeof(TS_UUID));
				(void)ob_lnk_obj(&t);
			}
		}
		if ( er < E_OK ) {
			(void)ob_del_obj(p_uuid);
			xfu_unmade(x, p_uuid);
			x->st.objects--;
		}
	}
	xb_free(&r0);
	xb_free(&kids);
	if ( s != NULL ) xfu_sys_free(s);
	dl_free(&d);
	return er;
}

/* ---------------------------------------------------------------- the call */

EXPORT ER xfu_import( CONST T_XFUTREE *t, CONST UB *path, CONST T_XFUOPT *opt,
		      CONST TS_UUID *box, TS_UUID *p_uuid )
{
	XFU	*x;
	INT	i, base = 0;
	ER	er;

	if ( t == NULL || p_uuid == NULL || t->open == NULL || t->read == NULL
	  || t->close == NULL || t->list == NULL || t->stat == NULL ) {
		return E_PAR;
	}
	x = (XFU *)xfu_sys_alloc(sizeof(XFU));
	if ( x == NULL ) {
		return E_NOMEM;
	}
	er = xfu_begin(x, t, opt);
	if ( er >= E_OK && box != NULL ) {
		x->near = *box;
		x->hasnear = TRUE;
	} else if ( er >= E_OK && opt != NULL && opt->near != NULL ) {
		x->near = *opt->near;
		x->hasnear = TRUE;
	}
	if ( er >= E_OK && xfu_scpy(x->path, XFU_PATH_MAX, ( path != NULL ) ? path : (CONST UB *)"")
			   != xfu_slen(path) ) {
		er = E_LIMIT;
	}
	if ( er >= E_OK ) {
		er = t->stat(t->ctx, x->path, &x->ent);
	}
	if ( er >= E_OK ) {
		UB	name[XFU_NAME_MAX];

		for ( i = 0; x->path[i] != 0; i++ ) {
			if ( x->path[i] == '/' && x->path[i + 1] != 0 ) base = i + 1;
		}
		(void)xfu_scpy(name, sizeof(name), x->path + base);
		i = xfu_slen(name);
		if ( i > 0 && name[i - 1] == '/' ) name[i - 1] = 0;
		if ( x->ent.kind == XFU_K_DIR ) {
			er = imp_dir(x, name, x->ent.mtime, 0, p_uuid);
		} else {
			er = imp_file(x, x->path, name, x->ent.mtime, ( opt != NULL ) ? opt->as : XFU_AS_AUTO,
				      p_uuid);
		}
	}
	if ( er >= E_OK && x->stop ) {
		er = E_ABORT;
	}
	if ( er >= E_OK && box != NULL ) {
		er = xfu_link_add(box, p_uuid);
	}
	if ( er < E_OK ) {
		xfu_undo(x);			/* nothing of it is kept */
	}
	xfu_finish(x);
	xfu_sys_free(x);
	return er;
}
