/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cab.c
 *	The cabinet (design 17.5.1, phase 13)
 *
 *	One xmlTAD <figure> of <link> elements, shown in one window.
 *
 *	The content of the record is the truth about what links exist and
 *	what they say; the table here is the truth about where they are on
 *	the screen while the window is open. The two are made to agree at
 *	two moments only -- opening and saving -- and at no other, so that
 *	reading a cabinet never changes a stored byte.
 *
 *	Picking carries an order rather than a flag, because every
 *	operation on a set of entries has to happen in the order the user
 *	built that set: "line these up" starting from the first one picked
 *	is a different answer from starting anywhere else.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/cab.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/dp.h>
#include <ts/ob.h>
#include <ts/proc.h>

/* Where a link with no rectangle of its own is put */
#define CAB_AUTO_W	120		/* one entry wide */
#define CAB_AUTO_H	40		/* one entry high */
#define CAB_AUTO_GAP	8		/* between them */

#define OP_NONE		0
#define OP_MOVE		1		/* what is picked was moved */
#define OP_ARRANGE	2		/* what is picked was laid out again */
#define OP_DELETE	3		/* what is picked was taken out */
#define OP_ADD		4		/* entries were put in */
#define OP_SIZE		5		/* what is picked changed size */
#define OP_COLOUR	6		/* what is picked changed colour */

typedef struct {
	BOOL		used;
	T_VOBJ		v;		/* what the link says, as it was read */
	ID		vid;		/* in the register of om */
	T_DPRECT	r;		/* where it is in the work area */
	T_DPRECT	saved;		/* where the record says it is */
	INT		pick;		/* -1, else the order it was picked in */
	BOOL		dirty;
	BOOL		gone;		/* taken out; the record still has it until a save */
	BOOL		added;		/* put in here; the record has it only after a save */
	BOOL		changed;	/* something about it differs from the record */
} CABENT;

/*
 * What one operation took, so that it can be given back. Every operation
 * keeps, for each entry it touched, where that entry was and where it
 * now is. Undo puts back the first, redo puts back the second, and one
 * pair of calls serves every operation there will ever be: nothing here
 * has to know what a move or an arrangement means.
 *
 * The items are on the heap because a cabinet may hold 256 entries and
 * the stack holds 32 operations twice over; keeping room for the worst
 * case in static memory would cost a third of a megabyte for something
 * that is usually a handful of rectangles.
 */
/*
 * One entry as an operation found it and as it left it. A rectangle
 * covers moving, laying out and resizing; the four colours cover
 * recolouring. Keeping both in every item costs a few tens of bytes and
 * saves every operation from having a record shape of its own.
 */
typedef struct {
	INT		idx;
	T_DPRECT	was;
	T_DPRECT	now;
	UW		wascol[4];	/* frame, character, band, background */
	UW		nowcol[4];
} CABITEM;

typedef struct {
	UINT		op;
	INT		n;
	CABITEM		*item;
} CABUNDO;

LOCAL CABENT		cab_ent[CAB_MAX_ENT];
LOCAL INT		cab_n = 0;
LOCAL INT		cab_npick = 0;
LOCAL BOOL		cab_ready = FALSE;
LOCAL TS_UUID		cab_uuid;
LOCAL INT		cab_recno = 0;
LOCAL INT		cab_wid = 0;
LOCAL INT		cab_gid = 0;
LOCAL INT		cab_scroll_x = 0;	/* how far the view is wound up */
LOCAL INT		cab_scroll_y = 0;
LOCAL T_TAD		*cab_doc = NULL;
LOCAL T_CABSTAT		cab_count_st;

LOCAL CABUNDO		cab_undo_st[CAB_UNDO_MAX];
LOCAL INT		cab_undo_n = 0;		/* what can be taken back */
LOCAL CABUNDO		cab_redo_st[CAB_UNDO_MAX];
LOCAL INT		cab_redo_n = 0;

/* What the pointer is in the middle of doing */
LOCAL BOOL		cab_dragging = FALSE;
LOCAL INT		cab_drag_x = 0, cab_drag_y = 0;
LOCAL INT		cab_drag_dx = 0, cab_drag_dy = 0;
LOCAL UD		cab_last_when = 0;	/* when the last press was */
LOCAL INT		cab_last_x = 0, cab_last_y = 0;
LOCAL INT		cab_last_ent = -1;

LOCAL void undo_drop( CABUNDO *u );

/* ---------------------------------------------------------------- shapes */

LOCAL BOOL rect_empty( CONST T_DPRECT *r )
{
	return ( r->right <= r->left || r->bottom <= r->top );
}

LOCAL BOOL rect_has( CONST T_DPRECT *r, INT x, INT y )
{
	return ( x >= r->left && x < r->right && y >= r->top && y < r->bottom );
}

/*
 * An entry's rectangle is where it is in the cabinet, which is what the
 * document holds and what a save writes back. What the window shows is
 * that minus how far the view has been wound up. Keeping the two apart
 * is what lets a cabinet be larger than its window without the stored
 * places moving every time someone scrolls.
 */
LOCAL void to_work( CONST T_DPRECT *r, T_DPRECT *out )
{
	out->left   = r->left - cab_scroll_x;
	out->top    = r->top - cab_scroll_y;
	out->right  = r->right - cab_scroll_x;
	out->bottom = r->bottom - cab_scroll_y;
}

LOCAL void reg_of( CONST T_DPRECT *r, T_OMREG *reg )
{
	T_DPRECT	v;

	to_work(r, &v);
	reg->left   = v.left;
	reg->top    = v.top;
	reg->right  = v.right;
	reg->bottom = v.bottom;
	reg->wid    = (ID)cab_wid;
	reg->gid    = (ID)cab_gid;
	reg->image  = NULL;
	reg->imagesz = 0;
}

