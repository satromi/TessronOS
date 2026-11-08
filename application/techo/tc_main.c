/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tc_main.c
 *	電子手帳: 予定表, カレンダー and 住所録 in a window each (design 11.23)
 *
 *	The program is started on a 電子手帳 object (a 手帳用紙) and shows
 *	what it holds in three windows, which the 表示 menu opens and puts
 *	away -- one at least stays; putting the last away closes the
 *	program, as 閉じる does. Each window is a set of pages turned with
 *	its 前へ and 次へ marks and the ページ menu: a day of 1990 to 2050,
 *	a month of the same years, and a person, in the order of よみ. A
 *	date pressed in the カレンダー shows that day in the 予定表.
 *
 *	A 記入欄 of the 予定表 or the 住所録 is pressed to type into it
 *	(tc_edit.c); Tab and Shift+Tab, and Enter, go on to the next or the
 *	one before. Letters taken in a field are carried by the pointer --
 *	moved, or copied when the second button goes down on the way -- to
 *	another field of the same window, or out of it, the desktop
 *	following the hand (DT_RQ_CARRY with DT_F_TEXT), to another window
 *	of the 手帳's, a text of the desktop's or any window that takes
 *	letters. Letters carried here from elsewhere go into the field under
 *	the corner of the outline (OB_E_TEXT). They go to the tray and come
 *	from it with the 編集 menu as well. A change of よみ sorts the 住所録 at
 *	once; the person stays shown, the index and the place updated.
 *
 *	The 検索パネル is a window of its own (a panel of the data box): its
 *	words are looked for from the first page or the page after the one
 *	shown, in whichever of the 予定表 and the 住所録 is in front.
 *
 *	印刷 makes the day or the person shown, or all of them, a document
 *	on the paper of 用紙設定 and hands it to the desktop (DT_RQ_PRINT).
 *
 *	Nothing is written to the object until it is saved; closing a
 *	手帳 changed asks first, and 保存しない is how a change is undone.
 *	What was done is said on the message line and, for the tests, on
 *	the console ("techo: ...").
 */

#include "tc.h"
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/obdrop.h>

/* The panels of the data box */
#define PN_FIND		1		/* 検索パネル */
#define PN_PRINT	2		/* which pages to print */
#define PN_PAPER	3		/* レイアウト用紙設定 */
#define PN_CLOSE	4		/* save before closing */
#define PN_MSG		5		/* a message to be confirmed */
#define PN_MONTH	6		/* a year and a month for the カレンダー */
#define P_YM		11		/* its year and month */
#define P_YM_OK		102		/* and 移動 */
#define P_FIND_TEXT	11
#define P_FIND_FIRST	101
#define P_FIND_NEXT	102
#define B_CANCEL	101
#define B_OK		102
#define B_OTHER		103

/* ---------------------------------------------------------------- state */

TCDB	g_db;
TS_UUID	g_obj;
UB	g_name[OB_NAME_MAX];
TCWIN	g_w[W_N];
UI	g_ui;
ID	g_port;
BOOL	g_changed, g_closing;
INT	g_day, g_cy, g_cm, g_cur, g_today;
UB	g_rows[TC_ROWS][TC_LINE_MAX];
BOOL	g_idx_open;
INT	g_drop_w = -1, g_drop_f = -1, g_drop_x;
UB	g_out[OUT_MAX];

static TS_UUID	g_ch;
static char	g_meta_none[1];
static char	*g_meta = g_meta_none;	/* the metadata meta_of read last ("" when none) */
static INT	g_last = W_SCHED;	/* the window last pressed or typed in */
static INT	g_front = W_SCHED;	/* and of the 予定表 and 住所録 */

static UI	g_find;			/* the 検索パネル */
static BOOL	g_find_open;

/* What the button down is doing */
#define HOLD_NONE	0
#define HOLD_SELECT	1		/* taking letters in the field being typed */
#define HOLD_CARRY	2		/* carrying the letters taken */
#define HOLD_INDEX	3		/* the index pressed: its list is out */
static INT	g_hold = HOLD_NONE;
static INT	g_hold_w;
static INT	g_hx0, g_hy0;
static BOOL	g_hold_moved, g_hold_copy;

/* ---------------------------------------------------------------- saying things */

void note( CONST char *s )
{
	char	line[300];

	snprintf(line, sizeof(line), "techo: %s\n", s);
	tm_putstring((CONST UB *)line);
}

void say( CONST char *s )
{
	(void)wm_msg_put(s);
}

/* What the engine asks of the program */
void *tc_alloc( INT size )
{
	return malloc((size_t)( size > 0 ? size : 1 ));
}

void tc_free( void *p )
{
	free(p);
}

/* ---------------------------------------------------------------- the object */

/* Record 0, whole; NULL when it cannot be read */
static UB *rec_read( INT *p_len )
{
	T_OBREC	r[16];
	INT	cnt = 0, i, at = 0;
	UD	size = 0;
	SZ	asz = 0;
	UB	*buf = NULL;
	BOOL	found = FALSE;
	ID	k;

	*p_len = 0;
	k = ob_opn_obj(&g_obj, OB_OP_R);
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 16; i++ ) {
			if ( r[i].recno == 0 ) {
				size = r[i].size;
				found = TRUE;
			}
		}
	}
	if ( found && size < OUT_MAX ) buf = malloc((size_t)size + 1);
	while ( buf != NULL && (UD)at < size ) {
		SZ	n = ( size - at > CHUNK ) ? CHUNK : (SZ)( size - at );

		if ( ob_rea_rec(k, 0, (D)at, buf + at, n, &asz) < E_OK || asz <= 0 ) break;
		at += (INT)asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL && (UD)at != size ) {
		free(buf);
		buf = NULL;
	}
	if ( buf != NULL ) {
		buf[size] = 0;
		*p_len = (INT)size;
	}
	return buf;
}

static ER rec_write( CONST UB *data, INT len )
{
	ID	k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_W);
	ER	er;

	if ( k <= 0 ) return (ER)k;
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(k, 0, data, (SZ)len);
	ob_cls_obj(k);
	return er;
}

static INT meta_of( CONST TS_UUID *u )
{
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	/* the metadata whole, in memory of its own: the last one read let go */
	if ( g_meta != g_meta_none ) free(g_meta);
	g_meta = g_meta_none;
	if ( k <= 0 ) return 0;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n <= 0 ) {
		free(m);
		return 0;
	}
	g_meta = (char *)m;
	return n;
}

static ER load( void )
{
	INT	n = 0;
	UB	*x = rec_read(&n);
	ER	er;
	char	s[120];

	if ( x == NULL ) return E_IO;
	er = tc_read(&g_db, x, n);
	free(x);
	if ( er < E_OK ) return er;
	/* a 住所録 has a page, empty on a new 手帳用紙 */
	if ( g_db.na == 0 && tc_addr_add(&g_db) < 0 ) return E_NOMEM;
	snprintf(s, sizeof(s), "read plans %d people %d holidays %d+%d dropped %d", g_db.np, g_db.na, g_db.nr,
		 (INT)( ( g_db.week & 0x7F ) != 0 ), g_db.dropped);
	note(s);
	return E_OK;
}

static ER save( void )
{
	CTBUF	b;
	ER	er;

	ed_commit();
	ct_buf(&b, g_out, OUT_MAX);
	er = tc_write(&g_db, &b, g_name);
	if ( er >= E_OK ) er = rec_write(b.p, b.n);
	if ( er >= E_OK ) {
		g_changed = FALSE;
		note("saved");
	}
	return er;
}

