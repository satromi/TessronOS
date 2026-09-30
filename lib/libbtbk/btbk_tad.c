/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btbk_tad.c
 *	xmlTAD written as binary TAD (design 11.19.6)
 *
 *	The XML is read as a run of tokens -- start tags, end tags and the
 *	text between -- straight out of the caller's buffer, and each is
 *	written as it comes: an element opens its segments, its end tag
 *	closes them or puts back what it changed. What a text or a figure
 *	needs before its first word (its view, drawing area and units) is
 *	looked for ahead in the text when it opens.
 *
 *	The elements are those lib/libbpk makes of the segments, so each is
 *	written back as the segment it came from, and a record taken through
 *	xmlTAD and back gives the same xmlTAD. Where XML had to close and
 *	open again what the record did not nest (a decoration round the end
 *	of a paragraph, the look of the letters round a change of weight),
 *	the ranges are written only when letters come, and so come out as
 *	they were.
 *
 *	Segments are little-endian words: id, length of the body in bytes
 *	(0xFFFF and a 32-bit length for a large one), then the body, whose
 *	first word of a text or figure attribute segment is subid << 8 |
 *	attr. Characters are TRON code words between them.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/btbk.h>
#include <ts/bpk.h>

/* What an open element is */
#define LV_OTHER	0
#define LV_TEXT		1		/* <document>: a TS_TEXT */
#define LV_FIG		2		/* <figure>: a TS_FIG */
#define LV_P		3
#define LV_FONT		4		/* the letters put back at its end */
#define LV_STYLE	5		/* a decoration of the letters */
#define LV_GROUP	6
#define LV_FATR		7		/* the font's look put back at its end */
#define LV_TBOX		8		/* a rectangle holding text: a TS_TEXT */
#define LV_SKIP		9		/* nothing inside is written */
#define LV_OVL		10		/* <paper-overlay-define>: a text inside a segment */

/* Tokens */
#define TK_EOF		0
#define TK_START	1
#define TK_EMPTY	2
#define TK_END		3
#define TK_TEXT		4
#define TK_CDATA	5

typedef struct {
	INT		kind;
	CONST UB	*p;		/* tag: after '<' up to '>' or "/>"; text: itself */
	INT		n;
	CONST UB	*name;
	INT		nlen;
} TOK;

#define DEF_UNIT	(-72)		/* units of a text that does not say */
#define DEF_SIZE	( 0x8000 | 14 * 20 )	/* 14 point, when nothing said otherwise */
#define BLACK		0x10000000U
#define WHITE		0x10FFFFFFU
#define CLEAR		0x80000000U
#define FCLASS		0x60C6		/* the font class a TT_FONT carries */

#define PEND_NL		1		/* waiting: a new paragraph */
#define PEND_LINK	2		/* waiting: a link */

#define COL_NONE	0		/* colour attribute absent */
#define COL_SET		1
#define COL_CLEAR	2

/* VLINK attr */
#define V_NONAME	0x0001
#define V_NORELN	0x0002
#define V_NOTYPE	0x0004
#define V_NOTIME	0x0008
#define V_NOPICT	0x0040
#define V_NOFDISP	0x0080
#define V_AUTEXE	0x4000

/* ------------------------------------------------------------------ */
/* Reading the XML */

LOCAL BOOL is_space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

LOCAL BOOL same( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( w[i] == 0 || s[i] != (UB)w[i] ) return FALSE;
	}
	return (BOOL)( w[n] == 0 );
}

LOCAL BOOL starts( CONST UB *s, INT at, INT len, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( at + i >= len || s[at + i] != (UB)w[i] ) return FALSE;
	}
	return TRUE;
}

/* Where `w` next starts at or after `at`, or -1 */
LOCAL INT find( CONST UB *s, INT at, INT len, CONST char *w )
{
	for ( ; at < len; at++ ) {
		if ( starts(s, at, len, w) ) return at;
	}
	return -1;
}

/* The next token: its kind, E_PAR for text that is not XML */
LOCAL INT token( CONST UB *s, INT len, INT *p_at, TOK *t )
{
	INT	at = *p_at, e, q;

	for ( ;; ) {
		if ( at >= len ) {
			*p_at = at;
			return t->kind = TK_EOF;
		}
		if ( s[at] != '<' ) {
			e = at;
			while ( e < len && s[e] != '<' ) e++;
			t->p = s + at;
			t->n = e - at;
			*p_at = e;
			return t->kind = TK_TEXT;
		}
		if ( starts(s, at, len, "<!--") ) {
			e = find(s, at + 4, len, "-->");
			if ( e < 0 ) return E_PAR;
			at = e + 3;
			continue;
		}
		if ( starts(s, at, len, "<![CDATA[") ) {
			e = find(s, at + 9, len, "]]>");
			if ( e < 0 ) return E_PAR;
			t->p = s + at + 9;
			t->n = e - ( at + 9 );
			*p_at = e + 3;
			return t->kind = TK_CDATA;
		}
		if ( starts(s, at, len, "<?") ) {
			e = find(s, at + 2, len, "?>");
			if ( e < 0 ) return E_PAR;
			at = e + 2;
			continue;
		}
		if ( starts(s, at, len, "<!") ) {
			e = find(s, at + 2, len, ">");
			if ( e < 0 ) return E_PAR;
			at = e + 1;
			continue;
		}
		break;
	}

	/* a tag: find its '>' outside quotes */
	q = 0;
	for ( e = at + 1; e < len; e++ ) {
		if ( q != 0 ) {
			if ( s[e] == q ) q = 0;
		} else if ( s[e] == '"' || s[e] == '\'' ) {
			q = s[e];
		} else if ( s[e] == '>' ) {
			break;
		}
	}
	if ( e >= len ) return E_PAR;
	*p_at = e + 1;
	if ( s[at + 1] == '/' ) {
		t->kind = TK_END;
		t->p = s + at + 2;
		t->n = e - ( at + 2 );
	} else if ( s[e - 1] == '/' ) {
		t->kind = TK_EMPTY;
		t->p = s + at + 1;
		t->n = e - 1 - ( at + 1 );
	} else {
		t->kind = TK_START;
		t->p = s + at + 1;
		t->n = e - ( at + 1 );
	}
	t->name = t->p;
	for ( t->nlen = 0; t->nlen < t->n && !is_space(t->p[t->nlen])
				&& t->p[t->nlen] != '/'; t->nlen++ ) ;
	if ( t->nlen == 0 ) return E_PAR;
	return t->kind;
}

/* One entity at s[0] ('&'): the character and how far it went; 0 when it is not one */
LOCAL INT entity( CONST UB *s, INT n, UINT *p_cp )
{
	INT	i;
	UINT	v = 0;

	for ( i = 1; i < n && i < 12 && s[i] != ';'; i++ ) ;
	if ( i >= n || s[i] != ';' ) return 0;
	if ( same(s + 1, i - 1, "amp") ) *p_cp = '&';
	else if ( same(s + 1, i - 1, "lt") ) *p_cp = '<';
	else if ( same(s + 1, i - 1, "gt") ) *p_cp = '>';
	else if ( same(s + 1, i - 1, "quot") ) *p_cp = '"';
	else if ( same(s + 1, i - 1, "apos") ) *p_cp = '\'';
	else if ( i > 2 && s[1] == '#' ) {
		INT	k = 2;
		BOOL	hex = ( s[2] == 'x' || s[2] == 'X' );

		if ( hex ) k++;
		for ( ; k < i; k++ ) {
			UB	c = s[k];

			if ( c >= '0' && c <= '9' ) v = v * ( hex ? 16 : 10 ) + ( c - '0' );
			else if ( hex && c >= 'a' && c <= 'f' ) v = v * 16 + ( c - 'a' + 10 );
			else if ( hex && c >= 'A' && c <= 'F' ) v = v * 16 + ( c - 'A' + 10 );
			else return 0;
		}
		*p_cp = v;
	} else {
		return 0;
	}
	return i + 1;
}

/* One character of UTF-8 (entities undone when ent): how far it went */
LOCAL INT next_cp( CONST UB *s, INT n, BOOL ent, UINT *p_cp )
{
	UINT	c = s[0];
	INT	k, i;

	if ( ent && c == '&' ) {
		k = entity(s, n, p_cp);
		if ( k > 0 ) return k;
	}
	if ( c < 0x80 ) {
		*p_cp = c;
		return 1;
	}
	k = ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 0;
	if ( k == 0 || k > n ) {
		*p_cp = UC_GETA;
		return 1;
	}
	*p_cp = c & ( 0x3F >> ( k - 1 ) );
	for ( i = 1; i < k; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) {
			*p_cp = UC_GETA;
			return i;
		}
		*p_cp = ( *p_cp << 6 ) | ( s[i] & 0x3F );
	}
	return k;
}

EXPORT INT bk_tad_attr( CONST UB *tag, INT n, CONST char *name, UB *buf, INT max )
{
	INT	at = 0, a, alen, v, e, k;
	UB	q;

	while ( at < n && !is_space(tag[at]) && tag[at] != '/' ) at++;	/* the element's name */
	for ( ;; ) {
		while ( at < n && is_space(tag[at]) ) at++;
		if ( at >= n || tag[at] == '/' ) return -1;
		a = at;
		while ( at < n && !is_space(tag[at]) && tag[at] != '=' ) at++;
		alen = at - a;
		while ( at < n && is_space(tag[at]) ) at++;
		if ( at >= n || tag[at] != '=' ) {
			continue;		/* a name without a value */
		}
		at++;
		while ( at < n && is_space(tag[at]) ) at++;
		if ( at >= n || ( tag[at] != '"' && tag[at] != '\'' ) ) return -1;
		q = tag[at++];
		v = at;
		while ( at < n && tag[at] != q ) at++;
		e = at;
		at++;
		if ( !same(tag + a, alen, name) ) continue;

		/* the value, its entities undone, as UTF-8 */
		for ( k = 0; v < e; ) {
			UINT	cp;
			INT	d = next_cp(tag + v, e - v, TRUE, &cp);
			char	o[4];
			INT	m = bpk_utf8(cp, o), j;

			if ( tag[v] != '&' || d == 1 ) {
				/* not an entity: the bytes as they are */
				m = d;
				for ( j = 0; j < m; j++ ) o[j] = (char)tag[v + j];
			}
			for ( j = 0; j < m; j++ ) {
				if ( k + 1 < max ) buf[k] = (UB)o[j];
				k++;
			}
			v += d;
		}
		if ( max > 0 ) buf[( k < max ) ? k : max - 1] = 0;
		return k;
	}
}

/* An attribute as a number in thousandths, rounded; `def` when absent */
LOCAL INT num1000( CONST TOK *t, CONST char *name, INT def )
{
	UB	b[32];
	INT	i = 0, v = 0, f = 0, fd = 0, sign = 1;

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return def;
	while ( is_space(b[i]) ) i++;
	if ( b[i] == '-' ) { sign = -1; i++; }
	else if ( b[i] == '+' ) i++;
	if ( ( b[i] < '0' || b[i] > '9' ) && b[i] != '.' ) return def;
	while ( b[i] >= '0' && b[i] <= '9' ) {
		if ( v < 100000000 ) v = v * 10 + ( b[i] - '0' );
		i++;
	}
	if ( b[i] == '.' ) {
		i++;
		while ( b[i] >= '0' && b[i] <= '9' ) {
			if ( fd < 4 ) { f = f * 10 + ( b[i] - '0' ); fd++; }
			i++;
		}
	}
	while ( fd < 4 ) { f *= 10; fd++; }
	return sign * ( v * 1000 + ( f + 5 ) / 10 );
}

LOCAL INT num( CONST TOK *t, CONST char *name, INT def )
{
	INT	v = num1000(t, name, def * 1000);

	return ( v >= 0 ) ? ( v + 500 ) / 1000 : -( ( -v + 500 ) / 1000 );
}

LOCAL BOOL has( CONST TOK *t, CONST char *name )
{
	UB	b[2];

	return (BOOL)( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) >= 0 );
}

/* "true" or "false"; `def` when absent */
LOCAL BOOL flag( CONST TOK *t, CONST char *name, BOOL def )
{
	UB	b[8];

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return def;
	return (BOOL)( b[0] == 't' || b[0] == '1' );
}

