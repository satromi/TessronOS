/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_draw.c
 *	基本表計算: the grid on the screen (design 11.13)
 *
 *	Along the top of the window are the columns' letters and down the
 *	left the rows' numbers; the rest is the cells, scrolled by the window's
 *	bars, the wheel and the keys. A column is 80 dots wide and a row 22
 *	high unless the sheet says otherwise. A cell is drawn on its ground,
 *	its words in its letters' colour and size, placed as it says, cut at
 *	its right edge with "…"; its frame over the grid's lines. Words with
 *	decorations (bold, italic, underline and the others) are drawn as a
 *	text of a figure by the kernel, and so are the links and images in
 *	the cells and the charts: a figure of each, made for the cells in
 *	sight.
 *
 *	What is taken is tinted, the cell in hand framed, with the fill
 *	handle at the corner of the range; a chart taken has a frame and a
 *	handle at its corner that sizes it.
 */

#include "calc.h"
#include <ts/hid.h>



/* ---------------------------------------------------------------- where things are */

INT grid_left( void )	{ return HEAD_W; }
INT grid_top( void )	{ return HEAD_H; }

INT col_x( INT col )
{
	INT	x = ( col - 1 ) * CX_CELL_W, i;

	for ( i = 0; i < g_s.nw; i++ ) {
		if ( g_s.w[i].at < col ) x += g_s.w[i].size - CX_CELL_W;
	}
	return x;
}

INT row_y( INT row )
{
	INT	y = ( row - 1 ) * CX_CELL_H, i;

	for ( i = 0; i < g_s.nh; i++ ) {
		if ( g_s.h[i].at < row ) y += g_s.h[i].size - CX_CELL_H;
	}
	return y;
}

INT col_at( INT x )
{
	INT	col = ( x >= 0 ) ? x / CX_CELL_W + 1 : 1;

	if ( col > CX_COLS_MAX ) col = CX_COLS_MAX;
	while ( col > 1 && col_x(col) > x ) col--;
	while ( col < CX_COLS_MAX && col_x(col + 1) <= x ) col++;
	return col;
}

INT row_at( INT y )
{
	INT	row = ( y >= 0 ) ? y / CX_CELL_H + 1 : 1;

	if ( row > CX_ROWS_MAX ) row = CX_ROWS_MAX;
	while ( row > 1 && row_y(row) > y ) row--;
	while ( row < CX_ROWS_MAX && row_y(row + 1) <= y ) row++;
	return row;
}

void range_of( CXRANGE *r )
{
	r->c0 = ( g_col < g_acol ) ? g_col : g_acol;
	r->c1 = ( g_col < g_acol ) ? g_acol : g_col;
	r->r0 = ( g_row < g_arow ) ? g_row : g_arow;
	r->r1 = ( g_row < g_arow ) ? g_arow : g_row;
}

/* A cell's box on the screen; a cell of a join is the join's whole box */
BOOL cell_box( INT col, INT row, T_DPRECT *r )
{
	INT	m = cx_merge_of(&g_s, col, row), c1 = col, r1 = row;

	if ( m >= 0 ) {
		col = g_s.m[m].c0;
		row = g_s.m[m].r0;
		c1 = g_s.m[m].c1;
		r1 = g_s.m[m].r1;
	}
	r->left = grid_left() + col_x(col) - g_sx;
	r->top = grid_top() + row_y(row) - g_sy;
	r->right = grid_left() + col_x(c1 + 1) - g_sx;
	r->bottom = grid_top() + row_y(r1 + 1) - g_sy;
	return (BOOL)( m < 0 || ( g_s.m[m].c0 == col && g_s.m[m].r0 == row ) );
}

