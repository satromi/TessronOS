/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_ceval.c
 *	基本表計算: the formulas worked out without V8 (design 11.13)
 *
 *	The engine of a program built without V8. It keeps the cells as the
 *	V8 engine does and turns a formula into an expression the same way
 *	-- a range into the list of its numbers, a cell into its value, a
 *	number as it is and anything else quoted -- and then evaluates what
 *	it knows of that expression: numbers, strings, true and false, the
 *	lists, the operators of arithmetic, comparison and logic, ?:, and
 *	the functions SUM, AVERAGE, COUNT, MAX, MIN, IF, ROUND, FLOOR, CEIL,
 *	ABS, SQRT, POWER, MOD, PI, RAND and RANDBETWEEN. Their values, and
 *	how a value is written as words, are those of the V8 engine; a
 *	result a function cannot give is an error as the functions there
 *	give it ("Error: #NUM!"). Anything else in an expression -- a method,
 *	a name not in the table -- is #ERROR!, and a search is in the words
 *	as they are, a regular expression not understood.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "calc_eng.h"

/* ---------------------------------------------------------------- values */

#define V_UNDEF		0
#define V_NULL		1
#define V_NUM		2
#define V_STR		3
#define V_BOOL		4
#define V_ARR		5
#define V_ERR		6		/* a function's error: s is its words ("#NUM!") */
#define V_FN		7		/* a function of the table: n its index */

typedef struct {
	int	t;
	double	n;
	char	*s;			/* V_STR, V_ERR: owned */
	double	*a;			/* V_ARR: owned */
	int	na;
} CV;

static CV v_num( double n )	{ CV v; memset(&v, 0, sizeof(v)); v.t = V_NUM; v.n = n; return v; }
static CV v_bool( int b )	{ CV v; memset(&v, 0, sizeof(v)); v.t = V_BOOL; v.n = b ? 1 : 0; return v; }
static CV v_undef( void )	{ CV v; memset(&v, 0, sizeof(v)); return v; }

static CV v_strn( const char *s, size_t n )
{
	CV	v;

	memset(&v, 0, sizeof(v));
	v.t = V_STR;
	v.s = malloc(n + 1);
	if ( v.s != NULL ) {
		memcpy(v.s, s, n);
		v.s[n] = 0;
	}
	return v;
}

static CV v_str( const char *s )	{ return v_strn(s, strlen(s)); }

static CV v_err( const char *what )
{
	CV	v = v_str(what);

	v.t = V_ERR;
	return v;
}

static void v_free( CV *v )
{
	free(v->s);
	free(v->a);
	memset(v, 0, sizeof(*v));
}

static CV v_copy( const CV *v )
{
	CV	c = *v;

	if ( v->s != NULL ) {
		c.s = strdup(v->s);
	}
	if ( v->a != NULL ) {
		c.a = malloc(sizeof(double) * (size_t)( v->na > 0 ? v->na : 1 ));
		if ( c.a != NULL ) memcpy(c.a, v->a, sizeof(double) * (size_t)v->na);
	}
	return c;
}

/* ---------------------------------------------------------------- words of a value */

/* A number as the script's language writes it: the fewest digits that read back the same */
static void num_words( double x, char *out, size_t max )
{
	char	buf[40], dig[24];
	int	k, e = 0, nd = 0, i, p;

	if ( isnan(x) ) {
		snprintf(out, max, "NaN");
		return;
	}
	if ( isinf(x) ) {
		snprintf(out, max, x < 0 ? "-Infinity" : "Infinity");
		return;
	}
	if ( x == 0 ) {
		snprintf(out, max, "0");
		return;
	}
	for ( k = 1; k <= 17; k++ ) {
		snprintf(buf, sizeof(buf), "%.*e", k - 1, x);
		if ( strtod(buf, NULL) == x ) break;
	}
	/* "-d.ddde+XX": the digits and the exponent */
	for ( i = ( buf[0] == '-' ) ? 1 : 0; buf[i] != 0 && buf[i] != 'e'; i++ ) {
		if ( buf[i] >= '0' && buf[i] <= '9' && nd < 20 ) dig[nd++] = buf[i];
	}
	dig[nd] = 0;
	if ( buf[i] == 'e' ) e = atoi(buf + i + 1);
	while ( nd > 1 && dig[nd - 1] == '0' ) dig[--nd] = 0;
	p = e + 1;				/* the point after this many digits */
	i = 0;
	if ( x < 0 && max > 1 ) out[i++] = '-';
	if ( nd <= p && p <= 21 ) {
		for ( k = 0; k < nd && (size_t)i < max - 1; k++ ) out[i++] = dig[k];
		for ( k = nd; k < p && (size_t)i < max - 1; k++ ) out[i++] = '0';
	} else if ( 0 < p && p <= 21 ) {
		for ( k = 0; k < nd && (size_t)i < max - 2; k++ ) {
			if ( k == p ) out[i++] = '.';
			out[i++] = dig[k];
		}
	} else if ( -6 < p && p <= 0 ) {
		if ( (size_t)i < max - 2 ) {
			out[i++] = '0';
			out[i++] = '.';
		}
		for ( k = 0; k < -p && (size_t)i < max - 1; k++ ) out[i++] = '0';
		for ( k = 0; k < nd && (size_t)i < max - 1; k++ ) out[i++] = dig[k];
	} else {
		out[i] = 0;
		snprintf(out + i, max - (size_t)i, "%c%s%s%se%c%d", dig[0], nd > 1 ? "." : "", nd > 1 ? dig + 1 : "",
			 "", ( p - 1 >= 0 ) ? '+' : '-', abs(p - 1));
		return;
	}
	out[i] = 0;
}

/* String(v), in memory of the caller's to free */
static char *v_words( const CV *v )
{
	char	b[40];
	size_t	n, at;
	char	*s;
	int	i;

	switch ( v->t ) {
	case V_NUM:	num_words(v->n, b, sizeof(b)); return strdup(b);
	case V_BOOL:	return strdup(v->n != 0 ? "true" : "false");
	case V_STR:	return strdup(v->s != NULL ? v->s : "");
	case V_NULL:	return strdup("null");
	case V_ERR:
		n = strlen(v->s != NULL ? v->s : "") + 8;
		s = malloc(n);
		if ( s != NULL ) snprintf(s, n, "Error: %s", v->s != NULL ? v->s : "");
		return s;
	case V_ARR:
		s = malloc((size_t)v->na * 26 + 1);
		if ( s == NULL ) return NULL;
		at = 0;
		for ( i = 0; i < v->na; i++ ) {
			if ( i > 0 ) s[at++] = ',';
			num_words(v->a[i], s + at, 26);
			at += strlen(s + at);
		}
		s[at] = 0;
		return s;
	case V_FN:	return strdup("function");
	default:	return strdup("undefined");
	}
}

