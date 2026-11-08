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

/* A request about the object of link v, its look and its applist said */
void fg_ask_of( CONST T_VOBJ *v, UINT req, T_DTREQ *rq )
{
	size_t	n = strlen((CONST char *)v->applist);

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
	snprintf((char *)rq->name, sizeof(rq->name), "%s", (CONST char *)v->relationship);
	if ( v->hidden ) rq->flags |= DT_F_HIDDEN;
	/* one that does not fit is asked of by its object alone */
	if ( n > 0 && n < sizeof(rq->text) - 1 ) {
		memcpy(rq->text, v->applist, n + 1);
		rq->flags |= DT_F_APPLIST;
	}
}

/* The name a link shows: its object's, as the store has it now, else what the record says */
CONST char *fg_link_name( CONST T_VOBJ *v )
{
	static UB	name[OB_NAME_MAX];
	T_OBREF		ref;

	if ( ob_ref_obj(&v->target, &ref) >= E_OK && ref.name[0] != 0 ) {
		memcpy(name, ref.name, OB_NAME_MAX);
		name[OB_NAME_MAX - 1] = 0;
		return (CONST char *)name;
	}
	return (CONST char *)v->name;
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
	INT	n = (INT)strlen((CONST char *)v->applist);

	return (BOOL)( app_default(v->applist, n, &e) && js_get_bool(&e, "defaultOpenLock", FALSE) );
}

/* Bytes put at out[*at], while they fit in max (with a terminator) */
static BOOL put( UB *out, INT max, INT *at, CONST UB *s, INT n )
{
	if ( *at + n >= max ) return FALSE;
	memcpy(out + *at, s, (size_t)n);
	*at += n;
	out[*at] = 0;
	return TRUE;
}

/*
 * The applist json with "defaultOpenLock" as keep says on its default
 * entry, into out: the whole, or, when that does not fit, the default
 * entry alone, under its key. The length, E_NOEXS when it has no default
 * entry, E_LIMIT when not even that fits.
 */
static INT app_lock( CONST UB *json, INT n, BOOL keep, UB *out, INT max )
{
	T_JSON		e, old;
	CONST UB	*word = (CONST UB *)( keep ? "true" : "false" );
	CONST UB	*from, *p;
	INT		at, end, k = 0, wn = (INT)strlen((CONST char *)word);
	BOOL		ok;

	if ( !app_default(json, n, &e) ) return E_NOEXS;
	if ( js_get(&e, "defaultOpenLock", &old) >= E_OK ) {
		at = (INT)( old.s - json );
		end = at + old.len;
		ok = put(out, max, &k, json, at) && put(out, max, &k, word, wn)
		  && put(out, max, &k, json + end, n - end);
	} else {
		/* before the entry's closing brace, after what it has */
		end = (INT)( e.s - json ) + e.len - 1;
		for ( at = end; at > 0 && space(json[at - 1]); at-- ) ;
		ok = put(out, max, &k, json, at)
		  && put(out, max, &k, (CONST UB *)( ( json[at - 1] == '{' ) ? "\"defaultOpenLock\":" : ",\"defaultOpenLock\":" ),
			 ( json[at - 1] == '{' ) ? 18 : 19)
		  && put(out, max, &k, word, wn) && put(out, max, &k, json + end, n - end);
	}
	if ( ok ) return k;

	/* the default entry alone: its key is the string before its colon */
	for ( p = e.s; p > json && space(p[-1]); p-- ) ;
	if ( p <= json || p[-1] != ':' ) return E_LIMIT;
	for ( p--; p > json && space(p[-1]); p-- ) ;
	if ( p <= json || p[-1] != '"' ) return E_LIMIT;
	for ( from = p - 1; from > json && from[-1] != '"'; from-- ) ;
	if ( from <= json ) return E_LIMIT;
	from--;
	{
		UB	*one = malloc((size_t)( p - from ) + (size_t)e.len + 3);
		INT	m = 0, r;

		if ( one == NULL ) return E_NOMEM;
		one[m++] = '{';
		memcpy(one + m, from, (size_t)( p - from ));
		m += (INT)( p - from );
		one[m++] = ':';
		memcpy(one + m, e.s, (size_t)e.len);
		m += e.len;
		one[m++] = '}';
		r = ( m < n ) ? app_lock(one, m, keep, out, max) : E_LIMIT;
		free(one);
		return r;
	}
}

/*
 * 起動アプリの固定 turned over: the applist the object has now, or the
 * link's own when the object has none, written into the link with the
 * default entry fixed or let go
 */
static void app_keep( T_VOBJ *v )
{
	T_JSON	root, al;
	UB	*m = NULL, out[TAD_APPL_MAX];
	BOOL	keep = (BOOL)!fg_app_locked(v);
	INT	n = -1, mn;
	ID	k = ob_opn_obj(&v->target, OB_OP_ATRRD);
	char	s[TAD_NAME_MAX + 32];

	if ( k > 0 ) {
		mn = ob_atr_read(k, 0, &m);
		ob_cls_obj(k);
		if ( mn > 0 && js_parse(m, mn, &root) >= E_OK && js_get(&root, "applist", &al) >= E_OK ) {
			n = app_lock(al.s, al.len, keep, out, (INT)sizeof(out));
		}
		free(m);
	}
	if ( n == E_NOEXS || n == -1 ) {
		n = app_lock(v->applist, (INT)strlen((CONST char *)v->applist), keep, out, (INT)sizeof(out));
	}
	if ( n < 0 ) {
		tell("起動アプリを持たない実身です", NULL);
		return;
	}
	fg_before();
	memcpy(v->applist, out, (size_t)n + 1);
	(void)tad_lnk_set(g_rec, v);
	fg_after();
	snprintf(s, sizeof(s), "%s %s", keep ? "kept" : "let go", fg_link_name(v));
	note(s);
}

