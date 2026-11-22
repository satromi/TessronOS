/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	udic.c
 *	ユーザ辞書: the words a person registers, as text (ts/udic.h)
 *
 *	The text is walked a line at a time; nothing is kept beside it, so
 *	what the caller holds is the dictionary whole. A word is put by
 *	moving the lines after its place along, and taken out by moving
 *	them back.
 */

#include <ts/udic.h>

/* ---------------------------------------------------------------- small things */

LOCAL void move( UB *to, CONST UB *from, INT n )
{
	INT	i;

	if ( to < from ) {
		for ( i = 0; i < n; i++ ) to[i] = from[i];
	} else if ( to > from ) {
		for ( i = n - 1; i >= 0; i-- ) to[i] = from[i];
	}
}

LOCAL INT slen( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

/* Two strings of bytes in order: below 0, 0, above 0 */
LOCAL INT cmp( CONST UB *a, INT na, CONST UB *b, INT nb )
{
	INT	i;

	for ( i = 0; i < na && i < nb; i++ ) {
		if ( a[i] != b[i] ) return (INT)a[i] - (INT)b[i];
	}
	return na - nb;
}

LOCAL BOOL same( CONST UB *a, INT na, CONST char *b )
{
	return (BOOL)( cmp(a, na, (CONST UB *)b, slen(b)) == 0 );
}

/* ---------------------------------------------------------------- parts of speech */

/* By mozc's number: its name for it */
LOCAL CONST char *CONST pos_names[UD_POS_MAX + 1] = {
	NULL,
	"名詞", "短縮よみ", "サジェストのみ", "固有名詞", "人名", "姓", "名", "組織",
	"地名", "名詞サ変", "名詞形動", "数", "アルファベット", "記号", "顔文字",
	"副詞", "連体詞", "接続詞", "感動詞", "接頭語", "助数詞", "接尾一般",
	"接尾人名", "接尾地名", "動詞ワ行五段", "動詞カ行五段", "動詞サ行五段",
	"動詞タ行五段", "動詞ナ行五段", "動詞マ行五段", "動詞ラ行五段",
	"動詞ガ行五段", "動詞バ行五段", "動詞ハ行四段", "動詞一段", "動詞カ変",
	"動詞サ変", "動詞ザ変", "動詞ラ変", "形容詞", "終助詞", "句読点",
	"独立語", "抑制単語",
};

/* The names other converters write, and the part each is nearest to */
typedef struct {
	CONST char	*name;
	UB		pos;
} POSALIAS;

LOCAL CONST POSALIAS pos_alias[] = {
	{ "一般名詞", 1 }, { "普通名詞", 1 }, { "単漢字", 1 },
	{ "さ変名詞", 10 }, { "サ変名詞", 10 }, { "名詞サ変", 10 },
	{ "形動名詞", 11 }, { "形容動詞", 11 }, { "名詞形容動詞", 11 },
	{ "人名（姓）", 6 }, { "人名（名）", 7 }, { "人名(姓)", 6 }, { "人名(名)", 7 },
	{ "地名その他", 9 }, { "組織名", 8 }, { "会社名", 8 },
	{ "数詞", 12 }, { "冠数詞", 12 }, { "接頭辞", 20 }, { "接尾辞", 22 },
	{ "カ行五段", 26 }, { "ガ行五段", 32 }, { "サ行五段", 27 }, { "タ行五段", 28 },
	{ "ナ行五段", 29 }, { "バ行五段", 33 }, { "マ行五段", 30 }, { "ラ行五段", 31 },
	{ "ワ行五段", 25 }, { "ハ行四段", 34 }, { "一段活用", 35 }, { "一段動詞", 35 },
	{ "カ変動詞", 36 }, { "サ変動詞", 37 }, { "ザ変動詞", 38 },
};

/* What the accessory offers, the most used first */
LOCAL CONST UB pos_offered[] = {
	1, 4, 5, 6, 7, 9, 8, 10, 11, 2, 15, 14, 16, 40, 19,
	26, 27, 28, 31, 25, 30, 32, 33, 29, 35, 37, 44,
};

EXPORT CONST char *ud_pos_name( INT pos )
{
	return ( pos >= 1 && pos <= UD_POS_MAX ) ? pos_names[pos] : NULL;
}

EXPORT INT ud_pos_of( CONST UB *name, INT n )
{
	INT	i;

	while ( n > 0 && ( name[n - 1] == ' ' || name[n - 1] == '\r' ) ) n--;
	while ( n > 0 && name[0] == ' ' ) {
		name++;
		n--;
	}
	if ( n <= 0 ) {
		return 0;
	}
	for ( i = 1; i <= UD_POS_MAX; i++ ) {
		if ( same(name, n, pos_names[i]) ) return i;
	}
	for ( i = 0; i < (INT)( sizeof(pos_alias) / sizeof(pos_alias[0]) ); i++ ) {
		if ( same(name, n, pos_alias[i].name) ) return pos_alias[i].pos;
	}
	return 0;
}

EXPORT INT ud_pos_offer( INT k )
{
	return ( k >= 0 && k < (INT)sizeof(pos_offered) ) ? pos_offered[k] : 0;
}

/* ---------------------------------------------------------------- readings */

EXPORT void ud_hira( UB *s, INT n )
{
	INT	i;

	for ( i = 0; i + 2 < n; i++ ) {
		UINT	u;

		if ( ( s[i] & 0xF0 ) != 0xE0 ) continue;
		u = ( (UINT)( s[i] & 0x0F ) << 12 ) | ( (UINT)( s[i + 1] & 0x3F ) << 6 ) | ( s[i + 2] & 0x3F );
		if ( u >= 0x30A1 && u <= 0x30F6 ) {
			u -= 0x60;
			s[i] = (UB)( 0xE0 | ( u >> 12 ) );
			s[i + 1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			s[i + 2] = (UB)( 0x80 | ( u & 0x3F ) );
		}
		i += 2;
	}
}

/* The ideographic space, which a reading cannot have either */
LOCAL BOOL wide_space( CONST UB *s, INT n )
{
	return (BOOL)( n >= 3 && s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80 );
}

/*
 * A reading as it is typed: where it starts and how long it is with the
 * spaces round it taken off; its length, or E_PAR when it is empty or has
 * a letter a reading cannot have. Nothing is copied, so no length is
 * too long for it.
 */
LOCAL INT yomi_span( CONST UB **p_in, INT n )
{
	CONST UB	*in = *p_in;
	INT		i;

	while ( n > 0 && ( in[0] == ' ' || wide_space(in, n) ) ) {
		i = ( in[0] == ' ' ) ? 1 : 3;
		in += i;
		n -= i;
	}
	while ( n > 0 && ( in[n - 1] == ' ' || in[n - 1] == '\r'
		|| ( n >= 3 && wide_space(in + n - 3, 3) ) ) ) {
		n -= ( in[n - 1] == 0x80 ) ? 3 : 1;
	}
	if ( n <= 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) {
		if ( in[i] <= 0x20 || in[i] == 0x7F || wide_space(in + i, n - i) ) {
			return E_PAR;
		}
	}
	*p_in = in;
	return n;
}

EXPORT INT ud_yomi( CONST UB *in, INT n, UB *out, INT max )
{
	INT	i;

	n = yomi_span(&in, n);
	if ( n < 0 || n >= max ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) out[i] = in[i];
	out[n] = 0;
	ud_hira(out, n);
	return n;
}

/*
 * A kept reading (already in hiragana) against one as typed, whose
 * katakana are taken as hiragana as they are met: below 0, 0, above 0.
 */
LOCAL INT cmp_hira( CONST UB *a, INT na, CONST UB *b, INT nb )
{
	INT	i = 0, j = 0, k;

	while ( i < na && j < nb ) {
		UB	t[3];
		INT	len = 1;

		t[0] = b[j];
		if ( j + 2 < nb && ( b[j] & 0xF0 ) == 0xE0 ) {
			t[1] = b[j + 1];
			t[2] = b[j + 2];
			ud_hira(t, 3);
			len = 3;
		}
		for ( k = 0; k < len; k++ ) {
			if ( i + k >= na ) return -1;
			if ( a[i + k] != t[k] ) return (INT)a[i + k] - (INT)t[k];
		}
		i += len;
		j += len;
	}
	return ( na - i ) - ( nb - j );
}

/* A word as it can be kept: the spaces round it taken off; its length, or E_PAR */
LOCAL INT word_ok( CONST UB **p_w, INT n )
{
	CONST UB	*w = *p_w;
	INT		i;

	while ( n > 0 && w[0] == ' ' ) {
		w++;
		n--;
	}
	while ( n > 0 && ( w[n - 1] == ' ' || w[n - 1] == '\r' ) ) n--;
	if ( n <= 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) {
		if ( w[i] < 0x20 || w[i] == 0x7F ) return E_PAR;
	}
	*p_w = w;
	return n;
}

/* ---------------------------------------------------------------- walking the text */

EXPORT BOOL ud_next( CONST UB *text, INT len, INT *p_at, UDENT *e )
{
	INT	at = *p_at;

	while ( at < len ) {
		INT	end = at, stop, t1 = -1, t2 = -1, t3 = -1, i;

		while ( end < len && text[end] != '\n' ) end++;
		stop = end;
		if ( stop > at && text[stop - 1] == '\r' ) stop--;
		if ( end < len ) end++;			/* past its LF */
		for ( i = at; i < stop; i++ ) {
			if ( text[i] != '\t' ) continue;
			if ( t1 < 0 ) t1 = i;
			else if ( t2 < 0 ) t2 = i;
			else if ( t3 < 0 ) t3 = i;
		}
		if ( stop > at && text[at] != '#' && text[at] != '!' && t1 > at && t1 + 1 < ( t2 < 0 ? stop : t2 ) ) {
			e->yomi = text + at;
			e->nyomi = t1 - at;
			e->word = text + t1 + 1;
			e->nword = ( ( t2 < 0 ) ? stop : t2 ) - ( t1 + 1 );
			e->pos = ( t2 < 0 ) ? 0 : ud_pos_of(text + t2 + 1, ( ( t3 < 0 ) ? stop : t3 ) - ( t2 + 1 ));
			if ( e->pos == 0 ) e->pos = UD_POS_NOUN;
			e->at = at;
			e->end = end;
			*p_at = end;
			return TRUE;
		}
		at = end;				/* not a word: a heading, a blank */
	}
	*p_at = len;
	return FALSE;
}

EXPORT INT ud_count( CONST UB *text, INT len )
{
	UDENT	e;
	INT	at = 0, n = 0;

	while ( ud_next(text, len, &at, &e) ) n++;
	return n;
}

EXPORT INT ud_lookup( CONST UB *text, INT len, CONST UB *yomi, INT nyomi, UDENT *e, INT max )
{
	UDENT	w;
	INT	at = 0, n = 0, k;

	k = yomi_span(&yomi, nyomi);
	if ( k <= 0 ) {
		return 0;
	}
	while ( ud_next(text, len, &at, &w) ) {
		if ( cmp_hira(w.yomi, w.nyomi, yomi, k) != 0 ) continue;
		if ( n < max ) e[n] = w;
		n++;
	}
	return n;
}

/*
 * The letter at byte i as it is compared in a search: the full-width
 * forms of ASCII as ASCII, the ideographic space as a space, katakana as
 * hiragana. Its bytes taken in *p_len. Nothing is copied, so the words
 * compared have no length limit.
 */
LOCAL UINT fold_at( CONST UB *s, INT n, INT i, INT *p_len )
{
	if ( i + 2 < n && ( s[i] & 0xF0 ) == 0xE0 ) {
		UINT	u = ( (UINT)( s[i] & 0x0F ) << 12 ) | ( (UINT)( s[i + 1] & 0x3F ) << 6 ) | ( s[i + 2] & 0x3F );

		*p_len = 3;
		if ( u >= 0xFF01 && u <= 0xFF5E ) return u - 0xFF01 + 0x21;
		if ( u == 0x3000 ) return ' ';
		if ( u >= 0x30A1 && u <= 0x30F6 ) u -= 0x60;
		return 0x10000 | u;
	}
	*p_len = 1;
	return s[i];
}

/* Whether a, from byte i on, starts with k, the letters folded */
LOCAL BOOL fold_starts( CONST UB *a, INT na, INT i, CONST UB *k, INT nk )
{
	INT	j = 0, la, lk;

	while ( j < nk ) {
		if ( i >= na ) return FALSE;
		if ( fold_at(a, na, i, &la) != fold_at(k, nk, j, &lk) ) return FALSE;
		i += la;
		j += lk;
	}
	return TRUE;
}

EXPORT BOOL ud_match( CONST UDENT *e, CONST UB *key, INT nkey )
{
	INT	i = 0, l;

	if ( nkey <= 0 ) {
		return TRUE;
	}
	if ( fold_starts(e->yomi, e->nyomi, 0, key, nkey) ) {
		return TRUE;
	}
	while ( i < e->nword ) {
		if ( fold_starts(e->word, e->nword, i, key, nkey) ) return TRUE;
		(void)fold_at(e->word, e->nword, i, &l);
		i += l;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- changing it */

/* n bytes made at at (n > 0) or taken from it (n < 0); FALSE when they do not fit */
LOCAL BOOL room( UB *text, INT *p_len, INT max, INT at, INT n )
{
	if ( *p_len + n > max ) {
		return FALSE;
	}
	if ( n > 0 ) {
		move(text + at + n, text + at, *p_len - at);
	} else if ( n < 0 ) {
		move(text + at, text + at - n, *p_len - ( at - n ));
	}
	*p_len += n;
	return TRUE;
}

EXPORT ER ud_put( UB *text, INT *p_len, INT max, CONST UB *yomi, INT nyomi,
		  CONST UB *word, INT nword, INT pos )
{
	CONST char	*pn = ud_pos_name(pos);
	UDENT		e;
	INT		ny, at = 0, place = -1, i, n, line;

	ny = yomi_span(&yomi, nyomi);
	nword = word_ok(&word, nword);
	if ( ny <= 0 || nword <= 0 || pn == NULL ) {
		return E_PAR;
	}
	for ( i = 0; i < nword; i++ ) {
		if ( word[i] == '\t' || word[i] == '\n' ) return E_PAR;
	}
	line = ny + 1 + nword + 1 + slen(pn) + 1;
	/* where it goes: its own line when it is there, else before the first after it */
	while ( ud_next(text, *p_len, &at, &e) ) {
		INT	c = cmp_hira(e.yomi, e.nyomi, yomi, ny);

		if ( c == 0 ) c = cmp(e.word, e.nword, word, nword);
		if ( c == 0 ) {
			if ( *p_len - ( e.end - e.at ) + line > max ) return E_LIMIT;
			(void)room(text, p_len, max, e.at, -( e.end - e.at ));
			place = e.at;
			break;
		}
		if ( c > 0 ) {
			place = e.at;
			break;
		}
	}
	if ( place < 0 ) {
		/* at the end, after a line's end of its own */
		if ( *p_len > 0 && text[*p_len - 1] != '\n' ) {
			if ( *p_len + 1 > max ) return E_LIMIT;
			text[( *p_len )++] = '\n';
		}
		place = *p_len;
	}
	if ( !room(text, p_len, max, place, line) ) {
		return E_LIMIT;
	}
	n = place;
	for ( i = 0; i < ny; i++ ) text[n++] = yomi[i];
	ud_hira(text + place, ny);
	text[n++] = '\t';
	for ( i = 0; i < nword; i++ ) text[n++] = word[i];
	text[n++] = '\t';
	for ( i = 0; pn[i] != 0; i++ ) text[n++] = (UB)pn[i];
	text[n++] = '\n';
	return E_OK;
}

EXPORT ER ud_del( UB *text, INT *p_len, CONST UB *yomi, INT nyomi, CONST UB *word, INT nword )
{
	UDENT	e;
	INT	ny, at = 0;

	ny = yomi_span(&yomi, nyomi);
	nword = word_ok(&word, nword);
	if ( ny <= 0 || nword <= 0 ) {
		return E_NOEXS;
	}
	while ( ud_next(text, *p_len, &at, &e) ) {
		if ( cmp_hira(e.yomi, e.nyomi, yomi, ny) == 0 && cmp(e.word, e.nword, word, nword) == 0 ) {
			(void)room(text, p_len, *p_len, e.at, -( e.end - e.at ));
			return E_OK;
		}
	}
	return E_NOEXS;
}

/* ---------------------------------------------------------------- reading in, giving out */

/* One line read in: 1 put, 0 not a word (or only a heading), E_LIMIT full */
LOCAL INT import_line( UB *text, INT *p_len, INT max, CONST UB *s, INT n, INT *p_bad )
{
	INT	t1 = -1, t2 = -1, t3 = -1, i, pos;
	ER	er;

	while ( n > 0 && ( s[n - 1] == '\r' || s[n - 1] == ' ' ) ) n--;
	if ( n == 0 || s[0] == '!' || s[0] == '#' ) {
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		if ( s[i] != '\t' ) continue;
		if ( t1 < 0 ) t1 = i;
		else if ( t2 < 0 ) t2 = i;
		else if ( t3 < 0 ) t3 = i;
	}
	if ( t1 <= 0 ) {
		( *p_bad )++;
		return 0;
	}
	pos = ( t2 < 0 ) ? UD_POS_NOUN : ud_pos_of(s + t2 + 1, ( ( t3 < 0 ) ? n : t3 ) - ( t2 + 1 ));
	if ( pos == 0 ) pos = UD_POS_NOUN;
	er = ud_put(text, p_len, max, s, t1, s + t1 + 1, ( ( t2 < 0 ) ? n : t2 ) - ( t1 + 1 ), pos);
	if ( er == E_LIMIT ) {
		return E_LIMIT;
	}
	if ( er < E_OK ) {
		( *p_bad )++;
		return 0;
	}
	return 1;
}

/* A character put as UTF-8; how many bytes */
LOCAL INT utf8( UB *t, UINT u )
{
	if ( u < 0x80 ) {
		t[0] = (UB)u;
		return 1;
	}
	if ( u < 0x800 ) {
		t[0] = (UB)( 0xC0 | ( u >> 6 ) );
		t[1] = (UB)( 0x80 | ( u & 0x3F ) );
		return 2;
	}
	if ( u < 0x10000 ) {
		t[0] = (UB)( 0xE0 | ( u >> 12 ) );
		t[1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		t[2] = (UB)( 0x80 | ( u & 0x3F ) );
		return 3;
	}
	t[0] = (UB)( 0xF0 | ( u >> 18 ) );
	t[1] = (UB)( 0x80 | ( ( u >> 12 ) & 0x3F ) );
	t[2] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
	t[3] = (UB)( 0x80 | ( u & 0x3F ) );
	return 4;
}

/*
 * One line of UTF-16 from *p_at to its LF, as UTF-8: how many bytes it
 * takes, and with out, written there. *p_at is left past the LF. An odd
 * last byte is passed over.
 */
LOCAL INT w16_line( CONST UB *in, INT nin, INT *p_at, BOOL big, UB *out )
{
	INT	at = *p_at, n = 0, k;

	while ( at + 1 < nin ) {
		UB	b[4];
		UINT	u = big ? ( ( (UINT)in[at] << 8 ) | in[at + 1] ) : ( ( (UINT)in[at + 1] << 8 ) | in[at] );
		INT	nb;

		at += 2;
		if ( u >= 0xD800 && u < 0xDC00 && at + 1 < nin ) {
			UINT	lo = big ? ( ( (UINT)in[at] << 8 ) | in[at + 1] )
					 : ( ( (UINT)in[at + 1] << 8 ) | in[at] );

			if ( lo >= 0xDC00 && lo < 0xE000 ) {
				u = 0x10000 + ( ( u - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
				at += 2;
			}
		}
		if ( u == '\n' ) break;
		nb = utf8(b, u);
		if ( out != NULL ) {
			for ( k = 0; k < nb; k++ ) out[n + k] = b[k];
		}
		n += nb;
	}
	*p_at = at;
	return n;
}

EXPORT INT ud_import( UB *text, INT *p_len, INT max, CONST UB *in, INT nin, INT *p_bad )
{
	INT	at = 0, put = 0, r;
	BOOL	wide = FALSE, big = FALSE;

	*p_bad = 0;
	if ( nin >= 2 && in[0] == 0xFF && in[1] == 0xFE ) {
		wide = TRUE;
		at = 2;
	} else if ( nin >= 2 && in[0] == 0xFE && in[1] == 0xFF ) {
		wide = big = TRUE;
		at = 2;
	} else if ( nin >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF ) {
		at = 3;
	}
	if ( !wide ) {
		/* UTF-8: each line is read where it lies, whatever its length */
		while ( at < nin ) {
			INT	end = at;

			while ( end < nin && in[end] != '\n' ) end++;
			r = import_line(text, p_len, max, in + at, end - at, p_bad);
			if ( r == E_LIMIT ) return E_LIMIT;
			put += r;
			at = end + 1;
		}
		return put;
	}
	/*
	 * UTF-16: a line is made into UTF-8 in the free end of the text,
	 * which the words put keep clear of. A line that does not fit there
	 * could not be put either.
	 */
	while ( at + 1 < nin ) {
		INT	from = at, nl = w16_line(in, nin, &at, big, NULL);
		UB	*line;

		if ( nl <= 0 ) continue;
		if ( *p_len > max - nl ) return E_LIMIT;
		line = text + ( max - nl );
		(void)w16_line(in, nin, &from, big, line);
		r = import_line(text, p_len, max - nl, line, nl, p_bad);
		if ( r == E_LIMIT ) return E_LIMIT;
		put += r;
	}
	return put;
}

EXPORT INT ud_engine( CONST UB *text, INT len, UB *out, INT max )
{
	UDENT	e;
	INT	at = 0, n = 0, i;

	while ( ud_next(text, len, &at, &e) ) {
		UB	num[4];
		INT	nn = 0, p = e.pos;

		do {
			num[nn++] = (UB)( '0' + p % 10 );
			p /= 10;
		} while ( p > 0 );
		if ( n + e.nyomi + e.nword + nn + 4 > max ) {
			return E_LIMIT;
		}
		for ( i = 0; i < e.nyomi; i++ ) out[n++] = e.yomi[i];
		out[n++] = '\t';
		for ( i = 0; i < e.nword; i++ ) out[n++] = e.word[i];
		out[n++] = '\t';
		while ( nn > 0 ) out[n++] = num[--nn];
		out[n++] = '\n';
	}
	if ( n < max ) out[n] = 0;
	return n;
}
