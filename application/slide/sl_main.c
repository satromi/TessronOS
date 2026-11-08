/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sl_main.c
 *	スライド: the objects a document links to, shown one at a time over
 *	the whole screen, as a program running as a process (design 11.11)
 *
 *	The document opened is the list of the slides (sl_deck.c). A slide
 *	is drawn by the same layout as the editors draw it, linked into this
 *	program (include/ts/tvport.h): its drawing calls are the kernel's,
 *	done on the environment of this program's window, and what its links
 *	show is asked of the kernel's store (lib/libtv/tv_src.c). The window
 *	is an ordinary one without a frame as large as the screen, and
 *	covers the message line too (WM_ATTR_SCREEN): the window manager
 *	takes the line down while it is open and puts it back after, however
 *	the program ends.
 *
 *	A slide that holds a <realtime> is played by the desktop's player in
 *	this window (DT_RQ_RT_OPEN with DT_F_RT_SLIDE, ts/dtreq.h), as
 *	MPEG Player has it play: what the person does goes to the slide's
 *	program first, and what the program asks of the slides comes back
 *	in the player's answers (DT_RTS_GO), asked for every tenth of a
 *	second while it plays.
 *
 *	The second button opens the slides' menu (SYSDEF_MENU_SLIDE), 実行
 *	for the link under the pointer; ジャンプ and 背景色変更 choose from a
 *	panel of the slides' data box (SYSDEF_BOX_SLIDE); 印刷 hands each
 *	slide's record to the desktop as a page (ts/uiprn.h). Nothing in the
 *	slides is changed.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/uapp.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/uiprn.h>
#include <ts/hid.h>
#include <ts/time.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/tvsrc.h>
#include <ts/fn.h>
#include <ts/rtad.h>
#include <ts/slide.h>
#include <ts/wm.h>

#define SL_PAD		16		/* round a document on the slide, at life size */
#define SL_PAGE_MAX	256
#define RT_POLL_MS	100		/* how often a playing slide's program is asked what it wants */
#define SWIPE_X		80		/* how far across a press must go to be a swipe */
#define SWIPE_Y		60

#define PN_JUMP		1
#define PN_GROUND	2
#define P_LIST		11
#define P_CANCEL	101
#define P_OK		102

#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_SPACE		0x2C
#define K_HOME		0x4A
#define K_PGUP		0x4B
#define K_END		0x4D
#define K_PGDN		0x4E
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define K_KPENTER	0x58
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )

#define C_END		0x00000000U
#define C_END_TEXT	0x00FFFFFFU
#define C_FAIL		0x00FF0000U
#define C_WHITE		0x00FFFFFFU

#define APP_MAX		32		/* programs offered in 実行 */
#define APP_ID		64
#define APP_NAME	WM_LABEL_MAX

/* ---------------------------------------------------------------- what it holds */

static TS_UUID	g_doc;			/* the document of the slides */
static UB	g_name[OB_NAME_MAX];
static TS_UUID	g_page[SL_PAGE_MAX];	/* what its links point at, in order */
static INT	g_n, g_cur;
static BOOL	g_end;			/* the screen after the last */
static INT	g_tw, g_th;		/* the screen the slides are made for */
static UW	g_back = TAD_COL_NONE;	/* round the slides: chosen in the menu */

static TS_UUID	g_win, g_ch;
static ID	g_kw, g_port;
static INT	g_gid, g_w, g_h;
static UI	g_ui;
static BOOL	g_closing;
static BOOL	g_down;
static INT	g_down_x, g_down_y;

/* the player of the slide shown, when it holds a <realtime> */
static UINT	g_rt;			/* the player's number, 0 none */
static INT	g_rt_page;
static UINT	g_rt_flags;		/* DT_RTS_* as it said last */

/* the programs 実行 offers for the link under the pointer */
static UB	g_app_id[APP_MAX][APP_ID];
static UB	g_app_name[APP_MAX][APP_NAME];
static BOOL	g_app_kind[APP_MAX];
static INT	g_napp;
static T_VOBJ	g_link;			/* that link */
static BOOL	g_has_link;

