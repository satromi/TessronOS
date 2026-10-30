/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_km.c
 *	キー配列 (design 11.15): the layouts and the keys given others, the
 *	window that never has the keys, and the screen keyboard
 *
 *	The calls a process reads and sets the layouts with (km_get,
 *	km_set, km_label) are worked in the kernel: the latin layout and
 *	the kana layout set and kept as lines of the machine's settings, a
 *	key given another and given its own back, what is wrong refused with
 *	nothing set, and the words keys are written with. A window that
 *	never has the keys (WM_ATTR_NOFOCUS) is not given them, nor are they
 *	handed to it when the window that had them closes. Such a window is
 *	told every key of the keyboard when it asks (OB_N_KEYSEEN), and no
 *	other window or request is; a key it types (wm_obj_key_put) is taken
 *	only after a press on it, and is put in as the keyboard would.
 *
 *	Then the program is started. Its window does not take the keys; a
 *	press on the A of its picture types A into the window that had them,
 *	which keeps them. 付け替え and a key, then another, gives the first
 *	the second's number, kept in the machine's settings, and its × gives
 *	it back; the latin layout's button sets the layout. The menu of the
 *	second button opens over the window, and its first item, 閉じる,
 *	ends the program. Last the program is started with "selftest", which
 *	checks the same calls from a process and answers 0.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/keymap.h>
#include <ts/conf.h>
#include "../../outer_kernel/wm/wmobj.h"

#define RUN_TMO		60000

/* The program's window, as km_main.c lays it: places in its work area */
#define KB_Q		10
#define KB_M		8
#define KB_TOP		66
#define KB_RH		40
#define KB_LIST		( KB_TOP + 5 * KB_RH + 8 )		/* where panel 2 of SYSDEF_BOX_KEYMAP is */
#define AT_A_X		( KB_M + 7 * KB_Q + 20 )		/* the A of the Japanese keyboard */
#define AT_A_Y		( KB_TOP + 2 * KB_RH + 20 )
#define AT_B_X		( KB_M + 25 * KB_Q + 20 )		/* and the B */
#define AT_B_Y		( KB_TOP + 3 * KB_RH + 20 )
/* panel 1, along the top (etc/def, the data box SYSDEF_BOX_KEYMAP) */
#define AT_LATIN_X	146					/* the latin layout's pop-up, 52,6,240,32 */
#define AT_LATIN_Y	19
#define AT_POP_ROW	21					/* a row of a pop-up's list */
#define AT_PICK_X	222					/* the switch 付け替え, 168,39,276,63 */
#define AT_PICK_Y	51
/* panel 2, under the keyboard */
#define AT_ROW1_X	144					/* the first row of the list, 8,24,480,120 */
#define AT_ROW1_Y	( KB_LIST + 37 )
#define AT_BACK_X	550					/* 〈元に戻す〉, 492,24,608,50 */
#define AT_BACK_Y	( KB_LIST + 37 )
#define AT_MENU_X	300					/* panel 1, right of the switch: room below for the menu */
#define AT_MENU_Y	50

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same( CONST char *a, CONST char *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == *b );
}

