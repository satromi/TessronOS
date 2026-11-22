/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dop_main.c
 *	ディスク操作: the program that does what the desktop's menu
 *	ディスク操作 offers (design 11.26)
 *
 *	The desktop starts it with the item chosen and the link it was
 *	chosen on (T_DTDISKARG of ts/dtreq.h): 接続, 切り離し, 状態表示,
 *	区画作成, 拡張 or 形式を新しくする. Run by itself, from the program box, it is 状態表示 of
 *	the system's disk. It does that one thing and ends.
 *
 *	The disks, the volumes and the windows open on them are the
 *	desktop's, so each step is asked of it (DT_RQ_DISK): the rows of
 *	接続 and 区画作成, how full a disk is and how far it can grow come
 *	back in the answers, and what changes a volume -- attaching it,
 *	taking it away, making a partition, making one larger -- is done by
 *	the desktop for this program, which has the administrator's rights
 *	for it only once the person has typed the administrator's password in
 *	the desktop's panel. The panels are this program's own, from its data
 *	box (SYSDEF_BOX_DISKOP), in windows of their own in the middle of the
 *	screen; what cannot be done is said in a panel, and what was done on
 *	the message line, as the desktop said them when it did this itself.
 *	Its window says it shows the program object, so that the menu chosen
 *	again while it is up brings it forward rather than starting another.
 *
 *	What it does is said on the console for the tests ("diskop: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/dtreq.h>
#include <ts/tsfsck.h>
#include "../dtask.h"

/* The panels of the data box */
#define PN_CONNECT	1
#define PN_STATUS	2
#define PN_CREATE	3
#define PN_GROW		4
#define PN_CONVERT	5
#define PN_TELL		10

/* Their parts */
#define DK_LIST		10		/* 接続, 区画作成: the rows */
#define DK_HEAD		1		/* 状態表示, 拡張: the line at the top */
#define DK_VALUE		11		/* 状態表示: the six values, 11 to 16 */
#define DK_NOW		2		/* 拡張: the size now */
#define DK_MOST		3		/* 拡張: the most it can be */
#define DK_SIZE		20		/* 区画作成, 拡張: megabytes */
#define DK_LABEL		21		/* 区画作成: the label */
#define B_OK		101
#define B_CANCEL	102

#define PW_CONNECT	480		/* the panels' sizes, for the middle of the screen */
#define PH_CONNECT	230
#define PW_STATUS	300
#define PH_STATUS	206
#define PW_CREATE	560
#define PH_CREATE	302
#define PW_GROW		480
#define PH_GROW		206
#define PW_CONVERT	560
#define PH_CONVERT	150

#define KEY_ENTER	0x28
#define KEY_ESCAPE	0x29

#define ROWS_MAX	64		/* rows of 接続 and 区画作成 */
#define DK_MIN_MB	8		/* the smallest partition made */

static UI	g_base;			/* the channel, and the data box, the panels are worked through */
static DTASK	g_dq;			/* the words of the requests and the answers (dtask.h) */
/*
 * The texts of the last two answers, each in memory of its own to be
 * parted into its fields: what one answer said stays while the next is
 * asked (拡張 says the disk's name after it asks to grow it)
 */
static char	*g_text[2];
static INT	g_ti = 0;
#define ANS_TEXT	( ( g_text[g_ti] != NULL ) ? g_text[g_ti] : (char *)"" )
static TS_UUID	g_win;			/* the window up now, for the desktop's panels to give the keys back to */

static void note( CONST char *s )
{
	char	b[200];

	snprintf(b, sizeof(b), "diskop: %s\n", s);
	tm_putstring((CONST UB *)b);
}

/* ---------------------------------------------------------------- asking the desktop */

static ER ask( T_DTREQ *rq, T_DTANS *an )
{
	ER	er;

	rq->req = DT_RQ_DISK;
	memset(an, 0, sizeof(*an));
	/* the answer waited for: the person may be typing the administrator's password */
	er = dq_ask(&g_dq, rq, an, &g_win, 10 * 60 * 1000);
	g_ti ^= 1;
	free(g_text[g_ti]);
	g_text[g_ti] = dq_ans_dup(&g_dq, DT_TAIL_TEXT);
	if ( g_text[g_ti] == NULL && er >= E_OK ) er = E_NOMEM;
	return er;
}

static void req_init( T_DTREQ *rq, INT what, CONST TS_UUID *target )
{
	memset(rq, 0, sizeof(*rq));
	rq->recno = what;
	if ( target != NULL ) rq->target = *target;
}

/* A field of an answer's text, the tab after it passed */
static char *field( char **p )
{
	char	*s = *p, *t = strchr(s, '\t');

	if ( t != NULL ) {
		*t = 0;
		*p = t + 1;
	} else {
		*p = s + strlen(s);
	}
	return s;
}

/* ---------------------------------------------------------------- panels */

/* A panel of the data box in a window of its own, in the middle of the screen */
static ER panel_open( UI *w, INT num, INT pw, INT ph )
{
	INT	x, y;
	ER	er;

	wm_new_centre(pw + 16, ph + 40, &x, &y);	/* the frame round it, on the screen new windows open on */
	ui_icon(SYSDEF_PROG_DISKOP);
	ui_shows(SYSDEF_PROG_DISKOP);
	ui_attr(UI_ATTR_HIDDEN);
	er = ui_open_on(w, &g_base, SYSDEF_BOX_DISKOP, num, x, y);
	if ( er >= E_OK ) {
		g_win = w->win;
	}
	return er;
}

static void panel_close( UI *w )
{
	ui_close(w);
	memset(&g_win, 0, sizeof(g_win));
}

/*
 * The window worked until a button answers: B_OK for the button that
 * goes ahead or Enter, 0 for 取消, Escape or the window closed
 */
static INT panel_run( UI *w )
{
	T_OBNTM	m;
	INT	num, ev;

	ui_reveal(w);
	for ( ;; ) {
		if ( ui_wait(&g_base, &m, TMO_FEVR) < E_OK ) return 0;
		if ( !ui_mine(w, &m) ) continue;
		ev = ui_event(w, &m, &num);
		if ( ev == UI_EV_CLOSE ) return 0;
		if ( ev == UI_EV_BUTTON ) return ( num == B_OK ) ? B_OK : 0;
		if ( ev == UI_EV_KEY && m.code == KEY_ENTER ) return B_OK;
		if ( ev == UI_EV_KEY && m.code == KEY_ESCAPE ) return 0;
	}
}

/* Something that cannot be done, in a panel; l2 may be NULL */
static void tell( CONST char *l1, CONST char *l2 )
{
	CONST char	*lines[2];

	lines[0] = l1;
	lines[1] = ( l2 != NULL ) ? l2 : " ";
	note(l1);
	(void)ui_ask(&g_base, PN_TELL, lines, 2);
}

/* The desktop's answer that the rights were not given: nothing more when the person put the panel away */
static BOOL no_right( ER er, CONST char *what )
{
	if ( er == E_ABORT ) {
		note("the administrator's password not given");
		return TRUE;
	}
	if ( er == E_OACV ) {
		ts_no_right(what);
		note("no right");
		return TRUE;
	}
	return FALSE;
}

static void said( CONST char *a, CONST char *name, CONST char *b )
{
	char	msg[256];

	snprintf(msg, sizeof(msg), "%s%s%s", a, name, b);
	(void)wm_msg_put(msg);
	note(msg);
}

/* ---------------------------------------------------------------- the rows of a list */

static char	*g_rows[ROWS_MAX];
static char	*g_keys[ROWS_MAX];		/* what goes before the row in each answer, as long as it is */
static UINT	g_flags[ROWS_MAX];
static INT	g_nrows = 0;

/* Every row the desktop has for a list (what: DT_DISK_LIST or DT_DISK_FREE); how many */
static INT rows_get( INT what, CONST TS_UUID *target, INT nkey )
{
	T_DTREQ	rq;
	T_DTANS	an;
	INT	i, k, n = 1;
	char	*p;

	g_nrows = 0;
	for ( i = 0; i < n && i < ROWS_MAX; i++ ) {
		req_init(&rq, what, target);
		rq.grab_x = i;
		if ( ask(&rq, &an) < E_OK ) break;
		n = (INT)an.how;
		free(g_keys[i]);
		g_keys[i] = strdup(ANS_TEXT);
		if ( g_keys[i] == NULL ) break;
		/* the fields before the row kept as they are, the row after them */
		p = g_keys[i];
		for ( k = 0; k < nkey; k++ ) {
			p = strchr(p, '\t');
			if ( p == NULL ) break;
			p++;
		}
		if ( p == NULL ) break;
		p[-1] = 0;
		g_rows[i] = p;
		g_flags[i] = an.flags;
		g_nrows++;
	}
	return g_nrows;
}

/* ---------------------------------------------------------------- 接続 */

static void do_connect( CONST TS_UUID *target )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UI	w;
	INT	n, i, pick = 0, at;
	char	b[64];
	ER	er;

	n = rows_get(DT_DISK_LIST, target, 1);
	snprintf(b, sizeof(b), "connect offers %d", n);
	note(b);
	if ( n == 0 ) {
		tell("接続できるディスクがありません", NULL);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		if ( ( g_flags[i] & DT_F_DISK_PICK ) != 0 ) pick = i;
	}
	if ( panel_open(&w, PN_CONNECT, PW_CONNECT, PH_CONNECT) < E_OK ) return;
	ui_names(&w, DK_LIST, (CONST char *const *)g_rows, n);
	ui_set_value(&w, DK_LIST, pick + 1);
	note("up connect");
	at = ( panel_run(&w) == B_OK ) ? ui_value(&w, DK_LIST) - 1 : -1;
	panel_close(&w);
	if ( at < 0 || at >= n ) {
		note("connect cancelled");
		return;
	}
	snprintf(b, sizeof(b), "connect %s", g_keys[at]);
	note(b);
	req_init(&rq, DT_DISK_ATTACH, NULL);
	(void)dq_part(&g_dq, DT_TAIL_NAME, (CONST UB *)g_keys[at], -1);
	er = ask(&rq, &an);
	snprintf(b, sizeof(b), "attach answered %d", (INT)er);
	note(b);
	if ( er >= E_OK ) {
		said("ディスク「", ANS_TEXT, "」を接続しました");
	} else if ( no_right(er, "ディスクを接続") ) {
		/* said */
	} else if ( er == E_OBJ ) {
		tell("同じディスクがすでに接続されています", NULL);
	} else if ( er == E_NOSPT ) {
		tell("実身のディスクではありません。", "ファイル変換で開いてください");
	} else if ( er == E_PAR ) {
		tell("この形式は扱えません", NULL);
	} else {
		tell("ディスクを接続できませんでした", NULL);
	}
}

