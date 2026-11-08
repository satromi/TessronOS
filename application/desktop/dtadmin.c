/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtadmin.c
 *	The administrator's rights asked for (DT_RQ_ADMIN of ts/dtreq.h,
 *	docs/tessronos/08-object.md 8.7)
 *
 *	A program the desktop started acts for the person at the machine,
 *	not as an administrator. When it is to do what only an
 *	administrator may -- set the clock, change the network's settings,
 *	open a disk -- it asks the desktop, and the desktop asks the person
 *	in a panel of its own for the administrator's password: the program
 *	never sees what is typed. Typed right, the program acts as an
 *	administrator for that operation (knl_ob_adm_give), until it gives
 *	the rights back or OB_ADM_MS has gone by.
 *
 *	A machine whose administrator has no password yet asks for one to
 *	be chosen, typed twice; it is the administrator's from then on, and
 *	the program is given the rights as though it had been typed. Only
 *	the first password is set so: a later one is changed by one who
 *	knows the old.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/dtreq.h>
#include "desktop.h"

#define AD_TRIES	3		/* a wrong password typed this many times: refused */
#define AD_PW_MAX	WM_LABEL_MAX
#define AD_W		520

/* "「name」が" for the asking process: the name of the program it runs */
LOCAL void who_asks( ID pid, UB *out, INT max )
{
	TS_UUID	prog;
	UB	name[TAD_NAME_MAX];
	INT	n = 0, i;

	name[0] = 0;
	if ( knl_prc_uuid(pid, NULL, &prog) < E_OK || om_store_name(&prog, name, sizeof(name)) <= 0 ) {
		dt_pn_text(name, sizeof(name), "プログラム");
	}
	for ( i = 0; "「"[i] != 0 && n < max - 1; i++ ) out[n++] = (UB)"「"[i];
	for ( i = 0; name[i] != 0 && n < max - 1; i++ ) out[n++] = name[i];
	for ( i = 0; "」が管理者の権限を求めています"[i] != 0 && n < max - 1; i++ ) {
		out[n++] = (UB)"」が管理者の権限を求めています"[i];
	}
	out[n] = 0;
}

LOCAL INT	admin_panels = 0;		/* how many have been shown, for the tests */

EXPORT INT dt_admin_panels( void )
{
	return admin_panels;
}

LOCAL void wipe( UB *s, INT n )
{
	volatile UB	*v = s;
	INT		i;

	for ( i = 0; i < n; i++ ) v[i] = 0;
}

/*
 * The password asked for once: TRUE with it in pw when the panel was
 * agreed to, FALSE when it was put away.
 */
LOCAL BOOL ask_password( DTWIN *host, CONST UB *who, CONST UB *what, UB *pw, INT max )
{
	T_WMPANEL	*def;
	INT		pid, top = 10;
	UINT		ans;

	def = dt_pn_new(AD_W, 196);
	if ( def == NULL ) {
		return FALSE;
	}
	def->kind = WM_PNL_WARN;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22, (CONST char *)who);
	top += 24;
	if ( what != NULL && what[0] != 0 ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22, (CONST char *)what);
	}
	top += 30;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22, "管理者のパスワードを入力してください");
	top += 26;
	dt_pn_add(def, WM_PT_SECRET, 1, 14, top, AD_W - 28, 26, NULL);
	dt_pn_buttons(def, "許可");

	admin_panels++;
	ans = dt_pn_run(host, def, &pid);
	pw[0] = 0;
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		wm_panel_text(pid, 1, pw, max);
	}
	dt_pn_done(host, pid);
	return (BOOL)( ans == WM_ANS_OK );
}

/*
 * The administrator's first password chosen: typed twice and the same.
 * E_OK when it was set, E_ABORT when the panel was put away.
 */
LOCAL ER choose_password( DTWIN *host, CONST UB *who )
{
	T_WMPANEL	*def;
	UB		*a, *b;
	INT		pid, top;
	UINT		ans;
	ER		er = E_ABORT;

	a = (UB *)Kmalloc(AD_PW_MAX * 2);
	if ( a == NULL ) {
		return E_NOMEM;
	}
	b = a + AD_PW_MAX;
	for ( ;; ) {
		def = dt_pn_new(AD_W, 254);
		if ( def == NULL ) {
			er = E_NOMEM;
			break;
		}
		def->kind = WM_PNL_WARN;
		top = 10;
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22, (CONST char *)who);
		top += 24;
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22,
			  "管理者のパスワードがまだ決まっていません。");
		top += 24;
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top, AD_W - 28, 22,
			  "この機械の管理者のパスワードを決めてください。");
		top += 32;
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top + 2, 170, 22, "新しいパスワード");
		dt_pn_add(def, WM_PT_SECRET, 1, 190, top, AD_W - 204, 26, NULL);
		top += 34;
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top + 2, 170, 22, "もう一度");
		dt_pn_add(def, WM_PT_SECRET, 2, 190, top, AD_W - 204, 26, NULL);
		dt_pn_buttons(def, "設定");

		admin_panels++;
		ans = dt_pn_run(host, def, &pid);
		a[0] = b[0] = 0;
		if ( ans == WM_ANS_OK && pid >= 0 ) {
			wm_panel_text(pid, 1, a, AD_PW_MAX);
			wm_panel_text(pid, 2, b, AD_PW_MAX);
		}
		dt_pn_done(host, pid);
		if ( ans != WM_ANS_OK ) {
			er = E_ABORT;
			break;
		}
		if ( a[0] == 0 ) {
			dt_tell(host, "パスワードが空です", "1文字以上のパスワードを決めてください");
			continue;
		}
		if ( knl_strcmp((CONST char *)a, (CONST char *)b) != 0 ) {
			dt_tell(host, "二つのパスワードが一致しません", "同じパスワードを二度入力してください");
			continue;
		}
		er = knl_ob_admin_set(a);
		if ( er < E_OK ) {
			dt_tell(host, "管理者のパスワードを設定できませんでした", NULL);
		} else {
			tm_printf((UB *)"desktop: the administrator's password was set\n");
		}
		break;
	}
	wipe(a, AD_PW_MAX * 2);
	Kfree(a);
	return er;
}

