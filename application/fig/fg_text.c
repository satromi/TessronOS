/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_text.c
 *	A piece of text in the figure edited where it stands (design 11.3)
 *
 *	A <document> element of the figure, pressed twice, is edited in
 *	its box with the editor of text the desktop uses (application/doc/
 *	docedit.c): its record is the figure's, its model is made from that
 *	element alone, and its page is the box the text stands in, as the
 *	figure is shown and magnified. A change to it is a change to the
 *	figure, taken back with the figure's. Pressing outside the box, or
 *	Escape, ends it.
 *
 *	The editor's places are the window's work area: what the window is
 *	told of the pointer is there already, so the work area is where the
 *	"screen" of the editor begins.
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/look.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/docmenu.h>
#include <ts/docedit.h>
#include <ts/figtool.h>

/* keyboard usages */
#define KEY_ESC		0x29

typedef struct {
	T_DDED		e;
	T_TADNODE	*node;		/* the <document> element */
	T_TVDOC		*doc;
	T_TAD		*xrec;
	INT		scroll_x, scroll_y, height;
	UINT		kind;
	BOOL		xml_view, detail_view, dirty, sealed, used, settling;
	INT		recno;
} T_FGTEXT;

static T_FGTEXT	*tb = NULL;
static UD	tx_at = 0;
static INT	tx_x, tx_y;

#define TB(e)	( (T_FGTEXT *)(e)->arg )

/* ---------------------------------------------------------------- what the editor asks */

static void h_model( T_DDED *e )
{
	T_FGTEXT	*t = TB(e);

	if ( t->doc != NULL ) tv_doc_free(t->doc);
	t->doc = NULL;
	if ( t->node != NULL ) (void)tv_doc_of(t->node, &t->doc);
}

/* The figure it stands in shows it as it now is */
static void h_changed( T_DDED *e )
{
	(void)e;
	fg_after();
}

static void h_draw( T_DDED *e )
{
	(void)e;
	g_dirty = TRUE;
}

static void h_none( T_DDED *e )
{
	(void)e;
}

/* The box the text stands in, as the figure is drawn; the text told the magnification */
static void h_work_rect( T_DDED *e, T_DPRECT *page )
{
	T_FGTEXT	*t = TB(e);
	INT		i;

	page->left = page->top = 0;
	page->right = page->bottom = 0;
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &g_fig->sh[i];

		if ( s->kind == TV_SH_DOC && s->node == t->node ) {
			page->left = s->r.left * g_zoom / 8 - g_sx;
			page->top = s->r.top * g_zoom / 8 - g_sy;
			page->right = s->r.right * g_zoom / 8 - g_sx;
			page->bottom = s->r.bottom * g_zoom / 8 - g_sy;
			break;
		}
	}
	if ( t->doc != NULL ) {
		t->doc->zoom = g_zoom;
		t->doc->zoom_den = 8;
	}
}

static BOOL h_origin( T_DDED *e, INT *p_x, INT *p_y )
{
	(void)e;
	*p_x = 0;
	*p_y = 0;
	return TRUE;
}

static BOOL h_true( T_DDED *e )
{
	(void)e;
	return TRUE;
}

static INT h_look( T_DDED *e, UINT num, INT dflt )
{
	INT	v = (INT)wm_look(num);

	(void)e;
	return ( v > 0 || dflt <= 1 ) ? v : dflt;
}

static void h_drag( T_DDED *e, CONST T_DPRECT *r, BOOL first, INT dx, INT dy )
{
	(void)e;
	(void)r;
	(void)first;
	(void)dx;
	(void)dy;
}

static void h_note( T_DDED *e, CONST char *s )
{
	(void)e;
	note(s);
}

