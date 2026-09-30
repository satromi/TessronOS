/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_rtlib.c
 *	The rest of the run time: the statements that show things, the
 *	system variables, the functions, the format of MESG and TEXT, the
 *	records of objects, and the procedures things happen to
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ts/dt.h>
#include <ts/proc.h>
#include <ts/ser.h>

IMPORT ER dt_gettime( TS_TIME *p_t );
IMPORT ER dt_localtime( CONST TS_TIME *t, TS_TM *tm );
IMPORT ER dt_mktime( CONST TS_TM *tm, TS_TIME *p_t );
IMPORT ER dt_getsystz( INT *p_minutes );

void	ms_beep( double freq, double dur );
void	ms_win_resize( INT w, INT h );
void	ms_win_mode( INT mode );
void	ms_win_move( INT x, INT y );
void	ms_win_save( void );
void	ms_transition( MSSEG **show, INT nshow, MSSEG **hide, INT nhide, MSSTR *effect, INT steps, BOOL whole );

/* ---------------------------------------------------------------- formatting */

static void pad_to( MSBUF *out, const char *s, INT width, BOOL left, char padc )
{
	INT	n = 0, i;
	UW	cp;

	for ( i = 0; s[i] != 0; ) { i += ms_utf8_dec((const UB *)s + i, (INT)strlen(s + i), &cp); n++; }
	if ( width > n && !left ) for ( i = 0; i < width - n; i++ ) mb_putc(out, padc);
	mb_puts(out, s);
	if ( width > n && left ) for ( i = 0; i < width - n; i++ ) mb_putc(out, ' ');
}

/* The exponent written with two digits at least, as C writes it */
static void fix_exp( char *s )
{
	(void)s;
}

/* %g as the script means it: %e below 0.0001 or when the integer part does not fit */
static void fmt_g( double a, INT prec, BOOL upper, char *out, INT max )
{
	INT	p = ( prec >= 0 ) ? ( prec < 1 ? 1 : prec ) : 6;
	double	av = fabs(a);
	INT	intdigits = ( av >= 1 ) ? (INT)floor(log10(av)) + 1 : 1;

	if ( a != 0 && isfinite(a) && ( av < 0.0001 || intdigits > p ) ) {
		char	*e;
		INT	k;

		snprintf(out, (size_t)max, "%.*e", ( p - 1 > 0 ) ? p - 1 : 0, a);
		e = strchr(out, 'e');
		if ( e != NULL ) {
			/* the mantissa without trailing zeros */
			char	ex[16];

			strcpy(ex, e);
			*e = 0;
			k = (INT)strlen(out);
			if ( strchr(out, '.') != NULL ) {
				while ( k > 0 && out[k - 1] == '0' ) out[--k] = 0;
				if ( k > 0 && out[k - 1] == '.' ) out[--k] = 0;
			}
			strcat(out, ex);
		}
	} else {
		char	*d;
		INT	k;

		snprintf(out, (size_t)max, "%.*g", p, a);
		/* toPrecision keeps the digits; the script's %g then drops zeros after the point */
		if ( strchr(out, 'e') == NULL ) {
			/* %.*g already drops them; a fixed number with p digits */
			INT	dec = p - intdigits;

			if ( av < 1 && a != 0 ) dec = p - 1 - (INT)floor(log10(av));
			if ( dec < 0 ) dec = 0;
			snprintf(out, (size_t)max, "%.*f", dec, a);
			d = strchr(out, '.');
			if ( d != NULL ) {
				k = (INT)strlen(out);
				while ( k > 0 && out[k - 1] == '0' ) out[--k] = 0;
				if ( k > 0 && out[k - 1] == '.' ) out[--k] = 0;
			}
		}
	}
	if ( upper ) {
		INT	i;

		for ( i = 0; out[i] != 0; i++ ) if ( out[i] >= 'a' && out[i] <= 'z' ) out[i] = (char)( out[i] - 32 );
	}
}

void rt_fmt( MSV fmt, MSV *args, INT nargs, MSBUF *out )
{
	MSBUF	fb;
	UW	*f;
	INT	nf = 0, i, ai = 0;

	/* the format as characters, normalised (it may come from a figure) */
	mb_init(&fb);
	mb_putn(&fb, "", 0);
	mv_text(fmt, &fb);
	f = ms_alloc(sizeof(UW) * (size_t)( fb.n + 1 ));
	for ( i = 0; i < fb.n; ) {
		UW	cp;
		INT	d = ms_utf8_dec((const UB *)fb.s + i, fb.n - i, &cp);

		if ( d <= 0 ) break;
		f[nf++] = ms_norm_cp(cp);
		i += d;
	}
	mb_free(&fb);
	mb_putn(out, "", 0);

	for ( i = 0; i < nf; ) {
		UW	c = f[i];

		if ( c == '%' && i + 1 < nf ) {
			INT	j = i + 1, width = 0, prec = -1;
			BOOL	left = FALSE, zero = FALSE;
			UW	spec;
			MSV	a;
			char	s[512];
			MSBUF	sb;

			if ( f[j] == '-' ) { left = TRUE; j++; }
			if ( j < nf && f[j] == '0' ) { zero = TRUE; j++; }
			while ( j < nf && f[j] >= '0' && f[j] <= '9' ) { width = width * 10 + (INT)( f[j] - '0' ); j++; }
			if ( j < nf && f[j] == '.' ) {
				j++;
				prec = 0;
				while ( j < nf && f[j] >= '0' && f[j] <= '9' ) { prec = prec * 10 + (INT)( f[j] - '0' ); j++; }
			}
			spec = ( j < nf ) ? f[j] : 0;
			if ( spec == '%' ) { mb_putc(out, '%'); i = j + 1; continue; }
			if ( spec == 'n' ) { mb_putc(out, '\n'); i = j + 1; continue; }
			a = ( ai < nargs ) ? args[ai] : mv_num(NAN);
			ai++;
			s[0] = 0;
			switch ( spec ) {
			case 'd':
				mb_init(&sb);
				ms_num_text(trunc(mv_n(a)), &sb);
				snprintf(s, sizeof(s), "%s", sb.s ? sb.s : "0");
				mb_free(&sb);
				break;
			case 'x':
				snprintf(s, sizeof(s), "%x", (UW)ms_int32(trunc(mv_n(a))));
				break;
			case 'X':
				snprintf(s, sizeof(s), "%X", (UW)ms_int32(trunc(mv_n(a))));
				break;
			case 'f':
				snprintf(s, sizeof(s), "%.*f", ( prec >= 0 ) ? prec : 6, mv_n(a));
				break;
			case 'e':
				snprintf(s, sizeof(s), "%.*e", ( prec >= 0 ) ? prec : 6, mv_n(a));
				fix_exp(s);
				break;
			case 'E':
				snprintf(s, sizeof(s), "%.*E", ( prec >= 0 ) ? prec : 6, mv_n(a));
				fix_exp(s);
				break;
			case 'g': case 'G':
				fmt_g(mv_n(a), prec, (BOOL)( spec == 'G' ), s, sizeof(s));
				break;
			case 's': {
				INT	k = 0, n = 0;
				UW	cp;

				mb_init(&sb);
				mb_putn(&sb, "", 0);
				mv_text(a, &sb);
				/* the first prec characters */
				if ( prec >= 0 ) {
					for ( k = 0; k < sb.n && n < prec; n++ ) {
						k += ms_utf8_dec((const UB *)sb.s + k, sb.n - k, &cp);
					}
					sb.s[k] = 0;
				}
				snprintf(s, sizeof(s), "%s", sb.s);
				mb_free(&sb);
				break;
			}
			case 'c': {
				UB	u[5];
				INT	m = ms_utf8_enc((UW)ms_int32(mv_n(a)) & 0xFFFF, u);

				u[m] = 0;
				snprintf(s, sizeof(s), "%s", (char *)u);
				break;
			}
			default:
				i++;
				ai--;
				continue;
			}
			pad_to(out, s, width, left,
			       ( zero && ( spec == 'd' || spec == 'x' || spec == 'X' ) ) ? '0' : ' ');
			i = j + 1;
			continue;
		}
		if ( c == '\\' && i + 1 < nf && f[i + 1] == 'n' ) {
			mb_putc(out, '\n');
			i += 2;
			continue;
		}
		mb_putcp(out, c);
		i++;
	}
	ms_free(f);
}

/*
 * An argument that is the name of an array alone, as the whole array:
 * "%s" writes its characters up to the 0, as TADjs has it, where the
 * value of the name would be its first element. What is not an array
 * of that kind is its value as usual; "%d" of an array is its first
 * element either way.
 */
MSV rt_whole( MSTH *t, MSN *e, MSSCOPE *sc )
{
	if ( e != NULL && e->k == N_NAME && e->a == NULL && e->name2 == NULL
	  && ( e->flags & NF_SLICE ) == 0 && st_seg(e->name) == NULL ) {
		MSVAR	*v = scope_find(sc, e->name);

		if ( v != NULL && !v->scalar && !v->segref && v->type != 'S' && v->size > 1 ) {
			MSV	out = mv_arr(v->size);
			INT	i;

			for ( i = 0; i < v->size; i++ ) out.u.a->e[i] = var_get(v, i);
			return out;
		}
	}
	return ev(t, e, sc);
}

/* The format and its arguments of MESG, TEXT, LOG and SPRINTF, evaluated */
static void fmt_args( MSTH *t, MSN **v, INT n, MSSCOPE *sc, MSBUF *out )
{
	MSV	fmt, *args;
	INT	i;

	fmt = rt_whole(t, v[0], sc);
	args = ms_alloc(sizeof(MSV) * (size_t)( n + 1 ));
	for ( i = 1; i < n; i++ ) args[i - 1] = rt_whole(t, v[i], sc);
	rt_fmt(fmt, args, n - 1, out);
	for ( i = 1; i < n; i++ ) mv_drop(args[i - 1]);
	ms_free(args);
	mv_drop(fmt);
}

/* ---------------------------------------------------------------- segment states */

static MSV tx_value( MSSEG *s )
{
	return mv_chars(s->tx, s->ntx);
}

MSV seg_state( MSSEG *s, MSSTR *state )
{
	const char	*u = ms_upper(state)->s;

	if ( strcmp(u, "S") == 0 ) return mv_num(s->visible ? 1 : 0);
	if ( strcmp(u, "PID") == 0 ) return mv_num(( s->fs != NULL && s->fs->isvobj ) ? s->pid : MS_INVALID);
	/* places as the window has them; X0, Y0 where it was made */
	if ( strcmp(u, "X") == 0 ) return mv_num(s->x - st_vx());
	if ( strcmp(u, "Y") == 0 ) return mv_num(s->y - st_vy());
	if ( strcmp(u, "X0") == 0 ) return mv_num(s->hx - st_vx());
	if ( strcmp(u, "Y0") == 0 ) return mv_num(s->hy - st_vy());
	if ( strcmp(u, "W") == 0 ) return mv_num(s->w);
	if ( strcmp(u, "H") == 0 ) return mv_num(s->h);
	if ( strcmp(u, "V") == 0 ) {
		/* the text read as a number; 0 when it is not one */
		MSBUF	b;
		INT	i, k = 0;
		char	*end;
		double	d;

		mb_init(&b);
		mb_putn(&b, "", 0);
		for ( i = 0; i < s->ntx; i++ ) {
			UW	c = ms_norm_cp(s->tx[i]);

			if ( c == 0 ) break;
			mb_putcp(&b, c);
		}
		while ( b.s[k] == ' ' || b.s[k] == '\t' || b.s[k] == '\n' ) k++;
		d = strtod(b.s + k, &end);
		if ( end == b.s + k ) d = 0;
		mb_free(&b);
		return mv_num(d);
	}
	if ( strcmp(u, "TL") == 0 ) {
		INT	i;

		for ( i = 0; i < s->ntx && s->tx[i] != 0; i++ ) ;
		return mv_num(i);
	}
	if ( strcmp(u, "TX") == 0 ) return tx_value(s);
	if ( strcmp(u, "TFCOL") == 0 ) return mv_num(s->tfcol);
	if ( strcmp(u, "TBCOL") == 0 ) return mv_num(s->tbcol);
	if ( strcmp(u, "TSTYL") == 0 ) return mv_num(s->tstyl);
	if ( strcmp(u, "TSIZE") == 0 ) return mv_num(s->tsize);
	if ( strcmp(u, "TCGAP") == 0 ) return mv_num(s->tcgap);
	if ( strcmp(u, "TLGAP") == 0 ) return mv_num(s->tlgap);
	if ( strcmp(u, "TFONT") == 0 ) {
		INT	n;

		for ( n = 0; n < 12 && s->tfont[n] != 0; n++ ) ;
		return mv_chars(s->tfont, n);
	}
	return mv_num(0);
}

void seg_state_set( MSSEG *s, MSSTR *state, MSV v )
{
	const char	*u = ms_upper(state)->s;

	if ( strcmp(u, "TFCOL") == 0 ) s->tfcol = mv_n(v);
	else if ( strcmp(u, "TBCOL") == 0 ) { s->tbcol = mv_n(v); s->tbset = TRUE; }
	else if ( strcmp(u, "TSTYL") == 0 ) s->tstyl = mv_n(v);
	else if ( strcmp(u, "TSIZE") == 0 ) s->tsize = ( mv_n(v) == 0 ) ? 16 : mv_n(v);
	else if ( strcmp(u, "TCGAP") == 0 ) s->tcgap = mv_n(v);
	else if ( strcmp(u, "TLGAP") == 0 ) s->tlgap = mv_n(v);
	else if ( strcmp(u, "TFONT") == 0 ) {
		if ( v.t == V_ARR ) {
			INT	i;

			memset(s->tfont, 0, sizeof(s->tfont));
			for ( i = 0; i < 12 && i < v.u.a->n; i++ ) s->tfont[i] = (UW)ms_int32(mv_n(v.u.a->e[i]));
		}
		return;
	} else {
		return;			/* the others are not set by assignment */
	}
	ms_dirty();
}

