/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_mcard.c
 *	マイクロカード (design 11.18): the records, the cards, searching,
 *	sorting, marks, duplicates, writing out, reading in and merging
 *
 *	The engine of the program (application/mcard: mc_db.c, mc_bin.c,
 *	mc_rx.c, mc_var.c, mc_io.c, mc_merge.c) makes no system call, so it
 *	is linked here as it is. The records of マイクロカード実身 are made
 *	byte by byte as マイクロカード実身データ構造説明書 lays them out and
 *	read; a database is written and its bytes checked word by word
 *	against the same layout, then read back. Then the format and its
 *	changes, typing into the fields, every kind of condition (the signs,
 *	the wild letters, regular expressions, 異形字ゆらぎ同一視), narrowing
 *	and the marks searched, the order of letters and of numbers, a sort
 *	on two keys and in the search state, 重複検索, @形式, CSV, tabs and a
 *	sheet out and in again, and the % letters of a text, a figure and the
 *	list merged page by page. Last the program is started on the sample
 *	住所録 and its two windows looked for.
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
#include "../../application/mcard/mc_db.h"

#define SAMPLE		"01a0d8c4-5336-7a17-8c28-d3e4f5a6b736"
#define BUF_MAX		( 128 * 1024 )

/* ---------------------------------------------------------------- what the engine asks */

EXPORT void *mc_alloc( INT size )
{
	return Kmalloc((SZ)( size > 0 ? size : 1 ));
}

EXPORT void mc_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

EXPORT INT mc_link_name( CONST UB *el, INT len, UB *out, INT max )
{
	TS_UUID	u;
	T_OBREF	ref;
	INT	n = 0;

	if ( max <= 0 ) return 0;
	out[0] = 0;
	if ( !ct_target(el, len, &u) || ob_ref_obj(&u, &ref) < E_OK ) return 0;
	while ( n < max - 1 && n < OB_NAME_MAX && ref.name[n] != 0 ) {
		out[n] = ref.name[n];
		n++;
	}
	out[n] = 0;
	return n;
}

