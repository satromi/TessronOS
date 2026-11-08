/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kconv.c
 *	かな漢字変換: the converter behind the text input port (ts/kconv.h)
 *
 *	Two converters answer to the same calls. The one always here turns
 *	romaji into kana as it is typed and commits it as it stands. With
 *	USE_MOZC the mozc engine (tools/mozc, ts/tsmozc.h) converts to kanji
 *	as well; it is built apart into one object and linked in, and runs
 *	in a task of its own:
 *
 *	  - it uses the FP/SIMD registers, which the rest of the system does
 *	    not, so the task is created TA_FPU;
 *	  - it wants a deep stack and a large heap, and its start takes a
 *	    while, which the desktop should not wait for.
 *
 *	A call hands its request to that task and waits for the answer. Until
 *	the engine is up, or when it could not be made, the romaji converter
 *	answers instead.
 *
 *	The engine's C library stands on the tsmozc_host_* functions here:
 *	memory from a region of its own, the clock, and files. The files
 *	are objects: the profile directory the engine names is the 学習箱,
 *	and each file in it an object the box links to (design 18.20).
 *
 *	The words the person registered (ユーザ辞書, SYSDEF_DICT_USER,
 *	ts/udic.h) are read when the converter starts and again whenever
 *	their object is written, which 単語登録 does: the romaji converter
 *	offers them for a reading typed whole, and mozc is given them as its
 *	user dictionary (design 11.16). While someone is logged in they are
 *	that person's own (kw_person); else those of 利用者, SYSDEF_DICT_USER.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include <ts/kconv.h>
#include <ts/keymap.h>
#include <ts/hid.h>
#include <ts/udic.h>

/* ---------------------------------------------------------------- the user's words */

/*
 * The user dictionary's text, read whole; kw_sem keeps a look-up from
 * meeting the text being replaced.
 */
LOCAL UB	*kw_text = NULL;
LOCAL INT	kw_len = 0;
LOCAL ID	kw_sem = 0;

LOCAL void kw_lock( void )
{
	if ( kw_sem > 0 ) (void)tk_wai_sem(kw_sem, 1, TMO_FEVR);
}

LOCAL void kw_unlock( void )
{
	if ( kw_sem > 0 ) (void)tk_sig_sem(kw_sem, 1);
}

/*
 * Whose words they are: a person logged in has a dictionary of their own,
 * their object linked from 辞書箱, told by its owner and by what its
 * metadata says it is (tessronos.userdic). The person cannot link from
 * the box, so it is made here the first time they log in. It starts with
 * the words of the dictionary there has been, so that nobody's words are
 * lost by logging in; that one is left as it is, for 利用者.
 */
#define KW_META_ROOM	4096
#define KW_NOTE		"<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>" \
			"単語登録で登録した単語です。持ち主だけが読み書きできます。" \
			"</p>\n</document>\n</tad>\n"

LOCAL BOOL kw_same( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* Whether the n bytes at a and b are the same */
LOCAL BOOL kw_bytes( CONST UB *a, CONST char *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

/* Whether u is user's dictionary */
LOCAL BOOL kw_is_of( CONST TS_UUID *u, CONST TS_UUID *user )
{
	UB	pbuf[OB_PRT_SIZE(16, 4)];
	T_OBPRT	*prt = (T_OBPRT *)pbuf;
	UB	*meta;
	SZ	asz = 0;
	ID	key;
	BOOL	yes = FALSE;

	if ( ob_get_prt(u, prt, sizeof(pbuf), &asz) < E_OK || !kw_same(&prt->owner, user) ) {
		return FALSE;
	}
	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key <= 0 ) {
		return FALSE;
	}
	meta = (UB *)Kmalloc(KW_META_ROOM);
	if ( meta != NULL && ob_get_atr(key, meta, KW_META_ROOM - 1, &asz) >= E_OK ) {
		SZ	i;
		static CONST char	mark[] = "\"userdic\"";
		INT			n = (INT)sizeof(mark) - 1;

		for ( i = 0; !yes && i + n <= asz; i++ ) {
			yes = kw_bytes(meta + i, mark, n);
		}
	}
	if ( meta != NULL ) Kfree(meta);
	(void)ob_cls_obj(key);
	return yes;
}

/* The words the dictionary there has been holds, put in the new one's record */
LOCAL ER kw_copy_words( ID to, INT recno )
{
	TS_UUID	u;
	UB	*t;
	SZ	asz = 0;
	ID	key;
	ER	er = E_OK;

	if ( ts_str_to_uuid(SYSDEF_DICT_USER, &u) < E_OK ) {
		return E_OK;
	}
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) {
		return E_OK;			/* none: it starts with no words */
	}
	t = (UB *)Kmalloc(UD_TEXT_MAX);
	if ( t == NULL ) {
		er = E_NOMEM;
	} else if ( ob_rea_rec(key, 1, 0, t, UD_TEXT_MAX, &asz) >= E_OK && asz > 0 ) {
		SZ	wr = 0;

		er = ob_wri_rec(to, recno, 0, t, ( asz > UD_TEXT_MAX ) ? UD_TEXT_MAX : asz, &wr);
	}
	if ( t != NULL ) Kfree(t);
	(void)ob_cls_obj(key);
	return er;
}

/* user's dictionary made, theirs alone, and linked from 辞書箱 */
LOCAL ER kw_make( CONST T_OBCRD *crd, CONST TS_UUID *box, TS_UUID *p_u )
{
	static CONST char meta[] =
		"{\"name\":\"ユーザ辞書(個人)\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		"\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},"
		"\"tessronos\":{\"userdic\":true,"
		"\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	T_OBPRT	prt;
	T_OBCRE	cr;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	knl_memset(&prt, 0, sizeof(prt));
	prt.owner = crd->user;
	if ( crd->ngrp > 0 ) prt.group = crd->grp[0];
	prt.mode = 0600;
	knl_memset(&cr, 0, sizeof(cr));
	cr.type = OB_T_STORAGE;
	cr.sub = OB_S_FILE;
	cr.json = (CONST UB *)meta;
	cr.jsonsz = (SZ)( sizeof(meta) - 1 );
	cr.near = *box;
	cr.prt = &prt;
	er = ob_cre_obj(&cr, p_u);
	if ( er < E_OK ) {
		return er;
	}
	/* a new object filled record by record: nobody reads it half made */
	key = ob_opn_obj(p_u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
	er = ( key > 0 ) ? E_OK : ( key < 0 ) ? (ER)key : E_SYS;
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, KW_NOTE, (SZ)( sizeof(KW_NOTE) - 1 ), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
	if ( er >= E_OK ) er = kw_copy_words(key, recno);
	if ( key > 0 ) (void)ob_cls_obj(key);
	if ( er >= E_OK ) er = om_obj_link_add(box, p_u);
	if ( er < E_OK ) {
		tm_printf((UB *)"kconv: a person's dictionary not made (%d)\n", (INT)er);
		(void)ob_del_obj(p_u);
	} else {
		tm_printf((UB *)"kconv: a person's dictionary made\n");
	}
	return er;
}

/*
 * The dictionary of whoever is at the machine, into *u. Should anything
 * keep the person's from being found or made, those of 利用者 are used:
 * typing goes on, and the words are not lost.
 */
LOCAL ER kw_person( TS_UUID *u )
{
	CONST T_OBCRD	*crd = knl_ob_session_crd();
	TS_UUID		box, *lk;
	INT		cnt = 0, i;
	ER		er = E_NOEXS;

	/* the watcher and a reader may come here together: one makes it, and the other finds it */
	kw_lock();
	if ( crd != NULL && !kw_same(&crd->user, &ob_user_person)
	  && ts_str_to_uuid(SYSDEF_DICT_BOX, &box) >= E_OK
	  && ob_lst_lnk(&box, NULL, NULL, 0, &cnt) >= E_OK ) {
		lk = ( cnt > 0 ) ? (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)cnt) : NULL;
		if ( lk != NULL ) {
			INT	got = 0;

			if ( ob_lst_lnk(&box, NULL, lk, cnt, &got) >= E_OK ) {
				for ( i = 0; i < got && i < cnt && er < E_OK; i++ ) {
					if ( kw_is_of(&lk[i], &crd->user) ) {
						*u = lk[i];
						er = E_OK;
					}
				}
			}
			Kfree(lk);
		}
		if ( er < E_OK ) {
			er = kw_make(crd, &box, u);
		}
		if ( er >= E_OK ) {
			kw_unlock();
			return E_OK;
		}
	}
	kw_unlock();
	return ts_str_to_uuid(SYSDEF_DICT_USER, u);
}

#ifdef USE_KTEST
/* For a test: whose dictionary is in use now */
EXPORT ER kc_words_dict( TS_UUID *u )
{
	return kw_person(u);
}
#endif

