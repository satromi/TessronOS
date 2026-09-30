/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cabmenu.h
 *	仮身一覧 の menu (design 16.5.13)
 *
 *	The menu of a cabinet is its definition (CABMENU.DEF, design 18.15):
 *	the words, the order and the menu letters are there. What is here
 *	is what the cabinet knows of it -- which state its items are put in,
 *	and which of the cabinet's commands each item's code is.
 */

#ifndef __TS_CABMENU_H__
#define __TS_CABMENU_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/part.h>
#include <ts/mn.h>

/* What the menu is opened on, which sets what can be chosen in it */
typedef struct {
	INT	npick;			/* how many virtual objects are taken */
	BOOL	can_save_new;		/* 新たな実身に保存 applies */
	BOOL	full;			/* the window fills the screen */
	BOOL	can_paste;		/* the set in hand in the tray holds links */
	BOOL	show_hidden;		/* 隠蔽仮身を表示 is on */
} T_CABMENU;

/* 保存 */
#define CM_SAVE			300
#define CM_SAVE_AS_NEW		301

/* 表示 */
#define CM_FULLSCREEN		310
#define CM_REFRESH		311
#define CM_BGCOLOUR		312
#define CM_SHOW_HIDDEN		313

/* 編集 */
#define CM_UNDO			320
#define CM_COPY			321
#define CM_PASTE		322
#define CM_CUT			323
#define CM_MOVE_BACK		324	/* クリップボードから移動 */
#define CM_DELETE		325
#define CM_FRONT		326
#define CM_BACK			327
#define CM_ARRANGE		328

/* 保護 */
#define CM_FIX			330
#define CM_UNFIX		331
#define CM_BG			332
#define CM_UNBG			333

/* The cabinet's menu made from its definition, its items set to st */
IMPORT ER  cab_menu_make( CONST T_CABMENU *st, ID *p_mid );

/* The command a chosen item is, or 0 when it is not one of the cabinet's */
IMPORT INT cab_menu_cmd( CONST T_MNSEL *sel );

#ifdef __cplusplus
}
#endif

#endif /* __TS_CABMENU_H__ */