static int white( unsigned char c )
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Number(s) */
static double str_num( const char *s )
{
	const char	*a = s, *e;
	char		*end;
	double		x;
	size_t		n;

	while ( white((unsigned char)*a) ) a++;
	n = strlen(a);
	while ( n > 0 && white((unsigned char)a[n - 1]) ) n--;
	if ( n == 0 ) return 0;
	{
		char	*t = malloc(n + 1);

		if ( t == NULL ) return NAN;
		memcpy(t, a, n);
		t[n] = 0;
		e = t;
		if ( strcmp(t, "Infinity") == 0 || strcmp(t, "+Infinity") == 0 ) x = INFINITY;
		else if ( strcmp(t, "-Infinity") == 0 ) x = -INFINITY;
		else if ( n > 2 && t[0] == '0' && ( t[1] == 'x' || t[1] == 'X' ) ) {
			x = (double)strtoull(t + 2, &end, 16);
			if ( *end != 0 ) x = NAN;
		} else {
			/* only a decimal number: no "nan", "inf" or hexadecimal of strtod's own */
			const char	*p = t;

			if ( *p == '+' || *p == '-' ) p++;
			if ( !( ( *p >= '0' && *p <= '9' ) || ( *p == '.' && p[1] >= '0' && p[1] <= '9' ) ) ) {
				x = NAN;
			} else {
				x = strtod(t, &end);
				if ( *end != 0 ) x = NAN;
			}
		}
		free((void *)e);
	}
	return x;
}

static double v_numval( const CV *v )
{
	switch ( v->t ) {
	case V_NUM:
	case V_BOOL:	return v->n;
	case V_NULL:	return 0;
	case V_STR:	return str_num(v->s != NULL ? v->s : "");
	case V_ARR:
		if ( v->na == 0 ) return 0;
		if ( v->na == 1 ) return v->a[0];
		return NAN;
	default:	return NAN;
	}
}

static int v_true( const CV *v )
{
	switch ( v->t ) {
	case V_NUM:	return !( v->n == 0 || isnan(v->n) );
	case V_BOOL:	return v->n != 0;
	case V_STR:	return v->s != NULL && v->s[0] != 0;
	case V_ARR:
	case V_ERR:
	case V_FN:	return 1;
	default:	return 0;
	}
}

/* ---------------------------------------------------------------- the cells */

/* A range a formula names: from one corner to the other, both in it */
typedef struct {
	int	c0, r0, c1, r1;
} CREF;

typedef struct {
	int	col, row;
	char	*value, *formula;
	CV	shown;			/* displayValue */
	int	pass;			/* the working out it was last worked out in */
	int	dpass;			/* and the one its dependents were last looked for in */
	int	nref;
	CREF	*refs;			/* what its formula names */
} CCELL;

static CCELL	*cells;
static int	ncell, mcell;
static int	*changed;		/* pairs, in the order they first changed */
static int	nchg, mchg;

/*
 * A working out is numbered: a formula worked out once in it keeps what
 * it came to for the rest of it, so that one named many times is worked
 * out once. While many cells are put in at once (ce_bulk), nothing is
 * worked out until they all are.
 */
static int	g_pass = 1;
static int	g_bulk = 0;
static int	deps_stale = 1;		/* who names whom to be made again (deps_build) */

/*
 * Places kept in open tables, at least twice as large as what is in
 * them, so that a place is found at once however many there are: the
 * cells by their place (each slot the cell's index and one), and the
 * places changed since last asked (the index in 'changed' and one).
 */
typedef struct {
	int		*slot;
	unsigned	cap;
} PTAB;

static PTAB	hx;			/* the cells */
static PTAB	hc;			/* the places changed */

static unsigned home_of( const PTAB *t, int col, int row )
{
	unsigned	h = (unsigned)col * 0x9E3779B1U ^ (unsigned)row * 0x85EBCA77U;

	h ^= h >> 15;
	return h & ( t->cap - 1 );
}

static void cell_place( int i, int *c, int *r )		{ *c = cells[i].col; *r = cells[i].row; }
static void chg_place( int i, int *c, int *r )		{ *c = changed[2 * i]; *r = changed[2 * i + 1]; }

/* The slot of a place, or the empty one it would go in */
static unsigned pt_slot( const PTAB *t, void (*at)( int, int *, int * ), int col, int row )
{
	unsigned	k;
	int		c, r;

	for ( k = home_of(t, col, row); t->slot[k] != 0; k = ( k + 1 ) & ( t->cap - 1 ) ) {
		at(t->slot[k] - 1, &c, &r);
		if ( c == col && r == row ) break;
	}
	return k;
}

/* The table made again for n entries, from 'at' */
static void pt_build( PTAB *t, void (*at)( int, int *, int * ), int n )
{
	unsigned	cap = 64, k;
	int		i, c, r;

	while ( cap < (unsigned)n * 2 + 16 ) cap *= 2;
	free(t->slot);
	t->slot = calloc(cap, sizeof(int));
	t->cap = ( t->slot != NULL ) ? cap : 0;
	for ( i = 0; t->slot != NULL && i < n; i++ ) {
		at(i, &c, &r);
		for ( k = home_of(t, c, r); t->slot[k] != 0; k = ( k + 1 ) & ( t->cap - 1 ) ) ;
		t->slot[k] = i + 1;
	}
}

/* An entry taken out, those after it in its run moved back into its place */
static void pt_del( PTAB *t, void (*at)( int, int *, int * ), int col, int row )
{
	unsigned	s, j, h;
	int		c, r;

	if ( t->cap == 0 ) return;
	s = pt_slot(t, at, col, row);
	if ( t->slot[s] == 0 ) return;
	for ( j = ( s + 1 ) & ( t->cap - 1 ); t->slot[j] != 0; j = ( j + 1 ) & ( t->cap - 1 ) ) {
		at(t->slot[j] - 1, &c, &r);
		h = home_of(t, c, r);
		if ( ( j > s && ( h <= s || h > j ) ) || ( j < s && h <= s && h > j ) ) {
			t->slot[s] = t->slot[j];
			s = j;
		}
	}
	t->slot[s] = 0;
}

static int find_cell( int col, int row )
{
	if ( hx.cap == 0 ) return -1;
	return hx.slot[pt_slot(&hx, cell_place, col, row)] - 1;
}

static int make_cell( int col, int row )
{
	int	i = find_cell(col, row);

	if ( i >= 0 ) return i;
	if ( ncell == mcell ) {
		int	m = mcell ? mcell * 2 : 64;
		CCELL	*c = realloc(cells, sizeof(CCELL) * (size_t)m);

		if ( c == NULL ) return -1;
		cells = c;
		mcell = m;
	}
	memset(&cells[ncell], 0, sizeof(CCELL));
	cells[ncell].col = col;
	cells[ncell].row = row;
	cells[ncell].value = strdup("");
	cells[ncell].formula = strdup("");
	cells[ncell].shown = v_str("");
	ncell++;
	if ( (unsigned)ncell * 2 + 16 > hx.cap ) {
		pt_build(&hx, cell_place, ncell);
	} else {
		hx.slot[pt_slot(&hx, cell_place, col, row)] = ncell;
	}
	return ncell - 1;
}

/* A place changed, kept once, in the order it first changed */
static void touch( int col, int row )
{
	if ( hc.cap != 0 && hc.slot[pt_slot(&hc, chg_place, col, row)] != 0 ) {
		return;
	}
	if ( nchg == mchg ) {
		int	m = mchg ? mchg * 2 : 64, *c = realloc(changed, sizeof(int) * 2 * (size_t)m);

		if ( c == NULL ) return;
		changed = c;
		mchg = m;
	}
	changed[2 * nchg] = col;
	changed[2 * nchg + 1] = row;
	nchg++;
	if ( (unsigned)nchg * 2 + 16 > hc.cap ) {
		pt_build(&hc, chg_place, nchg);
	} else {
		hc.slot[pt_slot(&hc, chg_place, col, row)] = nchg;
	}
}

/* ---------------------------------------------------------------- names in a formula */

