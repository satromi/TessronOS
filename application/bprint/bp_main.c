/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bp_main.c
 *	基本印刷, a utility running as a process (design 11.22)
 *
 *	The desktop starts it for a print job it holds: the pages a program
 *	handed over (DT_RQ_PRINT), an object chosen with 実行 from the menu
 *	of its link, or pages of the desktop's own (a slide, 画面印刷). It
 *	works the panels of the data box (SYSDEF_BOX_BPRINT): 印刷設定 --
 *	which pages, how many copies, the quality, colour -- and from it
 *	印刷詳細設定 -- the printing paper, its way round, the magnification,
 *	whether the links are printed and opened how deep -- and 画面表示,
 *	the sheets shown one at a time in a window of its own before
 *	anything is written. 〈印刷〉 has the desktop write the PDF file and
 *	put its link in the desktop's first window; 〈取消〉 puts the job away.
 *	The program that asked is answered by the desktop when the job ends.
 *
 *	Whatever needs the pages is the desktop's, asked with DT_RQ_PRJOB:
 *	the job and the setting it starts from, how a setting lays the pages
 *	on the paper (DT_PJ_PAPER), a sheet drawn into this program's window
 *	(DT_PJ_SHOW), the file written (DT_PJ_OUT). What the panels set
 *	travels as thirteen numbers in the order of T_PRNSET.
 *
 *	Run when no job waits for it, it ends at once. Started again while
 *	its panel is up, the one at work comes to the front. What it does is
 *	said on the console for the tests ("bprint: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/print.h>
#include <ts/look.h>

/* the panels of the data box */
#define PN_PRINT	1		/* 印刷設定 */
#define PN_DETAIL	2		/* 印刷詳細設定 */
#define PN_STRIP	3		/* 画面表示: the buttons at the top left */
#define PN_STRIP_END	4		/* and 終了 at the right */
#define PN_TELL		5		/* a few words and 了解 */

/* 印刷設定's parts */
#define PS_PAPER	1
#define PS_ALL		2
#define PS_PART		3
#define PS_FROM		4
#define PS_TO		5
#define PS_COPIES	6
#define PS_NORMAL	7		/* 7..9: 通常 高速 高密度 */
#define PS_COLOUR	10
#define PS_OUT		11
#define PS_CANCEL	12
#define PS_OK		13
#define PS_DETAIL	14
#define PS_SHOW		15

/* 印刷詳細設定's */
#define PD_LAYOUT	1
#define PD_LIST		2
#define PD_H		3
#define PD_W		4
#define PD_ROTATE	5
#define PD_AUTO		6
#define PD_GIVEN	7
#define PD_PCT		8
#define PD_LINKS	9
#define PD_EXPAND	10
#define PD_DEPTH	11
#define PD_CANCEL	12
#define PD_OK		13
#define PD_AUTO_PCT	14

/* 画面表示's */
#define PV_PREV		1
#define PV_NEXT		2
#define PV_STD		3
#define PV_ZOOM		4
#define PV_PAGE		5
#define PV_END		1		/* of the strip at the right */
#define PV_BAND		40		/* the band of buttons at the head */
#define PV_PAD		16		/* grey round the sheet, as the desktop draws it */
#define PV_END_W	72
#define PV_STRIP_W	340		/* the strip at the left, as the data box makes it */

/* what a panel was answered with */
#define ANS_CANCEL	0
#define ANS_OK		1
#define ANS_DETAIL	2
#define ANS_SHOW	3

#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_F10		0x43
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define KEY_RCTRL	0xE4

#define MOD_SHIFT	( 0x02 | 0x20 )
#define MOD_ALT		( 0x04 | 0x40 )

typedef struct {
	const char	*name;
	INT		w, h;		/* tenths of a millimetre, as it lies */
} PAPER;

static const PAPER	papers[] = {
	{ "レイアウト用紙と同じ", 0, 0 },
	{ "A3用紙:縦", 2970, 4200 },	{ "A3用紙:横", 4200, 2970 },
	{ "A4用紙:縦", 2100, 2970 },	{ "A4用紙:横", 2970, 2100 },
	{ "A5用紙:縦", 1480, 2100 },	{ "A5用紙:横", 2100, 1480 },
	{ "B4用紙:縦", 2570, 3640 },	{ "B4用紙:横", 3640, 2570 },
	{ "B5用紙:縦", 1820, 2570 },	{ "B5用紙:横", 2570, 1820 },
	{ "はがき:縦", 1000, 1480 },	{ "はがき:横", 1480, 1000 },
	{ "レター:縦", 2159, 2794 },	{ "レター:横", 2794, 2159 },
	{ "その他の用紙", -1, -1 }
};
#define N_PAPER		( (INT)( sizeof(papers) / sizeof(papers[0]) ) )

static UI	g;			/* the data box, and the channel the panels tell */
static T_PRNSET	set;			/* 印刷の指定 as the panels leave it */
static char	jname[DT_TEXT_MAX];	/* what is printed */
static INT	npage;			/* pages of layout paper */
static BOOL	can_show;		/* 画面表示 is offered */

/* how the setting lays the pages, as the desktop last said (DT_PJ_PAPER) */
static INT	pp_w, pp_h, pp_pct, pp_sheets, pp_lw, pp_lh;

static void note( const char *fmt, ... )
{
	char	s[256];
	va_list	ap;
	INT	n;

	n = snprintf(s, sizeof(s), "bprint: ");
	va_start(ap, fmt);
	vsnprintf(s + n, sizeof(s) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	strncat(s, "\n", sizeof(s) - strlen(s) - 1);
	tm_putstring((CONST UB *)s);
}

/* ---------------------------------------------------------------- asking the desktop */

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

/* The setting as the desktop reads it: thirteen numbers in the order of T_PRNSET */
static void set_text( const T_PRNSET *s, char *out, size_t max )
{
	snprintf(out, max, "%d %d %d %d %d %d %d %d %d %d %d %d %d",
		 s->all ? 1 : 0, s->from, s->to, s->copies, s->quality, s->colour ? 1 : 0,
		 s->paper_w, s->paper_h, s->rotate ? 1 : 0, s->scale, s->links ? 1 : 0,
		 s->expand ? 1 : 0, s->depth);
}

static BOOL text_set( const char *t, T_PRNSET *s )
{
	INT	v[13];

	if ( sscanf(t, "%d %d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
		    &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12]) != 13 ) {
		return FALSE;
	}
	s->all = (BOOL)( v[0] != 0 );
	s->from = v[1];
	s->to = v[2];
	s->copies = v[3];
	s->quality = v[4];
	s->colour = (BOOL)( v[5] != 0 );
	s->paper_w = v[6];
	s->paper_h = v[7];
	s->rotate = (BOOL)( v[8] != 0 );
	s->scale = v[9];
	s->links = (BOOL)( v[10] != 0 );
	s->expand = (BOOL)( v[11] != 0 );
	s->depth = v[12];
	return TRUE;
}

/* A step of the job asked with the setting as it stands */
static ER job_ask( INT step, T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	rq->req = DT_RQ_PRJOB;
	rq->recno = step;
	if ( step != DT_PJ_JOB ) set_text(&set, (char *)rq->text, sizeof(rq->text));
	memset(an, 0, sizeof(*an));
	return ask_desktop(rq, an, wait_ms);
}

/* How the setting lays the pages on the paper */
static void paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	if ( job_ask(DT_PJ_PAPER, &rq, &an, 30000) < E_OK ) return;
	an.text[DT_TEXT_MAX - 1] = 0;
	(void)sscanf((const char *)an.text, "%d %d %d %d %d %d", &pp_w, &pp_h, &pp_pct, &pp_sheets, &pp_lw, &pp_lh);
	npage = (INT)an.how;
}

