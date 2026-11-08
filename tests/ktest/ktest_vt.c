/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_vt.c
 *	縦書き and 原稿用紙 (design 17.20)
 *
 *	A text written downwards is laid out by the same walk as one written
 *	across, turned: these say where its places come out -- the first
 *	line at the right, each letter a square under the one before, the
 *	line after to the left -- that 縦中横 takes one square and 回転 the
 *	letters' own widths, that the rules of 禁則 hold down the line as
 *	they do across and that ぶら下げ hangs a full stop past the end, that
 *	the letters take their vertical forms, and that a document of squares
 *	(data/genko.xtad, put on /boot) puts each letter in a square of its grid. What the editor writes for
 *	縦書き, 縦中横 and 回転 is read back as it was written, and the
 *	record of a text that was not touched comes out byte for byte.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/tadview.h>
#include <ts/fs.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/fn.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include <ts/docmenu.h>
#include "../../application/desktop/desktop.h"

#define XT_GENKO	"/boot/genko.xtad"

#define INK		0x00000000U	/* what the letters are drawn in */
#define PAPER		0x00FFFFFFU
#define GRID		0x00C85A3CU	/* the squares of 原稿用紙 */

LOCAL SZ s_len( CONST char *t )
{
	SZ	n = 0;

	while ( t[n] != 0 ) {
		n++;
	}

	return n;
}

LOCAL BOOL	ready = FALSE;
LOCAL INT	wid = 0;

/* A face to draw with, and a window to show what is drawn */
LOCAL void start( void )
{
	T_DPRECT	o;

	if ( ready ) {
		return;
	}
	(void)kt_face();
	o.left = 20;  o.top = 20;  o.right = 1000;  o.bottom = 740;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "縦書き");
	if ( wid > 0 ) {
		wm_focus(wid);
	}
	ready = TRUE;
}

/* A record read whole. NULL when it is not on the disk. */
LOCAL UB *slurp( CONST char *path, SZ *p_size )
{
	T_FSTAT	st;
	UB	*buf;
	INT	fd, got = 0, n;

	if ( fs_stat(path, &st) < EX_OK || st.size == 0 ) {
		return NULL;
	}
	buf = (UB *)Kmalloc((SZ)st.size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		Kfree(buf);
		return NULL;
	}
	while ( got < (INT)st.size ) {
		n = fs_read(fd, buf + got, (SZ)( (INT)st.size - got ));
		if ( n <= 0 ) {
			break;
		}
		got += n;
	}
	fs_close(fd);
	buf[got] = 0;
	*p_size = (SZ)got;

	return buf;
}

/* A record parsed and its model made; FALSE when either failed */
LOCAL BOOL model_of( CONST char *xml, T_TAD **p_doc, T_TVDOC **p_d )
{
	*p_doc = NULL;
	*p_d = NULL;
	KT_ASSERT_ER(tad_parse((CONST UB *)xml, s_len(xml), NULL, p_doc), E_OK);
	if ( *p_doc == NULL ) {
		return FALSE;
	}
	KT_ASSERT_ER(tv_doc(*p_doc, p_d), E_OK);
	if ( *p_d == NULL ) {
		tad_free(*p_doc);
		*p_doc = NULL;
		return FALSE;
	}

	return TRUE;
}

LOCAL void model_free( T_TAD *doc, T_TVDOC *d )
{
	tv_doc_free(d);
	tad_free(doc);
}

/* ---------------------------------------------------------------- pixels of the test's own */

typedef struct {
	UW	*px;
	INT	w, h;
	INT	gid;
} CANVAS;

LOCAL BOOL canvas_open( CANVAS *c, INT w, INT h )
{
	T_DPRECT	all;
	INT		i;

	c->w = w;
	c->h = h;
	c->gid = -1;
	c->px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	if ( c->px == NULL ) {
		return FALSE;
	}
	for ( i = 0; i < w * h; i++ ) {
		c->px[i] = PAPER;
	}
	c->gid = dp_open();
	if ( c->gid < 0 ) {
		Kfree(c->px);
		c->px = NULL;
		return FALSE;
	}
	all.left = 0;  all.top = 0;  all.right = w;  all.bottom = h;
	dp_set_target(c->gid, c->px, (UINT)( w * 4 ), 0, 0);
	dp_set_origin(c->gid, 0, 0);
	dp_set_frame(c->gid, &all);
	dp_set_visible(c->gid, &all);

	return TRUE;
}

