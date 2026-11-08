/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cb_main.c
 *	仮身一覧 as a program running as a process (design 11.3)
 *
 *	A cabinet is a figure whose links are worked and whose shapes are
 *	not edited, as the desktop shows one in a window of its own (a
 *	figure's window without the figure editor). The program is
 *	基本図形編集's (application/fig) linked with this file, which says
 *	that it is the cabinet: its menu is the cabinet's (SYSDEF_MENU_CAB),
 *	nothing but links is taken, and there is no 道具パネル, no text
 *	edited where it stands and no パターン編集. What it does is said on
 *	the console for the tests ("cabinet: ...").
 */

#include "../fig/fg.h"

BOOL	fg_cabinet = TRUE;
