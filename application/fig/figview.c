/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figview.c
 *	Putting a figure on the screen (design 17.5.3, phase 15)
 *
 *	A figure is shapes in an order. Everything is drawn in that order
 *	and nothing else decides what covers what: a figure that drew its
 *	pieces in the order it happened to read them would change when it
 *	was saved and read back.
 *
 *	A piece of text inside a figure is a document, and is drawn by the
 *	same code that draws a document on its own. A virtual object is
 *	drawn by the layer that owns virtual objects. Neither is a special
 *	case here; they are shapes whose drawing is done elsewhere.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/om.h>
#include <ts/img.h>
#include <ts/fn.h>

LOCAL void tex_pat( T_DPPAT *pat, UW col, CONST UH *tex );
LOCAL INT zs( CONST T_TVFIG *f, INT v );

/* A length in the figure as a length on the screen, as it is magnified */
LOCAL INT zs( CONST T_TVFIG *f, INT v )
{
	INT	z = ( f->zoom > 0 ) ? f->zoom : 8;

	return ( z == 8 ) ? v : v * z / 8;
}

/* ------------------------------------------------- what a link points at */

EXPORT void tv_draw_picture( INT gid, CONST T_DPRECT *box, CONST UB *href )
{
	CONST UW	*px = NULL;
	INT		w = 0, h = 0;
	INT		bw = box->right - box->left;
	INT		bh = box->bottom - box->top;

	if ( bw <= 0 || bh <= 0 ) {
		return;
	}
	if ( href == NULL || knl_tv_src == NULL || knl_tv_src->picture == NULL
	  || knl_tv_src->picture(href, &px, &w, &h, knl_tv_src_arg) < E_OK
	  || px == NULL ) {
		dp_frame_rect(gid, box, wm_look(WM_LOOK_SHADOW), 1);
		return;
	}
	/*
	 * At the size of the box it was placed in. That is rarely the
	 * size it was made at, and a picture drawn at its own size in a
	 * box of another size overlaps whatever was placed beside it.
	 */
	if ( w == bw && h == bh ) {
		dp_put_argb(gid, box->left, box->top, px, w, w, h, IMG_CLEAR);
	} else {
		UW	*sc = img_scale(px, w, h, bw, bh);

		if ( sc == NULL ) {
			dp_frame_rect(gid, box, wm_look(WM_LOOK_SHADOW), 1);
			return;
		}
		dp_put_argb(gid, box->left, box->top, sc, bw, bw, bh, IMG_CLEAR);
		Kfree(sc);
	}
}

/*
 * A picture in its box, turned over and turned round as the shape says:
 * fitted to the box first, then turned about the box's middle.
 */
