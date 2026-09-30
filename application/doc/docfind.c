/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docfind.c
 *	Regular expressions, for 検索/置換 in a text
 *
 *	A pattern is read into a graph of nodes, each followed by the next
 *	of its sequence; a group or a choice holds the sequences it chooses
 *	between, each ending in nothing. Matching walks the graph from a
 *	place in the text and backs up when a way fails. What is to be done
 *	once a sequence ends -- close a group, count one more of a repeat,
 *	go on after a choice -- is carried in frames on the stack, so the
 *	walk needs no state beyond the places of the groups.
 *
 *	A repeat of one letter -- .* \d+ [a-z]? -- is matched in a loop,
 *	taking as many as it can and giving them back one at a time. Other
 *	repeats and groups go one step deeper on the stack for each time
 *	they match; past RX_DEPTH steps the match fails rather than run out
 *	of stack.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/rx.h>

#define RX_MAX_NODE	256		/* nodes of one pattern */
#define RX_MAX_RANGE	128		/* ranges of all its [...] */
#define RX_DEPTH	400		/* steps of one match that go deeper */
#define RX_INF		0x7FFFFFFF

/* node kinds */
#define N_CHAR		1		/* a: the letter */
#define N_ANY		2
#define N_CLASS		3		/* a: first range, b: how many; neg */
#define N_BOL		4
#define N_EOL		5
#define N_GROUP		6		/* a: its number, 0 for none; sub: the choices */
#define N_REPEAT	7		/* sub: what; min, max; lazy */

typedef struct rxn {
	UB		kind;
	BOOL		neg, lazy;
	INT		a, b;
	INT		min, max;
	struct rxn	*sub;		/* N_GROUP: the first choice; N_REPEAT: what */
	struct rxn	*alt;		/* the next choice of a group */
	struct rxn	*next;		/* the next of its sequence */
} RXN;

struct t_rx {
	RXN	node[RX_MAX_NODE];
	INT	nnode;
	INT	range[RX_MAX_RANGE][2];
	INT	nrange;
	INT	ngroup;
	RXN	*start;
};

/* ---------------------------------------------------------------- letters */

/* The letter at s[i] and how many bytes it takes */
LOCAL INT letter( CONST UB *s, INT n, INT i, INT *p_len )
{
	UB	c = s[i];
	INT	v, k, len;

	if ( c < 0x80 ) {
		*p_len = 1;
		return c;
	}
	len = ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 1;
	v = ( len == 4 ) ? c & 0x07 : ( len == 3 ) ? c & 0x0F : ( len == 2 ) ? c & 0x1F : c;
	for ( k = 1; k < len && i + k < n; k++ ) {
		v = ( v << 6 ) | ( s[i + k] & 0x3F );
	}
	*p_len = ( i + len <= n ) ? len : n - i;

	return v;
}

LOCAL BOOL is_digit( INT c )
{
	return (BOOL)( c >= '0' && c <= '9' );
}

/* ---------------------------------------------------------------- reading */

typedef struct {
	T_RX		*rx;
	CONST UB	*p;
	INT		n, i;
	ER		er;
} READ;

LOCAL RXN *node_new( READ *r, UB kind )
{
	RXN	*nd;

	if ( r->rx->nnode >= RX_MAX_NODE ) {
		r->er = E_LIMIT;
		return NULL;
	}
	nd = &r->rx->node[r->rx->nnode++];
	knl_memset(nd, 0, sizeof(*nd));
	nd->kind = kind;

	return nd;
}

LOCAL BOOL range_add( READ *r, INT lo, INT hi )
{
	if ( r->rx->nrange >= RX_MAX_RANGE ) {
		r->er = E_LIMIT;
		return FALSE;
	}
	r->rx->range[r->rx->nrange][0] = lo;
	r->rx->range[r->rx->nrange][1] = hi;
	r->rx->nrange++;

	return TRUE;
}

/* The ranges a class escape stands for: \d \w \s */
LOCAL void escape_ranges( READ *r, INT c )
{
	switch ( c ) {
	case 'd':
		(void)range_add(r, '0', '9');
		break;
	case 'w':
		(void)range_add(r, '0', '9');
		(void)range_add(r, 'a', 'z');
		(void)range_add(r, 'A', 'Z');
		(void)range_add(r, '_', '_');
		break;
	default:
		(void)range_add(r, ' ', ' ');
		(void)range_add(r, '\t', '\r');
		(void)range_add(r, 0x3000, 0x3000);
		break;
	}
}

