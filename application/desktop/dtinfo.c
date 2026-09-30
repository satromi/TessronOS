/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtinfo.c
 *	管理情報: what the system keeps of a real object, and setting it
 *	(design 18.15)
 *
 *	One panel with three tags over it.
 *
 *	実身情報 shows the object: its name and its UUID, its relationship,
 *	who made it and when, when it was changed and last looked at, how
 *	big it is, and how many links point at it and are in it.
 *
 *	使用者管理 is the object's protection. Who owns it and its group,
 *	and what the owner, the group and everyone else may do -- read,
 *	write and run -- as the users of the system have it; then whether
 *	it may be changed at all (編集不可) and deleted (削除不可); then
 *	users and groups given rights of their own beyond those three.
 *	It is set with ob_set_prt, which writes the editable and deletable
 *	of the metadata to agree, so that what the TADjs Desktop reads
 *	says the same.
 *
 *	付箋指定 is the applist: the programs the object can be opened with
 *	(its 機能付箋), and the one it opens with: the one chosen in the
 *	list, its lamp lit. A program is added from those the system holds, so
 *	that what is named is a program that can be run. A program's 付箋
 *	is a link to the program object: コピー in the window's 編集 menu
 *	puts such a link in the tray, and 貼り付け takes the programs of the
 *	links in the tray -- a 付箋 copied from another object's list, or a
 *	link taken from the program box -- into this list. Whether the link
 *	the panel was opened from keeps to its program (起動アプリの固定)
 *	is set here too (起動固定).
 *
 *	It is a window of its own, not a panel that holds the desktop
 *	still: the buttons that change a list leave it open (WM_ANS_ACT),
 *	nothing is written until 適用, and 適用 and 取消 close it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/fn.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define IF_W		460		/* the panel, inside its frame */
#define IF_H		420
#define IF_APP_MAX	16		/* programs in the applist */
#define IF_WHO_MAX	(8 + OB_ACL_MAX + CNF_OB_MAX_GROUPS)
#define IF_TEXT		96		/* one line of a list */
#define IF_CHSZ		14		/* a 付箋's letters */

/* The part numbers */
#define N_TAGS		100
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
#define N_BTN		60

/* What the buttons that leave the panel open do */
#define A_OWNER		( WM_ANS_ACT + 0 )
#define A_GROUP		( WM_ANS_ACT + 1 )
#define A_ACL_ADD	( WM_ANS_ACT + 2 )
#define A_ACL_DEL	( WM_ANS_ACT + 3 )
#define A_APP_ADD	( WM_ANS_ACT + 4 )
#define A_APP_DEL	( WM_ANS_ACT + 5 )

/* A user or a group the panel can name */
#define W_USER		OB_ACE_USER
#define W_GROUP		OB_ACE_GROUP
typedef struct {
	TS_UUID	u;
	UINT	kind;			/* W_USER, W_GROUP */
} WHO;

/* One window of 管理情報, and what its panel is setting until 適用 */
typedef struct {
	BOOL		used;
	INT		wid;		/* the window */
	INT		pid;		/* the panel filling it */
	BOOL		moving;		/* carried by its band */
	INT		grab_dx, grab_dy;

	/*
	 * The window the link was chosen in, while it stays open: what
	 * asks and tells are put over, and where 起動アプリの固定 is
	 * changed. 'd' is that window, or another of the desktop's when it
	 * has closed.
	 */
	DTWIN		*d;
	DTWIN		*parent;
	INT		parent_wid;
	T_VOBJ		link;		/* the link, as it was when chosen */
	CONST T_VOBJ	*v;		/* &link */
	BOOL		lock_was;

	BOOL		prt_ok;		/* the protection could be read */
	T_OBPRT		prt, prt0;

	INT		napp, dflt;
	UB		id[IF_APP_MAX][OM_APP_ID];
	UB		name[IF_APP_MAX][OM_APP_NAME];
	BOOL		apps_changed;

	/* the indices of the parts set anew while the panel is open */
	INT		i_owner, i_group, i_acl, i_apps, i_dflt;
} INFO;

LOCAL CONST TS_UUID	nobody;		/* all zeroes: no owner */

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT n, INT max, CONST char *s )
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

LOCAL INT put_num( UB *b, INT n, INT max, INT v )
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
LOCAL void put_time( UB *b, INT max, CONST UB *iso )
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

/* The name a user or group is shown by */
LOCAL void who_name( CONST TS_UUID *u, UB *b, INT max )
{
	char	t[TS_UUID_STRLEN + 1];

	if ( ts_uuid_cmp(u, &nobody) == 0 ) {
		(void)put(b, 0, max, "なし");
	} else if ( ts_uuid_cmp(u, &ob_user_system) == 0 ) {
		(void)put(b, 0, max, "システム");
	} else if ( ts_uuid_cmp(u, &ob_group_admin) == 0 ) {
		(void)put(b, 0, max, "管理者");
	} else if ( om_store_name(u, b, max) <= 0 ) {
		if ( ts_uuid_to_str(u, t, sizeof(t)) < E_OK ) t[0] = 0;
		(void)put(b, 0, max, t);
	}
}

/* ---------------------------------------------------------------- the users and groups there are */

