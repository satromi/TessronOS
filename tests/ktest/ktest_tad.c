/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tad.c
 *	The xmlTAD library (design 16.3.2, 16.3.3).
 *
 *	Nothing here touches a volume: a document is a run of bytes, and
 *	the library that reads it is on the far side of the file system
 *	from the store. What the links owe the reference counts is checked
 *	in ktest_om.
 *
 *	The test task has a small stack, so every document buffer and every
 *	T_VOBJ is taken from the heap.
 */

#include "ktest.h"
#include <ts/tad.h>

/* A cabinet: a figure holding one virtual object */
/* A link carrying attributes this library has no field for */
LOCAL CONST char unknown_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
"<figure>\n"
"<link id=\"019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad\""
" vobjid=\"019cf904-4412-7244-a1c2-10d9863b6120\" vobjleft=\"4\" vobjtop=\"50\""
" vobjright=\"174\" vobjbottom=\"81\" height=\"31\" chsz=\"14\""
" zIndex=\"7\" viewMode=\"detailed\" wordWrap=\"true\" background=\"false\"/>\n"
"</figure>\n"
"</tad>\n";

LOCAL CONST char fig_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"cabinet\">\n"
"<figure>\n"
"<figView top=\"0\" left=\"0\" right=\"400\" bottom=\"200\"/>\n"
"<figScale hunit=\"-72\" vunit=\"-72\"/>\n"
"<link id=\"019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad\""
" vobjid=\"019cf904-4412-7244-a1c2-10d9863b6120\" vobjleft=\"4\" vobjtop=\"50\""
" vobjright=\"174\" vobjbottom=\"81\" height=\"31\" chsz=\"14\""
" frcol=\"#000000\" chcol=\"#3f4345\" tbcol=\"#e1f2f9\" bgcol=\"#ffffff\""
" dlen=\"0\" pictdisp=\"true\" namedisp=\"true\" roledisp=\"false\""
" typedisp=\"false\" updatedisp=\"false\" framedisp=\"true\" autoopen=\"true\""
" fixed=\"true\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\""
" applist=\"{&quot;basic-text-editor&quot;:{&quot;name&quot;:&quot;kihon&quot;}}\"/>\n"
"</figure>\n"
"</tad>\n";

/* A text document: nested elements, a reference, and a link with a label */
LOCAL CONST char txt_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
"<document>\n"
"<p>BTRON &amp; TAD<br/>\n"
"<font size=\"14\"><bold>futoji</bold></font>\n"
"<link id=\"019a6c9b-e67e-7a35-a461-0d199550e4cf\""
" vobjid=\"019cf904-4417-7579-8562-bea3e6add6b3\" width=\"150\""
" heightpx=\"30\" chsz=\"14\" zoomratio=\"1.5\" roledisp=\"true\""
" relationship=\"gijiroku shiryou\">hyoujimei</link>\n"
"</p>\n"
"</document>\n"
"</tad>\n";