void link_box( CONST CXCELL *c, T_DPRECT *r )
{
	T_DPRECT	cb;
	INT		w, h, l, t;

	(void)cell_box(c->col, c->row, &cb);
	if ( c->kind == CX_IMAGE ) {
		w = ct_attr_int(c->el, c->ellen, "displayWidth", 0);
		h = ct_attr_int(c->el, c->ellen, "displayHeight", 0);
		if ( w <= 0 ) w = ct_attr_int(c->el, c->ellen, "right", 100) - ct_attr_int(c->el, c->ellen, "left", 0);
		if ( h <= 0 ) h = ct_attr_int(c->el, c->ellen, "bottom", 100) - ct_attr_int(c->el, c->ellen, "top", 0);
		r->left = cb.left;
		r->top = cb.top;
		r->right = cb.left + ( w > 0 ? w : cb.right - cb.left );
		r->bottom = cb.top + ( h > 0 ? h : cb.bottom - cb.top );
		return;
	}
	w = ct_attr_int(c->el, c->ellen, "width", 0);
	h = ct_attr_int(c->el, c->ellen, "height", 0);
	if ( h <= 0 ) h = ct_attr_int(c->el, c->ellen, "heightPx", 0);
	if ( w <= 0 ) w = LINK_W;
	if ( h <= 0 ) h = LINK_H;
	l = ct_attr_int(c->el, c->ellen, "vobjleft", -100000);
	t = ct_attr_int(c->el, c->ellen, "vobjtop", -100000);
	if ( l > -100000 && t > -100000 ) {
		INT	rr = ct_attr_int(c->el, c->ellen, "vobjright", l + w);
		INT	bb = ct_attr_int(c->el, c->ellen, "vobjbottom", t + h);

		r->left = cb.left + l;
		r->top = cb.top + t;
		r->right = cb.left + rr;
		r->bottom = cb.top + bb;
		return;
	}
	r->left = cb.left;
	r->top = cb.top;
	r->right = cb.left + w;
	r->bottom = cb.top + h;
}

void chart_box( INT i, T_DPRECT *r )
{
	CONST CXCHART	*c = &g_s.ch[i];

	/* a chart's place is from the grid's corner under the letters, the rows' numbers in it */
	r->left = c->left - g_sx;
	r->top = grid_top() + c->top - g_sy;
	r->right = c->right - g_sx;
	r->bottom = grid_top() + c->bottom - g_sy;
}

void scroll_to( INT col, INT row )
{
	INT	l = col_x(col), r = col_x(col + 1), t = row_y(row), b = row_y(row + 1);
	INT	vw = g_w - grid_left(), vh = g_h - grid_top();

	if ( l < g_sx ) g_sx = l;
	else if ( r > g_sx + vw && vw > 0 ) g_sx = ( r - l > vw ) ? l : r - vw;
	if ( t < g_sy ) g_sy = t;
	else if ( b > g_sy + vh && vh > 0 ) g_sy = ( b - t > vh ) ? t : b - vh;
	if ( g_sx < 0 ) g_sx = 0;
	if ( g_sy < 0 ) g_sy = 0;
	g_dirty = TRUE;
}

void bars( void )
{
	T_OBWBARS	b;
	SZ		asz = 0;

	memset(&b, 0, sizeof(b));
	b.bar[OB_BAR_R].lo = 0;
	b.bar[OB_BAR_R].hi = row_y(g_s.maxrow + 1);
	b.bar[OB_BAR_R].clo = g_sy;
	b.bar[OB_BAR_R].chi = g_sy + ( g_h - grid_top() );
	b.bar[OB_BAR_B].lo = 0;
	b.bar[OB_BAR_B].hi = col_x(g_s.maxcol + 1);
	b.bar[OB_BAR_B].clo = g_sx;
	b.bar[OB_BAR_B].chi = g_sx + ( g_w - grid_left() );
	if ( b.bar[OB_BAR_R].chi > b.bar[OB_BAR_R].hi ) b.bar[OB_BAR_R].hi = b.bar[OB_BAR_R].chi;
	if ( b.bar[OB_BAR_B].chi > b.bar[OB_BAR_B].hi ) b.bar[OB_BAR_B].hi = b.bar[OB_BAR_B].chi;
	(void)ob_wri_rec(g_kw, OB_WR_BARS, 0, &b, 2 * sizeof(T_OBWBAR), &asz);
}

/* ---------------------------------------------------------------- drawing */

static void fill( INT l, INT t, INT r, INT b, UW c )
{
	T_DPRECT	rc;

	rc.left = l;
	rc.top = t;
	rc.right = r;
	rc.bottom = b;
	if ( r > l && b > t ) (void)dp_fill_rect(g_gid, &rc, c);
}

