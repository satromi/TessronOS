/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dc_host.c
 *	What the editor of text asks of the document's window, in 基本文章編集
 *	running as a process (T_DDHOST, include/ts/docedit.h, design 11.3)
 *
 *	The editor's places are the window's work area: what the window is
 *	told of the pointer is there already, so the work area is where the
 *	"screen" of the editor begins. Drawing is asked for and done when
 *	the notices waiting have been taken (dc_main.c). What is taken back
 *	is the record as it was, kept as text.
 */

#include "dc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/look.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/udic.h>
#include <ts/sysdef.h>
#include <ts/fn.h>

#define UNDO_BYTES	( 64 * 1024 * 1024 )	/* what the kept records may take together */

/* ---------------------------------------------------------------- taking back */

static UB	**u_stk;
static SZ	*u_len;
static INT	u_n, u_max;

static SZ undo_held( void )
{
	SZ	t = 0;
	INT	i;

	for ( i = 0; i < u_n; i++ ) t += u_len[i];
	return t;
}

static void undo_drop_oldest( void )
{
	if ( u_n <= 0 ) return;
	free(u_stk[0]);
	memmove(u_stk, u_stk + 1, sizeof(UB *) * (size_t)( u_n - 1 ));
	memmove(u_len, u_len + 1, sizeof(SZ) * (size_t)( u_n - 1 ));
	u_n--;
}

/* A record as text; NULL when there is no memory for it */
static UB *rec_text( CONST T_TAD *rec, SZ *p_size )
{
	UB	*buf;
	SZ	size = 0;

	(void)tad_write_mem(rec, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return NULL;
	if ( tad_write_mem(rec, buf, size + 1, &size) < E_OK ) {
		free(buf);
		return NULL;
	}
	*p_size = size;
	return buf;
}

/*
 * About to change the record: kept as it is. The oldest go while the
 * kept ones would take more than their room; one that cannot be kept
 * at all goes, and the change is made all the same.
 */
void dc_before( void )
{
	SZ	size = 0;
	UB	*xml = ( g_rec != NULL ) ? rec_text(g_rec, &size) : NULL;

	if ( xml == NULL ) return;
	while ( u_n > 0 && undo_held() + size > UNDO_BYTES ) undo_drop_oldest();
	if ( u_n >= u_max ) {
		INT	m = ( u_max > 0 ) ? u_max * 2 : 16;
		UB	**ns = realloc(u_stk, sizeof(UB *) * (size_t)m);
		SZ	*nl;

		if ( ns != NULL ) u_stk = ns;
		nl = ( ns != NULL ) ? realloc(u_len, sizeof(SZ) * (size_t)m) : NULL;
		if ( nl == NULL ) {
			free(xml);
			return;
		}
		u_len = nl;
		u_max = m;
	}
	u_stk[u_n] = xml;
	u_len[u_n] = size;
	u_n++;
}

/* The record put back as it was before the last change */
BOOL dc_undo( void )
{
	T_TAD	*rec = NULL;

	if ( u_n <= 0 ) return FALSE;
	if ( tad_parse(u_stk[u_n - 1], u_len[u_n - 1], NULL, &rec) < E_OK || rec == NULL ) {
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
		return FALSE;
	}
	u_n--;
	free(u_stk[u_n]);
	u_stk[u_n] = NULL;
	if ( g_rec != NULL ) tad_free(g_rec);
	g_rec = rec;
	dc_model();
	g_changed = TRUE;
	g_dirty = TRUE;
	note("undone");
	return TRUE;
}

INT dc_undos( void )
{
	return u_n;
}

void dc_undo_clear( void )
{
	while ( u_n > 0 ) free(u_stk[--u_n]);
	free(u_stk);
	free(u_len);
	u_stk = NULL;
	u_len = NULL;
	u_max = 0;
}

/* ---------------------------------------------------------------- the tray */

T_TAD *dc_tray_frag( void )
{
	T_TRINF	inf;
	T_TAD	*doc = NULL;
	UB	*buf;
	SZ	asz = 0;
	INT	i;

	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) return NULL;
		if ( inf.kind == TR_TAD && inf.size > 0 ) break;
	}
	if ( i == TR_REC_MAX ) return NULL;
	buf = malloc((size_t)inf.size + 1);
	if ( buf == NULL ) return NULL;
	if ( tr_rea_rec(TR_HAND, i, buf, inf.size, &asz) >= E_OK ) {
		if ( tad_parse(buf, inf.size, NULL, &doc) < E_OK ) doc = NULL;
	}
	free(buf);
	return doc;
}

