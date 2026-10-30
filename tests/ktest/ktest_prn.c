/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_prn.c
 *	印刷: pages written into PDF files (design 11.22)
 *
 *	The files are read back here the way a reader of PDF reads them --
 *	the table of where the objects are, the pages, the drawing of a page
 *	unpacked -- and the numbers in the drawing are held against where
 *	the screen puts the same things. Each file is also left on /boot
 *	(PRN*.PDF), where tools on the host check it again with a reader
 *	of PDF of their own and draw it into pixels.
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
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/img.h>
#include <ts/print.h>
#include <ts/sysdef.h>
#include <ts/uiprn.h>
#include "../../application/desktop/desktop.h"
#include "../../application/mcard/mc_db.h"
#include "../../application/calc/calc_tad.h"

#define XT_GENKO	"/boot/genko.xtad"
#define PAPER		0x00FFFFFFU

LOCAL INT s_len( CONST char *t )
{
	INT	n = 0;

	while ( t[n] != 0 ) {
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------- reading a file of PDF */

/* Where a word is in the bytes from 'from' on, or -1 */
LOCAL INT find( CONST UB *s, INT n, INT from, CONST char *w )
{
	INT	i, k, wl = s_len(w);

	for ( i = ( from > 0 ) ? from : 0; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) {
			return i;
		}
	}
	return -1;
}

LOCAL INT count( CONST UB *s, INT n, CONST char *w )
{
	INT	at = 0, c = 0;

	while ( ( at = find(s, n, at, w) ) >= 0 ) {
		c++;
		at++;
	}
	return c;
}

/* A number written at s[i], and where it ends */
LOCAL D num_at( CONST UB *s, INT n, INT i, INT *p_end, INT *p_milli )
{
	D	v = 0;
	INT	m = 0, digits = 0;
	BOOL	neg = FALSE;

	while ( i < n && s[i] == ' ' ) i++;
	if ( i < n && s[i] == '-' ) {
		neg = TRUE;
		i++;
	}
	while ( i < n && s[i] >= '0' && s[i] <= '9' ) {
		v = v * 10 + ( s[i] - '0' );
		i++;
	}
	if ( i < n && s[i] == '.' ) {
		i++;
		while ( i < n && s[i] >= '0' && s[i] <= '9' ) {
			if ( digits < 3 ) {
				m = m * 10 + ( s[i] - '0' );
				digits++;
			}
			i++;
		}
	}
	while ( digits < 3 ) {
		m *= 10;
		digits++;
	}
	if ( p_end != NULL ) *p_end = i;
	if ( p_milli != NULL ) *p_milli = neg ? -(INT)( v * 1000 + m ) : (INT)( v * 1000 + m );
	return neg ? -v : v;
}

/*
 * The table of where the objects are, checked: startxref names it, it
 * lists every object, and each object that is in use begins where it
 * says with its own number. Answers how many objects, -1 when not.
 */
LOCAL INT xref_check( CONST UB *s, INT n )
{
	INT	sx = find(s, n, n > 64 ? n - 64 : 0, "startxref\n"), at, x, cnt, i, e;

	if ( sx < 0 || find(s, n, 0, "%PDF-1.7") != 0 || find(s, n, n - 7, "%%EOF\n") < 0 ) {
		return -1;
	}
	x = (INT)num_at(s, n, sx + 10, NULL, NULL);
	if ( x <= 0 || x >= n || find(s, n, x, "xref\n0 ") != x ) {
		return -1;
	}
	cnt = (INT)num_at(s, n, x + 7, &e, NULL);
	at = find(s, n, x, "\n0000000000 65535 f \n");
	if ( at < 0 ) {
		return -1;
	}
	at += 21;
	for ( i = 1; i < cnt; i++, at += 20 ) {
		INT	off = (INT)num_at(s, n, at, NULL, NULL), k;
		UB	t[16];

		if ( s[at + 17] != 'n' ) {
			continue;
		}
		k = 0;
		{
			INT	v = i, m = 0;
			UB	r[12];

			do {
				r[m++] = (UB)( '0' + v % 10 );
				v /= 10;
			} while ( v > 0 );
			while ( m > 0 ) t[k++] = r[--m];
		}
		t[k++] = ' ';
		t[k++] = '0';
		t[k++] = ' ';
		t[k] = 0;
		if ( off <= 0 || off >= n || find(s, n, off, (CONST char *)t) != off ) {
			tm_printf((UB *)"  object %d is not at %d\n", i, off);
			return -1;
		}
	}
	if ( find(s, n, at, "trailer\n<< /Size ") != at ) {
		return -1;
	}
	return cnt;
}

/* The number after a word, -1 when the word is not there */
LOCAL INT num_after( CONST UB *s, INT n, INT from, CONST char *w )
{
	INT	at = find(s, n, from, w);

	return ( at < 0 ) ? -1 : (INT)num_at(s, n, at + s_len(w), NULL, NULL);
}

/* An object's stream unpacked, in memory the caller frees; NULL when it cannot be */
LOCAL UB *stream_of( CONST UB *s, INT n, INT obj, INT *p_len )
{
	UB	head[20];
	INT	k = 0, v = obj, m = 0, at, len, data;
	UB	r[12];
	UB	*out;
	INT	got;

	head[k++] = '\n';
	do {
		r[m++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( m > 0 ) head[k++] = r[--m];
	head[k++] = ' ';
	head[k++] = '0';
	head[k++] = ' ';
	head[k++] = 'o';
	head[k++] = 'b';
	head[k++] = 'j';
	head[k] = 0;
	at = find(s, n, 0, (CONST char *)head);
	if ( at < 0 ) {
		return NULL;
	}
	len = num_after(s, n, at, "/Length ");
	data = find(s, n, at, "stream\n");
	if ( len <= 0 || data < 0 ) {
		return NULL;
	}
	data += 7;
	out = (UB *)Kmalloc(4 * 1024 * 1024);
	if ( out == NULL ) {
		return NULL;
	}
	if ( find(s, data, at, "/FlateDecode") < 0 ) {
		knl_memcpy(out, s + data, len);
		*p_len = len;
		return out;
	}
	got = ts_zlib_inflate(s + data, len, out, 4 * 1024 * 1024);
	if ( got < 0 ) {
		Kfree(out);
		return NULL;
	}
	*p_len = got;
	return out;
}

/* The drawing of page k (from 0), unpacked */
LOCAL UB *page_content( CONST UB *s, INT n, INT k, INT *p_len )
{
	INT	at = 0, i;

	for ( i = 0; i <= k; i++ ) {
		at = find(s, n, at, "/Type /Page /Parent");
		if ( at < 0 ) {
			return NULL;
		}
		if ( i < k ) at++;
	}
	return stream_of(s, n, num_after(s, n, at, "/Contents "), p_len);
}

/*
 * The first glyph drawn at or after 'from' in a page's drawing: where
 * its string starts (Tm's e and f, thousandths of a pixel) and the
 * glyph's number. FALSE when there is none.
 */
LOCAL BOOL glyph_at( CONST UB *c, INT n, INT *p_from, INT *p_e, INT *p_f, INT *p_gid )
{
	INT	at = find(c, n, *p_from, " Tm [<"), i, k, e;

	if ( at < 0 ) {
		return FALSE;
	}
	/* back over the six numbers of Tm */
	i = at;
	for ( k = 0; k < 6 && i > 0; ) {
		i--;
		if ( c[i] == ' ' || c[i] == '\n' ) {
			k++;
		}
	}
	for ( k = 0; k < 4; k++ ) {
		(void)num_at(c, n, i + 1, &e, NULL);
		i = e;
	}
	(void)num_at(c, n, i, &e, p_e);
	(void)num_at(c, n, e, &i, p_f);
	*p_gid = 0;
	for ( k = at + 6; k < at + 10 && k < n; k++ ) {
		UB	h = c[k];

		*p_gid = *p_gid * 16 + ( ( h >= 'A' ) ? h - 'A' + 10 : h - '0' );
	}
	*p_from = at + 1;
	return TRUE;
}

/* A file left on /boot for the tools on the host */
LOCAL void leave( CONST char *path, CONST UB *b, SZ n )
{
	INT	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC), at = 0, w;

	if ( fd < 0 ) {
		return;
	}
	while ( at < (INT)n ) {
		w = fs_write(fd, b + at, (SZ)( n - at ));
		if ( w <= 0 ) {
			break;
		}
		at += w;
	}
	fs_close(fd);
}

/*
 * The same page drawn as the screen draws it, at a point to a pixel,
 * left as a picture (PPM) beside the file: the host draws the file
 * into pixels and holds the two against each other.
 */
LOCAL void leave_screen( CONST char *path, CONST T_PRNJOB *job, INT page )
{
	INT		w = job->lw, h = job->lh, gid, i, n = 0, k;
	UW		*px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	UB		*out = (UB *)Kmalloc((SZ)w * h * 3 + 32);
	T_DPRECT	all;
	UB		t[12];

	if ( px == NULL || out == NULL ) {
		if ( px != NULL ) Kfree(px);
		if ( out != NULL ) Kfree(out);
		return;
	}
	for ( i = 0; i < w * h; i++ ) {
		px[i] = PAPER;
	}
	gid = dp_open();
	if ( gid > 0 ) {
		all.left = 0;
		all.top = 0;
		all.right = w;
		all.bottom = h;
		dp_set_target(gid, px, (UINT)( w * 4 ), 0, 0);
		dp_set_origin(gid, 0, 0);
		dp_set_frame(gid, &all);
		dp_set_visible(gid, &all);
		(void)job->draw(job->arg, gid, page, &all);
		dp_close(gid);
	}
	out[n++] = 'P';
	out[n++] = '6';
	out[n++] = '\n';
	for ( k = 0; k < 2; k++ ) {
		INT	v = ( k == 0 ) ? w : h, m = 0;

		do {
			t[m++] = (UB)( '0' + v % 10 );
			v /= 10;
		} while ( v > 0 );
		while ( m > 0 ) out[n++] = t[--m];
		out[n++] = ( k == 0 ) ? ' ' : '\n';
	}
	out[n++] = '2';
	out[n++] = '5';
	out[n++] = '5';
	out[n++] = '\n';
	for ( i = 0; i < w * h; i++ ) {
		out[n++] = (UB)( px[i] >> 16 );
		out[n++] = (UB)( px[i] >> 8 );
		out[n++] = (UB)px[i];
	}
	if ( kt_room(path, (UD)n) ) {
		leave(path, out, (SZ)n);
	}
	Kfree(px);
	Kfree(out);
}

/* ---------------------------------------------------------------- what is printed */

LOCAL BOOL	ready = FALSE;

LOCAL void start( void )
{
	if ( ready ) {
		return;
	}
	if ( fn_system() <= 0 ) {
		ID	fid = 0;
		TS_UUID	u;
		ER	er;

		er = kt_sysobj(SYSDEF_FONT_BOX, "NotoSansJP-Regular.otf", &u);
		if ( er >= E_OK ) {
			er = fn_open_obj(&u, xf_data_rec(&u), 0, &fid);
		}
		if ( er >= E_OK && fid > 0 ) {
			fn_set_size(fid, 14);
			fn_set_system(fid);
		}
	}
	ready = TRUE;
}

LOCAL BOOL model( CONST char *xml, T_TAD **p_doc, T_TVDOC **p_d )
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
		return FALSE;
	}
	return TRUE;
}

