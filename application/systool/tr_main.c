/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tr_main.c
 *	屑実身操作: the objects no link reaches any more, a system's tool
 *	running as a process (design 11.2.4)
 *
 *	The list is one closed link to each object of the store whose count
 *	of links is nought and that the system itself does not hold (the
 *	cabinet it opens with, the システム箱 and the templates of the
 *	programs), of those the person may read. A link carried out of the
 *	list is put where it is let go, as a copy: the object is linked to
 *	again, and leaves the list when that record is saved. 削除 (Delete)
 *	throws away for good what is taken, once the person agrees, by
 *	asking the desktop (DT_RQ_PURGE); 再探索 makes every count again from
 *	the records (DT_RQ_RECOUNT) and says what came of it. The desktop
 *	says when the list is to be made again (a record saved, a package
 *	installed or deleted). What it does is said on the console for the
 *	tests ("trash: ...").
 */

#include "st.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

static void tr_told( CONST T_DTANS *an );

static T_TAHOST	tr_host = {
	.who = "trash", .prog = SYSDEF_PROG_TRASHOBJ, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none, .told = tr_told,
	.at_x = 140, .at_y = 100, .at_w = 600, .at_h = 500,
};

static BOOL	g_up = FALSE;

/* ---------------------------------------------------------------- the list */

static BOOL in( CONST TS_UUID *u, CONST TS_UUID *set, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( memcmp(&set[i], u, sizeof(TS_UUID)) == 0 ) return TRUE;
	}
	return FALSE;
}

/*
 * What the system holds that no record links to (the cabinet it opens
 * with, the システム箱, the templates of the programs), as the desktop
 * counts them (DT_RQ_ROOTS). How many, the list in *p_ids (free it).
 */
static INT roots( TS_UUID **p_ids )
{
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	*r = NULL;
	INT	n = 0, i;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_ROOTS;
	rq.recno = -1;
	memset(&an, 0, sizeof(an));
	if ( ask_desktop(&rq, &an, 5000) >= E_OK && an.how > 0 ) {
		n = (INT)an.how;
		r = malloc(sizeof(TS_UUID) * (size_t)n);
	}
	for ( i = 0; r != NULL && i < n; i++ ) {
		rq.recno = i;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 5000) < E_OK ) break;
		r[i] = an.into;
	}
	*p_ids = r;
	return ( r != NULL ) ? i : 0;
}

static ER tr_load( T_TAD **p_rec )
{
	TS_UUID	*all, *rt;
	INT	n = tl_objects(&all), nr = roots(&rt), i, keep = 0;
	char	s[48];

	for ( i = 0; i < n; i++ ) {
		T_JSON	root;
		UB	*m;
		D	refs;

		if ( in(&all[i], rt, nr) || !tl_meta(&all[i], &m, &root) ) continue;
		refs = js_get_num(&root, "refCount", -1);
		free(m);
		if ( refs == 0 ) all[keep++] = all[i];
	}
	*p_rec = st_list_record("屑実身操作", all, NULL, keep);
	free(all);
	free(rt);
	snprintf(s, sizeof(s), "%d objects", keep);
	note(s);
	return ( *p_rec != NULL ) ? E_OK : E_NOMEM;
}

static void remake( void )
{
	T_TAD	*rec = NULL;

	if ( tr_load(&rec) >= E_OK ) fg_list_set(rec);
}

/* ---------------------------------------------------------------- what is done to it */

/* The objects of the links taken: how many, the list in *p_ids (free it) */
static INT picked( TS_UUID **p_ids )
{
	T_VOBJ	v = { .recno = 0 };
	INT	i, n = 0;
	TS_UUID	*ids = malloc(sizeof(TS_UUID) * (size_t)( ( g_npick > 0 ) ? g_npick : 1 ));

	tad_vobj_init(&v);
	for ( i = 0; ids != NULL && g_fig != NULL && i < g_fig->nsh && n < g_npick; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK ) {
			ids[n++] = v.target;
		}
	}
	tad_vobj_clear(&v);
	*p_ids = ids;
	return n;
}

/*
 * 削除: the objects taken thrown away for good, once it has been agreed
 * to. The desktop closes a window of its own showing one first; what
 * they linked to loses those links, and may itself become a 屑実身.
 */
