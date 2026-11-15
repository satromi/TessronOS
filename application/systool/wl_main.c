/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wl_main.c
 *	ウインドウ一覧: the windows on the screen, nearest first, an
 *	accessory running as a process (design 11.2.4)
 *
 *	Each window that shows an object -- one of the desktop's, or a
 *	program's that says what object it shows -- is a closed link to that
 *	object, named with the window's title, one to a row. A panel, a
 *	tool's window and the program's own are left out. The list is asked
 *	of the desktop a window at a time (DT_RQ_WIN_LIST), for only the
 *	desktop sees every window; it says when the windows on the screen or
 *	their order change, and the list is made again. Opening a link of the
 *	list brings the window that shows its object to the front, as
 *	opening an object already open does. The window has no menu, as it
 *	had none in the desktop. What it does is said on the console for the
 *	tests ("winlist: ...").
 */

#include "st.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

static void wl_told( CONST T_DTANS *an );

static T_TAHOST	wl_host = {
	.who = "winlist", .prog = SYSDEF_PROG_WINLIST, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none, .told = wl_told,
	.at_x = 700, .at_y = 80, .at_w = 300, .at_h = 360,
};

static BOOL	g_up = FALSE;

/* The windows, as the desktop lists them */
static ER wl_load( T_TAD **p_rec )
{
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	*ids = NULL;
	UB	(*names)[WM_TITLE_MAX] = NULL;
	CONST UB **np = NULL;
	INT	i, n = 1, k = 0;
	ER	er = E_OK;
	char	s[48];

	for ( i = 0; i < n; i++ ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_WIN_LIST;
		rq.recno = i;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 5000);
		if ( er < E_OK ) break;
		if ( i == 0 ) {
			n = (INT)an.how;
			ids = malloc(sizeof(TS_UUID) * (size_t)( n + 1 ));
			names = malloc(sizeof(*names) * (size_t)( n + 1 ));
			np = malloc(sizeof(*np) * (size_t)( n + 1 ));
			if ( ids == NULL || names == NULL || np == NULL ) {
				er = E_NOMEM;
				break;
			}
		}
		an.text[DT_TEXT_MAX - 1] = 0;
		ids[k] = an.into;
		snprintf((char *)names[k], WM_TITLE_MAX, "%s", (CONST char *)an.text);
		np[k] = names[k];
		k++;
	}
	if ( er == E_NOEXS ) er = E_OK;		/* fewer than there were when it began, or none */
	*p_rec = ( er >= E_OK ) ? st_list_record("ウインドウ一覧", ids, np, k) : NULL;
	free(ids);
	free(names);
	free((void *)np);
	snprintf(s, sizeof(s), "%d windows", k);
	note(s);
	return ( *p_rec != NULL ) ? E_OK : ( er < E_OK ) ? er : E_NOMEM;
}

static ER wl_start( T_OBREF *p_ref )
{
	return st_start(&wl_host, SYSDEF_PROG_WINLIST, p_ref);
}

static void wl_begin( void )
{
	fg_as = FG_AS_TOOL;
}

static void wl_listen( void )
{
	ta_listen("window-list-viewer");
}

static void wl_shown( void )
{
	if ( !g_up ) {
		g_up = TRUE;
		note("up");
	}
}

/* The windows changed: the list made again */
static void wl_told( CONST T_DTANS *an )
{
	T_TAD	*rec = NULL;

	if ( an->how != DT_HOW_TOOL_REMADE ) return;
	if ( wl_load(&rec) >= E_OK ) fg_list_set(rec);
}

/* A link opened: the window that shows its object to the front */
static BOOL wl_open( CONST T_VOBJ *v )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[TAD_NAME_MAX + 32];
	ER	er;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_WIN_LIST;
	rq.recno = -1;
	rq.target = v->target;
	rq.flags = DT_F_OPEN;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 3000);
	snprintf(s, sizeof(s), "to the front %s (%d)", (CONST char *)v->name, (INT)er);
	note(s);
	return TRUE;
}

/* Ctrl+X takes what is picked to the tray as Ctrl+C does */
static BOOL wl_command( CONST char *code, CONST T_VOBJ *v )
{
	(void)v;
	if ( strcmp(code, "cut") != 0 ) return FALSE;
	(void)fg_copy();
	return TRUE;
}

static CONST T_FGBOX	wl_box = {
	.name = "ウインドウ一覧", .start = wl_start, .begin = wl_begin, .load = wl_load, .listen = wl_listen,
	.shown = wl_shown, .command = wl_command, .open = wl_open, .menu_def = "",
};

CONST T_FGBOX	*fg_box = &wl_box;
