/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figcanvas.c
 *	The figure editor's canvas apart from its windows: a picture painted
 *	in the figure, and 焼き付け (design 17.13)
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
 *
 *	The pixels are read and written through the view's host (pic_get,
 *	pic_put), the record is changed through the figure editor's host
 *	(T_FGHOST), so that the desktop's figure editor and the one running
 *	as a process paint alike (include/ts/figtool.h).
 */

#include <ts/tvport.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/img.h>
#include <ts/uuid.h>
#include <ts/figedit.h>
#include <ts/docedit.h>
#include <ts/figtool.h>

#define fl	ft_look

/* The picture being painted */
LOCAL void	*cv_arg = NULL;		/* the view's arg it is painted in, NULL none */
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

/* ---------------------------------------------------------------- in and out */

/* The figure drawn again and put on the screen */
LOCAL void shown( T_FTVIEW *v )
{
	v->host->draw(v);
	v->host->show(v);
}

/* The panel's canvas button shows whether a picture is being painted */
LOCAL void tools_shown( T_FTVIEW *v )
{
	if ( v->host->tools_shown != NULL ) {
		v->host->tools_shown();
	}
}

/* The picture's pixels: what is written beside the record, else its ground */
LOCAL BOOL take_pixels( T_FTVIEW *v, CONST T_TVSHAPE *s, UW **p_px, INT *p_w, INT *p_h, UW *p_bg )
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
	if ( s->href != NULL && v->host->pic_get != NULL
	  && v->host->pic_get(v, s->href, &src, &sw, &sh) >= E_OK && src != NULL ) {
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
		if ( v->host->pic_done != NULL ) {
			v->host->pic_done(v, src);
		}
	}
	*p_px = px;
	*p_w = w;
	*p_h = h;
	*p_bg = bg;

	return TRUE;
}

/*
 * The name a new picture is written under: the object's identity, its
 * record, and the next number no picture of the figure has yet.
 */
LOCAL void new_name( T_FTVIEW *v, UB *out )
{
	char		id[48];
	TS_UUID		self;
	T_TADNODE	*n;
	INT		top = 0, k = 0, i;

	if ( v->host->self_id == NULL || !v->host->self_id(v, &self)
	  || ts_uuid_to_str(&self, id, sizeof(id)) < E_OK ) {
		id[0] = 'x';
		id[1] = 0;
	}
	for ( n = tad_walk(v->ed->rec, NULL); n != NULL; n = tad_walk(v->ed->rec, n) ) {
		CONST UB	*h;
		INT		j, val = 0, under = 0;

		if ( n->kind != TAD_ND_ELEM || !ft_same_word(n->name, "pixelmap") ) {
			continue;
		}
		h = tad_attr(n, "href");
		/* "<id>_<record>_<number>.png": the number after the second '_' */
		for ( j = 0; h != NULL && h[j] != 0 && h[j] != '.'; j++ ) {
			if ( h[j] == '_' ) {
				under++;
				val = 0;
			} else if ( under == 2 && h[j] >= '0' && h[j] <= '9' ) {
				val = val * 10 + ( h[j] - '0' );
			}
		}
		if ( under == 2 && val > top ) {
			top = val;
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
		INT	m = 0, val = top + 1;

		do {
			t[m++] = (char)( '0' + val % 10 );
			val /= 10;
		} while ( val > 0 );
		while ( m > 0 ) {
			out[k++] = (UB)t[--m];
		}
	}
	out[k++] = '.';  out[k++] = 'p';  out[k++] = 'n';  out[k++] = 'g';
	out[k] = 0;
}

/* The picture written beside the record, named by the <pixelmap> */
LOCAL void write_picture( T_FTVIEW *v, T_TADNODE *nd, CONST UW *px, INT w, INT h )
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
		new_name(v, name);
		(void)tad_set_attr(v->ed->rec, nd, "href", name);
	}
	if ( v->host->pic_put != NULL ) {
		(void)v->host->pic_put(v, name, px, w, h);
	}
}

