/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_db.c
 *	マイクロカード: the cards, their values, the format, searching, marks
 *	and sorting (design 11.18). The records are read and written in
 *	mc_bin.c. A database always has at least one card: the last one is
 *	made empty rather than taken away.
 *
 *	The cards worked on are all the cards, or in the search state those
 *	found; they keep their places among the others, so a card added,
 *	sorted or deleted in the search state leaves the rest as they were.
 */

#include "mc_db.h"

/* ---------------------------------------------------------------- bytes */

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

EXPORT INT mc_len( CONST UB *s )
{
	INT	n = 0;

	if ( s == NULL ) return 0;
	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same( CONST UB *a, INT an, CONST UB *b, INT bn )
{
	INT	i;

	if ( an != bn ) return FALSE;
	for ( i = 0; i < an; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
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

LOCAL UB *dup_bytes( CONST UB *s, INT n )
{
	UB	*d = (UB *)mc_alloc(n + 1);

	if ( d != NULL ) {
		m_copy(d, s, n);
		d[n] = 0;
	}
	return d;
}

/* ---------------------------------------------------------------- letters */

EXPORT INT mc_u8_get( CONST UB *s, INT n, UINT *p_c )
{
	UB	c;
	INT	len, i;
	UINT	v;

	if ( n <= 0 ) {
		*p_c = 0;
		return 0;
	}
	c = s[0];
	if ( c < 0x80 ) {
		*p_c = c;
		return 1;
	}
	len = ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 1;
	if ( len == 1 || len > n ) {
		*p_c = c;
		return 1;
	}
	v = ( len == 4 ) ? c & 0x07 : ( len == 3 ) ? c & 0x0F : c & 0x1F;
	for ( i = 1; i < len; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) {
			*p_c = c;
			return 1;
		}
		v = ( v << 6 ) | ( s[i] & 0x3F );
	}
	*p_c = v;
	return len;
}

EXPORT INT mc_u8_put( UINT c, UB *o )
{
	if ( c < 0x80 ) {
		o[0] = (UB)c;
		return 1;
	}
	if ( c < 0x800 ) {
		o[0] = (UB)( 0xC0 | ( c >> 6 ) );
		o[1] = (UB)( 0x80 | ( c & 0x3F ) );
		return 2;
	}
	if ( c < 0x10000 ) {
		o[0] = (UB)( 0xE0 | ( c >> 12 ) );
		o[1] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
		o[2] = (UB)( 0x80 | ( c & 0x3F ) );
		return 3;
	}
	o[0] = (UB)( 0xF0 | ( c >> 18 ) );
	o[1] = (UB)( 0x80 | ( ( c >> 12 ) & 0x3F ) );
	o[2] = (UB)( 0x80 | ( ( c >> 6 ) & 0x3F ) );
	o[3] = (UB)( 0x80 | ( c & 0x3F ) );
	return 4;
}

EXPORT INT mc_letters( CONST UB *s, INT n )
{
	INT	i = 0, k = 0;
	UINT	c;

	while ( i < n ) {
		INT	l = mc_u8_get(s + i, n - i, &c);

		if ( l <= 0 ) break;
		i += l;
		k++;
	}
	return k;
}

EXPORT INT mc_cut( CONST UB *s, INT n, INT letters )
{
	INT	i = 0, k = 0;
	UINT	c;

	while ( i < n && k < letters ) {
		INT	l = mc_u8_get(s + i, n - i, &c);

		if ( l <= 0 ) break;
		i += l;
		k++;
	}
	return i;
}

EXPORT BOOL mc_wide( UINT c )
{
	if ( c >= 0x0391 && c <= 0x0451 ) return TRUE;	/* Greek and Cyrillic, as JIS has them */
	if ( c < 0x2010 ) return FALSE;
	if ( c >= 0xFF61 && c <= 0xFFDC ) return FALSE;	/* half width katakana and hangul */
	if ( c >= 0xFFE8 && c <= 0xFFEE ) return FALSE;
	return TRUE;
}

/* ---------------------------------------------------------------- numbers */

/* A digit of either width, -1 none */
LOCAL INT digit_of( UINT c )
{
	if ( c >= '0' && c <= '9' ) return (INT)( c - '0' );
	if ( c >= 0xFF10 && c <= 0xFF19 ) return (INT)( c - 0xFF10 );
	return -1;
}

LOCAL BOOL is_minus( UINT c )
{
	return (BOOL)( c == '-' || c == 0x2212 || c == 0xFF0D );
}

LOCAL BOOL is_plus( UINT c )
{
	return (BOOL)( c == '+' || c == 0xFF0B );
}

LOCAL BOOL is_point( UINT c )
{
	return (BOOL)( c == '.' || c == 0xFF0E );
}

LOCAL BOOL is_comma( UINT c )
{
	return (BOOL)( c == ',' || c == 0xFF0C );
}

LOCAL BOOL is_space( UINT c )
{
	return (BOOL)( c == ' ' || c == 0x3000 || c == '\t' );
}

/*
 * The number at s: a sign, digits with commas between them, a point and
 * more digits, in either width. Written as kept into out; *p_used the
 * bytes of s it took. -1 when no digit came.
 */
LOCAL INT num_read( CONST UB *s, INT n, UB *out, INT max, INT *p_used )
{
	INT	i = 0, o = 0, l, digits = 0, intd = 0;
	BOOL	neg = FALSE, point = FALSE;
	UINT	c;
	UB	t[64];
	INT	tn = 0;

	while ( i < n && ( l = mc_u8_get(s + i, n - i, &c) ) > 0 && is_space(c) ) i += l;
	if ( i < n && ( l = mc_u8_get(s + i, n - i, &c) ) > 0 && ( is_minus(c) || is_plus(c) ) ) {
		neg = is_minus(c);
		i += l;
	}
	while ( i < n ) {
		INT	d;

		l = mc_u8_get(s + i, n - i, &c);
		if ( l <= 0 ) break;
		d = digit_of(c);
		if ( d >= 0 ) {
			if ( tn < (INT)sizeof(t) - 2 ) t[tn++] = (UB)( '0' + d );
			digits++;
			if ( !point ) intd++;
		} else if ( is_point(c) && !point ) {
			/* a point needs a digit after it to be one */
			UINT	nc;
			INT	nl = mc_u8_get(s + i + l, n - i - l, &nc);

			if ( nl <= 0 || digit_of(nc) < 0 ) break;
			point = TRUE;
			if ( intd == 0 && tn < (INT)sizeof(t) - 2 ) {
				t[tn++] = '0';
				intd = 1;
			}
			if ( tn < (INT)sizeof(t) - 2 ) t[tn++] = '.';
		} else if ( is_comma(c) && !point && intd > 0 ) {
			UINT	nc;
			INT	nl = mc_u8_get(s + i + l, n - i - l, &nc);

			if ( nl <= 0 || digit_of(nc) < 0 ) break;
		} else {
			break;
		}
		i += l;
	}
	if ( digits == 0 ) return -1;
	/* leading zeros of the whole part dropped, one kept */
	{
		INT	k = 0;

		while ( k + 1 < tn && t[k] == '0' && t[k + 1] != '.' ) k++;
		if ( neg ) {
			BOOL	zero = TRUE;
			INT	j;

			for ( j = k; j < tn; j++ ) {
				if ( t[j] >= '1' && t[j] <= '9' ) zero = FALSE;
			}
			if ( !zero && o < max - 1 ) out[o++] = '-';
		}
		for ( ; k < tn && o < max - 1; k++ ) out[o++] = t[k];
	}
	if ( max > 0 ) out[o] = 0;
	if ( p_used != NULL ) *p_used = i;
	return o;
}

EXPORT INT mc_num_norm( CONST UB *s, INT n, UB *out, INT max )
{
	INT	used = 0, len, i;
	UINT	c;

	len = num_read(s, n, out, max, &used);
	if ( len < 0 ) return -1;
	/* nothing but spaces may follow */
	for ( i = used; i < n; ) {
		INT	l = mc_u8_get(s + i, n - i, &c);

		if ( l <= 0 ) break;
		if ( !is_space(c) ) return -1;
		i += l;
	}
	return len;
}

EXPORT INT mc_num_lead( CONST UB *s, INT n, UB *out, INT max )
{
	INT	len = num_read(s, n, out, max, NULL);

	if ( len < 0 ) {
		if ( max > 0 ) out[0] = 0;
		return 0;
	}
	return len;
}

/* The parts of a number as kept: its sign, its whole part and its fraction */
LOCAL void num_parts( CONST UB *s, BOOL *p_neg, CONST UB **p_i, INT *p_in, CONST UB **p_f, INT *p_fn )
{
	INT	k = 0, n = mc_len(s), p;

	*p_neg = FALSE;
	if ( n > 0 && s[0] == '-' ) {
		*p_neg = TRUE;
		k = 1;
	}
	for ( p = k; p < n && s[p] != '.'; p++ ) ;
	*p_i = s + k;
	*p_in = p - k;
	*p_f = ( p < n ) ? s + p + 1 : s + n;
	*p_fn = ( p < n ) ? n - p - 1 : 0;
}

EXPORT INT mc_num_cmp( CONST UB *a, CONST UB *b )
{
	BOOL		na, nb;
	CONST UB	*ia, *ib, *fa, *fb;
	INT		ian, ibn, fan, fbn, r = 0, k;

	num_parts(a, &na, &ia, &ian, &fa, &fan);
	num_parts(b, &nb, &ib, &ibn, &fb, &fbn);
	if ( na != nb ) return na ? -1 : 1;
	if ( ian != ibn ) {
		r = ( ian < ibn ) ? -1 : 1;
	} else {
		for ( k = 0; k < ian && r == 0; k++ ) {
			if ( ia[k] != ib[k] ) r = ( ia[k] < ib[k] ) ? -1 : 1;
		}
		for ( k = 0; r == 0 && ( k < fan || k < fbn ); k++ ) {
			UB	x = ( k < fan ) ? fa[k] : '0', y = ( k < fbn ) ? fb[k] : '0';

			if ( x != y ) r = ( x < y ) ? -1 : 1;
		}
	}
	return na ? -r : r;
}

/* ---------------------------------------------------------------- the database */

EXPORT void mc_view_default( MCVIEW *v )
{
	v->csize = 10;
	v->lsize = 10;
	v->cprop = TRUE;
	v->lprop = TRUE;
	v->cgray = FALSE;
	v->lgray = FALSE;
	v->names = TRUE;
}

EXPORT void mc_print_default( MCPRINT *p, CONST UB *layout )
{
	INT	n = mc_len(layout);

	m_zero(p, sizeof(*p));
	if ( n >= MC_LAYOUT_NAME ) n = MC_LAYOUT_NAME - 1;
	if ( layout != NULL ) m_copy(p->layout, layout, n);
	p->layout[n] = 0;
	p->across = 1;
	p->down = ( n == 0 ) ? 0 : 1;	/* the list: as many rows as the page holds */
	p->gapx = 0;
	p->gapy = 0;
	p->paper = MC_PAPER_A4;
	p->margin[0] = p->margin[1] = p->margin[2] = p->margin[3] = 20;
}

EXPORT MCPRINT *mc_print_of( MCDB *db, CONST UB *layout )
{
	INT	i, n = mc_len(layout);

	for ( i = 0; i < db->npr; i++ ) {
		if ( same(db->pr[i].layout, mc_len(db->pr[i].layout), layout, n) ) return &db->pr[i];
	}
	if ( db->npr >= MC_PRINT_MAX ) return &db->pr[MC_PRINT_MAX - 1];
	mc_print_default(&db->pr[db->npr], layout);
	return &db->pr[db->npr++];
}

EXPORT void mc_init( MCDB *db )
{
	m_zero(db, sizeof(*db));
	mc_view_default(&db->view);
}

LOCAL void card_free( MCCARD *c )
{
	INT	k;

	if ( c == NULL ) return;
	for ( k = 0; k < c->nv; k++ ) mc_free(c->v[k]);
	mc_free(c->v);
	mc_free(c);
}

EXPORT void mc_clear( MCDB *db )
{
	INT	i;

	for ( i = 0; i < db->n; i++ ) card_free(db->c[i]);
	mc_free(db->c);
	mc_free(db->layout);
	mc_init(db);
}

LOCAL MCCARD *card_make( INT nf )
{
	MCCARD	*c = (MCCARD *)mc_alloc(sizeof(MCCARD));
	INT	nv = ( nf > 0 ) ? nf : 1;

	if ( c == NULL ) return NULL;
	m_zero(c, sizeof(*c));
	c->v = (UB **)mc_alloc((INT)sizeof(UB *) * nv);
	if ( c->v == NULL ) {
		mc_free(c);
		return NULL;
	}
	m_zero(c->v, (INT)sizeof(UB *) * nv);
	c->nv = nv;
	return c;
}

LOCAL BOOL room( MCDB *db )
{
	MCCARD	**nc;
	INT	cap;

	if ( db->n < db->cap ) return TRUE;
	if ( db->n >= MC_CARD_MAX ) return FALSE;
	cap = ( db->cap > 0 ) ? db->cap * 2 : 64;
	nc = (MCCARD **)mc_alloc((INT)sizeof(MCCARD *) * cap);
	if ( nc == NULL ) return FALSE;
	if ( db->n > 0 ) m_copy(nc, db->c, (INT)sizeof(MCCARD *) * db->n);
	mc_free(db->c);
	db->c = nc;
	db->cap = cap;
	return TRUE;
}

LOCAL MCCARD *card_put( MCDB *db, MCCARD *c, INT at )
{
	if ( c == NULL ) return NULL;
	if ( !room(db) ) {
		card_free(c);
		return NULL;
	}
	if ( at < 0 ) at = 0;
	if ( at > db->n ) at = db->n;
	m_copy(&db->c[at + 1], &db->c[at], (INT)sizeof(MCCARD *) * ( db->n - at ));
	db->c[at] = c;
	db->n++;
	c->found = db->searching;
	return c;
}

EXPORT MCCARD *mc_card_new( MCDB *db, INT at )
{
	return card_put(db, card_make(db->nf), at);
}

EXPORT MCCARD *mc_card_dup( MCDB *db, INT from, INT at )
{
	MCCARD	*s, *c;
	INT	k;

	if ( from < 0 || from >= db->n ) return NULL;
	s = db->c[from];
	c = card_make(db->nf);
	if ( c == NULL ) return NULL;
	for ( k = 0; k < db->nf && k < s->nv; k++ ) {
		if ( s->v[k] != NULL && ( c->v[k] = dup_bytes(s->v[k], mc_len(s->v[k])) ) == NULL ) {
			card_free(c);
			return NULL;
		}
	}
	return card_put(db, c, at);
}

EXPORT void mc_card_blank( MCDB *db, INT i )
{
	MCCARD	*c;
	INT	k;

	if ( i < 0 || i >= db->n ) return;
	c = db->c[i];
	for ( k = 0; k < c->nv; k++ ) {
		mc_free(c->v[k]);
		c->v[k] = NULL;
	}
}

EXPORT BOOL mc_card_empty( CONST MCDB *db, INT i )
{
	INT	k;

	if ( i < 0 || i >= db->n ) return TRUE;
	for ( k = 0; k < db->c[i]->nv; k++ ) {
		if ( db->c[i]->v[k] != NULL ) return FALSE;
	}
	return TRUE;
}

EXPORT void mc_card_del( MCDB *db, INT i )
{
	if ( i < 0 || i >= db->n ) return;
	if ( db->n == 1 ) {
		/* the last card is made empty, not taken away */
		mc_card_blank(db, 0);
		return;
	}
	card_free(db->c[i]);
	m_copy(&db->c[i], &db->c[i + 1], (INT)sizeof(MCCARD *) * ( db->n - i - 1 ));
	db->n--;
}

EXPORT CONST UB *mc_get( CONST MCCARD *c, INT f )
{
	if ( c == NULL || f < 0 || f >= c->nv ) return NULL;
	return c->v[f];
}

EXPORT ER mc_set( MCDB *db, MCCARD *c, INT f, CONST UB *v, INT len )
{
	UB	*nv = NULL;
	UB	num[MC_NUM_MAX * 2 + 8];

	if ( c == NULL || f < 0 || f >= db->nf || f >= c->nv ) return E_PAR;
	if ( v != NULL && len > 0 ) {
		CONST MCFIELD	*fd = &db->f[f];

		if ( fd->type == MC_T_NUM ) {
			INT	k = mc_num_norm(v, len, num, sizeof(num));

			if ( k < 0 ) return E_PAR;
			if ( k > MC_NUM_MAX ) return E_LIMIT;
			nv = dup_bytes(num, k);
		} else if ( fd->type == MC_T_LINK ) {
			if ( v[0] != '<' ) return E_PAR;
			nv = dup_bytes(v, len);
		} else {
			INT	max = ( fd->max > 0 && fd->max <= MC_TEXT_MAX ) ? fd->max : MC_TEXT_MAX;
			INT	k = mc_tron_fit(v, len, max), i;

			nv = dup_bytes(v, k);
			for ( i = 0; nv != NULL && i < k; i++ ) {
				if ( nv[i] == '\t' || nv[i] == '\n' || nv[i] == '\r' ) nv[i] = ' ';
			}
			if ( nv != NULL && k == 0 ) {
				mc_free(nv);
				nv = NULL;
			}
		}
		if ( nv == NULL && !( fd->type == MC_T_TEXT ) ) return E_NOMEM;
	}
	mc_free(c->v[f]);
	c->v[f] = nv;
	return E_OK;
}

EXPORT INT mc_shown( CONST MCDB *db, CONST MCCARD *c, INT f, UB *out, INT max )
{
	CONST UB	*v = mc_get(c, f);
	INT		n;

	if ( max <= 0 ) return 0;
	out[0] = 0;
	if ( v == NULL || f >= db->nf ) return 0;
	if ( db->f[f].type == MC_T_LINK ) {
		(void)mc_link_name(v, mc_len(v), out, max);	/* what fits, for showing */
		return (INT)mc_len(out);
	}
	n = mc_len(v);
	if ( n > max - 1 ) n = mc_cut(v, n, max / 4);
	m_copy(out, v, n);
	out[n] = 0;
	return n;
}

EXPORT UB *mc_link_name_all( CONST UB *el, INT len, INT *p_n )
{
	INT	n = mc_link_name(el, len, NULL, 0), got;
	UB	*b;

	*p_n = 0;
	if ( n <= 0 ) return NULL;
	b = (UB *)mc_alloc(n + 1);
	if ( b == NULL ) return NULL;
	got = mc_link_name(el, len, b, n + 1);
	if ( got <= 0 ) {
		mc_free(b);
		return NULL;
	}
	*p_n = mc_len(b);
	return b;
}

EXPORT INT mc_link_count( CONST MCDB *db )
{
	INT	i, k, n = ( db->layout != NULL ) ? 1 : 0;

	for ( k = 0; k < db->nf; k++ ) {
		if ( db->f[k].type != MC_T_LINK ) continue;
		for ( i = 0; i < db->n; i++ ) {
			if ( mc_get(db->c[i], k) != NULL ) n++;
		}
	}
	return n;
}

/* ---------------------------------------------------------------- the format */

EXPORT ER mc_fields_check( CONST MCFIELD *f, INT n, INT *p_at )
{
	INT	k, j;

	if ( n > MC_FIELD_MAX ) {
		*p_at = MC_FIELD_MAX;
		return E_LIMIT;
	}
	for ( k = 0; k < n; k++ ) {
		INT	len = mc_len(f[k].name), i;
		UINT	c;

		*p_at = k;
		if ( len == 0 || mc_tron_fit(f[k].name, len, MC_NAME_LETTERS) < len ) return E_PAR;
		for ( i = 0; i < len; ) {
			INT	l = mc_u8_get(f[k].name + i, len - i, &c);

			if ( l <= 0 || !mc_wide(c) ) return E_PAR;
			i += l;
		}
		for ( j = 0; j < k; j++ ) {
			if ( same(f[j].name, mc_len(f[j].name), f[k].name, len) ) return E_OBJ;
		}
		if ( f[k].type < MC_T_TEXT || f[k].type > MC_T_LINK ) return E_PAR;
		if ( f[k].type == MC_T_TEXT && ( f[k].max < 1 || f[k].max > MC_TEXT_MAX ) ) return E_LIMIT;
		if ( f[k].type == MC_T_NUM && ( f[k].max < 1 || f[k].max > MC_NUM_MAX ) ) return E_LIMIT;
		if ( f[k].type == MC_T_LINK && ( f[k].max < 1 || f[k].max > MC_LINK_LETTERS ) ) return E_LIMIT;
		if ( f[k].width < 0 || f[k].width > MC_WIDTH_MAX ) return E_LIMIT;
	}
	*p_at = -1;
	return E_OK;
}

/*
 * A value of the old field 'of' as the new field nf would hold it, in
 * out (its length; 0 empty, -1 a link kept as it is); *p_lost when
 * something did not come over.
 */
LOCAL INT convert( CONST MCDB *db, CONST MCCARD *c, INT of, CONST MCFIELD *nf, UB *out, INT max, BOOL *p_lost )
{
	CONST UB	*v = mc_get(c, of);
	INT		n = mc_len(v), ot = db->f[of].type;

	*p_lost = FALSE;
	if ( v == NULL ) return 0;
	if ( nf->type == MC_T_LINK ) {
		if ( ot == MC_T_LINK ) return -1;
		*p_lost = TRUE;
		return 0;
	}
	if ( ot == MC_T_LINK ) {
		if ( nf->type == MC_T_NUM ) {
			*p_lost = TRUE;
			return 0;
		}
		n = mc_link_name(v, n, out, max);
		if ( n > mc_len(out) ) {
			*p_lost = TRUE;		/* the name is longer than the field takes */
			n = mc_len(out);
		}
		v = out;
	} else if ( ot == MC_T_TEXT && nf->type == MC_T_NUM ) {
		UB	t[MC_NUM_MAX * 2 + 8];
		INT	k;

		/* as far as the text reads as a number from its start */
		if ( mc_num_norm(v, n, t, sizeof(t)) < 0 ) *p_lost = TRUE;
		k = mc_num_lead(v, n, t, sizeof(t));
		if ( k > MC_NUM_MAX ) {
			*p_lost = TRUE;
			return 0;
		}
		m_copy(out, t, k);
		return k;
	}
	if ( v != out ) {
		if ( n > max - 1 ) n = max - 1;
		m_copy(out, v, n);
	}
	if ( nf->type == MC_T_TEXT ) {
		INT	k = mc_tron_fit(out, n, nf->max);

		if ( k < n ) *p_lost = TRUE;
		n = k;
	}
	return n;
}

EXPORT INT mc_format_loss( CONST MCDB *db, CONST MCFIELD *f, CONST INT *from, INT n, BOOL *lose )
{
	UB	*buf = (UB *)mc_alloc(MC_VALUE_MAX + 8);
	INT	i, k, count = 0;

	for ( k = 0; k < n; k++ ) lose[k] = FALSE;
	if ( buf == NULL ) return 0;
	for ( k = 0; k < n; k++ ) {
		if ( from[k] < 0 || from[k] >= db->nf ) continue;
		for ( i = 0; i < db->n && !lose[k]; i++ ) {
			BOOL	lost;

			(void)convert(db, db->c[i], from[k], &f[k], buf, MC_VALUE_MAX, &lost);
			if ( lost ) lose[k] = TRUE;
		}
		if ( lose[k] ) count++;
	}
	mc_free(buf);
	return count;
}

EXPORT ER mc_format_set( MCDB *db, CONST MCFIELD *f, CONST INT *from, INT n )
{
	UB	*buf = (UB *)mc_alloc(MC_VALUE_MAX + 8);
	UB	**nv;
	INT	i, k;
	ER	er = E_OK;

	if ( buf == NULL ) return E_NOMEM;
	for ( i = 0; i < db->n && er >= E_OK; i++ ) {
		MCCARD	*c = db->c[i];
		INT	cnt = ( n > 0 ) ? n : 1;

		nv = (UB **)mc_alloc((INT)sizeof(UB *) * cnt);
		if ( nv == NULL ) {
			er = E_NOMEM;
			break;
		}
		m_zero(nv, (INT)sizeof(UB *) * cnt);
		for ( k = 0; k < n; k++ ) {
			BOOL	lost;
			INT	len;

			if ( from[k] < 0 || from[k] >= db->nf ) continue;
			len = convert(db, c, from[k], &f[k], buf, MC_VALUE_MAX, &lost);
			if ( len < 0 ) {
				nv[k] = c->v[from[k]];
				c->v[from[k]] = NULL;
			} else if ( len > 0 ) {
				nv[k] = dup_bytes(buf, len);
			}
		}
		for ( k = 0; k < c->nv; k++ ) mc_free(c->v[k]);
		mc_free(c->v);
		c->v = nv;
		c->nv = cnt;
	}
	mc_free(buf);
	if ( er < E_OK ) return er;
	for ( k = 0; k < n; k++ ) db->f[k] = f[k];
	db->nf = n;
	return E_OK;
}

/* ---------------------------------------------------------------- the cards worked on */

EXPORT BOOL mc_in( CONST MCDB *db, INT i )
{
	if ( i < 0 || i >= db->n ) return FALSE;
	return (BOOL)( !db->searching || db->c[i]->found );
}

EXPORT INT mc_count( CONST MCDB *db )
{
	INT	i, k = 0;

	if ( !db->searching ) return db->n;
	for ( i = 0; i < db->n; i++ ) {
		if ( db->c[i]->found ) k++;
	}
	return k;
}

EXPORT INT mc_index( CONST MCDB *db, INT pos )
{
	INT	i, k = 0;

	if ( pos < 0 ) return -1;
	if ( !db->searching ) return ( pos < db->n ) ? pos : -1;
	for ( i = 0; i < db->n; i++ ) {
		if ( !db->c[i]->found ) continue;
		if ( k++ == pos ) return i;
	}
	return -1;
}

EXPORT INT mc_pos( CONST MCDB *db, INT i )
{
	INT	j, k = 0;

	if ( !mc_in(db, i) ) return -1;
	if ( !db->searching ) return i;
	for ( j = 0; j < i; j++ ) {
		if ( db->c[j]->found ) k++;
	}
	return k;
}

/* ---------------------------------------------------------------- conditions */

/* Whether the bytes at s begin with the letter written w (UTF-8) */
LOCAL INT starts( CONST UB *s, INT n, CONST char *w )
{
	INT	l = mc_len((CONST UB *)w);

	return at_is(s, n, w) ? l : 0;
}

LOCAL BOOL has_wild( CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( s[i] == '*' || s[i] == '?' ) return TRUE;
	}
	return FALSE;
}

EXPORT void mc_cond_free( MCCOND *k )
{
	mc_free(k->a);
	if ( k->rx != NULL ) mc_rx_free(k->rx);
	m_zero(k, sizeof(*k));
}

EXPORT ER mc_cond_make( MCCOND *k, INT type, CONST UB *s, INT n )
{
	INT	l;
	CONST UB *a;
	INT	an;

	m_zero(k, sizeof(*k));
	if ( s == NULL || n <= 0 ) return E_OK;
	/* ≒/, ≒ and /: regular expressions */
	if ( ( l = starts(s, n, "≒/") ) > 0 ) {
		k->op = MC_OP_RX;
		return mc_rx_make(s + l, n - l, MC_RX_FOLD, &k->rx);
	}
	if ( ( l = starts(s, n, "≒") ) > 0 ) {
		k->op = MC_OP_RX;
		return mc_rx_make(s + l, n - l, MC_RX_SIMPLE | MC_RX_FOLD, &k->rx);
	}
	if ( s[0] == '/' ) {
		k->op = MC_OP_RX;
		return mc_rx_make(s + 1, n - 1, 0, &k->rx);
	}
	if ( ( l = starts(s, n, "==") ) > 0 ) {
		k->op = MC_OP_EQ;		/* the rest as it is: a sign itself */
	} else if ( ( l = starts(s, n, "≠") ) > 0 || ( l = starts(s, n, "<>") ) > 0 ) {
		k->op = MC_OP_NE;
	} else if ( ( l = starts(s, n, "≦") ) > 0 || ( l = starts(s, n, "<=") ) > 0 ) {
		k->op = MC_OP_LE;
	} else if ( ( l = starts(s, n, "≧") ) > 0 || ( l = starts(s, n, ">=") ) > 0 ) {
		k->op = MC_OP_GE;
	} else if ( ( l = starts(s, n, "<") ) > 0 ) {
		k->op = MC_OP_LT;
	} else if ( ( l = starts(s, n, ">") ) > 0 ) {
		k->op = MC_OP_GT;
	} else if ( ( l = starts(s, n, "=") ) > 0 ) {
		k->op = MC_OP_EQ;
	} else {
		k->op = MC_OP_EQ;
		l = 0;
	}
	a = s + l;
	an = n - l;
	if ( ( k->op == MC_OP_EQ || k->op == MC_OP_NE ) && !at_is(s, n, "==") && type != MC_T_NUM ) {
		k->wild = has_wild(a, an);
	}
	if ( type == MC_T_NUM && an > 0 && !k->wild ) {
		UB	t[MC_NUM_MAX * 2 + 8];
		INT	tn = mc_num_norm(a, an, t, sizeof(t));

		if ( tn < 0 ) {
			if ( k->op == MC_OP_EQ || k->op == MC_OP_NE ) {
				k->wild = TRUE;	/* matched as letters */
			} else {
				return E_PAR;
			}
		} else {
			k->a = dup_bytes(t, tn);
			k->an = tn;
			return ( k->a != NULL ) ? E_OK : E_NOMEM;
		}
	}
	if ( an > 0 ) {
		k->a = dup_bytes(a, an);
		if ( k->a == NULL ) return E_NOMEM;
		k->an = an;
	}
	return E_OK;
}

/* A condition tested on the value v (vn bytes) of a field of the type */
LOCAL BOOL cond_on( INT type, CONST MCCOND *k, CONST UB *v, INT vn )
{
	INT		r;
	BOOL		eq;

	switch ( k->op ) {
	case MC_OP_RX:
		return mc_rx_find(k->rx, v, vn);
	case MC_OP_EQ:
	case MC_OP_NE:
		if ( k->an == 0 ) eq = (BOOL)( vn == 0 );
		else if ( k->wild ) eq = mc_wild(k->a, k->an, v, vn);
		else if ( type == MC_T_NUM ) eq = (BOOL)( vn > 0 && mc_num_cmp(v, k->a) == 0 );
		else eq = same(v, vn, k->a, k->an);
		return ( k->op == MC_OP_EQ ) ? eq : (BOOL)!eq;
	default:
		break;
	}
	if ( vn == 0 ) return FALSE;
	r = ( type == MC_T_NUM ) ? mc_num_cmp(v, k->a) : mc_text_cmp(v, vn, k->a, k->an);
	switch ( k->op ) {
	case MC_OP_LT:	return (BOOL)( r < 0 );
	case MC_OP_GT:	return (BOOL)( r > 0 );
	case MC_OP_LE:	return (BOOL)( r <= 0 );
	case MC_OP_GE:	return (BOOL)( r >= 0 );
	default:	return FALSE;
	}
}

EXPORT BOOL mc_cond_test( CONST MCDB *db, CONST MCCARD *c, INT f, CONST MCCOND *k )
{
	UB		*name = NULL;
	CONST UB	*v = mc_get(c, f);
	INT		vn = 0;
	INT		type = db->f[f].type;
	BOOL		yes;

	if ( k->op == MC_OP_NONE ) return TRUE;
	if ( v != NULL && type == MC_T_LINK ) {
		/* the name whole: a condition on its end is met as on its start */
		name = mc_link_name_all(v, mc_len(v), &vn);
		v = name;
	} else {
		vn = mc_len(v);
	}
	if ( v == NULL ) {
		v = (CONST UB *)"";
		vn = 0;
	}
	yes = cond_on(type, k, v, vn);
	mc_free(name);
	return yes;
}

EXPORT INT mc_search( MCDB *db, CONST UB *CONST *cond, INT how, INT *p_at )
{
	MCCOND	*k;
	INT	f, i, found = 0;
	ER	er = E_OK;

	*p_at = -1;
	k = (MCCOND *)mc_alloc((INT)sizeof(MCCOND) * ( db->nf > 0 ? db->nf : 1 ));
	if ( k == NULL ) return E_NOMEM;
	m_zero(k, (INT)sizeof(MCCOND) * ( db->nf > 0 ? db->nf : 1 ));
	for ( f = 0; f < db->nf && er >= E_OK; f++ ) {
		er = mc_cond_make(&k[f], db->f[f].type, cond[f], mc_len(cond[f]));
		if ( er < E_OK ) *p_at = f;
	}
	if ( er >= E_OK ) {
		for ( i = 0; i < db->n; i++ ) {
			MCCARD	*c = db->c[i];
			BOOL	ok = (BOOL)( how == MC_S_NEW || !db->searching || c->found );

			for ( f = 0; ok && f < db->nf; f++ ) ok = mc_cond_test(db, c, f, &k[f]);
			c->found = ok;
			if ( ok ) found++;
		}
		db->searching = TRUE;
	}
	for ( f = 0; f < db->nf; f++ ) mc_cond_free(&k[f]);
	mc_free(k);
	return ( er < E_OK ) ? er : found;
}

EXPORT void mc_search_off( MCDB *db )
{
	INT	i;

	db->searching = FALSE;
	for ( i = 0; i < db->n; i++ ) db->c[i]->found = FALSE;
}

EXPORT INT mc_mark_search( MCDB *db, BOOL add )
{
	INT	i, found = 0;

	for ( i = 0; i < db->n; i++ ) {
		MCCARD	*c = db->c[i];

		if ( add ) c->found = (BOOL)( ( db->searching && c->found ) || c->mark );
		else c->found = (BOOL)( c->mark && ( !db->searching || c->found ) );
		if ( c->found ) found++;
	}
	db->searching = TRUE;
	return found;
}

/* ---------------------------------------------------------------- marks */

EXPORT INT mc_marks( MCDB *db, INT how, INT cur )
{
	INT	i, pos = 0, n = 0;

	for ( i = 0; i < db->n; i++ ) {
		MCCARD	*c = db->c[i];

		if ( !mc_in(db, i) ) continue;
		switch ( how ) {
		case MC_M_FLIP_ALL:	c->mark = (BOOL)!c->mark;		break;
		case MC_M_ALL:		c->mark = TRUE;				break;
		case MC_M_NONE:		c->mark = FALSE;			break;
		case MC_M_BEFORE_ON:	if ( pos <= cur ) c->mark = TRUE;	break;
		case MC_M_BEFORE_OFF:	if ( pos <= cur ) c->mark = FALSE;	break;
		case MC_M_AFTER_ON:	if ( pos >= cur ) c->mark = TRUE;	break;
		case MC_M_AFTER_OFF:	if ( pos >= cur ) c->mark = FALSE;	break;
		default:		break;
		}
		if ( c->mark ) n++;
		pos++;
	}
	return n;
}

EXPORT INT mc_mark_next( CONST MCDB *db, INT pos, INT d )
{
	INT	total = mc_count(db), p;

	for ( p = pos + d; p >= 0 && p < total; p += d ) {
		INT	i = mc_index(db, p);

		if ( i >= 0 && db->c[i]->mark ) return p;
	}
	return -1;
}

/* ---------------------------------------------------------------- order */

EXPORT INT mc_text_cmp( CONST UB *a, INT an, CONST UB *b, INT bn )
{
	INT	i = 0, j = 0;
	UINT	pa = 0, pb = 0;

	while ( i < an && j < bn ) {
		UINT	ca, cb, ka, kb;
		INT	la = mc_u8_get(a + i, an - i, &ca), lb = mc_u8_get(b + j, bn - j, &cb);

		if ( la <= 0 || lb <= 0 ) break;
		ka = mc_sort_key(ca, pa);
		kb = mc_sort_key(cb, pb);
		if ( ka != kb ) return ( ka < kb ) ? -1 : 1;
		pa = ( ca == 0x30FC ) ? pa : ca;
		pb = ( cb == 0x30FC ) ? pb : cb;
		i += la;
		j += lb;
	}
	if ( i < an ) return 1;
	if ( j < bn ) return -1;
	/* the same in order: the bytes decide, so that only the same text is equal */
	for ( i = 0; i < an && i < bn; i++ ) {
		if ( a[i] != b[i] ) return ( a[i] < b[i] ) ? -1 : 1;
	}
	return ( an == bn ) ? 0 : ( an < bn ) ? -1 : 1;
}

EXPORT INT mc_val_cmp( CONST MCDB *db, INT f, CONST MCCARD *a, CONST MCCARD *b )
{
	CONST UB	*va = mc_get(a, f), *vb = mc_get(b, f);

	if ( va == NULL || vb == NULL ) {
		if ( va == NULL && vb == NULL ) return 0;
		return ( va == NULL ) ? -1 : 1;
	}
	if ( db->f[f].type == MC_T_NUM ) return mc_num_cmp(va, vb);
	if ( db->f[f].type == MC_T_LINK ) {
		INT	la = 0, lb = 0, r;
		UB	*na = mc_link_name_all(va, mc_len(va), &la);
		UB	*nb = mc_link_name_all(vb, mc_len(vb), &lb);

		/* the names whole: two that differ only after their first letters are told apart */
		r = mc_text_cmp(( na != NULL ) ? na : (CONST UB *)"", la, ( nb != NULL ) ? nb : (CONST UB *)"", lb);
		mc_free(na);
		mc_free(nb);
		return r;
	}
	return mc_text_cmp(va, mc_len(va), vb, mc_len(vb));
}

/* The cards idx[0..n) sorted by field f, the equal ones kept in their order */
LOCAL void merge_sort( CONST MCDB *db, INT f, BOOL desc, INT *idx, INT *tmp, INT n )
{
	INT	w, lo;

	for ( w = 1; w < n; w *= 2 ) {
		for ( lo = 0; lo < n; lo += 2 * w ) {
			INT	mid = ( lo + w < n ) ? lo + w : n;
			INT	hi = ( lo + 2 * w < n ) ? lo + 2 * w : n;
			INT	i = lo, j = mid, k = lo;

			while ( i < mid && j < hi ) {
				INT	r = mc_val_cmp(db, f, db->c[idx[j]], db->c[idx[i]]);

				if ( desc ) r = -r;
				tmp[k++] = ( r < 0 ) ? idx[j++] : idx[i++];
			}
			while ( i < mid ) tmp[k++] = idx[i++];
			while ( j < hi ) tmp[k++] = idx[j++];
		}
		m_copy(idx, tmp, (INT)sizeof(INT) * n);
	}
}

/* The cards worked on, by their indexes; how many */
LOCAL INT taken( CONST MCDB *db, INT **p_idx )
{
	INT	i, n = 0, *idx = (INT *)mc_alloc((INT)sizeof(INT) * ( db->n + 1 ));

	*p_idx = idx;
	if ( idx == NULL ) return E_NOMEM;
	for ( i = 0; i < db->n; i++ ) {
		if ( mc_in(db, i) ) idx[n++] = i;
	}
	return n;
}

EXPORT ER mc_sort( MCDB *db, INT f, BOOL desc )
{
	INT	*idx, *tmp, *place, n, k;
	MCCARD	**was;

	if ( f < 0 || f >= db->nf ) return E_PAR;
	n = taken(db, &idx);
	if ( n < 0 ) return n;
	tmp = (INT *)mc_alloc((INT)sizeof(INT) * ( n + 1 ));
	place = (INT *)mc_alloc((INT)sizeof(INT) * ( n + 1 ));
	was = (MCCARD **)mc_alloc((INT)sizeof(MCCARD *) * ( n + 1 ));
	if ( tmp == NULL || place == NULL || was == NULL ) {
		mc_free(idx);
		mc_free(tmp);
		mc_free(place);
		mc_free(was);
		return E_NOMEM;
	}
	m_copy(place, idx, (INT)sizeof(INT) * n);
	merge_sort(db, f, desc, idx, tmp, n);
	for ( k = 0; k < n; k++ ) was[k] = db->c[idx[k]];
	/* into the places the cards worked on had, in their new order */
	for ( k = 0; k < n; k++ ) db->c[place[k]] = was[k];
	mc_free(idx);
	mc_free(tmp);
	mc_free(place);
	mc_free(was);
	return E_OK;
}

EXPORT INT mc_dups( MCDB *db, INT f )
{
	INT	*idx, *tmp, n, k, found = 0;

	if ( f < 0 || f >= db->nf ) return E_PAR;
	n = taken(db, &idx);
	if ( n < 0 ) return n;
	tmp = (INT *)mc_alloc((INT)sizeof(INT) * ( n + 1 ));
	if ( tmp == NULL ) {
		mc_free(idx);
		return E_NOMEM;
	}
	merge_sort(db, f, FALSE, idx, tmp, n);
	for ( k = 0; k < db->n; k++ ) db->c[k]->found = FALSE;
	for ( k = 0; k < n; ) {
		INT	e = k + 1;

		while ( e < n && mc_val_cmp(db, f, db->c[idx[k]], db->c[idx[e]]) == 0 ) e++;
		if ( e - k >= 2 && mc_get(db->c[idx[k]], f) != NULL ) {
			INT	j;

			for ( j = k; j < e; j++ ) {
				db->c[idx[j]]->found = TRUE;
				found++;
			}
		}
		k = e;
	}
	db->searching = TRUE;
	mc_free(idx);
	mc_free(tmp);
	return found;
}