/* The object's record 1, read whole into memory taken for it; NULL when there is no such object */
LOCAL UB *kw_read( INT *p_len )
{
	TS_UUID	u;
	T_OBREC	rec[4];
	UB	*t;
	SZ	size = 0, asz = 0;
	D	at = 0;
	INT	cnt = 0, i;
	ID	key;

	*p_len = 0;
	if ( kw_person(&u) < E_OK ) {
		return NULL;
	}
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) {
		return NULL;
	}
	if ( ob_lst_rec(key, rec, 4, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( rec[i].recno == 1 ) size = (SZ)rec[i].size;
		}
	}
	if ( size > UD_TEXT_MAX ) {
		size = UD_TEXT_MAX;
	}
	t = (UB *)Kmalloc(size + 1);
	while ( t != NULL && at < (D)size ) {
		if ( ob_rea_rec(key, 1, at, t + at, size - at, &asz) < E_OK || asz <= 0 ) break;
		at += asz;
	}
	ob_cls_obj(key);
	if ( t == NULL ) {
		return NULL;
	}
	t[at] = 0;
	*p_len = (INT)at;
	return t;
}

/* The words read again, in place of those there were */
LOCAL void kw_load( void )
{
	UB	*t, *old;
	INT	len = 0;

	t = kw_read(&len);
	if ( t == NULL ) {
		t = (UB *)Kmalloc(1);		/* none: no words */
		if ( t == NULL ) return;
		t[0] = 0;
	}
	kw_lock();
	old = kw_text;
	kw_text = t;
	kw_len = len;
	kw_unlock();
	if ( old != NULL ) {
		Kfree(old);
	}
	tm_printf((UB *)"kconv: user dictionary read (%d words)\n", ud_count(t, len));
}

#if USE_MOZC
/* The words as the engine takes them, in memory taken for them; NULL when there is none */
LOCAL UB *kw_engine( INT *p_n )
{
	UB	*b;
	INT	n;

	kw_lock();
	b = (UB *)Kmalloc(kw_len + 16);
	n = ( b != NULL ) ? ud_engine(( kw_text != NULL ) ? kw_text : (CONST UB *)"", kw_len, b, kw_len + 16) : 0;
	kw_unlock();
	if ( b != NULL && n < 0 ) {
		Kfree(b);
		b = NULL;
	}
	*p_n = n;
	return b;
}
#endif

/* ---------------------------------------------------------------- the romaji converter */

#define RK_SESS		8
#define RK_TEXT		256
#define RK_CAND		9		/* a reading's candidates: the list shows nine */
#define RK_CANDBUF	( RK_CAND * ( UD_WORD_MAX + 4 ) )

typedef struct {
	BOOL	used;
	UB	kana[RK_TEXT];		/* the kana made so far */
	INT	nk;
	UB	roma[4];		/* romaji not yet a kana */
	INT	nr;
	BOOL	conv;			/* converted: a candidate is shown for the kana */
	BOOL	listed;			/* ... and the list of them is shown */
	INT	ncand, sel;
	INT	coff[RK_CAND + 1];	/* where each candidate starts in cand */
	UB	cand[RK_CANDBUF];
} RKSESS;

LOCAL RKSESS	rk_sess[RK_SESS];

typedef struct {
	CONST char	*roma;
	CONST char	*kana;
} ROMA;

