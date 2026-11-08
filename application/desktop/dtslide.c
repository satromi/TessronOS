/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtslide.c
 *	スライド: the objects a document links to, shown one at a time over
 *	the whole screen (design 11.11)
 *
 *	The document opened is the list of the slides: each link in it, in
 *	the order it stands, is one slide, and the slide is what the link
 *	points at -- a document or a figure, drawn by the same layout as
 *	the editors draw it. When the first paragraph is "%size:W:H" the
 *	slides are made for a screen W by H; else for 1920 by 1080. A slide
 *	is drawn as large as fits, its shape kept, in the middle of a window
 *	that covers the whole screen: it fills the screen one way, and the
 *	margin is only the other way, the message line taken down
 *	while a slide window is open and put back when the last closes, on its
 *	object's own colour (its window's background colour); what is round
 *	it is that colour too, or the one chosen in the menu. After the last
 *	slide a black screen says how to end, and the next step closes the
 *	window. A document with no links that holds a <realtime> is a deck
 *	of one slide, itself.
 *
 *	A slide that holds a <realtime> is played (dtrt.c, design 15): its
 *	fades and movements run, its sounds and videos play, and what the
 *	person does goes to the program first -- a click or a key its
 *	triggers or waits take does not turn the page. What its media ask
 *	for when they end (onended "slide:next", "slide:prev",
 *	"slide:index:N") turns the page, from the desktop's loop.
 *
 *	The window has no frame and is not a window of an object; it is one
 *	of the desktop's windows whose work area this part draws and works
 *	(DTOWN), so the second button opens its menu -- 閉じる, the slides'
 *	items and the tail of ウインドウ, 小物 and 実行, 実行 for the link
 *	under the pointer -- and a letter with Ctrl chooses the item that has
 *	it, as in every window. Nothing in the slides is changed.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/mn.h>
#include <ts/rtad.h>
#include <ts/sysdef.h>
#include <ts/time.h>
#include <ts/slide.h>
#include "desktop.h"

#define SL_WIN_MAX	4		/* slide shows open at once */
#define SL_PAD		16		/* round a document on the slide, at life size */

#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_SPACE	0x2C
#define KEY_HOME	0x4A
#define KEY_PGUP	0x4B
#define KEY_END		0x4D
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52
#define MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )

#define C_END		0x00000000U
#define C_END_TEXT	0x00FFFFFFU
#define C_FAIL		0x00FF0000U

typedef struct {
	DTWIN	*d;			/* the window it is shown in */
	INT	wid;
	TS_UUID	id;			/* the document of the slides */
	TS_UUID	*page;			/* what its links point at, in order */
	INT	n;
	INT	cur;
	BOOL	end;			/* the screen after the last */
	INT	tw, th;			/* the screen the slides are made for */
	UW	back;			/* round the slides: chosen in the menu, or TAD_COL_NONE */
	UB	name[TAD_NAME_MAX];
	DTRT	*rt;			/* the program of the slide shown, when it has one */
	INT	rt_page;		/* which slide that is */
	BOOL	down;			/* the first button went down on the slide */
	INT	down_x, down_y;
} SLIDE;

LOCAL CONST DTOWN	sl_own;
LOCAL BOOL	sl_took_msg;		/* the message line was up and the slides took it down */
#ifdef USE_UITEST
LOCAL UD	sl_input_at;		/* when the key or press being answered happened, 0 none */
#endif

/* ---------------------------------------------------------------- what the document says */

/* The rules of the slides are the program's of the slides (application/slide/sl_deck.c) */
EXPORT INT dt_slide_pages( CONST T_TAD *rec, TS_UUID *ids, INT max, INT *p_w, INT *p_h )
{
	return sl_pages(rec, ids, max, p_w, p_h);
}

EXPORT BOOL dt_slide_takes( CONST T_TAD *rec )
{
	return sl_takes(rec);
}

EXPORT INT dt_slide_fit( INT ww, INT wh, INT tw, INT th, T_DPRECT *out, INT *p_den )
{
	return sl_fit(ww, wh, tw, th, out, p_den);
}

/* ---------------------------------------------------------------- drawing */

