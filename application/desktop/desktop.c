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
#include <ts/disp.h>
#include <ts/conf.h>
#include <ts/cabmenu.h>
#include <ts/kconv.h>
#include <ts/ob.h>
#include <ts/dtreq.h>
#include <ts/time.h>
#include <ts/rtmedia.h>
#include <ts/rtad.h>
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
#define DT_SETTLE_MS		120000	/* what the start opened not up after this long: told as such */

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
}

/* What is in a window of a part of the desktop drawn: the part draws it */
EXPORT void dt_draw( DTWIN *d )
{
	if ( d->own != NULL && wm_gid(d->wid) >= 0 ) {
		d->own->draw(d);	/* the part's own work area */
	}
}

/* The loop wakes at least this often, for what it does while idle */
#define DT_WAKE_MS	100
#define DT_PRE_IDLE_MS	300		/* nothing pressed so long: idle work begins */

LOCAL UW	dt_last_input = 0;		/* when a key or a button was last pressed */

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);

	return (UW)t.lo;
}

/*
 * Every window drawn again: the parts' here, and each program's window
 * told to draw itself again (OB_E_REDRAW), as when its size changes.
 * What a volume attached or gone changes -- a link drawn pale (虚身) or
 * not -- is shown in every window that has such a link.
 */
IMPORT void knl_wmobj_moved( INT wid, BOOL resized );

EXPORT void dt_draw_all( void )
{
	INT	i, n, *w;

	for ( i = 0; i < DT_MAX_WIN; i++ ) {
		if ( dt_win[i].used ) {
			dt_draw(&dt_win[i]);
		}
	}
	w = (INT *)Kmalloc(sizeof(INT) * 256);
	n = ( w != NULL ) ? wm_list(w, 256) : 0;
	for ( i = 0; i < n && i < 256; i++ ) {
		if ( dt_win_of(w[i]) == NULL && wm_obj_maker(w[i]) > 0 ) {
			knl_wmobj_moved(w[i], TRUE);
		}
	}
	if ( w != NULL ) Kfree(w);
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
 * its kind: 仮身一覧 shows a figure's links and opens nothing but a
 * figure, so a record that is a text is given to the program of texts
 * (基本文章編集) instead.
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
LOCAL void auto_req( CONST T_DTREQ *rq, T_VOBJ *v );
LOCAL void first_auto_done( ID asker, INT n );
LOCAL ER first_close_req( CONST T_DTREQ *rq );

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
		r.flags = DT_F_DUPLICATE | DT_F_BASE;
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
		/* a picture or a figure out of a text: no window of the desktop's takes one */
		(void)wm_msg_put("ここには置けません");
		wm_update();
		carry_answer(E_NOEXS, NULL);
		return;
	}
	if ( ( carry.rq.flags & DT_F_TEXT ) != 0 ) {
		/* letters: onto a program's window; no window of the desktop's takes them */
		INT	n;

		carry.rq.text[DT_CARRY_TEXT - 1] = 0;
		for ( n = 0; carry.rq.text[n] != 0; n++ ) ;
		if ( n > 0 && t == NULL && wm_at(px, py) > 0 ) {
			/* another program's window: asked first for a move, offered for a copy */
			if ( carry.copy ) {
				carry_text_to_prog(wm_at(px, py), px, py, FALSE);
			} else {
				carry_ask(wm_at(px, py), px, py);
			}
			return;
		}
		(void)wm_msg_put("ここには置けません");
		wm_update();
		carry_answer(E_NOEXS, NULL);
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
	/* the ground, or a window of a part of the desktop's: no window there takes links */
	(void)wm_msg_put("ここには置けません");
	carry_answer(E_NOEXS, NULL);
}

/*
 * 実身複製ドラッグ: the object copied, under a name asked for over t. A
 * template (tmpl: carried out of a 原紙箱 or 原紙集め, DT_F_BASE) makes a
 * new object of the person's, its name starting with the program's for
 * a program's template, else with the template's own (ユーザ原紙)
 */
LOCAL ER carry_duplicate( DTWIN *t, CONST TS_UUID *target, BOOL tmpl, TS_UUID *copy )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX], own[TAD_NAME_MAX];
	CONST char	*base = dt_base_name(target);
	INT		k = 0, i;

	if ( base == NULL && tmpl && om_store_name(target, own, TAD_NAME_MAX) >= 0 ) {
		base = (CONST char *)own;
	}
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
		dt_template_made(target, copy);	/* made by the user, from the template */
		tm_printf((UB *)"desktop: %s made from the template %s\n", name, base);
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
		er = carry_duplicate(host, &rq->target, (BOOL)( ( rq->flags & DT_F_BASE ) != 0 ), &target);
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
			dt_link_fit(c);
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
	if ( !carry.pending || rq->carry != carry.seq || asker != carry.asker ) {
		return E_OBJ;
	}
	if ( !more ) {
		carry.pending = FALSE;
	}
	if ( carry.toprog ) {
		return carry_prog_place(rq, more, p_into);
	}
	return E_NOEXS;				/* let go where no window takes links */
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
 * Windows opened from a program's link that asked to be told how the
 * link looks (DT_RQ_LINK_WATCH): when the program that holds the link
 * changes it by 仮身属性変更 (DT_RQ_ATTR with DT_F_LINK), each is told
 * the link's new look at its channel (DT_HOW_LINK_LOOK) before the
 * record is saved, and the look is kept with the entry: asked again
 * (〈再表示〉 of the window), the answer gives the last one told. An
 * entry goes when its window's process has ended or its channel is
 * gone; one link may have several windows.
 */
#define LWATCH_MAX	64

typedef struct {
	BOOL	used;
	TS_UUID	parent;			/* the object whose record holds the link */
	TS_UUID	vobjid;			/* the link there */
	TS_UUID	target;			/* the object the window shows */
	TS_UUID	chan;			/* where the window's program is told */
	TS_UUID	proc;			/* that program's process */
	UINT	age;
	BOOL	has_look;		/* look told since the window opened */
	T_DTLOOK look;
} LWATCH;

LOCAL LWATCH	lwatch[LWATCH_MAX];
LOCAL UINT	lwatch_age = 0;

/* An entry whose process has ended let go: FALSE then */
LOCAL BOOL lwatch_live( LWATCH *w )
{
	if ( w->used && knl_prc_of_uuid(&w->proc) <= 0 ) {
		w->used = FALSE;
	}
	return w->used;
}

/*
 * DT_RQ_LINK_WATCH: the window of the asking process told of the link it
 * was opened from; the answer's look the last told of that link since,
 * how DT_HOW_LINK_LOOK, when there is one
 */
LOCAL ER lwatch_req( T_DTREQ *rq, T_DTANS *an, ID asker )
{
	PLINK	e;
	INT	i, k = -1;
	BOOL	again = FALSE;

	if ( !plink_take(rq, asker, &e) ) {
		return E_PAR;
	}
	for ( i = 0; i < LWATCH_MAX; i++ ) {
		LWATCH	*w = &lwatch[i];

		if ( !lwatch_live(w) ) {
			if ( !again && ( k < 0 || lwatch[k].used ) ) k = i;
			continue;
		}
		if ( ts_uuid_cmp(&w->chan, &e.chan) == 0 ) {
			k = i;			/* the same window asking again: as it asks now */
			again = TRUE;
			continue;
		}
		if ( !again && ( k < 0 || ( lwatch[k].used && w->age < lwatch[k].age ) ) ) k = i;
	}
	/* a new window has the link's look as it was opened (T_PRCOPEN): none told yet */
	if ( !again || ts_uuid_cmp(&lwatch[k].parent, &e.parent) != 0 || ts_uuid_cmp(&lwatch[k].vobjid, &e.vobjid) != 0 ) {
		lwatch[k].has_look = FALSE;
	}
	lwatch[k].used = TRUE;
	lwatch[k].parent = e.parent;
	lwatch[k].vobjid = e.vobjid;
	lwatch[k].target = e.target;
	lwatch[k].chan = e.chan;
	lwatch[k].proc = e.proc;
	lwatch[k].age = ++lwatch_age;
	if ( lwatch[k].has_look ) {
		an->how = DT_HOW_LINK_LOOK;
		an->look = lwatch[k].look;
	}
	return E_OK;
}

