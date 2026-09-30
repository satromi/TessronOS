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

#define FN_MAX_FONT	8		/* faces open at once */

/*
 * What the letters are drawn as, on top of the face: thickened, leaned,
 * and made wider or narrower than they are tall. A face that has a bold
 * or an italic of its own is a face of its own; these are for the one
 * face a document names.
 */
#define FN_ST_BOLD	0x0001
#define FN_ST_ITALIC	0x0002

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
 * own, as a file is.
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
 * that is not open already; fn_nface and fn_face_at go through them;
 * fn_family says what one is called. fn_find answers the face a list of
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
 * Draw UTF-8 text with the left end of its baseline at (x, y), in the
 * coordinates of the drawing environment. Answers how far the pen
 * moved, so that a caller laying out a line does not have to measure
 * separately.
 */
IMPORT INT fn_draw( INT gid, ID fid, INT x, INT y, CONST UB *utf8, UW colour );

/* The same walk without drawing: how wide the text would be */
IMPORT INT fn_width( ID fid, CONST UB *utf8 );

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

/* What making and laying letters has cost so far, printed */
IMPORT void fn_report( void );

IMPORT INT knl_fn_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FN_H__ */
