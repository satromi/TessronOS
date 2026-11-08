/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtedit.c
 *	The virtual objects in a figure: taken, carried, sized, thrown
 *	away, copied, and taken back (design 16.5.12, 17.9)
 *
 *	A cabinet is a figure whose shapes are mostly virtual objects, so
 *	what is done here is what the cabinet does and what the figure
 *	editor does to the virtual objects in any figure.
 *
 *	  a press on one		takes it, and only it
 *	  a press with Shift		adds it to what is taken, or leaves it
 *	  a press on the page		lets everything go and draws a band;
 *					what the band touches is taken, or
 *					with Shift, changes whether it is
 *	  carrying the middle		moves everything that is taken
 *	  carrying an edge		sizes the one pressed: the right edge
 *					its width, the foot its height, the
 *					corner both
 *	  two presses			opens it in a window of its own
 *	  two presses, and carry	the object is copied, and a link to
 *					the copy put where the hand lets go
 *	  carrying into another window	moves them there: out of this
 *					record and into that one
 *
 *	Every change is made to the record and the model is made again
 *	from it. Before each change the record is kept as text, so taking
 *	a change back is putting that text back.
 *
 *	While something is being carried only its outline follows the
 *	hand, laid by the window system over every window, so that it can
 *	be carried out of one window and into another. The page does not
 *	change until the hand lets go, and drawing it again for every step
 *	of the hand would be slower than the hand.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/part.h>
#include <ts/fn.h>
#include <ts/tray.h>
#include <ts/figedit.h>
#include <ts/figtool.h>
#include "desktop.h"

#define ED_EDGE		5		/* how near an edge counts as on it */
#define ED_MIN		8		/* the least a box may be sized to */
#define ED_STEP		16		/* how far each paste is set off */
#define ED_NEAR		DT_DBL_NEAR	/* how far apart two presses may be */
#define ED_PICK_COL	0x00FF8F00U	/* the outline of what is taken */
#define ED_HANDLE	8		/* the handle a picked shape is sized by */

#define HOLD_NONE	0
#define HOLD_MOVE	1
#define HOLD_W		2
#define HOLD_H		3
#define HOLD_WH		4
#define HOLD_BAND	5		/* drawing a band round what to take */
#define HOLD_TWICE	6		/* pressed twice: opens, unless carried */
#define HOLD_DUP	7		/* pressed twice and carried: a copy */

LOCAL INT	ed_pasted = 0;		/* how many times since it was filled */

/* What the hand is holding */
LOCAL DTWIN	*hold_d = NULL;
LOCAL UINT	hold_how = HOLD_NONE;
LOCAL INT	hold_which = -1;	/* the shape pressed */
LOCAL T_DPRECT	hold_box;		/* that shape's box as it is sized */
LOCAL T_DPRECT	hold_was;		/* its box as the handle was taken */
LOCAL INT	hold_cx, hold_cy;	/* where the handle is carried, in the window */
LOCAL UINT	hold_flip;		/* turned over by being sized past an edge (FE_SIZE_FLIP_*) */
LOCAL INT	hold_dx = 0, hold_dy = 0;	/* how far everything has come */
LOCAL INT	hold_x0, hold_y0;	/* where the hand went down */
LOCAL INT	hold_px, hold_py;	/* where it was last */
LOCAL BOOL	hold_moved = FALSE;
LOCAL BOOL	hold_add = FALSE;	/* Shift was down: the band toggles */
LOCAL T_VOBJ	hold_v;			/* the link pressed twice */
LOCAL BOOL	band_shown = FALSE;

/* The last press, for telling a double press from two presses */
LOCAL INT	click_x = -1000, click_y = -1000;
LOCAL UD	click_at = 0;

/* ---------------------------------------------------------------- the model */

EXPORT void ed_forget( DTWIN *d )
{
	if ( d->doc != NULL ) {
		tv_doc_free(d->doc);
		d->doc = NULL;
	}
	if ( d->fig != NULL ) {
		tv_fig_free(d->fig);
		d->fig = NULL;
	}
}

/*
 * The model made again from the record. Picks past the end of what is
 * there now are let go: a shape that was thrown away cannot still be
 * taken.
 */
EXPORT void ed_model( DTWIN *d )
{
	INT	i;
	T_TVDOC	*was = d->doc;

	/* the document made before: what of its layout still holds is taken over */
	d->doc = NULL;
	ed_forget(d);
	if ( d->rec == NULL || d->tb_host != NULL ) {
		if ( was != NULL ) {
			tv_doc_free(was);
		}
		if ( d->rec != NULL ) {
			/* a piece of text in a figure: its own <document> element */
			(void)tv_doc_of(d->tb_node, &d->doc);
		}
		return;
	}
	if ( d->kind == TV_KIND_DOC ) {
		/* in 原稿, the record's text is what is shown and edited */
		tv_doc_again(( d->xml_view && d->xrec != NULL ) ? d->xrec : d->rec,
			     was, &d->doc);
		/*
		 * The paragraphs laid out are kept: what is drawn and asked
		 * about lays only what it wants and what has changed.
		 */
		if ( d->doc != NULL && tv_doc_cache(d->doc, was) >= E_OK ) {
			d->settling = TRUE;
		}
		if ( d->doc != NULL && !d->xml_view ) {
			d->doc->detail = d->detail_view;
			d->doc->show_hidden = d->show_hidden;
			d->doc->page_h = !d->paper_frame ? 0
				       : ( d->doc->paper_h > 0 ) ? d->doc->paper_h
				       : DT_PAPER_H;
			if ( d->paper_frame && d->doc->vert ) {
				/* 縦書き: the pages follow one another across */
				d->doc->page_h = ( d->doc->paper_w > 0 ) ? d->doc->paper_w
					       : DT_PAPER_W;
			}
		}
	} else {
		tv_fig(d->rec, &d->fig);
		/* the figure editor shows it magnified as the panel says */
		if ( d->fig != NULL && df_is(d) ) {
			d->fig->zoom = df_zoom();
		}
		if ( d->fig != NULL ) {
			d->fig->show_hidden = d->show_hidden;
		}
	}
	if ( was != NULL ) {
		tv_doc_free(was);
	}
	/* a pick for every shape there is */
	if ( d->fig != NULL && d->fig->nsh > d->maxpick ) {
		INT	m = d->fig->nsh + d->fig->nsh / 2 + 16;
		UINT	*p = (UINT *)Krealloc(d->pick, sizeof(UINT) * (size_t)m);

		if ( p == NULL ) {
			/* not shown as a figure that cannot be picked in */
			tv_fig_free(d->fig);
			d->fig = NULL;
			dt_no_memory(d);
		} else {
			knl_memset(p + d->maxpick, 0, sizeof(UINT) * (SZ)( m - d->maxpick ));
			d->pick = p;
			d->maxpick = m;
		}
	}
	d->npick = 0;
	for ( i = 0; i < d->maxpick; i++ ) {
		if ( d->pick[i] != 0 ) {
			if ( d->fig == NULL || i >= d->fig->nsh ) {
				d->pick[i] = 0;
			} else {
				d->npick++;
			}
		}
	}
}

/*
 * A figure's lengths on the screen, and back: magnified as the figure
 * editor shows the figure. A cabinet is shown at life size.
 */
LOCAL INT fig_zoom( CONST DTWIN *d )
{
	return ( d->fig != NULL && d->fig->zoom > 0 ) ? d->fig->zoom : 8;
}

LOCAL INT fz( CONST DTWIN *d, INT v )
{
	return v * fig_zoom(d) / 8;
}

LOCAL INT unz( CONST DTWIN *d, INT v )
{
	return v * 8 / fig_zoom(d);
}

/* Where shape i is drawn, in the window's own coordinates */
LOCAL BOOL shape_box( CONST DTWIN *d, INT i, T_DPRECT *b )
{
	T_DPRECT	page, r;
	INT		ox, oy;

	if ( d->fig == NULL || i < 0 || i >= d->fig->nsh ) {
		return FALSE;
	}
	dt_work_rect(d, &page);
	ox = page.left - d->scroll_x;
	oy = page.top - d->scroll_y;
	r = d->fig->sh[i].r;
	if ( d->fig->sh[i].kind == TV_SH_LINE
	  && ( d->fig->sh[i].c0_shape >= 0 || d->fig->sh[i].c1_shape >= 0 ) ) {
		/* a line joined to shapes is where it is drawn: its ends at those shapes, bent or curved */
		INT		max = TV_OUT_PTS(d->fig->sh[i].npt);
		T_DPPOINT	*pts = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * (SZ)max);
		INT		n = ( pts != NULL ) ? tv_line_path(d->fig, &d->fig->sh[i], pts, max) : 0, k;

		for ( k = 0; k < n; k++ ) {
			if ( k == 0 || pts[k].x < r.left ) r.left = pts[k].x;
			if ( k == 0 || pts[k].y < r.top ) r.top = pts[k].y;
			if ( k == 0 || pts[k].x + 1 > r.right ) r.right = pts[k].x + 1;
			if ( k == 0 || pts[k].y + 1 > r.bottom ) r.bottom = pts[k].y + 1;
		}
		if ( pts != NULL ) {
			Kfree(pts);
		}
	}
	b->left   = fz(d, r.left) + ox;
	b->top    = fz(d, r.top) + oy;
	b->right  = fz(d, r.right) + ox;
	b->bottom = fz(d, r.bottom) + oy;

	return TRUE;
}

LOCAL BOOL is_link( CONST DTWIN *d, INT i )
{
	return (BOOL)( d->fig != NULL && i >= 0 && i < d->fig->nsh
		    && d->fig->sh[i].kind == TV_SH_LINK
		    && d->fig->sh[i].link >= 0 );
}

/* What can be taken: a link, and in the figure editor any shape */
LOCAL BOOL takeable( CONST DTWIN *d, INT i )
{
	return (BOOL)( is_link(d, i) || df_takeable(d, i) );
}

/*
 * The least height shape i may be sized to, on the screen: a link is
 * never lower than it is closed, whatever the size of its letters; any
 * other shape, ED_MIN.
 */
LOCAL INT least_h( CONST DTWIN *d, INT i )
{
	T_VOBJ	v;
	INT	h;

	if ( !is_link(d, i) || tad_lnk_get(d->rec, d->fig->sh[i].link, &v) < E_OK ) {
		return ED_MIN;
	}
	h = fz(d, tad_lnk_closed_h(&v));

	return ( h > ED_MIN ) ? h : ED_MIN;
}

