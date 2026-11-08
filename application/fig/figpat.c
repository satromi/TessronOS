/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figpat.c
 *	パターン編集 apart from its window: a figure's pattern, dot by dot
 *	(design 17.13)
 *
 *	A pattern is sixteen by sixteen dots, each a colour or nothing. The
 *	editor shows it large, to be drawn in, and at its own size laid
 *	over and over. What is drawn is put into the figure with 入替 under
 *	the pattern's number: the numbers 2 to 127 are the fixed patterns,
 *	and a figure may draw one of them its own way; from 128 on they are
 *	the figure's alone. 読込 takes another pattern to start from, and
 *	復旧 takes back the last change.
 *
 *	In the record a pattern is its colours, each with the mask of the
 *	dots it covers: a <pattern> naming its colours and masks, and a
 *	<mask> of sixteen rows for each colour, both in <patterns>.
 *
 *	What is here is the editor's dots, what they are drawn as and how
 *	they go into the record (through the figure editor's host, T_FGHOST),
 *	so that the desktop's window and the one of the figure editor running
 *	as a process (application/fig/fg_pat.c) edit and draw alike; the
 *	window, its panel of parts and its menu are theirs.
 */

#include <ts/tvport.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/figedit.h>
#include <ts/docedit.h>
#include <ts/figtool.h>

#define NONE		TAD_COL_NONE
#define CELL		FP_CELL
#define GX		FP_GX
#define GY		FP_GY
#define RX		FP_RX

/* The colours a dot may be drawn in */
EXPORT CONST UW	fp_cols[FP_NCOL] = {
	0x000000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xEE0000, 0xFF8C00, 0xFFFF00,
	0x008000, 0x7FFF00, 0x00FFFF, 0x0000FF, 0x800080, 0xFF69B4, 0x8B4513,
	0x000080, 0x006400
};

LOCAL void text_at( INT gid, INT x, INT y, INT size, CONST char *s, UW col )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, size);
		fn_draw(gid, fid, x, y, (CONST UB *)s, col);
	}
}

/* ---------------------------------------------------------------- the pattern's dots */

EXPORT void fp_new( T_FPED *p )
{
	knl_memset(p, 0, sizeof(*p));
	p->col = 0x000000;
	p->tool = FP_T_PENCIL;
	p->id = 2;
}

/* A pattern's dots, from the figure's own or the fixed ones */
EXPORT void fp_load( T_FPED *p, CONST T_TVFIG *fig, INT id )
{
	CONST T_TVPAT	*own = ( fig != NULL ) ? tv_fig_pattern(fig, id) : NULL;
	UW		col;
	CONST UH	*tex = NULL;
	INT		x, y;

	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			p->px[y][x] = NONE;
		}
	}
	if ( own != NULL ) {
		for ( y = 0; y < 16; y++ ) {
			for ( x = 0; x < 16; x++ ) {
				if ( ( own->mask[y] & ( 0x80000000U >> x ) ) != 0 ) {
					p->px[y][x] = own->tile[y * 16 + x] & 0x00FFFFFFU;
				}
			}
		}
		return;
	}
	if ( id <= 0 || !tv_pattern(id, &col, &tex) ) {
		return;
	}
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			if ( tex == NULL || ( tex[y] & ( 0x8000U >> x ) ) != 0 ) {
				p->px[y][x] = col;
			}
		}
	}
}

EXPORT void fp_keep( T_FPED *p )
{
	knl_memcpy(p->undo, p->px, sizeof(p->px));
}

/* The dots moved round by one, those that go off one side coming on at the other */
LOCAL void shift( T_FPED *p, INT dx, INT dy )
{
	UW	t[16][16];
	INT	x, y;

	fp_keep(p);
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			t[( y + dy + 16 ) % 16][( x + dx + 16 ) % 16] = p->px[y][x];
		}
	}
	knl_memcpy(p->px, t, sizeof(t));
}

