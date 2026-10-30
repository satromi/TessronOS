/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_kana.c
 *	かな入力練習 (design 11.14): the layouts, the questions, the judging
 *	and the scores, and what the object keeps
 *
 *	The keyboards are read as the program's own drawing of them places
 *	the keys: the Japanese 109 keyboard and the split keyboard whose
 *	keys are turned, at the places to a thousandth of a key. The kana
 *	layouts the template carries, the system's and a translation are
 *	read. Every resource of the template is written back as
 *	JSON.stringify writes it, to the same length and hash, and a layout
 *	written into an object as a resource is read back the same. The
 *	questions are read from the template's record 0 and from paragraphs
 *	with entities and elements in them. Keys are judged: plain and
 *	shifted kana, a kana with a mark typed as two keys, a chord waiting
 *	for its last key, keys that are wrong, and romaji spelt in its
 *	several ways; the scores of a practice whose times are known are
 *	worked out. A window that asks for keys let go hears them, one that
 *	does not hears only the presses. The program is started on a copy
 *	of the template with one question, typed through to its result.
 *	Last the program is started with
 *	"selftest": it reads the template, types every question with each
 *	of its kana layouts and adds a keyboard to a copy, and answers 0.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/kbmap.h>
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/kanatype/kp_eng.h"

#define TEMPLATE	"019c403c-bb89-7133-acc4-ddd109fb8182"
#define OUT_MAX		( 64 * 1024 )
#define RUN_TMO		120000		/* ms: the selftest types 30 questions four times */
#define MILLI		( KB_UNIT / 1000 )

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
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

/* Code points as UTF-8 compared with a string */
LOCAL BOOL cp_is( CONST UH *cp, CONST char *s )
{
	UB	b[64];

	if ( cp == NULL ) return (BOOL)( s == NULL );
	(void)kp_utf8(cp, KB_CP, b, sizeof(b));
	return s_eq(b, s);
}

/* A place in thousandths of a key */
LOCAL INT milli( INT v )
{
	return ( v >= 0 ) ? ( v + MILLI / 2 ) / MILLI : -( ( -v + MILLI / 2 ) / MILLI );
}

LOCAL CONST KBKEY *key_of( CONST KBLAYOUT *l, UINT u )
{
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( l->k[i].usage == u ) return &l->k[i];
	}
	return NULL;
}

/* A resource of the template, whole, in buf: its length, or an error */
LOCAL INT tpl_res( CONST char *name, UB *buf, INT max )
{
	TS_UUID	u;
	SZ	asz = 0;
	INT	at = 0;
	ID	k;
	ER	er;

	if ( ts_str_to_uuid(TEMPLATE, &u) < E_OK ) return E_PAR;
	k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return (INT)k;
	for ( ;; ) {
		er = ob_rea_res(k, (CONST UB *)name, (D)at, buf + at, max - 1 - at, &asz);
		if ( er < E_OK || asz <= 0 ) break;
		at += (INT)asz;
	}
	ob_cls_obj(k);
	if ( at == 0 ) return ( er < E_OK ) ? er : E_NOEXS;
	buf[at] = 0;
	return at;
}

/* ---------------------------------------------------------------- the keys' names */

LOCAL void test_names( void )
{
	KT_ASSERT_EQ(kb_usage((CONST UB *)"KeyQ", 4), 0x14);
	KT_ASSERT_EQ(kb_usage((CONST UB *)"IntlRo", 6), 0x87);
	KT_ASSERT_EQ(kb_usage((CONST UB *)"Backslash", 9), 0x32);
	KT_ASSERT_EQ(kb_usage((CONST UB *)"ShiftRight", 10), KB_K_RSHIFT);
	KT_ASSERT_EQ(kb_usage((CONST UB *)"Key", 3), 0);
	KT_ASSERT_EQ(kb_usage((CONST UB *)"Hyper", 5), 0);
	KT_ASSERT(s_eq((CONST UB *)kb_key_name(0x31), "Backslash"));
	KT_ASSERT(s_eq((CONST UB *)kb_key_name(0x89), "IntlYen"));
	KT_ASSERT(kb_key_name(0xF0) == NULL);
	KT_ASSERT_EQ(kb_roma_key('a'), 0x04);
	KT_ASSERT_EQ(kb_roma_key('-'), 0x2D);
	KT_ASSERT_EQ(kb_roma_char(0x1D), 'z');
	KT_ASSERT_EQ(kb_roma_char(0x28), 0);
	/* the fixed point the turned keys are placed with */
	KT_ASSERT_EQ(kb_sin(0), 0);
	KT_ASSERT_EQ(kb_sin(90000), 1 << 30);
	KT_ASSERT_EQ(kb_cos(180000), -( 1 << 30 ));
	KT_ASSERT(kb_sin(30000) > ( 1 << 29 ) - 4 && kb_sin(30000) < ( 1 << 29 ) + 4);
	KT_ASSERT(kb_sin(-30000) < -( 1 << 29 ) + 4);
	KT_ASSERT_EQ((INT)kb_sqrt(0), 0);
	KT_ASSERT_EQ((INT)kb_sqrt(15), 4);		/* to the nearest */
	KT_ASSERT_EQ((INT)kb_sqrt(1000000), 1000);
}

/* ---------------------------------------------------------------- the keyboards */

