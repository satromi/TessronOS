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
#include <ts/proc.h>
#include <ts/dp.h>
#include <ts/fn.h>
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

/*
 * The calls the slide of a process draws with (tests/uprog/svcprog.c,
 * SP_TVSVC): ob_ref_vob and fn_dra_txt answer as the kernel's store and
 * faces do, and refuse what is wrong
 */
#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_TVSVC	46		/* as tests/uprog/svcprog.c */

LOCAL void test_process_svc( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg[8];
	ID	pid;

	/* what the process measures and draws letters with: the system's face, as on the desktop */
	if ( kt_face() <= 0 ) KT_SKIP("no face in the 書体箱");
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_TVSVC;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) KT_SKIP("no SVCPROG.ELF");
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 30000), E_OK);
	tm_printf((UB *)"  the calls of the slide of a process: exit %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

/*
 * An environment of memory of a process (dp_open_mem, svc_gate.c;
 * tests/uprog/svcprog.c, SP_DPMEM): what a process draws into its
 * picture through one comes back to it as the kernel draws the same,
 * pixel for pixel, its picture written back as it is closed; what is
 * wrong is refused; and one a process leaves open goes with it.
 */
#define SP_DPMEM	47		/* as tests/uprog/svcprog.c */
#define DPM_W		64
#define DPM_H		48
#define DPM_PITCH	( DPM_W + 3 )

IMPORT INT knl_dp_mem_count( void );

LOCAL ID dpm_run( UW mode, UW sum, T_PSTS *psts )
{
	T_CPRC	cprc;
	UW	arg[8];
	ID	pid;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_DPMEM;
	arg[1] = mode;
	arg[2] = sum;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) {
		return pid;
	}
	psts->exitcd = -1;
	(void)ts_wai_prc(pid, psts, 30000);
	return pid;
}

LOCAL void test_process_dpmem( void )
{
	static UW	px[DPM_H * DPM_PITCH], img[8 * 6];
	T_DPBMP		b;
	T_DPRECT	r;
	T_PSTS		psts;
	INT		gid, i, x, y;
	UW		h = 2166136261U;

	/* the picture the process starts from, drawn into by the kernel */
	for ( y = 0; y < DPM_H; y++ ) {
		for ( x = 0; x < DPM_PITCH; x++ ) {
			px[y * DPM_PITCH + x] = ( x < DPM_W ) ? (UW)( 0x00102030U + x * 3 + y * 0x100 ) : 0x00ABCDEFU;
		}
	}
	b.px = px;
	b.pitch = DPM_PITCH * 4;
	b.w = DPM_W;
	b.h = DPM_H;
	gid = dp_open_mem(&b);
	KT_ASSERT(gid >= 0);
	for ( i = 0; i < 8 * 6; i++ ) img[i] = (UW)( 0x00010203U * (UW)i );
	r.left = 4;  r.top = 4;  r.right = 30;  r.bottom = 20;
	(void)dp_fill_rect(gid, &r, 0x00FF0000U);
	r.left = 10;  r.top = 10;  r.right = 50;  r.bottom = 40;
	(void)dp_frame_rect(gid, &r, 0x0000FF00U, 2);
	(void)dp_line(gid, 0, DPM_H - 1, DPM_W - 1, 0, 0x000000FFU);
	(void)dp_put_argb(gid, 40, 30, img, 8, 8, 6, 0x00010203U);
	(void)dp_put_pixel(gid, 1, 1, 0x00FFFFFFU);
	r.left = -10;  r.top = -10;  r.right = DPM_W + 10;  r.bottom = 2;
	(void)dp_fill_rect(gid, &r, 0x00808080U);
	KT_ASSERT_ER(dp_close(gid), E_OK);
	for ( y = 0; y < DPM_H; y++ ) {
		for ( x = 0; x < DPM_W; x++ ) {
			h = ( h ^ px[y * DPM_PITCH + x] ) * 16777619U;
		}
		KT_ASSERT_EQ(px[y * DPM_PITCH + DPM_W], 0x00ABCDEFU);
	}
	tm_printf((UB *)"  the kernel's picture: sum %d\n", (INT)h);

	if ( dpm_run(0, h, &psts) <= 0 ) KT_SKIP("no SVCPROG.ELF");
	tm_printf((UB *)"  an environment of memory of a process: exit %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(knl_dp_mem_count(), 0);

	/* one left open as the process ends: let go of with it */
	KT_ASSERT(dpm_run(1, 0, &psts) > 0);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(knl_dp_mem_count(), 0);
}

/*
 * The faces of the kernel's listed by a process (fn_nface, fn_face_at,
 * fn_family, svc_gate.c; tests/uprog/svcprog.c, SP_FNFACE), as the
 * kernel lists them for 書体の一覧 of 基本文章編集
 */
#define SP_FNFACE	48		/* as tests/uprog/svcprog.c */

LOCAL void test_process_faces( void )
{
	T_CPRC	cprc;
	T_PSTS	psts;
	UW	arg[8];
	ID	pid;

	if ( kt_face() <= 0 || fn_nface() <= 0 ) KT_SKIP("no face in the 書体箱");
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_FNFACE;
	arg[1] = (UW)fn_nface();
	arg[2] = (UW)fn_face_at(0);
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	if ( pid <= 0 ) KT_SKIP("no SVCPROG.ELF");
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 30000), E_OK);
	tm_printf((UB *)"  the faces listed by a process: %d faces, exit %d\n", (INT)arg[1], psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_slide( void )
{
	tm_printf((UB *)"\n[slide] スライド (design 11.11)\n");
	KT_RUN(test_pages);
	KT_RUN(test_fit);
	KT_RUN(test_sample);
	KT_RUN(test_process_svc);
	KT_RUN(test_process_dpmem);
	KT_RUN(test_process_faces);
}
