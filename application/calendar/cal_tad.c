/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cal_tad.c
 *	予定表: the records it reads and writes (design 11.10)
 *
 *	The month record is in the one form both systems keep it in, to the
 *	byte: the same elements in the same order, the same patterns (one
 *	of the figure's own for each colour, numbered from 128 in the order
 *	the colours are first wanted), the same zIndex counting. A month
 *	written here opens in TADjs Desktop as it was, and the other way
 *	round.
 */

#include "cal_tad.h"
#include <ts/json.h>

/* ---------------------------------------------------------------- text out */

EXPORT void ct_buf( CTBUF *b, UB *mem, INT max )
{
	b->p = mem;
	b->n = 0;
	b->max = max;
	b->fail = FALSE;
	b->more = NULL;
	if ( max > 0 ) mem[0] = 0;
}

EXPORT void ct_buf_grow( CTBUF *b, INT first, void *(*more)( void *p, SZ size ) )
{
	if ( first < 16 ) first = 16;
	b->p = (UB *)more(NULL, (SZ)first);
	b->n = 0;
	b->max = ( b->p != NULL ) ? first : 0;
	b->fail = (BOOL)( b->p == NULL );
	b->more = more;
	if ( b->p != NULL ) b->p[0] = 0;
}

EXPORT void ct_putn( CTBUF *b, CONST UB *s, INT n )
{
	INT	i;

	if ( n < 0 || b->fail ) return;
	if ( b->n + n + 1 > b->max && b->more != NULL ) {
		/* twice as long, or as long as wanted */
		INT	m = ( b->max < 0x3FFFFFFF / 2 ) ? b->max * 2 : 0x7FFFFFF0;
		UB	*q;

		if ( m < b->n + n + 1 ) m = b->n + n + 1;
		q = (UB *)b->more(b->p, (SZ)m);
		if ( q != NULL ) {
			b->p = q;
			b->max = m;
		}
	}
	if ( b->n + n + 1 > b->max ) {
		b->fail = TRUE;
		return;
	}
	for ( i = 0; i < n; i++ ) b->p[b->n++] = s[i];
	b->p[b->n] = 0;
}

EXPORT void ct_put( CTBUF *b, CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	ct_putn(b, (CONST UB *)s, n);
}

EXPORT void ct_num( CTBUF *b, INT v )
{
	UB	d[12], t[13];
	UW	u = ( v < 0 ) ? (UW)-v : (UW)v;
	INT	i = 0, n = 0;

	do {
		d[i++] = (UB)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	if ( v < 0 ) t[n++] = '-';
	while ( i > 0 ) t[n++] = d[--i];
	ct_putn(b, t, n);
}

EXPORT void ct_esc( CTBUF *b, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && s[i] != 0; i++ ) {
		switch ( s[i] ) {
		case '&':	ct_put(b, "&amp;");	break;
		case '<':	ct_put(b, "&lt;");	break;
		case '>':	ct_put(b, "&gt;");	break;
		case '"':	ct_put(b, "&quot;");	break;
		default:	ct_putn(b, &s[i], 1);	break;
		}
	}
}

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

/* Whether the n bytes at s are w */
LOCAL BOOL at_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( i >= n || s[i] != (UB)w[i] ) return FALSE;
	}
	return TRUE;
}

