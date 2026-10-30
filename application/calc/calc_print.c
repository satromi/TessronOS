/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_print.c
 *	The sheet as pages for 印刷 (design 11.13, 11.22.6)
 *
 *	What is printed is the range that holds every cell that is not
 *	empty, from its top left cell. The labels of the rows and columns
 *	and the lines of the grid are not printed; what the cells show is,
 *	with their ground, their frames, their letters in their look, their
 *	links and pictures, and the charts over them.
 *
 *	Each page is a figure of xmlTAD on the sheet's paper (用紙設定), the
 *	range laid from the top left of the part inside the margins. Rows go
 *	on to the next page when the page is full; columns that go past the
 *	right margin are not printed. The letters of a cell are a piece of
 *	text in the figure, so that they are printed as letters.
 *
 *	No system call is made here: the kernel's test uses it as it is.
 */

#include "calc_tad.h"

#define CP_PX(pt)	( ( (pt) * 1333 + 500 ) / 1000 )	/* points as dots at 96 to the inch */
#define CP_DFLT_PX	12

LOCAL INT cp_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}
	return n;
}

LOCAL BOOL cp_same( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

LOCAL BOOL cell_empty( CONST CXCELL *c )
{
	if ( c == NULL ) {
		return TRUE;
	}
	if ( c->kind != CX_TEXT ) {
		return FALSE;
	}
	return (BOOL)( cx_shown(c)[0] == 0 && ( c->st->has & ( CX_S_BG | CX_S_FRAME ) ) == 0 );
}

EXPORT BOOL cx_print_range( CONST CXSHEET *s, CXRANGE *r )
{
	INT	i;
	BOOL	any = FALSE;

	for ( i = 0; i < s->n; i++ ) {
		CONST CXCELL	*c = s->c[i];

		if ( cell_empty(c) ) {
			continue;
		}
		if ( !any ) {
			r->c0 = r->c1 = c->col;
			r->r0 = r->r1 = c->row;
			any = TRUE;
		}
		if ( c->col < r->c0 ) r->c0 = c->col;
		if ( c->col > r->c1 ) r->c1 = c->col;
		if ( c->row < r->r0 ) r->r0 = c->row;
		if ( c->row > r->r1 ) r->r1 = c->row;
	}
	for ( i = 0; any && i < s->nm; i++ ) {
		if ( s->m[i].c0 >= r->c0 && s->m[i].r0 >= r->r0 && s->m[i].c0 <= r->c1 && s->m[i].r0 <= r->r1 ) {
			if ( s->m[i].c1 > r->c1 ) r->c1 = s->m[i].c1;
			if ( s->m[i].r1 > r->r1 ) r->r1 = s->m[i].r1;
		}
	}
	return any;
}

/* The paper and its margins, points, as the sheet's 用紙設定 says (A4 and 20 mm when it does not) */
LOCAL void paper_of( CONST CXSHEET *s, INT *pw, INT *pl, INT *m )
{
	INT	n = ( s->paper != NULL ) ? cp_len((CONST char *)s->paper) : 0;

	*pw = 595;
	*pl = 842;
	m[0] = m[1] = m[2] = m[3] = 57;		/* top, bottom, left, right */
	if ( n > 0 ) {
		*pw = ct_attr_int(s->paper, n, "width", *pw);
		*pl = ct_attr_int(s->paper, n, "length", *pl);
		m[0] = ct_attr_int(s->paper, n, "top", m[0]);
		m[1] = ct_attr_int(s->paper, n, "bottom", m[1]);
		m[2] = ct_attr_int(s->paper, n, "left", m[2]);
		m[3] = ct_attr_int(s->paper, n, "right", m[3]);
	}
}

LOCAL void attr_num( CTBUF *b, CONST char *name, INT v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_num(b, v);
	ct_put(b, "\"");
}

LOCAL void attr_str( CTBUF *b, CONST char *name, CONST char *v )
{
	ct_put(b, " ");
	ct_put(b, name);
	ct_put(b, "=\"");
	ct_put(b, v);
	ct_put(b, "\"");
}

LOCAL void box_attrs( CTBUF *b, INT l, INT t, INT r, INT bt )
{
	attr_num(b, "left", l);
	attr_num(b, "top", t);
	attr_num(b, "right", r);
	attr_num(b, "bottom", bt);
}

/* A cell's ground: a rectangle with no outline */
LOCAL void ground( CTBUF *b, INT l, INT t, INT r, INT bt, CONST char *col )
{
	ct_put(b, "<rect round=\"0\" lineWidth=\"0\"");
	attr_str(b, "fillColor", col);
	box_attrs(b, l, t, r, bt);
	ct_put(b, "/>\n");
}

LOCAL void line( CTBUF *b, INT x0, INT y0, INT x1, INT y1, INT w, CONST char *col, INT type )
{
	ct_put(b, "<line");
	attr_num(b, "lineWidth", w);
	attr_num(b, "lineType", type);
	attr_str(b, "lineColor", ( col != NULL && col[0] == '#' ) ? col : "#000000");
	ct_put(b, " points=\"");
	ct_num(b, x0);
	ct_put(b, ",");
	ct_num(b, y0);
	ct_put(b, " ");
	ct_num(b, x1);
	ct_put(b, ",");
	ct_num(b, y1);
	ct_put(b, "\"/>\n");
}

/* A side of a cell's frame, drawn as the screen draws it: along the cell's edge */
LOCAL void side( CTBUF *b, INT x0, INT y0, INT x1, INT y1, CONST CXSIDE *sd )
{
	INT	w = ( sd->width > 0 ) ? sd->width : 1;
	INT	type = ( sd->type == CX_DASHED ) ? 1 : ( sd->type == CX_DOTTED ) ? 2 : 0;

	if ( w > 8 ) w = 8;
	line(b, x0, y0, x1, y1, w, sd->colour, type);
	if ( sd->type == CX_DOUBLE ) {
		if ( y0 == y1 ) {
			line(b, x0, y0 + w + 1, x1, y1 + w + 1, w, sd->colour, type);
		} else {
			line(b, x0 + w + 1, y0, x1 + w + 1, y1, w, sd->colour, type);
		}
	}
}

/* The letters of a cell, as a piece of text in its box */
LOCAL void words( CTBUF *b, CONST CXCELL *c, INT l, INT t, INT r, INT bt )
{
	CONST CXSTYLE	*st = c->st;
	CONST UB	*w = cx_shown(c);
	INT		px = ( ( st->has & CX_S_SIZE ) && st->size > 0 ) ? CP_PX(st->size) : CP_DFLT_PX;
	INT		top = t + ( bt - t - px - 4 ) / 2;
	CONST char	*col = "#000000";
	INT		d;
	LOCAL CONST char *CONST tag[6] = { "bold", "italic", "underline", "strikethrough", "superscript", "subscript" };
	LOCAL CONST UINT bit[6] = { CX_S_BOLD, CX_S_ITALIC, CX_S_UNDER, CX_S_STRIKE, CX_S_SUPER, CX_S_SUB };

	if ( w[0] == 0 ) {
		return;
	}
	if ( st->has & CX_S_VALIGN ) {
		if ( cp_same(st->valign, "top") ) top = t + 1;
		else if ( cp_same(st->valign, "bottom") ) top = bt - px - 5;
	}
	if ( ( st->has & CX_S_COLOUR ) && st->colour[0] == '#' ) {
		col = st->colour;
	} else if ( w[0] == '#' && c->text[0] == '=' ) {
		col = "#cc0000";			/* a formula's error, as the screen shows it */
	}
	if ( st->has & CX_S_INVERT ) {
		col = ( ( st->has & CX_S_BG ) && st->bg[0] == '#' ) ? st->bg : "#ffffff";
	}
	ct_put(b, "<document>\n<docView");
	attr_num(b, "viewleft", l + 4);
	attr_num(b, "viewtop", top);
	attr_num(b, "viewright", r - 4);
	attr_num(b, "viewbottom", bt);
	ct_put(b, "/>\n<docDraw");
	attr_num(b, "drawleft", l + 4);
	attr_num(b, "drawtop", top);
	attr_num(b, "drawright", r - 4);
	attr_num(b, "drawbottom", bt);
	ct_put(b, "/>\n<docScale hunit=\"-72\" vunit=\"-72\"/>\n<text lang=\"0\" bpat=\"0\"/>\n<font");
	attr_num(b, "size", px);
	ct_put(b, "/>\n<font");
	attr_str(b, "color", col);
	ct_put(b, "/>\n");
	if ( ( st->has & CX_S_FACE ) && st->face[0] != 0 ) {
		ct_put(b, "<font face=\"");
		cx_esc(b, (CONST UB *)st->face, cp_len(st->face));
		ct_put(b, "\"/>\n");
	}
	if ( ( st->has & CX_S_ALIGN ) && ( cp_same(st->align, "center") || cp_same(st->align, "right") ) ) {
		ct_put(b, "<text align=\"");
		ct_put(b, st->align);
		ct_put(b, "\"/>\n");
	}
	for ( d = 5; d >= 0; d-- ) {
		if ( st->has & bit[d] ) {
			ct_put(b, "<");
			ct_put(b, tag[d]);
			ct_put(b, ">");
		}
	}
	cx_esc(b, w, cp_len((CONST char *)w));
	for ( d = 0; d < 6; d++ ) {
		if ( st->has & bit[d] ) {
			ct_put(b, "</");
			ct_put(b, tag[d]);
			ct_put(b, ">");
		}
	}
	ct_put(b, "\n</document>\n");
}

/* A cell's link or picture, from the cell's corner as the screen puts it */
LOCAL void thing( CTBUF *b, CONST CXCELL *c, INT l, INT t, INT r, INT bt )
{
	INT	box[4], w, h;

	if ( c->kind == CX_LINK ) {
		if ( ct_attr_bool(c->el, c->ellen, "hidden", FALSE) ) {
			return;
		}
		w = ct_attr_int(c->el, c->ellen, "width", 0);
		h = ct_attr_int(c->el, c->ellen, "height", 0);
		if ( h <= 0 ) h = ct_attr_int(c->el, c->ellen, "heightPx", 0);
		if ( w <= 0 ) w = r - l;
		if ( h <= 0 ) h = bt - t;
		box[0] = l;
		box[1] = t;
		box[2] = l + w;
		box[3] = t + h;
		{
			INT	vl = ct_attr_int(c->el, c->ellen, "vobjleft", -100000);
			INT	vt = ct_attr_int(c->el, c->ellen, "vobjtop", -100000);

			if ( vl > -100000 && vt > -100000 ) {
				box[0] = l + vl;
				box[1] = t + vt;
				box[2] = l + ct_attr_int(c->el, c->ellen, "vobjright", vl + w);
				box[3] = t + ct_attr_int(c->el, c->ellen, "vobjbottom", vt + h);
			}
		}
		ct_link_put(b, c->el, c->ellen, box, -1, FALSE);
		ct_put(b, "\n");
		return;
	}
	if ( c->kind == CX_IMAGE ) {
		UB	href[160];

		w = ct_attr_int(c->el, c->ellen, "displayWidth", 0);
		h = ct_attr_int(c->el, c->ellen, "displayHeight", 0);
		if ( w <= 0 ) w = ct_attr_int(c->el, c->ellen, "right", 100) - ct_attr_int(c->el, c->ellen, "left", 0);
		if ( h <= 0 ) h = ct_attr_int(c->el, c->ellen, "bottom", 100) - ct_attr_int(c->el, c->ellen, "top", 0);
		if ( ct_attr(c->el, c->ellen, "href", href, sizeof(href)) > 0 ) {
			ct_put(b, "<image");
			box_attrs(b, l, t, l + ( w > 0 ? w : r - l ), t + ( h > 0 ? h : bt - t ));
			ct_put(b, " href=\"");
			cx_esc(b, href, cp_len((char *)href));
			ct_put(b, "\"/>\n");
		}
	}
}

/* The cells of the rows r0..r1 of the range on one page, from (x0, y0) */
LOCAL void page_cells( CTBUF *b, CONST CXSHEET *s, CONST CXRANGE *rg, INT r0, INT r1, INT x0, INT y0,
		       INT width )
{
	INT	row, col, y, x, i;

	/* the grounds and the letters, then the frames over them */
	for ( y = y0, row = r0; row <= r1; y += cx_height(s, row), row++ ) {
		for ( x = x0, col = rg->c0; col <= rg->c1 && x - x0 < width; x += cx_width(s, col), col++ ) {
			CONST CXCELL	*c;
			INT		m = cx_merge_of(s, col, row), w = cx_width(s, col), h = cx_height(s, row);

			if ( m >= 0 ) {
				INT	k;

				if ( s->m[m].c0 != col || s->m[m].r0 != row ) {
					continue;	/* drawn once, at its first cell */
				}
				w = 0;
				h = 0;
				for ( k = s->m[m].c0; k <= s->m[m].c1; k++ ) w += cx_width(s, k);
				for ( k = s->m[m].r0; k <= s->m[m].r1; k++ ) h += cx_height(s, k);
			}
			if ( x + w > x0 + width ) {
				w = x0 + width - x;	/* past the right margin: not printed */
			}
			c = cx_get(s, col, row);
			if ( c == NULL ) {
				continue;
			}
			if ( ( c->st->has & CX_S_BG ) && c->st->bg[0] == '#' ) {
				ground(b, x, y, x + w, y + h, ( c->st->has & CX_S_INVERT ) ? "#000000" : c->st->bg);
			} else if ( c->st->has & CX_S_INVERT ) {
				ground(b, x, y, x + w, y + h, ( c->st->has & CX_S_COLOUR ) ? c->st->colour : "#000000");
			} else if ( c->st->has & CX_S_MESH ) {
				ground(b, x, y, x + w, y + h, "#e0e0e0");
			}
			if ( c->kind == CX_TEXT ) {
				words(b, c, x, y, x + w, y + h);
			} else {
				thing(b, c, x, y, x + w, y + h);
			}
		}
	}
	for ( i = 0; i < s->n; i++ ) {
		CONST CXCELL	*c = s->c[i];
		INT		l, t, r, bt, k;

		if ( c->row < r0 || c->row > r1 || c->col < rg->c0 || c->col > rg->c1
		  || ( c->st->has & CX_S_FRAME ) == 0 ) {
			continue;
		}
		l = x0;
		for ( k = rg->c0; k < c->col; k++ ) l += cx_width(s, k);
		if ( l - x0 >= width ) {
			continue;
		}
		t = y0;
		for ( k = r0; k < c->row; k++ ) t += cx_height(s, k);
		r = l + cx_width(s, c->col);
		bt = t + cx_height(s, c->row);
		if ( c->st->side[CX_TOP].on ) side(b, l, t, r, t, &c->st->side[CX_TOP]);
		if ( c->st->side[CX_BOTTOM].on ) side(b, l, bt, r, bt, &c->st->side[CX_BOTTOM]);
		if ( c->st->side[CX_LEFT].on ) side(b, l, t, l, bt, &c->st->side[CX_LEFT]);
		if ( c->st->side[CX_RIGHT].on ) side(b, r, t, r, bt, &c->st->side[CX_RIGHT]);
	}
}

EXPORT INT cx_print( CONST CXSHEET *s, CONST CXPRINT *pr )
{
	CXRANGE	rg;
	INT	pw, pl, m[4], bw, bh, x0, y0, row, pages = 0, i;
	CTBUF	b;

	if ( !cx_print_range(s, &rg) ) {
		rg.c0 = rg.c1 = rg.r0 = rg.r1 = 1;
		if ( s->nch == 0 ) {
			return 0;		/* nothing to print */
		}
	}
	paper_of(s, &pw, &pl, m);
	/* the part inside the margins, in the sheet's dots */
	x0 = m[2] * 4 / 3;
	y0 = m[0] * 4 / 3;
	bw = ( pw - m[2] - m[3] ) * 4 / 3;
	bh = ( pl - m[0] - m[1] ) * 4 / 3;
	if ( bw < 40 || bh < 40 ) {
		return E_PAR;
	}
	/* the rows a chart reaches belong to the range too */
	for ( i = 0; i < s->nch; i++ ) {
		INT	y = 0, r = 1;

		while ( r < CX_ROWS_MAX && y + cx_height(s, r) <= s->ch[i].bottom ) {
			y += cx_height(s, r);
			r++;
		}
		if ( r > rg.r1 ) rg.r1 = r;
	}
	for ( row = rg.r0; row <= rg.r1; ) {
		INT	r1 = row, h = cx_height(s, row), top = 0, k;

		/* as many rows as fit; a row taller than the page has a page of its own */
		while ( r1 + 1 <= rg.r1 && h + cx_height(s, r1 + 1) <= bh ) {
			r1++;
			h += cx_height(s, r1);
		}
		if ( pr->more != NULL ) {
			ct_buf_grow(&b, 64 * 1024, pr->more);
		} else {
			ct_buf(&b, pr->mem, pr->max);
		}
		ct_put(&b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<paper type=\"fig\"");
		attr_num(&b, "length", pl);
		attr_num(&b, "width", pw);
		attr_num(&b, "top", m[0]);
		attr_num(&b, "bottom", m[1]);
		attr_num(&b, "left", m[2]);
		attr_num(&b, "right", m[3]);
		ct_put(&b, "/>\n<docmargin");
		attr_num(&b, "top", m[0]);
		attr_num(&b, "bottom", m[1]);
		attr_num(&b, "left", m[2]);
		attr_num(&b, "right", m[3]);
		ct_put(&b, "/>\n<figure>\n");
		page_cells(&b, s, &rg, row, r1, x0, y0, bw);
		/* the charts: from the grid's corner, moved as the page's rows are */
		for ( k = rg.r0; k < row; k++ ) top += cx_height(s, k);
		for ( i = 0; pr->chart != NULL && i < s->nch; i++ ) {
			INT	left = 0;

			for ( k = 1; k < rg.c0; k++ ) left += cx_width(s, k);
			if ( s->ch[i].bottom <= top || s->ch[i].top >= top + h ) {
				continue;
			}
			pr->chart(&b, s, i, x0 - left, y0 - top, pr->carg);
		}
		ct_put(&b, "</figure>\n</tad>\n");
		if ( b.fail ) {
			if ( pr->more != NULL ) (void)pr->more(b.p, 0);
			return ( pr->more != NULL ) ? E_NOMEM : E_LIMIT;
		}
		{
			ER	er = pr->put(pr->arg, b.p, b.n);

			if ( pr->more != NULL ) (void)pr->more(b.p, 0);
			if ( er < E_OK ) {
				return er;
			}
		}
		pages++;
		row = r1 + 1;
	}
	return pages;
}
