/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	host_compose.c (host)
 *	compose.c's stages called with the plain types of hc.h.
 */

#include "kernel.h"
#include <ts/dp.h>
#include "../../../outer_kernel/wm/compose.h"
#include "hc.h"

/*
 * compose.c's vector loops (compose_simd.c, x64's), never called on the
 * host: no task there may use them, so the stages take the plain loops
 */
EXPORT void cps_copy( UW *d, CONST UW *s, INT n ) { (void)d; (void)s; (void)n; abort(); }
EXPORT void cps_lay( UW *dst, CONST UW *src, CONST UW *und, INT n, UINT a, BOOL keyed, UW tint, UINT tstr, UW wcol,
		     UINT wstr )
{
	(void)dst; (void)src; (void)und; (void)n; (void)a; (void)keyed; (void)tint; (void)tstr; (void)wcol; (void)wstr;
	abort();
}
EXPORT void cps_darken( UW *row, CONST UB *a, INT n ) { (void)row; (void)a; (void)n; abort(); }
EXPORT void cps_halve( CONST T_CPLEVEL *s, CONST T_DPRECT *d, UW *out, UH *row, UH *mid )
{
	(void)s; (void)d; (void)out; (void)row; (void)mid;
	abort();
}
EXPORT void cps_smooth( CONST T_CPLEVEL *s, CONST T_DPRECT *d, CONST INT *wt, UW *out, UH *row, UH *mid )
{
	(void)s; (void)d; (void)wt; (void)out; (void)row; (void)mid;
	abort();
}
EXPORT void cps_double( CONST T_CPLEVEL *s, CONST T_DPRECT *d, UW *out, UH *row, UH *mid )
{
	(void)s; (void)d; (void)out; (void)row; (void)mid;
	abort();
}

/* No window of the test has an outline of its own */
EXPORT INT dp_rgn_row( CONST T_DPRGN *rgn, INT y, CONST INT **p_spans )
{
	(void)rgn;
	(void)y;
	*p_spans = NULL;
	return 0;
}

LOCAL T_DPRECT rect_of( HC_RECT r )
{
	T_DPRECT	d;

	d.left = r.left;
	d.top = r.top;
	d.right = r.right;
	d.bottom = r.bottom;
	return d;
}

LOCAL void win_of( CONST HC_WIN *h, T_CPWIN *w )
{
	knl_memset(w, 0, sizeof(*w));
	w->outer = rect_of(h->outer);
	w->work = rect_of(h->work);
	w->band = rect_of(h->band);
	w->pixels = h->pixels;
	w->pitch = h->pitch;
	w->a_frame = h->a_frame;
	w->a_work = h->a_work;
	w->keyed = (BOOL)h->keyed;
	w->round = h->round;
	w->shape = NULL;
	w->wash = h->wash;
	w->wash_str = h->wash_str;
	w->bar_wash = h->bar_wash;
	w->bar_str = h->bar_str;
	w->tint = h->tint;
	w->tint_str = h->tint_str;
	w->blur = h->blur;
	w->shd_dx = h->shd_dx;
	w->shd_dy = h->shd_dy;
	w->shd_size = h->shd_size;
	w->shd_dark = h->shd_dark;
	w->shown = rect_of(h->shown);
	w->fade = h->fade;
}

void hc_window( uint8_t *back, unsigned pitch, HC_RECT box, int rows, const HC_WIN *h )
{
	T_CPDST	d;
	T_CPWIN	w;

	d.back = back;
	d.pitch = pitch;
	d.box = rect_of(box);
	d.rows = rows;
	win_of(h, &w);
	cp_window(&d, &w);
}

int hc_pieces( uint8_t *back, unsigned pitch, HC_RECT box, int rows, const HC_WIN *h, HC_PIECE *p )
{
	T_CPDST		d;
	T_CPWIN		w;
	T_CPPIECE	q[CP_NPIECE];
	INT		i, n;

	d.back = back;
	d.pitch = pitch;
	d.box = rect_of(box);
	d.rows = rows;
	win_of(h, &w);
	n = cp_pieces(&d, &w, q);
	for ( i = 0; i < n; i++ ) {
		p[i].r.left = q[i].r.left;
		p[i].r.top = q[i].r.top;
		p[i].r.right = q[i].r.right;
		p[i].r.bottom = q[i].r.bottom;
		p[i].a = q[i].a;
		p[i].frame = (int)q[i].frame;
		p[i].wcol = q[i].wcol;
		p[i].wstr = q[i].wstr;
	}
	return n;
}

int hc_round_of( HC_RECT r, int rad )
{
	T_DPRECT	d = rect_of(r);

	return cp_round_of(&d, rad);
}

const uint8_t *hc_corner_cov( int rad )
{
	return cp_corner_cov(rad);
}

int hc_shadow_extent( const HC_WIN *h, HC_RECT *out )
{
	T_CPWIN		w;
	T_DPRECT	e;

	win_of(h, &w);
	if ( !cp_shadow_extent(&w, &e) ) return 0;
	out->left = e.left;
	out->top = e.top;
	out->right = e.right;
	out->bottom = e.bottom;
	return 1;
}

int hc_moving( const HC_WIN *h )
{
	T_CPWIN	w;

	win_of(h, &w);
	return CP_MOVING(&w) ? 1 : 0;
}

LOCAL HC_RECT hc_of( T_DPRECT r )
{
	HC_RECT	h;

	h.left = r.left;
	h.top = r.top;
	h.right = r.right;
	h.bottom = r.bottom;
	return h;
}

int hc_spread_plan( HC_RECT box, HC_RECT r, int rad, HC_SPREAD *p )
{
	T_DPRECT	b = rect_of(box), q = rect_of(r);
	T_CPSPREAD	sp;
	INT		k;

	if ( !cp_spread_plan(&b, &q, rad, &sp) ) return 0;
	p->depth = sp.depth;
	for ( k = 0; k < 5; k++ ) p->w[k] = sp.w[k];
	for ( k = 0; k <= sp.depth; k++ ) {
		p->valid[k] = hc_of(sp.valid[k]);
		p->down[k] = hc_of(sp.down[k]);
		p->up[k] = hc_of(sp.up[k]);
	}
	return 1;
}
