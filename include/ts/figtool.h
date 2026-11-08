/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figtool.h
 *	What the figure editor's 道具パネル draws with and sets, apart from
 *	the windows it is shown in (application/fig/figtool.c, design 17.13)
 *
 *	The look new shapes are drawn with (T_FTLOOK), the tool in hand,
 *	the shape being drawn and how it is put into the record, the grid,
 *	what the palettes offer and how they lay out, and the fixed patterns
 *	of the 塗りつぶし palette drawn and pressed. The same code serves the
 *	figure editor of the desktop and a figure editor running as a
 *	process. A figure is worked on through T_FTVIEW: the figure
 *	editor's state (T_FGED, include/ts/figedit.h) with where the figure
 *	is drawn in its window, and T_FTHOST, what only the window's owner
 *	does -- drawing it again, putting it on the screen, a piece of text
 *	edited, a canvas, what the panel shows.
 */

#ifndef __TS_FIGTOOL_H__
#define __TS_FIGTOOL_H__

#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/figedit.h>
#include <ts/docedit.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- the look */

/* What a new shape is drawn with, as the palettes set it */
typedef struct {
	UW	fill;			/* the colour of the inside */
	BOOL	fill_on;		/* filled at all */
	INT	fill_pat;		/* a fixed pattern chosen, or 0 for the colour */
	UW	stroke;			/* the colour of lines */
	INT	width;
	INT	ltype;			/* 0 実線 .. 5 長破線 */
	INT	conn;			/* TV_CONN_*: how a joined line runs */
	INT	corner;			/* the corners' radius */
	INT	arrows;			/* 0 none, 1 start, 2 end, 3 both */
	BOOL	arrow_filled;
	INT	grid_mode;		/* FT_GRID_* */
	INT	grid;			/* its spacing */
	INT	zoom;			/* in eighths: 8 is life size */

	/* 画材: what the canvas paints with, and how wide */
	INT	paint_tool;
	INT	paint_size;

	/* the look of text in a new box of text */
	INT	font_size;
	INT	font_face;		/* 0 ゴシック, 1 明朝, 2 等幅 */
	UW	text_col;
	BOOL	bold, italic, under, strike;
} T_FTLOOK;

#define FT_GRID_NONE	0
#define FT_GRID_SHOW	1
#define FT_GRID_SNAP	2

IMPORT T_FTLOOK	ft_look;

/* ---------------------------------------------------------------- the tools */

#define FT_TL_SELECT	0
#define FT_TL_CANVAS	1
#define FT_TL_CURVE	2
#define FT_TL_LINE	3
#define FT_TL_ARC	4		/* 扇形 */
#define FT_TL_CHORD	5		/* 弦 */
#define FT_TL_EARC	6		/* 楕円弧 */
#define FT_TL_RECT	7
#define FT_TL_POLYGON	8
#define FT_TL_TRIANGLE	9
#define FT_TL_ELLIPSE	10
#define FT_TL_TEXT	11
#define FT_TL_POLYLINE	12		/* 折れ線 */
#define FT_TL_N		13

IMPORT INT	ft_tool;		/* the tool in hand */
IMPORT BOOL	ft_centre[FT_TL_N];	/* each that can, drawing from the middle out */
IMPORT BOOL	ft_centre_able( INT t );

/* The pointer, in the figure's coordinates, for the panel to show */
IMPORT BOOL	ft_have_xy;
IMPORT INT	ft_px, ft_py;
IMPORT void	ft_xy_words( UB *txt );		/* "x:N y:N", 32 bytes of room */

/* ---------------------------------------------------------------- a figure in a window */

struct t_ftview;