LOCAL void test_keyboards( void )
{
	KBLAYOUT	*l = (KBLAYOUT *)Kmalloc(sizeof(KBLAYOUT));
	UB		*buf = (UB *)Kmalloc(OUT_MAX);
	CONST KBKEY	*k;
	INT		n, x, y;

	KT_ASSERT(l != NULL && buf != NULL);
	if ( l == NULL || buf == NULL ) goto out;

	/* the system's 109 keyboard */
	KT_ASSERT_ER(kb_read_keyboard((CONST UB *)kb_jp109_json, s_len(kb_jp109_json), l), E_OK);
	KT_ASSERT(s_eq(l->name, "日本語109キー"));
	KT_ASSERT_EQ(l->n, 77);
	KT_ASSERT_EQ(l->w, 15 * KB_UNIT);
	KT_ASSERT_EQ(l->h, 6 * KB_UNIT + KB_UNIT / 2);
	k = key_of(l, KB_K_ENTER);
	KT_ASSERT(k != NULL && k->x == 13750 * MILLI && k->y == 2500 * MILLI && k->w == 1250 * MILLI && k->h == 2 * KB_UNIT);
	k = key_of(l, 0x1E);
	KT_ASSERT(k != NULL && s_eq(k->label, "1") && s_eq(k->shift, "!"));
	k = key_of(l, 0x27);
	KT_ASSERT(k != NULL && s_eq(k->label, "0") && k->shift[0] == 0);
	k = key_of(l, KB_K_SPACE);
	KT_ASSERT(k != NULL && k->label[0] == 0 && k->w == 4 * KB_UNIT);
	KT_ASSERT(kb_key_middle(l, 0x14, &x, &y) && x == 2 * KB_UNIT && y == 3 * KB_UNIT);

	/* the template's split keyboard, its keys turned */
	n = tpl_res("_1_1.json", buf, OUT_MAX);
	if ( n <= 0 ) {
		tm_printf((UB *)"  no template on the system volume (%d)\n", n);
	} else {
		KT_ASSERT_ER(kb_read_keyboard(buf, n, l), E_OK);
		KT_ASSERT(s_eq(l->name, "TL Split Keyboard 16mm Rev0"));
		tm_printf((UB *)"  split keyboard: %d keys, %d x %d thousandths\n", l->n, milli(l->w), milli(l->h));
		KT_ASSERT_EQ(milli(l->w), 20285);
		KT_ASSERT_EQ(milli(l->h), 7400);
		k = key_of(l, 0x17);			/* KeyT */
		KT_ASSERT(k != NULL && milli(k->x) == 7660 && milli(k->y) == 1469 && k->r == 7000);
		k = key_of(l, 0x88);			/* KanaMode */
		KT_ASSERT(k != NULL && milli(k->x) == 10344 && milli(k->y) == 6173 && k->r == -57000
			  && s_eq(k->label, "日"));
		k = key_of(l, KB_K_SPACE);
		KT_ASSERT(k != NULL && milli(k->x) == 6060 && milli(k->y) == 4380);
		KT_ASSERT(l->k[0].usage == 0x26 && milli(l->k[0].x) == 15214 && milli(l->k[0].y) == 42);
	}
out:
	if ( l != NULL ) Kfree(l);
	if ( buf != NULL ) Kfree(buf);
}

/* ---------------------------------------------------------------- the kana layouts */

LOCAL void test_kana( void )
{
	KBKANAMAP	*m = (KBKANAMAP *)Kmalloc(sizeof(KBKANAMAP));
	KBTRANS		*t = (KBTRANS *)Kmalloc(sizeof(KBTRANS));
	UB		*buf = (UB *)Kmalloc(OUT_MAX);
	INT		n;

	KT_ASSERT(m != NULL && t != NULL && buf != NULL);
	if ( m == NULL || t == NULL || buf == NULL ) goto out;

	/* the TRON kana layout the system makes when an object has none */
	KT_ASSERT_ER(kb_read_kana((CONST UB *)kb_tron_json, s_len(kb_tron_json), m), E_OK);
	KT_ASSERT_EQ(m->n, 31);
	KT_ASSERT(cp_is(kb_kana_of(m, 0x14, KB_PLAIN), "ら"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x14, KB_LEFT), "ひ"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x14, KB_RIGHT), "び"));
	KT_ASSERT(kb_kana_of(m, 0x34, KB_LEFT) == NULL);	/* Quote: ー alone */
	KT_ASSERT_EQ(kb_hand(m, 0x14), 1);
	KT_ASSERT_EQ(kb_hand(m, 0x2C), 2);			/* Space */
	KT_ASSERT_EQ(kb_hand(m, 0x1E), 0);			/* not said */

	/* JIS kana: either shift */
	KT_ASSERT_ER(kb_read_kana((CONST UB *)kb_jis_json, s_len(kb_jis_json), m), E_OK);
	KT_ASSERT(s_eq(m->name, "JISかな"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x20, KB_PLAIN), "あ"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x20, KB_LEFT), "ぁ") && cp_is(kb_kana_of(m, 0x20, KB_RIGHT), "ぁ"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x87, KB_PLAIN), "ろ"));
	KT_ASSERT(cp_is(kb_kana_of(m, 0x89, KB_PLAIN), "ー"));
	KT_ASSERT(kb_kana_of(m, 0x1E, KB_LEFT) == NULL);
	KT_ASSERT(m->has_hand && m->has_finger);

	n = tpl_res("_2_1.json", buf, OUT_MAX);
	if ( n > 0 ) {
		KT_ASSERT_ER(kb_read_kana(buf, n, m), E_OK);
		KT_ASSERT(s_eq(m->name, "薙刀式v17"));
		tm_printf((UB *)"  薙刀式: %d keys, %d chords\n", m->n, m->nc);
		KT_ASSERT_EQ(m->nc, 108);
		KT_ASSERT(kb_kana_of(m, 0x18, KB_PLAIN) == NULL);	/* KeyU: shifted only */
		KT_ASSERT(cp_is(kb_kana_of(m, 0x18, KB_LEFT), "さ"));
		KT_ASSERT(m->c[0].nkey == 2 && m->c[0].keys[0] == 0x0D && m->c[0].keys[1] == 0x09 && cp_is(m->c[0].cp, "が"));
	}
	n = tpl_res("_3_0.json", buf, OUT_MAX);
	if ( n > 0 ) {
		KT_ASSERT_EQ(kb_kind(buf, n), KB_KIND_TRANS);
		KT_ASSERT_ER(kb_read_trans(buf, n, t), E_OK);
		KT_ASSERT(s_eq(t->name, "Dvorak (ファームウェア)"));
		KT_ASSERT_EQ(t->n, 33);
		KT_ASSERT_EQ(kb_trans(t, 0x33), 0x14);	/* ; reported for q */
		KT_ASSERT_EQ(kb_trans(t, 0x04), 0x04);
		KT_ASSERT_EQ(kb_trans(t, 0x28), 0x28);	/* not changed */
	}
	KT_ASSERT_EQ(kb_kind((CONST UB *)"{\"mapping\":{}}", 14), KB_KIND_KANA);
	KT_ASSERT_EQ(kb_kind((CONST UB *)"{\"meta\":{\"format\":\"kle-extended\"}}", 34), KB_KIND_KEYBOARD);
	KT_ASSERT_EQ(kb_kind((CONST UB *)"{\"layout\":0}", 12), KB_KIND_NONE);
	KT_ASSERT_EQ(kb_kind((CONST UB *)"[1]", 3), KB_KIND_NONE);
	KT_ASSERT_ER(kb_read_kana((CONST UB *)"{\"mapping\":", 11, m), E_PAR);
