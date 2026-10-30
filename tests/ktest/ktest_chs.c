/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_chs.c
 *	文字検索 (design 11.19): the dictionary, the searches, the codes and
 *	typing a character
 *
 *	The dictionary object the system brings (SYSDEF_DICT_CHAR) is read
 *	and opened with the accessory's own code (application/charsearch/
 *	cs_dic.c and cs_code.c). The character sets and their codes are
 *	checked against places whose codes are known: 亜, ①, 號 and the
 *	first characters of each set's row 16, Shift_JIS-2004 of JIS X 0213's
 *	second plane, the TRON code of a character only GB 2312 has, and the
 *	codes as the code list's box reads them. The searches are checked
 *	with conditions of every kind (虎 号 ., 倹-イ, 木×2, =n, a
 *	reading, a katakana for an element) and those of 記号検索 (a name in
 *	English and in Japanese, romaji, parts, script and class), as far as
 *	the sources the dictionary was made from allow.
 *
 *	Typing: letters are put into the box that has the keys in a panel
 *	(wm_panel_type), and refused by a box of ASCII. Then the program is
 *	started: conditions put into its box that way are searched with
 *	Enter, and its 入力 (Ctrl+I), with no desktop to type into, puts the
 *	character found in the tray, as does the テキスト形式TRONコード
 *	(Ctrl+T); Ctrl+E closes it.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/tray.h>
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/charsearch/cs_dic.h"

#define RES_MAX		4096

LOCAL CDIC	dic;
LOCAL UB	*dic_rec = NULL;
LOCAL UINT	*res = NULL;

