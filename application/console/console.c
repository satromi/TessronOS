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
 */

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include "../lterm/lterm.h"

#define TICK_MS		40
#define INPUT_MAX	120
#define HIST_MAX	8
#define PROMPT		"TessronOS> "

#define KEY_E		0x08
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define MOD_CTRL	0x11
#define MOD_SHIFT	0x22

LOCAL CONST char win_json[] = "{\"rect\":[100,80,760,500],\"attr\":7}";

LOCAL LTERM	term;
LOCAL LTWIN	win;
LOCAL UD	logpos = 0;
LOCAL UB	input[INPUT_MAX];
LOCAL INT	ninput = 0;
LOCAL UB	hist[HIST_MAX][INPUT_MAX];
LOCAL INT	nhist = 0, hat = 0;
LOCAL BOOL	follow = TRUE;

LOCAL void say( CONST char *s )
{
	tm_putstring((CONST UB *)s);
}

LOCAL void say_num( D v )
{
	UB	b[24];

	(void)lt_num(b, v);
	tm_putstring(b);
}

LOCAL void redraw( void )
{
	UB	tail[sizeof(PROMPT) + INPUT_MAX];
	INT	n = 0, i;

	for ( i = 0; PROMPT[i] != 0; i++ ) tail[n++] = (UB)PROMPT[i];
	for ( i = 0; i < ninput; i++ ) tail[n++] = input[i];
	tail[n] = 0;
	lt_draw(&term, &win, tail, TRUE);
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
LOCAL void list_names( UINT type, CONST char *none )
{
	TS_UUID	ids[64];
	T_OBREF	r;
	INT	cnt = 0, i;

	if ( ob_lst_obj(type, 0, NULL, ids, 64, &cnt) < E_OK || cnt == 0 ) {
		say(none);
		return;
	}
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK ) {
			say("  ");
			tm_putstring(( r.name[0] != 0 ) ? r.name : (CONST UB *)"(名前なし)");
			say("\n");
		}
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

LOCAL void run( void )
{
	UB	*c = input;
	INT	k;

	input[ninput] = 0;
	say(PROMPT);
	tm_putstring(input);
	say("\n");
	if ( ninput > 0 ) {
		if ( nhist == HIST_MAX ) {
			for ( k = 0; k < HIST_MAX - 1; k++ ) memcpy(hist[k], hist[k + 1], INPUT_MAX);
			nhist--;
		}
		memcpy(hist[nhist++], input, INPUT_MAX);
	}
	hat = nhist;
	while ( *c == ' ' ) c++;
	if ( *c == 0 ) {
		/* nothing */
	} else if ( lt_same(c, "help") ) {
		say("help      この一覧\n"
		    "clear     窓を消す\n"
		    "echo 文   文をそのまま出す\n"
		    "date      今の日時\n"
		    "uptime    起動からの時間\n"
		    "ps        プロセスの一覧\n"
		    "dev       デバイスの一覧\n");
	} else if ( lt_same(c, "clear") ) {
		lt_clear(&term);
	} else if ( lt_same(c, "echo") ) {
		c += 4;
		while ( *c == ' ' ) c++;
		tm_putstring(c);
		say("\n");
	} else if ( lt_same(c, "date") ) {
		cmd_date();
	} else if ( lt_same(c, "uptime") ) {
		cmd_uptime();
	} else if ( lt_same(c, "ps") ) {
		list_names(OB_T_PROCESS, "プロセスはありません\n");
	} else if ( lt_same(c, "dev") ) {
		list_names(OB_T_DEVICE, "デバイスはありません\n");
	} else {
		tm_putstring(c);
		say(": そういう命令はありません(help で一覧)\n");
	}
	ninput = 0;
	term.back = 0;
}

/* ---------------------------------------------------------------- input */

LOCAL void key( CONST T_OBNTM *m, BOOL *p_done )
{
	BOOL	ctrl = (BOOL)( ( m->mods & MOD_CTRL ) != 0 );
	BOOL	shift = (BOOL)( ( m->mods & MOD_SHIFT ) != 0 );
	UB	ch;

	if ( ctrl && m->code == KEY_E ) {
		*p_done = TRUE;			/* 閉じる, the window menu's letter */
		return;
	}
	switch ( m->code ) {
	case KEY_ENTER:
		run();
		break;
	case KEY_BS:
		if ( ninput > 0 ) ninput--;
		break;
	case KEY_ESC:
		ninput = 0;
		break;
	case KEY_PGUP:
	case KEY_PGDN:
		if ( shift ) lt_wind(&term, ( m->code == KEY_PGUP ) ? 10 : -10);
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
			memcpy(input, hist[hat], INPUT_MAX);
			ninput = (INT)strlen((CONST char *)input);
		}
		break;
	default:
		ch = wm_key_char(m->code, m->mods);
		if ( !ctrl && ch >= 0x20 && ch < 0x7F && ninput < INPUT_MAX - 1 ) {
			input[ninput++] = ch;
			term.back = 0;
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

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	BOOL	done = FALSE, again;

	lt_clear(&term);
	if ( lt_open(&win, "コンソール", win_json) < E_OK ) {
		tm_putstring((CONST UB *)"console: no window\n");
		return 1;
	}
	(void)take_log();
	redraw();

	while ( !done ) {
		again = FALSE;
		while ( !done && ob_rea_rec(win.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			switch ( m.event ) {
			case OB_E_CLOSE:
			case OB_E_DELETE:
				done = TRUE;
				break;
			case OB_E_PRESS:
				if ( m.code == 1 ) menu(&m, &done);
				else (void)lt_pointer(&term, &m);
				again = TRUE;
				break;
			case OB_E_MOVE:
			case OB_E_RELEASE:
			case OB_E_WHEEL:
				if ( lt_pointer(&term, &m) ) again = TRUE;
				break;
			case OB_E_KEY:
				key(&m, &done);
				again = TRUE;
				break;
			case OB_E_REDRAW:
				again = TRUE;
				break;
			default:
				break;
			}
		}
		if ( done ) {
			break;
		}
		if ( take_log() || again ) {
			redraw();
		}
		tk_dly_tsk(TICK_MS);
	}
	lt_close(&win);
	return 0;
}
