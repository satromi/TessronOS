/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_fq.c
 *	強制終了 (outer_kernel/wm/fquit.c, design 5.5.1, 5.5.3 - 5.5.5), with
 *	the desktop running as it runs on the machine and the keys and the
 *	pointer put in as the devices report them (knl_hid_inject). The
 *	processes are tests/uprog/svcprog.c (/boot/SVCPROG.ELF).
 *
 *	- front: Shift+Pause over the window of a process ends it.
 *	- lost up: a press whose let go never comes is taken as the key
 *	  held for two seconds, and as a new press after that.
 *	- stalled: with the desktop's loop stopped, Shift+Pause still ends
 *	  the process in front, and the message line says so.
 *	- list: with the desktop stopped, Ctrl+Shift+Pause opens the window
 *	  of the processes, its panel worked as the input is handed to it;
 *	  one the system needs, chosen with the keys, is not ended by
 *	  Shift+Pause, and one with no window is ended by 〈強制終了〉; Esc
 *	  closes it.
 *	- stuck call: a process inside a call that waits for a lock the
 *	  test holds is "終了の途中" when Shift+Pause ends it once, and
 *	  stopped where it stands when it ends it again (knl_prc_kill); its
 *	  pages are kept.
 *	- serial cut: a read of the serial port that would wait for ever
 *	  comes out when its wait is cut (tk_rel_wai).
 *	- delegate: a panel the desktop shows for a process (the
 *	  administrator's password, DT_RQ_ADMIN), and a window the desktop
 *	  opens while doing what a process asked, are that process's.
 *	- sys: a process the system needs is not ended.
 *	- restart: with the desktop stuck and its window in front, the
 *	  first Shift+Pause says what will be lost and the second starts the
 *	  desktop again; the processes and their windows stay, and the
 *	  desktop and 強制終了 work after it. Then again from the window's
 *	  〈デスクトップを立ち上げ直す〉 and 〈立ち上げ直す〉 of the panel
 *	  that asks.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/ser.h>
#include <ts/uuid.h>
#include <ts/lockup.h>
#include <ts/fquit.h>
#include "../../application/desktop/desktop.h"

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_PAUSEWIN	25		/* as tests/uprog/svcprog.c */
#define SP_BGWAIT	33
#define SP_OBSTUCK	34
#define SP_ADMINASK	35

#define KEY_PAUSE	0x48
#define KEY_ESC		0x29
#define KEY_HOME	0x4A
#define KEY_DOWN	0x51
#define SILENT_MS	1500		/* the desktop's loop away this long is stuck, for the run */

/* The places of the window's buttons, as the data box SYSDEF_BOX_FQUIT lays them out */
#define FQW_BTN_Y	320		/* the middle of the row of push buttons */
#define FQW_RESTART_X	132		/* 〈デスクトップを立ち上げ直す〉 */
#define FQW_END_X	400		/* 〈強制終了〉 */
#define FQW_ASK_X	296		/* 〈立ち上げ直す〉 of the panel asking */
#define FQW_ASK_Y	126

LOCAL BOOL	have_prog = FALSE;
LOCAL BOOL	up = FALSE;
LOCAL T_WMWIN	wr;

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

LOCAL ID start( UW mode, ATR atr )
{
	T_CPRC	cprc;
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = atr;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	return ts_cre_prc(SVCPROG, &cprc);
}

/* The process made the semaphore that says it is ready */
LOCAL BOOL is_ready( ID pid )
{
	INT	t;

	for ( t = 0; t < 50 && knl_own_list(TK_OWN_SEM, pid, NULL, 0) < 1; t++ ) tk_dly_tsk(100);
	return (BOOL)( knl_own_list(TK_OWN_SEM, pid, NULL, 0) >= 1 );
}

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

/*
 * Whether no window has that title within tries tenths of a second. The
 * window of 強制終了 is opened by its own task, so taking it away asks
 * that task to close it (a window its program opened is the program's
 * to close): the list is said to be down before the window has gone.
 */
LOCAL BOOL window_gone( CONST char *title, INT tries )
{
	INT	t;

	for ( t = 0; t < tries; t++ ) {
		if ( window_titled(title, 0) == 0 ) return TRUE;
		tk_dly_tsk(100);
	}
	return (BOOL)( window_titled(title, 0) == 0 );
}

LOCAL void put( UINT type, UINT code, UINT mods, INT x, INT y )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.mods = mods;
	e.x = x;
	e.y = y;
	(void)knl_hid_inject(0, &e);
	tk_dly_tsk(30);
}