static void h_before( T_DDED *e )
{
	(void)e;
	fg_before();
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

/* The panels a document asks for have no place in a piece of text in a figure: put away */
static BOOL h_ask_text( T_DDED *e, CONST char *p1, CONST char *p2,
			CONST UB *start, UB *out, INT max, CONST char *ok )
{
	(void)e; (void)p1; (void)p2; (void)start; (void)out; (void)max; (void)ok;
	return FALSE;
}

/* A name asked (TA_PN_NAME of the data box, tvapp.h) */
static BOOL h_ask_name( T_DDED *e, CONST char *prompt, CONST UB *start, UB *out, INT max )
{
	(void)e;
	return ta_ask_name(prompt, start, out, max);
}

static BOOL h_fields_form( T_DDED *e, CONST char *title, INT n, CONST char *CONST *labels,
			   CONST char *CONST *hints, UB (*vals)[DE_FIELD_MAX] )
{
	(void)e; (void)title; (void)n; (void)labels; (void)hints; (void)vals;
	return FALSE;
}

static INT h_list_form( T_DDED *e, CONST char *title, CONST char *CONST *names, INT n, INT start )
{
	(void)e; (void)title; (void)names; (void)n; (void)start;
	return -1;
}

static INT h_paper_form( T_DDED *e, T_DEPAPER *pp )
{
	(void)e;
	return fgt_paper_form(pp);
}

/* 検索/置換 (TA_PN_FIND of the data box, tvapp.h) */
static INT h_find_form( T_DDED *e, UB *what, UB *with, INT max, BOOL *p_regex )
{
	(void)e;
	return ta_find_form(what, with, max, p_regex);
}

static BOOL h_pick_colour( T_DDED *e, UW *io )
{
	(void)e; (void)io;
	return FALSE;
}

static INT h_faces( T_DDED *e, UB (*names)[64], INT max )
{
	(void)e; (void)names; (void)max;
	return 0;
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

/* The letters, to the tray as one set */
static ER h_tray_put( T_DDED *e, CONST T_TAD *frag, CONST char *name )
{
	T_TRREC	r;
	UB	*buf;
	SZ	size = 0;
	ER	er;

	(void)e;
	(void)tad_write_mem(frag, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	er = ( buf != NULL ) ? tad_write_mem(frag, buf, size + 1, &size) : E_NOMEM;
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
	return fg_tray_frag();
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

/* Only the text itself is a place to let something go in */
static UINT h_at( T_DDED *e, INT sx, INT sy, T_DDED **p_t )
{
	T_DPRECT	page;

	h_work_rect(e, &page);
	if ( sx >= page.left && sx < page.right && sy >= page.top && sy < page.bottom ) {
		*p_t = e;
		return DE_AT_TEXT;
	}
	*p_t = NULL;
	return DE_AT_NONE;
}

static void h_focus( T_DDED *e, T_DDED *t )
{
	(void)e; (void)t;
}

static ER h_text_away( T_DDED *e, INT sx, INT sy, INT cx, INT cy, UB *s, INT n )
{
	(void)e; (void)sx; (void)sy; (void)cx; (void)cy; (void)s; (void)n;
	return E_PAR;
}

static BOOL h_thing_away( T_DDED *e, INT sx, INT sy, CONST T_DDTHING *th )
{
	(void)e; (void)sx; (void)sy; (void)th;
	return FALSE;
}

/*
 * 仮身化: a new document, a copy of the template of documents made as
 * the desktop makes one (ob_dup_obj), with the whole of the figure's
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
	(void)tad_write_mem(g_rec, NULL, 0, &len);
	if ( len == 0 ) return E_NOEXS;
	xml = malloc((size_t)len + 1);
	if ( xml == NULL ) return E_NOMEM;
	er = tad_write_mem(g_rec, xml, len + 1, &len);
	if ( er >= E_OK ) er = tad_parse(xml, len, NULL, p_rec);
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
	T_TAD		*rec = NULL;
	T_TADNODE	*root;
	ER		er = E_NOEXS;
	char		s[80];

	(void)e;
	/* the record the editor was handed, the one new_object made last */
	rec = nobj_rec;
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

static T_TAD *h_restore( T_DDED *e, CONST UB *xml, SZ n )
{
	(void)e; (void)xml; (void)n;
	return NULL;
}

static void h_word_register( T_DDED *e, CONST UB *w, INT n )
{
	(void)e; (void)w; (void)n;
}

static CONST T_DDHOST	fg_text_host = {
	.model = h_model, .changed = h_changed, .draw = h_draw, .draw_all = h_draw,
	.update = h_none, .shown = h_none, .work_rect = h_work_rect, .origin = h_origin,
	.caret_on = h_true, .look = h_look, .drag = h_drag, .note = h_note,
	.before = h_before, .forget = h_none, .undo = h_none,
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

/* ---------------------------------------------------------------- the figure editor's calls */

/*
 * What the 書式 palette sets (figtool.h), put on the piece of text being
 * edited: the letters picked in it, or else all of them
 */
void fg_text_look( INT item )
{
	static CONST char *CONST faces[3] = { "sans-serif", "serif", "monospace" };
	UB	word[16];

	if ( tb == NULL ) return;
	if ( !de_has_sel(&tb->e) ) de_pick_all(&tb->e);
	switch ( item ) {
	case 0:
		snprintf((char *)word, sizeof(word), "%d", ft_look.font_size);
		de_font(&tb->e, "size", word);
		break;
	case 1:
		de_font(&tb->e, "face", (CONST UB *)faces[ft_look.font_face % 3]);
		break;
	case 2:
	case 3:
		ft_colour_hex(ft_look.text_col, word);
		de_font(&tb->e, "color", word);
		break;
	case 4:	de_look(&tb->e, DM_STYLE_BOLD, ft_look.bold);		break;
	case 5:	de_look(&tb->e, DM_STYLE_ITALIC, ft_look.italic);	break;
	case 6:	de_look(&tb->e, DM_STYLE_UNDERLINE, ft_look.under);	break;
	default: de_look(&tb->e, DM_STYLE_STRIKE, ft_look.strike);	break;
	}
	g_dirty = TRUE;
}

BOOL fg_text_on( void )
{
	return (BOOL)( tb != NULL );
}

void fg_text_end( void )
{
	T_FGTEXT	*t = tb;

	if ( t == NULL ) return;
	ta_ime_end(&t->e);	/* what is being composed goes in first */
	tb = NULL;
	if ( t->doc != NULL ) tv_doc_free(t->doc);
	free(t);
	g_dirty = TRUE;
	note("text editing ended");
}

ER fg_text_letters( INT x, INT y, CONST UB *s, INT n )
{
	return ( tb != NULL ) ? de_text_drop(&tb->e, x, y, s, n) : E_PAR;
}

void fg_text_begin( T_TADNODE *node, INT x, INT y )
{
	T_FGTEXT	*t;

	fg_text_end();
	t = calloc(1, sizeof(T_FGTEXT));
	if ( t == NULL ) return;
	t->node = node;
	t->kind = TV_KIND_DOC;
	t->used = TRUE;
	t->e.rec = &g_rec;
	t->e.doc = &t->doc;
	t->e.xrec = &t->xrec;
	t->e.scroll_x = &t->scroll_x;
	t->e.scroll_y = &t->scroll_y;
	t->e.height = &t->height;
	t->e.kind = &t->kind;
	t->e.xml_view = &t->xml_view;
	t->e.detail_view = &t->detail_view;
	t->e.dirty = &t->dirty;
	t->e.sealed = &t->sealed;
	t->e.used = &t->used;
	t->e.settling = &t->settling;
	t->e.id = &g_obj;
	t->e.recno = &t->recno;
	t->e.framed = TRUE;
	t->e.host = &fg_text_host;
	t->e.arg = t;
	tb = t;
	h_model(&t->e);
	fg_pick_none();
	if ( x >= 0 ) {
		de_press(&t->e, x, y, FALSE, 0);
		de_release();
	}
	g_dirty = TRUE;
	note("text editing begun");
}

/*
 * A press: in the box of the text being edited, the caret put there;
 * elsewhere that text is done with, and the press goes on to the
 * figure. A second press on a piece of text with the first begins
 * editing it. TRUE when the press was the text's.
 */
BOOL fg_text_press( CONST T_OBNTM *m )
{
	INT	near = (INT)wm_look(LK_DBL_W) / 2, gap_ms = (INT)wm_look(WM_LOOK_DBLTIME);
	INT	i, fx, fy;
	BOOL	twice;

	if ( g_fig == NULL || fg_cabinet ) return FALSE;	/* a cabinet's text is not edited */
	if ( tb != NULL ) {
		T_DPRECT	page;

		ta_ime_commit(&tb->e);	/* a press anywhere commits what is being composed */
		h_work_rect(&tb->e, &page);
		if ( m->x >= page.left && m->x < page.right && m->y >= page.top && m->y < page.bottom ) {
			de_press(&tb->e, m->x, m->y, FALSE, 0);
			g_dirty = TRUE;
			return TRUE;
		}
		fg_text_end();		/* pressed elsewhere: done, and that press goes on */
		return FALSE;
	}
	if ( near <= 0 ) near = 5;
	if ( gap_ms <= 0 ) gap_ms = 400;
	twice = (BOOL)( m->x > tx_x - near && m->x < tx_x + near && m->y > tx_y - near && m->y < tx_y + near
		     && m->when - tx_at < (UD)gap_ms * 1000000ULL );
	tx_x = m->x;
	tx_y = m->y;
	tx_at = m->when;
	if ( !twice || ft_tool != FT_TL_SELECT ) return FALSE;
	fx = ( m->x + g_sx ) * 8 / g_zoom;
	fy = ( m->y + g_sy ) * 8 / g_zoom;
	for ( i = g_fig->nsh - 1; i >= 0; i-- ) {
		CONST T_TVSHAPE	*s = &g_fig->sh[i];

		if ( s->node == NULL || fx < s->r.left || fx >= s->r.right
		  || fy < s->r.top || fy >= s->r.bottom ) {
			continue;
		}
		if ( s->kind == TV_SH_DOC ) {
			fg_hold_cancel();
			fg_text_begin(s->node, m->x, m->y);
			return TRUE;
		}
		break;
	}
	/* a picture: painted on, the canvas in hand; a shape whose points can be moved: 変形 */
	if ( fgt_twice(m->x, m->y, fx, fy, i) ) {
		fg_hold_cancel();
		return TRUE;
	}
	return FALSE;
}

/* The hand moving with the button down, and let go */
BOOL fg_text_holding( void )
{
	return (BOOL)( tb != NULL && ( de_dragging() || de_link_carrying() || de_mark_carrying() ) );
}

void fg_text_second_down( void )
{
	de_sel_copy();
	de_link_copy();
}

void fg_text_moved( INT x, INT y )
{
	if ( tb == NULL ) return;
	if ( de_dragging() ) de_follow(x, y);
	else if ( de_link_carrying() ) de_link_follow(x, y);
	else if ( de_mark_carrying() ) de_mark_follow(x, y);
	g_dirty = TRUE;
}

void fg_text_released( INT x, INT y )
{
	if ( tb == NULL ) return;
	if ( de_sel_carrying() ) de_sel_release(x, y);
	de_release();
	de_mark_release();
	if ( de_link_carrying() ) de_link_release(x, y);
	g_dirty = TRUE;
}

/* A key typed into the text being edited; Escape ends it. TRUE when it was the text's */
BOOL fg_text_key( CONST T_OBNTM *m )
{
	if ( tb == NULL ) return FALSE;
	ta_ime_mode(m->imode);
	if ( m->code == KEY_ESC && !ta_ime_busy() ) {
		fg_text_end();		/* unless it is cancelling what is being composed */
		return TRUE;
	}
	if ( de_key(&tb->e, m->code, m->mods) ) {
		g_dirty = TRUE;
		return TRUE;
	}
	return FALSE;
}

/* A name on the system's list of candidates pressed */
void fg_text_pick( INT row )
{
	if ( tb != NULL ) ta_ime_pick(&tb->e, row);
}

/* A command of the menu over the text being edited: TRUE when it was the text's */
BOOL fg_text_command( INT cmd )
{
	if ( tb == NULL ) return FALSE;
	return de_command(&tb->e, cmd);
}

/* The box of the text being edited, its selection and its caret, over the figure drawn */
void fg_text_draw( INT gid )
{
	T_DPRECT	tr;
	T_DPPAT		tp;

	if ( tb == NULL ) return;
	h_work_rect(&tb->e, &tr);
	memset(&tp, 0, sizeof(tp));
	dp_pat_colour(&tp, 0x000078D7U);
	dp_line_wide(gid, tr.left - 1, tr.top - 1, tr.right, tr.top - 1, 1, DP_LINE_DASH, &tp);
	dp_line_wide(gid, tr.left - 1, tr.bottom, tr.right, tr.bottom, 1, DP_LINE_DASH, &tp);
	dp_line_wide(gid, tr.left - 1, tr.top - 1, tr.left - 1, tr.bottom, 1, DP_LINE_DASH, &tp);
	dp_line_wide(gid, tr.right, tr.top - 1, tr.right, tr.bottom, 1, DP_LINE_DASH, &tp);
	de_draw(&tb->e, gid);
}

/* The record put back from what was kept (取消): the text's element is gone with the old one */
void fg_text_lost( void )
{
	fg_text_end();
}
