/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc.h
 *	基本表計算: what the parts of the program share (design 11.13)
 *
 *	calc_main.c	the window, the person's presses and keys, the menu
 *	calc_draw.c	the grid on the screen and where things are on it
 *	calc_edit.c	a cell's words typed, かな漢字変換 with them
 *	calc_panel.c	the 操作パネル, a window of its own in front of the sheet's
 *	calc_obj.c	the object, its records, the desktop asked, the tray
 *	calc_chart.c	the charts as figures
 *	calc_test.c	the program's own test (started with "selftest")
 *	calc_tad.c	the sheet and its record
 *	calc_v8.cc or calc_ceval.c	the formulas
 */

#ifndef __CALC_H__
#define __CALC_H__

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/uiprn.h>
#include <ts/hid.h>
#include <ts/tray.h>
#include "calc_tad.h"
#include "calc_eng.h"

#define TICK_MS		20
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */
#define MOVE_MIN	3			/* a carry shorter than this is a press */

#define HEAD_W		50			/* the rows' numbers */
#define HEAD_H		24			/* the columns' letters */
#define FRAME_W		16
#define TITLE_H		34
#define MIN_W		400
#define MIN_H		300
#define EDGE		4			/* a header's edge sizes its row or column */
#define FILL_BOX	6			/* the fill handle */
#define CHART_MIN	50
#define LINK_W		150			/* a link's size in a cell when it says none */
#define LINK_H		25

#define CHUNK		( 64 * 1024 )
#define TEXT_MAX	4096			/* bytes of what a cell holds, typed */

/* The data box's panels and parts */
#define PN_FIND		2
#define PN_PAGE		3
#define PN_CLOSE	4
#define PN_WARN		5			/* what could not be done, told */

#define CP_FIND_TEXT	11
#define CP_FIND_REP	12
#define CP_FIND_RE	13
#define CP_FIND_SAID	14
#define CP_FIND_NEXT	101
#define CP_FIND_ONE	102
#define CP_FIND_ALL	103
#define CP_FIND_CLOSE	104

#define CP_PAGE_SIZE	11
#define CP_PAGE_TURN	12
#define CP_PAGE_MARGIN	13			/* top, bottom, left, right: mm */
#define CP_PAGE_CANCEL	101
#define CP_PAGE_OK	102

#define CP_CLOSE_CANCEL	101
#define CP_CLOSE_SAVE	102
#define CP_CLOSE_DROP	103

/* Keys: the keyboard's usages */
#define K_A		0x04
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_F2		0x3B
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

/* Colours on the screen */
#define C_PAPER		0x00FFFFFFU
#define C_GRID		0x00E0E0E0U
#define C_HEAD		0x00E8E8E8U
#define C_HEAD_PICK	0x00B0C4DEU
#define C_CORNER	0x00D4D4D4U
#define C_HEAD_LINE	0x00A0A0A0U
#define C_TEXT		0x00000000U
#define C_HEAD_TEXT	0x00333333U
#define C_RANGE		0x00E6F0FFU
#define C_CUR_FILL	0x00CCE5FFU
#define C_PICK		0x000050C0U
#define C_ERR_TEXT	0x00CC0000U
#define C_ERR_FILL	0x00FFE0E0U
#define C_LINK_CELL	0x00FFFFD0U
#define C_PAGE		0x00808080U
#define C_ORANGE	0x00FF8F00U

/* ---------------------------------------------------------------- state */

extern CXSHEET	g_s;
extern TS_UUID	g_obj;			/* the sheet's object */
extern UB	g_name[OB_NAME_MAX];
extern BOOL	g_autoref;		/* its store counts the links of its records itself */
extern UW	g_bg;			/* the object's paper (backgroundColor) */

extern TS_UUID	g_win;
extern ID	g_kw, g_port;
extern INT	g_gid, g_w, g_h;
extern UI	g_ui;
extern BOOL	g_dirty;		/* to be drawn again */
extern BOOL	g_changed;		/* not saved */
extern BOOL	g_closing;

extern INT	g_sx, g_sy;		/* how far the grid is scrolled */
extern INT	g_col, g_row;		/* the cell in hand */
extern INT	g_acol, g_arow;		/* the other corner of what is taken */
extern BOOL	g_cells_on;		/* cells are taken (not a chart) */
extern INT	g_chart;		/* the chart taken, -1 none */
extern BOOL	g_link_on;		/* the link in the cell in hand is taken */
extern BOOL	g_show_hidden;		/* 隠蔽仮身を表示 */
extern BOOL	g_paper_frame;		/* 用紙枠表示 */
extern BOOL	g_full;

