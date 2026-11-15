/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ri_main.c
 *	管理情報: what the system keeps of a real object, and setting it,
 *	a program running as a process (design 18.15)
 *
 *	The desktop starts one for each object, on the link 管理情報 was
 *	chosen on (openArg "link": the object, the object whose record holds
 *	the link, its vobjid), and brings its window to the front when it is
 *	asked again for the same object (application/desktop/dtinfo.c). The
 *	window's work area is panel 1 of the data box (SYSDEF_BOX_INFO), with
 *	three tags over it.
 *
 *	実身情報 shows the object: its name and its UUID, its relationship,
 *	who made it and when, when it was changed and last looked at, how
 *	big it is, and how many links point at it and are in it. All of it
 *	is read here, from the object's metadata and records.
 *
 *	使用者管理 is the object's protection. Who owns it and its group,
 *	and what the owner, the group and everyone else may do -- read,
 *	write and run -- then whether it may be changed at all (編集不可) and
 *	deleted (削除不可); then users and groups given rights of their own
 *	beyond those three. It is read with ob_get_prt and set with
 *	ob_set_prt by this process, with the rights of the person it runs
 *	for: the owner or an administrator may change it, as before, and
 *	nothing is asked of the desktop for it.
 *
 *	付箋指定 is the list 実行 of the link's menu shows: the programs of
 *	the applist, the object's 機能付箋; for an object whose applist
 *	names none, those offered for its kind, marked so. The list comes
 *	from the desktop (DT_RQ_APPS with DT_F_INFO), which keeps the
 *	programs, and 適用 has the desktop write it (DT_RQ_INFO_APPS). The
 *	one it opens with is the one chosen in the list, its lamp lit;
 *	chosen among those of its kind, it joins the applist when 適用
 *	writes it. A program is added from those of the program box. A
 *	program's 付箋 is a link to the program object: コピー in the
 *	window's 編集 menu puts such a link in the tray, and 貼り付け takes
 *	the programs of the links in the tray into this list. Whether the
 *	link keeps to its program (起動アプリの固定) is set here too (起動固定),
 *	by the desktop in the window the link was chosen in, while that
 *	window stays open (DT_RQ_INFO_KEEP).
 *
 *	The buttons that change a list leave the window open, nothing is
 *	written until 適用, and 適用 and 取消 close it. What it does is said
 *	on the console for the tests ("info: ...").
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/obatr.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/mn.h>
#include <ts/tad.h>
#include <ts/tray.h>
#include <ts/fn.h>
#include <ts/json.h>
#include <ts/dtreq.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

#define IF_TEXT		128		/* one line of a list */
#define IF_CHSZ		14		/* a 付箋's letters */
#define IF_ID		64		/* a program's id */
#define IF_NAME		TAD_NAME_MAX

/* The panels of the data box */
#define PN_MAIN		1
#define PN_LIST		2
#define PN_TELL2	3
#define PN_TELL1	4

/* The part numbers of the main panel */
#define N_TAGS		100
#define N_ROW		200		/* eleven: what 実身情報 says */
#define N_OWNER		20
#define N_GROUP		21
#define N_MODE		30		/* nine: owner, group, others by read, write, run */
#define N_RONLY		40
#define N_PERM		41
#define N_ACL		42
#define N_ACL_R		43
#define N_ACL_W		44
#define N_ACL_X		45
#define N_APPS		50
#define N_DFLT		51
#define N_LOCK		52
#define N_B_OWNER	60
#define N_B_GROUP	61
#define N_B_ACL_ADD	62
#define N_B_ACL_DEL	63
#define N_B_APP_ADD	64
#define N_B_APP_DEL	65
#define N_NOPRT		66		/* the protection could not be read */
#define N_CANCEL	90
#define N_OK		91

#define KEY_C		0x06
#define KEY_E		0x08
#define KEY_V		0x19

#define W_USER		OB_ACE_USER
#define W_GROUP		OB_ACE_GROUP
typedef struct {
	TS_UUID	u;
	UINT	kind;			/* W_USER, W_GROUP */
} WHO;

typedef struct {
	UB	id[IF_ID];
	UB	name[IF_NAME];
	BOOL	listed;			/* in the applist; FALSE: offered for the object's kind */
} APP;

/* A program of the program box, as 追加 offers it */
typedef struct {
	TS_UUID	uuid;
	UB	id[IF_ID];
	UB	name[IF_NAME];
	UB	kind[16];
	TS_UUID	*base;			/* its templates */
	INT	nbase;
} PROG;

static UI		ui;
static T_PRCOPEN	arg;
static TS_UUID		g_obj;
static BOOL		g_closing = FALSE;

static T_OBPRT		*g_prt, *g_prt0;	/* the protection as set here, and as it was read */
static BOOL		g_prt_ok;
static APP		*g_app;
static INT		g_napp, g_appcap, g_dflt = -1;
static BOOL		g_apps_changed;
static BOOL		g_lockable, g_lock_was;

static CONST TS_UUID	nobody;
static CONST TS_UUID	u_system = OB_USER_SYSTEM_INIT;
static CONST TS_UUID	g_admin = OB_GROUP_ADMIN_INIT;
static CONST TS_UUID	u_person = OB_USER_PERSON_INIT;
static CONST TS_UUID	g_users = OB_GROUP_USERS_INIT;

static void say( CONST char *s )
{
	char	line[TAD_NAME_MAX + 64];

	snprintf(line, sizeof(line), "info: %s\n", s);
	tm_putstring((CONST UB *)line);
}

static BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

/* ---------------------------------------------------------------- words */

static INT put( UB *b, INT n, INT max, CONST char *s )
{
	INT	n0 = n;

	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = (UB)*s++;
	}
	/* cut short: never half a letter */
	while ( n > n0 && s != NULL && *s != 0 && ( (UB)*s & 0xC0 ) == 0x80 ) {
		n--;
		s--;
	}
	b[n] = 0;
	return n;
}