EXPORT void mc_new_vobjid( char *out37 )
{
	TS_UUID	u;

	knl_memset(&u, 0, sizeof(u));
	(void)ts_gen_uuid(&u);
	ct_uuid_str(&u, out37);
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

/* Whether s[0..n) holds w */
LOCAL BOOL holds( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; w[k] != 0 && i + k < n && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL INT b_cmp( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = a, *y = b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return ( x[i] < y[i] ) ? -1 : 1;
	}
	return 0;
}

LOCAL UINT w16( CONST UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL void p16( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void p32( UB *p, UINT v )
{
	p16(p, v & 0xFFFF);
	p16(p + 2, v >> 16);
}

LOCAL void pbytes( UB *p, CONST char *hex8 )
{
	INT	i;

	for ( i = 0; i < 8; i++ ) {
		INT	h = hex8[2 * i], l = hex8[2 * i + 1];

		h = ( h <= '9' ) ? h - '0' : h - 'a' + 10;
		l = ( l <= '9' ) ? l - '0' : l - 'a' + 10;
		p[i] = (UB)( h * 16 + l );
	}
}

/* A database of fields given, with no cards */
LOCAL void db_fields( MCDB *db, CONST char *CONST *names, CONST INT *types, CONST INT *maxes, INT n )
{
	INT	k;

	mc_init(db);
	for ( k = 0; k < n; k++ ) {
		MCFIELD	*f = &db->f[k];
		INT	i;

		knl_memset(f, 0, sizeof(*f));
		for ( i = 0; names[k][i] != 0 && i < MC_NAME_MAX - 1; i++ ) f->name[i] = (UB)names[k][i];
		f->type = types[k];
		f->max = maxes[k];
		f->width = 8;
		f->face = MC_FACE_MINCHO;
	}
	db->nf = n;
}

/* A card added at the end with its values (NULL empty) */
LOCAL MCCARD *db_card( MCDB *db, CONST char *CONST *v )
{
	MCCARD	*c = mc_card_new(db, db->n);
	INT	k;

	for ( k = 0; c != NULL && k < db->nf; k++ ) {
		if ( v[k] != NULL ) (void)mc_set(db, c, k, (CONST UB *)v[k], s_len((CONST UB *)v[k]));
	}
	return c;
}

LOCAL CONST UB *val( MCDB *db, INT i, INT f )
{
	return ( i >= 0 && i < db->n ) ? mc_get(db->c[i], f) : NULL;
}

/* The search with one field's condition: how many found */
LOCAL INT find1( MCDB *db, INT f, CONST char *cond, INT how )
{
	CONST UB	*c[MC_FIELD_MAX];
	INT		k, at = -1;

	for ( k = 0; k < MC_FIELD_MAX; k++ ) c[k] = NULL;
	c[f] = (CONST UB *)cond;
	return mc_search(db, c, how, &at);
}

/* The sample people: 氏名, 年齢, 住所 */
LOCAL void people( MCDB *db )
{
	static CONST char *CONST names[3] = { "氏名", "年齢", "住所" };
	static CONST INT types[3] = { MC_T_TEXT, MC_T_NUM, MC_T_TEXT };
	static CONST INT maxes[3] = { 20, 5, 40 };
	static CONST char *CONST rows[][3] = {
		{ "山田　花子", "34", "東京都新宿区1-2" },
		{ "鈴木　亨", "100", "神奈川県横浜市" },
		{ "渡邊　真理", "99", "東京都大田区9" },
		{ "コート　次郎", "-5", "SUN Building" },
		{ "小川　さくら", "", "北海道札幌市" },
		{ "タナカ　ミホ", "20.5", "東京都葛飾区3" },
	};
	INT	i;

	db_fields(db, names, types, maxes, 3);
	for ( i = 0; i < 6; i++ ) (void)db_card(db, rows[i]);
}

/* ---------------------------------------------------------------- numbers */

LOCAL void test_numbers( void )
{
	UB	t[64], b[8];
	static CONST struct {
		CONST char	*in, *kept, *bits;
	} cases[] = {
		{ "0.1", "0.1", "9a9999999999b93f" },
		{ "1200", "1200", "0000000000c09240" },
		{ "-12.5", "-12.5", "00000000000029c0" },
		{ "34", "34", "0000000000004140" },
		{ "１，２３４．５", "1234.5", NULL },
		{ "007", "7", NULL },
		{ "-0", "0", NULL },
	};
	INT	i;

	for ( i = 0; i < (INT)( sizeof(cases) / sizeof(cases[0]) ); i++ ) {
		INT	n = mc_num_norm((CONST UB *)cases[i].in, s_len((CONST UB *)cases[i].in), t, sizeof(t));

		KT_ASSERT(n > 0 && s_eq(t, cases[i].kept));
		if ( cases[i].bits != NULL ) {
			UB	want[8];

			mc_num_bits(t, b);
			pbytes(want, cases[i].bits);
			KT_ASSERT(b_cmp(b, want, 8) == 0);
			n = mc_bits_num(b, t, sizeof(t));
			KT_ASSERT(n > 0 && s_eq(t, cases[i].kept));
		}
	}
	KT_ASSERT_EQ(mc_num_norm((CONST UB *)"12a", 3, t, sizeof(t)), -1);
	KT_ASSERT_EQ(mc_num_norm((CONST UB *)"", 0, t, sizeof(t)), -1);
	KT_ASSERT(mc_num_lead((CONST UB *)"12a3", 4, t, sizeof(t)) == 2 && s_eq(t, "12"));
	KT_ASSERT(mc_num_cmp((CONST UB *)"99", (CONST UB *)"100") < 0);
	KT_ASSERT(mc_num_cmp((CONST UB *)"-5", (CONST UB *)"-12.5") > 0);
	KT_ASSERT(mc_num_cmp((CONST UB *)"20.5", (CONST UB *)"20.50") == 0);
	/* an empty number is a quiet NaN, read back as nothing */
	mc_num_bits((CONST UB *)"", b);
	KT_ASSERT(b[7] == 0x7F && b[6] == 0xF8);
	KT_ASSERT_EQ(mc_bits_num(b, t, sizeof(t)), 0);
	/* fifteen figures: 1/3 and 2/3 as the DOUBLE nearest them are shown */
	pbytes(b, "555555555555d53f");
	KT_ASSERT(mc_bits_num(b, t, sizeof(t)) > 0 && s_eq(t, "0.333333333333333"));
}

/* ---------------------------------------------------------------- the records made by hand */

LOCAL void test_records_in( void )
{
	UB	fmt[32 + 3 * 24], card[2 * ( 4 + 22 + 8 + 48 )], idx[4], vd[44], *x;
	MCRECS	rs;
	MCDB	*db = Kmalloc(sizeof(MCDB));
	INT	k, i, datalen = 4 + 22 + 8 + 48;
	ER	er;
	static CONST UH name0[2] = { 0x3B61, 0x4C3E }, name1[2] = { 0x472F, 0x4E70 }, name2[2] = { 0x3C4C, 0x3F3F };
	static CONST UH yamada[4] = { 0x3B33, 0x4544, 0x3256, 0x3B52 }, satou[4] = { 0x3A34, 0x4623, 0x306C, 0x4F3A };
	char	xml[512];

	KT_ASSERT(db != NULL);
	if ( db == NULL ) return;
	/* the format: 氏名 a string of 10, 年齢 a number, 写真 a link */
	knl_memset(fmt, 0, sizeof(fmt));
	p16(fmt, 0x2343);
	p16(fmt + 2, 0x2352);
	p16(fmt + 4, 0x2344);
	p16(fmt + 6, 0x100);
	p16(fmt + 8, 2);
	p16(fmt + 10, (UINT)datalen);
	p16(fmt + 30, 3);
	for ( k = 0; k < 2; k++ ) p16(fmt + 32 + 2 * k, name0[k]);
	p16(fmt + 32 + 16, 4);
	p16(fmt + 32 + 18, 2);
	p16(fmt + 32 + 20, 10);
	p16(fmt + 32 + 22, 8);
	for ( k = 0; k < 2; k++ ) p16(fmt + 56 + 2 * k, name1[k]);
	p16(fmt + 56 + 16, 4 + 22);
	p16(fmt + 56 + 18, 1);
	p16(fmt + 56 + 20, 3);
	p16(fmt + 56 + 22, 3);
	for ( k = 0; k < 2; k++ ) p16(fmt + 80 + 2 * k, name2[k]);
	p16(fmt + 80 + 16, 4 + 22 + 8);
	p16(fmt + 80 + 18, 3);
	p16(fmt + 80 + 20, 20);
	p16(fmt + 80 + 22, 0);

	/* the cards as stored: 佐藤一郎 (no age, a link), then 山田花子 34 */
	knl_memset(card, 0, sizeof(card));
	for ( i = 0; i < 2; i++ ) {
		UB	*c = card + datalen * i;

		p16(c, 0xA5B6);
		p16(c + 2, (UINT)datalen);
		for ( k = 0; k < 4; k++ ) p16(c + 4 + 2 * k, ( i == 0 ) ? satou[k] : yamada[k]);
		if ( i == 0 ) {
			pbytes(c + 26, "000000000000f87f");
			p16(c + 34, 5);
		} else {
			pbytes(c + 26, "0000000000004140");
		}
	}
	/* the order: the second first */
	p16(idx, 1);
	p16(idx + 2, 0);
	/* the link of vobjnumber 5: link record 0, at 10,20 - 190,45, 14 points, its colours */
	knl_memset(vd, 0, sizeof(vd));
	p16(vd, 5);
	p16(vd + 2, 0);
	p16(vd + 4, 0x0002 | 0x0004 | 0x0008);
	p16(vd + 14, 10);
	p16(vd + 16, 20);
	p16(vd + 18, 190);
	p16(vd + 20, 45);
	p16(vd + 22, 25);
	p16(vd + 24, 0x8000 | 280);
	p32(vd + 26, 0x10112233);
	p32(vd + 30, 0x10000000);
	p32(vd + 34, 0x10FFFFFF);
	p32(vd + 38, 0x10FFFFFF);
	p16(vd + 42, 0);
	/* record 0: the link record 0 is the object of 原紙箱 */
	knl_strcpy(xml, "<tad><document><microcard version=\"1\" cardsize=\"12\" names=\"false\"><field n=\"0\" face=\"gothic\"/>"
		   "</microcard><p><mclinks/><link id=\"" SYSDEF_PROG_BOX "_0.xtad\" vobjid=\"x\"/></p></document></tad>");
	x = (UB *)xml;
	knl_memset(&rs, 0, sizeof(rs));
	rs.x = x;
	rs.xn = s_len(x);
	rs.r[MC_SUB_FORMAT] = fmt;
	rs.n[MC_SUB_FORMAT] = sizeof(fmt);
	rs.r[MC_SUB_CARDS] = card;
	rs.n[MC_SUB_CARDS] = sizeof(card);
	rs.r[MC_SUB_INDEX] = idx;
	rs.n[MC_SUB_INDEX] = sizeof(idx);
	rs.r[MC_SUB_VOBJ] = vd;
	rs.n[MC_SUB_VOBJ] = sizeof(vd);
	mc_init(db);
	er = mc_read(db, &rs);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(db->nf, 3);
	KT_ASSERT_EQ(db->n, 2);
	KT_ASSERT(s_eq(db->f[0].name, "氏名") && s_eq(db->f[1].name, "年齢") && s_eq(db->f[2].name, "写真"));
	KT_ASSERT(db->f[0].type == MC_T_TEXT && db->f[1].type == MC_T_NUM && db->f[2].type == MC_T_LINK);
	KT_ASSERT(db->f[0].max == 10 && db->f[0].width == 8 && db->f[1].width == 3);
	KT_ASSERT_EQ(db->f[0].face, MC_FACE_GOTHIC);
	KT_ASSERT(db->view.csize == 12 && !db->view.names);
	/* in the order of the order record */
	KT_ASSERT(s_eq(val(db, 0, 0), "山田花子") && s_eq(val(db, 0, 1), "34") && val(db, 0, 2) == NULL);
	KT_ASSERT(s_eq(val(db, 1, 0), "佐藤一郎") && val(db, 1, 1) == NULL);
	{
		CONST UB	*el = val(db, 1, 2);
		INT		n = s_len(el);
		UB		c[16];
		TS_UUID		u, want;

		KT_ASSERT(el != NULL);
		if ( el != NULL ) {
			KT_ASSERT(ct_target(el, n, &u) && ts_str_to_uuid(SYSDEF_PROG_BOX, &want) >= E_OK
				  && b_cmp(&u, &want, sizeof(u)) == 0);
			KT_ASSERT(ct_attr_int(el, n, "vobjleft", 0) == 10 && ct_attr_int(el, n, "vobjbottom", 0) == 45);
			KT_ASSERT_EQ(ct_attr_int(el, n, "chsz", 0), 14);
			KT_ASSERT(ct_attr(el, n, "frcol", c, sizeof(c)) == 7 && s_eq(c, "#112233"));
			KT_ASSERT(!ct_attr_bool(el, n, "roledisp", TRUE) && ct_attr_bool(el, n, "namedisp", FALSE));
		}
	}
	/* not a format record: E_PAR and a database of one empty card */
	p16(fmt + 6, 0x101);
	KT_ASSERT_ER(mc_read(db, &rs), E_PAR);
	KT_ASSERT(db->nf == 0 && db->n == 1);
	mc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- the records written */

LOCAL void test_records_out( void )
{
	static CONST char *CONST names[4] = { "氏名", "年齢", "写真", "あいうえおかきく" };
	static CONST INT types[4] = { MC_T_TEXT, MC_T_NUM, MC_T_LINK, MC_T_TEXT };
	static CONST INT maxes[4] = { 6, 3, 20, 4 };
	MCDB	*db = Kmalloc(sizeof(MCDB)), *rd = Kmalloc(sizeof(MCDB));
	UB	*bin[MC_SUBS], *x = Kmalloc(BUF_MAX);
	INT	len[MC_SUBS], sub, datalen = 4 + 14 + 8 + 48 + 10, k;
	CTBUF	b;
	MCRECS	rs;
	char	el[300];

	KT_ASSERT(db != NULL && rd != NULL && x != NULL);
	if ( db == NULL || rd == NULL || x == NULL ) return;
	db_fields(db, names, types, maxes, 4);
	{
		static CONST char *CONST r1[4] = { "山田花子", "34", NULL, "AB" };
		static CONST char *CONST r2[4] = { "佐藤", NULL, NULL, NULL };

		(void)db_card(db, r1);
		(void)db_card(db, r2);
	}
	knl_strcpy(el, "<link id=\"" SYSDEF_PROG_BOX "_0.xtad\" vobjid=\"v\" vobjleft=\"0\" vobjtop=\"0\" vobjright=\"150\" "
		   "vobjbottom=\"25\" chsz=\"14\" frcol=\"#ff0000\" namedisp=\"true\" pictdisp=\"false\"/>");
	KT_ASSERT_ER(mc_set(db, db->c[1], 2, (CONST UB *)el, s_len((CONST UB *)el)), E_OK);
	for ( sub = 0; sub < MC_SUBS; sub++ ) {
		bin[sub] = Kmalloc(BUF_MAX);
		len[sub] = 0;
		if ( sub > 0 && bin[sub] != NULL ) {
			ct_buf(&b, bin[sub], BUF_MAX);
			len[sub] = mc_write_bin(&b, db, sub, FALSE);
			KT_ASSERT(!b.fail);
		}
	}
	/* the format: ＣＲＤ, 0x100, 2 cards, the card's bytes, 4 items laid one after another */
	KT_ASSERT_EQ(len[MC_SUB_FORMAT], 32 + 4 * 24);
	KT_ASSERT(w16(bin[1]) == 0x2343 && w16(bin[1] + 2) == 0x2352 && w16(bin[1] + 4) == 0x2344);
	KT_ASSERT_EQ(w16(bin[1] + 6), 0x100);
	KT_ASSERT_EQ(w16(bin[1] + 8), 2);
	KT_ASSERT_EQ(w16(bin[1] + 10), datalen);
	KT_ASSERT_EQ(w16(bin[1] + 30), 4);
	KT_ASSERT(w16(bin[1] + 32) == 0x3B61 && w16(bin[1] + 34) == 0x4C3E && w16(bin[1] + 36) == 0);
	KT_ASSERT(w16(bin[1] + 48) == 4 && w16(bin[1] + 50) == 2 && w16(bin[1] + 52) == 6 && w16(bin[1] + 54) == 8);
	KT_ASSERT(w16(bin[1] + 72) == 18 && w16(bin[1] + 74) == 1);
	KT_ASSERT(w16(bin[1] + 96) == 26 && w16(bin[1] + 98) == 3);
	KT_ASSERT(w16(bin[1] + 120) == 74 && w16(bin[1] + 122) == 2);
	/* a name of eight letters, no TNULL after it */
	KT_ASSERT(w16(bin[1] + 104) != 0 && w16(bin[1] + 118) != 0);
	/* the cards: the mark, the length, the string with its TNULL, the DOUBLE, the link's number */
	KT_ASSERT_EQ(len[MC_SUB_CARDS], 2 * datalen);
	KT_ASSERT(w16(bin[2]) == 0xA5B6 && w16(bin[2] + 2) == (UINT)datalen);
	KT_ASSERT(w16(bin[2] + 4) == 0x3B33 && w16(bin[2] + 10) == 0x3B52 && w16(bin[2] + 12) == 0);
	{
		UB	want[8];

		pbytes(want, "0000000000004140");
		KT_ASSERT(b_cmp(bin[2] + 18, want, 8) == 0);
		pbytes(want, "000000000000f87f");
		KT_ASSERT(b_cmp(bin[2] + datalen + 18, want, 8) == 0);
	}
	KT_ASSERT_EQ(w16(bin[2] + 26), 0);
	KT_ASSERT_EQ(w16(bin[2] + datalen + 26), 5);
	KT_ASSERT(w16(bin[2] + 74) == 0x2341 && w16(bin[2] + 76) == 0x2342 && w16(bin[2] + 78) == 0);
	/* in their order: no order record */
	KT_ASSERT_EQ(len[MC_SUB_INDEX], 0);
	/* the link: number 5, link record 0, its box, 14 points in 1/20, red as a COLOR, no data */
	KT_ASSERT_EQ(len[MC_SUB_VOBJ], 44);
	KT_ASSERT(w16(bin[4]) == 5 && w16(bin[4] + 2) == 0);
	KT_ASSERT(( w16(bin[4] + 4) & 0x0040 ) != 0 && ( w16(bin[4] + 4) & 0x0001 ) == 0);
	KT_ASSERT(w16(bin[4] + 18) == 150 && w16(bin[4] + 20) == 25 && w16(bin[4] + 24) == ( 0x8000 | 280 ));
	KT_ASSERT(w16(bin[4] + 26) == 0x0000 && w16(bin[4] + 28) == 0x10FF);
	KT_ASSERT_EQ(w16(bin[4] + 42), 0);
	/* record 0: the link record's object after <mclinks/> */
	ct_buf(&b, x, BUF_MAX);
	mc_write_x(&b, db, (CONST UB *)"試験", FALSE);
	KT_ASSERT(!b.fail && holds(b.p, b.n, "<mclinks/><link id=\"" SYSDEF_PROG_BOX));
	KT_ASSERT(holds(b.p, b.n, "<microcard version=\"1\""));
	/* read back, the same */
	knl_memset(&rs, 0, sizeof(rs));
	rs.x = b.p;
	rs.xn = b.n;
	for ( sub = 1; sub < MC_SUBS; sub++ ) {
		rs.r[sub] = ( len[sub] > 0 ) ? bin[sub] : NULL;
		rs.n[sub] = len[sub];
	}
	mc_init(rd);
	KT_ASSERT_ER(mc_read(rd, &rs), E_OK);
	KT_ASSERT(rd->nf == 4 && rd->n == 2);
	for ( k = 0; k < 4; k++ ) {
		KT_ASSERT(s_eq(rd->f[k].name, (CONST char *)db->f[k].name));
		KT_ASSERT(rd->f[k].type == db->f[k].type && rd->f[k].max == db->f[k].max);
	}
	KT_ASSERT(s_eq(val(rd, 0, 0), "山田花子") && s_eq(val(rd, 0, 1), "34") && s_eq(val(rd, 0, 3), "AB"));
	KT_ASSERT(s_eq(val(rd, 1, 0), "佐藤") && val(rd, 1, 1) == NULL && val(rd, 1, 3) == NULL);
	KT_ASSERT(val(rd, 1, 2) != NULL && val(rd, 0, 2) == NULL);
	if ( val(rd, 1, 2) != NULL ) {
		UB	c[16];

		KT_ASSERT(ct_attr(val(rd, 1, 2), s_len(val(rd, 1, 2)), "frcol", c, sizeof(c)) == 7 && s_eq(c, "#ff0000"));
		KT_ASSERT(!ct_attr_bool(val(rd, 1, 2), s_len(val(rd, 1, 2)), "pictdisp", TRUE));
	}
	/* only the cards found: one card, its link record numbered 0 still */
	(void)find1(db, 0, "佐藤", MC_S_NEW);
	ct_buf(&b, bin[1], BUF_MAX);
	(void)mc_write_bin(&b, db, MC_SUB_FORMAT, TRUE);
	KT_ASSERT_EQ(w16(bin[1] + 8), 1);
	mc_clear(rd);
	mc_clear(db);
	for ( sub = 0; sub < MC_SUBS; sub++ ) {
		if ( bin[sub] != NULL ) Kfree(bin[sub]);
	}
	Kfree(x);
	Kfree(rd);
	Kfree(db);
}

/* ---------------------------------------------------------------- the format, typing */

LOCAL void test_format( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));
	MCFIELD	f[4];
	INT	from[4], at = -1;
	BOOL	lose[4];

	if ( db == NULL ) return;
	people(db);
	/* names: of full width, eight letters, each once */
	f[0] = db->f[0];
	f[1] = db->f[1];
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_OK);
	knl_strcpy((char *)f[1].name, "氏名");
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_OBJ);
	KT_ASSERT_EQ(at, 1);
	knl_strcpy((char *)f[1].name, "age");
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_PAR);
	knl_strcpy((char *)f[1].name, "一二三四五六七八九");
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_PAR);
	knl_strcpy((char *)f[1].name, "一二三四五六七八");
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_OK);
	f[1].max = 21;
	f[1].type = MC_T_NUM;
	KT_ASSERT_ER(mc_fields_check(f, 2, &at), E_LIMIT);

	/* 年齢 to text, 氏名 cut to 2, 住所 to a number (lost), a new field at the end */
	f[0] = db->f[1];
	f[0].type = MC_T_TEXT;
	f[0].max = 10;
	from[0] = 1;
	f[1] = db->f[0];
	f[1].max = 2;
	from[1] = 0;
	f[2] = db->f[2];
	f[2].type = MC_T_NUM;
	f[2].max = 10;
	from[2] = 2;
	f[3] = db->f[0];
	knl_strcpy((char *)f[3].name, "備考");
	from[3] = -1;
	KT_ASSERT_EQ(mc_format_loss(db, f, from, 4, lose), 2);
	KT_ASSERT(!lose[0] && lose[1] && lose[2] && !lose[3]);
	KT_ASSERT_ER(mc_format_set(db, f, from, 4), E_OK);
	KT_ASSERT(db->nf == 4 && s_eq(db->f[0].name, "年齢"));
	KT_ASSERT(s_eq(val(db, 0, 0), "34") && s_eq(val(db, 0, 1), "山田") && val(db, 0, 2) == NULL);
	KT_ASSERT(s_eq(val(db, 3, 0), "-5") && val(db, 4, 0) == NULL);
	KT_ASSERT(val(db, 0, 3) == NULL);
	mc_clear(db);
	Kfree(db);
}

