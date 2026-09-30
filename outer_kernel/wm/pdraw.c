/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pdraw.c
 *	The parts of panels drawn, and found under the pointer (design 16.5.8)
 *
 *	Each kind is drawn with the measures and in the order BTRON's parts
 *	are drawn: the letters sixteen high on a base line half their height
 *	below the middle, a switch a raised box with a corner of twelve, a
 *	lamp seven eighths of the margin wide and a third of it and eight
 *	high, a push button's name spread out across it, a box's letters a
 *	sixteenth of a letter apart, fields along a line each as wide as its
 *	letters with the field being worked marked by two triangles under
 *	it, a volume a bar with a knob, a scrolling selector a sunken box
 *	with its own bar in the right twenty pixels. The colours and the
 *	patterns are the numbered look table's.
 *
 *	A panel of tags is drawn as the application library's is: a frame
 *	round the sheet, the tags above it on the frame's ground, each tag a
 *	box with its upper corners cut, the tag shown joined to the sheet
 *	under it, and a second row inside the sheet for an upper tag that
 *	has one.
 *
 *	Everything here is found again by the functions that answer where a
 *	point is, from the same arithmetic, so that a press lands on the
 *	piece that was drawn under it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/part.h>
#include <ts/wm.h>
#include <ts/look.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include "pdraw.h"

#define RAD		12		/* the corner of a switch */
#define MARGIN		16		/* before a name: the letters' height, and never less */
#define WHITE		0x00FFFFFFU
#define BLACK		0x00000000U
#define GREY		0x007F7F7FU

#define IS_AT(s)	( (s)[0] == 0xEF && (s)[1] == 0xBC && (s)[2] == 0xA0 )	/* ＠ */
#define IS_PLUS(s)	( (s)[0] == 0xEF && (s)[1] == 0xBC && (s)[2] == 0x8B )	/* ＋ */

/* ---------------------------------------------------------------- pieces */

LOCAL void rc( T_DPRECT *q, INT l, INT t, INT r, INT b )
{
	q->left = l;
	q->top = t;
	q->right = r;
	q->bottom = b;
}

/* A rectangle in a pattern of the table */
LOCAL void pfill( INT gid, INT l, INT t, INT r, INT b, UINT look )
{
	T_DPRECT	q;
	T_DPPAT		pat;

	if ( r <= l || b <= t ) {
		return;
	}
	rc(&q, l, t, r, b);
	wm_look_pat(look, &pat);
	dp_fill_rect_pat(gid, &q, &pat);
}

/* And in a colour */
LOCAL void cfill( INT gid, INT l, INT t, INT r, INT b, UW colour )
{
	T_DPRECT	q;

	if ( r <= l || b <= t ) {
		return;
	}
	rc(&q, l, t, r, b);
	dp_fill_rect(gid, &q, colour);
}

LOCAL UW light( void )
{
	return wm_look(LK_LIGHT);
}

LOCAL UW dark( void )
{
	return wm_look(LK_SHADOW);
}

/*
 * Every other dot of a colour, the rest left as it is: the half-tone a
 * secret box's letters are shown as, an underline that cannot be
 * worked, and the veil over a part that cannot be.
 */
LOCAL CONST UW half_mask[4] = { 0xAAAAAAAAU, 0x55555555U, 0xAAAAAAAAU, 0x55555555U };

LOCAL void half_fill( INT gid, CONST T_DPRECT *q, UW colour, BOOL round )
{
	UW	tile[32 * 4];
	T_DPPAT	pat;
	INT	i;

	if ( q->right <= q->left || q->bottom <= q->top ) {
		return;
	}
	for ( i = 0; i < 32 * 4; i++ ) {
		tile[i] = colour;
	}
	knl_memset(&pat, 0, sizeof(pat));
	pat.kind = DP_PAT_TILE;
	pat.tile = tile;
	pat.hs = 32;
	pat.vs = 4;
	pat.mask = half_mask;
	if ( round ) {
		dp_fill_round(gid, q, RAD, RAD, &pat);
	} else {
		dp_fill_rect_pat(gid, q, &pat);
	}
}

/* A rectangle turned over: drawn twice, it is as it was */
LOCAL void xor_rect( INT gid, CONST T_DPRECT *q )
{
	T_DPENV	env;

	if ( q->right <= q->left || q->bottom <= q->top || dp_ref(gid, &env) < E_OK ) {
		return;
	}
	dp_set_mode(gid, DP_MODE_XOR);
	dp_fill_rect(gid, q, WHITE);
	dp_set_mode(gid, env.mode);
}

/* The rounded frame turned over: a push button while it is held */
LOCAL void xor_round( INT gid, CONST T_DPRECT *q )
{
	T_DPENV	env;
	T_DPPAT	pat;

	if ( dp_ref(gid, &env) < E_OK ) {
		return;
	}
	dp_pat_colour(&pat, WHITE);
	dp_set_mode(gid, DP_MODE_XOR);
	dp_frame_round(gid, q, RAD, RAD, 1, &pat);
	dp_set_mode(gid, env.mode);
}

/* What may be drawn narrowed to a rectangle, and put back */
LOCAL void clip_in( INT gid, CONST T_DPRECT *q, T_DPRECT *was )
{
	T_DPENV		env;
	T_DPRECT	c;

	if ( dp_ref(gid, &env) < E_OK ) {
		rc(was, 0, 0, 0, 0);
		return;
	}
	*was = env.visible;
	c = env.visible;
	if ( c.left < q->left )     c.left = q->left;
	if ( c.top < q->top )       c.top = q->top;
	if ( c.right > q->right )   c.right = q->right;
	if ( c.bottom > q->bottom ) c.bottom = q->bottom;
	if ( c.right < c.left )     c.right = c.left;
	if ( c.bottom < c.top )     c.bottom = c.top;
	dp_set_visible(gid, &c);
}

LOCAL void clip_back( INT gid, CONST T_DPRECT *was )
{
	if ( was->right > was->left || was->bottom > was->top ) {
		dp_set_visible(gid, was);
	}
}

/*
 * A rounded box, raised: filled, framed in the dark line, then framed
 * again in the light line with what may be drawn cut a third of the
 * corner short of the right and the foot -- the light falls on the head
 * and the left only.
 */
LOCAL void rrc_raised( INT gid, CONST T_DPRECT *q, UINT ground )
{
	T_DPPAT		pat;
	T_DPRECT	was, cut;

	if ( q->right - q->left < 2 || q->bottom - q->top < 2 ) {
		return;
	}
	wm_look_pat(ground, &pat);
	dp_fill_round(gid, q, RAD, RAD, &pat);
	wm_look_pat(LK_SHADOW, &pat);
	dp_frame_round(gid, q, RAD, RAD, 1, &pat);
	rc(&cut, q->left - 1, q->top - 1, q->right - RAD / 3, q->bottom - RAD / 3);
	clip_in(gid, &cut, &was);
	wm_look_pat(LK_LIGHT, &pat);
	dp_frame_round(gid, q, RAD, RAD, 1, &pat);
	clip_back(gid, &was);
}

/* ---------------------------------------------------------------- letters */

/*
 * The face parts are written in, set to their size. Everything drawn
 * here holds the font layer (pd_part), so the size stays set until the
 * part is done.
 */
LOCAL ID face( T_FNMET *met )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		(void)fn_set_size(fid, PD_FH);
	}
	if ( met != NULL ) {
		if ( fid <= 0 || fn_metrics(fid, met) < E_OK ) {
			met->ascent = PD_FH - 3;
			met->descent = 3;
		}
	}
	return fid;
}

/* How many bytes the letter at s takes */
LOCAL INT u8len( CONST UB *s )
{
	INT	n = 1;

	while ( n < 6 && ( s[n] & 0xC0 ) == 0x80 ) {
		n++;
	}
	return n;
}

LOCAL INT s_len( CONST UB *s, INT max )
{
	INT	n = 0;

	while ( s != NULL && n < max && s[n] != 0 ) {
		n++;
	}
	return n;
}

/* How many letters are in n bytes */
LOCAL INT letters( CONST UB *s, INT n )
{
	INT	i = 0, k = 0;

	while ( i < n && s[i] != 0 ) {
		i += u8len(s + i);
		k++;
	}
	return k;
}

/*
 * n bytes of letters from x on the base line y, a letter at a time with
 * 'gap' between them, or all at once when there is none; how far they
 * reach. With gid below nought they are only measured.
 */
