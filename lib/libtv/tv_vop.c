/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_vop.c
 *	The 仮身操作, 実身操作 and 実行 of a link in a program's window,
 *	the same in every program (include/ts/tvvop.h, design 11.2.6)
 *
 *	Each request about a link says the link as the desktop's own
 *	links are said: its look, where it is in the record, its
 *	relationship and its applist whole in the tail, so that the desktop
 *	decides as it does for one of its own and a link carried out keeps
 *	them. What the desktop answers the link is to become is handed to
 *	the program to write back (T_TVVOP put), and a link to a copy it made
 *	to the program to put (put_copy).
 */

#include <ts/tvvop.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ob.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/mn.h>
#include <ts/ui.h>
#include <ts/sysdef.h>

/* ---------------------------------------------------------------- asking */

static ER ask( CONST T_TVVOP *h, T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	return h->ask.ask(h->ask.arg, rq, an, wait_ms);
}

static void part( CONST T_TVVOP *h, UINT kind, CONST UB *s, SZ len )
{
	(void)h->ask.part(h->ask.arg, kind, s, len);
}

/* A part of the last answer, "" when it has none */
static CONST UB *ans_text( CONST T_TVVOP *h, UINT kind, SZ *p_len )
{
	SZ		len = 0;
	CONST UB	*t = h->ask.ans(h->ask.arg, kind, &len);

	if ( t == NULL ) {
		t = (CONST UB *)"";
		len = 0;
	}
	if ( p_len != NULL ) *p_len = len;
	return t;
}

static void note( CONST T_TVVOP *h, CONST char *s )
{
	if ( h->note != NULL ) h->note(s);
}

static void say( CONST T_TVVOP *h, CONST char *s )
{
	if ( h->say != NULL ) h->say(s);
}

static void changed( CONST T_TVVOP *h )
{
	if ( h->changed != NULL ) h->changed();
}

static BOOL no_uuid( CONST TS_UUID *u )
{
	static CONST TS_UUID	none;

	return (BOOL)( memcmp(u, &none, sizeof(none)) == 0 );
}

/* The name of the object, whole, in memory of its own ("" when it has none, NULL no memory) */
static UB *name_of( CONST TS_UUID *u )
{
	UB	*nm = ui_obj_name(u);

	return ( nm != NULL ) ? nm : (UB *)calloc(1, 1);
}

#define NM(p)	( ( (p) != NULL ) ? (CONST char *)(p) : "" )

/* ---------------------------------------------------------------- a link */

void tv_vop_vobj( T_TVLINK *l, CONST T_VOBJ *v, CONST T_DPRECT *box )
{
	memset(l, 0, sizeof(*l));
	l->target = v->target;
	l->vobjid = v->vobjid;
	if ( box != NULL ) {
		l->box = *box;
	} else {
		l->box.left = v->left;
		l->box.top = v->top;
		l->box.right = v->right;
		l->box.bottom = v->bottom;
	}
	l->was_left = v->left;
	l->was_top = v->top;
	l->was_right = v->right;
	l->was_bottom = v->bottom;
	l->look.chsz = v->chsz;
	l->look.frcol = v->frcol;
	l->look.chcol = v->chcol;
	l->look.tbcol = v->tbcol;
	l->look.bgcol = v->bgcol;
	l->look.disp = v->disp;
	l->look.autoopen = v->autoopen ? 1 : 0;
	l->hidden = v->hidden;
	l->wordwrap = v->wordwrap;
	l->rel = v->vrel;
	l->rel_len = v->vrel_len;
	l->applist = v->vapplist;
	l->applist_len = v->vapplist_len;
	l->view = v->vview;
	l->view_len = v->vview_len;
}

