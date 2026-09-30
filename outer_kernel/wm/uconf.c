/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uconf.c
 *	The person's settings, as ユーザ環境設定 left them (design 16.5.23)
 *
 *	The user information object (SYSDEF_CONF_USER) keeps in its record 1
 *	a line for each thing the accessory sets (lib/libconf's table): the
 *	times, sizes and switches of the pointer, the keyboard, the screen
 *	and the sound, which are entries of the look table, and the name of
 *	the person who uses the machine. They are taken once the store is
 *	there; the look table's entries are what the window layer, the
 *	keyboard and the pointer read as they go, so nothing else is told.
 *	The name is taken at the start only: a name changed is the machine's
 *	from its next start, as the accessory says.
 *
 *	The machine's settings (SYSDEF_CONF_DEV) are taken here too: the
 *	colour scheme, the wallpaper, and the keyboard's kind and the mode
 *	it starts in. The network's are the network layer's (so_conf.c).
 *
 *	All three objects are watched once taken: a program that writes one
 *	of them -- a settings accessory -- has it taken again, as a notice
 *	of the change (OB_E_CHANGE) reaches a task of this file. Nobody asks
 *	the system to take them; writing them is enough.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/so.h>
#include <ts/xf.h>
#include "obj/obj.h"

LOCAL UB	user_name[CF_NAME_MAX * 4 + 1];	/* UTF-8, up to twelve letters */
LOCAL BOOL	user_named = FALSE;

/* A settings object's record, into a buffer of CF_TEXT_MAX; its length or an error */
LOCAL INT conf_text_of( CONST char *uuid, UB *buf )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ts_str_to_uuid(uuid, &u) < E_OK ) {
		return E_SYS;
	}
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) {
		return (INT)key;
	}
	er = ob_rea_rec(key, 1, 0, buf, CF_TEXT_MAX, &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) {
		return (INT)er;
	}
	return (INT)( ( asz > CF_TEXT_MAX ) ? CF_TEXT_MAX : asz );
}

/*
 * The keyboard's and the pointer's attributes as the look table has
 * them, given to the device layer, which takes every report through
 * them: called whenever one of their entries is written. They are
 * written to the input object 入力 (OB_IN_ATTR), as whoever set the
 * entry -- the kernel, or the administrator's program that asked --
 * and directly only before the object layer is up.
 */
EXPORT void knl_input_sync( void )
{
	T_HIDATTR	a;
	ID		key;

	knl_memset(&a, 0, sizeof(a));
	a.pd_on = (UINT)wm_num(LK_PD_ON, 0);
	a.pd_off = (UINT)wm_num(LK_PD_OFF, 0);
	a.speed = (UINT)wm_num(LK_PD_SPEED, 12);
	a.accel = (UINT)wm_num(LK_PD_ACCEL, 7);
	a.main = (UINT)wm_num(LK_PD_MAIN, 0);
	a.absolute = (UINT)wm_num(LK_PD_ABS, 1);
	a.middbl = (UINT)wm_num(LK_PD_MIDDBL, 1);
	a.key_on = (UINT)wm_num(LK_KEY_ON, 0);
	a.key_off = (UINT)wm_num(LK_KEY_OFF, 0);
	a.key_sim = (UINT)wm_num(LK_KEY_SIM, 0);
	a.krp = (UINT)wm_num(LK_KRP, 1);
	a.krp_start = (UINT)wm_num(LK_KRP_START, 800);
	a.krp_int = (UINT)wm_num(LK_KRP_INT, 100);
	a.sclk = (UINT)wm_num(LK_SCLK, 0);
	a.tshift = (UINT)wm_num(LK_TSHIFT, 0);
	key = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE);
	if ( key > 0 ) {
		(void)ob_wri_rec(key, OB_IN_ATTR, 0, &a, sizeof(a), NULL);
		(void)ob_cls_obj(key);
	} else if ( key == E_NOEXS ) {
		(void)ts_hid_setattr(&a);	/* before there are objects */
	}
}