/* Segments of one name handed out in turn: which one comes next */
typedef struct consume {
	MSSTR		*name;
	INT		n;
	struct consume	*next;
} CONSUME;

INT seg_consume( MSSTR *name )
{
	CONSUME	*c;

	for ( c = (CONSUME *)rt.segconsume; c != NULL; c = c->next ) {
		if ( c->name == name ) break;
	}
	if ( c == NULL ) {
		c = ms_alloc(sizeof(CONSUME));
		c->name = name;
		c->next = (CONSUME *)rt.segconsume;
		rt.segconsume = c;
	}
	return c->n++;
}

/* ---------------------------------------------------------------- the stage statements */

static INT seg_list( MSTH *t, MSN *s, MSSCOPE *sc, MSSEG ***p_out )
{
	MSSEG	**out = ms_alloc(sizeof(MSSEG *) * (size_t)( s->nv + 1 ));
	INT	i, n = 0;

	for ( i = 0; i < s->nv; i++ ) {
		MSSEG	*g = th_seg(t, s->v[i], sc);

		if ( g != NULL ) out[n++] = g;
	}
	*p_out = out;
	return n;
}

static INT steps_of( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSV	v;
	INT	n;

	if ( s->a == NULL ) return 10;
	v = ev(t, s->a, sc);
	n = mv_i(v);
	mv_drop(v);
	return n;
}

/* A segment hidden that was taking input: the input ends */
static void input_check( void )
{
	MSSEG	*g = st_input_seg();

	if ( g != NULL && !g->visible ) st_input_end();
}

/*
 * SCENE: every segment hidden, then these shown from the back forward,
 * and the window made to show the figure from the top left corner of
 * the scene (the corners of the segments' groups).
 */
static INT st_scene( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSEG	*g, **segs;
	INT	n, i;
	double	minx = INFINITY, miny = INFINITY;

	for ( g = st_first(); g != NULL; g = g->next ) g->visible = FALSE;
	input_check();
	if ( s->nv == 0 ) {
		if ( !( s->flags & NF_DEFERRED ) ) ms_dirty();
		return FL_NONE;
	}
	n = seg_list(t, s, sc, &segs);
	for ( i = 0; i < n; i++ ) {
		double	lx = segs[i]->x + segs[i]->lox, ly = segs[i]->y + segs[i]->loy;

		if ( lx < minx ) minx = lx;
		if ( ly < miny ) miny = ly;
	}
	if ( !isfinite(minx) ) minx = 0;
	if ( !isfinite(miny) ) miny = 0;
	st_view(minx, miny);
	for ( i = 0; i < n; i++ ) st_front(segs[i]);
	if ( s->flags & NF_DEFERRED ) {
		for ( i = 0; i < n; i++ ) segs[i]->visible = TRUE;
	} else if ( s->name2 != NULL ) {
		ms_transition(segs, n, NULL, 0, s->name2, steps_of(t, s, sc), TRUE);
	} else {
		for ( i = 0; i < n; i++ ) segs[i]->visible = TRUE;
		ms_dirty();
	}
	ms_free(segs);
	return FL_NONE;
}

static INT st_appear( MSTH *t, MSN *s, MSSCOPE *sc, BOOL show )
{
	MSSEG	**segs;
	INT	n, i, k = 0;

	n = seg_list(t, s, sc, &segs);
	for ( i = 0; i < n; i++ ) {
		if ( segs[i]->visible != show ) {
			if ( show ) st_front(segs[i]);
			segs[k++] = segs[i];
		}
	}
	if ( s->flags & NF_DEFERRED ) {
		for ( i = 0; i < k; i++ ) segs[i]->visible = show;
	} else if ( s->name2 != NULL && k > 0 ) {
		/* the effect over the rectangle round these alone */
		if ( show ) ms_transition(segs, k, NULL, 0, s->name2, steps_of(t, s, sc), FALSE);
		else ms_transition(NULL, 0, segs, k, s->name2, steps_of(t, s, sc), FALSE);
	} else {
		for ( i = 0; i < k; i++ ) segs[i]->visible = show;
		ms_dirty();
	}
	if ( !show ) input_check();
	ms_free(segs);
	return FL_NONE;
}

static INT dup_count;

/*
 * MOVE: by (x, y); to (x, y) from another segment's top left corner; to
 * (x, y) of the window (@); back to where it was made (no place, and
 * its copies go). With DUP a copy is left where it was.
 */
static INT st_move( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSV	vx, vy;
	BOOL	hasx = (BOOL)( s->a != NULL ), hasy = (BOOL)( s->b != NULL );
	double	dx = 0, dy = 0;
	MSSEG	*base = NULL;
	INT	i;

	if ( hasx ) { vx = ev(t, s->a, sc); dx = mv_n(vx); mv_drop(vx); }
	if ( hasy ) { vy = ev(t, s->b, sc); dy = mv_n(vy); mv_drop(vy); }
	if ( s->c != NULL ) base = th_seg(t, s->c, sc);
	for ( i = 0; i < s->nv; i++ ) {
		MSSEG	*g = th_seg(t, s->v[i], sc);
		double	tx, ty;

		if ( g == NULL ) continue;
		if ( !hasx && !hasy ) {
			MSSEG	*q, *nx;

			g->x = g->hx;
			g->y = g->hy;
			for ( q = st_first(); q != NULL; q = nx ) {
				nx = q->next;
				if ( q->dupof == g ) st_remove(q);
			}
			continue;
		}
		if ( s->flags & NF_BASE_AT ) {
			tx = dx + st_vx();
			ty = dy + st_vy();
		} else if ( s->c != NULL ) {
			tx = ( base != NULL ) ? base->x + dx : g->x;
			ty = ( base != NULL ) ? base->y + dy : g->y;
		} else {
			tx = g->x + dx;
			ty = g->y + dy;
		}
		if ( ( s->flags & NF_DUP ) && !( g->fs != NULL && ( g->fs->haslink || g->fs->alltext ) ) ) {
			/* a copy stays where it was; texts and virtual objects are not copied */
			if ( dup_count < 16380 ) {
				char	nm[160];
				MSSEG	*d;

				snprintf(nm, sizeof(nm), "%s_dup%d", g->name->s, ++dup_count);
				d = st_add(ms_intern_z(nm), g->fs);
				d->x = d->x0 = d->hx = g->x;
				d->y = d->y0 = d->hy = g->y;
				d->w = g->w;
				d->h = g->h;
				d->lox = g->lox;
				d->loy = g->loy;
				d->tsize = g->tsize;
				d->shared = g->shared;
				d->dupof = g;
			}
		}
		g->x = tx;
		g->y = ty;
	}
	if ( !( s->flags & NF_DEFERRED ) ) ms_dirty();
	return FL_NONE;
}

static INT st_text( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSEG	*g = th_seg(t, s->a, sc);
	MSBUF	b;
	MSV	chars;

	if ( g == NULL ) {
		th_fail(t, "未定義のセグメント: %s", ( s->a != NULL && s->a->name != NULL ) ? s->a->name->s : "?");
		return FL_STOP;
	}
	mb_init(&b);
	mb_putn(&b, "", 0);
	if ( s->nv > 0 ) fmt_args(t, s->v, s->nv, sc, &b);
	chars = mv_cstr(b.s);
	mb_free(&b);
	{
		UW	*c = ms_alloc(sizeof(UW) * (size_t)( chars.u.a->n + 1 ));
		INT	i;

		for ( i = 0; i < chars.u.a->n; i++ ) c[i] = (UW)ms_int32(mv_n(chars.u.a->e[i]));
		st_text_set(g, c, chars.u.a->n - 1);
		ms_free(c);
	}
	mv_drop(chars);
	g->textover = TRUE;
	ms_dirty();
	return FL_NONE;
}

/* SETSEG / COPYSEG: the segment on the left, made when the variable names none yet */
static MSSEG *seg_for_assign( MSTH *t, MSN *s, MSSCOPE *sc, MSVAR **p_var, INT *p_idx )
{
	MSSEG	*g = st_seg(s->name);
	MSVAR	*v;
	INT	idx = 0;
	MSV	e;
	MSSTR	*nm;
	char	buf[160];

	*p_var = NULL;
	if ( g != NULL ) return g;
	v = scope_find(sc, s->name);
	if ( v == NULL ) return NULL;
	if ( s->a != NULL ) {
		MSV	iv = ev(t, s->a, sc);

		idx = mv_i(iv);
		mv_drop(iv);
	}
	*p_var = v;
	*p_idx = idx;
	e = var_get(v, idx);
	nm = ( e.t == V_STR ) ? e.u.s : NULL;
	mv_drop(e);
	if ( nm != NULL && st_seg(nm) != NULL ) return st_seg(nm);
	/* a new segment: the name the array element has, or the array's name and the index */
	if ( nm == NULL || nm->len == 0 ) {
		snprintf(buf, sizeof(buf), "%s%d", s->name->s, idx);
		nm = ms_intern_z(buf);
	}
	if ( st_seg(nm) != NULL ) {
		INT	k = 1;

		do {
			snprintf(buf, sizeof(buf), "%s_%d", nm->s, k++);
		} while ( st_seg(ms_intern_z(buf)) != NULL );
		nm = ms_intern_z(buf);
	}
	g = st_add(nm, NULL);
	g->visible = FALSE;
	rt_bind_segment(g);
	var_set(v, idx, mv_str(nm));
	return g;
}

static INT st_setseg( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSVAR	*v;
	INT	idx = 0;
	MSSEG	*dst = seg_for_assign(t, s, sc, &v, &idx), *src;

	if ( dst == NULL ) return FL_NONE;
	src = th_seg(t, s->v[0], sc);
	if ( src == NULL ) {
		/* the empty segment */
		dst->fs = NULL;
		dst->shared = NULL;
		st_text_set(dst, NULL, 0);
		dst->w = dst->h = 0;
		ms_dirty();
		return FL_NONE;
	}
	dst->fs = src->fs;
	st_text_set(dst, src->tx, src->ntx > 0 ? src->ntx - 1 : 0);
	dst->w = src->w;
	dst->h = src->h;
	/* the place is taken only the first time: later ones change the look alone */
	if ( !dst->positioned ) {
		dst->x = src->x;
		dst->y = src->y;
		dst->x0 = src->x0;
		dst->y0 = src->y0;
		dst->hx = src->hx;
		dst->hy = src->hy;
		dst->lox = src->lox;
		dst->loy = src->loy;
		dst->positioned = TRUE;
	}
	dst->shared = src;
	ms_dirty();
	return FL_NONE;
}

static INT st_copyseg( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSVAR	*v;
	INT	idx = 0, i;
	MSSEG	*dst = seg_for_assign(t, s, sc, &v, &idx), *first = NULL;
	double	w = 0, h = 0;

	if ( dst == NULL ) return FL_NONE;
	for ( i = 0; i < s->nv; i++ ) {
		MSSEG	*src = th_seg(t, s->v[i], sc);

		if ( src == NULL ) continue;
		if ( first == NULL ) first = src;
		if ( src->w > w ) w = src->w;
		if ( src->h > h ) h = src->h;
	}
	/* the look of the first; more than one are not laid over each other */
	dst->fs = ( first != NULL ) ? ( first->shared != NULL ? first->shared->fs : first->fs ) : NULL;
	dst->shared = NULL;
	dst->w = w;
	dst->h = h;
	if ( first != NULL ) {
		if ( !dst->positioned ) {
			dst->x = first->x;
			dst->y = first->y;
			dst->positioned = TRUE;
		}
		dst->x0 = first->x0;
		dst->y0 = first->y0;
		dst->hx = first->hx;
		dst->hy = first->hy;
		dst->lox = first->lox;
		dst->loy = first->loy;
	}
	ms_dirty();
	return FL_NONE;
}

static INT st_input( MSTH *t, MSN *s, MSSCOPE *sc, BOOL kanji )
{
	MSSEG	*g;
	INT	x = -1, y = -1;

	if ( s->a == NULL ) {
		st_input_end();
		return FL_NONE;
	}
	g = th_seg(t, s->a, sc);
	if ( g == NULL ) return FL_NONE;
	if ( s->b != NULL ) { MSV v = ev(t, s->b, sc); x = mv_i(v); mv_drop(v); }
	if ( s->c != NULL ) { MSV v = ev(t, s->c, sc); y = mv_i(v); mv_drop(v); }
	st_input_begin(g, kanji, x, y);
	return FL_NONE;
}

/* ---------------------------------------------------------------- records of objects */

typedef struct fcache {
	MSSTR		*path;
	TS_UUID		uuid;
	BOOL		ro;
	struct fcache	*next;
} FCACHE;

static FCACHE	*fopened;

/* The object a path names: a virtual object of the figure by its name, a path, or a UUID */
static BOOL path_uuid( MSSTR *p, TS_UUID *u )
{
	return ms_path_resolve(p->s, u);
}

