/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	si_main.c
 *	システム登録: the system the PC was started from put on an internal
 *	disk, an accessory running as a process (docs/tessronos/
 *	03-platform.md 3.10.10, 11-desktop.md 11.32)
 *
 *	The window is a panel of the data box (SYSDEF_BOX_SYSINST). The
 *	frame moves, sizes and closes it, and the second button opens its
 *	menu -- 閉じる, 開始, the three ways, ディスクの一覧を更新, 再起動 and the
 *	tail of ウインドウ, 小物 and 実行. 〈写し先のディスク〉 lists the NVMe
 *	and SATA disks but the one the system runs from (USB disks are never
 *	offered), each with its size, its partitions and its largest free
 *	stretch. 〈登録の方法〉 is Windowsと共存する (the default), 区画を1つ消し
 *	て使う or ディスク全体を消して使う; for the second, 〈消す区画〉 lists the
 *	partitions of the disk that can go. 〈登録先にTessronOSがあるとき〉 is
 *	上書きする (the default: the objects there kept, the system's put on
 *	as a system update puts them) or 初期化する (the copy written over
 *	it, as before); the desktop is asked whether the TessronOS there can
 *	be kept, and when it cannot, 上書きする is greyed with why, and
 *	neither is chosen until the person chooses 初期化する. With no
 *	TessronOS there both are greyed. [開始] has the way looked at and
 *	says why when it cannot be taken; else it asks first -- with every
 *	partition that is to go and its size -- and the copy is done, for an
 *	administrator, the window following the steps and the bytes. At the
 *	end it says how the machine is started from the disk, and [再起動]
 *	starts it again. The window is not closed while the copy runs.
 *
 *	The disks, the plan and the copy are the kernel's: each is asked of
 *	the desktop (DT_RQ_SYSINST, ts/dtreq.h), which reads the disks,
 *	keeps the plan, and does it in a task of its own that runs to its end
 *	whatever this program does. On a machine that is not a PC there is
 *	nothing to put the system on, which is said, and the program ends.
 *
 *	Its window says it shows the program object, so that running the
 *	program again brings it forward. What it does is said on the console
 *	for the tests ("sysinst: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <tk/typedef.h>
#include <tk/syscall.h>
#include <ts/ui.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/dtreq.h>
#include "../dtask.h"

/* The words of the next request and the last answer, as long as they are (dtask.h) */
static DTASK	g_dq;

/* The text of the last answer (DT_TAIL_TEXT), "" when it has none */
#define ANS_TEXT	dq_ans_text(&g_dq, DT_TAIL_TEXT)

#define SW_W		620		/* the window's panel */
#define SW_H		724
#define SW_ROWS		9		/* lines of what is under way and what came of it */
#define SW_DISK_MAX	8
#define SW_PART_MAX	32
#define ROW_MAX		DT_TEXT_MAX
#define POLL_MS		200		/* how often a copy that runs is asked how it goes */

/* the panels and their parts */
#define PN_MAIN		1
#define PN_ASK		10		/* 登録 */
#define PN_ASK_WARN	11		/* 消して登録 */
#define P_DISK		1
#define P_PART		2
#define P_COEXIST	20
#define P_ERASE		21
#define P_REPLACE	22
#define P_KEEP		23		/* 上書きする */
#define P_INIT		24		/* 初期化する */
#define P_KWHY		25		/* whether it can be kept, and why not */
#define B_START		30
#define B_RESTART	31
#define B_END		32
#define P_LINE0		41
#define B_YES		101

#define SW_SETUP	0
#define SW_RUN		1
#define SW_DONE		2

static UI	sw;
static BOOL	g_quit = FALSE;

static char	sw_dname[SW_DISK_MAX][ROW_MAX];		/* the disks' device names */
static char	sw_drow[SW_DISK_MAX][ROW_MAX];		/* and their rows */
static char	*sw_drows[SW_DISK_MAX];
static INT	sw_ndisk = 0, sw_sel = 0;
static INT	sw_punit[SW_PART_MAX];			/* the partitions that can go: their subunits */
static char	sw_prow[SW_PART_MAX][ROW_MAX];
static char	*sw_prows[SW_PART_MAX];
static INT	sw_npart = 0, sw_psel = 0, sw_pdisk = -1;
static INT	sw_mode = DT_SI_COEXIST;
static INT	sw_keep = 1;				/* 1 上書きする, 0 初期化する, -1 neither chosen */
static INT	sw_kq = DT_SI_K_NONE;			/* whether it can be kept: 0, DT_SI_K_NONE, or why not */
static char	sw_ktext[ROW_MAX];
static INT	sw_kq_disk = -1, sw_kq_mode = -1, sw_kq_unit = -2;	/* what sw_kq was asked for */
static INT	sw_state = SW_SETUP;
static INT	sw_step = -1;				/* the step shown last */
static char	sw_line[SW_ROWS][ROW_MAX];

static void note( CONST char *s )
{
	char	b[240];

	snprintf(b, sizeof(b), "sysinst: %s\n", s);
	tm_putstring((CONST UB *)b);
}

/* ---------------------------------------------------------------- the desktop */

/* One request to the desktop and its answer, waited for at most wait_ms */
static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	memset(an, 0, sizeof(*an));
	rq->req = DT_RQ_SYSINST;
	return dq_ask(&g_dq, rq, an, &sw.win, wait_ms);
}