static int letter_col( const char *s, int n )
{
	long long	col = 0;
	int		i;

	for ( i = 0; i < n; i++ ) {
		col = col * 26 + ( s[i] - 64 );
		if ( col > 0x3FFFFFFF ) return 0x3FFFFFFF;
	}
	return (int)col;
}

static int up( char c )		{ return c >= 'A' && c <= 'Z'; }
static int dig( char c )	{ return c >= '0' && c <= '9'; }
static int word( char c )	{ return up(c) || dig(c) || ( c >= 'a' && c <= 'z' ) || c == '_'; }

/* A growing string */
typedef struct {
	char	*p;
	size_t	n, max;
} SB;

static void sb_putn( SB *b, const char *s, size_t n )
{
	if ( b->n + n + 1 > b->max ) {
		size_t	m = ( b->n + n + 1 ) * 2;
		char	*q = realloc(b->p, m);

		if ( q == NULL ) return;
		b->p = q;
		b->max = m;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void sb_put( SB *b, const char *s )	{ sb_putn(b, s, strlen(s)); }

/* The cells being worked out now: a cell named among them comes round to itself */
typedef struct EVSET {
	int	n;
	int	*k;			/* pairs */
} EVSET;

static int ev_has( const EVSET *e, int col, int row )
{
	int	i;

	for ( i = 0; i < e->n; i++ ) {
		if ( e->k[2 * i] == col && e->k[2 * i + 1] == row ) return 1;
	}
	return 0;
}

static CV evaluate( const char *formula, int col, int row, const EVSET *ev );

/* A cell's value: a formula's worked out again */
static CV value_recalc( int col, int row, const EVSET *ev )
{
	int	i = find_cell(col, row);

	if ( i < 0 ) return v_str("");
	if ( cells[i].formula[0] != 0 && cells[i].pass == g_pass ) {
		return v_copy(&cells[i].shown);		/* worked out already in this one */
	}
	if ( cells[i].formula[0] != 0 ) {
		EVSET	in;
		CV	v;
		char	*f;

		in.n = ev->n + 1;
		in.k = malloc(sizeof(int) * 2 * (size_t)in.n);
		if ( in.k == NULL ) return v_str("");
		memcpy(in.k, ev->k, sizeof(int) * 2 * (size_t)ev->n);
		in.k[2 * ev->n] = col;
		in.k[2 * ev->n + 1] = row;
		f = strdup(cells[i].formula);
		v = evaluate(f, col, row, &in);
		free(f);
		free(in.k);
		i = find_cell(col, row);
		if ( i >= 0 ) {
			v_free(&cells[i].shown);
			cells[i].shown = v_copy(&v);
			cells[i].pass = g_pass;
		}
		return v;
	}
	return v_str(cells[i].value);
}

/* A value put in the expression: a number as it is, anything else quoted */
static void put_value( SB *b, const CV *v )
{
	char	*w = v_words(v);
	double	x = v_numval(v);

	if ( v->t == V_STR ) x = str_num(v->s != NULL ? v->s : "");
	if ( w == NULL ) return;
	if ( isnan(x) ) {
		sb_put(b, "\"");
		sb_put(b, w);
		sb_put(b, "\"");
	} else {
		sb_put(b, w);
	}
	free(w);
}

/* ---------------------------------------------------------------- the expression */

typedef struct {
	const char	*s;
	int		at;
	int		bad;		/* not an expression */
	int		thrown;		/* a name not known, a call of what is not a function */
} PX;

static void skip( PX *p )
{
	while ( white((unsigned char)p->s[p->at]) ) p->at++;
}

static int eat( PX *p, const char *op )
{
	size_t	n = strlen(op);

	skip(p);
	if ( strncmp(p->s + p->at, op, n) != 0 ) return 0;
	p->at += (int)n;
	return 1;
}

static int peek( PX *p, const char *op )
{
	skip(p);
	return strncmp(p->s + p->at, op, strlen(op)) == 0;
}

static CV cond( PX *p, int live );

/* ---------------------------------------------------------------- the functions */

static const char *const fn_name[] = {
	"SUM", "AVERAGE", "COUNT", "MAX", "MIN", "IF", "ROUND", "FLOOR", "CEIL", "ABS", "SQRT", "POWER",
	"MOD", "PI", "RAND", "RANDBETWEEN"
};
#define FN_N	( (int)( sizeof(fn_name) / sizeof(fn_name[0]) ) )
enum { F_SUM, F_AVERAGE, F_COUNT, F_MAX, F_MIN, F_IF, F_ROUND, F_FLOOR, F_CEIL, F_ABS, F_SQRT, F_POWER,
       F_MOD, F_PI, F_RAND, F_RANDBETWEEN };

/* The first error among the arguments, or -1 */
static int first_err( const CV *a, int n )
{
	int	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i].t == V_ERR ) return i;
	}
	return -1;
}

/* An argument as a number, as the functions read one; 0 and an error when it is not one */
static int arg_num( const CV *v, double *x )
{
	switch ( v->t ) {
	case V_UNDEF:
	case V_NULL:	*x = 0; return 1;
	case V_NUM:
	case V_BOOL:	*x = v->n; return 1;
	case V_STR:
		*x = str_num(v->s != NULL ? v->s : "");
		return !isnan(*x);
	case V_ARR:
		*x = ( v->na > 0 ) ? v->a[0] : 0;
		return 1;
	default:	*x = 0; return 0;
	}
}

/* Every number of the arguments, lists opened; strings that are numbers too when 'strings' */
static int numbers( const CV *a, int n, int strings, double *out, int max )
{
	int	i, k, m = 0;

	for ( i = 0; i < n; i++ ) {
		if ( a[i].t == V_NUM && m < max ) out[m++] = a[i].n;
		else if ( a[i].t == V_ARR ) {
			for ( k = 0; k < a[i].na && m < max; k++ ) out[m++] = a[i].a[k];
		} else if ( strings && a[i].t == V_STR ) {
			double	x = str_num(a[i].s != NULL ? a[i].s : "");

			if ( !isnan(x) && a[i].s[0] != 0 && m < max ) out[m++] = x;
		}
	}
	return m;
}

/* ROUND: through the digits as written, as the script's functions do */
static double round_to( double x, double d )
{
	char	w[40], e[40], t[96];
	double	y;

	num_words(x, w, sizeof(w));
	num_words(d, e, sizeof(e));
	snprintf(t, sizeof(t), "%se%s", w, e);
	y = str_num(t);
	if ( isnan(y) ) return NAN;
	y = floor(y + 0.5);
	num_words(y, w, sizeof(w));
	num_words(-d, e, sizeof(e));
	snprintf(t, sizeof(t), "%se%s", w, e);
	return str_num(t);
}