static void frame( INT l, INT t, INT r, INT b, UW c, INT w )
{
	T_DPRECT	f;

	f.left = l;
	f.top = t;
	f.right = r;
	f.bottom = b;
	if ( r > l && b > t ) (void)dp_frame_rect(g_gid, &f, c, w);
}

/* "#rrggbb" as a colour; dflt for anything else */
static UW colour_of( CONST char *s, UW dflt )
{
	INT	i;
	UW	v = 0;

	if ( s == NULL || s[0] != '#' ) return dflt;
	if ( strlen(s) == 4 ) {
		for ( i = 1; i < 4; i++ ) {
			char	c = s[i];
			INT	d = ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
				  : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;

			if ( d < 0 ) return dflt;
			v = ( v << 8 ) | (UW)( d * 17 );
		}
		return v;
	}
	if ( strlen(s) != 7 ) return dflt;
	return (UW)strtoul(s + 1, NULL, 16);
}

UW colour_value( CONST char *s, UW dflt )
{
	return colour_of(s, dflt);
}

/* A size in points as dots: 1.333 a point */
static INT px_of( INT pt )
{
	return ( pt * 1333 + 500 ) / 1000;
}

static INT cell_px( CONST CXSTYLE *st )
{
	return ( ( st->has & CX_S_SIZE ) && st->size > 0 ) ? px_of(st->size) : 12;
}

/* Words cut at a letter's edge to fit width, with "…" when they were cut */
static INT fit( CONST UB *s, INT px, INT width, char *out, INT max )
{
	INT	n;

	snprintf(out, (size_t)max - 4, "%s", (CONST char *)s);
	for ( n = 0; out[n] != 0; n++ ) {
		if ( out[n] == '\n' || out[n] == '\r' || out[n] == '\t' ) out[n] = ' ';
	}
	if ( dp_text_width((CONST UB *)out, px) <= width ) return dp_text_width((CONST UB *)out, px);
	while ( n > 0 ) {
		do {
			n--;
		} while ( n > 0 && ( out[n] & 0xC0 ) == 0x80 );
		strcpy(out + n, "…");
		if ( dp_text_width((CONST UB *)out, px) <= width ) break;
	}
	if ( n == 0 ) out[0] = 0;
	return dp_text_width((CONST UB *)out, px);
}

/* A side of a frame: its kind drawn as runs */
static void side_line( INT x0, INT y0, INT x1, INT y1, CONST CXSIDE *sd )
{
	UW	c = colour_of(sd->colour, C_TEXT);
	INT	w = ( sd->width > 0 ) ? sd->width : 1, len, step, on, i;
	BOOL	across = (BOOL)( y0 == y1 );

	if ( w > 8 ) w = 8;
	len = across ? x1 - x0 : y1 - y0;
	on = ( sd->type == CX_DASHED ) ? 6 : ( sd->type == CX_DOTTED ) ? 1 : len;
	step = ( sd->type == CX_DASHED ) ? 9 : ( sd->type == CX_DOTTED ) ? 3 : len;
	if ( step <= 0 ) return;
	for ( i = 0; i < len; i += step ) {
		INT	e = ( i + on < len ) ? i + on : len;

		if ( across ) {
			fill(x0 + i, y0, x0 + e, y0 + w, c);
			if ( sd->type == CX_DOUBLE ) fill(x0 + i, y0 + w + 1, x0 + e, y0 + 2 * w + 1, c);
		} else {
			fill(x0, y0 + i, x0 + w, y0 + e, c);
			if ( sd->type == CX_DOUBLE ) fill(x0 + w + 1, y0 + i, x0 + 2 * w + 1, y0 + e, c);
		}
	}
}

static BOOL is_error( CONST UB *s )
{
	return (BOOL)( s[0] == '#' );
}