LOCAL INT text( INT gid, INT x, INT y, CONST UB *s, INT n, INT gap, UW colour )
{
	ID	fid = face(NULL);
	UB	buf[WM_LABEL_MAX + 8];
	INT	i = 0, x0 = x;

	if ( fid <= 0 || s == NULL ) {
		return 0;
	}
	if ( n < 0 ) {
		n = s_len(s, WM_POOL_BYTES);
	}
	if ( gap == 0 && n < (INT)sizeof(buf) ) {
		knl_memcpy(buf, s, n);
		buf[n] = 0;
		if ( gid >= 0 && n > 0 ) {
			fn_draw(gid, fid, x, y, buf, colour);
		}
		return ( n > 0 ) ? fn_width(fid, buf) : 0;
	}
	while ( i < n && s[i] != 0 ) {
		INT	k = u8len(s + i);

		knl_memcpy(buf, s + i, k);
		buf[k] = 0;
		if ( gid >= 0 ) {
			fn_draw(gid, fid, x, y, buf, colour);
		}
		x += fn_width(fid, buf) + gap;
		i += k;
	}
	return x - x0;
}

LOCAL INT width( CONST UB *s, INT n, INT gap )
{
	return text(-1, 0, 0, s, n, gap, 0);
}

EXPORT CONST UB *pd_pool_name( CONST UB *pool, INT at, INT i )
{
	INT	k = 0;

	if ( pool == NULL || at < 0 || at >= WM_POOL_BYTES || i < 0 ) {
		return NULL;
	}
	while ( k < i ) {
		while ( at < WM_POOL_BYTES && pool[at] != 0 ) {
			at++;
		}
		at++;
		k++;
		if ( at >= WM_POOL_BYTES ) {
			return NULL;
		}
	}
	return &pool[at];
}

/* ---------------------------------------------------------------- switches */

/*
 * The lamp before a name: seven eighths of the margin wide and a third
 * of the margin and eight high, in the middle of the name's height; the
 * place filled with the ground first. A lamp that is neither on nor off
 * (sel below nought) leaves the ground; the rest is filled and edged in
 * the table's lamp patterns, the edge one wide while it can be worked
 * and is off and two otherwise -- and two, and not a pixel in, while it
 * is held.
 */
LOCAL void lamp( INT gid, INT l, INT t, INT b, INT sel, BOOL off, BOOL held, UINT ground )
{
	INT		mw = 7 * MARGIN / 8, mh = ( MARGIN + 8 ) / 3, w;
	T_DPRECT	q;
	T_DPPAT		pat;

	rc(&q, l + ( MARGIN - mw ) / 2, t + ( ( b - t ) - mh ) / 2, 0, 0);
	q.right = q.left + mw;
	q.bottom = q.top + mh;
	pfill(gid, q.left, q.top, q.right, q.bottom, ground);
	if ( sel < 0 ) {
		return;
	}
	if ( !off && !held ) {
		q.left++;  q.top++;  q.right--;  q.bottom--;
	}
	w = ( !off && !held && sel == 0 ) ? 1 : 2;
	pfill(gid, q.left, q.top, q.right, q.bottom, sel ? LK_LAMPON_IN : LK_LAMPOFF_IN);
	wm_look_pat(sel ? LK_LAMPON_LT : LK_LAMPOFF_LT, &pat);
	dp_frame_rect_pat(gid, &q, &pat, w);
}

/*
 * A name in its cell: after its lamp and the margin, or -- with xoff
 * below nought, a push button's -- spread over the cell with the room
 * beside its letters shared into equal gaps before, between and after
 * them. The base line is half the letters' height below the middle,
 * less one. The letters are drawn twice when their colour is not the
 * light one: a pixel down and to the right in the light colour, then
 * in their own. A name in use now is underlined two deep, in half-tone
 * when it cannot be worked. Nothing is drawn outside the part.
 */
LOCAL void name_in( INT gid, CONST T_DPRECT *part, CONST T_DPRECT *cell, CONST UB *name,
		    INT sel, BOOL off, BOOL held, INT xoff, BOOL now, UINT ground )
{
	INT		y = ( cell->top + cell->bottom + PD_FH ) / 2 - 1;
	INT		x = cell->left + xoff + MARGIN, gap = 0, n = s_len(name, WM_LABEL_MAX);
	UW		fg = wm_look(off ? LK_INACTPARTSCOL : LK_ACTPARTSCOL);
	UW		lt = wm_look(LK_LIGHTPARTSCOL);
	T_DPRECT	was;

	if ( xoff < 0 ) {
		INT	k = letters(name, n);

		gap = ( cell->right - cell->left - width(name, n, 0) ) / ( k + 1 );
		if ( gap < 0 ) {
			gap = 0;
		}
		x = cell->left + gap;
	} else {
		lamp(gid, cell->left, cell->top, cell->bottom, sel, off, held, ground);
	}
	if ( n <= 0 ) {
		return;
	}
	clip_in(gid, part, &was);
	if ( xoff >= 0 ) {
		/* a tab: what follows it stands against the cell's right end (a version, a size) */
		INT	t;

		for ( t = 0; t < n && name[t] != 0x09; t++ ) ;
		if ( t < n ) {
			INT	rn = n - t - 1, rx = cell->right - MARGIN - width(name + t + 1, rn, 0);

			if ( fg != lt ) {
				(void)text(gid, rx + 1, y + 1, name + t + 1, rn, 0, lt);
			}
			(void)text(gid, rx, y, name + t + 1, rn, 0, fg);
			n = t;
		}
	}
	if ( fg != lt ) {
		(void)text(gid, x + 1, y + 1, name, n, gap, lt);
	}
	(void)text(gid, x, y, name, n, gap, fg);
	if ( now ) {
		T_DPRECT	u;

		rc(&u, x, y, x + width(name, n, gap), y + 2);
		if ( off ) {
			half_fill(gid, &u, BLACK, FALSE);
		} else {
			dp_fill_rect(gid, &u, BLACK);
		}
	}
	clip_back(gid, &was);
}

/*
 * The cell of a selector's name: down one column, across one row when
 * the selector lies sideways, or down two columns when it asks; each
 * an equal share of the part less eight, four in from the head and the
 * left.
 */
LOCAL void ws_cell( CONST T_WMPART *pt, CONST T_DPRECT *r, INT n, INT i, T_DPRECT *out )
{
	INT	cols, rows, cw, ch;

	if ( n <= 0 ) {
		n = 1;
	}
	if ( pt->type & P_DOUBLE ) {
		cols = 2;
		rows = ( n + 1 ) / 2;
	} else if ( pt->type & P_HALIGN ) {
		cols = n;
		rows = 1;
	} else {
		cols = 1;
		rows = n;
	}
	if ( rows <= 0 ) {
		rows = 1;
	}
	cw = ( r->right - r->left - 8 ) / cols;
	ch = ( r->bottom - r->top - 8 ) / rows;
	out->left = r->left + 4 + ( i / rows ) * cw;
	out->top = r->top + 4 + ( i % rows ) * ch;
	out->right = out->left + cw;
	out->bottom = out->top + ch;
}

