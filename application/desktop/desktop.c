/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	desktop.c
 *	What comes up when the machine starts (design 16.4, 17.9)
 *
 *	The ground, one window on it holding the opening cabinet, and a
 *	loop that reads what the person does. Everything it draws with is
 *	the same code the tests draw with: this program adds no drawing of
 *	its own, it decides what is shown and when.
 *
 *	What it does:
 *
 *	  the pointer moved	the pointer is laid again where it went;
 *				whatever is held follows it
 *	  the second button	the menu of the program under it comes up
 *	  the first button	on a bar, the view is wound; on the band,
 *				the window is carried; on its foot corner,
 *				sized; on the page, what dtedit.c does
 *	  Ctrl+S, Ctrl+E	saves the window in front; closes it, saving
 *	  Ctrl+O		opens what is taken
 *	  Ctrl+A, C, X, V, Z	takes everything, copies, cuts, pastes,
 *				moves from the clipboard
 *	  Ctrl+L, F, R, D	全画面表示, いちばん前へ, いちばん後ろへ, 整頓
 *	  Delete		throws away what is taken
 *	  the second button	the menu, where 取消 takes a change back
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/fs.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/docmenu.h>
#include <ts/disp.h>
#include <ts/conf.h>
#include <ts/cabmenu.h>
#include <ts/kconv.h>
#include <ts/ob.h>
#include <ts/dtreq.h>
#include <ts/sysdef.h>
#include <ts/so.h>
#include "obj/obj.h"
#include "desktop.h"

/* The keys, as the keyboard numbers them */
#define KEY_A		0x04
#define KEY_C		0x06
#define KEY_D		0x07
#define KEY_E		0x08
#define KEY_F		0x09
#define KEY_L		0x0F
#define KEY_O		0x12
#define KEY_R		0x15
#define KEY_S		0x16
#define KEY_V		0x19
#define KEY_X		0x1B
#define KEY_Z		0x1D
#define KEY_1		0x1E
#define KEY_0		0x27
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_DEL		0x4C

#define MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

LOCAL DTWIN	dt_win[DT_MAX_WIN];
LOCAL UW	dt_layout = 0;		/* wm_layout_serial as the windows were last drawn at */
LOCAL BOOL	dt_stop = FALSE;
LOCAL INT	dt_first_wid = 0;	/* the window the desktop opened with (the cabinet) */
LOCAL BOOL	dt_poweroff = FALSE;	/* the person chose to turn the machine off */
LOCAL BOOL	dt_restart = FALSE;	/* or to start it again */

/*
 * The system object's power record written (obsys.c, システム環境設定):
 * the session ends as closing the first window ends it, the machine then
 * going off (how 0) or starting again (how 1).
 */
LOCAL ER dt_power( INT how )
{
	dt_poweroff = TRUE;
	dt_restart = (BOOL)( how == 1 );
	dt_stop = TRUE;
	return E_OK;
}
LOCAL ID	dt_port = 0;	/* where requests to close the windows arrive */
LOCAL ID	dt_req = 0;	/* where processes ask for things (DT_REQ_NAME) */

/*
 * The knob that is being held, if one is. A bar is wound by holding its
 * knob and moving, so the press has to be remembered until the button
 * is let go -- a press that only looked at where it landed would move
 * the view once and then stop following.
 */
LOCAL INT	dt_hold_wid = 0;
LOCAL UINT	dt_hold_bar = 0;
LOCAL INT	dt_hold_at = 0;	/* a program's bar: the start last told */

/*
 * The window being carried, and where it was taken hold of. A window is
 * moved by its band and sized by its bottom right corner; which of the
 * two is decided when the button goes down and does not change while it
 * is held, because a window that started to move should not start
 * changing size half way.
 */
LOCAL INT	dt_move_wid = 0;
LOCAL BOOL	dt_sizing = FALSE;
LOCAL INT	dt_grab_dx = 0, dt_grab_dy = 0;

LOCAL UINT	dt_up_mods = 0;	/* the keys held when the button was let go */
LOCAL ID	dt_tskid = 0;	/* the desktop's task, while it runs */

/* ---------------------------------------------------------------- the face */

/*
 * A face to draw with: one of the faces in the 書体箱, the objects the
 * faces on the disk were taken in as (dt_res_faces). The face is never
 * opened by its file's path.
 */
LOCAL ER face_open( void )
{
	if ( fn_system() > 0 ) {
		return E_OK;
	}
	return ( dt_res_faces() > 0 && fn_system() > 0 ) ? E_OK : E_NOEXS;
}

/* ---------------------------------------------------------------- windows */

EXPORT DTWIN *dt_win_of( INT wid )
{
	INT	i;

	if ( wid <= 0 ) {
		return NULL;
	}
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].wid == wid ) {
			return &dt_win[i];
		}
	}

	return NULL;
}

/* What the work area of a window comes to, in its own coordinates */
EXPORT void dt_work_rect( CONST DTWIN *d, T_DPRECT *r )
{
	T_WMWIN	w;

	/*
	 * A piece of text edited in a figure: its page is the box it
	 * stands in, as the figure is shown, magnified as the figure is.
	 * The text's layout is told the magnification here, since every
	 * reckoning of where its letters are starts by asking this.
	 */
	if ( d->tb_host != NULL ) {
		CONST DTWIN	*h = d->tb_host;
		INT		i, z = ( h->fig != NULL && h->fig->zoom > 0 ) ? h->fig->zoom : 8;

		dt_work_rect(h, r);
		for ( i = 0; h->fig != NULL && i < h->fig->nsh; i++ ) {
			CONST T_TVSHAPE	*s = &h->fig->sh[i];

			if ( s->kind == TV_SH_DOC && s->node == d->tb_node ) {
				INT	ox = r->left - h->scroll_x, oy = r->top - h->scroll_y;

				r->left = ox + s->r.left * z / 8;
				r->top = oy + s->r.top * z / 8;
				r->right = ox + s->r.right * z / 8;
				r->bottom = oy + s->r.bottom * z / 8;
				break;
			}
		}
		tv_doc_zoom(z);
		return;
	}
	tv_doc_zoom(8);
	r->left = DT_MARGIN;  r->top = DT_MARGIN;
	r->right = 600;  r->bottom = 400;
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		r->right  = w.work.right - w.work.left - DT_MARGIN;
		r->bottom = w.work.bottom - w.work.top - DT_MARGIN;
	}
	/*
	 * A document not wrapped to the window is wrapped to its paper:
	 * the paper's whole width when the paper is shown, its margins and
	 * all, and the width inside the margins when it is not.
	 */
	if ( d->doc != NULL && !d->xml_view && ( d->paper_frame || d->nowrap ) ) {
		INT	pw = ( d->doc->paper_w > 0 ) ? d->doc->paper_w : DT_PAPER_W;

		if ( !d->paper_frame ) {
			pw -= d->doc->margin[0] + d->doc->margin[2];
		}
		if ( pw < 64 ) {
			pw = 64;
		}
		r->right = r->left + pw;
	}
}

/*
 * 用紙枠: the edge of each page of the paper, and the desk beside it.
 * The pages are as the layout made them, one under another, each as
 * tall as the paper.
 */
LOCAL void paper_frame( DTWIN *d, INT gid, CONST T_DPRECT *r )
{
	T_WMWIN		w;
	T_DPRECT	pg, desk;
	INT		ph = d->doc->page_h, k, pages;

	if ( ph <= 0 || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	desk.left = r->right + 1;
	desk.top = 0;
	desk.right = w.work.right - w.work.left;
	desk.bottom = w.work.bottom - w.work.top;
	if ( desk.right > desk.left ) {
		dp_fill_rect(gid, &desk, 0x00A0A0A0U);
	}
	pages = ( d->height + ph - 1 ) / ph;
	if ( pages < 1 ) {
		pages = 1;
	}
	for ( k = 0; k < pages; k++ ) {
		pg.left = r->left;
		pg.right = r->right + 1;
		pg.top = r->top + k * ph - d->scroll_y;
		pg.bottom = pg.top + ph;
		if ( pg.bottom < 0 || pg.top > desk.bottom ) {
			continue;
		}
		dp_frame_rect(gid, &pg, 0x00606060U, 1);
	}
}

/*
 * Draw what is in a window, and set its bar to say how much of it is
 * being shown. The height comes back from the drawing itself, so the
 * bar and the page can never disagree about how long the page is.
 * What is taken is drawn last, over the page.
 */
EXPORT void dt_draw( DTWIN *d )
{
	T_DPRECT	r;
	INT		gid = wm_gid(d->wid);
	T_WMBAR		b;

	if ( d->tb_host != NULL ) {
		dt_draw(d->tb_host);		/* the figure, and the text's caret in it */
		return;
	}
	if ( gid < 0 || d->rec == NULL ) {
		return;
	}
	/*
	 * The margin round the page is the page's colour too, and is laid
	 * each time: what is taken is framed just outside its box, and a
	 * frame on an object at the page's edge reaches into the margin.
	 */
	{
		T_WMWIN		w;
		T_DPRECT	all;

		if ( wm_ref(d->wid, &w) >= E_OK ) {
			all.left = 0;  all.top = 0;
			all.right  = w.work.right - w.work.left;
			all.bottom = w.work.bottom - w.work.top;
			dp_fill_rect(gid, &all, ( d->paper == TV_PAPER_LOOK )
					? wm_look(WM_LOOK_WORK) : d->paper);
		}
	}
	dt_work_rect(d, &r);
	if ( d->doc != NULL ) {
		d->height = tv_doc_draw(gid, d->doc, &r, d->scroll_y, d->rec,
					d->paper);
		if ( d->paper_frame && !d->xml_view ) {
			paper_frame(d, gid, &r);
		}
	} else if ( d->fig != NULL ) {
		tv_fig_draw(gid, d->fig, &r, d->scroll_x, d->scroll_y, d->rec,
			    d->paper);
		tv_fig_size(d->fig, &d->width, &d->height);
		if ( d->fig->zoom > 0 ) {
			d->height = d->height * d->fig->zoom / 8;
			d->width = d->width * d->fig->zoom / 8;
		}
		df_overlay(d, gid, &r);
	}
	ed_draw_picks(d);
	dd_draw(d, gid);
	if ( d->tb != NULL ) {
		/* the box of the piece of text being edited, and its caret */
		T_DPRECT	tr;
		T_DPPAT		tp;

		dt_work_rect(d->tb, &tr);
		dp_pat_colour(&tp, 0x000078D7U);
		dp_line_wide(gid, tr.left - 1, tr.top - 1, tr.right, tr.top - 1, 1, DP_LINE_DASH, &tp);
		dp_line_wide(gid, tr.left - 1, tr.bottom, tr.right, tr.bottom, 1, DP_LINE_DASH, &tp);
		dp_line_wide(gid, tr.left - 1, tr.top - 1, tr.left - 1, tr.bottom, 1, DP_LINE_DASH, &tp);
		dp_line_wide(gid, tr.right, tr.top - 1, tr.right, tr.bottom, 1, DP_LINE_DASH, &tp);
		dd_draw(d->tb, gid);
		dt_work_rect(d, &r);		/* the figure's own page, again */
	}
	b.lo  = 0;
	b.hi  = ( d->height > 0 ) ? d->height : 1;
	b.clo = d->scroll_y;
	b.chi = d->scroll_y + ( r.bottom - r.top );
	if ( b.chi > b.hi ) {
		b.chi = b.hi;
	}
	wm_set_bar(d->wid, WM_BAR_R, &b);

	/* across: a figure wider than the page, or one scrolled over */
	b.hi  = ( d->doc == NULL && d->width > 0 ) ? d->width : ( r.right - r.left );
	if ( b.hi < d->scroll_x + ( r.right - r.left ) ) {
		b.hi = d->scroll_x + ( r.right - r.left );
	}
	if ( b.hi <= 0 ) {
		b.hi = 1;
	}
	b.clo = d->scroll_x;
	b.chi = d->scroll_x + ( r.right - r.left );
	if ( b.chi > b.hi ) {
		b.chi = b.hi;
	}
	wm_set_bar(d->wid, WM_BAR_B, &b);
}

/*
 * 選択枠のちらつき: the frames of what is taken in a figure editor shown
 * and hidden by turns, a turn as long as ユーザ環境設定 says (LK_MARCH);
 * and the caret of the text typed into, shown and hidden by turns of
 * LK_BLINK (0: it does not blink). The loop wakes at least this often.
 */
#define DT_BLINK_MS	200

LOCAL UW	blink_at;		/* when the turn last changed, in ms */
LOCAL UW	caret_at;		/* and the caret's */

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);

	return (UW)t.lo;
}