/* when the key or press being answered happened (the notice's time, ns since start), 0 none */
static UD	g_input_at;

static void sl_draw( void );
static void sl_go( INT to );

/* ---------------------------------------------------------------- saying things */

static void note( CONST char *s )
{
	char	line[TAD_NAME_MAX + 128];

	snprintf(line, sizeof(line), "slide: %s\n", s);
	tm_putstring((CONST UB *)line);
}

static UD now_ns( void )
{
	UD	t = 0;

	(void)ts_get_mono(&t);
	return t;
}

/* ---------------------------------------------------------------- the desktop */

/* A request to the desktop, its panels over this window; the answer waited for */
static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0;
	ID		k, ka;
	ER		er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	rq->win = g_win;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* ---------------------------------------------------------------- the player */

static void rt_take( CONST T_DTANS *an )
{
	g_rt_flags = an->flags;
	if ( ( an->flags & DT_RTS_GO ) == 0 ) return;
	/* what the slide's program asked of the slides */
	if ( an->look.disp == RT_ACT_NEXT ) {
		if ( g_end ) g_closing = TRUE;
		else sl_go(g_cur + 1);
	} else if ( an->look.disp == RT_ACT_PREV ) {
		if ( g_end ) sl_go(g_n - 1);
		else if ( g_cur > 0 ) sl_go(g_cur - 1);
	} else if ( an->look.disp == RT_ACT_INDEX ) {
		sl_go((INT)an->look.autoopen);
	}
}

/* Something done to the player; its answer taken. FALSE when there is no player any more */
static BOOL rt_do( UINT op, INT x, INT y )
{
	T_DTREQ	rq;
	T_DTANS	an;

	if ( g_rt == 0 ) return FALSE;
	memset(&rq, 0, sizeof(rq));
	memset(&an, 0, sizeof(an));
	rq.req = DT_RQ_RT_DO;
	rq.carry = g_rt;
	rq.recno = (INT)op;
	rq.grab_x = x;
	rq.grab_y = y;
	if ( ask_desktop(&rq, &an, 5000) < E_OK ) {
		g_rt = 0;
		g_rt_flags = 0;
		return FALSE;
	}
	rt_take(&an);
	return TRUE;
}

static void rt_end( void )
{
	T_DTREQ	rq;
	T_DTANS	an;

	if ( g_rt == 0 ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_RT_CLOSE;
	rq.carry = g_rt;
	(void)ask_desktop(&rq, &an, 5000);
	g_rt = 0;
	g_rt_flags = 0;
}

/* The slide shown played by the desktop's player, fitted to the window as a slide is */
static BOOL rt_open( UW ground, UW paper )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	memset(&an, 0, sizeof(an));
	rq.req = DT_RQ_RT_OPEN;
	rq.target = g_page[g_cur];
	rq.recno = 0;
	rq.flags = DT_F_RT_SLIDE;
	rq.look.w = g_w;
	rq.look.h = g_h;
	rq.look.bgcol = ground;
	rq.look.chcol = paper;
	rq.grab_x = g_tw;
	rq.grab_y = g_th;
	rq.carry = (UINT)( g_cur + 1 );
	if ( ask_desktop(&rq, &an, 10000) < E_OK ) return FALSE;
	g_rt = an.how;
	g_rt_page = g_cur;
	g_rt_flags = an.flags;
	return TRUE;
}

/* ---------------------------------------------------------------- drawing */

/* Letters in the system's face, px tall */
static INT text( INT x, INT y, CONST char *s, UW col, INT px, BOOL measure )
{
	ID	fid = fn_system();

	if ( fid <= 0 || fn_set_size(fid, px) < E_OK ) return 0;
	return measure ? fn_width(fid, (CONST UB *)s) : fn_draw(g_gid, fid, x, y, (CONST UB *)s, col);
}

/* A line of words in the middle of the window */
static void centre( CONST char *s, UW col, INT px )
{
	INT	w = text(0, 0, s, col, px, TRUE);

	(void)text(( g_w - w ) / 2, g_h / 2 + px / 3, s, col, px, FALSE);
}