/* Turned a quarter, right (1) or left (-1); or turned over */
LOCAL void turn( T_FPED *p, INT how )
{
	UW	t[16][16];
	INT	x, y;

	fp_keep(p);
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			switch ( how ) {
			case 1:  t[x][15 - y] = p->px[y][x];	break;
			case -1: t[15 - x][y] = p->px[y][x];	break;
			case 2:  t[y][15 - x] = p->px[y][x];	break;
			default: t[15 - y][x] = p->px[y][x];	break;
			}
		}
	}
	knl_memcpy(p->px, t, sizeof(t));
}

/* 展開: the dots joined to one, of its colour, all given the colour drawn in */
LOCAL void flood( T_FPED *p, INT x0, INT y0 )
{
	UW	was = p->px[y0][x0];
	INT	stack[256][2], n = 0;

	if ( was == p->col ) {
		return;
	}
	stack[n][0] = x0;
	stack[n][1] = y0;
	n++;
	while ( n > 0 ) {
		INT	x, y;

		n--;
		x = stack[n][0];
		y = stack[n][1];
		if ( x < 0 || x > 15 || y < 0 || y > 15 || p->px[y][x] != was ) {
			continue;
		}
		p->px[y][x] = p->col;
		if ( n + 4 <= 256 ) {
			stack[n][0] = x + 1;  stack[n][1] = y;  n++;
			stack[n][0] = x - 1;  stack[n][1] = y;  n++;
			stack[n][0] = x;  stack[n][1] = y + 1;  n++;
			stack[n][0] = x;  stack[n][1] = y - 1;  n++;
		}
	}
}

EXPORT void fp_act( T_FPED *p, INT what )
{
	if ( what == FP_N_NONE ) {
		p->col = NONE;
	} else if ( what >= FP_N_MOVE && what < FP_N_MOVE + 4 ) {
		CONST INT	dx[4] = { -1, 0, 0, 1 }, dy[4] = { 0, -1, 1, 0 };

		shift(p, dx[what - FP_N_MOVE], dy[what - FP_N_MOVE]);
	} else if ( what >= FP_N_TURN && what < FP_N_TURN + 4 ) {
		CONST INT	how[4] = { 1, -1, 2, 3 };

		turn(p, how[what - FP_N_TURN]);
	} else if ( what == FP_N_UNDO ) {
		UW	t[16][16];

		knl_memcpy(t, p->px, sizeof(t));
		knl_memcpy(p->px, p->undo, sizeof(t));
		knl_memcpy(p->undo, t, sizeof(t));
	}
}

EXPORT BOOL fp_press( T_FPED *p, CONST T_TVFIG *fig, INT x, INT y )
{
	if ( p->choosing ) {
		INT	cx = ( x - GX ) / CELL, cy = ( y - GY - 20 ) / CELL;

		if ( x >= GX && y >= GY + 20 && cx < 16 && cy < 8 && fig != NULL ) {
			fp_keep(p);
			p->id = cy * 16 + cx;
			if ( p->id < 2 ) {
				p->id = 2;
			}
			fp_load(p, fig, p->id);
			p->choosing = FALSE;
			return TRUE;
		}
		return FALSE;
	}
	if ( x >= GX && y >= GY && x < GX + 16 * CELL && y < GY + 16 * CELL ) {
		INT	px = ( x - GX ) / CELL, py = ( y - GY ) / CELL;

		p->down = TRUE;
		switch ( p->tool ) {
		case FP_T_PICK:
			p->col = p->px[py][px];
			break;
		case FP_T_FILL:
			fp_keep(p);
			flood(p, px, py);
			break;
		case FP_T_RECT:
			fp_keep(p);
			p->rx0 = px;
			p->ry0 = py;
			p->px[py][px] = p->col;
			break;
		default:
			fp_keep(p);
			p->px[py][px] = p->col;
			break;
		}
	}
	return FALSE;
}