typedef struct {
	void	(*draw)( struct t_ftview *v );		/* the figure drawn again, not yet on the screen */
	void	(*show)( struct t_ftview *v );		/* what is drawn in the window put on the screen */
	void	(*show_part)( struct t_ftview *v, CONST T_DPRECT *r );	/* only r of it, in the window's pixels */
	void	(*text_begin)( struct t_ftview *v, T_TADNODE *doc );	/* a new piece of text edited */
	void	(*tools_shown)( void );			/* the panel shows the tool and the look again */
	void	(*pointer)( BOOL picking );		/* the pointer's shape for the tool in hand */
	UD	(*dbl_ns)( void );			/* a double press's longest gap */
	void	(*zoom_set)( struct t_ftview *v );	/* ft_look.zoom taken by the figure, scrolled home */
	void	(*text_look)( struct t_ftview *v, INT item );	/* the 書式 palette's item put on text */

	/*
	 * The canvas: a picture the record names, its pixels the host's
	 * until pic_done; one written beside the record under its name, as
	 * a PNG, so that it is drawn as it now is; the object the figure is
	 * the record of; and what could not be done told.
	 */
	ER	(*pic_get)( struct t_ftview *v, CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h );
	void	(*pic_done)( struct t_ftview *v, CONST UW *px );
	ER	(*pic_put)( struct t_ftview *v, CONST UB *href, CONST UW *px, INT w, INT h );
	BOOL	(*self_id)( struct t_ftview *v, TS_UUID *id );
	void	(*tell)( struct t_ftview *v, CONST char *msg );
} T_FTHOST;

typedef struct t_ftview {
	T_FGED		*ed;		/* the record, the model and the picks */
	INT		gid;		/* where the window is drawn */
	T_DPRECT	page;		/* the figure's place in the window, where presses are told */
	INT		sx, sy;		/* scrolled to, in the window's pixels */
	CONST T_FTHOST	*host;
	void		*arg;		/* the host's: which window */
	BOOL		paper_frame;	/* 用紙枠 shown over the figure */
} T_FTVIEW;

IMPORT BOOL ft_is( CONST T_FTVIEW *v );
IMPORT void ft_to_fig( CONST T_FTVIEW *v, INT x, INT y, INT *fx, INT *fy );
IMPORT void ft_fig_to_win( CONST T_FTVIEW *v, INT fx, INT fy, INT *x, INT *y );
IMPORT void ft_snap( INT *x, INT *y );
/* A place in a window's pixels on the grid: (ox, oy) where the figure's 0,0 is drawn, zoom in eighths */
IMPORT void ft_snap_win( INT ox, INT oy, INT zoom, INT *x, INT *y );
IMPORT BOOL ft_join_at( CONST T_FTVIEW *v, INT x, INT y, INT *p_place, INT *p_point,
			INT *p_x, INT *p_y );

/* Small things the palettes and the record are written with */
IMPORT INT  ft_put_num( UB *buf, INT at, INT v );
IMPORT void ft_colour_hex( UW col, UB *out );		/* "#rrggbb", 8 bytes */
IMPORT BOOL ft_hex_colour( CONST UB *s, UW *p_col );
IMPORT BOOL ft_same_word( CONST UB *a, CONST char *b );
IMPORT INT  ft_pattern_for( T_TAD *rec, UW col );	/* the pattern number a colour is written as */
IMPORT INT  ft_fill_pattern( T_TAD *rec );		/* the inside's, 0 for nothing */
IMPORT void ft_frame_put( T_TAD *rec, T_TADNODE *nd, INT l, INT t, INT r, INT b );	/* an arc's or an ellipse's box */
IMPORT void ft_join_put( T_TAD *rec, T_TADNODE *nd, CONST char *name, INT place, INT point );	/* a joined end */

/* ---------------------------------------------------------------- drawing a shape */

/*
 * A press with a drawing tool in hand (x, y the window's, as presses
 * are told): TRUE when it was taken. While a shape is drawn the hand's
 * moves are followed and its letting go ends it; Enter or Escape ends
 * a polygon or a polyline, Escape abandons any other. ft_drawing_arg is the arg of
 * the view it is drawn in, NULL when none.
 */
IMPORT BOOL ft_press( T_FTVIEW *v, INT x, INT y, UD when, UINT mods );
IMPORT void *ft_drawing_arg( void );
IMPORT void ft_follow( T_FTVIEW *v, INT x, INT y, UINT mods );
IMPORT void ft_release( T_FTVIEW *v );
IMPORT BOOL ft_key( T_FTVIEW *v, UINT code );		/* TRUE: taken */
IMPORT void ft_hover( T_FTVIEW *v, INT x, INT y );	/* where the pointer is, for the panel */
IMPORT void ft_drawing_stop( void );			/* whatever was being drawn let go */
IMPORT void ft_drawing_over( CONST T_FTVIEW *v );	/* its lines put over what the host drew last */
IMPORT void ft_select( void );				/* the tool given back to the pointer */
IMPORT void ft_take( INT t );				/* tool t taken, or its middle turned */
IMPORT void ft_make_text( T_FTVIEW *v, INT l, INT t, INT r, INT b );