EXPORT ER wm_conf_apply( void )
{
	UB	*text;
	char	v[CF_VAL_MAX];
	INT	len, i, n, got = 0;

	text = (UB *)Kmalloc(CF_TEXT_MAX);
	if ( text == NULL ) {
		return E_NOMEM;
	}
	len = conf_text_of(SYSDEF_CONF_USER, text);
	if ( len < 0 ) {
		Kfree(text);
		return (ER)len;
	}
	for ( i = 0; i < cf_user_nitem; i++ ) {
		CONST CF_ITEM	*it = &cf_user_items[i];

		if ( !cf_get(text, len, it->key, 0, v, sizeof(v)) ) {
			continue;
		}
		n = cf_num(v, it->dflt);
		if ( n < it->lo ) n = it->lo;
		if ( n > it->hi ) n = it->hi;
		(void)knl_look_setting(it->look, (UW)n);
		got++;
	}
	if ( !user_named && cf_get(text, len, CF_USER_NAME, 0, v, sizeof(v)) ) {
		for ( i = 0; v[i] != 0 && i < (INT)sizeof(user_name) - 1; i++ ) {
			user_name[i] = (UB)v[i];
		}
		user_name[i] = 0;
		user_named = TRUE;
	}
	Kfree(text);
	knl_input_sync();
	tm_printf((UB *)"user: %d settings taken, the user is \"%s\"\n", got, user_name);
	return E_OK;
}

/*
 * The machine's settings: SCHEME n (the colour scheme), WALLPAPER uuid
 * [mode] (a picture of the wallpaper box), KBTYPE 0x41 or 0x40 (a
 * Japanese or an English keyboard) and KBMODE (the input mode the
 * desktop starts in: 0 かな, 1 英数). A line not there leaves what is.
 */
EXPORT ER wm_dev_conf_apply( void )
{
	UB	*text;
	char	v[CF_VAL_MAX], w[64];
	INT	len;

	text = (UB *)Kmalloc(CF_TEXT_MAX);
	if ( text == NULL ) {
		return E_NOMEM;
	}
	len = conf_text_of(SYSDEF_CONF_DEV, text);
	if ( len < 0 ) {
		Kfree(text);
		return (ER)len;
	}
	if ( cf_get(text, len, "SCHEME", 0, v, sizeof(v)) && v[0] != 0
	  && cf_num(v, -1) >= 0 && (UINT)cf_num(v, -1) != wm_scheme() ) {
		(void)wm_set_scheme((UINT)cf_num(v, 0));
	}
	if ( cf_get(text, len, "WALLPAPER", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) ) {
		TS_UUID	u;
		char	m[8];
		INT	mode = WM_WALL_FIT;

		if ( cf_word(v, 1, m, sizeof(m)) ) mode = cf_num(m, WM_WALL_FIT);
		if ( ts_str_to_uuid(w, &u) >= E_OK && wm_load_wall_obj(&u, xf_data_rec(&u), (UINT)mode) >= E_OK ) {
			knl_obsys_wall(&u, mode);
		}
	}
	if ( cf_get(text, len, "KBTYPE", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_US, ( cf_num(v, 0x41) == 0x40 ) ? 1 : 0);
	}
	if ( cf_get(text, len, "KBMODE", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_MODE, (UW)( cf_num(v, 1) & 3 ));
	}
	Kfree(text);
	return E_OK;
}

/* One line of the machine's settings written: the key's value, "" for the key alone */
EXPORT ER wm_dev_conf_put( CONST char *key, CONST char *val )
{
	TS_UUID	u;
	UB	*text;
	ID	k;
	SZ	asz = 0;
	INT	len;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_CONF_DEV, &u) < E_OK ) {
		return E_SYS;
	}
	text = (UB *)Kmalloc(CF_TEXT_MAX);
	if ( text == NULL ) {
		return E_NOMEM;
	}
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( k < E_OK ) {
		Kfree(text);
		return (ER)k;
	}
	er = ob_rea_rec(k, 1, 0, text, CF_TEXT_MAX, &asz);
	len = ( er >= E_OK ) ? (INT)asz : 0;
	len = cf_put(text, len, CF_TEXT_MAX, key, 0, val);
	er = ( len < 0 ) ? E_LIMIT : ob_wri_rec(k, 1, 0, text, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 1, (UD)len);
	ob_cls_obj(k);
	Kfree(text);
	return er;
}