/* The new look of link vobjid of parent, pointing at target, told to the windows opened from it */
LOCAL void lwatch_tell( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target,
			CONST T_DTLOOK *look )
{
	T_DTANS	*an = (T_DTANS *)Kmalloc(sizeof(T_DTANS));
	PLINK	ids;
	SZ	asz = 0;
	INT	i;
	ID	k;

	if ( an == NULL ) {
		return;
	}
	knl_memset(an, 0, sizeof(*an));
	knl_memset(&ids, 0, sizeof(ids));
	ids.parent = *parent;
	ids.vobjid = *vobjid;
	an->er = E_OK;
	an->how = DT_HOW_LINK_LOOK;
	an->into = *target;
	an->look = *look;
	(void)plink_ids(an->text, &ids);
	for ( i = 0; i < LWATCH_MAX; i++ ) {
		LWATCH	*w = &lwatch[i];

		if ( !lwatch_live(w) || ts_uuid_cmp(&w->parent, parent) != 0
		  || ts_uuid_cmp(&w->vobjid, vobjid) != 0 || ts_uuid_cmp(&w->target, target) != 0 ) {
			continue;
		}
		k = ob_opn_obj(&w->chan, OB_OP_WRITE | OB_O_NOWAIT);
		if ( k <= 0 ) {
			w->used = FALSE;	/* the window's channel is gone */
			continue;
		}
		w->has_look = TRUE;
		w->look = *look;
		if ( ob_wri_rec(k, 0, 0, an, sizeof(*an), &asz) >= E_OK ) {
			knl_ob_post(&w->chan, 0, OB_E_CHANGE, NULL);
		}
		(void)ob_cls_obj(k);
	}
	Kfree(an);
}

/*
 * DT_RQ_LET_GO: what a program carried within its window, let go outside
 * it. Over another program's window that shows an object (the first
 * window, a cabinet, a text), nothing goes there and nothing moves: that
 * window takes the keys, as when links are carried there, and the answer
 * names its object. Elsewhere (the ground) E_NOEXS.
 */
LOCAL ER let_go_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	INT	wid = wm_at(rq->grab_x, rq->grab_y);
	TS_UUID	u;

	if ( wid <= 0 || dt_win_of(wid) != NULL || wid == wm_obj_wid(&rq->win)
	  || wm_obj_maker(wid) <= 0 || !wm_obj_shown(wid, &u) ) {
		return E_NOEXS;
	}
	wm_raise(wid);
	(void)wm_focus(wid);
	wm_update();
	an->into = u;
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

/*
 * The requests are read as they come by a task of their own, which
 * wakes the loop (ts_hid_wake): a program's request is answered at once,
 * not when the loop next comes round, and a program that asks several
 * things one after another (the items of a menu it opens) does not wait
 * a turn of the loop for each. The loop takes them from the queue the
 * task puts them in, each with the process that wrote it.
 */
#define REQ_Q		16
#define REQ_WAIT_MS	200

typedef struct {
	T_DTREQ	rq;
	SZ	size;
	ID	asker;
} DTREQQ;

LOCAL DTREQQ	dt_reqq[REQ_Q];
LOCAL UINT	dt_reqq_in = 0, dt_reqq_out = 0;	/* put by the task, taken by the loop */
LOCAL ID	dt_req_tsk = 0;
LOCAL volatile BOOL dt_req_run = FALSE;
LOCAL ID	dt_asker = 0;			/* the process of the request being done */

LOCAL void req_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	while ( dt_req_run ) {
		UINT	in = __atomic_load_n(&dt_reqq_in, __ATOMIC_ACQUIRE);
		DTREQQ	*q = &dt_reqq[in % REQ_Q];
		ID	k = dt_req;
		SZ	asz = 0;

		if ( k <= 0 || in - __atomic_load_n(&dt_reqq_out, __ATOMIC_ACQUIRE) >= REQ_Q ) {
			tk_dly_tsk(20);		/* no channel yet, or the queue full until the loop takes some */
			continue;
		}
		if ( ob_rea_tmo(k, 0, 0, &q->rq, sizeof(q->rq), &asz, REQ_WAIT_MS) < E_OK ) {
			continue;
		}
		q->size = asz;
		q->asker = knl_obchan_last_from(&dt_req_u);
		__atomic_store_n(&dt_reqq_in, in + 1, __ATOMIC_RELEASE);
		ts_hid_wake();
	}
	tk_exd_tsk();
}

LOCAL void req_start( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	if ( dt_req_tsk > 0 ) {
		return;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = req_task;
	ctsk.itskpri = 20;
	ctsk.stksz   = 8192;
	dt_req_run = TRUE;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid < E_OK || tk_sta_tsk(tskid, 0) < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: no task to read the requests (%d)\n", (INT)tskid);
		if ( tskid > 0 ) tk_del_tsk(tskid);
		dt_req_run = FALSE;
		return;
	}
	dt_req_tsk = tskid;
}

/* The task let go, after the read it is in has timed out */
LOCAL void req_stop( void )
{
	T_RTSK	r;
	INT	i;

	if ( dt_req_tsk <= 0 ) {
		return;
	}
	dt_req_run = FALSE;
	for ( i = 0; i < 50; i++ ) {
		if ( tk_ref_tsk(dt_req_tsk, &r) < E_OK || r.tskstat == TTS_DMT ) break;
		tk_dly_tsk(20);
	}
	(void)tk_del_tsk(dt_req_tsk);
	dt_req_tsk = 0;
}

/* The next request the task read: FALSE when there is none */
LOCAL BOOL req_take( T_DTREQ *rq, SZ *p_size )
{
	UINT	out = __atomic_load_n(&dt_reqq_out, __ATOMIC_ACQUIRE);

	if ( out == __atomic_load_n(&dt_reqq_in, __ATOMIC_ACQUIRE) ) {
		return FALSE;
	}
	*rq = dt_reqq[out % REQ_Q].rq;
	*p_size = dt_reqq[out % REQ_Q].size;
	dt_asker = dt_reqq[out % REQ_Q].asker;
	__atomic_store_n(&dt_reqq_out, out + 1, __ATOMIC_RELEASE);
	return TRUE;
}