LOCAL void test_input( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));
	MCCARD	*c;

	if ( db == NULL ) return;
	people(db);
	c = db->c[0];
	/* a text cut to its size, tabs and new lines made spaces */
	KT_ASSERT_ER(mc_set(db, c, 0, (CONST UB *)"あ\tい\nう", 11), E_OK);
	KT_ASSERT(s_eq(mc_get(c, 0), "あ い う"));
	db->f[0].max = 3;
	KT_ASSERT_ER(mc_set(db, c, 0, (CONST UB *)"一二三四五", 15), E_OK);
	KT_ASSERT(s_eq(mc_get(c, 0), "一二三"));
	/* a number: its digits as kept, or not one */
	KT_ASSERT_ER(mc_set(db, c, 1, (CONST UB *)"３５", 6), E_OK);
	KT_ASSERT(s_eq(mc_get(c, 1), "35"));
	KT_ASSERT_ER(mc_set(db, c, 1, (CONST UB *)"x", 1), E_PAR);
	KT_ASSERT(s_eq(mc_get(c, 1), "35"));
	KT_ASSERT_ER(mc_set(db, c, 1, (CONST UB *)"123456789012345678901", 21), E_LIMIT);
	KT_ASSERT_ER(mc_set(db, c, 1, NULL, 0), E_OK);
	KT_ASSERT(mc_get(c, 1) == NULL);
	/* cards added after, duplicated, made empty, deleted; the last stays, empty */
	KT_ASSERT(mc_card_new(db, 1) != NULL && db->n == 7 && mc_card_empty(db, 1));
	KT_ASSERT(mc_card_dup(db, 2, 3) != NULL && s_eq(val(db, 3, 0), (CONST char *)val(db, 2, 0)));
	mc_card_blank(db, 3);
	KT_ASSERT(mc_card_empty(db, 3));
	while ( db->n > 1 ) mc_card_del(db, 0);
	(void)mc_set(db, db->c[0], 0, (CONST UB *)"残", 3);
	mc_card_del(db, 0);
	KT_ASSERT(db->n == 1 && mc_card_empty(db, 0));
	mc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- searching */