/* Where w is first found in s[from..n), or -1 */
LOCAL INT find( CONST UB *s, INT n, INT from, CONST char *w )
{
	INT	i;

	for ( i = from; i < n; i++ ) {
		if ( s[i] == (UB)w[0] && at_is(s + i, n - i, w) ) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- dates */

LOCAL BOOL leap( INT y )
{
	return (BOOL)( ( y % 4 == 0 && y % 100 != 0 ) || y % 400 == 0 );
}

EXPORT INT ct_days( INT y, INT m )
{
	static CONST UB	dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if ( m < 1 || m > 12 ) return 30;
	return ( m == 2 && leap(y) ) ? 29 : dm[m - 1];
}

EXPORT INT ct_wday( INT y, INT m, INT d )
{
	static CONST UB	t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

	if ( m < 3 ) y--;
	return ( y + y / 4 - y / 100 + y / 400 + t[( m - 1 ) % 12] + d ) % 7;
}

LOCAL INT digits( CONST UB *s, INT n )
{
	INT	i, v = 0;

	for ( i = 0; i < n; i++ ) {
		if ( s[i] < '0' || s[i] > '9' ) return -1;
		v = v * 10 + ( s[i] - '0' );
	}
	return v;
}

EXPORT BOOL ct_is_day( CONST UB *s, INT *p_y, INT *p_m, INT *p_d )
{
	INT	y, m, d;

	if ( s == NULL || s_len(s) != 10 || s[4] != '/' || s[7] != '/' ) return FALSE;
	y = digits(s, 4);
	m = digits(s + 5, 2);
	d = digits(s + 8, 2);
	if ( y < 0 || m < 1 || m > 12 || d < 1 || d > 31 ) return FALSE;
	if ( p_y != NULL ) *p_y = y;
	if ( p_m != NULL ) *p_m = m;
	if ( p_d != NULL ) *p_d = d;
	return TRUE;
}

EXPORT BOOL ct_is_month( CONST UB *s, INT *p_y, INT *p_m )
{
	INT	y, m;

	if ( s == NULL || s_len(s) != 7 || s[4] != '/' ) return FALSE;
	y = digits(s, 4);
	m = digits(s + 5, 2);
	if ( y < 0 || m < 1 || m > 12 ) return FALSE;
	if ( p_y != NULL ) *p_y = y;
	if ( p_m != NULL ) *p_m = m;
	return TRUE;
}

LOCAL void put_digits( UB *out, INT v, INT n )
{
	while ( n-- > 0 ) {
		out[n] = (UB)( '0' + v % 10 );
		v /= 10;
	}
}

EXPORT void ct_day_name( UB *out, INT y, INT m, INT d )
{
	put_digits(out, y, 4);
	out[4] = '/';
	put_digits(out + 5, m, 2);
	out[7] = '/';
	put_digits(out + 8, d, 2);
	out[10] = 0;
}

EXPORT void ct_month_name( UB *out, INT y, INT m )
{
	put_digits(out, y, 4);
	out[4] = '/';
	put_digits(out + 5, m, 2);
	out[7] = 0;
}

EXPORT void ct_cell( INT y, INT m, INT d, INT *p_row, INT *p_col )
{
	INT	i = ct_wday(y, m, 1) + d - 1;

	*p_row = i / CT_COLS;
	*p_col = i % CT_COLS;
}

EXPORT void ct_day_box( INT y, INT m, INT d, INT n, INT *box )
{
	INT	row, col, top;

	ct_cell(y, m, d, &row, &col);
	top = CT_GRID_TOP + row * CT_CELL_H + CT_DAYNO_H + CT_EV_GAP + n * ( CT_EV_H + CT_EV_GAP );
	box[0] = col * CT_CELL_W + CT_EV_GAP;
	box[1] = top;
	box[2] = col * CT_CELL_W + CT_CELL_W - CT_EV_GAP;
	box[3] = top + CT_EV_H;
}

/* ---------------------------------------------------------------- reading */

LOCAL BOOL space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* The end of a start tag at s[at] ('<'): the index after its '>' */
LOCAL INT tag_end( CONST UB *s, INT n, INT at )
{
	UB	q = 0;
	INT	i;

	for ( i = at + 1; i < n; i++ ) {
		if ( q != 0 ) {
			if ( s[i] == q ) q = 0;
		} else if ( s[i] == '"' || s[i] == '\'' ) {
			q = s[i];
		} else if ( s[i] == '>' ) {
			return i + 1;
		}
	}
	return n;
}

EXPORT INT ct_links( CONST UB *x, INT n, CTEL *out, INT max )
{
	INT	i = 0, k = 0, e, c;

	/* with out NULL, only how many there are */
	while ( ( out == NULL || k < max ) && ( i = find(x, n, i, "<link") ) >= 0 ) {
		if ( i + 5 >= n || !( space(x[i + 5]) || x[i + 5] == '/' || x[i + 5] == '>' ) ) {
			i += 5;
			continue;
		}
		e = tag_end(x, n, i);
		if ( e >= 2 && x[e - 2] != '/' ) {
			/* <link ...>words</link>: the whole of it */
			c = find(x, n, e, "</link>");
			if ( c >= 0 ) e = c + 7;
		}
		if ( out != NULL ) {
			out[k].at = i;
			out[k].len = e - i;
		}
		k++;
		i = e;
	}
	return k;
}

/*
 * The attributes of a start tag one after another: from *p_at, the
 * name's place and length and the value's (inside its quotes). FALSE at
 * the end of the tag.
 */
LOCAL BOOL next_attr( CONST UB *el, INT n, INT *p_at, INT *p_name, INT *p_nlen,
		      INT *p_val, INT *p_vlen )
{
	INT	i = *p_at, q;

	while ( i < n && el[i] != '>' ) {
		while ( i < n && ( space(el[i]) || el[i] == '/' ) ) i++;
		if ( i >= n || el[i] == '>' ) break;
		*p_name = i;
		while ( i < n && !space(el[i]) && el[i] != '=' && el[i] != '>' && el[i] != '/' ) i++;
		*p_nlen = i - *p_name;
		while ( i < n && space(el[i]) ) i++;
		if ( i < n && el[i] == '=' ) {
			i++;
			while ( i < n && space(el[i]) ) i++;
			if ( i < n && ( el[i] == '"' || el[i] == '\'' ) ) {
				q = el[i++];
				*p_val = i;
				while ( i < n && el[i] != q ) i++;
				*p_vlen = i - *p_val;
				if ( i < n ) i++;
			} else {
				*p_val = i;
				while ( i < n && !space(el[i]) && el[i] != '>' && el[i] != '/' ) i++;
				*p_vlen = i - *p_val;
			}
		} else {
			*p_val = i;
			*p_vlen = 0;
		}
		*p_at = i;
		return TRUE;
	}
	*p_at = i;
	return FALSE;
}

/* Where the attributes of an element start: past its name */
LOCAL INT attrs_at( CONST UB *el, INT n )
{
	INT	i = 1;

	while ( i < n && !space(el[i]) && el[i] != '/' && el[i] != '>' ) i++;
	return i;
}

LOCAL BOOL name_is( CONST UB *el, INT at, INT len, CONST char *name )
{
	INT	i;

	for ( i = 0; i < len; i++ ) {
		if ( name[i] == 0 || el[at + i] != (UB)name[i] ) return FALSE;
	}
	return (BOOL)( name[len] == 0 );
}

EXPORT INT ct_attr( CONST UB *el, INT n, CONST char *name, UB *out, INT max )
{
	INT	at = attrs_at(el, n), nm, nl, v, vl, i;

	while ( next_attr(el, n, &at, &nm, &nl, &v, &vl) ) {
		if ( !name_is(el, nm, nl, name) ) continue;
		for ( i = 0; i < vl && i < max - 1; i++ ) out[i] = el[v + i];
		if ( max > 0 ) out[( vl < max - 1 ) ? vl : max - 1] = 0;
		return vl;
	}
	return -1;
}

EXPORT INT ct_attr_int( CONST UB *el, INT n, CONST char *name, INT dflt )
{
	UB	s[24];
	INT	i = 0, v = 0;
	BOOL	neg = FALSE;

	if ( ct_attr(el, n, name, s, sizeof(s)) <= 0 ) return dflt;
	while ( s[i] == ' ' ) i++;
	if ( s[i] == '-' ) {
		neg = TRUE;
		i++;
	}
	if ( s[i] < '0' || s[i] > '9' ) return dflt;
	while ( s[i] >= '0' && s[i] <= '9' ) v = v * 10 + ( s[i++] - '0' );
	return neg ? -v : v;
}

LOCAL INT hexv( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT BOOL ct_uuid_parse( CONST UB *s, TS_UUID *u )
{
	INT	i, k = 0, v;

	for ( i = 0; s[i] != 0 && k < 32; i++ ) {
		if ( s[i] == '-' && ( i == 8 || i == 13 || i == 18 || i == 23 ) ) continue;
		v = hexv(s[i]);
		if ( v < 0 ) return FALSE;
		if ( ( k & 1 ) == 0 ) u->b[k / 2] = (UB)( v << 4 );
		else u->b[k / 2] |= (UB)v;
		k++;
	}
	return (BOOL)( k == 32 );
}

EXPORT void ct_uuid_str( CONST TS_UUID *u, char *out )
{
	static CONST char	hex[] = "0123456789abcdef";
	INT			i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[u->b[i] >> 4];
		out[n++] = hex[u->b[i] & 0x0F];
	}
	out[n] = 0;
}

EXPORT BOOL ct_target( CONST UB *el, INT n, TS_UUID *p_u )
{
	UB	id[64];

	if ( ct_attr(el, n, "id", id, sizeof(id)) < TS_UUID_STRLEN ) return FALSE;
	id[TS_UUID_STRLEN] = 0;
	return ct_uuid_parse(id, p_u);
}

/* ---------------------------------------------------------------- holidays */

/* An entity at s[i] ('&'): the letter it stands for into out, and its length */
LOCAL INT entity( CONST UB *s, INT n, INT i, UB *out, INT *p_len )
{
	UW	c = 0;
	INT	k;

	if ( at_is(s + i, n - i, "&amp;") ) { out[0] = '&'; *p_len = 1; return 5; }
	if ( at_is(s + i, n - i, "&lt;") ) { out[0] = '<'; *p_len = 1; return 4; }
	if ( at_is(s + i, n - i, "&gt;") ) { out[0] = '>'; *p_len = 1; return 4; }
	if ( at_is(s + i, n - i, "&quot;") ) { out[0] = '"'; *p_len = 1; return 6; }
	if ( at_is(s + i, n - i, "&apos;") ) { out[0] = '\''; *p_len = 1; return 6; }
	if ( i + 2 < n && s[i + 1] == '#' ) {
		k = i + 2;
		if ( s[k] == 'x' || s[k] == 'X' ) {
			for ( k++; k < n && hexv(s[k]) >= 0; k++ ) c = c * 16 + (UW)hexv(s[k]);
		} else {
			for ( ; k < n && s[k] >= '0' && s[k] <= '9'; k++ ) c = c * 10 + (UW)( s[k] - '0' );
		}
		if ( k < n && s[k] == ';' ) {
			if ( c < 0x80 ) {
				out[0] = (UB)c;
				*p_len = 1;
			} else if ( c < 0x800 ) {
				out[0] = (UB)( 0xC0 | ( c >> 6 ) );
				out[1] = (UB)( 0x80 | ( c & 0x3F ) );
				*p_len = 2;
			} else if ( c < 0x10000 ) {
				out[0] = (UB)( 0xE0 | ( c >> 12 ) );
				out[1] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
				out[2] = (UB)( 0x80 | ( c & 0x3F ) );
				*p_len = 3;
			} else {
				out[0] = (UB)( 0xF0 | ( c >> 18 ) );
				out[1] = (UB)( 0x80 | ( ( c >> 12 ) & 0x3F ) );
				out[2] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
				out[3] = (UB)( 0x80 | ( c & 0x3F ) );
				*p_len = 4;
			}
			return k + 1 - i;
		}
	}
	out[0] = '&';
	*p_len = 1;
	return 1;
}

/* Spaces at both ends of s[*p_a..*p_b) left off, the ideographic one too */
LOCAL void trim( CONST UB *s, INT *p_a, INT *p_b )
{
	INT	a = *p_a, b = *p_b;

	for ( ;; ) {
		if ( a < b && space(s[a]) ) a++;
		else if ( a + 2 < b && s[a] == 0xE3 && s[a + 1] == 0x80 && s[a + 2] == 0x80 ) a += 3;
		else break;
	}
	for ( ;; ) {
		if ( b > a && space(s[b - 1]) ) b--;
		else if ( b - 3 >= a && s[b - 3] == 0xE3 && s[b - 2] == 0x80 && s[b - 1] == 0x80 ) b -= 3;
		else break;
	}
	*p_a = a;
	*p_b = b;
}

/* One line "YYYY/MM/DD<tab>name" taken as a holiday */
LOCAL BOOL hol_line( CONST UB *s, INT a, INT b, CTHOL *h )
{
	UB	date[16];
	INT	t, da, db, na, nb, i;

	for ( t = a; t < b && s[t] != '\t'; t++ ) ;
	if ( t >= b ) return FALSE;
	da = a;
	db = t;
	na = t + 1;
	nb = b;
	trim(s, &da, &db);
	trim(s, &na, &nb);
	if ( db - da != 10 || nb <= na ) return FALSE;
	for ( i = 0; i < 10; i++ ) date[i] = s[da + i];
	date[10] = 0;
	if ( !ct_is_day(date, &h->y, &h->m, &h->d) ) return FALSE;
	for ( i = 0; i < nb - na && i < CT_HOL_NAME - 1; i++ ) h->name[i] = s[na + i];
	/* not cut in the middle of a letter */
	while ( i > 0 && i < nb - na && ( s[na + i] & 0xC0 ) == 0x80 ) i--;
	h->name[i] = 0;
	return TRUE;
}

#define LINE_MAX_B	512

EXPORT INT ct_hol_text( CONST UB *x, INT n, CTHOL *out, INT max )
{
	UB	line[LINE_MAX_B], e[4];
	INT	i, at, end, k = 0, ln = 0, el, len;

	at = find(x, n, 0, "<document");
	if ( at < 0 ) return 0;
	end = find(x, n, at, "</document>");
	if ( end < 0 ) end = n;
	i = tag_end(x, n, at);
	/* the text of the document: a break, a paragraph's end, a line's end each end a line */
	while ( i <= end && k < max ) {
		BOOL	eol = (BOOL)( i == end );

		if ( !eol && x[i] == '<' ) {
			INT	te = tag_end(x, n, i);

			if ( at_is(x + i, end - i, "<br") || at_is(x + i, end - i, "</p>") ) {
				eol = TRUE;
			} else if ( at_is(x + i, end - i, "<tab") ) {
				if ( ln < LINE_MAX_B ) line[ln++] = '\t';
			}
			i = te;
		} else if ( !eol && ( x[i] == '\n' || x[i] == '\r' ) ) {
			eol = TRUE;
			i++;
		} else if ( !eol && x[i] == '&' ) {
			i += entity(x, end, i, e, &len);
			for ( el = 0; el < len && ln < LINE_MAX_B; el++ ) line[ln++] = e[el];
		} else if ( !eol ) {
			if ( ln < LINE_MAX_B ) line[ln++] = x[i];
			i++;
		}
		if ( eol ) {
			if ( ln > 0 && hol_line(line, 0, ln, &out[k]) ) k++;
			ln = 0;
			if ( i == end ) break;
		}
	}
	return k;
}

EXPORT INT ct_hol_month( CONST UB *x, INT n, INT y, INT m, CTHOL *out, INT max )
{
	INT	at = 0, end, dm, a, b, k = 0, col, row, cell, d, i;

	while ( k < max && ( at = find(x, n, at, "<document") ) >= 0 ) {
		end = find(x, n, at, "</document>");
		if ( end < 0 ) break;
		dm = find(x, end, at, "<docmemo");
		if ( dm >= 0 ) {
			INT	te = tag_end(x, end, dm);
			UB	memo[16];
			INT	dv = find(x, end, at, "<docView");

			if ( ct_attr(x + dm, te - dm, "text", memo, sizeof(memo)) == 7
			  && at_is(memo, 7, "holiday") && dv >= 0 ) {
				INT	dve = tag_end(x, end, dv);
				INT	left = ct_attr_int(x + dv, dve - dv, "viewleft", -1);
				INT	top = ct_attr_int(x + dv, dve - dv, "viewtop", -1);

				/* the name: the words after the note, to the next element */
				a = te;
				for ( b = a; b < end && x[b] != '<'; b++ ) ;
				trim(x, &a, &b);
				col = ( left >= 0 ) ? left / CT_CELL_W : -1;
				row = ( top >= CT_GRID_TOP ) ? ( top - CT_GRID_TOP ) / CT_CELL_H : -1;
				cell = row * CT_COLS + col;
				d = cell - ct_wday(y, m, 1) + 1;
				if ( col >= 0 && col < CT_COLS && row >= 0 && row < CT_ROWS
				  && d >= 1 && d <= ct_days(y, m) && b > a ) {
					out[k].y = y;
					out[k].m = m;
					out[k].d = d;
					for ( i = 0; i < b - a && i < CT_HOL_NAME - 1; i++ ) out[k].name[i] = x[a + i];
					while ( i > 0 && i < b - a && ( x[a + i] & 0xC0 ) == 0x80 ) i--;
					out[k].name[i] = 0;
					k++;
				}
			}
		}
		at = end + 11;
	}
	return k;
}

EXPORT CONST UB *ct_hol_find( CONST CTHOL *h, INT nh, INT y, INT m, INT d )
{
	INT	i;

	for ( i = 0; i < nh; i++ ) {
		if ( h[i].y == y && h[i].m == m && h[i].d == d ) return h[i].name;
	}
	return NULL;
}

/* ---------------------------------------------------------------- writing */

/*
 * The colours the grid uses, as the patterns of the figure's own: the
 * band's ground, the grey of lines and other months' numbers, the
 * lines between cells, a cell's ground, another month's cell, today.
 */
#define PAT_BAND	128
#define PAT_GREY	129
#define PAT_LINE	130
#define PAT_CELL	131
#define PAT_OTHER	132
#define PAT_TODAY	133

LOCAL CONST char *CONST pat_col[6] = {
	"#e0e0e0", "#808080", "#c0c0c0", "#ffffff", "#f0f0f0", "#ffffd0"
};

LOCAL void rect( CTBUF *b, INT f, INT l, INT left, INT top, INT right, INT bottom, INT z )
{
	ct_put(b, "<rect round=\"0\" lineType=\"0\" lineWidth=\"1\" f_pat=\"");
	ct_num(b, f);
	ct_put(b, "\" l_pat=\"");
	ct_num(b, l);
	ct_put(b, "\" left=\"");
	ct_num(b, left);
	ct_put(b, "\" top=\"");
	ct_num(b, top);
	ct_put(b, "\" right=\"");
	ct_num(b, right);
	ct_put(b, "\" bottom=\"");
	ct_num(b, bottom);
	ct_put(b, "\" zIndex=\"");
	ct_num(b, z);
	ct_put(b, "\"/>");
}

/* A piece of text standing in the figure, its head as every one of the grid has */
LOCAL void doc_head( CTBUF *b, INT l, INT t, INT r, INT bt, INT z, INT size, CONST char *col,
		     CONST char *align )
{
	ct_put(b, "<document>\n<docView viewleft=\"");
	ct_num(b, l);
	ct_put(b, "\" viewtop=\"");
	ct_num(b, t);
	ct_put(b, "\" viewright=\"");
	ct_num(b, r);
	ct_put(b, "\" viewbottom=\"");
	ct_num(b, bt);
	ct_put(b, "\"/>\n<docDraw drawleft=\"");
	ct_num(b, l);
	ct_put(b, "\" drawtop=\"");
	ct_num(b, t);
	ct_put(b, "\" drawright=\"");
	ct_num(b, r);
	ct_put(b, "\" drawbottom=\"");
	ct_num(b, bt);
	ct_put(b, "\"/>\n<docScale hunit=\"-96\" vunit=\"-96\"/>\n<text zIndex=\"");
	ct_num(b, z);
	ct_put(b, "\"/>\n<font size=\"");
	ct_num(b, size);
	ct_put(b, "\"/>\n<font color=\"");
	ct_put(b, col);
	ct_put(b, "\"/>\n<text align=\"");
	ct_put(b, align);
	ct_put(b, "\"/>\n");
}

LOCAL CONST char *CONST wd_name[7] = { "日", "月", "火", "水", "木", "金", "土" };

/* The attributes a link's box is in */
LOCAL CONST char *CONST box_attr[5] = { "vobjleft", "vobjtop", "vobjright", "vobjbottom", "height" };

LOCAL INT box_index( CONST UB *el, INT at, INT len )
{
	INT	i;

	for ( i = 0; i < 5; i++ ) {
		if ( name_is(el, at, len, box_attr[i]) ) return i;
	}
	return -1;
}

LOCAL void attr_num( CTBUF *b, CONST char *name, INT v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_num(b, v);
	ct_put(b, "\"");
}

LOCAL void attr_str( CTBUF *b, CONST char *name, CONST char *v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_put(b, v);
	ct_put(b, "\"");
}

EXPORT void ct_link_put( CTBUF *b, CONST UB *el, INT len, CONST INT *box, INT z, BOOL day )
{
	static CONST char *CONST day_attr[5] = { "chsz", "frcol", "chcol", "tbcol", "bgcol" };
	static CONST char *CONST day_val[5] = { "10", CT_COL_DAYFR, CT_COL_TEXT, CT_COL_DAYBG, CT_COL_DAYBG };
	INT	at = attrs_at(el, len), nm, nl, v, vl, i, k;
	BOOL	seen_box[5] = { FALSE, FALSE, FALSE, FALSE, FALSE }, seen_z = FALSE;
	BOOL	seen_day[5] = { FALSE, FALSE, FALSE, FALSE, FALSE };
	INT	val[5] = { 0, 0, 0, 0, 0 };

	if ( box != NULL ) {
		val[0] = box[0];
		val[1] = box[1];
		val[2] = box[2];
		val[3] = box[3];
		val[4] = box[3] - box[1];
	}
	ct_put(b, "<link");
	while ( next_attr(el, len, &at, &nm, &nl, &v, &vl) ) {
		k = ( box != NULL ) ? box_index(el, nm, nl) : -1;
		if ( k >= 0 ) {
			seen_box[k] = TRUE;
			ct_put(b, " ");
			ct_putn(b, el + nm, nl);
			ct_put(b, "=\"");
			ct_num(b, val[k]);
			ct_put(b, "\"");
			continue;
		}
		if ( name_is(el, nm, nl, "zIndex") ) {
			seen_z = TRUE;
			if ( z >= 0 ) {
				attr_num(b, "zIndex", z);
				continue;
			}
		}
		for ( i = 0; i < 5; i++ ) {
			if ( name_is(el, nm, nl, day_attr[i]) ) {
				seen_day[i] = TRUE;
				/* an empty one is the calendar's, as one not there */
				if ( day && vl == 0 ) {
					attr_str(b, day_attr[i], day_val[i]);
					nl = -1;
				}
			}
		}
		if ( nl < 0 ) continue;
		/* the attribute as it was written */
		ct_put(b, " ");
		ct_putn(b, el + nm, ( v + vl < len && ( el[v + vl] == '"' || el[v + vl] == '\'' ) )
				   ? v + vl + 1 - nm : v + vl - nm);
	}
	for ( k = 0; box != NULL && k < 5; k++ ) {
		if ( !seen_box[k] ) attr_num(b, box_attr[k], val[k]);
	}
	for ( i = 0; day && i < 5; i++ ) {
		if ( !seen_day[i] ) attr_str(b, day_attr[i], day_val[i]);
	}
	if ( z >= 0 && !seen_z ) attr_num(b, "zIndex", z);
	ct_put(b, "/>");
}

EXPORT void ct_link_new( CTBUF *b, CONST TS_UUID *target, CONST TS_UUID *vobjid,
			 CONST INT *box, BOOL day )
{
	char	us[TS_UUID_STRLEN + 1];

	ct_uuid_str(target, us);
	ct_put(b, "<link id=\"");
	ct_put(b, us);
	ct_put(b, "_0.xtad\" vobjid=\"");
	ct_uuid_str(vobjid, us);
	ct_put(b, us);
	ct_put(b, "\"");
	attr_num(b, "vobjleft", box[0]);
	attr_num(b, "vobjtop", box[1]);
	attr_num(b, "vobjright", box[2]);
	attr_num(b, "vobjbottom", box[3]);
	attr_num(b, "height", box[3] - box[1]);
	attr_str(b, "chsz", day ? "10" : "14");
	attr_str(b, "frcol", day ? CT_COL_DAYFR : "#000000");
	attr_str(b, "chcol", CT_COL_TEXT);
	attr_str(b, "tbcol", day ? CT_COL_DAYBG : "#ffffff");
	attr_str(b, "bgcol", day ? CT_COL_DAYBG : "#ffffff");
	ct_put(b, " dlen=\"0\" pictdisp=\"true\" namedisp=\"true\" roledisp=\"false\" typedisp=\"false\""
		  " updatedisp=\"false\" framedisp=\"true\" autoopen=\"false\" hidden=\"false\""
		  " scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>");
}

EXPORT ER ct_month_xml( CTBUF *b, INT y, INT m, INT today, CONST CTHOL *h, INT nh,
			CONST CTLNK *l, INT nl )
{
	UB		mn[8];
	INT		i, row, col, z, start = ct_wday(y, m, 1), days = ct_days(y, m);
	INT		pm = ( m == 1 ) ? 12 : m - 1, py = ( m == 1 ) ? y - 1 : y;
	INT		prev_days = ct_days(py, pm), counter = 1, next = 1;

	ct_month_name(mn, y, m);
	ct_put(b, "<tad version=\"1.0\" filename=\"");
	ct_put(b, (CONST char *)mn);
	ct_put(b, "\" encoding=\"UTF-8\">\n<figure>\n"
		  "<figView top=\"0\" left=\"0\" right=\"800\" bottom=\"600\"/>\n"
		  "<figDraw top=\"0\" left=\"0\" right=\"800\" bottom=\"600\"/>\n"
		  "<figScale hunit=\"-96\" vunit=\"-96\"/>\n<patterns>\n");
	for ( i = 0; i < 6; i++ ) {
		ct_put(b, "<pattern id=\"");
		ct_num(b, PAT_BAND + i);
		ct_put(b, "\" width=\"1\" height=\"1\" ncol=\"1\" fgcolors=\"");
		ct_put(b, pat_col[i]);
		ct_put(b, "\" bgcolor=\"#ffffff\"/>\n");
	}
	ct_put(b, "</patterns>\n<!-- ナビゲーションバー背景 -->\n");
	rect(b, PAT_BAND, PAT_GREY, 0, 0, CT_BASE_W, CT_NAV_H, 1);
	ct_put(b, "\n<!-- 年月表示テキスト (太字) -->\n");
	doc_head(b, 350, 5, 450, 35, 2, 12, CT_COL_TEXT, "center");
	ct_put(b, "<bold>");
	ct_put(b, (CONST char *)mn);
	ct_put(b, "</bold>\n</document>\n\n<!-- 曜日ヘッダー背景 -->\n");
	rect(b, PAT_BAND, PAT_GREY, 0, CT_NAV_H, CT_BASE_W, CT_GRID_TOP, 3);
	ct_put(b, "\n\n");

	/* the days of the week */
	for ( i = 0; i < 7; i++ ) {
		ct_put(b, "<!-- 曜日: ");
		ct_put(b, wd_name[i]);
		ct_put(b, " -->\n");
		doc_head(b, i * CT_CELL_W, CT_NAV_H, ( i + 1 ) * CT_CELL_W, CT_GRID_TOP, 4 + i, 9,
			 ( i == 0 ) ? CT_COL_SUNDAY : ( i == 6 ) ? CT_COL_SATURDAY : CT_COL_TEXT, "center");
		ct_put(b, wd_name[i]);
		ct_put(b, "\n</document>\n\n");
	}

	/* the cells: their ground, the day's number, the holiday's name */
	z = 10;
	for ( row = 0; row < CT_ROWS; row++ ) {
		for ( col = 0; col < CT_COLS; col++ ) {
			INT		cell = row * CT_COLS + col, num, cpat = PAT_CELL;
			INT		left = col * CT_CELL_W, top = CT_GRID_TOP + row * CT_CELL_H;
			CONST char	*dcol = CT_COL_TEXT;
			CONST UB	*hol = NULL;

			if ( cell < start ) {
				num = prev_days - start + cell + 1;
				dcol = CT_COL_GREY;
				cpat = PAT_OTHER;
			} else if ( counter <= days ) {
				num = counter;
				if ( col == 0 ) dcol = CT_COL_SUNDAY;
				else if ( col == 6 ) dcol = CT_COL_SATURDAY;
				hol = ct_hol_find(h, nh, y, m, counter);
				if ( hol != NULL ) dcol = CT_COL_SUNDAY;
				if ( today == counter ) cpat = PAT_TODAY;
				counter++;
			} else {
				num = next++;
				dcol = CT_COL_GREY;
				cpat = PAT_OTHER;
			}
			rect(b, cpat, PAT_LINE, left, top, left + CT_CELL_W, top + CT_CELL_H, z++);
			ct_put(b, "\r\n");
			doc_head(b, left + 4, top + 2, left + 30, top + 18, z++, 9, dcol, "left");
			ct_num(b, num);
			ct_put(b, "\n</document>\n");
			if ( hol != NULL ) {
				doc_head(b, left + 30, top + 2, left + CT_CELL_W - 4, top + 18, z++, 8,
					 CT_COL_SUNDAY, "left");
				ct_put(b, "<docmemo text=\"holiday\"/>");
				ct_esc(b, hol, CT_HOL_NAME);
				ct_put(b, "\n</document>\n");
			}
		}
	}

	/* the links: a day's in its cell, under the day's others; the rest where they are */
	z = 200;
	for ( i = 0; i < nl; i++ ) {
		INT	box[4], dy, dmn, dd, k, n = 0;
		BOOL	day = ct_is_day(l[i].name, &dy, &dmn, &dd);

		if ( day ) {
			for ( k = 0; k < i; k++ ) {
				CONST UB	*a = l[k].name, *c = l[i].name;
				INT		j;

				for ( j = 0; a != NULL && j < 10 && a[j] == c[j]; j++ ) ;
				if ( j == 10 && a[10] == 0 ) n++;
			}
			ct_day_box(dy, dmn, dd, n, box);
		} else {
			box[0] = ct_attr_int(l[i].el, l[i].len, "vobjleft", 0);
			box[1] = ct_attr_int(l[i].el, l[i].len, "vobjtop", 0);
			box[2] = ct_attr_int(l[i].el, l[i].len, "vobjright", 0);
			box[3] = ct_attr_int(l[i].el, l[i].len, "vobjbottom", 0);
			if ( box[2] == 0 ) box[2] = box[0] + 150;
			if ( box[3] == 0 ) box[3] = box[1] + CT_LINK_H;
		}
		ct_link_put(b, l[i].el, l[i].len, box, z++, TRUE);
		ct_put(b, "\r\n");
	}
	ct_put(b, "\n</figure>\n</tad>");
	return b->fail ? E_LIMIT : E_OK;
}

EXPORT ER ct_insert( CTBUF *b, CONST UB *x, INT n, CONST UB *el, INT len )
{
	INT	at = -1, i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] == '<' && ( at_is(x + i, n - i, "</figure>") ) ) at = i;
	}
	if ( at < 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( x[i] == '<' && at_is(x + i, n - i, "</document>") ) at = i;
		}
	}
	if ( at < 0 ) return E_PAR;
	ct_putn(b, x, at);
	ct_putn(b, el, len);
	ct_put(b, "\n");
	ct_putn(b, x + at, n - at);
	return b->fail ? E_LIMIT : E_OK;
}

