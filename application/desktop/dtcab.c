/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtcab.c
 *	The cabinet's menu and what its own items do (design 16.5.13)
 *
 *	The menu is made from the cabinet's definition (CABMENU.DEF); the
 *	items every window has are the system's (dtvmn.c). What is here is
 *	the cabinet's own work: saving, the view, the clipboard, the order
 *	and protection of what is taken, and tidying it.
 *
 *	Everything goes through the record, as dtedit.c does, or through
 *	the object's metadata; nothing is kept on the side.
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
#include <ts/cabmenu.h>
#include <ts/figedit.h>
#include <ts/mn.h>
#include <ts/fn.h>
#include <ts/sysdef.h>
#include "desktop.h"

LOCAL void save_as_new( DTWIN *d );
LOCAL void bg_colour( DTWIN *d );
LOCAL void arrange( DTWIN *d );

/* ---------------------------------------------------------------- the menu */

EXPORT ER dt_cab_menu_make( DTWIN *d, ID *p_mid )
{
	T_CABMENU	st;

	knl_memset(&st, 0, sizeof(st));
	st.npick = d->npick;
	st.can_save_new = (BOOL)( d->has_parent && !d->sealed );
	st.full = d->full;
	st.show_hidden = d->show_hidden;
	st.prot = d->sealed ? 0 : fe_prot_can(d->rec, d->fig, d->pick);
#ifdef USE_UITEST
	tm_printf((UB *)"desktop: protect menu %x\n", ( st.npick == 0 ) ? 0 : st.prot);	/* for the tests */
#endif
	st.def = d->accbox ? SYSDEF_MENU_ACCBOX : NULL;
	{
		INT	links = 0;

		st.can_paste = (BOOL)( !d->sealed && dt_tray_kind(&links) == DT_TRAY_FIG && links > 0 );
	}

	return cab_menu_make(&st, p_mid);
}

EXPORT void dt_cab_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			    CONST T_VOBJ *v )
{
	INT	cmd = cab_menu_cmd(sel);

	if ( d->accbox && dt_acc_menu_do(sel, on_vobj, v) ) {
		return;
	}
	if ( cmd != 0 ) {
		dt_cab_command(d, cmd, on_vobj, v);
	}
}

/* ---------------------------------------------------------------- 保存 */

/*
 * 新たな実身に保存: what is in the window saved, then copied to a new
 * object, whose links are to the same objects as this one's; a link to
 * it is put over the link this window was opened from.
 */
LOCAL void save_as_new( DTWIN *d )
{
	TS_UUID	nid;

	if ( !d->has_parent || d->sealed ) {
		return;
	}
	dt_save(d);
	(void)dt_save_new_of(d, &d->id, &d->parent_id, &d->parent_vobj, NULL, &nid);
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
	DTWIN	*p;
	INT	n = 0, k;
	BOOL	held;
	ER	er;

	/* the link it goes with, in the record that holds it: a program's, while it runs, is its own to change */
	held = dt_plink_has(parent, parent_vobj, target);
	p = dt_showing(parent);
	prec = ( p != NULL ) ? p->rec : om_store_get(parent, 0);
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
	/* the program ended meanwhile, its record saved: put in as into a record of the desktop's */

	/* the parent's record read again: the panel let the person work in the meantime */
	p = dt_showing(parent);
	prec = ( p != NULL ) ? p->rec : om_store_get(parent, 0);
	if ( prec == NULL || tad_lnk_find(prec, parent_vobj, &pv) < E_OK ) {
		return E_OK;		/* the copy stands as a 屑実身 until it is linked */
	}
	pv.target = nid;
	knl_memset(&pv.vobjid, 0, sizeof(pv.vobjid));
	pv.name[0] = 0;
	pv.dlen = 0;			/* a new object has no data appended to it yet */
	ed_new_link_fit(&pv);		/* named anew: closed, as wide as that name */
	if ( p != NULL ) {
		if ( p->sealed ) {
			return E_OK;	/* a page no one keeps: the new object is a 屑実身 until linked */
		}
		ed_pick_none(p);
		if ( tad_lnk_add(prec, &pv, &lid) >= E_OK && p->doc != NULL ) {
			dd_link_after(p, parent_vobj);	/* in a text, just after the one it came from */
		}
		ed_model(p);
		p->dirty = TRUE;
		dt_draw_all();
	} else if ( tad_lnk_add(prec, &pv, &lid) >= E_OK ) {
		om_store_save(parent, 0);
	}
	return E_OK;
}

