/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kinsoku.h
 *	禁則: the letters that may not begin a line (行頭禁則) and those
 *	that may not end one (行末禁則), and where a line of letters that
 *	is full is broken so that neither stands where it may not (design
 *	10.7.1). The text laid out by the viewer (docview.c), the
 *	desktop's notices and the browser's panels all break their lines
 *	with these, in the kernel and in a process alike.
 *
 *	A line that a letter forbidden at its head would begin is broken
 *	before the letter in front of it, which goes down with it
 *	(追い出し, JIS X 4051): the lines of a panel or a notice are not
 *	stretched to the edge, so there is no space to take up (追い込み),
 *	and a letter hung past the edge (ぶら下げ) would stand outside the
 *	part the line is drawn in. A word of Latin letters is kept whole
 *	on a line when it fits on one.
 */

#ifndef __TS_KINSOKU_H__
#define __TS_KINSOKU_H__

#include <tk/typedef.h>

/* The letter at s (UTF-8), and how many bytes it takes */
static inline UW kin_utf8( const UB *s, INT *p_n )
{
	UW	c = s[0];
	INT	n = 1;

	if ( c >= 0xF0U && s[1] != 0 && s[2] != 0 && s[3] != 0 ) {
		c = ( ( c & 0x07U ) << 18 ) | ( ( s[1] & 0x3FU ) << 12 ) | ( ( s[2] & 0x3FU ) << 6 ) | ( s[3] & 0x3FU );
		n = 4;
	} else if ( c >= 0xE0U && s[1] != 0 && s[2] != 0 ) {
		c = ( ( c & 0x0FU ) << 12 ) | ( ( s[1] & 0x3FU ) << 6 ) | ( s[2] & 0x3FU );
		n = 3;
	} else if ( c >= 0xC0U && s[1] != 0 ) {
		c = ( ( c & 0x1FU ) << 6 ) | ( s[1] & 0x3FU );
		n = 2;
	}
	if ( p_n != NULL ) {
		*p_n = n;
	}

	return c;
}

static inline BOOL kin_in( const UW *tab, INT n, UW c )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( tab[i] == c ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* Letters that may not begin a line */
static inline BOOL kin_no_head( UW c )
{
	static const UW	tab[] = {
		0x3001, 0x3002,			/* 、 。 */
		0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B,	/* ， ． ： ； */
		0xFF1F, 0xFF01,			/* ？ ！ */
		0x30FB, 0x30FC,			/* ・ ー */
		0xFF09, 0x300D, 0x300F, 0x3011,	/* ） 」 』 】 */
		0x3015, 0xFF5D, 0x3009, 0x300B,	/* 〕 ｝ 〉 》 */
		0x3041, 0x3043, 0x3045, 0x3047, 0x3049,	/* ぁぃぅぇぉ */
		0x3063, 0x3083, 0x3085, 0x3087, 0x308E,	/* っゃゅょゎ */
		0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,	/* ァィゥェォ */
		0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE,	/* ッャュョヮ */
		',', '.', ':', ';', '?', '!', ')', ']', '}',
	};

	return kin_in(tab, (INT)( sizeof(tab) / sizeof(tab[0]) ), c);
}

/* Letters that may not end a line */
static inline BOOL kin_no_tail( UW c )
{
	static const UW	tab[] = {
		0xFF08, 0x300C, 0x300E, 0x3010,	/* （ 「 『 【 */
		0x3014, 0xFF5B, 0x3008, 0x300A,	/* 〔 ｛ 〈 《 */
		'(', '[', '{',
	};

	return kin_in(tab, (INT)( sizeof(tab) / sizeof(tab[0]) ), c);
}

/* A letter of a word of Latin letters, kept on one line */
static inline BOOL kin_word( UW c )
{
	return (BOOL)( c > 0x20 && c < 0x7F );
}

/*
 * Where a full line of s ends: its first 'fit' bytes (a letter's edge,
 * fit > 0) are as much as it holds and the letter at s + fit does not
 * fit on it. The bytes before the break that is returned (0 < it <=
 * fit) are the line; the letters after it begin the next one. When
 * nowhere on the line may it be broken, it is broken where it is full.
 * Spaces after the break begin no line: the caller passes over them.
 */
static inline INT kin_break( const UB *s, INT fit )
{
	INT	b = fit, p;
	UW	next, prev;

	while ( b > 0 ) {
		p = b - 1;
		while ( p > 0 && ( s[p] & 0xC0U ) == 0x80U ) {
			p--;
		}
		prev = kin_utf8(s + p, NULL);
		next = kin_utf8(s + b, NULL);
		if ( !kin_no_head(next) && !kin_no_tail(prev) && !( kin_word(prev) && kin_word(next) ) ) {
			return b;
		}
		b = p;
	}

	return fit;
}

#endif /* __TS_KINSOKU_H__ */
