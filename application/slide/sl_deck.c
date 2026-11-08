/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sl_deck.c
 *	スライド: what a document of slides says, and how a slide is fitted
 *	to a screen (design 11.11)
 *
 *	The links of the document, in the order they stand, are the slides;
 *	a first paragraph that is "%size:W:H" says the screen they are made
 *	for. A slide is drawn as large as the window lets it be with its
 *	shape kept. Nothing here calls the system or takes memory of its own
 *	but for a moment, so the program of the slides (sl_main.c) and the
 *	kernel's tests (tests/ktest/ktest_slide.c) use the same code.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/tvport.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/rtad.h>
#include <ts/slide.h>

/* The words directly or deeper inside an element, put after what out holds */
LOCAL INT words( CONST T_TADNODE *nd, UB *out, INT n, INT max )
{
	CONST T_TADNODE	*c;
	CONST UB	*t;

	for ( c = nd->first; c != NULL && n < max - 1; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
			for ( t = c->text; *t != 0 && n < max - 1; t++ ) out[n++] = *t;
		} else if ( c->kind == TAD_ND_ELEM ) {
			n = words(c, out, n, max);
		}
	}
	out[n] = 0;
	return n;
}

EXPORT BOOL sl_is_elem( CONST T_TADNODE *nd, CONST char *name )
{
	INT	i;

	if ( nd == NULL || nd->kind != TAD_ND_ELEM || nd->name == NULL ) return FALSE;
	for ( i = 0; name[i] != 0; i++ ) {
		if ( nd->name[i] != (UB)name[i] ) return FALSE;
	}
	return (BOOL)( nd->name[i] == 0 );
}

/* A number at s[*p_i], and *p_i past it; -1 when there is none */
LOCAL INT number( CONST UB *s, INT *p_i )
{
	INT	i = *p_i, v = 0;

	if ( s[i] < '0' || s[i] > '9' ) return -1;
	while ( s[i] >= '0' && s[i] <= '9' && v < 100000 ) v = v * 10 + ( s[i++] - '0' );
	*p_i = i;
	return v;
}

/*
 * "%size:W:H", the whole of a paragraph but the spaces round it: the
 * size the slides are made for. W and H may also stand either side of
 * "*", "x", "＊" or "×".
 */
LOCAL BOOL size_line( CONST UB *s, INT *p_w, INT *p_h )
{
	INT	i = 0, w, h;

	while ( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ) i++;
	if ( s[i] != '%' || s[i + 1] != 's' || s[i + 2] != 'i' || s[i + 3] != 'z' || s[i + 4] != 'e'
	  || s[i + 5] != ':' ) {
		return FALSE;
	}
	i += 6;
	w = number(s, &i);
	if ( w <= 0 ) return FALSE;
	if ( s[i] == ':' || s[i] == '*' || s[i] == 'x' ) {
		i++;
	} else if ( s[i] == 0xEF && s[i + 1] == 0xBC && s[i + 2] == 0x8A ) {
		i += 3;				/* ＊ */
	} else if ( s[i] == 0xC3 && s[i + 1] == 0x97 ) {
		i += 2;				/* × */
	} else {
		return FALSE;
	}
	h = number(s, &i);
	if ( h <= 0 ) return FALSE;
	while ( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ) i++;
	if ( s[i] != 0 ) return FALSE;
	*p_w = w;
	*p_h = h;
	return TRUE;
}

EXPORT INT sl_pages( CONST T_TAD *rec, TS_UUID *ids, INT max, INT *p_w, INT *p_h )
{
	CONST T_TADNODE	*nd;
	T_VOBJ		*v;
	UB		*s;
	INT		n, i, k = 0;

	*p_w = SL_W;
	*p_h = SL_H;
	if ( rec == NULL ) return 0;

	/* the first paragraph: the size, when it says one */
	for ( nd = tad_walk(rec, NULL); nd != NULL && !sl_is_elem(nd, "p"); nd = tad_walk(rec, nd) ) ;
	s = (UB *)Kmalloc(256);
	if ( nd != NULL && s != NULL ) {
		(void)words(nd, s, 0, 256);
		(void)size_line(s, p_w, p_h);
	}
	if ( s != NULL ) Kfree(s);

	/* the links, in the order they stand */
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) return 0;
	n = tad_lnk_count(rec);
	for ( i = 0; i < n && k < max; i++ ) {
		if ( tad_lnk_get(rec, i, v) >= E_OK ) ids[k++] = v->target;
	}
	Kfree(v);
	return k;
}

/*
 * The <realtime> of a record: the first element of that name, looked for
 * eight elements deep as the player looks for it (lib/librtad, rt_find)
 */
