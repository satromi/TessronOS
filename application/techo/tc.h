/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tc.h
 *	電子手帳: what the parts of the program share (design 11.23)
 *
 *	tc_main.c	the object, the windows, the menu, the commands, the keys,
 *			the 検索パネル, the panels asked, 印刷
 *	tc_draw.c	the three windows drawn, and where their parts are
 *	tc_edit.c	a 記入欄 typed into, かな漢字変換 with it, letters taken
 *	tc_db.c		what the object holds (tc_db.h)
 */

#ifndef __TC_H__
#define __TC_H__

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
#include "tc_db.h"

#define TICK_MS		20
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */
#define MOVE_MIN	4			/* a carry shorter than this is a press */
#define FRAME_W		16
#define TITLE_H		34
#define OUT_MAX		( 4 * 1024 * 1024 )
#define CHUNK		( 64 * 1024 )

/* The windows: one for each 機能 */
#define W_SCHED		0			/* 予定表 */
#define W_CAL		1			/* カレンダー */
#define W_ADDR		2			/* 住所録 */
#define W_N		3

/* The size each window's work area opens at, and the least it may be */
#define SCHED_W		400
#define SCHED_H		306
#define CAL_W		270
#define CAL_H		222
#define ADDR_W		420
#define ADDR_H		308

/* What the fields are drawn with */
#define FIELD_PX	16
#define ROW_H		26

/* Keys: the keyboard's usages */
#define K_A		0x04
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_DEL		0x4C
#define K_HOME		0x4A
#define K_PGUP		0x4B
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
#define C_BAND		0x00E8E8E8U
#define C_TEXT		0x00000000U
#define C_LABEL		0x00404040U
#define C_RULE		0x00A0A0A0U
#define C_LINE		0x00606060U
#define C_HOLIDAY	0x00C00000U
#define C_SATURDAY	0x000000A0U
#define C_PICK		0x000050C0U
#define C_SEL		0x00B8D0F0U
#define C_GREY		0x00A0A0A0U

/* The parts of a window that are not 記入欄 (draw_part_at) */
#define PT_NONE		-1
#define PT_PREV		-2			/* the 前へ mark */
#define PT_NEXT		-3			/* the 次へ mark */
#define PT_MONTH	-4			/* the カレンダー's year and month */
#define PT_INDEX	-5			/* the 住所録's index */
#define PT_DAY		-100			/* a day of the カレンダー: PT_DAY - d */

typedef struct {
	BOOL	open, dirty;
	TS_UUID	win;
	ID	kw;
	INT	gid, w, h;
	INT	x, y;			/* where it opens again */
} TCWIN;

/* ---------------------------------------------------------------- tc_main.c */

IMPORT TCDB	g_db;
IMPORT TS_UUID	g_obj;
IMPORT UB	g_name[OB_NAME_MAX];
IMPORT TCWIN	g_w[W_N];
IMPORT UI	g_ui;
IMPORT ID	g_port;
IMPORT BOOL	g_changed, g_closing;
IMPORT INT	g_day;			/* the day the 予定表 shows */
IMPORT INT	g_cy, g_cm;		/* the month the カレンダー shows */
IMPORT INT	g_cur;			/* the person the 住所録 shows */
IMPORT INT	g_today;
IMPORT UB	g_rows[TC_ROWS][TC_LINE_MAX];	/* the rows of the day shown */
IMPORT BOOL	g_idx_open;		/* the 住所録's list of indexes is out */
IMPORT INT	g_drop_w, g_drop_f;	/* the 記入欄 letters carried would go into; -1 none */
IMPORT INT	g_drop_x;		/* and where in it */
IMPORT UB	g_out[OUT_MAX];

IMPORT void note( CONST char *s );
IMPORT void say( CONST char *s );
IMPORT void command( CONST char *code, INT index );
/* What a 記入欄 holds as the window shows it */
IMPORT CONST UB *field_text( INT w, INT f );
/* What was typed into a 記入欄, kept */
IMPORT void field_commit( INT w, INT f, CONST UB *s, INT n );
IMPORT INT  field_count( INT w );

/* ---------------------------------------------------------------- tc_draw.c */

IMPORT void draw( INT w );
IMPORT BOOL field_box( INT w, INT f, T_DPRECT *r );
/* The 記入欄 at a place (0 up), or PT_* */
IMPORT INT  part_at( INT w, INT x, INT y );
IMPORT INT  index_at( INT x, INT y );		/* the list of indexes: TC_IDX_*, -1 */

/* ---------------------------------------------------------------- tc_edit.c */

IMPORT BOOL ed_on( void );
IMPORT INT  ed_win( void );
IMPORT INT  ed_field( void );
IMPORT void ed_begin( INT w, INT f, CONST UB *text );
IMPORT void ed_commit( void );
IMPORT void ed_cancel( void );
IMPORT void ed_end( void );
IMPORT BOOL ed_key( UINT key, UINT mods, UINT mode );
IMPORT void ed_insert( CONST UB *s, INT n );
IMPORT void ed_draw( void );
IMPORT void ed_pick( INT row );		/* a press on the system's list of candidates (OB_WR_CAND) */
IMPORT BOOL ed_busy( void );
/* The caret put where x is (in the window), and with 'extend' the letters from the anchor taken */
IMPORT void ed_caret_at( INT x, BOOL extend );
/* The letters taken: whether there are any, whether x is over them, and they themselves */
IMPORT BOOL ed_has_sel( void );
IMPORT BOOL ed_in_sel( INT x );
IMPORT INT  ed_sel_text( UB *out, INT max );
IMPORT void ed_sel_delete( void );
IMPORT void ed_select_all( void );
IMPORT INT  ed_sel_from( void );		/* where the letters taken begin */
/* Where x (in the window) falls among the letters typed, and the caret put at a byte offset */
IMPORT INT  ed_offset_at( INT x );
IMPORT void ed_set_caret( INT at );
/* Where x falls among the letters of a 記入欄 that is not being typed into: a byte offset */
IMPORT INT  text_offset( CONST UB *s, INT x, INT px );

#endif /* __TC_H__ */
