/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_main.c
 *	基本図形編集 as a program running as a process (design 11.3)
 *
 *	The program is started on a figure's object and shows it in an
 *	ordinary window, with a frame, a title, its size changed by its
 *	corner and scroll bars down the right and along the foot. The
 *	figure is drawn by the same code that draws it in the desktop,
 *	linked into the program (include/ts/tvport.h): the drawing calls are
 *	the kernel's, done on this window's environment, and what the links
 *	in it show is asked of the kernel's store (lib/libtv/tv_src.c).
 *
 *	What is done to the figure (fg_edit.c, application/fig/figedit.c) is
 *	done to the record held in the program's memory, and the record is
 *	written back to the object when it is saved: by 元の実身に保存
 *	(Ctrl+S), and when the window is closed. A window that could not be
 *	saved is not closed. What any editor running as a process does with
 *	its object, its window and the desktop -- the counts of what the
 *	record links to, the links watched, 新たな実身に保存, the view
 *	written back into the link it was opened from, where the window
 *	stood -- is lib/libtv/tv_app.c's.
 *
 *	The second button opens the figure editor's menu (SYSDEF_MENU_FIG).
 *
 *	What it does is said on the console for the tests ("figure: ...").
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/look.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/obatr.h>
#include <ts/obdrop.h>
#include <ts/json.h>
#include <ts/uiprn.h>
#include <ts/tvsrc.h>
#include <ts/tadlnk.h>
#include <ts/time.h>
#include <ts/docmenu.h>
#include <ts/figtool.h>
#include <ts/proc.h>

/* The program object of 基本図形編集: the icon of its windows */
#define FG_PROG		"01a0d6d1-08d2-7344-a6e2-6cede408d684"
/* and of 仮身一覧, when the program is the cabinet */
#define CB_PROG		"01a0d6d1-08d3-72ef-ad1e-323b44cc2172"

/* 基本図形編集 unless the program is linked with a cabinet's file (application/cab, basebox, accbox, basecol), which says TRUE */
__attribute__((weak)) BOOL	fg_cabinet = FALSE;
/* and what the 原紙箱, the 小物箱 and 原紙集め add, which their files give */
__attribute__((weak)) CONST T_FGBOX	*fg_box = NULL;
UINT		fg_as = 0;

#define TICK_MS		20
#define WHEEL_STEP	48		/* a notch of the wheel */

#define K_ENTER		0x28
#define K_ESC		0x29
#define K_DEL		0x4C
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- what it holds */

T_TVFIG		*g_fig;
UINT		*g_pick;
INT		g_maxpick, g_npick;
UINT		g_pick_seq;
T_FGED		g_ed;

INT		g_sx, g_sy;
INT		g_fw, g_fh;
INT		g_zoom = 8;
BOOL		g_show_hidden;

/*
 * The first window (最初のウインドウ): the window of the cabinet the system
 * opens with (SYSDEF_CABINET), which the desktop starts as it starts.
 * Closing it ends the session, asked of the desktop (DT_RQ_FIRST_CLOSE);
 * a link to what the system makes is put in it when the desktop says
 * (DT_HOW_LINK_PUT); deleting the link of a disk from it takes the disk
 * away (DT_DISK_REMOVE).
 */
static BOOL	g_first = FALSE;

/* the link under the pointer when the menu was opened, for 実行 and the link's items */
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

/* ---------------------------------------------------------------- the record */

/* The figure read from the object: E_OBJ when it is not a figure */
static ER load( void )
{
	SZ	len = 0;
	UB	*x = NULL;
	T_TAD	*rec = NULL;
	ER	er;

	if ( FG_MADE ) {
		/* the 小物箱, 原紙集め: the list the program makes, not the object's record */
		er = fg_box->load(&rec);
	} else {
		if ( ta_read(&x, &len) < E_OK ) return E_NOEXS;
		er = tad_parse(x, len, NULL, &rec);
		free(x);
	}
	if ( er < E_OK ) return er;
	if ( tv_kind(rec) != TV_KIND_FIG ) {
		tad_free(rec);
		return E_OBJ;
	}
	if ( g_rec != NULL ) tad_free(g_rec);
	g_rec = rec;
	ta_was();
	fg_model();
	return E_OK;
}

/* The record read again as it is stored, nothing taken, scrolled as it was */
static void reread( void )
{
	INT	sx = g_sx, sy = g_sy;
	char	s[48];
	ER	er;

	fg_pick_none();
	er = load();
	fg_scroll_to(sx, sy);
	ta_watch_links();
	snprintf(s, sizeof(s), "read again (%d)", (INT)er);
	note(s);
}

/* ---------------------------------------------------------------- the window */

/* 印刷: the figure's record handed to the desktop as a page, on the paper the window shows */
static void print( void )
{
	/* hidden links printed when the window shows them, as the desktop's figure editor prints */
	(void)ta_print(g_rec, g_show_hidden ? DT_F_PR_HIDDEN : 0);
}