LOCAL void canvas_close( CANVAS *c )
{
	if ( c->gid >= 0 ) {
		dp_close(c->gid);
	}
	if ( c->px != NULL ) {
		Kfree(c->px);
	}
	c->px = NULL;
	c->gid = -1;
}

/*
 * The box of the pixels in a stretch of columns and rows that are dark
 * (letters drawn in black are mixed into the paper at their edges, so
 * what is more than half covered counts); FALSE when there are none.
 */
LOCAL BOOL ink_box( CONST CANVAS *c, INT x0, INT x1, INT y0, INT y1, T_DPRECT *b )
{
	INT	x, y;
	BOOL	any = FALSE;

	for ( y = ( y0 > 0 ) ? y0 : 0; y < y1 && y < c->h; y++ ) {
		for ( x = ( x0 > 0 ) ? x0 : 0; x < x1 && x < c->w; x++ ) {
			UW	p = c->px[y * c->w + x];

			if ( ( ( p >> 16 ) & 0xFF ) + ( ( p >> 8 ) & 0xFF ) + ( p & 0xFF ) > 3 * 100 ) {
				continue;
			}
			if ( !any ) {
				b->left = b->right = x;
				b->top = b->bottom = y;
				any = TRUE;
			}
			if ( x < b->left ) b->left = x;
			if ( x > b->right ) b->right = x;
			if ( y < b->top ) b->top = y;
			if ( y > b->bottom ) b->bottom = y;
		}
	}

	return any;
}

/* How many pixels of a colour there are in a stretch */
LOCAL INT colour_count( CONST CANVAS *c, UW col, INT x0, INT x1, INT y0, INT y1 )
{
	INT	x, y, n = 0;

	for ( y = ( y0 > 0 ) ? y0 : 0; y < y1 && y < c->h; y++ ) {
		for ( x = ( x0 > 0 ) ? x0 : 0; x < x1 && x < c->w; x++ ) {
			if ( ( c->px[y * c->w + x] & 0x00FFFFFFU ) == col ) {
				n++;
			}
		}
	}

	return n;
}

/* ---------------------------------------------------------------- reading */

LOCAL CONST char dir_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<docScale hunit=\"-72\" vunit=\"-72\"/>\n<text lang=\"0\" bpat=\"0\"/>\n"
"<text direction=\"2\"/>\n"
"<p><font size=\"20\"/>あいうえおかきくけこ</p>\n<p>さし</p>\n"
"</document></tad>";

/* 文字方向: 2 is down, anything else across */
LOCAL void test_direction( void )
{
	T_TAD	*doc;
	T_TVDOC	*d;

	if ( !model_of(dir_doc, &doc, &d) ) return;
	KT_ASSERT(d->vert);
	KT_ASSERT_EQ(d->npara, 2);
	model_free(doc, d);

	if ( !model_of("<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
		       "<text direction=\"0\"/><p>a</p></document></tad>", &doc, &d) ) return;
	KT_ASSERT(!d->vert);
	model_free(doc, d);

	if ( !model_of("<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
		       "<p>a</p></document></tad>", &doc, &d) ) return;
	KT_ASSERT(!d->vert);
	model_free(doc, d);
}

/* ---------------------------------------------------------------- the forms of the letters */

/*
 * The face has vertical forms of the comma, the full stop, the brackets
 * and the long vowel, and none of a kana that stands the same either way;
 * a comma drawn down the line sits in the upper right of its square, and
 * across in the lower left; every letter down the line is a square tall.
 */