/* ---------------------------------------------------------------- what the editor asks */

static void h_model( T_DDED *e )
{
	(void)e;
	dc_model();
}

static void h_none( T_DDED *e )
{
	(void)e;
}

static void h_draw( T_DDED *e )
{
	(void)e;
	g_dirty = TRUE;
}

/* Only a part drawn again: drawn, as the whole would be, when the notices waiting have been taken */
static void h_draw_band( T_DDED *e, CONST T_DPRECT *band )
{
	(void)e;
	dc_band(band);
}

/* What the editor drew itself, put on the screen; what is to be drawn again is, when it is */
static void h_update( T_DDED *e )
{
	(void)e;
	if ( !g_dirty ) (void)wm_obj_flush(g_kw, NULL);
}

static void h_work_rect( T_DDED *e, T_DPRECT *page )
{
	(void)e;
	dc_work_rect(page);
}

static BOOL h_origin( T_DDED *e, INT *p_x, INT *p_y )
{
	(void)e;
	*p_x = 0;
	*p_y = 0;
	return TRUE;
}

/* Only the window typed into shows the caret, in its turn to be shown */
static BOOL h_caret_on( T_DDED *e )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	(void)e;
	if ( g_caret_hidden ) return FALSE;
	return (BOOL)( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK
		       && ( wp.flags & OB_WP_FOCUS ) != 0 );
}

static INT h_look( T_DDED *e, UINT num, INT dflt )
{
	(void)e;
	return dc_look(num, dflt);
}

/* The outline last followed in the window: its size is the one carried on outside it */
static T_DPRECT	drag_box;

static void h_drag( T_DDED *e, CONST T_DPRECT *r, BOOL first, INT dx, INT dy )
{
	(void)e; (void)first; (void)dx; (void)dy;
	if ( r != NULL ) drag_box = *r;
}

static void h_note( T_DDED *e, CONST char *s )
{
	(void)e;
	note(s);
}

static void h_before( T_DDED *e )
{
	(void)e;
	dc_before();
}

/* The record changed under the model: what was laid out is not kept */
static void h_forget( T_DDED *e )
{
	(void)e;
	if ( g_doc != NULL ) tv_doc_free(g_doc);
	g_doc = NULL;
}

static void h_undo( T_DDED *e )
{
	(void)e;
	(void)dc_undo();
}

static void h_tell( T_DDED *e, CONST char *l1, CONST char *l2 )
{
	(void)e;
	tell(l1, l2);
}

static void h_no_memory( T_DDED *e )
{
	(void)e;
	tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
}

/* The panels the editor asks for: those of the data box (tvapp.h), as the desktop's own has them */
static BOOL h_ask_text( T_DDED *e, CONST char *p1, CONST char *p2,
			CONST UB *start, UB *out, INT max, CONST char *ok )
{
	(void)e;
	return ta_ask_text(p1, p2, start, out, max, ok);
}

static BOOL h_ask_name( T_DDED *e, CONST char *prompt, CONST UB *start, UB *out, INT max )
{
	(void)e;
	return ta_ask_name(prompt, start, out, max);
}

static BOOL h_fields_form( T_DDED *e, CONST char *title, INT n, CONST char *CONST *labels,
			   CONST char *CONST *hints, UB (*vals)[DE_FIELD_MAX] )
{
	(void)e;
	return ta_fields_form(title, n, labels, hints, vals);
}

static INT h_list_form( T_DDED *e, CONST char *title, CONST char *CONST *names, INT n, INT start )
{
	(void)e;
	return ta_list_form(title, names, n, start);
}

static INT h_paper_form( T_DDED *e, T_DEPAPER *pp )
{
	(void)e;
	return ta_paper_form(pp);
}