/* What op asks, with its numbers; the answer in *an */
static ER ask( UINT op, INT x, INT y, CONST char *name, T_DTANS *an, INT wait_ms )
{
	T_DTREQ	rq;

	memset(&rq, 0, sizeof(rq));
	rq.recno = (INT)op;
	rq.grab_x = x;
	rq.grab_y = y;
	if ( name != NULL ) (void)dq_part(&g_dq, DT_TAIL_NAME, (CONST UB *)name, -1);
	return ask_desktop(&rq, an, wait_ms);
}

/* ---------------------------------------------------------------- words */

static void line_set( INT i, CONST char *s )
{
	if ( i >= 0 && i < SW_ROWS ) snprintf(sw_line[i], sizeof(sw_line[i]), "%s", s);
}

static void lines_clear( void )
{
	INT	i;

	for ( i = 0; i < SW_ROWS; i++ ) sw_line[i][0] = 0;
}

/* "A<tab>B": A into a, B into b */
static void split( CONST UB *t, char *a, INT na, char *b, INT nb )
{
	INT	i = 0, k = 0;

	while ( t[i] != 0 && t[i] != '\t' ) {
		if ( k < na - 1 ) a[k++] = (char)t[i];
		i++;
	}
	a[k] = 0;
	if ( t[i] == '\t' ) i++;
	snprintf(b, (size_t)nb, "%s", (CONST char *)t + i);
}