LOCAL CONST ROMA rk_tab[] = {
	{ "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
	{ "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
	{ "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
	{ "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
	{ "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
	{ "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
	{ "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
	{ "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
	{ "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
	{ "wa", "わ" }, { "wo", "を" }, { "nn", "ん" }, { "n'", "ん" },
	{ "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
	{ "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
	{ "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
	{ "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
	{ "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
	{ "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
	{ "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" },
	{ "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" },
	{ "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" },
	{ "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" },
	{ "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" },
	{ "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
	{ "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
	{ "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
	{ "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
	{ "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" },
	{ "zya", "じゃ" }, { "zyu", "じゅ" }, { "zyo", "じょ" },
	{ "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" },
	{ "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
	{ "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
	{ "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
	{ "xtu", "っ" }, { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" },
	{ "-", "ー" }, { ",", "、" }, { ".", "。" }, { "[", "「" }, { "]", "」" },
};

LOCAL BOOL rk_vowel( UB c )
{
	return (BOOL)( c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o' );
}

/*
 * The romaji typed so far looked up: 2 when it is a whole entry (the
 * kana given), 1 when an entry starts with it, 0 when none does.
 */
LOCAL INT rk_look( CONST UB *r, INT n, CONST char **p_kana )
{
	UINT	i;
	INT	k;
	BOOL	prefix = FALSE;

	for ( i = 0; i < sizeof(rk_tab) / sizeof(rk_tab[0]); i++ ) {
		CONST char	*e = rk_tab[i].roma;

		for ( k = 0; k < n && e[k] != 0 && e[k] == (char)r[k]; k++ ) {
			;
		}
		if ( k == n ) {
			if ( e[k] == 0 ) {
				*p_kana = rk_tab[i].kana;
				return 2;
			}
			prefix = TRUE;
		}
	}
	return prefix ? 1 : 0;
}

LOCAL void rk_put( RKSESS *s, CONST char *t )
{
	INT	i;

	for ( i = 0; t[i] != 0 && s->nk < RK_TEXT - 8; i++ ) {
		s->kana[s->nk++] = (UB)t[i];
	}
}

/* The romaji waiting, settled as it stands: a lone n is ん */
LOCAL void rk_settle( RKSESS *s )
{
	INT	k;

	if ( s->nr == 1 && s->roma[0] == 'n' ) {
		rk_put(s, "ん");
	} else {
		for ( k = 0; k < s->nr; k++ ) {
			UB	one[2] = { s->roma[k], 0 };

			rk_put(s, (CONST char *)one);
		}
	}
	s->nr = 0;
}

/* A letter typed: the romaji waiting turned into kana as far as it goes */
LOCAL void rk_letter( RKSESS *s, UB c )
{
	CONST char	*kana = NULL;
	INT		m, k;

	if ( s->nr >= (INT)sizeof(s->roma) ) {
		rk_settle(s);
	}
	s->roma[s->nr++] = c;
	for ( ;; ) {
		m = rk_look(s->roma, s->nr, &kana);
		if ( m == 2 ) {
			rk_put(s, kana);
			s->nr = 0;
			return;
		}
		if ( m == 1 ) {
			return;			/* more to come */
		}
		/* the first letter settles: ん, っ, or itself */
		if ( s->nr >= 2 && s->roma[0] == 'n' && !rk_vowel(s->roma[1])
		  && s->roma[1] != 'y' ) {
			rk_put(s, "ん");
		} else if ( s->nr >= 2 && s->roma[0] == s->roma[1]
			 && !rk_vowel(s->roma[0]) ) {
			rk_put(s, "っ");
		} else {
			UB	one[2] = { s->roma[0], 0 };

			rk_put(s, (CONST char *)one);
		}
		for ( k = 0; k + 1 < s->nr; k++ ) {
			s->roma[k] = s->roma[k + 1];
		}
		s->nr--;
		if ( s->nr == 0 ) {
			return;
		}
	}
}

/* The composition, as the port takes it: one clause, the caret at its end */
LOCAL void rk_out( RKSESS *s, T_KCOUT *out, CONST UB *committed, INT nc )
{
	INT	at = 0, i;

	knl_memset(out, 0, sizeof(*out));
	if ( nc > 0 ) {
		for ( i = 0; i < nc && at < TSMOZC_TEXT_MAX - 1; i++ ) {
			out->text[at++] = (char)committed[i];
		}
		out->n_out = 1;
		out->n_cl = 1;
		out->cl[1] = at;
	}
	if ( s->conv ) {
		/* the candidate chosen, standing for the kana */
		out->cl[out->n_cl] = at;
		for ( i = s->coff[s->sel]; i < s->coff[s->sel + 1] && at < TSMOZC_TEXT_MAX - 1; i++ ) {
			out->text[at++] = (char)s->cand[i];
		}
		out->n_cl++;
		out->cl[out->n_cl] = at;
		out->clause = out->n_cl - 1;
		out->sel = s->sel + 1;
	} else if ( s->nk + s->nr > 0 ) {
		out->cl[out->n_cl] = at;
		for ( i = 0; i < s->nk && at < TSMOZC_TEXT_MAX - 1; i++ ) {
			out->text[at++] = (char)s->kana[i];
		}
		for ( i = 0; i < s->nr && at < TSMOZC_TEXT_MAX - 1; i++ ) {
			out->text[at++] = (char)s->roma[i];
		}
		out->n_cl++;
		out->cl[out->n_cl] = at;
		out->clause = out->n_cl - 1;
	}
	out->text[at] = 0;
	out->caret = at;
}

/* Hiragana in n bytes of UTF-8 turned into katakana where they stand */
LOCAL void to_katakana( UB *p, INT n )
{
	INT	i;

	for ( i = 0; i + 2 < n; i++ ) {
		UB	a = p[i], b = p[i + 1], c = p[i + 2];
		UINT	u;

		if ( ( a & 0xF0 ) != 0xE0 ) {
			continue;
		}
		u = ( (UINT)( a & 0x0F ) << 12 ) | ( (UINT)( b & 0x3F ) << 6 ) | ( c & 0x3F );
		if ( u >= 0x3041 && u <= 0x3096 ) {
			u += 0x60;
			p[i] = (UB)( 0xE0 | ( u >> 12 ) );
			p[i + 1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			p[i + 2] = (UB)( 0x80 | ( u & 0x3F ) );
		}
		i += 2;
	}
}

/* The composition's hiragana from byte from on turned into katakana */
LOCAL void rk_katakana( RKSESS *s, INT from )
{
	to_katakana(s->kana + from, s->nk - from);
}

/* A candidate added, unless it is one already there */
LOCAL void rk_cand_add( RKSESS *s, CONST UB *w, INT n )
{
	INT	k, i, at = s->coff[s->ncand];

	if ( s->ncand >= RK_CAND || n <= 0 || at + n > RK_CANDBUF ) {
		return;
	}
	for ( k = 0; k < s->ncand; k++ ) {
		if ( s->coff[k + 1] - s->coff[k] != n ) continue;
		for ( i = 0; i < n && s->cand[s->coff[k] + i] == w[i]; i++ ) ;
		if ( i == n ) return;
	}
	for ( i = 0; i < n; i++ ) s->cand[at + i] = w[i];
	s->ncand++;
	s->coff[s->ncand] = at + n;
}

/*
 * The kana composed looked up in the user's words: when there are any,
 * the candidates are they, the kana in hiragana and in katakana, the
 * first shown, and TRUE.
 */
LOCAL BOOL rk_convert( RKSESS *s )
{
	UDENT	e[RK_CAND];
	UB	y[RK_TEXT];
	INT	n, i;

	if ( s->nk <= 0 || s->nk >= UD_YOMI_MAX ) {
		return FALSE;
	}
	for ( i = 0; i < s->nk; i++ ) y[i] = s->kana[i];
	ud_hira(y, s->nk);
	s->ncand = 0;
	s->coff[0] = 0;
	kw_lock();
	n = ( kw_text != NULL ) ? ud_lookup(kw_text, kw_len, y, s->nk, e, RK_CAND - 2) : 0;
	if ( n > RK_CAND - 2 ) n = RK_CAND - 2;
	for ( i = 0; i < n; i++ ) {
		rk_cand_add(s, e[i].word, e[i].nword);	/* copied while the text cannot change */
	}
	kw_unlock();
	if ( s->ncand == 0 ) {
		return FALSE;
	}
	rk_cand_add(s, y, s->nk);
	to_katakana(y, s->nk);
	rk_cand_add(s, y, s->nk);
	s->conv = TRUE;
	s->listed = FALSE;
	s->sel = 0;
	return TRUE;
}

/* The candidate shown, committed: into done (n bytes there), the composition emptied */
LOCAL INT rk_fix( RKSESS *s, UB *done )
{
	INT	n = 0, i;

	if ( s->conv ) {
		for ( i = s->coff[s->sel]; i < s->coff[s->sel + 1]; i++ ) done[n++] = s->cand[i];
		s->conv = FALSE;
		s->nk = s->nr = 0;
	}
	return n;
}

/* A character of the BMP put as UTF-8 */
LOCAL void rk_put_u( RKSESS *s, UINT u )
{
	UB	t[4];

	if ( u < 0x80 ) {
		t[0] = (UB)u;  t[1] = 0;
	} else if ( u < 0x800 ) {
		t[0] = (UB)( 0xC0 | ( u >> 6 ) );
		t[1] = (UB)( 0x80 | ( u & 0x3F ) );  t[2] = 0;
	} else {
		t[0] = (UB)( 0xE0 | ( u >> 12 ) );
		t[1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		t[2] = (UB)( 0x80 | ( u & 0x3F ) );  t[3] = 0;
	}
	rk_put(s, (CONST char *)t);
}

/* A kana key's TRON code as its character, or 0 for one that is not */
LOCAL UINT rk_tc_ucs( UINT tc )
{
	LOCAL CONST UH	sign[][2] = {
		{ 0x213C, 0x30FC },	/* ー */
		{ 0x212B, 0x309B },	/* ゛ */
		{ 0x212C, 0x309C },	/* ゜ */
		{ 0x2156, 0x300C },	/* 「 */
		{ 0x2157, 0x300D },	/* 」 */
		{ 0x2122, 0x3001 },	/* 、 */
		{ 0x2123, 0x3002 },	/* 。 */
		{ 0x2126, 0x30FB },	/* ・ */
	};
	UINT	i;

	if ( tc >= 0x2421 && tc <= 0x2473 ) {
		return 0x3041 + ( tc - 0x2421 );
	}
	if ( tc >= 0x2521 && tc <= 0x2576 ) {
		return 0x30A1 + ( tc - 0x2521 );
	}
	for ( i = 0; i < sizeof(sign) / sizeof(sign[0]); i++ ) {
		if ( sign[i][0] == tc ) {
			return sign[i][1];
		}
	}
	return 0;
}

/*
 * A voiced sound mark (゛, or ゜ when half) typed after a kana: the kana
 * before it voiced when it can be, TRUE then.
 */
LOCAL BOOL rk_voice( RKSESS *s, BOOL half )
{
	UB	*p;
	UINT	u, h, to = 0;

	if ( s->nk < 3 ) {
		return FALSE;
	}
	p = &s->kana[s->nk - 3];
	if ( ( p[0] & 0xF0 ) != 0xE0 ) {
		return FALSE;
	}
	u = ( (UINT)( p[0] & 0x0F ) << 12 ) | ( (UINT)( p[1] & 0x3F ) << 6 ) | ( p[2] & 0x3F );
	h = ( u >= 0x30A1 && u <= 0x30F6 ) ? u - 0x60 : u;	/* as hiragana */
	if ( h >= 0x306F && h <= 0x307B && ( h - 0x306F ) % 3 == 0 ) {
		to = h + ( half ? 2 : 1 );			/* は行 */
	} else if ( !half && h >= 0x304B && h <= 0x3061 && ( h - 0x304B ) % 2 == 0 ) {
		to = h + 1;					/* か行 to ち */
	} else if ( !half && h >= 0x3064 && h <= 0x3068 && ( h - 0x3064 ) % 2 == 0 ) {
		to = h + 1;					/* つ, て, と */
	} else if ( !half && h == 0x3046 ) {
		to = 0x3094;					/* ゔ */
	}
	if ( to == 0 ) {
		return FALSE;
	}
	s->nk -= 3;
	rk_put_u(s, ( u != h ) ? to + 0x60 : to);
	return TRUE;
}

LOCAL INT rk_open( void )
{
	INT	i;

	for ( i = 0; i < RK_SESS; i++ ) {
		if ( !rk_sess[i].used ) {
			knl_memset(&rk_sess[i], 0, sizeof(rk_sess[i]));
			rk_sess[i].used = TRUE;
			return i + 1;
		}
	}
	return E_LIMIT;
}

LOCAL RKSESS *rk_of( INT kid )
{
	return ( kid >= 1 && kid <= RK_SESS && rk_sess[kid - 1].used )
	       ? &rk_sess[kid - 1] : NULL;
}

/*
 * A key while a candidate is shown: the flags of what it did to the
 * conversion, or -1 for a key that types on, before which the
 * candidate is committed.
 */
LOCAL INT rk_conv_key( RKSESS *s, UINT code, T_KCOUT *out )
{
	UB	done[UD_WORD_MAX + 4];
	INT	n;

	switch ( code ) {
	case TSMOZC_K_SPACE:
	case TSMOZC_K_CNV:
	case TSMOZC_K_DOWN:
	case TSMOZC_K_SC_D:
	case TSMOZC_K_UP:
	case TSMOZC_K_SC_U: {
		/* the next candidate, and from the second conversion on the list of them */
		BOOL	was = s->listed;

		if ( code == TSMOZC_K_UP || code == TSMOZC_K_SC_U ) {
			s->sel = ( s->sel + s->ncand - 1 ) % s->ncand;
		} else {
			s->sel = ( s->sel + 1 ) % s->ncand;
		}
		s->listed = TRUE;
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_LIST | ( was ? 0 : TSMOZC_LISTCHG );
	}
	case TSMOZC_K_CR:
	case TSMOZC_K_NL:
	case TSMOZC_K_IEND:
		n = rk_fix(s, done);
		rk_out(s, out, done, n);
		return TSMOZC_OUT | TSMOZC_CNV;
	case TSMOZC_K_BS:
	case TSMOZC_K_ESC:
	case TSMOZC_K_CAN:
		/* back to the kana */
		s->conv = FALSE;
		s->listed = FALSE;
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_CAR;
	case TSMOZC_K_LIST_PREV:
	case TSMOZC_K_LIST_NEXT:
	case TSMOZC_K_PG_U:
	case TSMOZC_K_PG_D:
		rk_out(s, out, NULL, 0);
		return s->listed ? TSMOZC_LIST : 0;	/* one page holds them all */
	default:
		return -1;
	}
}

LOCAL INT rk_key( INT kid, UINT code, UINT stat, T_KCOUT *out )
{
	RKSESS	*s = rk_of(kid);
	BOOL	busy, kata = (BOOL)( ( stat & TSMOZC_S_KANA ) != 0 );
	UB	done[UD_WORD_MAX + 4];
	INT	was, nd = 0, r;
	UINT	u, also;

	if ( s == NULL ) {
		return E_ID;
	}
	if ( s->conv ) {
		r = rk_conv_key(s, code, out);
		if ( r >= 0 ) {
			return r;
		}
		nd = rk_fix(s, done);			/* typing on: what was shown is committed */
	}
	also = ( nd > 0 ) ? TSMOZC_OUT : 0;
	busy = (BOOL)( s->nk + s->nr > 0 );
	was = s->nk;
	if ( ( code >= 'a' && code <= 'z' ) || code == '-' || code == ','
	  || code == '.' || code == '[' || code == ']' || code == '\'' ) {
		rk_letter(s, (UB)code);
		if ( kata ) {
			rk_katakana(s, was);
		}
		rk_out(s, out, done, nd);
		return TSMOZC_CNV | TSMOZC_CAR | also;
	}
	if ( code > 0x20 && code < 0x7F ) {
		UB	one[2] = { (UB)code, 0 };

		rk_settle(s);
		rk_put(s, (CONST char *)one);
		rk_out(s, out, done, nd);
		return TSMOZC_CNV | TSMOZC_CAR | also;
	}
	/* a kana key: the kana itself, or the mark voicing the one before */
	u = ( code != TSMOZC_K_SPACE ) ? rk_tc_ucs(code) : 0;
	if ( u != 0 ) {
		rk_settle(s);
		was = s->nk;
		if ( ( u != 0x309B && u != 0x309C ) || !rk_voice(s, (BOOL)( u == 0x309C )) ) {
			rk_put_u(s, u);
		}
		if ( kata ) {
			rk_katakana(s, was);
		}
		rk_out(s, out, done, nd);
		return TSMOZC_CNV | TSMOZC_CAR | also;
	}
	if ( !busy ) {
		if ( nd > 0 ) {
			rk_out(s, out, done, nd);
			return TSMOZC_OUT | TSMOZC_CNV;
		}
		return KC_NOTMINE;
	}
	switch ( code ) {
	case TSMOZC_K_SPACE:
	case TSMOZC_K_CNV:
		/* the user's words for the reading, when there are any */
		rk_settle(s);
		if ( rk_convert(s) ) {
			rk_out(s, out, NULL, 0);
			return TSMOZC_CNV | TSMOZC_CL | TSMOZC_CAR;
		}
		/* there is nothing to convert it to: committed as below */
		/* fall through */
	case TSMOZC_K_CR:
	case TSMOZC_K_NL:
	case TSMOZC_K_IEND: {
		/* committed as it stands */
		UB	all[RK_TEXT + 4];
		INT	n = 0, i;

		rk_settle(s);
		for ( i = 0; i < s->nk; i++ ) all[n++] = s->kana[i];
		s->nk = 0;
		rk_out(s, out, all, n);
		return TSMOZC_OUT | TSMOZC_CNV;
	}
	case TSMOZC_K_BS:
		if ( s->nr > 0 ) {
			s->nr--;
		} else if ( s->nk > 0 ) {
			do {
				s->nk--;
			} while ( s->nk > 0 && ( s->kana[s->nk] & 0xC0 ) == 0x80 );
		}
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_CAR;
	case TSMOZC_K_ESC:
	case TSMOZC_K_CAN:
		s->nk = s->nr = 0;
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV;
	case TSMOZC_K_KATA:
		rk_settle(s);
		rk_katakana(s, 0);
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV;
	default:
		rk_out(s, out, NULL, 0);
		return 0;			/* taken, and nothing done */
	}
}

/* The candidates shown, as kc_list gives them; the one chosen (from 1), or E_NOEXS */
LOCAL INT rk_list( INT kid, UB *buf, INT size, INT *p_count, INT *p_first, INT *p_total )
{
	RKSESS	*s = rk_of(kid);
	INT	k, i, at = 0, n = 0;

	if ( s == NULL || !s->conv || buf == NULL || size <= 0 ) {
		return E_NOEXS;
	}
	for ( k = 0; k < s->ncand; k++ ) {
		INT	len = s->coff[k + 1] - s->coff[k];

		if ( at + len + 1 > size ) break;
		for ( i = 0; i < len; i++ ) buf[at++] = s->cand[s->coff[k] + i];
		buf[at++] = 0;
		n++;
	}
	if ( p_count != NULL ) *p_count = n;
	if ( p_first != NULL ) *p_first = 1;
	if ( p_total != NULL ) *p_total = s->ncand;
	return s->sel + 1;
}

/* Candidate n (from 1) chosen and committed */
LOCAL INT rk_choose( INT kid, INT n, T_KCOUT *out )
{
	RKSESS	*s = rk_of(kid);
	UB	done[UD_WORD_MAX + 4];
	INT	nd;

	if ( s == NULL || !s->conv ) {
		return KC_NOTMINE;
	}
	if ( n < 1 || n > s->ncand ) {
		return E_PAR;
	}
	s->sel = n - 1;
	nd = rk_fix(s, done);
	rk_out(s, out, done, nd);
	return TSMOZC_OUT | TSMOZC_CNV;
}

/* ---------------------------------------------------------------- mozc */

#if USE_MOZC

/*
 * The engine's task. A request is put in kc_req and the task woken; it
 * answers in the request and wakes the caller. Callers take turns.
 */
#define KC_STACK	( 8 * 1024 * 1024 )
#define KC_HEAP_MB	256

typedef struct {
	INT		op;
	INT		kid;
	UINT		code, stat;
	INT		n;
	T_KCOUT		*out;
	UB		*buf;
	INT		size;
	INT		*p1, *p2, *p3;
	INT		ret;
	volatile BOOL	done;		/* the engine has answered this one */
} KCREQ;

#define OP_OPEN		1
#define OP_CLOSE	2
#define OP_KEY		3
#define OP_CHOOSE	4
#define OP_LIST		5
#define OP_INPUT	6
#define OP_WORDS	7

LOCAL ID	kc_task = 0;
LOCAL ID	kc_go, kc_done;		/* semaphores */
LOCAL ID	kc_turn;		/* a mutex: one request at a time */
LOCAL KCREQ	*kc_req;
LOCAL volatile BOOL	kc_up = FALSE;		/* the engine is made and running */
LOCAL volatile BOOL	kc_gone = FALSE;	/* it could not be, or it stopped */

IMPORT void (*__tsmozc_init_start[])( void );
IMPORT void (*__tsmozc_init_end[])( void );

#define KC_DICT_CHUNK	( 1024 * 1024 )

/*
 * The dictionary: record 1 of the dictionary object (design 18.20), read
 * whole into pages of its own that stay while the engine runs. NULL when
 * the object is not there or cannot be read.
 */
LOCAL UB *kc_dict( SZ *p_size )
{
	TS_UUID	u;
	T_OBREC	rec[4];
	UB	*d = NULL;
	SZ	size = 0, asz = 0;
	D	at = 0;
	INT	cnt = 0, i;
	ID	key;
	ER	er = E_OK;

	if ( ts_str_to_uuid(SYSDEF_DICT_MOZC, &u) < E_OK ) {
		return NULL;
	}
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) {
		return NULL;
	}
	if ( ob_lst_rec(key, rec, 4, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( rec[i].recno == 1 ) size = (SZ)rec[i].size;
		}
	}
	if ( size > 0 ) {
		d = (UB *)knl_vmap(( size + 4095 ) / 4096, 0);
	}
	while ( d != NULL && at < (D)size && er >= E_OK ) {
		er = ob_rea_rec(key, 1, at, d + at,
				( size - at < KC_DICT_CHUNK ) ? size - at : KC_DICT_CHUNK, &asz);
		if ( er >= E_OK && asz <= 0 ) er = E_IO;
		at += asz;
	}
	ob_cls_obj(key);
	if ( d == NULL || er < E_OK ) {
		return NULL;				/* the pages stay the engine's */
	}
	*p_size = size;
	return d;
}

LOCAL void kc_serve( INT stacd, void *exinf )
{
	void	(**f)( void );
	UB	*dict;
	SZ	dsize = 0;

	/* the engine's static constructors, then the engine */
	for ( f = __tsmozc_init_start; f < __tsmozc_init_end; f++ ) {
		(*f)();
	}
	dict = kc_dict(&dsize);
	if ( dict == NULL ) {
		tm_printf((UB *)"kconv: no dictionary for mozc\n");
		kc_gone = TRUE;
		tk_ext_tsk();
	}
	if ( tsmozc_start(dict, (unsigned long)dsize) < 0 ) {
		tm_printf((UB *)"kconv: mozc could not be started\n");
		kc_gone = TRUE;
		tk_ext_tsk();
	}
	tm_printf((UB *)"kconv: mozc started (%s)\n", tsmozc_note());
	/* the user's words, as its user dictionary */
	{
		UB	*w;
		INT	n = 0;

		if ( kw_text == NULL ) {
			kw_load();
		}
		w = kw_engine(&n);
		if ( w != NULL ) {
			if ( tsmozc_words((CONST char *)w, (unsigned long)n) < 0 ) {
				tm_printf((UB *)"kconv: mozc did not take the user dictionary\n");
			}
			Kfree(w);
		}
	}
	kc_up = TRUE;

	for ( ;; ) {
		KCREQ	*r;

		if ( tk_wai_sem(kc_go, 1, TMO_FEVR) < E_OK ) {
			continue;
		}
		r = kc_req;
		switch ( r->op ) {
		case OP_OPEN:
			r->ret = tsmozc_open(TSMOZC_M_ROMAN);
			break;
		case OP_CLOSE:
			tsmozc_close(r->kid);
			r->ret = E_OK;
			break;
		case OP_KEY:
			r->ret = tsmozc_key(r->kid, r->code, r->stat, r->out);
			break;
		case OP_CHOOSE:
			r->ret = tsmozc_choose(r->kid, r->n, r->out);
			break;
		case OP_INPUT:
			r->ret = ( tsmozc_input(r->kid, r->n) < 0 ) ? E_PAR : E_OK;
			break;
		case OP_LIST:
			r->ret = tsmozc_list(r->kid, (char *)r->buf, r->size,
					     r->p1, r->p2, r->p3);
			break;
		case OP_WORDS:
			r->ret = ( tsmozc_words((CONST char *)r->buf, (unsigned long)r->size) < 0 ) ? E_IO : E_OK;
			break;
		default:
			r->ret = E_PAR;
			break;
		}
		r->done = TRUE;
		tk_sig_sem(kc_done, 1);
	}
}

/*
 * A request done by the engine's task: its answer. The turn is a mutex,
 * so that a caller stopped while it waits (the desktop's task, stopped
 * by 強制終了 to start the desktop again) does not keep it; the answer
 * to such a caller's request may still come, and is told from the one
 * asked for now by the request's own mark.
 */
LOCAL INT kc_call( KCREQ *r )
{
	if ( !kc_up ) {
		return E_NOEXS;
	}
	if ( tk_loc_mtx(kc_turn, TMO_FEVR) < E_OK ) {
		return E_SYS;
	}
	r->done = FALSE;
	kc_req = r;
	tk_sig_sem(kc_go, 1);
	while ( !r->done ) {
		if ( tk_wai_sem(kc_done, 1, 10000) < E_OK ) {
			/* the engine stopped in the middle: it is not asked again */
			kc_up = FALSE;
			kc_gone = TRUE;
			tm_printf((UB *)"kconv: mozc did not answer\n");
			tk_unl_mtx(kc_turn);
			return E_TMOUT;
		}
	}
	tk_unl_mtx(kc_turn);

	return r->ret;
}

/* ---------------------------------------------------------------- what the engine asks of the system */

LOCAL UB	*kc_heap = NULL;
LOCAL SZ	kc_heap_used = 0;

EXPORT void *tsmozc_host_sbrk( long incr )
{
	SZ	size = (SZ)KC_HEAP_MB * 1024 * 1024;
	void	*p;

	if ( kc_heap == NULL ) {
		kc_heap = (UB *)knl_vmap(size / 4096, 0);
		if ( kc_heap == NULL ) {
			tm_printf((UB *)"kconv: no heap for mozc\n");
			return (void *)-1;
		}
	}
	if ( incr < 0 ? (SZ)-incr > kc_heap_used : kc_heap_used + (SZ)incr > size ) {
		tm_printf((UB *)"kconv: mozc's heap is full\n");
		return (void *)-1;
	}
	p = kc_heap + kc_heap_used;
	kc_heap_used += (SZ)incr;

	return p;
}

EXPORT long long tsmozc_host_clock( int monotonic )
{
	SYSTIM	t;

	if ( monotonic ) {
		(void)tk_get_otm(&t);
	} else {
		(void)tk_get_tim(&t);
	}
	return ( ( (long long)t.hi << 32 ) | t.lo ) * 1000000LL;
}

EXPORT void tsmozc_host_sleep( long long ns )
{
	long long ms = ns / 1000000LL;

	tk_dly_tsk(( ms > 0 ) ? (TMO)ms : 1);
}

EXPORT void tsmozc_host_log( const char *s, int len )
{
	UB	line[160];
	INT	i, k = 0;

	for ( i = 0; i < len; i++ ) {
		line[k++] = (UB)s[i];
		if ( k == (INT)sizeof(line) - 1 || s[i] == '\n' ) {
			line[k] = 0;
			tm_putstring(line);
			k = 0;
		}
	}
	if ( k > 0 ) {
		line[k] = 0;
		tm_putstring(line);
	}
}

EXPORT void tsmozc_host_exit( int code )
{
	tm_printf((UB *)"kconv: mozc stopped (%d)\n", code);
	kc_up = FALSE;
	kc_gone = TRUE;
	if ( kc_req != NULL ) {
		kc_req->ret = E_SYS;
		kc_req->done = TRUE;
		tk_sig_sem(kc_done, 1);
	}
	tk_ext_tsk();
}

/*
 * The engine's files. The profile directory it names (/SYS/DIC/mozcprof,
 * or /SYS/WORK/mozcprof when that cannot be made) is the 学習箱, and a
 * file in it is the object the box links to that was taken in under
 * that name ("tessronos.file.name"), its bytes in record 1. A file made
 * by the engine is taken in empty by the common module; one it renames
 * is renamed as a file (xf_rename); one it removes loses its link and
 * is deleted. Nothing outside the directory is a file to the engine.
 * An open file is a key to the object and a place in its record.
 *
 * The engine writes a file through its C++ stream, a kilobyte at a
 * time: a learning file of 320KB made at the first start would be some
 * three hundred writes of the record, each a change of the volume of its
 * own that waits for the medium. What it writes one piece after another
 * is gathered here and written as one (KC_WBUF at most); the gathered
 * bytes go to the record before anything reads the file, asks its size,
 * moves it or closes it.
 */
#define KC_FD_MAX	8
#define KC_WBUF		( 256 * 1024 )

typedef struct {
	ID	key;		/* 0: free */
	D	at;
	BOOL	append;
	UB	*wbuf;		/* bytes written and not yet in the record */
	D	wat;		/* where in the record they go */
	SZ	wlen;
	BOOL	werr;		/* gathered bytes could not be written */
} KCFD;

LOCAL KCFD	kc_fd[KC_FD_MAX];

/* The gathered bytes of an open file into its record */
LOCAL ER kc_wflush( KCFD *f )
{
	SZ	asz = 0;
	ER	er;

	if ( f->wlen <= 0 ) {
		return E_OK;
	}
	er = ob_wri_rec(f->key, 1, f->wat, f->wbuf, f->wlen, &asz);
	if ( er >= E_OK && asz != f->wlen ) {
		er = E_IO;
	}
	f->wlen = 0;
	if ( er < E_OK ) {
		f->werr = TRUE;
	}
	return er;
}

/* Every open file's gathered bytes, before a file is looked up, moved or removed by name */
LOCAL void kc_wflush_all( void )
{
	INT	i;

	for ( i = 0; i < KC_FD_MAX; i++ ) {
		if ( kc_fd[i].key > 0 ) {
			(void)kc_wflush(&kc_fd[i]);
		}
	}
}

/* The name of a file in the profile directory; NULL for the directory itself or outside it */
LOCAL CONST UB *kc_file( CONST char *path, BOOL *p_dir )
{
	LOCAL CONST char *CONST names[] = { "/SYS/DIC/mozcprof", "/SYS/WORK/mozcprof" };
	UINT	i;
	INT	k;

	*p_dir = FALSE;
	for ( i = 0; i < sizeof(names) / sizeof(names[0]); i++ ) {
		for ( k = 0; names[i][k] != 0 && names[i][k] == path[k]; k++ ) {
			;
		}
		if ( names[i][k] != 0 ) {
			continue;
		}
		if ( path[k] == 0 || ( path[k] == '/' && path[k + 1] == 0 ) ) {
			*p_dir = TRUE;
			return NULL;
		}
		return ( path[k] == '/' ) ? (CONST UB *)path + k + 1 : NULL;
	}
	return NULL;
}

LOCAL ER kc_box( TS_UUID *box )
{
	return ob_fnd_sys(SYSROLE_LEARN, SYSDEF_LEARN_BOX, box);
}

LOCAL INT kc_nothing( void *ctx, void *buf, SZ n )
{
	(void)ctx;
	(void)buf;
	(void)n;
	return 0;
}

/* A new, empty file: an object taken in into the box */
LOCAL ER kc_make( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid )
{
	T_XFSRC	src;

	knl_memset(&src, 0, sizeof(src));
	src.filename = name;
	src.source = "mozc";
	src.read = kc_nothing;
	return xf_import(&src, box, p_uuid);
}

/* The size of an open file's record */
LOCAL D kc_size( ID key )
{
	T_OBREC	r[4];
	INT	cnt = 0, i;

	if ( ob_lst_rec(key, r, 4, &cnt) < E_OK ) {
		return 0;
	}
	for ( i = 0; i < cnt; i++ ) {
		if ( r[i].recno == 1 ) return (D)r[i].size;
	}
	return 0;
}

LOCAL KCFD *kc_of( int fd )
{
	return ( fd >= 0 && fd < KC_FD_MAX && kc_fd[fd].key > 0 ) ? &kc_fd[fd] : NULL;
}

EXPORT int tsmozc_host_open( const char *path, int flags )
{
	CONST UB	*name;
	TS_UUID		box, u;
	BOOL		dir;
	ID		key;
	INT		i;

	name = kc_file(path, &dir);
	if ( name == NULL || kc_box(&box) < E_OK ) {
		return -1;
	}
	for ( i = 0; i < KC_FD_MAX && kc_fd[i].key > 0; i++ ) ;
	if ( i >= KC_FD_MAX ) {
		return -1;
	}
	if ( xf_find(&box, name, &u) >= E_OK ) {
		if ( ( flags & TSMOZC_O_CREATE ) && ( flags & TSMOZC_O_EXCL ) ) return -1;
	} else if ( !( flags & TSMOZC_O_CREATE ) || kc_make(&box, name, &u) < E_OK ) {
		return -1;
	}
	key = ob_opn_obj(&u, ( flags & TSMOZC_O_WRITE ) ? ( OB_OP_R | OB_OP_WRITE ) : OB_OP_R);
	if ( key <= 0 ) {
		return -1;
	}
	if ( ( flags & TSMOZC_O_TRUNC ) && ( flags & TSMOZC_O_WRITE ) ) {
		(void)ob_trn_rec(key, 1, 0);
	}
	kc_fd[i].key = key;
	kc_fd[i].at = 0;
	kc_fd[i].append = (BOOL)( ( flags & TSMOZC_O_APPEND ) != 0 );
	kc_fd[i].wlen = 0;
	kc_fd[i].werr = FALSE;
	return i;
}

EXPORT int tsmozc_host_close( int fd )
{
	KCFD	*f = kc_of(fd);
	BOOL	bad;

	if ( f == NULL ) {
		return -1;
	}
	(void)kc_wflush(f);
	bad = f->werr;
	ob_cls_obj(f->key);
	f->key = 0;
	if ( f->wbuf != NULL ) {
		Kfree(f->wbuf);
		f->wbuf = NULL;
	}
	return bad ? -1 : 0;
}

EXPORT int tsmozc_host_read( int fd, void *buf, int len )
{
	KCFD	*f = kc_of(fd);
	SZ	asz = 0;
	ER	er;

	if ( f == NULL || len < 0 ) {
		return -1;
	}
	if ( kc_wflush(f) < E_OK ) {
		return -1;
	}
	er = ob_rea_rec(f->key, 1, f->at, buf, (SZ)len, &asz);
	if ( er < E_OK ) {
		return ( f->at >= kc_size(f->key) ) ? 0 : -1;	/* read past the end */
	}
	f->at += asz;
	return (int)asz;
}

EXPORT int tsmozc_host_write( int fd, const void *buf, int len )
{
	KCFD	*f = kc_of(fd);
	SZ	asz = 0;

	if ( f == NULL || len < 0 ) {
		return -1;
	}
	if ( f->append ) {
		if ( kc_wflush(f) < E_OK ) {
			return -1;
		}
		f->at = kc_size(f->key);
	}
	if ( f->wlen > 0 && ( f->at != f->wat + f->wlen || f->wlen + len > KC_WBUF ) ) {
		if ( kc_wflush(f) < E_OK ) {
			return -1;		/* not one after the other, or no room */
		}
	}
	if ( f->wbuf == NULL && len < KC_WBUF ) {
		f->wbuf = (UB *)Kmalloc(KC_WBUF);
	}
	if ( f->wbuf == NULL || len >= KC_WBUF ) {
		if ( ob_wri_rec(f->key, 1, f->at, buf, (SZ)len, &asz) < E_OK ) {
			return -1;
		}
		f->at += asz;
		return (int)asz;
	}
	if ( f->wlen == 0 ) {
		f->wat = f->at;
	}
	knl_memcpy(f->wbuf + f->wlen, buf, (SZ)len);
	f->wlen += len;
	f->at += len;
	return len;
}

EXPORT long long tsmozc_host_lseek( int fd, long long off, int whence )
{
	KCFD	*f = kc_of(fd);
	D	to;

	if ( f == NULL ) {
		return -1;
	}
	if ( whence == 2 && kc_wflush(f) < E_OK ) {
		return -1;
	}
	to = ( whence == 1 ) ? f->at + off : ( whence == 2 ) ? kc_size(f->key) + off : off;
	if ( to < 0 ) {
		return -1;
	}
	f->at = to;
	return (long long)to;
}

EXPORT int tsmozc_host_stat( const char *path, long long *p_size, int *p_dir,
			     long long *p_mtime )
{
	CONST UB	*name;
	TS_UUID		box, u;
	BOOL		dir;
	ID		key;

	name = kc_file(path, &dir);
	*p_size = 0;
	*p_dir = dir;
	*p_mtime = 0;
	if ( dir ) {
		return 0;
	}
	kc_wflush_all();
	if ( name == NULL || kc_box(&box) < E_OK || xf_find(&box, name, &u) < E_OK ) {
		return -1;
	}
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) {
		return -1;
	}
	*p_size = (long long)kc_size(key);
	ob_cls_obj(key);
	return 0;
}

EXPORT int tsmozc_host_fstat( int fd, long long *p_size, int *p_dir, long long *p_mtime )
{
	KCFD	*f = kc_of(fd);

	if ( f == NULL || kc_wflush(f) < E_OK ) {
		return -1;
	}
	*p_size = (long long)kc_size(f->key);
	*p_dir = 0;
	*p_mtime = 0;
	return 0;
}

EXPORT int tsmozc_host_unlink( const char *path )
{
	CONST UB	*name;
	TS_UUID		box, u;
	BOOL		dir;

	name = kc_file(path, &dir);
	kc_wflush_all();
	if ( name == NULL || kc_box(&box) < E_OK || xf_find(&box, name, &u) < E_OK ) {
		return -1;
	}
	(void)om_obj_link_del(&box, &u);
	return ( ob_del_obj(&u) >= E_OK ) ? 0 : -1;
}

EXPORT int tsmozc_host_rename( const char *from, const char *to )
{
	CONST UB	*a, *b;
	TS_UUID		box, u, was;
	BOOL		dir;

	a = kc_file(from, &dir);
	b = kc_file(to, &dir);
	kc_wflush_all();
	if ( a == NULL || b == NULL || kc_box(&box) < E_OK || xf_find(&box, a, &u) < E_OK ) {
		return -1;
	}
	if ( xf_find(&box, b, &was) >= E_OK && ts_uuid_cmp(&was, &u) != 0 ) {
		(void)om_obj_link_del(&box, &was);	/* what had the name goes */
		(void)ob_del_obj(&was);
	}
	return ( xf_rename(&u, b) >= E_OK ) ? 0 : -1;
}

EXPORT int tsmozc_host_mkdir( const char *path )
{
	BOOL	dir;

	(void)kc_file(path, &dir);
	return dir ? 0 : -1;				/* the box is always there */
}

#endif /* USE_MOZC */

/* ---------------------------------------------------------------- the calls */

/*
 * A session is either mozc's or the romaji converter's: mozc's numbers
 * are kept as they are, the others above RK_BASE.
 */
#define RK_BASE		0x10000

/* ---------------------------------------------------------------- keys */

/* keyboard usages */
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_TAB		0x2B
#define KEY_SPACE	0x2C
#define KEY_EISU	0x39
#define KEY_F1		0x3A
#define KEY_F8		0x41
#define KEY_F9		0x42
#define KEY_F10		0x43
#define KEY_F11		0x44
#define KEY_HOME	0x4A
#define KEY_PGUP	0x4B
#define KEY_DEL		0x4C
#define KEY_END		0x4D
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define KEY_HENKAN	0x8A
#define KEY_MUHENKAN	0x8B
#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define MOD_ALT		( HID_MOD_LALT | HID_MOD_RALT )

/*
 * The kana on the Japanese keyboard's keys, by usage: without shift and
 * with it. ゛ and ゜ are keys of their own; the converter puts them on
 * the kana before.
 */
typedef struct {
	UB		key;
	CONST char	*plain, *shift;
} KCKANA;

LOCAL CONST KCKANA kc_kana_keys[] = {
	{ 0x1E, "ぬ", "ぬ" }, { 0x1F, "ふ", "ふ" }, { 0x20, "あ", "ぁ" },
	{ 0x21, "う", "ぅ" }, { 0x22, "え", "ぇ" }, { 0x23, "お", "ぉ" },
	{ 0x24, "や", "ゃ" }, { 0x25, "ゆ", "ゅ" }, { 0x26, "よ", "ょ" },
	{ 0x27, "わ", "を" }, { 0x2D, "ほ", "ほ" }, { 0x2E, "へ", "へ" },
	{ 0x89, "ー", "ー" },
	{ 0x14, "た", "た" }, { 0x1A, "て", "て" }, { 0x08, "い", "ぃ" },
	{ 0x15, "す", "す" }, { 0x17, "か", "か" }, { 0x1C, "ん", "ん" },
	{ 0x18, "な", "な" }, { 0x0C, "に", "に" }, { 0x12, "ら", "ら" },
	{ 0x13, "せ", "せ" }, { 0x2F, "゛", "゛" }, { 0x30, "゜", "「" },
	{ 0x04, "ち", "ち" }, { 0x16, "と", "と" }, { 0x07, "し", "し" },
	{ 0x09, "は", "は" }, { 0x0A, "き", "き" }, { 0x0B, "く", "く" },
	{ 0x0D, "ま", "ま" }, { 0x0E, "の", "の" }, { 0x0F, "り", "り" },
	{ 0x33, "れ", "れ" }, { 0x34, "け", "け" }, { 0x31, "む", "」" },
	{ 0x32, "む", "」" },
	{ 0x1D, "つ", "っ" }, { 0x1B, "さ", "さ" }, { 0x06, "そ", "そ" },
	{ 0x19, "ひ", "ひ" }, { 0x05, "こ", "こ" }, { 0x11, "み", "み" },
	{ 0x10, "も", "も" }, { 0x36, "ね", "、" }, { 0x37, "る", "。" },
	{ 0x38, "め", "・" }, { 0x87, "ろ", "ろ" },
};

/* The kana a key types, UTF-8, with Shift or without; NULL for a key with none */
EXPORT CONST char *kc_kana_label( UINT key, BOOL shift )
{
	UINT	i;

	for ( i = 0; i < sizeof(kc_kana_keys) / sizeof(kc_kana_keys[0]); i++ ) {
		if ( kc_kana_keys[i].key == key ) {
			return (CONST char *)( shift ? kc_kana_keys[i].shift : kc_kana_keys[i].plain );
		}
	}
	return NULL;
}

/* A kana as its TRON code (JIS X 0208), from its UTF-8; 0 for one with none */
LOCAL UINT kana_code( CONST UB *s )
{
	LOCAL CONST UH	sign[][2] = {
		{ 0x30FC, 0x213C },	/* ー */
		{ 0x309B, 0x212B },	/* ゛ */
		{ 0x309C, 0x212C },	/* ゜ */
		{ 0x300C, 0x2156 },	/* 「 */
		{ 0x300D, 0x2157 },	/* 」 */
		{ 0x3001, 0x2122 },	/* 、 */
		{ 0x3002, 0x2123 },	/* 。 */
		{ 0x30FB, 0x2126 },	/* ・ */
		{ 0xFF0C, 0x2124 },	/* ， */
		{ 0xFF0E, 0x2125 },	/* ． */
		{ 0x30F4, 0x2574 },	/* ヴ */
		{ 0x30F5, 0x2575 },	/* ヵ */
		{ 0x30F6, 0x2576 },	/* ヶ */
	};
	UINT	i, u;

	if ( s == NULL || ( s[0] & 0xF0 ) != 0xE0 ) {
		return 0;
	}
	u = ( (UINT)( s[0] & 0x0F ) << 12 ) | ( (UINT)( s[1] & 0x3F ) << 6 ) | ( s[2] & 0x3F );
	if ( u >= 0x3041 && u <= 0x3093 ) {
		return 0x2421 + ( u - 0x3041 );
	}
	for ( i = 0; i < sizeof(sign) / sizeof(sign[0]); i++ ) {
		if ( sign[i][0] == u ) {
			return sign[i][1];
		}
	}
	return 0;
}

/*
 * A kana key's character as its TRON code, in the kana layout chosen
 * (キー配列), or the key's own (KM_MOD_LAYOUT, a screen keyboard's): JIS
 * かな by Shift, or a thumb-shift layout by the thumbs held
 * (ts/keymap.h); 0 for a key with none
 */
EXPORT UINT kc_kana_key_m( UINT key, UINT mods )
{
	if ( km_kana_of(mods) != KM_KANA_JIS ) {
		return kana_code((CONST UB *)km_kana_text(key, mods));
	}
	return kc_kana_key(key, (BOOL)( ( mods & MOD_SHIFT ) != 0 ));
}

/* A kana key's character as its TRON code (JIS X 0208), or 0 */
EXPORT UINT kc_kana_key( UINT key, BOOL shift )
{
	LOCAL CONST UH	sign[][2] = {
		{ 0x30FC, 0x213C },	/* ー */
		{ 0x309B, 0x212B },	/* ゛ */
		{ 0x309C, 0x212C },	/* ゜ */
		{ 0x300C, 0x2156 },	/* 「 */
		{ 0x300D, 0x2157 },	/* 」 */
		{ 0x3001, 0x2122 },	/* 、 */
		{ 0x3002, 0x2123 },	/* 。 */
		{ 0x30FB, 0x2126 },	/* ・ */
	};
	CONST UB	*s = NULL;
	UINT		i, u;

	for ( i = 0; i < sizeof(kc_kana_keys) / sizeof(kc_kana_keys[0]); i++ ) {
		if ( kc_kana_keys[i].key == key ) {
			s = (CONST UB *)( shift ? kc_kana_keys[i].shift
						: kc_kana_keys[i].plain );
			break;
		}
	}
	if ( s == NULL || ( s[0] & 0xF0 ) != 0xE0 ) {
		return 0;
	}
	u = ( (UINT)( s[0] & 0x0F ) << 12 ) | ( (UINT)( s[1] & 0x3F ) << 6 ) | ( s[2] & 0x3F );
	if ( u >= 0x3041 && u <= 0x3093 ) {
		return 0x2421 + ( u - 0x3041 );
	}
	for ( i = 0; i < sizeof(sign) / sizeof(sign[0]); i++ ) {
		if ( sign[i][0] == u ) {
			return sign[i][1];
		}
	}
	return 0;
}

/* The BTRON key a key is in 日本語 (TSMOZC_K_*), or 0 for a letter's key */
EXPORT UINT kc_code_key( UINT key, UINT mods )
{
	BOOL	shift = (BOOL)( ( mods & MOD_SHIFT ) != 0 );
	BOOL	alt = (BOOL)( ( mods & MOD_ALT ) != 0 );

	switch ( key ) {
	case KEY_SPACE:
		if ( alt ) {
			return shift ? TSMOZC_K_KATA : TSMOZC_K_HIRA;
		}
		return TSMOZC_K_SPACE;
	case KEY_TAB:		return alt ? TSMOZC_K_SPACE : TSMOZC_K_TAB;
	case KEY_HENKAN:	return shift ? TSMOZC_K_RCNV : TSMOZC_K_CNV;
	case KEY_MUHENKAN:	return shift ? TSMOZC_K_KATA : TSMOZC_K_HIRA;
	case KEY_ENTER:		return shift ? TSMOZC_K_CR : TSMOZC_K_NL;
	case KEY_BS:		return TSMOZC_K_BS;
	case KEY_DEL:		return TSMOZC_K_DEL;
	case KEY_ESC:
	case KEY_F9:		return TSMOZC_K_CAN;
	case KEY_F10:		return TSMOZC_K_IEND;
	case KEY_F11:		return TSMOZC_K_ASSIST;
	case KEY_EISU:		return alt ? TSMOZC_K_ASSIST : 0;
	case KEY_UP:		return alt ? TSMOZC_K_SC_U : TSMOZC_K_UP;
	case KEY_DOWN:		return alt ? TSMOZC_K_SC_D : TSMOZC_K_DOWN;
	case KEY_RIGHT:		return alt ? TSMOZC_K_SC_R : TSMOZC_K_RIGHT;
	case KEY_LEFT:		return alt ? TSMOZC_K_SC_L : TSMOZC_K_LEFT;
	case KEY_PGUP:		return TSMOZC_K_PG_U;
	case KEY_PGDN:		return TSMOZC_K_PG_D;
	case KEY_HOME:		return TSMOZC_K_HOME;
	case KEY_END:		return TSMOZC_K_END;
	default:
		break;
	}
	if ( key >= KEY_F1 && key <= KEY_F8 ) {
		return TSMOZC_K_PF1 + ( key - KEY_F1 );
	}
	return 0;
}

/* ---------------------------------------------------------------- the user dictionary watched */

EXPORT ER kc_words_reload( void )
{
	kw_load();
#if USE_MOZC
	if ( kc_up ) {
		KCREQ	r;
		UB	*w;
		INT	n = 0;
		ER	er;

		w = kw_engine(&n);
		if ( w == NULL ) {
			return E_NOMEM;
		}
		knl_memset(&r, 0, sizeof(r));
		r.op = OP_WORDS;
		r.buf = w;
		r.size = n;
		er = kc_call(&r);
		Kfree(w);
		if ( er < E_OK ) {
			tm_printf((UB *)"kconv: mozc did not take the user dictionary (%d)\n", (INT)er);
			return er;
		}
	}
#endif
	return E_OK;
}

#define KW_WATCH_PRI	30		/* low: reading the words is not urgent */

LOCAL ID	kw_watch_tsk = 0;

/*
 * The user dictionary object watched: a write to its record 1 (単語登録
 * writing a word, or anyone) has the words read again. A write and the
 * cut after it come as two notices close together, which are one change.
 */
LOCAL ID kw_watch_on( ID port )
{
	TS_UUID	u;
	T_OBNTF	req;
	ID	key;

	if ( kw_person(&u) < E_OK ) {
		return 0;
	}
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	key = ob_opn_obj(&u, OB_OP_ATRRD);
	if ( key <= 0 || ob_ntf_evt(key, 1, &req, port) <= 0 ) {
		tm_printf((UB *)"kconv: the user dictionary cannot be watched\n");
		if ( key > 0 ) (void)ob_cls_obj(key);
		return 0;
	}
	return key;
}

LOCAL void kw_watch( INT stacd, void *exinf )
{
	TS_UUID	ch, who;
	T_OBCRE	c;
	T_OBNTM	m;
	ID	port = 0, key = 0;
	SZ	asz = 0;

	(void)stacd;
	(void)exinf;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK
	  || ( port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) ) <= 0 ) {
		kw_watch_tsk = 0;
		tk_exd_tsk();
	}
	who = knl_ob_session_crd()->user;
	key = kw_watch_on(port);
	for ( ;; ) {
		if ( !kw_same(&who, &knl_ob_session_crd()->user) ) {
			/* someone else at the machine: their words, and theirs watched */
			who = knl_ob_session_crd()->user;
			if ( key > 0 ) (void)ob_cls_obj(key);
			key = kw_watch_on(port);
			(void)kc_words_reload();
			tm_printf((UB *)"kconv: the user dictionary of the person now at the machine read\n");
		}
		if ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) < E_OK || asz == 0 ) {
			tk_dly_tsk(100);		/* nothing yet */
			continue;
		}
		do {
			tk_dly_tsk(50);
		} while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz > 0 );
		(void)kc_words_reload();
	}
}

/* The words read, and their object watched from now on */
LOCAL void kw_start( void )
{
	T_CSEM	cs;
	T_CTSK	ct;
	ID	id;

	if ( kw_sem <= 0 ) {
		knl_memset(&cs, 0, sizeof(cs));
		cs.sematr = TA_TFIFO | TA_FIRST;
		cs.isemcnt = 1;
		cs.maxsem = 1;
		id = tk_cre_sem(&cs);
		if ( id > 0 ) kw_sem = id;
	}
	if ( kw_text == NULL ) {
		kw_load();
	}
	if ( kw_watch_tsk > 0 ) {
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)kw_watch;
	ct.itskpri = KW_WATCH_PRI;
	ct.stksz = 8 * 1024;
	id = tk_cre_tsk(&ct);
	if ( id > 0 && tk_sta_tsk(id, 0) >= E_OK ) {
		kw_watch_tsk = id;
	}
}

EXPORT ER kc_start( void )
{
#if USE_MOZC
	T_CSEM	cs;
	T_CTSK	ct;

	kw_start();
	if ( kc_task > 0 ) {
		return E_OK;
	}
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 1;
	kc_go = tk_cre_sem(&cs);
	kc_done = tk_cre_sem(&cs);
	{
		T_CMTX	cm;

		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		kc_turn = tk_cre_mtx(&cm);
	}
	if ( kc_go < E_OK || kc_done < E_OK || kc_turn < E_OK ) {
		return E_NOMEM;
	}
	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0 | TA_FPU;
	ct.task = (FP)kc_serve;
	ct.itskpri = 20;
	ct.stksz = KC_STACK;
	kc_task = tk_cre_tsk(&ct);
	if ( kc_task < E_OK ) {
		tm_printf((UB *)"kconv: no task for mozc (%d)\n", (INT)kc_task);
		kc_gone = TRUE;
		return kc_task;
	}
	return tk_sta_tsk(kc_task, 0);
#else
	kw_start();
	return E_OK;
#endif
}

EXPORT BOOL kc_kanji( void )
{
#if USE_MOZC
	return kc_up;
#else
	return FALSE;
#endif
}

EXPORT INT kc_open( void )
{
#if USE_MOZC
	KCREQ	r;
	INT	w;

	/* the engine may still be starting: wait for it a while */
	for ( w = 0; w < 300 && !kc_up && !kc_gone && kc_task > 0; w++ ) {
		tk_dly_tsk(100);
	}
	if ( kc_up ) {
		knl_memset(&r, 0, sizeof(r));
		r.op = OP_OPEN;
		if ( kc_call(&r) > 0 && r.ret > 0 ) {
			return r.ret;
		}
	}
#endif
	{
		INT	k = rk_open();

		return ( k > 0 ) ? RK_BASE + k : k;
	}
}

EXPORT void kc_close( INT kid )
{
	if ( kid > RK_BASE ) {
		RKSESS	*s = rk_of(kid - RK_BASE);

		if ( s != NULL ) {
			s->used = FALSE;
		}
		return;
	}
#if USE_MOZC
	{
		KCREQ	r;

		knl_memset(&r, 0, sizeof(r));
		r.op = OP_CLOSE;
		r.kid = kid;
		(void)kc_call(&r);
	}
#endif
}

EXPORT ER kc_input( INT kid, INT mode )
{
	if ( kid > RK_BASE ) {
		/* romaji and kana keys come as different codes: nothing to set */
		return ( rk_of(kid - RK_BASE) != NULL ) ? E_OK : E_ID;
	}
#if USE_MOZC
	{
		KCREQ	r;

		knl_memset(&r, 0, sizeof(r));
		r.op = OP_INPUT;
		r.kid = kid;
		r.n = mode;
		return kc_call(&r);
	}
#else
	(void)mode;
	return E_ID;
#endif
}

EXPORT INT kc_key( INT kid, UINT code, UINT stat, T_KCOUT *out )
{
	if ( kid > RK_BASE ) {
		return rk_key(kid - RK_BASE, code, stat, out);
	}
#if USE_MOZC
	{
		KCREQ	r;

		knl_memset(&r, 0, sizeof(r));
		r.op = OP_KEY;
		r.kid = kid;
		r.code = code;
		r.stat = stat;
		r.out = out;
		return kc_call(&r);
	}
#else
	return E_ID;
#endif
}

EXPORT INT kc_choose( INT kid, INT n, T_KCOUT *out )
{
	if ( kid > RK_BASE ) {
		return rk_choose(kid - RK_BASE, n, out);
	}
#if USE_MOZC
	{
		KCREQ	r;

		knl_memset(&r, 0, sizeof(r));
		r.op = OP_CHOOSE;
		r.kid = kid;
		r.n = n;
		r.out = out;
		return kc_call(&r);
	}
#else
	return E_ID;
#endif
}

EXPORT INT kc_list( INT kid, UB *buf, INT size, INT *p_count, INT *p_first,
		    INT *p_total )
{
	if ( kid > RK_BASE ) {
		return rk_list(kid - RK_BASE, buf, size, p_count, p_first, p_total);
	}
#if USE_MOZC
	{
		KCREQ	r;

		knl_memset(&r, 0, sizeof(r));
		r.op = OP_LIST;
		r.kid = kid;
		r.buf = buf;
		r.size = size;
		r.p1 = p_count;
		r.p2 = p_first;
		r.p3 = p_total;
		return kc_call(&r);
	}
#else
	return E_ID;
#endif
}
