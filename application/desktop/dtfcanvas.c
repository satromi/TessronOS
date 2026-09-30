/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfcanvas.c
 *	The figure editor's canvas: a picture painted in the figure (design 17.13)
 *
 *	With the canvas in hand, dragging out a box makes a <pixelmap> of
 *	that size: a picture of its own standing in the figure, laid on its
 *	ground colour. Pressing in one -- a new one at once -- paints in it
 *	with the 画材 the panel's canvas button offers: pencil, brush,
 *	eraser, airbrush, paint poured into a region, and the cutter, which
 *	lifts a piece out and carries it. Pressing outside it, or taking
 *	another tool, ends the painting, and the picture is written beside
 *	the record as a PNG under the name the <pixelmap> gives it.
 *
 *	焼き付け paints what stands in front of a picture into it: the
 *	shapes over it are drawn into its pixels, and taken out of the
 *	figure.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/om.h>
#include <ts/img.h>
#include <ts/uuid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include "desktop.h"

/* The picture being painted */
LOCAL DTWIN	*cv_win = NULL;
LOCAL T_TADNODE	*cv_node = NULL;
LOCAL T_DPRECT	cv_box;			/* where it stands in the figure */
LOCAL INT	cv_w, cv_h;
LOCAL UW	*cv_px = NULL;
LOCAL UW	cv_bg;			/* its ground, IMG_CLEAR for none */
LOCAL BOOL	cv_changed = FALSE;
LOCAL BOOL	cv_down = FALSE;
LOCAL INT	cv_lx, cv_ly;		/* where the hand was last, in the picture */
LOCAL UW	cv_seed = 12345;	/* the airbrush's scatter */

/* The cutter: a stretch marked, and a piece of it lifted and carried */
LOCAL BOOL	cut_marked = FALSE;
LOCAL INT	cut_l, cut_t, cut_r, cut_b;
LOCAL UW	*cut_piece = NULL;
LOCAL INT	cut_pw, cut_ph;
LOCAL BOOL	cut_moving = FALSE;
LOCAL INT	cut_gx, cut_gy;		/* where the piece was taken, from its corner */

/* ---------------------------------------------------------------- pixels */

LOCAL void dot( INT x, INT y, UW col )
{
	if ( x >= 0 && y >= 0 && x < cv_w && y < cv_h ) {
		cv_px[(SZ)y * cv_w + x] = col;
	}
}

/* A round dab of a size */
LOCAL void dab( INT x, INT y, INT size, UW col )
{
	INT	r = size / 2, dx, dy;

	if ( size <= 1 ) {
		dot(x, y, col);
		return;
	}
	for ( dy = -r; dy <= r; dy++ ) {
		for ( dx = -r; dx <= r; dx++ ) {
			if ( dx * dx + dy * dy <= r * r + r ) {
				dot(x + dx, y + dy, col);
			}
		}
	}
}

/* Dabs all along a line, so that a quick hand leaves no gaps */
LOCAL void stroke( INT x0, INT y0, INT x1, INT y1, INT size, UW col )
{
	INT	dx = ( x1 > x0 ) ? x1 - x0 : x0 - x1;
	INT	dy = ( y1 > y0 ) ? y1 - y0 : y0 - y1;
	INT	sx = ( x0 < x1 ) ? 1 : -1, sy = ( y0 < y1 ) ? 1 : -1;
	INT	err = dx - dy, guard = 0;

	for ( ;; ) {
		dab(x0, y0, size, col);
		if ( ( x0 == x1 && y0 == y1 ) || ++guard > 10000 ) {
			break;
		}
		{
			INT	e2 = 2 * err;

			if ( e2 > -dy ) { err -= dy; x0 += sx; }
			if ( e2 < dx )  { err += dx; y0 += sy; }
		}
	}
}