/* The lines of what grab_x of DT_SI_LINE says, from the first */
static void lines_take( INT which )
{
	T_DTANS	an;
	INT	i, n = SW_ROWS;

	lines_clear();
	for ( i = 0; i < n && i < SW_ROWS; i++ ) {
		if ( ask(DT_SI_LINE, which, i, NULL, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		line_set(i, (CONST char *)ANS_TEXT);
	}
}

static void setup_lines( void )
{
	lines_clear();
	line_set(0, "写し先のディスクと登録の方法を選んで、[開始]を押してください。");
	line_set(1, "いま動いているシステムボリューム(実身、設定、入れたアプリ)と、");
	line_set(2, "起動したEFIシステム区画のローダ、カーネル、起動の設定を写します。");
	line_set(3, "起動したディスクとUSBのディスクは、写し先に出しません。");
	line_set(5, "BitLockerやデバイスの暗号化を使っているときは、回復キーを控えてください。");
	line_set(6, "セキュアブートを切るとWindowsは回復キーを求めます(EFIシステム区画に");
	line_set(7, "ファイルを足すことは差し支えありません)。");
	if ( sw_ndisk == 0 ) line_set(4, "写し先にできる内蔵のディスクがありません。");
}

/* ---------------------------------------------------------------- the lists */

/* The partitions of the disk chosen that can go, the one offered first chosen */
static void list_parts( void )
{
	T_DTANS	an;
	char	unit[16];
	INT	i, n = SW_PART_MAX;

	sw_npart = 0;
	sw_psel = 0;
	sw_pdisk = sw_sel;
	for ( i = 0; sw_sel < sw_ndisk && i < n && i < SW_PART_MAX; i++ ) {
		if ( ask(DT_SI_PART, sw_sel, i, NULL, &an, 5000) < E_OK ) break;
		n = (INT)an.how;
		split(ANS_TEXT, unit, sizeof(unit), sw_prow[i], ROW_MAX);
		sw_punit[i] = atoi(unit);
		sw_prows[i] = sw_prow[i];
		if ( ( an.flags & DT_F_SI_BEST ) != 0 ) sw_psel = i;
		sw_npart++;
	}
	ui_names(&sw, P_PART, (CONST char *const *)sw_prows, sw_npart);
	ui_set_value(&sw, P_PART, ( sw_npart > 0 ) ? sw_psel + 1 : 0);
}

/* The disks the system may be put on, read again; FALSE on a machine that is not a PC */
static BOOL list_disks( void )
{
	T_DTANS	an;
	INT	i, n = SW_DISK_MAX;
	ER	er = E_OK;
	char	b[32];

	sw_ndisk = 0;
	for ( i = 0; i < n && i < SW_DISK_MAX; i++ ) {
		er = ask(DT_SI_DISK, i, 0, NULL, &an, 30000);
		if ( er < E_OK ) break;
		n = (INT)an.how;
		split(ANS_TEXT, sw_dname[i], ROW_MAX, sw_drow[i], ROW_MAX);
		sw_drows[i] = sw_drow[i];
		sw_ndisk++;
	}
	if ( er == E_NOSPT ) return FALSE;
	if ( sw_sel >= sw_ndisk ) sw_sel = 0;
	ui_names(&sw, P_DISK, (CONST char *const *)sw_drows, sw_ndisk);
	ui_set_value(&sw, P_DISK, ( sw_ndisk > 0 ) ? sw_sel + 1 : 0);
	list_parts();
	snprintf(b, sizeof(b), "%d disks", sw_ndisk);
	note(b);
	return TRUE;
}

/* ---------------------------------------------------------------- the window */

/* What was chosen in the panel, kept; 〈消す区画〉 follows the disk */
static void keep_choices( void )
{
	INT	i;

	if ( sw_state == SW_RUN ) return;
	i = ui_value(&sw, P_DISK);
	if ( i >= 1 && i <= sw_ndisk ) sw_sel = i - 1;
	if ( sw_sel != sw_pdisk ) list_parts();
	i = ui_value(&sw, P_PART);
	if ( i >= 1 && i <= sw_npart ) sw_psel = i - 1;
	sw_mode = ( ui_value(&sw, P_ERASE) != 0 ) ? DT_SI_ERASE
		: ( ui_value(&sw, P_REPLACE) != 0 ) ? DT_SI_REPLACE : DT_SI_COEXIST;
	if ( ui_value(&sw, P_KEEP) != 0 && sw_kq == 0 ) sw_keep = 1;
	else if ( ui_value(&sw, P_INIT) != 0 ) sw_keep = 0;
}

/*
 * Whether the TessronOS the way chosen writes over can be kept, asked of
 * the desktop when the disk, the way or the partition is another than
 * when it was asked last. 上書きする that cannot be taken is not left
 * chosen, nor is 初期化する chosen for the person.
 */
static void keep_refresh( BOOL again )
{
	T_DTANS	an;
	INT	unit = ( sw_mode == DT_SI_REPLACE && sw_psel < sw_npart ) ? sw_punit[sw_psel] : -1;
	char	b[96];

	if ( sw_ndisk == 0 ) {
		sw_kq = DT_SI_K_NONE;
		sw_ktext[0] = 0;
		return;
	}
	if ( !again && sw_kq_disk == sw_sel && sw_kq_mode == sw_mode && sw_kq_unit == unit ) return;
	sw_kq_disk = sw_sel;
	sw_kq_mode = sw_mode;
	sw_kq_unit = unit;
	sw_ktext[0] = 0;
	if ( ask(DT_SI_KEEPQ, sw_mode, unit, sw_dname[sw_sel], &an, 60000) < E_OK ) {
		sw_kq = DT_SI_K_NONE;
	} else {
		sw_kq = (INT)an.how;
		snprintf(sw_ktext, sizeof(sw_ktext), "%s", (CONST char *)ANS_TEXT);
	}
	if ( sw_kq != 0 && sw_kq != DT_SI_K_NONE && sw_keep == 1 ) sw_keep = -1;
	if ( sw_kq == 0 && sw_keep < 0 ) sw_keep = 1;
	snprintf(b, sizeof(b), "keep %s %d %d: %d", sw_dname[sw_sel], sw_mode, unit, sw_kq);
	note(b);
}

/* The window's parts set from what it shows now */
static void show( void )
{
	BOOL	busy = (BOOL)( sw_state != SW_SETUP );
	INT	i;

	ui_set_value(&sw, P_COEXIST, ( sw_mode == DT_SI_COEXIST ) ? 1 : 0);
	ui_set_value(&sw, P_REPLACE, ( sw_mode == DT_SI_REPLACE ) ? 1 : 0);
	ui_set_value(&sw, P_ERASE, ( sw_mode == DT_SI_ERASE ) ? 1 : 0);
	ui_off(&sw, P_DISK, busy);
	ui_off(&sw, P_COEXIST, busy);
	ui_off(&sw, P_REPLACE, busy);
	ui_off(&sw, P_ERASE, busy);
	ui_off(&sw, P_PART, (BOOL)( busy || sw_mode != DT_SI_REPLACE ));
	ui_set_value(&sw, P_KEEP, ( sw_kq != DT_SI_K_NONE && sw_keep == 1 ) ? 1 : 0);
	ui_set_value(&sw, P_INIT, ( sw_kq != DT_SI_K_NONE && sw_keep == 0 ) ? 1 : 0);
	ui_off(&sw, P_KEEP, (BOOL)( busy || sw_kq != 0 ));
	ui_off(&sw, P_INIT, (BOOL)( busy || sw_kq == DT_SI_K_NONE ));
	ui_set_text(&sw, P_KWHY, ( sw_kq == 0 ) ? "上書きできます。登録先の実身・仮身は残ります。"
		    : ( sw_kq == DT_SI_K_NONE ) ? ( ( sw_mode == DT_SI_ERASE ) ? "ディスク全体を消すので、上書きはできません。"
						    : "登録先にTessronOSの区画はありません。新しく写します。" )
		    : ( sw_ktext[0] != 0 ) ? sw_ktext : "上書きはできません。");
	ui_off(&sw, B_START, (BOOL)( busy || sw_ndisk == 0 ));
	ui_off(&sw, B_RESTART, (BOOL)( sw_state != SW_DONE || sw_line[0][0] == 0 || strncmp(sw_line[0], "登録が終わりました", 27) != 0 ));
	ui_off(&sw, B_END, (BOOL)( sw_state == SW_RUN ));
	for ( i = 0; i < SW_ROWS; i++ ) {
		ui_set_text(&sw, P_LINE0 + i, ( sw_line[i][0] != 0 ) ? sw_line[i] : " ");
	}
}

/* Whether the copy went well: 再起動 is for then only */
static BOOL went_well( void )
{
	T_DTANS	an;

	return (BOOL)( ask(DT_SI_STATE, 0, 0, NULL, &an, 3000) >= E_OK && ( an.flags & DT_F_SI_OVER ) != 0
		       && (INT)an.look.w >= 0 );
}

/* ---------------------------------------------------------------- starting */

static void restart( void )
{
	note("restart");
	if ( !went_well() || ask(DT_SI_RESTART, 0, 0, NULL, &(T_DTANS){ 0 }, 5000) < E_OK ) {
		(void)wm_msg_put("システム登録: 再起動できませんでした");
	}
}

/* The question before the plan kept is done, a line to a row: TRUE for the button that agrees */
static BOOL confirm( INT nline, BOOL warn )
{
	T_DTANS		an;
	static char	l[DT_SI_ROWS][ROW_MAX];
	CONST char	*lines[DT_SI_ROWS];
	INT		i, a, n = 0;
	char		b[64];

	for ( i = 0; i < nline && i < DT_SI_ROWS; i++ ) {
		if ( ask(DT_SI_LINE, DT_SI_L_ASK, i, NULL, &an, 3000) < E_OK ) break;
		snprintf(l[i], sizeof(l[i]), "%s", (CONST char *)ANS_TEXT);
		lines[i] = l[i];
		n++;
	}
	for ( i = n; i < DT_SI_ROWS; i++ ) lines[i] = " ";
	snprintf(b, sizeof(b), "ask %s", warn ? "消して登録" : "登録");
	note(b);
	a = ui_ask(&sw, warn ? PN_ASK_WARN : PN_ASK, lines, DT_SI_ROWS);
	snprintf(b, sizeof(b), "answered %d", ( a == B_YES ) ? 1 : 0);
	note(b);
	return (BOOL)( a == B_YES );
}

static void start( void )
{
	T_DTANS	an;
	INT	unit, way;
	ER	er;
	char	b[96];

	keep_choices();
	if ( sw_state == SW_RUN || sw_ndisk == 0 ) return;
	keep_refresh(FALSE);
	if ( sw_kq != DT_SI_K_NONE && sw_keep < 0 ) {
		lines_clear();
		line_set(0, "登録先のTessronOSを上書きするか初期化するかを選んでください。");
		if ( sw_kq != 0 ) line_set(1, sw_ktext);
		show();
		return;
	}
	unit = ( sw_mode == DT_SI_REPLACE && sw_psel < sw_npart ) ? sw_punit[sw_psel] : -1;
	way = sw_mode | ( ( sw_kq != DT_SI_K_NONE && sw_keep == 1 ) ? DT_SI_KEEP : 0 );
	er = ask(DT_SI_PLAN, way, unit, sw_dname[sw_sel], &an, 60000);
	snprintf(b, sizeof(b), "plan %s %d %d: %d", sw_dname[sw_sel], way, unit, (INT)( ( er < E_OK ) ? er : (ER)an.how ));
	note(b);
	if ( er < E_OK ) {
		(void)wm_msg_put("システム登録: ディスクを調べられませんでした");
		return;
	}
	if ( an.how != 0 ) {
		lines_clear();
		line_set(0, (CONST char *)ANS_TEXT);
		show();
		return;
	}
	if ( !confirm(an.look.w, (BOOL)( an.look.h != 0 )) ) return;
	/* the administrator's rights: the desktop asks for the password when the program has them not */
	er = ask(DT_SI_RUN, 0, 0, NULL, &an, 10 * 60 * 1000);
	snprintf(b, sizeof(b), "run %d", (INT)er);
	note(b);
	if ( er < E_OK ) {
		if ( er == E_OACV ) ts_no_right("システム登録を");
		else if ( er != E_ABORT ) (void)wm_msg_put("システム登録: 始められませんでした");
		return;
	}
	sw_state = SW_RUN;
	sw_step = -1;
	snprintf(b, sizeof(b), "started %s", sw_dname[sw_sel]);
	note(b);
	lines_take(DT_SI_L_RUN);
	show();
}

/* The copy followed: the lines again when its step or its bytes change, and what came of it at the end */
static void follow( void )
{
	T_DTANS	an;
	char	b[32];

	if ( sw_state != SW_RUN ) return;
	if ( ask(DT_SI_STATE, 0, 0, NULL, &an, 3000) < E_OK ) return;
	if ( ( an.flags & DT_F_SI_OVER ) != 0 ) {
		sw_state = SW_DONE;
		lines_take(DT_SI_L_RESULT);
		show();
		snprintf(b, sizeof(b), "over %d", (INT)an.look.w);
		note(b);
		return;
	}
	(void)sw_step;
	sw_step = (INT)an.how;
	lines_take(DT_SI_L_RUN);
	show();
}

/* ---------------------------------------------------------------- the menu */

static BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

static BOOL closing( void )
{
	if ( sw_state == SW_RUN ) {
		(void)wm_msg_put("システム登録: 登録が終わるまで閉じられません");
		return FALSE;
	}
	return TRUE;
}

static void menu_set( ID mid )
{
	BOOL	run = (BOOL)( sw_state == SW_RUN );

	keep_choices();
	(void)mn_chg_atr(mid, NULL, "sysinst.start", ( sw_state != SW_SETUP || sw_ndisk == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.coexist", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == DT_SI_COEXIST ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.replace", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == DT_SI_REPLACE ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.erase", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == DT_SI_ERASE ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.keep", ( sw_state != SW_SETUP || sw_kq != 0 ) ? MN_GREY
			 : ( sw_keep == 1 ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.init", ( sw_state != SW_SETUP || sw_kq == DT_SI_K_NONE ) ? MN_GREY
			 : ( sw_keep == 0 ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.list", run ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.restart", ( sw_state != SW_DONE || !went_well() ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "close", run ? MN_GREY : 0);
}

static void menu_do( CONST T_MNSEL *sel )
{
	if ( is_code(sel, "close") ) {
		if ( closing() ) g_quit = TRUE;
		return;
	}
	if ( is_code(sel, "sysinst.start") ) {
		start();
		return;
	}
	if ( is_code(sel, "sysinst.restart") ) {
		if ( sw_state == SW_DONE ) restart();
		return;
	}
	if ( sw_state == SW_RUN ) return;
	if ( is_code(sel, "sysinst.coexist") ) sw_mode = DT_SI_COEXIST;
	else if ( is_code(sel, "sysinst.replace") ) sw_mode = DT_SI_REPLACE;
	else if ( is_code(sel, "sysinst.erase") ) sw_mode = DT_SI_ERASE;
	else if ( is_code(sel, "sysinst.keep") ) {
		if ( sw_kq == 0 ) sw_keep = 1;
	} else if ( is_code(sel, "sysinst.init") ) {
		if ( sw_kq != DT_SI_K_NONE ) sw_keep = 0;
	}
	else if ( is_code(sel, "sysinst.list") ) {
		(void)list_disks();
		sw_state = SW_SETUP;
		setup_lines();
		keep_refresh(TRUE);
	} else return;
	keep_refresh(FALSE);
	show();
}

/* The menu, at the press */
static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(SYSDEF_MENU_SYSINST, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	menu_set(mid);
	chosen = (BOOL)( mn_pop_men(mid, sw.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) menu_do(&sel);
}

/* A letter of the menu with Ctrl: 閉じる's E is the only one its items have */
static void menu_key( UB c )
{
	if ( ( c == 'e' || c == 'E' ) && closing() ) g_quit = TRUE;
}

/* ---------------------------------------------------------------- the window's notices */

static void event( CONST T_OBNTM *m )
{
	INT	num;

	switch ( ui_event(&sw, m, &num) ) {
	case UI_EV_CLOSE:
		if ( closing() ) g_quit = TRUE;
		break;
	case UI_EV_MENU:
		menu(m);
		break;
	case UI_EV_KEY:
		if ( ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) menu_key(wm_key_char(m->code, 0));
		break;
	case UI_EV_BUTTON:
		if ( num == B_START ) start();
		else if ( num == B_RESTART ) restart();
		else if ( num == B_END && closing() ) g_quit = TRUE;
		break;
	case UI_EV_CHANGE:
		/* another disk or way: 〈消す区画〉 and whether the TessronOS there can be kept follow */
		keep_choices();
		keep_refresh(FALSE);
		show();
		break;
	default:
		break;
	}
}

int main( void )
{
	T_OBNTM	m;
	T_DTANS	an;
	INT	wx = 0, wy = 0;
	char	b[32];

	/* a copy that runs, from before this program was started: followed */
	if ( ask_desktop(&(T_DTREQ){ .recno = DT_SI_STATE }, &an, 3000) == E_NOSPT ) {
		(void)wm_msg_put("システム登録はx64のPCだけで使えます");
		note("not a PC");
		return 0;
	}
	wm_new_centre(SW_W + 24, SW_H + 40, &wx, &wy);	/* the frame round it, on the screen new windows open on */
	ui_icon(SYSDEF_PROG_SYSINST);
	ui_shows(SYSDEF_PROG_SYSINST);
	ui_attr(UI_ATTR_HIDDEN);
	if ( ui_open(&sw, SYSDEF_BOX_SYSINST, PN_MAIN, wx, wy) < E_OK ) {
		note("no window");
		return 1;
	}
	if ( ( an.flags & DT_F_SI_RUN ) != 0 ) {
		sw_state = SW_RUN;
		lines_take(DT_SI_L_RUN);
	} else {
		(void)list_disks();
		setup_lines();
		keep_refresh(TRUE);
	}
	show();
	ui_reveal(&sw);
	snprintf(b, sizeof(b), "opened %d", sw_ndisk);
	note(b);
	while ( !g_quit ) {
		/* while the copy runs, how it goes is asked a few times a second */
		ER	er = ui_wait(&sw, &m, ( sw_state == SW_RUN ) ? POLL_MS : TMO_FEVR);

		if ( er >= E_OK ) event(&m);
		follow();
	}
	ui_close(&sw);
	note("closed");
	return 0;
}
