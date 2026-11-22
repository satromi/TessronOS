/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvapp.h
 *	What an editor running as a process shares with the others: the
 *	object it was opened on, its window, saving it, the links it is
 *	watched by, the desktop asked, and the converter of かな漢字
 *	(lib/libtv/tv_app.c, design 11.3)
 *
 *	基本図形編集 (application/fig) and 基本文章編集 (application/doc)
 *	each edit one record of one object in one ordinary window. What
 *	they hold of it, and what they do with it that does not depend on
 *	what the record is -- writing it back and moving the counts of what
 *	it links to, the notices of the objects it links to, what a window
 *	opened from one of its links did to that link, 新たな実身に保存,
 *	where the window stood, 全画面表示, 背景色変更, 屑実身操作, the
 *	item トレー of the menu -- are here. What is particular to each
 *	program is told through T_TAHOST. The 仮身操作, 実身操作 and 実行 of
 *	a link are the shared part's of every program (ts/tvvop.h), asked
 *	through ta_tv_asker.
 *
 *	The converter of かな漢字 works for a text being edited (T_DDED,
 *	include/ts/docedit.h): its keys, what it is composing drawn at the
 *	caret, and the list of candidates the system shows.
 */

#ifndef __TS_TVAPP_H__
#define __TS_TVAPP_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/ui.h>
#include <ts/dtreq.h>
#include <ts/docedit.h>
#include <ts/tvvop.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the shared part asks of the program */
typedef struct {
	CONST char	*who;			/* what its notes on the console begin with ("figure") */
	CONST char	*prog;			/* its program object: the icon of its window */
	CONST char	*box;			/* its data box: the panels over its window */
	/* the record changed by what the desktop told of a link: the model made again */
	void		(*remade)( void );
	void		(*picks_none)( void );	/* before a link is added to the record */
	/* what else the desktop says at the channel of the links (DT_HOW_ACC); NULL none */
	void		(*told)( CONST T_DTANS *an );
	/* where its window opens when the object keeps no place of it; at_w 0: 60,40, 900 by 640 */
	INT		at_x, at_y, at_w, at_h;
} T_TAHOST;

IMPORT CONST T_TAHOST	*ta_host;

/* ---------------------------------------------------------------- what it holds */

IMPORT TS_UUID	g_obj;			/* the object opened */
IMPORT INT	g_recno;		/* its record shown */
IMPORT BOOL	g_has_parent;		/* opened from a link of a window of the desktop's: */
IMPORT TS_UUID	g_parent, g_vobjid;	/* the object whose record holds it, and its vobjid */
IMPORT BOOL	g_from_link;		/* opened from a link: g_start_arg is how it asks to be shown */
IMPORT T_PRCOPEN	g_start_arg;		/* the start-up argument, with the link's bgcol, wordWrap and viewMode */
/*
 * The object's name, whole (never NULL, "" before there is one): g_name,
 * and ta_name gives the same. ta_set_name changes what this program holds
 * as its name (a copy is taken; FALSE when there was no memory for it,
 * the name before kept).
 */
IMPORT UB	*g_name;

/* Bytes of a line note says on the console (a long name in it is cut at a whole letter) */
#define TA_LOG		320
IMPORT CONST UB *ta_name( void );
IMPORT BOOL ta_set_name( CONST UB *name );
IMPORT TS_UUID	g_win;
IMPORT ID	g_kw;
IMPORT INT	g_gid;
IMPORT INT	g_w, g_h;		/* the window's work area */
IMPORT UI	g_ui;			/* for the data box's panels over the window */
IMPORT ID	g_port;			/* the window's channel */
IMPORT T_TAD	*g_rec;			/* the record, as it is now */
IMPORT UW	g_paper;		/* the object's paper, TV_PAPER_LOOK the system's */
IMPORT BOOL	g_full;			/* 全画面表示 */
IMPORT BOOL	g_changed;		/* changed since it was read or saved */
IMPORT BOOL	g_dirty;		/* to be drawn again */
IMPORT BOOL	g_closing;
IMPORT UD	g_input_at;		/* when the key or press being answered happened (ns), 0 none */

/* ---------------------------------------------------------------- starting and ending */

/* The object the program was started on (T_PRCOPEN): E_NOEXS when there is none */
IMPORT ER   ta_start( CONST T_TAHOST *host, T_OBREF *p_ref );
/* The window stands for obj, whatever the start-up argument says: E_NOEXS when it is not there */
IMPORT ER   ta_start_on( CONST T_TAHOST *host, CONST TS_UUID *obj, T_OBREF *p_ref );
IMPORT ER   ta_channel( void );			/* the window's channel, and the desktop's on links */
IMPORT ER   ta_win_open( UW events );		/* the window, where it stood last */
IMPORT void ta_end( void );			/* the window let go of */
IMPORT void ta_channel_close( void );		/* and the channels, when nothing more is to come */
IMPORT UD   ta_now( void );			/* the monotonic clock, ns */