out:
	if ( m != NULL ) Kfree(m);
	if ( t != NULL ) Kfree(t);
	if ( buf != NULL ) Kfree(buf);
}

/* ---------------------------------------------------------------- JSON as it is written */

LOCAL void test_json( void )
{
	static CONST struct {
		CONST char	*name;
		INT		len;
		UW		hash;
	} want[] = {
		/* JSON.stringify(JSON.parse(the file)) */
		{ "_1_0.json",	1877,	0xDCBA8756U },
		{ "_1_1.json",	2991,	0xD103B431U },
		{ "_2_0.json",	3516,	0xC613747DU },	/* the right hand's Shift planes as the TRON keyboard has them */
		{ "_2_1.json",	8323,	0x62BA5341U },
		{ "_3_0.json",	743,	0xF6D55EEDU },
	};
	UB	*in = (UB *)Kmalloc(OUT_MAX), *out = (UB *)Kmalloc(OUT_MAX);
	INT	i, n, m;

	KT_ASSERT(in != NULL && out != NULL);
	if ( in == NULL || out == NULL ) goto done;
	for ( i = 0; i < (INT)( sizeof(want) / sizeof(want[0]) ); i++ ) {
		n = tpl_res(want[i].name, in, OUT_MAX);
		if ( n <= 0 ) {
			tm_printf((UB *)"  %s: not on the system volume\n", want[i].name);
			continue;
		}
		m = kb_json_compact(in, n, out, OUT_MAX);
		tm_printf((UB *)"  %s: %d bytes as kept, %d as written\n", want[i].name, n, m);
		KT_ASSERT_EQ(m, want[i].len);
		KT_ASSERT_EQ(fnv(out, m), want[i].hash);
	}
	/* the system's layouts are kept as they are written already */
	n = s_len(kb_jp109_json);
	KT_ASSERT_EQ(kb_json_compact((CONST UB *)kb_jp109_json, n, out, OUT_MAX), n);
	n = s_len(kb_jis_json);
	KT_ASSERT_EQ(kb_json_compact((CONST UB *)kb_jis_json, n, out, OUT_MAX), n);
	/* numbers and strings as JavaScript writes them */
	{
		CONST char	*lit = "[ 1.0, -0, 0.50, 1e2, 1E-7, 2.5e+3, 0.000001, -3.250, \"a\\/\\u3042\\n\" ]";

		n = kb_json_compact((CONST UB *)lit, s_len(lit), out, OUT_MAX);
	}
	KT_ASSERT(n > 0);
	if ( n > 0 ) {
		out[n] = 0;
		KT_ASSERT(s_eq(out, "[1,0,0.5,100,1e-7,2500,0.000001,-3.25,\"a/あ\\n\"]"));
	}
	KT_ASSERT_EQ(kb_json_compact((CONST UB *)"[1,", 3, out, OUT_MAX), E_PAR);
	KT_ASSERT_EQ(kb_json_compact((CONST UB *)"[1,2,3]", 7, out, 4), E_LIMIT);
done:
	if ( in != NULL ) Kfree(in);
	if ( out != NULL ) Kfree(out);
}

/* ---------------------------------------------------------------- the questions */

LOCAL CONST char kp_doc[] =
	"<tad version=\"1.0\"><document><p>漢字(かんじ)&amp;<font size=\"9\"/>かな　</p><p/>"
	"<p>　 </p><paper/><p>朝(あさ)ごはん に 夏休(なつやす)み</p></document></tad>";

