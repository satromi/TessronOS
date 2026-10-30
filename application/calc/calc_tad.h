/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_tad.h
 *	基本表計算: the sheet and the record it is read from and written to
 *	(design 11.13)
 *
 *	A sheet's record 0 is a text: a paragraph for each row, the cells of
 *	a row apart by tabs, each opened by <calcPos cell="B3"/>. A cell's
 *	frame and ground are a <calcCell .../> after it, its placing a
 *	<text align/> and a <cell valign/>, its letters' look the <font/>
 *	and decorations round its words; it holds words, a formula (from
 *	"="), a link or an image. Before the first paragraph are the sizes
 *	of the rows and columns that are not the usual (<calcSize/>) and the
 *	cells joined (<calcMerge/>); after the last, the charts, each a
 *	<group> of a <calc> naming its kind and its cells and the figure it
 *	is drawn as.
 *
 *	A sheet here is its cells, kept in the order of rows and columns,
 *	their words as they were written or typed, and what the program
 *	shows of each (what the formula came to). Reading takes in what an
 *	older record may hold: the cells named by row and column, and
 *	formulas with "［1，2］" for a cell and the letters in full width.
 *
 *	Nothing here makes a system call. Memory is taken through cx_alloc
 *	and cx_free, which the user gives: the program (calc_main.c) and
 *	the kernel's test both use it.
 */

#ifndef __CALC_TAD_H__
#define __CALC_TAD_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include "../calendar/cal_tad.h"

/* The grid */
#define CX_COLS_MAX	16384		/* XFD */
#define CX_ROWS_MAX	1048576		/* as xlsx has them */
#define CX_ROWS		100		/* the grid at least */
#define CX_COLS		26
#define CX_CELL_W	80		/* a column's usual width */
#define CX_CELL_H	22		/* a row's usual height */

#define CX_COLOUR	32		/* bytes of a colour as written, with its end */
#define CX_FACE		64		/* bytes of a face's name */

/* A side of a cell's frame */
#define CX_TOP		0
#define CX_BOTTOM	1
#define CX_LEFT		2
#define CX_RIGHT	3

#define CX_SOLID	0
#define CX_DASHED	1
#define CX_DOTTED	2
#define CX_DOUBLE	3

typedef struct {
	BOOL	on;
	INT	type;			/* CX_SOLID .. CX_DOUBLE */
	INT	width;
	char	colour[CX_COLOUR];
} CXSIDE;

/* What of a cell's look was said (CXSTYLE.has) */
#define CX_S_BG		0x0001
#define CX_S_FRAME	0x0002
#define CX_S_ALIGN	0x0004
#define CX_S_VALIGN	0x0008
#define CX_S_COLOUR	0x0010
#define CX_S_SIZE	0x0020
#define CX_S_FACE	0x0040
#define CX_S_BOLD	0x0080
#define CX_S_ITALIC	0x0100
#define CX_S_UNDER	0x0200
#define CX_S_STRIKE	0x0400
#define CX_S_SUPER	0x0800
#define CX_S_SUB	0x1000
#define CX_S_MESH	0x2000
#define CX_S_INVERT	0x4000

typedef struct {
	UINT	has;			/* CX_S_* */
	char	bg[CX_COLOUR];		/* the ground */
	CXSIDE	side[4];		/* CX_TOP .. CX_RIGHT */
	char	align[16];		/* left, center, right */
	char	valign[16];		/* top, middle, bottom */
	char	colour[CX_COLOUR];	/* the letters' */
	INT	size;			/* points */
	char	face[CX_FACE];
} CXSTYLE;

/* What a cell holds */
#define CX_TEXT		0		/* words, or a formula */
#define CX_LINK		1		/* a link (its <link/> element) */
#define CX_IMAGE	2		/* an image (its <image/> element) */

typedef struct {
	INT	col, row;
	INT	kind;			/* CX_TEXT .. CX_IMAGE */
	UB	*text;			/* the words or the formula; never NULL */
	UB	*shown;			/* what it shows; NULL: its text */
	UB	*el;			/* a link's or an image's element */
	INT	ellen;
	/*
	 * Its look, shared with every cell that looks the same (cx_style);
	 * never NULL, and never written through: a cell given another look
	 * is given another shared one (cx_style_put, cx_style_add).
	 */
	CONST CXSTYLE	*st;
} CXCELL;

