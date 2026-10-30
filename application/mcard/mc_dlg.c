/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_dlg.c
 *	マイクロカード: the panels asked over the card window (design 11.18)
 *
 *	Each is a panel of the program's data box (SYSDEF_BOX_MCARD), built
 *	and worked by the window manager: 表示設定, 状態表示, 並べ替え and
 *	its question, 重複検索, 新しい実身へ (the form and the name), the
 *	questions before a card is deleted, the window closed or the format
 *	changed, 印刷 (差し込み) and 印刷設定. While one is up, what comes for
 *	the windows behind it is drawn as it comes.
 */

#include "mc.h"

#define PN_VIEW		2
#define PN_STATUS	3
#define PN_SORT		4
#define PN_SORTOK	5
#define PN_DUPS		6
#define PN_EXPORT	7
#define PN_DELETE	8
#define PN_CLOSE	9
#define PN_WARN		10
#define PN_FORMAT	11
#define PN_PRINT	12
#define PN_PSETUP	13

#define B_CANCEL	101
#define B_OK		102
#define B_OTHER		103

static CONST INT sizes[10] = { 5, 7, 8, 10, 12, 14, 17, 19, 24, 29 };

static INT size_index( INT pt )
{
	INT	k;

	for ( k = 0; k < 10; k++ ) {
		if ( sizes[k] >= pt ) return k;
	}
	return 3;
}

/* A panel worked until a button is pushed: the button, or 0 for Esc and the panel closed */
static INT work( UI *q, void (*changed)( UI *q, INT num ) )
{
	T_OBNTM	m;
	INT	num, ev;

	for ( ;; ) {
		if ( g_cw.dirty || g_lw.dirty ) redraw_all();
		/* asleep until the panel or the windows have something */
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(q, &m, &num) : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			return num;
		case UI_EV_CHANGE:
			if ( changed != NULL ) changed(q, num);
			break;
		case UI_EV_CLOSE:
			return 0;
		case UI_EV_OTHER:
			if ( m.event == OB_E_REDRAW ) event(&m);
			break;
		default:
			break;
		}
		if ( g_closing ) return 0;
	}
}

static void names_of_fields( UI *q, INT num )
{
	CONST char	*nm[MC_FIELD_MAX];
	INT		k;

	for ( k = 0; k < g_db.nf; k++ ) nm[k] = (CONST char *)g_db.f[k].name;
	ui_names(q, num, nm, g_db.nf);
}

/* ---------------------------------------------------------------- 表示設定 */

void dlg_view( void )
{
	UI	q;
	INT	a;

	if ( ui_dlg_open(&g_ui, PN_VIEW, &q) < E_OK ) return;
	ui_set_value(&q, 11, size_index(g_db.view.csize) + 1);
	ui_set_value(&q, 12, g_db.view.cprop ? 1 : 0);
	ui_set_value(&q, 13, g_db.view.cgray ? 1 : 0);
	ui_set_value(&q, 14, size_index(g_db.view.lsize) + 1);
	ui_set_value(&q, 15, g_db.view.lprop ? 1 : 0);
	ui_set_value(&q, 16, g_db.view.lgray ? 1 : 0);
	ui_set_value(&q, 17, g_db.view.names ? 1 : 0);
	note("view panel");
	a = work(&q, NULL);
	if ( a == B_OK ) {
		INT	cs = ui_value(&q, 11) - 1, ls = ui_value(&q, 14) - 1;
		char	s[80];

		if ( cs >= 0 && cs < 10 ) g_db.view.csize = sizes[cs];
		if ( ls >= 0 && ls < 10 ) g_db.view.lsize = sizes[ls];
		g_db.view.cprop = (BOOL)( ui_value(&q, 12) != 0 );
		g_db.view.cgray = (BOOL)( ui_value(&q, 13) != 0 );
		g_db.view.lprop = (BOOL)( ui_value(&q, 15) != 0 );
		g_db.view.lgray = (BOOL)( ui_value(&q, 16) != 0 );
		g_db.view.names = (BOOL)( ui_value(&q, 17) != 0 );
		g_changed = TRUE;
		snprintf(s, sizeof(s), "view card %d list %d names %s", g_db.view.csize, g_db.view.lsize, g_db.view.names ? "on" : "off");
		note(s);
	}
	ui_dlg_close(&q);
	redraw_all();
}

