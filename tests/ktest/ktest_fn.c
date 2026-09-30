/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_fn.c
 *	Letters on the screen (design 16.6 stage 9c)
 *
 *	A font is read off the test disk and opened, a size is set, and a
 *	string is drawn into a window. What is checked is that the pieces
 *	agree with one another: the width a string measures is the width it
 *	draws, what is drawn lands inside the rectangle the metrics
 *	promised, and a string cut to a width fits in that width.
 *
 *	No font is kept in the repository -- a typeface is somebody's asset
 *	(design 17.1) -- so these tests skip when there is none on the
 *	disk. Put one in etc/font/ to run them.
 */

#include "ktest.h"
#include <ts/fn.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/disp.h>
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/xf.h>
#include <ts/sysdef.h>

/*
 * The system's faces, objects in the 書体箱 by the names of their files:
 * a TrueType face when one was put in etc/font/, else the OpenType face
 * the repository carries
 */
#define TTFFILE		"koganei.ttf"
#define OTFFILE		"NotoSansJP-Regular.otf"
LOCAL CONST char	*font_file = TTFFILE;
#define FONTFILE	font_file
#define PX		16

LOCAL UB	*blob = NULL;
LOCAL SZ	blobsz = 0;
LOCAL ID	fid = 0;
LOCAL ID	sysfid = 0;		/* the face the system draws words with */
LOCAL INT	wid = 0;
LOCAL BOOL	ready = FALSE;
LOCAL T_DISPSPEC spec;

LOCAL UW at( INT x, INT y )
{
	UB	*p = (UB *)ts_disp_buffer();

	return *(UW *)(p + (UBINT)y * spec.pitch + (UBINT)x * 4) & 0x00FFFFFF;
}

/* the font comes off the disk and opens */
LOCAL void test_open( void )
{
	TS_UUID	u, ou;

	if ( kt_sysobj(SYSDEF_FONT_BOX, TTFFILE, &u) >= E_OK ) {
		font_file = TTFFILE;
	} else if ( kt_sysobj(SYSDEF_FONT_BOX, OTFFILE, &u) >= E_OK ) {
		font_file = OTFFILE;
	} else {
		KT_SKIP("no font on the disk (put one in etc/font/)");
	}
	blob = kt_obj_data(&u, &blobsz);
	KT_ASSERT(blob != NULL && blobsz > 0);
	if ( blob == NULL ) return;

	KT_ASSERT_ER(fn_open_mem(blob, blobsz, 0, &fid), E_OK);
	KT_ASSERT(fid > 0);
	ready = TRUE;

	KT_ASSERT_ER(fn_set_size(fid, PX), E_OK);

	/*
	 * What the system draws words with has to be a face that holds the
	 * writing system, not merely a face. The names of things, the words
	 * on a panel and a window's own name are written in Japanese as
	 * often as not, and a face with only the Latin letters in it draws
	 * nothing at all where the rest should be. So the face read as a
	 * stream from the larger font is preferred, and the small one is
	 * kept only as what to fall back on.
	 */
	if ( kt_sysobj(SYSDEF_FONT_BOX, OTFFILE, &ou) >= E_OK
	  && fn_open_obj(&ou, xf_data_rec(&ou), 0, &sysfid) >= E_OK && sysfid > 0 ) {
		KT_ASSERT_ER(fn_set_size(sysfid, PX), E_OK);
		KT_ASSERT_ER(fn_set_system(sysfid), E_OK);
	} else {
		KT_ASSERT_ER(fn_set_system(fid), E_OK);
		KT_ASSERT_EQ((INT)fn_system(), (INT)fid);
	}
	KT_ASSERT(fn_system() > 0);
	KT_ASSERT_ER(fn_set_system(99), E_ID);

	/* what is not a font is refused */
	{
		ID	other = 0;
		UB	junk[64];
		INT	i;

		for ( i = 0; i < 64; i++ ) junk[i] = (UB)i;
		KT_ASSERT_ER(fn_open_mem(junk, sizeof(junk), 0, &other), E_OBJ);
		KT_ASSERT_ER(fn_open_mem(NULL, 10, 0, &other), E_PAR);
	}
}

/* a size gives measurements that hang together */
LOCAL void test_metrics( void )
{
	T_FNMET	met;

	if ( !ready ) KT_SKIP("no font");

	/* a face with no size yet has nothing to measure */
	{
		ID	fresh = 0;

		if ( fn_open_mem(blob, blobsz, 0, &fresh) == E_OK ) {
			KT_ASSERT_ER(fn_metrics(fresh, &met), E_PAR);
			KT_ASSERT_ER(fn_close(fresh), E_OK);
		}
	}
	KT_ASSERT_ER(fn_set_size(fid, PX), E_OK);
	KT_ASSERT_ER(fn_set_size(fid, 0), E_PAR);
	KT_ASSERT_ER(fn_set_size(fid, 10000), E_PAR);

	KT_ASSERT_ER(fn_metrics(fid, &met), E_OK);
	KT_ASSERT(met.ascent > 0);
	KT_ASSERT(met.descent >= 0);
	KT_ASSERT(met.height >= met.ascent);
	KT_ASSERT(met.height <= PX * 3);
	KT_ASSERT(met.max_advance > 0);
}

