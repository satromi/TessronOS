/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	objson.c
 *	JSON in a buffer: enough to read and change the metadata of an
 *	object (design 18.9, 18.10, 18.15)
 *
 *	Values are found where they are and changed in place; the text
 *	around them is left byte for byte, so the keys a TADjs Desktop
 *	wrote and TessronOS does not know survive a change. Offsets are into
 *	the buffer; -1 is "not there" or "does not fit".
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/json.h>
#include <ts/tsfs.h>
#include "obj.h"

LOCAL BOOL is_ws( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\n' || c == '\r' );
}

EXPORT INT knl_oj_ws( CONST UB *j, INT len, INT p )
{
	while ( p < len && is_ws(j[p]) ) {
		p++;
	}
	return p;
}

/*
 * Reading is the system's JSON reader's (ts/json.h); what is here turns
 * its spans into the offsets the object layer keeps.
 */

/* Past one value, whatever it is */
EXPORT INT knl_oj_skip( CONST UB *j, INT len, INT p )
{
	T_JSON	v;

	if ( js_span(j, len, p, &v) < E_OK ) {
		return -1;
	}
	return (INT)( v.s - j ) + v.len;
}

EXPORT INT knl_oj_root( CONST UB *j, INT len )
{
	INT	p = knl_oj_ws(j, len, 0);

	return ( p < len && j[p] == '{' ) ? p : -1;
}

/* The value of a member of the object whose '{' is at obj */
EXPORT INT knl_oj_member( CONST UB *j, INT len, INT obj, CONST char *key )
{
	T_JSON	o, v;

	if ( obj < 0 || obj >= len || j[obj] != '{'
	  || js_span(j, len, obj, &o) < E_OK || js_get(&o, key, &v) < E_OK ) {
		return -1;
	}
	return (INT)( v.s - j );
}

/*
 * The elements of the array whose '[' is at arr, one at a time: *p_pos
 * starts as arr and is moved on by each call. Answers where the element
 * starts, or -1 after the last.
 */
EXPORT INT knl_oj_next( CONST UB *j, INT len, INT arr, INT *p_pos )
{
	T_JSON	a, it;

	if ( arr < 0 || arr >= len || j[arr] != '[' || js_span(j, len, arr, &a) < E_OK ) {
		return -1;
	}
	if ( *p_pos == arr ) {
		it.s = NULL;
		it.len = 0;
	} else {
		it.s = j + *p_pos;			/* just after the one before */
		it.len = 0;
	}
	if ( !js_next(&a, &it) ) {
		return -1;
	}
	*p_pos = (INT)( it.s - j ) + it.len;

	return (INT)( it.s - j );
}

/* A string value, unescaped and ending in 0: its length, or -1 */
EXPORT INT knl_oj_str( CONST UB *j, INT len, INT p, UB *out, INT max )
{
	T_JSON	v;
	INT	n;

	if ( p < 0 || p >= len || j[p] != '"' || js_span(j, len, p, &v) < E_OK ) {
		return -1;
	}
	n = js_str(&v, out, max);
	return ( n >= 0 ) ? n : -1;
}

EXPORT BOOL knl_oj_num( CONST UB *j, INT len, INT p, D *p_val )
{
	T_JSON	v;

	return (BOOL)( p >= 0 && p < len && js_span(j, len, p, &v) >= E_OK
		       && js_num(&v, p_val) >= E_OK );
}

EXPORT BOOL knl_oj_bool( CONST UB *j, INT len, INT p, BOOL *p_val )
{
	T_JSON	v;
	INT	t;

	if ( p < 0 || p >= len || js_span(j, len, p, &v) < E_OK ) {
		return FALSE;
	}
	t = js_type(&v);
	if ( t != JS_TRUE && t != JS_FALSE ) {
		return FALSE;
	}
	*p_val = (BOOL)( t == JS_TRUE );
	return TRUE;
}

/* old bytes at..at+olen replaced by nw; the new length, or -1 */
LOCAL INT splice( UB *j, INT len, INT max, INT at, INT olen, CONST UB *nw, INT nlen )
{
	INT	nl = len - olen + nlen, i;

	if ( nl >= max ) {
		return -1;
	}
	if ( nlen > olen ) {
		for ( i = len - 1; i >= at + olen; i-- ) {
			j[i + nlen - olen] = j[i];
		}
	} else if ( nlen < olen ) {
		for ( i = at + olen; i < len; i++ ) {
			j[i + nlen - olen] = j[i];
		}
	}
	for ( i = 0; i < nlen; i++ ) {
		j[at + i] = nw[i];
	}
	j[nl] = 0;

	return nl;
}

