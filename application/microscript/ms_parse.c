/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_parse.c
 *	The tokens of the script read into a tree
 *
 *	The statements are the ones of the BTRON micro script, version 3.
 *	A DEFINE is a macro of tokens: a name defined so is replaced by its
 *	tokens wherever it comes after the definition. Arguments may be
 *	separated by spaces as well as by commas; in such a list a '-' with
 *	a space before it and a number right after it starts the next
 *	argument rather than subtracting, and a name with a space before its
 *	'(' is not a call.
 */

#include "ms.h"
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

#define N_BLOCK		0xFFFF		/* a list of statements, in v */

/* The tokens by address: a macro put in place moves addresses, not tokens */
typedef struct {
	MSTOK	**t;
	INT	n, cap;
} TKV;

typedef struct {
	TKV	*tk;
	INT	pos;
	MSPROG	*prog;
	BOOL	noexp;
	BOOL	argmode;
	jmp_buf	jb;
	char	*err;
	INT	errmax;
} PS;

static const char *const keywords[] = {
	"VERSION", "DEFINE", "VARIABLE", "LOCAL", "SEGMENT", "SCRIPT", "COMMENT",
	"PROLOGUE", "EPILOGUE", "ACTION", "MACTION", "FUNC", "END",
	"IF", "ELSEIF", "ELSE", "ENDIF",
	"SWITCH", "CASE", "DEFAULT", "ENDCASE",
	"REPEAT", "ENDREPEAT", "WHILE", "ENDWHILE",
	"BREAK", "CONTINUE",
	"SET", "SETSEG", "COPYSEG",
	"CALL", "EXECUTE", "EXIT", "FINISH", "TERMINATE", "SUSPEND", "SLEEP", "WAIT",
	"SCENE", "APPEAR", "DISAPPEAR", "MOVE", "BEEP",
	"WSIZE", "WMOVE", "WSAVE", "FULLWIND", "UPDATE",
	"MESG", "TEXT", "INPUT", "KINPUT",
	"VOPEN", "VCLOSE", "VWAIT",
	"PRESS", "CLICK", "DCLICK", "QPRESS", "MENU", "KEY",
	"DUP",
	"EVENT",
	"FOPEN", "FCLOSE", "FREAD", "FWRITE",
	"PROCESS", "PWAIT", "MSEND", "MRECV",
	"DOPEN", "DCLOSE", "DREAD", "DWRITE",
	"RSINIT", "RSPUTC", "RSPUT", "RSPUTN", "RSWAIT",
	"RSGETN", "RSGET", "RSGETC", "RSCNTL",
	"KEYD", "KEYU", "KEYC", "BUTD", "BUTU", "BUTC",
	"CONSOLE", "SPRINTF", "HTTPREQ", "HTTPHDR", "JSONGET", "JSONLEN",
	"TCPOPEN", "TCPSEND", "TCPWAIT", "TCPRECV", "TCPCLOSE",
	NULL
};

static const char *const boundaries[] = {
	"END", "ELSEIF", "ELSE", "ENDIF",
	"CASE", "DEFAULT", "ENDCASE",
	"ENDREPEAT", "ENDWHILE",
	"PROLOGUE", "EPILOGUE", "ACTION", "MACTION", "FUNC",
	"VERSION", "DEFINE", "VARIABLE", "SEGMENT", "SCRIPT", "LOCAL", "COMMENT",
	"DEBUG", "LOG",
	"PRESS", "CLICK", "DCLICK", "QPRESS", "MENU", "KEY",
	"BREAK", "CONTINUE",
	NULL
};

static BOOL in_list( MSSTR *name, const char *const *list )
{
	MSSTR	*u = ms_upper(name);
	INT	i;

	for ( i = 0; list[i] != NULL; i++ ) {
		if ( strcmp(u->s, list[i]) == 0 ) return TRUE;
	}
	return FALSE;
}

static BOOL is_keyword( MSSTR *n ) { return in_list(n, keywords); }
static BOOL is_boundary( MSSTR *n ) { return in_list(n, boundaries); }

static BOOL upper_is( MSSTR *n, const char *kw )
{
	return (BOOL)( n != NULL && strcmp(ms_upper(n)->s, kw) == 0 );
}

static void fail( PS *p, const MSTOK *t, const char *msg, const char *what )
{
	char	w[80];

	w[0] = 0;
	if ( t != NULL ) {
		if ( t->type == T_IDENT || t->type == T_SYSVAR ) snprintf(w, sizeof(w), "\"%s\"", t->name->s);
		else if ( t->type == T_OP ) {
			if ( t->op > 0xFF ) snprintf(w, sizeof(w), "\"%c%c\"", (char)( t->op >> 8 ), (char)( t->op & 0xFF ));
			else snprintf(w, sizeof(w), "\"%c\"", (char)t->op);
		} else if ( t->type == T_NL ) snprintf(w, sizeof(w), "改段落");
		else if ( t->type == T_EOF ) snprintf(w, sizeof(w), "終端");
		else if ( t->type == T_STRING ) snprintf(w, sizeof(w), "文字列");
		else snprintf(w, sizeof(w), "%g", t->num);
	}
	snprintf(p->err, (size_t)p->errmax, "構文エラー(%d行): %s%s%s",
		 ( t != NULL ) ? t->line : 0, msg, ( what != NULL ) ? what : "", w);
	longjmp(p->jb, 1);
}

/* ---------------------------------------------------------------- macros */

MSMACRO *ms_macro( MSPROG *prog, MSSTR *name )
{
	INT	i;

	for ( i = prog->nmacro - 1; i >= 0; i-- ) {
		if ( prog->macro[i].name == name ) return &prog->macro[i];
	}
	return NULL;
}

static void expand( PS *p, INT idx )
{
	INT	guard;

	if ( p->noexp || p->prog == NULL ) return;
	for ( guard = 0; guard < 16; guard++ ) {
		MSTOK	*t, orig;
		MSMACRO	*m;
		INT	k;

		if ( idx >= p->tk->n ) return;
		t = p->tk->t[idx];
		if ( t->type != T_IDENT ) return;
		m = ms_macro(p->prog, t->name);
		if ( m == NULL ) return;
		orig = *t;
		/* room for the body in place of the one token */
		if ( p->tk->n + m->nbody > p->tk->cap ) {
			p->tk->cap = p->tk->n + m->nbody + 256;
			p->tk->t = ms_realloc(p->tk->t, sizeof(MSTOK *) * (size_t)p->tk->cap);
		}
		memmove(&p->tk->t[idx + m->nbody], &p->tk->t[idx + 1],
			sizeof(MSTOK *) * (size_t)( p->tk->n - idx - 1 ));
		for ( k = 0; k < m->nbody; k++ ) {
			MSTOK	*c = ms_alloc(sizeof(MSTOK));

			*c = m->body[k];
			c->line = orig.line;
			if ( k == 0 ) c->spaced = orig.spaced;
			p->tk->t[idx + k] = c;
		}
		p->tk->n += m->nbody - 1;
	}
}