static INT put_num( UB *b, INT n, INT max, INT v )
{
	char	t[12];
	INT	k = 0;

	if ( v < 0 ) {
		v = 0;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 && n < max - 1 ) {
		b[n++] = (UB)t[--k];
	}
	b[n] = 0;
	return n;
}

/* A time as the metadata writes it, "2026-09-29T06:00:00.000Z", shown as "2026-09-29 06:00:00" */
static void put_time( UB *b, INT max, CONST UB *iso )
{
	INT	n = 0, i;

	for ( i = 0; iso[i] != 0 && n < max - 1; i++ ) {
		if ( i >= 19 && ( iso[i] == '.' || iso[i] == 'Z' || iso[i] == '+' ) ) {
			break;
		}
		b[n++] = ( iso[i] == 'T' ) ? ' ' : iso[i];
	}
	b[n] = 0;
}

/* ---------------------------------------------------------------- objects */

/* An object's metadata, the caller's to free; NULL when it cannot be read */
static UB *meta_of( CONST TS_UUID *u, INT *p_n )
{
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	if ( k <= 0 ) return NULL;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n <= 0 ) {
		free(m);
		return NULL;
	}
	*p_n = n;
	return m;
}

/* An object's name; 0 bytes when it has none that can be read */
static INT name_of( CONST TS_UUID *u, UB *b, INT max )
{
	T_JSON	root;
	UB	*m;
	INT	n = 0, r = 0;

	b[0] = 0;
	if ( ( m = meta_of(u, &n) ) == NULL ) return 0;
	if ( js_parse(m, n, &root) >= E_OK ) {
		r = js_get_str(&root, "name", b, max);
	}
	free(m);
	return ( r > 0 ) ? r : 0;
}

/* The name a user or group is shown by */
static void who_name( CONST TS_UUID *u, UB *b, INT max )
{
	char	t[TS_UUID_STRLEN + 1];

	if ( same_uuid(u, &nobody) ) {
		(void)put(b, 0, max, "なし");
	} else if ( same_uuid(u, &u_system) ) {
		(void)put(b, 0, max, "システム");
	} else if ( same_uuid(u, &g_admin) ) {
		(void)put(b, 0, max, "管理者");
	} else if ( same_uuid(u, &u_person) || same_uuid(u, &g_users) ) {
		(void)put(b, 0, max, "利用者");
	} else if ( name_of(u, b, max) <= 0 ) {
		ts_uuid_text(u, t);
		(void)put(b, 0, max, t);
	}
}

/* ---------------------------------------------------------------- the desktop */

/* A request to the desktop, its panels over this window; the answer waited for */
static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0;
	ID		k, ka;
	ER		er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	if ( ui.kw > 0 ) rq->win = ui.win;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

static void req_init( T_DTREQ *rq, UINT req )
{
	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	rq->target = g_obj;
	rq->recno = -1;
}

/* ---------------------------------------------------------------- panels that tell and ask */

static void tell( CONST char *l1, CONST char *l2 )
{
	CONST char	*lines[2];

	lines[0] = l1;
	lines[1] = l2;
	(void)ui_ask(&ui, ( l2 != NULL ) ? PN_TELL2 : PN_TELL1, lines, ( l2 != NULL ) ? 2 : 1);
}

/* One of the names chosen from a list headed title: its place, -1 none */
static INT list_form( CONST char *title, CONST char *CONST *names, INT n )
{
	UI	q;
	T_OBNTM	m;
	INT	num = 0, v = 0, ev, at = -1;
	BOOL	done = FALSE;

	if ( ui_dlg_open(&ui, PN_LIST, &q) < E_OK ) return -1;
	ui_set_text(&q, 1, title);
	ui_names(&q, 2, names, n);
	ui_set_value(&q, 2, 0);
	while ( !done && ui_wait(&ui, &m, TMO_FEVR) >= E_OK ) {
		ev = ui_dlg_take(&q, &m, &num);
		if ( ev == UI_EV_BUTTON ) {
			if ( num == N_OK ) {
				v = ui_value(&q, 2);
				at = ( v >= 1 && v <= n ) ? v - 1 : -1;
				done = TRUE;
			} else if ( num == N_CANCEL ) {
				done = TRUE;
			}
		} else if ( ev == UI_EV_CLOSE ) {
			done = TRUE;
		} else if ( ev == UI_EV_OTHER && ui_event(&ui, &m, &num) == UI_EV_CLOSE ) {
			g_closing = TRUE;
			done = TRUE;
		}
	}
	ui_dlg_close(&q);
	return at;
}

/* ---------------------------------------------------------------- the users and groups there are */

#define IF_WHO_ROOM	( 8 + g_prt->nacl + CNF_OB_MAX_GROUPS )

static INT who_add( WHO *w, INT n, CONST TS_UUID *u, UINT kind )
{
	INT	i;

	if ( same_uuid(u, &nobody) ) {
		return n;
	}
	for ( i = 0; i < n; i++ ) {
		if ( w[i].kind == kind && same_uuid(&w[i].u, u) ) {
			return n;
		}
	}
	w[n].u = *u;
	w[n].kind = kind;
	return n + 1;
}

/*
 * The users and groups this panel knows of: the system and the
 * administrators, the person at the machine (whom the process runs for)
 * and the ordinary users, its groups, and those the protection already
 * names. 'kind' 0 is both.
 */
