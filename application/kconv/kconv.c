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
#include <ts/hid.h>

/* ---------------------------------------------------------------- the romaji converter */

#define RK_SESS		8
#define RK_TEXT		256

typedef struct {
	BOOL	used;
	UB	kana[RK_TEXT];		/* the kana made so far */
	INT	nk;
	UB	roma[4];		/* romaji not yet a kana */
	INT	nr;
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
	if ( s->nk + s->nr > 0 ) {
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

/* The composition's hiragana from byte from on turned into katakana */
LOCAL void rk_katakana( RKSESS *s, INT from )
{
	INT	i;

	for ( i = from; i + 2 < s->nk; i++ ) {
		UB	a = s->kana[i], b = s->kana[i + 1], c = s->kana[i + 2];
		UINT	u;

		if ( ( a & 0xF0 ) != 0xE0 ) {
			continue;
		}
		u = ( (UINT)( a & 0x0F ) << 12 ) | ( (UINT)( b & 0x3F ) << 6 ) | ( c & 0x3F );
		if ( u >= 0x3041 && u <= 0x3096 ) {
			u += 0x60;
			s->kana[i] = (UB)( 0xE0 | ( u >> 12 ) );
			s->kana[i + 1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			s->kana[i + 2] = (UB)( 0x80 | ( u & 0x3F ) );
		}
		i += 2;
	}
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

LOCAL INT rk_key( INT kid, UINT code, UINT stat, T_KCOUT *out )
{
	RKSESS	*s = rk_of(kid);
	BOOL	busy, kata = (BOOL)( ( stat & TSMOZC_S_KANA ) != 0 );
	INT	was;
	UINT	u;

	if ( s == NULL ) {
		return E_ID;
	}
	busy = (BOOL)( s->nk + s->nr > 0 );
	was = s->nk;
	if ( ( code >= 'a' && code <= 'z' ) || code == '-' || code == ','
	  || code == '.' || code == '[' || code == ']' || code == '\'' ) {
		rk_letter(s, (UB)code);
		if ( kata ) {
			rk_katakana(s, was);
		}
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_CAR;
	}
	if ( code > 0x20 && code < 0x7F ) {
		UB	one[2] = { (UB)code, 0 };

		rk_settle(s);
		rk_put(s, (CONST char *)one);
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_CAR;
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
		rk_out(s, out, NULL, 0);
		return TSMOZC_CNV | TSMOZC_CAR;
	}
	if ( !busy ) {
		return KC_NOTMINE;
	}
	switch ( code ) {
	case TSMOZC_K_CR:
	case TSMOZC_K_NL:
	case TSMOZC_K_IEND:
	case TSMOZC_K_SPACE:
	case TSMOZC_K_CNV: {
		/* committed as it stands: there is nothing to convert it to */
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
} KCREQ;

#define OP_OPEN		1
#define OP_CLOSE	2
#define OP_KEY		3
#define OP_CHOOSE	4
#define OP_LIST		5
#define OP_INPUT	6

LOCAL ID	kc_task = 0;
LOCAL ID	kc_go, kc_done, kc_turn;	/* semaphores */
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
		default:
			r->ret = E_PAR;
			break;
		}
		tk_sig_sem(kc_done, 1);
	}
}

/* A request done by the engine's task: its answer */
LOCAL INT kc_call( KCREQ *r )
{
	if ( !kc_up ) {
		return E_NOEXS;
	}
	if ( tk_wai_sem(kc_turn, 1, TMO_FEVR) < E_OK ) {
		return E_SYS;
	}
	kc_req = r;
	tk_sig_sem(kc_go, 1);
	if ( tk_wai_sem(kc_done, 1, 10000) < E_OK ) {
		/* the engine stopped in the middle: it is not asked again */
		kc_up = FALSE;
		kc_gone = TRUE;
		tm_printf((UB *)"kconv: mozc did not answer\n");
		return E_TMOUT;
	}
	tk_sig_sem(kc_turn, 1);

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
 */
#define KC_FD_MAX	8

typedef struct {
	ID	key;		/* 0: free */
	D	at;
	BOOL	append;
} KCFD;

LOCAL KCFD	kc_fd[KC_FD_MAX];

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
	return ts_str_to_uuid(SYSDEF_LEARN_BOX, box);
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
	return i;
}

EXPORT int tsmozc_host_close( int fd )
{
	KCFD	*f = kc_of(fd);

	if ( f == NULL ) {
		return -1;
	}
	ob_cls_obj(f->key);
	f->key = 0;
	return 0;
}

EXPORT int tsmozc_host_read( int fd, void *buf, int len )
{
	KCFD	*f = kc_of(fd);
	SZ	asz = 0;
	ER	er;

	if ( f == NULL || len < 0 ) {
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
		f->at = kc_size(f->key);
	}
	if ( ob_wri_rec(f->key, 1, f->at, buf, (SZ)len, &asz) < E_OK ) {
		return -1;
	}
	f->at += asz;
	return (int)asz;
}

EXPORT long long tsmozc_host_lseek( int fd, long long off, int whence )
{
	KCFD	*f = kc_of(fd);
	D	to;

	if ( f == NULL ) {
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

	if ( f == NULL ) {
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

EXPORT ER kc_start( void )
{
#if USE_MOZC
	T_CSEM	cs;
	T_CTSK	ct;

	if ( kc_task > 0 ) {
		return E_OK;
	}
	cs.exinf = NULL;
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 1;
	kc_go = tk_cre_sem(&cs);
	kc_done = tk_cre_sem(&cs);
	cs.isemcnt = 1;
	kc_turn = tk_cre_sem(&cs);
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
		return KC_NOTMINE;
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
		return E_NOEXS;
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