static MSTOK *peek( PS *p, INT off )
{
	INT	i = p->pos + off;

	expand(p, i);
	if ( i >= p->tk->n ) i = p->tk->n - 1;		/* the EOF */
	return p->tk->t[i];
}

static MSTOK *next( PS *p )
{
	MSTOK	*t;

	expand(p, p->pos);
	t = p->tk->t[( p->pos < p->tk->n ) ? p->pos : p->tk->n - 1];
	if ( p->pos < p->tk->n - 1 ) p->pos++;
	return t;
}

static BOOL is_op( MSTOK *t, UH op ) { return (BOOL)( t->type == T_OP && t->op == op ); }
static BOOL at_op( PS *p, UH op ) { return is_op(peek(p, 0), op); }
static BOOL at_end( PS *p )
{
	MSTOK	*t = peek(p, 0);

	return (BOOL)( t->type == T_NL || t->type == T_EOF );
}

static MSTOK *expect_type( PS *p, UB type, const char *what )
{
	MSTOK	*t = peek(p, 0);

	if ( t->type != type ) fail(p, t, what, ", 実際: ");
	return next(p);
}

static void expect_op( PS *p, UH op )
{
	MSTOK	*t = peek(p, 0);
	char	m[32];

	if ( !is_op(t, op) ) {
		if ( op > 0xFF ) snprintf(m, sizeof(m), "期待: %c%c, 実際: ", (char)( op >> 8 ), (char)op);
		else snprintf(m, sizeof(m), "期待: %c, 実際: ", (char)op);
		fail(p, t, m, NULL);
	}
	next(p);
}

static BOOL check_kw( PS *p, const char *kw )
{
	MSTOK	*t = peek(p, 0);

	return (BOOL)( t->type == T_IDENT && upper_is(t->name, kw) );
}

static void skip_nl( PS *p )
{
	while ( peek(p, 0)->type == T_NL ) next(p);
}

static void expect_nl( PS *p )
{
	MSTOK	*t = peek(p, 0);

	if ( t->type != T_NL && t->type != T_EOF ) fail(p, t, "文末(改段落)が必要: ", NULL);
	if ( t->type == T_NL ) next(p);
}

/* ---------------------------------------------------------------- nodes */

static MSN *node( UH k, INT line )
{
	MSN	*n = ms_alloc(sizeof(MSN));

	n->k = k;
	n->line = line;
	return n;
}

static void push( MSN ***v, INT *n, MSN *x )
{
	*v = ms_realloc(*v, sizeof(MSN *) * (size_t)( *n + 1 ));
	(*v)[(*n)++] = x;
}

static MSN *parse_expr( PS *p );

static MSN *sub_expr( PS *p )
{
	BOOL	s = p->argmode;
	MSN	*e;

	p->argmode = FALSE;
	e = parse_expr(p);
	p->argmode = s;
	return e;
}

/* name [ index | a:b | :b | a: ] */
static void index_or_slice( PS *p, MSN **pa, MSN **pb, BOOL *p_slice )
{
	*pa = *pb = NULL;
	*p_slice = FALSE;
	next(p);				/* [ */
	if ( at_op(p, ':') ) {
		next(p);
		*p_slice = TRUE;
		if ( !at_op(p, ']') ) *pb = sub_expr(p);
	} else {
		MSN	*first = sub_expr(p);

		if ( at_op(p, ':') ) {
			next(p);
			*p_slice = TRUE;
			*pa = first;
			if ( !at_op(p, ']') ) *pb = sub_expr(p);
		} else {
			*pa = first;
		}
	}
	expect_op(p, ']');
}

static MSN *parse_primary( PS *p )
{
	MSTOK	*t = peek(p, 0);
	MSN	*n;

	if ( t->type == T_INT || t->type == T_FLOAT ) {
		next(p);
		n = node(N_NUM, t->line);
		n->num = t->num;
		if ( t->type == T_FLOAT ) n->flags |= NF_FLOAT;
		return n;
	}
	if ( t->type == T_STRING ) {
		next(p);
		n = node(N_STR, t->line);
		n->chars = t->chars;
		n->nchars = t->nchars;
		return n;
	}
	if ( t->type == T_SYSVAR ) {
		next(p);
		n = node(N_SYSVAR, t->line);
		n->name = t->name;
		if ( at_op(p, '[') ) {
			next(p);
			n->a = sub_expr(p);
			expect_op(p, ']');
		}
		return n;
	}
	if ( is_op(t, '(') ) {
		next(p);
		n = sub_expr(p);
		expect_op(p, ')');
		return n;
	}
	if ( t->type == T_IDENT ) {
		MSTOK	*q;
		BOOL	sl;

		next(p);
		q = peek(p, 0);
		/* a call: always in an expression; in an argument list only without a space */
		if ( is_op(q, '(') && ( !q->spaced || !p->argmode ) ) {
			next(p);
			n = node(N_CALL, t->line);
			n->name = t->name;
			if ( !at_op(p, ')') ) {
				push(&n->v, &n->nv, parse_expr(p));
				while ( at_op(p, ',') ) {
					next(p);
					push(&n->v, &n->nv, parse_expr(p));
				}
			}
			expect_op(p, ')');
			return n;
		}
		n = node(N_NAME, t->line);
		n->name = t->name;
		if ( at_op(p, '[') ) {
			index_or_slice(p, &n->a, &n->b, &sl);
			if ( sl ) n->flags |= NF_SLICE;
		}
		if ( at_op(p, '.') ) {
			next(p);
			n->name2 = expect_type(p, T_IDENT, "期待: IDENT")->name;
			if ( at_op(p, '[') ) {
				index_or_slice(p, &n->c, &n->d, &sl);
				if ( sl ) n->flags |= NF_SSLICE;
			}
		}
		return n;
	}
	fail(p, t, "式が必要: ", NULL);
	return NULL;
}

static MSN *parse_unary( PS *p )
{
	MSTOK	*t = peek(p, 0);

	if ( t->type == T_OP && ( t->op == '-' || t->op == '!' || t->op == '~' || t->op == '+' ) ) {
		MSN	*n = node(N_UNARY, t->line);

		next(p);
		n->op = t->op;
		n->a = parse_unary(p);
		return n;
	}
	return parse_primary(p);
}

