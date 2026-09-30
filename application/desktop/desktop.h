/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	desktop.h
 *	What the two halves of the desktop share (design 16.5.12)
 *
 *	desktop.c keeps the windows and reads what the person does.
 *	dtedit.c is what can be done to the virtual objects in a figure:
 *	taking them, carrying them, sizing them, throwing them away,
 *	copying them, and taking any of that back. It is the figure
 *	editor's handling of virtual objects, and it is the same for a
 *	cabinet because a cabinet is a figure.
 *
 *	One rule holds everything together: **the record is the only copy
 *	of the object.** Every change is made to the record, and the
 *	model that is drawn is made again from the record afterwards.
 *	Changing the model and the record separately is two copies that
 *	can disagree; taking a change back is then putting back the
 *	record, and saving is writing it.
 */

#ifndef __DESKTOP_H__
#define __DESKTOP_H__

#include <ts/wm.h>
#include <ts/part.h>
#include <ts/tad.h>
#include <ts/dtreq.h>
#include <ts/tadview.h>
#include <ts/mn.h>
#include <ts/ob.h>

#define DT_MAX_WIN	8		/* windows this program may have open */
#define DT_MARGIN	10		/* between the work area and the page */
#define DT_UNDO_MAX	16		/* changes that can be taken back */
#define DT_PAPER_W	595		/* A4, when a document names no paper */
#define DT_PAPER_H	842

/* One window this program has open, and what is being shown in it */
typedef struct dtwin {
	BOOL		used;
	INT		wid;
	ID		okey;		/* the window as an object, watched for OB_E_CLOSE */
	T_TAD		*rec;		/* the store's, not ours */
	UINT		kind;		/* TV_KIND_* */

	/* what the record comes to, made again after every change */
	T_TVDOC		*doc;
	T_TVFIG		*fig;
	UW		paper;		/* what the object is written on */

	/* which object it is, and whether it has changed since it was saved */
	TS_UUID		id;
	INT		recno;
	BOOL		dirty;

	/*
	 * A window whose page is made here rather than read: the 原紙箱's
	 * list of the templates this system's programs start from. Nothing
	 * in it is changed or saved; what is carried out of it is a new
	 * object made from the template.
	 */
	BOOL		sealed;

	/*
	 * A window of one of the system's own tools -- 屑実身操作,
	 * 実身/仮身検索, 仮身ネットワーク -- rather than of an object. Its
	 * page is made by the tool (dttool.c) and is sealed; the menu and
	 * the keys are the tool's.
	 */
	UINT		tool;

	/* a figure the figure editor works on: every shape, and the tools */
	BOOL		figed;

	/*
	 * The virtual object it was opened from, and the record that holds
	 * that one: where 新たな実身に保存 puts the link to the new object.
	 * A window opened some other way has none.
	 */
	BOOL		has_parent;
	TS_UUID		parent_id;
	TS_UUID		parent_vobj;

	/* 全画面表示, and where the window stood before it */
	BOOL		full;
	T_DPRECT	restore;

	INT		scroll_x, scroll_y;
	INT		height;		/* what the content came to */
	INT		width;		/* and how wide: a figure's; 0 goes no wider than the page */

	/*
	 * How a document is shown (docmenu.h): 原稿, the record as text
	 * that can be edited; 詳細, with the marks of what is not
	 * printed; or neither, 清書. Wrapped to the window or to the
	 * paper, the paper's edge and pages drawn, hidden links shown.
	 * In 原稿 the text being edited is a record of its own, one
	 * paragraph for each line, read back into the object on leaving.
	 */
	BOOL		xml_view;
	BOOL		detail_view;
	BOOL		nowrap;
	BOOL		paper_frame;
	BOOL		show_hidden;
	T_TAD		*xrec;
	BOOL		xml_edited;

	/*
	 * A piece of text in a figure being edited where it stands. The
	 * figure's window has 'tb', an editor of text of its own that is in
	 * no list of windows: its record is the figure's, its document is
	 * made from the <document> element 'tb_node', its page is the box
	 * the text stands in, and 'tb_host' is the figure's window.
	 */
	struct dtwin	*tb;
	struct dtwin	*tb_host;
	T_TADNODE	*tb_node;

	/*
	 * A document's caret and the other end of its selection, each a
	 * paragraph and a count into it (dtdoc.c); the same place when
	 * nothing is selected. 'typing' is set while what is being done
	 * is letters typed one after another, which are taken back as one.
	 */
	INT		cpara, cpos;
	INT		apara, apos;
	BOOL		typing;
	UB		name[TAD_NAME_MAX];

	/*
	 * The shapes that are picked: 0 when not, and otherwise the order
	 * they were picked in, from 1. 整頓 goes by that order: the first
	 * one taken is the one the others are made like.
	 */
	UH		pick[TV_MAX_SHAPE];
	INT		npick;
	UH		pick_seq;

	/*
	 * The record as it was before each change, newest last, and the
	 * changes taken back, newest last. Text, because a record put
	 * back from text is exactly the record that was.
	 */
	UB		*undo[DT_UNDO_MAX];
	SZ		undo_len[DT_UNDO_MAX];
	INT		nundo;
	UB		*redo[DT_UNDO_MAX];
	SZ		redo_len[DT_UNDO_MAX];
	INT		nredo;
} DTWIN;

