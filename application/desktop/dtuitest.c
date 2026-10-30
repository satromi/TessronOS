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
 *	  text STRING		letters, digits and . - / : typed one after another
 *	  wheel N		the wheel turned N notches (up is positive)
 *	  open UUID		the object opened, as a double press on a link to it opens it
 *	  run PATH [N ...]	the program file started as a process, the numbers (up
 *				to eight) its start-up argument: "ok PID"
 *	  win TITLE		the window whose band says TITLE: "ok L T R B" of its outside
 *	  wins			every window shown, nearest first: "ok N" then one line each
 *	  front TAG T1|T2|...	the windows shown whose bands say T1, T2, ... are the
 *				nearest ones, in that order: "ok TAG: in front"
 *	  hidden TAG T		no window shown says T: "ok TAG: hidden"; both are
 *				looked at again for up to two seconds while the screen settles,
 *				and the TAG lets a test expect each answer apart
 *	  wait MS		nothing for so long, and then the answer
 *	  idle			the input taken and the screen made up (a short wait)
 *	  lag MS		the pointer moved to and fro for MS ms: "ok WORST MOVES LATE",
 *				the longest the screen took to show a move (ms), the moves,
 *				and how many took longer than LAG_LATE_MS
 *	  look NUM VALUE	an entry of the look table set (LK_ROUND and the like)
 *				and the screen built again
 *	  stat			what was built and sent since the last stat: "ok BUILDS PARTS
 *				KPX_BUILT MOVES MOVES_BUILT RECTS_SENT KPX_SENT", the pointer's
 *				moves and how many of them came with something built
 *	  wifitest		on a machine with no Wi-Fi card, the Wi-Fi manager's card
 *				for the tests (a build with the Wi-Fi): wlan0 with a scan's
 *				networks, its saved networks changed as on a card
 *	  xml TEXT		a line of xmlTAD kept, after the lines kept before it
 *	  xmlopen TITLE		the xmlTAD kept, a document, opened in a window titled
 *				TITLE that can be edited (no object behind it), and forgotten
 *	  caret			the caret of the document in front: "ok c PARA,POS a PARA,POS",
 *				the caret and the other end of the selection
 *	  mkbig doc|fig|calc N [M]
 *				a large object for the tests of size, a copy of the
 *				template of its kind with record 0 made here: a text of N
 *				paragraphs, a figure of N rectangles and lines of M points,
 *				or a sheet of N rows and M columns: "ok UUID BYTES"
 *	  sum UUID		record 0 of the object: "ok BYTES HASH" (FNV-1a, 64 bits)
 *	  shapes		the figure in front: "ok SHAPES PICKED"
 *	  mark			the time from which the next two count
 *	  waitwin TITLE [S]	a window that says TITLE (spaces and all) shown, looked for up to S seconds
 *				(60): "ok MS" since the mark
 *	  sync [S]		the desktop's own task come round its loop and found no
 *				input left (all that was put in before taken and done),
 *				waited for up to S seconds (60): "ok MS" since the mark
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/hid.h>
#include <ts/ser.h>
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>
#include <ts/proc.h>
#include "desktop.h"

#ifdef USE_UITEST

IMPORT void knl_wifi_test_card( void ) __attribute__((weak));	/* device/net/wifi/wifi.c, with the Wi-Fi */

#define UT_LINE		256
#define UT_STEPS	12		/* moves a drag is made of */
#define UT_GAP_MS	15		/* between the pieces of one action */
#define UT_DCLICK_MS	60		/* between the two presses of a double click */

LOCAL ID	ut_dd = 0;

/* The large object made last: what "open big" and "sum" with no object take */
LOCAL TS_UUID	ut_big;
LOCAL BOOL	ut_big_have = FALSE;
LOCAL volatile INT ut_sync = 0;		/* 1 asked, 3 seen by the loop, 2 no input left after that */
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