LOCAL void test_questions( void )
{
	KPENG	*e = (KPENG *)Kmalloc(sizeof(KPENG));
	UB	b[256];
	INT	n;

	KT_ASSERT(e != NULL);
	if ( e == NULL ) return;
	kp_init(e);
	KT_ASSERT_EQ(kp_questions(e, (CONST UB *)kp_doc, s_len(kp_doc)), 2);
	kp_load(e, 0);
	KT_ASSERT_EQ(e->nph, 1);
	(void)kp_utf8(e->in, e->nin, b, sizeof(b));
	KT_ASSERT(s_eq(b, "かんじ&かな"));
	KT_ASSERT_EQ(e->nseg, 2);
	kp_load(e, 1);
	KT_ASSERT_EQ(e->nph, 3);
	KT_ASSERT_EQ(e->ph[0].nseg, 2);			/* 朝(あさ) and ごはん */
	(void)kp_utf8(e->in + e->ph[0].in0, e->ph[0].nin, b, sizeof(b));
	KT_ASSERT(s_eq(b, "あさごはん"));
	(void)kp_utf8(e->q + e->seg[e->ph[2].seg0].d0, e->seg[e->ph[2].seg0].d1 - e->seg[e->ph[2].seg0].d0, b, sizeof(b));
	KT_ASSERT(s_eq(b, "夏休"));
	KT_ASSERT_EQ(kp_questions(e, (CONST UB *)"<tad/>", 6), 1);	/* none: こんにちは */
	kp_load(e, 0);
	(void)kp_utf8(e->in, e->nin, b, sizeof(b));
	KT_ASSERT(s_eq(b, "こんにちは"));

	/* the template's thirty */
	{
		TS_UUID	u;
		SZ	size = 0;
		UB	*x;

		KT_ASSERT_ER(ts_str_to_uuid(TEMPLATE, &u), E_OK);
		x = om_obj_record(&u, 0, &size);
		if ( x == NULL ) {
			tm_printf((UB *)"  no template on the system volume\n");
		} else {
			n = kp_questions(e, x, (INT)size);
			KT_ASSERT_EQ(n, 30);
			kp_load(e, 0);
			KT_ASSERT_EQ(e->nph, 6);
			(void)kp_utf8(e->in + e->ph[0].in0, e->ph[0].nin, b, sizeof(b));
			KT_ASSERT(s_eq(b, "とうきょう"));
			kp_load(e, 29);
			(void)kp_utf8(e->q, e->nqcp, b, sizeof(b));
			KT_ASSERT(s_eq(b, "富士山(ふじさん) は 日本(にほん) で 一番(いちばん) 高(たか)い 山(やま) です"));
			Kfree(x);
		}
	}
	Kfree(e);
}

/* ---------------------------------------------------------------- judging */

LOCAL UINT zero_rnd( void )
{
	return 0;
}

LOCAL KBSET	kp_held;

LOCAL void press( KPENG *e, UINT u, BOOL l, BOOL r, CONST KBLAYOUT *kb, UD now, KPRES *res )
{
	KB_ADD(&kp_held, u);
	kp_key(e, u, l, r, &kp_held, kb, now, res);
}

LOCAL void release( UINT u )
{
	KB_DEL(&kp_held, u);
}

LOCAL void tap( KPENG *e, UINT u, BOOL l, BOOL r, CONST KBLAYOUT *kb, UD now, KPRES *res )
{
	press(e, u, l, r, kb, now, res);
	release(u);
}

/* A practice of one question */
LOCAL void one( KPENG *e, CONST char *q, INT mode, CONST KBKANAMAP *m, UD now )
{
	static UB	doc[512];
	INT		n = 0, i;
	CONST char	*a = "<p>", *z = "</p>";

	for ( i = 0; a[i] != 0; i++ ) doc[n++] = (UB)a[i];
	for ( i = 0; q[i] != 0 && n < 500; i++ ) doc[n++] = (UB)q[i];
	for ( i = 0; z[i] != 0; i++ ) doc[n++] = (UB)z[i];
	kp_init(e);
	(void)kp_questions(e, doc, n);
	kp_set_map(e, mode, m);
	kp_start(e, zero_rnd, now);
	knl_memset(&kp_held, 0, sizeof(kp_held));
}

