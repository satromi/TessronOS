/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_pat.c
 *	パターン編集 of the figure editor running as a process (design 17.13)
 *
 *	A window of its own, a subordinate window of the figure's, its work
 *	area the size the desktop's パターン編集 has: the program draws the
 *	dots, the number and the pattern at its own size in it, and the
 *	panel of parts down its right side is panel 7 of the data box,
 *	the parts where the desktop's stand. Its menu is the desktop's
 *	(閉じる, 読込, 入替, 復旧 and the tail every window has). What it
 *	edits and draws, and how 入替 puts the pattern into the record, are
 *	the figure editor's (figpat.c), as the desktop's are.
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/look.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/figtool.h>

#define PN_PAT		7		/* パターン編集 of the data box */
#define PN_OUT		8		/* it stands this far out on the left and the right of FP_PX..FP_PNW:
					   the outline of a group of choices is drawn outside the parts */
#define SL		( FP_PX - PN_OUT )
#define SR		( FP_PX + FP_PNW + PN_OUT )

static T_FPED	pe;			/* what is being edited; the colour and the tool stay */
static TS_UUID	pe_win;
static ID	pe_kw = 0;
static INT	pe_gid = -1;
static UI	pe_ui;			/* its panel */
static BOOL	pe_strip = FALSE;

static void say_pe( CONST char *s )
{
	char	line[96];

	snprintf(line, sizeof(line), "pattern: %s", s);
	note(line);
}

/* The parts as the editor stands: the colour drawn in marked, the tool, 読込, 入替 */
static void parts_show( void )
{
	INT	k;

	if ( !pe_strip ) return;
	for ( k = 0; k < FP_NCOL; k++ ) {
		ui_set_swatch(&pe_ui, FP_N_SW + k, fp_cols[k], (BOOL)( pe.col == fp_cols[k] ));
	}
	for ( k = 0; k < 4; k++ ) {
		ui_set_value(&pe_ui, FP_N_TOOL + k, ( pe.tool == k ) ? 1 : 0);
	}
	ui_set_value(&pe_ui, FP_N_LOAD, pe.choosing ? 1 : 0);
}

static void pe_draw( void )
{
	T_DPRECT	r;

	if ( pe_kw <= 0 || pe_gid < 0 ) return;
	r.left = r.top = 0;
	r.right = FP_WW;
	r.bottom = FP_WH;
	(void)dp_set_visible(pe_gid, &r);
	/*
	 * the ground, all but the panel, which the system draws: in the
	 * tone or the stripes ユーザ環境設定 gives the panels' ground, laid
	 * from the window's corner as the desktop lays it
	 */
	r.bottom = FP_PY;
	(void)wm_fill_look(pe_gid, &r, LK_PNLGROUND);
	r.top = FP_PY;  r.bottom = FP_WH;  r.right = SL;
	(void)wm_fill_look(pe_gid, &r, LK_PNLGROUND);
	r.left = SR;  r.right = FP_WW;
	(void)wm_fill_look(pe_gid, &r, LK_PNLGROUND);
	r.left = SL;  r.right = SR;  r.top = FP_PY + FP_PNH;
	(void)wm_fill_look(pe_gid, &r, LK_PNLGROUND);
	fp_draw(&pe, pe_gid, g_fig);
	(void)wm_obj_flush(pe_kw, NULL);
}

static void pe_close( void )
{
	if ( pe_kw <= 0 ) return;
	if ( pe_strip ) ui_strip_close(&pe_ui);
	pe_strip = FALSE;
	ob_cls_obj(pe_kw);
	(void)ob_del_obj(&pe_win);
	pe_kw = 0;
	pe_gid = -1;
	pe.down = FALSE;
	say_pe("closed");
}

void fpe_close( void )
{
	pe_close();
}

BOOL fpe_shown( void )
{
	return (BOOL)( pe_kw > 0 );
}

/*
 * パターン編集 opened on pattern pat of the figure, 40 below and to the
 * right of the figure's window, a subordinate window of it; one already
 * open is brought to the front
 */
void fpe_open( INT pat )
{
	T_OBWPOS	wp, pp;
	T_OBCRE		c;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[200], sub[TS_UUID_STRLEN + 1], s[32];
	INT		l, t;

	if ( g_fig == NULL ) return;
	if ( pe_kw > 0 ) {
		if ( ob_rea_rec(pe_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz) >= E_OK ) {
			pp.z = 0;
			(void)ob_wri_rec(pe_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz);
		}
		return;
	}
	{
		UW	col = pe.col;
		INT	tool = pe.tool;

		fp_new(&pe);
		pe.col = col;
		pe.tool = tool;
	}
	pe.id = ( pat < 2 ) ? 2 : pat;
	fp_load(&pe, g_fig, pe.id);
	fp_keep(&pe);
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	l = wp.left + 40;
	t = wp.top + 40;
	ts_uuid_text(&g_win, sub);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"sub\":\"%s\"}",
		 l, t, l + FP_WW + 16, t + FP_WH + 40, WM_ATTR_FRAME | WM_ATTR_TITLE, sub);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"パターン編集";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &pe_win) < E_OK ) {
		say_pe("cannot be opened");
		return;
	}
	pe_kw = ob_opn_obj(&pe_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	pe_gid = ( pe_kw > 0 ) ? wm_obj_gid(pe_kw) : -1;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_CLOSE | OB_E_DELETE | OB_E_REDRAW;
	if ( pe_kw <= 0 || pe_gid < 0 || ob_ntf_evt(pe_kw, OB_REC_ANY, &req, g_ui.port) <= 0 ) {
		pe_close();
		return;
	}
	/* the frame measured: the work area made the size it is drawn at */
	if ( ob_rea_rec(pe_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz) >= E_OK ) {
		pp.right = pp.left + FP_WW + ( ( pp.right - pp.left ) - ( pp.wright - pp.wleft ) );
		pp.bottom = pp.top + FP_WH + ( ( pp.bottom - pp.top ) - ( pp.wbottom - pp.wtop ) );
		(void)ob_wri_rec(pe_kw, OB_WR_PLACE, 0, &pp, sizeof(pp), &asz);
	}
	pe_strip = (BOOL)( ui_strip(&pe_ui, SYSDEF_BOX_FIG, PN_PAT, pe_kw, g_ui.port, SL, FP_PY) >= E_OK );
	/* 入替 as the figure is when the window opens: the button stays so while it is open */
	if ( pe_strip ) ui_off(&pe_ui, FP_N_SWAP, (BOOL)( g_fig == NULL || !g_ed.usable ));
	parts_show();
	pe_draw();
	snprintf(s, sizeof(s), "opened %d", pe.id);
	say_pe(s);
}