EXPORT INT pd_ws_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y )
{
	T_DPRECT	c;
	INT		i;

	for ( i = 0; i < pt->count; i++ ) {
		ws_cell(pt, r, pt->count, i, &c);
		if ( x >= c.left && x < c.right && y >= c.top && y < c.bottom ) {
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- bars */

/*
 * Where the knob of a bar lies along a rail of len pixels, from the two
 * numbers it covers and the two the bar runs between. The knob is never
 * shorter than a third of the rail, up to sixteen; the rail is
 * shortened by what that takes and what is left shared in proportion,
 * the shortening given back to the end that is not the end of the
 * whole, or half to each. Answered: how far the knob starts from the
 * low end (e) and ends from the high (s), and its middle from its start.
 */
LOCAL void knob_place( BOOL small, D lo, D hi, D clo, D chi, D len, INT *e, INT *s, INT *mid )
{
	D	span = chi - clo, range = hi - lo, m, p = 0, q = 0, L = len, a = clo - lo, b = hi - chi;

	m = L / 3;
	if ( m > 16 ) {
		m = 16;
	}
	if ( small ) {
		m = 2;
	} else if ( ( range + 2 >= 0 && range + 2 <= 4 ) || span == range ) {
		p = 0;
	} else {
		if ( range > 0 ) {
			p = ( range * m - 2 * L ) / ( range - 2 );
		} else {
			p = ( 2 * L + range * m ) / ( range + 2 );
		}
		if ( p < 0 ) {
			p = 0;
		} else {
			L -= p;
		}
	}
	if ( span != range ) {
		q = ( range * m - span * L ) / ( range - span );
		if ( q < 0 ) {
			q = 0;
		} else {
			L -= q;
		}
	}
	if ( range != 0 ) {
		*s = (INT)( L * b / range );
		*e = (INT)( L * a / range );
		if ( a == 0 ) {
			*s += (INT)p;
		} else if ( b == 0 ) {
			*e += (INT)p;
		} else {
			*s += (INT)( p / 2 );
			*e += (INT)( p / 2 );
		}
	} else {
		*e = *s = 0;
	}
	*mid = (INT)( ( p + L + q - *s - *e ) / 2 );
}

/*
 * A volume: unless it has no frame, a line of black round it and the
 * rest a pixel inside; the box sunken, the rail in it in the volume's
 * back pattern, the knob in its own a pixel in, a light and a dark line
 * at each end of the knob and along its sides, and in its middle the
 * mark three wide between a dark line and a light one. One that lies
 * across is laid from the right, as a bar across the foot is; it runs
 * up and down otherwise. An emphasised one has its two triangles in the
 * knob, and one that has the keys takes a working part's ground for its
 * rail.
 */
LOCAL void vl_pieces( CONST T_WMPART *pt, CONST T_DPRECT *r, T_DPRECT *pc )
{
	BOOL	across = ( pt->type & P_HALIGN ) != 0;
	INT	len = across ? r->right - r->left : r->bottom - r->top;
	INT	e, s, mid, i;

	knob_place(FALSE, pt->lo, pt->hi, pt->value, pt->value, len, &e, &s, &mid);
	for ( i = 0; i < 4; i++ ) {
		pc[i] = *r;
	}
	if ( across ) {
		pc[0].left = pc[1].right = r->right - e;
		pc[1].left = pc[2].right = r->left + s;
		pc[3].left = r->left + s + mid - 1;
		pc[3].right = r->left + s + mid + 2;
	} else {
		pc[0].bottom = pc[1].top = r->top + e;
		pc[1].bottom = pc[2].top = r->bottom - s;
		pc[3].top = pc[1].top + mid - 1;
		pc[3].bottom = pc[1].top + mid + 2;
	}
}

LOCAL void tri( INT gid, UINT look, INT x0, INT y0, INT x1, INT y1, INT x2, INT y2 )
{
	T_DPPOINT	p[3];
	T_DPPAT		pat;

	p[0].x = x0;  p[0].y = y0;
	p[1].x = x1;  p[1].y = y1;
	p[2].x = x2;  p[2].y = y2;
	wm_look_pat(look, &pat);
	dp_fill_poly(gid, p, 3, DP_POLY_ODD, &pat);
}

LOCAL void knob_marks( INT gid, INT kl, INT kt, INT kr, INT kb, BOOL across )
{
	INT	d;

	if ( across ) {
		d = kb - kt;
		tri(gid, LK_PEMPHAS, kl + d / 8, kt + d / 2, kl + 3 * d / 8, kt + d / 4,
		    kl + 3 * d / 8, kt + 3 * d / 4);
		tri(gid, LK_PEMPHAS, kr - d / 8, kt + d / 2, kr - 3 * d / 8, kt + d / 4,
		    kr - 3 * d / 8, kt + 3 * d / 4);
	} else {
		d = kr - kl;
		tri(gid, LK_PEMPHAS, kl + d / 2, kt + d / 8, kl + d / 4, kt + 3 * d / 8,
		    kl + 3 * d / 4, kt + 3 * d / 8);
		tri(gid, LK_PEMPHAS, kl + d / 2, kb - d / 8, kl + d / 4, kb - 3 * d / 8,
		    kl + 3 * d / 4, kb - 3 * d / 8);
	}
}

LOCAL void draw_volume( T_WMPART *pt, CONST T_DPRECT *r0, T_PDCTX *c )
{
	INT		gid = c->gid;
	T_DPRECT	r = *r0, pc[4], ki, pa, pb;
	BOOL		across = ( pt->type & P_HALIGN ) != 0;
	BOOL		inact = ( pt->type & ( P_INACT | P_DISABLE ) ) == P_INACT;
	UINT		knob = inact ? LK_INACTPARTS : LK_VOLKNOB;
	UINT		back = inact ? LK_INACTPARTS
			     : ( ( pt->type & P_EMPHAS ) || c->focus ) ? LK_ACTPARTS : LK_VOLBACK;
	INT		kl, kt, kr, kb;

	if ( !( pt->type & P_NOFRAME ) ) {
		dp_frame_rect(gid, &r, BLACK, 1);
		r.left++;  r.top++;  r.right--;  r.bottom--;
	}
	vl_pieces(pt, &r, pc);
	rc(&ki, pc[1].left + 1, pc[1].top + 1, pc[1].right - 1, pc[1].bottom - 1);
	/* the box sunken: dark along its head and its left */
	cfill(gid, r.left, r.top, r.right, r.top + 1, dark());
	cfill(gid, r.left, r.top + 1, r.left + 1, r.bottom, dark());
	cfill(gid, r.left + 1, r.bottom - 1, r.right, r.bottom, light());
	cfill(gid, r.right - 1, r.top + 1, r.right, r.bottom - 1, light());
	pa = r;
	pa.left++;  pa.top++;  pa.right--;  pa.bottom--;
	pb = pa;
	if ( across ) {
		pa.right = ki.left;
		pb.left = ki.right;
	} else {
		pa.bottom = ki.top;
		pb.top = ki.bottom;
	}
	pfill(gid, pa.left, pa.top, pa.right, pa.bottom, back);
	pfill(gid, pb.left, pb.top, pb.right, pb.bottom, back);
	pfill(gid, ki.left, ki.top, ki.right, ki.bottom, knob);
	kl = pc[1].left;  kt = pc[1].top;  kr = pc[1].right;  kb = pc[1].bottom;
	if ( across ) {
		dp_line(gid, kl, kt, kl, kb - 1, light());
		dp_line(gid, kl - 1, kt, kl - 1, kb - 1, dark());
		dp_line(gid, kr - 1, kt, kr - 1, kb - 1, dark());
		dp_line(gid, kr, kt, kr, kb - 1, dark());
		dp_line(gid, kl + 1, kt, kr - 2, kt, light());
		dp_line(gid, kl + 1, kb - 1, kr - 2, kb - 1, dark());
	} else {
		dp_line(gid, kl, kt, kr - 1, kt, light());
		dp_line(gid, kl, kt - 1, kr - 1, kt - 1, dark());
		dp_line(gid, kl, kb - 1, kr - 1, kb - 1, dark());
		dp_line(gid, kl, kb, kr - 1, kb, dark());
		dp_line(gid, kl, kt + 1, kl, kb - 2, light());
		dp_line(gid, kr - 1, kt + 1, kr - 1, kb - 2, dark());
	}
	if ( !( pt->type & ( P_DISABLE | P_INACT ) ) ) {
		pfill(gid, pc[3].left, pc[3].top, pc[3].right, pc[3].bottom, LK_VOLTOMBO);
		if ( across ) {
			dp_line(gid, pc[3].left - 1, pc[3].top, pc[3].left - 1, pc[3].bottom - 1, dark());
			dp_line(gid, pc[3].right, pc[3].top, pc[3].right, pc[3].bottom - 1, light());
		} else {
			dp_line(gid, pc[3].left, pc[3].top - 1, pc[3].right - 1, pc[3].top - 1, dark());
			dp_line(gid, pc[3].left, pc[3].bottom, pc[3].right - 1, pc[3].bottom, light());
		}
		if ( pt->type & P_EMPHAS ) {
			knob_marks(gid, kl, kt, kr, kb, across);
		}
	}
}

EXPORT INT pd_vl_at( CONST T_WMPART *pt, CONST T_DPRECT *r0, INT x, INT y )
{
	T_DPRECT	r = *r0, pc[4];
	BOOL		across = ( pt->type & P_HALIGN ) != 0;
	INT		len, klen, at, v, lo = pt->lo, hi = pt->hi;

	if ( !( pt->type & P_NOFRAME ) ) {
		r.left++;  r.top++;  r.right--;  r.bottom--;
	}
	vl_pieces(pt, &r, pc);
	if ( across ) {
		len = r.right - r.left;
		klen = pc[1].right - pc[1].left;
		at = ( r.right - 1 ) - x;		/* laid from the right */
	} else {
		len = r.bottom - r.top;
		klen = pc[1].bottom - pc[1].top;
		at = y - r.top;
	}
	at -= klen / 2;
	len -= klen;
	if ( len <= 0 ) {
		return pt->value;
	}
	if ( at < 0 ) at = 0;
	if ( at > len ) at = len;
	v = lo + (INT)( ( (D)at * ( hi - lo ) + ( ( hi >= lo ) ? len / 2 : -len / 2 ) ) / len );
	if ( lo <= hi ) {
		if ( v < lo ) v = lo;
		if ( v > hi ) v = hi;
	} else {
		if ( v > lo ) v = lo;
		if ( v < hi ) v = hi;
	}
	return v;
}

/*
 * The bar a scrolling selector carries in its right twenty pixels: a
 * black frame round them, and in it a rail eighteen wide, sunken, with
 * the knob raised on it over the rows shown -- a light row at its head,
 * two dark rows at its foot, the dark ones the whole twenty wide -- and
 * the knob's mark in its middle while the selector can be worked.
 */
LOCAL void draw_ss_bar( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r0, INT rows )
{
	INT	r = r0->right, t = r0->top, b = r0->bottom;
	INT	x0 = r - 20, xl = r - 19, xa = r - 18, xb = r - 3, xr = r - 2, x1 = r - 1;
	INT	L = ( b - 1 ) - ( t + 1 ), e, s, mid, kt, kb, cm;
	INT	n = ( pt->count > 0 ) ? pt->count : 1, first = ( pt->top > 0 ) ? pt->top : 1;
	INT	chi = first + rows - 1;
	BOOL	flat = ( pt->type & ( P_INACT | P_DISABLE ) ) != 0;

	if ( L <= 0 ) {
		return;
	}
	if ( chi > n ) {
		chi = n;
	}
	knob_place(FALSE, 1, n, first, chi, L, &e, &s, &mid);
	kt = t + 1 + e;
	kb = b - 1 - s;
	cm = kt + mid;
	/* the frame */
	cfill(gid, x0, t, x0 + 1, b, BLACK);
	cfill(gid, x1, t, x1 + 1, b, BLACK);
	cfill(gid, x0, t, x1 + 1, t + 1, BLACK);
	cfill(gid, x0, b - 1, x1 + 1, b, BLACK);
	/* the rail, sunken; its dark head the whole twenty */
	pfill(gid, xa, t + 1, xb + 1, b - 1,
	      ( ( pt->type & ( P_EMPHAS | P_DISABLE | P_INACT ) ) == P_EMPHAS ) ? LK_ACTPARTS : LK_SBARBACK);
	cfill(gid, xl, t + 1, xl + 1, b - 1, dark());
	cfill(gid, xr, t + 1, xr + 1, b - 1, light());
	cfill(gid, x0, t + 1, x1 + 1, t + 2, dark());
	cfill(gid, xa, b - 2, xr + 1, b - 1, light());
	/* the knob */
	if ( kb > kt ) {
		pfill(gid, xa, kt + 1, xb + 1, kb - 1, LK_SBARKNOB);
		cfill(gid, xl, kt, xl + 1, kb - 1, light());
		cfill(gid, xr, kt + 1, xr + 1, kb - 1, dark());
		cfill(gid, xl, kt, xr + 1, kt + 1, light());
		cfill(gid, x0, kb - 1, x1 + 1, kb + 1, dark());
		if ( kt - 1 > t ) {
			cfill(gid, x0, kt - 1, x1 + 1, kt, dark());
		}
		if ( !flat && cm - 2 > kt && cm + 2 < kb - 1 ) {
			cfill(gid, x0, cm - 2, x1 + 1, cm - 1, dark());
			pfill(gid, xl, cm - 1, xr + 1, cm + 2, LK_SBARTOMBO);
			cfill(gid, xl, cm + 2, xr + 1, cm + 3, light());
		}
	}
}

/* ---------------------------------------------------------------- scrolling selectors */

LOCAL INT ss_row_h( void )
{
	return MARGIN + 1;
}

EXPORT INT pd_ss_rows( CONST T_WMPART *pt, CONST T_DPRECT *r )
{
	INT	rows = ( r->bottom - r->top - 8 ) / ss_row_h();

	if ( rows > pt->count ) {
		rows = pt->count;
	}
	return ( rows > 0 ) ? rows : 1;
}

LOCAL void ss_cell( CONST T_DPRECT *r, INT i, T_DPRECT *out )
{
	out->left = r->left + 1;
	out->top = r->top + 4 + i * ss_row_h();
	out->right = r->right - 21;
	out->bottom = out->top + ss_row_h();
}

EXPORT INT pd_ss_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y )
{
	INT		rows = pd_ss_rows(pt, r), i;
	INT		n = ( pt->count > 0 ) ? pt->count : 1;
	INT		first = ( pt->top > 0 ) ? pt->top : 1, chi = first + rows - 1;
	INT		L = r->bottom - r->top - 2, e, s, mid;
	T_DPRECT	c;

	if ( x >= r->right - PD_SS_BAR ) {
		if ( chi > n ) chi = n;
		knob_place(FALSE, 1, n, first, chi, L, &e, &s, &mid);
		if ( y < r->top + 1 + e ) return -1;
		if ( y >= r->bottom - 1 - s ) return -3;
		return -2;
	}
	for ( i = 0; i < rows; i++ ) {
		ss_cell(r, i, &c);
		if ( y >= c.top && y < c.bottom ) {
			return ( first + i <= pt->count ) ? first + i : 0;
		}
	}
	return 0;
}

EXPORT INT pd_ss_top_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT y )
{
	INT	rows = pd_ss_rows(pt, r), n = pt->count;
	INT	L = r->bottom - r->top - 2, e, s, mid, klen, at, top;

	if ( n <= rows ) {
		return 1;
	}
	knob_place(FALSE, 1, n, 1, rows, L, &e, &s, &mid);
	klen = L - e - s;
	at = y - ( r->top + 1 ) - klen / 2;
	if ( L - klen <= 0 ) {
		return 1;
	}
	top = 1 + (INT)( ( (D)at * ( n - rows ) + ( L - klen ) / 2 ) / ( L - klen ) );
	if ( top < 1 ) top = 1;
	if ( top > n - rows + 1 ) top = n - rows + 1;
	return top;
}

LOCAL void draw_scroll( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	INT		gid = c->gid, rows = pd_ss_rows(pt, r), i;
	UINT		ground = ( pt->type & P_EMPHAS ) ? LK_ACTPARTS : LK_INACTPARTS;
	BOOL		off = ( pt->type & ( P_DISABLE | P_INACT ) ) != 0;
	T_DPRECT	cell;

	pt->rows = rows;
	if ( pt->top < 1 ) {
		pt->top = 1;
	}
	if ( pt->top > pt->count - rows + 1 ) {
		pt->top = ( pt->count - rows + 1 > 1 ) ? pt->count - rows + 1 : 1;
	}
	draw_ss_bar(gid, pt, r, rows);
	pfill(gid, r->left, r->top, r->right - PD_SS_BAR, r->bottom, ground);
	if ( !( pt->type & P_NOFRAME ) ) {
		cfill(gid, r->left, r->top, r->right, r->top + 1, dark());
		cfill(gid, r->left, r->top, r->left + 1, r->bottom, dark());
		cfill(gid, r->left + 1, r->bottom - 1, r->right - PD_SS_BAR, r->bottom, light());
		cfill(gid, r->right - 21, r->top + 1, r->right - PD_SS_BAR, r->bottom, light());
	}
	for ( i = 0; i < rows && pt->top + i <= pt->count; i++ ) {
		INT		idx = pt->top + i;
		CONST UB	*nm = pd_pool_name(c->pool, pt->pool, idx - 1);
		BOOL		noff = off || ( idx <= 32 && ( pt->inact & ( 1U << ( idx - 1 ) ) ) );

		if ( nm == NULL ) {
			break;
		}
		ss_cell(r, i, &cell);
		name_in(gid, r, &cell, nm, ( pt->value == idx ) ? 1 : -1, noff,
			(BOOL)( c->held_at == idx ), 0, (BOOL)( pt->now == idx ), ground);
		if ( c->focus && pt->value == idx && !off ) {
			T_DPRECT	f = cell;

			f.left += MARGIN - 1;
			dp_frame_rect(gid, &f, wm_look(LK_ACTPARTSCOL), 1);
		}
	}
}

/* ---------------------------------------------------------------- boxes */

/* The frame of a box: sunken by a line; a box that cannot be worked keeps two at its foot */
LOCAL void box_frame( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r )
{
	INT	l = r->left, t = r->top, rr = r->right, b = r->bottom;

	if ( pt->type & P_NOFRAME ) {
		return;
	}
	if ( pt->type & P_DISABLE ) {
		INT	wd = ( pt->type & P_EMPHAS ) ? 2 : 1;

		cfill(gid, l, b - wd, rr - wd, b, light());
		cfill(gid, l, b - 2 * wd, rr - wd, b - wd, dark());
		return;
	}
	cfill(gid, l, t, rr, t + 1, dark());
	cfill(gid, l, t, l + 1, b, dark());
	cfill(gid, l + 1, b - 1, rr, b, light());
	cfill(gid, rr - 1, t + 1, rr, b, light());
}

/* What the inside of a box's frame sees */
LOCAL void box_inside( CONST T_WMPART *pt, CONST T_DPRECT *r, T_DPRECT *in )
{
	INT	f = ( pt->type & P_NOFRAME ) ? 0 : ( pt->type & P_EMPHAS ) ? 3 : 1;

	rc(in, r->left + f, r->top, r->right - f, r->bottom);
	if ( pt->type & P_DISABLE ) {
		in->bottom -= 2 * f;
	} else {
		in->top += f;
		in->bottom -= f;
	}
}

/* A box's letters: what it holds, or for a number box its number */
LOCAL INT num_text( INT v, UB *buf, INT max )
{
	UB	tmp[12];
	INT	n = 0, i, neg = ( v < 0 );
	UW	u = (UW)( neg ? -v : v );

	do {
		tmp[n++] = (UB)( '0' + ( u % 10 ) );
		u /= 10;
	} while ( u > 0 && n < 11 );
	if ( neg && n < 11 ) {
		tmp[n++] = '-';
	}
	if ( n > max - 1 ) {
		n = max - 1;
	}
	for ( i = 0; i < n; i++ ) {
		buf[i] = tmp[n - 1 - i];
	}
	buf[n] = 0;
	return n;
}

/* Where a box's letters start, and their base line */
LOCAL INT tb_x( CONST T_WMPART *pt, CONST T_DPRECT *r, CONST UB *s, INT n )
{
	if ( WM_PT_KIND(pt->type) == NB_PARTS ) {
		INT	w = width(s, n, PD_GAP), room = ( r->right - r->left ) - 6;

		return ( r->right - 5 ) - ( ( w < room ) ? w : room );
	}
	return r->left + 3;
}

LOCAL INT tb_y( CONST T_DPRECT *r )
{
	return ( r->top + r->bottom + PD_FH ) / 2 - 1;
}

/* The width of the first n bytes of a box's letters: a secret box shows a block for each */
LOCAL INT tb_width( CONST T_WMPART *pt, CONST UB *s, INT n )
{
	if ( WM_PT_KIND(pt->type) == XB_PARTS ) {
		return letters(s, n) * ( PD_FH + PD_GAP );
	}
	return width(s, n, PD_GAP);
}

EXPORT INT pd_tb_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x )
{
	INT	n = s_len(pt->text, WM_LABEL_MAX), i = 0, best = 0, bd = 0x7FFFFFFF;
	INT	x0 = tb_x(pt, r, pt->text, n);

	for ( ;; ) {
		INT	d = x - ( x0 + tb_width(pt, pt->text, i) );

		if ( d < 0 ) d = -d;
		if ( d <= bd ) {
			bd = d;
			best = i;
		}
		if ( i >= n ) {
			break;
		}
		i += u8len(pt->text + i);
	}
	return best;
}