/* Closing: what was changed saved first; a window that could not be saved stays */
static void close_asked( void )
{
	if ( fg_holding() ) fg_hold_cancel();
	if ( g_first ) {
		/* the first window: closed when the session ends, and asked whether it is to end */
		T_DTREQ	rq;
		T_DTANS	an;
		ER	er;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_FIRST_CLOSE;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, TMO_FEVR);
		if ( er != E_OK ) {
			note("first window stays");
			return;
		}
		note("first window closes: the session ends");
	} else if ( !FG_MADE && ta_disk_gone(TRUE) ) {
		g_closing = TRUE;		/* the disk of the object went: what changed was handed over */
		return;
	}
	/* the list of the 小物箱 or of 原紙集め: nothing of it is the object's to keep */
	if ( g_changed && !FG_MADE && ta_save() < E_OK ) {
		tell("保存できなかったため、ウインドウを閉じませんでした", "メモリやディスクの空きを確かめてから、もう一度閉じてください");
		return;
	}
	/* a cabinet's link keeps no view of its own and no zoom, as the desktop's cabinet writes it */
	if ( fg_cabinet ) ta_link_view_back(g_sx, g_sy, 0, TAD_WRAP_NONE, NULL);
	else ta_link_view_back(g_sx, g_sy, g_zoom * 100 / 8, TAD_WRAP_NONE, "canvas");
	/*
	 * where the window stood, into the object it shows; 原紙集め shows
	 * the program object, the system's to change and the person's only
	 * to read, so its window opens where the program says each time; so
	 * does a tool's
	 */
	if ( !FG_BASECOL && !FG_TOOL ) ta_window_keep();
	g_closing = TRUE;
}

/*
 * 自動起動: the links of the cabinet that ask to be opened with it, each
 * opened as a double press opens it, by no act of the person, in the
 * order of the links. The first window says when they are all asked:
 * the system's start is over once what they opened is up.
 */
static void auto_open( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	T_VOBJ	v;
	INT	i, n, k = 0;
	char	s[48];

	if ( !fg_cabinet || FG_MADE || g_rec == NULL ) return;
	n = tad_lnk_count(g_rec);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(g_rec, i, &v) >= E_OK && v.autoopen && ( !v.hidden || g_show_hidden ) ) {
			ta_link_open(&v, DT_F_AUTO, i + 1);
			k++;
		}
	}
	if ( g_first ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_OPEN;
		rq.flags = DT_F_AUTO;
		rq.grab_y = k;
		memset(&an, 0, sizeof(an));
		(void)ask_desktop(&rq, &an, 3000);
	}
	snprintf(s, sizeof(s), "opened by itself %d", k);
	note(s);
}

/*
 * The first window told by the desktop: a link to what the system made
 * (a file printed, a screen saved, a disk attached) put under what is
 * there, closed and as wide as its name, and the record saved
 */
static void first_told( CONST T_DTANS *an )
{
	T_VOBJ	v;
	TS_UUID	id;
	INT	i, bottom = 8;
	char	s[TAD_NAME_MAX + 40];

	if ( an->how != DT_HOW_LINK_PUT || g_rec == NULL ) return;
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_fig->sh[i].kind != TV_SH_GROUP && g_fig->sh[i].r.bottom + 8 > bottom ) {
			bottom = g_fig->sh[i].r.bottom + 8;
		}
	}
	memset(&v, 0, sizeof(v));
	v.target = an->into;
	v.recno = -1;
	snprintf((char *)v.name, sizeof(v.name), "%s", (CONST char *)an->text);
	v.chsz = 14;
	v.frcol = 0x000000;
	v.chcol = 0x000000;
	v.tbcol = 0xFFFFFF;
	v.bgcol = 0xFFFFFF;
	v.disp = TAD_D_DEFAULT;
	v.zoom = 100;
	/* 200 by 24, as the desktop has always put what it made there */
	v.left = 16;
	v.top = bottom;
	v.right = v.left + 200;
	v.height = 24;
	v.bottom = v.top + v.height;
	if ( fg_holding() ) fg_hold_cancel();
	fg_before();
	fg_pick_none();
	if ( tad_lnk_add(g_rec, &v, &id) >= E_OK ) {
		tv_src_forget(&an->into);
		ta_watch(&an->into);
	}
	fg_after();
	(void)ta_save();
	snprintf(s, sizeof(s), "put on the desktop: %s", (CONST char *)v.name);
	note(s);
}

/* A disk's link deleted from the first window takes its disk away; FALSE: it cannot now, and stays */
static BOOL first_link_going( CONST T_VOBJ *v )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_DISK;
	rq.recno = DT_DISK_REMOVE;
	rq.target = v->target;
	ts_uuid_text(&v->tvol, (char *)rq.text);
	memset(&an, 0, sizeof(an));
	return (BOOL)( ask_desktop(&rq, &an, TMO_FEVR) >= E_OK );
}

/* An item of ディスク操作: the program ディスク操作 started with it, on the link the menu was opened on */
static void disk_item( CONST char *code )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_DISK;
	rq.recno = DT_DISK_MENU;
	snprintf((char *)rq.name, sizeof(rq.name), "%s", code);
	if ( g_has_link ) rq.target = g_link.target;
	memset(&an, 0, sizeof(an));
	(void)ask_desktop(&rq, &an, 5000);
}

/* ---------------------------------------------------------------- the menu */

/* The items that act on what is taken, grey when nothing is */
static CONST char *CONST on_pick[] = {
	"copy", "cut", "delete", "ungroup", "front", "back", "forward", "backward",
	"transform", "invert", "burn", "flip.h", "flip.v", "rot.l", "rot.r"
};

/* and of a cabinet's menu; 保護 in both is by what it can change of the links taken (fe_prot_grey) */
static CONST char *CONST cab_on_pick[] = {
	"copy", "cut", "delete", "front", "back", "arrange"
};


/*
 * What changes the record, which the 小物箱 does not do: its page is the
 * desktop's list, sealed as the desktop's own lists are
 */
static BOOL sealed_cmd( CONST char *code )
{
	static CONST char *CONST changes[] = {
		"save", "save.new", "undo", "cut", "paste", "moveback", "delete", "front", "back",
		"arrange", "fix", "unfix", "bg", "unbg"
	};
	INT	i;

	for ( i = 0; i < (INT)( sizeof(changes) / sizeof(changes[0]) ); i++ ) {
		if ( strcmp(code, changes[i]) == 0 ) return TRUE;
	}
	return FALSE;
}

