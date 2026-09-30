/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtvmn.c
 *	The menus every window has, and what their items do (design 18.15)
 *
 *	閉じる at the head of every window's menu, and 仮身操作, 実身操作,
 *	屑実身操作, 小物 and 実行 at its foot, are the system's definitions
 *	(SYSMENU.DEF), taken in by each program's own menu definition. A
 *	program never says what state they are in or what they do:
 *
 *	dt_get_vmn looks at the virtual object the menu was opened on and
 *	the window it is in, and sets the items -- grey what does not apply,
 *	show the one of a pair that does, fill 実行 with the programs of the
 *	object's applist, 小物 with the accessories, ウインドウ with the
 *	windows open and the edit menus' トレー with the levels of the tray.
 *
 *	dt_exe_vmn does an item that is the system's and answers TRUE; the
 *	program handles only what its own definitions hold.
 *
 *	What the items do to a virtual object or its real object is below
 *	them: 属性変更, 続柄設定 and the 実身操作; 管理情報 is dtinfo.c.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/fn.h>
#include <ts/mn.h>
#include <ts/json.h>
#include <ts/sysdef.h>
#include <ts/tray.h>
#include "desktop.h"

#define VM_ACC_MAX	16		/* accessories shown in 小物 */
#define VM_APP_MAX	8		/* programs shown in 実行 */

LOCAL void attributes( DTWIN *d, CONST T_VOBJ *start );
LOCAL void relationship( DTWIN *d, CONST T_VOBJ *start );
LOCAL void rename_real( DTWIN *d, CONST T_VOBJ *v );
LOCAL void duplicate_real( DTWIN *d, CONST T_VOBJ *v );
LOCAL void new_version( DTWIN *d, CONST T_VOBJ *v );
LOCAL void reconnect( DTWIN *d, CONST T_VOBJ *v );

/* ---------------------------------------------------------------- the system's definitions */

LOCAL CONST char * CONST sys_menus[] = {
	SYSDEF_MENU_WINDOW, SYSDEF_MENU_OBJECT, SYSDEF_MENU_VOBJ,
	SYSDEF_MENU_REAL, SYSDEF_MENU_ACC, SYSDEF_MENU_EXEC, SYSDEF_MENU_TRAY,
	SYSDEF_MENU_WINLIST
};

#define NSYS	( (INT)( sizeof(sys_menus) / sizeof(sys_menus[0]) ) )