LOCAL void test_search( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));
	static CONST struct {
		INT		f;
		CONST char	*cond;
		INT		found;
	} cases[] = {
		{ 0, "=鈴木　亨", 1 },	{ 0, "鈴木　亨", 1 },		{ 0, "鈴木", 0 },
		{ 1, "34", 1 },			{ 1, "≧34", 3 },		{ 1, ">=34", 3 },
		{ 1, "<20.5", 1 },		{ 1, "≦20.5", 2 },		{ 1, "≠34", 5 },
		{ 1, "<>34", 5 },		{ 1, "=", 1 },			{ 1, "≠", 5 },
		{ 2, "*東京*", 3 },		{ 2, "東京*", 3 },		{ 2, "*市", 2 },
		{ 2, "≠*東京*", 3 },		{ 2, "???SUN*", 0 },		{ 2, "SUN *", 1 },
		{ 0, "??　*", 4 },		{ 2, "==>", 0 },		{ 2, "<北", 5 },
		/* regular expressions: anywhere, ^ $ ( | ) [ ] + ? . \ */
		{ 2, "/東京", 3 },		{ 2, "/^東京都(新宿|大田)", 2 },	{ 2, "/[0-9]$", 3 },
		{ 0, "/^..　", 4 },		{ 2, "/S.N B", 1 },		{ 2, "/区[0-9]+$", 2 },
		{ 1, "/^-", 1 },		{ 0, "/\\(", 0 },		{ 0, "/[[:日本基本:]]", 6 },
		{ 0, "/[^[:日本基本:]]", 0 },	{ 2, "/[[:各国:]]", 0 },	{ 0, "/&T013B33;", 1 },
		/* ≒: the variants, the kana, the case, the width the same; * ? | */
		{ 0, "≒渡辺", 1 },		{ 0, "≒たなか", 1 },		{ 0, "≒こーと", 1 },
		{ 2, "≒sun", 1 },		{ 2, "≒ＳＵＮ", 1 },		{ 0, "≒鈴木|小川", 2 },
		{ 2, "≒東京都*区", 3 },		{ 0, "≒?田", 1 },		{ 2, "≒/^東京都(葛飾|大田)", 2 },
		{ 2, "≒葛飾", 1 },		{ 0, "≒山田　花子", 1 },	{ 0, "=≒", 0 },
	};
	INT	i;

	if ( db == NULL ) return;
	people(db);
	for ( i = 0; i < (INT)( sizeof(cases) / sizeof(cases[0]) ); i++ ) {
		INT	n = find1(db, cases[i].f, cases[i].cond, MC_S_NEW);

		if ( n != cases[i].found ) tm_printf((UB *)"  field %d \"%s\": %d found, %d wanted\n", cases[i].f + 1, cases[i].cond, n, cases[i].found);
		KT_ASSERT_EQ(n, cases[i].found);
	}
	/* a pattern that is not one */
	KT_ASSERT(find1(db, 0, "/(", MC_S_NEW) < 0);
	KT_ASSERT(find1(db, 0, "/*a", MC_S_NEW) < 0);
	KT_ASSERT(find1(db, 1, "≧abc", MC_S_NEW) < 0);
	/* narrowed: 東京 and then 34 and over */
	KT_ASSERT_EQ(find1(db, 2, "東京*", MC_S_NEW), 3);
	KT_ASSERT(db->searching && mc_count(db) == 3);
	KT_ASSERT_EQ(find1(db, 1, "≧30", MC_S_NARROW), 2);
	KT_ASSERT(mc_index(db, 0) == 0 && mc_index(db, 1) == 2 && mc_pos(db, 2) == 1 && mc_pos(db, 1) == -1);
	/* a card added in the search state is one of those found, after the one shown */
	KT_ASSERT(mc_card_new(db, 1) != NULL && mc_count(db) == 3 && mc_index(db, 1) == 1);
	mc_card_del(db, 1);
	mc_search_off(db);
	KT_ASSERT(!db->searching && mc_count(db) == 6);
	/* the marks searched: new among those found, added */
	(void)mc_marks(db, MC_M_NONE, 0);
	db->c[1]->mark = TRUE;
	db->c[4]->mark = TRUE;
	KT_ASSERT_EQ(find1(db, 2, "*東京*", MC_S_NEW), 3);
	KT_ASSERT_EQ(mc_mark_search(db, FALSE), 0);
	mc_search_off(db);
	KT_ASSERT_EQ(find1(db, 2, "*東京*", MC_S_NEW), 3);
	KT_ASSERT_EQ(mc_mark_search(db, TRUE), 5);
	mc_search_off(db);
	KT_ASSERT_EQ(mc_mark_search(db, FALSE), 2);
	mc_clear(db);
	Kfree(db);
}

