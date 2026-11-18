/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sp_main.c
 *	画面印刷, an accessory running as a process (design 11.24)
 *
 *	Chosen from 小物, it shows the panel 画面印刷／画面保存 of its data
 *	box (SYSDEF_BOX_SCRPRINT): where the picture goes (図形実身へ保存,
 *	トレーへ複写, プリンタへ印刷), whether the pointer is in it
 *	(ポインタ印刷), what is taken (全画面, 最前面ウィンドウのみ, 範囲指定)
 *	and when (so many seconds after 実行, at the PrintScreen key, or at
 *	every press of it). For a figure object it asks the volume it goes
 *	on and its name next, in the panel made for as many volumes as are
 *	attached.
 *
 *	The screen is the desktop's to read, and so is every input while a
 *	rectangle is dragged out or PrintScreen is waited for: the program
 *	asks the desktop (DT_RQ_SCRPRINT) for what was set last and for the
 *	volumes, and hands over what 実行 set; the desktop does the rest and
 *	the program ends. 取り消し says so to the desktop, which stops what
 *	was under way. Chosen again while its panel is up, the panel comes
 *	to the front instead of a second one. What it does is said on the
 *	console for the tests ("scrprint: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "../dtask.h"

/* the panels of the data box */
#define PN_SET		1		/* 画面印刷／画面保存 */
#define PN_NAME		100		/* 100 + n: 保存先ディスク and 実身名, for n volumes */
#define SP_VOLS		12		/* volumes offered: as many as a panel holds on the screen */

/* 画面印刷／画面保存's parts */
#define P_FIG		1
#define P_TRAY		2
#define P_PRINTER	3
#define P_POINTER	4
#define P_SCREEN	5
#define P_FRONT		6
#define P_RANGE		7
#define P_TIME		8
#define P_KEY		9
#define P_KEYS		10
#define P_SECS		11
#define P_CANCEL	12
#define P_OK		13

/* 保存先ディスク and 実身名's */
#define P_NAME		12
#define P_VOL		20
#define P_NCANCEL	90
#define P_NOK		91

#define SC_SEC_MIN	3
#define SC_SEC_MAX	99
#define SC_VOL_PATH	64

typedef struct {
	INT	to;			/* 0 図形実身へ保存, 1 トレーへ複写, 2 プリンタへ印刷 */
	INT	pointer;
	INT	area;			/* 0 全画面, 1 最前面ウィンドウのみ, 2 範囲指定 */
	INT	by;			/* 0 指定時間後, 1 PrntScrnキー, 2 連続 */
	INT	secs;
} SPSET;

static UI	g;
static SPSET	sc;
static char	vol[SC_VOL_PATH];
static char	name[256];

static void say( const char *fmt, ... )
{
	char	s[320];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	tm_putstring((CONST UB *)s);
}

/* The words of the next request and the last answer, as long as they are (dtask.h) */
static DTASK	g_dq;

static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	rq->req = DT_RQ_SCRPRINT;
	return dq_ask(&g_dq, rq, an, NULL, wait_ms);
}

/* What is set, as the desktop reads it: "to pointer area by secs<tab>volume<tab>name" */
static void set_text( char *out, size_t max )
{
	snprintf(out, max, "%d %d %d %d %d\t%s\t%s", sc.to, sc.pointer, sc.area, sc.by, sc.secs, vol, name);
}

static ER step( INT what, T_DTANS *an, BOOL with_set )
{
	T_DTREQ	rq;

	memset(&rq, 0, sizeof(rq));
	rq.recno = what;
	if ( with_set ) {
		size_t	max = strlen(vol) + strlen(name) + 64;
		char	*t = (char *)malloc(max);

		if ( t == NULL ) return E_NOMEM;
		set_text(t, max);
		(void)dq_part(&g_dq, DT_TAIL_TEXT, (CONST UB *)t, -1);
		free(t);
	}
	memset(an, 0, sizeof(*an));
	return ask_desktop(&rq, an, 10000);
}

