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


LOCAL void rename_real( DTWIN *d, CONST T_VOBJ *v );

/* ---------------------------------------------------------------- the system's definitions */

LOCAL CONST char * CONST sys_menus[] = {
	SYSDEF_MENU_WINDOW, SYSDEF_MENU_OBJECT, SYSDEF_MENU_VOBJ,
	SYSDEF_MENU_REAL, SYSDEF_MENU_ACC, SYSDEF_MENU_EXEC, SYSDEF_MENU_TRAY,
	SYSDEF_MENU_WINLIST, SYSDEF_MENU_TAIL, SYSDEF_MENU_DISK
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

/*
 * The accessories 小物 shows, in the order the menu lists them: every
 * one, in a list of its own (free it). How many; 0 with none or no memory.
 */
LOCAL INT menu_accessories( CONST DTPROG ***p_acc )
{
	return dt_acc_menu_list(p_acc);
}

/* The accessory at place i of 小物, NULL none */
LOCAL CONST DTPROG *menu_accessory( INT i )
{
	CONST DTPROG	**acc, *p = NULL;
	INT		n = menu_accessories(&acc);

	if ( i >= 0 && i < n ) {
		p = acc[i];
	}
	if ( acc != NULL ) Kfree(acc);
	return p;
}

/* 小物's list: a line for each accessory */
LOCAL void acc_fill( ID mid )
{
	TS_UUID		ad = sys_def(SYSDEF_MENU_ACC);
	CONST DTPROG	**acc;
	UB		(*names)[WM_LABEL_MAX];
	INT		n = menu_accessories(&acc), i, k;

	names = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * (SZ)( n + 1 ));
	if ( names != NULL ) {
		for ( i = 0; i < n; i++ ) {
			for ( k = 0; acc[i]->name[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) {
				names[i][k] = acc[i]->name[k];
			}
			names[i][k] = 0;
		}
		(void)mn_set_lst(mid, &ad, "acc.item", (CONST UB *)names, WM_LABEL_MAX, n);
		Kfree(names);
	}
	if ( acc != NULL ) Kfree(acc);
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
	TS_UUID		rd = sys_def(SYSDEF_MENU_REAL);
	TS_UUID		ed = sys_def(SYSDEF_MENU_EXEC);
	UB		(*apps)[OM_APP_NAME];
	BOOL		open, cut, locked, sealed = TRUE;	/* a part's window keeps no record: nothing of a link is changed in it */
	INT		n, i;

	tray_state(mid);

	/* 小物: the accessories that show in the menu */
	acc_fill(mid);

	/* ウインドウ: the windows open, nearest first */
	mn_winlist_fill(mid);

	/* ディスク操作: 切り離し on a link to an object of a disk that may be taken away */
	{
		TS_UUID	dd = sys_def(SYSDEF_MENU_DISK);

		atr(mid, &dd, "disk.detach", (BOOL)( !on_vobj || !dt_disk_detachable(v) ));
		atr(mid, &dd, "disk.grow", (BOOL)( on_vobj && !dt_disk_growable(v) ));
	}

	/* away from a virtual object, what is about one cannot be chosen */
	if ( !on_vobj || v == NULL ) {
		atr(mid, &obj, "vobj", TRUE);
		atr(mid, &obj, "real", TRUE);
		atr(mid, &obj, "exec", TRUE);
#ifdef USE_UITEST
		tm_printf((UB *)"desktop: exec menu grey\n");	/* for the tests */
#endif
		return;
	}
	open = dt_obj_open(v, NULL);	/* in any window, or one being started on it */
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
			 locked ? MN_HIDE : ( cut || sealed || !dt_app_lockable(v) ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, &rd, "real.unlock",
			 !locked ? MN_HIDE : ( cut || sealed ) ? MN_GREY : 0);

	/*
	 * 実行: the list 付箋指定 of 管理情報 shows too (dt_app_list) -- the
	 * programs the real object's applist names, or, when it names none,
	 * those that open an object of its kind (fileTypes). Grey while the
	 * object is open: it is open in one window only, and opening it
	 * again brings that one forward whatever program is chosen.
	 */
	atr(mid, &obj, "exec", open);
	if ( open ) {
		atr(mid, &ed, "exec.app", TRUE);
#ifdef USE_UITEST
		tm_printf((UB *)"desktop: exec menu open\n");	/* for the tests */
#endif
		return;
	}
	{
		INT	room = dt_app_list(v, NULL, 0, NULL), q;
		DTAPP	*a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));

		apps = (UB (*)[OM_APP_NAME])Kmalloc(sizeof(UB) * OM_APP_NAME * (SZ)( room + 1 ));
		if ( apps != NULL && a != NULL ) {
			n = dt_app_list(v, a, room, NULL);
			for ( i = 0; i < n; i++ ) {
				for ( q = 0; a[i].name[q] != 0 && q < OM_APP_NAME - 1; q++ ) apps[i][q] = a[i].name[q];
				apps[i][q] = 0;
			}
			(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)apps, OM_APP_NAME, n);
			atr(mid, &ed, "exec.app", cut);
#ifdef USE_UITEST
			tm_printf((UB *)"desktop: exec menu on %d\n", n);	/* for the tests */
#endif
		}
		if ( apps != NULL ) Kfree(apps);
		if ( a != NULL ) Kfree(a);
	}
}