LOCAL void test_marks( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));

	if ( db == NULL ) return;
	people(db);
	KT_ASSERT_EQ(mc_marks(db, MC_M_ALL, 0), 6);
	KT_ASSERT_EQ(mc_marks(db, MC_M_BEFORE_OFF, 2), 3);
	KT_ASSERT(!db->c[2]->mark && db->c[3]->mark);
	KT_ASSERT_EQ(mc_marks(db, MC_M_FLIP_ALL, 0), 3);
	KT_ASSERT(db->c[0]->mark && !db->c[5]->mark);
	KT_ASSERT_EQ(mc_marks(db, MC_M_AFTER_ON, 4), 5);
	KT_ASSERT_EQ(mc_marks(db, MC_M_AFTER_OFF, 1), 1);
	KT_ASSERT_EQ(mc_marks(db, MC_M_BEFORE_ON, 3), 4);
	KT_ASSERT_EQ(mc_marks(db, MC_M_NONE, 0), 0);
	db->c[1]->mark = db->c[4]->mark = TRUE;
	KT_ASSERT_EQ(mc_mark_next(db, 0, 1), 1);
	KT_ASSERT_EQ(mc_mark_next(db, 1, 1), 4);
	KT_ASSERT_EQ(mc_mark_next(db, 4, 1), -1);
	KT_ASSERT_EQ(mc_mark_next(db, 4, -1), 1);
	/* in the search state, only the cards found */
	(void)find1(db, 2, "*東京*", MC_S_NEW);
	KT_ASSERT_EQ(mc_marks(db, MC_M_ALL, 0), 3);
	mc_search_off(db);
	KT_ASSERT(db->c[0]->mark && db->c[1]->mark && !db->c[3]->mark);
	mc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- order */

