/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bb_main.c
 *	原紙箱 as a program running as a process (design 11.2.2)
 *
 *	The program is 仮身一覧's (application/fig with the cabinet's menu
 *	and ways) linked with this file, which says that it is the 原紙箱
 *	(FG_BASEBOX). The window shows the object's own record, a box of
 *	templates: what the figure's code does with its links (fg_edit.c)
 *	keeps them where they stand, neither moved nor sized and left by
 *	整頓, and one carried out is made a new object by the desktop where
 *	it is let go (DT_F_BASE, FG_TEMPLATES). Its windows have the 原紙箱's
 *	icon, and the process the program's name. The desktop starts it on
 *	an object whose program is the 原紙箱, and on the system's 原紙箱
 *	when the program is run by itself. What it does is said on the
 *	console for the tests ("basebox: ...").
 */

#include "../fig/fg.h"
#include <ts/sysdef.h>

BOOL	fg_cabinet = TRUE;

static CONST T_TAHOST	bb_host = {
	.who = "basebox", .prog = SYSDEF_PROG_BASEBOX, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none,
};

static ER bb_start( T_OBREF *p_ref )
{
	return ta_start(&bb_host, p_ref);
}

static void bb_begin( void )
{
	fg_as = FG_AS_BASEBOX;
}

static CONST T_FGBOX	bb_box = {
	.name = "原紙箱", .start = bb_start, .begin = bb_begin,
};

CONST T_FGBOX	*fg_box = &bb_box;