LOCAL void picture_at( INT gid, CONST T_DPRECT *box, CONST T_TVSHAPE *s )
{
	CONST UW	*px = NULL;
	UW		*sc, *out;
	INT		w = 0, h = 0, bw = box->right - box->left, bh = box->bottom - box->top;
	INT		a = ( s->rot % 360 + 360 ) % 360 * 4096 / 360, x, y;

	if ( !s->flip_h && !s->flip_v && a == 0 ) {
		tv_draw_picture(gid, box, s->href);
		return;
	}
	if ( bw <= 0 || bh <= 0 || s->href == NULL || knl_tv_src == NULL
	  || knl_tv_src->picture == NULL
	  || knl_tv_src->picture(s->href, &px, &w, &h, knl_tv_src_arg) < E_OK || px == NULL ) {
		tv_draw_picture(gid, box, s->href);
		return;
	}
	sc = img_scale(px, w, h, bw, bh);
	if ( sc == NULL ) {
		return;
	}
	for ( y = 0; y < bh; y++ ) {
		for ( x = 0; x < bw; x++ ) {
			INT	sx = s->flip_h ? bw - 1 - x : x, sy = s->flip_v ? bh - 1 - y : y;

			if ( sx > x || ( sx == x && sy > y ) ) {
				UW	t = sc[y * bw + x];

				sc[y * bw + x] = sc[sy * bw + sx];
				sc[sy * bw + sx] = t;
			}
		}
	}
	if ( a == 0 ) {
		dp_put_argb(gid, box->left, box->top, sc, bw, bw, bh, IMG_CLEAR);
		Kfree(sc);
		return;
	}
	{
		D	c = tv_cos(a), n = tv_sin(a);
		D	ac = ( c < 0 ) ? -c : c, an = ( n < 0 ) ? -n : n;
		INT	ow = (INT)( ( bw * ac + bh * an ) / 16384 ) + 1;
		INT	oh = (INT)( ( bw * an + bh * ac ) / 16384 ) + 1;

		out = (UW *)Kmalloc(sizeof(UW) * (SZ)ow * oh);
		if ( out == NULL ) {
			Kfree(sc);
			return;
		}
		/* each pixel of the turned picture, looked for in the upright one */
		for ( y = 0; y < oh; y++ ) {
			for ( x = 0; x < ow; x++ ) {
				D	dx = x - ow / 2, dy = y - oh / 2;
				INT	sx = (INT)( ( dx * c + dy * n ) / 16384 ) + bw / 2;
				INT	sy = (INT)( ( -dx * n + dy * c ) / 16384 ) + bh / 2;

				out[y * ow + x] = ( sx >= 0 && sy >= 0 && sx < bw && sy < bh )
						  ? sc[sy * bw + sx] : IMG_CLEAR;
			}
		}
		dp_put_argb(gid, ( box->left + box->right ) / 2 - ow / 2,
			    ( box->top + box->bottom ) / 2 - oh / 2, out, ow, ow, oh,
			    IMG_CLEAR);
		Kfree(out);
	}
	Kfree(sc);
}

/* A mark at a place: a dot, plus, star, circle, cross, or a diamond */
LOCAL void mark_at( INT gid, INT x, INT y, INT half, INT kind, UW col )
{
	T_DPPAT		pat;
	T_DPRECT	r;

	dp_pat_colour(&pat, col);
	switch ( kind ) {
	case 0:
		dp_put_pixel(gid, x, y, col);
		break;
	case 1:
		dp_line(gid, x - half, y, x + half, y, col);
		dp_line(gid, x, y - half, x, y + half, col);
		break;
	case 2:
		dp_line(gid, x, y - half, x, y + half, col);
		dp_line(gid, x - half, y + half, x + half, y - half, col);
		dp_line(gid, x - half, y - half, x + half, y + half, col);
		break;
	case 3:
		r.left = x - half;  r.top = y - half;
		r.right = x + half + 1;  r.bottom = y + half + 1;
		dp_frame_oval(gid, &r, 1, &pat);
		break;
	case 4:
		dp_line(gid, x - half, y - half, x + half, y + half, col);
		dp_line(gid, x + half, y - half, x - half, y + half, col);
		break;
	default: {
		T_DPPOINT	p[4];

		p[0].x = x;  p[0].y = y - half;
		p[1].x = x + half;  p[1].y = y;
		p[2].x = x;  p[2].y = y + half;
		p[3].x = x - half;  p[3].y = y;
		dp_fill_poly(gid, p, 4, DP_POLY_ODD, &pat);
		break;
	}
	}
}


/* How many opened objects deep what is being drawn now is: a text inside one draws its links there */
EXPORT INT	tv_open_depth = 0;

