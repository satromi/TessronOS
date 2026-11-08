/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_skb.c
 *	ソフトウェアキーボード (design 11.31): the screen keyboard, typed on
 *	with the pointer, in front of the windows worked in
 *
 *	The program is started in front of a window that has the keys, its
 *	layout as the user information object says. Its window never takes
 *	the keys, floats and can be sized. On the English letters' layout a
 *	press on the A of its picture types A into the window that had them,
 *	which keeps them; Shift pressed once holds for the next key only,
 *	pressed twice until pressed again. The input mode key sets 日本語 and
 *	then 英語 as the reader of the input works the key it types, so that
 *	the window the keys go to converts what follows as it would the
 *	keyboard's. Ctrl and the key written E sends what the keyboard
 *	sends for Ctrl and the key that types e, which the programs take as
 *	Ctrl+E in QWERTY and in Dvorak alike. Another window opened or brought forward comes up behind
 *	the keyboard, the keys stay where they were, a window's tool panel
 *	stays shown when the keyboard is brought forward, and between the
 *	keyboard and a picture-in-picture window the one brought forward
 *	last is in front. The window made larger, the keys grow with it. The
 *	menu of the second button opens, and its first item, 閉じる, ends the
 *	program.
 *
 *	Then the TRON keyboard, kept as the layout: its window opens to the
 *	keyboard's shape, the keys are where the outlines of the keyboard put
 *	them (the screen kept as a PPM to be laid over the drawing they were
 *	taken from), and each types the usage of its place: A, A with the red
 *	(left) Shift, with the blue (right), 1 and with Fn F1, 日本語 and 英語.
 *	Then the μTRON keyboard: its halves in two windows at the screen's
 *	left and right edges, both in front and never having the keys; the
 *	letters of its faces typed as the latin layout set types them (P, the
 *	red ^, the blue :), 1 with Fn F2, ら's place in 日本語 with kana typed,
 *	英語 and 日本語 from either half, and its menu's 閉じる closing both.
 *	The right half made larger, the left is made as large, each staying
 *	at its edge; opened again, both are where and as large as they were.
 *	Each layout's window is opened where that layout's was last, apart
 *	from the others', and a place off the screen is brought onto it.
 *	Last the program is started with "selftest", which checks its tables,
 *	the layout kept, the machine's layouts switched to Dvorak and TRON
 *	かな配列 with the TRON keyboard and put back with the English
 *	letters (a layout the person changed in between left as it is), and
 *	the calls from a process, and answers 0; the layouts are as they were
 *	after it.
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
#include <ts/kconv.h>
#include <ts/part.h>
#include <ts/conf.h>
#include "../../outer_kernel/wm/wmobj.h"

#define RUN_TMO		60000
#define SK_TITLE	"ソフトウェアキーボード"
#define SK_TITLE_L	"ソフトウェアキーボード(左手)"
#define SK_TITLE_R	"ソフトウェアキーボード(右手)"

/* The program's window, as sk_main.c lays it out: five rows of 60 quarters of a key, 4 left at each end */
#define SK_M		4
#define SK_SIDE		4
#define SK_W0		724
#define SK_H0		232
/* and the TRON keyboard: its plane, in tenths of a unit, as large as the work area lets it be */
#define TR_W0		760
#define TR_H0		310
#define TR_PW		8176
#define TR_PH		3254

LOCAL INT g_w = SK_W0, g_h = SK_H0;

/* The layouts the screen keyboard's keys carry: the English letters' own at first, and the TRON keyboards' */
#define OWN_QJ		KM_MOD_LAYOUT(KM_LATIN_KEYS, KM_KANA_JIS)
#define OWN_DT		KM_MOD_LAYOUT(KM_LATIN_DVORAK, KM_KANA_TRON)

LOCAL INT at_x( INT q2 )		/* q2: in halves of a quarter of a key, so that a key's middle is a whole number */
{
	return SK_M + ( q2 + 2 * SK_SIDE ) * ( g_w - 2 * SK_M ) / ( 120 + 4 * SK_SIDE );
}

LOCAL INT at_y( INT r2 )		/* r2: in halves of a row */
{
	return SK_M + r2 * ( g_h - 2 * SK_M ) / 10;
}

/* Keys' middles on both keyboards: (quarters x 2, rows x 2) */
#define Q_A		18		/* A: row 2, after 英数 (7) */
#define R_A		5
#define Q_SHIFT		9		/* the left Shift: row 3, 0..9 */
#define R_SHIFT		7
#define Q_IME		82		/* the input mode key: row 4, 38..44 */
#define Q_CTRL		5		/* the left Ctrl: row 4, 0..5 */
#define R_CTRL		9
#define Q_E		32		/* E as written: row 1, after Tab (6) */
#define R_E		3
#define Q_D		34		/* D's place: row 2, after 英数 (7); e in Dvorak */
#define R_D		5
#define R_IME		9
#define AT_MENU_X	300
#define AT_MENU_Y	20

/* Middles of keys of the TRON keyboard on its plane (TKB_White.svg, the eight corners' mean) */
typedef struct {
	INT	x, y;
} TRAT;

LOCAL CONST TRAT	tr_l73 = { 1394, 1502 };	/* A (0x04) */
LOCAL CONST TRAT	tr_l03 = { 3213, 2389 };	/* the left Shift, red */
LOCAL CONST TRAT	tr_r03 = { 4965, 2402 };	/* the right Shift, blue */
LOCAL CONST TRAT	tr_l81 = { 712, 1079 };		/* 1, F1 with Fn */
LOCAL CONST TRAT	tr_l93 = { 594, 1774 };		/* Fn */
LOCAL CONST TRAT	tr_l01 = { 3760, 2858 };	/* 英語 (0x39) */
LOCAL CONST TRAT	tr_r01 = { 4418, 2871 };	/* 日本語 (0x88) */

/* Where a point of the plane is in the work area, as sk_main.c's scale_of puts it */
LOCAL void tr_place( CONST TRAT *a, INT *p_x, INT *p_y )
{
	INT	aw = g_w - 2 * SK_M, ah = g_h - 2 * SK_M, num, den;

	if ( aw * TR_PH <= ah * TR_PW ) {
		num = aw;
		den = TR_PW;
	} else {
		num = ah;
		den = TR_PH;
	}
	*p_x = SK_M + ( aw - TR_PW * num / den ) / 2 + a->x * num / den;
	*p_y = SK_M + ( ah - TR_PH * num / den ) / 2 + a->y * num / den;
}

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL ID port_open( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, u) < E_OK ) return E_SYS;
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

LOCAL ER win_make( CONST char *json, TS_UUID *u, ID *p_key, INT *p_wid )
{
	T_OBCRE	c;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"skb window";
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	*p_key = ob_opn_obj(u, OB_OP_ALL);
	*p_wid = wm_obj_wid(u);
	return ( *p_key > 0 && *p_wid > 0 ) ? E_OK : E_OBJ;
}

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
 * The input read as the desktop reads it, for ms: the first key pressed
 * for window want is answered with its modifiers (*p_mods), 0 none.
 */
LOCAL UINT pump( INT ms, INT want, UINT *p_mods )
{
	T_WMEV	ev;
	UINT	got = 0;

	for ( ; ms > 0; ms -= 30 ) {
		while ( wm_read_event(&ev, 30) >= E_OK ) {
			if ( got == 0 && ev.type == HID_EV_KEY_DOWN && ev.wid == want ) {
				got = ev.code;
				if ( p_mods != NULL ) *p_mods = ev.mods;
			}
		}
		if ( got != 0 && want > 0 ) break;
	}
	return got;
}

/*
 * The input read as the desktop reads it, for ms: the first key that is
 * not a modifier pressed for window want, with its modifiers, 0 none
 */
