/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_techo.c
 *	電子手帳 (design 11.23): the record, the 予定表, the 住所録, the 休日,
 *	searching and the pages of 印刷
 *
 *	What the program keeps (application/techo/tc_db.c) makes no system
 *	call, so it is linked here as it is. A record is written by hand as
 *	基本文章編集 would leave it -- the three parts cut by tab formats,
 *	tabs, escapes, a link and a decoration among the letters, a line
 *	that is no plan -- and read; what is read is written and read again
 *	and must be the same. Then the rows of a day with an empty one
 *	between, the people sorted by よみ as it changes, the indexes and
 *	the place in them, deleting down to the last page, every kind of
 *	holiday date, the eras, searching in either kind of kana and case
 *	and in a field named, the paper and the pages printed. The template
 *	手帳用紙 is read from the store and its holidays looked at, and last
 *	the program is started on the sample 私の手帳 and its three windows
 *	looked for.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/techo/tc_db.h"

#define SAMPLE		"01a0d8c4-53c6-7a77-8b88-9cadbecf53c6"
#define BUF_MAX		( 256 * 1024 )

/* ---------------------------------------------------------------- what the engine asks */

EXPORT void *tc_alloc( INT size )
{
	return Kmalloc((SZ)( size > 0 ? size : 1 ));
}

EXPORT void tc_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

/* ---------------------------------------------------------------- small things */

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) return (BOOL)( b[0] == 0 );
	for ( i = 0; a[i] != 0 || b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL holds( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; w[k] != 0 && i + k < n && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/* The person whose 氏名 is name: its place, -1 none */
LOCAL INT person( TCDB *db, CONST char *name )
{
	INT	i;

	for ( i = 0; i < db->na; i++ ) {
		if ( s_eq(tc_addr_get(db, i, TC_F_NAME), name) ) return i;
	}
	return -1;
}

/* A record as 基本文章編集 leaves one: the parts after tab formats, a link and decorations among them */
LOCAL CONST char rec1[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"手帳\">\n<document>\n"
	"<paper type=\"doc\" length=\"595\" width=\"842\" binding=\"0\" imposition=\"0\" top=\"28\" bottom=\"28\" left=\"57\" right=\"57\"/>\n"
	"<p>私の手帳</p>\n"
	"<p><tab-format height=\"0.2\" pargap=\"0\"/>2005/9/1<tab/>13:00 渋谷駅南口  山崎部長と待合せ</p>\n"
	"<p>2005/9/14<tab/>19:00 中華「海鮮」で<font size=\"20\"/>横山さん&amp;家族と会食</p>\n"
	"<p>2005/9/1<tab/>&lt;資料&gt;を持参<link id=\"019a0f91-066f-706f-93da-ffe8150d6207_0.xtad\" vobjid=\"019a0f91-066f-706f-93da-ffe8150d6208\"/></p>\n"
	"<p>メモ: これは予定ではない</p>\n"
	"<p>2005/9/22<tab/>岩見CCでゴルフコンペ</p>\n"
	"<p><tab-format/>すずき  まさいち<tab/>980<tab/>仙台市若林区上館3-20<tab/>金山ハイツ304<tab/><tab/>鈴木  政一<tab/>022-48-1167<tab/>022-48-1562<tab/>アメリカ旅行のお土産をもらう<tab/>テニス仲間</p>\n"
	"<p>みき  しゅうさく<tab/>607<tab/>京都市山科区美駒6-2<tab/><tab/><tab/>三木  修作<tab/>075-34-9345<tab/><tab/><tab/></p>\n"
	"<p>アベ　ケイコ<tab/><tab/>東京都品川区<tab/><tab/><tab/>阿部  恵子<tab/>03-1111-2222</p>\n"
	"<p>Brown<tab/><tab/>London<tab/><tab/><tab/>Jim Brown<tab/><tab/><tab/>英会話の先生</p>\n"
	"<p>かとう<tab/>100<tab/>千代田区<tab/><tab/><tab/>加藤  一<tab/><tab/><tab/><tab/>教師</p>\n"
	"<p><tab-format/>土日</p>\n"
	"<p>*/11/3<tab/>文化の日</p>\n"
	"<p>*/*/10</p>\n"
	"<p>*/8/*</p>\n"
	"<p>1999/3/10</p>\n"
	"<p>1990-1999/1/15</p>\n"
	"<p>2000-2050/1/2月</p>\n"
	"<p>2000-2050/13/1</p>\n"
	"<p><tab-format/></p>\n"
	"</document>\n</tad>\n";

/* ---------------------------------------------------------------- reading and writing */

LOCAL void test_read( void )
{
	TCDB		*db = Kmalloc(sizeof(TCDB));
	CONST UB	*r[8];
	INT		n;

	if ( db == NULL ) KT_SKIP("no memory");
	tc_init(db);
	KT_ASSERT_ER(tc_read(db, (CONST UB *)rec1, sizeof(rec1) - 1), E_OK);
	/* 予定表: the day's rows in the order they came, the escapes undone, the link passed over */
	KT_ASSERT_EQ(db->np, 4);
	n = tc_plans(db, tc_day(2005, 9, 1), r, 8);
	KT_ASSERT_EQ(n, 2);
	KT_ASSERT(s_eq(r[0], "13:00 渋谷駅南口  山崎部長と待合せ"));
	KT_ASSERT(s_eq(r[1], "<資料>を持参"));
	n = tc_plans(db, tc_day(2005, 9, 14), r, 8);
	KT_ASSERT(n == 1 && s_eq(r[0], "19:00 中華「海鮮」で横山さん&家族と会食"));
	KT_ASSERT_EQ(tc_plans(db, tc_day(2005, 9, 2), r, 8), 0);
	/* the title and the line that is not a plan are not read as anything */
	KT_ASSERT_EQ(db->dropped, 2);
	/* 住所録: sorted by index and よみ, Brown in 他 */
	KT_ASSERT_EQ(db->na, 5);
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_NAME), "阿部  恵子"));
	KT_ASSERT(s_eq(tc_addr_get(db, 1, TC_F_NAME), "加藤  一"));
	KT_ASSERT(s_eq(tc_addr_get(db, 2, TC_F_NAME), "鈴木  政一"));
	KT_ASSERT(s_eq(tc_addr_get(db, 3, TC_F_NAME), "三木  修作"));
	KT_ASSERT(s_eq(tc_addr_get(db, 4, TC_F_NAME), "Jim Brown"));
	KT_ASSERT(s_eq(tc_addr_get(db, 2, TC_F_ADDR2), "金山ハイツ304"));
	KT_ASSERT(s_eq(tc_addr_get(db, 2, TC_F_ADDR3), ""));
	KT_ASSERT(s_eq(tc_addr_get(db, 2, TC_F_MEMO2), "テニス仲間"));
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_TEL), "03-1111-2222"));
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_MEMO2), ""));
	/* 休日: the weekdays, and the lines kept as written, the one that is no date too */
	KT_ASSERT_EQ(db->week, ( 1U << 0 ) | ( 1U << 6 ));
	KT_ASSERT_EQ(db->nhl, 8);
	KT_ASSERT_EQ(db->nr, 6);
	KT_ASSERT(s_eq(db->hl[1], "*/11/3\t文化の日"));
	/* the paper: A4 turned, as written */
	KT_ASSERT(db->pw == 842 && db->ph == 595 && db->mt == 28 && db->ml == 57);
	tc_clear(db);
	Kfree(db);
}

