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
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/part.h>
#include <ts/fn.h>
#include <ts/tray.h>
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

	ed_forget(d);
	if ( d->rec == NULL ) {
		return;
	}
	if ( d->tb_host != NULL ) {
		/* a piece of text in a figure: its own <document> element */
		(void)tv_doc_of(d->tb_node, &d->doc);
		return;
	}
	if ( d->kind == TV_KIND_DOC ) {
		/* in 原稿, the record's text is what is shown and edited */
		tv_doc(( d->xml_view && d->xrec != NULL ) ? d->xrec : d->rec,
		       &d->doc);
		if ( d->doc != NULL && !d->xml_view ) {
			d->doc->detail = d->detail_view;
			d->doc->show_hidden = d->show_hidden;
			d->doc->page_h = !d->paper_frame ? 0
				       : ( d->doc->paper_h > 0 ) ? d->doc->paper_h
				       : DT_PAPER_H;
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
	d->npick = 0;
	for ( i = 0; i < TV_MAX_SHAPE; i++ ) {
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
#define ED_PATH_MAX	( TV_MAX_PT * 8 + 64 )	/* the points of a line's drawn path */

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
		T_DPPOINT	*pts = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * ED_PATH_MAX);
		INT		n = ( pts != NULL ) ? tv_line_path(d->fig, &d->fig->sh[i], pts, ED_PATH_MAX) : 0, k;

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

LOCAL void stack_push( UB **stk, SZ *len, INT *n, UB *xml, SZ size )
{
	if ( *n == DT_UNDO_MAX ) {
		INT	i;

		Kfree(stk[0]);			/* the oldest goes */
		for ( i = 1; i < DT_UNDO_MAX; i++ ) {
			stk[i - 1] = stk[i];
			len[i - 1] = len[i];
		}
		(*n)--;
	}
	stk[*n] = xml;
	len[*n] = size;
	(*n)++;
}

/*
 * About to change the record: keep it as it is now. A new change
 * throws away what had been taken back, because the changes taken back
 * were made to a record that is no longer there.
 */
LOCAL void before( DTWIN *d )
{
	UB	*xml = NULL;
	SZ	size = 0;

	if ( om_store_snapshot(&d->id, d->recno, &xml, &size) >= E_OK ) {
		stack_push(d->undo, d->undo_len, &d->nundo, xml, size);
	}
	stack_clear(d->redo, &d->nredo);
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
	if ( d->pick_seq == 0xFFFF ) {
		d->pick_seq = 0xFFFE;		/* past counting: they tie */
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
	wm_composite();
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
	stack_push(to, to_len, nto, now, now_len);
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
	return (BOOL)( i >= 0 && i < TV_MAX_SHAPE && d->pick[i] != 0 );
}

EXPORT void ed_pick_none( DTWIN *d )
{
	INT	i;

	for ( i = 0; i < TV_MAX_SHAPE; i++ ) {
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
	/* what is protected is not taken with everything else */
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( takeable(d, i) && !is_protected(d, i) ) {
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
	T_DPRECT	b;

	if ( gid < 0 || d->fig == NULL || d->npick == 0 ) {
		return;
	}
	if ( ed_blink_off && df_is(d) && !ed_holding() ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || !shape_box(d, i, &b) ) {
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
 * the shape it sizes, or -1.
 */
LOCAL INT handle_at( DTWIN *d, INT x, INT y )
{
	T_DPRECT	b;
	INT		i;

	if ( !df_is(d) ) {
		return -1;
	}
	for ( i = 0; i < d->fig->nsh; i++ ) {
		if ( d->pick[i] == 0 || is_link(d, i)
		  || d->fig->sh[i].kind == TV_SH_LINE || !shape_box(d, i, &b) ) {
			continue;
		}
		b.right += 2;
		b.bottom += 2;
		if ( x >= b.right - ED_HANDLE / 2 - 1 && x <= b.right + ED_HANDLE / 2 + 1
		  && y >= b.bottom - ED_HANDLE / 2 - 1 && y <= b.bottom + ED_HANDLE / 2 + 1 ) {
			return i;
		}
	}

	return -1;
}

/* ---------------------------------------------------------------- changes */

EXPORT void ed_delete( DTWIN *d )
{
	TS_UUID	*gone;
	INT	i, n = 0;
	T_VOBJ	v;

	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	gone = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)d->npick);
	if ( gone == NULL ) {
		return;
	}
	/* the identities first: deleting one moves the others' numbers */
	for ( i = 0; i < d->fig->nsh && n < d->npick; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i) && !is_protected(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &v) >= E_OK ) {
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
EXPORT void ed_copy( DTWIN *d )
{
	T_TAD	*frag;
	T_VOBJ	v;
	TS_UUID	id;
	UB	name[TR_NAME_MAX];
	INT	i, nl = 0, ns;

	if ( d->fig == NULL || d->npick == 0 ) {
		return;
	}
	frag = dt_frag_new("figure");
	if ( frag == NULL ) {
		return;
	}
	ed_pasted = 0;
	ns = df_copy_into(d, frag);
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
		(void)dt_tray_put(frag, (CONST char *)name);
	}
	tad_free(frag);
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
	ed_copy(d);
	ed_delete(d);
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
	er = tad_lnk_reorder((T_TAD *)d->rec, order, n);
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
		tad_lnk_set((T_TAD *)d->rec, &v);
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
	tad_lnk_set((T_TAD *)d->rec, v);
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

typedef struct {
	T_VOBJ	v;
	UH	seq;
} ARITEM;

LOCAL INT ar_cmp_text( CONST UB *a, CONST UB *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}

	return (INT)*a - (INT)*b;
}

/* How two objects compare for 整列順 */
LOCAL INT ar_cmp( CONST ARITEM *a, CONST ARITEM *b, INT by )
{
	T_OMINFO	*ia, *ib;
	INT		c = 0;

	ia = (T_OMINFO *)Kmalloc(sizeof(T_OMINFO) * 2);
	if ( ia == NULL ) {
		return 0;
	}
	ib = ia + 1;
	om_store_info(&a->v.target, ia);
	om_store_info(&b->v.target, ib);
	switch ( by ) {
	case AR_BY_NAME:	c = ar_cmp_text(ia->name, ib->name);		break;
	case AR_BY_MADE:	c = ar_cmp_text(ia->made, ib->made);		break;
	case AR_BY_UPDATED:	c = ar_cmp_text(ia->updated, ib->updated);	break;
	case AR_BY_SIZE:	c = ia->bytes - ib->bytes;			break;
	default:		break;
	}
	Kfree(ia);

	return c;
}

/*
 * How wide an object's frame needs to be to show what 幅調整 asks for:
 * its pictogram and name, then its relationship, then what program it
 * opens with, then the date -- each step taking in what the one before
 * it did.
 */
LOCAL INT ar_width( CONST T_VOBJ *v, INT what )
{
	ID	fid = fn_system();
	INT	chsz = ( v->chsz > 0 ) ? v->chsz : 14;
	INT	w = 10 + chsz + 4;
	UB	text[TAD_NAME_MAX + 8];

	if ( fid <= 0 ) {
		return v->right - v->left;
	}
	fn_set_size(fid, chsz);
	if ( om_store_name(&v->target, text, TAD_NAME_MAX) >= 0 ) {
		w += fn_width(fid, text);
	}
	if ( what != AR_W_NAME ) {
		UB	rel[TAD_NAME_MAX];
		INT	n, i, k = 0;

		n = om_store_relationship(&v->target, rel, TAD_NAME_MAX);
		if ( n > 0 ) {
			text[k++] = ' ';  text[k++] = ':';  text[k++] = ' ';
			for ( i = 0; i < n && k < TAD_NAME_MAX; i++ ) {
				if ( rel[i] != '[' && rel[i] != ']' ) {
					text[k++] = rel[i];
				}
			}
			text[k] = 0;
			w += fn_width(fid, text);
		}
	}
	if ( what == AR_W_NODATE || what == AR_W_FULL ) {
		UB	app[1][OM_APP_NAME];

		if ( om_store_apps(&v->target, app, 1) > 0 ) {
			INT	i, k = 0;

			text[k++] = ' ';  text[k++] = '(';
			for ( i = 0; app[0][i] != 0 && k < TAD_NAME_MAX; i++ ) {
				text[k++] = app[0][i];
			}
			text[k++] = ')';
			text[k] = 0;
			w += fn_width(fid, text);
		}
	}
	if ( what == AR_W_FULL ) {
		w += fn_width(fid, (CONST UB *)" YYYY/MM/DD HH:MM:SS");
	}
	fn_set_size(fid, 14);

	return w + 10;
}

EXPORT void ed_arrange( DTWIN *d, CONST T_ARRANGE *o )
{
	ARITEM	*it, tmp;
	INT	n = 0, i, k, top, left, right, gap, column;

	if ( d->fig == NULL || d->npick == 0 || d->sealed ) {
		return;
	}
	it = (ARITEM *)Kmalloc(sizeof(ARITEM) * (SZ)d->npick);
	if ( it == NULL ) {
		return;
	}
	for ( i = 0; i < d->fig->nsh && n < d->npick; i++ ) {
		/* one that is 固定化 or 背景化 stays where it is */
		if ( d->pick[i] != 0 && !is_protected(d, i) && ed_link_at(d, i, &it[n].v) ) {
			it[n].seq = d->pick[i];
			n++;
		}
	}
	/* in the order they were taken */
	for ( i = 1; i < n; i++ ) {
		for ( k = i; k > 0 && it[k - 1].seq > it[k].seq; k-- ) {
			tmp = it[k];  it[k] = it[k - 1];  it[k - 1] = tmp;
		}
	}
	if ( n == 0 ) {
		Kfree(it);
		return;
	}
	top = it[0].v.top;  left = it[0].v.left;  right = it[0].v.right;
	for ( i = 1; i < n; i++ ) {
		if ( it[i].v.top < top )     top = it[i].v.top;
		if ( it[i].v.left < left )   left = it[i].v.left;
		if ( it[i].v.right > right ) right = it[i].v.right;
	}

	/* 幅調整, before anything is sorted: 最初 is the one taken first */
	if ( o->width != AR_NONE ) {
		INT	first_w = it[0].v.right - it[0].v.left;

		for ( i = 0; i < n; i++ ) {
			INT	w = ( o->width == AR_W_FIRST ) ? first_w
				  : ar_width(&it[i].v, o->width);

			it[i].v.right = it[i].v.left + w;
		}
	}

	if ( n > 1 && o->sort_by != AR_NONE ) {
		for ( i = 1; i < n; i++ ) {
			for ( k = i; k > 0; k-- ) {
				INT	c = ar_cmp(&it[k - 1], &it[k], o->sort_by);

				if ( o->descending ) {
					c = -c;
				}
				if ( c <= 0 ) {
					break;
				}
				tmp = it[k];  it[k] = it[k - 1];  it[k - 1] = tmp;
			}
		}
	}

	gap = ( o->vertical == AR_COMPACT ) ? 5 : 10;
	column = o->column;
	if ( o->sort_by != AR_NONE && column == AR_NONE
	  && o->horizontal == AR_NONE && o->vertical == AR_NONE ) {
		column = AR_SINGLE;	/* an order asked for is a column */
	}

	if ( n > 1 && column == AR_SINGLE ) {
		INT	at = top;

		for ( i = 0; i < n; i++ ) {
			INT	w = it[i].v.right - it[i].v.left;
			INT	h = it[i].v.bottom - it[i].v.top;

			if ( o->horizontal == AR_RIGHT ) {
				it[i].v.right = right;
				it[i].v.left = right - w;
			} else {
				it[i].v.left = left;
				it[i].v.right = left + w;
			}
			it[i].v.top = at;
			it[i].v.bottom = at + h;
			at = it[i].v.bottom + gap;
		}
	} else if ( n > 1 && ( column == AR_MULTI_H || column == AR_MULTI_V ) ) {
		INT	cols = ( o->columns > 0 ) ? o->columns : 2;
		INT	per = ( n + cols - 1 ) / cols;
		INT	*colw, *coll;

		colw = (INT *)Kmalloc(sizeof(INT) * (SZ)cols * 2);
		if ( colw != NULL ) {
			coll = colw + cols;
			for ( k = 0; k < cols; k++ ) {
				colw[k] = 0;
			}
			for ( i = 0; i < n; i++ ) {
				INT	c = ( column == AR_MULTI_H ) ? i % cols : i / per;
				INT	w = it[i].v.right - it[i].v.left;

				if ( c < cols && w > colw[c] ) {
					colw[c] = w;
				}
			}
			coll[0] = left;
			for ( k = 1; k < cols; k++ ) {
				coll[k] = coll[k - 1] + colw[k - 1] + 10;
			}
			if ( column == AR_MULTI_H ) {
				/* left to right, then down a row */
				INT	row_top = top, row_bottom = top;

				for ( i = 0; i < n; i++ ) {
					INT	c = i % cols;
					INT	w = it[i].v.right - it[i].v.left;
					INT	h = it[i].v.bottom - it[i].v.top;

					if ( c == 0 && i > 0 ) {
						row_top = row_bottom + gap;
					}
					it[i].v.left = coll[c];
					it[i].v.right = coll[c] + w;
					it[i].v.top = row_top;
					it[i].v.bottom = row_top + h;
					if ( c == 0 || it[i].v.bottom > row_bottom ) {
						row_bottom = it[i].v.bottom;
					}
				}
			} else {
				/* down a column, then the next column */
				for ( i = 0; i < n; i++ ) {
					INT	c = i / per;
					INT	w = it[i].v.right - it[i].v.left;
					INT	h = it[i].v.bottom - it[i].v.top;

					it[i].v.left = coll[c];
					it[i].v.right = coll[c] + w;
					it[i].v.top = ( i % per == 0 ) ? top
						    : it[i - 1].v.bottom + gap;
					it[i].v.bottom = it[i].v.top + h;
				}
			}
			Kfree(colw);
		}
	} else if ( n > 1 && ( o->horizontal != AR_NONE
			    || o->vertical != AR_NONE ) ) {
		/* only lined up: taken from the top down */
		for ( i = 1; i < n; i++ ) {
			for ( k = i; k > 0 && it[k - 1].v.top > it[k].v.top; k-- ) {
				tmp = it[k];  it[k] = it[k - 1];  it[k - 1] = tmp;
			}
		}
		for ( i = 0; i < n; i++ ) {
			INT	w = it[i].v.right - it[i].v.left;
			INT	h = it[i].v.bottom - it[i].v.top;

			if ( o->horizontal == AR_LEFT ) {
				it[i].v.left = left;
				it[i].v.right = left + w;
			} else if ( o->horizontal == AR_RIGHT ) {
				it[i].v.right = right;
				it[i].v.left = right - w;
			}
			if ( i > 0 && o->vertical != AR_NONE ) {
				it[i].v.top = it[i - 1].v.bottom + gap;
				it[i].v.bottom = it[i].v.top + h;
			}
		}
	}

	before(d);
	for ( i = 0; i < n; i++ ) {
		it[i].v.height = it[i].v.bottom - it[i].v.top;
		tad_lnk_set((T_TAD *)d->rec, &it[i].v);
	}
	Kfree(it);
	after(d);
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

LOCAL UINT hold_kind( CONST T_DPRECT *box, INT x, INT y )
{
	BOOL	right = (BOOL)( x >= box->right - ED_EDGE );
	BOOL	foot  = (BOOL)( y >= box->bottom - ED_EDGE );

	if ( right && foot ) return HOLD_WH;
	if ( right )         return HOLD_W;
	if ( foot )          return HOLD_H;
	return HOLD_MOVE;
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

	hold_copy = FALSE;
	dt_work_rect(d, &page);
	/* the handle of a picked shape sizes it, from its foot on the right */
	if ( !add && ( which = handle_at(d, x, y) ) >= 0 && shape_box(d, which, &box)
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
			dd_link_take(d, which, &box, x, y);
			return TRUE;
		}
		/* taking it */
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
		hold_how   = ( d->npick == 1 ) ? hold_kind(&box, x, y) : HOLD_MOVE;
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
		wm_composite();
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
		wm_composite();
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
		hold_dx = x - hold_x0;
		hold_dy = y - hold_y0;
		break;
	case HOLD_MOVE:
	case HOLD_DUP:
		hold_dx += dx;
		hold_dy += dy;
		break;
	case HOLD_W:
		hold_box.right += dx;
		break;
	case HOLD_H:
		hold_box.bottom += dy;
		break;
	case HOLD_WH:
		hold_box.right += dx;
		hold_box.bottom += dy;
		break;
	default:
		break;
	}
	if ( hold_box.right < hold_box.left + ED_MIN ) {
		hold_box.right = hold_box.left + ED_MIN;
	}
	if ( hold_box.bottom < hold_box.top + ED_MIN ) {
		hold_box.bottom = hold_box.top + ED_MIN;
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
 * A copy of the object a link points at, made because the link was
 * pressed twice and carried: the name is asked for, the object is
 * copied with everything it holds, and a link to the copy is put where
 * the hand let go, looking as the pressed one did.
 */
LOCAL void make_copy( DTWIN *d, DTWIN *t, CONST T_VOBJ *v )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX];
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
	base = dt_base_name(&v->target);
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
EXPORT BOOL ed_place_link( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v )
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

	before(t);
	first = tad_lnk_count(t->rec);
	if ( tad_lnk_add((T_TAD *)t->rec, &c, &id) < E_OK ) {
		return FALSE;
	}
	ed_model(t);
	pick_from(t, first);
	after(t);
	return TRUE;
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
 * The links taken, offered to the program whose window they were let
 * go over; they stay where they are here. FALSE when that is no such
 * window or nothing taken is a link.
 */
LOCAL BOOL drop_out( DTWIN *d, INT sx, INT sy )
{
	T_VOBJ	*vs;
	INT	i, n = 0;
	BOOL	done;

	vs = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * OB_DROP_MAX);
	if ( vs == NULL ) {
		return FALSE;
	}
	for ( i = 0; i < d->fig->nsh && n < OB_DROP_MAX; i++ ) {
		if ( d->pick[i] != 0 && is_link(d, i)
		  && tad_lnk_get(d->rec, d->fig->sh[i].link, &vs[n]) >= E_OK ) {
			n++;
		}
	}
	done = ( n > 0 ) ? dt_drop_links(d, sx, sy, vs, n) : FALSE;
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
		wm_composite();		/* the outline's last place, taken off */
		if ( how == HOLD_TWICE ) {
			dt_open_vobj(d, &v2);
		} else {
			make_copy(d, t, &v2);
		}
		wm_composite();
		return;
	}
	if ( t == NULL && hold_moved && hold_how == HOLD_MOVE && !d->sealed
	  && d->fig != NULL && drop_out(d, sx, sy) ) {
		/* let go over a program's window: it was offered what was taken */
		hold_d = NULL;
		hold_how = HOLD_NONE;
		hold_which = -1;
		hold_moved = FALSE;
		band_shown = FALSE;
		wm_composite();
		return;
	}
	if ( d->sealed ) {
		/*
		 * Out of the 原紙箱: a new object made from the template
		 * carried, where it is let go. Out of a tool's list: what
		 * the tool makes of it (dttool.c). Nothing inside moves.
		 */
		if ( hold_moved && hold_how == HOLD_MOVE && t != NULL
		  && t != d && is_link(d, hold_which)
		  && tad_lnk_get(d->rec, d->fig->sh[hold_which].link,
				 &v) >= E_OK ) {
			hold_d = NULL;
			hold_how = HOLD_NONE;
			wm_composite();
			if ( d->tool != DT_TOOL_NONE ) {
				dt_tool_carry(d, t, &v);
			} else {
				make_copy(d, t, &v);
			}
		}
		dt_draw(d);
	} else if ( hold_moved && hold_how == HOLD_MOVE && t != NULL && t != d
	  && t->fig != NULL && d->fig != NULL && !t->sealed ) {
		carry_over(d, t, hold_copy);
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
				tad_lnk_set((T_TAD *)d->rec, &v);
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
				wm_composite();
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
				df_resize(d, hold_which, &nb);
				after(d);
			} else if ( is_link(d, hold_which)
			  && tad_lnk_get(d->rec, d->fig->sh[hold_which].link,
					 &v) >= E_OK ) {
				before(d);
				v.left   = unz(d, hold_box.left - ox);
				v.top    = unz(d, hold_box.top - oy);
				v.right  = unz(d, hold_box.right - ox);
				v.bottom = unz(d, hold_box.bottom - oy);
				v.height = v.bottom - v.top;
				tad_lnk_set((T_TAD *)d->rec, &v);
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
	wm_composite();
}