/* The letter an escape stands for, when it stands for one */
LOCAL INT escaped( INT c )
{
	return ( c == 't' ) ? '\t' : ( c == 'n' ) ? '\n' : ( c == 'r' ) ? '\r'
	     : ( c == 'f' ) ? '\f' : ( c == 'v' ) ? '\v' : c;
}

LOCAL INT peek( READ *r )
{
	INT	len;

	return ( r->i < r->n ) ? letter(r->p, r->n, r->i, &len) : -1;
}

LOCAL INT take( READ *r )
{
	INT	len, c;

	if ( r->i >= r->n ) {
		return -1;
	}
	c = letter(r->p, r->n, r->i, &len);
	r->i += len;

	return c;
}

/* [...]: the ranges in it */
LOCAL RXN *read_class( READ *r )
{
	RXN	*nd = node_new(r, N_CLASS);
	INT	c;
	BOOL	first = TRUE;

	if ( nd == NULL ) {
		return NULL;
	}
	nd->a = r->rx->nrange;
	if ( peek(r) == '^' ) {
		(void)take(r);
		nd->neg = TRUE;
	}
	for ( ;; ) {
		INT	lo;

		c = take(r);
		if ( c < 0 ) {
			r->er = E_PAR;
			return NULL;
		}
		if ( c == ']' && !first ) {
			break;
		}
		first = FALSE;
		if ( c == '\\' ) {
			c = take(r);
			if ( c == 'd' || c == 'w' || c == 's' ) {
				escape_ranges(r, c);
				continue;
			}
			c = escaped(c);
		}
		lo = c;
		if ( peek(r) == '-' && r->i + 1 < r->n && r->p[r->i + 1] != ']' ) {
			(void)take(r);
			c = take(r);
			if ( c == '\\' ) {
				c = escaped(take(r));
			}
			if ( c < lo ) {
				r->er = E_PAR;
				return NULL;
			}
		}
		if ( !range_add(r, lo, c) ) {
			return NULL;
		}
	}
	nd->b = r->rx->nrange - nd->a;

	return nd;
}

LOCAL RXN *read_choice( READ *r );

/* One thing to match, without what may follow it */
LOCAL RXN *read_atom( READ *r )
{
	INT	c = take(r);
	RXN	*nd;

	switch ( c ) {
	case '(': {
		INT	num = 0;

		if ( peek(r) == '?' ) {
			(void)take(r);
			if ( take(r) != ':' ) {
				r->er = E_PAR;
				return NULL;
			}
		} else {
			num = ++r->rx->ngroup;
		}
		nd = read_choice(r);
		if ( nd == NULL || take(r) != ')' ) {
			r->er = E_PAR;
			return NULL;
		}
		nd->a = num;
		return nd;
	}
	case '[':
		return read_class(r);
	case '.':
		return node_new(r, N_ANY);
	case '^':
		return node_new(r, N_BOL);
	case '$':
		return node_new(r, N_EOL);
	case '\\':
		c = take(r);
		if ( c < 0 ) {
			r->er = E_PAR;
			return NULL;
		}
		if ( c == 'd' || c == 'w' || c == 's' || c == 'D' || c == 'W' || c == 'S' ) {
			nd = node_new(r, N_CLASS);
			if ( nd != NULL ) {
				nd->a = r->rx->nrange;
				escape_ranges(r, c | 0x20);
				nd->b = r->rx->nrange - nd->a;
				nd->neg = (BOOL)( c < 'a' );
			}
			return nd;
		}
		nd = node_new(r, N_CHAR);
		if ( nd != NULL ) {
			nd->a = escaped(c);
		}
		return nd;
	case '*': case '+': case '?': case ')': case '|': case -1:
		r->er = E_PAR;
		return NULL;
	default:
		nd = node_new(r, N_CHAR);
		if ( nd != NULL ) {
			nd->a = c;
		}
		return nd;
	}
}

/* A number of a {n,m} */
LOCAL INT read_num( READ *r )
{
	INT	v = -1;

	while ( is_digit(peek(r)) ) {
		v = ( ( v < 0 ) ? 0 : v * 10 ) + ( take(r) - '0' );
		if ( v > 10000 ) {
			v = 10000;
		}
	}

	return v;
}

