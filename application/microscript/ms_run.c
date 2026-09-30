/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_run.c
 *	The script run: values, variables, threads and the statements
 *
 *	A procedure runs as a thread of its own that takes turns with the
 *	others: a thread lets the others go when it sleeps or waits, at each
 *	round of a WHILE, every 256 rounds of a REPEAT, and after it has run
 *	for a while without doing either. The stage is drawn between turns.
 *
 *	A number is a double. A value may also be a name (of a segment or a
 *	procedure: a name that is not a variable stands for itself) or an
 *	array of values (a string is the array of its characters ending in
 *	0). Arrays are counted: a variable, a thread or an expression that
 *	keeps one holds a reference.
 *
 *	The invalid value is -2147483648. An expression with it in it is
 *	invalid unless it compares.
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---------------------------------------------------------------- values */

MSV mv_num( double n )
{
	MSV	v;

	v.t = V_NUM;
	v.u.n = n;
	return v;
}

MSV mv_str( MSSTR *s )
{
	MSV	v;

	v.t = V_STR;
	v.u.s = s;
	return v;
}

MSV mv_arr( INT n )
{
	MSV	v;

	if ( n < 0 ) n = 0;
	v.t = V_ARR;
	v.u.a = ms_alloc(sizeof(MSARR) + sizeof(MSV) * (size_t)n);
	v.u.a->ref = 1;
	v.u.a->n = n;
	return v;
}

MSV mv_chars( const UW *c, INT n )
{
	MSV	v = mv_arr(n + 1);
	INT	i;

	for ( i = 0; i < n; i++ ) v.u.a->e[i] = mv_num(c[i]);
	return v;
}

MSV mv_cstr( const char *s )
{
	INT	n = (INT)strlen(s), k = 0, i;
	UW	*c = ms_alloc(sizeof(UW) * (size_t)( n + 1 ));
	MSV	v;

	for ( i = 0; i < n; ) {
		UW	cp;
		INT	d = ms_utf8_dec((const UB *)s + i, n - i, &cp);

		if ( d <= 0 ) break;
		c[k++] = cp;
		i += d;
	}
	v = mv_chars(c, k);
	ms_free(c);
	return v;
}

MSV mv_ref( MSV v )
{
	if ( v.t == V_ARR ) v.u.a->ref++;
	return v;
}

void mv_drop( MSV v )
{
	INT	i;

	if ( v.t != V_ARR ) return;
	if ( --v.u.a->ref > 0 ) return;
	for ( i = 0; i < v.u.a->n; i++ ) mv_drop(v.u.a->e[i]);
	ms_free(v.u.a);
}

BOOL mv_is_invalid( MSV v )
{
	return (BOOL)( v.t == V_NUM && v.u.n == MS_INVALID );
}

double mv_n( MSV v )
{
	if ( v.t == V_NUM ) return v.u.n;
	if ( v.t == V_ARR ) return ( v.u.a->n > 0 && v.u.a->e[0].t == V_NUM ) ? v.u.a->e[0].u.n : 0;
	return NAN;
}

/* A number as a 32 bit integer: its whole part, wrapped round modulo 2^32; 0 for NaN and the infinities */
INT ms_int32( double d )
{
	double	m;

	if ( !isfinite(d) ) return 0;
	d = trunc(d);
	if ( d >= -2147483648.0 && d <= 2147483647.0 ) return (INT)d;
	m = fmod(d, 4294967296.0);
	if ( m < 0 ) m += 4294967296.0;
	return (INT)(UW)(UD)m;
}

INT mv_i( MSV v )
{
	return ms_int32(mv_n(v));
}

/* ---------------------------------------------------------------- number to text */

/*
 * A number as the script shows it: an integer without a point, other
 * numbers in the fewest digits that read back as the same number.
 */