/*
 * The window of the desktop's the panel is put over. Never the asking
 * program's: a panel over a window holds the keys of that window, and
 * what is typed would go to the program's own parts as well.
 */
LOCAL DTWIN *admin_host( void )
{
	DTWIN	*d = dt_first_win();
	INT	i;

	for ( i = 0; d == NULL && i < DT_MAX_WIN; i++ ) {
		if ( dt_win_at(i) != NULL && dt_win_at(i)->used ) d = dt_win_at(i);
	}
	return d;
}

/*
 * The person asked, for process pid, about what (UTF-8, may be empty),
 * in a panel over a window of the desktop's: E_OK when the
 * administrator's password was typed right (or chosen, on a machine
 * that had none), E_ABORT when the panel was put away, E_OACV after
 * AD_TRIES wrong ones. The keys go back to the asking program's window
 * win (NULL: none said) when the panel is put away.
 */
LOCAL ER admin_ask( CONST UB *who, CONST UB *what, CONST TS_UUID *win, ID pid );

EXPORT ER dt_admin_ask( ID pid, CONST UB *what, CONST TS_UUID *win )
{
	UB	who[160];

	who_asks(pid, who, sizeof(who));
	return admin_ask(who, what, win, pid);
}

/*
 * The same for what the desktop does itself from its menus, 'name' being
 * the menu ("ディスク操作"): no process is given the rights.
 */
EXPORT ER dt_admin_self( CONST char *name, CONST UB *what )
{
	UB	who[160];
	INT	n = 0, i;

	for ( i = 0; "「"[i] != 0 && n < (INT)sizeof(who) - 1; i++ ) who[n++] = (UB)"「"[i];
	for ( i = 0; name[i] != 0 && n < (INT)sizeof(who) - 1; i++ ) who[n++] = (UB)name[i];
	for ( i = 0; "」には管理者の権限が要ります"[i] != 0 && n < (INT)sizeof(who) - 1; i++ ) {
		who[n++] = (UB)"」には管理者の権限が要ります"[i];
	}
	who[n] = 0;
	return admin_ask(who, what, NULL, 0);
}

LOCAL ER admin_ask( CONST UB *who, CONST UB *what, CONST TS_UUID *win, ID pid )
{
	DTWIN	*host = admin_host();
	UB	*pw;
	INT	t;
	ER	er = E_OACV;

	if ( host == NULL ) {
		return E_NOEXS;
	}
	pw = (UB *)Kmalloc(AD_PW_MAX);
	if ( pw == NULL ) {
		return E_NOMEM;
	}
	for ( t = 0; t < AD_TRIES; t++ ) {
		pw[0] = 0;
		/* a machine with no administrator's password yet: one is chosen first */
		if ( knl_ob_admin_check(NULL) == E_NOEXS ) {
			er = choose_password(host, who);
			break;
		}
		if ( !ask_password(host, who, what, pw, AD_PW_MAX) ) {
			er = E_ABORT;
			break;
		}
		er = knl_ob_admin_check(pw);
		wipe(pw, AD_PW_MAX);
		if ( er >= E_OK ) {
			break;
		}
		tm_printf((UB *)"desktop: the administrator's password typed wrong (%d)\n", t + 1);
		if ( t < AD_TRIES - 1 ) {
			dt_tell(host, "パスワードが違います", NULL);
		}
		er = E_OACV;
	}
	wipe(pw, AD_PW_MAX);
	Kfree(pw);
	if ( win != NULL && wm_obj_wid(win) > 0 ) {
		(void)wm_focus(wm_obj_wid(win));
		wm_update();
	}
	tm_printf((UB *)"desktop: the administrator's rights for process %d: %s\n", (INT)pid,
		  ( er >= E_OK ) ? "given" : ( er == E_ABORT ) ? "not asked" : "refused");
	return er;
}

/*
 * DT_RQ_ADMIN: the rights for the process that wrote the request. One
 * that has them already is answered E_OK without a question.
 */
EXPORT ER dt_admin_req( CONST T_DTREQ *rq, ID pid )
{
	UB	what[sizeof(rq->name) + 1];
	INT	i;
	ER	er;

	if ( pid <= 0 || knl_ob_pid_admin(pid) ) {
		return E_OK;
	}
	for ( i = 0; i < (INT)sizeof(rq->name) && rq->name[i] != 0; i++ ) what[i] = rq->name[i];
	what[i] = 0;
	er = dt_admin_ask(pid, what, &rq->win);
	if ( er >= E_OK ) {
		er = knl_ob_adm_give(pid, 0);
	}
	return er;
}