/* Letters in the system's face, px tall, put back as they were for the rest of the desktop */
LOCAL INT text( INT gid, INT x, INT y, CONST char *s, UW col, INT px, BOOL measure )
{
	T_FNSTATE	st;
	ID		fid = fn_system();
	INT		r;

	if ( fid <= 0 ) return 0;
	fn_hold();
	(void)fn_state_get(fid, &st);
	(void)fn_set_size(fid, px);
	r = measure ? fn_width(fid, (CONST UB *)s) : fn_draw(gid, fid, x, y, (CONST UB *)s, col);
	(void)fn_state_set(fid, &st);
	fn_release();
	return r;
}

/* A line of words in the middle of the window */
LOCAL void centre( INT gid, INT ww, INT wh, CONST char *s, UW col, INT px )
{
	INT	w = text(gid, 0, 0, s, col, px, TRUE);

	(void)text(gid, ( ww - w ) / 2, wh / 2 + px / 3, s, col, px, FALSE);
}

/* A slide's model: its document or its figure */
LOCAL BOOL page_model( T_TAD *rec, T_TVDOC **p_d, T_TVFIG **p_f )
{
	return sl_model(rec, p_d, p_f);
}

/* One slide drawn into area, magnified z / den; FALSE when there was nothing to draw */
LOCAL BOOL draw_page( INT gid, CONST TS_UUID *page, CONST T_DPRECT *area, INT z, INT den, UW paper )
{
	T_TAD	*rec = om_store_get(page, 0);
	T_TVDOC	*d;
	T_TVFIG	*f;

	if ( !page_model(rec, &d, &f) ) return FALSE;
	om_store_self(page);
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
	om_store_self(NULL);
	return TRUE;
}

