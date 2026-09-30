/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pdraw.h
 *	The parts of panels drawn, and found under the pointer (design 16.5.8)
 *
 *	What part.c asks of pdraw.c: one part drawn in a window's drawing
 *	environment, with what the panel knows about it at the moment, and
 *	where the pieces of a part are, so that a press lands on the piece
 *	that was drawn there.
 */

#ifndef __PDRAW_H__
#define __PDRAW_H__

#include <ts/part.h>

#define PD_FH		16		/* the letters of parts: sixteen high, sixteen wide */
#define PD_GAP		1		/* between the letters in a box: a sixteenth of one */
#define PD_SS_BAR	20		/* the room a scrolling selector keeps for its bar */
#define PD_TAG_MAX	32		/* tags in one panel of them */

/* A conversion under way in a box, as the box shows it */
#define PD_CONV_TEXT	256
#define PD_CONV_CL	32
#define PD_CAND_MAX	1024
#define PD_CAND_ROWS	9

typedef struct {
	UB		text[PD_CONV_TEXT];	/* the letters being converted */
	INT		cl[PD_CONV_CL + 1];	/* where each clause starts in them, and the end */
	INT		ncl;
	INT		clause;		/* the one being converted, -1 while it is still read */
	BOOL		list;		/* candidates listed under the box */
	UB		cand[PD_CAND_MAX];	/* those listed, each ending in a nought */
	INT		ncand;
	INT		chosen;		/* which of them (0 up), -1 none */
	T_DPRECT	box;		/* where the list stood when drawn */
} T_PDCONV;

/* What the panel knows about a part as it is drawn */
typedef struct {
	INT		gid;
	CONST UB	*pool;		/* the panel's names */
	BOOL		focus;		/* the part has the keys */
	INT		held;		/* held down: 1; a field's half: -1 its up, -2 its down; 0 not */
	INT		held_at;	/* a selector's name or a tag held (1 up), 0 none */
	BOOL		caret;		/* the caret in the lit half of its blink */
	BOOL		first;		/* a choice of a group: the first, which draws the group's box */
	T_DPRECT	group;		/* the rectangle round the group's choices */
	T_PDCONV	*conv;		/* a conversion under way in this box, or NULL */
} T_PDCTX;

IMPORT void pd_part( T_WMPART *pt, CONST T_DPRECT *r, T_PDCTX *c );

/* The name numbered i (0 up) of those starting at 'at' in a pool, or NULL */
IMPORT CONST UB *pd_pool_name( CONST UB *pool, INT at, INT i );

/* A selector of one part: the name (0 up) whose cell holds the point, or -1 */
IMPORT INT pd_ws_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y );

/*
 * A scrolling selector: how many rows it shows, and where a point is --
 * the row's name (1 up), or on the bar: -1 above the knob, -2 on it,
 * -3 below it; 0 nowhere.
 */
IMPORT INT pd_ss_rows( CONST T_WMPART *pt, CONST T_DPRECT *r );
IMPORT INT pd_ss_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y );
IMPORT INT pd_ss_top_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT y );	/* the knob dragged to y */

/*
 * Fields along a line: the field whose place holds x (0 up), or -1; in
 * *p_half, when the point is on the marks under the field being worked,
 * -2 for the one that steps down and -1 for the one that steps up.
 */
IMPORT INT pd_sb_at( T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y, INT *p_half );

/* A volume: the value at a point */
IMPORT INT pd_vl_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x, INT y );

/* A box: the place between its letters nearest x, in bytes */
IMPORT INT pd_tb_at( CONST T_WMPART *pt, CONST T_DPRECT *r, INT x );

/*
 * A panel of tags: the tag under a point, 0 none, and its rectangle; a
 * tag chosen as the panel's own presses choose one (the one it stands for
 * when it is an upper tag with a lower row). pd_tag_show answers the tag
 * now shown, or 0 when it was already.
 */
IMPORT INT pd_tag_at( T_WMPART *pt, CONST UB *pool, INT x, INT y, T_DPRECT *p_tab );
IMPORT INT pd_tag_show( T_WMPART *pt, CONST UB *pool, INT num );
IMPORT void pd_tag_turn( INT gid, CONST T_DPRECT *tab );	/* a tag held: turned over */

/* The candidates of a conversion, listed under the box they belong to */
IMPORT void pd_cands( INT gid, CONST T_DPRECT *r, T_PDCONV *cv, INT h );
IMPORT INT  pd_cand_at( CONST T_PDCONV *cv, INT x, INT y );

#endif /* __PDRAW_H__ */
