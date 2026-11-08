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
#include <ts/proc.h>
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
#include <ts/time.h>
#include <ts/rtmedia.h>
#include <ts/img.h>
#include <ts/sysdef.h>
#include <ts/so.h>
#include <ts/lockup.h>
#include <ts/fquit.h>
#include <ts/tray.h>
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

LOCAL BOOL	dt_quit_asked = FALSE;	/* dt_quit waits for the session to end */

EXPORT BOOL dt_stopping( void )
{
	return dt_quit_asked;
}

/*
 * The system object's power record written (obsys.c, システム環境設定):
 * the session ends as closing the first window ends it, the machine then
 * going off (how 0) or starting again (how 1).
 */
LOCAL UW	dt_stop_at = 0;		/* when the power record asked the session to end, ms */

LOCAL ER dt_power( INT how )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	dt_stop_at = (UW)t.lo;
	dt_poweroff = TRUE;
	dt_restart = (BOOL)( how == 1 );
	dt_stop = TRUE;
	return E_OK;
}
#if defined(USE_TESTAGENT) || defined(USE_UITEST)
/* The test agent's reboot and poweroff (dttagent.c), and the steps' (dtuitest.c): the same way out */
EXPORT ER dt_tagent_power( INT how )
{
	return dt_power(how);
}
#endif

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
/*
 * With LK_LIVE at 0 the window stays where it is while it is carried and
 * an outline follows the hand instead; the window goes where the outline
 * is when the button is let go. Every window the desktop carries --
 * its own (those with a part of their own among them), a program's, the
 * dialog of a program ended --
 * goes through these two.
 */
LOCAL BOOL	dt_outline = FALSE;
LOCAL T_DPRECT	dt_outline_to;

/* A window carried to o: TRUE when it moved there now, FALSE when only its outline did */
EXPORT BOOL dt_carry_window( INT wid, CONST T_DPRECT *o )
{
	T_WMWIN		w;
	T_DPRECT	r = *o;
	INT		wide, high;

	if ( wm_num(LK_LIVE, 1) != 0 ) {
		return (BOOL)( wm_move(wid, o) >= E_OK );
	}
	/* no smaller than its frame, as wm_move would have it */
	if ( wm_ref(wid, &w) >= E_OK && wm_least(w.attr, &wide, &high) >= E_OK ) {
		if ( r.right - r.left < wide ) r.right = r.left + wide;
		if ( r.bottom - r.top < high ) r.bottom = r.top + high;
	}
	dt_outline = TRUE;
	dt_outline_to = r;
	(void)wm_set_drag(&r, 1);
	return FALSE;
}

/*
 * Links and shapes carried show as themselves when windows are moved as
 * they go (LK_LIVE), and as outlines otherwise: n outlines r, each dx, dy
 * away from where its thing lies on the screen, have the looks of those
 * places. Called once, when the outlines are first laid.
 */
EXPORT void dt_carry_looks( CONST T_DPRECT *r, INT n, INT dx, INT dy )
{
	T_DPRECT	from[WM_DRAG_MAX];
	INT		i;

	if ( wm_num(LK_LIVE, 1) == 0 || n <= 0 ) {
		return;
	}
	if ( n > WM_DRAG_MAX ) {
		n = WM_DRAG_MAX;
	}
	for ( i = 0; i < n; i++ ) {
		from[i] = r[i];
		from[i].left -= dx;  from[i].right -= dx;
		from[i].top -= dy;   from[i].bottom -= dy;
	}
	(void)wm_set_drag_from(from, n);
}

/* The button let go: TRUE when the window went where its outline was */
EXPORT BOOL dt_carry_end( INT wid )
{
	if ( !dt_outline ) {
		return FALSE;
	}
	dt_outline = FALSE;
	(void)wm_set_drag(NULL, 0);
	return (BOOL)( wm_move(wid, &dt_outline_to) >= E_OK );
}

LOCAL UINT	dt_up_mods = 0;	/* the keys held when the button was let go */

/*
 * The first button let go, as the loop read it: where, and whether since
 * the last press. A carry a program asks for once the button is up is
 * let go there (carry_start).
 */
LOCAL BOOL	dt_up_seen = FALSE;
LOCAL INT	dt_up_x = 0, dt_up_y = 0;
LOCAL ID	dt_tskid = 0;	/* the desktop's task, while it runs */

/*
 * The start of the desktop watched from the task that waits for it
 * (ts_desktop): each step before the loop is said as it begins, and
 * the loop counted as it comes round. What stands too long is told
 * with where the task waits (lockup.c), long before the watch of the
 * loop would (LK_SILENT_S): a step not done, the screen held so that
 * nothing drawn reaches it.
 */
IMPORT void knl_lockup_tell_task( ID tskid, CONST char *why );	/* lockup.c */
IMPORT INT  knl_disp_hold_note( ID *p_tsk, UW *p_ms );		/* disp.c */
IMPORT ID   knl_wm_compositor( void );				/* wm.c */
IMPORT void knl_bootlog_soon( void ) __attribute__((weak));	/* bootlog.c (x64): the records written soon */

#define DT_STEP_STUCK_MS	5000	/* a step before the loop this long: told */
#define DT_HELD_STUCK_MS	3000	/* the screen held this long: told */
#define DT_WATCH_SAY_MS		30000	/* the state said every DT_WATCH_EVERY_MS up to this */
#define DT_WATCH_EVERY_MS	5000