/* The link a slide shows at a place of its window, when there is one */
LOCAL BOOL link_at( SLIDE *s, INT x, INT y, T_VOBJ *v )
{
	T_WMWIN		w;
	T_DPRECT	area, r;
	T_TAD		*rec;
	T_TVDOC		*d;
	T_TVFIG		*f;
	INT		z, den;
	ER		er = E_NOEXS;

	if ( s->end || wm_ref(s->wid, &w) < E_OK ) return FALSE;
	rec = om_store_get(&s->page[s->cur], 0);
	if ( !page_model(rec, &d, &f) ) return FALSE;
	z = dt_slide_fit(w.work.right - w.work.left, w.work.bottom - w.work.top, s->tw, s->th, &area, &den);
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

/* The slide's own colour: its window's background colour, or white */
LOCAL UW page_paper( SLIDE *s )
{
	UW	paper = 0x00FFFFFFU;

	if ( om_store_paper(&s->page[s->cur], &paper) < E_OK || paper == TV_PAPER_LOOK ) {
		paper = 0x00FFFFFFU;
	}
	return paper;
}

LOCAL void rt_end( SLIDE *s )
{
	if ( s->rt != NULL ) {
		dtrt_close(s->rt);
		s->rt = NULL;
	}
}

LOCAL void sl_draw( SLIDE *s )
{
	T_WMWIN		w;
	T_DPRECT	all, area;
	INT		gid = wm_gid(s->wid), ww, wh, z, den;
	UW		paper, ground;
	UB		name[TAD_NAME_MAX];
	char		msg[TAD_NAME_MAX + 64];
	T_TAD		*rec;
#ifdef USE_UITEST
	UD		t0 = 0;
#endif

	if ( gid < 0 || wm_ref(s->wid, &w) < E_OK ) return;
#ifdef USE_UITEST
	(void)ts_get_mono(&t0);
#endif
	ww = w.work.right - w.work.left;
	wh = w.work.bottom - w.work.top;
	all.left = all.top = 0;
	all.right = ww;
	all.bottom = wh;
	if ( s->end ) {
		rt_end(s);
		dp_fill_rect(gid, &all, C_END);
		centre(gid, ww, wh, "マウスクリックでスライドを終了します", C_END_TEXT, 24);
		(void)wm_msg_put("スライド終了");
		wm_update();
		return;
	}
	paper = page_paper(s);
	/* round the slide: what the menu chose, else the slide's own colour */
	ground = ( s->back != TAD_COL_NONE ) ? s->back : paper;
	dp_fill_rect(gid, &all, ground);
	z = dt_slide_fit(ww, wh, s->tw, s->th, &area, &den);
	if ( om_store_name(&s->page[s->cur], name, sizeof(name)) < 0 ) name[0] = 0;
	rec = om_store_get(&s->page[s->cur], 0);
	if ( s->rt != NULL && s->rt_page != s->cur ) rt_end(s);
	if ( dtrt_has(rec) ) {
		if ( s->rt == NULL ) {
			INT	tw = s->tw, th = s->th;

			s->rt = dtrt_open(s->wid, &s->page[s->cur], 0, &all, tw, th, paper, s->cur + 1);
			s->rt_page = s->cur;
		} else {
			dtrt_redraw(s->rt);
		}
		if ( s->rt == NULL ) {
			dp_fill_rect(gid, &area, paper);
			(void)draw_page(gid, &s->page[s->cur], &area, z, den, paper);
		}
	} else {
		rt_end(s);
		dp_fill_rect(gid, &area, paper);
		if ( !draw_page(gid, &s->page[s->cur], &area, z, den, paper) ) {
			dt_pn_text((UB *)msg, sizeof(msg), "スライド「");
			knl_strcat(msg, (CONST char *)name);
			knl_strcat(msg, "」の読み込みに失敗しました");
			centre(gid, ww, wh, msg, C_FAIL, 20);
		}
	}
	{
		/* "2 / 5: name" kept as the message, the slide's place, for when the line is up */
		UB	num[24];
		INT	a = s->cur + 1, b = s->n, k = 0, i;
		UB	t[12];

		for ( i = 0; a > 0 || i == 0; a /= 10 ) t[i++] = (UB)( '0' + a % 10 );
		while ( i > 0 ) num[k++] = t[--i];
		num[k++] = ' ';
		num[k++] = '/';
		num[k++] = ' ';
		for ( i = 0; b > 0 || i == 0; b /= 10 ) t[i++] = (UB)( '0' + b % 10 );
		while ( i > 0 ) num[k++] = t[--i];
		num[k] = 0;
		dt_pn_text((UB *)msg, sizeof(msg), (CONST char *)num);
		knl_strcat(msg, ": ");
		knl_strcat(msg, (CONST char *)name);
		(void)wm_msg_put(msg);
	}
	tm_printf((UB *)"slide: %d of %d zoom %d/%d at %d,%d %dx%d%s\n", s->cur + 1, s->n, z, den,
		  area.left, area.top, area.right - area.left, area.bottom - area.top,
		  ( s->rt != NULL ) ? " playing" : "");
	wm_update();
#ifdef USE_UITEST
	/* how long the page took to be drawn and shown, for comparing with the slide of a process */
	{
		UD	t1 = 0;

		(void)ts_get_mono(&t1);
		tm_printf((UB *)"slide: drew %d in %d us\n", s->cur + 1, (INT)( ( t1 - t0 ) / 1000 ));
		if ( sl_input_at != 0 ) {
			/* from the key or the press, as the keyboard or the pointer told it, to the page shown */
			tm_printf((UB *)"slide: input to screen %d us\n", (INT)( ( t1 - sl_input_at ) / 1000 ));
			sl_input_at = 0;
		}
	}
#endif
}

/* ---------------------------------------------------------------- the windows */

LOCAL void sl_own_draw( DTWIN *d )
{
	if ( d->self != NULL ) sl_draw((SLIDE *)d->self);
}

/* What the window keeps let go of; the message line back when the last slide window goes */
LOCAL void sl_own_close( DTWIN *d )
{
	SLIDE	*s = (SLIDE *)d->self;

	if ( s == NULL ) return;
	rt_end(s);
	if ( sl_took_msg && dt_own_next(&sl_own, NULL) == NULL ) {
		sl_took_msg = FALSE;
		(void)wm_msg_show(TRUE);
	}
	(void)wm_msg_put(NULL);
	if ( s->page != NULL ) Kfree(s->page);
	tm_printf((UB *)"slide: closed\n");
	Kfree(s);
}

/* Closed: nothing of s is touched after */
LOCAL void sl_close( SLIDE *s )
{
	dt_close(s->d);
	wm_update();
}

#define SL_PAGE_MAX	256

EXPORT void dt_slide_open( DTWIN *from, CONST T_VOBJ *v )
{
	SLIDE		*s;
	DTWIN		*d;
	T_TAD		*rec;
	T_DPRECT	o;
	UINT		sw = 1024, sh = 768;
	INT		n = 0;

	(void)from;
	for ( d = dt_own_next(&sl_own, NULL); d != NULL; d = dt_own_next(&sl_own, d) ) {
		s = (SLIDE *)d->self;
		if ( s != NULL && ts_uuid_cmp(&s->id, &v->target) == 0 ) {
			wm_raise(d->wid);
			wm_focus(d->wid);
			wm_update();
			return;
		}
		n++;
	}
	rec = om_store_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);
	if ( n >= SL_WIN_MAX || rec == NULL ) {
		(void)wm_msg_put("スライドを開けません");
		return;
	}
	s = (SLIDE *)Kmalloc(sizeof(SLIDE));
	if ( s == NULL ) return;
	knl_memset(s, 0, sizeof(*s));
	s->page = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * SL_PAGE_MAX);
	if ( s->page == NULL ) {
		Kfree(s);
		return;
	}
	s->id = v->target;
	s->back = TAD_COL_NONE;
	s->n = dt_slide_pages(rec, s->page, SL_PAGE_MAX, &s->tw, &s->th);
	if ( s->n <= 0 && dtrt_has(rec) ) {
		/* a program of its own and no links: the one slide is itself, at the size it is made for */
		s->page[0] = v->target;
		s->n = 1;
		dtrt_scene_size(rec, &s->tw, &s->th);
	}
	if ( om_store_name(&v->target, s->name, sizeof(s->name)) < 0 ) s->name[0] = 0;
	tm_printf((UB *)"slide: %d pages, made for %d x %d\n", s->n, s->tw, s->th);
	if ( s->n <= 0 ) {
		(void)wm_msg_put("スライドがありません。仮身を追加してください");
		Kfree(s->page);
		Kfree(s);
		return;
	}

	/* the whole screen, the message line too, with no frame */
	if ( wm_msg_height() > 0 ) {
		(void)wm_msg_show(FALSE);
		sl_took_msg = TRUE;
	}
	(void)dt_screen_size(&sw, &sh);
	o.left = 0;
	o.top = 0;
	o.right = (INT)sw;
	o.bottom = (INT)sh;
	d = dt_own_open(&sl_own, s, &o, 0, (CONST char *)s->name, NULL);
	if ( d == NULL ) {
		Kfree(s->page);
		Kfree(s);
		if ( sl_took_msg && dt_own_next(&sl_own, NULL) == NULL ) {
			sl_took_msg = FALSE;
			(void)wm_msg_show(TRUE);
		}
		return;
	}
	s->d = d;
	s->wid = d->wid;
	sl_draw(s);
}