LOCAL void blink_restart( void )
{
	blink_at = now_ms();
	caret_at = blink_at;
	if ( dd_caret_hidden ) {
		DTWIN	*f = dt_win_of(wm_focused());

		dd_caret_hidden = FALSE;
		if ( f != NULL && dd_editable(f) ) dt_draw(f);
	}
}

/* The caret of the window typed into: shown and hidden by turns */
LOCAL void caret_step( UW now )
{
	INT	blink = wm_num(LK_BLINK, 800);
	DTWIN	*f;

	if ( blink <= 0 ) {
		if ( dd_caret_hidden ) {
			dd_caret_hidden = FALSE;
			f = dt_win_of(wm_focused());
			if ( f != NULL ) {
				dt_draw(f);
				wm_composite();
			}
		}
		return;
	}
	if ( (UW)( now - caret_at ) < (UW)blink ) {
		return;
	}
	caret_at = now;
	f = dt_win_of(wm_focused());
	if ( f == NULL || !dd_editable(f) || ed_holding() ) {
		dd_caret_hidden = FALSE;
		return;
	}
	dd_caret_hidden = (BOOL)!dd_caret_hidden;
	dt_draw(f);
	wm_composite();
}

LOCAL void blink_step( void )
{
	UW	now = now_ms();
	INT	i;
	BOOL	any = FALSE;

	caret_step(now);
	if ( (UW)( now - blink_at ) < (UW)wm_num(LK_MARCH, 800) ) {
		return;
	}
	blink_at = now;
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( ed_blinks(&dt_win[i]) ) {
			any = TRUE;
		}
	}
	if ( !any ) {
		ed_blink_show();
		return;
	}
	ed_blink_turn();
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( ed_blinks(&dt_win[i]) ) {
			dt_draw(&dt_win[i]);
		}
	}
	wm_composite();
}

/*
 * Every window drawn again. A record that changed is not only shown in
 * its own window: a window whose page holds an open virtual object of
 * it shows it too, and drawing only the window the change was made in
 * leaves the other showing the object as it was.
 */
EXPORT void dt_draw_all( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used ) {
			dt_draw(&dt_win[i]);
		}
	}
}

/* ---------------------------------------------------------------- pages of links */

#define BASE_PAD	10		/* round the list */
#define BASE_GAP	4		/* between one and the next */
#define BASE_CHSZ	14

LOCAL SZ put_s( UB *buf, SZ at, SZ max, CONST char *s )
{
	while ( *s != 0 && at + 1 < max ) {
		buf[at++] = (UB)*s++;
	}
	buf[at] = 0;

	return at;
}

