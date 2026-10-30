/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_tad.c
 *	基本表計算: the sheet and its record (design 11.13)
 *
 *	The record is read as the patterns of its elements, the way a record
 *	of either system is: a cell is what follows its <calcPos/> up to the
 *	next tab, the next <calcPos/> or the paragraph's end, on one line.
 *	What a cell is written as is the same elements in the same order,
 *	so a sheet written here opens as it was elsewhere and the other way
 *	round. A look left at what is usual -- the letters black, 12 point,
 *	the ground white, placed left and in the middle -- is not written.
 */

#include "calc_tad.h"

#define DEF_COLOUR	"#000000"
#define DEF_GROUND	"#ffffff"
#define DEF_FACE	"MS Gothic"
#define DEF_SIZE	12

/* ---------------------------------------------------------------- bytes */

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL void s_copy( char *d, CONST UB *s, INT n, INT max )
{
	INT	i;

	for ( i = 0; i < n && i < max - 1; i++ ) d[i] = (char)s[i];
	d[i] = 0;
}

LOCAL BOOL s_eq( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 || b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL void m_copy( void *d, CONST void *s, INT n )
{
	UB		*dd = (UB *)d;
	CONST UB	*ss = (CONST UB *)s;

	if ( dd < ss ) {
		while ( n-- > 0 ) *dd++ = *ss++;
	} else {
		dd += n;
		ss += n;
		while ( n-- > 0 ) *--dd = *--ss;
	}
}

LOCAL void m_zero( void *d, INT n )
{
	UB	*p = (UB *)d;

	while ( n-- > 0 ) *p++ = 0;
}

/* Whether the bytes at s (n of them left) begin with w */
LOCAL BOOL at_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( i >= n || s[i] != (UB)w[i] ) return FALSE;
	}
	return TRUE;
}

/* The same, the letters of w in either case */
LOCAL BOOL at_is_i( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		UB	a = s[i], b = (UB)w[i];

		if ( i >= n ) return FALSE;
		if ( a >= 'A' && a <= 'Z' ) a += 0x20;
		if ( b >= 'A' && b <= 'Z' ) b += 0x20;
		if ( a != b ) return FALSE;
	}
	return TRUE;
}

LOCAL INT find( CONST UB *s, INT n, INT from, CONST char *w )
{
	INT	i;

	for ( i = from; i < n; i++ ) {
		if ( s[i] == (UB)w[0] && at_is(s + i, n - i, w) ) return i;
	}
	return -1;
}

LOCAL BOOL is_space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' );
}

LOCAL BOOL is_digit( UB c )
{
	return (BOOL)( c >= '0' && c <= '9' );
}

LOCAL BOOL is_upper( UB c )
{
	return (BOOL)( c >= 'A' && c <= 'Z' );
}

/* Decimal digits from s[*p_at]; how many, the value in *p_v (held at a large number) */
LOCAL INT get_num( CONST UB *s, INT n, INT *p_at, D *p_v )
{
	INT	k = 0;
	D	v = 0;

	while ( *p_at < n && is_digit(s[*p_at]) ) {
		if ( v < 100000000000LL ) v = v * 10 + ( s[*p_at] - '0' );
		( *p_at )++;
		k++;
	}
	*p_v = v;
	return k;
}

/* The integer an attribute's value begins with, rounded, as a number written with a fraction is; dflt for none */
LOCAL INT num_of( CONST UB *s, INT n, INT dflt )
{
	INT	i = 0, sign = 1, half = 0;
	D	v;

	while ( i < n && is_space(s[i]) ) i++;
	if ( i < n && ( s[i] == '-' || s[i] == '+' ) ) {
		if ( s[i] == '-' ) sign = -1;
		i++;
	}
	if ( i >= n || ( !is_digit(s[i]) && s[i] != '.' ) ) return dflt;
	(void)get_num(s, n, &i, &v);
	if ( i + 1 < n && s[i] == '.' && is_digit(s[i + 1]) && s[i + 1] >= '5' ) half = 1;
	if ( sign < 0 ) return (INT)( -v - ( ( half && s[i + 1] > '5' ) ? 1 : 0 ) );
	return (INT)( v + half );
}

/* The integer an attribute's value begins with, its fraction cut off; dflt for none */
LOCAL INT int_of( CONST UB *s, INT n, INT dflt )
{
	INT	i = 0, sign = 1;
	D	v;

	while ( i < n && is_space(s[i]) ) i++;
	if ( i < n && ( s[i] == '-' || s[i] == '+' ) ) {
		if ( s[i] == '-' ) sign = -1;
		i++;
	}
	if ( get_num(s, n, &i, &v) == 0 ) return dflt;
	return (INT)( sign * v );
}

/* ---------------------------------------------------------------- names */

EXPORT INT cx_col_name( INT col, char *out )
{
	char	t[8];
	INT	n = 0, k = 0;

	while ( col > 0 && n < 7 ) {
		col--;
		t[n++] = (char)( 'A' + col % 26 );
		col /= 26;
	}
	while ( n > 0 ) out[k++] = t[--n];
	out[k] = 0;
	return k;
}

EXPORT INT cx_col_num( CONST UB *s, INT n )
{
	INT	i;
	D	col = 0;

	for ( i = 0; i < n; i++ ) {
		col = col * 26 + ( (INT)s[i] - 64 );
		if ( col > 0x3FFFFFFF || col < -0x3FFFFFFF ) return 0x3FFFFFFF;
	}
	return (INT)col;
}

EXPORT INT cx_cell_name( INT col, INT row, char *out )
{
	INT	n = cx_col_name(col, out), i = 0;
	char	d[12];

	do {
		d[i++] = (char)( '0' + row % 10 );
		row /= 10;
	} while ( row > 0 && i < 11 );
	while ( i > 0 ) out[n++] = d[--i];
	out[n] = 0;
	return n;
}

/* [A-Z]+ then \d+ at s[*p_at] */
LOCAL BOOL cell_at( CONST UB *s, INT n, INT *p_at, INT *p_col, INT *p_row )
{
	INT	i = *p_at, a = i;
	D	r;

	while ( i < n && is_upper(s[i]) ) i++;
	if ( i == a ) return FALSE;
	*p_col = cx_col_num(s + a, i - a);
	if ( get_num(s, n, &i, &r) == 0 ) return FALSE;
	*p_row = ( r > 0x7FFFFFFF ) ? 0x7FFFFFFF : (INT)r;
	*p_at = i;
	return TRUE;
}

EXPORT BOOL cx_cell_parse( CONST UB *s, INT n, INT *p_col, INT *p_row )
{
	INT	i = 0;

	return (BOOL)( cell_at(s, n, &i, p_col, p_row) && i == n );
}

EXPORT BOOL cx_range_parse( CONST UB *s, INT n, CXRANGE *r )
{
	INT	i = 0;

	if ( !cell_at(s, n, &i, &r->c0, &r->r0) || i >= n || s[i] != ':' ) return FALSE;
	i++;
	return (BOOL)( cell_at(s, n, &i, &r->c1, &r->r1) && i == n );
}

/* ---------------------------------------------------------------- the sheet */

EXPORT void cx_init( CXSHEET *s )
{
	m_zero(s, sizeof(*s));
	s->maxcol = CX_COLS;
	s->maxrow = CX_ROWS;
}

LOCAL void cell_free( CXCELL *c )
{
	if ( c == NULL ) return;
	cx_free(c->text);
	cx_free(c->shown);
	cx_free(c->el);
	cx_free(c);
}

EXPORT void cx_free_all( CXSHEET *s )
{
	INT	i;

	for ( i = 0; i < s->n; i++ ) cell_free(s->c[i]);
	for ( i = 0; i < s->nch; i++ ) cx_free(s->ch[i].el);
	cx_free(s->c);
	cx_free(s->w);
	cx_free(s->h);
	cx_free(s->m);
	cx_free(s->ch);
	cx_free(s->paper);
	cx_free(s->margin);
	cx_init(s);
}

/* Where (col, row) is or would go among the cells */
LOCAL INT cell_slot( CONST CXSHEET *s, INT col, INT row, BOOL *p_there )
{
	INT	lo = 0, hi = s->n;

	while ( lo < hi ) {
		INT		mid = ( lo + hi ) / 2;
		CONST CXCELL	*c = s->c[mid];

		if ( c->row < row || ( c->row == row && c->col < col ) ) lo = mid + 1;
		else hi = mid;
	}
	*p_there = (BOOL)( lo < s->n && s->c[lo]->row == row && s->c[lo]->col == col );
	return lo;
}