/*
 * A text box, a secret one or a number box: its ground the white of a
 * part being worked while it has the keys and the pale grey otherwise,
 * its frame sunken; the letters a sixteenth apart three in from the
 * left, a number ending five short of the right, a secret box a
 * half-tone block for each letter. While a conversion is under way its
 * letters stand at the caret: framed clause by clause while they are
 * read, and once converted the whole run framed with the clause worked
 * on turned over. The letters chosen are turned over from a line above
 * them to two below; the caret is a line of the letters' height.
 */
LOCAL void draw_box( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	INT		gid = c->gid, kind = WM_PT_KIND(pt->type);
	BOOL		off = ( pt->type & ( P_DISABLE | P_INACT ) ) != 0;
	UW		fg = wm_look(off ? LK_INACTPARTSCOL : LK_ACTPARTSCOL);
	UB		numb[16];
	CONST UB	*s = pt->text;
	INT		n, x, y = tb_y(r), cx, caret;
	T_DPRECT	in, was;

	if ( kind == NB_PARTS ) {
		n = num_text(pt->value, numb, sizeof(numb));
		s = numb;
	} else {
		n = s_len(pt->text, WM_LABEL_MAX);
	}
	pfill(gid, r->left, r->top, r->right, r->bottom,
	      ( c->focus && !( pt->type & P_INACT ) ) ? LK_ACTPARTS : LK_INACTPARTS);
	box_frame(gid, pt, r);
	box_inside(pt, r, &in);
	clip_in(gid, &in, &was);
	x = tb_x(pt, r, s, n);
	caret = ( kind == NB_PARTS ) ? n : pt->caret;
	if ( caret > n ) caret = n;
	if ( caret < 0 ) caret = n;

	if ( kind == XB_PARTS ) {
		INT	k, m = letters(s, n), step = PD_FH + PD_GAP;

		for ( k = 0; k < m; k++ ) {
			T_DPRECT	q;

			rc(&q, x + k * step + 1, y - PD_FH + 2, x + k * step + PD_FH - 1, y);
			half_fill(gid, &q, fg, FALSE);
		}
	} else if ( c->conv != NULL && c->conv->ncl > 0 ) {
		T_PDCONV	*cv = c->conv;
		INT		i, x0, x1 = 0, x2 = 0;

		if ( caret > 0 ) {
			x += text(gid, x, y, s, caret, PD_GAP, fg) ;
		}
		x0 = x;
		for ( i = 0; i < cv->ncl; i++ ) {
			INT	a = cv->cl[i], b = cv->cl[i + 1], w;

			if ( b <= a ) continue;
			if ( i == cv->clause ) x1 = x;
			w = text(gid, x, y, cv->text + a, b - a, PD_GAP, fg);
			if ( cv->clause < 0 ) {
				/* being read: each clause in a frame of its own */
				cfill(gid, x - 1, y - PD_FH, x + w, y - PD_FH + 1, dark());
				cfill(gid, x - 1, y + 1, x + w, y + 2, dark());
				cfill(gid, x - 1, y - PD_FH, x, y + 2, dark());
				cfill(gid, x + w - 1, y - PD_FH, x + w, y + 2, dark());
			}
			x += w;
			if ( i == cv->clause ) x2 = x;
		}
		if ( cv->clause >= 0 ) {
			T_DPRECT	q;

			cfill(gid, x0 - 1, y - PD_FH, x, y - PD_FH + 1, dark());
			cfill(gid, x0 - 1, y + 1, x, y + 2, dark());
			cfill(gid, x0 - 1, y - PD_FH, x0, y + 2, dark());
			cfill(gid, x - 1, y - PD_FH, x, y + 2, dark());
			rc(&q, x1, y - PD_FH + 1, x2 - 1, y + 1);
			xor_rect(gid, &q);
		}
		if ( n > caret ) {
			(void)text(gid, x, y, s + caret, n - caret, PD_GAP, fg);
		}
	} else if ( n > 0 ) {
		(void)text(gid, x, y, s, n, PD_GAP, fg);
	}

	/* the letters chosen, and the caret */
	if ( c->focus && !off && ( c->conv == NULL || c->conv->ncl == 0 ) ) {
		INT	sel = ( kind == TB_PARTS || kind == XB_PARTS ) ? pt->sb_at : -1;

		cx = x + tb_width(pt, s, caret);
		if ( kind == NB_PARTS ) {
			cx = tb_x(pt, r, s, n) + tb_width(pt, s, n);
		}
		if ( sel >= 0 && sel != caret && sel <= n ) {
			INT		xs = tb_x(pt, r, s, n) + tb_width(pt, s, sel);
			T_DPRECT	q;

			rc(&q, ( xs < cx ) ? xs : cx, y - PD_FH - 1, ( xs < cx ) ? cx + 1 : xs + 1, y + 2);
			xor_rect(gid, &q);
		} else if ( c->caret ) {
			dp_line(gid, cx, y - PD_FH + 1, cx, y + 1, fg);
		}
	}
	clip_back(gid, &was);
}

