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
 *	from, and 復旧 takes back the last change. It is a window of the
 *	desktop's like any other (DTOWN), its tools a panel of parts.
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
#include <ts/part.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include "desktop.h"

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

/*
 * A window of the desktop's like any other (DTOWN): its frame moves it
 * and closes it, the second button opens its menu (閉じる, 読込, 入替,
 * 復旧 and the tail every window has). The dots, large, and the pattern
 * at its own size are what it draws; the colours, the tools and the
 * buttons are a panel of parts down its right side.
 */
#define PX		RX		/* the panel: down the right side, under the pattern at its own size */
#define PY		( GY + 40 )
#define PNW		180
#define PNH		320
#define WW		( PX + PNW + 8 )	/* the work area */
#define WH		( PY + PNH + 8 )

/* The parts of the panel */
#define N_SW		10		/* the colours, 10 up */
#define N_NONE		30		/* 透明 */
#define N_TOOL		31		/* the tools, 31 up: 鉛筆 長方形 スポイト 展開 */
#define N_MOVE		40		/* 左 上 下 右 */
#define N_TURN		44		/* 右回 左回 左右 上下 */
#define N_LOAD		50		/* 読込, a switch */
#define N_SWAP		51		/* 入替 */
#define N_UNDO		52		/* 復旧 */

LOCAL CONST char *CONST pe_tools[4] = { "鉛筆", "長方形", "スポイト", "展開" };
LOCAL CONST char *CONST pe_moves[4] = { "左", "上", "下", "右" };
LOCAL CONST char *CONST pe_turns[4] = { "右回", "左回", "左右", "上下" };

LOCAL DTWIN	*pe_win = NULL;		/* the window, NULL when it is not open */
LOCAL INT	pe_fig = 0;		/* the window of the figure it works on */
LOCAL INT	pe_pid = -1;		/* its panel */
LOCAL BOOL	pe_down = FALSE;	/* the first button held in the dots */

LOCAL CONST DTOWN	pe_own;

/* The figure's window, while it is still there */
LOCAL DTWIN *pe_figure( void )
{
	DTWIN	*d = ( pe_fig > 0 ) ? dt_win_of(pe_fig) : NULL;

	return ( d != NULL && d->fig != NULL ) ? d : NULL;
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

/* A button that does something and leaves the panel up: it answers its own number */
LOCAL void act_button( T_WMPANEL *def, INT num, INT l, INT t, INT w, CONST char *label )
{
	T_WMPART	*pt = dt_pn_add(def, WM_PT_BUTTON, num, l, t, w, 22, label);

	if ( pt != NULL ) {
		pt->answer = WM_ANS_ACT + num;
	}
}

/* The panel of parts, as the editor stands */
LOCAL void pe_panel( DTWIN *d )
{
	T_WMPANEL	*def = dt_pn_new(PNW - 2 * DT_PN_IN, PNH - 2 * DT_PN_IN);
	T_WMPART	*pt;
	INT		k;

	if ( def == NULL ) {
		return;
	}
	def->kind = WM_PNL_BARE;
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 0, PNW, 18, "選択色:");
	for ( k = 0; k < NCOL; k++ ) {
		pt = dt_pn_add(def, WM_PT_SWATCH, N_SW + k, ( k % 8 ) * 22 - DT_PN_IN, 20 + ( k / 8 ) * 22, 20, 20, NULL);
		if ( pt != NULL ) {
			pt->colour = pe_cols[k];
			pt->group = 1;
			pt->value = ( pe_col == pe_cols[k] ) ? 1 : 0;
		}
	}
	act_button(def, N_NONE, -DT_PN_IN, 64, 56, "透明");
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 100, PNW, 18, "ツール:");
	for ( k = 0; k < 4; k++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, N_TOOL + k, ( k % 2 ) * 90 - DT_PN_IN, 126 + ( k / 2 ) * 24, 88, 22,
			       pe_tools[k]);
		if ( pt != NULL ) {
			pt->group = 2;
			pt->value = ( pe_tool == k ) ? 1 : 0;
		}
	}
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 182, PNW, 18, "移動:");
	for ( k = 0; k < 4; k++ ) {
		act_button(def, N_MOVE + k, k * 44 - DT_PN_IN, 202, 42, pe_moves[k]);
	}
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 232, PNW, 18, "変形:");
	for ( k = 0; k < 4; k++ ) {
		act_button(def, N_TURN + k, k * 44 - DT_PN_IN, 252, 42, pe_turns[k]);
	}
	pt = dt_pn_add(def, WM_PT_CHECK, N_LOAD, -DT_PN_IN, 286, 58, 22, "読込");
	if ( pt != NULL ) {
		pt->value = pe_choosing ? 1 : 0;
	}
	act_button(def, N_SWAP, 60 - DT_PN_IN, 286, 56, "入替");
	act_button(def, N_UNDO, 120 - DT_PN_IN, 286, 56, "復旧");
	if ( pe_figure() == NULL || pe_figure()->sealed ) {
		def->part[def->npart - 2].type |= P_DISABLE;	/* 入替: nothing to put it in */
	}
	def->r.left = PX;
	def->r.top = PY;
	def->r.right = PX + PNW;
	def->r.bottom = PY + PNH;
	if ( pe_pid >= 0 ) {
		(void)wm_panel_close(pe_pid);
	}
	pe_pid = wm_panel_open(d->wid, def);
	Kfree(def);
}