LOCAL void process_requests( void )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_VOBJ		v;
	SZ		asz = 0;
	INT		i, n, keys_were = 0;
	SZ		anssz;
	BOOL		quiet;

	while ( req_take(&rq, &asz) ) {
		/*
		 * What is opened while the request is done -- a panel, the
		 * page of 印刷 -- is the asking process's to 強制終了 (5.5.1).
		 */
		knl_wmobj_act_for(dt_asker);
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
		if ( rq.req == DT_RQ_OPEN && ( rq.flags & DT_F_AUTO ) != 0 ) {
			TS_UUID	none;

			knl_memset(&none, 0, sizeof(none));
			if ( ts_uuid_cmp(&rq.target, &none) == 0 ) {
				/* the asking window's 自動起動 all asked */
				first_auto_done(dt_asker, rq.grab_y);
				answer_as(&rq.reply, &an, anssz);
				continue;
			}
		}
		if ( rq.req == DT_RQ_OPEN || rq.req == DT_RQ_TOOL ) {
			ID	asker = dt_asker;

			dt_prog_reply(&rq.reply, rq.seq, anssz, (BOOL)( rq.req == DT_RQ_TOOL ));
			/*
			 * a program without the keys is not being worked, and what
			 * a window opens by itself is opened by no act of the
			 * person: what it asks for opens without them
			 */
			if ( ( asker > 0 && wm_obj_maker(wm_focused()) != asker )
			  || ( rq.req == DT_RQ_OPEN && ( rq.flags & DT_F_AUTO ) != 0 ) ) {
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
			  && plink_take(&rq, dt_asker, &plink_now) ) {
				INT	i;

				plink_put(&plink_now);
				plink_opening = TRUE;
				v.vobjid = plink_now.vobjid;
				/*
				 * the volume its object was saved on, as the record the
				 * program saved says it: a link to a disk taken
				 * away says which one to attach (dt_disk_absent)
				 */
				{
					/* read as stored: the program wrote it, past what the store kept of it */
					SZ	psz = 0;
					UB	*px = om_obj_record(&plink_now.parent, 0, &psz);
					T_TAD	*prec = NULL;
					T_VOBJ	*pv = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));

					if ( px != NULL && tad_parse(px, psz, NULL, &prec) < E_OK ) prec = NULL;
					if ( px != NULL ) Kfree(px);
					if ( prec != NULL && pv != NULL && tad_lnk_find(prec, &plink_now.vobjid, pv) >= E_OK
					  && ts_uuid_cmp(&pv->target, &v.target) == 0 ) {
						v.tvol = pv->tvol;
					}
					if ( pv != NULL ) Kfree(pv);
					if ( prec != NULL ) tad_free(prec);
				}
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
			if ( ( rq.flags & DT_F_AUTO ) != 0 ) {
				auto_req(&rq, &v);	/* 自動起動: in its place among those opened with it */
			} else {
				dt_open_vobj(NULL, &v);
			}
			plink_opening = FALSE;
		} else if ( rq.req == DT_RQ_CARRY ) {
			carry_start(&rq, anssz, dt_asker);	/* answered when the hand lets go */
			continue;
		} else if ( rq.req == DT_RQ_PLACE || rq.req == DT_RQ_PLACE_MORE ) {
			an.er = carry_place(&rq, (BOOL)( rq.req == DT_RQ_PLACE_MORE ), &an.into,
					    dt_asker);
		} else if ( rq.req == DT_RQ_ATTR && ( rq.flags & DT_F_LINK ) != 0 ) {
			/* 仮身属性変更 of a link the program names: the windows opened from it told its new look */
			PLINK	e;

			if ( !plink_take(&rq, dt_asker, &e) ) {
				an.er = E_PAR;
			} else {
				rq.flags &= ~DT_F_LINK;
				an.er = dt_proc_link_op(&rq, &an);
				if ( an.er >= E_OK ) lwatch_tell(&e.parent, &e.vobjid, &rq.target, &an.look);
			}
		} else if ( rq.req >= DT_RQ_CLOSE && rq.req <= DT_RQ_FULL ) {
			an.er = dt_proc_link_op(&rq, &an);	/* 仮身操作 and 実身操作 of a program's link */
		} else if ( rq.req == DT_RQ_LINK_WATCH ) {
			an.er = lwatch_req(&rq, &an, dt_asker);	/* a window told how the link it was opened from looks */
		} else if ( rq.req == DT_RQ_COLOUR ) {
			an.er = dt_proc_link_op(&rq, &an);	/* a colour chosen for a program, over its window */
		} else if ( ( rq.req == DT_RQ_APPS && ( rq.flags & DT_F_INFO ) != 0 )
			 || rq.req == DT_RQ_INFO_APPS || rq.req == DT_RQ_INFO_KEEP ) {
			/* 管理情報's process, for the link it was started on */
			an.er = dt_info_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_APPS ) {
			an.er = dt_apps_req(&rq, &an);		/* 実行's list for a program's link */
		} else if ( rq.req == DT_RQ_LINK_STATE ) {
			an.er = dt_link_state_req(&rq, &an);	/* how a program's link stands, for its menu */
		} else if ( ( rq.req >= DT_RQ_TELL && rq.req <= DT_RQ_WIN_LIST ) || rq.req == DT_RQ_ROOTS ) {
			/* the system's tools: told of changes, 削除 and 再探索 of 屑実身操作, ウインドウ一覧 */
			an.er = dt_tool_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_ACC_LIST || rq.req == DT_RQ_ACC_SET ) {
			an.er = dt_acc_req(&rq, &an);		/* the 小物箱's list, and the person's changes to it */
		} else if ( rq.req == DT_RQ_SAVE_NEW || rq.req == DT_RQ_LINK_VIEW ) {
			an.er = link_req(&rq, &an, dt_asker);	/* the link it was opened from */
		} else if ( rq.req == DT_RQ_LET_GO ) {
			an.er = let_go_req(&rq, &an);		/* shapes let go over another window: they stay */
		} else if ( rq.req == DT_RQ_FIRST_CLOSE ) {
			an.er = first_close_req(&rq);		/* the first window closing: the session's end */
		} else if ( rq.req == DT_RQ_DISK ) {
			an.er = dt_disk_req(&rq, &an, dt_asker);	/* ディスク操作's steps */
		} else if ( rq.req == DT_RQ_SYSINST ) {
			/* システム登録: the disks, the plan, and the copy done in a task of the desktop's */
			an.er = dt_sysinst_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_ADMIN ) {
			an.er = dt_admin_req(&rq, dt_asker);
		} else if ( rq.req == DT_RQ_SYSUP ) {
			/* the system updated from its package: the administrator asked at its beginning */
			an.er = dt_sysup_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_REMOVE
			 && !knl_ob_pid_admin(dt_asker)
			 && ( an.er = dt_admin_ask(dt_asker,
						   (CONST UB *)"プログラムの削除", &rq.win) ) < E_OK ) {
			/* only an administrator takes away programs: the password asked for */
		} else if ( rq.req == DT_RQ_INSTALL ) {
			an.er = dt_pkg_install(&rq.target, (BOOL)( rq.recno == 1 ), dt_asker,
					       &rq.win, &an.into, &an.how);
		} else if ( rq.req == DT_RQ_REMOVE ) {
			an.er = dt_prog_remove(&rq.target);
		} else if ( ( rq.req == DT_RQ_FONT_REG || rq.req == DT_RQ_FONT_SET || rq.req == DT_RQ_FONT_DEL )
			 && !knl_ob_pid_admin(dt_asker)
			 && ( an.er = dt_admin_ask(dt_asker,
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
			an.er = dt_print_spool(&rq, anssz);	/* 印刷 of the pages it made, by 基本印刷 */
		} else if ( rq.req == DT_RQ_PRJOB ) {
			/* 基本印刷 asks for what it prints */
			an.er = dt_print_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_SCRPRINT ) {
			/* 画面印刷's panel asks, and hands over what it set */
			an.er = dt_scr_req(&rq, &an, dt_asker);
		} else if ( rq.req == DT_RQ_NOTIFY ) {
			an.er = dt_note_req(&rq, &an);		/* 通知: shown, what comes of it told later */
		} else if ( rq.req == DT_RQ_SCREEN ) {
			an.er = dt_screen_req(&rq);		/* システム環境設定's 画面サイズ, now */
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
		if ( an.er == DT_ANS_LATER ) {
			continue;		/* answered when what it asked for is over */
		}
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

/* A window of the desktop's own given the icon of one of the programs, by its id */
EXPORT void dt_win_icon( INT wid, CONST char *prog )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)prog);

	if ( p != NULL ) {
		prog_icon(wid, p);
	}
}

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
	d->tool   = DT_TOOL_OWN;
	d->own    = own;
	d->self   = self;
	d->wid    = wid;
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
/* ---------------------------------------------------------------- the first window */

/*
 * The first window (最初のウインドウ) is the window of 仮身一覧 on the
 * cabinet the system opens with, a process like any program's: started
 * as the desktop starts (first_start), and started again when it ended
 * while the session goes on (first_poll). Closing it ends the session
 * (DT_RQ_FIRST_CLOSE): it is asked to close only once every other
 * program has ended, so that a going off given up leaves it as it was.
 * A link to what the system makes is put in it by the program
 * (DT_HOW_LINK_PUT); the panels the desktop asks with stand over it.
 */
#define FIRST_PROG	"virtual-object-list"	/* 仮身一覧 */
#define FIRST_TRIES	5			/* starts again at most, the session through */

LOCAL UINT	dt_first_starts = 0;		/* how many times it was started */
LOCAL BOOL	dt_first_ending = FALSE;	/* the session ends: it closes when asked */
LOCAL BOOL	dt_first_auto = FALSE;		/* its 自動起動 is being watched, as the start's */
LOCAL BOOL	dt_first_said = FALSE;		/* it came up, said once a start */
LOCAL UW	dt_first_seen = 0;		/* when it was last looked for */
LOCAL BOOL	dt_first_tasks = FALSE;		/* what the start asks over it was asked */
LOCAL DTWIN	dt_first_host;

