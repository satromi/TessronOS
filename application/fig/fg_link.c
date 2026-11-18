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
 *	基本図形編集 as a process: the 仮身操作 and 実身操作 of a link in the
 *	figure (design 11.3)
 *
 *	What needs the desktop -- its panels, the store's names and copies
 *	-- is asked of it (ts/dtreq.h) as for any program's link; what comes
 *	back the link is to become is written into the record here, as one
 *	change that can be taken back. 起動アプリの固定 is the link's own
 *	applist, written here: fixed, it is given the applist its object has
 *	now (else its own) with "defaultOpenLock" on the entry it opens with.
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
 * A request about the object of link v, its look said; its relationship
 * and its applist go in the tail of the next ask_desktop
 */
void fg_ask_of( CONST T_VOBJ *v, UINT req, T_DTREQ *rq )
{
	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	rq->target = v->target;
	rq->recno = -1;
	rq->look.w = v->right - v->left;
	rq->look.h = v->bottom - v->top;
	rq->look.chsz = v->chsz;
	rq->look.frcol = v->frcol;
	rq->look.chcol = v->chcol;
	rq->look.tbcol = v->tbcol;
	rq->look.bgcol = v->bgcol;
	rq->look.disp = v->disp;
	rq->look.autoopen = v->autoopen ? 1 : 0;
	dt_req_was(rq, v->left, v->top, v->right, v->bottom);	/* where it is in the figure's record */
	if ( v->vrel_len > 0 ) ta_req_part(DT_TAIL_REL, v->vrel, v->vrel_len);
	if ( v->hidden ) rq->flags |= DT_F_HIDDEN;
	if ( v->vapplist_len > 0 ) {
		ta_req_part(DT_TAIL_APPLIST, v->vapplist, v->vapplist_len);
		rq->flags |= DT_F_APPLIST;
	}
}

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

/* ---------------------------------------------------------------- applists */

static BOOL space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* The entry an applist opens with: the member whose "defaultOpen" is true */
static BOOL app_default( CONST UB *json, INT n, T_JSON *entry )
{
	T_JSON	root, it;

	if ( n <= 0 || js_parse(json, n, &root) < E_OK || js_type(&root) != JS_OBJECT ) return FALSE;
	it.s = NULL;
	it.len = 0;
	while ( js_next(&root, &it) ) {
		if ( js_type(&it) == JS_OBJECT && js_get_bool(&it, "defaultOpen", FALSE) ) {
			*entry = it;
			return TRUE;
		}
	}
	return FALSE;
}

BOOL fg_app_locked( CONST T_VOBJ *v )
{
	T_JSON	e;

	return (BOOL)( app_default(TAD_VS(v->vapplist), (INT)v->vapplist_len, &e)
		    && js_get_bool(&e, "defaultOpenLock", FALSE) );
}

/* n bytes of s put at out[*at], and a terminator after them */
static void put( UB *out, INT *at, CONST UB *s, INT n )
{
	memcpy(out + *at, s, (size_t)n);
	*at += n;
	out[*at] = 0;
}

/*
 * The applist json with "defaultOpenLock" as keep says on its default
 * entry, in memory of its own (*p_out, to be freed): the length,
 * E_NOEXS when it has no default entry, E_NOMEM.
 */
static INT app_lock( CONST UB *json, INT n, BOOL keep, UB **p_out )
{
	T_JSON		e, old;
	CONST UB	*word = (CONST UB *)( keep ? "true" : "false" );
	INT		at, end, k = 0, wn = (INT)strlen((CONST char *)word);
	UB		*out;

	*p_out = NULL;
	if ( !app_default(json, n, &e) ) return E_NOEXS;
	out = (UB *)malloc((size_t)n + 32);	/* the json, its key and the word added */
	if ( out == NULL ) return E_NOMEM;
	if ( js_get(&e, "defaultOpenLock", &old) >= E_OK ) {
		at = (INT)( old.s - json );
		end = at + old.len;
		put(out, &k, json, at);
		put(out, &k, word, wn);
		put(out, &k, json + end, n - end);
	} else {
		/* before the entry's closing brace, after what it has */
		end = (INT)( e.s - json ) + e.len - 1;
		for ( at = end; at > 0 && space(json[at - 1]); at-- ) ;
		put(out, &k, json, at);
		put(out, &k, (CONST UB *)( ( json[at - 1] == '{' ) ? "\"defaultOpenLock\":" : ",\"defaultOpenLock\":" ),
		    ( json[at - 1] == '{' ) ? 18 : 19);
		put(out, &k, word, wn);
		put(out, &k, json + end, n - end);
	}
	*p_out = out;
	return k;
}