static CV call_fn( int f, CV *a, int n )
{
	double	x, y, *nums;
	int	k, m, e = first_err(a, n);
	CV	r;

	switch ( f ) {
	case F_SUM:
	case F_AVERAGE:
	case F_COUNT:
	case F_MAX:
	case F_MIN:
		if ( e >= 0 ) return v_copy(&a[e]);
		for ( k = 0, m = 0; k < n; k++ ) m += ( a[k].t == V_ARR ) ? a[k].na : 1;
		nums = malloc(sizeof(double) * (size_t)( m + 1 ));
		if ( nums == NULL ) return v_err("#VALUE!");
		m = numbers(a, n, f == F_SUM, nums, m + 1);
		x = 0;
		if ( f == F_SUM || f == F_AVERAGE ) {
			for ( k = 0; k < m; k++ ) x += nums[k];
			if ( f == F_AVERAGE ) {
				if ( m == 0 ) {
					free(nums);
					return v_err("#DIV/0!");
				}
				x /= m;
			}
		} else if ( f == F_COUNT ) {
			x = m;
		} else if ( m > 0 ) {
			x = nums[0];
			for ( k = 1; k < m; k++ ) {
				if ( f == F_MAX ? nums[k] > x : nums[k] < x ) x = nums[k];
			}
		}
		free(nums);
		return v_num(x);
	case F_IF:
		if ( n > 0 && a[0].t == V_ERR ) return v_copy(&a[0]);
		if ( n > 0 && v_true(&a[0]) ) return ( n > 1 ) ? v_copy(&a[1]) : v_bool(1);
		return ( n > 2 ) ? v_copy(&a[2]) : v_bool(0);
	case F_PI:
		return v_num(M_PI);
	case F_RAND:
		return v_num((double)rand() / ( (double)RAND_MAX + 1 ));
	default:
		break;
	}
	if ( e >= 0 ) return v_copy(&a[e]);
	if ( !arg_num(n > 0 ? &a[0] : &(CV){ 0 }, &x) ) return v_err("#VALUE!");
	if ( !arg_num(n > 1 ? &a[1] : &(CV){ 0 }, &y) ) return v_err("#VALUE!");
	switch ( f ) {
	case F_ROUND:
		return v_num(round_to(x, y));
	case F_FLOOR:
		if ( y == 0 ) return v_num(0);
		if ( ( x < 0 ) != ( y < 0 ) && x != 0 ) return v_err("#NUM!");
		if ( x < 0 ) return v_num(-ceil(-x / -y) * -y);
		return v_num(floor(x / y) * y);
	case F_CEIL:
		if ( y == 0 ) return v_num(0);
		y = fabs(y);
		if ( x < 0 ) return v_num(-floor(-x / y) * y);
		return v_num(ceil(x / y) * y);
	case F_ABS:
		return v_num(fabs(x));
	case F_SQRT:
		if ( x < 0 ) return v_err("#NUM!");
		return v_num(sqrt(x));
	case F_POWER:
		r = v_num(pow(x, y));
		if ( isnan(r.n) ) return v_err("#NUM!");
		return r;
	case F_MOD:
		if ( y == 0 ) return v_err("#DIV/0!");
		{
			double	md = fabs(fmod(x, y));

			if ( x < 0 && md != 0 ) md = fabs(y) - md;
			return v_num(y > 0 ? md : -md);
		}
	case F_RANDBETWEEN:
		return v_num(floor((double)rand() / ( (double)RAND_MAX + 1 ) * ( y - x + 1 )) + x);
	default:
		return v_err("#VALUE!");
	}
}

/* ---------------------------------------------------------------- the grammar */

static CV primary( PX *p, int live )
{
	CV	v;

	skip(p);
	if ( p->bad ) return v_undef();
	{
		char	c = p->s[p->at];

		if ( c == '(' ) {
			p->at++;
			v = cond(p, live);
			if ( !eat(p, ")") ) p->bad = 1;
			return v;
		}
		if ( c == '"' || c == '\'' ) {
			SB	b = { NULL, 0, 0 };

			p->at++;
			sb_put(&b, "");
			while ( p->s[p->at] != 0 && p->s[p->at] != c ) {
				char	ch = p->s[p->at++];

				if ( ch == '\\' && p->s[p->at] != 0 ) {
					ch = p->s[p->at++];
					if ( ch == 'n' ) ch = '\n';
					else if ( ch == 't' ) ch = '\t';
				}
				sb_putn(&b, &ch, 1);
			}
			if ( p->s[p->at] != c ) {
				p->bad = 1;
				free(b.p);
				return v_undef();
			}
			p->at++;
			v = v_str(b.p != NULL ? b.p : "");
			free(b.p);
			return v;
		}
		if ( c == '[' ) {
			double	*a = NULL;
			int	na = 0;

			p->at++;
			if ( !eat(p, "]") ) {
				do {
					CV	e = cond(p, live);
					double	*q = realloc(a, sizeof(double) * (size_t)( na + 1 ));

					if ( q != NULL ) {
						a = q;
						a[na++] = v_numval(&e);
					}
					v_free(&e);
				} while ( !p->bad && eat(p, ",") );
				if ( !eat(p, "]") ) p->bad = 1;
			}
			memset(&v, 0, sizeof(v));
			v.t = V_ARR;
			v.a = a;
			v.na = na;
			return v;
		}
		if ( dig(c) || ( c == '.' && dig(p->s[p->at + 1]) ) ) {
			char	*end;
			double	x = strtod(p->s + p->at, &end);

			p->at = (int)( end - p->s );
			if ( word(p->s[p->at]) ) p->bad = 1;
			return v_num(x);
		}
		if ( word(c) || c == '$' ) {
			int	a = p->at, k;
			char	nm[64];

			while ( word(p->s[p->at]) || p->s[p->at] == '$' ) p->at++;
			snprintf(nm, sizeof(nm), "%.*s", p->at - a, p->s + a);
			if ( strcmp(nm, "true") == 0 ) return v_bool(1);
			if ( strcmp(nm, "false") == 0 ) return v_bool(0);
			if ( strcmp(nm, "null") == 0 ) {
				v = v_undef();
				v.t = V_NULL;
				return v;
			}
			if ( strcmp(nm, "undefined") == 0 ) return v_undef();
			if ( strcmp(nm, "NaN") == 0 ) return v_num(NAN);
			if ( strcmp(nm, "Infinity") == 0 ) return v_num(INFINITY);
			for ( k = 0; k < FN_N; k++ ) {
				if ( strcmp(nm, fn_name[k]) == 0 ) {
					v = v_undef();
					v.t = V_FN;
					v.n = k;
					return v;
				}
			}
			if ( live ) p->thrown = 1;
			return v_undef();
		}
	}
	p->bad = 1;
	return v_undef();
}

static CV postfix( PX *p, int live )
{
	CV	v = primary(p, live);

	while ( !p->bad && peek(p, "(") ) {
		CV	args[32];
		int	n = 0, k;

		p->at++;
		if ( !eat(p, ")") ) {
			do {
				CV	a = cond(p, live);

				if ( n < 32 ) args[n++] = a;
				else v_free(&a);
			} while ( !p->bad && eat(p, ",") );
			if ( !eat(p, ")") ) p->bad = 1;
		}
		if ( live && !p->bad ) {
			if ( v.t != V_FN ) {
				p->thrown = 1;
				v_free(&v);
				v = v_undef();
			} else {
				CV	r = call_fn((int)v.n, args, n);

				v_free(&v);
				v = r;
			}
		}
		for ( k = 0; k < n; k++ ) v_free(&args[k]);
	}
	return v;
}

static CV unary( PX *p, int live );

static CV power( PX *p, int live )
{
	CV	v = postfix(p, live);

	if ( !p->bad && peek(p, "**") ) {
		CV	e;
		double	x;

		p->at += 2;
		e = unary(p, live);
		x = pow(v_numval(&v), v_numval(&e));
		v_free(&v);
		v_free(&e);
		return v_num(x);
	}
	return v;
}

