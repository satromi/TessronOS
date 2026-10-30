/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cal_tad.h
 *	予定表: the records it reads and writes (design 11.10)
 *
 *	A 予定表 object is a figure whose links are the 休日 object (a
 *	text of "YYYY/MM/DD<tab>name" lines), the 年月日実身 template (a
 *	text whose "YYYY/MM/DD" is replaced by the day) and one object for
 *	each month, named "YYYY/MM". A month object is a figure of the month's
 *	grid -- 800 by 600, a row of the week's days, six rows of seven
 *	cells, the holidays written in their cells -- and of links: one to
 *	the object of each day that has one, named "YYYY/MM/DD" and placed
 *	in its cell, and any other link where it was put. The grid is made
 *	again from the month every time the record is written, and the links
 *	are written back with every attribute they came with, only their
 *	box changed.
 *
 *	Here too are a link's attributes changed and read as text, the
 *	figure the links taken go to the tray as, and the applist of a
 *	link: which program it opens with, and whether it keeps to it.
 *
 *	Nothing here makes a system call or takes memory: what is read is
 *	the caller's bytes, what is written goes into the caller's buffer.
 *	The program (cal_main.c) and the kernel's test both use it.
 */

#ifndef __CAL_TAD_H__
#define __CAL_TAD_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>

/* The month's grid as the record has it */
#define CT_BASE_W	800
#define CT_BASE_H	600
#define CT_NAV_H	40		/* the band with the month's name */
#define CT_GRID_TOP	70		/* under the row of the week's days */
#define CT_CELL_W	114		/* 800 / 7 */
#define CT_CELL_H	88		/* 530 / 6 */
#define CT_DAYNO_H	18		/* the day's number at the top of a cell */
#define CT_EV_GAP	2		/* round and between the links of a day */
#define CT_EV_H		18		/* a day's link */
#define CT_ROWS		6
#define CT_COLS		7

#define CT_LINK_H	25		/* a link the program puts that is not a day's */

/* Colours the grid is drawn in, as the records write them */
#define CT_COL_SUNDAY	"#ee0000"
#define CT_COL_SATURDAY	"#0000cc"
#define CT_COL_TEXT	"#000000"
#define CT_COL_GREY	"#808080"
#define CT_COL_DAYBG	"#e8f4ff"	/* a day's link: its ground */
#define CT_COL_DAYFR	"#b0c0d0"	/* and its frame */

/* ---------------------------------------------------------------- text out */

typedef struct {
	UB	*p;
	INT	n, max;
	BOOL	fail;			/* it did not all fit */
	/*
	 * Made larger as it fills (ct_buf_grow): 'more' gives the text the
	 * room asked for, as realloc does, or NULL; NULL here keeps to 'max'.
	 */
	void	*(*more)( void *p, SZ size );
} CTBUF;

IMPORT void ct_buf( CTBUF *b, UB *mem, INT max );
/*
 * A text that grows as long as it is written, through 'more' (realloc,
 * or one like it), from room for 'first' bytes; fail when that cannot
 * be had. The text is the caller's to let go of then (b->p).
 */
IMPORT void ct_buf_grow( CTBUF *b, INT first, void *(*more)( void *p, SZ size ) );
IMPORT void ct_put( CTBUF *b, CONST char *s );
IMPORT void ct_putn( CTBUF *b, CONST UB *s, INT n );
IMPORT void ct_num( CTBUF *b, INT v );
IMPORT void ct_esc( CTBUF *b, CONST UB *s, INT n );	/* XML-escaped */

/* ---------------------------------------------------------------- dates */

IMPORT INT  ct_days( INT y, INT m );			/* days in the month */
IMPORT INT  ct_wday( INT y, INT m, INT d );		/* 0 Sunday .. 6 Saturday */