EXPORT void ct_apply_date( CTBUF *b, CONST UB *tpl, INT n, CONST UB *day )
{
	INT	i = 0, f = find(tpl, n, 0, "filename=\"");

	while ( i < n ) {
		if ( i == f ) {
			INT	e;

			/* the first filename: the day's */
			for ( e = i + 10; e < n && tpl[e] != '"'; e++ ) ;
			ct_put(b, "filename=\"");
			ct_put(b, (CONST char *)day);
			ct_put(b, "\"");
			i = ( e < n ) ? e + 1 : n;
			continue;
		}
		if ( tpl[i] == 'Y' && at_is(tpl + i, n - i, "YYYY/MM/DD") ) {
			ct_put(b, (CONST char *)day);
			i += 10;
			continue;
		}
		ct_putn(b, tpl + i, 1);
		i++;
	}
}

EXPORT void ct_day_doc( CTBUF *b, CONST UB *day )
{
	ct_put(b, "<tad version=\"1.0\" filename=\"");
	ct_put(b, (CONST char *)day);
	ct_put(b, "\" encoding=\"UTF-8\">\n<document>\n<p>\n<font size=\"20\"/>\n<text align=\"center\"/>\n");
	ct_put(b, (CONST char *)day);
	ct_put(b, "\n</p>\n</document>\n</tad>\n");
}