/* ---------------------------------------------------------------- the tail of a program's menu */

/*
 * The menu of a process's window, and that of a window of the desktop's
 * with a part of its own (DTOWN), ends with ウインドウ, 小物 and 実行 as
 * every window's does (SYSDEF_MENU_TAIL). 小物 lists the accessories;
 * 実行 is shown grey, as there is no virtual object under the pointer to
 * run (a link in a slide is one: its 実行 is set for it).
 * An accessory chosen there is started from the desktop's own loop: the
 * choice is made in the task of whoever opened the menu.
 */
LOCAL volatile INT	dt_tail_req = 0;	/* n + 1 the n-th accessory */

EXPORT void dt_tail_fill( ID mid )
{
	TS_UUID		td = sys_def(SYSDEF_MENU_TAIL);

	acc_fill(mid);
	atr(mid, &td, "exec", TRUE);
}

EXPORT BOOL dt_tail_do( CONST T_MNSEL *sel )
{
	if ( sel == NULL || !is_sys(&sel->def) ) {
		return FALSE;
	}
	if ( mn_is(sel, "acc.item") ) {
		dt_tail_req = sel->index + 1;
		return TRUE;
	}
	return FALSE;
}

/* An accessory chosen in a program's menu, started */
EXPORT void dt_tail_poll( void )
{
	CONST DTPROG	*a;
	INT		r = dt_tail_req;

	if ( r == 0 ) {
		return;
	}
	dt_tail_req = 0;
	if ( ( a = menu_accessory(r - 1) ) != NULL ) {
		(void)dt_prog_run(NULL, a, NULL);
	}
}

EXPORT void dt_tail_start( void )
{
	mn_tail_hooks(dt_tail_fill, dt_tail_do);
}

EXPORT void dt_tail_stop( void )
{
	mn_tail_hooks(NULL, NULL);
}

/* ---------------------------------------------------------------- doing */

LOCAL void do_open( DTWIN *d, CONST T_VOBJ *v )		{ dt_open_vobj(d, v); }
LOCAL void do_close( DTWIN *d, CONST T_VOBJ *v )	{ (void)d; dt_close_vobj(v); }
LOCAL void do_network( DTWIN *d, CONST T_VOBJ *v )	{ dt_network_open(d, v); }

/*
 * The items that act on the virtual object the menu was opened on, in
 * a window of a part's (a part shows links it holds no record of: what
 * would change the link is the program's that holds it, dt_proc_link_op)
 */
typedef struct {
	CONST char	*code;
	void		(*fn)( DTWIN *d, CONST T_VOBJ *v );
} VMOP;

LOCAL CONST VMOP vm_ops[] = {
	{ "vobj.open",		do_open },
	{ "vobj.close",		do_close },
	{ "real.rename",	rename_real },
	{ "real.info",		dt_real_info },
	{ "real.network",	do_network },
};

#define NVMOP	( (INT)( sizeof(vm_ops) / sizeof(vm_ops[0]) ) )