/* The line of the machine's settings, compared */
LOCAL BOOL conf_is( CONST char *key, CONST char *want )
{
	char	v[CF_VAL_MAX];
	INT	i;

	/* the program writes the settings after the kernel takes the map: a moment later */
	for ( i = 0; i < 60; i++ ) {
		if ( !wm_dev_conf_get(key, v, sizeof(v)) ) {
			v[0] = 0;
		}
		if ( same(v, want) ) {
			break;
		}
		tk_dly_tsk(50);
	}
	if ( !same(v, want) ) {
		tm_printf((UB *)"  %s is \"%s\", not \"%s\"\n", key, v, want);
		return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the settings */

LOCAL void test_tables( void )
{
	T_KMSTATE	was, st, bad;
	char		b[64];

	KT_ASSERT_ER(km_get(&was), E_OK);
	KT_ASSERT(was.latin >= 0 && was.latin < KM_LATIN_N);
	KT_ASSERT(was.kana >= 0 && was.kana < KM_KANA_N);

	/* Dvorak and 親指シフト: set, read back, kept */
	st = was;
	st.latin = KM_LATIN_DVORAK;
	st.kana = KM_KANA_NICOLA;
	KT_ASSERT_ER(km_set(&st, KM_SET_LATIN | KM_SET_KANA), E_OK);
	KT_ASSERT_EQ(km_latin(), KM_LATIN_DVORAK);
	KT_ASSERT_EQ(km_kana(), KM_KANA_NICOLA);
	KT_ASSERT(conf_is("KBLAYOUT", "1"));
	KT_ASSERT(conf_is("KBKANA", "2"));
	KT_ASSERT_EQ(km_label(KM_L_CHAR, 0x14, 0, b, sizeof(b)), 1);	/* Q's place types ' in Dvorak */
	KT_ASSERT(same(b, "'"));
	KT_ASSERT(km_label(KM_L_KANA, 0x04, 0, b, sizeof(b)) > 0 && same(b, "う"));
	KT_ASSERT(km_label(KM_L_KANA, 0x04, KM_MOD_THUMB_L, b, sizeof(b)) > 0 && same(b, "を"));
	KT_ASSERT(km_label(KM_L_KANA, 0x04, KM_MOD_THUMB_R, b, sizeof(b)) > 0 && same(b, "ヴ"));

	/* CapsLock made the left Ctrl, then every key given back */
	st.nmap = 1;
	st.from[0] = 0x39;
	st.to[0] = 0xE0;
	KT_ASSERT_ER(km_set(&st, KM_SET_MAP), E_OK);
	KT_ASSERT_EQ(km_remap(0x39), 0xE0);
	KT_ASSERT_EQ(km_mods_away(), 0);
	KT_ASSERT(conf_is("KBMAP", "39E0"));
	KT_ASSERT_ER(km_get(&bad), E_OK);
	KT_ASSERT(bad.nmap == 1 && bad.from[0] == 0x39 && bad.to[0] == 0xE0);

	/* what is wrong: nothing of it set */
	bad = st;
	bad.kana = KM_KANA_N;
	bad.nmap = 0;
	KT_ASSERT_ER(km_set(&bad, KM_SET_KANA | KM_SET_MAP), E_PAR);
	bad = st;
	bad.nmap = KM_MAP_MAX + 1;
	KT_ASSERT_ER(km_set(&bad, KM_SET_MAP), E_PAR);
	bad = st;
	bad.to[0] = 0;
	KT_ASSERT_ER(km_set(&bad, KM_SET_MAP), E_PAR);
	KT_ASSERT_ER(km_set(&st, 0x80), E_PAR);
	KT_ASSERT_ER(km_set(NULL, KM_SET_MAP), E_PAR);
	KT_ASSERT_EQ(km_kana(), KM_KANA_NICOLA);
	KT_ASSERT_EQ(km_remap(0x39), 0xE0);

	st.nmap = 0;
	KT_ASSERT_ER(km_set(&st, KM_SET_MAP), E_OK);
	KT_ASSERT_EQ(km_remap(0x39), 0x39);
	KT_ASSERT(conf_is("KBMAP", ""));

	/* the words of the keys, in JIS かな */
	st.kana = KM_KANA_JIS;
	KT_ASSERT_ER(km_set(&st, KM_SET_KANA), E_OK);
	KT_ASSERT_EQ(km_label(KM_L_NAME, 0x2A, 0, b, sizeof(b)), 2);
	KT_ASSERT(same(b, "BS"));
	KT_ASSERT(km_label(KM_L_NAME, 0xE1, 0, b, sizeof(b)) > 0 && same(b, "左Shift"));
	KT_ASSERT_EQ(km_label(KM_L_NAME, 0xE1, 0, b, 4), 3);	/* 左 whole, Shift cut off */
	KT_ASSERT(same(b, "左"));
	KT_ASSERT_EQ(km_label(KM_L_NAME, 0xE1, 0, b, 3), 0);	/* not half a letter */
	KT_ASSERT(km_label(KM_L_JIS, 0x04, 0, b, sizeof(b)) > 0 && same(b, "ち"));
	KT_ASSERT(km_label(KM_L_JIS, 0x20, HID_MOD_RSHIFT, b, sizeof(b)) > 0 && same(b, "ぁ"));
	KT_ASSERT_EQ(km_label(KM_L_KANA, 0x04, 0, b, sizeof(b)), 0);	/* JIS かな: none by the thumbs */
	KT_ASSERT_EQ(km_label(7, 0x04, 0, b, sizeof(b)), E_PAR);
	KT_ASSERT_EQ(km_label(KM_L_NAME, 0x100, 0, b, sizeof(b)), E_PAR);

	/* as it was */
	KT_ASSERT_ER(km_set(&was, KM_SET_LATIN | KM_SET_KANA | KM_SET_MAP), E_OK);
}

/* ---------------------------------------------------------------- the window */

LOCAL void test_nofocus( void )
{
	T_DPRECT	o;
	T_WMWIN		w;
	INT		a, k, was = wm_focused();

	o.left = 60;  o.top = 60;  o.right = 360;  o.bottom = 260;
	a = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "km a");
	o.left = 400;  o.right = 700;
	k = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_NOFOCUS, "km keys");
	KT_ASSERT(a > 0 && k > 0);
	if ( a <= 0 || k <= 0 ) goto out;
	KT_ASSERT_ER(wm_ref(k, &w), E_OK);
	KT_ASSERT(( w.attr & WM_ATTR_NOFOCUS ) != 0);

	/* the keys stay where they were */
	KT_ASSERT_ER(wm_focus(a), E_OK);
	KT_ASSERT_ER(wm_focus(k), E_OBJ);
	KT_ASSERT_EQ(wm_focused(), a);
	KT_ASSERT_ER(wm_raise(k), E_OK);
	KT_ASSERT_EQ(wm_focused(), a);

	/* and are not handed to it when the window that had them closes */
	KT_ASSERT_ER(wm_close(a), E_OK);
	a = 0;
	KT_ASSERT(wm_focused() != k);
	KT_ASSERT_EQ(wm_self_check(), 0);
out:
	if ( a > 0 ) (void)wm_close(a);
	if ( k > 0 ) (void)wm_close(k);
	if ( was > 0 ) (void)wm_focus(was);
}