/*
 * A link of the record changed. One that cannot be found or written is
 * said on the console: the change is not made, and nothing is shown as
 * if it were.
 */
LOCAL ER link_put( DTWIN *d, CONST T_VOBJ *v )
{
	ER	er = tad_lnk_set((T_TAD *)d->rec, v);

	if ( er < E_OK ) {
		tm_printf((UB *)"desktop: the link \"%s\" was not changed (%d)\n", v->name, er);
	}

	return er;
}

/*
 * Whether a link is protected -- 固定化 or 背景化 -- which keeps it from
 * being carried, sized, cut or thrown away. It can still be taken, so
 * that the protection can be taken off again.
 */
LOCAL BOOL is_protected( CONST DTWIN *d, INT i )
{
	T_VOBJ	v;

	if ( !is_link(d, i)
	  || tad_lnk_get(d->rec, d->fig->sh[i].link, &v) < E_OK ) {
		return FALSE;
	}

	return (BOOL)( v.fixed || v.background );
}

/* ---------------------------------------------------------------- taking back */

LOCAL void stack_clear( UB **stk, INT *n )
{
	while ( *n > 0 ) {
		(*n)--;
		Kfree(stk[*n]);
		stk[*n] = NULL;
	}
}

/* The oldest of a stack let go */
LOCAL void stack_drop( UB **stk, SZ *len, INT *n )
{
	INT	i;

	if ( *n <= 0 ) {
		return;
	}
	Kfree(stk[0]);
	for ( i = 1; i < *n; i++ ) {
		stk[i - 1] = stk[i];
		len[i - 1] = len[i];
	}
	(*n)--;
}

/* What the records kept for taking back take together */
LOCAL SZ stacks_held( CONST DTWIN *d )
{
	SZ	t = 0;
	INT	i;

	for ( i = 0; i < d->nundo; i++ ) t += d->undo_len[i];
	for ( i = 0; i < d->nredo; i++ ) t += d->redo_len[i];

	return t;
}

IMPORT SZ	knl_Ifree_bytes( void );

/*
 * How much they may take: a quarter of the memory that is free and of
 * what they hold already, so that taking back never leaves the system
 * without memory. Any number of changes is kept within it. A record of
 * less than a megabyte is kept in the kernel heap, and one larger in
 * pages of its own (Kmalloc): the less free of the two is what counts.
 */
LOCAL SZ stacks_room( CONST DTWIN *d, SZ size )
{
	T_RSMB	r;
	SZ	pages = 0, heap, held = stacks_held(d);

	if ( tk_ref_smb(&r) >= E_OK && r.free > 0 && r.blksz > 0 ) {
		pages = (SZ)r.free * (SZ)r.blksz;
	}
	heap = knl_Ifree_bytes();
	if ( heap < 0 ) {
		heap = 0;
	}

	if ( size >= 1024 * 1024 ) {
		return ( pages + held ) / 4;
	}
	return ( ( ( pages < heap ) ? pages : heap ) + held ) / 4;
}

/*
 * A record kept on a stack, its newest. The oldest of the changes that
 * can be taken back go while the kept ones would take more than their
 * room. When even so it cannot be kept, it goes: the change itself is
 * made all the same, only it cannot be taken back.
 */
LOCAL void stack_push( DTWIN *d, UB ***stk, SZ **len, INT *n, INT *max, UB *xml, SZ size )
{
	SZ	room = stacks_room(d, size);

	while ( d->nundo > 0 && stacks_held(d) + size > room ) {
		stack_drop(d->undo, d->undo_len, &d->nundo);
	}
	if ( *n >= *max ) {
		INT	m = ( *max > 0 ) ? *max * 2 : 16;
		UB	**ns = (UB **)Krealloc(*stk, sizeof(UB *) * (size_t)m);
		SZ	*nl;

		if ( ns != NULL ) {
			*stk = ns;
		}
		nl = ( ns != NULL ) ? (SZ *)Krealloc(*len, sizeof(SZ) * (size_t)m) : NULL;
		if ( nl != NULL ) {
			*len = nl;
			*max = m;
		} else if ( *n > 0 ) {
			stack_drop(*stk, *len, n);	/* no room for more: the oldest goes */
		} else {
			Kfree(xml);
			return;
		}
	}
	(*stk)[*n] = xml;
	(*len)[*n] = size;
	(*n)++;
}

/*
 * About to change the record: keep it as it is now. A new change
 * throws away what had been taken back, because the changes taken back
 * were made to a record that is no longer there.
 */
/* Whether the record written out ahead is the record as it is now */
LOCAL BOOL pre_good( CONST DTWIN *d )
{
	CONST T_TADNODE	*root = ( d->rec != NULL ) ? tad_root(d->rec) : NULL;

	return (BOOL)( d->pre != NULL && root != NULL && d->pre_gen == root->gen
		    && d->pre_serial == tad_serial(d->rec) );
}

LOCAL void before( DTWIN *d )
{
	UB	*xml = NULL;
	SZ	size = 0;

	stack_clear(d->redo, &d->nredo);
	if ( pre_good(d) ) {
		/* written out ahead, and nothing changed since */
		xml = d->pre;
		size = d->pre_len;
		d->pre = NULL;
		stack_push(d, &d->undo, &d->undo_len, &d->nundo, &d->maxundo, xml, size);
		return;
	}
	if ( om_store_snapshot(&d->id, d->recno, &xml, &size) >= E_OK ) {
		stack_push(d, &d->undo, &d->undo_len, &d->nundo, &d->maxundo, xml, size);
	}
}

/*
 * A window whose record is to be written out ahead: one with an object
 * that can be changed, large enough for the writing to be felt, whose
 * record has changed since it was last written out. A small record is
 * written out when it is changed, as quickly.
 */
#define PRE_MIN		( 256 * 1024 )

LOCAL BOOL pre_wanted( CONST DTWIN *d )
{
	CONST T_TADNODE	*root;

	if ( !d->used || d->sealed || d->tool != DT_TOOL_NONE || d->rec == NULL
	  || d->tb_host != NULL || d->xml_view || ( root = tad_root(d->rec) ) == NULL ) {
		return FALSE;
	}
	/* the record as it is now not yet written out, nor found too small to be */
	return (BOOL)( d->pre_gen != root->gen || d->pre_serial != tad_serial(d->rec) );
}

EXPORT BOOL ed_prepare_pending( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( pre_wanted(dt_win_at(i)) ) {
			return TRUE;
		}
	}

	return FALSE;
}

EXPORT void ed_prepare_step( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN		*d = dt_win_at(i);
		UB		*xml = NULL;
		SZ		size = 0;
		CONST T_TADNODE	*root;

		if ( !pre_wanted(d) ) {
			continue;
		}
		if ( d->pre != NULL ) {
			Kfree(d->pre);
			d->pre = NULL;
		}
		root = tad_root(d->rec);
		if ( om_store_snapshot(&d->id, d->recno, &xml, &size) >= E_OK ) {
			if ( size >= PRE_MIN ) {
				d->pre = xml;
				d->pre_len = size;
				d->pre_gen = root->gen;
				d->pre_serial = tad_serial(d->rec);
				return;			/* one a turn */
			}
			Kfree(xml);
		}
		/* small, or not to be had: not tried again until it has changed */
		d->pre = NULL;
		d->pre_len = 0;
		d->pre_gen = root->gen;
		d->pre_serial = tad_serial(d->rec);
		return;
	}
}

/*
 * What has just been put into the record taken, and nothing else: the
 * links from number 'from' on are the ones added, because a link is
 * always added at the end.
 */
/* Shape i taken, after everything taken so far */
LOCAL void pick_add( DTWIN *d, INT i )
{
	if ( d->pick[i] != 0 ) {
		return;
	}
	if ( d->pick_seq == 0xFFFFFFFFU ) {
		d->pick_seq = 0xFFFFFFFEU;	/* past counting: they tie */
	}
	d->pick[i] = ++d->pick_seq;
	d->npick++;
}

LOCAL void pick_from( DTWIN *d, INT from )
{
	INT	i;

	ed_pick_none(d);
	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( is_link(d, i) && d->fig->sh[i].link >= from ) {
			pick_add(d, i);
		}
	}
}

/*
 * Changed: the model is made again, and the pages are drawn from it --
 * every window's, since another window may be showing this record open
 * inside its own page.
 */
LOCAL void after( DTWIN *d )
{
	ed_model(d);
	d->dirty = TRUE;
	dt_draw_all();
	wm_update();
}

/* One stack's newest record put back, and the present one kept on the other */
LOCAL void swap_back( DTWIN *d, UB **from, SZ *from_len, INT *nfrom,
		      UB **to, SZ *to_len, INT *nto )
{
	UB	*now = NULL;
	SZ	now_len = 0;
	T_TAD	*rec;
	INT	k;

	if ( *nfrom == 0 ) {
		return;
	}
	/* the record is put back whole: a piece of text being edited in it goes */
	if ( d->tb != NULL ) {
		df_text_end(d);
	}
	if ( om_store_snapshot(&d->id, d->recno, &now, &now_len) < E_OK ) {
		return;
	}
	k = *nfrom - 1;
	rec = om_store_restore(&d->id, d->recno, from[k], from_len[k]);
	if ( rec == NULL ) {
		Kfree(now);
		return;
	}
	Kfree(from[k]);
	from[k] = NULL;
	(*nfrom)--;
	if ( to == d->undo ) {
		stack_push(d, &d->undo, &d->undo_len, &d->nundo, &d->maxundo, now, now_len);
	} else {
		stack_push(d, &d->redo, &d->redo_len, &d->nredo, &d->maxredo, now, now_len);
	}
	d->rec = rec;
	ed_pick_none(d);
	after(d);
}

EXPORT void ed_before( DTWIN *d )
{
	before(d);
}

EXPORT void ed_changed( DTWIN *d )
{
	after(d);
}

EXPORT void ed_undo( DTWIN *d )
{
	if ( d->sealed ) {
		return;
	}
	swap_back(d, d->undo, d->undo_len, &d->nundo,
		  d->redo, d->redo_len, &d->nredo);
}

EXPORT void ed_redo( DTWIN *d )
{
	if ( d->sealed ) {
		return;
	}
	swap_back(d, d->redo, d->redo_len, &d->nredo,
		  d->undo, d->undo_len, &d->nundo);
}

/* ---------------------------------------------------------------- picking */

EXPORT BOOL ed_picked( CONST DTWIN *d, INT i )
{
	return (BOOL)( i >= 0 && i < d->maxpick && d->pick[i] != 0 );
}