EXPORT ER tv_draw_link( INT gid, CONST T_DPRECT *r, CONST T_VOBJ *v,
			INT depth )
{
	T_VOBJ		vv;
	T_DPRECT	inner;
	INT		band;
	T_TAD		*target = NULL;
	T_OMLOOK	look;
	UB		text[OM_LOOK_TEXT];

	if ( r == NULL || v == NULL ) {
		return E_PAR;
	}
	vv = *v;
	look.text = NULL;
	look.icon = NULL;
	look.icon_w = look.icon_h = 0;
	if ( knl_tv_src != NULL && knl_tv_src->label != NULL
	  && knl_tv_src->label(&vv, text, OM_LOOK_TEXT, knl_tv_src_arg) >= 0 ) {
		look.text = text;
	} else if ( vv.name[0] == '\0' && knl_tv_src != NULL && knl_tv_src->name != NULL ) {
		/* a link with no name of its own is called what the object is called */
		INT	n = knl_tv_src->name(&vv, vv.name, TAD_NAME_MAX, knl_tv_src_arg);

		if ( n < 0 ) {
			vv.name[0] = '\0';
		}
	}
	if ( ( vv.disp & TAD_D_PICT ) != 0 && knl_tv_src != NULL && knl_tv_src->icon != NULL ) {
		if ( knl_tv_src->icon(&vv, &look.icon, &look.icon_w, &look.icon_h, knl_tv_src_arg) < E_OK ) {
			look.icon = NULL;
		}
	}
	band = om_band_h(&vv);

	/*
	 * Open or closed is not a flag: it is the size of the box.
	 *
	 * A closed virtual object is a name on the page, and its box is
	 * the band that name sits in -- twenty-five pixels at fourteen
	 * point, in every record this system has been given. A box that
	 * is taller than that has been opened up to show what it points
	 * at, and the room past the band belongs to the object inside.
	 * The record's own 'height' does not say which it is: it is the
	 * height of the box either way.
	 *
	 * The margin matters. A closed object's box is a pixel or two
	 * taller than its band, from rounding when it was written; asking
	 * only whether the box is taller than the band would open every
	 * name in the document, and drawing a whole other record into a
	 * strip nine pixels high is both wrong and slow.
	 */
	if ( r->bottom - r->top <= band + OM_OPEN_SLACK
	  || depth >= TV_MAX_DEPTH
	  || knl_tv_src == NULL || knl_tv_src->open == NULL ) {
		return om_draw_vob_as(gid, r, &vv, &look, FALSE, NULL);
	}
	om_draw_vob_as(gid, r, &vv, &look, TRUE, &inner);
	if ( inner.right <= inner.left || inner.bottom <= inner.top ) {
		return E_OK;
	}
	target = knl_tv_src->open(&vv, knl_tv_src_arg);
	if ( target == NULL ) {
		return E_OK;			/* nothing to show inside it */
	}
	{
		T_DPENV		env;
		BOOL		narrowed = FALSE;
		UINT		kind = tv_kind(target);

		/* what is shown inside stays inside */
		if ( dp_ref(gid, &env) >= E_OK ) {
			T_DPRECT	cut = env.visible;

			if ( cut.left < inner.left )     cut.left = inner.left;
			if ( cut.top < inner.top )       cut.top = inner.top;
			if ( cut.right > inner.right )   cut.right = inner.right;
			if ( cut.bottom > inner.bottom ) cut.bottom = inner.bottom;
			if ( cut.right > cut.left && cut.bottom > cut.top ) {
				dp_set_visible(gid, &cut);
				narrowed = TRUE;
			}
		}
		/*
		 * What is inside is laid on the link's own bgcol: the link
		 * says what colour its content area is, and that is what
		 * shows round whatever the object draws. The object's own
		 * paper is used only when the link does not say.
		 */
		UW	paper = TV_PAPER_LOOK;

		if ( vv.bgcol != TAD_COL_NONE ) {
			paper = vv.bgcol;
		} else if ( knl_tv_src->paper != NULL ) {
			UW	c = 0;

			if ( knl_tv_src->paper(&vv, &c, knl_tv_src_arg) >= E_OK ) {
				paper = c;
			}
		}
		/* shown at the link's own magnification, eighths to the viewers */
		INT	z8 = ( vv.zoom > 0 && vv.zoom != 100 ) ? ( vv.zoom * 8 + 50 ) / 100 : 8;

		if ( z8 < 1 ) z8 = 1;
		if ( kind == TV_KIND_DOC ) {
			T_TVDOC	*d = NULL;

			if ( tv_doc(target, &d) >= E_OK && d != NULL ) {
				INT	was = tv_open_depth;

				tv_open_depth = depth + 1;
				tv_doc_zoom(z8);
				tv_doc_draw(gid, d, &inner, vv.scrolly, target,
					    paper);
				tv_doc_zoom(8);
				tv_open_depth = was;
				tv_doc_free(d);
			}
		} else if ( kind == TV_KIND_FIG ) {
			T_TVFIG	*fg = NULL;

			if ( tv_fig(target, &fg) >= E_OK && fg != NULL ) {
				if ( z8 != 8 ) fg->zoom = z8;
				tv_fig_draw_at(gid, fg, &inner, vv.scrollx,
					       vv.scrolly, target, paper,
					       depth + 1);
				tv_fig_free(fg);
			}
		}
		if ( narrowed ) {
			dp_set_visible(gid, &env.visible);
		}
	}
	if ( knl_tv_src->shut != NULL ) {
		knl_tv_src->shut(target, knl_tv_src_arg);
	}

	return E_OK;
}