LOCAL void test_forms( void )
{
	ID		fid;
	CANVAS		c;
	T_DPRECT	b;

	start();
	fid = fn_system();
	if ( fid <= 0 ) KT_SKIP("no face");
	KT_ASSERT(fn_has_vert(fid, 0x3001));		/* 、 */
	KT_ASSERT(fn_has_vert(fid, 0x3002));		/* 。 */
	KT_ASSERT(fn_has_vert(fid, 0x300C));		/* 「 */
	KT_ASSERT(fn_has_vert(fid, 0x30FC));		/* ー */
	KT_ASSERT(fn_has_vert(fid, 0x3063));		/* っ */
	KT_ASSERT(!fn_has_vert(fid, 0x6F22));		/* 漢 */
	KT_ASSERT(!fn_has_vert(fid, 'A'));

	KT_ASSERT_ER(fn_set_size(fid, 40), E_OK);
	KT_ASSERT_ER(fn_set_style(fid, FN_ST_VERT, 0), E_OK);
	KT_ASSERT_EQ(fn_vwidth(fid, (CONST UB *)"あいう"), 120);
	KT_ASSERT_EQ(fn_vwidth(fid, (CONST UB *)"Ab1"), 120);
	if ( !canvas_open(&c, 200, 100) ) return;
	(void)fn_vdraw(c.gid, fid, 50, 10, (CONST UB *)"、", INK);
	KT_ASSERT(ink_box(&c, 0, 100, 0, 100, &b));
	tm_printf((UB *)"  、 down: %d,%d - %d,%d\n", b.left, b.top, b.right, b.bottom);
	KT_ASSERT(b.left > 50);				/* right of the middle */
	KT_ASSERT(b.bottom < 10 + 20);			/* in the upper half */

	(void)fn_set_size(fid, 40);			/* across again */
	(void)fn_draw(c.gid, fid, 120, 10 + 34, (CONST UB *)"、", INK);
	KT_ASSERT(ink_box(&c, 100, 200, 0, 100, &b));
	tm_printf((UB *)"  、 across: %d,%d - %d,%d\n", b.left, b.top, b.right, b.bottom);
	KT_ASSERT(b.right < 120 + 20);			/* left of the middle */
	KT_ASSERT(b.top > 10 + 20);			/* in the lower half */

	/* the long vowel down the line stands upright: taller than wide */
	(void)fn_set_size(fid, 40);
	(void)fn_set_style(fid, FN_ST_VERT, 0);
	b.left = 0;  b.top = 0;  b.right = 200;  b.bottom = 100;
	(void)dp_fill_rect(c.gid, &b, PAPER);
	(void)fn_vdraw(c.gid, fid, 50, 10, (CONST UB *)"ー", INK);
	KT_ASSERT(ink_box(&c, 0, 100, 0, 100, &b));
	KT_ASSERT(b.bottom - b.top > b.right - b.left);
	(void)fn_set_size(fid, 14);
	canvas_close(&c);
}

/* ---------------------------------------------------------------- where the places are */

/*
 * In a box 300 wide and 100 tall, letters of 20: five to a line, the
 * lines 30 apart (half a letter between), the first at the right edge;
 * the place under a point is the place drawn there.
 */
LOCAL void test_places( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	T_DPRECT	r, b;
	INT		pi = -1, pos = -1, w;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	if ( !model_of(dir_doc, &doc, &d) ) return;
	r.left = 0;  r.top = 0;  r.right = 300;  r.bottom = 100;

	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 0, &b), E_OK);
	KT_ASSERT_EQ(b.right, 300);
	KT_ASSERT_EQ(b.left, 270);
	KT_ASSERT_EQ(b.top, 0);
	KT_ASSERT_EQ(b.bottom, 2);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 6, &b), E_OK);	/* after あい */
	KT_ASSERT_EQ(b.top, 40);
	KT_ASSERT_EQ(b.left, 270);
	/* the sixth letter begins the second line, to the left */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 15, &b), E_OK);
	KT_ASSERT_EQ(b.top, 0);
	KT_ASSERT_EQ(b.left, 240);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 21, &b), E_OK);
	KT_ASSERT_EQ(b.top, 40);
	/* the next paragraph after two lines and the space after a paragraph */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 1, 0, &b), E_OK);
	KT_ASSERT_EQ(b.right, 300 - 60 - 20);
	KT_ASSERT_EQ(b.top, 0);

	/* the place under a point */
	KT_ASSERT_ER(tv_doc_caret_at(d, &r, 0, doc, 285, 45, &pi, &pos), E_OK);
	KT_ASSERT_EQ(pi, 0);
	KT_ASSERT_EQ(pos, 6);
	KT_ASSERT_ER(tv_doc_caret_at(d, &r, 0, doc, 255, 1, &pi, &pos), E_OK);
	KT_ASSERT_EQ(pi, 0);
	KT_ASSERT_EQ(pos, 15);

	/* wound on: what was at the right edge has gone past it */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 30, doc, 0, 15, &b), E_OK);
	KT_ASSERT_EQ(b.right, 300);

	/* how wide the text comes to, for lines 100 long */
	w = tv_doc_height(d, 100, doc);
	tm_printf((UB *)"  width %d\n", w);
	KT_ASSERT(w > 60 + 20 && w < 200);
	model_free(doc, d);
}