LOCAL void test_write( void )
{
	TCDB		*a = Kmalloc(sizeof(TCDB)), *b = Kmalloc(sizeof(TCDB));
	UB		*buf = Kmalloc(BUF_MAX), *buf2 = Kmalloc(BUF_MAX);
	CTBUF		o, o2;
	CONST UB	*r[8];
	INT		i, f, turn_mm[4];
	BOOL		turn;

	if ( a == NULL || b == NULL || buf == NULL || buf2 == NULL ) KT_SKIP("no memory");
	tc_init(a);
	tc_init(b);
	KT_ASSERT_ER(tc_read(a, (CONST UB *)rec1, sizeof(rec1) - 1), E_OK);
	ct_buf(&o, buf, BUF_MAX);
	KT_ASSERT_ER(tc_write(a, &o, (CONST UB *)"手帳"), E_OK);
	/* the form 基本文章編集 reads: three tab formats and one after, tabs, escapes */
	KT_ASSERT(holds(o.p, o.n, "<p><tab-format "));
	KT_ASSERT(holds(o.p, o.n, "2005/9/1<tab/>13:00 渋谷駅南口  山崎部長と待合せ</p>"));
	KT_ASSERT(holds(o.p, o.n, "2005/9/1<tab/>&lt;資料&gt;を持参</p>"));
	KT_ASSERT(holds(o.p, o.n, "すずき  まさいち<tab/>980<tab/>仙台市若林区上館3-20<tab/>金山ハイツ304<tab/><tab/>鈴木  政一"));
	KT_ASSERT(holds(o.p, o.n, "*/11/3<tab/>文化の日</p>"));
	KT_ASSERT(holds(o.p, o.n, "<paper type=\"doc\" length=\"595\" width=\"842\""));
	{
		INT	k, tf = 0;

		for ( k = 0; k + 11 < o.n; k++ ) {
			if ( holds(o.p + k, 11, "<tab-format") ) tf++;
		}
		KT_ASSERT_EQ(tf, 4);
	}
	/* read again: the same */
	KT_ASSERT_ER(tc_read(b, o.p, o.n), E_OK);
	KT_ASSERT_EQ(b->np, a->np);
	for ( i = 0; i < a->np && i < b->np; i++ ) {
		KT_ASSERT(a->p[i].day == b->p[i].day && s_eq(a->p[i].text, (CONST char *)b->p[i].text));
	}
	KT_ASSERT_EQ(b->na, a->na);
	for ( i = 0; i < a->na && i < b->na; i++ ) {
		for ( f = 0; f < TC_NF; f++ ) {
			KT_ASSERT(s_eq(tc_addr_get(a, i, f), (CONST char *)tc_addr_get(b, i, f)));
		}
	}
	KT_ASSERT_EQ(b->nhl, a->nhl);
	KT_ASSERT_EQ(b->week, a->week);
	KT_ASSERT_EQ(b->dropped, 0);
	ct_buf(&o2, buf2, BUF_MAX);
	KT_ASSERT_ER(tc_write(b, &o2, (CONST UB *)"手帳"), E_OK);
	KT_ASSERT(o.n == o2.n && holds(o.p, o.n, (CONST char *)o2.p));
	/* a new 手帳用紙: nothing at all, and still four tab formats */
	tc_clear(b);
	ct_buf(&o2, buf2, BUF_MAX);
	KT_ASSERT_ER(tc_write(b, &o2, (CONST UB *)"空"), E_OK);
	KT_ASSERT_ER(tc_read(a, o2.p, o2.n), E_OK);
	KT_ASSERT(a->np == 0 && a->na == 0 && a->nhl == 0);
	KT_ASSERT_EQ(tc_plans(a, tc_day(2005, 9, 1), r, 8), 0);
	/* the paper: set by kind and turn, and told back */
	turn_mm[0] = 10;
	turn_mm[1] = 15;
	turn_mm[2] = 20;
	turn_mm[3] = 25;
	tc_set_paper(a, TC_PAPER_B5, TRUE, turn_mm);
	KT_ASSERT(a->pw == 729 && a->ph == 516 && a->mt == 28 && a->mr == 71);
	turn_mm[0] = turn_mm[1] = turn_mm[2] = turn_mm[3] = 0;
	KT_ASSERT_EQ(tc_paper_kind(a, &turn, turn_mm), TC_PAPER_B5);
	KT_ASSERT(turn && turn_mm[0] == 10 && turn_mm[1] == 15 && turn_mm[2] == 20 && turn_mm[3] == 25);
	tc_clear(a);
	tc_clear(b);
	Kfree(buf2);
	Kfree(buf);
	Kfree(b);
	Kfree(a);
}