EXPORT void ed_pick_none( DTWIN *d )
{
	INT	i;

	for ( i = 0; i < d->maxpick; i++ ) {
		d->pick[i] = 0;
	}
	d->npick = 0;
	d->pick_seq = 0;
}

EXPORT void ed_pick_all( DTWIN *d )
{
	INT	i;

	ed_pick_none(d);
	if ( d->fig == NULL ) {
		return;
	}
	/*
	 * what is protected is taken too, so that its protection can be taken
	 * off; what is done to what is taken leaves it where it is
	 */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( takeable(d, i) ) {
			pick_add(d, i);
		}
	}
}

EXPORT void ed_pick_only( DTWIN *d, INT i )
{
	ed_pick_none(d);
	if ( takeable(d, i) ) {
		pick_add(d, i);
	}
	df_whole_groups(d);
}

/* Shape i picked, after those picked already, its group not looked at */
EXPORT void ed_pick_add( DTWIN *d, INT i )
{
	if ( takeable(d, i) && d->pick[i] == 0 ) {
		pick_add(d, i);
	}
}

EXPORT void ed_pick_toggle( DTWIN *d, INT i )
{
	if ( !takeable(d, i) ) {
		return;
	}
	if ( d->pick[i] != 0 ) {
		d->pick[i] = 0;
		d->npick--;
	} else {
		pick_add(d, i);
		df_whole_groups(d);
	}
}

/* 選択枠のちらつき: the turn the frames in the figure editor are hidden */
LOCAL BOOL	ed_blink_off = FALSE;

EXPORT void ed_blink_turn( void )
{
	ed_blink_off = (BOOL)!ed_blink_off;
}

EXPORT void ed_blink_show( void )
{
	ed_blink_off = FALSE;
}

EXPORT BOOL ed_blinks( CONST DTWIN *d )
{
	return (BOOL)( d != NULL && d->used && d->fig != NULL && d->npick > 0
		    && df_is((DTWIN *)d) );
}

/*
 * What is taken, shown: a frame two pixels wide just outside each one,
 * in orange. It is drawn after the page, so it is over whatever the
 * object drew. In the figure editor the frames come and go by turns,
 * except while what is taken is being carried or sized.
 */
EXPORT void ed_draw_picks( DTWIN *d )
{
	INT		gid = wm_gid(d->wid), i, sw;
	T_DPRECT	b, page;

	if ( gid < 0 || d->fig == NULL || d->npick == 0 ) {
		return;
	}
	if ( ed_blink_off && df_is(d) && !ed_holding() ) {
		return;
	}
	dt_work_rect(d, &page);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !shape_box(d, i, &b) ) {
			continue;
		}
		/* only those the window shows */
		if ( b.right + ED_HANDLE < page.left || b.left - ED_HANDLE > page.right
		  || b.bottom + ED_HANDLE < page.top || b.top - ED_HANDLE > page.bottom ) {
			continue;
		}
		/* the frame as thick as ユーザ環境設定 says: 細 中 太 */
		sw = wm_num(LK_SEL_W, 1) + 1;
		b.left -= sw;  b.top -= sw;
		b.right += sw;  b.bottom += sw;
		if ( !df_is(d) ) {
			dp_frame_rect(gid, &b, ED_PICK_COL, sw);
			continue;
		}
		/*
		 * In the figure editor a broken frame, and at its foot on
		 * the right the handle it is sized by -- except for a
		 * virtual object, which is sized by its edges, and a
		 * line, which is changed by carrying its ends.
		 */
		{
			T_DPPAT	pat;

			dp_pat_colour(&pat, ED_PICK_COL);
			dp_line_wide(gid, b.left, b.top, b.right, b.top, sw, DP_LINE_DASH, &pat);
			dp_line_wide(gid, b.left, b.bottom, b.right, b.bottom, sw, DP_LINE_DASH, &pat);
			dp_line_wide(gid, b.left, b.top, b.left, b.bottom, sw, DP_LINE_DASH, &pat);
			dp_line_wide(gid, b.right, b.top, b.right, b.bottom, sw, DP_LINE_DASH, &pat);
		}
		if ( !is_link(d, i) && d->fig->sh[i].kind != TV_SH_LINE ) {
			T_DPRECT	h;

			h.left = b.right - ED_HANDLE / 2;
			h.top = b.bottom - ED_HANDLE / 2;
			h.right = h.left + ED_HANDLE;
			h.bottom = h.top + ED_HANDLE;
			dp_fill_rect(gid, &h, 0x00FFFFFFU);
			dp_frame_rect(gid, &h, ED_PICK_COL, 2);
		}
	}
}

/*
 * The handle of a picked shape under a place, in the figure editor:
 * the shape it sizes, or -1. A finger reaches it slop farther out of
 * the shape, and into it as far as wm_hit_in lets.
 */
LOCAL INT handle_at( DTWIN *d, INT x, INT y, INT slop )
{
	T_DPRECT	b;
	INT		i, ix, iy, out = ED_HANDLE / 2 + 1;

	if ( !df_is(d) ) {
		return -1;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || is_link(d, i)
		  || d->fig->sh[i].kind == TV_SH_LINE || !shape_box(d, i, &b) ) {
			continue;
		}
		ix = wm_hit_in(out, slop, b.right - b.left);
		iy = wm_hit_in(out, slop, b.bottom - b.top);
		b.right += 2;
		b.bottom += 2;
		if ( x >= b.right - ix && x <= b.right + out + slop
		  && y >= b.bottom - iy && y <= b.bottom + out + slop ) {
			return i;
		}
	}

	return -1;
}

/* ---------------------------------------------------------------- changes */

/*
 * The links and shapes taken, deleted. 'device': a disk's link deleted
 * from the first window takes its disk away (dt_disk_on_delete); one
 * that cannot be taken away now stays, not taken. Cutting moves a link
 * to the tray rather than deleting it, and leaves the disk as it is.
 */
LOCAL void del_picked( DTWIN *d, BOOL device )
{
	TS_UUID	*gone;
	INT	i, n = 0, np;
	T_VOBJ	v;

	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	gone = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)d->npick);
	if ( gone == NULL ) {
		return;
	}
	/* the identities first: deleting one moves the others' numbers */
	np = d->npick;
	for ( i = 0; i < d->fig->nsh && n < np; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && !is_protected(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
			if ( device && d == dt_first_win() && !dt_disk_on_delete(d, &v) ) {
				d->pick[i] = 0;
				d->npick--;
				continue;
			}
			gone[n++] = v.vobjid;
		}
	}
	if ( n > 0 || df_picked(d) > 0 ) {
		before(d);
		df_delete_picked(d);
		for ( i = 0; i < n; i++ ) {
			tad_lnk_del((T_TAD *)d->rec, &gone[i]);
		}
		ed_pick_none(d);
		after(d);
	}
	Kfree(gone);
}

EXPORT void ed_delete( DTWIN *d )
{
	del_picked(d, TRUE);
}

/* A set's name: what it holds and how many */
LOCAL void set_name( UB *out, CONST char *what, INT n )
{
	INT	at = 0, i, k;
	char	d[12];

	for ( i = 0; what[i] != 0 && at < TR_NAME_MAX - 16; i++ ) out[at++] = (UB)what[i];
	k = 0;
	do {
		d[k++] = (char)( '0' + n % 10 );
		n /= 10;
	} while ( n > 0 );
	while ( k > 0 ) out[at++] = (UB)d[--k];
	for ( i = 0; "個"[i] != 0; i++ ) out[at++] = (UB)"個"[i];
	out[at] = 0;
}

/*
 * The links and shapes taken, to the tray as one set: a <figure>
 * fragment holding a copy of each shape and a link to each object.
 */
/*
 * The links and shapes taken, to the tray: TRUE when they are there (or
 * there was nothing to take), FALSE when they could not be put there,
 * and then the person is told.
 */
LOCAL BOOL copy_picked( DTWIN *d )
{
	T_TAD	*frag;
	T_VOBJ	v;
	TS_UUID	id;
	UB	name[TR_NAME_MAX];
	INT	i, nl = 0, ns;
	ER	er = E_OK;

	if ( d->fig == NULL || d->npick == 0 ) {
		return TRUE;
	}
	frag = dt_frag_new("figure");
	if ( frag == NULL ) {
		dt_no_memory(d);
		return FALSE;
	}
	ed_pasted = 0;
	ns = df_copy_into(d, frag);
	if ( ns < 0 ) {
		tad_free(frag);
		dt_no_memory(d);
		return FALSE;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
			knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
			if ( tad_lnk_add(frag, &v, &id) >= E_OK ) {
				nl++;
			}
		}
	}
	if ( nl + ns > 0 ) {
		set_name(name, ( ns == 0 ) ? "仮身" : ( nl == 0 ) ? "図形" : "図形と仮身", nl + ns);
		er = dt_tray_put(frag, (CONST char *)name);
	}
	tad_free(frag);
	if ( er < E_OK ) {
		dt_tell(d, "トレーに入れられませんでした", ( er == E_NOMEM )
			? "メモリが足りません。図形は元のままです" : "図形は元のままです");
		return FALSE;
	}

	return TRUE;
}

EXPORT void ed_copy( DTWIN *d )
{
	(void)copy_picked(d);
}

EXPORT void ed_cut( DTWIN *d )
{
	INT	i;

	if ( d->sealed || d->fig == NULL ) {
		return;
	}
	/* what is protected stays, and is not taken to the tray */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_protected(d, i) ) {
			d->pick[i] = 0;
			d->npick--;
		}
	}
	/* taken away only once they are safe in the tray */
	if ( copy_picked(d) ) {
		del_picked(d, FALSE);
	}
}

/*
 * The set in hand in the tray put into this record, each time set off a
 * step further than the last so that pasting twice does not stack two
 * objects exactly on top of each other. A pasted link is a new link to
 * the same object: it gets an identity of its own. Text goes into a
 * figure as a piece of text; a cabinet takes only the links.
 *
 * 'take' takes the set out of the tray after, which is what moving from
 * the tray is, as against copying from it.
 */