void tv_vop_req( CONST T_TVVOP *h, CONST T_TVLINK *l, UINT req, T_DTREQ *rq )
{
	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	rq->target = l->target;
	rq->recno = -1;
	rq->look = l->look;
	if ( ( h->flags & TV_VOP_F_REC_SIZE ) != 0 && l->was_right > l->was_left ) {
		rq->look.w = l->was_right - l->was_left;
		rq->look.h = l->was_bottom - l->was_top;
	} else {
		rq->look.w = l->box.right - l->box.left;
		rq->look.h = l->box.bottom - l->box.top;
	}
	if ( l->hidden ) rq->flags |= DT_F_HIDDEN;
	dt_req_was(rq, l->was_left, l->was_top, l->was_right, l->was_bottom);	/* where it is in the record */
	if ( l->rel != NULL && l->rel_len > 0 ) part(h, DT_TAIL_REL, l->rel, l->rel_len);
	/* the program it keeps to, for the desktop to decide by as for a link of its own */
	if ( l->applist != NULL && l->applist_len > 0 ) {
		part(h, DT_TAIL_APPLIST, l->applist, l->applist_len);
		rq->flags |= DT_F_APPLIST;
	}
}

/*
 * The link named for the desktop (DT_F_LINK): the record's object, the
 * link's vobjid and the channel, parted by tabs, as the tail's text, so
 * that what a window opened from it does to it, and what 仮身属性変更
 * changes, is told there. FALSE when the program has no such channel or
 * the link no vobjid, and nothing is added.
 */
static BOOL link_named( CONST T_TVVOP *h, CONST T_TVLINK *l, T_DTREQ *rq )
{
	TS_UUID	parent, chan;
	char	po[TS_UUID_STRLEN + 1], pv[TS_UUID_STRLEN + 1], pc[TS_UUID_STRLEN + 1];
	char	head[3 * ( TS_UUID_STRLEN + 1 ) + 1];

	if ( h->named == NULL || no_uuid(&l->vobjid) || !h->named(&parent, &chan) ) return FALSE;
	ts_uuid_text(&parent, po);
	ts_uuid_text(&l->vobjid, pv);
	ts_uuid_text(&chan, pc);
	snprintf(head, sizeof(head), "%s\t%s\t%s", po, pv, pc);
	part(h, DT_TAIL_TEXT, (CONST UB *)head, -1);
	rq->flags |= DT_F_LINK;
	return TRUE;
}

ER tv_vop_open( CONST T_TVVOP *h, T_TVLINK *l )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UB	*nm;
	char	s[160];
	ER	er;

	if ( h->open != NULL && h->open(l) ) return E_OK;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = l->target;
	rq.recno = -1;
	if ( l->applist != NULL && l->applist_len > 0 ) {
		part(h, DT_TAIL_APPLIST, l->applist, l->applist_len);
		rq.flags |= DT_F_APPLIST;
	}
	/* what the window opened does to the link comes back to be put in the record, shown as the link says */
	if ( link_named(h, l, &rq) ) {
		if ( l->view != NULL && l->view_len > 0 ) part(h, DT_TAIL_VIEW, l->view, l->view_len);
		rq.grab_x = l->wordwrap;
		rq.look.frcol = l->look.frcol;
		rq.look.chcol = l->look.chcol;
		rq.look.tbcol = l->look.tbcol;
		rq.look.bgcol = l->look.bgcol;
	}
	er = ask(h, &rq, &an, 3000);
	nm = name_of(&rq.target);
	snprintf(s, sizeof(s), "opened %s (%d)", NM(nm), (INT)er);
	free(nm);
	note(h, s);
	if ( er < E_OK ) say(h, "開けませんでした");
	return er;
}

/* ---------------------------------------------------------------- applists */

/*
 * The applist that says the link's program: the link's own, else its
 * object's now. In memory of its own (*p_out, to be freed, NULL none):
 * its length, 0 when there is none.
 */