/* A declaration, a comment, and whitespace that has to come back as it was */
LOCAL CONST char decl_doc[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!-- what this document is for -->\n"
"<tad version=\"02.00\" encoding=\"utf-8\">\n"
"<document>\n"
"<p>  two spaces  kept  </p>\n"
"</document>\n"
"</tad>\n";

LOCAL CONST char *round_trip[] = { fig_doc, txt_doc, decl_doc };

/* What has to be refused, and with which error */
typedef struct {
	CONST char	*xml;
	ER		expect;
	CONST char	*what;
} BADDOC;

LOCAL CONST BADDOC bad_doc[] = {
	{ "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n",
	  E_PAR, "an element that is never closed" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document></p></tad>",
	  E_PAR, "an end tag that does not match" },
	{ "<!DOCTYPE tad><tad version=\"1.0\" encoding=\"UTF-8\"><document/></tad>",
	  E_PAR, "a document type declaration" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
	  "<![CDATA[raw]]></document></tad>",
	  E_PAR, "a CDATA section" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document>a&nbsp;b</document></tad>",
	  E_PAR, "an entity that is not defined" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p a=\"x<y\"/></document></tad>",
	  E_PAR, "'<' inside an attribute value" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p a=\"1\" a=\"2\"/></document></tad>",
	  E_PAR, "the same attribute twice" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p a=1/></document></tad>",
	  E_PAR, "an attribute value in no quotes" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document/></tad>"
	  "<tad version=\"1.0\" encoding=\"UTF-8\"><document/></tad>",
	  E_PAR, "a second root element" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document/></tad>trailing",
	  E_PAR, "text outside the root element" },
	{ "<xtad version=\"1.0\" encoding=\"UTF-8\"><document/></xtad>",
	  E_PAR, "a root element that is not <tad>" },
	{ "<tad version=\"1.0\"><document/></tad>",
	  E_PAR, "a root element that does not say its encoding" },
	{ "<tad version=\"1.0\" encoding=\"Shift_JIS\"><document/></tad>",
	  E_PAR, "an encoding that is not UTF-8" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document></document ></tad>",
	  E_PAR, "whitespace inside an end tag" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document>\001</document></tad>",
	  E_PAR, "a control character" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document>\303(</document></tad>",
	  E_PAR, "bytes that are not UTF-8" },
	{ "\357\273\277<tad version=\"1.0\" encoding=\"UTF-8\"><document/></tad>",
	  E_PAR, "a byte order mark" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><?pi here?><document/></tad>",
	  E_PAR, "a processing instruction after the head" },
	{ "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>&#x110000;</p></document></tad>",
	  E_PAR, "a character reference past U+10FFFF" },
};

#define NBAD	(INT)(sizeof(bad_doc) / sizeof(bad_doc[0]))

LOCAL CONST char lnk1_id[] = "019a6c96-e262-7dfd-a3bc-1e85d495d60d";
LOCAL CONST char add_id[]  = "019a4370-3819-79a2-9758-881e1e0de90a";

/* ---------------------------------------------------------------- */

LOCAL SZ s_len( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != '\0' ) n++;

	return n;
}

LOCAL void s_cpy( UB *dst, CONST char *src )
{
	while ( *src != '\0' ) *dst++ = (UB)*src++;
	*dst = '\0';
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
{
	SZ	i = 0;

	if ( a == NULL ) {
		return FALSE;
	}
	while ( b[i] != '\0' ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
		i++;
	}

	return ( a[i] == '\0' );
}

/* -1 when the two runs are the same, else where they first differ */
LOCAL INT first_diff( CONST UB *a, CONST UB *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return (INT)i;
	}

	return -1;
}

/*
 * Write a document out and give back the bytes. The caller frees them.
 */
LOCAL UB *serialize( T_TAD *doc, SZ *p_len )
{
	UB	*buf;
	SZ	need = 0, got = 0;

	if ( tad_write_mem(doc, NULL, 0, &need) < E_OK || need == 0 ) {
		return NULL;
	}
	buf = (UB *)Kmalloc(need + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tad_write_mem(doc, buf, need, &got) < E_OK || got != need ) {
		Kfree(buf);
		return NULL;
	}
	buf[need] = '\0';
	*p_len = need;

	return buf;
}

/* ---------------------------------------------------------------- */

/* what is parsed and written again comes back byte for byte */
LOCAL void test_roundtrip( void )
{
	T_TAD	*doc;
	UB	*out;
	SZ	len, want;
	INT	i, d;

	for ( i = 0; i < 3; i++ ) {
		want = s_len(round_trip[i]);
		KT_ASSERT_ER(tad_parse((CONST UB *)round_trip[i], want, NULL, &doc), E_OK);
		if ( doc == NULL ) continue;

		out = serialize(doc, &len);
		KT_ASSERT(out != NULL);
		if ( out != NULL ) {
			KT_ASSERT_EQ((INT)len, (INT)want);
			if ( len == want ) {
				d = first_diff(out, (CONST UB *)round_trip[i], len);
				KT_ASSERT_EQ(d, -1);
				if ( d >= 0 ) {
					tm_printf((UB*)"  document %d differs at %d\n", i, d);
				}
			}
			Kfree(out);
		}
		tad_free(doc);
		doc = NULL;
	}
}

/* what is not xmlTAD is refused, and with the reason it was refused for */
LOCAL void test_malformed( void )
{
	T_TAD	*doc;
	INT	i;
	ER	er;

	for ( i = 0; i < NBAD; i++ ) {
		doc = NULL;
		er = tad_parse((CONST UB *)bad_doc[i].xml,
			       s_len(bad_doc[i].xml), NULL, &doc);
		KT_ASSERT_ER(er, bad_doc[i].expect);
		if ( er != bad_doc[i].expect ) {
			tm_printf((UB*)"  taken: %s\n", bad_doc[i].what);
		}
		KT_ASSERT(doc == NULL || er >= E_OK);
		if ( er >= E_OK ) {
			tad_free(doc);
		}
	}

	/* and nothing at all is not a document either */
	doc = NULL;
	KT_ASSERT_ER(tad_parse((CONST UB *)"", 0, NULL, &doc), E_PAR);
	KT_ASSERT_ER(tad_parse(NULL, 4, NULL, &doc), E_PAR);
}

/* the limits stop a document before it is believed */
LOCAL void test_limits( void )
{
	T_TADLIM	lim;
	T_TAD		*doc = NULL;
	CONST char	*deep = "<tad version=\"1.0\" encoding=\"UTF-8\">"
				"<document><p><font size=\"1\">x</font></p>"
				"</document></tad>";
	CONST char	*wide = "<tad version=\"1.0\" encoding=\"UTF-8\">"
				"<document><p a=\"0123456789\"/></document></tad>";

	lim.depth = 3; lim.nodes = 0; lim.attrs = 0; lim.attrlen = 0; lim.docsize = 0;
	KT_ASSERT_ER(tad_parse((CONST UB *)deep, s_len(deep), &lim, &doc), E_LIMIT);

	lim.depth = 4;
	KT_ASSERT_ER(tad_parse((CONST UB *)deep, s_len(deep), &lim, &doc), E_OK);
	tad_free(doc);
	doc = NULL;

	lim.depth = 0; lim.nodes = 4;
	KT_ASSERT_ER(tad_parse((CONST UB *)deep, s_len(deep), &lim, &doc), E_LIMIT);

	lim.nodes = 0; lim.attrlen = 4;
	KT_ASSERT_ER(tad_parse((CONST UB *)wide, s_len(wide), &lim, &doc), E_LIMIT);

	lim.attrlen = 0; lim.docsize = 16;
	KT_ASSERT_ER(tad_parse((CONST UB *)wide, s_len(wide), &lim, &doc), E_LIMIT);

	lim.docsize = 0; lim.attrs = 1;
	KT_ASSERT_ER(tad_parse((CONST UB *)fig_doc, s_len(fig_doc), &lim, &doc), E_LIMIT);
}

/* the links of a document, and what each one says */
LOCAL void test_links( void )
{
	T_TAD		*doc = NULL;
	T_VOBJ		*v;
	TS_UUID		want;
	T_TADNODE	*nd;

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;

	KT_ASSERT_ER(tad_parse((CONST UB *)fig_doc, s_len(fig_doc), NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_EQ(tad_lnk_count(doc), 1);
		KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);

		KT_ASSERT_ER(ts_str_to_uuid(lnk1_id, &want), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &want), 0);
		KT_ASSERT_EQ(v->recno, 0);
		KT_ASSERT_EQ(v->left, 4);
		KT_ASSERT_EQ(v->top, 50);
		KT_ASSERT_EQ(v->right, 174);
		KT_ASSERT_EQ(v->bottom, 81);
		KT_ASSERT_EQ(v->height, 31);
		KT_ASSERT_EQ(v->chsz, 14);
		KT_ASSERT_EQ((INT)v->tbcol, 0xe1f2f9);
		KT_ASSERT_EQ((INT)v->chcol, 0x3f4345);
		KT_ASSERT_EQ((INT)v->disp, TAD_D_NAME | TAD_D_FRAME | TAD_D_PICT);
		KT_ASSERT(v->autoopen);
		KT_ASSERT(v->fixed);
		KT_ASSERT_EQ(v->zoom, 100);
		/* the value came through with its references resolved */
		KT_ASSERT_EQ(v->applist[0], (UB)'{');
		KT_ASSERT_EQ(v->applist[1], (UB)'"');

		/* past the end is no error, it is nothing */
		KT_ASSERT_ER(tad_lnk_get(doc, 1, v), E_NOEXS);

		/* the tree can be walked without knowing what is in it */
		nd = tad_body(doc);
		KT_ASSERT(nd != NULL);
		KT_ASSERT(s_eq(nd->name, "figure"));
		KT_ASSERT(s_eq(tad_attr(tad_root(doc), "filename"), "cabinet"));
		tad_free(doc);
		doc = NULL;
	}

	/* a link whose name is its text, and a magnification that is not 1 */
	KT_ASSERT_ER(tad_parse((CONST UB *)txt_doc, s_len(txt_doc), NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_EQ(tad_lnk_count(doc), 1);
		KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
		KT_ASSERT(s_eq(v->name, "hyoujimei"));
		KT_ASSERT(s_eq(v->relationship, "gijiroku shiryou"));
		KT_ASSERT_EQ(v->width, 150);
		KT_ASSERT_EQ(v->heightpx, 30);
		KT_ASSERT_EQ(v->zoom, 150);
		KT_ASSERT_EQ((INT)(v->disp & TAD_D_ROLE), TAD_D_ROLE);
		KT_ASSERT_EQ((INT)v->frcol, (INT)TAD_COL_NONE);

		/* found by the identity of the link itself */
		KT_ASSERT_ER(ts_str_to_uuid("019cf904-4417-7579-8562-bea3e6add6b3",
					    &want), E_OK);
		KT_ASSERT_ER(tad_lnk_find(doc, &want, v), E_OK);
		KT_ASSERT_EQ(v->width, 150);
		tad_free(doc);
	}
	Kfree(v);
}

/*
 * A link put in and taken out again. What it owes the reference counts
 * is written down as it happens, and taking it out leaves the text as it
 * was before it went in.
 */
LOCAL void test_edit( void )
{
	T_TAD	*doc = NULL, *again = NULL;
	T_VOBJ	*v;
	TS_UUID	newid, target, got;
	UB	*out;
	SZ	len = 0, want = s_len(fig_doc);
	INT	delta = 0, d;

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;

	KT_ASSERT_ER(tad_parse((CONST UB *)fig_doc, want, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }

	/* what is to be drawn: the caller fills in what it wants shown */
	{
		INT i;

		for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;
	}
	KT_ASSERT_ER(ts_str_to_uuid(add_id, &target), E_OK);
	v->target = target;
	v->recno  = 0;
	v->left   = 4;
	v->top    = 90;
	v->right  = 174;
	v->bottom = 121;
	v->height = 31;
	v->chsz   = 14;
	v->disp   = TAD_D_DEFAULT;
	v->zoom   = 100;
	v->tbcol  = 0xe1f2f9;
	s_cpy(v->name, "atarashii");

	KT_ASSERT_ER(tad_lnk_add(doc, v, &newid), E_OK);
	KT_ASSERT_EQ(tad_lnk_count(doc), 2);

	/* the identity of the new link was made here and is in the text */
	KT_ASSERT_ER(tad_lnk_find(doc, &newid, v), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &target), 0);

	/* one reference is owed */
	KT_ASSERT_EQ(tad_lnk_ndelta(doc), 1);
	KT_ASSERT_ER(tad_lnk_delta(doc, 0, &got, &delta), E_OK);
	KT_ASSERT_EQ(delta, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &target), 0);

	/* what was written reads back as the same link */
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT(len > want);
		KT_ASSERT_ER(tad_parse(out, len, NULL, &again), E_OK);
		if ( again != NULL ) {
			KT_ASSERT_EQ(tad_lnk_count(again), 2);
			KT_ASSERT_ER(tad_lnk_find(again, &newid, v), E_OK);
			KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &target), 0);
			KT_ASSERT_EQ(v->left, 4);
			KT_ASSERT_EQ(v->top, 90);
			KT_ASSERT_EQ(v->height, 31);
			KT_ASSERT_EQ(v->zoom, 100);
			KT_ASSERT_EQ((INT)v->tbcol, 0xe1f2f9);
			KT_ASSERT(s_eq(v->name, "atarashii"));
			KT_ASSERT_EQ(v->recno, 0);
			tad_free(again);
		}
		Kfree(out);
	}

	/* the same identity may not go in twice */
	v->vobjid = newid;
	KT_ASSERT_ER(tad_lnk_add(doc, v, NULL), E_OBJ);

	/* taking it out again leaves the document as it was */
	KT_ASSERT_ER(tad_lnk_del(doc, &newid), E_OK);
	KT_ASSERT_EQ(tad_lnk_count(doc), 1);
	KT_ASSERT_EQ(tad_lnk_ndelta(doc), 2);
	KT_ASSERT_ER(tad_lnk_delta(doc, 1, &got, &delta), E_OK);
	KT_ASSERT_EQ(delta, -1);
	KT_ASSERT_EQ(ts_uuid_cmp(&got, &target), 0);

	out = serialize(doc, &len);
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT_EQ((INT)len, (INT)want);
		if ( len == want ) {
			d = first_diff(out, (CONST UB *)fig_doc, len);
			KT_ASSERT_EQ(d, -1);
		}
		Kfree(out);
	}

	/* one that is not there cannot be taken out */
	KT_ASSERT_ER(tad_lnk_del(doc, &newid), E_NOEXS);

	tad_lnk_clr_delta(doc);
	KT_ASSERT_EQ(tad_lnk_ndelta(doc), 0);

	tad_free(doc);
	Kfree(v);
}