/* ---------------------------------------------------------------- 予定表 */

LOCAL void test_plans( void )
{
	TCDB		*db = Kmalloc(sizeof(TCDB));
	CONST UB	*rows[4], *r[8];
	INT		day = tc_day(2026, 10, 20), y, m, d;

	if ( db == NULL ) KT_SKIP("no memory");
	tc_init(db);
	/* a row left empty between two keeps its place; the empty ones after the last are left out */
	rows[0] = (CONST UB *)"09:30 歯医者";
	rows[1] = (CONST UB *)"";
	rows[2] = (CONST UB *)"19:00 英会話";
	rows[3] = (CONST UB *)"";
	KT_ASSERT_ER(tc_set_plans(db, day, rows, 4), E_OK);
	KT_ASSERT_EQ(tc_plans(db, day, r, 8), 3);
	KT_ASSERT(s_eq(r[1], "") && s_eq(r[2], "19:00 英会話"));
	rows[0] = (CONST UB *)"前の日";
	KT_ASSERT_ER(tc_set_plans(db, day - 1, rows, 1), E_OK);
	rows[0] = (CONST UB *)"次の日";
	KT_ASSERT_ER(tc_set_plans(db, tc_day_add(day, 1), rows, 1), E_OK);
	KT_ASSERT_EQ(db->np, 5);
	KT_ASSERT(db->p[0].day == day - 1 && db->p[4].day == day + 1);
	/* 空欄にする */
	KT_ASSERT_ER(tc_set_plans(db, day, NULL, 0), E_OK);
	KT_ASSERT_EQ(tc_plans(db, day, r, 8), 0);
	KT_ASSERT_EQ(db->np, 2);
	KT_ASSERT_EQ(tc_next_planned(db, TC_DAY_FIRST - 1), day - 1);
	KT_ASSERT_EQ(tc_next_planned(db, day - 1), day + 1);
	KT_ASSERT_EQ(tc_next_planned(db, day + 1), 0);
	/* days: the pages and stepping over months and years */
	KT_ASSERT_EQ(tc_day_add(tc_day(2026, 12, 31), 1), tc_day(2027, 1, 1));
	KT_ASSERT_EQ(tc_day_add(tc_day(2024, 3, 1), -1), tc_day(2024, 2, 29));
	KT_ASSERT_EQ(tc_day_add(tc_day(2023, 3, 1), -1), tc_day(2023, 2, 28));
	KT_ASSERT(tc_day_ok(TC_DAY_FIRST) && tc_day_ok(TC_DAY_LAST));
	KT_ASSERT(!tc_day_ok(tc_day(1989, 12, 31)) && !tc_day_ok(tc_day(2051, 1, 1)) && !tc_day_ok(tc_day(2026, 2, 29)));
	KT_ASSERT(tc_parse_day((CONST UB *)"2005/9/1\tx", 10, &d) == 8 && d == tc_day(2005, 9, 1));
	KT_ASSERT_EQ(tc_parse_day((CONST UB *)"2005/13/1", 9, &d), 0);
	KT_ASSERT_EQ(tc_parse_day((CONST UB *)"2005-9-1", 8, &d), 0);
	tc_day_split(tc_day(2050, 12, 31), &y, &m, &d);
	KT_ASSERT(y == 2050 && m == 12 && d == 31);
	tc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- 住所録 */

LOCAL void test_people( void )
{
	TCDB	*db = Kmalloc(sizeof(TCDB));
	INT	i, pos, count;
	UB	t[24];

	if ( db == NULL ) KT_SKIP("no memory");
	tc_init(db);
	/* the indexes of a よみ */
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"あおき"), 0);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ぉ"), 0);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ゔぃ"), 0);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ガッコウ"), 1);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ぞう"), 2);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ど"), 3);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"の"), 4);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ぽ"), 5);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"も"), 6);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ゃ"), 7);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ろ"), 8);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"ん"), 9);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"Abe"), TC_IDX_OTHER);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)"1番"), TC_IDX_OTHER);
	KT_ASSERT_EQ(tc_index_of((CONST UB *)""), TC_IDX_OTHER);
	KT_ASSERT(s_eq((CONST UB *)tc_index_name(1), "か") && s_eq((CONST UB *)tc_index_name(10), "他"));
	/* a new page goes to 他; its よみ sorts it at once */
	i = tc_addr_add(db);
	KT_ASSERT_EQ(i, 0);
	i = tc_addr_set(db, i, TC_F_NAME, (CONST UB *)"山田 太郎");
	KT_ASSERT_EQ(i, 0);
	i = tc_addr_set(db, i, TC_F_YOMI, (CONST UB *)"やまだ たろう");
	i = tc_addr_add(db);
	KT_ASSERT_EQ(i, 1);			/* 他, after や */
	i = tc_addr_set(db, i, TC_F_NAME, (CONST UB *)"加藤 健二");
	i = tc_addr_set(db, i, TC_F_YOMI, (CONST UB *)"かとう けんじ");
	KT_ASSERT_EQ(i, 0);
	KT_ASSERT_EQ(person(db, "山田 太郎"), 1);
	i = tc_addr_add(db);
	i = tc_addr_set(db, i, TC_F_NAME, (CONST UB *)"木村 優子");
	i = tc_addr_set(db, i, TC_F_YOMI, (CONST UB *)"キムラ ユウコ");	/* katakana sorts as hiragana */
	KT_ASSERT_EQ(i, 1);
	i = tc_addr_add(db);
	i = tc_addr_set(db, i, TC_F_NAME, (CONST UB *)"加藤 一");
	i = tc_addr_set(db, i, TC_F_YOMI, (CONST UB *)"かとう はじめ");
	KT_ASSERT_EQ(i, 1);
	/* か has three: 加藤 健二 1/3, 加藤 一 2/3, 木村 優子 3/3 */
	tc_index_pos(db, person(db, "加藤 一"), &pos, &count);
	KT_ASSERT(pos == 2 && count == 3);
	tc_pos_text(pos, count, t, sizeof(t));
	KT_ASSERT(s_eq(t, "2/3"));
	tc_pos_text(5, 1000, t, sizeof(t));
	KT_ASSERT(s_eq(t, "***"));
	KT_ASSERT_EQ(tc_index_first(db, 1), 0);
	KT_ASSERT_EQ(tc_index_first(db, 7), 3);
	KT_ASSERT_EQ(tc_index_first(db, 0), -1);
	/* よみ changed: from か to あ */
	i = tc_addr_set(db, person(db, "木村 優子"), TC_F_YOMI, (CONST UB *)"あおき ゆうこ");
	KT_ASSERT_EQ(i, 0);
	tc_index_pos(db, i, &pos, &count);
	KT_ASSERT(pos == 1 && count == 1);
	/* a field emptied */
	(void)tc_addr_set(db, 0, TC_F_NAME, (CONST UB *)"");
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_NAME), ""));
	/* deleted down to the last page, which is emptied rather than taken */
	while ( db->na > 1 ) tc_addr_del(db, 0);
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_NAME), "山田 太郎"));
	tc_addr_del(db, 0);
	KT_ASSERT_EQ(db->na, 1);
	KT_ASSERT(s_eq(tc_addr_get(db, 0, TC_F_NAME), "") && s_eq(tc_addr_get(db, 0, TC_F_YOMI), ""));
	KT_ASSERT_EQ(db->a[0]->idx, TC_IDX_OTHER);
	tc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- 休日 */

