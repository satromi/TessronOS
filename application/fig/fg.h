/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg.h
 *	基本図形編集 as a program running as a process: what its parts share
 *	(application/fig/fg_main.c, fg_edit.c, design 11.3)
 *
 *	The figure is the record 0 of the object opened, read and parsed
 *	into the program's memory (g_rec) and made into a model (g_fig) to
 *	draw and to ask where things are. What is done to it is done to the
 *	record by the figure editor's code (figedit.c) and by what is here,
 *	the model made again after; the record is written back when it is
 *	saved. What is picked and what can be taken back are the program's.
 */

#ifndef __FG_H__
#define __FG_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/figedit.h>
#include <ts/ui.h>
#include <ts/dtreq.h>
#include <ts/docedit.h>
#include <ts/tvapp.h>

/* The panels of the program's data box (SYSDEF_BOX_FIG) */
#define PN_ALIGN	1		/* 位置あわせ */
#define PN_TELL		2		/* what could not be done, two lines */
/* 3 検索/置換, 4 仮身化's name and 6 用紙設定, in a piece of text: TA_PN_* of tvapp.h */
#define P_ALIGN_H	10		/* 10..13: なし 左 中央 右, selector switches of one group */
#define P_ALIGN_V	20		/* 20..23: なし 上 中央 下 */
#define P_TELL_1	1
#define P_TELL_2	2
#define P_CANCEL	101
#define P_OK		102

/* ---------------------------------------------------------------- what it holds (fg_main.c) */

/* what any editor running as a process holds, g_obj, g_rec, g_kw and the rest: tvapp.h */
extern T_TVFIG	*g_fig;			/* the model made from g_rec */
extern UINT	*g_pick;		/* for each shape: 0, or the order it was picked in */
extern INT	g_maxpick, g_npick;
extern UINT	g_pick_seq;
extern T_FGED	g_ed;			/* the figure editor's code over them */

extern INT	g_sx, g_sy;		/* where it is scrolled to, in the window's pixels */
extern INT	g_fw, g_fh;		/* how large the figure is drawn */
extern INT	g_zoom;			/* in eighths */
extern UD	g_shown_px;		/* pixels of the window put on the screen, counted for the tests */
extern BOOL	g_show_hidden;

/*
 * 仮身一覧: the program is the cabinet (application/cab/cb_main.c), not
 * 基本図形編集. The figure is shown and its links are worked as in the
 * figure editor, but its shapes are not edited: nothing but links is
 * taken, and there is no 道具パネル, no text edited where it stands and
 * no パターン編集. The menu is the cabinet's (SYSDEF_MENU_CAB).
 */
extern BOOL	fg_cabinet;

IMPORT void tell( CONST char *l1, CONST char *l2 );

/* ---------------------------------------------------------------- links (fg_link.c) */

IMPORT void fg_ask_of( CONST T_VOBJ *v, UINT req, T_DTREQ *rq );
IMPORT CONST char *fg_link_name( CONST T_VOBJ *v );
IMPORT BOOL fg_app_locked( CONST T_VOBJ *v );
IMPORT void fg_link_menu( ID mid, CONST T_VOBJ *v );
IMPORT void fg_link_op( CONST char *code, T_VOBJ *v );
IMPORT void fg_link_copy_put( CONST T_VOBJ *v, CONST TS_UUID *copy, INT dx, INT dy );

/* ---------------------------------------------------------------- editing (fg_edit.c) */

IMPORT void fg_model( void );
IMPORT void fg_sync( void );		/* g_ed set from what is held */
IMPORT void fg_paint( BOOL quiet );
IMPORT void fg_paint_held( void );	/* drawn whole, not yet put on the screen */
IMPORT BOOL fg_paint_part( BOOL quiet );	/* what was marked alone */
IMPORT BOOL fg_damaged( void );
IMPORT void fg_damage( CONST T_DPRECT *r );
IMPORT void fg_damage_picks( void );
IMPORT void fg_after( void );		/* changed: the model made again, to be drawn */
IMPORT void fg_before( void );		/* what is there now kept, to be taken back to */
IMPORT BOOL fg_undo( void );
IMPORT INT  fg_undos( void );
IMPORT void fg_undo_clear( void );

IMPORT void fg_pick_none( void );
IMPORT void fg_pick_all( void );
IMPORT void fg_pick_add( INT i );
IMPORT INT  fg_first_pick( void );
IMPORT BOOL fg_is_link( INT i );
IMPORT BOOL fg_takeable( INT i );
IMPORT BOOL fg_blinks( void );
IMPORT void fg_blink_turn( void );
IMPORT void fg_blink_show( void );	/* a key or a press: the frames shown, for a whole turn */