/* ---------------------------------------------------------------- 表示 */

LOCAL void bg_colour( DTWIN *d )
{
	UW	col = ( d->paper == TV_PAPER_LOOK ) ? 0x00FFFFFFU : d->paper;

	if ( !dt_pick_colour(d->wid, &col) ) {
		dt_draw(d);
		return;
	}
	d->paper = col;
	om_store_set_paper(&d->id, col);
	dt_draw_all();
}

/* ---------------------------------------------------------------- 整頓 */

/* The numbers of the parts: a group of choices is its tens */
#define AR_P_COUNT	1
#define AR_G_H		10		/* 横 */
#define AR_G_V		20		/* 縦 */
#define AR_G_COL	30		/* 段組 */
#define AR_G_W		40		/* 幅調整 */
#define AR_G_SORT	50		/* 整列順 */
#define AR_G_ORDER	60		/* 順序 */

/* A group of choices down a column, the last one chosen to start with */
LOCAL INT ar_group( T_WMPANEL *p, INT group, INT x, INT y, CONST char *title,
		    CONST char * CONST *names, INT n, BOOL grey, INT on )
{
	T_WMPART	*pt;
	INT		i;

	dt_pn_add(p, WM_PT_LABEL, 0, x, y, 70, 20, title);
	for ( i = 0; i < n; i++ ) {
		pt = dt_pn_add(p, WM_PT_CHOICE | ( grey && i < n - 1 ? P_DISABLE : 0 ),
			    group + i, x + 70, y + i * 22, 120, 20, names[i]);
		if ( pt != NULL ) {
			pt->group = group;
			pt->value = ( i == on ) ? 1 : 0;
		}
	}

	return y + n * 22 + 8;
}

/* Which of a group was chosen: its place in the group */
LOCAL INT ar_chosen( INT pid, INT group, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( dt_pn_value(pid, group + i) != 0 ) {
			return i;
		}
	}

	return n - 1;
}

