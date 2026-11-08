/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dc_main.c
 *	基本文章編集 as a program running as a process (design 11.3)
 *
 *	The program is started on a document's object and shows it in an
 *	ordinary window, with a frame, a title, its size changed by its
 *	corner and scroll bars down the right and along the foot. The text
 *	is drawn by the same code that draws it in the desktop, linked into
 *	the program (include/ts/tvport.h), and edited by the same editor of
 *	text (application/doc/docedit.c): what the editor asks of the one
 *	showing the text is dc_host.c's. A window of the desktop's showing a
 *	document and this window lay the text on the same page: as wide as
 *	the window, or as the paper when the text does not wrap to the
 *	window or 用紙枠 is shown.
 *
 *	The record is held in the program's memory and written back to the
 *	object by 元の実身に保存 (Ctrl+S) and when the window is closed, when
 *	it has been changed. A window that could not be saved is not closed.
 *	What any editor running as a process does with its object, its
 *	window and the desktop is lib/libtv/tv_app.c's.
 *
 *	The caret of the window that has the keys is shown and hidden by
 *	turns (LK_BLINK), a turn counted from the last key or press. While
 *	nothing comes, the paragraphs laid only as a guess are laid a few at
 *	a time, as the desktop lays them while it is idle.
 *
 *	The second button opens the editor's menu (SYSDEF_MENU_DOC).
 *
 *	What it does is said on the console for the tests ("document: ...").
 */

#include "dc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/look.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/obdrop.h>
#include <ts/tvsrc.h>
#include <ts/tadlnk.h>
#include <ts/docmenu.h>

/* The program object of 基本文章編集: the icon of its windows */
#define DC_PROG		"01a0d6d1-08d1-781f-8f58-35c45aeacef9"

#define WHEEL_STEP	48		/* a notch of the wheel */
#define SETTLE_MS	20		/* laying in one turn of the loop, at most */
#define SETTLE_DRAW	25		/* turns between two drawings of the scroll bar */
#define PAPER_DESK	0x00A0A0A0U	/* beside the paper */
#define PAPER_EDGE	0x00606060U

#define K_DEL		0x4C
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- what it holds */

T_DDED		g_de;
T_TVDOC		*g_doc;
T_TAD		*g_xrec;
INT		g_sx, g_sy;
INT		g_height;
UINT		g_kind = TV_KIND_DOC;
BOOL		g_xml_view, g_detail_view, g_sealed, g_used = TRUE, g_settling;
BOOL		g_nowrap;
BOOL		g_paper_frame;
BOOL		g_show_hidden;
BOOL		g_caret_hidden;
BOOL		g_banded;
T_DPRECT	g_band;

static UINT	g_band_asks;		/* how many times a part was asked to be drawn */
static UD	g_key_at;		/* when the key typed into the text happened, until it is shown */
static UD	g_key_in, g_key_done;	/* when it came to the program, and when the editor was done with it */
static UD	g_blink_at;		/* when the caret's turn last changed, or a key or a press showed it */
static INT	g_settle_turns, g_settle_left;

/* the link under the pointer when the menu was opened, for 実行 */
static T_VOBJ	g_link;
static BOOL	g_has_link;

/* ---------------------------------------------------------------- saying things */

/* A panel over the window that says what could not be done, two lines and 了解 */
void tell( CONST char *l1, CONST char *l2 )
{
	CONST char	*lines[2];

	lines[0] = l1;
	lines[1] = ( l2 != NULL ) ? l2 : " ";
	(void)ui_ask(&g_ui, PN_TELL, lines, 2);
	g_dirty = TRUE;
}

/* A number of the look: what the system says, or dflt when it says none */
INT dc_look( UINT num, INT dflt )
{
	INT	v = (INT)wm_look(num);

	return ( v > 0 || dflt <= 1 ) ? v : dflt;
}

/* ---------------------------------------------------------------- the model and the page */

/*
 * The model made again from the record, what of its layout still holds
 * taken over: what is drawn and asked about lays only what it wants and
 * what has changed. In 原稿, the record's text is what is shown.
 */
void dc_model( void )
{
	T_TVDOC	*was = g_doc;

	g_doc = NULL;
	if ( g_rec == NULL ) {
		if ( was != NULL ) tv_doc_free(was);
		return;
	}
	(void)tv_doc_again(( g_xml_view && g_xrec != NULL ) ? g_xrec : g_rec, was, &g_doc);
	if ( g_doc != NULL && tv_doc_cache(g_doc, was) >= E_OK ) g_settling = TRUE;
	if ( g_doc != NULL && !g_xml_view ) {
		g_doc->detail = g_detail_view;
		g_doc->show_hidden = g_show_hidden;
		g_doc->page_h = !g_paper_frame ? 0 : ( g_doc->paper_h > 0 ) ? g_doc->paper_h : DC_PAPER_H;
		if ( g_paper_frame && g_doc->vert ) {
			/* 縦書き: the pages follow one another across */
			g_doc->page_h = ( g_doc->paper_w > 0 ) ? g_doc->paper_w : DC_PAPER_W;
		}
	}
	if ( was != NULL ) tv_doc_free(was);
}

