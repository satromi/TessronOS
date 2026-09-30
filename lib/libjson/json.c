/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	json.c
 *	JSON reader (design 18.15)
 *
 *	Values are spans of the text (json.h). skip() finds where a value
 *	ends; it is the one routine that knows the grammar, and js_parse is
 *	skip() over the whole text with the checking turned on.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/json.h>

#define JS_DEPTH	32		/* nesting js_parse accepts */

LOCAL BOOL is_space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\n' || c == '\r' );
}

LOCAL INT blank( CONST UB *s, INT at, INT end )
{
	while ( at < end && is_space(s[at]) ) at++;
	return at;
}

LOCAL BOOL same( CONST UB *s, INT at, INT end, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( at + i >= end || s[at + i] != (UB)w[i] ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* The end of the string that starts at s[at] ('"'), past its closing quote */
LOCAL INT skip_str( CONST UB *s, INT at, INT end )
{
	for ( at++; at < end; at++ ) {
		if ( s[at] == '\\' ) {
			at++;
		} else if ( s[at] == '"' ) {
			return at + 1;
		} else if ( s[at] < 0x20 ) {
			return -1;			/* a control character inside */
		}
	}
	return -1;
}

LOCAL INT skip_num( CONST UB *s, INT at, INT end )
{
	INT	from = at;

	if ( at < end && s[at] == '-' ) at++;
	while ( at < end && ( ( s[at] >= '0' && s[at] <= '9' ) || s[at] == '.'
			   || s[at] == 'e' || s[at] == 'E' || s[at] == '+' || s[at] == '-' ) ) {
		at++;
	}
	return ( at > from && s[at - 1] != '-' ) ? at : -1;
}

/*
 * The end of the value that starts at s[at] (no blank before it), or -1
 * when it is not a value. depth counts the nesting left to allow.
 */
LOCAL INT skip( CONST UB *s, INT at, INT end, INT depth )
{
	UB	close;
	BOOL	obj;

	if ( at >= end ) {
		return -1;
	}
	switch ( s[at] ) {
	case '"':
		return skip_str(s, at, end);
	case 't':
		return same(s, at, end, "true") ? at + 4 : -1;
	case 'f':
		return same(s, at, end, "false") ? at + 5 : -1;
	case 'n':
		return same(s, at, end, "null") ? at + 4 : -1;
	case '{':
	case '[':
		break;
	default:
		return skip_num(s, at, end);
	}
	if ( depth <= 0 ) {
		return -1;
	}
	obj = (BOOL)( s[at] == '{' );
	close = obj ? '}' : ']';
	at = blank(s, at + 1, end);
	if ( at < end && s[at] == close ) {
		return at + 1;
	}
	for ( ;; ) {
		if ( obj ) {
			if ( at >= end || s[at] != '"' ) {
				return -1;
			}
			at = blank(s, skip_str(s, at, end), end);
			if ( at < 0 || at >= end || s[at] != ':' ) {
				return -1;
			}
			at = blank(s, at + 1, end);
		}
		at = skip(s, at, end, depth - 1);
		if ( at < 0 ) {
			return -1;
		}
		at = blank(s, at, end);
		if ( at >= end ) {
			return -1;
		}
		if ( s[at] == close ) {
			return at + 1;
		}
		if ( s[at] != ',' ) {
			return -1;
		}
		at = blank(s, at + 1, end);
	}
}

EXPORT ER js_parse( CONST UB *text, INT len, T_JSON *root )
{
	INT	at, e;

	if ( text == NULL || root == NULL || len <= 0 ) {
		return E_PAR;
	}
	at = blank(text, 0, len);
	e = skip(text, at, len, JS_DEPTH);
	if ( e < 0 || blank(text, e, len) != len ) {
		return E_PAR;
	}
	root->s = text + at;
	root->len = e - at;

	return E_OK;
}

EXPORT ER js_span( CONST UB *text, INT len, INT at, T_JSON *v )
{
	INT	e;

	if ( text == NULL || v == NULL || at < 0 || at >= len ) {
		return E_PAR;
	}
	at = blank(text, at, len);
	e = skip(text, at, len, JS_DEPTH);
	if ( e < 0 ) {
		return E_PAR;
	}
	v->s = text + at;
	v->len = e - at;

	return E_OK;
}

EXPORT INT js_type( CONST T_JSON *v )
{
	if ( v == NULL || v->s == NULL || v->len <= 0 ) {
		return JS_NONE;
	}
	switch ( v->s[0] ) {
	case '{':	return JS_OBJECT;
	case '[':	return JS_ARRAY;
	case '"':	return JS_STRING;
	case 't':	return JS_TRUE;
	case 'f':	return JS_FALSE;
	case 'n':	return JS_NULL;
	default:	return JS_NUMBER;
	}
}

/*
 * The next member of an object or element of an array after position
 * at (just inside the opening bracket, or just after a value). Answers
 * where the value starts, and its name's span for a member.
 */
LOCAL INT step( CONST T_JSON *c, INT at, INT *p_name, INT *p_nlen )
{
	CONST UB *s = c->s;
	INT	end = c->len - 1;			/* the closing bracket */
	INT	e;

	at = blank(s, at, end);
	if ( at < end && s[at] == ',' ) {
		at = blank(s, at + 1, end);
	}
	if ( at >= end ) {
		return -1;
	}
	if ( s[0] == '{' ) {
		e = skip_str(s, at, end);
		if ( e < 0 ) {
			return -1;
		}
		*p_name = at + 1;
		*p_nlen = e - at - 2;
		at = blank(s, e, end);
		if ( at >= end || s[at] != ':' ) {
			return -1;
		}
		at = blank(s, at + 1, end);
	}
	return at;
}

EXPORT ER js_get( CONST T_JSON *obj, CONST char *name, T_JSON *val )
{
	INT	at = 1, nm = 0, nl = 0, e, i;

	if ( js_type(obj) != JS_OBJECT || name == NULL || val == NULL ) {
		return E_PAR;
	}
	while ( ( at = step(obj, at, &nm, &nl) ) >= 0 ) {
		e = skip(obj->s, at, obj->len - 1, JS_DEPTH);
		if ( e < 0 ) {
			break;
		}
		for ( i = 0; i < nl && name[i] != 0 && obj->s[nm + i] == (UB)name[i]; i++ ) ;
		if ( i == nl && name[i] == 0 ) {
			val->s = obj->s + at;
			val->len = e - at;
			return E_OK;
		}
		at = e;
	}
	return E_NOEXS;
}

EXPORT BOOL js_next( CONST T_JSON *arr, T_JSON *it )
{
	INT	t = js_type(arr), at, nm = 0, nl = 0, e;

	if ( ( t != JS_ARRAY && t != JS_OBJECT ) || it == NULL ) {
		return FALSE;
	}
	at = ( it->s == NULL ) ? 1 : (INT)( it->s - arr->s ) + it->len;
	at = step(arr, at, &nm, &nl);
	if ( at < 0 ) {
		return FALSE;
	}
	e = skip(arr->s, at, arr->len - 1, JS_DEPTH);
	if ( e < 0 ) {
		return FALSE;
	}
	it->s = arr->s + at;
	it->len = e - at;

	return TRUE;
}

EXPORT INT js_count( CONST T_JSON *arr )
{
	T_JSON	it;
	INT	n = 0;

	it.s = NULL;
	it.len = 0;
	while ( js_next(arr, &it) ) n++;
	return n;
}

LOCAL INT hex4( CONST UB *s )
{
	INT	v = 0, i;

	for ( i = 0; i < 4; i++ ) {
		UB	c = s[i];

		v <<= 4;
		if ( c >= '0' && c <= '9' ) v |= c - '0';
		else if ( c >= 'a' && c <= 'f' ) v |= c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

/* A character as UTF-8 into out[n..]; answers the new n, or -1 when it does not fit */
LOCAL INT put_utf8( UB *out, INT n, INT max, UW c )
{
	INT	k = ( c < 0x80 ) ? 1 : ( c < 0x800 ) ? 2 : ( c < 0x10000 ) ? 3 : 4;

	if ( n + k >= max ) {
		return -1;
	}
	switch ( k ) {
	case 1:
		out[n] = (UB)c;
		break;
	case 2:
		out[n] = (UB)( 0xC0 | ( c >> 6 ) );
		out[n + 1] = (UB)( 0x80 | ( c & 0x3F ) );
		break;
	case 3:
		out[n] = (UB)( 0xE0 | ( c >> 12 ) );
		out[n + 1] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
		out[n + 2] = (UB)( 0x80 | ( c & 0x3F ) );
		break;
	default:
		out[n] = (UB)( 0xF0 | ( c >> 18 ) );
		out[n + 1] = (UB)( 0x80 | ( ( c >> 12 ) & 0x3F ) );
		out[n + 2] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
		out[n + 3] = (UB)( 0x80 | ( c & 0x3F ) );
		break;
	}
	return n + k;
}

EXPORT INT js_str( CONST T_JSON *v, UB *out, INT max )
{
	CONST UB *s;
	INT	i, end, n = 0, k;

	if ( js_type(v) != JS_STRING || out == NULL || max <= 0 ) {
		return E_PAR;
	}
	s = v->s;
	end = v->len - 1;				/* the closing quote */
	for ( i = 1; i < end; i++ ) {
		UW	c = s[i];

		if ( c == '\\' && i + 1 < end ) {
			i++;
			switch ( s[i] ) {
			case 'n':	c = '\n';	break;
			case 't':	c = '\t';	break;
			case 'r':	c = '\r';	break;
			case 'b':	c = '\b';	break;
			case 'f':	c = '\f';	break;
			case 'u':
				if ( i + 4 >= end || ( k = hex4(s + i + 1) ) < 0 ) {
					out[n] = 0;
					return E_PAR;
				}
				c = (UW)k;
				i += 4;
				/* a pair of surrogates is one character */
				if ( c >= 0xD800 && c < 0xDC00 && i + 6 < end
				  && s[i + 1] == '\\' && s[i + 2] == 'u'
				  && ( k = hex4(s + i + 3) ) >= 0xDC00 && k < 0xE000 ) {
					c = 0x10000 + ( ( c - 0xD800 ) << 10 ) + ( (UW)k - 0xDC00 );
					i += 6;
				}
				break;
			default:	c = s[i];	break;	/* \" \\ \/ */
			}
			k = put_utf8(out, n, max, c);
		} else {
			k = ( n + 1 < max ) ? n + 1 : -1;
			if ( k > 0 ) out[n] = (UB)c;
		}
		if ( k < 0 ) {
			out[n] = 0;
			return E_LIMIT;
		}
		n = k;
	}
	out[n] = 0;

	return n;
}

EXPORT ER js_num( CONST T_JSON *v, D *p_num )
{
	CONST UB *s;
	INT	i = 0;
	BOOL	neg = FALSE;
	D	x = 0;

	if ( js_type(v) != JS_NUMBER || p_num == NULL ) {
		return E_PAR;
	}
	s = v->s;
	if ( s[0] == '-' ) {
		neg = TRUE;
		i = 1;
	}
	for ( ; i < v->len && s[i] >= '0' && s[i] <= '9'; i++ ) {
		x = x * 10 + ( s[i] - '0' );
	}
	*p_num = neg ? -x : x;

	return E_OK;
}

EXPORT BOOL js_bool( CONST T_JSON *v, BOOL dflt )
{
	switch ( js_type(v) ) {
	case JS_TRUE:	return TRUE;
	case JS_FALSE:	return FALSE;
	default:	return dflt;
	}
}

EXPORT INT js_get_str( CONST T_JSON *obj, CONST char *name, UB *out, INT max )
{
	T_JSON	v;

	if ( js_get(obj, name, &v) < E_OK ) {
		if ( out != NULL && max > 0 ) out[0] = 0;
		return E_NOEXS;
	}
	return js_str(&v, out, max);
}

EXPORT D js_get_num( CONST T_JSON *obj, CONST char *name, D dflt )
{
	T_JSON	v;
	D	x;

	if ( js_get(obj, name, &v) < E_OK || js_num(&v, &x) < E_OK ) {
		return dflt;
	}
	return x;
}

EXPORT BOOL js_get_bool( CONST T_JSON *obj, CONST char *name, BOOL dflt )
{
	T_JSON	v;

	if ( js_get(obj, name, &v) < E_OK ) {
		return dflt;
	}
	return js_bool(&v, dflt);
}