LOCAL SZ put_n( UB *buf, SZ at, SZ max, INT v )
{
	char	txt[12];
	INT	n = 0;

	if ( v < 0 ) {
		at = put_s(buf, at, max, "-");
		v = -v;
	}
	do {
		txt[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < 11 );
	while ( n > 0 && at + 1 < max ) {
		buf[at++] = (UB)txt[--n];
	}
	buf[at] = 0;

	return at;
}

/*
 * The 原紙箱's page: one closed virtual object for each template that is
 * in the store, one to a row, each as wide as its name. It is made as
 * a record, so that it is drawn, pressed and carried by the same code
 * as every other page.
 */
/* Words put into the record's text with what would end an attribute made safe */
LOCAL SZ put_x( UB *buf, SZ at, SZ max, CONST char *s )
{
	for ( ; *s != 0 && at + 7 < max; s++ ) {
		switch ( *s ) {
		case '&':	at = put_s(buf, at, max, "&amp;");	break;
		case '<':	at = put_s(buf, at, max, "&lt;");	break;
		case '>':	at = put_s(buf, at, max, "&gt;");	break;
		case '"':	at = put_s(buf, at, max, "&quot;");	break;
		default:	buf[at++] = (UB)*s;			break;
		}
	}
	buf[at] = 0;

	return at;
}

/*
 * A page that is a list of objects: one closed virtual object for each,
 * one to a row, each as wide as its name. 'names' gives the names to
 * show; without it, each object's own name. It is made as a record, so
 * that it is drawn, pressed and carried by the same code as every
 * other page.
 */
EXPORT T_TAD *dt_list_record( CONST char *title, CONST TS_UUID *ids,
			      CONST char *CONST *names, INT n )
{
	UB		*xml;
	SZ		max, at = 0;
	T_TAD		*doc = NULL;
	INT		i;
	INT		y = BASE_PAD + BASE_GAP, band = BASE_CHSZ + 11;
	INT		fid = fn_system();

	max = 512 + (SZ)( n > 0 ? n : 0 ) * ( 480 + TAD_NAME_MAX * 5 );
	xml = (UB *)Kmalloc(max);
	if ( xml == NULL ) {
		return NULL;
	}
	at = put_s(xml, at, max,
		   "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	at = put_x(xml, at, max, title);
	at = put_s(xml, at, max, "\"><figure>");
	for ( i = 0; i < n; i++ ) {
		UB	nm[TAD_NAME_MAX];
		char	txt[40];
		INT	w;

		if ( ts_uuid_to_str(&ids[i], txt, sizeof(txt)) < E_OK
		  || om_store_name(&ids[i], nm, TAD_NAME_MAX) < 0 ) {
			continue;		/* not in this store */
		}
		if ( names != NULL && names[i] != NULL ) {
			INT	k;

			for ( k = 0; names[i][k] != 0 && k < TAD_NAME_MAX - 1; k++ ) {
				nm[k] = (UB)names[i][k];
			}
			nm[k] = 0;
		}
		w = ( fid > 0 ) ? fn_width(fid, nm) : BASE_CHSZ * 6;
		w += BASE_CHSZ + 20;		/* the pictogram and the edges */

		at = put_s(xml, at, max, "<link id=\"");
		at = put_s(xml, at, max, txt);
		at = put_s(xml, at, max, "_0.xtad\" name=\"");
		at = put_x(xml, at, max, (CONST char *)nm);
		at = put_s(xml, at, max, "\" vobjleft=\"");
		at = put_n(xml, at, max, BASE_PAD);
		at = put_s(xml, at, max, "\" vobjtop=\"");
		at = put_n(xml, at, max, y);
		at = put_s(xml, at, max, "\" vobjright=\"");
		at = put_n(xml, at, max, BASE_PAD + w);
		at = put_s(xml, at, max, "\" vobjbottom=\"");
		at = put_n(xml, at, max, y + band);
		at = put_s(xml, at, max, "\" height=\"");
		at = put_n(xml, at, max, band);
		at = put_s(xml, at, max, "\" chsz=\"");
		at = put_n(xml, at, max, BASE_CHSZ);
		at = put_s(xml, at, max,
			   "\" frcol=\"#000000\" chcol=\"#000000\""
			   " tbcol=\"#ffffff\" bgcol=\"#ffffff\""
			   " pictdisp=\"true\" namedisp=\"true\""
			   " framedisp=\"true\" autoopen=\"false\"/>");
		y += band + BASE_GAP;
	}
	at = put_s(xml, at, max, "</figure></tad>");
	if ( tad_parse(xml, at, NULL, &doc) < E_OK ) {
		doc = NULL;
	}
	Kfree(xml);

	return doc;
}

/*
 * What the system itself holds rather than any record: the cabinet it
 * opens with, the システム箱 (every box of the system is linked from it)
 * and the templates of the applications. No link points at these, and
 * each is counted as linked to once when the counts are made again.
 */
#define DT_CABINET	SYSDEF_CABINET		/* 「BTRON」: the first window */

EXPORT INT dt_roots( TS_UUID *ids, INT max )
{
	INT	n = 0;

	if ( n < max && ts_str_to_uuid(DT_CABINET, &ids[n]) >= E_OK ) {
		n++;
	}
	return n + dt_prog_roots(ids + n, max - n);
}

/* The name a template is shown by, or NULL when the link is not to one */
EXPORT CONST char *dt_base_name( CONST TS_UUID *target )
{
	return dt_prog_base_name(target);
}

/* ---------------------------------------------------------------- windows */

/*
 * A window showing one record. The record belongs to the store; the
 * name is what the object is called. It opens where its metadata says
 * it stood when last closed, or stepped down from the last one when
 * the metadata says nothing.
 */
/*
 * The program a window is opened with: the one asked for from 実行, or
 * the one the object's applist opens it with.
 */
LOCAL CONST UB	*win_app = NULL;

LOCAL BOOL app_is( CONST UB *app, CONST char *id )
{
	INT	i;

	for ( i = 0; app[i] != 0 && id[i] != 0; i++ ) {
		if ( app[i] != (UB)id[i] ) return FALSE;
	}
	return (BOOL)( app[i] == 0 && id[i] == 0 );
}

/*
 * A window of this program as an object: it says what it shows, and it
 * is told when someone asks it to close (design 18.13) -- another
 * program deleting it, or the pictogram in its band pressed.
 */
LOCAL void win_watch( DTWIN *d, CONST TS_UUID *shows )
{
	T_OBNTF	req;
	TS_UUID	u;

	d->okey = 0;
	if ( dt_port <= 0 || wm_obj_uuid(d->wid, &u) < E_OK ) {
		return;
	}
	(void)wm_obj_shows(d->wid, shows);
	d->okey = ob_opn_obj(&u, OB_OP_READ);
	if ( d->okey <= 0 ) {
		d->okey = 0;
		return;
	}
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE;
	if ( ob_ntf_evt(d->okey, OB_REC_ANY, &req, dt_port) < E_OK ) {
		(void)ob_cls_obj(d->okey);
		d->okey = 0;
	}
}

LOCAL void win_close( DTWIN *d );
LOCAL void auto_open( DTWIN *d );

/* The requests to close that have come: each such window closes */
LOCAL void close_requests( void )
{
	T_OBNTM	m;
	TS_UUID	u;
	SZ	asz = 0;
	INT	i;

	while ( dt_port > 0 && ob_rea_rec(dt_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( m.event != OB_E_CLOSE ) {
			continue;
		}
		for ( i = 0; i < DT_MAX_WIN; i++ ) {
			if ( dt_win[i].used && wm_obj_uuid(dt_win[i].wid, &u) >= E_OK
			  && ts_uuid_cmp(&u, &m.uuid) == 0 ) {
				win_close(&dt_win[i]);
				wm_composite();
				break;
			}
		}
	}
}

/* ---------------------------------------------------------------- links carried out of a program's window */

/*
 * One carry at a time, the hand being one: what the program asked, and
 * once it was let go over a window that takes links, where -- the link
 * goes there when the program asks for it with DT_RQ_PLACE, after it
 * has made what the link points at.
 */
LOCAL struct {
	BOOL	on;			/* the outline follows the hand */
	T_DTREQ	rq;
	BOOL	pending;		/* let go where a link can go, the link not yet asked for */
	UINT	seq;
	INT	wid;			/* the window it was let go over */
	TS_UUID	into;			/* and that window's object */
	INT	sx, sy;			/* the outline's corner there, on the screen */
	INT	x0, y0;			/* where the hand was when the carry began */
} carry;

/*
 * The first form of a request and its answer, before a carried link
 * was asked for: a program written for it asks and reads in that size,
 * and is answered in it.
 */
#define DTREQ_OPEN_SIZE	( (SZ)( (UB *)&((T_DTREQ *)0)->look - (UB *)0 ) )
#define DTANS_OPEN_SIZE	( (SZ)( (UB *)&((T_DTANS *)0)->into - (UB *)0 ) )

LOCAL void answer_as( CONST TS_UUID *reply, CONST T_DTANS *an, SZ size )
{
	TS_UUID	none;
	SZ	asz = 0;
	ID	k;

	knl_memset(&none, 0, sizeof(none));
	if ( ts_uuid_cmp(reply, &none) == 0 ) {
		return;
	}
	k = ob_opn_obj(reply, OB_OP_WRITE);
	if ( k > 0 ) {
		(void)ob_wri_rec(k, 0, 0, an, size, &asz);
		(void)ob_cls_obj(k);
	}
}

LOCAL void answer( CONST TS_UUID *reply, CONST T_DTANS *an )
{
	answer_as(reply, an, sizeof(*an));
}

EXPORT void dt_answer( CONST TS_UUID *reply, CONST T_DTANS *an, SZ size )
{
	answer_as(reply, an, size);
}

LOCAL void carry_answer( ER er, CONST TS_UUID *into )
{
	T_DTANS	an;

	knl_memset(&an, 0, sizeof(an));
	an.seq = carry.rq.seq;
	an.er = er;
	if ( into != NULL ) {
		an.into = *into;
	}
	answer(&carry.rq.reply, &an);
}

/* The outline where the hand is, held where it was taken */
LOCAL void carry_outline( INT px, INT py )
{
	T_DPRECT	r;

	r.left = px - carry.rq.grab_x;
	r.top = py - carry.rq.grab_y;
	r.right = r.left + carry.rq.look.w;
	r.bottom = r.top + carry.rq.look.h;
	wm_pointer_moved(px, py);
	(void)wm_set_drag(&r, 1);
}

/*
 * A program asks its link to be carried: only while the hand is still
 * down, which it may no longer be when the request comes -- a press
 * that was a click answers at once that nothing was carried.
 */
LOCAL void carry_start( CONST T_DTREQ *rq )
{
	INT	px = 0, py = 0;
	UINT	btn = 0;

	carry.rq = *rq;
	carry.rq.ask[0][sizeof(rq->ask[0]) - 1] = 0;
	carry.rq.ask[1][sizeof(rq->ask[1]) - 1] = 0;
	carry.rq.yes[sizeof(rq->yes) - 1] = 0;
	if ( carry.on || rq->look.w <= 0 || rq->look.h <= 0
	  || ts_hid_pointer(&px, &py, &btn) < E_OK || ( btn & 1 ) == 0 ) {
		/* why nothing is carried, for the console: a click is the usual one */
		tm_printf((UB *)"desktop: carry refused: %s, outline %dx%d, buttons 0x%x\n",
			  carry.on ? "one carried already" : "", (INT)rq->look.w, (INT)rq->look.h, btn);
		carry_answer(E_OBJ, NULL);
		return;
	}
	carry.on = TRUE;
	carry.pending = FALSE;
	carry.x0 = px;
	carry.y0 = py;
	carry_outline(px, py);
	wm_composite();
}

LOCAL void carry_follow( INT px, INT py )
{
	carry_outline(px, py);
}

/* Let go: over a window that takes links, and when the question asked is answered yes */
LOCAL void carry_release( INT px, INT py )
{
	DTWIN	*t;

	carry.on = FALSE;
	(void)wm_set_drag(NULL, 0);
	wm_composite();
	if ( px - carry.x0 < 4 && carry.x0 - px < 4 && py - carry.y0 < 4 && carry.y0 - py < 4 ) {
		carry_answer(E_OBJ, NULL);	/* let go where it was taken: a click, nothing carried */
		return;
	}
	t = dt_win_of(wm_at(px, py));
	if ( t == NULL || ( t->fig == NULL && t->doc == NULL ) || t->sealed || t->xml_view ) {
		(void)wm_msg_put("ここには置けません");
		carry_answer(E_NOEXS, NULL);
		return;
	}
	if ( carry.rq.ask[0][0] != 0
	  && !dt_confirm(t, (CONST char *)carry.rq.ask[0],
			 ( carry.rq.ask[1][0] != 0 ) ? (CONST char *)carry.rq.ask[1] : NULL,
			 "取消", ( carry.rq.yes[0] != 0 ) ? (CONST char *)carry.rq.yes : "実行") ) {
		wm_composite();
		carry_answer(E_ABORT, NULL);
		return;
	}
	wm_composite();
	carry.pending = TRUE;
	carry.seq = carry.rq.seq;
	carry.wid = t->wid;
	carry.into = t->id;
	carry.sx = px - carry.rq.grab_x;
	carry.sy = py - carry.rq.grab_y;
	carry_answer(E_OK, &t->id);
}

/*
 * A link a carry ends with, put where it was let go (moved by grab_x,
 * grab_y from the outline's corner), looking as asked. With more, the
 * carry stays open for the next link of the same outline.
 */
LOCAL ER carry_place( CONST T_DTREQ *rq, BOOL more )
{
	DTWIN	*t;
	T_VOBJ	v;

	if ( !carry.pending || rq->carry != carry.seq ) {
		return E_OBJ;
	}
	if ( !more ) {
		carry.pending = FALSE;
	}
	t = dt_win_of(carry.wid);
	if ( t == NULL || ts_uuid_cmp(&t->id, &carry.into) != 0 ) {
		return E_NOEXS;			/* the window went while the program worked */
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = rq->target;
	v.right = rq->look.w;
	v.bottom = rq->look.h;
	v.height = rq->look.h;
	v.chsz = rq->look.chsz;
	v.frcol = rq->look.frcol;
	v.chcol = rq->look.chcol;
	v.tbcol = rq->look.tbcol;
	v.bgcol = rq->look.bgcol;
	v.disp = ( rq->look.disp != 0 ) ? rq->look.disp : TAD_D_DEFAULT;
	v.autoopen = (BOOL)( rq->look.autoopen != 0 );
	v.zoom = 100;
	if ( !ed_place_link(t, carry.sx + rq->grab_x, carry.sy + rq->grab_y, &v) ) {
		return E_OBJ;
	}
	wm_composite();
	return E_OK;
}

/*
 * What processes ask: an object opened as a double click opens it, an
 * accessory started by its name, or a link carried out of the asking
 * program's window and put where it was let go. The answer, when a
 * channel is given for it, says which process was started, or where
 * the carried link went.
 */
/* The channels: the desktop's own port, and the one processes ask at */
LOCAL TS_UUID	dt_port_u, dt_req_u;

LOCAL void process_requests( void )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_VOBJ		v;
	SZ		asz = 0;
	CONST DTPROG	*list[64];
	INT		i, n;

	while ( dt_req > 0 && ob_rea_rec(dt_req, 0, 0, &rq, sizeof(rq), &asz) >= E_OK ) {
		if ( asz != (SZ)sizeof(rq)
		  && !( asz == DTREQ_OPEN_SIZE && ( rq.req == DT_RQ_OPEN || rq.req == DT_RQ_TOOL ) ) ) {
			continue;
		}
		if ( asz < (SZ)sizeof(rq) ) {
			knl_memset((UB *)&rq + asz, 0, sizeof(rq) - (SZ)asz);
		}
		knl_memset(&an, 0, sizeof(an));
		an.seq = rq.seq;
		an.er = E_OK;
		/*
		 * A process started for the request answers it itself, once
		 * it is there (dt_prog_run): the error of the start is the
		 * answer's for an accessory asked for by its name, and not
		 * for an object opened, whose answer is E_OK as a double
		 * click's would be.
		 */
		if ( rq.req == DT_RQ_OPEN || rq.req == DT_RQ_TOOL ) {
			dt_prog_reply(&rq.reply, rq.seq,
				      ( asz == DTREQ_OPEN_SIZE ) ? DTANS_OPEN_SIZE : (SZ)sizeof(an),
				      (BOOL)( rq.req == DT_RQ_TOOL ));
		}
		if ( rq.req == DT_RQ_OPEN ) {
			/*
			 * No link stands behind the request: its colours are
			 * not said, so the window takes the paper the object
			 * asks for (or the usual white) rather than a colour 0,
			 * which is black.
			 */
			knl_memset(&v, 0, sizeof(v));
			v.target = rq.target;
			v.recno = rq.recno;
			v.frcol = v.chcol = v.tbcol = v.bgcol = TAD_COL_NONE;
			dt_open_vobj(NULL, &v);
		} else if ( rq.req == DT_RQ_CARRY ) {
			carry_start(&rq);	/* answered when the hand lets go */
			continue;
		} else if ( rq.req == DT_RQ_PLACE || rq.req == DT_RQ_PLACE_MORE ) {
			an.er = carry_place(&rq, (BOOL)( rq.req == DT_RQ_PLACE_MORE ));
		} else if ( ( rq.req == DT_RQ_INSTALL || rq.req == DT_RQ_REMOVE )
			 && !knl_ob_pid_admin(knl_obchan_last_from(&dt_req_u)) ) {
			an.er = E_OACV;		/* only an administrator adds or takes away programs */
		} else if ( rq.req == DT_RQ_INSTALL ) {
			an.er = dt_pkg_install(&rq.target, (BOOL)( rq.recno == 1 ), &an.into, &an.how);
		} else if ( rq.req == DT_RQ_REMOVE ) {
			an.er = dt_prog_remove(&rq.target);
		} else if ( rq.req == DT_RQ_TOOL ) {
			rq.name[sizeof(rq.name) - 1] = 0;
			n = dt_prog_list(DT_PK_ACCESSORY, list, 64);
			an.er = E_NOEXS;
			for ( i = 0; i < n; i++ ) {
				if ( knl_strcmp((CONST char *)list[i]->name, (CONST char *)rq.name) == 0 ) {
					an.er = dt_prog_run(NULL, list[i], NULL);
					break;
				}
			}
		} else {
			an.er = E_PAR;
		}
		wm_composite();
		if ( dt_prog_reply_taken() ) {
			dt_prog_reply(NULL, 0, 0, FALSE);
			continue;		/* the starter answers */
		}
		dt_prog_reply(NULL, 0, 0, FALSE);
		answer_as(&rq.reply, &an, ( asz == DTREQ_OPEN_SIZE ) ? DTANS_OPEN_SIZE : (SZ)sizeof(an));
	}
}

/* The port the requests come to: a channel of this program's own */

/*
 * Either may fail to be made (the object layer refusing, a slot not
 * free): what is missing is said once and made again at the next turn
 * of the main loop, so that the processes' requests are never lost for
 * good.
 */
LOCAL BOOL	dt_port_said = FALSE;

LOCAL void port_open( void )
{
	T_OBCRE	c;
	TS_UUID	u;
	ER	er = E_OK;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( dt_port <= 0 && ( er = ob_cre_obj(&c, &u) ) >= E_OK ) {
		dt_port_u = u;
		dt_port = ob_opn_obj(&u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
		if ( dt_port < 0 ) {
			dt_port = 0;
		}
	}
	/* and the one processes find by its name */
	c.name = (CONST UB *)DT_REQ_NAME;
	c.flags = OB_F_GLOBAL;
	if ( dt_req <= 0
	  && ( ( er = ob_cre_obj(&c, &u) ) >= E_OK
	    || ob_fnd_nam((CONST UB *)DT_REQ_NAME, &u) >= E_OK ) ) {
		dt_req_u = u;
		dt_req = ob_opn_obj(&u, OB_OP_READ | OB_O_NOWAIT);
		if ( dt_req < 0 ) {
			dt_req = 0;
		}
	}
	if ( ( dt_port <= 0 || dt_req <= 0 ) && !dt_port_said ) {
		tm_printf((UB *)"TessronOS desktop: the request channel is not there yet (%d), made again later\n",
			  (INT)er);
		dt_port_said = TRUE;
	}
}

/* The desktop ended: nobody is left to read either port, so they go */
LOCAL void port_close( void )
{
	if ( dt_port > 0 ) {
		(void)ob_cls_obj(dt_port);
		(void)ob_del_obj(&dt_port_u);
	}
	if ( dt_req > 0 ) {
		(void)ob_cls_obj(dt_req);
		(void)ob_del_obj(&dt_req_u);
	}
	dt_port = dt_req = 0;
}

LOCAL INT win_open( CONST TS_UUID *id, INT recno, INT x, INT y )
{
	DTWIN		*d = NULL;
	T_TAD		*rec;
	T_DPRECT	o;
	INT		i, wid;
	UB		name[TAD_NAME_MAX];
	UB		app[DT_PROG_ID];

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( !dt_win[i].used ) {
			d = &dt_win[i];
			break;
		}
	}
	if ( d == NULL ) {
		return E_LIMIT;
	}
	app[0] = 0;
	if ( win_app != NULL ) {
		for ( i = 0; win_app[i] != 0 && i < DT_PROG_ID - 1; i++ ) app[i] = win_app[i];
		app[i] = 0;
	} else if ( om_store_default_app(id, app, DT_PROG_ID) < 0 ) {
		app[0] = 0;
	}
	{
		/* the 原紙箱 and the 小物箱 show what the program box holds */
		BOOL	box = app_is(app, "base-file-manager");
		BOOL	acc = app_is(app, "accessory-box");

		rec = box ? dt_prog_page(DT_PK_APP)
		    : acc ? dt_prog_page(DT_PK_ACCESSORY) : om_store_get(id, recno);
		if ( rec == NULL ) {
			return E_NOEXS;
		}
		knl_memset(d, 0, sizeof(*d));
		d->sealed = (BOOL)( box || acc );
	}
	if ( om_store_name(id, name, TAD_NAME_MAX) < 0 ) {
		name[0] = 0;
	}
	if ( om_store_window(id, &o) < E_OK ) {
		o.left = x;  o.top = y;
		o.right = x + 900;  o.bottom = y + 640;
	}
	/* kept on the screen: a window whose band is off it cannot be moved */
	if ( o.left < 0 ) {
		o.right -= o.left;
		o.left = 0;
	}
	if ( o.top < 0 ) {
		o.bottom -= o.top;
		o.top = 0;
	}
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			  | WM_ATTR_RBAR | WM_ATTR_BBAR, (CONST char *)name);
	if ( wid < 0 ) {
		if ( d->sealed ) {
			tad_free(rec);
		}
		return wid;
	}
	d->used     = TRUE;
	d->wid      = wid;
	d->rec      = rec;
	d->kind     = tv_kind(rec);
	d->id       = *id;
	d->recno    = recno;
	d->paper    = TV_PAPER_LOOK;
	d->figed    = (BOOL)( d->kind == TV_KIND_FIG && !d->sealed
			   && app_is(app, "basic-figure-editor") );
	win_watch(d, id);
	om_store_paper(id, &d->paper);
	ed_model(d);
	for ( i = 0; i < TAD_NAME_MAX; i++ ) {
		d->name[i] = name[i];
	}
	wm_focus(wid);
	dt_draw(d);
	if ( d->figed ) {
		df_panel_show(d);
	}

	return wid;
}

/*
 * A window of one of the system's tools, showing a page the tool made.
 * The page is the window's: it goes with the window.
 */
EXPORT DTWIN *dt_open_made( T_TAD *rec, CONST char *title, UINT tool,
			    CONST T_DPRECT *o )
{
	DTWIN	*d = NULL;
	INT	i, wid;

	if ( rec == NULL ) {
		return NULL;
	}
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( !dt_win[i].used ) {
			d = &dt_win[i];
			break;
		}
	}
	if ( d == NULL ) {
		tad_free(rec);
		return NULL;
	}
	wid = wm_open(o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			 | WM_ATTR_RBAR | WM_ATTR_BBAR, title);
	if ( wid < 0 ) {
		tad_free(rec);
		return NULL;
	}
	knl_memset(d, 0, sizeof(*d));
	d->used   = TRUE;
	d->sealed = TRUE;
	d->tool   = tool;
	d->wid    = wid;
	d->rec    = rec;
	d->kind   = tv_kind(rec);
	d->recno  = 0;
	d->paper  = TV_PAPER_LOOK;
	for ( i = 0; title[i] != 0 && i < TAD_NAME_MAX - 1; i++ ) {
		d->name[i] = (UB)title[i];
	}
	d->name[i] = 0;
	win_watch(d, NULL);
	ed_model(d);
	wm_focus(wid);
	dt_draw(d);

	return d;
}

/* A tool's page made again: the old one goes, and nothing stays taken */
EXPORT void dt_remade( DTWIN *d, T_TAD *rec )
{
	if ( d == NULL || !d->used || rec == NULL ) {
		if ( rec != NULL ) {
			tad_free(rec);
		}
		return;
	}
	ed_forget(d);
	if ( d->rec != NULL ) {
		tad_free(d->rec);
	}
	d->rec = rec;
	d->kind = tv_kind(rec);
	d->npick = 0;
	d->scroll_x = d->scroll_y = 0;
	knl_memset(d->pick, 0, sizeof(d->pick));
	ed_model(d);
	dt_draw(d);
}


/* The window the desktop opened with, the cabinet's */
EXPORT DTWIN *dt_first_win( void )
{
	return dt_win_of(dt_first_wid);
}

/* The window a tool is shown in, if it is open */
EXPORT DTWIN *dt_tool_window( UINT tool )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].tool == tool ) {
			return &dt_win[i];
		}
	}

	return NULL;
}