LOCAL void test_holidays( void )
{
	TCDB		*db = Kmalloc(sizeof(TCDB));
	UB		era[24];
	CONST UB	*l[12];

	if ( db == NULL ) KT_SKIP("no memory");
	tc_init(db);
	KT_ASSERT_ER(tc_read(db, (CONST UB *)rec1, sizeof(rec1) - 1), E_OK);
	KT_ASSERT(tc_holiday(db, 2026, 10, 3));			/* 土 */
	KT_ASSERT(tc_holiday(db, 2026, 10, 4));			/* 日 */
	KT_ASSERT(!tc_holiday(db, 2026, 10, 5));
	KT_ASSERT(tc_holiday(db, 2026, 11, 3));			/* every year's */
	KT_ASSERT(tc_holiday(db, 1995, 11, 3));
	KT_ASSERT(tc_holiday(db, 2026, 6, 10));			/* every month's 10th */
	KT_ASSERT(tc_holiday(db, 2026, 8, 19) && tc_holiday(db, 2026, 8, 31));	/* all of August */
	KT_ASSERT(tc_holiday(db, 1999, 3, 10));
	KT_ASSERT(!tc_holiday(db, 2000, 3, 9));
	KT_ASSERT(tc_holiday(db, 1999, 1, 15) && !tc_holiday(db, 2001, 1, 15));
	KT_ASSERT(tc_holiday(db, 2026, 1, 12));			/* the second Monday */
	KT_ASSERT(!tc_holiday(db, 2026, 1, 5) && !tc_holiday(db, 2026, 1, 19));
	KT_ASSERT(tc_holiday(db, 2000, 1, 10) && !tc_holiday(db, 1999, 1, 11));
	KT_ASSERT(!tc_holiday(db, 2026, 9, 1) && !tc_holiday(db, 2026, 1, 13));
	/* given anew */
	l[0] = (CONST UB *)"水";
	l[1] = (CONST UB *)"*/12/2木";
	l[2] = (CONST UB *)"2026/7/*";
	l[3] = (CONST UB *)"2026/2/32";
	l[4] = (CONST UB *)"x/1/1";
	KT_ASSERT_ER(tc_set_holidays(db, l, 5), E_OK);
	KT_ASSERT_EQ(db->week, 1U << 3);
	KT_ASSERT_EQ(db->nr, 2);
	KT_ASSERT(tc_holiday(db, 2026, 10, 7));			/* 水 */
	KT_ASSERT(!tc_holiday(db, 2026, 10, 4));
	KT_ASSERT(tc_holiday(db, 2026, 12, 10) && !tc_holiday(db, 2026, 12, 3));
	KT_ASSERT(tc_holiday(db, 2026, 7, 13) && !tc_holiday(db, 2027, 7, 13));
	/* the eras */
	KT_ASSERT(tc_era(2005, 9, 1, era, sizeof(era)) > 0 && s_eq(era, "平成17年"));
	KT_ASSERT(tc_era(2019, 5, 1, era, sizeof(era)) > 0 && s_eq(era, "令和元年"));
	KT_ASSERT(tc_era(2019, 4, 1, era, sizeof(era)) > 0 && s_eq(era, "平成31年"));
	KT_ASSERT(tc_era(1989, 1, 1, era, sizeof(era)) > 0 && s_eq(era, "昭和64年"));
	KT_ASSERT(tc_era(2026, 10, 4, era, sizeof(era)) > 0 && s_eq(era, "令和8年"));
	tc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- 国民の祝日 */

#include "ktest_techo_hol.h"

/*
 * The law's reckoning against the table of 1990 to 2050 (every rule as
 * it was, each equinox, 振替休日 and 国民の休日 as a date): the same days
 * every day of the 61 years. Then years past the table.
 */
LOCAL void test_jp( void )
{
	TCDB		*db = Kmalloc(sizeof(TCDB)), *jp = Kmalloc(sizeof(TCDB));
	CONST UB	*l[1];
	INT		day, y, m, d, bad = 0, n = 0;

	if ( db == NULL || jp == NULL ) KT_SKIP("no memory");
	tc_init(db);
	tc_init(jp);
	KT_ASSERT_ER(tc_set_holidays(db, (CONST UB *CONST *)kt_hol_table,
				     (INT)( sizeof(kt_hol_table) / sizeof(kt_hol_table[0]) )), E_OK);
	KT_ASSERT(db->week == 0 && !db->jp && db->nr > 200);
	l[0] = (CONST UB *)"祝日\t国民の祝日";
	KT_ASSERT_ER(tc_set_holidays(jp, l, 1), E_OK);
	KT_ASSERT(jp->jp && jp->nr == 0 && jp->week == 0);
	for ( day = TC_DAY_FIRST; day <= TC_DAY_LAST; day = tc_day_add(day, 1) ) {
		BOOL	a, b;

		tc_day_split(day, &y, &m, &d);
		a = tc_holiday(db, y, m, d);
		b = tc_holiday(jp, y, m, d);
		if ( b ) n++;
		if ( a != b ) {
			if ( bad < 8 ) tm_printf((UB *)"KTEST techo %d/%d/%d table %d reckoned %d\n", y, m, d, a, b);
			bad++;
		}
	}
	tm_printf((UB *)"KTEST techo holidays 1990-2050: %d, differing %d\n", n, bad);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT(n > 900);
	/* the names */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2026, 9, 22), "国民の休日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2026, 5, 6), "振替休日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2026, 10, 12), "スポーツの日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2004, 5, 4), "国民の休日"));	/* a Tuesday */
	KT_ASSERT(tc_jp_holiday(2003, 5, 4) == NULL);					/* a Sunday */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2008, 5, 4), "みどりの日"));
	KT_ASSERT(tc_jp_holiday(2026, 10, 13) == NULL);
	/* past the table: the same rules go on */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2051, 1, 2), "振替休日"));		/* 元日 a Sunday */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2051, 1, 9), "成人の日"));		/* the 2nd Monday */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2051, 3, 21), "春分の日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2051, 9, 23), "秋分の日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2052, 9, 23), "振替休日"));	/* 秋分の日 a Sunday */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2099, 5, 6), "振替休日"));	/* 5/3 a Sunday, 4 and 5 holidays */
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2099, 3, 20), "春分の日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2100, 3, 20), "春分の日"));
	KT_ASSERT(s_eq((CONST UB *)tc_jp_holiday(2100, 9, 23), "秋分の日"));
	KT_ASSERT(tc_jp_holiday(2051, 1, 3) == NULL && tc_jp_holiday(1948, 1, 1) == NULL);
	tc_clear(jp);
	tc_clear(db);
	Kfree(jp);
	Kfree(db);
}