static INT h_find_form( T_DDED *e, UB *what, UB *with, INT max, BOOL *p_regex )
{
	(void)e;
	return ta_find_form(what, with, max, p_regex);
}

static BOOL h_pick_colour( T_DDED *e, UW *io )
{
	(void)e;
	return ta_pick_colour(io);
}

/* The faces there are, by their family names, as the kernel has them */
static INT h_faces( T_DDED *e, UB (*names)[64], INT max )
{
	INT	i, n = 0, all = fn_nface();

	(void)e;
	for ( i = 0; i < all && n < max; i++ ) {
		if ( fn_family(fn_face_at(i), names[n], 64) > 0 ) n++;
	}
	return n;
}

/* ---------------------------------------------------------------- かな漢字変換 (tvapp.h) */

static BOOL h_ime_key( T_DDED *e, UINT code, UINT mods )
{
	return ta_ime_key(e, code, mods);
}

static void h_ime_draw( T_DDED *e, INT gid )
{
	ta_ime_draw(e, gid);
}

static BOOL h_ime_on( T_DDED *e )
{
	(void)e;
	return ta_ime_on();
}

/* ---------------------------------------------------------------- the tray */

/* The letters, to the tray as one set */
static ER h_tray_put( T_DDED *e, CONST T_TAD *frag, CONST char *name )
{
	T_TRREC	r;
	UB	*buf;
	SZ	size = 0;
	ER	er;

	(void)e;
	buf = rec_text(frag, &size);
	er = ( buf != NULL ) ? E_OK : E_NOMEM;
	if ( er >= E_OK ) {
		memset(&r, 0, sizeof(r));
		r.kind = TR_TAD;
		r.data = buf;
		r.size = size;
		er = tr_psh_dat(&r, 1, (CONST UB *)name);
	}
	free(buf);
	return er;
}

static T_TAD *h_tray_frag( T_DDED *e )
{
	(void)e;
	return dc_tray_frag();
}

static void h_tray_pictures( T_DDED *e, T_TAD *frag )
{
	(void)e; (void)frag;
}

static void h_tray_taken( T_DDED *e )
{
	(void)e;
	(void)tr_del_dat(TR_HAND);
}

/* ---------------------------------------------------------------- carried out of the text */

/*
 * The text itself is a place to let something go in; anywhere outside the
 * window is something else, where the desktop carries it on (the window
 * hears the hand go out of it while it carries: dc_main.c)
 */
static UINT h_at( T_DDED *e, INT sx, INT sy, T_DDED **p_t )
{
	if ( sx >= 0 && sx < g_w && sy >= 0 && sy < g_h ) {
		*p_t = e;
		return DE_AT_TEXT;
	}
	*p_t = NULL;
	return DE_AT_OTHER;
}

static void h_focus( T_DDED *e, T_DDED *t )
{
	(void)e; (void)t;
}

/* The outline carried on by the desktop: as large as it was followed here, the hand where it held it */
static void carry_look( T_DTREQ *rq, INT sx, INT sy, INT cx, INT cy, INT w, INT h )
{
	rq->look.w = ( drag_box.right > drag_box.left ) ? drag_box.right - drag_box.left : w;
	rq->look.h = ( drag_box.bottom > drag_box.top ) ? drag_box.bottom - drag_box.top : h;
	rq->grab_x = sx - cx;
	rq->grab_y = sy - cy;
	rq->win = g_win;
	memset(&drag_box, 0, sizeof(drag_box));
}

/*
 * What is carried out, as a fragment with its look, in the tray's place
 * of its own for the desktop to put in where it is let go (DT_F_FRAG);
 * FALSE when it could not be put there
 */
static BOOL frag_put( CONST T_TAD *frag )
{
	T_TRREC	r;
	UB	*buf;
	SZ	size = 0;
	ER	er;

	buf = ( frag != NULL ) ? rec_text(frag, &size) : NULL;
	if ( buf == NULL ) return FALSE;
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = buf;
	r.size = size;
	er = tr_set_dat(&r, 1);
	free(buf);
	return (BOOL)( er >= E_OK );
}