LOCAL UINT pump_key( INT ms, INT want, UINT *p_mods )
{
	T_WMEV	ev;
	UINT	got = 0;

	for ( ; ms > 0 && got == 0; ms -= 30 ) {
		while ( wm_read_event(&ev, 30) >= E_OK ) {
			if ( got == 0 && ev.type == HID_EV_KEY_DOWN && ev.wid == want
			  && ( ev.code < 0xE0 || ev.code > 0xE7 ) ) {
				got = ev.code;
				*p_mods = ev.mods;
			}
		}
	}
	return got;
}

/* Ctrl and a key as the keyboard sends them: Ctrl pressed, the key pressed and let go with it, Ctrl let go */
LOCAL void ctrl_key( UINT code )
{
	T_HIDEV	h;

	knl_memset(&h, 0, sizeof(h));
	h.type = HID_EV_KEY_DOWN;
	h.code = 0xE0;
	h.mods = HID_MOD_LCTRL;
	(void)kt_inject(&h);
	h.code = code;
	(void)kt_inject(&h);
	h.type = HID_EV_KEY_UP;
	(void)kt_inject(&h);
	h.code = 0xE0;
	h.mods = 0;
	(void)kt_inject(&h);
}

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

/* A press at (x, y) of the picture; what came to window want, waited for */
LOCAL UINT press_at( INT wid, INT x, INT y, INT want, UINT *p_mods )
{
	UINT	got = 0;
	INT	i;

	*p_mods = 0xFFFFFFFFU;
	click(wid, x, y, 0);
	for ( i = 0; i < 20 && got == 0; i++ ) got = pump(100, want, p_mods);
	return got;
}

LOCAL UINT key_on( INT wid, INT q2, INT r2, INT want, UINT *p_mods )
{
	return press_at(wid, at_x(q2), at_y(r2), want, p_mods);
}

LOCAL UINT tron_on( INT wid, CONST TRAT *a, INT want, UINT *p_mods )
{
	INT	x, y;

	tr_place(a, &x, &y);
	return press_at(wid, x, y, want, p_mods);
}

/* A press on the picture that types nothing (a modifier, Fn), and the program given time to take it */
LOCAL void tron_hold( INT wid, CONST TRAT *a )
{
	INT	x, y;

	tr_place(a, &x, &y);
	click(wid, x, y, 0);
	(void)pump(300, -1, NULL);
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

LOCAL INT z_of( INT wid )
{
	T_WMWIN	w;

	return ( wm_ref(wid, &w) >= E_OK ) ? w.z : -1;
}

/* ---------------------------------------------------------------- the layout kept */

LOCAL UB	conf_was[CF_TEXT_MAX];
LOCAL INT	conf_was_len = -1;

LOCAL ID conf_open( UINT ops )
{
	TS_UUID	u;

	if ( ob_fnd_sys(SYSROLE_CONF_USER, SYSDEF_CONF_USER, &u) < E_OK ) return E_SYS;
	return ob_opn_obj(&u, ops);
}

/* The layout the program is to open with, as the line SOFTKB_LAYOUT; what there was kept to put back */
LOCAL ER conf_line( CONST char *line, CONST char *v )
{
	UB	t[CF_TEXT_MAX];
	SZ	asz = 0;
	INT	len;
	ID	key = conf_open(OB_OP_R | OB_OP_WRITE);
	ER	er;

	if ( key < E_OK ) return (ER)key;
	er = ob_rea_rec(key, 1, 0, t, sizeof(t), &asz);
	len = ( er >= E_OK ) ? (INT)( ( asz > (SZ)sizeof(t) ) ? (SZ)sizeof(t) : asz ) : 0;
	if ( conf_was_len < 0 ) {
		knl_memcpy(conf_was, t, (SZ)len);
		conf_was_len = len;
	}
	len = cf_put(t, len, (INT)sizeof(t), line, 0, v);
	er = ( len < 0 ) ? E_LIMIT : ob_rpl_rec(key, 1, t, (SZ)len);
	ob_cls_obj(key);
	return er;
}

LOCAL ER conf_layout( CONST char *v )
{
	return conf_line("SOFTKB_LAYOUT", v);
}

/* A line of the user information object as it is now; FALSE when there is none */
LOCAL BOOL conf_get_line( CONST char *line, char *v, INT max )
{
	UB	t[CF_TEXT_MAX];
	SZ	asz = 0;
	INT	len;
	ID	key = conf_open(OB_OP_R);
	ER	er;

	v[0] = 0;
	if ( key < E_OK ) return FALSE;
	er = ob_rea_rec(key, 1, 0, t, sizeof(t), &asz);
	ob_cls_obj(key);
	len = ( er >= E_OK ) ? (INT)( ( asz > (SZ)sizeof(t) ) ? (SZ)sizeof(t) : asz ) : 0;
	return cf_get(t, len, line, 0, v, max);
}

LOCAL void conf_back( void )
{
	ID	key;

	if ( conf_was_len < 0 ) return;
	key = conf_open(OB_OP_R | OB_OP_WRITE);
	if ( key >= E_OK ) {
		(void)ob_rpl_rec(key, 1, conf_was, (SZ)conf_was_len);
		ob_cls_obj(key);
	}
	conf_was_len = -1;
}

/* ---------------------------------------------------------------- the program */

/* The program started, its window found and drawn: its pid, 0 when it could not be */
LOCAL ID start_named( CONST char *title, INT *p_wid )
{
	TS_UUID		prog, pu;
	T_OBCRE		c;
	ID		pid;

	*p_wid = 0;
	if ( ts_str_to_uuid(SYSDEF_PROG_SOFTKB, &prog) < E_OK ) return 0;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	if ( ob_cre_obj(&c, &pu) < E_OK ) return 0;
	pid = knl_prc_of_uuid(&pu);
	*p_wid = win_named(title, 20000);
	tk_dly_tsk(800);			/* sized and drawn */
	return pid;
}

/* The menu of the second button, opened with 閉じる under the pointer, which ends it */
LOCAL ID start( INT *p_wid )
{
	return start_named(SK_TITLE, p_wid);
}

LOCAL BOOL menu_row( INT row, INT *x, INT *y );
LOCAL void at_screen( UINT type, UINT button, INT x, INT y );
LOCAL BOOL choose_item( INT wid, INT item, INT row );

/*
 * The menu opens with the item chosen last under the pointer, which is
 * 閉じる unless another was chosen: its row pressed where it is
 */
LOCAL void close_by_menu( INT wid, ID pid )
{
	T_PSTS	psts;
	INT	i, x, y, before = popups();
	ER	er;

	click(wid, AT_MENU_X, AT_MENU_Y, 1);
	for ( i = 0; i < 30 && popups() == before; i++ ) (void)pump(100, -1, NULL);
	KT_ASSERT(popups() > before);
	(void)pump(500, -1, NULL);
	if ( menu_row(0, &x, &y) ) {
		at_screen(HID_EV_MOVE, 0, x, y);
		(void)pump(200, -1, NULL);
		at_screen(HID_EV_BTN_DOWN, 0, x, y);
		at_screen(HID_EV_BTN_UP, 0, x, y);
	} else {
		click(wid, AT_MENU_X, AT_MENU_Y, 0);
	}
	(void)pump(500, -1, NULL);
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 10000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named(SK_TITLE, 300), 0);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
}

/*
 * The keyboard in front of the windows worked in (WM_ATTR_FLOAT with
 * WM_ATTR_NOFOCUS): another window opened or brought forward comes up
 * behind it, the keys stay with the window that has them, a tool panel
 * of the window worked in stays shown, and with a picture-in-picture
 * window the one brought forward last is in front, both in front of
 * the others
 */