static INT app_json( CONST T_TVLINK *l, UB **p_out )
{
	T_JSON	root, al;
	UB	*m = NULL;
	INT	n = 0;
	ID	k;

	*p_out = NULL;
	if ( l->applist != NULL && l->applist_len > 0 ) {
		*p_out = (UB *)malloc((size_t)l->applist_len + 1);
		if ( *p_out == NULL ) return 0;
		memcpy(*p_out, l->applist, (size_t)l->applist_len);
		(*p_out)[l->applist_len] = 0;
		return (INT)l->applist_len;
	}
	k = ob_opn_obj(&l->target, OB_OP_ATRRD);
	if ( k <= 0 ) return 0;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n > 0 && js_parse(m, n, &root) >= E_OK && js_get(&root, "applist", &al) >= E_OK
	  && ( *p_out = (UB *)malloc((size_t)al.len + 1) ) != NULL ) {
		memcpy(*p_out, al.s, (size_t)al.len);
		(*p_out)[al.len] = 0;
		n = al.len;
	} else {
		n = 0;
	}
	free(m);
	return n;
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

static BOOL app_locked( CONST UB *json, INT n )
{
	T_JSON	e;

	return (BOOL)( app_default(json, n, &e) && js_get_bool(&e, "defaultOpenLock", FALSE) );
}

BOOL tv_vop_kept( CONST T_TVLINK *l )
{
	UB	*j = NULL;
	INT	n = app_json(l, &j);
	BOOL	kept = ( n > 0 ) ? app_locked(j, n) : FALSE;

	free(j);
	return kept;
}

static BOOL space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* n bytes of s put at out[*at], and a terminator after them */
static void put_bytes( UB *out, INT *at, CONST UB *s, INT n )
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
		put_bytes(out, &k, json, at);
		put_bytes(out, &k, word, wn);
		put_bytes(out, &k, json + end, n - end);
	} else {
		/* before the entry's closing brace, after what it has */
		end = (INT)( e.s - json ) + e.len - 1;
		for ( at = end; at > 0 && space(json[at - 1]); at-- ) ;
		put_bytes(out, &k, json, at);
		put_bytes(out, &k, (CONST UB *)( ( json[at - 1] == '{' ) ? "\"defaultOpenLock\":" : ",\"defaultOpenLock\":" ),
			  ( json[at - 1] == '{' ) ? 18 : 19);
		put_bytes(out, &k, word, wn);
		put_bytes(out, &k, json + end, n - end);
	}
	*p_out = out;
	return k;
}

/* 起動アプリの固定 turned over: the link given the applist with its default entry fixed or let go */
static void app_keep( CONST T_TVVOP *h, T_TVLINK *l )
{
	UB	*j = NULL, *out = NULL;
	INT	n = app_json(l, &j), k = -1;
	BOOL	keep = FALSE;
	T_TVPUT	p;

	if ( n > 0 ) {
		keep = (BOOL)!app_locked(j, n);
		k = app_lock(j, n, keep, &out);
	}
	free(j);
	if ( k == E_NOMEM ) {
		say(h, "メモリが足りないため、この操作をできませんでした");
		return;
	}
	if ( k < 0 ) {
		say(h, "起動アプリを持たない実身です");
		return;
	}
	memset(&p, 0, sizeof(p));
	p.text = out;
	p.len = k;
	if ( h->put(l, TV_VOP_W_APPLIST, &p) ) {
		changed(h);
		note(h, keep ? "kept" : "let go");
		say(h, keep ? "起動アプリを固定しました" : "起動アプリを最新にしました");
	} else {
		say(h, "メモリが足りないため、この操作をできませんでした");
	}
	free(out);
}

/* ---------------------------------------------------------------- 実行 */

typedef struct {
	UB	*id, *name;
	BOOL	kind;			/* offered for the object's kind, not in its applist */
} VOPAPP;

static VOPAPP	*g_app;
static INT	g_napp, g_appcap;

static void apps_free( void )
{
	INT	k;

	for ( k = 0; k < g_napp; k++ ) {
		free(g_app[k].id);
		free(g_app[k].name);
	}
	g_napp = 0;
}