/* Where the hand let go back over this window (DT_F_BACK), in its work area */
static BOOL back_at( CONST T_DTANS *an, INT *p_x, INT *p_y )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return FALSE;
	*p_x = an->look.w - wp.wleft;
	*p_y = an->look.h - wp.wtop;
	return TRUE;
}

/*
 * Letters carried out of the window: the desktop follows the hand on
 * (DT_RQ_CARRY with DT_F_TEXT) and puts them where they are let go -- a
 * text of the desktop's as they looked (DT_F_FRAG), a figure of the
 * desktop's or another program's window as one line, line ends and tabs
 * as spaces. They leave here unless they were copied: the second button
 * on the way, or another program's window that was not asked beforehand
 * whether it takes them, and may yet refuse them. Let go back over this
 * window, they move within it (DT_F_BACK).
 */
static ER h_text_away( T_DDED *e, INT sx, INT sy, INT cx, INT cy, UB *s, INT n )
{
	T_DTREQ	rq;
	T_DTANS	an;
	T_TAD	*frag;
	INT	i, k;
	BOOL	put;
	ER	er;
	char	m[64];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.flags = DT_F_TEXT;
	k = ( n < DT_CARRY_TEXT - 1 ) ? n : DT_CARRY_TEXT - 1;
	while ( k > 0 && k < n && ( s[k] & 0xC0 ) == 0x80 ) k--;	/* cut at a letter's edge */
	for ( i = 0; i < k; i++ ) rq.text[i] = ( s[i] == '\n' || s[i] == '\t' ) ? ' ' : s[i];
	rq.text[k] = 0;
	frag = de_sel_frag(e);
	put = frag_put(frag);
	if ( frag != NULL ) tad_free(frag);
	if ( put ) rq.flags |= DT_F_FRAG;
	carry_look(&rq, sx, sy, cx, cy, 120, 20);
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( put ) (void)tr_set_dat(NULL, 0);
	snprintf(m, sizeof(m), "letters carried out (%d)%s%s%s", (INT)er,
		 ( er >= E_OK && an.how == DT_HOW_COPY ) ? " as a copy" : "",
		 ( er >= E_OK && ( an.flags & DT_F_TO_PROG ) != 0 ) ? " to a program" : "",
		 ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) ? " back here" : "");
	note(m);
	if ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) {
		/* back over this window: within it, where the hand let go */
		INT	x, y;

		if ( back_at(&an, &x, &y) ) de_sel_back(e, x, y, (BOOL)( an.how == DT_HOW_COPY ));
		return 0;
	}
	if ( er < E_OK ) return er;	/* the desktop said so, when it was not over this window */
	return ( an.how == DT_HOW_COPY ) ? 0 : 1;
}

/* What the desktop is asked of a link, carried or put, as it shows here */
static void link_ask( CONST T_VOBJ *v, UINT req, T_DTREQ *rq )
{
	size_t	n = strlen((CONST char *)v->applist);

	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	rq->target = v->target;
	rq->recno = -1;
	rq->look.chsz = v->chsz;
	rq->look.frcol = v->frcol;
	rq->look.chcol = v->chcol;
	rq->look.tbcol = v->tbcol;
	rq->look.bgcol = v->bgcol;
	rq->look.disp = v->disp;
	rq->look.autoopen = v->autoopen ? 1 : 0;
	snprintf((char *)rq->name, sizeof(rq->name), "%s", (CONST char *)v->relationship);
	if ( v->hidden ) rq->flags |= DT_F_HIDDEN;
	if ( n > 0 && n < sizeof(rq->text) - 1 ) {
		memcpy(rq->text, v->applist, n + 1);
		rq->flags |= DT_F_APPLIST;
	}
}

/*
 * A picture or a figure carried out of the window: the desktop follows
 * the hand on (DT_RQ_CARRY with DT_F_FRAG, the element in the tray's
 * place of its own) and puts it in a text or a figure of the desktop's
 * where it is let go; it leaves the text unless it was a copy. Let go
 * back over this window, it moves within it (DT_F_BACK).
 */