LOCAL BOOL same( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

LOCAL BOOL holds( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, m = (INT)knl_strlen(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL has( CONST UINT *r, INT n, UINT cp )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( r[i] == cp ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL need( UINT src )
{
	return (BOOL)( dic_rec != NULL && ( dic.src & src ) == src );
}

/* ---------------------------------------------------------------- the dictionary */

LOCAL void test_dic( void )
{
	TS_UUID	u;
	SZ	size = 0;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_DICT_CHAR, &u), E_OK);
	dic_rec = om_obj_record(&u, 1, &size);
	if ( dic_rec == NULL ) KT_SKIP("no dictionary object of 文字検索");
	KT_ASSERT_ER(cd_open(&dic, dic_rec, size), E_OK);
	tm_printf((UB *)"  %d characters, %d elements, sources %x, %d bytes\n", dic.n, dic.nelem, dic.src, (INT)size);
	KT_ASSERT(dic.n > 20000);
	KT_ASSERT(cd_find(&dic, 0x4E9C) != NULL);			/* 亜 */
	KT_ASSERT(cd_find(&dic, 0x3042) != NULL);			/* あ */
	KT_ASSERT(cd_find(&dic, 0x0041) != NULL);			/* A */
	KT_ASSERT(cd_find(&dic, 0xE000) == NULL);			/* private use */
	KT_ASSERT_EQ(cd_open(&dic, dic_rec, 63), E_OBJ);
	KT_ASSERT_ER(cd_open(&dic, dic_rec, size), E_OK);
	res = (UINT *)Kmalloc(sizeof(UINT) * RES_MAX);
	KT_ASSERT(res != NULL);
}

/* ---------------------------------------------------------------- the sets and the codes */

LOCAL void test_codes( void )
{
	CSPLACE	p[8];
	char	s[32];
	UINT	u[2];
	INT	n, kind, plane, row, cell, tp;
	UINT	tc;

	if ( dic_rec == NULL ) KT_SKIP("no dictionary");
	/* 亜: 日本基本 1-16-01, Shift_JIS 889F, EUC B0A1, TRON 1-3021 */
	n = cs_places(&dic, 0x4E9C, p, 8);
	KT_ASSERT(n >= 1);
	KT_ASSERT_EQ(p[0].set, CS_JIS);
	KT_ASSERT_EQ(p[0].kind, CS_K_JIS12);
	cs_place_text(&p[0], s);
	KT_ASSERT(same(s, "1-16-01"));
	KT_ASSERT_EQ(cs_sjis(&p[0]), 0x889F);
	KT_ASSERT_EQ(cs_euc(&p[0]), 0xB0A1);
	cs_tron_text(0x4E9C, s);
	KT_ASSERT(same(s, "&T213021;"));
	KT_ASSERT_EQ(cs_set_of(&dic, 0x4E9C), CS_SET_JIS);

	/* ①: JIS X 0213's row 13 (水色), 8740; also GB 2312 and KS X 1001 have it */
	n = cs_places(&dic, 0x2460, p, 8);
	KT_ASSERT(n >= 3);
	KT_ASSERT_EQ(p[0].kind, CS_K_JIS3);
	KT_ASSERT_EQ(cs_sjis(&p[0]), 0x8740);

	/* 號: zone A, and 韓国 has it too */
	n = cs_places(&dic, 0x865F, p, 8);
	KT_ASSERT(n >= 2);
	cs_tron_text(0x865F, s);
	KT_ASSERT(same(s, "&T21694B;"));

	/* 汉: only GB 2312 -- its TRON code is zone C's, not plane 16's */
	KT_ASSERT_ER(cs_tron(0x6C49, &tp, &tc), E_OK);
	KT_ASSERT_EQ(tp, 1);
	KT_ASSERT_EQ(tc, 0x33EB);
	KT_ASSERT_EQ(cs_set_of(&dic, 0x6C49), CS_SET_GB);
	KT_ASSERT_EQ(cs_kind_of(&dic, 0x6C49), CS_K_GB);
	KT_ASSERT_EQ(cs_set_of(&dic, 0xD55C), CS_SET_KS);		/* 한 */
	KT_ASSERT_EQ(cs_set_of(&dic, 0x0416), CS_SET_JIS);		/* Ж: JIS X 0208 row 7 */
	KT_ASSERT_EQ(cs_set_of(&dic, 0x0E01), CS_SET_UCS);		/* ก: 各国 */

	/* the first of each set's row 16 */
	KT_ASSERT_EQ(cs_cell(&dic, CS_JIS, 1, 16, 1, u, &kind), 1);
	KT_ASSERT_EQ(u[0], 0x4E9C);
	KT_ASSERT_EQ(cs_cell(&dic, CS_JIS, 1, 14, 1, u, &kind), 1);	/* JIS X 0213's 1-14-01 */
	KT_ASSERT_EQ(kind, CS_K_JIS3);
	KT_ASSERT_EQ(cs_cell(&dic, CS_JIS, 2, 1, 1, u, &kind), 1);	/* 2-1-01, past the BMP */
	KT_ASSERT_EQ(u[0], 0x20089);
	KT_ASSERT_EQ(kind, CS_K_JIS4);
	KT_ASSERT_EQ(cs_cell(&dic, CS_JIS, 2, 2, 1, u, &kind), 0);	/* a row the second plane lacks */
	KT_ASSERT_EQ(cs_cell(&dic, CS_HOJO, 0, 16, 1, u, &kind), 1);
	KT_ASSERT_EQ(u[0], 0x4E02);
	KT_ASSERT_EQ(cs_cell(&dic, CS_GB, 0, 16, 1, u, &kind), 1);
	KT_ASSERT_EQ(u[0], 0x554A);
	KT_ASSERT_EQ(cs_cell(&dic, CS_KS, 0, 16, 1, u, &kind), 1);
	KT_ASSERT_EQ(u[0], 0xAC00);
	KT_ASSERT_EQ(cs_cell(&dic, CS_TRON, 1, 0x30, 0, u, &kind), 1);
	KT_ASSERT_EQ(u[0], 0x4E9C);
	KT_ASSERT_EQ(cs_cell(&dic, CS_JIS, 1, 4, 87, u, &kind), 2);	/* か゚: a letter and its mark */
	KT_ASSERT_EQ(u[0], 0x304B);
	KT_ASSERT_EQ(u[1], 0x309A);
	KT_ASSERT_EQ(cs_from_tron(1, 0x3021), 0x4E9C);

	/* Shift_JIS-2004 of the second plane: 2-1-01 F040, 2-8-01 F09F, 2-94-94 FCFC */
	{
		CSPLACE	q;

		knl_memset(&q, 0, sizeof(q));
		q.set = CS_JIS;
		q.plane = 2;
		q.row = 1;
		q.cell = 1;
		KT_ASSERT_EQ(cs_sjis(&q), 0xF040);
		KT_ASSERT_EQ(cs_euc(&q), 0x8FA1A1);
		q.row = 8;
		KT_ASSERT_EQ(cs_sjis(&q), 0xF09F);
		q.row = 94;
		q.cell = 94;
		KT_ASSERT_EQ(cs_sjis(&q), 0xFCFC);
	}

	/* the codes as the code list's box reads them */
	KT_ASSERT(cs_parse_code(CS_JIS, "1-16-1", &plane, &row, &cell));
	KT_ASSERT(plane == 1 && row == 16 && cell == 1);
	KT_ASSERT(cs_parse_code(CS_JIS, "16-01", &plane, &row, &cell));
	KT_ASSERT(plane == 1 && row == 16 && cell == 1);
	KT_ASSERT(cs_parse_code(CS_JIS, "2-94-94", &plane, &row, &cell));
	KT_ASSERT(plane == 2 && row == 94 && cell == 94);
	KT_ASSERT(!cs_parse_code(CS_JIS, "16-95", &plane, &row, &cell));
	KT_ASSERT(!cs_parse_code(CS_HOJO, "78-1", &plane, &row, &cell));
	KT_ASSERT(cs_parse_code(CS_UCS, "U+3042", &plane, &row, &cell));
	KT_ASSERT(row == 0x30 && cell == 0x42);
	KT_ASSERT(cs_parse_code(CS_TRON, "&T213021;", &plane, &row, &cell));
	KT_ASSERT(plane == 1 && row == 0x30 && cell == 0);
	KT_ASSERT(cs_parse_code(CS_TRON, "1 33EB", &plane, &row, &cell));
	KT_ASSERT(plane == 1 && row == 0x33 && cell == 94 + 0x6B);
	KT_ASSERT(!cs_parse_code(CS_TRON, "&T21;", &plane, &row, &cell));
}

/* ---------------------------------------------------------------- 漢字検索 */

LOCAL INT kanji( CONST char *q, UINT sets )
{
	INT	total = 0, n = cs_kanji(&dic, q, sets, res, RES_MAX, &total);

	tm_printf((UB *)"  漢字検索 \"%s\": %d\n", q, n);
	return n;
}

LOCAL void test_kanji( void )
{
	INT	n;

	if ( dic_rec == NULL || res == NULL ) KT_SKIP("no dictionary");
	KT_ASSERT_EQ(kanji("", CS_SET_JIS), CS_E_EMPTY);
	KT_ASSERT_EQ(kanji("abc", CS_SET_JIS), CS_E_SYNTAX);
	KT_ASSERT_EQ(kanji("=5-", CS_SET_JIS), CS_E_SYNTAX);
	KT_ASSERT_EQ(kanji("=9~3", CS_SET_JIS), CS_E_SYNTAX);

	/* the strokes of the whole: the six of one stroke in 日本基本, 一 first */
	n = kanji("=1", CS_SET_JIS);
	KT_ASSERT_EQ(n, 6);
	KT_ASSERT(has(res, n, 0x4E00) && has(res, n, 0x4E59) && has(res, n, 0x4E3F));
	n = kanji("＝１", CS_SET_JIS);				/* written in full width */
	KT_ASSERT_EQ(n, 6);
	n = kanji("=2+1", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x5DDD) && has(res, n, 0x4EBA) && !has(res, n, 0x6728));	/* 川 人, not 木 */
	n = kanji("=5～6", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x6728) == FALSE && has(res, n, 0x4E16));			/* 世 */

	if ( !need(CD_SRC_KANJIDIC) ) {
		tm_printf((UB *)"  KANJIDIC2 was not used: no readings\n");
	} else {
		n = kanji("やま", CS_SET_JIS);
		KT_ASSERT(has(res, n, 0x5C71));				/* 山 */
		n = kanji("こう =4", CS_SET_JIS);
		KT_ASSERT(has(res, n, 0x516C) && has(res, n, 0x5B54));	/* 公 孔 */
		n = kanji("つぐ", CS_SET_JIS);
		KT_ASSERT(has(res, n, 0x4E9C));				/* 亜: つ.ぐ */
		n = kanji("あ =7", CS_SET_JIS);
		KT_ASSERT(n > 0);
		KT_ASSERT_EQ(res[0], 0x4E9C);
	}
	if ( !need(CD_SRC_KANJIVG | CD_SRC_KRAD) ) {
		tm_printf((UB *)"  KanjiVG or KRADFILE was not used: no elements\n");
		return;
	}
	/* elements: given, less one, counted, of n strokes, and those alone */
	n = kanji("虎 号", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x865F) && has(res, n, 0x9955));	/* 號 饕 */
	n = kanji("虎 号 .", CS_SET_JIS);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT_EQ(res[0], 0x865F);
	n = kanji("虎 号 。", CS_SET_JIS);
	KT_ASSERT_EQ(n, 1);
	n = kanji("倹-イ", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x5263) && has(res, n, 0x691C) && has(res, n, 0x967A) && has(res, n, 0x9A13));
	n = kanji("木×2", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x6797) && has(res, n, 0x5F6C) && !has(res, n, 0x68EE));	/* 林 彬, not 森 */
	n = kanji("木*3", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x68EE) && !has(res, n, 0x6797));
	n = kanji("口 =9", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x54C0) && has(res, n, 0x54BD));	/* 哀 咽 */
	n = kanji("口 9", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x97FB));				/* 韻: 音 */
	n = kanji("シ =5", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x6C41));				/* 汁: 氵 */
	n = kanji("豊", CS_SET_JIS);
	KT_ASSERT(has(res, n, 0x8C50) && has(res, n, 0x8C4A));	/* 豐 too, a variant */
	/* the sets searched */
	n = kanji("=1", CS_SET_GB);
	KT_ASSERT(!has(res, n, 0x4E9C));
}