/*
 * Changing a link keeps what this library does not model. The format
 * carries more than a T_VOBJ holds, and an attribute nobody here
 * understands still means something to whoever wrote it.
 */
LOCAL void test_set_keeps_unknown( void )
{
	T_TAD	*doc = NULL;
	T_VOBJ	*v;
	UB	*out;
	SZ	want = 0;
	INT	i;
	BOOL	saw_zindex = FALSE, saw_view = FALSE, saw_moved = FALSE;

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;

	KT_ASSERT_ER(tad_parse((CONST UB *)unknown_doc, s_len(unknown_doc),
			       NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }

	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;
	KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
	v->left = 300;			/* move it, so the element is rewritten */
	v->right = 470;
	KT_ASSERT_ER(tad_lnk_set(doc, v), E_OK);

	KT_ASSERT(tad_write_mem(doc, NULL, 0, &want) <= E_OK);
	out = (UB *)Kmalloc(want + 1);
	KT_ASSERT(out != NULL);
	if ( out == NULL ) { tad_free(doc); Kfree(v); return; }
	KT_ASSERT_ER(tad_write_mem(doc, out, want + 1, &want), E_OK);

	for ( i = 0; i + 8 < (INT)want; i++ ) {
		if ( out[i] == 'z' && out[i+1] == 'I' && out[i+2] == 'n' ) saw_zindex = TRUE;
		if ( out[i] == 'v' && out[i+1] == 'i' && out[i+2] == 'e'
		  && out[i+3] == 'w' && out[i+4] == 'M' ) saw_view = TRUE;
		if ( out[i] == '"' && out[i+1] == '3' && out[i+2] == '0'
		  && out[i+3] == '0' && out[i+4] == '"' ) saw_moved = TRUE;
	}
	KT_ASSERT(saw_zindex);		/* an attribute T_VOBJ has no field for */
	KT_ASSERT(saw_view);
	KT_ASSERT(saw_moved);		/* and the move did take */

	Kfree(out);
	tad_free(doc);
	Kfree(v);
}


LOCAL CONST char attrs_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
"<document>\n"
"<p><link id=\"019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad\""
" vobjid=\"019cf904-4412-7244-a1c2-10d9863b6120\" width=\"150\" heightPx=\"31\""
" viewmode=\"xml\" wordwrap=\"false\" vobjheight=\"90\" zindex=\"3\" dlen=\"5\"/></p>\n"
"</document>\n"
"</tad>\n";

/* How many times a stretch stands in what was written */
LOCAL INT count_of( CONST UB *s, SZ len, CONST char *w )
{
	SZ	i, k, n = s_len(w);
	INT	c = 0;

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) c++;
	}
	return c;
}

