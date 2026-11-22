/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvpage.h
 *	A page of a program's window with tags as an object of its own: the
 *	tag pulled out, and このページの仮身を作る, the same in every program
 *	(lib/libtv/tv_page.c, design 10.6, 11.6)
 *
 *	What a link shows in its band is the name of the object it points at,
 *	and a link has no name of its own that the band shows instead. So a
 *	link that says which page it opens points at an object of that page:
 *	a small object named "<the program>：<the page>" (システム環境設定：書体),
 *	whose metadata's applist names the program with the page as the
 *	entry's data (固有データ), and opened, opens the program on that page.
 *	Each tag pulled out and each このページの仮身を作る makes one, and the
 *	link put is the only one to it at first: renamed, it is the person's
 *	name for it; its last link deleted, it goes as any object does.
 *
 *	The program says only what the page is (T_TVPAGE) and handles the
 *	opening itself: it is started, or run again, with the page's
 *	identifier as the 固有データ (T_PRCOPEN's data, DT_HOW_AGAIN), as
 *	from a link of its own object with an applist of its own.
 */

#ifndef __TS_TVPAGE_H__
#define __TS_TVPAGE_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>
#include <ts/ui.h>
#include <ts/tvvop.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	CONST char	*key;		/* the program's key in an applist ("tessronos-sysenv") */
	CONST char	*prog;		/* the program's name as shown (システム環境設定) */
	CONST char	*progobj;	/* the program object's UUID as text (SYSDEF_PROG_*) */
	CONST char	*page;		/* the page's identifier: the applist entry's data */
	CONST char	*label;		/* the page's name as shown (書体) */
} T_TVPAGE;

/* The longest name of a page's object, its ending 0 with it */
#define TV_PAGE_NAME_MAX	160

/* The name of page p's object, "<prog>：<label>", cut at a whole letter to max bytes: its length */
IMPORT INT  tv_page_name( CONST T_TVPAGE *p, char *buf, INT max );

/*
 * The object of page p made, on the volume of near (NULL: the first
 * volume), linked from nothing yet: E_OK and its UUID in *made
 */
IMPORT ER   tv_page_make( CONST T_TVPAGE *p, CONST TS_UUID *near, TS_UUID *made );

/*
 * このページの仮身を作る: the object of page p made and a link to it put
 * in the desktop's first window (DT_RQ_LINK_PUT), asked through a. The
 * object is taken away again when the link could not be put.
 */
IMPORT ER   tv_page_put( CONST T_TVASK *a, CONST T_TVPAGE *p );

/*
 * Tag pulled out (UI_EV_TAGPULL of u, the notice m) that stands for page
 * p: the desktop carries the outline of the tag as drawn, held where the
 * button went down (DT_RQ_CARRY), and where it is let go over a window
 * that takes links the object of the page is made (on the volume of the
 * object the desktop answers the window shows, when it answers one; else
 * the first volume) and a closed link to it, as wide as its name, put at
 * the outline's corner (DT_RQ_PLACE). Nothing is made when it is let go
 * where nothing takes it, back over the window, or not carried at all (a
 * click): then E_NOEXS or E_OBJ, as the desktop answered. The tag is not
 * chosen and the page shown stays.
 */
IMPORT ER   tv_page_pull( CONST T_TVASK *a, UI *u, CONST T_OBNTM *m, CONST T_TVPAGE *p );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVPAGE_H__ */
