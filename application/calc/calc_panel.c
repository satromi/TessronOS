/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_panel.c
 *	基本表計算: the 操作パネル (design 11.13)
 *
 *	The parts that work on the cells are in a small window of their own,
 *	made a subordinate window of the sheet's ("sub" in its metadata):
 *	the window manager keeps it just in front of the sheet's window,
 *	carries it when that window is carried, and raises the two together.
 *	It has no band; its frame carries it. It stands where the object
 *	says (window.panelpos against window.pos), or else over the top edge
 *	of the sheet's window, and the 表示 menu puts it away and brings it
 *	back.
 *
 *	What fills it is panel 6 of the data box 基本表計算 (SYSDEF_BOX_CALC),
 *	placed on it with ui_strip: the window manager draws and works every
 *	part, and nothing here draws. Three rows:
 *
 *	  書体▼ 文字サイズ▼ B I U S 上付 下付 文字色■ 地の色■ 横の配置▼ 縦の配置▼
 *	  格子 外枠 上 下 左 右 なし 線の色■ 線種▼ 線幅▼ 集合縦棒 積上縦棒 折れ線 積上折線
 *	  セルの名前  fx  [値と数式の欄]  ✓ ×
 *
 *	A push button does what the same item of the menu does (command());
 *	one with ▼, and a colour, opens under it the menu the sheet's own
 *	menu has for it (書体, 文字サイズ, 文字色, セルの背景色, 横の配置,
 *	縦の配置, 線種, 線幅, 線の色). The frames and the charts are buttons
 *	with pictures, PNG resources of the data box. The decorations are
 *	switches that show whether the cell in hand has them; the buttons
 *	with ▼ are written with what the cell has, and the swatches show its
 *	colours and the line the next frame is drawn with.
 *
 *	The box in the third row shows what the cell in hand holds (its
 *	formula, or its words), and while a cell is typed into, what is
 *	typed. It is a box of the panel's own: what is typed into it is
 *	converted by the system as in any panel, fx puts "=" in front of what
 *	it holds and gives the box the keys, so that what is typed next goes
 *	on after the =, and ✓ or Enter puts it in the cell (leaving the cell
 *	in hand where it is), × or Esc leaves the cell as it was.
 */

#include "calc.h"
#include <ts/wm.h>
#include <ts/hid.h>

#define PW		600			/* the work area: panel 6's size */
#define PH		84
#define PN_PANEL	6

/* The parts of panel 6 */
#define P_FACE		11
#define P_SIZE		12
#define P_BOLD		13
#define P_ITALIC	14
#define P_UNDER		15
#define P_STRIKE	16
#define P_SUPER		17
#define P_SUB		18
#define P_COLOUR	19
#define P_COLOUR_SW	20
#define P_GROUND	21
#define P_GROUND_SW	22
#define P_ALIGN		23
#define P_VALIGN	24
#define P_FRAME		31			/* 31..37: 格子 外枠 上 下 左 右 なし */
#define P_LCOLOUR	38
#define P_LCOLOUR_SW	39
#define P_LINE		40
#define P_LWIDTH	41
#define P_CHART		42			/* 42..45: 集合縦棒 積上縦棒 折れ線 積上折線 */
#define P_REF		51
#define P_FX		52
#define P_FIELD		53
#define P_OK		54
#define P_CANCEL	55

/* What a push button does: its command, or the menu it opens and the corner the menu opens at */
typedef struct {
	INT		num;
	CONST char	*code;			/* a command */
	CONST char	*menu;			/* or the menu's definition */
	INT		x, y;			/* where the menu opens, in the work area */
} PART;