LOCAL void floats( INT kb, INT twid )
{
	T_DPRECT	o;
	T_WMWIN		ws;
	INT		plain, par, sub, pip;

	o.left = 300;  o.top = 300;  o.right = 900;  o.bottom = 760;	/* over the keyboard */
	plain = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "skb plain");
	KT_ASSERT(plain >= 1);
	if ( plain < 1 ) return;
	KT_ASSERT(z_of(kb) < z_of(plain));
	KT_ASSERT_ER(wm_raise(plain), E_OK);
	KT_ASSERT(z_of(kb) < z_of(plain));
	KT_ASSERT_EQ(wm_focused(), twid);
	KT_ASSERT_ER(wm_focus(kb), E_OBJ);
	KT_ASSERT_EQ(wm_focused(), twid);

	/* a window with a tool panel worked in: the panel stays when the keyboard is brought forward */
	o.left = 100;  o.top = 200;  o.right = 500;  o.bottom = 400;
	par = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "skb parent");
	o.left = 520;  o.right = 640;
	sub = wm_open(&o, WM_ATTR_FRAME, "skb panel");
	KT_ASSERT(par >= 1 && sub >= 1);
	if ( par >= 1 && sub >= 1 ) {
		KT_ASSERT_ER(wm_set_parent(sub, par), E_OK);
		KT_ASSERT_ER(wm_raise(par), E_OK);
		KT_ASSERT_ER(wm_focus(par), E_OK);
		KT_ASSERT_ER(wm_ref(sub, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
		KT_ASSERT(z_of(kb) < z_of(par) && z_of(kb) < z_of(sub));
		KT_ASSERT_ER(wm_raise(kb), E_OK);		/* as the desktop does when it is pressed */
		KT_ASSERT_ER(wm_ref(sub, &ws), E_OK);
		KT_ASSERT_EQ(ws.away, 0);
		KT_ASSERT_EQ(wm_focused(), par);
		KT_ASSERT_EQ(wm_self_check(), 0);
	}
	if ( sub >= 1 ) (void)wm_close(sub);
	if ( par >= 1 ) (void)wm_close(par);
	KT_ASSERT_ER(wm_focus(twid), E_OK);

	/* a picture-in-picture window: the one brought forward last in front, the others behind both */
	o.left = 600;  o.top = 500;  o.right = 900;  o.bottom = 700;
	pip = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_FLOAT, "skb pip");
	KT_ASSERT(pip >= 1);
	if ( pip >= 1 ) {
		KT_ASSERT(z_of(pip) < z_of(kb));
		KT_ASSERT_ER(wm_raise(kb), E_OK);
		KT_ASSERT(z_of(kb) < z_of(pip));
		KT_ASSERT_ER(wm_raise(plain), E_OK);
		KT_ASSERT(z_of(kb) < z_of(plain) && z_of(pip) < z_of(plain));
		KT_ASSERT_EQ(wm_self_check(), 0);
		(void)wm_close(pip);
	}
	(void)kt_shot("/boot/SOFTKB4.PPM");
	(void)wm_close(plain);
	KT_ASSERT_EQ(wm_focused(), twid);
	(void)pump(200, -1, NULL);
}

