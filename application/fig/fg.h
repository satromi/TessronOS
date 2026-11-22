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
#include <ts/tvlist.h>

/* The panels of the program's data box (SYSDEF_BOX_FIG) */
#define FG_LOG		TA_LOG		/* a line of the log (note cuts it at a whole letter) */

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

/*
 * What the cabinet is, said by the file it is linked with (fg_box's
 * begin; 0 an ordinary 仮身一覧). Each is a program of its own, with its
 * program object and its executable:
 *   FG_AS_BASEBOX  the 原紙箱 (application/basebox/bb_main.c): the links
 *                  stay where they were laid out, neither moved nor sized
 *                  within it, left by 整頓, and one carried out to another
 *                  window becomes a new object made from it there
 *   FG_AS_ACCBOX   the 小物箱 (application/accbox/ab_main.c): the
 *                  accessories of the program box as the desktop orders
 *                  them, nothing in it changed but their order
 *   FG_AS_BASECOL  原紙集め (application/basecol/bc_main.c): the templates
 *                  of every application of the program box, a list the
 *                  program makes and no object's record. Its templates
 *                  are carried out as the 原紙箱's are (FG_TEMPLATES), and
 *                  nothing in it is changed or saved, as nothing in the
 *                  小物箱 is (FG_MADE)
 *   FG_AS_TOOL     one of the system's tools (application/systool:
 *                  屑実身操作, 実身/仮身検索, 仮身ネットワーク, ウインドウ一覧):
 *                  a list the program makes (FG_MADE) with the tool's
 *                  own menu. A link carried out of it is put there as a
 *                  copy and stays in the list; carried within, nothing
 *                  moves. Where the window stood is not kept
 */
extern UINT	fg_as;
#define FG_AS_BASEBOX	1
#define FG_AS_ACCBOX	2
#define FG_AS_BASECOL	3
#define FG_AS_TOOL	4
#define FG_BASEBOX	( fg_as == FG_AS_BASEBOX )
#define FG_ACCBOX	( fg_as == FG_AS_ACCBOX )
#define FG_BASECOL	( fg_as == FG_AS_BASECOL )
#define FG_TOOL		( fg_as == FG_AS_TOOL )
#define FG_TEMPLATES	( FG_BASEBOX || FG_BASECOL )	/* carried out: a new object made from it */
#define FG_MADE		( FG_ACCBOX || FG_BASECOL || FG_TOOL )	/* the list made by the program, sealed */

/* What the cabinet adds for the boxes (bb_main.c, ab_main.c, bc_main.c); NULL in 基本図形編集 and 仮身一覧, a member NULL none */
typedef struct {
	CONST char	*name;			/* the program's name, in what it says it cannot open */
	void	(*begin)( void );		/* started: fg_as set */
	ER	(*load)( T_TAD **p_rec );	/* the record of a list made (FG_MADE): the 小物箱's, 原紙集め's */
	void	(*listen)( void );		/* the window's channel made: told of changes to the list */
	void	(*menu)( ID mid, CONST T_VOBJ *v );	/* 小物管理 greyed for the link */
	BOOL	(*command)( CONST char *code, CONST T_VOBJ *v );	/* TRUE: the box's own item */
	void	(*place)( CONST TS_UUID *target, CONST TS_UUID *before );	/* carried within the 小物箱 */
	ER	(*start)( T_OBREF *p_ref );	/* the object the window stands for, in place of ta_start */
	void	(*shown)( void );		/* the window opened, or its work area sized again (g_w, g_h) */
	BOOL	(*changed)( CONST TS_UUID *u );	/* an object watched changed: TRUE when it was the box's */
	CONST char	*menu_def;		/* the window's menu (SYSDEF_MENU_*) in place of the cabinet's; "" none */
	CONST char	*(*key)( INT ch );	/* a letter with Ctrl that no item of the cabinet's has: the code it does */
	BOOL	(*open)( CONST T_VOBJ *v );	/* a link opened: TRUE when the box opened it its own way */
} T_FGBOX;

extern CONST T_FGBOX	*fg_box;

IMPORT void tell( CONST char *l1, CONST char *l2 );

/* ---------------------------------------------------------------- links (fg_link.c) */

/* What the figure gives the shared 仮身操作 (ts/tvvop.h), and a link of the record as they take it */
IMPORT CONST T_TVVOP *fg_vop( void );
IMPORT void fg_tvlink( T_TVLINK *l, CONST T_VOBJ *v, CONST T_DPRECT *box );	/* box NULL: v's own */
IMPORT CONST char *fg_link_name( CONST T_VOBJ *v );
/* 仮身操作, 実身操作 and 実行 set for link v under the pointer (NULL none): how many programs 実行 offers */
IMPORT INT  fg_link_menu( ID mid, CONST T_VOBJ *v );
/* One of those items done for link v, of code and index in 実行's list: FALSE when it is none of them */
IMPORT BOOL fg_link_op( CONST char *code, INT index, T_VOBJ *v );
IMPORT void fg_link_copy_put( CONST T_VOBJ *v, CONST TS_UUID *copy, INT dx, INT dy );
IMPORT void fg_open( CONST T_VOBJ *v );	/* opened as a double press opens it, or as the box opens it */

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
/* Whether a link taken may be deleted (the first window: a disk's link takes the disk away); NULL every one */
extern BOOL	(*fg_link_going)( CONST T_VOBJ *v );
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
IMPORT void fg_list_set( T_TAD *rec );	/* a list made shown in place of the one shown, scrolled as it was */

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
IMPORT void fg_text_left( void );		/* the pointer left the window carrying letters */
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