static void today( void )
{
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		t[48];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&clk, OB_OP_READ);
	INT		y = 2026, m = 1, d = 1;

	if ( k > 0 ) {
		/* "2026-09-25T18:30:12+09:00", the time where the person is */
		if ( ob_rea_rec(k, 1, 0, t, sizeof(t) - 1, &asz) >= E_OK && asz >= 10 ) {
			t[asz] = 0;
			y = atoi((char *)t);
			m = atoi((char *)t + 5);
			d = atoi((char *)t + 8);
		}
		ob_cls_obj(k);
	}
	g_today = tc_day(y, m, d);
	if ( !tc_day_ok(g_today) ) g_today = tc_day(2026, 1, 1);
}

/* ---------------------------------------------------------------- the pages shown */

static void rows_load( void )
{
	CONST UB	*r[TC_ROWS];
	INT		n = tc_plans(&g_db, g_day, r, TC_ROWS), i;

	for ( i = 0; i < TC_ROWS; i++ ) {
		g_rows[i][0] = 0;
		if ( i < n ) {
			strncpy((char *)g_rows[i], (CONST char *)r[i], TC_LINE_MAX - 1);
			g_rows[i][TC_LINE_MAX - 1] = 0;
		}
	}
}

static void show_day( INT day )
{
	char	s[64];
	INT	y, m, d;

	if ( day < TC_DAY_FIRST ) day = TC_DAY_FIRST;
	if ( day > TC_DAY_LAST ) day = TC_DAY_LAST;
	if ( ed_win() == W_SCHED ) ed_commit();
	g_day = day;
	rows_load();
	tc_day_split(day, &y, &m, &d);
	snprintf(s, sizeof(s), "day %d/%d/%d%s", y, m, d, tc_holiday(&g_db, y, m, d) ? " holiday" : "");
	note(s);
	g_w[W_SCHED].dirty = TRUE;
}

static void show_month( INT y, INT m )
{
	char	s[48];

	if ( y < TC_Y_FIRST ) {
		y = TC_Y_FIRST;
		m = 1;
	}
	if ( y > TC_Y_LAST ) {
		y = TC_Y_LAST;
		m = 12;
	}
	g_cy = y;
	g_cm = m;
	snprintf(s, sizeof(s), "month %d/%d", y, m);
	note(s);
	g_w[W_CAL].dirty = TRUE;
}

static void show_person( INT i )
{
	char	s[TC_LINE_MAX + 48];
	UB	pt[24];
	INT	pos = 0, count = 0;

	if ( ed_win() == W_ADDR ) ed_commit();
	if ( i >= g_db.na ) i = g_db.na - 1;
	if ( i < 0 ) i = 0;
	g_cur = i;
	tc_index_pos(&g_db, i, &pos, &count);
	tc_pos_text(pos, count, pt, sizeof(pt));
	snprintf(s, sizeof(s), "person %d of %d %s %s [%s]", i + 1, g_db.na,
		 tc_index_name(tc_index_of(tc_addr_get(&g_db, i, TC_F_YOMI))), (char *)pt,
		 (CONST char *)tc_addr_get(&g_db, i, TC_F_NAME));
	note(s);
	g_w[W_ADDR].dirty = TRUE;
}

/* ---------------------------------------------------------------- the 記入欄 */

INT field_count( INT w )
{
	return ( w == W_SCHED ) ? TC_ROWS : ( w == W_ADDR ) ? TC_NF : 0;
}

CONST UB *field_text( INT w, INT f )
{
	if ( w == W_SCHED && f >= 0 && f < TC_ROWS ) return g_rows[f];
	if ( w == W_ADDR && f >= 0 && f < TC_NF ) return tc_addr_get(&g_db, g_cur, tc_show_order[f]);
	return (CONST UB *)"";
}

void field_commit( INT w, INT f, CONST UB *s, INT n )
{
	char	line[TC_LINE_MAX + 48];

	if ( strlen((CONST char *)field_text(w, f)) == (size_t)n && memcmp(field_text(w, f), s, (size_t)n) == 0 ) return;
	if ( w == W_SCHED ) {
		CONST UB	*r[TC_ROWS];
		INT		i;

		memcpy(g_rows[f], s, (size_t)n);
		g_rows[f][n] = 0;
		for ( i = 0; i < TC_ROWS; i++ ) r[i] = g_rows[i];
		if ( tc_set_plans(&g_db, g_day, r, TC_ROWS) < E_OK ) say("メモリが足りません");
		snprintf(line, sizeof(line), "set row %d %s", f + 1, (char *)g_rows[f]);
	} else if ( w == W_ADDR ) {
		INT	fld = tc_show_order[f], was = g_cur, pos = 0, count = 0;
		UB	t[TC_LINE_MAX], pt[24];

		memcpy(t, s, (size_t)n);
		t[n] = 0;
		g_cur = tc_addr_set(&g_db, g_cur, fld, t);
		if ( fld == TC_F_YOMI ) {
			tc_index_pos(&g_db, g_cur, &pos, &count);
			tc_pos_text(pos, count, pt, sizeof(pt));
			snprintf(line, sizeof(line), "set %s %s sorted %d to %d %s %s", tc_field_name[fld], (char *)t,
				 was + 1, g_cur + 1, tc_index_name(tc_index_of(t)), (char *)pt);
		} else {
			snprintf(line, sizeof(line), "set %s %s", tc_field_name[fld], (char *)t);
		}
	} else {
		return;
	}
	g_changed = TRUE;
	note(line);
	g_w[w].dirty = TRUE;
}

/* Typing begun in a field, the caret where x is (or at the end, x < 0) */
static void field_begin( INT w, INT f, INT x )
{
	char	s[48];

	if ( ed_on() && ed_win() == w && ed_field() == f ) {
		if ( x >= 0 ) ed_caret_at(x, FALSE);
		return;
	}
	ed_commit();
	ed_begin(w, f, field_text(w, f));
	if ( x >= 0 ) ed_caret_at(x, FALSE);
	snprintf(s, sizeof(s), "field %s %d", ( w == W_SCHED ) ? "sched" : "addr", f + 1);
	note(s);
}

/* The next field (or the one before), typing begun in it */
static void field_step( INT w, INT d )
{
	INT	n = field_count(w), f = ed_on() && ed_win() == w ? ed_field() : -1;

	if ( n == 0 ) return;
	f = ( f < 0 ) ? ( d > 0 ? 0 : n - 1 ) : ( f + d + n ) % n;
	ed_commit();
	field_begin(w, f, -1);
	ed_select_all();
}

/* ---------------------------------------------------------------- the windows */

static void work_size( TCWIN *w )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		w->w = wp.wright - wp.wleft;
		w->h = wp.wbottom - wp.wtop;
		w->x = wp.left;
		w->y = wp.top;
	}
}

static CONST INT win_w[W_N] = { SCHED_W, CAL_W, ADDR_W };
static CONST INT win_h[W_N] = { SCHED_H, CAL_H, ADDR_H };
static CONST char *CONST win_title[W_N] = { "予定表", "カレンダー", "住所録" };

#define WIN_EVENTS	( OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW \
			| OB_E_DELETE | OB_E_DROP | OB_E_TEXT | OB_E_LEAVE )