LOCAL ID port_open( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, u) < E_OK ) return E_SYS;
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

LOCAL ER take( ID port, T_OBNTM *m )
{
	SZ	asz = 0;
	ER	er = ob_rea_rec(port, 0, 0, m, sizeof(*m), &asz);

	return ( er >= E_OK && asz != (SZ)sizeof(*m) ) ? E_OBJ : er;
}

/* A window made as an object, and its number */
LOCAL ER win_make( CONST char *json, TS_UUID *u, ID *p_key, INT *p_wid )
{
	T_OBCRE	c;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"km window";
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	*p_key = ob_opn_obj(u, OB_OP_ALL);
	*p_wid = wm_obj_wid(u);
	return ( *p_key > 0 && *p_wid > 0 ) ? E_OK : E_OBJ;
}

LOCAL void press_on( INT wid )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wid;
	ev.x = 10;
	ev.y = 10;
	knl_wmobj_event(&ev);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
}

LOCAL void test_seen_put( void )
{
	TS_UUID		nw, pw, c1, c2, c3;
	ID		nk = 0, pk = 0, p1, p2, p3;
	INT		nwid = 0, pwid = 0, n;
	T_OBNTF		req;
	T_OBNTM		m;
	T_WMEV		ev;
	T_HIDEV		tap[8];
	UD		pos;
	CONST char	*nj = "{\"rect\":[100,100,400,300],\"attr\":515}";
	CONST char	*pj = "{\"rect\":[450,100,750,300],\"attr\":3}";

	knl_memset(&nw, 0, sizeof(nw));
	knl_memset(&pw, 0, sizeof(pw));
	p1 = port_open(&c1);
	p2 = port_open(&c2);
	p3 = port_open(&c3);
	KT_ASSERT(p1 > 0 && p2 > 0 && p3 > 0);
	KT_ASSERT_ER(win_make(nj, &nw, &nk, &nwid), E_OK);
	KT_ASSERT_ER(win_make(pj, &pw, &pk, &pwid), E_OK);
	if ( p1 <= 0 || p2 <= 0 || p3 <= 0 || nk <= 0 || pk <= 0 ) goto out;

	/* every key to the one that asked; the other request, and a plain window, hear none */
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_KEY;
	req.id = 1;
	req.flags = OB_N_KEYSEEN;
	KT_ASSERT(ob_ntf_evt(nk, OB_REC_ANY, &req, p1) > 0);
	req.id = 2;
	req.flags = OB_N_KEYUP;
	KT_ASSERT(ob_ntf_evt(nk, OB_REC_ANY, &req, p2) > 0);
	req.id = 3;
	req.flags = OB_N_KEYUP | OB_N_KEYSEEN;
	KT_ASSERT(ob_ntf_evt(pk, OB_REC_ANY, &req, p3) > 0);

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.code = 0x04;
	ev.mods = HID_MOD_LSHIFT;
	knl_wmobj_key_seen(&ev);
	ev.type = HID_EV_KEY_UP;
	knl_wmobj_key_seen(&ev);
	KT_ASSERT_ER(take(p1, &m), E_OK);
	KT_ASSERT(m.event == OB_E_KEY && m.code == 0x04 && m.mods == ( HID_MOD_LSHIFT | OB_KEY_SEEN ));
	KT_ASSERT_ER(take(p1, &m), E_OK);
	KT_ASSERT(m.event == OB_E_KEY && m.mods == ( HID_MOD_LSHIFT | OB_KEY_SEEN | OB_KEY_UP ));
	KT_ASSERT_ER(take(p1, &m), E_TMOUT);
	KT_ASSERT_ER(take(p2, &m), E_TMOUT);
	KT_ASSERT_ER(take(p3, &m), E_TMOUT);

	/* a key typed by the window: not before a press on it, not by a plain one */
	press_on(pwid);
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0x04, 0), E_OACV);
	press_on(nwid);
	KT_ASSERT_ER(wm_obj_key_put(pwid, 0x04, 0), E_OBJ);
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0, 0), E_PAR);
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0x100, 0), E_PAR);
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0x04, OB_KEY_UP), E_PAR);
	KT_ASSERT_ER(wm_obj_key_put(0, 0x04, 0), E_ID);

	/* and put in as the keyboard would: pressed and let go, with what is held */
	pos = knl_hid_tap_now();
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0x05, HID_MOD_RSHIFT | KM_MOD_THUMB_L), E_OK);
	n = knl_hid_tap(&pos, 0, tap, 8, NULL);
	KT_ASSERT_EQ(n, 2);
	if ( n == 2 ) {
		KT_ASSERT(tap[0].type == HID_EV_KEY_DOWN && tap[0].code == 0x05
		       && tap[0].mods == ( HID_MOD_RSHIFT | KM_MOD_THUMB_L ));
		KT_ASSERT(tap[1].type == HID_EV_KEY_UP && tap[1].code == 0x05);
	}
	/* long after the press: no more */
	tk_dly_tsk(WM_KEY_PUT_MS + 300);
	KT_ASSERT_ER(wm_obj_key_put(nwid, 0x04, 0), E_OACV);
	{
		T_WMEV	drain;

		while ( wm_read_event(&drain, 30) >= E_OK ) ;	/* what was put in, taken by nobody */
	}
