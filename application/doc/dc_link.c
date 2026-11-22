/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dc_link.c
 *	基本文章編集 as a process: the 仮身操作, 実身操作 and 実行 of a link in
 *	the text (design 11.3, 11.2.6)
 *
 *	The items, the requests to the desktop and what is done with its
 *	answers are the shared part's (lib/libtv/tv_vop.c). What is the
 *	text's is here: a link of the record made a T_TVLINK, what the
 *	desktop answers written into the record as one change that can be
 *	taken back, and where a link to a new copy goes in the text.
 */

#include "dc.h"
#include <string.h>
#include <stdio.h>
#include <ts/tvsrc.h>

/* The record changed under the model: made again from it, and drawn again */
static void remodel( void )
{
	dc_model();
	g_dirty = TRUE;
}

/* What the desktop answered, written into the link and the record, as one change */
static BOOL vop_put( T_TVLINK *l, UINT what, CONST T_TVPUT *p )
{
	T_VOBJ		*v = (T_VOBJ *)l->app;
	CONST T_DTANS	*an = p->an;

	if ( what == TV_VOP_W_LOOK ) {
		/*
		 * A link in a text says its width and height; one that says a
		 * box of its own keeps that. A size it does not say is laid out
		 * by its letters, and stays unsaid.
		 */
		if ( v->right > v->left ) {
			v->right = v->left + an->look.w;
			v->bottom = v->top + an->look.h;
			v->height = an->look.h;
		}
		if ( v->width > 0 ) v->width = an->look.w;
		if ( v->heightpx > 0 ) v->heightpx = an->look.h;
		v->chsz = an->look.chsz;
		v->frcol = an->look.frcol;
		v->chcol = an->look.chcol;
		v->tbcol = an->look.tbcol;
		v->bgcol = an->look.bgcol;
		v->disp = an->look.disp;
		v->autoopen = (BOOL)( an->look.autoopen != 0 );
		v->hidden = (BOOL)( ( an->flags & DT_F_HIDDEN ) != 0 );
	} else if ( tad_vobj_set(v, ( what == TV_VOP_W_REL ) ? TAD_W_REL : TAD_W_APPLIST,
				 ( p->len > 0 ) ? p->text : NULL, ( p->len > 0 ) ? p->len : 0) < E_OK ) {
		return FALSE;
	}
	if ( g_rec == NULL ) return FALSE;
	dc_before();
	if ( tad_lnk_set(g_rec, v) < E_OK ) return FALSE;
	remodel();
	return TRUE;
}

/*
 * 実身複製 and 新版作成: a link to the copy, looking as the link does, put
 * in the text right after it, as one change
 */
static void vop_put_copy( T_TVLINK *l, CONST TS_UUID *copy, UINT why )
{
	CONST T_VOBJ	*v = (CONST T_VOBJ *)l->app;
	T_VOBJ		c = { .recno = 0 };
	TS_UUID		id;
	ER		er;
	char		s[80];

	(void)why;
	if ( g_rec == NULL ) return;
	tad_vobj_init(&c);
	if ( tad_vobj_copy(&c, v) < E_OK ) {
		say("メモリが足りないため、この操作をできませんでした");
		return;
	}
	c.target = *copy;
	memset(&c.vobjid, 0, sizeof(c.vobjid));
	(void)tad_vobj_set(&c, TAD_W_NAME, NULL, 0);
	c.dlen = 0;			/* a new object has no data appended to it yet */
	dc_before();
	er = tad_lnk_add(g_rec, &c, &id);
	tad_vobj_clear(&c);
	if ( er >= E_OK ) {
		/* added at the end of the record: moved into the text after the link it was made from */
		de_link_after(&g_de, &v->vobjid);
		/*
		 * The copy is linked to by nothing yet: the link put to it is
		 * counted when the record is saved, as every link that came since is
		 */
		tv_src_forget(copy);
		ta_watch(copy);
	}
	remodel();
	snprintf(s, sizeof(s), "linked a copy after the link (%d), %d links", (INT)er, tad_lnk_count(g_rec));
	note(s);
}

static void vop_changed( void )
{
	g_changed = TRUE;
}

CONST T_TVVOP *dc_vop( void )
{
	static T_TVVOP	h;

	if ( h.put == NULL ) {
		ta_tv_asker(&h.ask);
		/* asked how the link stands before the menu shows: 開く and 閉じる as its object is open, and ディスク操作 on the disk of the link */
		h.flags = TV_VOP_F_STATE | TV_VOP_F_DISK;
		h.note = note;
		h.say = say;
		h.named = ta_tv_named;
		h.put = vop_put;
		h.put_copy = vop_put_copy;
		h.changed = vop_changed;
		h.forget = tv_src_forget;
	}
	return &h;
}

void dc_tvlink( T_TVLINK *l, CONST T_VOBJ *v, CONST T_DPRECT *box )
{
	T_DPRECT	b;

	if ( box == NULL ) {
		b.left = v->left;
		b.top = v->top;
		if ( v->right > v->left ) {
			b.right = v->right;
			b.bottom = v->bottom;
		} else {
			b.right = v->left + v->width;
			b.bottom = v->top + ( ( v->heightpx > 0 ) ? v->heightpx : tad_lnk_closed_h(v) );
		}
		box = &b;
	}
	tv_vop_vobj(l, v, box);
	/* where it is in the record: a link in a text has its width and height, not a right and foot */
	if ( v->right <= v->left ) {
		l->was_right = v->left + v->width;
		l->was_bottom = v->top + v->heightpx;
	}
	l->app = (void *)v;
}