LOCAL CONST T_TADNODE *rt_in( CONST T_TADNODE *nd, INT depth )
{
	CONST T_TADNODE	*c, *f;

	if ( nd == NULL || depth > 8 ) return NULL;
	if ( sl_is_elem(nd, "realtime") ) return nd;
	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) continue;
		if ( ( f = rt_in(c, depth + 1) ) != NULL ) return f;
	}
	return NULL;
}

EXPORT BOOL sl_has_rt( CONST T_TAD *rec )
{
	return (BOOL)( rec != NULL && rt_in(tad_root(rec), 0) != NULL );
}

/* The scene a record of 実時間制御TAD is made for: its figure's view, or 1920 by 1080 */
EXPORT void sl_scene_size( CONST T_TAD *rec, INT *p_w, INT *p_h )
{
	CONST T_TADNODE	*rtn = ( rec != NULL ) ? rt_in(tad_root(rec), 0) : NULL, *c, *v;

	*p_w = SL_W;
	*p_h = SL_H;
	for ( c = ( rtn != NULL ) ? rtn->first : NULL; c != NULL; c = c->next ) {
		if ( !sl_is_elem(c, "figure") ) continue;
		for ( v = c->first; v != NULL; v = v->next ) {
			if ( sl_is_elem(v, "figView") ) {
				INT	w = rt_int(tad_attr(v, "right"), 0) - rt_int(tad_attr(v, "left"), 0);
				INT	h = rt_int(tad_attr(v, "bottom"), 0) - rt_int(tad_attr(v, "top"), 0);

				if ( w > 0 && h > 0 ) {
					*p_w = w;
					*p_h = h;
					return;
				}
			}
		}
	}
}

EXPORT BOOL sl_takes( CONST T_TAD *rec )
{
	return (BOOL)( rec != NULL && ( tad_lnk_count(rec) > 0 || sl_has_rt(rec) ) );
}

LOCAL INT gcd( INT a, INT b )
{
	while ( b != 0 ) {
		INT	t = a % b;

		a = b;
		b = t;
	}
	return a;
}

/*
 * The magnification z / *p_den that makes the slide as large as the
 * window lets it be with its shape kept: the narrower way decides and
 * the slide meets the window's edges that way exactly, so that the
 * margin is only along the other way. Not smaller than an eighth, nor
 * larger than SL_ZOOM_MAX times.
 */
EXPORT INT sl_fit( INT ww, INT wh, INT tw, INT th, T_DPRECT *out, INT *p_den )
{
	INT	z, den, w, h, g;

	if ( tw <= 0 ) tw = SL_W;
	if ( th <= 0 ) th = SL_H;
	if ( ww < 0 ) ww = 0;
	if ( wh < 0 ) wh = 0;
	/* ww / tw against wh / th, without dividing */
	if ( (D)ww * th <= (D)wh * tw ) {
		z = ww;
		den = tw;
	} else {
		z = wh;
		den = th;
	}
	if ( z * 8 < den ) {
		z = 1;
		den = 8;
	} else if ( z > den * SL_ZOOM_MAX ) {
		z = SL_ZOOM_MAX;
		den = 1;
	}
	g = gcd(z, den);
	z /= g;
	den /= g;
	w = (INT)( (D)tw * z / den );
	h = (INT)( (D)th * z / den );
	out->left = ( ww - w ) / 2;
	out->top = ( wh - h ) / 2;
	out->right = out->left + w;
	out->bottom = out->top + h;
	*p_den = den;
	return z;
}

/* The part of a record a slide is drawn from, when it is not the body itself (<realtime>) */
LOCAL CONST T_TADNODE *inner( CONST T_TAD *rec, BOOL *p_fig )
{
	CONST T_TADNODE	*b = tad_body(rec), *c;

	for ( c = ( b != NULL ) ? b->first : NULL; c != NULL; c = c->next ) {
		if ( sl_is_elem(c, "figure") || sl_is_elem(c, "document") ) {
			*p_fig = sl_is_elem(c, "figure");
			return c;
		}
	}
	return NULL;
}

EXPORT BOOL sl_model( CONST T_TAD *rec, T_TVDOC **p_d, T_TVFIG **p_f )
{
	CONST T_TADNODE	*nd;
	UINT		kind;
	BOOL		fig = FALSE;

	*p_d = NULL;
	*p_f = NULL;
	if ( rec == NULL ) return FALSE;
	kind = tv_kind(rec);
	if ( kind == TV_KIND_DOC ) {
		(void)tv_doc(rec, p_d);
	} else if ( kind == TV_KIND_FIG ) {
		(void)tv_fig(rec, p_f);
	} else if ( ( nd = inner(rec, &fig) ) != NULL ) {
		if ( fig ) (void)tv_fig_of(nd, p_f);
		else (void)tv_doc_of(nd, p_d);
	}
	return (BOOL)( *p_d != NULL || *p_f != NULL );
}