/* ---------------------------------------------------------------- fields along a line */

/*
 * The pieces of a box of fields, laid out along the line: the fields
 * in their order, each as wide as its letters; the words between, as
 * wide as they are. Where each starts is measured from the box's left
 * less three.
 */
#define SB_ITEM_MAX	( 2 * WM_SB_MAX + 2 )

typedef struct {
	INT	field;			/* the field's number, or -1 for words */
	INT	at, n;			/* words: where in the label, how many bytes */
	INT	chars;			/* a field: how many letters wide */
	INT	dx, w;
} SBITEM;

LOCAL INT sb_items( T_WMPART *pt, SBITEM *it )
{
	INT	i = 0, k = 0, f = 0, x = 0;
	INT	digit = width((CONST UB *)"0", 1, 0);
	CONST UB *s = pt->label;

	while ( i < WM_LABEL_MAX && s[i] != 0 && k < SB_ITEM_MAX ) {
		if ( s[i] == '#' ) {
			INT	run = 0;

			while ( i < WM_LABEL_MAX && s[i] == '#' ) {
				run++;
				i++;
			}
			if ( f < WM_SB_MAX ) {
				it[k].field = f;
				it[k].chars = run;
				it[k].dx = x;
				/* a field of numbers as wide as its digits, of names as its letters */
				it[k].w = run * ( ( f < WM_SB_MAX && ( pt->sbf[f] & SBF_NAMES ) ) ? PD_FH : digit );
				pt->sbw[f] = run;
				x += it[k].w;
				k++;
				f++;
			}
			continue;
		}
		it[k].field = -1;
		it[k].at = i;
		while ( i < WM_LABEL_MAX && s[i] != 0 && s[i] != '#' ) {
			i += u8len(s + i);
		}
		it[k].n = i - it[k].at;
		it[k].dx = x;
		it[k].w = width(s + it[k].at, it[k].n, 0);
		x += it[k].w;
		k++;
	}
	pt->sb_n = f;
	return k;
}