LOCAL void sl_go( SLIDE *s, INT to )
{
	if ( to >= s->n ) {
		s->end = TRUE;
	} else {
		s->end = FALSE;
		s->cur = ( to < 0 ) ? 0 : to;
	}
	sl_draw(s);
}

/* The next step: the next slide, or after the last the end, and after the end closed */
LOCAL BOOL sl_next( SLIDE *s )
{
	if ( s->end ) {
		sl_close(s);
		return FALSE;
	}
	sl_go(s, s->cur + 1);
	return TRUE;
}

LOCAL void sl_prev( SLIDE *s )
{
	if ( s->end ) {
		sl_go(s, s->n - 1);
	} else if ( s->cur > 0 ) {
		sl_go(s, s->cur - 1);
	}
}

/* ジャンプ: the slides by their names, one chosen */
LOCAL void sl_jump( SLIDE *s )
{
	UB		(*names)[WM_LABEL_MAX];
	CONST char	**list;
	INT		i, k, at;

	names = (UB (*)[WM_LABEL_MAX])Kmalloc(sizeof(UB) * WM_LABEL_MAX * (SZ)s->n);
	list = (CONST char **)Kmalloc(sizeof(char *) * (SZ)s->n);
	if ( names != NULL && list != NULL ) {
		for ( i = 0; i < s->n; i++ ) {
			UB	nm[TAD_NAME_MAX], t[12], line[TAD_NAME_MAX + 16];
			INT	a = i + 1, j;

			/* "3. name", cut to the list's room at a letter's edge */
			k = 0;
			for ( j = 0; a > 0; a /= 10 ) t[j++] = (UB)( '0' + a % 10 );
			while ( j > 0 ) line[k++] = t[--j];
			line[k++] = '.';
			line[k++] = ' ';
			if ( om_store_name(&s->page[i], nm, sizeof(nm)) < 0 ) nm[0] = 0;
			for ( j = 0; nm[j] != 0; j++ ) line[k++] = nm[j];
			line[k] = 0;
			dt_pn_text(names[i], WM_LABEL_MAX, (CONST char *)line);
			list[i] = (CONST char *)names[i];
		}
		at = dt_list_form(s->d, "ジャンプ", list, s->n, s->end ? s->n - 1 : s->cur);
		if ( at >= 0 ) {
			sl_go(s, at);
		} else {
			sl_draw(s);
		}
	}
	if ( names != NULL ) Kfree(names);
	if ( list != NULL ) Kfree(list);
}

