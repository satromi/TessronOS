/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tadview.h
 *	What a document and a figure are, once read (design 16.3, 17.5)
 *
 *	The parser in tad.c gives a tree of elements; this gives the thing
 *	the tree means. A document becomes paragraphs of runs, a figure
 *	becomes shapes in the order they are drawn, and both keep pointing
 *	into the bytes the parser holds rather than copying the text.
 *
 *	The model is read-only. Editing is done on the tree, because that
 *	is what is written back and what keeps the bytes of everything
 *	nobody touched (16.3.3); the model is rebuilt from the tree after a
 *	change. Keeping the two apart is what lets a document be shown
 *	without the risk of the showing changing it.
 */

#ifndef __TS_TADVIEW_H__
#define __TS_TADVIEW_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/tad.h>
#include <ts/dp.h>

/* ---------------------------------------------------------------- documents */

/* What one run of a paragraph is */
#define TV_MAX_OVER	16		/* 用紙オーバーレイ, numbered 0 to 15 */

#define TV_RUN_TEXT	0		/* letters, all in one face and size */
#define TV_RUN_BREAK	1		/* <br/>: a new line inside a paragraph */
#define TV_RUN_LINK	2		/* a virtual object standing in the text */
#define TV_RUN_IMAGE	3		/* a picture standing in the text */
#define TV_RUN_TAB	4		/* <tab/>: on to the next tab stop */
#define TV_RUN_INDENT	5		/* <indent/>: later lines start here */
#define TV_RUN_PAGE	6		/* <pagebreak/>: a new page */
#define TV_RUN_FIXSP	7		/* <fixed-space>: a space of a width */
#define TV_RUN_FILL	8		/* <fill-char>: the rest of the line filled */
#define TV_RUN_PAGENO	9		/* <page-number>: the page's number */
#define TV_RUN_MEMO	10		/* <docmemo>: a note, not printed */
#define TV_RUN_FIGURE	11		/* a <figure> standing in the text: 'link' is
					   its number among the document's figures */
#define TV_RUN_VAR	12		/* <variable>: 'link' is its number, 'text'
					   its name when it has one (design 17.16) */

/* What a run is drawn in */
#define TV_ST_BOLD	0x0001
#define TV_ST_ITALIC	0x0002
#define TV_ST_UNDER	0x0004		/* 下線 */
#define TV_ST_OVER	0x0008		/* 上線 */
#define TV_ST_STRIKE	0x0010		/* 取り消し線 */
#define TV_ST_BOX	0x0020		/* 枠囲み */
#define TV_ST_INVERT	0x0040		/* 反転 */
#define TV_ST_MESH	0x0080		/* 網掛 */
#define TV_ST_BAG	0x0100		/* 袋文字: the outline of the letters */
#define TV_ST_SHADOW	0x0200		/* 影付き */
#define TV_ST_NOPRINT	0x0400		/* 無印字 */
#define TV_ST_SUP	0x0800		/* 上付き */
#define TV_ST_SUB	0x1000		/* 下付き */
#define TV_ST_COMB	0x2000		/* 結合文字: never broken */

#define TV_ALIGN_LEFT	0
#define TV_ALIGN_CENTRE	1
#define TV_ALIGN_RIGHT	2
#define TV_ALIGN_JUSTIFY 3

#define TV_MAX_TABS	16