LOCAL INT sb_ymar( CONST T_DPRECT *r )
{
	return ( ( r->bottom - r->top ) - PD_FH - 2 ) / 2 + 2;
}

/* A field's letters: its number, with noughts to its width when asked, or its name */
LOCAL INT sb_text( CONST T_WMPART *pt, CONST UB *pool, INT f, INT chars, UB *out, INT max )
{
	INT	v = pt->sbv[f], n;

	out[0] = 0;
	if ( pt->type & P_BLANK ) {
		return 0;
	}
	if ( pt->sbf[f] & SBF_NAMES ) {
		CONST UB *nm = pd_pool_name(pool, pt->sbpool[f], v);

		n = s_len(nm, max - 1);
		if ( n > 0 ) {
			knl_memcpy(out, nm, n);
		}
		out[n] = 0;
		return n;
	}
	n = num_text(v, out, max);
	if ( ( ( pt->type & P_ZERO ) || ( pt->sbf[f] & SBF_ZERO ) ) && n < chars && chars < max - 1 ) {
		INT	z = chars - n, j;

		for ( j = n; j >= 0; j-- ) out[j + z] = out[j];
		for ( j = 0; j < z; j++ ) out[j] = '0';
		n = chars;
	}
	return n;
}

EXPORT INT pd_sb_at( T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y, INT *p_half )
{
	SBITEM	it[SB_ITEM_MAX];
	INT	k, n = sb_items(pt, it), best = -1;
	INT	ybot = r->bottom - sb_ymar(r) + 1;

	*p_half = 0;
	for ( k = 0; k < n; k++ ) {
		INT	x0 = r->left + 3 + it[k].dx, xr = x0 + it[k].w + 1;

		if ( it[k].field < 0 ) {
			continue;
		}
		if ( best < 0 || x >= x0 ) {
			best = it[k].field;
		}
		if ( x >= x0 && x < xr ) {
			if ( it[k].field == pt->sb_at && y >= ybot ) {
				*p_half = ( x < ( x0 + ( x0 + it[k].w ) + 1 ) / 2 ) ? -2 : -1;
			}
			return it[k].field;
		}
	}
	return best;
}

/*
 * Fields along a line: a sunken box on the pale ground, and in it the
 * pieces from three in -- a number set against the right of its field
 * unless it asks for the left, a name, or the words between. The field
 * being worked, while the box has the keys, stands on white with two
 * triangles under it: the left one points down and steps it down, the
 * right one points up and steps it up, and the half being pressed is
 * turned over.
 */
LOCAL void draw_serial( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	INT		gid = c->gid, k, n;
	SBITEM		it[SB_ITEM_MAX];
	BOOL		off = ( pt->type & ( P_DISABLE | P_INACT ) ) != 0;
	UW		fg = wm_look(off ? LK_INACTPARTSCOL : LK_ACTPARTSCOL);
	INT		fr = ( pt->type & P_NOFRAME ) ? 0 : ( pt->type & P_EMPHAS ) ? 3 : 1;
	INT		top = r->top + fr, bot = r->bottom - fr;
	INT		base = r->bottom - sb_ymar(r), ybot = base + 1;
	T_DPRECT	in, was;
	UB		buf[WM_LABEL_MAX];

	n = sb_items(pt, it);
	pfill(gid, r->left, r->top, r->right, r->bottom, LK_INACTPARTS);
	box_frame(gid, pt, r);
	box_inside(pt, r, &in);
	clip_in(gid, &in, &was);
	for ( k = 0; k < n; k++ ) {
		INT	x0 = r->left + 3 + it[k].dx, xr = x0 + it[k].w + 1;

		if ( it[k].field < 0 ) {
			(void)text(gid, x0, base, pt->label + it[k].at, it[k].n, 0, fg);
			continue;
		}
		if ( c->focus && !off && it[k].field == pt->sb_at ) {
			INT		mid = ( x0 + ( x0 + it[k].w ) + 1 ) / 2;
			T_DPRECT	q;

			pfill(gid, x0, ybot - PD_FH, xr, ybot, LK_ACTPARTS);
			tri(gid, LK_PEMPHAS, x0, ybot, ( x0 + mid ) / 2, bot - 1, mid - 1, ybot);
			if ( c->held == -2 ) {
				rc(&q, x0, ybot, mid, bot);
				xor_rect(gid, &q);
			}
			tri(gid, LK_PEMPHAS, mid, bot - 1, ( xr + mid ) / 2, ybot, xr - 1, bot - 1);
			if ( c->held == -1 ) {
				rc(&q, mid, ybot, xr, bot);
				xor_rect(gid, &q);
			}
		}
		{
			INT	f = it[k].field, m = sb_text(pt, c->pool, f, it[k].chars, buf, sizeof(buf));
			INT	x = x0;

			if ( m <= 0 ) {
				continue;
			}
			if ( !( pt->sbf[f] & ( SBF_NAMES | SBF_LEFT ) ) ) {
				INT	tw = width(buf, m, 0);

				if ( xr - 1 - tw > x0 ) {
					x = xr - 1 - tw;
				}
			}
			(void)text(gid, x, base, buf, m, 0, fg);
		}
	}
	(void)top;
	clip_back(gid, &was);
}

/* ---------------------------------------------------------------- tags */

typedef struct {
	INT		num;		/* the tag's number */
	INT		sub;		/* -1 an upper tag with a lower row, 0 an upper tag
					   alone, above nought a lower tag: its upper tag's
					   place + 1 */
	INT		row;		/* which upper tag with a lower row (for sbv), -1 */
	CONST UB	*name;
	T_DPRECT	r;		/* the letters' box */
} PDTAG;

/*
 * The tags laid out from their names. Along each row a tag starts four
 * after where the last one's room ended; its box is as wide as its
 * name and four more, and the next one's room starts three after it.
 * The upper row stands three above the sheet, the letters' height tall;
 * the lower row three above a line the letters' height and ten below
 * the sheet's head, starting again from the sheet's left.
 */
LOCAL INT tag_layout( CONST T_WMPART *pt, CONST UB *pool, PDTAG *t )
{
	INT	i = 0, k, num = 1, x = pt->inner.left, saved = -1, grp = 0, nrow = 0, row = -1;
	INT	top2 = pt->inner.top + PD_FH + 10;

	for ( k = 0; k < pt->count && i < PD_TAG_MAX; k++ ) {
		CONST UB	*s = pd_pool_name(pool, pt->pool, k);
		INT		parent = -1;

		if ( s == NULL ) {
			break;
		}
		if ( IS_PLUS(s) ) {
			s += 3;
		} else {
			if ( saved > 0 ) {
				x = saved;
				saved = -1;
			}
			grp = 0;
			row = -1;
			if ( IS_AT(s) ) {
				s += 3;
				grp = -1;
			}
		}
		for ( ;; ) {
			t[i].num = num;
			t[i].sub = grp;
			t[i].row = row;
			t[i].name = s;
			x += 4;
			t[i].r.left = x;
			x += width(s, -1, 0) + 4;
			t[i].r.right = x;
			x += 3;
			t[i].r.bottom = ( ( grp > 0 ) ? top2 : pt->inner.top ) - 3;
			t[i].r.top = t[i].r.bottom - PD_FH;
			if ( grp >= 0 || i + 1 >= PD_TAG_MAX || k + 1 >= pt->count ) {
				i++;
				break;
			}
			/* an upper tag with a lower row: the name after it is that row's first */
			parent = i;
			t[i].row = nrow;
			row = nrow++;
			i++;
			k++;
			s = pd_pool_name(pool, pt->pool, k);
			if ( s == NULL ) {
				break;
			}
			grp = parent + 1;
			saved = x;
			x = pt->inner.left;
		}
		num++;
	}
	/* an upper tag with a lower row stands for the tag last shown in it */
	for ( k = 0; k < i; k++ ) {
		if ( t[k].sub < 0 && t[k].row >= 0 && t[k].row < WM_SB_MAX && pt->sbv[t[k].row] > 0 ) {
			t[k].num = pt->sbv[t[k].row];
		}
	}
	return i;
}