/* The grid's points over the figure, when it is shown */
IMPORT void ft_grid_draw( CONST T_FTVIEW *v );

/*
 * What is drawn over the figure after the grid: 用紙枠 when the view
 * says, and the handles of 変形 in its window.
 */
IMPORT void ft_overlay( CONST T_FTVIEW *v );

/*
 * A picked line's end carried with the pointer in hand (x, y of the
 * window; slop the farther reach of a finger's press), joined where it
 * is let go near a shape's point. ft_end_arg is the window it is
 * carried in, NULL none.
 */
IMPORT BOOL ft_end_press( T_FTVIEW *v, INT x, INT y, INT slop );
IMPORT void *ft_end_arg( void );
IMPORT void ft_end_follow( T_FTVIEW *v, INT x, INT y );
IMPORT void ft_end_release( T_FTVIEW *v );

/*
 * 変形: the first picked shape that can be changed point by point --
 * a polygon's corners, an arc's ends and a sector's middle, a curve's
 * ends -- its handles carried with the pointer. ft_transform answers
 * FALSE when none of the picked can be. A press away from the handles
 * ends it (ft_transform_press FALSE), as do ft_transform_end and
 * ft_forget, which a window gone calls with its arg.
 */
IMPORT BOOL ft_transformable( CONST T_TVSHAPE *s );
IMPORT BOOL ft_transform( T_FTVIEW *v );
IMPORT BOOL ft_transform_on( CONST T_FTVIEW *v );
IMPORT void *ft_transform_arg( void );
IMPORT BOOL ft_transform_press( T_FTVIEW *v, INT x, INT y, INT slop );
IMPORT void ft_transform_end( T_FTVIEW *v );
IMPORT BOOL ft_transform_carrying( void );
IMPORT void ft_transform_follow( T_FTVIEW *v, INT x, INT y );
IMPORT void ft_transform_release( T_FTVIEW *v );
IMPORT void ft_forget( void *arg );

/*
 * キャンバス (figcanvas.c): ft_canvas_new makes a <pixelmap> of the box
 * drawn and paints in it at once. ft_canvas_press, with the canvas in
 * hand (fx, fy the figure's): in the picture being painted, paints;
 * outside it, painting ends; on another picture, painting in it begins;
 * FALSE when nothing of the canvas's was pressed, and when a picture is
 * being painted in another view, which its own ft_canvas_leave ends.
 * ft_canvas_open does the same for a picture pressed twice with the
 * pointer in hand, the canvas taken. The hand is followed in the
 * window's x, y. ft_canvas_leave writes what was painted and lets the
 * picture go; ft_canvas_forget lets it go unwritten, for a window gone.
 * ft_canvas_arg is the arg of the view painted in, NULL none.
 * ft_canvas_draw draws the picture being painted over the figure, laid
 * on paper. ft_burn is 焼き付け on the one picture picked.
 */
IMPORT void  ft_canvas_new( T_FTVIEW *v, INT l, INT t, INT r, INT b );
IMPORT BOOL  ft_canvas_press( T_FTVIEW *v, INT fx, INT fy );
IMPORT BOOL  ft_canvas_open( T_FTVIEW *v, INT fx, INT fy );
IMPORT void *ft_canvas_arg( void );
IMPORT BOOL  ft_canvas_painting( void );
IMPORT void  ft_canvas_follow( T_FTVIEW *v, INT x, INT y );
IMPORT void  ft_canvas_release( T_FTVIEW *v );
IMPORT void  ft_canvas_leave( T_FTVIEW *v );
IMPORT void  ft_canvas_forget( void *arg );
IMPORT void  ft_canvas_draw( CONST T_FTVIEW *v, UW paper );
IMPORT void  ft_burn( T_FTVIEW *v );

/* 画材: what the canvas paints with (ft_look.paint_tool) */
#define FT_PAINT_PENCIL	0		/* 鉛筆 */
#define FT_PAINT_BRUSH	1		/* 絵筆 */
#define FT_PAINT_ERASER	2		/* 消しゴム */
#define FT_PAINT_AIR	3		/* エアブラシ */
#define FT_PAINT_POUR	4		/* 絵の具 */
#define FT_PAINT_CUTTER	5		/* カッター */