/* A link to an object, as the slides' document has it: only which object */
static void link_to( CONST TS_UUID *id, T_VOBJ *v )
{
	memset(v, 0, sizeof(*v));
	v->target = *id;
	v->disp = TAD_D_DEFAULT;
}

/* The slide's own colour: its window's background colour, or white */
static UW page_paper( void )
{
	T_VOBJ	v;
	UW	paper = C_WHITE;

	link_to(&g_page[g_cur], &v);
	if ( tv_src_look(&v, NULL, 0, &paper) < 0 || paper == TV_PAPER_LOOK ) paper = C_WHITE;
	return paper;
}

/* One slide drawn into area, magnified z / den; FALSE when there was nothing to draw */
static BOOL draw_page( INT gid, CONST TS_UUID *page, CONST T_DPRECT *area, INT z, INT den, UW paper )
{
	T_TAD	*rec = tv_src_get(page, 0);
	T_TVDOC	*d;
	T_TVFIG	*f;

	if ( !sl_model(rec, &d, &f) ) return FALSE;
	tv_src_self(page);
	tv_src_prefetch(rec);
	if ( f != NULL ) {
		f->zoom = z;
		f->zoom_den = den;
		(void)tv_fig_draw(gid, f, area, 0, 0, rec, paper);
		tv_fig_free(f);
	} else {
		T_DPRECT	r = *area;
		INT		pad = SL_PAD * z / den;

		r.left += pad;
		r.top += pad;
		r.right -= pad;
		d->zoom = z;
		d->zoom_den = den;
		(void)tv_doc_draw(gid, d, &r, 0, rec, paper);
		tv_doc_free(d);
	}
	tv_src_self(NULL);
	return TRUE;
}

/* The link a slide shows at a place of the window, when there is one */
static BOOL link_at( INT x, INT y, T_VOBJ *v )
{
	T_DPRECT	area, r;
	T_TAD		*rec;
	T_TVDOC		*d;
	T_TVFIG		*f;
	INT		z, den;
	ER		er = E_NOEXS;

	if ( g_end ) return FALSE;
	rec = tv_src_get(&g_page[g_cur], 0);
	if ( !sl_model(rec, &d, &f) ) return FALSE;
	z = sl_fit(g_w, g_h, g_tw, g_th, &area, &den);
	if ( f != NULL ) {
		f->zoom = z;
		f->zoom_den = den;
		er = tv_fig_at(f, &area, 0, 0, rec, x, y, v, NULL, NULL);
		tv_fig_free(f);
	} else {
		INT	pad = SL_PAD * z / den;

		r = area;
		r.left += pad;
		r.top += pad;
		r.right -= pad;
		d->zoom = z;
		d->zoom_den = den;
		er = tv_doc_at(d, &r, 0, rec, x, y, v, NULL, NULL);
		tv_doc_free(d);
	}
	return (BOOL)( er >= E_OK );
}