#define DT_TOOL_NONE	0
#define DT_TOOL_TRASH	1		/* 屑実身操作 */
#define DT_TOOL_SEARCH	2		/* 実身/仮身検索 */
#define DT_TOOL_NETWORK	3		/* 仮身ネットワーク */

/* desktop.c */
IMPORT DTWIN *dt_win_of( INT wid );

/*
 * dttray.c: what a window copies goes to the tray (design 18.16) as an
 * xmlTAD fragment, a <figure> of shapes and links or a <document> of
 * text; each kind of window takes from the set in hand what it can.
 */
#define DT_TRAY_NONE	0
#define DT_TRAY_FIG	1
#define DT_TRAY_DOC	2
IMPORT T_TAD *dt_frag_new( CONST char *body );
IMPORT ER     dt_tray_put( CONST T_TAD *frag, CONST char *name );
IMPORT T_TAD *dt_tray_frag( void );
IMPORT UINT   dt_tray_kind( INT *p_links );
IMPORT INT    dt_frag_shapes( CONST T_TAD *frag );
IMPORT void   dt_tray_taken( void );
IMPORT T_TAD *dt_list_record( CONST char *title, CONST TS_UUID *ids,
			      CONST char *CONST *names, INT n );
IMPORT DTWIN *dt_open_made( T_TAD *rec, CONST char *title, UINT tool,
			    CONST T_DPRECT *o );
IMPORT void   dt_remade( DTWIN *d, T_TAD *rec );
IMPORT DTWIN *dt_tool_window( UINT tool );
IMPORT INT    dt_roots( TS_UUID *ids, INT max );
IMPORT void   dt_work_rect( CONST DTWIN *d, T_DPRECT *r );
IMPORT void   dt_draw( DTWIN *d );
IMPORT void   dt_draw_all( void );
IMPORT CONST char *dt_base_name( CONST TS_UUID *target );
IMPORT void   dt_open_vobj( DTWIN *from, CONST T_VOBJ *v );
IMPORT ER     dt_open_as( DTWIN *from, CONST T_VOBJ *v, CONST UB *app );
IMPORT void   dt_exec( DTWIN *from, CONST T_VOBJ *v, INT index );

/*
 * Links let go of over a window that is not the desktop's (design
 * 16.5.21): handed to the program there when it takes drops, and a
 * word on the message line when it does not. TRUE when it was such a
 * window, and nothing else is to be done with the links.
 */
IMPORT BOOL   dt_drop_links( DTWIN *d, INT sx, INT sy, CONST T_VOBJ *v, INT n );

/* The desktop run in a task of its own, and stopped again (the tests) */
IMPORT ER     dt_start( void );
IMPORT void   dt_quit( void );