LOCAL void test_judge( void )
{
	KPENG		*e = (KPENG *)Kmalloc(sizeof(KPENG));
	KBKANAMAP	*m = (KBKANAMAP *)Kmalloc(sizeof(KBKANAMAP));
	KBLAYOUT	*kb = (KBLAYOUT *)Kmalloc(sizeof(KBLAYOUT));
	UB		*buf = (UB *)Kmalloc(OUT_MAX);
	KPNEXT		nx[8];
	KPRES		r;
	INT		n;

	KT_ASSERT(e != NULL && m != NULL && kb != NULL && buf != NULL);
	if ( e == NULL || m == NULL || kb == NULL || buf == NULL ) goto out;
	(void)kb_read_keyboard((CONST UB *)kb_jp109_json, s_len(kb_jp109_json), kb);

	/* TRON: a plain kana, a shifted one, a wrong key, a key that types nothing */
	(void)kb_read_kana((CONST UB *)kb_tron_json, s_len(kb_tron_json), m);
	one(e, "あひ ぱ", KP_M_KANA, m, 1000);
	n = kp_next(e, 4, nx);
	KT_ASSERT_EQ(n, 4);					/* あ, ひ (left), は, ゜ */
	KT_ASSERT(nx[0].usage == 0x12 && nx[0].shift == KP_SH_NONE);
	KT_ASSERT(nx[1].usage == 0x14 && nx[1].shift == KP_SH_LEFT);
	KT_ASSERT(nx[2].usage == 0x15 && cp_is(nx[2].cp, "は"));
	KT_ASSERT(nx[3].usage == 0x37 && nx[3].shift == KP_SH_RIGHT && cp_is(nx[3].cp, "゜"));
	tap(e, 0x14, FALSE, FALSE, kb, 1100, &r);		/* ら: wrong */
	KT_ASSERT(r.correct == 0 && r.has_char && cp_is(r.cp, "ら"));
	tap(e, 0x1E, FALSE, FALSE, kb, 1150, &r);		/* nothing on it */
	KT_ASSERT(r.correct == 0 && !r.has_char);
	tap(e, 0x12, FALSE, FALSE, kb, 1200, &r);
	KT_ASSERT(r.correct == 1 && cp_is(r.cp, "あ"));
	tap(e, 0x14, FALSE, TRUE, kb, 1300, &r);		/* び with the right shift: wrong */
	KT_ASSERT(r.correct == 0 && cp_is(r.cp, "び"));
	tap(e, 0x14, TRUE, FALSE, kb, 1400, &r);
	KT_ASSERT(r.correct == 1 && r.phrase_done && !r.finished);
	tap(e, 0x15, FALSE, FALSE, kb, 1500, &r);		/* は, and ゜ awaited */
	KT_ASSERT(r.correct == -1 && r.partial);
	KT_ASSERT_EQ(kp_next(e, 1, nx), 1);
	KT_ASSERT(nx[0].usage == 0x37);
	tap(e, 0x37, FALSE, TRUE, kb, 1600, &r);
	KT_ASSERT(r.correct == 1 && r.finished && cp_is(r.cp, "ぱ"));
	KT_ASSERT(e->completed && !e->practising);
	KT_ASSERT(e->correct == 3 && e->wrong == 2 && e->shift == 2 && e->keys == 7);

	/* JIS kana: が is か and ゛, either shift types ぁ */
	(void)kb_read_kana((CONST UB *)kb_jis_json, s_len(kb_jis_json), m);
	one(e, "がぁ", KP_M_KANA, m, 0);
	KT_ASSERT_EQ(kp_next(e, 3, nx), 3);
	KT_ASSERT(nx[0].usage == 0x17 && nx[1].usage == 0x2F && nx[2].usage == 0x20);
	tap(e, 0x17, FALSE, FALSE, kb, 10, &r);
	KT_ASSERT(r.partial);
	tap(e, 0x2F, FALSE, FALSE, kb, 20, &r);
	KT_ASSERT(r.correct == 1 && cp_is(r.cp, "が"));
	tap(e, 0x20, TRUE, FALSE, kb, 30, &r);
	KT_ASSERT(r.correct == 1 && r.finished);

	/* 薙刀式: a chord waits for its last key; a key outside it is wrong */
	n = tpl_res("_2_1.json", buf, OUT_MAX);
	if ( n > 0 ) {
		(void)kb_read_kana(buf, n, m);
		one(e, "がく", KP_M_KANA, m, 0);
		KT_ASSERT_EQ(kp_next(e, 1, nx), 1);
		KT_ASSERT(nx[0].nkey == 2 && nx[0].keys[0] == 0x0D && nx[0].keys[1] == 0x09);
		press(e, 0x0B, FALSE, FALSE, kb, 5, &r);	/* KeyH: not of the chord */
		KT_ASSERT(r.correct == 0 && r.combo_wrong);
		release(0x0B);
		press(e, 0x0D, FALSE, FALSE, kb, 10, &r);
		KT_ASSERT(r.correct == -1 && r.partial && r.ncombo == 2);
		press(e, 0x09, FALSE, FALSE, kb, 20, &r);
		KT_ASSERT(r.correct == 1 && cp_is(r.cp, "が") && r.ncombo == 2);
		release(0x0D);
		release(0x09);
		tap(e, 0x0B, FALSE, FALSE, kb, 30, &r);
		KT_ASSERT(r.correct == 1 && r.finished);
		/* ぽ: its chord, ゜ being on no key */
		one(e, "ぽ", KP_M_KANA, m, 0);
		KT_ASSERT_EQ(kp_next(e, 1, nx), 1);
		KT_ASSERT(nx[0].nkey == 2 && nx[0].keys[0] == 0x10 && nx[0].keys[1] == 0x1D);
	}

	/* romaji: きゃ as kya or ki and xya, ん as n before b and nn at the end, っ by doubling */
	(void)kb_read_kana((CONST UB *)kb_jis_json, s_len(kb_jis_json), m);
	one(e, "きゃく しんぶん かっこ", KP_M_ROMAJI, m, 0);
	KT_ASSERT_EQ(kp_next(e, 4, nx), 4);
	KT_ASSERT(nx[0].usage == 0x0E && nx[1].usage == 0x1C && nx[2].usage == 0x04 && nx[3].usage == 0x0E);
	KT_ASSERT_EQ(kp_expected_len(e), 2);
	tap(e, 0x0E, FALSE, FALSE, kb, 1, &r);			/* k */
	tap(e, 0x0C, FALSE, FALSE, kb, 2, &r);			/* i: き, or the start of kixya */
	KT_ASSERT(r.correct == 1 && e->cch == 0);
	tap(e, 0x0F, FALSE, FALSE, kb, 3, &r);			/* l: き, then lya for ゃ */
	KT_ASSERT(r.correct == 1 && e->cch == 1);
	tap(e, 0x1C, FALSE, FALSE, kb, 4, &r);
	tap(e, 0x04, FALSE, FALSE, kb, 5, &r);
	KT_ASSERT(e->cch == 2);
	tap(e, 0x0E, FALSE, FALSE, kb, 6, &r);
	tap(e, 0x0F, FALSE, FALSE, kb, 7, &r);			/* l: wrong */
	KT_ASSERT(r.correct == 0 && e->wrong == 1);
	tap(e, 0x18, FALSE, FALSE, kb, 8, &r);
	KT_ASSERT(r.correct == 1 && r.phrase_done);
	tap(e, 0x16, FALSE, FALSE, kb, 9, &r);			/* s, i: し */
	tap(e, 0x0C, FALSE, FALSE, kb, 10, &r);
	tap(e, 0x11, FALSE, FALSE, kb, 11, &r);			/* n */
	KT_ASSERT(e->cch == 1);
	tap(e, 0x05, FALSE, FALSE, kb, 12, &r);			/* b: ん by its n, b for ぶ */
	KT_ASSERT(r.correct == 1 && e->cch == 2);
	tap(e, 0x18, FALSE, FALSE, kb, 13, &r);
	tap(e, 0x11, FALSE, FALSE, kb, 14, &r);
	KT_ASSERT(e->cch == 3);
	tap(e, 0x11, FALSE, FALSE, kb, 15, &r);			/* nn at the end */
	KT_ASSERT(r.phrase_done);
	tap(e, 0x0E, FALSE, FALSE, kb, 16, &r);			/* ka */
	tap(e, 0x04, FALSE, FALSE, kb, 17, &r);
	KT_ASSERT_EQ(kp_next(e, 3, nx), 3);
	KT_ASSERT(nx[0].usage == 0x0E && nx[1].usage == 0x0E && nx[2].usage == 0x12);
	tap(e, 0x0E, FALSE, FALSE, kb, 18, &r);			/* k: っ */
	KT_ASSERT(e->cch == 2);
	tap(e, 0x0E, FALSE, FALSE, kb, 19, &r);
	tap(e, 0x12, FALSE, FALSE, kb, 20, &r);
	KT_ASSERT(r.correct == 1 && r.finished);
	KT_ASSERT(e->correct == 10 && e->wrong == 1);
out:
	if ( e != NULL ) Kfree(e);
	if ( m != NULL ) Kfree(m);
	if ( kb != NULL ) Kfree(kb);
	if ( buf != NULL ) Kfree(buf);
}