/* The words of a decorated cell, as a text in the figure */
static void doc_put( CTBUF *b, CONST CXCELL *c, CONST T_DPRECT *r, INT px )
{
	CONST CXSTYLE	*st = c->st;
	CONST UB	*w = cx_shown(c);
	INT		top = r->top + ( r->bottom - r->top - px - 4 ) / 2;
	char		col[CX_COLOUR];

	if ( st->has & CX_S_VALIGN ) {
		if ( strcmp(st->valign, "top") == 0 ) top = r->top + 1;
		else if ( strcmp(st->valign, "bottom") == 0 ) top = r->bottom - px - 5;
	}
	ct_put(b, "<document>\n<docView viewleft=\"");
	ct_num(b, r->left + 4);
	ct_put(b, "\" viewtop=\"");
	ct_num(b, top);
	ct_put(b, "\" viewright=\"");
	ct_num(b, r->right - 4);
	ct_put(b, "\" viewbottom=\"");
	ct_num(b, r->bottom);
	ct_put(b, "\"/>\n<docDraw drawleft=\"");
	ct_num(b, r->left + 4);
	ct_put(b, "\" drawtop=\"");
	ct_num(b, top);
	ct_put(b, "\" drawright=\"");
	ct_num(b, r->right - 4);
	ct_put(b, "\" drawbottom=\"");
	ct_num(b, r->bottom);
	ct_put(b, "\"/>\n<docScale hunit=\"-72\" vunit=\"-72\"/>\n<text lang=\"0\" bpat=\"0\"/>\n<font size=\"");
	ct_num(b, px);
	ct_put(b, "\"/>\n");
	snprintf(col, sizeof(col), "%s", ( st->has & CX_S_COLOUR ) && st->colour[0] ? st->colour
		 : is_error(w) ? "#cc0000" : "#000000");
	if ( st->has & CX_S_INVERT ) snprintf(col, sizeof(col), "%s", ( st->has & CX_S_BG ) ? st->bg : "#ffffff");
	ct_put(b, "<font color=\"");
	ct_put(b, col);
	ct_put(b, "\"/>\n");
	if ( ( st->has & CX_S_FACE ) && st->face[0] != 0 ) {
		ct_put(b, "<font face=\"");
		cx_esc(b, (CONST UB *)st->face, (INT)strlen(st->face));
		ct_put(b, "\"/>\n");
	}
	if ( ( st->has & CX_S_ALIGN ) && ( strcmp(st->align, "center") == 0 || strcmp(st->align, "right") == 0 ) ) {
		ct_put(b, "<text align=\"");
		ct_put(b, st->align);
		ct_put(b, "\"/>\n");
	}
	{
		static CONST char *CONST tag[6] = { "bold", "italic", "underline", "strikethrough", "superscript", "subscript" };
		static CONST UINT bit[6] = { CX_S_BOLD, CX_S_ITALIC, CX_S_UNDER, CX_S_STRIKE, CX_S_SUPER, CX_S_SUB };
		INT	d;

		for ( d = 5; d >= 0; d-- ) {
			if ( st->has & bit[d] ) {
				ct_put(b, "<");
				ct_put(b, tag[d]);
				ct_put(b, ">");
			}
		}
		cx_esc(b, w, (INT)strlen((CONST char *)w));
		for ( d = 0; d < 6; d++ ) {
			if ( st->has & bit[d] ) {
				ct_put(b, "</");
				ct_put(b, tag[d]);
				ct_put(b, ">");
			}
		}
	}
	ct_put(b, "\n</document>\n");
}