LOCAL CONST char * volatile	dt_step = "";	/* the step of the start begun last */
LOCAL volatile UW	dt_step_at = 0;		/* when, ms */
LOCAL volatile UW	dt_loops = 0;		/* times the loop came round */
#if USE_KTEST
EXPORT volatile INT	dt_test_stall = 0;	/* the tests: the loop stopped (desktop.h) */
#endif
#if USE_KTEST || defined(USE_UITEST)
/* The tests: how many times the loop came round, which starts only when the start's steps are over */
EXPORT UW dt_test_loops( void )
{
	return dt_loops;
}
#endif

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
		if ( d->doc != NULL ) {
			d->doc->zoom = z;
			d->doc->zoom_den = 8;
		}
		return;
	}
	/*
	 * The page reaches the frame: what is drawn runs up to its inner
	 * edge on every side, with no strip left undrawn inside it. Lines
	 * that wrap stop where they wrap; what does not wrap is cut at the
	 * frame.
	 */
	r->left = 0;  r->top = 0;
	r->right = 600;  r->bottom = 400;
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		r->right  = w.work.right - w.work.left;
		r->bottom = w.work.bottom - w.work.top;
	}
	/*
	 * A document not wrapped to the window is wrapped to its paper:
	 * the paper's whole width when the paper is shown, its margins and
	 * all, and the width inside the margins when it is not.
	 */
	if ( d->doc != NULL && !d->xml_view && d->doc->vert
	  && ( d->paper_frame || d->nowrap ) ) {
		/*
		 * 縦書き: its lines run down the page, so the paper decides how
		 * tall the page is, the whole of it when the paper is shown
		 * and what is inside the top and bottom margins when it is not.
		 */
		INT	ph = ( d->doc->paper_h > 0 ) ? d->doc->paper_h : DT_PAPER_H;

		if ( !d->paper_frame ) {
			ph -= d->doc->margin[1] + d->doc->margin[3];
		}
		if ( ph < 64 ) {
			ph = 64;
		}
		r->bottom = r->top + ph;
	} else if ( d->doc != NULL && !d->xml_view && ( d->paper_frame || d->nowrap ) ) {
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
	if ( d->doc->vert ) {
		/*
		 * 縦書き: the pages stand side by side from the right, each
		 * as wide as the paper, and the desk is below them and to the
		 * left of the last.
		 */
		desk.left = 0;
		desk.top = r->bottom + 1;
		desk.right = w.work.right - w.work.left;
		desk.bottom = w.work.bottom - w.work.top;
		if ( desk.bottom > desk.top ) {
			dp_fill_rect(gid, &desk, 0x00A0A0A0U);
		}
		pages = ( d->height + ph - 1 ) / ph;
		if ( pages < 1 ) {
			pages = 1;
		}
		desk.top = r->top;
		desk.bottom = r->bottom + 1;
		desk.left = 0;
		desk.right = r->right - pages * ph + d->scroll_y;
		if ( desk.right > desk.left ) {
			dp_fill_rect(gid, &desk, 0x00A0A0A0U);
		}
		for ( k = 0; k < pages; k++ ) {
			pg.right = r->right - k * ph + d->scroll_y;
			pg.left = pg.right - ph - 1;
			pg.top = r->top;
			pg.bottom = r->bottom + 1;
			if ( pg.right < 0 || pg.left > r->right ) {
				continue;
			}
			dp_frame_rect(gid, &pg, 0x00606060U, 1);
		}
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
#ifdef USE_UITEST
/* the tests: when the press being answered in a figure editor happened (ns), 0 none */
LOCAL UD	dt_fig_input_at = 0;

/* the tests: a window of links of the desktop's own, which 仮身一覧 as a process is measured against */
LOCAL BOOL ut_cabinet( CONST DTWIN *d )
{
	return (BOOL)( d->kind == TV_KIND_FIG && !d->figed && !d->sealed
		    && d->tool == DT_TOOL_NONE && d->tb_host == NULL );
}

/*
 * The tests: what the moves of one drag cost, from the first move with
 * something held to the let go. 'got' the moves read, 'put' the moves
 * the input layer was given meanwhile (the ring merges a move into the
 * one still waiting), 'busy' the time one move took to follow and show,
 * 'lag' from the move to the end of that.
 */
LOCAL struct {
	BOOL	on;
	INT	got;
	UD	put0, busy, busy_max, lag, lag_max, lag_last;
} dt_drag;
LOCAL UD	drag_t0;		/* when the move being followed was taken */
LOCAL BOOL	drag_timed;		/* it moves something held */

LOCAL void drag_moved( UD t0, UD when )
{
	UD		t1 = 0, b, l;
	T_HIDSTAT	st;

	(void)ts_get_mono(&t1);
	if ( !dt_drag.on ) {
		knl_memset(&dt_drag, 0, sizeof(dt_drag));
		dt_drag.on = TRUE;
		if ( ts_hid_stat(&st) >= E_OK ) dt_drag.put0 = st.events - 1;
	}
	b = t1 - t0;
	l = ( when != 0 && t1 > when ) ? t1 - when : b;
	dt_drag.got++;
	dt_drag.busy += b;
	dt_drag.lag += l;
	dt_drag.lag_last = l;
	if ( b > dt_drag.busy_max ) dt_drag.busy_max = b;
	if ( l > dt_drag.lag_max ) dt_drag.lag_max = l;
}

LOCAL void drag_said( void )
{
	T_HIDSTAT	st;
	INT		put = 0, n = dt_drag.got;

	if ( !dt_drag.on || n <= 0 ) {
		return;
	}
	/* the let go is the one event put after the last move */
	if ( ts_hid_stat(&st) >= E_OK ) put = (INT)( st.events - dt_drag.put0 ) - 1;
	tm_printf((UB *)"desktop: drag moves %d of %d follow avg %d max %d us lag avg %d max %d last %d us\n",
		  n, put, (INT)( dt_drag.busy / (UD)n / 1000 ), (INT)( dt_drag.busy_max / 1000 ),
		  (INT)( dt_drag.lag / (UD)n / 1000 ), (INT)( dt_drag.lag_max / 1000 ),
		  (INT)( dt_drag.lag_last / 1000 ));
	dt_drag.on = FALSE;
}
#endif

EXPORT void dt_draw( DTWIN *d )
{
	T_DPRECT	r;
	INT		gid = wm_gid(d->wid);
	T_WMBAR		b;

	if ( d->own != NULL ) {
		if ( gid >= 0 ) {
			d->own->draw(d);	/* the part's own work area */
		}
		return;
	}
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
		if ( !d->sealed && d->tool == DT_TOOL_NONE ) {
			tv_draw_host(&d->id);
		}
		d->height = tv_doc_draw(gid, d->doc, &r, d->scroll_y, d->rec,
					d->paper);
		if ( d->doc->layc != NULL ) {
			d->settling = TRUE;	/* what was not laid at this width, laid while idle */
		}
		tv_draw_host(NULL);
		if ( d->paper_frame && !d->xml_view ) {
			paper_frame(d, gid, &r);
		}
	} else if ( d->fig != NULL ) {
		/* the links in it to objects of other volumes are marked */
		if ( !d->sealed && d->tool == DT_TOOL_NONE ) {
			tv_draw_host(&d->id);
		}
#ifdef USE_UITEST
		/* the tests compare it with the figure editor running as a process (ui_fig_proc_cmp) */
		UD	t0 = 0, t1 = 0;

		(void)ts_get_mono(&t0);
#endif
		tv_fig_draw(gid, d->fig, &r, d->scroll_x, d->scroll_y, d->rec,
			    d->paper);
#ifdef USE_UITEST
		(void)ts_get_mono(&t1);
		if ( d->figed ) {
			tm_printf((UB *)"desktop: figure drew in %d us\n", (INT)( ( t1 - t0 ) / 1000 ));
		} else if ( ut_cabinet(d) ) {
			tm_printf((UB *)"desktop: cabinet drew in %d us\n", (INT)( ( t1 - t0 ) / 1000 ));
		}
#endif
		tv_draw_host(NULL);
		tv_fig_size(d->fig, &d->width, &d->height);
		if ( d->fig->zoom > 0 ) {
			d->height = d->height * d->fig->zoom / 8;
			d->width = d->width * d->fig->zoom / 8;
		}
		df_overlay(d, gid, &r);
	}
	ed_draw_picks(d);
#ifdef USE_UITEST
	if ( ( d->figed || ut_cabinet(d) ) && dt_fig_input_at != 0 ) {
		UD	t = 0;

		(void)ts_get_mono(&t);
		tm_printf((UB *)"desktop: %s input to screen %d us\n", d->figed ? "figure" : "cabinet",
			  (INT)( ( t - dt_fig_input_at ) / 1000 ));
		dt_fig_input_at = 0;
	}
#endif
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
	if ( d->fig != NULL ) {
		df_drawing_over(d);		/* a shape being drawn, inverted over all of it */
	}
	if ( d->doc != NULL && d->doc->vert && !d->xml_view ) {
		/*
		 * 縦書き: the text runs on to the left, so the bar across is
		 * the one that winds it, its knob at the right end at the head
		 * of the text. Where the knob is, is kept in scroll_x, so that
		 * what moves the knob moves the text (scroll_x_to).
		 */
		INT	view = r.right - r.left, room;

		b.lo = 0;
		b.hi = ( r.bottom - r.top > 0 ) ? r.bottom - r.top : 1;
		b.clo = 0;
		b.chi = b.hi;
		wm_set_bar(d->wid, WM_BAR_R, &b);
		b.hi = ( d->height > view ) ? d->height : view;
		if ( b.hi <= 0 ) {
			b.hi = 1;
		}
		room = b.hi - view;
		d->scroll_x = room - d->scroll_y;
		if ( d->scroll_x < 0 ) d->scroll_x = 0;
		b.clo = d->scroll_x;
		b.chi = d->scroll_x + view;
		if ( b.chi > b.hi ) {
			b.chi = b.hi;
		}
		wm_set_bar(d->wid, WM_BAR_B, &b);
		return;
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
#define DT_PRE_IDLE_MS	300		/* nothing pressed so long: idle work begins */

LOCAL UW	dt_last_input = 0;		/* when a key or a button was last pressed */

LOCAL UW	blink_at;		/* when the turn last changed, in ms */
LOCAL UW	caret_at;		/* and the caret's */

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);

	return (UW)t.lo;
}

LOCAL void caret_draw( DTWIN *f );

LOCAL void blink_restart( void )
{
	blink_at = now_ms();
	caret_at = blink_at;
	if ( dd_caret_hidden ) {
		DTWIN	*f = dt_win_of(wm_focused());

		dd_caret_hidden = FALSE;
		/* the caret shown again: only it drawn */
		if ( f != NULL && dd_editable(f) ) caret_draw(f);
	}
}

/*
 * The window drawn again inside the caret alone, when it has one: the
 * environment's visible part narrowed to it while the page is drawn, so
 * that what goes to the screen is the caret and not the window.
 */
LOCAL void caret_draw( DTWIN *f )
{
	T_DPRECT	r, all;
	T_WMWIN		w;
	INT		gid = wm_gid(f->wid);

	if ( gid < 0 || !dd_caret_rect(f, &r) || wm_ref(f->wid, &w) < E_OK ) {
		dt_draw(f);
		return;
	}
	all.left = 0;  all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	(void)dp_set_visible(gid, &r);
	dt_draw(f);
	(void)dp_set_visible(gid, &all);
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
				caret_draw(f);
				wm_update();
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
	caret_draw(f);
	wm_update();
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
	wm_update();
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
	return dt_list_record_dim(title, ids, names, NULL, n);
}

/* The same, those 'dim' says with their frame and name in grey */
EXPORT T_TAD *dt_list_record_dim( CONST char *title, CONST TS_UUID *ids,
				  CONST char *CONST *names, CONST BOOL *dim, INT n )
{
	UB		*xml;
	SZ		max, at = 0;
	T_TAD		*doc = NULL;
	INT		i;
	INT		y = BASE_PAD + BASE_GAP, band = tad_closed_h(BASE_CHSZ, TAD_D_DEFAULT);
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
		w = tad_closed_w(BASE_CHSZ, TAD_D_DEFAULT, w);	/* the pictogram and the edges */

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
		at = put_s(xml, at, max, ( dim != NULL && dim[i] )
			   ? "\" frcol=\"#a0a0a0\" chcol=\"#808080\"" : "\" frcol=\"#000000\" chcol=\"#000000\"");
		at = put_s(xml, at, max,
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
 * The program that opens the record v points at, of those that look at
 * its kind: 仮身一覧 shows a figure's links, and the desktop's own window
 * shows a record that is a text with the text editor whichever of them
 * opened it. 仮身一覧 as a process opens nothing but a figure, so a text
 * is given to the program of texts instead, as the desktop's own does.
 */
LOCAL CONST DTPROG *prog_for_kind( CONST DTPROG *p, CONST T_VOBJ *v )
{
	CONST DTPROG	*q;
	T_TAD		*rec;

	if ( p == NULL || !app_is(p->id, "virtual-object-list") ) {
		return p;
	}
	rec = om_store_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);
	if ( rec == NULL || tv_kind(rec) != TV_KIND_DOC ) {
		return p;
	}
	q = dt_prog_find((CONST UB *)"basic-text-editor");

	return ( q != NULL ) ? q : p;
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
				wm_update();
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
	INT	first;			/* the count of links before the first put, -1 none put yet */
	UINT	seq;
	INT	wid;			/* the window it was let go over */
	TS_UUID	into;			/* and that window's object */
	INT	sx, sy;			/* the outline's corner there, on the screen */
	INT	x0, y0;			/* where the hand was when the carry began */
	BOOL	copy;			/* the second button went down: 複写ドラッグ */
	SZ	anssz;			/* how much of an answer the program reads */
	ID	asker;			/* the process carrying: only it ends the carry */
	BOOL	toprog;			/* let go over another program's window */
	T_OBDROPV *links;		/* and the links asked for so far, dropped with the last */
	INT	nlink;
} carry;

LOCAL void name_cut( UB *out, CONST UB *s, INT max );

/*
 * The first form of a request and its answer, before a carried link
 * was asked for: a program written for it asks and reads in that size,
 * and is answered in it. The second form, before the asking window and
 * the flags, likewise.
 */
#define DTREQ_OPEN_SIZE	( (SZ)( (UB *)&((T_DTREQ *)0)->look - (UB *)0 ) )
#define DTANS_OPEN_SIZE	( (SZ)( (UB *)&((T_DTANS *)0)->into - (UB *)0 ) )
#define DTREQ_V2_SIZE	( (SZ)( (UB *)&((T_DTREQ *)0)->win - (UB *)0 ) )
#define DTANS_V2_SIZE	( (SZ)( (UB *)&((T_DTANS *)0)->look - (UB *)0 ) )
/* The third, before the letters carried: answered in the size of now */
#define DTREQ_V3_SIZE	( (SZ)( (UB *)&((T_DTREQ *)0)->text - (UB *)0 ) )

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


EXPORT void dt_answer( CONST TS_UUID *reply, CONST T_DTANS *an, SZ size )
{
	answer_as(reply, an, size);
}

LOCAL ER carry_place( CONST T_DTREQ *rq, BOOL more, TS_UUID *p_into, ID asker );

LOCAL void carry_answer( ER er, CONST TS_UUID *into )
{
	T_DTANS	an;
	TS_UUID	made;

	if ( er >= E_OK && ( carry.rq.flags & DT_F_BASE ) != 0 && carry.pending ) {
		/* a template let go where a link goes: the new object made from it, and its link put (DT_F_BASE) */
		static T_DTREQ	r;

		r = carry.rq;
		r.flags = DT_F_DUPLICATE;
		r.carry = carry.seq;
		r.grab_x = r.grab_y = 0;
		r.name[0] = 0;
		knl_memset(&made, 0, sizeof(made));
		er = carry_place(&r, FALSE, &made, carry.asker);
		into = ( er >= E_OK ) ? &made : NULL;
		tm_printf((UB *)"desktop: template carried, made from it: %d\n", (INT)er);
	}
	knl_memset(&an, 0, sizeof(an));
	an.seq = carry.rq.seq;
	an.er = er;
	if ( into != NULL ) {
		an.into = *into;
	}
	an.how = carry.copy ? DT_HOW_COPY : 0;
	an.flags = ( er >= E_OK && carry.toprog ) ? DT_F_TO_PROG : 0;
	answer_as(&carry.rq.reply, &an, carry.anssz);
}

/* Let go back over the asking program's own window, at px, py: its own to do there (DT_F_BACK) */
LOCAL void carry_back( INT px, INT py )
{
	T_DTANS	an;

	knl_memset(&an, 0, sizeof(an));
	an.seq = carry.rq.seq;
	an.er = E_NOEXS;
	an.how = carry.copy ? DT_HOW_COPY : 0;
	an.flags = DT_F_BACK;
	an.look.w = px;
	an.look.h = py;
	answer_as(&carry.rq.reply, &an, carry.anssz);
}

/*
 * What a text carried out (DT_F_FRAG), from the tray's place of its own;
 * NULL: none there
 */
LOCAL T_TAD *carry_frag( void )
{
	T_TRINF	inf;
	T_TAD	*frag = NULL;
	UB	*buf;
	SZ	asz = 0;

	if ( tr_ref_rec(TR_PUT, 0, &inf) < E_OK || inf.kind != TR_TAD || inf.size <= 0 ) {
		return NULL;
	}
	buf = (UB *)Kmalloc((SZ)inf.size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tr_rea_rec(TR_PUT, 0, buf, (SZ)inf.size, &asz) >= E_OK
	  && tad_parse(buf, (SZ)asz, NULL, &frag) < E_OK ) {
		frag = NULL;
	}
	Kfree(buf);
	return frag;
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
 * A program asks its link to be carried while the hand is down, or as
 * the hand carrying went out of its window -- which the button may have
 * been let go after, before the request comes. Then it is let go where
 * the button went up: at once when the loop has read that, else when it
 * does. A press that was a click is let go where it was taken, and
 * answered that nothing was carried (carry_release).
 */
LOCAL void carry_links_free( void )
{
	if ( carry.links != NULL ) {
		Kfree(carry.links);
		carry.links = NULL;
	}
	carry.nlink = 0;
}

/* A carry let go over a program's window whose program is being asked (DT_F_ASK) */
#define CARRY_ASK_MS	3000		/* how long an answer is waited for */

LOCAL struct {
	BOOL	on;
	INT	wid;
	INT	px, py;
	UINT	seq;
	UW	since;
} carry_asking;

LOCAL void carry_release( INT px, INT py );

LOCAL void carry_start( CONST T_DTREQ *rq, SZ anssz, ID asker )
{
	INT	px = 0, py = 0, x0 = 0, y0 = 0;
	UINT	btn = 0;
	BOOL	up;

	carry.rq = *rq;
	carry.anssz = anssz;
	carry.copy = FALSE;
	carry.rq.ask[0][sizeof(rq->ask[0]) - 1] = 0;
	carry.rq.ask[1][sizeof(rq->ask[1]) - 1] = 0;
	carry.rq.yes[sizeof(rq->yes) - 1] = 0;
	if ( carry.on || carry_asking.on || rq->look.w <= 0 || rq->look.h <= 0
	  || ts_hid_pointer(&px, &py, &btn) < E_OK
	  || ts_hid_press_at(&x0, &y0) < E_OK ) {
		/* why nothing is carried, for the console: a click is the usual one */
		tm_printf((UB *)"desktop: carry refused: %s, outline %dx%d, buttons 0x%x\n",
			  carry.on ? "one carried already" : "", (INT)rq->look.w, (INT)rq->look.h, btn);
		carry_answer(E_OBJ, NULL);
		return;
	}
	carry.on = TRUE;
	carry.pending = FALSE;
	carry.asker = asker;
	carry.toprog = FALSE;
	carry_links_free();
	carry.x0 = x0;
	carry.y0 = y0;
	up = (BOOL)( ( btn & 1 ) == 0 );
	if ( up ) {
		tm_printf((UB *)"desktop: carry asked with the button %s\n",
			  dt_up_seen ? "let go already: let go there" : "going up");
	}
	if ( up && dt_up_seen ) {
		carry_release(dt_up_x, dt_up_y);
		return;
	}
	/* letters have no picture of their own on the screen: they are carried as an outline */
	if ( ( carry.rq.flags & DT_F_TEXT ) == 0 ) {
		/*
		 * The link where it lies in the program's window: grab_x, grab_y
		 * are from the press, and the hand has moved on by the time the
		 * program has heard of it and asked.
		 */
		T_DPRECT	r;

		r.left = x0 - carry.rq.grab_x;
		r.top = y0 - carry.rq.grab_y;
		r.right = r.left + carry.rq.look.w;
		r.bottom = r.top + carry.rq.look.h;
		dt_carry_looks(&r, 1, 0, 0);
	}
	carry_outline(px, py);
	wm_update();
}

LOCAL void carry_follow( INT px, INT py )
{
	carry_outline(px, py);
}

/* Why links do not go to another program's window, on the message line */
LOCAL void carry_said_no( ER er )
{
	(void)wm_msg_put(( er == E_OACV ) ? "この実身を読む権利がないプログラムには渡せません"
					  : "ここには置けません");
}

/*
 * Let go over another program's window: told what is carried before the
 * program goes on, so that the keys do not move for a drop that cannot be
 * made. The links are dropped when the program asks them placed. into,
 * when given, is the object the window shows, for a program that makes
 * the objects there (DT_F_ASK). asked: the program said beforehand that
 * it takes them there (carry_ask), so the carry stays a move unless the
 * second button made it a copy; not asked, it may yet refuse them, and
 * the asking program keeps its links (a copy).
 */
LOCAL void carry_to_prog( INT wid, INT px, INT py, CONST TS_UUID *into, BOOL asked )
{
	TS_UUID	none, win;
	ER	er;

	knl_memset(&none, 0, sizeof(none));
	er = wm_obj_uuid(wid, &win);
	if ( er >= E_OK ) {
		er = wm_obj_drop_may(wid, carry.asker,
				     ( ts_uuid_cmp(&carry.rq.target, &none) != 0 ) ? &carry.rq.target : NULL);
	}
	if ( er < E_OK ) {
		carry_said_no(er);
		carry_answer(( er == E_OACV ) ? E_OACV : E_NOEXS, NULL);
		return;
	}
	if ( carry.rq.ask[0][0] != 0 ) {
		DTWIN	*host = dt_host_of(&win);
		BOOL	yes;

		wm_obj_quiet(TRUE);	/* the keys that answer are the panel's, not the program's */
		yes = (BOOL)( host != NULL
			      && dt_confirm(host, (CONST char *)carry.rq.ask[0],
					    ( carry.rq.ask[1][0] != 0 ) ? (CONST char *)carry.rq.ask[1] : NULL,
					    "取消", ( carry.rq.yes[0] != 0 ) ? (CONST char *)carry.rq.yes : "実行") );
		wm_obj_quiet(FALSE);
		if ( !yes ) {
			wm_update();
			carry_answer(E_ABORT, NULL);
			return;
		}
	}
	carry.pending = TRUE;
	carry.toprog = TRUE;
	carry.seq = carry.rq.seq;
	carry.wid = wid;
	carry.into = win;
	/* the outline's corner, as over a window of the desktop's: each link put at its place in it */
	carry.sx = px - carry.rq.grab_x;
	carry.sy = py - carry.rq.grab_y;
	if ( !asked ) {
		carry.copy = TRUE;	/* it may yet refuse: the link stays where it was */
	}
	carry_answer(E_OK, into);
}

/*
 * Letters (DT_F_TEXT) let go over another program's window that takes
 * letters (OB_E_TEXT): dropped on it as one line where the outline's
 * corner is. asked: it said beforehand that it takes them there, and the
 * carry stays a move unless the second button made it a copy; not asked,
 * it may yet refuse them, and the asking program keeps its letters.
 */
LOCAL void carry_text_to_prog( INT wid, INT px, INT py, BOOL asked )
{
	INT	n;
	ER	er;

	carry.rq.text[DT_CARRY_TEXT - 1] = 0;
	for ( n = 0; carry.rq.text[n] != 0; n++ ) ;
	er = wm_obj_drop_text(wid, px - carry.rq.grab_x, py - carry.rq.grab_y, dt_up_mods,
			      &carry.rq.win, carry.rq.text, n);
	if ( er == E_NOSPT ) {
		er = E_NOEXS;
	}
	carry.toprog = (BOOL)( er >= E_OK );
	if ( !asked ) {
		carry.copy = TRUE;	/* it may yet refuse: the letters stay where they were */
	}
	if ( er < E_OK ) {
		(void)wm_msg_put("ここには置けません");
	}
	wm_update();
	carry_answer(( er < E_OK ) ? E_NOEXS : E_OK, NULL);
}

/*
 * The program over whose window what is carried was let go is asked
 * first whether it would take it there, and the carry answered when it
 * has said (carry_ask_poll): with DT_F_ASK, as the objects are only made
 * then; for a move of links or letters, so that the asking program takes
 * them away only once they are taken, as a window of the desktop's does
 * (dt_drop_move). With DT_F_ASK, one that does not take the question is
 * not asked and refuses; a move to one is offered them and they stay (a
 * copy).
 */
LOCAL void carry_ask( INT wid, INT px, INT py )
{
	UINT	seq = 0;
	BOOL	letters = (BOOL)( ( carry.rq.flags & DT_F_TEXT ) != 0 );
	ER	er = letters ? wm_obj_ask_text(wid, px, py, dt_up_mods, &seq)
			     : wm_obj_ask(wid, px, py, dt_up_mods, &seq);

	tm_printf((UB *)"desktop: carry asks window %d at %d,%d%s: %d\n", wid, px, py,
		  letters ? " of letters" : "", (INT)er);
	if ( er == E_NOSPT && ( carry.rq.flags & DT_F_ASK ) == 0 ) {
		if ( letters ) {
			carry_text_to_prog(wid, px, py, FALSE);
		} else {
			carry_to_prog(wid, px, py, NULL, FALSE);
		}
		return;
	}
	if ( er < E_OK ) {
		carry_said_no(E_NOEXS);
		carry_answer(E_NOEXS, NULL);
		return;
	}
	carry_asking.on = TRUE;
	carry_asking.wid = wid;
	carry_asking.px = px;
	carry_asking.py = py;
	carry_asking.seq = seq;
	carry_asking.since = now_ms();
}

LOCAL void carry_ask_poll( void )
{
	TS_UUID	shown;
	ER	er;

	if ( !carry_asking.on ) {
		return;
	}
	er = wm_obj_asked(carry_asking.wid, carry_asking.seq);
	if ( er == E_TMOUT && (UW)( now_ms() - carry_asking.since ) < CARRY_ASK_MS ) {
		return;
	}
	carry_asking.on = FALSE;
	tm_printf((UB *)"desktop: carry asked window %d: %d\n", carry_asking.wid, (INT)er);
	if ( er < E_OK || !wm_obj_shown(carry_asking.wid, &shown) ) {
		carry_said_no(E_NOEXS);
		carry_answer(E_NOEXS, NULL);
		return;
	}
	if ( ( carry.rq.flags & DT_F_TEXT ) != 0 ) {
		carry_text_to_prog(carry_asking.wid, carry_asking.px, carry_asking.py, TRUE);
		return;
	}
	carry_to_prog(carry_asking.wid, carry_asking.px, carry_asking.py,
		      ( ( carry.rq.flags & DT_F_ASK ) != 0 ) ? &shown : NULL, TRUE);
}

/* Let go: over a window that takes links, and when the question asked is answered yes */
LOCAL void carry_release( INT px, INT py )
{
	DTWIN	*t;

	carry.on = FALSE;
	(void)wm_set_drag(NULL, 0);
	wm_update();
	if ( px - carry.x0 < 4 && carry.x0 - px < 4 && py - carry.y0 < 4 && carry.y0 - py < 4 ) {
		carry_answer(E_OBJ, NULL);	/* let go where it was taken: a click, nothing carried */
		return;
	}
	t = dt_win_of(wm_at(px, py));
	if ( t == NULL && wm_at(px, py) > 0 && wm_at(px, py) == wm_obj_wid(&carry.rq.win) ) {
		carry_back(px, py);		/* back over the program's own window: its own to do */
		return;
	}
	if ( ( carry.rq.flags & ( DT_F_FRAG | DT_F_TEXT ) ) == DT_F_FRAG ) {
		/* a picture or a figure out of a text: into a text or a figure of the desktop's */
		T_TAD	*frag = carry_frag();
		ER	er = E_NOEXS;

		if ( frag != NULL && t != NULL && dd_editable(t) && !t->xml_view ) {
			er = dd_frag_drop(t, px, py, frag);
		} else if ( frag != NULL && t != NULL && t->fig != NULL ) {
			er = dd_frag_into_fig(t, px, py, frag, carry.rq.look.w, carry.rq.look.h,
					      carry.rq.grab_x, carry.rq.grab_y);
		}
		if ( frag != NULL ) tad_free(frag);
		if ( er < E_OK ) {
			(void)wm_msg_put("ここには置けません");
		} else {
			dt_drop_keys(t);
		}
		wm_update();
		carry_answer(( er < E_OK ) ? E_NOEXS : E_OK, ( er < E_OK ) ? NULL : &t->id);
		return;
	}
	if ( ( carry.rq.flags & DT_F_TEXT ) != 0 ) {
		/* letters: into a text where the outline's corner is, or onto a program's window */
		INT	n, cx = px - carry.rq.grab_x, cy = py - carry.rq.grab_y;
		ER	er = E_NOEXS;

		carry.rq.text[DT_CARRY_TEXT - 1] = 0;
		for ( n = 0; carry.rq.text[n] != 0; n++ ) ;
		if ( n > 0 && t != NULL && dd_editable(t) && !t->xml_view
		  && ( carry.rq.flags & DT_F_FRAG ) != 0 ) {
			/* with their look, as from a text of the desktop's; else as the line */
			T_TAD	*frag = carry_frag();

			er = ( frag != NULL ) ? dd_frag_drop(t, cx, cy, frag) : E_NOEXS;
			if ( frag != NULL ) tad_free(frag);
			if ( er < E_OK ) {
				er = dd_text_drop(t, cx, cy, carry.rq.text, n);
			}
		} else if ( n > 0 && t != NULL && dd_editable(t) && !t->xml_view ) {
			er = dd_text_drop(t, cx, cy, carry.rq.text, n);
		} else if ( n > 0 && t != NULL && df_is(t) ) {
			/* a figure: as its text, in a piece of text there or a new one */
			er = df_text_drop(t, cx, cy, carry.rq.text, n);
		} else if ( n > 0 && t == NULL && wm_at(px, py) > 0 ) {
			/* another program's window: asked first for a move, offered for a copy */
			if ( carry.copy ) {
				carry_text_to_prog(wm_at(px, py), px, py, FALSE);
			} else {
				carry_ask(wm_at(px, py), px, py);
			}
			return;
		}
		if ( er < E_OK ) {
			(void)wm_msg_put("ここには置けません");
		} else if ( t != NULL ) {
			dt_drop_keys(t);
		}
		wm_update();
		carry_answer(( er < E_OK ) ? E_NOEXS : E_OK, NULL);
		return;
	}
	if ( t == NULL && wm_at(px, py) > 0 && ( carry.rq.flags & DT_F_TO_PROG ) != 0 ) {
		if ( ( carry.rq.flags & DT_F_ASK ) != 0 || !carry.copy ) {
			carry_ask(wm_at(px, py), px, py);
		} else {
			carry_to_prog(wm_at(px, py), px, py, NULL, FALSE);
		}
		return;
	}
	if ( t == NULL || ( t->fig == NULL && t->doc == NULL ) || t->sealed || t->xml_view ) {
		(void)wm_msg_put("ここには置けません");
		carry_answer(E_NOEXS, NULL);
		return;
	}
	if ( carry.rq.ask[0][0] != 0
	  && !dt_confirm(t, (CONST char *)carry.rq.ask[0],
			 ( carry.rq.ask[1][0] != 0 ) ? (CONST char *)carry.rq.ask[1] : NULL,
			 "取消", ( carry.rq.yes[0] != 0 ) ? (CONST char *)carry.rq.yes : "実行") ) {
		wm_update();
		carry_answer(E_ABORT, NULL);
		return;
	}
	dt_drop_keys(t);
	carry.pending = TRUE;
	carry.first = -1;
	carry.seq = carry.rq.seq;
	carry.wid = t->wid;
	carry.into = t->id;
	carry.sx = px - carry.rq.grab_x;
	carry.sy = py - carry.rq.grab_y;
	carry_answer(E_OK, &t->id);
}

/* 実身複製ドラッグ: the object copied, under a name asked for over t */
LOCAL ER carry_duplicate( DTWIN *t, CONST TS_UUID *target, TS_UUID *copy )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	CONST char	*base = dt_base_name(target);
	INT		k = 0, i;

	if ( base != NULL ) {
		for ( ; base[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) start[k] = (UB)base[k];
	} else if ( om_store_name(target, start, WM_LABEL_MAX - 16) > 0 ) {
		while ( start[k] != 0 ) k++;
		for ( i = 0; "のコピー"[i] != 0 && k < WM_LABEL_MAX - 1; i++ ) start[k++] = (UB)"のコピー"[i];
	}
	start[k] = 0;
	if ( !dt_ask_name(t, "新しい実身の名称を入力してください", start, name, WM_LABEL_MAX) ) {
		return E_ABORT;
	}
	if ( om_store_copy(target, name, TRUE, copy) < E_OK ) {
		(void)wm_msg_put("実身を作れませんでした");
		return E_IO;
	}
	if ( base != NULL ) {
		(void)om_store_set_maker(copy);
	}
	return E_OK;
}

/* One link for the other program's window, among those it is to be dropped with */
LOCAL ER carry_prog_link( CONST T_DTREQ *rq, TS_UUID *p_into )
{
	T_OBDROPV	*a, *e;
	TS_UUID		target = rq->target;
	UB		*name;
	ER		er = wm_obj_drop_may(carry.wid, carry.asker, &target);

	if ( er < E_OK ) {
		return er;
	}
	if ( ( rq->flags & DT_F_DUPLICATE ) != 0 ) {
		DTWIN	*host = dt_host_of(&carry.into);

		if ( host == NULL ) {
			return E_NOEXS;
		}
		wm_obj_quiet(TRUE);	/* the keys that answer are the panel's, not the program's */
		er = carry_duplicate(host, &rq->target, &target);
		wm_obj_quiet(FALSE);
		if ( er < E_OK ) {
			return er;
		}
		er = wm_obj_drop_may(carry.wid, carry.asker, &target);
		if ( er < E_OK ) {
			(void)ob_del_obj(&target);
			return er;
		}
		if ( p_into != NULL ) {
			*p_into = target;
		}
	}
	a = (T_OBDROPV *)Kmalloc(sizeof(T_OBDROPV) * (SZ)( carry.nlink + 1 ));
	name = (UB *)Kmalloc(TAD_NAME_MAX);
	if ( a == NULL || name == NULL ) {
		if ( a != NULL ) Kfree(a);
		if ( name != NULL ) Kfree(name);
		return E_NOMEM;
	}
	if ( carry.nlink > 0 ) {
		knl_memcpy(a, carry.links, (INT)( sizeof(T_OBDROPV) * (SZ)carry.nlink ));
		Kfree(carry.links);
	}
	carry.links = a;
	e = &a[carry.nlink++];
	knl_memset(e, 0, sizeof(*e));
	e->target = target;
	/* a link to a copy shows the copy's name, not the one the original's link had */
	if ( rq->name[0] != 0 && ( rq->flags & DT_F_DUPLICATE ) == 0 ) {
		name_cut(e->name, rq->name, OB_DROP_NAME);
	} else if ( om_store_name(&target, name, TAD_NAME_MAX) >= 0 ) {
		name_cut(e->name, name, OB_DROP_NAME);
	}
	Kfree(name);
	e->left = rq->grab_x;
	e->top = rq->grab_y;
	e->right = rq->grab_x + rq->look.w;
	e->bottom = rq->grab_y + rq->look.h;
	e->frcol = rq->look.frcol;
	e->chcol = rq->look.chcol;
	e->tbcol = rq->look.tbcol;
	e->bgcol = rq->look.bgcol;
	e->chsz = rq->look.chsz;
	e->disp = rq->look.disp;
	if ( ( rq->flags & DT_F_DUPLICATE ) != 0 ) {
		/* the copy is named anew: closed, its link is as wide as that name */
		T_VOBJ	*c = (T_VOBJ *)Kcalloc(1, sizeof(T_VOBJ));

		if ( c != NULL ) {
			c->target = target;
			c->left = e->left;
			c->top = e->top;
			c->right = e->right;
			c->bottom = e->bottom;
			c->chsz = e->chsz;
			c->disp = ( e->disp != 0 ) ? e->disp : TAD_D_DEFAULT;
			ed_new_link_fit(c);
			e->right = c->right;
			e->bottom = c->bottom;
			Kfree(c);
		}
	}
	return E_OK;
}

/*
 * The same over another program's window: the links gathered, and with
 * the last all of them dropped, in the name of the process carrying.
 */
LOCAL ER carry_prog_place( CONST T_DTREQ *rq, BOOL more, TS_UUID *p_into )
{
	ER	er;

	if ( wm_obj_wid(&carry.into) != carry.wid ) {
		carry.pending = FALSE;
		carry_links_free();
		return E_NOEXS;			/* the window went while the program worked */
	}
	er = carry_prog_link(rq, p_into);
	if ( er < E_OK && er != E_ABORT ) {
		carry_said_no(er);
	}
	if ( !more && carry.nlink > 0 ) {
		/* the window it says it came from is told only when it is the process's own */
		INT	fw = wm_obj_wid(&carry.rq.win);
		ER	de = wm_obj_drop_for(carry.wid, carry.sx, carry.sy, dt_up_mods,
					     ( fw > 0 && wm_obj_maker(fw) == carry.asker ) ? &carry.rq.win : NULL,
					     carry.asker, carry.links, carry.nlink);

		if ( de < E_OK ) {
			carry_said_no(de);
			er = de;
		}
		carry_links_free();
		wm_update();
	}
	return er;
}

/*
 * A link a carry ends with, put where it was let go (moved by grab_x,
 * grab_y from the outline's corner), looking as asked. With more, the
 * carry stays open for the next link of the same outline. Only the
 * process that began the carry ends it.
 */
LOCAL ER carry_place( CONST T_DTREQ *rq, BOOL more, TS_UUID *p_into, ID asker )
{
	DTWIN	*t;
	T_VOBJ	v;

	if ( !carry.pending || rq->carry != carry.seq || asker != carry.asker ) {
		return E_OBJ;
	}
	if ( !more ) {
		carry.pending = FALSE;
	}
	if ( carry.toprog ) {
		return carry_prog_place(rq, more, p_into);
	}
	t = dt_win_of(carry.wid);
	if ( t == NULL || ts_uuid_cmp(&t->id, &carry.into) != 0 ) {
		return E_NOEXS;			/* the window went while the program worked */
	}
	/* a link is a way to the object for whoever opens it: only one who may read it makes one */
	if ( ( ( knl_ob_permit_on(&rq->target, knl_ob_crd_of(asker)) | knl_ob_granted(asker, &rq->target) ) & OB_OP_R ) != OB_OP_R ) {
		(void)wm_msg_put("この実身を読む権利がないので、仮身を置けません");
		return E_OACV;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = rq->target;
	if ( ( rq->flags & DT_F_DUPLICATE ) != 0 ) {
		/* 実身複製ドラッグ: the link put is to a copy, named where it was let go */
		ER	er = carry_duplicate(t, &rq->target, &v.target);

		if ( er < E_OK ) {
			return er;
		}
		if ( p_into != NULL ) {
			*p_into = v.target;
		}
	}
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
	if ( ( rq->flags & DT_F_DUPLICATE ) != 0 ) {
		ed_new_link_fit(&v);	/* the copy is named anew: closed, as wide as that name */
	}
	if ( !ed_place_link_of(t, carry.sx + rq->grab_x, carry.sy + rq->grab_y, &v, &carry.first) ) {
		return E_OBJ;
	}
	wm_update();
	return E_OK;
}

/*
 * Links of program windows that windows were opened from (DT_RQ_OPEN
 * with DT_F_LINK): the record that holds such a link is the program's
 * while it runs, so what a window opened from it writes into the link
 * is sent to the program, on the channel it named, for it to put into
 * its record itself. An entry stands until the program ends, or until
 * the table is full and it is the oldest.
 */
#define PLINK_MAX	64

typedef struct {
	BOOL	used;
	TS_UUID	parent;			/* the object whose record holds the link */
	TS_UUID	vobjid;			/* the link there */
	TS_UUID	target;			/* the object it points at, which was opened */
	TS_UUID	chan;			/* where the program is told */
	TS_UUID	proc;			/* the program's process */
	UINT	seq;			/* of the request it was opened with */
	UINT	age;
} PLINK;

LOCAL PLINK	plink[PLINK_MAX];
LOCAL UINT	plink_age = 0;

/* The link being opened now from a program's window, while DT_RQ_OPEN opens it */
LOCAL BOOL	plink_opening = FALSE;
LOCAL PLINK	plink_now;

LOCAL void plink_put( CONST PLINK *e )
{
	INT	i, k = -1;

	for ( i = 0; i < PLINK_MAX; i++ ) {
		if ( plink[i].used && ts_uuid_cmp(&plink[i].parent, &e->parent) == 0
		  && ts_uuid_cmp(&plink[i].vobjid, &e->vobjid) == 0 ) {
			k = i;			/* the same link opened again: told as it asks now */
			break;
		}
		if ( !plink[i].used && k < 0 ) {
			k = i;
		}
	}
	if ( k < 0 ) {
		for ( k = 0, i = 1; i < PLINK_MAX; i++ ) {
			if ( plink[i].age < plink[k].age ) k = i;
		}
	}
	plink[k] = *e;
	plink[k].used = TRUE;
	plink[k].age = ++plink_age;
}

/* The entry of link vobjid of parent, pointing at target when it is given; NULL when the program has ended */
LOCAL PLINK *plink_of( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target )
{
	INT	i;

	for ( i = 0; i < PLINK_MAX; i++ ) {
		PLINK	*e = &plink[i];

		if ( !e->used || ts_uuid_cmp(&e->parent, parent) != 0 || ts_uuid_cmp(&e->vobjid, vobjid) != 0 ) {
			continue;
		}
		if ( knl_prc_of_uuid(&e->proc) <= 0 ) {
			e->used = FALSE;	/* it has ended, its record saved: written as the desktop's own */
			return NULL;
		}
		return ( target == NULL || ts_uuid_cmp(&e->target, target) == 0 ) ? e : NULL;
	}
	return NULL;
}

/* What the window did to the link, sent to the program that holds it */
LOCAL ER plink_tell( CONST PLINK *e, UINT how, CONST TS_UUID *into, CONST UB *text )
{
	T_DTANS	an;
	SZ	asz = 0;
	ID	k;
	INT	i;
	ER	er;

	knl_memset(&an, 0, sizeof(an));
	an.seq = e->seq;
	an.er = E_OK;
	an.how = how;
	an.into = *into;
	for ( i = 0; i < DT_TEXT_MAX - 1 && text[i] != 0; i++ ) {
		an.text[i] = text[i];
	}
	k = ob_opn_obj(&e->chan, OB_OP_WRITE | OB_O_NOWAIT);
	if ( k <= 0 ) {
		return (ER)k;
	}
	er = ob_wri_rec(k, 0, 0, &an, sizeof(an), &asz);
	(void)ob_cls_obj(k);
	if ( er >= E_OK ) {
		knl_ob_post(&e->chan, 0, OB_E_CHANGE, NULL);
	}
	return er;
}

/* A number put at the end of text, after a tab */
LOCAL INT plink_num( UB *text, INT n, INT max, INT v )
{
	UB	d[12];
	INT	k = 0;
	UINT	u = ( v < 0 ) ? (UINT)-v : (UINT)v;

	if ( n < max - 1 ) text[n++] = '\t';
	if ( v < 0 && n < max - 1 ) text[n++] = '-';
	do {
		d[k++] = (UB)( '0' + u % 10 );
		u /= 10;
	} while ( u != 0 && k < (INT)sizeof(d) );
	while ( k > 0 && n < max - 1 ) text[n++] = d[--k];
	text[n] = 0;
	return n;
}

/* The link named at the front of text: its record's object, a tab, its vobjid; the length */
LOCAL INT plink_ids( UB *text, CONST PLINK *e )
{
	INT	n;

	ts_uuid_text(&e->parent, (char *)text);
	for ( n = 0; text[n] != 0; n++ ) ;
	text[n++] = '\t';
	ts_uuid_text(&e->vobjid, (char *)text + n);
	for ( ; text[n] != 0; n++ ) ;
	return n;
}

/*
 * The view of a window opened from a program's link, sent to the
 * program: TRUE when the link is such a one (*p_er what came of it),
 * FALSE when it is the desktop's to write itself
 */
LOCAL BOOL plink_view( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target,
		       INT sx, INT sy, CONST char *vm, INT wrap, INT zoom, ER *p_er )
{
	PLINK	*e = plink_of(parent, vobjid, NULL);
	UB	text[DT_TEXT_MAX];
	INT	n, i;

	if ( e == NULL ) {
		return FALSE;
	}
	if ( target != NULL && ts_uuid_cmp(&e->target, target) != 0 ) {
		*p_er = E_NOEXS;	/* it does not point at what asks */
		return TRUE;
	}
	n = plink_ids(text, e);
	n = plink_num(text, n, DT_TEXT_MAX, sx);
	n = plink_num(text, n, DT_TEXT_MAX, sy);
	n = plink_num(text, n, DT_TEXT_MAX, zoom);
	n = plink_num(text, n, DT_TEXT_MAX, wrap);
	if ( n < DT_TEXT_MAX - 1 ) text[n++] = '\t';
	for ( i = 0; vm != NULL && vm[i] != 0 && n < DT_TEXT_MAX - 1; i++ ) text[n++] = (UB)vm[i];
	text[n] = 0;
	*p_er = plink_tell(e, DT_HOW_LINK_VIEW, &e->target, text);
	return TRUE;
}

EXPORT BOOL dt_plink_has( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target )
{
	return (BOOL)( plink_of(parent, vobjid, target) != NULL );
}

EXPORT ER dt_plink_add( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *copy )
{
	PLINK	*e = plink_of(parent, vobjid, NULL);
	UB	text[DT_TEXT_MAX];

	if ( e == NULL ) {
		return E_NOEXS;
	}
	(void)plink_ids(text, e);
	return plink_tell(e, DT_HOW_LINK_ADD, copy, text);
}

EXPORT BOOL dt_plink_opening( CONST T_VOBJ *v, TS_UUID *p_parent )
{
	if ( !plink_opening || v == NULL || ts_uuid_cmp(&v->vobjid, &plink_now.vobjid) != 0 ) {
		return FALSE;
	}
	*p_parent = plink_now.parent;
	return TRUE;
}

/*
 * DT_RQ_OPEN with DT_F_LINK: the link it is opened from, taken off the
 * front of text, kept for what the window opened does to it. FALSE
 * when text does not begin as it should.
 */
LOCAL BOOL plink_take( T_DTREQ *rq, ID asker, PLINK *e )
{
	INT	i, k, f = 0, at[3];

	knl_memset(e, 0, sizeof(*e));
	rq->text[DT_CARRY_TEXT - 1] = 0;
	at[0] = 0;
	for ( i = 0; rq->text[i] != 0 && rq->text[i] != '\n'; i++ ) {
		if ( rq->text[i] == '\t' && f < 2 ) at[++f] = i + 1;
	}
	if ( rq->text[i] != '\n' || f != 2
	  || ts_str_to_uuid((CONST char *)rq->text + at[0], &e->parent) < E_OK
	  || ts_str_to_uuid((CONST char *)rq->text + at[1], &e->vobjid) < E_OK
	  || ts_str_to_uuid((CONST char *)rq->text + at[2], &e->chan) < E_OK
	  || knl_prc_uuid(asker, &e->proc, NULL) < E_OK ) {
		return FALSE;
	}
	e->target = rq->target;
	e->seq = rq->seq;
	/* what follows the line, the applist, to the front */
	for ( k = 0, i++; i < DT_CARRY_TEXT && rq->text[i] != 0; ) {
		rq->text[k++] = rq->text[i++];
	}
	rq->text[k] = 0;
	return TRUE;
}

/*
 * DT_RQ_LET_GO: what a program carried within its window, let go outside
 * it. Over a window of the desktop's that a figure's links are carried
 * into, nothing goes there and nothing moves: that window takes the keys,
 * as when links are carried there.
 */
LOCAL ER let_go_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	DTWIN	*t = dt_win_of(wm_at(rq->grab_x, rq->grab_y));

	if ( t == NULL || t->fig == NULL || t->sealed || t->xml_view ) {
		return E_NOEXS;
	}
	dt_drop_keys(t);
	an->into = t->id;
	wm_update();
	return E_OK;
}

/*
 * DT_RQ_SAVE_NEW and DT_RQ_LINK_VIEW: what a program's window opened
 * from a link of the desktop's does to that link. text names the link,
 * the parent's UUID, a tab and its vobjid, and the link must point at
 * rq->target, which the asking process must be able to read: a program
 * acts only on the link to the object it was opened on.
 */
LOCAL ER link_req( CONST T_DTREQ *rq, T_DTANS *an, ID asker )
{
	TS_UUID	parent, vobjid;
	UB	vm[TAD_VIEW_MAX];
	INT	i;
	ER	er;

	if ( ts_str_to_uuid((CONST char *)rq->text, &parent) < E_OK ) {
		return E_PAR;
	}
	for ( i = 0; i < DT_CARRY_TEXT - 1 && rq->text[i] != 0 && rq->text[i] != '\t'; i++ ) ;
	if ( rq->text[i] != '\t' || ts_str_to_uuid((CONST char *)rq->text + i + 1, &vobjid) < E_OK ) {
		return E_PAR;
	}
	if ( ( ( knl_ob_permit_on(&rq->target, knl_ob_crd_of(asker)) | knl_ob_granted(asker, &rq->target) )
	       & OB_OP_R ) != OB_OP_R ) {
		return E_OACV;
	}
	if ( rq->req == DT_RQ_SAVE_NEW ) {
		DTWIN	*host = dt_host_of(&rq->win);

		wm_obj_quiet(TRUE);		/* what answers the panel is its own, not the program's */
		er = dt_save_new_of(host, &rq->target, &parent, &vobjid, &rq->target, &an->into);
		wm_obj_quiet(FALSE);
		return er;
	}
	for ( i = 0; i < TAD_VIEW_MAX - 1 && i < (INT)sizeof(rq->name) - 1 && rq->name[i] != 0; i++ ) {
		vm[i] = rq->name[i];
	}
	vm[i] = 0;
	return dt_link_view(&parent, &vobjid, &rq->target, rq->grab_x, rq->grab_y,
			    ( vm[0] != 0 ) ? (CONST char *)vm : NULL,
			    ( rq->look.h == TAD_WRAP_ON || rq->look.h == TAD_WRAP_OFF ) ? rq->look.h : TAD_WRAP_NONE,
			    (INT)rq->look.w);
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
	INT		i, n, keys_were = 0;
	SZ		anssz;
	BOOL		quiet;

	while ( dt_req > 0 && ob_rea_rec(dt_req, 0, 0, &rq, sizeof(rq), &asz) >= E_OK ) {
		/*
		 * What is opened while the request is done -- a panel, the
		 * page of 印刷 -- is the asking process's to 強制終了 (5.5.1).
		 */
		knl_wmobj_act_for(knl_obchan_last_from(&dt_req_u));
		if ( asz != (SZ)sizeof(rq) && asz != DTREQ_V2_SIZE && asz != DTREQ_V3_SIZE
		  && !( asz == DTREQ_OPEN_SIZE && ( rq.req == DT_RQ_OPEN || rq.req == DT_RQ_TOOL ) ) ) {
			continue;
		}
		if ( asz < (SZ)sizeof(rq) ) {
			knl_memset((UB *)&rq + asz, 0, sizeof(rq) - (SZ)asz);
		}
		anssz = ( asz == DTREQ_OPEN_SIZE ) ? DTANS_OPEN_SIZE
		      : ( asz == DTREQ_V2_SIZE ) ? DTANS_V2_SIZE : (SZ)sizeof(an);
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
		quiet = FALSE;
		if ( rq.req == DT_RQ_OPEN || rq.req == DT_RQ_TOOL ) {
			ID	asker = knl_obchan_last_from(&dt_req_u);

			dt_prog_reply(&rq.reply, rq.seq, anssz, (BOOL)( rq.req == DT_RQ_TOOL ));
			/* a program without the keys is not being worked: what it asks for opens without them */
			if ( asker > 0 && wm_obj_maker(wm_focused()) != asker ) {
				quiet = TRUE;
				dt_opens_quiet++;
				keys_were = wm_focused();
			}
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
			/* opened from a link of the program's window: what is opened acts on that link */
			if ( ( rq.flags & DT_F_LINK ) != 0
			  && plink_take(&rq, knl_obchan_last_from(&dt_req_u), &plink_now) ) {
				INT	i;

				plink_put(&plink_now);
				plink_opening = TRUE;
				v.vobjid = plink_now.vobjid;
				/* shown as the link says, as from a link of a window of the desktop's */
				v.frcol = rq.look.frcol;
				v.chcol = rq.look.chcol;
				v.tbcol = rq.look.tbcol;
				v.bgcol = rq.look.bgcol;
				v.wordwrap = rq.grab_x;
				for ( i = 0; i < TAD_VIEW_MAX - 1 && i < (INT)sizeof(rq.name) - 1 && rq.name[i] != 0; i++ ) {
					v.viewmode[i] = rq.name[i];
				}
				v.viewmode[i] = 0;
			}
			dt_req_applist(&rq, &v);	/* the program a fixed link keeps to */
			dt_open_vobj(NULL, &v);
			plink_opening = FALSE;
		} else if ( rq.req == DT_RQ_CARRY ) {
			carry_start(&rq, anssz, knl_obchan_last_from(&dt_req_u));	/* answered when the hand lets go */
			continue;
		} else if ( rq.req == DT_RQ_PLACE || rq.req == DT_RQ_PLACE_MORE ) {
			an.er = carry_place(&rq, (BOOL)( rq.req == DT_RQ_PLACE_MORE ), &an.into,
					    knl_obchan_last_from(&dt_req_u));
		} else if ( rq.req >= DT_RQ_CLOSE && rq.req <= DT_RQ_FULL ) {
			an.er = dt_proc_link_op(&rq, &an);	/* 仮身操作 and 実身操作 of a program's link */
		} else if ( rq.req == DT_RQ_COLOUR ) {
			an.er = dt_proc_link_op(&rq, &an);	/* a colour chosen for a program, over its window */
		} else if ( rq.req == DT_RQ_APPS ) {
			an.er = dt_apps_req(&rq, &an);		/* 実行's list for a program's link */
		} else if ( rq.req == DT_RQ_LINK_STATE ) {
			an.er = dt_link_state_req(&rq, &an);	/* how a program's link stands, for its menu */
		} else if ( rq.req == DT_RQ_SAVE_NEW || rq.req == DT_RQ_LINK_VIEW ) {
			an.er = link_req(&rq, &an, knl_obchan_last_from(&dt_req_u));	/* the link it was opened from */
		} else if ( rq.req == DT_RQ_LET_GO ) {
			an.er = let_go_req(&rq, &an);		/* shapes let go over another window: they stay */
		} else if ( rq.req == DT_RQ_ADMIN ) {
			an.er = dt_admin_req(&rq, knl_obchan_last_from(&dt_req_u));
		} else if ( rq.req == DT_RQ_SYSUP ) {
			/* the system updated from its package: the administrator asked at its beginning */
			an.er = dt_sysup_req(&rq, &an, knl_obchan_last_from(&dt_req_u));
		} else if ( rq.req == DT_RQ_REMOVE
			 && !knl_ob_pid_admin(knl_obchan_last_from(&dt_req_u))
			 && ( an.er = dt_admin_ask(knl_obchan_last_from(&dt_req_u),
						   (CONST UB *)"プログラムの削除", &rq.win) ) < E_OK ) {
			/* only an administrator takes away programs: the password asked for */
		} else if ( rq.req == DT_RQ_INSTALL ) {
			an.er = dt_pkg_install(&rq.target, (BOOL)( rq.recno == 1 ), knl_obchan_last_from(&dt_req_u),
					       &rq.win, &an.into, &an.how);
		} else if ( rq.req == DT_RQ_REMOVE ) {
			an.er = dt_prog_remove(&rq.target);
		} else if ( ( rq.req == DT_RQ_FONT_REG || rq.req == DT_RQ_FONT_SET || rq.req == DT_RQ_FONT_DEL )
			 && !knl_ob_pid_admin(knl_obchan_last_from(&dt_req_u))
			 && ( an.er = dt_admin_ask(knl_obchan_last_from(&dt_req_u),
						   (CONST UB *)( ( rq.req == DT_RQ_FONT_DEL ) ? "書体の削除"
										 : "書体の登録" ), &rq.win) ) < E_OK ) {
			/* only an administrator changes the system's faces: the password asked for */
		} else if ( rq.req >= DT_RQ_FONT_REG && rq.req <= DT_RQ_FONT_TRY ) {
			an.er = dt_font_req(&rq, &an);		/* システム環境設定's 書体 */
		} else if ( rq.req == DT_RQ_TOOL ) {
			CONST DTPROG	**acc;
			INT		kind;

			rq.name[sizeof(rq.name) - 1] = 0;
			an.er = E_NOEXS;
			/* every accessory, then every tool of the system's (屑実身操作 ...), however many */
			for ( kind = DT_PK_ACCESSORY; kind <= DT_PK_UTILITY && an.er == E_NOEXS; kind++ ) {
				n = dt_prog_count(kind);
				acc = (CONST DTPROG **)Kmalloc(sizeof(DTPROG *) * (SZ)( ( n > 0 ) ? n : 1 ));
				n = ( acc != NULL ) ? dt_prog_list(kind, acc, n) : 0;
				if ( acc == NULL ) an.er = E_NOMEM;
				for ( i = 0; i < n; i++ ) {
					if ( knl_strcmp((CONST char *)acc[i]->name, (CONST char *)rq.name) == 0 ) {
						an.er = dt_prog_run(NULL, acc[i], NULL);
						break;
					}
				}
				if ( acc != NULL ) Kfree((void *)acc);
			}
		} else if ( rq.req == DT_RQ_TYPE ) {
			an.er = dt_type_req(&rq, &an);	/* letters into the window the keys went to before */
		} else if ( rq.req == DT_RQ_PRINT ) {
			an.er = dt_print_spool(&rq, &an);	/* 印刷 of the pages it made */
		} else if ( rq.req == DT_RQ_NOTIFY ) {
			an.er = dt_note_req(&rq, &an);		/* 通知: shown, what comes of it told later */
		} else if ( rq.req == DT_RQ_SCREEN ) {
			an.er = dt_screen_req(&rq);		/* システム環境設定's 画面サイズ, now */
		} else if ( rq.req >= DT_RQ_RT_OPEN && rq.req <= DT_RQ_RT_CLOSE ) {
			/* 実時間制御TAD played in the process's window */
			an.er = dt_rthost_req(&rq, &an, knl_obchan_last_from(&dt_req_u));
		} else {
			an.er = E_PAR;
		}
		if ( quiet ) {
			dt_opens_quiet--;
			if ( keys_were > 0 && wm_focused() != keys_were ) (void)wm_focus(keys_were);
		}
		wm_update();
		if ( dt_prog_reply_taken() ) {
			dt_prog_reply(NULL, 0, 0, FALSE);
			continue;		/* the starter answers */
		}
		dt_prog_reply(NULL, 0, 0, FALSE);
		answer_as(&rq.reply, &an, anssz);
	}
	knl_wmobj_act_for(0);
}

/*
 * A window of a program's as if it were one of the desktop's, for the
 * panels the desktop asks with over it: only its number, and no record,
 * so that nothing of a window of the desktop's is drawn or changed.
 * The desktop's first window when the program's is not said or gone.
 */
LOCAL DTWIN	dt_host;

EXPORT DTWIN *dt_host_of( CONST TS_UUID *win )
{
	INT	wid = ( win != NULL ) ? wm_obj_wid(win) : E_NOEXS;

	if ( wid <= 0 ) {
		return dt_first_win();
	}
	knl_memset(&dt_host, 0, sizeof(dt_host));
	dt_host.wid = wid;
	return &dt_host;
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
	T_OBPRT	rp;
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
	/*
	 * and the one processes find by its name: written by every
	 * program, whoever it acts for, and read by the desktop alone
	 */
	knl_memset(&rp, 0, sizeof(rp));
	rp.owner = ob_user_system;
	rp.group = ob_group_admin;
	rp.mode = 0622;
	c.prt = &rp;
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

/*
 * 'use' is the program the window is of, decided by whoever opens it
 * (the one asked for from 実行, or the one the link opens with); NULL
 * takes the one the object's applist opens it with.
 */
LOCAL INT win_open( CONST TS_UUID *id, INT recno, INT x, INT y, CONST UB *use )
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
	if ( use != NULL ) {
		for ( i = 0; use[i] != 0 && i < DT_PROG_ID - 1; i++ ) app[i] = use[i];
		app[i] = 0;
	} else if ( om_store_default_app(id, app, DT_PROG_ID) < 0 ) {
		app[0] = 0;
	}
	{
		/*
		 * The 小物箱 shows what the program box holds. A 原紙箱 is
		 * its own record, links to the templates it holds, which
		 * the person adds to and takes from as in any box; what is
		 * carried out of it is a new object made from the template.
		 */
		BOOL	box = app_is(app, "base-file-manager");
		BOOL	acc = app_is(app, "accessory-box");

		rec = acc ? dt_prog_page(DT_PK_ACCESSORY) : om_store_get(id, recno);
		if ( rec == NULL ) {
			return E_NOEXS;
		}
		knl_memset(d, 0, sizeof(*d));
		d->sealed = acc;
		d->accbox = acc;
		d->basebox = box;
	}
	if ( om_store_name(id, name, TAD_NAME_MAX) < 0 ) {
		name[0] = 0;
	}
	if ( om_store_window(id, &o) < E_OK ) {
		o.left = x;  o.top = y;
		o.right = x + 900;  o.bottom = y + 640;
	}
	tm_printf((UB *)"desktop: window of %s at %d,%d %dx%d\n", name, (INT)o.left, (INT)o.top,
		  (INT)( o.right - o.left ), (INT)( o.bottom - o.top ));
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
	if ( d->doc != NULL && tv_doc_has_sheet(d->doc) ) {
		/*
		 * A document with something laid over its whole sheet -- the
		 * squares of 原稿用紙 -- is opened on its paper, where what is
		 * laid on the sheet is seen.
		 */
		d->paper_frame = TRUE;
		ed_model(d);
	}
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
 * The pictogram of a tool's window: the icon of the program that is the
 * tool, as the window of any object takes its object's (the page a tool
 * makes is no object, and would otherwise keep the plain pictogram).
 */
LOCAL void prog_icon( INT wid, CONST DTPROG *p )
{
	UB	*file;
	UW	*px = NULL;
	SZ	size = 0;
	INT	w = 0, h = 0;

	file = om_obj_icon(&p->uuid, &size);
	if ( file == NULL ) {
		return;
	}
	if ( img_decode(file, size, &px, &w, &h) >= E_OK && px != NULL ) {
		(void)wm_set_icon(wid, px, w, h);
		Kfree(px);
	}
	Kfree(file);
}

LOCAL void made_icon( INT wid, CONST char *title, UINT tool )
{
	CONST DTPROG	*p;
	CONST UB	*id;
	INT		i, k;

	id = ( tool == DT_TOOL_TRASH ) ? (CONST UB *)"trash-real-objects"
	   : ( tool == DT_TOOL_SEARCH ) ? (CONST UB *)"real-object-search"
	   : ( tool == DT_TOOL_NETWORK ) ? (CONST UB *)"virtual-object-network"
	   : ( tool == DT_TOOL_WINLIST ) ? (CONST UB *)"window-list-viewer" : NULL;
	p = ( id != NULL ) ? dt_prog_find(id) : NULL;
	for ( i = 0; p == NULL && id == NULL; i++ ) {
		/* 原紙箱 and 小物箱: the program the window is named after */
		p = dt_prog_at(i);
		if ( p == NULL ) {
			break;
		}
		for ( k = 0; title[k] != 0 && p->name[k] == (UB)title[k]; k++ ) ;
		if ( title[k] != 0 || p->name[k] != 0 ) {
			p = NULL;
		}
	}
	if ( p == NULL ) {
		return;
	}
	prog_icon(wid, p);
}

/* A window of the desktop's own given the icon of one of the programs, by its id */
EXPORT void dt_win_icon( INT wid, CONST char *prog )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)prog);

	if ( p != NULL ) {
		prog_icon(wid, p);
	}
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
	made_icon(wid, title, tool);
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

/*
 * A window whose work area a part of the desktop draws and works: one of
 * the desktop's windows like the others, with no record and no object
 * of its own, so that it is never taken for the window of an object.
 * The part draws it when it is ready.
 */
EXPORT DTWIN *dt_own_open( CONST DTOWN *own, void *self, CONST T_DPRECT *o, UINT attr,
			   CONST char *title, CONST char *prog )
{
	DTWIN	*d = NULL;
	INT	i, wid;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( !dt_win[i].used ) {
			d = &dt_win[i];
			break;
		}
	}
	if ( d == NULL ) {
		(void)wm_msg_put("これ以上ウインドウを開けません。どれかを閉じてください");
		return NULL;
	}
	wid = wm_open(o, attr, title);
	if ( wid < 0 ) {
		return NULL;
	}
	if ( prog != NULL ) {
		dt_win_icon(wid, prog);
	}
	knl_memset(d, 0, sizeof(*d));
	d->used   = TRUE;
	d->sealed = TRUE;
	d->tool   = DT_TOOL_OWN;
	d->own    = own;
	d->self   = self;
	d->wid    = wid;
	d->paper  = TV_PAPER_LOOK;
	for ( i = 0; title[i] != 0 && i < TAD_NAME_MAX - 1; i++ ) {
		d->name[i] = (UB)title[i];
	}
	d->name[i] = 0;
	win_watch(d, NULL);
	wm_raise(wid);
	wm_focus(wid);

	return d;
}