/*
 * Where to put a link that carries no rectangle. The specification lets
 * a link leave its place out, and a cabinet then decides: left to right,
 * wrapping at the width of the work area, in the order the links are in
 * the document. Deciding it here and writing it back on save is what
 * makes the placement stick.
 */
LOCAL void auto_place( INT i, T_DPRECT *r )
{
	T_WMWIN	w;
	INT	width = 640, cols, col, row;

	if ( wm_ref(cab_wid, &w) >= E_OK ) {
		width = w.work.right - w.work.left;
	}
	cols = ( width + CAB_AUTO_GAP ) / ( CAB_AUTO_W + CAB_AUTO_GAP );
	if ( cols < 1 ) {
		cols = 1;
	}
	col = i % cols;
	row = i / cols;
	r->left   = CAB_AUTO_GAP + col * ( CAB_AUTO_W + CAB_AUTO_GAP );
	r->top    = CAB_AUTO_GAP + row * ( CAB_AUTO_H + CAB_AUTO_GAP );
	r->right  = r->left + CAB_AUTO_W;
	r->bottom = r->top + CAB_AUTO_H;
}

/* ---------------------------------------------------------------- opening */

LOCAL void forget( void )
{
	INT	i;

	for ( i = 0; i < CAB_MAX_ENT; i++ ) {
		if ( cab_ent[i].used && cab_ent[i].vid > 0 ) {
			om_del_vob(cab_ent[i].vid);
		}
		cab_ent[i].used = FALSE;
		cab_ent[i].vid  = 0;
		cab_ent[i].pick = -1;
	}
	cab_n = 0;
	cab_npick = 0;
	while ( cab_undo_n > 0 ) {
		undo_drop(&cab_undo_st[--cab_undo_n]);
	}
	while ( cab_redo_n > 0 ) {
		undo_drop(&cab_redo_st[--cab_redo_n]);
	}
	cab_dragging = FALSE;
	cab_last_when = 0;
	cab_last_ent = -1;
	cab_scroll_x = 0;
	cab_scroll_y = 0;
	if ( cab_doc != NULL ) {
		tad_free(cab_doc);
		cab_doc = NULL;
	}
	cab_ready = FALSE;
}

EXPORT ER cab_open( CONST TS_UUID *uuid, INT recno, INT wid )
{
	INT		n, i;
	ER		er;

	if ( uuid == NULL || wid <= 0 ) {
		return E_PAR;
	}
	if ( cab_ready ) {
		return E_OBJ;			/* one cabinet at a time */
	}
	cab_gid = wm_gid(wid);
	if ( cab_gid < 0 ) {
		return (ER)cab_gid;
	}
	cab_wid = wid;

	/*
	 * The register of virtual objects belongs to the program, not to
	 * one cabinet, and starting it twice is not an error. Doing it
	 * here means a cabinet works whether or not something else in the
	 * program has already asked for it.
	 */
	er = om_ini();
	if ( er < E_OK ) {
		cab_wid = 0;
		return er;
	}

	er = om_rea_doc(uuid, recno, NULL, &cab_doc);	/* the library's own limits */
	if ( er < E_OK ) {
		cab_wid = 0;
		return er;
	}
	n = tad_lnk_count(cab_doc);
	if ( n > CAB_MAX_ENT ) {
		n = CAB_MAX_ENT;		/* the rest are not shown; see cab_count */
	}
	for ( i = 0; i < n; i++ ) {
		CABENT		*e = &cab_ent[i];
		T_OMREG		reg;
		ID		vid = 0;

		er = tad_lnk_get(cab_doc, i, &e->v);
		if ( er < E_OK ) {
			forget();
			return er;
		}
		e->r.left   = e->v.left;
		e->r.top    = e->v.top;
		e->r.right  = e->v.right;
		e->r.bottom = e->v.bottom;
		if ( rect_empty(&e->r) ) {
			auto_place(i, &e->r);
		}
		e->saved = e->r;
		e->pick  = -1;
		e->dirty = TRUE;
		e->gone  = FALSE;
		e->added = FALSE;
		e->changed = FALSE;
		e->used  = TRUE;

		reg_of(&e->r, &reg);
		er = om_reg_vob(&e->v, &reg, &vid);
		if ( er < E_OK ) {
			forget();
			return er;
		}
		e->vid = vid;
	}
	cab_n     = n;
	cab_uuid  = *uuid;
	cab_recno = recno;
	cab_ready = TRUE;

	return E_OK;
}

EXPORT ER cab_close( BOOL save )
{
	ER	er = E_OK;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( save ) {
		er = cab_save();
	}
	forget();
	cab_wid = 0;
	cab_gid = 0;

	return er;
}

/* ---------------------------------------------------------------- asking */

EXPORT INT cab_count( void )
{
	return ( cab_ready ) ? cab_n : 0;
}

EXPORT ER cab_ref( INT i, T_CABENT *out )
{
	CABENT	*e;

	if ( !cab_ready || out == NULL ) {
		return E_PAR;
	}
	if ( i < 0 || i >= cab_n || !cab_ent[i].used ) {
		return E_NOEXS;
	}
	e = &cab_ent[i];
	out->vobjid = e->v.vobjid;
	out->target = e->v.target;
	out->vid    = e->vid;
	out->r      = e->r;
	out->pick   = e->pick;
	out->hidden = e->v.hidden;
	out->fixed  = e->v.fixed;
	out->gone   = e->gone;
	out->added  = e->added;

	return E_OK;
}

EXPORT INT cab_at( INT x, INT y )
{
	INT	i;

	if ( !cab_ready ) {
		return -1;
	}
	/* the caller gives a place in the window; the entries are in the cabinet */
	x += cab_scroll_x;
	y += cab_scroll_y;
	/*
	 * From the end backwards: a link that comes later in the document
	 * is drawn later, so it is the one in front where two overlap.
	 */
	for ( i = cab_n - 1; i >= 0; i-- ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used || e->v.hidden || e->gone ) {
			continue;
		}
		if ( rect_has(&e->r, x, y) ) {
			return i;
		}
	}

	return -1;
}

