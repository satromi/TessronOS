/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docmenu.h
 *	基本文章編集 の menu (design 17.2)
 *
 *	The menu of a document is its definition (DOCMENU.DEF, design 18.15).
 *	What is here is what the editor knows of it: the state the marks
 *	and the grey items follow, and which of the editor's commands each
 *	item's code is.
 *
 *	The state is passed in rather than read out of the viewer: which of
 *	the three views is on, whether the text is wrapped, whether hidden
 *	links are shown, whether anything is picked. A menu set from stale
 *	state is a menu that lies about what the document is doing.
 */

#ifndef __TS_DOCMENU_H__
#define __TS_DOCMENU_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/part.h>
#include <ts/mn.h>

/* Which view the document is being shown in */
#define DM_VIEW_XML	0		/* the record as it is written */
#define DM_VIEW_DETAIL	1		/* with what is usually invisible */
#define DM_VIEW_CLEAN	2		/* as it would be printed */

typedef struct {
	INT	view;			/* DM_VIEW_* */
	BOOL	wrap;			/* wrapped to the window's width */
	BOOL	show_hidden;		/* hidden links shown */
	BOOL	paper_frame;		/* the paper's edge drawn */
	BOOL	has_pick;		/* something is picked */
	BOOL	is_root;		/* the document has no link to it */
	BOOL	can_paste;		/* the tray has a set a text can take */

	/* 書体: the faces used last, newest first, as <font face> names them */
	INT	nrecent;
	UB	recent[10][WM_LABEL_MAX];
} T_DOCMENU;

/* ------------------------------------------------------------- the commands */

/* 保存 */
#define DM_SAVE			100
#define DM_SAVE_AS_NEW		101

/* 表示 */
#define DM_VIEW_SET_XML		110
#define DM_VIEW_SET_DETAIL	111
#define DM_VIEW_SET_CLEAN	112
#define DM_FULLSCREEN		113
#define DM_REFRESH		114
#define DM_BGCOLOUR		115
#define DM_SHOW_HIDDEN		116
#define DM_WRAP			117

/* 編集 */
#define DM_UNDO			120
#define DM_COPY			121
#define DM_PASTE		122
#define DM_CUT			123
#define DM_MOVE_BACK		124
#define DM_SELECT_ALL		125
#define DM_FIND			126
#define DM_VIRTUALIZE		127

/* 書体 */
#define DM_FONT_LIST		130
#define DM_FONT_GOTHIC		131
#define DM_FONT_MINCHO		132
#define DM_FONT_MEIRYO		133

/* 文字修飾 */
#define DM_STYLE_NORMAL		140
#define DM_STYLE_BOLD		141
#define DM_STYLE_ITALIC		142
#define DM_STYLE_BAGCHAR	143
#define DM_STYLE_BOX		144
#define DM_STYLE_SHADOW		145
#define DM_STYLE_UNDERLINE	146
#define DM_STYLE_OVERLINE	147
#define DM_STYLE_STRIKE		148
#define DM_STYLE_HATCH		149
#define DM_STYLE_INVERSE	150
#define DM_STYLE_NOPRINT	151
#define DM_STYLE_SUPER		152
#define DM_STYLE_SUB		153
#define DM_STYLE_INDENT		154
#define DM_STYLE_CLEAR		155

/*
 * 文字サイズ. The command carries the size itself: a menu of sizes that
 * numbered them 1, 2, 3 would have to be read back through a table, and
 * the table would be a second place the sizes are written down. Sizes
 * that are not whole numbers are carried in tenths.
 */
#define DM_SIZE_BASE		1000		/* + the size in tenths */
#define DM_SIZE(tenths)		( DM_SIZE_BASE + (tenths) )
#define DM_SIZE_TENTHS(cmd)	( (cmd) - DM_SIZE_BASE )
#define DM_SIZE_CUSTOM		160
#define DM_SIZE_FULLWIDTH	161
#define DM_SIZE_HALFWIDTH	162

/* 文字色 */
#define DM_COLOUR_BLACK		170
#define DM_COLOUR_BLUE		171
#define DM_COLOUR_RED		172
#define DM_COLOUR_PINK		173
#define DM_COLOUR_ORANGE	174
#define DM_COLOUR_GREEN		175
#define DM_COLOUR_LIME		176
#define DM_COLOUR_CYAN		177
#define DM_COLOUR_YELLOW	178
#define DM_COLOUR_WHITE		179
#define DM_COLOUR_CUSTOM	180

/* 書式 */
#define DM_FMT_NEW_TAB		190
#define DM_FMT_INDENT		191
#define DM_ALIGN_LEFT		192
#define DM_ALIGN_CENTRE		193
#define DM_ALIGN_RIGHT		194
#define DM_SPACE_0		195		/* and the six below it */
#define DM_SPACE_1_16		196
#define DM_SPACE_1_8		197
#define DM_SPACE_1_4		198
#define DM_SPACE_3_8		199
#define DM_SPACE_1_2		200
#define DM_SPACE_3_4		201
#define DM_RUBY_TOP		202
#define DM_RUBY_BOTTOM		203
#define DM_PAGEBREAK		204
#define DM_PAPER_FRAME		205
#define DM_PAGE_SETUP		206

/* 書体: DM_FONT_RECENT + the number of the face among those used last */
#define DM_FONT_RECENT		240
#define DM_RECENT_MAX		10

/* ------------------------------------------------------------- the calls */

/* The document's menu made from its definition, its items set to st */
IMPORT ER  doc_menu_make( CONST T_DOCMENU *st, ID *p_mid );

/* The command a chosen item is, or 0 when it is not one of the editor's */
IMPORT INT doc_menu_cmd( CONST T_MNSEL *sel );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DOCMENU_H__ */
