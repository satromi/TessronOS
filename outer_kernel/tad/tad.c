/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad.c
 *	xmlTAD: parsing and writing (design 16.3.3, protection level 3)
 *
 *	A real object may have been written by someone else, so the text is
 *	read as something not to be trusted: every count has a limit, no
 *	construct is guessed at, and anything outside the subset below is
 *	refused with an error rather than taken for something near it.
 *
 *	The subset accepted:
 *
 *	  - UTF-8 only. The text has to be well formed UTF-8 and to hold no
 *	    control character other than tab, newline and carriage return.
 *	  - One optional <?xml ...?> declaration, as the very first thing in
 *	    the text. Its content is not read; it is kept as it stands.
 *	  - Comments <!-- ... --> anywhere outside a tag, kept as they stand.
 *	  - One root element <tad>, carrying version and encoding, the
 *	    encoding being UTF-8. Outside it only whitespace may stand.
 *	  - Elements <name a="v" ...> ... </name> and <name a="v" .../>. A
 *	    name starts with a letter or an underline and goes on with
 *	    letters, digits, underline, hyphen, full stop and colon.
 *	  - Attribute values in single or double quotes, holding no '<'. An
 *	    attribute name may not appear twice on one element.
 *	  - The references &amp; &lt; &gt; &quot; &apos; and the numeric
 *	    &#nn; and &#xnn; up to U+10FFFF, surrogates excluded.
 *	  - An end tag is exactly "</name>".
 *
 *	Refused with E_PAR: a document type, entity or notation declaration,
 *	a CDATA section, any processing instruction other than the leading
 *	declaration, an unquoted attribute value, an attribute name given
 *	twice, an unknown or malformed entity reference, a tag that is never
 *	closed, an end tag that does not match or that holds whitespace,
 *	more than one root element, a root element that is not <tad> or that
 *	does not say it is UTF-8, anything but whitespace outside the root,
 *	a byte order mark, and a NUL or other control character.
 *
 *	Refused with E_LIMIT: nesting, node count, attributes of one
 *	element, length of an attribute value and size of the document,
 *	each past what T_TADLIM says.
 *
 *	Writing gives back the bytes each node was parsed from, so a
 *	document that nothing has changed comes back as it was, down to the
 *	order of the attributes and the spaces inside a tag. Only a node
 *	that was built here is written out of its fields.
 */

#include <tk/tkernel.h>
#include <ts/ob.h>
#include "tad_local.h"

#define CHUNK_MIN	2048		/* bytes of the smallest arena chunk */

/* ---------------------------------------------------------------- memory */

/*
 * The store behind the arena. This is the one place the library asks for
 * memory, so a build outside the kernel has one pair of calls to change.
 */
LOCAL void *mem_get( SZ size )
{
	return Kmalloc(size);
}

LOCAL void mem_rel( void *p )
{
	Kfree(p);
}

LOCAL void zero( void *p, SZ n )
{
	UB	*q = (UB *)p;

	while ( n-- > 0 ) *q++ = 0;
}

LOCAL void copy( UB *dst, CONST UB *src, SZ n )
{
	while ( n-- > 0 ) *dst++ = *src++;
}

EXPORT SZ tad_slen( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != '\0' ) n++;

	return n;
}

/* An element or attribute name against a name written here */
EXPORT BOOL tad_same( CONST UB *a, CONST char *b )
{
	SZ	i = 0;

	if ( a == NULL ) {
		return FALSE;
	}
	while ( b[i] != '\0' ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
		i++;
	}

	return ( a[i] == '\0' );
}