static void purge( void )
{
	TS_UUID	*ids;
	T_DTREQ	rq;
	T_DTANS	an;
	UB	l1[WM_LABEL_MAX * 2];
	INT	n = picked(&ids), i, at = 0, done = 0;
	ER	er;
	char	s[48];

	if ( n == 0 ) {
		free(ids);
		return;
	}
	if ( n == 1 ) {
		UB	*nm = tl_name_dup(&ids[0]);

		at = st_cat(l1, at, sizeof(l1), "実身「");
		at = st_cat(l1, at, sizeof(l1), ( nm != NULL ) ? (CONST char *)nm : "");	/* a question's line: what fits */
		free(nm);
		at = st_cat(l1, at, sizeof(l1), "」を完全に削除しますか？");
	} else {
		at = st_cat_n(l1, at, sizeof(l1), n);
		at = st_cat(l1, at, sizeof(l1), "件の実身を完全に削除しますか？");
	}
	if ( !st_ask((CONST char *)l1, "この操作は取り消せません。", "いいえ", "はい") ) {
		free(ids);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PURGE;
		rq.target = ids[i];
		rq.win = g_win;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 30 * 60 * 1000);	/* the administrator's password may be asked */
		if ( er >= E_OK ) done++;
		snprintf(s, sizeof(s), "purge (%d)", (INT)er);
		note(s);
	}
	free(ids);
	snprintf(s, sizeof(s), "purged %d of %d", done, n);
	note(s);
	remake();
}

/* 再探索: every count made again from the records, once it has been agreed to, and what came of it told */
static void rescan( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UB	l1[WM_LABEL_MAX];
	INT	total = 0, changed = 0, k = 0, at = 0;
	ER	er;
	char	s[48];

	if ( !st_ask("全実身の参照数を、実際に置かれている仮身の数から数え直します。",
		     "開いている文書で保存していない仮身は数えません。", "取消", "数え直す") ) {
		return;
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_RECOUNT;
	rq.win = g_win;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	snprintf(s, sizeof(s), "recounted (%d)", (INT)er);
	note(s);
	if ( er < E_OK ) {
		st_tell("参照数を数え直せませんでした。", NULL);
		return;
	}
	{
		CONST UB	*t = ta_ans_part(DT_TAIL_TEXT, NULL);

		if ( t == NULL ) t = (CONST UB *)"";
		while ( t[k] >= '0' && t[k] <= '9' ) total = total * 10 + ( t[k++] - '0' );
		if ( t[k] == '\t' ) k++;
		while ( t[k] >= '0' && t[k] <= '9' ) changed = changed * 10 + ( t[k++] - '0' );
	}
	remake();
	at = st_cat_n(l1, at, sizeof(l1), total);
	if ( changed > 0 ) {
		at = st_cat(l1, at, sizeof(l1), "件の実身のうち、");
		at = st_cat_n(l1, at, sizeof(l1), changed);
		at = st_cat(l1, at, sizeof(l1), "件の参照数を修正しました。");
	} else {
		at = st_cat(l1, at, sizeof(l1), "件の実身の参照数はすべて正しい値でした。");
	}
	st_tell((CONST char *)l1, NULL);
}

/* ---------------------------------------------------------------- what the cabinet asks of it */

static ER tr_start( T_OBREF *p_ref )
{
	return st_start(&tr_host, SYSDEF_PROG_TRASHOBJ, p_ref);
}

static void tr_begin( void )
{
	fg_as = FG_AS_TOOL;
}

static void tr_listen( void )
{
	ta_listen("trash-real-objects");
}

static void tr_shown( void )
{
	if ( !g_up ) {
		g_up = TRUE;
		note("up");
	}
}

/* The desktop said the counts may have moved, or the tool was run again: the list made again */
static void tr_told( CONST T_DTANS *an )
{
	if ( an->how != DT_HOW_TOOL_REMADE && an->how != DT_HOW_TOOL_AGAIN ) return;
	note(( an->how == DT_HOW_TOOL_AGAIN ) ? "run again" : "told to make the list again");
	remake();
}

/* The menu's items: 再表示, トレーへ移動 (a copy: nothing leaves the list but by a link), 削除, 再探索 */
static BOOL tr_command( CONST char *code, CONST T_VOBJ *v )
{
	(void)v;
	if ( strcmp(code, "refresh") == 0 ) remake();
	else if ( strcmp(code, "cut") == 0 ) (void)fg_copy();
	else if ( strcmp(code, "delete") == 0 ) purge();
	else if ( strcmp(code, "rescan") == 0 ) rescan();
	else return FALSE;
	return TRUE;
}

static CONST T_FGBOX	tr_box = {
	.name = "屑実身操作", .start = tr_start, .begin = tr_begin, .load = tr_load, .listen = tr_listen,
	.shown = tr_shown, .command = tr_command, .menu_def = SYSDEF_MENU_TRASH,
};

CONST T_FGBOX	*fg_box = &tr_box;