/* A panel worked until one of its buttons or Esc answers it: the button, 0 for Esc */
static INT panel_wait( UI *q )
{
	T_OBNTM	m;
	INT	num, ev;

	for ( ;; ) {
		if ( ui_wait(&g, &m, TMO_FEVR) < E_OK ) return 0;
		ev = ui_dlg_take(q, &m, &num);
		if ( ev == UI_EV_CLOSE ) return 0;
		if ( ev == UI_EV_BUTTON ) return num;
	}
}

/* 画面印刷／画面保存: TRUE for 実行, what was set in sc */
static BOOL set_panel( INT told )
{
	UI	q;
	INT	b, v = sc.secs;

	if ( ui_dlg_open(&g, PN_SET, &q) < E_OK ) {
		say("scrprint: no panel\n");
		return FALSE;
	}
	ui_set_value(&q, P_FIG, sc.to == 0);
	ui_set_value(&q, P_TRAY, sc.to == 1);
	ui_set_value(&q, P_PRINTER, sc.to == 2);
	ui_set_value(&q, P_POINTER, sc.pointer != 0);
	ui_set_value(&q, P_SCREEN, sc.area == 0);
	ui_set_value(&q, P_FRONT, sc.area == 1);
	ui_set_value(&q, P_RANGE, sc.area == 2);
	ui_set_value(&q, P_TIME, sc.by == 0);
	ui_set_value(&q, P_KEY, sc.by == 1);
	ui_set_value(&q, P_KEYS, sc.by == 2);
	ui_set_fields(&q, P_SECS, &v, 1);
	say("scrprint: panel %d\n", told);

	b = panel_wait(&q);
	if ( b == P_OK ) {
		sc.to = ( ui_value(&q, P_TRAY) != 0 ) ? 1 : ( ui_value(&q, P_PRINTER) != 0 ) ? 2 : 0;
		sc.pointer = ( ui_value(&q, P_POINTER) != 0 ) ? 1 : 0;
		sc.area = ( ui_value(&q, P_FRONT) != 0 ) ? 1 : ( ui_value(&q, P_RANGE) != 0 ) ? 2 : 0;
		sc.by = ( ui_value(&q, P_KEY) != 0 ) ? 1 : ( ui_value(&q, P_KEYS) != 0 ) ? 2 : 0;
		if ( ui_fields(&q, P_SECS, &v, 1) >= 1 ) {
			sc.secs = ( v < SC_SEC_MIN ) ? SC_SEC_MIN : ( v > SC_SEC_MAX ) ? SC_SEC_MAX : v;
		}
	}
	ui_dlg_close(&q);
	return (BOOL)( b == P_OK );
}

/*
 * 保存先ディスク and 実身名: the volumes attached, one each (the first,
 * the system's, marked so), the one chosen last time chosen again, and
 * the name. TRUE for 保存.
 */