/* Three pages of A4, a break between each, letters of both kinds */
LOCAL CONST char doc3[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<paper type=\"doc\" length=\"842\" width=\"595\" top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<docmargin top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<p><font size=\"16\"/>印刷の試験 Print test 一頁目</p>"
"<p>二行目は<bold>太字</bold>と<font color=\"#cc0000\"/>赤い字<font color=\"#000000\"/>です。</p>"
"<p><pagebreak/></p><p>二頁目の文章です。</p>"
"<p><pagebreak/></p><p>三頁目の文章です。おわり</p>"
"</document></tad>";

/*
 * ヘッダ and フッタ as 用紙設定 writes them: the object's name, the page
 * of how many, the date (%N %P/%T and %y/%m/%d of the panel)
 */
LOCAL CONST char dochead[] =
"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"報告書\"><document>"
"<paper type=\"doc\" length=\"842\" width=\"595\" top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<docmargin top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<paper-overlay-define N=\"0\" P=\"0\"><p><text align=\"right\"/><variable id=\"0\"/> "
"<page-number step=\"1\" num=\"1\"/>/<variable id=\"250\"/></p></paper-overlay-define>"
"<paper-overlay-define N=\"1\" P=\"0\"><p><text align=\"center\"/>"
"<variable id=\"102\"/>年<variable id=\"110\"/>月<variable id=\"120\"/>日</p></paper-overlay-define>"
"<docoverlay active=\"0, 1\"/>"
"<p>一頁目</p><p><pagebreak/></p><p>二頁目</p><p><pagebreak/></p><p>三頁目</p>"
"</document></tad>";

/* A letter, a link and a letter: closed up when links are not printed */
LOCAL CONST char doclink[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p>前<link id=\"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_0.xtad\" name=\"仮身\""
" width=\"120\" heightpx=\"22\" chsz=\"14\"/>後</p>"
"</document></tad>";

/* Shapes of a figure: a box with round corners, an ellipse, lines, a pattern, words */
LOCAL CONST char fig1[] =
"<tad version=\"1.0\" encoding=\"UTF-8\">"
"<paper type=\"fig\" length=\"595\" width=\"420\"/>"
"<docmargin top=\"20\" bottom=\"20\" left=\"20\" right=\"20\"/>"
"<figure>"
"<patterns>"
"<pattern id=\"130\" type=\"0\" width=\"16\" height=\"16\" ncol=\"2\" fgcolors=\"#ff0000,#0000ff\" bgcolor=\"transparent\" masks=\"200,201\"/>"
"<mask id=\"200\" type=\"0\" width=\"16\" height=\"16\" data=\"8000,4000,2000,1000,0800,0400,0200,0100,0080,0040,0020,0010,0008,0004,0002,0001\"/>"
"<mask id=\"201\" type=\"0\" width=\"8\" height=\"1\" data=\"0100\"/>"
"</patterns>"
"<rect round=\"1\" cornerRadius=\"8\" figRH=\"16\" figRV=\"10\" l_pat=\"1\" f_pat=\"0\" left=\"40\" top=\"40\" right=\"240\" bottom=\"140\"/>"
"<ellipse l_pat=\"1\" f_pat=\"4\" frameLeft=\"300\" frameTop=\"40\" frameRight=\"500\" frameBottom=\"140\"/>"
"<line l_pat=\"1\" lineWidth=\"3\" points=\"40,200 500,200\"/>"
"<line l_pat=\"1\" lineType=\"2\" points=\"40,240 500,300\"/>"
"<rect l_pat=\"1\" f_pat=\"130\" left=\"40\" top=\"340\" right=\"240\" bottom=\"440\"/>"
"<rect l_pat=\"1\" fillColor=\"#e0f0ff\" left=\"300\" top=\"340\" right=\"500\" bottom=\"440\" fontSize=\"20\" textColor=\"#003399\">図形の文字</rect>"
"<rect l_pat=\"1\" left=\"40\" top=\"1200\" right=\"200\" bottom=\"1260\"/>"
"</figure></tad>";

/* ---------------------------------------------------------------- the tests */

LOCAL void test_deflate( void )
{
	UB	*in = (UB *)Kmalloc(200000), *z = NULL, *back = (UB *)Kmalloc(200000);
	SZ	zl = 0;
	INT	i, got;
	UW	r = 12345;

	KT_ASSERT(in != NULL && back != NULL);
	if ( in == NULL || back == NULL ) return;
	/* words that repeat, and bytes that do not */
	for ( i = 0; i < 120000; i++ ) {
		in[i] = (UB)"BT /F1 1 Tf 12 0 0 -12 100 200 Tm [<0123> 5 <04AB>] TJ\n"[i % 56];
	}
	for ( ; i < 200000; i++ ) {
		r = r * 1103515245U + 12345U;
		in[i] = (UB)( r >> 24 );
	}
	KT_ASSERT_ER(ts_zlib_deflate(in, 200000, &z, &zl, 6), E_OK);
	if ( z == NULL ) return;
	got = ts_zlib_inflate(z, zl, back, 200000);
	KT_ASSERT_EQ(got, 200000);
	for ( i = 0; i < 200000 && in[i] == back[i]; i++ ) ;
	KT_ASSERT_EQ(i, 200000);
	/* the repeated part packs to almost nothing; the rest cannot be packed */
	tm_printf((UB *)"  200000 bytes packed to %d\n", (INT)zl);
	KT_ASSERT(zl < 85000);
	Kfree(z);
	/* nothing, and a single byte */
	KT_ASSERT_ER(ts_zlib_deflate(in, 0, &z, &zl, 9), E_OK);
	KT_ASSERT_EQ(ts_zlib_inflate(z, zl, back, 16), 0);
	Kfree(z);
	KT_ASSERT_ER(ts_zlib_deflate(in, 1, &z, &zl, 9), E_OK);
	KT_ASSERT_EQ(ts_zlib_inflate(z, zl, back, 16), 1);
	KT_ASSERT_EQ(back[0], in[0]);
	Kfree(z);
	Kfree(in);
	Kfree(back);
}

/*
 * A document of three pages: the file's table and pages, the face
 * carried cut down, and each letter where the screen puts it.
 */
LOCAL void test_doc( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	T_PRNSTAT	st;
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		cn = 0, from = 0, e, f, g, nobj;
	T_DPRECT	r, box;

	start();
	if ( !model(doc3, &doc, &d) ) return;
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"印刷の試験", TRUE);
	KT_ASSERT_EQ(job.npage, 3);
	KT_ASSERT_EQ(job.lw, 595);
	KT_ASSERT_EQ(job.lh, 842);
	prn_default(&set);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf == NULL ) {
		dt_print_src_free(&s);
		tv_doc_free(d);
		tad_free(doc);
		return;
	}
	leave("/boot/PRNDOC.PDF", pdf, len);
	leave_screen("/boot/PRNDOC.PPM", &job, 0);
	prn_stat(&st);
	tm_printf((UB *)"  %d bytes, %d pages, %d faces of %d glyphs\n", (INT)len, st.pages, st.faces, st.glyphs);
	nobj = xref_check(pdf, (INT)len);
	KT_ASSERT(nobj > 10);
	KT_ASSERT_EQ(num_after(pdf, (INT)len, 0, "/Type /Pages /Count "), 3);
	KT_ASSERT_EQ(count(pdf, (INT)len, "/Type /Page /Parent"), 3);
	KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 595 842 ]") > 0);
	/* the face: of PostScript outlines, cut down, with its table of letters */
	KT_ASSERT(find(pdf, (INT)len, 0, "/Subtype /CIDFontType0C") > 0);
	KT_ASSERT(find(pdf, (INT)len, 0, "/Encoding /Identity-H") > 0);
	KT_ASSERT(find(pdf, (INT)len, 0, "/ToUnicode") > 0);
	KT_ASSERT(find(pdf, (INT)len, 0, "NotoSansJP-Regular") > 0);
	KT_ASSERT_EQ(st.faces, 1);
	KT_ASSERT(st.glyphs > 20 && st.glyphs < 80);
	KT_ASSERT(len < 120000);
	/* the title */
	KT_ASSERT(find(pdf, (INT)len, 0, "/Title <FEFF") > 0);

	/* the first letter of the page where the screen puts it */
	c = page_content(pdf, (INT)len, 0, &cn);
	KT_ASSERT(c != NULL);
	if ( c != NULL ) {
		KT_ASSERT(find(c, cn, 0, "q 1 0 0 -1 0 842 cm") == 0);
		KT_ASSERT(glyph_at(c, cn, &from, &e, &f, &g));
		r.left = 0;
		r.top = 0;
		r.right = 595;
		r.bottom = 842;
		{
			T_TVDOC	cp = *d;

			cp.page_h = 842;
			KT_ASSERT_ER(tv_doc_caret_box(&cp, &r, 0, doc, 0, 0, &box), E_OK);
		}
		tm_printf((UB *)"  first letter at %d.%03d, %d.%03d; the caret at %d, %d..%d\n",
			  e / 1000, e % 1000, f / 1000, f % 1000, box.left, box.top, box.bottom);
		KT_ASSERT(e / 1000 >= box.left - 1 && e / 1000 <= box.left + 1);
		KT_ASSERT(f / 1000 > box.top && f / 1000 < box.bottom);
		/* thickened letters are stroked, the red ones red */
		KT_ASSERT(find(c, cn, 0, "2 Tr") > 0);
		KT_ASSERT(find(c, cn, 0, "0.8 0 0 rg") > 0);
		/* the words in one string where they are on one line */
		KT_ASSERT(count(c, cn, "TJ") < 20);
		Kfree(c);
	}
	Kfree(pdf);
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/* 原稿用紙: the squares, and the letters down each line from the right */
LOCAL void test_vertical( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*d = NULL;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	UB		*pdf = NULL, *c, *raw, *words;
	SZ		len = 0, rl = 0;
	INT		cn = 0, from = 0, e0, f0, e1, f1, g, i, k, n = 0, fd;
	T_TADNODE	*p;
	T_FSTAT		fst;
	CONST char	*kanji = "\xE6\x9C\x88";		/* 月 */

	start();
	if ( fs_stat(XT_GENKO, &fst) < EX_OK ) {
		KT_SKIP("no 原稿用紙 on /boot");
		return;
	}
	raw = (UB *)Kmalloc((SZ)fst.size + 1);
	fd = fs_open(XT_GENKO, O_RDONLY);
	if ( raw == NULL || fd < 0 ) {
		if ( raw != NULL ) Kfree(raw);
		return;
	}
	rl = (SZ)fs_read(fd, raw, (SZ)fst.size);
	fs_close(fd);
	KT_ASSERT_ER(tad_parse(raw, rl, NULL, &doc), E_OK);
	Kfree(raw);
	if ( doc == NULL ) return;
	/* 30 letters: the first line and half the second */
	words = (UB *)Kmalloc(200);
	for ( i = 0; i < 30; i++ ) {
		for ( k = 0; k < 3; k++ ) words[n++] = (UB)kanji[k];
	}
	words[n] = 0;
	for ( p = tad_walk(doc, NULL); p != NULL; p = tad_walk(doc, p) ) {
		if ( p->kind == TAD_ND_ELEM && p->name != NULL && p->name[0] == 'p' && p->name[1] == 0 ) {
			break;
		}
	}
	if ( p != NULL ) {
		(void)tad_text_new(doc, p, NULL, words, n);
	}
	Kfree(words);
	KT_ASSERT_ER(tv_doc(doc, &d), E_OK);
	if ( d == NULL ) {
		tad_free(doc);
		return;
	}
	KT_ASSERT(d->vert);
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"原稿用紙", TRUE);
	KT_ASSERT_EQ(job.npage, 1);
	KT_ASSERT_EQ(job.lw, 842);
	prn_default(&set);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		leave("/boot/PRNGENK.PDF", pdf, len);
		leave_screen("/boot/PRNGENK.PPM", &job, 0);
		KT_ASSERT(xref_check(pdf, (INT)len) > 0);
		KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 842 595 ]") > 0);
		c = page_content(pdf, (INT)len, 0, &cn);
		KT_ASSERT(c != NULL);
		if ( c != NULL ) {
			/* the squares as lines, not pixels */
			KT_ASSERT(count(c, cn, "S\n") > 20);
			/* a letter a string, each down the line below the one before */
			KT_ASSERT(glyph_at(c, cn, &from, &e0, &f0, &g));
			KT_ASSERT(glyph_at(c, cn, &from, &e1, &f1, &g));
			tm_printf((UB *)"  down the line: %d,%d then %d,%d\n", e0 / 1000, f0 / 1000, e1 / 1000, f1 / 1000);
			KT_ASSERT(e1 == e0);
			KT_ASSERT(f1 - f0 == 22000);
			/* the first line's letters inside its squares */
			KT_ASSERT(e0 / 1000 >= 842 - 81 - 34 && e0 / 1000 < 842 - 81);
			KT_ASSERT(f0 / 1000 >= 78 && f0 / 1000 <= 78 + 22);
			Kfree(c);
		}
		Kfree(pdf);
	}
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/* A figure: shapes as paths, a pattern as a pattern, a second sheet below */
LOCAL void test_figure( void )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	T_PRNSTAT	st;
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		cn = 0;

	start();
	KT_ASSERT_ER(tad_parse((CONST UB *)fig1, s_len(fig1), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f == NULL ) {
		tad_free(doc);
		return;
	}
	dt_print_src_fig(&s, &job, f, doc, PAPER, (CONST UB *)"図形の試験", TRUE);
	/* A5, given ahead of the figure, is 560 by 793 of its pixels: the box at 1200 is on a second sheet */
	KT_ASSERT_EQ(job.lw, 560);
	KT_ASSERT_EQ(job.lh, 793);
	KT_ASSERT_EQ(job.npage, 2);
	KT_ASSERT_EQ(job.unit, 750);
	prn_default(&set);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		leave("/boot/PRNFIG.PDF", pdf, len);
		leave_screen("/boot/PRNFIG.PPM", &job, 0);
		prn_stat(&st);
		KT_ASSERT(xref_check(pdf, (INT)len) > 0);
		KT_ASSERT_EQ(st.pages, 2);
		/* the pixels of the figure made points: 0.75 a pixel */
		KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 420 594.75 ]") > 0);
		KT_ASSERT(st.patterns >= 1);
		KT_ASSERT(find(pdf, (INT)len, 0, "/PatternType 1") > 0);
		c = page_content(pdf, (INT)len, 0, &cn);
		KT_ASSERT(c != NULL);
		if ( c != NULL ) {
			KT_ASSERT(find(c, cn, 0, "q 0.75 0 0 -0.75 ") == 0);
			KT_ASSERT(find(c, cn, 0, "/Pattern cs /P") > 0);
			KT_ASSERT(count(c, cn, "h f") >= 2);		/* filled shapes as polygons */
			KT_ASSERT(find(c, cn, 0, "3 w") > 0);		/* the line three wide */
			KT_ASSERT(find(c, cn, 0, "[4 4] 0 d") > 0 || find(c, cn, 0, "[1 1] 0 d") > 0
				  || count(c, cn, " l S") > 10);	/* the broken line */
			KT_ASSERT(find(c, cn, 0, "TJ") > 0);		/* the words of the box */
			Kfree(c);
		}
		c = page_content(pdf, (INT)len, 1, &cn);
		KT_ASSERT(c != NULL);
		if ( c != NULL ) {
			KT_ASSERT(find(c, cn, 0, "re S") > 0 || count(c, cn, " l") >= 3);
			Kfree(c);
		}
		Kfree(pdf);
	}
	dt_print_src_free(&s);
	tv_fig_free(f);
	tad_free(doc);
}

