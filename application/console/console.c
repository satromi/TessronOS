/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	console.c
 *	コンソール: an accessory that runs as a process of its own
 *	(design 16.5.19)
 *
 *	It shows what went to the console since the machine started and goes
 *	on showing what goes to it, with a line at its foot where a command
 *	is typed. What a command answers goes to the console like anything
 *	else -- out on the serial line too -- and comes back into the window
 *	by the same road. The second button's menu clears the window, shows
 *	the record again from the start, and says whether new lines bring the
 *	view back to the foot. Shift with PageUp and PageDown winds the view.
 *
 *	It is in a place -- an object that links to others, bin when it starts (con_box.c) --
 *	and a command that is not one of its own is the name of a program
 *	there or in bin, run as a process until it ends (con_job.c).
 *	The line is typed through かな漢字変換 as other programs are
 *	(con_ime.c), and Tab completes a name from the links of the place.
 *
 *	Started with the UUID of a program object as its argument -- the
 *	desktop starts it so when a command, a program without a window of
 *	its own, is opened -- it runs that program at once.
 */

#include "console.h"
#include <ts/wm.h>

#define TICK_MS		40
#define HIST_MAX	16
#define PROMPT_END	"> "

#define KEY_A		0x04
#define KEY_Z		0x1D
#define KEY_C		0x06
#define KEY_E		0x08
#define KEY_INS		0x49
#define KEY_HOME	0x4A
#define KEY_DEL		0x4C
#define KEY_END		0x4D
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_TAB		0x2B
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define KEY_KPENTER	0x58
#define MOD_CTRL	0x11
#define MOD_SHIFT	0x22

LOCAL CONST char win_json[] = "{\"rect\":[100,80,760,500],\"attr\":15}";

LOCAL LTERM	term;
LOCAL LTWIN	win;
LOCAL UD	logpos = 0;
LOCAL UB	input[CON_LINE_MAX];
LOCAL INT	ninput = 0;
LOCAL UB	hist[HIST_MAX][CON_LINE_MAX];
LOCAL INT	nhist = 0, hat = 0;
LOCAL BOOL	follow = TRUE;

/*
 * A program of the POSIX layer on the pty (con_job.c): the window is
 * its terminal's screen (lterm/lt_vt.c) until it ends, the keys go to it
 */
LOCAL LTVT	vt;
LOCAL BOOL	vtmode = FALSE;
LOCAL BOOL	vtfull = TRUE;		/* the whole screen to draw next */

/* ---------------------------------------------------------------- what goes out */

EXPORT void say( CONST char *s )
{
	tm_putstring((CONST UB *)s);
}

EXPORT void say_u( CONST UB *s )
{
	tm_putstring(s);
}

EXPORT void say_num( D v )
{
	UB	b[24];

	(void)lt_num(b, v);
	tm_putstring(b);
}

/* ---------------------------------------------------------------- letters */

/*
 * The full-width forms of ASCII (U+FF01 to U+FF5E, UTF-8 EF BC 81 to
 * EF BD 9E) and the ideographic space (E3 80 80) as ASCII, in place
 */
EXPORT void con_fold( UB *s )
{
	INT	i = 0, o = 0;

	while ( s[i] != 0 ) {
		if ( s[i] == 0xEF && ( s[i + 1] == 0xBC || s[i + 1] == 0xBD ) && s[i + 2] >= 0x80 && s[i + 2] <= 0xBF ) {
			UW	cp = 0xF000 | ( (UW)( s[i + 1] & 0x3F ) << 6 ) | ( s[i + 2] & 0x3F );

			if ( cp >= 0xFF01 && cp <= 0xFF5E ) {
				s[o++] = (UB)( cp - 0xFF01 + 0x21 );
				i += 3;
				continue;
			}
		}
		if ( s[i] == 0xE3 && s[i + 1] == 0x80 && s[i + 2] == 0x80 ) {
			s[o++] = ' ';
			i += 3;
			continue;
		}
		s[o++] = s[i++];
	}
	s[o] = 0;
}

