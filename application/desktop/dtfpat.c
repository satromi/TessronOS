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
 *	パターン編集: a figure's pattern, dot by dot (design 17.13), in a
 *	window of the desktop's like any other (DTOWN), its tools a panel
 *	of parts. The dots, what they are drawn as and how they go into the
 *	record are the figure editor's (application/fig/figpat.c), shared
 *	with the figure editor running as a process.
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
#include <ts/figedit.h>
#include <ts/figtool.h>
#include "desktop.h"

#define PX		FP_PX		/* the panel: down the right side, under the pattern at its own size */
#define PY		FP_PY
#define PNW		FP_PNW
#define PNH		FP_PNH
#define WW		FP_WW		/* the work area */
#define WH		FP_WH

LOCAL T_FPED	pe;			/* what is being edited */

/* ---------------------------------------------------------------- the window */

/*
 * A window of the desktop's like any other (DTOWN): its frame moves it
 * and closes it, the second button opens its menu (閉じる, 読込, 入替,
 * 復旧 and the tail every window has). The dots, large, and the pattern
 * at its own size are what it draws; the colours, the tools and the
 * buttons are a panel of parts down its right side.
 */
LOCAL CONST char *CONST pe_tools[4] = { "鉛筆", "長方形", "スポイト", "展開" };
LOCAL CONST char *CONST pe_moves[4] = { "左", "上", "下", "右" };
LOCAL CONST char *CONST pe_turns[4] = { "右回", "左回", "左右", "上下" };

LOCAL DTWIN	*pe_win = NULL;		/* the window, NULL when it is not open */
LOCAL INT	pe_fig = 0;		/* the window of the figure it works on */
LOCAL INT	pe_pid = -1;		/* its panel */

LOCAL CONST DTOWN	pe_own;

/* The figure's window, while it is still there */
LOCAL DTWIN *pe_figure( void )
{
	DTWIN	*d = ( pe_fig > 0 ) ? dt_win_of(pe_fig) : NULL;

	return ( d != NULL && d->fig != NULL ) ? d : NULL;
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
	for ( k = 0; k < FP_NCOL; k++ ) {
		pt = dt_pn_add(def, WM_PT_SWATCH, FP_N_SW + k, ( k % 8 ) * 22 - DT_PN_IN, 20 + ( k / 8 ) * 22, 20, 20, NULL);
		if ( pt != NULL ) {
			pt->colour = fp_cols[k];
			pt->group = 1;
			pt->value = ( pe.col == fp_cols[k] ) ? 1 : 0;
		}
	}
	act_button(def, FP_N_NONE, -DT_PN_IN, 64, 56, "透明");
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 100, PNW, 18, "ツール:");
	for ( k = 0; k < 4; k++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, FP_N_TOOL + k, ( k % 2 ) * 90 - DT_PN_IN, 126 + ( k / 2 ) * 24, 88, 22,
			       pe_tools[k]);
		if ( pt != NULL ) {
			pt->group = 2;
			pt->value = ( pe.tool == k ) ? 1 : 0;
		}
	}
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 182, PNW, 18, "移動:");
	for ( k = 0; k < 4; k++ ) {
		act_button(def, FP_N_MOVE + k, k * 44 - DT_PN_IN, 202, 42, pe_moves[k]);
	}
	(void)dt_pn_add(def, WM_PT_LABEL, 0, -DT_PN_IN, 232, PNW, 18, "変形:");
	for ( k = 0; k < 4; k++ ) {
		act_button(def, FP_N_TURN + k, k * 44 - DT_PN_IN, 252, 42, pe_turns[k]);
	}
	pt = dt_pn_add(def, WM_PT_CHECK, FP_N_LOAD, -DT_PN_IN, 286, 58, 22, "読込");
	if ( pt != NULL ) {
		pt->value = pe.choosing ? 1 : 0;
	}
	act_button(def, FP_N_SWAP, 60 - DT_PN_IN, 286, 56, "入替");
	act_button(def, FP_N_UNDO, 120 - DT_PN_IN, 286, 56, "復旧");
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
	INT		gid = wm_gid(d->wid);
	T_DPRECT	c;

	if ( gid < 0 ) {
		return;
	}
	dt_own_ground(d);
	fp_draw(&pe, gid, ( f != NULL ) ? f->fig : NULL);
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

	if ( num == FP_N_SWAP ) {
		if ( f != NULL ) {
			T_FGED	e;

			df_ed_of(f, &e);
			if ( fp_replace(&pe, &e) ) {
				df_ed_back(&e);
				dt_draw(f);
				tm_printf((UB *)"pattern: %d put in the figure\n", pe.id);
			}
		}
	} else {
		fp_act(&pe, num);
		if ( num == FP_N_NONE ) {
			INT	k;

			/* no colour marked, so that what the panel holds does not take the colour back */
			for ( k = 0; k < FP_NCOL; k++ ) {
				(void)wm_panel_set(pe_pid, FP_N_SW + k, 0);
			}
			tm_printf((UB *)"pattern: colour none\n");
		}
	}
	(void)d;
}