/* Painting in the picture of shape i begun */
LOCAL BOOL enter( T_FTVIEW *v, INT i )
{
	CONST T_TVSHAPE	*s = &v->ed->fig->sh[i];

	if ( !take_pixels(v, s, &cv_px, &cv_w, &cv_h, &cv_bg) ) {
		return FALSE;
	}
	cv_arg = v->arg;
	cv_node = s->node;
	cv_box = s->r;
	cv_changed = FALSE;
	cv_down = FALSE;
	cut_marked = FALSE;
	cut_moving = FALSE;
	tools_shown(v);
	shown(v);

	return TRUE;
}

LOCAL void let_go( void )
{
	cv_arg = NULL;
	if ( cut_piece != NULL ) {
		Kfree(cut_piece);
		cut_piece = NULL;
	}
	if ( cv_px != NULL ) {
		Kfree(cv_px);
		cv_px = NULL;
	}
}

/* Painting ended: what was painted written, and the picture let go */
EXPORT void ft_canvas_leave( T_FTVIEW *v )
{
	T_FGED	*e = v->ed;

	if ( cv_arg == NULL || cv_arg != v->arg ) {
		return;
	}
	cv_arg = NULL;
	if ( cut_piece != NULL ) {
		Kfree(cut_piece);
		cut_piece = NULL;
	}
	if ( cv_changed && cv_node != NULL && cv_px != NULL ) {
		e->host->before(e);
		write_picture(v, cv_node, cv_px, cv_w, cv_h);
		e->host->changed(e);
	}
	let_go();
	tools_shown(v);
	shown(v);
}

/* A window gone: a picture being painted in it is let go, not written */
EXPORT void ft_canvas_forget( void *arg )
{
	if ( cv_arg != NULL && cv_arg == arg ) {
		let_go();
	}
}

EXPORT void *ft_canvas_arg( void )
{
	return cv_arg;
}

EXPORT BOOL ft_canvas_painting( void )
{
	return (BOOL)( cv_arg != NULL && cv_down );
}

/* A new canvas in the box drawn, painted in at once */
EXPORT void ft_canvas_new( T_FTVIEW *v, INT l, INT t, INT r, INT b )
{
	T_FGED		*e = v->ed;
	T_TAD		*rec = e->rec;
	T_TADNODE	*body = tad_body(rec), *nd;
	INT		i, top = 0;

	if ( body == NULL ) {
		return;
	}
	for ( i = 0; e->fig != NULL && i < e->fig->nsh; i++ ) {
		if ( e->fig->sh[i].z > top ) {
			top = e->fig->sh[i].z;
		}
	}
	e->host->before(e);
	nd = tad_elem_new(rec, "pixelmap", body, NULL, TRUE);
	if ( nd == NULL ) {
		return;
	}
	fe_attr_put(rec, nd, "left", l);
	fe_attr_put(rec, nd, "top", t);
	fe_attr_put(rec, nd, "right", r);
	fe_attr_put(rec, nd, "bottom", b);
	(void)tad_set_attr(rec, nd, "bgcolor", (CONST UB *)"#ffffff");
	fe_attr_put(rec, nd, "rotation", 0);
	(void)tad_set_attr(rec, nd, "flipH", (CONST UB *)"false");
	(void)tad_set_attr(rec, nd, "flipV", (CONST UB *)"false");
	fe_attr_put(rec, nd, "zIndex", top + 1);
	fe_adopt(e, nd);
	for ( i = 0; e->fig != NULL && i < e->fig->nsh; i++ ) {
		if ( e->fig->sh[i].node == nd ) {
			(void)enter(v, i);
			break;
		}
	}
}

/* ---------------------------------------------------------------- the hand */