LOCAL INT hexv( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* "#rrggbb", "#rgb", "transparent" or "none" as a COLOR */
LOCAL INT colour_of( CONST UB *b, UINT *p_col )
{
	INT	i, h[6];

	if ( b[0] == 't' || b[0] == 'n' ) {
		*p_col = CLEAR;
		return COL_CLEAR;
	}
	if ( b[0] != '#' ) return COL_NONE;
	for ( i = 0; i < 6 && b[1 + i] != 0; i++ ) {
		h[i] = hexv(b[1 + i]);
		if ( h[i] < 0 ) return COL_NONE;
	}
	if ( i == 3 ) {
		*p_col = 0x10000000U | ( (UINT)( h[0] * 17 ) << 16 )
			| ( (UINT)( h[1] * 17 ) << 8 ) | (UINT)( h[2] * 17 );
		return COL_SET;
	}
	if ( i != 6 ) return COL_NONE;
	*p_col = 0x10000000U | ( (UINT)( h[0] << 4 | h[1] ) << 16 )
		| ( (UINT)( h[2] << 4 | h[3] ) << 8 ) | (UINT)( h[4] << 4 | h[5] );
	return COL_SET;
}

LOCAL INT colour( CONST TOK *t, CONST char *name, UINT *p_col )
{
	UB	b[16];

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return COL_NONE;
	return colour_of(b, p_col);
}

/* How many attributes a tag has */
LOCAL INT nattr( CONST TOK *t )
{
	INT	i, n = 0;
	BOOL	q = FALSE;

	for ( i = 0; i < t->n; i++ ) {
		if ( t->p[i] == '"' ) q = !q;
		else if ( t->p[i] == '=' && !q ) n++;
	}
	return n;
}

/* Where an attribute's value is in the tag, and its length; NULL without it */
LOCAL CONST UB *attr_at( CONST TOK *t, CONST char *name, INT *p_n )
{
	INT	i, k, e;

	for ( k = 0; name[k] != 0; k++ ) ;
	for ( i = t->nlen; i + k + 2 <= t->n; i++ ) {
		if ( !is_space(t->p[i - 1]) || !starts(t->p, i, t->n, name) || t->p[i + k] != '='
		  || ( t->p[i + k + 1] != '"' && t->p[i + k + 1] != '\'' ) ) continue;
		for ( e = i + k + 2; e < t->n && t->p[e] != t->p[i + k + 1]; e++ ) ;
		*p_n = e - ( i + k + 2 );
		return t->p + i + k + 2;
	}
	*p_n = 0;
	return NULL;
}

/* A number written "0x12" or "12" */
LOCAL INT hexnum( CONST TOK *t, CONST char *name )
{
	UB	b[16];
	INT	i = 0, v = 0, h;

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return 0;
	if ( b[0] == '0' && ( b[1] == 'x' || b[1] == 'X' ) ) i = 2;
	for ( ; ( h = hexv(b[i]) ) >= 0; i++ ) v = v * 16 + h;
	return v;
}

/* The code of a letter and the mark after it that have a place together, 0 none */
LOCAL UINT seq_code( UINT a, UINT b )
{
	INT	i;

	if ( b < 0x0300 ) return 0;
	for ( i = 0; i < bpk_tron_nseq; i++ ) {
		if ( bpk_tron_seq[i][0] == a && bpk_tron_seq[i][1] == b ) return bpk_tron_seq[i][2];
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Writing */

LOCAL void put16( BK_TADW *w, UINT v )
{
	if ( w->out != NULL && w->len + 2 <= w->max ) {
		w->out[w->len] = (UB)v;
		w->out[w->len + 1] = (UB)( v >> 8 );
	}
	w->len += 2;
}

LOCAL void put32( BK_TADW *w, UINT v )
{
	put16(w, v & 0xFFFF);
	put16(w, v >> 16);
}

LOCAL void put8( BK_TADW *w, UINT v, INT at )
{
	if ( w->out != NULL && at < w->max ) w->out[at] = (UB)v;
}

/* The bytes of a hexadecimal attribute; how many */
LOCAL INT hex_put( BK_TADW *w, CONST TOK *t, CONST char *name, BOOL put )
{
	INT	n, i, k = 0, a, b;
	CONST UB *v = attr_at(t, name, &n);

	for ( i = 0; v != NULL && i + 1 < n; i += 2 ) {
		a = hexv(v[i]);
		b = hexv(v[i + 1]);
		if ( a < 0 || b < 0 ) break;
		if ( put ) {
			put8(w, (UINT)( a * 16 + b ), w->len);
			w->len++;
		}
		k++;
	}
	return k;
}

/* A segment's head, for a body of `body` bytes; nothing done before it */
LOCAL void seg_raw( BK_TADW *w, UINT id, INT body )
{
	put16(w, id);
	if ( body >= 0xFFFF ) {
		put16(w, 0xFFFF);
		put32(w, (UINT)body);
	} else {
		put16(w, (UINT)body);
	}
}

LOCAL void deco_flush( BK_TADW *w );

/* A segment's head, after the decorations the text has come into */
LOCAL void seg( BK_TADW *w, UINT id, INT body )
{
	deco_flush(w);
	seg_raw(w, id, body);
}

/* A text or figure attribute segment of `body` bytes, its first word written */
LOCAL void fusen( BK_TADW *w, UINT id, UINT sub, UINT attr, INT body )
{
	seg(w, id, body);
	put16(w, ( sub << 8 ) | ( attr & 0xFF ));
}

LOCAL void plane_to( BK_TADW *w, INT plane )
{
	if ( w->plane != plane ) {
		put16(w, 0xFE21 + plane - 1);
		w->plane = plane;
	}
}

/* TRON characters of a UTF-8 string, into tc[max]; how many. The plane
   goes back to 1 at the end */
LOCAL INT tc_of( CONST UB *s, INT n, UH *tc, INT max )
{
	INT	k = 0, m = 0, plane = 1;

	while ( k < n && m + 2 < max ) {
		UINT	cp, nx, code;
		INT	d = next_cp(s + k, n - k, FALSE, &cp);

		if ( k + d < n && ( next_cp(s + k + d, n - k - d, FALSE, &nx), code = seq_code(cp, nx) ) != 0 ) {
			if ( plane != 1 ) { tc[m++] = 0xFE21; plane = 1; }
			tc[m++] = (UH)code;
			k += d;
			k += next_cp(s + k, n - k, FALSE, &nx);
			continue;
		}
		if ( cp >= 0x20 ) {
			if ( cp == 0x20 ) {
				if ( plane != 1 ) { tc[m++] = 0xFE21; plane = 1; }
				tc[m++] = 0x2121;
			} else {
				m += bpk_ucs_tron(cp, tc + m, max - m - 1, &plane);
			}
		}
		k += d;
	}
	if ( plane != 1 ) tc[m++] = 0xFE21;
	return m;
}

LOCAL void fatr_flush( BK_TADW *w );

/* A letter is written: what the letters are in goes before it */
LOCAL void char_begin( BK_TADW *w )
{
	fatr_flush(w);
	deco_flush(w);
	w->nclosed = 0;
	w->st.nchar++;
}

/* A code of a plane written as it is */
LOCAL void put_tcode( BK_TADW *w, INT plane, UINT code )
{
	char_begin(w);
	plane_to(w, plane);
	put16(w, code);
}

/* One character of the text */
LOCAL void put_char( BK_TADW *w, UINT cp )
{
	UH	tc[2];
	INT	n, i;

	if ( cp == '\r' || cp == '\n' || ( cp < 0x20 && cp != '\t' ) ) return;
	char_begin(w);
	if ( cp == '\t' ) {
		put16(w, TC_TAB);
		return;
	}
	if ( cp == 0x20 ) {
		/* a space: the full-width one, as BTRON writes it */
		plane_to(w, 1);
		put16(w, 0x2121);
		return;
	}
	n = bpk_ucs_tron(cp, tc, 2, &w->plane);
	for ( i = 0; i < n; i++ ) put16(w, tc[i]);
	if ( w->plane != 1 ) w->st.nuni++;
}

/* A control character (a new paragraph, a new page): no letters come with it */
LOCAL void put_code( BK_TADW *w, UINT tc )
{
	put16(w, tc);
}

/* ------------------------------------------------------------------ */
/* Where the writing is */

LOCAL BK_TADLV *top( BK_TADW *w )
{
	return ( w->depth > 0 ) ? &w->lv[w->depth - 1] : NULL;
}

/* The text or figure being written in: LV_TEXT (a text or text box), LV_FIG, or LV_OTHER */
LOCAL INT where( BK_TADW *w, BK_TADLV **p_lv )
{
	INT	i;

	for ( i = w->depth - 1; i >= 0; i-- ) {
		UB	k = w->lv[i].kind;

		if ( k == LV_SKIP ) return LV_SKIP;
		if ( k == LV_TEXT || k == LV_TBOX || k == LV_FIG || k == LV_OVL ) {
			if ( p_lv != NULL ) *p_lv = &w->lv[i];
			return ( k == LV_FIG ) ? LV_FIG : LV_TEXT;
		}
	}
	return LV_OTHER;
}

LOCAL void units_out( BK_TADW *w, INT *p_h, INT *p_v )
{
	BK_TADLV *lv = NULL;

	if ( where(w, &lv) != LV_OTHER && lv != NULL ) {
		*p_h = lv->hunit;
		*p_v = lv->vunit;
	} else {
		*p_h = *p_v = DEF_UNIT;
	}
}

/* Something is written in a text: the first paragraph has begun */
LOCAL void para_touch( BK_TADW *w )
{
	BK_TADLV *lv = NULL;

	if ( where(w, &lv) == LV_TEXT && lv != NULL && lv->npara == 0 ) lv->npara = 1;
}

/* ------------------------------------------------------------------ */
/* Decorations
 *
 * An element that decorates the letters (下線, 反転, ルビ ...) says what
 * the text is in; its segments are written when something is written
 * inside it, or when it ends, for a range of nothing. An element closed
 * and opened again with nothing between -- as XML must do where the
 * ranges of the record did not nest, round a new paragraph and round a
 * link -- is one range: the one opened again is not a range of its own
 * and writes nothing by itself.
 */

LOCAL UINT hash_attr( CONST UB *tag, INT n, CONST char *name )
{
	UB	b[256];
	INT	k = bk_tad_attr(tag, n, name, b, sizeof(b)), i;
	UINT	h = 2166136261U;

	for ( i = 0; i < k && i < (INT)sizeof(b) - 1; i++ ) h = ( h ^ b[i] ) * 16777619U;
	return h ^ (UINT)k;
}

LOCAL BOOL deco_same( CONST BK_TADDECO *a, CONST BK_TADDECO *b )
{
	return (BOOL)( a->seg == b->seg && a->sub == b->sub && a->attr == b->attr
		       && a->w1 == b->w1 && a->hash == b->hash );
}

LOCAL void deco_start( BK_TADW *w, CONST BK_TADDECO *d )
{
	if ( d->seg == TS_TATTR && d->sub == 6 ) {
		/* ルビ: its text after the first word */
		UB	b[256];
		UH	tc[128];
		INT	n = bk_tad_attr(d->tag, d->taglen, "text", b, sizeof(b)), m, i;

		m = ( n > 0 ) ? tc_of(b, n, tc, 128) : 0;
		seg_raw(w, TS_TATTR, 2 + 2 * m);
		put16(w, ( 6 << 8 ) | d->attr);
		for ( i = 0; i < m; i++ ) put16(w, tc[i]);
		return;
	}
	if ( d->seg == TS_TATTR && ( d->sub == 2 || d->sub == 4 ) ) {
		seg_raw(w, TS_TATTR, 4);
		put16(w, ( (UINT)d->sub << 8 ) | d->attr);
		put16(w, d->w1);
		return;
	}
	seg_raw(w, d->seg, 2);
	put16(w, ( (UINT)d->sub << 8 ) | d->attr);
}

LOCAL void deco_end( BK_TADW *w, CONST BK_TADDECO *d )
{
	seg_raw(w, d->seg, 2);
	put16(w, (UINT)( d->sub + 1 ) << 8);
}

/* How many of the decorations wanted the record has open already */
LOCAL INT deco_prefix( BK_TADW *w )
{
	INT	p = w->deco_base;

	while ( p < w->nwant && p < w->nhave && deco_same(&w->want[p], &w->have[p]) ) p++;
	return p;
}

/* The record's decorations brought to what the text is in */
LOCAL void deco_flush( BK_TADW *w )
{
	INT	p, i;

	if ( w->flushing ) return;
	w->flushing = TRUE;
	p = deco_prefix(w);
	for ( i = w->nhave - 1; i >= p; i-- ) deco_end(w, &w->have[i]);
	for ( i = p; i < w->nwant; i++ ) {
		deco_start(w, &w->want[i]);
		w->have[i] = w->want[i];
	}
	w->nhave = w->nwant;
	w->flushing = FALSE;
}

LOCAL INT deco_push( BK_TADW *w, UH segid, UB sub, UB attr, UH w1, CONST TOK *t, BOOL ruby )
{
	BK_TADDECO *d;
	INT	i;

	if ( w->nwant >= BK_TAD_DECO ) return -1;
	d = &w->want[w->nwant++];
	d->seg = segid;
	d->sub = sub;
	d->attr = attr;
	d->w1 = w1;
	d->tag = t->p;
	d->taglen = t->n;
	d->hash = ruby ? hash_attr(t->p, t->n, "text") : 0;
	d->serial = ++w->serial;
	d->real = TRUE;
	for ( i = 0; i < w->nclosed; i++ ) {
		if ( deco_same(&w->closed[i], d) ) {
			/* opened again after XML closed it: the same range goes on */
			d->real = FALSE;
			for ( ; i + 1 < w->nclosed; i++ ) w->closed[i] = w->closed[i + 1];
			w->nclosed--;
			break;
		}
	}
	return d->serial;
}

LOCAL void deco_pop( BK_TADW *w, INT serial )
{
	INT	i;

	for ( i = w->nwant - 1; i >= w->deco_base; i-- ) {
		if ( w->want[i].serial == serial ) {
			/* a range of its own not yet written: written, of nothing */
			if ( w->want[i].real && i >= deco_prefix(w) ) deco_flush(w);
			if ( w->nclosed < BK_TAD_DECO ) w->closed[w->nclosed++] = w->want[i];
			for ( ; i + 1 < w->nwant; i++ ) w->want[i] = w->want[i + 1];
			w->nwant--;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Letters */

LOCAL void size_seg( BK_TADW *w, UH size )
{
	fusen(w, TS_TFONT, 2, 0, 4);
	put16(w, size);
}

LOCAL void fatr_seg( BK_TADW *w, UH fatr )
{
	fusen(w, TS_TFONT, 1, 0, 4);
	put16(w, fatr);
	w->fatr_out = fatr;
}

LOCAL void color_seg( BK_TADW *w, UINT col )
{
	fusen(w, TS_TFONT, 6, 0, 6);
	put32(w, col);
}

LOCAL void ratio_seg( BK_TADW *w, UINT hr, UINT wr )
{
	fusen(w, TS_TFONT, 3, 0, 6);
	put16(w, hr);
	put16(w, wr);
}

/* The font's look the elements it is in give it, written before letters */
LOCAL void fatr_flush( BK_TADW *w )
{
	if ( w->fatr != w->fatr_out ) fatr_seg(w, w->fatr);
}

LOCAL void font_save( BK_TADW *w, BK_TADLV *lv )
{
	lv->size = w->size;
	lv->fatr = w->fatr;
	lv->hr = w->hr;
	lv->wr = w->wr;
	lv->color = w->color;
}

/* The letters back as `lv` holds them, where they were changed */
LOCAL void font_back( BK_TADW *w, BK_TADLV *lv )
{
	if ( lv->size != w->size ) {
		size_seg(w, ( lv->size != 0 ) ? lv->size : DEF_SIZE);
		w->size = lv->size;
	}
	w->fatr = lv->fatr;			/* written when letters come */
	if ( lv->hr != w->hr || lv->wr != w->wr ) {
		w->hr = lv->hr;
		w->wr = lv->wr;
		ratio_seg(w, w->hr, w->wr);
	}
	if ( lv->color != w->color ) {
		color_seg(w, lv->color);
		w->color = lv->color;
	}
}

/* A number in thousandths, from an attribute's text; FALSE when it is not one */
LOCAL BOOL milli_of( CONST UB *b, INT *p_v )
{
	INT	i = 0, v = 0, f = 0, fd = 0, sign = 1;

	while ( is_space(b[i]) ) i++;
	if ( b[i] == '-' ) { sign = -1; i++; }
	if ( ( b[i] < '0' || b[i] > '9' ) && b[i] != '.' ) return FALSE;
	while ( b[i] >= '0' && b[i] <= '9' ) {
		if ( v < 1000000 ) v = v * 10 + ( b[i] - '0' );
		i++;
	}
	if ( b[i] == '.' ) {
		i++;
		while ( b[i] >= '0' && b[i] <= '9' ) {
			if ( fd < 4 ) { f = f * 10 + ( b[i] - '0' ); fd++; }
			i++;
		}
	}
	while ( fd < 4 ) { f *= 10; fd++; }
	*p_v = sign * ( v * 1000 + ( f + 5 ) / 10 );
	return TRUE;
}

/*
 * A fraction a/b whose thousandths, rounded, are x: a of at most amax,
 * the smallest b that gives it. 0 when there is none.
 */
LOCAL UH frac_of( INT x, INT amax )
{
	INT	a, b;

	if ( x < 0 ) return 0;
	for ( b = 1; b <= 255; b++ ) {
		a = ( x * b + 500 ) / 1000;
		if ( a < 0 || a > amax ) continue;
		if ( ( a * 1000 + b / 2 ) / b == x ) return (UH)( ( a << 8 ) | b );
	}
	return 0;
}

/*
 * A SCALE as xmlTAD says it: "abs:n" for so many units, "a/b", a
 * fraction in thousandths ("0.25"), or a whole number a (b of 0).
 */
LOCAL UH scale_text( CONST UB *b )
{
	INT	i = 0, a = 0, d = 0, x;

	if ( b[0] == 'a' ) {
		for ( i = 4; b[i] >= '0' && b[i] <= '9'; i++ ) a = a * 10 + ( b[i] - '0' );
		return (UH)( 0x8000 | ( a & 0x7FFF ) );
	}
	while ( b[i] >= '0' && b[i] <= '9' ) a = a * 10 + ( b[i++] - '0' );
	if ( b[i] == '/' ) {
		i++;
		while ( b[i] >= '0' && b[i] <= '9' ) d = d * 10 + ( b[i++] - '0' );
		if ( a == 1 && d == 1 ) return 0x0101;
		return (UH)( ( ( a & 0x7F ) << 8 ) | ( d & 0xFF ) );
	}
	if ( b[i] != '.' ) return (UH)( ( a & 0x7F ) << 8 );		/* a whole number */
	if ( !milli_of(b, &x) ) return 0x8000;
	return frac_of(x, 127);
}

LOCAL UH scale_of( CONST TOK *t, CONST char *name, UH def )
{
	UB	b[32];

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return def;
	return scale_text(b);
}

/* "a/b" as a RATIO; 1/1 is the default, 0 */
LOCAL UH ratio_of( CONST TOK *t, CONST char *name, BOOL *p_has )
{
	UB	b[24];
	INT	a = 0, d = 0, i = 0;

	*p_has = FALSE;
	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return 0;
	while ( b[i] >= '0' && b[i] <= '9' ) a = a * 10 + ( b[i++] - '0' );
	if ( b[i] == '/' ) {
		i++;
		while ( b[i] >= '0' && b[i] <= '9' ) d = d * 10 + ( b[i++] - '0' );
	} else {
		d = 1;
	}
	*p_has = TRUE;
	if ( a <= 0 || d <= 0 || a > 255 || d > 255 || ( a == 1 && d == 1 ) ) return 0;
	return (UH)( ( a << 8 ) | d );
}

/* A font name: CSS's families as the two BTRON has */
LOCAL void face_seg( BK_TADW *w, CONST TOK *t )
{
	UB	b[64];
	UH	tc[40];
	INT	n, i, m;
	CONST UB *name = b;

	n = bk_tad_attr(t->p, t->n, "face", b, sizeof(b));
	if ( n < 0 ) return;
	if ( same(b, n, "sans-serif") || same(b, n, "monospace") ) {
		name = (CONST UB *)"ゴシック";
	} else if ( same(b, n, "serif") ) {
		name = (CONST UB *)"明朝";
	}
	for ( n = 0; name[n] != 0; n++ ) ;
	m = tc_of(name, n, tc, 40);
	fusen(w, TS_TFONT, 0, 0, 4 + 2 * m);
	put16(w, FCLASS);
	for ( i = 0; i < m; i++ ) put16(w, tc[i]);
}

/* A font size: 1/20 point, or 1/20 Q where the size in points says so */
LOCAL UH size_of( INT milli )
{
	INT	v = ( milli * 20 + 500 ) / 1000, q;

	if ( milli * 20 % 1000 != 0 ) {
		q = (INT)( ( (D)milli * 70560 + 5000000 ) / 10000000 );
		if ( q > 0 && q <= 0x3FFF
		  && (INT)( ( (D)q * 10000000 + 35280 ) / 70560 ) == milli ) return (UH)( 0x4000 | q );
	}
	if ( v < 1 ) v = 1;
	if ( v > 0x3FFF ) v = 0x3FFF;
	return (UH)( 0x8000 | v );
}

/* What a <font> says, written and made the letters from here */
LOCAL void font_attrs( BK_TADW *w, CONST TOK *t )
{
	UINT	col;
	BOOL	hh, hw;
	UH	hr, wr;
	INT	v;
	UB	b[32];

	if ( has(t, "face") ) face_seg(w, t);
	if ( has(t, "rotation") ) {
		/* 文字回転 */
		fusen(w, TS_TFONT, 5, (UINT)num(t, "rotabs", 0) & 0xFF, 4);
		put16(w, (UH)num(t, "rotation", 0));
	}
	if ( bk_tad_attr(t->p, t->n, "baseshift", b, sizeof(b)) >= 0 ) {
		/* 文字基準位置移動 */
		fusen(w, TS_TFONT, 7, (UINT)num(t, "baseattr", 0) & 0xFF, 4);
		put16(w, scale_text(b));
	}
	if ( has(t, "size") ) {
		w->size = size_of(num1000(t, "size", 14000));
		size_seg(w, w->size);
	}
	if ( has(t, "weight") || has(t, "style") || has(t, "stretch") ) {
		/* 文字属性: every field the element says, the rest as they are */
		UH	fatr = w->fatr;

		if ( has(t, "weight") ) {
			v = num(t, "weight", 0);
			if ( v > 9 ) {
				v = ( v <= 100 ) ? 1 : ( v <= 300 ) ? 2 : ( v < 500 ) ? 0
				  : ( v < 600 ) ? 4 : ( v <= 700 ) ? 5 : ( v <= 800 ) ? 6 : 7;
			}
			fatr = (UH)( ( fatr & ~0x0038 ) | ( ( v & 7 ) << 3 ) );
		}
		if ( bk_tad_attr(t->p, t->n, "style", b, sizeof(b)) >= 0 ) {
			v = ( b[0] >= '1' && b[0] <= '7' ) ? b[0] - '0' : ( b[0] == 'i' ) ? 1 : 0;
			if ( b[0] == 'o' ) {
				INT	n;

				for ( n = 0; b[n] != 0; n++ ) ;
				v = same(b, n, "oblique 20deg") ? 7 : same(b, n, "oblique 15deg") ? 6 : 5;
			}
			fatr = (UH)( ( fatr & ~0x01C0 ) | ( v << 6 ) );
		}
		if ( bk_tad_attr(t->p, t->n, "stretch", b, sizeof(b)) >= 0 ) {
			static CONST char *CONST sn[] = { "normal", "condensed", "extra-condensed",
							  "ultra-condensed", "", "extra-expanded", "ultra-expanded" };
			INT	k, n;

			for ( n = 0; b[n] != 0; n++ ) ;
			for ( v = 0, k = 1; k < 7; k++ ) {
				if ( sn[k][0] != 0 && same(b, n, sn[k]) ) v = k;
			}
			fatr = (UH)( ( fatr & ~0x0007 ) | v );
		}
		w->fatr = fatr;
		fatr_seg(w, fatr);
	}
	hr = ratio_of(t, "hRatio", &hh);
	if ( !hh ) hr = ratio_of(t, "hratio", &hh);
	wr = ratio_of(t, "wRatio", &hw);
	if ( !hw ) wr = ratio_of(t, "wratio", &hw);
	if ( hh || hw ) {
		if ( hh ) w->hr = hr;
		if ( hw ) w->wr = wr;
		ratio_seg(w, w->hr, w->wr);
	}
	if ( has(t, "space") || has(t, "kerning") || has(t, "direction") ) {
		/* 文字間隔: so many units when whole, else a fraction of a letter */
		UH	pitch = 0x8000;
		INT	x;

		if ( bk_tad_attr(t->p, t->n, "space", b, sizeof(b)) >= 0 && milli_of(b, &x) ) {
			if ( x % 1000 == 0 && x >= 0 ) pitch = (UH)( 0x8000 | ( ( x / 1000 ) & 0x7FFF ) );
			else pitch = frac_of(x, 127);
		}
		fusen(w, TS_TFONT, 4, ( ( num(t, "direction", 0) & 1 ) << 7 )
		      | ( ( num(t, "kerning", 0) & 1 ) << 6 ) | ( num(t, "pattern", 0) & 1 ), 4);
		put16(w, pitch);
	}
	if ( colour(t, "color", &col) == COL_SET ) {
		w->color = col;
		color_seg(w, col);
	}
}

/* <text>: the line pitch, alignment and direction from here */
LOCAL void ruler( BK_TADW *w, CONST TOK *t )
{
	UB	b[24];
	INT	x;

	if ( bk_tad_attr(t->p, t->n, "line-height", b, sizeof(b)) >= 0 && milli_of(b, &x) ) {
		/* the letter's height times this: n + 1 for n units, else 1 + a/b */
		UH	pitch;

		if ( x >= 1000 && x % 1000 == 0 ) pitch = (UH)( 0x8000 | ( ( x / 1000 - 1 ) & 0x7FFF ) );
		else pitch = frac_of(x - 1000, 127);
		fusen(w, TS_TRULER, 0, 1, 4);
		put16(w, pitch);
	}
	if ( bk_tad_attr(t->p, t->n, "align", b, sizeof(b)) >= 0 ) {
		INT	n;
		UINT	a;

		for ( n = 0; b[n] != 0; n++ ) ;
		a = ( b[0] == 'c' ) ? 1 : ( b[0] == 'r' ) ? 2 : same(b, n, "justify-all") ? 4 : ( b[0] == 'j' ) ? 3 : 0;

		fusen(w, TS_TRULER, 1, a, 2);
	}
	if ( has(t, "direction") ) {
		fusen(w, TS_TRULER, 4, (UINT)num(t, "direction", 0), 2);
	}
}

/* A list of numbers "1,2,3" into v[max]; how many */
LOCAL INT list_of( CONST TOK *t, CONST char *name, INT *v, INT max, BOOL hex )
{
	UB	b[1024];
	INT	n = 0, i = 0, x, sign, h;

	if ( bk_tad_attr(t->p, t->n, name, b, sizeof(b)) < 0 ) return 0;
	while ( b[i] != 0 && n < max ) {
		while ( b[i] == ',' || is_space(b[i]) ) i++;
		if ( b[i] == 0 ) break;
		sign = 1;
		if ( b[i] == '-' ) { sign = -1; i++; }
		if ( hex ) {
			if ( hexv(b[i]) < 0 ) break;
			for ( x = 0; ( h = hexv(b[i]) ) >= 0; i++ ) x = x * 16 + h;
		} else {
			if ( b[i] < '0' || b[i] > '9' ) break;
			for ( x = 0; b[i] >= '0' && b[i] <= '9'; i++ ) x = x * 10 + ( b[i] - '0' );
		}
		v[n++] = sign * x;
		while ( b[i] != 0 && b[i] != ',' && !is_space(b[i]) ) i++;
	}
	return n;
}

/* <tab-format>: TT_TAB */
LOCAL void tab_format( BK_TADW *w, CONST TOK *t )
{
	INT	tabs[64], n, i;

	n = list_of(t, "tabs", tabs, 64, FALSE);
	fusen(w, TS_TRULER, 2, ( ( num(t, "R", 0) & 1 ) << 7 ) | ( num(t, "P", 0) & 3 ), 14 + 2 * n);
	put16(w, scale_of(t, "height", 0x8000));
	put16(w, scale_of(t, "pargap", 0x8000));
	put16(w, (UH)num(t, "left", 0));
	put16(w, (UH)num(t, "right", 0));
	put16(w, (UH)num(t, "indent", 0));
	put16(w, (UH)n);
	for ( i = 0; i < n; i++ ) put16(w, (UH)tabs[i]);
}

/* <field-format> and its <field>s: TT_FIELD, the fields looked for ahead */
LOCAL void field_format( BK_TADW *w, CONST UB *s, INT len, INT at, CONST TOK *t, BOOL empty )
{
	TOK	f;
	INT	k, a, nfld = 0, depth = 0;

	for ( a = at; !empty && ( k = token(s, len, &a, &f) ) > TK_EOF; ) {
		if ( k == TK_END && depth-- == 0 ) break;
		if ( k == TK_START ) depth++;
		if ( ( k == TK_START || k == TK_EMPTY ) && same(f.name, f.nlen, "field") ) nfld++;
	}
	fusen(w, TS_TRULER, 3, ( ( num(t, "R", 0) & 1 ) << 7 ) | ( num(t, "P", 0) & 3 ), 10 + 10 * nfld);
	put16(w, scale_of(t, "height", 0x8000));
	put16(w, scale_of(t, "pargap", 0x8000));
	put16(w, (UH)num(t, "line", 0));
	put16(w, (UH)nfld);
	depth = 0;
	for ( a = at; !empty && ( k = token(s, len, &a, &f) ) > TK_EOF; ) {
		if ( k == TK_END && depth-- == 0 ) break;
		if ( k == TK_START ) depth++;
		if ( ( k == TK_START || k == TK_EMPTY ) && same(f.name, f.nlen, "field") ) {
			put16(w, (UH)num(&f, "fld", 0));
			put16(w, (UH)num(&f, "left", 0));
			put16(w, (UH)num(&f, "right", 0));
			put16(w, (UH)num(&f, "margin", 0));
			put16(w, (UH)num(&f, "f_attr", 0));
		}
	}
}

/* The page's 付箋 of a text: <paper>, <docmargin>, <column> ... */
LOCAL BOOL text_page( BK_TADW *w, CONST TOK *t )
{
	if ( same(t->name, t->nlen, "paper") && has(t, "length") ) {
		fusen(w, TS_TPAGE, 0, ( num(t, "imposition", 0) & 1 ) | ( ( num(t, "binding", 0) & 1 ) << 1 ), 14);
		put16(w, (UH)num(t, "length", 0));
		put16(w, (UH)num(t, "width", 0));
		put16(w, (UH)num(t, "top", 0));
		put16(w, (UH)num(t, "bottom", 0));
		put16(w, (UH)num(t, "left", 0));
		put16(w, (UH)num(t, "right", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "docmargin")
	  || ( same(t->name, t->nlen, "paper") && has(t, "margintop") ) ) {
		BOOL	m = same(t->name, t->nlen, "paper");

		fusen(w, TS_TPAGE, 1, 0, 10);
		put16(w, (UH)num(t, m ? "margintop" : "top", 0));
		put16(w, (UH)num(t, m ? "marginbottom" : "bottom", 0));
		put16(w, (UH)num(t, m ? "marginleft" : "left", 0));
		put16(w, (UH)num(t, m ? "marginright" : "right", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "column") ) {
		BOOL	lines = has(t, "colline");

		fusen(w, TS_TPAGE, 2, ( num(t, "column", 1) & 0x0F ) | ( ( num(t, "balance", 0) & 1 ) << 7 ),
		      lines ? 6 : 4);
		put16(w, (UH)num(t, "colsp", 0));
		if ( lines ) {
			UINT	lo = ( ( num(t, "colline", 0) & 0x0F ) << 4 ) | ( num(t, "lineType", 0) & 0x0F );
			UINT	hi = ( ( num(t, "colline2", 0) & 0x0F ) << 4 ) | ( num(t, "lineType2", 0) & 0x0F );

			put16(w, ( hi << 8 ) | lo);
		}
		return TRUE;
	}
	if ( same(t->name, t->nlen, "frame-open") ) {
		fusen(w, TS_TPAGE, 5, ( ( num(t, "abs", 0) & 1 ) << 7 ) | ( ( num(t, "page", 0) & 1 ) << 6 )
		      | ( ( num(t, "wrap", 0) & 1 ) << 5 ) | ( ( num(t, "halign", 0) & 3 ) << 2 )
		      | ( num(t, "valign", 0) & 3 ), 10);
		put16(w, (UH)num(t, "top", 0));
		put16(w, (UH)num(t, "left", 0));
		put16(w, (UH)( num(t, "bottom", 0) + 1 ));
		put16(w, (UH)( num(t, "right", 0) + 1 ));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "page-number") ) {
		fusen(w, TS_TPAGE, 6, (UINT)num(t, "step", 1) & 0xFF, 4);
		put16(w, (UH)num(t, "num", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "pagebreak") && has(t, "cond") ) {
		fusen(w, TS_TPAGE, 7, (UINT)num(t, "cond", 0) & 0xFF, 4);
		put16(w, (UH)num(t, "remain", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "fill-line") ) {
		fusen(w, TS_TPAGE, 8, 0, 2);
		return TRUE;
	}
	return FALSE;
}

/* 図形ページ割付け: <paper>, <figpagenumber> of a figure */
LOCAL BOOL fig_page( BK_TADW *w, CONST TOK *t )
{
	if ( same(t->name, t->nlen, "paper") && has(t, "length") ) {
		fusen(w, TS_FPAGE, 0, ( num(t, "imposition", 0) & 1 ) | ( ( num(t, "binding", 0) & 1 ) << 1 ), 14);
		put16(w, (UH)num(t, "length", 0));
		put16(w, (UH)num(t, "width", 0));
		put16(w, (UH)num(t, "top", 0));
		put16(w, (UH)num(t, "bottom", 0));
		put16(w, (UH)num(t, "left", 0));
		put16(w, (UH)num(t, "right", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "paper") && has(t, "margintop") ) {
		fusen(w, TS_FPAGE, 1, 0, 10);
		put16(w, (UH)num(t, "margintop", 0));
		put16(w, (UH)num(t, "marginbottom", 0));
		put16(w, (UH)num(t, "marginleft", 0));
		put16(w, (UH)num(t, "marginright", 0));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "figpagenumber") ) {
		fusen(w, TS_FPAGE, 6, (UINT)num(t, "step", 1) & 0xFF, 4);
		put16(w, (UH)num(t, "num", 0));
		return TRUE;
	}
	return FALSE;
}

/* A memo, or 充填文字: the attribute's text as TRON characters after the first word */
LOCAL void text_seg_a( BK_TADW *w, UINT id, UINT sub, UINT attr, CONST TOK *t, CONST char *name )
{
	UB	b[512];
	UH	tc[256];
	INT	n = bk_tad_attr(t->p, t->n, name, b, sizeof(b)), m, i;

	m = ( n > 0 ) ? tc_of(b, n, tc, 256) : 0;
	fusen(w, id, sub, attr, 2 + 2 * m);
	for ( i = 0; i < m; i++ ) put16(w, tc[i]);
}

LOCAL void text_seg( BK_TADW *w, UINT id, UINT sub, CONST TOK *t, CONST char *name )
{
	text_seg_a(w, id, sub, 0, t, name);
}

/* Overlays by number, "0, 2": the top bit overlay 0 */
LOCAL UINT overlay_bits( CONST TOK *t, CONST char *name )
{
	INT	v[16], n = list_of(t, name, v, 16, FALSE), i;
	UINT	bits = 0;

	for ( i = 0; i < n; i++ ) {
		if ( v[i] >= 0 && v[i] < 16 ) bits |= 0x8000U >> v[i];
	}
	return bits;
}

/*
 * A segment the converter kept as its bytes (<tadseg>, <docappl>,
 * <figappl>): written back as it was. One too long for an attribute
 * goes on in the elements right after it (part="1", "2" ...), which
 * bring only their bytes.
 */
LOCAL void kept( BK_TADW *w, CONST UB *s, INT len, INT at, CONST TOK *t, UINT id )
{
	TOK	f;
	INT	body, a = at, k, i;

	if ( has(t, "part") ) {
		(void)hex_put(w, t, "data", TRUE);
		return;
	}
	body = hex_put(w, t, "data", FALSE);
	for ( ;; ) {
		k = token(s, len, &a, &f);
		if ( k == TK_TEXT ) {
			for ( i = 0; i < f.n && is_space(f.p[i]); i++ ) ;
			if ( i == f.n ) continue;
		}
		if ( k != TK_EMPTY || f.nlen != t->nlen ) break;
		for ( i = 0; i < f.nlen && f.name[i] == t->name[i]; i++ ) ;
		if ( i < f.nlen || !has(&f, "part") ) break;
		body += hex_put(w, &f, "data", FALSE);
	}
	seg(w, id, body);
	(void)hex_put(w, t, "data", TRUE);
}

/* 用紙オーバーレイ定義: its text inside the segment, the length put in at its end */
LOCAL void overlay_open( BK_TADW *w, BK_TADLV *lv, CONST TOK *t, CONST BK_TADLV *ctx )
{
	fatr_flush(w);
	deco_flush(w);
	lv->segat = w->len;
	seg_raw(w, TS_TPAGE, 0);
	put16(w, ( 3 << 8 ) | ( num(t, "N", 0) & 0x0F ) | ( ( num(t, "P", 0) & 3 ) << 4 ));
	lv->npara = 0;
	lv->deco_base = w->deco_base;
	lv->hunit = ( ctx != NULL ) ? ctx->hunit : DEF_UNIT;
	lv->vunit = ( ctx != NULL ) ? ctx->vunit : DEF_UNIT;
	w->deco_base = w->nwant;
	w->plane = 1;			/* its text begins in plane 1 */
}

LOCAL void overlay_close( BK_TADW *w, BK_TADLV *lv )
{
	INT	body;

	w->nwant = w->deco_base;	/* what is still open in it ends in it */
	deco_flush(w);
	plane_to(w, 1);
	body = w->len - lv->segat - 4;
	if ( body >= 0xFFFF ) {
		if ( w->er == E_OK ) w->er = E_LIMIT;
	} else {
		put8(w, (UINT)body, lv->segat + 2);
		put8(w, (UINT)body >> 8, lv->segat + 3);
	}
	w->deco_base = lv->deco_base;
	w->nclosed = 0;
}

/*
 * <calcPos cell="B3"/>: a cell of 基本表計算, a 指定付箋. What the cell
 * says of its letters comes right after it, in this order, and is taken
 * into it rather than written apart: its size, its decorations (to the
 * cell's end), its rules and its colour.
 */
LOCAL void calc_pos( BK_TADW *w, CONST UB *s, INT len, INT at, CONST TOK *t )
{
	static CONST UB size_pt[7] = { 0, 6, 9, 0, 24, 36, 48 };
	static CONST char *CONST deco_name[5] = { "bold", "italic", "underline", "mesh", "invert" };
	static CONST UB deco_bit[5] = { 0x01, 0x02, 0x04, 0x20, 0x40 };
	static CONST char *CONST col_name[8] = {
		"#ffffff", "", "#ff0000", "#00ff00", "#0000ff", "#ffff00", "#ff00ff", "#00ffff"
	};
	static CONST char *CONST rule_name[4] = { "", "line", "double", "dot" };
	UB	b[16];
	INT	row = 0, col = 0, i, n, k, a = at, b4, fs = 0, deco = 0, rule = 0, colour = 1, v, off;
	TOK	f;

	n = bk_tad_attr(t->p, t->n, "cell", b, sizeof(b));
	i = ( n > 0 && b[0] == '@' ) ? 1 : 0;		/* "@": column 0 */
	for ( ; i < n && b[i] >= 'A' && b[i] <= 'Z'; i++ ) col = col * 26 + ( b[i] - 'A' + 1 );
	for ( ; i < n && b[i] >= '0' && b[i] <= '9'; i++ ) row = row * 10 + ( b[i] - '0' );

	b4 = a;
	k = token(s, len, &a, &f);
	if ( k == TK_EMPTY && same(f.name, f.nlen, "font") && nattr(&f) == 1 && has(&f, "size") ) {
		v = num(&f, "size", 0);
		for ( i = 1; i < 7 && ( size_pt[i] == 0 || size_pt[i] != v ); i++ ) ;
		if ( i < 7 ) {
			fs = i;
			w->absorb++;
			b4 = a;
		}
	}
	a = b4;
	for ( i = 0; i < 5; i++ ) {
		k = token(s, len, &a, &f);
		if ( k == TK_START && same(f.name, f.nlen, deco_name[i]) && nattr(&f) == 0 ) {
			deco |= deco_bit[i];
			w->absorb++;
			b4 = a;
		} else {
			a = b4;
		}
	}
	k = token(s, len, &a, &f);
	if ( k == TK_EMPTY && same(f.name, f.nlen, "calcCell") ) {
		INT	nl = bk_tad_attr(f.p, f.n, "borderLeftType", b, sizeof(b));

		for ( i = 1; i < 4; i++ ) if ( nl >= 0 && same(b, nl, rule_name[i]) ) rule |= i;
		nl = bk_tad_attr(f.p, f.n, "borderTopType", b, sizeof(b));
		for ( i = 1; i < 4; i++ ) if ( nl >= 0 && same(b, nl, rule_name[i]) ) rule |= i << 4;
		w->absorb++;
		b4 = a;
	} else {
		a = b4;
	}
	k = token(s, len, &a, &f);
	if ( k == TK_EMPTY && same(f.name, f.nlen, "font") && nattr(&f) == 1
	  && bk_tad_attr(f.p, f.n, "color", b, sizeof(b)) == 7 ) {
		for ( i = 0; i < 8; i++ ) {
			if ( i != 1 && same(b, 7, col_name[i]) ) break;
		}
		if ( i < 8 ) {
			colour = i;
			w->absorb++;
		}
	}

	para_touch(w);
	seg(w, TS_DFUSEN, DF_DATA + 12);
	for ( off = 0; off < DF_DATA; off += 2 ) {
		put16(w, ( off == DF_APPL || off == DF_APPL + 4 ) ? 0x8000
		       : ( off == DF_APPL + 2 ) ? 0x0009 : ( off == DF_DLEN ) ? 12 : 0);
	}
	put16(w, 0);
	put16(w, (UH)row);
	put16(w, (UH)col);
	put16(w, 0);
	put16(w, (UINT)fs | ( (UINT)deco << 8 ));
	put16(w, (UINT)rule | ( (UINT)colour << 8 ));
}

/* ------------------------------------------------------------------ */
/* Texts and figures */

typedef struct {
	INT	view[4], draw[4];
	INT	hunit, vunit;
	INT	lang, bpat;
	BOOL	has_view, has_draw, has_unit, has_lang;
} HEAD;

LOCAL void rect4( CONST TOK *t, CONST char *l, CONST char *tp, CONST char *r,
		  CONST char *b, INT *v )
{
	v[0] = num(t, l, 0);
	v[1] = num(t, tp, 0);
	v[2] = num(t, r, 0);
	v[3] = num(t, b, 0);
}

/*
 * What a text or figure opening at `at` says of itself: its view,
 * drawing area, units, and for a text its language and background, from
 * the first of those elements in it (a paragraph may hold them) but not
 * from a text or figure inside it. Of several units the first that is
 * not 0 (0 is what an inner one says when it means the outer's), else 0
 * as it is written.
 */
LOCAL void head_scan( CONST UB *s, INT len, INT at, BOOL text, HEAD *h )
{
	TOK	t;
	INT	depth = 0, inner = -1, k;
	BOOL	nonzero = FALSE;

	while ( ( k = token(s, len, &at, &t) ) > TK_EOF ) {
		if ( k == TK_END ) {
			if ( depth == 0 ) return;
			depth--;
			if ( inner == depth ) inner = -1;
			continue;
		}
		if ( k != TK_START && k != TK_EMPTY ) continue;
		if ( inner < 0 && depth <= 1 ) {
			if ( same(t.name, t.nlen, text ? "docView" : "figView") && !h->has_view ) {
				if ( text ) rect4(&t, "viewleft", "viewtop", "viewright", "viewbottom", h->view);
				else rect4(&t, "left", "top", "right", "bottom", h->view);
				h->has_view = TRUE;
			} else if ( same(t.name, t.nlen, text ? "docDraw" : "figDraw") && !h->has_draw ) {
				if ( text ) rect4(&t, "drawleft", "drawtop", "drawright", "drawbottom", h->draw);
				else rect4(&t, "left", "top", "right", "bottom", h->draw);
				h->has_draw = TRUE;
			} else if ( same(t.name, t.nlen, text ? "docScale" : "figScale") && !nonzero ) {
				INT	hu = num(&t, "hunit", 0), vu = num(&t, "vunit", 0);

				if ( hu != 0 || vu != 0 || !h->has_unit ) {
					h->hunit = hu;
					h->vunit = vu;
					h->has_unit = TRUE;
					nonzero = ( hu != 0 || vu != 0 );
				}
			} else if ( text && same(t.name, t.nlen, "text") ) {
				if ( has(&t, "lang") && !h->has_lang ) {
					h->lang = num(&t, "lang", 0);
					h->has_lang = TRUE;
				}
				if ( has(&t, "bpat") ) h->bpat = num(&t, "bpat", 0);
			}
		}
		if ( k == TK_START ) {
			if ( inner < 0 && ( same(t.name, t.nlen, "document")
					 || same(t.name, t.nlen, "figure") ) ) {
				inner = depth;
			}
			depth++;
		}
	}
}

LOCAL void head_init( BK_TADW *w, HEAD *h )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) h->view[i] = h->draw[i] = 0;
	units_out(w, &h->hunit, &h->vunit);
	h->lang = 0x21;
	h->bpat = 0;
	h->has_view = h->has_draw = h->has_unit = h->has_lang = FALSE;
}

/* A new text: its level is `lv`, pushed already */
LOCAL void text_open( BK_TADW *w, BK_TADLV *lv, CONST HEAD *h )
{
	INT	i;

	fatr_flush(w);
	seg(w, TS_TEXT, 24);
	for ( i = 0; i < 4; i++ ) put16(w, (UH)h->view[i]);
	for ( i = 0; i < 4; i++ ) put16(w, (UH)h->draw[i]);
	put16(w, (UH)h->hunit);
	put16(w, (UH)h->vunit);
	put16(w, (UH)h->lang);
	put16(w, (UH)h->bpat);

	lv->npara = 0;
	lv->plane = w->plane;
	font_save(w, lv);
	lv->fatr_out = w->fatr_out;
	lv->deco_base = w->deco_base;
	lv->hunit = h->hunit;
	lv->vunit = h->vunit;

	/* a text starts in plane 1 with its letters as they are by default */
	w->plane = 1;
	w->size = 0;
	w->fatr = w->fatr_out = 0;
	w->hr = w->wr = 0;
	w->color = BLACK;
	w->deco_base = w->nwant;
}

LOCAL void text_close( BK_TADW *w, BK_TADLV *lv )
{
	w->nwant = w->deco_base;		/* what is still open ends with it */
	seg(w, TS_TEXTEND, 0);
	w->plane = lv->plane;
	w->size = lv->size;
	w->fatr = lv->fatr;
	w->fatr_out = lv->fatr_out;
	w->hr = lv->hr;
	w->wr = lv->wr;
	w->color = lv->color;
	w->deco_base = lv->deco_base;
}

LOCAL void fig_open( BK_TADW *w, BK_TADLV *lv, CONST HEAD *h )
{
	INT	i;

	fatr_flush(w);
	seg(w, TS_FIG, 24);
	for ( i = 0; i < 4; i++ ) put16(w, (UH)h->view[i]);
	for ( i = 0; i < 4; i++ ) put16(w, (UH)h->draw[i]);
	put16(w, (UH)h->hunit);
	put16(w, (UH)h->vunit);
	put32(w, 0);
	lv->npat = w->npat;
	lv->hunit = h->hunit;
	lv->vunit = h->vunit;
	w->fmod = FALSE;
}

LOCAL void fig_close( BK_TADW *w, BK_TADLV *lv )
{
	seg(w, TS_FIGEND, 0);
	w->npat = lv->npat;		/* patterns made inside are not known outside */
}

/* ------------------------------------------------------------------ */
/* Virtual objects */

LOCAL ER vobj( BK_TADW *w, CONST TOK *t )
{
	BK_TADLINK lk;
	INT	v[4], height, i, dlen;
	UINT	col[4];
	static CONST char *CONST cn[4] = { "frcol", "chcol", "tbcol", "bgcol" };
	static CONST UINT cdef[4] = { BLACK, BLACK, WHITE, WHITE };
	UB	id[64];
	INT	n;

	if ( has(t, "vobjleft") || !has(t, "width") ) {
		rect4(t, "vobjleft", "vobjtop", "vobjright", "vobjbottom", v);
	} else {
		v[0] = v[1] = 0;
		v[2] = num(t, "width", 0);
		v[3] = num(t, "heightpx", num(t, "heightPx", 0));
	}
	height = num(t, "vobjheight", 0);
	if ( !has(t, "vobjheight") ) height = num(t, "height", v[3] - v[1]);
	for ( i = 0; i < 4; i++ ) {
		if ( colour(t, cn[i], &col[i]) != COL_SET ) col[i] = cdef[i];
	}
	/* the object's own data is not kept: as long as it was, of zeros */
	dlen = num(t, "dlen", 0);
	if ( dlen < 0 || dlen > 1024 || ( dlen & 1 ) ) dlen = 0;

	para_touch(w);
	seg(w, TS_VOBJ, 30 + dlen);
	for ( i = 0; i < 4; i++ ) put16(w, (UH)v[i]);
	put16(w, (UH)height);
	/* the name's size is in points, as a letter's size is; none is left as none */
	put16(w, has(t, "chsz") && num1000(t, "chsz", 0) > 0 ? size_of(num1000(t, "chsz", 0)) : 0);
	for ( i = 0; i < 4; i++ ) put32(w, col[i]);
	put16(w, (UH)dlen);
	for ( i = 0; i < dlen; i += 2 ) put16(w, 0);

	lk.n = w->st.nlink++;
	lk.tag = t->p;
	lk.taglen = t->n;
	lk.target[0] = 0;
	n = bk_tad_attr(t->p, t->n, "id", id, sizeof(id));
	if ( n > 0 ) {
		/* "uuid_0.xtad": the object is the part before the '_' */
		for ( i = 0; i < n && i < (INT)sizeof(lk.target) - 1 && id[i] != '_'; i++ ) {
			lk.target[i] = id[i];
		}
		lk.target[i] = 0;
	}
	lk.attr = 0;
	if ( !flag(t, "namedisp", TRUE) ) lk.attr |= V_NONAME;
	if ( !flag(t, "roledisp", FALSE) ) lk.attr |= V_NORELN;
	if ( !flag(t, "typedisp", FALSE) ) lk.attr |= V_NOTYPE;
	if ( !flag(t, "updatedisp", FALSE) ) lk.attr |= V_NOTIME;
	if ( !flag(t, "pictdisp", TRUE) ) lk.attr |= V_NOPICT;
	if ( !flag(t, "framedisp", TRUE) ) lk.attr |= V_NOFDISP;
	if ( flag(t, "autoopen", FALSE) ) lk.attr |= V_AUTEXE;
	return ( w->hk != NULL && w->hk->link != NULL ) ? w->hk->link(w->hk->arg, &lk) : E_OK;
}

/* ------------------------------------------------------------------ */
/* Pictures */

/*
 * TS_IMAGE of 24-bit RGB: the head (view, drawing area, units, colour
 * RGB with channels R 16..23, G 8..15, B 0..7, the offsets of the mask
 * and the pixels), the rows of blue, green, red each padded to a whole
 * word, then, when some pixels are not there, the mask: a bit for each
 * pixel, 1 where it is, rows of a whole number of words.
 */
LOCAL ER image( BK_TADW *w, CONST TOK *t )
{
	UB	href[256];
	CONST UINT *px = NULL;
	INT	iw = 0, ih = 0, r[4], hu, vu, x, y, rowbytes, mrow = 0, body, maskoff = 0;
	BOOL	clear = FALSE;
	ER	er;

	if ( bk_tad_attr(t->p, t->n, "href", href, sizeof(href)) <= 0
	  || w->hk == NULL || w->hk->image == NULL ) {
		w->st.noimage++;
		return E_OK;
	}
	er = w->hk->image(w->hk->arg, href, &px, &iw, &ih);
	if ( er == E_NOEXS || ( er >= E_OK && ( px == NULL || iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192 ) ) ) {
		w->st.noimage++;
		return E_OK;
	}
	if ( er < E_OK ) return er;

	rect4(t, "left", "top", "right", "bottom", r);
	if ( r[2] <= r[0] || r[3] <= r[1] ) {
		r[2] = r[0] + iw;
		r[3] = r[1] + ih;
	}
	units_out(w, &hu, &vu);
	for ( x = 0; x < iw * ih && !clear; x++ ) clear = ( px[x] == BK_PX_CLEAR );
	rowbytes = ( ( iw * 24 + 15 ) / 8 ) & ~1;
	body = 0x40 + rowbytes * ih;
	if ( clear ) {
		mrow = ( ( iw + 15 ) / 16 ) * 2;
		maskoff = body;
		body += mrow * ih;
	}

	para_touch(w);
	fatr_flush(w);
	seg(w, TS_IMAGE, body);
	for ( x = 0; x < 4; x++ ) put16(w, (UH)r[x]);		/* view */
	for ( x = 0; x < 4; x++ ) put16(w, (UH)r[x]);		/* draw */
	put16(w, (UH)hu);
	put16(w, (UH)vu);
	put16(w, 0);				/* slope */
	put16(w, 0x0001);			/* RGB, no colour map */
	put16(w, 0x1008);			/* red: from bit 16, 8 bits */
	put16(w, 0x0808);
	put16(w, 0x0008);
	put16(w, 0);
	put32(w, 0);				/* extlen */
	put32(w, 0);				/* extend */
	put32(w, (UINT)maskoff);
	put16(w, 0);				/* not compressed */
	put16(w, 1);				/* planes */
	put16(w, 0x1818);			/* 24 bits a pixel */
	put16(w, (UINT)rowbytes);
	put16(w, 0);				/* bounds */
	put16(w, 0);
	put16(w, (UINT)iw);
	put16(w, (UINT)ih);
	put32(w, 0x40);				/* the pixels */
	for ( y = 0; y < ih; y++ ) {
		INT	at = w->len;

		for ( x = 0; x < rowbytes; x += 2 ) put16(w, 0);
		for ( x = 0; x < iw; x++ ) {
			UINT	p = px[y * iw + x];

			if ( p == BK_PX_CLEAR ) p = 0xFFFFFF;
			put8(w, p, at + 3 * x);
			put8(w, p >> 8, at + 3 * x + 1);
			put8(w, p >> 16, at + 3 * x + 2);
		}
	}
	if ( clear ) {
		for ( y = 0; y < ih; y++ ) {
			INT	at = w->len;

			for ( x = 0; x < mrow; x += 2 ) put16(w, 0);
			for ( x = 0; x < iw; x++ ) {
				if ( px[y * iw + x] != BK_PX_CLEAR && w->out != NULL && at + x / 8 < w->max ) {
					w->out[at + x / 8] |= (UB)( 0x80 >> ( x % 8 ) );
				}
			}
		}
	}
	w->st.nimage++;
	return E_OK;
}

/* ------------------------------------------------------------------ */
/* Figures */

/* The pattern of one colour: made the first time the figure needs it */
LOCAL UH pattern_of( BK_TADW *w, UINT col )
{
	INT	i;
	UH	id;

	for ( i = 0; i < w->npat; i++ ) {
		if ( w->patcol[i] == col ) return w->patid[i];
	}
	id = (UH)w->patnext++;
	if ( w->npat < BK_TAD_PATS ) {
		w->patcol[w->npat] = col;
		w->patid[w->npat] = id;
		w->npat++;
	}
	fusen(w, TS_FDEF, 2, 0, 20);
	put16(w, id);
	put16(w, 16);
	put16(w, 16);
	put16(w, 1);
	put32(w, col);
	put32(w, CLEAR);
	put16(w, 7);			/* filled all over */
	return id;
}

typedef struct {
	UH	l_atr, l_pat, f_pat;
} LINEPAT;

/*
 * How a shape's line and inside are drawn: pattern numbers as they are;
 * colours made patterns here, before the shape.
 */
LOCAL void line_pat( BK_TADW *w, CONST TOK *t, BOOL fill, LINEPAT *lp )
{
	UINT	col;
	INT	k, width = num(t, "lineWidth", 1), type = num(t, "lineType", 0);

	k = colour(t, "lineColor", &col);
	if ( k == COL_NONE ) k = colour(t, "strokeColor", &col);
	if ( k == COL_CLEAR ) {
		width = 0;
		lp->l_pat = 0;
	} else if ( k == COL_SET ) {
		lp->l_pat = pattern_of(w, col);
	} else if ( has(t, "l_pat") ) {
		lp->l_pat = (UH)num(t, "l_pat", 1);
	} else {
		lp->l_pat = pattern_of(w, BLACK);
	}
	lp->l_atr = (UH)( ( ( type & 0xFF ) << 8 ) | ( width & 0xFF ) );

	lp->f_pat = 0;
	if ( fill ) {
		k = colour(t, "fillColor", &col);
		if ( k == COL_SET ) lp->f_pat = pattern_of(w, col);
		else if ( k == COL_NONE && has(t, "f_pat") ) lp->f_pat = (UH)num(t, "f_pat", 0);
	}
}

/* Arrows for the next line element, when <figmodifier> did not give them */
LOCAL void arrows( BK_TADW *w, CONST TOK *t )
{
	UINT	a = ( num(t, "start_arrow", 0) != 0 ? 1 : 0 )
		  | ( num(t, "end_arrow", 0) != 0 ? 2 : 0 );

	if ( a != 0 && !w->fmod ) {
		fusen(w, TS_FATTR, 0, 0, 4);
		put16(w, a);
	}
}

/* "x,y x,y ...": how many, and each written when put */
LOCAL INT points( BK_TADW *w, CONST TOK *t, BOOL put )
{
	UB	b[4096];
	INT	n, i = 0, cnt = 0, k, v[2];

	n = bk_tad_attr(t->p, t->n, "points", b, sizeof(b));
	if ( n < 0 || n >= (INT)sizeof(b) ) return 0;
	for ( ;; ) {
		for ( k = 0; k < 2; k++ ) {
			INT	x = 0, sign = 1, f = -1;

			while ( b[i] == ' ' || b[i] == ',' || b[i] == '\n' || b[i] == '\t' ) i++;
			if ( b[i] == 0 ) return cnt;
			if ( b[i] == '-' ) { sign = -1; i++; }
			if ( b[i] < '0' || b[i] > '9' ) return cnt;
			while ( b[i] >= '0' && b[i] <= '9' ) x = x * 10 + ( b[i++] - '0' );
			if ( b[i] == '.' ) {
				i++;
				f = ( b[i] >= '0' && b[i] <= '9' ) ? b[i] - '0' : 0;
				while ( b[i] >= '0' && b[i] <= '9' ) i++;
			}
			if ( f >= 5 ) x++;
			v[k] = sign * x;
		}
		if ( put ) {
			put16(w, (UH)v[0]);
			put16(w, (UH)v[1]);
		}
		cnt++;
	}
}

LOCAL void prim_head( BK_TADW *w, UINT sub, UINT mode, INT body )
{
	fusen(w, TS_FPRIM, sub, mode, body);
}

/* A shape of the figure; FALSE when the element is none */
LOCAL BOOL shape( BK_TADW *w, CONST TOK *t )
{
	LINEPAT	lp;
	INT	r[4], np, i;
	UINT	mode = (UINT)num(t, "mode", 0) & 0xFF;

	if ( same(t->name, t->nlen, "rect") ) {
		INT	rh = -1, rv = -1;
		BOOL	tbox = ( has(t, "fontSize") || has(t, "textColor") );

		rect4(t, "left", "top", "right", "bottom", r);
		if ( has(t, "figRH") ) {
			rh = num(t, "figRH", 0);
			rv = num(t, "figRV", rh);
		} else if ( has(t, "cornerRadius") ) {
			rh = rv = 2 * num(t, "cornerRadius", 0);
		} else if ( num(t, "round", 0) > 1 ) {
			rh = rv = 2 * num(t, "round", 0);
		}
		line_pat(w, t, TRUE, &lp);
		if ( tbox ) lp.l_atr = 0;	/* a box of text has no frame */
		if ( rh >= 0 ) {
			prim_head(w, 1, mode, 22);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			put16(w, lp.f_pat);
			put16(w, (UH)num(t, "angle", 0));
			put16(w, (UH)rh);
			put16(w, (UH)rv);
		} else {
			prim_head(w, 0, mode, 18);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			put16(w, lp.f_pat);
			put16(w, (UH)num(t, "angle", 0));
		}
		for ( i = 0; i < 4; i++ ) put16(w, (UH)r[i]);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "ellipse") || same(t->name, t->nlen, "circle") ) {
		if ( has(t, "frameLeft") ) {
			rect4(t, "frameLeft", "frameTop", "frameRight", "frameBottom", r);
		} else if ( has(t, "cx") ) {
			INT	cx = num(t, "cx", 0), cy = num(t, "cy", 0);
			INT	rx = num(t, "rx", 0), ry = num(t, "ry", 0);

			if ( rx == 0 && ry == 0 ) rx = ry = num(t, "r", 0);
			r[0] = cx - rx;  r[1] = cy - ry;
			r[2] = cx + rx;  r[3] = cy + ry;
		} else {
			rect4(t, "left", "top", "right", "bottom", r);
		}
		line_pat(w, t, TRUE, &lp);
		prim_head(w, 2, mode, 18);
		put16(w, lp.l_atr);
		put16(w, lp.l_pat);
		put16(w, lp.f_pat);
		put16(w, (UH)num(t, "angle", 0));
		for ( i = 0; i < 4; i++ ) put16(w, (UH)r[i]);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "arc") || same(t->name, t->nlen, "chord")
	  || same(t->name, t->nlen, "elliptical_arc") ) {
		BOOL	earc = same(t->name, t->nlen, "elliptical_arc");

		rect4(t, "frameLeft", "frameTop", "frameRight", "frameBottom", r);
		line_pat(w, t, !earc, &lp);
		arrows(w, t);
		if ( earc ) {
			prim_head(w, 7, mode, 24);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			put16(w, (UH)num(t, "angle", 0));
		} else {
			prim_head(w, same(t->name, t->nlen, "arc") ? 3 : 4, mode, 26);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			put16(w, lp.f_pat);
			put16(w, (UH)num(t, "angle", 0));
		}
		for ( i = 0; i < 4; i++ ) put16(w, (UH)r[i]);
		put16(w, (UH)num(t, "startX", r[2]));
		put16(w, (UH)num(t, "startY", r[1]));
		put16(w, (UH)num(t, "endX", r[0]));
		put16(w, (UH)num(t, "endY", r[3]));
		return TRUE;
	}
	if ( same(t->name, t->nlen, "polygon") ) {
		np = points(w, t, FALSE);
		if ( np < 1 ) return TRUE;
		line_pat(w, t, TRUE, &lp);
		prim_head(w, 5, mode, 12 + 4 * np);
		put16(w, lp.l_atr);
		put16(w, lp.l_pat);
		put16(w, lp.f_pat);
		put16(w, (UH)num(t, "round", 0));
		put16(w, (UH)np);
		(void)points(w, t, TRUE);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "line") ) {
		np = points(w, t, FALSE);
		line_pat(w, t, FALSE, &lp);
		arrows(w, t);
		if ( np == 0 ) {
			prim_head(w, 6, mode, 14);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			put16(w, (UH)num(t, "x1", 0));
			put16(w, (UH)num(t, "y1", 0));
			put16(w, (UH)num(t, "x2", 0));
			put16(w, (UH)num(t, "y2", 0));
		} else {
			prim_head(w, 6, mode, 6 + 4 * np);
			put16(w, lp.l_atr);
			put16(w, lp.l_pat);
			(void)points(w, t, TRUE);
		}
		return TRUE;
	}
	if ( same(t->name, t->nlen, "polyline") ) {
		np = points(w, t, FALSE);
		line_pat(w, t, FALSE, &lp);
		arrows(w, t);
		prim_head(w, 8, mode, 10 + 4 * np);
		put16(w, lp.l_atr);
		put16(w, lp.l_pat);
		put16(w, (UH)num(t, "round", 0));
		put16(w, (UH)np);
		(void)points(w, t, TRUE);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "curve") ) {
		np = points(w, t, FALSE);
		if ( np < 2 ) return TRUE;
		line_pat(w, t, TRUE, &lp);
		arrows(w, t);
		prim_head(w, 9, mode, 12 + 4 * np);
		put16(w, lp.l_atr);
		put16(w, lp.l_pat);
		put16(w, lp.f_pat);
		put16(w, (UH)num(t, "type", 0));
		put16(w, (UH)np);
		(void)points(w, t, TRUE);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "marker") ) {
		CONST char *mid = has(t, "markerId") ? "markerId" : "markerid";

		np = points(w, t, FALSE);
		if ( np < 1 ) return TRUE;
		prim_head(w, 10, mode, 6 + 4 * np);
		put16(w, (UH)num(t, mid, 0));
		put16(w, (UH)np);
		(void)points(w, t, TRUE);
		return TRUE;
	}
	if ( same(t->name, t->nlen, "freefig") ) {
		INT	h[512], nh = list_of(t, "h", h, 512, FALSE);

		prim_head(w, 11, mode, 12 + 2 * nh);
		put16(w, (UH)num(t, "f_pat", 0));
		put16(w, (UH)num(t, "sy", 0));
		put16(w, (UH)num(t, "nr", 0));
		put16(w, (UH)num(t, "bx", 0));
		put16(w, (UH)nh);
		for ( i = 0; i < nh; i++ ) put16(w, (UH)h[i]);
		return TRUE;
	}
	return FALSE;
}

/* <pattern>: TS_PATTERN as it is */
LOCAL void pattern_def( BK_TADW *w, CONST TOK *t )
{
	UB	fg[512];
	INT	ncol = num(t, "ncol", 1), i, a, b, mk[32], nm;
	UINT	col;

	if ( ncol < 1 || ncol > 32 ) ncol = 1;
	if ( bk_tad_attr(t->p, t->n, "fgcolors", fg, sizeof(fg)) < 0 ) fg[0] = 0;
	nm = list_of(t, "masks", mk, 32, FALSE);
	fusen(w, TS_FDEF, 2, (UINT)num(t, "type", 0), 14 + 6 * ncol);
	put16(w, (UH)num(t, "id", 0));
	put16(w, (UH)num(t, "width", 16));
	put16(w, (UH)num(t, "height", 16));
	put16(w, (UH)ncol);
	for ( i = 0, a = 0; i < ncol; i++ ) {
		UB	one[16];

		while ( fg[a] == ' ' ) a++;
		for ( b = 0; fg[a] != 0 && fg[a] != ',' && b < 15; ) one[b++] = fg[a++];
		one[b] = 0;
		if ( fg[a] == ',' ) a++;
		if ( colour_of(one, &col) == COL_NONE ) col = BLACK;
		put32(w, col);
	}
	{
		UB	bg[16];

		if ( bk_tad_attr(t->p, t->n, "bgcolor", bg, sizeof(bg)) < 0
		  || colour_of(bg, &col) == COL_NONE ) {
			col = CLEAR;
		}
		put32(w, col);
	}
	for ( i = 0; i < ncol; i++ ) put16(w, (UH)( ( i < nm ) ? mk[i] : 7 ));
}

/* <mask>: TS_MASK, its words as they are written */
LOCAL void mask_def( BK_TADW *w, CONST TOK *t )
{
	INT	d[1024], n = list_of(t, "data", d, 1024, TRUE), i;

	fusen(w, TS_FDEF, 1, (UINT)num(t, "type", 0), 8 + 2 * n);
	put16(w, (UH)num(t, "id", 0));
	put16(w, (UH)num(t, "width", 16));
	put16(w, (UH)num(t, "height", 16));
	for ( i = 0; i < n; i++ ) put16(w, (UH)d[i]);
}

/* <markerDefine>: TS_MARKPAT */
LOCAL void marker_def( BK_TADW *w, CONST TOK *t )
{
	UINT	col = BLACK;
	BOOL	mask = has(t, "mask");
	UB	b[16];

	if ( bk_tad_attr(t->p, t->n, "fgCol", b, sizeof(b)) < 0 ) (void)bk_tad_attr(t->p, t->n, "fgcol", b, sizeof(b));
	(void)colour_of(b, &col);
	fusen(w, TS_FDEF, 4, (UINT)num(t, "type", 0), mask ? 12 : 10);
	put16(w, (UH)num(t, "id", 0));
	put16(w, (UH)num(t, "size", 16));
	put32(w, col);
	if ( mask ) put16(w, (UH)num(t, "mask", 0));
}

/* ------------------------------------------------------------------ */
/* Elements */

LOCAL BK_TADLV *push( BK_TADW *w, CONST TOK *t, UB kind )
{
	BK_TADLV *lv;

	if ( w->depth >= BK_TAD_DEPTH ) {
		w->er = E_LIMIT;
		return NULL;
	}
	lv = &w->lv[w->depth++];
	lv->name = t->name;
	lv->nlen = t->nlen;
	lv->kind = kind;
	lv->serial = 0;
	lv->npara = 0;
	return lv;
}

/* The ranges of letters: TS_TSTYLE by kind, TS_TATTR by sub id */
LOCAL BOOL deco_of( CONST TOK *t, UH *p_seg, UB *p_sub, UB *p_attr, UH *p_w1 )
{
	static CONST struct { CONST char *name; UB sub, attr; } tab[] = {
		{ "underline", 0, 0x10 }, { "overline", 2, 0 }, { "strikethrough", 4, 0 },
		{ "strike", 4, 0 }, { "box", 6, 0 }, { "invert", 12, 0 }, { "mesh", 14, 0 },
		{ "background", 16, 0 }, { "noprint", 18, 0 },
	};
	UB	b[16];
	INT	i;

	*p_w1 = 0;
	for ( i = 0; i < (INT)( sizeof(tab) / sizeof(tab[0]) ); i++ ) {
		if ( same(t->name, t->nlen, tab[i].name) ) {
			*p_seg = TS_TSTYLE;
			*p_sub = tab[i].sub;
			*p_attr = tab[i].attr;
			return TRUE;
		}
	}
	if ( same(t->name, t->nlen, "bouten") ) {
		*p_seg = TS_TSTYLE;
		*p_attr = (UB)( num(t, "kind", 0) & 0x0F );
		*p_sub = ( bk_tad_attr(t->p, t->n, "side", b, sizeof(b)) >= 0 && b[0] == 'l' ) ? 10 : 8;
		return TRUE;
	}
	*p_seg = TS_TATTR;
	if ( same(t->name, t->nlen, "combchar") ) {
		*p_sub = 0;
		*p_attr = 0;
		return TRUE;
	}
	if ( same(t->name, t->nlen, "char-layout") ) {
		UB	wb[24];

		*p_sub = 2;
		*p_attr = (UB)num(t, "kind", 0);
		INT	n = bk_tad_attr(t->p, t->n, "width", wb, sizeof(wb));

		*p_w1 = ( n >= 0 && !same(wb, n, "1/1") ) ? scale_text(wb) : 0;
		return TRUE;
	}
	if ( same(t->name, t->nlen, "attend") ) {
		INT	unit = num(t, "unit", 0) & 1;

		*p_sub = 4;
		*p_attr = (UB)( ( ( num(t, "position", 0) & 1 ) << 3 ) | ( unit << 2 ) | ( num(t, "type", 0) & 3 ) );
		*p_w1 = (UH)( unit ? num(t, "targetPosition", 0) & 1 : num(t, "baseline", 0) & 7 );
		return TRUE;
	}
	if ( same(t->name, t->nlen, "ruby") ) {
		*p_sub = 6;
		*p_attr = ( bk_tad_attr(t->p, t->n, "position", b, sizeof(b)) >= 0
			    && ( b[0] == '1' || b[0] == 'b' ) ) ? 1 : 0;
		return TRUE;
	}
	return FALSE;
}

/*
 * A new paragraph and a link are where XML closes every decoration and
 * opens them again after; what they are in is known only once the
 * elements opened after them are read. They wait for the first token
 * that opens nothing, and are written inside what is open then.
 */
LOCAL BOOL reopens( BK_TADW *w, INT k, CONST TOK *t )
{
	UH	sg, w1;
	UB	sb, at;

	if ( k == TK_TEXT ) {
		INT	i;

		for ( i = 0; i < t->n && ( t->p[i] == '\r' || t->p[i] == '\n' ); i++ ) ;
		return (BOOL)( i == t->n );
	}
	if ( k != TK_START || where(w, NULL) != LV_TEXT ) return FALSE;
	if ( w->pend == PEND_LINK ) {
		/* round a link only what draws a line or a ground */
		return (BOOL)( deco_of(t, &sg, &sb, &at, &w1) && sg == TS_TSTYLE && sb != 8 && sb != 10 );
	}
	return (BOOL)( deco_of(t, &sg, &sb, &at, &w1) || same(t->name, t->nlen, "strong")
		       || same(t->name, t->nlen, "bold") || same(t->name, t->nlen, "i")
		       || same(t->name, t->nlen, "italic") || same(t->name, t->nlen, "bagchar") );
}

LOCAL void pend_resolve( BK_TADW *w )
{
	INT	k = w->pend, keep = w->nwant;

	if ( k == 0 ) return;
	w->pend = 0;
	/* what was open and still is goes on over it; what is new starts after it */
	w->nwant = deco_prefix(w);
	deco_flush(w);
	w->nwant = keep;
	if ( k == PEND_NL ) {
		put_code(w, TC_NL);
	} else {
		TOK	t;
		ER	er;

		t.kind = TK_EMPTY;
		t.p = w->pend_p;
		t.n = w->pend_n;
		t.name = t.p;
		for ( t.nlen = 0; t.nlen < t.n && !is_space(t.p[t.nlen]) && t.p[t.nlen] != '/'; t.nlen++ ) ;
		er = vobj(w, &t);
		if ( er < E_OK && w->er == E_OK ) w->er = er;
	}
	w->nclosed = 0;
}

LOCAL void on_start( BK_TADW *w, CONST UB *s, INT len, INT at, CONST TOK *t, BOOL empty )
{
	BK_TADLV *lv = NULL, *ctx = NULL;
	INT	in = where(w, &ctx);
	UH	dseg, w1;
	UB	dsub, dattr;
	HEAD	h;

	if ( in == LV_SKIP ) {
		if ( !empty ) (void)push(w, t, LV_SKIP);
		return;
	}
	if ( same(t->name, t->nlen, "document") || same(t->name, t->nlen, "figure") ) {
		BOOL	text = same(t->name, t->nlen, "document");

		head_init(w, &h);
		if ( !empty ) head_scan(s, len, at, text, &h);
		lv = push(w, t, text ? LV_TEXT : LV_FIG);
		if ( lv == NULL ) return;
		if ( text ) text_open(w, lv, &h);
		else fig_open(w, lv, &h);
		if ( empty ) {
			w->depth--;
			if ( text ) text_close(w, lv);
			else fig_close(w, lv);
		}
		return;
	}

	/* elements that say something of the text or figure, read at its start */
	if ( same(t->name, t->nlen, "docView") || same(t->name, t->nlen, "docDraw")
	  || same(t->name, t->nlen, "docScale") || same(t->name, t->nlen, "figView")
	  || same(t->name, t->nlen, "figDraw") || same(t->name, t->nlen, "figScale")
	  || same(t->name, t->nlen, "tad") ) {
		if ( !empty ) (void)push(w, t, LV_OTHER);
		return;
	}

	/* a segment kept as its bytes */
	if ( same(t->name, t->nlen, "tadseg") || same(t->name, t->nlen, "docappl")
	  || same(t->name, t->nlen, "figappl") ) {
		INT	id[1];

		if ( t->name[0] == 't' ) id[0] = ( list_of(t, "id", id, 1, TRUE) == 1 ) ? id[0] : 0;
		else id[0] = ( t->name[0] == 'd' ) ? TS_TAPPL : TS_FAPPL;
		if ( id[0] >= 0xFF80 && id[0] <= 0xFFFF ) {
			para_touch(w);
			kept(w, s, len, at, t, (UINT)id[0]);
		}
		if ( !empty ) (void)push(w, t, LV_SKIP);
		return;
	}

	if ( in == LV_TEXT ) {
		if ( same(t->name, t->nlen, "paper-overlay-define") ) {
			if ( empty ) {
				BK_TADLV tmp;

				overlay_open(w, &tmp, t, ctx);
				overlay_close(w, &tmp);
				return;
			}
			lv = push(w, t, LV_OVL);
			if ( lv != NULL ) overlay_open(w, lv, t, ctx);
			return;
		}
		if ( same(t->name, t->nlen, "docoverlay") ) {
			fusen(w, TS_TPAGE, 4, 0, 4);
			put16(w, overlay_bits(t, "active"));
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "line-head-kinsoku") || same(t->name, t->nlen, "line-tail-kinsoku") ) {
			/* 行頭禁則, 行末禁則: the kind and the letters */
			text_seg_a(w, TS_TATTR, ( t->name[5] == 'h' ) ? 8 : 9, (UINT)hexnum(t, "kind") & 0xFF, t, "ch");
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( ( same(t->name, t->nlen, "page-number") && starts(t->p, t->nlen + 1, t->n, "num=") )
		  || same(t->name, t->nlen, "variable") ) {
			/* 変数参照: the page's number is variable 200 */
			para_touch(w);
			if ( t->name[0] == 'v' && has(t, "name") ) {
				text_seg(w, TS_TVAR, 1, t, "name");
			} else {
				fusen(w, TS_TVAR, 0, 0, 4);
				put16(w, (UH)( ( t->name[0] == 'p' ) ? 200 : num(t, "id", 0) ));
			}
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "tchar") ) {
			/* letters with no Unicode, as their plane and codes */
			INT	code[64], n = list_of(t, "code", code, 64, TRUE), i;

			para_touch(w);
			for ( i = 0; i < n; i++ ) put_tcode(w, num(t, "plane", 1), (UINT)code[i] & 0xFFFF);
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "calcPos") ) {
			calc_pos(w, s, len, at, t);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "p") ) {
			if ( ctx->npara > 0 ) w->pend = PEND_NL;
			ctx->npara++;
			if ( !empty ) (void)push(w, t, LV_P);
			return;
		}
		if ( same(t->name, t->nlen, "br") ) {
			if ( ctx->npara == 0 ) ctx->npara = 1;
			w->pend = PEND_NL;
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "tab") ) {
			/* 行頭移動 */
			para_touch(w);
			fusen(w, TS_TRULER, 5, 0, 2);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "pagebreak") && !has(t, "cond") ) {
			para_touch(w);
			put_code(w, TC_FF);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "font") ) {
			if ( !empty ) {
				lv = push(w, t, LV_FONT);
				if ( lv == NULL ) return;
				font_save(w, lv);
			}
			font_attrs(w, t);
			return;
		}
		if ( same(t->name, t->nlen, "bold") || same(t->name, t->nlen, "strong")
		  || same(t->name, t->nlen, "i") || same(t->name, t->nlen, "italic")
		  || same(t->name, t->nlen, "bagchar") ) {
			/* the look of the letters: written when letters come */
			if ( empty ) return;
			lv = push(w, t, LV_FATR);
			if ( lv == NULL ) return;
			lv->fatr = w->fatr;
			if ( t->name[0] == 'b' && t->name[1] == 'a' ) w->fatr = (UH)( ( w->fatr & ~0x0E00 ) | 0x0200 );
			else if ( t->name[0] == 'b' || t->name[0] == 's' ) w->fatr = (UH)( ( w->fatr & ~0x0038 ) | 0x0028 );
			else w->fatr = (UH)( ( w->fatr & ~0x01C0 ) | 0x0040 );
			return;
		}
		if ( deco_of(t, &dseg, &dsub, &dattr, &w1) ) {
			if ( empty ) return;
			lv = push(w, t, LV_STYLE);
			if ( lv == NULL ) return;
			lv->serial = deco_push(w, dseg, dsub, dattr, w1, t, (BOOL)( dseg == TS_TATTR && dsub == 6 ));
			return;
		}
		if ( same(t->name, t->nlen, "text") ) {
			ruler(w, t);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "tab-format") ) {
			tab_format(w, t);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "field-format") ) {
			field_format(w, s, len, at, t, empty);
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "fixed-space") || same(t->name, t->nlen, "fixedSpace") ) {
			UB	wb[24];
			INT	n = bk_tad_attr(t->p, t->n, "width", wb, sizeof(wb));

			para_touch(w);
			fusen(w, TS_TCHAR, 0, 0, 4);
			put16(w, ( n >= 0 && !same(wb, n, "1/1") ) ? scale_text(wb) : 0);
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "fill-char") ) {
			text_seg(w, TS_TCHAR, 1, t, "str");
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "docmemo") ) {
			text_seg(w, TS_TMEMO, 0, t, "text");
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( text_page(w, t) ) {
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
	}

	if ( in == LV_FIG ) {
		if ( same(t->name, t->nlen, "group") ) {
			fusen(w, TS_FGRP, 0, 0, 4);
			put16(w, (UH)num(t, "id", 0));
			if ( empty ) {
				fusen(w, TS_FGRP, 1, 0, 2);
			} else {
				(void)push(w, t, LV_GROUP);
			}
			return;
		}
		if ( same(t->name, t->nlen, "pattern") ) {
			pattern_def(w, t);
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "mask") ) {
			mask_def(w, t);
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "lineTypeDefine") ) {
			/* 線種定義: the pattern a bit a pixel */
			INT	nb = hex_put(w, t, "mask", FALSE);

			fusen(w, TS_FDEF, 3, 0, 6 + nb + ( nb & 1 ));
			put16(w, (UH)num(t, "id", 0));
			put16(w, (UH)nb);
			(void)hex_put(w, t, "mask", TRUE);
			if ( nb & 1 ) {
				put8(w, 0, w->len);
				w->len++;
			}
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "figoverlay") ) {
			if ( has(t, "active") ) {
				fusen(w, TS_FPAGE, 4, 0, 4);
				put16(w, overlay_bits(t, "active"));
			} else {
				/*
				 * 用紙オーバーレイ定義 of a figure: its words as they
				 * are. The shapes written inside it are the same
				 * words read out, and are not written again.
				 */
				INT	n, i, k = 0, v;
				CONST UB *d = attr_at(t, "overlayData", &n);

				for ( i = 0; d != NULL && i < n; i++ ) if ( d[i] == ',' ) k++;
				seg(w, TS_FPAGE, ( d != NULL && n > 0 ) ? 2 * ( k + 1 ) : 0);
				for ( i = 0; d != NULL && i < n; ) {
					for ( v = 0; i < n && d[i] >= '0' && d[i] <= '9'; i++ ) v = v * 10 + ( d[i] - '0' );
					put16(w, (UINT)v);
					while ( i < n && ( d[i] < '0' || d[i] > '9' ) ) i++;
				}
				if ( !empty ) (void)push(w, t, LV_SKIP);
				return;
			}
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "markerDefine") || same(t->name, t->nlen, "markerdefine") ) {
			marker_def(w, t);
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
		if ( same(t->name, t->nlen, "figmodifier") ) {
			UB	a[8];
			UINT	v = 0;

			if ( bk_tad_attr(t->p, t->n, "arrow", a, sizeof(a)) >= 0 ) {
				INT	k;

				for ( k = 0; a[k] != 0; k++ ) {
					if ( a[k] == 'S' ) v |= 1;
					if ( a[k] == 'E' ) v |= 2;
				}
			}
			fusen(w, TS_FATTR, 0, 0, 4);
			put16(w, v);
			w->fmod = TRUE;
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "transform") ) {
			fusen(w, TS_FATTR, 1, 0, 10);
			put16(w, (UH)num(t, "dh", 0));
			put16(w, (UH)num(t, "dv", 0));
			put16(w, (UH)num(t, "hangle", 0));
			put16(w, (UH)num(t, "vangle", 0));
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "figmemo") ) {
			text_seg(w, TS_FMEMO, 0, t, "text");
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( fig_page(w, t) ) {
			if ( !empty ) (void)push(w, t, LV_OTHER);
			return;
		}
		if ( same(t->name, t->nlen, "rect") && !empty
		  && ( has(t, "fontSize") || has(t, "textColor") ) ) {
			/* a box of text: the rectangle, then the text in it */
			UINT	col;

			(void)shape(w, t);
			w->fmod = FALSE;
			head_init(w, &h);
			rect4(t, "left", "top", "right", "bottom", h.view);
			rect4(t, "left", "top", "right", "bottom", h.draw);
			lv = push(w, t, LV_TBOX);
			if ( lv == NULL ) return;
			text_open(w, lv, &h);
			if ( has(t, "fontSize") ) {
				w->size = size_of(num1000(t, "fontSize", 14000));
				size_seg(w, w->size);
			}
			if ( colour(t, "textColor", &col) == COL_SET && col != w->color ) {
				w->color = col;
				color_seg(w, col);
			}
			return;
		}
		if ( shape(w, t) ) {
			w->fmod = FALSE;
			if ( !empty ) (void)push(w, t, LV_SKIP);
			return;
		}
	}

	if ( same(t->name, t->nlen, "link") && in == LV_TEXT && empty ) {
		/* written when the decorations round it are known */
		w->pend = PEND_LINK;
		w->pend_p = t->p;
		w->pend_n = t->n;
		return;
	}
	if ( same(t->name, t->nlen, "link") && in != LV_OTHER ) {
		ER	er = vobj(w, t);

		w->fmod = FALSE;
		if ( er < E_OK && w->er == E_OK ) w->er = er;
		if ( !empty ) (void)push(w, t, LV_SKIP);
		return;
	}
	if ( ( same(t->name, t->nlen, "image") || same(t->name, t->nlen, "pixelmap") ) && in != LV_OTHER ) {
		ER	er = image(w, t);

		w->fmod = FALSE;
		if ( er < E_OK && w->er == E_OK ) w->er = er;
		if ( !empty ) (void)push(w, t, LV_SKIP);
		return;
	}

	/* anything else: its text still counts, it adds nothing of its own */
	w->st.nskip++;
	if ( !empty ) (void)push(w, t, LV_OTHER);
}