/* ---------------------------------------------------------------- 切り離し */

static void do_detach( CONST TS_UUID *target )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	l1[DT_TEXT_MAX + 64], b[64];
	ER	er;

	req_init(&rq, DT_DISK_DETACH, target);
	er = ask(&rq, &an);
	snprintf(b, sizeof(b), "detach answered %d %u", (INT)er, an.how);
	note(b);
	if ( er >= E_OK ) {
		said("ディスク「", ANS_TEXT, "」を切り離しました。取り出せます");
	} else if ( no_right(er, "ディスクの切り離しを") ) {
		/* said */
	} else if ( er == E_NOEXS ) {
		tell("そのディスクは接続されていません", NULL);
	} else if ( er == E_RONLY ) {
		tell("システムのディスクは切り離せません", NULL);
	} else if ( er == E_BUSY ) {
		snprintf(l1, sizeof(l1), "ディスク「%s」%s", ANS_TEXT,
			 ( an.how == DT_HOW_DISK_WIN ) ? "の実身が開いています。"
			 : ( an.how == DT_HOW_DISK_CHECK ) ? "はディスク修復で検査中です。"
			 : "の実身を使っているプログラムがあります。");
		tell(l1, ( an.how == DT_HOW_DISK_WIN ) ? "閉じてから切り離してください" : "終わってから切り離してください");
	} else {
		tell("ディスクを切り離せませんでした", NULL);
	}
}