static MSSTR *path_arg( MSTH *t, MSN *e, MSSCOPE *sc )
{
	MSV	v;
	MSSTR	*n;

	/* an array variable given bare is its whole text, not its first element */
	if ( e->k == N_NAME && e->a == NULL && !( e->flags & NF_SLICE ) && e->name2 == NULL
	  && st_seg(e->name) == NULL && ms_macro(&ms_prog, e->name) == NULL ) {
		MSVAR	*var = scope_find(sc, e->name);

		if ( var != NULL && var->size > 1 ) {
			MSV	a = mv_arr(var->size);
			INT	i;

			for ( i = 0; i < var->size; i++ ) {
				mv_drop(a.u.a->e[i]);
				a.u.a->e[i] = var_get(var, i);
			}
			n = mv_name(a);
			mv_drop(a);
			return n;
		}
	}
	v = ev(t, e, sc);
	if ( v.t == V_NUM ) {
		MSBUF	b;

		mb_init(&b);
		ms_num_text(v.u.n, &b);
		n = ms_intern(b.s, b.n);
		mb_free(&b);
	} else {
		n = mv_name(v);
	}
	mv_drop(v);
	return n;
}

static FCACHE *fopen_of( MSSTR *p, BOOL ro )
{
	FCACHE	*c;
	TS_UUID	u;

	for ( c = fopened; c != NULL; c = c->next ) if ( c->path == p ) return c;
	if ( !path_uuid(p, &u) ) return NULL;
	c = ms_alloc(sizeof(FCACHE));
	c->path = p;
	c->uuid = u;
	c->ro = ro;
	c->next = fopened;
	fopened = c;
	return c;
}

/*
 * A text record read as the micro script sees one: its paragraphs as
 * 16 bit characters between a text segment's start and end, each
 * paragraph a page (0x0C) unless the text has its own pages.
 */
static UB *tad_bytes( const char *xml, INT len, INT *p_n )
{
	MSXDOC	d;
	MSX	*doc, *p;
	MSBUF	txt, out;
	BOOL	ff = FALSE, first = TRUE;
	INT	i;

	msx_parse(&d, xml, len);
	doc = msx_find(d.root, "document");
	mb_init(&txt);
	mb_putn(&txt, "", 0);
	if ( doc != NULL ) {
		for ( p = doc->first; p != NULL; p = p->next ) {
			MSBUF	para;

			if ( !msx_is(p, "p") ) continue;
			mb_init(&para);
			mb_putn(&para, "", 0);
			msx_text(p, &para, TRUE);
			if ( para.n > 0 ) {
				if ( strchr(para.s, '\f') != NULL ) ff = TRUE;
				mb_putn(&txt, para.s, para.n);
				mb_putc(&txt, 0x1F);		/* a mark between paragraphs */
			}
			mb_free(&para);
		}
	}
	mb_init(&out);
#define PUT16(v)	do { UH _h = (UH)(v); mb_putc(&out, (char)( _h & 0xFF )); mb_putc(&out, (char)( _h >> 8 )); } while ( 0 )
	PUT16(0xFFE1);
	PUT16(0);
	for ( i = 0; i < txt.n; ) {
		UW	cp;
		INT	k = ms_utf8_dec((const UB *)txt.s + i, txt.n - i, &cp);

		if ( k <= 0 ) break;
		i += k;
		if ( cp == 0x1F ) {
			/* between paragraphs: a page unless the text has its own */
			if ( i < txt.n && !ff ) PUT16(0x0C);
			continue;
		}
		(void)first;
		if ( cp <= 0xFFFF ) PUT16(cp);
	}
	PUT16(0xFFE2);
	PUT16(0);
#undef PUT16
	mb_free(&txt);
	*p_n = out.n;
	return (UB *)out.s;
}

/* A record's bytes, text records turned as above */
static UB *rec_bytes( const TS_UUID *u, INT recno, INT *p_n, INT *p_nrec )
{
	ID	k = ob_opn_obj(u, OB_OP_R);
	T_OBREC	recs[64];
	INT	cnt = 0;
	SZ	asz = 0;
	UB	*buf;

	*p_n = 0;
	*p_nrec = 0;
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, recs, 64, &cnt) < E_OK ) cnt = 0;
	*p_nrec = cnt;
	if ( recno >= cnt ) { ob_cls_obj(k); return NULL; }
	buf = ms_alloc((size_t)recs[recno].size + 1);
	if ( recs[recno].size > 0 ) (void)ob_rea_rec(k, recno, 0, buf, recs[recno].size, &asz);
	ob_cls_obj(k);
	if ( recs[recno].rt == 1 && asz > 0 && strstr((char *)buf, "<tad") != NULL ) {
		INT	n;
		UB	*b = tad_bytes((char *)buf, (INT)asz, &n);

		ms_free(buf);
		*p_n = n;
		return b;
	}
	*p_n = (INT)asz;
	return buf;
}

/* The place (a variable and an element) an argument names */
static MSVAR *place_of( MSTH *t, MSN *e, MSSCOPE *sc, INT *p_start )
{
	MSVAR	*v;

	*p_start = 0;
	if ( e == NULL || e->k != N_NAME ) return NULL;
	v = scope_find(sc, e->name);
	if ( v == NULL ) return NULL;
	if ( ( e->flags & NF_SLICE ) && e->a != NULL ) {
		MSV	x = ev(t, e->a, sc);

		*p_start = mv_i(x);
		mv_drop(x);
	} else if ( e->a != NULL ) {
		MSV	x = ev(t, e->a, sc);

		*p_start = mv_i(x);
		mv_drop(x);
	}
	return v;
}

static double num_arg( MSTH *t, MSN *e, MSSCOPE *sc )
{
	MSV	v = ev(t, e, sc);
	double	d = mv_n(v);

	mv_drop(v);
	return d;
}

static INT st_fopen( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*p = ( s->nv > 0 ) ? path_arg(t, s->v[0], sc) : NULL;
	BOOL	ro = FALSE;

	if ( p == NULL ) { rt.lasterr = 17; return FL_NONE; }
	if ( s->nv > 1 ) {
		MSV	m = ev(t, s->v[1], sc);
		MSSTR	*mn = mv_name(m);

		ro = (BOOL)( mn != NULL && ( mn->s[0] == 'R' || mn->s[0] == 'r' ) );
		mv_drop(m);
	}
	rt.lasterr = ( fopen_of(p, ro) != NULL ) ? 0 : 32;
	return FL_NONE;
}

static INT st_fclose( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*p = ( s->nv > 0 ) ? path_arg(t, s->v[0], sc) : NULL;
	FCACHE	**pp;

	for ( pp = &fopened; *pp != NULL; pp = &(*pp)->next ) {
		if ( (*pp)->path == p ) {
			FCACHE	*c = *pp;

			*pp = c->next;
			ms_free(c);
			break;
		}
	}
	rt.lasterr = 0;
	return FL_NONE;
}

/*
 * Bytes and the elements of an array: an element is as many bytes as its
 * type is wide (B 1, C 2, F 8 as a double, the others 4), little end
 * first. How many elements from `start` the bytes go to or come from.
 */
static INT el_width( const MSVAR *v )
{
	return ( v->type == 'B' ) ? 1 : ( v->type == 'C' ) ? 2 : ( v->type == 'F' ) ? 8 : 4;
}

/* Room from `start` to the array's end, in bytes */
static INT el_room( const MSVAR *v, INT start )
{
	INT	n = v->size - start;

	return ( n > 0 ) ? n * el_width(v) : 0;
}

static void bytes_to_els( MSVAR *v, INT start, const UB *b, INT n )
{
	INT	w = el_width(v), i, k;

	for ( i = 0; i * w < n && start + i < v->size; i++ ) {
		const UB	*p = b + i * w;
		UB		tmp[8];
		INT		m = ( n - i * w < w ) ? n - i * w : w;

		memset(tmp, 0, sizeof(tmp));
		if ( m < w ) {
			/* a piece of an element: the rest of it stays as it was */
			MSV	e = var_get(v, start + i);
			UD	old = (UD)(D)ms_int32(mv_n(e));

			mv_drop(e);
			for ( k = 0; k < w && k < 8; k++ ) tmp[k] = (UB)( old >> ( 8 * k ) );
		}
		memcpy(tmp, p, (size_t)m);
		if ( w == 8 ) {
			double	d;

			memcpy(&d, tmp, 8);
			var_set(v, start + i, mv_num(d));
		} else if ( w == 4 ) {
			INT	x;

			memcpy(&x, tmp, 4);
			var_set(v, start + i, mv_num(x));
		} else if ( w == 2 ) {
			var_set(v, start + i, mv_num((double)( tmp[0] | ( tmp[1] << 8 ) )));
		} else {
			var_set(v, start + i, mv_num(tmp[0]));
		}
	}
}

static void els_to_bytes( const MSVAR *v, INT start, UB *b, INT n )
{
	INT	w = el_width(v), i;

	for ( i = 0; i * w < n; i++ ) {
		UB	tmp[8];
		INT	m = ( n - i * w < w ) ? n - i * w : w;
		MSV	e = ( start + i < v->size ) ? var_get(v, start + i) : mv_num(0);
		double	d = mv_n(e);

		mv_drop(e);
		if ( w == 8 ) {
			memcpy(tmp, &d, 8);
		} else {
			INT	x = ms_int32(d);

			memcpy(tmp, &x, 4);
		}
		memcpy(b + i * w, tmp, (size_t)m);
	}
}

static INT st_fread( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*p;
	double	recno, off, size;
	FCACHE	*c;
	UB	*data;
	INT	n, nrec, actual, start;
	MSVAR	*dst;

	if ( s->nv < 5 ) { rt.lasterr = 17; return FL_NONE; }
	p = path_arg(t, s->v[0], sc);
	recno = trunc(num_arg(t, s->v[1], sc));
	off = trunc(num_arg(t, s->v[2], sc));
	size = trunc(num_arg(t, s->v[3], sc));
	if ( !isfinite(recno) || !isfinite(off) || !isfinite(size) || recno < 0 || off < 0 || size < 0 ) {
		rt.lasterr = 17;
		return FL_NONE;
	}
	c = ( p != NULL ) ? fopen_of(p, FALSE) : NULL;
	if ( c == NULL ) { rt.lasterr = 32; return FL_NONE; }
	data = rec_bytes(&c->uuid, (INT)recno, &n, &nrec);
	if ( (INT)recno >= nrec || off > n ) { ms_free(data); rt.lasterr = 17; return FL_NONE; }
	dst = place_of(t, s->v[4], sc, &start);
	actual = (INT)( ( size < n - off ) ? size : n - off );
	if ( dst != NULL && actual > el_room(dst, start) ) actual = el_room(dst, start);
	if ( actual < 0 ) actual = 0;
	if ( dst != NULL && data != NULL && actual > 0 ) bytes_to_els(dst, start, data + (INT)off, actual);
	if ( s->nv > 5 ) {
		INT	rs;
		MSVAR	*sv = place_of(t, s->v[5], sc, &rs);

		if ( sv != NULL ) var_set(sv, rs, mv_num(n));
	}
	ms_free(data);
	rt.lasterr = 0;
	return FL_NONE;
}

static INT st_fwrite( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*p;
	double	recno, off, size;
	FCACHE	*c;
	ID	k;
	T_OBREC	recs[64];
	INT	cnt = 0, start, n;
	MSVAR	*src;
	SZ	asz;

	if ( s->nv < 5 ) { rt.lasterr = 17; return FL_NONE; }
	p = path_arg(t, s->v[0], sc);
	recno = trunc(num_arg(t, s->v[1], sc));
	off = trunc(num_arg(t, s->v[2], sc));
	size = trunc(num_arg(t, s->v[3], sc));
	if ( !isfinite(recno) || !isfinite(off) || !isfinite(size) || recno < 0 || size < 0
	  || ( off < 0 && off != -1 ) || recno >= 4096 ) {
		rt.lasterr = 17;
		return FL_NONE;
	}
	c = ( p != NULL ) ? fopen_of(p, FALSE) : NULL;
	if ( c == NULL ) { rt.lasterr = 32; return FL_NONE; }
	if ( c->ro ) { rt.lasterr = 17; return FL_NONE; }
	k = ob_opn_obj(&c->uuid, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( k <= 0 ) { rt.lasterr = 32; return FL_NONE; }
	if ( ob_lst_rec(k, recs, 64, &cnt) < E_OK ) cnt = 0;
	if ( (INT)recno > cnt ) { ob_cls_obj(k); rt.lasterr = 32; return FL_NONE; }
	if ( (INT)recno == cnt ) {
		INT	rn;

		if ( off != 0 ) { ob_cls_obj(k); rt.lasterr = 32; return FL_NONE; }
		if ( ob_apd_rec(k, OB_RT_SYSDATA, 0, &rn) < E_OK ) { ob_cls_obj(k); rt.lasterr = 32; return FL_NONE; }
	} else if ( off > (double)recs[(INT)recno].size ) {
		ob_cls_obj(k);
		rt.lasterr = 32;
		return FL_NONE;
	}
	if ( off == -1 ) {
		/* cut to `size` bytes, when it is longer */
		if ( (INT)recno < cnt && (double)recs[(INT)recno].size > size ) (void)ob_trn_rec(k, (INT)recno, (UD)size);
	} else {
		src = place_of(t, s->v[4], sc, &start);
		n = (INT)size;
		if ( src == NULL ) n = 0;
		else if ( n > el_room(src, start) ) n = el_room(src, start);
		if ( n > 0 ) {
			UB	*b = ms_alloc((size_t)n + 8);

			els_to_bytes(src, start, b, n);
			(void)ob_wri_rec(k, (INT)recno, (D)off, b, n, &asz);
			ms_free(b);
		}
	}
	if ( s->nv > 5 ) {
		INT	rs;
		MSVAR	*sv = place_of(t, s->v[5], sc, &rs);

		if ( ob_lst_rec(k, recs, 64, &cnt) >= E_OK && (INT)recno < cnt && sv != NULL ) {
			var_set(sv, rs, mv_num((double)recs[(INT)recno].size));
		}
	}
	ob_cls_obj(k);
	rt.lasterr = 0;
	return FL_NONE;
}

/* ---------------------------------------------------------------- devices */

/*
 * DOPEN and the others: a device by its name, its data (record 1 of its
 * object: a disk's bytes at their places, a stream's bytes as they
 * come). A device not opened is opened for the statement.
 */
#define DEV_MAX		8

typedef struct {
	MSSTR	*name;
	ID	key;
} DEVOPEN;

static DEVOPEN	devs[DEV_MAX];

static BOOL dev_uuid( MSSTR *name, TS_UUID *u )
{
	TS_UUID	ids[128];
	T_OBREF	r;
	INT	cnt = 0, i;

	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, ids, 128, &cnt) < E_OK ) return FALSE;
	for ( i = 0; i < cnt && i < 128; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK && strcmp((const char *)r.name, name->s) == 0 ) {
			*u = ids[i];
			return TRUE;
		}
	}
	return FALSE;
}