LOCAL ER on_end( BK_TADW *w, CONST TOK *t )
{
	BK_TADLV *lv = top(w);
	INT	i;

	/* the end tag must be the one of the element last opened */
	if ( lv == NULL || lv->nlen != t->nlen ) return E_PAR;
	for ( i = 0; i < t->nlen; i++ ) {
		if ( lv->name[i] != t->name[i] ) return E_PAR;
	}
	w->depth--;
	switch ( lv->kind ) {
	case LV_TEXT:
	case LV_TBOX:
		text_close(w, lv);
		break;
	case LV_FIG:
		fig_close(w, lv);
		break;
	case LV_FONT:
		font_back(w, lv);
		break;
	case LV_FATR:
		w->fatr = lv->fatr;		/* written when letters come */
		break;
	case LV_STYLE:
		deco_pop(w, lv->serial);
		break;
	case LV_GROUP:
		fusen(w, TS_FGRP, 1, 0, 2);
		break;
	case LV_OVL:
		overlay_close(w, lv);
		break;
	default:
		break;
	}
	return E_OK;
}

LOCAL void on_text( BK_TADW *w, CONST TOK *t, BOOL ent )
{
	INT	i = 0, in = where(w, NULL);

	if ( in == LV_SKIP ) return;
	if ( in != LV_TEXT ) {
		/* letters outside a text are written as they are; the spaces
		   between elements are not letters */
		for ( i = 0; i < t->n && ( is_space(t->p[i]) ); i++ ) ;
		if ( i == t->n ) return;
		i = 0;
	}
	while ( i < t->n ) {
		UINT	cp, nx, code;

		i += next_cp(t->p + i, t->n - i, ent, &cp);
		if ( cp == '\r' || cp == '\n' ) continue;
		para_touch(w);
		if ( i < t->n ) {
			INT	d = next_cp(t->p + i, t->n - i, ent, &nx);

			if ( ( code = seq_code(cp, nx) ) != 0 ) {
				/* a letter and its mark that have one place */
				put_tcode(w, 1, code);
				i += d;
				continue;
			}
		}
		put_char(w, cp);
	}
}