static void sl_draw( void )
{
	T_DPRECT	all, area;
	INT		z, den;
	UW		paper, ground;
	UB		name[TAD_NAME_MAX];
	char		msg[TAD_NAME_MAX + 96];
	T_TAD		*rec;
	T_VOBJ		v;
	UD		t0 = now_ns(), f0 = tv_fn_calls();
	T_TVSRCSTAT	s0, s1;

	tv_src_stat(&s0);
	all.left = all.top = 0;
	all.right = g_w;
	all.bottom = g_h;
	if ( g_end ) {
		rt_end();
		dp_fill_rect(g_gid, &all, C_END);
		centre("マウスクリックでスライドを終了します", C_END_TEXT, 24);
		(void)wm_msg_put("スライド終了");
		(void)wm_obj_flush(g_kw, NULL);
		return;
	}
	paper = page_paper();
	/* round the slide: what the menu chose, else the slide's own colour */
	ground = ( g_back != TAD_COL_NONE ) ? g_back : paper;
	link_to(&g_page[g_cur], &v);
	if ( tv_src_look(&v, name, sizeof(name), NULL) < 0 ) name[0] = 0;
	z = sl_fit(g_w, g_h, g_tw, g_th, &area, &den);
	rec = tv_src_get(&g_page[g_cur], 0);
	if ( g_rt != 0 && g_rt_page != g_cur ) rt_end();
	if ( sl_has_rt(rec) ) {
		if ( g_rt == 0 ) {
			dp_fill_rect(g_gid, &all, ground);
			if ( !rt_open(ground, paper) ) {
				/* no player: the slide shown as it stands, still */
				dp_fill_rect(g_gid, &area, paper);
				(void)draw_page(g_gid, &g_page[g_cur], &area, z, den, paper);
			}
		} else {
			(void)rt_do(DT_RT_REDRAW, 0, 0);
		}
	} else {
		rt_end();
		dp_fill_rect(g_gid, &all, ground);
		dp_fill_rect(g_gid, &area, paper);
		if ( !draw_page(g_gid, &g_page[g_cur], &area, z, den, paper) ) {
			snprintf(msg, sizeof(msg), "スライド「%s」の読み込みに失敗しました", (char *)name);
			centre(msg, C_FAIL, 20);
		}
	}
	/* "2 / 5: name" kept as the message, the slide's place, for when the line is up */
	snprintf(msg, sizeof(msg), "%d / %d: %s", g_cur + 1, g_n, (char *)name);
	(void)wm_msg_put(msg);
	snprintf(msg, sizeof(msg), "%d of %d zoom %d/%d at %d,%d %dx%d%s", g_cur + 1, g_n, z, den,
		 area.left, area.top, area.right - area.left, area.bottom - area.top,
		 ( g_rt != 0 ) ? " playing" : "");
	note(msg);
	(void)wm_obj_flush(g_kw, NULL);
	tv_src_stat(&s1);
	snprintf(msg, sizeof(msg), "drew %d in %d us", g_cur + 1, (INT)( ( now_ns() - t0 ) / 1000 ));
	note(msg);
	if ( g_input_at != 0 ) {
		/* from the key or the press, as the keyboard or the pointer told it, to the page shown */
		snprintf(msg, sizeof(msg), "input to screen %d us", (INT)( ( now_ns() - g_input_at ) / 1000 ));
		note(msg);
		g_input_at = 0;
	}
	/* of the calls of the system the drawing made: the letters, the looks of links, the reading of records */
	snprintf(msg, sizeof(msg), "calls %d text %d look %d read", (INT)( tv_fn_calls() - f0 ),
		 (INT)( s1.asks - s0.asks ), (INT)( s1.reads - s0.reads ));
	note(msg);
}

/* ---------------------------------------------------------------- moving through them */

static void sl_go( INT to )
{
	if ( to >= g_n ) {
		g_end = TRUE;
	} else {
		g_end = FALSE;
		g_cur = ( to < 0 ) ? 0 : to;
	}
	sl_draw();
}

/* The next step: the next slide, or after the last the end, and after the end closed */
static void sl_next( void )
{
	if ( g_end ) {
		g_closing = TRUE;
		return;
	}
	sl_go(g_cur + 1);
}

static void sl_prev( void )
{
	if ( g_end ) {
		sl_go(g_n - 1);
	} else if ( g_cur > 0 ) {
		sl_go(g_cur - 1);
	}
}

/*
 * One of a list chosen in a panel of the slides' data box, over the
 * window: its index, or -1 when it was put away
 */