EXPORT BOOL dt_exe_vmn( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj, CONST T_VOBJ *v )
{
	CONST DTPROG	*a;
	INT		i;

	if ( sel == NULL || !is_sys(&sel->def) ) {
		return FALSE;
	}
	if ( v == NULL ) {
		on_vobj = FALSE;
	}
	if ( mn_is(sel, "close") ) {
		dt_close(d);			/* nothing of the window is touched after */
		wm_update();
		return TRUE;
	}
	if ( mn_is(sel, "trash") ) {
		dt_trash_open();
	} else if ( mn_winlist_do(sel) ) {
		/* the window chosen came to the front and takes the keys */
	} else if ( mn_is(sel, "real.search") ) {
		dt_search_open(d);
	} else if ( mn_is(sel, "acc.item") ) {
		/* chosen on a virtual object, the accessory is started on its object */
		if ( ( a = menu_accessory(sel->index) ) != NULL ) {
			(void)dt_prog_run(d, a, on_vobj ? v : NULL);
		}
	} else if ( mn_is(sel, "disk.connect") || mn_is(sel, "disk.status") || mn_is(sel, "disk.create")
		 || mn_is(sel, "disk.grow") || ( mn_is(sel, "disk.detach") && on_vobj ) ) {
		/* ディスク操作: the program does it, told the item and the link */
		dt_disk_menu(d, (CONST char *)sel->code, on_vobj ? v : NULL);
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
				if ( on_vobj ) {
					vm_ops[i].fn(d, v);
				}
				break;
			}
		}
	}
	wm_update();

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

/*
 * A link's height h once its letters or what its band shows have
 * changed from 'was' to 'now'.
 */
LOCAL INT refit_h( CONST T_VOBJ *was, CONST T_VOBJ *now, INT h )
{
	INT	c = tad_lnk_closed_h(now);

	if ( c <= 0 ) {
		return h;			/* no band: a plain box keeps its size */
	}
	if ( h <= tad_lnk_closed_h(was) + TAD_OPEN_SLACK ) {
		return c;			/* closed before, closed now */
	}

	return tad_lnk_fit_h(now, h);
}

/* The panel of 属性変更: TRUE with what was chosen in *io when it was agreed to */
LOCAL BOOL attr_ask( DTWIN *d, T_VOBJ *io )
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
	T_VOBJ		v = *io;
	CONST UW	*colp[4];
	UW		shown[4];
	INT		pid, i, r0 = 2, chsz = ( v.chsz > 0 ) ? v.chsz : 14;
	UINT		ans;

	colp[0] = &v.frcol;  colp[1] = &v.chcol;
	colp[2] = &v.tbcol;  colp[3] = &v.bgcol;

	def = dt_pn_new(640, 300);
	if ( def == NULL ) {
		return FALSE;
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
		shown[i] = ( *colp[i] != TAD_COL_NONE ) ? *colp[i] : ( i < 2 ) ? 0x00000000U : 0x00FFFFFFU;
		dt_pn_colour(def, AT_FRCOL + i, 310, 58 + i * 30, 96, 24, shown[i]);
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
	dt_pn_buttons(def, "設定");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
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
		/* a colour left as first shown stays unset if it was unset */
		for ( i = 0; i < 4; i++ ) {
			if ( wm_panel_colour(pid, AT_FRCOL + i, &col) >= E_OK && col != shown[i] ) {
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
			v.chsz = size;
		}
		if ( v.chsz != io->chsz || v.disp != io->disp ) {
			/*
			 * The closed height follows the letters and what the
			 * band shows: one that was closed stays closed at the
			 * new height, and an opened one is never lower than
			 * that or so little above it that it would read as
			 * closed. A link in a text with no box of its own is
			 * laid out at no less than that already.
			 */
			if ( v.right > v.left ) {
				v.bottom = v.top + refit_h(io, &v, v.bottom - v.top);
				v.height = v.bottom - v.top;
			}
			if ( v.heightpx > 0 ) {
				v.heightpx = refit_h(io, &v, v.heightpx);
			}
		}
		dt_pn_done(d, pid);
		*io = v;
		return TRUE;
	}
	dt_pn_done(d, pid);
	return FALSE;
}

/* ---------------------------------------------------------------- 続柄 */

/*
 * The relationship: tags in brackets belong to the object and go in its
 * metadata; the rest belong to this link and go in its record.
 */
LOCAL BOOL rel_ask( DTWIN *d, T_VOBJ *io )
{
	UB	text[WM_LABEL_MAX], out[WM_LABEL_MAX], tags[WM_LABEL_MAX];
	T_VOBJ	v = *io;
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
		return FALSE;
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
	*io = v;
	return TRUE;
}

/* ---------------------------------------------------------------- 実身操作 */