EXPORT BOOL con_same_name( CONST UB *a, CONST UB *b )
{
	UB	x[OB_NAME_MAX * 2], y[OB_NAME_MAX * 2];
	INT	i;

	for ( i = 0; a[i] != 0 && i < (INT)sizeof(x) - 1; i++ ) x[i] = a[i];
	x[i] = 0;
	for ( i = 0; b[i] != 0 && i < (INT)sizeof(y) - 1; i++ ) y[i] = b[i];
	y[i] = 0;
	con_fold(x);
	con_fold(y);
	for ( i = 0; x[i] != 0 && x[i] == y[i]; i++ ) ;
	return (BOOL)( x[i] == 0 && y[i] == 0 );
}

/* Letters onto the end of the line, as many whole ones as fit */
EXPORT void con_insert( CONST UB *s, INT n )
{
	INT	i, k;

	if ( vtmode ) {
		job_pty_write(s, n);		/* typed to the program, its terminal echoes */
		return;
	}

	for ( i = 0; i < n; i = k ) {
		for ( k = i + 1; k < n && ( s[k] & 0xC0 ) == 0x80; k++ ) ;
		if ( ninput + ( k - i ) > CON_LINE_MAX - 1 ) break;
		while ( i < k ) input[ninput++] = s[i++];
	}
	input[ninput] = 0;
	term.back = 0;
}

/* ---------------------------------------------------------------- the window */

LOCAL INT prompt( UB *out, INT max )
{
	CONST UB *h = box_here_name();
	INT	n = 0, i;

	for ( i = 0; h[i] != 0 && n < max - 3; i++ ) out[n++] = h[i];
	for ( i = 0; PROMPT_END[i] != 0 && n < max - 1; i++ ) out[n++] = (UB)PROMPT_END[i];
	out[n] = 0;
	return n;
}

LOCAL void redraw( void )
{
	static UB	tail[OB_NAME_MAX + 4 + CON_LINE_MAX];
	INT		n, i;
	BOOL		running = job_running();

	if ( vtmode ) {
		lt_vt_draw(&vt, &win, (BOOL)!ime_busy(), vtfull);
		vtfull = FALSE;
		ime_draw(&win, vt.caret_x, vt.caret_y);
		(void)wm_obj_flush(win.kw, NULL);
		return;
	}

	if ( !running ) {
		n = prompt(tail, OB_NAME_MAX + 4);
		for ( i = 0; i < ninput; i++ ) tail[n++] = input[i];
		tail[n] = 0;
	}
	term.hold = TRUE;
	lt_draw(&term, &win, running ? NULL : tail, (BOOL)( !running && !ime_busy() ));
	if ( !running ) {
		ime_draw(&win, term.tail_x, term.tail_y);
	}
	(void)wm_obj_flush(win.kw, NULL);
}

/* What has gone to the console since the last look; TRUE when anything did */
LOCAL BOOL take_log( void )
{
	UB	buf[512];
	INT	n;
	BOOL	got = FALSE;

	while ( ( n = tm_log_read(&logpos, buf, sizeof(buf)) ) > 0 ) {
		lt_put(&term, buf, n);
		got = TRUE;
	}
	if ( got && follow ) {
		term.back = 0;
	}
	return got;
}

/* ---------------------------------------------------------------- commands */

/* The name of each object of a kind the object layer lists */
/*
 * The objects of a kind, every one, a page at a time: each page asked
 * from the last object of the one before (the console keeps no heap)
 */
#define LIST_PAGE	64

LOCAL void list_names( UINT type, CONST char *none )
{
	TS_UUID	ids[LIST_PAGE], last;
	T_OBREF	r;
	INT	cnt, i, all = 0;

	do {
		cnt = 0;
		if ( ob_lst_obj(type, 0, ( all > 0 ) ? &last : NULL, ids, LIST_PAGE, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&ids[i], &r) >= E_OK ) {
				say("  ");
				tm_putstring(( r.name[0] != 0 ) ? r.name : (CONST UB *)"(名前なし)");
				say("\n");
			}
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == LIST_PAGE );
	if ( all == 0 ) {
		say(none);
	}
}