LOCAL void pe_draw( DTWIN *d )
{
	DTWIN		*f = pe_figure();
	INT		gid = wm_gid(d->wid), x, y, k;
	T_DPRECT	c;
	char		txt[24];
	UB		num[12];

	if ( gid < 0 ) {
		return;
	}
	dt_own_ground(d);
	if ( pe_choosing ) {
		/* the patterns to choose from, sixteen across */
		text_at(gid, GX, GY + 10, 12, "読み込むパターンを選んでください", 0);
		for ( k = 0; k < TV_PAT_FIXED; k++ ) {
			CONST T_TVPAT	*own = ( f != NULL ) ? tv_fig_pattern(f->fig, k) : NULL;
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
			dp_line(gid, GX + k * CELL, GY, GX + k * CELL, GY + 16 * CELL, 0x00B0B0B0U);
			dp_line(gid, GX, GY + k * CELL, GX + 16 * CELL, GY + k * CELL, 0x00B0B0B0U);
		}
	}
	/* the number, the colour drawn in, and the pattern at its own size, laid four times */
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

			dp_put_pixel(gid, RX + 40 + x, GY + 2 + y, ( v == NONE ) ? 0x00FFFFFFU : v);
		}
	}
	c.left = RX + 39;  c.top = GY + 1;  c.right = RX + 73;  c.bottom = GY + 35;
	dp_frame_rect(gid, &c, 0x00000000U, 1);
	c.left = RX + 140;  c.top = GY + 20;  c.right = RX + 156;  c.bottom = GY + 36;
	cell_colour(gid, &c, pe_col);		/* the colour drawn in, 透明 as nothing */
	dp_frame_rect(gid, &c, 0x00000000U, 1);
	if ( pe_pid >= 0 ) {
		wm_panel_draw(pe_pid);
	}
	c.left = 0;  c.top = 0;  c.right = WW;  c.bottom = WH;
	wm_damage(d->wid, &c);
	wm_update();
}

/* A button of the panel pushed */
LOCAL void pe_act( DTWIN *d, INT num )
{
	DTWIN	*f = pe_figure();

	if ( num == N_NONE ) {
		pe_col = NONE;
		tm_printf((UB *)"pattern: colour none\n");
	} else if ( num >= N_MOVE && num < N_MOVE + 4 ) {
		CONST INT	dx[4] = { -1, 0, 0, 1 }, dy[4] = { 0, -1, 1, 0 };

		shift(dx[num - N_MOVE], dy[num - N_MOVE]);
	} else if ( num >= N_TURN && num < N_TURN + 4 ) {
		CONST INT	how[4] = { 1, -1, 2, 3 };

		turn(how[num - N_TURN]);
	} else if ( num == N_SWAP ) {
		if ( f != NULL ) {
			replace(f);
			dt_draw(f);
			tm_printf((UB *)"pattern: %d put in the figure\n", pe_id);
		}
	} else if ( num == N_UNDO ) {
		UW	t[16][16];

		knl_memcpy(t, pe_px, sizeof(t));
		knl_memcpy(pe_px, pe_undo, sizeof(t));
		knl_memcpy(pe_undo, t, sizeof(t));
	}
	(void)d;
}

/* What the panel's choices hold now, taken into the editor */
LOCAL void pe_follow( DTWIN *d )
{
	INT	k, v;
	BOOL	choosing;

	for ( k = 0; k < NCOL; k++ ) {
		if ( wm_panel_get(pe_pid, N_SW + k, &v) >= E_OK && v != 0 && pe_col != pe_cols[k] ) {
			pe_col = pe_cols[k];
			tm_printf((UB *)"pattern: colour %06x\n", (UINT)pe_col);
		}
	}
	for ( k = 0; k < 4; k++ ) {
		if ( wm_panel_get(pe_pid, N_TOOL + k, &v) >= E_OK && v != 0 && pe_tool != k ) {
			pe_tool = k;
			tm_printf((UB *)"pattern: tool %d\n", k);
		}
	}
	choosing = (BOOL)( wm_panel_get(pe_pid, N_LOAD, &v) >= E_OK && v != 0 );
	if ( choosing != pe_choosing ) {
		pe_choosing = choosing;
		tm_printf((UB *)"pattern: choosing %s\n", choosing ? "on" : "off");
	}
	(void)d;
}