/* Pictures: packed, a clear part as a soft mask, the same picture written once */
LOCAL ER draw_pictures( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	UW	*px = (UW *)arg;
	UB	bits[32];
	INT	i;

	(void)page;
	dp_fill_rect(gid, sheet, PAPER);
	dp_put_argb(gid, 50, 60, px, 40, 40, 30, IMG_CLEAR);
	dp_put_argb(gid, 150, 60, px, 40, 40, 30, IMG_CLEAR);
	for ( i = 0; i < 32; i++ ) bits[i] = (UB)( ( i & 1 ) ? 0x0F : 0xF0 );
	dp_put_mono(gid, 250, 60, bits, 2, 16, 16, 0x00008000U);
	return E_OK;
}

LOCAL void test_pictures( void )
{
	T_PRNJOB	job;
	T_PRNSET	set;
	T_PRNSTAT	st;
	UW		*px = (UW *)Kmalloc(sizeof(UW) * 40 * 30);
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		i, cn = 0;

	if ( px == NULL ) return;
	for ( i = 0; i < 40 * 30; i++ ) {
		px[i] = ( ( i % 40 ) < 5 ) ? IMG_CLEAR : ( ( ( i % 40 ) * 6 ) << 16 ) | ( ( ( i / 40 ) * 8 ) << 8 ) | 0x80;
	}
	knl_memset(&job, 0, sizeof(job));
	job.npage = 1;
	job.lw = 400;
	job.lh = 300;
	job.unit = 1000;
	job.name = (CONST UB *)"pictures";
	job.draw = draw_pictures;
	job.arg = px;
	prn_default(&set);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		leave("/boot/PRNPIC.PDF", pdf, len);
		prn_stat(&st);
		KT_ASSERT(xref_check(pdf, (INT)len) > 0);
		KT_ASSERT_EQ(st.images, 2);			/* drawn three times, two pictures */
		KT_ASSERT_EQ(count(pdf, (INT)len, "/Subtype /Image"), 3);	/* and one soft mask */
		KT_ASSERT(find(pdf, (INT)len, 0, "/SMask ") > 0);
		KT_ASSERT(find(pdf, (INT)len, 0, "/ImageMask true") > 0);
		KT_ASSERT(find(pdf, (INT)len, 0, "/Predictor 15") > 0);
		KT_ASSERT_EQ(count(pdf, (INT)len, "/FlateDecode"), count(pdf, (INT)len, "/Length ") - 0);
		c = page_content(pdf, (INT)len, 0, &cn);
		if ( c != NULL ) {
			KT_ASSERT_EQ(count(c, cn, " Do Q"), 3);
			KT_ASSERT(find(c, cn, 0, "q 40 0 0 -30 50 90 cm /Im") > 0);
			Kfree(c);
		}
		Kfree(pdf);
	}
	Kfree(px);
}