EXPORT CXCELL *cx_get( CONST CXSHEET *s, INT col, INT row )
{
	BOOL	there;
	INT	i = cell_slot(s, col, row, &there);

	return there ? s->c[i] : NULL;
}

/* Room for one more of something kept in an array */
LOCAL BOOL room( void **p, INT n, INT *p_max, INT size )
{
	void	*q;
	INT	m;

	if ( n < *p_max ) return TRUE;
	m = ( *p_max == 0 ) ? 16 : *p_max * 2;
	q = cx_alloc(m * size);
	if ( q == NULL ) return FALSE;
	if ( *p ) m_copy(q, *p, n * size);
	cx_free(*p);
	*p = q;
	*p_max = m;
	return TRUE;
}

LOCAL UB *dup_bytes( CONST UB *s, INT n )
{
	UB	*d = (UB *)cx_alloc(n + 1);

	if ( d == NULL ) return NULL;
	m_copy(d, s, n);
	d[n] = 0;
	return d;
}

EXPORT CXCELL *cx_make( CXSHEET *s, INT col, INT row )
{
	BOOL	there;
	INT	i = cell_slot(s, col, row, &there);
	CXCELL	*c;

	if ( there ) return s->c[i];
	if ( !room((void **)&s->c, s->n, &s->max, sizeof(CXCELL *)) ) return NULL;
	c = (CXCELL *)cx_alloc(sizeof(CXCELL));
	if ( c == NULL ) return NULL;
	m_zero(c, sizeof(*c));
	c->col = col;
	c->row = row;
	c->st = &cx_plain;
	c->text = dup_bytes((CONST UB *)"", 0);
	if ( c->text == NULL ) {
		cx_free(c);
		return NULL;
	}
	m_copy(&s->c[i + 1], &s->c[i], ( s->n - i ) * (INT)sizeof(CXCELL *));
	s->c[i] = c;
	s->n++;
	return c;
}

EXPORT void cx_remove( CXSHEET *s, INT col, INT row )
{
	BOOL	there;
	INT	i = cell_slot(s, col, row, &there);

	if ( !there ) return;
	cell_free(s->c[i]);
	m_copy(&s->c[i], &s->c[i + 1], ( s->n - i - 1 ) * (INT)sizeof(CXCELL *));
	s->n--;
}

EXPORT INT cx_remove_range( CXSHEET *s, CONST CXRANGE *r )
{
	INT	i, k = 0, gone = 0;

	/* in one walk, the others closed up behind */
	for ( i = 0; i < s->n; i++ ) {
		CXCELL	*c = s->c[i];

		if ( c->col >= r->c0 && c->col <= r->c1 && c->row >= r->r0 && c->row <= r->r1 ) {
			cell_free(c);
			gone++;
			continue;
		}
		s->c[k++] = c;
	}
	s->n = k;
	return gone;
}

EXPORT BOOL cx_set_text( CXCELL *c, CONST UB *text )
{
	UB	*t = dup_bytes(text, s_len(text));

	if ( t == NULL ) return FALSE;
	cx_free(c->text);
	c->text = t;
	cx_free(c->el);
	c->el = NULL;
	c->ellen = 0;
	c->kind = CX_TEXT;
	cx_free(c->shown);
	c->shown = NULL;
	return TRUE;
}

EXPORT BOOL cx_set_el( CXCELL *c, INT kind, CONST UB *el, INT len )
{
	UB	*e = dup_bytes(el, len), *t = dup_bytes((CONST UB *)"", 0);

	if ( e == NULL || t == NULL ) {
		cx_free(e);
		cx_free(t);
		return FALSE;
	}
	cx_free(c->el);
	cx_free(c->text);
	cx_free(c->shown);
	c->el = e;
	c->ellen = len;
	c->text = t;
	c->shown = NULL;
	c->kind = kind;
	return TRUE;
}

EXPORT BOOL cx_set_shown( CXCELL *c, CONST UB *s, INT len )
{
	UB	*d;
	INT	i;

	/* what it shows is its text: nothing kept apart (a sheet of numbers keeps them once) */
	for ( i = 0; i < len && c->text[i] == s[i]; i++ ) ;
	if ( i == len && c->text[len] == 0 ) {
		cx_free(c->shown);
		c->shown = NULL;
		return TRUE;
	}
	d = dup_bytes(s, len);
	if ( d == NULL ) return FALSE;
	cx_free(c->shown);
	c->shown = d;
	return TRUE;
}

EXPORT CONST UB *cx_shown( CONST CXCELL *c )
{
	return ( c->shown != NULL ) ? c->shown : c->text;
}

LOCAL INT size_of( CONST CXSIZE *a, INT n, INT at, INT dflt )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i].at == at ) return a[i].size;
	}
	return dflt;
}

LOCAL void size_set( CXSIZE **a, INT *p_n, INT *p_max, INT at, INT size )
{
	INT	i;

	for ( i = 0; i < *p_n; i++ ) {
		if ( ( *a )[i].at == at ) {
			( *a )[i].size = size;
			return;
		}
	}
	if ( !room((void **)a, *p_n, p_max, sizeof(CXSIZE)) ) return;
	( *a )[*p_n].at = at;
	( *a )[*p_n].size = size;
	( *p_n )++;
}

EXPORT INT cx_width( CONST CXSHEET *s, INT col )
{
	return size_of(s->w, s->nw, col, CX_CELL_W);
}

EXPORT INT cx_height( CONST CXSHEET *s, INT row )
{
	return size_of(s->h, s->nh, row, CX_CELL_H);
}

EXPORT void cx_set_width( CXSHEET *s, INT col, INT w )
{
	size_set(&s->w, &s->nw, &s->mw, col, w);
}

EXPORT void cx_set_height( CXSHEET *s, INT row, INT h )
{
	size_set(&s->h, &s->nh, &s->mh, row, h);
}

EXPORT void cx_grow( CXSHEET *s, INT col, INT row )
{
	if ( col > s->maxcol ) s->maxcol = ( col > CX_COLS_MAX ) ? CX_COLS_MAX : col;
	if ( row > s->maxrow ) s->maxrow = ( row > CX_ROWS_MAX ) ? CX_ROWS_MAX : row;
}

/* The cells and sizes after 'at' moved by d along rows (or columns); d < 0 takes 'at' out */
LOCAL void shift( CXSHEET *s, BOOL cols, INT at, INT d )
{
	INT	i, k = 0;
	CXSIZE	*a = cols ? s->w : s->h;
	INT	*pn = cols ? &s->nw : &s->nh;

	for ( i = 0; i < s->n; i++ ) {
		CXCELL	*c = s->c[i];
		INT	*v = cols ? &c->col : &c->row;

		if ( d < 0 && *v == at ) {
			cell_free(c);
			continue;
		}
		if ( *v > at || ( d > 0 && *v == at ) ) *v += d;
		s->c[k++] = c;
	}
	s->n = k;
	for ( i = 0, k = 0; i < *pn; i++ ) {
		if ( d < 0 && a[i].at == at ) continue;
		if ( a[i].at > at || ( d > 0 && a[i].at == at ) ) a[i].at += d;
		a[k++] = a[i];
	}
	*pn = k;
	cx_order(s);
}

EXPORT void cx_insert_row( CXSHEET *s, INT at )	{ shift(s, FALSE, at, 1); }
EXPORT void cx_insert_col( CXSHEET *s, INT at )	{ shift(s, TRUE, at, 1); }
EXPORT void cx_delete_row( CXSHEET *s, INT at )	{ shift(s, FALSE, at, -1); }
EXPORT void cx_delete_col( CXSHEET *s, INT at )	{ shift(s, TRUE, at, -1); }

EXPORT INT cx_merge_of( CONST CXSHEET *s, INT col, INT row )
{
	INT	i;

	for ( i = 0; i < s->nm; i++ ) {
		CONST CXRANGE	*r = &s->m[i];

		if ( col >= r->c0 && col <= r->c1 && row >= r->r0 && row <= r->r1 ) return i;
	}
	return -1;
}

EXPORT BOOL cx_merge_add( CXSHEET *s, CONST CXRANGE *r )
{
	INT	i;

	/* one join a corner: a second there takes the first's place */
	for ( i = 0; i < s->nm; i++ ) {
		if ( s->m[i].c0 == r->c0 && s->m[i].r0 == r->r0 ) {
			s->m[i] = *r;
			return TRUE;
		}
	}
	if ( !room((void **)&s->m, s->nm, &s->mm, sizeof(CXRANGE)) ) return FALSE;
	s->m[s->nm++] = *r;
	return TRUE;
}