/* ---------------------------------------------------------------- 状態表示 */

void dlg_status( void )
{
	char	a[120], b[120];
	CONST char *l[2];
	INT	datalen = 4, k, more, links = mc_link_count(&g_db);

	for ( k = 0; k < g_db.nf; k++ ) {
		datalen += ( g_db.f[k].type == MC_T_NUM ) ? 8 : ( g_db.f[k].type == MC_T_LINK ) ? 48 : 2 * ( g_db.f[k].max + 1 );
	}
	/* the cards a record holds, as far as the program reads one */
	more = ( OUT_MAX - g_db.n * datalen ) / datalen;
	if ( more > MC_CARD_MAX - g_db.n ) more = MC_CARD_MAX - g_db.n;
	if ( more < 0 ) more = 0;
	snprintf(a, sizeof(a), "カード枚数：現在 %d 枚、あと約 %d 枚追加できます", g_db.n, more);
	snprintf(b, sizeof(b), "仮身数：現在 %d 個、あと %d 個格納できます", links, MC_LINK_MAX - links);
	l[0] = a;
	l[1] = b;
	{
		char	s[80];

		snprintf(s, sizeof(s), "status cards %d more %d links %d", g_db.n, more, links);
		note(s);
	}
	(void)ui_ask(&g_ui, PN_STATUS, l, 2);
	redraw_all();
}

/* ---------------------------------------------------------------- 並べ替え, 重複検索 */

void dlg_sort( void )
{
	UI	q;
	INT	a, f = -1;
	BOOL	desc = FALSE;

	if ( g_db.nf == 0 ) return;
	ed_commit();
	if ( ui_dlg_open(&g_ui, PN_SORT, &q) < E_OK ) return;
	names_of_fields(&q, 11);
	ui_set_value(&q, 11, ( g_field >= 0 && g_field < g_db.nf ) ? g_field + 1 : 1);
	ui_set_value(&q, 12, 1);
	note("sort panel");
	a = work(&q, NULL);
	if ( a == B_OK ) {
		f = ui_value(&q, 11) - 1;
		desc = (BOOL)( ui_value(&q, 12) == 2 );
	}
	ui_dlg_close(&q);
	if ( f >= 0 && f < g_db.nf ) {
		char		s[120];
		CONST char	*l[1];

		snprintf(s, sizeof(s), "「%s」の%sに並べ替えます。", (char *)g_db.f[f].name, desc ? "降順" : "昇順");
		l[0] = s;
		if ( ui_ask(&g_ui, PN_SORTOK, l, 1) == B_OK ) {
			MCCARD	*keep = cur_card();
			INT	i;

			if ( mc_sort(&g_db, f, desc) >= E_OK ) {
				g_changed = TRUE;
				/* the card shown stays shown, wherever it went */
				for ( i = 0; i < g_db.n && g_db.c[i] != keep; i++ ) ;
				g_cur = ( i < g_db.n ) ? mc_pos(&g_db, i) : 0;
				if ( g_cur < 0 ) g_cur = 0;
				snprintf(s, sizeof(s), "sorted %d %s", f + 1, desc ? "down" : "up");
				note(s);
			}
		}
	}
	redraw_all();
}

void dlg_dups( void )
{
	UI	q;
	INT	a, f = -1;

	if ( g_db.nf == 0 ) return;
	ed_commit();
	if ( ui_dlg_open(&g_ui, PN_DUPS, &q) < E_OK ) return;
	names_of_fields(&q, 11);
	ui_set_value(&q, 11, ( g_field >= 0 && g_field < g_db.nf ) ? g_field + 1 : 1);
	note("dups panel");
	a = work(&q, NULL);
	if ( a == B_OK ) f = ui_value(&q, 11) - 1;
	ui_dlg_close(&q);
	if ( f >= 0 && f < g_db.nf ) {
		INT	n = mc_dups(&g_db, f);
		char	s[80];

		g_cur = 0;
		snprintf(s, sizeof(s), "dups %d found %d", f + 1, n);
		note(s);
		snprintf(s, sizeof(s), "重複したカード %d 枚を検索しました", n > 0 ? n : 0);
		say(s);
	}
	redraw_all();
}

/* ---------------------------------------------------------------- 新しい実身へ */

