/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_main.c
 *	マイクロカード: a database of cards (design 11.18)
 *
 *	The program is started on a マイクロカード実身 and shows its cards in
 *	two windows: the card window, one card at a time (mc_card.c), and
 *	the list window, the cards a row each (mc_list.c), which the 一覧
 *	switch and the 表示 menu open and close. The two follow each other:
 *	the card shown is the list's row turned over.
 *
 *	The cards worked on are all of them, or in the search state those
 *	found: 新規検索 and 絞込検索 of 検索条件画面, 重複検索, and マーク付
 *	カード新規検索 and 追加検索 put the database in it, 検索解除 takes it
 *	out. Cards are added after the one shown, duplicated, made empty and
 *	deleted; marked one by one or together; sorted, written out to a new
 *	object and merged into a layout (差し込み) -- each on the cards
 *	worked on. A new database, with no fields, opens on 設計画面.
 *
 *	The menu and the keys do the rest; what was done is said on the
 *	message line and, for the tests, on the console ("mcard: ...").
 *	Nothing is written to the object until it is saved; closing a
 *	database changed asks first.
 */

#include "mc.h"
#include <ts/json.h>

/* ---------------------------------------------------------------- state */

MCDB	g_db;
TS_UUID	g_obj;
UB	g_name[OB_NAME_MAX];
BOOL	g_autoref;
BOOL	g_changed, g_closing;
ID	g_port;
UI	g_ui;
MCWIN	g_cw, g_lw;
INT	g_screen = SCR_CARD;
INT	g_cur;
INT	g_field = -1;
BOOL	g_link_on;
UB	*g_cond[MC_FIELD_MAX];
UB	g_out[OUT_MAX];
char	g_meta_none[1];
char	*g_meta = g_meta_none;
MCFIELD	g_dsf[MC_FIELD_MAX + 1];
INT	g_dsfrom[MC_FIELD_MAX + 1];
INT	g_dsn;
UB	*g_dslayout;
INT	g_dslayoutlen;

static TS_UUID	g_ch;
static BOOL	g_in_list;		/* the last press or key was the list window's */
static INT	g_cx = 100, g_cy = 80;	/* where the card window stood when it opened */

extern BOOL card_undo( void );
extern void card_undo_forget( void );

/* ---------------------------------------------------------------- saying things */

void note( CONST char *s )
{
	char	line[300];

	snprintf(line, sizeof(line), "mcard: %s\n", s);
	tm_putstring((CONST UB *)line);
}

void say( CONST char *s )
{
	(void)wm_msg_put(s);
}

void warn( CONST char *a, CONST char *b )
{
	CONST char	*l[3] = { a, ( b != NULL ) ? b : " ", " " };

	(void)ui_ask(&g_ui, 10, l, 3);
	redraw_all();
}

INT card_px( void )
{
	return ( g_db.view.csize * 4 + 1 ) / 3;
}

INT list_px( void )
{
	return ( g_db.view.lsize * 4 + 1 ) / 3;
}

/* ---------------------------------------------------------------- the card shown */

INT cur_index( void )
{
	return mc_index(&g_db, g_cur);
}

MCCARD *cur_card( void )
{
	INT	i = cur_index();

	return ( i >= 0 ) ? g_db.c[i] : NULL;
}

void show_card( INT pos )
{
	INT	n = mc_count(&g_db);
	char	s[48];

	if ( ed_on() && ed_kind() != ED_COND && g_screen != SCR_DESIGN ) ed_commit();
	if ( pos >= n ) pos = n - 1;
	if ( pos < 0 ) pos = 0;
	if ( pos != g_cur ) {
		g_cur = pos;
		g_link_on = FALSE;
		snprintf(s, sizeof(s), "card %d of %d", pos + 1, n);
		note(s);
	}
	list_show_cur();
	g_cw.dirty = TRUE;
	g_lw.dirty = TRUE;
}

void redraw_all( void )
{
	if ( g_cur >= mc_count(&g_db) ) g_cur = mc_count(&g_db) - 1;
	if ( g_cur < 0 ) g_cur = 0;
	card_paint();
	if ( g_lw.open ) list_paint();
}

/* ---------------------------------------------------------------- the windows */

static void work_size( MCWIN *w )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		w->w = wp.wright - wp.wleft;
		w->h = wp.wbottom - wp.wtop;
	}
	if ( w == &g_cw ) {
		g_ui.w = w->w;
		g_ui.h = w->h;
	}
}