/* What the tool does at a place of the picture */
LOCAL void apply( INT x, INT y, BOOL first )
{
	INT	tool = fl.paint_tool, size = fl.paint_size;
	UW	col = fl.stroke & 0x00FFFFFFU;

	switch ( tool ) {
	case FT_PAINT_PENCIL:
		if ( first ) dot(x, y, col); else stroke(cv_lx, cv_ly, x, y, 1, col);
		break;
	case FT_PAINT_BRUSH:
		if ( first ) dab(x, y, size, col); else stroke(cv_lx, cv_ly, x, y, size, col);
		break;
	case FT_PAINT_ERASER:
		if ( first ) dab(x, y, size, cv_bg); else stroke(cv_lx, cv_ly, x, y, size, cv_bg);
		break;
	case FT_PAINT_AIR:
		spray(x, y, size, col);
		break;
	case FT_PAINT_POUR:
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
			UW	val = cut_piece[j * cut_pw + i];

			if ( val != IMG_CLEAR ) {
				dot(cut_l + i, cut_t + j, val);
			}
		}
	}
	Kfree(cut_piece);
	cut_piece = NULL;
	cut_moving = FALSE;
	cut_marked = FALSE;
}

/*
 * A press with the canvas in hand (fx, fy the figure's): in the picture
 * being painted, paint; outside it, painting ends; on another picture,
 * painting in it begins. FALSE when nothing was pressed that is the
 * canvas's, so that a new canvas may be drawn there.
 */
EXPORT BOOL ft_canvas_press( T_FTVIEW *v, INT fx, INT fy )
{
	T_FGED	*e = v->ed;
	INT	i;

	if ( cv_arg != NULL && cv_arg == v->arg ) {
		INT	x = fx - cv_box.left, y = fy - cv_box.top;

		if ( x >= 0 && y >= 0 && x < cv_w && y < cv_h ) {
			cv_down = TRUE;
			if ( fl.paint_tool == FT_PAINT_CUTTER ) {
				cut_press(x, y);
			} else {
				apply(x, y, TRUE);
			}
			shown(v);
			return TRUE;
		}
		ft_canvas_leave(v);
		return TRUE;
	}
	if ( cv_arg != NULL ) {
		return FALSE;		/* painted in another window: that window ends it */
	}
	for ( i = ( e->fig != NULL ) ? e->fig->nsh - 1 : -1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*s = &e->fig->sh[i];

		if ( s->kind == TV_SH_PIXMAP && s->node != NULL
		  && fx >= s->r.left && fx < s->r.right
		  && fy >= s->r.top && fy < s->r.bottom ) {
			return enter(v, i);
		}
	}

	return FALSE;
}

/* A picture pressed twice with the pointer in hand: the canvas taken, and painting in it begun */
EXPORT BOOL ft_canvas_open( T_FTVIEW *v, INT fx, INT fy )
{
	ft_select();
	ft_tool = FT_TL_CANVAS;
	if ( v->host->pointer != NULL ) {
		v->host->pointer(FALSE);
	}
	tools_shown(v);
	return ft_canvas_press(v, fx, fy);
}

/* The hand carried to (x, y) of the window while it paints */
EXPORT void ft_canvas_follow( T_FTVIEW *v, INT x, INT y )
{
	INT	px, py;

	if ( cv_arg == NULL || cv_arg != v->arg || !cv_down ) {
		return;
	}
	px = ( x - v->page.left + v->sx ) * 8 / fl.zoom - cv_box.left;
	py = ( y - v->page.top + v->sy ) * 8 / fl.zoom - cv_box.top;
	if ( fl.paint_tool == FT_PAINT_CUTTER ) {
		cut_follow(px, py);
	} else if ( fl.paint_tool != FT_PAINT_POUR ) {
		apply(px, py, FALSE);
	}
	shown(v);
}

EXPORT void ft_canvas_release( T_FTVIEW *v )
{
	if ( cv_arg == NULL || cv_arg != v->arg || !cv_down ) {
		return;
	}
	cv_down = FALSE;
	if ( fl.paint_tool == FT_PAINT_CUTTER ) {
		cut_release();
		shown(v);
	}
}