static BOOL piece_away( T_DDED *e, CONST T_DDTHING *th, INT w, INT h )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_TAD		*frag = de_frag_new("document");
	T_TADNODE	*body = ( frag != NULL ) ? tad_body(frag) : NULL;
	T_TADNODE	*p = ( body != NULL ) ? tad_elem_new(frag, "p", body, NULL, FALSE) : NULL;
	BOOL		put = FALSE;
	ER		er;
	char		m[64];

	if ( p != NULL && tad_node_copy(frag, p, NULL, th->node) != NULL ) {
		put = frag_put(frag);
	}
	if ( frag != NULL ) tad_free(frag);
	if ( !put ) {
		say("ここには置けません");
		return FALSE;
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.flags = DT_F_FRAG;
	rq.look.w = w;
	rq.look.h = h;
	rq.grab_x = th->x0 - th->box.left;
	rq.grab_y = th->y0 - th->box.top;
	rq.win = g_win;
	memset(&drag_box, 0, sizeof(drag_box));
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	(void)tr_set_dat(NULL, 0);
	snprintf(m, sizeof(m), "%s carried out (%d)%s%s", ( th->kind == TV_RUN_FIGURE ) ? "figure" : "picture",
		 (INT)er, ( er >= E_OK && ( an.how == DT_HOW_COPY || th->copy ) ) ? " as a copy" : "",
		 ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) ? " back here" : "");
	note(m);
	if ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) {
		T_DDTHING	t = *th;
		INT		x, y;

		t.copy = (BOOL)( an.how == DT_HOW_COPY || th->copy );
		if ( back_at(&an, &x, &y) ) de_thing_back(e, x, y, &t);
		return FALSE;
	}
	if ( er < E_OK || an.how == DT_HOW_COPY || th->copy ) return FALSE;
	/* put there: it leaves the text */
	dc_before();
	tad_node_remove(th->node);
	return TRUE;
}

/*
 * A link carried out of the window: the desktop follows the hand on
 * (DT_RQ_CARRY) and it is put where it is let go (DT_RQ_PLACE), in a
 * figure, a cabinet or a text of the desktop's or on another program's
 * window. It leaves the text unless it was a copy: the second button on
 * the way, or another program's window that was not asked beforehand
 * whether it takes it, and may yet refuse it. A picture or a figure goes
 * as piece_away carries it. Let go back over this window, it moves
 * within it (DT_F_BACK).
 */
static BOOL h_thing_away( T_DDED *e, INT sx, INT sy, CONST T_DDTHING *th )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UINT	seq;
	BOOL	copy, given;
	INT	w = th->box.right - th->box.left, h = th->box.bottom - th->box.top;
	ER	er;
	char	m[TAD_NAME_MAX + 64];

	if ( th->kind != TV_RUN_LINK ) {
		return piece_away(e, th, w, h);
	}
	link_ask(&th->v, DT_RQ_CARRY, &rq);
	rq.flags |= DT_F_TO_PROG;
	rq.look.w = w;
	rq.look.h = h;
	rq.grab_x = th->x0 - th->box.left;
	rq.grab_y = th->y0 - th->box.top;
	rq.win = g_win;
	(void)sx; (void)sy;
	memset(&drag_box, 0, sizeof(drag_box));
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) {
		T_DDTHING	t = *th;
		INT		x, y;

		note("link carried back here");
		t.copy = (BOOL)( an.how == DT_HOW_COPY || th->copy );
		if ( back_at(&an, &x, &y) ) de_thing_back(e, x, y, &t);
		return FALSE;
	}
	if ( er < E_OK ) {
		snprintf(m, sizeof(m), "link carried nowhere (%d)", (INT)er);
		note(m);
		return FALSE;
	}
	seq = rq.seq;
	copy = (BOOL)( an.how == DT_HOW_COPY || th->copy );
	given = (BOOL)( ( an.flags & DT_F_TO_PROG ) != 0 );
	link_ask(&th->v, DT_RQ_PLACE, &rq);
	rq.look.w = w;
	rq.look.h = h;
	rq.carry = seq;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 10 * 60 * 1000);
	snprintf(m, sizeof(m), "link carried out (%d)%s%s", (INT)er, copy ? " as a copy" : "", given ? " to a program" : "");
	note(m);
	if ( er < E_OK || copy ) return FALSE;
	/* put there: it leaves the text */
	dc_before();
	return (BOOL)( tad_lnk_del(g_rec, &th->v.vobjid) >= E_OK );
}