/* ---------------------------------------------------------------- 状態表示 */

/* A size the way a person reads it: 512KB, 64MB, 3GB */
static void size_text( char *b, INT max, unsigned long long bytes )
{
	if ( bytes >= 1024ULL * 1024 * 1024 ) {
		snprintf(b, (size_t)max, "%lluGB", ( bytes + 512ULL * 1024 * 1024 ) / ( 1024ULL * 1024 * 1024 ));
	} else if ( bytes >= 1024ULL * 1024 ) {
		snprintf(b, (size_t)max, "%lluMB", ( bytes + 512ULL * 1024 ) / ( 1024ULL * 1024 ));
	} else {
		snprintf(b, (size_t)max, "%lluKB", bytes / 1024);
	}
}

static void do_status( CONST TS_UUID *target )
{
	T_DTREQ			rq;
	T_DTANS			an;
	UI			w;
	char			*p, *name, b[96];
	unsigned long long	total, used;
	long			nobj, gc;
	ER			er;

	req_init(&rq, DT_DISK_STATUS, target);
	er = ask(&rq, &an);
	if ( er < E_OK ) {
		tell("その実身のディスクは接続されていません", NULL);
		return;
	}
	p = ANS_TEXT;
	name = field(&p);
	total = strtoull(field(&p), NULL, 10);
	used = strtoull(field(&p), NULL, 10);
	nobj = strtol(field(&p), NULL, 10);
	gc = strtol(field(&p), NULL, 10);
	if ( panel_open(&w, PN_STATUS, PW_STATUS, PH_STATUS) < E_OK ) return;
	snprintf(b, sizeof(b), "ディスク状態: %s", name);
	ui_set_text(&w, DK_HEAD, b);
	size_text(b, sizeof(b), total);
	ui_set_text(&w, DK_VALUE, b);
	size_text(b, sizeof(b), used);
	ui_set_text(&w, DK_VALUE + 1, b);
	size_text(b, sizeof(b), total - used);
	ui_set_text(&w, DK_VALUE + 2, b);
	snprintf(b, sizeof(b), "%llu%%", ( total > 0 ) ? used * 100 / total : 0);
	ui_set_text(&w, DK_VALUE + 3, b);
	snprintf(b, sizeof(b), "%ld", nobj);
	ui_set_text(&w, DK_VALUE + 4, b);
	snprintf(b, sizeof(b), "%ld", gc);
	ui_set_text(&w, DK_VALUE + 5, b);
	snprintf(b, sizeof(b), "up status \"%s\" %ld objects %ld garbage", name, nobj, gc);
	note(b);
	(void)panel_run(&w);
	panel_close(&w);
	note("status closed");
}