/* The whole work area laid with a panel's ground: round a panel when the window is made larger */
EXPORT void dt_own_ground( DTWIN *d )
{
	T_WMWIN		w;
	T_DPRECT	all;
	INT		gid = wm_gid(d->wid);

	if ( gid < 0 || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	all.left = 0;
	all.top = 0;
	all.right = w.work.right - w.work.left;
	all.bottom = w.work.bottom - w.work.top;
	(void)wm_fill_look(gid, &all, LK_PNLGROUND);
}

EXPORT DTWIN *dt_own_next( CONST DTOWN *own, DTWIN *d )
{
	INT	i = ( d != NULL ) ? (INT)( d - dt_win ) + 1 : 0;

	for ( ; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].own == own ) {
			return &dt_win[i];
		}
	}
	return NULL;
}

/*
 * An event given to the part of a window with one, measured from that
 * window's work area: the window the button went down in follows the
 * hand and hears the let go wherever the pointer is.
 */
LOCAL INT	dt_own_held = 0;

LOCAL BOOL own_give( INT wid, CONST T_WMEV *ev )
{
	DTWIN	*o = dt_win_of(wid);
	T_WMEV	e = *ev;
	T_WMWIN	w, at;
	INT	px = 0, py = 0;
	UINT	btn = 0;

	if ( o == NULL || o->own == NULL ) {
		return FALSE;
	}
	if ( e.wid != wid ) {
		if ( wm_ref(wid, &w) < E_OK ) {
			return FALSE;
		}
		if ( e.wid > 0 && wm_ref(e.wid, &at) >= E_OK ) {
			px = at.work.left + e.x;
			py = at.work.top + e.y;
		} else if ( ts_hid_pointer(&px, &py, &btn) < E_OK ) {
			return FALSE;
		}
		e.x = px - w.work.left;
		e.y = py - w.work.top;
		e.wid = wid;
	}
	return o->own->input(o, &e);
}