/* The links taken, each opened */
static void open_picked( void )
{
	T_VOBJ	v;
	INT	i;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK ) fg_open(&v);
	}
}

/*
 * 整頓: panel 1 of the cabinet's data box (SYSDEF_BOX_CAB), its parts
 * where the desktop's cabinet puts them. Each group of choices is the
 * tens of its parts' numbers, its last choice none.
 */
#define CB_PN_ARRANGE	1
#define CB_P_AR_COUNT	1		/* 段組数 */
#define CB_P_AR_PICKED	2		/* 選択: N個 */
#define CB_P_AR_H	10		/* 左揃え 右揃え なし */
#define CB_P_AR_V	20		/* 詰める 上揃え なし */
#define CB_P_AR_COL	30		/* 1段 左右 上下 なし */
#define CB_P_AR_W	40		/* 最初 全項目 名前まで 続柄まで 日付除く なし */
#define CB_P_AR_SORT	50		/* 名前 作成日 更新日 サイズ なし */
#define CB_P_AR_ORDER	60		/* 昇順 降順 */

/* Which of a group was chosen: its place in the group */
static INT ar_chosen( UI *q, INT group, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( ui_value(q, group + i) != 0 ) return i;
	}
	return n - 1;
}

static void arrange( void )
{
	static CONST INT	hmap[] = { FE_AR_LEFT, FE_AR_RIGHT, FE_AR_NONE };
	static CONST INT	vmap[] = { FE_AR_COMPACT, FE_AR_ALIGN, FE_AR_NONE };
	static CONST INT	cmap[] = { FE_AR_SINGLE, FE_AR_MULTI_H, FE_AR_MULTI_V, FE_AR_NONE };
	static CONST INT	wmap[] = { FE_AR_W_FIRST, FE_AR_W_FULL, FE_AR_W_NAME, FE_AR_W_RELATION,
					   FE_AR_W_NODATE, FE_AR_NONE };
	static CONST INT	smap[] = { FE_AR_BY_NAME, FE_AR_BY_MADE, FE_AR_BY_UPDATED, FE_AR_BY_SIZE, FE_AR_NONE };
	static CONST struct { INT group, n; } grey[] = {
		{ CB_P_AR_H, 3 }, { CB_P_AR_V, 3 }, { CB_P_AR_COL, 4 }, { CB_P_AR_SORT, 5 }
	};
	UI		box = g_ui, q;
	T_OBNTM		m;
	T_FEARRANGE	o;
	INT		num, ev, done = 0, i, k, cols;
	char		t[64];

	if ( g_npick <= 0 || ui_uuid(SYSDEF_BOX_CAB, &box.box) < E_OK || ui_dlg_open(&box, CB_PN_ARRANGE, &q) < E_OK ) return;
	snprintf(t, sizeof(t), "選択: %d個", g_npick);
	ui_set_text(&q, CB_P_AR_PICKED, t);
	if ( g_npick < 2 ) {
		/* one link: only 幅調整 means anything, and none of the others but なし */
		for ( i = 0; i < (INT)( sizeof(grey) / sizeof(grey[0]) ); i++ ) {
			for ( k = 0; k < grey[i].n - 1; k++ ) ui_off(&q, grey[i].group + k, TRUE);
		}
		ui_off(&q, CB_P_AR_COUNT, TRUE);
		ui_off(&q, CB_P_AR_ORDER, TRUE);
		ui_off(&q, CB_P_AR_ORDER + 1, TRUE);
	}
	note("arrange panel");
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		if ( ev == UI_EV_BUTTON ) done = ( num == P_OK ) ? 1 : -1;
		else if ( ev == UI_EV_CLOSE ) done = -1;
		else if ( ev == UI_EV_NONE && m.event == OB_E_KEY && ( m.code == 0x28 || m.code == 0x58 ) ) done = 1;
	}
	if ( done > 0 ) {
		o.horizontal = hmap[ar_chosen(&q, CB_P_AR_H, 3)];
		o.vertical = vmap[ar_chosen(&q, CB_P_AR_V, 3)];
		o.column = cmap[ar_chosen(&q, CB_P_AR_COL, 4)];
		o.width = wmap[ar_chosen(&q, CB_P_AR_W, 6)];
		o.sort_by = smap[ar_chosen(&q, CB_P_AR_SORT, 5)];
		o.descending = (BOOL)( ar_chosen(&q, CB_P_AR_ORDER, 2) == 1 );
		cols = ui_value(&q, CB_P_AR_COUNT);
		o.columns = ( cols > 0 ) ? cols : 1;
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
	if ( done <= 0 ) {
		note("arrange put away");
		return;
	}
	snprintf(t, sizeof(t), "arrange %d,%d,%d,%d,%d,%d,%d", o.horizontal, o.vertical, o.column, o.columns,
		 o.width, o.sort_by, o.descending ? 1 : 0);
	note(t);
	fg_arrange(&o);
}

/* One call of the figure editor's code, the picks it counted taken back after */
#define FE_DO(call)	do { fg_sync(); call; g_npick = g_ed.npick; fg_sync(); } while ( 0 )

/* Of the links taken, the one taken first, in *v; FALSE when no link is taken */
static BOOL picked_link( T_VOBJ *v )
{
	INT	i, best = -1;

	for ( i = 0; g_fig != NULL && g_npick > 0 && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && ( best < 0 || g_pick[i] < g_pick[best] ) ) best = i;
	}
	return (BOOL)( best >= 0 && tad_lnk_get(g_rec, g_fig->sh[best].link, v) >= E_OK );
}