/* 印刷の指定: the pages and copies, the paper and its way round, greys */
LOCAL void test_settings( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		cn = 0, w, h, pct;

	start();
	if ( !model(doc3, &doc, &d) ) return;
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"指定", TRUE);
	prn_default(&set);
	set.all = FALSE;
	set.from = 2;
	set.to = 9;				/* past the end: to the last page */
	set.copies = 2;
	set.paper_w = 1820;			/* B5 */
	set.paper_h = 2570;
	set.colour = FALSE;
	set.rotate = TRUE;
	KT_ASSERT_EQ(prn_sheets(&job, &set), 4);
	KT_ASSERT_EQ(prn_sheet_page(&job, &set, 0), 1);
	KT_ASSERT_EQ(prn_sheet_page(&job, &set, 2), 1);
	KT_ASSERT_EQ(prn_sheet_page(&job, &set, 3), 2);
	prn_paper(&job, &set, &w, &h, &pct);
	tm_printf((UB *)"  B5 %d x %d thousandths of a point, %d.%02d%%\n", w, h, pct / 100, pct % 100);
	KT_ASSERT(pct >= 8640 && pct <= 8670);	/* A4 onto B5 */
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		leave("/boot/PRNSET.PDF", pdf, len);
		KT_ASSERT(xref_check(pdf, (INT)len) > 0);
		KT_ASSERT_EQ(num_after(pdf, (INT)len, 0, "/Type /Pages /Count "), 4);
		KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 515.905 728.503 ]") > 0);
		KT_ASSERT_EQ(count(pdf, (INT)len, "/Rotate 90"), 4);
		/* two drawings, each named by both copies */
		KT_ASSERT_EQ(count(pdf, (INT)len, "/Contents "), 4);
		c = page_content(pdf, (INT)len, 0, &cn);
		if ( c != NULL ) {
			KT_ASSERT(find(c, cn, 0, "q 0.8652 0 0 -0.8652 ") == 0);
			KT_ASSERT(find(c, cn, 0, " rg\n") < 0);	/* no colours, greys */
			KT_ASSERT(find(c, cn, 0, " g\n") > 0);
			Kfree(c);
		}
		Kfree(pdf);
	}
	/* a magnification given: from the corner, at that size */
	prn_default(&set);
	set.scale = 50;
	prn_paper(&job, &set, &w, &h, &pct);
	KT_ASSERT_EQ(pct, 5000);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		c = page_content(pdf, (INT)len, 0, &cn);
		if ( c != NULL ) {
			KT_ASSERT(find(c, cn, 0, "q 0.5 0 0 -0.5 0 842 cm") == 0);
			Kfree(c);
		}
		Kfree(pdf);
	}
	/* a range with no page in it */
	prn_default(&set);
	set.all = FALSE;
	set.from = 7;
	set.to = 8;
	KT_ASSERT_EQ(prn_sheets(&job, &set), 0);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_NOEXS);
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/* ヘッダ and フッタ: the name, the page of how many and the date filled in on each page */
LOCAL void test_header( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		cn = 0, k;

	start();
	if ( !model(dochead, &doc, &d) ) return;
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"報告書", TRUE);
	KT_ASSERT_EQ(job.npage, 3);
	prn_default(&set);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		leave("/boot/PRNHEAD.PDF", pdf, len);
		for ( k = 0; k < 3; k++ ) {
			c = page_content(pdf, (INT)len, k, &cn);
			KT_ASSERT(c != NULL);
			if ( c != NULL ) {
				/* the header at the top right, the footer at the foot */
				INT	from = 0, e, f, g, top = 842000, foot = 0;

				while ( glyph_at(c, cn, &from, &e, &f, &g) ) {
					if ( f < top ) top = f;
					if ( f > foot ) foot = f;
				}
				KT_ASSERT(top < 57000);
				KT_ASSERT(foot > 842000 - 57000);
				Kfree(c);
			}
		}
		Kfree(pdf);
	}
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/* 仮身: 印刷 off closes the text up where the link stood */
LOCAL void test_links( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	UB		*pdf = NULL, *c;
	SZ		len = 0;
	INT		cn = 0, from, e[2][2], f, g, k;

	start();
	if ( !model(doclink, &doc, &d) ) return;
	for ( k = 0; k < 2; k++ ) {
		dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"仮身", (BOOL)( k == 0 ));
		prn_default(&set);
		set.links = (BOOL)( k == 0 );
		e[k][0] = e[k][1] = -1;
		KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
		if ( pdf != NULL ) {
			if ( k == 0 ) leave("/boot/PRNLINK.PDF", pdf, len);
			c = page_content(pdf, (INT)len, 0, &cn);
			if ( c != NULL ) {
				/* 前 and 後: two strings when the link stands between them */
				/* the first letter, and the last: 後, after the link's name */
				from = 0;
				(void)glyph_at(c, cn, &from, &e[k][0], &f, &g);
				e[k][1] = e[k][0];
				while ( glyph_at(c, cn, &from, &e[k][1], &f, &g) ) ;
				Kfree(c);
			}
			Kfree(pdf);
		}
		dt_print_src_free(&s);
	}
	tm_printf((UB *)"  with the link the letters %d apart; without, %d\n",
		  ( e[0][1] - e[0][0] ) / 1000, ( e[1][1] - e[1][0] ) / 1000);
	KT_ASSERT(e[0][1] - e[0][0] > 120000);	/* the link between them */
	/* without it the two letters stand together, one string of glyphs */
	KT_ASSERT(e[1][1] == e[1][0]);
	KT_ASSERT(e[1][0] == e[0][0]);
	tv_doc_free(d);
	tad_free(doc);
}