LOCAL void key( UINT code, UINT mods, UINT type )
{
	put(type, code, mods, 0, 0);
}

LOCAL void press( UINT code, UINT mods )
{
	key(code, mods, HID_EV_KEY_DOWN);
	key(code, mods, HID_EV_KEY_UP);
}

LOCAL void shift_pause( void )
{
	press(KEY_PAUSE, HID_MOD_LSHIFT);
}

LOCAL void ctrl_shift_pause( void )
{
	press(KEY_PAUSE, HID_MOD_LSHIFT | HID_MOD_LCTRL);
}

LOCAL void click( INT x, INT y )
{
	put(HID_EV_MOVE, 0, 0, x, y);
	put(HID_EV_BTN_DOWN, 0, 0, x, y);
	put(HID_EV_BTN_UP, 0, 0, x, y);
}

/* 強制終了 said w, looked at for up to tries tenths of a second */
LOCAL BOOL fq_says( CONST char *w, INT tries )
{
	T_FQSTAT	st;
	INT		t;

	for ( t = 0; t < tries; t++ ) {
		knl_fq_stat(&st);
		if ( has(st.text, w) ) return TRUE;
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  強制終了 said \"%s\", not \"%s\"\n", st.text, w);
	return FALSE;
}

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

LOCAL INT ended( ID pid, TMO tmo )
{
	T_PSTS	psts;

	psts.exitcd = 0;
	if ( ts_wai_prc(pid, &psts, tmo) < E_OK ) {
		return 1;
	}
	return psts.exitcd;
}

LOCAL void gone( ID pid )
{
	T_PSTS	psts;

	if ( pid > 0 && ts_ter_prc(pid, -1) >= E_OK ) {
		(void)ts_wai_prc(pid, &psts, 5000);
	} else if ( pid > 0 ) {
		(void)ts_wai_prc(pid, &psts, 1000);
	}
}

/* A process with a window in front, with the keys: its pid, and the window in *p_wid */
LOCAL ID window_process( ATR atr, INT *p_wid )
{
	ID	pid = start(SP_PAUSEWIN, atr);
	INT	wid = 0;

	*p_wid = 0;
	if ( pid <= 0 ) return pid;
	(void)is_ready(pid);
	wid = window_titled("ktpause", 30);
	if ( wid > 0 ) {
		wm_raise(wid);
		wm_focus(wid);
		wm_composite();
	}
	*p_wid = wid;
	return pid;
}

LOCAL INT kernel_window( void )
{
	DTWIN	*d = dt_first_win();
	INT	wid = ( d != NULL ) ? d->wid : 0;

	if ( wid > 0 ) {
		wm_raise(wid);
		wm_focus(wid);
		wm_composite();
	}
	return wid;
}