LOCAL void test_sort( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));
	static CONST char *CONST names[2] = { "ランク", "取引額" };
	static CONST INT types[2] = { MC_T_TEXT, MC_T_NUM };
	static CONST INT maxes[2] = { 4, 10 };
	static CONST char *CONST rows[][2] = {
		{ "B", "300" }, { "A", "50" }, { "B", "20" }, { "A", "700" }, { "C", "" }, { "A", "50" },
	};
	INT	i;

	if ( db == NULL ) return;
	/* the order of letters: JIS, a long sound as its vowel, 99 before 100 only as numbers */
	KT_ASSERT(mc_text_cmp((CONST UB *)"あ", 3, (CONST UB *)"い", 3) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"亜", 3, (CONST UB *)"唖", 3) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"A", 1, (CONST UB *)"a", 1) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"9", 1, (CONST UB *)"あ", 3) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"あ", 3, (CONST UB *)"ア", 3) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"ア", 3, (CONST UB *)"亜", 3) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"99", 2, (CONST UB *)"100", 3) > 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"コート", 9, (CONST UB *)"コオトア", 12) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"コート", 9, (CONST UB *)"コオト", 9) != 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"セーター", 12, (CONST UB *)"セエタ", 9) > 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"セーター", 12, (CONST UB *)"セエタイ", 12) < 0);
	KT_ASSERT(mc_text_cmp((CONST UB *)"Ａ", 3, (CONST UB *)"A", 1) != 0);

	db_fields(db, names, types, maxes, 2);
	for ( i = 0; i < 6; i++ ) (void)db_card(db, rows[i]);
	/* the finer key first, then the rougher: each rank in the order of the sums */
	KT_ASSERT_ER(mc_sort(db, 1, FALSE), E_OK);
	KT_ASSERT(val(db, 0, 1) == NULL && s_eq(val(db, 1, 1), "20") && s_eq(val(db, 5, 1), "700"));
	KT_ASSERT_ER(mc_sort(db, 0, FALSE), E_OK);
	{
		static CONST char *CONST want[6][2] = {
			{ "A", "50" }, { "A", "50" }, { "A", "700" }, { "B", "20" }, { "B", "300" }, { "C", "" }
		};

		for ( i = 0; i < 6; i++ ) {
			KT_ASSERT(s_eq(val(db, i, 0), want[i][0]) && s_eq(val(db, i, 1), want[i][1]));
		}
	}
	KT_ASSERT_ER(mc_sort(db, 1, TRUE), E_OK);
	KT_ASSERT(s_eq(val(db, 0, 1), "700") && val(db, 5, 1) == NULL);
	/* in the search state only the cards found move, into the places they had */
	KT_ASSERT_EQ(find1(db, 0, "B", MC_S_NEW), 2);
	KT_ASSERT(s_eq(val(db, 1, 0), "B") && s_eq(val(db, 1, 1), "300") && s_eq(val(db, 4, 1), "20"));
	KT_ASSERT_ER(mc_sort(db, 1, FALSE), E_OK);
	KT_ASSERT(s_eq(val(db, 1, 1), "20") && s_eq(val(db, 4, 1), "300"));
	KT_ASSERT(s_eq(val(db, 0, 1), "700") && s_eq(val(db, 2, 0), "A"));
	mc_search_off(db);
	/* duplicates: the cards with a value another has */
	KT_ASSERT_EQ(mc_dups(db, 1), 2);
	KT_ASSERT(db->searching && s_eq(val(db, mc_index(db, 0), 1), "50"));
	KT_ASSERT_EQ(mc_dups(db, 0), 2);	/* the two found are both A */
	mc_search_off(db);
	KT_ASSERT_EQ(mc_dups(db, 0), 5);
	/* in the search state, among those found only */
	KT_ASSERT_EQ(find1(db, 1, "≧50", MC_S_NEW), 4);
	KT_ASSERT_EQ(mc_dups(db, 0), 3);
	mc_clear(db);
	Kfree(db);
}

/* ---------------------------------------------------------------- out and in */