/* ---------------------------------------------------------------- 縦中横, 回転, ルビ */

LOCAL CONST char tcy_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<text direction=\"2\"/>"
"<p><font size=\"20\"/>平成<combchar>12</combchar>年A<font rotation=\"90\" rotabs=\"0\">Word</font>X</p>"
"<p><ruby position=\"0\" text=\"かんじ\">漢字</ruby>です</p>"
"</document></tad>";

LOCAL void test_tcy( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	T_DPRECT	r, b, ink = { 0, 0, 0, 0 };
	CANVAS		c;
	INT		i, comb = 0, turned = 0;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	if ( !model_of(tcy_doc, &doc, &d) ) return;
	for ( i = 0; i < d->nrun; i++ ) {
		if ( ( d->run[i].style & TV_ST_COMB ) != 0 ) comb++;
		if ( d->run[i].turn == 90 ) turned++;
	}
	KT_ASSERT_EQ(comb, 1);
	KT_ASSERT_EQ(turned, 1);
	r.left = 0;  r.top = 0;  r.right = 200;  r.bottom = 400;

	/* 縦中横: the two figures take one square */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 6, &b), E_OK);
	KT_ASSERT_EQ(b.top, 40);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 8, &b), E_OK);
	KT_ASSERT_EQ(b.top, 60);
	/* 回転: the word as long as it is wide across, less than four squares */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 12, &b), E_OK);
	KT_ASSERT_EQ(b.top, 100);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 16, &b), E_OK);
	tm_printf((UB *)"  turned word ends at %d\n", b.top);
	KT_ASSERT(b.top > 100 + 30 && b.top < 100 + 80);

	/* the same with letters spaced apart, as 原稿用紙 has them */
	{
		T_TAD	*doc2;
		T_TVDOC	*d2;
		T_DPRECT	e;

		if ( model_of("<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
			      "<text direction=\"2\"/><font size=\"18\" space=\"0.223\"/>"
			      "<p>あ<font rotation=\"90\" rotabs=\"0\">TRON</font>い</p>"
			      "</document></tad>", &doc2, &d2) ) {
			KT_ASSERT_ER(tv_doc_caret_box(d2, &r, 0, doc2, 0, 3, &e), E_OK);
			KT_ASSERT_EQ(e.top, 22);
			KT_ASSERT_ER(tv_doc_caret_box(d2, &r, 0, doc2, 0, 7, &e), E_OK);
			tm_printf((UB *)"  spaced turned word ends at %d\n", e.top);
			KT_ASSERT(e.top < 22 + 4 * 22 - 8);
			model_free(doc2, d2);
		}
	}

	if ( !canvas_open(&c, 200, 400) ) { model_free(doc, d); return; }
	(void)tv_doc_draw(c.gid, d, &r, 0, doc, PAPER);
	/* the square of 縦中横 holds letters laid across: wider than tall */
	KT_ASSERT(ink_box(&c, 160, 200, 41, 59, &ink));
	tm_printf((UB *)"  12: %d,%d - %d,%d\n", ink.left, ink.top, ink.right, ink.bottom);
	KT_ASSERT(ink.right - ink.left > ink.bottom - ink.top);
	/* the turned word lies along the line: taller than wide */
	KT_ASSERT(ink_box(&c, 160, 200, 101, b.top - 1, &ink));
	KT_ASSERT(ink.bottom - ink.top > ink.right - ink.left);
	/*
	 * ルビ: down the right side of the letters it belongs to. The line
	 * is as wide as the letters and the ruby beside them, the letters
	 * at its left, so what is drawn in its rightmost pixels is the ruby.
	 */
	{
		T_DPRECT	l2, rb = { 0, 0, 0, 0 };

		KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 1, 0, &l2), E_OK);
		KT_ASSERT(ink_box(&c, l2.left, l2.right - 10, 0, 40, &ink));
		KT_ASSERT(ink_box(&c, l2.right - 9, l2.right, 0, 40, &rb));
		tm_printf((UB *)"  line %d-%d letters %d-%d ruby %d-%d\n", l2.left, l2.right,
			  ink.left, ink.right, rb.left, rb.right);
	}
	canvas_close(&c);
	model_free(doc, d);
}