static UINT mode_ops( MSSTR *m )
{
	if ( m == NULL || m->len == 0 ) return OB_OP_R | OB_OP_WRITE;
	switch ( m->s[0] ) {
	case 'R': case 'r':	return OB_OP_R;
	case 'W': case 'w':	return OB_OP_ATRRD | OB_OP_WRITE;
	}
	return OB_OP_R | OB_OP_WRITE;
}

static DEVOPEN *dev_find( MSSTR *name )
{
	INT	i;

	for ( i = 0; i < DEV_MAX; i++ ) if ( devs[i].key > 0 && devs[i].name == name ) return &devs[i];
	return NULL;
}

static ID dev_open( MSSTR *name, UINT ops )
{
	TS_UUID	u;
	INT	i;
	ID	k;

	if ( !dev_uuid(name, &u) ) return E_NOEXS;
	k = ob_opn_obj(&u, ops);
	if ( k <= 0 ) return k;
	for ( i = 0; i < DEV_MAX; i++ ) {
		if ( devs[i].key <= 0 ) {
			devs[i].name = name;
			devs[i].key = k;
			return k;
		}
	}
	ob_cls_obj(k);
	return E_LIMIT;
}

static INT st_dev( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*name = ( s->nv > 0 ) ? path_arg(t, s->v[0], sc) : NULL;
	DEVOPEN	*d;
	ID	k;
	BOOL	once = FALSE;

	if ( name == NULL ) { rt.lasterr = 16; return FL_NONE; }
	d = dev_find(name);
	if ( s->op == 'O' ) {			/* DOPEN */
		MSSTR	*m = ( s->nv > 1 ) ? path_arg(t, s->v[1], sc) : NULL;

		if ( d != NULL ) { rt.lasterr = 0; return FL_NONE; }
		rt.lasterr = ( dev_open(name, mode_ops(m)) > 0 ) ? 0 : 16;
		return FL_NONE;
	}
	if ( s->op == 'C' ) {			/* DCLOSE */
		if ( d != NULL ) {
			ob_cls_obj(d->key);
			d->key = 0;
		}
		rt.lasterr = 0;
		return FL_NONE;
	}
	if ( d != NULL ) {
		k = d->key;
	} else {
		k = dev_open(name, OB_OP_R | OB_OP_WRITE);
		if ( k <= 0 ) { rt.lasterr = 16; return FL_NONE; }
		once = TRUE;
		d = dev_find(name);
	}
	if ( s->nv < 4 ) {
		rt.lasterr = 17;
	} else {
		double	off = trunc(num_arg(t, s->v[1], sc)), size = trunc(num_arg(t, s->v[2], sc));
		INT	start, n;
		MSVAR	*v = place_of(t, s->v[3], sc, &start);
		SZ	asz = 0;
		UB	*b;

		n = ( v != NULL && size > 0 ) ? (INT)size : 0;
		if ( v != NULL && n > el_room(v, start) ) n = el_room(v, start);
		b = ms_alloc((size_t)n + 8);
		if ( s->op == 'R' ) {
			rt.lasterr = ( ob_rea_rec(k, 1, (D)off, b, n, &asz) >= E_OK ) ? 0 : 16;
			if ( rt.lasterr == 0 && asz > 0 ) bytes_to_els(v, start, b, (INT)asz);
		} else {
			els_to_bytes(v, start, b, n);
			rt.lasterr = ( ob_wri_rec(k, 1, (D)off, b, n, &asz) >= E_OK ) ? 0 : 16;
		}
		ms_free(b);
		if ( s->nv > 4 ) {
			INT	rs;
			MSVAR	*sv = place_of(t, s->v[4], sc, &rs);

			if ( sv != NULL ) var_set(sv, rs, mv_num((double)asz));
		}
	}
	if ( once && d != NULL ) {
		ob_cls_obj(d->key);
		d->key = 0;
	}
	return FL_NONE;
}

/* ---------------------------------------------------------------- processes */

/*
 * PROCESS pid, program, message: a program object started as a process
 * of its own, the message (its words split by spaces) as its argument.
 * It goes on when this one ends. PWAIT waits for it to end.
 */
static INT st_process( MSTH *t, MSN *s, MSSCOPE *sc )
{
	MSSTR	*path, *msg = NULL;
	TS_UUID	prog, pu;
	T_OBCRE	c;
	INT	start, pid = 0;
	MSVAR	*pv;

	if ( s->nv < 2 ) { rt.lasterr = 48; return FL_NONE; }
	pv = place_of(t, s->v[0], sc, &start);
	path = path_arg(t, s->v[1], sc);
	if ( s->nv > 2 ) msg = path_arg(t, s->v[2], sc);
	if ( path == NULL || !ms_path_resolve(path->s, &prog) ) { rt.lasterr = 32; return FL_NONE; }
	memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	if ( msg != NULL && msg->len > 0 ) {
		c.arg = msg->s;
		c.argsz = msg->len + 1;
	}
	if ( ob_cre_obj(&c, &pu) < E_OK ) { rt.lasterr = 48; return FL_NONE; }
	pid = ms_pid_of(&pu);
	if ( pv != NULL ) var_set(pv, start, mv_num(pid));
	rt.lasterr = 0;
	return FL_NONE;
}

static INT st_pwait( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	pid, st = 0;
	MSVAR	*cv = NULL;
	double	tmo = -1;
	UD	begin = ms_now_ms();
	T_PSTS	ps;

	if ( s->nv < 1 ) { rt.lasterr = 48; return FL_NONE; }
	pid = ms_int32(num_arg(t, s->v[0], sc));
	if ( s->nv > 1 ) cv = place_of(t, s->v[1], sc, &st);
	if ( s->a != NULL ) tmo = num_arg(t, s->a, sc);
	t->waiting = TRUE;
	while ( !th_stop(t) ) {
		ER	er = ts_wai_prc(pid, &ps, TMO_POL);

		if ( er >= E_OK ) {
			if ( cv != NULL ) var_set(cv, st, mv_num(ps.exitcd));
			rt.lasterr = 0;
			break;
		}
		if ( er != E_TMOUT ) { rt.lasterr = 48; break; }
		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
		if ( tmo == 0 ) { rt.lasterr = 1; break; }
		if ( tmo > 0 && (double)( ms_now_ms() - begin ) >= tmo * 1000 ) { rt.lasterr = 1; break; }
		ms_yield(ms_now_ms() + 50);
	}
	t->waiting = FALSE;
	return FL_NONE;
}

/* ---------------------------------------------------------------- virtual objects */

#define VARG_MAX	32

typedef struct {
	TS_UUID	target[VARG_MAX];
	MSSEG	*seg[VARG_MAX];		/* the virtual object segment it came from, or NULL */
	INT	nt;
	char	name[VARG_MAX][64];	/* accessories by name */
	INT	nn;
} VARGS;

/*
 * What VOPEN, VCLOSE and VWAIT name: virtual object segments, and
 * strings -- the name of an accessory, or a path. Nothing named in a
 * procedure a virtual object segment's event started: that segment.
 */
static void vargs( MSTH *t, MSN *s, MSSCOPE *sc, VARGS *a, BOOL own )
{
	INT	i;

	memset(a, 0, sizeof(*a));
	if ( s->nv == 0 && own && t->evkey != NULL ) {
		MSSEG	*g = st_seg(t->evkey);

		if ( g != NULL && g->fs != NULL && g->fs->isvobj ) {
			a->seg[0] = g;
			a->target[a->nt++] = g->fs->target;
		}
		return;
	}
	for ( i = 0; i < s->nv; i++ ) {
		MSN	*x = s->v[i];
		MSSEG	*g = ( x->k == N_NAME ) ? th_seg(t, x, sc) : NULL;
		MSSTR	*p;
		TS_UUID	u;

		if ( g != NULL ) {
			if ( g->fs != NULL && g->fs->isvobj && a->nt < VARG_MAX ) {
				a->seg[a->nt] = g;
				a->target[a->nt++] = g->fs->target;
			}
			continue;
		}
		p = path_arg(t, x, sc);
		if ( p == NULL || p->len == 0 ) continue;
		if ( ms_path_resolve(p->s, &u) ) {
			if ( a->nt < VARG_MAX ) a->target[a->nt++] = u;
		} else if ( a->nn < VARG_MAX ) {
			strncpy(a->name[a->nn++], p->s, 63);
		}
	}
}

/* VOPEN: each opened as a double click opens it; what it started is the segment's PID */
static INT st_vopen( MSTH *t, MSN *s, MSSCOPE *sc )
{
	VARGS	*a = ms_alloc(sizeof(VARGS));
	INT	i, pid;

	vargs(t, s, sc, a, TRUE);
	rt.lasterr = 0;
	for ( i = 0; i < a->nt; i++ ) {
		if ( ms_vopen_obj(&a->target[i], &pid) < E_OK ) { rt.lasterr = 48; continue; }
		if ( a->seg[i] != NULL && pid > 0 ) a->seg[i]->pid = pid;
	}
	for ( i = 0; i < a->nn; i++ ) {
		if ( ms_vopen_tool(a->name[i], &pid) < E_OK ) rt.lasterr = 48;
	}
	ms_free(a);
	return FL_NONE;
}

static INT st_vclose( MSTH *t, MSN *s, MSSCOPE *sc )
{
	VARGS	*a = ms_alloc(sizeof(VARGS));

	vargs(t, s, sc, a, FALSE);
	ms_vclose(a->target, a->nt, (const char (*)[64])a->name, a->nn);
	ms_free(a);
	rt.lasterr = 0;
	return FL_NONE;
}

/* VWAIT: until every window opened from them has closed; a time limit in seconds */
static INT st_vwait( MSTH *t, MSN *s, MSSCOPE *sc )
{
	VARGS	*a = ms_alloc(sizeof(VARGS));
	double	tmo = ( s->a != NULL ) ? num_arg(t, s->a, sc) : -1;
	UD	begin = ms_now_ms();

	vargs(t, s, sc, a, FALSE);
	rt.lasterr = 0;
	t->waiting = TRUE;
	while ( !th_stop(t) && ms_vopen_any(a->target, a->nt, (const char (*)[64])a->name, a->nn) ) {
		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
		if ( tmo == 0 || ( tmo > 0 && (double)( ms_now_ms() - begin ) >= tmo * 1000 ) ) {
			rt.lasterr = 1;
			break;
		}
		ms_yield(ms_now_ms() + 100);
	}
	t->waiting = FALSE;
	ms_free(a);
	return FL_NONE;
}

/* ---------------------------------------------------------------- messages between processes */

static INT st_msend( MSTH *t, MSN *s, MSSCOPE *sc )
{
	T_TSMSG	m;
	INT	pid, type, size, i, eb, start;
	MSVAR	*src;

	if ( s->nv < 4 ) { rt.lasterr = 48; return FL_NONE; }
	pid = ms_int32(num_arg(t, s->v[0], sc));
	type = ms_int32(num_arg(t, s->v[1], sc));
	size = ms_int32(num_arg(t, s->v[2], sc));
	if ( type < 0 || type > 7 || size < 0 || size > TS_MSG_MAX ) { rt.lasterr = 48; return FL_NONE; }
	if ( size == 0 ) { rt.lasterr = 0; return FL_NONE; }
	src = place_of(t, s->v[3], sc, &start);
	if ( src == NULL ) { rt.lasterr = 48; return FL_NONE; }
	memset(&m, 0, sizeof(m));
	m.type = (UINT)type;
	m.size = size;
	eb = ( src->type == 'B' ) ? 1 : ( src->type == 'C' ) ? 2 : 4;
	for ( i = 0; i * eb < size; i++ ) {
		MSV	e = var_get(src, start + i);
		INT	v = ms_int32(mv_n(e)), k;

		mv_drop(e);
		for ( k = 0; k < eb && i * eb + k < size; k++ ) m.body[i * eb + k] = (UB)( v >> ( 8 * k ) );
	}
	rt.lasterr = ( ts_snd_msg(( pid == 0 ) ? ts_get_pid() : pid, &m, TMO_POL) >= E_OK ) ? 0 : 48;
	return FL_NONE;
}