EXPORT void ed_paste( DTWIN *d, BOOL take )
{
	T_TAD	*frag;
	T_VOBJ	v;
	TS_UUID	id;
	INT	i, off, first, nlinks = 0;
	UINT	kind = dt_tray_kind(&nlinks);

	if ( kind == DT_TRAY_DOC ) {
		if ( df_is(d) && !d->sealed ) {
			df_paste_text(d);
			if ( take ) {
				dt_tray_taken();
			}
		}
		return;
	}
	if ( kind != DT_TRAY_FIG || d->fig == NULL || d->sealed
	  || ( nlinks == 0 && !df_is(d) ) ) {
		return;
	}
	frag = dt_tray_frag();
	if ( frag == NULL ) {
		return;
	}
	before(d);
	first = tad_lnk_count(d->rec);
	ed_pasted++;
	off = ED_STEP * ed_pasted;
	(void)dt_tray_pictures(&d->id, d->rec, frag);	/* the pictures the set carries, beside the record */
	(void)df_paste_frag(d, frag, off);
	for ( i = 0; i < nlinks; i++ ) {
		if ( tad_lnk_get(frag, i, &v) < E_OK ) {
			continue;
		}
		knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
		v.left += off;  v.right += off;
		v.top += off;   v.bottom += off;
		(void)tad_lnk_add((T_TAD *)d->rec, &v, &id);
	}
	tad_free(frag);
	if ( take ) {
		dt_tray_taken();
	}
	ed_model(d);
	pick_from(d, first);
	df_pick_pasted(d);
	after(d);
}

EXPORT void ed_open_picked( DTWIN *d )
{
	INT	i;
	T_VOBJ	v;

	if ( d->fig == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
			dt_open_vobj(d, &v);
		}
	}
}

EXPORT void ed_close_picked( DTWIN *d )
{
	INT	i;
	T_VOBJ	v;

	if ( d->fig == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
			dt_close_vobj(&v);
		}
	}
}

/* ---------------------------------------------------------------- order */

/* Every link's identity, in the order the record holds them; NULL when none */
LOCAL TS_UUID *link_ids( DTWIN *d, INT *p_n )
{
	TS_UUID	*ids;
	T_VOBJ	v;
	INT	n = tad_lnk_count(d->rec), i;

	*p_n = 0;
	if ( n <= 0 ) {
		return NULL;
	}
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( ids == NULL ) {
		return NULL;
	}
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(d->rec, i, &v) < E_OK ) {
			Kfree(ids);
			return NULL;
		}
		ids[i] = v.vobjid;
	}
	*p_n = n;

	return ids;
}

/* Whether link number k is one of those taken */
LOCAL BOOL link_picked( CONST DTWIN *d, INT k )
{
	INT	i;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && d->fig->sh[i].link == k ) {
			return TRUE;
		}
	}

	return FALSE;
}

LOCAL BOOL link_background( CONST DTWIN *d, INT k )
{
	T_VOBJ	v;

	return (BOOL)( tad_lnk_get(d->rec, k, &v) >= E_OK && v.background );
}

/*
 * The links put in the order given by three groups, each keeping the
 * order it had: first those 'first' says, then those 'mid' says, then
 * the rest. Later is further forward.
 */
LOCAL ER regroup( DTWIN *d, BOOL (*first)( CONST DTWIN *, INT ),
		  BOOL (*mid)( CONST DTWIN *, INT ) )
{
	TS_UUID	*ids, *order;
	INT	n, i, k = 0, pass;
	BOOL	*placed;
	UINT	*taken;
	ER	er;

	ids = link_ids(d, &n);
	if ( ids == NULL ) {
		return E_NOEXS;
	}
	order = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	placed = (BOOL *)Kmalloc(sizeof(BOOL) * (SZ)n);
	if ( order == NULL || placed == NULL ) {
		if ( order != NULL ) Kfree(order);
		if ( placed != NULL ) Kfree(placed);
		Kfree(ids);
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		placed[i] = FALSE;
	}
	for ( pass = 0; pass < 3; pass++ ) {
		for ( i = 0; i < n; i++ ) {
			BOOL	in;

			if ( placed[i] ) {
				continue;
			}
			in = ( pass == 0 ) ? ( first != NULL && first(d, i) )
			   : ( pass == 1 ) ? ( mid != NULL && mid(d, i) )
			   : TRUE;
			if ( in ) {
				order[k++] = ids[i];
				placed[i] = TRUE;
			}
		}
	}
	/*
	 * A pick belongs to a shape by its number, and the numbers of the
	 * links' shapes change with their order: what is taken is kept by
	 * the identities of its links, and taken again by them once the
	 * model has been made again.
	 */
	taken = (UINT *)Kmalloc(sizeof(UINT) * (SZ)n);
	if ( taken == NULL ) {
		Kfree(placed);
		Kfree(order);
		Kfree(ids);
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		taken[i] = 0;
	}
	for ( i = 0; d->fig != NULL && i < d->fig->nsh && i < d->maxpick; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && d->fig->sh[i].link < n ) {
			taken[d->fig->sh[i].link] = d->pick[i];
		}
	}
	er = tad_lnk_reorder((T_TAD *)d->rec, order, n);
	if ( er >= E_OK ) {
		ed_model(d);
		d->npick = 0;
		for ( i = 0; i < d->maxpick; i++ ) {
			d->pick[i] = 0;
		}
		for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
			T_VOBJ	v;
			INT	j;

			if ( !is_link(d, i) || tad_lnk_get(d->rec, d->fig->sh[i].link, &v) < E_OK ) {
				continue;
			}
			for ( j = 0; j < n; j++ ) {
				if ( taken[j] != 0 && ts_uuid_cmp(&ids[j], &v.vobjid) == 0 ) {
					d->pick[i] = taken[j];
					d->npick++;
					break;
				}
			}
		}
	}
	Kfree(taken);
	Kfree(placed);
	Kfree(order);
	Kfree(ids);

	return er;
}

LOCAL BOOL moving_front( CONST DTWIN *d, INT k )
{
	return (BOOL)( !link_picked(d, k) || link_background(d, k) );
}

LOCAL BOOL moving_back( CONST DTWIN *d, INT k )
{
	return (BOOL)( link_picked(d, k) && !link_background(d, k) );
}

/*
 * いちばん前へ: what is taken goes after everything else. What is in the
 * background stays there: it does not come in front of the rest.
 */
EXPORT void ed_front( DTWIN *d )
{
	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	if ( df_is(d) ) {
		df_front(d, TRUE);		/* by the order shapes are drawn in */
		return;
	}
	before(d);
	regroup(d, moving_front, NULL);
	after(d);
}

/* いちばん後ろへ: behind everything but what is in the background */
EXPORT void ed_back( DTWIN *d )
{
	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	if ( df_is(d) ) {
		df_front(d, FALSE);
		return;
	}
	before(d);
	regroup(d, link_background, moving_back);
	after(d);
}

/*
 * 固定化 and 背景化, put on or taken off what is taken. What goes into
 * the background goes behind everything else as well.
 */
EXPORT void ed_protect( DTWIN *d, BOOL background, BOOL on )
{
	INT	i;
	T_VOBJ	v;

	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	before(d);
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !is_link(d, i)
		  || tad_lnk_get(d->rec, d->fig->sh[i].link, &v) < E_OK ) {
			continue;
		}
		if ( background ) {
			v.background = on;
		} else {
			v.fixed = on;
		}
		(void)link_put(d, &v);
	}
	if ( background && on ) {
		regroup(d, link_background, NULL);
	}
	after(d);
}

EXPORT BOOL ed_link_at( DTWIN *d, INT which, T_VOBJ *v )
{
	return (BOOL)( is_link(d, which)
		    && tad_lnk_get(d->rec, d->fig->sh[which].link, v) >= E_OK );
}

EXPORT void ed_set_link( DTWIN *d, CONST T_VOBJ *v )
{
	if ( d->sealed ) {
		return;
	}
	before(d);
	(void)link_put(d, v);
	after(d);
}

/* The shape taken first, or -1 */
EXPORT INT ed_first_pick( DTWIN *d )
{
	INT	i, best = -1;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0
		  && ( best < 0 || d->pick[i] < d->pick[best] ) ) {
			best = i;
		}
	}

	return best;
}

/* ---------------------------------------------------------------- 整頓 */

/* What 整頓 knows of the object a link points at: the store's metadata */
LOCAL BOOL ar_info( void *arg, CONST TS_UUID *id, T_FEARINFO *inf )
{
	T_OMINFO	*mi = (T_OMINFO *)Kmalloc(sizeof(T_OMINFO));
	UB		app[1][OM_APP_NAME];
	INT		i;

	(void)arg;
	if ( mi != NULL ) {
		(void)om_store_info(id, mi);
		knl_memcpy(inf->name, mi->name, TAD_NAME_MAX);
		for ( i = 0; i < FE_AR_INFO_TEXT - 1 && i < OM_INFO_TEXT && mi->made[i] != 0; i++ ) {
			inf->made[i] = mi->made[i];
		}
		for ( i = 0; i < FE_AR_INFO_TEXT - 1 && i < OM_INFO_TEXT && mi->updated[i] != 0; i++ ) {
			inf->updated[i] = mi->updated[i];
		}
		inf->bytes = mi->bytes;
		Kfree(mi);
	}
	inf->has_label = (BOOL)( om_store_name(id, inf->label, TAD_NAME_MAX) >= 0 );
	if ( om_store_relationship(id, inf->rel, TAD_NAME_MAX) <= 0 ) {
		inf->rel[0] = 0;
	}
	if ( om_store_apps(id, app, 1) > 0 ) {
		for ( i = 0; i < FE_AR_APP_NAME - 1 && i < OM_APP_NAME && app[0][i] != 0; i++ ) {
			inf->app[i] = app[0][i];
		}
		inf->has_app = TRUE;
	}

	return TRUE;
}

/*
 * 整頓 of the links taken, by the figure editor's core (fe_arrange),
 * which the process's 仮身一覧 shares. AR_W_* and AR_BY_* are numbered
 * as FE_AR_W_* and FE_AR_BY_*.
 */
EXPORT void ed_arrange( DTWIN *d, CONST T_ARRANGE *o )
{
	T_FEARITEM	*it;
	T_FEARRANGE	a;
	INT		n = 0, i;

	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	it = (T_FEARITEM *)Kmalloc(sizeof(T_FEARITEM) * (SZ)d->npick);
	if ( it == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh && n < d->npick; i++ ) {
		if ( d->pick[i] != 0 && ed_link_at(d, i, &it[n].v) ) {
			it[n].seq = d->pick[i];
			n++;
		}
	}
	a.horizontal = ( o->horizontal == AR_LEFT ) ? FE_AR_LEFT
		     : ( o->horizontal == AR_RIGHT ) ? FE_AR_RIGHT : FE_AR_NONE;
	a.vertical = ( o->vertical == AR_COMPACT ) ? FE_AR_COMPACT
		   : ( o->vertical == AR_ALIGN ) ? FE_AR_ALIGN : FE_AR_NONE;
	a.column = ( o->column == AR_SINGLE ) ? FE_AR_SINGLE
		 : ( o->column == AR_MULTI_H ) ? FE_AR_MULTI_H
		 : ( o->column == AR_MULTI_V ) ? FE_AR_MULTI_V : FE_AR_NONE;
	a.columns = o->columns;
	a.width = o->width;
	a.sort_by = o->sort_by;
	a.descending = o->descending;
	n = fe_arrange(it, n, &a, ar_info, NULL);
	if ( n > 0 ) {
		before(d);
		for ( i = 0; i < n; i++ ) {
			(void)link_put(d, &it[i].v);
		}
		after(d);
	}
	Kfree(it);
}