/* "YYYY/MM/DD" and "YYYY/MM", exactly */
IMPORT BOOL ct_is_day( CONST UB *s, INT *p_y, INT *p_m, INT *p_d );
IMPORT BOOL ct_is_month( CONST UB *s, INT *p_y, INT *p_m );
IMPORT void ct_day_name( UB *out, INT y, INT m, INT d );	/* 11 bytes */
IMPORT void ct_month_name( UB *out, INT y, INT m );		/* 8 bytes */

/* The cell of a day in its month's grid, and the box of its n-th link there */
IMPORT void ct_cell( INT y, INT m, INT d, INT *p_row, INT *p_col );
IMPORT void ct_day_box( INT y, INT m, INT d, INT n, INT *box );

/* ---------------------------------------------------------------- reading */

/* One <link> element of a record: where its bytes are */
typedef struct {
	INT	at, len;
} CTEL;

/* The <link> elements of a record, in order; how many */
/* The <link> elements of a record, max at most; with out NULL how many there are */
IMPORT INT  ct_links( CONST UB *x, INT n, CTEL *out, INT max );

/* An attribute of an element, its bytes as written: its length, -1 when it has none */
IMPORT INT  ct_attr( CONST UB *el, INT n, CONST char *name, UB *out, INT max );
IMPORT INT  ct_attr_int( CONST UB *el, INT n, CONST char *name, INT dflt );

/* The object a link points at, from its id "uuid_0.xtad" */
IMPORT BOOL ct_target( CONST UB *el, INT n, TS_UUID *p_u );

IMPORT void ct_uuid_str( CONST TS_UUID *u, char *out );		/* 37 bytes */
IMPORT BOOL ct_uuid_parse( CONST UB *s, TS_UUID *u );

/* ---------------------------------------------------------------- holidays */

#define CT_HOL_NAME	64

typedef struct {
	INT	y, m, d;
	UB	name[CT_HOL_NAME];
} CTHOL;

/* The lines "YYYY/MM/DD<tab>name" of the 休日 object's text */
IMPORT INT  ct_hol_text( CONST UB *x, INT n, CTHOL *out, INT max );

/* The holidays a month record names in its cells (<docmemo text="holiday"/>) */
IMPORT INT  ct_hol_month( CONST UB *x, INT n, INT y, INT m, CTHOL *out, INT max );

IMPORT CONST UB *ct_hol_find( CONST CTHOL *h, INT nh, INT y, INT m, INT d );

/* ---------------------------------------------------------------- writing */

/* A link to be written: its element as read (or made), and its object's name */
typedef struct {
	CONST UB	*el;
	INT		len;
	CONST UB	*name;
} CTLNK;

/*
 * A month record: the grid of y/m, today's cell coloured when today is
 * a day of it (0 when not), the holidays named in their cells, and the
 * links, a day's in its cell under the others of that day, the rest
 * where they are.
 */
IMPORT ER   ct_month_xml( CTBUF *b, INT y, INT m, INT today, CONST CTHOL *h, INT nh,
			  CONST CTLNK *l, INT nl );

/*
 * An element written again with its box (left, top, right, bottom) and
 * zIndex (-1 to leave it), every other attribute as it was. With
 * 'day' the attributes a day's link must have are added when it has
 * none: its size of letters and the calendar's colours.
 */
IMPORT void ct_link_put( CTBUF *b, CONST UB *el, INT len, CONST INT *box, INT z, BOOL day );

/* A new link's element: to target, as vobjid, in box; a day's when 'day' */
IMPORT void ct_link_new( CTBUF *b, CONST TS_UUID *target, CONST TS_UUID *vobjid,
			 CONST INT *box, BOOL day );

/* A record with an element put before its </figure>, or its </document> */
IMPORT ER   ct_insert( CTBUF *b, CONST UB *x, INT n, CONST UB *el, INT len );

/* A day's text from the template: "YYYY/MM/DD" made the day, and its filename */
IMPORT void ct_apply_date( CTBUF *b, CONST UB *tpl, INT n, CONST UB *day );