/* What could not be done told in the data box's panel, two lines */
extern void warn( CONST char *a, CONST char *b );

/* Room for a text that grows (ct_buf_grow): realloc, and 0 bytes lets it go */
extern void *ct_more( void *p, SZ size );
extern char	*g_meta;		/* the metadata meta_of read last ("" when none) */
extern char	g_meta_none[1];

/* ---------------------------------------------------------------- calc_main.c */

extern void note( CONST char *s );
extern void say( CONST char *s );
extern void event( CONST T_OBNTM *m );
extern void command( CONST char *code, INT index, D value );
extern CONST CXSIDE *line_side( void );			/* the line the next frame is drawn with */
extern INT  g_panel_dx, g_panel_dy;			/* where the 操作パネル stands against the window */
extern BOOL g_panel_said;				/* the object says where it stands */

/* ---------------------------------------------------------------- calc_draw.c */

extern void range_of( CXRANGE *r );			/* what is taken */
extern INT  col_x( INT col );				/* from the grid's left, before scrolling */
extern INT  row_y( INT row );
extern INT  col_at( INT x );				/* the column under a grid x */
extern INT  row_at( INT y );
extern INT  grid_left( void );
extern INT  grid_top( void );
extern BOOL cell_box( INT col, INT row, T_DPRECT *r );	/* on the screen, the join it is in */
extern void link_box( CONST CXCELL *c, T_DPRECT *r );	/* a link's or image's box on the screen */
extern void chart_box( INT i, T_DPRECT *r );
extern void scroll_to( INT col, INT row );		/* the cell brought into sight */
extern void bars( void );				/* the scroll bars told */
extern void paint( void );
extern UW   colour_value( CONST char *s, UW dflt );	/* "#rrggbb" as a colour */

/* ---------------------------------------------------------------- calc_edit.c */

extern BOOL ed_on( void );
extern void ed_begin( INT col, INT row, CONST UB *first, BOOL keep );
extern BOOL ed_key( UINT key, UINT mods, UINT mode );		/* TRUE when it was the editor's */
extern void ed_commit( void );				/* what is typed put in the cell */
extern void ed_cancel( void );
extern void ed_pick( INT row );			/* a press on the system's list of candidates (OB_WR_CAND) */
extern void ed_draw( void );
extern void ed_end( void );
extern INT  ed_text( UB *out, INT max );		/* what is typed in the cell now, for the panel's box */

/* ---------------------------------------------------------------- calc_panel.c */

extern void panel_open( void );
extern void panel_close( void );
extern BOOL panel_on( void );
extern BOOL panel_is( CONST TS_UUID *u );		/* a notice from the panel's window */
extern BOOL panel_event( CONST T_OBNTM *m );		/* FALSE: what the panel leaves to the sheet */
extern void panel_draw( void );
extern INT  panel_test( INT *p_all );			/* calc_test.c: the parts checked; how many passed */

/* ---------------------------------------------------------------- calc_obj.c */

extern void shown_all( void );				/* every cell's words taken from the engine */
extern void cell_text( INT col, INT row, CONST UB *text );	/* a cell given what is typed */
extern void cell_clear( INT col, INT row );
extern ER   load( void );
extern ER   save( void );
extern INT  meta_of( CONST TS_UUID *u );
extern void name_of( CONST TS_UUID *u, UB *out );
extern BOOL link_target( CONST CXCELL *c, TS_UUID *u );
extern ER   ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms );
extern void link_open( INT col, INT row );
extern void link_op( CONST char *code, INT index );
extern void exec_menu( ID mid );
extern BOOL app_kept( void );
extern void dropped( void );
extern void carry_out( INT x0, INT y0, BOOL twice );
extern void copy_cells( BOOL cut );
extern void paste_cells( BOOL take );
extern BOOL tray_has( void );
extern void tray_menu( ID mid );
extern void paper_ask( void );
extern void new_vobjid( TS_UUID *u );

/* ---------------------------------------------------------------- calc_chart.c */

extern void chart_xml( CTBUF *b, CONST CXSHEET *s, INT i, void *arg );	/* as the record keeps it */
extern void chart_draw( INT i );
extern BOOL chart_insert( INT kind );
extern BOOL chart_frag( CTBUF *b, INT i );
extern void chart_print( CTBUF *b, CONST CXSHEET *s, INT i, INT dx, INT dy, void *arg );	/* on a page of 印刷 */
extern ER   print_sheet( void );					/* 印刷 */
extern double cell_number( CONST CXSHEET *s, INT col, INT row );

/* ---------------------------------------------------------------- calc_test.c */

extern INT  selftest( void );

#endif /* __CALC_H__ */