/* ---------------------------------------------------------------- carrying */

/*
 * The outline of what is being carried, or of the band being drawn,
 * handed to the window system in screen coordinates. It is laid over
 * every window, so it follows the hand out of the window it came from
 * -- into another window, or onto the ground between them -- and it is
 * never drawn into any window's own pixels, so taking it away leaves
 * nothing behind.
 */
LOCAL void band_show( void )
{
	T_DPRECT	r[WM_DRAG_MAX], b;
	T_WMWIN		w;
	INT		i, n = 0;

	if ( hold_d == NULL || wm_ref(hold_d->wid, &w) < E_OK ) {
		return;
	}
	switch ( hold_how ) {
	case HOLD_MOVE:
		for ( i = 0; hold_d->fig != NULL && i < hold_d->fig->nsh
			     && n < WM_DRAG_MAX; i++ ) {
			if ( hold_d->pick[i] == 0 || is_protected(hold_d, i)
			  || !shape_box(hold_d, i, &b) ) {
				continue;
			}
			b.left += hold_dx;  b.right += hold_dx;
			b.top += hold_dy;   b.bottom += hold_dy;
			r[n++] = b;
		}
		break;
	case HOLD_W:
	case HOLD_H:
	case HOLD_WH:
		r[n++] = hold_box;
		break;
	case HOLD_DUP:
		b = hold_box;
		b.left += hold_dx;  b.right += hold_dx;
		b.top += hold_dy;   b.bottom += hold_dy;
		r[n++] = b;
		break;
	case HOLD_BAND:
		b.left   = ( hold_x0 < hold_px ) ? hold_x0 : hold_px;
		b.right  = ( hold_x0 < hold_px ) ? hold_px : hold_x0;
		b.top    = ( hold_y0 < hold_py ) ? hold_y0 : hold_py;
		b.bottom = ( hold_y0 < hold_py ) ? hold_py : hold_y0;
		if ( b.right > b.left && b.bottom > b.top ) {
			r[n++] = b;
		}
		break;
	default:
		break;
	}
	for ( i = 0; i < n; i++ ) {
		r[i].left += w.work.left;  r[i].right += w.work.left;
		r[i].top += w.work.top;    r[i].bottom += w.work.top;
	}
	if ( !band_shown && n > 0 && ( hold_how == HOLD_MOVE || hold_how == HOLD_DUP ) ) {
		dt_carry_looks(r, n, hold_dx, hold_dy);
	}
	wm_set_drag(r, n);
	band_shown = (BOOL)( n > 0 );
}

LOCAL void band_hide( void )
{
	if ( band_shown ) {
		wm_set_drag(NULL, 0);
		band_shown = FALSE;
	}
}

/*
 * What a press at (x, y) on or by a box does to it: its right edge
 * sizes its width, its foot its height, the corner both; elsewhere it
 * is carried. A finger's press (slop, wm_hit_slop) reaches the edges
 * farther into the box, as far as wm_hit_in lets, and slop out of it.
 */
LOCAL UINT hold_kind( CONST T_DPRECT *box, INT x, INT y, INT slop )
{
	BOOL	right = (BOOL)( x >= box->right - wm_hit_in(ED_EDGE, slop, box->right - box->left) );
	BOOL	foot  = (BOOL)( y >= box->bottom - wm_hit_in(ED_EDGE, slop, box->bottom - box->top) );

	if ( right && foot ) return HOLD_WH;
	if ( right )         return HOLD_W;
	if ( foot )          return HOLD_H;
	return HOLD_MOVE;
}

/*
 * A finger's press just out of the one thing taken, by its right edge
 * or its foot (no farther than slop): the thing, which the press is to
 * size, or -1. Only when nothing is under the press: a thing pressed
 * is always the one pressed, and the band is drawn where the press is
 * farther from what is taken.
 */
LOCAL INT edge_near( DTWIN *d, INT x, INT y, INT slop, T_DPRECT *p_box )
{
	T_DPRECT	b;
	INT		i;

	if ( slop <= 0 || d->fig == NULL || d->npick != 1 ) {
		return -1;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !takeable(d, i) || d->fig->sh[i].kind == TV_SH_LINE
		  || is_protected(d, i) || !shape_box(d, i, &b) ) {
			continue;
		}
		if ( x >= b.left && x < b.right + slop && y >= b.top && y < b.bottom + slop
		  && ( x >= b.right || y >= b.bottom ) ) {
			*p_box = b;
			return i;
		}
	}
	return -1;
}

/*
 * A press in a window, in the window's own coordinates. Answers TRUE
 * when it was on something this layer deals with, so that the caller
 * knows not to treat it as anything else.
 */
LOCAL BOOL	hold_copy = FALSE;	/* 複写ドラッグ: the second button went down */

EXPORT void ed_hold_copy( void )
{
	if ( hold_d != NULL && hold_how == HOLD_MOVE ) {
		hold_copy = TRUE;
	}
}

EXPORT BOOL ed_press( DTWIN *d, INT x, INT y, BOOL add, UD when )
{
	T_DPRECT	page, box;
	T_VOBJ		v;
	INT		which = -1;
	ER		er = E_NOEXS;
	UD		gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	BOOL		twice, shape = FALSE;
	INT		slop = wm_hit_slop(when);

	hold_copy = FALSE;
	dt_work_rect(d, &page);
	/* the handle of a picked shape sizes it, from its foot on the right */
	if ( !add && ( which = handle_at(d, x, y, slop) ) >= 0 && shape_box(d, which, &box)
	  && !is_protected(d, which) ) {
		hold_d     = d;
		hold_which = which;
		hold_box   = box;
		hold_how   = HOLD_WH;
		hold_dx = hold_dy = 0;
		hold_x0 = hold_px = x;
		hold_y0 = hold_py = y;
		hold_moved = FALSE;
		band_shown = FALSE;
		return TRUE;
	}
	which = -1;
	if ( d->doc != NULL ) {
		er = tv_doc_at(d->doc, &page, d->scroll_y, d->rec, x, y,
			       &v, &box, &which);
	} else if ( d->fig != NULL ) {
		er = tv_fig_at(d->fig, &page, d->scroll_x, d->scroll_y,
			       d->rec, x, y, &v, &box, &which);
	}
	/* in the figure editor, a shape that is not a link is taken too */
	if ( er < E_OK && df_is(d)
	  && tv_fig_shape_at(d->fig, &page, d->scroll_x, d->scroll_y, x, y,
			     &which) >= E_OK
	  && df_takeable(d, which) && shape_box(d, which, &box) ) {
		shape = TRUE;
		er = E_OK;
	}
	/* a finger by the edge of what is taken, on nothing: that edge, to size it */
	if ( er < E_OK && !add && ( which = edge_near(d, x, y, slop, &box) ) >= 0 ) {
		hold_d     = d;
		hold_which = which;
		hold_box   = box;
		hold_how   = hold_kind(&box, x, y, slop);
		hold_dx = hold_dy = 0;
		hold_x0 = hold_px = x;
		hold_y0 = hold_py = y;
		hold_moved = FALSE;
		band_shown = FALSE;
		return TRUE;
	}
	twice = (BOOL)( x > click_x - ED_NEAR && x < click_x + ED_NEAR
		     && y > click_y - ED_NEAR && y < click_y + ED_NEAR
		     && when - click_at < gap );
	click_x = x;
	click_y = y;
	click_at = when;

	if ( er >= E_OK ) {
		if ( twice && !shape ) {
			/*
			 * Opening waits for the hand to let go: carried
			 * first, a press twice is a copy of the object
			 * rather than a window onto it.
			 */
			hold_d     = d;
			hold_how   = HOLD_TWICE;
			hold_which = which;
			hold_box   = box;
			hold_v     = v;
			hold_dx = hold_dy = 0;
			hold_x0 = hold_px = x;
			hold_y0 = hold_py = y;
			hold_moved = FALSE;
			band_shown = FALSE;
			return TRUE;
		}
		if ( d->fig == NULL ) {
			/* a link in a text: carried if the hand moves */
			dd_link_take(d, which, &box, x, y, when);
			return TRUE;
		}
		/*
		 * taking it; a finger reaches the edges farther in only of
		 * one taken before the press, whose handles are shown, so a
		 * press to take one is not taken for sizing it
		 */
		if ( !ed_picked(d, which) ) {
			slop = 0;
		}
		if ( add ) {
			ed_pick_toggle(d, which);
		} else if ( !ed_picked(d, which) ) {
			ed_pick_only(d, which);
		}
		dt_draw(d);

		/*
		 * What happens to it is decided now and does not change
		 * until the hand lets go. Sizing is for one object; with
		 * several taken, a press on an edge carries them all.
		 */
		hold_d     = d;
		hold_which = which;
		hold_box   = box;
		hold_how   = ( d->npick == 1 ) ? hold_kind(&box, x, y, slop) : HOLD_MOVE;
		if ( !ed_picked(d, which) ) {
			hold_how = HOLD_NONE;	/* let go of by Shift: nothing held */
		}
		if ( is_protected(d, which) ) {
			hold_how = HOLD_NONE;	/* taken, but it stays where it is */
		}
		hold_dx = hold_dy = 0;
		hold_x0 = hold_px = x;
		hold_y0 = hold_py = y;
		hold_moved = FALSE;
		band_shown = FALSE;
		wm_update();
		return TRUE;
	}

	/* the page itself: let go of everything, and draw a band */
	if ( d->fig != NULL ) {
		if ( !add ) {
			ed_pick_none(d);
			dt_draw(d);
		}
		hold_d = d;
		hold_how = HOLD_BAND;
		hold_add = add;
		hold_which = -1;
		hold_x0 = hold_px = x;
		hold_y0 = hold_py = y;
		hold_moved = FALSE;
		band_shown = FALSE;
		wm_update();
		return TRUE;
	}

	return FALSE;
}