/* ---------------------------------------------------------------- changing a link */

LOCAL INT set_index( CONST UB *el, INT at, INT len, CONST CTSET *set, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( name_is(el, at, len, set[i].name) ) return i;
	}
	return -1;
}

LOCAL void set_put( CTBUF *b, CONST CTSET *s )
{
	ct_put(b, " ");
	ct_put(b, s->name);
	ct_put(b, "=\"");
	ct_esc(b, s->value, 0x7FFFFFFF);
	ct_put(b, "\"");
}

EXPORT void ct_link_edit( CTBUF *b, CONST UB *el, INT len, CONST CTSET *set, INT n )
{
	INT	at = attrs_at(el, len), nm, nl, v, vl, k, i;
	BOOL	seen[16];

	for ( i = 0; i < 16; i++ ) seen[i] = FALSE;
	if ( n > 16 ) n = 16;
	ct_put(b, "<link");
	while ( next_attr(el, len, &at, &nm, &nl, &v, &vl) ) {
		k = set_index(el, nm, nl, set, n);
		if ( k >= 0 ) {
			/* a second one of the same name goes, as the first is the one that counts */
			if ( !seen[k] && set[k].value != NULL ) set_put(b, &set[k]);
			seen[k] = TRUE;
			continue;
		}
		ct_put(b, " ");
		ct_putn(b, el + nm, ( v + vl < len && ( el[v + vl] == '"' || el[v + vl] == '\'' ) )
				   ? v + vl + 1 - nm : v + vl - nm);
	}
	for ( k = 0; k < n; k++ ) {
		if ( !seen[k] && set[k].value != NULL ) set_put(b, &set[k]);
	}
	ct_put(b, "/>");
}