/* The usage code a letter, digit or sign is typed with, and whether with Shift */
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
	if ( c == ':' ) return 0x34;		/* the Japanese keyboard's colon, unshifted */
	if ( c == ',' ) return 0x36;
	if ( c == ';' ) return 0x33;
	if ( c == '[' ) return 0x30;
	if ( c == ']' ) return 0x31;
	if ( c == '@' ) return 0x2F;
	if ( c == '^' ) return 0x2E;
	{
		/* the signs over the digits and the other keys, shifted, as the Japanese keyboard has them */
		static CONST char	sh[] = "!\"#$%&'()";
		INT			i;

		for ( i = 0; sh[i] != 0; i++ ) {
			if ( c == sh[i] ) {
				*p_shift = TRUE;
				return 0x1E + (UINT)i;
			}
		}
	}
	*p_shift = TRUE;
	if ( c == '=' ) return 0x2D;
	if ( c == '+' ) return 0x33;
	if ( c == '*' ) return 0x34;
	if ( c == '<' ) return 0x36;
	if ( c == '>' ) return 0x37;
	if ( c == '?' ) return 0x38;
	if ( c == '{' ) return 0x30;
	if ( c == '}' ) return 0x31;
	*p_shift = FALSE;
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

/*
 * The windows shown with a band, front first, however many there are:
 * the list asked for again into twice the room while it filled all it
 * was given. Freed by the caller (Kfree); NULL with *p_n 0 when there
 * is no memory.
 */
LOCAL INT *ut_windows( INT *p_n )
{
	INT	cap = 32, n;
	INT	*wids;

	for ( ;; ) {
		wids = (INT *)Kmalloc((SZ)cap * sizeof(INT));
		if ( wids == NULL ) {
			*p_n = 0;
			return NULL;
		}
		n = wm_list(wids, cap);
		if ( n < cap ) {
			*p_n = n;
			return wids;
		}
		Kfree(wids);
		cap *= 2;
	}
}

/* The first window shown with a band that says title, 0 when there is none */
LOCAL INT ut_named( CONST char *title )
{
	INT	n, i, got = 0;
	INT	*wids = ut_windows(&n);
	char	t[WM_TITLE_MAX];

	for ( i = 0; i < n && got == 0; i++ ) {
		if ( wm_title(wids[i], t, sizeof(t)) >= 0 && same_title(t, title) ) got = wids[i];
	}
	if ( wids != NULL ) Kfree(wids);
	return got;
}

/* The window of that name, looked for again for up to two seconds while it opens */
LOCAL void find_win( CONST char *title )
{
	INT	tries, wid;

	for ( tries = 0; tries < 20; tries++ ) {
		if ( ( wid = ut_named(title) ) > 0 ) {
			say_rect("ok ", wid, NULL);
			return;
		}
		tk_dly_tsk(100);
	}
	say("err no such window\n");
}

LOCAL void all_wins( void )
{
	INT	n, i, k = 0;
	INT	*wids = ut_windows(&n);
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
	if ( wids != NULL ) Kfree(wids);
}

/*
 * The tag of a front or hidden line, and where what follows it begins:
 * FALSE when there is no tag
 */
LOCAL BOOL tag_of( CONST char **p, char *tag, INT max )
{
	CONST char	*s = skip(*p);
	INT		n = 0;

	while ( *s != 0 && *s != ' ' && *s != '\t' && n < max - 1 ) tag[n++] = *s++;
	tag[n] = 0;
	*p = skip(s);
	return (BOOL)( n > 0 );
}