LOCAL BOOL is_sys( CONST TS_UUID *def )
{
	TS_UUID	u;
	INT	i;

	for ( i = 0; i < NSYS; i++ ) {
		if ( ts_str_to_uuid(sys_menus[i], &u) >= E_OK && ts_uuid_cmp(&u, def) == 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL TS_UUID sys_def( CONST char *id )
{
	TS_UUID	u;

	if ( ts_str_to_uuid(id, &u) < E_OK ) {
		knl_memset(&u, 0, sizeof(u));
	}
	return u;
}

/* The accessories 小物 shows, in the order the menu lists them */
LOCAL INT menu_accessories( CONST DTPROG **acc, INT max )
{
	CONST DTPROG	*all[VM_ACC_MAX];
	INT		n = dt_prog_list(DT_PK_ACCESSORY, all, VM_ACC_MAX), i, k = 0;

	for ( i = 0; i < n && k < max; i++ ) {
		if ( all[i]->menu ) {
			acc[k++] = all[i];
		}
	}
	return k;
}

/* ---------------------------------------------------------------- the state */

LOCAL void atr( ID mid, CONST TS_UUID *def, CONST char *code, BOOL grey )
{
	(void)mn_chg_atr(mid, def, code, grey ? MN_GREY : 0);
}


/*
 * The tray's history: a row for each level, "1 仮身3個", the one in hand
 * marked. With nothing in the tray the list is empty and 空にする grey,
 * so the トレー row of the edit menu is grey too.
 */
LOCAL void tray_state( ID mid )
{
	TS_UUID	td = sys_def(SYSDEF_MENU_TRAY);
	T_TRSTS	*sts;
	UB	(*rows)[WM_LABEL_MAX];
	INT	i, k, at;

	sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	rows = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * TR_LEVELS);
	if ( sts != NULL && rows != NULL && tr_get_sts(sts) >= E_OK ) {
		for ( i = 1; i <= sts->nlevel; i++ ) {
			at = 0;
			if ( i >= 10 ) rows[i - 1][at++] = (UB)( '0' + i / 10 );
			rows[i - 1][at++] = (UB)( '0' + i % 10 );
			rows[i - 1][at++] = ' ';
			for ( k = 0; sts->set[i].name[k] != 0 && at < WM_LABEL_MAX - 1; k++ ) {
				rows[i - 1][at++] = sts->set[i].name[k];
			}
			rows[i - 1][at] = 0;
		}
		(void)mn_set_lst(mid, &td, "tray.level", (CONST UB *)rows, WM_LABEL_MAX,
				 sts->nlevel);
		if ( sts->hand > 0 ) {
			(void)mn_chg_idx(mid, &td, "tray.level", sts->hand - 1, MN_TICK);
		}
		atr(mid, &td, "tray.clear", (BOOL)( sts->nlevel == 0 ));
	}
	if ( sts != NULL ) Kfree(sts);
	if ( rows != NULL ) Kfree(rows);
}

EXPORT void dt_get_vmn( DTWIN *d, ID mid, BOOL on_vobj, CONST T_VOBJ *v )
{
	TS_UUID		obj = sys_def(SYSDEF_MENU_OBJECT), vd = sys_def(SYSDEF_MENU_VOBJ);
	TS_UUID		rd = sys_def(SYSDEF_MENU_REAL), ad = sys_def(SYSDEF_MENU_ACC);
	TS_UUID		ed = sys_def(SYSDEF_MENU_EXEC);
	CONST DTPROG	*acc[VM_ACC_MAX];
	UB		(*names)[WM_LABEL_MAX];
	UB		(*apps)[OM_APP_NAME];
	BOOL		open, cut, locked, sealed = d->sealed;
	INT		n, i, k;

	tray_state(mid);

	/* 小物: the accessories that show in the menu */
	names = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * VM_ACC_MAX);
	if ( names != NULL ) {
		n = menu_accessories(acc, VM_ACC_MAX);
		for ( i = 0; i < n; i++ ) {
			for ( k = 0; acc[i]->name[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) {
				names[i][k] = acc[i]->name[k];
			}
			names[i][k] = 0;
		}
		(void)mn_set_lst(mid, &ad, "acc.item", (CONST UB *)names, WM_LABEL_MAX, n);
		Kfree(names);
	}

	/* ウインドウ: the windows open, nearest first */
	mn_winlist_fill(mid);

	/* away from a virtual object, what is about one cannot be chosen */
	if ( !on_vobj || v == NULL ) {
		atr(mid, &obj, "vobj", TRUE);
		atr(mid, &obj, "real", TRUE);
		atr(mid, &obj, "exec", TRUE);
		return;
	}
	open = (BOOL)( dt_showing(&v->target) != NULL );
	cut = (BOOL)!dt_reachable(v);
	locked = dt_app_locked(v);

	/* 仮身操作: what changes the virtual object is grey in a sealed window */
	atr(mid, &vd, "vobj.open", (BOOL)( open || cut ));
	atr(mid, &vd, "vobj.close", (BOOL)!open);
	atr(mid, &vd, "vobj.attr", sealed);
	atr(mid, &vd, "vobj.relation", sealed);
	(void)mn_chg_atr(mid, &vd, "vobj.reconnect", !cut ? MN_HIDE : sealed ? MN_GREY : 0);

	/* 実身操作: what needs the real object is grey when it cannot be reached */
	atr(mid, &rd, "real.rename", cut);
	atr(mid, &rd, "real.duplicate", (BOOL)( cut || sealed ));
	atr(mid, &rd, "real.newversion", (BOOL)( cut || sealed ));
	(void)mn_chg_atr(mid, &rd, "real.lock",
			 locked ? MN_HIDE : ( cut || sealed ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, &rd, "real.unlock",
			 !locked ? MN_HIDE : ( cut || sealed ) ? MN_GREY : 0);

	/* 実行: the programs the real object's applist names */
	apps = (UB (*)[OM_APP_NAME])Kmalloc(sizeof(UB) * OM_APP_NAME * VM_APP_MAX);
	if ( apps != NULL ) {
		n = om_link_apps(v, apps, VM_APP_MAX);
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)apps, OM_APP_NAME,
				 ( n > 0 ) ? n : 0);
		atr(mid, &ed, "exec.app", cut);
		Kfree(apps);
	}
}

/* ---------------------------------------------------------------- doing */

LOCAL void do_open( DTWIN *d, CONST T_VOBJ *v )		{ dt_open_vobj(d, v); }
LOCAL void do_close( DTWIN *d, CONST T_VOBJ *v )	{ (void)d; dt_close_vobj(v); }
LOCAL void do_network( DTWIN *d, CONST T_VOBJ *v )	{ dt_network_open(d, v); }

/* The items that act on the virtual object the menu was opened on */
typedef struct {
	CONST char	*code;
	void		(*fn)( DTWIN *d, CONST T_VOBJ *v );
	BOOL		writes;		/* changes the window's record */
} VMOP;

LOCAL CONST VMOP vm_ops[] = {
	{ "vobj.open",		do_open,	FALSE },
	{ "vobj.close",		do_close,	FALSE },
	{ "vobj.attr",		attributes,	TRUE },
	{ "vobj.relation",	relationship,	TRUE },
	{ "vobj.reconnect",	reconnect,	TRUE },
	{ "real.rename",	rename_real,	FALSE },
	{ "real.duplicate",	duplicate_real,	TRUE },
	{ "real.newversion",	new_version,	TRUE },
	{ "real.lock",		dt_lock_app,	TRUE },
	{ "real.unlock",	dt_lock_app,	TRUE },
	{ "real.info",		dt_real_info,	FALSE },
	{ "real.network",	do_network,	FALSE },
};

#define NVMOP	( (INT)( sizeof(vm_ops) / sizeof(vm_ops[0]) ) )

EXPORT BOOL dt_exe_vmn( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj, CONST T_VOBJ *v )
{
	CONST DTPROG	*acc[VM_ACC_MAX];
	INT		i;

	if ( sel == NULL || !is_sys(&sel->def) ) {
		return FALSE;
	}
	if ( v == NULL ) {
		on_vobj = FALSE;
	}
	if ( mn_is(sel, "close") ) {
		dt_close(d);			/* nothing of the window is touched after */
		wm_composite();
		return TRUE;
	}
	if ( mn_is(sel, "trash") ) {
		dt_trash_open();
	} else if ( mn_winlist_do(sel) ) {
		/* the window chosen came to the front and takes the keys */
	} else if ( mn_is(sel, "real.search") ) {
		dt_search_open(d);
	} else if ( mn_is(sel, "acc.box") ) {
		(void)dt_prog_run(d, dt_prog_find((CONST UB *)"accessory-box"), NULL);
	} else if ( mn_is(sel, "acc.item") ) {
		/* chosen on a virtual object, the accessory is started on its object */
		if ( sel->index < menu_accessories(acc, VM_ACC_MAX) ) {
			(void)dt_prog_run(d, acc[sel->index], on_vobj ? v : NULL);
		}
	} else if ( mn_is(sel, "tray.level") ) {
		(void)tr_sel_dat(sel->index + 1);
	} else if ( mn_is(sel, "tray.clear") ) {
		(void)tr_clr_tra();
	} else if ( mn_is(sel, "exec.app") ) {
		if ( on_vobj ) {
			dt_exec(d, v, sel->index);
		}
	} else {
		for ( i = 0; i < NVMOP; i++ ) {
			if ( mn_is(sel, vm_ops[i].code) ) {
				if ( on_vobj && !( vm_ops[i].writes && d->sealed ) ) {
					vm_ops[i].fn(d, v);
				}
				break;
			}
		}
	}
	wm_composite();

	return TRUE;
}

/* ---------------------------------------------------------------- 属性変更 */

#define AT_PICT		1
#define AT_NAME		2
#define AT_ROLE		3
#define AT_TYPE		4
#define AT_UPDATE	5
#define AT_FRAME	6
#define AT_FRCOL	7
#define AT_CHCOL	8
#define AT_TBCOL	9
#define AT_BGCOL	10
#define AT_AUTO		11
#define AT_HIDDEN	12
#define AT_SIZE		13
#define AT_RATIO	20		/* and the six after it */

LOCAL void attributes( DTWIN *d, CONST T_VOBJ *start )
{
	CONST char * CONST shows[] = { "ピクトグラム", "名称", "続柄", "タイプ",
				       "更新日時", "仮身枠" };
	CONST UINT	bits[] = { TAD_D_PICT, TAD_D_NAME, TAD_D_ROLE,
				   TAD_D_TYPE, TAD_D_UPDATE, TAD_D_FRAME };
	CONST char * CONST cols[] = { "枠", "仮身文字", "仮身背景",
				      "表示領域背景" };
	CONST char * CONST ratios[] = { "1/2倍", "3/4倍", "標準", "3/2倍", "2倍",
					"3倍", "4倍" };
	CONST INT	tenths[] = { 5, 7, 10, 15, 20, 30, 40 };
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_VOBJ		v = *start;
	CONST UW	*colp[4];
	INT		pid, i, r0 = 2, chsz = ( v.chsz > 0 ) ? v.chsz : 14;
	UINT		ans;

	colp[0] = &v.frcol;  colp[1] = &v.chcol;
	colp[2] = &v.tbcol;  colp[3] = &v.bgcol;

	def = dt_pn_new(640, 300);
	if ( def == NULL ) {
		return;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 8, 300, 20, "仮身属性変更");

	/* 表示項目 */
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 34, 150, 20, "表示項目：");
	for ( i = 0; i < 6; i++ ) {
		pt = dt_pn_add(def, WM_PT_CHECK, AT_PICT + i, 14, 58 + i * 24, 170,
			    22, shows[i]);
		if ( pt != NULL ) {
			pt->value = ( ( v.disp & bits[i] ) != 0 ) ? 1 : 0;
		}
	}

	/* 色 */
	dt_pn_add(def, WM_PT_LABEL, 0, 200, 34, 150, 20, "色：");
	for ( i = 0; i < 4; i++ ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 200, 60 + i * 30, 110, 20, cols[i]);
		pt = dt_pn_add(def, WM_PT_BOX, AT_FRCOL + i, 310, 58 + i * 30, 96,
			    24, NULL);
		if ( pt != NULL ) {
			if ( *colp[i] != TAD_COL_NONE ) {
				dt_colour_text(*colp[i], pt->text);
				pt->caret = 7;
			}
		}
	}
	pt = dt_pn_add(def, WM_PT_CHECK, AT_AUTO, 200, 184, 170, 22, "自動起動");
	if ( pt != NULL ) {
		pt->value = v.autoopen ? 1 : 0;
	}
	pt = dt_pn_add(def, WM_PT_CHECK, AT_HIDDEN, 200, 208, 170, 22, "隠蔽仮身");
	if ( pt != NULL ) {
		pt->value = v.hidden ? 1 : 0;
	}

	/* 文字サイズ */
	dt_pn_add(def, WM_PT_LABEL, 0, 430, 34, 150, 20, "文字サイズ：");
	for ( i = 0; i < 7; i++ ) {
		if ( chsz * 10 == 14 * tenths[i] ) {
			r0 = i;
		}
	}
	for ( i = 0; i < 7; i++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, AT_RATIO + i, 430, 58 + i * 22,
			    110, 20, ratios[i]);
		if ( pt != NULL ) {
			pt->group = AT_RATIO;
			pt->value = ( i == r0 ) ? 1 : 0;
		}
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 430, 216, 80, 20, "自由入力");
	pt = dt_pn_add(def, WM_PT_NUMBOX, AT_SIZE, 510, 214, 60, 24, NULL);
	if ( pt != NULL ) {
		INT	k = 0, j = 0, s = chsz;
		char	num[8];

		do {
			num[j++] = (char)( '0' + s % 10 );
			s /= 10;
		} while ( s > 0 && j < 7 );
		while ( j > 0 ) {
			pt->text[k++] = (UB)num[--j];
		}
		pt->text[k] = 0;
		pt->caret = k;
		pt->value = chsz;
	}
	/* the focus goes to the colours, the first boxes to type in */
	dt_pn_buttons(def, "設定");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		UB	text[WM_LABEL_MAX];
		UW	col;
		INT	size = chsz, r = r0;
		UW	*colw[4];

		colw[0] = &v.frcol;  colw[1] = &v.chcol;
		colw[2] = &v.tbcol;  colw[3] = &v.bgcol;
		v.disp = 0;
		for ( i = 0; i < 6; i++ ) {
			if ( dt_pn_value(pid, AT_PICT + i) != 0 ) {
				v.disp |= bits[i];
			}
		}
		for ( i = 0; i < 4; i++ ) {
			if ( wm_panel_text(pid, AT_FRCOL + i, text, WM_LABEL_MAX)
			     >= E_OK && dt_colour_of(text, &col) ) {
				*colw[i] = col;
			}
		}
		v.autoopen = (BOOL)( dt_pn_value(pid, AT_AUTO) != 0 );
		v.hidden = (BOOL)( dt_pn_value(pid, AT_HIDDEN) != 0 );
		for ( i = 0; i < 7; i++ ) {
			if ( dt_pn_value(pid, AT_RATIO + i) != 0 ) {
				r = i;
			}
		}
		wm_panel_get(pid, AT_SIZE, &size);
		/* a ratio chosen overrides; otherwise what was typed */
		if ( r != r0 ) {
			size = 14 * tenths[r] / 10;
		}
		if ( size > 0 && size != chsz ) {
			/* the band grows with the letters */
			INT	band = size + 11;

			v.chsz = size;
			if ( v.bottom - v.top < band ) {
				v.bottom = v.top + band;
			}
			v.height = v.bottom - v.top;
		}
		dt_pn_done(d, pid);
		ed_set_link(d, &v);
		return;
	}
	dt_pn_done(d, pid);
}