IMPORT void fg_press( CONST T_OBNTM *m );
IMPORT void fg_second_down( void );
IMPORT void fg_moved( INT x, INT y );
IMPORT void fg_released( INT x, INT y );
IMPORT BOOL fg_holding( void );
IMPORT void fg_hold_cancel( void );
IMPORT void fg_left( void );		/* the pointer left the window while the hand was down */

IMPORT void fg_delete( void );
IMPORT BOOL fg_copy( void );
IMPORT void fg_cut( void );
IMPORT void fg_paste( BOOL take );
IMPORT BOOL fg_tray_has( void );
IMPORT INT  fg_tray_links( void );		/* the links of the set in hand, when it is a figure */
IMPORT void fg_protect( BOOL background, BOOL on );
IMPORT void fg_link_order( BOOL front );	/* a cabinet's いちばん前へ, いちばん後ろへ */
IMPORT void fg_arrange( CONST T_FEARRANGE *o );	/* a cabinet's 整頓 (fe_arrange) */
IMPORT void fg_dropped( CONST T_OBDROP *dr );

IMPORT void fg_scroll_to( INT x, INT y );
IMPORT T_TAD *fg_tray_frag( void );	/* the first fragment of the set in hand; NULL none */

/* ---------------------------------------------------------------- a piece of text edited (fg_text.c) */

IMPORT BOOL fg_text_on( void );
IMPORT void fg_text_begin( T_TADNODE *node, INT x, INT y );
IMPORT void fg_text_end( void );
IMPORT ER   fg_text_letters( INT x, INT y, CONST UB *s, INT n );	/* into the text being edited, at x, y of the work area */
IMPORT ER   fgt_text_drop( INT x, INT y, CONST UB *s, INT n );	/* letters dropped on the figure */
IMPORT BOOL fg_text_press( CONST T_OBNTM *m );
IMPORT BOOL fg_text_holding( void );
IMPORT void fg_text_second_down( void );	/* 複写ドラッグ: what is carried, a copy */
IMPORT void fg_text_moved( INT x, INT y );
IMPORT void fg_text_released( INT x, INT y );
IMPORT BOOL fg_text_key( CONST T_OBNTM *m );
IMPORT void fg_text_pick( INT row );		/* a name on the list of candidates */
IMPORT BOOL fg_text_command( INT cmd );
IMPORT void fg_text_draw( INT gid );
IMPORT void fg_text_look( INT item );		/* the 書式 palette's item, on the letters (figtool.h) */

/* ---------------------------------------------------------------- the 道具パネル (fg_tool.c) */

IMPORT void fgt_show( void );
IMPORT void fgt_hide( void );
IMPORT void fgt_toggle( void );
IMPORT BOOL fgt_open( void );
IMPORT void fgt_sync( void );		/* the panel shows the tool and the look as they are */
IMPORT BOOL fgt_event( CONST T_OBNTM *m );	/* a notice of the panel's: TRUE when it was */
IMPORT BOOL fgt_press( CONST T_OBNTM *m );	/* a drawing tool took the press */
IMPORT BOOL fgt_twice( INT x, INT y, INT fx, INT fy, INT i );	/* a second press: the canvas or 変形 */
IMPORT BOOL fgt_drawing( void );
IMPORT void fgt_moved( INT x, INT y, UINT mods );
IMPORT void fgt_released( void );
IMPORT BOOL fgt_key( CONST T_OBNTM *m );
IMPORT void fgt_hover( INT x, INT y );
IMPORT void fgt_picked( void );
IMPORT void fgt_drawing_over( void );	/* the shape being drawn, inverted over the window */
IMPORT void fgt_overlay( void );	/* the grid, the paper and 変形's handles over the figure */
IMPORT BOOL fgt_command( CONST char *code );	/* 変形, 用紙枠, 実寸, 用紙枠設定: TRUE when it was one */
IMPORT INT  fgt_paper_form( T_DEPAPER *pp );	/* 用紙設定: DE_PAPER_* */
IMPORT void rs_draw( void );		/* 実寸, when it is shown, drawn again */

/* ---------------------------------------------------------------- パターン編集 (fg_pat.c) */

IMPORT void fpe_open( INT pat );		/* on pattern pat of the figure; one open brought to the front */
IMPORT void fpe_close( void );
IMPORT BOOL fpe_event( CONST T_OBNTM *m );	/* a notice of its window or panel: TRUE when it was */
IMPORT BOOL fpe_shown( void );			/* its window is open */

#endif /* __FG_H__ */