/* ---------------------------------------------------------------- 検索 */

LOCAL void test_search( void )
{
	TCDB	*db = Kmalloc(sizeof(TCDB));
	INT	i;

	if ( db == NULL ) KT_SKIP("no memory");
	tc_init(db);
	KT_ASSERT_ER(tc_read(db, (CONST UB *)rec1, sizeof(rec1) - 1), E_OK);
	/* either kind of kana, either case */
	KT_ASSERT(tc_match((CONST UB *)"リンゴ", (CONST UB *)"りんご"));
	KT_ASSERT(tc_match((CONST UB *)"りんご", (CONST UB *)"リンゴ"));
	KT_ASSERT(tc_match((CONST UB *)"Say HELLO", (CONST UB *)"hello"));
	KT_ASSERT(tc_match((CONST UB *)"Hello", (CONST UB *)"hELLO"));
	KT_ASSERT(!tc_match((CONST UB *)"りんご", (CONST UB *)"みかん"));
	KT_ASSERT(!tc_match((CONST UB *)"abc", (CONST UB *)""));
	/* 予定表: from the first page, and from the page after */
	KT_ASSERT_EQ(tc_find_plan(db, TC_DAY_FIRST - 1, (CONST UB *)"ゴルフ"), tc_day(2005, 9, 22));
	KT_ASSERT_EQ(tc_find_plan(db, TC_DAY_FIRST - 1, (CONST UB *)"cc"), tc_day(2005, 9, 22));
	KT_ASSERT_EQ(tc_find_plan(db, TC_DAY_FIRST - 1, (CONST UB *)"しりょう"), 0);
	KT_ASSERT_EQ(tc_find_plan(db, TC_DAY_FIRST - 1, (CONST UB *)"資料"), tc_day(2005, 9, 1));
	KT_ASSERT_EQ(tc_find_plan(db, tc_day(2005, 9, 1), (CONST UB *)"資料"), 0);
	KT_ASSERT_EQ(tc_find_plan(db, tc_day(2005, 9, 1), (CONST UB *)":00"), tc_day(2005, 9, 14));
	/* 住所録: every field, or the one named */
	i = tc_find_addr(db, 0, (CONST UB *)"テニス");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "鈴木  政一"));
	KT_ASSERT_EQ(tc_find_addr(db, i + 1, (CONST UB *)"テニス"), -1);
	i = tc_find_addr(db, 0, (CONST UB *)"あべ");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "阿部  恵子"));
	i = tc_find_addr(db, 0, (CONST UB *)"氏名:三木");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "三木  修作"));
	KT_ASSERT_EQ(tc_find_addr(db, 0, (CONST UB *)"住所1:金山"), -1);
	i = tc_find_addr(db, 0, (CONST UB *)"住所:金山");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "鈴木  政一"));
	i = tc_find_addr(db, 0, (CONST UB *)"住所２：金山");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "鈴木  政一"));
	i = tc_find_addr(db, 0, (CONST UB *)"メモ:教師");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "加藤  一"));
	KT_ASSERT_EQ(tc_find_addr(db, 0, (CONST UB *)"メモ1:教師"), -1);
	i = tc_find_addr(db, 0, (CONST UB *)"fax:1562");
	KT_ASSERT(i >= 0 && s_eq(tc_addr_get(db, i, TC_F_NAME), "鈴木  政一"));
	KT_ASSERT_EQ(tc_find_addr(db, 0, (CONST UB *)"〒:607"), person(db, "三木  修作"));
	/* a colon after no field's name is looked for as it is */
	KT_ASSERT_EQ(tc_find_addr(db, 0, (CONST UB *)"x:y"), -1);
	tc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- 印刷 and the tray */

