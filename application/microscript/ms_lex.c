/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_lex.c
 *	The script cut into tokens
 *
 *	Full width letters, digits and signs are read as the plain ones. A
 *	paragraph ends a statement, and so does ';' (which the display
 *	statements also read as "draw later"). '#' starts a comment to the
 *	end of the line, and a line ending in '\' goes on on the next. A name
 *	is known by its first 12 characters. '?' followed by letters or
 *	digits is a name too: the segments named by a number are written so.
 */

#include "ms.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static BOOL is_digit( UW c ) { return (BOOL)( c >= '0' && c <= '9' ); }
static BOOL is_hex( UW c )
{
	return (BOOL)( is_digit(c) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ) );
}
static BOOL is_alpha( UW c )
{
	return (BOOL)( c == '_' || ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c >= 0x80 );
}
static BOOL is_cont( UW c ) { return (BOOL)( is_alpha(c) || is_digit(c) ); }

typedef struct {
	UW	*s;
	INT	n;
	MSTOKS	*out;
	INT	line;
	BOOL	had;		/* the line has a token */
	BOOL	spaced;
} LX;

static MSTOK *emit( LX *x, UB type )
{
	MSTOK	*t;

	if ( x->out->n >= x->out->cap ) {
		x->out->cap = ( x->out->cap == 0 ) ? 256 : x->out->cap * 2;
		x->out->t = ms_realloc(x->out->t, sizeof(MSTOK) * (size_t)x->out->cap);
	}
	t = &x->out->t[x->out->n++];
	memset(t, 0, sizeof(*t));
	t->type = type;
	t->line = x->line;
	t->spaced = (UB)x->spaced;
	x->had = TRUE;
	x->spaced = FALSE;
	return t;
}

/* A name from code points: UTF-8, its first 12 characters */
static MSSTR *name_of( const UW *s, INT n )
{
	UB	buf[12 * 4];
	INT	k = 0, i;

	if ( n > 12 ) n = 12;
	for ( i = 0; i < n; i++ ) k += ms_utf8_enc(s[i], buf + k);
	return ms_intern((const char *)buf, k);
}

static UW esc_char( UW e )
{
	switch ( e ) {
	case 'n': return 0x0A;
	case 'r': return 0x0D;
	case 't': return 0x09;
	case 'b': return 0x08;
	case '0': return 0x00;
	}
	return e;
}

static const char *const two_ops[] = { ">=", "<=", "==", "!=", "&&", "||", "<<", ">>", NULL };
static const char one_ops[] = "+-*/%&|^~<>=!,;:()[].@";