/* ---------------------------------------------------------------- the menu */

/* 仮身操作 and 実身操作 set for link v under the pointer, or grey when there is none */
void fg_link_menu( ID mid, CONST T_VOBJ *v )
{
	T_OBREF	ref;
	T_DTREQ	rq;
	T_DTANS	an;
	BOOL	cut, kept, open = FALSE, lockable = TRUE;

	if ( v == NULL ) {
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
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
	}
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
	T_VOBJ	c = *v;
	TS_UUID	id;
	ER	er;
	char	s[80];

	c.target = *copy;
	memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.name[0] = 0;
	c.dlen = 0;			/* a new object has no data appended to it yet */
	c.left += dx;  c.right += dx;
	c.top += dy;   c.bottom += dy;
	fg_before();
	fg_pick_none();
	er = tad_lnk_add(g_rec, &c, &id);
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
 * One of the 仮身操作 and 実身操作 on link v, by its code (the link as the
 * menu found it under the pointer)
 */
void fg_link_op( CONST char *code, T_VOBJ *v )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[TAD_NAME_MAX + 48];
	ER	er;

	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		link_open(v);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		fg_ask_of(v, DT_RQ_CLOSE, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		fg_ask_of(v, DT_RQ_ATTR, &rq);
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
			snprintf(s, sizeof(s), "attr %s", fg_link_name(v));
			note(s);
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		fg_ask_of(v, DT_RQ_RELATION, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			an.text[DT_TEXT_MAX - 1] = 0;
			fg_before();
			snprintf((char *)v->relationship, TAD_REL_MAX, "%s", (CONST char *)an.text);
			(void)tad_lnk_set(g_rec, v);
			fg_after();
			snprintf(s, sizeof(s), "relation %s [%s]", fg_link_name(v), (CONST char *)an.text);
			note(s);
		}
	} else if ( strcmp(code, "vobj.reconnect") == 0 ) {
		T_OBREF	ref;

		tv_src_forget(&v->target);
		if ( ob_ref_obj(&v->target, &ref) < E_OK ) tell("実身に接続できませんでした", NULL);
	} else if ( strcmp(code, "real.rename") == 0 ) {
		fg_ask_of(v, DT_RQ_RENAME, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			an.text[DT_TEXT_MAX - 1] = 0;
			tv_src_forget(&v->target);
			snprintf(s, sizeof(s), "renamed %s", (CONST char *)an.text);
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
static BOOL ar_info( void *arg, CONST TS_UUID *id, T_FEARINFO *inf )
{
	T_JSON	root, rel, al, t, e;
	T_OBREC	rec[16];
	UB	*m = NULL, tag[TAD_NAME_MAX];
	INT	n, cnt = 0, i, k = 0, len;
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
	len = js_get_str(&root, "name", inf->name, TAD_NAME_MAX);
	if ( len >= 0 || len == E_LIMIT ) {
		memcpy(inf->label, inf->name, TAD_NAME_MAX);
		inf->has_label = TRUE;
	}
	(void)js_get_str(&root, "makeDate", inf->made, FE_AR_INFO_TEXT);
	(void)js_get_str(&root, "updateDate", inf->updated, FE_AR_INFO_TEXT);
	if ( js_get(&root, "relationship", &rel) >= E_OK && js_type(&rel) == JS_ARRAY ) {
		t.s = NULL;
		while ( js_next(&rel, &t) ) {
			len = js_str(&t, tag, (INT)sizeof(tag));
			if ( len < 0 ) continue;
			if ( k > 0 && k < TAD_NAME_MAX - 1 ) inf->rel[k++] = ' ';
			if ( k < TAD_NAME_MAX - 1 ) inf->rel[k++] = '[';
			for ( i = 0; tag[i] != 0 && k < TAD_NAME_MAX - 2; i++ ) inf->rel[k++] = tag[i];
			if ( k < TAD_NAME_MAX - 1 ) inf->rel[k++] = ']';
		}
		inf->rel[k] = 0;
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
	INT		i, n = 0;
	ER		er;
	char		s[TAD_NAME_MAX + 48];

	if ( g_fig == NULL || g_npick <= 0 ) return;
	it = malloc(sizeof(T_FEARITEM) * (size_t)g_npick);
	if ( it == NULL ) return;
	for ( i = 0; i < g_fig->nsh && n < g_npick; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &it[n].v) >= E_OK ) {
			it[n].seq = g_pick[i];
			n++;
		}
	}
	n = fe_arrange(it, n, o, ar_info, NULL);
	if ( n > 0 ) {
		fg_before();
		for ( i = 0; i < n; i++ ) {
			er = tad_lnk_set(g_rec, &it[i].v);
			if ( er < E_OK ) {
				snprintf(s, sizeof(s), "the link \"%s\" was not changed (%d)", (char *)it[i].v.name, (INT)er);
				note(s);
			}
		}
		fg_after();
	}
	free(it);
	snprintf(s, sizeof(s), "arranged %d", n);
	note(s);
	g_dirty = TRUE;
}