LOCAL void test_program( void )
{
	TS_UUID		tw;
	T_WMWIN		w;
	T_DPRECT	o;
	ID		pid = 0, tk = 0;
	INT		wid, twid = 0;
	UINT		mods, mode_was = wm_msg_get_mode();

	knl_memset(&tw, 0, sizeof(tw));
	g_w = SK_W0;
	g_h = SK_H0;
	KT_ASSERT_ER(conf_layout("qwerty"), E_OK);

	/* the window the keys go to */
	KT_ASSERT_ER(win_make("{\"rect\":[60,40,460,180],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 ) goto out;
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	(void)pump(100, -1, NULL);

	pid = start(&wid);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) goto stop;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	KT_ASSERT(( w.attr & WM_ATTR_NOFOCUS ) != 0 && ( w.attr & WM_ATTR_RESIZE ) != 0
	       && ( w.attr & WM_ATTR_FLOAT ) != 0);
	KT_ASSERT_EQ(w.work.right - w.work.left, SK_W0);
	KT_ASSERT_EQ(w.work.bottom - w.work.top, SK_H0);
	(void)kt_shot("/boot/SOFTKB0.PPM");

	/* A: typed into the window that had the keys, which keeps them */
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, OWN_QJ);
	KT_ASSERT_EQ(wm_focused(), twid);

	/* held: typed again and again as the keyboard's keys are, and no more once let go */
	{
		T_WMWIN	kw;
		T_HIDEV	h;
		T_WMEV	ev;
		INT	n = 0, t;

		KT_ASSERT_ER(wm_ref(wid, &kw), E_OK);
		knl_memset(&h, 0, sizeof(h));
		h.type = HID_EV_MOVE;
		h.x = kw.work.left + at_x(Q_A);
		h.y = kw.work.top + at_y(R_A);
		(void)kt_inject(&h);
		h.type = HID_EV_BTN_DOWN;
		(void)kt_inject(&h);
		for ( t = 0; t < 2500; t += 30 ) {
			while ( wm_read_event(&ev, 30) >= E_OK ) {
				if ( ev.type == HID_EV_KEY_DOWN && ev.wid == twid && ev.code == 0x04 ) n++;
			}
		}
		h.type = HID_EV_BTN_UP;
		(void)kt_inject(&h);
		(void)pump(300, -1, NULL);
		tm_printf((UB *)"  held 2.5 s: typed %d times\n", n);
		KT_ASSERT(n >= 3);
		KT_ASSERT_EQ(pump(500, twid, NULL), 0);
	}

	/* Shift once: the next key only */
	click(wid, at_x(Q_SHIFT), at_y(R_SHIFT), 0);
	(void)pump(300, -1, NULL);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, HID_MOD_LSHIFT | OWN_QJ);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, OWN_QJ);

	/* Shift twice: until pressed again */
	click(wid, at_x(Q_SHIFT), at_y(R_SHIFT), 0);
	(void)pump(300, -1, NULL);
	click(wid, at_x(Q_SHIFT), at_y(R_SHIFT), 0);
	(void)pump(300, -1, NULL);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, HID_MOD_LSHIFT | OWN_QJ);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, HID_MOD_LSHIFT | OWN_QJ);
	click(wid, at_x(Q_SHIFT), at_y(R_SHIFT), 0);
	(void)pump(300, -1, NULL);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, OWN_QJ);

	/*
	 * Ctrl+E: the picture's Ctrl and the key written e (E's place, the
	 * keyboard's own QWERTY) send what the keyboard plugged in sends for
	 * Ctrl and the key that types e in the machine's layout -- E's place
	 * with Ctrl as the keys are written, D's place with Ctrl in Dvorak --
	 * and the programs take both as Ctrl+E, by the letter the key types
	 * (wm_key_cmd). Without Ctrl, the keys type their own letters whatever
	 * the machine's layout: D's place d.
	 */
	{
		UINT		kmods = 0, kcode;
		T_KMSTATE	km, dv;

		KT_ASSERT_ER(km_get(&km), E_OK);
		dv = km;
		dv.latin = KM_LATIN_KEYS;
		KT_ASSERT_ER(km_set(&dv, KM_SET_LATIN), E_OK);
		ctrl_key(0x08);
		kcode = pump_key(2000, twid, &kmods);
		KT_ASSERT_EQ(kcode, 0x08);
		KT_ASSERT_EQ(kmods, HID_MOD_LCTRL);
		KT_ASSERT_EQ(wm_key_cmd(kcode), 0x08);
		click(wid, at_x(Q_CTRL), at_y(R_CTRL), 0);
		(void)pump(300, -1, NULL);
		KT_ASSERT_EQ(key_on(wid, Q_E, R_E, twid, &mods), kcode);
		KT_ASSERT_EQ(mods, kmods);

		dv.latin = KM_LATIN_DVORAK;
		KT_ASSERT_ER(km_set(&dv, KM_SET_LATIN), E_OK);
		ctrl_key(0x07);
		kcode = pump_key(2000, twid, &kmods);
		KT_ASSERT_EQ(kcode, 0x07);
		KT_ASSERT_EQ(kmods, HID_MOD_LCTRL);
		KT_ASSERT_EQ(wm_key_cmd(kcode), 0x08);		/* taken as E */
		click(wid, at_x(Q_CTRL), at_y(R_CTRL), 0);
		(void)pump(300, -1, NULL);
		KT_ASSERT_EQ(key_on(wid, Q_E, R_E, twid, &mods), kcode);
		KT_ASSERT_EQ(mods, kmods);
		KT_ASSERT_EQ(key_on(wid, Q_D, R_D, twid, &mods), 0x07);	/* without Ctrl: the letter d, its own */
		KT_ASSERT_EQ(mods, OWN_QJ);
		KT_ASSERT_EQ(wm_key_char(0x07, mods), 'd');
		(void)km_set(&km, KM_SET_LATIN);
	}

	/* the input mode key: 日本語 from 英語 (ひらがなカタカナ), then 英語 again (英数) */
	(void)wm_msg_mode(WM_MODE_ALPH | WM_MODE_ROMAN);
	tk_dly_tsk(600);			/* the program reads the mode again */
	KT_ASSERT_EQ(key_on(wid, Q_IME, R_IME, twid, &mods), 0x88);
	KT_ASSERT_EQ(wm_msg_get_mode() & WM_MODE_ALPH, 0);
	(void)pump(300, -1, NULL);
	KT_ASSERT_EQ(key_on(wid, Q_IME, R_IME, twid, &mods), 0x39);
	KT_ASSERT(( wm_msg_get_mode() & WM_MODE_ALPH ) != 0);
	KT_ASSERT_EQ(wm_focused(), twid);

	/* in front of the windows worked in */
	floats(wid, twid);

	/* made larger: the keys with it */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	o = w.outer;
	o.right += 320;
	o.bottom += 100;
	KT_ASSERT_ER(wm_move(wid, &o), E_OK);
	(void)pump(600, -1, NULL);
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	g_w = w.work.right - w.work.left;
	g_h = w.work.bottom - w.work.top;
	KT_ASSERT_EQ(g_w, SK_W0 + 320);
	KT_ASSERT_EQ(key_on(wid, Q_A, R_A, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, OWN_QJ);

	close_by_menu(wid, pid);
	pid = 0;
stop:
	if ( pid > 0 ) {
		T_PSTS	psts;

		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	(void)pump(100, -1, NULL);
	(void)wm_msg_mode(mode_was);
	conf_back();
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

IMPORT BOOL kt_touch_plug( void );
IMPORT void kt_touch_at( INT id, BOOL on, INT sx, INT sy );
IMPORT void kt_touch_unplug( void );

/* A finger held down this long: what came to window want counted, and when the first came, ms */
LOCAL INT finger_typed( INT ms, INT want, UINT code, INT *p_first, INT *p_up )
{
	T_WMEV	ev;
	INT	n = 0, t;

	for ( t = 0; t < ms; t += 20 ) {
		while ( wm_read_event(&ev, 20) >= E_OK ) {
			if ( ev.wid != want || ev.code != code ) continue;
			if ( ev.type == HID_EV_KEY_DOWN && n++ == 0 && p_first != NULL ) *p_first = t;
			if ( ev.type == HID_EV_KEY_UP && p_up != NULL ) ( *p_up )++;
		}
	}
	return n;
}

/*
 * A finger on the English letters: on a key it presses at once and stays
 * pressed, repeating until it lets go or goes out of the window, and no
 * menu comes; on the ground at a row's end and between the keys a tap types
 * nothing and a hold opens the menu
 */
LOCAL void test_finger( void )
{
	TS_UUID		tw;
	T_WMWIN		w;
	T_PSTS		psts;
	ID		pid = 0, tk = 0;
	INT		wid = 0, twid = 0, before, first = -1, up = 0, n, i, ax, ay, gx, gy;
	UINT		mode_was = wm_msg_get_mode();
	BOOL		plugged = FALSE;

	knl_memset(&tw, 0, sizeof(tw));
	g_w = SK_W0;
	g_h = SK_H0;
	KT_ASSERT_ER(conf_layout("qwerty"), E_OK);
	KT_ASSERT_ER(win_make("{\"rect\":[60,40,460,180],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 ) goto out;
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	pid = start(&wid);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	if ( wid <= 0 ) goto stop;
	plugged = kt_touch_plug();
	if ( !plugged ) KT_SKIP("no touch screen plugged in");
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	ax = w.work.left + at_x(Q_A);
	ay = w.work.top + at_y(R_A);
	gx = w.work.left + SK_M + 4;			/* the ground at the left of the rows */
	gy = ay;
	(void)pump(300, -1, NULL);
	before = popups();

	/* on a key: at once, repeating past a hold, no menu; let go, it stops */
	kt_touch_at(50, TRUE, ax, ay);
	n = finger_typed(2500, twid, 0x04, &first, NULL);
	tm_printf((UB *)"  a finger held 2.5 s: typed %d times, the first at %d ms\n", n, first);
	KT_ASSERT(n >= 3);
	KT_ASSERT(first >= 0 && first <= 300);
	KT_ASSERT_EQ(popups(), before);
	kt_touch_at(50, FALSE, ax, ay);
	(void)finger_typed(400, twid, 0x04, NULL, &up);
	KT_ASSERT(up >= 1);
	KT_ASSERT_EQ(finger_typed(1000, twid, 0x04, NULL, NULL), 0);

	/* a quick tap of a key: typed once */
	kt_touch_at(51, TRUE, ax, ay);
	tk_dly_tsk(40);
	kt_touch_at(51, FALSE, ax, ay);
	KT_ASSERT_EQ(finger_typed(500, twid, 0x04, NULL, NULL), 1);

	/* gone out of the window with the finger on it: let go there */
	first = -1;
	up = 0;
	kt_touch_at(52, TRUE, ax, ay);
	(void)finger_typed(200, twid, 0x04, &first, NULL);
	KT_ASSERT(first >= 0);
	kt_touch_at(52, TRUE, 20, 20);
	(void)finger_typed(400, twid, 0x04, NULL, &up);
	KT_ASSERT(up >= 1);
	KT_ASSERT_EQ(finger_typed(1000, twid, 0x04, NULL, NULL), 0);
	kt_touch_at(52, FALSE, 20, 20);
	(void)pump(300, -1, NULL);

	/* a tap on the ground: nothing typed, no menu */
	kt_touch_at(53, TRUE, gx, gy);
	tk_dly_tsk(40);
	kt_touch_at(53, FALSE, gx, gy);
	KT_ASSERT_EQ(pump(500, twid, NULL), 0);
	KT_ASSERT_EQ(popups(), before);

	/* a hold on it: the menu, as the right button; its first row closes the program */
	kt_touch_at(54, TRUE, gx, gy);
	for ( i = 0; i < 30 && popups() == before; i++ ) (void)pump(100, -1, NULL);
	KT_ASSERT(popups() > before);
	kt_touch_at(54, FALSE, gx, gy);
	(void)pump(500, -1, NULL);
	click(wid, gx - w.work.left, gy - w.work.top, 0);
	(void)pump(500, -1, NULL);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	pid = 0;
stop:
	if ( pid > 0 ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
	if ( plugged ) kt_touch_unplug();
out:
	(void)pump(100, -1, NULL);
	(void)wm_msg_mode(mode_was);
	conf_back();
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

/* The TRON keyboard, kept as the layout the program opens with */
LOCAL void test_tron( void )
{
	TS_UUID		tw;
	T_WMWIN		w;
	ID		pid = 0, tk = 0;
	INT		wid, twid = 0;
	UINT		mods, mode_was = wm_msg_get_mode();

	knl_memset(&tw, 0, sizeof(tw));
	KT_ASSERT_ER(conf_layout("tron"), E_OK);
	KT_ASSERT_ER(win_make("{\"rect\":[60,40,460,180],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 ) goto out;
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	(void)pump(100, -1, NULL);

	pid = start(&wid);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) goto stop;
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	g_w = w.work.right - w.work.left;
	g_h = w.work.bottom - w.work.top;
	KT_ASSERT_EQ(g_w, TR_W0);
	KT_ASSERT_EQ(g_h, TR_H0);
	(void)kt_shot("/boot/SOFTKB5.PPM");

	/* each key the usage of its place: A, with the red Shift, with the blue */
	KT_ASSERT_EQ(tron_on(wid, &tr_l73, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, OWN_DT);
	tron_hold(wid, &tr_l03);
	KT_ASSERT_EQ(tron_on(wid, &tr_l73, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, HID_MOD_LSHIFT | OWN_DT);
	tron_hold(wid, &tr_r03);
	KT_ASSERT_EQ(tron_on(wid, &tr_l73, twid, &mods), 0x04);
	KT_ASSERT_EQ(mods, HID_MOD_RSHIFT | OWN_DT);

	/* 1, and with Fn F1 */
	KT_ASSERT_EQ(tron_on(wid, &tr_l81, twid, &mods), 0x1E);
	tron_hold(wid, &tr_l93);
	KT_ASSERT_EQ(tron_on(wid, &tr_l81, twid, &mods), 0x3A);
	KT_ASSERT_EQ(tron_on(wid, &tr_l81, twid, &mods), 0x1E);

	/* 日本語 and 英語: the input mode as the keyboard's keys set it */
	(void)wm_msg_mode(WM_MODE_ALPH | WM_MODE_ROMAN);
	KT_ASSERT_EQ(tron_on(wid, &tr_r01, twid, &mods), 0x88);
	KT_ASSERT_EQ(wm_msg_get_mode() & WM_MODE_ALPH, 0);
	KT_ASSERT_EQ(tron_on(wid, &tr_l01, twid, &mods), 0x39);
	KT_ASSERT(( wm_msg_get_mode() & WM_MODE_ALPH ) != 0);
	KT_ASSERT_EQ(wm_focused(), twid);

	close_by_menu(wid, pid);
	pid = 0;
stop:
	if ( pid > 0 ) {
		T_PSTS	psts;

		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	(void)pump(100, -1, NULL);
	(void)wm_msg_mode(mode_was);
	conf_back();
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

/*
 * The μTRON keyboard (sk_utron.c): its halves in two windows at the
 * screen's edges, places of the chart's plane in tenths of a unit, the
 * middles of keys' boxes
 */
#define UT_W0		380
#define UT_H0		290
#define UT_ROOM		60
#define UT_HL_L		367		/* the halves' boxes */
#define UT_HR_L		4232
#define UT_H_T		1335
#define UT_H_W		( 4188 - 367 )
#define UT_H_H		( 4192 - 1335 )

LOCAL CONST TRAT	ut_l23 = { 1528, 2538 };	/* A */
LOCAL CONST TRAT	ut_l16 = { 2947, 2083 };	/* P (は) */
LOCAL CONST TRAT	ut_l13 = { 1582, 2083 };	/* ; ^ : (ら) */
LOCAL CONST TRAT	ut_l03 = { 1711, 1628 };	/* 1, F2 with Fn */
LOCAL CONST TRAT	ut_l43 = { 2832, 3449 };	/* the left Shift, red */
LOCAL CONST TRAT	ut_l51 = { 687, 3708 };		/* Fn */
LOCAL CONST TRAT	ut_l64 = { 3788, 3907 };	/* 英語 (0x39) */
LOCAL CONST TRAT	ut_r42 = { 5574, 3449 };	/* the right Shift, blue */
LOCAL CONST TRAT	ut_r61 = { 4618, 3907 };	/* 日本語 (0x88) */
LOCAL CONST TRAT	ut_l25 = { 2438, 2538 };	/* E */
LOCAL CONST TRAT	ut_l31 = { 766, 2993 };		/* Ctrl */
LOCAL CONST TRAT	ut_l61 = { 1126, 3907 };	/* Home, PrtSc with Fn */

/* Where a point of the plane is in a half's window of work area w x h, as sk_main.c's scale_of puts it */
LOCAL void ut_place( CONST TRAT *a, BOOL right, INT w, INT h, INT *p_x, INT *p_y )
{
	INT	rx = ( right ? UT_HR_L : UT_HL_L ) - UT_ROOM, ry = UT_H_T - UT_ROOM;
	INT	rw = UT_H_W + 2 * UT_ROOM, rh = UT_H_H + 2 * UT_ROOM;
	INT	aw = w - 2 * SK_M, ah = h - 2 * SK_M, num, den;

	if ( aw * rh <= ah * rw ) {
		num = aw;
		den = rw;
	} else {
		num = ah;
		den = rh;
	}
	*p_x = SK_M + ( aw - rw * num / den ) / 2 + ( a->x - rx ) * num / den;
	*p_y = SK_M + ( ah - rh * num / den ) / 2 + ( a->y - ry ) * num / den;
}

LOCAL UINT ut_on( INT wid, BOOL right, CONST TRAT *a, INT want, UINT *p_mods )
{
	T_WMWIN	w;
	INT	x, y;

	if ( wm_ref(wid, &w) < E_OK ) return 0;
	ut_place(a, right, w.work.right - w.work.left, w.work.bottom - w.work.top, &x, &y);
	return press_at(wid, x, y, want, p_mods);
}

LOCAL void ut_hold( INT wid, BOOL right, CONST TRAT *a )
{
	T_WMWIN	w;
	INT	x, y;

	if ( wm_ref(wid, &w) < E_OK ) return;
	ut_place(a, right, w.work.right - w.work.left, w.work.bottom - w.work.top, &x, &y);
	click(wid, x, y, 0);
	(void)pump(300, -1, NULL);
}

/* The usage that types a letter in a latin layout (own, else the machine's), with Shift or not, as the program looks for it */
LOCAL BOOL latin_of( char ch, UINT own, UINT *p_code, UINT *p_mods )
{
	char	b[8];
	UINT	c;
	INT	sh;

	for ( sh = 0; sh < 2; sh++ ) {
		for ( c = 0x04; c <= 0x89; c++ ) {
			if ( c > 0x38 && c != 0x87 && c != 0x89 ) continue;
			if ( km_label(KM_L_CHAR, c, ( sh ? HID_MOD_LSHIFT : 0 ) | own, b, sizeof(b)) == 1 && b[0] == ch ) {
				*p_code = c;
				*p_mods = sh ? HID_MOD_LSHIFT : 0;
				return TRUE;
			}
		}
	}
	return FALSE;
}

LOCAL void test_utron( void )
{
	TS_UUID		tw;
	T_WMWIN		wl, wr;
	T_KMSTATE	was, st;
	ID		pid = 0, tk = 0;
	INT		lw, rw, twid = 0, sw = 0, sh = 0;
	UINT		mods, code, cmods, mode_was = wm_msg_get_mode();

	knl_memset(&tw, 0, sizeof(tw));
	KT_ASSERT_ER(km_get(&was), E_OK);
	st = was;
	st.latin = KM_LATIN_KEYS;			/* the machine's: not the keyboard's own */
	st.kana = KM_KANA_JIS;
	KT_ASSERT_ER(km_set(&st, KM_SET_LATIN | KM_SET_KANA), E_OK);
	KT_ASSERT_ER(conf_layout("utron"), E_OK);
	KT_ASSERT_ER(win_make("{\"rect\":[400,40,800,180],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 ) goto out;
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	(void)pump(100, -1, NULL);

	pid = start_named(SK_TITLE_L, &lw);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	rw = win_named(SK_TITLE_R, 3000);
	KT_ASSERT(lw > 0 && rw > 0);
	if ( lw <= 0 || rw <= 0 ) goto stop;

	/* a half at each edge of the screen, both in front and never having the keys */
	wm_screen(&sw, &sh);
	KT_ASSERT_ER(wm_ref(lw, &wl), E_OK);
	KT_ASSERT_ER(wm_ref(rw, &wr), E_OK);
	KT_ASSERT(( wl.attr & ( WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ) ) == ( WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ));
	KT_ASSERT(( wr.attr & ( WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ) ) == ( WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ));
	KT_ASSERT_EQ(wl.outer.left, 0);
	KT_ASSERT_EQ(wr.outer.right, sw);
	KT_ASSERT_EQ(wl.work.right - wl.work.left, UT_W0);
	KT_ASSERT_EQ(wr.work.bottom - wr.work.top, UT_H0);
	(void)kt_shot("/boot/SOFTKB7.PPM");

	/* the right half made larger by its corner: the left the same size, each at its edge */
	{
		T_DPRECT	o = wr.outer;

		o.right += 60;
		o.bottom += 40;
		KT_ASSERT_ER(wm_move(rw, &o), E_OK);
		(void)pump(800, -1, NULL);
		KT_ASSERT_ER(wm_ref(lw, &wl), E_OK);
		KT_ASSERT_ER(wm_ref(rw, &wr), E_OK);
		KT_ASSERT_EQ(wr.work.right - wr.work.left, UT_W0 + 60);
		KT_ASSERT_EQ(wr.work.bottom - wr.work.top, UT_H0 + 40);
		KT_ASSERT_EQ(wl.work.right - wl.work.left, UT_W0 + 60);
		KT_ASSERT_EQ(wl.work.bottom - wl.work.top, UT_H0 + 40);
		KT_ASSERT_EQ(wr.outer.right, sw);
		KT_ASSERT_EQ(wl.outer.left, 0);
		KT_ASSERT_EQ(wm_self_check(), 0);
	}

	/* NumLock, Fn on the @ key: the green faces type the keypad's keys; again, the letters */
	(void)wm_msg_mode(WM_MODE_ALPH | WM_MODE_ROMAN);
	tk_dly_tsk(600);
	{
		static CONST TRAT	ut_l11 = { 685, 2083 };
		static CONST TRAT	ut_r23 = { 5513, 2538 };
		UINT			pl;

		pl = ut_on(rw, TRUE, &ut_r23, twid, &mods);
		KT_ASSERT(pl != 0 && pl != 0x59);
		ut_hold(lw, FALSE, &ut_l51);
		ut_hold(lw, FALSE, &ut_l11);
		KT_ASSERT_EQ(ut_on(rw, TRUE, &ut_r23, twid, &mods), 0x59);
		ut_hold(lw, FALSE, &ut_l51);
		ut_hold(lw, FALSE, &ut_l11);
		KT_ASSERT_EQ(ut_on(rw, TRUE, &ut_r23, twid, &mods), pl);
	}

	/*
	 * the letters of the faces, typed as the keyboard's own latin layout
	 * (Dvorak) types them, which the window reads them in: P, the red ^
	 * and the blue :
	 */
	(void)wm_msg_mode(WM_MODE_ALPH | WM_MODE_ROMAN);
	tk_dly_tsk(600);
	KT_ASSERT(latin_of('p', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l16, twid, &mods), code);
	KT_ASSERT_EQ(mods, cmods | OWN_DT);
	KT_ASSERT_EQ(wm_key_char(code, mods), 'p');
	ut_hold(lw, FALSE, &ut_l43);
	KT_ASSERT(latin_of('^', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), code);
	KT_ASSERT_EQ(mods, cmods | OWN_DT);
	ut_hold(rw, TRUE, &ut_r42);
	KT_ASSERT(latin_of(':', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), code);
	KT_ASSERT_EQ(mods, cmods | OWN_DT);
	KT_ASSERT_EQ(wm_key_char(code, mods), ':');
	KT_ASSERT(latin_of('a', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l23, twid, &mods), code);

	/* Fn and 1: F2 */
	ut_hold(lw, FALSE, &ut_l51);
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l03, twid, &mods), 0x3B);

	/* Ctrl and the key written E: the key that types e in the machine's layout with Ctrl, taken as Ctrl+E */
	ut_hold(lw, FALSE, &ut_l31);
	KT_ASSERT(latin_of('e', 0, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l25, twid, &mods), code);
	KT_ASSERT_EQ(mods, HID_MOD_LCTRL);
	KT_ASSERT_EQ(wm_key_cmd(code), 0x08);

	/*
	 * The machine's latin layout changed (キー配列): the keyboard keeps
	 * its own, the key at Q's place the chart's ;. The logical layout
	 * QWERTY chosen with its menu: the key at Q's place written q and
	 * typing it, the machine's left as it is; then Dvorak again.
	 */
	st.latin = KM_LATIN_DVORAK;
	KT_ASSERT_ER(km_set(&st, KM_SET_LATIN), E_OK);
	(void)pump(900, -1, NULL);
	KT_ASSERT(latin_of(';', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), code);
	KT_ASSERT_EQ(mods, cmods | OWN_DT);
	st.latin = KM_LATIN_KEYS;
	KT_ASSERT_ER(km_set(&st, KM_SET_LATIN), E_OK);
	KT_ASSERT(choose_item(lw, 3, 0));
	(void)pump(600, -1, NULL);
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), 0x14);
	KT_ASSERT_EQ(mods, KM_MOD_LAYOUT(KM_LATIN_KEYS, KM_KANA_TRON));
	KT_ASSERT_EQ(wm_key_char(0x14, mods), 'q');
	KT_ASSERT_EQ(km_latin(), KM_LATIN_KEYS);
	KT_ASSERT(choose_item(lw, 3, 1));
	(void)pump(600, -1, NULL);
	KT_ASSERT(latin_of(';', OWN_DT, &code, &cmods));
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), code);
	KT_ASSERT_EQ(km_latin(), KM_LATIN_KEYS);

	/* the left arrows' keys: Home as written, PrtSc under it with Fn */
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l61, twid, &mods), 0x4A);
	ut_hold(lw, FALSE, &ut_l51);
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l61, twid, &mods), 0x46);

	/* 日本語 with kana typed: the usage of ら's place; 英語 and 日本語 from either half */
	(void)wm_msg_mode(0);
	tk_dly_tsk(600);
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l13, twid, &mods), 0x14);
	KT_ASSERT_EQ(ut_on(lw, FALSE, &ut_l64, twid, &mods), 0x39);
	KT_ASSERT(( wm_msg_get_mode() & WM_MODE_ALPH ) != 0);
	KT_ASSERT_EQ(ut_on(rw, TRUE, &ut_r61, twid, &mods), 0x88);
	KT_ASSERT_EQ(wm_msg_get_mode() & WM_MODE_ALPH, 0);
	KT_ASSERT_EQ(wm_focused(), twid);

	/* the menu of the right half, 閉じる: both halves closed, the program ended */
	KT_ASSERT_ER(wm_ref(lw, &wl), E_OK);
	KT_ASSERT_ER(wm_ref(rw, &wr), E_OK);
	close_by_menu(rw, pid);
	KT_ASSERT_EQ(win_named(SK_TITLE_L, 300), 0);
	KT_ASSERT_EQ(win_named(SK_TITLE_R, 300), 0);
	pid = 0;

	/* opened again: where the halves were, as large as they were */
	{
		T_WMWIN	nl, nr;

		pid = start_named(SK_TITLE_L, &lw);
		rw = win_named(SK_TITLE_R, 3000);
		KT_ASSERT(pid > 0 && lw > 0 && rw > 0);
		if ( pid <= 0 || lw <= 0 || rw <= 0 ) goto stop;
		KT_ASSERT_ER(wm_ref(lw, &nl), E_OK);
		KT_ASSERT_ER(wm_ref(rw, &nr), E_OK);
		KT_ASSERT(nl.outer.left == wl.outer.left && nl.outer.top == wl.outer.top
		       && nl.outer.right == wl.outer.right && nl.outer.bottom == wl.outer.bottom);
		KT_ASSERT(nr.outer.left == wr.outer.left && nr.outer.top == wr.outer.top
		       && nr.outer.right == wr.outer.right && nr.outer.bottom == wr.outer.bottom);
		close_by_menu(rw, pid);
		pid = 0;
	}
stop:
	if ( pid > 0 ) {
		T_PSTS	psts;

		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	(void)pump(100, -1, NULL);
	(void)wm_msg_mode(mode_was);
	(void)km_set(&was, KM_SET_LATIN | KM_SET_KANA);
	conf_back();
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

/* A window of the program opened with its layout and where it was last, as the lines say */
LOCAL ID start_at( CONST char *layout, CONST char *line, CONST char *place, CONST char *title, INT *p_wid,
		   T_WMWIN *w )
{
	ID	pid;

	KT_ASSERT_ER(conf_layout(layout), E_OK);
	if ( line != NULL ) KT_ASSERT_ER(conf_line(line, place), E_OK);
	pid = start_named(title, p_wid);
	if ( pid > 0 && *p_wid > 0 ) KT_ASSERT_ER(wm_ref(*p_wid, w), E_OK);
	return pid;
}

LOCAL void test_wins( void )
{
	T_WMWIN	w;
	ID	pid;
	INT	wid, sw = 0, sh = 0;
	char	q[64], t[64];

	wm_screen(&sw, &sh);

	/* where the English letters' window was: there again */
	pid = start_at("qwerty", "SOFTKB_WIN_qwerty", "100 60 500 200 0", SK_TITLE, &wid, &w);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) goto out;
	KT_ASSERT_EQ(w.outer.left, 100);
	KT_ASSERT_EQ(w.outer.top, 60);
	KT_ASSERT_EQ(w.work.right - w.work.left, 500);
	KT_ASSERT_EQ(w.work.bottom - w.work.top, 200);
	close_by_menu(wid, pid);

	/* the TRON keyboard's apart from it */
	pid = start_at("tron", "SOFTKB_WIN_tron", "200 100 600 250 0", SK_TITLE, &wid, &w);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto out;
	KT_ASSERT_EQ(w.outer.left, 200);
	KT_ASSERT_EQ(w.work.right - w.work.left, 600);
	close_by_menu(wid, pid);
	KT_ASSERT(conf_get_line("SOFTKB_WIN_qwerty", q, sizeof(q)) && conf_get_line("SOFTKB_WIN_tron", t, sizeof(t)));
	KT_ASSERT(q[0] == '1' && q[1] == '0' && q[2] == '0' && q[3] == ' ');	/* left as it was */
	KT_ASSERT(t[0] == '2' && t[1] == '0' && t[2] == '0' && t[3] == ' ');

	/* a place off the screen, and a size larger than it: brought onto it, the message line kept clear */
	pid = start_at("qwerty", "SOFTKB_WIN_qwerty", "5000 4000 3000 2000 0", SK_TITLE, &wid, &w);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto out;
	KT_ASSERT(w.outer.left >= 0 && w.outer.top >= 0);
	KT_ASSERT(w.outer.right <= sw && w.outer.bottom <= sh - 24);
	KT_ASSERT(w.work.right - w.work.left <= sw);
	close_by_menu(wid, pid);
out:
	(void)pump(100, -1, NULL);
	conf_back();
}

/* ---------------------------------------------------------------- the layouts apart */

IMPORT INT  knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );
IMPORT void knl_hid_ext_detach( INT hd );
IMPORT void knl_hid_ext_report( INT hd, CONST UB *r, INT len );

LOCAL CONST TRAT	tr_l43 = { 2538, 1215 };	/* D's place (0x07): e in Dvorak, d in QWERTY */

/* A pointer event at a place of the screen */
LOCAL void at_screen( UINT type, UINT button, INT x, INT y )
{
	T_HIDEV	h;

	knl_memset(&h, 0, sizeof(h));
	h.type = type;
	h.code = button;
	h.x = x;
	h.y = y;
	(void)kt_inject(&h);
}

/*
 * The middle of a row of the menu that is up (the list farthest to the
 * left), its rows counted from 0 with the lines between groups; FALSE
 * while no list is up
 */
LOCAL BOOL menu_row( INT row, INT *x, INT *y )
{
	T_DPRECT	r;
	T_WMWIN		w;
	INT		pid, wid, best = 0x7fffffff;

	for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) {
		if ( wm_panel_rect(pid, &r) < E_OK ) continue;
		wid = wm_panel_wid(pid);
		if ( wid <= 0 || wm_ref(wid, &w) < E_OK || ( w.attr & WM_ATTR_POPUP ) == 0 ) continue;
		if ( w.work.left < best ) {
			best = w.work.left;
			*x = w.work.left + ( r.right - r.left ) / 2;
			*y = w.work.top + r.top + 1 + row * wm_menu_row_h() + wm_menu_row_h() / 2;
		}
	}
	return (BOOL)( best != 0x7fffffff );
}