/* thousandths of a point as millimetres */
static INT mm( INT v )
{
	return ( v * 254 + 360000 ) / 720000;
}

/* ---------------------------------------------------------------- panels */

/* A few words and 了解, in front of everything */
static void tell( const char *a, const char *b )
{
	const char	*l[2];

	l[0] = a;
	l[1] = ( b != NULL ) ? b : "";
	(void)ui_ask(&g, PN_TELL, l, 2);
}

/* A panel worked until one of its buttons or Esc answers it: the button, 0 for Esc */
static INT panel_wait( UI *q )
{
	T_OBNTM	m;
	INT	num, ev;

	for ( ;; ) {
		if ( ui_wait(&g, &m, TMO_FEVR) < E_OK ) return 0;
		ev = ui_dlg_take(q, &m, &num);
		if ( ev == UI_EV_CLOSE ) return 0;
		if ( ev == UI_EV_BUTTON ) return num;
	}
}

/* The paper of the list the setting is, or the last: その他 */
static INT paper_index( void )
{
	INT	i;

	if ( set.paper_w <= 0 ) return 0;
	for ( i = 1; i < N_PAPER - 1; i++ ) {
		if ( papers[i].w == set.paper_w && papers[i].h == set.paper_h ) return i;
	}
	return N_PAPER - 1;
}

