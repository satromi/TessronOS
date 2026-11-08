/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fn.h
 *	Letters on the screen (design 16.6 stage 9c)
 *
 *	FreeType turns an outline into a pattern of pixels; this layer says
 *	which outline, how large, and where the result goes. There is no
 *	font manager between the two: FreeType is used directly, because
 *	what such a manager would add -- naming and caching faces -- is a
 *	few dozen lines here (design 16.2.4).
 *
 *	A font arrives either as a block of bytes the caller already has --
 *	a record read out of the store, say -- or as a path this layer
 *	reads from as it goes. The first lets a font come from anywhere;
 *	the second is what a font of a whole writing system needs, because
 *	holding tens of megabytes to use a few hundred kilobytes of them
 *	is memory nobody uses.
 *
 *	What is drawn today is black and white. Grey coverage needs the
 *	drawing layer to mix a new colour with what is already there, and
 *	it cannot read back from a window's own pixels yet (design 16.5.3).
 *	The shape of this interface does not change when that arrives.
 */

#ifndef __TS_FN_H__
#define __TS_FN_H__

#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/dp.h>

#define FN_MAX_FONT	16		/* faces open at once */

/*
 * What the letters are drawn as, on top of the face: thickened, leaned,
 * and made wider or narrower than they are tall. A face that has a bold
 * or an italic of its own is a face of its own; these are for the one
 * face a document names.
 */
#define FN_ST_BOLD	0x0001
#define FN_ST_ITALIC	0x0002

/*
 * The forms of the letters for a line written downwards: each letter is
 * taken in the form the face's table of substitutions ('vert' of GSUB)
 * gives for vertical writing -- the comma and the full stop in the upper
 * right of their box, the brackets, the long vowel and the dashes turned,
 * the small kana moved up and to the right. A letter the table does not
 * name is drawn as it is.
 */
#define FN_ST_VERT	0x0004

/* What a face measures, in pixels at the size it is set to */
typedef struct {
	INT	ascent;			/* above the baseline */
	INT	descent;		/* below it, as a positive number */
	INT	height;			/* one line to the next */
	INT	max_advance;
} T_FNMET;

/*
 * Open a face from bytes the caller owns. The bytes have to stay where
 * they are until the face is closed: FreeType reads from them as it
 * goes rather than taking a copy, which is what makes a large font
 * cheap to open.
 */
IMPORT ER  fn_open_mem( CONST UB *data, SZ size, INT index, ID *p_fid );
IMPORT ER  fn_close( ID fid );

/*
 * Open a face from a file, reading it as it is needed rather than all
 * at once. A font of a whole writing system is tens of megabytes and
 * only a few hundred kilobytes of it are ever touched; holding all of
 * it costs memory nobody uses, and on a machine that reads slowly it
 * costs the time as well.
 */
IMPORT ER  fn_open_file( CONST char *path, INT index, ID *p_fid );

/*
 * The same, from a record of a real object: a face kept as an object
 * (design 18.18). The record is read whole into pages of the face's
 * own, as a file is. The face remembers the object and the index it
 * came from (fn_of_obj).
 */
IMPORT ER  fn_open_obj( CONST TS_UUID *uuid, INT recno, INT index, ID *p_fid );

/* How large the letters are, in pixels from one line to the next */
IMPORT ER  fn_set_size( ID fid, INT px );

/*
 * The look the letters are drawn in, until it is set again: FN_ST_*,
 * and how wide they are in pixels when that is not their height (0 for
 * the same). Kept apart from the size so that setting the size alone,
 * as every caller that knows nothing of styles does, leaves it plain.
 */
IMPORT ER  fn_set_style( ID fid, UINT style, INT xpx );

/*
 * The letters turned clockwise by so many degrees, until set again;
 * setting the size sets it back upright. Each letter is turned in its
 * own box -- its advance across, the size down -- about the box's
 * middle, and takes the width of the turned box along the line, so that
 * the letters of a turned run stand side by side on the baseline as
 * they would upright. fn_extent says how far the letters' boxes, turned
 * as they are set, reach above and below the baseline.
 */
IMPORT ER  fn_set_angle( ID fid, INT deg );
IMPORT ER  fn_extent( ID fid, CONST UB *utf8, INT *p_above, INT *p_below );

