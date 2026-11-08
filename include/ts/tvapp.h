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
 *	items 実行 and トレー of the menu -- are here. What is particular
 *	to each program is told through T_TAHOST.
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
} T_TAHOST;

IMPORT CONST T_TAHOST	*ta_host;

/* ---------------------------------------------------------------- what it holds */

IMPORT TS_UUID	g_obj;			/* the object opened */
IMPORT INT	g_recno;		/* its record shown */
IMPORT BOOL	g_has_parent;		/* opened from a link of a window of the desktop's: */
IMPORT TS_UUID	g_parent, g_vobjid;	/* the object whose record holds it, and its vobjid */
IMPORT BOOL	g_from_link;		/* opened from a link: g_start_arg is how it asks to be shown */
IMPORT T_PRCOPEN	g_start_arg;		/* the start-up argument, with the link's bgcol, wordWrap and viewMode */
IMPORT UB	g_name[OB_NAME_MAX];
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
IMPORT void link_open( CONST T_VOBJ *v );

/* ---------------------------------------------------------------- the record */

IMPORT ER   ta_read( UB **p_x, SZ *p_len );	/* record g_recno, as it is stored */
IMPORT void ta_was( void );			/* what g_rec links to now, as read */
IMPORT ER   ta_save( void );			/* 元の実身に保存 */
IMPORT ER   ta_rec_put( CONST TS_UUID *id, T_TAD *rec );	/* record 0 of another object written */
IMPORT void ta_watch( CONST TS_UUID *id );	/* notices of its changes, at the window's channel */
IMPORT void ta_watch_links( void );		/* of every object g_rec links to */
IMPORT BOOL ta_links_event( CONST T_OBNTM *m );	/* the desktop told of a link: TRUE when it was that */
IMPORT void ta_links_told( void );		/* what is waiting of it, read */
/* Asked whether links would be taken where the hand is (OB_E_ASK): answered takes */
IMPORT void ta_ask_answer( CONST T_OBNTM *m, BOOL takes );
IMPORT void ta_paper_read( void );

/* ---------------------------------------------------------------- the window */

IMPORT void ta_work_size( void );
IMPORT void ta_fullscreen( void );
IMPORT void ta_paper_ask( void );		/* 背景色変更 */
IMPORT ER   ta_print( CONST T_TAD *rec, UINT flags );	/* 印刷 of rec; flags DT_F_PR_DETAIL, _HIDDEN */
IMPORT void ta_save_new( void );		/* 新たな実身に保存 */
/* closing, opened from a link: the view written back into it (DT_RQ_LINK_VIEW); wrap TAD_WRAP_* */
IMPORT void ta_link_view_back( INT sx, INT sy, INT zoom, INT wrap, CONST char *view );
IMPORT void ta_window_keep( void );
IMPORT void ta_trash_open( void );		/* 屑実身操作 */

/* ---------------------------------------------------------------- the menu */

IMPORT void ta_tray_menu( ID mid );		/* トレー: the sets in it */
IMPORT INT  ta_exec_menu( ID mid, CONST T_VOBJ *v );	/* 実行 for the link v: how many programs */
IMPORT void ta_exec_app( CONST T_VOBJ *v, INT index );

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

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVAPP_H__ */