/*
 * The processes: each one's number and the name of the program object
 * it was started from, which its object links to
 */
LOCAL void cmd_ps( void )
{
	TS_UUID	ids[LIST_PAGE], last, prog;
	T_OBREF	r, pr;
	INT	cnt, i, k, n, all = 0;

	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_PROCESS, 0, ( all > 0 ) ? &last : NULL, ids, LIST_PAGE, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&ids[i], &r) < E_OK ) continue;
			for ( k = 0; r.name[k] != 0 && r.name[k] != ' '; k++ ) ;
			say("  ");
			say_u(( r.name[k] == ' ' ) ? r.name + k + 1 : r.name);
			say("  ");
			n = 0;
			if ( ob_acc_obj(&ids[i], OB_OP_READ) < E_OK ) {
				say("(読めません)");	/* asked, not opened to be refused */
			} else if ( ob_lst_lnk(&ids[i], NULL, &prog, 1, &n) >= E_OK && n > 0
			  && ob_ref_obj(&prog, &pr) >= E_OK && pr.name[0] != 0 ) {
				say_u(pr.name);
			} else {
				say("(プログラムの実身はありません)");
			}
			say("\n");
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == LIST_PAGE );
	if ( all == 0 ) {
		say("プロセスはありません\n");
	}
}

LOCAL void cmd_date( void )
{
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		t[48];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&clk, OB_OP_READ);

	if ( k <= 0 || ob_rea_rec(k, 1, 0, t, sizeof(t) - 2, &asz) < E_OK || asz == 0 ) {
		say("時計が読めません\n");
	} else {
		t[asz] = '\n';
		t[asz + 1] = 0;
		tm_putstring(t);
	}
	if ( k > 0 ) ob_cls_obj(k);
}

LOCAL void cmd_uptime( void )
{
	UD	ns, s;

	if ( ts_get_mono(&ns) < E_OK ) {
		return;
	}
	s = ns / 1000000000U;
	say_num((D)( s / 86400 ));
	say(" 日 ");
	say_num((D)( s / 3600 % 24 ));
	say(" 時間 ");
	say_num((D)( s / 60 % 60 ));
	say(" 分 ");
	say_num((D)( s % 60 ));
	say(" 秒\n");
}

LOCAL void cmd_help( void )
{
	say("help        この一覧\n"
	    "clear       ウインドウを消す\n"
	    "echo 文     文をそのまま出す\n"
	    "date        今の日時\n"
	    "uptime      起動からの時間\n"
	    "ps          プロセスの一覧(番号とプログラムの実身名)\n"
	    "dev         デバイスの一覧\n"
	    "pwd         いまいる所のパス\n"
	    "ls [-l] [パス]  仮身を実身名で並べる(/ は中に仮身を持つ実身、-l で UUID とレコード)\n"
	    "cd [パス]   仮身を持つ実身に移る(.. で戻る、/ でルート、パスなしで bin)\n"
	    "名前 [引数]  プログラムを実行する(いまいる所、次に bin から探す)\n"
	    "Tab で名前を補う。実行中は Ctrl+C か Esc で止める\n");
}

/*
 * The line cut into words: spaces part them, except one after '\' or
 * between '"'s, which stays in the word as "\ "; the other escapes are
 * kept for the paths, the quotes are taken off.
 */
LOCAL INT words( UB *line, UB **argv, UB *store, INT smax )
{
	INT	argc = 0, n = 0;
	BOOL	quote = FALSE, in = FALSE;
	UB	*p;

	for ( p = line; *p != 0 && n < smax - 3; p++ ) {
		if ( *p == '"' ) {
			quote = (BOOL)!quote;
			if ( !in ) {
				if ( argc == CON_ARGS_MAX ) break;
				argv[argc++] = store + n;
				in = TRUE;
			}
			continue;
		}
		if ( *p == ' ' && !quote ) {
			if ( in ) {
				store[n++] = 0;
				in = FALSE;
			}
			continue;
		}
		if ( !in ) {
			if ( argc == CON_ARGS_MAX ) break;
			argv[argc++] = store + n;
			in = TRUE;
		}
		if ( *p == '\\' && p[1] != 0 ) {
			store[n++] = *p++;
		} else if ( *p == ' ' ) {
			store[n++] = '\\';
		}
		store[n++] = *p;
	}
	if ( in ) store[n] = 0;
	return argc;
}

