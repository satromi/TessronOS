/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtdoc.c
 *	Editing the text of a document in a window of the desktop (design
 *	17.12)
 *
 *	What is done to the text is the editor's code (application/doc/
 *	docedit.c), the same code a process edits text with. A window is
 *	what it works on: the window's record, model and the way it is
 *	shown, which the editor reads and changes in place, and the caret
 *	and the selection the window keeps for it (DTWIN's dd). What the
 *	editor asks of whoever shows the text is done here with the
 *	desktop's own: drawing the window, taking back, the panels, the
 *	tray, the converter of かな漢字 (dttip.c), and what is under the
 *	hand where something carried out of a text is let go.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/docmenu.h>
#include <ts/part.h>
#include <ts/fn.h>
#include <ts/uuid.h>
#include <ts/tray.h>
#include <ts/sysdef.h>
#include <ts/udic.h>
#include <ts/docedit.h>
#include "desktop.h"

#define DOC_BASE	"019a1132-762b-7b02-ba2a-a918a9b37c39"	/* 仮身化's new object: a document */
#define SETTLE_MS	20		/* laying in one turn of the loop, at most */
#define SETTLE_DRAW	25		/* turns between two drawings of the scroll bar */

/* The caret's turn to be hidden, when it blinks (desktop.c turns it) */
EXPORT BOOL	dd_caret_hidden = FALSE;

LOCAL CONST T_DDHOST	dd_desk;

#define DW(e)	( (DTWIN *)(e)->arg )

/*
 * A window as what the editor works on. Where the window keeps what
 * the editor reads and changes is said again on every call, so that
 * the editor finds it wherever the window is.
 */
LOCAL T_DDED *dd_of( DTWIN *d )
{
	T_DDED	*e = &d->dd;

	e->rec = &d->rec;
	e->doc = &d->doc;
	e->xrec = &d->xrec;
	e->scroll_x = &d->scroll_x;
	e->scroll_y = &d->scroll_y;
	e->height = &d->height;
	e->kind = &d->kind;
	e->xml_view = &d->xml_view;
	e->detail_view = &d->detail_view;
	e->dirty = &d->dirty;
	e->sealed = &d->sealed;
	e->used = &d->used;
	e->settling = &d->settling;
	e->id = &d->id;
	e->recno = &d->recno;
	e->framed = (BOOL)( d->tb_host != NULL );
	e->host = &dd_desk;
	e->arg = d;
	return e;
}

/* ---------------------------------------------------------------- what the editor asks */

LOCAL void h_model( T_DDED *e )
{
	ed_model(DW(e));
}

LOCAL void h_changed( T_DDED *e )
{
	DTWIN	*d = DW(e);

	if ( d->tb_host != NULL ) {
		ed_model(d->tb_host);
		d->tb_host->dirty = TRUE;
	}
}

LOCAL void h_draw( T_DDED *e )
{
	dt_draw(DW(e));
}

LOCAL void h_draw_all( T_DDED *e )
{
	(void)e;
	dt_draw_all();
}

LOCAL void h_update( T_DDED *e )
{
	(void)e;
	wm_update();
}

/*
 * What was drawn in the window laid on the screen: all of its work area
 * is marked as changed, and only that is built again.
 */
LOCAL void h_shown( T_DDED *e )
{
	DTWIN		*d = DW(e);
	T_WMWIN		w;
	T_DPRECT	r;

	if ( wm_ref(d->wid, &w) >= E_OK ) {
		r.left = 0;
		r.top = 0;
		r.right = w.work.right - w.work.left;
		r.bottom = w.work.bottom - w.work.top;
		wm_damage(d->wid, &r);
	}
	wm_update();
}

/*
 * A part of the work area drawn again: the window drawn with the
 * environment's visible area narrowed to it, so that only the lines in
 * it are laid and drawn, and only that part built again on the screen.
 */
LOCAL void h_draw_band( T_DDED *e, CONST T_DPRECT *band )
{
	DTWIN		*d = DW(e);
	T_WMWIN		w;
	T_DPRECT	all, r = *band;
	INT		gid = wm_gid(d->wid);

	if ( gid < 0 || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	all.left = 0;
	all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	if ( r.left < all.left )     r.left = all.left;
	if ( r.top < all.top )       r.top = all.top;
	if ( r.right > all.right )   r.right = all.right;
	if ( r.bottom > all.bottom ) r.bottom = all.bottom;
	if ( r.right <= r.left || r.bottom <= r.top ) {
		return;
	}
	(void)dp_set_visible(gid, &r);
	dt_draw(d);
	(void)dp_set_visible(gid, &all);
	wm_update();
}

LOCAL void h_work_rect( T_DDED *e, T_DPRECT *page )
{
	dt_work_rect(DW(e), page);
}

LOCAL BOOL h_origin( T_DDED *e, INT *p_x, INT *p_y )
{
	T_WMWIN	w;

	if ( wm_ref(DW(e)->wid, &w) < E_OK ) {
		return FALSE;
	}
	*p_x = w.work.left;
	*p_y = w.work.top;
	return TRUE;
}

/* Only the window typed into shows the caret */
LOCAL BOOL h_caret_on( T_DDED *e )
{
	return (BOOL)( wm_focused() == DW(e)->wid && !dd_caret_hidden );
}

LOCAL INT h_look( T_DDED *e, UINT num, INT dflt )
{
	(void)e;
	return wm_num(num, dflt);
}

LOCAL void h_drag( T_DDED *e, CONST T_DPRECT *r, BOOL first, INT dx, INT dy )
{
	(void)e;
	if ( r == NULL ) {
		wm_set_drag(NULL, 0);
		return;
	}
	if ( first ) {
		dt_carry_looks(r, 1, dx, dy);
	}
	wm_set_drag(r, 1);
}

LOCAL void h_note( T_DDED *e, CONST char *s )
{
	(void)e;
	tm_printf((UB *)"desktop: %s\n", s);
}

/* A piece of text in a figure is a change to the figure */
LOCAL void h_before( T_DDED *e )
{
	DTWIN	*d = DW(e);

	ed_before(( d->tb_host != NULL ) ? d->tb_host : d);
}

LOCAL void h_forget( T_DDED *e )
{
	ed_forget(DW(e));
}

LOCAL void h_undo( T_DDED *e )
{
	ed_undo(DW(e));
}

LOCAL void h_tell( T_DDED *e, CONST char *l1, CONST char *l2 )
{
	dt_tell(DW(e), l1, l2);
}

LOCAL void h_no_memory( T_DDED *e )
{
	dt_no_memory(DW(e));
}

LOCAL BOOL h_ask_text( T_DDED *e, CONST char *p1, CONST char *p2,
		       CONST UB *start, UB *out, INT max, CONST char *ok )
{
	return dt_ask_text(DW(e), p1, p2, start, out, max, ok);
}

LOCAL BOOL h_ask_name( T_DDED *e, CONST char *prompt, CONST UB *start, UB *out, INT max )
{
	return dt_ask_name(DW(e), prompt, start, out, max);
}

LOCAL BOOL h_fields_form( T_DDED *e, CONST char *title, INT n, CONST char *CONST *labels,
			  CONST char *CONST *hints, UB (*vals)[DE_FIELD_MAX] )
{
	return dt_fields_form(DW(e), title, n, labels, hints, vals);
}

LOCAL INT h_list_form( T_DDED *e, CONST char *title, CONST char *CONST *names, INT n, INT start )
{
	return dt_list_form(DW(e), title, names, n, start);
}

LOCAL INT h_paper_form( T_DDED *e, T_DEPAPER *pp )
{
	return dt_paper_form(DW(e), pp);
}

LOCAL INT h_find_form( T_DDED *e, UB *what, UB *with, INT max, BOOL *p_regex )
{
	return dt_find_form(DW(e), what, with, max, p_regex);
}

LOCAL BOOL h_pick_colour( T_DDED *e, UW *io )
{
	return dt_pick_colour(DW(e)->wid, io);
}

/* The faces there are, by their family names */
LOCAL INT h_faces( T_DDED *e, UB (*names)[64], INT max )
{
	INT	i, n = 0;

	(void)e;
	for ( i = 0; i < fn_nface() && n < max; i++ ) {
		if ( fn_family(fn_face_at(i), names[n], 64) > 0 ) {
			n++;
		}
	}
	return n;
}

/* The text input port sees the keys first (dttip.c) */
LOCAL BOOL h_ime_key( T_DDED *e, UINT code, UINT mods )
{
	return tip_key(DW(e), code, mods);
}

LOCAL void h_ime_draw( T_DDED *e, INT gid )
{
	tip_draw(DW(e), gid);
}

LOCAL BOOL h_ime_on( T_DDED *e )
{
	(void)e;
	return tip_japanese();
}

LOCAL ER h_tray_put( T_DDED *e, CONST T_TAD *frag, CONST char *name )
{
	(void)e;
	return dt_tray_put(frag, name);
}

LOCAL T_TAD *h_tray_frag( T_DDED *e )
{
	(void)e;
	return dt_tray_frag();
}

/* The pictures the set carries (画面印刷), beside this text's record */
LOCAL void h_tray_pictures( T_DDED *e, T_TAD *frag )
{
	DTWIN	*d = DW(e);

	(void)dt_tray_pictures(&d->id, d->rec, frag);
}

LOCAL void h_tray_taken( T_DDED *e )
{
	(void)e;
	dt_tray_taken();
}

/*
 * What is under a place on the screen: a text of the desktop, a
 * program's window or another of the desktop's, or nothing.
 */
LOCAL UINT h_at( T_DDED *e, INT sx, INT sy, T_DDED **p_t )
{
	INT	wid = wm_at(sx, sy);
	DTWIN	*t = dt_win_of(wid);

	(void)e;
	*p_t = NULL;
	if ( t == NULL ) {
		return ( wid > 0 ) ? DE_AT_OTHER : DE_AT_NONE;
	}
	if ( t->doc != NULL ) {
		*p_t = dd_of(t);
		return DE_AT_TEXT;
	}
	return DE_AT_OTHER;
}

LOCAL void h_focus( T_DDED *e, T_DDED *t )
{
	(void)e;
	dt_drop_keys(DW(t));
}

/*
 * Letters let go over a program's window that takes letters (OB_E_TEXT):
 * dropped on it as one line, and they stay here as well, for the program
 * may still refuse them. Over a figure: its text, leaving here.
 */
LOCAL ER h_text_away( T_DDED *e, INT sx, INT sy, INT cx, INT cy, UB *s, INT n )
{
	DTWIN	*d = DW(e);
	INT	wid = wm_at(sx, sy), i;
	DTWIN	*t = dt_win_of(wid);
	TS_UUID	u;
	ER	er;

	if ( t == NULL && wid > 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( s[i] == '\n' || s[i] == '\t' ) s[i] = ' ';
		}
		knl_memset(&u, 0, sizeof(u));
		(void)wm_obj_uuid(d->wid, &u);
		er = wm_obj_drop_text(wid, cx, cy, 0, &u, s, n);
		if ( er < E_OK ) {
			(void)wm_msg_put("ここには置けません");
			return er;
		}
		return 0;
	}
	if ( t != NULL && t != d && t->doc == NULL && df_is(t) ) {
		er = df_text_drop(t, cx, cy, s, n);
		if ( er < E_OK ) {
			(void)wm_msg_put("ここには置けません");
			return er;
		}
		dt_drop_keys(t);
		return 1;
	}
	(void)wm_msg_put("ここには置けません");
	return E_PAR;
}

LOCAL void num_word( INT v, UB *out )
{
	char	t[12];
	INT	k = 0, n = 0;

	if ( v < 0 ) {
		out[n++] = '-';
		v = -v;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 ) {
		out[n++] = (UB)t[--k];
	}
	out[n] = 0;
}

LOCAL void box_put( T_TAD *rec, T_TADNODE *nd, INT l, INT t, INT r, INT b )
{
	UB	num[16];

	num_word(l, num);
	(void)tad_set_attr(rec, nd, "left", num);
	num_word(t, num);
	(void)tad_set_attr(rec, nd, "top", num);
	num_word(r, num);
	(void)tad_set_attr(rec, nd, "right", num);
	num_word(b, num);
	(void)tad_set_attr(rec, nd, "bottom", num);
}

LOCAL BOOL is_word( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) {
		return FALSE;
	}
	for ( i = 0; a[i] != 0 && b[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/*
 * A thing of a text let go over something that is not a text: a link
 * offered to a program's window, where it stays; a link, a picture or a
 * figure put into a figure or a cabinet where it was let go, leaving
 * the text unless it was a copy.
 */
/*
 * A thing of a text put into the figure t (or the cabinet) where it was
 * let go, the hand at sx, sy: a link, a picture or a figure. from is the
 * text it came from, which it leaves unless it was a copy; NULL when it
 * came from a program's text, which takes it out itself. Answers -1 when
 * t does not take it, 1 when it was put there, 0 when t took it but
 * nothing was put.
 */
LOCAL INT thing_into_fig( DTWIN *t, INT sx, INT sy, CONST T_DDTHING *th, T_DDED *from )
{
	T_WMWIN		wt;
	T_DPRECT	page;
	T_VOBJ		v;
	INT		z, w, h, l, tp, put = 0;

	if ( th->node == NULL || t->sealed ) {
		return -1;
	}
	/* an object is not put inside itself */
	if ( th->kind == TV_RUN_LINK && ts_uuid_cmp(&th->v.target, &t->id) == 0 ) {
		return -1;
	}
	dt_drop_keys(t);
	if ( t->fig == NULL || wm_ref(t->wid, &wt) < E_OK ) {
		return -1;
	}
	z = ( t->fig->zoom > 0 ) ? t->fig->zoom : 8;
	w = th->box.right - th->box.left;
	h = th->box.bottom - th->box.top;
	dt_work_rect(t, &page);
	l = ( sx - wt.work.left - page.left + t->scroll_x ) * 8 / z - ( th->x0 - th->box.left );
	tp = ( sy - wt.work.top - page.top + t->scroll_y ) * 8 / z - ( th->y0 - th->box.top );
	ed_before(t);
	if ( from != NULL ) {
		h_before(from);
	}
	if ( th->kind == TV_RUN_LINK ) {
		TS_UUID	id, was = th->v.vobjid;

		v = th->v;
		knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
		v.left = l;
		v.top = tp;
		v.right = l + w;
		v.bottom = tp + h;
		v.height = h;
		v.width = 0;		/* written as the rectangle, not as a width in a text */
		v.heightpx = 0;
		if ( tad_lnk_add((T_TAD *)t->rec, &v, &id) >= E_OK ) {
			put = 1;
			if ( from != NULL && !th->copy ) {
				(void)tad_lnk_del((T_TAD *)DW(from)->rec, &was);
			}
		}
	} else if ( t->figed && th->kind == TV_RUN_FIGURE ) {
		/* a figure in the figure: drawn into the box it was let go in */
		T_TADNODE	*body = tad_body((T_TAD *)t->rec);
		T_TADNODE	*n = ( body != NULL )
				     ? tad_node_copy((T_TAD *)t->rec, body, NULL, th->node) : NULL;
		T_TADNODE	*c;

		for ( c = ( n != NULL ) ? n->first : NULL; c != NULL; c = c->next ) {
			if ( c->kind == TAD_ND_ELEM && is_word(c->name, "figView") ) {
				box_put((T_TAD *)t->rec, c, l, tp, l + w, tp + h);
			}
		}
		if ( n != NULL ) {
			put = 1;
			if ( from != NULL && !th->copy ) {
				tad_node_remove(th->node);
			}
		}
	} else if ( t->figed ) {
		T_TADNODE	*body = tad_body((T_TAD *)t->rec);
		T_TADNODE	*n = ( body != NULL )
				     ? tad_node_copy((T_TAD *)t->rec, body, NULL, th->node) : NULL;

		if ( n != NULL ) {
			box_put((T_TAD *)t->rec, n, l, tp, l + w, tp + h);
			put = 1;
			if ( from != NULL && !th->copy ) {
				tad_node_remove(th->node);
			}
		}
	}
	ed_model(t);
	t->dirty = TRUE;
	return put;
}

LOCAL BOOL h_thing_away( T_DDED *e, INT sx, INT sy, CONST T_DDTHING *th )
{
	DTWIN	*d = DW(e), *t = dt_win_of(wm_at(sx, sy));

	if ( t == NULL ) {
		if ( th->kind == TV_RUN_LINK ) {
			(void)dt_drop_links(d, sx, sy, &th->v, 1);
		}
		return FALSE;
	}
	return (BOOL)( thing_into_fig(t, sx, sy, th, e) >= 0 );
}

/*
 * A picture or a figure a program carried out of its text (DT_F_FRAG):
 * the first element of the fragment's first paragraph, with its box w by
 * h taken at gx, gy, put into the figure t where the hand let go at sx,
 * sy, as one from a text of the desktop's is
 */
EXPORT ER dd_frag_into_fig( DTWIN *t, INT sx, INT sy, CONST T_TAD *frag, INT w, INT h, INT gx, INT gy )
{
	T_TADNODE	*body = tad_body(frag), *p, *n = NULL;
	T_DDTHING	th;

	p = ( body != NULL ) ? body->first : NULL;
	while ( p != NULL && !( p->kind == TAD_ND_ELEM && is_word(p->name, "p") ) ) p = p->next;
	for ( n = ( p != NULL ) ? p->first : NULL; n != NULL && n->kind != TAD_ND_ELEM; n = n->next ) ;
	if ( n == NULL || w <= 0 || h <= 0 ) {
		return E_OBJ;
	}
	knl_memset(&th, 0, sizeof(th));
	th.kind = is_word(n->name, "figure") ? TV_RUN_FIGURE : TV_RUN_IMAGE;
	th.node = n;
	th.box.right = w;
	th.box.bottom = h;
	th.x0 = gx;
	th.y0 = gy;
	th.copy = TRUE;
	return ( thing_into_fig(t, sx, sy, &th, NULL) > 0 ) ? E_OK : E_NOEXS;
}

/* The pieces of a fragment a program carried out of its text (DT_F_FRAG), put in t as they looked */
EXPORT ER dd_frag_drop( DTWIN *t, INT sx, INT sy, CONST T_TAD *frag )
{
	return ( t != NULL ) ? de_frag_drop(dd_of(t), sx, sy, frag) : E_PAR;
}

/* 仮身化: a new document made with the whole of this one's record in it */
LOCAL ER h_new_object( T_DDED *e, CONST UB *name, TS_UUID *p_id, T_TAD **p_rec )
{
	DTWIN	*d = DW(e);
	TS_UUID	base;
	UB	*xml;
	SZ	len = 0;
	ER	er;

	*p_rec = NULL;
	er = ts_str_to_uuid(DOC_BASE, &base);
	if ( er >= E_OK ) {
		er = om_store_copy(&base, name, FALSE, p_id);
	}
	if ( er < E_OK ) {
		return er;
	}
	(void)tad_write_mem(d->rec, NULL, 0, &len);
	if ( len == 0 ) {
		return E_NOEXS;
	}
	xml = (UB *)Kmalloc(len + 1);
	if ( xml == NULL ) {
		return E_NOMEM;
	}
	if ( tad_write_mem(d->rec, xml, len + 1, &len) < E_OK ) {
		Kfree(xml);
		return E_NOMEM;
	}
	*p_rec = om_store_restore(p_id, 0, xml, len);
	Kfree(xml);
	return ( *p_rec != NULL ) ? E_OK : E_NOMEM;
}

LOCAL void h_object_done( T_DDED *e, CONST TS_UUID *id, CONST UB *name )
{
	(void)e;
	(void)om_store_save(id, 0);
	(void)om_store_rename(id, name);
}

LOCAL T_TAD *h_restore( T_DDED *e, CONST UB *xml, SZ n )
{
	DTWIN	*d = DW(e);

	return om_store_restore(&d->id, d->recno, xml, n);
}

/*
 * 単語登録 started with the selection's first line as the word: it is
 * put in the tray's place of its own (UD_TRAY_SUB), where the accessory
 * takes it from as it starts.
 */
LOCAL void h_word_register( T_DDED *e, CONST UB *w, INT n )
{
	DTWIN		*d = DW(e);
	TS_UUID		u;
	CONST DTPROG	*p;
	T_TRREC		r;

	if ( n > UD_WORD_MAX ) {
		n = UD_WORD_MAX;
	}
	if ( n > 0 ) {
		knl_memset(&r, 0, sizeof(r));
		r.kind = TR_REC;
		r.sub = UD_TRAY_SUB;
		r.data = (UB *)w;
		r.size = n;
		(void)tr_set_dat(&r, 1);
	}
	if ( ts_str_to_uuid(SYSDEF_PROG_WORDDIC, &u) < E_OK || ( p = dt_prog_of(&u) ) == NULL
	  || dt_prog_run(d, p, NULL) < E_OK ) {
		dt_tell(d, "単語登録を起動できませんでした。", NULL);
	}
}

LOCAL CONST T_DDHOST	dd_desk = {
	.model = h_model, .changed = h_changed, .draw = h_draw, .draw_all = h_draw_all,
	.update = h_update, .shown = h_shown, .draw_band = h_draw_band,
	.work_rect = h_work_rect, .origin = h_origin,
	.caret_on = h_caret_on, .look = h_look, .drag = h_drag, .note = h_note,
	.before = h_before, .forget = h_forget, .undo = h_undo,
	.tell = h_tell, .no_memory = h_no_memory, .ask_text = h_ask_text,
	.ask_name = h_ask_name, .fields_form = h_fields_form, .list_form = h_list_form,
	.paper_form = h_paper_form, .find_form = h_find_form, .pick_colour = h_pick_colour,
	.faces = h_faces,
	.ime_key = h_ime_key, .ime_draw = h_ime_draw, .ime_on = h_ime_on,
	.tray_put = h_tray_put, .tray_frag = h_tray_frag, .tray_pictures = h_tray_pictures,
	.tray_taken = h_tray_taken,
	.at = h_at, .focus = h_focus, .text_away = h_text_away, .thing_away = h_thing_away,
	.new_object = h_new_object, .object_done = h_object_done, .restore = h_restore,
	.word_register = h_word_register,
};

/* ---------------------------------------------------------------- the desktop's calls */

EXPORT BOOL dd_editable( CONST DTWIN *d )
{
	return (BOOL)( d != NULL && de_editable(dd_of((DTWIN *)d)) );
}

EXPORT BOOL dd_key( DTWIN *d, UINT code, UINT mods )
{
	return de_key(dd_of(d), code, mods);
}

EXPORT void dd_type_text( DTWIN *d, CONST UB *s, INT n )
{
	de_type_text(dd_of(d), s, n);
}

EXPORT void dd_press( DTWIN *d, INT x, INT y, BOOL extend, UD when )
{
	de_press(dd_of(d), x, y, extend, when);
}

EXPORT void dd_follow( INT sx, INT sy )
{
	de_follow(sx, sy);
}

EXPORT BOOL dd_dragging( void )
{
	return de_dragging();
}

EXPORT void dd_release( void )
{
	de_release();
}

EXPORT BOOL dd_mark_press( DTWIN *d, INT x, INT y, UD when )
{
	return (BOOL)( d != NULL && de_mark_press(dd_of(d), x, y, when) );
}

EXPORT BOOL dd_mark_carrying( void )
{
	return de_mark_carrying();
}

EXPORT void dd_mark_follow( INT sx, INT sy )
{
	de_mark_follow(sx, sy);
}

EXPORT void dd_mark_release( void )
{
	de_mark_release();
}

EXPORT BOOL dd_sel_carrying( void )
{
	return de_sel_carrying();
}

EXPORT void dd_sel_copy( void )
{
	de_sel_copy();
}

EXPORT void dd_sel_release( INT sx, INT sy )
{
	de_sel_release(sx, sy);
}

EXPORT ER dd_text_drop( DTWIN *t, INT sx, INT sy, CONST UB *s, INT n )
{
	return ( t != NULL ) ? de_text_drop(dd_of(t), sx, sy, s, n) : E_PAR;
}

EXPORT void dd_link_take( DTWIN *d, INT which, CONST T_DPRECT *box, INT x, INT y, UD when )
{
	de_link_take(dd_of(d), which, box, x, y, when);
}

EXPORT BOOL dd_link_carrying( void )
{
	return de_link_carrying();
}

EXPORT void dd_link_follow( INT sx, INT sy )
{
	de_link_follow(sx, sy);
}

EXPORT void dd_link_copy( void )
{
	de_link_copy();
}

EXPORT void dd_link_release( INT sx, INT sy )
{
	de_link_release(sx, sy);
}

EXPORT BOOL dd_link_picked( DTWIN *d, T_VOBJ *v )
{
	return de_link_picked(dd_of(d), v);
}

EXPORT ER dd_link_drop( DTWIN *t, INT sx, INT sy, CONST T_VOBJ *v )
{
	return ( t != NULL ) ? de_link_drop(dd_of(t), sx, sy, v) : E_PAR;
}

EXPORT void dd_link_after( DTWIN *d, CONST TS_UUID *orig )
{
	de_link_after(dd_of(d), orig);
}

EXPORT void dd_draw( DTWIN *d, INT gid )
{
	de_draw(dd_of(d), gid);
}

EXPORT void dd_show_caret( DTWIN *d )
{
	de_show_caret(dd_of(d));
}

/* A figure's window with a piece of its text being edited blinks that one's caret */
EXPORT BOOL dd_caret_rect( DTWIN *d, T_DPRECT *r )
{
	if ( d == NULL || d->tb != NULL ) {
		return FALSE;
	}
	return de_caret_rect(dd_of(d), r);
}

EXPORT INT dd_caret_px( DTWIN *d )
{
	return de_caret_px(dd_of(d));
}

EXPORT BOOL dd_command( DTWIN *d, INT cmd )
{
	return de_command(dd_of(d), cmd);
}

EXPORT void dd_menu_state( DTWIN *d, BOOL *p_comb, BOOL *p_turned )
{
	de_menu_state(dd_of(d), p_comb, p_turned);
}

EXPORT void dd_look( DTWIN *d, INT cmd, BOOL on )
{
	de_look(dd_of(d), cmd, on);
}

EXPORT void dd_font( DTWIN *d, CONST char *attr, CONST UB *value )
{
	de_font(dd_of(d), attr, value);
}

EXPORT void dd_pick_all( DTWIN *d )
{
	de_pick_all(dd_of(d));
}

EXPORT BOOL dd_has_sel( DTWIN *d )
{
	return de_has_sel(dd_of(d));
}

EXPORT INT dd_recent_faces( UB (*out)[WM_LABEL_MAX], INT max )
{
	return de_recent_faces(out, max);
}

EXPORT void dd_view( DTWIN *d )
{
	de_view(dd_of(d));
}

EXPORT void dd_xml_enter( DTWIN *d )
{
	de_xml_enter(dd_of(d));
}

EXPORT BOOL dd_xml_commit( DTWIN *d )
{
	return de_xml_commit(dd_of(d));
}

EXPORT BOOL dd_xml_leave( DTWIN *d )
{
	return de_xml_leave(dd_of(d));
}

/* Whether the set in hand in the tray is text */
EXPORT BOOL dd_clip_has( void )
{
	return (BOOL)( dt_tray_kind(NULL) == DT_TRAY_DOC );
}

/* ---------------------------------------------------------------- laying while nothing is done */

LOCAL INT	dd_settle_turns = 0;
LOCAL INT	dd_settle_left[DT_MAX_WIN];

EXPORT BOOL dd_settle_pending( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		CONST DTWIN	*d = dt_win_at(i);

		if ( d->used && d->settling && d->doc != NULL && d->doc->layc != NULL ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* One window a turn: a few more of its paragraphs laid */
EXPORT void dd_settle_step( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = dt_win_at(i);
		INT	left, shift, laid, was;

		if ( !d->used || !d->settling || d->doc == NULL || d->doc->layc == NULL ) {
			continue;
		}
		left = de_settle(dd_of(d), SETTLE_MS, &shift, &laid);
		was = dd_settle_left[i];
		dd_settle_left[i] = left;
		/* the last laid, the first time round too: drawn, the bars as tall as the text */
		if ( shift != 0 || ( left == 0 && ( was > 0 || laid > 0 ) )
		  || ( left > 0 && ++dd_settle_turns % SETTLE_DRAW == 0 ) ) {
			dt_draw(d);
			wm_update();
			if ( left == 0 ) {
				d->settling = FALSE;	/* drawn with nothing left: done */
			}
		}
		return;
	}
}
