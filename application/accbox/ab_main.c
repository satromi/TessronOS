/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ab_main.c
 *	小物箱 as a program running as a process (design 11.2.1)
 *
 *	The program is 仮身一覧's (application/fig with the cabinet's menu
 *	and ways) linked with this file, which says that it is the 小物箱
 *	(FG_ACCBOX). The window shows not the object's record but the
 *	accessories of the program box, in the order the desktop keeps
 *	(DT_RQ_ACC_LIST), laid out as the system's lists are (tvlist.h),
 *	those 小物 does not list in grey. Nothing is put in, taken out or saved (FG_MADE); the
 *	person orders them (小物管理, or carried within the window) and says
 *	whether 小物 lists them, by asking the desktop (DT_RQ_ACC_SET), which
 *	writes 小物の設定 and tells every 小物箱 to make its list again. Its
 *	windows have the 小物箱's icon, and the process the program's name.
 *	The desktop starts it on an object whose program is the 小物箱, and
 *	on the system's 小物箱 when the program is run by itself. What it
 *	does is said on the console for the tests ("accbox: ...").
 */

#include "../fig/fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/mn.h>

BOOL	fg_cabinet = TRUE;

#define ACC_MAX		256		/* accessories listed */

static void box_told( CONST T_DTANS *an );

static CONST T_TAHOST	ab_host = {
	.who = "accbox", .prog = SYSDEF_PROG_ACCBOX, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none, .told = box_told,
};

/* The accessories as the list was last made: the program objects, and whether 小物 lists each */
static TS_UUID	acc_id[ACC_MAX];
static BOOL	acc_show[ACC_MAX];
static INT	acc_n;

static ER ab_start( T_OBREF *p_ref )
{
	return ta_start(&ab_host, p_ref);
}

static void ab_begin( void )
{
	fg_as = FG_AS_ACCBOX;
}

/* ---------------------------------------------------------------- the 小物箱's list */

/* The record: one link to each accessory, in the desktop's order */
static ER acc_load( T_TAD **p_rec )
{
	T_DTREQ	rq;
	T_DTANS	an;
	T_TAD	*rec;
	INT	i, n = 1;
	char	s[48];
	ER	er = E_OK;
	static UB	*acc_name[ACC_MAX];	/* each as long as the desktop says it */
	static CONST UB	*names[ACC_MAX];
	static BOOL	dim[ACC_MAX];

	for ( i = 0; i < ACC_MAX; i++ ) {
		free(acc_name[i]);
		acc_name[i] = NULL;
	}
	acc_n = 0;
	for ( i = 0; i < n && i < ACC_MAX; i++ ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_ACC_LIST;
		rq.recno = i;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 5000);
		if ( er < E_OK ) break;
		n = (INT)an.how;
		{
			SZ		tl = 0;
			CONST UB	*t = ta_ans_part(DT_TAIL_TEXT, &tl);

			acc_name[acc_n] = (UB *)malloc((size_t)tl + 1);
			if ( acc_name[acc_n] == NULL ) {
				er = E_NOMEM;
				break;
			}
			if ( tl > 0 ) memcpy(acc_name[acc_n], t, (size_t)tl);
			acc_name[acc_n][tl] = 0;
		}
		acc_id[acc_n] = an.into;
		acc_show[acc_n] = (BOOL)( ( an.flags & DT_F_HIDDEN ) == 0 );
		names[acc_n] = acc_name[acc_n];
		dim[acc_n] = (BOOL)!acc_show[acc_n];
		acc_n++;
	}
	if ( er == E_NOEXS && acc_n == 0 ) er = E_OK;	/* no accessory at all: an empty box */
	if ( er < E_OK ) return er;
	rec = tl_record("小物箱", acc_id, names, dim, acc_n, &tl_rows, NULL);
	if ( rec == NULL ) return E_NOMEM;
	*p_rec = rec;
	snprintf(s, sizeof(s), "%d accessories", acc_n);
	note(s);
	return E_OK;
}

/* Told whenever the list changes, at the channel the desktop says on the links */
static void acc_listen( void )
{
	ta_listen("accessory-box");
}

/* The list changed: made again, scrolled as it was */
static void box_told( CONST T_DTANS *an )
{
	T_TAD	*rec = NULL;

	if ( an->how != DT_HOW_ACC || fg_holding() ) return;
	if ( acc_load(&rec) < E_OK ) return;
	fg_list_set(rec);
	note("list made again");
}

/* Where the accessory stands in the list, -1 not in it */
static INT acc_at( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; u != NULL && i < acc_n; i++ ) {
		if ( memcmp(&acc_id[i], u, sizeof(TS_UUID)) == 0 ) return i;
	}
	return -1;
}

/* 小物管理 as it stands for the link the menu was opened on, grey away from one */
static void acc_menu( ID mid, CONST T_VOBJ *v )
{
	TS_UUID	md;
	INT	at = ( v != NULL ) ? acc_at(&v->target) : -1;
	BOOL	show = (BOOL)( at >= 0 && acc_show[at] );

	if ( ts_str_to_uuid(SYSDEF_MENU_ACCMGR, &md) < E_OK ) return;
	(void)mn_chg_atr(mid, &md, "acc.top", ( at > 0 ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, &md, "acc.up", ( at > 0 ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, &md, "acc.down", ( at >= 0 && at < acc_n - 1 ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, &md, "acc.bottom", ( at >= 0 && at < acc_n - 1 ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, &md, "acc.show", ( at < 0 ) ? MN_GREY : show ? MN_HIDE : 0);
	(void)mn_chg_atr(mid, &md, "acc.hide", ( at < 0 || !show ) ? MN_HIDE : 0);
}

/* What the person changed, asked of the desktop: a code of 小物管理, or acc.place before another */
static void acc_set( CONST char *code, CONST TS_UUID *target, CONST TS_UUID *before )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[64];
	ER	er;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_ACC_SET;
	rq.target = *target;
	ta_req_part(DT_TAIL_NAME, (CONST UB *)code, -1);
	if ( before != NULL ) {
		char	t[TS_UUID_STRLEN + 1];

		ts_uuid_text(before, t);
		ta_req_part(DT_TAIL_TEXT, (CONST UB *)t, -1);
	}
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "%s (%d)", code, (INT)er);
	note(s);
}

static BOOL acc_command( CONST char *code, CONST T_VOBJ *v )
{
	if ( strncmp(code, "acc.", 4) != 0 ) return FALSE;
	if ( v != NULL ) acc_set(code, &v->target, NULL);
	return TRUE;
}

static void acc_place( CONST TS_UUID *target, CONST TS_UUID *before )
{
	acc_set("acc.place", target, before);
}

static CONST T_FGBOX	ab_box = {
	.name = "小物箱", .start = ab_start, .begin = ab_begin, .load = acc_load, .listen = acc_listen,
	.menu = acc_menu, .command = acc_command, .place = acc_place,
};

CONST T_FGBOX	*fg_box = &ab_box;