static INT choose( INT panel, CONST char *const *names, INT n, INT at )
{
	UI	q;
	T_OBNTM	m;
	INT	num, ev, done = 0, got = -1;

	if ( ui_dlg_open(&g_ui, panel, &q) < E_OK ) return -1;
	ui_names(&q, P_LIST, names, n);
	if ( at >= 0 ) ui_set_value(&q, P_LIST, at + 1);	/* a list's value counts its rows from 1, 0 none */
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			done = ( num == P_OK ) ? 1 : -1;
			break;
		case UI_EV_CLOSE:
			done = -1;
			break;
		case UI_EV_NONE:
			if ( m.event == OB_E_KEY && ( m.code == K_ENTER || m.code == K_KPENTER ) ) done = 1;
			break;
		case UI_EV_OTHER:
			if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				g_closing = TRUE;
				done = -1;
			}
			break;
		default:
			break;
		}
	}
	if ( done > 0 ) got = ui_value(&q, P_LIST) - 1;
	ui_dlg_close(&q);
	return ( got >= 0 && got < n ) ? got : -1;
}

/* ジャンプ: the slides by their names, one chosen */
static void sl_jump( void )
{
	char	**names = (char **)calloc((size_t)g_n, sizeof(char *));
	INT	i, at;

	if ( names == NULL ) return;
	for ( i = 0; i < g_n; i++ ) {
		UB	nm[TAD_NAME_MAX];
		T_VOBJ	v;

		link_to(&g_page[i], &v);
		if ( tv_src_look(&v, nm, sizeof(nm), NULL) < 0 ) nm[0] = 0;
		names[i] = (char *)malloc(TAD_NAME_MAX + 16);
		if ( names[i] != NULL ) snprintf(names[i], TAD_NAME_MAX + 16, "%d. %s", i + 1, (char *)nm);
	}
	at = choose(PN_JUMP, (CONST char *const *)names, g_n, g_end ? g_n - 1 : g_cur);
	for ( i = 0; i < g_n; i++ ) free(names[i]);
	free(names);
	if ( g_closing ) return;
	if ( at >= 0 ) sl_go(at);
	else sl_draw();
}

/* 背景色変更: what is round the slides, for as long as the window is open */
static void sl_ground( void )
{
	static CONST char *CONST names[4] = { "白", "黒", "グレー", "青" };
	static CONST UW		cols[4] = { 0x00FFFFFFU, 0x00000000U, 0x00808080U, 0x00000080U };
	INT			at = choose(PN_GROUND, names, 4, -1);

	if ( at >= 0 ) g_back = cols[at];
	if ( !g_closing ) sl_draw();
}

/* 印刷: each slide's record a page, handed to the desktop to print */
static void sl_print( void )
{
	T_UIPRN	up;
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	INT	i;
	char	s[64];

	er = ui_prn_open(&up, &g_doc);
	for ( i = 0; er >= E_OK && i < g_n; i++ ) {
		SZ	len = 0;
		UB	*x = tv_src_read(&g_page[i], 0, &len);

		if ( x == NULL ) continue;
		er = ui_prn_page(&up, x, (INT)len);
		free(x);
	}
	if ( er >= E_OK ) er = ui_prn_ready(&up);
	if ( er >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_name);
		/* the person works the desktop's panels: as long as that takes */
		er = ask_desktop(&rq, &an, 60 * 60 * 1000);
	}
	snprintf(s, sizeof(s), "print %d pages (%d)", up.pages, (INT)er);
	note(s);
	ui_prn_close(&up);
	sl_draw();
}

/* ---------------------------------------------------------------- the menu */