LOCAL void video_confirm( DTWIN *d );

/* The first window's object, the cabinet */
LOCAL BOOL first_obj( TS_UUID *p )
{
	return (BOOL)( ts_str_to_uuid(DT_CABINET, p) >= E_OK );
}

/* The first window's number, 0 while there is none (not up yet, or ended) */
LOCAL INT first_wid( void )
{
	TS_UUID	cab;
	INT	wid;

	if ( !first_obj(&cab) ) {
		return 0;
	}
	wid = wm_obj_showing(&cab);
	return ( wid > 0 ) ? wid : 0;
}

/*
 * The first window as a window of the desktop's, for the panels the
 * desktop asks with over it: only its number, and no record. NULL while
 * there is none.
 */
EXPORT DTWIN *dt_first_win( void )
{
	INT	wid = first_wid();

	if ( wid <= 0 ) {
		return NULL;
	}
	knl_memset(&dt_first_host, 0, sizeof(dt_first_host));
	dt_first_host.used = TRUE;
	dt_first_host.wid = wid;
	(void)first_obj(&dt_first_host.id);
	return &dt_first_host;
}

/* The process of the first window, 0 none */
LOCAL ID first_pid( void )
{
	INT	wid = first_wid();

	return ( wid > 0 ) ? wm_obj_maker(wid) : 0;
}

/* The first window started: 仮身一覧 on the cabinet, as a double click opens it */
LOCAL ER first_start( void )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)FIRST_PROG);
	T_VOBJ		v;
	ER		er;

	knl_memset(&v, 0, sizeof(v));
	if ( p == NULL || !first_obj(&v.target) ) {
		return E_NOEXS;
	}
	v.recno = -1;
	v.frcol = v.chcol = v.tbcol = v.bgcol = TAD_COL_NONE;
	dt_first_starts++;
	dt_first_said = FALSE;
	er = dt_prog_open(NULL, p, &v);
	tm_printf((UB *)"TessronOS desktop: the first window started (%d)\n", (INT)er);
	return er;
}

/*
 * Each turn: the first window come up said, what the start asks over it
 * asked once it is there, its 自動起動 given up being watched when its
 * process went first, and the window started again when it ended while
 * the session goes on
 */
LOCAL void first_poll( void )
{
	TS_UUID	cab;
	INT	wid;
	UW	now = now_ms();

	if ( dt_stop || !first_obj(&cab) || (UW)( now - dt_first_seen ) < 200 ) {
		return;
	}
	dt_first_seen = now;
	wid = first_wid();
	if ( wid > 0 ) {
		if ( !dt_first_said ) {
			dt_first_said = TRUE;
			tm_printf((UB *)"TessronOS desktop: the first window is up\n");
		}
		if ( !dt_first_tasks ) {
			dt_first_tasks = TRUE;
			video_confirm(dt_first_win());		/* the screen's size confirmed */
			dt_sysup_resume(dt_first_win());	/* a system update the last start did not end */
		}
		return;
	}
	if ( dt_prog_opening(&cab) ) {
		return;					/* being started */
	}
	if ( dt_first_auto ) {
		dt_first_auto = FALSE;
		dt_prog_watch(FALSE);			/* it ended before it said: nothing more to wait for */
	}
	if ( dt_first_starts >= FIRST_TRIES ) {
		return;
	}
	tm_printf((UB *)"TessronOS desktop: the first window ended: started again\n");
	(void)wm_msg_put("最初のウインドウが閉じられたため、開き直しました");
	(void)first_start();
}

/*
 * DT_RQ_OPEN with DT_F_AUTO and no target: the first window's 自動起動
 * all asked, and the start's watch over what it opened ends there
 */
LOCAL void first_auto_done( ID asker, INT n )
{
	if ( asker > 0 && asker == first_pid() && dt_first_auto ) {
		dt_first_auto = FALSE;
		dt_prog_watch(FALSE);
		tm_printf((UB *)"TessronOS desktop: the first window opened %d by itself\n", n);
	}
}

/*
 * DT_RQ_FIRST_CLOSE: closing the first window ends the session, asked
 * first while it is not ending; the window closes only when the session
 * ends (programs_end asks it then)
 */
LOCAL ER first_close_req( CONST T_DTREQ *rq )
{
	if ( dt_first_ending ) {
		return E_OK;
	}
	if ( dt_poweroff ) {
		return E_ABORT;			/* asked already: it is asked again as the session ends */
	}
	if ( dt_confirm(dt_host_of(&rq->win), "OSを終了しますか？", NULL, "いいえ", "はい") ) {
		dt_poweroff = TRUE;
		dt_stop = TRUE;
	}
	return E_ABORT;
}

/* The window at a place of the table (0 to DT_MAX_WIN - 1), used or not */
EXPORT DTWIN *dt_win_at( INT i )
{
	return ( i >= 0 && i < DT_MAX_WIN ) ? &dt_win[i] : NULL;
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
	T_TAD	*rec;
	T_VOBJ	*v;
	BOOL	changed = FALSE;
	ER	er = E_OK;

	/* the record is a program's while it runs: it is told, and writes it itself */
	if ( plink_view(parent, vobjid, target, sx, sy, vm, wrap, zoom, &er) ) {
		return ( er < E_OK ) ? er : E_OK;
	}
	rec = om_store_get(parent, 0);
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
		er = om_store_save(parent, 0);
	}
	Kfree(v);
	return ( er < E_OK ) ? er : E_OK;
}

/* A close the person asked for: a part's window, unless the part says not now */
LOCAL void win_close_asked( DTWIN *d )
{
	if ( d == NULL || !d->used ) {
		return;
	}
	if ( d->own != NULL && d->own->closing != NULL && !d->own->closing(d) ) {
		return;				/* the part's: not now (a check running to its end) */
	}
	win_close(d);
}

LOCAL void win_close( DTWIN *d )
{
	CONST DTOWN	*own;

	if ( d == NULL || !d->used ) {
		return;
	}
	/* the part lets go of what it keeps, then the window goes */
	own = d->own;
	d->own = NULL;
	if ( own != NULL ) {
		own->close(d);
	}
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

EXPORT BOOL dt_obj_open( CONST T_VOBJ *v, INT *p_wid )
{
	INT	wid = 0;
	BOOL	open = TRUE;

	if ( ( wid = wm_obj_showing(&v->target) ) > 0 ) {
		/* a program's window that says it shows the object */
	} else {
		wid = 0;
		open = dt_prog_opening(&v->target);
	}
	if ( p_wid != NULL ) {
		*p_wid = wid;
	}
	return open;
}

/*
 * The window the object v points at is open in (wid) brought to the
 * front with the keys; 0, one still being started, takes them when it
 * comes up
 */
EXPORT void dt_obj_front( CONST T_VOBJ *v, INT wid )
{
	if ( wid > 0 ) {
		wm_raise(wid);
		(void)wm_focus(wid);
		wm_update();
	} else {
		dt_prog_opening_keys(&v->target);
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

/*
 * A record of 実時間制御TAD opened: played by the program that plays
 * media (RTM_VIDEO_PROG, MPEG Player) in its own window
 */
LOCAL void rt_play( DTWIN *from, CONST T_VOBJ *v )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)RTM_VIDEO_PROG);

	if ( p == NULL || p->builtin ) {
		tm_printf((UB *)"player: no MPEG Player to play 実時間制御TAD\n");
		(void)wm_msg_put("MPEG Playerが入っていないので、実時間制御TADを再生できません");
		return;
	}
	(void)dt_prog_run(from, p, v);
}