EXPORT void cx_merge_del( CXSHEET *s, INT i )
{
	if ( i < 0 || i >= s->nm ) return;
	m_copy(&s->m[i], &s->m[i + 1], ( s->nm - i - 1 ) * (INT)sizeof(CXRANGE));
	s->nm--;
}

EXPORT BOOL cx_chart_add( CXSHEET *s, CONST CXCHART *c )
{
	if ( !room((void **)&s->ch, s->nch, &s->mch, sizeof(CXCHART)) ) return FALSE;
	s->ch[s->nch++] = *c;
	return TRUE;
}

EXPORT void cx_chart_del( CXSHEET *s, INT i )
{
	if ( i < 0 || i >= s->nch ) return;
	cx_free(s->ch[i].el);
	m_copy(&s->ch[i], &s->ch[i + 1], ( s->nch - i - 1 ) * (INT)sizeof(CXCHART));
	s->nch--;
}

/* ---------------------------------------------------------------- reading */

/* name="value" with a value of at least one letter, as `name="([^"]+)"` reads it */
LOCAL INT attr_some( CONST UB *a, INT n, CONST char *name, INT *p_v )
{
	INT	i, k, from = 0;

	for ( ;; ) {
		for ( i = from; i < n; i++ ) {
			if ( a[i] == (UB)name[0] && at_is(a + i, n - i, name) ) break;
		}
		if ( i >= n ) return -1;
		k = i + s_len((CONST UB *)name);
		if ( k + 2 < n && a[k] == '=' && a[k + 1] == '"' && a[k + 2] != '"' ) {
			INT	e = k + 2;

			while ( e < n && a[e] != '"' ) e++;
			if ( e < n ) {
				*p_v = k + 2;
				return e - ( k + 2 );
			}
		}
		from = i + 1;
	}
}

LOCAL INT line_type( CONST UB *s, INT n )
{
	if ( n == 4 && at_is(s, n, "dash") ) return CX_DASHED;
	if ( n == 3 && at_is(s, n, "dot") ) return CX_DOTTED;
	return CX_SOLID;
}

/* <calcCell .../>: the ground and the sides of the frame */
LOCAL void calc_cell( CONST UB *a, INT n, CXSTYLE *st )
{
	static CONST char *CONST side[4] = { "Top", "Bottom", "Left", "Right" };
	char	name[32];
	INT	v, k, i;

	k = attr_some(a, n, "bgCol", &v);
	if ( k > 0 ) {
		s_copy(st->bg, a + v, k, CX_COLOUR);
		st->has |= CX_S_BG;
	}
	for ( i = 0; i < 4; i++ ) {
		CXSIDE	*sd = &st->side[i];
		INT	l;

		l = 0;
		name[l++] = 'b'; name[l++] = 'o'; name[l++] = 'r'; name[l++] = 'd'; name[l++] = 'e'; name[l++] = 'r';
		for ( k = 0; side[i][k] != 0; k++ ) name[l++] = side[i][k];
		name[l] = '=';
		name[l + 1] = '"';
		name[l + 2] = '1';
		name[l + 3] = '"';
		name[l + 4] = 0;
		if ( find(a, n, 0, name) < 0 ) continue;
		name[l] = 0;
		sd->on = TRUE;
		sd->type = CX_SOLID;
		sd->width = 1;
		s_copy(sd->colour, (CONST UB *)DEF_COLOUR, 7, CX_COLOUR);
		{
			char	t[40];
			INT	j;

			for ( j = 0; j < l; j++ ) t[j] = name[j];
			t[l] = 'T'; t[l + 1] = 'y'; t[l + 2] = 'p'; t[l + 3] = 'e'; t[l + 4] = 0;
			k = attr_some(a, n, t, &v);
			if ( k > 0 ) sd->type = line_type(a + v, k);
			t[l] = 'W'; t[l + 1] = 'i'; t[l + 2] = 'd'; t[l + 3] = 't'; t[l + 4] = 'h'; t[l + 5] = 0;
			k = attr_some(a, n, t, &v);
			if ( k > 0 ) sd->width = int_of(a + v, k, 1);
			t[l] = 'C'; t[l + 1] = 'o'; t[l + 2] = 'l'; t[l + 3] = 'o'; t[l + 4] = 'r'; t[l + 5] = 0;
			k = attr_some(a, n, t, &v);
			if ( k > 0 ) s_copy(sd->colour, a + v, k, CX_COLOUR);
		}
		st->has |= CX_S_FRAME;
	}
}

/*
 * The first element at s[from..n) beginning with open and ending at the
 * first '>' that ends `pat` -- given as the part after the name: its
 * start and length, -1 when there is none.
 */

/* `<font NAME="([^"]+)"/>`: the first's value taken, and every one taken out of the words */
LOCAL BOOL font_tag( UB *w, INT *p_n, CONST char *name, char *out, INT max )
{
	char	head[16];
	INT	i, k, l = 0, n = *p_n;
	BOOL	got = FALSE;

	for ( k = 0; "<font "[k] != 0; k++ ) head[l++] = "<font "[k];
	for ( k = 0; name[k] != 0; k++ ) head[l++] = name[k];
	head[l++] = '=';
	head[l++] = '"';
	head[l] = 0;
	for ( i = 0; i < n; ) {
		INT	e;

		if ( !at_is(w + i, n - i, head) ) {
			i++;
			continue;
		}
		e = i + l;
		while ( e < n && w[e] != '"' ) e++;
		if ( e == i + l || e + 2 >= n || w[e + 1] != '/' || w[e + 2] != '>' ) {
			i++;
			continue;
		}
		if ( !got ) {
			s_copy(out, w + i + l, e - ( i + l ), max);
			got = TRUE;
		}
		m_copy(w + i, w + e + 3, n - ( e + 3 ));
		n -= e + 3 - i;
	}
	*p_n = n;
	return got;
}

/* Whether the words hold <tag>; every <tag> and </tag> taken out when they do */
LOCAL BOOL pair_tag( UB *w, INT *p_n, CONST char *tag )
{
	char	o[24], c[24];
	INT	i, k, n = *p_n, lo, lc;

	o[0] = '<';
	c[0] = '<';
	c[1] = '/';
	for ( k = 0; tag[k] != 0; k++ ) {
		o[k + 1] = tag[k];
		c[k + 2] = tag[k];
	}
	o[k + 1] = '>';
	o[k + 2] = 0;
	c[k + 2] = '>';
	c[k + 3] = 0;
	lo = k + 2;
	lc = k + 3;
	if ( find(w, n, 0, o) < 0 ) return FALSE;
	for ( i = 0; i < n; ) {
		if ( at_is(w + i, n - i, o) ) {
			m_copy(w + i, w + i + lo, n - i - lo);
			n -= lo;
		} else if ( at_is(w + i, n - i, c) ) {
			m_copy(w + i, w + i + lc, n - i - lc);
			n -= lc;
		} else {
			i++;
		}
	}
	*p_n = n;
	return TRUE;
}

/* Every `from` in the words made `to` */
LOCAL void subst( UB *w, INT *p_n, CONST char *from, CONST char *to )
{
	INT	i, n = *p_n, lf = s_len((CONST UB *)from), lt = s_len((CONST UB *)to);

	for ( i = 0; i + lf <= n; ) {
		if ( at_is(w + i, n - i, from) ) {
			m_copy(w + i + lt, w + i + lf, n - i - lf);
			m_copy(w + i, to, lt);
			n += lt - lf;
			i += lt;
		} else {
			i++;
		}
	}
	*p_n = n;
}

/* A code point at s, its length in *p_len; -1 for bytes that are not one */
LOCAL INT utf8_at( CONST UB *s, INT n, INT *p_len )
{
	UB	c = s[0];

	if ( c < 0x80 ) {
		*p_len = 1;
		return c;
	}
	if ( ( c & 0xE0 ) == 0xC0 && n >= 2 ) {
		*p_len = 2;
		return ( ( c & 0x1F ) << 6 ) | ( s[1] & 0x3F );
	}
	if ( ( c & 0xF0 ) == 0xE0 && n >= 3 ) {
		*p_len = 3;
		return ( ( c & 0x0F ) << 12 ) | ( ( s[1] & 0x3F ) << 6 ) | ( s[2] & 0x3F );
	}
	if ( ( c & 0xF8 ) == 0xF0 && n >= 4 ) {
		*p_len = 4;
		return ( ( c & 0x07 ) << 18 ) | ( ( s[1] & 0x3F ) << 12 ) | ( ( s[2] & 0x3F ) << 6 ) | ( s[3] & 0x3F );
	}
	*p_len = 1;
	return -1;
}