void dlg_export( void )
{
	UI	q;
	INT	a, form = 0;
	char	name[OB_NAME_MAX + 8];

	ed_commit();
	if ( ui_dlg_open(&g_ui, PN_EXPORT, &q) < E_OK ) return;
	ui_set_value(&q, 11, 1);
	snprintf(name, sizeof(name), "%sのコピー", (char *)g_name);
	ui_set_text(&q, 12, name);
	note("export panel");
	a = work(&q, NULL);
	if ( a == B_OK ) {
		INT	n;

		form = ui_value(&q, 11) - 1;
		n = ui_text(&q, 12, name, OB_NAME_MAX);
		if ( n < 0 ) n = 0;
		name[( n < OB_NAME_MAX ) ? n : OB_NAME_MAX - 1] = 0;
	}
	ui_dlg_close(&q);
	if ( a == B_OK ) {
		if ( name[0] == 0 ) {
			warn("新しい実身の名前を入力してください。", NULL);
		} else if ( export_obj(( form >= 0 && form <= EX_CSV ) ? form : 0, (CONST UB *)name) < E_OK ) {
			warn("新しい実身に書き出せませんでした。", NULL);
		}
	}
	redraw_all();
}

/* ---------------------------------------------------------------- questions */

BOOL dlg_delete( CONST char *what )
{
	CONST char	*l[2];
	BOOL		yes;

	l[0] = what;
	l[1] = "削除したカードは取り消せません。";
	yes = (BOOL)( ui_ask(&g_ui, PN_DELETE, l, 2) == B_OK );
	redraw_all();
	return yes;
}

INT dlg_close( void )
{
	INT	a = ui_ask(&g_ui, PN_CLOSE, NULL, 0);

	redraw_all();
	return ( a == B_OK ) ? DLG_SAVE : ( a == B_OTHER ) ? DLG_DROP : 0;
}

BOOL dlg_format( INT lost )
{
	char		s[100];
	CONST char	*l[2];
	BOOL		yes;

	snprintf(s, sizeof(s), "%d 個の項目でデータが失われるか書き換わります。", lost);
	l[0] = s;
	l[1] = "形式を変更しますか？";
	yes = (BOOL)( ui_ask(&g_ui, PN_FORMAT, l, 2) == B_OK );
	note(yes ? "format change accepted" : "format change refused");
	redraw_all();
	return yes;
}

/* ---------------------------------------------------------------- 印刷, 印刷設定 */

#define LAYOUT_MAX	16

static UB	lay_name[LAYOUT_MAX + 1][MC_LAYOUT_NAME];
static TS_UUID	lay_obj[LAYOUT_MAX + 1];
static INT	nlay;

static void layouts( UI *q, INT num )
{
	CONST char	*nm[LAYOUT_MAX + 1];
	INT		k;

	snprintf((char *)lay_name[0], MC_LAYOUT_NAME, "一覧表(標準形式)");
	nlay = 1 + layout_names(lay_name + 1, lay_obj + 1, LAYOUT_MAX);
	for ( k = 0; k < nlay; k++ ) nm[k] = (CONST char *)lay_name[k];
	ui_names(q, num, nm, nlay);
	ui_set_value(q, num, 1);
}

/* The layout chosen: "" the list */
static CONST UB *layout_chosen( UI *q, INT num )
{
	INT	k = ui_value(q, num) - 1;

	return ( k >= 1 && k < nlay ) ? lay_name[k] : (CONST UB *)"";
}

static void print_count( UI *q )
{
	INT		one = ( ui_value(q, 12) == 1 ), n, pages;
	CONST UB	*lay = layout_chosen(q, 11);
	MCPRINT		*p = mc_print_of(&g_db, lay);
	char		s[96];

	if ( lay[0] == 0 ) one = FALSE;		/* the list: always all the cards */
	n = one ? 1 : mc_count(&g_db);
	pages = mc_pages(&g_db, p, (BOOL)( lay[0] != 0 && p->across * ( p->down > 0 ? p->down : 1 ) > 1 ), (BOOL)( lay[0] == 0 ), n);
	snprintf(s, sizeof(s), "印刷カード数 %d 枚　印刷ページ数 %d ページ", n, pages);
	ui_set_text(q, 13, s);
}

static void print_changed( UI *q, INT num )
{
	if ( num == 11 || num == 12 ) print_count(q);
}