LOCAL void say_tagged( CONST char *tag, CONST char *what )
{
	char	b[UT_LINE];
	INT	n = 0, i;

	b[n++] = 'o';  b[n++] = 'k';  b[n++] = ' ';
	for ( i = 0; tag[i] != 0 && n < UT_LINE - 40; i++ ) b[n++] = tag[i];
	b[n++] = ':';  b[n++] = ' ';
	for ( i = 0; what[i] != 0 && n < UT_LINE - 2; i++ ) b[n++] = what[i];
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

/* Whether the n names of 'names' (split by '|') are the first n windows of the list, in that order */
LOCAL BOOL front_is( CONST char *names )
{
	INT	n, i = 0, k;
	INT	*wids = ut_windows(&n);
	char	want[WM_TITLE_MAX], t[WM_TITLE_MAX];
	BOOL	yes = TRUE;

	while ( *names != 0 && yes ) {
		for ( k = 0; names[k] != 0 && names[k] != '|' && k < WM_TITLE_MAX - 1; k++ ) {
			want[k] = names[k];
		}
		want[k] = 0;
		names += k;
		if ( *names == '|' ) names++;
		if ( i >= n || wm_title(wids[i], t, sizeof(t)) < 0 || !same_title(t, want) ) {
			yes = FALSE;
		}
		i++;
	}
	if ( wids != NULL ) Kfree(wids);
	return (BOOL)( yes && i > 0 );
}

LOCAL BOOL shown_named( CONST char *title )
{
	return (BOOL)( ut_named(title) > 0 );
}

LOCAL void front_check( CONST char *s )
{
	char	tag[32];
	INT	tries;

	if ( !tag_of(&s, tag, sizeof(tag)) || *s == 0 ) {
		say("err front TAG T1|T2\n");
		return;
	}
	for ( tries = 0; tries < 20; tries++ ) {
		if ( front_is(s) ) {
			say_tagged(tag, "in front");
			return;
		}
		tk_dly_tsk(100);
	}
	say("err not in front\n");
}

LOCAL void hidden_check( CONST char *s )
{
	char	tag[32];
	INT	tries;

	if ( !tag_of(&s, tag, sizeof(tag)) || *s == 0 ) {
		say("err hidden TAG T\n");
		return;
	}
	for ( tries = 0; tries < 20; tries++ ) {
		if ( !shown_named(s) ) {
			say_tagged(tag, "hidden");
			return;
		}
		tk_dly_tsk(100);
	}
	say("err still shown\n");
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
	if ( us[0] == 'b' && us[1] == 'i' && us[2] == 'g' && us[3] == 0 && ut_big_have ) {
		rq->target = ut_big;		/* the large object made last */
		er = E_OK;
	} else {
		er = ts_str_to_uuid(us, &rq->target);
	}
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

/*
 * The counts of the window manager and the screen since the last time
 * they were asked for
 */
LOCAL T_WMSTAT		ut_wm_was;
LOCAL T_DISPSTAT	ut_disp_was;

LOCAL void ut_stat( void )
{
	T_WMSTAT	w;
	T_DISPSTAT	d;
	char		b[160];
	INT		k = 0;

	knl_memset(&w, 0, sizeof(w));
	knl_memset(&d, 0, sizeof(d));
	(void)wm_stat(&w);
	(void)ts_disp_stat(&d);
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)( w.composites - ut_wm_was.composites ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.parts - ut_wm_was.parts ));  b[k++] = ' ';
	put_num(b, &k, (INT)( ( w.pixels - ut_wm_was.pixels ) / 1000 ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.ptr_moves - ut_wm_was.ptr_moves ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.ptr_built - ut_wm_was.ptr_built ));  b[k++] = ' ';
	put_num(b, &k, (INT)( d.rects - ut_disp_was.rects ));  b[k++] = ' ';
	put_num(b, &k, (INT)( ( d.pixels - ut_disp_was.pixels ) / 1000 ));
	b[k++] = '\n';
	b[k] = 0;
	ut_wm_was = w;
	ut_disp_was = d;
	say(b);
}

/*
 * The calls of processes, the takes of the process table's lock and
 * those of them that waited, since the last time they were asked for
 */
IMPORT volatile UD	knl_svc_calls;
IMPORT void knl_prc_lock_stat( UD *p_takes, UD *p_waits );
LOCAL UD	ut_svc_was[3];