/* Each turn of the loop: what the parts follow (a program's requests, a check under way) */
LOCAL void own_poll( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used && dt_win[i].own != NULL && dt_win[i].own->poll != NULL ) {
			dt_win[i].own->poll(&dt_win[i]);
		}
	}
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
	if ( d->pick != NULL ) {
		knl_memset(d->pick, 0, sizeof(UINT) * (SZ)d->maxpick);
	}
	ed_model(d);
	dt_draw(d);
}

/*
 * The disk of the object a window shows went and the window stays: the
 * record it shows is taken out of the store, which lets go of what it
 * holds of the object next, and is the window's own until the object is
 * found again. For the first window, which is never closed. FALSE when
 * the record is not the store's to take.
 */
EXPORT BOOL dt_win_keep( DTWIN *d )
{
	T_TAD	*rec;

	if ( d == NULL || !d->used || d->sealed || d->kept || d->rec == NULL ) {
		return FALSE;
	}
	rec = om_store_take(&d->id, d->recno);
	if ( rec == NULL ) {
		return FALSE;
	}
	if ( rec != d->rec ) {
		tad_free(rec);			/* not the one shown: the store's copy goes */
		return FALSE;
	}
	d->kept = TRUE;
	return TRUE;
}

/*
 * The object of a window kept with dt_win_keep found again: its record
 * read anew from the store, and what was changed in the window while the
 * disk was out written to it. The window shows the store's record again.
 */