/* ---------------------------------------------------------------- a program on the pty */

/* A program started; one on the pty makes the window its terminal's screen */
LOCAL void start_job( CONST TS_UUID *prog, CONST UB *name, INT argc, UB **argv )
{
	INT	rows, cols;

	lt_vt_fit(&vt, &win, &rows, &cols);
	if ( job_start(prog, name, argc, argv, rows, cols) < E_OK ) return;
	if ( job_on_pty() ) {
		lt_vt_init(&vt, &term, rows, cols);
		vtmode = TRUE;
		vtfull = TRUE;
	}
}

/*
 * What the program wrote, onto the screen, and the terminal's answers
 * back to it; TRUE when anything came. Read until there is nothing,
 * which asks to be told of what comes next, or for 64 pieces at most,
 * the rest left for the next round (vt_more) rather than waited for.
 */
LOCAL BOOL	vt_more = FALSE;

LOCAL BOOL vt_pump( void )
{
	static UB	buf[1024];
	INT		n = 0, k = 0;
	BOOL		got = FALSE;

	while ( k++ < 64 && ( n = job_pty_read(buf, sizeof(buf)) ) > 0 ) {
		lt_vt_put(&vt, buf, n);
		got = TRUE;
	}
	vt_more = (BOOL)( n > 0 );
	while ( ( n = lt_vt_reply(&vt, buf, sizeof(buf)) ) > 0 ) job_pty_write(buf, n);
	return got;
}

/* The program ended: its screen into the record of lines, the window the console's again */
LOCAL void vt_end( void )
{
	(void)vt_pump();
	lt_vt_flush(&vt);
	job_pty_close();
	vtmode = FALSE;
	term.back = 0;
}

/* The window's size changed: the screen and the terminal's size with it */
LOCAL void vt_fit( void )
{
	INT	rows, cols;

	lt_vt_fit(&vt, &win, &rows, &cols);
	if ( rows != vt.rows || cols != vt.cols ) {
		lt_vt_resize(&vt, rows, cols);
		job_pty_size(rows, cols);
	}
	vtfull = TRUE;
}

/* A key typed at the program: the bytes a terminal sends for it */
LOCAL void vt_key( CONST T_OBNTM *m )
{
	BOOL	ctrl = (BOOL)( ( m->mods & MOD_CTRL ) != 0 );
	BOOL	shift = (BOOL)( ( m->mods & MOD_SHIFT ) != 0 );
	UB	b[8];
	INT	n = 0;

	if ( ime_busy() && ime_key(m->code, m->mods, m->imode) ) return;
	switch ( m->code ) {
	case KEY_ENTER:
	case KEY_KPENTER:	b[n++] = '\r'; break;
	case KEY_BS:		b[n++] = 0x7F; break;
	case KEY_TAB:		if ( shift ) n = lt_vt_key(&vt, VTK_BTAB, b); else b[n++] = '\t'; break;
	case KEY_ESC:		b[n++] = 0x1B; break;
	case KEY_UP:		n = lt_vt_key(&vt, VTK_UP, b); break;
	case KEY_DOWN:		n = lt_vt_key(&vt, VTK_DOWN, b); break;
	case KEY_RIGHT:		n = lt_vt_key(&vt, VTK_RIGHT, b); break;
	case KEY_LEFT:		n = lt_vt_key(&vt, VTK_LEFT, b); break;
	case KEY_HOME:		n = lt_vt_key(&vt, VTK_HOME, b); break;
	case KEY_END:		n = lt_vt_key(&vt, VTK_END, b); break;
	case KEY_INS:		n = lt_vt_key(&vt, VTK_INS, b); break;
	case KEY_DEL:		n = lt_vt_key(&vt, VTK_DEL, b); break;
	case KEY_PGUP:		n = lt_vt_key(&vt, VTK_PGUP, b); break;
	case KEY_PGDN:		n = lt_vt_key(&vt, VTK_PGDN, b); break;
	default:
		if ( ctrl && wm_key_cmd(m->code) >= KEY_A && wm_key_cmd(m->code) <= KEY_Z ) {
			b[n++] = (UB)( wm_key_cmd(m->code) - KEY_A + 1 );	/* ^A .. ^Z of the letter typed: ^C stops what runs */
			break;
		}
		if ( ime_key(m->code, m->mods, m->imode) ) return;	/* its letters come through con_insert */
		b[0] = wm_key_char(m->code, m->mods);
		if ( !ctrl && b[0] >= 0x20 && b[0] < 0x7F ) n = 1;
		break;
	}
	if ( n > 0 ) job_pty_write(b, n);
}