static INT st_mrecv( MSTH *t, MSN *s, MSSCOPE *sc )
{
	T_TSMSG	m;
	INT	mask, i, eb, ps, ts, ss, bs;
	MSVAR	*pv, *tv, *sv, *bv;
	double	tmo = -1;
	UD	start = ms_now_ms();

	if ( s->nv < 5 ) { rt.lasterr = 48; return FL_NONE; }
	mask = ms_int32(num_arg(t, s->v[0], sc));
	pv = place_of(t, s->v[1], sc, &ps);
	tv = place_of(t, s->v[2], sc, &ts);
	sv = place_of(t, s->v[3], sc, &ss);
	bv = place_of(t, s->v[4], sc, &bs);
	if ( s->a != NULL ) tmo = num_arg(t, s->a, sc);
	else if ( s->nv >= 6 ) tmo = num_arg(t, s->v[5], sc);
	t->waiting = TRUE;
	while ( !th_stop(t) ) {
		if ( ts_rcv_msg(&m, TMO_POL) >= E_OK ) {
			if ( ( m.type & 0xFFFF0000U ) != 0 || !( mask & rt_mmask() & ( 1 << ( m.type & 7 ) ) ) ) {
				continue;		/* not for the script */
			}
			if ( pv != NULL ) var_set(pv, ps, mv_num(( m.from == ts_get_pid() ) ? 0 : m.from));
			if ( tv != NULL ) var_set(tv, ts, mv_num(m.type));
			if ( sv != NULL ) var_set(sv, ss, mv_num((double)m.size));
			if ( bv != NULL ) {
				eb = ( bv->type == 'B' ) ? 1 : ( bv->type == 'C' ) ? 2 : 4;
				for ( i = 0; i * eb < m.size; i++ ) {
					INT	v = 0, k;

					for ( k = 0; k < eb && i * eb + k < m.size; k++ ) v |= m.body[i * eb + k] << ( 8 * k );
					var_set(bv, bs + i, mv_num(v));
				}
			}
			rt.lasterr = 0;
			break;
		}
		if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
		if ( tmo == 0 ) { rt.lasterr = 1; break; }
		if ( tmo > 0 && (double)( ms_now_ms() - start ) >= tmo * 1000 ) { rt.lasterr = 1; break; }
		ms_yield(ms_now_ms() + 50);
	}
	t->waiting = FALSE;
	return FL_NONE;
}

/* ---------------------------------------------------------------- serial lines */

/*
 * RS port n is the n-th serial port that is not the console's. What
 * comes in is gathered in a buffer of the program's, which RSGETN and
 * RSGETC copy into $RS and RSWAIT looks through.
 */
#define RS_PORTS	4
#define RS_BUF		4096

typedef struct {
	TS_UUID	dev;
	ID	key;
	UB	buf[RS_BUF];
	INT	n;
} RSPORT;

static RSPORT	rsport[RS_PORTS];
static INT	nrsport = -1;
static UB	rs_data[RS_BUF];
static INT	rs_cnt;

static void rs_list( void )
{
	TS_UUID	ids[32];
	T_OBREF	r;
	T_SERINFO info;
	INT	cnt = 0, i;
	SZ	asz;
	ID	key;

	nrsport = 0;
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_CHAR, NULL, ids, 32, &cnt) < E_OK ) return;
	for ( i = 0; i < cnt && i < 32 && nrsport < RS_PORTS; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) < E_OK || r.name[0] != 's' || r.name[1] != 'e' || r.name[2] != 'r' ) continue;
		key = ob_opn_obj(&ids[i], OB_OP_READ);
		if ( key <= 0 ) continue;
		asz = 0;
		if ( ob_rea_rec(key, 3, 0, &info, sizeof(info), &asz) >= E_OK && asz == (SZ)sizeof(info)
		  && !info.console ) {
			memset(&rsport[nrsport], 0, sizeof(RSPORT));
			rsport[nrsport].dev = ids[i];
			nrsport++;
		}
		ob_cls_obj(key);
	}
}

static RSPORT *rs_open( INT port )
{
	RSPORT	*p;

	if ( nrsport < 0 ) rs_list();
	if ( port < 0 || port >= nrsport ) return NULL;
	p = &rsport[port];
	if ( p->key <= 0 ) {
		p->key = ob_opn_obj(&p->dev, OB_OP_READ | OB_OP_WRITE);
		if ( p->key <= 0 ) {
			p->key = 0;
			return NULL;
		}
	}
	return p;
}

/* What has come in, into the buffer; the oldest goes when it is full */
static void rs_poll( RSPORT *p )
{
	UB	tmp[512];
	SZ	asz;
	INT	guard;

	for ( guard = 0; guard < 16; guard++ ) {
		asz = 0;
		if ( ob_rea_rec(p->key, 1, 0, tmp, sizeof(tmp), &asz) < E_OK || asz <= 0 ) break;
		if ( p->n + (INT)asz > RS_BUF ) {
			INT	drop = p->n + (INT)asz - RS_BUF;

			memmove(p->buf, p->buf + drop, (size_t)( p->n - drop ));
			p->n -= drop;
		}
		memcpy(p->buf + p->n, tmp, (size_t)asz);
		p->n += (INT)asz;
	}
}

/* The bytes the arguments from `from` on give: a number its low byte, a string and an array each element's */
static INT rs_bytes( MSTH *t, MSN *s, INT from, MSSCOPE *sc, UB *out, INT max )
{
	INT	i, n = 0, k;

	for ( i = from; i < s->nv && n < max; i++ ) {
		MSV	v = ev(t, s->v[i], sc);

		if ( v.t == V_ARR ) {
			for ( k = 0; k < v.u.a->n && n < max; k++ ) out[n++] = (UB)ms_int32(mv_n(v.u.a->e[k]));
		} else if ( v.t == V_STR ) {
			for ( k = 0; k < v.u.s->len && n < max; k++ ) out[n++] = (UB)v.u.s->s[k];
		} else {
			out[n++] = (UB)mv_i(v);
		}
		mv_drop(v);
	}
	return n;
}

static INT st_rs( MSTH *t, MSN *s, MSSCOPE *sc )
{
	INT	port = ( s->nv > 0 ) ? ms_int32(num_arg(t, s->v[0], sc)) : 0;
	RSPORT	*p = rs_open(port);
	UB	out[1024];
	INT	n;
	SZ	asz;

	if ( p == NULL ) {
		if ( s->k == S_RSGETN || s->k == S_RSGETC ) rs_cnt = 0;
		rt.lasterr = 16;
		return FL_NONE;
	}
	switch ( s->k ) {
	case S_RSINIT: {
		double	baud = ( s->nv > 2 ) ? num_arg(t, s->v[2], sc) : 0;

		if ( baud > 0 ) {
			UW	v = (UW)baud;

			(void)ob_wri_rec(p->key, 2, 0, &v, sizeof(v), &asz);
		}
		rs_poll(p);
		p->n = 0;
		rt.lasterr = 0;
		break;
	}
	case S_RSPUT:
	case S_RSPUTN:
		rs_poll(p);
		if ( s->k == S_RSPUT ) p->n = 0;	/* what came before goes */
		n = rs_bytes(t, s, 1, sc, out, sizeof(out));
		rt.lasterr = ( n == 0 || ob_wri_rec(p->key, 1, 0, out, n, &asz) >= E_OK ) ? 0 : 16;
		break;
	case S_RSWAIT: {
		double	tmo = ( s->a != NULL ) ? num_arg(t, s->a, sc) : -1;
		UD	start = ms_now_ms();

		n = rs_bytes(t, s, 1, sc, out, sizeof(out));
		if ( n == 0 ) { rt.lasterr = 0; break; }
		t->waiting = TRUE;
		while ( !th_stop(t) ) {
			INT	i;
			BOOL	found = FALSE;

			rs_poll(p);
			for ( i = 0; i + n <= p->n && !found; i++ ) found = (BOOL)( memcmp(p->buf + i, out, (size_t)n) == 0 );
			if ( found ) { rt.lasterr = 0; break; }
			if ( t->brk ) { t->brk = FALSE; rt.lasterr = 2; break; }
			if ( tmo == 0 ) { rt.lasterr = 1; break; }
			if ( tmo > 0 && (double)( ms_now_ms() - start ) >= tmo * 1000 ) { rt.lasterr = 1; break; }
			ms_yield(ms_now_ms() + 20);
		}
		t->waiting = FALSE;
		break;
	}
	case S_RSGETN:
	case S_RSGETC:
		rs_poll(p);
		memcpy(rs_data, p->buf, (size_t)p->n);
		rs_cnt = p->n;
		if ( s->k == S_RSGETC ) p->n = 0;
		rt.lasterr = 0;
		break;
	case S_RSCNTL:
		/* the line's state: nothing of it is known here */
		if ( s->nv > 2 && ms_int32(num_arg(t, s->v[1], sc)) == 0 ) {
			INT	st;
			MSVAR	*v = place_of(t, s->v[2], sc, &st);

			if ( v != NULL ) var_set(v, st, mv_num(0));
		}
		rt.lasterr = 0;
		break;
	}
	return FL_NONE;
}

/* ---------------------------------------------------------------- the other statements */

static INT mmask_v;

INT rt_mmask( void )
{
	return mmask_v;
}

INT exec_other( MSTH *t, MSN *s, MSSCOPE *sc )
{
	switch ( s->k ) {
	case S_SCENE:		return st_scene(t, s, sc);
	case S_APPEAR:		return st_appear(t, s, sc, TRUE);
	case S_DISAPPEAR:	return st_appear(t, s, sc, FALSE);
	case S_MOVE:		return st_move(t, s, sc);
	case S_TEXT:		return st_text(t, s, sc);
	case S_MESG: {
		MSBUF	b;

		if ( s->nv == 0 ) {
			ms_message_clear();
			return FL_NONE;
		}
		mb_init(&b);
		fmt_args(t, s->v, s->nv, sc, &b);
		mb_putc(&b, '\n');
		ms_message(b.s);
		mb_free(&b);
		return FL_NONE;
	}
	case S_LOG: {
		MSBUF	b;

		if ( s->nv == 0 ) return FL_NONE;
		mb_init(&b);
		mb_puts(&b, "[MS-LOG] ");
		fmt_args(t, s->v, s->nv, sc, &b);
		mb_putc(&b, '\n');
		tm_putstring((const UB *)b.s);
		mb_free(&b);
		return FL_NONE;
	}
	case S_BEEP: {
		double	f = ( s->a != NULL ) ? num_arg(t, s->a, sc) : 1000;
		double	d = ( s->b != NULL ) ? num_arg(t, s->b, sc) : 200;

		ms_beep(f, d);
		return FL_NONE;
	}
	case S_WSIZE: {
		INT	w = ms_int32(num_arg(t, s->a, sc)), h = ms_int32(num_arg(t, s->b, sc));

		/* 0 0 and 0 1: a window without a title (a wide one), from the next start */
		if ( w == 0 && ( h == 0 || h == 1 ) ) ms_win_mode(h == 0 ? 1 : 2);
		else ms_win_resize(w, h);
		return FL_NONE;
	}
	case S_WMOVE:
		ms_win_move(ms_int32(num_arg(t, s->a, sc)), ms_int32(num_arg(t, s->b, sc)));
		return FL_NONE;
	case S_WSAVE:
		ms_win_save();
		return FL_NONE;
	case S_FULLWIND:
		ms_win_mode(3);
		return FL_NONE;
	case S_UPDATE:
		ms_win.update = (BOOL)( num_arg(t, s->a, sc) != 0 );
		if ( ms_win.update ) ms_dirty();
		return FL_NONE;
	case S_INPUT:		return st_input(t, s, sc, FALSE);
	case S_KINPUT:		return st_input(t, s, sc, TRUE);
	case S_VOPEN:		return st_vopen(t, s, sc);
	case S_VCLOSE:		return st_vclose(t, s, sc);
	case S_VWAIT:		return st_vwait(t, s, sc);
	case S_SETSEG:		return st_setseg(t, s, sc);
	case S_COPYSEG:		return st_copyseg(t, s, sc);
	case S_EVENT: {
		double	ch = ( s->nv > 0 ) ? num_arg(t, s->v[0], sc) : 0;
		double	st = ( s->nv > 1 ) ? num_arg(t, s->v[1], sc) : 0;
		const char *k = s->name2->s;

		if ( strcmp(k, "KEYC") == 0 || strcmp(k, "KEYD") == 0 || strcmp(k, "KEYU") == 0 ) {
			rt_key((UW)ms_int32(ch), (UW)ms_int32(st));
		} else if ( strcmp(k, "BUTC") == 0 || strcmp(k, "BUTD") == 0 || strcmp(k, "BUTU") == 0 ) {
			INT	x = ( s->a != NULL ) ? ms_int32(num_arg(t, s->a, sc)) : ms_win.pdx;
			INT	y = ( s->b != NULL ) ? ms_int32(num_arg(t, s->b, sc)) : ms_win.pdy;
			MSSEG	*g = st_hit(x, y, NULL);

			if ( g != NULL ) {
				if ( strcmp(k, "BUTU") != 0 ) rt_event(EV_PRESS, g, x - (INT)g->x, y - (INT)g->y);
				if ( strcmp(k, "BUTC") == 0 ) rt_event(EV_CLICK, g, x - (INT)g->x, y - (INT)g->y);
			}
		}
		return FL_NONE;
	}
	case S_FOPEN:		return st_fopen(t, s, sc);
	case S_FCLOSE:		return st_fclose(t, s, sc);
	case S_FREAD:		return st_fread(t, s, sc);
	case S_FWRITE:		return st_fwrite(t, s, sc);
	case S_MSEND:		return st_msend(t, s, sc);
	case S_MRECV:		return st_mrecv(t, s, sc);
	case S_PROCESS:		return st_process(t, s, sc);
	case S_PWAIT:		return st_pwait(t, s, sc);
	case S_HWNOOP:		return st_dev(t, s, sc);
	case S_RSINIT: case S_RSPUT: case S_RSPUTN: case S_RSWAIT:
	case S_RSGETN: case S_RSGETC: case S_RSCNTL:
		return st_rs(t, s, sc);
	case S_CONSOLE: case S_SPRINTF: case S_HTTPREQ: case S_HTTPHDR: case S_JSONGET:
	case S_JSONLEN: case S_TCPOPEN: case S_TCPSEND: case S_TCPWAIT: case S_TCPRECV:
	case S_TCPCLOSE:
		return ms_net_exec(t, s, sc);
	}
	return FL_NONE;
}

