/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_fault.c
 *	A process ended from the keyboard, and one ended by an exception
 *	told in a dialog (design 5.5, 11.1), with the desktop running as it
 *	runs on the machine and the keys and the pointer put in as the
 *	devices would report them (kt_inject, through 入力). The processes are
 *	tests/uprog/svcprog.c (/boot/SVCPROG.ELF).
 *
 *	- pause desk: Shift+Pause over a window of the system's own ends
 *	  nothing, and says there is no process to end.
 *	- pause proc: over the window of a process -- the one with the keys,
 *	  or, when no window has them, the one in front -- it ends that
 *	  process with TS_ABORT_TERM and its window goes; the key held and
 *	  repeating ends no second process.
 *	- pause sys: a process the system needs (TS_PRC_SYSTEM), and init,
 *	  are not ended.
 *	- fault abort, fault ill, fault iabt, fault kcall: a data abort, an
 *	  instruction that is none, a jump to an address not mapped, and an
 *	  address handed to the kernel that is not the process's, each shown
 *	  in the dialog プログラムの異常終了 with the right words, address and
 *	  exit code, the keys left where they were; 了解, Enter and a double
 *	  press on its pictogram close it.
 *	- fault many: six faults one after another make one dialog, which
 *	  counts the five it does not show.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/uuid.h>
#include "../../application/desktop/desktop.h"

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_KFAULT	11		/* as tests/uprog/svcprog.c */
#define SP_PAUSEWIN	25
#define SP_FAULT	26
#define FAULT_VA	"0x0000200000001230"

#define KEY_PAUSE	0x48
#define KEY_ENTER	0x28
#define FT_TITLE	"プログラムの異常終了"
#define LINES_MAX	1600

LOCAL BOOL	have_prog = FALSE;
LOCAL BOOL	up = FALSE;
LOCAL INT	own_wid = 0;		/* a window of the kernel's own, when the desktop showed none */
LOCAL UB	*lines;
LOCAL T_WMWIN	wr;			/* a window looked at, only to see that it is there */