/* Points of the outline of one shape, at most */
#define OUT_MAX		( TV_MAX_PT * 8 + 64 )

/*
 * An arrow head at (tx, ty), coming from (fx, fy): two strokes back
 * from the point, or a filled triangle, as long as the line is thick.
 */
LOCAL void arrow_at( INT gid, INT fx, INT fy, INT tx, INT ty, INT kind, INT w,
		     CONST T_DPPAT *pat )
{
	D		dx = tx - fx, dy = ty - fy, len = tv_isqrt(dx * dx + dy * dy);
	D		al = 15 + 2 * w, aw = 8 + w;
	T_DPPOINT	tri[3];

	if ( len == 0 ) {
		return;
	}
	tri[0].x = tx;
	tri[0].y = ty;
	tri[1].x = (INT)( tx - ( dx * al + dy * aw ) / len );
	tri[1].y = (INT)( ty - ( dy * al - dx * aw ) / len );
	tri[2].x = (INT)( tx - ( dx * al - dy * aw ) / len );
	tri[2].y = (INT)( ty - ( dy * al + dx * aw ) / len );
	if ( kind == 1 ) {
		dp_fill_poly(gid, tri, 3, DP_POLY_ODD, pat);
		return;
	}
	dp_line_wide(gid, tx, ty, tri[1].x, tri[1].y, ( w > 0 ) ? w : 1,
		     DP_LINE_SOLID, pat);
	dp_line_wide(gid, tx, ty, tri[2].x, tri[2].y, ( w > 0 ) ? w : 1,
		     DP_LINE_SOLID, pat);
}

/* The point before the end that is not the end, for the way an arrow points */
LOCAL INT back_from( CONST T_DPPOINT *p, INT n, INT end, INT step )
{
	INT	k = end + step;

	while ( k >= 0 && k < n && p[k].x == p[end].x && p[k].y == p[end].y ) {
		k += step;
	}

	return ( k >= 0 && k < n ) ? k : end;
}

/*
 * A shape drawn from its outline: the inside filled when it is closed
 * and has something to fill with, then the line of the kind and width
 * it says, then the heads of its arrows.
 */