/* ---------------------------------------------------------------- 続柄 */

/*
 * The relationship: tags in brackets belong to the object and go in its
 * metadata; the rest belong to this link and go in its record.
 */
LOCAL void relationship( DTWIN *d, CONST T_VOBJ *start )
{
	UB	text[WM_LABEL_MAX], out[WM_LABEL_MAX], tags[WM_LABEL_MAX];
	T_VOBJ	v = *start;
	INT	n, i, k = 0, t = 0;

	n = om_store_relationship(&v.target, text, WM_LABEL_MAX);
	if ( n < 0 ) {
		n = 0;
	}
	if ( v.relationship[0] != 0 ) {
		if ( n > 0 && n < WM_LABEL_MAX - 1 ) {
			text[n++] = ' ';
		}
		for ( i = 0; v.relationship[i] != 0 && n < WM_LABEL_MAX - 1; i++ ) {
			text[n++] = v.relationship[i];
		}
	}
	text[n] = 0;
	if ( !dt_ask_text(d, "続柄（リレーションタグ名）を入力してください",
		       "[タグ]形式は実身用、それ以外は仮身用（半角スペース区切りで複数設定可）",
		       text, out, WM_LABEL_MAX, "設定") ) {
		return;
	}
	/* the bracketed words for the object, the others for the link */
	for ( i = 0; out[i] != 0; i++ ) {
		if ( out[i] == '[' ) {
			while ( out[i] != 0 && out[i] != ']' && t < WM_LABEL_MAX - 2 ) {
				tags[t++] = out[i++];
			}
			tags[t++] = ']';
			if ( out[i] == 0 ) {
				break;
			}
			continue;
		}
		if ( out[i] == ' ' && ( k == 0 || v.relationship[k - 1] == ' ' ) ) {
			continue;
		}
		if ( k < TAD_REL_MAX - 1 ) {
			v.relationship[k++] = out[i];
		}
	}
	while ( k > 0 && v.relationship[k - 1] == ' ' ) {
		k--;
	}
	v.relationship[k] = 0;
	tags[t] = 0;
	om_store_set_relationship(&v.target, tags);
	ed_set_link(d, &v);
}