/* A program by its name, run */
LOCAL void run_prog( INT argc, UB **argv )
{
	TS_UUID	prog;
	UB	pname[OB_NAME_MAX + 16];
	ER	er = box_find_prog(argv[0], &prog, pname, sizeof(pname));

	if ( er == E_NOEXS ) {
		say_u(argv[0]);
		say(": そういう命令もプログラムもありません(help で一覧)\n");
		return;
	}
	if ( er < E_OK ) {
		return;				/* why was said */
	}
	start_job(&prog, pname, argc, argv);
}

LOCAL void run( void )
{
	static UB	line[CON_LINE_MAX], store[CON_LINE_MAX * 2];
	UB		*argv[CON_ARGS_MAX], p[OB_NAME_MAX + 4];
	INT		k, argc;

	input[ninput] = 0;
	(void)prompt(p, sizeof(p));
	say_u(p);
	tm_putstring(input);
	say("\n");
	if ( ninput > 0 ) {
		if ( nhist == HIST_MAX ) {
			for ( k = 0; k < HIST_MAX - 1; k++ ) memcpy(hist[k], hist[k + 1], CON_LINE_MAX);
			nhist--;
		}
		memcpy(hist[nhist++], input, CON_LINE_MAX);
	}
	hat = nhist;
	memcpy(line, input, CON_LINE_MAX);
	ninput = 0;
	input[0] = 0;
	term.back = 0;

	con_fold(line);
	argc = words(line, argv, store, sizeof(store));
	if ( argc == 0 ) {
		return;
	}
	if ( lt_same(argv[0], "help") ) {
		cmd_help();
	} else if ( lt_same(argv[0], "clear") ) {
		lt_clear(&term);
	} else if ( lt_same(argv[0], "echo") ) {
		UB	*c = line;

		while ( *c == ' ' ) c++;
		c += 4;
		while ( *c == ' ' ) c++;
		tm_putstring(c);
		say("\n");
	} else if ( lt_same(argv[0], "date") ) {
		cmd_date();
	} else if ( lt_same(argv[0], "uptime") ) {
		cmd_uptime();
	} else if ( lt_same(argv[0], "ps") ) {
		cmd_ps();
	} else if ( lt_same(argv[0], "dev") ) {
		list_names(OB_T_DEVICE, "デバイスはありません\n");
	} else if ( lt_same(argv[0], "pwd") ) {
		cmd_pwd();
	} else if ( lt_same(argv[0], "ls") ) {
		cmd_ls(argc, argv);
	} else if ( lt_same(argv[0], "cd") ) {
		cmd_cd(argc, argv);
	} else {
		run_prog(argc, argv);
	}
}

/* ---------------------------------------------------------------- completion */