/* The main task's stack: ta_stack_start first thing in main, ta_stack_used the most used, in bytes */
#define TA_STACK_SIZE	( 64 * 1024 )	/* what the system gives a program's main task */
#define TA_STACK_SEEN	( TA_STACK_SIZE - 4096 )
IMPORT void ta_stack_start( void );
IMPORT INT  ta_stack_used( void );

/* ---------------------------------------------------------------- saying things */

IMPORT void note( CONST char *s );		/* on the console, for the tests */
IMPORT void say( CONST char *s );		/* on the desktop's message line */

/* ---------------------------------------------------------------- the desktop */

IMPORT ER   ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms );
/*
 * The tail of a request and its answer (dtreq.h, DT_TAIL_*): a part of
 * the next ask_desktop's tail added, as long as it is (len < 0 up to its
 * 0); a part of the last answer's found, NULL none. The fixed fields name
 * and text of a request and text of an answer are not used. told() is
 * given a notice with its tail after it: dt_ans_tail(an,
 * sizeof(T_DTANS) + an->tailsz). ta_read_whole reads one message of a
 * channel whole into *p_buf, made as large as it must be (*p_cap): its
 * size, or an error.
 */
IMPORT void ta_req_part( UINT kind, CONST UB *s, SZ len );
IMPORT CONST UB *ta_ans_part( UINT kind, SZ *p_len );
IMPORT SZ   ta_read_whole( ID k, UB **p_buf, SZ *p_cap, TMO tmo );
IMPORT void ta_link_open( CONST T_VOBJ *v, UINT flags, INT rank );	/* opened by DT_F_AUTO: 自動起動, at place rank */
/* The 仮身操作 (ts/tvvop.h): asked through ask_desktop, the links named at the window's channel */
IMPORT void ta_tv_asker( T_TVASK *a );
IMPORT BOOL ta_tv_named( TS_UUID *p_parent, TS_UUID *p_chan );

/* ---------------------------------------------------------------- the record */

IMPORT ER   ta_read( UB **p_x, SZ *p_len );	/* record g_recno, as it is stored */
IMPORT void ta_was( void );			/* what g_rec links to now, as read */
IMPORT ER   ta_save( void );			/* 元の実身に保存 */
IMPORT ER   ta_rec_put( CONST TS_UUID *id, T_TAD *rec );	/* record 0 of another object written */
IMPORT BOOL ta_disk_gone( BOOL figure );	/* closing, the disk of the object out: what changed handed over */
IMPORT void ta_watch( CONST TS_UUID *id );	/* notices of its changes, at the window's channel */
IMPORT void ta_watch_links( void );		/* of every object g_rec links to */
IMPORT BOOL ta_links_event( CONST T_OBNTM *m );	/* the desktop told of a link: TRUE when it was that */
IMPORT BOOL ta_told_channel( TS_UUID *p );	/* that channel, for the desktop to say other things at */
IMPORT void ta_links_told( void );		/* what is waiting of it, read */
IMPORT void ta_listen( CONST char *prog_id );	/* told there of the program run again, and of its list (DT_RQ_TELL) */
IMPORT void ta_title( CONST char *title );	/* the window's title, for a list made */
/* Asked whether links would be taken where the hand is (OB_E_ASK): answered takes */
IMPORT void ta_ask_answer( CONST T_OBNTM *m, BOOL takes );

/* Carried out of the window: the desktop follows the hand on (DT_RQ_CARRY) */
IMPORT INT  ta_tray_pictures( T_TAD *frag );	/* the tray's pictures it names, written beside the record */
IMPORT BOOL ta_frag_put( CONST T_TAD *frag );	/* the fragment, in the tray's place for it (DT_F_FRAG) */
IMPORT BOOL ta_back_at( CONST T_DTANS *an, INT *p_x, INT *p_y );	/* let go back here (DT_F_BACK) */
IMPORT ER   ta_text_away( T_DDED *e, T_DTREQ *rq, CONST UB *s, INT n );	/* letters of e: T_DDHOST text_away */
IMPORT void ta_paper_read( void );		/* the paper, the object's own read again */
IMPORT void ta_paper_open( void );		/* as the window opens, the link's watched */
IMPORT void ta_paper_refresh( void );		/* 再表示: and the link's as it is now */
IMPORT BOOL ta_paper_changed( CONST TS_UUID *u );	/* OB_E_CHANGE of u: TRUE when the paper is not as it was */