/* ---------------------------------------------------------------- picking */

LOCAL void pick_clear( void )
{
	INT	i;

	for ( i = 0; i < cab_n; i++ ) {
		if ( cab_ent[i].pick >= 0 ) {
			cab_ent[i].pick = -1;
			cab_ent[i].dirty = TRUE;
		}
	}
	cab_npick = 0;
}

/*
 * Take an entry out of the picked set and close the gap its place left,
 * so that the orders stay 0..n-1 with nothing missing.
 */
LOCAL void pick_drop( INT i )
{
	INT	k, gone = cab_ent[i].pick;

	if ( gone < 0 ) {
		return;
	}
	cab_ent[i].pick = -1;
	cab_ent[i].dirty = TRUE;
	for ( k = 0; k < cab_n; k++ ) {
		if ( cab_ent[k].pick > gone ) {
			cab_ent[k].pick--;
		}
	}
	cab_npick--;
}

EXPORT ER cab_pick( INT i, UINT mode )
{
	CABENT	*e;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( i < 0 || i >= cab_n || !cab_ent[i].used ) {
		return E_NOEXS;
	}
	e = &cab_ent[i];
	if ( e->v.hidden || e->gone ) {
		return E_OBJ;			/* what is not shown is not picked */
	}
	switch ( mode ) {
	case CAB_SEL_ONE:
		pick_clear();
		e->pick = 0;
		e->dirty = TRUE;
		cab_npick = 1;
		break;
	case CAB_SEL_ADD:
		if ( e->pick < 0 ) {
			e->pick = cab_npick++;
			e->dirty = TRUE;
		}
		break;
	case CAB_SEL_TOGGLE:
		if ( e->pick >= 0 ) {
			pick_drop(i);
		} else {
			e->pick = cab_npick++;
			e->dirty = TRUE;
		}
		break;
	default:
		return E_PAR;
	}

	return E_OK;
}

EXPORT ER cab_pick_all( void )
{
	INT	i;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	pick_clear();
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used || e->v.hidden || e->gone ) {
			continue;
		}
		e->pick = cab_npick++;
		e->dirty = TRUE;
	}

	return E_OK;
}

EXPORT ER cab_pick_none( void )
{
	if ( !cab_ready ) {
		return E_OBJ;
	}
	pick_clear();

	return E_OK;
}

EXPORT INT cab_picked( void )
{
	return ( cab_ready ) ? cab_npick : 0;
}

/* ---------------------------------------------------------------- moving */

LOCAL void damage_rect( CONST T_DPRECT *r )
{
	T_DPRECT	v;

	if ( cab_wid > 0 && !rect_empty(r) ) {
		to_work(r, &v);			/* the window is told where it shows */
		wm_damage(cab_wid, &v);
	}
}

/* Put the entries that are picked into a list, in the order they were picked */
LOCAL INT pick_list( INT *idx )
{
	INT	i, n = 0, order;

	for ( order = 0; order < cab_npick; order++ ) {
		for ( i = 0; i < cab_n; i++ ) {
			if ( cab_ent[i].used && cab_ent[i].pick == order ) {
				idx[n++] = i;
				break;
			}
		}
	}

	return n;
}

LOCAL void undo_drop( CABUNDO *u )
{
	if ( u->item != NULL ) {
		Kfree(u->item);
		u->item = NULL;
	}
	u->n = 0;
	u->op = OP_NONE;
}

LOCAL void redo_clear( void )
{
	while ( cab_redo_n > 0 ) {
		undo_drop(&cab_redo_st[--cab_redo_n]);
	}
}

LOCAL void undo_push( CABUNDO *u )
{
	INT	i;

	if ( cab_undo_n == CAB_UNDO_MAX ) {
		/* the oldest goes: a stack that refuses new work is worse */
		undo_drop(&cab_undo_st[0]);
		for ( i = 1; i < CAB_UNDO_MAX; i++ ) {
			cab_undo_st[i - 1] = cab_undo_st[i];
		}
		cab_undo_n--;
	}
	cab_undo_st[cab_undo_n++] = *u;
	u->item = NULL;				/* the stack owns it now */
}

/* Put an entry where the record says, and tell the window what changed */
LOCAL void place( CABENT *e, CONST T_DPRECT *r )
{
	T_OMREG		reg;
	T_DPRECT	was = e->r;

	e->r = *r;
	e->dirty = TRUE;
	e->changed = TRUE;
	reg_of(&e->r, &reg);
	om_set_vob(e->vid, &reg);
	damage_rect(&was);
	damage_rect(&e->r);
}

/*
 * Start an operation: take room for the entries it will touch. The
 * caller fills in `was` and `now` per entry and pushes it, or drops it
 * when it turns out there was nothing to do.
 */
LOCAL ER undo_begin( CABUNDO *u, UINT op, INT max )
{
	u->op = op;
	u->n  = 0;
	u->item = NULL;
	if ( max <= 0 ) {
		return E_OK;
	}
	u->item = (CABITEM *)Kmalloc(sizeof(CABITEM) * (SZ)max);

	return ( u->item != NULL ) ? E_OK : E_NOMEM;
}

LOCAL void cols_of( CONST CABENT *e, UW *c )
{
	c[0] = e->v.frcol;
	c[1] = e->v.chcol;
	c[2] = e->v.tbcol;
	c[3] = e->v.bgcol;
}

LOCAL void cols_to( CABENT *e, CONST UW *c )
{
	e->v.frcol = c[0];
	e->v.chcol = c[1];
	e->v.tbcol = c[2];
	e->v.bgcol = c[3];
}