/* What the trimming of words takes off their ends */
LOCAL BOOL white( INT u )
{
	return (BOOL)( u == 0x09 || u == 0x0A || u == 0x0B || u == 0x0C || u == 0x0D || u == 0x20
		    || u == 0xA0 || u == 0x1680 || ( u >= 0x2000 && u <= 0x200A ) || u == 0x2028
		    || u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF );
}

/* A double quote of either width and either side */
LOCAL BOOL quote( INT u )
{
	return (BOOL)( u == 0x22 || u == 0x201C || u == 0x201D || u == 0x301D || u == 0x301E || u == 0xFF02 );
}

/* The last code point of s[0..n): its start */
LOCAL INT last_at( CONST UB *s, INT n )
{
	INT	i = n - 1;

	while ( i > 0 && ( s[i] & 0xC0 ) == 0x80 ) i--;
	return i;
}

/*
 * A cell's words read: the letters' look taken from the font elements
 * and decorations round them, the entities made letters again, the
 * spaces at the ends and a double quote at each end taken off. w is
 * changed where it is; its length after.
 */
LOCAL INT cell_words( UB *w, INT n, CXSTYLE *st )
{
	char	t[CX_FACE];
	INT	i, a, b, len;

	for ( i = 0, a = 0; i < n; i++ ) {
		if ( w[i] != '\t' && w[i] != '\n' && w[i] != '\r' ) w[a++] = w[i];
	}
	n = a;
	for ( i = 0; i < n && w[i] != '<' && w[i] != '&'; i++ ) ;
	if ( i == n ) goto ends;		/* words alone, the most of a large sheet */
	if ( font_tag(w, &n, "color", st->colour, CX_COLOUR) ) st->has |= CX_S_COLOUR;
	if ( font_tag(w, &n, "size", t, sizeof(t)) ) {
		st->size = int_of((CONST UB *)t, s_len((CONST UB *)t), 0);
		st->has |= CX_S_SIZE;
	}
	if ( font_tag(w, &n, "face", st->face, CX_FACE) ) st->has |= CX_S_FACE;
	if ( pair_tag(w, &n, "bold") ) st->has |= CX_S_BOLD;
	if ( pair_tag(w, &n, "italic") ) st->has |= CX_S_ITALIC;
	if ( pair_tag(w, &n, "underline") ) st->has |= CX_S_UNDER;
	if ( pair_tag(w, &n, "strikethrough") ) st->has |= CX_S_STRIKE;
	if ( pair_tag(w, &n, "superscript") ) {
		st->has |= CX_S_SUPER;
		st->has &= ~CX_S_SUB;
	}
	if ( pair_tag(w, &n, "subscript") ) {
		st->has |= CX_S_SUB;
		st->has &= ~CX_S_SUPER;
	}
	if ( pair_tag(w, &n, "mesh") ) st->has |= CX_S_MESH;
	if ( pair_tag(w, &n, "invert") ) st->has |= CX_S_INVERT;
	subst(w, &n, "&lt;", "<");
	subst(w, &n, "&gt;", ">");
	subst(w, &n, "&quot;", "\"");
	subst(w, &n, "&apos;", "'");
	subst(w, &n, "&amp;", "&");

	/* the white space at the ends */
    ends:
	a = 0;
	while ( a < n ) {
		INT	u = utf8_at(w + a, n - a, &len);

		if ( !white(u) ) break;
		a += len;
	}
	b = n;
	while ( b > a ) {
		INT	k = last_at(w + a, b - a) + a, u = utf8_at(w + k, b - k, &len);

		if ( !white(u) ) break;
		b = k;
	}
	if ( a < b && quote(utf8_at(w + a, b - a, &len)) ) a += len;
	if ( a < b ) {
		INT	k = last_at(w + a, b - a) + a;

		if ( quote(utf8_at(w + k, b - k, &len)) ) b = k;
	}
	m_copy(w, w + a, b - a);
	w[b - a] = 0;
	return b - a;
}

/* A cell read from its words: what is new of its look goes over what it had */
LOCAL ER put_cell( CXSHEET *s, INT col, INT row, CONST UB *text, INT tn, CONST CXSTYLE *st )
{
	CXCELL	*c;
	UB	*conv;
	INT	cn;

	if ( col < 1 || row < 1 ) return E_OK;
	c = cx_make(s, col, row);
	conv = (UB *)cx_alloc(tn * 2 + 8);
	if ( c == NULL || conv == NULL ) {
		cx_free(conv);
		return E_NOMEM;
	}
	cn = cx_old_form(text, tn, conv, tn * 2 + 8);
	conv[cn] = 0;
	/* the words given to the cell as they are, without a copy between */
	cx_free(c->text);
	c->text = conv;
	cx_free(c->shown);
	c->shown = NULL;
	cx_free(c->el);
	c->el = NULL;
	c->ellen = 0;
	c->kind = CX_TEXT;
	if ( st->has != 0 && !cx_style_add(c, st) ) return E_NOMEM;
	return E_OK;
}

/* `<tag\s+align="([^"]*)"\s*\/>`, its value and where it was in w; taken out of w */
LOCAL BOOL one_attr_tag( UB *w, INT *p_n, CONST char *tag, CONST char *attr, char *out, INT max )
{
	INT	i, n = *p_n, lt = s_len((CONST UB *)tag), la = s_len((CONST UB *)attr);

	for ( i = 0; i < n; i++ ) {
		INT	k, v, e;

		if ( w[i] != '<' || !at_is_i(w + i + 1, n - i - 1, tag) ) continue;
		k = i + 1 + lt;
		if ( k >= n || !is_space(w[k]) ) continue;
		while ( k < n && is_space(w[k]) ) k++;
		if ( !at_is_i(w + k, n - k, attr) || k + la + 1 >= n || w[k + la] != '=' || w[k + la + 1] != '"' ) continue;
		v = k + la + 2;
		e = v;
		while ( e < n && w[e] != '"' ) e++;
		if ( e >= n ) continue;
		k = e + 1;
		while ( k < n && is_space(w[k]) ) k++;
		if ( k + 1 >= n || w[k] != '/' || w[k + 1] != '>' ) continue;
		s_copy(out, w + v, e - v, max);
		k += 2;
		m_copy(w + i, w + k, n - k);
		*p_n = n - ( k - i );
		return TRUE;
	}
	return FALSE;
}

/* `<link\s+([^>]*?)(?:\/>|>(.*?)<\/link>)` in w: where, and its length */
LOCAL INT link_in( CONST UB *w, INT n, INT *p_at )
{
	INT	i, k;

	for ( i = 0; i + 5 < n; i++ ) {
		if ( !at_is(w + i, n - i, "<link") || !is_space(w[i + 5]) ) continue;
		for ( k = i + 6; k < n && w[k] != '>'; k++ ) {
			if ( w[k] == '/' && k + 1 < n && w[k + 1] == '>' ) {
				*p_at = i;
				return k + 2 - i;
			}
		}
		if ( k < n ) {
			INT	e = find(w, n, k + 1, "</link>");

			if ( e >= 0 ) {
				*p_at = i;
				return e + 7 - i;
			}
		}
	}
	return -1;
}

/* `<image\s+([^>]*)(?:\/>|>)` */
LOCAL INT image_in( CONST UB *w, INT n, INT *p_at )
{
	INT	i, k;

	for ( i = 0; i + 6 < n; i++ ) {
		if ( !at_is(w + i, n - i, "<image") || !is_space(w[i + 6]) ) continue;
		for ( k = i + 7; k < n && w[k] != '>'; k++ ) ;
		if ( k < n ) {
			*p_at = i;
			return k + 1 - i;
		}
	}
	return -1;
}

/*
 * What follows a <calcPos/> up to the next tab, <calcPos, </p>, <p> or
 * the end, on the same line: its length, or -1 when a line ends first.
 */
LOCAL INT cell_span( CONST UB *x, INT n, INT at )
{
	INT	k;

	for ( k = at; ; k++ ) {
		if ( k >= n ) return k - at;
		if ( x[k] == '\t' || at_is(x + k, n - k, "<calcPos") || at_is(x + k, n - k, "</p>")
		  || at_is(x + k, n - k, "<p>") ) {
			return k - at;
		}
		if ( x[k] == '\n' || x[k] == '\r' ) return -1;
	}
}