/* ---------------------------------------------------------------- 実身操作 */

LOCAL void rename_real( DTWIN *d, CONST T_VOBJ *v )
{
	UB	start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	DTWIN	*w;

	if ( om_store_name(&v->target, start, WM_LABEL_MAX) < 0 ) {
		start[0] = 0;
	}
	if ( !dt_ask_text(d, "実身名を入力してください", NULL, start, name,
		       WM_LABEL_MAX, "決定") || name[0] == 0 ) {
		return;
	}
	if ( om_store_rename(&v->target, name) < E_OK ) {
		return;
	}
	/* a window showing it is called by the new name too */
	w = dt_showing(&v->target);
	if ( w != NULL ) {
		wm_set_title(w->wid, (CONST char *)name);
	}
	dt_draw_all();
}

/*
 * 実身複製 from the menu: the object copied with all it holds, and a
 * link to the copy put a step down and across from the one it was
 * copied from.
 */
LOCAL void place_copy( DTWIN *d, CONST T_VOBJ *orig, CONST T_VOBJ *c );

LOCAL void duplicate_real( DTWIN *d, CONST T_VOBJ *v )
{
	UB	start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	TS_UUID	nid;
	T_VOBJ	c;
	INT	n = 0, k;

	if ( d->sealed ) {
		return;
	}
	if ( om_store_name(&v->target, start, WM_LABEL_MAX - 16) < 0 ) {
		start[0] = 0;
	}
	while ( start[n] != 0 ) {
		n++;
	}
	{
		CONST UB	*tail = (CONST UB *)"のコピー";

		for ( k = 0; tail[k] != 0 && n < WM_LABEL_MAX - 1; k++ ) {
			start[n++] = tail[k];
		}
		start[n] = 0;
	}
	if ( !dt_ask_text(d, "新しい実身の名称を入力してください", NULL, start,
		       name, WM_LABEL_MAX, "設定") || name[0] == 0 ) {
		return;
	}
	if ( om_store_copy(&v->target, name, TRUE, &nid) < E_OK ) {
		return;
	}
	c = *v;
	c.target = nid;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.name[0] = 0;
	c.dlen = 0;			/* a new object has no data appended to it yet */
	c.left += 16;  c.right += 16;
	c.top += 16;   c.bottom += 16;
	place_copy(d, v, &c);
}