LOCAL void undo_add( CABUNDO *u, INT i, CONST T_DPRECT *was, CONST T_DPRECT *now )
{
	CABITEM	*it;

	if ( u->item == NULL ) {
		return;
	}
	it = &u->item[u->n];
	it->idx = i;
	it->was = *was;
	it->now = *now;
	cols_of(&cab_ent[i], it->wascol);
	cols_of(&cab_ent[i], it->nowcol);
	u->n++;
}

/*
 * Move what is picked. `push` says whether this is a whole operation or
 * one step of a drag: a drag moves things many times and has to be one
 * thing to take back, so its steps do not push and its end does.
 */
LOCAL ER move_picked( INT dx, INT dy, BOOL push )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( dx == 0 && dy == 0 ) {
		return E_OK;
	}
	all = pick_list(idx);
	er = undo_begin(&u, OP_MOVE, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT		*e = &cab_ent[idx[k]];
		T_DPRECT	was, to;

		if ( !e->used || e->v.fixed ) {
			continue;		/* what is fixed does not move */
		}
		was = e->r;
		to = was;
		to.left += dx;  to.right += dx;
		to.top += dy;   to.bottom += dy;
		place(e, &to);
		undo_add(&u, idx[k], &was, &to);
	}
	if ( u.n == 0 || !push ) {
		undo_drop(&u);			/* nothing moved, or a step of a drag */
		return E_OK;
	}
	undo_push(&u);
	redo_clear();				/* a new operation ends the redo line */

	return E_OK;
}

EXPORT ER cab_move( INT dx, INT dy )
{
	return move_picked(dx, dy, TRUE);
}

/* ---------------------------------------------------------------- arranging */

/*
 * Lay out what is picked. The entries are taken in reading order --
 * down the page first, then across -- and put in rows, each one keeping
 * its own size. Nothing about the layout is hard-coded here: the
 * spacing and the width come from what the caller asks for, because
 * those are a person's choice and not a property of a cabinet.
 *
 * The choices are passed as what they are, each in a field of its own,
 * rather than packed into bits of a stored word (design 17.6).
 */
EXPORT ER cab_arrange( CONST T_CABARR *a )
{
	CABUNDO		u;
	INT		idx[CAB_MAX_ENT], all, n = 0, k, j;
	INT		x, y, rowh = 0, gap, width;
	T_WMWIN		w;
	ER		er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( a == NULL ) {
		return E_PAR;
	}
	gap = ( a->gap > 0 ) ? a->gap : CAB_AUTO_GAP;
	width = a->width;
	if ( width <= 0 ) {
		width = ( wm_ref(cab_wid, &w) >= E_OK )
			? ( w.work.right - w.work.left ) : 640;
	}
	/* everything when nothing is picked: "tidy up" means the whole of it */
	if ( cab_npick > 0 ) {
		all = pick_list(idx);
	} else {
		all = 0;
		for ( k = 0; k < cab_n; k++ ) {
			if ( cab_ent[k].used && !cab_ent[k].v.hidden && !cab_ent[k].gone ) {
				idx[all++] = k;
			}
		}
	}
	/* in reading order: by top edge, then by left edge */
	for ( k = 1; k < all; k++ ) {
		INT		t = idx[k];
		CONST T_DPRECT	*rt = &cab_ent[t].r;

		for ( j = k; j > 0; j-- ) {
			CONST T_DPRECT *rp = &cab_ent[idx[j - 1]].r;

			if ( rp->top < rt->top
			  || ( rp->top == rt->top && rp->left <= rt->left ) ) {
				break;
			}
			idx[j] = idx[j - 1];
		}
		idx[j] = t;
	}
	er = undo_begin(&u, OP_ARRANGE, all);
	if ( er < E_OK ) {
		return er;
	}
	x = gap;
	y = gap;
	for ( k = 0; k < all; k++ ) {
		CABENT		*e = &cab_ent[idx[k]];
		T_DPRECT	was = e->r, to;
		INT		ew = e->r.right - e->r.left;
		INT		eh = e->r.bottom - e->r.top;

		if ( !e->used || e->v.fixed ) {
			continue;		/* a fixed entry stays where it is */
		}
		if ( x > gap && x + ew > width ) {
			x = gap;		/* it does not fit: start a new row */
			y += rowh + gap;
			rowh = 0;
		}
		to.left   = x;
		to.top    = y;
		to.right  = x + ew;
		to.bottom = y + eh;
		place(e, &to);
		undo_add(&u, idx[k], &was, &to);
		x += ew + gap;
		if ( eh > rowh ) {
			rowh = eh;
		}
		n++;
	}
	if ( n == 0 ) {
		undo_drop(&u);
		return E_OK;
	}
	undo_push(&u);
	redo_clear();

	return E_OK;
}

/* ---------------------------------------------------------------- removing */

/*
 * Take what is picked out of the cabinet. The links stay in the
 * document until a save, which is what lets this be taken back without
 * having read anything again, and what keeps the reference counts from
 * moving until the person says the change is real.
 */
EXPORT ER cab_delete( void )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	all = pick_list(idx);
	er = undo_begin(&u, OP_DELETE, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT *e = &cab_ent[idx[k]];

		if ( !e->used || e->v.fixed || e->gone ) {
			continue;		/* a fixed entry is not taken out */
		}
		e->gone = TRUE;
		e->dirty = TRUE;
		damage_rect(&e->r);
		undo_add(&u, idx[k], &e->r, &e->r);
	}
	if ( u.n == 0 ) {
		undo_drop(&u);
		return E_OK;
	}
	undo_push(&u);
	redo_clear();
	cab_pick_none();			/* what was picked is no longer there */

	return E_OK;
}

/* ---------------------------------------------------------------- undoing */

EXPORT INT cab_undo_depth( void )
{
	return cab_undo_n;
}