/* The airbrush: dots scattered round the hand */
LOCAL void spray( INT x, INT y, INT size, UW col )
{
	INT	r = ( size < 2 ) ? 4 : size * 2, k;

	for ( k = 0; k < r * 2; k++ ) {
		INT	dx, dy;

		cv_seed = cv_seed * 1103515245U + 12345U;
		dx = (INT)( ( cv_seed >> 16 ) % (UW)( 2 * r + 1 ) ) - r;
		cv_seed = cv_seed * 1103515245U + 12345U;
		dy = (INT)( ( cv_seed >> 16 ) % (UW)( 2 * r + 1 ) ) - r;
		if ( dx * dx + dy * dy <= r * r ) {
			dot(x + dx, y + dy, col);
		}
	}
}

/* 絵の具: the colour poured into the region of one colour round a place */
LOCAL void pour( INT x0, INT y0, UW col )
{
	UW	was;
	INT	*stack, n = 0, max = cv_w * cv_h;

	if ( x0 < 0 || y0 < 0 || x0 >= cv_w || y0 >= cv_h ) {
		return;
	}
	was = cv_px[(SZ)y0 * cv_w + x0];
	if ( was == col ) {
		return;
	}
	stack = (INT *)Kmalloc(sizeof(INT) * (SZ)max);
	if ( stack == NULL ) {
		return;
	}
	stack[n++] = y0 * cv_w + x0;
	while ( n > 0 ) {
		INT	p = stack[--n], x = p % cv_w, y = p / cv_w, l, r;

		if ( cv_px[p] != was ) {
			continue;
		}
		/* the run of the row, then the rows above and below it */
		l = x;
		while ( l > 0 && cv_px[(SZ)y * cv_w + l - 1] == was ) l--;
		r = x;
		while ( r < cv_w - 1 && cv_px[(SZ)y * cv_w + r + 1] == was ) r++;
		for ( x = l; x <= r; x++ ) {
			cv_px[(SZ)y * cv_w + x] = col;
			if ( y > 0 && cv_px[(SZ)( y - 1 ) * cv_w + x] == was && n < max ) {
				stack[n++] = ( y - 1 ) * cv_w + x;
			}
			if ( y < cv_h - 1 && cv_px[(SZ)( y + 1 ) * cv_w + x] == was && n < max ) {
				stack[n++] = ( y + 1 ) * cv_w + x;
			}
		}
	}
	Kfree(stack);
}

/* ---------------------------------------------------------------- places */

LOCAL INT zoom( void )
{
	return df_zoom();
}

/* A place on the screen as a place in the picture */
LOCAL BOOL to_picture( DTWIN *d, INT sx, INT sy, INT *px, INT *py )
{
	T_WMWIN		w;
	T_DPRECT	page;

	if ( wm_ref(d->wid, &w) < E_OK ) {
		return FALSE;
	}
	dt_work_rect(d, &page);
	*px = ( sx - w.work.left - page.left + d->scroll_x ) * 8 / zoom() - cv_box.left;
	*py = ( sy - w.work.top - page.top + d->scroll_y ) * 8 / zoom() - cv_box.top;

	return TRUE;
}

/* ---------------------------------------------------------------- in and out */

/* The picture's pixels: what is written beside the record, else its ground */
LOCAL BOOL take_pixels( CONST T_TVSHAPE *s, UW **p_px, INT *p_w, INT *p_h, UW *p_bg )
{
	CONST UW	*src = NULL;
	INT		w = s->r.right - s->r.left, h = s->r.bottom - s->r.top;
	INT		sw = 0, sh = 0, k;
	UW		*px, bg = ( s->back == TAD_COL_NONE ) ? IMG_CLEAR : s->back;

	if ( w < 1 ) w = 1;
	if ( h < 1 ) h = 1;
	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	if ( px == NULL ) {
		return FALSE;
	}
	for ( k = 0; k < w * h; k++ ) {
		px[k] = bg;
	}
	if ( s->href != NULL && om_store_picture(s->href, &src, &sw, &sh) >= E_OK
	  && src != NULL ) {
		if ( sw == w && sh == h ) {
			for ( k = 0; k < w * h; k++ ) {
				if ( src[k] != IMG_CLEAR ) {
					px[k] = src[k];
				}
			}
		} else {
			UW	*sc = img_scale(src, sw, sh, w, h);

			if ( sc != NULL ) {
				for ( k = 0; k < w * h; k++ ) {
					if ( sc[k] != IMG_CLEAR ) {
						px[k] = sc[k];
					}
				}
				Kfree(sc);
			}
		}
	}
	*p_px = px;
	*p_w = w;
	*p_h = h;
	*p_bg = bg;

	return TRUE;
}

