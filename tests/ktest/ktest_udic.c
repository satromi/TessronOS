/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_udic.c
 *	ユーザ辞書 and 単語登録 (design 11.16)
 *
 *	The dictionary's text: words put in the order of their readings,
 *	one word once under a reading, its part changed in place, taken
 *	out; readings that cannot be refused; katakana read as hiragana;
 *	looked up and found. Files of words read in: the lines another
 *	converter writes, in UTF-8 and in UTF-16 with their headings, and
 *	the names of its parts of speech; and given to the engine.
 *
 *	The object: the words written to the user dictionary object and read
 *	back. The converter: a word registered is a candidate for its
 *	reading -- read again when asked, and again on its own when the
 *	object is written, which is what 単語登録 does -- and no longer one
 *	once it is taken out. With mozc (make MOZC=1) the word is among
 *	mozc's candidates; without it the romaji converter offers it first.
 *	What the object held before is put back at the end.
 */

#include "ktest.h"
#include <ts/udic.h>
#include <ts/om.h>
#include <ts/kconv.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>

#define TEXT_MAX	4096

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same( CONST UB *a, INT n, CONST char *b )
{
	INT	i;

	if ( n != s_len(b) ) return FALSE;
	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL str_is( CONST char *a, CONST char *b )
{
	return same((CONST UB *)a, s_len(a), b);
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

LOCAL ER put( UB *t, INT *p_len, INT max, CONST char *y, CONST char *w, INT pos )
{
	return ud_put(t, p_len, max, (CONST UB *)y, s_len(y), (CONST UB *)w, s_len(w), pos);
}

/* the words in order, one under a reading once, changed, taken out */
LOCAL void test_text( void )
{
	UB	*t = (UB *)Kmalloc(TEXT_MAX);
	UDENT	e[4];
	INT	len = 0, at = 0;

	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "たなか", "田中", UD_POS_FAMILY), E_OK);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "あいう", "愛宇", UD_POS_NOUN), E_OK);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "たなか", "多中", UD_POS_FAMILY), E_OK);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "カタカナ", "片仮名", UD_POS_NOUN), E_OK);
	KT_ASSERT_EQ(ud_count(t, len), 4);
	KT_ASSERT(same(t, len, "あいう\t愛宇\t名詞\nかたかな\t片仮名\t名詞\n"
			       "たなか\t多中\t姓\nたなか\t田中\t姓\n"));

	/* the same word again: its part is changed, not a second line */
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "たなか", "田中", UD_POS_PLACE), E_OK);
	KT_ASSERT_EQ(ud_count(t, len), 4);
	KT_ASSERT(has(t, len, "たなか\t田中\t地名\n"));
	KT_ASSERT(!has(t, len, "田中\t姓"));

	/* readings that cannot be */
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "", "空", UD_POS_NOUN), E_PAR);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "あ い", "間", UD_POS_NOUN), E_PAR);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "あい", "", UD_POS_NOUN), E_PAR);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "あい", "愛\t", UD_POS_NOUN), E_PAR);
	KT_ASSERT_ER(put(t, &len, TEXT_MAX, "あい", "愛", 99), E_PAR);
	KT_ASSERT_EQ(ud_count(t, len), 4);

	/* looked up: the reading whole, in katakana too */
	KT_ASSERT_EQ(ud_lookup(t, len, (CONST UB *)"たなか", s_len("たなか"), e, 4), 2);
	KT_ASSERT(same(e[0].word, e[0].nword, "多中"));
	KT_ASSERT(same(e[1].word, e[1].nword, "田中"));
	KT_ASSERT_EQ(e[1].pos, UD_POS_PLACE);
	KT_ASSERT_EQ(ud_lookup(t, len, (CONST UB *)"タナカ", s_len("タナカ"), e, 4), 2);
	KT_ASSERT_EQ(ud_lookup(t, len, (CONST UB *)"たな", s_len("たな"), e, 4), 0);

	/* found by the start of the reading, or letters of the word */
	KT_ASSERT(ud_next(t, len, &at, &e[0]));
	KT_ASSERT(ud_match(&e[0], (CONST UB *)"あい", s_len("あい")));
	KT_ASSERT(ud_match(&e[0], (CONST UB *)"宇", s_len("宇")));
	KT_ASSERT(!ud_match(&e[0], (CONST UB *)"いう", s_len("いう")));
	KT_ASSERT(ud_match(&e[0], (CONST UB *)"", 0));
	{
		/* full-width letters as half-width ones, katakana as hiragana */
		UDENT	x;

		x.yomi = (CONST UB *)"てすろん";
		x.nyomi = s_len("てすろん");
		x.word = (CONST UB *)"tessronOS";
		x.nword = s_len("tessronOS");
		x.pos = UD_POS_PROPER;
		KT_ASSERT(ud_match(&x, (CONST UB *)"ｔｅｓｓｒｏｎＯＳ", s_len("ｔｅｓｓｒｏｎＯＳ")));
		KT_ASSERT(ud_match(&x, (CONST UB *)"テス", s_len("テス")));
		KT_ASSERT(!ud_match(&x, (CONST UB *)"ｔｅｓｔ", s_len("ｔｅｓｔ")));
	}

	/* taken out */
	KT_ASSERT_ER(ud_del(t, &len, (CONST UB *)"たなか", s_len("たなか"), (CONST UB *)"多中", s_len("多中")), E_OK);
	KT_ASSERT_ER(ud_del(t, &len, (CONST UB *)"たなか", s_len("たなか"), (CONST UB *)"多中", s_len("多中")), E_NOEXS);
	KT_ASSERT_EQ(ud_count(t, len), 3);
	KT_ASSERT(same(t, len, "あいう\t愛宇\t名詞\nかたかな\t片仮名\t名詞\nたなか\t田中\t地名\n"));

	/* full: nothing is lost */
	len = 0;
	KT_ASSERT_ER(put(t, &len, 40, "あいう", "愛宇", UD_POS_NOUN), E_OK);
	KT_ASSERT_ER(put(t, &len, 40, "かきく", "柿区", UD_POS_NOUN), E_LIMIT);
	KT_ASSERT_EQ(ud_count(t, len), 1);
	KT_ASSERT_ER(put(t, &len, 40, "あいう", "愛宇", UD_POS_PROPER), E_OK);
	KT_ASSERT(same(t, len, "あいう\t愛宇\t固有名詞\n"));

	/* the names of the parts */
	KT_ASSERT(str_is(ud_pos_name(UD_POS_SAHEN), "名詞サ変"));
	KT_ASSERT_EQ(ud_pos_of((CONST UB *)"人名", s_len("人名")), UD_POS_PERSON);
	KT_ASSERT_EQ(ud_pos_of((CONST UB *)"さ変名詞", s_len("さ変名詞")), UD_POS_SAHEN);
	KT_ASSERT_EQ(ud_pos_of((CONST UB *)"地名その他", s_len("地名その他")), UD_POS_PLACE);
	KT_ASSERT_EQ(ud_pos_of((CONST UB *)"ないもの", s_len("ないもの")), 0);
	KT_ASSERT_EQ(ud_pos_offer(0), UD_POS_NOUN);
	Kfree(t);
}