static CV unary( PX *p, int live )
{
	CV	v;
	double	x;

	skip(p);
	if ( p->s[p->at] == '!' && p->s[p->at + 1] != '=' ) {
		p->at++;
		v = unary(p, live);
		x = !v_true(&v);
		v_free(&v);
		return v_bool((int)x);
	}
	if ( p->s[p->at] == '-' || p->s[p->at] == '+' ) {
		int	neg = ( p->s[p->at] == '-' );

		p->at++;
		v = unary(p, live);
		x = v_numval(&v);
		v_free(&v);
		return v_num(neg ? -x : x);
	}
	return power(p, live);
}

static CV mul( PX *p, int live )
{
	CV	v = unary(p, live);

	for ( ;; ) {
		char	op;
		CV	w;
		double	a, b, r;

		skip(p);
		op = p->s[p->at];
		if ( p->bad || ( op != '*' && op != '/' && op != '%' ) || p->s[p->at + 1] == '*' ) return v;
		p->at++;
		w = unary(p, live);
		a = v_numval(&v);
		b = v_numval(&w);
		r = ( op == '*' ) ? a * b : ( op == '/' ) ? a / b : fmod(a, b);
		v_free(&v);
		v_free(&w);
		v = v_num(r);
	}
}

/* Whether + is a joining of words: either side is not a number, true, false, null or undefined */
static int joins( const CV *v )
{
	return v->t == V_STR || v->t == V_ARR || v->t == V_ERR || v->t == V_FN;
}

static CV add( PX *p, int live )
{
	CV	v = mul(p, live);

	for ( ;; ) {
		char	op;
		CV	w;

		skip(p);
		op = p->s[p->at];
		if ( p->bad || ( op != '+' && op != '-' ) ) return v;
		p->at++;
		w = mul(p, live);
		if ( op == '+' && ( joins(&v) || joins(&w) ) ) {
			char	*a = v_words(&v), *b = v_words(&w);
			SB	s = { NULL, 0, 0 };

			sb_put(&s, a != NULL ? a : "");
			sb_put(&s, b != NULL ? b : "");
			free(a);
			free(b);
			v_free(&v);
			v = v_str(s.p != NULL ? s.p : "");
			free(s.p);
		} else {
			double	r = ( op == '+' ) ? v_numval(&v) + v_numval(&w) : v_numval(&v) - v_numval(&w);

			v_free(&v);
			v = v_num(r);
		}
		v_free(&w);
	}
}

/* <, >, <=, >=: words with words by their letters, anything else as numbers */
static int compare( const CV *a, const CV *b, const char *op )
{
	if ( joins(a) && joins(b) && a->t != V_FN && b->t != V_FN ) {
		char	*x = v_words(a), *y = v_words(b);
		int	c = strcmp(x != NULL ? x : "", y != NULL ? y : "");

		free(x);
		free(y);
		if ( op[0] == '<' ) return op[1] == '=' ? c <= 0 : c < 0;
		return op[1] == '=' ? c >= 0 : c > 0;
	} else {
		double	x = v_numval(a), y = v_numval(b);

		if ( op[0] == '<' ) return op[1] == '=' ? x <= y : x < y;
		return op[1] == '=' ? x >= y : x > y;
	}
}

static CV rel( PX *p, int live )
{
	CV	v = add(p, live);

	for ( ;; ) {
		const char	*op;
		CV		w;
		int		r;

		skip(p);
		if ( p->bad ) return v;
		if ( peek(p, "<=") ) op = "<=";
		else if ( peek(p, ">=") ) op = ">=";
		else if ( peek(p, "<") ) op = "<";
		else if ( peek(p, ">") ) op = ">";
		else return v;
		p->at += (int)strlen(op);
		w = add(p, live);
		r = compare(&v, &w, op);
		v_free(&v);
		v_free(&w);
		v = v_bool(r);
	}
}

static int equal( const CV *a, const CV *b, int strict )
{
	if ( a->t == b->t ) {
		switch ( a->t ) {
		case V_NUM:
		case V_BOOL:	return a->n == b->n;
		case V_STR:	return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
		case V_UNDEF:
		case V_NULL:	return 1;
		default:	return 0;
		}
	}
	if ( strict ) return 0;
	if ( ( a->t == V_NULL || a->t == V_UNDEF ) && ( b->t == V_NULL || b->t == V_UNDEF ) ) return 1;
	if ( a->t == V_NULL || a->t == V_UNDEF || b->t == V_NULL || b->t == V_UNDEF ) return 0;
	if ( ( a->t == V_ARR || a->t == V_ERR ) && ( b->t == V_ARR || b->t == V_ERR ) ) return 0;
	if ( joins(a) && joins(b) ) {
		char	*x = v_words(a), *y = v_words(b);
		int	r = strcmp(x ? x : "", y ? y : "") == 0;

		free(x);
		free(y);
		return r;
	}
	return v_numval(a) == v_numval(b);
}

static CV eq( PX *p, int live )
{
	CV	v = rel(p, live);

	for ( ;; ) {
		int	neg, strict, r;
		CV	w;

		skip(p);
		if ( p->bad ) return v;
		if ( peek(p, "===") ) { neg = 0; strict = 1; p->at += 3; }
		else if ( peek(p, "!==") ) { neg = 1; strict = 1; p->at += 3; }
		else if ( peek(p, "==") ) { neg = 0; strict = 0; p->at += 2; }
		else if ( peek(p, "!=") ) { neg = 1; strict = 0; p->at += 2; }
		else return v;
		w = rel(p, live);
		r = equal(&v, &w, strict);
		v_free(&v);
		v_free(&w);
		v = v_bool(neg ? !r : r);
	}
}

/* && and ||: the value of the side that decided, the other not worked out */
static CV logic_and( PX *p, int live )
{
	CV	v = eq(p, live);

	while ( !p->bad && eat(p, "&&") ) {
		int	go = live && v_true(&v);
		CV	w = eq(p, go);

		if ( go ) {
			v_free(&v);
			v = w;
		} else {
			v_free(&w);
		}
	}
	return v;
}

static CV logic_or( PX *p, int live )
{
	CV	v = logic_and(p, live);

	while ( !p->bad && eat(p, "||") ) {
		int	go = live && !v_true(&v);
		CV	w = logic_and(p, go);

		if ( go ) {
			v_free(&v);
			v = w;
		} else {
			v_free(&w);
		}
	}
	return v;
}

static CV cond( PX *p, int live )
{
	CV	c = logic_or(p, live), a, b;
	int	t;

	if ( p->bad || !eat(p, "?") ) return c;
	t = v_true(&c);
	v_free(&c);
	a = cond(p, live && t);
	if ( !eat(p, ":") ) {
		p->bad = 1;
		v_free(&a);
		return v_undef();
	}
	b = cond(p, live && !t);
	if ( t ) {
		v_free(&b);
		return a;
	}
	v_free(&a);
	return b;
}

/* ---------------------------------------------------------------- a formula */

static const char *const dangerous[] = {
	"require", "import", "process", "window", "document", "global", "globalThis", "self", "top",
	"parent", "frames", "constructor", "__proto__", "prototype", "eval", "Function", "setTimeout",
	"setInterval", "setImmediate", "fetch", "XMLHttpRequest", "WebSocket", "Worker", "Blob", "module",
	"exports", "child_process", "fs", "path", "os", "net", "http", "https", "crypto"
};