typedef struct {
	UINT		kind;		/* TV_RUN_* */
	CONST UB	*text;		/* into the document's own bytes */
	INT		len;
	INT		size;		/* the letters' height */
	UW		colour;
	UINT		style;		/* TV_ST_* */
	INT		link;		/* which link, for TV_RUN_LINK */
	CONST UB	*href;		/* the file, for TV_RUN_IMAGE */

	CONST UB	*face;		/* the names <font face> gives, or NULL */
	INT		space;		/* between letters, thousandths of the size */
	INT		wr_n, wr_d;	/* width against height: 長体, 平体 */
	INT		hr_n, hr_d;	/* height against the size */
	UW		back;		/* the ground behind, or TAD_COL_NONE */
	UINT		bouten;		/* 傍点: 0, or side (1 above, 2 below) and kind << 4 */
	CONST UB	*ruby;		/* the ruby over or under it, or NULL */
	INT		ruby_len;
	BOOL		ruby_below;
	INT		ruby_group;	/* runs under the same <ruby> share this, else -1 */
	INT		width;		/* a fixed space's width, in pixels */

	/*
	 * 文字回転: each letter turned clockwise so many degrees in its own
	 * box (fn_set_angle). 文字基準位置移動: the run's baseline raised
	 * by 'lift' -- lowered when below nought -- in thousandths of the
	 * letters' size, or in pixels when lift_abs is set.
	 */
	INT		turn;
	INT		lift;
	BOOL		lift_abs;

	/*
	 * The node of the record it was made from -- the text node, or
	 * the <br>, <link> or <image> element -- and, for text, where in
	 * that node's text it begins. What edits the text edits the
	 * record through these.
	 */
	T_TADNODE	*node;
	INT		noff;
} T_TVRUN;

typedef struct {
	INT		first, n;	/* its runs */
	UINT		align;
	INT		indent;		/* the first line starts this far in */
	T_TADNODE	*node;		/* its <p>, or NULL for one it implies */

	/*
	 * From the tab format that holds for it: the space between its
	 * lines and after it -- a fraction of its letters' size in
	 * thousandths, or, when abs is set, pixels -- its margins, and its
	 * tab stops, from the left margin.
	 */
	INT		gap, pargap;
	BOOL		gap_abs, pargap_abs;
	INT		left, right;
	INT		ntabs;
	INT		tabs[TV_MAX_TABS];
	BOOL		page_before;	/* a page break stands at its head */

	/*
	 * The settings given at it, as 詳細 shows them: the <tab-format>
	 * standing at its head and the <text align> inside it; NULL for
	 * what it only carries on from before.
	 */
	T_TADNODE	*fmt;
	T_TADNODE	*align_at;

	/*
	 * 禁則 as the document gives it: the letters that may not begin
	 * (head) and may not end (tail) a line, and the kind 0xKL, K 0
	 * saying none; the kind is -1 when the document does not say, and
	 * the viewer's own sets hold.
	 */
	CONST UB	*khead, *ktail;
	INT		khead_len, ktail_len;
	INT		khead_kind, ktail_kind;
} T_TVPARA;

typedef struct t_tvdoc {
	T_DPRECT	view;		/* where it is shown, when it says */
	T_DPRECT	draw;		/* and what part of it is drawn */
	INT		hunit, vunit;	/* the scale it was written at */
	INT		margin[4];	/* left, top, right, bottom */
	INT		lang, bpat;
	INT		npara;
	INT		nrun;
	T_TVPARA	*para;
	T_TVRUN		*run;

	/*
	 * The paper it is laid on, when it says, and its margins: in
	 * pixels, whatever units the record wrote them in. 0 when it does
	 * not say.
	 */
	INT		paper_w, paper_h;
	INT		columns, colsp;

	/*
	 * How whoever shows it wants it shown; the record does not say.
	 * With page_h set it is laid on pages that tall, each inside the
	 * margins, a page break going on to the next; without, it runs on
	 * in one length with no margins, as a window shows it. 'detail'
	 * draws the marks of breaks, tabs, pages and notes; 'show_hidden'
	 * shows the links that are marked hidden.
	 */
	INT		page_h;
	BOOL		detail;
	BOOL		show_hidden;

	/* the figures standing in the text, made when it is read */
	INT		nfig;
	struct t_tvfig	**figs;

	/*
	 * 用紙オーバーレイ: what is laid on the pages as well as the text,
	 * by number -- 0 the header, 1 the footer, and any other in the top
	 * margin unless its text begins with a fill line (foot) -- and which
	 * of them the document puts on. 'over_pages' is P: 0 every page, 1 the odd, 2
	 * the even.
	 */
	struct t_tvdoc	*over[TV_MAX_OVER];
	UINT		over_pages[TV_MAX_OVER];
	UINT		over_on;

	/*
	 * As an overlay: its text begins with a <fill-line>, which pushes
	 * what follows to the foot of the page, so it stands in the bottom
	 * margin whatever its number.
	 */
	BOOL		foot;
} T_TVDOC;