/* ---------------------------------------------------------------- the scores */

LOCAL void test_score( void )
{
	KPENG		*e = (KPENG *)Kmalloc(sizeof(KPENG));
	KBKANAMAP	*m = (KBKANAMAP *)Kmalloc(sizeof(KBKANAMAP));
	KBLAYOUT	*kb = (KBLAYOUT *)Kmalloc(sizeof(KBLAYOUT));
	KPSCORE		s;
	KPRES		r;
	char		b[16];
	INT		cur = 0, tot = 0;

	KT_ASSERT(e != NULL && m != NULL && kb != NULL);
	if ( e == NULL || m == NULL || kb == NULL ) goto out;
	(void)kb_read_keyboard((CONST UB *)kb_jp109_json, s_len(kb_jp109_json), kb);
	(void)kb_read_kana((CONST UB *)kb_tron_json, s_len(kb_tron_json), m);

	one(e, "あい", KP_M_KANA, m, 1000);
	kp_score(e, 1000, &s);
	KT_ASSERT(s.speed == 0 && s.accuracy == 1000 && s.shift_ratio == 0 && s.distance == 0 && s.total == 0);
	kp_progress(e, &cur, &tot);
	KT_ASSERT(cur == 1 && tot == 1);
	tap(e, 0x12, FALSE, FALSE, kb, 1500, &r);		/* あ: O, the right hand */
	tap(e, 0x14, FALSE, FALSE, kb, 2000, &r);		/* ら: Q, wrong, 8 keys to the left */
	tap(e, 0x0D, FALSE, FALSE, kb, 4000, &r);		/* い: J, 6.25 across and one down */
	KT_ASSERT(r.finished);
	kp_score(e, 9999, &s);					/* done: the time stopped at the last key */
	KT_ASSERT_EQ(s.correct, 2);
	KT_ASSERT_EQ(s.wrong, 1);
	KT_ASSERT_EQ(s.keys, 3);
	KT_ASSERT_EQ(s.speed, 40);				/* two letters in three seconds */
	KT_ASSERT_EQ(s.accuracy, 667);
	KT_ASSERT(s.left == 1 && s.right == 2);
	KT_ASSERT_EQ(s.shift_ratio, 0);
	KT_ASSERT_EQ(s.distance, 716);				/* 14.329 / 2 */
	KT_ASSERT_EQ(s.total, 143);
	KT_ASSERT_EQ(s.seconds, 3);
	kp_fixed(s.accuracy, 1, b);
	KT_ASSERT(s_eq((UB *)b, "66.7"));
	kp_fixed(s.distance, 2, b);
	KT_ASSERT(s_eq((UB *)b, "7.16"));
	kp_fixed(5, 2, b);
	KT_ASSERT(s_eq((UB *)b, "0.05"));
	kp_fixed(1000, 1, b);
	KT_ASSERT(s_eq((UB *)b, "100.0"));
out:
	if ( e != NULL ) Kfree(e);
	if ( m != NULL ) Kfree(m);
	if ( kb != NULL ) Kfree(kb);
}