EXPORT INT ct_attr_text( CONST UB *el, INT n, CONST char *name, UB *out, INT max )
{
	INT	at = attrs_at(el, n), nm, nl, v, vl, i, k = 0, len, j;
	UB	e[4];
	BOOL	cut = FALSE;

	while ( next_attr(el, n, &at, &nm, &nl, &v, &vl) ) {
		if ( !name_is(el, nm, nl, name) ) continue;
		for ( i = 0; i < vl; ) {
			if ( el[v + i] == '&' ) {
				i += entity(el + v, vl, i, e, &len);
			} else {
				e[0] = el[v + i];
				len = 1;
				i++;
			}
			for ( j = 0; j < len; j++ ) {
				if ( k < max - 1 ) out[k++] = e[j];
				else cut = TRUE;
			}
		}
		/* cut short: not in the middle of a letter */
		if ( cut ) {
			while ( k > 0 && ( out[k - 1] & 0xC0 ) == 0x80 ) k--;
			if ( k > 0 && ( out[k - 1] & 0xC0 ) == 0xC0 ) k--;
		}
		if ( max > 0 ) out[k] = 0;
		return k;
	}
	if ( max > 0 ) out[0] = 0;
	return -1;
}

EXPORT BOOL ct_attr_bool( CONST UB *el, INT n, CONST char *name, BOOL dflt )
{
	UB	s[8];

	if ( ct_attr(el, n, name, s, sizeof(s)) <= 0 ) return dflt;
	if ( at_is(s, 4, "true") ) return TRUE;
	if ( at_is(s, 5, "false") ) return FALSE;
	return dflt;
}