/*
 * What was changed in a window's record, written to its file. A window
 * with nothing changed is not written: saving an unchanged record
 * would only move its date.
 */
LOCAL ER win_save( DTWIN *d )
{
	ER	er;

	if ( d == NULL || !d->used || !d->dirty || d->sealed ) {
		return E_OK;
	}
	/* 原稿 being edited: its text read back into the object first */
	if ( d->xml_view && !dd_xml_commit(d) ) {
		return E_PAR;
	}
	er = om_store_save(&d->id, d->recno);
	(void)wm_msg_put(( er >= E_OK ) ? "保存しました" : "保存できませんでした");
	if ( er >= E_OK ) {
		d->dirty = FALSE;
		/* the counts of what it links to may have moved */
		dt_tool_refresh(DT_TOOL_TRASH);
	} else {
		tm_printf((UB *)"TessronOS desktop: not saved (%d)\n", (INT)er);
	}

	return er;
}

LOCAL BOOL str_is( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

LOCAL void str_put( UB *out, CONST char *s, INT max )
{
	INT	i;

	for ( i = 0; s[i] != 0 && i < max - 1; i++ ) out[i] = (UB)s[i];
	out[i] = 0;
}

/*
 * A window opened from a link, closing: where it was scrolled to, how it
 * was shown (viewMode, wordWrap) and how large (zoomratio) go back into
 * that link, so that the link opens the same way next time and shows
 * the same part when it is opened in place. The parent is saved when
 * it is not open; when it is, it is marked changed and saved with it.
 */
LOCAL void to_link( DTWIN *d )
{
	DTWIN	*p = dt_showing(&d->parent_id);
	T_TAD	*rec;
	T_VOBJ	*v;
	BOOL	changed = FALSE;

	if ( p != NULL && p->sealed ) {
		return;
	}
	rec = ( p != NULL ) ? p->rec : om_store_get(&d->parent_id, 0);
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( rec == NULL || v == NULL || tad_lnk_find(rec, &d->parent_vobj, v) < E_OK ) {
		if ( v != NULL ) Kfree(v);
		return;
	}
	if ( v->scrollx != d->scroll_x || v->scrolly != d->scroll_y ) {
		v->scrollx = d->scroll_x;
		v->scrolly = d->scroll_y;
		changed = TRUE;
	}
	if ( d->kind == TV_KIND_DOC ) {
		CONST char	*vm = d->xml_view ? "xml" : d->detail_view ? "detailed" : "formatted";
		INT		wrap = d->nowrap ? TAD_WRAP_OFF : TAD_WRAP_ON;

		if ( !str_is(v->viewmode, vm) ) {
			str_put(v->viewmode, vm, TAD_VIEW_MAX);
			changed = TRUE;
		}
		if ( v->wordwrap != wrap ) {
			v->wordwrap = wrap;
			changed = TRUE;
		}
	} else if ( d->figed ) {
		INT	z = df_zoom() * 100 / 8;

		if ( !str_is(v->viewmode, "canvas") ) {
			str_put(v->viewmode, "canvas", TAD_VIEW_MAX);
			changed = TRUE;
		}
		if ( z > 0 && v->zoom != z ) {
			v->zoom = z;
			changed = TRUE;
		}
	}
	if ( changed && tad_lnk_set(rec, v) >= E_OK ) {
		if ( p != NULL ) {
			p->dirty = TRUE;
		} else {
			(void)om_store_save(&d->parent_id, 0);
		}
	}
	Kfree(v);
}

/*
 * A window opened from a link, as the link asks: a document in its
 * viewMode (清書, 詳細 or 原稿) with its wordWrap, and laid on the link's
 * bgcol when the object says no colour of its own.
 */
LOCAL void from_link( DTWIN *w, CONST T_VOBJ *v )
{
	if ( w == NULL ) {
		return;
	}
	if ( w->paper == TV_PAPER_LOOK && v->bgcol != TAD_COL_NONE ) {
		w->paper = v->bgcol;
	}
	if ( w->kind == TV_KIND_DOC && !w->sealed ) {
		if ( v->wordwrap != TAD_WRAP_NONE ) {
			w->nowrap = (BOOL)( v->wordwrap == TAD_WRAP_OFF );
		}
		if ( str_is(v->viewmode, "xml") ) {
			w->detail_view = FALSE;
			dd_xml_enter(w);
		} else {
			w->detail_view = str_is(v->viewmode, "detailed");
			dd_view(w);
		}
	}
	dt_draw(w);
}

/*
 * A close the person asked for. The window the desktop opened with is
 * where everything else is reached from; closing it is taken as ending
 * the session, so it is asked first: いいえ leaves the window as it is,
 * はい ends the desktop, whose windows are all saved and closed on the
 * way out, and turns the machine off (ts_desktop).
 */
LOCAL void win_close_asked( DTWIN *d )
{
	if ( d == NULL || !d->used ) {
		return;
	}
	if ( d->wid == dt_first_wid && dt_first_wid > 0 ) {
		if ( dt_confirm(d, "OSを終了しますか？", NULL, "いいえ", "はい") ) {
			dt_poweroff = TRUE;
			dt_stop = TRUE;
		}
		return;
	}
	win_close(d);
}

LOCAL void win_close( DTWIN *d )
{
	INT	i;

	if ( d == NULL || !d->used ) {
		return;
	}
	tip_forget(d);				/* a composition in it, committed */
	if ( d->has_parent ) {
		to_link(d);
	}
	/*
	 * What was changed is kept. A window that is closed with changes
	 * in it and loses them is the one thing an editor must never do;
	 * asking first is for later, when there is a dialogue to ask with.
	 */
	win_save(d);
	{
		T_WMWIN	w;

		/* where it stood, so that it opens there next time */
		if ( d->tool == DT_TOOL_NONE && wm_ref(d->wid, &w) >= E_OK ) {
			om_store_set_window(&d->id, &w.outer);
		}
	}
	df_text_end(d);
	wm_close(d->wid);
	df_canvas_forget(d);
	df_forget(d);
	ed_forget(d);
	if ( d->xrec != NULL ) {
		tad_free(d->xrec);
		d->xrec = NULL;
	}
	d->xml_view = FALSE;
	if ( d->sealed && d->rec != NULL ) {
		tad_free(d->rec);		/* made here, not the store's */
	}
	for ( i = 0; i < d->nundo; i++ ) {
		Kfree(d->undo[i]);
	}
	for ( i = 0; i < d->nredo; i++ ) {
		Kfree(d->redo[i]);
	}
	d->nundo = d->nredo = 0;
	if ( d->okey > 0 ) {
		(void)ob_cls_obj(d->okey);	/* and its request goes with it */
		d->okey = 0;
	}
	d->used = FALSE;
	d->rec = NULL;
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].figed ) {
			return;
		}
	}
	df_panel_hide();
}

/* The window showing what a virtual object points at, if one is */
LOCAL DTWIN *win_showing( CONST T_VOBJ *v )
{
	INT	i, recno = ( v->recno >= 0 ) ? v->recno : 0;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].recno == recno
		  && ts_uuid_cmp(&dt_win[i].id, &v->target) == 0 ) {
			return &dt_win[i];
		}
	}

	return NULL;
}

/*
 * What a virtual object points at, opened in a window of its own. A
 * window already showing it is brought forward instead of a second one
 * being opened: two windows onto one object are two things that can
 * disagree about it.
 */
EXPORT void dt_open_vobj( DTWIN *from, CONST T_VOBJ *v )
{
	DTWIN		*w = win_showing(v);
	CONST DTPROG	*p;
	INT		i, n = 0, wid;
	UB		app[DT_PROG_ID];

	/* a program object: the program is run */
	p = dt_prog_of(&v->target);
	if ( p != NULL ) {
		(void)dt_prog_run(from, p, NULL);
		return;
	}
	/* an object a program that is a process opens: that process */
	if ( win_app == NULL && om_link_default_app(v, app, DT_PROG_ID) > 0 ) {
		p = dt_prog_find(app);
		if ( p != NULL && !p->builtin ) {
			(void)dt_prog_run(from, p, v);
			return;
		}
	}
	if ( w != NULL ) {
		wm_raise(w->wid);
		wm_focus(w->wid);
		wm_composite();
		return;
	}
	/*
	 * A new window, stepped down and across from the last so that it
	 * does not land exactly on the one it was opened from.
	 */
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used ) {
			n++;
		}
	}
	wid = win_open(&v->target, ( v->recno >= 0 ) ? v->recno : 0,
		       60 + n * 28, 40 + n * 28);
	if ( wid >= 0 ) {
		w = dt_win_of(wid);
		if ( w != NULL && from != NULL ) {
			w->has_parent  = TRUE;
			w->parent_id   = from->id;
			w->parent_vobj = v->vobjid;
		}
		from_link(w, v);
		auto_open(w);
		wm_composite();
	}
}