LOCAL void test_out_in( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB)), *rd = Kmalloc(sizeof(MCDB));
	UB	*t = Kmalloc(BUF_MAX);
	CTBUF	b;
	INT	n;

	if ( db == NULL || rd == NULL || t == NULL ) return;
	people(db);
	(void)mc_set(db, db->c[0], 2, (CONST UB *)"新宿:1\"2\"＼", s_len((CONST UB *)"新宿:1\"2\"＼"));
	/* @形式: the definition, $, the cards; all in full width, : and ＼ escaped */
	ct_buf(&b, t, BUF_MAX);
	mc_out_at(&b, db, FALSE);
	KT_ASSERT(!b.fail);
	KT_ASSERT(holds(b.p, b.n, "＠氏名：文字：２０：８\n") && holds(b.p, b.n, "＠年齢：数値：５：８\n＠住所"));
	KT_ASSERT(holds(b.p, b.n, "＄\n＠氏名：山田　花子\n＠年齢：３４\n"));
	KT_ASSERT(holds(b.p, b.n, "＠住所：新宿＼：１＂２＂＼＼\n＄"));
	mc_init(rd);
	(void)mc_card_new(rd, 0);
	n = mc_in_text(rd, b.p, b.n, 0, MC_IN_AUTO);
	KT_ASSERT_EQ(n, 6);
	KT_ASSERT(rd->nf == 3 && rd->n == 6 && rd->f[1].type == MC_T_NUM && rd->f[0].max == 20);
	KT_ASSERT(s_eq(val(rd, 0, 2), "新宿：１＂２＂＼") && s_eq(val(rd, 0, 1), "34") && s_eq(val(rd, 3, 1), "-5"));
	mc_clear(rd);

	/* CSV: names first, texts in quotes, " and ＼ with ＼ before them */
	ct_buf(&b, t, BUF_MAX);
	mc_out_rows(&b, db, -1, -1, FALSE, MC_OUT_HEAD | MC_OUT_CSV | MC_OUT_QUOTE);
	KT_ASSERT(holds(b.p, b.n, "\"氏名\",\"年齢\",\"住所\"\n\"山田　花子\",34,\"新宿:1＼\"2＼\"＼＼\"\n"));
	KT_ASSERT(holds(b.p, b.n, "\"小川　さくら\",,\"北海道札幌市\"\n"));
	mc_init(rd);
	n = mc_in_text(rd, b.p, b.n, 0, MC_IN_AUTO);
	KT_ASSERT(n == 6 && rd->nf == 3 && rd->f[1].type == MC_T_NUM && rd->f[2].type == MC_T_TEXT);
	KT_ASSERT(s_eq(val(rd, 0, 2), "新宿:1\"2\"＼") && val(rd, 4, 1) == NULL && s_eq(val(rd, 5, 1), "20.5"));
	/* read again into the same: by the names, after the second card */
	n = mc_in_text(rd, (CONST UB *)"住所\t氏名\t趣味\n大阪\t新人\t囲碁\n", s_len((CONST UB *)"住所\t氏名\t趣味\n大阪\t新人\t囲碁\n"), 2, MC_IN_AUTO);
	KT_ASSERT(n == 1 && rd->n == 7 && s_eq(val(rd, 2, 0), "新人") && s_eq(val(rd, 2, 2), "大阪") && rd->nf == 3);
	mc_clear(rd);

	/* the tray's lines: tabs, no names, no quotes; the rows read into the fields from the first */
	ct_buf(&b, t, BUF_MAX);
	mc_out_rows(&b, db, 1, 2, FALSE, 0);
	KT_ASSERT(b.n > 0 && holds(b.p, b.n, "鈴木　亨\t100\t神奈川県横浜市\n渡邊　真理\t99\t") && !holds(b.p, b.n, "山田"));
	n = mc_in_rows(db, b.p, b.n, db->n);
	KT_ASSERT(n == 2 && db->n == 8 && s_eq(val(db, 7, 1), "99"));

	/* a sheet: the names in row 1, a card a row */
	{
		CXSHEET	*s = Kmalloc(sizeof(CXSHEET));

		if ( s != NULL ) {
			cx_init(s);
			mc_out_sheet(s, db, FALSE);
			KT_ASSERT(cx_get(s, 1, 1) != NULL && s_eq(cx_get(s, 1, 1)->text, "氏名"));
			KT_ASSERT(cx_get(s, 2, 3) != NULL && s_eq(cx_get(s, 2, 3)->text, "100"));
			mc_init(rd);
			KT_ASSERT_EQ(mc_in_sheet(rd, s, 0), 8);
			KT_ASSERT(rd->nf == 3 && rd->f[1].type == MC_T_NUM && s_eq(val(rd, 1, 0), "鈴木　亨"));
			mc_clear(rd);
			cx_free_all(s);
			Kfree(s);
		}
	}
	/* a database read into another: the fields of the same names */
	mc_init(rd);
	{
		static CONST char *CONST names[2] = { "住所", "年齢" };
		static CONST INT types[2] = { MC_T_TEXT, MC_T_TEXT };
		static CONST INT maxes[2] = { 4, 10 };

		db_fields(rd, names, types, maxes, 2);
	}
	KT_ASSERT_EQ(mc_in_db(rd, db, 0), 8);
	KT_ASSERT(s_eq(val(rd, 1, 0), "神奈川県") && s_eq(val(rd, 1, 1), "100"));
	/* a text of the program's own, as a record */
	ct_buf(&b, t, BUF_MAX);
	mc_text_doc(&b, (CONST UB *)"a<b\tc\nd", 7, (CONST UB *)"x");
	KT_ASSERT(holds(b.p, b.n, "<p>a&lt;b<tab/>c</p>\n<p>d</p>"));
	{
		UB	o[64];

		n = mc_doc_text(b.p, b.n, o, sizeof(o));
		KT_ASSERT(n == 7 && o[1] == '<' && o[3] == '\t' && o[5] == '\n');
	}
	mc_clear(rd);
	mc_clear(db);
	Kfree(t);
	Kfree(rd);
	Kfree(db);
}

/* ---------------------------------------------------------------- merging */

typedef struct {
	CTBUF	*b;
	INT	kind, pages;
} TSINK;

LOCAL void ts_start( void *arg, INT kind, INT w, INT h )
{
	TSINK	*s = arg;

	s->kind = kind;
	s->pages = 0;
	(void)w;
	(void)h;
}

LOCAL void ts_page( void *arg, CONST UB *body, INT n )
{
	TSINK	*s = arg;

	ct_put(s->b, "[page]");
	ct_putn(s->b, body, n);
	s->pages++;
}

LOCAL void ts_end( void *arg )
{
	ct_put(((TSINK *)arg)->b, "[end]");
}