out:
	if ( p1 > 0 ) ob_cls_obj(p1);
	if ( p2 > 0 ) ob_cls_obj(p2);
	if ( p3 > 0 ) ob_cls_obj(p3);
	(void)ob_del_obj(&c1);
	(void)ob_del_obj(&c2);
	(void)ob_del_obj(&c3);
	if ( nk > 0 ) ob_cls_obj(nk);
	if ( pk > 0 ) ob_cls_obj(pk);
	(void)ob_del_obj(&nw);
	(void)ob_del_obj(&pw);
}

/* ---------------------------------------------------------------- the program */

LOCAL INT win_named( CONST char *title, INT ms )
{
	INT	wid, k;
	char	t[WM_TITLE_MAX];

	for ( ; ms > 0; ms -= 100 ) {
		for ( wid = 1; wid <= knl_wm_nwin(); wid++ ) {
			if ( wm_title(wid, t, sizeof(t)) < 0 ) continue;
			for ( k = 0; title[k] != 0 && t[k] == title[k]; k++ ) ;
			if ( title[k] == 0 && t[k] == 0 ) return wid;
		}
		tk_dly_tsk(100);
	}
	return 0;
}

/*
 * The input read as the desktop reads it, for ms: what comes is
 * addressed to its window and told. The first key pressed for window
 * want is answered, 0 none.
 */