/*
 * A place on the screen as a place in a window's own coordinates. The
 * hand is followed on the screen, not by the events' own places: those
 * are measured from whichever window the pointer is over, and while
 * something is carried that need not be the window it came from.
 */
LOCAL void to_win( CONST DTWIN *d, INT sx, INT sy, INT *x, INT *y )
{
	T_WMWIN	w;

	*x = sx;
	*y = sy;
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		*x = sx - w.work.left;
		*y = sy - w.work.top;
	}
}

EXPORT BOOL ed_holding( void )
{
	return (BOOL)( hold_d != NULL && hold_how != HOLD_NONE );
}

/*
 * A place of the window on the grid of a figure being edited, when the
 * grid holds (格子点拘束); a cabinet's figure has no grid.
 */
LOCAL void grid_snap( INT *x, INT *y )
{
	T_DPRECT	page;

	if ( !df_is(hold_d) ) {
		return;
	}
	dt_work_rect(hold_d, &page);
	ft_snap_win(page.left - hold_d->scroll_x, page.top - hold_d->scroll_y, fig_zoom(hold_d), x, y);
}

/* What is carried brought to the pointer at (x, y), the corner of the shape pressed on the grid */
LOCAL void carry_to( INT x, INT y )
{
	INT	cx = hold_box.left + x - hold_x0, cy = hold_box.top + y - hold_y0;

	grid_snap(&cx, &cy);
	hold_dx = cx - hold_box.left;
	hold_dy = cy - hold_box.top;
}

EXPORT void ed_follow( INT sx, INT sy )
{
	INT	x, y, dx, dy;

	if ( !ed_holding() ) {
		return;
	}
	to_win(hold_d, sx, sy, &x, &y);
	dx = x - hold_px;
	dy = y - hold_py;
	if ( dx == 0 && dy == 0 ) {
		return;
	}
	switch ( hold_how ) {
	case HOLD_TWICE:
		/* a little movement is a shaky hand, not a carry */
		if ( x - hold_x0 < ED_NEAR && hold_x0 - x < ED_NEAR
		  && y - hold_y0 < ED_NEAR && hold_y0 - y < ED_NEAR ) {
			hold_px = x;
			hold_py = y;
			return;
		}
		hold_how = HOLD_DUP;
		carry_to(x, y);
		break;
	case HOLD_MOVE:
	case HOLD_DUP:
		carry_to(x, y);
		break;
	case HOLD_W:
	case HOLD_H:
	case HOLD_WH:
		/* past the edge that stays the shape goes on to the other side, turned over (fe_size_box) */
		if ( !hold_moved ) {
			hold_was = hold_box;
			hold_cx = hold_box.right;
			hold_cy = hold_box.bottom;
			hold_flip = 0;
		}
		hold_cx += dx;
		hold_cy += dy;
		{
			/* the height is looked into only while it is what changes */
			INT	least = ( hold_how == HOLD_H || hold_how == HOLD_WH )
					? least_h(hold_d, hold_which) : ED_MIN;
			INT	cx = hold_cx, cy = hold_cy;

			grid_snap(&cx, &cy);
			fe_size_box(&hold_was, cx, cy, (BOOL)( hold_how != HOLD_H ),
				    (BOOL)( hold_how != HOLD_W ), ED_MIN, least, &hold_box, &hold_flip);
		}
		break;
	default:
		break;
	}
	hold_px = x;
	hold_py = y;
	hold_moved = TRUE;
	/*
	 * The pointer first: laying the outline lays the pointer where it
	 * now is too, and the place it left would then never be repaired.
	 */
	wm_pointer_moved(sx, sy);
	band_show();
}

/*
 * What is taken in one window, carried into another and let go there:
 * each link leaves this record and a link like it is put into that one,
 * where the hand put it. Both records change, so both can be taken
 * back, each in its own window.
 */
LOCAL void carry_over( DTWIN *d, DTWIN *t, BOOL copy )
{
	T_DPRECT	from, to;
	T_WMWIN		wf, wt;
	TS_UUID		*gone;
	INT		i, n = 0, dx, dy, first;
	T_VOBJ		v;

	if ( wm_ref(d->wid, &wf) < E_OK || wm_ref(t->wid, &wt) < E_OK ) {
		return;
	}
	gone = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( d->npick + 1 ));
	if ( gone == NULL ) {
		return;
	}
	dt_work_rect(d, &from);
	dt_work_rect(t, &to);

	/* from a place in this record to the same place on the screen to
	   a place in that one */
	dx = ( wf.work.left + from.left - d->scroll_x + hold_dx )
	   - ( wt.work.left + to.left - t->scroll_x );
	dy = ( wf.work.top + from.top - d->scroll_y + hold_dy )
	   - ( wt.work.top + to.top - t->scroll_y );

	/* what leaves this record, and where it lands in that one */
	before(t);
	before(d);
	first = tad_lnk_count(t->rec);
	for ( i = 0; i < d->fig->nsh && n < d->npick; i++ ) {
		TS_UUID	id;

		if ( d->pick[i] == 0 || !is_link(d, i) || is_protected(d, i)
		  || tad_lnk_get(d->rec, d->fig->sh[i].link, &v) < E_OK ) {
			continue;
		}
		/*
		 * An object is not put inside itself: a link to the record
		 * of the window it is dropped in stays where it was.
		 */
		if ( ts_uuid_cmp(&v.target, &t->id) == 0 ) {
			continue;
		}
		gone[n++] = v.vobjid;
		knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
		v.left += dx;  v.right += dx;
		v.top += dy;   v.bottom += dy;
		tad_lnk_add((T_TAD *)t->rec, &v, &id);
	}
	/* carried as a copy, nothing leaves this record */
	for ( i = 0; !copy && i < n; i++ ) {
		tad_lnk_del((T_TAD *)d->rec, &gone[i]);
	}

	/*
	 * Both records are changed before either is drawn: the one may be
	 * shown open inside the other's page, and drawing between the two
	 * changes would show it half moved.
	 */
	ed_pick_none(d);
	ed_model(d);
	d->dirty = TRUE;
	ed_model(t);
	pick_from(t, first);
	after(t);
	Kfree(gone);
}

/*
 * The name a copy starts with: the original's and 「のコピー」, cut at
 * a letter's edge so that it fits the box it is shown in.
 */
LOCAL void copy_name( CONST T_VOBJ *v, UB *out, INT max )
{
	CONST UB	*tail = (CONST UB *)"のコピー";
	UB		name[TAD_NAME_MAX];
	INT		n, t = 0, k;

	if ( om_store_name(&v->target, name, TAD_NAME_MAX) < 0 ) {
		name[0] = 0;
	}
	while ( tail[t] != 0 ) {
		t++;
	}
	for ( n = 0; name[n] != 0; n++ ) {
		;
	}
	if ( n + t > max - 1 ) {
		n = max - 1 - t;
		if ( n < 0 ) {
			n = 0;
		}
		while ( n > 0 && ( name[n] & 0xC0 ) == 0x80 ) {
			n--;			/* not in the middle of a letter */
		}
	}
	for ( k = 0; k < n; k++ ) {
		out[k] = name[k];
	}
	for ( k = 0; k < t && n + k < max - 1; k++ ) {
		out[n + k] = tail[k];
	}
	out[n + k] = 0;
}

/*
 * The size a link to a new object is put with, given the link it was
 * made from (v, its letters and what it shows) and the height that one
 * stood at (h). Closed -- no taller than its band and TAD_OPEN_SLACK --
 * it is as wide as the new object's name measured in the system's face
 * at the link's letters (14 when it does not say), with its picture and
 * edges (tad_closed_w), and as tall as it is closed; the face is left as
 * it was set. FALSE, nothing set, when it stood opened or shows no band:
 * it keeps the size it had.
 */
LOCAL BOOL new_link_size( CONST T_VOBJ *v, INT h, CONST TS_UUID *target, INT *p_w, INT *p_h )
{
	UB		nm[TAD_NAME_MAX];
	T_FNSTATE	st;
	ID		fid;
	INT		chsz = ( v->chsz > 0 ) ? v->chsz : 14;
	INT		band = tad_lnk_closed_h(v), w = chsz * 6;

	if ( band <= 0 || h > band + TAD_OPEN_SLACK ) {
		return FALSE;
	}
	if ( om_store_name(target, nm, sizeof(nm)) > 0 ) {
		fn_hold();
		fid = fn_system();
		if ( fid > 0 && fn_state_get(fid, &st) >= E_OK ) {
			if ( fn_set_size(fid, chsz) >= E_OK ) {
				INT	m = fn_width(fid, nm);

				if ( m >= 0 ) w = m;
			}
			(void)fn_state_set(fid, &st);
		}
		fn_release();
	}
	*p_w = tad_closed_w(chsz, v->disp, w);
	*p_h = band;
	return TRUE;
}

/*
 * A link c to a new object, made with the look of the link it was made
 * from, sized to the new object's name when it is closed: in a text by
 * its width and height, in a figure by its rectangle from its corner.
 */
EXPORT void ed_new_link_fit( T_VOBJ *c )
{
	BOOL	text = (BOOL)( c->right <= c->left && c->width > 0 );
	INT	w, h;

	if ( !new_link_size(c, text ? c->heightpx : c->bottom - c->top, &c->target, &w, &h) ) {
		return;
	}
	if ( text ) {
		c->width = w;
		c->heightpx = h;
	} else {
		c->right = c->left + w;
		c->bottom = c->top + h;
		c->height = h;
	}
}

/*
 * The same, let go over a program's window that takes links (a
 * process's): the name asked for over that window, the object copied
 * -- or made from the template carried out of the 原紙箱 -- and a link
 * to it dropped there, as links carried out of a figure are. FALSE
 * when the window takes no links.
 */
/*
 * The name a new object made from a template starts with: a program's
 * template, its program's name; any object a 原紙箱 holds (a template
 * the person put there, ユーザ原紙), the object's own name. NULL when
 * the link is to neither, the copy being a plain copy.
 */
LOCAL CONST char *tmpl_name( CONST DTWIN *d, CONST T_VOBJ *v, UB *buf, INT max )
{
	CONST char	*base = dt_base_name(&v->target);

	if ( base == NULL && d != NULL && d->basebox ) {
		if ( om_store_name(&v->target, buf, max) < 0 ) {
			buf[0] = 0;
		}
		base = (CONST char *)buf;
	}
	return base;
}

