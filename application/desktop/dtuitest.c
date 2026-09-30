/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtuitest.c
 *	Driving the desktop from outside, for the tests of what it shows
 *	(make DESKTOP=1 UITEST=1; tools/ui_run.py)
 *
 *	A task reads lines from the second serial port (serb) and does
 *	what each says, answering "ok ..." or "err ..." on the console,
 *	each line marked "@ut ", when it is done, so that the one driving it goes on at once rather
 *	than waiting a fixed time. What it puts in goes in as the keyboard
 *	and the pointer would give it, written to the input object 入力
 *	(OB_IN_EVENTS) with a key kept open, at the place asked for, in one
 *	step. The lines:
 *
 *	  ready			answered once the desktop is up
 *	  move X Y		the pointer to X,Y
 *	  down [B] / up [B]	a button (0 the first, 1 the second) pressed, let go
 *	  click X Y [B]		there, pressed and let go at once
 *	  dclick X Y		two clicks, as close as a double press
 *	  hold X Y [B]		there, pressed and held (a menu opened and kept)
 *	  drag X1 Y1 X2 Y2 [B]	pressed at one place, carried in steps, let go at the other
 *	  key CODE [MODS]	a key by its usage code (hex), with the modifiers (hex)
 *	  text STRING		letters and digits typed one after another
 *	  wheel N		the wheel turned N notches (up is positive)
 *	  open UUID		the object opened, as a double press on a link to it opens it
 *	  run PATH [N ...]	the program file started as a process, the numbers (up
 *				to eight) its start-up argument: "ok PID"
 *	  win TITLE		the window whose band says TITLE: "ok L T R B" of its outside
 *	  wins			every window shown, nearest first: "ok N" then one line each
 *	  wait MS		nothing for so long, and then the answer
 *	  idle			the input taken and the screen made up (a short wait)
 *	  lag MS		the pointer moved to and fro for MS ms: "ok WORST MOVES LATE",
 *				the longest the screen took to show a move (ms), the moves,
 *				and how many took longer than LAG_LATE_MS
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/hid.h>
#include <ts/ser.h>
#include <ts/wm.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>
#include <ts/proc.h>
#include "desktop.h"

#ifdef USE_UITEST

#define UT_LINE		256
#define UT_STEPS	12		/* moves a drag is made of */
#define UT_GAP_MS	15		/* between the pieces of one action */
#define UT_DCLICK_MS	60		/* between the two presses of a double click */

LOCAL ID	ut_dd = 0;
LOCAL INT	ut_x = 0, ut_y = 0;

/*
 * An answer goes out on the console, each line marked "@ut ", which the
 * runner reads from the console's log: the console reaches the host
 * however busy the machine is.
 */
LOCAL char	ut_out[UT_LINE + 8];
LOCAL INT	ut_outn = 0;

/* Gathered into whole lines, each written at once so that what other tasks print cannot come between */
LOCAL void say( CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		if ( ut_outn == 0 ) {
			ut_out[0] = '@';  ut_out[1] = 'u';  ut_out[2] = 't';  ut_out[3] = ' ';
			ut_outn = 4;
		}
		if ( ut_outn < UT_LINE + 6 ) ut_out[ut_outn++] = *s;
		if ( *s == '\n' ) {
			ut_out[ut_outn] = 0;
			tm_putstring((UB *)ut_out);
			ut_outn = 0;
		}
	}
}

LOCAL ID	ut_in_key = 0;		/* 入力, opened once to put events in */

/* One event put in, as a keyboard or the pointer would have reported it */
LOCAL void inject( CONST T_HIDEV *e )
{
	if ( ut_in_key <= 0 ) {
		ut_in_key = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE | OB_OP_EXEC);
		if ( ut_in_key <= 0 ) {
			ut_in_key = 0;
			return;
		}
	}
	(void)ob_wri_rec(ut_in_key, OB_IN_EVENTS, 0, e, sizeof(*e), NULL);
}

LOCAL void ev( UINT type, UINT code, UINT mods )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.mods = mods;
	e.x = ut_x;
	e.y = ut_y;
	inject(&e);
}

LOCAL void move( INT x, INT y )
{
	ut_x = x;
	ut_y = y;
	ev(HID_EV_MOVE, 0, 0);
}

/* ---------------------------------------------------------------- the words of a line */

