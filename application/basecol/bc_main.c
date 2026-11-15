/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bc_main.c
 *	原紙集め: the templates of every application of the system, shown
 *	as the 原紙箱 shows them, an accessory running as a process (design
 *	11.2.3)
 *
 *	The program is 仮身一覧's (application/fig with the cabinet's
 *	menu and ways) linked with this file, which says that it is
 *	原紙集め (FG_BASECOL): the window, its scroll bars down the right
 *	and along the foot, what is drawn and what a press does are the
 *	cabinet's, as in the 原紙箱.
 *
 *	The list is made, not read: one closed link to each template the
 *	applications of the program box name in "tessronos.program.base",
 *	in the box's order, each as wide as the template's name, laid out
 *	in rows from the left as far as the window is wide, 27 pixels from
 *	one row to the next. It is no object's record and nothing in it is
 *	changed or saved (FG_MADE). The window stands for the program
 *	object of 原紙集め, whatever the desktop started it on. A template
 *	carried out of it is asked of the desktop with DT_F_BASE, as one
 *	carried out of the 原紙箱 (FG_TEMPLATES): where it is let go, the
 *	desktop makes a new object from the template and puts the link to
 *	it there. Whatever was taken out of the 原紙箱, the templates are
 *	still the programs', and here.
 *
 *	The list is made again when the program box changes (a program
 *	installed or deleted), from the menu's 再表示, and laid out again
 *	when the window is made wider or narrower. What it does is said on
 *	the console for the tests ("basecol: ...").
 */

#include "../fig/fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/tvsrc.h>

BOOL	fg_cabinet = TRUE;

#define BC_CHSZ		14		/* the letters of a link, as the 原紙箱's */
#define BC_PAD		8		/* left of the links, and above the first */
#define BC_GAP		8		/* between one link and the next, across and down */
#define BC_BAND		( BC_CHSZ + BC_CHSZ / 4 + 2 )	/* a closed link's height (19 at 14), for the window's height */
#define BC_MAX		256		/* templates listed */

/* where the window opens: right of the 原紙箱's links, as tall as the list (most of the screen at most) */
#define BC_WIN_X	660
#define BC_WIN_Y	40
#define BC_WIN_W	220
#define BC_WIN_HMIN	160
#define BC_WIN_HMAX	640

typedef struct {
	TS_UUID	id;
	UB	name[TAD_NAME_MAX];
} BCENT;

static BCENT	g_ent[BC_MAX];
static INT	g_n = 0;
static INT	g_laid_w = -1;		/* the width the list was last laid out to */
static BOOL	g_up = FALSE;
static TS_UUID	g_box;			/* the program box */
static BOOL	g_has_box = FALSE;

static T_TAHOST	bc_host = {
	.who = "basecol", .prog = SYSDEF_PROG_BASECOL, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none,
	.at_x = BC_WIN_X, .at_y = BC_WIN_Y, .at_w = BC_WIN_W, .at_h = BC_WIN_HMIN,
};

/* ---------------------------------------------------------------- what the programs name */

/* One template more, unless it is listed already */
static void put_template( CONST TS_UUID *b )
{
	INT	i;

	for ( i = 0; i < g_n; i++ ) {
		if ( memcmp(&g_ent[i].id, b, sizeof(*b)) == 0 ) return;
	}
	if ( g_n >= BC_MAX || !tl_name(b, g_ent[g_n].name, TAD_NAME_MAX) ) return;
	g_ent[g_n].id = *b;
	g_n++;
}

/* The templates of the applications of the program box, in its order */
static void gather( void )
{
	TS_UUID	*lk;
	INT	n = 0, all = 0, i;

	g_n = 0;
	g_has_box = (BOOL)( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &g_box) >= E_OK );
	if ( !g_has_box || ob_lst_lnk(&g_box, NULL, NULL, 0, &n) < E_OK || n <= 0 ) {
		return;
	}
	lk = malloc(sizeof(TS_UUID) * (size_t)n);
	if ( lk == NULL ) return;
	if ( ob_lst_lnk(&g_box, NULL, lk, n, &all) < E_OK ) all = 0;
	if ( all < n ) n = all;
	for ( i = 0; i < n; i++ ) {
		T_JSON	root, tf, g, base, it;
		UB	*m, kind[16], us[TS_UUID_STRLEN + 1];

		if ( !tl_meta(&lk[i], &m, &root) ) continue;
		if ( js_get(&root, "tessronos", &tf) >= E_OK
		  && js_get(&tf, "program", &g) >= E_OK
		  && ( js_get_str(&g, "kind", kind, sizeof(kind)) <= 0 || strcmp((char *)kind, "app") == 0 )
		  && js_get(&g, "base", &base) >= E_OK ) {
			for ( it.s = NULL; js_next(&base, &it); ) {
				TS_UUID	b;

				if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
				  && ts_str_to_uuid((CONST char *)us, &b) >= E_OK ) {
					put_template(&b);
				}
			}
		}
		free(m);
	}
	free(lk);
}

