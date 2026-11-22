/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_merge.c
 *	マイクロカード: the cards merged into a layout, page by page
 *	(design 11.18)
 *
 *	A layout is a figure or a text (an object in the layout cabinet, the
 *	印刷書式). Its letters beginning with % are made the card's: %(名前)
 *	and %nC the value of a field, %nI a field's name, %CT %Ct %CN %Cn
 *	the counts and places of the card, %N the object's name, %P %T the
 *	page and the pages, %Y %y %M %m %D %d the day, %% a %. With ? after
 *	the % (%?1C) a letter that names nothing gives nothing, without it
 *	the letters stay as they are; with - (%-5C) the value is put to the
 *	right of the room the field has, spaces before it.
 *
 *	A figure's elements are the card's, put again for each card on the
 *	page at the place the 印刷設定 gives it (across and down, so far
 *	apart), but those 背景化 (background="true"), which are the page's
 *	and put once on it. A text's whole is one card's, a card a page. The
 *	list as it is shown (一覧表(標準形式)) is a text of the names and the
 *	values, a line a card, as many as the page holds.
 *
 *	Each page goes to the sink as its elements, in the page's own place:
 *	the program's sink puts the pages in one object, one after another,
 *	and a sink that makes PDF takes the same pages.
 */

#include "mc_db.h"

#define PAGE_MAX	500
#define LIST_PT		10		/* the letters of the list on paper */

LOCAL BOOL at_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( i >= n || s[i] != (UB)w[i] ) return FALSE;
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