EXPORT void fp_drag( T_FPED *p, INT x, INT y )
{
	INT	px = ( x - GX ) / CELL, py = ( y - GY ) / CELL;

	if ( p->choosing || x < GX || y < GY || px > 15 || py > 15 ) {
		return;
	}
	if ( p->tool == FP_T_PENCIL ) {
		p->px[py][px] = p->col;
	} else if ( p->tool == FP_T_RECT ) {
		INT	l = ( p->rx0 < px ) ? p->rx0 : px, r = ( p->rx0 < px ) ? px : p->rx0;
		INT	t = ( p->ry0 < py ) ? p->ry0 : py, b = ( p->ry0 < py ) ? py : p->ry0;
		INT	i, j;

		knl_memcpy(p->px, p->undo, sizeof(p->px));
		for ( j = t; j <= b; j++ ) {
			for ( i = l; i <= r; i++ ) {
				p->px[j][i] = p->col;
			}
		}
	}
}

/* ---------------------------------------------------------------- into the record */

LOCAL INT put_num( UB *buf, INT at, INT v )
{
	char	t[12];
	INT	k = 0;

	if ( v < 0 ) {
		buf[at++] = '-';
		v = -v;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 ) {
		buf[at++] = (UB)t[--k];
	}
	buf[at] = 0;

	return at;
}

LOCAL INT put_hex( UB *buf, INT at, UW v, INT digits )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	for ( i = digits - 1; i >= 0; i-- ) {
		buf[at++] = (UB)hex[( v >> ( i * 4 ) ) & 0xF];
	}
	buf[at] = 0;

	return at;
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
 * 入替: the pattern put into the figure under its number, in place of
 * what the figure had under that number. A colour of its own each, with
 * a new mask of the dots it covers.
 */