/* The first row's middle of the list opened beside the menu (the one farthest to the right), once there are two */
LOCAL BOOL menu_sub_row( INT *x, INT *y )
{
	T_DPRECT	r;
	T_WMWIN		w;
	INT		pid, wid, best = -1, lists = 0;

	for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) {
		if ( wm_panel_rect(pid, &r) < E_OK ) continue;
		wid = wm_panel_wid(pid);
		if ( wid <= 0 || wm_ref(wid, &w) < E_OK || ( w.attr & WM_ATTR_POPUP ) == 0 ) continue;
		lists++;
		if ( w.work.left > best ) {
			best = w.work.left;
			*x = w.work.left + ( r.right - r.left ) / 2;
			*y = w.work.top + r.top + 1 + wm_menu_row_h() / 2;
		}
	}
	return (BOOL)( lists >= 2 );
}

/*
 * A row of a list of the menu of the second button chosen: the menu's row
 * item (counted with the line after 閉じる: 2 配列, 3 論理配列) held until
 * its list opens, then the list's row
 */
LOCAL BOOL choose_item( INT wid, INT item, INT row )
{
	INT	i, x = 0, y = 0, sx = 0, sy = 0, before = popups();

	click(wid, AT_MENU_X, AT_MENU_Y, 1);
	for ( i = 0; i < 30 && popups() == before; i++ ) (void)pump(100, -1, NULL);
	(void)pump(300, -1, NULL);
	if ( !menu_row(item, &x, &y) ) return FALSE;
	at_screen(HID_EV_MOVE, 0, x, y);
	for ( i = 0; i < 40 && !menu_sub_row(&sx, &sy); i++ ) {
		(void)pump(100, -1, NULL);
		if ( i % 5 == 4 ) at_screen(HID_EV_MOVE, 0, x + 1 + ( i / 5 ) % 2, y);
	}
	if ( !menu_sub_row(&sx, &sy) || sx <= x ) return FALSE;
	at_screen(HID_EV_MOVE, 0, sx, y);
	(void)pump(200, -1, NULL);
	at_screen(HID_EV_MOVE, 0, sx, sy + row * wm_menu_row_h());
	(void)pump(200, -1, NULL);
	at_screen(HID_EV_BTN_DOWN, 0, sx, sy + row * wm_menu_row_h());
	at_screen(HID_EV_BTN_UP, 0, sx, sy + row * wm_menu_row_h());
	(void)pump(300, -1, NULL);
	return TRUE;
}