LOCAL void svc_stat( void )
{
	UD	v[3];
	char	b[96];
	INT	k = 0, i;

	v[0] = knl_svc_calls;
	knl_prc_lock_stat(&v[1], &v[2]);
	b[k++] = 'o';  b[k++] = 'k';
	for ( i = 0; i < 3; i++ ) {
		b[k++] = ' ';
		put_num(b, &k, (INT)( v[i] - ut_svc_was[i] ));
		ut_svc_was[i] = v[i];
	}
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/*
 * What the tasks are doing now: how many run, are ready, and wait for
 * each kind of thing, and the three objects most waited on with how
 * many wait on each ("m12x30": mutex 12, thirty tasks)
 */
#define UT_WAIT_TOP	3

LOCAL void waits_say( void )
{
	T_RTSK	r;
	INT	run = 0, rdy = 0, dly = 0, slp = 0, sem = 0, flg = 0, mtx = 0, mbf = 0, oth = 0, sus = 0;
	INT	topid[UT_WAIT_TOP], topn[UT_WAIT_TOP], topk[UT_WAIT_TOP];
	INT	tid, i, j, k = 0, n;
	ID	ids[64];
	INT	idn[64], idk[64], nid = 0;
	char	b[200];

	for ( tid = 1; tid <= CNF_MAX_TSKID; tid++ ) {
		INT	kind = 0;

		if ( tk_ref_tsk(tid, &r) < E_OK ) continue;
		if ( ( r.tskstat & TTS_RUN ) != 0 ) run++;
		else if ( ( r.tskstat & TTS_RDY ) != 0 ) rdy++;
		else if ( ( r.tskstat & TTS_WAI ) != 0 ) {
			if ( r.tskwait & TTW_DLY ) dly++;
			else if ( r.tskwait & TTW_SLP ) slp++;
			else if ( r.tskwait & TTW_SEM ) { sem++; kind = 's'; }
			else if ( r.tskwait & TTW_FLG ) { flg++; kind = 'f'; }
			else if ( r.tskwait & TTW_MTX ) { mtx++; kind = 'm'; }
			else if ( r.tskwait & ( TTW_SMBF | TTW_RMBF ) ) { mbf++; kind = 'b'; }
			else oth++;
		} else if ( ( r.tskstat & TTS_SUS ) != 0 ) sus++;
		if ( kind != 0 && r.wid > 0 ) {
			for ( i = 0; i < nid && !( ids[i] == r.wid && idk[i] == kind ); i++ ) ;
			if ( i == nid && nid < 64 ) { ids[nid] = r.wid; idk[nid] = kind; idn[nid] = 0; nid++; }
			if ( i < nid ) idn[i]++;
		}
	}
	for ( j = 0; j < UT_WAIT_TOP; j++ ) {
		topid[j] = 0; topn[j] = 0; topk[j] = 0;
		for ( i = 0; i < nid; i++ ) {
			if ( idn[i] > topn[j] ) { topn[j] = idn[i]; topid[j] = ids[i]; topk[j] = idk[i]; n = i; }
		}
		if ( topn[j] > 0 ) idn[n] = 0;
	}
	b[k++] = 'o';  b[k++] = 'k';
	{
		CONST char	*lab[10] = { " run ", " rdy ", " dly ", " slp ", " sem ", " flg ", " mtx ", " mbf ", " oth ", " sus " };
		INT		v[10];

		v[0] = run; v[1] = rdy; v[2] = dly; v[3] = slp; v[4] = sem; v[5] = flg; v[6] = mtx; v[7] = mbf; v[8] = oth; v[9] = sus;
		for ( i = 0; i < 10; i++ ) {
			for ( j = 0; lab[i][j] != 0; j++ ) b[k++] = lab[i][j];
			put_num(b, &k, v[i]);
		}
	}
	for ( j = 0; j < UT_WAIT_TOP && topn[j] > 0; j++ ) {
		b[k++] = ' ';
		b[k++] = (char)topk[j];
		put_num(b, &k, topid[j]);
		b[k++] = 'x';
		put_num(b, &k, topn[j]);
	}
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* xmlTAD gathered a line at a time for xmlopen */
LOCAL UB	*ut_xml = NULL;
LOCAL SZ	ut_xml_n = 0, ut_xml_max = 0;

LOCAL void xml_keep( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) n++;
	if ( ut_xml_n + n + 2 > ut_xml_max ) {
		SZ	m = ( ut_xml_max > 0 ) ? ut_xml_max * 2 : 4096;
		UB	*b;

		while ( m < ut_xml_n + n + 2 ) m *= 2;
		b = (UB *)Kmalloc(m);
		if ( b == NULL ) {
			say("err no memory\n");
			return;
		}
		if ( ut_xml != NULL ) {
			knl_memcpy(b, ut_xml, ut_xml_n);
			Kfree(ut_xml);
		}
		ut_xml = b;
		ut_xml_max = m;
	}
	knl_memcpy(ut_xml + ut_xml_n, s, n);
	ut_xml_n += n;
	ut_xml[ut_xml_n++] = '\n';
	say("ok\n");
}

/*
 * The window is opened by the desktop's own task, in its loop
 * (dt_uitest_poll): the drawing is the desktop's, and its task has the
 * stack for it. This task waits for it.
 */
LOCAL T_TAD	*ut_open_rec = NULL;
LOCAL char	ut_open_title[UT_LINE];
LOCAL volatile INT ut_open_state = 0;	/* 1 asked, 2 opened, 3 not */

/*
 * The desktop's loop asked for input and none was there. A sync is
 * answered by the first such answer after the loop has seen the ask
 * (dt_uitest_poll, before it reads the input): the keys put in before
 * the ask are read and done by then. Coming round the loop is not
 * enough, nor is an answer the loop had before it saw the ask (it may
 * lay paragraphs for a while after one, with keys come meanwhile).
 */
EXPORT void dt_uitest_idle( void )
{
	if ( ut_sync == 3 ) {
		ut_sync = 2;
	}
}

EXPORT void dt_uitest_poll( void )
{
	T_DPRECT	o;
	DTWIN		*d;

	if ( ut_sync == 1 ) {
		ut_sync = 3;
	}
	if ( ut_open_state != 1 ) {
		return;
	}
	o.left = 100;  o.top = 60;  o.right = 600;  o.bottom = 460;
	d = dt_open_made(ut_open_rec, ut_open_title, DT_TOOL_NONE, &o);
	ut_open_rec = NULL;
	if ( d != NULL ) {
		d->sealed = FALSE;
		dt_draw(d);
	}
	ut_open_state = ( d != NULL ) ? 2 : 3;
}

LOCAL void xml_open( CONST char *title )
{
	T_TAD	*rec = NULL;
	INT	i, tries;

	if ( ut_xml == NULL || tad_parse(ut_xml, ut_xml_n, NULL, &rec) < E_OK ) {
		ut_xml_n = 0;
		say("err not xmlTAD\n");
		return;
	}
	ut_xml_n = 0;
	for ( i = 0; title[i] != 0 && i < UT_LINE - 1; i++ ) {
		ut_open_title[i] = title[i];
	}
	ut_open_title[i] = 0;
	ut_open_rec = rec;
	ut_open_state = 1;
	for ( tries = 0; tries < 100 && ut_open_state == 1; tries++ ) {
		tk_dly_tsk(50);
	}
	say(( ut_open_state == 2 ) ? "ok\n" : "err not opened\n");
	ut_open_state = 0;
}

/* The caret of the document in front, or of the text being edited in the figure in front */
/* How many shapes the figure in front has, and how many of them are picked */
LOCAL void shapes_say( void )
{
	DTWIN	*d = dt_win_of(wm_focused());
	char	b[48];
	INT	k = 0;

	tk_dly_tsk(200);			/* what was put in before taken first */
	d = dt_win_of(wm_focused());
	if ( d == NULL || d->fig == NULL ) {
		say("err no figure\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, d->fig->nsh);
	b[k++] = ' ';
	put_num(b, &k, d->npick);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

LOCAL void caret_say( void )
{
	DTWIN	*d;
	char	b[64];
	INT	k = 0, same = 0, tries, was[4] = { -1, -1, -1, -1 };

	/* the keys put in before are taken first: asked until it stays the same */
	for ( tries = 0; tries < 100 && same < 3; tries++ ) {
		tk_dly_tsk(100);
		d = dt_win_of(wm_focused());
		if ( d != NULL && d->tb != NULL ) d = d->tb;
		if ( d == NULL || d->doc == NULL ) break;
		if ( d->cpara == was[0] && d->cpos == was[1] && d->apara == was[2] && d->apos == was[3] ) {
			same++;
		} else {
			same = 0;
			was[0] = d->cpara;  was[1] = d->cpos;  was[2] = d->apara;  was[3] = d->apos;
		}
	}
	d = dt_win_of(wm_focused());
	if ( d != NULL && d->tb != NULL ) {
		d = d->tb;
	}
	if ( d == NULL || d->doc == NULL ) {
		say("err no document\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';  b[k++] = 'c';  b[k++] = ' ';
	put_num(b, &k, d->cpara);
	b[k++] = ',';
	put_num(b, &k, d->cpos);
	b[k++] = ' ';  b[k++] = 'a';  b[k++] = ' ';
	put_num(b, &k, d->apara);
	b[k++] = ',';
	put_num(b, &k, d->apos);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* ---------------------------------------------------------------- objects of size */

/* The templates the large objects are copies of: 基本文章編集, 画用紙, 基本表計算 */
LOCAL CONST char *CONST ut_tpl[3] = {
	"019a1132-762b-7b02-ba2a-a918a9b37c39",
	"019a0f91-066f-706f-93da-ffe8150d6207",
	"019b54a4-6ccf-72f7-e320-c98e4a0bab4e"
};

/* What is written, a buffer at a time */
typedef struct {
	ID	key;
	UB	*buf;
	SZ	n, max, at;
	ER	er;
} UTOUT;

LOCAL void out_flush( UTOUT *o )
{
	SZ	asz = 0;
	ER	er;

	if ( o->n == 0 || o->er < E_OK ) {
		o->n = 0;
		return;
	}
	er = ob_wri_rec(o->key, 0, (D)o->at, o->buf, o->n, &asz);
	if ( er < E_OK || asz != o->n ) {
		o->er = ( er < E_OK ) ? er : E_IO;
	}
	o->at += o->n;
	o->n = 0;
}

LOCAL void out_str( UTOUT *o, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		if ( o->n >= o->max ) {
			out_flush(o);
		}
		o->buf[o->n++] = (UB)*s;
	}
}

LOCAL void out_num( UTOUT *o, INT v )
{
	char	b[16];
	INT	k = 0;

	put_num(b, &k, v);
	b[k] = 0;
	out_str(o, b);
}

/* "A", "Z", "AA": a column's letters */
LOCAL void out_col( UTOUT *o, INT col )
{
	char	b[8];
	INT	k = 0, i;

	col++;
	while ( col > 0 && k < 7 ) {
		b[k++] = (char)( 'A' + ( col - 1 ) % 26 );
		col = ( col - 1 ) / 26;
	}
	for ( i = 0; i < k / 2; i++ ) {
		char	t = b[i];

		b[i] = b[k - 1 - i];
		b[k - 1 - i] = t;
	}
	b[k] = 0;
	out_str(o, b);
}

LOCAL void big_doc( UTOUT *o, INT n )
{
	INT	i;

	out_str(o, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<text lang=\"0\" bpat=\"0\"/>\n");
	for ( i = 0; i < n && o->er >= E_OK; i++ ) {
		out_str(o, "<p>段落");
		out_num(o, i);
		out_str(o, " あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん");
		out_str(o, "ABCDEFGHIJKLMNOPQRSTUVWXYZ</p>\n");
	}
	out_str(o, "</document>\n</tad>\n");
}

LOCAL void big_fig( UTOUT *o, INT n, INT m )
{
	INT	i, k;

	out_str(o, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n");
	for ( i = 0; i < n && o->er >= E_OK; i++ ) {
		INT	x = ( i % 100 ) * 12, y = ( i / 100 ) * 12;

		out_str(o, "<rect round=\"0\" lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" angle=\"0\" left=\"");
		out_num(o, x);
		out_str(o, "\" top=\"");
		out_num(o, y);
		out_str(o, "\" right=\"");
		out_num(o, x + 8);
		out_str(o, "\" bottom=\"");
		out_num(o, y + 8);
		out_str(o, "\" zIndex=\"");
		out_num(o, i);
		out_str(o, "\"/>\n");
	}
	/* two lines of many points: a zigzag across under the rectangles */
	for ( k = 0; k < 2 && m > 1; k++ ) {
		out_str(o, "<polyline lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" points=\"");
		for ( i = 0; i < m && o->er >= E_OK; i++ ) {
			if ( i > 0 ) out_str(o, " ");
			out_num(o, i * 1200 / m);
			out_str(o, ",");
			out_num(o, ( n / 100 ) * 12 + 20 + k * 40 + ( ( i & 1 ) ? 30 : 0 ));
		}
		out_str(o, "\" zIndex=\"");
		out_num(o, n + k);
		out_str(o, "\"/>\n");
	}
	out_str(o, "</figure>\n</tad>\n");
}

LOCAL void big_calc( UTOUT *o, INT rows, INT cols )
{
	INT	r, c;

	out_str(o, "<tad version=\"1.0\" filename=\"基本表計算\" encoding=\"UTF-8\">\n<document>\n");
	for ( r = 0; r < rows && o->er >= E_OK; r++ ) {
		out_str(o, "<p>");
		for ( c = 0; c < cols; c++ ) {
			if ( c > 0 ) out_str(o, "\t");
			out_str(o, "<calcPos cell=\"");
			out_col(o, c);
			out_num(o, r + 1);
			out_str(o, "\"/>");
			if ( c == cols - 1 && cols > 2 ) {
				/* the last column a formula of the first two */
				out_str(o, "=A");
				out_num(o, r + 1);
				out_str(o, "+B");
				out_num(o, r + 1);
			} else {
				out_num(o, r * cols + c);
			}
		}
		out_str(o, "</p>\n");
	}
	out_str(o, "</document>\n</tad>\n");
}

LOCAL void make_big( CONST char *s )
{
	INT	kind, n = 0, m = 0, k = 0;
	TS_UUID	tpl, u;
	UTOUT	o;
	char	b[80];
	ER	er;

	s = skip(s);
	kind = ( s[0] == 'd' ) ? 0 : ( s[0] == 'f' ) ? 1 : ( s[0] == 'c' ) ? 2 : -1;
	while ( *s != 0 && *s != ' ' ) s++;
	if ( kind < 0 || !num(&s, &n, FALSE) ) {
		say("err mkbig doc|fig|calc N [M]\n");
		return;
	}
	(void)num(&s, &m, FALSE);
	er = ts_str_to_uuid(ut_tpl[kind], &tpl);
	/* the person's, as a copy made from a template on the desktop is, so that a program may write it */
	knl_ob_make_for_person(TRUE);
	if ( er >= E_OK ) er = ob_cpy_obj(&tpl, NULL, &u);
	knl_ob_make_for_person(FALSE);
	if ( er < E_OK ) {
		say("err no copy\n");
		return;
	}
	knl_memset(&o, 0, sizeof(o));
	o.max = 64 * 1024;
	o.buf = (UB *)Kmalloc(o.max);
	o.key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	if ( o.buf == NULL || o.key <= 0 ) {
		if ( o.buf != NULL ) Kfree(o.buf);
		if ( o.key > 0 ) ob_cls_obj(o.key);
		say("err not opened\n");
		return;
	}
	if ( kind == 0 )      big_doc(&o, n);
	else if ( kind == 1 ) big_fig(&o, n, m);
	else                  big_calc(&o, n, m);
	out_flush(&o);
	if ( o.er >= E_OK ) o.er = ob_trn_rec(o.key, 0, (UD)o.at);
	ob_cls_obj(o.key);
	Kfree(o.buf);
	if ( o.er < E_OK ) {
		say("err not written\n");
		return;
	}
	ut_big = u;
	ut_big_have = TRUE;
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	(void)ts_uuid_to_str(&u, b + k, sizeof(b) - k);
	while ( b[k] != 0 ) k++;
	b[k++] = ' ';
	put_num(b, &k, (INT)o.at);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* Record 0 of an object, as long as it is and a hash of it */
LOCAL void rec_sum( CONST char *us )
{
	TS_UUID	u;
	ID	k;
	UB	*buf;
	SZ	at = 0, asz = 0;
	UD	h = 0xCBF29CE484222325ULL;
	char	b[64];
	INT	n = 0, i;
	ER	er = E_OK;

	us = skip(us);
	if ( *us == 0 && ut_big_have ) {
		u = ut_big;
	} else if ( ts_str_to_uuid(us, &u) < E_OK ) {
		say("err no object\n");
		return;
	}
	if ( ( k = ob_opn_obj(&u, OB_OP_R) ) <= 0 ) {
		say("err no object\n");
		return;
	}
	buf = (UB *)Kmalloc(64 * 1024);
	while ( buf != NULL ) {
		er = ob_rea_rec(k, 0, (D)at, buf, 64 * 1024, &asz);
		if ( er < E_OK || asz <= 0 ) break;
		for ( i = 0; i < (INT)asz; i++ ) {
			h ^= buf[i];
			h *= 0x100000001B3ULL;
		}
		at += asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL ) Kfree(buf);
	b[n++] = 'o';  b[n++] = 'k';  b[n++] = ' ';
	put_num(b, &n, (INT)at);
	b[n++] = ' ';
	for ( i = 15; i >= 0; i-- ) {
		b[n++] = "0123456789abcdef"[( h >> ( i * 4 ) ) & 0xF];
	}
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

LOCAL UW	ut_mark = 0;

LOCAL void say_ms( UW ms )
{
	char	b[24];
	INT	k = 0;

	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)ms);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* Up to S seconds for the window that says TITLE */
LOCAL void wait_win( CONST char *s )
{
	INT	i, secs = 60;
	char	title[WM_TITLE_MAX];
	UW	start = ut_now();

	INT	n, k;

	/* the rest of the line is the title, spaces and all; a number last is the seconds */
	s = skip(s);
	for ( n = 0; s[n] != 0 && s[n] != '\r' && s[n] != '\n'; n++ ) {
	}
	while ( n > 0 && s[n - 1] == ' ' ) n--;
	for ( k = n; k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9'; k-- ) {
	}
	if ( k < n && k > 0 && s[k - 1] == ' ' ) {
		CONST char	*d = s + k;

		(void)num(&d, &secs, FALSE);
		for ( n = k; n > 0 && s[n - 1] == ' '; n-- ) {
		}
	}
	for ( i = 0; i < n && i < WM_TITLE_MAX - 1; i++ ) {
		title[i] = s[i];
	}
	title[i] = 0;
	while ( (INT)( ut_now() - start ) < secs * 1000 ) {
		if ( ut_named(title) > 0 ) {
			say_ms(ut_now() - ut_mark);
			return;
		}
		tk_dly_tsk(20);
	}
	say("err no such window\n");
}

/* Up to S seconds for the desktop's task to come round its loop: what it was doing done */
LOCAL void sync_desk( CONST char *s )
{
	INT	secs = 60;
	UW	start = ut_now();

	(void)num(&s, &secs, FALSE);
	ut_sync = 1;
	while ( ( ut_sync == 1 || ut_sync == 3 ) && (INT)( ut_now() - start ) < secs * 1000 ) {
		tk_dly_tsk(10);
	}
	if ( ut_sync != 2 ) {
		ut_sync = 0;
		say("err desktop busy\n");
		return;
	}
	ut_sync = 0;
	say_ms(ut_now() - ut_mark);
}

/* ---------------------------------------------------------------- a line done */

LOCAL void line( CONST char *s )
{
	INT	x, y, x2, y2, b = 0, i, m = 0;

	if ( word(&s, "ready") ) {
		/*
		 * The desktop is up once processes can ask it for things: its
		 * request channel (DT_REQ_NAME) is made after its settings and
		 * programs are taken, which on a slow start is seconds after
		 * this task begins. Until then an open would be refused.
		 */
		TS_UUID	rq;

		for ( i = 0; i < 1000 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &rq) < E_OK; i++ ) {
			tk_dly_tsk(50);
		}
		say(( i < 1000 ) ? "ok\n" : "err the desktop has no request channel\n");
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
	} else if ( word(&s, "keydown") && num(&s, &x, TRUE) ) {
		/* held until its keyup: keys pressed together, a shift held over others */
		(void)num(&s, &m, TRUE);
		ev(HID_EV_KEY_DOWN, (UINT)x, (UINT)m);
		say("ok\n");
	} else if ( word(&s, "keyup") && num(&s, &x, TRUE) ) {
		(void)num(&s, &m, TRUE);
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
	} else if ( word(&s, "front") ) {
		front_check(s);
	} else if ( word(&s, "hidden") ) {
		hidden_check(s);
	} else if ( word(&s, "wait") && num(&s, &x, FALSE) ) {
		tk_dly_tsk(( x > 0 ) ? x : 0);
		say("ok\n");
	} else if ( word(&s, "idle") ) {
		tk_dly_tsk(150);
		say("ok\n");
	} else if ( word(&s, "lag") && num(&s, &x, FALSE) ) {
		lag(x);
	} else if ( word(&s, "svcstat") ) {
		svc_stat();
	} else if ( word(&s, "stkuse") ) {
		/* the deepest use of every stack, to the console (design 4.7.7) */
#if CNF_STK_USAGE
		IMPORT void knl_stk_usage_say( void );

		knl_stk_usage_say();
#endif
		say("ok\n");
	} else if ( word(&s, "waits") ) {
		waits_say();
	} else if ( word(&s, "stat") ) {
		ut_stat();
	} else if ( word(&s, "xmlopen") ) {
		xml_open(skip(s));
	} else if ( word(&s, "xml") ) {
		xml_keep(skip(s));
	} else if ( word(&s, "shapes") ) {
		shapes_say();
	} else if ( word(&s, "caret") ) {
		caret_say();
	} else if ( word(&s, "mkbig") ) {
		make_big(s);
	} else if ( word(&s, "sum") ) {
		rec_sum(s);
	} else if ( word(&s, "mark") ) {
		ut_mark = ut_now();
		say("ok\n");
	} else if ( word(&s, "waitwin") ) {
		wait_win(s);
	} else if ( word(&s, "sync") ) {
		sync_desk(s);
	} else if ( word(&s, "wifitest") ) {
		if ( knl_wifi_test_card != NULL ) {
			knl_wifi_test_card();
			say("ok\n");
		} else {
			say("err no wifi\n");
		}
	} else if ( word(&s, "look") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		if ( wm_set_look((UINT)x, (UW)y) >= E_OK ) {
			(void)wm_composite();
			say("ok\n");
		} else {
			say("err look\n");
		}
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

EXPORT void dt_uitest_poll( void )
{
}

EXPORT void dt_uitest_idle( void )
{
}

#endif /* USE_UITEST */