/* One cell: its look and what it holds, from the bytes after its <calcPos/> */
LOCAL ER read_cell( CXSHEET *s, INT col, INT row, CONST UB *src, INT len, INT *p_count )
{
	CXSTYLE	st;
	UB	small[256];
	UB	*w = ( len < (INT)sizeof(small) ) ? small : (UB *)cx_alloc(len + 1);
	INT	n = len, at, k;
	ER	er = E_OK;

	if ( w == NULL ) return E_NOMEM;
	m_copy(w, src, len);
	w[len] = 0;
	m_zero(&st, sizeof(st));
	for ( k = 0; k < n && w[k] != '<'; k++ ) ;
	if ( k == n ) {
		/* no element: words alone */
		n = cell_words(w, n, &st);
		if ( n > 0 ) {
			er = put_cell(s, col, row, w, n, &st);
			( *p_count )++;
		}
		if ( w != small ) cx_free(w);
		return er;
	}

	/* <calcCell .../> */
	at = find(w, n, 0, "<calcCell ");
	while ( at >= 0 ) {
		for ( k = at + 10; k < n && w[k] != '>'; k++ ) ;
		if ( k < n && k - 1 > at + 10 && w[k - 1] == '/' ) {
			calc_cell(w + at + 10, k - 1 - ( at + 10 ), &st);
			m_copy(w + at, w + k + 1, n - k - 1);
			n -= k + 1 - at;
			break;
		}
		at = find(w, n, at + 1, "<calcCell ");
	}
	if ( one_attr_tag(w, &n, "text", "align", st.align, sizeof(st.align)) ) st.has |= CX_S_ALIGN;
	if ( one_attr_tag(w, &n, "cell", "valign", st.valign, sizeof(st.valign)) ) st.has |= CX_S_VALIGN;

	k = link_in(w, n, &at);
	if ( k > 0 ) {
		CXCELL	*c = ( col >= 1 && row >= 1 ) ? cx_make(s, col, row) : NULL;

		if ( c != NULL ) {
			if ( !cx_set_el(c, CX_LINK, w + at, k ) ) er = E_NOMEM;
			c->st = &cx_plain;
			if ( !cx_style_add(c, &st) ) er = E_NOMEM;
			( *p_count )++;
		}
	} else if ( ( k = image_in(w, n, &at) ) > 0 ) {
		CXCELL	*c = ( col >= 1 && row >= 1 ) ? cx_make(s, col, row) : NULL;

		if ( c != NULL ) {
			if ( !cx_set_el(c, CX_IMAGE, w + at, k) ) er = E_NOMEM;
			c->st = &cx_plain;
			if ( !cx_style_add(c, &st) ) er = E_NOMEM;
			( *p_count )++;
		}
	} else {
		n = cell_words(w, n, &st);
		if ( n > 0 || st.has != 0 ) {
			er = put_cell(s, col, row, w, n, &st);
			( *p_count )++;
		}
	}
	if ( w != small ) cx_free(w);
	return er;
}

/* The cells named by "<calcPos col=... row=...>", read when none are named the newer way */
LOCAL ER read_old( CXSHEET *s, CONST UB *x, INT n )
{
	INT	i = 0;
	CXSTYLE	none;

	m_zero(&none, sizeof(none));
	while ( ( i = find(x, n, i, "<calcPos col=\"") ) >= 0 ) {
		INT	k = i + 14, a, b;
		D	r, c;

		i++;
		if ( get_num(x, n, &k, &r) == 0 || !at_is(x + k, n - k, "\" row=\"") ) continue;
		k += 7;
		if ( get_num(x, n, &k, &c) == 0 || k >= n || x[k] != '"' ) continue;
		k++;
		if ( k < n && x[k] == '/' ) k++;
		if ( k >= n || x[k] != '>' ) continue;
		k++;
		for ( a = k; k < n && x[k] != '<' && x[k] != '\t'; k++ ) ;
		b = k;
		/* the value trimmed: white space and the ASCII line ends */
		while ( a < b && is_space(x[a]) ) a++;
		while ( b > a && is_space(x[b - 1]) ) b--;
		if ( b > a ) {
			/* "col" is the row and "row" the column, as they were written */
			ER	er = put_cell(s, (INT)c, (INT)r, x + a, b - a, &none);

			if ( er < E_OK ) return er;
		}
		i = k;
	}
	return E_OK;
}

/* <calcSize cell="X" width|height="N"/> */
LOCAL void read_sizes( CXSHEET *s, CONST UB *x, INT n )
{
	INT	i = 0;

	while ( ( i = find(x, n, i, "<calcSize cell=\"") ) >= 0 ) {
		INT	v = i + 16, e = v, k;
		BOOL	wide;
		D	size;

		i++;
		while ( e < n && x[e] != '"' ) e++;
		if ( e == v || e >= n || !at_is(x + e, n - e, "\" ") ) continue;
		k = e + 2;
		if ( at_is(x + k, n - k, "width=\"") ) {
			wide = TRUE;
			k += 7;
		} else if ( at_is(x + k, n - k, "height=\"") ) {
			wide = FALSE;
			k += 8;
		} else {
			continue;
		}
		if ( get_num(x, n, &k, &size) == 0 || k >= n || x[k] != '"' ) continue;
		k++;
		if ( k < n && x[k] == '/' ) k++;
		while ( k < n && is_space(x[k]) ) k++;
		if ( k >= n || x[k] != '>' ) continue;
		if ( wide ) {
			INT	col = cx_col_num(x + v, e - v);

			if ( col > 0 ) cx_set_width(s, col, (INT)size);
		} else {
			INT	j = v;
			D	row;

			if ( get_num(x, e, &j, &row) > 0 ) cx_set_height(s, (INT)row, (INT)size);
		}
		i = k;
	}
}

/* <calcMerge range="A1:B2"/> */
LOCAL void read_merges( CXSHEET *s, CONST UB *x, INT n )
{
	INT	i = 0;

	while ( ( i = find(x, n, i, "<calcMerge range=\"") ) >= 0 ) {
		INT	k = i + 18;
		CXRANGE	r;

		i++;
		if ( !cell_at(x, n, &k, &r.c0, &r.r0) || k >= n || x[k] != ':' ) continue;
		k++;
		if ( !cell_at(x, n, &k, &r.c1, &r.r1) || k >= n || x[k] != '"' ) continue;
		k++;
		if ( k < n && x[k] == '/' ) k++;
		while ( k < n && is_space(x[k]) ) k++;
		if ( k >= n || x[k] != '>' ) continue;
		if ( r.c0 > 0 && r.c1 > 0 && r.r0 > 0 && r.r1 > 0 ) (void)cx_merge_add(s, &r);
	}
}

LOCAL CONST char *CONST chart_name[CX_CH_KINDS] = { "clustered-bar", "stacked-bar", "line", "stacked-line" };

/* <group ...><calc chartType="..." dataRange="...">...</calc></group> */
LOCAL ER read_charts( CXSHEET *s, CONST UB *x, INT n )
{
	INT	i = 0;

	while ( ( i = find(x, n, i, "<group") ) >= 0 ) {
		INT	a = i + 6, ae, ce, c, v, k;
		CXCHART	ch;

		i++;
		if ( a >= n || !is_space(x[a]) ) continue;
		for ( ae = a; ae < n && x[ae] != '>'; ae++ ) ;
		if ( ae >= n ) continue;
		ce = find(x, n, ae + 1, "</group>");
		if ( ce < 0 ) continue;
		i = ce + 8;
		/* the <calc> in it, its kind and its cells */
		for ( c = ae + 1; c < ce; c++ ) {
			if ( x[c] == '<' && at_is_i(x + c + 1, ce - c - 1, "calc") && c + 5 < ce && is_space(x[c + 5]) ) {
				INT	j = c + 5;

				while ( j < ce && is_space(x[j]) ) j++;
				if ( at_is_i(x + j, ce - j, "chartType=\"") ) break;
			}
		}
		if ( c >= ce ) continue;
		m_zero(&ch, sizeof(ch));
		{
			INT	tend;

			for ( tend = c; tend < ce && x[tend] != '>'; tend++ ) ;
			k = attr_some(x + c, tend - c, "chartType", &v);
			if ( k <= 0 ) continue;
			s_copy(ch.name, x + c + v, k, sizeof(ch.name));
			k = attr_some(x + c, tend - c, "dataRange", &v);
			if ( k > 0 ) ch.has_range = cx_range_parse(x + c + v, k, &ch.range);
		}
		ch.kind = -1;
		for ( k = 0; k < CX_CH_KINDS; k++ ) {
			if ( s_eq(ch.name, chart_name[k]) ) ch.kind = k;
		}
		if ( ( k = attr_some(x + a, ae - a, "left", &v) ) <= 0 ) continue;
		ch.left = num_of(x + a + v, k, 0);
		if ( ( k = attr_some(x + a, ae - a, "top", &v) ) <= 0 ) continue;
		ch.top = num_of(x + a + v, k, 0);
		if ( ( k = attr_some(x + a, ae - a, "right", &v) ) <= 0 ) continue;
		ch.right = num_of(x + a + v, k, 0);
		if ( ( k = attr_some(x + a, ae - a, "bottom", &v) ) <= 0 ) continue;
		ch.bottom = num_of(x + a + v, k, 0);
		ch.el = dup_bytes(x + a - 6, ce + 8 - ( a - 6 ));
		ch.ellen = ce + 8 - ( a - 6 );
		if ( ch.el == NULL || !cx_chart_add(s, &ch) ) {
			cx_free(ch.el);
			return E_NOMEM;
		}
	}
	return E_OK;
}