static INT prec_of( UH op )
{
	switch ( op ) {
	case OPC('|', '|'):				return 1;
	case OPC('&', '&'):				return 2;
	case '|':					return 3;
	case '^':					return 4;
	case '&':					return 5;
	case '=': case OPC('=', '='): case OPC('!', '='):	return 6;
	case '<': case OPC('<', '='): case '>': case OPC('>', '='):	return 7;
	case '+': case '-':				return 8;
	case OPC('<', '<'): case OPC('>', '>'):		return 9;
	case '*': case '/': case '%':			return 10;
	}
	return -1;
}

static MSN *parse_binary( PS *p, INT minprec )
{
	MSN	*lhs = parse_unary(p);

	for ( ;; ) {
		MSTOK	*t = peek(p, 0);
		INT	prec;
		MSN	*n;

		if ( t->type != T_OP ) break;
		prec = prec_of(t->op);
		if ( prec < 0 || prec < minprec ) break;
		if ( p->argmode && t->op == '-' && t->spaced ) {
			MSTOK	*nx = peek(p, 1);

			if ( !nx->spaced && ( nx->type == T_INT || nx->type == T_FLOAT ) ) break;
		}
		next(p);
		n = node(N_BIN, t->line);
		n->op = t->op;
		n->a = lhs;
		n->b = parse_binary(p, prec + 1);
		lhs = n;
	}
	return lhs;
}

static MSN *parse_expr( PS *p )
{
	return parse_binary(p, 0);
}

/* ---------------------------------------------------------------- statements */

static MSN *parse_block( PS *p, const char *const *ends );
static MSN *parse_statement( PS *p );

static BOOL is_end( MSSTR *name, const char *const *ends )
{
	INT	i;

	for ( i = 0; ends[i] != NULL; i++ ) {
		if ( upper_is(name, ends[i]) ) return TRUE;
	}
	return FALSE;
}

static MSN *parse_block( PS *p, const char *const *ends )
{
	MSN	*b = node(N_BLOCK, peek(p, 0)->line);

	for ( ;; ) {
		MSTOK	*t = peek(p, 0);
		MSN	*s;

		if ( t->type == T_EOF ) fail(p, t, "予期しないファイル終端、ブロック終端が必要", NULL);
		if ( t->type == T_NL ) { next(p); continue; }
		if ( t->type == T_IDENT && is_end(t->name, ends) ) {
			/* a variable of the same name at the head of an assignment is not the end */
			MSTOK	*n1 = peek(p, 1);

			if ( !( n1->type == T_OP && ( n1->op == '[' || n1->op == '=' || n1->op == '.' ) ) ) {
				return b;
			}
		}
		s = parse_statement(p);
		if ( s != NULL ) push(&b->v, &b->nv, s);
	}
}

static void skip_comment( PS *p )
{
	while ( !at_end(p) ) next(p);
	if ( peek(p, 0)->type == T_NL ) next(p);
}

static void parse_define( PS *p )
{
	MSTOK	*nt;
	MSMACRO	*m;
	INT	start, k;

	p->noexp = TRUE;
	next(p);
	nt = expect_type(p, T_IDENT, "期待: IDENT");
	start = p->pos;
	/* to the paragraph's end; a ';' is part of the body */
	while ( peek(p, 0)->type != T_EOF ) {
		MSTOK	*q = peek(p, 0);

		if ( q->type == T_NL && q->op != ';' ) break;
		next(p);
	}
	p->prog->macro = ms_realloc(p->prog->macro, sizeof(MSMACRO) * (size_t)( p->prog->nmacro + 1 ));
	m = &p->prog->macro[p->prog->nmacro++];
	memset(m, 0, sizeof(*m));
	m->name = nt->name;
	m->nbody = p->pos - start;
	m->body = ms_alloc(sizeof(MSTOK) * (size_t)( m->nbody + 1 ));
	for ( k = 0; k < m->nbody; k++ ) m->body[k] = *p->tk->t[start + k];
	if ( peek(p, 0)->type == T_NL ) next(p);
	p->noexp = FALSE;
}

/* The next name continues a declaration list: not a keyword, or one used as a name */
static BOOL decl_goes_on( PS *p )
{
	MSTOK	*t = peek(p, 0), *n1;

	if ( t->type != T_IDENT ) return FALSE;
	if ( !is_keyword(t->name) ) return TRUE;
	n1 = peek(p, 1);
	return (BOOL)( is_op(n1, ':') || is_op(n1, '[') );
}

static void add_decl( MSDECL **v, INT *n, MSDECL *d )
{
	*v = ms_realloc(*v, sizeof(MSDECL) * (size_t)( *n + 1 ));
	(*v)[(*n)++] = *d;
}

static void parse_var_decl( PS *p, MSDECL **v, INT *n, BOOL share_ok )
{
	next(p);
	for ( ;; ) {
		MSDECL	d;

		memset(&d, 0, sizeof(d));
		d.name = expect_type(p, T_IDENT, "期待: IDENT")->name;
		d.type = 'G';
		if ( at_op(p, ':') ) {
			MSTOK	*tt;
			MSSTR	*u;

			next(p);
			tt = expect_type(p, T_IDENT, "期待: 型");
			u = ms_upper(tt->name);
			if ( u->len != 1 || strchr("BCIFSG", u->s[0]) == NULL ) fail(p, tt, "未知の型: ", NULL);
			d.type = u->s[0];
		}
		if ( at_op(p, '[') ) {
			next(p);
			d.size = parse_expr(p);
			expect_op(p, ']');
		}
		if ( share_ok && at_op(p, '=') ) {
			next(p);
			d.share = expect_type(p, T_IDENT, "期待: IDENT")->name;
			if ( at_op(p, '[') ) {
				next(p);
				d.share_off = parse_expr(p);
				expect_op(p, ']');
			}
		}
		add_decl(v, n, &d);
		if ( at_op(p, ',') ) { next(p); continue; }
		if ( decl_goes_on(p) ) continue;
		break;
	}
}

static void parse_segment_decl( PS *p )
{
	next(p);
	for ( ;; ) {
		MSDECL	d;

		memset(&d, 0, sizeof(d));
		d.name = expect_type(p, T_IDENT, "期待: IDENT")->name;
		d.type = 'S';
		if ( at_op(p, '[') ) {
			next(p);
			d.size = parse_expr(p);
			expect_op(p, ']');
		}
		add_decl(&p->prog->segdecl, &p->prog->nsegdecl, &d);
		if ( at_op(p, ',') ) { next(p); continue; }
		if ( decl_goes_on(p) ) continue;
		break;
	}
}

static void parse_script_decl( PS *p )
{
	next(p);
	for ( ;; ) {
		expect_type(p, T_IDENT, "期待: IDENT");
		if ( at_op(p, ',') ) { next(p); continue; }
		if ( decl_goes_on(p) ) continue;
		break;
	}
}