static int has_dangerous( const char *s )
{
	int	i = 0, k;

	while ( s[i] != 0 ) {
		int	a;

		if ( !word(s[i]) ) {
			i++;
			continue;
		}
		a = i;
		while ( word(s[i]) ) i++;
		for ( k = 0; k < (int)( sizeof(dangerous) / sizeof(dangerous[0]) ); k++ ) {
			if ( (int)strlen(dangerous[k]) == i - a && strncmp(s + a, dangerous[k], (size_t)( i - a )) == 0 ) {
				return 1;
			}
		}
	}
	return 0;
}

/* Whether s[at..] is an A1 name with the edges of a word: its length, letters and digits */
static int a1_at( const char *s, int at, int *p_col, long long *p_row, int bounded, int *p_nl )
{
	int	i = at, a;
	long long	r = 0;

	if ( bounded && at > 0 && word(s[at - 1]) ) return 0;
	while ( up(s[i]) ) i++;
	if ( i == at ) return 0;
	a = i;
	while ( dig(s[i]) ) {
		if ( r < 100000000000LL ) r = r * 10 + ( s[i] - '0' );
		i++;
	}
	if ( i == a ) return 0;
	if ( bounded && word(s[i]) ) return 0;
	*p_col = letter_col(s + at, a - at);
	*p_row = r;
	*p_nl = a - at;
	return i - at;
}

/* "[row,col]" of either width at s[at..]: its length */
static int old_at( const char *s, int at, int *p_col, long long *p_row )
{
	static const char *const open[2] = { "[", "\xEF\xBC\xBB" }, *const comma[2] = { ",", "\xEF\xBC\x8C" },
			  *const shut[2] = { "]", "\xEF\xBC\xBD" };
	int		i = at, k, a;
	long long	r = 0, c = 0;

	for ( k = 0; k < 2 && strncmp(s + i, open[k], strlen(open[k])) != 0; k++ ) ;
	if ( k == 2 ) return 0;
	i += (int)strlen(open[k]);
	for ( a = i; dig(s[i]); i++ ) if ( r < 100000000000LL ) r = r * 10 + ( s[i] - '0' );
	if ( i == a ) return 0;
	for ( k = 0; k < 2 && strncmp(s + i, comma[k], strlen(comma[k])) != 0; k++ ) ;
	if ( k == 2 ) return 0;
	i += (int)strlen(comma[k]);
	for ( a = i; dig(s[i]); i++ ) if ( c < 100000000000LL ) c = c * 10 + ( s[i] - '0' );
	if ( i == a ) return 0;
	for ( k = 0; k < 2 && strncmp(s + i, shut[k], strlen(shut[k])) != 0; k++ ) ;
	if ( k == 2 ) return 0;
	i += (int)strlen(shut[k]);
	*p_row = r;
	*p_col = ( c > 0x3FFFFFFF ) ? 0x3FFFFFFF : (int)c;
	return i - at;
}

static CV evaluate( const char *formula, int col, int row, const EVSET *ev )
{
	SB		a = { NULL, 0, 0 }, b = { NULL, 0, 0 }, c = { NULL, 0, 0 };
	const char	*s;
	int		i, n;
	PX		p;
	CV		v;

	(void)col;
	(void)row;
	/* the text after "=", trimmed */
	s = formula + 1;
	while ( white((unsigned char)*s ) ) s++;
	n = (int)strlen(s);
	while ( n > 0 && white((unsigned char)s[n - 1]) ) n--;
	sb_putn(&a, s, (size_t)n);
	if ( a.p == NULL ) sb_put(&a, "");

	/* a range: the numbers in it */
	for ( i = 0; a.p[i] != 0; ) {
		int		c0, c1, len, nl, k;
		long long	r0, r1;

		len = a1_at(a.p, i, &c0, &r0, 0, &nl);
		if ( len > 0 && a.p[i + len] == ':' && ( k = a1_at(a.p, i + len + 1, &c1, &r1, 0, &nl) ) > 0 ) {
			SB		list = { NULL, 0, 0 };
			int		circ = 0, first = 1, cc;
			long long	rr;

			sb_put(&list, "[");
			for ( rr = r0; rr <= r1 && !circ; rr++ ) {
				for ( cc = c0; cc <= c1; cc++ ) {
					CV	x;

					if ( ev_has(ev, cc, (int)rr) ) {
						circ = 1;
						break;
					}
					x = value_recalc(cc, (int)rr, ev);
					if ( !( x.t == V_STR && ( x.s == NULL || x.s[0] == 0 ) ) && !isnan(v_numval(&x)) ) {
						char	w[32];

						num_words(v_numval(&x), w, sizeof(w));
						/* ", " keeps a list of two from reading as [row,col] */
						if ( !first ) sb_put(&list, ", ");
						sb_put(&list, w);
						first = 0;
					}
					v_free(&x);
				}
			}
			sb_put(&list, "]");
			sb_put(&b, circ ? "#CIRCULAR!" : list.p);
			free(list.p);
			i += len + 1 + k;
			continue;
		}
		sb_putn(&b, a.p + i, 1);
		i++;
	}
	if ( b.p == NULL ) sb_put(&b, "");

	/* [row,col] */
	for ( i = 0; b.p[i] != 0; ) {
		int		cc, len;
		long long	rr;

		len = old_at(b.p, i, &cc, &rr);
		if ( len > 0 ) {
			if ( ev_has(ev, cc, (int)rr) ) {
				sb_put(&c, "#CIRCULAR!");
			} else {
				CV	x = value_recalc(cc, (int)rr, ev);

				put_value(&c, &x);
				v_free(&x);
			}
			i += len;
			continue;
		}
		sb_putn(&c, b.p + i, 1);
		i++;
	}
	if ( c.p == NULL ) sb_put(&c, "");

	/* A1 */
	free(a.p);
	a.p = NULL;
	a.n = a.max = 0;
	for ( i = 0; c.p[i] != 0; ) {
		int		cc, len, nl;
		long long	rr;

		len = a1_at(c.p, i, &cc, &rr, 1, &nl);
		if ( len > 0 ) {
			if ( ev_has(ev, cc, (int)rr) ) {
				sb_put(&a, "#CIRCULAR!");
			} else {
				CV	x = value_recalc(cc, (int)rr, ev);

				put_value(&a, &x);
				v_free(&x);
			}
			i += len;
			continue;
		}
		sb_putn(&a, c.p + i, 1);
		i++;
	}
	if ( a.p == NULL ) sb_put(&a, "");
	free(b.p);
	free(c.p);

	if ( strstr(a.p, "#CIRCULAR!") != NULL ) {
		free(a.p);
		return v_str("#CIRCULAR!");
	}
	if ( has_dangerous(a.p) ) {
		free(a.p);
		return v_str("#ERROR!");
	}
	memset(&p, 0, sizeof(p));
	p.s = a.p;
	v = cond(&p, 1);
	skip(&p);
	if ( p.bad || p.thrown || p.s[p.at] != 0 ) {
		v_free(&v);
		v = v_str("#ERROR!");
	}
	free(a.p);
	return v;
}

/* ---------------------------------------------------------------- the calls */