LOCAL void draw_outline( INT gid, CONST T_TVFIG *f, CONST T_TVSHAPE *s,
			 INT ox, INT oy, T_DPPOINT *pts, CONST T_DPPAT *lpat,
			 CONST T_DPPAT *fpat )
{
	INT	n, k, lw;
	BOOL	closed = TRUE;

	if ( s->kind == TV_SH_LINE ) {
		n = tv_line_path(f, s, pts, OUT_MAX);
		closed = FALSE;
	} else {
		n = tv_shape_outline(s, pts, OUT_MAX, &closed);
	}
	if ( n < 2 ) {
		return;
	}
	for ( k = 0; k < n; k++ ) {
		pts[k].x = zs(f, pts[k].x) + ox;
		pts[k].y = zs(f, pts[k].y) + oy;
	}
	lw = ( s->line_w > 0 ) ? zs(f, s->line_w) : 0;
	if ( s->line_w > 0 && lw < 1 ) {
		lw = 1;
	}
	if ( closed && n >= 3 && s->fill_col != TAD_COL_NONE ) {
		dp_fill_poly(gid, pts, n, DP_POLY_ODD, fpat);
	}
	if ( lw > 0 ) {
		/* a kind of line the figure defines for itself, else one of the six */
		CONST UB	*dash = tv_fig_dash(f, s->line_type);

		if ( dash != NULL ) {
			tv_stroke_dash(gid, pts, n, closed, lw, dash, lpat);
		} else {
			tv_stroke(gid, pts, n, closed, lw, s->line_type, lpat);
		}
	}
	if ( !closed && ( s->arrow & 1 ) != 0 ) {
		k = back_from(pts, n, n - 1, -1);
		arrow_at(gid, pts[k].x, pts[k].y, pts[n - 1].x, pts[n - 1].y,
			 s->arrow_type, lw, lpat);
	}
	if ( !closed && ( s->arrow & 2 ) != 0 ) {
		k = back_from(pts, n, 0, 1);
		arrow_at(gid, pts[k].x, pts[k].y, pts[0].x, pts[0].y,
			 s->arrow_type, lw, lpat);
	}
}

/*
 * How far a figure reaches: the view it was written for, and every
 * shape in it, whichever is further. A figure whose shapes lie outside
 * its view is not a fault -- things are put into a cabinet and the view
 * it was saved with is not rewritten -- so what can be scrolled to is
 * the whole of what is there.
 */
EXPORT ER tv_fig_size( CONST T_TVFIG *f, INT *p_w, INT *p_h )
{
	INT	w, h, i;

	if ( f == NULL ) {
		return E_PAR;
	}
	w = f->view.right - f->view.left;
	h = f->view.bottom - f->view.top;
	for ( i = 0; i < f->nsh; i++ ) {
		CONST T_DPRECT	*r = &f->sh[i].r;

		if ( r->right > w ) {
			w = r->right;
		}
		if ( r->bottom > h ) {
			h = r->bottom;
		}
	}
	if ( p_w != NULL ) {
		*p_w = w;
	}
	if ( p_h != NULL ) {
		*p_h = h;
	}

	return E_OK;
}