LOCAL UINT pump( INT ms, INT want )
{
	T_WMEV	ev;
	UINT	got = 0;

	for ( ; ms > 0; ms -= 30 ) {
		while ( wm_read_event(&ev, 30) >= E_OK ) {
			if ( got == 0 && ev.type == HID_EV_KEY_DOWN && ev.wid == want ) got = ev.code;
		}
	}
	return got;
}

/* A press of a button at a place of the window's work area, through the input */
LOCAL void click( INT wid, INT x, INT y, UINT button )
{
	T_WMWIN	w;
	T_HIDEV	h;

	if ( wm_ref(wid, &w) < E_OK ) return;
	knl_memset(&h, 0, sizeof(h));
	h.type = HID_EV_MOVE;
	h.x = w.work.left + x;
	h.y = w.work.top + y;
	(void)kt_inject(&h);
	h.type = HID_EV_BTN_DOWN;
	h.code = button;
	(void)kt_inject(&h);
	h.type = HID_EV_BTN_UP;
	(void)kt_inject(&h);
}

/* The keys given others, waited for until from is given to (to 0: given none) */
LOCAL BOOL map_is( UB from, UB to, INT ms )
{
	UB	f[KM_MAP_MAX], t[KM_MAP_MAX];
	INT	n, i;

	for ( ; ms > 0; ms -= 100 ) {
		n = km_map_get(f, t, KM_MAP_MAX);
		for ( i = 0; i < n && f[i] != from; i++ ) ;
		if ( to == 0 ? ( i == n ) : ( i < n && t[i] == to ) ) return TRUE;
		(void)pump(100, -1);
	}
	return FALSE;
}

LOCAL INT popups( void )
{
	T_WMWIN	w;
	INT	wid, n = 0;

	for ( wid = 1; wid <= knl_wm_nwin(); wid++ ) {
		if ( wm_ref(wid, &w) >= E_OK && w.visible && ( w.attr & WM_ATTR_POPUP ) != 0 ) n++;
	}
	return n;
}