LOCAL void rename_real( DTWIN *d, CONST T_VOBJ *v )
{
	UB	start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	INT	wid;

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
	if ( ( wid = wm_obj_showing(&v->target) ) > 0 ) {
		wm_set_title(wid, (CONST char *)name);
	}
}

/* The object copied under a name asked for, the copy in *p_nid; FALSE when it was not */
LOCAL BOOL dup_ask( DTWIN *d, CONST T_VOBJ *v, TS_UUID *p_nid )
{
	UB	start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	INT	n = 0, k;

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
		return FALSE;
	}
	return (BOOL)( om_store_copy(&v->target, name, TRUE, p_nid) >= E_OK );
}

/* Whether the object a link points at can be reached: a cut link's cannot */
EXPORT BOOL dt_reachable( CONST T_VOBJ *v )
{
	UB	nm[8];

	return (BOOL)( v != NULL && om_store_name(&v->target, nm, sizeof(nm)) >= 0 );
}

/*
 * 起動アプリの固定 (design 11.3): whether the link keeps to the program
 * it opens with ("defaultOpenLock" on the default entry of its own
 * applist), or opens with the one run last on its object. Fixing it
 * gives the link the applist its object has now, so that it keeps to
 * the program the object opens with at that moment (om_link_lock).
 */
EXPORT BOOL dt_app_locked( CONST T_VOBJ *v )
{
	return om_link_locked(v);
}

/* Whether there is a program the link opens with, which is what can be fixed */
EXPORT BOOL dt_app_lockable( CONST T_VOBJ *v )
{
	UB	app[OM_APP_ID];

	return (BOOL)( v != NULL && om_link_default_app(v, app, sizeof(app)) > 0 );
}

/* ---------------------------------------------------------------- a program's links */

/*
 * The applist a program carried of its link (DT_F_APPLIST), given to the
 * link the desktop makes of the request: the program it opens with is
 * then decided as for a link of the desktop's own (om_link_default_app),
 * the one it keeps to when it is fixed (起動アプリの固定).
 */
EXPORT void dt_req_applist( CONST T_DTREQ *rq, T_VOBJ *v )
{
	INT	i;

	v->applist[0] = 0;
	if ( ( rq->flags & DT_F_APPLIST ) == 0 ) {
		return;
	}
	for ( i = 0; rq->text[i] != 0 && i < TAD_APPL_MAX - 1 && i < DT_CARRY_TEXT - 1; i++ ) {
		v->applist[i] = rq->text[i];
	}
	v->applist[i] = 0;
}

/* A link as a program says it looks, at the corner of a box of its size */
LOCAL void link_of_req( CONST T_DTREQ *rq, T_VOBJ *v )
{
	INT	i;

	knl_memset(v, 0, sizeof(*v));
	v->target = rq->target;
	v->recno = -1;
	v->right = rq->look.w;
	v->bottom = rq->look.h;
	v->height = rq->look.h;
	v->chsz = rq->look.chsz;
	v->frcol = rq->look.frcol;
	v->chcol = rq->look.chcol;
	v->tbcol = rq->look.tbcol;
	v->bgcol = rq->look.bgcol;
	v->disp = ( rq->look.disp != 0 ) ? rq->look.disp : TAD_D_DEFAULT;
	v->autoopen = (BOOL)( rq->look.autoopen != 0 );
	v->hidden = (BOOL)( ( rq->flags & DT_F_HIDDEN ) != 0 );
	v->zoom = 100;
	dt_req_applist(rq, v);
	for ( i = 0; rq->name[i] != 0 && i < TAD_REL_MAX - 1 && i < (INT)sizeof(rq->name) - 1; i++ ) {
		v->relationship[i] = rq->name[i];
	}
	v->relationship[i] = 0;
}

LOCAL void text_out( UB *out, CONST UB *s )
{
	INT	i;

	for ( i = 0; s[i] != 0 && i < DT_TEXT_MAX - 1; i++ ) {
		out[i] = s[i];
	}
	out[i] = 0;
}

/*
 * DT_RQ_APPS: the entry at rq->recno of the list 実行 shows for a
 * program's link (dt_app_list), the id the applist writes for one it
 * names (so that DT_RQ_EXEC finds it there), a tab and the name. For an
 * object open already the list is empty and flags say DT_F_OPEN, so
 * that every program greys 実行 as the desktop does.
 */
