/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cab.h
 *	The cabinet: a window onto the virtual objects of one real object
 *	(design 17.5.1, phase 13)
 *
 *	A cabinet is one xmlTAD <figure> whose children are <link>
 *	elements. Opening it turns each link into an entry that knows where
 *	it is on the screen; the entries are what the user picks up, moves
 *	and opens, and writing the cabinet puts their places back into the
 *	links.
 *
 *	Three rules hold this together, and each is checked by
 *	cab_self_check rather than only believed.
 *
 *	Picking is an order, not a flag. An entry carries the position it
 *	was picked in, so an operation on the picked set happens in the
 *	order the user picked them, and "the first one" means something.
 *
 *	Nothing is written until it is asked for. Drawing a cabinet never
 *	touches the file system: what the screen shows and what the record
 *	holds are made to agree by cab_save and by nothing else.
 *
 *	Undo is a stack. Every operation that changes where things are
 *	pushes what it took, so any number of them can be taken back in
 *	order, and taking one back pushes its inverse for redo.
 */

#ifndef __TS_CAB_H__
#define __TS_CAB_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/tad.h>
#include <ts/dp.h>
#include <ts/wm.h>

#define CAB_MAX_ENT	256		/* links one cabinet shows */
#define CAB_UNDO_MAX	32		/* operations that can be taken back */

/* How a pick adds to what is already picked */
#define CAB_SEL_ONE	0		/* this one alone */
#define CAB_SEL_ADD	1		/* this one as well */
#define CAB_SEL_TOGGLE	2		/* this one in or out */

/* What one entry is, for a caller that asks */
typedef struct {
	TS_UUID		vobjid;
	TS_UUID		target;
	ID		vid;		/* in the register of om, 0 when not in it */
	T_DPRECT	r;		/* where it is in the work area */
	INT		pick;		/* -1 not picked, else the order it was picked */
	BOOL		hidden;
	BOOL		fixed;
	BOOL		gone;		/* taken out, and not yet written */
	BOOL		added;		/* put in here, and not yet written */
} T_CABENT;

/*
 * Open a cabinet into a window and close it again. The window is the
 * caller's: the cabinet draws into it and does not own it.
 *
 * cab_close writes first when `save` is true; when it is false what was
 * changed is dropped, which is what answering "no" to the question is.
 */
IMPORT ER  cab_open( CONST TS_UUID *uuid, INT recno, INT wid );
IMPORT ER  cab_close( BOOL save );

/*
 * How many entries the table holds. An entry that was taken out and
 * written leaves a hole, so this is the size of the table and not the
 * number of things on the screen: cab_ref answers E_NOEXS for a hole.
 */
IMPORT INT cab_count( void );
IMPORT ER  cab_ref( INT i, T_CABENT *e );

/*
 * Which entry is at a place in the work area: the one nearest the front,
 * skipping the hidden ones. -1 when the place is bare.
 */
IMPORT INT cab_at( INT x, INT y );

IMPORT ER  cab_pick( INT i, UINT mode );
IMPORT ER  cab_pick_all( void );
IMPORT ER  cab_pick_none( void );
IMPORT INT cab_picked( void );

/*
 * Move everything that is picked. An entry marked fixed does not move,
 * and moving nothing is not an error: it is what a cabinet of fixed
 * entries does.
 */
IMPORT ER  cab_move( INT dx, INT dy );

/*
 * How to lay entries out. Each choice is a field of its own rather than
 * a pair of bits packed into a fusen of the real object, and the caller
 * decides where they come from.
 *
 * `width` of 0 means the width of the work area, and `gap` of 0 means
 * the cabinet's own spacing.
 */
typedef struct {
	INT	width;			/* where a row wraps */
	INT	gap;			/* between entries, and around the edge */
} T_CABARR;

/*
 * Lay out what is picked, in reading order. With nothing picked it lays
 * out the whole cabinet, because "tidy this up" with no selection means
 * all of it.
 */
IMPORT ER  cab_arrange( CONST T_CABARR *a );

/*
 * Take what is picked out. The links stay in the document until a save,
 * so this can be taken back with no reading, and the reference counts
 * do not move until the change is written.
 */
IMPORT ER  cab_delete( void );

