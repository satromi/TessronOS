/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_xml.c
 *	The XML of a record read into a tree whose elements know where they
 *	are in the text
 *
 *	A piece of the figure is drawn by giving the kernel the text of its
 *	element as it stands, so each element keeps the place it starts (its
 *	'<') and the place after it ends. Attribute values and text have
 *	their entities replaced.
 */

#include "ms.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
	const char	*s;
	INT		n, i;
} RD;

static BOOL name_ch( char c )
{
	return (BOOL)( c != 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n'
		    && c != '/' && c != '>' && c != '=' && c != '<' );
}

static BOOL space( char c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* s[0..n) with the entities replaced, NUL ended */
static char *unescape( const char *s, INT n )
{
	MSBUF	b;
	INT	i, j;

	mb_init(&b);
	mb_putn(&b, "", 0);
	for ( i = 0; i < n; i++ ) {
		if ( s[i] != '&' ) {
			mb_putc(&b, s[i]);
			continue;
		}
		for ( j = i + 1; j < n && j < i + 12 && s[j] != ';'; j++ ) ;
		if ( j >= n || s[j] != ';' ) {
			mb_putc(&b, s[i]);
			continue;
		}
		if ( j - i == 3 && memcmp(s + i, "&lt", 3) == 0 ) mb_putc(&b, '<');
		else if ( j - i == 3 && memcmp(s + i, "&gt", 3) == 0 ) mb_putc(&b, '>');
		else if ( j - i == 4 && memcmp(s + i, "&amp", 4) == 0 ) mb_putc(&b, '&');
		else if ( j - i == 5 && memcmp(s + i, "&quot", 5) == 0 ) mb_putc(&b, '"');
		else if ( j - i == 5 && memcmp(s + i, "&apos", 5) == 0 ) mb_putc(&b, '\'');
		else if ( s[i + 1] == '#' ) {
			UW	cp = 0;
			INT	k = i + 2;

			if ( k < j && ( s[k] == 'x' || s[k] == 'X' ) ) {
				for ( k++; k < j; k++ ) {
					char	c = s[k];

					cp = cp * 16 + (UW)( ( c >= '0' && c <= '9' ) ? c - '0'
						: ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
						: ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : 0 );
				}
			} else {
				for ( ; k < j; k++ ) cp = cp * 10 + (UW)( s[k] - '0' );
			}
			mb_putcp(&b, cp);
		} else {
			mb_putn(&b, s + i, j - i + 1);
		}
		i = j;
	}
	return b.s;
}

static MSX *node_new( MSX *parent )
{
	MSX	*e = ms_alloc(sizeof(MSX));

	e->parent = parent;
	if ( parent != NULL ) {
		if ( parent->last != NULL ) parent->last->next = e;
		else parent->first = e;
		parent->last = e;
	}
	return e;
}

static void skip_to( RD *r, const char *end )
{
	INT	k = (INT)strlen(end);

	while ( r->i + k <= r->n && memcmp(r->s + r->i, end, (size_t)k) != 0 ) r->i++;
	r->i = ( r->i + k <= r->n ) ? r->i + k : r->n;
}

/* The attributes of a start tag; answers TRUE when it closed itself */
static BOOL read_attrs( RD *r, MSX *e )
{
	INT	cap = 0;

	for ( ;; ) {
		INT	ns, nl, vs;
		char	q;

		while ( r->i < r->n && space(r->s[r->i]) ) r->i++;
		if ( r->i >= r->n ) return FALSE;
		if ( r->s[r->i] == '>' ) { r->i++; return FALSE; }
		if ( r->s[r->i] == '/' ) {
			r->i++;
			if ( r->i < r->n && r->s[r->i] == '>' ) r->i++;
			return TRUE;
		}
		ns = r->i;
		while ( r->i < r->n && name_ch(r->s[r->i]) ) r->i++;
		nl = r->i - ns;
		if ( nl == 0 ) { r->i++; continue; }
		while ( r->i < r->n && space(r->s[r->i]) ) r->i++;
		if ( r->i >= r->n || r->s[r->i] != '=' ) continue;	/* no value */
		r->i++;
		while ( r->i < r->n && space(r->s[r->i]) ) r->i++;
		if ( r->i >= r->n ) return FALSE;
		q = r->s[r->i];
		if ( q != '"' && q != '\'' ) continue;
		vs = ++r->i;
		while ( r->i < r->n && r->s[r->i] != q ) r->i++;
		if ( e->nattr >= cap ) {
			cap = ( cap == 0 ) ? 8 : cap * 2;
			e->attr = ms_realloc(e->attr, sizeof(MSXA) * (size_t)cap);
		}
		e->attr[e->nattr].name = r->s + ns;
		e->attr[e->nattr].nlen = nl;
		e->attr[e->nattr].val = unescape(r->s + vs, r->i - vs);
		e->nattr++;
		if ( r->i < r->n ) r->i++;
	}
}

ER msx_parse( MSXDOC *d, const char *src, INT len )
{
	RD	r;
	MSX	*cur;

	d->src = src;
	d->len = len;
	d->root = node_new(NULL);		/* holds the top elements */
	d->root->start = 0;
	d->root->end = len;
	cur = d->root;
	r.s = src;
	r.n = len;
	r.i = 0;
	while ( r.i < r.n ) {
		if ( src[r.i] != '<' ) {
			INT	ts = r.i;

			while ( r.i < r.n && src[r.i] != '<' ) r.i++;
			if ( cur != d->root ) {
				MSX	*t = node_new(cur);

				t->text = unescape(src + ts, r.i - ts);
				t->start = ts;
				t->end = r.i;
			}
			continue;
		}
		if ( r.i + 4 <= r.n && memcmp(src + r.i, "<!--", 4) == 0 ) { skip_to(&r, "-->"); continue; }
		if ( r.i + 9 <= r.n && memcmp(src + r.i, "<![CDATA[", 9) == 0 ) {
			INT	cs = r.i + 9;
			MSX	*t;

			skip_to(&r, "]]>");
			t = node_new(cur);
			t->text = ms_alloc((size_t)( r.i - 3 - cs ) + 1);
			memcpy(t->text, src + cs, (size_t)( r.i - 3 - cs ));
			t->start = cs;
			t->end = r.i;
			continue;
		}
		if ( r.i + 1 < r.n && ( src[r.i + 1] == '?' || src[r.i + 1] == '!' ) ) { skip_to(&r, ">"); continue; }
		if ( r.i + 1 < r.n && src[r.i + 1] == '/' ) {
			/* an end tag: close up to the element it names */
			INT	ns = r.i + 2, nl;
			MSX	*e;

			r.i = ns;
			while ( r.i < r.n && name_ch(src[r.i]) ) r.i++;
			nl = r.i - ns;
			skip_to(&r, ">");
			for ( e = cur; e != d->root; e = e->parent ) {
				if ( e->tlen == nl && memcmp(e->tag, src + ns, (size_t)nl) == 0 ) break;
			}
			if ( e == d->root ) continue;		/* nothing open of that name */
			for ( ; cur != e; cur = cur->parent ) cur->end = r.i;
			cur->end = r.i;
			cur = cur->parent;
			continue;
		}
		{
			MSX	*e = node_new(cur);
			INT	ns;

			e->start = r.i;
			ns = ++r.i;
			while ( r.i < r.n && name_ch(src[r.i]) ) r.i++;
			e->tag = src + ns;
			e->tlen = r.i - ns;
			if ( read_attrs(&r, e) ) {
				e->end = r.i;
			} else {
				e->end = r.n;
				cur = e;
			}
		}
	}
	return E_OK;
}

BOOL msx_is( const MSX *e, const char *tag )
{
	INT	n = (INT)strlen(tag), i;

	if ( e == NULL || e->tag == NULL || e->tlen != n ) return FALSE;
	for ( i = 0; i < n; i++ ) {
		char	a = e->tag[i], b = tag[i];

		if ( a >= 'A' && a <= 'Z' ) a = (char)( a + 32 );
		if ( b >= 'A' && b <= 'Z' ) b = (char)( b + 32 );
		if ( a != b ) return FALSE;
	}
	return TRUE;
}

const char *msx_attr( const MSX *e, const char *name )
{
	INT	n = (INT)strlen(name), i;

	if ( e == NULL ) return NULL;
	for ( i = 0; i < e->nattr; i++ ) {
		if ( e->attr[i].nlen == n && memcmp(e->attr[i].name, name, (size_t)n) == 0 ) {
			return e->attr[i].val;
		}
	}
	return NULL;
}

double msx_num( const MSX *e, const char *name, double dflt )
{
	const char	*v = msx_attr(e, name);
	char		*end;
	double		d;

	if ( v == NULL || *v == 0 ) return dflt;
	d = strtod(v, &end);
	return ( end == v ) ? dflt : d;
}

MSX *msx_find( MSX *e, const char *tag )
{
	MSX	*c, *f;

	if ( e == NULL ) return NULL;
	for ( c = e->first; c != NULL; c = c->next ) {
		if ( msx_is(c, tag) ) return c;
		f = msx_find(c, tag);
		if ( f != NULL ) return f;
	}
	return NULL;
}

void msx_text( const MSX *e, MSBUF *b, BOOL br_newline )
{
	const MSX	*c;

	if ( e == NULL ) return;
	if ( e->tag == NULL ) {
		if ( e->text != NULL ) mb_puts(b, e->text);
		return;
	}
	for ( c = e->first; c != NULL; c = c->next ) {
		if ( c->tag != NULL && msx_is(c, "br") ) {
			if ( br_newline ) mb_putc(b, '\n');
			continue;
		}
		msx_text(c, b, br_newline);
	}
}