static INT clamp( INT v, INT lo, INT hi )
{
	return ( v < lo ) ? lo : ( v > hi ) ? hi : v;
}

/* 印刷設定 */
static INT print_panel( void )
{
	UI	q;
	char	t[160];
	INT	ans, b, i, pi;

	if ( ui_dlg_open(&g, PN_PRINT, &q) < E_OK ) {
		note("no panel");
		return ANS_CANCEL;
	}
	paper_ask();
	pi = paper_index();
	if ( pi == 0 ) {
		snprintf(t, sizeof(t), "印刷用紙:  レイアウト用紙と同じ (%d×%dmm)", mm(pp_w), mm(pp_h));
	} else {
		snprintf(t, sizeof(t), "印刷用紙:  %s (%d×%dmm)", papers[pi].name, mm(pp_w), mm(pp_h));
	}
	ui_set_text(&q, PS_PAPER, t);
	ui_set_value(&q, PS_ALL, set.all ? 1 : 0);
	ui_set_value(&q, PS_PART, set.all ? 0 : 1);
	ui_set_value(&q, PS_FROM, set.from);
	ui_set_value(&q, PS_TO, ( set.to > npage ) ? npage : set.to);
	ui_set_value(&q, PS_COPIES, set.copies);
	for ( i = 0; i < 3; i++ ) ui_set_value(&q, PS_NORMAL + i, ( set.quality == i ) ? 1 : 0);
	ui_set_value(&q, PS_COLOUR, set.colour ? 1 : 0);
	snprintf(t, sizeof(t), "出力先:  PDF ファイル (デスクトップに置く)  全 %d ページ", npage);
	ui_set_text(&q, PS_OUT, t);
	if ( !can_show ) ui_show(&q, PS_SHOW, FALSE);
	note("panel 印刷設定, %d pages", npage);

	b = panel_wait(&q);
	ans = ( b == PS_OK ) ? ANS_OK : ( b == PS_DETAIL ) ? ANS_DETAIL : ( b == PS_SHOW ) ? ANS_SHOW : ANS_CANCEL;
	if ( ans != ANS_CANCEL ) {
		set.all = (BOOL)( ui_value(&q, PS_ALL) != 0 );
		set.from = clamp(ui_value(&q, PS_FROM), 1, PRN_PAGE_MAX);
		set.to = clamp(ui_value(&q, PS_TO), set.from, PRN_PAGE_MAX);
		set.copies = clamp(ui_value(&q, PS_COPIES), 1, PRN_COPY_MAX);
		set.quality = ( ui_value(&q, PS_NORMAL + 1) != 0 ) ? PRN_Q_FAST
			    : ( ui_value(&q, PS_NORMAL + 2) != 0 ) ? PRN_Q_FINE : PRN_Q_NORMAL;
		set.colour = (BOOL)( ui_value(&q, PS_COLOUR) != 0 );
	}
	ui_dlg_close(&q);
	return ans;
}