/* The programs as real objects (dtprog.c, design 18.12) */
#define DT_PROG_ID	40
#define DT_PROG_BASE	4
#define DT_VER_MAX	16		/* R, a number, a point and three digits */
#define DT_PK_APP	1		/* an application: templates, opens objects */
#define DT_PK_ACCESSORY	2		/* 小物: no data of its own */
#define DT_PK_UTILITY	3		/* a tool of the system */

typedef struct {
	UB	id[DT_PROG_ID];		/* what an applist names it by */
	UB	name[TAD_NAME_MAX];
	UINT	kind;			/* DT_PK_* */
	TS_UUID	uuid;			/* the program object */
	INT	nbase;
	TS_UUID	base[DT_PROG_BASE];	/* its templates (原紙) */
	BOOL	builtin;		/* carried in the desktop */
	BOOL	menu;			/* an accessory shown in the 小物 menu */
	UB	version[DT_VER_MAX];	/* R1.000; empty when its metadata says none */
} DTPROG;

IMPORT ER     dt_prog_start( void );
IMPORT CONST DTPROG *dt_prog_find( CONST UB *id );
IMPORT CONST DTPROG *dt_prog_of( CONST TS_UUID *uuid );
IMPORT INT    dt_prog_list( UINT kind, CONST DTPROG **out, INT max );
IMPORT T_TAD *dt_prog_page( UINT kind );
IMPORT INT    dt_prog_roots( TS_UUID *ids, INT max );
IMPORT CONST char *dt_prog_base_name( CONST TS_UUID *target );
/*
 * A program run. One that is a process is started by a task of its own
 * (dtprog.c): E_OK means it was handed over, and the desktop goes on
 * while the program is read in.
 */
IMPORT ER     dt_prog_run( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v );
/*
 * The answer to a request of a process (T_DTANS, 'size' bytes of it),
 * handed to the next process dt_prog_run starts: the task that starts
 * it answers at 'reply' once the process is there, with the process,
 * and with the error of the start when 'with_er'. NULL takes back what
 * no start took. dt_prog_reply_taken says whether a start took it.
 */
IMPORT void   dt_prog_reply( CONST TS_UUID *reply, UINT seq, SZ size, BOOL with_er );
IMPORT BOOL   dt_prog_reply_taken( void );
/* An answer written at a channel (desktop.c) */
IMPORT void   dt_answer( CONST TS_UUID *reply, CONST T_DTANS *an, SZ size );

/* The program box read again: what the registry knows is what the box links to */
IMPORT void   dt_prog_reread( void );

/*
 * dtpkg.c: applications installed from a package (.tpk), updated and
 * deleted (design 18.12). A version R1.000 as a number to compare
 * (1000), -1 when it is not one.
 */
IMPORT D      dt_ver_num( CONST UB *v );
#define DT_PKG_NEW	DT_HOW_NEW
#define DT_PKG_UPDATED	DT_HOW_UPDATED
IMPORT ER     dt_pkg_install( CONST TS_UUID *pkg, BOOL force, TS_UUID *p_prog, UINT *p_how );
IMPORT ER     dt_prog_remove( CONST TS_UUID *prog );

/* A TADjs plugin directory taken in as a program and its template (design 18.12) */
IMPORT ER     dt_plugin_import( CONST char *dir, TS_UUID *p_prog );

/* dtres.c: the faces and the pictures of the ground as objects (design 18.18) */
IMPORT INT    dt_res_faces( void );
IMPORT ER     dt_res_wall( UINT mode );
IMPORT DTWIN *dt_showing( CONST TS_UUID *id );
IMPORT ER     dt_save( DTWIN *d );
IMPORT void   dt_close( DTWIN *d );
IMPORT void   dt_fullscreen( DTWIN *d );
IMPORT ER     dt_screen_size( UINT *p_w, UINT *p_h );	/* through the screen object 画面 */
IMPORT INT    dt_run_menu( DTWIN *d, INT pid, UD when );

