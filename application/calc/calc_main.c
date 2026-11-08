/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_main.c
 *	基本表計算: a sheet of cells (design 11.13)
 *
 *	The program is started on a 基本表計算 object and shows its sheet
 *	in a window of its own, the grid all of it (calc_draw.c). The parts
 *	that work on the cells -- the letters' look, the placing, the
 *	frames, the charts, and a box with what the cell in hand holds --
 *	are in a small window of their own kept in front of the sheet's,
 *	the 操作パネル (calc_panel.c), which the 表示 menu puts away and
 *	brings back.
 *
 *	A press takes a cell, and carried over others a range of them;
 *	Shift takes from the cell in hand to the one pressed; a press on a
 *	column's letters or a row's number takes the whole of it, and on the
 *	line at its edge sizes it. The square at the corner of what is
 *	taken, carried, fills the cells it is carried over with copies of
 *	what is taken, a formula's references moved as it goes. A second
 *	press, Enter, F2 or a letter typed begins typing (calc_edit.c).
 *	A link in a cell is taken by a press on it, opened by a second,
 *	carried to another cell or out of the window, and sized by its
 *	right and lower edges; a chart is taken, moved and sized the same
 *	way. The menu, the keys and the panel do the rest; what was done is
 *	said on the message line and, for the tests, on the console
 *	("calc: ...").
 *
 *	Started with the words "selftest" instead of an object, the program
 *	runs its own test (calc_test.c) and ends.
 */

#include "calc.h"
#include <ts/hid.h>
#include <ts/json.h>
#include <ts/tray.h>

/* ---------------------------------------------------------------- state */

CXSHEET	g_s;
TS_UUID	g_obj;
UB	g_name[OB_NAME_MAX];
BOOL	g_autoref;
UW	g_bg = C_PAPER;

TS_UUID	g_win;
ID	g_kw, g_port;
TS_UUID	g_lch;
ID	g_lport;
INT	g_gid = -1, g_w, g_h;
UI	g_ui;
BOOL	g_dirty, g_changed, g_closing;
INT	g_panel_dx, g_panel_dy;
BOOL	g_panel_said;

INT	g_sx, g_sy;
INT	g_col = 1, g_row = 1, g_acol = 1, g_arow = 1;
BOOL	g_cells_on = TRUE;
INT	g_chart = -1;
BOOL	g_link_on;
BOOL	g_show_hidden;
BOOL	g_paper_frame;
BOOL	g_full;

void warn( CONST char *a, CONST char *b )
{
	CONST char	*l[2] = { a, ( b != NULL ) ? b : " " };

	note(a);
	(void)ui_ask(&g_ui, PN_WARN, l, 2);
}

void *ct_more( void *p, SZ size )
{
	if ( size == 0 ) {
		free(p);
		return NULL;
	}
	return realloc(p, (size_t)size);
}
char	g_meta_none[1];
char	*g_meta = g_meta_none;

static TS_UUID	g_ch;
static T_OBWPOS	g_restore;
static BOOL	g_panel_shown = TRUE;	/* the object's window.panel */

/* What the hand holds */
#define HOLD_NONE	0
#define HOLD_CELLS	1			/* a range being pulled out */
#define HOLD_COLW	2			/* a column's edge */
#define HOLD_ROWH	3			/* a row's edge */
#define HOLD_FILL	4			/* the fill handle */
#define HOLD_CHART	5			/* a chart, moved */
#define HOLD_CSIZE	6			/* a chart's corner */
#define HOLD_LINK	7			/* a link, carried */
#define HOLD_LSIZE	8			/* a link's edge */
#define CHART_GRIP	8			/* how near a chart's foot corner sizes it */
#define HOLD_COLS	9			/* columns' letters */
#define HOLD_ROWS	10			/* rows' numbers */

static INT	g_hold = HOLD_NONE;
static INT	g_hx0, g_hy0, g_hx, g_hy;
static INT	g_hat;				/* the column or row sized */
static BOOL	g_hmoved, g_htwice;
static BOOL	g_hright, g_hfoot;
static CXRANGE	g_fill0;			/* what is taken when filling began */
static UD	g_press_at;
static INT	g_press_col, g_press_row;
static INT	g_chart_was[4];
static CXCHART	g_cclip;			/* the chart copied */
static BOOL	g_cclip_on;
static D	g_cclip_time;			/* when it went in the tray */

/* When the set in hand went in the tray; 0 when the tray is empty */
static D hand_time( void )
{
	T_TRSTS	*s = malloc(sizeof(T_TRSTS));
	D	t = 0;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS ) t = s->set[s->hand].time;
	free(s);
	return t;
}

/* ---------------------------------------------------------------- saying things */

void note( CONST char *s )
{
	char	line[300];

	snprintf(line, sizeof(line), "calc: %s\n", s);
	tm_putstring((CONST UB *)line);
}


void say( CONST char *s )
{
	(void)wm_msg_put(s);
}

/* ---------------------------------------------------------------- the window */

static void work_size( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		g_w = wp.wright - wp.wleft;
		g_h = wp.wbottom - wp.wtop;
	}
	g_ui.w = g_w;
	g_ui.h = g_h;
}

/* The window where the object last stood, the size it says */
static ER win_open( CONST char *title )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	T_JSON		root, w, pos;
	SZ		asz = 0;
	char		json[260];
	char		shown[TS_UUID_STRLEN + 1];
	INT		x = 100, y = 80, ww = 800, wh = 600, mn = meta_of(&g_obj);

	if ( mn > 0 && js_parse((CONST UB *)g_meta, mn, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK ) {
		UB	bg[16];

		ww = (INT)js_get_num(&w, "width", ww);
		wh = (INT)js_get_num(&w, "height", wh);
		if ( js_get(&w, "pos", &pos) >= E_OK ) {
			x = (INT)js_get_num(&pos, "x", x);
			y = (INT)js_get_num(&pos, "y", y);
		}
		/* the 操作パネル: shown or not, and where against the window */
		g_panel_shown = js_get_bool(&w, "panel", TRUE);
		if ( js_get(&w, "panelpos", &pos) >= E_OK ) {
			g_panel_dx = (INT)js_get_num(&pos, "x", x) - x;
			g_panel_dy = (INT)js_get_num(&pos, "y", y) - y;
			g_panel_said = TRUE;
		}
		if ( js_get_str(&w, "backgroundColor", bg, sizeof(bg)) == 7 && bg[0] == '#' ) {
			g_bg = (UW)strtoul((char *)bg + 1, NULL, 16);
		}
	}
	if ( ww < MIN_W ) ww = MIN_W;
	if ( wh < MIN_H ) wh = MIN_H;
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	ts_uuid_text(&g_obj, shown);	/* what it shows, for ウインドウ一覧 */
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 x, y, x + ww + FRAME_W, y + wh + TITLE_H,
		 WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR | WM_ATTR_BBAR, SYSDEF_PROG_CALC, shown);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) return E_OBJ;
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
		   | OB_E_DELETE | OB_E_DROP | OB_E_LEAVE | OB_E_WHEEL | OB_E_SCROLL;
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( g_kw > 0 ) ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
		g_kw = 0;
		return E_OBJ;
	}
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + ww + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + wh + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	g_ui.kw = g_kw;
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_CALC, &g_ui.box);
	work_size();
	return E_OK;
}

static void resized( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	work_size();
	if ( ( g_w < MIN_W || g_h < MIN_H ) && ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		if ( g_w < MIN_W ) wp.right += MIN_W - g_w;
		if ( g_h < MIN_H ) wp.bottom += MIN_H - g_h;
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		work_size();
	}
	g_dirty = TRUE;
}

static void fullscreen( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	SZ	asz = 0;

	if ( g_full ) {
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz);
		g_full = FALSE;
		note("fullscreen off");
	} else if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz) >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_FULL;
		if ( ask_desktop(&rq, &an, 5000) >= E_OK ) {
			g_full = TRUE;
			note("fullscreen on");
		}
	}
	resized();
}