/* A thing and how many of it */
LOCAL RXN *read_piece( READ *r )
{
	RXN	*atom = read_atom(r), *rep;
	INT	c = peek(r), min, max;

	if ( atom == NULL ) {
		return NULL;
	}
	switch ( c ) {
	case '*':	min = 0;  max = RX_INF;	(void)take(r);	break;
	case '+':	min = 1;  max = RX_INF;	(void)take(r);	break;
	case '?':	min = 0;  max = 1;	(void)take(r);	break;
	case '{': {
		INT	keep = r->i;

		(void)take(r);
		min = read_num(r);
		max = min;
		if ( peek(r) == ',' ) {
			(void)take(r);
			max = read_num(r);
			if ( max < 0 ) {
				max = RX_INF;
			}
		}
		if ( min < 0 || take(r) != '}' || max < min ) {
			/* not a count after all: a brace, as it is */
			r->i = keep;
			return atom;
		}
		break;
	}
	default:
		return atom;
	}
	if ( atom->kind == N_BOL || atom->kind == N_EOL ) {
		r->er = E_PAR;
		return NULL;
	}
	rep = node_new(r, N_REPEAT);
	if ( rep == NULL ) {
		return NULL;
	}
	rep->sub = atom;
	rep->min = min;
	rep->max = max;
	if ( peek(r) == '?' ) {
		(void)take(r);
		rep->lazy = TRUE;
	}

	return rep;
}

/* Things one after another, up to a '|', a ')' or the end */
LOCAL RXN *read_seq( READ *r, BOOL *p_empty )
{
	RXN	*head = NULL, *tail = NULL, *nd;
	INT	c;

	*p_empty = TRUE;
	while ( ( c = peek(r) ) >= 0 && c != '|' && c != ')' ) {
		nd = read_piece(r);
		if ( nd == NULL ) {
			return NULL;
		}
		if ( tail != NULL ) {
			tail->next = nd;
		} else {
			head = nd;
		}
		tail = nd;
		*p_empty = FALSE;
	}

	return head;
}

/* Sequences to choose between, as one group (numbered by the caller) */
LOCAL RXN *read_choice( READ *r )
{
	RXN	*g = node_new(r, N_GROUP), *last = NULL, *seq;
	BOOL	empty;

	if ( g == NULL ) {
		return NULL;
	}
	for ( ;; ) {
		seq = read_seq(r, &empty);
		if ( seq == NULL && !empty ) {
			return NULL;
		}
		/* an empty choice is a group of nothing: it holds at once */
		if ( seq == NULL && ( seq = node_new(r, N_GROUP) ) == NULL ) {
			return NULL;
		}
		if ( last == NULL ) {
			g->sub = seq;
		} else {
			last->alt = seq;
		}
		last = seq;
		if ( peek(r) != '|' ) {
			break;
		}
		(void)take(r);
	}

	return g;
}

EXPORT ER rx_compile( CONST UB *pat, T_RX **p_rx )
{
	READ	r;
	T_RX	*rx;

	if ( pat == NULL || p_rx == NULL ) {
		return E_PAR;
	}
	*p_rx = NULL;
	rx = (T_RX *)Kmalloc(sizeof(T_RX));
	if ( rx == NULL ) {
		return E_NOMEM;
	}
	knl_memset(rx, 0, sizeof(*rx));
	r.rx = rx;
	r.p = pat;
	r.n = 0;
	while ( pat[r.n] != 0 ) {
		r.n++;
	}
	r.i = 0;
	r.er = E_OK;
	rx->start = read_choice(&r);
	if ( r.er == E_OK && ( rx->start == NULL || r.i < r.n ) ) {
		r.er = E_PAR;			/* a ')' with no '(' */
	}
	if ( r.er < E_OK ) {
		Kfree(rx);
		return r.er;
	}
	*p_rx = rx;

	return E_OK;
}

EXPORT void rx_free( T_RX *rx )
{
	if ( rx != NULL ) {
		Kfree(rx);
	}
}

/* ---------------------------------------------------------------- matching */

#define F_SEQ		1		/* go on at n */
#define F_GROUP		2		/* close group a, then go on at n */
#define F_REPEAT	3		/* one more of repeat n was matched */

typedef struct frame {
	INT			kind;
	CONST RXN		*n;
	INT			a;		/* F_GROUP: its number; F_REPEAT: the count */
	INT			at;		/* F_REPEAT: where that one began */
	CONST struct frame	*up;
} FRAME;

typedef struct {
	CONST T_RX	*rx;
	CONST UB	*s;
	INT		n;
	INT		g[RX_GROUPS * 2];
	INT		depth;
} MATCH;