/* ---------------------------------------------------------------- 記号検索 */

LOCAL INT symbol( CONST char *name, CONST char *parts, INT script, INT cls )
{
	INT	total = 0, n = cs_symbol(&dic, name, parts, script, cls, CS_SET_ALL, res, RES_MAX, &total);

	tm_printf((UB *)"  記号検索 \"%s\" \"%s\" %d %d: %d\n", name, parts, script, cls, n);
	return n;
}

LOCAL void test_symbol( void )
{
	INT	n;

	if ( dic_rec == NULL || res == NULL ) KT_SKIP("no dictionary");
	KT_ASSERT_EQ(symbol("", "", 0, 0), CS_E_EMPTY);
	n = symbol("ハート", "", 0, 0);
	KT_ASSERT(has(res, n, 0x2665) && has(res, n, 0x2661));	/* ♥ ♡ */
	n = symbol("heart", "", 0, 0);
	KT_ASSERT(has(res, n, 0x2665));
	n = symbol("KA", "", 0, 0);
	KT_ASSERT(has(res, n, 0x304B) && has(res, n, 0x30AB) && has(res, n, 0x041A));	/* か カ К */
	KT_ASSERT(!has(res, n, 0x304C));			/* not が */
	n = symbol("チルド U", "", 0, 0);
	KT_ASSERT(has(res, n, 0x0168) && has(res, n, 0x0169) && !has(res, n, 0x00C3));	/* Ũ ũ, not Ã */
	n = symbol("", "◎ 1", 0, 0);
	KT_ASSERT_EQ(n, 2);
	KT_ASSERT(has(res, n, 0x24F5) && has(res, n, 0x24FE));	/* ⓵ ⓾ */
	n = symbol("IPA", "", 0, 0);
	KT_ASSERT(has(res, n, 0x0250));
	n = symbol("", "", CD_SC_GREEK, CD_CL_CASE);
	KT_ASSERT(has(res, n, 0x0391) && !has(res, n, 0x0041));	/* Α, not A */
	n = symbol("", "", 0, CD_CL_UNIT);
	KT_ASSERT(has(res, n, 0x2103) && has(res, n, 0x00A5));	/* ℃ ¥ */
	n = symbol("A", "", CD_SC_LATIN, CD_CL_CASE);
	KT_ASSERT(has(res, n, 0x0041) && has(res, n, 0x0061));
	n = symbol("", "", CD_SC_JAPAN, CD_CL_LETTER);
	KT_ASSERT(has(res, n, 0x3042) && !has(res, n, 0x4E9C));	/* あ; no kanji */
}