EXPORT BOOL fp_replace( CONST T_FPED *p, T_FGED *e )
{
	T_TAD		*rec;
	T_TADNODE	*body, *pats = NULL, *n, *next, *pe;
	UW		cols[16];
	UB		*fg, *mk, num[16];
	INT		ncol = 0, x, y, c, top_mask = 127, at_fg = 0, at_mk = 0;

	if ( e == NULL || e->rec == NULL || !e->usable ) {
		return FALSE;
	}
	body = tad_body(e->rec);
	if ( body == NULL ) {
		return FALSE;
	}
	/* the colours used */
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			UW	v = p->px[y][x];

			if ( v == NONE ) {
				continue;
			}
			for ( c = 0; c < ncol && cols[c] != v; c++ ) {
				;
			}
			if ( c == ncol && ncol < 16 ) {
				cols[ncol++] = v;
			}
		}
	}
	fg = (UB *)Kmalloc(16 * 8 + 2);
	mk = (UB *)Kmalloc(16 * 12 + 2);
	if ( fg == NULL || mk == NULL ) {
		if ( fg != NULL ) Kfree(fg);
		if ( mk != NULL ) Kfree(mk);
		return FALSE;
	}
	e->host->before(e);
	rec = e->rec;
	body = tad_body(rec);
	for ( n = ( body != NULL ) ? body->first : NULL; n != NULL; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "patterns") ) {
			pats = n;
			break;
		}
	}
	if ( pats == NULL && body != NULL ) {
		T_TADNODE	*at = body->first;

		while ( at != NULL && ( at->kind != TAD_ND_ELEM
			|| same_word(at->name, "figView")
			|| same_word(at->name, "figDraw")
			|| same_word(at->name, "figScale") ) ) {
			at = at->next;
		}
		pats = tad_elem_new(rec, "patterns", body, at, FALSE);
	}
	if ( pats == NULL ) {
		Kfree(fg);
		Kfree(mk);
		return FALSE;
	}
	/* the old one of that number out, and the highest mask number seen */
	for ( n = pats->first; n != NULL; n = next ) {
		next = n->next;
		if ( n->kind != TAD_ND_ELEM ) {
			continue;
		}
		if ( same_word(n->name, "mask") ) {
			CONST UB	*v = tad_attr(n, "id");
			INT		k = 0, id = 0;

			while ( v != NULL && v[k] >= '0' && v[k] <= '9' ) {
				id = id * 10 + ( v[k++] - '0' );
			}
			if ( id > top_mask ) {
				top_mask = id;
			}
		} else if ( same_word(n->name, "pattern") ) {
			CONST UB	*v = tad_attr(n, "id");
			INT		k = 0, id = 0;

			while ( v != NULL && v[k] >= '0' && v[k] <= '9' ) {
				id = id * 10 + ( v[k++] - '0' );
			}
			if ( id == p->id ) {
				tad_node_remove(n);
			}
		}
	}
	/* a mask for each colour, then the pattern naming them */
	fg[0] = 0;
	mk[0] = 0;
	for ( c = 0; c < ncol; c++ ) {
		T_TADNODE	*m = tad_elem_new(rec, "mask", pats, NULL, TRUE);
		UB		data[16 * 5 + 2];
		INT		at = 0;

		if ( m == NULL ) {
			continue;
		}
		for ( y = 0; y < 16; y++ ) {
			UW	row = 0;

			for ( x = 0; x < 16; x++ ) {
				if ( p->px[y][x] == cols[c] ) {
					row |= 0x8000U >> x;
				}
			}
			if ( y > 0 ) {
				data[at++] = ',';
			}
			at = put_hex(data, at, row, 4);
		}
		(void)put_num(num, 0, top_mask + 1 + c);
		(void)tad_set_attr(rec, m, "id", num);
		(void)tad_set_attr(rec, m, "type", (CONST UB *)"0");
		(void)tad_set_attr(rec, m, "width", (CONST UB *)"16");
		(void)tad_set_attr(rec, m, "height", (CONST UB *)"16");
		(void)tad_set_attr(rec, m, "data", data);
		if ( c > 0 ) {
			fg[at_fg++] = ',';
			mk[at_mk++] = ',';
		}
		fg[at_fg++] = '#';
		at_fg = put_hex(fg, at_fg, cols[c], 6);
		at_mk = put_num(mk, at_mk, top_mask + 1 + c);
	}
	pe = tad_elem_new(rec, "pattern", pats, NULL, TRUE);
	if ( pe != NULL ) {
		(void)put_num(num, 0, p->id);
		(void)tad_set_attr(rec, pe, "id", num);
		(void)tad_set_attr(rec, pe, "type", (CONST UB *)"0");
		(void)tad_set_attr(rec, pe, "width", (CONST UB *)"16");
		(void)tad_set_attr(rec, pe, "height", (CONST UB *)"16");
		(void)put_num(num, 0, ncol);
		(void)tad_set_attr(rec, pe, "ncol", num);
		(void)tad_set_attr(rec, pe, "fgcolors", fg);
		(void)tad_set_attr(rec, pe, "bgcolor", (CONST UB *)"transparent");
		(void)tad_set_attr(rec, pe, "masks", mk);
	}
	Kfree(fg);
	Kfree(mk);
	e->host->changed(e);

	return TRUE;
}

/* ---------------------------------------------------------------- what it is drawn as */

LOCAL void cell_colour( INT gid, CONST T_DPRECT *c, UW col )
{
	if ( col == NONE ) {
		/* nothing: a check of light and lighter */
		T_DPRECT	h = *c;

		dp_fill_rect(gid, c, 0x00FFFFFFU);
		h.right = ( c->left + c->right ) / 2;
		h.bottom = ( c->top + c->bottom ) / 2;
		dp_fill_rect(gid, &h, 0x00E0E0E0U);
		h.left = h.right;  h.right = c->right;
		h.top = h.bottom;  h.bottom = c->bottom;
		dp_fill_rect(gid, &h, 0x00E0E0E0U);
		return;
	}
	dp_fill_rect(gid, c, col);
}