/* ---------------------------------------------------------------- helpers */

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL has( CONST char *hay, CONST char *w )
{
	INT	i, j, n = s_len(hay), m = s_len(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( j = 0; j < m && hay[i + j] == w[j]; j++ ) ;
		if ( j == m ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL said( UD pos, CONST char *w, INT tries )
{
	UB	*b = (UB *)Kmalloc(4096);
	INT	i, n, k;

	if ( b == NULL ) return FALSE;
	for ( i = 0; i < tries; i++ ) {
		UD	p = pos;

		n = 0;
		while ( n < 4095 && ( k = tm_log_read(&p, b + n, 4095 - n) ) > 0 ) n += k;
		b[n] = 0;
		if ( has((CONST char *)b, w) ) {
			Kfree(b);
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	Kfree(b);
	return FALSE;
}

LOCAL UD log_now( void )
{
	UB	b[256];
	UD	pos = 0;

	while ( tm_log_read(&pos, b, sizeof(b)) > 0 ) ;
	return pos;
}

LOCAL ID start( UW mode, UW a1, UW a2, ATR atr )
{
	T_CPRC	cprc;
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	arg[1] = a1;
	arg[2] = a2;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = atr;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	return ts_cre_prc(SVCPROG, &cprc);
}

/* The window whose band says title, looked for up to tries tenths of a second; 0 none */
LOCAL INT window_titled( CONST char *title, INT tries )
{
	INT	wids[64], n, i, t;
	char	b[WM_TITLE_MAX];

	for ( t = 0; t < tries || t == 0; t++ ) {
		n = wm_list(wids, 64);
		for ( i = 0; i < n; i++ ) {
			if ( wm_title(wids[i], b, sizeof(b)) >= 0 && s_len(b) == s_len(title) && has(b, title) ) {
				return wids[i];
			}
		}
		if ( tries > 0 ) tk_dly_tsk(100);
	}
	return 0;
}

LOCAL INT count_titled( CONST char *title )
{
	INT	wids[64], n, i, c = 0;
	char	b[WM_TITLE_MAX];

	n = wm_list(wids, 64);
	for ( i = 0; i < n; i++ ) {
		if ( wm_title(wids[i], b, sizeof(b)) >= 0 && s_len(b) == s_len(title) && has(b, title) ) c++;
	}
	return c;
}

LOCAL void key( UINT code, UINT mods, UINT type )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.mods = mods;
	(void)kt_inject(&e);
	tk_dly_tsk(30);
}

LOCAL void shift_pause( void )
{
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_UP);
}

LOCAL void click( INT x, INT y )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = HID_EV_MOVE;
	e.x = x;
	e.y = y;
	(void)kt_inject(&e);
	tk_dly_tsk(30);
	e.type = HID_EV_BTN_DOWN;
	(void)kt_inject(&e);
	tk_dly_tsk(30);
	e.type = HID_EV_BTN_UP;
	(void)kt_inject(&e);
	tk_dly_tsk(30);
}

/* The message line says w, looked at for up to tries tenths of a second */
LOCAL BOOL msg_says( CONST char *w, INT tries )
{
	char	b[256];
	INT	t;

	for ( t = 0; t < tries; t++ ) {
		(void)wm_msg_text(b, sizeof(b));
		if ( has(b, w) ) return TRUE;
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  the message line says \"%s\", not \"%s\"\n", b, w);
	return FALSE;
}

LOCAL BOOL running( ID pid )
{
	T_RPRC	r;

	return (BOOL)( ts_ref_prc(pid, &r) >= E_OK && r.state == PS_RUNNING );
}

/* A window of the system's own in front, with the keys */
LOCAL INT kernel_window( void )
{
	DTWIN		*d = dt_first_win();
	T_DPRECT	o;
	INT		wid = ( d != NULL ) ? d->wid : 0;

	if ( wid <= 0 || wm_ref(wid, &wr) < E_OK ) {
		if ( own_wid <= 0 ) {
			o.left = 40;  o.top = 60;  o.right = 300;  o.bottom = 220;
			own_wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ktdesk");
		}
		wid = own_wid;
	}
	if ( wid > 0 ) {
		wm_raise(wid);
		wm_focus(wid);
		wm_composite();
	}
	return wid;
}

/* A process with a window in front, with the keys: its pid, and the window in *p_wid */
LOCAL ID window_process( ATR atr, INT *p_wid )
{
	ID	pid = start(SP_PAUSEWIN, 0, 0, atr);
	INT	wid = 0, t;

	*p_wid = 0;
	if ( pid <= 0 ) return pid;
	for ( t = 0; t < 50 && knl_own_list(TK_OWN_SEM, pid, NULL, 0) < 1; t++ ) tk_dly_tsk(100);
	wid = window_titled("ktpause", 30);
	if ( wid > 0 ) {
		wm_raise(wid);
		wm_focus(wid);
		wm_composite();
	}
	*p_wid = wid;
	return pid;
}

/* The dialog's lines, and whether they say w */
LOCAL BOOL dialog_says( CONST char *w )
{
	(void)dt_fault_lines(lines, LINES_MAX);
	if ( has((CONST char *)lines, w) ) return TRUE;
	tm_printf((UB *)"  the dialog does not say \"%s\":\n%s", w, lines);
	return FALSE;
}

/* The dialog's 了解, pressed */
LOCAL void press_ok( INT wid )
{
	T_WMWIN	w;

	if ( wm_ref(wid, &w) < E_OK ) return;
	click(w.work.right - DT_PN_IN - 61, w.work.bottom - DT_PN_IN - 23);
}

LOCAL BOOL dialog_gone( void )
{
	INT	t;

	for ( t = 0; t < 30 && ( dt_fault_wid() != 0 || window_titled(FT_TITLE, 0) > 0 ); t++ ) {
		tk_dly_tsk(100);
	}
	return (BOOL)( dt_fault_wid() == 0 && window_titled(FT_TITLE, 0) == 0 );
}

/* A process that faults as svcprog's SP_FAULT kind says, waited for; the dialog's window */
LOCAL INT fault_run( UW kind, INT want, ID *p_pid )
{
	T_PSTS	psts;
	ID	pid;

	pid = start(SP_FAULT, kind, 0, 0);
	*p_pid = pid;
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return 0;
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, want);
	return window_titled(FT_TITLE, 30);
}

/* ---------------------------------------------------------------- the tests */

LOCAL void test_setup( void )
{
	T_FSTAT	st;
	UD	pos;
	INT	i;

	lines = (UB *)Kmalloc(LINES_MAX);
	if ( lines == NULL ) KT_SKIP("no memory");
	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	have_prog = TRUE;
	pos = log_now();
	KT_ASSERT_ER(dt_start(), E_OK);
	up = said(pos, "TessronOS desktop: start", 100);
	KT_ASSERT(up);
	for ( i = 0; i < 30 && dt_first_win() == NULL; i++ ) tk_dly_tsk(100);
	tk_dly_tsk(500);
}

LOCAL void test_pause_desk( void )
{
	ID	pids[8];
	INT	wid, n0, n1;

	if ( !up ) KT_SKIP("no desktop");
	wid = kernel_window();
	KT_ASSERT(wid > 0);
	KT_ASSERT_EQ(wm_focused(), wid);
	n0 = knl_prc_list(pids, 8);
	(void)wm_msg_put(NULL);
	shift_pause();
	KT_ASSERT(msg_says("強制終了できるプロセスがありません", 30));
	n1 = knl_prc_list(pids, 8);
	KT_ASSERT_EQ(n1, n0);
	KT_ASSERT_ER(wm_ref(wid, &wr), E_OK);		/* the window stays, and the desktop goes on */
	KT_ASSERT_EQ(wm_focused(), wid);
}

LOCAL void test_pause_proc( void )
{
	T_PSTS	psts;
	char	w[48];
	INT	wa, wb;
	ID	a, b;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");

	/* the one with the keys */
	a = window_process(0, &wa);
	KT_ASSERT(a > 0 && wa > 0);
	if ( a <= 0 || wa <= 0 ) return;
	KT_ASSERT_EQ(wm_focused(), wa);
	(void)wm_msg_put(NULL);
	shift_pause();
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(a, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_TERM);
	KT_ASSERT(window_titled("ktpause", 0) == 0);
	(void)tm_sprintf((UB *)w, (CONST UB *)"SVCPROG.ELF(pid %d)を強制終了しました", a);
	KT_ASSERT(msg_says(w, 30));

	/* none has the keys: the one in front */
	b = window_process(0, &wb);
	KT_ASSERT(b > 0 && wb > 0);
	if ( b <= 0 || wb <= 0 ) return;
	(void)wm_focus(0);
	KT_ASSERT_EQ(wm_focused(), 0);
	shift_pause();
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(b, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_TERM);

	/* held down and repeating: the first press ends one, and no more */
	a = window_process(0, &wa);
	b = window_process(0, &wb);			/* in front, with the keys; a behind it */
	KT_ASSERT(a > 0 && b > 0 && wb > 0);
	if ( a <= 0 || b <= 0 ) return;
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_UP);
	KT_ASSERT_ER(ts_wai_prc(b, &psts, 5000), E_OK);
	tk_dly_tsk(300);
	KT_ASSERT(running(a));
	(void)ts_ter_prc(a, -1);
	(void)ts_wai_prc(a, &psts, 5000);
	KT_ASSERT_EQ(wm_self_check(), 0);
}