/*
 * Put every entry a record touched back to one of the two rectangles it
 * holds. Undo and redo are the same walk in opposite directions, and an
 * operation that takes entries out of the cabinet puts them back by the
 * same route: `gone` is part of what is restored.
 */
LOCAL void apply( CONST CABUNDO *u, BOOL forward )
{
	INT	k;

	for ( k = 0; k < u->n; k++ ) {
		CABENT		*e = &cab_ent[u->item[k].idx];
		CONST T_DPRECT	*r = forward ? &u->item[k].now : &u->item[k].was;

		if ( u->op == OP_COLOUR ) {
			cols_to(e, forward ? u->item[k].nowcol : u->item[k].wascol);
			om_upd_vob(e->vid, &e->v);
			e->dirty = TRUE;
			e->changed = TRUE;
			damage_rect(&e->r);
			continue;
		}
		if ( u->op == OP_DELETE || u->op == OP_ADD ) {
			/* putting in and taking out are the same thing read
			   in opposite directions */
			e->gone = ( u->op == OP_DELETE ) ? forward : !forward;
			e->dirty = TRUE;
			damage_rect(&e->r);
			continue;
		}
		place(e, r);
	}
}

EXPORT ER cab_undo( void )
{
	CABUNDO	u;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( cab_undo_n == 0 ) {
		return E_NOEXS;			/* nothing has been done yet */
	}
	u = cab_undo_st[--cab_undo_n];
	apply(&u, FALSE);
	if ( cab_redo_n < CAB_UNDO_MAX ) {
		cab_redo_st[cab_redo_n++] = u;
	} else {
		undo_drop(&u);
	}
	cab_count_st.undos++;

	return E_OK;
}

EXPORT ER cab_redo( void )
{
	CABUNDO	u;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( cab_redo_n == 0 ) {
		return E_NOEXS;
	}
	u = cab_redo_st[--cab_redo_n];
	apply(&u, TRUE);
	undo_push(&u);

	return E_OK;
}

/* ---------------------------------------------------------------- looks */

/*
 * Change a colour of what is picked. Which colour is named rather than
 * numbered, because a link carries four of them and a number would say
 * nothing about which.
 *
 * Choosing the colour is the caller's business: a panel of swatches is
 * a thing the desktop puts up, and a cabinet is not the place to decide
 * what that panel looks like (design 16.2.2).
 */
EXPORT ER cab_colour( UINT which, UW colour )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( which >= CAB_COL_MAX ) {
		return E_PAR;
	}
	all = pick_list(idx);
	er = undo_begin(&u, OP_COLOUR, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT	*e = &cab_ent[idx[k]];
		UW	*slot[CAB_COL_MAX];

		if ( !e->used || e->gone ) {
			continue;
		}
		undo_add(&u, idx[k], &e->r, &e->r);

		slot[CAB_COL_FRAME] = &e->v.frcol;
		slot[CAB_COL_CHAR]  = &e->v.chcol;
		slot[CAB_COL_BAND]  = &e->v.tbcol;
		slot[CAB_COL_BACK]  = &e->v.bgcol;
		*slot[which] = colour;

		cols_of(e, u.item[u.n - 1].nowcol);
		om_upd_vob(e->vid, &e->v);
		e->dirty = TRUE;
		e->changed = TRUE;
		damage_rect(&e->r);
	}
	if ( u.n == 0 ) {
		undo_drop(&u);
		return E_OK;
	}
	undo_push(&u);
	redo_clear();

	return E_OK;
}

/*
 * Change the size of what is picked. The corner that stays is the top
 * left, because that is the corner a cabinet lays out from, and a
 * rectangle is never allowed to close up to nothing.
 */
EXPORT ER cab_resize( INT dw, INT dh )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( dw == 0 && dh == 0 ) {
		return E_OK;
	}
	all = pick_list(idx);
	er = undo_begin(&u, OP_SIZE, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT		*e = &cab_ent[idx[k]];
		T_DPRECT	was, to;

		if ( !e->used || e->gone || e->v.fixed ) {
			continue;
		}
		was = e->r;
		to = was;
		to.right  += dw;
		to.bottom += dh;
		if ( to.right <= to.left + CAB_MIN_W ) {
			to.right = to.left + CAB_MIN_W;
		}
		if ( to.bottom <= to.top + CAB_MIN_H ) {
			to.bottom = to.top + CAB_MIN_H;
		}
		if ( to.right == was.right && to.bottom == was.bottom ) {
			continue;		/* already as small as it goes */
		}
		place(e, &to);
		e->v.height = to.bottom - to.top;
		om_upd_vob(e->vid, &e->v);
		undo_add(&u, idx[k], &was, &to);
	}
	if ( u.n == 0 ) {
		undo_drop(&u);
		return E_OK;
	}
	undo_push(&u);
	redo_clear();

	return E_OK;
}

/* ---------------------------------------------------------------- copying */

#define CAB_DUP_OFF	12		/* how far a copy sits from its original */

/*
 * Make a copy of everything that is picked. A copy is another link to
 * the same real object -- not a copy of the object -- which is what a
 * virtual object is for.
 *
 * The new links go into the document at the save, like every other
 * change. Their identities are made now, because an entry has to have
 * one to be an entry at all, and the reference count of what they point
 * at moves when the document is written (design 16.3.4).
 */