/*
 * The faces there are. fn_open_dir opens every font file in a directory
 * that is not open already; fn_nface and fn_face_at go through those in
 * use; fn_family says what one is called (the family it is registered
 * as, else its own). fn_find answers the face a list of
 * names asks for -- '"Noto Serif JP", serif' -- the first name that is
 * a face that is open, a general name (serif, sans-serif, monospace,
 * 明朝, ゴシック) the first face of that kind, and the system's face when
 * nothing matches.
 */
IMPORT INT fn_open_dir( CONST char *dir );
IMPORT INT fn_nface( void );
IMPORT ID  fn_face_at( INT i );
IMPORT INT fn_family( ID fid, UB *buf, INT max );
IMPORT ID  fn_find( CONST UB *names );
IMPORT ER  fn_metrics( ID fid, T_FNMET *met );

/*
 * What a face is registered as (the 書体 of システム環境設定, design
 * 10.3): the family documents name it by ("" the face's own), its class
 * (the 16 bits that say what its letters look like: ornamented ends,
 * strokes that swell, Japanese in feeling...; 0 none said), what its
 * letters are (FN_FA_*), the family that draws the letters it has none
 * of ("" none), and whether it is used.
 *
 * A face in use is found by its family and drawn with. A face not in
 * use (FN_USE_OFF) stays open -- a window may still be drawing with it
 * -- but is never found again, by name or by kind, and is not among
 * fn_nface's. A face opened only to be looked at (FN_USE_TRIAL) is found
 * by its family alone, which is then a name nothing else has.
 *
 * FN_FA_BOLD, FN_FA_ITALIC, FN_FA_BAG and FN_FA_SHADOW say the face's
 * letters are drawn so already: asked for a family in one of these
 * looks, the face of the family that has it is taken, and the look is
 * not added to it again (fn_find_style). FN_FA_PROP and FN_FA_VERT are
 * what the face's data says -- its letters of different widths, its
 * forms for vertical writing -- and are not set by hand.
 */
#define FN_NAME_MAX	64

#define FN_FA_BOLD	0x0001		/* 太字 */
#define FN_FA_ITALIC	0x0002		/* 斜体 */
#define FN_FA_BAG	0x0004		/* 袋文字 */
#define FN_FA_SHADOW	0x0008		/* 影付文字 */
#define FN_FA_PROP	0x0010		/* 比例: of the data */
#define FN_FA_VERT	0x0020		/* 縦書き: of the data */
#define FN_FA_LOOK	( FN_FA_BOLD | FN_FA_ITALIC | FN_FA_BAG | FN_FA_SHADOW )

#define FN_USE_ON	0
#define FN_USE_OFF	1
#define FN_USE_TRIAL	2

typedef struct {
	UB	family[FN_NAME_MAX];
	UINT	cls;
	UINT	attr;			/* FN_FA_* */
	UB	base[FN_NAME_MAX];
	UINT	use;			/* FN_USE_* */
} T_FNREG;

IMPORT ER  fn_set_reg( ID fid, CONST T_FNREG *reg );
IMPORT ER  fn_get_reg( ID fid, T_FNREG *reg );

/* The face open from an object's face 'index' (fn_open_obj), in use or not, but not one opened to be looked at; 0 none */
IMPORT ID  fn_of_obj( CONST TS_UUID *uuid, INT index );

/*
 * fn_find for letters in a look (FN_FA_LOOK bits wanted): of the faces
 * of the family found, the one that is most nearly that look; *p_has
 * says which of the bits the face has of itself, which the caller then
 * does not add.
 */
IMPORT ID  fn_find_style( CONST UB *names, UINT want, UINT *p_has );

/*
 * Draw UTF-8 text with the left end of its baseline at (x, y), in the
 * coordinates of the drawing environment. Answers how far the pen
 * moved, so that a caller laying out a line does not have to measure
 * separately.
 */
IMPORT INT fn_draw( INT gid, ID fid, INT x, INT y, CONST UB *utf8, UW colour );

/* The same walk without drawing: how wide the text would be */
IMPORT INT fn_width( ID fid, CONST UB *utf8 );