LOCAL void test_pause_sys( void )
{
	T_PSTS	psts;
	T_RPRC	r;
	INT	wid;
	ID	pid, ip;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	pid = window_process(TS_PRC_SYSTEM, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 ) return;
	(void)wm_msg_put(NULL);
	shift_pause();
	KT_ASSERT(msg_says("はシステムのプロセスなので強制終了できません", 30));
	KT_ASSERT(running(pid));
	KT_ASSERT_ER(wm_ref(wid, &wr), E_OK);
	KT_ASSERT_ER(knl_prc_force_end(pid), E_OACV);

	/* init, running or not, is never ended from the keyboard */
	ip = knl_init_pid();
	knl_memset(&r, 0, sizeof(r));
	if ( ip > 0 ) {
		BOOL	was = (BOOL)( ts_ref_prc(ip, &r) >= E_OK );
		UINT	st = r.state;

		KT_ASSERT(knl_prc_force_end(ip) < E_OK);
		if ( was ) {
			KT_ASSERT_ER(ts_ref_prc(ip, &r), E_OK);
			KT_ASSERT_EQ(r.state, st);
		}
	}
	KT_ASSERT_ER(ts_ter_prc(pid, -1), E_OK);
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
}

LOCAL void test_fault_abort( void )
{
	char	w[48];
	INT	wid, kw;
	ID	pid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	kw = kernel_window();
	wid = fault_run(0, TS_ABORT_FAULT, &pid);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT_EQ(dt_fault_wid(), wid);
	KT_ASSERT_EQ(count_titled(FT_TITLE), 1);
	(void)tm_sprintf((UB *)w, (CONST UB *)"SVCPROG.ELF(pid %d)", pid);
	KT_ASSERT(dialog_says(w));
	KT_ASSERT(dialog_says("プロセッサの例外"));
	KT_ASSERT(dialog_says("例外 データアボート(マップされていないアドレスへの書込み)\n"));
	KT_ASSERT(dialog_says("(EC 0x24)"));
	KT_ASSERT(dialog_says("アドレス(FAR) " FAULT_VA));
	KT_ASSERT(dialog_says("PC(ELR) 0x0000000000"));
	KT_ASSERT(dialog_says("LR 0x0000000000"));
	KT_ASSERT(dialog_says("終了コード -256(TS_ABORT_FAULT)"));
	KT_ASSERT_EQ(wm_focused(), kw);			/* the keys stay where they were */
	press_ok(wid);
	KT_ASSERT(dialog_gone());
}

