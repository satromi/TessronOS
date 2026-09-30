/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfpat.c
 *	パターン編集: a figure's pattern, dot by dot (design 17.13)
 *
 *	A pattern is sixteen by sixteen dots, each a colour or nothing.
 *	The window shows it large, to be drawn in, and at its own size
 *	laid over and over. What is drawn is put into the figure with 入替
 *	under the pattern's number: the numbers 2 to 127 are the fixed
 *	patterns, and a figure may draw one of them its own way; from 128
 *	on they are the figure's alone. 読込 takes another pattern to start
 *	from, and 復旧 takes back the last change.
 *
 *	In the record a pattern is its colours, each with the mask of the
 *	dots it covers: a <pattern> naming its colours and masks, and a
 *	<mask> of sixteen rows for each colour, both in <patterns>.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/fn.h>
#include <ts/look.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESC		0x29

#define NONE		TAD_COL_NONE

#define CELL		12		/* one dot, drawn large */
#define GX		10		/* where the large dots start */
#define GY		10
#define RX		( GX + 16 * CELL + 14 )	/* the column on the right */
#define PW		( RX + 180 )
#define PH		( GY + 16 * CELL + 46 )

/* The colours a dot may be drawn in */
LOCAL CONST UW	pe_cols[] = {
	0x000000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xEE0000, 0xFF8C00, 0xFFFF00,
	0x008000, 0x7FFF00, 0x00FFFF, 0x0000FF, 0x800080, 0xFF69B4, 0x8B4513,
	0x000080, 0x006400
};
#define NCOL	( (INT)( sizeof(pe_cols) / sizeof(pe_cols[0]) ) )

#define T_PENCIL	0
#define T_RECT		1
#define T_PICK		2
#define T_FILL		3

/* What is being edited */
LOCAL UW	pe_px[16][16];		/* each dot's colour, or NONE */
LOCAL UW	pe_undo[16][16];
LOCAL UW	pe_col = 0x000000;	/* the colour drawn in; NONE rubs out */
LOCAL INT	pe_tool = T_PENCIL;
LOCAL INT	pe_id = 2;		/* the pattern's number */
LOCAL BOOL	pe_choosing = FALSE;	/* 読込: the patterns laid out to choose */
LOCAL INT	pe_wid = 0;
LOCAL INT	pe_rx0, pe_ry0;		/* a rectangle being drawn */

LOCAL void text_at( INT gid, INT x, INT y, INT size, CONST char *s, UW col )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, size);
		fn_draw(gid, fid, x, y, (CONST UB *)s, col);
	}
}

LOCAL void bevel( INT gid, CONST T_DPRECT *c, BOOL in )
{
	UW	lit = in ? wm_look(LK_SHADOW) : 0x00FFFFFFU;
	UW	dark = in ? 0x00FFFFFFU : wm_look(LK_SHADOW);

	dp_fill_rect(gid, c, in ? 0x00B0B0B0U : wm_look(LK_PNLGROUND));
	dp_line(gid, c->left, c->top, c->right - 1, c->top, lit);
	dp_line(gid, c->left, c->top, c->left, c->bottom - 1, lit);
	dp_line(gid, c->left, c->bottom - 1, c->right - 1, c->bottom - 1, dark);
	dp_line(gid, c->right - 1, c->top, c->right - 1, c->bottom - 1, dark);
}

/* ---------------------------------------------------------------- the pattern's dots */

/* A pattern's dots, from the figure's own or the fixed ones */
LOCAL void load_pattern( DTWIN *d, INT id )
{
	CONST T_TVPAT	*own = ( d->fig != NULL ) ? tv_fig_pattern(d->fig, id) : NULL;
	UW		col;
	CONST UH	*tex = NULL;
	INT		x, y;

	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			pe_px[y][x] = NONE;
		}
	}
	if ( own != NULL ) {
		for ( y = 0; y < 16; y++ ) {
			for ( x = 0; x < 16; x++ ) {
				if ( ( own->mask[y] & ( 0x80000000U >> x ) ) != 0 ) {
					pe_px[y][x] = own->tile[y * 16 + x] & 0x00FFFFFFU;
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
				pe_px[y][x] = col;
			}
		}
	}
}

LOCAL void keep_undo( void )
{
	knl_memcpy(pe_undo, pe_px, sizeof(pe_px));
}

/* The dots moved round by one, those that go off one side coming on at the other */
LOCAL void shift( INT dx, INT dy )
{
	UW	t[16][16];
	INT	x, y;

	keep_undo();
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			t[( y + dy + 16 ) % 16][( x + dx + 16 ) % 16] = pe_px[y][x];
		}
	}
	knl_memcpy(pe_px, t, sizeof(t));
}