/* files of words read in, and the words as the engine is given them */
LOCAL void test_import( void )
{
	CONST char	*mozc = "# a comment\n"
				"とうきょう\t東京\t地名\tcomment\n"
				"ほげ\tホゲ\n"
				"だめ\n"
				"\n"
				"さとう\t佐藤\t姓\r\n"
				"じっけん\t実験\t名詞サ変";
	/* !Microsoft IME Dictionary Tool, then two words, in UTF-16LE */
	CONST char	*ime = "!Microsoft IME Dictionary Tool\r\n!Format:WORDLIST\r\n"
			       "\r\nきかく\t企画\tさ変名詞\r\nヤマダ\t山田\t人名\r\n";
	UB	*t = (UB *)Kmalloc(TEXT_MAX), *w = (UB *)Kmalloc(TEXT_MAX);
	INT	len = 0, bad = -1, n, i, k;

	KT_ASSERT(t != NULL && w != NULL);
	if ( t == NULL || w == NULL ) return;
	n = ud_import(t, &len, TEXT_MAX, (CONST UB *)mozc, s_len(mozc), &bad);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT_EQ(bad, 1);
	KT_ASSERT(same(t, len, "さとう\t佐藤\t姓\nじっけん\t実験\t名詞サ変\n"
			       "とうきょう\t東京\t地名\nほげ\tホゲ\t名詞\n"));

	/* UTF-16 with its mark: the text turned to UTF-8 a character at a time */
	n = 0;
	w[n++] = 0xFF;
	w[n++] = 0xFE;
	for ( i = 0; ime[i] != 0 && n < TEXT_MAX - 4; ) {
		UB	c = (UB)ime[i];
		UINT	u;

		if ( c < 0x80 ) {
			u = c;
			i += 1;
		} else {
			u = ( (UINT)( c & 0x0F ) << 12 ) | ( (UINT)( ime[i + 1] & 0x3F ) << 6 ) | ( ime[i + 2] & 0x3F );
			i += 3;
		}
		w[n++] = (UB)( u & 0xFF );
		w[n++] = (UB)( u >> 8 );
	}
	k = ud_import(t, &len, TEXT_MAX, w, n, &bad);
	KT_ASSERT_EQ(k, 2);
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(ud_count(t, len), 6);
	KT_ASSERT(has(t, len, "きかく\t企画\t名詞サ変\n"));
	KT_ASSERT(has(t, len, "やまだ\t山田\t人名\n"));

	/* read in again: the same words, nothing more */
	k = ud_import(t, &len, TEXT_MAX, (CONST UB *)mozc, s_len(mozc), &bad);
	KT_ASSERT_EQ(k, 4);
	KT_ASSERT_EQ(ud_count(t, len), 6);

	/* written out is read in as it is */
	{
		UB	*c = (UB *)Kmalloc(TEXT_MAX);
		INT	clen = 0;

		KT_ASSERT(c != NULL);
		if ( c != NULL ) {
			KT_ASSERT_EQ(ud_import(c, &clen, TEXT_MAX, t, len, &bad), 6);
			KT_ASSERT_EQ(clen, len);
			for ( i = 0; i < len && c[i] == t[i]; i++ ) ;
			KT_ASSERT_EQ(i, len);
			Kfree(c);
		}
	}

	/* given to the engine: the parts as their numbers */
	n = ud_engine(t, len, w, TEXT_MAX);
	KT_ASSERT(n > 0);
	KT_ASSERT(has(w, n, "きかく\t企画\t10\n"));
	KT_ASSERT(has(w, n, "とうきょう\t東京\t9\n"));
	KT_ASSERT_EQ(ud_engine(t, len, w, 10), E_LIMIT);
	Kfree(w);
	Kfree(t);
}