LOCAL CONST char *skip( CONST char *s )
{
	while ( *s == ' ' || *s == '\t' ) s++;
	return s;
}

/* A number, decimal or with 0x hex; FALSE when there is none */
LOCAL BOOL num( CONST char **p, INT *v, BOOL hex )
{
	CONST char	*s = skip(*p);
	INT		n = 0, sign = 1, d, any = 0;

	if ( *s == '-' ) {
		sign = -1;
		s++;
	}
	if ( s[0] == '0' && ( s[1] == 'x' || s[1] == 'X' ) ) {
		hex = TRUE;
		s += 2;
	}
	for ( ;; s++, any++ ) {
		if ( *s >= '0' && *s <= '9' ) d = *s - '0';
		else if ( hex && *s >= 'a' && *s <= 'f' ) d = *s - 'a' + 10;
		else if ( hex && *s >= 'A' && *s <= 'F' ) d = *s - 'A' + 10;
		else break;
		n = n * ( hex ? 16 : 10 ) + d;
	}
	*p = s;
	*v = n * sign;
	return (BOOL)( any > 0 );
}

LOCAL BOOL word( CONST char **p, CONST char *w )
{
	CONST char	*s = skip(*p);
	INT		i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( s[i] != w[i] ) return FALSE;
	}
	if ( s[i] != 0 && s[i] != ' ' && s[i] != '\t' ) return FALSE;
	*p = s + i;
	return TRUE;
}

/* The usage code a letter or digit is typed with, and whether with Shift */
LOCAL UINT usage_of( char c, BOOL *p_shift )
{
	*p_shift = FALSE;
	if ( c >= 'a' && c <= 'z' ) return 0x04 + (UINT)( c - 'a' );
	if ( c >= 'A' && c <= 'Z' ) { *p_shift = TRUE; return 0x04 + (UINT)( c - 'A' ); }
	if ( c >= '1' && c <= '9' ) return 0x1E + (UINT)( c - '1' );
	if ( c == '0' ) return 0x27;
	if ( c == ' ' ) return 0x2C;
	if ( c == '.' ) return 0x37;
	if ( c == '-' ) return 0x2D;
	if ( c == '/' ) return 0x38;
	return 0;
}

/* ---------------------------------------------------------------- the windows */

LOCAL BOOL same_title( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

LOCAL void put_num( char *b, INT *n, INT v )
{
	char	t[12];
	INT	k = 0;

	if ( v < 0 ) {
		b[(*n)++] = '-';
		v = -v;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( k > 0 ) b[(*n)++] = t[--k];
}

LOCAL void say_rect( CONST char *head, INT wid, CONST char *tail )
{
	T_WMWIN	w;
	char	b[UT_LINE];
	INT	n = 0, i;

	if ( wm_ref(wid, &w) < E_OK ) {
		say("err no window\n");
		return;
	}
	for ( i = 0; head[i] != 0; i++ ) b[n++] = head[i];
	put_num(b, &n, w.outer.left);  b[n++] = ' ';
	put_num(b, &n, w.outer.top);   b[n++] = ' ';
	put_num(b, &n, w.outer.right); b[n++] = ' ';
	put_num(b, &n, w.outer.bottom);
	for ( i = 0; tail != NULL && tail[i] != 0 && n < UT_LINE - 2; i++ ) b[n++] = tail[i];
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

/* The window of that name, looked for again for up to two seconds while it opens */
LOCAL void find_win( CONST char *title )
{
	INT	wids[32], n, i, tries;
	char	t[WM_TITLE_MAX];

	for ( tries = 0; tries < 20; tries++ ) {
		n = wm_list(wids, 32);
		for ( i = 0; i < n; i++ ) {
			if ( wm_title(wids[i], t, sizeof(t)) >= 0 && same_title(t, title) ) {
				say_rect("ok ", wids[i], NULL);
				return;
			}
		}
		tk_dly_tsk(100);
	}
	say("err no such window\n");
}

LOCAL void all_wins( void )
{
	INT	wids[32], n = wm_list(wids, 32), i, k = 0;
	char	b[16], t[WM_TITLE_MAX + 2];

	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, n);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
	for ( i = 0; i < n; i++ ) {
		t[0] = ' ';
		if ( wm_title(wids[i], t + 1, WM_TITLE_MAX) < 0 ) t[1] = 0;
		say_rect("", wids[i], t);
	}
}

/* An object opened as a double press on a link to it opens it, asked of the desktop as a program asks */
LOCAL void open_obj( CONST char *us )
{
	T_DTREQ	*rq = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ));
	TS_UUID	d;
	SZ	asz = 0;
	ID	k;
	ER	er;

	if ( rq == NULL ) {
		say("err no memory\n");
		return;
	}
	knl_memset(rq, 0, sizeof(*rq));
	rq->req = DT_RQ_OPEN;
	rq->recno = -1;
	er = ts_str_to_uuid(us, &rq->target);
	if ( er >= E_OK ) er = ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d);
	k = ( er >= E_OK ) ? ob_opn_obj(&d, OB_OP_WRITE) : (ID)er;
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	Kfree(rq);
	say(( er >= E_OK ) ? "ok\n" : "err not opened\n");
}

