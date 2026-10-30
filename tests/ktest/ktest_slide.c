/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_slide.c
 *	スライド (design 11.11): which objects a document's slides are, the
 *	screen they are made for, and where they go in a window
 *
 *	The slides are the links of the document in the order they stand;
 *	a first paragraph "%size:W:H" says the screen they are made for,
 *	else it is 1920 by 1080. The sample of TADjs Desktop in the system
 *	(実時間TADデモ) is read as it is on the disk: three slides.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include "../../application/desktop/desktop.h"

#define DECK		"72c584f2-af6c-45cb-8dfc-3f181d13cd0a"

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

/* The slides of a document given as text: how many, and its size */
LOCAL INT pages_of( CONST char *xml, TS_UUID *ids, INT max, INT *p_w, INT *p_h )
{
	T_TAD	*doc = NULL;
	INT	n;

	if ( tad_parse((CONST UB *)xml, s_len(xml), NULL, &doc) < E_OK || doc == NULL ) return -1;
	n = dt_slide_pages(doc, ids, max, p_w, p_h);
	tad_free(doc);
	return n;
}

LOCAL void test_pages( void )
{
	static CONST char	sized[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
		"<p><font size=\"18\">%size:1280:720</font></p>"
		"<p><link id=\"019d0000-0000-7000-8000-000000000011_0.xtad\" vobjid=\"019d0000-0000-7000-8000-0000000000f1\"/></p>"
		"<p>次は<link id=\"019d0000-0000-7000-8000-000000000012_0.xtad\" vobjid=\"019d0000-0000-7000-8000-0000000000f2\"/>"
		"と<link id=\"019d0000-0000-7000-8000-000000000013_0.xtad\" vobjid=\"019d0000-0000-7000-8000-0000000000f3\"/></p>"
		"</document></tad>";
	static CONST char	star[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>\n<font size=\"18\">%size:1024＊768</font>\n</p>"
		"<p><link id=\"019d0000-0000-7000-8000-000000000011_0.xtad\"/></p></document></tad>";
	static CONST char	plain[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>発表 %size:800:600</p>"
		"<p><link id=\"019d0000-0000-7000-8000-000000000011_0.xtad\"/></p></document></tad>";
	static CONST char	none[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>まだ何もない</p></document></tad>";
	TS_UUID	ids[4], u;
	INT	w = 0, h = 0;

	KT_ASSERT_EQ(pages_of(sized, ids, 4, &w, &h), 3);
	KT_ASSERT_EQ(w, 1280);
	KT_ASSERT_EQ(h, 720);
	KT_ASSERT_ER(ts_str_to_uuid("019d0000-0000-7000-8000-000000000012", &u), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[1], &u), 0);	/* in the order they stand */
	KT_ASSERT_EQ(pages_of(sized, ids, 2, &w, &h), 2);	/* no more than there is room for */

	KT_ASSERT_EQ(pages_of(star, ids, 4, &w, &h), 1);
	KT_ASSERT_EQ(w, 1024);
	KT_ASSERT_EQ(h, 768);

	/* a size not alone in its paragraph says nothing */
	KT_ASSERT_EQ(pages_of(plain, ids, 4, &w, &h), 1);
	KT_ASSERT_EQ(w, 1920);
	KT_ASSERT_EQ(h, 1080);

	KT_ASSERT_EQ(pages_of(none, ids, 4, &w, &h), 0);
}

LOCAL void test_fit( void )
{
	T_DPRECT	r;
	INT		den;

	/* a screen 1280 by 800: two thirds, as wide as the screen, the margin above and below only */
	KT_ASSERT_EQ(dt_slide_fit(1280, 800, 1920, 1080, &r, &den), 2);
	KT_ASSERT_EQ(den, 3);
	KT_ASSERT_EQ(r.left, 0);
	KT_ASSERT_EQ(r.top, 40);
	KT_ASSERT_EQ(r.right - r.left, 1280);
	KT_ASSERT_EQ(r.bottom - r.top, 720);

	/* the narrower way decides: here the height, the margin left and right only */
	KT_ASSERT_EQ(dt_slide_fit(1280, 600, 1920, 1080, &r, &den), 5);
	KT_ASSERT_EQ(den, 9);
	KT_ASSERT_EQ(r.top, 0);
	KT_ASSERT_EQ(r.bottom - r.top, 600);
	KT_ASSERT_EQ(r.right - r.left, 1066);
	KT_ASSERT_EQ(r.left, ( 1280 - 1066 ) / 2);

	/* a screen of the slides' own shape is filled whole, at any size */
	KT_ASSERT_EQ(dt_slide_fit(1366, 768, 1366, 768, &r, &den), 1);
	KT_ASSERT_EQ(den, 1);
	KT_ASSERT_EQ(dt_slide_fit(1600, 900, 1920, 1080, &r, &den), 5);
	KT_ASSERT_EQ(den, 6);
	KT_ASSERT_EQ(r.left, 0);
	KT_ASSERT_EQ(r.top, 0);
	KT_ASSERT_EQ(r.right, 1600);
	KT_ASSERT_EQ(r.bottom, 900);

	/* never smaller than an eighth, nor larger than four times */
	KT_ASSERT_EQ(dt_slide_fit(100, 100, 1920, 1080, &r, &den), 1);
	KT_ASSERT_EQ(den, 8);
	KT_ASSERT_EQ(dt_slide_fit(1920, 1080, 960, 540, &r, &den), 2);
	KT_ASSERT_EQ(den, 1);
	KT_ASSERT_EQ(dt_slide_fit(3840, 2160, 480, 270, &r, &den), 4);
	KT_ASSERT_EQ(den, 1);
	KT_ASSERT_EQ(r.right - r.left, 1920);
	KT_ASSERT_EQ(r.left, 960);

	/* no size given: 1920 by 1080 */
	KT_ASSERT_EQ(dt_slide_fit(1024, 768, 0, 0, &r, &den), 8);
	KT_ASSERT_EQ(den, 15);
	KT_ASSERT_EQ(r.right - r.left, 1024);
	KT_ASSERT_EQ(r.bottom - r.top, 576);
}

/* the sample in the system, as it is on the disk */
LOCAL void test_sample( void )
{
	TS_UUID	deck, ids[8], u;
	T_TAD	*doc = NULL;
	UB	*rec;
	SZ	size = 0;
	INT	n, w = 0, h = 0;

	KT_ASSERT_ER(ts_str_to_uuid(DECK, &deck), E_OK);
	rec = om_obj_record(&deck, 0, &size);
	if ( rec == NULL ) KT_SKIP("the sample is not on this disk");
	KT_ASSERT_ER(tad_parse(rec, size, NULL, &doc), E_OK);
	Kfree(rec);
	if ( doc == NULL ) return;
	n = dt_slide_pages(doc, ids, 8, &w, &h);
	tad_free(doc);
	tm_printf((UB *)"  sample: %d slides, %d x %d\n", n, w, h);
	KT_ASSERT_EQ(n, 3);
	KT_ASSERT_EQ(w, 1920);
	KT_ASSERT_EQ(h, 1080);
	KT_ASSERT_ER(ts_str_to_uuid("a529abdf-ce53-456c-9be1-58119b3c40cd", &u), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[0], &u), 0);
	KT_ASSERT_ER(ts_str_to_uuid("713b8f42-b7f6-42a8-979e-90af8b824d70", &u), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&ids[2], &u), 0);
}

EXPORT void ktest_slide( void )
{
	tm_printf((UB *)"\n[slide] スライド (design 11.11)\n");
	KT_RUN(test_pages);
	KT_RUN(test_fit);
	KT_RUN(test_sample);
}