/* Tab: the last word of the line completed from the names of the links of the place */
LOCAL void complete( void )
{
	static UB	pre[CON_LINE_MAX], common[CON_LINE_MAX], a[CON_LINE_MAX], b[CON_LINE_MAX];
	INT		ws = 0, i, n, k;
	BOOL		first = TRUE, quote = FALSE;

	input[ninput] = 0;
	for ( i = 0; i < ninput; i++ ) {
		if ( input[i] == '\\' && i + 1 < ninput ) {
			i++;
		} else if ( input[i] == '"' ) {
			quote = (BOOL)!quote;
		} else if ( input[i] == ' ' && !quote ) {
			for ( k = ws; k < i && input[k] == ' '; k++ ) ;
			if ( k < i ) first = FALSE;
			ws = i + 1;
		}
	}
	if ( quote ) return;
	for ( i = ws; i < ninput && i - ws < CON_LINE_MAX - 1; i++ ) pre[i - ws] = input[i];
	pre[i - ws] = 0;
	con_fold(pre);
	n = box_complete(pre, first, common, CON_LINE_MAX, TRUE);
	if ( n <= 0 ) return;

	/* the line takes what was found only when it says more than was typed */
	for ( i = 0; pre[i] != 0; i++ ) a[i] = pre[i];
	a[i] = 0;
	for ( i = 0; common[i] != 0; i++ ) b[i] = common[i];
	b[i] = 0;
	con_fold(b);
	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	if ( a[i] != 0 ) return;
	ninput = ws;
	con_insert(common, (INT)strlen((CONST char *)common));
}

/* ---------------------------------------------------------------- input */

LOCAL void key( CONST T_OBNTM *m, BOOL *p_done )
{
	BOOL	ctrl = (BOOL)( ( m->mods & MOD_CTRL ) != 0 );
	BOOL	shift = (BOOL)( ( m->mods & MOD_SHIFT ) != 0 );
	UB	ch;

	if ( ctrl && wm_key_cmd(m->code) == KEY_E ) {
		*p_done = TRUE;			/* 閉じる, the window menu's letter */
		return;
	}
	if ( ( m->code == KEY_PGUP || m->code == KEY_PGDN ) && shift ) {
		lt_wind(&term, ( m->code == KEY_PGUP ) ? 10 : -10);
		return;
	}
	if ( vtmode ) {
		vt_key(m);
		return;
	}
	if ( job_running() ) {
		/* the program has no input: the keys only stop it */
		if ( ( ctrl && m->code == KEY_C ) || m->code == KEY_ESC ) job_stop();
		return;
	}
	if ( ime_key(m->code, m->mods, m->imode) ) {
		term.back = 0;
		return;
	}
	switch ( m->code ) {
	case KEY_ENTER:
	case KEY_KPENTER:
		run();
		break;
	case KEY_BS:
		while ( ninput > 0 && ( input[--ninput] & 0xC0 ) == 0x80 ) ;
		input[ninput] = 0;
		break;
	case KEY_ESC:
		ninput = 0;
		input[0] = 0;
		break;
	case KEY_TAB:
		complete();
		break;
	case KEY_UP:
	case KEY_DOWN:
		if ( nhist == 0 ) break;
		hat += ( m->code == KEY_UP ) ? -1 : 1;
		if ( hat < 0 ) hat = 0;
		if ( hat >= nhist ) {
			hat = nhist;
			ninput = 0;
		} else {
			memcpy(input, hist[hat], CON_LINE_MAX);
			ninput = (INT)strlen((CONST char *)input);
		}
		break;
	default:
		if ( ctrl && m->code == KEY_C ) {
			ninput = 0;
			input[0] = 0;
			break;
		}
		ch = wm_key_char(m->code, m->mods);
		if ( !ctrl && ch >= 0x20 && ch < 0x7F ) {
			con_insert(&ch, 1);
		}
		break;
	}
}

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	INT	i;

	for ( i = 0; code[i] != 0; i++ ) {
		if ( sel->code[i] != (UB)code[i] ) return FALSE;
	}
	return (BOOL)( sel->code[i] == 0 );
}