/*
 * Where the text is laid: the work area, to the frame on every side. A
 * document not wrapped to the window is wrapped to its paper: the
 * paper's whole width when the paper is shown, its margins and all, and
 * the width inside the margins when it is not. 縦書き: the paper decides
 * how tall the page is, likewise.
 */
void dc_work_rect( T_DPRECT *r )
{
	r->left = 0;
	r->top = 0;
	r->right = g_w;
	r->bottom = g_h;
	if ( g_doc != NULL && !g_xml_view && g_doc->vert && ( g_paper_frame || g_nowrap ) ) {
		INT	ph = ( g_doc->paper_h > 0 ) ? g_doc->paper_h : DC_PAPER_H;

		if ( !g_paper_frame ) ph -= g_doc->margin[1] + g_doc->margin[3];
		if ( ph < 64 ) ph = 64;
		r->bottom = r->top + ph;
	} else if ( g_doc != NULL && !g_xml_view && ( g_paper_frame || g_nowrap ) ) {
		INT	pw = ( g_doc->paper_w > 0 ) ? g_doc->paper_w : DC_PAPER_W;

		if ( !g_paper_frame ) pw -= g_doc->margin[0] + g_doc->margin[2];
		if ( pw < 64 ) pw = 64;
		r->right = r->left + pw;
	}
}

/*
 * 用紙枠: the edge of each page of the paper, and the desk beside it.
 * The pages are as the layout made them, one under another, each as
 * tall as the paper; 縦書き, side by side from the right.
 */
static void paper_frame( CONST T_DPRECT *r )
{
	T_DPRECT	pg, desk;
	INT		ph = g_doc->page_h, k, pages;

	if ( ph <= 0 ) return;
	if ( g_doc->vert ) {
		desk.left = 0;
		desk.top = r->bottom + 1;
		desk.right = g_w;
		desk.bottom = g_h;
		if ( desk.bottom > desk.top ) dp_fill_rect(g_gid, &desk, PAPER_DESK);
		pages = ( g_height + ph - 1 ) / ph;
		if ( pages < 1 ) pages = 1;
		desk.top = r->top;
		desk.bottom = r->bottom + 1;
		desk.left = 0;
		desk.right = r->right - pages * ph + g_sy;
		if ( desk.right > desk.left ) dp_fill_rect(g_gid, &desk, PAPER_DESK);
		for ( k = 0; k < pages; k++ ) {
			pg.right = r->right - k * ph + g_sy;
			pg.left = pg.right - ph - 1;
			pg.top = r->top;
			pg.bottom = r->bottom + 1;
			if ( pg.right < 0 || pg.left > r->right ) continue;
			dp_frame_rect(g_gid, &pg, PAPER_EDGE, 1);
		}
		return;
	}
	desk.left = r->right + 1;
	desk.top = 0;
	desk.right = g_w;
	desk.bottom = g_h;
	if ( desk.right > desk.left ) dp_fill_rect(g_gid, &desk, PAPER_DESK);
	pages = ( g_height + ph - 1 ) / ph;
	if ( pages < 1 ) pages = 1;
	for ( k = 0; k < pages; k++ ) {
		pg.left = r->left;
		pg.right = r->right + 1;
		pg.top = r->top + k * ph - g_sy;
		pg.bottom = pg.top + ph;
		if ( pg.bottom < 0 || pg.top > desk.bottom ) continue;
		dp_frame_rect(g_gid, &pg, PAPER_EDGE, 1);
	}
}

/*
 * The scroll bars, to say how much of the text is shown. 縦書き: the
 * text runs on to the left, so the bar across is the one that winds
 * it, its knob at the right end at the head of the text; where the knob
 * is, is kept in g_sx, so that what moves the knob moves the text.
 */
static void bars( CONST T_DPRECT *r )
{
	T_OBWBARS	b;
	SZ		asz = 0;
	INT		hi;

	memset(&b, 0, sizeof(b));
	if ( g_doc != NULL && g_doc->vert && !g_xml_view ) {
		INT	view = r->right - r->left, room;

		b.bar[OB_BAR_R].hi = ( r->bottom - r->top > 0 ) ? r->bottom - r->top : 1;
		b.bar[OB_BAR_R].chi = b.bar[OB_BAR_R].hi;
		hi = ( g_height > view ) ? g_height : view;
		if ( hi <= 0 ) hi = 1;
		room = hi - view;
		g_sx = room - g_sy;
		if ( g_sx < 0 ) g_sx = 0;
		b.bar[OB_BAR_B].hi = hi;
		b.bar[OB_BAR_B].clo = g_sx;
		b.bar[OB_BAR_B].chi = ( g_sx + view > hi ) ? hi : g_sx + view;
	} else {
		b.bar[OB_BAR_R].hi = ( g_height > 0 ) ? g_height : 1;
		b.bar[OB_BAR_R].clo = g_sy;
		b.bar[OB_BAR_R].chi = g_sy + ( r->bottom - r->top );
		if ( b.bar[OB_BAR_R].chi > b.bar[OB_BAR_R].hi ) b.bar[OB_BAR_R].chi = b.bar[OB_BAR_R].hi;
		/* across: as wide as the page, unless scrolled over */
		hi = r->right - r->left;
		if ( hi < g_sx + ( r->right - r->left ) ) hi = g_sx + ( r->right - r->left );
		if ( hi <= 0 ) hi = 1;
		b.bar[OB_BAR_B].hi = hi;
		b.bar[OB_BAR_B].clo = g_sx;
		b.bar[OB_BAR_B].chi = g_sx + ( r->right - r->left );
		if ( b.bar[OB_BAR_B].chi > hi ) b.bar[OB_BAR_B].chi = hi;
	}
	(void)ob_wri_rec(g_kw, OB_WR_BARS, 0, &b, (SZ)( sizeof(T_OBWBAR) * 2 ), &asz);
}