/* A program file started as a process, with up to eight numbers as its argument */
LOCAL void run_prog( CONST char *s )
{
	T_CPRC	cprc;
	UW	arg[8];
	char	path[UT_LINE], b[24];
	INT	n = 0, k = 0, v;
	ID	pid;

	s = skip(s);
	while ( *s != 0 && *s != ' ' && *s != '\t' && n < UT_LINE - 1 ) {
		path[n++] = *s++;
	}
	path[n] = 0;
	knl_memset(arg, 0, sizeof(arg));
	for ( n = 0; n < 8 && num(&s, &v, FALSE); n++ ) {
		arg[n] = (UW)v;
	}
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.arg = arg;
	cprc.argsz = sizeof(arg);
	pid = ts_cre_prc(path, &cprc);
	if ( pid <= 0 ) {
		say("err not started\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, pid);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* ---------------------------------------------------------------- how the pointer keeps up */

#define LAG_STEP_MS	50		/* between two moves of the probe */
#define LAG_POLL_MS	5		/* how often it looks whether the screen shows the move */
#define LAG_LATE_MS	200		/* a move shown later than this is counted late */
#define LAG_GIVEUP_MS	120000		/* a move not shown by then is not waited for */

LOCAL UW ut_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

/*
 * For 'ms' milliseconds, the pointer moved to and fro every LAG_STEP_MS,
 * and each time, how long the screen took to show it at the place it
 * was sent to (wm_pointer_last): "ok WORST MOVES LATE", the longest
 * wait in ms, the moves made, and how many were shown later than
 * LAG_LATE_MS. A move is not made before the last one is shown, so a
 * screen that stood still for a second shows as one move of a second.
 */
LOCAL void lag( INT ms )
{
	T_DPRECT	to;
	char		b[48];
	INT		x0 = ut_x, y0 = ut_y, x, lx, ly, side, n = 0, late = 0, k = 0;
	UW		start = ut_now(), sent, waited, worst = 0;

	while ( (INT)( ut_now() - start ) < ms ) {
		x = ( ( n & 1 ) != 0 ) ? x0 + 24 : x0;
		move(x, y0);
		sent = ut_now();
		wm_pointer_box(x, y0, &to);
		for ( ;; ) {
			waited = ut_now() - sent;
			if ( ( wm_pointer_last(&lx, &ly, &side) && lx == to.left && ly == to.top )
			  || waited >= LAG_GIVEUP_MS ) {
				break;
			}
			tk_dly_tsk(LAG_POLL_MS);
		}
		if ( waited > worst ) worst = waited;
		if ( waited > LAG_LATE_MS ) late++;
		n++;
		if ( waited < LAG_STEP_MS ) tk_dly_tsk((RELTIM)( LAG_STEP_MS - waited ));
	}
	move(x0, y0);
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)worst);  b[k++] = ' ';
	put_num(b, &k, n);  b[k++] = ' ';
	put_num(b, &k, late);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* ---------------------------------------------------------------- a line done */

LOCAL void line( CONST char *s )
{
	INT	x, y, x2, y2, b = 0, i, m = 0;

	if ( word(&s, "ready") ) {
		say("ok\n");
	} else if ( word(&s, "move") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		move(x, y);
		say("ok\n");
	} else if ( word(&s, "down") ) {
		(void)num(&s, &b, FALSE);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "up") ) {
		(void)num(&s, &b, FALSE);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "click") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "dclick") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		move(x, y);
		for ( i = 0; i < 2; i++ ) {
			tk_dly_tsk(i == 0 ? UT_GAP_MS : UT_DCLICK_MS);
			ev(HID_EV_BTN_DOWN, 0, 0);
			tk_dly_tsk(UT_GAP_MS);
			ev(HID_EV_BTN_UP, 0, 0);
		}
		say("ok\n");
	} else if ( word(&s, "hold") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "drag") && num(&s, &x, FALSE) && num(&s, &y, FALSE)
		 && num(&s, &x2, FALSE) && num(&s, &y2, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		for ( i = 1; i <= UT_STEPS; i++ ) {
			tk_dly_tsk(UT_GAP_MS);
			move(x + ( x2 - x ) * i / UT_STEPS, y + ( y2 - y ) * i / UT_STEPS);
		}
		tk_dly_tsk(UT_GAP_MS * 4);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "key") && num(&s, &x, TRUE) ) {
		(void)num(&s, &m, TRUE);
		ev(HID_EV_KEY_DOWN, (UINT)x, (UINT)m);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_KEY_UP, (UINT)x, (UINT)m);
		say("ok\n");
	} else if ( word(&s, "text") ) {
		s = skip(s);
		for ( ; *s != 0; s++ ) {
			BOOL	sh;
			UINT	u = usage_of(*s, &sh);

			if ( u == 0 ) continue;
			ev(HID_EV_KEY_DOWN, u, sh ? HID_MOD_LSHIFT : 0);
			tk_dly_tsk(UT_GAP_MS);
			ev(HID_EV_KEY_UP, u, sh ? HID_MOD_LSHIFT : 0);
			tk_dly_tsk(UT_GAP_MS);
		}
		say("ok\n");
	} else if ( word(&s, "wheel") && num(&s, &x, FALSE) ) {
		T_HIDEV	e;

		knl_memset(&e, 0, sizeof(e));
		e.type = HID_EV_WHEEL;
		e.code = HID_WHEEL_V;
		e.x = ut_x;
		e.y = ut_y;
		e.dz = x;
		inject(&e);
		say("ok\n");
	} else if ( word(&s, "open") ) {
		open_obj(skip(s));
	} else if ( word(&s, "run") ) {
		run_prog(s);
	} else if ( word(&s, "win") ) {
		find_win(skip(s));
	} else if ( word(&s, "wins") ) {
		all_wins();
	} else if ( word(&s, "wait") && num(&s, &x, FALSE) ) {
		tk_dly_tsk(( x > 0 ) ? x : 0);
		say("ok\n");
	} else if ( word(&s, "idle") ) {
		tk_dly_tsk(150);
		say("ok\n");
	} else if ( word(&s, "lag") && num(&s, &x, FALSE) ) {
		lag(x);
	} else {
		say("err what\n");
	}
}