/* ---------------------------------------------------------------- what the object keeps */

LOCAL void test_record( void )
{
	T_OBCRE		c;
	TS_UUID		u;
	KBLAYOUT	*a = (KBLAYOUT *)Kmalloc(sizeof(KBLAYOUT)), *b = (KBLAYOUT *)Kmalloc(sizeof(KBLAYOUT));
	UB		*out = (UB *)Kmalloc(OUT_MAX), *back = (UB *)Kmalloc(OUT_MAX), names[256];
	SZ		asz = 0;
	INT		n, m, cnt = 0, i;
	ID		k;
	ER		er;

	KT_ASSERT(a != NULL && b != NULL && out != NULL && back != NULL);
	if ( a == NULL || b == NULL || out == NULL || back == NULL ) goto done;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.name = (CONST UB *)"かな入力練習の試験";
	er = ob_cre_obj(&c, &u);
	if ( er < E_OK ) {
		tm_printf((UB *)"  no object can be made (%d)\n", (INT)er);
		goto done;
	}
	k = ob_opn_obj(&u, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) goto gone;

	/* a keyboard written as a resource the way TADjs Desktop names the file, and read back */
	n = kb_json_compact((CONST UB *)kb_jp109_json, s_len(kb_jp109_json), out, OUT_MAX);
	KT_ASSERT_ER(ob_wri_res(k, (CONST UB *)"_1_0.json", out, n), E_OK);
	n = kb_json_compact((CONST UB *)kb_jis_json, s_len(kb_jis_json), out, OUT_MAX);
	KT_ASSERT_ER(ob_wri_res(k, (CONST UB *)"_2_0.json", out, n), E_OK);
	m = 0;
	while ( ob_rea_res(k, (CONST UB *)"_2_0.json", (D)m, back + m, OUT_MAX - m, &asz) >= E_OK && asz > 0 ) m += (INT)asz;
	KT_ASSERT_EQ(m, n);
	for ( i = 0; i < n && i < m && out[i] == back[i]; i++ ) ;
	KT_ASSERT_EQ(i, n);
	m = 0;
	while ( ob_rea_res(k, (CONST UB *)"_1_0.json", (D)m, back + m, OUT_MAX - m, &asz) >= E_OK && asz > 0 ) m += (INT)asz;
	KT_ASSERT_ER(kb_read_keyboard(back, m, a), E_OK);
	KT_ASSERT_ER(kb_read_keyboard((CONST UB *)kb_jp109_json, s_len(kb_jp109_json), b), E_OK);
	KT_ASSERT(a->n == b->n && a->w == b->w && a->h == b->h);
	for ( i = 0; i < a->n && i < b->n; i++ ) {
		if ( a->k[i].usage != b->k[i].usage || a->k[i].x != b->k[i].x || a->k[i].y != b->k[i].y ) break;
	}
	KT_ASSERT_EQ(i, a->n);
	KT_ASSERT_ER(ob_lst_res(k, names, sizeof(names), &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(ob_rea_res(k, (CONST UB *)"_3_0.json", 0, back, OUT_MAX, &asz) < E_OK || asz == 0, TRUE);
	ob_cls_obj(k);
gone:
	(void)ob_del_obj(&u);
done:
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);
	if ( out != NULL ) Kfree(out);
	if ( back != NULL ) Kfree(back);
}

/* ---------------------------------------------------------------- keys let go */

LOCAL ID port_open( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, u) < E_OK ) return E_SYS;
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

LOCAL ER take( ID port, T_OBNTM *m )
{
	SZ	asz = 0;
	ER	er = ob_rea_rec(port, 0, 0, m, sizeof(*m), &asz);

	return ( er >= E_OK && asz != (SZ)sizeof(*m) ) ? E_OBJ : er;
}

/* The window's number, from its metadata (tessronos.window.number) */
LOCAL INT number_of( ID key )
{
	UB	j[512];
	SZ	asz = 0;
	T_JSON	root, ts, w;

	if ( ob_get_atr(key, j, sizeof(j) - 1, &asz) < E_OK ) return -1;
	if ( js_parse(j, (INT)asz, &root) < E_OK || js_get(&root, "tessronos", &ts) < E_OK
	  || js_get(&ts, "window", &w) < E_OK ) {
		return -1;
	}
	return (INT)js_get_num(&w, "number", -1);
}

