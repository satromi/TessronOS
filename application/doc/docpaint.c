/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docpaint.c
 *	A window's drawing record and its shape record shown (design 18.13,
 *	wm_obj_painter)
 *
 *	A record of xmlTAD drawn into a box as whatever it is, and the
 *	region a figure covers made by drawing it into pixels of the
 *	kernel's own. Only the window manager asks for these, so they stay
 *	in the kernel when the drawing of documents is linked into a
 *	program running as a process as well.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/wm.h>
#include <ts/om.h>
#include <ts/dt.h>

EXPORT ER tv_paint( INT gid, CONST T_DPRECT *r, CONST UB *xml, SZ len )
{
	T_TAD	*doc;
	UW	paper = wm_look(WM_LOOK_WORK);
	ER	er;

	if ( r == NULL ) {
		return E_PAR;
	}
	dp_fill_rect(gid, r, paper);
	if ( xml == NULL || len <= 0 ) {
		return E_OK;
	}
	er = tad_parse(xml, len, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	switch ( tv_kind(doc) ) {
	case TV_KIND_DOC: {
		T_TVDOC	*d;

		er = tv_doc(doc, &d);
		if ( er >= E_OK ) {
			(void)tv_doc_draw(gid, d, r, 0, doc, paper);
			tv_doc_free(d);
		}
		break;
	}
	case TV_KIND_FIG: {
		T_TVFIG	*f;

		er = tv_fig(doc, &f);
		if ( er >= E_OK ) {
			er = tv_fig_draw(gid, f, r, 0, 0, doc, paper);
			tv_fig_free(f);
		}
		break;
	}
	default:
		break;
	}
	tad_free(doc);

	return er;
}

/* The colour nothing a figure draws comes out as: what is left of it is outside */
#define TV_SHAPE_KEY	0xFF01FE03U

/*
 * The rows of a mask put into a region. A region is made from rows of
 * one span each, so the k-th span of each row is gathered while the
 * rows run on with one, and each such run is added in one piece.
 */
LOCAL ER mask_region( CONST UW *px, INT w, INT h, T_DPRGN **p_rgn )
{
	INT	*x0, *x1;
	T_DPRGN	*all = NULL, *part, *both;
	INT	k, y, run, found;
	ER	er = E_OK;

	x0 = (INT *)Kmalloc(sizeof(INT) * (SZ)h);
	x1 = (INT *)Kmalloc(sizeof(INT) * (SZ)h);
	if ( x0 == NULL || x1 == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	for ( k = 0, found = 1; found && er >= E_OK; k++ ) {
		found = 0;
		run = 0;
		for ( y = 0; y <= h && er >= E_OK; y++ ) {
			INT	a = -1, b = -1, n = 0, x;

			/* the k-th span of this row, if it has one */
			for ( x = 0; y < h && x < w; ) {
				if ( px[(SZ)y * w + x] == TV_SHAPE_KEY ) { x++; continue; }
				a = x;
				while ( x < w && px[(SZ)y * w + x] != TV_SHAPE_KEY ) x++;
				b = x;
				if ( n++ == k ) break;
				a = -1;
			}
			if ( a >= 0 ) {
				x0[run] = a;
				x1[run] = b;
				run++;
				found = 1;
				continue;
			}
			if ( run > 0 ) {
				er = dp_rgn_from_rows(y - run, run, x0, x1, &part);
				if ( er >= E_OK ) {
					if ( all == NULL ) {
						all = part;
					} else {
						er = dp_rgn_or(all, part, &both);
						dp_rgn_free(part);
						if ( er >= E_OK ) {
							dp_rgn_free(all);
							all = both;
						}
					}
				}
				run = 0;
			}
		}
	}
	if ( er >= E_OK && all == NULL ) {
		T_DPRECT	none = { 0, 0, 0, 0 };

		er = dp_rgn_rect(&none, &all);
	}
    out:
	if ( x0 != NULL ) Kfree(x0);
	if ( x1 != NULL ) Kfree(x1);
	if ( er < E_OK ) {
		dp_rgn_free(all);
		all = NULL;
	}
	*p_rgn = all;
	return er;
}

EXPORT ER tv_shape( CONST UB *xml, SZ len, INT w, INT h, T_DPRGN **p_rgn )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;
	T_DPRECT	r;
	UW		*px;
	INT		gid;
	SZ		i;
	ER		er;

	if ( xml == NULL || len <= 0 || w <= 0 || h <= 0 || p_rgn == NULL ) {
		return E_PAR;
	}
	er = tad_parse(xml, len, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	if ( tv_kind(doc) != TV_KIND_FIG || tv_fig(doc, &f) < E_OK ) {
		tad_free(doc);
		return E_PAR;			/* an outline is a figure */
	}
	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	gid = ( px != NULL ) ? dp_open() : E_NOMEM;
	if ( gid < 0 ) {
		er = (ER)gid;
		goto out;
	}
	for ( i = 0; i < (SZ)w * h; i++ ) {
		px[i] = TV_SHAPE_KEY;
	}
	r.left = 0;
	r.top = 0;
	r.right = w;
	r.bottom = h;
	dp_set_target(gid, px, (UINT)( w * sizeof(UW) ), 0, 0);
	dp_set_origin(gid, 0, 0);
	dp_set_frame(gid, &r);
	dp_set_visible(gid, &r);
	er = tv_fig_draw(gid, f, &r, 0, 0, doc, TV_SHAPE_KEY);
	dp_close(gid);
	if ( er >= E_OK ) {
		er = mask_region(px, w, h, p_rgn);
	}
    out:
	if ( px != NULL ) Kfree(px);
	tv_fig_free(f);
	tad_free(doc);
	return er;
}