/* ---------------------------------------------------------------- figures */

#define TV_SH_RECT	1
#define TV_SH_ELLIPSE	2
#define TV_SH_LINE	3		/* a straight line or a run of them */
#define TV_SH_POLY	4		/* closed */
#define TV_SH_IMAGE	5
#define TV_SH_DOC	6		/* a piece of text standing in the figure */
#define TV_SH_LINK	7		/* a virtual object */
#define TV_SH_GROUP	8
#define TV_SH_ARC	9		/* a sector: the arc and two radii */
#define TV_SH_CHORD	10		/* the arc and the line across its ends */
#define TV_SH_EARC	11		/* the arc alone */
#define TV_SH_CURVE	12		/* a curve through points */
#define TV_SH_PIXMAP	13		/* a picture painted in the figure */
#define TV_SH_MARKER	14		/* a mark at each of its points: rad_h is its
					   size, rad_v its kind (0 dot, 1 plus,
					   2 star, 3 circle, 4 cross, 5 diamond) */

#define TV_MAX_PT	1024		/* points of one shape */
#define TV_PT_POOL	16384		/* points of all the shapes of a figure */

/*
 * A pattern of the figure's own, as it is drawn: sixteen by sixteen
 * pixels, each its colour, and the mask of which of them show -- a row
 * a word, the leftmost pixel in the top bit. 'colour' is its first
 * colour, for whatever needs one colour rather than a pattern.
 */
#define TV_MAX_FPAT	64		/* a figure's own patterns that are kept */

typedef struct t_tvpat {
	INT		id;
	UW		tile[16 * 16];
	UW		mask[16];
	UW		colour;
} T_TVPAT;

/* How a line that joins two shapes runs between them */
#define TV_CONN_STRAIGHT 0
#define TV_CONN_ELBOW	1
#define TV_CONN_CURVE	2

typedef struct {
	UINT		kind;		/* TV_SH_* */
	INT		z;		/* the order it is drawn in */
	T_DPRECT	r;		/* its box: an ellipse's frame */
	INT		npt;
	T_DPPOINT	*pt;		/* in the figure's pool */
	UW		line_col;
	UW		fill_col;
	INT		line_w;
	UINT		line_type;	/* 0 実線 1 破線 2 点線 3 一点鎖線 4 二点鎖線 5 長破線 */
	INT		rad_h, rad_v;	/* a rectangle's corners */
	INT		doc;		/* which document, for TV_SH_DOC */
	INT		link;		/* which link, for TV_SH_LINK */
	CONST UB	*href;		/* the file, for TV_SH_IMAGE and TV_SH_PIXMAP */
	CONST UB	*text;		/* a rectangle's own text (fontSize or textColor), or NULL */
	INT		text_px;	/* and its size and colour */
	UW		text_col;
	INT		arrow;		/* 1 at the end, 2 at the start, 3 both */
	INT		arrow_type;	/* 0 lines, 1 filled */
	T_TADNODE	*node;		/* the element it was made from */
	CONST UH	*fill_tex;	/* a fill that is a mask, not a colour */
	CONST T_TVPAT	*fill_pat;	/* or a pattern of the figure's own */
	INT		rot;		/* turned about its middle, in degrees */
	INT		angle;		/* an ellipse's own turn, in 4096ths */
	INT		a0, a1;		/* where an arc starts and ends, 4096ths */
	BOOL		closed;		/* a curve that is closed and filled */
	BOOL		flip_h, flip_v;	/* a picture turned over */
	UW		back;		/* what a painted picture is laid on */

	/*
	 * Its place among the figure's own elements, counting from 0 --
	 * what a joined line's ends name -- and, for a line joined to
	 * shapes, which by their place and which of their points.
	 */
	INT		place;
	UINT		conn;		/* TV_CONN_* */
	INT		c0_shape, c0_point;	/* -1 when that end is free */
	INT		c1_shape, c1_point;
	BOOL		hidden;		/* a link marked hidden (隠蔽仮身) */
} T_TVSHAPE;

#define TV_MAX_SHAPE	256
#define TV_MAX_LTYPE	16		/* kinds of line a figure defines */
#define TV_DASH_MAX	16
#define TV_MAX_DOC	32