/* ---------------------------------------------------------------- objects */

/*
 * 仮身化: a new document, a copy of the template of documents made as
 * the desktop makes one (ob_dup_obj), with the whole of this one's
 * record in it for the editor to cut down to what was picked
 */
#define DOC_BASE	"019a1132-762b-7b02-ba2a-a918a9b37c39"

static T_TAD	*nobj_rec;		/* the record of the object new_object made, until it is kept */

static ER h_new_object( T_DDED *e, CONST UB *name, TS_UUID *p_id, T_TAD **p_rec )
{
	TS_UUID	base;
	UB	*xml;
	SZ	len = 0;
	ER	er;
	char	s[80];

	(void)e;
	*p_rec = NULL;
	er = ui_uuid(DOC_BASE, &base);
	if ( er >= E_OK ) er = ob_dup_obj(&base, name, p_id);
	snprintf(s, sizeof(s), "a new object made (%d)", (INT)er);
	note(s);
	if ( er < E_OK ) return er;
	xml = rec_text(g_rec, &len);
	if ( xml == NULL ) return E_NOMEM;
	er = tad_parse(xml, len, NULL, p_rec);
	free(xml);
	if ( er >= E_OK && *p_rec == NULL ) er = E_NOMEM;
	if ( er < E_OK ) return er;
	if ( nobj_rec != NULL ) tad_free(nobj_rec);
	nobj_rec = *p_rec;
	return E_OK;
}

/* Kept: its record written, calling itself by its name, as the desktop renames one */
static void h_object_done( T_DDED *e, CONST TS_UUID *id, CONST UB *name )
{
	T_TAD		*rec = nobj_rec;
	T_TADNODE	*root;
	ER		er = E_NOEXS;
	char		s[80];

	(void)e;
	nobj_rec = NULL;
	if ( rec != NULL ) {
		root = tad_root(rec);
		if ( root != NULL ) (void)tad_set_attr(rec, root, "filename", name);
		er = ta_rec_put(id, rec);
		tad_free(rec);
	}
	snprintf(s, sizeof(s), "a new object kept (%d)", (INT)er);
	note(s);
}

/* 原稿 read back as the record: the one held is let go of, the editor holds the new one */
static T_TAD *h_restore( T_DDED *e, CONST UB *xml, SZ n )
{
	T_TAD	*rec = NULL;

	(void)e;
	if ( tad_parse(xml, n, NULL, &rec) < E_OK || rec == NULL ) return NULL;
	if ( g_rec != NULL ) tad_free(g_rec);
	g_rec = rec;
	return rec;
}

/*
 * 単語登録 started with the selection's first line as the word: it is
 * put in the tray's place of its own (UD_TRAY_SUB), where the accessory
 * takes it from as it starts, and the desktop brings the accessory up
 */
static void h_word_register( T_DDED *e, CONST UB *w, INT n )
{
	T_TRREC	r;
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[48];

	(void)e;
	if ( n > UD_WORD_MAX ) n = UD_WORD_MAX;
	if ( n > 0 ) {
		memset(&r, 0, sizeof(r));
		r.kind = TR_REC;
		r.sub = UD_TRAY_SUB;
		r.data = (UB *)w;
		r.size = n;
		(void)tr_set_dat(&r, 1);
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_TOOL;
	strncpy((char *)rq.name, "単語登録", sizeof(rq.name) - 1);
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 5000);
	if ( er >= E_OK ) er = an.er;
	snprintf(s, sizeof(s), "word register %d (%d)", (INT)n, (INT)er);
	note(s);
	if ( er < E_OK ) tell("単語登録を起動できませんでした。", NULL);
}

CONST T_DDHOST	dc_host = {
	.model = h_model, .changed = h_none, .draw = h_draw, .draw_all = h_draw,
	.update = h_update, .shown = h_update, .draw_band = h_draw_band,
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