/*
 * A link to a new object put where it belongs: in a figure where it
 * says, and in a document in the text right after the link it was
 * made from.
 */
LOCAL void place_copy( DTWIN *d, CONST T_VOBJ *orig, CONST T_VOBJ *c )
{
	TS_UUID	id;

	ed_before(d);
	ed_pick_none(d);
	if ( tad_lnk_add((T_TAD *)d->rec, c, &id) < E_OK ) {
		return;
	}
	if ( d->doc != NULL ) {
		dd_link_after(d, &orig->vobjid);
	}
	ed_model(d);
	d->dirty = TRUE;
	dt_draw_all();
}

/*
 * 新版作成: the object copied under the same name, the copy's links
 * pointing at the same objects as the original's, and a link to the
 * copy laid over the old one a step down and across, so that the old
 * one's edge shows behind it.
 */
#define NEW_VERSION_STEP	8

LOCAL void new_version( DTWIN *d, CONST T_VOBJ *v )
{
	UB	name[WM_LABEL_MAX];
	TS_UUID	nid;
	T_VOBJ	c;

	if ( d->sealed || om_store_name(&v->target, name, WM_LABEL_MAX) < 0 ) {
		return;
	}
	if ( om_store_copy(&v->target, name, FALSE, &nid) < E_OK ) {
		return;
	}
	c = *v;
	c.target = nid;
	knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
	c.name[0] = 0;
	c.dlen = 0;			/* a new object has no data appended to it yet */
	c.left += NEW_VERSION_STEP;  c.right += NEW_VERSION_STEP;
	c.top += NEW_VERSION_STEP;   c.bottom += NEW_VERSION_STEP;
	place_copy(d, v, &c);
}