EXPORT void dt_open_vobj( DTWIN *from, CONST T_VOBJ *v )
{
	CONST DTPROG	*p;
	INT		wid;
	UB		app[DT_PROG_ID];

	/* a 虚身: what it points at is on a volume that is not attached; which one is said */
	if ( dt_disk_absent(from, v) ) {
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
	/*
	 * Open already, in a window of the desktop's or of a program, or in
	 * one being started on it: that window brought to the front with the
	 * keys, and no second one opened -- whichever way this open came,
	 * a double click, 実行, 自動起動 or a program's link
	 */
	if ( dt_obj_open(v, &wid) ) {
#ifdef USE_UITEST
		tm_printf((UB *)"desktop: open already, window %d to the front\n", wid);	/* for the tests */
#endif
		dt_obj_front(v, wid);
		return;
	}
	/* an object a program that is a process opens: that process */
	app[0] = 0;
	if ( win_app == NULL && om_link_default_app(v, app, DT_PROG_ID) > 0 ) {
		p = prog_for_kind(dt_prog_find(app), v);
		if ( p != NULL && !p->builtin ) {
			(void)dt_prog_open(from, p, v);
			return;
		}
	}
	/*
	 * スライド: the document's links shown one at a time, in a window of
	 * its own, by its program, a process (application/slide)
	 */
	if ( app_is(( win_app != NULL ) ? win_app : app, "basic-slide") ) {
		CONST DTPROG	*sp = dt_prog_find((CONST UB *)"basic-slide");

		if ( sp != NULL && !sp->builtin ) {
			(void)dt_prog_open(from, sp, v);
		} else {
			(void)wm_msg_put("この実身を開くプログラムがありません");
		}
		return;
	}
	/*
	 * A record of 実時間制御TAD -- asked to be played by the id of the
	 * player that plays media (an applist a TADjs Desktop wrote names it
	 * basic-player, an alias of the player), or a record that is nothing
	 * but a <realtime>, which the editors have no page for -- played by
	 * that player, which plays it in its own window (lib/librtplay)
	 */
	if ( app_is(( win_app != NULL ) ? win_app : app, "basic-player")
	  || app_is(( win_app != NULL ) ? win_app : app, RTM_VIDEO_PROG) ) {
		rt_play(from, v);
		return;
	}
	if ( win_app == NULL ) {
		T_TAD	*rec = om_store_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);

		if ( rec != NULL && tv_kind(rec) == TV_KIND_NONE && rt_find(rec) != NULL ) {
			rt_play(from, v);
			return;
		}
	}
	/*
	 * Nothing to open it with, as 実行 of its menu shows: its records are
	 * no text to be shown (a package, a file taken in).
	 */
	if ( win_app == NULL && app[0] == 0 && !app_any(v) ) {
		(void)wm_msg_put("この実身を開くプログラムがありません");
		return;
	}
	/*
	 * An object whose program is not there or that names none is opened
	 * by the program of its kind, a process: 仮身一覧 for a figure,
	 * 基本文章編集 for a text (prog_for_kind). The 原紙箱 and the 小物箱
	 * are programs of their own (application/basebox, application/accbox),
	 * started above as any program that is a process.
	 */
	{
		T_TAD	*rec = om_store_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);

		p = ( rec != NULL && ( tv_kind(rec) == TV_KIND_FIG || tv_kind(rec) == TV_KIND_DOC ) )
		    ? prog_for_kind(dt_prog_find((CONST UB *)"virtual-object-list"), v) : NULL;
		if ( p == NULL || p->builtin ) {
			(void)wm_msg_put("この実身を開くプログラムがありません");
			return;
		}
		(void)dt_prog_open(from, p, v);
	}
}

/*
 * 自動起動: the links of what a window has just opened on that ask to be
 * opened with it, opened as a double click opens them, each in a window
 * of its own, by no act of the person: the program of the window asks
 * for each (DT_RQ_OPEN with DT_F_AUTO). What is opened that way does
 * the same in its turn; a link to something already open brings that
 * window forward and goes no further.
 *
 * The windows opened together stand as if each had opened at once, in
 * the order of the links, in front of the window that asked: the window
 * of the last link in front, the first link's behind the others, and the
 * window that asked behind them all. They are opened without the keys,
 * which stay where they were (design 10.4), the window that asked
 * keeping them when it had them. A program
 * that is a process puts its window up in its own time, the starts in
 * the order asked but each as fast as its program is; so the order is
 * not left to that. Each window is given its place in the group of the
 * asking window (wm_set_rank), the window of a process as it comes up
 * (dt_prog_rank carries it to the process), and takes it behind those
 * of the group that are to stand in front of it.
 */
#define AUTO_RANK_ASKER	0		/* the window that asked: behind them all */

LOCAL void auto_req( CONST T_DTREQ *rq, T_VOBJ *v )
{
	INT	asking = wm_obj_wid(&rq->win), wid;
	UINT	g = ( asking > 0 ) ? (UINT)asking : 0;
	UINT	r = ( rq->grab_y > 0 ) ? (UINT)rq->grab_y : 1;

	if ( g > 0 ) {
		(void)wm_set_rank(asking, g, AUTO_RANK_ASKER);
	}
	dt_prog_rank(g, r);
	dt_open_vobj(NULL, v);
	dt_prog_rank(0, 0);
	/* one up already: its place now */
	wid = wm_obj_showing(&v->target);
	if ( g > 0 && wid > 0 && wid != asking ) {
		(void)wm_set_rank(wid, g, r);
	}
}

EXPORT void dt_close( DTWIN *d )
{
	win_close_asked(d);
}

/* What 全画面表示 makes a window: the screen above the message line */
LOCAL ER full_rect( T_DPRECT *o, CONST T_DPRECT *near )
{
	UINT	sw, sh;
	ER	er = dt_screen_size(&sw, &sh);

	if ( er < E_OK ) {
		return er;
	}
	if ( ts_disp_screens() > 1 && near != NULL ) {
		/* the screen the window's centre is on; the main one has the message line */
		wm_screen_at(( near->left + near->right ) / 2, ( near->top + near->bottom ) / 2, o);
		if ( wm_screen_is_main(o) ) {
			o->bottom -= wm_msg_height();
		}
		return E_OK;
	}
	o->left = 0;  o->top = 0;
	o->right = (INT)sw;
	o->bottom = (INT)sh - wm_msg_height();
	return E_OK;
}

/*
 * 全画面表示: the window made the size of the screen, or put back where
 * it stood before.
 */