/* A layout chosen with the menu: 配列, then 0 英字, 1 TRON配列, 2 μTRON配列 */
LOCAL BOOL choose_layout( INT wid, INT row )
{
	return choose_item(wid, 2, row);
}

/* The program's window once it has opened w wide (another layout chosen), its size in g_w, g_h; 0 none */
LOCAL INT win_wide( INT w, INT ms )
{
	T_WMWIN	ww;
	INT	wid;

	for ( ; ms > 0; ms -= 100 ) {
		wid = win_named(SK_TITLE, 100);
		if ( wid > 0 && wm_ref(wid, &ww) >= E_OK && ww.work.right - ww.work.left == w ) {
			g_w = ww.work.right - ww.work.left;
			g_h = ww.work.bottom - ww.work.top;
			return wid;
		}
		(void)pump(100, -1, NULL);
	}
	return 0;
}

/* A key pressed and let go on the keyboard plugged in, as its reports say: what came to window want */
LOCAL UINT phys_key( INT hd, UB code, INT want, UINT *p_mods )
{
	UB	r[8];
	UINT	got;

	knl_memset(r, 0, sizeof(r));
	r[2] = code;
	knl_hid_ext_report(hd, r, 8);
	*p_mods = 0xFFFFFFFFU;
	got = pump(2000, want, p_mods);
	r[2] = 0;
	knl_hid_ext_report(hd, r, 8);
	(void)pump(300, -1, NULL);
	return got;
}