static void parse_params( PS *p, MSPROC *pr )
{
	if ( !at_op(p, '(') ) return;
	next(p);
	if ( at_op(p, ')') ) { next(p); return; }
	for ( ;; ) {
		MSDECL	d;

		memset(&d, 0, sizeof(d));
		d.name = expect_type(p, T_IDENT, "期待: IDENT")->name;
		d.type = 'G';
		if ( at_op(p, ':') ) {
			next(p);
			d.type = ms_upper(expect_type(p, T_IDENT, "期待: 型")->name)->s[0];
		}
		if ( at_op(p, '[') ) {
			next(p);
			if ( at_op(p, ']') ) d.open = TRUE;
			else d.size = parse_expr(p);
			expect_op(p, ']');
		}
		add_decl(&pr->param, &pr->nparam, &d);
		if ( at_op(p, ',') ) { next(p); continue; }
		break;
	}
	expect_op(p, ')');
}

static const char *const ends_end[] = { "END", NULL };

static void add_proc( PS *p, MSPROC *pr )
{
	p->prog->proc = ms_realloc(p->prog->proc, sizeof(MSPROC *) * (size_t)( p->prog->nproc + 1 ));
	p->prog->proc[p->prog->nproc++] = pr;
}

static void parse_special( PS *p, INT type )
{
	MSPROC	*pr = ms_alloc(sizeof(MSPROC));
	MSN	*b;

	next(p);
	expect_nl(p);
	b = parse_block(p, ends_end);
	expect_type(p, T_IDENT, "期待: END");
	expect_nl(p);
	pr->type = type;
	pr->body = b->v;
	pr->nbody = b->nv;
	add_proc(p, pr);
}

static void parse_handler( PS *p, INT type )
{
	MSPROC	*pr = ms_alloc(sizeof(MSPROC));
	MSN	*b;

	next(p);
	pr->type = type;
	pr->name = expect_type(p, T_IDENT, "期待: 手続き名")->name;
	parse_params(p, pr);
	if ( peek(p, 0)->type == T_IDENT ) {
		MSSTR	*u = ms_upper(peek(p, 0)->name);
		INT	ev = EV_NONE;

		if ( strcmp(u->s, "PRESS") == 0 ) ev = EV_PRESS;
		else if ( strcmp(u->s, "CLICK") == 0 ) ev = EV_CLICK;
		else if ( strcmp(u->s, "DCLICK") == 0 ) ev = EV_DCLICK;
		else if ( strcmp(u->s, "QPRESS") == 0 ) ev = EV_QPRESS;
		else if ( strcmp(u->s, "MENU") == 0 ) ev = EV_MENU;
		else if ( strcmp(u->s, "KEY") == 0 ) ev = EV_KEY;
		if ( ev != EV_NONE ) {
			next(p);
			pr->event = ev;
			if ( ev == EV_MENU ) {
				if ( peek(p, 0)->type == T_STRING ) {
					MSTOK	*s = next(p);

					pr->menu = s->chars;
					pr->nmenu = s->nchars;
				}
			} else if ( ev != EV_KEY ) {
				/* the segments, to the statement's end; keywords among them too */
				while ( peek(p, 0)->type == T_IDENT ) {
					pr->target = ms_realloc(pr->target, sizeof(MSSTR *) * (size_t)( pr->ntarget + 1 ));
					pr->target[pr->ntarget++] = next(p)->name;
					if ( at_op(p, ',') ) next(p);
				}
			}
		}
	}
	expect_nl(p);
	b = parse_block(p, ends_end);
	expect_type(p, T_IDENT, "期待: END");
	expect_nl(p);
	pr->body = b->v;
	pr->nbody = b->nv;
	add_proc(p, pr);
}

/* A statement's arguments up to its end or ':', with or without commas */
static void generic_args( PS *p, MSN *n )
{
	while ( !at_end(p) ) {
		MSTOK	*nx;

		if ( at_op(p, ':') ) break;
		push(&n->v, &n->nv, parse_expr(p));
		if ( at_op(p, ',') ) { next(p); continue; }
		nx = peek(p, 0);
		if ( nx->type == T_IDENT && !is_boundary(nx->name) ) continue;
		if ( nx->type == T_INT || nx->type == T_FLOAT || nx->type == T_STRING || nx->type == T_SYSVAR ) continue;
		break;
	}
}

static MSN *parse_generic( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	generic_args(p, n);
	if ( at_op(p, ':') ) {
		next(p);
		n->a = parse_expr(p);
	}
	expect_nl(p);
	return n;
}

static MSN *parse_fread( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	generic_args(p, n);
	expect_nl(p);
	return n;
}

static BOOL expr_start( PS *p )
{
	MSTOK	*t = peek(p, 0);

	if ( t->type == T_INT || t->type == T_FLOAT || t->type == T_STRING || t->type == T_SYSVAR ) return TRUE;
	if ( t->type == T_IDENT && !is_boundary(t->name) ) return TRUE;
	if ( t->type == T_OP && ( t->op == '(' || t->op == '-' || t->op == '!' || t->op == '~' ) ) return TRUE;
	return FALSE;
}

/* Arguments separated by spaces or commas: CALL, MESG, TEXT */
static void bare_args( PS *p, MSN *n )
{
	BOOL	am = p->argmode, after_comma = FALSE;

	p->argmode = TRUE;
	for ( ;; ) {
		MSTOK	*t = peek(p, 0), *nx;

		if ( t->type == T_NL || t->type == T_EOF ) break;
		if ( t->type == T_OP && ( t->op == ':' || t->op == ';' ) ) break;
		if ( t->type == T_IDENT && is_boundary(t->name) && n->nv > 0 && !after_comma ) break;
		push(&n->v, &n->nv, parse_expr(p));
		after_comma = FALSE;
		if ( at_op(p, ',') ) { next(p); after_comma = TRUE; continue; }
		nx = peek(p, 0);
		if ( nx->type == T_IDENT && !is_boundary(nx->name) ) continue;
		if ( nx->type == T_INT || nx->type == T_FLOAT || nx->type == T_STRING || nx->type == T_SYSVAR ) continue;
		if ( nx->type == T_OP && ( nx->op == '(' || nx->op == '-' || nx->op == '!' || nx->op == '~' ) ) continue;
		break;
	}
	p->argmode = am;
}