EXPORT ER tv_fig_draw_at( INT gid, CONST T_TVFIG *f, CONST T_DPRECT *r,
			  INT scroll_x, INT scroll_y, CONST T_TAD *src,
			  UW paper, INT depth )
{
	INT		i;
	T_DPENV		env;
	BOOL		narrowed = FALSE;
	T_DPPOINT	*pts;

	if ( f == NULL || r == NULL ) {
		return E_PAR;
	}
	/*
	 * The paper first, for the same reason a document lays its own:
	 * the shapes of a figure cover very little of the paper, and
	 * whatever was on the screen before shows between them.
	 */
	if ( gid >= 0 && paper != TAD_COL_NONE ) {
		/* none: drawn over what is there, as into a picture */
		dp_fill_rect(gid, r, ( paper == TV_PAPER_LOOK )
				     ? wm_look(WM_LOOK_WORK) : paper);
	}
	/* nothing goes outside the box the figure was given */
	if ( dp_ref(gid, &env) >= E_OK ) {
		T_DPRECT	cut = env.visible;

		if ( cut.left < r->left )     cut.left = r->left;
		if ( cut.top < r->top )       cut.top = r->top;
		if ( cut.right > r->right )   cut.right = r->right;
		if ( cut.bottom > r->bottom ) cut.bottom = r->bottom;
		if ( cut.right > cut.left && cut.bottom > cut.top ) {
			dp_set_visible(gid, &cut);
			narrowed = TRUE;
		}
	}
	/*
	 * 用紙オーバーレイ: laid on the paper first, under the figure's own
	 * shapes, in the order of their numbers. The figure is one sheet,
	 * its first page, so what is only for the even pages is not shown.
	 */
	for ( i = 0; i < TV_MAX_OVER; i++ ) {
		T_TVFIG	*o = f->over[i];

		if ( o == NULL || ( f->over_on & ( 1U << i ) ) == 0
		  || f->over_pages[i] == 2 || f->over_pages[i] == 3 ) {
			continue;
		}
		o->zoom = f->zoom;
		o->show_hidden = f->show_hidden;
		(void)tv_fig_draw_at(gid, o, r, scroll_x, scroll_y, src,
				     TAD_COL_NONE, depth);
	}
	pts = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * OUT_MAX);
	for ( i = 0; pts != NULL && i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];
		T_DPRECT	box;
		T_DPPAT		lpat, fpat;
		INT		ox = r->left - scroll_x;
		INT		oy = r->top - scroll_y;

		box.left   = zs(f, s->r.left) + ox;
		box.top    = zs(f, s->r.top) + oy;
		box.right  = zs(f, s->r.right) + ox;
		box.bottom = zs(f, s->r.bottom) + oy;
		dp_pat_colour(&lpat, s->line_col);
		dp_pat_colour(&fpat, s->fill_col);
		if ( s->fill_tex != NULL ) {
			tex_pat(&fpat, s->fill_col, s->fill_tex);
		}
		if ( s->fill_pat != NULL ) {
			/* a pattern of the figure's own: its pixels, as they are */
			knl_memset(&fpat, 0, sizeof(fpat));
			fpat.kind = DP_PAT_TILE;
			fpat.hs = 16;
			fpat.vs = 16;
			fpat.tile = s->fill_pat->tile;
			fpat.mask = s->fill_pat->mask;
		}

		switch ( s->kind ) {
		case TV_SH_RECT:
			draw_outline(gid, f, s, ox, oy, pts, &lpat, &fpat);
			if ( s->text != NULL && s->text_px > 0 ) {
				/* a box of text: its letters from its top left corner */
				ID		fid = fn_system();
				INT		px = zs(f, s->text_px);
				CONST UB	*t = s->text;

				while ( *t == ' ' || *t == '\n' || *t == '\t' ) t++;
				if ( fid > 0 && px > 0 ) {
					(void)fn_set_size(fid, px);
					(void)fn_draw(gid, fid, box.left + 2, box.top + 2 + px, t, s->text_col);
				}
			}
			break;
		case TV_SH_ELLIPSE:
		case TV_SH_ARC:
		case TV_SH_CHORD:
		case TV_SH_EARC:
		case TV_SH_LINE:
		case TV_SH_POLY:
		case TV_SH_CURVE:
			draw_outline(gid, f, s, ox, oy, pts, &lpat, &fpat);
			break;

		case TV_SH_PIXMAP:
			/* a painted picture: its ground, then what was painted */
			if ( s->back != TAD_COL_NONE && s->rot % 360 == 0 ) {
				dp_fill_rect(gid, &box, s->back);
			}
			picture_at(gid, &box, s);
			break;

		case TV_SH_MARKER: {
			INT	k, half = zs(f, s->rad_h) / 2;

			for ( k = 0; k < s->npt; k++ ) {
				mark_at(gid, zs(f, s->pt[k].x) + ox, zs(f, s->pt[k].y) + oy,
					half, s->rad_v, s->line_col);
			}
			break;
		}

		case TV_SH_IMAGE:
			/*
			 * The picture alone. A frame is drawn only when there
			 * is no picture to show, to keep the room it takes
			 * visible; round a picture that is there it is a line
			 * the person did not draw.
			 */
			picture_at(gid, &box, s);
			break;

		case TV_SH_DOC:
			if ( s->doc >= 0 && s->doc < f->ndoc ) {
				/* a piece of text in a figure is on the
				   figure's own paper, not on a paper of
				   its own */
				tv_doc_zoom(( f->zoom > 0 ) ? f->zoom : 8);
				tv_doc_draw(gid, f->doc[s->doc], &box, 0, src,
					    paper);
				tv_doc_zoom(8);
			}
			break;

		case TV_SH_LINK: {
			T_VOBJ	v;

			if ( s->hidden && !f->show_hidden ) {
				break;		/* 隠蔽仮身: not shown */
			}

			if ( src != NULL && s->link >= 0
			  && tad_lnk_get(src, s->link, &v) >= E_OK ) {
				/* its letters magnified with the rest */
				if ( v.chsz > 0 ) {
					v.chsz = zs(f, v.chsz);
				}
				tv_draw_link(gid, &box, &v, depth);
			} else {
				dp_frame_rect(gid, &box, wm_look(WM_LOOK_FRAME), 1);
			}
			break;
		}

		default:
			break;
		}
	}
	if ( pts != NULL ) {
		Kfree(pts);
	}
	if ( narrowed ) {
		dp_set_visible(gid, &env.visible);
	}

	return E_OK;
}