typedef struct {
	INT	at, size;
} CXSIZE;

typedef struct {
	INT	c0, r0, c1, r1;		/* from, to, both in it */
} CXRANGE;

/* The kinds of chart */
#define CX_CH_BAR	0		/* clustered-bar */
#define CX_CH_SBAR	1		/* stacked-bar */
#define CX_CH_LINE	2		/* line */
#define CX_CH_SLINE	3		/* stacked-line */
#define CX_CH_KINDS	4

typedef struct {
	INT	kind;			/* CX_CH_*; -1 a kind not known (kept as read) */
	char	name[24];		/* the kind as written */
	INT	left, top, right, bottom;
	BOOL	has_range;
	CXRANGE	range;
	UB	*el;			/* the <group> as read, written again when the kind is not known */
	INT	ellen;
} CXCHART;

typedef struct {
	CXCELL	**c;			/* by row, then column */
	INT	n, max;
	CXSIZE	*w, *h;			/* columns' widths, rows' heights not the usual */
	INT	nw, nh, mw, mh;
	CXRANGE	*m;			/* cells joined, in the order they were */
	INT	nm, mm;
	CXCHART	*ch;
	INT	nch, mch;
	UB	*paper, *margin;	/* <paper .../>, <docmargin .../> as read */
	INT	maxcol, maxrow;		/* how far the grid goes */
} CXSHEET;

/* Memory, the user's */
IMPORT void *cx_alloc( INT size );
IMPORT void cx_free( void *p );

/* ---------------------------------------------------------------- names */

IMPORT INT  cx_col_name( INT col, char *out );			/* "A", "AB"; its length */
IMPORT INT  cx_col_num( CONST UB *s, INT n );			/* "AB" -> 28 */
IMPORT INT  cx_cell_name( INT col, INT row, char *out );	/* "B3" */
IMPORT BOOL cx_cell_parse( CONST UB *s, INT n, INT *p_col, INT *p_row );
IMPORT BOOL cx_range_parse( CONST UB *s, INT n, CXRANGE *r );	/* "A1:B3" */

/* ---------------------------------------------------------------- the sheet */

IMPORT void cx_init( CXSHEET *s );
IMPORT void cx_free_all( CXSHEET *s );
IMPORT CXCELL *cx_get( CONST CXSHEET *s, INT col, INT row );
IMPORT CXCELL *cx_make( CXSHEET *s, INT col, INT row );	/* there, empty, when it was not */
IMPORT void cx_remove( CXSHEET *s, INT col, INT row );
/* Every cell of a range taken away at once; how many there were */
IMPORT INT  cx_remove_range( CXSHEET *s, CONST CXRANGE *r );
IMPORT BOOL cx_set_text( CXCELL *c, CONST UB *text );		/* words; a link or image let go */
IMPORT BOOL cx_set_el( CXCELL *c, INT kind, CONST UB *el, INT len );
IMPORT BOOL cx_set_shown( CXCELL *c, CONST UB *s, INT len );
IMPORT CONST UB *cx_shown( CONST CXCELL *c );			/* what it shows */
IMPORT INT  cx_width( CONST CXSHEET *s, INT col );
IMPORT INT  cx_height( CONST CXSHEET *s, INT row );
IMPORT void cx_set_width( CXSHEET *s, INT col, INT w );
IMPORT void cx_set_height( CXSHEET *s, INT row, INT h );
IMPORT void cx_grow( CXSHEET *s, INT col, INT row );		/* the grid made to reach a cell */

/* A row or a column put in before 'at', or taken out: the cells and the sizes after it move */
IMPORT void cx_insert_row( CXSHEET *s, INT at );
IMPORT void cx_insert_col( CXSHEET *s, INT at );
IMPORT void cx_delete_row( CXSHEET *s, INT at );
IMPORT void cx_delete_col( CXSHEET *s, INT at );

/* Cells joined: the join a cell is in (its index), or -1 */
IMPORT INT  cx_merge_of( CONST CXSHEET *s, INT col, INT row );
IMPORT BOOL cx_merge_add( CXSHEET *s, CONST CXRANGE *r );
IMPORT void cx_merge_del( CXSHEET *s, INT i );