/* 印刷詳細設定 */
static void detail_panel( void )
{
	UI	q;
	char	t[160];
	INT	b, v;

	if ( ui_dlg_open(&g, PN_DETAIL, &q) < E_OK ) return;
	paper_ask();
	snprintf(t, sizeof(t), "レイアウト用紙:  %d×%dmm", mm(pp_lw), mm(pp_lh));
	ui_set_text(&q, PD_LAYOUT, t);
	ui_set_value(&q, PD_LIST, paper_index() + 1);
	ui_set_value(&q, PD_H, mm(pp_h));
	ui_set_value(&q, PD_W, mm(pp_w));
	ui_set_value(&q, PD_ROTATE, set.rotate ? 1 : 0);
	snprintf(t, sizeof(t), "(%d%%)", pp_pct / 100);
	ui_set_text(&q, PD_AUTO_PCT, t);
	ui_set_value(&q, PD_AUTO, ( set.scale == PRN_SCALE_AUTO ) ? 1 : 0);
	ui_set_value(&q, PD_GIVEN, ( set.scale != PRN_SCALE_AUTO ) ? 1 : 0);
	ui_set_value(&q, PD_PCT, ( set.scale != PRN_SCALE_AUTO ) ? set.scale : pp_pct / 100);
	ui_set_value(&q, PD_LINKS, set.links ? 1 : 0);
	ui_set_value(&q, PD_EXPAND, set.expand ? 1 : 0);
	ui_set_value(&q, PD_DEPTH, set.depth);
	note("panel 印刷詳細設定");

	b = panel_wait(&q);
	if ( b == PD_OK ) {
		v = ui_value(&q, PD_LIST);
		if ( v == 1 ) {
			set.paper_w = set.paper_h = 0;
		} else if ( v > 1 && v < N_PAPER ) {
			set.paper_w = papers[v - 1].w;
			set.paper_h = papers[v - 1].h;
		} else if ( v == N_PAPER ) {
			/* その他の用紙: the size typed in */
			INT	hh = ui_value(&q, PD_H), ww = ui_value(&q, PD_W);

			if ( hh >= 30 && hh <= 1000 && ww >= 30 && ww <= 1000 ) {
				set.paper_w = ww * 10;
				set.paper_h = hh * 10;
			}
		}
		set.rotate = (BOOL)( ui_value(&q, PD_ROTATE) != 0 );
		set.scale = ( ui_value(&q, PD_GIVEN) != 0 ) ? clamp(ui_value(&q, PD_PCT), 10, 400) : PRN_SCALE_AUTO;
		set.links = (BOOL)( ui_value(&q, PD_LINKS) != 0 );
		set.expand = (BOOL)( set.links && ui_value(&q, PD_EXPAND) != 0 );
		set.depth = clamp(ui_value(&q, PD_DEPTH), 1, PRN_DEPTH_MAX);
		note("set: paper %d x %d, scale %d", set.paper_w, set.paper_h, set.scale);
	}
	ui_dlg_close(&q);
}

/* ---------------------------------------------------------------- 画面表示 */

typedef struct {
	TS_UUID	win;
	ID	kw;
	UI	strip, end;
	INT	ww, wh;			/* the work area */
	INT	page, pages;
	BOOL	zoom;			/* 拡大 */
	INT	cx, cy;			/* the middle of what is shown when magnified, sheet pixels */
	INT	sw, sh;			/* the sheet as shown */
	INT	typed;			/* a page number being typed, 0 none */
} PV;