/* 背景色変更: what is round the slides, for as long as the window is open */
LOCAL void sl_ground( SLIDE *s )
{
	static CONST char *CONST names[4] = { "白", "黒", "グレー", "青" };
	static CONST UW		cols[4] = { 0x00FFFFFFU, 0x00000000U, 0x00808080U, 0x00000080U };
	INT			at;

	at = dt_list_form(s->d, "背景色変更", names, 4, -1);
	if ( at >= 0 && at < 4 ) s->back = cols[at];
	sl_draw(s);
}

/*
 * 印刷 of the slides: each slide a page, as large as the screen the
 * slides are made for (in pixels of 96 to the inch), drawn as it is
 * shown at life size on its own colour.
 */
typedef struct {
	SLIDE	*s;
	UW	paper;
} SLPRN;

LOCAL ER sl_print_page( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	SLPRN	*p = (SLPRN *)arg;

	dp_fill_rect(gid, sheet, p->paper);
	(void)draw_page(gid, &p->s->page[page], sheet, 8, 8, p->paper);
	return E_OK;
}

LOCAL void sl_print( SLIDE *s )
{
	T_PRNJOB	job;
	SLPRN		p;

	if ( s->n <= 0 ) {
		return;
	}
	knl_memset(&job, 0, sizeof(job));
	p.s = s;
	p.paper = page_paper(s);
	job.npage = s->n;
	job.lw = ( s->tw > 0 ) ? s->tw : 1024;
	job.lh = ( s->th > 0 ) ? s->th : 768;
	job.unit = 750;
	job.name = s->name;
	job.draw = sl_print_page;
	job.arg = &p;
	rt_end(s);
	(void)dt_print(dt_first_win(), &job);
	sl_draw(s);
}

/* The link of the slide at a place of the window, for 実行 of the menu */
LOCAL BOOL sl_link_at( DTWIN *d, INT x, INT y, T_VOBJ *v )
{
	return ( d->self != NULL ) ? link_at((SLIDE *)d->self, x, y, v) : FALSE;
}

LOCAL void sl_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	SLIDE	*s = (SLIDE *)d->self;

	if ( mn_is(sel, "slide.next") ) (void)sl_next(s);
	else if ( mn_is(sel, "slide.prev") ) sl_prev(s);
	else if ( mn_is(sel, "slide.jump") ) sl_jump(s);
	else if ( mn_is(sel, "slide.refresh") ) {
		rt_end(s);		/* shown again from its start */
		sl_draw(s);
	} else if ( mn_is(sel, "slide.bg") ) sl_ground(s);
	else if ( mn_is(sel, "slide.print") ) sl_print(s);
}

/* A key of the slides' own; one with Ctrl is a letter of the menu */
LOCAL BOOL sl_key( SLIDE *s, CONST T_WMEV *ev )
{
	switch ( ev->code ) {
	case KEY_RIGHT:
	case KEY_DOWN:
	case KEY_PGDN:
	case KEY_SPACE:
	case KEY_ENTER:
		(void)sl_next(s);
		break;
	case KEY_LEFT:
	case KEY_UP:
	case KEY_PGUP:
	case KEY_BS:
		sl_prev(s);
		break;
	case KEY_HOME:
		sl_go(s, 0);
		break;
	case KEY_END:
		sl_go(s, s->n - 1);
		break;
	case KEY_ESC:
		sl_close(s);
		break;
	default:
		break;
	}
	return TRUE;
}