/* ---------------------------------------------------------------- 禁則 */

LOCAL CONST char kin_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<text direction=\"2\"/>"
"<p><font size=\"20\"/>あいうえお。か</p>"
"<line-head-kinsoku kind=\"0x30\" ch=\"、。\"/>"
"<p>あいうえお。か</p>"
"<line-tail-kinsoku kind=\"0x10\" ch=\"「\"/>"
"<p>あいうえ「か」</p>"
"</document></tad>";

/*
 * Five squares to a line. A full stop may not begin a line: the letter
 * before it goes on to the next line with it (追い出し); with ぶら下げ
 * (K = 3) it stays on the line past its end. An opening bracket may not
 * end a line: it goes on to the next.
 */
LOCAL void test_kinsoku( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	T_DPRECT	r, a, b;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	if ( !model_of(kin_doc, &doc, &d) ) return;
	r.left = 0;  r.top = 0;  r.right = 400;  r.bottom = 100;

	/* 追い出し: お at the head of the second line */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 0, &a), E_OK);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 12, &b), E_OK);
	KT_ASSERT_EQ(b.top, 0);
	KT_ASSERT(b.right <= a.left);
	/* ぶら下げ: 。 below the fifth square, on the first line */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 1, 0, &a), E_OK);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 1, 15, &b), E_OK);
	KT_ASSERT_EQ(b.top, 100);
	KT_ASSERT_EQ(b.left, a.left);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 1, 18, &b), E_OK);	/* か */
	KT_ASSERT_EQ(b.top, 0);
	KT_ASSERT(b.right <= a.left);
	/* 「 does not end the first line */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 2, 0, &a), E_OK);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 2, 12, &b), E_OK);
	KT_ASSERT_EQ(b.top, 0);
	KT_ASSERT(b.right <= a.left);
	model_free(doc, d);
}

/* ---------------------------------------------------------------- 原稿用紙 */

/*
 * The document of squares, with 401 letters and a full stop after the twentieth put
 * in its paragraph: on its paper each
 * is in a square of the grid -- the twenty of a line from the top
 * margin down, the lines from the right margin to the left -- a full
 * stop after the twentieth hangs below the grid, and the 401st letter
 * starts a second sheet. Each sheet carries the grid.
 */