void ms_num_text( double d, MSBUF *b )
{
	char	s[40], e[40];
	INT	p;

	if ( isnan(d) ) { mb_puts(b, "NaN"); return; }
	if ( isinf(d) ) { mb_puts(b, d < 0 ? "-Infinity" : "Infinity"); return; }
	if ( d == 0 ) { mb_puts(b, "0"); return; }
	if ( d == trunc(d) && fabs(d) < 1e21 ) {
		snprintf(s, sizeof(s), "%.0f", d);
		mb_puts(b, s);
		return;
	}
	for ( p = 1; p <= 17; p++ ) {
		snprintf(s, sizeof(s), "%.*g", p, d);
		if ( strtod(s, NULL) == d ) break;
	}
	/* with an exponent from 1e21 up and below 1e-6 */
	{
		INT	ex = (INT)floor(log10(fabs(d)));

		if ( ex >= 21 || ex < -6 ) {
			char	*m;
			INT	k, ee;

			snprintf(e, sizeof(e), "%.*e", p - 1, d);
			m = strchr(e, 'e');
			if ( m != NULL ) {
				ee = atoi(m + 1);
				*m = 0;
				/* trailing zeros of the mantissa */
				k = (INT)strlen(e);
				if ( strchr(e, '.') != NULL ) {
					while ( k > 0 && e[k - 1] == '0' ) e[--k] = 0;
					if ( k > 0 && e[k - 1] == '.' ) e[--k] = 0;
				}
				mb_printf(b, "%se%c%d", e, ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
				return;
			}
		}
		snprintf(s, sizeof(s), "%.*f", ( ex < 0 ) ? p - 1 - ex : ( p - 1 - ex > 0 ? p - 1 - ex : 0 ), d);
		/* no trailing zeros after the point */
		if ( strchr(s, '.') != NULL ) {
			INT	k = (INT)strlen(s);

			while ( k > 0 && s[k - 1] == '0' ) s[--k] = 0;
			if ( k > 0 && s[k - 1] == '.' ) s[--k] = 0;
		}
		mb_puts(b, s);
	}
}

void mv_text( MSV v, MSBUF *b )
{
	INT	i;

	switch ( v.t ) {
	case V_NUM:
		ms_num_text(v.u.n, b);
		break;
	case V_STR:
		mb_puts(b, v.u.s->s);
		break;
	case V_ARR:
		for ( i = 0; i < v.u.a->n; i++ ) {
			double	c = mv_n(v.u.a->e[i]);

			if ( c == 0 ) break;
			mb_putcp(b, (UW)ms_int32(c) & 0xFFFF);
		}
		break;
	}
}

/* ---------------------------------------------------------------- variables */

static INT type_bytes( char t )
{
	return ( t == 'B' ) ? 1 : ( t == 'C' ) ? 2 : 4;
}

static MSV coerce( const MSVAR *v, MSV val )
{
	if ( val.t != V_NUM ) return val;
	if ( v->type == 'B' ) return mv_num((double)( ms_int32(val.u.n) & 0xFF ));
	if ( v->type == 'C' ) return mv_num((double)( ms_int32(val.u.n) & 0xFFFF ));
	return val;
}

static INT var_avail( const MSVAR *v )
{
	INT	n = v->size;

	if ( v->raw != NULL ) {
		INT	k = ( v->rawlen - v->rawoff ) / type_bytes(v->type);

		if ( k < n ) n = k;
	} else if ( v->avail < n ) {
		n = v->avail;
	}
	return n;
}

/* An element, owned by the caller */
MSV var_get( const MSVAR *v, INT i )
{
	if ( i < 0 || i >= v->size ) return mv_num(MS_INVALID);
	if ( i >= var_avail(v) ) return mv_num(0);
	if ( v->raw != NULL ) {
		const UB	*p = v->raw + v->rawoff + i * type_bytes(v->type);

		switch ( v->type ) {
		case 'B':	return mv_num(p[0]);
		case 'C':	return mv_num((double)( p[0] | ( p[1] << 8 ) ));
		case 'F': {
			float	f;

			memcpy(&f, p, 4);
			return mv_num(f);
		}
		default: {
			INT	k;

			memcpy(&k, p, 4);
			return mv_num(k);
		}
		}
	}
	return mv_ref(v->data[i]);
}

/* An element set; the value is the caller's still */
void var_set( MSVAR *v, INT i, MSV val )
{
	if ( i < 0 || i >= v->size || i >= var_avail(v) ) return;
	val = coerce(v, val);
	if ( v->raw != NULL ) {
		UB	*p = v->raw + v->rawoff + i * type_bytes(v->type);
		double	d = mv_n(val);

		switch ( v->type ) {
		case 'B':	p[0] = (UB)ms_int32(d); break;
		case 'C': {
			UH	h = (UH)ms_int32(d);

			p[0] = (UB)h;
			p[1] = (UB)( h >> 8 );
			break;
		}
		case 'F': {
			float	f = (float)d;

			memcpy(p, &f, 4);
			break;
		}
		default: {
			INT	k = ms_int32(d);

			memcpy(p, &k, 4);
			break;
		}
		}
		return;
	}
	mv_drop(v->data[i]);
	v->data[i] = mv_ref(val);
}

static MSVAR *var_new( MSSTR *name, char type, INT size, BOOL force_array, BOOL local )
{
	MSVAR	*v = ms_alloc(sizeof(MSVAR));
	INT	cap = local ? MS_MAX_LOCAL : MS_MAX_GLOBAL;

	if ( size < 1 ) size = 1;
	if ( size > cap ) size = cap;
	v->name = name;
	v->type = type ? type : 'G';
	v->size = size;
	v->scalar = (BOOL)( size <= 1 && !force_array );
	v->data = ms_alloc(sizeof(MSV) * (size_t)size);	/* zeros: V_NUM 0 */
	v->avail = size;
	v->owned = TRUE;
	return v;
}

void var_free( MSVAR *v )
{
	INT	i;

	if ( v->owned && v->data != NULL ) {
		for ( i = 0; i < v->avail; i++ ) mv_drop(v->data[i]);
		ms_free(v->data);
	}
	if ( v->rawowned ) ms_free(v->raw);
	ms_free(v);
}

MSVAR *scope_find( MSSCOPE *s, MSSTR *name )
{
	for ( ; s != NULL; s = s->parent ) {
		MSVAR	*v;

		for ( v = s->vars; v != NULL; v = v->next ) {
			if ( v->name == name ) return v;
		}
	}
	return NULL;
}

static MSVAR *scope_here( MSSCOPE *s, MSSTR *name )
{
	MSVAR	*v;

	for ( v = s->vars; v != NULL; v = v->next ) {
		if ( v->name == name ) return v;
	}
	return NULL;
}

static void scope_add( MSSCOPE *s, MSVAR *v )
{
	v->next = s->vars;
	s->vars = v;
}

/* The bytes of v kept as bytes from now on, so that another type can be laid over them */
static void var_to_raw( MSVAR *v )
{
	INT	w = type_bytes(v->type), i;
	MSV	*old = v->data;
	INT	n = v->avail;

	v->raw = ms_alloc((size_t)( v->size * w ));
	v->rawlen = v->size * w;
	v->rawoff = 0;
	v->rawowned = TRUE;
	v->data = NULL;
	for ( i = 0; i < n; i++ ) {
		var_set(v, i, old[i]);
	}
	if ( v->owned ) {
		for ( i = 0; i < n; i++ ) mv_drop(old[i]);
		ms_free(old);
	}
	v->owned = FALSE;
}

MSVAR *scope_declare( MSSCOPE *s, MSDECL *d, INT size, INT share_off, BOOL force_array, BOOL local )
{
	MSVAR	*v = scope_here(s, d->name), *base;

	if ( v != NULL ) return v;
	v = var_new(d->name, d->type, size, force_array, local);
	if ( d->share != NULL && ( base = scope_find(s, d->share) ) != NULL ) {
		INT	bw = type_bytes(base->type), mw = type_bytes(v->type);

		if ( share_off < 0 ) share_off = 0;
		if ( base->raw == NULL && bw == mw ) {
			/* the same width: the same elements */
			var_free_data(v);
			v->data = base->data + ( ( share_off < base->avail ) ? share_off : base->avail );
			v->avail = ( share_off < base->avail ) ? base->avail - share_off : 0;
			v->owned = FALSE;
			v->scalar = base->scalar;
		} else {
			/* another width: the bytes */
			if ( base->raw == NULL ) var_to_raw(base);
			var_free_data(v);
			v->raw = base->raw;
			v->rawlen = base->rawlen;
			v->rawoff = base->rawoff + share_off * bw;
			if ( v->rawoff > v->rawlen ) v->rawoff = v->rawlen;
			v->owned = FALSE;
			v->scalar = FALSE;
		}
	}
	scope_add(s, v);
	return v;
}

void var_free_data( MSVAR *v )
{
	INT	i;

	if ( v->owned && v->data != NULL ) {
		for ( i = 0; i < v->avail; i++ ) mv_drop(v->data[i]);
		ms_free(v->data);
	}
	v->data = NULL;
	v->avail = 0;
	v->owned = FALSE;
}

MSVAR *scope_bind_alias( MSSCOPE *s, MSSTR *name, char type, MSVAR *of )
{
	MSVAR	*v = ms_alloc(sizeof(MSVAR));

	v->name = name;
	v->type = type ? type : of->type;
	v->size = of->size;
	v->scalar = FALSE;
	v->data = of->data;
	v->avail = of->avail;
	v->raw = of->raw;
	v->rawlen = of->rawlen;
	v->rawoff = of->rawoff;
	v->segref = of->segref;
	v->owned = FALSE;
	scope_add(s, v);
	return v;
}

MSSCOPE *scope_new( MSSCOPE *parent )
{
	MSSCOPE	*s = ms_alloc(sizeof(MSSCOPE));

	s->parent = parent;
	return s;
}

void scope_free( MSSCOPE *s )
{
	MSVAR	*v, *n;
	INT	i;

	for ( v = s->vars; v != NULL; v = n ) {
		n = v->next;
		var_free(v);
	}
	for ( i = 0; i < s->nargs; i++ ) mv_drop(s->args[i]);
	ms_free(s->args);
	ms_free(s);
}

/* ---------------------------------------------------------------- the run time */

MSRT	rt;

static void thread_free( MSTH *t );

BOOL rt_finished( void )
{
	return rt.finished && rt.threads == NULL;
}

void rt_error( const char *msg )
{
	MSBUF	b;

	mb_init(&b);
	mb_puts(&b, msg);
	mb_putc(&b, '\n');
	ms_message(b.s);
	mb_free(&b);
	tm_putstring((const UB *)"microscript: ");
	tm_putstring((const UB *)msg);
	tm_putstring((const UB *)"\n");
}

/* An error in a thread: it stops, and the message says where */
void th_fail( MSTH *t, const char *fmt, ... )
{
	va_list	ap;
	char	msg[256];

	if ( t->err ) return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	t->err = TRUE;
	snprintf(t->errmsg, sizeof(t->errmsg), "実行時エラー: %s", msg);
}

MSPROC *rt_proc( MSSTR *name )
{
	INT	i;

	if ( name == NULL ) return NULL;
	for ( i = 0; i < ms_prog.nproc; i++ ) {
		if ( ms_prog.proc[i]->name == name ) return ms_prog.proc[i];
	}
	return NULL;
}

static MSPROC *rt_proc_of_type( INT type )
{
	INT	i;

	for ( i = 0; i < ms_prog.nproc; i++ ) {
		if ( ms_prog.proc[i]->type == type ) return ms_prog.proc[i];
	}
	return NULL;
}

/* A number the declarations give, before anything runs */
static double eval_const( MSN *n )
{
	if ( n == NULL ) return 1;
	switch ( n->k ) {
	case N_NUM:
		return n->num;
	case N_NAME: {
		MSMACRO	*m = ms_macro(&ms_prog, n->name);

		if ( m != NULL && m->nbody == 1 && ( m->body[0].type == T_INT || m->body[0].type == T_FLOAT ) ) {
			return m->body[0].num;
		}
		return 1;
	}
	case N_UNARY: {
		double	v = eval_const(n->a);

		if ( n->op == '-' ) return -v;
		if ( n->op == '~' ) return ~ms_int32(v);
		if ( n->op == '!' ) return ( v != 0 ) ? 0 : 1;
		return v;
	}
	case N_BIN: {
		double	l = eval_const(n->a), r = eval_const(n->b);

		switch ( n->op ) {
		case '+': return l + r;
		case '-': return l - r;
		case '*': return l * r;
		case '/': return ( r == 0 ) ? 0 : trunc(l / r);
		case '%': return ( r == 0 ) ? 0 : l - trunc(l / r) * r;
		case '&': return ms_int32(l) & ms_int32(r);
		case '|': return ms_int32(l) | ms_int32(r);
		case '^': return ms_int32(l) ^ ms_int32(r);
		case OPC('<', '<'): return (double)(INT)( (UW)ms_int32(l) << ( ms_int32(r) & 31 ) );
		case OPC('>', '>'): return ms_int32(l) >> ( ms_int32(r) & 31 );
		}
		return 1;
	}
	}
	return 1;
}

/* The segment names the script can use, as variables of their own */
void rt_bind_segment( MSSEG *s )
{
	MSVAR	*v;

	if ( scope_here(rt.global, s->name) != NULL ) return;
	v = var_new(s->name, 'S', 1, FALSE, FALSE);
	v->data[0] = mv_str(s->name);
	v->segref = TRUE;
	v->segname = TRUE;
	scope_add(rt.global, v);
}

INT rt_load( MSPROG *p )
{
	INT	i, k;
	MSSEG	*s;

	(void)p;
	rt.global = scope_new(NULL);
	for ( i = 0; i < ms_prog.nglobal; i++ ) {
		MSDECL	*d = &ms_prog.global[i];
		INT	size = (INT)eval_const(d->size);
		INT	off = ( d->share != NULL ) ? ms_int32(eval_const(d->share_off ? d->share_off : NULL)) : 0;

		if ( d->share != NULL && d->share_off == NULL ) off = 0;
		scope_declare(rt.global, d, size, off, size > 1, FALSE);
	}
	for ( i = 0; i < ms_prog.nsegdecl; i++ ) {
		MSDECL	*d = &ms_prog.segdecl[i];
		INT	size = (INT)eval_const(d->size);
		MSVAR	*v;

		if ( size < 1 ) size = 1;
		if ( size > 500 ) size = 500;
		v = scope_declare(rt.global, d, size, 0, size > 1, FALSE);
		v->segref = TRUE;
		if ( size > 1 ) {
			/* SEGMENT a[3] is a0, a1, a2 and the array a naming them */
			for ( k = 0; k < size; k++ ) {
				char	nm[80];
				MSDECL	e;
				MSVAR	*ev;

				snprintf(nm, sizeof(nm), "%s%d", d->name->s, k);
				memset(&e, 0, sizeof(e));
				e.name = ms_intern_z(nm);
				e.type = 'S';
				ev = scope_declare(rt.global, &e, 1, 0, FALSE, FALSE);
				ev->segref = TRUE;
				var_set(ev, 0, mv_str(e.name));
				var_set(v, k, mv_str(e.name));
			}
		}
	}
	for ( s = st_first(); s != NULL; s = s->next ) rt_bind_segment(s);
	rt.segconsume = NULL;
	return E_OK;
}

/* ---------------------------------------------------------------- threads */

static MSTH	*spawn_last;

static void thread_main( void *arg );

MSTH *rt_spawn( MSPROC *proc, MSV *args, INT nargs, BOOL epilogue )
{
	MSTH	*t;
	INT	i, n = 0;

	spawn_last = NULL;
	if ( proc->type == P_ACTION && proc->running > 0 ) return NULL;	/* one at a time */
	for ( t = rt.threads; t != NULL; t = t->next ) n++;
	if ( n >= MS_MAX_THREADS ) {
		rt_error("実行時エラー: 手続きの数が多すぎます");
		return NULL;
	}
	t = ms_alloc(sizeof(MSTH));
	t->id = rt.next_id++;
	t->proc = proc;
	t->name = proc->name;
	t->epilogue = epilogue;
	t->args = ms_alloc(sizeof(MSV) * (size_t)( nargs + 1 ));
	for ( i = 0; i < nargs; i++ ) t->args[i] = mv_ref(args[i]);
	t->nargs = nargs;
	t->stack = ms_alloc(MS_STACK);
	memset(&t->ctx, 0, sizeof(t->ctx));
	t->ctx.x[0] = (UD)(UBINT)thread_main;		/* x19 */
	t->ctx.x[1] = (UD)(UBINT)t;			/* x20 */
	t->ctx.x[11] = (UD)(UBINT)ms_co_entry;		/* x30 */
	t->ctx.sp = ( (UD)(UBINT)( (UB *)t->stack + MS_STACK ) ) & ~(UD)15;
	t->wake = 0;
	proc->running++;
	/* at the end of the list: they are run in the order they were started */
	if ( rt.threads == NULL ) {
		rt.threads = t;
	} else {
		MSTH	*q = rt.threads;

		while ( q->next != NULL ) q = q->next;
		q->next = t;
	}
	spawn_last = t;
	return t;
}

INT rt_last_spawned( void )
{
	return ( spawn_last != NULL ) ? spawn_last->id : ms_int32(MS_INVALID);
}

static void thread_main( void *arg )
{
	MSTH	*t = (MSTH *)arg;
	MSSCOPE	*sc = scope_new(rt.global);
	INT	i;

	sc->hasargs = TRUE;
	sc->args = t->args;
	sc->nargs = t->nargs;
	t->args = NULL;
	t->nargs = 0;
	for ( i = 0; i < t->proc->nparam; i++ ) {
		MSDECL	*p = &t->proc->param[i];
		INT	size = p->open ? 0 : (INT)eval_const(p->size);
		MSVAR	*v = scope_declare(sc, p, size, 0, size > 1, TRUE);

		if ( i < sc->nargs ) var_set(v, 0, sc->args[i]);
	}
	(void)exec_block(t, t->proc->body, t->proc->nbody, sc);
	if ( t->err ) {
		char	m[400];

		snprintf(m, sizeof(m), "[%s エラー] %s",
			 ( t->proc->name != NULL ) ? t->proc->name->s
			 : ( t->proc->type == P_PROLOGUE ) ? "PROLOGUE" : "EPILOGUE", t->errmsg);
		rt_error(m);
	}
	scope_free(sc);
}

/* Called when a thread's function returns: it is done, and does not come back */
void ms_co_done( void )
{
	MSTH	*t = rt.cur;

	t->done = TRUE;
	ms_co_switch(&t->ctx, &rt.sched);
	for ( ;; ) ;
}

void ms_yield( UD wake )
{
	MSTH	*t = rt.cur;

	if ( t == NULL ) return;
	t->wake = wake;
	t->steps = 0;
	ms_co_switch(&t->ctx, &rt.sched);
}

/* Whether a thread must stop where it is */
BOOL th_stop( MSTH *t )
{
	if ( t->terminated || t->err ) return TRUE;
	if ( rt.finished && !t->epilogue ) return TRUE;
	return FALSE;
}

static void thread_free( MSTH *t )
{
	INT	i;

	for ( i = 0; i < t->nargs; i++ ) mv_drop(t->args[i]);
	ms_free(t->args);
	ms_free(t->stack);
	ms_free(t);
}

void rt_run_threads( void )
{
	MSTH	*t, **pp;
	UD	now = ms_now_ms();

	for ( t = rt.threads; t != NULL; t = t->next ) t->turn = FALSE;
	for ( ;; ) {
		/* the next that has not had its turn and may run */
		for ( t = rt.threads; t != NULL; t = t->next ) {
			if ( !t->turn && t->wake <= now ) break;
		}
		if ( t == NULL ) break;
		t->turn = TRUE;
		rt.cur = t;
		ms_co_switch(&rt.sched, &t->ctx);
		rt.cur = NULL;
		if ( t->done ) {
			for ( pp = &rt.threads; *pp != NULL; pp = &(*pp)->next ) {
				if ( *pp == t ) { *pp = t->next; break; }
			}
			if ( t->proc->running > 0 ) t->proc->running--;
			if ( t->epilogue ) rt.epilogue_done = TRUE;
			thread_free(t);
		}
	}
}

BOOL rt_ready( void )
{
	MSTH	*t;
	UD	now = ms_now_ms();

	for ( t = rt.threads; t != NULL; t = t->next ) {
		if ( t->wake <= now ) return TRUE;
	}
	return FALSE;
}

UD rt_next_wake( void )
{
	MSTH	*t;
	UD	w = ~(UD)0;

	for ( t = rt.threads; t != NULL; t = t->next ) {
		if ( t->wake < w ) w = t->wake;
	}
	return w;
}

/* A running statement lets the others go now and then even when it does not wait */
static void th_step( MSTH *t )
{
	if ( ++t->steps >= 4096 ) ms_yield(0);
}

void rt_start( void )
{
	MSPROC	*p = rt_proc_of_type(P_PROLOGUE);

	rt_bind_handlers();
	if ( p != NULL ) (void)rt_spawn(p, NULL, 0, FALSE);
}

/* The window is closing: the others stop, and the epilogue runs if there is one */
void rt_stop( void )
{
	MSTH	*t;
	MSPROC	*p = rt_proc_of_type(P_EPILOGUE);

	if ( rt.finished ) return;
	rt.finished = TRUE;
	for ( t = rt.threads; t != NULL; t = t->next ) t->terminated = TRUE;
	if ( p != NULL ) {
		(void)rt_spawn(p, NULL, 0, TRUE);
	} else {
		rt.epilogue_done = TRUE;
	}
}

BOOL rt_epilogue_done( void )
{
	return rt.epilogue_done && rt.threads == NULL;
}

/* ---------------------------------------------------------------- truth and names */

BOOL mv_truthy( MSV v )
{
	switch ( v.t ) {
	case V_NUM: return (BOOL)( v.u.n != 0 && !isnan(v.u.n) );
	case V_STR: return (BOOL)( v.u.s->len > 0 );
	case V_ARR: return (BOOL)( v.u.a->n > 0 && mv_n(v.u.a->e[0]) != 0 );
	}
	return FALSE;
}

/* The name a value gives: a name as it is, the characters of an array */
MSSTR *mv_name( MSV v )
{
	MSBUF	b;
	MSSTR	*s;

	if ( v.t == V_STR ) return v.u.s;
	if ( v.t == V_ARR ) {
		mb_init(&b);
		mb_putn(&b, "", 0);
		mv_text(v, &b);
		s = ms_intern(b.s, b.n);
		mb_free(&b);
		return s;
	}
	return NULL;
}

/* A segment named by an expression */
MSSEG *th_seg( MSTH *t, MSN *e, MSSCOPE *sc )
{
	MSV	v = ev(t, e, sc);
	MSSTR	*n = mv_name(v);

	mv_drop(v);
	return ( n != NULL ) ? st_seg(n) : NULL;
}

/* ---------------------------------------------------------------- expressions */

static BOOL expr_float( MSN *n, MSSCOPE *sc )
{
	if ( n == NULL ) return FALSE;
	switch ( n->k ) {
	case N_NUM: return (BOOL)( ( n->flags & NF_FLOAT ) != 0 );
	case N_NAME: {
		MSVAR	*v = scope_find(sc, n->name);

		return (BOOL)( v != NULL && v->type == 'F' );
	}
	case N_UNARY: return expr_float(n->a, sc);
	case N_BIN: return (BOOL)( expr_float(n->a, sc) || expr_float(n->b, sc) );
	case N_CALL: {
		MSSTR	*u = ms_upper(n->name);

		return (BOOL)( strcmp(u->s, "SIN") == 0 || strcmp(u->s, "COS") == 0 || strcmp(u->s, "TAN") == 0
			    || strcmp(u->s, "ATAN") == 0 || strcmp(u->s, "ASIN") == 0 || strcmp(u->s, "ACOS") == 0
			    || strcmp(u->s, "SQRT") == 0 || strcmp(u->s, "EXP") == 0 || strcmp(u->s, "LOG") == 0 );
	}
	}
	return FALSE;
}

static BOOL same( MSV l, MSV r )
{
	if ( l.t != r.t ) return FALSE;
	switch ( l.t ) {
	case V_NUM: return (BOOL)( l.u.n == r.u.n );
	case V_STR: return (BOOL)( l.u.s == r.u.s );
	case V_ARR: return (BOOL)( l.u.a == r.u.a );
	}
	return FALSE;
}

/* Two values ordered: names by their text, numbers by value; neither when one is not a number */
static INT compare( MSV l, MSV r, BOOL *p_ok )
{
	*p_ok = TRUE;
	if ( l.t == V_STR && r.t == V_STR ) {
		INT	c = strcmp(l.u.s->s, r.u.s->s);

		return ( c < 0 ) ? -1 : ( c > 0 ) ? 1 : 0;
	}
	{
		double	a = mv_n(l), b = mv_n(r);

		if ( l.t == V_STR || r.t == V_STR || isnan(a) || isnan(b) ) {
			*p_ok = FALSE;
			return 0;
		}
		return ( a < b ) ? -1 : ( a > b ) ? 1 : 0;
	}
}

static MSV ev_binary( MSTH *t, MSN *n, MSSCOPE *sc )
{
	MSV	l, r, out;
	double	a, b;
	BOOL	ok;
	INT	c;

	if ( n->op == OPC('&', '&') ) {
		l = ev(t, n->a, sc);
		if ( !mv_truthy(l) ) { mv_drop(l); return mv_num(0); }
		mv_drop(l);
		r = ev(t, n->b, sc);
		out = mv_num(mv_truthy(r) ? 1 : 0);
		mv_drop(r);
		return out;
	}
	if ( n->op == OPC('|', '|') ) {
		l = ev(t, n->a, sc);
		if ( mv_truthy(l) ) { mv_drop(l); return mv_num(1); }
		mv_drop(l);
		r = ev(t, n->b, sc);
		out = mv_num(mv_truthy(r) ? 1 : 0);
		mv_drop(r);
		return out;
	}
	l = ev(t, n->a, sc);
	r = ev(t, n->b, sc);
	if ( l.t == V_NUM && r.t == V_NUM && ( l.u.n == MS_INVALID || r.u.n == MS_INVALID )
	  && n->op != '=' && n->op != OPC('=', '=') && n->op != OPC('!', '=') ) {
		return mv_num(MS_INVALID);
	}
	/* a name joined with something: the names as text, joined */
	if ( n->op == '+' && ( l.t == V_STR || r.t == V_STR ) && l.t != V_ARR && r.t != V_ARR ) {
		MSBUF	bb;

		mb_init(&bb);
		mb_putn(&bb, "", 0);
		mv_text(l, &bb);
		mv_text(r, &bb);
		out = mv_str(ms_intern(bb.s, bb.n));
		mb_free(&bb);
		return out;
	}
	switch ( n->op ) {
	case '=': case OPC('=', '='):
		out = mv_num(same(l, r) ? 1 : 0);
		break;
	case OPC('!', '='):
		out = mv_num(same(l, r) ? 0 : 1);
		break;
	case '<':
		c = compare(l, r, &ok);
		out = mv_num(( ok && c < 0 ) ? 1 : 0);
		break;
	case OPC('<', '='):
		c = compare(l, r, &ok);
		out = mv_num(( ok && c <= 0 ) ? 1 : 0);
		break;
	case '>':
		c = compare(l, r, &ok);
		out = mv_num(( ok && c > 0 ) ? 1 : 0);
		break;
	case OPC('>', '='):
		c = compare(l, r, &ok);
		out = mv_num(( ok && c >= 0 ) ? 1 : 0);
		break;
	default:
		a = mv_n(l);
		b = mv_n(r);
		switch ( n->op ) {
		case '+': out = mv_num(a + b); break;
		case '-': out = mv_num(a - b); break;
		case '*': out = mv_num(a * b); break;
		case '/':
			if ( b == 0 ) { out = mv_num(( a >= 0 ) ? 2147483647.0 : -2147483648.0); break; }
			if ( expr_float(n->a, sc) || expr_float(n->b, sc) ) { out = mv_num(a / b); break; }
			out = mv_num(( a == trunc(a) && b == trunc(b) ) ? trunc(a / b) : a / b);
			break;
		case '%': {
			double	li = trunc(a), ri = trunc(b);

			if ( ri == 0 ) { out = mv_num(( li >= 0 ) ? 2147483647.0 : -2147483648.0); break; }
			out = mv_num(li - trunc(li / ri) * ri);
			break;
		}
		case '&': out = mv_num(ms_int32(a) & ms_int32(b)); break;
		case '|': out = mv_num(ms_int32(a) | ms_int32(b)); break;
		case '^': out = mv_num(ms_int32(a) ^ ms_int32(b)); break;
		case OPC('<', '<'): out = mv_num((double)(INT)( (UW)ms_int32(a) << ( ms_int32(b) & 31 ) )); break;
		case OPC('>', '>'): out = mv_num(ms_int32(a) >> ( ms_int32(b) & 31 )); break;
		default: out = mv_num(0); break;
		}
	}
	mv_drop(l);
	mv_drop(r);
	return out;
}

/* An element or slice index as the script means it: an integer */
static INT ev_int( MSTH *t, MSN *n, MSSCOPE *sc )
{
	MSV	v = ev(t, n, sc);
	INT	i = mv_i(v);

	mv_drop(v);
	return i;
}

static MSV slice_of_arr( MSV src, INT st, INT ln )
{
	MSV	out;
	INT	i, n = ( src.t == V_ARR ) ? src.u.a->n : 0;

	if ( st < 0 ) st = 0;
	if ( ln < 0 ) ln = 0;
	if ( st > n ) st = n;
	if ( st + ln > n ) ln = n - st;
	out = mv_arr(ln);
	for ( i = 0; i < ln; i++ ) out.u.a->e[i] = mv_ref(src.u.a->e[st + i]);
	return out;
}

static MSV ev_name( MSTH *t, MSN *n, MSSCOPE *sc )
{
	MSVAR	*v;

	if ( n->name2 != NULL ) {
		/* NAME.STATE: a segment's state, or a procedure's */
		MSSEG	*s = st_seg(n->name);

		if ( s == NULL && ( v = scope_find(sc, n->name) ) != NULL ) {
			INT	idx = ( n->a != NULL ) ? ev_int(t, n->a, sc) : 0;
			MSV	sv = var_get(v, idx);
			MSSTR	*nm = ( sv.t == V_STR ) ? sv.u.s : NULL;

			mv_drop(sv);
			if ( nm != NULL ) s = st_seg(nm);
		}
		if ( s != NULL ) {
			MSV	sv = seg_state(s, n->name2);

			if ( sv.t == V_ARR ) {
				if ( n->flags & NF_SSLICE ) {
					INT	st = ( n->c != NULL ) ? ev_int(t, n->c, sc) : 0;
					INT	ln = ( n->d != NULL ) ? ev_int(t, n->d, sc) : sv.u.a->n - st;
					MSV	out = slice_of_arr(sv, st, ln);

					mv_drop(sv);
					return out;
				}
				if ( n->c != NULL ) {
					INT	ix = ev_int(t, n->c, sc);
					MSV	out = ( ix < 0 || ix >= sv.u.a->n ) ? mv_num(MS_INVALID)
						      : mv_ref(sv.u.a->e[ix]);

					mv_drop(sv);
					return out;
				}
			}
			return sv;
		}
		if ( strcmp(ms_upper(n->name2)->s, "S") == 0 && rt_proc(n->name) != NULL ) {
			MSTH	*q;
			INT	state = 0;

			for ( q = rt.threads; q != NULL; q = q->next ) {
				if ( q->name == n->name && !q->terminated ) {
					state = q->waiting ? 2 : ( state == 2 ? 2 : 1 );
				}
			}
			return mv_num(state);
		}
		return mv_num(0);
	}
	if ( n->a == NULL && !( n->flags & NF_SLICE ) && st_seg(n->name) != NULL ) {
		return mv_str(n->name);
	}
	if ( n->a != NULL && !( n->flags & NF_SLICE ) ) {
		/* an element of an array of segments: its name */
		MSVAR	*v0 = scope_find(sc, n->name);

		if ( v0 != NULL && ( v0->segref || v0->type == 'S' ) ) {
			INT	idx = ev_int(t, n->a, sc);
			MSV	e = var_get(v0, idx);

			if ( e.t == V_STR ) return e;
			mv_drop(e);
		}
	}
	v = scope_find(sc, n->name);
	if ( v == NULL ) {
		MSMACRO	*m = ms_macro(&ms_prog, n->name);

		if ( m != NULL ) {
			if ( m->nbody == 1 ) {
				if ( m->body[0].type == T_INT || m->body[0].type == T_FLOAT ) return mv_num(m->body[0].num);
				if ( m->body[0].type == T_IDENT ) {
					MSN	x;

					memset(&x, 0, sizeof(x));
					x.k = N_NAME;
					x.name = m->body[0].name;
					return ev(t, &x, sc);
				}
			}
			if ( !m->tried ) {
				m->tried = TRUE;
				m->expr = ms_parse_expr_toks(m->body, m->nbody);
			}
			return ( m->expr != NULL ) ? ev(t, m->expr, sc) : mv_num(0);
		}
		return mv_str(n->name);
	}
	if ( n->flags & NF_SLICE ) {
		INT	st = ( n->a != NULL ) ? ev_int(t, n->a, sc) : 0;
		INT	ln = ( n->b != NULL ) ? ev_int(t, n->b, sc) : v->size - st;
		MSV	out;
		INT	i;

		if ( st < 0 ) { ln += st; st = 0; }
		if ( st > v->size ) st = v->size;
		if ( ln < 0 ) ln = 0;
		if ( st + ln > v->size ) ln = v->size - st;
		out = mv_arr(ln);
		for ( i = 0; i < ln; i++ ) out.u.a->e[i] = var_get(v, st + i);
		return out;
	}
	if ( n->a != NULL ) {
		INT	idx = ev_int(t, n->a, sc);

		if ( idx < 0 || idx >= v->size ) return mv_num(MS_INVALID);
		return var_get(v, idx);
	}
	return var_get(v, 0);
}

MSV ev( MSTH *t, MSN *n, MSSCOPE *sc )
{
	if ( n == NULL ) return mv_num(0);
	th_step(t);
	switch ( n->k ) {
	case N_NUM:
		return mv_num(n->num);
	case N_STR:
		return mv_chars(n->chars, n->nchars);
	case N_UNARY: {
		MSV	v = ev(t, n->a, sc), out;

		switch ( n->op ) {
		case '-': out = mv_num(-mv_n(v)); break;
		case '+': out = mv_num(mv_n(v)); break;
		case '!': out = mv_num(mv_truthy(v) ? 0 : 1); break;
		case '~': out = mv_num(~mv_i(v)); break;
		default: out = mv_ref(v); break;
		}
		mv_drop(v);
		return out;
	}
	case N_BIN:
		return ev_binary(t, n, sc);
	case N_SYSVAR: {
		MSV	iv = ( n->a != NULL ) ? ev(t, n->a, sc) : mv_num(0);
		MSV	out = sysvar_get(t, n->name, iv, sc);

		mv_drop(iv);
		return out;
	}
	case N_CALL:
		return call_builtin(t, n, sc);
	case N_NAME:
		return ev_name(t, n, sc);
	}
	return mv_num(0);
}

/* ---------------------------------------------------------------- statements */

static INT exec_stmt( MSTH *t, MSN *s, MSSCOPE *sc );

INT exec_block( MSTH *t, MSN **v, INT n, MSSCOPE *sc )
{
	INT	i, f;

	for ( i = 0; i < n; i++ ) {
		if ( th_stop(t) ) return FL_STOP;
		f = exec_stmt(t, v[i], sc);
		if ( t->err ) return FL_STOP;
		if ( f != FL_NONE ) return f;
	}
	return FL_NONE;
}

static INT exec_body( MSTH *t, MSN *b, MSSCOPE *sc )
{
	return ( b != NULL ) ? exec_block(t, b->v, b->nv, sc) : FL_NONE;
}

/* A value put into a place the script names */
void assign( MSTH *t, MSN *target, MSV *vals, INT nvals, MSSCOPE *sc )
{
	MSVAR	*v;

	if ( target->k == N_SYSVAR ) {
		MSV	iv = ( target->a != NULL ) ? ev(t, target->a, sc) : mv_num(0);

		sysvar_set(t, target->name, iv, vals[0], sc);
		mv_drop(iv);
		return;
	}
	if ( target->name2 != NULL ) {
		MSSEG	*s = st_seg(target->name);

		if ( s == NULL && ( v = scope_find(sc, target->name) ) != NULL ) {
			MSV	e = var_get(v, 0);

			if ( e.t == V_STR ) s = st_seg(e.u.s);
			mv_drop(e);
		}
		if ( s != NULL ) seg_state_set(s, target->name2, vals[0]);
		return;
	}
	v = scope_find(sc, target->name);
	if ( v == NULL ) {
		th_fail(t, "未定義の変数: %s", target->name->s);
		return;
	}
	if ( target->flags & NF_SLICE ) {
		INT	st = ( target->a != NULL ) ? ev_int(t, target->a, sc) : 0;
		INT	ln = ( target->b != NULL ) ? ev_int(t, target->b, sc) : v->size - st;
		MSV	*flat;
		INT	nf = 0, cap = 16, i, k;

		/* the values laid out one after another: arrays give their elements */
		flat = ms_alloc(sizeof(MSV) * (size_t)cap);
		for ( k = 0; k < nvals; k++ ) {
			if ( vals[k].t == V_ARR ) {
				for ( i = 0; i < vals[k].u.a->n; i++ ) {
					if ( nf >= cap ) { cap *= 2; flat = ms_realloc(flat, sizeof(MSV) * (size_t)cap); }
					flat[nf++] = vals[k].u.a->e[i];
				}
			} else {
				if ( nf >= cap ) { cap *= 2; flat = ms_realloc(flat, sizeof(MSV) * (size_t)cap); }
				flat[nf++] = vals[k];
			}
		}
		for ( i = 0; i < ln; i++ ) {
			INT	idx = st + i;
			MSV	src;

			if ( idx < 0 || idx >= v->size ) break;
			src = ( i < nf ) ? flat[i] : ( nf > 0 ? flat[nf - 1] : mv_num(0) );
			var_set(v, idx, src);
		}
		ms_free(flat);
		return;
	}
	if ( target->a != NULL ) {
		INT	idx = ev_int(t, target->a, sc);
		MSV	val = vals[0];

		if ( idx >= 0 && idx < v->size ) {
			/* several segments of one name handed out in turn to an array of segments */
			if ( ( v->segref || v->type == 'S' ) && val.t == V_STR && st_seg_count(val.u.s) > 1 ) {
				INT	occ = seg_consume(val.u.s);
				MSSEG	*s = st_seg_occ(val.u.s, occ);

				if ( s != NULL ) val = mv_str(s->key);
			}
			var_set(v, idx, val);
		}
		return;
	}
	var_set(v, 0, vals[0]);
}

static INT st_set( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSV	*vals = ms_alloc(sizeof(MSV) * (size_t)( s->nv + 1 ));
	INT	i;

	for ( i = 0; i < s->nv; i++ ) vals[i] = ev(t, s->v[i], sc);
	assign(t, s->a, vals, s->nv, sc);
	for ( i = 0; i < s->nv; i++ ) mv_drop(vals[i]);
	ms_free(vals);
	return FL_NONE;
}

static INT st_while( MSTH *t, MSN *s, MSSCOPE *sc )
{
	while ( !th_stop(t) ) {
		MSV	c = ev(t, s->a, sc);
		BOOL	go = mv_truthy(c);
		INT	f;

		mv_drop(c);
		if ( !go ) break;
		ms_yield(0);
		if ( th_stop(t) ) break;
		f = exec_body(t, s->b, sc);
		if ( f == FL_BREAK ) break;
		if ( f == FL_CONTINUE ) continue;
		if ( f != FL_NONE ) return f;
	}
	return FL_NONE;
}

static INT st_repeat( MSTH *t, MSN *s, MSSCOPE *sc )
{
	double	count = -1, prev = t->cnt;
	INT	f = FL_NONE;

	if ( s->a != NULL ) {
		MSV	c = ev(t, s->a, sc);

		count = mv_n(c);
		mv_drop(c);
	}
	t->cnt = 0;
	while ( !th_stop(t) ) {
		INT	r;

		if ( count >= 0 && t->cnt >= count ) break;
		if ( ( ms_int32(t->cnt) & 0xFF ) == 0 ) ms_yield(0);
		if ( th_stop(t) ) break;
		r = exec_body(t, s->b, sc);
		if ( r == FL_BREAK ) break;
		if ( r == FL_CONTINUE ) { t->cnt++; continue; }
		if ( r != FL_NONE ) { f = r; break; }
		t->cnt++;
	}
	t->cnt = prev;
	return f;
}

static INT st_switch( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSV	base = ev(t, s->a, sc);
	BOOL	matched = FALSE;
	INT	i, f;

	for ( i = 0; i < s->nv; i++ ) {
		if ( !matched ) {
			MSV	c = ev(t, s->v[i], sc);

			matched = same(c, base);
			mv_drop(c);
		}
		if ( matched ) {
			f = exec_body(t, s->w[i], sc);
			if ( f == FL_BREAK ) { mv_drop(base); return FL_NONE; }
			if ( f != FL_NONE ) { mv_drop(base); return f; }
		}
	}
	mv_drop(base);
	if ( s->b != NULL ) {
		f = exec_body(t, s->b, sc);
		if ( f == FL_BREAK ) return FL_NONE;
		return f;
	}
	return FL_NONE;
}

/* The name of a procedure an expression gives */
static MSSTR *proc_name_of( MSTH *t, MSN *e, MSSCOPE *sc )
{
	MSV	v;
	MSSTR	*n;

	if ( e->k == N_NAME && e->a == NULL && e->name2 == NULL ) return e->name;
	v = ev(t, e, sc);
	n = mv_name(v);
	mv_drop(v);
	return n;
}

/* A procedure named directly, through a variable holding its name, or an element of one */
static MSPROC *proc_by( MSTH *t, MSSTR *name, MSN *index, MSSCOPE *sc )
{
	MSPROC	*p;
	MSVAR	*hv;

	if ( index != NULL ) {
		hv = scope_find(sc, name);
		if ( hv != NULL ) {
			INT	ix = ev_int(t, index, sc);
			MSV	e = var_get(hv, ix);

			if ( e.t == V_STR ) name = e.u.s;
			mv_drop(e);
		}
	}
	p = rt_proc(name);
	if ( p == NULL && ( hv = scope_find(sc, name) ) != NULL ) {
		MSV	e = var_get(hv, 0);

		if ( e.t == V_STR ) p = rt_proc(e.u.s);
		mv_drop(e);
	}
	return p;
}

/*
 * A procedure called, in this thread: its parameters in a scope of their
 * own over the global one. An array given without an index is handed
 * over as it is, so that what the procedure does to it is seen.
 */
MSV call_proc( MSTH *t, MSPROC *p, MSN **argn, INT nargn, MSSCOPE *sc )
{
	MSSCOPE	*cs;
	MSVAR	**arrv;
	INT	i, f;
	MSV	ret = mv_num(0);

	{
		/* the room left on the thread's stack: a call goes no deeper than it allows */
		volatile UB	probe = 0;
		UBINT		left = (UBINT)&probe - (UBINT)t->stack;

		if ( left < MS_STACK_SPARE ) {
			th_fail(t, "手続きの呼び出しが深すぎます: %s", p->name ? p->name->s : "?");
			return ret;
		}
	}
	if ( ++t->depth > MS_MAX_DEPTH ) {
		t->depth--;
		th_fail(t, "手続きの呼び出しが深すぎます: %s", p->name ? p->name->s : "?");
		return ret;
	}
	cs = scope_new(rt.global);
	cs->hasargs = TRUE;
	cs->args = ms_alloc(sizeof(MSV) * (size_t)( nargn + 1 ));
	arrv = ms_alloc(sizeof(MSVAR *) * (size_t)( nargn + 1 ));
	for ( i = 0; i < nargn; i++ ) {
		MSN	*an = argn[i];

		if ( an != NULL && an->k == N_NAME && an->a == NULL && !( an->flags & NF_SLICE ) && an->name2 == NULL ) {
			MSVAR	*vv = scope_find(sc, an->name);

			if ( vv != NULL && vv->size > 1 ) arrv[i] = vv;
		}
		cs->args[i] = ( arrv[i] != NULL ) ? var_get(arrv[i], 0) : ev(t, an, sc);
	}
	cs->nargs = nargn;
	for ( i = 0; i < p->nparam; i++ ) {
		MSDECL	*d = &p->param[i];
		INT	size = d->open ? 0 : (INT)eval_const(d->size);

		if ( ( d->open || size > 1 ) && i < nargn && arrv[i] != NULL ) {
			scope_bind_alias(cs, d->name, d->type, arrv[i]);
		} else {
			MSVAR	*v = scope_declare(cs, d, size, 0, size > 1, FALSE);

			if ( i < nargn ) var_set(v, 0, cs->args[i]);
		}
	}
	ms_free(arrv);
	f = exec_block(t, p->body, p->nbody, cs);
	if ( f == FL_EXIT ) {
		ret = t->exitval;
		t->exitval = mv_num(0);
	}
	scope_free(cs);
	t->depth--;
	return ret;
}

static INT st_call( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSPROC	*p = proc_by(t, s->name, s->b, sc);
	MSV	r;

	if ( p == NULL ) {
		th_fail(t, "未定義の手続き: %s", s->name->s);
		return FL_STOP;
	}
	r = call_proc(t, p, s->v, s->nv, sc);
	mv_drop(r);
	return FL_NONE;
}

static INT st_execute( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	i, j, last = ms_int32(MS_INVALID);

	for ( i = 0; i < s->nv; i++ ) {
		MSN	*h = s->v[i];
		MSPROC	*p = proc_by(t, h->name, h->b, sc);
		MSV	*args;

		if ( p == NULL ) continue;
		args = ms_alloc(sizeof(MSV) * (size_t)( h->nv + 1 ));
		for ( j = 0; j < h->nv; j++ ) args[j] = ev(t, h->v[j], sc);
		(void)rt_spawn(p, args, h->nv, FALSE);
		last = rt_last_spawned();
		for ( j = 0; j < h->nv; j++ ) mv_drop(args[j]);
		ms_free(args);
	}
	if ( s->name2 != NULL ) {
		MSVAR	*v = scope_find(sc, s->name2);

		if ( v != NULL ) var_set(v, 0, mv_num(last));
	}
	return FL_NONE;
}

/* Waiting: SLEEP, WAIT and the like come back here each time round */
BOOL th_wait_round( MSTH *t, UD step_ms )
{
	t->waiting = TRUE;
	ms_yield(ms_now_ms() + step_ms);
	t->waiting = FALSE;
	return (BOOL)!th_stop(t);
}

static INT st_sleep( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSV	v = ev(t, s->a, sc);
	double	ms = mv_n(v);
	UD	start = ms_now_ms(), end;

	mv_drop(v);
	if ( !isfinite(ms) ) ms = 0;
	if ( ms < 0 ) ms = 0;
	if ( ms > 86400000.0 ) ms = 86400000.0;
	end = start + (UD)ms;
	t->waiting = TRUE;
	while ( !th_stop(t) ) {
		UD	now;

		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
		now = ms_now_ms();
		if ( now >= end ) { rt.lasterr = 0; break; }
		ms_yield(( end - now > 50 ) ? now + 50 : end);
	}
	t->waiting = FALSE;
	return FL_NONE;
}

static INT st_wait( MSTH *t, MSN *s, MSSCOPE *sc )
{
	double	tmo = -1;
	UD	start = ms_now_ms();

	if ( s->b != NULL ) {
		MSV	v = ev(t, s->b, sc);

		tmo = mv_n(v);
		mv_drop(v);
	}
	t->waiting = TRUE;
	while ( !th_stop(t) ) {
		MSV	c = ev(t, s->a, sc);
		BOOL	ok = mv_truthy(c);

		mv_drop(c);
		if ( ok ) { rt.lasterr = 0; break; }
		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
		if ( tmo == 0 ) { rt.lasterr = 1; break; }
		if ( tmo > 0 && (double)( ms_now_ms() - start ) >= tmo * 1000 ) { rt.lasterr = 1; break; }
		ms_yield(ms_now_ms() + 50);
	}
	t->waiting = FALSE;
	return FL_NONE;
}

static INT st_break( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	i;

	if ( s->nv == 0 ) return FL_BREAK;
	/* BREAK procedure...: those procedures stop waiting */
	for ( i = 0; i < s->nv; i++ ) {
		MSSTR	*nm = proc_name_of(t, s->v[i], sc);
		MSTH	*q;

		for ( q = rt.threads; q != NULL; q = q->next ) {
			if ( nm != NULL && q->name == nm ) q->brk = TRUE;
		}
	}
	return FL_NONE;
}

static BOOL thread_matches( MSTH *q, MSV v )
{
	if ( v.t == V_NUM ) return (BOOL)( (double)q->id == v.u.n );
	{
		MSSTR	*n = mv_name(v);

		return (BOOL)( n != NULL && q->name == n );
	}
}

static INT st_terminate( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	i;
	MSTH	*q;

	if ( s->nv == 0 ) {
		for ( q = rt.threads; q != NULL; q = q->next ) if ( q != t ) q->terminated = TRUE;
		return FL_NONE;
	}
	for ( i = 0; i < s->nv; i++ ) {
		MSV	v = ev(t, s->v[i], sc);

		for ( q = rt.threads; q != NULL; q = q->next ) {
			if ( thread_matches(q, v) ) q->terminated = TRUE;
		}
		mv_drop(v);
	}
	return th_stop(t) ? FL_STOP : FL_NONE;
}

static INT st_suspend( MSTH *t, MSN *s, MSSCOPE *sc )
{
	double	tmo = -1;
	INT	ids[MS_MAX_THREADS], n = 0, i;
	MSTH	*q;
	UD	start = ms_now_ms();

	if ( s->a != NULL ) {
		MSV	v = ev(t, s->a, sc);

		tmo = mv_n(v);
		mv_drop(v);
	}
	if ( s->nv == 0 ) {
		for ( q = rt.threads; q != NULL && n < MS_MAX_THREADS; q = q->next ) {
			if ( q != t ) ids[n++] = q->id;
		}
	} else {
		for ( i = 0; i < s->nv; i++ ) {
			MSV	v = ev(t, s->v[i], sc);

			for ( q = rt.threads; q != NULL && n < MS_MAX_THREADS; q = q->next ) {
				if ( thread_matches(q, v) ) ids[n++] = q->id;
			}
			mv_drop(v);
		}
	}
	t->waiting = TRUE;
	while ( !th_stop(t) ) {
		BOOL	alive = FALSE;

		for ( q = rt.threads; q != NULL && !alive; q = q->next ) {
			for ( i = 0; i < n; i++ ) {
				if ( q->id == ids[i] && !q->terminated && !q->done ) { alive = TRUE; break; }
			}
		}
		if ( !alive ) { rt.lasterr = 0; break; }
		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 1; break; }
		if ( tmo == 0 ) { rt.lasterr = 2; break; }
		if ( tmo > 0 && (double)( ms_now_ms() - start ) >= tmo * 1000 ) { rt.lasterr = 2; break; }
		ms_yield(ms_now_ms() + 50);
	}
	t->waiting = FALSE;
	return FL_NONE;
}