/* A setting of the machine's read: the value's words into v; FALSE when there is no such line */
EXPORT BOOL wm_dev_conf_get( CONST char *key, char *v, INT max )
{
	UB	*text = (UB *)Kmalloc(CF_TEXT_MAX);
	INT	len;
	BOOL	got = FALSE;

	if ( text == NULL ) {
		return FALSE;
	}
	len = conf_text_of(SYSDEF_CONF_DEV, text);
	if ( len > 0 ) got = cf_get(text, len, key, 0, v, max);
	Kfree(text);
	return got;
}

/* ---------------------------------------------------------------- watched */

#define WATCH_PRI	30		/* low, and within 1..CNF_MAX_TSKPRI: beyond it the task is not made */

LOCAL ID	watch_tsk = 0;

LOCAL void conf_watch( INT stacd, void *exinf )
{
	CONST char *CONST which[3] = { SYSDEF_CONF_NET, SYSDEF_CONF_USER, SYSDEF_CONF_DEV };
	TS_UUID	u[3], ch;
	T_OBCRE	c;
	T_OBNTF	req;
	T_OBNTM	m;
	ID	port = 0, key[3];
	SZ	asz = 0;
	INT	i;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK || ( port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) ) <= 0 ) {
		watch_tsk = 0;
		tk_exd_tsk();
	}
	/*
	 * The network's settings, taken behind the desktop's start: the
	 * interface may have to ask the network for an address, which is
	 * not to hold up the first window. A machine run for its tests
	 * keeps the interface its tests set.
	 */
#if !USE_KTEST
	(void)so_conf(SO_CONF_APPLY);
#endif
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	for ( i = 0; i < 3; i++ ) {
		key[i] = 0;
		if ( ts_str_to_uuid(which[i], &u[i]) < E_OK ) continue;
		key[i] = ob_opn_obj(&u[i], OB_OP_ATRRD);
		if ( key[i] > 0 && ob_ntf_evt(key[i], 1, &req, port) <= 0 ) {
			(void)ob_cls_obj(key[i]);
			key[i] = 0;
		}
	}
	for ( ;; ) {
		BOOL	got[3] = { FALSE, FALSE, FALSE };

		if ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) < E_OK || asz == 0 ) {
			tk_dly_tsk(200);		/* nothing yet */
			continue;
		}
		/* a write and the cut after it are one change: what else comes soon is the same */
		for ( ;; ) {
			for ( i = 0; i < 3; i++ ) {
				if ( ts_uuid_cmp(&m.uuid, &u[i]) == 0 ) got[i] = TRUE;
			}
			tk_dly_tsk(100);
			if ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) < E_OK || asz == 0 ) break;
		}
#if !USE_KTEST
		if ( got[0] ) {
			tm_printf((UB *)"settings: the network's were written\n");
			(void)so_conf(SO_CONF_APPLY);
		}
#endif
		if ( got[1] ) {
			tm_printf((UB *)"settings: the person's were written\n");
			(void)wm_conf_apply();
		}
		if ( got[2] ) {
			tm_printf((UB *)"settings: the machine's were written\n");
			(void)wm_dev_conf_apply();
		}
	}
}

/* The three settings objects watched from now on */
EXPORT ER wm_conf_watch( void )
{
	T_CTSK	ct;

	if ( watch_tsk > 0 ) {
		return E_OK;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)conf_watch;
	ct.itskpri = WATCH_PRI;
	ct.stksz = 16 * 1024;
	watch_tsk = tk_cre_tsk(&ct);
	if ( watch_tsk <= 0 ) {
		tm_printf((UB *)"settings: the watcher could not be made (%d)\n", (INT)watch_tsk);
		return (ER)watch_tsk;
	}
	return tk_sta_tsk(watch_tsk, 0);
}

EXPORT INT wm_user_name( UB *buf, INT max )
{
	INT	i;

	if ( buf == NULL || max <= 0 ) {
		return 0;
	}
	for ( i = 0; user_name[i] != 0 && i < max - 1; i++ ) {
		buf[i] = user_name[i];
	}
	buf[i] = 0;
	return i;
}