/*
 * The screen keyboard's layouts and the machine's apart: the TRON
 * keyboard chosen with the menu leaves the machine's layouts (QWERTY and
 * JIS かな here) as they are, so that the keyboard plugged in still
 * types d at D's place and ち at A's, while the screen keyboard's own
 * keys type in Dvorak and TRON かな配列: e at D's place, た at A's. The
 * English letters chosen again, the machine's are still as they were.
 */
LOCAL void test_apart( void )
{
	TS_UUID		tw;
	T_KMSTATE	was, st, now;
	ID		pid = 0, tk = 0;
	INT		wid = 0, twid = 0, hd = 0;
	UINT		code, mods;
	char		b[16];

	knl_memset(&tw, 0, sizeof(tw));
	KT_ASSERT_ER(km_get(&was), E_OK);
	st = was;
	st.latin = KM_LATIN_KEYS;
	st.kana = KM_KANA_JIS;
	KT_ASSERT_ER(km_set(&st, KM_SET_LATIN | KM_SET_KANA), E_OK);
	KT_ASSERT_ER(conf_layout("qwerty"), E_OK);
	KT_ASSERT_ER(conf_line("SOFTKB_TRONLATIN", "1"), E_OK);
	KT_ASSERT_ER(win_make("{\"rect\":[60,40,460,180],\"attr\":3}", &tw, &tk, &twid), E_OK);
	if ( tk <= 0 ) goto out;
	KT_ASSERT_ER(wm_focus(twid), E_OK);
	(void)pump(100, -1, NULL);
	hd = knl_hid_ext_attach(TRUE, NULL, 0, (CONST UB *)"kt-skb");
	KT_ASSERT(hd > 0);
	if ( hd <= 0 ) goto out;

	pid = start(&wid);
	if ( pid <= 0 ) KT_SKIP("no program object of ソフトウェアキーボード");
	if ( wid <= 0 ) goto stop;
	KT_ASSERT_EQ(phys_key(hd, 0x07, twid, &mods), 0x07);
	KT_ASSERT_EQ(wm_key_char(0x07, mods), 'd');

	/* the TRON keyboard chosen: the machine's layouts left as they are */
	KT_ASSERT(choose_layout(wid, 1));
	wid = win_wide(TR_W0, 5000);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) goto stop;
	tk_dly_tsk(800);
	KT_ASSERT_ER(km_get(&now), E_OK);
	tm_printf((UB *)"  the machine's layouts with the TRON keyboard: latin %d kana %d\n", now.latin, now.kana);
	KT_ASSERT_EQ(now.latin, KM_LATIN_KEYS);
	KT_ASSERT_EQ(now.kana, KM_KANA_JIS);

	/* the keyboard plugged in: as the machine's layouts say */
	code = phys_key(hd, 0x07, twid, &mods);
	KT_ASSERT_EQ(code, 0x07);
	KT_ASSERT_EQ(wm_key_char(code, mods), 'd');
	code = phys_key(hd, 0x04, twid, &mods);
	KT_ASSERT_EQ(code, 0x04);
	KT_ASSERT_EQ(km_label(KM_L_KANA, code, mods, b, sizeof(b)), 0);
	KT_ASSERT_EQ(kc_kana_key_m(code, mods), kc_kana_key(0x04, FALSE));

	/* the screen keyboard's keys: in its own layouts */
	code = tron_on(wid, &tr_l43, twid, &mods);
	KT_ASSERT_EQ(code, 0x07);
	KT_ASSERT_EQ(wm_key_char(code, mods), 'e');
	code = tron_on(wid, &tr_l73, twid, &mods);
	KT_ASSERT_EQ(code, 0x04);
	KT_ASSERT(km_label(KM_L_KANA, code, mods, b, sizeof(b)) > 0 && b[0] == (char)0xE3 && b[1] == (char)0x81
		  && b[2] == (char)0x9F);		/* た */
	KT_ASSERT_EQ(kc_kana_key_m(code, mods), 0x243F);

	/* the English letters again: the machine's layouts as they were all along */
	KT_ASSERT(choose_layout(wid, 0));
	(void)pump(1000, -1, NULL);
	wid = win_named(SK_TITLE, 5000);
	KT_ASSERT_ER(km_get(&now), E_OK);
	KT_ASSERT_EQ(now.latin, KM_LATIN_KEYS);
	KT_ASSERT_EQ(now.kana, KM_KANA_JIS);
	KT_ASSERT_EQ(phys_key(hd, 0x07, twid, &mods), 0x07);
	KT_ASSERT_EQ(wm_key_char(0x07, mods), 'd');
	if ( wid > 0 ) {
		close_by_menu(wid, pid);
		pid = 0;
	}