LOCAL INT tag_end( CONST UB *s, INT n, INT at )
{
	INT	i;
	UB	q = 0;

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

/* ---------------------------------------------------------------- the paper */

EXPORT void mc_paper( CONST MCPRINT *p, INT *p_w, INT *p_h, INT *margin )
{
	static CONST INT pt[MC_PAPERS][2] = {
		{ 595, 842 }, { 842, 1191 }, { 420, 595 }, { 729, 1032 }, { 516, 729 }, { 612, 792 }
	};
	INT	k = ( p->paper >= 0 && p->paper < MC_PAPERS ) ? p->paper : 0, i;
	INT	w = pt[k][0] * 4 / 3, h = pt[k][1] * 4 / 3;

	*p_w = p->turn ? h : w;
	*p_h = p->turn ? w : h;
	for ( i = 0; i < 4; i++ ) margin[i] = p->margin[i] * 3780 / 1000;	/* mm as dots at 96 an inch */
}

EXPORT INT mc_targets( CONST MCDB *db, BOOL one, INT cur, INT *idx, INT max )
{
	INT	i, n = 0;

	if ( one ) {
		i = mc_index(db, cur);
		if ( i >= 0 && max > 0 ) idx[n++] = i;
		return n;
	}
	for ( i = 0; i < db->n && n < max; i++ ) {
		if ( mc_in(db, i) ) idx[n++] = i;
	}
	return n;
}

LOCAL INT list_row( void )
{
	return LIST_PT * 4 / 3 + 8;
}

/* The cards a page holds: the list's lines, or the 印刷設定's cards */
LOCAL INT per_page( CONST MCPRINT *p, BOOL fig, BOOL list )
{
	INT	w, h, m[4], rows;

	if ( list ) {
		mc_paper(p, &w, &h, m);
		rows = ( h - m[0] - m[1] ) / list_row() - 1;
		if ( p->down > 0 && p->down < rows ) rows = p->down;
		return ( rows > 0 ) ? rows : 1;
	}
	if ( !fig ) return 1;
	return ( p->across > 0 ? p->across : 1 ) * ( p->down > 0 ? p->down : 1 );
}

EXPORT INT mc_pages( CONST MCDB *db, CONST MCPRINT *p, BOOL fig, BOOL list, INT ntgt )
{
	INT	per = per_page(p, fig, list);

	(void)db;
	if ( ntgt <= 0 ) return 0;
	return ( ntgt + per - 1 ) / per;
}

/* ---------------------------------------------------------------- the % letters */

LOCAL void put_num( CTBUF *b, INT v, INT width )
{
	UB	d[16];
	INT	n = 0;

	do {
		d[n++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < 15 );
	while ( n < width ) d[n++] = '0';
	while ( n > 0 ) ct_putn(b, &d[--n], 1);
}

LOCAL void put_text( CTBUF *b, CONST UB *s, INT n, BOOL esc )
{
	if ( esc ) ct_esc(b, s, n);
	else ct_putn(b, s, n);
}

/*
 * What a % at s[0] stands for written to b: the bytes it took, 0 when it
 * stands for nothing (then it is written as it is).
 */
LOCAL INT one_subst( CTBUF *b, CONST UB *s, INT n, CONST MCDB *db, CONST MCCARD *c, INT all_i, INT tgt_i,
		     INT ntgt, CONST MCMINFO *mi, INT page, BOOL esc )
{
	INT	i = 1, f = -1, num = 0, digits = 0;
	BOOL	quiet = FALSE, right = FALSE, name = FALSE;
	UB	*v;

	if ( n >= 2 && s[1] == '%' ) {
		ct_put(b, "%");
		return 2;
	}
	if ( i < n && s[i] == '?' ) {
		quiet = TRUE;
		i++;
	}
	if ( i < n && s[i] == '-' ) {
		right = TRUE;
		i++;
	}
	if ( i < n && s[i] == '(' ) {
		INT	e = i + 1, k;

		while ( e < n && s[e] != ')' && e - i < MC_NAME_MAX ) e++;
		if ( e >= n || s[e] != ')' ) return 0;
		for ( k = 0; k < db->nf; k++ ) {
			INT	ln = mc_len(db->f[k].name), j;

			if ( ln != e - i - 1 ) continue;
			for ( j = 0; j < ln && db->f[k].name[j] == s[i + 1 + j]; j++ ) ;
			if ( j == ln ) f = k;
		}
		i = e + 1;
		if ( f < 0 ) return quiet ? i : 0;
	} else {
		while ( i < n && s[i] >= '0' && s[i] <= '9' && digits < 3 ) {
			num = num * 10 + ( s[i++] - '0' );
			digits++;
		}
		if ( digits > 0 ) {
			if ( i >= n || ( s[i] != 'C' && s[i] != 'I' ) ) return 0;
			name = (BOOL)( s[i] == 'I' );
			i++;
			f = num - 1;
			if ( f < 0 || f >= db->nf ) return quiet ? i : 0;
		} else if ( !quiet && !right && i < n ) {
			INT	two = ( i + 1 < n ) ? s[i + 1] : 0;

			if ( s[i] == 'C' && two == 'T' ) { put_num(b, db->n, 0); return i + 2; }
			if ( s[i] == 'C' && two == 't' ) { put_num(b, ntgt, 0); return i + 2; }
			if ( s[i] == 'C' && two == 'N' ) { put_num(b, all_i + 1, 0); return i + 2; }
			if ( s[i] == 'C' && two == 'n' ) { put_num(b, tgt_i + 1, 0); return i + 2; }
			switch ( s[i] ) {
			case 'N':	if ( mi->name != NULL ) put_text(b, mi->name, mc_len(mi->name), esc);
					return i + 1;
			case 'P':	put_num(b, page, 0);				return i + 1;
			case 'T':	put_num(b, mi->pages, 0);			return i + 1;
			case 'Y':	put_num(b, mi->y % 100, 2);			return i + 1;
			case 'y':	put_num(b, mi->y, 4);				return i + 1;
			case 'M':	put_num(b, mi->m, 0);				return i + 1;
			case 'm':	put_num(b, mi->m, 2);				return i + 1;
			case 'D':	put_num(b, mi->d, 0);				return i + 1;
			case 'd':	put_num(b, mi->d, 2);				return i + 1;
			default:	return 0;
			}
		} else {
			return 0;
		}
	}
	if ( name ) {
		put_text(b, db->f[f].name, mc_len(db->f[f].name), esc);
		return i;
	}
	v = (UB *)mc_alloc(MC_VALUE_MAX + 8);
	if ( v == NULL ) return i;
	{
		INT	vn = ( c != NULL ) ? mc_shown(db, c, f, v, MC_VALUE_MAX) : 0;

		if ( right ) {
			/* the value to the right of the field's room: full width spaces, or half for a number */
			INT	pad = db->f[f].max - mc_letters(v, vn);

			while ( pad-- > 0 ) ct_put(b, ( db->f[f].type == MC_T_NUM ) ? " " : "　");
		}
		put_text(b, v, vn, esc);
	}
	mc_free(v);
	return i;
}

EXPORT void mc_subst( CTBUF *b, CONST UB *s, INT n, CONST MCDB *db, CONST MCCARD *c, INT all_i, INT tgt_i,
		      INT ntgt, CONST MCMINFO *mi, INT page, BOOL esc )
{
	INT	i = 0, from = 0;

	while ( i < n ) {
		if ( esc && s[i] == '<' ) {
			i = tag_end(s, n, i);
			continue;
		}
		if ( s[i] == '%' ) {
			INT	k;

			ct_putn(b, s + from, i - from);
			k = one_subst(b, s + i, n - i, db, c, all_i, tgt_i, ntgt, mi, page, esc);
			if ( k <= 0 ) {
				ct_putn(b, s + i, 1);
				k = 1;
			}
			i += k;
			from = i;
			continue;
		}
		i++;
	}
	ct_putn(b, s + from, n - from);
}

/* ---------------------------------------------------------------- moving a figure's elements */

LOCAL INT axis_of( CONST UB *name, INT n )
{
	static CONST char *CONST xs[] = { "left", "right", "vobjleft", "vobjright", "viewleft", "viewright",
					  "drawleft", "drawright", "x", "x1", "x2", "cx", NULL };
	static CONST char *CONST ys[] = { "top", "bottom", "vobjtop", "vobjbottom", "viewtop", "viewbottom",
					  "drawtop", "drawbottom", "y", "y1", "y2", "cy", NULL };
	INT	k;

	for ( k = 0; xs[k] != NULL; k++ ) {
		if ( mc_len((CONST UB *)xs[k]) == n && at_is(name, n, xs[k]) ) return 1;
	}
	for ( k = 0; ys[k] != NULL; k++ ) {
		if ( mc_len((CONST UB *)ys[k]) == n && at_is(name, n, ys[k]) ) return 2;
	}
	if ( n == 6 && at_is(name, n, "points") ) return 3;
	return 0;
}

/* A number at s, written moved by d; the bytes it took (0: not a number) */
LOCAL INT moved_num( CTBUF *b, CONST UB *s, INT n, INT d )
{
	INT	i = 0, v = 0;
	BOOL	neg = FALSE;

	if ( i < n && s[i] == '-' ) {
		neg = TRUE;
		i++;
	}
	if ( i >= n || s[i] < '0' || s[i] > '9' ) return 0;
	while ( i < n && s[i] >= '0' && s[i] <= '9' ) v = v * 10 + ( s[i++] - '0' );
	if ( i < n && s[i] == '.' ) return 0;		/* a fraction is left as it is */
	ct_num(b, ( neg ? -v : v ) + d);
	return i;
}

EXPORT void mc_shift( CTBUF *b, CONST UB *s, INT n, INT dx, INT dy )
{
	INT	i = 0, from = 0;

	while ( i < n ) {
		INT	e, j;

		if ( s[i] != '<' ) {
			i++;
			continue;
		}
		e = tag_end(s, n, i);
		for ( j = i + 1; j < e; ) {
			INT	a, ax, q;

			/* an attribute: its name, = and its quoted value */
			while ( j < e && ( s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r' ) ) j++;
			a = j;
			while ( j < e && s[j] != '=' && s[j] != ' ' && s[j] != '>' && s[j] != '/' ) j++;
			if ( j >= e || s[j] != '=' || j + 1 >= e || s[j + 1] != '"' ) {
				j++;
				continue;
			}
			ax = axis_of(s + a, j - a);
			q = j + 2;
			if ( ax == 1 || ax == 2 ) {
				INT	k;

				ct_putn(b, s + from, q - from);
				k = moved_num(b, s + q, e - q, ( ax == 1 ) ? dx : dy);
				from = q + k;
			} else if ( ax == 3 ) {
				/* "x,y x,y ...": every pair moved */
				INT	p = q, which = 0;

				ct_putn(b, s + from, q - from);
				from = q;
				while ( p < e && s[p] != '"' ) {
					INT	k = moved_num(b, s + p, e - p, ( which == 0 ) ? dx : dy);

					if ( k > 0 ) {
						p += k;
						which ^= 1;
					} else {
						ct_putn(b, s + p, 1);
						p++;
					}
				}
				from = p;
			}
			while ( q < e && s[q] != '"' ) q++;
			j = q + 1;
		}
		i = e;
	}
	ct_putn(b, s + from, n - from);
}

/* ---------------------------------------------------------------- the pages */

/* The page's ヘッダ or フッタ, as a paragraph */
LOCAL void head_foot( CTBUF *b, CONST UB *s, CONST MCDB *db, INT ntgt, CONST MCMINFO *mi, INT page )
{
	if ( s[0] == 0 ) return;
	ct_put(b, "<p>");
	mc_subst(b, s, mc_len(s), db, NULL, 0, 0, ntgt, mi, page, FALSE);
	ct_put(b, "</p>\n");
}

/* The list on paper: the names, then a line a card */
LOCAL ER list_pages( CONST MCDB *db, CONST MCPRINT *p, CONST INT *idx, INT n, CONST MCMINFO *mi,
		     CONST MCSINK *sink, UB *work, INT wmax )
{
	INT	per = per_page(p, FALSE, TRUE), page, w, h, m[4], k, at = 0;
	UB	*v = (UB *)mc_alloc(MC_VALUE_MAX + 8);
	CTBUF	b;

	if ( v == NULL ) return E_NOMEM;
	mc_paper(p, &w, &h, m);
	sink->start(sink->arg, MC_PG_DOC, w, h);
	for ( page = 1; at < n; page++ ) {
		INT	i;

		ct_buf(&b, work, wmax);
		head_foot(&b, p->head, db, n, mi, page);
		ct_put(&b, "<p>");
		for ( k = 0; k < db->nf; k++ ) {
			if ( db->f[k].width == 0 ) continue;
			ct_put(&b, "<tab/>");
			ct_esc(&b, db->f[k].name, mc_len(db->f[k].name));
		}
		ct_put(&b, "</p>\n");
		for ( i = 0; i < per && at < n; i++, at++ ) {
			CONST MCCARD	*c = db->c[idx[at]];

			ct_put(&b, "<p>");
			put_num(&b, at + 1, 0);
			for ( k = 0; k < db->nf; k++ ) {
				INT	vn;

				if ( db->f[k].width == 0 ) continue;
				vn = mc_shown(db, c, k, v, MC_VALUE_MAX);
				/* as much as the list shows of it */
				vn = mc_cut(v, vn, db->f[k].width * ( ( db->f[k].type == MC_T_NUM ) ? 1 : 1 ));
				ct_put(&b, "<tab/>");
				ct_esc(&b, v, vn);
			}
			ct_put(&b, "</p>\n");
		}
		head_foot(&b, p->foot, db, n, mi, page);
		if ( b.fail ) {
			mc_free(v);
			return E_LIMIT;
		}
		sink->page(sink->arg, b.p, b.n);
	}
	mc_free(v);
	sink->end(sink->arg);
	return E_OK;
}

/* The elements of a figure between <figure> and </figure> */
LOCAL BOOL inner( CONST UB *x, INT n, CONST char *open, CONST char *close, INT *p_a, INT *p_e )
{
	INT	a = find(x, n, 0, open), e;

	if ( a < 0 ) return FALSE;
	a = tag_end(x, n, a);
	e = find(x, n, a, close);
	if ( e < 0 ) e = n;
	*p_a = a;
	*p_e = e;
	return TRUE;
}

/* The end of the element at x[a] ('<'), its end tag with it */
LOCAL INT elem_end( CONST UB *x, INT n, INT a )
{
	INT	e = tag_end(x, n, a), depth = 1;

	if ( e >= 2 && x[e - 2] == '/' ) return e;
	while ( e < n && depth > 0 ) {
		if ( x[e] == '<' ) {
			INT	t = tag_end(x, n, e);

			if ( e + 1 < n && x[e + 1] == '/' ) depth--;
			else if ( !( t >= 2 && x[t - 2] == '/' ) && !( e + 1 < n && ( x[e + 1] == '?' || x[e + 1] == '!' ) ) ) depth++;
			e = t;
			continue;
		}
		e++;
	}
	return e;
}

LOCAL ER fig_pages( CONST MCDB *db, CONST MCPRINT *p, CONST UB *x, INT xn, CONST INT *idx, INT n,
		    CONST MCMINFO *mi, CONST MCSINK *sink, UB *work, INT wmax )
{
	INT	a, e, w, h, m[4], per = per_page(p, TRUE, FALSE), at = 0, page;
	INT	across = ( p->across > 0 ) ? p->across : 1;
	INT	down = ( p->down > 0 ) ? p->down : 1;
	INT	gx, gy;
	CTBUF	b;

	if ( !inner(x, xn, "<figure", "</figure>", &a, &e) ) return E_PAR;
	mc_paper(p, &w, &h, m);
	/* how far apart the cards are: as set, or the room shared out */
	gx = ( p->gapx > 0 ) ? p->gapx * 96 / 254 : ( w - m[2] - m[3] ) / across;
	gy = ( p->gapy > 0 ) ? p->gapy * 96 / 254 : ( h - m[0] - m[1] ) / down;
	sink->start(sink->arg, MC_PG_FIG, w, h);
	for ( page = 1; at < n; page++ ) {
		INT	i, j;

		ct_buf(&b, work, wmax);
		/* the page's own elements, once */
		for ( j = a; j < e; ) {
			INT	k;

			if ( x[j] != '<' ) {
				j++;
				continue;
			}
			k = elem_end(x, e, j);
			if ( ct_attr_bool(x + j, tag_end(x, e, j) - j, "background", FALSE) ) {
				UB	*t = (UB *)mc_alloc(( k - j ) * 4 + 1024);
				CTBUF	s;

				if ( t != NULL ) {
					ct_buf(&s, t, ( k - j ) * 4 + 1024);
					mc_subst(&s, x + j, k - j, db, NULL, 0, 0, n, mi, page, TRUE);
					mc_shift(&b, s.p, s.n, m[2], m[0]);
					mc_free(t);
				}
				ct_put(&b, "\n");
			}
			j = k;
		}
		for ( i = 0; i < per && at < n; i++, at++ ) {
			INT	col = p->downfirst ? i / down : i % across;
			INT	row = p->downfirst ? i % down : i / across;
			INT	dx = m[2] + col * gx, dy = m[0] + row * gy;

			for ( j = a; j < e; ) {
				INT	k;

				if ( x[j] != '<' ) {
					j++;
					continue;
				}
				k = elem_end(x, e, j);
				if ( !ct_attr_bool(x + j, tag_end(x, e, j) - j, "background", FALSE) ) {
					UB	*t = (UB *)mc_alloc(( k - j ) * 4 + 4096);
					CTBUF	s;

					if ( t != NULL ) {
						ct_buf(&s, t, ( k - j ) * 4 + 4096);
						mc_subst(&s, x + j, k - j, db, db->c[idx[at]], idx[at], at, n, mi, page, TRUE);
						mc_shift(&b, s.p, s.n, dx, dy);
						mc_free(t);
					}
					ct_put(&b, "\n");
				}
				j = k;
			}
		}
		if ( b.fail ) return E_LIMIT;
		sink->page(sink->arg, b.p, b.n);
	}
	sink->end(sink->arg);
	return E_OK;
}

LOCAL ER doc_pages( CONST MCDB *db, CONST MCPRINT *p, CONST UB *x, INT xn, CONST INT *idx, INT n,
		    CONST MCMINFO *mi, CONST MCSINK *sink, UB *work, INT wmax )
{
	INT	a, e, w, h, m[4], at;
	CTBUF	b;

	if ( !inner(x, xn, "<document", "</document>", &a, &e) ) return E_PAR;
	mc_paper(p, &w, &h, m);
	sink->start(sink->arg, MC_PG_DOC, w, h);
	for ( at = 0; at < n; at++ ) {
		ct_buf(&b, work, wmax);
		head_foot(&b, p->head, db, n, mi, at + 1);
		mc_subst(&b, x + a, e - a, db, db->c[idx[at]], idx[at], at, n, mi, at + 1, TRUE);
		head_foot(&b, p->foot, db, n, mi, at + 1);
		if ( b.fail ) return E_LIMIT;
		sink->page(sink->arg, b.p, b.n);
	}
	sink->end(sink->arg);
	return E_OK;
}

EXPORT ER mc_merge( CONST MCDB *db, CONST MCPRINT *p, CONST UB *x, INT xn, CONST INT *idx, INT n,
		    CONST MCMINFO *mi, CONST MCSINK *sink, UB *work, INT wmax )
{
	BOOL	fig = (BOOL)( x != NULL && find(x, xn, 0, "<figure") >= 0 );
	BOOL	list = (BOOL)( x == NULL );

	if ( mc_pages(db, p, fig, list, n) > PAGE_MAX ) return E_LIMIT;
	if ( n <= 0 ) return E_NOEXS;
	if ( list ) return list_pages(db, p, idx, n, mi, sink, work, wmax);
	if ( fig ) return fig_pages(db, p, x, xn, idx, n, mi, sink, work, wmax);
	return doc_pages(db, p, x, xn, idx, n, mi, sink, work, wmax);
}