EXPORT ER cab_duplicate( void )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k, slot;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	all = pick_list(idx);
	if ( all == 0 ) {
		return E_OK;
	}
	er = undo_begin(&u, OP_ADD, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT		*src = &cab_ent[idx[k]];
		CABENT		*dst;
		T_OMREG		reg;
		ID		vid = 0;

		if ( !src->used ) {
			continue;
		}
		for ( slot = 0; slot < CAB_MAX_ENT; slot++ ) {
			if ( !cab_ent[slot].used ) {
				break;
			}
		}
		if ( slot == CAB_MAX_ENT ) {
			break;			/* no room: what was made stands */
		}
		dst = &cab_ent[slot];
		*dst = *src;
		er = ts_gen_uuid(&dst->v.vobjid);
		if ( er < E_OK ) {
			break;
		}
		dst->r.left   += CAB_DUP_OFF;
		dst->r.top    += CAB_DUP_OFF;
		dst->r.right  += CAB_DUP_OFF;
		dst->r.bottom += CAB_DUP_OFF;
		dst->saved = dst->r;
		dst->pick  = -1;
		dst->dirty = TRUE;
		dst->gone  = FALSE;
		dst->added = TRUE;
		dst->used  = TRUE;
		dst->vid   = 0;

		reg_of(&dst->r, &reg);
		er = om_reg_vob(&dst->v, &reg, &vid);
		if ( er < E_OK ) {
			dst->used = FALSE;
			break;
		}
		dst->vid = vid;
		if ( slot >= cab_n ) {
			cab_n = slot + 1;
		}
		damage_rect(&dst->r);
		undo_add(&u, slot, &dst->r, &dst->r);
	}
	if ( u.n == 0 ) {
		undo_drop(&u);
		return ( er < E_OK ) ? er : E_OK;
	}
	undo_push(&u);
	redo_clear();

	return E_OK;
}

/* ---------------------------------------------------------------- scrolling */

EXPORT ER cab_scroll( INT x, INT y )
{
	INT	i;
	T_DPRECT	all;
	T_WMWIN		w;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	if ( x == cab_scroll_x && y == cab_scroll_y ) {
		return E_OK;
	}
	cab_scroll_x = x;
	cab_scroll_y = y;
	wm_set_scroll(cab_wid, x, y);

	/*
	 * Every entry shows somewhere else now, so the register is told
	 * again and the whole work area is drawn afresh. Scrolling by
	 * moving the pixels that stayed is worth doing when there is
	 * something to measure; there is not yet (design 16.5.2).
	 */
	for ( i = 0; i < cab_n; i++ ) {
		CABENT		*e = &cab_ent[i];
		T_OMREG		reg;

		if ( !e->used ) {
			continue;
		}
		reg_of(&e->r, &reg);
		om_set_vob(e->vid, &reg);
		e->dirty = TRUE;
	}
	if ( wm_ref(cab_wid, &w) >= E_OK ) {
		all.left = 0;  all.top = 0;
		all.right = w.work.right - w.work.left;
		all.bottom = w.work.bottom - w.work.top;
		wm_damage(cab_wid, &all);
	}

	return E_OK;
}

EXPORT ER cab_scroll_of( INT *p_x, INT *p_y )
{
	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( p_x != NULL ) *p_x = cab_scroll_x;
	if ( p_y != NULL ) *p_y = cab_scroll_y;

	return E_OK;
}

/*
 * How much cabinet there is: the corner past the furthest entry. A
 * scroll bar needs it, and so does anything deciding whether scrolling
 * is possible at all.
 */
EXPORT ER cab_extent( INT *p_w, INT *p_h )
{
	INT	i, right = 0, bottom = 0;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used || e->v.hidden || e->gone ) {
			continue;
		}
		if ( e->r.right > right ) right = e->r.right;
		if ( e->r.bottom > bottom ) bottom = e->r.bottom;
	}
	if ( p_w != NULL ) *p_w = right;
	if ( p_h != NULL ) *p_h = bottom;

	return E_OK;
}

/* ---------------------------------------------------------------- commands */

#define CAB_NUDGE	8		/* how far a key moves what is picked */

EXPORT ER cab_command( UINT cmd )
{
	INT	i;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	switch ( cmd ) {
	case CAB_CMD_PICK_ALL:	return cab_pick_all();
	case CAB_CMD_PICK_NONE:	return cab_pick_none();
	case CAB_CMD_UNDO:	return cab_undo();
	case CAB_CMD_REDO:	return cab_redo();
	case CAB_CMD_SAVE:	return cab_save();
	case CAB_CMD_LEFT:	return move_picked(-CAB_NUDGE, 0, TRUE);
	case CAB_CMD_RIGHT:	return move_picked(CAB_NUDGE, 0, TRUE);
	case CAB_CMD_UP:	return move_picked(0, -CAB_NUDGE, TRUE);
	case CAB_CMD_DOWN:	return move_picked(0, CAB_NUDGE, TRUE);
	case CAB_CMD_DELETE:	return cab_delete();
	case CAB_CMD_DUPLICATE:	return cab_duplicate();
	case CAB_CMD_BIGGER:	return cab_resize(CAB_NUDGE, CAB_NUDGE);
	case CAB_CMD_SMALLER:	return cab_resize(-CAB_NUDGE, -CAB_NUDGE);
	case CAB_CMD_ARRANGE:	{
		T_CABARR a;

		a.width = 0;			/* the work area */
		a.gap = 0;			/* the cabinet's own spacing */

		return cab_arrange(&a);
	}
	case CAB_CMD_OPEN:
		/* the one picked first, which is why picking keeps an order */
		for ( i = 0; i < cab_n; i++ ) {
			if ( cab_ent[i].used && cab_ent[i].pick == 0 ) {
				return cab_open_ent(i);
			}
		}
		return E_NOEXS;
	default:
		return E_PAR;
	}
}

/*
 * Which command a key runs. The codes are what the keyboard reports
 * (USB HID usage identifiers), so this is the only place that knows
 * them; everything above works in commands.
 */
typedef struct {
	UINT	code;
	UINT	cmd;
} CABKEY;