/*
 * 自動起動: the links of what a window has just opened on that ask to be
 * opened with it, opened as a double click opens them, each in a window
 * of its own. What is opened that way does the same in its turn; a link
 * to something already open brings that window forward and goes no
 * further, and the chain stops a few deep in any case.
 */
#define AUTO_DEPTH	4

LOCAL INT	auto_depth;

LOCAL void auto_open( DTWIN *d )
{
	INT	i, n;
	T_VOBJ	*v;

	if ( d == NULL || d->rec == NULL || auto_depth >= AUTO_DEPTH ) {
		return;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) {
		return;
	}
	auto_depth++;
	n = tad_lnk_count(d->rec);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(d->rec, i, v) >= E_OK && v->autoopen
		  && ( !v->hidden || d->show_hidden ) ) {
			dt_open_vobj(d, v);
		}
	}
	auto_depth--;
	Kfree(v);
}

/* The window showing an object, if one is */
EXPORT DTWIN *dt_showing( CONST TS_UUID *id )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && ts_uuid_cmp(&dt_win[i].id, id) == 0 ) {
			return &dt_win[i];
		}
	}

	return NULL;
}

EXPORT ER dt_save( DTWIN *d )
{
	return win_save(d);
}

EXPORT void dt_close( DTWIN *d )
{
	win_close_asked(d);
}

/*
 * 全画面表示: the window made the size of the screen, or put back where
 * it stood before.
 */
EXPORT void dt_fullscreen( DTWIN *d )
{
	T_WMWIN		w;
	T_DPRECT	o;
	UINT		sw, sh;

	if ( d == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	if ( !d->full ) {
		if ( dt_screen_size(&sw, &sh) < E_OK ) {
			return;
		}
		d->restore = w.outer;
		o.left = 0;  o.top = 0;
		o.right = (INT)sw;
		o.bottom = (INT)sh - wm_msg_height();	/* above the message line */
	} else {
		o = d->restore;
	}
	if ( wm_move(d->wid, &o) >= E_OK ) {
		d->full = (BOOL)!d->full;
		dt_draw(d);
		wm_composite();
	}
}

/*
 * The object a virtual object points at opened with the program asked
 * for (実行): one that is a process is started with it, one of the
 * desktop's own opens it in a window of its kind.
 */
EXPORT ER dt_open_as( DTWIN *from, CONST T_VOBJ *v, CONST UB *app )
{
	CONST DTPROG	*p = dt_prog_find(app);

	if ( p != NULL && ( !p->builtin || p->kind != DT_PK_APP ) ) {
		return dt_prog_run(from, p, v);
	}
	win_app = app;
	dt_open_vobj(from, v);
	win_app = NULL;

	return E_OK;
}

/*
 * The program at a place of an object's applist run on it (実行). Unless
 * the link keeps to its program (起動固定), the one run last is the one
 * the object opens with from now, when it is opened by a double press.
 */
EXPORT void dt_exec( DTWIN *from, CONST T_VOBJ *v, INT index )
{
	UB	app[DT_PROG_ID];

	if ( om_link_app_id(v, index, app, DT_PROG_ID) > 0 ) {
		if ( !dt_app_locked(v) ) {
			(void)om_store_set_default_app(&v->target, app);
		}
		(void)dt_open_as(from, v, app);
	} else {
		dt_open_vobj(from, v);
	}
}

/* The window a virtual object was opened into, closed */
EXPORT void dt_close_vobj( CONST T_VOBJ *v )
{
	DTWIN	*w = win_showing(v);

	if ( w != NULL ) {
		win_close_asked(w);
		wm_composite();
	}
}

/* ---------------------------------------------------------------- the menu */

/*
 * The menu of the program under the pointer, with what applies at that
 * place: whether the press was on a virtual object, whether that one is
 * open in a window, whether anything is taken.
 *
 * It is used in either of two ways. The button is pressed and let go
 * at once: the menu stays, and a row is chosen with the first button,
 * and a press anywhere off the menu puts it away. Or the button is held,
 * carried to a row and let go there, which chooses that row. Which of
 * the two it was is told by how long the button was down -- as long as
 * a press of a double press, and it was a click.
 */
EXPORT INT dt_run_menu( DTWIN *d, INT pid, UD when )
{
	T_WMEV	ev;
	INT	cmd = 0;
	UD	gap;

	wm_panel_draw(pid);
	wm_composite();

	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	while ( !dt_stop ) {
		BOOL	on_menu;

		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		on_menu = wm_menu_has(pid, ev.wid);
		if ( ev.type == HID_EV_BTN_DOWN && !on_menu ) {
			break;		/* a press off the menu puts it away */
		}
		if ( ev.type == HID_EV_BTN_UP && ev.code == 1
		  && ev.when - when < gap ) {
			continue;	/* a click: the menu stays */
		}
		if ( wm_menu_event(pid, &ev, &cmd) < E_OK ) {
			break;
		}
		/* the rows that changed and the pointer, not the screen */
		wm_update();
		if ( cmd != 0 ) {
			break;
		}
		if ( ev.type == HID_EV_BTN_UP && !on_menu ) {
			break;		/* held, carried off it and let go */
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ESC ) {
			break;
		}
	}
	wm_panel_close(pid);
	wm_composite();

	return cmd;
}

LOCAL void doc_do( DTWIN *d, INT cmd, BOOL on_vobj, CONST T_VOBJ *v );

/* A document's menu, set to what the document is doing */
LOCAL ER doc_menu( DTWIN *d, ID *p_mid )
{
	T_DOCMENU	st;

	knl_memset(&st, 0, sizeof(st));
	st.view      = d->xml_view ? DM_VIEW_XML
		     : d->detail_view ? DM_VIEW_DETAIL : DM_VIEW_CLEAN;
	st.wrap      = (BOOL)!d->nowrap;
	st.show_hidden = d->show_hidden;
	st.paper_frame = d->paper_frame;
	st.is_root   = (BOOL)!d->has_parent;
	st.has_pick  = (BOOL)( d->npick > 0 || d->cpara != d->apara
				  || d->cpos != d->apos );
	st.nrecent   = dd_recent_faces(st.recent, DM_RECENT_MAX);
	st.can_paste = (BOOL)( dt_tray_kind(NULL) != DT_TRAY_NONE );

	return doc_menu_make(&st, p_mid);
}

/*
 * A window's menu: made from the definition of the program the window
 * shows -- a tool's, the figure editor's, the cabinet's (a figure of
 * virtual objects), the text editor's -- set to what the window is, and
 * the system's items set for the virtual object it was opened on.
 */
LOCAL ER win_menu( DTWIN *d, BOOL on_vobj, CONST T_VOBJ *v, ID *p_mid )
{
	ER	er;

	if ( d->tool != DT_TOOL_NONE ) {
		er = dt_tool_menu_make(d, on_vobj, p_mid);
	} else if ( d->fig != NULL ) {
		er = df_is(d) ? df_menu_make(d, p_mid) : dt_cab_menu_make(d, p_mid);
	} else {
		er = doc_menu(d, p_mid);
	}
	if ( er >= E_OK ) {
		dt_get_vmn(d, *p_mid, on_vobj, v);
	}
	return er;
}

/* A chosen item done: the system's by dtvmn.c, the rest by the window's program */
LOCAL void win_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj, CONST T_VOBJ *v )
{
	if ( dt_exe_vmn(d, sel, on_vobj, v) ) {
		return;				/* the window may be gone */
	}
	if ( d->tool != DT_TOOL_NONE ) {
		dt_tool_menu_do(d, sel, on_vobj, v);
	} else if ( d->fig != NULL ) {
		if ( df_is(d) ) {
			df_menu_do(d, sel, on_vobj, v);
		} else {
			dt_cab_menu_do(d, sel, on_vobj, v);
		}
	} else {
		doc_do(d, doc_menu_cmd(sel), on_vobj, v);
	}
	wm_composite();
}

LOCAL void menu_at( DTWIN *d, INT x, INT y, UD when )
{
	T_DPRECT	page, box;
	T_VOBJ		v;
	T_MNSEL		sel;
	INT		pid, cmd = 0, which = -1;
	BOOL		on;
	ID		mid;
	ER		er = E_NOEXS;

	/*
	 * A press of the second button on an object that is not taken
	 * takes it first, so that what the menu does is done to the
	 * object it was opened on. A press outside the window (on the
	 * ground, which gives the menu to the active window) is on no
	 * object: the window's menu opens where the press was.
	 */
	dt_work_rect(d, &page);
	{
		T_WMWIN	ww;

		if ( wm_ref(d->wid, &ww) >= E_OK
		  && ( x < 0 || y < 0 || x >= ww.work.right - ww.work.left
		    || y >= ww.work.bottom - ww.work.top ) ) {
			/* outside: no object is looked for */
			goto no_object;
		}
	}
	if ( d->doc != NULL ) {
		er = tv_doc_at(d->doc, &page, d->scroll_y, d->rec, x, y,
			       &v, &box, &which);
	} else if ( d->fig != NULL ) {
		er = tv_fig_at(d->fig, &page, d->scroll_x, d->scroll_y,
			       d->rec, x, y, &v, &box, &which);
		if ( er >= E_OK && !ed_picked(d, which) ) {
			ed_pick_only(d, which);
			dt_draw(d);
			wm_composite();
		}
	}
no_object:
	on = (BOOL)( er >= E_OK );
	if ( win_menu(d, on, &v, &mid) < E_OK ) {
		return;
	}
	pid = mn_opn_men(mid, d->wid, x, y);
	if ( pid >= 0 ) {
		cmd = dt_run_menu(d, pid, when);
	}
	er = mn_get_sel(mid, cmd, &sel);
	(void)mn_del_men(mid);
	if ( er >= E_OK ) {
		win_menu_do(d, &sel, on, &v);
	}
	wm_composite();
}

/*
 * What a row of a document's menu does, as the editor's command. The
 * rows about saving and the window that the document shares with the
 * cabinet are done by the cabinet's code; the rest are the editor's
 * (dtdoc.c).
 */
LOCAL void doc_do( DTWIN *d, INT cmd, BOOL on_vobj, CONST T_VOBJ *v )
{
	switch ( cmd ) {
	case DM_SAVE_AS_NEW:
		dt_cab_command(d, CM_SAVE_AS_NEW, on_vobj, v);
		return;
	case DM_BGCOLOUR:
		dt_cab_command(d, CM_BGCOLOUR, on_vobj, v);
		return;
	default:
		break;
	}
	switch ( cmd ) {
	case DM_SAVE:
		win_save(d);
		break;
	case DM_REFRESH:
		dt_draw(d);
		break;
	case DM_FULLSCREEN:
		dt_fullscreen(d);
		break;
	case DM_WRAP:
		d->nowrap = (BOOL)!d->nowrap;
		dd_view(d);
		break;
	case DM_SHOW_HIDDEN:
		d->show_hidden = (BOOL)!d->show_hidden;
		dd_view(d);
		break;
	case DM_PAPER_FRAME:
		d->paper_frame = (BOOL)!d->paper_frame;
		dd_view(d);
		break;
	case DM_VIEW_SET_XML:
		if ( !d->xml_view ) {
			d->detail_view = FALSE;
			dd_xml_enter(d);
		}
		break;
	case DM_VIEW_SET_DETAIL:
	case DM_VIEW_SET_CLEAN:
		if ( d->xml_view && !dd_xml_leave(d) ) {
			break;		/* the text does not read as a record */
		}
		d->detail_view = (BOOL)( cmd == DM_VIEW_SET_DETAIL );
		dd_view(d);
		break;
	default:
		(void)dd_command(d, cmd);
		break;
	}
}

/* ---------------------------------------------------------------- asking */

#define ASK_W		396		/* its contents, and eight all round */
#define ASK_H		128
#define ASK_BOX		1
#define ASK_CANCEL	2
#define ASK_OK		3

LOCAL void ask_label( UB *to, CONST char *s )
{
	INT	i;

	for ( i = 0; s[i] != 0 && i < WM_LABEL_MAX - 1; i++ ) {
		to[i] = (UB)s[i];
	}
	to[i] = 0;
}

/*
 * A name asked for, in a panel in the middle of a window: what it is
 * for, a box holding a name to start from, and 取消 and 設定. Enter is
 * 設定 and Escape is 取消, as they are in any panel that asks one
 * thing. Answers TRUE with the name in 'out' when it was agreed to and
 * is not empty.
 */