/* What a formula names: its ranges, its A1 names and its [row,col] ones, as ranges */
static void refs_of( CCELL *c )
{
	const char	*f = c->formula;
	int		i, n = 0, m = 0;
	CREF		*r = NULL;

	if ( c->nref > 0 ) {
		deps_stale = 1;
	}
	free(c->refs);
	c->refs = NULL;
	c->nref = 0;
	if ( f == NULL || f[0] != '=' ) return;
	deps_stale = 1;
	for ( i = 0; f[i] != 0; ) {
		int		c0, c1, len, k, nl;
		long long	r0, r1;
		CREF		one;

		len = a1_at(f, i, &c0, &r0, 0, &nl);
		if ( len > 0 && f[i + len] == ':' && ( k = a1_at(f, i + len + 1, &c1, &r1, 0, &nl) ) > 0 ) {
			one.c0 = ( c0 < c1 ) ? c0 : c1;  one.c1 = ( c0 < c1 ) ? c1 : c0;
			one.r0 = (int)( ( r0 < r1 ) ? r0 : r1 );  one.r1 = (int)( ( r0 < r1 ) ? r1 : r0 );
			len += 1 + k;
		} else if ( ( len = a1_at(f, i, &c0, &r0, 1, &nl) ) > 0 || ( len = old_at(f, i, &c0, &r0) ) > 0 ) {
			one.c0 = one.c1 = c0;
			one.r0 = one.r1 = (int)( ( r0 > 0x3FFFFFFF ) ? 0x3FFFFFFF : r0 );
		} else {
			i++;
			continue;
		}
		if ( n == m ) {
			CREF	*q;

			m = m ? m * 2 : 4;
			q = realloc(r, sizeof(CREF) * (size_t)m);
			if ( q == NULL ) break;
			r = q;
		}
		r[n++] = one;
		i += len;
	}
	c->refs = r;
	c->nref = n;
}

static int refs_hold( const CCELL *c, int col, int row )
{
	int	k;

	for ( k = 0; k < c->nref; k++ ) {
		if ( col >= c->refs[k].c0 && col <= c->refs[k].c1 && row >= c->refs[k].r0 && row <= c->refs[k].r1 ) {
			return 1;
		}
	}
	return 0;
}

/*
 * Who names whom, made from the formulas' references when it is next
 * wanted after a formula changed: each place a formula names on its own,
 * in the order of the places, and each range a formula names. A changed
 * cell's formulas are then found at once, however many cells there are.
 */
typedef struct {
	int	col, row;		/* named */
	int	fc, fr;			/* by the formula there */
} DONE1;

typedef struct {
	CREF	r;
	int	fc, fr;
} DRANGE;

static DONE1	*d1;
static int	nd1;
static DRANGE	*dr;
static int	ndr;

static int d1_cmp( const void *a, const void *b )
{
	const DONE1	*x = a, *y = b;

	if ( x->row != y->row ) return ( x->row < y->row ) ? -1 : 1;
	if ( x->col != y->col ) return ( x->col < y->col ) ? -1 : 1;
	return 0;
}

static void deps_build( void )
{
	int	i, k, n1 = 0, nr = 0;

	free(d1);
	free(dr);
	d1 = NULL;
	dr = NULL;
	nd1 = ndr = 0;
	for ( i = 0; i < ncell; i++ ) {
		for ( k = 0; k < cells[i].nref; k++ ) {
			if ( cells[i].refs[k].c0 == cells[i].refs[k].c1 && cells[i].refs[k].r0 == cells[i].refs[k].r1 ) n1++;
			else nr++;
		}
	}
	d1 = malloc(sizeof(DONE1) * (size_t)( n1 + 1 ));
	dr = malloc(sizeof(DRANGE) * (size_t)( nr + 1 ));
	if ( d1 == NULL || dr == NULL ) {
		free(d1);
		free(dr);
		d1 = NULL;
		dr = NULL;
		return;				/* stays stale: looked for the slow way */
	}
	for ( i = 0; i < ncell; i++ ) {
		for ( k = 0; k < cells[i].nref; k++ ) {
			CREF	*r = &cells[i].refs[k];

			if ( r->c0 == r->c1 && r->r0 == r->r1 ) {
				d1[nd1].col = r->c0;
				d1[nd1].row = r->r0;
				d1[nd1].fc = cells[i].col;
				d1[nd1].fr = cells[i].row;
				nd1++;
			} else {
				dr[ndr].r = *r;
				dr[ndr].fc = cells[i].col;
				dr[ndr].fr = cells[i].row;
				ndr++;
			}
		}
	}
	qsort(d1, (size_t)nd1, sizeof(DONE1), d1_cmp);
	deps_stale = 0;
}

/* Places to look at next, in the order they came */
typedef struct {
	int	*p;			/* pairs */
	int	head, n, max;
} WQ;

static void wq_put( WQ *q, int col, int row )
{
	if ( q->n == q->max ) {
		int	m = q->max ? q->max * 2 : 64, *np;

		/* what was taken off the front is dropped as it grows */
		if ( q->head > 0 ) {
			memmove(q->p, q->p + 2 * q->head, sizeof(int) * 2 * (size_t)( q->n - q->head ));
			q->n -= q->head;
			q->head = 0;
		}
		if ( q->n == q->max ) {
			np = realloc(q->p, sizeof(int) * 2 * (size_t)m);
			if ( np == NULL ) return;
			q->p = np;
			q->max = m;
		}
	}
	q->p[2 * q->n] = col;
	q->p[2 * q->n + 1] = row;
	q->n++;
}

/* The formula at (c, r) worked out in this working out, when it is not yet, and looked past */
static void dep_one( WQ *q, int c, int r )
{
	int	i = find_cell(c, r);

	if ( i < 0 || cells[i].formula[0] == 0 || cells[i].dpass == g_pass ) return;
	cells[i].dpass = g_pass;
	if ( cells[i].pass != g_pass ) {
		EVSET	ev;
		int	k[2];
		CV	v;

		k[0] = c;
		k[1] = r;
		ev.n = 1;
		ev.k = k;
		v = value_recalc(c, r, &ev);
		v_free(&v);
	}
	touch(c, r);
	wq_put(q, c, r);
}

/*
 * Every formula naming the cell worked out again, and the ones naming
 * those, each once in a working out: a ring of them does not go round
 * for ever, and a long chain is followed one link after another rather
 * than by calling down it.
 */
static void recalc_dependents( int col, int row )
{
	WQ	q = { NULL, 0, 0, 0 };

	if ( g_bulk ) return;
	if ( deps_stale ) deps_build();
	wq_put(&q, col, row);
	while ( q.head < q.n ) {
		int	c = q.p[2 * q.head], r = q.p[2 * q.head + 1], k;

		q.head++;
		if ( deps_stale ) {
			/* no room for the index: every formula looked at */
			int	i;

			for ( i = 0; i < ncell; i++ ) {
				if ( cells[i].nref > 0 && refs_hold(&cells[i], c, r) ) {
					dep_one(&q, cells[i].col, cells[i].row);
				}
			}
			continue;
		}
		{
			/* those naming it on its own: the first of them, then on */
			int	lo = 0, hi = nd1;
			DONE1	key;

			key.col = c;
			key.row = r;
			while ( lo < hi ) {
				int	mid = ( lo + hi ) / 2;

				if ( d1_cmp(&d1[mid], &key) < 0 ) lo = mid + 1;
				else hi = mid;
			}
			for ( k = lo; k < nd1 && d1[k].col == c && d1[k].row == r; k++ ) {
				dep_one(&q, d1[k].fc, d1[k].fr);
			}
		}
		for ( k = 0; k < ndr; k++ ) {
			if ( c >= dr[k].r.c0 && c <= dr[k].r.c1 && r >= dr[k].r.r0 && r <= dr[k].r.r1 ) {
				dep_one(&q, dr[k].fc, dr[k].fr);
			}
		}
	}
	free(q.p);
}