/* One cell: its ground, its words, its frame; its link, image and decorated words to the figure */
static void draw_cell( CONST CXCELL *c, INT col, INT row, CONST T_DPRECT *r, CONST CXRANGE *pick, CTBUF *fig )
{
	UW		ground = g_bg, ink = C_TEXT;
	BOOL		in = (BOOL)( g_cells_on && col >= pick->c0 && col <= pick->c1 && row >= pick->r0 && row <= pick->r1 );
	CONST CXSTYLE	*st = ( c != NULL ) ? c->st : NULL;
	INT		px = ( st != NULL ) ? cell_px(st) : 12;

	if ( c != NULL && c->kind == CX_TEXT && is_error(cx_shown(c)) && c->text[0] == '=' ) {
		ground = C_ERR_FILL;
		ink = C_ERR_TEXT;
	}
	if ( c != NULL && c->kind == CX_LINK ) ground = C_LINK_CELL;
	if ( st != NULL && ( st->has & CX_S_BG ) ) ground = colour_of(st->bg, ground);
	if ( st != NULL && ( st->has & CX_S_COLOUR ) ) ink = colour_of(st->colour, ink);
	if ( st != NULL && ( st->has & CX_S_INVERT ) ) {
		UW	t = ground;

		ground = ink;
		ink = t;
	}
	if ( in ) ground = ( col == g_col && row == g_row ) ? C_CUR_FILL : C_RANGE;
	fill(r->left, r->top, r->right - 1, r->bottom - 1, ground);
	if ( st != NULL && ( st->has & CX_S_MESH ) ) {
		INT	y, x;

		for ( y = r->top; y < r->bottom - 1; y += 4 ) {
			for ( x = r->left + ( ( y - r->top ) & 4 ); x < r->right - 1; x += 8 ) fill(x, y, x + 2, y + 2, 0x00B0B0B0U);
		}
	}
	/* the grid's lines */
	fill(r->right - 1, r->top, r->right, r->bottom, C_GRID);
	fill(r->left, r->bottom - 1, r->right, r->bottom, C_GRID);
	if ( c == NULL ) return;

	if ( c->kind == CX_LINK || c->kind == CX_IMAGE ) {
		T_DPRECT	lb;
		INT		box[4];

		if ( c->kind == CX_LINK && !g_show_hidden && ct_attr_bool(c->el, c->ellen, "hidden", FALSE) ) return;
		link_box(c, &lb);
		box[0] = lb.left;
		box[1] = lb.top;
		box[2] = lb.right;
		box[3] = lb.bottom;
		if ( c->kind == CX_LINK ) {
			ct_link_put(fig, c->el, c->ellen, box, -1, FALSE);
		} else {
			UB	href[160];

			if ( ct_attr(c->el, c->ellen, "href", href, sizeof(href)) > 0 ) {
				ct_put(fig, "<image left=\"");
				ct_num(fig, box[0]);
				ct_put(fig, "\" top=\"");
				ct_num(fig, box[1]);
				ct_put(fig, "\" right=\"");
				ct_num(fig, box[2]);
				ct_put(fig, "\" bottom=\"");
				ct_num(fig, box[3]);
				ct_put(fig, "\" href=\"");
				cx_esc(fig, href, (INT)strlen((char *)href));
				ct_put(fig, "\"/>");
			}
		}
		ct_put(fig, "\n");
		return;
	}
	if ( cx_shown(c)[0] == 0 ) return;
	if ( ( st->has & CX_S_ITALIC ) || ( ( st->has & CX_S_FACE ) && st->face[0] != 0 ) ) {
		/* slanted letters and other faces: the kernel's text */
		doc_put(fig, c, r, px);
		return;
	}
	{
		char	w[TEXT_MAX];
		INT	tw, x = r->left + 4, y, lpx = px;

		/* above and below the line: smaller, raised or lowered */
		if ( st->has & ( CX_S_SUPER | CX_S_SUB ) ) lpx = px * 3 / 4;
		tw = fit(cx_shown(c), lpx, r->right - r->left - 8 - ( ( st->has & CX_S_BOLD ) ? 1 : 0 ), w, sizeof(w));
		y = r->top + ( r->bottom - r->top + px ) / 2 - px / 8;
		if ( st->has & CX_S_ALIGN ) {
			if ( strcmp(st->align, "center") == 0 ) x = r->left + ( r->right - r->left - tw ) / 2;
			else if ( strcmp(st->align, "right") == 0 ) x = r->right - 4 - tw;
		}
		if ( st->has & CX_S_VALIGN ) {
			if ( strcmp(st->valign, "top") == 0 ) y = r->top + 2 + px;
			else if ( strcmp(st->valign, "bottom") == 0 ) y = r->bottom - 4;
		}
		if ( st->has & CX_S_SUPER ) y -= px / 3;
		if ( st->has & CX_S_SUB ) y += px / 4;
		if ( w[0] == 0 ) return;
		(void)dp_text(g_gid, x, y, (CONST UB *)w, ink, lpx);
		if ( st->has & CX_S_BOLD ) (void)dp_text(g_gid, x + 1, y, (CONST UB *)w, ink, lpx);
		if ( st->has & CX_S_UNDER ) fill(x, y + 2, x + tw, y + 3, ink);
		if ( st->has & CX_S_STRIKE ) fill(x, y - lpx / 3, x + tw, y - lpx / 3 + 1, ink);
	}
}