LOCAL BOOL wait_stuck( BOOL want, INT tries )
{
	INT	t;

	for ( t = 0; t < tries; t++ ) {
		if ( knl_wdog_stuck(NULL, NULL) == want ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/* The window of the processes up (or down), looked at for tries tenths of a second */
LOCAL BOOL list_up( BOOL want, INT tries )
{
	T_FQSTAT	st;
	INT		t;

	for ( t = 0; t < tries; t++ ) {
		knl_fq_stat(&st);
		if ( ( st.wid > 0 ) == want ) return TRUE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/* The row of pid chosen with the keys: Home, then Down as far as it is */
LOCAL BOOL choose( ID pid )
{
	T_FQSTAT	st;
	INT		i, t, k = -1;

	for ( t = 0; t < 30 && k < 0; t++ ) {
		knl_fq_stat(&st);
		for ( i = 0; i < st.nrow; i++ ) {
			if ( st.pid[i] == pid ) k = i;
		}
		if ( k < 0 ) tk_dly_tsk(100);
	}
	if ( k < 0 ) {
		tm_printf((UB *)"  pid %d is not in the list (%d rows)\n", pid, st.nrow);
		return FALSE;
	}
	press(KEY_HOME, 0);
	for ( i = 0; i < k; i++ ) press(KEY_DOWN, 0);
	/*
	 * The keys go through the window's own task, which an emulated
	 * processor (QEMU virt) runs some hundred ms behind: looked at
	 * until the line is chosen, for up to 2 s
	 */
	for ( t = 0; t < 40; t++ ) {
		tk_dly_tsk(50);
		knl_fq_stat(&st);
		if ( st.sel == pid ) break;
	}
	return (BOOL)( st.sel == pid );
}

/* The desktop's loop stopped (1, 2) or let go (0), and waited for to be seen so */
LOCAL BOOL stall( INT how )
{
	dt_test_stall = how;
	if ( how != 0 ) {
		return wait_stuck(TRUE, 60);
	}
	return wait_stuck(FALSE, 60);
}

/* ---------------------------------------------------------------- the tests */

LOCAL void test_setup( void )
{
	T_FSTAT	st;
	INT	i;

	knl_lockup_tune(0, 0, SILENT_MS);
	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	have_prog = TRUE;
	KT_ASSERT_ER(dt_start(), E_OK);
	for ( i = 0; i < 100 && dt_first_win() == NULL; i++ ) tk_dly_tsk(100);
	up = (BOOL)( dt_first_win() != NULL );
	KT_ASSERT(up);
	tk_dly_tsk(500);
}

LOCAL void test_front( void )
{
	char	w[64];
	INT	wa;
	ID	a;

	if ( !up ) KT_SKIP("no desktop");
	a = window_process(0, &wa);
	KT_ASSERT(a > 0 && wa > 0);
	if ( a <= 0 || wa <= 0 ) return;
	(void)wm_msg_put(NULL);
	shift_pause();
	KT_ASSERT_EQ(ended(a, 5000), TS_ABORT_TERM);
	KT_ASSERT(window_titled("ktpause", 0) == 0);
	(void)tm_sprintf((UB *)w, (CONST UB *)"SVCPROG.ELF(pid %d)を強制終了しました", a);
	KT_ASSERT(msg_says(w, 30));
}

LOCAL void test_lost_up( void )
{
	INT	wa, wb;
	ID	a, b;

	if ( !up ) KT_SKIP("no desktop");
	a = window_process(0, &wa);
	b = window_process(0, &wb);			/* in front; a behind it */
	KT_ASSERT(a > 0 && b > 0 && wb > 0);
	if ( a <= 0 || b <= 0 ) { gone(a); gone(b); return; }
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);	/* its let go never comes */
	KT_ASSERT_EQ(ended(b, 5000), TS_ABORT_TERM);
	if ( wa > 0 ) {
		wm_focus(wa);
		wm_raise(wa);
	}
	tk_dly_tsk(300);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);	/* soon after: the key held */
	tk_dly_tsk(500);
	KT_ASSERT(running(a));
	tk_dly_tsk(2200);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_DOWN);	/* later: a new press */
	KT_ASSERT_EQ(ended(a, 5000), TS_ABORT_TERM);
	key(KEY_PAUSE, HID_MOD_LSHIFT, HID_EV_KEY_UP);
	gone(a);
}

LOCAL void test_stalled( void )
{
	char	w[64];
	INT	wa;
	ID	a;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	a = window_process(0, &wa);
	KT_ASSERT(a > 0 && wa > 0);
	if ( a <= 0 ) return;
	KT_ASSERT(stall(1));
	(void)wm_msg_put(NULL);
	shift_pause();
	KT_ASSERT_EQ(ended(a, 5000), TS_ABORT_TERM);
	(void)tm_sprintf((UB *)w, (CONST UB *)"SVCPROG.ELF(pid %d)を強制終了しました", a);
	KT_ASSERT(fq_says(w, 30));
	KT_ASSERT(msg_says(w, 30));			/* written by 強制終了's own task */
	KT_ASSERT(knl_wdog_stuck(NULL, NULL));		/* the desktop is still stopped */
}

LOCAL void test_list( void )
{
	T_FQSTAT	st;
	ID		b, c;
	INT		t;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	if ( !knl_wdog_stuck(NULL, NULL) ) KT_ASSERT(stall(1));
	b = start(SP_BGWAIT, 0);
	c = start(SP_BGWAIT, TS_PRC_SYSTEM);
	KT_ASSERT(b > 0 && c > 0);
	if ( b <= 0 || c <= 0 ) { gone(b); gone(c); return; }
	KT_ASSERT(is_ready(b) && is_ready(c));
	ctrl_shift_pause();
	KT_ASSERT(list_up(TRUE, 30));
	knl_fq_stat(&st);
	KT_ASSERT(st.wid > 0 && window_titled("強制終了", 0) == st.wid);
	for ( t = 0; t < 30 && !st.grab; t++ ) {
		tk_dly_tsk(100);
		knl_fq_stat(&st);
	}
	KT_ASSERT(st.grab);				/* the desktop reads no input: the window takes it */
	tm_printf((UB *)"  %d processes listed\n", st.nrow);

	/* one the system needs: chosen with the keys, and not ended (Shift+Pause ends the one chosen) */
	KT_ASSERT(choose(c));
	shift_pause();
	KT_ASSERT(fq_says("はシステムのプロセスなので強制終了できません", 30));
	KT_ASSERT(running(c));

	/* one with no window: chosen, and ended with 〈強制終了〉 */
	KT_ASSERT(choose(b));
	knl_fq_stat(&st);
	if ( st.wid > 0 && wm_ref(st.wid, &wr) >= E_OK ) {
		click(wr.work.left + FQW_END_X, wr.work.top + FQW_BTN_Y);
	}
	KT_ASSERT_EQ(ended(b, 5000), TS_ABORT_TERM);

	press(KEY_ESC, 0);
	KT_ASSERT(list_up(FALSE, 30));
	KT_ASSERT(window_gone("強制終了", 30));
	KT_ASSERT(knl_wdog_stuck(NULL, NULL));		/* all of it with the desktop stopped */
	KT_ASSERT_ER(ts_ter_prc(c, -1), E_OK);
	KT_ASSERT_EQ(ended(c, 5000), -1);
	KT_ASSERT(stall(0));
}

LOCAL void test_stuck_call( void )
{
	char	w[64];
	T_RPRC	r;
	T_RTSK	rt;
	ID	p, mt;
	INT	t, wid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	p = start(SP_OBSTUCK, 0);
	KT_ASSERT(p > 0 && is_ready(p));
	if ( p <= 0 ) return;
	KT_ASSERT_ER(ts_ref_prc(p, &r), E_OK);
	mt = r.maintsk;
	wid = window_titled("ktstuck", 30);
	KT_ASSERT(wid > 0);
	if ( wid > 0 ) {
		wm_raise(wid);
		wm_focus(wid);
	}

	/* inside a call, waiting for the lock the test holds */
	knl_ob_test_hold(TRUE);
	for ( t = 0; t < 30; t++ ) {
		if ( tk_ref_tsk(mt, &rt) >= E_OK && ( rt.tskstat & TTS_WAI ) != 0 && rt.tskwait == TTW_MTX ) break;
		tk_dly_tsk(20);
	}
	KT_ASSERT(( rt.tskstat & TTS_WAI ) != 0 && rt.tskwait == TTW_MTX);

	/* once: it cannot end where it is */
	shift_pause();
	KT_ASSERT(fq_says("は終了の途中です", 30));
	KT_ASSERT_ER(ts_ref_prc(p, &r), E_OK);
	KT_ASSERT_EQ(r.state, PS_EXITING);

	/* again, a while later: stopped where it stands */
	tk_dly_tsk(1300);
	shift_pause();
	for ( t = 0; t < 40; t++ ) {
		if ( tk_ref_tsk(mt, &rt) < E_OK || rt.tskstat == TTS_DMT ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(tk_ref_tsk(mt, &rt) < E_OK || rt.tskstat == TTS_DMT);
	knl_ob_test_hold(FALSE);			/* what it had open closes now */
	KT_ASSERT_EQ(ended(p, 10000), TS_ABORT_TERM);
	(void)tm_sprintf((UB *)w, (CONST UB *)"(pid %d)の待ちを切り、強制的に終わらせました", p);
	KT_ASSERT(fq_says(w, 30));
	KT_ASSERT(window_titled("ktstuck", 10) == 0);
}

/* A read of the second port that would wait for ever, in a task of its own */
LOCAL volatile INT	ser_done;

LOCAL void ser_reader( INT stacd, void *exinf )
{
	UB	b[16];
	SZ	asz = 0;
	ID	dd = (ID)stacd;

	ser_done = ( tk_srea_dev(dd, 0, b, sizeof(b), &asz) >= E_OK ) ? 1 : 2;
	tk_exd_tsk();
}

LOCAL void test_serial_cut( void )
{
	TMO	tmo = TMO_FEVR;
	SZ	asz;
	ID	dd, tid;
	INT	t;

	dd = tk_opn_dev((UB *)"serb", TD_UPDATE);
	if ( dd <= 0 ) KT_SKIP("no second port");
	KT_ASSERT_ER(tk_swri_dev(dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz), E_OK);
	ser_done = 0;
	tid = kt_cre_tsk((FP)ser_reader, KT_PRI_HIGH, NULL);
	KT_ASSERT(tid > 0);
	if ( tid <= 0 ) { tk_cls_dev(dd, 0); return; }
	KT_ASSERT_ER(tk_sta_tsk(tid, (INT)dd), E_OK);
	tk_dly_tsk(1500);
	KT_ASSERT_EQ(ser_done, 0);			/* nothing comes: it waits */
	KT_ASSERT_ER(tk_rel_wai(tid), E_OK);
	for ( t = 0; t < 20 && ser_done == 0; t++ ) {
		tk_dly_tsk(50);
		(void)tk_rel_wai(tid);			/* between two waits, the next one */
	}
	KT_ASSERT(ser_done != 0);
	tmo = TMO_POL;
	(void)tk_swri_dev(dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz);
	tk_cls_dev(dd, 0);
	tk_dly_tsk(50);
	(void)tk_del_tsk(tid);
}

/* A program object of SVCPROG, started as the desktop starts one: as 利用者 */
LOCAL ER make_prog( TS_UUID *prog )
{
	CONST char	*meta = "{\"name\":\"ktfq\",\"refCount\":0,"
				"\"tessronos\":{\"exec\":{\"record\":0,\"arch\":\"aarch64\"}}}";
	T_OBCRE		c;
	UB		*buf;
	SZ		asz;
	INT		fd, n, total = 0, recno;
	ID		k;
	ER		er = E_OK;

	fd = fs_open(SVCPROG, O_RDONLY);
	if ( fd < 0 ) return E_NOEXS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)meta;
	c.jsonsz = s_len(meta);
	er = ob_cre_obj(&c, prog);
	if ( er < E_OK ) {
		fs_close(fd);
		return er;
	}
	k = ob_opn_obj(prog, OB_OP_R | OB_OP_W);
	buf = (UB *)Kmalloc(4096);
	if ( k <= 0 || buf == NULL || ob_apd_rec(k, OB_RT_PROG, 0, &recno) < E_OK ) {
		er = E_OBJ;
	}
	while ( er >= E_OK && ( n = fs_read(fd, buf, 4096) ) > 0 ) {
		er = ob_wri_rec(k, recno, total, buf, n, &asz);
		total += n;
	}
	fs_close(fd);
	if ( buf != NULL ) Kfree(buf);
	if ( k > 0 ) (void)ob_cls_obj(k);
	if ( er < E_OK ) (void)ob_del_obj(prog);
	return er;
}

LOCAL ID start_obj( CONST TS_UUID *prog, UW mode )
{
	T_OBCRE	c;
	TS_UUID	pu;
	UW	arg[8];
	ER	er;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = *prog;
	c.arg = arg;
	c.argsz = sizeof(arg);
	er = ob_cre_obj(&c, &pu);
	return ( er < E_OK ) ? (ID)er : knl_prc_of_uuid(&pu);
}

/* A popup in front that was opened for pid, looked for up to tries tenths of a second; 0 none */
LOCAL INT panel_for( ID pid, INT tries )
{
	INT	pops[8], np, wid, i, t;
	ID	mk, fr;

	for ( t = 0; t < tries; t++ ) {
		if ( knl_wm_front_pick(500, &wid, pops, 8, &np) >= E_OK ) {
			for ( i = 0; i < np; i++ ) {
				if ( knl_wmobj_who(pops[i], 500, &mk, &fr) >= E_OK && fr == pid ) return pops[i];
			}
		}
		tk_dly_tsk(100);
	}
	return 0;
}

LOCAL void test_delegate( void )
{
	T_DPRECT	o;
	TS_UUID		prog;
	char		w[64];
	INT		pw, kw, ow;
	ID		p, q;
	ER		er;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");

	/* the administrator's password asked for a process, over a window of the desktop's */
	er = make_prog(&prog);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) return;
	kw = kernel_window();
	p = start_obj(&prog, SP_ADMINASK);
	KT_ASSERT(p > 0);
	if ( p > 0 ) {
		pw = panel_for(p, 100);
		KT_ASSERT(pw > 0);
		KT_ASSERT_EQ(wm_focused(), kw);		/* the keys are the desktop's window's */
		(void)wm_msg_put(NULL);
		shift_pause();
		KT_ASSERT(ended(p, 10000) != 1);
		(void)tm_sprintf((UB *)w, (CONST UB *)"(pid %d)を強制終了しました", p);
		KT_ASSERT(fq_says(w, 30));
		press(KEY_ESC, 0);				/* the panel put away */
		KT_ASSERT(panel_for(p, 30) == 0);
	}
	(void)ob_del_obj(&prog);

	/* a window the desktop's task opens while it does what a process asked */
	q = start(SP_BGWAIT, 0);
	KT_ASSERT(q > 0 && is_ready(q));
	if ( q <= 0 ) return;
	knl_wmobj_act_for(q);
	o.left = 80;  o.top = 80;  o.right = 380;  o.bottom = 280;
	ow = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "ktfor");
	knl_wmobj_act_for(0);
	KT_ASSERT(ow > 0);
	if ( ow > 0 ) {
		wm_raise(ow);
		wm_focus(ow);
		shift_pause();
		KT_ASSERT_EQ(ended(q, 5000), TS_ABORT_TERM);
		(void)wm_close(ow);
	} else {
		gone(q);
	}
	KT_ASSERT_EQ(wm_self_check(), 0);
}

LOCAL void test_sys( void )
{
	INT	wid;
	ID	pid;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	pid = window_process(TS_PRC_SYSTEM, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 ) return;
	shift_pause();
	KT_ASSERT(fq_says("はシステムのプロセスなので強制終了できません", 30));
	KT_ASSERT(running(pid));
	KT_ASSERT_ER(knl_prc_kill(pid), E_OACV);
	KT_ASSERT_ER(ts_ter_prc(pid, -1), E_OK);
	KT_ASSERT_EQ(ended(pid, 5000), -1);
}

/* Started again: a new task, its first window again, and the count gone up from n */
LOCAL BOOL restarted( ID old, INT n )
{
	T_FQSTAT	st;
	INT		t;

	for ( t = 0; t < 150; t++ ) {
		knl_fq_stat(&st);
		if ( st.restarts > n && knl_wdog_watched() > 0 && knl_wdog_watched() != old
		  && dt_first_win() != NULL ) {
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  not started again: %d times, task %d (was %d)\n", st.restarts,
		  knl_wdog_watched(), old);
	return FALSE;
}

LOCAL void test_restart( void )
{
	T_FQSTAT	st;
	INT		wa, n0, t;
	ID		a, old;

	if ( !up || !have_prog ) KT_SKIP("no desktop or no program");
	a = window_process(0, &wa);
	KT_ASSERT(a > 0 && wa > 0);
	if ( a <= 0 ) return;
	old = knl_wdog_watched();
	knl_fq_stat(&st);
	n0 = st.restarts;
	KT_ASSERT(kernel_window() > 0);
	KT_ASSERT(stall(1));

	/* the first press says what is lost, the second does it */
	shift_pause();
	KT_ASSERT(fq_says("応答していません", 30));
	knl_fq_stat(&st);
	KT_ASSERT(st.armed);
	KT_ASSERT(running(a));
	tk_dly_tsk(300);
	shift_pause();
	KT_ASSERT(restarted(old, n0));
	KT_ASSERT(fq_says("デスクトップを立ち上げ直しました", 50));
	KT_ASSERT(!knl_wdog_stuck(NULL, NULL));

	/* the process and its window stayed */
	KT_ASSERT(running(a));
	KT_ASSERT_ER(wm_ref(wa, &wr), E_OK);
	KT_ASSERT(window_titled("ktpause", 0) == wa);
	for ( t = 0; t < 30 && wm_self_check() != 0; t++ ) tk_dly_tsk(100);
	KT_ASSERT_EQ(wm_self_check(), 0);

	/* and Shift+Pause works on it as before */
	tk_dly_tsk(1000);
	wm_raise(wa);
	wm_focus(wa);
	shift_pause();
	KT_ASSERT_EQ(ended(a, 5000), TS_ABORT_TERM);
}

LOCAL void test_restart_button( void )
{
	T_FQSTAT	st;
	T_WMWIN		w;
	INT		n0, x, y, t;
	ID		old;

	if ( !up ) KT_SKIP("no desktop");
	old = knl_wdog_watched();
	knl_fq_stat(&st);
	n0 = st.restarts;
	ctrl_shift_pause();
	KT_ASSERT(list_up(TRUE, 30));
	knl_fq_stat(&st);
	if ( st.wid <= 0 || wm_ref(st.wid, &w) < E_OK ) return;
	click(w.work.left + FQW_RESTART_X, w.work.top + FQW_BTN_Y);
	for ( t = 0; t < 30; t++ ) {
		knl_fq_stat(&st);
		if ( st.askwid > 0 ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(st.confirm && st.askwid > 0);
	KT_ASSERT_EQ(st.restarts, n0);
	if ( st.askwid <= 0 || wm_ref(st.askwid, &w) < E_OK ) return;
	x = w.work.left + FQW_ASK_X;
	y = w.work.top + FQW_ASK_Y;
	click(x, y);
	KT_ASSERT(restarted(old, n0));
	KT_ASSERT(list_up(TRUE, 5));			/* the window stays up */
	ctrl_shift_pause();
	KT_ASSERT(list_up(FALSE, 30));
	KT_ASSERT_EQ(wm_self_check(), 0);
}

EXPORT void ktest_fq( void )
{
	KT_RUN(test_setup);
	KT_RUN(test_front);
	KT_RUN(test_lost_up);
	KT_RUN(test_stalled);
	KT_RUN(test_list);
	KT_RUN(test_stuck_call);
	KT_RUN(test_serial_cut);
	KT_RUN(test_delegate);
	KT_RUN(test_sys);
	KT_RUN(test_restart);
	KT_RUN(test_restart_button);
	dt_test_stall = 0;
	if ( up ) {
		dt_quit();
		up = FALSE;
	}
	knl_lockup_tune(0, 0, 0);
	wm_update();
}