/* `<name\s+[^>]+>` as it is */
LOCAL UB *element( CONST UB *x, INT n, CONST char *name )
{
	INT	i = 0, l = s_len((CONST UB *)name);

	while ( ( i = find(x, n, i, name) ) >= 0 ) {
		INT	k = i + l;

		i++;
		if ( k >= n || !is_space(x[k]) ) continue;
		while ( k < n && x[k] != '>' ) k++;
		if ( k >= n || k == i + l ) continue;
		return dup_bytes(x + i - 1, k + 1 - ( i - 1 ));
	}
	return NULL;
}

EXPORT ER cx_read( CXSHEET *s, CONST UB *x, INT n )
{
	INT	i = 0, count = 0, k;
	ER	er;

	s->paper = element(x, n, "<paper");
	s->margin = element(x, n, "<docmargin");
	read_sizes(s, x, n);

	while ( ( i = find(x, n, i, "<calcPos cell=\"") ) >= 0 ) {
		INT	at = i + 15, col, row, len;

		i++;
		if ( !cell_at(x, n, &at, &col, &row) || at >= n || x[at] != '"' ) continue;
		at++;
		if ( at < n && x[at] == '/' ) at++;
		if ( at >= n || x[at] != '>' ) continue;
		at++;
		len = cell_span(x, n, at);
		if ( len < 0 ) continue;
		er = read_cell(s, col, row, x + at, len, &count);
		if ( er < E_OK ) return er;
		i = at + len;
	}
	if ( count == 0 ) {
		er = read_old(s, x, n);
		if ( er < E_OK ) return er;
	}
	read_merges(s, x, n);
	er = read_charts(s, x, n);
	if ( er < E_OK ) return er;

	/* the grid reaches every cell and join */
	for ( k = 0; k < s->n; k++ ) cx_grow(s, s->c[k]->col, s->c[k]->row);
	for ( k = 0; k < s->nm; k++ ) cx_grow(s, s->m[k].c1, s->m[k].r1);
	return E_OK;
}

EXPORT void cx_order( CXSHEET *s )
{
	INT	i, k;

	/* insertion: the cells are nearly in order already */
	for ( i = 1; i < s->n; i++ ) {
		CXCELL	*c = s->c[i];

		for ( k = i; k > 0 && ( s->c[k - 1]->row > c->row
				   || ( s->c[k - 1]->row == c->row && s->c[k - 1]->col > c->col ) ); k-- ) {
			s->c[k] = s->c[k - 1];
		}
		s->c[k] = c;
	}
}

/* ---------------------------------------------------------------- writing */

EXPORT void cx_esc( CTBUF *b, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && s[i] != 0; i++ ) {
		if ( s[i] == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && s[i + 2] == 0x8B ) {
			i += 2;			/* the zero width space is not kept */
			continue;
		}
		switch ( s[i] ) {
		case '&':	ct_put(b, "&amp;");	break;
		case '<':	ct_put(b, "&lt;");	break;
		case '>':	ct_put(b, "&gt;");	break;
		case '"':	ct_put(b, "&quot;");	break;
		case '\'':	ct_put(b, "&apos;");	break;
		default:	ct_putn(b, &s[i], 1);	break;
		}
	}
}

LOCAL void put_side( CTBUF *b, CONST char *name, CONST CXSIDE *sd, BOOL first )
{
	static CONST char *CONST type[4] = { "line", "dash", "dot", "double" };

	if ( !first ) ct_put(b, " ");
	ct_put(b, "border");
	ct_put(b, name);
	ct_put(b, "=\"1\" border");
	ct_put(b, name);
	ct_put(b, "Type=\"");
	ct_put(b, type[( sd->type >= 0 && sd->type < 4 ) ? sd->type : 0]);
	ct_put(b, "\" border");
	ct_put(b, name);
	ct_put(b, "Width=\"");
	ct_num(b, sd->width);
	ct_put(b, "\" border");
	ct_put(b, name);
	ct_put(b, "Color=\"");
	ct_put(b, sd->colour);
	ct_put(b, "\"");
}

LOCAL void put_calc_cell( CTBUF *b, CONST CXSTYLE *st )
{
	static CONST char *CONST side[4] = { "Top", "Bottom", "Left", "Right" };
	BOOL	first = TRUE;
	INT	i;
	BOOL	bg = (BOOL)( ( st->has & CX_S_BG ) && st->bg[0] != 0 && !s_eq(st->bg, DEF_GROUND) );

	for ( i = 0; i < 4 && !st->side[i].on; i++ ) ;
	if ( i == 4 && !bg ) return;
	ct_put(b, "<calcCell ");
	if ( bg ) {
		ct_put(b, "bgCol=\"");
		ct_put(b, st->bg);
		ct_put(b, "\"");
		first = FALSE;
	}
	for ( i = 0; i < 4; i++ ) {
		if ( !st->side[i].on ) continue;
		put_side(b, side[i], &st->side[i], first);
		first = FALSE;
	}
	ct_put(b, "/>");
}

/* The words in their font elements and decorations */
LOCAL void put_words( CTBUF *b, CONST UB *text, CONST CXSTYLE *st )
{
	INT	n = s_len(text), d;
	static CONST char *CONST tag[6] = { "bold", "italic", "underline", "strikethrough", "superscript", "subscript" };
	static CONST UINT bit[6] = { CX_S_BOLD, CX_S_ITALIC, CX_S_UNDER, CX_S_STRIKE, CX_S_SUPER, CX_S_SUB };

	if ( n == 0 ) return;
	if ( ( st->has & CX_S_COLOUR ) && st->colour[0] != 0 && !s_eq(st->colour, DEF_COLOUR) ) {
		ct_put(b, "<font color=\"");
		ct_put(b, st->colour);
		ct_put(b, "\"/>");
	}
	if ( ( st->has & CX_S_SIZE ) && st->size != 0 && st->size != DEF_SIZE ) {
		ct_put(b, "<font size=\"");
		ct_num(b, st->size);
		ct_put(b, "\"/>");
	}
	if ( ( st->has & CX_S_FACE ) && st->face[0] != 0 && !s_eq(st->face, DEF_FACE) ) {
		ct_put(b, "<font face=\"");
		ct_put(b, st->face);
		ct_put(b, "\"/>");
	}
	/* the first decoration innermost */
	for ( d = 5; d >= 0; d-- ) {
		if ( st->has & bit[d] ) {
			ct_put(b, "<");
			ct_put(b, tag[d]);
			ct_put(b, ">");
		}
	}
	cx_esc(b, text, n);
	for ( d = 0; d < 6; d++ ) {
		if ( st->has & bit[d] ) {
			ct_put(b, "</");
			ct_put(b, tag[d]);
			ct_put(b, ">");
		}
	}
}