LOCAL INT who_add( WHO *w, INT n, CONST TS_UUID *u, UINT kind )
{
	INT	i;

	if ( n >= IF_WHO_MAX || ts_uuid_cmp(u, &nobody) == 0 ) {
		return n;
	}
	for ( i = 0; i < n; i++ ) {
		if ( w[i].kind == kind && ts_uuid_cmp(&w[i].u, u) == 0 ) {
			return n;
		}
	}
	w[n].u = *u;
	w[n].kind = kind;
	return n + 1;
}

/*
 * The users and groups this panel knows of: the system and the
 * administrators, whoever the desktop is logged in as and its groups,
 * and those the protection already names. 'kind' 0 is both.
 */
LOCAL INT who_all( CONST INFO *st, UINT kind, WHO *w )
{
	T_OBCRD	*crd = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	INT	n = 0, i;

	if ( kind != W_GROUP ) n = who_add(w, n, &ob_user_system, W_USER);
	if ( kind != W_USER )  n = who_add(w, n, &ob_group_admin, W_GROUP);
	if ( crd != NULL && ob_get_crd(crd) >= E_OK ) {
		if ( kind != W_GROUP ) n = who_add(w, n, &crd->user, W_USER);
		for ( i = 0; i < crd->ngrp && kind != W_USER; i++ ) {
			n = who_add(w, n, &crd->grp[i], W_GROUP);
		}
	}
	if ( crd != NULL ) Kfree(crd);
	if ( kind != W_GROUP ) n = who_add(w, n, &st->prt.owner, W_USER);
	if ( kind != W_USER )  n = who_add(w, n, &st->prt.group, W_GROUP);
	for ( i = 0; i < st->prt.nacl; i++ ) {
		if ( kind == 0 || kind == st->prt.acl[i].kind ) {
			n = who_add(w, n, &st->prt.acl[i].who, st->prt.acl[i].kind);
		}
	}
	return n;
}

/*
 * One of them chosen from a list; 'none' offers なし first. FALSE when
 * nothing was chosen.
 */
LOCAL BOOL who_choose( INFO *st, CONST char *title, UINT kind, BOOL none, WHO *out )
{
	WHO		*w = (WHO *)Kmalloc(sizeof(WHO) * IF_WHO_MAX);
	UB		(*txt)[IF_TEXT] = (UB (*)[IF_TEXT])Kmalloc(IF_TEXT * ( IF_WHO_MAX + 1 ));
	CONST char	**names = (CONST char **)Kmalloc(sizeof(char *) * ( IF_WHO_MAX + 1 ));
	INT		n = 0, m = 0, i, k, at;
	BOOL		ok = FALSE;

	if ( w == NULL || txt == NULL || names == NULL ) {
		goto done;
	}
	n = who_all(st, kind, w);
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
	at = dt_list_form(st->d, title, names, m, 0);
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
	if ( w != NULL ) Kfree(w);
	if ( txt != NULL ) Kfree(txt);
	if ( names != NULL ) Kfree((void *)names);
	return ok;
}

/* ---------------------------------------------------------------- the panel kept up to date */

LOCAL void set_label( INFO *st, INT idx, CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	(void)knl_pn_set_state(st->pid, idx, s, n);
}

LOCAL void show_owner( INFO *st )
{
	UB	b[IF_TEXT];

	who_name(&st->prt.owner, b, sizeof(b));
	set_label(st, st->i_owner, b);
	who_name(&st->prt.group, b, sizeof(b));
	set_label(st, st->i_group, b);
}

/* The names of a list, each ending in a nought, put in the panel */
LOCAL void set_names( INFO *st, INT idx, INT num, UB *all, INT len, INT count )
{
	INT	v = 0;

	(void)knl_pn_set_names(st->pid, idx, all, len);
	(void)wm_panel_get(st->pid, num, &v);
	if ( v > count ) {
		(void)wm_panel_set(st->pid, num, count);
	}
}

LOCAL void show_acl( INFO *st )
{
	UB	*all = (UB *)Kmalloc(WM_POOL_BYTES);
	INT	len = 0, i, k;

	if ( all == NULL ) return;
	for ( i = 0; i < st->prt.nacl && len + IF_TEXT < WM_POOL_BYTES / 2; i++ ) {
		CONST T_OBACE	*a = &st->prt.acl[i];
		UB		*b = all + len;
		BOOL		any = FALSE;

		k = put(b, 0, IF_TEXT, ( a->kind == OB_ACE_GROUP ) ? "グループ　" : "利用者　");
		who_name(&a->who, b + k, IF_TEXT - 40 - k);
		while ( b[k] != 0 ) k++;
		k = put(b, k, IF_TEXT, "：");
		if ( a->ops & OB_OP_READ )  { k = put(b, k, IF_TEXT, "読出し"); any = TRUE; }
		if ( a->ops & OB_OP_WRITE ) { k = put(b, k, IF_TEXT, any ? "・書込み" : "書込み"); any = TRUE; }
		if ( a->ops & OB_OP_EXEC )  { k = put(b, k, IF_TEXT, any ? "・実行" : "実行"); any = TRUE; }
		if ( !any ) k = put(b, k, IF_TEXT, "なし");
		len += k + 1;
	}
	set_names(st, st->i_acl, N_ACL, all, len, st->prt.nacl);
	Kfree(all);
}