LOCAL void test_keyup( void )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBNTM		m;
	T_WMEV		ev;
	TS_UUID		w, ch1, ch2;
	ID		kw, p1, p2;
	INT		wid;
	ER		er;
	CONST char	*json = "{\"rect\":[40,50,360,290]}";

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"kana keys";
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, &w);
	if ( er == E_NOSPT || er == E_NOEXS ) KT_SKIP("no window manager");
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return;
	kw = ob_opn_obj(&w, OB_OP_ALL);
	wid = ( kw > 0 ) ? number_of(kw) : -1;
	KT_ASSERT(kw > 0 && wid > 0);
	p1 = port_open(&ch1);
	p2 = port_open(&ch2);
	KT_ASSERT(p1 > 0 && p2 > 0);
	if ( kw <= 0 || wid <= 0 || p1 <= 0 || p2 <= 0 ) goto out;

	/* one asks to hear keys let go, the other only presses */
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_KEY;
	req.id = 1;
	req.flags = OB_N_KEYUP;
	KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &req, p1) > 0);
	req.id = 2;
	req.flags = 0;
	KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &req, p2) > 0);

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.code = 0x14;
	ev.mods = HID_MOD_RSHIFT;
	ev.wid = wid;
	knl_wmobj_event(&ev);
	ev.type = HID_EV_KEY_UP;
	knl_wmobj_event(&ev);

	KT_ASSERT_ER(take(p1, &m), E_OK);
	KT_ASSERT(m.event == OB_E_KEY && m.code == 0x14 && m.mods == HID_MOD_RSHIFT);
	KT_ASSERT_ER(take(p1, &m), E_OK);
	KT_ASSERT(m.event == OB_E_KEY && m.code == 0x14 && m.mods == ( HID_MOD_RSHIFT | OB_KEY_UP ));
	KT_ASSERT_ER(take(p1, &m), E_TMOUT);
	KT_ASSERT_ER(take(p2, &m), E_OK);
	KT_ASSERT(m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0);
	KT_ASSERT_ER(take(p2, &m), E_TMOUT);
out:
	if ( p1 > 0 ) ob_cls_obj(p1);
	if ( p2 > 0 ) ob_cls_obj(p2);
	(void)ob_del_obj(&ch1);
	(void)ob_del_obj(&ch2);
	if ( kw > 0 ) ob_cls_obj(kw);
	(void)ob_del_obj(&w);
}

/* ---------------------------------------------------------------- the program on an object */

/* A window by its title, waited for: its number, 0 when it did not come */
LOCAL INT win_named( CONST char *title, INT ms )
{
	INT	wid, k;
	char	t[WM_TITLE_MAX];

	/* every window, pop-ups among them: a panel that asks is one */
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

/* A key pressed and let go in a window, and time for the program to take it */
LOCAL void key_to( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.code = code;
	ev.mods = mods;
	ev.wid = wid;
	knl_wmobj_event(&ev);
	tk_dly_tsk(60);
	ev.type = HID_EV_KEY_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(200);
}

/*
 * The program on a copy of the template whose record 0 is one question,
 * あい: Enter begins, O and J type it with the TRON kana layout, the
 * result is shown in its panel, Enter takes the panel away and Ctrl+E
 * closes the window. The screen is kept as PPM pictures on /boot.
 */
LOCAL CONST char kp_one[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>あい</p></document></tad>";

LOCAL void test_window( void )
{
	TS_UUID	tpl, cp, prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	SZ	asz = 0;
	ID	k, pid;
	INT	wid, res;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(TEMPLATE, &tpl), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_KANATYPE, &prog), E_OK);
	if ( ob_cpy_obj(&tpl, NULL, &cp) < E_OK ) KT_SKIP("no template on the system volume");
	k = ob_opn_obj(&cp, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k <= 0 ) goto gone;
	KT_ASSERT_ER(ob_wri_rec(k, 0, 0, kp_one, sizeof(kp_one) - 1, &asz), E_OK);
	KT_ASSERT_ER(ob_trn_rec(k, 0, sizeof(kp_one) - 1), E_OK);
	ob_cls_obj(k);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &cp;
	c.argsz = sizeof(cp);
	er = ob_cre_obj(&c, &pu);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) goto gone;
	pid = knl_prc_of_uuid(&pu);
	wid = win_named("かな入力練習", 20000);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto stop;
	(void)wm_focus(wid);
	tk_dly_tsk(500);
	(void)kt_shot("/boot/KANA0.PPM");
	key_to(wid, 0x28, 0);				/* Enter: begun */
	key_to(wid, 0x12, 0);				/* O: あ */
	tk_dly_tsk(300);				/* the green flash gone */
	(void)kt_shot("/boot/KANA1.PPM");
	key_to(wid, 0x0D, 0);				/* J: い, and done */
	res = win_named("結果", 10000);
	KT_ASSERT(res > 0);
	tk_dly_tsk(500);
	(void)kt_shot("/boot/KANA2.PPM");
	if ( res > 0 ) {
		(void)wm_focus(res);
		key_to(res, 0x28, 0);			/* Enter: the panel away */
	}
	KT_ASSERT_EQ(win_named("結果", 300), 0);
	key_to(wid, 0x08, HID_MOD_LCTRL);		/* Ctrl+E: closed */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 10000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
gone:
	(void)ob_del_obj(&cp);
}

/* ---------------------------------------------------------------- the program */

LOCAL void test_program( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	SYSTIM	t0, t1;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_KANATYPE, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	(void)tk_get_otm(&t0);
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of かな入力練習");
	}
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	(void)tk_get_otm(&t1);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	tm_printf((UB *)"  exit code %d in %d ms\n", psts.exitcd, (INT)( t1.lo - t0.lo ));
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_kana( void )
{
	tm_printf((UB *)"\n[kana] かな入力練習 (design 11.14)\n");
	KT_RUN(test_names);
	KT_RUN(test_keyboards);
	KT_RUN(test_kana);
	KT_RUN(test_json);
	KT_RUN(test_questions);
	KT_RUN(test_judge);
	KT_RUN(test_score);
	KT_RUN(test_record);
	KT_RUN(test_keyup);
	KT_RUN_SCREEN(test_window);
	KT_RUN(test_program);
}