/* ---------------------------------------------------------------- the window */

IMPORT void ta_work_size( void );
IMPORT void ta_fullscreen( void );
IMPORT BOOL ta_full_shown( void );		/* the window the screen's size by 全画面表示, as the desktop says */
IMPORT void ta_paper_ask( void );		/* 背景色変更 */
IMPORT ER   ta_print( CONST T_TAD *rec, UINT flags );	/* 印刷 of rec; flags DT_F_PR_DETAIL, _HIDDEN */
IMPORT void ta_save_new( void );		/* 新たな実身に保存 */
/* closing, opened from a link: the view written back into it (DT_RQ_LINK_VIEW); wrap TAD_WRAP_* */
IMPORT void ta_link_view_back( INT sx, INT sy, INT zoom, INT wrap, CONST char *view );
IMPORT void ta_window_keep( void );
IMPORT void ta_trash_open( void );		/* 屑実身操作 */

/* ---------------------------------------------------------------- the menu */

IMPORT void ta_tray_menu( ID mid );		/* トレー: the sets in it */

/* ---------------------------------------------------------------- the converter of かな漢字 */

IMPORT void ta_ime_mode( UINT imode );		/* the input mode of the key being worked */
IMPORT BOOL ta_ime_key( T_DDED *e, UINT code, UINT mods );	/* TRUE: the converter's */
IMPORT void ta_ime_draw( T_DDED *e, INT gid );
IMPORT BOOL ta_ime_on( void );			/* かな input on */
IMPORT BOOL ta_ime_busy( void );		/* composing */
IMPORT void ta_ime_commit( T_DDED *e );		/* what is being composed, into e */
IMPORT void ta_ime_end( T_DDED *e );		/* and the session ended */
IMPORT void ta_ime_pick( T_DDED *e, INT row );	/* a name on the system's list pressed */

/*
 * ---------------------------------------------------------------- the panels
 * Panels of the program's data box, the same numbers in every program
 * that uses them, each with the parts the desktop's own panel has, in
 * the same places: 取消 is button 101, the one that goes ahead 102.
 */
#define TA_PN_FIND	3		/* 検索/置換 */
#define TA_PN_NAME	4		/* the name of a new object: 仮身化 */
#define TA_PN_PAPER	6		/* 用紙設定 */
#define TA_PN_TEXT	8		/* one line of text: ルビ, 文字サイズ */
#define TA_PN_FIELDS6	9		/* six labelled boxes: タブ書式設定 */
#define TA_PN_FIELDS9	10		/* nine: ヘッダ/フッタ */
#define TA_PN_LIST	11		/* one of a list of names: 段落間隔, 書体 */

IMPORT BOOL ta_ask_name( CONST char *prompt, CONST UB *start, UB *out, INT max );
IMPORT BOOL ta_ask_text( CONST char *p1, CONST char *p2, CONST UB *start, UB *out, INT max,
			 CONST char *ok );
IMPORT BOOL ta_fields_form( CONST char *title, INT n, CONST char *CONST *labels,
			    CONST char *CONST *hints, UB (*vals)[DE_FIELD_MAX] );
IMPORT INT  ta_list_form( CONST char *title, CONST char *CONST *names, INT n, INT start );
IMPORT INT  ta_find_form( UB *what, UB *with, INT max, BOOL *p_regex );	/* DE_FIND_*, 0 put away */
IMPORT INT  ta_paper_form( T_DEPAPER *pp );	/* DE_PAPER_* */
IMPORT BOOL ta_pick_colour( UW *io );		/* the system's panel (DT_RQ_COLOUR) */

/*
 * Any panel of a data box (box, NULL the program's), before the window
 * is open too: opened, worked until a button (its number, 'enter' for
 * Enter, 0 for 取消 and for one put away) and put away. ta_tell_in and
 * ta_ask_in work a panel of two lines and 了解, or two lines, 取消
 * (button 101, its words 'no') and the button that goes ahead (102,
 * 'yes'): TRUE for that one.
 */
IMPORT ER   ta_dlg_open( CONST char *box, INT num, UI *q );
IMPORT INT  ta_dlg_run( UI *q, INT enter );
IMPORT void ta_dlg_close( UI *q );
IMPORT void ta_tell_in( CONST char *box, INT num, CONST char *l1, CONST char *l2 );
IMPORT BOOL ta_ask_in( CONST char *box, INT num, CONST char *l1, CONST char *l2,
		       CONST char *no, CONST char *yes );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVAPP_H__ */