static void command( CONST char *code, INT index )
{
	char	s[MN_CODE_MAX + 32];

	snprintf(s, sizeof(s), "command %s", code);
	note(s);
	if ( !fg_cabinet && fgt_command(code) ) ;
	else if ( fg_box != NULL && fg_box->command != NULL
		  && fg_box->command(code, g_has_link ? &g_link : NULL) ) ;	/* 小物管理, 原紙集め's 再表示 */
	else if ( strcmp(code, "close") == 0 ) close_asked();
	else if ( FG_MADE && sealed_cmd(code) ) ;	/* nothing in a list made is put, taken or saved */
	else if ( strcmp(code, "save") == 0 ) (void)ta_save();
	else if ( strcmp(code, "save.new") == 0 ) ta_save_new();
	else if ( ( strncmp(code, "vobj.", 5) == 0 || strncmp(code, "real.", 5) == 0 ) && g_has_link ) {
		fg_link_op(code, &g_link);
		ta_watch_links();
	}
	else if ( strcmp(code, "fullscreen") == 0 ) ta_fullscreen();
	else if ( strcmp(code, "panel") == 0 ) fgt_toggle();
	else if ( strcmp(code, "trash") == 0 ) ta_trash_open();
	else if ( strncmp(code, "disk.", 5) == 0 ) disk_item(code);
	else if ( strcmp(code, "pattern") == 0 ) fpe_open(( ft_look.fill_pat > 0 ) ? ft_look.fill_pat : 2);
	else if ( strcmp(code, "refresh") == 0 ) {
		tv_src_forget(NULL);
		ta_paper_refresh();
	} else if ( strcmp(code, "bgcolour") == 0 ) ta_paper_ask();
	else if ( strcmp(code, "hidden") == 0 ) {
		g_show_hidden = (BOOL)!g_show_hidden;
		fg_model();
	} else if ( strcmp(code, "undo") == 0 ) {
		fg_text_end();		/* the record put back holds the text's element no more */
		(void)fg_undo();
	} else if ( fg_text_on() && strcmp(code, "copy") == 0 ) (void)fg_text_command(DM_COPY);
	else if ( fg_text_on() && strcmp(code, "cut") == 0 ) (void)fg_text_command(DM_CUT);
	else if ( fg_text_on() && strcmp(code, "paste") == 0 ) (void)fg_text_command(DM_PASTE);
	else if ( fg_text_on() && strcmp(code, "moveback") == 0 ) (void)fg_text_command(DM_MOVE_BACK);
	else if ( fg_text_on() && strcmp(code, "selectall") == 0 ) (void)fg_text_command(DM_SELECT_ALL);
	else if ( strcmp(code, "copy") == 0 ) (void)fg_copy();
	else if ( strcmp(code, "cut") == 0 ) fg_cut();
	else if ( strcmp(code, "paste") == 0 ) fg_paste(FALSE);
	else if ( strcmp(code, "moveback") == 0 ) fg_paste(TRUE);
	else if ( strcmp(code, "delete") == 0 ) fg_delete();
	else if ( strcmp(code, "tray.level") == 0 ) (void)tr_sel_dat(index + 1);
	else if ( strcmp(code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp(code, "group") == 0 ) FE_DO(fe_group(&g_ed));
	else if ( strcmp(code, "ungroup") == 0 ) FE_DO(fe_ungroup(&g_ed));
	else if ( strcmp(code, "selectall") == 0 ) fg_pick_all();
	else if ( fg_cabinet && strcmp(code, "front") == 0 ) fg_link_order(TRUE);
	else if ( fg_cabinet && strcmp(code, "back") == 0 ) fg_link_order(FALSE);
	else if ( fg_cabinet && !FG_TEMPLATES && strcmp(code, "arrange") == 0 ) arrange();
	else if ( strcmp(code, "front") == 0 ) FE_DO(fe_order(&g_ed, FE_TO_FRONT));
	else if ( strcmp(code, "back") == 0 ) FE_DO(fe_order(&g_ed, FE_TO_BACK));
	else if ( strcmp(code, "forward") == 0 ) FE_DO(fe_order(&g_ed, FE_TO_FORWARD));
	else if ( strcmp(code, "backward") == 0 ) FE_DO(fe_order(&g_ed, FE_TO_BACKWARD));
	else if ( strcmp(code, "align") == 0 ) FE_DO(fe_align(&g_ed));
	else if ( strcmp(code, "invert") == 0 ) FE_DO(fe_invert(&g_ed));
	else if ( strcmp(code, "flip.h") == 0 ) FE_DO(fe_turn(&g_ed, FE_FLIP_H));
	else if ( strcmp(code, "flip.v") == 0 ) FE_DO(fe_turn(&g_ed, FE_FLIP_V));
	else if ( strcmp(code, "rot.l") == 0 ) FE_DO(fe_turn(&g_ed, FE_ROT_L));
	else if ( strcmp(code, "rot.r") == 0 ) FE_DO(fe_turn(&g_ed, FE_ROT_R));
	else if ( strcmp(code, "fix") == 0 ) fg_protect(FALSE, TRUE);
	else if ( strcmp(code, "unfix") == 0 ) fg_protect(FALSE, FALSE);
	else if ( strcmp(code, "bg") == 0 ) fg_protect(TRUE, TRUE);
	else if ( strcmp(code, "unbg") == 0 ) fg_protect(TRUE, FALSE);
	else if ( strcmp(code, "print") == 0 ) print();
	else if ( strcmp(code, "open") == 0 ) open_picked();
	else if ( strcmp(code, "exec.app") == 0 && g_has_link ) ta_exec_app(&g_link, index);
	g_dirty = TRUE;
	/* how many shapes there are and are picked after it, for the tests */
	if ( g_fig != NULL ) {
		snprintf(s, sizeof(s), "shapes %d picked %d", g_fig->nsh, g_npick);
		note(s);
	}
}

static void menu( CONST T_OBNTM *m )
{
	TS_UUID		def;
	T_MNSEL		sel;
	T_DPRECT	page;
	ID		mid;
	BOOL		chosen, none;
	INT		i, which = -1;
	char		s[MN_CODE_MAX + 16];

	/*
	 * A press on a link that is not taken takes it first, so that what
	 * the menu does is done to the link it was opened on; away from a
	 * link, the menu is about the link taken
	 */
	page.left = page.top = 0;
	page.right = g_w;
	page.bottom = g_h;
	g_has_link = (BOOL)( g_fig != NULL
			  && tv_fig_at(g_fig, &page, g_sx, g_sy, g_rec, m->x, m->y, &g_link, NULL, &which) >= E_OK );
	if ( g_has_link && which >= 0 && g_pick[which] == 0 ) {
		fg_pick_none();
		fg_pick_add(which);
		FE_DO(fe_whole_groups(&g_ed));
		fg_paint(TRUE);
	} else if ( !g_has_link ) {
		g_has_link = picked_link(&g_link);
	}
	none = (BOOL)( g_npick == 0 );
	if ( fg_box != NULL && fg_box->menu_def != NULL && fg_box->menu_def[0] == 0 ) return;	/* a tool with no menu */
	if ( ui_uuid(( fg_box != NULL && fg_box->menu_def != NULL ) ? fg_box->menu_def
		     : FG_BASECOL ? SYSDEF_MENU_BASECOL : FG_ACCBOX ? SYSDEF_MENU_ACCBOX
		     : fg_cabinet ? SYSDEF_MENU_CAB : SYSDEF_MENU_FIG, &def) < E_OK
	  || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( fg_cabinet ) {
		/* what the desktop's cabinet greys (cabmenu.c): its undo is never grey */
		for ( i = 0; i < (INT)( sizeof(cab_on_pick) / sizeof(cab_on_pick[0]) ); i++ ) {
			(void)mn_chg_atr(mid, NULL, cab_on_pick[i], none ? MN_GREY : 0);
		}
		/* the templates of a 原紙箱 stay where they were laid out: nothing for 整頓 to do */
		if ( FG_TEMPLATES ) (void)mn_chg_atr(mid, NULL, "arrange", MN_GREY);
		note(( FG_TEMPLATES || none ) ? "arrange menu grey" : "arrange menu on");	/* for the tests */
	} else {
		for ( i = 0; i < (INT)( sizeof(on_pick) / sizeof(on_pick[0]) ); i++ ) {
			(void)mn_chg_atr(mid, NULL, on_pick[i], none ? MN_GREY : 0);
		}
		(void)mn_chg_atr(mid, NULL, "undo", ( fg_undos() == 0 ) ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "group", ( g_npick < 2 ) ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "align", ( g_npick < 2 ) ? MN_GREY : 0);
	}
	/* nothing of a list made can be protected, as nothing in it is changed */
	i = FG_MADE ? 0 : (INT)fe_prot_can(g_rec, g_fig, g_pick);
	fe_prot_grey(mid, (UINT)i);
	snprintf(s, sizeof(s), "protect menu %x", (UINT)i);	/* for the tests */
	note(s);
	(void)mn_chg_atr(mid, NULL, "save.new", ( g_has_parent && !FG_MADE ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "hidden", g_show_hidden ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "fullscreen", ta_full_shown() ? MN_TICK : 0);
	i = ( !FG_MADE && ( fg_cabinet ? fg_tray_links() > 0 : fg_tray_has() ) ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "paste", (UINT)i);
	(void)mn_chg_atr(mid, NULL, "moveback", (UINT)i);
	ta_tray_menu(mid);
	i = ( g_has_link && ta_exec_menu(mid, &g_link) > 0 ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "exec", (UINT)i);
	note(( i == 0 ) ? "exec menu on" : "exec menu grey");	/* for the tests */
	fg_link_menu(mid, g_has_link ? &g_link : NULL);
	if ( fg_box != NULL && fg_box->menu != NULL ) fg_box->menu(mid, g_has_link ? &g_link : NULL);
	note("menu");
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		snprintf(s, sizeof(s), "chosen %s", (char *)sel.code);
		note(s);
		command((CONST char *)sel.code, sel.index);
	}
}

/* ---------------------------------------------------------------- what the person does */

static void key( CONST T_OBNTM *m )
{
	if ( ( m->mods & M_CTRL ) == 0 ) {
		if ( m->code == K_DEL ) command("delete", 0);
		else if ( m->code == K_ESC && fg_holding() ) fg_hold_cancel();
		return;
	}
	/* the letters of the menu's items, and the keys that are in no menu */
	switch ( wm_key_char(m->code, 0) ) {
	case 'e': case 'E':	command("close", 0);		break;
	case 's': case 'S':	command("save", 0);		break;
	case 'l': case 'L':	command("fullscreen", 0);	break;
	case 'c': case 'C':	command("copy", 0);		break;
	case 'v': case 'V':	command("paste", 0);		break;
	case 'x': case 'X':	command("cut", 0);		break;
	case 'z': case 'Z':	command("moveback", 0);		break;
	case 'a': case 'A':	command("selectall", 0);	break;
	case 'f': case 'F':	if ( g_npick > 0 ) command("front", 0);	break;
	case 'r': case 'R':	if ( g_npick > 0 ) command("back", 0);	break;
	case 'p': case 'P':	if ( !fg_cabinet ) command("print", 0);	break;	/* a cabinet's menu has no 印刷 */
	case 'd': case 'D':	if ( fg_cabinet && !FG_TEMPLATES && g_npick > 0 ) command("arrange", 0);	break;
	case 'o': case 'O':	command("open", 0);		break;
	default: {
		/* a tool's own letters (仮身ネットワーク's Ctrl+N, Ctrl+G) */
		CONST char	*code = ( fg_box != NULL && fg_box->key != NULL )
				  ? fg_box->key((INT)wm_key_char(m->code, 0)) : NULL;

		if ( code != NULL ) command(code, 0);
		break;
	}
	}
}

static void wheel( CONST T_OBNTM *m )
{
	if ( m->dz == 0 ) return;
	if ( m->code == HID_WHEEL_H ) fg_scroll_to(g_sx + m->dz * WHEEL_STEP, g_sy);
	else if ( ( m->mods & M_SHIFT ) != 0 ) fg_scroll_to(g_sx - m->dz * WHEEL_STEP, g_sy);
	else fg_scroll_to(g_sx, g_sy - m->dz * WHEEL_STEP);
}

static void dropped( void )
{
	T_OBDROP	*d = ob_drop_read(g_ui.kw);	/* links, or letters (n 0) */

	if ( d == NULL ) return;
	if ( g_fig == NULL || FG_MADE ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "");
		free(d);
		return;
	}
	if ( d->n <= 0 ) {
		/* letters: into a piece of text where the outline's corner was */
		INT	n = (INT)strlen((char *)OB_DROP_LETTERS(d));

		if ( n == 0 || !g_ed.usable ) {
			ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "");
		} else {
			char	s[48];
			ER	er;

			ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
			er = fgt_text_drop(d->x, d->y, OB_DROP_LETTERS(d), n);
			g_dirty = TRUE;
			snprintf(s, sizeof(s), "letters dropped at %d,%d (%d)", d->x, d->y, (INT)er);
			note(s);
		}
		free(d);
		return;
	}
	ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
	fg_dropped(d);
	free(d);
	ta_watch_links();
}