/* ---------------------------------------------------------------- what is taken */

static void take_cell( INT col, INT row, BOOL extend )
{
	if ( col < 1 ) col = 1;
	if ( row < 1 ) row = 1;
	if ( col > CX_COLS_MAX ) col = CX_COLS_MAX;
	if ( row > CX_ROWS_MAX ) row = CX_ROWS_MAX;
	cx_grow(&g_s, col, row);
	g_col = col;
	g_row = row;
	if ( !extend ) {
		g_acol = col;
		g_arow = row;
	}
	g_cells_on = TRUE;
	g_chart = -1;
	g_link_on = FALSE;
	scroll_to(col, row);
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the cells' look */

static void style_range( CONST CXSTYLE *st )
{
	CXRANGE	r;
	INT	col, row;

	range_of(&r);
	for ( row = r.r0; row <= r.r1; row++ ) {
		for ( col = r.c0; col <= r.c1; col++ ) {
			CXCELL	*c = cx_make(&g_s, col, row);

			if ( c == NULL || !cx_style_add(c, st) ) {
				warn("書式を付けられませんでした", "メモリが足りません");
				goto done;
			}
		}
	}
done:
	g_changed = TRUE;
	g_dirty = TRUE;
}

/* A decoration turned on or off for what is taken, as the cell in hand has it */
static void toggle( UINT bit )
{
	CXCELL	*c = cx_get(&g_s, g_col, g_row);
	BOOL	on = !( c != NULL && ( c->st->has & bit ) );
	CXRANGE	r;
	INT	col, row;

	range_of(&r);
	for ( row = r.r0; row <= r.r1; row++ ) {
		for ( col = r.c0; col <= r.c1; col++ ) {
			CXCELL	*d = cx_make(&g_s, col, row);
			CXSTYLE	st;

			if ( d == NULL ) continue;
			st = *d->st;
			if ( on ) {
				if ( bit == CX_S_SUPER ) st.has &= ~CX_S_SUB;
				if ( bit == CX_S_SUB ) st.has &= ~CX_S_SUPER;
				st.has |= bit;
			} else {
				st.has &= ~bit;
			}
			if ( !cx_style_put(d, &st) ) {
				warn("書式を付けられませんでした", "メモリが足りません");
				goto done;
			}
		}
	}
done:
	g_changed = TRUE;
	g_dirty = TRUE;
}

static CXSIDE	g_side = { TRUE, CX_SOLID, 1, "#000000" };

CONST CXSIDE *line_side( void )
{
	return &g_side;
}

static void frames( INT how )
{
	CXRANGE	r;

	range_of(&r);
	cx_frame_range(&g_s, &r, how, &g_side);
	g_changed = TRUE;
	g_dirty = TRUE;
}

static void set_colour( char *dst, UINT rgb )
{
	snprintf(dst, CX_COLOUR, "#%06x", rgb & 0x00FFFFFFU);
}

/* ---------------------------------------------------------------- rows, columns, fill, joins */

static void insert_or_delete( BOOL cols, INT d )
{
	INT	at = cols ? g_col : g_row;

	ed_cancel();
	if ( cols ) {
		if ( d > 0 ) cx_insert_col(&g_s, at);
		else cx_delete_col(&g_s, at);
	} else {
		if ( d > 0 ) cx_insert_row(&g_s, at);
		else cx_delete_row(&g_s, at);
	}
	(void)ce_move(cols, at, d);
	shown_all();
	g_changed = TRUE;
	note(cols ? ( d > 0 ? "column inserted" : "column deleted" ) : ( d > 0 ? "row inserted" : "row deleted" ));
}

/* A cell copied to another, its formula's references moved */
static void copy_cell( INT sc, INT sr, INT dc, INT dr )
{
	CXCELL	*s = cx_get(&g_s, sc, sr);
	UB	*t;

	if ( s == NULL ) {
		if ( cx_get(&g_s, dc, dr) != NULL ) cell_clear(dc, dr);
		return;
	}
	{
		/* a reference moved may grow by a few letters, or to #REF! */
		INT	n = (INT)strlen((char *)s->text), max = n * 2 + 64;

		t = malloc((size_t)max);
		if ( t == NULL ) {
			warn("セルを複写できませんでした", "メモリが足りません");
			return;
		}
		if ( s->text[0] == '=' ) {
			(void)cx_move_refs(s->text, n, dc - sc, dr - sr, g_s.maxcol, g_s.maxrow, t, max);
		} else {
			memcpy(t, s->text, (size_t)n + 1);
		}
	}
	{
		CONST CXSTYLE	*st = s->st;

		cell_text(dc, dr, t);
		s = cx_get(&g_s, dc, dr);
		if ( s != NULL && st->has != 0 ) (void)cx_style_add(s, st);
	}
	free(t);
}

static void fill_dir( BOOL down )
{
	CXRANGE	r;
	INT	col, row;

	range_of(&r);
	if ( down && r.r0 == r.r1 ) return;
	if ( !down && r.c0 == r.c1 ) return;
	for ( col = r.c0; col <= r.c1; col++ ) {
		for ( row = r.r0; row <= r.r1; row++ ) {
			if ( down && row > r.r0 ) copy_cell(col, r.r0, col, row);
			if ( !down && col > r.c0 ) copy_cell(r.c0, row, col, row);
		}
	}
	note(down ? "filled down" : "filled right");
}

/* The cells the fill handle was carried over, filled from what was taken, the pattern repeated */
static void fill_out( CONST CXRANGE *src, CONST CXRANGE *dst )
{
	INT	w = src->c1 - src->c0 + 1, h = src->r1 - src->r0 + 1, col, row;

	for ( row = dst->r0; row <= dst->r1; row++ ) {
		for ( col = dst->c0; col <= dst->c1; col++ ) {
			INT	sc, sr;

			if ( col >= src->c0 && col <= src->c1 && row >= src->r0 && row <= src->r1 ) continue;
			sc = ( col - dst->c0 ) % w;
			sr = ( row - dst->r0 ) % h;
			if ( sc < 0 ) sc += w;
			if ( sr < 0 ) sr += h;
			if ( cx_get(&g_s, src->c0 + sc, src->r0 + sr) == NULL ) continue;
			copy_cell(src->c0 + sc, src->r0 + sr, col, row);
		}
	}
	note("filled");
}

static void merge( void )
{
	CXRANGE	r;
	INT	col, row;

	range_of(&r);
	if ( r.c0 == r.c1 && r.r0 == r.r1 ) return;
	for ( row = r.r0; row <= r.r1; row++ ) {
		for ( col = r.c0; col <= r.c1; col++ ) {
			CONST CXCELL	*c = cx_get(&g_s, col, row);

			if ( cx_merge_of(&g_s, col, row) >= 0 ) {
				say("選択範囲内に既に結合されているセルがあります");
				return;
			}
			if ( c != NULL && c->kind == CX_LINK ) {
				say("仮身を含むセルは結合できません");
				return;
			}
		}
	}
	(void)cx_merge_add(&g_s, &r);
	if ( cx_get(&g_s, r.c0, r.r0) == NULL ) {
		(void)cx_make(&g_s, r.c0, r.r0);
		(void)ce_blank(r.c0, r.r0);
	}
	for ( row = r.r0; row <= r.r1; row++ ) {
		for ( col = r.c0; col <= r.c1; col++ ) {
			CXCELL	*c;

			if ( col == r.c0 && row == r.r0 ) continue;
			c = cx_make(&g_s, col, row);
			if ( c != NULL && c->kind == CX_TEXT ) {
				CONST CXSTYLE	*st = c->st;

				(void)cx_set_text(c, (CONST UB *)"");
				c->st = st;
				(void)ce_blank(col, row);
			}
		}
	}
	shown_all();
	g_col = g_acol = r.c0;
	g_row = g_arow = r.r0;
	g_changed = TRUE;
	note("merged");
}

static void unmerge( void )
{
	INT	m = cx_merge_of(&g_s, g_col, g_row);

	if ( m < 0 ) return;
	cx_merge_del(&g_s, m);
	g_changed = TRUE;
	g_dirty = TRUE;
	note("unmerged");
}

/* ---------------------------------------------------------------- the dialogs */

/* Find and replace, worked while the panel is up */
static INT find_from( CONST UB *pat, BOOL re, INT c0, INT r0, INT lc, INT lr, BOOL limit )
{
	INT	col, row;

	for ( row = r0; row <= g_s.maxrow; row++ ) {
		for ( col = ( row == r0 ) ? c0 : 1; col <= g_s.maxcol; col++ ) {
			CONST CXCELL	*c;
			UB		nm[OB_NAME_MAX];
			CONST UB	*t;

			if ( limit && ( row > lr || ( row == lr && col >= lc ) ) ) return 0;
			c = cx_get(&g_s, col, row);
			if ( c == NULL ) continue;
			if ( c->kind == CX_LINK ) {
				TS_UUID	u;

				nm[0] = 0;
				if ( link_target(c, &u) ) name_of(&u, nm);
				t = nm;
			} else {
				t = ( c->text[0] == '=' ) ? (CONST UB *)"" : c->text;
			}
			if ( t[0] == 0 ) continue;
			if ( ce_test(t, pat, re) > 0 ) {
				take_cell(col, row, FALSE);
				return 1;
			}
		}
	}
	return 0;
}

static BOOL find_next( CONST UB *pat, BOOL re )
{
	INT	c0 = g_col + 1, r0 = g_row;

	if ( pat[0] == 0 ) return FALSE;
	if ( re && ce_test((CONST UB *)"", pat, re) < 0 ) return FALSE;
	if ( c0 > g_s.maxcol ) {
		c0 = 1;
		r0++;
	}
	if ( find_from(pat, re, c0, r0, 0, 0, FALSE) ) return TRUE;
	return (BOOL)find_from(pat, re, 1, 1, c0, r0, TRUE);
}

static void replace_cell( INT col, INT row, CONST UB *pat, CONST UB *rep, BOOL re )
{
	CXCELL	*c = cx_get(&g_s, col, row);
	UB	*out;

	if ( c == NULL || c->kind == CX_LINK || c->text[0] == 0 ) return;
	if ( ce_test(c->text, pat, re) <= 0 ) return;
	{
		/* as long as the words come to: written again into room enough when they did not fit */
		INT	max = (INT)strlen((char *)c->text) * 2 + 256, k;

		for ( ;; ) {
			out = malloc((size_t)max);
			if ( out == NULL ) {
				warn("置換できませんでした", "メモリが足りません");
				return;
			}
			k = ce_replace(c->text, pat, rep, re, out, max);
			if ( k < max ) break;
			free(out);
			max = k + 1;
		}
		if ( k >= 0 ) cell_text(col, row, out);
		free(out);
	}
}

static void find_dialog( void )
{
	UI	q;
	T_OBNTM	m;
	INT	num, ev;
	BOOL	done = FALSE;
	char	pat[512], rep[512], s[80];

	if ( ui_dlg_open(&g_ui, PN_FIND, &q) < E_OK ) return;
	note("find panel");
	while ( !done && !g_closing ) {
		/* asleep until the panel or the window has something */
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			(void)ui_text(&q, CP_FIND_TEXT, pat, sizeof(pat));
			(void)ui_text(&q, CP_FIND_REP, rep, sizeof(rep));
			if ( num == CP_FIND_CLOSE ) {
				done = TRUE;
			} else if ( num == CP_FIND_NEXT ) {
				BOOL	found = find_next((CONST UB *)pat, (BOOL)( ui_value(&q, CP_FIND_RE) != 0 ));

				ui_set_text(&q, CP_FIND_SAID, found ? "見つかりました" : "見つかりません");
				snprintf(s, sizeof(s), "find %s %s", pat, found ? "found" : "not found");
				note(s);
			} else if ( num == CP_FIND_ONE ) {
				BOOL	re = (BOOL)( ui_value(&q, CP_FIND_RE) != 0 );

				replace_cell(g_col, g_row, (CONST UB *)pat, (CONST UB *)rep, re);
				(void)find_next((CONST UB *)pat, re);
				note("replaced one");
			} else if ( num == CP_FIND_ALL ) {
				BOOL	re = (BOOL)( ui_value(&q, CP_FIND_RE) != 0 );
				INT	i, n = 0;

				for ( i = 0; i < g_s.n; i++ ) {
					CXCELL	*c = g_s.c[i];

					if ( c->kind == CX_LINK || c->text[0] == 0 || ce_test(c->text, (CONST UB *)pat, re) <= 0 ) continue;
					replace_cell(c->col, c->row, (CONST UB *)pat, (CONST UB *)rep, re);
					n++;
				}
				snprintf(s, sizeof(s), "%d件置換しました", n);
				ui_set_text(&q, CP_FIND_SAID, s);
				snprintf(s, sizeof(s), "replaced %d", n);
				note(s);
			}
			if ( g_dirty ) paint();
			break;
		case UI_EV_CLOSE:
			done = TRUE;
			break;
		case UI_EV_OTHER:
			if ( memcmp(&m.uuid, &g_win, sizeof(TS_UUID)) == 0 && m.event == OB_E_REDRAW ) {
				resized();
				paint();
			} else if ( panel_is(&m.uuid) && m.event == OB_E_REDRAW ) {
				(void)panel_event(&m);
			}
			break;
		default:
			break;
		}
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
}

/* The paper's sizes, in points */
static CONST struct {
	INT	w, l;
} papers[6] = { { 842, 1191 }, { 595, 842 }, { 420, 595 }, { 729, 1032 }, { 516, 729 }, { 612, 792 } };

static void page_dialog( void )
{
	UI	q;
	T_OBNTM	m;
	INT	num, ev, done = 0, size = 1, turn = 0, mg[4] = { 20, 20, 20, 20 }, i, w = 595, l = 842;
	char	el[200], ml[160];

	if ( g_s.paper != NULL ) {
		INT	n = (INT)strlen((char *)g_s.paper);
		static CONST char *CONST side[4] = { "top", "bottom", "left", "right" };

		w = ct_attr_int(g_s.paper, n, "width", 595);
		l = ct_attr_int(g_s.paper, n, "length", 842);
		for ( i = 0; i < 4; i++ ) mg[i] = ( ct_attr_int(g_s.paper, n, side[i], 57) * 254 + 360 ) / 720;
	}
	for ( i = 0; i < 6; i++ ) {
		if ( ( papers[i].w == w && papers[i].l == l ) || ( papers[i].w == l && papers[i].l == w ) ) {
			size = i;
			turn = ( papers[i].w == l ) ? 1 : 0;
		}
	}
	if ( ui_dlg_open(&g_ui, PN_PAGE, &q) < E_OK ) return;
	ui_set_value(&q, CP_PAGE_SIZE, size + 1);
	ui_set_value(&q, CP_PAGE_TURN, turn + 1);
	for ( i = 0; i < 4; i++ ) ui_set_fields(&q, CP_PAGE_MARGIN + i, &mg[i], 1);
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		if ( ev == UI_EV_BUTTON ) done = ( num == CP_PAGE_OK ) ? 1 : -1;
		else if ( ev == UI_EV_CLOSE ) done = -1;
	}
	if ( done > 0 ) {
		INT	pt[4];

		size = ui_value(&q, CP_PAGE_SIZE) - 1;
		turn = ui_value(&q, CP_PAGE_TURN) - 1;
		if ( size < 0 || size > 5 ) size = 1;
		for ( i = 0; i < 4; i++ ) {
			(void)ui_fields(&q, CP_PAGE_MARGIN + i, &mg[i], 1);
			pt[i] = ( mg[i] * 720 + 127 ) / 254;
		}
		w = turn ? papers[size].l : papers[size].w;
		l = turn ? papers[size].w : papers[size].l;
		snprintf(el, sizeof(el), "<paper imposition=\"0\" binding=\"0\" length=\"%d\" width=\"%d\" top=\"%d\" bottom=\"%d\""
			 " left=\"%d\" right=\"%d\" />", l, w, pt[0], pt[1], pt[2], pt[3]);
		snprintf(ml, sizeof(ml), "<docmargin top=\"%d\" bottom=\"%d\" left=\"%d\" right=\"%d\" />", pt[0], pt[1], pt[2], pt[3]);
		cx_free(g_s.paper);
		cx_free(g_s.margin);
		g_s.paper = (UB *)strdup(el);
		g_s.margin = (UB *)strdup(ml);
		g_changed = TRUE;
		note("paper set");
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
}

/* Closing, asked first when the sheet is not saved */
static void close_asked( void )
{
	INT	a;

	if ( !g_changed ) {
		g_closing = TRUE;
		return;
	}
	ed_cancel();
	a = ui_ask(&g_ui, PN_CLOSE, NULL, 0);
	if ( a == CP_CLOSE_SAVE ) {
		ER	er = save();

		if ( er >= E_OK ) {
			note("saved");
			g_closing = TRUE;
		} else {
			/* the window stays, and what was changed with it */
			warn("保存できませんでした", ( er == E_NOMEM ) ? "メモリが足りません" : "実身に書き込めませんでした");
		}
	} else if ( a == CP_CLOSE_DROP ) {
		note("not saved");
		g_closing = TRUE;
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- commands */

static CONST char *CONST face_name[3] = { "MS Gothic", "MS Mincho", "Meiryo" };

void command( CONST char *code, INT index, D value )
{
	CXSTYLE	st;
	char	s[80];

	memset(&st, 0, sizeof(st));
	if ( strcmp(code, "close") == 0 ) close_asked();
	else if ( strcmp(code, "save") == 0 ) {
		ER	er;

		ed_commit();
		if ( ( er = save() ) >= E_OK ) {
			note("saved");
			say("保存しました");
		} else {
			snprintf(s, sizeof(s), "not saved %d", (int)er);
			note(s);
			warn("保存できませんでした", ( er == E_NOMEM ) ? "メモリが足りません" : "実身に書き込めませんでした");
		}
	} else if ( strcmp(code, "panel") == 0 ) {
		if ( panel_on() ) {
			ed_commit();
			panel_close();
		} else {
			panel_open();
		}
		note(panel_on() ? "panel on" : "panel off");
	} else if ( strcmp(code, "fullscreen") == 0 ) fullscreen();
	else if ( strcmp(code, "refresh") == 0 ) {
		shown_all();
		say("再表示しました");
	} else if ( strcmp(code, "bgcolour") == 0 ) paper_ask();
	else if ( strcmp(code, "hidden") == 0 ) {
		g_show_hidden = (BOOL)!g_show_hidden;
		note(g_show_hidden ? "hidden shown" : "hidden not shown");
	} else if ( strcmp(code, "copy") == 0 || strcmp(code, "cut") == 0 ) {
		ed_commit();
		if ( g_chart >= 0 ) {
			CTBUF	b;
			T_TRREC	r;
			BOOL	put = FALSE;

			ct_buf_grow(&b, 64 * 1024, ct_more);
			if ( !b.fail && chart_frag(&b, g_chart) && !b.fail ) {
				memset(&r, 0, sizeof(r));
				r.kind = TR_TAD;
				r.data = b.p;
				r.size = b.n;
				if ( tr_psh_dat(&r, 1, (CONST UB *)"グラフ") >= E_OK ) {
					/* kept here too, pasted back as a chart while it is the set in hand */
					g_cclip = g_s.ch[g_chart];
					g_cclip.el = NULL;
					g_cclip.ellen = 0;
					g_cclip_time = hand_time();
					g_cclip_on = TRUE;
					put = TRUE;
				}
			}
			(void)ct_more(b.p, 0);
			if ( !put ) {
				/* not in the tray: nor taken away */
				warn("グラフをトレーに入れられませんでした", "メモリが足りません。グラフは元のままです");
			} else {
				note("chart copied");
				say("グラフをトレーへ複写しました");
				if ( code[1] == 'u' ) {
					cx_chart_del(&g_s, g_chart);
					g_chart = -1;
					g_changed = TRUE;
				}
			}
		} else {
			copy_cells((BOOL)( code[1] == 'u' ));
		}
	} else if ( strcmp(code, "paste") == 0 || strcmp(code, "moveback") == 0 ) {
		ed_commit();
		if ( g_cclip_on && hand_time() == g_cclip_time && g_cclip_time != 0 ) {
			/* the chart copied: a new one 20 dots down and across, the next a step further */
			g_cclip.left += 20;
			g_cclip.top += 20;
			g_cclip.right += 20;
			g_cclip.bottom += 20;
			if ( cx_chart_add(&g_s, &g_cclip) ) {
				g_chart = g_s.nch - 1;
				g_cells_on = FALSE;
				g_changed = TRUE;
				if ( code[0] == 'm' ) (void)tr_del_dat(TR_HAND);
				note("chart pasted");
				say("グラフを貼り付けました");
			}
		} else {
			paste_cells((BOOL)( code[0] == 'm' ));
		}
	} else if ( strcmp(code, "tray.level") == 0 ) (void)tr_sel_dat(index + 1);
	else if ( strcmp(code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp(code, "clear") == 0 ) {
		if ( g_chart >= 0 ) {
			cx_chart_del(&g_s, g_chart);
			g_chart = -1;
			g_changed = TRUE;
			note("chart deleted");
		} else if ( cx_get(&g_s, g_col, g_row) != NULL ) {
			cell_clear(g_col, g_row);
			g_link_on = FALSE;
			note("cleared");
		}
	} else if ( strcmp(code, "row.insert") == 0 ) insert_or_delete(FALSE, 1);
	else if ( strcmp(code, "col.insert") == 0 ) insert_or_delete(TRUE, 1);
	else if ( strcmp(code, "row.delete") == 0 ) insert_or_delete(FALSE, -1);
	else if ( strcmp(code, "col.delete") == 0 ) insert_or_delete(TRUE, -1);
	else if ( strcmp(code, "fill.down") == 0 ) fill_dir(TRUE);
	else if ( strcmp(code, "fill.right") == 0 ) fill_dir(FALSE);
	else if ( strcmp(code, "find") == 0 ) find_dialog();
	else if ( strcmp(code, "merge") == 0 ) merge();
	else if ( strcmp(code, "print") == 0 ) (void)print_sheet();
	else if ( strcmp(code, "unmerge") == 0 ) unmerge();
	else if ( strncmp(code, "font.", 5) == 0 ) {
		INT	k = ( code[5] == 'g' ) ? 0 : ( code[5] == 'm' && code[6] == 'i' ) ? 1 : 2;

		st.has = CX_S_FACE;
		snprintf(st.face, CX_FACE, "%s", face_name[k]);
		style_range(&st);
	} else if ( strcmp(code, "style.bold") == 0 ) toggle(CX_S_BOLD);
	else if ( strcmp(code, "style.italic") == 0 ) toggle(CX_S_ITALIC);
	else if ( strcmp(code, "style.underline") == 0 ) toggle(CX_S_UNDER);
	else if ( strcmp(code, "style.strike") == 0 ) toggle(CX_S_STRIKE);
	else if ( strcmp(code, "style.super") == 0 ) toggle(CX_S_SUPER);
	else if ( strcmp(code, "style.sub") == 0 ) toggle(CX_S_SUB);
	else if ( strcmp(code, "style.mesh") == 0 ) toggle(CX_S_MESH);
	else if ( strcmp(code, "style.inverse") == 0 ) toggle(CX_S_INVERT);
	else if ( strcmp(code, "style.normal") == 0 ) {
		CXRANGE	r;
		INT	col, row;

		range_of(&r);
		for ( row = r.r0; row <= r.r1; row++ ) {
			for ( col = r.c0; col <= r.c1; col++ ) {
				CXCELL	*c = cx_get(&g_s, col, row);
				CXSTYLE	cs;

				if ( c == NULL ) continue;
				cs = *c->st;
				cs.has &= ~( CX_S_BOLD | CX_S_ITALIC | CX_S_UNDER | CX_S_STRIKE
					     | CX_S_SUPER | CX_S_SUB | CX_S_MESH | CX_S_INVERT );
				(void)cx_style_put(c, &cs);
			}
		}
		g_changed = TRUE;
	} else if ( strcmp(code, "size") == 0 ) {
		st.has = CX_S_SIZE;
		st.size = (INT)value;
		style_range(&st);
	} else if ( strcmp(code, "colour") == 0 ) {
		st.has = CX_S_COLOUR;
		set_colour(st.colour, (UINT)value);
		style_range(&st);
	} else if ( strcmp(code, "ground") == 0 ) {
		st.has = CX_S_BG;
		set_colour(st.bg, (UINT)value);
		style_range(&st);
	} else if ( strncmp(code, "align.", 6) == 0 ) {
		st.has = CX_S_ALIGN;
		snprintf(st.align, sizeof(st.align), "%s", code + 6);
		style_range(&st);
	} else if ( strncmp(code, "valign.", 7) == 0 ) {
		st.has = CX_S_VALIGN;
		snprintf(st.valign, sizeof(st.valign), "%s", code + 7);
		style_range(&st);
	} else if ( strncmp(code, "frame.", 6) == 0 ) {
		static CONST char *CONST how[7] = { "all", "outline", "top", "bottom", "left", "right", "none" };
		INT	k;

		for ( k = 0; k < 7 && strcmp(code + 6, how[k]) != 0; k++ ) ;
		if ( k < 7 ) frames(k);
	} else if ( strncmp(code, "line.", 5) == 0 ) {
		static CONST char *CONST type[4] = { "solid", "dashed", "dotted", "double" };
		INT	k;

		for ( k = 0; k < 4 && strcmp(code + 5, type[k]) != 0; k++ ) ;
		if ( k < 4 ) g_side.type = k;
	} else if ( strcmp(code, "linewidth") == 0 ) g_side.width = (INT)value;
	else if ( strcmp(code, "linecolour") == 0 ) set_colour(g_side.colour, (UINT)value);
	else if ( strncmp(code, "chart.", 6) == 0 ) {
		static CONST char *CONST kind[4] = { "bar", "sbar", "line", "sline" };
		INT	k;

		for ( k = 0; k < 4 && strcmp(code + 6, kind[k]) != 0; k++ ) ;
		if ( k < 4 && chart_insert(k) ) {
			snprintf(s, sizeof(s), "chart %s", kind[k]);
			note(s);
		} else {
			say("グラフにする範囲を選択してください（1列目が見出し、2列目から値）");
		}
	} else if ( strcmp(code, "paperframe") == 0 ) {
		g_paper_frame = (BOOL)!g_paper_frame;
		note(g_paper_frame ? "paper frame on" : "paper frame off");
	} else if ( strcmp(code, "pagesetup") == 0 ) page_dialog();
	else if ( strcmp(code, "open") == 0 ) link_open(g_col, g_row);
	else {
		link_op(code, index);
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the menu */

static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	s;
	ID	mid;
	BOOL	chosen, link;
	INT	col = col_at(m->x - grid_left() + g_sx), row = row_at(m->y - grid_top() + g_sy);

	ed_commit();
	if ( m->y >= grid_top() && m->x >= grid_left() ) {
		CXRANGE		r;
		CONST CXCELL	*c = cx_get(&g_s, col, row);
		T_DPRECT	lb;

		range_of(&r);
		if ( !( g_cells_on && col >= r.c0 && col <= r.c1 && row >= r.r0 && row <= r.r1 ) ) take_cell(col, row, FALSE);
		if ( c != NULL && c->kind == CX_LINK ) {
			link_box(c, &lb);
			if ( m->x >= lb.left && m->x < lb.right && m->y >= lb.top && m->y < lb.bottom ) {
				take_cell(col, row, FALSE);
				g_link_on = TRUE;
			}
		}
	}
	paint();
	if ( ui_uuid(SYSDEF_MENU_CALC, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	link = (BOOL)( g_link_on && cx_get(&g_s, g_col, g_row) != NULL && cx_get(&g_s, g_col, g_row)->kind == CX_LINK );
	(void)mn_chg_atr(mid, NULL, "save.new", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "trash", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "panel", panel_on() ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "fullscreen", g_full ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "hidden", g_show_hidden ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "paperframe", g_paper_frame ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "paste", tray_has() ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "moveback", tray_has() ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "unmerge", cx_merge_of(&g_s, g_col, g_row) >= 0 ? 0 : MN_GREY);
	tray_menu(mid);
	if ( !link ) {
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "exec", MN_GREY);
	} else {
		BOOL	kept = app_kept();
		T_OBREF	ref;
		TS_UUID	u;
		BOOL	cut = (BOOL)( !link_target(cx_get(&g_s, g_col, g_row), &u) || ob_ref_obj(&u, &ref) < E_OK );

		(void)mn_chg_atr(mid, NULL, "vobj.reconnect", cut ? 0 : MN_HIDE);
		(void)mn_chg_atr(mid, NULL, "real.lock", kept ? MN_HIDE : cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.unlock", !kept ? MN_HIDE : cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.rename", cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.duplicate", cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.newversion", cut ? MN_GREY : 0);
		exec_menu(mid);
	}
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) command((CONST char *)s.code, s.index, s.value);
}

/* ---------------------------------------------------------------- keys */

static void move_by( INT dc, INT dr, BOOL extend )
{
	INT	col = g_col + dc, row = g_row + dr;

	if ( col < 1 ) col = 1;
	if ( row < 1 ) row = 1;
	take_cell(col, row, extend);
}

static void key( CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);
	BOOL	shift = (BOOL)( ( m->mods & M_SHIFT ) != 0 );
	INT	page = ( g_h - grid_top() ) / CX_CELL_H - 1;

	if ( ed_on() && ed_key(m->code, m->mods, m->imode) ) {
		return;
	}
	if ( ( m->mods & M_CTRL ) != 0 ) {
		UB	k = wm_key_char(m->code, 0);

		switch ( k ) {
		case 'e': case 'E':	command("close", 0, 0);		break;
		case 's': case 'S':	command("save", 0, 0);		break;
		case 'c': case 'C':	command("copy", 0, 0);		break;
		case 'x': case 'X':	command("cut", 0, 0);		break;
		case 'v': case 'V':	command("paste", 0, 0);		break;
		case 'z': case 'Z':	command("moveback", 0, 0);	break;
		case 'd': case 'D':	command("fill.down", 0, 0);	break;
		case 'r': case 'R':	command("fill.right", 0, 0);	break;
		case 'f': case 'F':	command("find", 0, 0);		break;
		case 'l': case 'L':	command("fullscreen", 0, 0);	break;
		case 'o': case 'O':	command("open", 0, 0);		break;
		case 'p': case 'P':	command("print", 0, 0);		break;
		default:		break;
		}
		return;
	}
	if ( g_chart >= 0 ) {
		if ( m->code == K_DEL || m->code == K_BS ) command("clear", 0, 0);
		else if ( m->code == K_ESC ) {
			g_chart = -1;
			g_cells_on = TRUE;
			g_dirty = TRUE;
		}
		return;
	}
	if ( page < 1 ) page = 1;
	switch ( m->code ) {
	case K_UP:	move_by(0, -1, shift);		return;
	case K_DOWN:	move_by(0, 1, shift);		return;
	case K_LEFT:	move_by(-1, 0, shift);		return;
	case K_RIGHT:	move_by(1, 0, shift);		return;
	case K_PGUP:	move_by(0, -page, shift);	return;
	case K_PGDN:	move_by(0, page, shift);	return;
	case K_HOME:	take_cell(1, g_row, shift);	return;
	case K_TAB:	move_by(shift ? -1 : 1, 0, FALSE);	return;
	case K_ENTER:
	case K_KPENTER:
	case K_F2:
		if ( g_link_on ) {
			link_open(g_col, g_row);
			return;
		}
		ed_begin(g_col, g_row, NULL, TRUE);
		return;
	case K_DEL:
	case K_BS:
		command("clear", 0, 0);
		return;
	case K_ESC:
		g_link_on = FALSE;
		g_acol = g_col;
		g_arow = g_row;
		g_dirty = TRUE;
		return;
	default:
		break;
	}
	if ( c >= 0x20 && c < 0x7F ) {
		/* a letter begins typing, through the converter like the rest */
		ed_begin(g_col, g_row, NULL, FALSE);
		(void)ed_key(m->code, m->mods, m->imode);
	}
}

/* ---------------------------------------------------------------- the pointer */

static INT chart_at( INT x, INT y )
{
	INT		i;
	T_DPRECT	r;

	for ( i = g_s.nch - 1; i >= 0; i-- ) {
		chart_box(i, &r);
		if ( x >= r.left && x <= r.right && y >= r.top && y <= r.bottom && y >= grid_top() ) return i;
	}
	return -1;
}

/* The link or image under a place, in the cell of that place or one before it whose box reaches there */
static CXCELL *link_at( INT x, INT y )
{
	INT	i;

	for ( i = g_s.n - 1; i >= 0; i-- ) {
		CXCELL		*c = g_s.c[i];
		T_DPRECT	r;

		if ( c->kind == CX_TEXT ) continue;
		if ( c->kind == CX_LINK && !g_show_hidden && ct_attr_bool(c->el, c->ellen, "hidden", FALSE) ) continue;
		link_box(c, &r);
		if ( x >= r.left && x < r.right && y >= r.top && y < r.bottom ) return c;
	}
	return NULL;
}

/*
 * The link taken (in the cell taken, g_link_on) when a place is just
 * out of its right edge or its foot, no farther than slop: what a
 * finger's press there is to size. NULL when it is not.
 */
static CXCELL *link_near( INT x, INT y, INT slop )
{
	INT	i;

	for ( i = g_s.n - 1; i >= 0; i-- ) {
		CXCELL		*c = g_s.c[i];
		T_DPRECT	r;

		if ( c->kind != CX_LINK || c->col != g_col || c->row != g_row ) continue;
		link_box(c, &r);
		if ( x >= r.left && x < r.right + slop && y >= r.top && y < r.bottom + slop
		  && ( x >= r.right || y >= r.bottom ) ) return c;
	}
	return NULL;
}

static void press( CONST T_OBNTM *m )
{
	BOOL		shift = (BOOL)( ( m->mods & M_SHIFT ) != 0 );
	BOOL		twice;
	INT		x = m->x, y = m->y, col, row, i;
	CXRANGE		r;
	T_DPRECT	b;
	CXCELL		*c;
	INT		slop, e;

	if ( x < 0 || y < 0 || x >= g_w || y >= g_h ) return;	/* the band, the frame: the desktop's */
	slop = wm_hit_slop(m->when);		/* a finger reaches the edges and handles farther */
	if ( ed_on() ) {
		ed_commit();
	}
	g_hold = HOLD_NONE;
	g_hmoved = FALSE;
	g_hx0 = g_hx = x;
	g_hy0 = g_hy = y;

	/* the letters and numbers: an edge sizes, the rest takes the whole column or row */
	if ( y < grid_top() && x >= grid_left() ) {
		col = col_at(x - grid_left() + g_sx);
		e = wm_hit_in(EDGE, slop, col_x(col + 1) - col_x(col));
		if ( x >= grid_left() + col_x(col + 1) - g_sx - e ) {
			g_hold = HOLD_COLW;
			g_hat = col;
		} else if ( x < grid_left() + col_x(col) - g_sx + e && col > 1 ) {
			g_hold = HOLD_COLW;
			g_hat = col - 1;
		} else {
			g_col = g_acol = col;
			g_row = 1;
			g_arow = g_s.maxrow;
			if ( shift ) g_acol = g_col;
			g_cells_on = TRUE;
			g_chart = -1;
			g_link_on = FALSE;
			g_hold = HOLD_COLS;
		}
		g_dirty = TRUE;
		return;
	}
	if ( x < grid_left() && y >= grid_top() ) {
		row = row_at(y - grid_top() + g_sy);
		e = wm_hit_in(EDGE, slop, row_y(row + 1) - row_y(row));
		if ( y >= grid_top() + row_y(row + 1) - g_sy - e ) {
			g_hold = HOLD_ROWH;
			g_hat = row;
		} else if ( y < grid_top() + row_y(row) - g_sy + e && row > 1 ) {
			g_hold = HOLD_ROWH;
			g_hat = row - 1;
		} else {
			g_row = g_arow = row;
			g_col = 1;
			g_acol = g_s.maxcol;
			g_cells_on = TRUE;
			g_chart = -1;
			g_link_on = FALSE;
			g_hold = HOLD_ROWS;
		}
		g_dirty = TRUE;
		return;
	}
	if ( y < grid_top() || x < grid_left() ) return;

	/* a chart: its corner sizes it, the rest moves it */
	if ( g_chart >= 0 ) {
		chart_box(g_chart, &b);
		if ( x >= b.right - wm_hit_in(CHART_GRIP, slop, b.right - b.left) && x <= b.right + CHART_GRIP + slop
		  && y >= b.bottom - wm_hit_in(CHART_GRIP, slop, b.bottom - b.top) && y <= b.bottom + CHART_GRIP + slop ) {
			g_hold = HOLD_CSIZE;
			return;
		}
	}
	i = chart_at(x, y);
	if ( i >= 0 ) {
		g_chart = i;
		g_cells_on = FALSE;
		g_link_on = FALSE;
		g_hold = HOLD_CHART;
		g_chart_was[0] = g_s.ch[i].left;
		g_chart_was[1] = g_s.ch[i].top;
		g_chart_was[2] = g_s.ch[i].right;
		g_chart_was[3] = g_s.ch[i].bottom;
		g_dirty = TRUE;
		return;
	}

	/* the fill handle */
	if ( g_cells_on ) {
		range_of(&r);
		(void)cell_box(r.c1, r.r1, &b);
		if ( x >= b.right - wm_hit_in(FILL_BOX, slop, b.right - b.left) && x <= b.right + FILL_BOX + slop
		  && y >= b.bottom - wm_hit_in(FILL_BOX, slop, b.bottom - b.top) && y <= b.bottom + FILL_BOX + slop ) {
			g_fill0 = r;
			g_hold = HOLD_FILL;
			return;
		}
	}

	col = col_at(x - grid_left() + g_sx);
	row = row_at(y - grid_top() + g_sy);
	twice = (BOOL)( col == g_press_col && row == g_press_row && m->when - g_press_at < DBL_NS );
	g_press_at = m->when;
	g_press_col = col;
	g_press_row = row;

	/* a link in a cell; with a finger, the link taken by its edges from just out of it */
	c = link_at(x, y);
	if ( c == NULL && slop > 0 && g_link_on && !shift ) {
		CXCELL	*t = link_near(x, y, slop);

		if ( t != NULL ) {
			c = t;
			twice = FALSE;
		}
	}
	if ( c != NULL && !shift ) {
		T_DPRECT	lb;

		link_box(c, &lb);
		/* farther in only on the link taken before the press */
		if ( !( g_link_on && c->col == g_col && c->row == g_row ) ) slop = 0;
		take_cell(c->col, c->row, FALSE);
		g_link_on = TRUE;
		g_htwice = twice;
		g_hright = (BOOL)( c->kind == CX_LINK && x >= lb.right - wm_hit_in(EDGE, slop, lb.right - lb.left) );
		g_hfoot = (BOOL)( c->kind == CX_LINK && y >= lb.bottom - wm_hit_in(EDGE, slop, lb.bottom - lb.top) );
		g_hold = ( ( g_hright || g_hfoot ) && !twice ) ? HOLD_LSIZE : HOLD_LINK;
		g_press_col = c->col;
		g_press_row = c->row;
		return;
	}
	if ( twice && !shift ) {
		CONST CXCELL	*d = cx_get(&g_s, col, row);
		INT		mg = cx_merge_of(&g_s, col, row);

		if ( mg >= 0 ) {
			col = g_s.m[mg].c0;
			row = g_s.m[mg].r0;
		}
		take_cell(col, row, FALSE);
		if ( d == NULL || d->kind == CX_TEXT ) ed_begin(col, row, NULL, TRUE);
		return;
	}
	take_cell(col, row, shift);
	g_hold = HOLD_CELLS;
}

static void moved( CONST T_OBNTM *m )
{
	INT	x = m->x, y = m->y, col, row;

	if ( g_hold == HOLD_NONE ) return;
	g_hx = x;
	g_hy = y;
	if ( x - g_hx0 >= MOVE_MIN || g_hx0 - x >= MOVE_MIN || y - g_hy0 >= MOVE_MIN || g_hy0 - y >= MOVE_MIN ) g_hmoved = TRUE;
	col = col_at(x - grid_left() + g_sx);
	row = row_at(y - grid_top() + g_sy);
	switch ( g_hold ) {
	case HOLD_CELLS:
		if ( !g_hmoved ) return;
		g_col = col;
		g_row = row;
		cx_grow(&g_s, col, row);
		break;
	case HOLD_COLS:
		g_acol = col;
		break;
	case HOLD_ROWS:
		g_arow = row;
		break;
	case HOLD_FILL:
		{
			CXRANGE	r = g_fill0;

			if ( col > r.c1 ) r.c1 = col;
			else if ( col < r.c0 ) r.c0 = col;
			if ( row > r.r1 ) r.r1 = row;
			else if ( row < r.r0 ) r.r0 = row;
			g_col = r.c1;
			g_row = r.r1;
			g_acol = r.c0;
			g_arow = r.r0;
		}
		break;
	case HOLD_CHART:
		if ( g_chart >= 0 ) {
			CXCHART	*c = &g_s.ch[g_chart];
			INT	w = c->right - c->left, h = c->bottom - c->top;

			c->left = g_chart_was[0] + ( x - g_hx0 );
			c->top = g_chart_was[1] + ( y - g_hy0 );
			c->right = c->left + w;
			c->bottom = c->top + h;
		}
		break;
	case HOLD_CSIZE:
		if ( g_chart >= 0 ) {
			CXCHART		*c = &g_s.ch[g_chart];
			T_DPRECT	b;

			chart_box(g_chart, &b);
			c->right = c->left + ( ( x - b.left > CHART_MIN ) ? x - b.left : CHART_MIN );
			c->bottom = c->top + ( ( y - b.top > CHART_MIN ) ? y - b.top : CHART_MIN );
		}
		break;
	case HOLD_LINK:
		/* carried out of the window: the desktop takes it on */
		if ( x < 0 || y < 0 || x >= g_w || y >= g_h ) {
			INT	hx = g_hx0, hy = g_hy0;
			BOOL	tw = g_htwice;

			g_hold = HOLD_NONE;
			carry_out(hx, hy, tw);
		}
		break;
	default:
		break;
	}
	g_dirty = TRUE;
}

static void released( CONST T_OBNTM *m )
{
	INT	hold = g_hold, col, row;
	char	s[80];

	g_hold = HOLD_NONE;
	col = col_at(m->x - grid_left() + g_sx);
	row = row_at(m->y - grid_top() + g_sy);
	switch ( hold ) {
	case HOLD_COLW:
		if ( g_hmoved || m->x != g_hx0 ) {
			INT	w = m->x - ( grid_left() + col_x(g_hat) - g_sx );

			if ( w < 20 ) w = 20;
			cx_set_width(&g_s, g_hat, w);
			g_changed = TRUE;
			snprintf(s, sizeof(s), "column %d width %d", g_hat, w);
			note(s);
		}
		break;
	case HOLD_ROWH:
		if ( g_hmoved || m->y != g_hy0 ) {
			INT	h = m->y - ( grid_top() + row_y(g_hat) - g_sy );

			if ( h < 12 ) h = 12;
			cx_set_height(&g_s, g_hat, h);
			g_changed = TRUE;
			snprintf(s, sizeof(s), "row %d height %d", g_hat, h);
			note(s);
		}
		break;
	case HOLD_FILL:
		{
			CXRANGE	d;

			range_of(&d);
			if ( d.c0 != g_fill0.c0 || d.c1 != g_fill0.c1 || d.r0 != g_fill0.r0 || d.r1 != g_fill0.r1 ) {
				fill_out(&g_fill0, &d);
			}
		}
		break;
	case HOLD_CHART:
	case HOLD_CSIZE:
		if ( g_hmoved ) g_changed = TRUE;
		break;
	case HOLD_LINK:
		if ( !g_hmoved && g_htwice ) {
			link_open(g_col, g_row);
		} else if ( g_hmoved && m->y >= grid_top() && m->x >= grid_left() && ( col != g_col || row != g_row ) ) {
			/* to another cell: the link moves there, what it held goes */
			CXCELL	*c = cx_get(&g_s, g_col, g_row);
			UB	*el;
			INT	len;

			if ( c != NULL && c->el != NULL && cx_merge_of(&g_s, col, row) < 0 ) {
				CONST CXSTYLE	*st = c->st;
				INT	kind = c->kind;

				len = c->ellen;
				el = malloc((size_t)len + 1);
				if ( el != NULL ) {
					memcpy(el, c->el, (size_t)len);
					cell_clear(g_col, g_row);
					c = cx_make(&g_s, col, row);
					if ( c != NULL && cx_set_el(c, kind, el, len) ) {
						c->st = st;
						(void)ce_blank(col, row);
						shown_all();
					}
					free(el);
					take_cell(col, row, FALSE);
					g_link_on = TRUE;
					snprintf(s, sizeof(s), "link moved to %d,%d", col, row);
					note(s);
				}
			}
		}
		break;
	case HOLD_LSIZE:
		{
			CXCELL		*c = cx_get(&g_s, g_col, g_row);
			T_DPRECT	lb;

			if ( c != NULL && c->kind == CX_LINK && g_hmoved ) {
				CTSET	set[4];
				char	ws[12], hs[12], rs[12], bs[12];
				INT	w, h, n = 2;

				link_box(c, &lb);
				w = lb.right - lb.left + ( g_hright ? m->x - g_hx0 : 0 );
				h = lb.bottom - lb.top + ( g_hfoot ? m->y - g_hy0 : 0 );
				if ( w < 40 ) w = 40;
				if ( h < 16 ) h = 16;
				snprintf(ws, sizeof(ws), "%d", w);
				snprintf(hs, sizeof(hs), "%d", h);
				set[0].name = "width";
				set[0].value = (CONST UB *)ws;
				set[1].name = "height";
				set[1].value = (CONST UB *)hs;
				if ( ct_attr_int(c->el, c->ellen, "vobjleft", -100000) > -100000 ) {
					snprintf(rs, sizeof(rs), "%d", ct_attr_int(c->el, c->ellen, "vobjleft", 0) + w);
					snprintf(bs, sizeof(bs), "%d", ct_attr_int(c->el, c->ellen, "vobjtop", 0) + h);
					set[2].name = "vobjright";
					set[2].value = (CONST UB *)rs;
					set[3].name = "vobjbottom";
					set[3].value = (CONST UB *)bs;
					n = 4;
				}
				{
					UB	*tmp = malloc((size_t)c->ellen + 256);
					CTBUF	b;

					if ( tmp != NULL ) {
						ct_buf(&b, tmp, c->ellen + 256);
						ct_link_edit(&b, c->el, c->ellen, set, n);
						if ( !b.fail ) (void)cx_set_el(c, CX_LINK, b.p, b.n);
						free(tmp);
					}
				}
				g_changed = TRUE;
				snprintf(s, sizeof(s), "link resized %dx%d", w, h);
				note(s);
			}
		}
		break;
	default:
		break;
	}
	g_dirty = TRUE;
}

static void wheel( CONST T_OBNTM *m )
{
	INT	d = ( m->dz != 0 ) ? m->dz : 0;

	/* the wheel across goes the way it turns; the upright one with Shift held across too */
	if ( m->code == HID_WHEEL_H ) g_sx += d * 3 * CX_CELL_W;
	else if ( ( m->mods & M_SHIFT ) != 0 ) g_sx -= d * 3 * CX_CELL_W;
	else g_sy -= d * 3 * CX_CELL_H;
	if ( g_sx < 0 ) g_sx = 0;
	if ( g_sy < 0 ) g_sy = 0;
	/* near the grid's end, more of it */
	if ( g_sy + g_h > row_y(g_s.maxrow + 1) ) cx_grow(&g_s, 1, g_s.maxrow + 50);
	if ( g_sx + g_w > col_x(g_s.maxcol + 1) ) cx_grow(&g_s, g_s.maxcol + 10, 1);
	g_dirty = TRUE;
}

static void scrolled( CONST T_OBNTM *m )
{
	if ( m->code == OB_BAR_R ) {
		g_sy = m->x;
		if ( g_sy + g_h > row_y(g_s.maxrow + 1) ) cx_grow(&g_s, 1, g_s.maxrow + 50);
	} else if ( m->code == OB_BAR_B ) {
		g_sx = m->x;
		if ( g_sx + g_w > col_x(g_s.maxcol + 1) ) cx_grow(&g_s, g_s.maxcol + 10, 1);
	}
	if ( g_sx < 0 ) g_sx = 0;
	if ( g_sy < 0 ) g_sy = 0;
	g_dirty = TRUE;
}

void links_told( void )
{
	T_DTANS	an;
	SZ	asz = 0;

	while ( g_lport > 0 && ob_rea_tmo(g_lport, 0, 0, &an, sizeof(an), &asz, TMO_POL) >= E_OK ) {
		if ( asz == (SZ)sizeof(an) ) link_told(&an);
	}
}

/* The channel the desktop says on, and a notice of it at the window's channel */
static void links_channel( void )
{
	T_OBCRE	c;
	T_OBNTF	req;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_lch) < E_OK ) return;
	g_lport = ob_opn_obj(&g_lch, OB_OP_READ | OB_OP_ATRRD | OB_O_NOWAIT);
	if ( g_lport <= 0 ) {
		g_lport = 0;
		(void)ob_del_obj(&g_lch);
		return;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 4;
	(void)ob_ntf_evt(g_lport, OB_REC_ANY, &req, g_port);
}

void event( CONST T_OBNTM *m )
{
	if ( panel_is(&m->uuid) ) {
		/* a key with the panel in front is its box's, or the sheet's when the panel leaves it */
		if ( !panel_event(m) && m->event == OB_E_KEY ) key(m);
		return;
	}
	if ( g_lport > 0 && memcmp(&m->uuid, &g_lch, sizeof(TS_UUID)) == 0 ) {
		links_told();
		return;
	}
	if ( memcmp(&m->uuid, &g_win, sizeof(TS_UUID)) != 0 ) return;
	switch ( m->event ) {
	case OB_E_CLOSE:
		close_asked();
		break;
	case OB_E_DELETE:
		g_closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->recno == OB_WR_CAND ) ed_pick((INT)m->code);	/* a name on the list of candidates */
		else if ( m->code == 1 && g_hold == HOLD_NONE ) menu(m);
		else if ( m->code == 0 ) press(m);
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m);
		break;
	case OB_E_MOVE:
		moved(m);
		break;
	case OB_E_LEAVE:
		if ( g_hold == HOLD_LINK ) {
			INT	hx = g_hx0, hy = g_hy0;
			BOOL	tw = g_htwice;

			g_hold = HOLD_NONE;
			carry_out(hx, hy, tw);
		}
		break;
	case OB_E_KEY:
		key(m);
		break;
	case OB_E_WHEEL:
		wheel(m);
		break;
	case OB_E_SCROLL:
		scrolled(m);
		break;
	case OB_E_REDRAW:
		resized();
		break;
	case OB_E_DROP:
		dropped();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBCRE	c;
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0, argn;
	char	title[OB_NAME_MAX + 8], s[160], arg[32];
	ER	er;

	memset(arg, 0, sizeof(arg));
	argn = ts_get_arg(arg, sizeof(arg) - 1);
	if ( argn == 8 && memcmp(arg, "selftest", 8) == 0 ) return selftest();
	if ( argn < (SZ)sizeof(g_obj) || ob_ref_obj((CONST TS_UUID *)arg, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("基本表計算は、表計算の実身を開いて使います");
		return 1;
	}
	memcpy(&g_obj, arg, sizeof(g_obj));
	g_autoref = (BOOL)( ( ref.flags & OB_F_AUTOREF ) != 0 );
	memcpy(g_name, ref.name, OB_NAME_MAX);
	g_name[OB_NAME_MAX - 1] = 0;
	snprintf(title, sizeof(title), "%s", (char *)ref.name);
	cx_init(&g_s);
	if ( ce_start() < 0 ) {
		note("the engine cannot be started");
		(void)wm_msg_put("基本表計算の計算部を起動できません");
		return 2;
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port > 0 ) links_channel();
	if ( g_port <= 0 ) {
		note("no channel");
		return 2;
	}
	er = load();
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "the object cannot be read (%d)", (INT)er);
		note(s);
		(void)wm_msg_put("表計算の実身を読めません");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	if ( win_open(title) < E_OK ) {
		note("no window");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	if ( g_panel_shown ) panel_open();
	snprintf(s, sizeof(s), "ready %s cells %d charts %d joins %d engine %s", title, g_s.n, g_s.nch, g_s.nm, ce_name());
	note(s);
	say("セルを選んで文字を入力すると値、= で始めると数式になります");
	paint();
	bars();

	/* the links that ask to be opened with the sheet */
	{
		INT	i;

		for ( i = 0; i < g_s.n; i++ ) {
			CONST CXCELL	*c = g_s.c[i];

			if ( c->kind == CX_LINK && ct_attr_bool(c->el, c->ellen, "autoopen", FALSE) ) link_open(c->col, c->row);
		}
	}

	while ( !g_closing ) {
		for ( TMO tmo = TMO_FEVR; !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) event(&m);
		links_told();		/* what a panel's wait took the notice of is read too */
		if ( g_dirty && !g_closing ) {
			paint();
			bars();
		}
	}
	if ( g_lport > 0 ) {
		ob_cls_obj(g_lport);
		(void)ob_del_obj(&g_lch);
	}
	ed_end();
	panel_close();
	if ( g_kw > 0 ) ob_cls_obj(g_kw);
	(void)ob_del_obj(&g_win);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	cx_free_all(&g_s);
	ce_end();
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}

/* Memory for the sheet */
void *cx_alloc( INT size )
{
	return malloc((size_t)( size > 0 ? size : 1 ));
}

void cx_free( void *p )
{
	free(p);
}