typedef struct t_tvfig {
	T_DPRECT	view, draw;
	INT		hunit, vunit;
	INT		nsh;
	T_TVSHAPE	*sh;
	INT		ndoc;
	T_TVDOC		**doc;		/* the pieces of text standing in it */
	T_DPPOINT	*pool;		/* the points of every shape */
	INT		npool;

	/*
	 * What it is magnified by, in eighths: 8 is life size, and 0 is
	 * taken as 8. Whoever shows it sets this; the record does not say.
	 * The places it is drawn at and asked about are then the figure's
	 * multiplied by it, and a scroll is in those magnified pixels.
	 */
	INT		zoom;

	/* the links marked hidden are drawn and can be pressed; else neither */
	BOOL		show_hidden;

	/* the patterns it defines for itself, as they are used */
	T_TVPAT		*pats;
	INT		npats;

	/*
	 * The paper it is laid on, when the record says, in points, and
	 * its margins (left, top, right, bottom): given before the figure
	 * itself, in <paper> and <docmargin>. 0 when it does not say.
	 */
	INT		paper_w, paper_h;
	INT		pmargin[4];

	/*
	 * The kinds of line it defines (<lineTypeDefine>): each id's
	 * dashes, lengths on, off, on ... in pixels at a width of one, 0
	 * ending them; lt_dash[i][0] 0 is a solid line.
	 */
	INT		nlt;
	INT		lt_id[TV_MAX_LTYPE];
	UB		lt_dash[TV_MAX_LTYPE][TV_DASH_MAX];

	/*
	 * 用紙オーバーレイ (<figoverlay>): the shapes laid on the paper
	 * under the figure's own, by number, and which of them the figure
	 * puts on. 'over_pages' is 0 every page, 1 the odd, 2 the even, 3
	 * none. A figure is shown as one sheet, its first page.
	 */
	struct t_tvfig	*over[TV_MAX_OVER];
	UINT		over_pages[TV_MAX_OVER];
	UINT		over_on;
} T_TVFIG;

/*
 * A pattern the figure defines for itself -- one of its own numbers, or
 * one of the fixed numbers it draws its own way -- or NULL.
 */
IMPORT CONST T_TVPAT *tv_fig_pattern( CONST T_TVFIG *f, INT id );

/* figgeom.c: angles in 4096ths of a turn, sines in 16384ths */
IMPORT INT  tv_sin( INT a );
IMPORT INT  tv_cos( INT a );
IMPORT INT  tv_atan2( D dy, D dx );
IMPORT D    tv_isqrt( D v );
IMPORT INT  tv_arc_points( T_DPPOINT *out, INT max, D cx, D cy, D rx, D ry,
			   INT a0, INT a1, INT rot );
IMPORT void tv_turn_points( T_DPPOINT *p, INT n, D cx, D cy, INT rot );
IMPORT INT  tv_curve_points( T_DPPOINT *out, INT max, CONST T_DPPOINT *p, INT np );
IMPORT INT  tv_bezier_points( T_DPPOINT *out, INT max, D x0, D y0, D x1, D y1,
			      D x2, D y2, D x3, D y3 );
IMPORT INT  tv_shape_outline( CONST T_TVSHAPE *s, T_DPPOINT *out, INT max,
			      BOOL *p_closed );
IMPORT void tv_stroke( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed, INT w,
		       UINT type, CONST T_DPPAT *pat );
/* the same with dashes of its own (lengths on, off ... 0 ending them) */
IMPORT void tv_stroke_dash( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed, INT w,
			    CONST UB *dash, CONST T_DPPAT *pat );
/* the dashes of a kind of line the figure defines, or NULL */
IMPORT CONST UB *tv_fig_dash( CONST T_TVFIG *f, UINT type );
IMPORT BOOL tv_connector( CONST T_TVSHAPE *s, INT k, T_DPPOINT *p,
			  T_DPPOINT *dir );
IMPORT CONST T_TVSHAPE *tv_fig_place( CONST T_TVFIG *f, INT place );
IMPORT INT  tv_line_path( CONST T_TVFIG *f, CONST T_TVSHAPE *s, T_DPPOINT *out,
			  INT max );