/* ---------------------------------------------------------------- 文字情報 */

LOCAL void test_info( void )
{
	CONST CDENT	*e;
	CONST UB	*p;
	char		r[8];
	INT		len, i;
	BOOL		simp = FALSE;

	if ( !need(CD_SRC_KANJIDIC | CD_SRC_UNIHAN) ) KT_SKIP("the dictionary was made without KANJIDIC2 or Unihan");
	e = cd_find(&dic, 0x4E9C);
	KT_ASSERT(e != NULL);
	if ( e == NULL ) return;
	KT_ASSERT((e->flags & CD_F_KANJI) != 0);
	KT_ASSERT((e->flags & CD_F_JOYO) != 0);
	KT_ASSERT_EQ(e->strokes, 7);
	KT_ASSERT_EQ(e->radical, 7);
	cd_radical(e->radical, r);
	KT_ASSERT(same(r, "二"));
	len = cd_field(&dic, e, CD_T_ON, &p);
	KT_ASSERT(len > 0 && holds(p, len, "あ"));
	len = cd_field(&dic, e, CD_T_KUN, &p);
	KT_ASSERT(len > 0 && holds(p, len, "つ.ぐ"));
	/* 號's simplified form is 号 */
	e = cd_find(&dic, 0x865F);
	len = cd_field(&dic, e, CD_T_VARS, &p);
	for ( i = 0; i + 4 <= len; i += 4 ) {
		UINT	v = (UINT)p[i] | ( (UINT)p[i + 1] << 8 ) | ( (UINT)p[i + 2] << 16 ) | ( (UINT)p[i + 3] << 24 );

		if ( ( v >> 24 ) == CD_V_SIMP && ( v & 0xFFFFFF ) == 0x53F7 ) simp = TRUE;
	}
	KT_ASSERT(simp);
	/* A: its name, and a as its small letter */
	e = cd_find(&dic, 0x0041);
	len = cd_field(&dic, e, CD_T_NAME, &p);
	KT_ASSERT(len > 0 && holds(p, len, "LATIN CAPITAL LETTER A"));
	KT_ASSERT_EQ(cd_strokes(&dic, 0x6728), 4);		/* 木 */
	KT_ASSERT(dic.nelem > 200);
}