/* Turned a quarter, right (1) or left (-1); or turned over */
LOCAL void turn( INT how )
{
	UW	t[16][16];
	INT	x, y;

	keep_undo();
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			switch ( how ) {
			case 1:  t[x][15 - y] = pe_px[y][x];	break;
			case -1: t[15 - x][y] = pe_px[y][x];	break;
			case 2:  t[y][15 - x] = pe_px[y][x];	break;
			default: t[15 - y][x] = pe_px[y][x];	break;
			}
		}
	}
	knl_memcpy(pe_px, t, sizeof(t));
}

/* 展開: the dots joined to one, of its colour, all given the colour drawn in */
LOCAL void flood( INT x0, INT y0 )
{
	UW	was = pe_px[y0][x0];
	INT	stack[256][2], n = 0;

	if ( was == pe_col ) {
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
		if ( x < 0 || x > 15 || y < 0 || y > 15 || pe_px[y][x] != was ) {
			continue;
		}
		pe_px[y][x] = pe_col;
		if ( n + 4 <= 256 ) {
			stack[n][0] = x + 1;  stack[n][1] = y;  n++;
			stack[n][0] = x - 1;  stack[n][1] = y;  n++;
			stack[n][0] = x;  stack[n][1] = y + 1;  n++;
			stack[n][0] = x;  stack[n][1] = y - 1;  n++;
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
LOCAL void replace( DTWIN *d )
{
	T_TAD		*rec = (T_TAD *)d->rec;
	T_TADNODE	*body = tad_body(rec), *pats = NULL, *n, *next, *pe;
	UW		cols[16];
	UB		*fg, *mk, num[16];
	INT		ncol = 0, x, y, c, top_mask = 127, at_fg = 0, at_mk = 0;

	if ( body == NULL || d->sealed ) {
		return;
	}
	/* the colours used */
	for ( y = 0; y < 16; y++ ) {
		for ( x = 0; x < 16; x++ ) {
			UW	v = pe_px[y][x];

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
		return;
	}
	ed_before(d);
	for ( n = body->first; n != NULL; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "patterns") ) {
			pats = n;
			break;
		}
	}
	if ( pats == NULL ) {
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
		return;
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
			if ( id == pe_id ) {
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
				if ( pe_px[y][x] == cols[c] ) {
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
		(void)put_num(num, 0, pe_id);
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
	ed_changed(d);
}

/* ---------------------------------------------------------------- the window */

/* Where each thing of the window is */
#define B_TOOL		0		/* four tools, from here */
#define B_MOVE		4		/* 左 上 下 右 */
#define B_TURN		8		/* 右回 左回 左右 上下 */
#define B_LOAD		12
#define B_SWAP		13
#define B_UNDO		14
#define B_CLOSE		15
#define B_N		16

LOCAL CONST char *CONST pe_names[B_N] = {
	"鉛筆", "長方形", "スポイト", "展開",
	"左", "上", "下", "右",
	"右回", "左回", "左右", "上下",
	"読込", "入替", "復旧", "閉じる"
};

LOCAL void button_rect( INT b, T_DPRECT *r )
{
	if ( b < B_LOAD ) {
		INT	row = b / 4, col = b % 4;

		r->left = RX + col * 42;
		r->top = GY + 110 + row * 34;
		r->right = r->left + 40;
		r->bottom = r->top + 24;
		return;
	}
	r->left = GX + ( b - B_LOAD ) * 76;
	r->top = GY + 16 * CELL + 12;
	r->right = r->left + 70;
	r->bottom = r->top + 26;
}

LOCAL void swatch_rect( INT k, T_DPRECT *r )
{
	r->left = RX + ( k % 9 ) * 18;
	r->top = GY + 58 + ( k / 9 ) * 18;
	r->right = r->left + 16;
	r->bottom = r->top + 16;
}

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

LOCAL void pe_draw( DTWIN *d )
{
	INT		gid = wm_gid(pe_wid), x, y, k;
	T_WMWIN		w;
	T_DPRECT	all, c;
	char		txt[24];
	UB		num[12];

	(void)d;
	if ( gid < 0 || wm_ref(pe_wid, &w) < E_OK ) {
		return;
	}
	all.left = 0;  all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	dp_fill_rect(gid, &all, wm_look(LK_PNLGROUND));

	if ( pe_choosing ) {
		/* the patterns to choose from, sixteen across */
		text_at(gid, GX, GY + 10, 12, "読み込むパターンを選んでください", 0);
		for ( k = 0; k < TV_PAT_FIXED; k++ ) {
			CONST T_TVPAT	*own = ( d->fig != NULL ) ? tv_fig_pattern(d->fig, k) : NULL;
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
				dp_fill_rect_pat(gid, &c, &pat);
			} else if ( k > 0 && tv_pattern(k, &col, &tex) ) {
				if ( tex != NULL ) {
					T_DPPAT	pat;

					tv_tex_pat(&pat, col, tex);
					dp_fill_rect_pat(gid, &c, &pat);
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
				cell_colour(gid, &c, pe_px[y][x]);
			}
		}
		for ( k = 0; k <= 16; k++ ) {
			dp_line(gid, GX + k * CELL, GY, GX + k * CELL, GY + 16 * CELL,
				0x00B0B0B0U);
			dp_line(gid, GX, GY + k * CELL, GX + 16 * CELL, GY + k * CELL,
				0x00B0B0B0U);
		}
	}
	/* the number, and the pattern at its own size, laid four times */
	k = 0;
	txt[k++] = 'N';  txt[k++] = 'o';  txt[k++] = '.';
	(void)put_num(num, 0, pe_id);
	for ( x = 0; num[x] != 0; x++ ) txt[k++] = (char)num[x];
	txt[k] = 0;
	text_at(gid, RX, GY + 12, 12, "原寸:", 0);
	text_at(gid, RX + 90, GY + 12, 12, txt, 0);
	for ( y = 0; y < 32; y++ ) {
		for ( x = 0; x < 32; x++ ) {
			UW	v = pe_px[y % 16][x % 16];

			dp_put_pixel(gid, RX + 40 + x, GY + 2 + y,
				     ( v == NONE ) ? 0x00FFFFFFU : v);
		}
	}
	c.left = RX + 39;  c.top = GY + 1;  c.right = RX + 73;  c.bottom = GY + 35;
	dp_frame_rect(gid, &c, 0x00000000U, 1);

	/* the colours, and nothing */
	text_at(gid, RX, GY + 52, 12, "選択色:", 0);
	for ( k = 0; k <= NCOL; k++ ) {
		swatch_rect(k, &c);
		cell_colour(gid, &c, ( k < NCOL ) ? pe_cols[k] : NONE);
		if ( ( k < NCOL && pe_col == pe_cols[k] ) || ( k == NCOL && pe_col == NONE ) ) {
			dp_frame_rect(gid, &c, 0x000078D7U, 2);
		} else {
			dp_frame_rect(gid, &c, 0x00999999U, 1);
		}
	}
	text_at(gid, RX, GY + 106, 11, "ツール:", 0);
	text_at(gid, RX, GY + 140, 11, "移動:", 0);
	text_at(gid, RX, GY + 174, 11, "変形:", 0);
	for ( k = 0; k < B_N; k++ ) {
		button_rect(k, &c);
		if ( k < B_LOAD ) {
			c.top += 8;
			c.bottom += 8;
		}
		bevel(gid, &c, (BOOL)( ( k == pe_tool ) || ( k == B_LOAD && pe_choosing ) ));
		text_at(gid, c.left + 4, c.top + 17, 11, pe_names[k], 0);
	}
	dp_frame_rect(gid, &all, 0x00000000U, 1);
	wm_damage(pe_wid, &all);
	wm_update();
}

/* A press in the window: TRUE when it asks to close */
LOCAL BOOL pe_press( DTWIN *d, INT x, INT y )
{
	T_DPRECT	c;
	INT		k;

	for ( k = 0; k < B_N; k++ ) {
		button_rect(k, &c);
		if ( k < B_LOAD ) {
			c.top += 8;
			c.bottom += 8;
		}
		if ( x < c.left || x >= c.right || y < c.top || y >= c.bottom ) {
			continue;
		}
		if ( k < B_MOVE ) {
			pe_tool = k;
		} else if ( k < B_TURN ) {
			CONST INT	dx[4] = { -1, 0, 0, 1 }, dy[4] = { 0, -1, 1, 0 };

			shift(dx[k - B_MOVE], dy[k - B_MOVE]);
		} else if ( k < B_LOAD ) {
			CONST INT	how[4] = { 1, -1, 2, 3 };

			turn(how[k - B_TURN]);
		} else if ( k == B_LOAD ) {
			pe_choosing = (BOOL)!pe_choosing;
		} else if ( k == B_SWAP ) {
			replace(d);
		} else if ( k == B_UNDO ) {
			UW	t[16][16];

			knl_memcpy(t, pe_px, sizeof(t));
			knl_memcpy(pe_px, pe_undo, sizeof(t));
			knl_memcpy(pe_undo, t, sizeof(t));
		} else {
			return TRUE;
		}
		return FALSE;
	}
	for ( k = 0; k <= NCOL; k++ ) {
		swatch_rect(k, &c);
		if ( x >= c.left && x < c.right && y >= c.top && y < c.bottom ) {
			pe_col = ( k < NCOL ) ? pe_cols[k] : NONE;
			return FALSE;
		}
	}
	if ( pe_choosing ) {
		INT	cx = ( x - GX ) / CELL, cy = ( y - GY - 20 ) / CELL;

		if ( x >= GX && y >= GY + 20 && cx < 16 && cy < 8 ) {
			keep_undo();
			pe_id = cy * 16 + cx;
			if ( pe_id < 2 ) {
				pe_id = 2;
			}
			load_pattern(d, pe_id);
			pe_choosing = FALSE;
		}
		return FALSE;
	}
	if ( x >= GX && y >= GY && x < GX + 16 * CELL && y < GY + 16 * CELL ) {
		INT	px = ( x - GX ) / CELL, py = ( y - GY ) / CELL;

		switch ( pe_tool ) {
		case T_PICK:
			pe_col = pe_px[py][px];
			break;
		case T_FILL:
			keep_undo();
			flood(px, py);
			break;
		case T_RECT:
			keep_undo();
			pe_rx0 = px;
			pe_ry0 = py;
			pe_px[py][px] = pe_col;
			break;
		default:
			keep_undo();
			pe_px[py][px] = pe_col;
			break;
		}
	}

	return FALSE;
}

/* The hand carried with the button down: dots drawn, or a rectangle */
LOCAL void pe_drag( INT x, INT y )
{
	INT	px = ( x - GX ) / CELL, py = ( y - GY ) / CELL;

	if ( pe_choosing || x < GX || y < GY || px > 15 || py > 15 ) {
		return;
	}
	if ( pe_tool == T_PENCIL ) {
		pe_px[py][px] = pe_col;
	} else if ( pe_tool == T_RECT ) {
		INT	l = ( pe_rx0 < px ) ? pe_rx0 : px, r = ( pe_rx0 < px ) ? px : pe_rx0;
		INT	t = ( pe_ry0 < py ) ? pe_ry0 : py, b = ( pe_ry0 < py ) ? py : pe_ry0;
		INT	i, j;

		knl_memcpy(pe_px, pe_undo, sizeof(pe_px));
		for ( j = t; j <= b; j++ ) {
			for ( i = l; i <= r; i++ ) {
				pe_px[j][i] = pe_col;
			}
		}
	}
}

EXPORT void df_pattern_edit( DTWIN *d, INT pat )
{
	T_WMWIN		w;
	T_DPRECT	o;
	T_WMEV		ev;
	BOOL		done = FALSE, down = FALSE;

	if ( d == NULL || d->fig == NULL || pe_wid > 0
	  || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	pe_id = ( pat < 2 ) ? 2 : pat;
	load_pattern(d, pe_id);
	keep_undo();
	pe_choosing = FALSE;
	o.left = w.outer.left + 40;
	o.top = w.outer.top + 40;
	o.right = o.left + PW + 8;
	o.bottom = o.top + PH + 30;
	pe_wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "パターン編集");
	if ( pe_wid < 0 ) {
		pe_wid = 0;
		return;
	}
	wm_raise(pe_wid);
	pe_draw(d);
	wm_composite();
	while ( !done ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		switch ( ev.type ) {
		case HID_EV_BTN_DOWN:
			if ( ev.wid != pe_wid ) {
				break;		/* it stays until it is closed */
			}
			down = TRUE;
			done = pe_press(d, ev.x, ev.y);
			pe_draw(d);
			wm_composite();
			break;
		case HID_EV_BTN_UP:
			down = FALSE;
			break;
		case HID_EV_MOVE: {
			INT	px, py;
			UINT	btn;

			if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
				if ( down ) {
					T_WMWIN	pw;

					if ( wm_ref(pe_wid, &pw) >= E_OK ) {
						pe_drag(px - pw.work.left, py - pw.work.top);
						pe_draw(d);
						wm_composite();
					}
				}
				wm_pointer_moved(px, py);
			}
			break;
		}
		case HID_EV_KEY_DOWN:
			if ( ev.code == KEY_ESC || ev.code == KEY_ENTER ) {
				done = TRUE;
			}
			break;
		default:
			break;
		}
	}
	wm_close(pe_wid);
	pe_wid = 0;
	dt_draw(d);
	wm_composite();
}