/*
 * The window drawn: the page in its colour to the frame, the text over
 * it, 用紙枠, and the selection and the caret. clip, when it is given,
 * is all that goes to the screen.
 */
static void paint( BOOL quiet, CONST T_DPRECT *clip )
{
	T_DPRECT	all, r;
	UD		t0 = ta_now(), f0, f1, f2, f3, f4, c0 = tv_fn_calls();
	char		s[128];

	all.left = all.top = 0;
	all.right = g_w;
	all.bottom = g_h;
	if ( clip != NULL ) (void)dp_set_visible(g_gid, clip);
	dp_fill_rect(g_gid, &all, ( g_paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : g_paper);
	dc_work_rect(&r);
	f0 = ta_now();
	if ( g_doc != NULL ) {
		tv_src_self(&g_obj);
		tv_src_prefetch(g_rec);
		g_height = tv_doc_draw(g_gid, g_doc, &r, g_sy, g_rec, g_paper);
		tv_src_self(NULL);
		if ( g_doc->layc != NULL ) g_settling = TRUE;	/* what was not laid at this width, laid while idle */
		if ( g_paper_frame && !g_xml_view ) paper_frame(&r);
	}
	f1 = ta_now();
	de_draw(&g_de, g_gid);
	if ( clip != NULL ) (void)dp_set_visible(g_gid, &all);
	f2 = ta_now();
	(void)wm_obj_flush(g_kw, clip);
	f3 = ta_now();
	bars(&r);
	f4 = ta_now();
	if ( clip == NULL ) g_dirty = g_banded = FALSE;
	/* the times taken before anything is said: saying it on the console is not drawing */
	if ( !quiet ) {
		snprintf(s, sizeof(s), "drew in %d us text %d us letters %d flush %d us bars %d us",
			 (INT)( ( f4 - t0 ) / 1000 ), (INT)( ( f1 - f0 ) / 1000 ), (INT)( tv_fn_calls() - c0 ),
			 (INT)( ( f3 - f2 ) / 1000 ), (INT)( ( f4 - f3 ) / 1000 ));
		note(s);
	}
	if ( g_key_at != 0 ) {
		/* from the key, as the keyboard told it, to the window shown */
		snprintf(s, sizeof(s), "key to screen %d us came %d us edited %d us drawn from %d us",
			 (INT)( ( f4 - g_key_at ) / 1000 ), (INT)( ( g_key_in - g_key_at ) / 1000 ),
			 (INT)( ( g_key_done - g_key_in ) / 1000 ), (INT)( ( t0 - g_key_at ) / 1000 ));
		note(s);
		g_key_at = 0;
	}
	g_input_at = 0;
}

/*
 * A part of the work area to be drawn again, in the work area: joined to
 * what is waiting to be, and drawn when the notices waiting have been
 * taken, unless the whole is to be by then
 */
void dc_band( CONST T_DPRECT *r )
{
	T_DPRECT	b = *r;

	g_band_asks++;
	if ( b.left < 0 ) b.left = 0;
	if ( b.top < 0 ) b.top = 0;
	if ( b.right > g_w ) b.right = g_w;
	if ( b.bottom > g_h ) b.bottom = g_h;
	if ( b.right <= b.left || b.bottom <= b.top ) return;
	if ( !g_banded ) {
		g_band = b;
		g_banded = TRUE;
		return;
	}
	if ( b.left < g_band.left ) g_band.left = b.left;
	if ( b.top < g_band.top ) g_band.top = b.top;
	if ( b.right > g_band.right ) g_band.right = b.right;
	if ( b.bottom > g_band.bottom ) g_band.bottom = b.bottom;
}

/* The caret's turn: the window drawn again inside the caret alone */
static void caret_turn( void )
{
	T_DPRECT	r;

	g_caret_hidden = (BOOL)!g_caret_hidden;
	if ( de_caret_rect(&g_de, &r) ) paint(TRUE, &r);
	else paint(TRUE, NULL);
}

/* A key or a press: the caret shown, and its turn counted from now */
static void blink_restart( void )
{
	T_DPRECT	r;

	g_blink_at = ta_now();
	if ( g_caret_hidden ) {
		/* the caret shown again: only it drawn */
		g_caret_hidden = FALSE;
		if ( de_caret_rect(&g_de, &r) ) dc_band(&r);
		else g_dirty = TRUE;
	}
}

/* ---------------------------------------------------------------- winding */

static void scroll_to( INT to )
{
	T_DPRECT	r;
	INT		room;

	dc_work_rect(&r);
	room = g_height - ( r.bottom - r.top );
	if ( g_doc != NULL && g_doc->vert && !g_xml_view ) room = g_height - ( r.right - r.left );	/* 縦書き: across */
	if ( room < 0 ) room = 0;
	if ( to < 0 ) to = 0;
	if ( to > room ) to = room;
	if ( to == g_sy ) return;
	g_sy = to;
	g_dirty = TRUE;
}

/* Across: a document is as wide as its page and does not go across */
static void scroll_x_to( INT to )
{
	T_DPRECT	r;
	INT		room;

	dc_work_rect(&r);
	if ( g_doc != NULL && g_doc->vert && !g_xml_view ) {
		/* 縦書き: the text is wound as far as the knob is from the right end */
		room = g_height - ( r.right - r.left );
		if ( room < 0 ) room = 0;
		if ( to > room ) to = room;
		if ( to < 0 ) to = 0;
		scroll_to(room - to);
		return;
	}
	to = 0;
	if ( to == g_sx ) return;
	g_sx = to;
	g_dirty = TRUE;
}

/* A notch of the wheel: up and down, or across for the wheel that goes across or with Shift */
static void wheel( CONST T_OBNTM *m )
{
	BOOL	across = (BOOL)( m->code == HID_WHEEL_H || ( m->mods & M_SHIFT ) != 0 );

	if ( m->dz == 0 || dc_look(LK_PD_WHEEL, 1) == 0 ) return;
	if ( across ) scroll_x_to(g_sx + ( m->code == HID_WHEEL_H ? m->dz : -m->dz ) * WHEEL_STEP);
	else scroll_to(g_sy - m->dz * WHEEL_STEP);
}

/* While nothing comes: a few more of the paragraphs laid, the window drawn when the text moved */
static BOOL settle_pending( void )
{
	return (BOOL)( g_settling && g_doc != NULL && g_doc->layc != NULL );
}

static void settle_step( void )
{
	INT	left, shift = 0, laid = 0, was = g_settle_left;

	left = de_settle(&g_de, SETTLE_MS, &shift, &laid);
	g_settle_left = left;
	/* the last laid, the first time round too: drawn, the bars as tall as the text */
	if ( shift != 0 || ( left == 0 && ( was > 0 || laid > 0 ) )
	  || ( left > 0 && ++g_settle_turns % SETTLE_DRAW == 0 ) ) {
		paint(TRUE, NULL);
		if ( left == 0 ) g_settling = FALSE;	/* drawn with nothing left: done */
	}
}

/* ---------------------------------------------------------------- the record */

/* The document read from the object: E_OBJ when it is not a document */
static ER load( void )
{
	SZ	len = 0;
	UB	*x = NULL;
	T_TAD	*rec = NULL;
	ER	er;

	if ( ta_read(&x, &len) < E_OK ) return E_NOEXS;
	er = tad_parse(x, len, NULL, &rec);
	free(x);
	if ( er < E_OK ) return er;
	if ( tv_kind(rec) != TV_KIND_DOC ) {
		tad_free(rec);
		return E_OBJ;
	}
	g_rec = rec;
	g_kind = TV_KIND_DOC;
	ta_was();
	dc_model();
	if ( g_doc != NULL && tv_doc_has_sheet(g_doc) ) {
		/* something laid over the whole sheet -- the squares of 原稿用紙 -- is seen on its paper */
		g_paper_frame = TRUE;
		dc_model();
	}
	return E_OK;
}

/*
 * Opened from a link: shown as the link asks, in its viewMode (清書,
 * 詳細 or 原稿) with its wordWrap, and laid on the link's bgcol when the
 * object says no colour of its own. The desktop gives the link as it is
 * now (T_PRCOPEN), with what a window that closed wrote back into it
 * before the record holding it was saved.
 */
static void from_link( void )
{
	if ( !g_from_link ) return;
	if ( g_paper == TV_PAPER_LOOK && g_start_arg.bgcol != TAD_COL_NONE ) g_paper = g_start_arg.bgcol;
	if ( g_start_arg.wordwrap != TAD_WRAP_NONE ) g_nowrap = (BOOL)( g_start_arg.wordwrap == TAD_WRAP_OFF );
	if ( strcmp((CONST char *)g_start_arg.viewmode, "xml") == 0 ) {
		g_detail_view = FALSE;
		de_xml_enter(&g_de);
	} else {
		g_detail_view = (BOOL)( strcmp((CONST char *)g_start_arg.viewmode, "detailed") == 0 );
		de_view(&g_de);
	}
}

/* 元の実身に保存, when it has been changed; 原稿 being edited read back into the record first */
static ER save( void )
{
	if ( !g_changed || g_sealed ) return E_OK;
	if ( g_xml_view && !de_xml_commit(&g_de) ) return E_PAR;
	return ta_save();
}

/* Closing: what was changed saved first; a window that could not be saved stays */
static void close_asked( void )
{
	ta_ime_commit(&g_de);		/* a composition is part of what is saved */
	if ( save() < E_OK ) {
		tell("保存できなかったため、ウインドウを閉じませんでした", "メモリやディスクの空きを確かめてから、もう一度閉じてください");
		return;
	}
	ta_link_view_back(g_sx, g_sy, 0, g_nowrap ? TAD_WRAP_OFF : TAD_WRAP_ON,
			  g_xml_view ? "xml" : g_detail_view ? "detailed" : "formatted");
	ta_window_keep();
	g_closing = TRUE;
}

/* ---------------------------------------------------------------- the menu */

/* Whether the set in hand in the tray is one a text can take: a text or a figure */
static BOOL tray_has( void )
{
	T_TAD		*f = dc_tray_frag();
	T_TADNODE	*body;
	BOOL		has;

	if ( f == NULL ) return FALSE;
	body = tad_body(f);
	has = (BOOL)( body != NULL && body->name != NULL && ( body->name[0] == 'f' || body->name[0] == 'd' ) );
	tad_free(f);
	return has;
}

/* 印刷: what the window shows -- 原稿 its text, 詳細 its marks, the hidden letters when shown */
static void print_doc( void )
{
	UINT	flags = 0;

	if ( !g_xml_view && g_detail_view ) flags |= DT_F_PR_DETAIL;
	if ( !g_xml_view && g_show_hidden ) flags |= DT_F_PR_HIDDEN;
	(void)ta_print(( g_xml_view && g_xrec != NULL ) ? g_xrec : g_rec, flags);
}

static void doc_do( INT cmd )
{
	switch ( cmd ) {
	case DM_SAVE:
		(void)save();
		break;
	case DM_SAVE_AS_NEW:
		ta_save_new();
		break;
	case DM_BGCOLOUR:
		ta_paper_ask();
		break;
	case DM_REFRESH:
		tv_src_forget(NULL);
		ta_paper_read();
		break;
	case DM_FULLSCREEN:
		ta_fullscreen();
		break;
	case DM_PRINT:
		print_doc();
		break;
	case DM_WRAP:
		g_nowrap = (BOOL)!g_nowrap;
		de_view(&g_de);
		break;
	case DM_SHOW_HIDDEN:
		g_show_hidden = (BOOL)!g_show_hidden;
		de_view(&g_de);
		break;
	case DM_PAPER_FRAME:
		g_paper_frame = (BOOL)!g_paper_frame;
		de_view(&g_de);
		break;
	case DM_VIEW_SET_XML:
		if ( !g_xml_view ) {
			g_detail_view = FALSE;
			de_xml_enter(&g_de);
		}
		break;
	case DM_VIEW_SET_DETAIL:
	case DM_VIEW_SET_CLEAN:
		if ( g_xml_view && !de_xml_leave(&g_de) ) break;	/* the text does not read as a record */
		g_detail_view = (BOOL)( cmd == DM_VIEW_SET_DETAIL );
		de_view(&g_de);
		break;
	default:
		(void)de_command(&g_de, cmd);
		break;
	}
	g_dirty = TRUE;
}

/* What a chosen item does: the editor's command, or one of the system's items */
static void command( CONST T_MNSEL *sel )
{
	INT	cmd = doc_menu_cmd(sel);
	char	s[MN_CODE_MAX + 32];

	snprintf(s, sizeof(s), "command %s", (CONST char *)sel->code);
	note(s);
	if ( cmd != 0 ) doc_do(cmd);
	else if ( strcmp((CONST char *)sel->code, "close") == 0 ) close_asked();
	else if ( strcmp((CONST char *)sel->code, "trash") == 0 ) ta_trash_open();
	else if ( strcmp((CONST char *)sel->code, "tray.level") == 0 ) (void)tr_sel_dat(sel->index + 1);
	else if ( strcmp((CONST char *)sel->code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp((CONST char *)sel->code, "exec.app") == 0 && g_has_link ) ta_exec_app(&g_link, sel->index);
	g_dirty = TRUE;
	/* where the caret and the other end are after it, for the tests */
	snprintf(s, sizeof(s), "caret %d,%d other %d,%d", g_de.cpara, g_de.cpos, g_de.apara, g_de.apos);
	note(s);
}

static void menu( CONST T_OBNTM *m )
{
	T_DOCMENU	st;
	T_MNSEL		sel;
	T_DPRECT	page, box;
	ID		mid;
	INT		which = -1, ex;
	char		s[MN_CODE_MAX + 16];

	memset(&st, 0, sizeof(st));
	st.view = g_xml_view ? DM_VIEW_XML : g_detail_view ? DM_VIEW_DETAIL : DM_VIEW_CLEAN;
	st.wrap = (BOOL)!g_nowrap;
	st.show_hidden = g_show_hidden;
	st.paper_frame = g_paper_frame;
	st.is_root = (BOOL)!g_has_parent;
	st.has_pick = de_has_sel(&g_de);
	st.nrecent = de_recent_faces(st.recent, DM_RECENT_MAX);
	st.can_paste = tray_has();
	st.vertical = (BOOL)( g_doc != NULL && g_doc->vert );
	de_menu_state(&g_de, &st.comb, &st.turned);
	if ( doc_menu_make(&st, &mid) < E_OK ) return;
	ta_tray_menu(mid);
	dc_work_rect(&page);
	g_has_link = (BOOL)( g_doc != NULL
			  && tv_doc_at(g_doc, &page, g_sy, g_rec, m->x, m->y, &g_link, &box, &which) >= E_OK );
	/* away from a link, 実行 is for the link the selection is alone */
	if ( !g_has_link ) g_has_link = de_link_picked(&g_de, &g_link);
	ex = ( g_has_link && ta_exec_menu(mid, &g_link) > 0 ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "exec", (UINT)ex);
	note(( ex == 0 ) ? "exec menu on" : "exec menu grey");	/* for the tests */
	note("menu");
	if ( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		(void)mn_del_men(mid);
		snprintf(s, sizeof(s), "chosen %s", (char *)sel.code);
		note(s);
		command(&sel);
		return;
	}
	(void)mn_del_men(mid);
}

/* ---------------------------------------------------------------- what the person does */

/* A letter of the menu with Ctrl that the editor did not take: the item that has it */
static void menu_key( CONST T_OBNTM *m )
{
	static CONST struct {
		UB		letter;
		CONST char	*code;
	} keys[] = {
		{ 'E', "close" }, { 'S', "save" }, { 'L', "fullscreen" }, { 'C', "copy" }, { 'V', "paste" },
		{ 'X', "cut" }, { 'Z', "moveback" }, { 'F', "find" }, { '9', "virtualize" }, { 'P', "print" },
	};
	T_MNSEL	sel;
	UB	ch = wm_key_char(m->code, 0);
	INT	i;

	if ( ch >= 'a' && ch <= 'z' ) ch = (UB)( ch - 'a' + 'A' );
	for ( i = 0; i < (INT)( sizeof(keys) / sizeof(keys[0]) ); i++ ) {
		if ( keys[i].letter == ch ) {
			memset(&sel, 0, sizeof(sel));
			snprintf((char *)sel.code, sizeof(sel.code), "%s", keys[i].code);
			command(&sel);
			return;
		}
	}
}

static void key( CONST T_OBNTM *m )
{
	UINT	asks;

	ta_ime_mode(m->imode);
	/* a modifier key alone, but 右Ctrl (入力終), which commits what is being composed */
	if ( m->code >= 0xE0 && m->code <= 0xE7 && m->code != 0xE4 ) return;
	g_key_in = ta_now();
	asks = g_band_asks;
	if ( de_editable(&g_de) && de_key(&g_de, m->code, m->mods) ) {
		g_key_at = m->when;		/* typed into the text: said when it is shown */
		g_key_done = ta_now();
		/* the editor said what to draw again, a part or the whole; else the whole */
		if ( g_band_asks == asks ) g_dirty = TRUE;
		return;
	}
	if ( ( m->mods & M_CTRL ) != 0 ) menu_key(m);
}

/* The hand pressed: 詳細's marks, a link in the text, or the caret put there */
static void press( CONST T_OBNTM *m )
{
	static INT	px = -1000, py = -1000;
	static UD	pat = 0;
	T_DPRECT	page, box;
	T_VOBJ		v;
	INT		which = -1, near = dc_look(LK_DBL_W, 10) / 2;
	UD		gap = (UD)dc_look(WM_LOOK_DBLTIME, 400) * 1000000ULL;
	BOOL		twice;

	ta_ime_commit(&g_de);		/* a press commits what is being composed */
	if ( de_mark_press(&g_de, m->x, m->y, m->when) ) return;
	twice = (BOOL)( m->x > px - near && m->x < px + near && m->y > py - near && m->y < py + near
		     && m->when - pat < gap );
	px = m->x;
	py = m->y;
	pat = m->when;
	dc_work_rect(&page);
	if ( g_doc != NULL && tv_doc_at(g_doc, &page, g_sy, g_rec, m->x, m->y, &v, &box, &which) >= E_OK ) {
		if ( twice ) link_open(&v);
		else de_link_take(&g_de, which, &box, m->x, m->y, m->when);
		g_dirty = TRUE;
		return;
	}
	if ( de_editable(&g_de) ) {
		de_press(&g_de, m->x, m->y, (BOOL)( ( m->mods & M_SHIFT ) != 0 ), m->when);
		g_dirty = TRUE;
	}
}

static void released( INT x, INT y )
{
	if ( de_sel_carrying() ) de_sel_release(x, y);
	de_release();
	de_mark_release();
	if ( de_link_carrying() ) de_link_release(x, y);
	g_dirty = TRUE;
}

/*
 * The hand went out of the window carrying letters or a link: let go of
 * here as outside the window, where the desktop follows it on and puts
 * it where it is let go (dc_host.c, text_away and thing_away)
 */
#define DC_OUTSIDE	(-30000)

static void left( void )
{
	if ( !de_sel_carrying() && !de_link_carrying() ) return;
	note("left the window carrying");
	released(DC_OUTSIDE, DC_OUTSIDE);
}

/*
 * What was let go on the window: letters where the outline's corner was,
 * or links, put in the text there as a text of the desktop's puts them
 * (as wide and high as each was, where the outline's corner and its place
 * in it say)
 */
static void dropped( void )
{
	T_OBDROP	*d = ob_drop_read(g_ui.kw);
	T_VOBJ		v;
	INT		i, n = 0;
	ER		er = E_OK;
	char		s[64];

	if ( d == NULL ) return;
	if ( g_doc == NULL || !de_editable(&g_de) || g_xml_view ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "");
		free(d);
		return;
	}
	ta_ime_commit(&g_de);
	if ( d->n <= 0 ) {
		INT	k = (INT)strlen((char *)OB_DROP_LETTERS(d));

		er = ( k > 0 ) ? de_text_drop(&g_de, d->x, d->y, OB_DROP_LETTERS(d), k) : E_PAR;
		ui_drop_answer(&g_ui, d, ( er >= E_OK ) ? OB_DR_ACCEPT : OB_DR_REFUSE, "");
		snprintf(s, sizeof(s), "letters dropped at %d,%d (%d)", (INT)d->x, (INT)d->y, (INT)er);
		note(s);
		free(d);
		g_dirty = TRUE;
		return;
	}
	ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
	for ( i = 0; i < d->n; i++ ) {
		CONST T_OBDROPV	*o = &d->v[i];

		memset(&v, 0, sizeof(v));
		v.target = o->target;
		v.width = o->right - o->left;
		v.heightpx = o->bottom - o->top;
		v.chsz = o->chsz;
		v.frcol = o->frcol;
		v.chcol = o->chcol;
		v.tbcol = o->tbcol;
		v.bgcol = o->bgcol;
		v.disp = ( o->disp != 0 ) ? o->disp : TAD_D_DEFAULT;
		v.zoom = 100;
		if ( de_link_drop(&g_de, d->x + o->left + 2, d->y + o->top + 2, &v) >= E_OK ) n++;
	}
	snprintf(s, sizeof(s), "dropped %d", n);
	note(s);
	free(d);
	ta_watch_links();
	g_dirty = TRUE;
}

/* The pointer's moves come one after another: only where it is last is followed */
static BOOL g_move_pending;
static INT  g_move_x, g_move_y;

static void moved( INT x, INT y )
{
	if ( de_dragging() ) de_follow(x, y);
	else if ( de_link_carrying() ) de_link_follow(x, y);
	else if ( de_mark_carrying() ) de_mark_follow(x, y);
	else return;
	g_dirty = TRUE;
}

static void event( CONST T_OBNTM *m )
{
	if ( m->event == OB_E_PRESSED ) {
		blink_restart();		/* a button or a key pressed anywhere on the screen */
		return;
	}
	if ( memcmp(&m->uuid, &g_win, sizeof(TS_UUID)) != 0 ) {
		if ( ta_links_event(m) ) return;
		/* what a link points at changed: what was read of it is read again */
		if ( m->event == OB_E_CHANGE ) {
			tv_src_forget(&m->uuid);
			if ( memcmp(&m->uuid, &g_obj, sizeof(TS_UUID)) == 0 ) ta_paper_read();
			g_dirty = TRUE;
		}
		return;
	}
	if ( m->event != OB_E_MOVE && g_move_pending ) {
		g_move_pending = FALSE;
		moved(g_move_x, g_move_y);
	}
	switch ( m->event ) {
	case OB_E_CLOSE:
		close_asked();
		break;
	case OB_E_DELETE:
		g_closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( g_input_at == 0 ) g_input_at = m->when;
		blink_restart();
		if ( m->recno == OB_WR_CAND ) ta_ime_pick(&g_de, (INT)m->code);
		else if ( m->code == 1 && ( de_dragging() || de_link_carrying() || de_sel_carrying() ) ) {
			de_link_copy();		/* 複写ドラッグ: what is carried goes as a copy */
			de_sel_copy();
		} else if ( m->code == 1 ) menu(m);
		else if ( m->code == 0 ) press(m);
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m->x, m->y);
		break;
	case OB_E_MOVE:
		if ( de_dragging() || de_link_carrying() || de_mark_carrying() ) {
			g_move_pending = TRUE;
			g_move_x = m->x;
			g_move_y = m->y;
		}
		break;
	case OB_E_KEY:
		if ( ( m->mods & OB_KEY_UP ) == 0 ) {
			if ( g_input_at == 0 ) g_input_at = m->when;
			blink_restart();
			key(m);
		}
		break;
	case OB_E_WHEEL:
		wheel(m);
		break;
	case OB_E_SCROLL:
		if ( m->code == OB_BAR_R ) scroll_to(m->x);
		else if ( m->code == OB_BAR_B ) scroll_x_to(m->x);
		break;
	case OB_E_LEAVE:
		left();
		break;
	case OB_E_DROP:
		dropped();
		break;
	case OB_E_ASK:
		/* links, or letters, would be taken as dropped() takes them */
		ta_ask_answer(m, (BOOL)( g_doc != NULL && de_editable(&g_de) && !g_xml_view ));
		break;
	case OB_E_REDRAW:
		ta_work_size();
		g_dirty = TRUE;
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

static void remade( void )
{
	dc_model();
}

static CONST T_TAHOST	dc_tahost = {
	.who = "document", .prog = DC_PROG, .box = SYSDEF_BOX_DOC,
	.remade = remade, .picks_none = NULL,
};

static void editor_set( void )
{
	T_DDED	*e = &g_de;

	memset(e, 0, sizeof(*e));
	e->rec = &g_rec;
	e->doc = &g_doc;
	e->xrec = &g_xrec;
	e->scroll_x = &g_sx;
	e->scroll_y = &g_sy;
	e->height = &g_height;
	e->kind = &g_kind;
	e->xml_view = &g_xml_view;
	e->detail_view = &g_detail_view;
	e->dirty = &g_changed;
	e->sealed = &g_sealed;
	e->used = &g_used;
	e->settling = &g_settling;
	e->id = &g_obj;
	e->recno = &g_recno;
	e->framed = FALSE;
	e->host = &dc_host;
	e->arg = NULL;
}

int main( void )
{
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0;
	UD	t0 = ta_now();
	char	s[TAD_NAME_MAX + 96];
	ER	er;

	ta_stack_start();
	if ( ta_start(&dc_tahost, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("基本文章編集は、文章の実身を開いて使います");
		return 1;
	}
	editor_set();
	er = load();
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "the object cannot be read as a document (%d)", (INT)er);
		note(s);
		(void)wm_msg_put(( er == E_OBJ ) ? "文章の実身ではありません" : "実身を読めません");
		return 2;
	}
	if ( ta_channel() < E_OK ) {
		note("no channel");
		return 3;
	}
	if ( ta_win_open(OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
			 | OB_E_DELETE | OB_E_DROP | OB_E_TEXT | OB_E_ASK | OB_E_WHEEL | OB_E_SCROLL | OB_E_LEAVE
			 | OB_E_PRESSED) < E_OK ) {
		note("no window");
		ta_channel_close();
		return 3;
	}
	tv_src_host(&g_obj);
	/* a link in it to itself, opened, shows it as it is being edited, not as it was saved */
	tv_src_live(&g_obj, g_recno, &g_rec);
	ta_paper_read();
	ta_watch(&g_obj);
	ta_watch_links();
	from_link();
	paint(FALSE, NULL);
	snprintf(s, sizeof(s), "opened %s paragraphs %d links %d autoref %d parent %d up in %d us", (char *)g_name,
		 ( g_doc != NULL ) ? g_doc->npara : 0, tad_lnk_count(g_rec), (INT)( ( ref.flags & OB_F_AUTOREF ) != 0 ),
		 (INT)g_has_parent, (INT)( ( ta_now() - t0 ) / 1000 ));
	note(s);
	g_blink_at = ta_now();

	while ( !g_closing ) {
		INT	blink = dc_look(LK_BLINK, 800);
		TMO	tmo = TMO_FEVR;
		UD	now, gone;

		if ( settle_pending() ) {
			tmo = TMO_POL;
		} else if ( blink > 0 ) {
			/* until the caret's turn is due, counted from the last key or press */
			gone = ( ta_now() - g_blink_at ) / 1000000ULL;
			tmo = ( gone >= (UD)blink ) ? 1 : (TMO)( (UD)blink - gone );
		}
		er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo);
		/* everything waiting taken first, the window drawn once after */
		while ( er >= E_OK && !g_closing ) {
			event(&m);
			er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, TMO_POL);
		}
		if ( er < E_OK && er != E_TMOUT ) break;
		ta_links_told();		/* what a panel's wait took the notice of is read too */
		if ( g_move_pending ) {
			g_move_pending = FALSE;
			moved(g_move_x, g_move_y);
		}
		if ( g_closing ) break;
		if ( g_dirty ) {
			paint(FALSE, NULL);
			continue;
		}
		if ( g_banded ) {
			T_DPRECT	b = g_band;

			g_banded = FALSE;
			paint(FALSE, &b);
			continue;
		}
		now = ta_now();
		if ( blink > 0 && now - g_blink_at >= (UD)blink * 1000000ULL ) {
			g_blink_at = now;
			if ( de_editable(&g_de) && !de_dragging() ) caret_turn();
		} else if ( blink <= 0 && g_caret_hidden ) {
			g_caret_hidden = FALSE;
			paint(TRUE, NULL);
		}
		if ( settle_pending() ) settle_step();
	}
	ta_ime_end(&g_de);
	ta_end();
	dc_undo_clear();
	if ( g_doc != NULL ) tv_doc_free(g_doc);
	g_doc = NULL;
	if ( g_xrec != NULL ) tad_free(g_xrec);
	g_xrec = NULL;
	if ( g_rec != NULL ) tad_free(g_rec);
	g_rec = NULL;
	snprintf(s, sizeof(s), "stack used %d of %d bytes", ta_stack_used(), TA_STACK_SIZE);
	note(s);
	note("closed");
	ta_channel_close();
	return 0;
}