static INT who_all( UINT kind, WHO *w )
{
	T_OBCRD	*crd = (T_OBCRD *)malloc(sizeof(T_OBCRD));
	INT	n = 0, i;

	if ( kind != W_GROUP ) n = who_add(w, n, &u_system, W_USER);
	if ( kind != W_USER )  n = who_add(w, n, &g_admin, W_GROUP);
	if ( crd != NULL && ob_get_crd(crd) >= E_OK ) {
		if ( kind != W_GROUP ) n = who_add(w, n, &crd->user, W_USER);
		if ( kind != W_USER )  n = who_add(w, n, &g_users, W_GROUP);
		for ( i = 0; i < crd->ngrp && kind != W_USER; i++ ) {
			n = who_add(w, n, &crd->grp[i], W_GROUP);
		}
	} else if ( kind != W_USER ) {
		n = who_add(w, n, &g_users, W_GROUP);
	}
	free(crd);
	if ( kind != W_GROUP ) n = who_add(w, n, &g_prt->owner, W_USER);
	if ( kind != W_USER )  n = who_add(w, n, &g_prt->group, W_GROUP);
	for ( i = 0; i < g_prt->nacl; i++ ) {
		if ( kind == 0 || kind == g_prt->acl[i].kind ) {
			n = who_add(w, n, &g_prt->acl[i].who, g_prt->acl[i].kind);
		}
	}
	return n;
}

/* One of them chosen from a list; 'none' offers なし first. FALSE when nothing was chosen */
static BOOL who_choose( CONST char *title, UINT kind, BOOL none, WHO *out )
{
	WHO		*w = (WHO *)malloc(sizeof(WHO) * (size_t)IF_WHO_ROOM);
	UB		(*txt)[IF_TEXT] = malloc(IF_TEXT * (size_t)( IF_WHO_ROOM + 1 ));
	CONST char	**names = malloc(sizeof(char *) * (size_t)( IF_WHO_ROOM + 1 ));
	INT		n = 0, m = 0, i, k, at;
	BOOL		ok = FALSE;

	if ( w == NULL || txt == NULL || names == NULL ) {
		goto done;
	}
	n = who_all(kind, w);
	if ( none ) {
		(void)put(txt[m], 0, IF_TEXT, "なし");
		names[m] = (CONST char *)txt[m];
		m++;
	}
	for ( i = 0; i < n; i++ ) {
		k = 0;
		if ( kind == 0 ) {
			k = put(txt[m], 0, IF_TEXT, ( w[i].kind == W_USER ) ? "利用者　" : "グループ　");
		}
		who_name(&w[i].u, txt[m] + k, IF_TEXT - k);
		names[m] = (CONST char *)txt[m];
		m++;
	}
	at = list_form(title, names, m);
	if ( at >= 0 ) {
		if ( none && at == 0 ) {
			out->u = nobody;
			out->kind = kind;
		} else {
			*out = w[at - ( none ? 1 : 0 )];
		}
		ok = TRUE;
	}
done:
	free(w);
	free(txt);
	free((void *)names);
	return ok;
}

/* ---------------------------------------------------------------- the protection */

static T_OBPRT *prt_new( INT nacl, INT nrmask )
{
	T_OBPRT	*p = (T_OBPRT *)calloc(1, OB_PRT_SIZE(nacl, nrmask));

	if ( p != NULL ) {
		p->nacl = nacl;
		p->nrmask = nrmask;
	}
	return p;
}

/* The protection with room for nacl entries, the records' limits kept after them */
static T_OBPRT *prt_room( T_OBPRT *p, INT nacl, INT nrmask )
{
	T_OBPRT	*n = prt_new(nacl, nrmask);
	INT	i;

	if ( n == NULL ) {
		return NULL;
	}
	if ( p != NULL ) {
		n->owner = p->owner;
		n->group = p->group;
		n->mode = p->mode;
		n->attr = p->attr;
		for ( i = 0; i < nacl && i < p->nacl; i++ ) n->acl[i] = p->acl[i];
		for ( i = 0; i < nrmask && i < p->nrmask; i++ ) OB_PRT_RMASK(n)[i] = OB_PRT_RMASK(p)[i];
		free(p);
	}
	return n;
}

static T_OBPRT *prt_read( CONST TS_UUID *u )
{
	T_OBPRT	*p;
	SZ	asz = 0;
	ER	er;

	er = ob_get_prt(u, NULL, 0, &asz);
	if ( ( er < E_OK && er != E_LIMIT ) || asz < (SZ)sizeof(T_OBPRT) ) {
		return NULL;
	}
	p = (T_OBPRT *)malloc((size_t)asz);
	if ( p == NULL ) return NULL;
	if ( ob_get_prt(u, p, asz, &asz) < E_OK ) {
		free(p);
		return NULL;
	}
	return p;
}

static T_OBPRT *prt_dup( CONST T_OBPRT *p )
{
	T_OBPRT	*n = (T_OBPRT *)malloc(OB_PRT_LEN(p));

	if ( n != NULL ) memcpy(n, p, OB_PRT_LEN(p));
	return n;
}

/* ---------------------------------------------------------------- the panel kept up to date */

static void show_owner( void )
{
	UB	b[IF_TEXT];

	who_name(&g_prt->owner, b, sizeof(b));
	ui_set_text(&ui, N_OWNER, (CONST char *)b);
	who_name(&g_prt->group, b, sizeof(b));
	ui_set_text(&ui, N_GROUP, (CONST char *)b);
}

/* A list's names, its value kept within them */
static void set_names( INT num, CONST char *CONST *names, INT n )
{
	ui_names(&ui, num, names, n);
	if ( ui_value(&ui, num) > n ) ui_set_value(&ui, num, n);
}

static void show_acl( void )
{
	UB		(*txt)[IF_TEXT] = malloc(IF_TEXT * (size_t)( g_prt->nacl + 1 ));
	CONST char	**names = malloc(sizeof(char *) * (size_t)( g_prt->nacl + 1 ));
	INT		i, k;

	if ( txt == NULL || names == NULL ) goto done;
	for ( i = 0; i < g_prt->nacl; i++ ) {
		CONST T_OBACE	*a = &g_prt->acl[i];
		UB		*b = txt[i];
		BOOL		any = FALSE;

		k = put(b, 0, IF_TEXT, ( a->kind == OB_ACE_GROUP ) ? "グループ　" : "利用者　");
		who_name(&a->who, b + k, IF_TEXT - 40 - k);
		while ( b[k] != 0 ) k++;
		k = put(b, k, IF_TEXT, "：");
		if ( a->ops & OB_OP_READ )  { k = put(b, k, IF_TEXT, "読出し"); any = TRUE; }
		if ( a->ops & OB_OP_WRITE ) { k = put(b, k, IF_TEXT, any ? "・書込み" : "書込み"); any = TRUE; }
		if ( a->ops & OB_OP_EXEC )  { k = put(b, k, IF_TEXT, any ? "・実行" : "実行"); any = TRUE; }
		if ( !any ) (void)put(b, k, IF_TEXT, "なし");
		names[i] = (CONST char *)b;
	}
	set_names(N_ACL, names, g_prt->nacl);
done:
	free(txt);
	free((void *)names);
}