EXPORT void dt_fullscreen( DTWIN *d )
{
	T_WMWIN		w;
	T_DPRECT	o;

	if ( d == NULL || wm_ref(d->wid, &w) < E_OK ) {
		return;
	}
	if ( !d->full ) {
		if ( full_rect(&o, &w.outer) < E_OK ) {
			return;
		}
		d->restore = w.outer;
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
 * 全画面表示 of a program's window, kept here for the program as d->full
 * and d->restore are for a window of the desktop's own: asked by the
 * program (DT_RQ_FULL) and by a double press on the corner that sizes
 * the window, the one way both times, so that the two never disagree
 * about where it stood. A window is known by its object's UUID as well
 * as its number: a number given again to a window opened later does not
 * find the place of the one before.
 */
#define DT_FULL_MAX	32

typedef struct {
	INT		wid;		/* 0: the entry is free */
	TS_UUID		win;
	T_DPRECT	restore;
} DTFULL;

LOCAL DTFULL	dt_full[DT_FULL_MAX];

LOCAL DTFULL *full_of( INT wid, CONST TS_UUID *win )
{
	INT	i;

	for ( i = 0; i < DT_FULL_MAX; i++ ) {
		if ( dt_full[i].wid == wid && ts_uuid_cmp(&dt_full[i].win, win) == 0
		  && wm_obj_wid(&dt_full[i].win) == wid ) {
			return &dt_full[i];
		}
	}
	return NULL;
}

/* A free entry, or one whose window has gone */
LOCAL DTFULL *full_free( void )
{
	INT	i;

	for ( i = 0; i < DT_FULL_MAX; i++ ) {
		if ( dt_full[i].wid <= 0 || wm_obj_wid(&dt_full[i].win) != dt_full[i].wid ) {
			return &dt_full[i];
		}
	}
	return NULL;
}

/* change FALSE: the window left as it is, only whether it is so said */
EXPORT ER dt_obj_fullscreen( INT wid, BOOL change, BOOL *p_full )
{
	T_WMWIN		w;
	T_DPRECT	o;
	TS_UUID		win;
	DTFULL		*f;
	ER		er;

	if ( wid <= 0 || wm_ref(wid, &w) < E_OK || wm_obj_uuid(wid, &win) < E_OK ) {
		return E_NOEXS;
	}
	/*
	 * It is so only while it is still the screen's size: a window
	 * sized or moved by hand since, or put back by a program that puts
	 * it back itself (as programs did before the desktop kept the
	 * place), is made the screen's size again rather than put back.
	 */
	f = full_of(wid, &win);
	if ( f != NULL && ( full_rect(&o, &w.outer) < E_OK
			 || w.outer.left != o.left || w.outer.top != o.top
			 || w.outer.right != o.right || w.outer.bottom != o.bottom ) ) {
		f->wid = 0;
		f = NULL;
	}
	if ( !change ) {
		*p_full = (BOOL)( f != NULL );
		return E_OK;
	}
	if ( f != NULL ) {
		er = wm_move(wid, &f->restore);
		if ( er >= E_OK ) {
			f->wid = 0;
			*p_full = FALSE;
		}
	} else {
		f = full_free();
		er = ( f == NULL ) ? E_LIMIT : full_rect(&o, &w.outer);
		if ( er >= E_OK ) {
			er = wm_move(wid, &o);
		}
		if ( er >= E_OK ) {
			f->wid = wid;
			f->win = win;
			f->restore = w.outer;
			*p_full = TRUE;
		}
	}
	wm_update();
	return er;
}

/*
 * Whether a press on the corner that sizes a window is the second of a
 * double press there: the same window's corner pressed again within the
 * double press time (LK_DBLTIME), no farther off than LK_DBL_W, as the
 * pictogram's double press is told (wm_pict_close).
 */
LOCAL INT	grip_wid = 0;
LOCAL INT	grip_x, grip_y;
LOCAL UD	grip_when;

LOCAL BOOL grip_twice( INT wid, INT sx, INT sy, UD when )
{
	UD	gap = (UD)wm_num(WM_LOOK_DBLTIME, 400) * 1000000U;
	INT	near = wm_num(LK_DBL_W, 10);
	BOOL	twice;

	twice = (BOOL)( grip_wid == wid && when >= grip_when && when - grip_when <= gap
		     && sx - grip_x <= near && grip_x - sx <= near
		     && sy - grip_y <= near && grip_y - sy <= near );
	if ( twice ) {
		grip_wid = 0;		/* a third press starts again */
	} else {
		grip_wid = wid;
		grip_x = sx;
		grip_y = sy;
		grip_when = when;
	}
	return twice;
}

/*
 * The object a virtual object points at opened with the program asked
 * for (実行): one that is a process is started with it, one of the
 * desktop's own opens it in a window of its kind.
 */
EXPORT ER dt_open_as( DTWIN *from, CONST T_VOBJ *v, CONST UB *app )
{
	CONST DTPROG	*p = prog_for_kind(dt_prog_find(app), v);
	INT		wid;

	if ( app_is(app, "basic-print") ) {
		dt_print_vobj(from, v);		/* 基本印刷: printed without being opened */
		return E_OK;
	}
	/* open already: that window, as a double click finds it, whatever program was asked for */
	if ( dt_obj_open(v, &wid) ) {
		dt_obj_front(v, wid);
		return E_OK;
	}
	if ( p != NULL && !p->builtin ) {
		return dt_prog_open(from, p, v);
	}
	if ( p != NULL && p->kind != DT_PK_APP ) {
		return dt_prog_run(from, p, v);
	}
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

/*
 * The window a virtual object was opened into, closed: a program's
 * asked to close by deleting it as an object (its program closes it
 * once it has kept what it has)
 */
EXPORT void dt_close_vobj( CONST T_VOBJ *v )
{
	TS_UUID	u;
	INT	wid;

	if ( ( wid = wm_obj_showing(&v->target) ) > 0 && wm_obj_maker(wid) > 0
	  && wm_obj_uuid(wid, &u) >= E_OK ) {
		(void)ob_del_obj(&u);
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

/*
 * The menu of a window of a part of the desktop's: its definition, set
 * by the part, with 小物 and 実行 of the tail filled in as for every
 * window -- 実行 for a link in it, grey away from one -- and the
 * system's items set for the link it was opened on
 */
LOCAL ER win_menu( DTWIN *d, BOOL on_vobj, CONST T_VOBJ *v, ID *p_mid )
{
	TS_UUID	def, td;
	ER	er;

	if ( d->own == NULL ) {
		return E_OBJ;
	}
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

/* A chosen item done: the system's by dtvmn.c, the rest by the part */
LOCAL void win_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj, CONST T_VOBJ *v )
{
	if ( dt_exe_vmn(d, sel, on_vobj, v) ) {
		return;				/* the window may be gone */
	}
	if ( d->own != NULL ) {
		d->own->menu_do(d, sel);	/* the part's own items */
	}
	wm_update();
}

LOCAL void menu_at( DTWIN *d, INT x, INT y, UD when )
{
	T_VOBJ		v;
	T_MNSEL		sel;
	INT		pid, cmd = 0;
	BOOL		on = FALSE;
	ID		mid;
	ER		er;

	/* a press outside the window (on the ground, which gives the menu to the active window) is on no link */
	{
		T_WMWIN	ww;

		if ( wm_ref(d->wid, &ww) >= E_OK
		  && x >= 0 && y >= 0 && x < ww.work.right - ww.work.left && y < ww.work.bottom - ww.work.top
		  && d->own != NULL && d->own->link_at != NULL ) {
			on = d->own->link_at(d, x, y, &v);
		}
	}
	if ( win_menu(d, on, &v, &mid) < E_OK ) {
		return;
	}
	pid = mn_opn_men(mid, d->wid, x, y);
	if ( pid >= 0 ) {
		tm_printf((UB *)"desktop: menu %s\n", d->name);	/* for the tests, as a process's says it */
		cmd = dt_run_menu(d, pid, when);
	}
	er = mn_get_sel(mid, cmd, &sel);
	(void)mn_del_men(mid);
	if ( er >= E_OK ) {
		tm_printf((UB *)"desktop: chosen %s\n", sel.code);
		win_menu_do(d, &sel, on, &v);
	}
	wm_update();
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
		/*
		 * A key typed into another window -- one the person moved the
		 * keys to, a program's question that took them -- is that
		 * window's: the panel is answered only by what is typed to it
		 */
		if ( ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) && ev.wid != d->wid ) {
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

/* A wheel turned over a part's window: the part's to make what it will of */
LOCAL void wheel( DTWIN *d, CONST T_WMEV *ev )
{
	if ( d != NULL && d->own != NULL ) {
		(void)own_give(d->wid, ev);	/* the part's to make what it will of */
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
 * The outputs (10.4.10): each one's place and mode kept in the machine's
 * settings under the identity its EDID gives it (DISPLAY.<ident>, "on
 * 1024x768 right 0" or "off"), so that a display plugged in again finds
 * them. The screen object's record 1 has a line for each output (OUTi
 * name on|off WIDTHxHEIGHT side offset ident ...) where there is more
 * than one; the outputs are set by writing OUTi lines there.
 */
#define OUTS_REC	4096

IMPORT void dt_identify( DTWIN *d, INT ms );	/* dtform.c */

LOCAL void screen_redraw( void );

/* The screen object's record 1, whole: its length, 0 when it cannot be read */
LOCAL INT outs_rec( UB *t, INT max )
{
	SZ	asz = 0;
	ID	key = disp_key();

	if ( key <= 0 || ob_rea_rec(key, OB_DSP_MODE, 0, t, (SZ)max, &asz) < E_OK ) {
		return 0;
	}
	return (INT)asz;
}

/*
 * The setting of the i-th output's line of the record, as it is written
 * back ("on 1024x768 right 0", "off") into set, and its identity into id:
 * FALSE when there is no such line
 */
LOCAL BOOL outs_line( CONST UB *t, INT len, INT i, char *set, INT setmax, char *id, INT idmax )
{
	char	key[8] = "OUT0", v[CF_VAL_MAX], w[5][24];
	INT	k;

	key[3] = (char)( '0' + i );
	if ( !cf_get(t, len, key, 0, v, sizeof(v)) ) {
		return FALSE;
	}
	/* name on|off size side offset ident */
	for ( k = 0; k < 5; k++ ) {
		if ( !cf_word(v, k + 1, w[k], sizeof(w[k])) ) w[k][0] = 0;
	}
	if ( knl_strcmp(w[0], "on") == 0 ) {
		tm_sprintf((UB *)set, (UB *)"on %s %s %s", w[1], w[2], w[3]);
	} else {
		tm_sprintf((UB *)set, (UB *)"off");
	}
	(void)setmax;
	knl_strncpy(id, w[4], (SZ)idmax - 1);
	id[idmax - 1] = 0;
	return TRUE;
}

/* The outputs' settings written to the screen: "OUTi setting" lines, one for each */
LOCAL ER outs_write( CONST char *lines )
{
	ID	key = disp_key();

	if ( key <= 0 ) {
		return E_NOEXS;
	}
	return ob_wri_rec(key, OB_DSP_MODE, 0, lines, (SZ)knl_strlen(lines), NULL);
}

/* The outputs as they are now, as lines to write back */
LOCAL INT outs_now( char *lines, INT max )
{
	UB	t[OUTS_REC];
	char	set[64], id[16], l[80];
	INT	len = outs_rec(t, sizeof(t)), i, n = 0;

	lines[0] = 0;
	for ( i = 1; i < 4 && outs_line(t, len, i, set, sizeof(set), id, sizeof(id)); i++ ) {
		tm_sprintf((UB *)l, (UB *)"OUT%d %s\n", i, set);
		if ( n + (INT)knl_strlen(l) >= max ) break;
		knl_strcpy(lines + n, l);
		n += (INT)knl_strlen(l);
	}
	return n;
}

/* The outputs' settings now kept in the machine's settings, each under its display's identity */
LOCAL void outs_keep( void )
{
	UB	t[OUTS_REC];
	char	set[64], id[16], k[32];
	INT	len = outs_rec(t, sizeof(t)), i;

	for ( i = 1; i < 4 && outs_line(t, len, i, set, sizeof(set), id, sizeof(id)); i++ ) {
		if ( id[0] == 0 ) continue;
		tm_sprintf((UB *)k, (UB *)"DISPLAY.%s", id);
		(void)wm_dev_conf_put(k, set);
		tm_printf((UB *)"TessronOS desktop: output %d kept as %s: %s\n", i, k, set);
	}
}

/*
 * As the desktop starts: each output besides the main one set as the
 * machine's settings keep it for its display, those of no setting left
 * as the driver put them (right of the main one)
 */
LOCAL void outs_start( void )
{
	UB	t[OUTS_REC];
	char	set[64], id[16], k[32], v[CF_VAL_MAX], lines[256], l[80];
	INT	len = outs_rec(t, sizeof(t)), i, n = 0;
	ER	er;

	lines[0] = 0;
	for ( i = 1; i < 4 && outs_line(t, len, i, set, sizeof(set), id, sizeof(id)); i++ ) {
		if ( id[0] == 0 ) continue;
		tm_sprintf((UB *)k, (UB *)"DISPLAY.%s", id);
		if ( !wm_dev_conf_get(k, v, sizeof(v)) || knl_strcmp(v, set) == 0 ) continue;
		tm_sprintf((UB *)l, (UB *)"OUT%d %s\n", i, v);
		if ( n + (INT)knl_strlen(l) >= (INT)sizeof(lines) ) break;
		knl_strcpy(lines + n, l);
		n += (INT)knl_strlen(l);
	}
	if ( n == 0 ) {
		return;
	}
	er = outs_write(lines);
	tm_printf((UB *)"TessronOS desktop: the outputs set as kept (%d)\n", (INT)er);
}

/*
 * システム環境設定's outputs, now (DT_RQ_SCREEN with OUTi lines): set,
 * shown, and asked whether they show right; kept for each display when
 * they do, put back otherwise
 */
LOCAL ER outs_req( CONST char *t )
{
	CONST char *CONST lines[5] = {
		"画面の設定を切り換えましたので表示を確認してください。",
		"正しく表示されているときは、３０秒以内に［Ｅｎｔｅｒ］",
		"キーか［確認］を押してください。",
		"［Ｅｎｔｅｒ］以外のキーを押すか、３０秒以上キーを押さ",
		"ないと、以前の画面の設定に戻します。",
	};
	char	was[256];
	ER	er;

	(void)outs_now(was, sizeof(was));
	er = outs_write(t);
	if ( er < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: the outputs cannot be set so (%d)\n", (INT)er);
		return er;
	}
	screen_redraw();
	if ( dt_first_win() != NULL && dt_confirm_timed(dt_first_win(), lines, 5, "取り消し", "確認", 30000) ) {
		outs_keep();
		return E_OK;
	}
	if ( was[0] != 0 ) (void)outs_write(was);
	screen_redraw();
	tm_printf((UB *)"TessronOS desktop: the outputs put back\n");
	return E_ABORT;
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
	if ( t[0] == 'O' && t[1] == 'U' && t[2] == 'T' ) {
		return outs_req(t);		/* the outputs besides the main one */
	}
	if ( knl_strcmp(t, "IDENT") == 0 ) {
		if ( dt_first_win() != NULL ) dt_identify(dt_first_win(), 3000);
		return E_OK;			/* each screen's number shown on it */
	}
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

/*
 * The pointer over the corner that sizes a window, nothing held: it
 * takes the shape of the hand holding (WM_PT_GRIP), and the one it had
 * when it leaves. The corner is found as a press finds it -- the
 * window nearest the viewer there, outside its work area and its
 * bars -- so that the shape says what a press there would do. Only the
 * ordinary pointer is changed, and put back to it: a shape something
 * else has set stays.
 */
LOCAL BOOL	dt_on_grip = FALSE;

LOCAL BOOL grip_under( INT px, INT py, UD when )
{
	T_WMWIN	w;
	UINT	which = 0;
	INT	wid = wm_at(px, py);

	if ( wid <= 0 || wm_ref(wid, &w) < E_OK
	  || ( w.attr & ( WM_ATTR_RESIZE | WM_ATTR_POPUP ) ) != WM_ATTR_RESIZE ) {
		return FALSE;
	}
	if ( px >= w.work.left && px < w.work.right && py >= w.work.top && py < w.work.bottom ) {
		return FALSE;
	}
	if ( wm_bar_at(wid, px, py, &which) != WM_BARHIT_NONE ) {
		return FALSE;
	}
	return grip_at(&w, px, py, when);
}

LOCAL void grip_hover( INT px, INT py, UD when )
{
	BOOL	on = grip_under(px, py, when);

	if ( on && !dt_on_grip ) {
		if ( wm_pointer_kind() != WM_PT_SELECT ) {
			return;
		}
		(void)wm_set_pointer(WM_PT_GRIP);
		dt_on_grip = TRUE;
	} else if ( !on && dt_on_grip ) {
		if ( wm_pointer_kind() == WM_PT_GRIP ) {
			(void)wm_set_pointer(WM_PT_SELECT);
		}
		dt_on_grip = FALSE;
	} else {
		return;
	}
	(void)wm_pointer_moved(px, py);		/* laid again in its new shape */
}

LOCAL void press( DTWIN *d, CONST T_WMEV *ev )
{
	UINT		which = 0;
	INT		hit, sx, sy;
	T_WMWIN		w;

	if ( wm_ref(d->wid, &w) < E_OK ) {
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
		/* a double press on the corner that sizes it: 全画面表示, or back */
		if ( ( w.attr & WM_ATTR_RESIZE ) != 0 && grip_at(&w, sx, sy, ev->when)
		  && grip_twice(d->wid, sx, sy, ev->when) ) {
			dt_fullscreen(d);
			grip_hover(sx, sy, ev->when);
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
		return;				/* a part's window has no bars of its own to wind */
	}
	/* the work area: the press is the part's, and so is the let go */
	if ( d->own != NULL ) {
		dt_own_held = d->wid;
		(void)own_give(d->wid, ev);
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
		BOOL	full = FALSE;

		/* a double press on the corner that sizes it: 全画面表示, or back, as the program would ask it */
		if ( ( w.attr & WM_ATTR_RESIZE ) != 0 && grip_at(&w, sx, sy, ev->when)
		  && grip_twice(ev->wid, sx, sy, ev->when)
		  && dt_obj_fullscreen(ev->wid, TRUE, &full) >= E_OK ) {
			grip_hover(sx, sy, ev->when);
			return;
		}
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
	if ( wm_kbmode_key(ev->code, ev->mods) ) {
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

/* A step of the start begun: said, and its time kept for the watch */
LOCAL void dt_begin( CONST char *step )
{
	dt_step_at = now_ms();
	dt_step = step;
	tm_printf((UB *)"TessronOS desktop: - %s\n", step);
}

/*
 * The start over, told once on the console: every process the start
 * opened by itself (auto_open) has shown its first picture or ended
 * (dt_prog_settled), counted from when the loop began. What waits for
 * the desktop to be up as a person sees it waits for this line.
 */
LOCAL BOOL	dt_settled = FALSE;
LOCAL UW	dt_loop_at = 0;

LOCAL void settle_poll( void )
{
	INT	drawn = 0, gone = 0, all = 0;
	UW	ms;
	BOOL	up;

	if ( dt_settled ) {
		return;
	}
	up = dt_prog_settled(&drawn, &gone, &all);
	ms = now_ms() - dt_loop_at;
	if ( up ) {
		tm_printf((UB *)"TessronOS desktop: settled in %d ms: %d of %d opened at start drawn, %d ended\n",
			  (INT)ms, drawn, all, gone);
	} else if ( ms >= DT_SETTLE_MS ) {
		tm_printf((UB *)"TessronOS desktop: not settled in %d ms: %d of %d opened at start drawn, %d ended\n",
			  (INT)ms, drawn, all, gone);
	} else {
		return;
	}
	dt_settled = TRUE;
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
	DTWIN		*d = dt_first_win();
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
		DTWIN	*f = dt_first_win();

		if ( f != NULL ) {
			dt_pn_done(f, dt_off_pid);
		} else {
			wm_panel_close(dt_off_pid);
		}
		dt_off_pid = -1;
	}
}

/* Whether the machine goes on going off: FALSE when the session is to go on */
LOCAL BOOL programs_end( void )
{
	ID	*pids = NULL, fpid = first_pid();
	INT	max, n, i, left = 0, shown = 0;
	UW	t0 = now_ms(), panel_at = 0;
	BOOL	forced = FALSE, cancel = FALSE;

	max = knl_prc_list_all(&pids);
	/* the first window last: a going off given up leaves it as it was */
	n = ( pids != NULL ) ? wm_obj_close_programs_but(pids, max, fpid) : 0;
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
	if ( !cancel && fpid > 0 ) {
		/* every other program ended: the first window closes, saving what changed */
		T_PRCINFO	pi;
		UW		f0 = now_ms();

		dt_first_ending = TRUE;
		(void)wm_obj_close_window(first_wid());
		while ( knl_prc_info(fpid, &pi) >= E_OK && !pi.done && (UW)( now_ms() - f0 ) < DT_END_MS ) {
			knl_off_limit(DT_OFF_MS, dt_restart ? -1 : 0);
			programs_tick();
			wm_update();
		}
		tm_printf((UB *)"TessronOS desktop: the first window closed after %d ms\n", (INT)( now_ms() - f0 ));
	}
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
	outs_start();				/* and the outputs besides the main one, as kept for their displays */
	(void)wm_conf_watch();			/* the network's behind, and each again when written */
	knl_obsys_power((FP)dt_power);		/* the system object's power record comes here */
	(void)dt_prog_start();			/* the programs, as real objects */
	wm_obj_painter(tv_paint, tv_shape);	/* what a window's drawing and shape records show */
	port_open();
	req_start();				/* the requests read as they come */
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
	wm_kbmode_start();		/* the input mode the settings start in, on the message line */

	/*
	 * The cabinet to open with. Which one that is belongs in the
	 * system's own settings; until there are settings, the one the
	 * records this system was given open with is named here.
	 */
	dt_begin("the cabinet");
	/*
	 * The first window, and what it opens by itself: the start is over
	 * once they are up. The screen's size is confirmed and a system
	 * update the last start did not end is taken up over it once it is
	 * there (first_poll).
	 */
	dt_prog_watch(TRUE);
	dt_first_auto = TRUE;
	if ( first_start() < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: no cabinet to open\n");
		dt_first_auto = FALSE;
		dt_prog_watch(FALSE);
	}
	dt_begin("the first screen");
	wm_composite();
	dt_begin("the disk repair's result");
	dt_fix_boot();				/* the system volume repaired at this start: what came of it */
	dt_begin("the loop");
	dt_loop_at = now_ms();

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
		settle_poll();			/* what the start opened by itself up: told once */
		first_poll();			/* the first window: up, or started again */
		dt_fault_poll();		/* a process ended by an exception: told in a dialog */
		own_poll();			/* a slide's program asked for something */
		dt_uitest_poll();		/* nothing unless built with UITEST=1 */
		dt_tail_poll();			/* an accessory chosen in a program's menu */
		dt_prog_opening_poll();		/* objects being opened: the starts that are over forgotten */
		dt_winlist_poll();		/* ウインドウ一覧, when the windows changed */
		dt_scr_poll();			/* 画面印刷: the count down */
		dt_print_poll();		/* 印刷: a job whose 基本印刷 went */
		dt_disk_poll();			/* disks attached, taken away, taken out, come back */
		dt_type_poll();			/* where the keys went, for DT_RQ_TYPE */
		carry_ask_poll();		/* a program asked whether it takes what was carried */
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
		if ( wm_read_event(&ev, DT_WAKE_MS) < E_OK ) {
			dt_uitest_idle();	/* no input left: what was put in has been taken */
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
			(void)wm_msg_put(NULL);	/* a message stays until the next thing done */
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
			if ( carry.on ) {
				carry_follow(px, py);
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
				/* a program's window: it is told where the knob went */
				T_WMBAR	b;

				wm_bar_drag(dt_hold_wid, dt_hold_bar, px, py);
				if ( wm_num(LK_SCR_LIVE, 1) != 0
				  && wm_bar(dt_hold_wid, dt_hold_bar, &b) >= E_OK
				  && b.clo != dt_hold_at ) {
					dt_hold_at = b.clo;
					wm_obj_scroll(dt_hold_wid, dt_hold_bar, b.clo, OB_SCR_KNOB);
				}
				wm_update();
			}
			if ( dt_own_held != 0 ) {
				(void)own_give(dt_own_held, &ev);	/* the button went down in a part's window */
			} else if ( d != NULL && d->own != NULL && dt_move_wid == 0 ) {
				(void)own_give(d->wid, &ev);		/* the pointer over a part's window */
			}
			if ( !carry.on && dt_move_wid == 0 && dt_hold_wid == 0 && dt_own_held == 0 ) {
				grip_hover(px, py, ev.when);	/* the corner that sizes a window */
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
			dt_up_mods = ev.mods;
			if ( dt_hold_wid != 0 ) {
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
				dt_on_grip = FALSE;
				dt_hold_wid = 0;
				dt_move_wid = 0;
				if ( ts_hid_pointer(&px, &py, &btn) >= E_OK ) {
					grip_hover(px, py, ev.when);	/* still over the corner */
				}
				wm_update();
			}
			dt_hold_wid = 0;
			dt_move_wid = 0;
			break;

		case HID_EV_BTN_DOWN:
			if ( carry.on ) {
				/* another button, while a program's link is carried: the second makes it a copy */
				if ( ev.code == 1 ) {
					carry.copy = TRUE;
				}
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
	req_stop();				/* nobody left to read for */
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
	/* the desktop holds no object's record of its own: whatever is not saved is a program's */
	if ( names != NULL && max > 0 ) {
		names[0] = 0;
	}
	return 0;
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
