/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sr_main.c
 *	実身/仮身検索: the objects whose name, relationship tags or text hold
 *	some words, or whose dates fall between two days, a system's tool
 *	running as a process (design 11.2.4)
 *
 *	What to look for is asked first, in the tools' data box's panel
 *	(ST_PN_SEARCH): the words, where to look for them -- the name, the
 *	tags of the object and of the links in its record, the text of its
 *	first record, or all three -- and, when the dates matter, which date
 *	and the days it must fall between. Every object of the store the
 *	person may read is asked, and one closed link to each that answers
 *	is listed, the window titled with the words and how many answered.
 *	Put away before anything answered, or when nothing answers the first
 *	question, the tool ends without a window. 再検索 asks again from the
 *	last question, 再表示 puts the same question again; run again while
 *	it is open, the desktop brings the window to the front and it asks
 *	again. What it does is said on the console for the tests ("search:
 *	...").
 */

#include "st.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

#define Q_NAME		0		/* 実身名 */
#define Q_RELATION	1		/* 続柄 */
#define Q_TEXT		2		/* 本文 */
#define Q_ALL		3		/* 全て */
#define Q_DATE_NONE	0
#define Q_DATE_MADE	1		/* 作成日 */
#define Q_DATE_UPDATED	2		/* 更新日 */
#define Q_DATE_ACCESSED	3		/* 参照日 */

/* The parts of the question's panel */
#define P_TEXT		1
#define P_FROM		2
#define P_TO		3
#define P_G_TARGET	10		/* 実身名 続柄 本文 全て */
#define P_G_DATE	20		/* なし 作成日 更新日 参照日 */

typedef struct {
	UB	text[WM_LABEL_MAX];	/* the words, or empty */
	INT	target;			/* Q_NAME .. Q_ALL */
	INT	date_kind;		/* Q_DATE_* */
	UB	from[16], to[16];	/* "YYYY-MM-DD", either may be empty */
} QUERY;

static void sr_told( CONST T_DTANS *an );

static T_TAHOST	sr_host = {
	.who = "search", .prog = SYSDEF_PROG_OBJSEARCH, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none, .told = sr_told,
	.at_x = 200, .at_y = 120, .at_w = 600, .at_h = 500,
};

/* The last question asked, kept so that asking again starts from it */
static QUERY	g_q = { .target = Q_ALL, .date_kind = Q_DATE_NONE };
static T_TAD	*g_first = NULL;		/* the answer to the first question, for the window to show */
static UB	g_title[WM_TITLE_MAX];
static BOOL	g_up = FALSE;

/* ---------------------------------------------------------------- matching */

/* An ASCII letter made small, so that the words match either way */
static UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c + 'a' - 'A' ) : c;
}