static void show_apps( void )
{
	UB		(*txt)[IF_TEXT] = malloc(IF_TEXT * (size_t)( g_napp + 1 ));
	CONST char	**names = malloc(sizeof(char *) * (size_t)( g_napp + 1 ));
	UB		b[IF_TEXT];
	INT		i, k;

	if ( txt == NULL || names == NULL ) goto done;
	for ( i = 0; i < g_napp; i++ ) {
		k = put(txt[i], 0, IF_TEXT - 44, (CONST char *)g_app[i].name);
		k = put(txt[i], k, IF_TEXT, "（");
		k = put(txt[i], k, IF_TEXT - 40, (CONST char *)g_app[i].id);
		if ( !g_app[i].listed ) k = put(txt[i], k, IF_TEXT - 4, "、実身の種類から");
		(void)put(txt[i], k, IF_TEXT, "）");
		names[i] = (CONST char *)txt[i];
	}
	set_names(N_APPS, names, g_napp);
	/* the one it opens with is the one chosen in the list, its lamp lit */
	ui_set_now(&ui, N_APPS, ui_value(&ui, N_APPS), ( g_dflt >= 0 && g_dflt < g_napp ) ? g_dflt + 1 : 0);
	(void)put(b, 0, sizeof(b), ( g_dflt >= 0 && g_dflt < g_napp ) ? (CONST char *)g_app[g_dflt].name : "なし");
	ui_set_text(&ui, N_DFLT, (CONST char *)b);
done:
	free(txt);
	free((void *)names);
}

/* A program chosen in the list: the object opens with it from now */
static void apps_follow( void )
{
	INT	v = ui_value(&ui, N_APPS) - 1;

	if ( v >= 0 && v < g_napp && v != g_dflt ) {
		g_dflt = v;
		g_apps_changed = TRUE;
		ui_set_now(&ui, N_APPS, v + 1, v + 1);
		ui_set_text(&ui, N_DFLT, (CONST char *)g_app[v].name);
	}
}

/* ---------------------------------------------------------------- the programs */

static BOOL app_room( INT want )
{
	INT	cap = ( g_appcap > 0 ) ? g_appcap : 8;
	APP	*a;

	if ( want <= g_appcap ) return TRUE;
	while ( cap < want ) cap *= 2;
	a = (APP *)realloc(g_app, sizeof(APP) * (size_t)cap);
	if ( a == NULL ) return FALSE;
	g_app = a;
	g_appcap = cap;
	return TRUE;
}

static void app_add( CONST UB *id, CONST UB *name )
{
	INT	i;

	for ( i = 0; i < g_napp; i++ ) {
		if ( strcmp((CONST char *)g_app[i].id, (CONST char *)id) == 0 ) {
			if ( !g_app[i].listed ) {
				g_app[i].listed = TRUE;	/* one of its kind, now in the applist */
				g_apps_changed = TRUE;
			}
			return;			/* already in it */
		}
	}
	if ( !app_room(g_napp + 1) ) return;
	(void)put(g_app[g_napp].id, 0, IF_ID, (CONST char *)id);
	(void)put(g_app[g_napp].name, 0, IF_NAME, (CONST char *)name);
	g_app[g_napp].listed = TRUE;
	if ( g_dflt < 0 ) g_dflt = g_napp;
	g_napp++;
	g_apps_changed = TRUE;
}

/* The programs of the program box, with their kinds and templates: how many, *p_all to free with progs_free */
static INT progs_read( PROG **p_all )
{
	TS_UUID	box, *lk = NULL;
	PROG	*all;
	INT	n, i, k = 0;

	*p_all = NULL;
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) < E_OK || ( n = ui_lst_lnk(&box, &lk) ) <= 0 ) {
		free(lk);
		return 0;
	}
	all = (PROG *)calloc((size_t)n, sizeof(PROG));
	if ( all == NULL ) {
		free(lk);
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		T_JSON	root, tf, g, base, it;
		UB	*m, us[TS_UUID_STRLEN + 1];
		INT	len = 0, b;

		if ( ( m = meta_of(&lk[i], &len) ) == NULL ) continue;
		if ( js_parse(m, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
		  && js_get(&tf, "program", &g) >= E_OK && js_get_str(&g, "id", all[k].id, IF_ID) > 0 ) {
			all[k].uuid = lk[i];
			if ( js_get_str(&g, "name", all[k].name, IF_NAME) <= 0 ) {
				(void)js_get_str(&root, "name", all[k].name, IF_NAME);
			}
			if ( js_get_str(&g, "kind", all[k].kind, sizeof(all[k].kind)) <= 0 ) {
				(void)put(all[k].kind, 0, sizeof(all[k].kind), "app");
			}
			if ( js_get(&g, "base", &base) >= E_OK ) {
				for ( b = 0, it.s = NULL; js_next(&base, &it); ) b++;
				all[k].base = ( b > 0 ) ? (TS_UUID *)calloc((size_t)b, sizeof(TS_UUID)) : NULL;
				for ( it.s = NULL; all[k].base != NULL && js_next(&base, &it); ) {
					if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
					  && ts_str_to_uuid((CONST char *)us, &all[k].base[all[k].nbase]) >= E_OK ) {
						all[k].nbase++;
					}
				}
			}
			k++;
		}
		free(m);
	}
	free(lk);
	*p_all = all;
	return k;
}