/* Segments of a display statement */
static void seg_args( PS *p, MSN *n )
{
	BOOL	am = p->argmode, after_comma = FALSE;

	p->argmode = TRUE;
	for ( ;; ) {
		MSTOK	*t = peek(p, 0), *nx;

		if ( t->type == T_NL || t->type == T_EOF ) break;
		if ( t->type == T_OP && ( t->op == ':' || t->op == ';' ) ) break;
		if ( t->type == T_IDENT && is_boundary(t->name) && n->nv > 0 && !after_comma ) break;
		push(&n->v, &n->nv, parse_expr(p));
		after_comma = FALSE;
		if ( at_op(p, ',') ) { next(p); after_comma = TRUE; continue; }
		nx = peek(p, 0);
		if ( nx->type == T_IDENT && !is_boundary(nx->name) ) continue;
		break;
	}
	p->argmode = am;
}

static MSN *optional_args( PS *p, MSN *n )
{
	if ( !at_op(p, '(') ) return n;
	next(p);
	if ( at_op(p, ')') ) { next(p); return n; }
	push(&n->v, &n->nv, parse_expr(p));
	while ( at_op(p, ',') ) {
		next(p);
		push(&n->v, &n->nv, parse_expr(p));
	}
	expect_op(p, ')');
	return n;
}

static MSN *parse_lvalue( PS *p )
{
	MSTOK	*t = next(p);
	MSN	*n;
	BOOL	sl;

	if ( t->type == T_IDENT ) {
		n = node(N_NAME, t->line);
		n->name = t->name;
		if ( at_op(p, '[') ) {
			index_or_slice(p, &n->a, &n->b, &sl);
			if ( sl ) n->flags |= NF_SLICE;
		}
		if ( at_op(p, '.') ) {
			next(p);
			n->name2 = expect_type(p, T_IDENT, "期待: 状態名")->name;
		}
		return n;
	}
	if ( t->type == T_SYSVAR ) {
		n = node(N_SYSVAR, t->line);
		n->name = t->name;
		if ( at_op(p, '[') ) {
			next(p);
			n->a = sub_expr(p);
			expect_op(p, ']');
		}
		return n;
	}
	fail(p, t, "代入先が不正: ", NULL);
	return NULL;
}

static MSN *parse_set( PS *p, BOOL kw )
{
	MSN	*n = node(S_SET, peek(p, 0)->line);

	if ( kw ) next(p);
	n->a = parse_lvalue(p);
	if ( kw ) {
		if ( at_op(p, '=') ) next(p);
	} else {
		expect_op(p, '=');
	}
	push(&n->v, &n->nv, parse_expr(p));
	for ( ;; ) {
		MSTOK	*t;

		if ( at_op(p, ',') ) {
			next(p);
			push(&n->v, &n->nv, parse_expr(p));
			continue;
		}
		t = peek(p, 0);
		if ( t->type == T_INT || t->type == T_FLOAT || t->type == T_STRING
		  || ( t->type == T_IDENT && !is_keyword(t->name) ) || is_op(t, '(') ) {
			push(&n->v, &n->nv, parse_expr(p));
			continue;
		}
		break;
	}
	expect_nl(p);
	return n;
}

static const char *const ends_if[] = { "ELSEIF", "ELSE", "ENDIF", NULL };
static const char *const ends_endif[] = { "ENDIF", NULL };
static const char *const ends_while[] = { "ENDWHILE", NULL };
static const char *const ends_repeat[] = { "ENDREPEAT", NULL };
static const char *const ends_case[] = { "CASE", "DEFAULT", "ENDCASE", NULL };
static const char *const ends_endcase[] = { "ENDCASE", NULL };

static MSN *parse_if( PS *p )
{
	MSN	*n = node(S_IF, peek(p, 0)->line);

	next(p);
	push(&n->v, &n->nv, parse_expr(p));
	expect_nl(p);
	push(&n->w, &n->nw, parse_block(p, ends_if));
	while ( check_kw(p, "ELSEIF") ) {
		next(p);
		push(&n->v, &n->nv, parse_expr(p));
		expect_nl(p);
		push(&n->w, &n->nw, parse_block(p, ends_if));
	}
	if ( check_kw(p, "ELSE") ) {
		next(p);
		expect_nl(p);
		n->a = parse_block(p, ends_endif);
	}
	expect_type(p, T_IDENT, "期待: ENDIF");
	expect_nl(p);
	return n;
}

static MSN *parse_while( PS *p )
{
	MSN	*n = node(S_WHILE, peek(p, 0)->line);

	next(p);
	n->a = parse_expr(p);
	expect_nl(p);
	n->b = parse_block(p, ends_while);
	expect_type(p, T_IDENT, "期待: ENDWHILE");
	expect_nl(p);
	return n;
}

static MSN *parse_repeat( PS *p )
{
	MSN	*n = node(S_REPEAT, peek(p, 0)->line);

	next(p);
	if ( peek(p, 0)->type != T_NL ) n->a = parse_expr(p);
	expect_nl(p);
	n->b = parse_block(p, ends_repeat);
	expect_type(p, T_IDENT, "期待: ENDREPEAT");
	expect_nl(p);
	return n;
}

static MSN *parse_switch( PS *p )
{
	MSN	*n = node(S_SWITCH, peek(p, 0)->line);

	next(p);
	n->a = parse_expr(p);
	expect_nl(p);
	skip_nl(p);
	for ( ;; ) {
		if ( check_kw(p, "CASE") ) {
			next(p);
			push(&n->v, &n->nv, parse_expr(p));
			expect_nl(p);
			push(&n->w, &n->nw, parse_block(p, ends_case));
			continue;
		}
		if ( check_kw(p, "DEFAULT") ) {
			next(p);
			expect_nl(p);
			n->b = parse_block(p, ends_endcase);
			continue;
		}
		break;
	}
	expect_type(p, T_IDENT, "期待: ENDCASE");
	expect_nl(p);
	return n;
}

static MSN *parse_targets( PS *p, UH k, BOOL timeout )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	while ( peek(p, 0)->type == T_IDENT || peek(p, 0)->type == T_SYSVAR ) {
		push(&n->v, &n->nv, parse_expr(p));
		if ( at_op(p, ',') ) { next(p); continue; }
		break;
	}
	if ( timeout && at_op(p, ':') ) {
		next(p);
		n->a = parse_expr(p);
	}
	expect_nl(p);
	return n;
}

static MSN *parse_call( PS *p )
{
	MSN	*n = node(S_CALL, peek(p, 0)->line);

	next(p);
	n->name = expect_type(p, T_IDENT, "期待: 手続き名")->name;
	if ( at_op(p, '[') ) {
		next(p);
		n->b = parse_expr(p);			/* the name taken from an array */
		expect_op(p, ']');
	}
	if ( at_op(p, '(') ) {
		/* CALL name(a,b), or CALL name (expr)... with the bracket the first argument */
		INT	save = p->pos;
		MSN	*tmp = node(S_CALL, n->line);
		MSTOK	*nx;

		optional_args(p, tmp);
		nx = peek(p, 0);
		if ( nx->type == T_NL || nx->type == T_EOF || is_op(nx, ':') || is_op(nx, ';') ) {
			n->v = tmp->v;
			n->nv = tmp->nv;
		} else {
			p->pos = save;
			bare_args(p, n);
		}
	} else {
		bare_args(p, n);
	}
	expect_nl(p);
	return n;
}

