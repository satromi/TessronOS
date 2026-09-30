/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docmenu.c
 *	基本文章編集 の menu (design 17.2, 18.15)
 *
 *	The menu is the editor's definition (DOCMENU.DEF). Here is only what
 *	the editor knows of it: the marks and the grey items that follow the
 *	document's state, the faces used last, and which of its commands
 *	each item's code is. An item with a value (文字サイズ, 文字間隔)
 *	carries the size or the step in the definition, not in a number
 *	made up here.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include <ts/docmenu.h>

typedef struct {
	CONST char	*code;
	INT		cmd;
} CODECMD;

LOCAL CONST CODECMD doc_cmds[] = {
	{ "save",		DM_SAVE },
	{ "save.new",		DM_SAVE_AS_NEW },
	{ "view.xml",		DM_VIEW_SET_XML },
	{ "view.detail",	DM_VIEW_SET_DETAIL },
	{ "view.clean",		DM_VIEW_SET_CLEAN },
	{ "fullscreen",		DM_FULLSCREEN },
	{ "refresh",		DM_REFRESH },
	{ "bgcolour",		DM_BGCOLOUR },
	{ "hidden",		DM_SHOW_HIDDEN },
	{ "wrap",		DM_WRAP },
	{ "undo",		DM_UNDO },
	{ "copy",		DM_COPY },
	{ "paste",		DM_PASTE },
	{ "cut",		DM_CUT },
	{ "moveback",		DM_MOVE_BACK },
	{ "selectall",		DM_SELECT_ALL },
	{ "find",		DM_FIND },
	{ "virtualize",		DM_VIRTUALIZE },
	{ "font.list",		DM_FONT_LIST },
	{ "font.gothic",	DM_FONT_GOTHIC },
	{ "font.mincho",	DM_FONT_MINCHO },
	{ "font.meiryo",	DM_FONT_MEIRYO },
	{ "style.normal",	DM_STYLE_NORMAL },
	{ "style.bold",		DM_STYLE_BOLD },
	{ "style.italic",	DM_STYLE_ITALIC },
	{ "style.bagchar",	DM_STYLE_BAGCHAR },
	{ "style.box",		DM_STYLE_BOX },
	{ "style.shadow",	DM_STYLE_SHADOW },
	{ "style.underline",	DM_STYLE_UNDERLINE },
	{ "style.overline",	DM_STYLE_OVERLINE },
	{ "style.strike",	DM_STYLE_STRIKE },
	{ "style.hatch",	DM_STYLE_HATCH },
	{ "style.inverse",	DM_STYLE_INVERSE },
	{ "style.noprint",	DM_STYLE_NOPRINT },
	{ "style.super",	DM_STYLE_SUPER },
	{ "style.sub",		DM_STYLE_SUB },
	{ "style.indent",	DM_STYLE_INDENT },
	{ "style.clear",	DM_STYLE_CLEAR },
	{ "size.custom",	DM_SIZE_CUSTOM },
	{ "size.full",		DM_SIZE_FULLWIDTH },
	{ "size.half",		DM_SIZE_HALFWIDTH },
	{ "colour.black",	DM_COLOUR_BLACK },
	{ "colour.blue",	DM_COLOUR_BLUE },
	{ "colour.red",		DM_COLOUR_RED },
	{ "colour.pink",	DM_COLOUR_PINK },
	{ "colour.orange",	DM_COLOUR_ORANGE },
	{ "colour.green",	DM_COLOUR_GREEN },
	{ "colour.lime",	DM_COLOUR_LIME },
	{ "colour.cyan",	DM_COLOUR_CYAN },
	{ "colour.yellow",	DM_COLOUR_YELLOW },
	{ "colour.white",	DM_COLOUR_WHITE },
	{ "colour.custom",	DM_COLOUR_CUSTOM },
	{ "fmt.tab",		DM_FMT_NEW_TAB },
	{ "fmt.indent",		DM_FMT_INDENT },
	{ "align.left",		DM_ALIGN_LEFT },
	{ "align.centre",	DM_ALIGN_CENTRE },
	{ "align.right",	DM_ALIGN_RIGHT },
	{ "ruby.top",		DM_RUBY_TOP },
	{ "ruby.bottom",	DM_RUBY_BOTTOM },
	{ "pagebreak",		DM_PAGEBREAK },
	{ "paperframe",		DM_PAPER_FRAME },
	{ "pagesetup",		DM_PAGE_SETUP },
};

#define NCMD	( (INT)( sizeof(doc_cmds) / sizeof(doc_cmds[0]) ) )

#define DM_SPACE_STEPS	( DM_SPACE_3_4 - DM_SPACE_0 + 1 )

EXPORT ER doc_menu_make( CONST T_DOCMENU *st, ID *p_mid )
{
	UB	(*shown)[WM_LABEL_MAX];
	TS_UUID	def;
	ID	mid;
	ER	er;
	INT	i, a, b;

	if ( st == NULL || p_mid == NULL ) {
		return E_PAR;
	}
	er = ts_str_to_uuid(SYSDEF_MENU_DOC, &def);
	if ( er >= E_OK ) {
		er = mn_cre_men(&def, &mid);
	}
	if ( er < E_OK ) {
		return er;
	}
	(void)mn_chg_atr(mid, NULL, "save.new", st->is_root ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "view.xml", ( st->view == DM_VIEW_XML ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "view.detail", ( st->view == DM_VIEW_DETAIL ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "view.clean", ( st->view == DM_VIEW_CLEAN ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "hidden", st->show_hidden ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "wrap", st->wrap ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "paperframe", st->paper_frame ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "virtualize", st->has_pick ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "paste", st->can_paste ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "moveback", st->can_paste ? 0 : MN_GREY);

	/* the faces used last, their names shown without the quotes */
	shown = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * DM_RECENT_MAX);
	if ( shown != NULL ) {
		for ( i = 0; i < st->nrecent && i < DM_RECENT_MAX; i++ ) {
			for ( a = 0, b = 0; st->recent[i][a] != 0 && b < WM_LABEL_MAX - 1; a++ ) {
				if ( st->recent[i][a] != '"' ) {
					shown[i][b++] = st->recent[i][a];
				}
			}
			shown[i][b] = 0;
		}
		(void)mn_set_lst(mid, NULL, "font.recent", (CONST UB *)shown, WM_LABEL_MAX, i);
		Kfree(shown);
	}
	*p_mid = mid;

	return E_OK;
}

EXPORT INT doc_menu_cmd( CONST T_MNSEL *sel )
{
	INT	i;

	if ( mn_is(sel, "size") ) {
		return ( sel->value > 0 ) ? DM_SIZE((INT)sel->value) : 0;
	}
	if ( mn_is(sel, "space") ) {
		return ( sel->value >= 0 && sel->value < DM_SPACE_STEPS )
		       ? DM_SPACE_0 + (INT)sel->value : 0;
	}
	if ( mn_is(sel, "font.recent") ) {
		return ( sel->index < DM_RECENT_MAX ) ? DM_FONT_RECENT + sel->index : 0;
	}
	for ( i = 0; i < NCMD; i++ ) {
		if ( mn_is(sel, doc_cmds[i].code) ) {
			return doc_cmds[i].cmd;
		}
	}
	return 0;
}
