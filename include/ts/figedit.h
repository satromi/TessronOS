/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figedit.h
 *	What the figure editor does to a figure, apart from the window it
 *	is shown in (application/fig/figedit.c, design 17.13)
 *
 *	The shapes of a figure are worked on by changing their elements in
 *	the record: carried, sized, thrown away, copied, put in front or
 *	behind, turned over and round, grouped, lined up, their colours
 *	turned the other way. The same code serves the figure editor of the
 *	desktop and a figure editor running as a process. What it works on
 *	is in T_FGED: the record, the model made from it, and which shapes
 *	are picked. What belongs to whoever shows the figure -- taking back
 *	(before), making the model again and drawing it (changed, model),
 *	what is picked, a panel that says memory ran out or asks how to line
 *	up -- is asked through the table T_FGHOST, each call of which leaves
 *	rec, fig, pick and npick as they are after it.
 */

#ifndef __TS_FIGEDIT_H__
#define __TS_FIGEDIT_H__

#include <ts/tad.h>
#include <ts/tadview.h>

#ifdef __cplusplus
extern "C" {
#endif

struct t_fged;

typedef struct {
	void	(*before)( struct t_fged *e );		/* what it is now kept, to be taken back to */
	void	(*changed)( struct t_fged *e );		/* the record changed: the model made again, drawn */
	void	(*model)( struct t_fged *e );		/* the model made again from the record, not drawn */
	void	(*pick_none)( struct t_fged *e );
	void	(*pick_add)( struct t_fged *e, INT i );
	INT	(*first_pick)( struct t_fged *e );	/* the shape picked first, -1 none */
	void	(*no_memory)( struct t_fged *e );
	/* how to line up: across 0 not, 1 left, 2 middle, 3 right; down likewise. FALSE: put away */
	BOOL	(*align_form)( struct t_fged *e, INT *p_h, INT *p_v );
} T_FGHOST;

typedef struct t_fged {
	T_TAD		*rec;
	T_TVFIG		*fig;		/* made from rec */
	UINT		*pick;		/* for each shape: 0, or the order it was picked in */
	INT		npick;
	BOOL		usable;		/* a figure the editor works on, not one sealed */
	CONST T_FGHOST	*host;
	void		*arg;		/* the host's */
} T_FGED;

/* How shapes are turned (fe_turn) and put in order (fe_order) */
#define FE_FLIP_H	1
#define FE_FLIP_V	2
#define FE_ROT_L	3
#define FE_ROT_R	4

#define FE_TO_FRONT	1
#define FE_TO_BACK	2
#define FE_TO_FORWARD	3
#define FE_TO_BACKWARD	4

IMPORT void fe_attr_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT v );
IMPORT void fe_points_put( T_TAD *rec, T_TADNODE *nd, CONST T_DPPOINT *pt, INT n );
IMPORT BOOL fe_is( CONST T_FGED *e );
IMPORT BOOL fe_takeable( CONST T_FGED *e, INT i );
IMPORT void fe_whole_groups( T_FGED *e );
IMPORT void fe_group( T_FGED *e );
IMPORT void fe_ungroup( T_FGED *e );
IMPORT void fe_shift( T_FGED *e, INT i, INT dx, INT dy );
IMPORT void fe_resize( T_FGED *e, INT i, CONST T_DPRECT *nb );
/* Sized past the edge that stays: the box from the other side, and the shape turned over that way */
#define FE_SIZE_FLIP_H	0x0001
#define FE_SIZE_FLIP_V	0x0002
IMPORT void fe_resize_flip( T_FGED *e, INT i, CONST T_DPRECT *nb, UINT flip );
IMPORT void fe_size_box( CONST T_DPRECT *was, INT cx, INT cy, BOOL across, BOOL down,
			 INT least_w, INT least_h, T_DPRECT *nb, UINT *p_flip );
IMPORT INT  fe_picked( CONST T_FGED *e );
IMPORT void fe_delete_picked( T_FGED *e );
IMPORT void fe_turn( T_FGED *e, INT how );
IMPORT void fe_order( T_FGED *e, INT how );
IMPORT INT  fe_copy_into( T_FGED *e, T_TAD *frag );
IMPORT void fe_dup_picked( T_FGED *e );
IMPORT INT  fe_frag_into( T_TAD *rec, T_TADNODE *parent, CONST T_TAD *frag );
IMPORT INT  fe_paste_frag( T_FGED *e, CONST T_TAD *frag, INT off );
IMPORT void fe_adopt( T_FGED *e, T_TADNODE *nd );
IMPORT void fe_pick_pasted( T_FGED *e );
IMPORT void fe_align( T_FGED *e );