IMPORT BOOL cx_chart_add( CXSHEET *s, CONST CXCHART *c );
IMPORT void cx_chart_del( CXSHEET *s, INT i );

/* ---------------------------------------------------------------- the record */

/*
 * A record read into an empty sheet. The cells come in the order the
 * record has them, which is the order the formulas are to be worked
 * out in; cx_order puts them in the sheet's order after. E_NOMEM when
 * memory ran out.
 */
IMPORT ER   cx_read( CXSHEET *s, CONST UB *x, INT n );
IMPORT void cx_order( CXSHEET *s );

/*
 * The record of a sheet. A chart of a kind known is written by fn (the
 * program's: its figure is drawn from the cells), or as it was read when
 * fn is NULL.
 */
typedef void (*CX_CHARTFN)( CTBUF *b, CONST CXSHEET *s, INT i, void *arg );
IMPORT void cx_write( CTBUF *b, CONST CXSHEET *s, CONST UB *name, CX_CHARTFN fn, void *arg );

/* A cell's words as the record keeps them: XML-escaped, in its decorations */
IMPORT void cx_esc( CTBUF *b, CONST UB *s, INT n );

/* ---------------------------------------------------------------- formulas */

/* What a cell read from a record holds: full width marks made plain, a result before "=" taken off, ［r，c］ as A1 */
IMPORT INT  cx_old_form( CONST UB *in, INT n, UB *out, INT max );

/*
 * A formula copied cols across and rows down: every reference not held
 * by "$" moved, one that leaves the grid becoming #REF!. Words that are
 * not a formula are copied as they are. The length written.
 */
IMPORT INT  cx_move_refs( CONST UB *in, INT n, INT cols, INT rows, INT maxcol, INT maxrow,
			  UB *out, INT max );

/* ---------------------------------------------------------------- a cell's look */

IMPORT void cx_style_merge( CXSTYLE *to, CONST CXSTYLE *from );	/* what from says, over to */

/*
 * The looks the cells share. cx_plain is the look of a cell nothing was
 * said of. cx_style gives the one shared look the same as st (made when
 * there was none), NULL when there is no memory for it; it lasts as long
 * as the program. cx_style_put gives a cell the look st, cx_style_add
 * what from says over its own; FALSE (the cell as it was) for want of
 * memory.
 */
IMPORT CONST CXSTYLE	cx_plain;
IMPORT CONST CXSTYLE *cx_style( CONST CXSTYLE *st );
IMPORT BOOL cx_style_put( CXCELL *c, CONST CXSTYLE *st );
IMPORT BOOL cx_style_add( CXCELL *c, CONST CXSTYLE *from );

/* The frames of a range: all, outline, top, bottom, left, right, none */
#define CX_FR_ALL	0
#define CX_FR_OUTLINE	1
#define CX_FR_TOP	2
#define CX_FR_BOTTOM	3
#define CX_FR_LEFT	4
#define CX_FR_RIGHT	5
#define CX_FR_NONE	6
IMPORT void cx_frame_range( CXSHEET *s, CONST CXRANGE *r, INT how, CONST CXSIDE *side );

/* ---------------------------------------------------------------- 印刷 (calc_print.c) */

/*
 * The sheet as pages for 印刷: each a figure of xmlTAD on the sheet's
 * paper, handed to put; written in mem (max bytes). A chart is written
 * by 'chart' (NULL: none), its shapes moved by dx, dy from where the
 * grid's corner puts them. Answers how many pages, or an error.
 */
typedef struct {
	ER	(*put)( void *arg, CONST UB *xml, INT n );
	void	*arg;
	void	(*chart)( CTBUF *b, CONST CXSHEET *s, INT i, INT dx, INT dy, void *arg );
	void	*carg;
	UB	*mem;
	INT	max;
	/*
	 * Or, when set, a page's text grows as long as it is (ct_buf_grow)
	 * through 'more', which lets the text go when asked for 0 bytes
	 */
	void	*(*more)( void *p, SZ size );
} CXPRINT;

/* The range printed: every cell that is not empty; FALSE when there is none */
IMPORT BOOL cx_print_range( CONST CXSHEET *s, CXRANGE *r );
IMPORT INT  cx_print( CONST CXSHEET *s, CONST CXPRINT *pr );

#endif /* __CALC_TAD_H__ */
