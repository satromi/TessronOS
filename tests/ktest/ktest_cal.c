/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_cal.c
 *	予定表 (design 11.10): the records it reads and writes
 *
 *	The month record is the one TADjs Desktop keeps for the same month
 *	and the same link, byte for byte: its length and its FNV-1a hash
 *	are those of the record of January 2026 with the holidays 元日 and
 *	成人の日 and a link to 2026/01/05 as TADjs Desktop writes it.
 *	The holidays are read from the 休日 object as TADjs Desktop writes it
 *	and as a text edited here writes it, and back from a month record;
 *	a link keeps every attribute it came with; the day's text is made
 *	from the template. A link's attributes are set, taken off and read
 *	as text; a link sized is closed or open by its height; what is
 *	taken goes to the tray as a figure of links; an applist's program
 *	is kept to and let go. The program itself, started on a 予定表 as
 *	a system before wrote it, finds the 休日 object and the template by
 *	their names, gives their links the relationships 休日 and 年月日実身,
 *	and finds them by those once they are renamed.
 */

#include "ktest.h"
#include "tstdlib.h"
#include "../../application/calendar/cal_tad.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/sysdef.h>
#include <tm/tmonitor.h>

#define OUT_MAX		( 64 * 1024 )

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, m = s_len(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL str_eq( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 );
}

LOCAL UW fnv( CONST UB *s, INT n )
{
	UW	h = 0x811C9DC5U;
	INT	i;

	for ( i = 0; i < n; i++ ) h = ( h ^ s[i] ) * 0x01000193U;
	return h;
}

/* the calendar's arithmetic */
LOCAL void test_dates( void )
{
	INT	y = 0, m = 0, d = 0, row = 0, col = 0, box[4];
	UB	s[12];

	KT_ASSERT_EQ(ct_days(2026, 2), 28);
	KT_ASSERT_EQ(ct_days(2024, 2), 29);
	KT_ASSERT_EQ(ct_days(2000, 2), 29);
	KT_ASSERT_EQ(ct_days(1900, 2), 28);
	KT_ASSERT_EQ(ct_days(2026, 12), 31);
	KT_ASSERT_EQ(ct_wday(2026, 1, 1), 4);		/* Thursday */
	KT_ASSERT_EQ(ct_wday(2026, 10, 1), 4);
	KT_ASSERT_EQ(ct_wday(2026, 2, 1), 0);		/* Sunday */
	KT_ASSERT_EQ(ct_wday(2024, 2, 29), 4);

	KT_ASSERT(ct_is_day((CONST UB *)"2026/01/05", &y, &m, &d));
	KT_ASSERT_EQ(y, 2026);
	KT_ASSERT_EQ(m, 1);
	KT_ASSERT_EQ(d, 5);
	KT_ASSERT(!ct_is_day((CONST UB *)"2026/1/05", NULL, NULL, NULL));
	KT_ASSERT(!ct_is_day((CONST UB *)"2026/13/05", NULL, NULL, NULL));
	KT_ASSERT(!ct_is_day((CONST UB *)"2026/01/05 会議", NULL, NULL, NULL));
	KT_ASSERT(ct_is_month((CONST UB *)"2026/10", &y, &m));
	KT_ASSERT_EQ(m, 10);
	KT_ASSERT(!ct_is_month((CONST UB *)"2026/10/01", NULL, NULL));
	ct_day_name(s, 2026, 3, 7);
	KT_ASSERT(str_eq(s, "2026/03/07"));
	ct_month_name(s, 987, 11);
	KT_ASSERT(str_eq(s, "0987/11"));

	/* 2026/01/05 is the Monday of the second row */
	ct_cell(2026, 1, 5, &row, &col);
	KT_ASSERT_EQ(row, 1);
	KT_ASSERT_EQ(col, 1);
	ct_day_box(2026, 1, 5, 1, box);
	KT_ASSERT_EQ(box[0], 116);
	KT_ASSERT_EQ(box[1], 198);
	KT_ASSERT_EQ(box[2], 226);
	KT_ASSERT_EQ(box[3], 216);
}