LOCAL void show_apps( INFO *st )
{
	UB	*all = (UB *)Kmalloc(WM_POOL_BYTES);
	UB	b[IF_TEXT];
	INT	len = 0, i, k, now[2];

	if ( all == NULL ) return;
	for ( i = 0; i < st->napp && len + IF_TEXT < WM_POOL_BYTES / 2; i++ ) {
		k = put(all + len, 0, IF_TEXT - 44, (CONST char *)st->name[i]);
		k = put(all + len, k, IF_TEXT, "（");
		k = put(all + len, k, IF_TEXT - 4, (CONST char *)st->id[i]);
		k = put(all + len, k, IF_TEXT, "）");
		len += k + 1;
	}
	set_names(st, st->i_apps, N_APPS, all, len, st->napp);
	Kfree(all);
	/* the one it opens with is the one chosen in the list, its lamp lit */
	now[0] = ( st->dflt >= 0 && st->dflt < st->napp ) ? st->dflt + 1 : 0;
	now[1] = 0;
	(void)knl_pn_set_state(st->pid, st->i_apps, (CONST UB *)now, sizeof(now));

	k = 0;
	if ( st->dflt >= 0 && st->dflt < st->napp ) {
		k = put(b, 0, sizeof(b), (CONST char *)st->name[st->dflt]);
	} else {
		k = put(b, 0, sizeof(b), "なし");
	}
	set_label(st, st->i_dflt, b);
}

/* A program chosen in the list: the object opens with it from now */
LOCAL void apps_follow( INFO *st )
{
	INT	v = dt_pn_value(st->pid, N_APPS) - 1;
	UB	b[IF_TEXT];

	if ( v >= 0 && v < st->napp && v != st->dflt ) {
		st->dflt = v;
		st->apps_changed = TRUE;
		(void)put(b, 0, sizeof(b), (CONST char *)st->name[v]);
		set_label(st, st->i_dflt, b);
	}
}

/* ---------------------------------------------------------------- the programs */

LOCAL BOOL str_eq( CONST UB *a, CONST UB *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == *b );
}

LOCAL void app_add( INFO *st, CONST DTPROG *p )
{
	INT	i;

	for ( i = 0; i < st->napp; i++ ) {
		if ( str_eq(st->id[i], p->id) ) {
			return;			/* already in it */
		}
	}
	if ( st->napp >= IF_APP_MAX ) {
		return;
	}
	(void)put(st->id[st->napp], 0, OM_APP_ID, (CONST char *)p->id);
	(void)put(st->name[st->napp], 0, OM_APP_NAME, (CONST char *)p->name);
	if ( st->dflt < 0 ) {
		st->dflt = st->napp;
	}
	st->napp++;
	st->apps_changed = TRUE;
}

/* The program a link names: the program object, or one of its templates */
LOCAL CONST DTPROG *prog_of_link( CONST TS_UUID *target )
{
	CONST DTPROG	*all[64], *p = dt_prog_of(target);
	INT		n, i, k;

	if ( p != NULL ) {
		return p;
	}
	n = dt_prog_list(0, all, 64);
	for ( i = 0; i < n; i++ ) {
		for ( k = 0; k < all[i]->nbase; k++ ) {
			if ( ts_uuid_cmp(&all[i]->base[k], target) == 0 ) {
				return all[i];
			}
		}
	}
	return NULL;
}

LOCAL void choose_app( INFO *st )
{
	CONST DTPROG	*all[64];
	UB		(*txt)[IF_TEXT] = (UB (*)[IF_TEXT])Kmalloc(IF_TEXT * 64);
	CONST char	*names[64];
	INT		n, i, k, at;

	if ( txt == NULL ) return;
	n = dt_prog_list(0, all, 64);
	for ( i = 0; i < n; i++ ) {
		k = put(txt[i], 0, IF_TEXT - 44, (CONST char *)all[i]->name);
		k = put(txt[i], k, IF_TEXT, ( all[i]->kind == DT_PK_APP ) ? "　（アプリケーション）"
					   : ( all[i]->kind == DT_PK_ACCESSORY ) ? "　（小物）" : "　（道具）");
		names[i] = (CONST char *)txt[i];
	}
	at = dt_list_form(st->d, "機能付箋に加えるプログラム", names, n, 0);
	if ( at >= 0 && at < n ) {
		app_add(st, all[at]);
	}
	Kfree(txt);
}

/* The 付箋 of the program chosen put in the tray: a link to the program object */
LOCAL void copy_tag( INFO *st, INT sel )
{
	CONST DTPROG	*p;
	T_TAD		*frag;
	T_VOBJ		*lv;
	TS_UUID		id;
	INT		fid = fn_system(), w, band = IF_CHSZ + 11, i;

	if ( sel < 0 || sel >= st->napp ) {
		dt_tell(st->d, "複写する機能付箋を選んでください", NULL);
		return;
	}
	p = dt_prog_find(st->id[sel]);
	if ( p == NULL || ts_uuid_cmp(&p->uuid, &nobody) == 0 ) {
		dt_tell(st->d, "このプログラムはシステムにありません", (CONST char *)st->id[sel]);
		return;
	}
	lv = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	frag = dt_frag_new("figure");
	if ( lv == NULL || frag == NULL ) {
		if ( lv != NULL ) Kfree(lv);
		if ( frag != NULL ) tad_free(frag);
		return;
	}
	knl_memset(lv, 0, sizeof(*lv));
	lv->target = p->uuid;
	lv->recno = 0;
	for ( i = 0; p->name[i] != 0 && i < TAD_NAME_MAX - 1; i++ ) {
		lv->name[i] = p->name[i];
	}
	lv->name[i] = 0;
	w = ( fid > 0 ) ? fn_width(fid, lv->name) : IF_CHSZ * 6;
	lv->left = 0;
	lv->top = 0;
	lv->right = w + IF_CHSZ + 20;
	lv->bottom = band;
	lv->height = band;
	lv->frcol = 0x000000;
	lv->chcol = 0x000000;
	lv->tbcol = 0xFFFFFF;
	lv->bgcol = 0xFFFFFF;
	lv->chsz = IF_CHSZ;
	lv->disp = TAD_D_DEFAULT;
	if ( tad_lnk_add(frag, lv, &id) >= E_OK ) {
		(void)dt_tray_put(frag, "機能付箋");
	}
	tad_free(frag);
	Kfree(lv);
}