/* The file as a real object, as a file taken in is, and given out again whole */
typedef struct {
	UB	*b;
	INT	n, max;
} OUT;

LOCAL INT out_write( void *ctx, CONST void *buf, SZ n )
{
	OUT	*o = (OUT *)ctx;

	if ( o->n + (INT)n > o->max ) return -1;
	knl_memcpy(o->b + o->n, buf, n);
	o->n += (INT)n;
	return (INT)n;
}

LOCAL void test_object( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	TS_UUID		u;
	UB		name[TAD_NAME_MAX], fname[XF_NAME_MAX];
	OUT		o;
	T_PRNSTAT	st;

	start();
	if ( !model(doc3, &doc, &d) ) return;
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"報告書", TRUE);
	prn_default(&set);
	KT_ASSERT_ER(prn_object(&job, &set, NULL, &u), E_OK);
	prn_stat(&st);
	KT_ASSERT(om_store_name(&u, name, sizeof(name)) > 0);
	tm_printf((UB *)"  the object: %s\n", name);
	KT_ASSERT(find(name, s_len((CONST char *)name), 0, "報告書") == 0);
	KT_ASSERT_EQ(xf_data_rec(&u), 1);
	o.max = 1024 * 1024;
	o.n = 0;
	o.b = (UB *)Kmalloc((SZ)o.max);
	if ( o.b != NULL ) {
		KT_ASSERT_ER(xf_export(&u, out_write, &o, fname, sizeof(fname)), E_OK);
		KT_ASSERT_EQ(o.n, (INT)st.bytes);
		KT_ASSERT(xref_check(o.b, o.n) > 0);
		KT_ASSERT(find(fname, s_len((CONST char *)fname), 0, ".pdf") > 0);
		Kfree(o.b);
	}
	(void)ob_del_obj(&u);
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/* 画面表示: the sheet drawn into pixels, its letters where the page has them */
LOCAL void test_sheet( void )
{
	T_TAD		*doc;
	T_TVDOC		*d;
	DTPRSRC		s;
	T_PRNJOB	job;
	T_PRNSET	set;
	UW		*px;
	INT		w = 298, h = 421, x, y, ink = 0, top = h, left = w;

	start();
	if ( !model(doc3, &doc, &d) ) return;
	dt_print_src_doc(&s, &job, d, doc, PAPER, (CONST UB *)"画面表示", TRUE);
	prn_default(&set);
	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	if ( px != NULL ) {
		KT_ASSERT_ER(prn_sheet(&job, &set, 0, px, w, h), E_OK);
		for ( y = 0; y < h; y++ ) {
			for ( x = 0; x < w; x++ ) {
				UW	p = px[y * w + x];

				if ( ( ( p >> 16 ) & 0xFF ) + ( ( p >> 8 ) & 0xFF ) + ( p & 0xFF ) < 3 * 160 ) {
					ink++;
					if ( y < top ) top = y;
					if ( x < left ) left = x;
				}
			}
		}
		tm_printf((UB *)"  %d dark pixels, the first at %d, %d\n", ink, left, top);
		KT_ASSERT(ink > 100);
		/* half size: the margins of 57 points are 28 pixels */
		KT_ASSERT(left >= 27 && left <= 31);
		KT_ASSERT(top >= 28 && top <= 40);
		/* the last page in grey */
		set.colour = FALSE;
		KT_ASSERT_ER(prn_sheet(&job, &set, 2, px, w, h), E_OK);
		Kfree(px);
	}
	dt_print_src_free(&s);
	tv_doc_free(d);
	tad_free(doc);
}