stop:
	if ( pid > 0 ) {
		T_PSTS	psts;

		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
out:
	if ( hd > 0 ) knl_hid_ext_detach(hd);
	(void)pump(100, -1, NULL);
	(void)km_set(&was, KM_SET_LATIN | KM_SET_KANA);
	conf_back();
	if ( tk > 0 ) ob_cls_obj(tk);
	(void)ob_del_obj(&tw);
}

/* The program started with "selftest": its tables, the layout kept and the calls from a process */
LOCAL void test_selftest( void )
{
	TS_UUID		prog, pu;
	T_OBCRE		c;
	T_PSTS		psts;
	T_KMSTATE	was, now;
	ID		pid;
	ER		er;

	KT_ASSERT_ER(km_get(&was), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_SOFTKB, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of ソフトウェアキーボード");
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
	/* the layouts it switched and put back: as they were */
	KT_ASSERT_ER(km_get(&now), E_OK);
	KT_ASSERT(now.latin == was.latin && now.kana == was.kana);
	if ( now.latin != was.latin || now.kana != was.kana ) {
		(void)km_set(&was, KM_SET_LATIN | KM_SET_KANA);
	}
}

EXPORT void ktest_skb( void )
{
	tm_printf((UB *)"\n[skb] ソフトウェアキーボード (design 11.31)\n");
	KT_RUN_SCREEN(test_program);
	KT_RUN_SCREEN(test_finger);
	KT_RUN_SCREEN(test_tron);
	KT_RUN_SCREEN(test_utron);
	KT_RUN_SCREEN(test_apart);
	KT_RUN_SCREEN(test_wins);
	KT_RUN_SCREEN(test_selftest);
}