LOCAL INT walk( MATCH *m, CONST RXN *nd, INT pos, CONST FRAME *k );

LOCAL BOOL in_class( CONST T_RX *rx, CONST RXN *nd, INT c )
{
	INT	i;
	BOOL	in = FALSE;

	for ( i = nd->a; i < nd->a + nd->b; i++ ) {
		if ( c >= rx->range[i][0] && c <= rx->range[i][1] ) {
			in = TRUE;
			break;
		}
	}

	return (BOOL)( in != nd->neg );
}

/* Whether the letter at pos is one a letter node takes: its length */
LOCAL BOOL one_letter( CONST MATCH *m, CONST RXN *nd, INT pos, INT *p_len )
{
	INT	c;

	if ( pos >= m->n ) {
		return FALSE;
	}
	c = letter(m->s, m->n, pos, p_len);

	return (BOOL)( ( nd->kind == N_CHAR && c == nd->a )
		    || ( nd->kind == N_ANY && c != '\n' )
		    || ( nd->kind == N_CLASS && in_class(m->rx, nd, c) ) );
}

LOCAL BOOL single( CONST RXN *nd )
{
	return (BOOL)( nd->next == NULL && ( nd->kind == N_CHAR
		    || nd->kind == N_ANY || nd->kind == N_CLASS ) );
}

/*
 * A repeat of one letter, without going deeper for each: as many as
 * there are given back one by one, or, for as few as will do, one
 * more taken at a time.
 */
LOCAL INT repeat_letters( MATCH *m, CONST RXN *nd, INT pos, CONST FRAME *k )
{
	INT	cnt = 0, p = pos, e, len;

	if ( !nd->lazy ) {
		while ( cnt < nd->max && one_letter(m, nd->sub, p, &len) ) {
			p += len;
			cnt++;
		}
		for ( ;; ) {
			if ( cnt < nd->min ) {
				return -1;
			}
			if ( ( e = walk(m, nd->next, p, k) ) >= 0 ) {
				return e;
			}
			if ( cnt == 0 ) {
				return -1;
			}
			/* one letter back: past the bytes that go on a letter */
			p--;
			while ( p > pos && ( m->s[p] & 0xC0U ) == 0x80U ) {
				p--;
			}
			cnt--;
		}
	}
	for ( ;; ) {
		if ( cnt >= nd->min && ( e = walk(m, nd->next, p, k) ) >= 0 ) {
			return e;
		}
		if ( cnt >= nd->max || !one_letter(m, nd->sub, p, &len) ) {
			return -1;
		}
		p += len;
		cnt++;
	}
}

/* More of a repeat, or what comes after it: the count so far is c */
LOCAL INT repeat( MATCH *m, CONST RXN *nd, INT pos, INT c, INT last,
		  CONST FRAME *k )
{
	FRAME	f;
	INT	e;
	BOOL	more = (BOOL)( c < nd->max && pos != last );

	if ( c == 0 && single(nd->sub) ) {
		return repeat_letters(m, nd, pos, k);
	}
	f.kind = F_REPEAT;
	f.n = nd;
	f.a = c + 1;
	f.at = pos;
	f.up = k;
	if ( nd->lazy ) {
		if ( c >= nd->min && ( e = walk(m, nd->next, pos, k) ) >= 0 ) {
			return e;
		}
		return more ? walk(m, nd->sub, pos, &f) : -1;
	}
	if ( more && ( e = walk(m, nd->sub, pos, &f) ) >= 0 ) {
		return e;
	}

	return ( c >= nd->min ) ? walk(m, nd->next, pos, k) : -1;
}

/*
 * The graph from 'nd' matched at 'pos', and then what the frames say:
 * where the whole match ends, or -1.
 */