LOCAL void test_genko( void )
{
	UB		*xml, *words;
	SZ		size = 0;
	T_TAD		*doc = NULL;
	T_TVDOC		*d = NULL;
	T_TADNODE	*p;
	T_DPRECT	r, b, ink, sheet;
	CANVAS		c;
	INT		i, k, n = 0;
	BOOL		inside = TRUE;
	static CONST char kanji[] = "永";		/* a letter that fills its square */

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	xml = slurp(XT_GENKO, &size);
	if ( xml == NULL ) KT_SKIP("no template on the disk");
	KT_ASSERT_ER(tad_parse(xml, size, NULL, &doc), E_OK);
	Kfree(xml);
	if ( doc == NULL ) return;

	/* twenty letters and a full stop, then 381 more */
	words = (UB *)Kmalloc(3 * 402 + 1);
	if ( words == NULL ) { tad_free(doc); return; }
	for ( i = 0; i < 20; i++ ) {
		for ( k = 0; k < 3; k++ ) words[n++] = (UB)kanji[k];
	}
	words[n++] = 0xE3;  words[n++] = 0x80;  words[n++] = 0x82;	/* 。 */
	for ( i = 0; i < 381; i++ ) {
		for ( k = 0; k < 3; k++ ) words[n++] = (UB)kanji[k];
	}
	words[n] = 0;
	for ( p = tad_walk(doc, NULL); p != NULL; p = tad_walk(doc, p) ) {
		if ( p->kind == TAD_ND_ELEM && p->name != NULL && p->name[0] == 'p' && p->name[1] == 0 ) {
			break;
		}
	}
	KT_ASSERT(p != NULL);
	if ( p == NULL ) { Kfree(words); tad_free(doc); return; }
	KT_ASSERT(tad_text_new(doc, p, NULL, words, n) != NULL);
	Kfree(words);
	KT_ASSERT_ER(tv_doc(doc, &d), E_OK);
	if ( d == NULL ) { tad_free(doc); return; }

	KT_ASSERT(d->vert);
	KT_ASSERT(tv_doc_has_sheet(d));
	KT_ASSERT_EQ(d->paper_w, 842);
	KT_ASSERT_EQ(d->paper_h, 595);
	KT_ASSERT_EQ(tv_doc_pages(d, doc), 2);

	/* on the paper, a sheet across and the next to its left */
	d->page_h = d->paper_w;
	r.left = 0;  r.top = 0;  r.right = 842;  r.bottom = 595;
	for ( i = 0; i < 20; i++ ) {
		KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 3 * i, &b), E_OK);
		if ( b.top != 78 + 22 * i || b.right != 842 - 81 || b.left != 842 - 81 - 34 ) {
			inside = FALSE;
			tm_printf((UB *)"  letter %d at %d,%d - %d\n", i, b.left, b.top, b.right);
		}
	}
	KT_ASSERT(inside);
	/* the full stop after the twentieth hangs below the grid */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 60, &b), E_OK);
	KT_ASSERT_EQ(b.top, 78 + 22 * 20);
	KT_ASSERT_EQ(b.right, 842 - 81);
	/* the line after begins in the first square of the second line */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 63, &b), E_OK);
	KT_ASSERT_EQ(b.top, 78);
	KT_ASSERT_EQ(b.right, 842 - 81 - 34);
	/* the twentieth line, and the 401st letter on the next sheet */
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 63 + 3 * 18 * 20, &b), E_OK);
	KT_ASSERT_EQ(b.right, 842 - 81 - 34 * 19);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 63 + 3 * 19 * 20, &b), E_OK);
	KT_ASSERT_EQ(b.top, 78);
	KT_ASSERT_EQ(b.right, 842 - 81 - 842);

	/* a sheet drawn as a printer is given it: the grid, and each letter in its square */
	if ( canvas_open(&c, 842, 595) ) {
		sheet.left = 0;  sheet.top = 0;  sheet.right = 842;  sheet.bottom = 595;
		KT_ASSERT_ER(tv_doc_draw_page(c.gid, d, 0, &sheet, doc, PAPER), E_OK);
		/* the sides of the first line of squares, and a square's top */
		KT_ASSERT(colour_count(&c, GRID, 731, 732, 76, 516) > 400);
		KT_ASSERT(colour_count(&c, GRID, 754, 755, 76, 516) > 400);
		KT_ASSERT(colour_count(&c, GRID, 732, 754, 98, 99) > 15);
		/* nothing of the grid between two lines of squares */
		KT_ASSERT_EQ(colour_count(&c, GRID, 722, 730, 90, 98), 0);
		/* the letters inside their squares */
		inside = TRUE;
		for ( i = 0; i < 20; i++ ) {
			if ( !ink_box(&c, 732, 754, 77 + 22 * i, 98 + 22 * i, &ink) ) {
				inside = FALSE;
				tm_printf((UB *)"  square %d empty\n", i);
			}
		}
		KT_ASSERT(inside);
		/* and none of them reaching the lines between the squares */
		for ( i = 1; i < 20; i++ ) {
			if ( ink_box(&c, 732, 754, 76 + 22 * i, 77 + 22 * i, &ink) ) {
				inside = FALSE;
			}
		}
		KT_ASSERT(inside);
		KT_ASSERT(ink_box(&c, 732, 760, 520, 545, &ink));	/* the full stop */
		/* the second sheet has the grid too */
		(void)dp_fill_rect(c.gid, &sheet, PAPER);
		KT_ASSERT_ER(tv_doc_draw_page(c.gid, d, 1, &sheet, doc, PAPER), E_OK);
		KT_ASSERT(colour_count(&c, GRID, 731, 732, 76, 516) > 400);
		KT_ASSERT(ink_box(&c, 732, 754, 77, 98, &ink));
		KT_ASSERT(!ink_box(&c, 698, 720, 77, 98, &ink));
		canvas_close(&c);
	}

	/* on the screen, as the window shows it */
	if ( wid > 0 ) {
		T_DPRECT	w;
		INT		gid = wm_gid(wid);
		T_WMWIN		ww;

		if ( gid >= 0 && wm_ref(wid, &ww) >= E_OK ) {
			w.left = 0;  w.top = 0;
			w.right = ww.work.right - ww.work.left;
			w.bottom = ww.work.bottom - ww.work.top;
			(void)dp_fill_rect(gid, &w, 0x00A0A0A0U);
			w.bottom = 595;
			(void)tv_doc_draw(gid, d, &w, 842 - w.right, doc, PAPER);
			KT_ASSERT_ER(wm_composite(), E_OK);
			kt_shot("/boot/GENKO.PPM");
		}
	}
	tv_doc_free(d);
	tad_free(doc);
}