LOCAL BOOL same_word( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) {
		return FALSE;
	}
	for ( i = 0; b[i] != 0 && a[i] == (UB)b[i]; i++ ) {
		;
	}

	return (BOOL)( b[i] == 0 && a[i] == 0 );
}

/*
 * The name a new picture is written under: the object's identity, its
 * record, and the next number no picture of the figure has yet.
 */
LOCAL void new_name( DTWIN *d, UB *out )
{
	char		id[48];
	T_TADNODE	*n;
	INT		top = 0, k = 0, i;

	if ( ts_uuid_to_str(&d->id, id, sizeof(id)) < E_OK ) {
		id[0] = 'x';
		id[1] = 0;
	}
	for ( n = tad_walk(d->rec, NULL); n != NULL; n = tad_walk(d->rec, n) ) {
		CONST UB	*h;
		INT		j, v = 0, under = 0;

		if ( n->kind != TAD_ND_ELEM || !same_word(n->name, "pixelmap") ) {
			continue;
		}
		h = tad_attr(n, "href");
		/* "<id>_<record>_<number>.png": the number after the second '_' */
		for ( j = 0; h != NULL && h[j] != 0 && h[j] != '.'; j++ ) {
			if ( h[j] == '_' ) {
				under++;
				v = 0;
			} else if ( under == 2 && h[j] >= '0' && h[j] <= '9' ) {
				v = v * 10 + ( h[j] - '0' );
			}
		}
		if ( under == 2 && v > top ) {
			top = v;
		}
	}
	for ( i = 0; id[i] != 0; i++ ) {
		out[k++] = (UB)id[i];
	}
	out[k++] = '_';
	out[k++] = '0';
	out[k++] = '_';
	{
		char	t[12];
		INT	m = 0, v = top + 1;

		do {
			t[m++] = (char)( '0' + v % 10 );
			v /= 10;
		} while ( v > 0 );
		while ( m > 0 ) {
			out[k++] = (UB)t[--m];
		}
	}
	out[k++] = '.';  out[k++] = 'p';  out[k++] = 'n';  out[k++] = 'g';
	out[k] = 0;
}

/* The picture written beside the record, named by the <pixelmap> */
LOCAL void write_picture( DTWIN *d, T_TADNODE *nd, CONST UW *px, INT w, INT h )
{
	UB	name[96];
	INT	k;

	if ( tad_attr(nd, "href") != NULL && tad_attr(nd, "href")[0] != 0 ) {
		CONST UB	*hr = tad_attr(nd, "href");

		for ( k = 0; hr[k] != 0 && k < 95; k++ ) {
			name[k] = hr[k];
		}
		name[k] = 0;
	} else {
		new_name(d, name);
		(void)tad_set_attr((T_TAD *)d->rec, nd, "href", name);
	}
	(void)om_store_put_picture(name, px, w, h);
	d->dirty = TRUE;
}

/* Painting in the picture of shape i begun */
LOCAL BOOL enter( DTWIN *d, INT i )
{
	CONST T_TVSHAPE	*s = &d->fig->sh[i];

	if ( !take_pixels(s, &cv_px, &cv_w, &cv_h, &cv_bg) ) {
		return FALSE;
	}
	cv_win = d;
	cv_node = s->node;
	cv_box = s->r;
	cv_changed = FALSE;
	cv_down = FALSE;
	cut_marked = FALSE;
	cut_moving = FALSE;
	dt_draw(d);
	wm_composite();

	return TRUE;
}