EXPORT void cx_write( CTBUF *b, CONST CXSHEET *s, CONST UB *name, CX_CHARTFN fn, void *arg )
{
	INT	i, row, col, maxrow = 1, maxcol = 1, at = 0;
	char	nm[16];

	ct_put(b, "<tad version=\"1.0\" filename=\"");
	ct_put(b, ( name != NULL && name[0] != 0 ) ? (CONST char *)name : "表計算");
	ct_put(b, "\" encoding=\"UTF-8\">\n<document>\n");

	/* the rows' heights, then the columns' widths, that are not the usual */
	for ( row = 1; row <= s->maxrow; row++ ) {
		INT	h = size_of(s->h, s->nh, row, CX_CELL_H);

		if ( h == CX_CELL_H ) continue;
		ct_put(b, "<calcSize cell=\"");
		ct_num(b, row);
		ct_put(b, "\" height=\"");
		ct_num(b, h);
		ct_put(b, "\"/>\n");
	}
	for ( col = 1; col <= s->maxcol; col++ ) {
		INT	w = size_of(s->w, s->nw, col, CX_CELL_W);

		if ( w == CX_CELL_W ) continue;
		ct_put(b, "<calcSize cell=\"");
		(void)cx_col_name(col, nm);
		ct_put(b, nm);
		ct_put(b, "\" width=\"");
		ct_num(b, w);
		ct_put(b, "\"/>\n");
	}
	for ( i = 0; i < s->nm; i++ ) {
		ct_put(b, "<calcMerge range=\"");
		(void)cx_cell_name(s->m[i].c0, s->m[i].r0, nm);
		ct_put(b, nm);
		ct_put(b, ":");
		(void)cx_cell_name(s->m[i].c1, s->m[i].r1, nm);
		ct_put(b, nm);
		ct_put(b, "\"/>\n");
	}
	ct_put(b, "<p>\n");
	if ( s->paper != NULL ) {
		ct_put(b, (CONST char *)s->paper);
		ct_put(b, "\n");
		if ( s->margin != NULL ) {
			ct_put(b, (CONST char *)s->margin);
			ct_put(b, "\n");
		} else {
			ct_put(b, "<docmargin top=\"0\" bottom=\"0\" left=\"0\" right=\"0\" />\n");
		}
	} else if ( s->margin != NULL ) {
		ct_put(b, (CONST char *)s->margin);
		ct_put(b, "\n");
	}

	for ( i = 0; i < s->n; i++ ) {
		if ( s->c[i]->row > maxrow ) maxrow = s->c[i]->row;
		if ( s->c[i]->col > maxcol ) maxcol = s->c[i]->col;
	}
	for ( row = 1; row <= maxrow; row++ ) {
		if ( row > 1 ) ct_put(b, "<p>");
		for ( col = 1; col <= maxcol; col++ ) {
			CONST CXCELL	*c = NULL;

			while ( at < s->n && ( s->c[at]->row < row || ( s->c[at]->row == row && s->c[at]->col < col ) ) ) at++;
			if ( at < s->n && s->c[at]->row == row && s->c[at]->col == col ) c = s->c[at];
			ct_put(b, "<calcPos cell=\"");
			(void)cx_cell_name(col, row, nm);
			ct_put(b, nm);
			ct_put(b, "\"/>");
			if ( c != NULL ) {
				CONST CXSTYLE	*st = c->st;

				put_calc_cell(b, st);
				if ( ( st->has & CX_S_ALIGN ) && st->align[0] != 0 && !s_eq(st->align, "left") ) {
					ct_put(b, "<text align=\"");
					ct_put(b, st->align);
					ct_put(b, "\"/>");
				}
				if ( ( st->has & CX_S_VALIGN ) && st->valign[0] != 0 && !s_eq(st->valign, "middle") ) {
					ct_put(b, "<cell valign=\"");
					ct_put(b, st->valign);
					ct_put(b, "\"/>");
				}
				if ( c->kind != CX_TEXT && c->el != NULL ) {
					ct_putn(b, c->el, c->ellen);
				} else {
					put_words(b, c->text, st);
				}
			}
			if ( col < maxcol ) ct_put(b, "\t");
		}
		ct_put(b, "</p>\n");
	}

	for ( i = 0; i < s->nch; i++ ) {
		if ( fn != NULL && s->ch[i].kind >= 0 ) {
			fn(b, s, i, arg);
		} else if ( s->ch[i].el != NULL ) {
			ct_putn(b, s->ch[i].el, s->ch[i].ellen);
			ct_put(b, "\n");
		}
	}
	ct_put(b, "</document>\n</tad>\n");
}

/* ---------------------------------------------------------------- formulas */

/* The full width marks a cell read from a record may hold, and what they are */
LOCAL CONST struct {
	UH	wide;
	UB	plain;
} full_half[] = {
	{ 0xFF02, '"' }, { 0x201C, '"' }, { 0x201D, '"' }, { 0xFF07, '\'' }, { 0x2018, '\'' },
	{ 0x2019, '\'' }, { 0xFF1D, '=' }, { 0xFF0B, '+' }, { 0xFF0D, '-' }, { 0x00D7, '*' },
	{ 0x00F7, '/' }, { 0xFF08, '(' }, { 0xFF09, ')' }, { 0xFF3B, '[' }, { 0xFF3D, ']' },
	{ 0xFF0C, ',' }, { 0xFF0A, '*' }, { 0xFF0F, '/' }, { 0xFF1C, '<' }, { 0xFF1E, '>' },
	{ 0xFF05, '%' }, { 0xFF3E, '^' }, { 0xFF06, '&' }, { 0xFF5C, '|' }, { 0xFF1A, ':' },
	{ 0xFF1B, ';' }, { 0xFF0E, '.' }, { 0x3000, ' ' }
};

LOCAL void out_byte( UB *out, INT max, INT *p_k, UB c )
{
	if ( *p_k < max - 1 ) out[( *p_k )++] = c;
}

LOCAL void out_str( UB *out, INT max, INT *p_k, CONST char *s )
{
	while ( *s != 0 ) out_byte(out, max, p_k, (UB)*s++);
}

EXPORT INT cx_old_form( CONST UB *in, INT n, UB *out, INT max )
{
	UB	*t = (UB *)cx_alloc(n + 1);
	INT	i, k = 0, a;

	if ( t == NULL ) {
		if ( max > 0 ) out[0] = 0;
		return 0;
	}
	/* the marks made plain */
	for ( i = 0; i < n; ) {
		INT	len, u = utf8_at(in + i, n - i, &len), j;

		for ( j = 0; j < (INT)( sizeof(full_half) / sizeof(full_half[0]) ); j++ ) {
			if ( u == full_half[j].wide ) break;
		}
		if ( j < (INT)( sizeof(full_half) / sizeof(full_half[0]) ) ) {
			t[k++] = full_half[j].plain;
		} else {
			m_copy(t + k, in + i, len);
			k += len;
		}
		i += len;
	}
	n = k;

	/* a result before the "=" */
	a = 0;
	if ( a < n && t[a] == '-' ) a++;
	i = a;
	while ( i < n && ( is_digit(t[i]) || t[i] == '.' ) ) i++;
	a = ( i > a && i < n && t[i] == '=' ) ? i : 0;

	/* [row,col] as A1 */
	k = 0;
	for ( i = a; i < n; ) {
		if ( t[i] == '[' ) {
			INT	j = i + 1;
			D	r, c;

			if ( get_num(t, n, &j, &r) > 0 && j < n && t[j] == ',' ) {
				j++;
				if ( get_num(t, n, &j, &c) > 0 && j < n && t[j] == ']' ) {
					char	nm[16];

					if ( c > 0 && c <= CX_COLS_MAX * 64 ) {
						(void)cx_col_name((INT)c, nm);
						out_str(out, max, &k, nm);
					}
					(void)cx_cell_name(1, (INT)r, nm);
					out_str(out, max, &k, nm + 1);
					i = j + 1;
					continue;
				}
			}
		}
		out_byte(out, max, &k, t[i++]);
	}
	if ( max > 0 ) out[k] = 0;
	cx_free(t);
	return k;
}

EXPORT INT cx_move_refs( CONST UB *in, INT n, INT cols, INT rows, INT maxcol, INT maxrow, UB *out, INT max )
{
	INT	i = 0, k = 0;

	if ( n == 0 || in[0] != '=' ) {
		for ( i = 0; i < n; i++ ) out_byte(out, max, &k, in[i]);
		if ( max > 0 ) out[k] = 0;
		return k;
	}
	while ( i < n ) {
		INT	j = i, a, b;
		BOOL	cabs = FALSE, rabs = FALSE;
		D	row;

		if ( in[j] == '$' ) {
			cabs = TRUE;
			j++;
		}
		a = j;
		while ( j < n && is_upper(in[j]) ) j++;
		b = j;
		if ( b > a ) {
			INT	jj = j;

			if ( jj < n && in[jj] == '$' ) {
				rabs = TRUE;
				jj++;
			}
			if ( get_num(in, n, &jj, &row) > 0 ) {
				D	col = cx_col_num(in + a, b - a);

				if ( !cabs ) col += cols;
				if ( !rabs ) row += rows;
				if ( col < 1 || col > maxcol || row < 1 || row > maxrow ) {
					out_str(out, max, &k, "#REF!");
				} else {
					char	nm[16];

					if ( cabs ) out_byte(out, max, &k, '$');
					(void)cx_col_name((INT)col, nm);
					out_str(out, max, &k, nm);
					if ( rabs ) out_byte(out, max, &k, '$');
					(void)cx_cell_name(1, (INT)row, nm);
					out_str(out, max, &k, nm + 1);
				}
				i = jj;
				continue;
			}
		}
		out_byte(out, max, &k, in[i++]);
	}
	if ( max > 0 ) out[k] = 0;
	return k;
}