/*
 * Pages a process hands over: an object of its own with a page in each
 * record, a figure and a document, read and printed as the desktop
 * does for DT_RQ_PRINT.
 */
LOCAL void test_spool( void )
{
	T_OBCRE		cr;
	TS_UUID		u;
	ID		k;
	INT		rec = -1;
	SZ		asz = 0;
	T_PRNJOB	job;
	T_PRNSET	set;
	void		*sp;
	UB		*pdf = NULL;
	SZ		len = 0;
	CONST char	*m = "{\"name\":\"印刷の受け渡し\",\"refCount\":0}";

	start();
	knl_memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_FILE;
	cr.json = (CONST UB *)m;
	cr.jsonsz = s_len(m);
	KT_ASSERT_ER(ob_cre_obj(&cr, &u), E_OK);
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) return;
	KT_ASSERT_ER(ob_apd_rec(k, OB_RT_TAD, 0, &rec), E_OK);
	KT_ASSERT_ER(ob_wri_rec(k, rec, 0, fig1, s_len(fig1), &asz), E_OK);
	KT_ASSERT_ER(ob_apd_rec(k, OB_RT_TAD, 0, &rec), E_OK);
	KT_ASSERT_ER(ob_wri_rec(k, rec, 0, doc3, s_len(doc3), &asz), E_OK);
	ob_cls_obj(k);
	sp = dt_spool_open(&u, &job, (CONST UB *)"受け渡し");
	KT_ASSERT(sp != NULL);
	if ( sp != NULL ) {
		/* the figure one sheet, the document three */
		KT_ASSERT_EQ(job.npage, 4);
		KT_ASSERT_EQ(job.unit, 750);
		prn_default(&set);
		KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
		if ( pdf != NULL ) {
			leave("/boot/PRNSPL.PDF", pdf, len);
			KT_ASSERT(xref_check(pdf, (INT)len) > 0);
			KT_ASSERT_EQ(num_after(pdf, (INT)len, 0, "/Type /Pages /Count "), 4);
			Kfree(pdf);
		}
		dt_spool_close(sp);
	}
	(void)ob_del_obj(&u);
}