LOCAL void test_print( void )
{
	TCDB	*db = Kmalloc(sizeof(TCDB));
	UB	*buf = Kmalloc(BUF_MAX), t[64];
	CTBUF	b;
	INT	k, n;

	if ( db == NULL || buf == NULL ) KT_SKIP("no memory");
	tc_init(db);
	KT_ASSERT_ER(tc_read(db, (CONST UB *)rec1, sizeof(rec1) - 1), E_OK);
	ct_buf(&b, buf, BUF_MAX);
	KT_ASSERT_ER(tc_print_plans(db, &b, tc_day(2005, 9, 1)), E_OK);
	KT_ASSERT(holds(b.p, b.n, "<paper type=\"doc\" length=\"595\" width=\"842\""));
	KT_ASSERT(holds(b.p, b.n, "2005年9月1日（木）"));
	KT_ASSERT(holds(b.p, b.n, "&lt;資料&gt;を持参"));
	KT_ASSERT(!holds(b.p, b.n, "ゴルフ"));
	ct_buf(&b, buf, BUF_MAX);
	KT_ASSERT_ER(tc_print_plans(db, &b, 0), E_OK);
	KT_ASSERT(holds(b.p, b.n, "2005年9月14日（水）") && holds(b.p, b.n, "ゴルフ"));
	ct_buf(&b, buf, BUF_MAX);
	KT_ASSERT_ER(tc_print_addrs(db, &b, person(db, "鈴木  政一")), E_OK);
	KT_ASSERT(holds(b.p, b.n, "鈴木  政一") && holds(b.p, b.n, "金山ハイツ304") && !holds(b.p, b.n, "三木"));
	ct_buf(&b, buf, BUF_MAX);
	KT_ASSERT_ER(tc_print_addrs(db, &b, -1), E_OK);
	for ( k = 0; k < db->na; k++ ) KT_ASSERT(holds(b.p, b.n, (CONST char *)tc_addr_get(db, k, TC_F_NAME)));
	/* nothing planned: nothing to print */
	tc_clear(db);
	ct_buf(&b, buf, BUF_MAX);
	KT_ASSERT_ER(tc_print_plans(db, &b, 0), E_NOEXS);
	/* the letters of what the tray holds */
	n = tc_doc_text((CONST UB *)"<tad><document><p><font size=\"9\"/>A&amp;B<tab/>C</p><p>next</p></document></tad>",
			s_len((CONST UB *)"<tad><document><p><font size=\"9\"/>A&amp;B<tab/>C</p><p>next</p></document></tad>"), t, sizeof(t));
	KT_ASSERT(n == 5 && s_eq(t, "A&B C"));
	tc_clear(db);
	Kfree(buf);
	Kfree(db);
}

/* ---------------------------------------------------------------- the template in the store */

LOCAL UB *rec0_of( CONST char *uuid, INT *p_len )
{
	TS_UUID	u;
	T_OBREC	r[8];
	INT	cnt = 0, i, at = 0;
	UD	size = 0;
	SZ	asz = 0;
	UB	*buf = NULL;
	ID	k;

	*p_len = 0;
	if ( ts_str_to_uuid(uuid, &u) < E_OK || ( k = ob_opn_obj(&u, OB_OP_R) ) <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 8, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 8; i++ ) {
			if ( r[i].recno == 0 ) size = r[i].size;
		}
	}
	if ( size > 0 && size < 1024 * 1024 ) buf = Kmalloc((SZ)size + 1);
	while ( buf != NULL && (UD)at < size ) {
		if ( ob_rea_rec(k, 0, (D)at, buf + at, (SZ)( size - at ), &asz) < E_OK || asz <= 0 ) break;
		at += (INT)asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL && (UD)at != size ) {
		Kfree(buf);
		return NULL;
	}
	*p_len = (INT)size;
	return buf;
}

