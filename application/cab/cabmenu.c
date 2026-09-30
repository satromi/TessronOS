/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cabmenu.c
 *	仮身一覧 の menu (design 16.5.13, 18.15)
 *
 *	The menu is the cabinet's definition (CABMENU.DEF). Here is only
 *	what the cabinet knows of it: which items are grey or marked for
 *	what the window is, and which of its commands each item's code is.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include <ts/cabmenu.h>

typedef struct {
	CONST char	*code;
	INT		cmd;
} CODECMD;

LOCAL CONST CODECMD cab_cmds[] = {
	{ "save",	CM_SAVE },
	{ "save.new",	CM_SAVE_AS_NEW },
	{ "fullscreen",	CM_FULLSCREEN },
	{ "refresh",	CM_REFRESH },
	{ "bgcolour",	CM_BGCOLOUR },
	{ "hidden",	CM_SHOW_HIDDEN },
	{ "undo",	CM_UNDO },
	{ "copy",	CM_COPY },
	{ "paste",	CM_PASTE },
	{ "cut",	CM_CUT },
	{ "moveback",	CM_MOVE_BACK },
	{ "delete",	CM_DELETE },
	{ "front",	CM_FRONT },
	{ "back",	CM_BACK },
	{ "arrange",	CM_ARRANGE },
	{ "fix",	CM_FIX },
	{ "unfix",	CM_UNFIX },
	{ "bg",		CM_BG },
	{ "unbg",	CM_UNBG },
};

#define NCMD	( (INT)( sizeof(cab_cmds) / sizeof(cab_cmds[0]) ) )

/* The items that act on what is taken, grey when nothing is */
LOCAL CONST char * CONST on_pick[] = {
	"copy", "cut", "delete", "front", "back", "arrange",
	"fix", "unfix", "bg", "unbg"
};

EXPORT ER cab_menu_make( CONST T_CABMENU *st, ID *p_mid )
{
	TS_UUID	def;
	ID	mid;
	ER	er;
	INT	i;

	if ( st == NULL || p_mid == NULL ) {
		return E_PAR;
	}
	er = ts_str_to_uuid(SYSDEF_MENU_CAB, &def);
	if ( er >= E_OK ) {
		er = mn_cre_men(&def, &mid);
	}
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < (INT)( sizeof(on_pick) / sizeof(on_pick[0]) ); i++ ) {
		(void)mn_chg_atr(mid, NULL, on_pick[i], ( st->npick == 0 ) ? MN_GREY : 0);
	}
	(void)mn_chg_atr(mid, NULL, "save.new", st->can_save_new ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "fullscreen", st->full ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "hidden", st->show_hidden ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "paste", st->can_paste ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "moveback", st->can_paste ? 0 : MN_GREY);
	*p_mid = mid;

	return E_OK;
}

EXPORT INT cab_menu_cmd( CONST T_MNSEL *sel )
{
	INT	i;

	for ( i = 0; i < NCMD; i++ ) {
		if ( mn_is(sel, cab_cmds[i].code) ) {
			return cab_cmds[i].cmd;
		}
	}
	return 0;
}
