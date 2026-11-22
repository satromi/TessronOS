/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_link.c
 *	基本図形編集 as a process: the 仮身操作, 実身操作 and 実行 of a link in
 *	the figure (design 11.3, 11.2.6)
 *
 *	The items, the requests to the desktop and what is done with its
 *	answers are the shared part's (lib/libtv/tv_vop.c). What is the
 *	figure's is here: a link of the record made a T_TVLINK, what the
 *	desktop answers written into the record as one change that can be
 *	taken back, and where a link to a new copy goes.
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/tvsrc.h>

#define NEW_VERSION_STEP	8	/* 新版作成: the copy's link over the old, so much down and across */
#define DUPLICATE_STEP		16	/* 実身複製 */

/*
 * The name a link shows: its object's, whole, as the store has it now,
 * else what the record says. Kept until the next call.
 */
CONST char *fg_link_name( CONST T_VOBJ *v )
{
	static UB	*name = NULL;

	free(name);
	name = ui_obj_name(&v->target);
	if ( name != NULL && name[0] != 0 ) {
		return (CONST char *)name;
	}
	return (CONST char *)TAD_VS(v->vname);
}

/* ---------------------------------------------------------------- the figure's part */

/* What the desktop answered, written into the link and the record, as one change */
static BOOL vop_put( T_TVLINK *l, UINT what, CONST T_TVPUT *p )
{
	T_VOBJ		*v = (T_VOBJ *)l->app;
	CONST T_DTANS	*an = p->an;

	if ( what == TV_VOP_W_LOOK ) {
		v->right = v->left + an->look.w;
		v->bottom = v->top + an->look.h;
		v->height = an->look.h;
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
	fg_before();
	(void)tad_lnk_set(g_rec, v);
	fg_after();
	return TRUE;
}

/* 実身複製 and 新版作成: the copy's link a step down and across from the link */
static void vop_put_copy( T_TVLINK *l, CONST TS_UUID *copy, UINT why )
{
	INT	step = ( why == TV_VOP_C_DUP ) ? DUPLICATE_STEP : NEW_VERSION_STEP;

	fg_link_copy_put((CONST T_VOBJ *)l->app, copy, step, step);
}

static void vop_changed( void )
{
	g_changed = TRUE;
}

/* The box's own way of opening a link (ウインドウ一覧 brings the window that shows it to the front) */
static BOOL vop_open( T_TVLINK *l )
{
	return (BOOL)( fg_box != NULL && fg_box->open != NULL && fg_box->open((CONST T_VOBJ *)l->app) );
}

CONST T_TVVOP *fg_vop( void )
{
	static T_TVVOP	h;

	if ( h.put == NULL ) {
		ta_tv_asker(&h.ask);
		/*
		 * the figure is drawn scaled: the desktop is told the links' sizes
		 * in the record, and asked how they stand before the menu shows
		 */
		h.flags = TV_VOP_F_STATE | TV_VOP_F_DISK | TV_VOP_F_REC_SIZE;
		h.note = note;
		h.say = say;
		h.named = ta_tv_named;
		h.put = vop_put;
		h.put_copy = vop_put_copy;
		h.changed = vop_changed;
		h.open = vop_open;
		h.forget = tv_src_forget;
	}
	return &h;
}

void fg_tvlink( T_TVLINK *l, CONST T_VOBJ *v, CONST T_DPRECT *box )
{
	tv_vop_vobj(l, v, box);
	l->app = (void *)v;
}

/* ---------------------------------------------------------------- the menu */

INT fg_link_menu( ID mid, CONST T_VOBJ *v )
{
	T_TVLINK	l;

	if ( v == NULL ) return tv_vop_menu(fg_vop(), mid, NULL);
	fg_tvlink(&l, v, NULL);
	return tv_vop_menu(fg_vop(), mid, &l);
}

/* A link to a copy of v's object put a step down and across from v, as one change */
void fg_link_copy_put( CONST T_VOBJ *v, CONST TS_UUID *copy, INT dx, INT dy )
{
	T_VOBJ	c = { .recno = 0 };
	TS_UUID	id;
	ER	er;
	char	s[80];

	tad_vobj_init(&c);
	er = tad_vobj_copy(&c, v);
	if ( er < E_OK ) {
		say("メモリが足りないため、この操作をできませんでした");
		return;
	}
	c.target = *copy;
	memset(&c.vobjid, 0, sizeof(c.vobjid));
	(void)tad_vobj_set(&c, TAD_W_NAME, NULL, 0);
	c.dlen = 0;			/* a new object has no data appended to it yet */
	c.left += dx;  c.right += dx;
	c.top += dy;   c.bottom += dy;
	fg_before();
	fg_pick_none();
	er = tad_lnk_add(g_rec, &c, &id);
	tad_vobj_clear(&c);
	/*
	 * The copy is linked to by nothing yet: the link put to it is counted
	 * when the record is saved, as every link that came since is
	 */
	if ( er >= E_OK ) tv_src_forget(copy);
	fg_after();
	snprintf(s, sizeof(s), "linked a copy at %d,%d (%d), %d shapes", c.left, c.top, (INT)er,
		 ( g_fig != NULL ) ? g_fig->nsh : 0);
	note(s);
}

/* A link opened: as a double press opens it, unless the box opens it its own way */
void fg_open( CONST T_VOBJ *v )
{
	T_TVLINK	l;

	fg_tvlink(&l, v, NULL);
	(void)tv_vop_open(fg_vop(), &l);
}

BOOL fg_link_op( CONST char *code, INT index, T_VOBJ *v )
{
	T_TVLINK	l;

	fg_tvlink(&l, v, NULL);
	if ( !tv_vop_command(fg_vop(), &l, code, index) ) return FALSE;
	g_dirty = TRUE;
	return TRUE;
}

/* ---------------------------------------------------------------- 整頓 */

/*
 * What 整頓 knows of the object a link points at: its metadata (name,
 * makeDate, updateDate, relationship, the first program the applist
 * names) and the size of its record 0, as the desktop reads them
 */
/* A string of the metadata as long as it is, in memory of its own: NULL when it is none */
static UB *js_dup( CONST T_JSON *v )
{
	UB	*d;

	if ( js_type(v) != JS_STRING || ( d = (UB *)malloc((size_t)v->len + 1) ) == NULL ) return NULL;
	if ( js_str(v, d, v->len + 1) < 0 ) {
		free(d);
		return NULL;
	}
	return d;
}

static BOOL ar_info( void *arg, CONST TS_UUID *id, T_FEARINFO *inf )
{
	T_JSON	root, rel, al, t, e, nm;
	T_OBREC	rec[16];
	UB	*m = NULL;
	INT	n, cnt = 0, i, k = 0, len, room = 0;
	ID	key = ob_opn_obj(id, OB_OP_ATRRD);

	(void)arg;
	if ( key <= 0 ) return FALSE;
	n = ob_atr_read(key, 0, &m);
	if ( ob_lst_rec(key, rec, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 16; i++ ) {
			if ( rec[i].recno == 0 ) inf->bytes = (INT)rec[i].size;
		}
	}
	ob_cls_obj(key);
	if ( n <= 0 || js_parse(m, n, &root) < E_OK ) {
		free(m);
		return FALSE;
	}
	if ( js_get(&root, "name", &nm) >= E_OK && ( inf->name = js_dup(&nm) ) != NULL ) {
		inf->label = js_dup(&nm);
		inf->has_label = (BOOL)( inf->label != NULL );
	}
	(void)js_get_str(&root, "makeDate", inf->made, FE_AR_INFO_TEXT);
	(void)js_get_str(&root, "updateDate", inf->updated, FE_AR_INFO_TEXT);
	if ( js_get(&root, "relationship", &rel) >= E_OK && js_type(&rel) == JS_ARRAY ) {
		/* "[tag] [tag]", every tag whole: room for all of them first */
		t.s = NULL;
		while ( js_next(&rel, &t) ) room += t.len + 3;
		inf->rel = (UB *)malloc((size_t)room + 1);
		t.s = NULL;
		while ( inf->rel != NULL && js_next(&rel, &t) ) {
			UB	*tag = js_dup(&t);

			if ( tag == NULL ) continue;
			if ( k > 0 ) inf->rel[k++] = ' ';
			inf->rel[k++] = '[';
			for ( i = 0; tag[i] != 0; i++ ) inf->rel[k++] = tag[i];
			inf->rel[k++] = ']';
			free(tag);
		}
		if ( inf->rel != NULL ) inf->rel[k] = 0;
	}
	if ( js_get(&root, "applist", &al) >= E_OK && js_type(&al) == JS_OBJECT ) {
		e.s = NULL;
		while ( !inf->has_app && js_next(&al, &e) ) {
			if ( js_type(&e) != JS_OBJECT ) continue;
			len = js_get_str(&e, "name", inf->app, FE_AR_APP_NAME);
			if ( len >= 0 || len == E_LIMIT ) inf->has_app = TRUE;
		}
	}
	free(m);
	return TRUE;
}

void fg_arrange( CONST T_FEARRANGE *o )
{
	T_FEARITEM	*it;
	INT		i, n = 0, got;
	ER		er;
	char		s[FG_LOG];

	if ( g_fig == NULL || g_npick <= 0 ) return;
	it = calloc((size_t)g_npick, sizeof(T_FEARITEM));	/* zeroed: each link empty */
	if ( it == NULL ) {
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
		return;
	}
	/* the templates of the 原紙箱 stay where they were laid out: 整頓 leaves them */
	for ( i = 0; i < g_fig->nsh && n < g_npick && !FG_TEMPLATES; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &it[n].v) >= E_OK ) {
			it[n].seq = g_pick[i];
			n++;
		}
	}
	got = n;
	n = fe_arrange(it, n, o, ar_info, NULL);
	if ( n > 0 ) {
		fg_before();
		for ( i = 0; i < n; i++ ) {
			er = tad_lnk_set(g_rec, &it[i].v);
			if ( er < E_OK ) {
				snprintf(s, sizeof(s), "the link \"%.120s\" was not changed (%d)", (char *)TAD_VS(it[i].v.vname), (INT)er);
				note(s);
			}
		}
		fg_after();
	}
	/* fe_arrange keeps every link it was given in it[], laid out or not */
	for ( i = 0; i < got; i++ ) tad_vobj_clear(&it[i].v);
	free(it);
	snprintf(s, sizeof(s), "arranged %d", n);
	note(s);
	g_dirty = TRUE;
}