EXPORT void *tad_alloc( T_TAD *doc, SZ size )
{
	T_TADCHUNK	*c;
	SZ		want = (size + 7) & ~(SZ)7;
	void		*p;

	c = doc->chunk;
	if ( c == NULL || c->used + want > c->size ) {
		SZ	csz = ( want > CHUNK_MIN ) ? want : CHUNK_MIN;

		c = (T_TADCHUNK *)mem_get(sizeof(T_TADCHUNK) + csz);
		if ( c == NULL ) {
			return NULL;
		}
		c->next = doc->chunk;
		c->size = csz;
		c->used = 0;
		doc->chunk = c;
	}
	p = (UB *)(c + 1) + c->used;
	c->used += want;
	zero(p, size);

	return p;
}

EXPORT UB *tad_dup( T_TAD *doc, CONST UB *s, SZ len )
{
	UB	*p = (UB *)tad_alloc(doc, len + 1);

	if ( p != NULL ) {
		copy(p, s, len);
		p[len] = '\0';
	}

	return p;
}

/* ---------------------------------------------------------------- nodes */

EXPORT T_TADNODE *tad_new_node( T_TAD *doc, UINT kind )
{
	T_TADNODE	*nd;

	if ( doc->nnode >= doc->lim.nodes ) {
		return NULL;
	}
	nd = (T_TADNODE *)tad_alloc(doc, sizeof(T_TADNODE));
	if ( nd == NULL ) {
		return NULL;
	}
	nd->kind = kind;
	doc->nnode++;

	return nd;
}

EXPORT void tad_add_child( T_TADNODE *parent, T_TADNODE *nd )
{
	nd->parent = parent;
	nd->next   = NULL;
	if ( parent->last == NULL ) {
		parent->first = nd;
	} else {
		parent->last->next = nd;
	}
	parent->last = nd;
}

/* At the top level the nodes are a list of their own, with no parent */
LOCAL void add_top( T_TAD *doc, T_TADNODE *nd )
{
	nd->parent = NULL;
	nd->next   = NULL;
	if ( doc->tail == NULL ) {
		doc->head = nd;
	} else {
		doc->tail->next = nd;
	}
	doc->tail = nd;
}

EXPORT ER tad_put_delta( T_TAD *doc, CONST TS_UUID *target, INT delta )
{
	T_TADDELTA	*d = (T_TADDELTA *)tad_alloc(doc, sizeof(T_TADDELTA));

	if ( d == NULL ) {
		return E_NOMEM;
	}
	d->target = *target;
	d->delta  = delta;
	if ( doc->delta_last == NULL ) {
		doc->delta = d;
	} else {
		doc->delta_last->next = d;
	}
	doc->delta_last = d;
	doc->ndelta++;

	return E_OK;
}

/* ---------------------------------------------------------------- text */

LOCAL BOOL is_space( UB c )
{
	return ( c == ' ' || c == '\t' || c == '\n' || c == '\r' );
}