/* ---------------------------------------------------------------- the object */

LOCAL UB	*kept = NULL;		/* what the object held before the tests */
LOCAL INT	kept_len = -1;

LOCAL INT obj_read( UB *buf, INT max )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_DICT_USER, &u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R);
	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, 1, 0, buf, max, &asz);
	ob_cls_obj(k);
	if ( er == E_NOEXS ) return 0;
	return ( er < E_OK ) ? (INT)er : (INT)asz;
}

LOCAL ER obj_write( CONST UB *buf, INT len )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_DICT_USER, &u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( k <= 0 ) return (ER)k;
	er = ob_wri_rec(k, 1, 0, buf, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 1, (UD)len);
	ob_cls_obj(k);
	return er;
}

LOCAL ER obj_words( CONST char *s )
{
	return obj_write((CONST UB *)s, s_len(s));
}

/* written and read back */
LOCAL void test_store( void )
{
	UB	*t = (UB *)Kmalloc(TEXT_MAX);
	INT	n;

	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	if ( kept == NULL ) {
		kept = (UB *)Kmalloc(UD_TEXT_MAX);
		kept_len = ( kept != NULL ) ? obj_read(kept, UD_TEXT_MAX) : -1;
	}
	KT_ASSERT(kept_len >= 0);
	KT_ASSERT_ER(obj_words("あいう\t愛宇\t名詞\nてすろん\t手須論\t固有名詞\n"), E_OK);
	n = obj_read(t, TEXT_MAX);
	KT_ASSERT_EQ(ud_count(t, n), 2);
	KT_ASSERT(same(t, n, "あいう\t愛宇\t名詞\nてすろん\t手須論\t固有名詞\n"));
	/* a shorter text: the rest of the record goes */
	KT_ASSERT_ER(obj_words("あいう\t愛宇\t名詞\n"), E_OK);
	n = obj_read(t, TEXT_MAX);
	KT_ASSERT(same(t, n, "あいう\t愛宇\t名詞\n"));
	Kfree(t);
}