LOCAL void test_merge( void )
{
	MCDB	*db = Kmalloc(sizeof(MCDB));
	UB	*t = Kmalloc(BUF_MAX), *work = Kmalloc(BUF_MAX);
	CTBUF	b;
	MCMINFO	mi;
	MCPRINT	p;
	TSINK	ts;
	MCSINK	sink;
	INT	idx[600], n, i;
	static CONST char doc[] = "<tad><document><p>%(氏名)様 %1C %2I %?9C %9C %CT %Ct %CN %Cn %N %P/%T %y-%m-%d %Y %M/%D %% [%-2C]"
				  "</p></document></tad>";
	static CONST char fig[] = "<tad><figure><document background=\"true\"><docView viewleft=\"0\" viewtop=\"0\"/>%N</document>"
				  "<document><docView viewleft=\"10\" viewtop=\"20\" viewright=\"110\" viewbottom=\"40\"/>%(氏名)</document>"
				  "</figure></tad>";

	if ( db == NULL || t == NULL || work == NULL ) return;
	people(db);
	knl_memset(&mi, 0, sizeof(mi));
	knl_strcpy((char *)mi.name, "名簿");
	mi.y = 1995;
	mi.m = 8;
	mi.d = 9;
	sink.arg = &ts;
	sink.start = ts_start;
	sink.page = ts_page;
	sink.end = ts_end;

	/* a text: a card a page, every % letter */
	(void)find1(db, 2, "*東京*", MC_S_NEW);
	n = mc_targets(db, FALSE, 0, idx, 600);
	KT_ASSERT_EQ(n, 3);
	mc_print_default(&p, (CONST UB *)"案内");
	mi.pages = mc_pages(db, &p, FALSE, FALSE, n);
	KT_ASSERT_EQ(mi.pages, 3);
	ts.b = &b;
	ct_buf(&b, t, BUF_MAX);
	KT_ASSERT_ER(mc_merge(db, &p, (CONST UB *)doc, sizeof(doc) - 1, idx, n, &mi, &sink, work, BUF_MAX), E_OK);
	KT_ASSERT_EQ(ts.pages, 3);
	KT_ASSERT(holds(b.p, b.n, "[page]<p>山田　花子様 山田　花子 年齢  %9C 6 3 1 1 名簿 1/3 1995-08-09 95 8/9 % [   34]</p>"));
	KT_ASSERT(holds(b.p, b.n, "[page]<p>渡邊　真理様 渡邊　真理 年齢  %9C 6 3 3 2 名簿 2/3"));
	KT_ASSERT(holds(b.p, b.n, "タナカ　ミホ様 タナカ　ミホ 年齢  %9C 6 3 6 3 名簿 3/3"));
	/* the one card shown only */
	n = mc_targets(db, TRUE, 1, idx, 600);
	KT_ASSERT(n == 1 && idx[0] == 2);
	mc_search_off(db);

	/* a figure: two across, two down; the background once a page, the card's elements moved */
	mc_print_default(&p, (CONST UB *)"ラベル");
	p.across = 2;
	p.down = 2;
	p.gapx = 500;		/* 50 mm: 188 dots */
	p.gapy = 300;
	p.margin[0] = p.margin[2] = 0;
	n = mc_targets(db, FALSE, 0, idx, 600);
	KT_ASSERT_EQ(mc_pages(db, &p, TRUE, FALSE, n), 2);
	ct_buf(&b, t, BUF_MAX);
	KT_ASSERT_ER(mc_merge(db, &p, (CONST UB *)fig, sizeof(fig) - 1, idx, n, &mi, &sink, work, BUF_MAX), E_OK);
	KT_ASSERT_EQ(ts.pages, 2);
	KT_ASSERT(holds(b.p, b.n, "viewleft=\"10\" viewtop=\"20\" viewright=\"110\" viewbottom=\"40\"/>山田　花子"));
	KT_ASSERT(holds(b.p, b.n, "viewleft=\"198\" viewtop=\"20\" viewright=\"298\" viewbottom=\"40\"/>鈴木　亨"));
	KT_ASSERT(holds(b.p, b.n, "viewleft=\"10\" viewtop=\"133\" viewright=\"110\" viewbottom=\"153\"/>渡邊　真理"));
	KT_ASSERT(holds(b.p, b.n, "[page]<document background=\"true\"><docView viewleft=\"0\" viewtop=\"0\"/>名簿"));
	/* the list as it is shown: the names, then a line a card */
	mc_print_default(&p, (CONST UB *)"");
	ct_buf(&b, t, BUF_MAX);
	KT_ASSERT_ER(mc_merge(db, &p, NULL, 0, idx, n, &mi, &sink, work, BUF_MAX), E_OK);
	KT_ASSERT_EQ(ts.pages, 1);
	KT_ASSERT(holds(b.p, b.n, "<p><tab/>氏名<tab/>年齢<tab/>住所</p>") && holds(b.p, b.n, "<p>2<tab/>鈴木　亨<tab/>100<tab/>神奈川県横浜市</p>"));
	/* more than 500 pages are not made */
	for ( i = 0; i < 501; i++ ) idx[i] = 0;
	mc_print_default(&p, (CONST UB *)"案内");
	KT_ASSERT_ER(mc_merge(db, &p, (CONST UB *)doc, sizeof(doc) - 1, idx, 501, &mi, &sink, work, BUF_MAX), E_LIMIT);
	/* the paper */
	{
		INT	w, h, m[4];

		p.paper = MC_PAPER_A4;
		p.turn = TRUE;
		mc_paper(&p, &w, &h, m);
		KT_ASSERT(w == 1122 && h == 793 && m[0] == 75);
	}
	mc_clear(db);
	Kfree(work);
	Kfree(t);
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

LOCAL void test_program( void )
{
	TS_UUID	prog, pu, obj;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	INT	wid, lid;
	ER	er = E_OK;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_MCARD, &prog), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &obj), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &obj;
	c.argsz = sizeof(obj);
	if ( ob_cre_obj(&c, &pu) < E_OK ) KT_SKIP("no program object of マイクロカード");
	pid = knl_prc_of_uuid(&pu);
	wid = win_named("住所録", 30000);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto stop;
	(void)wm_focus(wid);
	tk_dly_tsk(500);
	key_to(wid, 0x4E, 0);				/* PgDn: the next card */
	key_to(wid, 0x0F, HID_MOD_LCTRL);		/* Ctrl+L: the list */
	lid = win_named("住所録（一覧）", 10000);
	KT_ASSERT(lid > 0);
	(void)kt_shot("/boot/MCARD0.PPM");
	key_to(wid, 0x08, HID_MOD_LCTRL);		/* Ctrl+E: nothing changed, closed */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 15000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named("住所録", 300), 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
}

EXPORT void ktest_mcard( void )
{
	tm_printf((UB *)"\n[mcard] マイクロカード (design 11.18)\n");
	KT_RUN(test_numbers);
	KT_RUN(test_records_in);
	KT_RUN(test_records_out);
	KT_RUN(test_format);
	KT_RUN(test_input);
	KT_RUN(test_search);
	KT_RUN(test_marks);
	KT_RUN(test_sort);
	KT_RUN(test_out_in);
	KT_RUN(test_merge);
	KT_RUN_SCREEN(test_program);
}