/* ---------------------------------------------------------------- reading */

/*
 * The model of what a parsed record says. Answers E_NOEXS when the
 * record is not of that kind, so a caller may try both and take the one
 * that answers -- which is what opening a record nobody said the kind
 * of comes down to.
 */
IMPORT ER  tv_doc( CONST T_TAD *doc, T_TVDOC **p_out );
IMPORT ER  tv_fig( CONST T_TAD *doc, T_TVFIG **p_out );
IMPORT void tv_doc_free( T_TVDOC *d );
/* The same for the <document> element of a piece of text in a figure */
IMPORT ER  tv_doc_of( CONST T_TADNODE *nd, T_TVDOC **p_out );

/*
 * 詳細: the marks the settings are shown by. A paragraph with a tab
 * format of its own has a ruler above it, with its margins, its first
 * line and its tab stops on it; an alignment is a flag at its right;
 * a note, a page number and a fixed space are flags in the line.
 */
#define TV_MARK_NONE	0
#define TV_MARK_BAR	1		/* the ruler, off its markers */
#define TV_MARK_ORIGIN	2		/* where it counts from: 絶対 or 相対 */
#define TV_MARK_FHEAD	3		/* 文頭位置: where the first line starts */
#define TV_MARK_LHEAD	4		/* 行頭位置: the left margin */
#define TV_MARK_LEND	5		/* 行末位置: the right margin */
#define TV_MARK_TAB	6		/* a tab stop; 'index' says which */
#define TV_MARK_ALIGN	7		/* the flag of an alignment */
#define TV_MARK_RUN	8		/* the flag of a run; 'run' says which */

typedef struct {
	INT		kind;		/* TV_MARK_* */
	INT		para, run, index;
	T_TADNODE	*node;		/* the element it shows */
	INT		zero;		/* x of the ruler's left end: the paper's edge */
	INT		head;		/* x of the left margin, where tabs count from */
	INT		end;		/* x of the ruler's right end */
	INT		unit;		/* one letter on the ruler, in pixels */
	INT		at;		/* x of the place a marker stands for */
	T_DPRECT	box;
} T_TVMARK;

/* The mark under a place, as the document is drawn in detail: its kind */
IMPORT INT tv_doc_mark( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			CONST T_TAD *src, INT x, INT y, T_TVMARK *out );
/* The same for a <figure> element standing in a text */
IMPORT ER  tv_fig_of( CONST T_TADNODE *nd, struct t_tvfig **p_out );
IMPORT void tv_fig_free( T_TVFIG *f );

/* Which kind a record is, without building the model */
#define TV_KIND_NONE	0
#define TV_KIND_DOC	1
#define TV_KIND_FIG	2

IMPORT UINT tv_kind( CONST T_TAD *doc );

/*
 * The fixed patterns a shape's l_pat and f_pat name: the colour of one,
 * and for one that is a mask rather than a colour, its sixteen rows
 * (the colour is then what the mask is drawn in). FALSE past the fixed
 * ones. tv_pattern_of is the fixed pattern that is a colour, or 0.
 */
#define TV_PAT_FIXED	128
IMPORT BOOL tv_pattern( INT id, UW *p_col, CONST UH **p_tex );
IMPORT INT  tv_pattern_of( UW col );

/*
 * A fill that is a sixteen by sixteen mask drawn in a colour. The
 * pattern lives in one buffer, so it holds until the next is made.
 */
IMPORT void tv_tex_pat( T_DPPAT *pat, UW col, CONST UH *tex );

/* A colour written the way the records write them, "#rrggbb" */
IMPORT UW  tv_colour( CONST UB *s, UW dflt );

/* ------------------------------------------------- what a link points at */

/*
 * A link says which real object it refers to; it does not carry that
 * object's name or its content. Both live in the store, and the viewer
 * has no business knowing what a store is -- so whoever draws a
 * document says how to reach one.
 *
 *   name  fills in what the object is called, and answers how many
 *         bytes that came to, or a negative number when it cannot be
 *         found. A link that carries a name of its own is not asked.
 *   open  answers the record the link points at, parsed, or NULL.
 *   shut  gives back what open answered.
 *
 * With no source set, a link is drawn with what it carries and nothing
 * more, which is what a document with no store behind it should look
 * like.
 */