/* 実行 for the link under the pointer: the programs the desktop offers for it */
static void exec_menu( ID mid )
{
	UB	(*rows)[WM_LABEL_MAX];
	TS_UUID	ed;
	T_DTREQ	rq;
	T_DTANS	an;
	INT	n = 1, k;

	g_napp = 0;
	for ( k = 0; k < n && g_napp < APP_MAX; k++ ) {
		INT	i = 0, j;

		memset(&rq, 0, sizeof(rq));
		memset(&an, 0, sizeof(an));
		rq.req = DT_RQ_APPS;
		rq.target = g_link.target;
		rq.recno = k;
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		/* "id\tname" */
		for ( j = 0; i < DT_TEXT_MAX && an.text[i] != 0 && an.text[i] != '\t'; i++ ) {
			if ( j < APP_ID - 1 ) g_app_id[g_napp][j++] = an.text[i];
		}
		g_app_id[g_napp][j] = 0;
		if ( j == 0 || i >= DT_TEXT_MAX || an.text[i] != '\t' ) continue;
		for ( i++, j = 0; i < DT_TEXT_MAX && an.text[i] != 0; i++ ) {
			if ( j < APP_NAME - 1 ) g_app_name[g_napp][j++] = an.text[i];
		}
		g_app_name[g_napp][j] = 0;
		g_app_kind[g_napp] = (BOOL)( ( an.flags & DT_F_KIND ) != 0 );
		g_napp++;
	}
	rows = malloc(WM_LABEL_MAX * (size_t)( g_napp + 1 ));
	if ( rows == NULL ) g_napp = 0;
	for ( k = 0; k < g_napp; k++ ) {
		snprintf((char *)rows[k], WM_LABEL_MAX, "%s", (char *)g_app_name[k]);
	}
	if ( rows != NULL && ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, g_napp);
	}
	free(rows);
}

static void command( CONST char *code, INT index )
{
	char	s[TAD_NAME_MAX + 64];

	if ( strcmp(code, "close") == 0 ) g_closing = TRUE;
	else if ( strcmp(code, "slide.next") == 0 ) sl_next();
	else if ( strcmp(code, "slide.prev") == 0 ) sl_prev();
	else if ( strcmp(code, "slide.jump") == 0 ) sl_jump();
	else if ( strcmp(code, "slide.refresh") == 0 ) {
		rt_end();		/* shown again from its start, read again */
		tv_src_forget(NULL);
		sl_draw();
	} else if ( strcmp(code, "slide.bg") == 0 ) sl_ground();
	else if ( strcmp(code, "slide.print") == 0 ) {
		rt_end();
		sl_print();
	} else if ( strcmp(code, "exec.app") == 0 && g_has_link && index >= 0 && index < g_napp ) {
		T_DTREQ	rq;
		T_DTANS	an;
		ER	er;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_EXEC;
		rq.target = g_link.target;
		rq.recno = -1;
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_app_id[index]);
		if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
		er = ask_desktop(&rq, &an, 5000);
		snprintf(s, sizeof(s), "exec %s %s (%d)", (char *)g_link.name, (char *)g_app_id[index], (INT)er);
		note(s);
	}
}

static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;
	char	s[MN_CODE_MAX + 16];

	if ( ui_uuid(SYSDEF_MENU_SLIDE, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	g_has_link = link_at(m->x, m->y, &g_link);
	if ( g_has_link ) {
		exec_menu(mid);
		(void)mn_chg_atr(mid, NULL, "exec", ( g_napp == 0 ) ? MN_GREY : 0);
	} else {
		(void)mn_chg_atr(mid, NULL, "exec", MN_GREY);
	}
	note("menu");
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		snprintf(s, sizeof(s), "chosen %s", (char *)sel.code);
		note(s);
		command((CONST char *)sel.code, sel.index);
	}
}

/* ---------------------------------------------------------------- what the person does */

static void sl_key( UINT code )
{
	switch ( code ) {
	case K_RIGHT: case K_DOWN: case K_PGDN: case K_SPACE: case K_ENTER: case K_KPENTER:
		sl_next();
		break;
	case K_LEFT: case K_UP: case K_PGUP: case K_BS:
		sl_prev();
		break;
	case K_HOME:
		sl_go(0);
		break;
	case K_END:
		sl_go(g_n - 1);
		break;
	case K_ESC:
		g_closing = TRUE;
		break;
	default:
		break;
	}
}