MSVAR *rt_place( MSTH *t, MSN *e, MSSCOPE *sc, INT *p_start )
{
	return place_of(t, e, sc, p_start);
}

double rt_num( MSTH *t, MSN *e, MSSCOPE *sc )
{
	return num_arg(t, e, sc);
}

void rt_format( MSTH *t, MSN **v, INT n, MSSCOPE *sc, MSBUF *out )
{
	fmt_args(t, v, n, sc, out);
}

/* ---------------------------------------------------------------- system variables */

static double	sv_saved[50];
static BOOL	sv_loaded;
static INT	kmode_v, dskins_v = 1;
static UW	rand_state;

void rt_preset_sv( INT i, INT v )
{
	if ( i >= 0 && i < 50 && !sv_loaded ) sv_saved[i] = v;
}

void ms_sv_store( const double *sv );
void ms_sv_load( double *sv, BOOL *p_had );

static void sv_load( void )
{
	BOOL	had = FALSE;
	double	tmp[50];
	INT	i;

	if ( sv_loaded ) return;
	sv_loaded = TRUE;
	for ( i = 0; i < 50; i++ ) tmp[i] = sv_saved[i];
	ms_sv_load(tmp, &had);
	if ( had ) for ( i = 0; i < 50; i++ ) sv_saved[i] = tmp[i];
}

static UW rnd( void )
{
	if ( rand_state == 0 ) {
		UD	ns = 0;
		IMPORT ER ts_get_mono( UD *p_ns );

		(void)ts_get_mono(&ns);
		rand_state = (UW)( ns ^ ( ns >> 32 ) ) | 1;
	}
	rand_state ^= rand_state << 13;
	rand_state ^= rand_state >> 17;
	rand_state ^= rand_state << 5;
	return rand_state;
}

static BOOL now_tm( TS_TM *tm, TS_TIME *p_t )
{
	TS_TIME	t = 0;

	if ( dt_gettime(&t) < E_OK ) return FALSE;
	if ( p_t != NULL ) *p_t = t;
	return (BOOL)( dt_localtime(&t, tm) >= E_OK );
}

MSV sysvar_get( MSTH *t, MSSTR *name, MSV index, MSSCOPE *sc )
{
	const char	*u = ms_upper(name)->s;
	INT		i = mv_i(index);
	TS_TM		tm;
	TS_TIME		tt;

	if ( strcmp(u, "ERR") == 0 ) return mv_num(rt.lasterr);
	if ( strcmp(u, "PID") == 0 ) return mv_num(ts_get_pid());
	if ( strcmp(u, "PPID") == 0 ) return mv_num(1);
	if ( strcmp(u, "WID") == 0 ) return mv_num(ms_win.wid);
	if ( strcmp(u, "TID") == 0 ) return mv_num(( t != NULL ) ? t->id : 0);
	if ( strcmp(u, "PDX") == 0 ) return mv_num(ms_win.pdx);
	if ( strcmp(u, "PDY") == 0 ) return mv_num(ms_win.pdy);
	if ( strcmp(u, "KSTAT") == 0 ) { UW v = ms_win.kstat; ms_win.kstat = 0; return mv_num(v); }
	if ( strcmp(u, "PDB") == 0 ) return mv_num(ms_win.pdb);
	if ( strcmp(u, "KEY") == 0 ) { UW v = ms_win.lastkey; ms_win.lastkey = 0; return mv_num(v); }
	if ( strcmp(u, "METAKEY") == 0 ) return mv_num(ms_win.metakey);
	if ( strcmp(u, "ARG") == 0 ) {
		MSSCOPE	*s;

		for ( s = sc; s != NULL; s = s->parent ) {
			if ( s->hasargs ) {
				return ( i >= 0 && i < s->nargs ) ? mv_ref(s->args[i]) : mv_num(0);
			}
		}
		return mv_num(0);
	}
	if ( strcmp(u, "DATE") == 0 ) {
		if ( !now_tm(&tm, NULL) ) return mv_num(0);
		return mv_num(tm.tm_year * 10000.0 + ( tm.tm_mon + 1 ) * 100 + tm.tm_mday);
	}
	if ( strcmp(u, "TIME") == 0 ) {
		if ( !now_tm(&tm, NULL) ) return mv_num(0);
		return mv_num(tm.tm_hour * 10000.0 + tm.tm_min * 100 + tm.tm_sec);
	}
	if ( strcmp(u, "MSEC") == 0 ) return mv_num((double)( ms_now_ms() - ms_win.start_ms ));
	if ( strcmp(u, "SYSTM") == 0 ) return mv_num(( dt_gettime(&tt) >= E_OK ) ? (double)tt : 0);
	if ( strcmp(u, "WDW") == 0 ) return mv_num(ms_win.wdw);
	if ( strcmp(u, "WDH") == 0 ) return mv_num(ms_win.wdh);
	if ( strcmp(u, "WDX") == 0 ) return mv_num(ms_win.wdx);
	if ( strcmp(u, "WDY") == 0 ) return mv_num(ms_win.wdy);
	if ( strcmp(u, "WACT") == 0 ) return mv_num(1);
	if ( strcmp(u, "SCRW") == 0 ) return mv_num(ms_win.scrw);
	if ( strcmp(u, "SCRH") == 0 ) return mv_num(ms_win.scrh);
	if ( strcmp(u, "RAND") == 0 ) return mv_num(rnd() & 0xFFFF);
	if ( strcmp(u, "VERS") == 0 ) return mv_num(0x2106);
	if ( strcmp(u, "CPULOAD") == 0 ) return mv_num(0);
	if ( strcmp(u, "CNT") == 0 ) return mv_num(( t != NULL ) ? t->cnt : 0);
	if ( strcmp(u, "GV") == 0 ) {
		if ( i < 0 || i >= 50 ) return mv_num(MS_INVALID);
		return mv_num(ms_gv_get(i));
	}
	if ( strcmp(u, "SV") == 0 ) {
		if ( i < 0 || i >= 50 ) return mv_num(MS_INVALID);
		sv_load();
		return mv_num(sv_saved[i]);
	}
	if ( strcmp(u, "PDS") == 0 ) return mv_num(ms_win.pds);
	if ( strcmp(u, "KMODE") == 0 ) return mv_num(kmode_v);
	if ( strcmp(u, "DSKINS") == 0 ) return mv_num(dskins_v);
	if ( strcmp(u, "PWID") == 0 ) return mv_num(0);
	if ( strcmp(u, "MMASK") == 0 ) return mv_num(mmask_v);
	if ( strcmp(u, "RSCNT") == 0 ) return mv_num(rs_cnt);
	if ( strcmp(u, "RS") == 0 ) return mv_num(( i >= 0 && i < rs_cnt ) ? rs_data[i] : 0);
	{
		MSV	o;

		if ( ms_net_sysvar(u, i, &o) ) return o;
	}
	return mv_num(0);
}

void sysvar_set( MSTH *t, MSSTR *name, MSV index, MSV val, MSSCOPE *sc )
{
	const char	*u = ms_upper(name)->s;
	INT		i = mv_i(index);

	if ( strcmp(u, "CNT") == 0 ) { if ( t != NULL ) t->cnt = mv_i(val); return; }
	if ( strcmp(u, "GV") == 0 ) { ms_gv_set(i, mv_i(val)); return; }
	if ( strcmp(u, "SV") == 0 ) {
		if ( i < 0 || i >= 50 ) return;
		sv_load();
		sv_saved[i] = mv_i(val);
		ms_sv_store(sv_saved);
		return;
	}
	if ( strcmp(u, "ARG") == 0 ) {
		MSSCOPE	*s;

		for ( s = sc; s != NULL && !s->hasargs; s = s->parent ) ;
		if ( s != NULL && i >= 0 ) {
			if ( i >= s->nargs ) {
				INT	k;

				s->args = ms_realloc(s->args, sizeof(MSV) * (size_t)( i + 1 ));
				for ( k = s->nargs; k <= i; k++ ) s->args[k] = mv_num(0);
				s->nargs = i + 1;
			}
			mv_drop(s->args[i]);
			s->args[i] = mv_ref(val);
		}
		return;
	}
	if ( strcmp(u, "PDS") == 0 ) { ms_win.pds = mv_i(val); return; }
	if ( strcmp(u, "KMODE") == 0 ) { kmode_v = mv_i(val); return; }
	if ( strcmp(u, "MMASK") == 0 ) { mmask_v = mv_i(val); return; }
	if ( strcmp(u, "DSKINS") == 0 ) { dskins_v = mv_i(val); return; }
}

/* ---------------------------------------------------------------- Shift_JIS and EUC */

extern const UH	ms_jis_char[94 * 94];
extern const UW	ms_jis_back[];
extern const INT ms_jis_nback;

static double el_n( MSVAR *v, INT i );