/* When the frames of what is taken last turned, or a key or a press showed them */
static UD g_blink_at;

/*
 * The pointer's moves come one after another, each told on the channel,
 * faster than a shape drawn or a frame carried can follow them all. Of
 * the moves waiting together only where the pointer is last is
 * followed: once all that waits is taken, or before the next notice
 * that is not a move (a let go, a key) is acted on.
 */
static BOOL g_move_pending;
static INT  g_move_x, g_move_y;
static UINT g_move_mods;
static UD   g_move_when;

/*
 * What one drag cost, from its first move with something held to the
 * let go, said then as the desktop says its own (the tests compare the
 * two): the moves told, how many of them waited together at most, the
 * moves followed, how long one took to follow and show, from the move
 * to the end of that, and how much of the window was put on the screen.
 */
UD		g_shown_px;
static struct {
	BOOL	on;
	INT	got, followed, batch, batch_max;
	UD	busy, busy_max, lag, lag_max, lag_last, px0;
} g_drag;

static void drag_told( void )
{
	if ( !g_drag.on ) {
		memset(&g_drag, 0, sizeof(g_drag));
		g_drag.on = TRUE;
		g_drag.px0 = g_shown_px;
	}
	g_drag.got++;
	if ( ++g_drag.batch > g_drag.batch_max ) g_drag.batch_max = g_drag.batch;
}