/* The largest pattern number the text uses, for the ones made here to follow */
LOCAL INT pattern_max( CONST UB *s, INT len )
{
	TOK	t;
	INT	at = 0, k, m = 0, v;

	while ( ( k = token(s, len, &at, &t) ) > TK_EOF ) {
		if ( k != TK_START && k != TK_EMPTY ) continue;
		if ( same(t.name, t.nlen, "pattern") ) {
			v = num(&t, "id", 0);
			if ( v > m ) m = v;
		}
		v = num(&t, "l_pat", 0);
		if ( v > m ) m = v;
		v = num(&t, "f_pat", 0);
		if ( v > m ) m = v;
	}
	return m;
}

EXPORT ER bk_tad_from_xml( BK_TADW *w, CONST UB *xml, INT len, UB *out, INT max,
			   INT *p_len, CONST BK_TADHOOK *hk, BK_TADSTAT *st )
{
	TOK	t;
	INT	at = 0, k;
	ER	er;

	w->out = out;
	w->max = ( out != NULL ) ? max : 0;
	w->len = 0;
	w->plane = 1;
	w->size = 0;
	w->fatr = w->fatr_out = 0;
	w->hr = w->wr = 0;
	w->color = BLACK;
	w->depth = 0;
	w->nwant = w->nhave = w->nclosed = w->deco_base = w->serial = 0;
	w->flushing = FALSE;
	w->npat = 0;
	w->fmod = FALSE;
	w->pend = 0;
	w->absorb = 0;
	w->hk = hk;
	w->er = E_OK;
	w->st.nlink = w->st.nimage = w->st.noimage = w->st.nskip = w->st.nchar = w->st.nuni = 0;

	if ( len >= 3 && xml[0] == 0xEF && xml[1] == 0xBB && xml[2] == 0xBF ) at = 3;
	w->patnext = pattern_max(xml + at, len - at) + 1;

	/* TS_INFO: the version of TAD */
	put16(w, TS_INFO);
	put16(w, 6);
	put16(w, 0);
	put16(w, 2);
	put16(w, BK_TAD_VERSION);

	while ( ( k = token(xml, len, &at, &t) ) > TK_EOF ) {
		if ( w->absorb > 0 && ( k == TK_START || k == TK_EMPTY ) ) {
			/* said by the table's cell before it */
			w->absorb--;
			if ( k == TK_START ) (void)push(w, &t, LV_OTHER);
			if ( w->er < E_OK ) return w->er;
			continue;
		}
		if ( w->pend != 0 && !reopens(w, k, &t) ) pend_resolve(w);
		switch ( k ) {
		case TK_START:
		case TK_EMPTY:
			on_start(w, xml, len, at, &t, (BOOL)( k == TK_EMPTY ));
			break;
		case TK_END:
			er = on_end(w, &t);
			if ( er < E_OK ) return er;
			break;
		case TK_TEXT:
		case TK_CDATA:
			on_text(w, &t, (BOOL)( k == TK_TEXT ));
			break;
		}
		if ( w->er < E_OK ) return w->er;
	}
	if ( k < E_OK ) return k;
	pend_resolve(w);
	if ( w->er < E_OK ) return w->er;
	if ( w->depth != 0 ) return E_PAR;

	if ( st != NULL ) *st = w->st;
	*p_len = w->len;
	if ( out != NULL && w->len > max ) return E_NOMEM;
	return E_OK;
}

