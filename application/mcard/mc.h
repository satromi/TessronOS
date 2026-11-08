/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc.h
 *	マイクロカード: what the parts of the program share (design 11.18)
 *
 *	mc_main.c	the windows, the menu, the commands, the keys
 *	mc_card.c	the card window: カード画面, 検索条件画面, 設計画面
 *	mc_list.c	the list window
 *	mc_edit.c	a box typed into, かな漢字変換 with it
 *	mc_dlg.c	the panels asked (表示設定, 並べ替え, 印刷 ...)
 *	mc_obj.c	the object and its records, the desktop asked, the tray,
 *			writing out, reading in, the merged pages as an object
 *	mc_db.c mc_bin.c mc_rx.c mc_var.c mc_io.c mc_merge.c	the cards (mc_db.h)
 */

#ifndef __MC_H__
#define __MC_H__

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/uiprn.h>
#include <ts/hid.h>
#include <ts/tray.h>
#include <ts/wm.h>
#include <ts/vobjsz.h>
#include "mc_db.h"

#define TICK_MS		20
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */
#define MOVE_MIN	4			/* a carry shorter than this is a press */
#define FRAME_W		16
#define TITLE_H		34
#define MIN_W		360
#define MIN_H		240
#define OUT_MAX		( 8 * 1024 * 1024 )
#define CHUNK		( 64 * 1024 )
#define LINK_W		180			/* a link in a field when it says no size */
#define LINK_H		VOBJ_CLOSED_H(0, 1, 1)		/* closed, at 14 */

/* The card window's screens */
#define SCR_CARD	0			/* カード画面 */
#define SCR_FIND	1			/* 検索条件画面 */
#define SCR_DESIGN	2			/* 設計画面 */

/* Keys: the keyboard's usages */
#define K_A		0x04
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_F10		0x43
#define K_HOME		0x4A
#define K_PGUP		0x4B
#define K_DEL		0x4C
#define K_END		0x4D
#define K_PGDN		0x4E
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define K_KPENTER	0x58
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define M_ALT		( HID_MOD_LALT | HID_MOD_RALT )

/* Colours */
#define C_PAPER		0x00FFFFFFU
#define C_GROUND	0x00E8ECF0U		/* round the boxes */
#define C_BAR		0x00D8DDE4U		/* the bar along the top */
#define C_LINE		0x00A0A8B0U
#define C_TEXT		0x00000000U
#define C_GREY		0x00707070U
#define C_PICK		0x000050C0U
#define C_ROW		0x00C8DCF5U		/* the list's row in hand */
#define C_FIND		0x00FFF4D0U		/* the bar in the search state */
#define C_DESIGN	0x00E4F2E4U
#define C_BUTTON	0x00F4F4F4U
#define C_ORANGE	0x00FF8F00U

/* ---------------------------------------------------------------- a window of the program */

typedef struct {
	TS_UUID	win;
	ID	kw;
	INT	gid;
	INT	w, h;			/* the work area */
	BOOL	open;
	BOOL	dirty;
} MCWIN;

/* A box that can be typed into */
#define ED_NONE		0
#define ED_VALUE	1			/* a field of the card in hand */
#define ED_COND		2			/* a field's condition */
#define ED_DNAME	3			/* 設計画面: a name */
#define ED_DMAX		4			/* the largest size */
#define ED_DWIDTH	5			/* the width in the list */

/* ---------------------------------------------------------------- state */

extern MCDB	g_db;
extern TS_UUID	g_obj;			/* the database's object */
extern UB	g_name[OB_NAME_MAX];
extern BOOL	g_autoref;		/* its store counts the links of its records itself */
extern BOOL	g_changed;		/* not saved */
extern BOOL	g_closing;
extern ID	g_port;
extern UI	g_ui;			/* the data box's panels, asked over the card window */
extern MCWIN	g_cw, g_lw;		/* the card window, the list window */
extern INT	g_screen;		/* SCR_* */
extern INT	g_cur;			/* the place of the card shown, among those worked on */
extern INT	g_field;		/* the field in hand, -1 none */
extern BOOL	g_link_on;		/* the link in the field in hand is taken */
extern UB	*g_cond[MC_FIELD_MAX];	/* what is written for each field to search with */
extern UB	g_out[OUT_MAX];
extern char	*g_meta;		/* the metadata meta_of read last ("" when none) */
extern char	g_meta_none[1];

/* 設計画面: the fields as they are being made */
extern MCFIELD	g_dsf[MC_FIELD_MAX + 1];
extern INT	g_dsfrom[MC_FIELD_MAX + 1];	/* the field each came from, -1 new */
extern INT	g_dsn;				/* rows: the fields, and one empty after them */
extern UB	*g_dslayout;			/* the 印刷書式 as it is being set */
extern INT	g_dslayoutlen;

/* ---------------------------------------------------------------- mc_main.c */