LOCAL void test_template( void )
{
	TCDB	*db = Kmalloc(sizeof(TCDB));
	UB	*x;
	INT	n = 0;

	if ( db == NULL ) KT_SKIP("no memory");
	x = rec0_of(SYSDEF_TMPL_TECHO, &n);
	if ( x == NULL ) {
		Kfree(db);
		KT_SKIP("no template 手帳用紙 in the store");
	}
	tc_init(db);
	KT_ASSERT_ER(tc_read(db, x, n), E_OK);
	KT_ASSERT(db->np == 0 && db->na == 0 && db->dropped == 0);
	KT_ASSERT_EQ(db->week, 1U << 0);			/* Sundays */
	KT_ASSERT(db->jp && db->nhl == 2 && db->nr == 0);	/* "祝日": reckoned, not listed */
	KT_ASSERT(tc_holiday(db, 2026, 1, 1));			/* 元日 */
	KT_ASSERT(tc_holiday(db, 2026, 1, 12));			/* 成人の日 */
	KT_ASSERT(tc_holiday(db, 1995, 1, 15) && !tc_holiday(db, 2005, 1, 15));
	KT_ASSERT(tc_holiday(db, 2026, 3, 20));			/* 春分の日 */
	KT_ASSERT(tc_holiday(db, 2026, 5, 6));			/* 振替休日 */
	KT_ASSERT(tc_holiday(db, 2026, 9, 21) && tc_holiday(db, 2026, 9, 22) && tc_holiday(db, 2026, 9, 23));
	KT_ASSERT(tc_holiday(db, 2026, 10, 12));			/* スポーツの日 */
	KT_ASSERT(tc_holiday(db, 2020, 7, 24) && !tc_holiday(db, 2020, 10, 12));
	KT_ASSERT(tc_holiday(db, 2018, 12, 23) && !tc_holiday(db, 2019, 12, 23));
	KT_ASSERT(tc_holiday(db, 2019, 5, 1) && tc_holiday(db, 2019, 4, 30));
	KT_ASSERT(!tc_holiday(db, 2026, 10, 13) && !tc_holiday(db, 2026, 10, 3));
	Kfree(x);
	tc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- the program */

LOCAL INT win_named( CONST char *title, INT ms )
{
	INT	wid, k;
	char	t[WM_TITLE_MAX];

	for ( ; ms > 0; ms -= 100 ) {
		for ( wid = 1; wid <= knl_wm_nwin(); wid++ ) {
			if ( wm_title(wid, t, sizeof(t)) < 0 ) continue;
			for ( k = 0; title[k] != 0 && t[k] == title[k]; k++ ) ;
			if ( title[k] == 0 && t[k] == 0 ) return wid;
		}
		tk_dly_tsk(100);
	}
	return 0;
}

LOCAL void key_to( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.code = code;
	ev.mods = mods;
	ev.wid = wid;
	knl_wmobj_event(&ev);
	tk_dly_tsk(30);
	ev.type = HID_EV_KEY_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(150);
}

/* The metadata of an object, into buf (max bytes); its length, 0 none */
LOCAL INT meta_get( CONST TS_UUID *u, UB *buf, INT max )
{
	SZ	asz = 0;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	buf[0] = 0;
	if ( k <= 0 ) return 0;
	if ( ob_get_atr(k, buf, max - 1, &asz) < E_OK || asz < 0 ) asz = 0;
	ob_cls_obj(k);
	buf[asz] = 0;
	return (INT)asz;
}

LOCAL ER meta_set( CONST TS_UUID *u, CONST UB *buf, INT n )
{
	ID	k = ob_opn_obj(u, OB_OP_ATRRD | OB_OP_ATRWR);
	ER	er;

	if ( k <= 0 ) return (ER)k;
	er = ob_set_atr(k, buf, (SZ)n);
	ob_cls_obj(k);
	return er;
}

/* The sample's metadata as it was before the program ran, put back after */
LOCAL UB	*meta0;
LOCAL INT	nmeta0;

/* The program on the sample, its process, once its windows (those named non-zero) are there */
LOCAL ID techo_start( INT *ws, INT *wc, INT *wa )
{
	TS_UUID	prog, pu, obj;
	T_OBCRE	c;

	if ( ts_str_to_uuid(SYSDEF_PROG_TECHO, &prog) < E_OK || ts_str_to_uuid(SAMPLE, &obj) < E_OK ) return 0;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &obj;
	c.argsz = sizeof(obj);
	if ( ob_cre_obj(&c, &pu) < E_OK ) return 0;
	*ws = win_named("私の手帳（予定表）", 30000);
	*wc = win_named("私の手帳（カレンダー）", ( *ws > 0 ) ? 3000 : 10000);
	*wa = win_named("私の手帳（住所録）", 10000);
	return knl_prc_of_uuid(&pu);
}

LOCAL void test_program( void )
{
	TS_UUID	prog, pu, obj;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	INT	ws, wc, wa;
	ER	er = E_OK;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_TECHO, &prog), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &obj), E_OK);
	meta0 = (UB *)Kmalloc(KT_META_ROOM + 1);
	if ( meta0 != NULL ) nmeta0 = meta_get(&obj, meta0, KT_META_ROOM + 1);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &obj;
	c.argsz = sizeof(obj);
	if ( ob_cre_obj(&c, &pu) < E_OK ) KT_SKIP("no program object of 電子手帳");
	pid = knl_prc_of_uuid(&pu);
	ws = win_named("私の手帳（予定表）", 30000);
	wc = win_named("私の手帳（カレンダー）", 10000);
	wa = win_named("私の手帳（住所録）", 10000);
	KT_ASSERT(pid > 0 && ws > 0 && wc > 0 && wa > 0);
	if ( pid <= 0 || ws <= 0 ) goto stop;
	(void)wm_focus(ws);
	tk_dly_tsk(500);
	key_to(ws, 0x4E, 0);				/* PgDn: the next day */
	key_to(wa, 0x4E, 0);				/* the next person */
	key_to(wc, 0x4B, 0);				/* PgUp: the month before */
	tk_dly_tsk(300);
	/*
	 * Letters dropped from outside, the outline's corner over 住所3 of
	 * the person shown: they go into the field, and are in the record
	 * once it is saved. The record is put back as it was afterwards.
	 */
	{
		static CONST char	drop[] = "ドロップ試験 1-2-3";
		T_WMWIN			w;
		UB			*was, *now;
		INT			nwas = 0, nnow = 0;

		was = rec0_of(SAMPLE, &nwas);
		KT_ASSERT_ER(wm_obj_drop_text(wa, 100, 100, 0, NULL, (CONST UB *)drop, 0), E_PAR);
		KT_ASSERT(wm_ref(wa, &w) >= E_OK);
		KT_ASSERT_ER(wm_obj_drop_text(wa, w.work.left + 70, w.work.top + 100, 0, NULL,
					      (CONST UB *)drop, (INT)sizeof(drop) - 1), E_OK);
		tk_dly_tsk(500);
		key_to(wa, 0x16, HID_MOD_LCTRL);	/* Ctrl+S */
		tk_dly_tsk(800);
		now = rec0_of(SAMPLE, &nnow);
		KT_ASSERT(now != NULL && holds(now, nnow, "<tab/><tab/>ドロップ試験 1-2-3<tab/>加藤 健二"));
		if ( was != NULL ) {
			TS_UUID	u;
			ID	k;
			SZ	asz = 0;

			if ( ts_str_to_uuid(SAMPLE, &u) >= E_OK && ( k = ob_opn_obj(&u, OB_OP_R | OB_OP_W) ) > 0 ) {
				(void)ob_wri_rec(k, 0, 0, was, (SZ)nwas, &asz);
				(void)ob_trn_rec(k, 0, (UD)nwas);
				ob_cls_obj(k);
			}
			Kfree(was);
		}
		if ( now != NULL ) Kfree(now);
	}
	(void)kt_shot("/boot/TECHO0.PPM");
	key_to(ws, 0x08, HID_MOD_LCTRL);		/* Ctrl+E: nothing changed since saved, closed */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 15000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named("私の手帳（予定表）", 300), 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
	if ( meta0 != NULL && nmeta0 > 0 ) (void)meta_set(&obj, meta0, nmeta0);
}