/* ---------------------------------------------------------------- the list as a figure */

/* The width the list is laid out to: the work area's, or the window's to be before it is open */
static INT lay_width( void )
{
	return ( g_w > 0 ) ? g_w : BC_WIN_W - 30;
}

/*
 * The record of the templates gathered: one closed link to each, from
 * the left of a row as far as the width allows (one at least), then on
 * the next row
 */
static ER make_list( T_TAD **p_rec )
{
	T_TLLAY	lay = { .how = TL_FLOW, .x = BC_PAD, .y = BC_PAD, .gap = BC_GAP, .chsz = BC_CHSZ };
	TS_UUID	ids[BC_MAX];
	CONST UB *names[BC_MAX];
	INT	i, rows = 0;
	char	s[64];

	lay.width = lay_width();
	for ( i = 0; i < g_n; i++ ) {
		ids[i] = g_ent[i].id;
		names[i] = g_ent[i].name;
	}
	*p_rec = tl_record("原紙集め", ids, names, NULL, g_n, &lay, &rows);
	if ( *p_rec == NULL ) return E_NOMEM;
	g_laid_w = lay.width;
	snprintf(s, sizeof(s), "%d templates in %d rows %d wide", g_n, rows, lay.width);
	note(s);
	return E_OK;
}

static ER bc_load( T_TAD **p_rec )
{
	gather();
	return make_list(p_rec);
}

/*
 * The list made again in place of the one shown, scrolled as it was
 * within what it is now; gather FALSE: the same templates laid out
 * again. What the hand holds within the window (a band, a template
 * carried within it) is let go first, for the shapes it holds are the
 * old list's; a template carried out is the desktop's until it is let
 * go, and nothing of this window is read meanwhile.
 */
static void remake( BOOL again_gather )
{
	T_TAD	*rec = NULL;

	if ( fg_holding() ) fg_hold_cancel();
	if ( again_gather ) gather();
	if ( make_list(&rec) >= E_OK ) fg_list_set(rec);
}

/* ---------------------------------------------------------------- what the cabinet asks of it */

/* The window stands for the program object of 原紙集め, as tall as the list will be */
static ER bc_start( T_OBREF *p_ref )
{
	TS_UUID	self;
	INT	h;

	if ( ts_str_to_uuid(SYSDEF_PROG_BASECOL, &self) < E_OK ) return E_NOEXS;
	gather();
	h = BC_PAD + g_n * ( BC_BAND + BC_GAP ) + 48;
	if ( h < BC_WIN_HMIN ) h = BC_WIN_HMIN;
	if ( h > BC_WIN_HMAX ) h = BC_WIN_HMAX;
	bc_host.at_h = h;
	return ta_start_on(&bc_host, &self, p_ref);
}

static void bc_begin( void )
{
	fg_as = FG_AS_BASECOL;
}

/* The program box watched: a program installed or deleted is seen at once */
static void bc_listen( void )
{
	if ( g_has_box ) ta_watch(&g_box);
}

/* The window open, or made wider or narrower: laid out again to its width */
static void bc_shown( void )
{
	if ( !g_up ) {
		g_up = TRUE;
		note("up");
	}
	if ( g_w > 0 && g_w != g_laid_w ) remake(FALSE);
}

static BOOL bc_changed( CONST TS_UUID *u )
{
	if ( !g_has_box || memcmp(u, &g_box, sizeof(TS_UUID)) != 0 ) return FALSE;
	note("the program box changed");
	remake(TRUE);
	return TRUE;
}

/* 再表示: the templates gathered again */
static BOOL bc_command( CONST char *code, CONST T_VOBJ *v )
{
	(void)v;
	if ( strcmp(code, "reread") != 0 ) return FALSE;
	tv_src_forget(NULL);
	remake(TRUE);
	return TRUE;
}

static CONST T_FGBOX	bc_box = {
	.begin = bc_begin, .load = bc_load, .listen = bc_listen, .command = bc_command,
	.start = bc_start, .shown = bc_shown, .changed = bc_changed,
};

CONST T_FGBOX	*fg_box = &bc_box;