static ER win_make( MCWIN *w, CONST char *title, INT x, INT y, INT ww, INT wh, UINT events )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[280];
	char		shown[TS_UUID_STRLEN + 1];

	ts_uuid_text(&g_obj, shown);	/* what it shows, for ウインドウ一覧 */
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 x, y, x + ww + FRAME_W, y + wh + TITLE_H,
		 WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR | WM_ATTR_BBAR, SYSDEF_PROG_MCARD, shown);
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
	req.events = events;
	if ( w->kw <= 0 || w->gid < 0 || ob_ntf_evt(w->kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( w->kw > 0 ) ob_cls_obj(w->kw);
		(void)ob_del_obj(&w->win);
		w->kw = 0;
		w->gid = -1;
		return E_OBJ;
	}
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + ww + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + wh + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	w->open = TRUE;
	w->dirty = TRUE;
	work_size(w);
	return E_OK;
}

static void win_free( MCWIN *w )
{
	if ( !w->open ) return;
	if ( w == &g_cw ) card_bar_close();	/* the bar is on it */
	w->open = FALSE;
	if ( w->kw > 0 ) ob_cls_obj(w->kw);
	(void)ob_del_obj(&w->win);
	w->kw = 0;
	w->gid = -1;
}

static void resized( MCWIN *w )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	work_size(w);
	if ( ( w->w < MIN_W || w->h < MIN_H ) && ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		if ( w->w < MIN_W ) wp.right += MIN_W - w->w;
		if ( w->h < MIN_H ) wp.bottom += MIN_H - w->h;
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		work_size(w);
	}
	w->dirty = TRUE;
}

void win_front( MCWIN *w )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( !w->open ) return;
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.z = 0;
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
}

#define WIN_EVENTS	( OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW \
			| OB_E_DELETE | OB_E_DROP | OB_E_LEAVE | OB_E_WHEEL | OB_E_SCROLL )

static ER card_open( CONST char *title )
{
	T_JSON	root, w, pos;
	INT	ww = 560, wh = 440, mn = meta_of(&g_obj);
	ER	er;

	if ( mn > 0 && js_parse((CONST UB *)g_meta, mn, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK ) {
		ww = (INT)js_get_num(&w, "width", ww);
		wh = (INT)js_get_num(&w, "height", wh);
		if ( js_get(&w, "pos", &pos) >= E_OK ) {
			g_cx = (INT)js_get_num(&pos, "x", g_cx);
			g_cy = (INT)js_get_num(&pos, "y", g_cy);
		}
	}
	if ( ww < MIN_W ) ww = MIN_W;
	if ( wh < MIN_H ) wh = MIN_H;
	if ( g_cx < 0 ) g_cx = 0;
	if ( g_cy < 0 ) g_cy = 0;
	er = win_make(&g_cw, title, g_cx, g_cy, ww, wh, WIN_EVENTS);
	if ( er < E_OK ) return er;
	g_ui.kw = g_cw.kw;
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_MCARD, &g_ui.box);
	work_size(&g_cw);
	return E_OK;
}