LOCAL BOOL copy_drop( DTWIN *d, CONST T_VOBJ *v, INT sx, INT sy )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX], own[TAD_NAME_MAX];
	CONST char	*base = tmpl_name(d, v, own, TAD_NAME_MAX);
	TS_UUID		w, nid;
	DTWIN		*host;
	T_VOBJ		c;
	ID		taker = 0;
	INT		wid = wm_at(sx, sy), k;

	if ( wid <= 0 || dt_win_of(wid) != NULL || wm_obj_uuid(wid, &w) < E_OK
	  || !knl_ob_taker(&w, OB_E_DROP, &taker) ) {
		return FALSE;
	}
	if ( base != NULL ) {
		for ( k = 0; base[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) start[k] = (UB)base[k];
		start[k] = 0;
	} else {
		copy_name(v, start, WM_LABEL_MAX);
	}
	host = dt_host_of(&w);
	wm_obj_quiet(TRUE);		/* the keys that answer are the panel's, not the program's */
	k = ( host != NULL && dt_ask_name(host, "新しい実身の名称を入力してください", start, name, WM_LABEL_MAX) );
	wm_obj_quiet(FALSE);
	if ( !k ) {
		return TRUE;
	}
	if ( om_store_copy(&v->target, name, TRUE, &nid) < E_OK ) {
		(void)wm_msg_put("実身を作れませんでした");
		return TRUE;
	}
	if ( base != NULL ) {
		(void)om_store_set_maker(&nid);	/* made by the user, from the template */
		tm_printf((UB *)"desktop: %s made from the template %s\n", name, base);
	}
	c = *v;
	c.target = nid;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.name[0] = 0;
	c.dlen = 0;
	ed_new_link_fit(&c);
	(void)dt_drop_links(d, sx, sy, &c, 1);
	return TRUE;
}

/*
 * A copy of the object a link points at, made because the link was
 * pressed twice and carried: the name is asked for, the object is
 * copied with everything it holds, and a link to the copy is put where
 * the hand let go, looking as the pressed one did.
 */
LOCAL void make_copy( DTWIN *d, DTWIN *t, CONST T_VOBJ *v )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX], own[TAD_NAME_MAX];
	T_DPRECT	from, to;
	T_WMWIN		wf, wt;
	TS_UUID		nid, id;
	T_VOBJ		c;
	INT		first, x, y;
	CONST char	*base;

	if ( t == NULL || ( t->fig == NULL && t->doc == NULL ) || t->sealed || t->xml_view
	  || wm_ref(d->wid, &wf) < E_OK || wm_ref(t->wid, &wt) < E_OK ) {
		return;				/* dropped where nothing takes it */
	}
	base = tmpl_name(d, v, own, TAD_NAME_MAX);
	{
		/*
		 * A new object from a template starts with the template's
		 * name; a copy of an object, with the object's name and
		 * 「のコピー」.
		 */
		if ( base != NULL ) {
			INT	k;

			for ( k = 0; base[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) {
				start[k] = (UB)base[k];
			}
			start[k] = 0;
		} else {
			copy_name(v, start, WM_LABEL_MAX);
		}
	}
	if ( !dt_ask_name(t, "新しい実身の名称を入力してください", start,
			  name, WM_LABEL_MAX) ) {
		return;
	}
	if ( om_store_copy(&v->target, name, TRUE, &nid) < E_OK ) {
		(void)wm_msg_put("実身を作れませんでした");
		return;
	}
	if ( base != NULL ) {
		(void)om_store_set_maker(&nid);	/* made by the user, from the template */
		tm_printf((UB *)"desktop: %s made from the template %s\n", name, base);
	}
	if ( t->fig == NULL ) {
		/* into a text: where the outline's corner was let go */
		c = *v;
		c.target = nid;
		knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
		c.name[0] = 0;
		c.width = v->right - v->left;
		c.heightpx = v->bottom - v->top;
		c.left = c.top = c.right = c.bottom = c.height = 0;
		ed_new_link_fit(&c);
		(void)dd_link_drop(t, wf.work.left + hold_box.left + hold_dx + 2,
				   wf.work.top + hold_box.top + hold_dy + 2, &c);
		return;
	}

	/* where the outline was let go, in that record's own terms */
	dt_work_rect(d, &from);
	dt_work_rect(t, &to);
	x = ( wf.work.left + hold_box.left + hold_dx )
	  - ( wt.work.left + to.left - t->scroll_x );
	y = ( wf.work.top + hold_box.top + hold_dy )
	  - ( wt.work.top + to.top - t->scroll_y );

	c = *v;
	c.target = nid;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.name[0] = 0;
	c.right  = x + ( v->right - v->left );
	c.bottom = y + ( v->bottom - v->top );
	c.left   = x;
	c.top    = y;
	ed_new_link_fit(&c);

	before(t);
	first = tad_lnk_count(t->rec);
	if ( tad_lnk_add((T_TAD *)t->rec, &c, &id) < E_OK ) {
		return;
	}
	ed_model(t);
	pick_from(t, first);
	after(t);
}

/*
 * A link put into a window's record where an outline carried from a
 * program's window was let go, its corner at (sx, sy) on the screen: in
 * a figure or a cabinet at that place, in a text where the letters
 * there are. FALSE when the window takes no links.
 */
EXPORT BOOL ed_place_link_of( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v, INT *p_first )
{
	T_DPRECT	to;
	T_WMWIN		wt;
	TS_UUID		id;
	T_VOBJ		c;
	INT		first, x, y, w, h;

	if ( t == NULL || ( t->fig == NULL && t->doc == NULL ) || t->sealed || t->xml_view
	  || wm_ref(t->wid, &wt) < E_OK ) {
		return FALSE;
	}
	w = v->right - v->left;
	h = v->bottom - v->top;
	c = *v;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	if ( t->fig == NULL ) {
		c.width = w;
		c.heightpx = h;
		c.left = c.top = c.right = c.bottom = c.height = 0;
		return (BOOL)( dd_link_drop(t, sx + 2, sy + 2, &c) >= E_OK );
	}
	dt_work_rect(t, &to);
	x = sx - ( wt.work.left + to.left - t->scroll_x );
	y = sy - ( wt.work.top + to.top - t->scroll_y );
	c.left = x;
	c.top = y;
	c.right = x + w;
	c.bottom = y + h;

	if ( *p_first < 0 ) {
		before(t);
		*p_first = tad_lnk_count(t->rec);
	}
	first = *p_first;
	if ( tad_lnk_add((T_TAD *)t->rec, &c, &id) < E_OK ) {
		return FALSE;
	}
	ed_model(t);
	pick_from(t, first);
	after(t);
	return TRUE;
}

EXPORT BOOL ed_place_link( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v )
{
	INT	first = -1;

	return ed_place_link_of(t, sx, sy, v, &first);
}

/*
 * A link put into another window's record where the carried outline
 * was let go, looking as the carried one did, with an identity of its
 * own. The object it points at is the same: nothing is copied.
 */
EXPORT void ed_drop_link( DTWIN *d, DTWIN *t, CONST T_VOBJ *v )
{
	T_DPRECT	to;
	T_WMWIN		wf, wt;
	TS_UUID		id;
	T_VOBJ		c;
	INT		first, x, y;

	if ( t == NULL || t->fig == NULL || t->sealed
	  || wm_ref(d->wid, &wf) < E_OK || wm_ref(t->wid, &wt) < E_OK ) {
		return;				/* dropped where no figure takes it */
	}
	dt_work_rect(t, &to);
	x = ( wf.work.left + hold_box.left + hold_dx )
	  - ( wt.work.left + to.left - t->scroll_x );
	y = ( wf.work.top + hold_box.top + hold_dy )
	  - ( wt.work.top + to.top - t->scroll_y );

	c = *v;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.right  = x + ( v->right - v->left );
	c.bottom = y + ( v->bottom - v->top );
	c.left   = x;
	c.top    = y;

	before(t);
	first = tad_lnk_count(t->rec);
	if ( tad_lnk_add((T_TAD *)t->rec, &c, &id) < E_OK ) {
		return;
	}
	ed_model(t);
	pick_from(t, first);
	after(t);
}

/*
 * The links a program took out of this window: they leave its record,
 * as one change, as they leave it carried into a window of the desktop's
 */
LOCAL void drop_gone( DTWIN *d, CONST T_VOBJ *v, INT n )
{
	INT	i;

	if ( d->sealed || d->rec == NULL || d->fig == NULL ) {
		return;
	}
	before(d);
	for ( i = 0; i < n; i++ ) {
		(void)tad_lnk_del((T_TAD *)d->rec, &v[i].vobjid);
	}
	ed_pick_none(d);
	after(d);
}

/*
 * The links taken, carried into a text of the desktop's and let go
 * there: each put in the text where the letters under its corner are,
 * as a link carried from a program's window is put (ed_place_link_of),
 * and taken out of this record unless the second button went down on the
 * way (a copy). A link that is 固定化 or 背景化, or one to the object the
 * text shows, stays and is not carried. A text that takes no links (one
 * shown as xmlTAD, a sealed page) takes none, and nothing leaves here.
 */
LOCAL void carry_into_text( DTWIN *d, DTWIN *t )
{
	T_VOBJ		*vs;
	T_DPRECT	b;
	T_WMWIN		w;
	INT		i, n = 0, first = -1;

	if ( wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	vs = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * (SZ)( d->fig->nsh + 1 ));
	if ( vs == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !is_link(d, i) || is_protected(d, i) || !shape_box(d, i, &b)
		  || tad_lnk_get(d->rec, d->fig->sh[i].link, &vs[n]) < E_OK
		  || ts_uuid_cmp(&vs[n].target, &t->id) == 0 ) {
			continue;
		}
		if ( ed_place_link_of(t, b.left + hold_dx + w.work.left, b.top + hold_dy + w.work.top,
				      &vs[n], &first) ) {
			n++;
		}
	}
	if ( n == 0 ) {
		(void)wm_msg_put("ここには置けません");
	} else {
		dt_drop_keys(t);
		if ( !hold_copy ) {
			drop_gone(d, vs, n);
		}
	}
	Kfree(vs);
	dt_draw_all();
	wm_update();
}

/*
 * The links taken, carried to the program whose window they were let go
 * over: a move, as between the desktop's windows (dt_drop_move). They
 * leave here once that program has taken them, and stay when it would
 * not or the second button went down on the way (a copy). A link that
 * is 固定化 or 背景化, or one to the object the window shows, stays and
 * is not carried. FALSE when that is no such window or nothing taken is
 * a link that may go.
 */