/*
 * 起動アプリの固定 turned over: the applist the object has now, or the
 * link's own when the object has none, written into the link with the
 * default entry fixed or let go
 */
static void app_keep( T_VOBJ *v )
{
	T_JSON	root, al;
	UB	*m = NULL, *out = NULL;
	BOOL	keep = (BOOL)!fg_app_locked(v);
	INT	n = -1, mn;
	ID	k = ob_opn_obj(&v->target, OB_OP_ATRRD);
	char	s[96];
	ER	er;

	if ( k > 0 ) {
		mn = ob_atr_read(k, 0, &m);
		ob_cls_obj(k);
		if ( mn > 0 && js_parse(m, mn, &root) >= E_OK && js_get(&root, "applist", &al) >= E_OK ) {
			n = app_lock(al.s, al.len, keep, &out);
		}
		free(m);
	}
	if ( n == E_NOEXS || n == -1 ) {
		n = app_lock(TAD_VS(v->vapplist), (INT)v->vapplist_len, keep, &out);
	}
	if ( n == E_NOMEM ) {
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
		return;
	}
	if ( n < 0 ) {
		tell("起動アプリを持たない実身です", NULL);
		return;
	}
	er = tad_vobj_set(v, TAD_W_APPLIST, out, n);
	free(out);
	if ( er < E_OK ) {
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
		return;
	}
	fg_before();
	(void)tad_lnk_set(g_rec, v);
	fg_after();
	snprintf(s, sizeof(s), "%s %.60s", keep ? "kept" : "let go", fg_link_name(v));
	note(s);
}

/* ---------------------------------------------------------------- the menu */