EXPORT INT ct_band_h( INT chsz )
{
	return ( ( chsz > 0 ) ? chsz : 14 ) + 11;
}

#define OPEN_SLACK	8

EXPORT INT ct_snap_h( INT chsz, INT h )
{
	INT	band = ct_band_h(chsz);

	return ( h <= band + OPEN_SLACK ) ? band : h;
}

EXPORT void ct_fit_box( INT *box )
{
	INT	w = box[2] - box[0], h = box[3] - box[1], d;

	if ( w > CT_BASE_W ) w = CT_BASE_W;
	if ( h > CT_BASE_H - CT_GRID_TOP ) h = CT_BASE_H - CT_GRID_TOP;
	d = 0;
	if ( box[0] + w > CT_BASE_W ) d = CT_BASE_W - w - box[0];
	if ( box[0] + d < 0 ) d = -box[0];
	box[0] += d;
	d = 0;
	if ( box[1] + h > CT_BASE_H ) d = CT_BASE_H - h - box[1];
	if ( box[1] + d < CT_GRID_TOP ) d = CT_GRID_TOP - box[1];
	box[1] += d;
	box[2] = box[0] + w;
	box[3] = box[1] + h;
}

/* ---------------------------------------------------------------- the tray */

EXPORT ER ct_print_page( CTBUF *b, CONST UB *x, INT n )
{
	INT	a;

	for ( a = 0; a + 7 <= n; a++ ) {
		if ( x[a] == '<' && x[a + 1] == 'f' && x[a + 2] == 'i' && x[a + 3] == 'g'
		  && x[a + 4] == 'u' && x[a + 5] == 'r' && x[a + 6] == 'e' ) {
			break;
		}
	}
	if ( a + 7 > n ) {
		return E_PAR;			/* not a figure */
	}
	ct_putn(b, x, a);
	ct_put(b, "<paper type=\"fig\" length=\"");
	ct_num(b, CT_PRINT_H);
	ct_put(b, "\" width=\"");
	ct_num(b, CT_PRINT_W);
	ct_put(b, "\" top=\"0\" bottom=\"0\" left=\"0\" right=\"0\"/>\n");
	ct_putn(b, x + a, n - a);
	return b->fail ? E_LIMIT : E_OK;
}