int ce_start( void )
{
	return 0;
}

void ce_end( void )
{
	(void)ce_reset();
	free(changed);
	changed = NULL;
	nchg = mchg = 0;
}

const char *ce_name( void )
{
	return "C";
}

int ce_set( int col, int row, const unsigned char *text )
{
	int	i = make_cell(col, row);
	char	*t = strdup((const char *)text);

	if ( i < 0 || t == NULL ) {
		free(t);
		return -1;
	}
	free(cells[i].value);
	free(cells[i].formula);
	v_free(&cells[i].shown);
	if ( !g_bulk ) {
		g_pass++;			/* a new working out */
	}
	if ( t[0] == '=' && g_bulk ) {
		/* worked out with all the others, when they are all in (ce_bulk) */
		cells[i].formula = t;
		cells[i].value = strdup("");
		cells[i].shown = v_str("");
		refs_of(&cells[i]);
		touch(col, row);
		return 0;
	}
	if ( t[0] == '=' ) {
		EVSET	ev;
		int	k[2] = { col, row };
		CV	v;

		cells[i].formula = t;
		cells[i].value = strdup("");
		cells[i].shown = v_str("");
		refs_of(&cells[i]);
		ev.n = 1;
		ev.k = k;
		v = evaluate(t, col, row, &ev);
		i = find_cell(col, row);
		if ( i >= 0 ) {
			v_free(&cells[i].shown);
			cells[i].shown = v;
			cells[i].pass = g_pass;
		}
	} else {
		cells[i].value = t;
		cells[i].formula = strdup("");
		cells[i].shown = v_str(t);
		refs_of(&cells[i]);
		if ( g_bulk ) {
			/*
			 * What it shows is what it was given, which the sheet holds
			 * already: not told back (a sheet read keeps a million
			 * numbers from being taken again one at a time).
			 */
			return 0;
		}
	}
	if ( i >= 0 ) {
		cells[i].dpass = g_pass;
	}
	touch(col, row);
	recalc_dependents(col, row);
	return 0;
}

/*
 * Many cells put in at once: from ce_bulk(1) on, a cell is kept as it
 * is given and nothing is worked out; at ce_bulk(0) every formula is
 * worked out once, each in the light of the others.
 */
int ce_bulk( int on )
{
	int	i;

	if ( on ) {
		g_bulk = 1;
		return 0;
	}
	g_bulk = 0;
	g_pass++;
	for ( i = 0; i < ncell; i++ ) {
		if ( cells[i].formula[0] != 0 && cells[i].pass != g_pass ) {
			int	c = cells[i].col, r = cells[i].row;
			EVSET	ev;
			CV	v;

			ev.n = 0;
			ev.k = NULL;
			v = value_recalc(c, r, &ev);
			v_free(&v);
			touch(c, r);
		}
	}
	return 0;
}

static void drop_at( int i )
{
	if ( cells[i].nref > 0 ) {
		deps_stale = 1;
	}
	pt_del(&hx, cell_place, cells[i].col, cells[i].row);
	free(cells[i].value);
	free(cells[i].formula);
	free(cells[i].refs);
	v_free(&cells[i].shown);
	ncell--;
	if ( i < ncell ) {
		/* the last in its place: in the table, under its own place */
		cells[i] = cells[ncell];
		hx.slot[pt_slot(&hx, cell_place, cells[i].col, cells[i].row)] = i + 1;
	}
}

int ce_clear( int col, int row )
{
	int	i = find_cell(col, row);

	if ( i >= 0 ) drop_at(i);
	touch(col, row);
	if ( !g_bulk ) {
		g_pass++;
		recalc_dependents(col, row);	/* what named it names nothing now */
	}
	return 0;
}

int ce_blank( int col, int row )
{
	int	i = make_cell(col, row);

	if ( i < 0 ) return -1;
	free(cells[i].value);
	free(cells[i].formula);
	if ( cells[i].nref > 0 ) {
		deps_stale = 1;
	}
	free(cells[i].refs);
	cells[i].refs = NULL;
	cells[i].nref = 0;
	v_free(&cells[i].shown);
	cells[i].value = strdup("");
	cells[i].formula = strdup("");
	cells[i].shown = v_str("");
	touch(col, row);
	return 0;
}

int ce_move( int cols, int at, int d )
{
	int	i, k = 0;

	/* in one walk: those taken out let go, the others renamed and closed up */
	for ( i = 0; i < ncell; i++ ) {
		int	*v = cols ? &cells[i].col : &cells[i].row;

		if ( d < 0 && *v == at ) {
			free(cells[i].value);
			free(cells[i].formula);
			free(cells[i].refs);
			v_free(&cells[i].shown);
			continue;
		}
		if ( *v > at || ( d > 0 && *v == at ) ) *v += d;
		cells[k++] = cells[i];
	}
	ncell = k;
	pt_build(&hx, cell_place, ncell);
	deps_stale = 1;			/* the places renamed */
	return 0;
}

int ce_reset( void )
{
	int	i;

	for ( i = 0; i < ncell; i++ ) {
		free(cells[i].value);
		free(cells[i].formula);
		free(cells[i].refs);
		v_free(&cells[i].shown);
	}
	ncell = 0;
	nchg = 0;
	deps_stale = 1;
	pt_build(&hx, cell_place, 0);
	pt_build(&hc, chg_place, 0);
	return 0;
}

int ce_taken( CE_FN fn, void *arg )
{
	int	i, n = nchg;

	for ( i = 0; i < nchg; i++ ) {
		int	c = changed[2 * i], r = changed[2 * i + 1], k = find_cell(c, r);
		char	*w = NULL;

		/* the words shown: the value, or the formula's result, when either is anything */
		if ( k >= 0 ) {
			if ( v_true(&cells[k].shown) ) w = v_words(&cells[k].shown);
			else if ( cells[k].value[0] != 0 ) w = strdup(cells[k].value);
		}
		fn(c, r, (const unsigned char *)( w != NULL ? w : "" ), w != NULL ? (int)strlen(w) : 0, arg);
		free(w);
	}
	nchg = 0;
	if ( hc.cap != 0 ) {
		memset(hc.slot, 0, sizeof(int) * hc.cap);
	}
	return n;
}

int ce_test( const unsigned char *text, const unsigned char *pat, int regex )
{
	(void)regex;
	return strstr((const char *)text, (const char *)pat) != NULL;
}

int ce_replace( const unsigned char *text, const unsigned char *pat, const unsigned char *rep, int regex,
		unsigned char *out, int max )
{
	const char	*s = (const char *)text, *f;
	size_t		lp = strlen((const char *)pat), lr = strlen((const char *)rep);
	int		k = 0;

	(void)regex;
	if ( max <= 0 ) return -1;
	/* written as far as there is room; the whole counted all the same */
	while ( lp > 0 && ( f = strstr(s, (const char *)pat) ) != NULL ) {
		size_t	m = (size_t)( f - s ), i;

		for ( i = 0; i < m; i++, k++ ) if ( k < max - 1 ) out[k] = (unsigned char)s[i];
		for ( i = 0; i < lr; i++, k++ ) if ( k < max - 1 ) out[k] = rep[i];
		s = f + lp;
	}
	for ( ; *s != 0; s++, k++ ) if ( k < max - 1 ) out[k] = (unsigned char)*s;
	out[( k < max - 1 ) ? k : max - 1] = 0;
	return k;
}