EXPORT ER dt_apps_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	DTAPP	*a = NULL;
	INT	room, n = 0, k = 0, i;
	ER	er = E_NOEXS;

	if ( v == NULL ) {
		return E_NOMEM;
	}
	link_of_req(rq, v);
	/* open already: 実行 of the program's menu is grey, as the desktop's is (dt_get_vmn) */
	if ( dt_obj_open(v, NULL) ) {
		an->how = 0;
		an->flags = DT_F_OPEN;
		Kfree(v);
		return E_NOEXS;
	}
	room = dt_app_list(v, NULL, 0, NULL);
	a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));
	if ( a == NULL ) {
		Kfree(v);
		return E_NOMEM;
	}
	n = dt_app_list(v, a, room, NULL);
	an->how = (UINT)n;
	an->flags = 0;
	if ( rq->recno >= 0 && rq->recno < n ) {
		CONST DTAPP	*e = &a[rq->recno];
		CONST UB	*id = ( e->listed && e->raw[0] != 0 ) ? e->raw : e->id;

		for ( i = 0; id[i] != 0 && k < DT_TEXT_MAX - 2; i++ ) an->text[k++] = id[i];
		an->text[k++] = '\t';
		for ( i = 0; e->name[i] != 0 && k < DT_TEXT_MAX - 1; i++ ) an->text[k++] = e->name[i];
		an->text[k] = 0;
		if ( !e->listed ) an->flags = DT_F_KIND;
		er = E_OK;
	}
	Kfree(a);
	Kfree(v);
	return er;
}

/*
 * DT_RQ_LINK_STATE: what the menu of a link of the desktop's looks at
 * to grey 〈開く〉, 〈閉じる〉 and 〈開くプログラムを固定〉, for a
 * program's link
 */
EXPORT ER dt_link_state_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));

	if ( v == NULL ) {
		return E_NOMEM;
	}
	link_of_req(rq, v);
	an->flags = 0;
	if ( dt_obj_open(v, NULL) ) {
		an->flags |= DT_F_OPEN;
	}
	if ( dt_app_lockable(v) ) {
		an->flags |= DT_F_LOCKABLE;
	}
	if ( dt_disk_detachable(v) ) {
		an->flags |= DT_F_DETACHABLE;	/* ディスク操作's 切り離し */
	}
	if ( dt_disk_growable(v) ) {
		an->flags |= DT_F_GROWABLE;	/* and its 拡張 */
	}
	Kfree(v);
	return E_OK;
}