/* A cell's frame, over its neighbours' lines */
static void draw_frame( CONST CXCELL *c, CONST T_DPRECT *r )
{
	CONST CXSTYLE	*st = c->st;

	if ( st->side[CX_TOP].on ) side_line(r->left, r->top - 1, r->right, r->top - 1, &st->side[CX_TOP]);
	if ( st->side[CX_BOTTOM].on ) side_line(r->left, r->bottom - 1, r->right, r->bottom - 1, &st->side[CX_BOTTOM]);
	if ( st->side[CX_LEFT].on ) side_line(r->left - 1, r->top, r->left - 1, r->bottom, &st->side[CX_LEFT]);
	if ( st->side[CX_RIGHT].on ) side_line(r->right - 1, r->top, r->right - 1, r->bottom, &st->side[CX_RIGHT]);
}

static void headers( INT c0, INT c1, INT r0, INT r1, CONST CXRANGE *pick )
{
	INT	col, row;
	char	nm[16];

	fill(0, 0, g_w, grid_top(), C_HEAD);
	fill(0, 0, grid_left(), g_h, C_HEAD);
	for ( col = c0; col <= c1; col++ ) {
		INT	l = grid_left() + col_x(col) - g_sx, r = grid_left() + col_x(col + 1) - g_sx, tw;

		if ( g_cells_on && col >= pick->c0 && col <= pick->c1 ) fill(l, 0, r, grid_top() - 1, C_HEAD_PICK);
		fill(r - 1, 0, r, grid_top(), C_HEAD_LINE);
		(void)cx_col_name(col, nm);
		tw = dp_text_width((CONST UB *)nm, 12);
		(void)dp_text(g_gid, l + ( r - l - tw ) / 2, 17, (CONST UB *)nm, C_HEAD_TEXT, 12);
		(void)dp_text(g_gid, l + ( r - l - tw ) / 2 + 1, 17, (CONST UB *)nm, C_HEAD_TEXT, 12);
	}
	for ( row = r0; row <= r1; row++ ) {
		INT	t = grid_top() + row_y(row) - g_sy, b = grid_top() + row_y(row + 1) - g_sy, tw;

		if ( g_cells_on && row >= pick->r0 && row <= pick->r1 ) fill(0, t, grid_left() - 1, b, C_HEAD_PICK);
		fill(0, b - 1, grid_left(), b, C_HEAD_LINE);
		snprintf(nm, sizeof(nm), "%d", row);
		tw = dp_text_width((CONST UB *)nm, 12);
		(void)dp_text(g_gid, ( grid_left() - tw ) / 2, t + ( b - t + 12 ) / 2 - 1, (CONST UB *)nm, C_HEAD_TEXT, 12);
	}
	fill(0, grid_top() - 1, g_w, grid_top(), C_HEAD_LINE);
	fill(grid_left() - 1, 0, grid_left(), g_h, C_HEAD_LINE);
	fill(0, 0, grid_left() - 1, grid_top() - 1, C_CORNER);
}

/* The page's edges, dashed: the paper less its margins, from the grid's corner */
static void page_edges( void )
{
	INT	pw = 595, pl = 842, ml = 57, mr = 57, mt = 57, mb = 57, w, h, k;

	if ( g_s.paper != NULL ) {
		INT	n = (INT)strlen((char *)g_s.paper);

		pw = ct_attr_int(g_s.paper, n, "width", pw);
		pl = ct_attr_int(g_s.paper, n, "length", pl);
		mt = ct_attr_int(g_s.paper, n, "top", mt);
		mb = ct_attr_int(g_s.paper, n, "bottom", mb);
		ml = ct_attr_int(g_s.paper, n, "left", ml);
		mr = ct_attr_int(g_s.paper, n, "right", mr);
	}
	/* points to dots at 96 a inch */
	w = ( pw - ml - mr ) * 4 / 3;
	h = ( pl - mt - mb ) * 4 / 3;
	if ( w < 40 || h < 40 ) return;
	for ( k = 1; grid_left() + w * k - g_sx < g_w; k++ ) {
		INT	x = grid_left() + w * k - g_sx, y;

		if ( x <= grid_left() ) continue;
		for ( y = grid_top(); y < g_h; y += 12 ) fill(x, y, x + 2, y + 8, C_PAGE);
	}
	for ( k = 1; grid_top() + h * k - g_sy < g_h; k++ ) {
		INT	y = grid_top() + h * k - g_sy, x;

		if ( y <= grid_top() ) continue;
		for ( x = grid_left(); x < g_w; x += 12 ) fill(x, y, x + 8, y + 2, C_PAGE);
	}
}