LOCAL CONST CABKEY cab_key[] = {
	{ 0x04, CAB_CMD_PICK_ALL },	/* A */
	{ 0x1D, CAB_CMD_UNDO },		/* Z */
	{ 0x1C, CAB_CMD_REDO },		/* Y */
	{ 0x16, CAB_CMD_SAVE },		/* S */
	{ 0x28, CAB_CMD_OPEN },		/* Enter */
	{ 0x29, CAB_CMD_PICK_NONE },	/* Escape */
	{ 0x50, CAB_CMD_LEFT },
	{ 0x4F, CAB_CMD_RIGHT },
	{ 0x52, CAB_CMD_UP },
	{ 0x51, CAB_CMD_DOWN },
	{ 0x07, CAB_CMD_ARRANGE },	/* D */
	{ 0x4C, CAB_CMD_DELETE },	/* Delete */
	{ 0x06, CAB_CMD_DUPLICATE },	/* C */
	{ 0x2E, CAB_CMD_BIGGER },	/* = */
	{ 0x2D, CAB_CMD_SMALLER },	/* - */
};

LOCAL UINT cmd_of_key( UINT code )
{
	INT	i, n = (INT)( sizeof(cab_key) / sizeof(cab_key[0]) );

	for ( i = 0; i < n; i++ ) {
		if ( cab_key[i].code == code ) {
			return cab_key[i].cmd;
		}
	}

	return CAB_CMD_NONE;
}

/* ---------------------------------------------------------------- events */

#define CAB_DCLICK_NS	400000000U	/* two presses this close are one act */
#define CAB_DCLICK_PX	4		/* and no further apart than this */


LOCAL BOOL near_last( INT x, INT y )
{
	INT	dx = x - cab_last_x, dy = y - cab_last_y;

	if ( dx < 0 ) dx = -dx;
	if ( dy < 0 ) dy = -dy;

	return ( dx <= CAB_DCLICK_PX && dy <= CAB_DCLICK_PX );
}

LOCAL UINT pick_mode_of( UINT mods )
{
	if ( (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0 ) {
		return CAB_SEL_ADD;
	}
	if ( (mods & (HID_MOD_LCTRL | HID_MOD_RCTRL)) != 0 ) {
		return CAB_SEL_TOGGLE;
	}

	return CAB_SEL_ONE;
}

LOCAL ER on_press( CONST T_WMEV *ev )
{
	INT	i = cab_at(ev->x, ev->y);
	BOOL	twice;

	/* the drag is measured in the window, so the scroll does not enter it */

	twice = ( i >= 0 && i == cab_last_ent && near_last(ev->x, ev->y)
	       && cab_last_when != 0
	       && ev->when > cab_last_when
	       && (ev->when - cab_last_when) <= (UD)CAB_DCLICK_NS );

	cab_last_when = ev->when;
	cab_last_x    = ev->x;
	cab_last_y    = ev->y;
	cab_last_ent  = i;

	if ( i < 0 ) {
		cab_dragging = FALSE;
		return cab_pick_none();		/* pressing bare work area lets go */
	}
	if ( twice ) {
		cab_dragging = FALSE;
		cab_last_when = 0;		/* three presses are not two acts */
		return cab_open_ent(i);
	}
	/*
	 * Pressing something that is already picked keeps the whole picked
	 * set, so that a set can be dragged. Pressing something else picks
	 * that one, unless a modifier says to add or to toggle.
	 */
	if ( cab_ent[i].pick < 0 || pick_mode_of(ev->mods) != CAB_SEL_ONE ) {
		ER er = cab_pick(i, pick_mode_of(ev->mods));

		if ( er < E_OK ) {
			return er;
		}
	}
	cab_dragging = TRUE;
	cab_drag_x = ev->x;
	cab_drag_y = ev->y;
	cab_drag_dx = 0;
	cab_drag_dy = 0;

	return E_OK;
}

LOCAL ER on_move( CONST T_WMEV *ev )
{
	INT	dx, dy;

	if ( !cab_dragging ) {
		return E_OK;
	}
	dx = ev->x - cab_drag_x;
	dy = ev->y - cab_drag_y;
	if ( dx == 0 && dy == 0 ) {
		return E_OK;
	}
	cab_drag_x = ev->x;
	cab_drag_y = ev->y;
	cab_drag_dx += dx;
	cab_drag_dy += dy;

	return move_picked(dx, dy, FALSE);	/* the drag is one operation */
}

LOCAL ER on_release( CONST T_WMEV *ev )
{
	CABUNDO	u;
	INT	idx[CAB_MAX_ENT], all, k;
	ER	er;

	(void)ev;
	if ( !cab_dragging ) {
		return E_OK;
	}
	cab_dragging = FALSE;
	if ( cab_drag_dx == 0 && cab_drag_dy == 0 ) {
		return E_OK;			/* a press that did not move */
	}
	all = pick_list(idx);
	er = undo_begin(&u, OP_MOVE, all);
	if ( er < E_OK ) {
		return er;
	}
	for ( k = 0; k < all; k++ ) {
		CABENT		*e = &cab_ent[idx[k]];
		T_DPRECT	was;

		if ( !e->used || e->v.fixed ) {
			continue;
		}
		/* where it was before the drag began */
		was = e->r;
		was.left -= cab_drag_dx;  was.right -= cab_drag_dx;
		was.top -= cab_drag_dy;   was.bottom -= cab_drag_dy;
		undo_add(&u, idx[k], &was, &e->r);
	}
	if ( u.n == 0 ) {
		undo_drop(&u);
		return E_OK;
	}
	undo_push(&u);				/* the whole drag, in one record */
	redo_clear();

	return E_OK;
}

EXPORT ER cab_event( CONST T_WMEV *ev )
{
	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( ev == NULL ) {
		return E_PAR;
	}
	if ( ev->wid != cab_wid ) {
		return E_OK;			/* it belongs to another window */
	}
	switch ( ev->type ) {
	case HID_EV_BTN_DOWN:	return on_press(ev);
	case HID_EV_MOVE:	return on_move(ev);
	case HID_EV_BTN_UP:	return on_release(ev);
	case HID_EV_KEY_DOWN:	{
		UINT cmd = cmd_of_key(ev->code);

		return ( cmd == CAB_CMD_NONE ) ? E_OK : cab_command(cmd);
	}
	default:
		return E_OK;
	}
}

/* ---------------------------------------------------------------- writing */

EXPORT ER cab_save( void )
{
	INT	i;
	ER	er;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	/*
	 * What was taken out goes out of the document first. The count of
	 * what each link pointed at is settled by om_wri_doc in the same
	 * transaction as the write, so a cut power cannot leave a count
	 * disagreeing with the text (design 16.3.4).
	 */
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used ) {
			continue;
		}
		if ( e->added ) {
			/* made here and still here: it joins the document now */
			if ( !e->gone ) {
				e->v.left   = e->r.left;
				e->v.top    = e->r.top;
				e->v.right  = e->r.right;
				e->v.bottom = e->r.bottom;
				er = tad_lnk_add(cab_doc, &e->v, NULL);
				if ( er < E_OK ) {
					return er;
				}
				e->added = FALSE;
			}
			continue;		/* made and then taken back: nothing to do */
		}
		if ( e->gone ) {
			er = tad_lnk_del(cab_doc, &e->v.vobjid);
			if ( er < E_OK && er != E_NOEXS ) {
				return er;
			}
		}
	}
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used || e->gone || e->added ) {
			continue;
		}
		if ( !e->changed ) {
			continue;		/* untouched: its bytes stay as they were */
		}
		e->v.left   = e->r.left;
		e->v.top    = e->r.top;
		e->v.right  = e->r.right;
		e->v.bottom = e->r.bottom;
		er = tad_lnk_set(cab_doc, &e->v);
		if ( er < E_OK ) {
			return er;
		}
	}
	er = om_wri_doc(&cab_uuid, cab_recno, cab_doc);
	if ( er < E_OK ) {
		return er;
	}
	/*
	 * Now that the document says so, the entries that were taken out
	 * leave the table as well, and what can be taken back goes with
	 * them: undoing a delete after it was written would put an entry
	 * back that the document no longer has.
	 */
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( e->used && ( e->gone || e->added ) ) {
			if ( e->vid > 0 ) {
				om_del_vob(e->vid);
				e->vid = 0;
			}
			e->used = FALSE;
			e->pick = -1;
		}
		cab_ent[i].saved = cab_ent[i].r;
		cab_ent[i].changed = FALSE;
	}
	while ( cab_undo_n > 0 ) {
		undo_drop(&cab_undo_st[--cab_undo_n]);
	}
	redo_clear();
	cab_count_st.saves++;

	return E_OK;
}