LOCAL INT walk( MATCH *m, CONST RXN *nd, INT pos, CONST FRAME *k )
{
	INT	e, len;

	if ( ++m->depth > RX_DEPTH ) {
		m->depth--;
		return -1;
	}
	for ( ;; ) {
		if ( nd == NULL ) {
			if ( k == NULL ) {
				e = pos;
				break;
			}
			if ( k->kind == F_SEQ ) {
				nd = k->n;
				k = k->up;
				continue;
			}
			if ( k->kind == F_GROUP ) {
				INT	was = m->g[k->a * 2 + 1];

				m->g[k->a * 2 + 1] = pos;
				e = walk(m, k->n, pos, k->up);
				if ( e < 0 ) {
					m->g[k->a * 2 + 1] = was;
				}
				break;
			}
			e = repeat(m, k->n, pos, k->a, k->at, k->up);
			break;
		}
		switch ( nd->kind ) {
		case N_CHAR:
		case N_ANY:
		case N_CLASS:
			if ( !one_letter(m, nd, pos, &len) ) {
				e = -1;
				goto done;
			}
			pos += len;
			nd = nd->next;
			continue;
		case N_BOL:
			if ( pos != 0 ) {
				e = -1;
				goto done;
			}
			nd = nd->next;
			continue;
		case N_EOL:
			if ( pos != m->n ) {
				e = -1;
				goto done;
			}
			nd = nd->next;
			continue;
		case N_GROUP: {
			CONST RXN	*a;
			FRAME		f;
			INT		ws = 0, we = 0;

			f.kind = ( nd->a > 0 ) ? F_GROUP : F_SEQ;
			f.n = nd->next;
			f.a = nd->a;
			f.at = 0;
			f.up = k;
			if ( nd->a > 0 && nd->a < RX_GROUPS ) {
				ws = m->g[nd->a * 2];
				we = m->g[nd->a * 2 + 1];
				m->g[nd->a * 2] = pos;
			} else if ( nd->a >= RX_GROUPS ) {
				f.kind = F_SEQ;		/* past what is kept */
			}
			e = -1;
			for ( a = nd->sub; a != NULL && e < 0; a = a->alt ) {
				/* a choice's own sequence, with no choices after it */
				e = ( a->kind == N_GROUP && a->sub == NULL )
				    ? walk(m, NULL, pos, &f) : walk(m, a, pos, &f);
			}
			if ( e < 0 && nd->a > 0 && nd->a < RX_GROUPS ) {
				m->g[nd->a * 2] = ws;
				m->g[nd->a * 2 + 1] = we;
			}
			goto done;
		}
		case N_REPEAT:
			e = repeat(m, nd, pos, 0, -1, k);
			goto done;
		default:
			e = -1;
			goto done;
		}
	}
done:
	m->depth--;

	return e;
}

EXPORT BOOL rx_find( CONST T_RX *rx, CONST UB *s, INT n, INT from,
		     INT *p_start, INT *p_end, INT *groups )
{
	MATCH	m;
	INT	pos, i, e, len;

	if ( rx == NULL || s == NULL || from < 0 ) {
		return FALSE;
	}
	m.rx = rx;
	m.s = s;
	m.n = n;
	for ( pos = from; pos <= n; pos += len ) {
		for ( i = 0; i < RX_GROUPS * 2; i++ ) {
			m.g[i] = -1;
		}
		m.depth = 0;
		e = walk(&m, rx->start, pos, NULL);
		if ( e >= 0 ) {
			if ( p_start != NULL ) *p_start = pos;
			if ( p_end != NULL )   *p_end = e;
			if ( groups != NULL ) {
				m.g[0] = pos;
				m.g[1] = e;
				for ( i = 0; i < RX_GROUPS * 2; i++ ) {
					groups[i] = m.g[i];
				}
			}
			return TRUE;
		}
		len = 1;
		if ( pos < n ) {
			(void)letter(s, n, pos, &len);
		}
	}

	return FALSE;
}

EXPORT INT rx_expand( CONST UB *with, CONST UB *s, CONST INT *groups,
		      UB *out, INT max )
{
	INT	i = 0, k = 0;

	if ( out == NULL || max <= 0 ) {
		return 0;
	}
	while ( with != NULL && with[i] != 0 && k < max - 1 ) {
		INT	g = -1;

		if ( with[i] == '$' && with[i + 1] == '$' ) {
			out[k++] = '$';
			i += 2;
			continue;
		}
		if ( with[i] == '$' && with[i + 1] == '&' ) {
			g = 0;
		} else if ( with[i] == '$' && with[i + 1] >= '1' && with[i + 1] <= '9' ) {
			g = with[i + 1] - '0';
		}
		if ( g < 0 ) {
			out[k++] = with[i++];
			continue;
		}
		i += 2;
		if ( groups != NULL && s != NULL && groups[g * 2] >= 0 ) {
			INT	j;

			for ( j = groups[g * 2]; j < groups[g * 2 + 1] && k < max - 1; j++ ) {
				out[k++] = s[j];
			}
		}
	}
	out[k] = 0;

	return k;
}