static MSN *parse_execute( PS *p )
{
	MSN	*n = node(S_EXECUTE, peek(p, 0)->line);

	next(p);
	for ( ;; ) {
		MSN	*h = node(N_CALL, peek(p, 0)->line);
		MSTOK	*nx;

		h->name = expect_type(p, T_IDENT, "期待: 手続き名")->name;
		if ( at_op(p, '[') ) {
			next(p);
			h->b = parse_expr(p);
			expect_op(p, ']');
		}
		nx = peek(p, 0);
		if ( is_op(nx, ',') ) {
			push(&n->v, &n->nv, h);
			next(p);
			continue;
		}
		if ( is_op(nx, '(') ) optional_args(p, h);
		push(&n->v, &n->nv, h);
		break;
	}
	if ( at_op(p, ':') ) {
		next(p);
		n->name2 = expect_type(p, T_IDENT, "期待: 変数")->name;
	}
	expect_nl(p);
	return n;
}

static MSN *parse_display( PS *p, UH k, BOOL allow_empty )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	if ( allow_empty && at_end(p) ) {
		if ( peek(p, 0)->type == T_NL && peek(p, 0)->op == ';' ) n->flags |= NF_DEFERRED;
		expect_nl(p);
		return n;
	}
	seg_args(p, n);
	if ( at_op(p, ':') ) {
		next(p);
		n->name2 = ms_upper(expect_type(p, T_IDENT, "期待: 効果")->name);
		n->a = parse_expr(p);
	}
	if ( peek(p, 0)->type == T_NL && peek(p, 0)->op == ';' ) n->flags |= NF_DEFERRED;
	expect_nl(p);
	return n;
}

static MSN *parse_move( PS *p )
{
	BOOL	am = p->argmode;
	MSN	*n = node(S_MOVE, peek(p, 0)->line);

	p->argmode = TRUE;
	next(p);
	seg_args(p, n);
	if ( at_op(p, ':') ) {
		next(p);
		if ( at_end(p) ) {
			/* nothing: back to where they were made */
		} else if ( check_kw(p, "DUP") ) {
			next(p);
			n->flags |= NF_DUP;
		} else {
			n->a = parse_expr(p);
			if ( at_op(p, ',') ) {
				next(p);
				n->b = parse_expr(p);
			} else if ( expr_start(p) && !at_op(p, '@') ) {
				n->b = parse_expr(p);
			}
			if ( at_op(p, ',') ) next(p);
			if ( at_op(p, '@') ) {
				next(p);
				n->flags |= NF_BASE_AT;
			} else if ( peek(p, 0)->type == T_IDENT && !is_boundary(peek(p, 0)->name)
				 && !check_kw(p, "DUP") ) {
				n->c = parse_expr(p);
			}
			if ( at_op(p, ':') ) {
				next(p);
				if ( check_kw(p, "DUP") ) { next(p); n->flags |= NF_DUP; }
			}
			if ( check_kw(p, "DUP") ) { next(p); n->flags |= NF_DUP; }
		}
	}
	if ( peek(p, 0)->type == T_NL && peek(p, 0)->op == ';' ) n->flags |= NF_DEFERRED;
	expect_nl(p);
	p->argmode = am;
	return n;
}

static MSN *parse_beep( PS *p )
{
	MSN	*n = node(S_BEEP, peek(p, 0)->line);

	next(p);
	if ( !at_end(p) ) {
		n->a = parse_expr(p);
		if ( at_op(p, ',') ) next(p);
		if ( expr_start(p) ) n->b = parse_expr(p);
	}
	expect_nl(p);
	return n;
}

static MSN *parse_two( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	n->a = parse_expr(p);
	if ( at_op(p, ',') ) next(p);
	n->b = parse_expr(p);
	expect_nl(p);
	return n;
}

static MSN *parse_one( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	n->a = parse_expr(p);
	expect_nl(p);
	return n;
}

static MSN *parse_bare( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	bare_args(p, n);
	expect_nl(p);
	return n;
}

static MSN *parse_text( PS *p )
{
	MSN	*n = node(S_TEXT, peek(p, 0)->line);

	next(p);
	n->a = parse_expr(p);
	if ( at_op(p, ',') ) next(p);
	bare_args(p, n);
	expect_nl(p);
	return n;
}

static MSN *parse_input( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	if ( peek(p, 0)->type == T_IDENT && !is_keyword(peek(p, 0)->name) ) {
		n->a = parse_expr(p);
		if ( at_op(p, ':') || at_op(p, ',') ) {
			next(p);
			n->b = parse_expr(p);
			if ( at_op(p, ':') || at_op(p, ',') ) {
				next(p);
				n->c = parse_expr(p);
			}
		}
	}
	expect_nl(p);
	return n;
}

static MSN *parse_vcmd( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	while ( peek(p, 0)->type == T_IDENT || peek(p, 0)->type == T_STRING ) {
		MSTOK	*t = peek(p, 0);
		MSN	*x;

		if ( t->type == T_STRING ) {
			next(p);
			x = node(N_STR, t->line);
			x->chars = t->chars;
			x->nchars = t->nchars;
		} else {
			if ( is_keyword(t->name) ) break;
			next(p);
			x = node(N_NAME, t->line);
			x->name = t->name;
			if ( at_op(p, '[') ) {
				next(p);
				x->a = parse_expr(p);
				expect_op(p, ']');
			}
		}
		push(&n->v, &n->nv, x);
		if ( at_op(p, ',') ) { next(p); continue; }
		break;
	}
	if ( at_op(p, ':') ) {
		next(p);
		n->a = parse_expr(p);
	}
	expect_nl(p);
	return n;
}

static MSN *parse_setseg( PS *p, UH k )
{
	MSN	*n = node(k, peek(p, 0)->line);

	next(p);
	n->name = expect_type(p, T_IDENT, "期待: セグメント")->name;
	if ( at_op(p, '[') ) {
		next(p);
		n->a = parse_expr(p);
		expect_op(p, ']');
	}
	expect_op(p, '=');
	push(&n->v, &n->nv, parse_expr(p));
	if ( k == S_COPYSEG ) {
		while ( at_op(p, ',') ) {
			next(p);
			push(&n->v, &n->nv, parse_expr(p));
		}
	}
	expect_nl(p);
	return n;
}