EXPORT ER dt_proc_link_op( CONST T_DTREQ *rq, T_DTANS *an )
{
	DTWIN	*host = dt_host_of(&rq->win);
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	UB	name[WM_LABEL_MAX];
	ER	er = E_OK;

	if ( v == NULL ) {
		return E_NOMEM;
	}
	link_of_req(rq, v);
	wm_obj_quiet(TRUE);		/* what answers the panels is theirs, not the program's */
	if ( host == NULL && rq->req != DT_RQ_CLOSE && rq->req != DT_RQ_EXEC
	  && rq->req != DT_RQ_NETWORK && rq->req != DT_RQ_FULL ) {
		wm_obj_quiet(FALSE);
		Kfree(v);
		return E_NOEXS;			/* nowhere to ask in */
	}
	switch ( rq->req ) {
	case DT_RQ_CLOSE:
		dt_close_vobj(v);
		break;
	case DT_RQ_ATTR:
		if ( !attr_ask(host, v) ) {
			er = E_ABORT;
			break;
		}
		an->look.w = v->right - v->left;
		an->look.h = v->bottom - v->top;
		an->look.chsz = v->chsz;
		an->look.frcol = v->frcol;
		an->look.chcol = v->chcol;
		an->look.tbcol = v->tbcol;
		an->look.bgcol = v->bgcol;
		an->look.disp = v->disp;
		an->look.autoopen = v->autoopen ? 1 : 0;
		an->flags = v->hidden ? DT_F_HIDDEN : 0;
		break;
	case DT_RQ_RELATION:
		if ( !rel_ask(host, v) ) {
			er = E_ABORT;
			break;
		}
		text_out(an->text, v->relationship);
		break;
	case DT_RQ_RENAME:
		rename_real(host, v);
		if ( om_store_name(&v->target, name, WM_LABEL_MAX) >= 0 ) {
			text_out(an->text, name);
		}
		break;
	case DT_RQ_DUPLICATE:
		if ( !dup_ask(host, v, &an->into) ) {
			er = E_ABORT;
		}
		break;
	case DT_RQ_NEWVER:
		if ( om_store_name(&v->target, name, WM_LABEL_MAX) < 0
		  || om_store_copy(&v->target, name, FALSE, &an->into) < E_OK ) {
			er = E_IO;
		}
		break;
	case DT_RQ_INFO:
		dt_real_info(host, v);
		break;
	case DT_RQ_NETWORK:
		dt_network_open(host, v);
		break;
	case DT_RQ_SEARCH:
		dt_search_open(host);
		break;
	case DT_RQ_EXEC:
		text_out(name, rq->name);
		if ( name[0] == 0 ) {
			er = E_PAR;
			break;
		}
		if ( ( rq->flags & DT_F_KIND ) != 0 ) {
			/* one offered for the object's kind: the program it opens with stays */
		} else if ( ( rq->flags & DT_F_APPLIST ) != 0 ) {
			(void)om_link_ran(v, name);	/* the link's applist says whether it keeps to its program */
		} else if ( ( rq->flags & DT_F_KEEP ) == 0 ) {
			(void)om_store_set_default_app(&v->target, name);
		}
		v->frcol = v->chcol = v->tbcol = v->bgcol = TAD_COL_NONE;
		er = dt_open_as(NULL, v, name);
		break;
	case DT_RQ_COLOUR: {
		UW	col = ( rq->look.bgcol == TAD_COL_NONE ) ? 0x00FFFFFFU : rq->look.bgcol;

		if ( !( ( ( rq->flags & DT_F_AT ) != 0 )
			? dt_pick_colour_at(host->wid, &col, rq->grab_x, rq->grab_y )
			: dt_pick_colour(host->wid, &col) ) ) {
			er = E_ABORT;
			break;
		}
		dt_draw(host);
		an->look.bgcol = col;
		break;
	}
	case DT_RQ_PAPER: {
		/*
		 * 仮身属性と同じ starts on when the object's metadata gives it
		 * no paper of its own, showing the colour the program asked
		 * with; kept on, the paper is taken away (TAD_COL_NONE)
		 */
		UW	col = TAD_COL_NONE;
		BOOL	same = (BOOL)( om_store_paper(&v->target, &col) < E_OK || col == TAD_COL_NONE );
		ER	put;

		if ( same ) {
			col = ( rq->look.bgcol != TAD_COL_NONE ) ? rq->look.bgcol : 0x00FFFFFFU;
		}
		tm_printf((UB *)"desktop: paper asked, same %s\n", same ? "on" : "off");
		if ( !dt_pick_paper(host->wid, &col, &same) ) {
			er = E_ABORT;
			break;
		}
		dt_draw(host);
		if ( same ) {
			col = TAD_COL_NONE;
		}
		put = om_store_set_paper(&v->target, col);
		an->look.bgcol = col;
		tm_printf((UB *)"desktop: paper %s %06x (%d)\n", same ? "same" : "own", (UINT)( col & 0x00FFFFFFU ), (INT)put);
		break;
	}
	case DT_RQ_FULL: {
		/* 全画面表示 or back, kept here as a double press on the window's corner keeps it */
		BOOL	full = FALSE;

		er = dt_obj_fullscreen(wm_obj_wid(&rq->win), (BOOL)( ( rq->flags & DT_F_ASK ) == 0 ), &full);
		if ( er >= E_OK && full ) {
			an->flags |= DT_F_FULL;
		}
		break;
	}
	default:
		er = E_PAR;
		break;
	}
	wm_obj_quiet(FALSE);
	Kfree(v);
	wm_update();
	return er;
}

/* ---------------------------------------------------------------- links to new objects */

/*
 * The size a link to a new object is put with, given the link it was
 * made from (v, its letters and what it shows) and the height that one
 * stood at (h). Closed -- no taller than its band and TAD_OPEN_SLACK --
 * it is as wide as the new object's name measured in the system's face
 * at the link's letters (14 when it does not say), with its picture and
 * edges (tad_closed_w), and as tall as it is closed; the face is left as
 * it was set. FALSE, nothing set, when it stood opened or shows no band:
 * it keeps the size it had.
 */