/* A rectangle filled with a pattern, over white */
LOCAL void pat_rect( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat )
{
	T_DPPOINT	q[4];

	q[0].x = r->left;   q[0].y = r->top;
	q[1].x = r->right;  q[1].y = r->top;
	q[2].x = r->right;  q[2].y = r->bottom;
	q[3].x = r->left;   q[3].y = r->bottom;
	(void)dp_fill_poly(gid, q, 4, DP_POLY_ODD, pat);
}

EXPORT void fp_draw( CONST T_FPED *p, INT gid, CONST T_TVFIG *fig )
{
	INT		x, y, k;
	T_DPRECT	c;
	char		txt[24];
	UB		num[12];

	if ( p->choosing ) {
		/* the patterns to choose from, sixteen across */
		text_at(gid, GX, GY + 10, 12, "読み込むパターンを選んでください", 0);
		for ( k = 0; k < TV_PAT_FIXED; k++ ) {
			CONST T_TVPAT	*own = ( fig != NULL ) ? tv_fig_pattern(fig, k) : NULL;
			UW		col;
			CONST UH	*tex = NULL;

			c.left = GX + ( k % 16 ) * CELL;
			c.top = GY + 20 + ( k / 16 ) * CELL;
			c.right = c.left + CELL - 1;
			c.bottom = c.top + CELL - 1;
			dp_fill_rect(gid, &c, 0x00FFFFFFU);
			if ( own != NULL ) {
				T_DPPAT	pat;

				knl_memset(&pat, 0, sizeof(pat));
				pat.kind = DP_PAT_TILE;
				pat.hs = pat.vs = 16;
				pat.tile = own->tile;
				pat.mask = own->mask;
				pat_rect(gid, &c, &pat);
			} else if ( k > 0 && tv_pattern(k, &col, &tex) ) {
				if ( tex != NULL ) {
					T_DPPAT	pat;
					T_TVTEX	room;

					tv_tex_pat(&pat, &room, col, tex);
					pat_rect(gid, &c, &pat);
				} else {
					dp_fill_rect(gid, &c, col);
				}
			}
			dp_frame_rect(gid, &c, 0x00999999U, 1);
		}
	} else {
		/* the dots, large */
		for ( y = 0; y < 16; y++ ) {
			for ( x = 0; x < 16; x++ ) {
				c.left = GX + x * CELL;
				c.top = GY + y * CELL;
				c.right = c.left + CELL;
				c.bottom = c.top + CELL;
				cell_colour(gid, &c, p->px[y][x]);
			}
		}
		for ( k = 0; k <= 16; k++ ) {
			dp_line(gid, GX + k * CELL, GY, GX + k * CELL, GY + 16 * CELL, 0x00B0B0B0U);
			dp_line(gid, GX, GY + k * CELL, GX + 16 * CELL, GY + k * CELL, 0x00B0B0B0U);
		}
	}
	/* the number, the colour drawn in, and the pattern at its own size, laid four times */
	k = 0;
	txt[k++] = 'N';  txt[k++] = 'o';  txt[k++] = '.';
	(void)put_num(num, 0, p->id);
	for ( x = 0; num[x] != 0; x++ ) txt[k++] = (char)num[x];
	txt[k] = 0;
	text_at(gid, RX, GY + 12, 12, "原寸:", 0);
	text_at(gid, RX + 90, GY + 12, 12, txt, 0);
	for ( y = 0; y < 32; y++ ) {
		for ( x = 0; x < 32; x++ ) {
			UW	v = p->px[y % 16][x % 16];

			dp_put_pixel(gid, RX + 40 + x, GY + 2 + y, ( v == NONE ) ? 0x00FFFFFFU : v);
		}
	}
	c.left = RX + 39;  c.top = GY + 1;  c.right = RX + 73;  c.bottom = GY + 35;
	dp_frame_rect(gid, &c, 0x00000000U, 1);
	c.left = RX + 140;  c.top = GY + 20;  c.right = RX + 156;  c.bottom = GY + 36;
	cell_colour(gid, &c, p->col);		/* the colour drawn in, 透明 as nothing */
	dp_frame_rect(gid, &c, 0x00000000U, 1);
}