/* The programs of the links in the tray taken into the list */
LOCAL void paste_tags( INFO *st )
{
	T_TAD		*frag = dt_tray_frag();
	T_VOBJ		*lv = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	CONST DTPROG	*p;
	INT		n, i, got = 0;

	if ( frag != NULL && lv != NULL ) {
		n = tad_lnk_count(frag);
		for ( i = 0; i < n; i++ ) {
			if ( tad_lnk_get(frag, i, lv) >= E_OK
			  && ( p = prog_of_link(&lv->target) ) != NULL ) {
				app_add(st, p);
				got++;
			}
		}
	}
	if ( frag != NULL ) tad_free(frag);
	if ( lv != NULL ) Kfree(lv);
	if ( got == 0 ) {
		dt_tell(st->d, "トレーにプログラムの付箋がありません", NULL);
	}
}

/* ---------------------------------------------------------------- the buttons */

LOCAL void act( INFO *st, UINT a )
{
	INT	sel = dt_pn_value(st->pid, N_APPS) - 1, i;
	WHO	w;

	switch ( a ) {
	case A_OWNER:
		if ( who_choose(st, "所有者", W_USER, TRUE, &w) ) {
			st->prt.owner = w.u;
			show_owner(st);
		}
		break;
	case A_GROUP:
		if ( who_choose(st, "グループ", W_GROUP, TRUE, &w) ) {
			st->prt.group = w.u;
			show_owner(st);
		}
		break;
	case A_ACL_ADD: {
		UINT	ops = 0;

		if ( dt_pn_value(st->pid, N_ACL_R) != 0 ) ops |= OB_OP_R;
		if ( dt_pn_value(st->pid, N_ACL_W) != 0 ) ops |= OB_OP_W;
		if ( dt_pn_value(st->pid, N_ACL_X) != 0 ) ops |= OB_OP_EXEC;
		if ( !who_choose(st, "個別の利用権を与える相手", 0, FALSE, &w) ) {
			break;
		}
		/* one entry for each: given again, it is the rights that change */
		for ( i = 0; i < st->prt.nacl; i++ ) {
			if ( st->prt.acl[i].kind == w.kind && ts_uuid_cmp(&st->prt.acl[i].who, &w.u) == 0 ) {
				break;
			}
		}
		if ( i == st->prt.nacl ) {
			if ( st->prt.nacl >= OB_ACL_MAX ) break;
			st->prt.nacl++;
		}
		st->prt.acl[i].kind = w.kind;
		st->prt.acl[i].who = w.u;
		st->prt.acl[i].ops = ops;
		show_acl(st);
		break;
	}
	case A_ACL_DEL:
		i = dt_pn_value(st->pid, N_ACL) - 1;
		if ( i >= 0 && i < st->prt.nacl ) {
			for ( ; i < st->prt.nacl - 1; i++ ) {
				st->prt.acl[i] = st->prt.acl[i + 1];
			}
			st->prt.nacl--;
			show_acl(st);
		}
		break;
	case A_APP_ADD:
		choose_app(st);
		show_apps(st);
		break;
	case A_APP_DEL:
		if ( sel >= 0 && sel < st->napp ) {
			for ( i = sel; i < st->napp - 1; i++ ) {
				knl_memcpy(st->id[i], st->id[i + 1], OM_APP_ID);
				knl_memcpy(st->name[i], st->name[i + 1], OM_APP_NAME);
			}
			st->napp--;
			if ( st->dflt == sel ) st->dflt = ( st->napp > 0 ) ? 0 : -1;
			else if ( st->dflt > sel ) st->dflt--;
			st->apps_changed = TRUE;
			show_apps(st);
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the panel */

LOCAL T_WMPART *add( T_WMPANEL *def, INT sheet, UW type, INT num, INT l, INT t,
		     INT w, INT h, CONST char *label )
{
	T_WMPART	*pt = dt_pn_add(def, type, num, l, t, w, h, label);

	if ( pt != NULL ) {
		pt->sheet = sheet;
	}
	return pt;
}

LOCAL void add_act( T_WMPANEL *def, INT sheet, INT l, INT t, INT w, CONST char *label,
		    UINT a, BOOL off )
{
	T_WMPART	*pt = add(def, sheet, WM_PT_BUTTON, N_BTN + (INT)( a - WM_ANS_ACT ),
				  l, t, w, 26, label);

	if ( pt != NULL ) {
		pt->answer = a;
		if ( off ) pt->type |= P_DISABLE;
	}
}

/* A row of 実身情報: its name and what it is */
LOCAL void row( T_WMPANEL *def, INT i, CONST char *name, CONST UB *val )
{
	T_WMPART	*pt;

	(void)add(def, 1, WM_PT_LABEL, 0, 14, 40 + i * 26, 112, 22, name);
	pt = add(def, 1, WM_PT_LABEL, 0, 130, 40 + i * 26, IF_W - 144, 22, NULL);
	if ( pt != NULL ) {
		dt_pn_text(pt->label, WM_LABEL_MAX, (CONST char *)val);
	}
}

LOCAL INT index_of_last( CONST T_WMPANEL *def )
{
	return def->npart - 1;
}

LOCAL void build_info( T_WMPANEL *def, CONST T_VOBJ *v )
{
	T_OMINFO	*info = (T_OMINFO *)Kmalloc(sizeof(T_OMINFO));
	UB		b[WM_LABEL_MAX];
	char		t[TS_UUID_STRLEN + 1];
	T_TAD		*rec;
	INT		n;

	if ( info == NULL ) return;
	if ( om_store_info(&v->target, info) < E_OK ) {
		knl_memset(info, 0, sizeof(*info));
	}
	row(def, 0, "実身名", info->name);
	if ( ts_uuid_to_str(&v->target, t, sizeof(t)) < E_OK ) t[0] = 0;
	(void)put(b, 0, sizeof(b), t);
	row(def, 1, "ＵＵＩＤ", b);
	if ( om_store_relationship(&v->target, b, sizeof(b)) < 0 ) b[0] = 0;
	row(def, 2, "続柄", b);
	row(def, 3, "作成者", info->maker);
	put_time(b, sizeof(b), info->made);
	row(def, 4, "作成日時", b);
	put_time(b, sizeof(b), info->updated);
	row(def, 5, "更新日時", b);
	put_time(b, sizeof(b), info->accessed);
	row(def, 6, "参照日時", b);
	n = put_num(b, 0, sizeof(b), info->bytes);
	(void)put(b, n, sizeof(b), " バイト");
	row(def, 7, "サイズ", b);
	(void)put_num(b, 0, sizeof(b), info->records);
	row(def, 8, "レコード数", b);
	(void)put_num(b, 0, sizeof(b), info->refs);
	row(def, 9, "参照仮身数", b);
	rec = om_store_get(&v->target, 0);
	(void)put_num(b, 0, sizeof(b), ( rec != NULL ) ? tad_lnk_count(rec) : 0);
	row(def, 10, "含む仮身数", b);
	Kfree(info);
}

LOCAL void build_users( T_WMPANEL *def, INFO *st )
{
	CONST char	*who[3] = { "所有者", "グループ", "一般" };
	CONST char	*ops[3] = { "読出し", "書込み", "実行" };
	T_WMPART	*pt;
	BOOL		off = !st->prt_ok;
	INT		r, c;

	(void)add(def, 2, WM_PT_LABEL, 0, 14, 40, 110, 22, "所有者");
	(void)add(def, 2, WM_PT_LABEL, N_OWNER, 130, 40, IF_W - 230, 22, NULL);
	st->i_owner = index_of_last(def);
	add_act(def, 2, IF_W - 92, 38, 78, "変更", A_OWNER, off);
	(void)add(def, 2, WM_PT_LABEL, 0, 14, 72, 110, 22, "グループ");
	(void)add(def, 2, WM_PT_LABEL, N_GROUP, 130, 72, IF_W - 230, 22, NULL);
	st->i_group = index_of_last(def);
	add_act(def, 2, IF_W - 92, 70, 78, "変更", A_GROUP, off);

	(void)add(def, 2, WM_PT_LABEL, 0, 14, 108, 110, 22, "利用権");
	for ( r = 0; r < 3; r++ ) {
		(void)add(def, 2, WM_PT_LABEL, 0, 40, 132 + r * 26, 84, 22, who[r]);
		for ( c = 0; c < 3; c++ ) {
			pt = add(def, 2, WM_PT_CHECK, N_MODE + r * 3 + c, 130 + c * 96, 132 + r * 26,
				 90, 22, ops[c]);
			if ( pt != NULL ) {
				pt->value = ( st->prt.mode & ( 0400U >> ( r * 3 + c ) ) ) ? 1 : 0;
				if ( off ) pt->type |= P_DISABLE;
			}
		}
	}
	(void)add(def, 2, WM_PT_LABEL, 0, 14, 216, 110, 22, "属性");
	pt = add(def, 2, WM_PT_CHECK, N_RONLY, 130, 216, 110, 22, "編集不可");
	if ( pt != NULL ) {
		pt->value = ( st->prt.attr & OB_A_RONLY ) ? 1 : 0;
		if ( off ) pt->type |= P_DISABLE;
	}
	pt = add(def, 2, WM_PT_CHECK, N_PERM, 250, 216, 110, 22, "削除不可");
	if ( pt != NULL ) {
		pt->value = ( st->prt.attr & OB_A_PERM ) ? 1 : 0;
		if ( off ) pt->type |= P_DISABLE;
	}

	(void)add(def, 2, WM_PT_LABEL, 0, 14, 248, 200, 22, "個別の利用権");
	pt = add(def, 2, WM_PT_LIST, N_ACL, 14, 272, IF_W - 28, 66, NULL);
	st->i_acl = index_of_last(def);
	if ( pt != NULL ) {
		pt->top = 1;
		if ( off ) pt->type |= P_DISABLE;
	}
	for ( c = 0; c < 3; c++ ) {
		pt = add(def, 2, WM_PT_CHECK, N_ACL_R + c, 14 + c * 84, 346, 80, 22, ops[c]);
		if ( pt != NULL ) {
			pt->value = ( c == 0 ) ? 1 : 0;
			if ( off ) pt->type |= P_DISABLE;
		}
	}
	add_act(def, 2, IF_W - 184, 344, 82, "追加", A_ACL_ADD, off);
	add_act(def, 2, IF_W - 96, 344, 82, "削除", A_ACL_DEL, off);
	if ( off ) {
		(void)add(def, 2, WM_PT_LABEL, 0, 130, 108, IF_W - 144, 22,
			  "（この実身の保護は読めません）");
	}
}

LOCAL void build_apps( T_WMPANEL *def, INFO *st )
{
	T_WMPART	*pt;
	BOOL		lockable = (BOOL)( !st->d->sealed && dt_app_lockable(st->v) );

	(void)add(def, 3, WM_PT_LABEL, 0, 14, 40, 110, 22, "既定の起動");
	(void)add(def, 3, WM_PT_LABEL, N_DFLT, 130, 40, IF_W - 144, 22, NULL);
	st->i_dflt = index_of_last(def);
	(void)add(def, 3, WM_PT_LABEL, 0, 14, 68, 300, 22, "機能付箋");
	pt = add(def, 3, WM_PT_LIST, N_APPS, 14, 92, IF_W - 28, 150, NULL);
	st->i_apps = index_of_last(def);
	if ( pt != NULL ) {
		pt->top = 1;
	}
	add_act(def, 3, 14, 250, 92, "追加", A_APP_ADD, FALSE);
	add_act(def, 3, 112, 250, 92, "削除", A_APP_DEL, FALSE);
	pt = add(def, 3, WM_PT_CHECK, N_LOCK, 14, 290, 120, 22, "起動固定");
	if ( pt != NULL ) {
		pt->value = dt_app_locked(st->v) ? 1 : 0;
		if ( !lockable ) pt->type |= P_DISABLE;
	}
}

LOCAL void load( INFO *st )
{
	UB	dflt[OM_APP_ID];
	INT	i;

	st->prt_ok = (BOOL)( ob_get_prt(&st->v->target, &st->prt) >= E_OK );
	if ( !st->prt_ok ) {
		knl_memset(&st->prt, 0, sizeof(st->prt));
	}
	st->prt0 = st->prt;

	st->napp = om_store_apps(&st->v->target, st->name, IF_APP_MAX);
	st->dflt = -1;
	if ( om_store_default_app(&st->v->target, dflt, sizeof(dflt)) < 0 ) {
		dflt[0] = 0;
	}
	for ( i = 0; i < st->napp; i++ ) {
		if ( om_store_app_id(&st->v->target, i, st->id[i], OM_APP_ID) < 0 ) {
			st->id[i][0] = 0;
		}
		if ( st->dflt < 0 && dflt[0] != 0 && str_eq(st->id[i], dflt) ) {
			st->dflt = i;
		}
	}
}

LOCAL BOOL same_bytes( CONST void *a, CONST void *b, SZ n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	SZ		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

/* The window the link was chosen in, if it is still open */
LOCAL DTWIN *parent_of( INFO *st )
{
	DTWIN	*p = dt_win_of(st->parent_wid);

	return ( p != NULL && p == st->parent && p->used ) ? p : NULL;
}

/* What was set, written: FALSE when the protection could not be */
LOCAL BOOL apply( INFO *st )
{
	BOOL	ok = TRUE;
	INT	r, c;

	if ( st->prt_ok ) {
		st->prt.mode &= ~0777U;
		for ( r = 0; r < 3; r++ ) {
			for ( c = 0; c < 3; c++ ) {
				if ( dt_pn_value(st->pid, N_MODE + r * 3 + c) != 0 ) {
					st->prt.mode |= 0400U >> ( r * 3 + c );
				}
			}
		}
		st->prt.attr &= ~( OB_A_RONLY | OB_A_PERM );
		if ( dt_pn_value(st->pid, N_RONLY) != 0 ) st->prt.attr |= OB_A_RONLY;
		if ( dt_pn_value(st->pid, N_PERM) != 0 )  st->prt.attr |= OB_A_PERM;
		if ( !same_bytes(&st->prt, &st->prt0, sizeof(T_OBPRT))
		  && ob_set_prt(&st->v->target, &st->prt) < E_OK ) {
			ok = FALSE;
		}
	}
	if ( st->apps_changed ) {
		CONST UB	*ids[IF_APP_MAX], *names[IF_APP_MAX];
		INT		i;

		for ( i = 0; i < st->napp; i++ ) {
			ids[i] = st->id[i];
			names[i] = st->name[i];
		}
		(void)om_store_set_apps(&st->v->target, ids, names, st->napp, st->dflt);
	}
	if ( ( dt_pn_value(st->pid, N_LOCK) != 0 ) != st->lock_was && parent_of(st) != NULL ) {
		dt_lock_app(st->parent, st->v);
	}
	return ok;
}

/* ---------------------------------------------------------------- the windows */

/*
 * 管理情報 is a window like any other: several can be open, each of its
 * own object, and the desktop goes on with the rest while they are.
 * The desktop's loop gives each event to dt_info_event first.
 */
#define IF_WIN_MAX	8

LOCAL INFO	*info_win[IF_WIN_MAX];

LOCAL INFO *info_of( INT wid )
{
	INT	i;

	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		if ( info_win[i] != NULL && info_win[i]->wid == wid ) {
			return info_win[i];
		}
	}
	return NULL;
}

LOCAL void info_close( INFO *st )
{
	INT	i;

	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		if ( info_win[i] == st ) {
			info_win[i] = NULL;
		}
	}
	if ( st->pid >= 0 ) {
		(void)wm_panel_close(st->pid);
	}
	(void)wm_close(st->wid);
	Kfree(st);
	wm_composite();
}

/* A desktop window for what asks and tells to stand over */
LOCAL DTWIN *host_of( INFO *st )
{
	DTWIN	*p = parent_of(st);

	if ( p != NULL ) {
		return p;
	}
	return dt_first_win();
}

/* The window's name: the object's with 管理情報 after it */
LOCAL void title_of( CONST T_VOBJ *v, char *t, INT max )
{
	UB	nm[TAD_NAME_MAX];
	INT	n;

	if ( om_store_name(&v->target, nm, sizeof(nm)) <= 0 ) {
		nm[0] = 0;
	}
	n = put((UB *)t, 0, max, (CONST char *)nm);
	(void)put((UB *)t, n, max, "の管理情報");
}

EXPORT void dt_real_info( DTWIN *d, CONST T_VOBJ *v )
{
	INFO		*st;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_WMWIN		w;
	T_DPRECT	o;
	CONST char	*tags[3] = { "実身情報", "使用者管理", "付箋指定" };
	char		title[TAD_NAME_MAX + 24];
	INT		i, at = -1, k, pw, ph, slot = -1;

	/* the object's window already open: brought to the front */
	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		if ( info_win[i] == NULL ) {
			if ( slot < 0 ) slot = i;
		} else if ( ts_uuid_cmp(&info_win[i]->link.target, &v->target) == 0 ) {
			wm_raise(info_win[i]->wid);
			wm_focus(info_win[i]->wid);
			wm_composite();
			return;
		}
	}
	if ( slot < 0 ) {
		dt_tell(d, "管理情報のウインドウをこれ以上開けません", NULL);
		return;
	}
	st = (INFO *)Kmalloc(sizeof(INFO));
	if ( st == NULL ) {
		return;
	}
	knl_memset(st, 0, sizeof(*st));
	st->used = TRUE;
	st->d = d;
	st->parent = d;
	st->parent_wid = d->wid;
	st->link = *v;
	st->v = &st->link;
	st->pid = -1;
	load(st);
	st->lock_was = dt_app_locked(st->v);

	def = dt_pn_new(IF_W, IF_H);
	if ( def == NULL ) {
		Kfree(st);
		return;
	}
	def->kind = WM_PNL_BARE;
	pt = dt_pn_add(def, WM_PT_TAGS, N_TAGS, 0, 0, IF_W, IF_H - 44, NULL);
	if ( pt != NULL ) {
		for ( i = 0; i < 3; i++ ) {
			k = wm_panel_name(def, (CONST UB *)tags[i]);
			if ( at < 0 ) at = k;
		}
		pt->pool = ( at >= 0 ) ? at : 0;
		pt->count = 3;
		pt->value = 1;
		pt->inner.left = DT_PN_IN + 4;
		pt->inner.top = DT_PN_IN + 28;
		pt->inner.right = DT_PN_IN + IF_W - 4;
		pt->inner.bottom = DT_PN_IN + IF_H - 48;
	}
	build_info(def, st->v);
	build_users(def, st);
	build_apps(def, st);
	dt_pn_buttons(def, "適用");

	/*
	 * A window whose work area is the panel, stepped down and right of
	 * the window the link was chosen in
	 */
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		o.left = w.outer.left + 40;
		o.top = w.outer.top + 40;
	} else {
		o.left = o.top = 80;
	}
	o.right = o.left + pw + 16;
	o.bottom = o.top + ph + 40;
	title_of(st->v, title, sizeof(title));
	st->wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, title);
	if ( st->wid < 0 ) {
		Kfree(def);
		Kfree(st);
		return;
	}
	if ( wm_ref(st->wid, &w) >= E_OK ) {
		/* the frame measured: the work area made the panel's size */
		o = w.outer;
		o.right += pw - ( w.work.right - w.work.left );
		o.bottom += ph - ( w.work.bottom - w.work.top );
		(void)wm_move(st->wid, &o);
	}
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = pw;
	def->r.bottom = ph;
	st->pid = wm_panel_open(st->wid, def);
	Kfree(def);
	if ( st->pid < 0 ) {
		(void)wm_close(st->wid);
		Kfree(st);
		return;
	}
	info_win[slot] = st;
	show_owner(st);
	show_acl(st);
	show_apps(st);
	wm_raise(st->wid);
	wm_focus(st->wid);
	wm_panel_draw(st->pid);
	wm_composite();
}