void list_open( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		title[OB_NAME_MAX + 16];
	INT		x = g_cx + 40, y = g_cy + 60;

	if ( g_lw.open ) {
		win_front(&g_lw);
		return;
	}
	if ( ob_rea_rec(g_cw.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		x = wp.right + 8;
		y = wp.top + 20;
		if ( wp.sw > 0 && x + 400 > wp.sw ) x = wp.sw - 560;
		if ( x < 0 ) x = 0;
	}
	snprintf(title, sizeof(title), "%s（一覧）", (char *)g_name);
	if ( win_make(&g_lw, title, x, y, 520, 360, WIN_EVENTS) < E_OK ) {
		say("一覧ウィンドウを開けません");
		return;
	}
	list_show_cur();
	note("list on");
}

void list_close( void )
{
	if ( !g_lw.open ) return;
	win_free(&g_lw);
	g_cw.dirty = TRUE;
	note("list off");
}

/* ---------------------------------------------------------------- searching */

static void search( INT how )
{
	BOOL	*was = malloc(sizeof(BOOL) * (size_t)( g_db.n + 1 ));
	BOOL	searching = g_db.searching;
	INT	n, at = -1, i;
	char	s[96];

	ed_commit();
	if ( was == NULL ) return;
	for ( i = 0; i < g_db.n; i++ ) was[i] = g_db.c[i]->found;
	n = mc_search(&g_db, (CONST UB *CONST *)g_cond, how, &at);
	if ( n < 0 ) {
		snprintf(s, sizeof(s), "項目%d「%s」", at + 1, at >= 0 ? (char *)g_db.f[at].name : "");
		warn("検索条件が正しくありません。", s);
		for ( i = 0; i < g_db.n; i++ ) g_db.c[i]->found = was[i];
		g_db.searching = searching;
		free(was);
		return;
	}
	if ( n == 0 ) {
		/* nothing found: the state stays as it was */
		for ( i = 0; i < g_db.n; i++ ) g_db.c[i]->found = was[i];
		g_db.searching = searching;
		free(was);
		snprintf(s, sizeof(s), "search %s found 0", how == MC_S_NEW ? "new" : "narrow");
		note(s);
		say("条件に合うカードはありませんでした");
		redraw_all();
		return;
	}
	free(was);
	g_cur = 0;
	snprintf(s, sizeof(s), "search %s found %d of %d", how == MC_S_NEW ? "new" : "narrow", n, g_db.n);
	note(s);
	snprintf(s, sizeof(s), "%d 枚のカードを検索しました", n);
	say(s);
	card_screen(SCR_CARD);
	list_show_cur();
	redraw_all();
}

static void search_off( void )
{
	MCCARD	*keep = cur_card();
	INT	i;

	mc_search_off(&g_db);
	for ( i = 0; i < g_db.n && g_db.c[i] != keep; i++ ) ;
	g_cur = ( i < g_db.n ) ? i : 0;
	note("search off");
	if ( g_screen == SCR_FIND ) card_screen(SCR_CARD);
	list_show_cur();
	redraw_all();
}

/* ---------------------------------------------------------------- cards */

static void card_add( BOOL dup )
{
	INT	i = cur_index(), at = ( i >= 0 ) ? i + 1 : g_db.n;
	MCCARD	*c;
	char	s[48];

	ed_commit();
	if ( g_db.n >= MC_CARD_MAX ) {
		warn("これ以上カードを追加できません。", NULL);
		return;
	}
	c = dup ? mc_card_dup(&g_db, i, at) : mc_card_new(&g_db, at);
	if ( c == NULL ) {
		warn("カードを追加できません。", "メモリが足りません。");
		return;
	}
	g_changed = TRUE;
	g_cur = mc_pos(&g_db, at);
	g_link_on = FALSE;
	snprintf(s, sizeof(s), "%s %d of %d", dup ? "duplicated" : "new card", g_cur + 1, mc_count(&g_db));
	note(s);
	list_show_cur();
	redraw_all();
}

static void card_delete( void )
{
	INT	i = cur_index();
	char	s[80];

	ed_cancel();
	if ( i < 0 ) return;
	snprintf(s, sizeof(s), "%d 枚目のカードを削除します。", g_cur + 1);
	if ( !dlg_delete(s) ) return;
	card_undo_forget();
	mc_card_del(&g_db, i);
	g_changed = TRUE;
	if ( g_db.searching && mc_count(&g_db) == 0 ) mc_search_off(&g_db);
	if ( g_cur >= mc_count(&g_db) ) g_cur = mc_count(&g_db) - 1;
	snprintf(s, sizeof(s), "deleted, %d left", mc_count(&g_db));
	note(s);
	list_show_cur();
	redraw_all();
}

static void found_delete( void )
{
	INT	i, n = 0;
	char	s[80];

	ed_cancel();
	if ( !g_db.searching ) return;
	snprintf(s, sizeof(s), "検索されている %d 枚のカードを削除します。", mc_count(&g_db));
	if ( !dlg_delete(s) ) return;
	card_undo_forget();
	for ( i = g_db.n - 1; i >= 0; i-- ) {
		if ( g_db.c[i]->found ) {
			mc_card_del(&g_db, i);
			n++;
		}
	}
	mc_search_off(&g_db);
	g_cur = 0;
	g_changed = TRUE;
	snprintf(s, sizeof(s), "deleted found %d, %d left", n, g_db.n);
	note(s);
	list_show_cur();
	redraw_all();
}

static void marks( INT how, CONST char *what )
{
	INT	n = mc_marks(&g_db, how, g_cur);
	char	s[64];

	snprintf(s, sizeof(s), "%s marked %d", what, n);
	note(s);
	redraw_all();
}

static void mark_search( BOOL add )
{
	INT	n = mc_mark_search(&g_db, add);
	char	s[64];

	if ( n == 0 ) {
		mc_search_off(&g_db);
		say("マークの付いたカードがありません");
	}
	g_cur = 0;
	snprintf(s, sizeof(s), "mark search %s found %d", add ? "add" : "new", n);
	note(s);
	list_show_cur();
	redraw_all();
}

/* ---------------------------------------------------------------- closing */

static void close_asked( void )
{
	INT	a;

	if ( g_screen == SCR_DESIGN ) design_cancel();
	ed_commit();
	if ( !g_changed ) {
		g_closing = TRUE;
		return;
	}
	a = dlg_close();
	if ( a == DLG_SAVE ) {
		if ( save() >= E_OK ) {
			note("saved");
			g_closing = TRUE;
		} else {
			say("保存できませんでした");
		}
	} else if ( a == DLG_DROP ) {
		note("not saved");
		g_closing = TRUE;
	}
}

/* ---------------------------------------------------------------- commands */

void command( CONST char *code, INT index )
{
	if ( strcmp(code, "close") == 0 ) close_asked();
	else if ( strcmp(code, "save") == 0 ) {
		if ( save() >= E_OK ) {
			note("saved");
			say("保存しました");
		} else {
			say("保存できませんでした");
		}
	} else if ( strcmp(code, "save.new") == 0 ) dlg_export();
	else if ( strcmp(code, "list") == 0 ) {
		if ( g_lw.open ) list_close();
		else list_open();
	} else if ( strcmp(code, "view") == 0 ) dlg_view();
	else if ( strcmp(code, "status") == 0 ) dlg_status();
	else if ( strcmp(code, "scr.find") == 0 ) card_screen(g_screen == SCR_FIND ? SCR_CARD : SCR_FIND);
	else if ( strcmp(code, "scr.design") == 0 ) {
		if ( g_screen == SCR_DESIGN ) design_cancel();
		else card_screen(SCR_DESIGN);
	} else if ( strcmp(code, "scr.card") == 0 ) card_screen(SCR_CARD);
	else if ( strcmp(code, "search.new") == 0 ) search(MC_S_NEW);
	else if ( strcmp(code, "search.narrow") == 0 ) search(MC_S_NARROW);
	else if ( strcmp(code, "search.off") == 0 ) search_off();
	else if ( strcmp(code, "design.ok") == 0 ) (void)design_apply();
	else if ( strcmp(code, "design.cancel") == 0 ) design_cancel();
	else if ( strcmp(code, "undo") == 0 ) {
		if ( ed_on() ) ed_cancel();
		else if ( !card_undo() ) say("取り消せる操作がありません");
	} else if ( strcmp(code, "copy") == 0 || strcmp(code, "cut") == 0 ) {
		if ( g_in_list ) tray_copy_rows(g_cur, g_cur);
		else tray_copy_field((BOOL)( code[1] == 'u' ));
	} else if ( strcmp(code, "paste") == 0 || strcmp(code, "moveback") == 0 ) {
		if ( g_in_list ) tray_paste_rows((BOOL)( code[0] == 'm' ));
		else tray_paste_field((BOOL)( code[0] == 'm' ));
	} else if ( strcmp(code, "delete") == 0 ) {
		if ( g_in_list ) card_delete();
		else if ( g_field >= 0 && cur_card() != NULL ) {
			(void)mc_set(&g_db, cur_card(), g_field, NULL, 0);
			g_changed = TRUE;
			g_link_on = FALSE;
			redraw_all();
		}
	} else if ( strcmp(code, "clear.link") == 0 ) {
		if ( g_field >= 0 && cur_card() != NULL ) {
			(void)mc_set(&g_db, cur_card(), g_field, NULL, 0);
			g_changed = TRUE;
			g_link_on = FALSE;
			note("link cleared");
			redraw_all();
		}
	} else if ( strcmp(code, "sort") == 0 ) dlg_sort();
	else if ( strcmp(code, "dups") == 0 ) dlg_dups();
	else if ( strcmp(code, "found.tray") == 0 ) tray_copy_rows(-1, -1);
	else if ( strcmp(code, "tray.level") == 0 ) (void)tr_sel_dat(index + 1);
	else if ( strcmp(code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp(code, "card.new") == 0 ) card_add(FALSE);
	else if ( strcmp(code, "card.dup") == 0 ) card_add(TRUE);
	else if ( strcmp(code, "card.blank") == 0 && g_screen == SCR_FIND ) {
		/* on 検索条件画面: every condition taken away */
		INT	k;

		ed_cancel();
		for ( k = 0; k < MC_FIELD_MAX; k++ ) {
			free(g_cond[k]);
			g_cond[k] = NULL;
		}
		note("conditions blanked");
		redraw_all();
	} else if ( strcmp(code, "card.blank") == 0 ) {
		ed_cancel();
		mc_card_blank(&g_db, cur_index());
		card_undo_forget();
		g_changed = TRUE;
		note("blanked");
		redraw_all();
	} else if ( strcmp(code, "card.del") == 0 ) card_delete();
	else if ( strcmp(code, "card.delfound") == 0 ) found_delete();
	else if ( strcmp(code, "go.prev") == 0 ) show_card(g_cur - 1);
	else if ( strcmp(code, "go.next") == 0 ) show_card(g_cur + 1);
	else if ( strcmp(code, "go.first") == 0 ) show_card(0);
	else if ( strcmp(code, "go.last") == 0 ) show_card(mc_count(&g_db) - 1);
	else if ( strcmp(code, "go.pmark") == 0 || strcmp(code, "go.nmark") == 0 ) {
		INT	p = mc_mark_next(&g_db, g_cur, ( code[3] == 'p' ) ? -1 : 1);

		if ( p >= 0 ) show_card(p);
		else say("マークの付いたカードがありません");
	} else if ( strcmp(code, "mark.flip") == 0 ) {
		MCCARD	*c = cur_card();
		char	s[48];

		if ( c != NULL ) {
			c->mark = (BOOL)!c->mark;
			snprintf(s, sizeof(s), "mark %d %s", g_cur + 1, c->mark ? "on" : "off");
			note(s);
		}
		redraw_all();
	} else if ( strcmp(code, "mark.flipall") == 0 ) marks(MC_M_FLIP_ALL, "flip all");
	else if ( strcmp(code, "mark.all") == 0 ) marks(MC_M_ALL, "all");
	else if ( strcmp(code, "mark.none") == 0 ) marks(MC_M_NONE, "none");
	else if ( strcmp(code, "mark.bon") == 0 ) marks(MC_M_BEFORE_ON, "before on");
	else if ( strcmp(code, "mark.boff") == 0 ) marks(MC_M_BEFORE_OFF, "before off");
	else if ( strcmp(code, "mark.aon") == 0 ) marks(MC_M_AFTER_ON, "after on");
	else if ( strcmp(code, "mark.aoff") == 0 ) marks(MC_M_AFTER_OFF, "after off");
	else if ( strcmp(code, "mark.find") == 0 ) mark_search(FALSE);
	else if ( strcmp(code, "mark.add") == 0 ) mark_search(TRUE);
	else if ( strcmp(code, "print") == 0 ) dlg_print();
	else if ( strcmp(code, "print.setup") == 0 ) dlg_print_setup();
	else link_op(code, index);
	g_cw.dirty = TRUE;
	g_lw.dirty = TRUE;
}

/* ---------------------------------------------------------------- the menu */

static void menu( CONST T_OBNTM *m, MCWIN *w )
{
	TS_UUID	def;
	T_MNSEL	s;
	ID	mid;
	BOOL	chosen, link = (BOOL)( !g_in_list && g_link_on && g_field >= 0 && mc_get(cur_card(), g_field) != NULL );

	if ( w == &g_cw && g_screen == SCR_CARD && field_at_link(m->x, m->y) >= 0 ) {
		g_field = field_at_link(m->x, m->y);
		g_link_on = TRUE;
		link = TRUE;
		card_paint();
	}
	if ( ui_uuid(SYSDEF_MENU_MCARD, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "trash", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "list", g_lw.open ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "scr.find", ( g_screen == SCR_FIND ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "scr.design", ( g_screen == SCR_DESIGN ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "search.off", g_db.searching ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "card.delfound", g_db.searching ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "found.tray", g_db.searching ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "paste", tray_has() ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "moveback", tray_has() ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "print", g_db.nf > 0 ? 0 : MN_GREY);
	tray_menu(mid);
	if ( !link ) {
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "exec", MN_GREY);
	} else {
		BOOL	kept = app_kept();

		(void)mn_chg_atr(mid, NULL, "vobj.reconnect", MN_HIDE);
		(void)mn_chg_atr(mid, NULL, "real.lock", kept ? MN_HIDE : 0);
		(void)mn_chg_atr(mid, NULL, "real.unlock", kept ? 0 : MN_HIDE);
		exec_menu(mid);
	}
	chosen = (BOOL)( mn_pop_men(mid, w->kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		char	line[80];

		snprintf(line, sizeof(line), "menu %s", (char *)s.code);
		note(line);
		command((CONST char *)s.code, s.index);
	}
}

/* ---------------------------------------------------------------- what happens */

void event( CONST T_OBNTM *m )
{
	MCWIN	*w = NULL;

	if ( card_bar_event(m) ) return;
	if ( g_cw.open && memcmp(&m->uuid, &g_cw.win, sizeof(TS_UUID)) == 0 ) w = &g_cw;
	else if ( g_lw.open && memcmp(&m->uuid, &g_lw.win, sizeof(TS_UUID)) == 0 ) w = &g_lw;
	if ( w == NULL ) return;
	if ( m->event & ( OB_E_PRESS | OB_E_KEY ) ) g_in_list = (BOOL)( w == &g_lw );
	switch ( m->event ) {
	case OB_E_CLOSE:
		if ( w == &g_lw ) list_close();
		else close_asked();
		return;
	case OB_E_DELETE:
		if ( w == &g_lw ) {
			g_lw.open = FALSE;
			g_cw.dirty = TRUE;
		} else {
			g_closing = TRUE;
		}
		return;
	case OB_E_REDRAW:
		resized(w);
		return;
	case OB_E_PRESS:
		if ( m->code == 1 && m->recno != OB_WR_CAND ) {
			/* the second button while a row is carried makes it a copy; else the menu */
			if ( w == &g_lw && list_holding() ) list_event(m);
			else menu(m, w);
			return;
		}
		break;
	default:
		break;
	}
	if ( w == &g_cw ) card_event(m);
	else list_event(m);
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
	INT	k;

	memset(arg, 0, sizeof(arg));
	argn = ts_get_arg(arg, sizeof(arg) - 1);
	if ( argn < (SZ)sizeof(g_obj) || ob_ref_obj((CONST TS_UUID *)arg, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("マイクロカードは、カード用紙の実身を開いて使います");
		return 1;
	}
	memcpy(&g_obj, arg, sizeof(g_obj));
	g_autoref = (BOOL)( ( ref.flags & OB_F_AUTOREF ) != 0 );
	memcpy(g_name, ref.name, OB_NAME_MAX);
	g_name[OB_NAME_MAX - 1] = 0;
	snprintf(title, sizeof(title), "%s", (char *)ref.name);
	mc_init(&g_db);
	g_cw.gid = g_lw.gid = -1;
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
		(void)wm_msg_put("マイクロカードの実身を読めません");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	if ( card_open(title) < E_OK ) {
		note("no window");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	if ( g_db.nf == 0 ) card_screen(SCR_DESIGN);	/* a new database: the cards are designed first */
	snprintf(s, sizeof(s), "ready %s fields %d cards %d", title, g_db.nf, g_db.n);
	note(s);
	say(g_db.nf == 0 ? "項目名を入力してカードを設計し、〔設定〕を押してください" : "カードの項目を押して入力します");
	redraw_all();

	while ( !g_closing ) {
		for ( TMO tmo = TMO_FEVR; !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) event(&m);
		if ( !g_closing && ( g_cw.dirty || g_lw.dirty ) ) redraw_all();
	}
	ed_end();
	list_close();
	win_free(&g_cw);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	for ( k = 0; k < MC_FIELD_MAX; k++ ) free(g_cond[k]);
	mc_clear(&g_db);
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}

/* ---------------------------------------------------------------- what the parts ask of the program */

void *mc_alloc( INT size )
{
	return malloc((size_t)( size > 0 ? size : 1 ));
}

void mc_free( void *p )
{
	free(p);
}

void *cx_alloc( INT size )
{
	return malloc((size_t)( size > 0 ? size : 1 ));
}

void cx_free( void *p )
{
	free(p);
}

INT mc_link_name( CONST UB *el, INT len, UB *out, INT max )
{
	TS_UUID	u;
	T_OBREF	ref;
	INT	n;

	if ( max <= 0 ) return 0;
	out[0] = 0;
	if ( !ct_target(el, len, &u) || ob_ref_obj(&u, &ref) < E_OK ) return 0;
	n = (INT)strnlen((CONST char *)ref.name, OB_NAME_MAX);
	if ( n > max - 1 ) n = max - 1;
	memcpy(out, ref.name, (size_t)n);
	out[n] = 0;
	return n;
}

void mc_new_vobjid( char *out37 )
{
	TS_UUID	u;

	new_vobjid(&u);
	ct_uuid_str(&u, out37);
}