LOCAL void ut_task( INT stacd, void *exinf )
{
	char	buf[UT_LINE];
	UB	c;
	INT	n = 0;
	SZ	asz = 0;
	TMO	tmo = 200;

	(void)stacd;
	(void)exinf;
	(void)tk_swri_dev(ut_dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz);
	tm_printf((UB *)"uitest: ready\n");
	for ( ;; ) {
		asz = 0;
		if ( tk_srea_dev(ut_dd, 0, &c, 1, &asz) < E_OK || asz == 0 ) {
			continue;
		}
		if ( c == '\r' ) continue;
		if ( c != '\n' ) {
			if ( n < UT_LINE - 1 ) buf[n++] = (char)c;
			continue;
		}
		buf[n] = 0;
		n = 0;
		say("> ");			/* what came, in the log beside what was answered */
		say(buf);
		say("\n");
		line(buf);
	}
}

EXPORT void dt_uitest_start( void )
{
	T_CTSK	ct;
	ID	tid;

	ut_dd = tk_opn_dev((UB *)"serb", TD_UPDATE);
	if ( ut_dd <= 0 ) {
		tm_printf((UB *)"uitest: no second serial port\n");
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ut_task;
	ct.itskpri = 6;		/* above the desktop, which may be busy for a while; it mostly waits on the port */
	ct.stksz = 8192;
	tid = tk_cre_tsk(&ct);
	if ( tid > 0 ) {
		(void)tk_sta_tsk(tid, 0);
	}
}

#else

EXPORT void dt_uitest_start( void )
{
}

#endif /* USE_UITEST */