LOCAL void test_fault_ill( void )
{
	INT	wid;
	ID	pid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	wid = fault_run(1, TS_ABORT_ILL, &pid);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT(dialog_says("例外 未定義命令\n"));
	KT_ASSERT(dialog_says("(EC 0x00)"));
	KT_ASSERT(dialog_says("終了コード -257(TS_ABORT_ILL)"));
	/* given the keys by a press on it, Enter closes it */
	{
		T_WMWIN	w;

		KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
		click(w.work.left + 20, w.work.top + 20);
	}
	KT_ASSERT_EQ(wm_focused(), wid);
	key(KEY_ENTER, 0, HID_EV_KEY_DOWN);
	key(KEY_ENTER, 0, HID_EV_KEY_UP);
	KT_ASSERT(dialog_gone());
}

LOCAL void test_fault_iabt( void )
{
	T_WMWIN	w;
	INT	wid, i;
	ID	pid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	wid = fault_run(2, TS_ABORT_FAULT, &pid);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT(dialog_says("例外 命令アボート(マップされていないアドレスの実行)\n"));
	KT_ASSERT(dialog_says("(EC 0x20)"));
	KT_ASSERT(dialog_says("アドレス(FAR) " FAULT_VA));
	KT_ASSERT(dialog_says("PC(ELR) " FAULT_VA));
	/* a double press on its pictogram closes it */
	KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
	for ( i = 0; i < 2; i++ ) {
		INT	x, y, got = 0;
		UINT	bar;

		/* the pictogram: the first place along the band that is one */
		for ( x = w.outer.left + 2; x < w.outer.left + 60; x++ ) {
			y = ( w.outer.top + w.work.top ) / 2;
			if ( wm_part_at(x, y, &got, &bar) == WM_PART_PICT && got == wid ) break;
		}
		click(x + 4, ( w.outer.top + w.work.top ) / 2);
	}
	KT_ASSERT(dialog_gone());
}

LOCAL void test_fault_kcall( void )
{
	T_PSTS	psts;
	TS_UUID	u;
	INT	wid;
	ID	pid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	pid = start(SP_KFAULT, 0, 0, 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_FAULT);
	if ( ob_fnd_nam((CONST UB *)"ktkfault", &u) >= E_OK ) (void)ob_del_obj(&u);
	wid = window_titled(FT_TITLE, 30);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	KT_ASSERT(dialog_says("システムコールに不正なアドレスを渡した"));
	KT_ASSERT(dialog_says("例外 データアボート"));
	KT_ASSERT(dialog_says("PC(カーネル) 0xffff"));
	KT_ASSERT(dialog_says("終了コード -256(TS_ABORT_FAULT)"));
	press_ok(wid);
	KT_ASSERT(dialog_gone());
}

#define MANY	6

LOCAL void test_fault_many( void )
{
	T_PSTS	psts;
	ID	pid[MANY];
	INT	i, t, wid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	for ( i = 0; i < MANY; i++ ) {
		pid[i] = start(SP_FAULT, (UW)( i % 2 ), 0, 0);
		KT_ASSERT(pid[i] > 0);
	}
	for ( i = 0; i < MANY; i++ ) {
		if ( pid[i] > 0 ) KT_ASSERT_ER(ts_wai_prc(pid[i], &psts, 10000), E_OK);
	}
	wid = window_titled(FT_TITLE, 30);
	KT_ASSERT(wid > 0);
	for ( t = 0; t < 30; t++ ) {
		(void)dt_fault_lines(lines, LINES_MAX);
		if ( has((CONST char *)lines, "このほかに5件") ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(dialog_says("このほかに5件、例外で終了したプログラムがあります"));
	KT_ASSERT_EQ(count_titled(FT_TITLE), 1);
	press_ok(wid);
	KT_ASSERT(dialog_gone());
	KT_ASSERT_EQ(wm_self_check(), 0);
}

EXPORT void ktest_fault( void )
{
	KT_RUN(test_setup);
	KT_RUN(test_pause_desk);
	KT_RUN(test_pause_proc);
	KT_RUN(test_pause_sys);
	KT_RUN(test_fault_abort);
	KT_RUN(test_fault_ill);
	KT_RUN(test_fault_iabt);
	KT_RUN(test_fault_kcall);
	KT_RUN(test_fault_many);
	if ( up ) {
		dt_quit();
		up = FALSE;
	}
	if ( own_wid > 0 ) {
		(void)wm_close(own_wid);
		own_wid = 0;
	}
	if ( lines != NULL ) {
		Kfree(lines);
		lines = NULL;
	}
	wm_update();
}