/* Painting ended: what was painted written, and the picture let go */
EXPORT void df_canvas_leave( void )
{
	DTWIN	*d = cv_win;

	if ( d == NULL ) {
		return;
	}
	cv_win = NULL;
	if ( cut_piece != NULL ) {
		Kfree(cut_piece);
		cut_piece = NULL;
	}
	if ( cv_changed && d->used && cv_node != NULL ) {
		ed_before(d);
		write_picture(d, cv_node, cv_px, cv_w, cv_h);
		ed_changed(d);
	}
	if ( cv_px != NULL ) {
		Kfree(cv_px);
		cv_px = NULL;
	}
	if ( d->used ) {
		dt_draw(d);
		wm_composite();
	}
}

/* A new canvas in the box drawn, painted in at once */
EXPORT void df_canvas_new( DTWIN *d, INT l, INT t, INT r, INT b )
{
	T_TAD		*rec = (T_TAD *)d->rec;
	T_TADNODE	*body = tad_body(rec), *nd;
	INT		i, top = 0;

	if ( body == NULL ) {
		return;
	}
	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->fig->sh[i].z > top ) {
			top = d->fig->sh[i].z;
		}
	}
	ed_before(d);
	nd = tad_elem_new(rec, "pixelmap", body, NULL, TRUE);
	if ( nd == NULL ) {
		return;
	}
	df_attr_put(rec, nd, "left", l);
	df_attr_put(rec, nd, "top", t);
	df_attr_put(rec, nd, "right", r);
	df_attr_put(rec, nd, "bottom", b);
	(void)tad_set_attr(rec, nd, "bgcolor", (CONST UB *)"#ffffff");
	df_attr_put(rec, nd, "rotation", 0);
	(void)tad_set_attr(rec, nd, "flipH", (CONST UB *)"false");
	(void)tad_set_attr(rec, nd, "flipV", (CONST UB *)"false");
	df_attr_put(rec, nd, "zIndex", top + 1);
	df_adopt(d, nd);
	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->fig->sh[i].node == nd ) {
			(void)enter(d, i);
			break;
		}
	}
}

/* ---------------------------------------------------------------- the hand */

/* What the tool does at a place of the picture */
LOCAL void apply( INT x, INT y, BOOL first )
{
	INT	tool = df_paint_tool(), size = df_paint_size();
	UW	col = df_paint_colour() & 0x00FFFFFFU;

	switch ( tool ) {
	case DF_PAINT_PENCIL:
		if ( first ) dot(x, y, col); else stroke(cv_lx, cv_ly, x, y, 1, col);
		break;
	case DF_PAINT_BRUSH:
		if ( first ) dab(x, y, size, col); else stroke(cv_lx, cv_ly, x, y, size, col);
		break;
	case DF_PAINT_ERASER:
		if ( first ) dab(x, y, size, cv_bg); else stroke(cv_lx, cv_ly, x, y, size, cv_bg);
		break;
	case DF_PAINT_AIR:
		spray(x, y, size, col);
		break;
	case DF_PAINT_POUR:
		if ( first ) pour(x, y, col);
		break;
	default:
		return;
	}
	cv_changed = TRUE;
	cv_lx = x;
	cv_ly = y;
}

/* The cutter: a stretch marked by dragging, or a piece carried */
LOCAL void cut_press( INT x, INT y )
{
	if ( cut_marked && x >= cut_l && x <= cut_r && y >= cut_t && y <= cut_b ) {
		/* the piece lifted, and the place it leaves given the ground */
		INT	i, j;

		cut_pw = cut_r - cut_l + 1;
		cut_ph = cut_b - cut_t + 1;
		cut_piece = (UW *)Kmalloc(sizeof(UW) * (SZ)cut_pw * cut_ph);
		if ( cut_piece == NULL ) {
			return;
		}
		for ( j = 0; j < cut_ph; j++ ) {
			for ( i = 0; i < cut_pw; i++ ) {
				cut_piece[j * cut_pw + i] = cv_px[(SZ)( cut_t + j ) * cv_w + cut_l + i];
				cv_px[(SZ)( cut_t + j ) * cv_w + cut_l + i] = cv_bg;
			}
		}
		cut_gx = x - cut_l;
		cut_gy = y - cut_t;
		cut_moving = TRUE;
		cv_changed = TRUE;
		return;
	}
	cut_marked = TRUE;
	cut_l = cut_r = x;
	cut_t = cut_b = y;
	cv_lx = x;
	cv_ly = y;
}