/* Whether 'what' is somewhere in the first 'n' bytes of 'in' */
static BOOL has( CONST UB *in, SZ n, CONST UB *what )
{
	SZ	i, k;

	if ( what[0] == 0 ) return TRUE;
	for ( i = 0; i < n; i++ ) {
		for ( k = 0; what[k] != 0 && i + k < n && lower(in[i + k]) == lower(what[k]); k++ ) ;
		if ( what[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/* Whether any value of an attribute in the record holds the words */
static BOOL attr_has( CONST UB *buf, SZ len, CONST char *attr, CONST UB *what )
{
	SZ	i, k, n = (SZ)strlen(attr);

	for ( i = 0; i + n < len; i++ ) {
		if ( memcmp(buf + i, attr, (size_t)n) != 0 ) continue;
		for ( k = i + n; k < len && buf[k] != '"'; k++ ) ;
		if ( k > i + n && has(buf + i + n, k - ( i + n ), what) ) return TRUE;
		i = k;
	}
	return FALSE;
}

/* The words of a record, without its markup, the five named characters put back. Done in place */
static SZ text_of( UB *buf, SZ len )
{
	static CONST char *CONST nm[] = { "&amp;", "&lt;", "&gt;", "&quot;", "&apos;" };
	static CONST UB	ch[] = { '&', '<', '>', '"', '\'' };
	SZ	i, o = 0, j;
	BOOL	in_tag = FALSE;
	INT	k;

	for ( i = 0; i < len; i++ ) {
		UB	c = buf[i];

		if ( in_tag ) {
			if ( c == '>' ) in_tag = FALSE;
			continue;
		}
		if ( c == '<' ) {
			in_tag = TRUE;
			continue;
		}
		if ( c == '&' ) {
			for ( k = 0; k < 5; k++ ) {
				for ( j = 0; nm[k][j] != 0 && i + j < len && buf[i + j] == (UB)nm[k][j]; j++ ) ;
				if ( nm[k][j] == 0 ) {
					c = ch[k];
					i += j - 1;
					break;
				}
			}
		}
		buf[o++] = c;
	}
	buf[o] = 0;
	return o;
}

/* "YYYY-MM-DD" as a number that sorts as the days do; 0 when not a day */
static INT day_of( CONST UB *s )
{
	INT	v = 0, n = 0, i;

	for ( i = 0; s[i] != 0 && n < 8; i++ ) {
		if ( s[i] >= '0' && s[i] <= '9' ) {
			v = v * 10 + ( s[i] - '0' );
			n++;
		} else if ( s[i] != '-' && s[i] != '/' && s[i] != ' ' ) {
			break;
		}
	}
	return ( n == 8 ) ? v : 0;
}

/* Whether one object answers the question */
static BOOL match( CONST TS_UUID *id, CONST QUERY *q )
{
	T_JSON	root, tags, it;
	UB	*m, s[64];		/* a date, as ISO 8601 writes one */
	BOOL	hit = FALSE;

	if ( !tl_meta(id, &m, &root) ) return FALSE;	/* not one the person may read */
	if ( q->date_kind != Q_DATE_NONE ) {
		INT	day, from = day_of(q->from), to = day_of(q->to);

		s[0] = 0;
		(void)js_get_str(&root, ( q->date_kind == Q_DATE_MADE ) ? "makeDate"
				 : ( q->date_kind == Q_DATE_UPDATED ) ? "updateDate" : "accessDate", s, sizeof(s));
		day = day_of(s);
		if ( day == 0 || ( from != 0 && day < from ) || ( to != 0 && day > to ) ) {
			free(m);
			return FALSE;
		}
		if ( q->text[0] == 0 ) {
			free(m);
			return TRUE;		/* the dates alone */
		}
	}
	if ( ( q->target == Q_NAME || q->target == Q_ALL )
	  && js_get_str(&root, "name", s, sizeof(s)) >= 0 && has(s, (SZ)strlen((char *)s), q->text) ) {
		hit = TRUE;
	}
	if ( !hit && ( q->target == Q_RELATION || q->target == Q_ALL ) && js_get(&root, "relationship", &tags) >= E_OK ) {
		for ( it.s = NULL; !hit && js_next(&tags, &it); ) {
			if ( js_str(&it, s, sizeof(s)) > 0 && has(s, (SZ)strlen((char *)s), q->text) ) hit = TRUE;
		}
	}
	free(m);
	if ( !hit && ( q->target == Q_TEXT || q->target == Q_ALL || q->target == Q_RELATION ) ) {
		UB	*buf = NULL;
		INT	len = ui_rec_all(id, 0, 0, &buf);

		if ( len > 0 && buf != NULL ) {
			/* the tags the links in the record carry */
			if ( q->target != Q_TEXT ) hit = attr_has(buf, (SZ)len, "relationship=\"", q->text);
			if ( !hit && q->target != Q_RELATION ) hit = has(buf, text_of(buf, (SZ)len), q->text);
		}
		free(buf);
	}
	return hit;
}

/* The question put to every object in the store: the page of those that answer, its title in g_title */
static T_TAD *answer( CONST QUERY *q, INT *p_hits )
{
	TS_UUID	*all;
	INT	n = tl_objects(&all), i, hits = 0, at = 0;
	T_TAD	*rec;
	char	s[64];

	for ( i = 0; i < n; i++ ) {
		if ( match(&all[i], q) ) all[hits++] = all[i];
	}
	at = st_cat(g_title, at, sizeof(g_title), "検索結果");
	if ( q->text[0] != 0 ) {
		at = st_cat(g_title, at, sizeof(g_title), "「");
		at = st_cat(g_title, at, sizeof(g_title), (CONST char *)q->text);
		at = st_cat(g_title, at, sizeof(g_title), "」");
	}
	at = st_cat(g_title, at, sizeof(g_title), " ");
	at = st_cat_n(g_title, at, sizeof(g_title), hits);
	(void)st_cat(g_title, at, sizeof(g_title), "件");
	rec = st_list_record((CONST char *)g_title, all, NULL, hits);
	free(all);
	snprintf(s, sizeof(s), "objects answered %d of %d", hits, n);
	note(s);
	*p_hits = hits;
	return rec;
}

/* ---------------------------------------------------------------- the question */

/* Asked in the panel, starting from the last: TRUE when 検索 was pressed, with the question in g_q */
static BOOL ask( void )
{
	UI	q;
	INT	how, i;

	if ( ta_dlg_open(SYSDEF_BOX_SYSTOOL, ST_PN_SEARCH, &q) < E_OK ) return FALSE;
	ui_set_text(&q, P_TEXT, (CONST char *)g_q.text);
	ui_set_text(&q, P_FROM, (CONST char *)g_q.from);
	ui_set_text(&q, P_TO, (CONST char *)g_q.to);
	for ( i = 0; i < 4; i++ ) {
		ui_set_value(&q, P_G_TARGET + i, ( i == g_q.target ) ? 1 : 0);
		ui_set_value(&q, P_G_DATE + i, ( i == g_q.date_kind ) ? 1 : 0);
	}
	ui_focus(&q, P_TEXT);
	note("search panel open");
	how = ta_dlg_run(&q, ST_P_YES);
	if ( how == ST_P_YES ) {
		(void)ui_text(&q, P_TEXT, (char *)g_q.text, sizeof(g_q.text));
		(void)ui_text(&q, P_FROM, (char *)g_q.from, sizeof(g_q.from));
		(void)ui_text(&q, P_TO, (char *)g_q.to, sizeof(g_q.to));
		for ( i = 0; i < 4; i++ ) {
			if ( ui_value(&q, P_G_TARGET + i) != 0 ) g_q.target = i;
			if ( ui_value(&q, P_G_DATE + i) != 0 ) g_q.date_kind = i;
		}
	}
	ta_dlg_close(&q);
	note(( how == ST_P_YES ) ? "search panel answered" : "search panel put away");
	return (BOOL)( how == ST_P_YES );
}

/* The question asked again in the open window, and what answered shown in it */
static void ask_again( void )
{
	T_TAD	*rec;
	INT	hits = 0;

	if ( !ask() ) return;
	rec = answer(&g_q, &hits);
	if ( rec == NULL ) return;
	if ( hits == 0 ) {
		tad_free(rec);
		st_tell("該当する実身はありませんでした。", NULL);
		return;
	}
	fg_list_set(rec);
	ta_title((CONST char *)g_title);
}

/* 再表示: the same question put again, whatever answers it now */
static void put_again( void )
{
	INT	hits = 0;
	T_TAD	*rec = answer(&g_q, &hits);

	if ( rec == NULL ) return;
	fg_list_set(rec);
	ta_title((CONST char *)g_title);
}

/* ---------------------------------------------------------------- what the cabinet asks of it */

/*
 * The question asked before the window opens: put away, or answered by
 * nothing, the tool ends with no window, as nothing was found
 */
static ER sr_start( T_OBREF *p_ref )
{
	INT	hits = 0;
	ER	er = st_start(&sr_host, SYSDEF_PROG_OBJSEARCH, p_ref);

	if ( er < E_OK ) return er;
	if ( !ask() ) {
		note("closed");
		ts_ext_prc(0);
	}
	g_first = answer(&g_q, &hits);
	if ( g_first != NULL && hits == 0 ) {
		st_tell("該当する実身はありませんでした。", NULL);
		note("closed");
		ts_ext_prc(0);
	}
	return ( g_first != NULL ) ? E_OK : E_NOMEM;
}

static void sr_begin( void )
{
	fg_as = FG_AS_TOOL;
}

static ER sr_load( T_TAD **p_rec )
{
	if ( g_first == NULL ) return E_NOMEM;
	*p_rec = g_first;
	g_first = NULL;
	return E_OK;
}

static void sr_listen( void )
{
	ta_listen("real-object-search");
}

static void sr_shown( void )
{
	if ( !g_up ) {
		g_up = TRUE;
		ta_title((CONST char *)g_title);
		note("up");
	}
}

/* Run again while open: the window is in front, and the question is asked again */
static void sr_told( CONST T_DTANS *an )
{
	if ( an->how != DT_HOW_TOOL_AGAIN ) return;
	note("run again");
	ask_again();
}

static BOOL sr_command( CONST char *code, CONST T_VOBJ *v )
{
	(void)v;
	if ( strcmp(code, "research") == 0 ) ask_again();
	else if ( strcmp(code, "refresh") == 0 ) put_again();
	else if ( strcmp(code, "cut") == 0 ) (void)fg_copy();	/* Ctrl+X: a copy, nothing leaves the list */
	else return FALSE;
	return TRUE;
}

static CONST T_FGBOX	sr_box = {
	.name = "実身/仮身検索", .start = sr_start, .begin = sr_begin, .load = sr_load, .listen = sr_listen,
	.shown = sr_shown, .command = sr_command, .menu_def = SYSDEF_MENU_SEARCH,
};

CONST T_FGBOX	*fg_box = &sr_box;