/*
 * Which virtual object is at a place in a figure, if any.
 *
 * The shapes are walked from the front backwards, because what is in
 * front is what was pressed. A figure's shapes carry their own
 * rectangles, so this needs no layout -- unlike a document, where where
 * a link sits is only known by laying the page out.
 */
EXPORT ER tv_fig_at( CONST T_TVFIG *f, CONST T_DPRECT *r, INT scroll_x,
		     INT scroll_y, CONST T_TAD *src, INT x, INT y,
		     T_VOBJ *out, T_DPRECT *p_box, INT *p_which )
{
	INT	i;

	if ( f == NULL || r == NULL || src == NULL ) {
		return E_PAR;
	}
	for ( i = f->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*sh = &f->sh[i];
		T_DPRECT	box;

		if ( sh->kind != TV_SH_LINK || sh->link < 0 ) {
			continue;
		}
		if ( sh->hidden && !f->show_hidden ) {
			continue;		/* not there to be pressed */
		}
		box.left   = zs(f, sh->r.left) + r->left - scroll_x;
		box.top    = zs(f, sh->r.top) + r->top - scroll_y;
		box.right  = zs(f, sh->r.right) + r->left - scroll_x;
		box.bottom = zs(f, sh->r.bottom) + r->top - scroll_y;
		if ( x < box.left || x >= box.right
		  || y < box.top || y >= box.bottom ) {
			continue;
		}
		if ( p_box != NULL ) {
			*p_box = box;
		}
		if ( p_which != NULL ) {
			*p_which = i;		/* which shape, not which link */
		}
		if ( out != NULL ) {
			return tad_lnk_get(src, sh->link, out);
		}
		return E_OK;
	}

	return E_NOEXS;
}

/*
 * A fill that is a mask: sixteen rows of sixteen, drawn in the colour
 * where a bit is set and leaving what is under it where one is not.
 */
LOCAL UW	tex_tile[16 * 16];
LOCAL UW	tex_mask[16];

EXPORT void tv_tex_pat( T_DPPAT *pat, UW col, CONST UH *tex )
{
	tex_pat(pat, col, tex);
}