LOCAL void cut_follow( INT x, INT y )
{
	if ( cut_moving ) {
		INT	w = cut_r - cut_l, h = cut_b - cut_t;

		cut_l = x - cut_gx;
		cut_t = y - cut_gy;
		cut_r = cut_l + w;
		cut_b = cut_t + h;
		return;
	}
	cut_l = ( cv_lx < x ) ? cv_lx : x;
	cut_r = ( cv_lx < x ) ? x : cv_lx;
	cut_t = ( cv_ly < y ) ? cv_ly : y;
	cut_b = ( cv_ly < y ) ? y : cv_ly;
	if ( cut_l < 0 ) cut_l = 0;
	if ( cut_t < 0 ) cut_t = 0;
	if ( cut_r >= cv_w ) cut_r = cv_w - 1;
	if ( cut_b >= cv_h ) cut_b = cv_h - 1;
}

LOCAL void cut_release( void )
{
	INT	i, j;

	if ( !cut_moving || cut_piece == NULL ) {
		return;
	}
	/* the piece put down where it was carried to */
	for ( j = 0; j < cut_ph; j++ ) {
		for ( i = 0; i < cut_pw; i++ ) {
			UW	v = cut_piece[j * cut_pw + i];

			if ( v != IMG_CLEAR ) {
				dot(cut_l + i, cut_t + j, v);
			}
		}
	}
	Kfree(cut_piece);
	cut_piece = NULL;
	cut_moving = FALSE;
	cut_marked = FALSE;
}

/*
 * A press with the canvas in hand: in the picture being painted, paint;
 * outside it, painting ends; on another picture, painting in it begins.
 * FALSE when nothing was pressed that is the canvas's, so that a new
 * canvas may be drawn there.
 */
EXPORT BOOL df_canvas_press( DTWIN *d, INT fx, INT fy )
{
	INT	i;

	if ( cv_win == d ) {
		INT	x = fx - cv_box.left, y = fy - cv_box.top;

		if ( x >= 0 && y >= 0 && x < cv_w && y < cv_h ) {
			cv_down = TRUE;
			if ( df_paint_tool() == DF_PAINT_CUTTER ) {
				cut_press(x, y);
			} else {
				apply(x, y, TRUE);
			}
			dt_draw(d);
			wm_composite();
			return TRUE;
		}
		df_canvas_leave();
		return TRUE;
	}
	if ( cv_win != NULL ) {
		df_canvas_leave();
	}
	for ( i = ( d->fig != NULL ) ? d->fig->nsh - 1 : -1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*s = &d->fig->sh[i];

		if ( s->kind == TV_SH_PIXMAP && s->node != NULL
		  && fx >= s->r.left && fx < s->r.right
		  && fy >= s->r.top && fy < s->r.bottom ) {
			return enter(d, i);
		}
	}

	return FALSE;
}

EXPORT BOOL df_canvas_painting( void )
{
	return (BOOL)( cv_win != NULL && cv_down );
}

EXPORT void df_canvas_follow( INT sx, INT sy )
{
	INT	x, y;

	if ( cv_win == NULL || !cv_down || !to_picture(cv_win, sx, sy, &x, &y) ) {
		return;
	}
	if ( df_paint_tool() == DF_PAINT_CUTTER ) {
		cut_follow(x, y);
	} else if ( df_paint_tool() != DF_PAINT_POUR ) {
		apply(x, y, FALSE);
	}
	dt_draw(cv_win);
	wm_composite();
}

