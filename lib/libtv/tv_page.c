/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_page.c
 *	A page of a program's window with tags as an object of its own: the
 *	tag pulled out, and このページの仮身を作る (include/ts/tvpage.h,
 *	design 10.6, 11.6)
 *
 *	The object is made only once it is known where its link goes: for
 *	a tag pulled out, when the desktop answers where the outline was let
 *	go, so that a press that was a click, or a carry let go where nothing
 *	takes links, leaves nothing behind. One the link of which could not
 *	be put is taken away again, as nothing would link to it.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/uapp.h>
#include <ts/ui.h>
#include <ts/dtreq.h>
#include <ts/vobjsz.h>
#include <ts/tvvop.h>
#include <ts/tvpage.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* How many bytes of s fit in room bytes, cut before a letter that does not fit whole */
static INT utf8_fit( CONST char *s, INT room )
{
	INT	n = (INT)strlen(s), k;

	if ( n <= room ) return n;
	for ( k = room; k > 0 && ( (UB)s[k] & 0xC0 ) == 0x80; k-- ) ;
	return k;
}

EXPORT INT tv_page_name( CONST T_TVPAGE *p, char *buf, INT max )
{
	INT	n;

	if ( max <= 0 ) return 0;
	n = snprintf(buf, (size_t)max, "%s：%s", p->prog, p->label);
	if ( n >= max ) {
		n = utf8_fit(buf, max - 1);
		buf[n] = 0;
	}
	return n;
}

/* s put after the n bytes in b of room max, each '"' and '\\' and control letter made harmless for JSON */
static INT put_js( char *b, INT n, INT max, CONST char *s )
{
	for ( ; *s != 0 && n + 2 < max; s++ ) {
		if ( *s == '"' || *s == '\\' ) b[n++] = '\\';
		b[n++] = ( (UB)*s < 0x20 ) ? ' ' : *s;
	}
	b[n] = 0;
	return n;
}

/* The same for the letters of an xmlTAD document */
static INT put_xml( char *b, INT n, INT max, CONST char *s )
{
	for ( ; *s != 0 && n + 6 < max; s++ ) {
		CONST char	*e = ( *s == '&' ) ? "&amp;" : ( *s == '<' ) ? "&lt;" : ( *s == '>' ) ? "&gt;" : NULL;

		if ( e != NULL ) {
			memcpy(b + n, e, strlen(e));
			n += (INT)strlen(e);
		} else {
			b[n++] = *s;
		}
	}
	b[n] = 0;
	return n;
}

static INT put_s( char *b, INT n, INT max, CONST char *s )
{
	INT	l = (INT)strlen(s);

	if ( n + l >= max ) l = utf8_fit(s, max - 1 - n);
	memcpy(b + n, s, (size_t)l);
	n += l;
	b[n] = 0;
	return n;
}

#define PAGE_META_MAX	1024
#define PAGE_DOC_MAX	1024