static CONST PART parts[] = {
	{ P_FACE,	NULL, SYSDEF_MENU_CALC_FACE,	6,   28 },
	{ P_SIZE,	NULL, SYSDEF_MENU_CALC_SIZE,	82,  28 },
	{ P_BOLD,	"style.bold",		NULL,	0, 0 },
	{ P_ITALIC,	"style.italic",		NULL,	0, 0 },
	{ P_UNDER,	"style.underline",	NULL,	0, 0 },
	{ P_STRIKE,	"style.strike",		NULL,	0, 0 },
	{ P_SUPER,	"style.super",		NULL,	0, 0 },
	{ P_SUB,	"style.sub",		NULL,	0, 0 },
	{ P_COLOUR,	NULL, SYSDEF_MENU_CALC_COLOUR,	384, 28 },
	{ P_GROUND,	NULL, SYSDEF_MENU_CALC_GROUND,	454, 28 },
	{ P_ALIGN,	NULL, SYSDEF_MENU_CALC_ALIGN,	526, 28 },
	{ P_VALIGN,	NULL, SYSDEF_MENU_CALC_VALIGN,	564, 28 },
	{ P_FRAME,	"frame.all",		NULL,	0, 0 },
	{ P_FRAME + 1,	"frame.outline",	NULL,	0, 0 },
	{ P_FRAME + 2,	"frame.top",		NULL,	0, 0 },
	{ P_FRAME + 3,	"frame.bottom",		NULL,	0, 0 },
	{ P_FRAME + 4,	"frame.left",		NULL,	0, 0 },
	{ P_FRAME + 5,	"frame.right",		NULL,	0, 0 },
	{ P_FRAME + 6,	"frame.none",		NULL,	0, 0 },
	{ P_LCOLOUR,	NULL, SYSDEF_MENU_CALC_LCOLOUR,	204, 55 },
	{ P_LINE,	NULL, SYSDEF_MENU_CALC_LINE,	278, 55 },
	{ P_LWIDTH,	NULL, SYSDEF_MENU_CALC_LWIDTH,	340, 55 },
	{ P_CHART,	"chart.bar",		NULL,	0, 0 },
	{ P_CHART + 1,	"chart.sbar",		NULL,	0, 0 },
	{ P_CHART + 2,	"chart.line",		NULL,	0, 0 },
	{ P_CHART + 3,	"chart.sline",		NULL,	0, 0 },
};
#define NPART	( (INT)( sizeof(parts) / sizeof(parts[0]) ) )

static CONST PART *part_of( INT num )
{
	INT	k;

	for ( k = 0; k < NPART; k++ ) {
		if ( parts[k].num == num ) return &parts[k];
	}
	return NULL;
}

/* ---------------------------------------------------------------- what the parts show */

typedef struct {
	UINT		has;			/* the cell's decorations, CX_S_* */
	CONST char	*face;			/* as the button says it */
	INT		size;
	UW		colour, ground;
	char		align[16], valign[16];
} LOOK;

static CONST char *CONST face_names[3][2] = {
	{ "MS Gothic", "ゴシック" }, { "MS Mincho", "明朝" }, { "Meiryo", "メイリオ" }
};
static CONST char *CONST line_names[4] = { "実線", "破線", "点線", "二重線" };

static void look_of( LOOK *k )
{
	CONST CXCELL	*c = cx_get(&g_s, g_col, g_row);
	CONST CXSTYLE	*st = ( c != NULL ) ? c->st : NULL;
	INT		i;

	memset(k, 0, sizeof(*k));
	k->face = face_names[0][1];
	k->size = 12;
	k->colour = 0x00000000U;
	k->ground = 0x00FFFFFFU;
	snprintf(k->align, sizeof(k->align), "left");
	snprintf(k->valign, sizeof(k->valign), "middle");
	if ( st == NULL ) return;
	k->has = st->has;
	if ( st->has & CX_S_FACE ) {
		k->face = st->face;
		for ( i = 0; i < 3; i++ ) {
			if ( strcmp(st->face, face_names[i][0]) == 0 ) k->face = face_names[i][1];
		}
	}
	if ( ( st->has & CX_S_SIZE ) && st->size > 0 ) k->size = st->size;
	if ( st->has & CX_S_COLOUR ) k->colour = colour_value(st->colour, k->colour);
	if ( st->has & CX_S_BG ) k->ground = colour_value(st->bg, k->ground);
	if ( ( st->has & CX_S_ALIGN ) && st->align[0] != 0 ) snprintf(k->align, sizeof(k->align), "%s", st->align);
	if ( ( st->has & CX_S_VALIGN ) && st->valign[0] != 0 ) snprintf(k->valign, sizeof(k->valign), "%s", st->valign);
}