LOCAL BOOL tag_off( CONST T_WMPART *pt, INT num )
{
	return (BOOL)( num >= 1 && num <= 32 && ( pt->inact & ( 1U << ( num - 1 ) ) ) != 0 );
}

/* One tag, over the sheet B it belongs to */
LOCAL void draw_tag( INT gid, CONST T_WMPART *pt, CONST PDTAG *e, CONST T_DPRECT *B, BOOL shown )
{
	INT		a = e->r.left - 4, b = e->r.top - 4, cc = e->r.right + 3, d = e->r.bottom;
	INT		L = B->left, T = B->top, R = B->right, Bt = B->bottom;
	T_DPPOINT	p[11];
	T_DPPAT		in, blk, wht;
	T_FNMET		met;
	INT		x, y;

	wm_look_pat(LK_PNLGROUND, &in);
	dp_pat_colour(&blk, BLACK);
	dp_pat_colour(&wht, WHITE);
	if ( shown ) {
		p[0].x = L;      p[0].y = Bt - 1;
		p[1].x = L;      p[1].y = T;
		p[2].x = a;      p[2].y = T;
		p[3].x = a;      p[3].y = b + 4;
		p[4].x = a + 4;  p[4].y = b;
		p[5].x = cc - 4; p[5].y = b;
		p[6].x = cc;     p[6].y = b + 4;
		p[7].x = cc;     p[7].y = T;
		p[8].x = R - 1;  p[8].y = T;
		p[9].x = R - 1;  p[9].y = Bt - 1;
		p[10].x = L;     p[10].y = Bt - 1;
		dp_fill_poly(gid, p, 11, DP_POLY_ODD, &in);
		dp_draw_poly(gid, p, 11, FALSE, 1, DP_LINE_SOLID, &blk);
		p[0].x = R - 2;  p[0].y = T + 1;
		p[1].x = R - 2;  p[1].y = Bt - 2;
		p[2].x = L + 1;  p[2].y = Bt - 2;
		dp_draw_poly(gid, p, 3, FALSE, 1, DP_LINE_SOLID, &blk);
		p[0].x = L + 1;  p[0].y = Bt - 2;
		p[1].x = L + 1;  p[1].y = T + 1;
		p[2].x = a + 1;  p[2].y = T + 1;
		p[3].x = a + 1;  p[3].y = b + 4;
		p[4].x = a + 4;  p[4].y = b + 1;
		p[5].x = cc - 4; p[5].y = b + 1;
		p[6].x = cc - 1; p[6].y = b + 4;
		p[7].x = cc - 1; p[7].y = T + 1;
		p[8].x = R - 3;  p[8].y = T + 1;
		dp_draw_poly(gid, p, 9, FALSE, 1, DP_LINE_SOLID, &wht);
	} else {
		p[0].x = a;      p[0].y = T - 1;
		p[1].x = a;      p[1].y = b + 4;
		p[2].x = a + 4;  p[2].y = b;
		p[3].x = cc - 4; p[3].y = b;
		p[4].x = cc;     p[4].y = b + 4;
		p[5].x = cc;     p[5].y = T - 1;
		dp_fill_poly(gid, p, 6, DP_POLY_ODD, &in);
		dp_draw_poly(gid, p, 6, FALSE, 1, DP_LINE_SOLID, &blk);
		p[0].x = a + 1;  p[0].y = T - 1;
		p[1].x = a + 1;  p[1].y = b + 4;
		p[2].x = a + 4;  p[2].y = b + 1;
		p[3].x = cc - 4; p[3].y = b + 1;
		dp_draw_poly(gid, p, 4, FALSE, 1, DP_LINE_SOLID, &wht);
	}
	p[0].x = cc - 4; p[0].y = b + 1;
	p[1].x = cc - 1; p[1].y = b + 4;
	p[2].x = cc - 1; p[2].y = T;
	dp_draw_poly(gid, p, 3, FALSE, 1, DP_LINE_SOLID, &blk);

	/* the name: white a pixel down and to the right, then black or grey */
	/* in the middle of the tag, from its cut head to the sheet's edge */
	(void)face(&met);
	x = a + 6;
	y = ( b + T + met.ascent - met.descent ) / 2;
	(void)text(gid, x + 1, y + 1, e->name, -1, 0, WHITE);
	(void)text(gid, x, y, e->name, -1, 0, tag_off(pt, e->num) ? GREY : BLACK);
	(void)d;
}

/*
 * A panel of tags: the frame round the sheet as wide as the widest of
 * its sides, the ground above the sheet in the frame's pattern, then
 * the upper row -- the tag shown joined to the sheet, which it fills
 * with the panels' ground -- and, under an upper tag shown that has
 * one, its lower row over the sheet's lower part.
 */
LOCAL void draw_tags( T_WMPART *pt, CONST T_DPRECT *F, T_PDCTX *c )
{
	INT		gid = c->gid, n, k, w, lower = -1;
	PDTAG		t[PD_TAG_MAX];
	T_DPRECT	I = pt->inner, I2 = pt->inner, top;
	T_DPPAT		fr;

	n = tag_layout(pt, c->pool, t);
	I2.top += PD_FH + 10;
	w = I.left - F->left;
	if ( F->right - I.right > w ) w = F->right - I.right;
	if ( F->bottom - I.bottom > w ) w = F->bottom - I.bottom;
	wm_look_pat(LK_ACTFRAME, &fr);
	if ( w > 0 ) {
		dp_frame_rect_pat(gid, F, &fr, w);
	}
	top = *F;
	top.bottom = I.top;
	dp_fill_rect_pat(gid, &top, &fr);
	for ( k = 0; k < n; k++ ) {
		if ( t[k].name == NULL || t[k].sub > 0 ) {
			continue;
		}
		draw_tag(gid, pt, &t[k], &I, (BOOL)( t[k].num == pt->value ));
		if ( t[k].num == pt->value && t[k].sub < 0 ) {
			lower = k;
		}
	}
	for ( k = lower + 1; lower >= 0 && k < n && t[k].sub > 0; k++ ) {
		draw_tag(gid, pt, &t[k], &I2, (BOOL)( t[k].num == pt->value ));
	}
	if ( c->held_at > 0 ) {
		for ( k = 0; k < n; k++ ) {
			if ( t[k].num == c->held_at && ( t[k].sub <= 0 || ( lower >= 0 && t[k].sub == lower + 1 ) ) ) {
				pd_tag_turn(gid, &t[k].r);
				break;
			}
		}
	}
}

EXPORT void pd_tag_turn( INT gid, CONST T_DPRECT *tab )
{
	T_DPRECT	q = *tab;

	q.bottom++;
	xor_rect(gid, &q);
}

/*
 * The tag under a point: of the upper row, or of the lower row of the
 * upper tag shown; none that cannot be chosen.
 */
EXPORT INT pd_tag_at( T_WMPART *pt, CONST UB *pool, INT x, INT y, T_DPRECT *p_tab )
{
	PDTAG	t[PD_TAG_MAX];
	INT	n = tag_layout(pt, pool, t), k;

	for ( k = 0; k < n; k++ ) {
		if ( t[k].name == NULL ) {
			continue;
		}
		if ( t[k].sub > 0 && t[t[k].sub - 1].num != pt->value ) {
			continue;		/* a lower row not shown */
		}
		if ( x >= t[k].r.left && x < t[k].r.right && y >= t[k].r.top && y < t[k].r.bottom ) {
			if ( tag_off(pt, t[k].num) ) {
				return 0;
			}
			if ( p_tab != NULL ) {
				*p_tab = t[k].r;
			}
			return t[k].num;
		}
	}
	return 0;
}

/*
 * A tag shown. A lower tag becomes the one its upper tag stands for;
 * the answer is the tag, or 0 when it was the one shown already.
 */
EXPORT INT pd_tag_show( T_WMPART *pt, CONST UB *pool, INT num )
{
	PDTAG	t[PD_TAG_MAX];
	INT	n = tag_layout(pt, pool, t), k;

	for ( k = 0; k < n; k++ ) {
		if ( t[k].num != num || t[k].sub < 0 || t[k].name == NULL ) {
			continue;
		}
		if ( t[k].sub > 0 ) {
			INT	up = t[k].sub - 1;

			if ( t[up].row >= 0 && t[up].row < WM_SB_MAX ) {
				pt->sbv[t[up].row] = num;
			}
		}
		if ( num == pt->value ) {
			return 0;
		}
		pt->value = num;
		return num;
	}
	return E_PAR;
}