/* What the panel's choices hold now, taken into the editor */
LOCAL void pe_follow( DTWIN *d )
{
	INT	k, v;
	BOOL	choosing;

	for ( k = 0; k < FP_NCOL; k++ ) {
		if ( wm_panel_get(pe_pid, FP_N_SW + k, &v) >= E_OK && v != 0 && pe.col != fp_cols[k] ) {
			pe.col = fp_cols[k];
			tm_printf((UB *)"pattern: colour %06x\n", (UINT)pe.col);
		}
	}
	for ( k = 0; k < 4; k++ ) {
		if ( wm_panel_get(pe_pid, FP_N_TOOL + k, &v) >= E_OK && v != 0 && pe.tool != k ) {
			pe.tool = k;
			tm_printf((UB *)"pattern: tool %d\n", k);
		}
	}
	choosing = (BOOL)( wm_panel_get(pe_pid, FP_N_LOAD, &v) >= E_OK && v != 0 );
	if ( choosing != pe.choosing ) {
		pe.choosing = choosing;
		tm_printf((UB *)"pattern: choosing %s\n", choosing ? "on" : "off");
	}
	(void)d;
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
		pe.down = FALSE;
	}
	if ( ev->type == HID_EV_MOVE && pe.down ) {
		fp_drag(&pe, ev->x, ev->y);
		pe_draw(d);
		return TRUE;
	}
	if ( pe_pid >= 0 && wm_panel_event(pe_pid, ev, &ans) < E_OK ) {
		ans = WM_ANS_NONE;
	}
	if ( ev->type == HID_EV_BTN_DOWN && ( ev->x < PX || ev->y < PY ) ) {
		DTWIN	*f = pe_figure();

		if ( fp_press(&pe, ( f != NULL ) ? f->fig : NULL, ev->x, ev->y ) ) {
			(void)wm_panel_set(pe_pid, FP_N_LOAD, 0);
			tm_printf((UB *)"pattern: %d read\n", pe.id);
		}
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
	(void)mn_chg_atr(mid, NULL, "pat.load", pe.choosing ? MN_TICK : 0);
	(void)d;
}

LOCAL void pe_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	if ( mn_is(sel, "pat.load") ) {
		pe.choosing = !pe.choosing;
		(void)wm_panel_set(pe_pid, FP_N_LOAD, pe.choosing ? 1 : 0);
	} else if ( mn_is(sel, "pat.swap") ) {
		pe_act(d, FP_N_SWAP);
	} else if ( mn_is(sel, "pat.undo") ) {
		pe_act(d, FP_N_UNDO);
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
	pe.down = FALSE;
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
	pe_fig = d->wid;
	{
		UW	col = pe.col;
		INT	tool = pe.tool;

		/* the colour and the tool stay as they were last left */
		fp_new(&pe);
		pe.col = col;
		pe.tool = tool;
	}
	pe.id = ( pat < 2 ) ? 2 : pat;
	fp_load(&pe, d->fig, pe.id);
	fp_keep(&pe);
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
	tm_printf((UB *)"pattern: opened %d\n", pe.id);
	dt_draw(e);
}