/* ---------------------------------------------------------------- the editor's own records */

LOCAL CONST char edit_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n"
"<docScale hunit=\"-72\" vunit=\"-72\"/>\n<text lang=\"0\" bpat=\"0\"/>\n"
"<p>令和7年ABC版</p>\n"
"</document>\n</tad>\n";

/* The record written out, NUL ended; NULL when it cannot be */
LOCAL UB *written( CONST T_TAD *doc, SZ *p_n )
{
	SZ	n = 0;
	UB	*buf;

	(void)tad_write_mem(doc, NULL, 0, &n);
	buf = (UB *)Kmalloc(n + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tad_write_mem(doc, buf, n + 1, &n) < E_OK ) {
		Kfree(buf);
		return NULL;
	}
	buf[n] = 0;
	*p_n = n;

	return buf;
}

LOCAL BOOL has_text( CONST UB *s, CONST char *w )
{
	INT	i, k;

	for ( i = 0; s[i] != 0; i++ ) {
		for ( k = 0; w[k] != 0 && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) {
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * 縦書き, 縦中横 and 回転 from the menu, through the editor: the record
 * says <text direction="2"/> after the head of the document, before the
 * first paragraph; the figures picked are put in <combchar>, the letters
 * in <font rotation="90">; each comes back off again, and 横書き leaves
 * the record byte for byte as it was. A record written elsewhere for a
 * text written downwards -- its lines ending in CR LF, the direction
 * after the paper -- reads as 縦書き and writes back unchanged.
 */
LOCAL void test_edit( void )
{
	DTWIN	*w;
	T_TAD	*doc = NULL;
	UB	*out;
	SZ	n = 0;

	start();
	KT_ASSERT_ER(tad_parse((CONST UB *)edit_doc, s_len(edit_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	w = (DTWIN *)Kmalloc(sizeof(DTWIN));
	if ( w == NULL ) { tad_free(doc); return; }
	knl_memset(w, 0, sizeof(*w));
	w->used = TRUE;
	w->rec = doc;
	w->kind = TV_KIND_DOC;
	w->paper = TV_PAPER_LOOK;
	ed_model(w);
	KT_ASSERT(w->doc != NULL && !w->doc->vert);

	/* 縦書き */
	KT_ASSERT(dd_command(w, DM_DIR_VERT));
	KT_ASSERT(w->doc->vert);
	out = written(doc, &n);
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT(has_text(out, "<text lang=\"0\" bpat=\"0\"/>\n<text direction=\"2\"/>\n<p>"));
		Kfree(out);
	}

	/* 縦中横 on "7", and off again */
	w->dd.apara = 0;  w->dd.apos = 6;  w->dd.cpara = 0;  w->dd.cpos = 7;
	KT_ASSERT(dd_command(w, DM_COMB));
	out = written(doc, &n);
	if ( out != NULL ) {
		KT_ASSERT(has_text(out, "<combchar>7</combchar>"));
		Kfree(out);
	}
	{
		BOOL	comb = FALSE, turned = FALSE;

		w->dd.apara = 0;  w->dd.apos = 6;  w->dd.cpara = 0;  w->dd.cpos = 7;
		dd_menu_state(w, &comb, &turned);
		KT_ASSERT(comb);
		KT_ASSERT(!turned);
	}
	w->dd.apara = 0;  w->dd.apos = 6;  w->dd.cpara = 0;  w->dd.cpos = 7;
	KT_ASSERT(dd_command(w, DM_COMB));
	out = written(doc, &n);
	if ( out != NULL ) {
		KT_ASSERT(!has_text(out, "combchar"));
		Kfree(out);
	}

	/* 回転 on "ABC", and off again */
	w->dd.apara = 0;  w->dd.apos = 10;  w->dd.cpara = 0;  w->dd.cpos = 13;
	KT_ASSERT(dd_command(w, DM_TURN));
	out = written(doc, &n);
	if ( out != NULL ) {
		KT_ASSERT(has_text(out, "<font rotation=\"90\" rotabs=\"0\">ABC</font>"));
		Kfree(out);
	}
	w->dd.apara = 0;  w->dd.apos = 10;  w->dd.cpara = 0;  w->dd.cpos = 13;
	KT_ASSERT(dd_command(w, DM_TURN));

	/* 横書き: the record as it was */
	KT_ASSERT(dd_command(w, DM_DIR_HORIZ));
	KT_ASSERT(!w->doc->vert);
	out = written(doc, &n);
	if ( out != NULL ) {
		if ( !has_text(out, edit_doc) ) {
			tm_printf((UB *)"  %s\n", out);
		}
		KT_ASSERT_EQ(n, s_len(edit_doc));
		KT_ASSERT(has_text(out, edit_doc));
		Kfree(out);
	}
	ed_forget(w);
	Kfree(w);
	tad_free(doc);
}

/* A text written downwards as another program writes it, read and written back */
LOCAL CONST char tadjs_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">\r\n<document>\r\n"
"<text lang=\"0\" bpat=\"0\"/>\r\n"
"<paper imposition=\"0\" binding=\"0\" length=\"842\" width=\"595\" top=\"57\" bottom=\"57\" left=\"57\" right=\"57\" />\r\n"
"<docmargin top=\"57\" bottom=\"57\" left=\"57\" right=\"57\" />\r\n"
"<text direction=\"2\"/>\r\n"
"<p>\r\n<font size=\"14\">縦書きの<combchar>12</combchar>文章</font>\r\n</p>\r\n"
"</document>\r\n</tad>\r\n";

LOCAL void test_roundtrip( void )
{
	T_TAD	*doc;
	T_TVDOC	*d;
	UB	*out;
	SZ	n = 0;

	if ( !model_of(tadjs_doc, &doc, &d) ) return;
	KT_ASSERT(d->vert);
	KT_ASSERT_EQ(d->paper_w, 595);
	out = written(doc, &n);
	KT_ASSERT(out != NULL);
	if ( out != NULL ) {
		KT_ASSERT_EQ(n, s_len(tadjs_doc));
		KT_ASSERT(has_text(out, tadjs_doc));
		Kfree(out);
	}
	model_free(doc, d);
}

/* ---------------------------------------------------------------- on the screen */

LOCAL CONST char show_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<text direction=\"2\"/>"
"<p><font size=\"24\"/>「縦書き」の文章です。句読点、括弧（かっこ）、長音ー、小書きのっゃゅょ。</p>"
"<p>平成<combchar>12</combchar>年に<font rotation=\"90\" rotabs=\"0\">TRON</font>を使う。"
"<ruby position=\"0\" text=\"かんじ\">漢字</ruby>にルビ、<underline>傍線</underline>も。</p>"
"</document></tad>";

/* A text written downwards in the window, its caret and a stretch picked */
LOCAL void test_show( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	T_DPRECT	r, b;
	INT		gid;

	start();
	if ( fn_system() <= 0 || wid <= 0 ) KT_SKIP("no face or window");
	if ( !model_of(show_doc, &doc, &d) ) return;
	gid = wm_gid(wid);
	r.left = 0;  r.top = 0;  r.right = 976;  r.bottom = 690;
	(void)dp_fill_rect(gid, &r, PAPER);
	r.right = 900;  r.bottom = 360;
	(void)tv_doc_draw(gid, d, &r, 0, doc, PAPER);
	tv_doc_select(gid, d, &r, 0, doc, 1, 0, 1, 6);
	KT_ASSERT_ER(tv_doc_caret_box(d, &r, 0, doc, 0, 9, &b), E_OK);
	b.bottom = b.top + 2;
	(void)dp_fill_rect(gid, &b, 0x00D00000U);
	KT_ASSERT_ER(wm_composite(), E_OK);
	kt_shot("/boot/VERT.PPM");
	model_free(doc, d);
}

LOCAL void test_done( void )
{
	if ( wid > 0 ) {
		wm_close(wid);
		wid = 0;
	}
	ready = FALSE;
	KT_ASSERT_ER(wm_composite(), E_OK);
}

EXPORT void ktest_vt( void )
{
	KT_RUN(test_direction);
	KT_RUN(test_forms);
	KT_RUN(test_places);
	KT_RUN(test_tcy);
	KT_RUN(test_kinsoku);
	KT_RUN(test_genko);
	KT_RUN(test_edit);
	KT_RUN(test_roundtrip);
	KT_RUN(test_show);
	KT_RUN(test_done);
}