/*
 * 保護 of the links picked: what each item can change, from the links'
 * own attributes in the record (fixed, background). 固定化 when one
 * picked is not fixed, 固定解除 when one is, and the same for 背景化;
 * nothing when no link is picked. fe_prot_grey sets the four items of a
 * menu (fix, unfix, bg, unbg) to it.
 */
#define FE_PROT_FIX	0x0001
#define FE_PROT_UNFIX	0x0002
#define FE_PROT_BG	0x0004
#define FE_PROT_UNBG	0x0008
IMPORT UINT fe_prot_can( CONST T_TAD *rec, CONST T_TVFIG *fig, CONST UINT *pick );
IMPORT void fe_prot_grey( ID mid, UINT can );
IMPORT void fe_invert( T_FGED *e );

/*
 * 整頓 of the links of a 仮身一覧: the boxes of what is picked, in the
 * order they are to go, laid out by fe_arrange_lay. 'shown' is how tall
 * each is drawn (fe_shown_h: a closed link's band, which may be less
 * than its box), and the room between two is taken from its foot.
 * 'id' is the caller's own, carried with the box. top, left and right
 * are where the layout starts and the edge 右揃え goes to.
 */
#define FE_AR_NONE	0
#define FE_AR_LEFT	1		/* 横 */
#define FE_AR_RIGHT	2
#define FE_AR_COMPACT	1		/* 縦 */
#define FE_AR_ALIGN	2
#define FE_AR_SINGLE	1		/* 段組 */
#define FE_AR_MULTI_H	2
#define FE_AR_MULTI_V	3
#define FE_AR_GAP	5		/* 詰める: the room left between two */
#define FE_AR_ROOM	10		/* between columns, and rows with neither */
#define FE_AR_COLS_MAX	64

typedef struct {
	T_DPRECT	r;
	INT		shown;
	INT		id;
} T_FEARBOX;

typedef struct {
	INT	horizontal, vertical, column, columns;
	INT	top, left, right;
} T_FEARLAY;

IMPORT INT  fe_shown_h( CONST T_VOBJ *v );
IMPORT INT  fe_arrange_gap( CONST T_FEARBOX *b, INT n, INT vertical );
IMPORT void fe_arrange_lay( T_FEARBOX *b, INT n, CONST T_FEARLAY *o );

/*
 * 整頓 as a whole, from the links picked: it[] holds each with the order
 * it was picked in. Those 固定化 or 背景化 stay where they are and are
 * dropped from it[]; the rest are taken in the order they were picked,
 * given the width 幅調整 asks for (the first one's, or what each has to
 * show), put in 整列順 when one is asked for, and laid out by
 * fe_arrange_lay from the top, left and right of them all. What is
 * known of the object a link points at is asked of the caller's info,
 * once a link and only when the width or the order needs it. Answers
 * how many links, at the head of it[], were laid out, their boxes and
 * heights changed, for the caller to write back into its record.
 */
#define FE_AR_W_FIRST	1		/* 幅調整 */
#define FE_AR_W_FULL	2
#define FE_AR_W_NAME	3
#define FE_AR_W_RELATION 4
#define FE_AR_W_NODATE	5
#define FE_AR_BY_NAME	1		/* 整列順 */
#define FE_AR_BY_MADE	2
#define FE_AR_BY_UPDATED 3
#define FE_AR_BY_SIZE	4
#define FE_AR_INFO_TEXT	40		/* bytes of a date as the metadata writes it */
#define FE_AR_APP_NAME	64		/* bytes of a program's shown name */

typedef struct {
	T_VOBJ	v;
	UINT	seq;
} T_FEARITEM;

typedef struct {
	UB	name[TAD_NAME_MAX];		/* the metadata's name, for 名前 */
	UB	label[TAD_NAME_MAX];		/* the name the link shows, for 幅調整 */
	UB	made[FE_AR_INFO_TEXT];		/* makeDate */
	UB	updated[FE_AR_INFO_TEXT];	/* updateDate */
	INT	bytes;				/* of record 0 */
	UB	rel[TAD_NAME_MAX];		/* the relationship tags, "[tag] [tag]" */
	UB	app[FE_AR_APP_NAME];		/* the first program the applist names */
	BOOL	has_label, has_app;
} T_FEARINFO;

typedef struct {
	INT	horizontal, vertical, column, columns;	/* FE_AR_*, as T_FEARLAY */
	INT	width;				/* FE_AR_W_*, FE_AR_NONE */
	INT	sort_by;			/* FE_AR_BY_*, FE_AR_NONE */
	BOOL	descending;
} T_FEARRANGE;

/* What is known of object id; the caller gives inf cleared. FALSE: nothing */
typedef BOOL (*FE_ARINFO)( void *arg, CONST TS_UUID *id, T_FEARINFO *inf );

IMPORT INT  fe_arrange( T_FEARITEM *it, INT n, CONST T_FEARRANGE *o, FE_ARINFO info, void *arg );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FIGEDIT_H__ */