/* ---------------------------------------------------------------- typing */

LOCAL void part_box( T_WMPART *pt, INT num, UINT type, INT l, INT t, INT r, INT b )
{
	knl_memset(pt, 0, sizeof(*pt));
	pt->type = type;
	pt->num = num;
	pt->r.left = l;
	pt->r.top = t;
	pt->r.right = r;
	pt->r.bottom = b;
	pt->answer = WM_ANS_NONE;
	pt->top = 1;
	pt->sb_at = -1;
}

/* Letters put into the box that has the keys, as a committed conversion is */
LOCAL void test_type_box( void )
{
	T_WMPANEL	*d;
	T_DPRECT	o;
	INT		wid, pid;
	UB		buf[WM_LABEL_MAX];

	o.left = 100;
	o.top = 100;
	o.right = 500;
	o.bottom = 260;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "chs");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	d = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	KT_ASSERT(d != NULL);
	if ( d == NULL ) {
		(void)wm_close(wid);
		return;
	}
	knl_memset(d, 0, sizeof(*d));
	d->num = 1;
	d->kind = WM_PNL_PLAIN;
	d->r.left = 0;
	d->r.top = 0;
	d->r.right = 380;
	d->r.bottom = 100;
	d->npart = 1;
	part_box(&d->part[0], 1, WM_PT_BOX | P_JP, 10, 10, 300, 36);
	pid = wm_panel_open(wid, d);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		KT_ASSERT(wm_panel_type(wid, (CONST UB *)"亜", 3));
		KT_ASSERT(wm_panel_type(wid, (CONST UB *)"&T213021;", 9));
		KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
		KT_ASSERT(same((char *)buf, "亜&T213021;"));
		KT_ASSERT(!wm_panel_type(wid + 1, (CONST UB *)"亜", 3));	/* no such panel */
		KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	}
	/* a box of ASCII takes no kanji */
	part_box(&d->part[0], 1, WM_PT_BOX | P_ASCII, 10, 10, 300, 36);
	pid = wm_panel_open(wid, d);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		KT_ASSERT(!wm_panel_type(wid, (CONST UB *)"亜", 3));
		KT_ASSERT(wm_panel_type(wid, (CONST UB *)"&T213021;", 9));
		KT_ASSERT_ER(wm_panel_text(pid, 1, buf, sizeof(buf)), E_OK);
		KT_ASSERT(same((char *)buf, "&T213021;"));
		KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	}
	Kfree(d);
	(void)wm_close(wid);
}

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
	tk_dly_tsk(80);
}