/* ---------------------------------------------------------------- candidates */

/*
 * The candidates of a conversion listed under the box, or over it when
 * there is no room below: a white box framed in the dark line, a row
 * for each with its number, the one chosen turned over.
 */
EXPORT void pd_cands( INT gid, CONST T_DPRECT *r, T_PDCONV *cv, INT h )
{
	CONST UB	*s = cv->cand;
	UB		row[PD_CONV_TEXT + 8];
	INT		k, w = 0, rh = PD_FH + 6, base;
	T_DPRECT	q;

	if ( !cv->list || cv->ncand <= 0 ) {
		rc(&cv->box, 0, 0, 0, 0);
		return;
	}
	for ( k = 0; k < cv->ncand && k < PD_CAND_ROWS; k++ ) {
		INT	n = s_len(s, PD_CONV_TEXT);

		row[0] = (UB)( '1' + k );
		row[1] = ' ';
		knl_memcpy(row + 2, s, n);
		if ( width(row, n + 2, 0) > w ) {
			w = width(row, n + 2, 0);
		}
		s += n + 1;
	}
	rc(&cv->box, r->left, r->bottom, r->left + w + 12, r->bottom + rh * k + 4);
	if ( cv->box.bottom > h && r->top - ( rh * k + 4 ) >= 0 ) {
		rc(&cv->box, r->left, r->top - rh * k - 4, r->left + w + 12, r->top);
	}
	dp_fill_rect(gid, &cv->box, WHITE);
	dp_frame_rect(gid, &cv->box, dark(), 1);
	s = cv->cand;
	for ( k = 0; k < cv->ncand && k < PD_CAND_ROWS; k++ ) {
		INT	n = s_len(s, PD_CONV_TEXT), top = cv->box.top + 2 + k * rh;
		BOOL	sel = (BOOL)( k == cv->chosen );

		row[0] = (UB)( '1' + k );
		row[1] = ' ';
		knl_memcpy(row + 2, s, n);
		base = top + ( rh + PD_FH ) / 2 - 2;
		rc(&q, cv->box.left + 1, top, cv->box.right - 1, top + rh);
		if ( sel ) {
			dp_fill_rect(gid, &q, wm_look(LK_ACTPARTSCOL));
		}
		(void)text(gid, cv->box.left + 6, base, row, n + 2, 0,
			   sel ? WHITE : wm_look(LK_ACTPARTSCOL));
		s += n + 1;
	}
}

EXPORT INT pd_cand_at( CONST T_PDCONV *cv, INT x, INT y )
{
	INT	rh = PD_FH + 6, k;

	if ( !cv->list || x < cv->box.left || x >= cv->box.right
	  || y < cv->box.top || y >= cv->box.bottom ) {
		return -1;
	}
	k = ( y - cv->box.top - 2 ) / rh;
	return ( k >= 0 && k < cv->ncand && k < PD_CAND_ROWS ) ? k : -1;
}

/* ---------------------------------------------------------------- one part */

/* A switch: its body, a ring when it answers, then its name or names */
LOCAL void draw_switch( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	INT		gid = c->gid, kind = WM_PT_KIND(pt->type);
	BOOL		off = ( pt->type & P_DISABLE ) != 0;
	T_DPRECT	e;
	T_DPPAT		pat;

	if ( kind == WS_PARTS && pt->count == 0 ) {
		/* a choice of a group: the first draws the box round them all */
		if ( c->first ) {
			T_DPRECT	g = c->group;

			g.left -= 4;  g.top -= 4;  g.right += 4;  g.bottom += 4;
			if ( pt->type & P_NOFRAME ) {
				wm_look_pat(LK_INACTPARTS, &pat);
				dp_fill_round(gid, &g, RAD, RAD, &pat);
			} else {
				rrc_raised(gid, &g, LK_INACTPARTS);
			}
		}
		name_in(gid, r, r, pt->label, pt->value != 0 ? 1 : 0, off || ( pt->type & P_INACT ),
			(BOOL)( c->held != 0 ), 0, (BOOL)( ( pt->type & P_NOW ) != 0 ), LK_INACTPARTS);
		return;
	}
	if ( pt->type & P_NOFRAME ) {
		wm_look_pat(LK_INACTPARTS, &pat);
		dp_fill_round(gid, r, RAD, RAD, &pat);
	} else {
		rrc_raised(gid, r, LK_INACTPARTS);
	}
	if ( kind != WS_PARTS && ( pt->type & P_EMPHAS ) ) {
		rc(&e, r->left + 2, r->top + 2, r->right - 2, r->bottom - 2);
		wm_look_pat(LK_PEMPHAS, &pat);
		dp_frame_round(gid, &e, 6, 6, 2, &pat);
	}
	switch ( kind ) {
	case AS_PARTS:
		name_in(gid, r, r, pt->label, pt->value ? 1 : 0, off, (BOOL)( c->held != 0 ), 0,
			FALSE, LK_INACTPARTS);
		break;
	case MS_PARTS:
		name_in(gid, r, r, pt->label, 0, off, FALSE, -1, FALSE, LK_INACTPARTS);
		if ( c->held != 0 && !( pt->type & P_NOFRAME ) ) {
			xor_round(gid, r);
		} else if ( c->held != 0 ) {
			xor_rect(gid, r);
		}
		break;
	case WS_PARTS: {
		INT	i;

		for ( i = 0; i < pt->count; i++ ) {
			CONST UB	*nm = pd_pool_name(c->pool, pt->pool, i);
			BOOL		noff = off || ( i < 32 && ( pt->inact & ( 1U << i ) ) );

			if ( nm == NULL ) {
				break;
			}
			ws_cell(pt, r, pt->count, i, &e);
			name_in(gid, r, &e, nm, ( pt->value == i + 1 ) ? 1 : 0, noff,
				(BOOL)( c->held_at == i + 1 ), 0, (BOOL)( pt->now == i + 1 ), LK_INACTPARTS);
			if ( c->held_at == i + 1 ) {
				xor_rect(gid, &e);
			}
		}
		break;
	}
	default:
		break;
	}
}

/*
 * Words on the panel: from the left of the rectangle on its foot as
 * the base line (P_BASE), or in the middle of it; in the colour of a
 * part's words.
 */
LOCAL void draw_label( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	BOOL		off = ( pt->type & ( P_DISABLE | P_INACT ) ) != 0;
	UW		fg = wm_look(off ? LK_INACTPARTSCOL : LK_ACTPARTSCOL);
	T_FNMET		met;
	T_DPRECT	was;

	(void)face(&met);
	clip_in(c->gid, r, &was);
	if ( pt->type & P_BASE ) {
		(void)text(c->gid, r->left, r->bottom - 4, pt->label, -1, 0, fg);
	} else {
		(void)text(c->gid, r->left + 4,
			   r->top + ( r->bottom - r->top + met.ascent - met.descent ) / 2,
			   pt->label, -1, 0, fg);
	}
	clip_back(c->gid, &was);
}

EXPORT void pd_part( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c )
{
	ID		fid;
	T_FNSTATE	st;
	BOOL		veil = FALSE;

	fn_hold();
	fid = fn_system();
	if ( fid > 0 ) {
		(void)fn_state_get(fid, &st);
	}
	switch ( WM_PT_KIND(pt->type) ) {
	case TB_PARTS:
	case XB_PARTS:
	case NB_PARTS:
		draw_box(pt, r, c);
		break;
	case SB_PARTS:
		draw_serial(pt, r, c);
		break;
	case AS_PARTS:
	case MS_PARTS:
	case WS_PARTS:
		draw_switch(pt, r, c);
		veil = TRUE;
		break;
	case SS_PARTS:
		draw_scroll(pt, r, c);
		veil = TRUE;
		break;
	case VL_PARTS:
		draw_volume(pt, r, c);
		break;
	case TG_PARTS:
		draw_tags(pt, r, c);
		break;
	case WM_PT_LABEL:
		draw_label(pt, r, c);
		break;
	case WM_PT_LINE:
		dp_line(c->gid, r->left, r->top, r->right, r->bottom, dark());
		break;
	default:
		break;
	}
	/* a switch that cannot be worked is veiled in a half-tone of white */
	if ( veil && ( pt->type & P_DISABLE ) ) {
		half_fill(c->gid, r, WHITE, TRUE);
	}
	if ( fid > 0 ) {
		(void)fn_state_set(fid, &st);
	}
	fn_release();
}