EXPORT ER dt_win_back( DTWIN *d )
{
	T_TAD	*own, *rec;
	UB	*xml = NULL;
	SZ	size = 0, cap;
	ER	er = E_OK;

	if ( d == NULL || !d->used || !d->kept ) {
		return E_PAR;
	}
	own = d->rec;
	if ( d->dirty ) {
		/* the window's record as text, the way the store takes it back */
		cap = 64 * 1024;
		for ( ;; ) {
			xml = (UB *)Kmalloc(cap + 1);
			if ( xml == NULL ) {
				return E_NOMEM;
			}
			er = tad_write_mem(own, xml, cap + 1, &size);
			if ( er != E_NOMEM || size <= cap ) {
				break;
			}
			Kfree(xml);
			xml = NULL;
			cap = size;
		}
		if ( er < E_OK ) {
			Kfree(xml);
			return er;
		}
	}
	om_store_forget(&d->id);		/* what was read meanwhile is not what the window shows */
	rec = om_store_get(&d->id, d->recno);
	if ( rec != NULL && xml != NULL ) {
		rec = om_store_restore(&d->id, d->recno, xml, size);
		er = ( rec != NULL ) ? om_store_save(&d->id, d->recno) : E_IO;
	}
	if ( xml != NULL ) {
		Kfree(xml);
	}
	if ( rec == NULL ) {
		return E_NOEXS;			/* not readable yet: kept as it is */
	}
	df_text_end(d);				/* a piece of text being edited is in the old record */
	ed_forget(d);
	d->rec = rec;
	d->kept = FALSE;
	if ( er >= E_OK ) {
		d->dirty = FALSE;
	}
	tad_free(own);
	d->kind = tv_kind(rec);
	d->npick = 0;
	if ( d->pick != NULL ) {
		knl_memset(d->pick, 0, sizeof(UINT) * (SZ)d->maxpick);
	}
	ed_model(d);
	dt_draw(d);
	return er;
}


/* The window the desktop opened with, the cabinet's */
EXPORT DTWIN *dt_first_win( void )
{
	return dt_win_of(dt_first_wid);
}

/* The window at a place of the table (0 to DT_MAX_WIN - 1), used or not */
EXPORT DTWIN *dt_win_at( INT i )
{
	return ( i >= 0 && i < DT_MAX_WIN ) ? &dt_win[i] : NULL;
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
 * vm NULL, wrap TAD_WRAP_NONE and zoom 0 leave what the link says.
 * E_NOEXS when the parent holds no such link, or it does not point at
 * target (when target is given).
 */
EXPORT ER dt_link_view( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target,
			INT sx, INT sy, CONST char *vm, INT wrap, INT zoom )
{
	DTWIN	*p = dt_showing(parent);
	T_TAD	*rec;
	T_VOBJ	*v;
	BOOL	changed = FALSE;
	ER	er = E_OK;

	/* the record is a program's while it runs: it is told, and writes it itself */
	if ( plink_view(parent, vobjid, target, sx, sy, vm, wrap, zoom, &er) ) {
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( p != NULL && p->sealed ) {
		return E_OK;
	}
	rec = ( p != NULL ) ? p->rec : om_store_get(parent, 0);
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( rec == NULL || v == NULL || tad_lnk_find(rec, vobjid, v) < E_OK
	  || ( target != NULL && ts_uuid_cmp(&v->target, target) != 0 ) ) {
		if ( v != NULL ) Kfree(v);
		return E_NOEXS;
	}
	if ( v->scrollx != sx || v->scrolly != sy ) {
		v->scrollx = sx;
		v->scrolly = sy;
		changed = TRUE;
	}
	if ( vm != NULL && !str_is(v->viewmode, vm) ) {
		str_put(v->viewmode, vm, TAD_VIEW_MAX);
		changed = TRUE;
	}
	if ( wrap != TAD_WRAP_NONE && v->wordwrap != wrap ) {
		v->wordwrap = wrap;
		changed = TRUE;
	}
	if ( zoom > 0 && v->zoom != zoom ) {
		v->zoom = zoom;
		changed = TRUE;
	}
	if ( changed && ( er = tad_lnk_set(rec, v) ) >= E_OK ) {
		if ( p != NULL ) {
			p->dirty = TRUE;
		} else {
			er = om_store_save(parent, 0);
		}
	}
	Kfree(v);
	return ( er < E_OK ) ? er : E_OK;
}

LOCAL void to_link( DTWIN *d )
{
	if ( d->kind == TV_KIND_DOC ) {
		(void)dt_link_view(&d->parent_id, &d->parent_vobj, NULL, d->scroll_x, d->scroll_y,
				   d->xml_view ? "xml" : d->detail_view ? "detailed" : "formatted",
				   d->nowrap ? TAD_WRAP_OFF : TAD_WRAP_ON, 0);
	} else if ( d->figed ) {
		(void)dt_link_view(&d->parent_id, &d->parent_vobj, NULL, d->scroll_x, d->scroll_y,
				   "canvas", TAD_WRAP_NONE, df_zoom() * 100 / 8);
	} else {
		(void)dt_link_view(&d->parent_id, &d->parent_vobj, NULL, d->scroll_x, d->scroll_y,
				   NULL, TAD_WRAP_NONE, 0);
	}
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
	if ( d->own != NULL && d->own->closing != NULL && !d->own->closing(d) ) {
		return;				/* the part's: not now (a check running to its end) */
	}
	if ( d->wid == dt_first_wid && dt_first_wid > 0 ) {
		if ( dt_confirm(d, "OSを終了しますか？", NULL, "いいえ", "はい") ) {
			dt_poweroff = TRUE;
			dt_stop = TRUE;
		}
		return;
	}
	/*
	 * What could not be saved is not closed away: the window stays,
	 * with it. A composition in it is part of what is saved.
	 */
	if ( d->own == NULL ) {
		tip_forget(d);
	}
	if ( win_save(d) < E_OK ) {
		dt_tell(d, "保存できなかったため、ウインドウを閉じませんでした",
			"メモリやディスクの空きを確かめてから、もう一度閉じてください");
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
	if ( d->own != NULL ) {
		/* a window with a part of its own: the part lets go of what it keeps, then the window goes */
		CONST DTOWN	*own = d->own;

		d->own = NULL;
		own->close(d);
		d->self = NULL;
		wm_close(d->wid);
		if ( d->okey > 0 ) {
			(void)ob_cls_obj(d->okey);
			d->okey = 0;
		}
		if ( dt_own_held == d->wid ) {
			dt_own_held = 0;
		}
		d->used = FALSE;
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
			tm_printf((UB *)"desktop: window of %s kept at %d,%d %dx%d\n", d->name,
				  (INT)w.outer.left, (INT)w.outer.top, (INT)( w.outer.right - w.outer.left ),
				  (INT)( w.outer.bottom - w.outer.top ));
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
	if ( ( d->sealed || d->kept ) && d->rec != NULL ) {
		tad_free(d->rec);		/* made here or taken out of the store: not the store's */
	}
	d->kept = FALSE;
	for ( i = 0; i < d->nundo; i++ ) {
		Kfree(d->undo[i]);
	}
	for ( i = 0; i < d->nredo; i++ ) {
		Kfree(d->redo[i]);
	}
	if ( d->undo != NULL ) Kfree(d->undo);
	if ( d->undo_len != NULL ) Kfree(d->undo_len);
	if ( d->redo != NULL ) Kfree(d->redo);
	if ( d->redo_len != NULL ) Kfree(d->redo_len);
	if ( d->pick != NULL ) Kfree(d->pick);
	if ( d->pre != NULL ) Kfree(d->pre);
	d->pre = NULL;
	d->pre_len = 0;
	d->pick = NULL;
	d->maxpick = 0;
	d->undo = d->redo = NULL;
	d->undo_len = d->redo_len = NULL;
	d->nundo = d->nredo = 0;
	d->maxundo = d->maxredo = 0;
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
EXPORT INT	dt_opens_quiet;

EXPORT void dt_drop_keys( DTWIN *t )
{
	if ( t != NULL && t->used ) {
		wm_raise(t->wid);
		(void)wm_focus(t->wid);
		wm_update();
	}
}

/* Whether a program opens the object: what 実行 of its menu would list */
LOCAL BOOL app_any( CONST T_VOBJ *v )
{
	INT	room = dt_app_list(v, NULL, 0, NULL), n = 0;
	DTAPP	*a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));

	if ( a == NULL ) {
		return TRUE;
	}
	n = dt_app_list(v, a, room, NULL);
	Kfree(a);
	return (BOOL)( n > 0 );
}

EXPORT void dt_open_vobj( DTWIN *from, CONST T_VOBJ *v )
{
	DTWIN		*w = win_showing(v);
	CONST DTPROG	*p;
	INT		i, n = 0, wid;
	UB		app[DT_PROG_ID];

	/* a 虚身: what it points at is on a volume that is not attached; which one is said */
	if ( dt_disk_absent(from, v) ) {
		return;
	}
	/* from ウインドウ一覧: a program's window showing it brought to the front */
	if ( from != NULL && from->tool == DT_TOOL_WINLIST && w == NULL
	  && ( wid = wm_obj_showing(&v->target) ) > 0 ) {
		wm_raise(wid);
		(void)wm_focus(wid);
		wm_update();
		return;
	}
	/* a program object: the program is run */
	p = dt_prog_of(&v->target);
	if ( p != NULL ) {
		(void)dt_prog_run(from, p, NULL);
		return;
	}
	/* a command (a program without a window of its own) wherever it is: run in a コンソール */
	if ( dt_command_run(&v->target) != E_NOEXS ) {
		return;
	}
	/* an object a program that is a process opens: that process */
	app[0] = 0;
	if ( win_app == NULL && om_link_default_app(v, app, DT_PROG_ID) > 0 ) {
		p = prog_for_kind(dt_prog_find(app), v);
#ifdef USE_UITEST
		if ( dt_ut_as_kernel(p) ) {
			p = NULL;		/* the desktop's own editor, below */
		}
#endif
		if ( p != NULL && !p->builtin ) {
			(void)dt_prog_run(from, p, v);
			return;
		}
#ifdef USE_UITEST
		if ( p != NULL && dt_ut_as_process(from, p, v) ) {
			return;
		}
#endif
	}
	/*
	 * スライド: the document's links shown one at a time, in a window of
	 * its own. The program is a process (application/slide), started
	 * above; the desktop's own slides are shown only when the program
	 * object says it is built in, as it did before it was a process.
	 */
	if ( app_is(( win_app != NULL ) ? win_app : app, "basic-slide") ) {
		CONST DTPROG	*sp = dt_prog_find((CONST UB *)"basic-slide");

		if ( sp != NULL && !sp->builtin ) {
			(void)dt_prog_run(from, sp, v);
		} else {
			dt_slide_open(from, v);
		}
		return;
	}
	/*
	 * A record of 実時間制御TAD -- asked to be played by the id of the
	 * player that plays media (an applist a TADjs Desktop wrote names it
	 * basic-player, an alias of the player), or a record that is nothing
	 * but a <realtime>, which the editors have no page for -- played by
	 * that player, which hands the scene to the desktop (dtrthost.c)
	 */
	if ( app_is(( win_app != NULL ) ? win_app : app, "basic-player")
	  || app_is(( win_app != NULL ) ? win_app : app, RTM_VIDEO_PROG) ) {
		dt_rt_play(from, v);
		return;
	}
	if ( win_app == NULL && w == NULL ) {
		T_TAD	*rec = om_store_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);

		if ( rec != NULL && tv_kind(rec) == TV_KIND_NONE && dtrt_has(rec) ) {
			dt_rt_play(from, v);
			return;
		}
	}
	if ( w != NULL ) {
		wm_raise(w->wid);
		wm_focus(w->wid);
		wm_update();
		return;
	}
	/*
	 * Nothing to open it with, as 実行 of its menu shows: its records are
	 * no text to be shown (a package, a file taken in), and the first
	 * would be put in a window of the text editor as if it were.
	 */
	if ( win_app == NULL && app[0] == 0 && !app_any(v) ) {
		(void)wm_msg_put("この実身を開くプログラムがありません");
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
	/* the program decided above: a link fixed to its program keeps to it here too */
	wid = win_open(&v->target, ( v->recno >= 0 ) ? v->recno : 0,
		       60 + n * 28, 40 + n * 28,
		       ( win_app != NULL ) ? win_app : ( app[0] != 0 ) ? app : NULL);
	if ( wid >= 0 ) {
		w = dt_win_of(wid);
		if ( w != NULL && from != NULL ) {
			w->has_parent  = TRUE;
			w->parent_id   = from->id;
			w->parent_vobj = v->vobjid;
		} else if ( w != NULL && dt_plink_opening(v, &w->parent_id) ) {
			w->has_parent  = TRUE;	/* a link of a program's window: written through the program */
			w->parent_vobj = v->vobjid;
		}
		from_link(w, v);
		auto_open(w);
		wm_update();
	} else if ( wid == E_LIMIT ) {
		/* the table of windows full: said, not left to look as if nothing was asked */
		tm_printf((UB *)"desktop: no window opened: %d are open already\n", DT_MAX_WIN);
		(void)wm_msg_put("これ以上ウインドウを開けません。どれかを閉じてください");
	} else {
		tm_printf((UB *)"desktop: no window opened for the object: %d\n", (INT)wid);
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
	INT	i, n, was;
	BOOL	any = FALSE;
	T_VOBJ	*v;

	if ( d == NULL || d->rec == NULL || auto_depth >= AUTO_DEPTH ) {
		return;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) {
		return;
	}
	auto_depth++;
	dt_opens_quiet++;
	was = wm_focused();
	n = tad_lnk_count(d->rec);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(d->rec, i, v) >= E_OK && v->autoopen
		  && ( !v->hidden || d->show_hidden ) ) {
			dt_open_vobj(d, v);
			any = TRUE;
		}
	}
	/* those came with it: what was opened, or what had the keys, keeps them and stays in sight */
	if ( any && was > 0 && wm_focus(was) >= E_OK ) {
		wm_raise(was);
	}
	dt_opens_quiet--;
	auto_depth--;
	Kfree(v);
}

/* The window showing an object, if one is */
/* Every 小物箱 on the screen made again, each left scrolled as it was */
EXPORT void dt_acc_boxes_remake( void )
{
	INT	i, sx, sy;
	T_TAD	*rec;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = &dt_win[i];

		if ( !d->used || !d->accbox || ( rec = dt_acc_page() ) == NULL ) {
			continue;
		}
		sx = d->scroll_x;
		sy = d->scroll_y;
		dt_remade(d, rec);
		d->scroll_x = sx;
		d->scroll_y = sy;
		dt_draw(d);
	}
	wm_update();
}

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
		wm_update();
	}
}

/*
 * The object a virtual object points at opened with the program asked
 * for (実行): one that is a process is started with it, one of the
 * desktop's own opens it in a window of its kind.
 */
EXPORT ER dt_open_as( DTWIN *from, CONST T_VOBJ *v, CONST UB *app )
{
	CONST DTPROG	*p = prog_for_kind(dt_prog_find(app), v);

	if ( app_is(app, "basic-print") ) {
		dt_print_vobj(from, v);		/* 基本印刷: printed without being opened */
		return E_OK;
	}
#ifdef USE_UITEST
	if ( dt_ut_as_kernel(p) ) {
		win_app = app;
		dt_open_vobj(from, v);
		win_app = NULL;
		return E_OK;
	}
#endif
	if ( p != NULL && ( !p->builtin || p->kind != DT_PK_APP ) ) {
		return dt_prog_run(from, p, v);
	}
#ifdef USE_UITEST
	if ( p != NULL && dt_ut_as_process(from, p, v) ) {
		return E_OK;
	}
#endif
	win_app = app;
	dt_open_vobj(from, v);
	win_app = NULL;

	return E_OK;
}

/*
 * The program at a place of the list 実行 shows (dt_app_list) run on the
 * object. One its applist names: unless the link keeps to its program
 * (起動固定), the one run last is written into the object's metadata as
 * the one it opens with from now, when it is opened by a double press
 * (om_link_ran). One offered for the object's kind leaves its default
 * as it is.
 */
EXPORT void dt_exec( DTWIN *from, CONST T_VOBJ *v, INT index )
{
	INT	room = dt_app_list(v, NULL, 0, NULL), n;
	DTAPP	*a = (DTAPP *)Kmalloc(sizeof(DTAPP) * (SZ)( room + 1 ));

	n = ( a != NULL ) ? dt_app_list(v, a, room, NULL) : 0;
	if ( index >= 0 && index < n ) {
		if ( a[index].listed ) (void)om_link_ran(v, a[index].raw);
		(void)dt_open_as(from, v, a[index].id);
	} else {
		dt_open_vobj(from, v);
	}
	if ( a != NULL ) Kfree(a);
}

/* The window a virtual object was opened into, closed */
EXPORT void dt_close_vobj( CONST T_VOBJ *v )
{
	DTWIN	*w = win_showing(v);

	if ( w != NULL ) {
		win_close_asked(w);
		wm_update();
	}
}

/* ---------------------------------------------------------------- the menu */

/*
 * The menu of the program under the pointer, with what applies at that
 * place: whether the press was on a virtual object, whether that one is
 * open in a window, whether anything is taken.
 *
 * The button that opened it is pressed and let go as a click (quickly,
 * or without the pointer moving, wm_click_take): the menu stays, and a
 * row is chosen with the first button only (wm_menu_event), and a press
 * anywhere off the menu puts it away. While the button that opened it
 * is held, the other buttons are not the menu's. The button held,
 * carried off the menu and let go there puts it away too.
 */