/* ---------------------------------------------------------------- the programs' pages */

/*
 * What a process does to print, done here with the same code: its
 * pages made into an object of pages (ui_prn_*), the object read as the
 * desktop reads it for DT_RQ_PRINT, and the file written. Answers the
 * file, the caller's to Kfree, or NULL.
 */
typedef struct {
	T_UIPRN		up;
	INT		pages;
} KPAGES;

LOCAL ER kp_put( void *arg, CONST UB *xml, INT n )
{
	T_TAD	*t = NULL;
	ER	er = tad_parse(xml, n, NULL, &t);

	/* each page reads as a record of its own */
	KT_ASSERT_ER(er, E_OK);
	if ( t != NULL ) {
		UINT	k = tv_kind(t);

		KT_ASSERT(k == TV_KIND_DOC || k == TV_KIND_FIG);
		if ( k == TV_KIND_FIG ) {
			T_TVFIG	*f = NULL;

			KT_ASSERT_ER(tv_fig(t, &f), E_OK);
			if ( f != NULL ) {
				tm_printf((UB *)"  a page of %d bytes: %d shapes, %d pieces of text\n", n, f->nsh, f->ndoc);
				tv_fig_free(f);
			}
		}
		tad_free(t);
	}
	return ui_prn_page(&((KPAGES *)arg)->up, xml, n);
}

LOCAL UB *pages_pdf( KPAGES *kp, CONST char *name, CONST char *path, SZ *p_len, INT *p_npage )
{
	T_PRNJOB	job;
	T_PRNSET	set;
	void		*sp;
	UB		*pdf = NULL;

	*p_npage = 0;
	KT_ASSERT_ER(ui_prn_ready(&kp->up), E_OK);
	sp = dt_spool_open(&kp->up.obj, &job, (CONST UB *)name);
	KT_ASSERT(sp != NULL);
	if ( sp != NULL ) {
		*p_npage = job.npage;
		prn_default(&set);
		KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, p_len), E_OK);
		if ( pdf != NULL ) {
			leave(path, pdf, *p_len);
			KT_ASSERT(xref_check(pdf, (INT)*p_len) > 0);
		}
		dt_spool_close(sp);
	}
	ui_prn_close(&kp->up);
	return pdf;
}

/* マイクロカード: the list, a card a page of text, and labels as a figure */
LOCAL void test_mcard( void )
{
	static CONST char *CONST names[3] = { "氏名", "郵便番号", "住所" };
	static CONST INT types[3] = { MC_T_TEXT, MC_T_TEXT, MC_T_TEXT };
	static CONST INT maxes[3] = { 20, 8, 40 };
	static CONST char *CONST rows[][3] = {
		{ "岡田　勇作", "146-0082", "東京都大田区石川町9-5-12" },
		{ "鈴木　亨", "160-0005", "東京都新宿区一番町5-1-30" },
		{ "山田　花子", "100-0001", "東京都千代田区1-1" },
	};
	static CONST char letter[] = "<tad><document><p>%(氏名)様</p><p>〒%2C %3C</p><p>%P/%T</p></document></tad>";
	static CONST char label[] = "<tad><figure>"
		"<document><docView viewleft=\"10\" viewtop=\"10\" viewright=\"280\" viewbottom=\"40\"/>%(氏名)　様</document>"
		"<document><docView viewleft=\"10\" viewtop=\"40\" viewright=\"280\" viewbottom=\"70\"/>〒%2C</document>"
		"</figure></tad>";
	MCDB		*db = (MCDB *)Kmalloc(sizeof(MCDB));
	UB		*work = (UB *)Kmalloc(256 * 1024), *page = (UB *)Kmalloc(256 * 1024), *pdf;
	MCMINFO		mi;
	MCPRINT		p;
	MCSINK		sink;
	MCPSINK		ps;
	KPAGES		kp;
	INT		idx[8], n, k, i, np;
	SZ		len;

	start();
	if ( db == NULL || work == NULL || page == NULL ) return;
	mc_init(db);
	for ( k = 0; k < 3; k++ ) {
		MCFIELD	*f = &db->f[k];

		knl_memset(f, 0, sizeof(*f));
		for ( i = 0; names[k][i] != 0; i++ ) f->name[i] = (UB)names[k][i];
		f->type = types[k];
		f->max = maxes[k];
		f->width = 10;
		f->face = MC_FACE_MINCHO;
	}
	db->nf = 3;
	for ( i = 0; i < 3; i++ ) {
		MCCARD	*c = mc_card_new(db, db->n);

		for ( k = 0; c != NULL && k < 3; k++ ) {
			(void)mc_set(db, c, k, (CONST UB *)rows[i][k], s_len(rows[i][k]));
		}
	}
	n = mc_targets(db, FALSE, 0, idx, 8);
	KT_ASSERT_EQ(n, 3);
	knl_memset(&mi, 0, sizeof(mi));
	knl_strcpy((char *)mi.name, "名簿");
	mi.y = 2026;
	mi.m = 10;
	mi.d = 4;

	/* a letter for each card: three pages of text on the 印刷設定's paper */
	mc_print_default(&p, (CONST UB *)"案内状");
	mi.pages = mc_pages(db, &p, FALSE, FALSE, n);
	KT_ASSERT_ER(ui_prn_open(&kp.up, NULL), E_OK);
	mc_prn_sink(&sink, &ps, &p, page, 256 * 1024, kp_put, &kp);
	KT_ASSERT_ER(mc_merge(db, &p, (CONST UB *)letter, sizeof(letter) - 1, idx, n, &mi, &sink, work,
			      256 * 1024), E_OK);
	KT_ASSERT_EQ(ps.pages, 3);
	pdf = pages_pdf(&kp, "名簿", "/boot/PRNMCDOC.PDF", &len, &np);
	KT_ASSERT_EQ(np, 3);
	if ( pdf != NULL ) {
		KT_ASSERT_EQ(num_after(pdf, (INT)len, 0, "/Type /Pages /Count "), 3);
		KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 595 842 ]") > 0);
		Kfree(pdf);
	}

	/* labels: two across, a figure; and the list as it is shown */
	mc_print_default(&p, (CONST UB *)"宛名ラベル");
	p.across = 2;
	p.down = 4;
	mi.pages = mc_pages(db, &p, TRUE, FALSE, n);
	KT_ASSERT_ER(ui_prn_open(&kp.up, NULL), E_OK);
	mc_prn_sink(&sink, &ps, &p, page, 256 * 1024, kp_put, &kp);
	KT_ASSERT_ER(mc_merge(db, &p, (CONST UB *)label, sizeof(label) - 1, idx, n, &mi, &sink, work,
			      256 * 1024), E_OK);
	KT_ASSERT_EQ(ps.pages, 1);
	mc_print_default(&p, (CONST UB *)"");
	mc_prn_sink(&sink, &ps, &p, page, 256 * 1024, kp_put, &kp);
	KT_ASSERT_ER(mc_merge(db, &p, NULL, 0, idx, n, &mi, &sink, work, 256 * 1024), E_OK);
	pdf = pages_pdf(&kp, "名簿の印刷", "/boot/PRNMCFIG.PDF", &len, &np);
	KT_ASSERT_EQ(np, 2);
	if ( pdf != NULL ) {
		INT	cn = 0;
		UB	*c = page_content(pdf, (INT)len, 0, &cn);

		/* a label's letters in a string of their own */
		if ( c != NULL ) {
			KT_ASSERT(count(c, cn, "TJ") >= 6);
			Kfree(c);
		}
		Kfree(pdf);
	}
	mc_clear(db);
	Kfree(db);
	Kfree(work);
	Kfree(page);
}

