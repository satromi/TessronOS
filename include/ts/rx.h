/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	rx.h
 *	Regular expressions, for 検索/置換 in a text
 */

#ifndef __TS_RX_H__
#define __TS_RX_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a pattern may say, as a script's regular expressions say it:
 *
 *   x       a letter, as it is (UTF-8)
 *   .       any letter but a new line
 *   [...]   one of the letters, or ranges a-z; [^...] none of them
 *   \d \w \s  a digit, a word letter, a space; \D \W \S not one
 *   \t \n   a tab, a new line; \ before anything else: that, as it is
 *   ^ $     the start and the end of what is looked through
 *   (...)   a group, numbered from 1 in order of its '('; (?:...) one
 *           with no number
 *   a|b     either
 *   * + ? {n} {n,} {n,m}   so many of what is before; with ? after,
 *           as few as will do
 *
 * The letters are matched exactly, as they are written.
 */
#define RX_GROUPS	10		/* the whole match and groups 1 to 9 */

typedef struct t_rx T_RX;

/* A pattern read; E_PAR when it is not one, E_NOMEM, E_LIMIT too long */
IMPORT ER   rx_compile( CONST UB *pat, T_RX **p_rx );
IMPORT void rx_free( T_RX *rx );

/*
 * The first match in s[0..n) that starts at or after 'from': where it
 * starts and ends, and, when 'groups' is given, where each group
 * matched (groups[2k], groups[2k+1]; -1 for one that did not).
 */
IMPORT BOOL rx_find( CONST T_RX *rx, CONST UB *s, INT n, INT from,
		     INT *p_start, INT *p_end, INT *groups );

/*
 * The words to put in place of a match: 'with' with $& the match, $1
 * to $9 its groups and $$ a dollar. Answers the length, at most max-1,
 * and ends the words with 0.
 */
IMPORT INT  rx_expand( CONST UB *with, CONST UB *s, CONST INT *groups,
		       UB *out, INT max );

#ifdef __cplusplus
}
#endif
#endif /* __TS_RX_H__ */