static INT st_local( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	i;

	for ( i = 0; i < s->ndecl; i++ ) {
		MSDECL	*d = &s->decl[i];
		INT	size = 1;

		if ( d->size != NULL ) size = ev_int(t, d->size, sc);
		scope_declare(sc, d, size, 0, size > 1, TRUE);
	}
	return FL_NONE;
}

static INT exec_stmt( MSTH *t, MSN *s, MSSCOPE *sc )
{
	th_step(t);
	switch ( s->k ) {
	case S_LOCAL:		return st_local(t, s, sc);
	case S_SET:		return st_set(t, s, sc);
	case S_IF: {
		INT	i;

		for ( i = 0; i < s->nv; i++ ) {
			MSV	c = ev(t, s->v[i], sc);
			BOOL	go = mv_truthy(c);

			mv_drop(c);
			if ( go ) return exec_body(t, s->w[i], sc);
		}
		return exec_body(t, s->a, sc);
	}
	case S_WHILE:		return st_while(t, s, sc);
	case S_REPEAT:		return st_repeat(t, s, sc);
	case S_SWITCH:		return st_switch(t, s, sc);
	case S_BREAK:		return st_break(t, s, sc);
	case S_CONTINUE:	return FL_CONTINUE;
	case S_EXIT:
		mv_drop(t->exitval);
		t->exitval = ( s->a != NULL ) ? ev(t, s->a, sc) : mv_num(0);
		return FL_EXIT;
	case S_CALL:		return st_call(t, s, sc);
	case S_EXECUTE:		return st_execute(t, s, sc);
	case S_FINISH:
		rt_stop();
		return FL_STOP;
	case S_TERMINATE:	return st_terminate(t, s, sc);
	case S_SUSPEND:		return st_suspend(t, s, sc);
	case S_SLEEP:		return st_sleep(t, s, sc);
	case S_WAIT:		return st_wait(t, s, sc);
	default:
		return exec_other(t, s, sc);
	}
}