/* The row and cell (row << 8 | cell) of a character, or -1 */
static INT jis_of( UW u )
{
	INT	lo = 0, hi = ms_jis_nback - 1;

	while ( lo <= hi ) {
		INT	mid = ( lo + hi ) / 2;
		UW	m = ms_jis_back[mid] >> 16;

		if ( m == u ) return (INT)( ms_jis_back[mid] & 0xFFFF );
		if ( m < u ) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

static UW jis_char( INT row, INT cell )
{
	if ( row < 1 || row > 94 || cell < 1 || cell > 94 ) return 0;
	return ms_jis_char[( row - 1 ) * 94 + cell - 1];
}

/* The full width katakana of the half width ones U+FF61..FF9F */
static const UH	kana_full[63] = {
	0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
	0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB,
	0x30AD, 0x30AF, 0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1,
	0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5,
	0x30D8, 0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9,
	0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C
};

/*
 * sconv: bytes in Shift_JIS ('s') or EUC ('e') to characters. A byte
 * that is not a character of the code, and 0x0D, are passed over; a 0
 * ends it. The number of characters stored.
 */
static INT sconv( MSVAR *dst, INT s1, MSVAR *src, INT s2, INT len, BOOL euc )
{
	INT	k, j = 0;

	if ( len <= 0 ) len = src->size - s2;
	for ( k = 0; k < len && s2 + k < src->size && s1 + j < dst->size; ) {
		INT	b = ms_int32(el_n(src, s2 + k)) & 0xFF, b2;
		UW	u = 0;

		if ( b == 0 ) break;
		k++;
		if ( b == 0x0D ) continue;
		if ( b < 0x80 ) {
			u = (UW)b;
		} else if ( euc ) {
			if ( k >= len || s2 + k >= src->size ) break;
			b2 = ms_int32(el_n(src, s2 + k)) & 0xFF;
			if ( b == 0x8E ) {
				k++;
				if ( b2 >= 0xA1 && b2 <= 0xDF ) u = 0xFF61 + (UW)( b2 - 0xA1 );
			} else if ( b == 0x8F ) {
				k += 2;			/* JIS X 0212: not kept */
			} else if ( b >= 0xA1 && b <= 0xFE && b2 >= 0xA1 && b2 <= 0xFE ) {
				k++;
				u = jis_char(b - 0xA0, b2 - 0xA0);
			}
		} else if ( b >= 0xA1 && b <= 0xDF ) {
			u = 0xFF61 + (UW)( b - 0xA1 );
		} else if ( ( b >= 0x81 && b <= 0x9F ) || ( b >= 0xE0 && b <= 0xFC ) ) {
			INT	row, cell;

			if ( k >= len || s2 + k >= src->size ) break;
			b2 = ms_int32(el_n(src, s2 + k)) & 0xFF;
			if ( b2 < 0x40 || b2 == 0x7F || b2 > 0xFC ) continue;
			k++;
			row = ( ( ( b >= 0xE0 ) ? b - 0x40 : b ) - 0x81 ) * 2 + 1;
			if ( b2 >= 0x9F ) {
				row++;
				cell = b2 - 0x9E;
			} else {
				cell = b2 - 0x3F - ( ( b2 >= 0x80 ) ? 1 : 0 );
			}
			u = jis_char(row, cell);
		}
		if ( u != 0 ) var_set(dst, s1 + j++, mv_num(u));
	}
	if ( s1 + j < dst->size ) var_set(dst, s1 + j, mv_num(0));
	return j;
}

/*
 * srconv: characters to Shift_JIS ('S' all two bytes, 's' ASCII in one,
 * 'k' ASCII and half width katakana in one) or EUC ('E', 'e'). A new
 * paragraph is CR LF in Shift_JIS. A character the code does not have
 * is left out. The number of bytes stored.
 */
static INT srconv( MSVAR *dst, INT s1, MSVAR *src, INT s2, INT len, INT mode )
{
	BOOL	euc = (BOOL)( mode == 'E' || mode == 'e' );
	BOOL	wide = (BOOL)( mode == 'S' || mode == 'E' );
	INT	k, j = 0, n = 0;
	UB	out[4];

	if ( len <= 0 ) len = src->size - s2;
	for ( k = 0; k < len && s2 + k < src->size; k++ ) {
		UW	u = (UW)ms_int32(el_n(src, s2 + k));
		INT	i, rc;

		if ( u == 0 ) break;
		n = 0;
		if ( u == 0x0A ) {
			if ( !euc ) out[n++] = 0x0D;
			out[n++] = 0x0A;
		} else if ( u < 0x80 && !wide ) {
			out[n++] = (UB)u;
		} else if ( u >= 0xFF61 && u <= 0xFF9F && mode == 'k' ) {
			out[n++] = (UB)( 0xA1 + ( u - 0xFF61 ) );
		} else if ( u >= 0xFF61 && u <= 0xFF9F && mode == 'e' ) {
			out[n++] = 0x8E;
			out[n++] = (UB)( 0xA1 + ( u - 0xFF61 ) );
		} else {
			if ( u == 0x20 ) u = 0x3000;
			else if ( u > 0x20 && u < 0x7F ) u = 0xFF01 + ( u - 0x21 );
			else if ( u >= 0xFF61 && u <= 0xFF9F ) u = kana_full[u - 0xFF61];
			rc = jis_of(u);
			if ( rc < 0 ) continue;
			if ( euc ) {
				out[n++] = (UB)( 0xA0 + ( rc >> 8 ) );
				out[n++] = (UB)( 0xA0 + ( rc & 0xFF ) );
			} else {
				INT	row = rc >> 8, cell = rc & 0xFF;
				INT	b1 = ( ( row - 1 ) >> 1 ) + 0x81;

				if ( b1 > 0x9F ) b1 += 0x40;
				out[n++] = (UB)b1;
				out[n++] = (UB)( ( row & 1 ) ? cell + 0x3F + ( ( cell >= 64 ) ? 1 : 0 ) : cell + 0x9E );
			}
		}
		if ( s1 + j + n > dst->size ) break;
		for ( i = 0; i < n; i++ ) var_set(dst, s1 + j++, mv_num(out[i]));
	}
	return j;
}

/* ---------------------------------------------------------------- functions */

static MSVAR *var_arg( MSN *n, MSSCOPE *sc )
{
	if ( n == NULL || n->k != N_NAME ) return NULL;
	return scope_find(sc, n->name);
}

static double arg_n( MSV *a, INT n, INT i )
{
	return ( i < n ) ? mv_n(a[i]) : NAN;
}

static INT arg_i( MSV *a, INT n, INT i )
{
	return ( i < n ) ? mv_i(a[i]) : 0;
}

static INT eff_len( MSVAR *v1, INT s1, MSVAR *v2, INT s2, INT len )
{
	INT	remain = v1->size - s1;

	if ( v2->size - s2 < remain ) remain = v2->size - s2;
	if ( len > 0 && len < remain ) remain = len;
	return ( remain > 0 ) ? remain : 0;
}

static double el_n( MSVAR *v, INT i )
{
	MSV	e = var_get(v, i);
	double	d = mv_n(e);

	mv_drop(e);
	return isnan(d) ? 0 : d;
}

static MSV fn_tmdate( MSTH *t, MSN *n, MSSCOPE *sc, MSV *a, INT na )
{
	INT	st;
	MSVAR	*v = place_of(t, n->v[0], sc, &st);
	TS_TIME	tt;
	TS_TM	tm;
	INT	systm = arg_i(a, na, 1);

	if ( v == NULL || v->size < 9 ) return mv_num(MS_INVALID);
	if ( systm == 0 ) {
		if ( !now_tm(&tm, NULL) ) return mv_num(MS_INVALID);
	} else {
		tt = systm;
		if ( dt_localtime(&tt, &tm) < E_OK ) return mv_num(MS_INVALID);
	}
	var_set(v, st + 0, mv_num(tm.tm_year + 1900));
	var_set(v, st + 1, mv_num(tm.tm_mon + 1));
	var_set(v, st + 2, mv_num(tm.tm_mday));
	var_set(v, st + 3, mv_num(tm.tm_hour));
	var_set(v, st + 4, mv_num(tm.tm_min));
	var_set(v, st + 5, mv_num(tm.tm_sec));
	var_set(v, st + 6, mv_num(tm.tm_yday / 7 + 1));
	var_set(v, st + 7, mv_num(tm.tm_wday));
	var_set(v, st + 8, mv_num(tm.tm_yday + 1));
	return mv_num(0);
}

static MSV fn_datetm( MSTH *t, MSN *n, MSSCOPE *sc )
{
	INT	st;
	MSVAR	*v = place_of(t, n->v[0], sc, &st);
	TS_TM	tm;
	TS_TIME	tt;
	INT	tz = 0;

	if ( v == NULL || v->size < 6 ) return mv_num(MS_INVALID);
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = ms_int32(el_n(v, st + 0)) - 1900;
	tm.tm_mon = ms_int32(el_n(v, st + 1)) - 1;
	tm.tm_mday = ms_int32(el_n(v, st + 2));
	tm.tm_hour = ms_int32(el_n(v, st + 3));
	tm.tm_min = ms_int32(el_n(v, st + 4));
	tm.tm_sec = ms_int32(el_n(v, st + 5));
	if ( dt_mktime(&tm, &tt) < E_OK ) return mv_num(MS_INVALID);
	(void)dt_getsystz(&tz);
	return mv_num((double)tt - tz * 60.0);
}

static MSV fn_strnum( MSTH *t, MSN *n, MSSCOPE *sc, MSV *a, INT na )
{
	MSVAR	*v = var_arg(n->v[0], sc);
	INT	start = arg_i(a, na, 1), i, len = 0;
	double	fc = arg_n(a, na, 2), result = 0;
	char	s[160], *end;
	INT	endpos;

	if ( v == NULL ) return mv_num(0);
	for ( i = start; i < v->size && i < start + 128; i++ ) {
		double	c = el_n(v, i);
		UW	cp;

		if ( c == 0 ) break;
		cp = ms_norm_cp((UW)ms_int32(c));
		s[len++] = ( cp < 0x80 ) ? (char)cp : '?';
	}
	s[len] = 0;
	endpos = start + len;
	if ( len >= 2 && s[0] == '0' && ( s[1] == 'x' || s[1] == 'X' ) ) {
		result = (double)strtoul(s + 2, &end, 16);
		if ( end > s + 2 ) endpos = start + (INT)( end - s );
	} else if ( fc == 'd' ) {
		result = (double)strtol(s, &end, 10);
		if ( end > s ) endpos = start + (INT)( end - s );
	} else if ( fc == 'x' || fc == 'X' ) {
		result = (double)strtoul(s, &end, 16);
		if ( end > s ) endpos = start + (INT)( end - s );
	} else {
		result = strtod(s, &end);
		if ( end > s ) endpos = start + (INT)( end - s );
	}
	if ( n->nv > 3 ) {
		MSVAR	*ev_ = var_arg(n->v[3], sc);

		if ( ev_ != NULL ) var_set(ev_, 0, mv_num(endpos));
	}
	return mv_num(result);
}

/*
 * filelist(path, var, from): the names of the objects the virtual
 * objects of an object point to, from the `from`-th on, each ended with
 * 0 in the array one after another; how many went in.
 */
/* ---------------------------------------------------------------- pictures */

/*
 * The segments newimgseg made, each without a name of its own. One that
 * no segment shows any longer (SETSEG gave another the look) goes, and
 * its pixels with it, the next time a picture is made.
 */
#define IMGSEG_MAX	256

static MSSEG	*imgsegs[IMGSEG_MAX];
static INT	nimgsegs, imgseq;

static BOOL img_used( const MSSEG *g )
{
	MSSEG	*s;

	for ( s = st_first(); s != NULL; s = s->next ) {
		if ( s != g && ( s->shared == g || s->fs == g->fs ) ) return TRUE;
	}
	return FALSE;
}

static void img_sweep( void )
{
	INT	i, k = 0;

	for ( i = 0; i < nimgsegs; i++ ) {
		MSSEG	*g = imgsegs[i];

		if ( img_used(g) ) {
			imgsegs[k++] = g;
			continue;
		}
		ms_free(g->fs->imgbuf);
		ms_free(g->fs);
		g->fs = NULL;
		st_remove(g);
	}
	nimgsegs = k;
}

/* The first record of an object that is a picture, read whole */
static UB *picture_of( const TS_UUID *u, SZ *p_len )
{
	T_OBREC	r[16];
	INT	cnt = 0, i;
	ID	k = ob_opn_obj(u, OB_OP_R);
	UB	*b = NULL;

	*p_len = 0;
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && b == NULL; i++ ) {
			SZ	asz = 0;
			INT	w, h;

			if ( r[i].size < 8 ) continue;
			b = ms_alloc((size_t)r[i].size);
			if ( ob_rea_rec(k, i, 0, b, (SZ)r[i].size, &asz) < E_OK
			  || dp_img_decode(b, asz, NULL, 0, &w, &h) < E_OK ) {
				ms_free(b);
				b = NULL;
				continue;
			}
			*p_len = asz;
		}
	}
	ob_cls_obj(k);
	return b;
}

/* newimgseg(path, x, y[, Xi, Yi, Wi, Hi]): a segment of a JPEG, PNG or BMP picture */
static MSV fn_newimgseg( MSV *a, INT na )
{
	MSSTR	*path = ( na > 0 ) ? mv_name(a[0]) : NULL;
	TS_UUID	u;
	UB	*file;
	SZ	len = 0;
	INT	w = 0, h = 0, xi = 0, yi = 0, wi, hi;
	UW	*px;
	MSFSEG	*fs;
	MSSEG	*g;
	char	nm[32];

	if ( path == NULL || na < 3 || !ms_path_resolve(path->s, &u) ) return mv_num(MS_INVALID);
	file = picture_of(&u, &len);
	if ( file == NULL ) return mv_num(MS_INVALID);
	if ( dp_img_decode(file, len, NULL, 0, &w, &h) < E_OK || w <= 0 || h <= 0 ) {
		ms_free(file);
		return mv_num(MS_INVALID);
	}
	px = ms_alloc(sizeof(UW) * (size_t)w * (size_t)h);
	if ( dp_img_decode(file, len, px, (SZ)w * h, &w, &h) < E_OK ) {
		ms_free(file);
		ms_free(px);
		return mv_num(MS_INVALID);
	}
	ms_free(file);
	wi = w;
	hi = h;
	if ( na >= 7 ) {
		/* only this part of it */
		xi = mv_i(a[3]);
		yi = mv_i(a[4]);
		wi = mv_i(a[5]);
		hi = mv_i(a[6]);
		if ( xi < 0 ) xi = 0;
		if ( yi < 0 ) yi = 0;
		if ( xi + wi > w ) wi = w - xi;
		if ( yi + hi > h ) hi = h - yi;
		if ( wi <= 0 || hi <= 0 ) { ms_free(px); return mv_num(MS_INVALID); }
	}
	img_sweep();
	if ( nimgsegs >= IMGSEG_MAX ) { ms_free(px); return mv_num(MS_INVALID); }
	fs = ms_alloc(sizeof(MSFSEG));
	fs->imgbuf = px;
	fs->img = px + (SZ)yi * w + xi;
	fs->imgpitch = w;
	fs->imgw = wi;
	fs->imgh = hi;
	fs->textcol = 0xFFFFFFFFU;
	/* (x, y) as the window has it */
	fs->box.l = mv_n(a[1]) + st_vx();
	fs->box.t = mv_n(a[2]) + st_vy();
	fs->box.r = fs->box.l + wi;
	fs->box.b = fs->box.t + hi;
	fs->box.ok = TRUE;
	fs->gl = fs->box.l;
	fs->gt = fs->box.t;
	snprintf(nm, sizeof(nm), " image%d", ++imgseq);
	fs->name = ms_intern_z(nm);
	g = st_add(fs->name, fs);
	imgsegs[nimgsegs++] = g;
	return mv_str(g->key);
}

static MSV fn_filelist( MSTH *t, MSN *n, MSSCOPE *sc, MSV *a, INT na )
{
	MSSTR	*path = ( na > 0 ) ? mv_name(a[0]) : NULL;
	TS_UUID	u, *ids;
	char	(*nm)[MS_NAME];
	INT	start, from = ( na > 2 ) ? mv_i(a[2]) : 0, cnt, i, got = 0;
	MSVAR	*v = ( n->nv > 1 ) ? place_of(t, n->v[1], sc, &start) : NULL;

	if ( path == NULL || v == NULL || !ms_path_resolve(path->s, &u) ) return mv_num(MS_INVALID);
	ids = ms_alloc(sizeof(TS_UUID) * 256);
	nm = ms_alloc(sizeof(*nm) * 256);
	cnt = ms_links_of(&u, ids, nm, 256);
	if ( cnt < 0 ) {
		ms_free(ids);
		ms_free(nm);
		return mv_num(MS_INVALID);
	}
	for ( i = ( from > 0 ) ? from : 0; i < cnt; i++ ) {
		UW	cps[MS_NAME];
		INT	k = 0, b = 0, len = (INT)strlen(nm[i]);

		while ( b < len && k < MS_NAME - 1 ) {
			INT	d = ms_utf8_dec((const UB *)nm[i] + b, len - b, &cps[k]);

			if ( d <= 0 ) break;
			b += d;
			k++;
		}
		if ( start + k + 1 > v->size ) break;	/* no room for the whole name */
		for ( b = 0; b < k; b++ ) var_set(v, start + b, mv_num(cps[b]));
		var_set(v, start + k, mv_num(0));
		start += k + 1;
		got++;
	}
	ms_free(ids);
	ms_free(nm);
	return mv_num(got);
}

