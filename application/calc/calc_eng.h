/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_eng.h
 *	基本表計算: what works the formulas out (design 11.13)
 *
 *	The program hands a cell's text -- words, or a formula from "=" --
 *	and is handed back the cells whose words changed, each as the words
 *	it now shows. Two engines answer to these calls, which one decided
 *	when the program is built: JavaScript in V8 (calc_v8.cc, with
 *	formula.min.js, jstat.min.js and js/calc_engine.js), when the
 *	system is built with the engine (make CALC_V8=1, the default with
 *	BROWSER=1), and otherwise a small evaluator of its own
 *	(calc_ceval.c) that knows the arithmetic, the comparisons and the
 *	functions of the table, and says #ERROR! for the rest.
 *
 *	The calls are plain C, in the types of both languages.
 */

#ifndef __CALC_ENG_H__
#define __CALC_ENG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Started once, before the first cell; 0, or below 0 when it could not be */
extern int  ce_start( void );
extern void ce_end( void );

/* Which engine this is ("V8 13.x.y", "C") */
extern const char *ce_name( void );

/* A cell given its text (NUL ended, UTF-8); the formulas naming it worked out again */
extern int  ce_set( int col, int row, const unsigned char *text );

/* A cell taken away; nothing worked out again */
extern int  ce_clear( int col, int row );

/* A cell made empty where it is, nothing worked out again (the cells of a join) */
extern int  ce_blank( int col, int row );

/* A row (cols 0) or a column put in before 'at' (d 1) or taken out (d -1): the cells after renamed */
extern int  ce_move( int cols, int at, int d );

/* Every cell taken away */
extern int  ce_reset( void );

/*
 * Many cells put in or taken out at once: from ce_bulk(1) on nothing is
 * worked out as each comes; at ce_bulk(0) every formula is worked out
 * once, in the light of all the others.
 */
extern int  ce_bulk( int on );

/* The cells whose words changed since last asked, told one by one; how many */
typedef void (*CE_FN)( int col, int row, const unsigned char *shown, int len, void *arg );
extern int  ce_taken( CE_FN fn, void *arg );

/*
 * Searching words: whether text holds pat (1, 0, or -1 for a regular
 * expression that is not one), and text with every pat in it replaced
 * (its length in out, NUL ended). A regular expression is understood
 * only by the V8 engine; the other searches the words as they are.
 * ce_replace writes as much as max holds and answers how long the whole
 * is, as snprintf does: an answer of max or more asks for more room.
 */
extern int  ce_test( const unsigned char *text, const unsigned char *pat, int regex );
extern int  ce_replace( const unsigned char *text, const unsigned char *pat, const unsigned char *rep,
			int regex, unsigned char *out, int max );

#ifdef __cplusplus
}
#endif

#endif /* __CALC_ENG_H__ */