/* A member of the object at obj given the value val (JSON text), made if it is not there */
EXPORT INT knl_oj_set( UB *j, INT len, INT max, INT obj, CONST char *key,
		       CONST UB *val, INT vlen )
{
	INT	v, e, close, p, n = 0;
	BOOL	empty;
	UB	head[80];

	v = knl_oj_member(j, len, obj, key);
	if ( v >= 0 ) {
		e = knl_oj_skip(j, len, v);
		if ( e < 0 ) {
			return -1;
		}
		return splice(j, len, max, v, e - v, val, vlen);
	}
	e = knl_oj_skip(j, len, obj);
	if ( e < 0 ) {
		return -1;
	}
	close = e - 1;			/* its '}' */
	p = knl_oj_ws(j, len, obj + 1);
	empty = (BOOL)( p == close );

	if ( !empty ) head[n++] = ',';
	head[n++] = '"';
	for ( p = 0; key[p] != 0 && n < (INT)sizeof(head) - 3; p++ ) {
		head[n++] = (UB)key[p];
	}
	head[n++] = '"';
	head[n++] = ':';
	len = splice(j, len, max, close, 0, head, n);
	if ( len < 0 ) {
		return -1;
	}
	return splice(j, len, max, close + n, 0, val, vlen);
}

/* The value at k1, or at k1.k2 when k2 is given */
EXPORT INT knl_oj_path( CONST UB *j, INT len, CONST char *k1, CONST char *k2 )
{
	INT	v = knl_oj_member(j, len, knl_oj_root(j, len), k1);

	if ( v < 0 || k2 == NULL ) {
		return v;
	}
	return knl_oj_member(j, len, v, k2);
}

/* The value at k1 (or k1.k2) set, what is missing on the way made */
EXPORT INT knl_oj_set_path( UB *j, INT len, INT max, CONST char *k1,
			    CONST char *k2, CONST UB *val, INT vlen )
{
	INT	root, v1, n = 0, i;
	UB	*wrap;

	root = knl_oj_root(j, len);
	if ( root < 0 ) {
		if ( max < 3 ) {
			return -1;
		}
		j[0] = '{';  j[1] = '}';  j[2] = 0;
		len = 2;
		root = 0;
	}
	if ( k2 == NULL ) {
		return knl_oj_set(j, len, max, root, k1, val, vlen);
	}
	v1 = knl_oj_member(j, len, root, k1);
	if ( v1 >= 0 && j[v1] == '{' ) {
		return knl_oj_set(j, len, max, v1, k2, val, vlen);
	}
	/* {"k2":val} made and put at k1 */
	wrap = (UB *)Kmalloc((SZ)vlen + 80);
	if ( wrap == NULL ) {
		return -1;
	}
	wrap[n++] = '{';
	wrap[n++] = '"';
	for ( i = 0; k2[i] != 0 && i < 64; i++ ) {
		wrap[n++] = (UB)k2[i];
	}
	wrap[n++] = '"';
	wrap[n++] = ':';
	for ( i = 0; i < vlen; i++ ) {
		wrap[n++] = val[i];
	}
	wrap[n++] = '}';
	len = knl_oj_set(j, len, max, root, k1, wrap, n);
	Kfree(wrap);

	return len;
}

/* ---------------------------------------------------------------- building */

EXPORT INT knl_oj_put( UB *out, INT pos, INT max, CONST char *s )
{
	if ( pos < 0 ) {
		return -1;
	}
	while ( *s != 0 ) {
		if ( pos + 1 >= max ) {
			return -1;
		}
		out[pos++] = (UB)*s++;
	}
	out[pos] = 0;
	return pos;
}

EXPORT INT knl_oj_put_str( UB *out, INT pos, INT max, CONST UB *s )
{
	CONST char	*hex = "0123456789abcdef";

	pos = knl_oj_put(out, pos, max, "\"");
	for ( ; pos >= 0 && *s != 0; s++ ) {
		UB	c = *s;

		if ( c == '"' || c == '\\' ) {
			if ( pos + 2 >= max ) return -1;
			out[pos++] = '\\';
			out[pos++] = c;
		} else if ( c < 0x20 ) {
			if ( pos + 6 >= max ) return -1;
			out[pos++] = '\\';  out[pos++] = 'u';
			out[pos++] = '0';   out[pos++] = '0';
			out[pos++] = (UB)hex[c >> 4];
			out[pos++] = (UB)hex[c & 15];
		} else {
			if ( pos + 1 >= max ) return -1;
			out[pos++] = c;
		}
	}
	return knl_oj_put(out, pos, max, "\"");
}