/* "id\tname" of an answer to DT_RQ_APPS kept as entry g_napp */
static BOOL app_add( CONST UB *t, SZ tl, BOOL kind )
{
	VOPAPP	*a;
	INT	i, j;

	for ( i = 0; i < tl && t[i] != '\t'; i++ ) ;
	if ( i == 0 || i >= tl ) return TRUE;		/* not an entry: the next one is asked */
	if ( g_napp >= g_appcap ) {
		INT	cap = g_appcap * 2 + 4;
		VOPAPP	*na = (VOPAPP *)realloc(g_app, sizeof(VOPAPP) * (size_t)cap);

		if ( na == NULL ) return FALSE;
		g_app = na;
		g_appcap = cap;
	}
	a = &g_app[g_napp];
	a->id = (UB *)malloc((size_t)i + 1);
	a->name = (UB *)malloc((size_t)( tl - i ));
	if ( a->id == NULL || a->name == NULL ) {
		free(a->id);
		free(a->name);
		return FALSE;
	}
	memcpy(a->id, t, (size_t)i);
	a->id[i] = 0;
	for ( i++, j = 0; i < tl; i++ ) a->name[j++] = t[i];
	a->name[j] = 0;
	a->kind = kind;
	g_napp++;
	return TRUE;
}

/* 実行's list: the one the desktop's own links show (DT_RQ_APPS), asked entry by entry */
static void exec_list( CONST T_TVVOP *h, ID mid, CONST T_TVLINK *l )
{
	UB	(*rows)[WM_LABEL_MAX];
	TS_UUID	ed;
	T_DTREQ	rq;
	T_DTANS	an;
	INT	n = 1, k;

	apps_free();
	for ( k = 0; k < n; k++ ) {
		CONST UB	*t;
		SZ		tl = 0;

		tv_vop_req(h, l, DT_RQ_APPS, &rq);
		rq.recno = k;
		memset(&an, 0, sizeof(an));
		if ( ask(h, &rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		t = ans_text(h, DT_TAIL_TEXT, &tl);
		if ( !app_add(t, tl, (BOOL)( ( an.flags & DT_F_KIND ) != 0 )) ) {
			note(h, "exec: no memory for the programs");
			break;
		}
	}
	rows = malloc(WM_LABEL_MAX * (size_t)( g_napp + 1 ));
	if ( rows == NULL ) apps_free();
	for ( k = 0; k < g_napp; k++ ) snprintf((char *)rows[k], WM_LABEL_MAX, "%s", (char *)g_app[k].name);
	if ( rows != NULL && ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, g_napp);
	}
	free(rows);
}

/* ---------------------------------------------------------------- the menu */

INT tv_vop_menu( CONST T_TVVOP *h, ID mid, T_TVLINK *l )
{
	T_OBREF	ref;
	BOOL	cut, kept, open = FALSE, lockable = TRUE;

	if ( l == NULL ) {
		apps_free();
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "exec", MN_GREY);
		/* ディスク操作 away from a link: 切り離し is of a disk's link */
		if ( ( h->flags & TV_VOP_F_DISK ) != 0 ) (void)mn_chg_atr(mid, NULL, "disk.detach", MN_GREY);
		return 0;
	}
	cut = (BOOL)( no_uuid(&l->target) || ob_ref_obj(&l->target, &ref) < E_OK );
	kept = tv_vop_kept(l);
	if ( ( h->flags & TV_VOP_F_STATE ) != 0 ) {
		T_DTREQ	rq;
		T_DTANS	an;
		BOOL	detach = FALSE, grow = FALSE;

		/* whether the object is open, and has a program to be fixed to, as the desktop sees it */
		tv_vop_req(h, l, DT_RQ_LINK_STATE, &rq);
		memset(&an, 0, sizeof(an));
		if ( ask(h, &rq, &an, 3000) >= E_OK ) {
			open = (BOOL)( ( an.flags & DT_F_OPEN ) != 0 );
			lockable = (BOOL)( ( an.flags & DT_F_LOCKABLE ) != 0 );
			detach = (BOOL)( ( an.flags & DT_F_DETACHABLE ) != 0 );
			grow = (BOOL)( ( an.flags & DT_F_GROWABLE ) != 0 );
		}
		(void)mn_chg_atr(mid, NULL, "vobj.open", ( open || cut ) ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "vobj.close", open ? 0 : MN_GREY);
		if ( ( h->flags & TV_VOP_F_DISK ) != 0 ) {
			(void)mn_chg_atr(mid, NULL, "disk.detach", detach ? 0 : MN_GREY);
			(void)mn_chg_atr(mid, NULL, "disk.grow", grow ? 0 : MN_GREY);
		}
	}
	(void)mn_chg_atr(mid, NULL, "vobj.reconnect", cut ? 0 : MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "real.lock", kept ? MN_HIDE : ( cut || !lockable ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.unlock", !kept ? MN_HIDE : cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.rename", cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.duplicate", cut ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "real.newversion", cut ? MN_GREY : 0);
	exec_list(h, mid, l);
	/* nothing to run for a link that reaches no object, whatever the desktop offered */
	(void)mn_chg_atr(mid, NULL, "exec", ( cut || g_napp == 0 ) ? MN_GREY : 0);
	return cut ? 0 : g_napp;
}

/* ---------------------------------------------------------------- the items */

/* What the desktop answered the link is to become, handed to the program */
static BOOL put( CONST T_TVVOP *h, T_TVLINK *l, UINT what, CONST T_DTANS *an, CONST UB *t, SZ len )
{
	T_TVPUT	p;

	p.an = an;
	p.text = t;
	p.len = len;
	if ( !h->put(l, what, &p) ) return FALSE;
	changed(h);
	return TRUE;
}

BOOL tv_vop_disk( CONST T_TVVOP *h, CONST char *code, CONST TS_UUID *target )
{
	T_DTREQ	rq;
	T_DTANS	an;

	if ( strncmp(code, "disk.", 5) != 0 ) return FALSE;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_DISK;
	rq.recno = DT_DISK_MENU;
	if ( target != NULL ) rq.target = *target;
	part(h, DT_TAIL_NAME, (CONST UB *)code, (SZ)strlen(code));
	memset(&an, 0, sizeof(an));
	(void)ask(h, &rq, &an, 5000);
	return TRUE;
}

BOOL tv_vop_command( CONST T_TVVOP *h, T_TVLINK *l, CONST char *code, INT index )
{
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	u;
	UB	*nm;
	char	s[200];
	ER	er;

	if ( strncmp(code, "vobj.", 5) != 0 && strncmp(code, "real.", 5) != 0 && strcmp(code, "exec.app") != 0 ) {
		return FALSE;
	}
	if ( l == NULL || no_uuid(&l->target) ) return TRUE;
	if ( strcmp(code, "exec.app") == 0 && ( index < 0 || index >= g_napp ) ) return TRUE;
	/* the link may be written anew by what is done: its object kept here */
	u = l->target;
	nm = name_of(&u);
	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		(void)tv_vop_open(h, l);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		tv_vop_req(h, l, DT_RQ_CLOSE, &rq);
		(void)ask(h, &rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		tv_vop_req(h, l, DT_RQ_ATTR, &rq);
		(void)link_named(h, l, &rq);	/* the windows opened from it told its new look */
		if ( ask(h, &rq, &an, 30 * 60 * 1000) >= E_OK ) {
			(void)put(h, l, TV_VOP_W_LOOK, &an, NULL, 0);
			snprintf(s, sizeof(s), "attr %s chsz %d bgcol %06x", NM(nm), (INT)an.look.chsz,
				 (UINT)( an.look.bgcol & 0x00FFFFFFU ));
			note(h, s);
			say(h, "仮身属性を変更しました");
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		tv_vop_req(h, l, DT_RQ_RELATION, &rq);
		if ( ask(h, &rq, &an, 30 * 60 * 1000) >= E_OK ) {
			SZ		tl = 0;
			CONST UB	*t = ans_text(h, DT_TAIL_TEXT, &tl);

			(void)put(h, l, TV_VOP_W_REL, &an, t, tl);
			snprintf(s, sizeof(s), "relation %s [%.80s]", NM(nm), (CONST char *)t);
			note(h, s);
			say(h, "続柄を変更しました");
		}
	} else if ( strcmp(code, "vobj.reconnect") == 0 ) {
		T_OBREF	ref;

		if ( h->forget != NULL ) h->forget(&u);
		if ( ob_ref_obj(&u, &ref) < E_OK ) say(h, "実身に接続できませんでした");
	} else if ( strcmp(code, "real.rename") == 0 ) {
		tv_vop_req(h, l, DT_RQ_RENAME, &rq);
		if ( ask(h, &rq, &an, 30 * 60 * 1000) >= E_OK ) {
			if ( h->forget != NULL ) h->forget(&u);
			snprintf(s, sizeof(s), "renamed %.80s", (CONST char *)ans_text(h, DT_TAIL_TEXT, NULL));
			note(h, s);
		}
	} else if ( strcmp(code, "real.duplicate") == 0 || strcmp(code, "real.newversion") == 0 ) {
		BOOL	dup = (BOOL)( code[5] == 'd' );

		tv_vop_req(h, l, dup ? DT_RQ_DUPLICATE : DT_RQ_NEWVER, &rq);
		er = ask(h, &rq, &an, 30 * 60 * 1000);
		if ( er >= E_OK ) {
			/* the copy is linked to by nothing yet: its link is counted when the record is written */
			h->put_copy(l, &an.into, dup ? TV_VOP_C_DUP : TV_VOP_C_NEWVER);
			changed(h);
			snprintf(s, sizeof(s), "%s %s", dup ? "duplicated" : "new version", NM(nm));
			note(h, s);
			say(h, dup ? "実身を複製しました" : "新版を作成しました");
		}
	} else if ( strcmp(code, "real.lock") == 0 || strcmp(code, "real.unlock") == 0 ) {
		app_keep(h, l);
	} else if ( strcmp(code, "real.info") == 0 ) {
		tv_vop_req(h, l, DT_RQ_INFO, &rq);
		(void)ask(h, &rq, &an, 5000);
	} else if ( strcmp(code, "real.network") == 0 ) {
		tv_vop_req(h, l, DT_RQ_NETWORK, &rq);
		(void)ask(h, &rq, &an, 5000);
	} else if ( strcmp(code, "real.search") == 0 ) {
		tv_vop_req(h, l, DT_RQ_SEARCH, &rq);
		(void)ask(h, &rq, &an, 30 * 60 * 1000);
	} else if ( strcmp(code, "exec.app") == 0 ) {
		BOOL	kept = tv_vop_kept(l);

		tv_vop_req(h, l, DT_RQ_EXEC, &rq);
		part(h, DT_TAIL_NAME, g_app[index].id, -1);
		if ( kept ) rq.flags |= DT_F_KEEP;
		if ( g_app[index].kind ) rq.flags |= DT_F_KIND;
		er = ask(h, &rq, &an, 5000);
		snprintf(s, sizeof(s), "exec %s %s (%d)", NM(nm), (char *)g_app[index].id, (INT)er);
		note(h, s);
	}
	free(nm);
	return TRUE;
}

/* ---------------------------------------------------------------- carried out */

ER tv_vop_carry_out( CONST T_TVVOP *h, T_TVLINK *l, INT n, CONST T_DPRECT *out,
		     INT x0, INT y0, BOOL twice, BOOL *placed, T_TVCARRY *res )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	o;
	UB		**nm;
	char		s[200];
	INT		i;
	UINT		seq;
	ER		er;

	memset(res, 0, sizeof(*res));
	res->twice = twice;
	if ( n <= 0 ) return E_PAR;
	o = ( out != NULL ) ? *out : l[0].box;
	/* the names for the console, read before the store is changed by the carrying */
	nm = (UB **)calloc((size_t)n, sizeof(UB *));
	if ( nm == NULL ) return E_NOMEM;
	for ( i = 0; i < n; i++ ) nm[i] = name_of(&l[i].target);
	if ( n == 1 ) {
		tv_vop_req(h, &l[0], DT_RQ_CARRY, &rq);
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_CARRY;
		rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	}
	rq.flags |= DT_F_TO_PROG;
	rq.look.w = o.right - o.left;
	rq.look.h = o.bottom - o.top;
	rq.grab_x = x0 - o.left;
	rq.grab_y = y0 - o.top;
	memset(&an, 0, sizeof(an));
	er = ask(h, &rq, &an, 30 * 60 * 1000);
	res->an = an;
	if ( er >= E_OK ) {
		seq = rq.seq;
		res->copy = (BOOL)( an.how == DT_HOW_COPY || ( h->flags & TV_VOP_F_COPY ) != 0 );
		res->given = (BOOL)( ( an.flags & DT_F_TO_PROG ) != 0 );
		for ( i = 0; i < n; i++ ) {
			ER	e;

			tv_vop_req(h, &l[i], ( i + 1 < n ) ? DT_RQ_PLACE_MORE : DT_RQ_PLACE, &rq);
			rq.grab_x = l[i].box.left - o.left;
			rq.grab_y = l[i].box.top - o.top;
			rq.carry = seq;
			if ( twice ) rq.flags |= DT_F_DUPLICATE;
			memset(&an, 0, sizeof(an));
			e = ask(h, &rq, &an, 10 * 60 * 1000);
			snprintf(s, sizeof(s), "carried %s (%d)%s", NM(nm[i]), (INT)e,
				 twice ? " as a new object" : res->copy ? " as a copy" : "");
			note(h, s);
			if ( e >= E_OK ) res->done++;
			if ( placed != NULL ) placed[i] = (BOOL)( e >= E_OK );
		}
		res->moved = (BOOL)( res->done > 0 && !twice && !res->copy );
		if ( res->moved ) {
			if ( n == 1 ) {
				say(h, "仮身を移動しました");
			} else {
				snprintf(s, sizeof(s), "%d個の仮身を移動しました", res->done);
				say(h, s);
			}
		} else if ( res->done > 0 && !res->given ) {
			if ( n == 1 ) {
				say(h, twice ? "実身を複製しました" : "仮身を複写しました");
			} else {
				snprintf(s, sizeof(s), "%d個の仮身を%s", res->done, twice ? "実身複製しました" : "複写しました");
				say(h, s);
			}
		}
		er = E_OK;
	}
	for ( i = 0; i < n; i++ ) free(nm[i]);
	free(nm);
	return er;
}

ER tv_vop_carry( CONST T_TVASK *a, CONST TS_UUID *target, CONST char *name, CONST char *appl,
		 CONST T_DTLOOK *out, INT grab_x, INT grab_y, CONST T_DTLOOK *put, T_DTANS *an )
{
	T_DTREQ	rq;
	UINT	flags = ( appl != NULL ) ? DT_F_APPLIST : 0, seq;
	ER	er;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.target = *target;
	rq.recno = -1;
	rq.flags = flags | DT_F_TO_PROG;
	rq.look = *out;
	rq.grab_x = grab_x;
	rq.grab_y = grab_y;
	if ( name != NULL ) (void)a->part(a->arg, DT_TAIL_NAME, (CONST UB *)name, -1);
	if ( appl != NULL ) (void)a->part(a->arg, DT_TAIL_APPLIST, (CONST UB *)appl, -1);
	memset(an, 0, sizeof(*an));
	er = a->ask(a->arg, &rq, an, 30 * 60 * 1000);
	if ( er < E_OK ) return er;
	seq = rq.seq;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PLACE;
	rq.target = *target;
	rq.recno = -1;
	rq.flags = flags;
	rq.look = *put;
	rq.carry = seq;
	if ( name != NULL ) (void)a->part(a->arg, DT_TAIL_NAME, (CONST UB *)name, -1);
	if ( appl != NULL ) (void)a->part(a->arg, DT_TAIL_APPLIST, (CONST UB *)appl, -1);
	memset(an, 0, sizeof(*an));
	return a->ask(a->arg, &rq, an, 10 * 60 * 1000);
}