static void key( CONST T_OBNTM *m )
{
	BOOL	up = (BOOL)( ( m->mods & OB_KEY_UP ) != 0 );

	if ( up ) {
		if ( g_rt != 0 ) (void)rt_do(DT_RT_KEY, (INT)m->code, DT_RT_KEY_UP);
		return;
	}
	if ( ( m->mods & M_CTRL ) != 0 ) {
		/* the letters of the menu's items */
		switch ( wm_key_char(m->code, 0) ) {
		case 'n': case 'N':	command("slide.next", 0);	break;
		case 'p': case 'P':	command("slide.prev", 0);	break;
		case 'j': case 'J':	command("slide.jump", 0);	break;
		case 'e': case 'E':	command("close", 0);		break;
		default:		break;
		}
		return;
	}
	if ( g_rt != 0 && !g_end ) {
		/* the program's own key first; a program waiting to be begun is begun by Space and Enter */
		if ( rt_do(DT_RT_KEY, (INT)m->code, DT_RT_KEY_MINE) && ( g_rt_flags & DT_RTS_TAKEN ) != 0 ) return;
		if ( ( g_rt_flags & DT_RTS_WAITING ) != 0 && ( m->code == K_SPACE || m->code == K_ENTER ) ) {
			(void)rt_do(DT_RT_TOGGLE, 0, 0);
			return;
		}
		(void)rt_do(DT_RT_KEY, (INT)m->code, 0);
	}
	sl_key(m->code);
}

static void press( CONST T_OBNTM *m )
{
	T_VOBJ	v;

	g_down = TRUE;
	g_down_x = m->x;
	g_down_y = m->y;
	if ( g_rt != 0 && !g_end ) {
		/* a program waiting to be begun is begun; a click it waits for is its */
		if ( ( g_rt_flags & DT_RTS_WAITING ) != 0 ) {
			(void)rt_do(DT_RT_TOGGLE, 0, 0);
			return;
		}
		if ( rt_do(DT_RT_PRESS, m->x, m->y) && ( g_rt_flags & DT_RTS_TAKEN ) != 0 ) return;
	}
	/* a press on a link does not turn the page */
	if ( link_at(m->x, m->y, &v) ) return;
	sl_next();
}

static void released( CONST T_OBNTM *m )
{
	INT	dx = m->x - g_down_x, dy = m->y - g_down_y;

	if ( !g_down ) return;
	g_down = FALSE;
	if ( g_rt != 0 && rt_do(DT_RT_RELEASE, m->x, m->y) && ( g_rt_flags & DT_RTS_TAKEN ) != 0 ) return;
	/* a swipe across the slide, when its program does not take it */
	if ( ( dx > SWIPE_X || dx < -SWIPE_X ) && ( dy < SWIPE_Y && dy > -SWIPE_Y ) ) {
		if ( dx < 0 ) sl_next();
		else sl_prev();
	}
}

/* The window's work area as it is now */
static void work_size( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		g_w = wp.wright - wp.wleft;
		g_h = wp.wbottom - wp.wtop;
	}
	g_ui.w = g_w;
	g_ui.h = g_h;
}

static void event( CONST T_OBNTM *m )
{
	if ( memcmp(&m->uuid, &g_win, sizeof(TS_UUID)) != 0 ) {
		/* a slide or the document changed: what was read of it is read again */
		if ( m->event == OB_E_CHANGE ) {
			tv_src_forget(&m->uuid);
			sl_draw();
		}
		return;
	}
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		g_closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(m);
		else if ( m->code == 0 ) {
			g_input_at = m->when;
			press(m);
			g_input_at = 0;
		}
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m);
		break;
	case OB_E_MOVE:
		if ( g_rt != 0 ) (void)rt_do(DT_RT_MOVE, m->x, m->y);
		break;
	case OB_E_KEY:
		if ( ( m->mods & OB_KEY_UP ) == 0 ) g_input_at = m->when;
		key(m);
		g_input_at = 0;
		break;
	case OB_E_WHEEL:
		if ( m->code != HID_WHEEL_V ) break;	/* the slides go one way: across is let go */
		if ( m->dz < 0 ) sl_next();
		else if ( m->dz > 0 ) sl_prev();
		break;
	case OB_E_REDRAW:
		work_size();
		if ( g_rt != 0 ) {
			T_DTREQ	rq;
			T_DTANS	an;

			memset(&rq, 0, sizeof(rq));
			rq.req = DT_RQ_RT_DO;
			rq.carry = g_rt;
			rq.recno = DT_RT_AREA;
			rq.look.w = g_w;
			rq.look.h = g_h;
			if ( ask_desktop(&rq, &an, 5000) >= E_OK ) rt_take(&an);
		}
		sl_draw();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