ER ms_lex( const char *src, INT len, MSTOKS *out, char *err, INT errmax )
{
	LX	x;
	INT	i, j, k;

	memset(&x, 0, sizeof(x));
	x.out = out;
	x.line = 1;
	x.spaced = TRUE;
	out->n = 0;

	/* the source as code points, normalised */
	x.s = ms_alloc(sizeof(UW) * (size_t)( len + 1 ));
	for ( i = 0; i < len; ) {
		UW	cp;
		INT	d = ms_utf8_dec((const UB *)src + i, len - i, &cp);

		if ( d <= 0 ) break;
		x.s[x.n++] = ms_norm_cp(cp);
		i += d;
	}

	for ( i = 0; i < x.n; ) {
		UW	c = x.s[i];

		if ( c == '\r' ) { i++; continue; }
		if ( c == '\n' ) {
			if ( x.had ) {
				MSTOK	*t = emit(&x, T_NL);

				t->op = '\n';
			}
			x.line++;
			x.had = FALSE;
			x.spaced = TRUE;
			i++;
			continue;
		}
		if ( c == ' ' || c == '\t' ) { x.spaced = TRUE; i++; continue; }

		if ( c == '#' ) {
			/* to the line's end; a line ending in '\' carries the comment on */
			for ( ;; ) {
				while ( i < x.n && x.s[i] != '\n' && x.s[i] != '\r' ) i++;
				j = i - 1;
				while ( j >= 0 && ( x.s[j] == ' ' || x.s[j] == '\t' ) ) j--;
				if ( j >= 0 && x.s[j] == '\\' ) {
					while ( i < x.n && ( x.s[i] == '\r' || x.s[i] == '\n' ) ) {
						if ( x.s[i] == '\n' ) x.line++;
						i++;
					}
					continue;
				}
				break;
			}
			continue;
		}

		if ( c == '\\' && i + 1 < x.n && ( x.s[i + 1] == '\n' || x.s[i + 1] == '\r' ) ) {
			i++;
			while ( i < x.n && ( x.s[i] == '\r' || x.s[i] == '\n' ) ) {
				if ( x.s[i] == '\n' ) x.line++;
				i++;
			}
			x.spaced = TRUE;
			continue;
		}

		if ( c == ';' ) {
			MSTOK	*t = emit(&x, T_NL);

			t->op = ';';
			i++;
			continue;
		}

		if ( c == '$' ) {
			MSTOK	*t;

			for ( j = i + 1; j < x.n && is_cont(x.s[j]); j++ ) ;
			if ( j == i + 1 ) {
				snprintf(err, (size_t)errmax, "字句エラー(%d行): 不正なシステム変数記法", x.line);
				return E_PAR;
			}
			t = emit(&x, T_SYSVAR);
			t->name = name_of(x.s + i + 1, j - i - 1);
			i = j;
			continue;
		}

		if ( is_digit(c) ) {
			char	num[64];
			BOOL	flt = FALSE;
			MSTOK	*t;

			if ( c == '0' && i + 1 < x.n && ( x.s[i + 1] == 'x' || x.s[i + 1] == 'X' ) ) {
				double	v = 0;

				for ( j = i + 2; j < x.n && is_hex(x.s[j]); j++ ) {
					UW	h = x.s[j];

					v = v * 16 + (double)( is_digit(h) ? h - '0'
							   : ( h >= 'a' ) ? h - 'a' + 10 : h - 'A' + 10 );
				}
				if ( j == i + 2 ) {
					snprintf(err, (size_t)errmax, "字句エラー(%d行): 16進数の桁がありません", x.line);
					return E_PAR;
				}
				t = emit(&x, T_INT);
				t->num = v;
				i = j;
				continue;
			}
			for ( j = i; j < x.n && is_digit(x.s[j]); j++ ) ;
			if ( j < x.n && x.s[j] == '.' ) {
				flt = TRUE;
				for ( j++; j < x.n && is_digit(x.s[j]); j++ ) ;
			}
			if ( j < x.n && ( x.s[j] == 'e' || x.s[j] == 'E' ) ) {
				flt = TRUE;
				j++;
				if ( j < x.n && ( x.s[j] == '+' || x.s[j] == '-' ) ) j++;
				while ( j < x.n && is_digit(x.s[j]) ) j++;
			}
			for ( k = 0; k < j - i && k < (INT)sizeof(num) - 1; k++ ) num[k] = (char)x.s[i + k];
			num[k] = 0;
			t = emit(&x, flt ? T_FLOAT : T_INT);
			t->num = strtod(num, NULL);
			i = j;
			continue;
		}

		if ( c == '\'' ) {
			INT	start = x.line, count = 0;
			UW	cp = 0;
			MSTOK	*t;

			for ( j = i + 1; j < x.n && x.s[j] != '\''; ) {
				if ( x.s[j] == '\\' && j + 1 < x.n ) {
					cp = esc_char(x.s[j + 1]);
					j += 2;
				} else if ( x.s[j] == '\n' ) {
					snprintf(err, (size_t)errmax, "字句エラー(%d行): 文字定数が閉じていません", start);
					return E_PAR;
				} else {
					cp = x.s[j++];
				}
				count++;
				if ( count > 1 ) break;
			}
			if ( j >= x.n || x.s[j] != '\'' ) {
				if ( count == 0 ) {
					t = emit(&x, T_INT);
					t->num = 0;
					i = j + ( ( j < x.n && x.s[j] == '\'' ) ? 1 : 0 );
					continue;
				}
				snprintf(err, (size_t)errmax, "字句エラー(%d行): 文字定数が閉じていません", start);
				return E_PAR;
			}
			t = emit(&x, T_INT);
			t->num = ( count == 0 ) ? 0 : cp;
			i = j + 1;
			continue;
		}

		if ( c == '"' ) {
			INT	start = x.line, n = 0, cap = 16;
			UW	*chars = ms_alloc(sizeof(UW) * (size_t)cap);
			MSTOK	*t;

			for ( j = i + 1; j < x.n && x.s[j] != '"'; ) {
				UW	ch;

				if ( x.s[j] == '\\' && j + 1 < x.n ) {
					ch = esc_char(x.s[j + 1]);
					j += 2;
				} else if ( x.s[j] == '\n' ) {
					snprintf(err, (size_t)errmax, "字句エラー(%d行): 文字列定数が閉じていません", start);
					return E_PAR;
				} else {
					ch = x.s[j++];
				}
				if ( n >= cap ) {
					cap *= 2;
					chars = ms_realloc(chars, sizeof(UW) * (size_t)cap);
				}
				chars[n++] = ch;
			}
			if ( j >= x.n ) {
				snprintf(err, (size_t)errmax, "字句エラー(%d行): 文字列定数が閉じていません", start);
				return E_PAR;
			}
			t = emit(&x, T_STRING);
			t->chars = chars;
			t->nchars = n;
			i = j + 1;
			continue;
		}

		if ( i + 1 < x.n ) {
			for ( k = 0; two_ops[k] != NULL; k++ ) {
				if ( c == (UW)two_ops[k][0] && x.s[i + 1] == (UW)two_ops[k][1] ) break;
			}
			if ( two_ops[k] != NULL ) {
				MSTOK	*t = emit(&x, T_OP);

				t->op = OPC(two_ops[k][0], two_ops[k][1]);
				i += 2;
				continue;
			}
		}
		if ( c < 0x80 && c != 0 && strchr(one_ops, (int)c) != NULL ) {
			MSTOK	*t = emit(&x, T_OP);

			t->op = (UH)c;
			i++;
			continue;
		}

		if ( is_alpha(c) || c == '?' ) {
			MSTOK	*t;

			for ( j = i + 1; j < x.n && is_cont(x.s[j]); j++ ) ;
			if ( c == '?' && j == i + 1 ) {
				snprintf(err, (size_t)errmax, "字句エラー(%d行): 不正なセグメント名記法", x.line);
				return E_PAR;
			}
			t = emit(&x, T_IDENT);
			t->name = name_of(x.s + i, j - i);
			i = j;
			continue;
		}

		{
			UB	u[5];
			INT	m = ms_utf8_enc(c, u);

			u[m] = 0;
			snprintf(err, (size_t)errmax, "字句エラー(%d行): 予期しない文字: \"%s\"", x.line, (char *)u);
		}
		return E_PAR;
	}
	if ( x.had ) {
		MSTOK	*t = emit(&x, T_NL);

		t->op = '\n';
	}
	emit(&x, T_EOF);
	ms_free(x.s);
	return E_OK;
}