/*
 * The windows as they were left: their places and which were shown come
 * from the metadata (the カレンダー put away, the other two where it
 * says), and closing writes them back as they stand then (the 住所録
 * moved), the rest of the metadata as it was.
 */
LOCAL void test_places( void )
{
	static CONST char kept[] = ",\"tessronos\":{\"techo\":{\"windows\":[{\"x\":40,\"y\":50,\"open\":true},"
				   "{\"x\":300,\"y\":60,\"open\":false},{\"x\":480,\"y\":290,\"open\":true}]}}}";
	TS_UUID		obj;
	T_WMWIN		w;
	T_PSTS		psts;
	T_DPRECT	o;
	UB		*m = NULL;
	INT		ws = 0, wc = 0, wa = 0, n, e, i;
	ID		pid = 0;
	ER		er = E_OK;

	if ( meta0 == NULL || nmeta0 <= 0 ) KT_SKIP("no metadata of the sample");
	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &obj), E_OK);
	m = (UB *)Kmalloc(KT_META_ROOM + 1);
	if ( m == NULL ) KT_SKIP("no memory");

	/* the sample's metadata with the windows as they were left */
	for ( e = nmeta0; e > 0 && meta0[e - 1] != '}'; e-- ) ;
	KT_ASSERT(e > 0);
	if ( e <= 0 ) goto out;
	for ( i = 0; i < e - 1; i++ ) m[i] = meta0[i];
	for ( n = 0; kept[n] != 0; n++ ) m[i + n] = (UB)kept[n];
	n += i;
	KT_ASSERT_ER(meta_set(&obj, m, n), E_OK);

	pid = techo_start(&ws, &wc, &wa);
	KT_ASSERT(pid > 0 && ws > 0 && wa > 0);
	KT_ASSERT_EQ(wc, 0);					/* put away when it was left */
	if ( pid <= 0 || ws <= 0 || wa <= 0 ) goto stop;
	KT_ASSERT(wm_ref(ws, &w) >= E_OK && w.outer.left == 40 && w.outer.top == 50);
	KT_ASSERT(wm_ref(wa, &w) >= E_OK && w.outer.left == 480 && w.outer.top == 290);
	(void)kt_shot("/boot/TECHO1.PPM");

	/* the 住所録 moved, and the 手帳 closed */
	o = w.outer;
	o.left += 30;
	o.right += 30;
	o.top += 20;
	o.bottom += 20;
	KT_ASSERT_ER(wm_move(wa, &o), E_OK);
	tk_dly_tsk(500);
	key_to(ws, 0x08, HID_MOD_LCTRL);			/* Ctrl+E */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 15000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	n = meta_get(&obj, m, KT_META_ROOM + 1);
	KT_ASSERT(holds(m, n, "\"techo\":{\"windows\":[{\"x\":40,\"y\":50,\"open\":true},"
			      "{\"x\":300,\"y\":60,\"open\":false},{\"x\":510,\"y\":310,\"open\":true}]}"));
	KT_ASSERT(holds(m, n, "\"name\":\"私の手帳\"") || holds(m, n, "\"name\": \"私の手帳\""));
	KT_ASSERT(holds(m, n, "\"tessronos-techo\""));
	if ( !holds(m, n, "\"x\":510") ) tm_putstring(m);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	(void)meta_set(&obj, meta0, nmeta0);
	Kfree(m);
}

EXPORT void ktest_techo( void )
{
	tm_printf((UB *)"\n[techo] 電子手帳 (design 11.23)\n");
	KT_RUN(test_read);
	KT_RUN(test_write);
	KT_RUN(test_plans);
	KT_RUN(test_people);
	KT_RUN(test_holidays);
	KT_RUN(test_jp);
	KT_RUN(test_search);
	KT_RUN(test_print);
	KT_RUN(test_template);
	KT_RUN_SCREEN(test_program);
	KT_RUN_SCREEN(test_places);
	if ( meta0 != NULL ) {
		Kfree(meta0);
		meta0 = NULL;
	}
}