/* ---------------------------------------------------------------- drawing */

EXPORT ER cab_draw( BOOL all )
{
	INT	i;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	for ( i = 0; i < cab_n; i++ ) {
		CABENT *e = &cab_ent[i];

		if ( !e->used || e->v.hidden || e->gone ) {
			continue;
		}
		if ( !all && !e->dirty ) {
			continue;
		}
		om_dsp_vob(cab_gid, e->vid);
		if ( e->pick >= 0 ) {
			om_sel_vob(cab_gid, e->vid, TRUE);
		}
		damage_rect(&e->r);
		e->dirty = FALSE;
		cab_count_st.entries_drawn++;
	}
	cab_count_st.draws++;

	return E_OK;
}

/* ---------------------------------------------------------------- opening */

EXPORT ER cab_open_ent( INT i )
{
	CABENT	*e;

	if ( !cab_ready ) {
		return E_OBJ;
	}
	if ( i < 0 || i >= cab_n || !cab_ent[i].used ) {
		return E_NOEXS;
	}
	e = &cab_ent[i];

	/*
	 * Which program opens it is written in the real object the link
	 * points at, so starting it is making a process object of it, and
	 * the cabinet does not decide anything (design 16.3.4, 18.8).
	 * Answers the process's ID.
	 */
	{
		T_OBCRE	c;
		TS_UUID	pu;
		ER	er;

		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_PROCESS;
		c.prog = e->v.target;
		er = ob_cre_obj(&c, &pu);
		return ( er < E_OK ) ? er : knl_prc_of_uuid(&pu);
	}
}

/* ---------------------------------------------------------------- checking */

EXPORT INT cab_self_check( void )
{
	INT	faults = 0;
	INT	i, order, seen;

	if ( !cab_ready ) {
		return 0;
	}
	/* the picked entries carry 0..npick-1, once each */
	for ( order = 0; order < cab_npick; order++ ) {
		seen = 0;
		for ( i = 0; i < cab_n; i++ ) {
			if ( cab_ent[i].used && cab_ent[i].pick == order ) {
				seen++;
			}
		}
		if ( seen != 1 ) {
			faults++;
		}
	}
	for ( i = 0; i < cab_n; i++ ) {
		CABENT		*e = &cab_ent[i];
		T_OMVOBJ	v;

		if ( !e->used || e->gone ) {
			continue;
		}
		if ( e->pick >= cab_npick ) {
			faults++;		/* an order outside the set */
		}
		if ( rect_empty(&e->r) ) {
			faults++;
		}
		if ( om_ref_vob(e->vid, &v) < E_OK ) {
			faults++;
			continue;
		}
		if ( v.reg.left != e->r.left || v.reg.top != e->r.top
		  || v.reg.right != e->r.right || v.reg.bottom != e->r.bottom ) {
			faults++;		/* the register says somewhere else */
		}
	}

	return faults;
}

EXPORT ER cab_stat( T_CABSTAT *st )
{
	if ( st == NULL ) {
		return E_PAR;
	}
	*st = cab_count_st;

	return E_OK;
}