/* Which of a link's four colours a call means */
#define CAB_COL_FRAME	0
#define CAB_COL_CHAR	1
#define CAB_COL_BAND	2
#define CAB_COL_BACK	3
#define CAB_COL_MAX	4

/* How small a virtual object is allowed to become */
#define CAB_MIN_W	16
#define CAB_MIN_H	16

/*
 * Change a colour, or the size, of what is picked. Choosing the colour
 * is the caller's business: a panel of swatches belongs to the desktop,
 * not to a cabinet.
 */
IMPORT ER  cab_colour( UINT which, UW colour );
IMPORT ER  cab_resize( INT dw, INT dh );

/*
 * Copy what is picked. A copy is another link to the same real object,
 * not a copy of that object. The new links reach the document at the
 * save, and the reference count of what they point at moves then.
 */
IMPORT ER  cab_duplicate( void );

/* ---------------------------------------------------------------- the view */

/*
 * How far the view is wound up. An entry's rectangle is where it is in
 * the cabinet -- what the document holds -- and the window shows that
 * minus this. Scrolling therefore moves nothing and writes nothing.
 */
IMPORT ER  cab_scroll( INT x, INT y );
IMPORT ER  cab_scroll_of( INT *p_x, INT *p_y );

/* How much cabinet there is: past the furthest entry. For a scroll bar. */
IMPORT ER  cab_extent( INT *p_w, INT *p_h );

/* ---------------------------------------------------------------- commands */

/*
 * What a cabinet can be told to do. A command key and a menu item are
 * two doors into this one table, so the two cannot drift apart: a
 * command key never does something a menu item does not.
 */
#define CAB_CMD_NONE		0
#define CAB_CMD_PICK_ALL	1
#define CAB_CMD_PICK_NONE	2
#define CAB_CMD_UNDO		3
#define CAB_CMD_REDO		4
#define CAB_CMD_SAVE		5
#define CAB_CMD_OPEN		6	/* open what is picked */
#define CAB_CMD_LEFT		7	/* nudge what is picked */
#define CAB_CMD_RIGHT		8
#define CAB_CMD_UP		9
#define CAB_CMD_DOWN		10
#define CAB_CMD_ARRANGE		11
#define CAB_CMD_DELETE		12
#define CAB_CMD_DUPLICATE	13
#define CAB_CMD_BIGGER		14
#define CAB_CMD_SMALLER		15
#define CAB_CMD_MAX		16

IMPORT ER  cab_command( UINT cmd );

/*
 * Hand the cabinet something that happened in its window. Pressing on a
 * virtual object picks it up and starts a drag; pressing twice on one
 * opens it; pressing on bare work area lets everything go. A key runs
 * whatever command the table gives it.
 *
 * A whole drag is one operation: it can be taken back in one go, not a
 * step at a time.
 */
IMPORT ER  cab_event( CONST T_WMEV *ev );

/* Take back the last operation, and put it back again */
IMPORT ER  cab_undo( void );
IMPORT ER  cab_redo( void );
IMPORT INT cab_undo_depth( void );

/* Write the cabinet back. The places of the entries go into the links. */
IMPORT ER  cab_save( void );

/*
 * Draw what has changed since the last time, and say so to the window.
 * Passing TRUE draws all of it, which is what a window that was covered
 * and came back needs.
 */
IMPORT ER  cab_draw( BOOL all );

/*
 * Open an entry: start the program that can deal with what the link
 * points at. Which program that is comes from the real object, not from
 * the cabinet (design 16.3.4).
 */
IMPORT ER  cab_open_ent( INT i );

/*
 * Check that what is kept still makes sense: the picked entries carry
 * 0..n-1 with no gaps or repeats, every entry that is drawn is in the
 * register of om with the same rectangle, and no rectangle is empty.
 * Answers how many faults were found, so a test can require zero.
 */
IMPORT INT cab_self_check( void );

typedef struct {
	UD	draws;			/* times anything was drawn */
	UD	entries_drawn;
	UD	saves;
	UD	undos;
} T_CABSTAT;

IMPORT ER  cab_stat( T_CABSTAT *st );

#ifdef __cplusplus
}
#endif

#endif /* __TS_CAB_H__ */