/* ---------------------------------------------------------------- 区画作成 */

static void do_create( void )
{
	T_DTREQ			rq;
	T_DTANS			an;
	UI			w;
	char			label[64], b[128], *p, *disk;
	unsigned long long	first, last;
	INT			n, at, mb;
	ER			er;

	n = rows_get(DT_DISK_FREE, NULL, 3);
	snprintf(b, sizeof(b), "create offers %d", n);
	note(b);
	if ( n == 0 ) {
		tell("区画を作れる空き領域がありません", NULL);
		return;
	}
	if ( panel_open(&w, PN_CREATE, PW_CREATE, PH_CREATE) < E_OK ) return;
	ui_names(&w, DK_LIST, (CONST char *const *)g_rows, n);
	ui_set_value(&w, DK_LIST, 1);
	ui_set_value(&w, DK_SIZE, 0);
	ui_set_text(&w, DK_LABEL, "ディスク");
	note("up create");
	at = -1;
	label[0] = 0;
	mb = 0;
	if ( panel_run(&w) == B_OK ) {
		at = ui_value(&w, DK_LIST) - 1;
		mb = ui_value(&w, DK_SIZE);
		if ( ui_text(&w, DK_LABEL, label, sizeof(label)) < 0 ) label[0] = 0;
	}
	panel_close(&w);
	if ( at < 0 || at >= n ) {
		note("create cancelled");
		return;
	}
	p = g_keys[at];
	disk = field(&p);
	first = strtoull(field(&p), NULL, 10);
	last = strtoull(field(&p), NULL, 10);
	if ( mb < 0 || (unsigned long long)mb * 2048 > last - first + 1 || ( mb > 0 && mb < DK_MIN_MB ) ) {
		snprintf(b, sizeof(b), "create %s refused: %d MB", disk, mb);
		note(b);
		tell("その大きさの区画は作れません", "空き領域に収まる、8MB以上の大きさにしてください");
		return;
	}
	req_init(&rq, DT_DISK_MAKE, NULL);
	{
		size_t	max = strlen(label) + 32;
		char	*t = (char *)malloc(max);

		if ( t == NULL ) {
			tell("メモリが足りないため、区画を作れませんでした", NULL);
			return;
		}
		(void)dq_part(&g_dq, DT_TAIL_NAME, (CONST UB *)disk, -1);
		snprintf(t, max, "%llu\t%s", first, label);
		(void)dq_part(&g_dq, DT_TAIL_TEXT, (CONST UB *)t, -1);
		free(t);
	}
	rq.grab_x = mb;
	er = ask(&rq, &an);
	snprintf(b, sizeof(b), "make answered %d", (INT)er);
	note(b);
	if ( er >= E_OK ) {
		said("区画「", ANS_TEXT, "」を作り、接続しました");
	} else if ( no_right(er, "区画を作成") ) {
		/* said */
	} else if ( an.how == DT_HOW_DISK_MADE ) {
		tell("区画は作りましたが、接続できませんでした", NULL);
	} else if ( er == E_PAR ) {
		tell("その大きさの区画は作れません", "空き領域に収まる、8MB以上の大きさにしてください");
	} else {
		tell("区画を作れませんでした", NULL);
	}
}