static MSN *parse_event( PS *p )
{
	MSN	*n = node(S_EVENT, peek(p, 0)->line);

	next(p);
	n->name2 = ms_upper(expect_type(p, T_IDENT, "期待: 事象の種別")->name);
	while ( at_op(p, ',') ) {
		next(p);
		push(&n->v, &n->nv, parse_expr(p));
	}
	if ( at_op(p, ':') ) {
		next(p);
		n->a = parse_expr(p);
		if ( at_op(p, ',') ) {
			next(p);
			n->b = parse_expr(p);
		}
	}
	expect_nl(p);
	return n;
}

static MSN *parse_local( PS *p )
{
	MSN	*n = node(S_LOCAL, peek(p, 0)->line);

	parse_var_decl(p, &n->decl, &n->ndecl, FALSE);
	expect_nl(p);
	return n;
}

static MSN *parse_statement( PS *p )
{
	MSTOK	*t = peek(p, 0);
	const char *kw;

	if ( t->type != T_IDENT && t->type != T_SYSVAR ) fail(p, t, "文の先頭は識別子: ", NULL);
	if ( t->type == T_IDENT ) {
		kw = ms_upper(t->name)->s;
		if ( strcmp(kw, "LOCAL") == 0 ) return parse_local(p);
		if ( strcmp(kw, "DEFINE") == 0 ) { parse_define(p); return NULL; }
		if ( strcmp(kw, "COMMENT") == 0 || strcmp(kw, "DEBUG") == 0 ) { skip_comment(p); return NULL; }
		if ( strcmp(kw, "LOG") == 0 ) return parse_bare(p, S_LOG);
		if ( strcmp(kw, "IF") == 0 ) return parse_if(p);
		if ( strcmp(kw, "WHILE") == 0 ) return parse_while(p);
		if ( strcmp(kw, "REPEAT") == 0 ) return parse_repeat(p);
		if ( strcmp(kw, "SWITCH") == 0 ) return parse_switch(p);
		if ( strcmp(kw, "BREAK") == 0 ) return parse_targets(p, S_BREAK, FALSE);
		if ( strcmp(kw, "CONTINUE") == 0 ) {
			MSN	*n = node(S_CONTINUE, t->line);

			next(p);
			expect_nl(p);
			return n;
		}
		if ( strcmp(kw, "SET") == 0 ) return parse_set(p, TRUE);
		if ( strcmp(kw, "EXIT") == 0 ) {
			MSN	*n = node(S_EXIT, t->line);

			next(p);
			if ( !at_end(p) ) n->a = parse_expr(p);
			expect_nl(p);
			return n;
		}
		if ( strcmp(kw, "CALL") == 0 ) return parse_call(p);
		if ( strcmp(kw, "EXECUTE") == 0 ) return parse_execute(p);
		if ( strcmp(kw, "FINISH") == 0 || strcmp(kw, "WSAVE") == 0 || strcmp(kw, "FULLWIND") == 0 ) {
			MSN	*n = node(( kw[0] == 'F' && kw[1] == 'I' ) ? S_FINISH
					  : ( kw[0] == 'W' ) ? S_WSAVE : S_FULLWIND, t->line);

			next(p);
			expect_nl(p);
			return n;
		}
		if ( strcmp(kw, "TERMINATE") == 0 ) return parse_targets(p, S_TERMINATE, FALSE);
		if ( strcmp(kw, "SUSPEND") == 0 ) return parse_targets(p, S_SUSPEND, TRUE);
		if ( strcmp(kw, "SLEEP") == 0 ) return parse_one(p, S_SLEEP);
		if ( strcmp(kw, "WAIT") == 0 ) {
			MSN	*n = node(S_WAIT, t->line);

			next(p);
			n->a = parse_expr(p);
			if ( at_op(p, ':') ) {
				next(p);
				n->b = parse_expr(p);
			}
			expect_nl(p);
			return n;
		}
		if ( strcmp(kw, "SCENE") == 0 ) return parse_display(p, S_SCENE, TRUE);
		if ( strcmp(kw, "APPEAR") == 0 ) return parse_display(p, S_APPEAR, FALSE);
		if ( strcmp(kw, "DISAPPEAR") == 0 ) return parse_display(p, S_DISAPPEAR, FALSE);
		if ( strcmp(kw, "MOVE") == 0 ) return parse_move(p);
		if ( strcmp(kw, "BEEP") == 0 ) return parse_beep(p);
		if ( strcmp(kw, "WSIZE") == 0 ) return parse_two(p, S_WSIZE);
		if ( strcmp(kw, "WMOVE") == 0 ) return parse_two(p, S_WMOVE);
		if ( strcmp(kw, "UPDATE") == 0 ) return parse_one(p, S_UPDATE);
		if ( strcmp(kw, "MESG") == 0 ) return parse_bare(p, S_MESG);
		if ( strcmp(kw, "TEXT") == 0 ) return parse_text(p);
		if ( strcmp(kw, "INPUT") == 0 ) return parse_input(p, S_INPUT);
		if ( strcmp(kw, "KINPUT") == 0 ) return parse_input(p, S_KINPUT);
		if ( strcmp(kw, "VOPEN") == 0 ) return parse_vcmd(p, S_VOPEN);
		if ( strcmp(kw, "VCLOSE") == 0 ) return parse_vcmd(p, S_VCLOSE);
		if ( strcmp(kw, "VWAIT") == 0 ) return parse_vcmd(p, S_VWAIT);
		if ( strcmp(kw, "SETSEG") == 0 ) return parse_setseg(p, S_SETSEG);
		if ( strcmp(kw, "COPYSEG") == 0 ) return parse_setseg(p, S_COPYSEG);
		if ( strcmp(kw, "EVENT") == 0 ) return parse_event(p);
		if ( strcmp(kw, "FOPEN") == 0 ) return parse_generic(p, S_FOPEN);
		if ( strcmp(kw, "FCLOSE") == 0 ) return parse_generic(p, S_FCLOSE);
		if ( strcmp(kw, "FREAD") == 0 ) return parse_fread(p, S_FREAD);
		if ( strcmp(kw, "FWRITE") == 0 ) return parse_fread(p, S_FWRITE);
		if ( strcmp(kw, "PROCESS") == 0 ) return parse_generic(p, S_PROCESS);
		if ( strcmp(kw, "PWAIT") == 0 ) return parse_generic(p, S_PWAIT);
		if ( strcmp(kw, "MSEND") == 0 ) return parse_generic(p, S_MSEND);
		if ( strcmp(kw, "MRECV") == 0 ) return parse_generic(p, S_MRECV);
		if ( strcmp(kw, "DOPEN") == 0 || strcmp(kw, "DCLOSE") == 0 || strcmp(kw, "DREAD") == 0
		  || strcmp(kw, "DWRITE") == 0 ) {
			MSN	*n = parse_generic(p, S_HWNOOP);

			n->op = (UH)( ( kw[1] == 'O' ) ? 'O' : ( kw[1] == 'C' ) ? 'C' : ( kw[1] == 'R' ) ? 'R' : 'W' );
			return n;
		}
		if ( strcmp(kw, "RSINIT") == 0 ) return parse_generic(p, S_RSINIT);
		if ( strcmp(kw, "RSPUTC") == 0 || strcmp(kw, "RSPUT") == 0 ) return parse_generic(p, S_RSPUT);
		if ( strcmp(kw, "RSPUTN") == 0 ) return parse_generic(p, S_RSPUTN);
		if ( strcmp(kw, "RSWAIT") == 0 ) return parse_generic(p, S_RSWAIT);
		if ( strcmp(kw, "RSGETN") == 0 || strcmp(kw, "RSGET") == 0 ) return parse_generic(p, S_RSGETN);
		if ( strcmp(kw, "RSGETC") == 0 ) return parse_generic(p, S_RSGETC);
		if ( strcmp(kw, "RSCNTL") == 0 ) return parse_generic(p, S_RSCNTL);
		if ( strcmp(kw, "CONSOLE") == 0 ) return parse_generic(p, S_CONSOLE);
		if ( strcmp(kw, "SPRINTF") == 0 ) return parse_generic(p, S_SPRINTF);
		if ( strcmp(kw, "HTTPREQ") == 0 ) return parse_generic(p, S_HTTPREQ);
		if ( strcmp(kw, "HTTPHDR") == 0 ) return parse_generic(p, S_HTTPHDR);
		if ( strcmp(kw, "JSONGET") == 0 ) return parse_generic(p, S_JSONGET);
		if ( strcmp(kw, "JSONLEN") == 0 ) return parse_generic(p, S_JSONLEN);
		if ( strcmp(kw, "TCPOPEN") == 0 ) return parse_generic(p, S_TCPOPEN);
		if ( strcmp(kw, "TCPSEND") == 0 ) return parse_generic(p, S_TCPSEND);
		if ( strcmp(kw, "TCPWAIT") == 0 ) return parse_generic(p, S_TCPWAIT);
		if ( strcmp(kw, "TCPRECV") == 0 ) return parse_generic(p, S_TCPRECV);
		if ( strcmp(kw, "TCPCLOSE") == 0 ) return parse_generic(p, S_TCPCLOSE);
	}
	return parse_set(p, FALSE);
}