/* a string measures the same width it draws, and lands where it should */
LOCAL void test_draw( void )
{
	T_DPRECT	o;
	T_FNMET		met;
	INT		gid, w1, w2, drawn, x, y, i, found = 0;

	if ( !ready ) KT_SKIP("no font");
	if ( ts_disp_ref(&spec) < E_OK ) KT_SKIP("the machine has no screen");

	o.left = 80;  o.top = 80;  o.right = 480;  o.bottom = 260;
	wid = wm_open(&o, 0, "moji");		/* no frame: the work area is all of it */
	if ( wid < 0 ) KT_SKIP("windows did not start");
	gid = wm_gid(wid);
	KT_ASSERT(gid >= 0);
	if ( gid < 0 ) return;

	KT_ASSERT_ER(fn_metrics(fid, &met), E_OK);

	w1 = fn_width(fid, (CONST UB *)"AV");
	KT_ASSERT(w1 > 0);
	w2 = fn_width(fid, (CONST UB *)"AVAV");
	KT_ASSERT_EQ(w2, w1 * 2);		/* twice the text, twice the width */
	KT_ASSERT_EQ(fn_width(fid, (CONST UB *)""), 0);

	/* the pen moves as far as the measurement said it would */
	x = 10;
	y = 10 + met.ascent;
	drawn = fn_draw(gid, fid, x, y, (CONST UB *)"AV", 0x00000000U);
	KT_ASSERT_EQ(drawn, w1);

	KT_ASSERT_ER(wm_composite(), E_OK);

	/*
	 * Something was put down inside the line the metrics promised, and
	 * nothing was put down above it.
	 */
	for ( i = 0; i < w1; i++ ) {
		INT	ry;

		for ( ry = 0; ry < met.ascent + met.descent; ry++ ) {
			if ( at(o.left + x + i, o.top + 10 + ry) == 0 ) {
				found++;
			}
		}
	}
	KT_ASSERT(found > 0);

	/* above the line: the work area's own colour, untouched */
	KT_ASSERT_EQ((INT)at(o.left + x, o.top + 2), (INT)wm_look(WM_LOOK_WORK));
}

/* a string cut to a width fits in that width */
LOCAL void test_fit( void )
{
	INT	full, half, n;
	CONST UB *s = (CONST UB *)"AVAVAVAV";

	if ( !ready ) KT_SKIP("no font");

	full = fn_width(fid, s);
	KT_ASSERT(full > 0);

	KT_ASSERT_EQ(fn_fit(fid, s, 0), 0);		/* nothing fits in nothing */
	n = fn_fit(fid, s, full);
	KT_ASSERT_EQ(n, 8);				/* all of it fits in its own width */

	half = fn_fit(fid, s, full / 2);
	KT_ASSERT(half > 0);
	KT_ASSERT(half < 8);
	{
		UB	cut[16];
		INT	i;

		for ( i = 0; i < half; i++ ) cut[i] = s[i];
		cut[half] = '\0';
		KT_ASSERT(fn_width(fid, cut) <= full / 2);
	}
}

/* what is opened is given back, and the table says when it is full */
LOCAL void test_close( void )
{
	ID	more[FN_MAX_FONT + 1];
	INT	i, n = 0;

	if ( !ready ) KT_SKIP("no font");

	/* the table fills up rather than overwriting something */
	for ( i = 0; i < FN_MAX_FONT; i++ ) {
		if ( fn_open_mem(blob, blobsz, 0, &more[i]) == E_OK ) {
			n++;
		}
	}
	{
		ID	over = 0;

		KT_ASSERT_ER(fn_open_mem(blob, blobsz, 0, &over), E_LIMIT);
	}
	for ( i = 0; i < n; i++ ) {
		KT_ASSERT_ER(fn_close(more[i]), E_OK);
	}

	/*
	 * The face this opened stays: it is the system's font now, and the
	 * tests that follow draw with it. What was taken for this test is
	 * given back, and nothing else.
	 */
	KT_ASSERT_ER(fn_close(99), E_ID);

	if ( wid > 0 ) {
		wm_close(wid);
		wid = 0;
	}
}

/*
 * An OpenType font with PostScript outlines opens as well as one with
 * TrueType outlines, and a character outside Latin draws. Those are two
 * different drivers inside FreeType, and the point of building both in
 * is that the layer above does not have to know which it got.
 */