/* ---------------------------------------------------------------- 拡張 */

static void do_grow( CONST TS_UUID *target )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UI	w;
	char	b[DT_TEXT_MAX + 64], *p, *name, *path, *from, *to;
	long	cur, most, mb = -1;
	ER	er;

	req_init(&rq, DT_DISK_GROWS, target);
	er = ask(&rq, &an);
	if ( er == E_OBJ ) {
		tell("このディスクは広げられません", "区画表(GPT)のTSFSの区画ではありません");
		return;
	}
	if ( er == E_LIMIT ) {
		tell("広げられる空き領域がありません", "区画のすぐ後ろに空き領域が要ります");
		return;
	}
	if ( er < E_OK ) {
		tell("このディスクは広げられません", NULL);
		return;
	}
	p = ANS_TEXT;
	name = field(&p);
	path = field(&p);
	cur = strtol(field(&p), NULL, 10);
	most = strtol(field(&p), NULL, 10);
	if ( panel_open(&w, PN_GROW, PW_GROW, PH_GROW) < E_OK ) return;
	snprintf(b, sizeof(b), "ディスク「%s」(%s)を広げます", name, path);
	ui_set_text(&w, DK_HEAD, b);
	snprintf(b, sizeof(b), "%ldMB", cur);
	ui_set_text(&w, DK_NOW, b);
	snprintf(b, sizeof(b), "最大 %ldMB", most);
	ui_set_text(&w, DK_MOST, b);
	ui_set_value(&w, DK_SIZE, (INT)most);
	snprintf(b, sizeof(b), "up grow %s %ld MB to %ld MB", path, cur, most);
	note(b);
	if ( panel_run(&w) == B_OK ) {
		mb = ui_value(&w, DK_SIZE);
	} else {
		panel_close(&w);
		note("grow cancelled");
		return;
	}
	panel_close(&w);
	if ( mb <= cur || mb > most ) {
		snprintf(b, sizeof(b), "grow %s refused: %ld MB", path, mb);
		note(b);
		tell("その大きさには広げられません", "今の大きさより大きく、最大までの大きさにしてください");
		return;
	}
	req_init(&rq, DT_DISK_GROW, target);
	rq.grab_x = (INT)mb;
	er = ask(&rq, &an);
	snprintf(b, sizeof(b), "grow answered %d", (INT)er);
	note(b);
	if ( er >= E_OK ) {
		p = ANS_TEXT;
		from = field(&p);
		to = field(&p);
		snprintf(b, sizeof(b), "ディスク「%s」を%sMBから%sMBに広げました", name, from, to);
		(void)wm_msg_put(b);
		note(b);
	} else if ( no_right(er, "ディスクを拡張") ) {
		/* said */
	} else if ( er == E_BUSY ) {
		tell("ディスク修復で検査中のため、広げられません", "検査が終わってから広げてください");
	} else if ( er == E_PAR ) {
		tell("その大きさには広げられません", "今の大きさより大きく、最大までの大きさにしてください");
	} else {
		tell("ディスクを広げられませんでした", NULL);
	}
}