MSV call_builtin( MSTH *t, MSN *n, MSSCOPE *sc )
{
	const char	*u = ms_upper(n->name)->s;
	MSV		*a, out = mv_num(0);
	INT		na = n->nv, i;
	MSPROC		*p;

	a = ms_alloc(sizeof(MSV) * (size_t)( na + 1 ));
	for ( i = 0; i < na; i++ ) a[i] = ev(t, n->v[i], sc);

	if ( strcmp(u, "VALID") == 0 ) out = mv_num(( na > 0 && mv_is_invalid(a[0]) ) ? 0 : 1);
	else if ( strcmp(u, "NUMBER") == 0 ) out = mv_num(( na > 0 && a[0].t == V_NUM ) ? 1 : 0);
	else if ( strcmp(u, "SIN") == 0 ) out = mv_num(sin(arg_n(a, na, 0)));
	else if ( strcmp(u, "COS") == 0 ) out = mv_num(cos(arg_n(a, na, 0)));
	else if ( strcmp(u, "TAN") == 0 ) out = mv_num(tan(arg_n(a, na, 0)));
	else if ( strcmp(u, "ASIN") == 0 ) { double x = arg_n(a, na, 0); out = mv_num(( x < -1 || x > 1 ) ? 0 : asin(x)); }
	else if ( strcmp(u, "ACOS") == 0 ) { double x = arg_n(a, na, 0); out = mv_num(( x < -1 || x > 1 ) ? 0 : acos(x)); }
	else if ( strcmp(u, "ATAN") == 0 ) out = mv_num(atan(arg_n(a, na, 0)));
	else if ( strcmp(u, "SQRT") == 0 ) { double x = arg_n(a, na, 0); out = mv_num(( x < 0 ) ? 0 : sqrt(x)); }
	else if ( strcmp(u, "EXP") == 0 ) out = mv_num(exp(arg_n(a, na, 0)));
	else if ( strcmp(u, "LOG") == 0 || strcmp(u, "LOG10") == 0 ) {
		double	x = arg_n(a, na, 0);

		out = ( x == 0 ) ? mv_num(-1e300) : ( x < 0 ) ? mv_num(MS_INVALID)
		      : mv_num(( u[3] == '1' ) ? log10(x) : log(x));
	}
	else if ( strcmp(u, "FLOOR") == 0 ) out = mv_num(floor(arg_n(a, na, 0)));
	else if ( strcmp(u, "CEIL") == 0 ) out = mv_num(ceil(arg_n(a, na, 0)));
	else if ( strcmp(u, "ROUND") == 0 ) {
		double	x = arg_n(a, na, 0);

		out = mv_num(( x < 0 ? -1 : 1 ) * floor(fabs(x) + 0.5));
	}
	else if ( strcmp(u, "FABS") == 0 ) out = mv_num(fabs(arg_n(a, na, 0)));
	else if ( strcmp(u, "POW") == 0 ) {
		double	x = arg_n(a, na, 0), y = arg_n(a, na, 1);

		if ( x == 0 && y <= 0 ) out = mv_num(0);
		else if ( x < 0 && y != trunc(y) ) out = mv_num(0);
		else out = mv_num(pow(x, y));
	}
	else if ( strcmp(u, "MAX") == 0 ) out = mv_num(fmax(arg_n(a, na, 0), arg_n(a, na, 1)));
	else if ( strcmp(u, "MIN") == 0 ) out = mv_num(fmin(arg_n(a, na, 0), arg_n(a, na, 1)));
	else if ( strcmp(u, "ASRCH") == 0 ) {
		MSVAR	*v = var_arg(n->v[0], sc);

		out = mv_num(MS_INVALID);
		if ( v != NULL ) {
			INT	start = arg_i(a, na, 1), len = arg_i(a, na, 3), step, cnt, k;
			MSV	val = ( na > 2 ) ? a[2] : mv_num(0);

			if ( len == 0 ) len = v->size - start;
			step = ( len < 0 ) ? -1 : 1;
			cnt = ( len < 0 ) ? -len : len;
			for ( k = 0; k < cnt; k++ ) {
				INT	idx = start + k * step;
				MSV	e;
				BOOL	eq;

				if ( idx < 0 || idx >= v->size ) break;
				e = var_get(v, idx);
				eq = (BOOL)( e.t == val.t && ( ( e.t == V_NUM && e.u.n == val.u.n ) || ( e.t == V_STR && e.u.s == val.u.s ) ) );
				mv_drop(e);
				if ( eq ) { out = mv_num(idx); break; }
			}
		}
	}
	else if ( strcmp(u, "ACMP") == 0 || strcmp(u, "SCMP") == 0 ) {
		MSVAR	*v1 = var_arg(n->nv > 0 ? n->v[0] : NULL, sc), *v2 = var_arg(n->nv > 2 ? n->v[2] : NULL, sc);

		out = mv_num(MS_INVALID);
		if ( v1 != NULL && v2 != NULL ) {
			INT	s1 = arg_i(a, na, 1), s2 = arg_i(a, na, 3);
			INT	len = eff_len(v1, s1, v2, s2, arg_i(a, na, 4)), k;

			out = mv_num(0);
			for ( k = 0; k < len; k++ ) {
				double	x = el_n(v1, s1 + k), y = el_n(v2, s2 + k);

				if ( u[0] == 'S' && x == 0 && y == 0 ) break;
				if ( x > y ) { out = mv_num(1); break; }
				if ( x < y ) { out = mv_num(-1); break; }
			}
		}
	}
	else if ( strcmp(u, "ACOPY") == 0 ) {
		MSVAR	*v1 = var_arg(n->nv > 0 ? n->v[0] : NULL, sc), *v2 = var_arg(n->nv > 2 ? n->v[2] : NULL, sc);

		if ( v1 != NULL && v2 != NULL ) {
			INT	s1 = arg_i(a, na, 1), s2 = arg_i(a, na, 3);
			INT	len = eff_len(v1, s1, v2, s2, arg_i(a, na, 4)), k;

			for ( k = 0; k < len; k++ ) {
				MSV	e = var_get(v2, s2 + k);

				var_set(v1, s1 + k, e);
				mv_drop(e);
			}
			out = mv_num(len);
		}
	}
	else if ( strcmp(u, "SLEN") == 0 ) {
		MSVAR	*v = var_arg(n->nv > 0 ? n->v[0] : NULL, sc);

		out = mv_num(MS_INVALID);
		if ( v != NULL ) {
			INT	k;

			for ( k = 0; k < v->size; k++ ) if ( el_n(v, k) == 0 ) { out = mv_num(k); break; }
		} else if ( na > 0 && a[0].t == V_ARR ) {
			INT	k;

			for ( k = 0; k < a[0].u.a->n; k++ ) if ( mv_n(a[0].u.a->e[k]) == 0 ) { out = mv_num(k); break; }
		}
	}
	else if ( strcmp(u, "STRNUM") == 0 ) out = fn_strnum(t, n, sc, a, na);
	else if ( strcmp(u, "SCONV") == 0 || strcmp(u, "SRCONV") == 0 ) {
		MSVAR	*v1 = var_arg(n->nv > 0 ? n->v[0] : NULL, sc), *v2 = var_arg(n->nv > 2 ? n->v[2] : NULL, sc);
		INT	mode = arg_i(a, na, 5);

		out = mv_num(MS_INVALID);
		if ( v1 != NULL && v2 != NULL ) {
			if ( u[1] == 'C' ) {
				if ( mode == 's' || mode == 'e' ) {
					out = mv_num(sconv(v1, arg_i(a, na, 1), v2, arg_i(a, na, 3), arg_i(a, na, 4), (BOOL)( mode == 'e' )));
				}
			} else if ( mode == 'S' || mode == 's' || mode == 'k' || mode == 'E' || mode == 'e' ) {
				out = mv_num(srconv(v1, arg_i(a, na, 1), v2, arg_i(a, na, 3), arg_i(a, na, 4), mode));
			}
		}
	}
	else if ( strcmp(u, "NEWIMGSEG") == 0 ) out = fn_newimgseg(a, na);
	else if ( strcmp(u, "FILELIST") == 0 ) out = fn_filelist(t, n, sc, a, na);
	else if ( strcmp(u, "GETGNM") == 0 || strcmp(u, "SETGNM") == 0 || strcmp(u, "DELGNM") == 0 ) {
		/* global names every process on the machine sees; integers only */
		MSSTR	*key = ( na > 0 ) ? mv_name(a[0]) : NULL;
		INT	val = 0;

		if ( key == NULL && na > 0 && a[0].t == V_NUM ) {
			MSBUF	b;

			mb_init(&b);
			ms_num_text(a[0].u.n, &b);
			key = ms_intern(b.s, b.n);
			mb_free(&b);
		}
		if ( key == NULL ) {
			out = mv_num(MS_INVALID);
		} else if ( u[0] == 'G' ) {
			out = ms_gnm_get(key->s, &val) ? mv_num(val) : mv_num(MS_INVALID);
		} else if ( u[0] == 'S' ) {
			out = ms_gnm_set(key->s, ( na > 1 ) ? mv_i(a[1]) : 0) ? mv_num(0) : mv_num(MS_INVALID);
		} else {
			out = ms_gnm_del(key->s) ? mv_num(0) : mv_num(MS_INVALID);
		}
	}
	else if ( strcmp(u, "TMDATE") == 0 ) out = fn_tmdate(t, n, sc, a, na);
	else if ( strcmp(u, "DATETM") == 0 ) out = fn_datetm(t, n, sc);
	else if ( ( p = rt_proc(n->name) ) != NULL ) {
		out = call_proc(t, p, n->v, n->nv, sc);
	}
	for ( i = 0; i < na; i++ ) mv_drop(a[i]);
	ms_free(a);
	return out;
}

/* ---------------------------------------------------------------- what happens */

typedef struct {
	INT	kind;
	MSSTR	*key;
	MSPROC	*proc;
	INT	index;
} HANDLER;

static HANDLER	*handlers;
static INT	nhandlers;
static MSPROC	*keyprocs[32];
static INT	nkeyprocs;
static MSPROC	*menuprocs[8];
static INT	nmenuprocs;

void rt_bind_handlers( void )
{
	INT	i, k;

	for ( i = 0; i < ms_prog.nproc; i++ ) {
		MSPROC	*p = ms_prog.proc[i];

		if ( p->event == EV_NONE ) continue;
		if ( p->event == EV_MENU ) {
			if ( nmenuprocs < 8 && p->menu != NULL ) menuprocs[nmenuprocs++] = p;
			continue;
		}
		if ( p->event == EV_KEY ) {
			if ( nkeyprocs < 32 ) keyprocs[nkeyprocs++] = p;
			continue;
		}
		/* several segments of one name: each place in the list its own */
		for ( k = 0; k < p->ntarget; k++ ) {
			INT	occ = 0, j;
			MSSEG	*inst;

			for ( j = 0; j < k; j++ ) if ( p->target[j] == p->target[k] ) occ++;
			inst = st_seg_occ(p->target[k], occ);
			handlers = ms_realloc(handlers, sizeof(HANDLER) * (size_t)( nhandlers + 1 ));
			handlers[nhandlers].kind = p->event;
			handlers[nhandlers].key = ( inst != NULL ) ? inst->key : p->target[k];
			handlers[nhandlers].proc = p;
			handlers[nhandlers].index = k;
			nhandlers++;
		}
	}
}

BOOL rt_has_handler( MSSEG *s, INT kind )
{
	INT	i;

	for ( i = 0; i < nhandlers; i++ ) {
		if ( handlers[i].kind == kind && handlers[i].key == s->key ) return TRUE;
	}
	return FALSE;
}

BOOL rt_has_any_handler( MSSEG *s )
{
	INT	i;

	for ( i = 0; i < nhandlers; i++ ) {
		if ( handlers[i].key == s->key ) return TRUE;
	}
	return FALSE;
}

BOOL rt_has_dclick( MSSEG *s )
{
	return rt_has_handler(s, EV_DCLICK);
}

void rt_event( INT kind, MSSEG *seg, INT x, INT y )
{
	INT	i;
	MSTH	*th;

	if ( rt.finished ) return;
	for ( i = 0; i < nhandlers; i++ ) {
		MSV	args[4];

		if ( handlers[i].kind != kind || handlers[i].key != seg->key ) continue;
		args[0] = mv_str(seg->key);
		args[1] = mv_num(handlers[i].index);
		args[2] = mv_num(x);
		args[3] = mv_num(y);
		th = rt_spawn(handlers[i].proc, args, 4, FALSE);
		if ( th != NULL ) th->evkey = seg->key;
	}
}

void rt_key( UW ch, UW meta )
{
	INT	i;

	if ( rt.finished ) return;
	ms_win.lastkey = ch;
	ms_win.metakey = meta;
	ms_win.kstat = meta;
	for ( i = 0; i < nkeyprocs; i++ ) {
		MSV	args[2];

		args[0] = mv_num(ch);
		args[1] = mv_num(meta);
		(void)rt_spawn(keyprocs[i], args, 2, FALSE);
	}
}

INT rt_menu_count( void )
{
	return nmenuprocs;
}

const UW *rt_menu_label( INT i, INT *p_n )
{
	if ( i < 0 || i >= nmenuprocs ) { *p_n = 0; return NULL; }
	*p_n = menuprocs[i]->nmenu;
	return menuprocs[i]->menu;
}

void rt_menu( INT i )
{
	if ( i < 0 || i >= nmenuprocs || rt.finished ) return;
	(void)rt_spawn(menuprocs[i], NULL, 0, FALSE);
}