static void progs_free( PROG *all, INT n )
{
	INT	i;

	for ( i = 0; all != NULL && i < n; i++ ) free(all[i].base);
	free(all);
}

static void choose_app( void )
{
	PROG		*all = NULL;
	INT		n = progs_read(&all), i, k, at;
	UB		(*txt)[IF_TEXT] = malloc(IF_TEXT * (size_t)( n + 1 ));
	CONST char	**names = malloc(sizeof(char *) * (size_t)( n + 1 ));
	INT		*of = malloc(sizeof(INT) * (size_t)( n + 1 ));

	if ( txt == NULL || names == NULL || of == NULL ) goto done;
	/* a command opens no object: it is not one to add */
	for ( i = k = 0; i < n; i++ ) {
		CONST char	*kd = (CONST char *)all[i].kind;
		INT		j;

		if ( strcmp(kd, "command") == 0 ) continue;
		j = put(txt[k], 0, IF_TEXT - 44, (CONST char *)all[i].name);
		(void)put(txt[k], j, IF_TEXT, ( strcmp(kd, "accessory") == 0 ) ? "　（小物）"
					   : ( strcmp(kd, "utility") == 0 ) ? "　（道具）" : "　（アプリケーション）");
		names[k] = (CONST char *)txt[k];
		of[k++] = i;
	}
	at = list_form("機能付箋に加えるプログラム", names, k);
	if ( at >= 0 && at < k ) {
		app_add(all[of[at]].id, all[of[at]].name);
	}
done:
	free(txt);
	free((void *)names);
	free(of);
	progs_free(all, n);
}

/* The 付箋 of the program chosen put in the tray: a link to the program object */
static void copy_tag( INT sel )
{
	PROG	*all = NULL;
	INT	n, i, w;
	T_TAD	*frag = NULL;
	T_VOBJ	*lv;
	TS_UUID	id;
	CONST char *head = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>";

	if ( sel < 0 || sel >= g_napp ) {
		tell("複写する機能付箋を選んでください", NULL);
		return;
	}
	n = progs_read(&all);
	for ( i = 0; i < n && strcmp((CONST char *)all[i].id, (CONST char *)g_app[sel].id) != 0; i++ ) ;
	if ( i == n ) {
		progs_free(all, n);
		tell("このプログラムはシステムにありません", (CONST char *)g_app[sel].id);
		return;
	}
	lv = (T_VOBJ *)calloc(1, sizeof(T_VOBJ));
	if ( lv != NULL && tad_parse((CONST UB *)head, (SZ)strlen(head), NULL, &frag) >= E_OK && frag != NULL ) {
		ID	fid = fn_system();
		INT	band = tad_closed_h(IF_CHSZ, TAD_D_DEFAULT);
		UB	*buf = NULL;
		SZ	size = 0;

		lv->target = all[i].uuid;
		lv->recno = 0;
		(void)put(lv->name, 0, TAD_NAME_MAX, (CONST char *)all[i].name);
		if ( fid > 0 ) (void)fn_set_size(fid, IF_CHSZ);
		w = ( fid > 0 ) ? fn_width(fid, lv->name) : IF_CHSZ * 6;
		lv->right = tad_closed_w(IF_CHSZ, TAD_D_DEFAULT, w);
		lv->bottom = band;
		lv->height = band;
		lv->frcol = 0x000000;
		lv->chcol = 0x000000;
		lv->tbcol = 0xFFFFFF;
		lv->bgcol = 0xFFFFFF;
		lv->chsz = IF_CHSZ;
		lv->disp = TAD_D_DEFAULT;
		if ( tad_lnk_add(frag, lv, &id) >= E_OK ) {
			(void)tad_write_mem(frag, NULL, 0, &size);
			buf = (UB *)malloc((size_t)size + 1);
			if ( buf != NULL && tad_write_mem(frag, buf, size + 1, &size) >= E_OK ) {
				T_TRREC	r;

				memset(&r, 0, sizeof(r));
				r.kind = TR_TAD;
				r.data = buf;
				r.size = size;
				if ( tr_psh_dat(&r, 1, (CONST UB *)"機能付箋") >= E_OK ) say("copied");
			}
			free(buf);
		}
	}
	if ( frag != NULL ) tad_free(frag);
	free(lv);
	progs_free(all, n);
}

/* The set in hand in the tray, as a fragment; NULL none */
static T_TAD *tray_frag( void )
{
	T_TRINF	inf;
	T_TAD	*doc = NULL;
	UB	*b;
	SZ	asz = 0;
	INT	i;

	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( inf.kind != TR_TAD || inf.size <= 0 ) continue;
		b = (UB *)malloc((size_t)inf.size + 1);
		if ( b == NULL ) break;
		if ( tr_rea_rec(TR_HAND, i, b, inf.size, &asz) >= E_OK && tad_parse(b, asz, NULL, &doc) >= E_OK ) {
			free(b);
			return doc;
		}
		free(b);
	}
	return NULL;
}

static BOOL tray_has( void )
{
	T_TRSTS	*s = (T_TRSTS *)malloc(sizeof(T_TRSTS));
	BOOL	has = FALSE;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS
	  && s->set[s->hand].nrec > 0 ) {
		has = TRUE;
	}
	free(s);
	return has;
}

/* The programs of the links in the tray taken into the list: the program object, or one of its templates */
static void paste_tags( void )
{
	T_TAD	*frag = tray_frag();
	T_VOBJ	*lv = (T_VOBJ *)malloc(sizeof(T_VOBJ));
	PROG	*all = NULL;
	INT	n, np, i, k, b, got = 0;

	np = progs_read(&all);
	if ( frag != NULL && lv != NULL ) {
		n = tad_lnk_count(frag);
		for ( i = 0; i < n; i++ ) {
			if ( tad_lnk_get(frag, i, lv) < E_OK ) continue;
			for ( k = 0; k < np; k++ ) {
				BOOL	is = same_uuid(&all[k].uuid, &lv->target);

				for ( b = 0; !is && b < all[k].nbase; b++ ) is = same_uuid(&all[k].base[b], &lv->target);
				if ( is ) {
					app_add(all[k].id, all[k].name);
					got++;
					break;
				}
			}
		}
	}
	if ( frag != NULL ) tad_free(frag);
	free(lv);
	progs_free(all, np);
	if ( got == 0 ) {
		tell("トレーにプログラムの付箋がありません", NULL);
	}
}