/*
 * The attributes of a link that the records spell two ways are read
 * either way and written back in place, once: heightPx, viewMode,
 * wordWrap, zIndex; vobjheight and dlen are kept as they are.
 */
LOCAL void test_link_attrs( void )
{
	T_TAD	*doc = NULL;
	T_VOBJ	*v;
	UB	*out;
	SZ	want = 0;

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	KT_ASSERT_ER(tad_parse((CONST UB *)attrs_doc, s_len(attrs_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }
	KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
	KT_ASSERT_EQ(v->heightpx, 31);
	KT_ASSERT(v->viewmode[0] == 'x' && v->viewmode[1] == 'm' && v->viewmode[2] == 'l' && v->viewmode[3] == 0);
	KT_ASSERT_EQ(v->wordwrap, TAD_WRAP_OFF);
	KT_ASSERT_EQ(v->vobjheight, 90);
	KT_ASSERT(v->has_zindex && v->zindex == 3);
	KT_ASSERT_EQ(v->dlen, 5);

	v->heightpx = 40;
	v->wordwrap = TAD_WRAP_ON;
	v->viewmode[0] = 'd'; v->viewmode[1] = 'e'; v->viewmode[2] = 't';
	v->viewmode[3] = 'a'; v->viewmode[4] = 'i'; v->viewmode[5] = 'l';
	v->viewmode[6] = 'e'; v->viewmode[7] = 'd'; v->viewmode[8] = 0;
	KT_ASSERT_ER(tad_lnk_set(doc, v), E_OK);
	KT_ASSERT(tad_write_mem(doc, NULL, 0, &want) <= E_OK);
	out = (UB *)Kmalloc(want + 1);
	if ( out != NULL && tad_write_mem(doc, out, want + 1, &want) >= E_OK ) {
		/* the spelling the record had, with the new value, and no second one */
		KT_ASSERT_EQ(count_of(out, want, "heightPx=\"40\""), 1);
		KT_ASSERT_EQ(count_of(out, want, "heightpx="), 0);
		KT_ASSERT_EQ(count_of(out, want, "viewmode=\"detailed\""), 1);
		KT_ASSERT_EQ(count_of(out, want, "viewMode="), 0);
		KT_ASSERT_EQ(count_of(out, want, "wordwrap=\"true\""), 1);
		KT_ASSERT_EQ(count_of(out, want, "wordWrap="), 0);
		KT_ASSERT_EQ(count_of(out, want, "zindex=\"3\""), 1);
		KT_ASSERT_EQ(count_of(out, want, "zIndex="), 0);
		KT_ASSERT_EQ(count_of(out, want, "vobjheight=\"90\""), 1);
		KT_ASSERT_EQ(count_of(out, want, "dlen=\"5\""), 1);
	}
	if ( out != NULL ) Kfree(out);
	tad_free(doc);
	Kfree(v);
}

LOCAL CONST char z_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\n"
"<figure>\n"
"<link id=\"019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad\" vobjid=\"019cf904-4412-7244-a1c2-10d9863b6121\""
" vobjleft=\"0\" vobjtop=\"0\" vobjright=\"10\" vobjbottom=\"10\" zIndex=\"1\"/>\n"
"<link id=\"019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad\" vobjid=\"019cf904-4412-7244-a1c2-10d9863b6122\""
" vobjleft=\"0\" vobjtop=\"0\" vobjright=\"10\" vobjbottom=\"10\" zIndex=\"2\"/>\n"
"</figure>\n"
"</tad>\n";

/*
 * Links that say their place in the drawing keep the order they were
 * put in: the numbers do not go with them and sort them straight back.
 */
LOCAL void test_reorder_z( void )
{
	T_TAD	*doc = NULL;
	TS_UUID	order[2];
	T_VOBJ	*v;

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) return;
	KT_ASSERT_ER(tad_parse((CONST UB *)z_doc, s_len(z_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }
	(void)ts_str_to_uuid("019cf904-4412-7244-a1c2-10d9863b6122", &order[0]);
	(void)ts_str_to_uuid("019cf904-4412-7244-a1c2-10d9863b6121", &order[1]);
	KT_ASSERT_ER(tad_lnk_reorder(doc, order, 2), E_OK);
	/* the one now first is drawn first: the smaller number */
	KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&v->vobjid, &order[0]), 0);
	KT_ASSERT(v->has_zindex && v->zindex == 1);
	KT_ASSERT_ER(tad_lnk_get(doc, 1, v), E_OK);
	KT_ASSERT(v->has_zindex && v->zindex == 2);
	tad_free(doc);
	Kfree(v);
}

/* ---------------------------------------------------------------- text */

LOCAL CONST char edit_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p><font size=\"20\">abcdef</font>xyz</p><p>second</p>"
"</document></tad>";

/* Whether the written document holds a stretch of text */
LOCAL BOOL holds( CONST UB *s, SZ len, CONST char *want )
{
	SZ	i, k, n = s_len(want);

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && s[i + k] == (UB)want[k]; k++ ) {
			;
		}
		if ( k == n ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* The first text node that is exactly this */
LOCAL T_TADNODE *text_node( T_TAD *doc, CONST char *t )
{
	T_TADNODE	*nd;

	for ( nd = tad_walk(doc, NULL); nd != NULL; nd = tad_walk(doc, nd) ) {
		if ( nd->kind == TAD_ND_TEXT && nd->text != NULL
		  && s_len((CONST char *)nd->text) == s_len(t)
		  && first_diff(nd->text, (CONST UB *)t, s_len(t)) < 0 ) {
			return nd;
		}
	}

	return NULL;
}

/*
 * What the text editor does to a record: letters put in and taken
 * out, a paragraph cut in two with the elements round the cut going on
 * round both halves, two paragraphs put together, a stretch wrapped.
 */
LOCAL void test_text( void )
{
	T_TAD		*doc = NULL;
	T_TADNODE	*t, *two, *p1, *p2, *copy;
	UB		*out;
	SZ		len = 0;

	KT_ASSERT_ER(tad_parse((CONST UB *)edit_doc, s_len(edit_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	t = text_node(doc, "abcdef");
	KT_ASSERT(t != NULL);
	if ( t == NULL ) { tad_free(doc); return; }
	p1 = t->parent->parent;

	/* letters in, and the markup they need made safe */
	KT_ASSERT_ER(tad_text_splice(doc, t, 3, 0, (CONST UB *)"<&>", 3), E_OK);
	KT_ASSERT_ER(tad_text_splice(doc, t, 3, 3, (CONST UB *)"12", 2), E_OK);
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL && holds(out, len, "<font size=\"20\">abc12def</font>"));
	if ( out != NULL ) Kfree(out);
	KT_ASSERT_ER(tad_text_splice(doc, t, 0, 0, (CONST UB *)"<&", 2), E_OK);
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL && holds(out, len, "&lt;&amp;abc12def"));
	if ( out != NULL ) Kfree(out);
	KT_ASSERT_ER(tad_text_splice(doc, t, 0, 2, NULL, 0), E_OK);

	/* a paragraph cut after "abc": the size goes on round both halves */
	two = tad_text_split(doc, t, 3);
	KT_ASSERT(two != NULL);
	copy = tad_split_before(doc, two, p1);
	KT_ASSERT(copy != NULL);
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL && holds(out, len,
		"<p><font size=\"20\">abc</font></p><p><font size=\"20\">12def</font>xyz</p><p>second</p>"));
	if ( out != NULL ) Kfree(out);

	/* the copy has attributes of its own */
	(void)tad_set_attr(doc, copy, "align", (CONST UB *)"right");
	KT_ASSERT(tad_attr(p1, "align") == NULL);

	/* and put together again */
	tad_join(p1, copy);
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL && holds(out, len,
		"<p><font size=\"20\">abc</font><font size=\"20\">12def</font>xyz</p><p>second</p>"));
	if ( out != NULL ) Kfree(out);

	/* a stretch wrapped, and a text node made in an empty place */
	t = text_node(doc, "xyz");
	KT_ASSERT(t != NULL);
	if ( t != NULL ) {
		KT_ASSERT(tad_wrap(doc, t, t, "bold") != NULL);
	}
	t = text_node(doc, "second");
	p2 = ( t != NULL ) ? t->parent : NULL;
	KT_ASSERT(p2 != NULL);
	if ( p2 != NULL ) {
		KT_ASSERT(tad_text_new(doc, p2, t, (CONST UB *)"NEW ", 4) != NULL);
		tad_node_remove(text_node(doc, "second"));
	}
	out = serialize(doc, &len);
	KT_ASSERT(out != NULL && holds(out, len, "<bold>xyz</bold></p><p>NEW </p>"));
	if ( out != NULL ) Kfree(out);

	/* what was written reads back */
	out = serialize(doc, &len);
	if ( out != NULL ) {
		T_TAD	*again = NULL;

		KT_ASSERT_ER(tad_parse(out, len, NULL, &again), E_OK);
		if ( again != NULL ) tad_free(again);
		Kfree(out);
	}
	tad_free(doc);
}

EXPORT void ktest_tad( void )
{
	KT_RUN(test_roundtrip);
	KT_RUN(test_malformed);
	KT_RUN(test_limits);
	KT_RUN(test_links);
	KT_RUN(test_edit);
	KT_RUN(test_set_keeps_unknown);
	KT_RUN(test_link_attrs);
	KT_RUN(test_reorder_z);
	KT_RUN(test_text);
}