/* 用紙枠設定: the paper as the paper panel asks it, and its answer (DE_PAPER_*) put ahead of the figure */
IMPORT void ft_paper_of( CONST T_FTVIEW *v, T_DEPAPER *ps );
IMPORT void ft_paper_put( T_FTVIEW *v, T_DEPAPER *ps, INT r );

/* 実寸パネル: the figure at its own size in gid over all, from where the view is scrolled to */
IMPORT void ft_realsize_draw( CONST T_FTVIEW *v, INT gid, CONST T_DPRECT *all, UW paper );

/* ---------------------------------------------------------------- the picked shapes */

#define FT_APPLY_FILL	1
#define FT_APPLY_STROKE	2
#define FT_APPLY_WIDTH	3
#define FT_APPLY_LTYPE	4
#define FT_APPLY_CONN	5
#define FT_APPLY_CORNER	6
#define FT_APPLY_ARROWS	7

IMPORT void ft_apply( T_FTVIEW *v, INT what );		/* what a palette set, on the picked shapes */
IMPORT BOOL ft_take_look( CONST T_FGED *e );		/* the look of the picked; TRUE: one had it */

/* ---------------------------------------------------------------- the palettes */

/*
 * A palette: rows, each a label and a control. What is changed in it
 * is handed to its changed() at once, by the item's number. The parts
 * of item i are numbered FT_PN_ITEM(i, k), k the choice or swatch.
 */
#define FT_PI_CHOICE	1		/* one of a row of words */
#define FT_PI_CHECK	2		/* on or off */
#define FT_PI_GRID	3		/* the fixed patterns, sixteen across */
#define FT_PI_TEXT	4		/* words typed: a number or a colour */
#define FT_PI_BUTTON	5		/* ends the palette with its number */
#define FT_PI_SWATCH	6		/* a row of colours to pick from */
#define FT_PI_COLOUR	7		/* the colour in hand, its code in 'text': pressed, another is chosen */

#define FT_PI_MAX	8
#define FT_POP_W	430
#define FT_ROW_H	26
#define FT_LABEL_W	118
#define FT_PART_FH	16		/* the letters of parts, as high as wide */

#define FT_PN_ITEM( i, k )	( ( ( i ) + 1 ) * 32 + ( k ) )

typedef struct {
	UINT		type;
	CONST char	*label;
	CONST char *CONST *names;	/* FT_PI_CHOICE */
	INT		n;
	INT		*value;		/* FT_PI_CHOICE, FT_PI_GRID, FT_PI_CHECK (0/1) */
	UB		*text;		/* FT_PI_TEXT, FT_PI_COLOUR */
	INT		max;
	CONST UW	*cols;		/* FT_PI_SWATCH */
	UW		*col;
	T_DPRECT	r;		/* where it was laid */
} T_FTITEM;

typedef struct {
	T_FTITEM	it[FT_PI_MAX];
	INT		n;
	void		(*changed)( INT item );
} T_FTPOP;

/* The palettes, by the panel's button they come up under */
#define FT_PAL_FILL	0
#define FT_PAL_STROKE	1
#define FT_PAL_LSTYLE	2
#define FT_PAL_CORNER	3
#define FT_PAL_PAINT	4		/* 画材, under the canvas button while it paints */
#define FT_PAL_ZOOM	6
#define FT_PAL_GRID	7
#define FT_PAL_FONT	18
#define FT_FILL_EDIT	3		/* the 塗りつぶし palette's item that ends it for パターン編集 */

/*
 * The palette of a button made, its items standing as the look is now;
 * what is changed in it is put on the picked shapes of the view set
 * with ft_pal_on (NULL none). FALSE: no such palette.
 */
IMPORT BOOL ft_palette( INT which, T_FTPOP *p );
IMPORT void ft_pal_on( T_FTVIEW *v );
IMPORT T_FTVIEW *ft_pal_view( void );

/* Laying out: the items' rectangles, a choice's cell, the height, a label's width */
IMPORT INT  ft_str_w( CONST char *s, INT size );
IMPORT void ft_pop_layout( T_FTPOP *p );
IMPORT INT  ft_pop_height( CONST T_FTPOP *p );
IMPORT INT  ft_choice_at( CONST T_FTITEM *it, INT k, T_DPRECT *c );
IMPORT INT  ft_item_h( CONST T_FTITEM *it );