extern void note( CONST char *s );		/* to the console, for the tests */
extern void say( CONST char *s );		/* on the message line */
extern void warn( CONST char *a, CONST char *b );
extern void command( CONST char *code, INT index );
extern void event( CONST T_OBNTM *m );
extern void show_card( INT pos );		/* the card at a place shown, the list's row with it */
extern MCCARD *cur_card( void );
extern INT  cur_index( void );
extern void redraw_all( void );
extern void win_front( MCWIN *w );
extern void list_open( void );
extern void list_close( void );
extern INT  card_px( void );			/* the card's letters, in dots */
extern INT  list_px( void );

/* ---------------------------------------------------------------- mc_card.c */

extern void card_paint( void );
extern void card_event( CONST T_OBNTM *m );
extern BOOL card_bar_event( CONST T_OBNTM *m );	/* a notice of the bar along the top: TRUE when it was */
extern void card_bar_close( void );
extern void card_key( CONST T_OBNTM *m );
extern BOOL card_box( INT kind, INT k, T_DPRECT *r );	/* where a box is in the window */
extern void card_commit( INT kind, INT k, CONST UB *s, INT n );
extern void card_scroll_to( INT k );
extern void card_screen( INT scr );			/* the card window shows another screen */
extern void card_dropped( void );
extern void design_begin( void );
extern BOOL design_apply( void );
extern void design_cancel( void );
extern INT  field_at_link( INT x, INT y );		/* the field whose link is there, -1 */
extern void link_box( INT f, T_DPRECT *r );

/* ---------------------------------------------------------------- mc_list.c */

extern void list_paint( void );
extern void list_event( CONST T_OBNTM *m );
extern void list_show_cur( void );
extern void list_dropped( void );
extern BOOL list_holding( void );			/* a row is being carried */

/* ---------------------------------------------------------------- mc_edit.c */

extern BOOL ed_on( void );
extern INT  ed_kind( void );
extern INT  ed_index( void );
extern void ed_begin( INT kind, INT k, CONST UB *text, BOOL all );
extern BOOL ed_key( UINT key, UINT mods, UINT mode );		/* TRUE when it was the editor's */
extern void ed_commit( void );
extern void ed_cancel( void );
extern void ed_end( void );
extern void ed_pick( INT row );		/* a press on the system's list of candidates (OB_WR_CAND) */
extern void ed_draw( void );
extern void ed_insert( CONST UB *s, INT n );		/* letters put in at the caret */
extern void ed_caret_at( INT x );
extern INT  ed_text( UB *out, INT max );		/* what is typed now */

/* ---------------------------------------------------------------- mc_dlg.c */

extern void dlg_view( void );
extern void dlg_status( void );
extern void dlg_sort( void );
extern void dlg_dups( void );
extern void dlg_export( void );
extern BOOL dlg_delete( CONST char *what );
extern INT  dlg_close( void );				/* 1 save, 2 not, 0 stay */
extern BOOL dlg_format( INT lost );
extern void dlg_print( void );
extern void dlg_print_setup( void );

#define DLG_SAVE	1
#define DLG_DROP	2

/* ---------------------------------------------------------------- mc_obj.c */

extern ER   load( void );
extern ER   save( void );
extern INT  meta_of( CONST TS_UUID *u );
extern void name_of( CONST TS_UUID *u, UB *out );
extern BOOL link_target( CONST UB *el, TS_UUID *u );
extern ER   ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms );
extern void link_open_el( CONST UB *el );
extern void link_op( CONST char *code, INT index );
extern void exec_menu( ID mid );
extern BOOL app_kept( void );
extern void tray_menu( ID mid );
extern BOOL tray_has( void );
extern void carry_link_out( INT x0, INT y0, BOOL twice );
extern void carry_rows_out( INT from, INT to );
extern UB  *link_el_from_drop( CONST T_OBDROPV *v, INT *p_len );
extern INT  import_obj( CONST TS_UUID *u, INT at );	/* cards read in from an object; how many */
extern void tray_copy_field( BOOL cut );
extern void tray_paste_field( BOOL take );
extern void tray_copy_rows( INT from, INT to );
extern void tray_paste_rows( BOOL take );
extern ER   export_obj( INT form, CONST UB *name );	/* the cards worked on to a new object */
extern ER   merge_print( CONST UB *layout_name, BOOL one );	/* 印刷: the pages to the desktop to print */
extern ER   merge_obj( CONST UB *layout_name, BOOL one );	/* 差し込み: the pages as a new object */
extern INT  layout_names( UB (*names)[MC_LAYOUT_NAME], TS_UUID *objs, INT max );
extern ER   paper_ask( void );
extern void new_vobjid( TS_UUID *u );

/* The forms written out */
#define EX_MCARD	0
#define EX_AT		1
#define EX_SHEET	2
#define EX_CSV		3

#endif /* __MC_H__ */