/* dtsys.c: the protection of what the system owns */
IMPORT void   dt_sys_prt( T_OBPRT *prt );		/* owned by the system, read by all */

/*
 * dtvmn.c: the items every window's menu has (design 18.15). dt_get_vmn
 * sets them for the virtual object the menu was opened on; dt_exe_vmn
 * does one of them and answers TRUE, FALSE when the item is not one.
 */
IMPORT void   dt_get_vmn( DTWIN *d, ID mid, BOOL on_vobj, CONST T_VOBJ *v );
IMPORT BOOL   dt_exe_vmn( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			  CONST T_VOBJ *v );
IMPORT BOOL   dt_app_locked( CONST T_VOBJ *v );
IMPORT BOOL   dt_app_lockable( CONST T_VOBJ *v );
IMPORT void   dt_lock_app( DTWIN *d, CONST T_VOBJ *v );	/* 起動アプリの固定, turned over */

/* dtinfo.c: 管理情報 of the real object a link points at */
IMPORT void   dt_real_info( DTWIN *d, CONST T_VOBJ *v );

/* dtuitest.c: the desktop driven over the second serial port (make UITEST=1) */
IMPORT void   dt_uitest_start( void );
IMPORT BOOL   dt_info_event( CONST T_WMEV *ev );
IMPORT void   dt_info_close_all( void );

/*
 * dtfault.c: a process ended by a processor exception, told in a
 * dialog (プログラムの異常終了). The desktop's loop takes what the kernel
 * recorded (dt_fault_poll) and gives each event to dt_fault_event
 * first; dt_fault_wid and dt_fault_lines say what is shown.
 */
IMPORT void   dt_fault_poll( void );
IMPORT void   dt_fault_forget( void );
IMPORT BOOL   dt_fault_event( CONST T_WMEV *ev );
IMPORT void   dt_fault_close_all( void );
IMPORT INT    dt_fault_wid( void );
IMPORT INT    dt_fault_lines( UB *buf, INT max );
IMPORT DTWIN *dt_first_win( void );
IMPORT BOOL   dt_reachable( CONST T_VOBJ *v );

/* dtcab.c: the cabinet's menu and what its own items do */
IMPORT ER     dt_cab_menu_make( DTWIN *d, ID *p_mid );
IMPORT void   dt_cab_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			      CONST T_VOBJ *v );
IMPORT void   dt_cab_command( DTWIN *d, INT cmd, BOOL on_vobj,
			      CONST T_VOBJ *v );
IMPORT void   dt_close_vobj( CONST T_VOBJ *v );

/* dtform.c: panels that ask and tell, and the helpers panels are built with */
#define DT_PN_IN	8		/* contents stand in this far from the outline */
#define DT_PN_CANCEL	90		/* the part numbers of the two buttons */
#define DT_PN_OK	91
IMPORT void   dt_pn_text( UB *to, INT max, CONST char *s );
IMPORT T_WMPANEL *dt_pn_new( INT w, INT h );
IMPORT T_WMPART *dt_pn_add( T_WMPANEL *p, UW type, INT num, INT l, INT t,
			    INT w, INT h, CONST char *label );
IMPORT void   dt_pn_buttons( T_WMPANEL *p, CONST char *ok );
IMPORT UINT   dt_pn_run( DTWIN *d, T_WMPANEL *def, INT *p_pid );
IMPORT void   dt_pn_done( DTWIN *d, INT pid );
IMPORT INT    dt_pn_value( INT pid, INT num );
IMPORT void   dt_colour_text( UW col, UB *out );
IMPORT BOOL   dt_colour_of( CONST UB *s, UW *p_col );
IMPORT BOOL   dt_ask_name( DTWIN *d, CONST char *prompt, CONST UB *start,
			   UB *out, INT max );
IMPORT BOOL   dt_confirm( DTWIN *d, CONST char *l1, CONST char *l2,
			  CONST char *no, CONST char *yes );