LOCAL void arrange( DTWIN *d )
{
	CONST char * CONST h_n[] = { "左揃え", "右揃え", "なし" };
	CONST char * CONST v_n[] = { "詰める", "上揃え", "なし" };
	CONST char * CONST c_n[] = { "1段", "左右", "上下", "なし" };
	CONST char * CONST w_n[] = { "最初", "全項目", "名前まで", "続柄まで",
				     "日付除く", "なし" };
	CONST char * CONST s_n[] = { "名前", "作成日", "更新日", "サイズ", "なし" };
	CONST char * CONST o_n[] = { "昇順", "降順" };
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_ARRANGE	o;
	UB		txt[16];
	BOOL		one = (BOOL)( d->npick < 2 );
	INT		pid, y1, y2, y3, n;
	UINT		ans;

	if ( d->npick == 0 || d->sealed ) {
		return;
	}
	def = dt_pn_new(640, 320);
	if ( def == NULL ) {
		return;
	}
	n = d->npick;
	txt[0] = 0;
	{
		CONST char	*pre = "選択: ";
		INT		k = 0, j = 0, v = n;
		char		num[8];

		while ( pre[k] != 0 ) {
			txt[k] = (UB)pre[k];
			k++;
		}
		do {
			num[j++] = (char)( '0' + v % 10 );
			v /= 10;
		} while ( v > 0 && j < 7 );
		while ( j > 0 ) {
			txt[k++] = (UB)num[--j];
		}
		txt[k++] = 0xE5;  txt[k++] = 0x80;  txt[k++] = 0x8B;	/* 個 */
		txt[k] = 0;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 8, 200, 20, (CONST char *)txt);

	y1 = ar_group(def, AR_G_H, 14, 34, "横", h_n, 3, one, 2);
	y1 = ar_group(def, AR_G_V, 14, y1, "縦", v_n, 3, one, 2);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, y1, 70, 20, "段組数");
	pt = dt_pn_add(def, WM_PT_NUMBOX | ( one ? P_DISABLE : 0 ), AR_P_COUNT,
		    84, y1 - 2, 60, 24, NULL);
	if ( pt != NULL ) {
		pt->text[0] = '1';
		pt->text[1] = 0;
		pt->caret = 1;
		pt->value = 1;
	}
	y2 = ar_group(def, AR_G_COL, 220, 34, "段組", c_n, 4, one, 3);
	(void)ar_group(def, AR_G_W, 220, y2, "幅調整", w_n, 6, FALSE, 5);
	y3 = ar_group(def, AR_G_SORT, 430, 34, "整列順", s_n, 5, one, 4);
	(void)ar_group(def, AR_G_ORDER, 430, y3, "順序", o_n, 2, FALSE, 0);
	/* 昇順 and 降順 are a pair: neither is "none" */
	if ( one ) {
		def->part[def->npart - 2].type |= P_DISABLE;
		def->part[def->npart - 1].type |= P_DISABLE;
	}
	dt_pn_buttons(def, "OK");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		CONST INT	hmap[] = { AR_LEFT, AR_RIGHT, AR_NONE };
		CONST INT	vmap[] = { AR_COMPACT, AR_ALIGN, AR_NONE };
		CONST INT	cmap[] = { AR_SINGLE, AR_MULTI_H, AR_MULTI_V, AR_NONE };
		CONST INT	wmap[] = { AR_W_FIRST, AR_W_FULL, AR_W_NAME,
					   AR_W_RELATION, AR_W_NODATE, AR_NONE };
		CONST INT	smap[] = { AR_BY_NAME, AR_BY_MADE, AR_BY_UPDATED,
					   AR_BY_SIZE, AR_NONE };
		INT		cols = 1;

		wm_panel_get(pid, AR_P_COUNT, &cols);
		o.horizontal = hmap[ar_chosen(pid, AR_G_H, 3)];
		o.vertical = vmap[ar_chosen(pid, AR_G_V, 3)];
		o.column = cmap[ar_chosen(pid, AR_G_COL, 4)];
		o.width = wmap[ar_chosen(pid, AR_G_W, 6)];
		o.sort_by = smap[ar_chosen(pid, AR_G_SORT, 5)];
		o.descending = (BOOL)( ar_chosen(pid, AR_G_ORDER, 2) == 1 );
		o.columns = ( cols > 0 ) ? cols : 1;
		dt_pn_done(d, pid);
		ed_arrange(d, &o);
		return;
	}
	dt_pn_done(d, pid);
}

/* ---------------------------------------------------------------- the items */

EXPORT void dt_cab_command( DTWIN *d, INT cmd, BOOL on_vobj, CONST T_VOBJ *v )
{
	(void)on_vobj;
	(void)v;
	switch ( cmd ) {
	case CM_SAVE:		dt_save(d);			break;
	case CM_SAVE_AS_NEW:	save_as_new(d);			break;
	case CM_FULLSCREEN:	dt_fullscreen(d);		break;
	case CM_REFRESH:	dt_draw_all();			break;
	case CM_BGCOLOUR:	bg_colour(d);			break;
	case CM_SHOW_HIDDEN:
		d->show_hidden = (BOOL)!d->show_hidden;
		ed_model(d);
		dt_draw(d);
		break;
	case CM_UNDO:		ed_undo(d);			break;
	case CM_COPY:		ed_copy(d);			break;
	case CM_PASTE:		ed_paste(d, FALSE);		break;
	case CM_CUT:		ed_cut(d);			break;
	case CM_MOVE_BACK:	ed_paste(d, TRUE);		break;
	case CM_DELETE:		ed_delete(d);			break;
	case CM_FRONT:		ed_front(d);			break;
	case CM_BACK:		ed_back(d);			break;
	case CM_ARRANGE:	arrange(d);			break;
	case CM_FIX:		ed_protect(d, FALSE, TRUE);	break;
	case CM_UNFIX:		ed_protect(d, FALSE, FALSE);	break;
	case CM_BG:		ed_protect(d, TRUE, TRUE);	break;
	case CM_UNBG:		ed_protect(d, TRUE, FALSE);	break;
	default:		break;
	}
	wm_update();
}