/*
 * The fixed patterns of an FT_PI_GRID item drawn in the palette's
 * window, the chosen one marked; a press there choosing one: the item
 * whose value changed, or -1.
 */
IMPORT void ft_pop_grid_draw( INT gid, CONST T_FTPOP *p );
IMPORT INT  ft_pop_grid_press( T_FTPOP *p, INT x, INT y );

/* ---------------------------------------------------------------- パターン編集 (figpat.c) */

/*
 * The editor of a figure's pattern, dot by dot, apart from its window.
 * Its window's work area is FP_WW by FP_WH: the dots, large, from
 * (FP_GX, FP_GY), FP_CELL a dot; the number and the pattern at its own
 * size from FP_RX along the top; its panel of parts, FP_PNW by FP_PNH,
 * at (FP_PX, FP_PY). The parts are numbered as FP_N_* say: the colours
 * of fp_cols from FP_N_SW, a group; 透明; the tools from FP_N_TOOL, a
 * group; 左 上 下 右 from FP_N_MOVE; 右回 左回 左右 上下 from FP_N_TURN;
 * the switch 読込; 入替; 復旧.
 *
 * fp_load reads pattern id of the figure (its own, or the fixed one)
 * into the dots; fp_keep keeps them for 復旧. fp_act does what a
 * button of the panel does (FP_N_NONE, FP_N_MOVE.., FP_N_TURN..,
 * FP_N_UNDO). fp_press is a press in the work area: on the dots it
 * draws with the tool; while 読込 is on, on a pattern it reads that
 * one and turns 読込 off (TRUE). fp_drag is the hand carried with the
 * button down. fp_replace is 入替: the pattern put into the figure's
 * record under its number, through the figure editor's host; FALSE
 * when it could not be. fp_draw draws all but the ground and the panel.
 */
#define FP_CELL		12
#define FP_GX		10
#define FP_GY		10
#define FP_RX		( FP_GX + 16 * FP_CELL + 14 )
#define FP_PX		FP_RX
#define FP_PY		( FP_GY + 40 )
#define FP_PNW		180
#define FP_PNH		320
#define FP_WW		( FP_PX + FP_PNW + 8 )
#define FP_WH		( FP_PY + FP_PNH + 8 )

#define FP_NCOL		16
#define FP_N_SW		10		/* the colours, 10 up */
#define FP_N_NONE	30		/* 透明 */
#define FP_N_TOOL	31		/* the tools, 31 up: 鉛筆 長方形 スポイト 展開 */
#define FP_N_MOVE	40		/* 左 上 下 右 */
#define FP_N_TURN	44		/* 右回 左回 左右 上下 */
#define FP_N_LOAD	50		/* 読込, a switch */
#define FP_N_SWAP	51		/* 入替 */
#define FP_N_UNDO	52		/* 復旧 */

#define FP_T_PENCIL	0
#define FP_T_RECT	1
#define FP_T_PICK	2
#define FP_T_FILL	3

typedef struct {
	UW	px[16][16];		/* each dot's colour, or TAD_COL_NONE */
	UW	undo[16][16];
	UW	col;			/* the colour drawn in; TAD_COL_NONE rubs out */
	INT	tool;			/* FP_T_* */
	INT	id;			/* the pattern's number */
	BOOL	choosing;		/* 読込: the patterns laid out to choose */
	BOOL	down;			/* the first button held in the dots */
	INT	rx0, ry0;		/* a rectangle being drawn */
} T_FPED;

IMPORT CONST UW	fp_cols[FP_NCOL];

IMPORT void fp_new( T_FPED *p );
IMPORT void fp_load( T_FPED *p, CONST T_TVFIG *fig, INT id );
IMPORT void fp_keep( T_FPED *p );
IMPORT void fp_act( T_FPED *p, INT what );
IMPORT BOOL fp_press( T_FPED *p, CONST T_TVFIG *fig, INT x, INT y );
IMPORT void fp_drag( T_FPED *p, INT x, INT y );
IMPORT BOOL fp_replace( CONST T_FPED *p, T_FGED *e );
IMPORT void fp_draw( CONST T_FPED *p, INT gid, CONST T_TVFIG *fig );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FIGTOOL_H__ */