/* 休日: as TADjs Desktop writes it, and as a text edited here may write it */
LOCAL void test_holidays( void )
{
	static CONST char	tadjs[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"休日\">\r\n<document>\r\n<p>\r\n"
		"2026/01/01\t元日<br/>\r\n2026/01/12\t成人の日<br/>\r\n"
		"2026/10/12\tスポーツの日<br/>\r\n2026/11/23\t勤労感謝の日<br/>\r\n</p>\r\n</document>\r\n</tad>\r\n";
	static CONST char	edited[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><text lang=\"0\"/><p>2026/05/03<tab/>憲法記念日"
		"<br/>2026/05/04\t &amp;みどりの日　</p><p><font size=\"14\"/>2026/05/05\tこどもの日</p>"
		"<p>覚え書き</p></document></tad>";
	CTHOL	h[8];
	INT	n;

	n = ct_hol_text((CONST UB *)tadjs, s_len(tadjs), h, 8);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT(ct_hol_find(h, n, 2026, 1, 1) != NULL && str_eq(ct_hol_find(h, n, 2026, 1, 1), "元日"));
	KT_ASSERT(str_eq(ct_hol_find(h, n, 2026, 10, 12), "スポーツの日"));
	KT_ASSERT(ct_hol_find(h, n, 2026, 1, 2) == NULL);

	n = ct_hol_text((CONST UB *)edited, s_len(edited), h, 8);
	KT_ASSERT_EQ(n, 3);
	KT_ASSERT(str_eq(ct_hol_find(h, n, 2026, 5, 3), "憲法記念日"));
	KT_ASSERT(str_eq(ct_hol_find(h, n, 2026, 5, 4), "&みどりの日"));	/* the ideographic space left off */
	KT_ASSERT(str_eq(ct_hol_find(h, n, 2026, 5, 5), "こどもの日"));

	/* no more than there is room for */
	KT_ASSERT_EQ(ct_hol_text((CONST UB *)tadjs, s_len(tadjs), h, 2), 2);
}

/* a month written as TADjs Desktop writes it, and read back */
LOCAL void test_month( void )
{
	UB	*out = (UB *)Kmalloc(OUT_MAX), el[1024];
	CTBUF	b, e;
	CTHOL	h[4], back[4];
	CTLNK	l[2];
	CTEL	links[4];
	TS_UUID	t, v, u;
	INT	box[4] = { 0, 0, 0, 0 }, n;

	if ( out == NULL ) KT_SKIP("no memory");
	knl_memset(h, 0, sizeof(h));
	h[0].y = 2026; h[0].m = 1; h[0].d = 1;
	knl_strcpy((char *)h[0].name, "元日");
	h[1].y = 2026; h[1].m = 1; h[1].d = 12;
	knl_strcpy((char *)h[1].name, "成人の日");
	KT_ASSERT(ct_uuid_parse((CONST UB *)"019d0000-0000-7000-8000-0000000000aa", &t));
	KT_ASSERT(ct_uuid_parse((CONST UB *)"019d0000-0000-7000-8000-000000000001", &v));
	ct_buf(&e, el, sizeof(el));
	ct_link_new(&e, &t, &v, box, TRUE);
	l[0].el = e.p;
	l[0].len = e.n;
	l[0].name = (CONST UB *)"2026/01/05";
	ct_buf(&b, out, OUT_MAX);
	KT_ASSERT_ER(ct_month_xml(&b, 2026, 1, 0, h, 2, l, 1), E_OK);
	tm_printf((UB *)"  month record: %d bytes, %08x\n", b.n, fnv(b.p, b.n));
	KT_ASSERT_EQ(b.n, 22148);
	KT_ASSERT_EQ(fnv(b.p, b.n), 0xFA5DF6C3U);
	KT_ASSERT(has(b.p, b.n, "vobjleft=\"116\" vobjtop=\"178\" vobjright=\"226\" vobjbottom=\"196\" height=\"18\""));
	KT_ASSERT(has(b.p, b.n, "<docmemo text=\"holiday\"/>成人の日\n"));

	/* the holidays and the link read back from it */
	n = ct_hol_month(b.p, b.n, 2026, 1, back, 4);
	KT_ASSERT_EQ(n, 2);
	KT_ASSERT(str_eq(ct_hol_find(back, n, 2026, 1, 1), "元日"));
	KT_ASSERT(str_eq(ct_hol_find(back, n, 2026, 1, 12), "成人の日"));
	n = ct_links(b.p, b.n, links, 4);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT(ct_target(b.p + links[0].at, links[0].len, &u));
	KT_ASSERT_EQ(ts_uuid_cmp(&u, &t), 0);
	KT_ASSERT_EQ(ct_attr_int(b.p + links[0].at, links[0].len, "zIndex", -1), 200);

	/* today's cell, and a second link of the day under the first */
	l[1] = l[0];
	ct_buf(&b, out, OUT_MAX);
	KT_ASSERT_ER(ct_month_xml(&b, 2026, 1, 5, h, 2, l, 2), E_OK);
	KT_ASSERT(has(b.p, b.n, "f_pat=\"133\" l_pat=\"130\" left=\"114\" top=\"158\""));
	KT_ASSERT(has(b.p, b.n, "vobjtop=\"198\" vobjright=\"226\" vobjbottom=\"216\""));

	/* too small a buffer says so */
	ct_buf(&b, out, 1000);
	KT_ASSERT_ER(ct_month_xml(&b, 2026, 1, 0, h, 2, l, 1), E_LIMIT);
	Kfree(out);
}

/* a link written again keeps what it had; a day's gets the calendar's look when it has none */
LOCAL void test_link( void )
{
	static CONST char	in[] = "<link id=\"019d0000-0000-7000-8000-0000000000bb_0.xtad\" vobjleft=\"5\""
				       " vobjtop='6' relationship=\"会議 定例\" applist=\"{&quot;a&quot;:1}\""
				       " zIndex=\"3\" frcol=\"\">2026/01/07</link>";
	UB	out[1024], val[64];
	CTBUF	b;
	CTEL	el[2];
	INT	box[4] = { 10, 20, 110, 38 };

	KT_ASSERT_EQ(ct_links((CONST UB *)in, s_len(in), el, 2), 1);
	KT_ASSERT_EQ(el[0].len, s_len(in));		/* the words and the end tag with it */
	ct_buf(&b, out, sizeof(out));
	ct_link_put(&b, (CONST UB *)in, s_len(in), box, 205, TRUE);
	KT_ASSERT(!b.fail);
	KT_ASSERT(has(b.p, b.n, "vobjleft=\"10\" vobjtop=\"20\""));
	KT_ASSERT(has(b.p, b.n, " relationship=\"会議 定例\" applist=\"{&quot;a&quot;:1}\" zIndex=\"205\""));
	KT_ASSERT(has(b.p, b.n, "frcol=\"#b0c0d0\""));
	KT_ASSERT(has(b.p, b.n, "vobjright=\"110\" vobjbottom=\"38\" height=\"18\" chsz=\"10\""));
	KT_ASSERT(b.p[b.n - 2] == '/' && b.p[b.n - 1] == '>');
	KT_ASSERT_EQ(ct_attr(b.p, b.n, "chcol", val, sizeof(val)), 7);
	KT_ASSERT(str_eq(val, "#000000"));
	KT_ASSERT_EQ(ct_attr(b.p, b.n, "tbcol", val, sizeof(val)), 7);
	KT_ASSERT(str_eq(val, "#e8f4ff"));
	KT_ASSERT_EQ(ct_attr(b.p, b.n, "hidden", val, sizeof(val)), -1);
}

/* a month's link put in the 予定表 object, and a day's text from the template */
LOCAL void test_records( void )
{
	static CONST char	cal[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"予定表\">\n<figure>\n"
		"<link id=\"019b8230-f63a-7985-b69d-20e43d55506c_0.xtad\" vobjleft=\"10\"/>\n</figure>\n</tad>\n";
	static CONST char	tpl[] =
		"<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>\n<text align=\"center\"/>"
		"<font size=\"21\">YYYY/MM/DD</font>\n</p>\n<p>□TODO YYYY/MM/DD</p>\n</document>\n</tad>\n";
	static CONST char	named[] = "<tad version=\"1.0\" filename=\"年月日実身\" encoding=\"UTF-8\"><document/></tad>";
	UB	out[1024], el[600];
	CTBUF	b, e;
	CTEL	links[4];
	TS_UUID	t, v;
	INT	box[4] = { 10, 90, 160, 115 };

	KT_ASSERT(ct_uuid_parse((CONST UB *)"019d0000-0000-7000-8000-0000000000cc", &t));
	KT_ASSERT(ct_uuid_parse((CONST UB *)"019d0000-0000-7000-8000-000000000002", &v));
	ct_buf(&e, el, sizeof(el));
	ct_link_new(&e, &t, &v, box, FALSE);
	KT_ASSERT(has(e.p, e.n, "height=\"25\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\""));
	ct_buf(&b, out, sizeof(out));
	KT_ASSERT_ER(ct_insert(&b, (CONST UB *)cal, s_len(cal), e.p, e.n), E_OK);
	KT_ASSERT_EQ(ct_links(b.p, b.n, links, 4), 2);
	KT_ASSERT(has(b.p, b.n, "zoomratio=\"1\"/>\n</figure>\n</tad>\n"));

	ct_buf(&b, out, sizeof(out));
	ct_apply_date(&b, (CONST UB *)tpl, s_len(tpl), (CONST UB *)"2026/10/05");
	KT_ASSERT(!has(b.p, b.n, "YYYY"));
	KT_ASSERT(has(b.p, b.n, "<font size=\"21\">2026/10/05</font>"));
	KT_ASSERT(has(b.p, b.n, "□TODO 2026/10/05"));
	ct_buf(&b, out, sizeof(out));
	ct_apply_date(&b, (CONST UB *)named, s_len(named), (CONST UB *)"2026/10/05");
	KT_ASSERT(has(b.p, b.n, "filename=\"2026/10/05\" encoding"));
	ct_buf(&b, out, sizeof(out));
	ct_day_doc(&b, (CONST UB *)"2026/10/06");
	KT_ASSERT(has(b.p, b.n, "<font size=\"20\"/>\n<text align=\"center\"/>\n2026/10/06\n</p>"));
}

/* a link's attributes set, taken off and added, and read as text */
LOCAL void test_edit( void )
{
	static CONST char	in[] = "<link id=\"019d0000-0000-7000-8000-0000000000bb_0.xtad\" vobjleft=\"5\""
				       " relationship=\"会議 &amp; 定例\" chsz=\"10\" hidden=\"false\">2026/01/07</link>";
	CTSET	set[4];
	UB	out[1024], val[64];
	CTBUF	b;

	set[0].name = "chsz";		set[0].value = (CONST UB *)"14";
	set[1].name = "hidden";		set[1].value = NULL;
	set[2].name = "autoopen";	set[2].value = (CONST UB *)"true";
	set[3].name = "applist";	set[3].value = (CONST UB *)"{\"a\":\"<b>\"}";
	ct_buf(&b, out, sizeof(out));
	ct_link_edit(&b, (CONST UB *)in, s_len(in), set, 4);
	KT_ASSERT(!b.fail);
	KT_ASSERT(has(b.p, b.n, " vobjleft=\"5\" relationship=\"会議 &amp; 定例\" chsz=\"14\""));
	KT_ASSERT(!has(b.p, b.n, "hidden"));
	KT_ASSERT(has(b.p, b.n, " autoopen=\"true\" applist=\"{&quot;a&quot;:&quot;&lt;b&gt;&quot;}\"/>"));
	KT_ASSERT(!has(b.p, b.n, "</link>"));

	/* read back as text, the escapes undone */
	KT_ASSERT_EQ(ct_attr_text((CONST UB *)in, s_len(in), "relationship", val, sizeof(val)), 15);
	KT_ASSERT(str_eq(val, "会議 & 定例"));
	KT_ASSERT_EQ(ct_attr_text(b.p, b.n, "applist", val, sizeof(val)), 11);
	KT_ASSERT(str_eq(val, "{\"a\":\"<b>\"}"));
	KT_ASSERT_EQ(ct_attr_text(b.p, b.n, "hidden", val, sizeof(val)), -1);
	KT_ASSERT_EQ(ct_attr_text((CONST UB *)in, s_len(in), "relationship", val, 5), 3);	/* not in a letter */
	KT_ASSERT(str_eq(val, "会"));
	KT_ASSERT(!ct_attr_bool((CONST UB *)in, s_len(in), "hidden", TRUE));
	KT_ASSERT(ct_attr_bool(b.p, b.n, "autoopen", FALSE));
	KT_ASSERT(ct_attr_bool(b.p, b.n, "pictdisp", TRUE));
}

/* a link sized: closed or open by its height; a box kept inside the month */
LOCAL void test_box( void )
{
	INT	b1[4] = { 700, 50, 850, 75 }, b2[4] = { -5, 590, 145, 615 }, b3[4] = { 10, 100, 900, 800 };

	KT_ASSERT_EQ(ct_band_h(14), 25);
	KT_ASSERT_EQ(ct_band_h(0), 25);
	KT_ASSERT_EQ(ct_band_h(10), 21);
	KT_ASSERT_EQ(ct_snap_h(14, 30), 25);
	KT_ASSERT_EQ(ct_snap_h(14, 33), 25);
	KT_ASSERT_EQ(ct_snap_h(14, 34), 34);
	KT_ASSERT_EQ(ct_snap_h(10, 12), 21);
	ct_fit_box(b1);
	KT_ASSERT(b1[0] == 650 && b1[1] == 70 && b1[2] == 800 && b1[3] == 95);
	ct_fit_box(b2);
	KT_ASSERT(b2[0] == 0 && b2[1] == 575 && b2[2] == 150 && b2[3] == 600);
	ct_fit_box(b3);
	KT_ASSERT(b3[0] == 0 && b3[1] == CT_GRID_TOP && b3[2] == 800 && b3[3] == 600);
}

/* what is taken, as the figure it goes to the tray as, and read back from it */
LOCAL void test_frag( void )
{
	static CONST char	a[] = "<link id=\"019d0000-0000-7000-8000-0000000000aa_0.xtad\" vobjleft=\"1\" vobjtop=\"80\""
				      " vobjright=\"151\" vobjbottom=\"105\">2026/01/05</link>";
	static CONST char	c[] = "<link id=\"019d0000-0000-7000-8000-0000000000cc_0.xtad\" chsz=\"10\"/>";
	UB	out[1024];
	CTBUF	b;
	CTLNK	l[2];
	CTEL	el[4];
	TS_UUID	u, t;

	l[0].el = (CONST UB *)a;
	l[0].len = s_len(a);
	l[0].name = (CONST UB *)"2026/01/05";
	l[1].el = (CONST UB *)c;
	l[1].len = s_len(c);
	l[1].name = (CONST UB *)"会議";
	ct_buf(&b, out, sizeof(out));
	KT_ASSERT_ER(ct_frag(&b, l, 2), E_OK);
	KT_ASSERT(has(b.p, b.n, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n<link "));
	KT_ASSERT(has(b.p, b.n, "</figure>\n</tad>\n"));
	KT_ASSERT_EQ(ct_links(b.p, b.n, el, 4), 2);
	KT_ASSERT_EQ(ct_attr_int(b.p + el[0].at, el[0].len, "vobjtop", -1), 80);
	KT_ASSERT(ct_target(b.p + el[1].at, el[1].len, &u));
	KT_ASSERT(ct_uuid_parse((CONST UB *)"019d0000-0000-7000-8000-0000000000cc", &t));
	KT_ASSERT_EQ(ts_uuid_cmp(&u, &t), 0);
	ct_buf(&b, out, 40);
	KT_ASSERT_ER(ct_frag(&b, l, 2), E_LIMIT);
}

/* an applist: the program the object opens with kept to and let go, its entries listed; a colour of metadata */
LOCAL void test_apps( void )
{
	static CONST char	apps[] = "{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
					 "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false}}";
	static CONST char	none[] = "{\"x\":{\"name\":\"X\"}}";
	static CONST char	meta[] = "{\"name\":\"予定表\",\"window\":{\"width\":800,\"backgroundColor\":\"#dedede\"}}";
	UB	out[512], back[512], ids[4][CT_APP_ID], names[4][CT_APP_NAME];
	CTBUF	b, c;

	KT_ASSERT(!ct_app_locked((CONST UB *)apps, s_len(apps)));
	ct_buf(&b, out, sizeof(out));
	KT_ASSERT_ER(ct_app_lock(&b, (CONST UB *)apps, s_len(apps), TRUE), E_OK);
	KT_ASSERT(has(b.p, b.n, "\"defaultOpen\":true,\"defaultOpenLock\":true},\"basic-figure-editor\""));
	KT_ASSERT(ct_app_locked(b.p, b.n));
	ct_buf(&c, back, sizeof(back));
	KT_ASSERT_ER(ct_app_lock(&c, b.p, b.n, FALSE), E_OK);
	KT_ASSERT(has(c.p, c.n, "\"defaultOpenLock\":false}"));
	KT_ASSERT_EQ(c.n, b.n + 1);
	KT_ASSERT(!ct_app_locked(c.p, c.n));
	ct_buf(&b, out, sizeof(out));
	KT_ASSERT_ER(ct_app_lock(&b, (CONST UB *)none, s_len(none), TRUE), E_NOEXS);

	KT_ASSERT_EQ(ct_app_list((CONST UB *)apps, s_len(apps), ids, names, 4), 2);
	KT_ASSERT(str_eq(ids[0], "basic-text-editor"));
	KT_ASSERT(str_eq(names[0], "基本文章編集"));
	KT_ASSERT(str_eq(ids[1], "basic-figure-editor"));
	KT_ASSERT_EQ(ct_app_list((CONST UB *)apps, s_len(apps), ids, names, 1), 1);

	KT_ASSERT_EQ(ct_json_colour((CONST UB *)meta, s_len(meta), "window", "backgroundColor", 0), 0xDEDEDEU);
	KT_ASSERT_EQ(ct_json_colour((CONST UB *)meta, s_len(meta), "window", "foreground", 7), 7U);
	KT_ASSERT_EQ(ct_json_colour((CONST UB *)none, s_len(none), "window", "backgroundColor", 9), 9U);
}

/* ---------------------------------------------------------------- the program on a 予定表 */

#define CAL_SAMPLE	"019d6f00-1a2b-7c3d-8e4f-a1b2c3d4e5f6"	/* the 予定表 of the samples (etc/xtad) */
#define CAL_LOG_MAX	( 64 * 1024 )

LOCAL UB	*cal_log;

/* What the console says after pos, up to tries tenths of a second, until it says w; the line's rest */
LOCAL BOOL cal_wait( UD pos, CONST char *w, char *line, INT max )
{
	INT	i, n, k, at, m = s_len(w);
	UD	p;

	for ( i = 0; i < 150; i++ ) {
		p = pos;
		n = tm_log_read(&p, cal_log, CAL_LOG_MAX - 1);
		if ( n < 0 ) n = 0;
		for ( at = 0; at + m <= n; at++ ) {
			for ( k = 0; k < m && cal_log[at + k] == (UB)w[k]; k++ ) ;
			if ( k < m ) continue;
			at += m;
			for ( k = 0; line != NULL && k < max - 1 && at + k < n && cal_log[at + k] != '\n'; k++ ) {
				line[k] = (char)cal_log[at + k];
			}
			if ( line != NULL ) line[k] = 0;
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	return FALSE;
}

LOCAL UD cal_now( void )
{
	UD	pos = 0;

	while ( tm_log_read(&pos, cal_log, CAL_LOG_MAX) > 0 ) ;
	return pos;
}

/* The program started on the object, until it says it is ready: what it said after "ready " */
LOCAL ID cal_start( CONST TS_UUID *obj, char *ready, INT max )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	UD	pos = cal_now();
	ID	pid;

	if ( ts_str_to_uuid(SYSDEF_PROG_CALENDAR, &prog) < E_OK ) return E_SYS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = obj;
	c.argsz = sizeof(*obj);
	if ( ob_cre_obj(&c, &pu) < E_OK ) return E_OBJ;
	pid = knl_prc_of_uuid(&pu);
	if ( pid <= 0 || !cal_wait(pos, "calendar: ready ", ready, max) ) return E_TMOUT;
	return pid;
}

LOCAL void cal_stop( ID pid )
{
	T_PSTS	ps;

	(void)ts_ter_prc(pid, TS_ABORT_TERM);
	(void)ts_wai_prc(pid, &ps, 5000);
}

/* Every occurrence of w taken out of a record of n bytes: the new length */
LOCAL INT cut_all( UB *s, INT n, CONST char *w )
{
	INT	m = s_len(w), i = 0, k;

	while ( i + m <= n ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k < m ) {
			i++;
			continue;
		}
		knl_memcpy(s + i, s + i + m, n - i - m);
		n -= m;
	}
	return n;
}

/* The object the link with that relationship points at in a record */
LOCAL BOOL link_with( CONST UB *x, INT n, CONST char *rel, TS_UUID *p_u )
{
	CTEL	el[16];
	UB	r[64];
	INT	k = ct_links(x, n, el, 16), i;

	for ( i = 0; i < k; i++ ) {
		INT	l = ct_attr_text(x + el[i].at, el[i].len, "relationship", r, sizeof(r));

		if ( l > 0 && str_eq(r, rel) && ct_target(x + el[i].at, el[i].len, p_u) ) return TRUE;
	}
	return FALSE;
}

/*
 * A 予定表 as a system before wrote it, its links to the 休日 object and
 * the template without a relationship: the program finds them by their
 * names and gives the links the words 休日 and 年月日実身; the 休日
 * object renamed afterwards is found all the same, by its link.
 */
LOCAL void test_relation( void )
{
	TS_UUID	src, cal, hol, tpl;
	UB	*x;
	SZ	size = 0;
	INT	n;
	char	first[160], again[160];
	ID	pid;

	cal_log = (UB *)Kmalloc(CAL_LOG_MAX);
	KT_ASSERT(cal_log != NULL);
	if ( cal_log == NULL ) return;
	KT_ASSERT_ER(ts_str_to_uuid(CAL_SAMPLE, &src), E_OK);
	/* the person's, as the desktop makes a copy, for the program to write it */
	knl_ob_make_for_person(TRUE);
	KT_ASSERT_ER(om_store_copy(&src, (CONST UB *)"KT予定表", TRUE, &cal), E_OK);
	knl_ob_make_for_person(FALSE);
	x = om_obj_record(&cal, 0, &size);
	KT_ASSERT(x != NULL);
	if ( x == NULL ) {
		Kfree(cal_log);
		return;
	}
	n = cut_all(x, (INT)size, " relationship=\"休日\"");
	n = cut_all(x, n, " relationship=\"年月日実身\"");
	KT_ASSERT(!has(x, n, "relationship=\"休日\""));
	KT_ASSERT_ER(om_obj_record_put(&cal, 0, OB_RT_TAD, 0, x, (SZ)n), E_OK);
	Kfree(x);

	pid = cal_start(&cal, first, sizeof(first));
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) cal_stop(pid);
	tm_printf((UB *)"  as written before: ready %s\n", first);
	KT_ASSERT(cal_wait(0, "calendar: relationship given", NULL, 0));
	KT_ASSERT(has((CONST UB *)first, s_len(first), " template") && !has((CONST UB *)first, s_len(first), "holidays 0"));
	x = om_obj_record(&cal, 0, &size);
	KT_ASSERT(x != NULL && link_with(x, (INT)size, "休日", &hol) && link_with(x, (INT)size, "年月日実身", &tpl));
	if ( x != NULL ) Kfree(x);

	/* the 休日 object and the template renamed: found by the links' relationships */
	KT_ASSERT_ER(om_store_rename(&hol, (CONST UB *)"KTやすみ"), E_OK);
	KT_ASSERT_ER(om_store_rename(&tpl, (CONST UB *)"KT日の原紙"), E_OK);
	pid = cal_start(&cal, again, sizeof(again));
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) cal_stop(pid);
	tm_printf((UB *)"  renamed: ready %s\n", again);
	KT_ASSERT(has((CONST UB *)again, s_len(again), " template") && !has((CONST UB *)again, s_len(again), "holidays 0"));

	(void)ob_del_obj(&hol);
	(void)ob_del_obj(&tpl);
	(void)ob_del_obj(&cal);
	Kfree(cal_log);
}

EXPORT void ktest_cal( void )
{
	tm_printf((UB *)"\n[cal] 予定表 (design 11.10)\n");
	KT_RUN(test_dates);
	KT_RUN(test_holidays);
	KT_RUN(test_month);
	KT_RUN(test_link);
	KT_RUN(test_records);
	KT_RUN(test_edit);
	KT_RUN(test_box);
	KT_RUN(test_frag);
	KT_RUN(test_apps);
	KT_RUN_SCREEN(test_relation);
}