/* 基本表計算: the cells as figures, the range of those not empty */
LOCAL void test_calc( void )
{
	CXSHEET		*s = (CXSHEET *)Kmalloc(sizeof(CXSHEET));
	UB		*page = (UB *)Kmalloc(1024 * 1024), *pdf, *c;
	CXPRINT		pr;
	CXRANGE		rg;
	KPAGES		kp;
	INT		row, np, cn = 0;
	SZ		len;
	static CONST char *CONST heads[3] = { "品名", "数量", "金額" };

	start();
	if ( s == NULL || page == NULL ) return;
	cx_init(s);
	for ( row = 0; row < 3; row++ ) {
		CXCELL	*h = cx_make(s, 2 + row, 2);

		if ( h != NULL ) {
			CXSTYLE	st = *h->st;

			(void)cx_set_text(h, (CONST UB *)heads[row]);
			st.has |= CX_S_BOLD | CX_S_BG | CX_S_FRAME;
			knl_strcpy(st.bg, "#e0e8ff");
			st.side[CX_BOTTOM].on = TRUE;
			st.side[CX_BOTTOM].width = 2;
			knl_strcpy(st.side[CX_BOTTOM].colour, "#000000");
			KT_ASSERT(cx_style_put(h, &st));
		}
	}
	/* 120 rows: more than one page of A4 */
	for ( row = 3; row < 123; row++ ) {
		CXCELL	*a = cx_make(s, 2, row), *b = cx_make(s, 3, row);
		UB	num[12];
		INT	v = row * 7, k = 0, m = 0;
		UB	t[12];

		do {
			t[m++] = (UB)( '0' + v % 10 );
			v /= 10;
		} while ( v > 0 );
		while ( m > 0 ) num[k++] = t[--m];
		num[k] = 0;
		if ( a != NULL ) (void)cx_set_text(a, (CONST UB *)"りんご");
		if ( b != NULL ) {
			CXSTYLE	st = *b->st;

			(void)cx_set_text(b, num);
			st.has |= CX_S_ALIGN;
			knl_strcpy(st.align, "right");
			KT_ASSERT(cx_style_put(b, &st));
		}
	}
	KT_ASSERT(cx_print_range(s, &rg));
	KT_ASSERT(rg.c0 == 2 && rg.r0 == 2 && rg.c1 == 4 && rg.r1 == 122);
	KT_ASSERT_ER(ui_prn_open(&kp.up, NULL), E_OK);
	knl_memset(&pr, 0, sizeof(pr));
	pr.put = kp_put;
	pr.arg = &kp;
	pr.mem = page;
	pr.max = 1024 * 1024;
	np = cx_print(s, &pr);
	tm_printf((UB *)"  %d pages of cells\n", np);
	KT_ASSERT_EQ(np, 3);			/* 121 rows of 22 dots, 966 dots a page */
	pdf = pages_pdf(&kp, "売上表", "/boot/PRNCALC.PDF", &len, &np);
	KT_ASSERT_EQ(np, 3);
	if ( pdf != NULL ) {
		c = page_content(pdf, (INT)len, 0, &cn);
		if ( c != NULL ) {
			KT_ASSERT(find(c, cn, 0, "2 Tr") > 0);		/* the heads thickened */
			KT_ASSERT(find(c, cn, 0, "re f") > 0);		/* their ground */
			KT_ASSERT(count(c, cn, "TJ") > 20);
			Kfree(c);
		}
		Kfree(pdf);
	}
	cx_free_all(s);
	Kfree(s);
	Kfree(page);
}

/* 予定表: a month, the grid and a day's link */
LOCAL void test_calendar( void )
{
	UB		*x = (UB *)Kmalloc(512 * 1024), *page = (UB *)Kmalloc(512 * 1024), *pdf;
	CTBUF		b, pb;
	CTHOL		h[1];
	KPAGES		kp;
	INT		np;
	SZ		len;

	start();
	if ( x == NULL || page == NULL ) return;
	h[0].y = 2026;
	h[0].m = 11;
	h[0].d = 3;
	knl_strcpy((char *)h[0].name, "文化の日");
	ct_buf(&b, x, 512 * 1024);
	KT_ASSERT_ER(ct_month_xml(&b, 2026, 11, 0, h, 1, NULL, 0), E_OK);
	ct_buf(&pb, page, 512 * 1024);
	KT_ASSERT_ER(ct_print_page(&pb, b.p, b.n), E_OK);
	KT_ASSERT(find(pb.p, pb.n, 0, "<paper type=\"fig\" length=\"450\" width=\"600\"") > 0);
	KT_ASSERT_ER(ui_prn_open(&kp.up, NULL), E_OK);
	KT_ASSERT_ER(ui_prn_page(&kp.up, pb.p, pb.n), E_OK);
	pdf = pages_pdf(&kp, "予定表", "/boot/PRNCAL.PDF", &len, &np);
	KT_ASSERT_EQ(np, 1);
	if ( pdf != NULL ) {
		INT	cn = 0;
		UB	*c = page_content(pdf, (INT)len, 0, &cn);

		KT_ASSERT(find(pdf, (INT)len, 0, "/MediaBox [ 0 0 600 450 ]") > 0);
		if ( c != NULL ) {
			KT_ASSERT(count(c, cn, "TJ") > 30);	/* the days' numbers */
			Kfree(c);
		}
		Kfree(pdf);
	}
	Kfree(x);
	Kfree(page);
}

EXPORT void ktest_prn( void )
{
	KT_RUN(test_deflate);
	KT_RUN(test_doc);
	KT_RUN(test_vertical);
	KT_RUN(test_figure);
	KT_RUN(test_pictures);
	KT_RUN(test_settings);
	KT_RUN(test_header);
	KT_RUN(test_links);
	KT_RUN(test_object);
	KT_RUN(test_sheet);
	KT_RUN(test_spool);
	KT_RUN(test_mcard);
	KT_RUN(test_calc);
	KT_RUN(test_calendar);
}