EXPORT void df_canvas_release( void )
{
	if ( cv_win == NULL || !cv_down ) {
		return;
	}
	cv_down = FALSE;
	if ( df_paint_tool() == DF_PAINT_CUTTER ) {
		cut_release();
		dt_draw(cv_win);
		wm_composite();
	}
}

EXPORT BOOL df_canvas_active( void )
{
	return (BOOL)( cv_win != NULL );
}

/* The picture being painted, over the figure as it now is */
EXPORT void df_canvas_draw( DTWIN *d, INT gid, CONST T_DPRECT *page )
{
	T_DPRECT	b;
	INT		z = zoom(), x, y;
	T_DPPAT		pat;

	if ( cv_win != d || cv_px == NULL ) {
		return;
	}
	b.left = page->left + cv_box.left * z / 8 - d->scroll_x;
	b.top = page->top + cv_box.top * z / 8 - d->scroll_y;
	b.right = b.left + cv_w * z / 8;
	b.bottom = b.top + cv_h * z / 8;
	dp_fill_rect(gid, &b, ( d->paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK)
							     : d->paper);
	if ( z == 8 ) {
		dp_put_argb(gid, b.left, b.top, cv_px, cv_w, cv_w, cv_h, IMG_CLEAR);
	} else {
		UW	*sc = img_scale(cv_px, cv_w, cv_h, b.right - b.left, b.bottom - b.top);

		if ( sc != NULL ) {
			dp_put_argb(gid, b.left, b.top, sc, b.right - b.left,
				    b.right - b.left, b.bottom - b.top, IMG_CLEAR);
			Kfree(sc);
		}
	}
	/* the piece being carried, over the rest */
	if ( cut_moving && cut_piece != NULL ) {
		for ( y = 0; y < cut_ph; y++ ) {
			for ( x = 0; x < cut_pw; x++ ) {
				UW	v = cut_piece[y * cut_pw + x];

				if ( v != IMG_CLEAR ) {
					dp_put_pixel(gid, b.left + ( cut_l + x ) * z / 8,
						     b.top + ( cut_t + y ) * z / 8, v);
				}
			}
		}
	}
	dp_pat_colour(&pat, 0x000078D7U);
	dp_line_wide(gid, b.left - 1, b.top - 1, b.right, b.top - 1, 1, DP_LINE_DASH, &pat);
	dp_line_wide(gid, b.left - 1, b.bottom, b.right, b.bottom, 1, DP_LINE_DASH, &pat);
	dp_line_wide(gid, b.left - 1, b.top - 1, b.left - 1, b.bottom, 1, DP_LINE_DASH, &pat);
	dp_line_wide(gid, b.right, b.top - 1, b.right, b.bottom, 1, DP_LINE_DASH, &pat);
	if ( cut_marked ) {
		T_DPPAT	cp;

		dp_pat_colour(&cp, 0x00FF8F00U);
		dp_line_wide(gid, b.left + cut_l * z / 8, b.top + cut_t * z / 8,
			     b.left + cut_r * z / 8, b.top + cut_t * z / 8, 1, DP_LINE_DASH, &cp);
		dp_line_wide(gid, b.left + cut_l * z / 8, b.top + cut_b * z / 8,
			     b.left + cut_r * z / 8, b.top + cut_b * z / 8, 1, DP_LINE_DASH, &cp);
		dp_line_wide(gid, b.left + cut_l * z / 8, b.top + cut_t * z / 8,
			     b.left + cut_l * z / 8, b.top + cut_b * z / 8, 1, DP_LINE_DASH, &cp);
		dp_line_wide(gid, b.left + cut_r * z / 8, b.top + cut_t * z / 8,
			     b.left + cut_r * z / 8, b.top + cut_b * z / 8, 1, DP_LINE_DASH, &cp);
	}
}