/*
 * The same down a line written from the top: each letter stands in a box
 * as wide and as tall as the size, its middle on the line x = cx, the
 * first box's top at y, each box under the one before. A letter is
 * centred across the box by its advance and set in it as the face's em
 * square sets it (the em square's top is at the box's top). A face
 * turned (fn_set_angle) puts each letter down turned in its own box,
 * which then takes as much of the line as the turned box is tall: the
 * letters of a word turned by ninety degrees read downwards. Answers how
 * far down the pen moved; fn_vwidth answers that without drawing.
 * Letters in FN_ST_VERT whose face has no vertical form of their own --
 * the comma and full stop, the brackets, the long vowel, the dashes --
 * are moved to the upper right or turned by the layer itself.
 */
IMPORT INT fn_vdraw( INT gid, ID fid, INT cx, INT y, CONST UB *utf8, UW colour );
IMPORT INT fn_vwidth( ID fid, CONST UB *utf8 );

/* Whether a letter has a vertical form of its own in the face */
IMPORT BOOL fn_has_vert( ID fid, UW cp );

/*
 * A table of the face's file by its tag (four letters, the first the
 * highest byte), copied into buf: what a file of pages needs to carry
 * the face with it (design 11.22). With buf NULL only the length is
 * said; *p_len is the room in buf, and the length on return.
 */
IMPORT ER  fn_sfnt_table( ID fid, UW tag, UB *buf, SZ *p_len );

/*
 * How many bytes of a UTF-8 string fit in a given width. What a text
 * box needs to know where to break, and what a name in a frame needs to
 * know where to stop.
 */
IMPORT INT fn_fit( ID fid, CONST UB *utf8, INT width );

/*
 * The font the system draws with: window titles, the names of virtual
 * objects, the labels of parts. Whoever has the bytes -- the desktop,
 * usually -- opens a face and says so here; everything that draws a
 * word asks for it. Answers 0 when there is none, and what draws words
 * then draws nothing rather than something the caller did not ask for.
 */
IMPORT ER  fn_set_system( ID fid );

/*
 * The layer held by one task across several calls, and what a face is
 * set to kept and put back: for drawing on another's behalf -- a
 * process's letters -- without leaving the face as the other's calls
 * left it for the next of the kernel's own.
 */
typedef struct {
	INT	px;
	UINT	style;
	INT	xpx;
	INT	angle;
} T_FNSTATE;

IMPORT void fn_hold( void );
IMPORT void fn_release( void );
IMPORT ER   fn_state_get( ID fid, T_FNSTATE *st );
IMPORT ER   fn_state_set( ID fid, CONST T_FNSTATE *st );
IMPORT ID  fn_system( void );

/*
 * Letters drawn or measured by a program running as a process, with
 * the kernel's faces (TSN_FN_DRA_TXT): the face set as st says while it
 * is done -- st.px 0 the face as it is -- and put back after, all while
 * the layer is held, so that the kernel's own drawing never finds a
 * face as a process set it. What comes back is what the call of the
 * kernel that op names answers: fn_draw, fn_width, fn_vdraw,
 * fn_vwidth, fn_extent (above, below), fn_fit (width), fn_metrics
 * (met), fn_has_vert (cp). The words are len bytes of UTF-8 at text,
 * taken into the kernel first. A process calls the fn_ functions as
 * the kernel does; lib/libtv/tv_fn.c keeps what they set and makes this
 * call.
 */
#define FN_OP_DRAW	1
#define FN_OP_WIDTH	2
#define FN_OP_VDRAW	3
#define FN_OP_VWIDTH	4
#define FN_OP_EXTENT	5
#define FN_OP_FIT	6
#define FN_OP_METRICS	7
#define FN_OP_HAS_VERT	8

typedef struct {
	UINT		op;		/* FN_OP_* */
	INT		gid;		/* where FN_OP_DRAW and FN_OP_VDRAW draw */
	ID		fid;
	INT		x, y;		/* where they draw */
	CONST UB	*text;
	INT		len;
	UW		colour;
	INT		width;		/* FN_OP_FIT */
	UW		cp;		/* FN_OP_HAS_VERT */
	T_FNSTATE	st;
	INT		above, below;	/* FN_OP_EXTENT, out */
	T_FNMET		met;		/* FN_OP_METRICS, out */
} T_FNTXT;

IMPORT INT fn_dra_txt( T_FNTXT *pk );

/* What making and laying letters has cost so far, printed */
IMPORT void fn_report( void );

IMPORT INT knl_fn_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FN_H__ */