/* The band between the two strips of buttons, in the ground they stand on */
static void pv_band( PV *v )
{
	T_DPRECT	r;
	INT		gid = wm_obj_gid(v->kw);

	r.left = PV_STRIP_W;
	r.top = 0;
	r.right = v->ww - PV_END_W;
	r.bottom = PV_BAND;
	if ( gid < 0 || r.right <= r.left ) return;
	(void)wm_fill_look(gid, &r, LK_PNLGROUND);
	(void)wm_obj_flush(v->kw, &r);
}

/* The sheet drawn by the desktop below the buttons, and the page shown said between them */
static void pv_show( PV *v )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	t[32];

	memset(&rq, 0, sizeof(rq));
	rq.win = v->win;
	rq.grab_x = v->page;
	rq.grab_y = PV_BAND;
	rq.look.w = v->cx;
	rq.look.h = v->cy;
	rq.flags = v->zoom ? DT_F_PJ_ZOOM : 0;
	if ( job_ask(DT_PJ_SHOW, &rq, &an, 60000) >= E_OK ) {
		v->sw = an.look.w;
		v->sh = an.look.h;
	}
	snprintf(t, sizeof(t), "%d / %d", ( v->typed > 0 ) ? v->typed : v->page + 1, v->pages);
	ui_set_text(&v->strip, PV_PAGE, t);
}

static void pv_go( PV *v, INT page )
{
	if ( page < 0 ) page = 0;
	if ( page >= v->pages ) page = v->pages - 1;
	v->typed = 0;
	v->page = page;
	pv_show(v);
}