LOCAL BOOL sl_input( DTWIN *d, CONST T_WMEV *ev )
{
	SLIDE	*s = (SLIDE *)d->self;
	BOOL	ctrl = (BOOL)( ( ev->mods & MOD_CTRL ) != 0 );

	if ( s == NULL ) return FALSE;
#ifdef USE_UITEST
	sl_input_at = ( ev->type == HID_EV_KEY_DOWN || ( ev->type == HID_EV_BTN_DOWN && ev->code == 0 ) )
		      ? ev->when : 0;
#endif
	switch ( ev->type ) {
	case HID_EV_BTN_DOWN:
		if ( ev->code == 0 ) {
			T_VOBJ	*v;

			s->down = TRUE;
			s->down_x = ev->x;
			s->down_y = ev->y;
			if ( s->rt != NULL && !s->end ) {
				/* a program waiting to be begun is begun; a click it waits for is its */
				if ( dtrt_waiting(s->rt) ) {
					dtrt_toggle(s->rt);
					return TRUE;
				}
				if ( dtrt_input(s->rt, ev, TRUE) ) {
					(void)dtrt_input(s->rt, ev, FALSE);
					return TRUE;
				}
				(void)dtrt_input(s->rt, ev, FALSE);
			}
			/* a press on a link does not turn the page */
			v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
			if ( v != NULL ) {
				BOOL	on = link_at(s, ev->x, ev->y, v);

				Kfree(v);
				if ( on ) return TRUE;
			}
			(void)sl_next(s);
		}
		return TRUE;
	case HID_EV_BTN_UP:
		if ( ev->code == 0 && s->down ) {
			INT	dx = ev->x - s->down_x, dy = ev->y - s->down_y;

			s->down = FALSE;
			if ( s->rt != NULL && dtrt_input(s->rt, ev, FALSE) ) return TRUE;
			/* a swipe across the slide, when its program does not take it */
			if ( ( dx > 80 || dx < -80 ) && ( dy < 60 && dy > -60 ) ) {
				if ( dx < 0 ) (void)sl_next(s);
				else sl_prev(s);
			}
		}
		return TRUE;
	case HID_EV_KEY_DOWN:
		if ( ctrl ) return FALSE;
		if ( s->rt != NULL && !s->end ) {
			if ( dtrt_input(s->rt, ev, TRUE) ) {
				(void)dtrt_input(s->rt, ev, FALSE);
				return TRUE;
			}
			if ( dtrt_waiting(s->rt) && ( ev->code == KEY_SPACE || ev->code == KEY_ENTER ) ) {
				dtrt_toggle(s->rt);
				return TRUE;
			}
			(void)dtrt_input(s->rt, ev, FALSE);
		}
		return sl_key(s, ev);
	case HID_EV_KEY_UP:
		if ( s->rt != NULL ) (void)dtrt_input(s->rt, ev, FALSE);
		return TRUE;
	case HID_EV_WHEEL:
		if ( ev->code != HID_WHEEL_V ) return TRUE;	/* the slides go one way: across is let go */
		if ( ev->dz < 0 ) (void)sl_next(s);
		else if ( ev->dz > 0 ) sl_prev(s);
		return TRUE;
	case HID_EV_MOVE:
		if ( s->rt != NULL ) (void)dtrt_input(s->rt, ev, FALSE);
		return TRUE;
	default:
		return TRUE;
	}
}

/* What the slide's program asked for: the page turned, from the desktop's task */
LOCAL void sl_poll( DTWIN *d )
{
	SLIDE	*s = (SLIDE *)d->self;
	UINT	a;
	INT	n = 0;

	if ( s == NULL || s->rt == NULL ) return;
	a = dtrt_action(s->rt, &n);
	if ( a == RT_ACT_NEXT ) (void)sl_next(s);
	else if ( a == RT_ACT_PREV ) sl_prev(s);
	else if ( a == RT_ACT_INDEX ) sl_go(s, n);
}

/*
 * The slide being played, or the document of the slides, can no longer
 * be found: the window goes before the store lets go of the record the
 * player holds.
 */
LOCAL BOOL sl_lost( DTWIN *d )
{
	SLIDE	*s = (SLIDE *)d->self;

	return (BOOL)( s != NULL && ( om_store_lost(&s->id)
			|| ( s->rt != NULL && om_store_lost(&s->page[s->rt_page]) ) ) );
}

LOCAL CONST DTOWN	sl_own = {
	SYSDEF_MENU_SLIDE,
	sl_own_draw,
	sl_input,
	sl_link_at,
	NULL,
	sl_menu_do,
	NULL,
	sl_own_close,
	sl_poll,
	sl_lost
};