void paint( void )
{
	CXRANGE		pick;
	CTBUF		fig;
	T_DPRECT	clip, r;
	INT		c0, c1, r0, r1, col, row, i;

	if ( g_gid < 0 ) return;
	range_of(&pick);
	fill(0, 0, g_w, g_h, g_bg);
	c0 = col_at(g_sx);
	c1 = col_at(g_sx + g_w - grid_left());
	r0 = row_at(g_sy);
	r1 = row_at(g_sy + g_h - grid_top());
	if ( c1 > g_s.maxcol ) c1 = g_s.maxcol;
	if ( r1 > g_s.maxrow ) r1 = g_s.maxrow;

	/* what is drawn as a figure, as long as it comes to */
	ct_buf_grow(&fig, 64 * 1024, ct_more);
	ct_put(&fig, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n");

	/* the cells in sight, a row at a time; a join is drawn at its corner */
	for ( row = r0; row <= r1; row++ ) {
		for ( col = c0; col <= c1; col++ ) {
			INT	m = cx_merge_of(&g_s, col, row);

			if ( m >= 0 ) {
				INT	oc = g_s.m[m].c0, orr = g_s.m[m].r0;

				/* once, at the first of its cells in sight */
				if ( col != ( oc > c0 ? oc : c0 ) || row != ( orr > r0 ? orr : r0 ) ) continue;
				(void)cell_box(oc, orr, &r);
				draw_cell(cx_get(&g_s, oc, orr), oc, orr, &r, &pick, &fig);
				continue;
			}
			(void)cell_box(col, row, &r);
			draw_cell(cx_get(&g_s, col, row), col, row, &r, &pick, &fig);
		}
	}
	/* the frames over the lines */
	for ( i = 0; i < g_s.n; i++ ) {
		CONST CXCELL	*c = g_s.c[i];

		if ( c->row < r0 || c->row > r1 || c->col < c0 || c->col > c1 ) continue;
		if ( ( c->st->has & CX_S_FRAME ) == 0 ) continue;
		(void)cell_box(c->col, c->row, &r);
		draw_frame(c, &r);
	}
	ct_put(&fig, "</figure>\n</tad>\n");
	clip.left = grid_left();
	clip.top = grid_top();
	clip.right = g_w;
	clip.bottom = g_h;
	if ( !fig.fail ) (void)dp_draw_tad(g_gid, &clip, 0, 0, fig.p, fig.n);
	(void)ct_more(fig.p, 0);

	/* the charts */
	for ( i = 0; i < g_s.nch; i++ ) {
		chart_draw(i);
		if ( i == g_chart ) {
			chart_box(i, &r);
			frame(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, C_PICK, 2);
			fill(r.right - 8, r.bottom - 8, r.right, r.bottom, C_PICK);
		}
	}
	if ( g_paper_frame ) page_edges();

	/* what is taken */
	if ( g_cells_on ) {
		T_DPRECT	a, b;

		(void)cell_box(pick.c0, pick.r0, &a);
		(void)cell_box(pick.c1, pick.r1, &b);
		if ( pick.c0 != pick.c1 || pick.r0 != pick.r1 ) frame(a.left - 1, a.top - 1, b.right, b.bottom, C_PICK, 1);
		(void)cell_box(g_col, g_row, &r);
		frame(r.left - 1, r.top - 1, r.right, r.bottom, C_PICK, 2);
		fill(b.right - FILL_BOX / 2 - 1, b.bottom - FILL_BOX / 2 - 1, b.right + FILL_BOX / 2 - 1,
		     b.bottom + FILL_BOX / 2 - 1, C_PICK);
		if ( g_link_on ) {
			CONST CXCELL	*c = cx_get(&g_s, g_col, g_row);

			if ( c != NULL && c->kind != CX_TEXT ) {
				link_box(c, &r);
				frame(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, C_ORANGE, 2);
			}
		}
	}
	headers(c0, c1, r0, r1, &pick);
	ed_draw();
	(void)wm_obj_flush(g_kw, NULL);
	panel_draw();			/* the panel follows the cell in hand and what is typed */
	g_dirty = FALSE;
}