EXPORT INT dt_run_menu( DTWIN *d, INT pid, UD when )
{
	T_WMEV		ev;
	T_WMCLICK	ck;
	INT		cmd = 0, k;

	wm_panel_draw(pid);
	wm_update();

	wm_click_begin(&ck, when, WM_CK_FIND);
	while ( !dt_stop ) {
		BOOL	on_menu;

		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		k = wm_click_take(&ck, &ev);
		if ( k != WM_CK_PASS ) {
			if ( k == WM_CK_EAT ) {
				tm_printf((UB *)"desktop: menu: button %d %s while the menu's is held: not taken\n",
					  ev.code, ( ev.type == HID_EV_BTN_DOWN ) ? "pressed" : "let go");
			}
			continue;	/* a click: the menu stays */
		}
		on_menu = wm_menu_has(pid, ev.wid);
		if ( ev.type == HID_EV_BTN_DOWN && !on_menu ) {
			break;		/* a press off the menu puts it away */
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
	wm_update();

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
	st.has_pick  = (BOOL)( d->npick > 0 || d->dd.cpara != d->dd.apara
				  || d->dd.cpos != d->dd.apos );
	st.nrecent   = dd_recent_faces(st.recent, DM_RECENT_MAX);
	st.can_paste = (BOOL)( dt_tray_kind(NULL) != DT_TRAY_NONE );
	st.vertical  = (BOOL)( d->doc != NULL && d->doc->vert );
	dd_menu_state(d, &st.comb, &st.turned);

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

	if ( d->own != NULL ) {
		/*
		 * A part's: its definition, set by the part, with 小物 and
		 * 実行 of the tail filled in as for every window -- 実行 for
		 * a link in it, grey away from one
		 */
		TS_UUID	def, td;

		er = ts_str_to_uuid(d->own->menu, &def);
		if ( er >= E_OK ) {
			er = mn_cre_men(&def, p_mid);
		}
		if ( er < E_OK ) {
			return er;
		}
		dt_tail_fill(*p_mid);
		dt_get_vmn(d, *p_mid, on_vobj, v);
		if ( on_vobj && ts_str_to_uuid(SYSDEF_MENU_TAIL, &td) >= E_OK ) {
			(void)mn_chg_atr(*p_mid, &td, "exec", 0);
		}
		if ( d->own->menu_set != NULL ) {
			d->own->menu_set(d, *p_mid);
		}
		return E_OK;
	}
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
	if ( d->own != NULL ) {
		d->own->menu_do(d, sel);	/* the part's own items */
		wm_update();
		return;
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
	wm_update();
}

/*
 * The link that is taken, in *v: in a figure, of the links taken the
 * one taken first; in a document, the link the selection is alone.
 * FALSE when no link is taken.
 */
LOCAL BOOL picked_link( DTWIN *d, T_VOBJ *v )
{
	INT	i, best = -1;

	if ( d->doc != NULL ) {
		return dd_link_picked(d, v);
	}
	if ( d->fig == NULL || d->npick == 0 ) {
		return FALSE;
	}
	for ( i = 0; i < d->fig->nsh && i < d->maxpick; i++ ) {
		if ( d->pick[i] != 0 && d->fig->sh[i].kind == TV_SH_LINK && d->fig->sh[i].link >= 0
		  && ( best < 0 || d->pick[i] < d->pick[best] ) ) {
			best = i;
		}
	}

	return (BOOL)( best >= 0 && ed_link_at(d, best, v) );
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
			wm_update();
		}
	} else if ( d->own != NULL && d->own->link_at != NULL ) {
		er = d->own->link_at(d, x, y, &v) ? E_OK : E_NOEXS;
	}
no_object:
	/*
	 * Away from a link, the menu is about the link that is taken:
	 * 実行 lists its programs and 仮身操作 and 実身操作 act on it,
	 * wherever the menu was opened
	 */
	if ( er < E_OK && picked_link(d, &v) ) {
		er = E_OK;
	}
	on = (BOOL)( er >= E_OK );
	if ( win_menu(d, on, &v, &mid) < E_OK ) {
		return;
	}
	pid = mn_opn_men(mid, d->wid, x, y);
	if ( pid >= 0 ) {
		if ( d->own != NULL ) {
			tm_printf((UB *)"desktop: menu %s\n", d->name);	/* for the tests, as a process's says it */
		}
		cmd = dt_run_menu(d, pid, when);
	}
	er = mn_get_sel(mid, cmd, &sel);
	(void)mn_del_men(mid);
	if ( er >= E_OK ) {
		if ( d->own != NULL ) {
			tm_printf((UB *)"desktop: chosen %s\n", sel.code);
		}
		win_menu_do(d, &sel, on, &v);
	}
	wm_update();
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
	case DM_PRINT:
		dt_print_window(d);
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
	wm_update();

	return (BOOL)( ans == WM_ANS_OK && out[0] != 0 );
}

/* ---------------------------------------------------------------- winding */

LOCAL void scroll_to( DTWIN *d, INT to )
{
	T_DPRECT	r;
	INT		room;

	dt_work_rect(d, &r);
	room = d->height - ( r.bottom - r.top );
	if ( d->doc != NULL && d->doc->vert && !d->xml_view ) {
		room = d->height - ( r.right - r.left );	/* 縦書き: across */
	}
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
	wm_update();
}

/* Across: a document is as wide as its page and does not go across */
LOCAL void scroll_x_to( DTWIN *d, INT to )
{
	T_DPRECT	r;
	INT		room;

	dt_work_rect(d, &r);
	if ( d->doc != NULL && d->doc->vert && !d->xml_view ) {
		/*
		 * 縦書き: the bar across winds the text, its knob at the
		 * right end showing the head; the text is wound as far as the
		 * knob is from that end.
		 */
		INT	view = r.right - r.left;

		room = d->height - view;
		if ( room < 0 ) {
			room = 0;
		}
		if ( to > room ) to = room;
		if ( to < 0 )    to = 0;
		scroll_to(d, room - to);
		return;
	}
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
	wm_update();
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

	if ( d != NULL && d->own != NULL ) {
		(void)own_give(d->wid, ev);	/* the part's to make what it will of */
		return;
	}
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

/*
 * A size of the screen object's record 1: "SIZE", the screen's pixels,
 * or "VIEW", the view the windows are laid in (the screen at its scale;
 * the size where the record has no view)
 */
LOCAL ER screen_rec_size( CONST char *what, UINT *p_w, UINT *p_h )
{
	UB	t[640];
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
	if ( ( cf_get(t, (INT)asz, what, 0, v, sizeof(v)) && size_of(v, p_w, p_h) )
	  || ( cf_get(t, (INT)asz, "SIZE", 0, v, sizeof(v)) && size_of(v, p_w, p_h) ) ) {
		return E_OK;
	}
	return E_NOEXS;
}

EXPORT ER dt_screen_size( UINT *p_w, UINT *p_h )
{
	return screen_rec_size("VIEW", p_w, p_h);
}

/* The screen's own pixels, which a size is set in */
LOCAL ER screen_px_size( UINT *p_w, UINT *p_h )
{
	return screen_rec_size("SIZE", p_w, p_h);
}

/*
 * The screen set to w x h; 'fit' ("aspect" or "stretch", or NULL or ""
 * for neither) says how a size of another shape than the panel is laid
 * on it, where the screen can choose (画面's FIT)
 */
LOCAL ER screen_set_fit( UINT w, UINT h, CONST char *fit )
{
	char	s[48];
	ID	key = disp_key();

	if ( key <= 0 ) {
		return E_NOEXS;
	}
	if ( fit != NULL && fit[0] != 0 ) {
		tm_sprintf((UB *)s, (UB *)"%dx%d %s", (INT)w, (INT)h, fit);
	} else {
		tm_sprintf((UB *)s, (UB *)"%dx%d", (INT)w, (INT)h);
	}
	return ob_wri_rec(key, OB_DSP_MODE, 0, s, (SZ)knl_strlen(s), NULL);
}

LOCAL ER screen_set( UINT w, UINT h )
{
	return screen_set_fit(w, h, NULL);
}

/* How the screen lays a size of another shape now (画面's FIT): "" where it does not choose */
/* The scale the screen is drawn at (画面's SCALE), 100 where it says none */
LOCAL UINT screen_scale( void )
{
	UB	t[640];
	char	v[16];
	SZ	asz = 0;
	ID	key = disp_key();

	if ( key > 0 && ob_rea_rec(key, OB_DSP_MODE, 0, t, sizeof(t), &asz) >= E_OK
	  && cf_get(t, (INT)asz, "SCALE", 0, v, sizeof(v)) && cf_num(v, 100) >= 100 ) {
		return (UINT)cf_num(v, 100);
	}
	return 100;
}

/* The screen drawn at a scale (percent): E_NOSPT when the view would be too small */
LOCAL ER screen_set_scale( UINT scale )
{
	char	s[16];
	ID	key = disp_key();

	if ( key <= 0 ) {
		return E_NOEXS;
	}
	tm_sprintf((UB *)s, (UB *)"%d%%", (INT)scale);
	return ob_wri_rec(key, OB_DSP_MODE, 0, s, (SZ)knl_strlen(s), NULL);
}

/* "150%" among the words of a line: the scale, or 0 */
LOCAL UINT word_scale( CONST char *t )
{
	char	a[16];
	INT	k, n;

	for ( k = 0; k < 6 && cf_word(t, k, a, sizeof(a)); k++ ) {
		n = (INT)knl_strlen(a);
		if ( n >= 2 && a[n - 1] == '%' ) {
			a[n - 1] = 0;
			return (UINT)cf_num(a, 0);
		}
	}
	return 0;
}

LOCAL void screen_fit( char *v, INT max )
{
	UB	t[512];
	SZ	asz = 0;
	ID	key = disp_key();

	v[0] = 0;
	if ( key > 0 && ob_rea_rec(key, OB_DSP_MODE, 0, t, sizeof(t), &asz) >= E_OK ) {
		(void)cf_get(t, (INT)asz, "FIT", 0, v, max);
	}
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

/*
 * The system's containers named as the objects they hold, not as boxes
 * (docs/private/btron2-audit.md 10): a system volume made before has
 * them by their old names, and each still named so is renamed, once, at
 * the start. A name an administrator gave is left as it is. Nothing
 * looks for these objects by their names -- they are opened by their
 * identities -- so the names are only what the person sees.
 */
LOCAL void sysdef_names( void )
{
	static CONST struct {
		CONST char	*id;
		CONST char	*was;
		CONST char	*now;
	} nm[] = {
		{ SYSDEF_SYSTEM_BOX,				"システム箱",	"システム" },
		{ SYSDEF_BOX,					"定義箱",	"画面の定義" },
		{ "01a0d889-45e6-7ad9-ba7f-b36dc9467f24",	"メニュー箱",	"メニューの定義" },
		{ "01a0d6d1-08d9-79a7-b927-ed74aba76bce",	"部品箱",	"部品の定義" },
		{ SYSDEF_FONT_BOX,				"書体箱",	"書体" },
		{ SYSDEF_WALL_BOX,				"壁紙箱",	"壁紙" },
		{ SYSDEF_LEARN_BOX,				"学習箱",	"かな漢字変換の学習" },
		{ SYSDEF_DICT_BOX,				"辞書箱",	"辞書" },
		{ SYSDEF_SND_BOX,				"音箱",		"システムの音" },
		{ SYSDEF_XFCONV_BOX,				"ファイル変換の箱", "ファイル変換" },
	};
	TS_UUID	u;
	T_OBREF	r;
	UINT	i;
	INT	k;

	for ( i = 0; i < sizeof(nm) / sizeof(nm[0]); i++ ) {
		if ( ts_str_to_uuid(nm[i].id, &u) < E_OK || ob_ref_obj(&u, &r) < E_OK ) continue;
		for ( k = 0; nm[i].was[k] != 0 && r.name[k] == (UB)nm[i].was[k]; k++ ) ;
		if ( nm[i].was[k] != 0 || r.name[k] != 0 ) continue;
		if ( om_store_rename(&u, (CONST UB *)nm[i].now) >= E_OK ) {
			tm_printf((UB *)"TessronOS desktop: %s renamed %s\n", nm[i].was, nm[i].now);
		}
	}
}

LOCAL void video_start( void )
{
	char	v[CF_VAL_MAX], a[32], b[32], fit[16];

	vm_w = vm_h = vm_ow = vm_oh = 0;
	if ( !wm_dev_conf_get("VIDEOFIT", fit, sizeof(fit)) ) fit[0] = 0;
	if ( !wm_dev_conf_get("VIDEOMODE", v, sizeof(v)) || !cf_word(v, 0, a, sizeof(a))
	  || !size_of(a, &vm_w, &vm_h) ) {
		UINT	w, h;

		if ( fit[0] != 0 && screen_px_size(&w, &h) >= E_OK ) {
			(void)screen_set_fit(w, h, fit);	/* the shape alone was chosen */
		}
		return;
	}
	if ( cf_word(v, 1, b, sizeof(b)) ) (void)size_of(b, &vm_ow, &vm_oh);
	if ( screen_set_fit(vm_w, vm_h, fit) < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: the screen cannot be %d x %d\n", (INT)vm_w, (INT)vm_h);
		vm_ow = vm_oh = 0;
	}
}

/*
 * The scale the machine's settings ask for (VIDEOSCALE, percent), set as
 * the desktop starts, after the size: a scale the screen's size cannot
 * take is kept asked for and the largest that it can take is used
 * (ts_disp_setscale says so for a size that leaves too small a view).
 */
LOCAL void scale_start( void )
{
	char	v[CF_VAL_MAX];
	UINT	sc;
	ER	er;

	if ( !wm_dev_conf_get("VIDEOSCALE", v, sizeof(v)) ) {
		return;
	}
	sc = (UINT)cf_num(v, 100);
	if ( sc == 100 || sc == screen_scale() ) {
		return;
	}
	er = screen_set_scale(sc);
	tm_printf((UB *)"TessronOS desktop: the screen drawn at %d%% (%d)\n", (INT)sc, (INT)er);
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

/*
 * DT_RQ_SCREEN (システム環境設定's 画面サイズ, while the desktop runs):
 * the screen set now to the size and shape rq->text says
 * ("WIDTHxHEIGHT" and "aspect" or "stretch"), every window of the
 * desktop drawn again and the wallpaper laid again, then asked about:
 * Enter or 確認 within thirty seconds keeps it, and the machine's
 * settings take it for the next start (VIDEOMODE, VIDEOFIT); anything
 * else, or nothing, puts back the size and shape there were. E_OK when
 * kept, E_ABORT when put back, E_NOSPT when the screen cannot be set
 * so, E_PAR for words that are not a size.
 */
LOCAL void screen_redraw( void )
{
	INT	i;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used ) dt_draw(&dt_win[i]);
	}
	(void)dt_res_wall(WM_WALL_FIT);
	(void)wm_composite();
	wm_update();
}

EXPORT ER dt_screen_req( CONST T_DTREQ *rq )
{
	CONST char *CONST lines[5] = {
		"画面の設定を切り換えましたので表示を確認してください。",
		"正しく表示されているときは、３０秒以内に［Ｅｎｔｅｒ］",
		"キーか［確認］を押してください。",
		"［Ｅｎｔｅｒ］以外のキーを押すか、３０秒以上キーを押さ",
		"ないと、以前の画面の設定に戻します。",
	};
	char	t[DT_CARRY_TEXT], a[32], fit[16], ofit[16], s[48];
	UINT	w, h, ow, oh, sc, osc;
	ER	er;

	knl_memcpy(t, rq->text, sizeof(t));
	t[sizeof(t) - 1] = 0;
	if ( !cf_word(t, 0, a, sizeof(a)) || !size_of(a, &w, &h) ) {
		return E_PAR;
	}
	if ( !cf_word(t, 1, fit, sizeof(fit)) || fit[knl_strlen(fit) - 1] == '%' ) fit[0] = 0;
	if ( screen_px_size(&ow, &oh) < E_OK ) {
		return E_NOEXS;
	}
	osc = screen_scale();
	sc = word_scale(t);			/* "150%": the scale it is to be drawn at */
	if ( sc == 0 ) sc = osc;
	screen_fit(ofit, sizeof(ofit));
	if ( fit[0] != 0 && ofit[0] == 0 ) fit[0] = 0;	/* a screen that does not choose the shape */
	tm_printf((UB *)"TessronOS desktop: the screen %d x %d %s -> %d x %d %s, asked\n", (INT)ow, (INT)oh, ofit,
		  (INT)w, (INT)h, fit);
	if ( sc != osc ) {
		tm_printf((UB *)"TessronOS desktop: drawn at %d%% -> %d%%, asked\n", (INT)osc, (INT)sc);
	}
	if ( w == ow && h == oh && knl_strcmp(fit, ofit) == 0 && sc == osc ) {
		return E_OK;
	}
	er = ( w == ow && h == oh && knl_strcmp(fit, ofit) == 0 ) ? E_OK : screen_set_fit(w, h, fit);
	if ( er < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: the screen cannot be %d x %d (%d)\n", (INT)w, (INT)h, (INT)er);
		return er;
	}
	if ( sc != screen_scale() ) {
		er = screen_set_scale(sc);
		if ( er < E_OK ) {
			tm_printf((UB *)"TessronOS desktop: the screen cannot be drawn at %d%% (%d)\n", (INT)sc, (INT)er);
			if ( w != ow || h != oh || knl_strcmp(fit, ofit) != 0 ) {
				(void)screen_set_fit(ow, oh, ofit);
				screen_redraw();
			}
			return er;
		}
	}
	screen_redraw();
	if ( dt_first_win() != NULL && dt_confirm_timed(dt_first_win(), lines, 5, "取り消し", "確認", 30000) ) {
		tm_sprintf((UB *)s, (UB *)"%dx%d", (INT)w, (INT)h);
		(void)wm_dev_conf_put("VIDEOMODE", s);
		if ( fit[0] != 0 ) (void)wm_dev_conf_put("VIDEOFIT", fit);
		tm_printf((UB *)"TessronOS desktop: the screen of %s %s kept\n", s, fit);
		if ( sc != osc ) {
			tm_sprintf((UB *)s, (UB *)"%d", (INT)sc);
			(void)wm_dev_conf_put("VIDEOSCALE", s);
			tm_printf((UB *)"TessronOS desktop: drawn at %d%%, kept\n", (INT)sc);
		}
		return E_OK;
	}
	if ( w != ow || h != oh || knl_strcmp(fit, ofit) != 0 ) {
		(void)screen_set_fit(ow, oh, ofit);
	}
	if ( sc != osc ) {
		(void)screen_set_scale(osc);
	}
	screen_redraw();
	tm_printf((UB *)"TessronOS desktop: the screen put back to %d x %d %s\n", (INT)ow, (INT)oh, ofit);
	if ( sc != osc ) {
		tm_printf((UB *)"TessronOS desktop: drawn at %d%% again\n", (INT)osc);
	}
	return E_ABORT;
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
/*
 * Whether a press on a window's frame at (sx, sy) on the screen is on
 * the corner at its foot that sizes it (WM_GRIP_CORNER). A finger's
 * (wm_hit_slop) reaches it farther along the frame, and out of the
 * window, where the window manager gave it the press (wm_read_event).
 */
LOCAL BOOL grip_at( CONST T_WMWIN *w, INT sx, INT sy, UD when )
{
	INT	slop = wm_hit_slop(when);
	INT	cx = wm_hit_in(WM_GRIP_CORNER, slop, w->outer.right - w->outer.left);
	INT	cy = wm_hit_in(WM_GRIP_CORNER, slop, w->outer.bottom - w->outer.top);

	return (BOOL)( sx > w->outer.right - cx && sy > w->outer.bottom - cy );
}

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
				wm_update();
			}
			return;
		}
		dt_move_wid = d->wid;
		dt_sizing = grip_at(&w, sx, sy, ev->when);
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

	/* a window with a part of its own: the press is the part's, and so is the let go */
	if ( d->own != NULL ) {
		dt_own_held = d->wid;
		(void)own_give(d->wid, ev);
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
	if ( df_transform_press(d, ev->x, ev->y, ev->when) || df_end_press(d, ev->x, ev->y, ev->when) ) {
		return;
	}
	/* 詳細: the flags and rulers of the settings */
	if ( dd_mark_press(d, ev->x, ev->y, ev->when) ) {
		return;
	}
	/* the page: taking, carrying, sizing, opening; in a text, the caret */
#ifdef USE_UITEST
	if ( df_is(d) || ut_cabinet(d) ) dt_fig_input_at = ev->when;
#endif
	if ( !ed_press(d, ev->x, ev->y, (BOOL)( ( ev->mods & MOD_SHIFT ) != 0 ),
		       ev->when) ) {
		if ( dd_editable(d) ) {
			dd_press(d, ev->x, ev->y,
				 (BOOL)( ( ev->mods & MOD_SHIFT ) != 0 ), ev->when);
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
	  || ( w.attr & WM_ATTR_POPUP ) != 0 || ( ( w.attr & WM_ATTR_OWN ) == 0 && w.parent <= 0 ) ) {
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
		wm_update();
		return;
	}
	if ( ev->code == 0 && ( ev->x < 0 || ev->y < 0
			       || sx >= w.work.right || sy >= w.work.bottom )
	  && !( wm_part_at(sx, sy, &got, &bar) == WM_PART_PICT && got == ev->wid ) ) {
		dt_move_wid = ev->wid;
		dt_sizing = (BOOL)( ( w.attr & WM_ATTR_RESIZE ) != 0 && grip_at(&w, sx, sy, ev->when) );
		dt_grab_dx = sx - ( dt_sizing ? w.outer.right : w.outer.left );
		dt_grab_dy = sy - ( dt_sizing ? w.outer.bottom : w.outer.top );
		wm_set_pointer(dt_sizing ? WM_PT_PICK : WM_PT_MOVE);
	}
	wm_update();
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
	UINT	cmd = wm_key_cmd(ev->code);		/* the key of the letter it types: a command's */
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
	if ( d->own != NULL ) {
		/* the part's key first; one with Ctrl it leaves is a letter of its menu */
		T_WMEV	e = *ev;

		e.wid = d->wid;
		if ( own_give(d->wid, &e) ) {
			return;
		}
		if ( ctrl && key_letter(cmd, &letter) && win_menu(d, FALSE, NULL, &mid) >= E_OK ) {
			er = mn_fnd_key(mid, letter, &sel);
			(void)mn_del_men(mid);
			if ( er >= E_OK ) {
				win_menu_do(d, &sel, FALSE, NULL);
			}
		}
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
#ifdef USE_UITEST
	UD	ut_in = 0;

	(void)ts_get_mono(&ut_in);
#endif
	if ( dd_editable(d) && dd_key(d, ev->code, ev->mods) ) {
#ifdef USE_UITEST
		/*
		 * the tests compare it with the text editor running as a process
		 * (ui_doc_proc_perf): the screen built here, as the loop would
		 * build it next, for the time to be to the screen as the
		 * process's is; with when the key came to the desktop and how
		 * long the editor took
		 */
		if ( d->doc != NULL ) {
			UD	t = 0, t2 = 0;

			(void)ts_get_mono(&t);
			wm_update();
			(void)ts_get_mono(&t2);
			tm_printf((UB *)"desktop: document key to screen %d us came %d us edited %d us built %d us\n",
				  (INT)( ( t2 - ev->when ) / 1000 ), (INT)( ( ut_in - ev->when ) / 1000 ),
				  (INT)( ( t - ut_in ) / 1000 ), (INT)( ( t2 - t ) / 1000 ));
		}
#endif
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
	if ( key_letter(cmd, &letter) && win_menu(d, FALSE, NULL, &mid) >= E_OK ) {
		er = mn_fnd_key(mid, letter, &sel);
		(void)mn_del_men(mid);
		if ( er >= E_OK ) {
			win_menu_do(d, &sel, FALSE, NULL);
			return;
		}
	}
	/* the keys that are not in a menu */
	switch ( cmd ) {
	case KEY_O:	ed_open_picked(d);		break;
	case KEY_A:
		ed_pick_all(d);
		dt_draw(d);
		wm_update();
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
	return dt_drop_links_at(d, sx, sy, sx, sy, v, n);
}

/* The links v of window d (NULL: none) dropped on window wid, the outline's corner at ax, ay */
LOCAL ER drop_give( DTWIN *d, INT wid, INT ax, INT ay, UINT mods, CONST T_VOBJ *v, INT n )
{
	T_OBDROPV	*dv;
	TS_UUID		from;
	UB		*name;
	INT		i;
	ER		er;

	dv = (T_OBDROPV *)Kmalloc(sizeof(T_OBDROPV) * (SZ)n);
	name = (UB *)Kmalloc(TAD_NAME_MAX);
	if ( dv == NULL || name == NULL ) {
		if ( dv != NULL ) Kfree(dv);
		if ( name != NULL ) Kfree(name);
		return E_NOMEM;
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
	er = wm_obj_drop(wid, ax, ay, mods, &from, dv, n);
	Kfree(dv);
	Kfree(name);
	return er;
}

EXPORT BOOL dt_drop_links_at( DTWIN *d, INT sx, INT sy, INT ax, INT ay, CONST T_VOBJ *v, INT n )
{
	INT	wid = wm_at(sx, sy);
	ER	er;

	if ( wid <= 0 || dt_win_of(wid) != NULL || n <= 0 ) {
		return FALSE;			/* the ground, or a window of the desktop's */
	}
	er = drop_give(d, wid, ax, ay, dt_up_mods, v, n);
	if ( er == E_NOMEM ) {
		return FALSE;
	}
	if ( er < E_OK ) {
		(void)wm_msg_put("このウインドウには置けません");
	}
	return TRUE;			/* over another's window, nothing here moves */
}

/* A move of links out of a window of the desktop's, waiting for the program to say it takes them */
LOCAL struct {
	BOOL	on;
	INT	wid, ax, ay, n;
	UINT	seq, mods;
	UW	since;
	TS_UUID	from;			/* the object of the window they leave, to find it again */
	T_VOBJ	*v;
	void	(*gone)( DTWIN *d, CONST T_VOBJ *v, INT n );
} drop_asking;

EXPORT BOOL dt_drop_move( DTWIN *d, INT sx, INT sy, INT ax, INT ay, CONST T_VOBJ *v, INT n,
			  void (*gone)( DTWIN *d, CONST T_VOBJ *v, INT n ) )
{
	INT	wid = wm_at(sx, sy);
	UINT	seq = 0;
	ER	er;

	if ( wid <= 0 || dt_win_of(wid) != NULL || n <= 0 ) {
		return FALSE;			/* the ground, or a window of the desktop's */
	}
	if ( gone == NULL || d == NULL || drop_asking.on ) {
		return dt_drop_links_at(d, sx, sy, ax, ay, v, n);	/* a copy: offered, and they stay */
	}
	er = wm_obj_ask(wid, sx, sy, dt_up_mods, &seq);
	tm_printf((UB *)"desktop: drop asks window %d at %d,%d: %d\n", wid, sx, sy, (INT)er);
	if ( er < E_OK ) {
		return dt_drop_links_at(d, sx, sy, ax, ay, v, n);	/* one not asked is offered them, and they stay */
	}
	drop_asking.v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * (SZ)n);
	if ( drop_asking.v == NULL ) {
		return dt_drop_links_at(d, sx, sy, ax, ay, v, n);
	}
	knl_memcpy(drop_asking.v, v, sizeof(T_VOBJ) * (SZ)n);
	drop_asking.on = TRUE;
	drop_asking.wid = wid;
	drop_asking.ax = ax;
	drop_asking.ay = ay;
	drop_asking.n = n;
	drop_asking.seq = seq;
	drop_asking.mods = dt_up_mods;
	drop_asking.since = now_ms();
	drop_asking.from = d->id;
	drop_asking.gone = gone;
	return TRUE;
}

/* The answer to the question of dt_drop_move: dropped and taken out of the window when it was yes */
EXPORT void dt_drop_move_poll( void )
{
	DTWIN	*d;
	ER	er;

	if ( !drop_asking.on ) {
		return;
	}
	er = wm_obj_asked(drop_asking.wid, drop_asking.seq);
	if ( er == E_TMOUT && (UW)( now_ms() - drop_asking.since ) < CARRY_ASK_MS ) {
		return;
	}
	drop_asking.on = FALSE;
	d = dt_showing(&drop_asking.from);
	if ( er >= E_OK ) {
		er = drop_give(d, drop_asking.wid, drop_asking.ax, drop_asking.ay, drop_asking.mods,
			       drop_asking.v, drop_asking.n);
	}
	tm_printf((UB *)"desktop: drop asked window %d: %d\n", drop_asking.wid, (INT)er);
	if ( er >= E_OK && d != NULL ) {
		drop_asking.gone(d, drop_asking.v, drop_asking.n);
	} else if ( er < E_OK ) {
		(void)wm_msg_put("このウインドウには置けません");
	}
	Kfree(drop_asking.v);
	drop_asking.v = NULL;
	wm_update();
}

/* A step of the start begun: said, and its time kept for the watch */
LOCAL void dt_begin( CONST char *step )
{
	dt_step_at = now_ms();
	dt_step = step;
	tm_printf((UB *)"TessronOS desktop: - %s\n", step);
}

/* How a task stands, in a few words: its state, what it waits for and on which object */
LOCAL void task_note( char *s, ID tskid )
{
	T_RTSK	r;

	if ( tskid <= 0 || tk_ref_tsk(tskid, &r) < E_OK ) {
		tm_sprintf((UB *)s, (UB *)"none");
		return;
	}
	tm_sprintf((UB *)s, (UB *)"state %x waits %x on %d", (UINT)r.tskstat, (UINT)r.tskwait, (INT)r.wid);
}

/*
 * The watch, from the task waiting for the desktop, once a second from
 * its start (ms after it): the state said every DT_WATCH_EVERY_MS up to
 * DT_WATCH_SAY_MS; a step before the loop that stands DT_STEP_STUCK_MS,
 * and the screen held DT_HELD_STUCK_MS, told once each with where the
 * tasks concerned wait.
 */
LOCAL void dt_watch( UW ms )
{
	static UW	said_at = 0;
	static CONST char *step_told = NULL;
	static BOOL	held_told = FALSE;
	CONST char	*step = dt_step;
	T_WMSTAT	ws;
	T_DISPSTAT	ds;
	ID		holder = 0;
	UW		held_ms = 0, stood;
	INT		held;
	char		a[48], b[48], why[96];

	held = knl_disp_hold_note(&holder, &held_ms);
	stood = now_ms() - dt_step_at;
	if ( ms <= DT_WATCH_SAY_MS && ms - said_at >= DT_WATCH_EVERY_MS ) {
		said_at = ms;
		if ( wm_stat(&ws) < E_OK ) knl_memset(&ws, 0, sizeof(ws));
		if ( ts_disp_stat(&ds) < E_OK ) knl_memset(&ds, 0, sizeof(ds));
		task_note(a, dt_tskid);
		task_note(b, knl_wm_compositor());
		tm_printf((UB *)"TessronOS desktop: at %d s: step %s (%d ms), loop %d; built %ld, sent %ld, "
			  "held %d (task %d, %d ms); desktop %s; compositor %s\n",
			  (INT)( ms / 1000 ), step, (INT)stood, (INT)dt_loops, (D)ws.composites, (D)ds.sends,
			  held, (INT)holder, (INT)held_ms, a, b);
	}
	if ( step[0] != 0 && knl_strcmp(step, "the loop") != 0 && stood >= DT_STEP_STUCK_MS && step_told != step ) {
		step_told = step;
		tm_sprintf((UB *)why, (UB *)"TessronOS desktop: the step \"%s\" has stood %d ms; the desktop's task:",
			   step, (INT)stood);
		knl_lockup_tell_task(dt_tskid, why);
		knl_lockup_tell_task(knl_wm_compositor(), "TessronOS desktop: the compositor's task:");
		if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
	}
	if ( held > 0 && held_ms >= DT_HELD_STUCK_MS ) {
		if ( !held_told ) {
			held_told = TRUE;
			tm_sprintf((UB *)why, (UB *)"TessronOS desktop: the screen held %d ms (%d holds), the first by task %d:",
				   (INT)held_ms, held, (INT)holder);
			knl_lockup_tell_task(holder, why);
			if ( knl_bootlog_soon != NULL ) knl_bootlog_soon();
		}
	} else {
		held_told = FALSE;
	}
}

/*
 * The machine going off or starting again: the programs' windows are asked to
 * close, as their close boxes ask, and they are given time to keep what they
 * hold. A program may ask the person whether to save, so the input goes on being
 * read here. When a program still has its window after DT_BLOCK_MS, a panel names
 * those that do and offers 取消, which goes back to the session with nothing
 * lost, and 強制的に終了, which goes on without them. It goes away by itself
 * when the last of them has closed. With no answer in DT_ASK_MS the machine is
 * not turned off: what is not saved is not lost for want of someone there, and
 * the session goes on. A program with no window left cannot hold the machine: it
 * has until DT_END_MS to finish what it keeps. Then every process left is told to
 * end and after that ended by force (knl_prc_end_all).
 */
#define DT_BLOCK_MS	5000		/* a window still there: its program is named */
#define DT_END_MS	20000		/* for a program whose window has gone to finish */
#define DT_ASK_MS	180000		/* the panel with no answer: the going off is given up */
#define DT_TERM_MS	10000		/* for the processes told to end */
#define DT_OFF_MS	90000		/* for a step of going off, before the machine goes off regardless */
#define DT_STUCK_MS	60000		/* the desktop's task not coming round: what it should do is done without it */
#define DT_BLOCK_LINES	6		/* programs named on the panel */

LOCAL volatile UW	dt_end_alive = 0;	/* when programs_end last came round */
LOCAL BOOL		dt_ended_all = FALSE;	/* knl_prc_end_all has been done */
LOCAL INT		dt_off_pid = -1;	/* the panel naming the programs */
LOCAL UINT		dt_off_ans = WM_ANS_NONE;

LOCAL void programs_tick( void )
{
	T_WMEV	ev;
	ER	er;

	dt_end_alive = now_ms();
	knl_wdog_alive();
	close_requests();
	process_requests();
	dt_uitest_poll();
	/* input for a program's window goes to it from inside this, a question of its own included */
	er = wm_read_event(&ev, 50);
	if ( er == E_NOEXS ) {
		tk_dly_tsk(50);
	} else if ( er >= E_OK && dt_off_pid >= 0 && ev.type != HID_EV_KEY_DOWN && ev.type != HID_EV_KEY_UP ) {
		if ( wm_panel_event(dt_off_pid, &ev, &dt_off_ans) >= E_OK ) {
			wm_update();
		}
	}
}

/* The panel over the first window, naming those of pids[n] that have a window: how many there are */
LOCAL INT off_panel( CONST ID *pids, INT n )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	DTWIN		*d = dt_win_of(dt_first_wid);
	char		name[64], more[64];
	INT		i, k = 0, w = 480, h, nblock = 0;

	for ( i = 0; i < n; i++ ) {
		if ( wm_obj_program_window(pids[i]) > 0 ) nblock++;
	}
	h = 24 * ( ( nblock < DT_BLOCK_LINES ? nblock : DT_BLOCK_LINES + 1 ) + 2 ) + 56;
	def = ( d != NULL ) ? dt_pn_new(w, h) : NULL;
	if ( def == NULL ) {
		return -1;
	}
	def->kind = WM_PNL_WARN;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 10, w - 28, 22, "次のプログラムがまだ終わっていません。");
	for ( i = 0; i < n; i++ ) {
		INT	wid = wm_obj_program_window(pids[i]);

		if ( wid <= 0 ) continue;
		name[0] = 0;
		(void)wm_title(wid, name, sizeof(name));
		if ( name[0] == 0 ) (void)knl_prc_name(pids[i], (UB *)name, sizeof(name));
		tm_printf((UB *)"shutdown: pid %d %s has not closed\n", (INT)pids[i], name);
		if ( k < DT_BLOCK_LINES ) {
			dt_pn_add(def, WM_PT_LABEL, 0, 30, 34 + k * 24, w - 44, 22, name);
		} else if ( k == DT_BLOCK_LINES ) {
			tm_sprintf((UB *)more, (UB *)"ほか%d個", nblock - DT_BLOCK_LINES);
			dt_pn_add(def, WM_PT_LABEL, 0, 30, 34 + k * 24, w - 44, 22, more);
		}
		k++;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, h - 62, w - 28, 22, "強制的に終了すると、保存していない内容は失われます。");
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_CANCEL, w - 262, h - 36, 94, 26, "取消");
	if ( pt != NULL ) pt->answer = WM_ANS_CANCEL;
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_OK, w - 158, h - 36, 144, 26, "強制的に終了");
	if ( pt != NULL ) pt->answer = WM_ANS_OK;
	dt_off_ans = WM_ANS_NONE;
	dt_off_pid = wm_panel_open_centre(d->wid, def);
	Kfree(def);
	if ( dt_off_pid < 0 ) {
		return -1;
	}
	wm_panel_draw(dt_off_pid);
	wm_update();
	return nblock;
}

/* The panel taken away */
LOCAL void off_panel_done( void )
{
	if ( dt_off_pid >= 0 ) {
		dt_pn_done(dt_win_of(dt_first_wid), dt_off_pid);
		dt_off_pid = -1;
	}
}

/* Whether the machine goes on going off: FALSE when the session is to go on */
LOCAL BOOL programs_end( void )
{
	ID	*pids = NULL;
	INT	max, n, i, left = 0, shown = 0;
	UW	t0 = now_ms(), panel_at = 0;
	BOOL	forced = FALSE, cancel = FALSE;

	max = knl_prc_list_all(&pids);
	n = ( pids != NULL ) ? wm_obj_close_programs(pids, max) : 0;
	wm_update();
	for ( ;; ) {
		UW	now = now_ms();
		INT	block = 0;

		left = 0;
		for ( i = 0; i < n; i++ ) {
			T_PRCINFO	pi;

			if ( knl_prc_info(pids[i], &pi) >= E_OK && !pi.done ) {
				left++;
				if ( wm_obj_program_window(pids[i]) > 0 ) block++;
			}
		}
		knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
		if ( left == 0 ) {
			break;
		}
		if ( block == 0 && dt_off_pid >= 0 ) {
			off_panel_done();	/* the last of them closed: the person need not answer */
			shown = 0;
		} else if ( block > 0 && (UW)( now - t0 ) >= DT_BLOCK_MS && ( dt_off_pid < 0 || block != shown ) ) {
			off_panel_done();
			if ( panel_at == 0 ) panel_at = now;
			shown = off_panel(pids, n);
			if ( shown < 0 ) {
				tm_printf((UB *)"shutdown: no panel to ask with: the going off is given up\n");
				cancel = TRUE;
				break;
			}
		}
		if ( dt_off_pid >= 0 && dt_off_ans == WM_ANS_OK ) {
			tm_printf((UB *)"shutdown: the person chose to end them by force\n");
			forced = TRUE;
			break;
		}
		if ( dt_off_pid >= 0 && dt_off_ans != WM_ANS_NONE ) {
			tm_printf((UB *)"shutdown: the person cancelled the going off\n");
			cancel = TRUE;
			break;
		}
		if ( panel_at != 0 && block > 0 && (UW)( now - panel_at ) >= DT_ASK_MS ) {
			tm_printf((UB *)"shutdown: no answer in %d ms: the going off is given up\n", DT_ASK_MS);
			cancel = TRUE;
			break;
		}
		if ( block == 0 && (UW)( now - t0 ) >= DT_END_MS ) {
			break;
		}
		programs_tick();
		wm_update();
	}
	off_panel_done();
	if ( pids != NULL ) Kfree(pids);
	if ( cancel ) {
		knl_off_limit(0, 0);
		(void)wm_msg_put("OSの終了を取り消しました。");
		(void)wm_msg_show(TRUE);
		wm_update();
		return FALSE;
	}
	tm_printf((UB *)"TessronOS desktop: %d programs asked to end, %d not ended, after %d ms%s\n",
		  n, left, (INT)( now_ms() - t0 ), forced ? " (by force)" : "");

	knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
	(void)knl_prc_end_all(forced ? 1000 : DT_TERM_MS, programs_tick);
	dt_ended_all = TRUE;
	dt_end_alive = now_ms();
	knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
	return TRUE;
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
	/* a window with no icon of its own shows the gear of the settings in colour */
	{
		TS_UUID	gear;
		UB	*file;
		UW	*px = NULL;
		SZ	size = 0;
		INT	w = 0, h = 0;

		file = ( ts_str_to_uuid(SYSDEF_PROG_SYSENV, &gear) >= E_OK ) ? om_obj_icon(&gear, &size) : NULL;
		if ( file != NULL && img_decode(file, size, &px, &w, &h) >= E_OK && px != NULL ) {
			(void)wm_set_icon_default(px, w, h);
			Kfree(px);
		} else {
			tm_printf((UB *)"TessronOS desktop: no icon for windows without one\n");
		}
		if ( file != NULL ) Kfree(file);
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
	sysdef_names();				/* the system's containers by their names of now */
	knl_sys_mark();				/* the root's links known by their relationships */
	(void)wm_conf_apply();			/* the person's settings (ユーザ環境設定) */
	(void)wm_dev_conf_apply();		/* the machine's (システム環境設定) */
	video_start();				/* the screen's size, before any window */
	scale_start();				/* and the scale it is drawn at */
	(void)wm_conf_watch();			/* the network's behind, and each again when written */
	knl_obsys_power((FP)dt_power);		/* the system object's power record comes here */
	(void)dt_prog_start();			/* the programs, as real objects */
	wm_obj_painter(tv_paint, tv_shape);	/* what a window's drawing and shape records show */
	port_open();
	dt_tail_start();			/* 小物 and 実行 in the programs' menus */
	dt_fault_forget();			/* exceptions before the desktop was up: the console said them */
	dt_begin("mozc");
	(void)kc_start();			/* かな漢字変換: mozc starts behind */
	dt_begin("the wallpaper");
	if ( dt_res_wall(WM_WALL_FIT) < E_OK ) {	/* a picture of the 壁紙箱 */
		tm_printf((UB *)"TessronOS desktop: no wallpaper\n");
	}
	dt_begin("the message line");
	(void)wm_msg_show(TRUE);		/* the message line along the foot */
	dt_begin("the input modes");
	tip_start();

	/*
	 * The cabinet to open with. Which one that is belongs in the
	 * system's own settings; until there are settings, the one the
	 * records this system was given open with is named here.
	 */
	dt_begin("the cabinet");
	if ( ts_str_to_uuid(DT_CABINET, &cab) < E_OK
	  || ( cwid = win_open(&cab, 0, 60, 40, NULL) ) < 0 ) {
		tm_printf((UB *)"TessronOS desktop: no cabinet to open\n");
	} else {
		dt_first_wid = cwid;
		auto_open(dt_win_of(cwid));
	}
	dt_begin("the first screen");
	wm_composite();
	dt_begin("the screen's size confirmed");
	video_confirm(dt_win_of(dt_first_wid));
	dt_begin("the disk repair's result");
	dt_fix_boot();				/* the system volume repaired at this start: what came of it */
	dt_begin("a system update left");
	dt_sysup_resume(dt_win_of(dt_first_wid));	/* a system update the last start did not end */
	dt_begin("the loop");

    the_session:
	while ( !dt_stop ) {
		DTWIN	*d;
		INT	px = 0, py = 0;
		UINT	btn = 0;

		if ( dt_port <= 0 || dt_req <= 0 ) {
			port_open();		/* one that could not be made at the start */
		}
		knl_wdog_alive();		/* the loop came round (lockup.c tells one that does not) */
		dt_loops++;
#if USE_KTEST
		/* the tests stop the loop here: 1 waits, 2 runs on the spot */
		while ( dt_test_stall == 1 ) {
			tk_dly_tsk(20);
		}
		while ( dt_test_stall == 2 ) {
			;
		}
#endif
		close_requests();
		process_requests();
		dt_fault_poll();		/* a process ended by an exception: told in a dialog */
		own_poll();			/* a slide's program asked for something */
		dt_rthost_poll();		/* a player in a process's window: gone with the window */
		dt_uitest_poll();		/* nothing unless built with UITEST=1 */
		dt_tail_poll();			/* an accessory chosen in a program's menu */
		dt_winlist_poll();		/* ウインドウ一覧, when the windows changed */
		df_panel_poll();		/* 道具パネル: with the figure in use */
		dt_scr_poll();			/* 画面印刷: its panel, and the count down */
		dt_fix_poll();			/* ディスク修復: the check followed */
		dt_disk_poll();			/* disks attached, taken away, taken out, come back */
		dt_type_poll();			/* where the keys went, for DT_RQ_TYPE */
		carry_ask_poll();		/* a program asked whether it takes what was carried */
		dt_drop_move_poll();		/* and whether it takes what a window of the desktop's carried */
		if ( wm_layout_serial() != dt_layout ) {
			/* the frames were measured again: every window's page with them */
			INT	i;

			dt_layout = wm_layout_serial();
			for ( i = 0; i < DT_MAX_WIN; i++ ) {
				if ( dt_win[i].used ) {
					dt_draw(&dt_win[i]);
				}
			}
			wm_update();
		}
		/* with paragraphs still to lay, they are laid between the events */
		if ( wm_read_event(&ev, dd_settle_pending() ? 0 : DT_BLINK_MS / 2) < E_OK ) {
			dt_uitest_idle();	/* no input left: what was put in has been taken */
			if ( (UW)( now_ms() - dt_last_input ) >= DT_PRE_IDLE_MS
			  && ed_prepare_pending() ) {
				/* nothing done for a while: the next change's "as it was" written out */
				ed_prepare_step();
			} else if ( dd_settle_pending() ) {
				dd_settle_step();
			}
			blink_step();
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_BTN_DOWN ) {
			dt_last_input = now_ms();
		}
		if ( ev.code == 0 && ( ev.type == HID_EV_BTN_DOWN || ev.type == HID_EV_BTN_UP ) ) {
			/* where the first button went up, for a carry asked after it (carry_start) */
			UINT	b = 0;

			dt_up_seen = (BOOL)( ev.type == HID_EV_BTN_UP
					     && ts_hid_pointer(&dt_up_x, &dt_up_y, &b) >= E_OK );
		}
		if ( ev.type == HID_EV_BTN_DOWN || ev.type == HID_EV_KEY_DOWN ) {
			/* what was just done shows at once, and for a whole turn */
			ed_blink_show();
			blink_restart();
			(void)wm_msg_put(NULL);	/* a message stays until the next thing done */
		} else {
			blink_step();
		}
		/* 画面印刷 first: PrintScreen, and a rectangle being dragged out (dtscr.c) */
		if ( dt_scr_event(&ev) ) {
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
#ifdef USE_UITEST
			drag_t0 = 0;
			drag_timed = (BOOL)( !carry.on && ( ed_holding() || df_drawing_now() ) );
			(void)ts_get_mono(&drag_t0);
#endif
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
					if ( dt_carry_window(dt_move_wid, &o) ) {
						DTWIN	*m = dt_win_of(dt_move_wid);

						if ( m != NULL && dt_sizing ) {
							dt_draw(m);
						}
						wm_update();
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
						wm_update();	/* the knob alone */
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
					wm_update();
				}
			}
#ifdef USE_UITEST
			if ( drag_timed ) {
				drag_moved(drag_t0, ev.when);
			}
#endif
			if ( dt_own_held != 0 ) {
				(void)own_give(dt_own_held, &ev);	/* the button went down in a part's window */
			} else if ( d != NULL && d->own != NULL && dt_move_wid == 0 ) {
				(void)own_give(d->wid, &ev);		/* the pointer over a part's window */
			} else if ( d != NULL && !ed_holding() && !df_drawing_now()
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
#ifdef USE_UITEST
			if ( ev.code == 0 ) {
				drag_said();
			}
#endif
			if ( carry.on ) {
				/* a link carried out of a program's window, let go */
				if ( ev.code == 0 && ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
					dt_up_mods = ev.mods;
					carry_release(px, py);
				}
				break;
			}
			if ( dt_own_held != 0 ) {
				/* let go of what was pressed in a part's window, wherever the pointer is */
				INT	held = dt_own_held;

				dt_own_held = 0;
				(void)own_give(held, &ev);
				break;
			}
			if ( ev.code != 0 && ( ed_holding() || dd_link_carrying() || dd_sel_carrying() ) ) {
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
			if ( dt_move_wid != 0 && dt_carry_end(dt_move_wid) ) {
				DTWIN	*m = dt_win_of(dt_move_wid);

				if ( m != NULL && dt_sizing ) {
					dt_draw(m);
				}
			}
			if ( dt_hold_wid != 0 || dt_move_wid != 0 ) {
				wm_set_pointer(WM_PT_SELECT);
				wm_update();
			}
			dt_hold_wid = 0;
			dt_move_wid = 0;
			if ( dd_sel_carrying() && ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
				dd_sel_release(px, py);	/* selected letters carried, let go */
			}
			dd_release();
			df_tool_release();
			if ( df_end_carrying() ) {
				df_end_release();
				wm_update();
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
			df_panel_release(&ev);
			if ( ed_holding() ) {
				if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
					ed_release(px, py);
				}
			}
			break;

		case HID_EV_BTN_DOWN:
			if ( carry.on ) {
				/* another button, while a program's link is carried: the second makes it a copy */
				if ( ev.code == 1 ) {
					carry.copy = TRUE;
				}
				break;
			}
			if ( df_panel_press(&ev) ) {
				break;
			}
			if ( ev.code == 1 && ( ed_holding() || dd_link_carrying() || dd_sel_carrying() ) ) {
				/* 複写ドラッグ, over any window: what is carried goes as a copy */
				ed_hold_copy();
				dd_link_copy();
				dd_sel_copy();
				break;
			}
			if ( d == NULL ) {
				obj_press(&ev);		/* a program's own window */
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

		case HID_EV_KEY_UP:
			{
				/* a key let go: only a part may want it (a program played that waits for it) */
				DTWIN	*f = dt_win_of(wm_focused());

				if ( f != NULL && f->own != NULL ) {
					T_WMEV	e = ev;

					e.wid = f->wid;
					(void)own_give(f->wid, &e);
				}
			}
			break;

		case HID_EV_WHEEL:
			wheel(d, &ev);		/* a program's own window is told by its notice */
			break;

		default:
			break;
		}
	}
	if ( dt_poweroff && !programs_end() ) {
		dt_poweroff = FALSE;
		dt_restart = FALSE;
		dt_stop_at = 0;
		dt_stop = FALSE;	/* last: usermain goes back to watching on seeing this */
		goto the_session;
	}
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
	/* what it makes as the person works is the person's (design 8.7) */
	knl_ob_make_for_person(TRUE);
	knl_wdog_watch(tk_get_tid());		/* a loop that stops coming round is told (lockup.c) */
	desktop_run();
	knl_wdog_watch(0);
	knl_ob_make_for_person(FALSE);
	dt_tail_stop();
	port_close();
	dt_tskid = 0;
	tk_exd_tsk();
}

/*
 * What 強制終了 is told of the desktop (include/ts/fquit.h): its task,
 * the windows with changes not saved, and how it is started again.
 * The windows are read without a lock: the desktop may have stopped
 * anywhere, and this is asked from another task only to say what will
 * be lost.
 */
LOCAL ID fq_task_of( void )
{
	return dt_tskid;
}

LOCAL INT fq_unsaved_of( UB *names, INT max )
{
	INT	i, n = 0, k = 0, j;

	if ( names != NULL && max > 0 ) {
		names[0] = 0;
	}
	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		DTWIN	*d = &dt_win[i];

		if ( !d->used || !d->dirty ) {
			continue;
		}
		n++;
		if ( names == NULL || max <= 0 ) {
			continue;
		}
		if ( n > 1 && k + 4 < max ) {
			for ( j = 0; "、"[j] != 0; j++ ) names[k++] = (UB)"、"[j];
		}
		for ( j = 0; j < TAD_NAME_MAX && d->name[j] != 0 && k < max - 1; j++ ) {
			names[k++] = d->name[j];
		}
		/* never half a character */
		while ( k > 0 && k >= max - 1 && ( names[k - 1] & 0xC0 ) == 0x80 ) {
			k--;
		}
		names[k] = 0;
	}
	return n;
}

LOCAL CONST T_FQDESK	dt_fq_ops = { fq_task_of, fq_unsaved_of, dt_start };

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
	knl_fq_desktop(&dt_fq_ops);		/* 強制終了 knows it from now on */
	dt_stop = FALSE;
	dt_quit_asked = FALSE;
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

	dt_quit_asked = TRUE;
	dt_stop = TRUE;
	for ( i = 0; i < 100 && dt_tskid > 0; i++ ) {
		tk_dly_tsk(100);
	}
}

EXPORT INT ts_desktop( void )
{
	UINT		sw, sh;
	UW		t0;

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
	 * because returning from usermain shuts the machine down. It kicks
	 * the watchdog as it waits: when this task, the first of all and
	 * above every other, is not let run for twenty seconds, the machine
	 * has stopped (lockup.c), and is reset after the time set.
	 */
	t0 = now_ms();
    the_watch:
	while ( !dt_stop ) {
		knl_wdog_kick();
		tk_dly_tsk(1000);
		dt_watch(now_ms() - t0);
	}
	/* a person may still cancel the going off: the session then goes on, and this with it */
	if ( dt_poweroff ) {
		dt_end_alive = now_ms();
		knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
		while ( dt_stop && dt_tskid > 0 && (UW)( now_ms() - dt_end_alive ) < DT_STUCK_MS ) {
			knl_wdog_kick();
			tk_dly_tsk(100);
		}
		if ( !dt_stop ) {
			goto the_watch;
		}
	}
	knl_wdog_kick_stop();			/* what follows takes as long as it takes */
	t0 = ( dt_stop_at != 0 ) ? dt_stop_at : now_ms();
	/*
	 * Turned off from the desktop: its windows are saved and closed by
	 * its own task on the way out, which is waited for, and which also
	 * sees every process end. Only after that are the file systems
	 * written out, each told on the console, with nothing left to write
	 * after them. A task that stops coming round is not waited for, and
	 * its part is done here. What could not be written is told on the
	 * message line too; the volumes are put away as the kernel goes down
	 * (knl_fs_finish).
	 */
	if ( dt_poweroff ) {
		if ( dt_tskid > 0 ) {
			knl_lockup_tell_task(dt_tskid, "TessronOS desktop: its task is not coming round:");
		} else {
			tm_printf((UB *)"TessronOS desktop: its task ended %d ms after the session was asked to end\n",
				  (INT)( now_ms() - t0 ));
		}
		if ( !dt_ended_all ) {
			(void)knl_prc_end_all(DT_TERM_MS, NULL);
		}
		knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
		if ( knl_fs_sync_report() > 0 ) {
			(void)wm_msg_put("書き込めなかったボリュームがあります。内容を確認してください。");
			(void)wm_msg_show(TRUE);
			(void)wm_composite();
			tk_dly_tsk(3000);
		}
		tm_printf((UB *)( dt_restart ? "TessronOS desktop: starting the machine again\n"
					     : "TessronOS desktop: turning the machine off\n" ));
	}

	return dt_restart ? -1 : 0;		/* usermain's: -1 resets and starts again */
}