/*
 * 起動アプリの固定: the link's applist says of the program it opens
 * with -- the entry whose "defaultOpen" is true -- whether that stays
 * so when the object is opened with another ("defaultOpenLock"). The
 * applist is kept as the JSON it was written in, and only that one
 * value of it is changed.
 */

/* The applist's entry of the program the object opens with ("defaultOpen": true) */
LOCAL BOOL app_entry( CONST UB *s, T_JSON *entry )
{
	T_JSON	root, it;
	INT	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}
	if ( js_parse(s, n, &root) < E_OK || js_type(&root) != JS_OBJECT ) {
		return FALSE;
	}
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

/* Whether the object a link points at can be reached: a cut link's cannot */
EXPORT BOOL dt_reachable( CONST T_VOBJ *v )
{
	UB	nm[8];

	return (BOOL)( v != NULL && om_store_name(&v->target, nm, sizeof(nm)) >= 0 );
}

/*
 * 接続: a cut link tried again. What was remembered of its object --
 * that it was not there -- is let go and it is looked for afresh; the
 * link is drawn as it now stands, or the person is told it is still
 * cut.
 */
LOCAL void reconnect( DTWIN *d, CONST T_VOBJ *v )
{
	if ( !dt_reachable(v) ) {
		om_store_forget(&v->target);
	}
	if ( !dt_reachable(v) ) {
		dt_tell(d, "実身に接続できませんでした", NULL);
		return;
	}
	dt_draw_all();
}