LOCAL BOOL is_name_start( UB c )
{
	return ( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' );
}

LOCAL BOOL is_name( UB c )
{
	return ( is_name_start(c) || (c >= '0' && c <= '9')
	      || c == '-' || c == '.' || c == ':' );
}

LOCAL BOOL all_space( CONST UB *s, SZ len )
{
	SZ	i;

	for ( i = 0; i < len; i++ ) {
		if ( !is_space(s[i]) ) return FALSE;
	}

	return TRUE;
}

/*
 * The whole text at once: well formed UTF-8, no overlong form, no
 * surrogate, nothing past U+10FFFF, and no control character but tab,
 * newline and carriage return. A byte order mark is refused as well,
 * because the record is UTF-8 by definition and a mark in the middle of
 * a record would be a character of its own.
 */
LOCAL ER check_text( CONST UB *s, SZ len )
{
	SZ	i = 0;

	if ( len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF ) {
		return E_PAR;
	}
	while ( i < len ) {
		UB	c = s[i];
		INT	n;
		UW	v;

		if ( c < 0x80 ) {
			if ( c < 0x20 && c != '\t' && c != '\n' && c != '\r' ) {
				return E_PAR;
			}
			i++;
			continue;
		}
		if ( (c & 0xE0) == 0xC0 ) { n = 1; v = c & 0x1F; }
		else if ( (c & 0xF0) == 0xE0 ) { n = 2; v = c & 0x0F; }
		else if ( (c & 0xF8) == 0xF0 ) { n = 3; v = c & 0x07; }
		else return E_PAR;

		if ( i + (SZ)n >= len ) {
			return E_PAR;		/* the sequence runs off the end */
		}
		{
			INT k;

			for ( k = 1; k <= n; k++ ) {
				UB d = s[i + k];

				if ( (d & 0xC0) != 0x80 ) return E_PAR;
				v = (v << 6) | (d & 0x3F);
			}
		}
		if ( n == 1 && v < 0x80 ) return E_PAR;
		if ( n == 2 && v < 0x800 ) return E_PAR;
		if ( n == 3 && v < 0x10000 ) return E_PAR;
		if ( v >= 0xD800 && v <= 0xDFFF ) return E_PAR;
		if ( v > 0x10FFFF ) return E_PAR;
		i += (SZ)(n + 1);
	}

	return E_OK;
}

/* The bytes one code point takes as UTF-8 */
LOCAL SZ utf8_len( UW v )
{
	if ( v < 0x80 ) return 1;
	if ( v < 0x800 ) return 2;
	if ( v < 0x10000 ) return 3;

	return 4;
}

/* One code point as UTF-8; returns the bytes written */
LOCAL SZ put_utf8( UB *out, UW v )
{
	if ( v < 0x80 ) {
		out[0] = (UB)v;
		return 1;
	}
	if ( v < 0x800 ) {
		out[0] = (UB)(0xC0 | (v >> 6));
		out[1] = (UB)(0x80 | (v & 0x3F));
		return 2;
	}
	if ( v < 0x10000 ) {
		out[0] = (UB)(0xE0 | (v >> 12));
		out[1] = (UB)(0x80 | ((v >> 6) & 0x3F));
		out[2] = (UB)(0x80 | (v & 0x3F));
		return 3;
	}
	out[0] = (UB)(0xF0 | (v >> 18));
	out[1] = (UB)(0x80 | ((v >> 12) & 0x3F));
	out[2] = (UB)(0x80 | ((v >> 6) & 0x3F));
	out[3] = (UB)(0x80 | (v & 0x3F));

	return 4;
}

/*
 * Resolve the entity references of a run of text. Nothing grows, so a
 * buffer of len + 1 bytes always holds the answer. With p_out NULL the
 * run is only checked, which is what the first pass over a tag does.
 */
LOCAL ER unesc( T_TAD *doc, CONST UB *s, SZ len, UB **p_out )
{
	UB	*out = NULL;
	SZ	i = 0, o = 0;

	if ( p_out != NULL ) {
		out = (UB *)tad_alloc(doc, len + 1);
		if ( out == NULL ) {
			return E_NOMEM;
		}
	}
	while ( i < len ) {
		SZ	j;
		UW	v = 0;

		if ( s[i] != '&' ) {
			if ( out != NULL ) out[o] = s[i];
			o++;
			i++;
			continue;
		}
		for ( j = i + 1; j < len && s[j] != ';'; j++ ) {
			if ( j - i > 12 ) return E_PAR;
		}
		if ( j >= len ) {
			return E_PAR;		/* no semicolon */
		}
		if ( s[i + 1] == '#' ) {
			SZ	k = i + 2;
			INT	base = 10, digits = 0;

			if ( k < j && (s[k] == 'x' || s[k] == 'X') ) {
				base = 16;
				k++;
			}
			for ( ; k < j; k++ ) {
				UB d = s[k];
				UW dv;

				if ( d >= '0' && d <= '9' ) dv = d - '0';
				else if ( base == 16 && d >= 'a' && d <= 'f' ) dv = d - 'a' + 10;
				else if ( base == 16 && d >= 'A' && d <= 'F' ) dv = d - 'A' + 10;
				else return E_PAR;
				v = v * (UW)base + dv;
				if ( v > 0x10FFFF ) return E_PAR;
				digits++;
			}
			if ( digits == 0 ) {
				return E_PAR;
			}
			if ( v >= 0xD800 && v <= 0xDFFF ) return E_PAR;
			if ( v < 0x20 && v != '\t' && v != '\n' && v != '\r' ) return E_PAR;
		} else {
			CONST UB *e = s + i + 1;
			SZ	 elen = j - i - 1;

			if ( elen == 3 && e[0] == 'a' && e[1] == 'm' && e[2] == 'p' ) v = '&';
			else if ( elen == 2 && e[0] == 'l' && e[1] == 't' ) v = '<';
			else if ( elen == 2 && e[0] == 'g' && e[1] == 't' ) v = '>';
			else if ( elen == 4 && e[0] == 'q' && e[1] == 'u'
			       && e[2] == 'o' && e[3] == 't' ) v = '"';
			else if ( elen == 4 && e[0] == 'a' && e[1] == 'p'
			       && e[2] == 'o' && e[3] == 's' ) v = '\'';
			else return E_PAR;	/* nothing else is defined here */
		}
		if ( out != NULL ) {
			o += put_utf8(out + o, v);
		} else {
			o += utf8_len(v);
		}
		i = j + 1;
	}
	if ( out != NULL ) {
		out[o] = '\0';
		*p_out = out;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- parsing */

/*
 * The attributes of one start tag, from just past the name to just past
 * the '>' that ends it. With nd NULL they are counted and checked; with
 * nd given they are put in its table, which the count made room for.
 */
LOCAL ER scan_attrs( T_TAD *doc, CONST UB **pp, CONST UB *end, T_TADNODE *nd,
		     INT *p_n, BOOL *p_empty )
{
	CONST UB	*p = *pp;
	INT		n = 0;
	ER		er;

	for ( ;; ) {
		CONST UB	*ns, *vs;
		SZ		nlen, vlen, ws = 0;
		UB		quote;

		while ( p < end && is_space(*p) ) { p++; ws++; }
		if ( p >= end ) {
			return E_PAR;
		}
		if ( *p == '/' ) {
			p++;
			if ( p >= end || *p != '>' ) return E_PAR;
			p++;
			*p_empty = TRUE;
			break;
		}
		if ( *p == '>' ) {
			p++;
			*p_empty = FALSE;
			break;
		}
		if ( ws == 0 ) {
			return E_PAR;		/* two attributes run together */
		}
		ns = p;
		if ( !is_name_start(*p) ) {
			return E_PAR;
		}
		while ( p < end && is_name(*p) ) p++;
		nlen = (SZ)(p - ns);

		while ( p < end && is_space(*p) ) p++;
		if ( p >= end || *p != '=' ) {
			return E_PAR;
		}
		p++;
		while ( p < end && is_space(*p) ) p++;
		if ( p >= end || (*p != '"' && *p != '\'') ) {
			return E_PAR;		/* the value has to be quoted */
		}
		quote = *p;
		p++;
		vs = p;
		while ( p < end && *p != quote ) {
			if ( *p == '<' ) return E_PAR;
			p++;
		}
		if ( p >= end ) {
			return E_PAR;
		}
		vlen = (SZ)(p - vs);
		p++;
		if ( vlen > doc->lim.attrlen ) {
			return E_LIMIT;
		}
		if ( nd == NULL ) {
			er = unesc(doc, vs, vlen, NULL);
			if ( er < E_OK ) return er;
		} else {
			INT	k;
			UB	*name = tad_dup(doc, ns, nlen);

			if ( name == NULL ) return E_NOMEM;
			for ( k = 0; k < n; k++ ) {
				if ( tad_same(nd->attr[k].name, (CONST char *)name ) ) {
					return E_PAR;	/* the same attribute twice */
				}
			}
			nd->attr[n].name = name;
			er = unesc(doc, vs, vlen, &nd->attr[n].value);
			if ( er < E_OK ) return er;
		}
		n++;
		if ( n > doc->lim.attrs ) {
			return E_LIMIT;
		}
	}
	*p_n = n;
	*pp  = p;

	return E_OK;
}

/* One start tag, from the '<' to just past the '>' that ends it */
LOCAL ER parse_elem( T_TAD *doc, CONST UB **pp, CONST UB *end, T_TADNODE **p_nd )
{
	CONST UB	*start = *pp, *p = *pp, *ns;
	T_TADNODE	*nd;
	INT		n = 0, n2 = 0;
	BOOL		empty = FALSE;
	SZ		nlen;
	ER		er;

	p++;					/* past the '<' */
	ns = p;
	if ( p >= end || !is_name_start(*p) ) {
		return E_PAR;
	}
	while ( p < end && is_name(*p) ) p++;
	nlen = (SZ)(p - ns);

	{
		CONST UB *q = p;

		er = scan_attrs(doc, &q, end, NULL, &n, &empty);
		if ( er < E_OK ) {
			return er;
		}
	}

	nd = tad_new_node(doc, TAD_ND_ELEM);
	if ( nd == NULL ) {
		return E_LIMIT;
	}
	nd->name = tad_dup(doc, ns, nlen);
	if ( nd->name == NULL ) {
		return E_NOMEM;
	}
	if ( n > 0 ) {
		nd->attr = (T_TADATTR *)tad_alloc(doc, sizeof(T_TADATTR) * (SZ)n);
		if ( nd->attr == NULL ) {
			return E_NOMEM;
		}
	}
	er = scan_attrs(doc, &p, end, nd, &n2, &empty);
	if ( er < E_OK ) {
		return er;
	}
	nd->nattr  = n2;
	nd->empty  = empty;
	nd->raw    = start;
	nd->rawlen = (SZ)(p - start);

	*pp   = p;
	*p_nd = nd;

	return E_OK;
}

LOCAL ER parse_all( T_TAD *doc )
{
	CONST UB	*p = doc->src;
	CONST UB	*end = doc->src + doc->srclen;
	T_TADNODE	*cur = NULL, *nd;
	INT		depth = 0;
	BOOL		seen_root = FALSE;
	ER		er;

	while ( p < end ) {
		if ( *p != '<' ) {
			CONST UB *ts = p;

			while ( p < end && *p != '<' ) p++;
			if ( cur == NULL && !all_space(ts, (SZ)(p - ts)) ) {
				return E_PAR;	/* text outside the root element */
			}
			nd = tad_new_node(doc, TAD_ND_TEXT);
			if ( nd == NULL ) {
				return E_LIMIT;
			}
			er = unesc(doc, ts, (SZ)(p - ts), &nd->text);
			if ( er < E_OK ) {
				return er;
			}
			nd->raw    = ts;
			nd->rawlen = (SZ)(p - ts);
			if ( cur == NULL ) add_top(doc, nd); else tad_add_child(cur, nd);
			continue;
		}
		if ( p + 1 >= end ) {
			return E_PAR;
		}
		if ( p[1] == '/' ) {
			SZ	nlen;

			if ( cur == NULL ) {
				return E_PAR;	/* an end tag with nothing open */
			}
			nlen = tad_slen((CONST char *)cur->name);
			if ( (SZ)(end - p) < nlen + 3 ) {
				return E_PAR;
			}
			{
				SZ k;

				for ( k = 0; k < nlen; k++ ) {
					if ( p[2 + k] != cur->name[k] ) return E_PAR;
				}
			}
			if ( p[2 + nlen] != '>' ) {
				return E_PAR;	/* "</name>" and nothing else */
			}
			p += nlen + 3;
			cur = cur->parent;
			depth--;
			continue;
		}
		if ( p[1] == '!' ) {
			CONST UB *q;

			if ( (SZ)(end - p) < 7 || p[2] != '-' || p[3] != '-' ) {
				return E_PAR;	/* a declaration or a CDATA section */
			}
			for ( q = p + 4; q + 2 < end; q++ ) {
				if ( q[0] == '-' && q[1] == '-' && q[2] == '>' ) break;
			}
			if ( q + 2 >= end ) {
				return E_PAR;	/* the comment is never closed */
			}
			nd = tad_new_node(doc, TAD_ND_COMMENT);
			if ( nd == NULL ) {
				return E_LIMIT;
			}
			nd->raw    = p;
			nd->rawlen = (SZ)(q + 3 - p);
			if ( cur == NULL ) add_top(doc, nd); else tad_add_child(cur, nd);
			p = q + 3;
			continue;
		}
		if ( p[1] == '?' ) {
			CONST UB *q;

			if ( doc->head != NULL || cur != NULL ) {
				return E_PAR;	/* only at the head of the text */
			}
			if ( (SZ)(end - p) < 6 || p[2] != 'x' || p[3] != 'm'
			  || p[4] != 'l' || !is_space(p[5]) ) {
				return E_PAR;	/* no other processing instruction */
			}
			for ( q = p + 5; q + 1 < end; q++ ) {
				if ( q[0] == '?' && q[1] == '>' ) break;
			}
			if ( q + 1 >= end ) {
				return E_PAR;
			}
			nd = tad_new_node(doc, TAD_ND_DECL);
			if ( nd == NULL ) {
				return E_LIMIT;
			}
			nd->raw    = p;
			nd->rawlen = (SZ)(q + 2 - p);
			add_top(doc, nd);
			p = q + 2;
			continue;
		}
		if ( cur == NULL && seen_root ) {
			return E_PAR;		/* a second root element */
		}
		er = parse_elem(doc, &p, end, &nd);
		if ( er < E_OK ) {
			return er;
		}
		if ( cur == NULL ) {
			add_top(doc, nd);
			doc->root = nd;
			seen_root = TRUE;
		} else {
			tad_add_child(cur, nd);
		}
		if ( !nd->empty ) {
			depth++;
			if ( depth > doc->lim.depth ) {
				return E_LIMIT;
			}
			cur = nd;
		}
	}
	if ( cur != NULL ) {
		return E_PAR;			/* an element is never closed */
	}
	if ( !seen_root ) {
		return E_PAR;
	}

	return E_OK;
}

/* The root element is <tad>, it says which version it is, and it is UTF-8 */
LOCAL ER check_root( CONST T_TAD *doc )
{
	CONST UB	*enc;

	if ( doc->root == NULL || !tad_same(doc->root->name, "tad") ) {
		return E_PAR;
	}
	if ( tad_attr(doc->root, "version") == NULL ) {
		return E_PAR;
	}
	enc = tad_attr(doc->root, "encoding");
	if ( enc == NULL ) {
		return E_PAR;
	}
	{
		CONST char	*want = "utf-8";
		INT		i;

		for ( i = 0; i < 5; i++ ) {
			UB c = enc[i];

			if ( c >= 'A' && c <= 'Z' ) c = (UB)(c - 'A' + 'a');
			if ( c != (UB)want[i] ) return E_PAR;
		}
		if ( enc[5] != '\0' ) {
			return E_PAR;
		}
	}

	return E_OK;
}

EXPORT ER tad_parse( CONST UB *xml, SZ size, CONST T_TADLIM *lim, T_TAD **p_doc )
{
	T_TAD	*doc;
	ER	er;

	if ( xml == NULL || p_doc == NULL || size == 0 ) {
		return E_PAR;
	}
	doc = (T_TAD *)mem_get(sizeof(T_TAD));
	if ( doc == NULL ) {
		return E_NOMEM;
	}
	zero(doc, sizeof(T_TAD));

	doc->lim.depth   = ( lim != NULL && lim->depth   > 0 ) ? lim->depth   : TAD_DEF_DEPTH;
	doc->lim.nodes   = ( lim != NULL && lim->nodes   > 0 ) ? lim->nodes   : TAD_DEF_NODES;
	doc->lim.attrs   = ( lim != NULL && lim->attrs   > 0 ) ? lim->attrs   : TAD_DEF_ATTRS;
	doc->lim.attrlen = ( lim != NULL && lim->attrlen > 0 ) ? lim->attrlen : TAD_DEF_ATTRLEN;
	doc->lim.docsize = ( lim != NULL && lim->docsize > 0 ) ? lim->docsize : TAD_DEF_DOCSIZE;

	if ( size > doc->lim.docsize ) {
		tad_free(doc);
		return E_LIMIT;
	}
	doc->src = tad_dup(doc, xml, size);
	if ( doc->src == NULL ) {
		tad_free(doc);
		return E_NOMEM;
	}
	doc->srclen = size;

	er = check_text(doc->src, doc->srclen);
	if ( er >= E_OK ) {
		er = parse_all(doc);
	}
	if ( er >= E_OK ) {
		er = check_root(doc);
	}
	if ( er < E_OK ) {
		tad_free(doc);
		return er;
	}
	*p_doc = doc;

	return E_OK;
}

EXPORT void tad_free( T_TAD *doc )
{
	T_TADCHUNK	*c, *next;

	if ( doc == NULL ) {
		return;
	}
	for ( c = doc->chunk; c != NULL; c = next ) {
		next = c->next;
		mem_rel(c);
	}
	mem_rel(doc);
}

/* ---------------------------------------------------------------- walking */

EXPORT T_TADNODE *tad_root( CONST T_TAD *doc )
{
	return ( doc != NULL ) ? doc->root : NULL;
}

/* The element the content is in: <document>, <figure> or <realtime> */
EXPORT T_TADNODE *tad_body( CONST T_TAD *doc )
{
	T_TADNODE	*nd;

	if ( doc == NULL || doc->root == NULL ) {
		return NULL;
	}
	for ( nd = doc->root->first; nd != NULL; nd = nd->next ) {
		if ( nd->kind == TAD_ND_ELEM ) return nd;
	}

	return NULL;
}

/*
 * The next node in the order the document holds them: the first child,
 * else the next sibling, else the next sibling of the nearest parent
 * that has one. With cur NULL it starts at the first node of all.
 */
EXPORT T_TADNODE *tad_walk( CONST T_TAD *doc, CONST T_TADNODE *cur )
{
	T_TADNODE	*nd;

	if ( doc == NULL ) {
		return NULL;
	}
	if ( cur == NULL ) {
		return doc->head;
	}
	if ( cur->first != NULL ) {
		return cur->first;
	}
	nd = (T_TADNODE *)cur;
	while ( nd != NULL ) {
		if ( nd->next != NULL ) return nd->next;
		nd = nd->parent;
	}

	return NULL;
}

EXPORT CONST UB *tad_attr( CONST T_TADNODE *nd, CONST char *name )
{
	INT	i;

	if ( nd == NULL || nd->attr == NULL ) {
		return NULL;
	}
	for ( i = 0; i < nd->nattr; i++ ) {
		if ( tad_same(nd->attr[i].name, name) ) return nd->attr[i].value;
	}

	return NULL;
}

/* ---------------------------------------------------------------- writing */

typedef struct {
	UB	*buf;			/* NULL while only measuring */
	SZ	size;
	SZ	used;
	BOOL	over;			/* the buffer given was too small */
} TADOUT;

LOCAL void out_put( TADOUT *o, CONST UB *s, SZ n )
{
	if ( o->buf != NULL ) {
		if ( o->used + n > o->size ) {
			o->over = TRUE;
		} else {
			copy(o->buf + o->used, s, n);
		}
	}
	o->used += n;
}

LOCAL void out_str( TADOUT *o, CONST char *s )
{
	out_put(o, (CONST UB *)s, tad_slen(s));
}

LOCAL void out_esc( TADOUT *o, CONST UB *s, SZ len, BOOL in_attr )
{
	SZ	i;

	for ( i = 0; i < len; i++ ) {
		switch ( s[i] ) {
		case '&':	out_str(o, "&amp;");	break;
		case '<':	out_str(o, "&lt;");	break;
		case '>':	out_str(o, "&gt;");	break;
		case '"':
			if ( in_attr ) out_str(o, "&quot;");
			else out_put(o, s + i, 1);
			break;
		default:	out_put(o, s + i, 1);	break;
		}
	}
}

/*
 * A node that was parsed is written back byte for byte. One that was
 * built here is written out of its fields, in the order they were put
 * there.
 */
LOCAL void wr_open( TADOUT *o, CONST T_TADNODE *nd )
{
	INT	i;

	if ( nd->raw != NULL ) {
		out_put(o, nd->raw, nd->rawlen);
		return;
	}
	switch ( nd->kind ) {
	case TAD_ND_ELEM:
		out_str(o, "<");
		out_put(o, nd->name, tad_slen((CONST char *)nd->name));
		for ( i = 0; i < nd->nattr; i++ ) {
			out_str(o, " ");
			out_put(o, nd->attr[i].name,
				tad_slen((CONST char *)nd->attr[i].name));
			out_str(o, "=\"");
			out_esc(o, nd->attr[i].value,
				tad_slen((CONST char *)nd->attr[i].value), TRUE);
			out_str(o, "\"");
		}
		out_str(o, ( nd->empty ) ? "/>" : ">");
		break;
	case TAD_ND_TEXT:
		if ( nd->text != NULL ) {
			out_esc(o, nd->text, tad_slen((CONST char *)nd->text), FALSE);
		}
		break;
	default:
		break;
	}
}

LOCAL void wr_close( TADOUT *o, CONST T_TADNODE *nd )
{
	if ( nd->kind != TAD_ND_ELEM || nd->empty ) {
		return;
	}
	out_str(o, "</");
	out_put(o, nd->name, tad_slen((CONST char *)nd->name));
	out_str(o, ">");
}

EXPORT ER tad_write_mem( CONST T_TAD *doc, UB *buf, SZ size, SZ *p_asize )
{
	TADOUT		o;
	T_TADNODE	*nd;

	if ( doc == NULL || p_asize == NULL ) {
		return E_PAR;
	}
	o.buf  = buf;
	o.size = size;
	o.used = 0;
	o.over = FALSE;

	nd = doc->head;
	while ( nd != NULL ) {
		wr_open(&o, nd);
		if ( nd->kind == TAD_ND_ELEM && !nd->empty && nd->first != NULL ) {
			nd = nd->first;
			continue;
		}
		for ( ;; ) {
			wr_close(&o, nd);
			if ( nd->next != NULL ) {
				nd = nd->next;
				break;
			}
			nd = nd->parent;
			if ( nd == NULL ) break;
		}
	}
	*p_asize = o.used;

	return ( o.over ) ? E_NOMEM : E_OK;
}

/*
 * Put the document in a record of an open object (a key of the object
 * layer, design 18.3) and cut the record to it. The reference counts of
 * the objects the links point at are not touched here: that belongs to
 * om_wri_doc, which does both in one transaction (design 16.3.4).
 */
EXPORT ER tad_write( ID key, INT recno, CONST T_TAD *doc )
{
	UB	*buf;
	SZ	need = 0, asize = 0;
	ER	er;

	er = tad_write_mem(doc, NULL, 0, &need);
	if ( er < E_OK ) {
		return er;
	}
	buf = (UB *)mem_get(need);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = tad_write_mem(doc, buf, need, &asize);
	if ( er >= E_OK ) {
		er = ob_wri_rec(key, recno, 0, buf, asize, &asize);
	}
	mem_rel(buf);
	if ( er < E_OK ) {
		return er;
	}

	return ob_trn_rec(key, recno, (UD)need);
}