LOCAL BOOL tray_holds( CONST char *w, INT ms )
{
	T_TRINF	inf;
	UB	b[512];
	SZ	asz = 0;

	for ( ; ms > 0; ms -= 100 ) {
		if ( tr_ref_rec(TR_HAND, 0, &inf) >= E_OK && inf.kind == TR_TAD
		  && tr_rea_rec(TR_HAND, 0, b, sizeof(b), &asz) >= E_OK
		  && holds(b, (INT)( ( asz < (SZ)sizeof(b) ) ? asz : (SZ)sizeof(b) ), w) ) {
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  the tray does not hold \"%s\"\n", w);
	return FALSE;
}

/* The program: conditions put into its box, searched, the character found typed into the tray */
LOCAL void test_program( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	INT	wid;
	ER	er = E_SYS;

	if ( !need(CD_SRC_KANJIVG | CD_SRC_KRAD) ) KT_SKIP("the dictionary has no elements");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_CHARSRCH, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "kanji:";
	c.argsz = 6;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of 文字検索");
	}
	pid = knl_prc_of_uuid(&pu);
	wid = win_named("文字検索", 20000);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto stop;
	(void)wm_focus(wid);
	tk_dly_tsk(800);

	/* the conditions put into the box that has the keys, and Enter: 號 */
	KT_ASSERT(wm_panel_type(wid, (CONST UB *)"虎 号 .", (INT)knl_strlen("虎 号 .")));
	tk_dly_tsk(200);
	key_to(wid, 0x28, 0);					/* Enter: 検索 */
	tk_dly_tsk(500);
	(void)kt_shot("/boot/CHS0.PPM");
	key_to(wid, 0x0C, HID_MOD_LCTRL);			/* Ctrl+I: 入力 */
	KT_ASSERT(tray_holds("<p>號</p>", 5000));
	key_to(wid, 0x17, HID_MOD_LCTRL);			/* Ctrl+T: テキスト形式TRONコード */
	KT_ASSERT(tray_holds("<p>&amp;T21694B;</p>", 5000));
	key_to(wid, 0x21, HID_MOD_LCTRL);			/* Ctrl+4: 文字情報 */
	tk_dly_tsk(500);
	(void)kt_shot("/boot/CHS1.PPM");

	key_to(wid, 0x08, HID_MOD_LCTRL);			/* Ctrl+E: closed */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 10000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named("文字検索", 300), 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
}

EXPORT void ktest_chs( void )
{
	tm_printf((UB *)"\n[chs] 文字検索 (design 11.19)\n");
	KT_RUN(test_dic);
	KT_RUN(test_codes);
	KT_RUN(test_kanji);
	KT_RUN(test_symbol);
	KT_RUN(test_info);
	KT_RUN_SCREEN(test_type_box);
	KT_RUN_SCREEN(test_program);
	if ( res != NULL ) Kfree(res);
	if ( dic_rec != NULL ) Kfree(dic_rec);
	res = NULL;
	dic_rec = NULL;
}
