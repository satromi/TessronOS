/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	json.h
 *	JSON reader (design 18.15)
 *
 *	A value is a span of the text: where it starts and how many bytes
 *	it covers. Nothing is copied or allocated while reading; members and
 *	elements are found by walking the text, and a string is unescaped
 *	only when it is taken out with js_str.
 *
 *	js_parse checks the whole text once. The walking calls after it
 *	stop at the end of the span they are given and answer "not there"
 *	on anything they cannot read, so a span from js_parse or js_span is
 *	safe to walk.
 *
 *	This is the one JSON reader of the system: the kernel's layers use
 *	it, and a program links the same file (lib/libjson).
 */

#ifndef __TS_JSON_H__
#define __TS_JSON_H__

#ifdef __cplusplus
extern "C" {
#endif

/* What a value is */
#define JS_NONE		0
#define JS_OBJECT	1
#define JS_ARRAY	2
#define JS_STRING	3
#define JS_NUMBER	4
#define JS_TRUE		5
#define JS_FALSE	6
#define JS_NULL		7

typedef struct {
	CONST UB	*s;		/* first byte of the value */
	INT		len;		/* bytes it covers */
} T_JSON;

/* The value the whole text is, after checking that it is well formed */
IMPORT ER   js_parse( CONST UB *text, INT len, T_JSON *root );

/*
 * The value that starts at an offset of a text (blanks before it are
 * passed over), without looking at the rest of the text. For callers
 * that keep offsets into a buffer rather than spans.
 */
IMPORT ER   js_span( CONST UB *text, INT len, INT at, T_JSON *v );

IMPORT INT  js_type( CONST T_JSON *v );

/* A member of an object by name; E_NOEXS when there is none */
IMPORT ER   js_get( CONST T_JSON *obj, CONST char *name, T_JSON *val );

/*
 * The elements of an array (or the values of an object's members) one
 * after another: start with it->s == NULL; answers FALSE after the last.
 */
IMPORT BOOL js_next( CONST T_JSON *arr, T_JSON *it );
IMPORT INT  js_count( CONST T_JSON *arr );

/*
 * A string's text, unescaped into UTF-8 and ended with 0. Answers its
 * length, E_PAR when the value is not a string, E_LIMIT when it does not
 * fit (what fits is written).
 */
IMPORT INT  js_str( CONST T_JSON *v, UB *out, INT max );

/* A number's value, whole part only; E_PAR when it is not a number */
IMPORT ER   js_num( CONST T_JSON *v, D *p_num );

/* true or false; the default for anything else */
IMPORT BOOL js_bool( CONST T_JSON *v, BOOL dflt );

/* A member taken out at once: answers the default when it is missing */
IMPORT INT  js_get_str( CONST T_JSON *obj, CONST char *name, UB *out, INT max );
IMPORT D    js_get_num( CONST T_JSON *obj, CONST char *name, D dflt );
IMPORT BOOL js_get_bool( CONST T_JSON *obj, CONST char *name, BOOL dflt );

#ifdef __cplusplus
}
#endif

#endif /* __TS_JSON_H__ */