LOCAL void test_program( void )
{
	TS_UUID		prog, pu, tw, tc;
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBNTM		m;
	T_PSTS		psts;
	T_WMWIN		w;
	T_KMSTATE	was;
	ID		pid = 0, tk = 0, tp;
	INT		wid, twid = 0, latin, before, i;
	BOOL		typed = FALSE;
	ER		er = E_OK;

	KT_ASSERT_ER(km_get(&was), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_KEYMAP, &prog), E_OK);
	knl_memset(&tw, 0, sizeof(tw));
	tp = port_open(&tc);
	KT_ASSERT(tp > 0);

	/* the window the keys go to, told of them */
	KT_ASSERT_ER(win_make("{\"rect\":[60,560,460,700],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 || tp <= 0 ) goto out;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_KEY;
	req.id = 9;
	KT_ASSERT(ob_ntf_evt(tk, OB_REC_ANY, &req, tp) > 0);
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	(void)pump(100, -1);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of キー配列");
	}
	pid = knl_prc_of_uuid(&pu);
	wid = win_named("キー配列", 20000);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto stop;
	tk_dly_tsk(800);			/* sized to the keyboard and drawn */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT(( w.attr & WM_ATTR_NOFOCUS ) != 0);
	KT_ASSERT_EQ(w.work.right - w.work.left, KB_M * 2 + 60 * KB_Q);
	(void)kt_shot("/boot/KEYMAP0.PPM");

	/* the A pressed on the picture: typed into the window that had the keys, which keeps them */
	click(wid, AT_A_X, AT_A_Y, 0);
	for ( i = 0; i < 30 && !typed; i++ ) {
		typed = (BOOL)( pump(100, twid) == 0x04 );
	}
	KT_ASSERT(typed);
	KT_ASSERT_EQ(wm_focused(), twid);
	for ( i = 0; take(tp, &m) >= E_OK; ) {
		if ( m.event == OB_E_KEY && m.code == 0x04 ) i++;
	}
	KT_ASSERT_EQ(i, 1);

	/* 付け替え (the switch): A becomes B, kept; chosen in the list under the keyboard, 〈元に戻す〉 gives it back */
	click(wid, AT_PICK_X, AT_PICK_Y, 0);
	(void)pump(400, -1);
	click(wid, AT_A_X, AT_A_Y, 0);
	(void)pump(400, -1);
	click(wid, AT_B_X, AT_B_Y, 0);
	KT_ASSERT(map_is(0x04, 0x05, 3000));
	KT_ASSERT(conf_is("KBMAP", "0405"));
	(void)pump(300, -1);
	(void)kt_shot("/boot/KEYMAP1.PPM");
	click(wid, AT_ROW1_X, AT_ROW1_Y, 0);
	(void)pump(300, -1);
	click(wid, AT_BACK_X, AT_BACK_Y, 0);
	KT_ASSERT(map_is(0x04, 0, 3000));
	KT_ASSERT(conf_is("KBMAP", ""));

	/* the latin layout's pop-up: its list opens with the layout in use under the pointer; the next, kept, then back */
	latin = km_latin();
	click(wid, AT_LATIN_X, AT_LATIN_Y, 0);
	(void)pump(600, -1);
	click(wid, AT_LATIN_X, AT_LATIN_Y + ( ( latin + 1 ) % KM_LATIN_N - latin ) * AT_POP_ROW, 0);
	for ( i = 0; i < 30 && km_latin() == latin; i++ ) (void)pump(100, -1);
	KT_ASSERT_EQ(km_latin(), ( latin + 1 ) % KM_LATIN_N);
	click(wid, AT_LATIN_X, AT_LATIN_Y, 0);
	(void)pump(600, -1);
	click(wid, AT_LATIN_X, AT_LATIN_Y + ( latin - ( latin + 1 ) % KM_LATIN_N ) * AT_POP_ROW, 0);
	for ( i = 0; i < 30 && km_latin() != latin; i++ ) (void)pump(100, -1);
	KT_ASSERT_EQ(km_latin(), latin);

	/* the menu of the second button, opened with 閉じる under the pointer, which ends it */
	before = popups();
	click(wid, AT_MENU_X, AT_MENU_Y, 1);
	for ( i = 0; i < 30 && popups() == before; i++ ) (void)pump(100, -1);
	KT_ASSERT(popups() > before);
	(void)pump(500, -1);
	(void)kt_shot("/boot/KEYMAP2.PPM");
	click(wid, AT_MENU_X, AT_MENU_Y, 0);
	(void)pump(500, -1);
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 10000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named("キー配列", 300), 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	(void)pump(100, -1);
	(void)km_set(&was, KM_SET_LATIN | KM_SET_KANA | KM_SET_MAP);
	if ( tp > 0 ) ob_cls_obj(tp);
	(void)ob_del_obj(&tc);
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

/* The program started with "selftest": the calls from a process */
LOCAL void test_selftest( void )
{
	TS_UUID		prog, pu;
	T_OBCRE		c;
	T_PSTS		psts;
	ID		pid;
	ER		er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_KEYMAP, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of キー配列");
	}
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_km( void )
{
	tm_printf((UB *)"\n[km] キー配列 (design 11.15)\n");
	KT_RUN(test_tables);
	KT_RUN_SCREEN(test_nofocus);
	KT_RUN_SCREEN(test_seen_put);
	KT_RUN_SCREEN(test_program);
	KT_RUN_SCREEN(test_selftest);
}