/* The day's text when there is no template */
IMPORT void ct_day_doc( CTBUF *b, CONST UB *day );

/* ---------------------------------------------------------------- changing a link */

/* An attribute to set: its value as text, written escaped; NULL takes it off */
typedef struct {
	CONST char	*name;
	CONST UB	*value;
} CTSET;

/*
 * An element written again with the attributes of 'set' given the
 * values there -- in their places when it has them, after the others
 * when not -- and every other attribute as it was.
 */
IMPORT void ct_link_edit( CTBUF *b, CONST UB *el, INT len, CONST CTSET *set, INT n );

/* An attribute's value as text: the XML's escapes undone; its length, -1 when it has none */
IMPORT INT  ct_attr_text( CONST UB *el, INT n, CONST char *name, UB *out, INT max );

/* "true" or "false"; dflt when it has none */
IMPORT BOOL ct_attr_bool( CONST UB *el, INT n, CONST char *name, BOOL dflt );

/*
 * A link's box when the box it is given makes it open or closed: the
 * band of a closed link is its letters and eleven pixels, and a box no
 * more than eight taller than that is closed, made the band's height.
 */
IMPORT INT  ct_band_h( INT chsz );
IMPORT INT  ct_snap_h( INT chsz, INT h );

/* A box moved, as little as it must be, to be whole inside the month under the row of the week's days */
IMPORT void ct_fit_box( INT *box );

/* ---------------------------------------------------------------- the tray */

/* The links as a figure for the tray: <tad><figure> and each element */
IMPORT ER   ct_frag( CTBUF *b, CONST CTLNK *l, INT n );

/* ---------------------------------------------------------------- 印刷 */

/*
 * A month record as a page of 印刷 (design 11.22.6): the record with
 * its paper given ahead of its figure, the grid's own size (800 by 600
 * dots, 600 by 450 points), so that the month fills the page and can be
 * fitted to any printing paper.
 */
#define CT_PRINT_W	600		/* points */
#define CT_PRINT_H	450
IMPORT ER   ct_print_page( CTBUF *b, CONST UB *x, INT n );

/* ---------------------------------------------------------------- applists */

/*
 * An applist (JSON) with the entry the object opens with -- the one
 * whose "defaultOpen" is true -- keeping to it ("defaultOpenLock") or
 * not: E_NOEXS when it has no such entry. ct_app_locked says which.
 */
IMPORT ER   ct_app_lock( CTBUF *b, CONST UB *json, INT n, BOOL lock );
IMPORT BOOL ct_app_locked( CONST UB *json, INT n );

/*
 * A request about the link el (n bytes) given the link's applist in
 * text with DT_F_APPLIST, for the desktop to open it with the program
 * it keeps to; nothing added when the link has no applist or it does
 * not fit
 */
IMPORT void ct_link_req( T_DTREQ *rq, CONST UB *el, INT n );

/* The entries of an applist, in order: each one's key (the program's id) and "name"; how many */
#define CT_APP_ID	48
#define CT_APP_NAME	64
IMPORT INT  ct_app_list( CONST UB *json, INT n, UB (*ids)[CT_APP_ID], UB (*names)[CT_APP_NAME], INT max );

/*
 * An answer to DT_RQ_APPS, an entry of the list 実行 shows for a link
 * (the desktop's dt_app_list): the program's id and name, and whether it
 * is offered for the object's kind (DT_F_KIND). FALSE when it gives none.
 */
IMPORT BOOL ct_app_entry( CONST T_DTANS *an, UB *id, UB *name, BOOL *p_kind );

/* A colour "#rrggbb" of a JSON object's member by its path ("window", "backgroundColor"), or dflt */
IMPORT UW   ct_json_colour( CONST UB *json, INT n, CONST char *outer, CONST char *name, UW dflt );

#endif /* __CAL_TAD_H__ */