/* 仮身操作 and 実身操作 set for link v under the pointer, or grey when there is none */
void fg_link_menu( ID mid, CONST T_VOBJ *v )
{
	T_OBREF	ref;
	T_DTREQ	rq;
	T_DTANS	an;
	BOOL	cut, kept, open = FALSE, lockable = TRUE, detach = FALSE, grow = FALSE;

	if ( v == NULL ) {
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
		/* ディスク操作 away from a link: 切り離し is of a disk's link, 拡張 of the system's volume */
		(void)mn_chg_atr(mid, NULL, "disk.detach", MN_GREY);
		return;
	}
	cut = (BOOL)( ob_ref_obj(&v->target, &ref) < E_OK );
	kept = fg_app_locked(v);
	/* whether the object is open, and has a program to be fixed to, as the desktop sees it */
	fg_ask_of(v, DT_RQ_LINK_STATE, &rq);
	memset(&an, 0, sizeof(an));
	if ( ask_desktop(&rq, &an, 3000) >= E_OK ) {
		open = (BOOL)( ( an.flags & DT_F_OPEN ) != 0 );
		lockable = (BOOL)( ( an.flags & DT_F_LOCKABLE ) != 0 );
		detach = (BOOL)( ( an.flags & DT_F_DETACHABLE ) != 0 );
		grow = (BOOL)( ( an.flags & DT_F_GROWABLE ) != 0 );
	}
	(void)mn_chg_atr(mid, NULL, "disk.detach", detach ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "disk.grow", grow ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "vobj.open", ( open || cut ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "vobj.close", open ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "vobj.reconnect", cut ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "real.rename", cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.duplicate", cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.newversion", cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.lock", kept ? MN_HIDE : ( cut || !lockable ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.unlock", !kept ? MN_HIDE : cut ? MN_GREY : 0);
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
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
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

/*
 * A link opened: as a double press opens it, unless the box opens it its
 * own way (ウインドウ一覧 brings the window that shows it to the front)
 */
void fg_open( CONST T_VOBJ *v )
{
	if ( fg_box != NULL && fg_box->open != NULL && fg_box->open(v) ) return;
	link_open(v);
}

/*
 * One of the 仮身操作 and 実身操作 on link v, by its code (the link as the
 * menu found it under the pointer)
 */
void fg_link_op( CONST char *code, T_VOBJ *v )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[FG_LOG];
	ER	er;

	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		fg_open(v);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		fg_ask_of(v, DT_RQ_CLOSE, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		fg_ask_of(v, DT_RQ_ATTR, &rq);
		(void)ta_link_named(&rq, &v->vobjid);	/* the windows opened from it told its new look */
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			fg_before();
			v->right = v->left + an.look.w;
			v->bottom = v->top + an.look.h;
			v->height = an.look.h;
			v->chsz = an.look.chsz;
			v->frcol = an.look.frcol;
			v->chcol = an.look.chcol;
			v->tbcol = an.look.tbcol;
			v->bgcol = an.look.bgcol;
			v->disp = an.look.disp;
			v->autoopen = (BOOL)( an.look.autoopen != 0 );
			v->hidden = (BOOL)( ( an.flags & DT_F_HIDDEN ) != 0 );
			(void)tad_lnk_set(g_rec, v);
			fg_after();
			snprintf(s, sizeof(s), "attr %s chsz %d bgcol %06x", fg_link_name(v), (INT)v->chsz,
				 (UINT)( v->bgcol & 0x00FFFFFFU ));
			note(s);
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		fg_ask_of(v, DT_RQ_RELATION, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			SZ		tl = 0;
			CONST UB	*t = ta_ans_part(DT_TAIL_TEXT, &tl);

			if ( tad_vobj_set(v, TAD_W_REL, t, tl) < E_OK ) {
				tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
				return;
			}
			fg_before();
			(void)tad_lnk_set(g_rec, v);
			fg_after();
			snprintf(s, sizeof(s), "relation %.80s [%.80s]", fg_link_name(v), (CONST char *)TAD_VS(v->vrel));
			note(s);
		}
	} else if ( strcmp(code, "vobj.reconnect") == 0 ) {
		T_OBREF	ref;

		tv_src_forget(&v->target);
		if ( ob_ref_obj(&v->target, &ref) < E_OK ) tell("実身に接続できませんでした", NULL);
	} else if ( strcmp(code, "real.rename") == 0 ) {
		fg_ask_of(v, DT_RQ_RENAME, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			CONST UB	*t = ta_ans_part(DT_TAIL_TEXT, NULL);

			tv_src_forget(&v->target);
			snprintf(s, sizeof(s), "renamed %.120s", (CONST char *)TAD_VS(t));
			note(s);
		}
	} else if ( strcmp(code, "real.duplicate") == 0 || strcmp(code, "real.newversion") == 0 ) {
		BOOL	dup = (BOOL)( code[5] == 'd' );

		fg_ask_of(v, dup ? DT_RQ_DUPLICATE : DT_RQ_NEWVER, &rq);
		er = ask_desktop(&rq, &an, 30 * 60 * 1000);
		if ( er >= E_OK ) {
			INT	step = dup ? DUPLICATE_STEP : NEW_VERSION_STEP;

			fg_link_copy_put(v, &an.into, step, step);
			snprintf(s, sizeof(s), "%s %s", dup ? "duplicated" : "new version", fg_link_name(v));
			note(s);
		}
	} else if ( strcmp(code, "real.lock") == 0 || strcmp(code, "real.unlock") == 0 ) {
		app_keep(v);
	} else if ( strcmp(code, "real.info") == 0 ) {
		fg_ask_of(v, DT_RQ_INFO, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.network") == 0 ) {
		fg_ask_of(v, DT_RQ_NETWORK, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.search") == 0 ) {
		fg_ask_of(v, DT_RQ_SEARCH, &rq);
		(void)ask_desktop(&rq, &an, 30 * 60 * 1000);
	}
	g_dirty = TRUE;
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