IMPORT void   dt_tell( DTWIN *d, CONST char *l1, CONST char *l2 );
IMPORT BOOL   dt_ask_text( DTWIN *d, CONST char *p1, CONST char *p2,
			   CONST UB *start, UB *out, INT max, CONST char *ok );

/* A panel of labelled boxes, what each holds coming in and going out */
#define DT_FIELD_MAX	32
IMPORT BOOL   dt_fields_form( DTWIN *d, CONST char *title, INT n,
			      CONST char *CONST *labels,
			      CONST char *CONST *hints,
			      UB (*vals)[DT_FIELD_MAX] );

/* One of a list of names chosen: its number, or -1 */
IMPORT INT    dt_list_form( DTWIN *d, CONST char *title,
			    CONST char *CONST *names, INT n, INT start );

/* 用紙設定: sizes in tenths of a millimetre */
typedef struct {
	INT	w, h;
	INT	top, bottom, left, right;
	INT	imposition;		/* 0 片面, 1 見開き */
} T_DTPAPER;
#define DT_PAPER_CANCEL	0
#define DT_PAPER_SET	1
#define DT_PAPER_STD	2		/* 標準設定 */
#define DT_PAPER_HF	3		/* ヘッダ/フッタ, and back to the form after */
IMPORT INT    dt_paper_form( DTWIN *d, T_DTPAPER *pp );

/* 実身/仮身検索: what is asked */
#define DQ_NAME		0		/* 実身名 */
#define DQ_RELATION	1		/* 続柄 */
#define DQ_TEXT		2		/* 本文 */
#define DQ_ALL		3		/* 全て */
#define DQ_DATE_NONE	0
#define DQ_DATE_MADE	1		/* 作成日 */
#define DQ_DATE_UPDATED	2		/* 更新日 */
#define DQ_DATE_ACCESSED 3		/* 参照日 */

typedef struct {
	UB	text[WM_LABEL_MAX];	/* the words, or empty */
	INT	target;			/* DQ_NAME .. DQ_ALL */
	INT	date_kind;		/* DQ_DATE_* */
	UB	from[16], to[16];	/* "YYYY-MM-DD", either may be empty */
} T_DTQUERY;

IMPORT BOOL   dt_search_form( DTWIN *d, T_DTQUERY *q );

/* 検索/置換: what was pressed */
#define DT_FIND_NEXT	10
#define DT_FIND_REPLACE	11
#define DT_FIND_ALL	12
IMPORT INT    dt_find_form( DTWIN *d, UB *what, UB *with, INT max,
			    BOOL *p_regex );

/* dtdoc.c: editing a document's text */
IMPORT BOOL   dd_editable( CONST DTWIN *d );
IMPORT BOOL   dd_key( DTWIN *d, UINT code, UINT mods );
/* 詳細: the marks of settings pressed, and a ruler's marker carried */
IMPORT BOOL   dd_mark_press( DTWIN *d, INT x, INT y, UD when );
IMPORT BOOL   dd_mark_carrying( void );
IMPORT void   dd_mark_follow( INT sx, INT sy );
IMPORT void   dd_mark_release( void );
IMPORT void   dd_press( DTWIN *d, INT x, INT y, BOOL extend );
IMPORT void   dd_type_text( DTWIN *d, CONST UB *s, INT n );