/*
 * The window's menu: 閉じる, 編集 with コピー and 貼り付け of the
 * 機能付箋 chosen on 付箋指定, and the system's items after them.
 */
LOCAL void info_menu( INFO *st, CONST T_WMEV *ev )
{
	TS_UUID		mu;
	T_MNSEL		sel;
	T_VOBJ		*none = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	DTWIN		*host = host_of(st);
	ID		mid;
	INT		pid, cmd = 0;
	ER		er;

	if ( none == NULL ) return;
	knl_memset(none, 0, sizeof(*none));
	if ( ts_str_to_uuid(SYSDEF_MENU_INFO, &mu) < E_OK || mn_cre_men(&mu, &mid) < E_OK ) {
		Kfree(none);
		return;
	}
	if ( host != NULL ) {
		dt_get_vmn(host, mid, FALSE, none);
	}
	pid = mn_opn_men(mid, st->wid, ev->x, ev->y);
	if ( pid >= 0 ) {
		cmd = dt_run_menu(host, pid, ev->when);
	}
	er = mn_get_sel(mid, cmd, &sel);
	(void)mn_del_men(mid);
	if ( er >= E_OK ) {
		st->d = host;
		if ( mn_is(&sel, "close") ) {
			info_close(st);
		} else if ( mn_is(&sel, "info.copy") ) {
			if ( host != NULL ) copy_tag(st, dt_pn_value(st->pid, N_APPS) - 1);
		} else if ( mn_is(&sel, "info.paste") ) {
			if ( host != NULL ) paste_tags(st);
			show_apps(st);
			wm_panel_draw(st->pid);
		} else if ( host != NULL ) {
			(void)dt_exe_vmn(host, &sel, FALSE, none);
		}
	}
	Kfree(none);
	wm_composite();
}