LOCAL BOOL new_link_size( CONST T_VOBJ *v, INT h, CONST TS_UUID *target, INT *p_w, INT *p_h )
{
	UB		nm[TAD_NAME_MAX];
	T_FNSTATE	st;
	ID		fid;
	INT		chsz = ( v->chsz > 0 ) ? v->chsz : 14;
	INT		band = tad_lnk_closed_h(v), w = chsz * 6;

	if ( band <= 0 || h > band + TAD_OPEN_SLACK ) {
		return FALSE;
	}
	if ( om_store_name(target, nm, sizeof(nm)) > 0 ) {
		fn_hold();
		fid = fn_system();
		if ( fid > 0 && fn_state_get(fid, &st) >= E_OK ) {
			if ( fn_set_size(fid, chsz) >= E_OK ) {
				INT	m = fn_width(fid, nm);

				if ( m >= 0 ) w = m;
			}
			(void)fn_state_set(fid, &st);
		}
		fn_release();
	}
	*p_w = tad_closed_w(chsz, v->disp, w);
	*p_h = band;
	return TRUE;
}

/*
 * A link c to a new object, made with the look of the link it was made
 * from, sized to the new object's name when it is closed: in a text by
 * its width and height, in a figure by its rectangle from its corner.
 */
EXPORT void dt_link_fit( T_VOBJ *c )
{
	BOOL	text = (BOOL)( c->right <= c->left && c->width > 0 );
	INT	w, h;

	if ( !new_link_size(c, text ? c->heightpx : c->bottom - c->top, &c->target, &w, &h) ) {
		return;
	}
	if ( text ) {
		c->width = w;
		c->heightpx = h;
	} else {
		c->right = c->left + w;
		c->bottom = c->top + h;
		c->height = h;
	}
}

/*
 * The copy of 新たな実身に保存 made of object id, as it is in the store,
 * under a name asked for in a panel over ask, and a link to it put with
 * the link parent_vobj of the record of parent: in a text, just after
 * it. With target given, that link must point at it. The copy in
 * *p_new; E_ABORT when the panel was put away, E_NOEXS when the parent
 * holds no such link.
 */
EXPORT ER dt_save_new_of( DTWIN *ask, CONST TS_UUID *id, CONST TS_UUID *parent,
			  CONST TS_UUID *parent_vobj, CONST TS_UUID *target, TS_UUID *p_new )
{
	UB	start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	TS_UUID	nid, lid;
	T_TAD	*prec;
	T_VOBJ	pv;
	INT	n = 0, k;
	BOOL	held;
	ER	er;

	/* the link it goes with, in the record that holds it: a program's, while it runs, is its own to change */
	held = dt_plink_has(parent, parent_vobj, target);
	prec = om_store_get(parent, 0);
	if ( !held && ( prec == NULL || tad_lnk_find(prec, parent_vobj, &pv) < E_OK
		       || ( target != NULL && ts_uuid_cmp(&pv.target, target) != 0 ) ) ) {
		return E_NOEXS;
	}
	if ( om_store_name(id, start, WM_LABEL_MAX - 16) < 0 ) {
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
	if ( !dt_ask_text(ask, "新しい実身の名称を入力してください", NULL, start,
		       name, WM_LABEL_MAX, "設定") || name[0] == 0 ) {
		return E_ABORT;
	}
	er = om_store_copy(id, name, FALSE, &nid);
	if ( er < E_OK ) {
		return er;
	}
	*p_new = nid;
	if ( held && dt_plink_add(parent, parent_vobj, &nid) >= E_OK ) {
		return E_OK;	/* the program puts the link to the copy in, and keeps it with its record */
	}
	/* the program ended meanwhile, its record saved: put in as the store keeps it */

	/* the parent's record read again: the panel let the person work in the meantime */
	prec = om_store_get(parent, 0);
	if ( prec == NULL || tad_lnk_find(prec, parent_vobj, &pv) < E_OK ) {
		return E_OK;		/* the copy stands as a 屑実身 until it is linked */
	}
	pv.target = nid;
	knl_memset(&pv.vobjid, 0, sizeof(pv.vobjid));
	pv.name[0] = 0;
	pv.dlen = 0;			/* a new object has no data appended to it yet */
	dt_link_fit(&pv);		/* named anew: closed, as wide as that name */
	if ( tad_lnk_add(prec, &pv, &lid) >= E_OK ) {
		om_store_save(parent, 0);
	}
	return E_OK;
}