/* The text input port: かな漢字変換 in the text windows (dttip.c) */
IMPORT BOOL   tip_mode_key( DTWIN *d, UINT key, UINT mods );
IMPORT BOOL   tip_japanese( void );
IMPORT BOOL   tip_key( DTWIN *d, UINT key, UINT mods );
IMPORT void   tip_draw( DTWIN *d, INT gid );
IMPORT void   tip_start( void );
IMPORT BOOL   tip_press( DTWIN *d, INT x, INT y );
IMPORT void   tip_commit( void );
IMPORT void   tip_forget( DTWIN *d );
IMPORT BOOL   tip_busy( CONST DTWIN *d );
IMPORT void   dd_follow( INT sx, INT sy );
IMPORT BOOL   dd_dragging( void );
IMPORT void   dd_release( void );
IMPORT void   dd_draw( DTWIN *d, INT gid );
IMPORT void   dd_show_caret( DTWIN *d );
IMPORT BOOL   dd_command( DTWIN *d, INT cmd );
IMPORT void   dd_link_after( DTWIN *d, CONST TS_UUID *orig );
IMPORT ER     dd_link_drop( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v );
IMPORT BOOL   dd_clip_has( void );
IMPORT void   dd_link_take( DTWIN *d, INT which, CONST T_DPRECT *box, INT x, INT y );
IMPORT BOOL   dd_link_carrying( void );
IMPORT void   dd_link_follow( INT sx, INT sy );
IMPORT void   dd_link_release( INT sx, INT sy );
IMPORT void   dd_look( DTWIN *d, INT cmd, BOOL on );
IMPORT void   dd_font( DTWIN *d, CONST char *attr, CONST UB *value );
IMPORT void   dd_pick_all( DTWIN *d );
IMPORT BOOL   dd_has_sel( DTWIN *d );
IMPORT INT    dd_recent_faces( UB (*out)[WM_LABEL_MAX], INT max );
IMPORT BOOL   dd_xml_leave( DTWIN *d );
IMPORT BOOL   dd_xml_commit( DTWIN *d );
IMPORT void   dd_xml_enter( DTWIN *d );
IMPORT void   dd_view( DTWIN *d );

/* dtfig.c: the figure editor */
IMPORT BOOL   df_is( CONST DTWIN *d );
IMPORT BOOL   df_takeable( CONST DTWIN *d, INT i );
IMPORT void   df_shift( DTWIN *d, INT i, INT dx, INT dy );
IMPORT void   df_resize( DTWIN *d, INT i, CONST T_DPRECT *nb );
IMPORT INT    df_picked( CONST DTWIN *d );
IMPORT void   df_delete_picked( DTWIN *d );
IMPORT INT    df_copy_into( DTWIN *d, T_TAD *frag );
IMPORT INT    df_frag_into( T_TAD *rec, T_TADNODE *parent, CONST T_TAD *frag );
IMPORT INT    df_paste_frag( DTWIN *d, CONST T_TAD *frag, INT off );
IMPORT void   df_pick_pasted( DTWIN *d );
IMPORT void   df_front( DTWIN *d, BOOL front );
IMPORT BOOL   df_tool_press( DTWIN *d, INT x, INT y, UD when, UINT mods );
IMPORT BOOL   df_drawing_now( void );
IMPORT void   df_tool_follow( INT sx, INT sy, UINT mods );
IMPORT void   df_tool_release( void );
IMPORT BOOL   df_tool_key( DTWIN *d, UINT code );
IMPORT void   df_panel_show( DTWIN *d );
IMPORT void   df_panel_hide( void );
IMPORT BOOL   df_panel_press( CONST T_WMEV *ev );
IMPORT BOOL   df_panel_carrying( void );
IMPORT void   df_panel_follow( INT sx, INT sy );
IMPORT void   df_panel_let_go( void );
IMPORT INT    df_panel_wid( void );
IMPORT ER     df_menu_make( DTWIN *d, ID *p_mid );
IMPORT void   df_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			  CONST T_VOBJ *v );
IMPORT void   df_whole_groups( DTWIN *d );
IMPORT void   df_attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v );
IMPORT void   df_points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt,
			     INT n );
IMPORT void   df_adopt( DTWIN *d, T_TADNODE *nd );
IMPORT void   df_panel_toggle( DTWIN *d );