LOCAL void tex_pat( T_DPPAT *pat, UW col, CONST UH *tex )
{
	INT	i;

	for ( i = 0; i < 16 * 16; i++ ) {
		tex_tile[i] = ( col == TAD_COL_NONE ) ? 0 : col;
	}
	for ( i = 0; i < 16; i++ ) {
		tex_mask[i] = (UW)tex[i] << 16;
	}
	knl_memset(pat, 0, sizeof(*pat));
	pat->kind = DP_PAT_TILE;
	pat->hs = 16;
	pat->vs = 16;
	pat->tile = tex_tile;
	pat->mask = tex_mask;
}

#define LINE_REACH	6		/* pixels of the screen a press may be off a line */

/* How far a point is from a segment, roughly: enough to say "on it" */
LOCAL INT seg_near( INT px, INT py, INT x1, INT y1, INT x2, INT y2 )
{
	D	dx = x2 - x1, dy = y2 - y1, t, len2 = dx * dx + dy * dy;
	D	qx, qy;

	if ( len2 == 0 ) {
		qx = x1;
		qy = y1;
	} else {
		t = ( ( px - x1 ) * dx + ( py - y1 ) * dy ) * 1024 / len2;
		if ( t < 0 ) t = 0;
		if ( t > 1024 ) t = 1024;
		qx = x1 + dx * t / 1024;
		qy = y1 + dy * t / 1024;
	}
	qx -= px;
	qy -= py;
	if ( qx < 0 ) qx = -qx;
	if ( qy < 0 ) qy = -qy;

	return (INT)( ( qx > qy ) ? qx : qy );
}

EXPORT ER tv_fig_shape_at( CONST T_TVFIG *f, CONST T_DPRECT *r,
			   INT scroll_x, INT scroll_y, INT x, INT y,
			   INT *p_which )
{
	T_DPPOINT	*pts;
	INT		i, k, n, px, py, z, reach;
	ER		er = E_NOEXS;

	if ( f == NULL || r == NULL ) {
		return E_PAR;
	}
	/* a line is pressed where it is drawn: its ends at the shapes it joins, curved or bent as it runs */
	pts = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * OUT_MAX);
	z = ( f->zoom > 0 ) ? f->zoom : 8;
	px = ( x - r->left + scroll_x ) * 8 / z;
	py = ( y - r->top + scroll_y ) * 8 / z;
	/* how near a line a press takes it: a few pixels of the screen, whatever the zoom */
	reach = ( LINE_REACH * 8 + z - 1 ) / z;
	for ( i = f->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*sh = &f->sh[i];
		BOOL		hit = FALSE;

		if ( sh->kind == TV_SH_GROUP ) {
			continue;
		}
		if ( sh->kind == TV_SH_LINE ) {
			n = ( pts != NULL ) ? tv_line_path(f, sh, pts, OUT_MAX) : 0;
			for ( k = 0; k + 1 < n && !hit; k++ ) {
				hit = (BOOL)( seg_near(px, py, pts[k].x, pts[k].y,
						       pts[k + 1].x, pts[k + 1].y) <= reach );
			}
			for ( k = 0; pts == NULL && k + 1 < sh->npt && !hit; k++ ) {
				hit = (BOOL)( seg_near(px, py, sh->pt[k].x, sh->pt[k].y,
						       sh->pt[k + 1].x,
						       sh->pt[k + 1].y) <= reach );
			}
		} else {
			hit = (BOOL)( px >= sh->r.left && px < sh->r.right
				   && py >= sh->r.top && py < sh->r.bottom );
		}
		if ( hit ) {
			if ( p_which != NULL ) {
				*p_which = i;
			}
			er = E_OK;
			break;
		}
	}
	if ( pts != NULL ) {
		Kfree(pts);
	}

	return er;
}

EXPORT ER tv_fig_draw( INT gid, CONST T_TVFIG *f, CONST T_DPRECT *r,
		       INT scroll_x, INT scroll_y, CONST T_TAD *src,
		       UW paper )
{
	return tv_fig_draw_at(gid, f, r, scroll_x, scroll_y, src, paper, 0);
}