static void drag_followed( UD t0, UD when )
{
	UD	t1 = ta_now(), b = t1 - t0, l = ( when != 0 && t1 > when ) ? t1 - when : b;

	if ( !g_drag.on ) return;
	g_drag.followed++;
	g_drag.busy += b;
	g_drag.lag += l;
	g_drag.lag_last = l;
	if ( b > g_drag.busy_max ) g_drag.busy_max = b;
	if ( l > g_drag.lag_max ) g_drag.lag_max = l;
}

static void drag_said( void )
{
	char	s[160];
	INT	n = g_drag.followed;

	if ( !g_drag.on ) return;
	g_drag.on = FALSE;
	if ( n <= 0 ) return;
	snprintf(s, sizeof(s), "drag moves %d waited %d followed %d follow avg %d max %d us lag avg %d max %d last %d us"
		 " shown %d kpx", g_drag.got, g_drag.batch_max, n, (INT)( g_drag.busy / (UD)n / 1000 ),
		 (INT)( g_drag.busy_max / 1000 ), (INT)( g_drag.lag / (UD)n / 1000 ), (INT)( g_drag.lag_max / 1000 ),
		 (INT)( g_drag.lag_last / 1000 ), (INT)( ( g_shown_px - g_drag.px0 ) / 1000 ));
	note(s);
}

/* The held thing follows the pointer to where it was last told, timed */
static void held_moved( void )
{
	UD	t0 = ta_now();

	g_move_pending = FALSE;
	if ( fgt_drawing() ) fgt_moved(g_move_x, g_move_y, g_move_mods);
	else if ( fg_holding() ) fg_moved(g_move_x, g_move_y);
	drag_followed(t0, g_move_when);
}