static BOOL name_panel( void )
{
	static char	vols[SP_VOLS][SC_VOL_PATH];
	char		labels[SP_VOLS][SC_VOL_PATH + 80];
	const char	*np[SP_VOLS];
	char		t[256];
	T_DTREQ		rq;
	T_DTANS		an;
	UI		q;
	INT		n = 0, i, at = 0, b, k;

	for ( i = 0; i < SP_VOLS; i++ ) {
		memset(&rq, 0, sizeof(rq));
		rq.recno = DT_SP_VOL;
		rq.grab_x = i;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 10000) < E_OK ) break;
		{
			CONST char	*at = (CONST char *)dq_ans_text(&g_dq, DT_TAIL_TEXT);
			CONST char	*tab = strchr(at, '\t');
			CONST char	*label = ( tab != NULL ) ? tab + 1 : "";
			INT		vl = ( tab != NULL ) ? (INT)( tab - at ) : (INT)strlen(at);

			snprintf(vols[n], sizeof(vols[n]), "%.*s", vl, at);
			if ( n == 0 ) {
				snprintf(labels[n], sizeof(labels[n]), "%s（システム）", vols[n]);
			} else if ( label[0] != 0 ) {
				/* a disk attached with ディスク操作: its name */
				snprintf(labels[n], sizeof(labels[n]), "%s「%s」", vols[n], label);
			} else {
				snprintf(labels[n], sizeof(labels[n]), "%s", vols[n]);
			}
		}
		if ( strcmp(vols[n], vol) == 0 ) at = n;
		np[n] = labels[n];
		n++;
	}
	if ( ui_dlg_open(&g, PN_NAME + n, &q) < E_OK ) {
		say("scrprint: no panel\n");
		return FALSE;
	}
	if ( n > 0 ) {
		ui_names(&q, P_VOL, np, n);
		ui_set_value(&q, P_VOL, at + 1);
	}
	ui_set_text(&q, P_NAME, name);
	ui_focus(&q, P_NAME);
	say("scrprint: volumes %d\n", n);

	b = panel_wait(&q);
	if ( b == P_NOK ) {
		if ( ui_text(&q, P_NAME, t, sizeof(t)) > 0 ) {
			for ( k = 0; t[k] == ' '; k++ ) ;
			if ( t[k] != 0 ) snprintf(name, sizeof(name), "%s", t + k);
		}
		vol[0] = 0;
		i = ( n > 0 ) ? ui_value(&q, P_VOL) : 0;
		if ( i >= 1 && i <= n ) snprintf(vol, sizeof(vol), "%s", vols[i - 1]);
		say("scrprint: save to %s\n", ( vol[0] != 0 ) ? vol : "-");
	}
	ui_dlg_close(&q);
	return (BOOL)( b == P_NOK );
}

int main( void )
{
	T_DTANS	an;
	T_OBCRE	c;
	TS_UUID	ch;
	char	*t1, *t2, *tx;
	INT	told;
	ER	er;

	er = step(DT_SP_GET, &an, FALSE);
	if ( er == E_BUSY ) {
		say("scrprint: the panel up already: brought to the front\n");
		return 0;
	}
	if ( er < E_OK ) {
		say("scrprint: the desktop did not answer (%d)\n", (INT)er);
		return 1;
	}
	told = (INT)an.how;
	tx = dq_ans_dup(&g_dq, DT_TAIL_TEXT);
	if ( tx == NULL ) {
		say("scrprint: no memory\n");
		return 1;
	}
	if ( sscanf(tx, "%d %d %d %d %d", &sc.to, &sc.pointer, &sc.area, &sc.by, &sc.secs) != 5 ) {
		sc.to = sc.pointer = sc.area = sc.by = 0;
		sc.secs = 5;
	}
	t1 = strchr(tx, '\t');
	t2 = ( t1 != NULL ) ? strchr(t1 + 1, '\t') : NULL;
	if ( t1 != NULL && t2 != NULL ) {
		*t2 = 0;
		snprintf(vol, sizeof(vol), "%s", t1 + 1);
		snprintf(name, sizeof(name), "%s", t2 + 1);
	} else {
		snprintf(name, sizeof(name), "画面印刷");
	}
	free(tx);

	memset(&g, 0, sizeof(g));
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ui_uuid(SYSDEF_BOX_SCRPRINT, &g.box) < E_OK || ob_cre_obj(&c, &ch) < E_OK
	  || ( g.port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) ) <= 0 ) {
		say("scrprint: no channel\n");
		(void)step(DT_SP_STOP, &an, FALSE);
		return 1;
	}
	ui_icon(SYSDEF_PROG_SCRPRINT);

	if ( !set_panel(told) ) {
		(void)step(DT_SP_STOP, &an, FALSE);
	} else if ( sc.to == 0 && !name_panel() ) {
		/* what the first panel set is kept all the same */
		(void)step(DT_SP_STOP, &an, TRUE);
	} else {
		er = step(DT_SP_ARM, &an, TRUE);
		if ( er < E_OK ) say("scrprint: not armed (%d)\n", (INT)er);
	}
	ob_cls_obj(g.port);
	(void)ob_del_obj(&ch);
	return 0;
}