static ER win_open( INT k )
{
	TCWIN		*w = &g_w[k];
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[280], title[OB_NAME_MAX + 32];
	char		shown[TS_UUID_STRLEN + 1];

	if ( w->open ) return E_OK;
	snprintf(title, sizeof(title), "%s（%s）", (char *)g_name, win_title[k]);
	ts_uuid_text(&g_obj, shown);	/* what it shows, for ウインドウ一覧 */
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 w->x, w->y, w->x + win_w[k] + FRAME_W, w->y + win_h[k] + TITLE_H,
		 WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, SYSDEF_PROG_TECHO, shown);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &w->win) < E_OK ) return E_OBJ;
	w->kw = ob_opn_obj(&w->win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	w->gid = ( w->kw > 0 ) ? wm_obj_gid(w->kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = WIN_EVENTS;
	if ( w->kw <= 0 || w->gid < 0 || ob_ntf_evt(w->kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( w->kw > 0 ) ob_cls_obj(w->kw);
		(void)ob_del_obj(&w->win);
		w->kw = 0;
		w->gid = -1;
		return E_OBJ;
	}
	/* the work area the size asked for, whatever the frame takes */
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + win_w[k] + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + win_h[k] + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	w->open = TRUE;
	w->dirty = TRUE;
	work_size(w);
	snprintf(title, sizeof(title), "window %s on", ( k == W_SCHED ) ? "sched" : ( k == W_CAL ) ? "cal" : "addr");
	note(title);
	return E_OK;
}

static void win_close( INT k )
{
	TCWIN	*w = &g_w[k];
	char	s[40];

	if ( !w->open ) return;
	if ( ed_win() == k ) ed_commit();
	if ( k == W_ADDR ) g_idx_open = FALSE;
	work_size(w);
	w->open = FALSE;
	if ( w->kw > 0 ) ob_cls_obj(w->kw);
	(void)ob_del_obj(&w->win);
	w->kw = 0;
	w->gid = -1;
	snprintf(s, sizeof(s), "window %s off", ( k == W_SCHED ) ? "sched" : ( k == W_CAL ) ? "cal" : "addr");
	note(s);
}

static INT open_count( void )
{
	INT	k, n = 0;

	for ( k = 0; k < W_N; k++ ) {
		if ( g_w[k].open ) n++;
	}
	return n;
}

static void win_front( INT k )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( !g_w[k].open ) return;
	if ( ob_rea_rec(g_w[k].kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.z = 0;
		(void)ob_wri_rec(g_w[k].kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
}

/* Of the windows open, the one nearest the viewer; -1 none */
static INT win_frontmost( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		k, best = -1, bz = 0x7FFFFFFF;

	for ( k = 0; k < W_N; k++ ) {
		if ( !g_w[k].open ) continue;
		if ( ob_rea_rec(g_w[k].kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && wp.z < bz ) {
			bz = wp.z;
			best = k;
		}
	}
	return best;
}

/*
 * The windows as they were left: where each stood and whether it was
 * shown, kept in the object's metadata as
 * "tessronos":{"techo":{"windows":[{"x":..,"y":..,"open":..}, ...]}},
 * the 予定表, the カレンダー and the 住所録 in that order.
 */
static BOOL	g_want[W_N] = { TRUE, TRUE, TRUE };	/* the windows to open */

static BOOL kept_places( T_JSON *root )
{
	T_JSON	tf, tc, arr, it;
	INT	k = 0, x[W_N], y[W_N], shown = 0;
	BOOL	on[W_N];

	if ( js_get(root, "tessronos", &tf) < E_OK || js_get(&tf, "techo", &tc) < E_OK
	  || js_get(&tc, "windows", &arr) < E_OK || js_type(&arr) != JS_ARRAY || js_count(&arr) != W_N ) return FALSE;
	for ( it.s = NULL; k < W_N && js_next(&arr, &it); k++ ) {
		if ( js_type(&it) != JS_OBJECT ) return FALSE;
		x[k] = (INT)js_get_num(&it, "x", -1);
		y[k] = (INT)js_get_num(&it, "y", -1);
		on[k] = js_get_bool(&it, "open", TRUE);
		if ( x[k] < 0 || y[k] < 0 ) return FALSE;
		if ( on[k] ) shown++;
	}
	if ( k != W_N ) return FALSE;
	for ( k = 0; k < W_N; k++ ) {
		g_w[k].x = x[k];
		g_w[k].y = y[k];
		g_want[k] = ( shown > 0 ) ? on[k] : TRUE;	/* one at least */
	}
	return TRUE;
}

/*
 * The windows' places and which are shown written into the metadata, the
 * rest of it as it was; nothing written when they are as it says already
 * or when no window is open.
 */
static void places_keep( void )
{
	char	v[200], *nm;
	INT	mn, k, at, n, vn;
	T_JSON	root, tf, tc;
	ID	key;
	BOOL	ok = FALSE;

	if ( open_count() == 0 ) return;
	for ( k = 0; k < W_N; k++ ) {
		if ( g_w[k].open ) work_size(&g_w[k]);
	}
	vn = snprintf(v, sizeof(v), "{\"windows\":[{\"x\":%d,\"y\":%d,\"open\":%s},{\"x\":%d,\"y\":%d,\"open\":%s},"
		      "{\"x\":%d,\"y\":%d,\"open\":%s}]}",
		      g_w[W_SCHED].x, g_w[W_SCHED].y, g_w[W_SCHED].open ? "true" : "false",
		      g_w[W_CAL].x, g_w[W_CAL].y, g_w[W_CAL].open ? "true" : "false",
		      g_w[W_ADDR].x, g_w[W_ADDR].y, g_w[W_ADDR].open ? "true" : "false");
	mn = meta_of(&g_obj);
	if ( mn <= 0 || js_parse((CONST UB *)g_meta, mn, &root) < E_OK || js_type(&root) != JS_OBJECT ) return;
	nm = malloc((size_t)mn + sizeof(v) + 32);
	if ( nm == NULL ) return;
	if ( js_get(&root, "tessronos", &tf) >= E_OK && js_type(&tf) == JS_OBJECT ) {
		if ( js_get(&tf, "techo", &tc) >= E_OK ) {
			/* the member's value replaced */
			if ( tc.len == vn && memcmp(tc.s, v, (size_t)vn) == 0 ) {
				free(nm);
				return;
			}
			at = (INT)( (CONST char *)tc.s - g_meta );
			n = snprintf(nm, (size_t)mn + sizeof(v) + 32, "%.*s%s%s", at, g_meta, v, g_meta + at + tc.len);
		} else {
			/* a member put first in "tessronos" */
			at = (INT)( (CONST char *)tf.s - g_meta ) + 1;
			for ( k = at; k < mn && ( g_meta[k] == ' ' || g_meta[k] == '\t' || g_meta[k] == '\r' || g_meta[k] == '\n' ); k++ ) ;
			n = snprintf(nm, (size_t)mn + sizeof(v) + 32, "%.*s\"techo\":%s%s%s", at, g_meta, v,
				     ( k < mn && g_meta[k] != '}' ) ? "," : "", g_meta + at);
		}
	} else {
		/* "tessronos" put last in the metadata */
		for ( at = mn; at > 0 && g_meta[at - 1] != '}'; at-- ) ;
		if ( at == 0 ) {
			free(nm);
			return;
		}
		at--;
		for ( k = at; k > 0 && ( g_meta[k - 1] == ' ' || g_meta[k - 1] == '\t' || g_meta[k - 1] == '\r'
					|| g_meta[k - 1] == '\n' ); k-- ) ;
		n = snprintf(nm, (size_t)mn + sizeof(v) + 32, "%.*s%s\"tessronos\":{\"techo\":%s}%s", at, g_meta,
			     ( k > 0 && g_meta[k - 1] != '{' ) ? "," : "", v, g_meta + at);
	}
	key = ob_opn_obj(&g_obj, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key > 0 ) {
		ok = (BOOL)( n > 0 && ob_set_atr(key, (CONST UB *)nm, (SZ)n) >= E_OK );
		ob_cls_obj(key);
	}
	free(nm);
	if ( ok ) note("places kept");
}

/* Where each window opens first: as it was left, else from the object's place, side by side, kept on the screen */
static void places( void )
{
	T_JSON	root, w, pos;
	INT	x = 100, y = 70, mn = meta_of(&g_obj), sw = 1024, sh = 768, k;
	BOOL	kept = FALSE;

	if ( mn > 0 && js_parse((CONST UB *)g_meta, mn, &root) >= E_OK ) {
		kept = kept_places(&root);
		if ( !kept && js_get(&root, "window", &w) >= E_OK && js_get(&w, "pos", &pos) >= E_OK ) {
			x = (INT)js_get_num(&pos, "x", x);
			y = (INT)js_get_num(&pos, "y", y);
		}
	}
	if ( !kept ) {
		g_w[W_SCHED].x = x;
		g_w[W_SCHED].y = y;
		g_w[W_CAL].x = x + SCHED_W + FRAME_W + 12;
		g_w[W_CAL].y = y;
		g_w[W_ADDR].x = x + SCHED_W + FRAME_W + 12;
		g_w[W_ADDR].y = y + CAL_H + TITLE_H + 12;
	}
	{
		/* the screen's size, from the clock's window if there is one of ours later; else the usual */
		T_OBWPOS	wp;
		SZ		asz = 0;

		if ( g_w[W_SCHED].open && ob_rea_rec(g_w[W_SCHED].kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK
		  && wp.sw > 0 ) {
			sw = wp.sw;
			sh = wp.sh;
		}
	}
	for ( k = 0; k < W_N; k++ ) {
		if ( g_w[k].x + win_w[k] + FRAME_W > sw ) g_w[k].x = sw - win_w[k] - FRAME_W;
		if ( g_w[k].y + win_h[k] + TITLE_H > sh - 24 ) g_w[k].y = sh - 24 - win_h[k] - TITLE_H;
		if ( g_w[k].x < 0 ) g_w[k].x = 0;
		if ( g_w[k].y < 0 ) g_w[k].y = 0;
	}
}

static void view( INT k )
{
	if ( g_w[k].open ) {
		if ( open_count() <= 1 ) {
			say("ウインドウを1つは表示しておきます");
			return;
		}
		win_close(k);
		return;
	}
	if ( win_open(k) < E_OK ) {
		say("ウインドウを開けません");
		return;
	}
	win_front(k);
}

/* ---------------------------------------------------------------- the desktop */

/* What came while the desktop was asked: what is not the person's input is heard */
static void event( CONST T_OBNTM *m );

static void told( CONST T_OBNTM *m );

static void settle( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( !g_closing && ob_rea_rec(g_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( ( m.event & ( OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_ENTER
				 | OB_E_LEAVE | OB_E_WHEEL ) ) != 0 ) continue;
		told(&m);
	}
}

static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0;
	ID		k, ka;
	ER		er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	rq->win = g_w[g_last].open ? g_w[g_last].win : g_w[win_frontmost() >= 0 ? win_frontmost() : 0].win;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	/* the answer waited for on its channel, at most wait_ms */
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	settle();
	return er;
}

/* ---------------------------------------------------------------- panels asked */

/* g_ui made to ask over the window k */
static void ask_over( INT k )
{
	if ( k < 0 || k >= W_N || !g_w[k].open ) k = win_frontmost();
	if ( k < 0 ) return;
	g_ui.kw = g_w[k].kw;
	g_ui.w = g_w[k].w;
	g_ui.h = g_w[k].h;
}

static void redraw_dirty( void )
{
	INT	k;

	for ( k = 0; k < W_N; k++ ) {
		if ( g_w[k].open && g_w[k].dirty ) draw(k);
	}
}

/* A panel worked until a button is pushed: its number, 0 when it was put away */
static INT work( UI *q )
{
	T_OBNTM	m;
	INT	num, ev;

	/* the notices waited for; the windows drawn again once none is waiting */
	for ( ;; ) {
		if ( ui_wait(&g_ui, &m, TMO_FEVR) < E_OK ) return 0;
		do {
			ev = ui_dlg_take(q, &m, &num);
			if ( ev == UI_EV_BUTTON ) return num;
			if ( ev == UI_EV_CLOSE ) return 0;
			if ( ev == UI_EV_OTHER && m.event == OB_E_REDRAW ) event(&m);
			if ( g_closing ) return 0;
		} while ( ui_wait(&g_ui, &m, TMO_POL) >= E_OK );
		redraw_dirty();
	}
}

static void warn( CONST char *a, CONST char *b )
{
	CONST char	*l[2] = { a, ( b != NULL ) ? b : " " };

	ask_over(g_last);
	(void)ui_ask(&g_ui, PN_MSG, l, 2);
	redraw_dirty();
}

/* ---------------------------------------------------------------- 検索 */

static void find_open( void )
{
	/* right of the カレンダー's place, out of the way of the pages it finds */
	INT	x = g_w[W_CAL].x + CAL_W + FRAME_W + 12, y = g_w[W_CAL].y;

	if ( g_find_open ) {
		ui_close(&g_find);
		g_find_open = FALSE;
		note("find panel off");
		return;
	}
	{
		T_OBWPOS	wp;
		SZ		asz = 0;
		INT		k = win_frontmost();

		if ( k >= 0 && ob_rea_rec(g_w[k].kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK
		  && wp.sw > 0 && x + 360 > wp.sw ) {
			x = wp.sw - 360;
		}
	}
	ui_icon(SYSDEF_PROG_TECHO);
	if ( ui_open_on(&g_find, &g_ui, SYSDEF_BOX_TECHO, PN_FIND, x, y) < E_OK ) {
		say("検索パネルを開けません");
		return;
	}
	g_find_open = TRUE;
	note("find panel on");
}

static void find( BOOL from_first )
{
	UB	w[TC_LINE_MAX];
	INT	n, k = win_frontmost(), r;
	char	s[TC_LINE_MAX + 64];

	if ( k == W_CAL || k < 0 ) {
		/* the カレンダー in front: the one of the other two last worked in, when it is open */
		k = g_w[g_front].open ? g_front : -1;
	}
	if ( k < 0 ) {
		say("予定表か住所録のウインドウを表示してください");
		return;
	}
	n = ui_text(&g_find, P_FIND_TEXT, (char *)w, sizeof(w) - 1);
	if ( n < 0 ) n = 0;
	w[n] = 0;
	if ( n == 0 ) {
		say("検索する文字列を入力してください");
		return;
	}
	ed_commit();
	if ( k == W_SCHED ) {
		r = tc_find_plan(&g_db, from_first ? TC_DAY_FIRST - 1 : g_day, w);
		snprintf(s, sizeof(s), "find sched %s %s %d", from_first ? "first" : "next", (char *)w, r);
		note(s);
		if ( r > 0 ) show_day(r);
	} else {
		r = tc_find_addr(&g_db, from_first ? 0 : g_cur + 1, w);
		snprintf(s, sizeof(s), "find addr %s %s %d", from_first ? "first" : "next", (char *)w, r + 1);
		note(s);
		if ( r >= 0 ) show_person(r);
	}
	if ( r <= 0 && !( k == W_ADDR && r == 0 ) ) {
		say("検索文字列は見つかりません");
	} else {
		say("");
	}
}

static void find_event( CONST T_OBNTM *m );

/* A notice of the channel: the find window's, or one of the pages' */
static void told( CONST T_OBNTM *m )
{
	if ( g_find_open && ui_mine(&g_find, m) ) find_event(m);
	else event(m);
}

static void find_event( CONST T_OBNTM *m )
{
	INT	num = 0;

	switch ( ui_event(&g_find, m, &num) ) {
	case UI_EV_BUTTON:
		if ( num == P_FIND_FIRST ) find(TRUE);
		else if ( num == P_FIND_NEXT ) find(FALSE);
		break;
	case UI_EV_CLOSE:
		ui_close(&g_find);
		g_find_open = FALSE;
		note("find panel off");
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- 印刷 */

static void paper_setup( void )
{
	UI	q;
	INT	mm[4], v, kind, k;
	BOOL	turn;
	char	s[64];

	ed_commit();
	ask_over(g_last);
	if ( ui_dlg_open(&g_ui, PN_PAPER, &q) < E_OK ) return;
	kind = tc_paper_kind(&g_db, &turn, mm);
	ui_set_value(&q, 11, ( kind < TC_PAPER_N ) ? kind + 1 : 1);
	ui_set_value(&q, 12, turn ? 2 : 1);
	for ( k = 0; k < 4; k++ ) ui_set_fields(&q, 13 + k, &mm[k], 1);
	note("paper panel");
	if ( work(&q) == B_OK ) {
		kind = ui_value(&q, 11) - 1;
		turn = (BOOL)( ui_value(&q, 12) == 2 );
		for ( k = 0; k < 4; k++ ) {
			v = mm[k];
			if ( ui_fields(&q, 13 + k, &v, 1) >= 0 && v >= 0 && v <= 99 ) mm[k] = v;
		}
		tc_set_paper(&g_db, kind, turn, mm);
		g_changed = TRUE;
		snprintf(s, sizeof(s), "paper %d %s %d %d %d %d", kind, turn ? "turned" : "upright", mm[0], mm[1], mm[2], mm[3]);
		note(s);
	}
	ui_dlg_close(&q);
	redraw_dirty();
}

/*
 * 年月を選択 (the ページ menu of the カレンダー): a year and a month asked,
 * the one shown to start from; 移動 or Enter shows it
 */
static void ask_month( void )
{
	UI	q;
	T_OBNTM	m;
	INT	v[2], num, ev, done = 0;

	ed_commit();
	ask_over(W_CAL);
	if ( ui_dlg_open(&g_ui, PN_MONTH, &q) < E_OK ) return;
	v[0] = g_cy;
	v[1] = g_cm;
	ui_set_fields(&q, P_YM, v, 2);
	note("month panel");
	while ( done == 0 ) {
		if ( ui_wait(&g_ui, &m, TMO_FEVR) < E_OK ) break;
		ev = ui_dlg_take(&q, &m, &num);
		if ( ev == UI_EV_BUTTON ) {
			done = ( num == P_YM_OK ) ? 1 : -1;
		} else if ( ev == UI_EV_CLOSE ) {
			done = -1;
		} else if ( ev == UI_EV_NONE && m.id == 3 && m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0
			 && ( m.code == K_ENTER || m.code == K_KPENTER ) ) {
			done = 1;
		} else if ( ev == UI_EV_OTHER && m.event == OB_E_REDRAW ) {
			event(&m);
		}
		if ( g_closing ) done = -1;
	}
	if ( done > 0 ) (void)ui_fields(&q, P_YM, v, 2);
	ui_dlg_close(&q);
	if ( done > 0 && v[1] >= 1 && v[1] <= 12 ) {
		show_month(v[0], v[1]);
		g_w[W_CAL].dirty = TRUE;
	}
	redraw_dirty();
}

static void print( void )
{
	T_UIPRN	up;
	CTBUF	b;
	INT	k = win_frontmost(), a;
	ER	er;
	char	s[96];

	ed_commit();
	if ( k == W_CAL || k < 0 ) k = g_w[g_front].open ? g_front : -1;
	if ( k < 0 ) {
		say("カレンダーは印刷できません");
		return;
	}
	ask_over(k);
	note("print panel");
	a = ui_ask(&g_ui, PN_PRINT, NULL, 0);
	redraw_dirty();
	if ( a != B_OK && a != B_OTHER ) return;
	ct_buf(&b, g_out, OUT_MAX);
	if ( k == W_SCHED ) er = tc_print_plans(&g_db, &b, ( a == B_OTHER ) ? g_day : 0);
	else er = tc_print_addrs(&g_db, &b, ( a == B_OTHER ) ? g_cur : -1);
	snprintf(s, sizeof(s), "print %s %s (%d)", ( k == W_SCHED ) ? "sched" : "addr", ( a == B_OTHER ) ? "one" : "all", (INT)er);
	note(s);
	if ( er == E_NOEXS ) {
		say("印刷する予定がありません");
		return;
	}
	if ( er < E_OK ) {
		say("印刷するページを作れません");
		return;
	}
	if ( ui_prn_open(&up, &g_obj) < E_OK ) {
		say("印刷できません");
		return;
	}
	er = ui_prn_page(&up, b.p, b.n);
	if ( er >= E_OK ) er = ui_prn_ready(&up);
	if ( er >= E_OK ) {
		T_DTREQ	rq;
		T_DTANS	an;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		snprintf((char *)rq.name, sizeof(rq.name), "%s（%s）", (char *)g_name, win_title[k]);
		/* the person works the desktop's panels: as long as that takes */
		er = ask_desktop(&rq, &an, 60 * 60 * 1000);
		snprintf(s, sizeof(s), "print answered %d", (INT)er);
		note(s);
	}
	ui_prn_close(&up);
}

/* ---------------------------------------------------------------- the tray */

static void tray_menu( ID mid )
{
	static UB	rows[TR_LEVELS][WM_LABEL_MAX];
	TS_UUID		td;
	T_TRSTS		*s = malloc(sizeof(T_TRSTS));
	INT		i;

	if ( s == NULL ) return;
	if ( ui_uuid(SYSDEF_MENU_TRAY, &td) >= E_OK && tr_get_sts(s) >= E_OK ) {
		for ( i = 1; i <= s->nlevel && i <= TR_LEVELS; i++ ) {
			snprintf((char *)rows[i - 1], WM_LABEL_MAX, "%d %s", i, (char *)s->set[i].name);
		}
		(void)mn_set_lst(mid, &td, "tray.level", (CONST UB *)rows, WM_LABEL_MAX, s->nlevel);
		if ( s->hand > 0 ) (void)mn_chg_idx(mid, &td, "tray.level", s->hand - 1, MN_TICK);
		(void)mn_chg_atr(mid, &td, "tray.clear", ( s->nlevel == 0 ) ? MN_GREY : 0);
	}
	free(s);
}

static BOOL tray_has( void )
{
	T_TRSTS	*s = malloc(sizeof(T_TRSTS));
	BOOL	yes = FALSE;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS ) {
		yes = (BOOL)( ( s->set[s->hand].kinds & ( 1U << TR_TAD ) ) != 0 );
	}
	free(s);
	return yes;
}

/* The letters taken (or the whole field) to the tray as a text; with cut, taken out */
static void tray_copy( BOOL cut )
{
	UB	t[TC_LINE_MAX], name[TR_NAME_MAX];
	CTBUF	b;
	T_TRREC	r;
	INT	n;
	char	s[TC_LINE_MAX + 32];

	if ( !ed_on() ) {
		say("記入欄を選んでください");
		return;
	}
	if ( !ed_has_sel() ) ed_select_all();
	n = ed_sel_text(t, sizeof(t));
	if ( n == 0 ) {
		say("トレーへ入れる文字がありません");
		return;
	}
	ct_buf(&b, g_out, OUT_MAX);
	ct_put(&b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<p>");
	ct_esc(&b, t, n);
	ct_put(&b, "</p>\n</document>\n</tad>\n");
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = b.p;
	r.size = b.n;
	snprintf((char *)name, sizeof(name), "%s", (char *)t);
	if ( b.fail || tr_psh_dat(&r, 1, name) < E_OK ) {
		say("トレーに入れられませんでした");
		return;
	}
	if ( cut ) ed_sel_delete();
	snprintf(s, sizeof(s), "%s %s", cut ? "cut" : "copied", (char *)t);
	note(s);
	say(cut ? "トレーへ移動しました" : "トレーへ複写しました");
}

static void tray_paste( BOOL take )
{
	T_TRINF	inf;
	UB	*x = NULL, t[TC_LINE_MAX];
	SZ	asz = 0;
	INT	i, n;
	char	s[TC_LINE_MAX + 32];

	if ( !ed_on() ) {
		say("記入欄を選んでください");
		return;
	}
	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( inf.kind == TR_TAD && inf.size > 0 && inf.size < OUT_MAX ) {
			x = malloc((size_t)inf.size + 1);
			if ( x != NULL && ( tr_rea_rec(TR_HAND, i, x, inf.size, &asz) < E_OK || asz != inf.size ) ) {
				free(x);
				x = NULL;
			}
			break;
		}
	}
	if ( x == NULL ) {
		say("トレーに文字がありません");
		return;
	}
	n = tc_doc_text(x, (INT)inf.size, t, sizeof(t));
	free(x);
	if ( n == 0 ) {
		say("記入欄には文字だけを入れられます");
		return;
	}
	ed_insert(t, n);
	if ( take ) (void)tr_del_dat(TR_HAND);
	snprintf(s, sizeof(s), "pasted %s", (char *)t);
	note(s);
	say(take ? "トレーから移動しました" : "トレーから複写しました");
}

/* ---------------------------------------------------------------- carrying letters */

/* The letters taken put into the field under (x, y) of window w: moved, or copied */
static void carry_drop( INT w, INT x, INT y, BOOL copy )
{
	UB		t[TC_LINE_MAX], v[TC_LINE_MAX * 2];
	INT		f = part_at(w, x, y), n, at, sw = ed_win(), sf = ed_field();
	T_DPRECT	r;
	char		s[TC_LINE_MAX + 48];

	if ( !ed_has_sel() || f < 0 || w != sw ) return;
	n = ed_sel_text(t, sizeof(t));
	if ( f == sf ) {
		INT	lo = ed_sel_from();

		at = ed_offset_at(x);
		if ( at >= lo && at <= lo + n ) return;		/* over itself: nothing moves */
		if ( !copy ) {
			ed_sel_delete();
			if ( at > lo ) at -= n;
		}
		ed_set_caret(at);
		ed_insert(t, n);
	} else {
		CONST UB	*was;
		INT		len;

		if ( !copy ) ed_sel_delete();
		ed_commit();
		was = field_text(w, f);
		len = (INT)strlen((CONST char *)was);
		(void)field_box(w, f, &r);
		at = text_offset(was, x - r.left - 4, FIELD_PX);
		if ( len + n >= TC_LINE_MAX ) n = TC_LINE_MAX - 1 - len;
		if ( n <= 0 ) return;
		memcpy(v, was, (size_t)at);
		memcpy(v + at, t, (size_t)n);
		memcpy(v + at + n, was + at, (size_t)( len - at ));
		v[len + n] = 0;
		ed_begin(w, f, v);
		ed_set_caret(at + n);
	}
	snprintf(s, sizeof(s), "%s %s to %d", copy ? "dragged copy" : "dragged", (char *)t, f + 1);
	note(s);
	g_w[w].dirty = TRUE;
}

/* ---------------------------------------------------------------- closing */

static void close_asked( void )
{
	INT	a;

	ed_commit();
	if ( !g_changed ) {
		g_closing = TRUE;
		return;
	}
	ask_over(g_last);
	a = ui_ask(&g_ui, PN_CLOSE, NULL, 0);
	redraw_dirty();
	if ( a == B_OK ) {
		if ( save() >= E_OK ) g_closing = TRUE;
		else warn("保存できませんでした。", NULL);
	} else if ( a == B_OTHER ) {
		note("not saved");
		g_closing = TRUE;
	}
}

/* ---------------------------------------------------------------- commands */

static INT g_menu_w = W_SCHED;		/* the window the menu was asked on */

static void page( CONST char *how, INT w )
{
	BOOL	first = (BOOL)( strcmp(how, "first") == 0 ), last = (BOOL)( strcmp(how, "last") == 0 );
	INT	d = ( strcmp(how, "prev") == 0 ) ? -1 : 1;

	if ( w == W_SCHED ) {
		show_day(first ? TC_DAY_FIRST : last ? TC_DAY_LAST : tc_day_add(g_day, d));
	} else if ( w == W_CAL ) {
		INT	y = g_cy, m = g_cm + d;

		if ( first ) {
			y = TC_Y_FIRST;
			m = 1;
		} else if ( last ) {
			y = TC_Y_LAST;
			m = 12;
		} else if ( m < 1 ) {
			m = 12;
			y--;
		} else if ( m > 12 ) {
			m = 1;
			y++;
		}
		show_month(y, m);
	} else if ( w == W_ADDR ) {
		show_person(first ? 0 : last ? g_db.na - 1 : g_cur + d);
	}
}

void command( CONST char *code, INT index )
{
	INT	w = g_menu_w;

	if ( strcmp(code, "close") == 0 ) close_asked();
	else if ( strcmp(code, "save") == 0 ) {
		if ( save() >= E_OK ) {
			places_keep();
			say("保存しました");
		} else {
			warn("保存できませんでした。", NULL);
		}
	} else if ( strcmp(code, "view.sched") == 0 ) view(W_SCHED);
	else if ( strcmp(code, "view.cal") == 0 ) view(W_CAL);
	else if ( strcmp(code, "view.addr") == 0 ) view(W_ADDR);
	else if ( strcmp(code, "view.find") == 0 ) find_open();
	else if ( strncmp(code, "page.", 5) == 0 && ( strcmp(code + 5, "first") == 0 || strcmp(code + 5, "prev") == 0
						 || strcmp(code + 5, "next") == 0 || strcmp(code + 5, "last") == 0 ) ) {
		page(code + 5, w);
	} else if ( strcmp(code, "page.month") == 0 && w == W_CAL ) {
		ask_month();
	} else if ( strcmp(code, "page.blank") == 0 && w == W_SCHED ) {
		ed_cancel();
		(void)tc_set_plans(&g_db, g_day, NULL, 0);
		rows_load();
		g_changed = TRUE;
		note("blanked");
		say("記入欄を空にしました");
	} else if ( strcmp(code, "page.add") == 0 && w == W_ADDR ) {
		INT	i;

		ed_commit();
		i = tc_addr_add(&g_db);
		if ( i < 0 ) {
			say("ページを追加できません");
		} else {
			g_changed = TRUE;
			show_person(i);
			note("page added");
		}
	} else if ( strcmp(code, "page.del") == 0 && w == W_ADDR ) {
		char	s[48];

		ed_cancel();
		tc_addr_del(&g_db, g_cur);
		g_changed = TRUE;
		snprintf(s, sizeof(s), "page deleted, %d left", g_db.na);
		note(s);
		show_person(g_cur);
		say("ページを削除しました");
	} else if ( strcmp(code, "undo") == 0 ) {
		if ( ed_on() ) {
			ed_cancel();
			note("typing undone");
		} else {
			say("取り消せる操作がありません");
		}
	} else if ( strcmp(code, "copy") == 0 ) tray_copy(FALSE);
	else if ( strcmp(code, "cut") == 0 ) tray_copy(TRUE);
	else if ( strcmp(code, "paste") == 0 ) tray_paste(FALSE);
	else if ( strcmp(code, "moveback") == 0 ) tray_paste(TRUE);
	else if ( strcmp(code, "delete") == 0 ) {
		if ( ed_has_sel() ) ed_sel_delete();
	} else if ( strcmp(code, "tray.level") == 0 ) (void)tr_sel_dat(index + 1);
	else if ( strcmp(code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp(code, "print.setup") == 0 ) paper_setup();
	else if ( strcmp(code, "print") == 0 ) print();
	for ( w = 0; w < W_N; w++ ) g_w[w].dirty = TRUE;
}

/* ---------------------------------------------------------------- the menu */

static void menu( CONST T_OBNTM *m, INT w )
{
	TS_UUID	def;
	T_MNSEL	s;
	ID	mid;
	BOOL	chosen, edit = (BOOL)( ed_on() && ed_win() == w );
	UINT	g;

	g_menu_w = w;
	if ( ui_uuid(SYSDEF_MENU_TECHO, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "save.new", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "view.sched", g_w[W_SCHED].open ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "view.cal", g_w[W_CAL].open ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "view.addr", g_w[W_ADDR].open ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "view.find", g_find_open ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "page.blank", ( w == W_SCHED ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "page.month", ( w == W_CAL ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "page.add", ( w == W_ADDR ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "page.del", ( w == W_ADDR ) ? 0 : MN_GREY);
	g = edit ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "copy", g);
	(void)mn_chg_atr(mid, NULL, "cut", g);
	(void)mn_chg_atr(mid, NULL, "undo", g);
	(void)mn_chg_atr(mid, NULL, "delete", ( edit && ed_has_sel() ) ? 0 : MN_GREY);
	g = ( edit && tray_has() ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "paste", g);
	(void)mn_chg_atr(mid, NULL, "moveback", g);
	tray_menu(mid);
	(void)mn_chg_atr(mid, NULL, "print", ( w == W_CAL ) ? MN_GREY : 0);
	chosen = (BOOL)( mn_pop_men(mid, g_w[w].kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		char	line[80];

		snprintf(line, sizeof(line), "menu %s", (char *)s.code);
		note(line);
		command((CONST char *)s.code, s.index);
	}
}

/* ---------------------------------------------------------------- keys */

static void key( CONST T_OBNTM *m, INT w )
{
	UB	c = wm_key_char(m->code, 0);
	UINT	mods = m->mods;

	if ( ( mods & OB_KEY_UP ) != 0 ) return;
	g_menu_w = w;
	if ( ( mods & M_CTRL ) != 0 && !ed_busy() ) {
		switch ( c ) {
		case 'e': case 'E':	command("close", 0);	break;
		case 's': case 'S':	command("save", 0);	break;
		case 'c': case 'C':	command("copy", 0);	break;
		case 'x': case 'X':	command("cut", 0);	break;
		case 'v': case 'V':	command("paste", 0);	break;
		case 'z': case 'Z':	command("undo", 0);	break;
		case 'p': case 'P':	command("print", 0);	break;
		case 'f': case 'F':	command("view.find", 0);	break;
		default:		break;
		}
		return;
	}
	if ( ed_on() && ed_win() == w && !ed_busy() ) {
		if ( m->code == K_TAB ) {
			field_step(w, ( mods & M_SHIFT ) ? -1 : 1);
			return;
		}
		if ( m->code == K_ENTER || m->code == K_KPENTER ) {
			field_step(w, 1);
			return;
		}
	}
	if ( ed_on() && ed_win() == w && ed_key(m->code, mods, m->imode) ) return;
	switch ( m->code ) {
	case K_PGUP:	page("prev", w);	break;
	case K_PGDN:	page("next", w);	break;
	case K_HOME:
		if ( w == W_CAL ) {
			INT	y, mm, d;

			tc_day_split(g_today, &y, &mm, &d);
			show_month(y, mm);
		} else if ( w == W_SCHED ) {
			show_day(g_today);
		}
		break;
	case K_TAB:
		field_step(w, ( mods & M_SHIFT ) ? -1 : 1);
		break;
	case K_ESC:
		g_idx_open = FALSE;
		g_w[w].dirty = TRUE;
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the pointer */

static void index_choose( INT k )
{
	INT	i = tc_index_first(&g_db, k);
	char	s[64];

	g_idx_open = FALSE;
	g_w[W_ADDR].dirty = TRUE;
	if ( i < 0 ) {
		snprintf(s, sizeof(s), "〈%s〉のページはありません", tc_index_name(k));
		say(s);
		return;
	}
	show_person(i);
}

static void press( CONST T_OBNTM *m, INT w )
{
	INT	p;

	g_hold = HOLD_NONE;
	g_hold_w = w;
	g_hx0 = m->x;
	g_hy0 = m->y;
	g_hold_moved = FALSE;
	g_hold_copy = FALSE;
	if ( w == W_ADDR && g_idx_open ) {
		INT	k = index_at(m->x, m->y);

		if ( k >= 0 ) {
			index_choose(k);
			return;
		}
		g_idx_open = FALSE;
		g_w[W_ADDR].dirty = TRUE;
	}
	p = part_at(w, m->x, m->y);
	if ( p >= 0 ) {
		if ( ed_on() && ed_win() == w && ed_field() == p && ed_in_sel(m->x) ) {
			g_hold = HOLD_CARRY;		/* carried, unless let go where it went down */
			return;
		}
		field_begin(w, p, m->x);
		g_hold = HOLD_SELECT;
		g_w[w].dirty = TRUE;
		return;
	}
	if ( ed_win() == w ) ed_commit();
	switch ( p ) {
	case PT_PREV:	page("prev", w);	break;
	case PT_NEXT:	page("next", w);	break;
	case PT_MONTH: {
		INT	y, mm, d;

		/* the year and month pressed: this month again */
		tc_day_split(g_today, &y, &mm, &d);
		show_month(y, mm);
		break;
	}
	case PT_INDEX:
		g_idx_open = TRUE;
		g_hold = HOLD_INDEX;
		g_w[W_ADDR].dirty = TRUE;
		break;
	default:
		if ( p <= PT_DAY - 1 && w == W_CAL ) {
			INT	d = PT_DAY - p;

			if ( !g_w[W_SCHED].open && win_open(W_SCHED) < E_OK ) break;
			show_day(tc_day(g_cy, g_cm, d));
			win_front(W_SCHED);
		}
		break;
	}
	g_w[w].dirty = TRUE;
}

static void moved( CONST T_OBNTM *m, INT w )
{
	if ( w != g_hold_w ) return;
	if ( m->x - g_hx0 >= MOVE_MIN || g_hx0 - m->x >= MOVE_MIN || m->y - g_hy0 >= MOVE_MIN || g_hy0 - m->y >= MOVE_MIN ) {
		g_hold_moved = TRUE;
	}
	switch ( g_hold ) {
	case HOLD_SELECT:
		if ( g_hold_moved ) ed_caret_at(m->x, TRUE);
		break;
	case HOLD_CARRY:
		if ( g_hold_moved ) {
			INT		f = part_at(w, m->x, m->y);
			T_DPRECT	r;

			g_drop_w = ( f >= 0 ) ? w : -1;
			g_drop_f = f;
			g_drop_x = m->x;
			if ( f >= 0 && field_box(w, f, &r) && g_drop_x < r.left + 4 ) g_drop_x = r.left + 4;
			g_w[w].dirty = TRUE;
		}
		break;
	default:
		break;
	}
}

static void released( CONST T_OBNTM *m, INT w )
{
	INT	hold = g_hold;

	g_hold = HOLD_NONE;
	g_drop_w = g_drop_f = -1;
	if ( w != g_hold_w ) return;
	if ( hold == HOLD_CARRY ) {
		if ( g_hold_moved ) carry_drop(w, m->x, m->y, g_hold_copy);
		else ed_caret_at(m->x, FALSE);
	} else if ( hold == HOLD_INDEX && g_hold_moved ) {
		INT	k = index_at(m->x, m->y);

		if ( k >= 0 ) index_choose(k);
	}
	g_w[w].dirty = TRUE;
}

/*
 * Something dropped on a window. Letters carried from another window go
 * into the 記入欄 where the corner of the outline stood, at the place
 * there, and the field is typed into from them; links are refused, as a
 * 記入欄 takes letters only.
 */
static void dropped( INT w )
{
	UI		u;
	T_OBDROP	*d;
	INT		f, n;
	char		s[160];

	memset(&u, 0, sizeof(u));
	u.kw = g_w[w].kw;
	d = ob_drop_read(g_w[w].kw);		/* all the letters let go */
	if ( d == NULL ) return;
	if ( d->n > 0 ) {
		ui_drop_answer(&u, d, OB_DR_REFUSE, "記入欄には仮身を入れられません");
		note("drop refused");
		free(d);
		return;
	}
	n = (INT)strlen((char *)OB_DROP_LETTERS(d));
	f = part_at(w, d->x, d->y);
	if ( n == 0 || f < 0 ) {
		ui_drop_answer(&u, d, OB_DR_REFUSE, "記入欄の上に置いてください");
		note("drop refused");
		free(d);
		return;
	}
	ui_drop_answer(&u, d, OB_DR_ACCEPT, "");
	field_begin(w, f, d->x);
	ed_insert(OB_DROP_LETTERS(d), n);
	g_last = w;
	if ( w != W_CAL ) g_front = w;
	snprintf(s, sizeof(s), "dropped text %s to %s %d", (char *)OB_DROP_LETTERS(d),
		 ( w == W_SCHED ) ? "sched" : "addr", f + 1);
	note(s);
	g_w[w].dirty = TRUE;
	free(d);
}

/*
 * The letters taken carried out of the window: the desktop follows the
 * hand with an outline and answers where it was let go. Into a text of
 * the desktop's, or another window that takes letters -- another of the
 * 手帳's among them -- they go there and leave the field they came from
 * (a move), unless the second button went down on the way (a copy).
 * Let go back over this window or anywhere else, nothing happens.
 */
static void carry_out( INT w )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UB	t[TC_LINE_MAX];
	INT	n, lo, sw = ed_win(), sf = ed_field(), tw;
	ER	er;
	char	s[TC_LINE_MAX + 64];

	g_hold = HOLD_NONE;
	g_drop_w = g_drop_f = -1;
	g_w[w].dirty = TRUE;
	if ( !ed_has_sel() || sw != w ) return;
	n = ed_sel_text(t, sizeof(t));
	lo = ed_sel_from();
	if ( n <= 0 || n >= DT_CARRY_TEXT ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.flags = DT_F_TEXT;
	memcpy(rq.text, t, (size_t)n);
	tw = dp_text_width(t, FIELD_PX) + 8;
	rq.look.w = ( tw < 24 ) ? 24 : ( tw > 320 ) ? 320 : tw;
	rq.look.h = ROW_H;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	rq.grab_x = 4;
	rq.grab_y = ROW_H / 2;
	g_last = w;
	memset(&an, 0, sizeof(an));
	draw(w);
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	snprintf(s, sizeof(s), "carried text %s (%d)%s", (char *)t, (INT)er,
		 ( er >= E_OK && an.how == DT_HOW_COPY ) ? " as a copy" : "");
	note(s);
	if ( er < E_OK || an.how == DT_HOW_COPY ) return;
	/* a move: the letters leave the field they came from */
	if ( ed_on() && ed_win() == sw && ed_field() == sf && ed_has_sel() ) {
		ed_sel_delete();
	} else {
		CONST UB	*was = field_text(sw, sf);
		INT		len = (INT)strlen((CONST char *)was);
		UB		v[TC_LINE_MAX];

		if ( lo + n <= len && memcmp(was + lo, t, (size_t)n) == 0 ) {
			memcpy(v, was, (size_t)lo);
			memcpy(v + lo, was + lo + n, (size_t)( len - lo - n ));
			field_commit(sw, sf, v, len - n);
		}
	}
	g_w[sw].dirty = TRUE;
}

/* ---------------------------------------------------------------- what happens */

static INT win_of( CONST T_OBNTM *m )
{
	INT	k;

	for ( k = 0; k < W_N; k++ ) {
		if ( g_w[k].open && memcmp(&m->uuid, &g_w[k].win, sizeof(TS_UUID)) == 0 ) return k;
	}
	return -1;
}

static void event( CONST T_OBNTM *m )
{
	INT	w = win_of(m);

	if ( w < 0 ) return;
	if ( ( m->event & ( OB_E_PRESS | OB_E_KEY ) ) != 0 ) {
		g_last = w;
		if ( w != W_CAL ) g_front = w;
	}
	switch ( m->event ) {
	case OB_E_CLOSE:
		/* the pictogram: the window put away, the program closed with the last */
		if ( open_count() <= 1 ) close_asked();
		else win_close(w);
		return;
	case OB_E_DELETE:
		g_w[w].open = FALSE;
		if ( open_count() == 0 ) g_closing = TRUE;
		return;
	case OB_E_REDRAW:
		work_size(&g_w[w]);
		g_w[w].dirty = TRUE;
		return;
	case OB_E_PRESS:
		if ( m->recno == OB_WR_CAND ) {
			if ( ed_on() && ed_win() == w ) ed_pick((INT)m->code);	/* a name on the list of candidates */
			return;
		}
		if ( m->code == 1 ) {
			/* the second button while letters are carried makes it a copy; else the menu */
			if ( g_hold == HOLD_CARRY ) g_hold_copy = TRUE;
			else if ( g_hold == HOLD_NONE ) menu(m, w);
			return;
		}
		if ( m->code == 0 ) press(m, w);
		return;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m, w);
		return;
	case OB_E_MOVE:
		moved(m, w);
		return;
	case OB_E_LEAVE:
		if ( g_hold == HOLD_CARRY && g_hold_moved && w == g_hold_w ) {
			carry_out(w);		/* the letters carried out of the window */
		} else if ( g_hold == HOLD_CARRY ) {
			g_hold = HOLD_NONE;
			g_drop_w = g_drop_f = -1;
			g_w[w].dirty = TRUE;
		}
		return;
	case OB_E_KEY:
		key(m, w);
		return;
	case OB_E_DROP:
		dropped(w);
		return;
	default:
		return;
	}
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBCRE	c;
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0;
	char	s[160], arg[32];
	ER	er;
	INT	k, y, mo, d;

	memset(arg, 0, sizeof(arg));
	if ( ts_get_arg(arg, sizeof(arg) - 1) < (SZ)sizeof(g_obj) || ob_ref_obj((CONST TS_UUID *)arg, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("電子手帳は、手帳用紙の実身を開いて使います");
		return 1;
	}
	memcpy(&g_obj, arg, sizeof(g_obj));
	memcpy(g_name, ref.name, OB_NAME_MAX);
	g_name[OB_NAME_MAX - 1] = 0;
	tc_init(&g_db);
	for ( k = 0; k < W_N; k++ ) g_w[k].gid = -1;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port <= 0 ) {
		note("no channel");
		return 2;
	}
	er = load();
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "the object cannot be read (%d)", (INT)er);
		note(s);
		(void)wm_msg_put("電子手帳の実身を読めません");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	today();
	tc_day_split(g_today, &y, &mo, &d);
	g_day = g_today;
	g_cy = y;
	g_cm = mo;
	g_cur = 0;
	rows_load();
	places();
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_TECHO, &g_ui.box);
	/* those left shown: the 住所録 first, then the カレンダー, the 予定表 in front */
	if ( ( g_want[W_ADDR] && win_open(W_ADDR) < E_OK ) || ( g_want[W_CAL] && win_open(W_CAL) < E_OK )
	  || ( g_want[W_SCHED] && win_open(W_SCHED) < E_OK ) ) {
		note("no window");
		for ( k = 0; k < W_N; k++ ) win_close(k);
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	snprintf(s, sizeof(s), "ready %s today %d", (char *)g_name, g_today);
	note(s);
	say("記入欄を押して書き込みます");
	if ( !g_w[g_last].open ) g_last = win_frontmost();
	if ( !g_w[g_front].open ) g_front = g_w[W_ADDR].open ? W_ADDR : W_SCHED;
	for ( k = 0; k < W_N; k++ ) draw(k);

	/* the find window's notices come to the same channel (ui_open_on): one wait has both */
	while ( !g_closing ) {
		for ( TMO tmo = TMO_FEVR; !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK;
		      tmo = TMO_POL ) {
			told(&m);
		}
		if ( !g_closing ) redraw_dirty();
	}
	ed_end();
	if ( g_find_open ) ui_close(&g_find);
	places_keep();
	for ( k = 0; k < W_N; k++ ) win_close(k);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	tc_clear(&g_db);
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}