/* dtftool.c: the 道具パネル and the drawing tools */
IMPORT void   df_hover( DTWIN *d, INT x, INT y );
IMPORT void   df_grid_draw( DTWIN *d, INT gid, CONST T_DPRECT *page );
IMPORT INT    df_zoom( void );
IMPORT void   df_take_look( DTWIN *d );
IMPORT BOOL   df_picking( void );
IMPORT void   df_overlay( DTWIN *d, INT gid, CONST T_DPRECT *page );
IMPORT void   df_transform( DTWIN *d );
IMPORT BOOL   df_transform_press( DTWIN *d, INT x, INT y );
IMPORT BOOL   df_transform_carrying( void );
IMPORT void   df_transform_end( DTWIN *d );
IMPORT void   df_transform_follow( INT sx, INT sy );
IMPORT void   df_transform_release( void );
IMPORT void   df_paper_toggle( DTWIN *d );
IMPORT void   df_page_setup( DTWIN *d );
IMPORT void   df_realsize_toggle( DTWIN *d );
IMPORT void   df_forget( DTWIN *d );
IMPORT BOOL   df_text_press( DTWIN *d, INT x, INT y, UD when );
IMPORT void   df_text_end( DTWIN *d );
IMPORT void   df_text_begin( DTWIN *d, T_TADNODE *node, INT x, INT y );
IMPORT INT    df_fill_pattern( void );
IMPORT BOOL   df_end_press( DTWIN *d, INT x, INT y );
IMPORT BOOL   df_end_carrying( void );
IMPORT void   df_end_follow( INT sx, INT sy );
IMPORT void   df_end_release( void );
IMPORT void   df_tool_select( void );

/* dtfcanvas.c: the canvas, and the patterns */
IMPORT void   df_canvas_new( DTWIN *d, INT l, INT t, INT r, INT b );
IMPORT BOOL   df_canvas_press( DTWIN *d, INT fx, INT fy );
IMPORT void   df_canvas_leave( void );
IMPORT BOOL   df_canvas_painting( void );
IMPORT void   df_canvas_follow( INT sx, INT sy );
IMPORT void   df_canvas_release( void );
IMPORT BOOL   df_canvas_active( void );
IMPORT void   df_canvas_draw( DTWIN *d, INT gid, CONST T_DPRECT *page );
IMPORT void   df_canvas_forget( DTWIN *d );

/* 画材: what the canvas paints with, from the panel */
#define DF_PAINT_PENCIL	0		/* 鉛筆 */
#define DF_PAINT_BRUSH	1		/* 絵筆 */
#define DF_PAINT_ERASER	2		/* 消しゴム */
#define DF_PAINT_AIR	3		/* エアブラシ */
#define DF_PAINT_POUR	4		/* 絵の具 */
#define DF_PAINT_CUTTER	5		/* カッター */
IMPORT INT    df_paint_tool( void );
IMPORT INT    df_paint_size( void );
IMPORT UW     df_paint_colour( void );
IMPORT void   df_burn( DTWIN *d );
IMPORT void   df_pattern_edit( DTWIN *d, INT pat );

/* 位置あわせ: across (0 none, 1 left, 2 middle, 3 right) and up and down */
IMPORT BOOL   dt_align_form( DTWIN *d, INT *p_h, INT *p_v );

/* dttool.c: the system's own tools */
IMPORT void   dt_trash_open( void );
IMPORT void   dt_search_open( DTWIN *from );
IMPORT void   dt_network_open( DTWIN *from, CONST T_VOBJ *v );
IMPORT ER     dt_tool_menu_make( DTWIN *d, BOOL on_vobj, ID *p_mid );
IMPORT void   dt_tool_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			       CONST T_VOBJ *v );
IMPORT BOOL   dt_tool_key( DTWIN *d, UINT code, UINT mods );
IMPORT void   dt_tool_carry( DTWIN *d, DTWIN *t, CONST T_VOBJ *v );
IMPORT void   dt_tool_refresh( UINT tool );

IMPORT void   df_paste_text( DTWIN *d );

/* dtedit.c: the model, made again from the record */
IMPORT void   ed_model( DTWIN *d );
IMPORT void   ed_forget( DTWIN *d );
IMPORT void   ed_before( DTWIN *d );	/* the record kept, to take back to */
IMPORT void   ed_changed( DTWIN *d );	/* made again, marked, drawn */