LOCAL BOOL drop_out( DTWIN *d, INT sx, INT sy )
{
	T_VOBJ		*vs;
	T_DPRECT	b;
	T_WMWIN		w;
	TS_UUID		shown;
	INT		i, n = 0, ax = sx, ay = sy;
	BOOL		done, has_shown;

	has_shown = (BOOL)( wm_obj_shown(wm_at(sx, sy), &shown) );
	/* every link taken that may go */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && !is_protected(d, i) ) {
			n++;
		}
	}
	vs = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * (SZ)( n + 1 ));
	if ( vs == NULL ) {
		return FALSE;
	}
	n = 0;
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && !is_protected(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &vs[n]) >= E_OK
		  && !( has_shown && ts_uuid_cmp(&vs[n].target, &shown) == 0 ) ) {
			/*
			 * put where the outline of the first was let go, its
			 * corner, as a link carried between the desktop's
			 * windows is put, and as a program's carried is offered
			 */
			if ( n == 0 && shape_box(d, i, &b) && wm_ref(d->wid, &w) >= E_OK ) {
				ax = b.left + hold_dx + w.work.left;
				ay = b.top + hold_dy + w.work.top;
			}
			n++;
		}
	}
	done = ( n > 0 ) ? dt_drop_move(d, sx, sy, ax, ay, vs, n, hold_copy ? NULL : drop_gone) : FALSE;
	Kfree(vs);

	return done;
}

/*
 * The hand let go, at a place on the screen. What it did is put into
 * the record now -- once, as one change, so that taking it back takes
 * back the whole of it and not one step of the hand at a time.
 */
EXPORT void ed_release( INT sx, INT sy )
{
	DTWIN		*d = hold_d;
	DTWIN		*t;
	INT		i, nkeep = 0;
	T_VOBJ		v;
	T_TADNODE	**keep = NULL;

	if ( d == NULL ) {
		return;
	}
	band_hide();
	t = dt_win_of(wm_at(sx, sy));
	if ( hold_how == HOLD_TWICE || hold_how == HOLD_DUP ) {
		UINT	how = hold_how;
		T_VOBJ	v2 = hold_v;

		/* let go of first: what follows reads events of its own */
		hold_d = NULL;
		hold_how = HOLD_NONE;
		hold_which = -1;
		hold_moved = FALSE;
		band_shown = FALSE;
		wm_update();		/* the outline's last place, taken off */
		if ( how == HOLD_TWICE ) {
			dt_open_vobj(d, &v2);
		} else if ( t != NULL || !copy_drop(d, &v2, sx, sy) ) {
			make_copy(d, t, &v2);
		}
		wm_update();
		return;
	}
	if ( t == NULL && hold_moved && hold_how == HOLD_MOVE && !d->sealed && !d->basebox
	  && d->fig != NULL && drop_out(d, sx, sy) ) {
		/* let go over a program's window: it was offered what was taken */
		hold_d = NULL;
		hold_how = HOLD_NONE;
		hold_which = -1;
		hold_moved = FALSE;
		band_shown = FALSE;
		wm_update();
		return;
	}
	if ( d->accbox && t == d && hold_moved && hold_how == HOLD_MOVE && !hold_copy
	  && is_link(d, hold_which) && tad_lnk_get(d->rec, d->fig->sh[hold_which].link, &v) >= E_OK ) {
		/* within a 小物箱: the accessory goes before the row it was let go over */
		T_DPRECT	page;
		T_VOBJ		b;
		T_WMWIN		wf;
		BOOL		has_b = FALSE;
		INT		fy;

		hold_d = NULL;
		hold_how = HOLD_NONE;
		hold_which = -1;
		hold_moved = FALSE;
		dt_work_rect(d, &page);
		fy = ( wm_ref(d->wid, &wf) >= E_OK ) ? unz(d, sy - wf.work.top - page.top + d->scroll_y) : 0;
		for ( i = 0; i < d->fig->nsh && !has_b; i++ ) {
			T_DPRECT r = d->fig->sh[i].r;

			if ( is_link(d, i) && fy < ( r.top + r.bottom ) / 2
			  && tad_lnk_get(d->rec, d->fig->sh[i].link, &b) >= E_OK ) {
				has_b = TRUE;
			}
		}
		dt_acc_place(&v.target, has_b ? &b.target : NULL);
		wm_update();
		return;
	}
	if ( d->sealed || ( d->basebox && hold_moved && hold_how == HOLD_MOVE && t != d ) ) {
		/*
		 * Out of a 原紙箱 to another window: a new object made from
		 * the template carried, where it is let go; within it, the
		 * link moves as in any box. Out of a tool's list: what the
		 * tool makes of it (dttool.c). Nothing inside a sealed page
		 * moves.
		 */
		if ( hold_moved && hold_how == HOLD_MOVE && t != NULL
		  && t != d && is_link(d, hold_which)
		  && tad_lnk_get(d->rec, d->fig->sh[hold_which].link,
				 &v) >= E_OK ) {
			hold_d = NULL;
			hold_how = HOLD_NONE;
			dt_drop_keys(t);
			if ( d->tool != DT_TOOL_NONE ) {
				dt_tool_carry(d, t, &v);
			} else {
				make_copy(d, t, &v);
			}
		} else if ( hold_moved && hold_how == HOLD_MOVE && t == NULL && d->tool == DT_TOOL_NONE
			 && is_link(d, hold_which)
			 && tad_lnk_get(d->rec, d->fig->sh[hold_which].link, &v) >= E_OK ) {
			/* over a program's window: the new object's link dropped there */
			hold_d = NULL;
			hold_how = HOLD_NONE;
			wm_update();
			(void)copy_drop(d, &v, sx, sy);
		}
		dt_draw(d);
	} else if ( hold_moved && hold_how == HOLD_MOVE && t != NULL && t != d
	  && t->fig != NULL && d->fig != NULL && !t->sealed ) {
		dt_drop_keys(t);
		carry_over(d, t, hold_copy);
	} else if ( hold_moved && hold_how == HOLD_MOVE && t != NULL && t != d
	  && t->doc != NULL && d->fig != NULL ) {
		carry_into_text(d, t);
	} else if ( hold_moved && d->fig != NULL ) {
		T_DPRECT	page;
		INT		ox, oy;

		dt_work_rect(d, &page);
		ox = page.left - d->scroll_x;
		oy = page.top - d->scroll_y;

		switch ( hold_how ) {
		case HOLD_MOVE:
			before(d);
			keep = NULL;
			nkeep = 0;
			if ( hold_copy ) {
				/*
				 * Copies stay behind and what was taken moves. The
				 * copies go in under what they copy, which puts the
				 * shapes' numbers out: what was taken is known by
				 * its element, to be taken again after.
				 */
				keep = (T_TADNODE **)Kmalloc(sizeof(T_TADNODE *)
							     * (SZ)( d->fig->nsh + 1 ));
				for ( i = 0; keep != NULL && i < d->fig->nsh; i++ ) {
					if ( d->pick[i] != 0 && d->fig->sh[i].node != NULL ) {
						keep[nkeep++] = d->fig->sh[i].node;
					}
				}
				df_dup_picked(d);
			}
			for ( i = 0; i < d->fig->nsh; i++ ) {
				if ( d->pick[i] != 0 && df_takeable(d, i) ) {
					df_shift(d, i, unz(d, hold_dx), unz(d, hold_dy));
					continue;
				}
				if ( d->pick[i] == 0 || !is_link(d, i)
				  || is_protected(d, i)
				  || tad_lnk_get(d->rec, d->fig->sh[i].link,
						 &v) < E_OK ) {
					continue;
				}
				v.left += unz(d, hold_dx);  v.right += unz(d, hold_dx);
				v.top += unz(d, hold_dy);   v.bottom += unz(d, hold_dy);
				(void)link_put(d, &v);
			}
			after(d);
			if ( keep != NULL ) {
				INT	k;

				ed_pick_none(d);
				for ( i = 0; i < d->fig->nsh; i++ ) {
					for ( k = 0; k < nkeep; k++ ) {
						if ( d->fig->sh[i].node == keep[k] && d->pick[i] == 0 ) {
							pick_add(d, i);
						}
					}
				}
				Kfree(keep);
				dt_draw(d);
				wm_update();
			}
			break;

		case HOLD_W:
		case HOLD_H:
		case HOLD_WH:
			if ( df_takeable(d, hold_which) ) {
				T_DPRECT	nb;

				nb.left   = unz(d, hold_box.left - ox);
				nb.top    = unz(d, hold_box.top - oy);
				nb.right  = unz(d, hold_box.right - ox);
				nb.bottom = unz(d, hold_box.bottom - oy);
				before(d);
				df_resize_flip(d, hold_which, &nb, hold_moved ? hold_flip : 0);
				after(d);
			} else if ( is_link(d, hold_which)
			  && tad_lnk_get(d->rec, d->fig->sh[hold_which].link,
					 &v) >= E_OK ) {
				before(d);
				v.left   = unz(d, hold_box.left - ox);
				v.top    = unz(d, hold_box.top - oy);
				v.right  = unz(d, hold_box.right - ox);
				v.bottom = unz(d, hold_box.bottom - oy);
				/* closed at the closed height; opened only when well past it */
				v.bottom = v.top + tad_lnk_fit_h(&v, v.bottom - v.top);
				v.height = v.bottom - v.top;
				(void)link_put(d, &v);
				after(d);
			}
			break;

		case HOLD_BAND: {
			T_DPRECT	band, b;

			band.left   = ( hold_x0 < hold_px ) ? hold_x0 : hold_px;
			band.right  = ( hold_x0 < hold_px ) ? hold_px : hold_x0;
			band.top    = ( hold_y0 < hold_py ) ? hold_y0 : hold_py;
			band.bottom = ( hold_y0 < hold_py ) ? hold_py : hold_y0;
			for ( i = 0; i < d->fig->nsh; i++ ) {
				if ( !takeable(d, i) || !shape_box(d, i, &b) ) {
					continue;
				}
				/* touched by the band at all counts */
				if ( b.right < band.left || b.left > band.right
				  || b.bottom < band.top || b.top > band.bottom ) {
					continue;
				}
				if ( hold_add ) {
					ed_pick_toggle(d, i);
				} else {
					pick_add(d, i);
				}
			}
			df_whole_groups(d);
			dt_draw(d);
			break;
		}

		default:
			break;
		}
	}
	hold_d = NULL;
	hold_how = HOLD_NONE;
	hold_which = -1;
	hold_moved = FALSE;
	band_shown = FALSE;
	wm_update();
}