/* Notices of changes to an object, at the window's channel */
static void watch( CONST TS_UUID *id )
{
	T_OBNTF	req;
	ID	k = ob_opn_obj(id, OB_OP_READ | OB_OP_ATRRD);	/* that it changed is told to one who may read its attributes */

	if ( k <= 0 ) return;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 1;
	(void)ob_ntf_evt(k, OB_REC_ANY, &req, g_port);
	/* the request goes with the key: the key is kept until the program ends */
}

static ER win_open( void )
{
	T_OBCRE	c;
	T_OBNTF	req;
	char	json[200], shown[TS_UUID_STRLEN + 1];

	ts_uuid_text(&g_doc, shown);
	/* the whole screen, the message line too, with no frame */
	snprintf(json, sizeof(json), "{\"rect\":[0,0,640,480],\"attr\":%d,\"full\":1,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 WM_ATTR_SCREEN, SYSDEF_PROG_SLIDE, shown);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = g_name;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) return E_OBJ;
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
		   | OB_E_DELETE | OB_E_WHEEL;
	req.flags = OB_N_KEYUP;		/* a key let go goes to a playing slide's program */
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( g_kw > 0 ) ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
		g_kw = 0;
		return E_OBJ;
	}
	g_ui.kw = g_kw;
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_SLIDE, &g_ui.box);
	work_size();
	return E_OK;
}

int main( void )
{
	T_OBCRE	c;
	T_OBNTM	m;
	T_OBREF	ref;
	T_TAD	*rec;
	SZ	asz = 0;
	UD	t0 = now_ns();
	char	s[160];
	INT	i;

	if ( ts_get_arg(&g_doc, sizeof(g_doc)) < (SZ)sizeof(g_doc) || ob_ref_obj(&g_doc, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("スライドを開けません");
		return 1;
	}
	snprintf((char *)g_name, sizeof(g_name), "%s", (char *)ref.name);
	tv_src_init();
	rec = tv_src_get(&g_doc, 0);
	if ( rec == NULL ) {
		note("the object cannot be read");
		(void)wm_msg_put("スライドを開けません");
		return 2;
	}
	g_n = sl_pages(rec, g_page, SL_PAGE_MAX, &g_tw, &g_th);
	if ( g_n <= 0 && sl_has_rt(rec) ) {
		/* a program of its own and no links: the one slide is itself, at the size it is made for */
		g_page[0] = g_doc;
		g_n = 1;
		sl_scene_size(rec, &g_tw, &g_th);
	}
	snprintf(s, sizeof(s), "%d pages, made for %d x %d", g_n, g_tw, g_th);
	note(s);
	if ( g_n <= 0 ) {
		(void)wm_msg_put("スライドがありません。仮身を追加してください");
		return 0;
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port <= 0 ) {
		note("no channel");
		return 3;
	}
	if ( win_open() < E_OK ) {
		note("no window");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	tv_src_host(&g_doc);
	watch(&g_doc);
	for ( i = 0; i < g_n; i++ ) watch(&g_page[i]);
	sl_draw();
	snprintf(s, sizeof(s), "up in %d us", (INT)( ( now_ns() - t0 ) / 1000 ));
	note(s);

	while ( !g_closing ) {
		ER	er;
		TMO	tmo = ( g_rt != 0 ) ? RT_POLL_MS : TMO_FEVR;

		er = ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo);
		if ( er >= E_OK ) {
			event(&m);
		} else if ( er == E_TMOUT && g_rt != 0 ) {
			/* what a playing slide's program asks of the slides */
			(void)rt_do(DT_RT_STATE, 0, 0);
		} else if ( er != E_TMOUT ) {
			break;
		}
	}
	rt_end();
	if ( g_kw > 0 ) ob_cls_obj(g_kw);
	(void)ob_del_obj(&g_win);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}
