/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mn.h
 *	Menu manager: menus made from menu definitions (design 18.15)
 *
 *	A menu's words, order, menu letters and nesting are held by menu
 *	definition objects. mn_cre_men reads one and everything it opens
 *	(sub) or takes in (include) into one menu; the program then sets the
 *	state of items, fills the lists it keeps, shows the menu and turns
 *	the answer back into which item of which definition was chosen.
 *	Items are named by their definition and their code, never by where
 *	they stand, so a definition can be reordered without touching the
 *	program.
 */

#ifndef __TS_MN_H__
#define __TS_MN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/part.h>

#define MN_MAX		8		/* menus made at once */
#define MN_NODE_MAX	24		/* lists in one menu */
#define MN_CODE_MAX	32		/* bytes of an item's code, with its terminator */

/* The state of an item (mn_chg_atr) */
#define MN_GREY		0x0001		/* shown, but cannot be chosen */
#define MN_TICK		0x0002		/* shown with a mark */
#define MN_HIDE		0x0010		/* not shown */

/* Which item was chosen */
typedef struct {
	TS_UUID	def;			/* the definition the item is in */
	UB	code[MN_CODE_MAX];
	INT	index;			/* its place in a list the program filled */
	D	value;			/* the item's value in the definition, or 0 */
	UB	label[WM_LABEL_MAX];
} T_MNSEL;

/* A menu from a definition and all it opens and takes in */
IMPORT ER   mn_cre_men( CONST TS_UUID *def, ID *p_mid );
IMPORT ER   mn_del_men( ID mid );

/*
 * The state of the items with a code, in one definition or (def NULL)
 * in any; answers how many there were.
 */
IMPORT INT  mn_chg_atr( ID mid, CONST TS_UUID *def, CONST char *code, UINT atr );

/* The state of the first item with a code; E_NOEXS when there is none */
IMPORT INT  mn_get_atr( ID mid, CONST TS_UUID *def, CONST char *code );

/*
 * A list the definition leaves to the program ("list": true), filled
 * with n labels, each stride bytes apart. The items keep the list's
 * code and are told apart by T_MNSEL.index.
 */
IMPORT ER   mn_set_lst( ID mid, CONST TS_UUID *def, CONST char *code,
			CONST UB *labels, INT stride, INT n );

/* The state of one item of a list the program filled, by its place in it */
IMPORT ER   mn_chg_idx( ID mid, CONST TS_UUID *def, CONST char *code, INT index,
			UINT atr );

/* Show it at (x, y) in a window's work area; answers the menu's panel */
IMPORT INT  mn_opn_men( ID mid, INT wid, INT x, INT y );

/* The panel's answer (wm_menu_event) as the item chosen */
IMPORT ER   mn_get_sel( ID mid, INT cmd, T_MNSEL *sel );

/* The item a menu letter chooses, when it is there and not grey */
IMPORT ER   mn_fnd_key( ID mid, UB key, T_MNSEL *sel );

/* Whether a chosen item has this code */
IMPORT BOOL mn_is( CONST T_MNSEL *sel, CONST char *code );

/*
 * ウインドウ (SYSDEF_MENU_WINLIST): the list filled with the windows
 * shown, before the menu opens; and an item chosen from it done -- the
 * window brought to the front with the keys -- TRUE when it was one.
 */
IMPORT void mn_winlist_fill( ID mid );
IMPORT BOOL mn_winlist_do( CONST T_MNSEL *sel );

/*
 * For a program running as a process: the menu shown at (x, y) of the
 * work area of its window (the key of a window it may write), opened by
 * a press at `when` (T_OBNTM.when), and waited for. E_OK and the item
 * chosen in *sel; E_NOEXS when it was put away with nothing chosen.
 */
IMPORT ER   mn_pop_men( ID mid, ID wkey, INT x, INT y, UD when, T_MNSEL *sel );

#ifdef __cplusplus
}
#endif

#endif /* __TS_MN_H__ */