EXPORT BOOL dt_ask_name( DTWIN *d, CONST char *prompt, CONST UB *start,
			 UB *out, INT max )
{
	T_WMPANEL	*def;
	T_WMWIN		w;
	T_WMEV		ev;
	INT		pid, cx, cy, i;
	UINT		ans = WM_ANS_NONE;

	if ( d == NULL || out == NULL || max < 2
	  || wm_ref(d->wid, &w) < E_OK ) {
		return FALSE;
	}
	def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( def == NULL ) {
		return FALSE;
	}
	knl_memset(def, 0, sizeof(*def));
	cx = ( w.work.right - w.work.left ) / 2;
	cy = ( w.work.bottom - w.work.top ) / 2;
	def->num   = 1;
	def->owner = 0;
	def->kind  = WM_PNL_PLAIN;
	def->r.left   = cx - ASK_W / 2;
	def->r.top    = cy - ASK_H / 2;
	def->r.right  = def->r.left + ASK_W;
	def->r.bottom = def->r.top + ASK_H;
	def->npart = 4;

	def->part[0].type = WM_PT_LABEL;
	def->part[0].num  = 0;
	def->part[0].r.left = 22;  def->part[0].r.top = 18;
	def->part[0].r.right = ASK_W - 22;  def->part[0].r.bottom = 40;
	ask_label(def->part[0].label, prompt);

	def->part[1].type = WM_PT_BOX;
	def->part[1].num  = ASK_BOX;
	def->part[1].r.left = 22;  def->part[1].r.top = 46;
	def->part[1].r.right = ASK_W - 22;  def->part[1].r.bottom = 72;
	for ( i = 0; start != NULL && start[i] != 0
		     && i < WM_LABEL_MAX - 1; i++ ) {
		def->part[1].text[i] = start[i];
	}
	def->part[1].text[i] = 0;
	def->part[1].caret = i;

	def->part[2].type   = WM_PT_BUTTON;
	def->part[2].num    = ASK_CANCEL;
	def->part[2].answer = WM_ANS_CANCEL;
	def->part[2].r.left = ASK_W - 204;  def->part[2].r.top = 84;
	def->part[2].r.right = ASK_W - 118;  def->part[2].r.bottom = 110;
	ask_label(def->part[2].label, "取消");

	def->part[3].type   = WM_PT_BUTTON | P_EMPHAS;
	def->part[3].num    = ASK_OK;
	def->part[3].answer = WM_ANS_OK;
	def->part[3].r.left = ASK_W - 108;  def->part[3].r.top = 84;
	def->part[3].r.right = ASK_W - 22;  def->part[3].r.bottom = 110;
	ask_label(def->part[3].label, "設定");

	pid = wm_panel_open_centre(d->wid, def);
	Kfree(def);
	if ( pid < 0 ) {
		return FALSE;
	}
	wm_focus(d->wid);
	wm_panel_draw(pid);
	wm_update();

	while ( !dt_stop && ans == WM_ANS_NONE ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ENTER ) {
			ans = WM_ANS_OK;
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) {
			ev.wid = d->wid;	/* the letters are the panel's */
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		wm_update();
	}
	out[0] = 0;
	if ( ans == WM_ANS_OK ) {
		wm_panel_text(pid, ASK_BOX, out, max);
	}
	wm_panel_close(pid);
	dt_draw(d);
	wm_composite();

	return (BOOL)( ans == WM_ANS_OK && out[0] != 0 );
}

/* ---------------------------------------------------------------- winding */

LOCAL void scroll_to( DTWIN *d, INT to )
{
	T_DPRECT	r;
	INT		room;

	dt_work_rect(d, &r);
	room = d->height - ( r.bottom - r.top );
	if ( room < 0 ) {
		room = 0;
	}
	if ( to < 0 )    to = 0;
	if ( to > room ) to = room;
	if ( to == d->scroll_y ) {
		return;
	}
	d->scroll_y = to;
	dt_draw(d);
	wm_composite();
}

/* Across: a document is as wide as its page and does not go across */
LOCAL void scroll_x_to( DTWIN *d, INT to )
{
	T_DPRECT	r;
	INT		room;

	dt_work_rect(d, &r);
	room = ( d->doc == NULL ) ? d->width - ( r.right - r.left ) : 0;
	if ( room < 0 ) {
		room = 0;
	}
	if ( to > room ) to = room;
	if ( to < 0 )    to = 0;
	if ( to == d->scroll_x ) {
		return;
	}
	d->scroll_x = to;
	dt_draw(d);
	wm_composite();
}

/*
 * A wheel turned over a window: three lines of the page for each notch,
 * up and down, or across for the wheel that goes across or for the
 * upright one turned with Shift held. Nothing moves when the person has
 * turned the wheel off (ユーザ環境設定, LK_PD_WHEEL).
 */
#define DT_WHEEL_STEP	48