EXPORT INT knl_oj_put_num( UB *out, INT pos, INT max, D v )
{
	char	t[24];
	INT	n = 0;
	BOOL	neg = (BOOL)( v < 0 );
	UD	u = neg ? (UD)-v : (UD)v;

	do {
		t[n++] = (char)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	if ( neg ) {
		t[n++] = '-';
	}
	while ( n > 0 && pos >= 0 ) {
		char	one[2] = { t[--n], 0 };

		pos = knl_oj_put(out, pos, max, one);
	}
	return pos;
}

EXPORT INT knl_oj_put_uuid( UB *out, INT pos, INT max, CONST TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];

	if ( ts_uuid_to_str(u, s, sizeof(s)) < E_OK ) {
		return -1;
	}
	return knl_oj_put_str(out, pos, max, (CONST UB *)s);
}

/* ---------------------------------------------------------------- a text made as large as it needs */

/*
 * The text fn makes, in a buffer as large as it needs: fn is called with
 * OJ_MAKE_FIRST bytes, and again with twice as many each time it answers
 * -1 (the knl_oj_put of a buffer too small), up to OJ_MAKE_LIMIT. Another
 * negative answer is an error and is passed on. Answers the length, with
 * the buffer in *p_buf (the caller Kfrees it), or an error.
 */
#define OJ_MAKE_FIRST	4096
#define OJ_MAKE_LIMIT	( 16 * 1024 * 1024 )

EXPORT INT knl_oj_make( OJ_MAKE fn, void *ctx, UB **p_buf )
{
	INT	max = OJ_MAKE_FIRST, n;
	UB	*t;

	*p_buf = NULL;
	for ( ;; ) {
		t = (UB *)Kmalloc((SZ)max);
		if ( t == NULL ) {
			return E_NOMEM;
		}
		n = fn(ctx, t, max);
		if ( n != -1 ) {
			break;
		}
		Kfree(t);
		if ( max >= OJ_MAKE_LIMIT ) {
			return E_LIMIT;
		}
		max *= 2;
	}
	if ( n < 0 ) {
		Kfree(t);
		return n;
	}
	*p_buf = t;
	return n;
}

/*
 * Metadata read whole, however long: get reads from the start, copying
 * at most size bytes and saying how many it copied; it is asked again
 * with twice the room while it fills what it was given. The buffer has
 * room bytes more after the text and a nought after that. Answers the
 * length, the buffer in *p_buf (Kfree it) and the bytes it holds in
 * *p_cap, or an error.
 */
#define META_ALL_FIRST	4096
#define META_ALL_LIMIT	OB_META_LIMIT

EXPORT INT knl_meta_all( FP_META_GET get, void *ctx, INT room, UB **p_buf, INT *p_cap )
{
	UB	*b;
	SZ	cap = META_ALL_FIRST, asz;
	ER	er;

	*p_buf = NULL;
	if ( room < 0 ) {
		return E_PAR;
	}
	for ( ;; ) {
		b = (UB *)Kmalloc(cap + room + 1);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		asz = 0;
		er = get(ctx, b, cap, &asz);
		if ( er < E_OK ) {
			Kfree(b);
			return (INT)er;
		}
		if ( asz < cap ) {
			break;
		}
		Kfree(b);
		if ( cap >= META_ALL_LIMIT ) {
			return E_LIMIT;
		}
		cap *= 2;
	}
	b[asz] = 0;
	*p_buf = b;
	if ( p_cap != NULL ) *p_cap = (INT)( cap + room );
	return (INT)asz;
}

typedef struct {
	ID		vol;
	CONST TS_UUID	*uuid;
} TSMETA;

LOCAL ER tsfs_meta_get( void *ctx, UB *buf, SZ size, SZ *p_asize )
{
	TSMETA	*t = (TSMETA *)ctx;
	INT	n = 0;
	ER	er = knl_tsfs_get_meta(t->vol, t->uuid, buf, (INT)size, &n);

	*p_asize = ( er >= E_OK ) ? (SZ)n : 0;
	return er;
}

EXPORT INT knl_tsfs_meta_all( ID vol, CONST TS_UUID *uuid, INT room, UB **p_buf, INT *p_cap )
{
	TSMETA	t;

	t.vol = vol;
	t.uuid = uuid;
	return knl_meta_all(tsfs_meta_get, &t, room, p_buf, p_cap);
}

/* Bytes off.. of the text fn makes, as a record is read: E_OK, or the error it made */
EXPORT ER knl_oj_make_rea( OJ_MAKE fn, void *ctx, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	INT	len;
	SZ	n = 0;

	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	len = knl_oj_make(fn, ctx, &t);
	if ( len < 0 ) {
		return (ER)len;
	}
	if ( off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

/* The length of the text fn makes, 0 when it cannot be made */
EXPORT UD knl_oj_make_len( OJ_MAKE fn, void *ctx )
{
	UB	*t;
	INT	len = knl_oj_make(fn, ctx, &t);

	if ( len < 0 ) {
		return 0;
	}
	Kfree(t);
	return (UD)len;
}