/* ---------------------------------------------------------------- the window */

static void shown_forget( void );

static TS_UUID	p_win;
static ID	p_kw;
static BOOL	p_open;
static UI	p_ui;				/* panel 6, placed on the window */
static BOOL	p_typed;			/* the box was typed into: what it holds is not the cell's */

BOOL panel_on( void )
{
	return p_open;
}

BOOL panel_is( CONST TS_UUID *u )
{
	return (BOOL)( p_open && ( memcmp(u, &p_win, sizeof(TS_UUID)) == 0
				   || memcmp(u, &p_ui.pnl, sizeof(TS_UUID)) == 0 ) );
}

void panel_open( void )
{
	T_OBWPOS	wp, pp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[200], par[TS_UUID_STRLEN + 1], s[80];
	INT		l, t, ow = PW + 12, oh = PH + 12;

	if ( p_open || g_kw <= 0 ) return;
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	memset(&pp, 0, sizeof(pp));

	/* where the object says, against the window; else over the window's top edge */
	l = wp.left + ( g_panel_said ? g_panel_dx : 0 );
	t = wp.top + ( g_panel_said ? g_panel_dy : -( oh + 2 ) );
	if ( wp.sw > 0 && l + ow > wp.sw ) l = wp.sw - ow;
	if ( wp.sh > 0 && t + oh > wp.sh ) t = wp.sh - oh;
	if ( l < 0 ) l = 0;
	if ( t < 0 ) t = 0;
	ts_uuid_text(&g_win, par);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"sub\":\"%s\"}",
		 l, t, l + ow, t + oh, WM_ATTR_FRAME, par);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"操作パネル";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &p_win) < E_OK ) {
		note("the panel cannot be opened");
		return;
	}
	p_kw = ob_opn_obj(&p_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_CLOSE | OB_E_DELETE;
	if ( p_kw <= 0 || ob_ntf_evt(p_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( p_kw > 0 ) ob_cls_obj(p_kw);
		(void)ob_del_obj(&p_win);
		p_kw = 0;
		note("the panel cannot be opened");
		return;
	}
	/* the work area as large as the panel, whatever the frame takes */
	if ( ob_rea_rec(p_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz) >= E_OK ) {
		pp.right = pp.left + PW + ( ( pp.right - pp.left ) - ( pp.wright - pp.wleft ) );
		pp.bottom = pp.top + PH + ( ( pp.bottom - pp.top ) - ( pp.wbottom - pp.wtop ) );
		(void)ob_wri_rec(p_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz);
		(void)ob_rea_rec(p_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz);
	}
	if ( ui_strip(&p_ui, SYSDEF_BOX_CALC, PN_PANEL, p_kw, g_port, 0, 0) < E_OK ) {
		ob_cls_obj(p_kw);
		(void)ob_del_obj(&p_win);
		p_kw = 0;
		note("the panel cannot be opened");
		return;
	}
	p_open = TRUE;
	p_typed = FALSE;
	shown_forget();
	panel_draw();
	snprintf(s, sizeof(s), "panel at %d %d %d %d", pp.left, pp.top, pp.right, pp.bottom);
	note(s);
}

void panel_close( void )
{
	if ( !p_open ) return;
	p_open = FALSE;
	ui_strip_close(&p_ui);
	if ( p_kw > 0 ) ob_cls_obj(p_kw);
	(void)ob_del_obj(&p_win);
	p_kw = 0;
}

/* ---------------------------------------------------------------- what is shown */

/* What the cell in hand holds, as the box shows it when nothing is typed */
static void held( char *s, INT size )
{
	CONST CXCELL	*c = cx_get(&g_s, g_col, g_row);

	if ( c == NULL ) s[0] = 0;
	else if ( c->kind == CX_LINK ) snprintf(s, (size_t)size, "[仮身]");
	else if ( c->kind == CX_IMAGE ) snprintf(s, (size_t)size, "[画像]");
	else snprintf(s, (size_t)size, "%s", (CONST char *)c->text);
}

/* A part's words changed only when they differ, so that the panel is not drawn again for nothing */
static void words( INT num, CONST char *s, char *was, INT max )
{
	if ( strncmp(was, s, (size_t)max - 1) == 0 ) return;
	snprintf(was, (size_t)max, "%s", s);
	ui_set_text(&p_ui, num, s);
}

/* A swatch's colour, the same way */
static void colour( INT num, UW c, UW *was )
{
	c &= 0x00FFFFFFU;
	if ( *was == c ) return;
	*was = c;
	ui_set_value(&p_ui, num, (INT)c);
}

/* What the panel last showed, forgotten: shown anew when it opens */
static char	w_face[40], w_size[16], w_al[16], w_va[16], w_line[16], w_lw[16], w_ref[24], w_box[512];
static UW	w_col = ~0U, w_gnd = ~0U, w_lcol = ~0U;
static INT	w_has = -1;

static void shown_forget( void )
{
	w_face[0] = w_size[0] = w_al[0] = w_va[0] = w_line[0] = w_lw[0] = w_ref[0] = 0;
	w_box[0] = 1;
	w_box[1] = 0;
	w_col = w_gnd = w_lcol = ~0U;
	w_has = -1;
}

static CONST char *placing( CONST char *how, BOOL across )
{
	if ( across ) return ( how[0] == 'c' ) ? "中 ▼" : ( how[0] == 'r' ) ? "右 ▼" : "左 ▼";
	return ( how[0] == 't' ) ? "上 ▼" : ( how[0] == 'b' ) ? "下 ▼" : "中 ▼";
}

void panel_draw( void )
{
	static CONST UINT has[6] = { CX_S_BOLD, CX_S_ITALIC, CX_S_UNDER, CX_S_STRIKE, CX_S_SUPER, CX_S_SUB };
	LOOK		k;
	CONST CXSIDE	*ln = line_side();
	char		s[512], nm[24];
	INT		i, v;

	if ( !p_open ) return;
	look_of(&k);
	snprintf(s, sizeof(s), "%s▼", k.face);
	words(P_FACE, s, w_face, sizeof(w_face));
	snprintf(s, sizeof(s), "%d ▼", k.size);
	words(P_SIZE, s, w_size, sizeof(w_size));
	for ( i = 0, v = 0; i < 6; i++ ) {
		if ( k.has & has[i] ) v |= 1 << i;
	}
	for ( i = 0; i < 6 && v != w_has; i++ ) {
		ui_set_value(&p_ui, P_BOLD + i, ( v >> i ) & 1);
	}
	w_has = v;
	colour(P_COLOUR_SW, k.colour, &w_col);
	colour(P_GROUND_SW, k.ground, &w_gnd);
	colour(P_LCOLOUR_SW, colour_value(ln->colour, 0x00000000U), &w_lcol);
	words(P_ALIGN, placing(k.align, TRUE), w_al, sizeof(w_al));
	words(P_VALIGN, placing(k.valign, FALSE), w_va, sizeof(w_va));
	snprintf(s, sizeof(s), "%s ▼", ( ln->type >= 0 && ln->type < 4 ) ? line_names[ln->type] : line_names[0]);
	words(P_LINE, s, w_line, sizeof(w_line));
	snprintf(s, sizeof(s), "%s ▼", ( ln->width == 1 ) ? "細" : ( ln->width == 2 ) ? "中" : ( ln->width == 3 ) ? "太" : "細");
	words(P_LWIDTH, s, w_lw, sizeof(w_lw));
	(void)cx_cell_name(g_col, g_row, nm);
	words(P_REF, nm, w_ref, sizeof(w_ref));

	/* the box: what is typed in the cell, or what the cell holds; left alone while it is typed into */
	if ( !p_typed ) {
		if ( ed_on() ) (void)ed_text((UB *)s, sizeof(s));
		else held(s, sizeof(s));
		words(P_FIELD, s, w_box, sizeof(w_box));
	}
}

/* ---------------------------------------------------------------- what the parts do */

/* The menu a ▼ part or a colour opens under it, what is in use marked */
static void drop( CONST PART *p )
{
	TS_UUID		def;
	T_MNSEL		sel;
	LOOK		lk;
	CONST CXSIDE	*ln = line_side();
	ID		mid;
	char		code[MN_CODE_MAX];
	BOOL		chosen;

	if ( ui_uuid(p->menu, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	look_of(&lk);
	code[0] = 0;
	switch ( p->num ) {
	case P_FACE:
		snprintf(code, sizeof(code), "font.%s", ( strcmp(lk.face, "明朝") == 0 ) ? "mincho"
			 : ( strcmp(lk.face, "メイリオ") == 0 ) ? "meiryo" : "gothic");
		break;
	case P_ALIGN:	snprintf(code, sizeof(code), "align.%s", lk.align);	break;
	case P_VALIGN:	snprintf(code, sizeof(code), "valign.%s", lk.valign);	break;
	case P_LINE:
		{
			static CONST char *CONST type[4] = { "solid", "dashed", "dotted", "double" };

			snprintf(code, sizeof(code), "line.%s", type[( ln->type >= 0 && ln->type < 4 ) ? ln->type : 0]);
		}
		break;
	default:
		break;
	}
	if ( code[0] != 0 ) (void)mn_chg_atr(mid, NULL, code, MN_TICK);
	chosen = (BOOL)( mn_pop_men(mid, p_kw, p->x, p->y, ui_now(), &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		char	s[80];

		snprintf(s, sizeof(s), "panel %s %d", (CONST char *)sel.code, (INT)sel.value);
		note(s);
		command((CONST char *)sel.code, sel.index, sel.value);
	}
}

/* Whether the cell in hand can be typed into: not a link or an image */
static BOOL typable( void )
{
	CONST CXCELL	*c = cx_get(&g_s, g_col, g_row);

	if ( c != NULL && c->kind != CX_TEXT ) {
		say("仮身や画像のセルには入力できません");
		return FALSE;
	}
	return TRUE;
}

/* What the box holds put in the cell in hand, which stays in hand: ✓ and Enter */
static void field_put( void )
{
	char	s[512];

	if ( !p_typed ) {
		if ( ed_on() ) ed_commit();
		return;
	}
	(void)ui_text(&p_ui, P_FIELD, s, sizeof(s));
	if ( ed_on() ) ed_cancel();
	if ( typable() ) cell_text(g_col, g_row, (CONST UB *)s);
	p_typed = FALSE;
	note("panel ok");
}

/* What the box holds given up: × and Esc */
static void field_cancel( void )
{
	if ( ed_on() ) ed_cancel();
	p_typed = FALSE;
	note("panel cancel");
}

/* A part of the panel worked, by its number */
static void work( INT num )
{
	CONST PART	*p = part_of(num);
	char		s[512];

	switch ( num ) {
	case P_FX:
		if ( !typable() ) break;
		if ( ed_on() ) (void)ed_text((UB *)s + 1, sizeof(s) - 1);
		else held(s + 1, sizeof(s) - 1);
		s[0] = '=';
		ui_set_text(&p_ui, P_FIELD, ( s[1] == '=' ) ? s + 1 : s);
		ui_focus(&p_ui, P_FIELD);	/* what is typed next goes on after the = */
		p_typed = TRUE;
		note("panel fx");
		break;
	case P_FIELD:
		if ( p_typed || !typable() ) break;
		p_typed = TRUE;
		note("panel field");
		break;
	case P_OK:
		field_put();
		break;
	case P_CANCEL:
		field_cancel();
		break;
	default:
		if ( p == NULL ) break;
		ed_commit();
		if ( p->menu != NULL ) {
			if ( p_kw > 0 ) drop(p);
		} else {
			snprintf(s, sizeof(s), "panel %s", p->code);
			note(s);
			command(p->code, 0, 0);
		}
		break;
	}
	g_dirty = TRUE;
}

BOOL panel_event( CONST T_OBNTM *m )
{
	INT	num, ev;

	if ( memcmp(&m->uuid, &p_ui.pnl, sizeof(TS_UUID)) == 0 ) {
		ev = ui_event(&p_ui, m, &num);
		if ( ev == UI_EV_BUTTON || ev == UI_EV_CHANGE ) work(num);
		return TRUE;
	}
	switch ( m->event ) {
	case OB_E_KEY:
		/*
		 * The keys of the panel's window are its box's, which has them
		 * while the window does: Enter puts what it holds in the cell
		 * (what is being converted taken in first, by the box), Esc
		 * gives it up. A key with Ctrl is the sheet's, as the menu's
		 * keys are; the rest do not go on to the sheet.
		 */
		if ( ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) return FALSE;
		if ( ( m->mods & OB_KEY_UP ) != 0 ) break;
		if ( m->code == 0x28 || m->code == 0x58 ) field_put();
		else if ( m->code == 0x29 ) field_cancel();
		g_dirty = TRUE;
		break;
	case OB_E_REDRAW:
		panel_draw();
		break;
	case OB_E_CLOSE:
		ed_commit();
		panel_close();
		note("panel off");
		g_dirty = TRUE;
		break;
	case OB_E_DELETE:
		p_open = FALSE;
		p_kw = 0;
		break;
	default:
		break;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the program's own test */

static INT	t_pass, t_all;

static void t_check( BOOL ok, CONST char *what )
{
	char	s[120];

	t_all++;
	if ( ok ) {
		t_pass++;
	} else {
		snprintf(s, sizeof(s), "selftest panel: %s", what);
		note(s);
	}
}

/*
 * Every button of the panel does something; and the buttons worked on
 * a sheet of their own: the decorations, the frames, and what the parts
 * show of the cell in hand.
 */
INT panel_test( INT *p_all )
{
	LOOK		lk;
	CONST CXCELL	*c;
	INT		k;

	t_pass = t_all = 0;
	for ( k = 0; k < NPART; k++ ) {
		t_check((BOOL)( ( parts[k].code != NULL ) != ( parts[k].menu != NULL ) ), "a part does nothing");
		t_check((BOOL)( parts[k].menu == NULL || ( parts[k].x < PW && parts[k].y < PH ) ), "a menu off the panel");
	}

	cx_init(&g_s);
	g_col = g_acol = 1;
	g_row = g_arow = 1;
	cell_text(1, 1, (CONST UB *)"5");
	work(P_BOLD);
	work(P_SUPER);
	work(P_SUB);
	look_of(&lk);
	t_check((BOOL)( ( lk.has & CX_S_BOLD ) != 0 ), "bold");
	t_check((BOOL)( ( lk.has & ( CX_S_SUPER | CX_S_SUB ) ) == CX_S_SUB ), "上付き and 下付き both");
	command("valign.top", 0, 0);
	command("align.right", 0, 0);
	look_of(&lk);
	t_check((BOOL)( strcmp(lk.valign, "top") == 0 && strcmp(lk.align, "right") == 0 ), "the placing");
	t_check((BOOL)( strcmp(placing(lk.align, TRUE), "右 ▼") == 0 && strcmp(placing(lk.valign, FALSE), "上 ▼") == 0 ),
		"the placing's words");
	command("linewidth", 0, 3);
	command("line.dotted", 0, 0);
	work(P_FRAME + 2);
	c = cx_get(&g_s, 1, 1);
	t_check((BOOL)( c != NULL && c->st->side[CX_TOP].on && c->st->side[CX_TOP].width == 3
			&& c->st->side[CX_TOP].type == CX_DOTTED && !c->st->side[CX_BOTTOM].on ), "the top frame");
	command("line.solid", 0, 0);
	command("linewidth", 0, 1);
	ed_end();
	cx_free_all(&g_s);
	g_col = g_acol = 1;
	*p_all = t_all;
	return t_pass;
}