LOCAL void wheel( DTWIN *d, CONST T_WMEV *ev )
{
	BOOL	across = (BOOL)( ev->code == HID_WHEEL_H
			      || ( ev->mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 );

	if ( d == NULL || ev->dz == 0 || wm_num(LK_PD_WHEEL, 1) == 0 ) {
		return;
	}
	if ( across ) {
		/* the upright wheel turned away goes left; the across one goes the way it turns */
		scroll_x_to(d, d->scroll_x + ( ev->code == HID_WHEEL_H ? ev->dz : -ev->dz ) * DT_WHEEL_STEP);
	} else {
		scroll_to(d, d->scroll_y - ev->dz * DT_WHEEL_STEP);
	}
}

/* ---------------------------------------------------------------- the screen's size */

/*
 * The screen, through its object 画面 (OB_S_DISPLAY): the size is read
 * from its record 1 and a new size is written there. The key is opened
 * once and kept.
 */
LOCAL ID	dt_disp_key = 0;

LOCAL ID disp_key( void )
{
	if ( dt_disp_key <= 0 ) {
		dt_disp_key = ob_opn_obj(&ob_uuid_display, OB_OP_READ | OB_OP_WRITE);
		if ( dt_disp_key <= 0 ) dt_disp_key = 0;
	}
	return dt_disp_key;
}

LOCAL BOOL size_of( CONST char *s, UINT *p_w, UINT *p_h );

EXPORT ER dt_screen_size( UINT *p_w, UINT *p_h )
{
	UB	t[512];
	char	v[32];
	SZ	asz = 0;
	ID	key = disp_key();
	ER	er;

	if ( key <= 0 ) {
		return E_NOEXS;
	}
	er = ob_rea_rec(key, OB_DSP_MODE, 0, t, sizeof(t), &asz);
	if ( er < E_OK ) {
		return er;
	}
	if ( !cf_get(t, (INT)asz, "SIZE", 0, v, sizeof(v)) || !size_of(v, p_w, p_h) ) {
		return E_NOEXS;
	}
	return E_OK;
}

LOCAL ER screen_set( UINT w, UINT h )
{
	char	s[32];
	ID	key = disp_key();

	if ( key <= 0 ) {
		return E_NOEXS;
	}
	tm_sprintf((UB *)s, (UB *)"%dx%d", (INT)w, (INT)h);
	return ob_wri_rec(key, OB_DSP_MODE, 0, s, (SZ)knl_strlen(s), NULL);
}

/*
 * The screen size the machine's settings ask for (システム環境設定,
 * VIDEOMODE new [old]): set as the desktop starts, before a window is
 * open. A size that has not been seen yet -- one written with the size
 * it replaces -- is asked about once the first window is up: Enter within
 * thirty seconds keeps it, anything else, or nothing, puts back the old.
 */
LOCAL UINT	vm_w, vm_h, vm_ow, vm_oh;	/* the size asked for, and the one before */

LOCAL BOOL size_of( CONST char *s, UINT *p_w, UINT *p_h )
{
	INT	w = 0, h = 0, i = 0;

	while ( s[i] >= '0' && s[i] <= '9' ) w = w * 10 + ( s[i++] - '0' );
	if ( s[i] != 'x' ) return FALSE;
	i++;
	while ( s[i] >= '0' && s[i] <= '9' ) h = h * 10 + ( s[i++] - '0' );
	if ( s[i] != 0 || w <= 0 || h <= 0 ) return FALSE;
	*p_w = (UINT)w;
	*p_h = (UINT)h;
	return TRUE;
}

LOCAL void video_start( void )
{
	char	v[CF_VAL_MAX], a[32], b[32];

	vm_w = vm_h = vm_ow = vm_oh = 0;
	if ( !wm_dev_conf_get("VIDEOMODE", v, sizeof(v)) || !cf_word(v, 0, a, sizeof(a))
	  || !size_of(a, &vm_w, &vm_h) ) {
		return;
	}
	if ( cf_word(v, 1, b, sizeof(b)) ) (void)size_of(b, &vm_ow, &vm_oh);
	if ( screen_set(vm_w, vm_h) < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: the screen cannot be %d x %d\n", (INT)vm_w, (INT)vm_h);
		vm_ow = vm_oh = 0;
	}
}

LOCAL void video_confirm( DTWIN *d )
{
	CONST char *CONST lines[5] = {
		"画面の設定を切り換えましたので表示を確認してください。",
		"正しく表示されているときは、３０秒以内に［Ｅｎｔｅｒ］",
		"キーか［確認］を押してください。",
		"［Ｅｎｔｅｒ］以外のキーを押すか、３０秒以上キーを押さ",
		"ないと、以前の画面の設定に戻します。",
	};
	char	s[32];
	INT	i;

	if ( d == NULL || vm_ow == 0 || vm_oh == 0 ) {
		return;
	}
	if ( dt_confirm_timed(d, lines, 5, "取り消し", "確認", 30000) ) {
		tm_sprintf((UB *)s, (UB *)"%dx%d", (INT)vm_w, (INT)vm_h);
		tm_printf((UB *)"TessronOS desktop: the screen of %s kept\n", s);
	} else {
		(void)screen_set(vm_ow, vm_oh);
		tm_sprintf((UB *)s, (UB *)"%dx%d", (INT)vm_ow, (INT)vm_oh);
		tm_printf((UB *)"TessronOS desktop: the screen put back to %s\n", s);
		for ( i = 0; i < DT_MAX_WIN; i++ ) {
			if ( dt_win[i].used ) dt_draw(&dt_win[i]);
		}
		(void)dt_res_wall(WM_WALL_FIT);
		wm_composite();
	}
	(void)wm_dev_conf_put("VIDEOMODE", s);	/* seen: no longer asked */
}

/* ---------------------------------------------------------------- the loop */

/*
 * A press in a window. The bars are asked first: a press on one is not
 * a press on the page.
 *
 * The event carries the place in the work area's own coordinates and
 * the bars are outside the work area, so the place is turned back into
 * a place on the screen to ask about them. A press on a bar comes
 * through with the work area's corner subtracted from it like any
 * other, and putting it back is the only way to say where it really
 * was.
 */
LOCAL void press( DTWIN *d, CONST T_WMEV *ev )
{
	UINT		which = 0;
	INT		hit, sx, sy;
	T_WMWIN		w;
	T_DPRECT	r;

	if ( wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	/* on the candidates of a composition, or elsewhere: it is committed */
	if ( tip_press(d, ev->x, ev->y) ) {
		return;
	}
	sx = w.work.left + ev->x;
	sy = w.work.top + ev->y;

	hit = wm_bar_at(d->wid, sx, sy, &which);

	/*
	 * A press that is not on a bar and not in the work area is on the
	 * frame: the band carries the window, the corner at its foot
	 * sizes it. The bars are asked first because they are part of the
	 * frame too, and a press on one is not a press to carry it.
	 */
	if ( hit == WM_BARHIT_NONE
	  && ( ev->x < 0 || ev->y < 0
	    || sx >= w.work.right || sy >= w.work.bottom ) ) {
		INT	got = 0;
		UINT	bar = 0;

		/* a double press on the pictogram in the band closes the window */
		if ( wm_part_at(sx, sy, &got, &bar) == WM_PART_PICT && got == d->wid ) {
			if ( wm_pict_close(d->wid, sx, sy, ev->when) ) {
				win_close_asked(d);
				wm_composite();
			}
			return;
		}
		dt_move_wid = d->wid;
		dt_sizing = (BOOL)( sx > w.outer.right - 20
				 && sy > w.outer.bottom - 20 );
		dt_grab_dx = sx - ( dt_sizing ? w.outer.right : w.outer.left );
		dt_grab_dy = sy - ( dt_sizing ? w.outer.bottom : w.outer.top );
		wm_set_pointer(dt_sizing ? WM_PT_PICK : WM_PT_MOVE);
		return;
	}
	if ( hit != WM_BARHIT_NONE ) {
		INT	page;

		dt_work_rect(d, &r);
		page = r.bottom - r.top;
		if ( which == WM_BAR_B ) {
			page = r.right - r.left;
		}
		switch ( hit ) {
		case WM_BARHIT_BEFORE:
			if ( which == WM_BAR_B ) scroll_x_to(d, d->scroll_x - page);
			else scroll_to(d, d->scroll_y - page);
			break;
		case WM_BARHIT_AFTER:
			if ( which == WM_BAR_B ) scroll_x_to(d, d->scroll_x + page);
			else scroll_to(d, d->scroll_y + page);
			break;
		default:
			/* the knob: it is held until the button is let go */
			dt_hold_wid = d->wid;
			dt_hold_bar = which;
			wm_set_pointer(( which == WM_BAR_B ) ? WM_PT_HGRIP
							    : WM_PT_VGRIP);
			break;
		}
		return;
	}

	/* a piece of text in a figure: edited where it stands */
	if ( df_text_press(d, ev->x, ev->y, ev->when) ) {
		return;
	}
	/* a shape drawn with the tool in hand */
	if ( df_tool_press(d, ev->x, ev->y, ev->when, ev->mods) ) {
		return;
	}
	/* a handle of 変形, or the end of a picked line, carried */
	if ( df_transform_press(d, ev->x, ev->y) || df_end_press(d, ev->x, ev->y) ) {
		return;
	}
	/* 詳細: the flags and rulers of the settings */
	if ( dd_mark_press(d, ev->x, ev->y, ev->when) ) {
		return;
	}
	/* the page: taking, carrying, sizing, opening; in a text, the caret */
	if ( !ed_press(d, ev->x, ev->y, (BOOL)( ( ev->mods & MOD_SHIFT ) != 0 ),
		       ev->when) ) {
		if ( dd_editable(d) ) {
			dd_press(d, ev->x, ev->y,
				 (BOOL)( ( ev->mods & MOD_SHIFT ) != 0 ));
		}
	} else if ( df_is(d) ) {
		df_take_look(d);		/* the palettes follow what is picked */
	}
}

/*
 * A press on the window of a program running as a process. The window
 * comes to the front and has the keys; its band carries it and the
 * corner at its foot sizes it, as for a window of the desktop's own. A
 * subordinate window (a tool panel, wm_set_parent) is a window of its
 * own too, though it has no band: its frame carries it.
 * What is inside is the program's, told to it by the object layer; so
 * is the pictogram in the band, which asks the program to close.
 * Its scroll bars are worked here, as a window of the desktop's own
 * has them worked, and where they are asked to go is told to the
 * program (OB_E_SCROLL): the program scrolls and sets them again.
 */
LOCAL void obj_press( CONST T_WMEV *ev )
{
	T_WMWIN	w;
	T_WMBAR	b;
	INT	sx, sy, got = 0, hit;
	UINT	bar = 0, which = 0;

	if ( ev->wid <= 0 || wm_ref(ev->wid, &w) < E_OK
	  || ( w.attr & WM_ATTR_POPUP ) != 0 || ( ( w.attr & WM_ATTR_TITLE ) == 0 && w.parent <= 0 ) ) {
		return;				/* a menu, a panel: not a window of its own */
	}
	wm_raise(ev->wid);
	wm_focus(ev->wid);
	sx = w.work.left + ev->x;
	sy = w.work.top + ev->y;
	hit = ( ev->code == 0 ) ? wm_bar_at(ev->wid, sx, sy, &which) : WM_BARHIT_NONE;
	if ( hit != WM_BARHIT_NONE && wm_bar(ev->wid, which, &b) >= E_OK ) {
		INT	page = b.chi - b.clo, last = b.hi - page, to = b.clo;

		if ( last < b.lo ) last = b.lo;
		switch ( hit ) {
		case WM_BARHIT_BEFORE:
		case WM_BARHIT_AFTER:
			to += ( hit == WM_BARHIT_BEFORE ) ? -page : page;
			if ( to > last ) to = last;
			if ( to < b.lo ) to = b.lo;
			wm_obj_scroll(ev->wid, which, to, OB_SCR_PAGE);
			break;
		default:
			/* the knob: it is held until the button is let go */
			dt_hold_wid = ev->wid;
			dt_hold_bar = which;
			dt_hold_at = b.clo;
			wm_set_pointer(( which == WM_BAR_B ) ? WM_PT_HGRIP : WM_PT_VGRIP);
			break;
		}
		wm_composite();
		return;
	}
	if ( ev->code == 0 && ( ev->x < 0 || ev->y < 0
			       || sx >= w.work.right || sy >= w.work.bottom )
	  && !( wm_part_at(sx, sy, &got, &bar) == WM_PART_PICT && got == ev->wid ) ) {
		dt_move_wid = ev->wid;
		dt_sizing = (BOOL)( ( w.attr & WM_ATTR_RESIZE ) != 0
				 && sx > w.outer.right - 20 && sy > w.outer.bottom - 20 );
		dt_grab_dx = sx - ( dt_sizing ? w.outer.right : w.outer.left );
		dt_grab_dy = sy - ( dt_sizing ? w.outer.bottom : w.outer.top );
		wm_set_pointer(dt_sizing ? WM_PT_PICK : WM_PT_MOVE);
	}
	wm_composite();
}

/*
 * A key, for the window in front. What is done to the virtual objects
 * is dtedit.c's; saving and closing are the window's.
 */
/* The letter or digit a key is, as a menu letter */
LOCAL BOOL key_letter( INT code, UB *p_letter )
{
	if ( code >= KEY_A && code <= KEY_Z ) {
		*p_letter = (UB)( 'A' + ( code - KEY_A ) );
		return TRUE;
	}
	if ( code >= KEY_1 && code <= KEY_0 ) {
		*p_letter = ( code == KEY_0 ) ? (UB)'0' : (UB)( '1' + ( code - KEY_1 ) );
		return TRUE;
	}
	return FALSE;
}

LOCAL void key( DTWIN *d, CONST T_WMEV *ev )
{
	BOOL	ctrl  = (BOOL)( ( ev->mods & MOD_CTRL ) != 0 );
	T_MNSEL	sel;
	UB	letter;
	ID	mid;
	ER	er;

	/* the input mode is the keyboard's, whichever window has the keys */
	if ( tip_mode_key(d, ev->code, ev->mods) ) {
		return;
	}
	if ( ev->code >= 0xE0 && ev->code <= 0xE7 ) {
		return;				/* a modifier key, alone */
	}
	if ( d == NULL ) {
		return;
	}
	if ( d->tool != DT_TOOL_NONE && dt_tool_key(d, ev->code, ev->mods) ) {
		return;				/* the tool's own key */
	}
	if ( d->tb != NULL ) {
		/*
		 * typed into the piece of text being edited; Escape ends it,
		 * unless it is cancelling what is being composed
		 */
		if ( ev->code == KEY_ESC && !tip_busy(d->tb) ) {
			df_text_end(d);
		} else {
			(void)dd_key(d->tb, ev->code, ev->mods);
		}
		return;
	}
	if ( dd_editable(d) && dd_key(d, ev->code, ev->mods) ) {
		return;				/* typed into the text */
	}
	if ( df_tool_key(d, ev->code) ) {
		return;				/* a shape being drawn ended */
	}
	if ( !ctrl ) {
		if ( ev->code == KEY_DEL ) {
			ed_delete(d);
		}
		return;
	}
	/* a menu letter: the item of the window's menu that has it */
	if ( key_letter(ev->code, &letter) && win_menu(d, FALSE, NULL, &mid) >= E_OK ) {
		er = mn_fnd_key(mid, letter, &sel);
		(void)mn_del_men(mid);
		if ( er >= E_OK ) {
			win_menu_do(d, &sel, FALSE, NULL);
			return;
		}
	}
	/* the keys that are not in a menu */
	switch ( ev->code ) {
	case KEY_O:	ed_open_picked(d);		break;
	case KEY_A:
		ed_pick_all(d);
		dt_draw(d);
		wm_composite();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- dropping on another program */

/* s cut to fit max bytes with its end, not in the middle of a letter */
LOCAL void name_cut( UB *out, CONST UB *s, INT max )
{
	INT	n = 0;

	while ( s[n] != 0 && n < max - 1 ) {
		n++;
	}
	while ( n > 0 && s[n] != 0 && ( s[n] & 0xC0 ) == 0x80 ) {
		n--;
	}
	knl_memcpy(out, s, n);
	out[n] = 0;
}

EXPORT BOOL dt_drop_links( DTWIN *d, INT sx, INT sy, CONST T_VOBJ *v, INT n )
{
	T_OBDROPV	*dv;
	TS_UUID		from;
	UB		*name;
	INT		wid = wm_at(sx, sy), i;
	ER		er;

	if ( wid <= 0 || dt_win_of(wid) != NULL || n <= 0 ) {
		return FALSE;			/* the ground, or a window of the desktop's */
	}
	if ( n > OB_DROP_MAX ) {
		n = OB_DROP_MAX;
	}
	dv = (T_OBDROPV *)Kmalloc(sizeof(T_OBDROPV) * (SZ)n);
	name = (UB *)Kmalloc(TAD_NAME_MAX);
	if ( dv == NULL || name == NULL ) {
		if ( dv != NULL ) Kfree(dv);
		if ( name != NULL ) Kfree(name);
		return FALSE;
	}
	knl_memset(dv, 0, sizeof(T_OBDROPV) * (SZ)n);
	for ( i = 0; i < n; i++ ) {
		dv[i].target = v[i].target;
		dv[i].vobjid = v[i].vobjid;
		/* the name the link shows: its own, or the object's */
		if ( v[i].name[0] != 0 ) {
			name_cut(dv[i].name, v[i].name, OB_DROP_NAME);
		} else if ( om_store_name(&v[i].target, name, TAD_NAME_MAX) >= 0 ) {
			name_cut(dv[i].name, name, OB_DROP_NAME);
		}
		dv[i].left = v[i].left;
		dv[i].top = v[i].top;
		dv[i].right = v[i].right;
		dv[i].bottom = v[i].bottom;
		if ( v[i].right <= v[i].left && v[i].width > 0 ) {
			dv[i].right = v[i].left + v[i].width;		/* a link in a text */
			dv[i].bottom = v[i].top + v[i].heightpx;
		}
		dv[i].frcol = (UINT)v[i].frcol;
		dv[i].chcol = (UINT)v[i].chcol;
		dv[i].tbcol = (UINT)v[i].tbcol;
		dv[i].bgcol = (UINT)v[i].bgcol;
		dv[i].chsz = v[i].chsz;
		dv[i].disp = v[i].disp;
	}
	knl_memset(&from, 0, sizeof(from));
	if ( d != NULL ) {
		(void)wm_obj_uuid(d->wid, &from);
	}
	er = wm_obj_drop(wid, sx, sy, dt_up_mods, &from, dv, n);
	Kfree(dv);
	Kfree(name);
	if ( er < E_OK ) {
		(void)wm_msg_put("この窓には置けません");
	}
	return TRUE;			/* over another's window, nothing here moves */
}

LOCAL void desktop_run( void )
{
	T_WMEV	ev;
	INT	i;
	TS_UUID	cab;
	INT	cwid;

	tm_printf((UB *)"TessronOS desktop: start\n");

	if ( face_open() < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: no face on the disk\n");
	}
	{
		T_OBVOL	vol;

		/*
		 * The objects' volume: the store on /boot only when the system
		 * has none attached (the system volume, attached at start).
		 * /boot keeps copies of the system's objects for a machine
		 * without one; attached beside it, every object is there twice.
		 */
		om_store_files(( ob_ref_vol(NULL, &vol) >= E_OK ) ? NULL : "/boot");
	}
	(void)wm_conf_apply();			/* the person's settings (ユーザ環境設定) */
	(void)wm_dev_conf_apply();		/* the machine's (システム環境設定) */
	video_start();				/* the screen's size, before any window */
	(void)wm_conf_watch();			/* the network's behind, and each again when written */
	knl_obsys_power((FP)dt_power);		/* the system object's power record comes here */
	(void)dt_prog_start();			/* the programs, as real objects */
	wm_obj_painter(tv_paint, tv_shape);	/* what a window's drawing and shape records show */
	port_open();
	dt_fault_forget();			/* exceptions before the desktop was up: the console said them */
	(void)kc_start();			/* かな漢字変換: mozc starts behind */
	if ( dt_res_wall(WM_WALL_FIT) < E_OK ) {	/* a picture of the 壁紙箱 */
		tm_printf((UB *)"TessronOS desktop: no wallpaper\n");
	}
	(void)wm_msg_show(TRUE);		/* the message line along the foot */
	tip_start();

	/*
	 * The cabinet to open with. Which one that is belongs in the
	 * system's own settings; until there are settings, the one the
	 * records this system was given open with is named here.
	 */
	if ( ts_str_to_uuid(DT_CABINET, &cab) < E_OK
	  || ( cwid = win_open(&cab, 0, 60, 40) ) < 0 ) {
		tm_printf((UB *)"TessronOS desktop: no cabinet to open\n");
	} else {
		dt_first_wid = cwid;
		auto_open(dt_win_of(cwid));
	}
	wm_composite();
	video_confirm(dt_win_of(dt_first_wid));

	while ( !dt_stop ) {
		DTWIN	*d;
		INT	px = 0, py = 0;
		UINT	btn = 0;

		if ( dt_port <= 0 || dt_req <= 0 ) {
			port_open();		/* one that could not be made at the start */
		}
		close_requests();
		process_requests();
		dt_fault_poll();		/* a process ended by an exception: told in a dialog */
		if ( wm_layout_serial() != dt_layout ) {
			/* the frames were measured again: every window's page with them */
			INT	i;

			dt_layout = wm_layout_serial();
			for ( i = 0; i < DT_MAX_WIN; i++ ) {
				if ( dt_win[i].used ) {
					dt_draw(&dt_win[i]);
				}
			}
			wm_composite();
		}
		if ( wm_read_event(&ev, DT_BLINK_MS / 2) < E_OK ) {
			blink_step();
			continue;
		}
		if ( ev.type == HID_EV_BTN_DOWN || ev.type == HID_EV_KEY_DOWN ) {
			/* what was just done shows at once, and for a whole turn */
			ed_blink_show();
			blink_restart();
			(void)wm_msg_put(NULL);	/* a message stays until the next thing done */
		} else {
			blink_step();
		}
		/* a window of 管理情報 is worked by its own code (dtinfo.c) */
		if ( dt_info_event(&ev) ) {
			continue;
		}
		/* and the dialog of a program ended by an exception (dtfault.c) */
		if ( dt_fault_event(&ev) ) {
			continue;
		}
		d = ( ev.wid != 0 ) ? dt_win_of(ev.wid) : NULL;

		switch ( ev.type ) {
		case HID_EV_MOVE:
			/*
			 * The pointer moved. Whatever is held follows it,
			 * asked for on the screen: the event's own place is
			 * measured from the window the pointer is over,
			 * which need not be the one holding anything.
			 */
			if ( ts_hid_pointer(&px, &py, &btn) < E_OK ) {
				break;
			}
			if ( carry.on ) {
				carry_follow(px, py);
			} else if ( ed_holding() ) {
				ed_follow(px, py);
			} else if ( dd_dragging() ) {
				dd_follow(px, py);
			} else if ( df_drawing_now() ) {
				df_tool_follow(px, py, ev.mods);
			} else if ( df_end_carrying() ) {
				df_end_follow(px, py);
			} else if ( df_transform_carrying() ) {
				df_transform_follow(px, py);
			} else if ( df_canvas_painting() ) {
				df_canvas_follow(px, py);
			} else if ( dd_link_carrying() ) {
				dd_link_follow(px, py);
			} else if ( dd_mark_carrying() ) {
				dd_mark_follow(px, py);
			} else if ( df_panel_carrying() ) {
				df_panel_follow(px, py);
			} else if ( dt_move_wid != 0 ) {
				T_WMWIN	w;

				if ( wm_ref(dt_move_wid, &w) >= E_OK ) {
					T_DPRECT	o = w.outer;

					if ( dt_sizing ) {
						o.right  = px - dt_grab_dx;
						o.bottom = py - dt_grab_dy;
					} else {
						INT	dw = o.right - o.left;
						INT	dh = o.bottom - o.top;

						o.left = px - dt_grab_dx;
						o.top  = py - dt_grab_dy;
						o.right = o.left + dw;
						o.bottom = o.top + dh;
					}
					if ( wm_move(dt_move_wid, &o) >= E_OK ) {
						DTWIN	*m = dt_win_of(dt_move_wid);

						if ( m != NULL && dt_sizing ) {
							dt_draw(m);
						}
						wm_composite();
					}
				}
			} else if ( dt_hold_wid != 0 ) {
				DTWIN	*h = dt_win_of(dt_hold_wid);
				T_WMBAR	b;

				if ( h != NULL ) {
					wm_bar_drag(dt_hold_wid, dt_hold_bar, px, py);
					/* the page follows the knob, unless it is to wait for the let go */
					if ( wm_num(LK_SCR_LIVE, 1) != 0
					  && wm_bar(dt_hold_wid, dt_hold_bar, &b) >= E_OK ) {
						if ( dt_hold_bar == WM_BAR_B ) scroll_x_to(h, b.clo);
						else scroll_to(h, b.clo);
					} else {
						wm_composite();	/* the knob alone */
					}
				} else {
					/* a program's window: it is told where the knob went */
					wm_bar_drag(dt_hold_wid, dt_hold_bar, px, py);
					if ( wm_num(LK_SCR_LIVE, 1) != 0
					  && wm_bar(dt_hold_wid, dt_hold_bar, &b) >= E_OK
					  && b.clo != dt_hold_at ) {
						dt_hold_at = b.clo;
						wm_obj_scroll(dt_hold_wid, dt_hold_bar, b.clo, OB_SCR_KNOB);
					}
					wm_composite();
				}
			}
			if ( d != NULL && !ed_holding() && !df_drawing_now()
			  && !df_end_carrying() && !df_transform_carrying()
			  && !df_canvas_painting() ) {
				df_hover(d, ev.x, ev.y);
			}
			/*
			 * Only the pointer's own two places are laid again.
			 * Building the whole screen for every step of the
			 * hand is what makes a pointer lag behind it.
			 */
			wm_pointer_moved(px, py);
			break;

		case HID_EV_BTN_UP:
			if ( carry.on ) {
				/* a link carried out of a program's window, let go */
				if ( ev.code == 0 && ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
					carry_release(px, py);
				}
				break;
			}
			if ( ev.code != 0 && ( ed_holding() || dd_link_carrying() ) ) {
				/* the second button let go first: the first still carries */
				break;
			}
			dt_up_mods = ev.mods;
			if ( dt_hold_wid != 0 && wm_num(LK_SCR_LIVE, 1) == 0 ) {
				/* the knob let go: the page goes where it was left */
				DTWIN	*h = dt_win_of(dt_hold_wid);
				T_WMBAR	b;

				if ( h != NULL && wm_bar(dt_hold_wid, dt_hold_bar, &b) >= E_OK ) {
					if ( dt_hold_bar == WM_BAR_B ) scroll_x_to(h, b.clo);
					else scroll_to(h, b.clo);
				}
			}
			if ( dt_hold_wid != 0 && dt_win_of(dt_hold_wid) == NULL ) {
				/* a program's knob let go: told once more, where it rests */
				T_WMBAR	b;

				if ( wm_bar(dt_hold_wid, dt_hold_bar, &b) >= E_OK ) {
					wm_obj_scroll(dt_hold_wid, dt_hold_bar, b.clo, OB_SCR_DONE);
				}
			}
			if ( dt_hold_wid != 0 || dt_move_wid != 0 ) {
				wm_set_pointer(WM_PT_SELECT);
				wm_composite();
			}
			dt_hold_wid = 0;
			dt_move_wid = 0;
			dd_release();
			df_tool_release();
			if ( df_end_carrying() ) {
				df_end_release();
				wm_composite();
			}
			if ( df_transform_carrying() ) {
				df_transform_release();
			}
			df_canvas_release();
			dd_mark_release();
			if ( dd_link_carrying()
			  && ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
				dd_link_release(px, py);
			}
			df_panel_let_go();
			if ( ed_holding() ) {
				if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
					ed_release(px, py);
				}
			}
			break;

		case HID_EV_BTN_DOWN:
			if ( carry.on ) {
				break;		/* another button, while a program's link is carried */
			}
			if ( df_panel_press(&ev) ) {
				break;
			}
			if ( d == NULL ) {
				obj_press(&ev);		/* a program's own window */
				break;
			}
			if ( ev.code == 1 && ( ed_holding() || dd_link_carrying() ) ) {
				/* 複写ドラッグ: what is carried goes as a copy */
				ed_hold_copy();
				dd_link_copy();
				break;
			}
			wm_raise(d->wid);
			wm_focus(d->wid);
			if ( ev.code == 1 ) {
				menu_at(d, ev.x, ev.y, ev.when);
			} else {
				press(d, &ev);
			}
			break;

		case HID_EV_KEY_DOWN:
			key(dt_win_of(wm_focused()), &ev);
			break;

		case HID_EV_WHEEL:
			wheel(d, &ev);		/* a program's own window is told by its notice */
			break;

		default:
			break;
		}
	}
	dt_info_close_all();
	dt_fault_close_all();
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		win_close(&dt_win[i]);
	}
	om_store_empty();
	/*
	 * What the session laid on the window system goes with it: the
	 * wallpaper and the message line it put up, and the power record
	 * that asked it to end. The window system is left as the desktop
	 * found it, for whatever uses the screen after it (a desktop
	 * started again lays them again).
	 */
	(void)wm_set_wall(NULL, 0, 0, WM_WALL_TILE);
	(void)wm_msg_show(FALSE);
	knl_obsys_power(NULL);
	wm_composite();
}

LOCAL void desktop_task( INT stacd, void *exinf )
{
	desktop_run();
	port_close();
	dt_tskid = 0;
	tk_exd_tsk();
}

/*
 * The desktop is run in a task of its own, with a stack large enough
 * for the font library.
 *
 * Turning an outline into pixels is deeply recursive -- a face with
 * PostScript outlines runs an interpreter over each letter -- and it
 * wants some tens of kilobytes of stack. Run on a stack meant for a
 * startup routine it goes over the end of it, and what it lands on is
 * whatever the allocator keeps next door; the fault then comes out of
 * the allocator, which had nothing to do with it. That is an hour's
 * search every time, so the room is given here where it is asked for.
 */
EXPORT ER dt_start( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	if ( dt_tskid > 0 ) {
		return E_OBJ;
	}
	dt_stop = FALSE;
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = desktop_task;
	ctsk.itskpri = 20;
	ctsk.stksz   = 65536;

	tskid = tk_cre_tsk(&ctsk);
	if ( tskid < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: no task (%d)\n", (INT)tskid);
		return tskid;
	}
	dt_tskid = tskid;
	if ( tk_sta_tsk(tskid, 0) < E_OK ) {
		dt_tskid = 0;
		tk_del_tsk(tskid);
		return E_SYS;
	}
	return E_OK;
}

/* The loop let go of, its windows closed, and waited for until it is gone */
EXPORT void dt_quit( void )
{
	INT	i;

	dt_stop = TRUE;
	for ( i = 0; i < 100 && dt_tskid > 0; i++ ) {
		tk_dly_tsk(100);
	}
}

EXPORT INT ts_desktop( void )
{
	UINT		sw, sh;

	/*
	 * Without a screen there is no desktop to run, but the machine is
	 * still of use over its serial line and its network: it is kept
	 * up rather than let return, which would shut it down.
	 */
	if ( dt_screen_size(&sw, &sh) < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: no screen; not started (serial only)\n");
		tk_slp_tsk(TMO_FEVR);
		return 0;
	}
	if ( dt_start() < E_OK ) {
		return 1;
	}
	dt_uitest_start();		/* nothing unless built with UITEST=1 */
	/*
	 * The desktop does not end. This waits rather than returning,
	 * because returning from usermain shuts the machine down.
	 */
	while ( !dt_stop ) {
		tk_dly_tsk(1000);
	}
	/*
	 * Turned off from the desktop: its windows are saved and closed by
	 * its own task on the way out, which is waited for, and what the
	 * file systems still hold is written before usermain returns and the
	 * machine powers off.
	 */
	if ( dt_poweroff ) {
		INT	i;

		for ( i = 0; i < 100 && dt_tskid > 0; i++ ) {
			tk_dly_tsk(100);
		}
		(void)fs_sync();
		tm_printf((UB *)( dt_restart ? "TessronOS desktop: starting the machine again\n"
					     : "TessronOS desktop: turning the machine off\n" ));
	}

	return dt_restart ? -1 : 0;		/* usermain's: -1 resets and starts again */
}