/* ---------------------------------------------------------------- the converter */

LOCAL INT type( INT kid, CONST char *keys, T_KCOUT *out )
{
	INT	r = 0;

	for ( ; *keys != 0; keys++ ) {
		r = kc_key(kid, (UINT)(UB)*keys, 0, out);
		if ( r < 0 ) return r;
	}
	return r;
}

/*
 * The reading typed and converted: the place (from 1) of word among the
 * candidates shown for it, 0 when it is not there. What was typed is
 * taken back, so that nothing is committed.
 */
LOCAL INT rank_of( INT kid, CONST char *roma, CONST char *word, T_KCOUT *out )
{
	UB	*list = (UB *)Kmalloc(4096);
	INT	count = 0, first = 0, total = 0, r, at = 0, i, place = 0;

	if ( list == NULL ) return 0;
	(void)type(kid, roma, out);
	r = kc_key(kid, TSMOZC_K_SPACE, 0, out);
	if ( r >= 0 && str_is(out->text + out->cl[out->clause], word) && out->n_cl - out->n_out == 1 ) {
		place = 1;			/* the first shown */
	}
	if ( place == 0 ) {
		r = kc_key(kid, TSMOZC_K_SPACE, 0, out);
		if ( r >= 0 && ( r & TSMOZC_LIST ) != 0 ) {
			r = kc_list(kid, list, 4096, &count, &first, &total);
			for ( i = 0; r >= 1 && i < count; i++ ) {
				INT	n = s_len((CONST char *)list + at);

				if ( same(list + at, n, word) ) {
					place = first + i;
					break;
				}
				at += n + 1;
			}
			tm_printf((UB *)"  candidates: %d of %d, first %s\n", count, total, list);
		}
	}
	(void)kc_key(kid, TSMOZC_K_CAN, 0, out);
	(void)kc_key(kid, TSMOZC_K_CAN, 0, out);
	(void)kc_key(kid, TSMOZC_K_CAN, 0, out);
	Kfree(list);
	return place;
}