/* 入替: the pattern put into the figure under its number */
static void swap( void )
{
	char	s[40];

	if ( g_fig == NULL ) return;		/* the figure's window has closed */
	fg_sync();
	if ( !fp_replace(&pe, &g_ed) ) return;
	g_npick = g_ed.npick;
	fg_sync();
	g_dirty = TRUE;
	snprintf(s, sizeof(s), "%d put in the figure", pe.id);
	say_pe(s);
}

/* What a part of the panel told: the colour, the tool, 読込, or a button */
static void part_told( CONST T_OBNTM *m )
{
	INT	ev, num;
	char	s[40];

	ev = ui_event(&pe_ui, m, &num);
	if ( ev == UI_EV_BUTTON ) {
		if ( num == FP_N_SWAP ) {
			swap();
		} else if ( num == FP_N_NONE || ( num >= FP_N_MOVE && num < FP_N_TURN + 4 ) || num == FP_N_UNDO ) {
			fp_act(&pe, num);
			if ( num == FP_N_NONE ) {
				say_pe("colour none");
				parts_show();	/* no colour marked: 透明 is drawn in */
			}
		}
	} else if ( ev == UI_EV_CHANGE ) {
		if ( num >= FP_N_SW && num < FP_N_SW + FP_NCOL && ui_value(&pe_ui, num) != 0
		  && pe.col != fp_cols[num - FP_N_SW] ) {
			pe.col = fp_cols[num - FP_N_SW];
			snprintf(s, sizeof(s), "colour %06x", (UINT)pe.col);
			say_pe(s);
		} else if ( num >= FP_N_TOOL && num < FP_N_TOOL + 4 && ui_value(&pe_ui, num) != 0
			 && pe.tool != num - FP_N_TOOL ) {
			pe.tool = num - FP_N_TOOL;
			snprintf(s, sizeof(s), "tool %d", pe.tool);
			say_pe(s);
		} else if ( num == FP_N_LOAD ) {
			BOOL	on = (BOOL)( ui_value(&pe_ui, num) != 0 );

			if ( on != pe.choosing ) {
				pe.choosing = on;
				say_pe(on ? "choosing on" : "choosing off");
			}
		}
	} else {
		return;
	}
	pe_draw();
}

/* The window's menu, at the place of the press */
static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;
	char	s[MN_CODE_MAX + 16];

	if ( ui_uuid(SYSDEF_MENU_FPAT, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "pat.swap", ( g_fig != NULL && g_ed.usable ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "pat.load", pe.choosing ? MN_TICK : 0);
	chosen = (BOOL)( mn_pop_men(mid, pe_kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	snprintf(s, sizeof(s), "chosen %s", (char *)sel.code);
	say_pe(s);
	if ( strcmp((CONST char *)sel.code, "close") == 0 ) {
		pe_close();
		return;
	} else if ( strcmp((CONST char *)sel.code, "pat.load") == 0 ) {
		pe.choosing = (BOOL)!pe.choosing;
		parts_show();
	} else if ( strcmp((CONST char *)sel.code, "pat.swap") == 0 ) {
		swap();
	} else if ( strcmp((CONST char *)sel.code, "pat.undo") == 0 ) {
		fp_act(&pe, FP_N_UNDO);
	}
	pe_draw();
}

/* A notice of パターン編集's window or panel: TRUE when it was one */
BOOL fpe_event( CONST T_OBNTM *m )
{
	if ( pe_kw <= 0 ) return FALSE;
	if ( pe_strip && memcmp(&m->uuid, &pe_ui.pnl, sizeof(TS_UUID)) == 0 ) {
		part_told(m);
		return TRUE;
	}
	if ( memcmp(&m->uuid, &pe_win, sizeof(TS_UUID)) != 0 ) return FALSE;
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		pe_close();
		break;
	case OB_E_REDRAW:
		pe_draw();
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) {
			menu(m);
		} else if ( m->code == 0 && ( m->x < SL || m->y < FP_PY ) ) {
			if ( fp_press(&pe, g_fig, m->x, m->y) ) {
				char	s[24];

				parts_show();
				snprintf(s, sizeof(s), "%d read", pe.id);
				say_pe(s);
			}
			pe_draw();
		}
		break;
	case OB_E_MOVE:
		if ( pe.down ) {
			fp_drag(&pe, m->x, m->y);
			pe_draw();
		}
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 && pe.down ) {
			pe.down = FALSE;
			pe_draw();
		}
		break;
	case OB_E_KEY:
		/* the letter of the menu's 閉じる */
		if ( ( m->mods & OB_KEY_UP ) == 0 && ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0
		  && ( wm_key_char(m->code, 0) == 'e' || wm_key_char(m->code, 0) == 'E' ) ) {
			pe_close();
		}
		break;
	default:
		break;
	}
	return TRUE;
}