/* A press in the dots, or on a pattern to choose */
LOCAL void pe_press( DTWIN *d, INT x, INT y )
{
	DTWIN	*f = pe_figure();

	if ( pe_choosing ) {
		INT	cx = ( x - GX ) / CELL, cy = ( y - GY - 20 ) / CELL;

		if ( x >= GX && y >= GY + 20 && cx < 16 && cy < 8 && f != NULL ) {
			keep_undo();
			pe_id = cy * 16 + cx;
			if ( pe_id < 2 ) {
				pe_id = 2;
			}
			load_pattern(f, pe_id);
			pe_choosing = FALSE;
			(void)wm_panel_set(pe_pid, N_LOAD, 0);
			tm_printf((UB *)"pattern: %d read\n", pe_id);
		}
		return;
	}
	if ( x >= GX && y >= GY && x < GX + 16 * CELL && y < GY + 16 * CELL ) {
		INT	px = ( x - GX ) / CELL, py = ( y - GY ) / CELL;

		pe_down = TRUE;
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
	(void)d;
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

LOCAL BOOL pe_input( DTWIN *d, CONST T_WMEV *ev )
{
	UINT	ans = WM_ANS_NONE;

	if ( ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP )
	  && ( ev->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
		return FALSE;			/* the menu's keys */
	}
	if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return TRUE;			/* the second button: the desktop's menu */
	}
	if ( ev->type == HID_EV_BTN_UP ) {
		pe_down = FALSE;
	}
	if ( ev->type == HID_EV_MOVE && pe_down ) {
		pe_drag(ev->x, ev->y);
		pe_draw(d);
		return TRUE;
	}
	if ( pe_pid >= 0 && wm_panel_event(pe_pid, ev, &ans) < E_OK ) {
		ans = WM_ANS_NONE;
	}
	if ( ev->type == HID_EV_BTN_DOWN && ( ev->x < PX || ev->y < PY ) ) {
		pe_press(d, ev->x, ev->y);
	} else if ( pe_pid >= 0 ) {
		pe_follow(d);
	}
	if ( ans >= WM_ANS_ACT ) {
		pe_act(d, (INT)( ans - WM_ANS_ACT ));
	}
	if ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP || ans >= WM_ANS_ACT ) {
		pe_draw(d);
	}
	return TRUE;
}

LOCAL void pe_menu_set( DTWIN *d, ID mid )
{
	DTWIN	*f = pe_figure();

	(void)mn_chg_atr(mid, NULL, "pat.swap", ( f != NULL && !f->sealed ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "pat.load", pe_choosing ? MN_TICK : 0);
	(void)d;
}

LOCAL void pe_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	if ( mn_is(sel, "pat.load") ) {
		pe_choosing = !pe_choosing;
		(void)wm_panel_set(pe_pid, N_LOAD, pe_choosing ? 1 : 0);
	} else if ( mn_is(sel, "pat.swap") ) {
		pe_act(d, N_SWAP);
	} else if ( mn_is(sel, "pat.undo") ) {
		pe_act(d, N_UNDO);
	} else {
		return;
	}
	pe_draw(d);
}

LOCAL void pe_close( DTWIN *d )
{
	if ( pe_pid >= 0 ) {
		(void)wm_panel_close(pe_pid);
	}
	pe_pid = -1;
	pe_win = NULL;
	pe_fig = 0;
	pe_down = FALSE;
	(void)d;
	tm_printf((UB *)"pattern: closed\n");
}

LOCAL CONST DTOWN	pe_own = {
	SYSDEF_MENU_FPAT,
	pe_draw,
	pe_input,
	NULL,
	pe_menu_set,
	pe_menu_do,
	NULL,
	pe_close,
	NULL
};

EXPORT void df_pattern_edit( DTWIN *d, INT pat )
{
	T_WMWIN		w, ww;
	T_DPRECT	o;
	DTWIN		*e;

	if ( d == NULL || d->fig == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	if ( pe_win != NULL && pe_win->used && pe_win->own == &pe_own ) {
		/* one at a time: brought to the front, on this figure from now on */
		pe_fig = d->wid;
		wm_raise(pe_win->wid);
		wm_focus(pe_win->wid);
		wm_update();
		return;
	}
	pe_id = ( pat < 2 ) ? 2 : pat;
	pe_fig = d->wid;
	load_pattern(d, pe_id);
	keep_undo();
	pe_choosing = FALSE;
	pe_down = FALSE;
	o.left = w.outer.left + 40;
	o.top = w.outer.top + 40;
	o.right = o.left + WW + 16;
	o.bottom = o.top + WH + 40;
	e = dt_own_open(&pe_own, NULL, &o, WM_ATTR_FRAME | WM_ATTR_TITLE, "パターン編集", NULL);
	if ( e == NULL ) {
		return;
	}
	pe_win = e;
	/* the figure's, in front of it: the figure's group stays in front and its 道具パネル up */
	(void)wm_set_parent(e->wid, d->wid);
	if ( wm_ref(e->wid, &ww) >= E_OK ) {
		/* the frame measured: the work area made the size it is drawn at */
		o = ww.outer;
		o.right += WW - ( ww.work.right - ww.work.left );
		o.bottom += WH - ( ww.work.bottom - ww.work.top );
		(void)wm_move(e->wid, &o);
	}
	pe_panel(e);
	tm_printf((UB *)"pattern: opened %d\n", pe_id);
	dt_draw(e);
}