EXPORT BOOL dt_app_locked( CONST T_VOBJ *v )
{
	T_JSON	e;

	return (BOOL)( v != NULL && app_entry(v->applist, &e)
		       && js_get_bool(&e, "defaultOpenLock", FALSE) );
}

/* Whether the link names the program it opens with, which is what can be fixed */
EXPORT BOOL dt_app_lockable( CONST T_VOBJ *v )
{
	T_JSON	e;

	return (BOOL)( v != NULL && app_entry(v->applist, &e) );
}

EXPORT void dt_lock_app( DTWIN *d, CONST T_VOBJ *v )
{
	T_VOBJ		c = *v;
	UB		*s = c.applist, ins[40];
	CONST char	*val = dt_app_locked(v) ? "false" : "true";
	T_JSON		e, lock;
	INT		at, cut = 0, n = 0, len = 0, i;

	if ( d->sealed || !app_entry(s, &e) ) {
		return;
	}
	while ( s[len] != 0 ) {
		len++;
	}
	if ( js_get(&e, "defaultOpenLock", &lock) >= E_OK ) {
		at = (INT)( lock.s - s );
		cut = lock.len;
	} else {
		CONST char	*key = ",\"defaultOpenLock\":";

		/* before the entry's closing brace */
		at = (INT)( e.s - s ) + e.len - 1;
		for ( i = 0; key[i] != 0; i++ ) {
			ins[n++] = (UB)key[i];
		}
	}
	for ( i = 0; val[i] != 0; i++ ) {
		ins[n++] = (UB)val[i];
	}
	if ( len - cut + n >= TAD_APPL_MAX ) {
		return;
	}
	if ( n > cut ) {
		for ( i = len; i >= at + cut; i-- ) {
			s[i + n - cut] = s[i];
		}
	} else {
		for ( i = at + cut; i <= len; i++ ) {
			s[i + n - cut] = s[i];
		}
	}
	for ( i = 0; i < n; i++ ) {
		s[at + i] = ins[i];
	}
	ed_set_link(d, &c);
}