EXPORT ER tv_page_make( CONST T_TVPAGE *p, CONST TS_UUID *near, TS_UUID *made )
{
	char	name[TV_PAGE_NAME_MAX], *meta, *doc, s[TV_PAGE_NAME_MAX + 64];
	INT	n, d, recno = -1;
	SZ	asz = 0;
	T_OBCRE	c;
	ID	k;
	ER	er;

	if ( p == NULL || p->key == NULL || p->prog == NULL || p->page == NULL || p->label == NULL ) return E_PAR;
	(void)tv_page_name(p, name, (INT)sizeof(name));
	meta = (char *)malloc(PAGE_META_MAX + PAGE_DOC_MAX);
	if ( meta == NULL ) return E_NOMEM;
	doc = meta + PAGE_META_MAX;

	/* the metadata: the name, and the program that opens it on the page (固有データ) */
	n = put_s(meta, 0, PAGE_META_MAX, "{\"name\":\"");
	n = put_js(meta, n, PAGE_META_MAX, name);
	n = put_s(meta, n, PAGE_META_MAX, "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		  "\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{\"");
	n = put_js(meta, n, PAGE_META_MAX, p->key);
	n = put_s(meta, n, PAGE_META_MAX, "\":{\"name\":\"");
	n = put_js(meta, n, PAGE_META_MAX, p->prog);
	n = put_s(meta, n, PAGE_META_MAX, "\",\"defaultOpen\":true,\"data\":\"");
	n = put_js(meta, n, PAGE_META_MAX, p->page);
	n = put_s(meta, n, PAGE_META_MAX, "\"}},\"tessronos\":{\"page\":{\"program\":\"");
	n = put_js(meta, n, PAGE_META_MAX, p->key);
	n = put_s(meta, n, PAGE_META_MAX, "\",\"page\":\"");
	n = put_js(meta, n, PAGE_META_MAX, p->page);
	n = put_s(meta, n, PAGE_META_MAX, "\"}}}");
	if ( n >= PAGE_META_MAX - 1 ) {
		free(meta);
		return E_LIMIT;
	}

	/* record 0: what the object is, for whatever shows it as a document */
	d = put_s(doc, 0, PAGE_DOC_MAX, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>");
	d = put_xml(doc, d, PAGE_DOC_MAX, name);
	d = put_s(doc, d, PAGE_DOC_MAX, "</p>\n<p>開くと、");
	d = put_xml(doc, d, PAGE_DOC_MAX, p->prog);
	d = put_s(doc, d, PAGE_DOC_MAX, "の〈");
	d = put_xml(doc, d, PAGE_DOC_MAX, p->label);
	d = put_s(doc, d, PAGE_DOC_MAX, "〉のページを開きます。</p>\n</document>\n</tad>\n");

	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)meta;
	c.jsonsz = (SZ)n;
	if ( near != NULL ) c.near = *near;
	er = ob_cre_obj(&c, made);
	if ( er >= E_OK ) {
		k = ob_opn_obj(made, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* written before anyone reads it */
		er = ( k > 0 ) ? ob_apd_rec(k, OB_RT_TAD, 0, &recno) : ( k < 0 ) ? (ER)k : E_IO;
		if ( er >= E_OK ) er = ob_wri_rec(k, recno, 0, doc, (SZ)d, &asz);
		if ( k > 0 ) ob_cls_obj(k);
		if ( er < E_OK ) (void)ob_del_obj(made);
	}
	snprintf(s, sizeof(s), "tvpage: object \"%s\" of page \"%s\" made (%d)\n", name, p->page, (INT)er);
	tm_putstring((CONST UB *)s);
	free(meta);
	return er;
}

EXPORT ER tv_page_put( CONST T_TVASK *a, CONST T_TVPAGE *p )
{
	char	name[TV_PAGE_NAME_MAX];
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	u;
	ER	er;

	er = tv_page_make(p, NULL, &u);
	if ( er < E_OK ) return er;
	(void)tv_page_name(p, name, (INT)sizeof(name));
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_LINK_PUT;
	rq.target = u;
	rq.recno = -1;
	(void)a->part(a->arg, DT_TAIL_NAME, (CONST UB *)name, -1);
	memset(&an, 0, sizeof(an));
	er = a->ask(a->arg, &rq, &an, 10000);
	if ( er < E_OK ) (void)ob_del_obj(&u);	/* nothing links to it */
	return er;
}

EXPORT ER tv_page_pull( CONST T_TVASK *a, UI *u, CONST T_OBNTM *m, CONST T_TVPAGE *p )
{
	char	name[TV_PAGE_NAME_MAX], s[TV_PAGE_NAME_MAX + 96];
	UI_PULL	pl;
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	prog, obj;
	UINT	seq = 0;
	ER	er;

	memset(&obj, 0, sizeof(obj));
	if ( p == NULL || ui_tag_pull(u, m, &pl) < E_OK || ui_uuid(p->progobj, &prog) < E_OK ) return E_NOEXS;
	(void)tv_page_name(p, name, (INT)sizeof(name));
	snprintf(s, sizeof(s), "tvpage: tag %d pulled out, page \"%s\", outline %dx%d\n",
		 pl.tag, p->page, (INT)( pl.right - pl.left ), (INT)( pl.bottom - pl.top ));
	tm_putstring((CONST UB *)s);

	/*
	 * the outline carried stands for the program's object: what the
	 * window let go over may take is asked of that, as the object of the
	 * page is not made yet
	 */
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.target = prog;
	rq.recno = -1;
	rq.flags = DT_F_TO_PROG;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	rq.look.w = pl.right - pl.left;
	rq.look.h = pl.bottom - pl.top;
	rq.grab_x = pl.x - pl.left;
	rq.grab_y = pl.y - pl.top;
	(void)a->part(a->arg, DT_TAIL_NAME, (CONST UB *)name, -1);
	memset(&an, 0, sizeof(an));
	er = a->ask(a->arg, &rq, &an, 30 * 60 * 1000);
	if ( er >= E_OK ) {
		static CONST TS_UUID	none;

		seq = rq.seq;
		/*
		 * made on the volume of what the window it was let go over shows;
		 * not made, nothing is put, and the desktop forgets the carry
		 * when the next one begins
		 */
		er = tv_page_make(p, ( memcmp(&an.into, &none, sizeof(none)) == 0 ) ? NULL : &an.into, &obj);
	}
	if ( er >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PLACE;
		rq.target = obj;
		rq.recno = -1;
		rq.carry = seq;
		rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
		rq.look.w = VOBJ_CLOSED_EDGE_W(VOBJ_CHSZ_DEF) + dp_text_width((CONST UB *)name, VOBJ_CHSZ_DEF);
		rq.look.h = VOBJ_CLOSED_H(VOBJ_CHSZ_DEF, TRUE, TRUE);
		(void)a->part(a->arg, DT_TAIL_NAME, (CONST UB *)name, -1);
		memset(&an, 0, sizeof(an));
		er = a->ask(a->arg, &rq, &an, 10 * 60 * 1000);
		if ( er < E_OK ) (void)ob_del_obj(&obj);	/* nothing links to it */
	}
	snprintf(s, sizeof(s), "tvpage: link of page \"%s\" carried, named \"%s\" (%d)\n", p->page, name, (INT)er);
	tm_putstring((CONST UB *)s);
	return er;
}