/* The second button: the menu, and what was chosen in it */
LOCAL void menu( CONST T_OBNTM *m, BOOL *p_done )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;

	if ( lt_uuid(SYSDEF_MENU_CONSOLE, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	(void)mn_chg_atr(mid, NULL, "follow", follow ? MN_TICK : 0);
	if ( mn_pop_men(mid, win.kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		if ( is_code(&sel, "close") ) {
			*p_done = TRUE;
		} else if ( is_code(&sel, "clear") ) {
			lt_clear(&term);
		} else if ( is_code(&sel, "history") ) {
			lt_clear(&term);
			logpos = 0;
			(void)take_log();
		} else if ( is_code(&sel, "follow") ) {
			follow = (BOOL)!follow;
			if ( follow ) term.back = 0;
		}
	}
	(void)mn_del_men(mid);
}

/* The program whose object the console was started with, run as if its name had been typed */
LOCAL void run_given( void )
{
	TS_UUID	u;
	UB	pname[OB_NAME_MAX + 16], p[OB_NAME_MAX + 4];
	UB	*argv[1];

	if ( ts_get_arg(&u, sizeof(u)) != (SZ)sizeof(u) || box_prog_named(&u, pname, sizeof(pname)) < E_OK ) {
		return;
	}
	(void)prompt(p, sizeof(p));
	say_u(p);
	say_u(pname);
	say("\n");
	argv[0] = pname;
	start_job(&u, pname, 1, argv);
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	BOOL	done = FALSE, again;
	INT	k;
	ID	klog = 0;

	lt_clear(&term);
	box_init();
	if ( lt_open(&win, "コンソール", win_json) < E_OK ) {
		tm_putstring((CONST UB *)"console: no window\n");
		return 1;
	}
	job_port(win.port);
	/* what goes to the console told as it comes (OB_SYS_LOG), once the log has been read to its end */
	{
		TS_UUID	u;
		T_OBNTF	req;

		if ( lt_uuid(SYSDEF_SYSTEM, &u) >= E_OK && ( klog = ob_opn_obj(&u, OB_OP_ATRRD) ) > 0 ) {
			memset(&req, 0, sizeof(req));
			req.events = OB_E_CHANGE;
			(void)ob_ntf_evt(klog, OB_SYS_LOG, &req, win.port);
		}
	}
	(void)take_log();
	run_given();
	redraw();

	while ( !done ) {
		again = FALSE;
		/* asleep until the window, the log, the program's output or its end has something */
		for ( TMO tmo = vt_more ? TMO_POL : TMO_FEVR;
		      !done && ob_rea_tmo(win.port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) {
			switch ( m.event ) {
			case OB_E_CLOSE:
			case OB_E_DELETE:
				done = TRUE;
				break;
			case OB_E_PRESS:
				if ( m.recno == OB_WR_CAND ) {
					ime_pick((INT)m.code);	/* a name on the list of candidates */
				} else if ( m.code == 1 ) {
					menu(&m, &done);
				} else {
					ime_press();
					(void)lt_pointer(&term, &m);
				}
				again = TRUE;
				break;
			case OB_E_MOVE:
			case OB_E_RELEASE:
			case OB_E_WHEEL:
			case OB_E_SCROLL:
				if ( lt_pointer(&term, &m) ) again = TRUE;
				break;
			case OB_E_KEY:
				key(&m, &done);
				again = TRUE;
				break;
			case OB_E_REDRAW:
				if ( vtmode ) vt_fit();
				again = TRUE;
				break;
			default:
				break;
			}
		}
		if ( done ) {
			break;
		}
		if ( vtmode && vt_pump() ) {
			again = TRUE;
		}
		if ( job_poll() ) {
			if ( vtmode ) vt_end();
			again = TRUE;
		}
		if ( take_log() || again ) {
			redraw();
		}
	}
	if ( klog > 0 ) ob_cls_obj(klog);

	/* a program still running ends with the window */
	job_stop();
	for ( k = 0; k < 50 && job_running(); k++ ) {
		(void)job_poll();
		tk_dly_tsk(TICK_MS);
	}
	ime_end();
	lt_close(&win);
	return 0;
}