/* ---------------------------------------------------------------- the program */

static void tkv_of( TKV *v, MSTOK *t, INT n )
{
	INT	i;

	v->cap = n + 256;
	v->t = ms_alloc(sizeof(MSTOK *) * (size_t)v->cap);
	for ( i = 0; i < n; i++ ) v->t[i] = &t[i];
	v->n = n;
}

ER ms_parse( MSTOKS *toks, MSPROG *prog, char *err, INT errmax )
{
	PS	p;
	TKV	v;

	memset(&p, 0, sizeof(p));
	tkv_of(&v, toks->t, toks->n);
	p.tk = &v;
	p.prog = prog;
	p.err = err;
	p.errmax = errmax;
	if ( setjmp(p.jb) != 0 ) {
		return E_PAR;
	}
	skip_nl(&p);
	if ( check_kw(&p, "VERSION") ) {
		next(&p);
		prog->version = (INT)expect_type(&p, T_INT, "期待: 版数")->num;
		expect_nl(&p);
		skip_nl(&p);
	}
	while ( peek(&p, 0)->type != T_EOF ) {
		MSTOK	*t = peek(&p, 0);
		const char *kw;

		if ( t->type == T_NL ) { next(&p); continue; }
		if ( t->type != T_IDENT ) fail(&p, t, "宣言または手続き定義が必要: ", NULL);
		kw = ms_upper(t->name)->s;
		if ( strcmp(kw, "VERSION") == 0 ) {
			/* a second script joined on says its version again */
			next(&p);
			if ( peek(&p, 0)->type == T_INT ) prog->version = (INT)next(&p)->num;
			expect_nl(&p);
			continue;
		}
		if ( strcmp(kw, "DEFINE") == 0 ) { parse_define(&p); continue; }
		if ( strcmp(kw, "VARIABLE") == 0 ) {
			parse_var_decl(&p, &prog->global, &prog->nglobal, TRUE);
			expect_nl(&p);
			continue;
		}
		if ( strcmp(kw, "SEGMENT") == 0 ) { parse_segment_decl(&p); expect_nl(&p); continue; }
		if ( strcmp(kw, "SCRIPT") == 0 ) { parse_script_decl(&p); expect_nl(&p); continue; }
		if ( strcmp(kw, "COMMENT") == 0 || strcmp(kw, "LOG") == 0 ) { skip_comment(&p); continue; }
		if ( strcmp(kw, "DEBUG") == 0 ) {
			next(&p);
			if ( peek(&p, 0)->type == T_INT ) prog->debug = (INT)next(&p)->num;
			skip_comment(&p);
			continue;
		}
		if ( strcmp(kw, "PROLOGUE") == 0 ) { parse_special(&p, P_PROLOGUE); continue; }
		if ( strcmp(kw, "EPILOGUE") == 0 ) { parse_special(&p, P_EPILOGUE); continue; }
		if ( strcmp(kw, "ACTION") == 0 ) { parse_handler(&p, P_ACTION); continue; }
		if ( strcmp(kw, "MACTION") == 0 ) { parse_handler(&p, P_MACTION); continue; }
		if ( strcmp(kw, "FUNC") == 0 ) { parse_handler(&p, P_FUNC); continue; }
		fail(&p, t, "未対応のトップレベル要素: ", NULL);
	}
	return E_OK;
}

/* An expression from the tokens of a macro; NULL when they are not one */
MSN *ms_parse_expr_toks( const MSTOK *t, INT n )
{
	PS	p;
	MSTOK	*c;
	TKV	v;
	char	err[128];
	MSN	*e;

	c = ms_alloc(sizeof(MSTOK) * (size_t)( n + 1 ));
	memcpy(c, t, sizeof(MSTOK) * (size_t)n);
	c[n].type = T_EOF;
	tkv_of(&v, c, n + 1);
	memset(&p, 0, sizeof(p));
	p.tk = &v;
	p.err = err;
	p.errmax = sizeof(err);
	p.noexp = TRUE;
	if ( setjmp(p.jb) != 0 ) {
		return NULL;
	}
	e = parse_expr(&p);
	return e;
}