/* The picture being painted, over the figure as it now is; paper the colour it is laid on */
EXPORT void ft_canvas_draw( CONST T_FTVIEW *v, UW paper )
{
	T_DPRECT	b;
	INT		z = fl.zoom, x, y, gid = v->gid;
	T_DPPAT		pat;

	if ( cv_arg == NULL || cv_arg != v->arg || cv_px == NULL || gid < 0 ) {
		return;
	}
	b.left = v->page.left + cv_box.left * z / 8 - v->sx;
	b.top = v->page.top + cv_box.top * z / 8 - v->sy;
	b.right = b.left + cv_w * z / 8;
	b.bottom = b.top + cv_h * z / 8;
	dp_fill_rect(gid, &b, paper);
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
				UW	val = cut_piece[y * cut_pw + x];

				if ( val != IMG_CLEAR ) {
					dp_put_pixel(gid, b.left + ( cut_l + x ) * z / 8,
						     b.top + ( cut_t + y ) * z / 8, val);
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

/* ---------------------------------------------------------------- 焼き付け */

LOCAL void tell( T_FTVIEW *v, CONST char *msg )
{
	if ( v->host->tell != NULL ) {
		v->host->tell(v, msg);
	}
}

/*
 * The shapes of 焼き付け drawn into a picture's own pixels, w by h, the
 * figure's (ox, oy) at their corner, through an environment of memory;
 * FALSE when it could not be drawn
 */
LOCAL BOOL burn_into( T_FTVIEW *v, CONST T_TVFIG *part, UW *px, INT w, INT h, INT ox, INT oy )
{
	T_DPBMP		bmp;
	T_DPRECT	all;
	INT		gid;

	bmp.px = px;
	bmp.pitch = w * 4;
	bmp.w = w;
	bmp.h = h;
	gid = dp_open_mem(&bmp);
	if ( gid < 0 ) {
		return FALSE;
	}
	all.left = 0;  all.top = 0;  all.right = w;  all.bottom = h;
	(void)tv_fig_draw_at(gid, part, &all, ox, oy, v->ed->rec, TAD_COL_NONE, 0);

	return (BOOL)( dp_close(gid) >= E_OK );
}

/*
 * The one picture picked has what stands in front of it and over it
 * drawn into its pixels: those shapes are drawn into the picture as
 * the figure draws them, the picture is written,
 * and they are taken out of the figure. A virtual object is not drawn
 * into a picture.
 */
EXPORT void ft_burn( T_FTVIEW *v )
{
	T_FGED		*e = v->ed;
	T_TVFIG		*f = e->fig, *part;
	INT		i, pm = -1;
	UW		*px, bg;
	INT		w, h;

	if ( f == NULL || !e->usable ) {
		return;
	}
	for ( i = 0; i < f->nsh; i++ ) {
		if ( e->pick[i] != 0 ) {
			if ( pm >= 0 || f->sh[i].kind != TV_SH_PIXMAP ) {
				tell(v, "ピクセルマップを1つ選択してください");
				return;
			}
			pm = i;
		}
	}
	if ( pm < 0 ) {
		tell(v, "ピクセルマップを選択してください");
		return;
	}
	ft_canvas_leave(v);
	f = e->fig;
	if ( f == NULL || pm >= f->nsh || !take_pixels(v, &f->sh[pm], &px, &w, &h, &bg) ) {
		return;
	}
	/* what is in front of it and over it: the shapes after it */
	part = (T_TVFIG *)Kmalloc(sizeof(T_TVFIG));
	if ( part == NULL ) {
		Kfree(px);
		e->host->no_memory(e);
		return;
	}
	*part = *f;
	part->sh = (T_TVSHAPE *)Kmalloc(sizeof(T_TVSHAPE) * (SZ)( f->nsh + 1 ));
	if ( part->sh == NULL ) {
		Kfree(part);
		Kfree(px);
		e->host->no_memory(e);
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
		tell(v, "焼き付ける図形がありません");
		return;
	}
	/* drawn into the picture's own pixels */
	if ( !burn_into(v, part, px, w, h, f->sh[pm].r.left, f->sh[pm].r.top) ) {
		Kfree(part->sh);
		Kfree(part);
		Kfree(px);
		return;
	}
	/* written, and the shapes drawn into it taken out */
	e->host->before(e);
	write_picture(v, f->sh[pm].node, px, w, h);
	for ( i = 0; i < part->nsh; i++ ) {
		if ( part->sh[i].node != NULL && part->sh[i].node->parent != NULL ) {
			tad_node_remove(part->sh[i].node);
		}
	}
	Kfree(part->sh);
	Kfree(part);
	Kfree(px);
	e->host->pick_none(e);
	e->host->changed(e);
}