/* ---------------------------------------------------------------- 形式を新しくする */

/*
 * The volume of the link converted to the format's version 5 (design
 * 7.3): asked of the person in a panel, then done by this program in
 * its own task through the kernel (ob_cnv_vol), which checks the whole
 * volume first and writes nothing when the check finds damage. The
 * system's volume is converted at start, not here.
 */
static void do_convert( CONST TS_UUID *target )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_TSFSCNV	r;
	UI		w;
	char		b[DT_TEXT_MAX + 96], name[DT_TEXT_MAX], path[TSFSCK_DEVNM_MAX], *p;
	INT		told = 0, form;
	BOOL		go;
	ER		er;

	req_init(&rq, DT_DISK_FORM, target);
	er = ask(&rq, &an);
	if ( er < E_OK ) {
		tell("このディスクの形式は変えられません", "実身のディスク(TSFS)ではありません");
		return;
	}
	p = ANS_TEXT;
	snprintf(name, sizeof(name), "%s", field(&p));
	snprintf(path, sizeof(path), "%s", field(&p));
	form = (INT)an.how;
	snprintf(b, sizeof(b), "convert %s is of version %d%s", path, form,
		 ( an.flags & DT_F_DISK_SYS ) ? ", the system's" : "");
	note(b);
	if ( form >= 5 ) {
		snprintf(b, sizeof(b), "ディスク「%s」は新しい形式です", name);
		tell(b, NULL);
		return;
	}
	if ( ( an.flags & DT_F_DISK_SYS ) != 0 ) {
		tell("システムのディスクは、起動のときに新しい形式に変わります",
		     "障害があるときは、ディスク修復で修復してから再起動してください");
		return;
	}
	if ( panel_open(&w, PN_CONVERT, PW_CONVERT, PH_CONVERT) < E_OK ) return;
	snprintf(b, sizeof(b), "ディスク「%s」(%s)を新しい形式に変えます。", name, path);
	ui_set_text(&w, DK_HEAD, b);
	ui_set_text(&w, DK_HEAD + 1, "変える前にディスク全体を検査し、障害があれば変えません。");
	ui_set_text(&w, DK_HEAD + 2, "変えたあとは、R1.410までのシステムでは使えません。");
	note("up convert");
	go = (BOOL)( panel_run(&w) == B_OK );
	if ( go ) {
		er = ts_admin_ask("ディスクの形式の変換", &w.win);
		snprintf(b, sizeof(b), "administrator %d", (INT)er);
		note(b);
		if ( er < E_OK ) {
			panel_close(&w);
			if ( er != E_ABORT ) ts_no_right("ディスクの形式を変換");
			return;
		}
	}
	panel_close(&w);
	if ( !go ) {
		note("convert cancelled");
		return;
	}
	snprintf(b, sizeof(b), "ディスク「%s」を検査しています", name);
	(void)wm_msg_put(b);
	memset(&r, 0, sizeof(r));
	er = ob_cnv_vol(path, 0, &r, NULL, 0, &told);
	snprintf(b, sizeof(b), "convert answered %d: state %u, %d found, %d ms", (INT)er, (UINT)r.state,
		 r.ck.problems, (INT)r.total_ms);
	note(b);
	if ( er == E_BUSY && r.ck.inuse ) {
		snprintf(b, sizeof(b), "ディスク「%s」の実身が開いています", name);
		tell(b, "閉じてから変えてください");
	} else if ( er == E_BUSY ) {
		tell("ディスク修復で検査中のため、形式を変えられません", "検査が終わってから変えてください");
	} else if ( er == E_OACV ) {
		ts_no_right("ディスクの形式を変換");
	} else if ( r.state == TSFSCNV_S_DONE ) {
		said("ディスク「", name, "」を新しい形式に変えました");
	} else if ( r.state == TSFSCNV_S_NOTV4 ) {
		snprintf(b, sizeof(b), "ディスク「%s」は新しい形式です", name);
		tell(b, NULL);
	} else if ( r.state == TSFSCNV_S_DAMAGE ) {
		tell("ディスクに障害が見つかったので、形式を変えませんでした。",
		     "ディスク修復で修復してから、もう一度変えてください。");
	} else {
		tell("ディスクの形式を変えられませんでした", "ディスクの読み書きに失敗しました");
	}
}