/* a registered word converted to; read again when the object is written; gone once taken out */
LOCAL void test_convert( void )
{
	T_KCOUT	*out = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
	INT	kid, place, w, r;
	BOOL	kanji;

	KT_ASSERT(out != NULL);
	if ( out == NULL ) return;
	if ( kept == NULL ) {
		kept = (UB *)Kmalloc(UD_TEXT_MAX);
		kept_len = ( kept != NULL ) ? obj_read(kept, UD_TEXT_MAX) : -1;
	}
	KT_ASSERT_ER(kc_start(), E_OK);
	KT_ASSERT_ER(obj_words("てすろん\t手須論\t固有名詞\n"), E_OK);
	KT_ASSERT_ER(kc_words_reload(), E_OK);
	kid = kc_open();
	KT_ASSERT(kid > 0);
	if ( kid <= 0 ) {
		Kfree(out);
		return;
	}
	kanji = kc_kanji();
	tm_printf((UB *)"  converter: %s\n", kanji ? "mozc" : "romaji to kana");

	place = rank_of(kid, "tesuron", "手須論", out);
	tm_printf((UB *)"  手須論 for てすろん: %d\n", place);
	KT_ASSERT(place >= 1);
	if ( !kanji ) {
		KT_ASSERT_EQ(place, 1);
	}

	/* committed as the word: the first shown, or chosen from the list */
	(void)type(kid, "tesuron", out);
	(void)kc_key(kid, TSMOZC_K_SPACE, 0, out);
	if ( place == 1 ) {
		r = kc_key(kid, TSMOZC_K_NL, 0, out);
	} else {
		(void)kc_key(kid, TSMOZC_K_SPACE, 0, out);
		r = kc_choose(kid, ( place >= 1 ) ? place : 1, out);
	}
	KT_ASSERT(r >= 0 && ( r & TSMOZC_OUT ) != 0);
	KT_ASSERT(str_is(out->text, "手須論"));
	(void)kc_key(kid, TSMOZC_K_NL, 0, out);

	/* without the romaji converter's own list: the list and choosing from it */
	if ( !kanji ) {
		UB	list[256];
		INT	count = 0, first = 0, total = 0;

		(void)type(kid, "tesuron", out);
		(void)kc_key(kid, TSMOZC_K_SPACE, 0, out);
		r = kc_key(kid, TSMOZC_K_SPACE, 0, out);
		KT_ASSERT(r >= 0 && ( r & TSMOZC_LIST ) != 0);
		KT_ASSERT_EQ(kc_list(kid, list, sizeof(list), &count, &first, &total), 2);
		KT_ASSERT_EQ(count, 3);
		KT_ASSERT_EQ(total, 3);
		KT_ASSERT(str_is((CONST char *)list, "手須論"));
		r = kc_choose(kid, 3, out);
		KT_ASSERT(r >= 0 && ( r & TSMOZC_OUT ) != 0);
		KT_ASSERT(str_is(out->text, "テスロン"));
		/* typing on commits what is shown */
		(void)type(kid, "tesuron", out);
		(void)kc_key(kid, TSMOZC_K_SPACE, 0, out);
		r = kc_key(kid, 'a', 0, out);
		KT_ASSERT(r >= 0 && ( r & TSMOZC_OUT ) != 0);
		KT_ASSERT_EQ(out->n_out, 1);
		KT_ASSERT(str_is(out->text, "手須論あ"));
		(void)kc_key(kid, TSMOZC_K_CAN, 0, out);
	}

	/* a word written to the object is a candidate without being asked for */
	KT_ASSERT_ER(obj_words("てすろん\t手須論\t固有名詞\nわーどてすと\t和亜度試\t名詞\n"), E_OK);
	for ( w = 0; w < 50; w++ ) {
		place = rank_of(kid, "wa-dotesuto", "和亜度試", out);
		if ( place >= 1 ) break;
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  和亜度試 for わーどてすと, %d ms after it was written: %d\n", w * 100, place);
	KT_ASSERT(place >= 1);

	/* taken out: no longer a candidate */
	KT_ASSERT_ER(obj_words("わーどてすと\t和亜度試\t名詞\n"), E_OK);
	KT_ASSERT_ER(kc_words_reload(), E_OK);
	place = rank_of(kid, "tesuron", "手須論", out);
	tm_printf((UB *)"  手須論 once taken out: %d\n", place);
	KT_ASSERT_EQ(place, 0);
	kc_close(kid);
	Kfree(out);
}

/* what the object held, put back */
/*
 * A person logged in has words of their own, starting with those there
 * were, and their object is theirs alone; those of 利用者 stay and come
 * back when they log out.
 */
IMPORT ER kc_words_dict( TS_UUID *u );

LOCAL void test_person( void )
{
	static T_OBCRD	crd;
	static CONST char	mine[] = "こじん\t個人語\t名詞\n";
	TS_UUID		shared, u, box;
	UB		pbuf[OB_PRT_SIZE(16, 4)];
	T_OBPRT		*prt = (T_OBPRT *)pbuf;
	UB		*t = (UB *)Kmalloc(TEXT_MAX);
	SZ		asz = 0;
	ID		k;
	INT		n, i;

	KT_ASSERT(t != NULL);
	if ( t == NULL ) return;
	if ( kept == NULL ) {
		kept = (UB *)Kmalloc(UD_TEXT_MAX);
		kept_len = ( kept != NULL ) ? obj_read(kept, UD_TEXT_MAX) : -1;
	}
	KT_ASSERT_ER(kc_start(), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_DICT_USER, &shared), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_DICT_BOX, &box), E_OK);
	KT_ASSERT_ER(obj_words("きょうつう\t共通語\t名詞\n"), E_OK);
	KT_ASSERT_ER(kc_words_reload(), E_OK);
	KT_ASSERT_ER(kc_words_dict(&u), E_OK);
	KT_ASSERT(u.d.hi == shared.d.hi && u.d.lo == shared.d.lo);

	crd.user.d.hi = 0x0123456789abcdefULL;
	crd.user.d.lo = 0x1032547698badcfeULL;
	crd.ngrp = 1;
	crd.grp[0] = ob_group_users;
	KT_ASSERT_ER(knl_ob_session(&crd), E_OK);
	KT_ASSERT_ER(kc_words_dict(&u), E_OK);
	KT_ASSERT(!( u.d.hi == shared.d.hi && u.d.lo == shared.d.lo ));
	KT_ASSERT_ER(ob_get_prt(&u, prt, sizeof(pbuf), &asz), E_OK);
	KT_ASSERT(prt->owner.d.hi == crd.user.d.hi && prt->owner.d.lo == crd.user.d.lo);
	KT_ASSERT_EQ(prt->mode & 0777, 0600);

	/* the words there were are in it, and writing it leaves the shared one alone */
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		asz = 0;
		KT_ASSERT_ER(ob_rea_rec(k, 1, 0, t, TEXT_MAX, &asz), E_OK);
		KT_ASSERT(same(t, (INT)asz, "きょうつう\t共通語\t名詞\n"));
		KT_ASSERT_ER(ob_rpl_rec(k, 1, mine, (SZ)s_len(mine)), E_OK);
		ob_cls_obj(k);
	}
	n = obj_read(t, TEXT_MAX);
	KT_ASSERT(same(t, n, "きょうつう\t共通語\t名詞\n"));

	/* the same dictionary found again, not another made */
	{
		TS_UUID	again;

		KT_ASSERT_ER(kc_words_dict(&again), E_OK);
		KT_ASSERT(again.d.hi == u.d.hi && again.d.lo == u.d.lo);
	}
	/* the watcher follows who is at the machine, by itself */
	for ( i = 0; i < 30; i++ ) tk_dly_tsk(100);

	KT_ASSERT_ER(knl_ob_session(NULL), E_OK);
	KT_ASSERT_ER(kc_words_dict(&box), E_OK);
	KT_ASSERT(box.d.hi == shared.d.hi && box.d.lo == shared.d.lo);
	for ( i = 0; i < 5; i++ ) tk_dly_tsk(100);

	/* taken away again, as the test found it */
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_DICT_BOX, &box), E_OK);
	KT_ASSERT_ER(om_obj_link_del(&box, &u), E_OK);
	(void)ob_del_obj(&u);
	Kfree(t);
}

LOCAL void test_restore( void )
{
	if ( kept != NULL && kept_len >= 0 ) {
		KT_ASSERT_ER(obj_write(kept, kept_len), E_OK);
		KT_ASSERT_ER(kc_words_reload(), E_OK);
	}
	if ( kept != NULL ) {
		Kfree(kept);
		kept = NULL;
	}
}

EXPORT void ktest_udic( void )
{
	tm_printf((UB *)"\n[udic] ユーザ辞書と単語登録 (design 11.16)\n");
	KT_RUN(test_text);
	KT_RUN(test_import);
	KT_RUN(test_store);
	KT_RUN(test_convert);
	KT_RUN(test_person);
	KT_RUN(test_restore);
}