/* picking */
IMPORT void   ed_pick_none( DTWIN *d );
IMPORT void   ed_pick_all( DTWIN *d );
IMPORT void   ed_pick_only( DTWIN *d, INT i );
IMPORT void   ed_pick_toggle( DTWIN *d, INT i );
IMPORT BOOL   ed_picked( CONST DTWIN *d, INT i );
IMPORT void   ed_draw_picks( DTWIN *d );
/*
 * The frames of what is taken in the figure editor come and go by
 * turns: ed_blink_turn changes the turn, ed_blink_show makes it the
 * turn they show, and ed_blinks says whether a window has any.
 */
IMPORT void   ed_blink_turn( void );
IMPORT void   ed_blink_show( void );
IMPORT BOOL   ed_blinks( CONST DTWIN *d );

/* what can be done to the picked ones */
IMPORT void   ed_delete( DTWIN *d );
IMPORT void   ed_copy( DTWIN *d );
IMPORT void   ed_cut( DTWIN *d );
IMPORT void   ed_paste( DTWIN *d, BOOL take );
IMPORT void   ed_undo( DTWIN *d );
IMPORT void   ed_redo( DTWIN *d );
IMPORT void   ed_open_picked( DTWIN *d );
IMPORT void   ed_close_picked( DTWIN *d );

/* the order front to back, protection, and a link's attributes */
IMPORT void   ed_front( DTWIN *d );
IMPORT void   ed_back( DTWIN *d );
IMPORT void   ed_protect( DTWIN *d, BOOL background, BOOL on );
IMPORT BOOL   ed_link_at( DTWIN *d, INT which, T_VOBJ *v );
IMPORT void   ed_set_link( DTWIN *d, CONST T_VOBJ *v );
IMPORT INT    ed_first_pick( DTWIN *d );

/* 整頓 */
#define AR_NONE		0
#define AR_LEFT		1		/* 横 */
#define AR_RIGHT	2
#define AR_COMPACT	1		/* 縦 */
#define AR_ALIGN	2
#define AR_SINGLE	1		/* 段組 */
#define AR_MULTI_H	2
#define AR_MULTI_V	3
#define AR_W_FIRST	1		/* 幅調整 */
#define AR_W_FULL	2
#define AR_W_NAME	3
#define AR_W_RELATION	4
#define AR_W_NODATE	5
#define AR_BY_NAME	1		/* 整列順 */
#define AR_BY_MADE	2
#define AR_BY_UPDATED	3
#define AR_BY_SIZE	4

typedef struct {
	INT	horizontal, vertical, column, columns, width, sort_by;
	BOOL	descending;
} T_ARRANGE;

IMPORT void   ed_arrange( DTWIN *d, CONST T_ARRANGE *o );

/* a link put into a window's record where the carried outline was let go */
IMPORT void   ed_drop_link( DTWIN *d, DTWIN *t, CONST T_VOBJ *v );
IMPORT BOOL   ed_place_link( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v );

/* carrying, sizing, and drawing a band round what to pick */
IMPORT BOOL   ed_press( DTWIN *d, INT x, INT y, BOOL add, UD when );
IMPORT BOOL   ed_holding( void );
/*
 * 複写ドラッグ: the second button pressed while what is taken is carried
 * makes the carrying a copy -- what was taken stays where it was, and a
 * copy goes where the hand lets go.
 */
IMPORT void   ed_hold_copy( void );
IMPORT void   dd_link_copy( void );
IMPORT void   df_dup_picked( DTWIN *d );
IMPORT void   ed_follow( INT sx, INT sy );		/* on the screen */
IMPORT void   ed_release( INT sx, INT sy );

/* How far apart the presses of a double press may be, each way (ユーザ環境設定 位置許容度) */
#define DT_DBL_NEAR	( wm_num(LK_DBL_W, 10) / 2 )

IMPORT BOOL dd_caret_hidden;
IMPORT BOOL dt_confirm_timed( DTWIN *d, CONST char *CONST *lines, INT nline,
			      CONST char *no, CONST char *yes, INT ms );		/* the caret's turn to be hidden (dtdoc.c) */

#endif /* __DESKTOP_H__ */