/* The window the sheets are shown in: its work area w by h, the buttons along its top */
static ER pv_open( PV *v, INT x, INT y, INT w, INT h )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[200];

	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%u,\"icon\":\"%s\"}",
		 x, y, x + w, y + h, (UINT)( WM_ATTR_FRAME | WM_ATTR_TITLE ), SYSDEF_PROG_BPRINT);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"画面表示";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &v->win) < E_OK ) return E_OBJ;
	v->kw = ob_opn_obj(&v->win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	if ( v->kw <= 0 ) {
		(void)ob_del_obj(&v->win);
		return E_OBJ;
	}
	if ( ob_rea_rec(v->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		v->ww = wp.wright - wp.wleft;
		v->wh = wp.wbottom - wp.wtop;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW;
	if ( ob_ntf_evt(v->kw, OB_REC_ANY, &req, g.port) <= 0
	  || ui_strip(&v->strip, SYSDEF_BOX_BPRINT, PN_STRIP, v->kw, g.port, 0, 0) < E_OK ) {
		ob_cls_obj(v->kw);
		(void)ob_del_obj(&v->win);
		return E_OBJ;
	}
	if ( ui_strip(&v->end, SYSDEF_BOX_BPRINT, PN_STRIP_END, v->kw, g.port, v->ww - PV_END_W, 0) < E_OK ) {
		ui_strip_close(&v->strip);
		ob_cls_obj(v->kw);
		(void)ob_del_obj(&v->win);
		return E_OBJ;
	}
	return E_OK;
}

static void pv_close( PV *v )
{
	ui_strip_close(&v->end);
	ui_strip_close(&v->strip);
	ob_cls_obj(v->kw);
	(void)ob_del_obj(&v->win);
}

/* A key: a page number typed and gone to, the next or the one before, or the end */
static BOOL pv_key( PV *v, UINT code )
{
	if ( code >= 0x1E && code <= 0x27 ) {
		/* digits: a page number, gone to by right Ctrl, F10 or Enter */
		INT	d = ( code == 0x27 ) ? 0 : (INT)code - 0x1D;

		v->typed = v->typed * 10 + d;
		if ( v->typed > PRN_PAGE_MAX ) v->typed = d;
		pv_show(v);
	} else if ( code == KEY_BS ) {
		v->typed /= 10;
		pv_show(v);
	} else if ( ( code == KEY_F10 || code == KEY_ENTER || code == KEY_RCTRL ) && v->typed > 0 ) {
		pv_go(v, v->typed - 1);
	} else if ( code == KEY_PGDN || code == KEY_RIGHT || code == KEY_DOWN ) {
		pv_go(v, v->page + 1);
	} else if ( code == KEY_PGUP || code == KEY_LEFT || code == KEY_UP ) {
		pv_go(v, v->page - 1);
	} else if ( code == KEY_ESC || code == KEY_ENTER ) {
		return TRUE;
	}
	return FALSE;
}

/* 画面表示: the sheets shown one at a time until 終了 */
static void preview( void )
{
	PV	v;
	T_OBNTM	m;
	INT	sw = 1024, shh = 768, h, wd, num, ev;
	T_DPRECT	mr = { 0, 0, 1024, 768 };
	BOOL	done = FALSE;

	paper_ask();
	if ( pp_sheets <= 0 ) {
		tell("印刷するページがありません", NULL);
		return;
	}
	memset(&v, 0, sizeof(v));
	v.pages = pp_sheets;
	wm_screen_new_rect(&mr);	/* the screen new windows open on: the pointer's, else the main one */
	sw = mr.right - mr.left;
	shh = mr.bottom - mr.top;
	/* as tall as the screen allows, as wide as the sheet's sides say */
	h = shh - 120;
	if ( h > 900 ) h = 900;
	wd = (INT)( (long long)( h - PV_BAND - 2 * PV_PAD ) * pp_w / ( pp_h > 0 ? pp_h : 1 ) ) + 2 * PV_PAD;
	if ( wd < 420 ) wd = 420;
	if ( wd > sw - 40 ) wd = sw - 40;
	if ( pv_open(&v, mr.left + ( sw - wd ) / 2, mr.top + 30, wd, h) < E_OK ) {
		note("no window to show the sheets in");
		return;
	}
	pv_band(&v);
	pv_show(&v);
	note("preview of %d sheets, %d x %d shown", v.pages, v.sw, v.sh);

	while ( !done ) {
		if ( ui_wait(&g, &m, TMO_FEVR) < E_OK ) break;
		if ( ui_mine(&v.strip, &m) && memcmp(&m.uuid, &v.win, sizeof(TS_UUID)) != 0 ) {
			ev = ui_event(&v.strip, &m, &num);
			if ( ev != UI_EV_BUTTON ) continue;
			switch ( num ) {
			case PV_PREV:	pv_go(&v, v.page - 1);	break;
			case PV_NEXT:	pv_go(&v, v.page + 1);	break;
			case PV_STD:
				v.zoom = FALSE;
				pv_show(&v);
				break;
			case PV_ZOOM:
				if ( !v.zoom ) {
					v.zoom = TRUE;
					v.cx = v.sw;		/* the middle of a sheet twice as large */
					v.cy = v.sh;
				}
				pv_show(&v);
				break;
			default:
				break;
			}
			continue;
		}
		if ( ui_mine(&v.end, &m) && memcmp(&m.uuid, &v.win, sizeof(TS_UUID)) != 0 ) {
			if ( ui_event(&v.end, &m, &num) == UI_EV_BUTTON && num == PV_END ) done = TRUE;
			continue;
		}
		if ( memcmp(&m.uuid, &v.win, sizeof(TS_UUID)) != 0 ) continue;
		switch ( m.event ) {
		case OB_E_CLOSE:
		case OB_E_DELETE:
			done = TRUE;
			break;
		case OB_E_REDRAW:
			pv_band(&v);
			pv_show(&v);
			break;
		case OB_E_KEY:
			done = pv_key(&v, m.code);
			break;
		case OB_E_PRESS:
			if ( m.recno >= 0 || m.code != 0 || m.y < PV_BAND ) break;
			if ( ( m.mods & MOD_ALT ) != 0 ) {
				/* Alt and a press: magnified about where it was pressed */
				INT	aw = v.ww - 2 * PV_PAD, x0 = PV_PAD + ( aw - v.sw ) / 2;
				INT	y0 = PV_BAND + PV_PAD + ( v.wh - PV_BAND - 2 * PV_PAD - v.sh ) / 2;

				if ( !v.zoom ) {
					v.cx = ( m.x - x0 ) * 2;
					v.cy = ( m.y - y0 ) * 2;
					v.zoom = TRUE;
				}
				pv_show(&v);
			} else if ( ( m.mods & MOD_SHIFT ) != 0 ) {
				/* a press on the sheet: the next, or with Shift the one before */
				pv_go(&v, v.page - 1);
			} else {
				pv_go(&v, v.page + 1);
			}
			break;
		default:
			break;
		}
	}
	pv_close(&v);
	note("preview put away");
}

/* ---------------------------------------------------------------- the job */

int main( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	T_OBCRE	c;
	TS_UUID	ch;
	char	*tab;
	INT	ans;
	ER	er;

	memset(&rq, 0, sizeof(rq));
	er = job_ask(DT_PJ_JOB, &rq, &an, 30000);
	if ( er == E_BUSY ) {
		note("another is at work: brought to the front");
		return 0;
	}
	if ( er < E_OK ) {
		note("no job (%d)", (INT)er);
		return 0;
	}
	an.text[DT_TEXT_MAX - 1] = 0;
	tab = strchr((char *)an.text, '\t');
	snprintf(jname, sizeof(jname), "%s", ( tab != NULL ) ? tab + 1 : "");
	if ( tab != NULL ) *tab = 0;
	if ( !text_set((const char *)an.text, &set) ) {
		/* what prn_default sets */
		memset(&set, 0, sizeof(set));
		set.all = TRUE;
		set.from = set.copies = set.depth = 1;
		set.to = PRN_PAGE_MAX;
		set.colour = set.links = TRUE;
	}
	npage = (INT)an.how;
	can_show = (BOOL)( ( an.flags & DT_F_PJ_SHOW ) != 0 );
	note("job \"%s\" of %d pages%s", jname, npage, ( an.flags & DT_F_PJ_SET ) ? ", the setting only" : "");

	/* the data box, and a channel the panels tell */
	memset(&g, 0, sizeof(g));
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ui_uuid(SYSDEF_BOX_BPRINT, &g.box) < E_OK || ob_cre_obj(&c, &ch) < E_OK
	  || ( g.port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) ) <= 0 ) {
		note("no channel");
		memset(&rq, 0, sizeof(rq));
		(void)job_ask(DT_PJ_END, &rq, &an, 5000);
		return 1;
	}
	ui_icon(SYSDEF_PROG_BPRINT);

	for ( ;; ) {
		ans = print_panel();
		if ( ans == ANS_DETAIL ) {
			detail_panel();
			continue;
		}
		if ( ans == ANS_SHOW ) {
			preview();
			continue;
		}
		break;
	}
	memset(&rq, 0, sizeof(rq));
	if ( ans == ANS_OK ) {
		paper_ask();
		if ( pp_sheets <= 0 ) {
			tell("印刷するページがありません", "ページの範囲を確かめてください");
			er = job_ask(DT_PJ_END, &rq, &an, 5000);
			note("nothing in the range: put away (%d)", (INT)er);
		} else {
			/* a large file takes its time */
			er = job_ask(DT_PJ_OUT, &rq, &an, 30 * 60 * 1000);
			note("printed (%d)", (INT)er);
		}
	} else {
		er = job_ask(DT_PJ_END, &rq, &an, 5000);
		note("put away (%d)", (INT)er);
	}
	ob_cls_obj(g.port);
	(void)ob_del_obj(&ch);
	return 0;
}