EXPORT ER ct_frag( CTBUF *b, CONST CTLNK *l, INT n )
{
	INT	i;

	ct_put(b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n");
	for ( i = 0; i < n; i++ ) {
		ct_link_put(b, l[i].el, l[i].len, NULL, -1, FALSE);
		ct_put(b, "\n");
	}
	ct_put(b, "</figure>\n</tad>\n");
	return b->fail ? E_LIMIT : E_OK;
}

/* ---------------------------------------------------------------- applists */

/* The entry the object opens with: the member whose "defaultOpen" is true */
LOCAL BOOL app_default( CONST UB *json, INT n, T_JSON *entry )
{
	T_JSON	root, it;

	if ( js_parse(json, n, &root) < E_OK || js_type(&root) != JS_OBJECT ) return FALSE;
	it.s = NULL;
	it.len = 0;
	while ( js_next(&root, &it) ) {
		if ( js_type(&it) == JS_OBJECT && js_get_bool(&it, "defaultOpen", FALSE) ) {
			*entry = it;
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT BOOL ct_app_locked( CONST UB *json, INT n )
{
	T_JSON	e;

	return (BOOL)( app_default(json, n, &e) && js_get_bool(&e, "defaultOpenLock", FALSE) );
}

/*
 * A request about a link to the desktop given the link's own applist
 * (DT_F_APPLIST): the desktop decides the program from it as it does for
 * a link of its own window, so that a link that keeps to its program
 * (起動アプリの固定) opens with that one when it is opened by a double
 * press. A link with no applist, or one too long to carry, is asked of
 * as before, by its object alone.
 */
EXPORT void ct_link_req( T_DTREQ *rq, CONST UB *el, INT n )
{
	INT	k;

	if ( rq == NULL || el == NULL ) return;
	/* one that fills the room may have been cut short: not carried */
	k = ct_attr_text(el, n, "applist", rq->text, (INT)sizeof(rq->text));
	if ( k > 0 && k < (INT)sizeof(rq->text) - 1 ) {
		rq->flags |= DT_F_APPLIST;
	} else {
		rq->text[0] = 0;
	}
}

EXPORT ER ct_app_lock( CTBUF *b, CONST UB *json, INT n, BOOL lock )
{
	T_JSON	e, old;
	INT	at, end;

	if ( !app_default(json, n, &e) ) return E_NOEXS;
	if ( js_get(&e, "defaultOpenLock", &old) >= E_OK ) {
		at = (INT)( old.s - json );
		end = at + old.len;
		ct_putn(b, json, at);
		ct_put(b, lock ? "true" : "false");
	} else {
		/* before the entry's closing brace, after what it has */
		end = (INT)( e.s - json ) + e.len - 1;
		at = end;
		while ( at > 0 && space(json[at - 1]) ) at--;
		ct_putn(b, json, at);
		ct_put(b, ( json[at - 1] == '{' ) ? "\"defaultOpenLock\":" : ",\"defaultOpenLock\":");
		ct_put(b, lock ? "true" : "false");
	}
	ct_putn(b, json + end, n - end);
	return b->fail ? E_LIMIT : E_OK;
}

/* The key of an object's member whose value starts at v: the string before its colon */
LOCAL INT member_key( CONST UB *json, CONST UB *v, UB *out, INT max )
{
	CONST UB	*p = v;
	INT		len, i;

	while ( p > json && space(p[-1]) ) p--;
	if ( p <= json || p[-1] != ':' ) return -1;
	p--;
	while ( p > json && space(p[-1]) ) p--;
	if ( p <= json || p[-1] != '"' ) return -1;
	p--;
	len = 0;
	while ( p - len > json && p[-len - 1] != '"' ) len++;
	for ( i = 0; i < len && i < max - 1; i++ ) out[i] = p[i - len];
	out[i] = 0;
	return i;
}

EXPORT INT ct_app_list( CONST UB *json, INT n, UB (*ids)[CT_APP_ID], UB (*names)[CT_APP_NAME], INT max )
{
	T_JSON	root, it;
	INT	k = 0;

	if ( js_parse(json, n, &root) < E_OK || js_type(&root) != JS_OBJECT ) return 0;
	it.s = NULL;
	it.len = 0;
	while ( k < max && js_next(&root, &it) ) {
		if ( js_type(&it) != JS_OBJECT || member_key(json, it.s, ids[k], CT_APP_ID) <= 0 ) continue;
		if ( js_get_str(&it, "name", names[k], CT_APP_NAME) <= 0 ) {
			INT	i;

			for ( i = 0; ids[k][i] != 0; i++ ) names[k][i] = ids[k][i];
			names[k][i] = 0;
		}
		k++;
	}
	return k;
}

EXPORT BOOL ct_app_entry( CONST T_DTANS *an, UB *id, UB *name, BOOL *p_kind )
{
	INT	i = 0, k;

	if ( an == NULL || an->er < E_OK ) return FALSE;
	for ( k = 0; i < DT_TEXT_MAX && an->text[i] != 0 && an->text[i] != '\t'; i++ ) {
		if ( k < CT_APP_ID - 1 ) id[k++] = an->text[i];
	}
	id[k] = 0;
	if ( k == 0 || i >= DT_TEXT_MAX || an->text[i] != '\t' ) return FALSE;
	for ( i++, k = 0; i < DT_TEXT_MAX && an->text[i] != 0; i++ ) {
		if ( k < CT_APP_NAME - 1 ) name[k++] = an->text[i];
	}
	name[k] = 0;
	if ( p_kind != NULL ) *p_kind = (BOOL)( ( an->flags & DT_F_KIND ) != 0 );
	return TRUE;
}

EXPORT UW ct_json_colour( CONST UB *json, INT n, CONST char *outer, CONST char *name, UW dflt )
{
	T_JSON	root, o;
	UB	s[16];
	UW	c = 0;
	INT	i, h;

	if ( js_parse(json, n, &root) < E_OK ) return dflt;
	if ( outer != NULL ) {
		if ( js_get(&root, outer, &o) < E_OK ) return dflt;
	} else {
		o = root;
	}
	if ( js_get_str(&o, name, s, sizeof(s)) != 7 || s[0] != '#' ) return dflt;
	for ( i = 1; i < 7; i++ ) {
		h = hexv(s[i]);
		if ( h < 0 ) return dflt;
		c = ( c << 4 ) | (UW)h;
	}
	return c;
}