/*
 * An event, if it is for a window of 管理情報: TRUE when it was, and
 * nothing else is to be done with it. A move of the pointer is also
 * the desktop's to follow, so it answers FALSE for that.
 */
EXPORT BOOL dt_info_event( CONST T_WMEV *ev )
{
	INFO	*st = NULL;
	T_WMEV	e = *ev;
	T_WMWIN	w;
	UINT	ans = WM_ANS_NONE, btn;
	INT	i, px, py;

	/* one being carried takes the moves and the let go wherever they are */
	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		if ( info_win[i] != NULL && info_win[i]->moving ) {
			st = info_win[i];
		}
	}
	if ( st != NULL ) {
		if ( ev->type == HID_EV_MOVE && ts_hid_pointer(&px, &py, &btn) >= E_OK
		  && wm_ref(st->wid, &w) >= E_OK ) {
			T_DPRECT	o;

			o.left = px - st->grab_dx;
			o.top = py - st->grab_dy;
			o.right = o.left + ( w.outer.right - w.outer.left );
			o.bottom = o.top + ( w.outer.bottom - w.outer.top );
			if ( wm_move(st->wid, &o) >= E_OK ) wm_composite();
			return FALSE;
		}
		if ( ev->type == HID_EV_BTN_UP ) {
			st->moving = FALSE;
			return TRUE;
		}
	}

	if ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) {
		st = info_of(wm_focused());
		e.wid = ( st != NULL ) ? st->wid : 0;
	} else {
		st = info_of(ev->wid);
	}
	if ( st == NULL ) {
		return FALSE;
	}
	if ( ev->type == HID_EV_BTN_DOWN ) {
		wm_raise(st->wid);
		wm_focus(st->wid);
		if ( ev->y < 0 ) {
			/* its band: carried */
			if ( wm_ref(st->wid, &w) >= E_OK ) {
				st->moving = TRUE;
				st->grab_dx = ( w.work.left + ev->x ) - w.outer.left;
				st->grab_dy = ( w.work.top + ev->y ) - w.outer.top;
			}
			wm_composite();
			return TRUE;
		}
		if ( ev->code == 1 ) {
			info_menu(st, ev);	/* the second button: the window's menu */
			return TRUE;
		}
		if ( ev->code != 0 ) {
			wm_composite();
			return TRUE;
		}
	}
	st->d = host_of(st);
	if ( wm_panel_event(st->pid, &e, &ans) < E_OK ) {
		ans = WM_ANS_CANCEL;
	}
	apps_follow(st);
	if ( ans >= WM_ANS_ACT && st->d != NULL ) {
		act(st, ans);
		wm_raise(st->wid);
		wm_focus(st->wid);
		ans = WM_ANS_NONE;
	}
	if ( ans == WM_ANS_OK ) {
		if ( !apply(st) && st->d != NULL ) {
			dt_tell(st->d, "使用者管理を変更できませんでした", "所有者か管理者だけが変更できます");
			wm_raise(st->wid);
			wm_panel_draw(st->pid);
			wm_composite();
			return TRUE;
		}
		info_close(st);
		dt_draw_all();
		return TRUE;
	}
	if ( ans == WM_ANS_CANCEL ) {
		info_close(st);
		return TRUE;
	}
	wm_panel_draw(st->pid);
	wm_update();
	return (BOOL)( ev->type != HID_EV_MOVE );
}

/* Every window of 管理情報 closed (the desktop stopping) */
EXPORT void dt_info_close_all( void )
{
	INT	i;

	for ( i = 0; i < IF_WIN_MAX; i++ ) {
		if ( info_win[i] != NULL ) {
			info_close(info_win[i]);
		}
	}
}