typedef struct {
	INT	(*name)( CONST T_VOBJ *v, UB *buf, INT max, void *arg );
	T_TAD	*(*open)( CONST T_VOBJ *v, void *arg );
	void	(*shut)( T_TAD *doc, void *arg );

	/*
	 * What the object is written on. A real object carries the colour
	 * of its own paper and a viewer that lays every document on white
	 * shows a document nobody wrote. NULL, or an answer below E_OK,
	 * means the system's own paper.
	 */
	ER	(*paper)( CONST T_VOBJ *v, UW *p_colour, void *arg );

	/*
	 * A picture a record names, by the file name it gives. What comes
	 * back is kept by the source and is not the caller's to free;
	 * the viewer only lays it down.
	 */
	ER	(*picture)( CONST UB *href, CONST UW **p_px, INT *p_w,
			    INT *p_h, void *arg );

	/*
	 * The words of a link's band as the link asks for them: the
	 * object's name (the link's own when the object has none), " : "
	 * and its relationship, " (" the program it opens with ")", and
	 * the date it was last changed. Answers the length, or a negative
	 * number to draw the link's own name.
	 */
	INT	(*label)( CONST T_VOBJ *v, UB *buf, INT max, void *arg );

	/*
	 * The object's own icon, kept by the source, 0x00rrggbb with
	 * IMG_CLEAR where it is clear. Below E_OK: it has none.
	 */
	ER	(*icon)( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h,
			 void *arg );
} T_TVSRC;

/*
 * A picture the record names, laid into a box at the box's size. The
 * room is framed when there is no picture to put in it, so that what is
 * around it stays where it was written.
 */
IMPORT void tv_draw_picture( INT gid, CONST T_DPRECT *box, CONST UB *href );

/*
 * The paper to lay a document or a figure on. TV_PAPER_LOOK is the
 * system's own, from the numbered table; anything else is a colour.
 */
#define TV_PAPER_LOOK	0xFFFFFFFFU

IMPORT void tv_source( CONST T_TVSRC *src, void *arg );

/*
 * What tv_source() named, for the drawing to ask. Kept with the reading
 * of records (outer_kernel/tad/tad_view.c) rather than with the drawing,
 * so that the store can name itself on a machine that draws nothing.
 */
IMPORT CONST T_TVSRC	*knl_tv_src;
IMPORT void		*knl_tv_src_arg;

/*
 * One virtual object drawn in the rectangle given. A box no taller than
 * its own band is drawn closed -- the band, the picture, the name. A
 * taller one is open: the band, and under it the real object it points
 * at, drawn as a document or a figure in its own right.
 *
 * 'depth' is how many open objects deep this already is. An open object
 * may hold another, and a document may hold a link to itself; the depth
 * is what keeps that from going on for ever.
 */
IMPORT ER  tv_draw_link( INT gid, CONST T_DPRECT *r, CONST T_VOBJ *v,
			 INT depth );

/* The depth of the opened object a text is being drawn inside: its links are drawn that deep */
IMPORT INT tv_open_depth;

#define TV_MAX_DEPTH	3

/* ---------------------------------------------------------------- drawing */

/*
 * Draw a document into a drawing environment. The rectangle is where it
 * goes, in the caller's own coordinates; the scroll is how far into the
 * document the top of that rectangle is.
 *
 * Answers how tall the whole document came to, so that a caller may set
 * a scroll bar without laying it out twice.
 *
 * 'src' is the record the model was read from, and may be NULL. It is
 * wanted only for the virtual objects standing in the text: what one of
 * them shows -- its name, its colours -- is in the record, and the model
 * keeps only which link it is.
 */
IMPORT INT tv_doc_draw( INT gid, CONST T_TVDOC *d, CONST T_DPRECT *r,
			INT scroll_y, CONST T_TAD *src, UW paper );