/* A window gone: a picture being painted in it is let go, not written */
EXPORT void df_canvas_forget( DTWIN *d )
{
	if ( cv_win == d ) {
		cv_win = NULL;
		if ( cv_px != NULL ) {
			Kfree(cv_px);
			cv_px = NULL;
		}
		if ( cut_piece != NULL ) {
			Kfree(cut_piece);
			cut_piece = NULL;
		}
	}
}

/* ---------------------------------------------------------------- 焼き付け */

/*
 * The one picture picked has what stands in front of it and over it
 * drawn into its pixels: those shapes are drawn into the picture as
 * the figure draws them, the picture is written, and they are taken out
 * of the figure. A virtual object is not drawn into a picture.
 */
EXPORT void df_burn( DTWIN *d )
{
	T_TVFIG		*f = d->fig, *part;
	INT		i, pm = -1, gid;
	UW		*px, bg;
	INT		w, h;
	T_DPRECT	all;

	if ( f == NULL || d->sealed ) {
		return;
	}
	for ( i = 0; i < f->nsh; i++ ) {
		if ( d->pick[i] != 0 ) {
			if ( pm >= 0 || f->sh[i].kind != TV_SH_PIXMAP ) {
				dt_tell(d, "ピクセルマップを1つ選択してください", NULL);
				return;
			}
			pm = i;
		}
	}
	if ( pm < 0 ) {
		dt_tell(d, "ピクセルマップを選択してください", NULL);
		return;
	}
	df_canvas_leave();
	f = d->fig;
	if ( f == NULL || pm >= f->nsh || !take_pixels(&f->sh[pm], &px, &w, &h, &bg) ) {
		return;
	}
	/* what is in front of it and over it: the shapes after it */
	part = (T_TVFIG *)Kmalloc(sizeof(T_TVFIG));
	if ( part == NULL ) {
		Kfree(px);
		return;
	}
	*part = *f;
	part->sh = (T_TVSHAPE *)Kmalloc(sizeof(T_TVSHAPE) * TV_MAX_SHAPE);
	if ( part->sh == NULL ) {
		Kfree(part);
		Kfree(px);
		return;
	}
	part->nsh = 0;
	part->zoom = 8;
	part->over_on = 0;		/* the paper's overlays are not burnt in */
	for ( i = pm + 1; i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];

		if ( s->kind == TV_SH_LINK || s->kind == TV_SH_GROUP
		  || s->r.right <= f->sh[pm].r.left || s->r.left >= f->sh[pm].r.right
		  || s->r.bottom <= f->sh[pm].r.top || s->r.top >= f->sh[pm].r.bottom ) {
			continue;
		}
		part->sh[part->nsh++] = *s;
	}
	if ( part->nsh == 0 ) {
		Kfree(part->sh);
		Kfree(part);
		Kfree(px);
		dt_tell(d, "焼き付ける図形がありません", NULL);
		return;
	}
	/* drawn into the picture's own pixels */
	gid = dp_open();
	if ( gid >= 0 ) {
		all.left = 0;  all.top = 0;  all.right = w;  all.bottom = h;
		dp_set_target(gid, px, (UINT)w * 4, 0, 0);
		dp_set_origin(gid, 0, 0);
		dp_set_frame(gid, &all);
		dp_set_visible(gid, &all);
		(void)tv_fig_draw_at(gid, part, &all, f->sh[pm].r.left,
				     f->sh[pm].r.top, d->rec, TAD_COL_NONE, 0);
		dp_close(gid);
	}
	/* written, and the shapes drawn into it taken out */
	ed_before(d);
	write_picture(d, f->sh[pm].node, px, w, h);
	for ( i = 0; i < part->nsh; i++ ) {
		if ( part->sh[i].node != NULL && part->sh[i].node->parent != NULL ) {
			tad_node_remove(part->sh[i].node);
		}
	}
	Kfree(part->sh);
	Kfree(part);
	Kfree(px);
	ed_pick_none(d);
	ed_changed(d);
}