LOCAL void test_opentype( void )
{
	TS_UUID	u;
	UB	*otf = NULL;
	SZ	size = 0;
	ID	ofid = 0;
	INT	n, w;

	if ( kt_sysobj(SYSDEF_FONT_BOX, OTFFILE, &u) < E_OK ) {
		KT_SKIP("no OpenType font on the disk");
	}
	otf = kt_obj_data(&u, &size);
	if ( otf == NULL ) {
		KT_SKIP("no room for a font of this size");
	}

	KT_ASSERT_ER(fn_open_mem(otf, size, 0, &ofid), E_OK);
	if ( ofid <= 0 ) { Kfree(otf); return; }
	KT_ASSERT_ER(fn_set_size(ofid, PX), E_OK);

	{
		T_FNMET	met;

		KT_ASSERT_ER(fn_metrics(ofid, &met), E_OK);
		KT_ASSERT(met.ascent > 0);
		KT_ASSERT(met.height >= met.ascent);
	}
	/* Latin */
	w = fn_width(ofid, (CONST UB *)"AV");
	KT_ASSERT(w > 0);

	/* and a character that only a CJK font has: U+65E5 in UTF-8 */
	{
		UB	kanji[4];

		kanji[0] = 0xE6;  kanji[1] = 0x97;  kanji[2] = 0xA5;  kanji[3] = 0;
		n = fn_width(ofid, kanji);
		KT_ASSERT(n > 0);
		/* a full-width character is wider than a Latin capital */
		KT_ASSERT(n > w / 2);
	}

	KT_ASSERT_ER(fn_close(ofid), E_OK);
	Kfree(otf);
}

/*
 * A face out of a file, read as it is needed. The same font opened both
 * ways has to measure the same: if the stream answered differently from
 * the block of bytes, text would move when the way it was opened
 * changed.
 */
LOCAL void test_stream( void )
{
	ID	sfid = 0;
	INT	w_mem, w_file;
	T_FNMET	m1, m2;
	TS_UUID	u;

	if ( !ready ) KT_SKIP("no font");

	/* the same face read as a stream from its object's record */
	KT_ASSERT_ER(kt_sysobj(SYSDEF_FONT_BOX, FONTFILE, &u), E_OK);
	KT_ASSERT_ER(fn_open_obj(&u, xf_data_rec(&u), 0, &sfid), E_OK);
	if ( sfid <= 0 ) return;
	KT_ASSERT_ER(fn_set_size(sfid, PX), E_OK);

	KT_ASSERT_ER(fn_metrics(fid, &m1), E_OK);
	KT_ASSERT_ER(fn_metrics(sfid, &m2), E_OK);
	KT_ASSERT_EQ(m2.ascent, m1.ascent);
	KT_ASSERT_EQ(m2.descent, m1.descent);
	KT_ASSERT_EQ(m2.height, m1.height);

	w_mem  = fn_width(fid, (CONST UB *)"AVAV");
	w_file = fn_width(sfid, (CONST UB *)"AVAV");
	KT_ASSERT_EQ(w_file, w_mem);

	KT_ASSERT_ER(fn_close(sfid), E_OK);

	/* a path that is not there is not a font */
	KT_ASSERT_ER(fn_open_file("/boot/NOSUCH.TTF", 0, &sfid), E_NOEXS);
	KT_ASSERT_ER(fn_open_file(NULL, 0, &sfid), E_PAR);
}

/*
 * Words in Japanese come out. The text is UTF-8, so a character is one
 * to three bytes and the walk has to step by characters: a walk that
 * stepped by bytes would ask the face for a glyph at a byte in the
 * middle of a character and draw whatever that came to.
 */
LOCAL void test_japanese( void )
{
	CONST UB	*jp = (CONST UB *)"TessronOS\u03b1\u7248";
	INT		w_all, w_latin, n;

	if ( fn_system() <= 0 ) KT_SKIP("no system font");

	KT_ASSERT_ER(fn_set_size(fn_system(), 16), E_OK);
	w_latin = fn_width(fn_system(), (CONST UB *)"TessronOS");
	w_all   = fn_width(fn_system(), jp);
	KT_ASSERT(w_latin > 0);

	/* the two characters after the Latin part add to its width */
	KT_ASSERT(w_all > w_latin);

	/*
	 * What fits is counted in bytes, and the count always falls
	 * between characters: cutting inside one would leave a byte that
	 * is not a character behind it.
	 */
	n = fn_fit(fn_system(), jp, w_all);
	KT_ASSERT_EQ(n, 14);			/* 9 + 2 + 3 bytes */
	n = fn_fit(fn_system(), jp, w_latin);
	KT_ASSERT_EQ(n, 9);
	n = fn_fit(fn_system(), jp, w_latin + 1);
	KT_ASSERT_EQ(n, 9);			/* not 10: that is half a character */
}

EXPORT void ktest_fn( void )
{
	KT_RUN(test_open);
	KT_RUN(test_metrics);
	KT_RUN(test_draw);
	KT_RUN(test_fit);
	KT_RUN(test_opentype);
	KT_RUN(test_stream);
	KT_RUN(test_japanese);
	KT_RUN(test_close);
}