/* ---------------------------------------------------------------- a cell's look */

LOCAL void str_set( char *d, CONST char *s, INT max )
{
	INT	i;

	for ( i = 0; s[i] != 0 && i < max - 1; i++ ) d[i] = s[i];
	d[i] = 0;
}

EXPORT void cx_style_merge( CXSTYLE *to, CONST CXSTYLE *from )
{
	INT	i;

	if ( from->has & CX_S_BG ) str_set(to->bg, from->bg, CX_COLOUR);
	if ( from->has & CX_S_FRAME ) {
		for ( i = 0; i < 4; i++ ) {
			if ( from->side[i].on ) to->side[i] = from->side[i];
		}
	}
	if ( from->has & CX_S_ALIGN ) str_set(to->align, from->align, sizeof(to->align));
	if ( from->has & CX_S_VALIGN ) str_set(to->valign, from->valign, sizeof(to->valign));
	if ( from->has & CX_S_COLOUR ) str_set(to->colour, from->colour, CX_COLOUR);
	if ( from->has & CX_S_SIZE ) to->size = from->size;
	if ( from->has & CX_S_FACE ) str_set(to->face, from->face, CX_FACE);
	if ( from->has & ( CX_S_SUPER | CX_S_SUB ) ) to->has &= ~( CX_S_SUPER | CX_S_SUB );
	to->has |= from->has;
}

/* ---------------------------------------------------------------- the looks shared */

EXPORT CONST CXSTYLE	cx_plain;

/*
 * The looks, kept once each in a table found by their bytes: a sheet of
 * a million cells has a few looks, not a million of 340 bytes. A look
 * is not let go while the program runs, so a cell may keep the one it
 * has however the others change.
 */
LOCAL CXSTYLE	**looks = NULL;
LOCAL INT	nlooks = 0, maxlooks = 0;	/* maxlooks a power of 2, at most half full */

/* The bytes after a string's end made 0, so that a look the same is the same bytes */
LOCAL void str_tidy( char *s, INT max )
{
	INT	i = 0;

	while ( i < max && s[i] != 0 ) i++;
	for ( ; i < max; i++ ) s[i] = 0;
}

LOCAL BOOL m_same( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*p = (CONST UB *)a, *q = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) if ( p[i] != q[i] ) return FALSE;
	return TRUE;
}

LOCAL UW look_hash( CONST CXSTYLE *st )
{
	CONST UB	*p = (CONST UB *)st;
	UW		h = 2166136261U;
	INT		i;

	for ( i = 0; i < (INT)sizeof(*st); i++ ) h = ( h ^ p[i] ) * 16777619U;
	return h;
}

LOCAL BOOL looks_grow( void )
{
	INT	m = ( maxlooks > 0 ) ? maxlooks * 2 : 64, i;
	CXSTYLE	**t = (CXSTYLE **)cx_alloc(m * (INT)sizeof(CXSTYLE *));

	if ( t == NULL ) return FALSE;
	m_zero(t, m * (INT)sizeof(CXSTYLE *));
	for ( i = 0; i < maxlooks; i++ ) {
		UW	h;

		if ( looks[i] == NULL ) continue;
		for ( h = look_hash(looks[i]) & (UW)( m - 1 ); t[h] != NULL; h = ( h + 1 ) & (UW)( m - 1 ) ) ;
		t[h] = looks[i];
	}
	cx_free(looks);
	looks = t;
	maxlooks = m;
	return TRUE;
}

EXPORT CONST CXSTYLE *cx_style( CONST CXSTYLE *st )
{
	CXSTYLE	k = *st, *d;
	UW	h;
	INT	i;

	str_tidy(k.bg, CX_COLOUR);
	str_tidy(k.align, sizeof(k.align));
	str_tidy(k.valign, sizeof(k.valign));
	str_tidy(k.colour, CX_COLOUR);
	str_tidy(k.face, CX_FACE);
	for ( i = 0; i < 4; i++ ) {
		if ( k.side[i].on ) str_tidy(k.side[i].colour, CX_COLOUR);
		else m_zero(&k.side[i], sizeof(k.side[i]));
	}
	if ( m_same(&k, &cx_plain, sizeof(k)) ) return &cx_plain;
	if ( ( nlooks + 1 ) * 2 > maxlooks && !looks_grow() ) return NULL;
	for ( h = look_hash(&k) & (UW)( maxlooks - 1 ); looks[h] != NULL; h = ( h + 1 ) & (UW)( maxlooks - 1 ) ) {
		if ( m_same(looks[h], &k, sizeof(k)) ) return looks[h];
	}
	d = (CXSTYLE *)cx_alloc((INT)sizeof(CXSTYLE));
	if ( d == NULL ) return NULL;
	*d = k;
	looks[h] = d;
	nlooks++;
	return d;
}

EXPORT BOOL cx_style_put( CXCELL *c, CONST CXSTYLE *st )
{
	CONST CXSTYLE	*d = cx_style(st);

	if ( d == NULL ) return FALSE;
	c->st = d;
	return TRUE;
}

EXPORT BOOL cx_style_add( CXCELL *c, CONST CXSTYLE *from )
{
	CXSTYLE	st = *c->st;

	cx_style_merge(&st, from);
	return cx_style_put(c, &st);
}

LOCAL void frame_cell( CXSHEET *s, INT col, INT row, UINT sides, CONST CXSIDE *sd )
{
	CXCELL	*c = cx_make(s, col, row);
	CXSTYLE	st;
	INT	i;

	if ( c == NULL ) return;
	st = *c->st;
	for ( i = 0; i < 4; i++ ) {
		if ( sides & ( 1U << i ) ) {
			st.side[i] = *sd;
			st.side[i].on = TRUE;
		}
	}
	st.has |= CX_S_FRAME;
	(void)cx_style_put(c, &st);
}

EXPORT void cx_frame_range( CXSHEET *s, CONST CXRANGE *r, INT how, CONST CXSIDE *side )
{
	INT	col, row, i;

	switch ( how ) {
	case CX_FR_ALL:
	case CX_FR_OUTLINE:
		for ( row = r->r0; row <= r->r1; row++ ) {
			for ( col = r->c0; col <= r->c1; col++ ) {
				UINT	sd = 0x0F;

				if ( how == CX_FR_OUTLINE ) {
					sd = 0;
					if ( row == r->r0 ) sd |= 1U << CX_TOP;
					if ( row == r->r1 ) sd |= 1U << CX_BOTTOM;
					if ( col == r->c0 ) sd |= 1U << CX_LEFT;
					if ( col == r->c1 ) sd |= 1U << CX_RIGHT;
				}
				if ( sd != 0 ) frame_cell(s, col, row, sd, side);
			}
		}
		break;
	case CX_FR_TOP:
		for ( col = r->c0; col <= r->c1; col++ ) frame_cell(s, col, r->r0, 1U << CX_TOP, side);
		break;
	case CX_FR_BOTTOM:
		for ( col = r->c0; col <= r->c1; col++ ) frame_cell(s, col, r->r1, 1U << CX_BOTTOM, side);
		break;
	case CX_FR_LEFT:
		for ( row = r->r0; row <= r->r1; row++ ) frame_cell(s, r->c0, row, 1U << CX_LEFT, side);
		break;
	case CX_FR_RIGHT:
		for ( row = r->r0; row <= r->r1; row++ ) frame_cell(s, r->c1, row, 1U << CX_RIGHT, side);
		break;
	case CX_FR_NONE:
		for ( row = r->r0; row <= r->r1; row++ ) {
			for ( col = r->c0; col <= r->c1; col++ ) {
				CXCELL	*c = cx_get(s, col, row);
				CXSTYLE	st;

				if ( c == NULL ) continue;
				st = *c->st;
				for ( i = 0; i < 4; i++ ) st.side[i].on = FALSE;
				st.has &= ~CX_S_FRAME;
				(void)cx_style_put(c, &st);
			}
		}
		break;
	default:
		break;
	}
}