/* ------------------------------------------------------------------ */
/* Checking a record */

EXPORT ER bk_tad_check( CONST UB *s, INT len, INT *p_nseg, INT *p_nchar )
{
	UH	nest[BK_TAD_DEPTH];
	INT	at = 0, depth = 0, nseg = 0, nchar = 0;
	UINT	id, body;

	/* a segment of an odd length, as some records have, leaves the rest on odd bytes */
	while ( at + 2 <= len ) {
		id = (UINT)( s[at] | ( s[at + 1] << 8 ) );
		if ( id < 0xFF80 ) {
			nchar++;
			at += 2;
			continue;
		}
		if ( at + 4 > len ) return E_OBJ;
		body = (UINT)( s[at + 2] | ( s[at + 3] << 8 ) );
		at += 4;
		if ( body == 0xFFFF ) {
			if ( at + 4 > len ) return E_OBJ;
			body = (UINT)s[at] | ( (UINT)s[at + 1] << 8 )
				| ( (UINT)s[at + 2] << 16 ) | ( (UINT)s[at + 3] << 24 );
			at += 4;
		}
		if ( body > (UINT)( len - at ) ) return E_OBJ;
		if ( nseg == 0 && id != TS_INFO ) return E_OBJ;
		switch ( id ) {
		case TS_TEXT:
		case TS_FIG:
			if ( body < 24 || depth >= BK_TAD_DEPTH ) return E_OBJ;
			nest[depth++] = (UH)id;
			break;
		case TS_TEXTEND:
		case TS_FIGEND:
			if ( depth == 0 || nest[depth - 1] != id - 1 ) return E_OBJ;
			depth--;
			break;
		default:
			break;
		}
		at += (INT)body;
		nseg++;
	}
	if ( at != len || depth != 0 ) return E_OBJ;
	if ( p_nseg != NULL ) *p_nseg = nseg;
	if ( p_nchar != NULL ) *p_nchar = nchar;
	return E_OK;
}