/* ---------------------------------------------------------------- start */

/* The channel the panels are told to, and the data box they come from */
static ER base_open( void )
{
	T_OBCRE	c;
	TS_UUID	ch;

	memset(&g_base, 0, sizeof(g_base));
	if ( ui_uuid(SYSDEF_BOX_DISKOP, &g_base.box) < E_OK ) return E_SYS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK ) return E_NOMEM;
	g_base.ch = ch;
	g_base.port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE);
	return ( g_base.port > 0 ) ? E_OK : E_OBJ;
}

int main( void )
{
	T_DTDISKARG	a;
	TS_UUID		none, *t;
	SZ		n;
	char		b[64];

	memset(&a, 0, sizeof(a));
	memset(&none, 0, sizeof(none));
	n = ts_get_arg(&a, sizeof(a));
	if ( n < (SZ)sizeof(a) ) {
		memset(&a, 0, sizeof(a));
		strcpy((char *)a.code, "disk.status");	/* run by itself: the system's disk */
	}
	a.code[sizeof(a.code) - 1] = 0;
	t = ( memcmp(&a.target, &none, sizeof(none)) != 0 ) ? &a.target : NULL;
	if ( base_open() < E_OK ) {
		note("no channel");
		return 1;
	}
	snprintf(b, sizeof(b), "started %s%s", (char *)a.code, ( t != NULL ) ? " on a link" : "");
	note(b);
	if ( strcmp((char *)a.code, "disk.connect") == 0 ) {
		do_connect(t);
	} else if ( strcmp((char *)a.code, "disk.detach") == 0 && t != NULL ) {
		do_detach(t);
	} else if ( strcmp((char *)a.code, "disk.status") == 0 ) {
		do_status(t);
	} else if ( strcmp((char *)a.code, "disk.create") == 0 ) {
		do_create();
	} else if ( strcmp((char *)a.code, "disk.grow") == 0 ) {
		do_grow(t);
	} else if ( strcmp((char *)a.code, "disk.convert") == 0 ) {
		do_convert(t);
	}
	dq_free(&g_dq);
	(void)ob_cls_obj(g_base.port);
	(void)ob_del_obj(&g_base.ch);
	note("done");
	return 0;
}