/* ---------------------------------------------------------------- the buttons */

static void button( INT num )
{
	INT	sel = ui_value(&ui, N_APPS) - 1, i;
	WHO	w;

	switch ( num ) {
	case N_B_OWNER:
		if ( who_choose("所有者", W_USER, TRUE, &w) ) {
			g_prt->owner = w.u;
			show_owner();
		}
		break;
	case N_B_GROUP:
		if ( who_choose("グループ", W_GROUP, TRUE, &w) ) {
			g_prt->group = w.u;
			show_owner();
		}
		break;
	case N_B_ACL_ADD: {
		UINT	ops = 0;

		if ( ui_value(&ui, N_ACL_R) != 0 ) ops |= OB_OP_R;
		if ( ui_value(&ui, N_ACL_W) != 0 ) ops |= OB_OP_W;
		if ( ui_value(&ui, N_ACL_X) != 0 ) ops |= OB_OP_EXEC;
		if ( !who_choose("個別の利用権を与える相手", 0, FALSE, &w) ) {
			break;
		}
		/* one entry for each: given again, it is the rights that change */
		for ( i = 0; i < g_prt->nacl; i++ ) {
			if ( g_prt->acl[i].kind == w.kind && same_uuid(&g_prt->acl[i].who, &w.u) ) break;
		}
		if ( i == g_prt->nacl ) {
			T_OBPRT	*np = prt_room(g_prt, g_prt->nacl + 1, g_prt->nrmask);

			if ( np == NULL ) break;
			g_prt = np;
		}
		g_prt->acl[i].kind = w.kind;
		g_prt->acl[i].who = w.u;
		g_prt->acl[i].ops = ops;
		show_acl();
		break;
	}
	case N_B_ACL_DEL:
		i = ui_value(&ui, N_ACL) - 1;
		if ( i >= 0 && i < g_prt->nacl ) {
			T_OBPRT	*np;

			for ( ; i < g_prt->nacl - 1; i++ ) g_prt->acl[i] = g_prt->acl[i + 1];
			/* one entry fewer: the limits move up after the entries left */
			np = prt_room(g_prt, g_prt->nacl - 1, g_prt->nrmask);
			if ( np != NULL ) g_prt = np;
			show_acl();
		}
		break;
	case N_B_APP_ADD:
		choose_app();
		show_apps();
		break;
	case N_B_APP_DEL:
		if ( sel >= 0 && sel < g_napp && !g_app[sel].listed ) {
			tell("実身の種類で選べるプログラムは削除できません", (CONST char *)g_app[sel].name);
		} else if ( sel >= 0 && sel < g_napp ) {
			for ( i = sel; i < g_napp - 1; i++ ) g_app[i] = g_app[i + 1];
			g_napp--;
			if ( g_dflt == sel ) g_dflt = ( g_napp > 0 ) ? 0 : -1;
			else if ( g_dflt > sel ) g_dflt--;
			g_apps_changed = TRUE;
			show_apps();
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- reading and writing */

/* What 実身情報 says, read from the object itself */
static void show_info( void )
{
	T_JSON	root;
	T_OBREC	rec[64];
	UB	*m, b[IF_TEXT], t[IF_TEXT];
	char	us[TS_UUID_STRLEN + 1];
	INT	n = 0, i, cnt = 0, bytes = 0, links = 0;
	D	d;
	ID	k;

	m = meta_of(&g_obj, &n);
	if ( m == NULL || js_parse(m, n, &root) < E_OK ) {
		free(m);
		m = NULL;
	}
	b[0] = 0;
	if ( m != NULL ) (void)js_get_str(&root, "name", b, sizeof(b));
	ui_set_text(&ui, N_ROW + 0, (CONST char *)b);
	ts_uuid_text(&g_obj, us);
	ui_set_text(&ui, N_ROW + 1, us);
	/* the relationship, each in brackets with a space between */
	b[0] = 0;
	if ( m != NULL ) {
		T_JSON	rel, it;
		INT	k2 = 0;

		if ( js_get(&root, "relationship", &rel) >= E_OK ) {
			for ( it.s = NULL; js_next(&rel, &it); ) {
				if ( js_str(&it, t, sizeof(t)) < 0 ) continue;
				if ( k2 > 0 ) k2 = put(b, k2, sizeof(b), " ");
				k2 = put(b, k2, sizeof(b), "[");
				k2 = put(b, k2, sizeof(b), (CONST char *)t);
				k2 = put(b, k2, sizeof(b), "]");
			}
		}
	}
	ui_set_text(&ui, N_ROW + 2, (CONST char *)b);
	b[0] = 0;
	if ( m != NULL ) (void)js_get_str(&root, "maker", b, sizeof(b));
	ui_set_text(&ui, N_ROW + 3, (CONST char *)b);
	{
		CONST char	*keys[3] = { "makeDate", "updateDate", "accessDate" };

		for ( i = 0; i < 3; i++ ) {
			t[0] = 0;
			if ( m != NULL ) (void)js_get_str(&root, keys[i], t, sizeof(t));
			put_time(b, sizeof(b), t);
			ui_set_text(&ui, N_ROW + 4 + i, (CONST char *)b);
		}
	}
	k = ob_opn_obj(&g_obj, OB_OP_ATRRD);
	if ( k > 0 && ob_lst_rec(k, rec, 64, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 64; i++ ) {
			if ( rec[i].recno == 0 ) bytes = (INT)rec[i].size;
		}
	}
	if ( k > 0 ) ob_cls_obj(k);
	n = put_num(b, 0, sizeof(b), bytes);
	(void)put(b, n, sizeof(b), " バイト");
	ui_set_text(&ui, N_ROW + 7, (CONST char *)b);
	d = 0;
	if ( m != NULL ) d = js_get_num(&root, "recordCount", 0);
	(void)put_num(b, 0, sizeof(b), (INT)d);
	ui_set_text(&ui, N_ROW + 8, (CONST char *)b);
	d = 0;
	if ( m != NULL ) d = js_get_num(&root, "refCount", 0);
	(void)put_num(b, 0, sizeof(b), (INT)d);
	ui_set_text(&ui, N_ROW + 9, (CONST char *)b);
	{
		UB	*r = NULL;
		T_TAD	*doc = NULL;
		INT	len = ui_rec_all(&g_obj, 0, 0, &r);

		if ( len > 0 && tad_parse(r, len, NULL, &doc) >= E_OK && doc != NULL ) {
			links = tad_lnk_count(doc);
			tad_free(doc);
		}
		free(r);
	}
	(void)put_num(b, 0, sizeof(b), links);
	ui_set_text(&ui, N_ROW + 10, (CONST char *)b);
	free(m);
}

/* The protection, the list 実行 shows and 起動固定, as they are now */
static void load( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	INT	i, n = 1, k;

	g_prt = prt_read(&g_obj);
	g_prt_ok = (BOOL)( g_prt != NULL );
	if ( !g_prt_ok ) g_prt = prt_new(0, 0);
	g_prt0 = ( g_prt != NULL ) ? prt_dup(g_prt) : NULL;
	if ( g_prt == NULL || g_prt0 == NULL ) g_prt_ok = FALSE;

	g_napp = 0;
	g_dflt = -1;
	for ( i = 0; i < n; i++ ) {
		req_init(&rq, DT_RQ_APPS);
		rq.recno = i;
		rq.flags = DT_F_INFO;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 5000) < E_OK || !app_room(g_napp + 1) ) break;
		n = (INT)an.how;
		an.text[DT_TEXT_MAX - 1] = 0;
		for ( k = 0; an.text[k] != 0 && an.text[k] != '\t'; k++ ) ;
		if ( an.text[k] != '\t' ) continue;
		an.text[k] = 0;
		(void)put(g_app[g_napp].id, 0, IF_ID, (CONST char *)an.text);
		(void)put(g_app[g_napp].name, 0, IF_NAME, (CONST char *)an.text + k + 1);
		g_app[g_napp].listed = (BOOL)( ( an.flags & DT_F_KIND ) == 0 );
		if ( ( an.flags & DT_F_DEFAULT ) != 0 ) g_dflt = g_napp;
		g_napp++;
	}
}

/* What the desktop says of 起動固定, and where the window the link was chosen in stands */
static void lock_state( INT *p_x, INT *p_y )
{
	T_DTREQ	rq;
	T_DTANS	an;

	req_init(&rq, DT_RQ_INFO_KEEP);
	rq.flags = DT_F_ASK;
	memset(&an, 0, sizeof(an));
	an.look.w = an.look.h = -1;
	g_lockable = g_lock_was = FALSE;
	if ( ask_desktop(&rq, &an, 5000) >= E_OK ) {
		g_lockable = (BOOL)( ( an.flags & DT_F_LOCKABLE ) != 0 );
		g_lock_was = (BOOL)( ( an.flags & DT_F_KEEP ) != 0 );
	}
	*p_x = ( an.look.w >= 0 ) ? an.look.w + 40 : 80;
	*p_y = ( an.look.h >= 0 ) ? an.look.h + 40 : 80;
}

/* What was set, written: FALSE when the protection could not be */
static BOOL apply( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	BOOL	ok = TRUE, lock;
	INT	r, c, i, n, at;

	if ( g_prt_ok ) {
		g_prt->mode &= ~0777U;
		for ( r = 0; r < 3; r++ ) {
			for ( c = 0; c < 3; c++ ) {
				if ( ui_value(&ui, N_MODE + r * 3 + c) != 0 ) g_prt->mode |= 0400U >> ( r * 3 + c );
			}
		}
		g_prt->attr &= ~( OB_A_RONLY | OB_A_PERM );
		if ( ui_value(&ui, N_RONLY) != 0 ) g_prt->attr |= OB_A_RONLY;
		if ( ui_value(&ui, N_PERM) != 0 )  g_prt->attr |= OB_A_PERM;
		if ( !( OB_PRT_LEN(g_prt) == OB_PRT_LEN(g_prt0) && memcmp(g_prt, g_prt0, OB_PRT_LEN(g_prt)) == 0 )
		  && ob_set_prt(&g_obj, g_prt) < E_OK ) {
			ok = FALSE;
		}
	}
	if ( g_apps_changed ) {
		/* the applist's own, and the one it opens with when it is one of its kind */
		req_init(&rq, DT_RQ_INFO_APPS);
		for ( i = n = 0, at = -1; i < g_napp; i++ ) {
			if ( !g_app[i].listed && i != g_dflt ) continue;
			if ( i == g_dflt ) at = n;
			n++;
		}
		c = snprintf((char *)rq.text, sizeof(rq.text), "%d", at);
		for ( i = 0; i < g_napp && c < (INT)sizeof(rq.text); i++ ) {
			if ( !g_app[i].listed && i != g_dflt ) continue;
			c += snprintf((char *)rq.text + c, sizeof(rq.text) - (size_t)c, "\n%s\t%s",
				      (CONST char *)g_app[i].id, (CONST char *)g_app[i].name);
		}
		if ( c >= (INT)sizeof(rq.text) || ask_desktop(&rq, &an, 5000) < E_OK ) ok = FALSE;
	}
	/* fixed or let go; still fixed with another default chosen, fixed to that one */
	lock = (BOOL)( ui_value(&ui, N_LOCK) != 0 );
	if ( g_lockable && ( lock != g_lock_was || ( lock && g_apps_changed ) ) ) {
		req_init(&rq, DT_RQ_INFO_KEEP);
		rq.flags = lock ? DT_F_KEEP : 0;
		(void)ask_desktop(&rq, &an, 5000);
	}
	return ok;
}

/* ---------------------------------------------------------------- the window */

static BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

static void edit( BOOL copy )
{
	if ( copy ) {
		copy_tag(ui_value(&ui, N_APPS) - 1);
	} else {
		paste_tags();
		show_apps();
	}
}

/* 閉じる, 編集's コピー and 貼り付け (on the 機能付箋 chosen on 付箋指定), and the tail */
static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	INT	s = ui_value(&ui, N_APPS) - 1;
	BOOL	chosen;
	char	line[80];

	if ( ui_uuid(SYSDEF_MENU_INFO, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "info.copy", ( s >= 0 && s < g_napp ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "info.paste", tray_has() ? 0 : MN_GREY);
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	snprintf(line, sizeof(line), "chosen %s", (CONST char *)sel.code);
	say(line);
	if ( is_code(&sel, "close") ) {
		g_closing = TRUE;
	} else if ( is_code(&sel, "info.copy") ) {
		edit(TRUE);
	} else if ( is_code(&sel, "info.paste") ) {
		edit(FALSE);
	}
}

static void event( CONST T_OBNTM *m )
{
	INT	num = 0;

	switch ( ui_event(&ui, m, &num) ) {
	case UI_EV_CLOSE:
		g_closing = TRUE;
		break;
	case UI_EV_MENU:
		menu(m);
		break;
	case UI_EV_KEY:
		if ( ( m->mods & OB_KEY_UP ) == 0 && ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
			UINT	c = wm_key_cmd(m->code);

			if ( c == KEY_E ) g_closing = TRUE;
			else if ( c == KEY_C ) edit(TRUE);
			else if ( c == KEY_V && tray_has() ) edit(FALSE);
		}
		break;
	case UI_EV_CHANGE:
		if ( num == N_APPS ) apps_follow();
		break;
	case UI_EV_BUTTON:
		if ( num == N_OK ) {
			if ( !apply() ) {
				tell("使用者管理を変更できませんでした", "所有者か管理者だけが変更できます");
				break;
			}
			say("applied");
			g_closing = TRUE;
		} else if ( num == N_CANCEL ) {
			g_closing = TRUE;
		} else {
			button(num);
		}
		break;
	default:
		break;
	}
}

/* The window's name: the object's with の管理情報 after it, written as JSON says a string */
static void title_set( char *t, INT max )
{
	UB	nm[TAD_NAME_MAX];
	char	json[TAD_NAME_MAX * 2 + 48];
	INT	n, i, k;

	(void)name_of(&g_obj, nm, sizeof(nm));
	n = put((UB *)t, 0, max, (CONST char *)nm);
	(void)put((UB *)t, n, max, "の管理情報");
	k = snprintf(json, sizeof(json), "{\"name\":\"");
	for ( i = 0; t[i] != 0 && k < (INT)sizeof(json) - 4; i++ ) {
		if ( t[i] == '"' || t[i] == '\\' ) json[k++] = '\\';
		json[k++] = t[i];
	}
	k += snprintf(json + k, sizeof(json) - (size_t)k, "\"}");
	(void)ob_set_atr(ui.kw, (CONST UB *)json, (SZ)k);
}

int main( void )
{
	T_OBNTM	m;
	char	title[TAD_NAME_MAX + 24], line[TAD_NAME_MAX + 40];
	SZ	n;
	INT	x, y, r, c;

	memset(&arg, 0, sizeof(arg));
	n = ts_get_arg(&arg, sizeof(arg));
	if ( n < (SZ)sizeof(TS_UUID) ) {
		say("no object");
		return 1;
	}
	g_obj = arg.target;
	load();
	lock_state(&x, &y);
	ui_icon(SYSDEF_PROG_INFO);
	ui_attr(WM_ATTR_RESIZE | UI_ATTR_HIDDEN);
	if ( ui_open(&ui, SYSDEF_BOX_INFO, PN_MAIN, x, y) < E_OK ) {
		say("no window");
		return 1;
	}
	title_set(title, sizeof(title));
	show_info();
	/* 使用者管理 as the protection reads, every part of it off when it cannot be read */
	for ( r = 0; r < 3; r++ ) {
		for ( c = 0; c < 3; c++ ) {
			ui_set_value(&ui, N_MODE + r * 3 + c, ( g_prt->mode & ( 0400U >> ( r * 3 + c ) ) ) ? 1 : 0);
		}
	}
	ui_set_value(&ui, N_RONLY, ( g_prt->attr & OB_A_RONLY ) ? 1 : 0);
	ui_set_value(&ui, N_PERM, ( g_prt->attr & OB_A_PERM ) ? 1 : 0);
	ui_show(&ui, N_NOPRT, (BOOL)!g_prt_ok);
	if ( !g_prt_ok ) {
		CONST INT	off[] = { N_B_OWNER, N_B_GROUP, N_RONLY, N_PERM, N_ACL, N_ACL_R, N_ACL_W, N_ACL_X,
					  N_B_ACL_ADD, N_B_ACL_DEL };

		for ( r = 0; r < 9; r++ ) ui_off(&ui, N_MODE + r, TRUE);
		for ( r = 0; r < (INT)( sizeof(off) / sizeof(off[0]) ); r++ ) ui_off(&ui, off[r], TRUE);
	}
	ui_set_value(&ui, N_LOCK, g_lock_was ? 1 : 0);
	ui_off(&ui, N_LOCK, (BOOL)!g_lockable);
	show_owner();
	show_acl();
	show_apps();
	ui_reveal(&ui);
	snprintf(line, sizeof(line), "opened %s", title);
	say(line);

	while ( !g_closing && ui_wait(&ui, &m, TMO_FEVR) >= E_OK ) {
		event(&m);
	}
	ui_close(&ui);
	free(g_prt);
	free(g_prt0);
	free(g_app);
	say("closed");
	return 0;
}
