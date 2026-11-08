/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docedit.h
 *	Editing the text of a document, apart from the window it is shown
 *	in (application/doc/docedit.c, design 17.12)
 *
 *	The same code edits a document of the desktop, a piece of text in
 *	a figure of the desktop, and a piece of text in a figure edited by
 *	a process. What it works on is a T_DDED: the caret and the other
 *	end of the selection, which are its own, and where the one showing
 *	the text keeps the record, the model made from it and the way it is
 *	shown, which the editor reads and changes in place. What belongs to
 *	whoever shows the text -- making the model again, drawing, taking
 *	back, the panels, the tray, the converter of かな漢字, what is under
 *	the hand when something carried out of the text is let go -- is
 *	asked through the table T_DDHOST.
 */

#ifndef __TS_DOCEDIT_H__
#define __TS_DOCEDIT_H__

#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/part.h>

#ifdef __cplusplus
extern "C" {
#endif

struct t_dded;

/* A panel of labelled boxes, what each holds coming in and going out */
#define DE_FIELD_MAX	32

/* 用紙設定: sizes in tenths of a millimetre */
typedef struct {
	INT	w, h;
	INT	top, bottom, left, right;
	INT	imposition;		/* 0 片面, 1 見開き */
} T_DEPAPER;
#define DE_PAPER_CANCEL	0
#define DE_PAPER_SET	1
#define DE_PAPER_STD	2		/* 標準設定 */
#define DE_PAPER_HF	3		/* ヘッダ/フッタ, and back to the form after */
#define DE_PAPER_W	595		/* A4, when a document names no paper */
#define DE_PAPER_H	842

/* 検索/置換: what was pressed */
#define DE_FIND_NEXT	10
#define DE_FIND_REPLACE	11
#define DE_FIND_ALL	12

/* What is under the hand where something carried out of a text is let go */
#define DE_AT_NONE	0		/* nothing that takes it */
#define DE_AT_TEXT	1		/* a text, *p_t: possibly the one it came from */
#define DE_AT_OTHER	2		/* something else, which the host gives it to */

/* A thing standing in a text, carried out of it: what is to be put elsewhere */
typedef struct {
	UINT		kind;		/* TV_RUN_LINK, TV_RUN_IMAGE, TV_RUN_FIGURE */
	T_VOBJ		v;		/* a link's */
	T_TADNODE	*node;		/* the element it is in the text's record */
	T_DPRECT	box;		/* its box, in the work area where it was taken */
	INT		x0, y0;		/* where the hand took it, likewise */
	BOOL		copy;		/* a copy is put; it stays where it was */
} T_DDTHING;

typedef struct {
	/* the model, the drawing and the window */
	void	(*model)( struct t_dded *e );		/* the model made again from the record */
	void	(*changed)( struct t_dded *e );		/* the text changed: what it stands in made again too */
	void	(*draw)( struct t_dded *e );		/* the page drawn again */
	void	(*draw_all)( struct t_dded *e );	/* every window drawn again */
	void	(*update)( struct t_dded *e );		/* what was drawn put on the screen */
	void	(*shown)( struct t_dded *e );		/* the whole work area put on the screen */
	/*
	 * Only a part of the work area drawn again, the rest left as it is
	 * (tv_doc_band), and that part put on the screen. NULL: the page is
	 * drawn whole.
	 */
	void	(*draw_band)( struct t_dded *e, CONST T_DPRECT *band );
	void	(*work_rect)( struct t_dded *e, T_DPRECT *page );	/* where the text is laid, in the work area */
	BOOL	(*origin)( struct t_dded *e, INT *p_x, INT *p_y );	/* the work area on the screen; FALSE: none */
	BOOL	(*caret_on)( struct t_dded *e );	/* typed into, and the caret's blink shown */
	INT	(*look)( struct t_dded *e, UINT num, INT dflt );	/* a number of the look (wm_num) */
	/* the outline carried, on the screen; NULL: no more. first: the first, dx, dy from where it was */
	void	(*drag)( struct t_dded *e, CONST T_DPRECT *r, BOOL first, INT dx, INT dy );
	void	(*note)( struct t_dded *e, CONST char *s );	/* said for the tests */

	/* taking back */
	void	(*before)( struct t_dded *e );		/* what it is now kept, to be taken back to */
	void	(*forget)( struct t_dded *e );		/* the record changed under it: what was laid out is not kept */
	void	(*undo)( struct t_dded *e );

	/* panels */
	void	(*tell)( struct t_dded *e, CONST char *l1, CONST char *l2 );
	void	(*no_memory)( struct t_dded *e );
	BOOL	(*ask_text)( struct t_dded *e, CONST char *p1, CONST char *p2,
			     CONST UB *start, UB *out, INT max, CONST char *ok );
	BOOL	(*ask_name)( struct t_dded *e, CONST char *prompt, CONST UB *start,
			     UB *out, INT max );
	BOOL	(*fields_form)( struct t_dded *e, CONST char *title, INT n,
				CONST char *CONST *labels, CONST char *CONST *hints,
				UB (*vals)[DE_FIELD_MAX] );
	INT	(*list_form)( struct t_dded *e, CONST char *title,
			      CONST char *CONST *names, INT n, INT start );
	INT	(*paper_form)( struct t_dded *e, T_DEPAPER *pp );
	INT	(*find_form)( struct t_dded *e, UB *what, UB *with, INT max, BOOL *p_regex );
	BOOL	(*pick_colour)( struct t_dded *e, UW *io );
	INT	(*faces)( struct t_dded *e, UB (*names)[64], INT max );	/* the family names there are */

	/* the converter of かな漢字 */
	BOOL	(*ime_key)( struct t_dded *e, UINT code, UINT mods );	/* TRUE: the converter's */
	void	(*ime_draw)( struct t_dded *e, INT gid );
	BOOL	(*ime_on)( struct t_dded *e );		/* かな input on */

	/* the tray */
	ER	(*tray_put)( struct t_dded *e, CONST T_TAD *frag, CONST char *name );
	T_TAD	*(*tray_frag)( struct t_dded *e );	/* the set in hand, freed by the caller; NULL none */
	void	(*tray_pictures)( struct t_dded *e, T_TAD *frag );	/* the set's pictures beside the record */
	void	(*tray_taken)( struct t_dded *e );	/* the set in hand taken out of the tray */

	/* carried out of the text and let go */
	UINT	(*at)( struct t_dded *e, INT sx, INT sy, struct t_dded **p_t );
	void	(*focus)( struct t_dded *e, struct t_dded *t );	/* keys go to t, where something was let go */
	/*
	 * letters, let go with the hand at sx, sy and the outline's corner at
	 * cx, cy: >0 taken there, to leave here; 0 taken as a copy; <E_OK not
	 * taken. s may be changed.
	 */
	ER	(*text_away)( struct t_dded *e, INT sx, INT sy, INT cx, INT cy, UB *s, INT n );
	/* a thing: TRUE taken there (the text's record changed by it, if it was not a copy) */
	BOOL	(*thing_away)( struct t_dded *e, INT sx, INT sy, CONST T_DDTHING *th );

	/* objects */
	ER	(*new_object)( struct t_dded *e, CONST UB *name, TS_UUID *p_id, T_TAD **p_rec );
	void	(*object_done)( struct t_dded *e, CONST TS_UUID *id, CONST UB *name );
	T_TAD	*(*restore)( struct t_dded *e, CONST UB *xml, SZ n );	/* 原稿 read back as the record */
	void	(*word_register)( struct t_dded *e, CONST UB *w, INT n );
} T_DDHOST;

typedef struct t_dded {
	/* where the host keeps what the editor reads and changes in place */
	T_TAD		**rec;		/* the record the text is in */
	T_TVDOC		**doc;		/* the model made from it */
	T_TAD		**xrec;		/* 原稿: the record as text, a paragraph for each line */
	INT		*scroll_x, *scroll_y;
	INT		*height;	/* what the content came to */
	UINT		*kind;		/* TV_KIND_* of the record */
	BOOL		*xml_view;	/* 原稿 */
	BOOL		*detail_view;	/* 詳細 */
	BOOL		*dirty;		/* changed since it was saved */
	BOOL		*sealed;	/* nothing in it changed */
	BOOL		*used;		/* still there: FALSE once its window is gone */
	BOOL		*settling;	/* paragraphs still laid only as a guess */
	TS_UUID		*id;		/* the object */
	INT		*recno;

	/* a piece of text in a figure: it does not wind, and a change to it is the figure's */
	BOOL		framed;

	/*
	 * The editor's own: the caret and the other end of the selection,
	 * each a paragraph and a count into it, the same place when nothing
	 * is selected; letters being typed one after another, taken back as
	 * one; 原稿 changed since it was entered.
	 */
	INT		cpara, cpos;
	INT		apara, apos;
	BOOL		typing;
	BOOL		xml_edited;

	/*
	 * Where the caret, the selection and what is being converted were
	 * last drawn (de_draw): the lines they were on, across the page, in
	 * the work area. A change drawn again in part draws these again too.
	 */
	T_DPRECT	ink;
	BOOL		inked;

	CONST T_DDHOST	*host;
	void		*arg;		/* the host's */
} T_DDED;

IMPORT BOOL de_editable( CONST T_DDED *e );
IMPORT BOOL de_key( T_DDED *e, UINT code, UINT mods );
IMPORT void de_type_text( T_DDED *e, CONST UB *s, INT n );
IMPORT void de_press( T_DDED *e, INT x, INT y, BOOL extend, UD when );
IMPORT void de_follow( INT sx, INT sy );
IMPORT BOOL de_dragging( void );
IMPORT void de_release( void );
IMPORT BOOL de_mark_press( T_DDED *e, INT x, INT y, UD when );
IMPORT BOOL de_mark_carrying( void );
IMPORT void de_mark_follow( INT sx, INT sy );
IMPORT void de_mark_release( void );
IMPORT BOOL de_sel_carrying( void );
IMPORT void de_sel_copy( void );
IMPORT void de_sel_release( INT sx, INT sy );
IMPORT void de_sel_back( T_DDED *d, INT sx, INT sy, BOOL copy );
IMPORT T_TAD *de_sel_frag( T_DDED *d );
IMPORT ER   de_text_drop( T_DDED *t, INT sx, INT sy, CONST UB *s, INT n );
IMPORT ER   de_frag_drop( T_DDED *t, INT sx, INT sy, CONST T_TAD *frag );
IMPORT void de_link_take( T_DDED *e, INT which, CONST T_DPRECT *box, INT x, INT y, UD when );
IMPORT BOOL de_link_carrying( void );
IMPORT void de_link_follow( INT sx, INT sy );
IMPORT void de_link_copy( void );
IMPORT void de_link_release( INT sx, INT sy );
IMPORT void de_thing_back( T_DDED *d, INT sx, INT sy, CONST T_DDTHING *th );
IMPORT ER   de_link_drop( T_DDED *t, INT sx, INT sy, CONST T_VOBJ *v );
IMPORT void de_link_after( T_DDED *e, CONST TS_UUID *orig );
IMPORT BOOL de_link_picked( T_DDED *e, T_VOBJ *v );	/* the link picked alone, in *v */
IMPORT void de_draw( T_DDED *e, INT gid );
IMPORT void de_show_caret( T_DDED *e );
IMPORT BOOL de_caret_rect( T_DDED *e, T_DPRECT *r );
IMPORT INT  de_caret_px( T_DDED *e );
IMPORT BOOL de_command( T_DDED *e, INT cmd );
IMPORT void de_menu_state( T_DDED *e, BOOL *p_comb, BOOL *p_turned );
IMPORT void de_look( T_DDED *e, INT cmd, BOOL on );
IMPORT void de_font( T_DDED *e, CONST char *attr, CONST UB *value );
IMPORT void de_pick_all( T_DDED *e );
IMPORT BOOL de_has_sel( T_DDED *e );
IMPORT INT  de_recent_faces( UB (*out)[WM_LABEL_MAX], INT max );
IMPORT void de_view( T_DDED *e );
IMPORT void de_xml_enter( T_DDED *e );
IMPORT BOOL de_xml_commit( T_DDED *e );
IMPORT BOOL de_xml_leave( T_DDED *e );
IMPORT INT  de_settle( T_DDED *e, INT ms, INT *p_shift, INT *p_laid );	/* laid more: how many are left */
IMPORT T_TAD *de_frag_new( CONST char *body );
IMPORT INT  de_frag_shapes( CONST T_TAD *frag );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DOCEDIT_H__ */