void dlg_print( void )
{
	UI	q;
	INT	a;
	UB	lay[MC_LAYOUT_NAME];
	BOOL	one = FALSE;

	ed_commit();
	if ( ui_dlg_open(&g_ui, PN_PRINT, &q) < E_OK ) return;
	layouts(&q, 11);
	ui_set_value(&q, 12, 2);
	print_count(&q);
	note("print panel");
	a = work(&q, print_changed);
	snprintf((char *)lay, sizeof(lay), "%s", (CONST char *)layout_chosen(&q, 11));
	one = (BOOL)( ui_value(&q, 12) == 1 && lay[0] != 0 );
	ui_dlg_close(&q);
	if ( a == B_OK ) {
		ER	er = merge_print(lay, one);

		if ( er == E_LIMIT ) warn("印刷ページ数が500ページを超えます。", "検索で対象のカードを絞ってください。");
		else if ( er < E_OK && er != E_ABORT ) warn("印刷できませんでした。", NULL);
	}
	redraw_all();
}

static void setup_show( UI *q )
{
	MCPRINT	*p = mc_print_of(&g_db, layout_chosen(q, 11));
	INT	v;

	v = p->across;
	ui_set_fields(q, 12, &v, 1);
	v = p->down;
	ui_set_fields(q, 13, &v, 1);
	v = p->gapx / 10;
	ui_set_fields(q, 14, &v, 1);
	v = p->gapy / 10;
	ui_set_fields(q, 15, &v, 1);
	ui_set_value(q, 16, p->downfirst ? 2 : 1);
	ui_set_value(q, 17, p->paper + 1);
	ui_set_value(q, 18, p->turn ? 2 : 1);
	ui_set_text(q, 19, (CONST char *)p->head);
	ui_set_text(q, 20, (CONST char *)p->foot);
}

static void setup_keep( UI *q )
{
	MCPRINT	*p = mc_print_of(&g_db, layout_chosen(q, 11));
	INT	v = 0, n;
	char	s[64];

	if ( ui_fields(q, 12, &v, 1) >= 0 ) p->across = ( v >= 1 && v <= 20 ) ? v : 1;
	if ( ui_fields(q, 13, &v, 1) >= 0 ) p->down = ( v >= 0 && v <= 50 ) ? v : 1;
	if ( ui_fields(q, 14, &v, 1) >= 0 ) p->gapx = v * 10;
	if ( ui_fields(q, 15, &v, 1) >= 0 ) p->gapy = v * 10;
	p->downfirst = (BOOL)( ui_value(q, 16) == 2 );
	v = ui_value(q, 17) - 1;
	p->paper = ( v >= 0 && v < MC_PAPERS ) ? v : MC_PAPER_A4;
	p->turn = (BOOL)( ui_value(q, 18) == 2 );
	n = ui_text(q, 19, s, sizeof(s));
	if ( n < 0 ) n = 0;
	s[n < (INT)sizeof(s) ? n : (INT)sizeof(s) - 1] = 0;
	snprintf((char *)p->head, sizeof(p->head), "%s", s);
	n = ui_text(q, 20, s, sizeof(s));
	if ( n < 0 ) n = 0;
	s[n < (INT)sizeof(s) ? n : (INT)sizeof(s) - 1] = 0;
	snprintf((char *)p->foot, sizeof(p->foot), "%s", s);
}

static INT	setup_was;

static void setup_changed( UI *q, INT num )
{
	if ( num != 11 ) return;
	/* another layout: what was set for the one before is kept, the new one's shown */
	{
		INT	now = ui_value(q, 11);

		ui_set_value(q, 11, setup_was);
		setup_keep(q);
		ui_set_value(q, 11, now);
		setup_was = now;
	}
	setup_show(q);
}

void dlg_print_setup( void )
{
	UI	q;
	INT	a;

	ed_commit();
	if ( ui_dlg_open(&g_ui, PN_PSETUP, &q) < E_OK ) return;
	layouts(&q, 11);
	setup_was = 1;
	setup_show(&q);
	note("print setup panel");
	a = work(&q, setup_changed);
	if ( a == B_OK ) {
		setup_keep(&q);
		g_changed = TRUE;
		note("print set");
	}
	ui_dlg_close(&q);
	redraw_all();
}