/*
 * A record of xmlTAD drawn into a box as whatever it is -- a document
 * set out from the top, a figure from its corner -- on the work area's
 * colour. This is what draws a window whose drawing record is written
 * (design 18.13, wm_obj_painter).
 */
IMPORT ER  tv_paint( INT gid, CONST T_DPRECT *r, CONST UB *xml, SZ len );

/*
 * The region a figure covers in a box w by h from its corner: every
 * pixel it draws, outlines and fills alike. What makes a window's
 * outline from its shape record (wm_obj_painter).
 */
IMPORT ER  tv_shape( CONST UB *xml, SZ len, INT w, INT h, T_DPRGN **p_rgn );

/* How tall it would be, laid out to that width, without drawing it */
/*
 * How tall it comes out in a box that wide: the same reckoning the
 * drawing does, with nothing drawn. The record is wanted because a link
 * is as wide as the record says, and a height measured without it is a
 * height the drawing will not agree with.
 */
IMPORT INT tv_doc_height( CONST T_TVDOC *d, INT width, CONST T_TAD *src );

/* The same for a figure. Shapes are drawn in the order they say. */
/*
 * Which virtual object is at a place, if any: E_NOEXS when none is.
 * The place is in the coordinates the document or figure was drawn in,
 * and what comes back is the link itself and the box it was drawn in.
 *
 * A document is asked by laying it out again with nothing drawn, so the
 * answer is about the page that is on the screen and not about a second
 * reckoning of where the links might be.
 */
/* How far a figure reaches: its view, and every shape in it */
IMPORT ER  tv_fig_size( CONST T_TVFIG *f, INT *p_w, INT *p_h );

/*
 * The caret in a document: a place is a paragraph and a count into it
 * -- the bytes of its text, and one for each virtual object, picture
 * or break standing in it. The place under a point (the nearest one),
 * where a place is drawn (a box two pixels wide, as tall as its line),
 * and a stretch between two places turned over.
 */
/* Whether the marks of breaks, tabs, pages and notes are drawn too */
IMPORT void tv_doc_detail( BOOL on );
/* What documents are drawn magnified by, in eighths, until set again */
IMPORT void tv_doc_zoom( INT z );
IMPORT ER  tv_doc_caret_at( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			    CONST T_TAD *src, INT x, INT y, INT *p_para,
			    INT *p_pos );
IMPORT ER  tv_doc_caret_box( CONST T_TVDOC *d, CONST T_DPRECT *r,
			     INT scroll_y, CONST T_TAD *src, INT para, INT pos,
			     T_DPRECT *box );
IMPORT void tv_doc_select( INT gid, CONST T_TVDOC *d, CONST T_DPRECT *r,
			   INT scroll_y, CONST T_TAD *src, INT a_para,
			   INT a_pos, INT b_para, INT b_pos );
IMPORT INT tv_doc_thing( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			 CONST T_TAD *src, INT x, INT y, INT want,
			 T_DPRECT *p_box );
IMPORT ER  tv_doc_at( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
		      CONST T_TAD *src, INT x, INT y, T_VOBJ *out,
		      T_DPRECT *p_box, INT *p_which );
IMPORT ER  tv_fig_at( CONST T_TVFIG *f, CONST T_DPRECT *r, INT scroll_x,
		      INT scroll_y, CONST T_TAD *src, INT x, INT y,
		      T_VOBJ *out, T_DPRECT *p_box, INT *p_which );

/*
 * The shape at a point, whatever it is, the one in front first: a line
 * within a few pixels of it, anything else inside its box. Answers
 * which shape it is.
 */
IMPORT ER  tv_fig_shape_at( CONST T_TVFIG *f, CONST T_DPRECT *r,
			    INT scroll_x, INT scroll_y, INT x, INT y,
			    INT *p_which );
IMPORT ER  tv_fig_draw( INT gid, CONST T_TVFIG *f, CONST T_DPRECT *r,
			INT scroll_x, INT scroll_y, CONST T_TAD *src,
			UW paper );
IMPORT ER  tv_fig_draw_at( INT gid, CONST T_TVFIG *f, CONST T_DPRECT *r,
			   INT scroll_x, INT scroll_y, CONST T_TAD *src,
			   UW paper, INT depth );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TADVIEW_H__ */