static void event( CONST T_OBNTM *m )
{
	if ( m->event == OB_E_PRESSED ) {
		/*
		 * a button or a key pressed anywhere on the screen: the frames
		 * shown at once and their turn counted from it, as the
		 * desktop counts its own windows' at every press
		 */
		fg_blink_show();
		g_blink_at = ta_now();
		if ( fg_damaged() ) (void)fg_paint_part(TRUE);
		return;
	}
	if ( fpe_event(m) ) return;		/* パターン編集's */
	if ( fgt_event(m) ) return;		/* the 道具パネル's */
	if ( memcmp(&m->uuid, &g_win, sizeof(TS_UUID)) != 0 ) {
		if ( ta_links_event(m) ) return;
		/* what a link points at changed: what was read of it is read again */
		if ( m->event == OB_E_CHANGE ) {
			tv_src_forget(&m->uuid);
			(void)ta_paper_changed(&m->uuid);
			/*
			 * the 原紙箱 written by another (a package installed or
			 * deleted puts or takes its templates' links): shown as it
			 * is now, unless the person has changed it here
			 */
			if ( FG_BASEBOX && memcmp(&m->uuid, &g_obj, sizeof(TS_UUID)) == 0 && !g_changed && !fg_holding() ) {
				reread();
			}
			/* the program box of 原紙集め: a program installed or deleted, the list made again */
			if ( fg_box != NULL && fg_box->changed != NULL ) (void)fg_box->changed(&m->uuid);
			g_dirty = TRUE;
		}
		return;
	}
	if ( m->event != OB_E_MOVE && g_move_pending ) held_moved();
	switch ( m->event ) {
	case OB_E_CLOSE:
		close_asked();
		break;
	case OB_E_DELETE:
		g_closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( g_input_at == 0 ) g_input_at = m->when;
		fg_blink_show();
		g_blink_at = ta_now();
		if ( m->recno == OB_WR_CAND ) fg_text_pick((INT)m->code);
		else if ( m->code == 1 && fg_text_holding() ) fg_text_second_down();
		else if ( m->code == 1 && fg_holding() ) fg_second_down();
		else if ( m->code == 1 ) menu(m);
		else if ( m->code == 0 && !fg_text_press(m) && !fgt_press(m) ) {
			fg_press(m);
			fgt_picked();
		}
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) drag_said();
		if ( m->code == 0 && fgt_drawing() ) fgt_released();
		else if ( m->code == 0 && fg_text_holding() ) fg_text_released(m->x, m->y);
		else if ( m->code == 0 ) fg_released(m->x, m->y);
		break;
	case OB_E_MOVE:
		if ( fgt_drawing() || ( !fg_text_holding() && fg_holding() ) ) {
			drag_told();
			g_move_pending = TRUE;
			g_move_x = m->x;
			g_move_y = m->y;
			g_move_mods = m->mods;
			g_move_when = m->when;
		} else if ( fg_text_holding() ) {
			fg_text_moved(m->x, m->y);
		} else {
			fgt_hover(m->x, m->y);
		}
		break;
	case OB_E_LEAVE:
		if ( fg_text_holding() ) fg_text_left();
		else if ( fg_holding() ) fg_left();
		break;
	case OB_E_KEY:
		if ( g_input_at == 0 ) g_input_at = m->when;
		if ( ( m->mods & OB_KEY_UP ) == 0 ) {
			fg_blink_show();
			g_blink_at = ta_now();
		}
		if ( ( m->mods & OB_KEY_UP ) == 0 && !fgt_key(m) && !fg_text_key(m) ) key(m);
		break;
	case OB_E_WHEEL:
		wheel(m);
		break;
	case OB_E_SCROLL:
		if ( m->code == OB_BAR_R ) fg_scroll_to(g_sx, m->x);
		else if ( m->code == OB_BAR_B ) fg_scroll_to(m->x, g_sy);
		break;
	case OB_E_REDRAW:
		ta_work_size();
		if ( fg_box != NULL && fg_box->shown != NULL ) fg_box->shown();
		g_dirty = TRUE;
		break;
	case OB_E_DROP:
		dropped();
		break;
	case OB_E_ASK:
		/* links, or letters, would be taken as dropped() takes them */
		ta_ask_answer(m, (BOOL)( g_fig != NULL && !FG_MADE && ( ( m->mods & OB_ASK_LETTERS ) == 0 || g_ed.usable ) ));
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

/* What the shared part asks of the program (tvapp.h) */
static void remade( void )
{
	fg_model();
}

static CONST T_TAHOST	fg_host = {
	.who = "figure", .prog = FG_PROG, .box = SYSDEF_BOX_FIG,
	.remade = remade, .picks_none = fg_pick_none,
};

/* 仮身一覧's: its panels are those of 基本図形編集's data box, which say what could not be done */
static CONST T_TAHOST	cb_host = {
	.who = "cabinet", .prog = CB_PROG, .box = SYSDEF_BOX_FIG,
	.remade = remade, .picks_none = fg_pick_none, .told = first_told,
};

int main( void )
{
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0;
	UD	t0 = ta_now(), t1, t2, t3, t4, t5;
	char	s[TAD_NAME_MAX + 160];
	CONST char	*cab_name = ( fg_box != NULL && fg_box->name != NULL ) ? fg_box->name : "仮身一覧";
	ER	er;

	er = ( fg_box != NULL && fg_box->start != NULL ) ? fg_box->start(&ref)
	     : ta_start(fg_cabinet ? &cb_host : &fg_host, &ref);
	if ( er < E_OK ) {
		note("no object to open");
		if ( fg_cabinet ) {
			snprintf(s, sizeof(s), "%sは、実身を開いて使います", cab_name);
			(void)wm_msg_put(s);
		} else {
			(void)wm_msg_put("基本図形編集は、図形の実身を開いて使います");
		}
		return 1;
	}
	if ( fg_box != NULL && fg_box->begin != NULL ) fg_box->begin();
	{
		TS_UUID	cab;

		g_first = (BOOL)( fg_cabinet && fg_box == NULL && ts_str_to_uuid(SYSDEF_CABINET, &cab) >= E_OK
				  && memcmp(&cab, &g_obj, sizeof(cab)) == 0 );
		if ( g_first ) fg_link_going = first_link_going;
	}
	t1 = ta_now();
	er = load();
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "the object cannot be read as a figure (%d)", (INT)er);
		note(s);
		if ( er == E_OBJ && fg_cabinet ) {
			snprintf(s, sizeof(s), "%sで開けない実身です", cab_name);
			(void)wm_msg_put(s);
		} else {
			(void)wm_msg_put(( er == E_OBJ ) ? "図形の実身ではありません" : "実身を読めません");
		}
		return 2;
	}
	t2 = ta_now();
	if ( ta_channel() < E_OK ) {
		note("no channel");
		return 3;
	}
	if ( fg_box != NULL && fg_box->listen != NULL ) fg_box->listen();
	if ( ta_win_open(OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
			 | OB_E_DELETE | OB_E_DROP | OB_E_TEXT | OB_E_ASK | OB_E_WHEEL | OB_E_SCROLL | OB_E_LEAVE
			 | OB_E_PRESSED) < E_OK ) {	/* every press: the frames' turn counted from it */
		note("no window");
		ta_channel_close();
		return 3;
	}
	if ( fg_box != NULL && fg_box->shown != NULL ) fg_box->shown();
	t3 = ta_now();
	tv_src_host(&g_obj);
	/* a link in it to itself, opened, shows it as it is being edited, not as it was saved */
	tv_src_live(&g_obj, g_recno, &g_rec);
	ta_paper_open();	/* opened from a link: laid on its bgcol when the figure says no colour of its own */
	ta_watch(&g_obj);
	ta_watch_links();
	t4 = ta_now();
	fg_paint(FALSE);
	if ( !fg_cabinet ) fgt_show();
	t5 = ta_now();
	/* and where the time went, for the tests: start, read, window, watching, the first picture */
	snprintf(s, sizeof(s), "opened %s shapes %d links %d autoref %d parent %d up in %d us"
		 " (start %d read %d window %d watch %d paint %d us)", (char *)g_name,
		 ( g_fig != NULL ) ? g_fig->nsh : 0, tad_lnk_count(g_rec), (INT)( ( ref.flags & OB_F_AUTOREF ) != 0 ), (INT)g_has_parent,
		 (INT)( ( t5 - t0 ) / 1000 ), (INT)( ( t1 - t0 ) / 1000 ), (INT)( ( t2 - t1 ) / 1000 ),
		 (INT)( ( t3 - t2 ) / 1000 ), (INT)( ( t4 - t3 ) / 1000 ), (INT)( ( t5 - t4 ) / 1000 ));
	note(s);
	if ( g_first ) {
		ta_listen(DT_TELL_FIRST);	/* its window up: what the system puts on the desktop comes here */
		note("first window up");
	}
	auto_open();
	g_blink_at = ta_now();

	while ( !g_closing ) {
		INT	march = (INT)wm_look(LK_MARCH);
		TMO	tmo = TMO_FEVR;
		UD	now, gone;
		BOOL	damaged, turned = FALSE;

		if ( march <= 0 ) march = 800;
		if ( fg_blinks() ) {
			/* until the turn is due, counted from the last key or press */
			gone = ( ta_now() - g_blink_at ) / 1000000ULL;
			tmo = ( gone >= (UD)march ) ? 1 : (TMO)( (UD)march - gone );
		}
		er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo);
		g_drag.batch = 0;
		/* everything waiting taken first, the window drawn once after */
		while ( er >= E_OK && !g_closing ) {
			event(&m);
			er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, TMO_POL);
		}
		if ( er < E_OK && er != E_TMOUT ) break;
		ta_links_told();		/* what a panel's wait took the notice of is read too */
		if ( g_move_pending ) held_moved();
		if ( g_closing ) break;
		now = ta_now();
		damaged = fg_damaged();
		/*
		 * 選択枠のちらつき: the frames of what is taken shown and hidden
		 * by turns, a turn counted from the last key or press alone,
		 * however often the figure is drawn in between
		 */
		if ( now - g_blink_at >= (UD)march * 1000000ULL ) {
			g_blink_at = now;
			if ( fg_blinks() ) {
				fg_blink_turn();
				turned = TRUE;
			}
		}
		if ( g_dirty ) {
			fg_paint(FALSE);
		} else if ( damaged ) {
			(void)fg_paint_part(FALSE);
		} else if ( turned ) {
			(void)fg_paint_part(TRUE);	/* the frames there alone */
		} else {
			g_input_at = 0;		/* nothing came of it to draw */
		}
	}
	/* the memory the process has mapped as it ends its work, for the tests of size */
	{
		T_RPRC	r;

		if ( ts_ref_prc(ts_get_pid(), &r) >= E_OK ) {
			snprintf(s, sizeof(s), "mapped %d KB", (INT)( r.memsz / 1024 ));
			note(s);
		}
	}
	fgt_hide();
	ta_end();
	fg_undo_clear();
	if ( g_fig != NULL ) tv_fig_free(g_fig);
	g_fig = NULL;
	if ( g_rec != NULL ) tad_free(g_rec);
	g_rec = NULL;
	note("closed");
	/*
	 * パターン編集 stays open when the figure's window closes, as the
	 * desktop's does: on its own, with no figure to read from or to put
	 * a pattern in, until it is closed too
	 */
	while ( fpe_shown() ) {
		er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, TMO_FEVR);
		if ( er < E_OK ) break;
		(void)fpe_event(&m);
	}
	fpe_close();
	ta_channel_close();
	return 0;
}
