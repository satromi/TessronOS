/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	part.c
 *	Parts, panels and menus (design 16.2.2, stage 10b)
 *
 *	A panel is a rectangle in a window's work area holding a list of
 *	parts. Everything it draws comes from its definition and from the
 *	numbered look table; nothing here carries a measurement of its own
 *	except the few this file names, and those are named once.
 *
 *	The definition is copied when the panel opens. A pointer into shared
 *	memory could be neither freed when its program died nor kept from
 *	being changed under another program's screen; a copy of a few
 *	hundred bytes rules out both (design 16.2.3).
 *
 *	A menu is a panel whose parts are one column of buttons. Saying it
 *	that way means a menu and a panel cannot drift apart in how they
 *	are drawn, hit or answered.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/part.h>
#include <ts/wm.h>
#include <ts/keymap.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/fn.h>
#include <ts/dbox.h>
#include <ts/disp.h>
#include <ts/ob.h>
#include <ts/look.h>
#include <ts/kconv.h>
#include <ts/om.h>
#include <ts/img.h>
#include <ts/uuid.h>
#include "wmobj.h"
#include "pdraw.h"

/* What a panel takes for itself. One place, as with a window's frame. */
#define PN_EDGE		2		/* the line around a panel */
#define PN_GAP		4		/* between the edge and what is inside */
#define PN_MARK		6		/* the mark on a choice that is on */
#define PN_INSET	4		/* how far in from its outline a panel's frame is */
#define PN_FRAME_W	8		/* and how wide */

/*
 * The most of a window a panel will keep a copy of. A panel larger
 * than this is drawn over what is there and the program is told to
 * draw that part again when it closes, which is slower but wants no
 * memory.
 */
#define PN_UNDER_MAX	( 4 * 1024 * 1024 )

/* A colour no pixel of a saved rectangle can be, so all of it is laid */
#define PN_NO_CLEAR	0xFF000000U

typedef struct {
	BOOL		used;
	BOOL		is_menu;
	INT		wid;
	T_WMPANEL	def;			/* the copy this panel owns */
	INT		pressed;		/* the part being pressed, -1 for none */
	INT		focus;			/* the box the letters go into, -1 */
	INT		hot;			/* the menu item under the pointer */
	UINT		answer;
	INT		cmd;			/* what a menu answered */
	INT		child;			/* the menu open beside a row, 0 none */
	INT		parent;			/* the menu this one opened from */
	INT		at_row;			/* which row of the parent opened it */
	INT		pend;			/* the row whose list opens once the pointer has stayed, -1 */
	UD		pend_at;		/* since when (ns) */
	BOOL		kept;			/* the list beside a row stays: the row was clicked */

	/*
	 * What was under it before it opened. A menu covers part of a
	 * window that belongs to a program, and that program cannot be
	 * asked to draw itself again while the menu is up -- it is
	 * waiting for what the menu answers. So the pixels are kept here
	 * and laid back when the menu goes, which is the only way a menu
	 * can be opened over anything at all.
	 */
	UW		*under;
	INT		under_w, under_h;
	BOOL		under_dev;		/* under is the window's own pixels at a scale (dp_get_dev) */

	/* a menu's list is a popup window of its own: that window, and the
	   window the menu was opened from */
	INT		popup;
	INT		owner_wid;

	/*
	 * What a press is holding: the name of a selector or the tag held
	 * (1 up), the half of a field's marks, whether the pointer is still
	 * on what went down, and a scrolling selector's knob taken.
	 */
	INT		held_at;
	INT		held_half;
	BOOL		held_in;
	BOOL		held_knob;

	/*
	 * Beside what the program draws itself: a panel that is not the
	 * whole of its window's work area -- the buttons along the top of
	 * a window whose content is the program's. The keys are the
	 * content's, so none of its boxes has the letters until it is
	 * pressed, and a press on the content takes them away again.
	 */
	BOOL		beside;

	/*
	 * A panel of tags whose sheets scroll (P_SCROLL on its tags, the
	 * index scr; -1 none): the height it was defined with, how much
	 * shorter its window's work area has made it (cut), where the tags'
	 * foot and their sheet's foot were defined, and how far the sheet
	 * shown (sy_sheet) is scrolled. The parts of the sheet stand sy
	 * higher and are drawn only inside the sheet above its foot parts
	 * (P_FOOT), which rise by cut with the panel's foot.
	 */
	INT		scr;
	INT		full_h;
	INT		cut;
	INT		tag_b0, tag_ib0;
	INT		sy, sy_sheet;
	/* what shows of the sheet, worked out once for a whole drawing of the panel */
	T_DPRECT	vcache;
	BOOL		vvalid;
} PANEL;

LOCAL PANEL	pn_tab[WM_PANEL_MAX];

/*
 * What draws a panel, and what moves its sheet, one task at a time. A
 * panel is drawn by the task of the program that writes one of its parts
 * (ob_wri_rec), by the task that reads the input (a press, the caret's
 * blink, the wheel) and by the desktop's working a window's bar, on any
 * processor: two drawings at once lay the panel's ground over each
 * other's parts, and each narrows what its window's environment may draw
 * (a sheet that scrolls, a box's letters) and puts back what it found,
 * which may be the other's narrowing -- left so, the window draws in a
 * strip of itself from then on. The task holding it may take it again
 * (a drawing calls drawing); it is taken before the window manager's and
 * the objects' locks, never under them.
 */
LOCAL ID	pn_mtx = 0;
LOCAL ID	pn_owner = 0;
LOCAL INT	pn_depth = 0;

EXPORT ER knl_pn_init( void )
{
	T_CMTX	c;

	if ( pn_mtx > 0 ) {
		return E_OK;
	}
	knl_memset(&c, 0, sizeof(c));
	c.mtxatr = TA_INHERIT;
	pn_mtx = tk_cre_mtx(&c);
	return ( pn_mtx > 0 ) ? E_OK : (ER)pn_mtx;
}

LOCAL void pn_lock( void )
{
	ID	me = tk_get_tid();

	if ( pn_mtx <= 0 ) {
		return;
	}
	if ( pn_owner == me ) {
		pn_depth++;
		return;
	}
	tk_loc_mtx(pn_mtx, TMO_FEVR);
	pn_owner = me;
	pn_depth = 1;
}

LOCAL void pn_unlock( void )
{
	if ( pn_mtx <= 0 || pn_owner != tk_get_tid() ) {
		return;
	}
	if ( --pn_depth > 0 ) {
		return;
	}
	pn_owner = 0;
	tk_unl_mtx(pn_mtx);
}

/*
 * The list of a pop-up, open: one at a time on the whole screen, since
 * while it is open every input is its own. Its panel, the panel and the
 * part it was opened from, which name its first row is (0 up), when the
 * press that opened it was, and whether a press has been made in it
 * since -- before one, a release just after the opening press leaves
 * the list standing, which is how a click opens it.
 */
LOCAL INT	ls_pid = 0;
LOCAL INT	ls_owner = 0;
LOCAL INT	ls_part = -1;
LOCAL INT	ls_first = 0;
LOCAL INT	ls_hot0 = -1;		/* the row under the opening press: where a click stays */
LOCAL T_WMCLICK	ls_click;		/* the press that opened it, to its release */

LOCAL void pop_open( INT pid, INT i, CONST T_WMEV *ev );
LOCAL void pop_close( BOOL draw );
LOCAL INT  tlen( CONST UB *t );

/*
 * The caret of the box with the keys blinks, and one conversion at a
 * time is under way in one box of one panel: the converter's session is
 * kept here for the panels' boxes, as the text windows keep theirs.
 */
LOCAL BOOL	pn_lit = TRUE;		/* the caret in the lit half of its blink */
LOCAL UD	pn_lit_at = 0;
LOCAL INT	pn_kid = 0;
LOCAL T_KCOUT	*pn_kout = NULL;
LOCAL T_PDCONV	*pn_conv = NULL;	/* what is being converted */
LOCAL INT	pn_conv_pid = 0;	/* in which panel's box */
LOCAL INT	pn_conv_i = -1;
LOCAL INT	pn_first = 0;		/* the number of the first candidate listed */
LOCAL INT	pn_roman = -1;		/* how the session is typed into, as last set */

LOCAL INT  list_rowh( void );
LOCAL INT  list_rows( CONST T_DPRECT *r );
LOCAL void list_press( PANEL *p, T_WMPART *pt, INT x, INT y );
LOCAL BOOL box_key( T_WMPART *pt, UINT code, UINT mods );
LOCAL void menu_sub_open( INT pid, INT row );
LOCAL void menu_sub_keep( INT pid, INT row );
LOCAL INT  menu_deepest( INT pid );
LOCAL BOOL menu_in_child( CONST PANEL *p, INT wid );
LOCAL void menu_paint( PANEL *p );
typedef struct {
	INT	cw, ch, row, base, ind_w, ind_h, indent, hpad, vpad;
} MNMET;
LOCAL void mn_metrics( MNMET *m );


/*
 * The face panels and menus write with, at the size they write in. The
 * face is shared by everything on the screen and each user sets the
 * size it wants, so a panel that took the size as it found it would
 * measure a row at one size and draw it at another -- and a row
 * measured too narrow loses its last letters.
 */
#define PN_FONT_SIZE	16

LOCAL ID pn_font( void )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, PN_FONT_SIZE);
	}

	return fid;
}

/* Whether a part is shown but cannot be worked */
#define PART_GREY(pt)	( ( (pt)->type & (P_DISABLE | P_INACT) ) != 0 )

/* The tag a panel's tags show, 0 when it has none */
LOCAL INT pn_sheet( CONST PANEL *p )
{
	INT	i;

	for ( i = 0; i < p->def.npart; i++ ) {
		if ( WM_PT_KIND(p->def.part[i].type) == TG_PARTS ) {
			return p->def.part[i].value;
		}
	}
	return 0;
}

/* Whether a part is shown: not put away, and under the tag shown or every one */
LOCAL BOOL pn_shown( CONST PANEL *p, CONST T_WMPART *pt )
{
	if ( ( pt->type & P_GONE ) != 0 ) {
		return FALSE;
	}
	return (BOOL)( pt->sheet == 0 || pt->sheet == pn_sheet(p) );
}

/* Whether a part takes the keys: a box of some kind, shown and able to be worked */
LOCAL BOOL pn_takes_keys( CONST PANEL *p, CONST T_WMPART *pt )
{
	UINT	k = WM_PT_KIND(pt->type);

	return (BOOL)( ( k == TB_PARTS || k == XB_PARTS || k == NB_PARTS || k == SB_PARTS )
		       && !PART_GREY(pt) && pn_shown(p, pt) );
}

/* ---------------------------------------------------------------- shapes */


LOCAL BOOL rect_has( CONST T_DPRECT *r, INT x, INT y )
{
	return ( x >= r->left && x < r->right && y >= r->top && y < r->bottom );
}

LOCAL PANEL *pn_of( INT pid )
{
	if ( pid < 1 || pid > WM_PANEL_MAX ) {
		return NULL;
	}

	return ( pn_tab[pid - 1].used ) ? &pn_tab[pid - 1] : NULL;
}

LOCAL T_WMPART *part_of( PANEL *p, INT num )
{
	INT	i;

	for ( i = 0; i < p->def.npart; i++ ) {
		if ( p->def.part[i].num == num ) {
			return &p->def.part[i];
		}
	}

	return NULL;
}

/*
 * How far a part stands from its place in the definition, in a panel
 * whose sheets scroll: a part at the foot rises with the panel's foot,
 * the tags stay, and every other part is as far up as the sheet is
 * scrolled. Nought in any other panel.
 */
LOCAL INT part_dy( CONST PANEL *p, CONST T_WMPART *pt )
{
	if ( p->scr < 0 || WM_PT_KIND(pt->type) == TG_PARTS ) {
		return 0;
	}
	return ( ( pt->type & P_FOOT ) != 0 ) ? -p->cut : -p->sy;
}

/* Whether a part moves as the sheet is scrolled: shown only inside the sheet (pn_view) */
LOCAL BOOL part_scrolls( CONST PANEL *p, CONST T_WMPART *pt )
{
	return (BOOL)( p->scr >= 0 && WM_PT_KIND(pt->type) != TG_PARTS && ( pt->type & P_FOOT ) == 0 );
}

/* A part's rectangle in the work area, from its place inside the panel */
LOCAL void part_rect( CONST PANEL *p, CONST T_WMPART *pt, T_DPRECT *out )
{
	INT	dy = part_dy(p, pt);

	out->left   = p->def.r.left + pt->r.left;
	out->top    = p->def.r.top + pt->r.top + dy;
	out->right  = p->def.r.left + pt->r.right;
	out->bottom = p->def.r.top + pt->r.bottom + dy;
}

/* The panel of tags that makes a panel's sheets scroll (P_SCROLL), found again as the parts change */
LOCAL void pn_find_scroller( PANEL *p )
{
	INT	i, was = p->scr;

	p->scr = -1;
	for ( i = 0; i < p->def.npart && !p->is_menu; i++ ) {
		if ( WM_PT_KIND(p->def.part[i].type) == TG_PARTS && ( p->def.part[i].type & P_SCROLL ) != 0 ) {
			p->scr = i;
			break;
		}
	}
	if ( p->scr >= 0 && was < 0 ) {
		/* the feet as defined: what the panel is made shorter from */
		p->tag_b0 = p->def.part[p->scr].r.bottom + p->cut;
		p->tag_ib0 = p->def.part[p->scr].inner.bottom + p->cut;
	}
}

/*
 * What of a scrolling panel's sheet shows its parts, in the work area:
 * the sheet of the tag shown (under the lower row of tags when it has
 * one) inside its outline, down to the head of the highest part at its
 * foot. *p_max is how far the sheet can be scrolled: how much further
 * down than that the lowest of its other parts reaches.
 */
LOCAL BOOL pn_view( CONST PANEL *p, T_DPRECT *v, INT *p_max )
{
	T_WMPART	*tg;
	T_DPRECT	s;
	INT		i, foot = 0x7FFFFFFF, low = 0;

	if ( p->scr < 0 || p->scr >= p->def.npart ) {
		return FALSE;
	}
	tg = (T_WMPART *)&p->def.part[p->scr];
	pd_tag_sheet(tg, p->def.pool, &s);
	v->left = p->def.r.left + s.left + 2;
	v->top = p->def.r.top + s.top + 2;
	v->right = p->def.r.left + s.right - 2;
	v->bottom = p->def.r.top + s.bottom - 2;
	for ( i = 0; i < p->def.npart; i++ ) {
		CONST T_WMPART	*pt = &p->def.part[i];

		if ( i == p->scr || !pn_shown(p, pt) ) {
			continue;
		}
		if ( ( pt->type & P_FOOT ) != 0 ) {
			if ( pt->r.top - p->cut < foot ) foot = pt->r.top - p->cut;
		} else if ( pt->r.bottom > low ) {
			low = pt->r.bottom;
		}
	}
	if ( foot != 0x7FFFFFFF && p->def.r.top + foot < v->bottom ) {
		v->bottom = p->def.r.top + foot;
	}
	if ( v->bottom < v->top ) {
		v->bottom = v->top;
	}
	if ( p_max != NULL ) {
		*p_max = p->def.r.top + low - v->bottom;
		if ( *p_max < 0 ) *p_max = 0;
	}
	return TRUE;
}

/* How far the sheet is scrolled, kept within what it can be: from the top for a sheet shown anew */
LOCAL void pn_settle( PANEL *p )
{
	T_DPRECT	v;
	INT		mx = 0, sheet;

	if ( !pn_view(p, &v, &mx) ) {
		p->sy = 0;
		return;
	}
	sheet = p->def.part[p->scr].value;
	if ( sheet != p->sy_sheet ) {
		p->sy = 0;
		p->sy_sheet = sheet;
	}
	if ( p->sy > mx ) p->sy = mx;
	if ( p->sy < 0 ) p->sy = 0;
}

/*
 * The window's right bar, when it has one, as the panel's: all of the
 * sheet against what of it shows, in pixels. A sheet that shows whole
 * has the knob over the whole bar, and there is nothing to work.
 */
LOCAL void pn_bar( PANEL *p )
{
	T_WMWIN		w;
	T_WMBAR		b;
	T_DPRECT	v;
	INT		mx = 0, vh;

	if ( !pn_view(p, &v, &mx) || wm_ref(p->wid, &w) < E_OK || ( w.attr & WM_ATTR_RBAR ) == 0 ) {
		return;
	}
	vh = v.bottom - v.top;
	if ( vh < 1 ) vh = 1;
	knl_memset(&b, 0, sizeof(b));
	b.lo = 0;
	b.hi = vh + mx;
	b.clo = p->sy;
	b.chi = p->sy + vh;
	(void)wm_set_bar(p->wid, WM_BAR_R, &b);
}

/*
 * A scrolling panel made as tall as its window's work area lets it, up
 * to the height it was defined with: shorter, its foot and the tags'
 * sheet rise with the work area's foot, and its sheets scroll.
 */
LOCAL void pn_fit( PANEL *p )
{
	T_WMWIN		w;
	T_WMPART	*tg;
	INT		h, cut;

	if ( p->scr < 0 || wm_ref(p->wid, &w) < E_OK ) {
		return;
	}
	tg = &p->def.part[p->scr];
	h = ( w.work.bottom - w.work.top ) - p->def.r.top;
	cut = p->full_h - h;
	if ( cut > p->tag_ib0 - tg->inner.top - 1 ) cut = p->tag_ib0 - tg->inner.top - 1;
	if ( cut < 0 ) cut = 0;
	p->cut = cut;
	p->def.r.bottom = p->def.r.top + p->full_h - cut;
	tg->r.bottom = p->tag_b0 - cut;
	tg->inner.bottom = p->tag_ib0 - cut;
	pn_settle(p);
}

EXPORT void knl_pn_fit( INT pid )
{
	PANEL	*p = pn_of(pid);

	if ( p != NULL && !p->is_menu ) {
		pn_lock();
		pn_fit(p);
		pn_unlock();
	}
}

/* A part of the sheet brought into what shows of it: the box Tab gives the keys to */
LOCAL void pn_reveal( PANEL *p, INT i )
{
	T_DPRECT	v, r;

	pn_lock();
	if ( i < 0 || i >= p->def.npart || !part_scrolls(p, &p->def.part[i]) || !pn_view(p, &v, NULL) ) {
		pn_unlock();
		return;
	}
	part_rect(p, &p->def.part[i], &r);
	if ( r.top < v.top ) {
		p->sy -= v.top - r.top;
	} else if ( r.bottom > v.bottom ) {
		/* its foot into sight, but never its head out of it */
		p->sy += ( r.bottom - v.bottom < r.top - v.top ) ? r.bottom - v.bottom : r.top - v.top;
	}
	pn_settle(p);
	pn_unlock();
}

EXPORT BOOL knl_pn_scroll( INT wid, INT pos )
{
	INT	i;

	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( !p->used || p->is_menu || p->wid != wid || p->scr < 0 ) {
			continue;
		}
		pn_lock();
		p->sy = pos;
		pn_settle(p);
		pn_unlock();
		wm_panel_draw(i + 1);
		wm_update();
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- the object layer's view */

/* How many parts a panel has, and whether it is a menu */
EXPORT INT knl_pn_count( INT pid, BOOL *p_menu )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	if ( p_menu != NULL ) {
		*p_menu = p->is_menu;
	}
	return p->def.npart;
}

/*
 * A part put on a panel that is open, after the ones it has: its place
 * among them is answered. The caller draws the panel again.
 */
LOCAL INT knl_pn_add_u( INT pid, CONST T_WMPART *pt );

EXPORT INT knl_pn_add( INT pid, CONST T_WMPART *pt )
{
	INT	res;

	pn_lock();
	res = knl_pn_add_u(pid, pt);
	pn_unlock();
	return res;
}

LOCAL INT knl_pn_add_u( INT pid, CONST T_WMPART *pt )
{
	PANEL	*p = pn_of(pid);
	INT	i;

	if ( p == NULL || p->is_menu ) {
		return E_ID;
	}
	if ( pt == NULL ) {
		return E_PAR;
	}
	if ( p->def.npart >= WM_PART_MAX ) {
		return E_LIMIT;
	}
	i = p->def.npart++;
	p->def.part[i] = *pt;
	pn_find_scroller(p);

	return i;
}

/* A part taken off a panel that is open; the ones after it move up one */
LOCAL ER knl_pn_del_u( INT pid, INT i );

EXPORT ER knl_pn_del( INT pid, INT i )
{
	ER	res;

	pn_lock();
	res = knl_pn_del_u(pid, i);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_del_u( INT pid, INT i )
{
	PANEL	*p = pn_of(pid);
	INT	k;

	if ( p == NULL || p->is_menu ) {
		return E_ID;
	}
	if ( i < 0 || i >= p->def.npart ) {
		return E_PAR;
	}
	if ( ls_owner == pid && ls_part >= i ) {
		pop_close(FALSE);		/* its part goes, or moves up one */
	}
	for ( k = i; k + 1 < p->def.npart; k++ ) {
		p->def.part[k] = p->def.part[k + 1];
	}
	p->def.npart--;
	if ( p->pressed == i ) p->pressed = -1; else if ( p->pressed > i ) p->pressed--;
	if ( p->focus == i ) p->focus = -1; else if ( p->focus > i ) p->focus--;
	if ( p->hot == i ) p->hot = -1; else if ( p->hot > i ) p->hot--;
	pn_find_scroller(p);
	wm_damage(p->wid, &p->def.r);

	return E_OK;
}

/* A part moved within its panel: the rectangle is the panel's own coordinates */
LOCAL ER knl_pn_set_rect_u( INT pid, INT i, CONST T_DPRECT *r );

EXPORT ER knl_pn_set_rect( INT pid, INT i, CONST T_DPRECT *r )
{
	ER	res;

	pn_lock();
	res = knl_pn_set_rect_u(pid, i, r);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_set_rect_u( INT pid, INT i, CONST T_DPRECT *r )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	if ( i < 0 || i >= p->def.npart || r == NULL || r->right <= r->left
	  || r->bottom <= r->top ) {
		return E_PAR;
	}
	p->def.part[i].r = *r;
	wm_damage(p->wid, &p->def.r);

	return E_OK;
}

/* One of a panel's parts, copied out: nothing outside holds a pointer in */
LOCAL ER knl_pn_part_u( INT pid, INT i, T_WMPART *out );

EXPORT ER knl_pn_part( INT pid, INT i, T_WMPART *out )
{
	ER	res;

	pn_lock();
	res = knl_pn_part_u(pid, i, out);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_part_u( INT pid, INT i, T_WMPART *out )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	if ( i < 0 || i >= p->def.npart || out == NULL ) {
		return E_PAR;
	}
	*out = p->def.part[i];

	return E_OK;
}

/* ---------------------------------------------------------------- opening */

LOCAL INT wm_panel_open_u( INT wid, CONST T_WMPANEL *def );

EXPORT INT wm_panel_open( INT wid, CONST T_WMPANEL *def )
{
	INT	res;

	pn_lock();
	res = wm_panel_open_u(wid, def);
	pn_unlock();
	return res;
}

LOCAL INT wm_panel_open_u( INT wid, CONST T_WMPANEL *def )
{
	PANEL	*p;
	INT	i;

	if ( def == NULL || wid <= 0 ) {
		return E_PAR;
	}
	if ( def->npart < 0 || def->npart > WM_PART_MAX ) {
		return E_PAR;
	}
	if ( def->r.right <= def->r.left || def->r.bottom <= def->r.top ) {
		return E_PAR;
	}
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		if ( !pn_tab[i].used ) {
			break;
		}
	}
	if ( i == WM_PANEL_MAX ) {
		return E_LIMIT;
	}
	p = &pn_tab[i];
	p->def = *def;				/* the copy: see the head of this file */
	p->wid = wid;
	p->is_menu = FALSE;
	p->pressed = -1;
	p->focus   = -1;
	p->hot     = -1;
	p->pend    = -1;
	p->kept    = FALSE;
	/*
	 * The first box that can be typed in has the letters from the
	 * start. A panel that asks for a name and then wants a press on
	 * the box before it will take one is a panel that loses the first
	 * thing typed into it. One its program places beside its own
	 * drawing (knl_pn_set_beside) gives the letters up again.
	 */
	{
		INT	k;

		p->beside = FALSE;
		for ( k = 0; k < p->def.npart; k++ ) {
			T_WMPART	*pt = &p->def.part[k];

			if ( pn_takes_keys(p, pt) ) {
				p->focus = k;
				/* a box with all its letters chosen, what is typed first taking their place */
				if ( WM_PT_KIND(pt->type) == TB_PARTS ) {
					pt->caret = tlen(pt->text);
					pt->sb_at = 0;
				}
				break;
			}
		}
	}
	p->answer = WM_ANS_NONE;
	p->cmd = 0;
	p->child  = 0;
	p->parent = 0;
	p->at_row = -1;
	p->under  = NULL;
	p->popup  = 0;
	p->owner_wid = 0;
	p->under_w = 0;
	p->under_h = 0;
	p->under_dev = FALSE;
	p->held_at = 0;
	p->held_half = 0;
	p->held_in = FALSE;
	p->held_knob = FALSE;
	p->full_h = p->def.r.bottom - p->def.r.top;
	p->cut = 0;
	p->sy = 0;
	p->sy_sheet = 0;
	p->scr = -1;
	p->vvalid = FALSE;
	pn_find_scroller(p);
	pn_fit(p);
	p->used = TRUE;

	/* what it is about to cover */
	{
		INT	w = p->def.r.right - p->def.r.left;
		INT	h = p->def.r.bottom - p->def.r.top;
		INT	gid = wm_gid(wid);

		if ( gid >= 0 && w > 0 && h > 0 && dp_scale(gid) > 100 ) {
			/* at a scale, the window's own pixels it covers, put back as they were */
			UW	*px = NULL;
			INT	pw, ph;

			if ( dp_get_dev(gid, &p->def.r, &px, &pw, &ph) >= E_OK
			  && (SZ)pw * ph * sizeof(UW) <= PN_UNDER_MAX * 4 ) {
				p->under = px;
				p->under_w = pw;
				p->under_h = ph;
				p->under_dev = TRUE;
			} else if ( px != NULL ) {
				Kfree(px);
			}
		} else if ( gid >= 0 && w > 0 && h > 0
		  && (SZ)w * h * sizeof(UW) <= PN_UNDER_MAX ) {
			p->under = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
			if ( p->under != NULL ) {
				p->under_w = w;
				p->under_h = h;
				dp_get_argb(gid, p->def.r.left, p->def.r.top,
					    p->under, w, w, h);
			}
		}
	}
	wm_damage(wid, &p->def.r);
	knl_wmobj_panel(i + 1);

	return i + 1;
}

EXPORT void knl_pn_set_beside( INT pid )
{
	PANEL	*p = pn_of(pid);

	if ( p != NULL && !p->is_menu ) {
		p->beside = TRUE;
		p->focus = -1;		/* what is typed is the content's until a box is pressed */
	}
}

EXPORT INT wm_panel_open_box( INT wid, INT num, ID owner )
{
	T_WMPANEL	*def;
	INT		n, pid;

	def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( def == NULL ) {
		return E_NOMEM;
	}
	n = db_get(DB_PANEL, num, owner, def, sizeof(T_WMPANEL));
	if ( n < 0 ) {
		Kfree(def);
		return n;
	}
	pid = wm_panel_open(wid, def);
	Kfree(def);

	return pid;
}

/*
 * Where a panel sits, in the work area of its window. A menu is not
 * opened where it was asked for -- it is moved so that the item in
 * hand comes up under the pointer and so that none of it falls outside
 * -- so whoever wants to reach a row of it has to ask.
 */
EXPORT ER wm_panel_rect( INT pid, T_DPRECT *out )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	if ( out == NULL ) {
		return E_PAR;
	}
	*out = p->def.r;

	return E_OK;
}

/* How tall one row of a menu is, which is what a caller steps by */
EXPORT INT wm_menu_row_h( void )
{
	MNMET	m;

	mn_metrics(&m);

	return m.row;
}

/* The window a panel is drawn in: for a menu, its list's own */
EXPORT INT wm_panel_wid( INT pid )
{
	PANEL	*p = pn_of(pid);

	return ( p != NULL ) ? p->wid : E_ID;
}

/*
 * The window of the panel in its own window (wm_panel_open_centre) that
 * stands nearest the viewer, menus left out; 0 when none is open.
 */
EXPORT INT wm_panel_front( void )
{
	T_WMWIN	w;
	INT	i, best = 0, bz = 0;

	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( !p->used || p->is_menu || p->popup <= 0
		  || wm_ref(p->popup, &w) < E_OK || !w.visible ) {
			continue;
		}
		if ( best == 0 || w.z < bz ) {
			best = p->popup;
			bz = w.z;
		}
	}
	return best;
}

LOCAL ER wm_panel_close_u( INT pid );

EXPORT ER wm_panel_close( INT pid )
{
	ER	res;

	pn_lock();
	res = wm_panel_close_u(pid);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_close_u( INT pid )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	/* a pop-up's list goes with the panel it was opened from */
	if ( pid == ls_pid ) {
		ls_pid = 0;
		ls_owner = 0;
		ls_part = -1;
	} else if ( pid == ls_owner ) {
		pop_close(FALSE);
	}
	knl_wmobj_panel_close(pid);
	/*
	 * A menu that opened another takes it with it. The child is closed
	 * first, so that what was under the chain comes back from the far
	 * end inwards and no part of it is left drawn over.
	 */
	if ( p->child > 0 ) {
		INT	c = p->child;

		p->child = 0;
		wm_panel_close(c);
	}
	/* a list goes with its own window: nothing under it to lay back */
	if ( p->popup > 0 ) {
		if ( !p->is_menu && p->owner_wid > 0 ) {
			(void)wm_lit(p->owner_wid, FALSE);	/* the band of the window with the input again */
		}
		if ( p->under != NULL ) {
			Kfree(p->under);
			p->under = NULL;
		}
		wm_close(p->popup);
		p->popup = 0;
		p->used = FALSE;
		return E_OK;
	}
	/* what was under it, laid back */
	if ( p->under != NULL ) {
		INT	gid = wm_gid(p->wid);

		if ( gid >= 0 && p->under_dev ) {
			(void)dp_put_dev(gid, &p->def.r, p->under, p->under_w, p->under_h);
		} else if ( gid >= 0 ) {
			dp_put_argb(gid, p->def.r.left, p->def.r.top,
				    p->under, p->under_w, p->under_w,
				    p->under_h, PN_NO_CLEAR);
		}
		Kfree(p->under);
		p->under = NULL;
	}
	wm_damage(p->wid, &p->def.r);
	p->used = FALSE;

	return E_OK;
}

/*
 * The panels and menus standing in windows that are going, or whose
 * lists are those windows, closed with them: the desktop's tasks were
 * stopped and its windows go (knl_wm_tsk_end).
 */
EXPORT void knl_pn_wins_gone( CONST INT *wids, INT n )
{
	INT	i, k;

	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( !p->used ) {
			continue;
		}
		for ( k = 0; k < n; k++ ) {
			if ( p->wid == wids[k] || p->popup == wids[k] || p->owner_wid == wids[k] ) {
				(void)wm_panel_close(i + 1);
				break;
			}
		}
	}
	if ( pn_conv_pid > 0 && pn_of(pn_conv_pid) == NULL ) {
		/* the box being converted into went with them */
		if ( pn_conv != NULL ) {
			pn_conv->ncl = 0;
			pn_conv->list = FALSE;
		}
		pn_conv_pid = 0;
	}
}

/* ---------------------------------------------------------------- drawing */

/* The room a list keeps on its right for its own bar */
#define LIST_BAR_W	20

/* How tall one row of a list is, and how many fit in it */
LOCAL INT list_rowh( void )
{
	ID	fid = pn_font();
	T_FNMET	met;

	if ( fid > 0 && fn_metrics(fid, &met) >= E_OK ) {
		return met.ascent + met.descent + 2;
	}

	return 18;
}

LOCAL INT list_rows( CONST T_DPRECT *r )
{
	INT	rowh = list_rowh();
	INT	n = ( (r->bottom - r->top) - 4 ) / rowh;

	return ( n > 0 ) ? n : 1;
}

/* Half the letters' height, and never less than eight: how far in a
 * name starts, and the room the lamp has before it. */
LOCAL INT part_margin( void )
{
	ID	fid = pn_font();
	T_FNMET	met;

	if ( fid > 0 && fn_metrics(fid, &met) >= E_OK ) {
		INT	h = met.ascent + met.descent;

		return ( h / 2 > 8 ) ? h / 2 : 8;
	}

	return 8;
}

/*
 * A half-tone of white over what is there, leaving every other dot as
 * it was: the veil over a part that cannot be worked.
 */
LOCAL CONST UW	veil_tile[32 * 4] = { 0 };
LOCAL UW	veil_white[32 * 4];
LOCAL CONST UW	veil_mask[4] = {
	0xAAAAAAAAU, 0x55555555U, 0xAAAAAAAAU, 0x55555555U
};

LOCAL void veil_pat( T_DPPAT *pat )
{
	INT	i;

	for ( i = 0; i < 32 * 4; i++ ) {
		veil_white[i] = 0x00FFFFFFU;
	}
	knl_memset(pat, 0, sizeof(*pat));
	pat->kind = DP_PAT_TILE;
	pat->tile = veil_white;
	pat->hs = 32;
	pat->vs = 4;
	pat->mask = veil_mask;
	(void)veil_tile;
}

/* The height the names of parts are drawn at, and the room before them */
#define SW_FONT		16

/*
 * The panel itself: a black line all round, inside it a raised box --
 * light along the top and the left, dark along the foot and the right
 * -- in the panel's ground, and eight pixels in from the outline a
 * frame eight wide: the panel frame for an ordinary panel, the
 * warning's stripes for one that warns, and red, struck through from
 * corner to corner, for one that cannot be carried on from.
 */
LOCAL void panel_body( PANEL *p, INT gid )
{
	T_DPRECT	q = p->def.r, in;
	T_DPPAT		pat;
	INT		l = q.left, t = q.top, r = q.right, b = q.bottom;
	UINT		fno;

	if ( p->def.kind == WM_PNL_SHEET ) {
		dp_fill_rect(gid, &q, 0x00FFFFFFU);
		dp_frame_rect(gid, &q, wm_look(WM_LOOK_FRAME), 1);
		return;
	}
	if ( p->def.kind == WM_PNL_BARE ) {
		wm_look_pat(LK_PNLGROUND, &pat);
		dp_fill_rect_pat(gid, &q, &pat);
		return;
	}
	dp_fill_rect(gid, &q, wm_look(WM_LOOK_FRAME));
	in.left = l + 1;  in.top = t + 1;  in.right = r - 1;  in.bottom = b - 1;
	wm_look_pat(LK_PNLGROUND, &pat);
	dp_fill_rect_pat(gid, &in, &pat);
	dp_line(gid, l + 1, t + 1, r - 2, t + 1, wm_look(WM_LOOK_LIGHT));
	dp_line(gid, l + 1, t + 1, l + 1, b - 2, wm_look(WM_LOOK_LIGHT));
	dp_line(gid, l + 2, b - 2, r - 2, b - 2, wm_look(WM_LOOK_SHADOW));
	dp_line(gid, r - 2, t + 2, r - 2, b - 2, wm_look(WM_LOOK_SHADOW));

	fno = ( p->def.kind == WM_PNL_WARN ) ? LK_WRNFRAME
	    : ( p->def.kind == WM_PNL_FATAL ) ? LK_FATFRAME : LK_PNLFRAME;
	wm_look_pat(fno, &pat);
	in.left = l + PN_INSET;  in.top = t + PN_INSET;
	in.right = r - PN_INSET;  in.bottom = b - PN_INSET;
	dp_frame_rect_pat(gid, &in, &pat, PN_FRAME_W);
	if ( p->def.kind == WM_PNL_FATAL ) {
		dp_line_wide(gid, in.left, in.top, in.right - PN_FRAME_W,
			     in.bottom - PN_FRAME_W, PN_FRAME_W, 0, &pat);
	}
}

/* The rectangle round every choice of a group: the one selector they make */
LOCAL void group_rect( CONST PANEL *p, INT group, T_DPRECT *out )
{
	INT		i;
	BOOL		any = FALSE;
	T_DPRECT	r;

	for ( i = 0; i < p->def.npart; i++ ) {
		CONST T_WMPART	*pt = &p->def.part[i];

		if ( WM_PT_KIND(pt->type) != WM_PT_CHOICE || pt->count != 0 || pt->group != group ) {
			continue;
		}
		part_rect(p, pt, &r);
		if ( !any ) {
			*out = r;
			any = TRUE;
		} else {
			if ( r.left < out->left )     out->left = r.left;
			if ( r.top < out->top )       out->top = r.top;
			if ( r.right > out->right )   out->right = r.right;
			if ( r.bottom > out->bottom ) out->bottom = r.bottom;
		}
	}
}

/* Whether a part is the first choice of its group, where the box is drawn */
LOCAL BOOL group_first( CONST PANEL *p, INT i )
{
	INT	k;

	for ( k = 0; k < i; k++ ) {
		if ( WM_PT_KIND(p->def.part[k].type) == WM_PT_CHOICE && p->def.part[k].count == 0
		  && p->def.part[k].group == p->def.part[i].group ) {
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * One part of a panel drawn, when it is shown. The kinds a program
 * works are drawn as BTRON's are (pdraw.c), with what the panel knows
 * of the part now: whether it has the keys, what is held of it, the
 * caret's blink and a conversion under way in it. A part that was given
 * looks of its own is drawn in them.
 */
/*
 * The icons icon parts show, decoded once and kept: an object's icon is
 * read when a part is given the object (knl_pn_set_state, in the task
 * of the program that sets it), never while the panel is being drawn.
 * Drawn in the middle of the part's rectangle, made smaller to fit
 * when it is larger, the icon's clear pixels leaving the panel as it is.
 */
#define ICON_KEEP	24

#define ICON_ID		64

typedef struct {
	UB	id[ICON_ID];		/* the UUID as it was given, or a picture's href */
	UW	*px;			/* NULL: the object has no icon */
	INT	w, h;
	UW	used_at;		/* for letting the oldest go */
} ICONKEEP;

LOCAL ICONKEEP	icon_keep[ICON_KEEP];
LOCAL UW	icon_tick = 0;

LOCAL BOOL icon_same( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < ICON_ID - 1 && a[i] == b[i] && a[i] != 0; i++ ) {
		;
	}
	return ( a[i] == b[i] );
}

LOCAL ICONKEEP *icon_find( CONST UB *id )
{
	INT	i;

	for ( i = 0; i < ICON_KEEP; i++ ) {
		if ( icon_keep[i].id[0] != 0 && icon_same(icon_keep[i].id, id) ) {
			icon_keep[i].used_at = ++icon_tick;
			return &icon_keep[i];
		}
	}
	return NULL;
}

/* The icon of object id read and kept, unless it is kept already */
LOCAL void icon_load( CONST UB *id )
{
	TS_UUID		u;
	ICONKEEP	*k;
	UB		*ico;
	SZ		size = 0;
	INT		i, w = 0, h = 0;
	UW		*px = NULL;

	if ( id[0] == 0 || icon_find(id) != NULL || ts_str_to_uuid((CONST char *)id, &u) < E_OK ) {
		return;
	}
	ico = om_obj_icon(&u, &size);
	if ( ico != NULL ) {
		if ( img_ico_decode(ico, size, &px, &w, &h) < E_OK ) {
			px = NULL;
		}
		Kfree(ico);
	}
	k = &icon_keep[0];
	for ( i = 1; i < ICON_KEEP; i++ ) {
		if ( icon_keep[i].used_at < k->used_at ) {
			k = &icon_keep[i];
		}
	}
	if ( k->px != NULL ) {
		Kfree(k->px);
	}
	knl_memset(k, 0, sizeof(*k));
	for ( i = 0; i < ICON_ID - 1 && id[i] != 0; i++ ) {
		k->id[i] = id[i];
	}
	k->px = px;
	k->w = w;
	k->h = h;
	k->used_at = ++icon_tick;
}

/* A slot for something to keep: the one least lately used, emptied */
LOCAL ICONKEEP *icon_slot( void )
{
	ICONKEEP	*k = &icon_keep[0];
	INT		i;

	for ( i = 1; i < ICON_KEEP; i++ ) {
		if ( icon_keep[i].used_at < k->used_at ) {
			k = &icon_keep[i];
		}
	}
	if ( k->px != NULL ) {
		Kfree(k->px);
	}
	knl_memset(k, 0, sizeof(*k));
	return k;
}

EXPORT void knl_pn_pict_load( CONST UB *href )
{
	TS_UUID		u;
	CONST UB	*name;
	ICONKEEP	*k;
	UB		*png;
	SZ		size = 0;
	UW		*px = NULL;
	INT		i, w = 0, h = 0;

	if ( href == NULL || href[0] == 0 || icon_find(href) != NULL || !om_obj_href(href, &u, &name) ) {
		return;
	}
	png = om_obj_res(&u, name, &size);
	if ( png != NULL ) {
		if ( img_png_decode(png, size, &px, &w, &h) < E_OK ) {
			px = NULL;
		}
		Kfree(png);
	}
	k = icon_slot();
	for ( i = 0; i < ICON_ID - 1 && href[i] != 0; i++ ) {
		k->id[i] = href[i];
	}
	k->px = px;
	k->w = w;
	k->h = h;
	k->used_at = ++icon_tick;
}

LOCAL void icon_draw( INT gid, CONST T_DPRECT *r, CONST UB *id )
{
	ICONKEEP	*k = icon_find(id);
	INT		rw = r->right - r->left, rh = r->bottom - r->top, s, w, h, x, y;
	UW		*sm;

	if ( k == NULL || k->px == NULL || rw <= 0 || rh <= 0 ) {
		return;
	}
	s = k->w;
	if ( k->h > s ) s = k->h;
	w = k->w;
	h = k->h;
	if ( s > rw || s > rh ) {
		INT	d = ( rw < rh ) ? rw : rh;

		w = k->w * d / s;
		h = k->h * d / s;
	}
	if ( w <= 0 || h <= 0 ) {
		return;
	}
	if ( dp_scale(gid) > 100 ) {
		T_DPRECT	b;

		/* at a scale the picture is taken again at the box's own pixels, from all of its detail */
		b.left = r->left + ( rw - w ) / 2;
		b.top = r->top + ( rh - h ) / 2;
		b.right = b.left + w;
		b.bottom = b.top + h;
		(void)dp_put_image(gid, &b, k->px, k->w, k->h, k->w, IMG_CLEAR);
		return;
	}
	if ( w == k->w && h == k->h ) {
		(void)dp_put_argb(gid, r->left + ( rw - w ) / 2, r->top + ( rh - h ) / 2, k->px, k->w, w, h, IMG_CLEAR);
		return;
	}
	sm = (UW *)Kmalloc((SZ)w * h * sizeof(UW));
	if ( sm == NULL ) {
		return;
	}
	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < w; x++ ) {
			sm[y * w + x] = k->px[( y * k->h / h ) * k->w + x * k->w / w];
		}
	}
	(void)dp_put_argb(gid, r->left + ( rw - w ) / 2, r->top + ( rh - h ) / 2, sm, w, w, h, IMG_CLEAR);
	Kfree(sm);
}

LOCAL void draw_part_body( PANEL *p, INT pid, INT gid, INT i, INT rad, INT margin );

/*
 * A part of a scrolling sheet is drawn only inside what shows of the
 * sheet, and not at all when none of it is there.
 */
LOCAL void draw_part_at( PANEL *p, INT pid, INT gid, INT i, INT rad, INT margin )
{
	T_WMPART	*pt = &p->def.part[i];
	T_DPRECT	r, v, c;
	T_DPENV		env;

	if ( !part_scrolls(p, pt) ) {
		draw_part_body(p, pid, gid, i, rad, margin);
		return;
	}
	if ( p->vvalid ) {
		v = p->vcache;
	} else if ( !pn_view(p, &v, NULL) ) {
		draw_part_body(p, pid, gid, i, rad, margin);
		return;
	}
	part_rect(p, pt, &r);
	if ( r.bottom <= v.top || r.top >= v.bottom || dp_ref(gid, &env) < E_OK ) {
		return;
	}
	c = env.visible;
	if ( c.left < v.left )     c.left = v.left;
	if ( c.top < v.top )       c.top = v.top;
	if ( c.right > v.right )   c.right = v.right;
	if ( c.bottom > v.bottom ) c.bottom = v.bottom;
	if ( c.right <= c.left || c.bottom <= c.top ) {
		return;
	}
	dp_set_visible(gid, &c);
	draw_part_body(p, pid, gid, i, rad, margin);
	dp_set_visible(gid, &env.visible);
}


/* ---------------------------------------------------------------- choosing a colour */

#define CHOOSER_ROW	512		/* pixels of a row laid at once */

LOCAL UW chooser_rgb( INT h, INT s, INT v )
{
	INT	f = h % 60;
	INT	p = v * ( 255 - s ) / 255;
	INT	q = v * ( 255 - s * f / 60 ) / 255;
	INT	t = v * ( 255 - s * ( 60 - f ) / 60 ) / 255;
	INT	r, g, b;

	switch ( h / 60 ) {
	case 0:		r = v; g = t; b = p;	break;
	case 1:		r = q; g = v; b = p;	break;
	case 2:		r = p; g = v; b = t;	break;
	case 3:		r = p; g = q; b = v;	break;
	case 4:		r = t; g = p; b = v;	break;
	default:	r = v; g = p; b = q;	break;
	}

	return ( (UW)r << 16 ) | ( (UW)g << 8 ) | (UW)b;
}

#define HUE_MARK	16		/* across the hue bar's mark, and how high its rectangle should be */
#define HUE_BAR_H	10

LOCAL void chooser_oval( INT gid, INT x, INT y, INT d, UW colour, INT frame )
{
	T_DPRECT	m;
	T_DPPAT		pat;

	m.left = x - d / 2;  m.top = y - d / 2;
	m.right = m.left + d;  m.bottom = m.top + d;
	dp_pat_colour(&pat, colour);
	if ( frame > 0 ) {
		dp_frame_oval(gid, &m, frame, &pat);
	} else {
		dp_fill_oval(gid, &m, &pat);
	}
}

LOCAL void svmap_draw( INT gid, CONST T_DPRECT *r, INT value )
{
	UW	row[CHOOSER_ROW];
	INT	w = r->right - r->left, h = r->bottom - r->top, x, y, hue = WM_SVMAP_H(value);
	INT	s = WM_SVMAP_S(value), v = WM_SVMAP_V(value);
	UW	c;

	if ( w <= 1 || h <= 1 || w > CHOOSER_ROW || hue >= 360 ) {
		return;
	}
	for ( y = 0; y < h; y++ ) {
		INT	vy = 255 - y * 255 / ( h - 1 );

		for ( x = 0; x < w; x++ ) {
			row[x] = chooser_rgb(hue, x * 255 / ( w - 1 ), vy);
		}
		dp_put_argb(gid, r->left, r->top + y, row, w, w, 1, 0xFFFFFFFFU);
	}
	/* the ring dark on what is light and light on what is dark */
	c = chooser_rgb(hue, s, v);
	x = (INT)( ( c >> 16 ) & 0xFF ) * 3 + (INT)( ( c >> 8 ) & 0xFF ) * 6 + (INT)( c & 0xFF );
	chooser_oval(gid, r->left + s * ( w - 1 ) / 255, r->top + ( 255 - v ) * ( h - 1 ) / 255,
		     14, ( x > 1280 ) ? 0x00000000U : 0x00FFFFFFU, 2);
}

/* The bar lies this far in from its rectangle's ends, so that the mark stays inside at either */
LOCAL void huebar_draw( INT gid, CONST T_DPRECT *r, INT value )
{
	UW	row[CHOOSER_ROW];
	INT	w = r->right - r->left - HUE_MARK, h = HUE_BAR_H, x, y, k;
	INT	l = r->left + HUE_MARK / 2, t = r->top + ( r->bottom - r->top - h ) / 2;

	dp_fill_rect(gid, r, 0x00FFFFFFU);
	if ( w <= 1 || w > CHOOSER_ROW ) {
		return;
	}
	for ( x = 0; x < w; x++ ) {
		row[x] = chooser_rgb(359 - x * 359 / ( w - 1 ), 255, 255);
	}
	for ( y = 0; y < h; y++ ) {
		/* the ends round: what a row leaves out at each end */
		INT	dy = 2 * y + 1 - h, in = 0;

		for ( k = 0; k < h / 2; k++ ) {
			INT	dx = h - 2 * k - 1;

			if ( dx * dx + dy * dy <= h * h ) {
				break;
			}
			in = k + 1;
		}
		dp_put_argb(gid, l + in, t + y, row + in, w, w - 2 * in, 1, 0xFFFFFFFFU);
	}
	x = l + ( 359 - value ) * ( w - 1 ) / 359;
	y = t + h / 2;
	chooser_oval(gid, x, y, HUE_MARK, 0x00FFFFFFU, 0);
	chooser_oval(gid, x, y, HUE_MARK - 4, chooser_rgb(value, 255, 255), 0);
	chooser_oval(gid, x, y, HUE_MARK, 0x00808080U, 1);
}

LOCAL void coldot_draw( INT gid, CONST T_DPRECT *r, UW colour )
{
	T_DPPAT	pat;

	dp_pat_colour(&pat, colour);
	dp_fill_oval(gid, r, &pat);
	dp_pat_colour(&pat, 0x00A0A0A0U);
	dp_frame_oval(gid, r, 1, &pat);
}

/* A dropper leaning to the right: its stem, the collar across it, and the bulb */
LOCAL void dropper_draw( INT gid, CONST T_DPRECT *r )
{
	T_DPPAT	pat;
	INT	x = r->left + ( r->right - r->left - 18 ) / 2, y = r->top + ( r->bottom - r->top - 18 ) / 2;

	dp_pat_colour(&pat, 0x00000000U);
	dp_line_wide(gid, x + 1, y + 16, x + 9, y + 8, 2, 0, &pat);
	dp_line_wide(gid, x + 7, y + 4, x + 13, y + 10, 2, 0, &pat);
	dp_line_wide(gid, x + 10, y + 7, x + 15, y + 2, 5, 0, &pat);
}

/* Where the pointer stands, held to the part's ends, taken into its value */
LOCAL void chooser_at( T_WMPART *pt, CONST T_DPRECT *r, INT px, INT py )
{
	INT	w = r->right - r->left - 1, h = r->bottom - r->top - 1;
	INT	x = px - r->left, y = py - r->top;

	if ( WM_PT_KIND(pt->type) == WM_PT_HUEBAR ) {
		w -= HUE_MARK;
		x -= HUE_MARK / 2;
		if ( w <= 0 ) {
			return;
		}
		if ( x < 0 ) x = 0;
		if ( x > w ) x = w;
		pt->value = 359 - x * 359 / w;
		return;
	}
	if ( w <= 0 || h <= 0 ) {
		return;
	}
	if ( x < 0 ) x = 0;
	if ( x > w ) x = w;
	if ( y < 0 ) y = 0;
	if ( y > h ) y = h;
	pt->value = WM_SVMAP_VALUE(WM_SVMAP_H(pt->value), x * 255 / w, 255 - y * 255 / h);
}

LOCAL void draw_part_body( PANEL *p, INT pid, INT gid, INT i, INT rad, INT margin )
{
	T_WMPART	*pt = &p->def.part[i];
	T_DPRECT	r;
	UINT		kind = WM_PT_KIND(pt->type);
	UW		face;

	if ( !pn_shown(p, pt) ) {
		return;
	}
	part_rect(p, pt, &r);
	face = wm_look(PART_GREY(pt) ? WM_LOOK_PART_OFF : WM_LOOK_PART_FACE);
	if ( pid > 0 && knl_wmobj_part_look(pid, i, gid, &r,
					    p->pressed == i || ( kind != MS_PARTS && pt->value != 0 ) ) ) {
		return;
	}
	switch ( kind ) {
	case WM_PT_ICON:
		icon_draw(gid, &r, pt->label);
		break;

	case WM_PT_SVMAP:
		svmap_draw(gid, &r, pt->value);
		break;

	case WM_PT_HUEBAR:
		huebar_draw(gid, &r, pt->value);
		break;

	case WM_PT_COLDOT:
		coldot_draw(gid, &r, pt->colour);
		break;

	case WM_PT_DROPPER:
		dropper_draw(gid, &r);
		break;

	case WM_PT_SWATCH:
		dp_fill_rect(gid, &r, pt->colour);
		dp_frame_rect(gid, &r, wm_look(WM_LOOK_FRAME), 1);
		if ( pt->value != 0 ) {
			T_DPRECT	e = r;

			e.left += 2;  e.top += 2;
			e.right -= 2; e.bottom -= 2;
			if ( e.right > e.left && e.bottom > e.top ) {
				dp_frame_rect(gid, &e,
					      wm_look(WM_LOOK_PART_EMPH), 2);
			}
		}
		break;

	default: {
		T_PDCTX	c;

		knl_memset(&c, 0, sizeof(c));
		c.gid = gid;
		c.pool = p->def.pool;
		c.focus = (BOOL)( p->focus == i );
		c.held = ( p->pressed == i && p->held_in ) ? ( ( p->held_half != 0 ) ? p->held_half : 1 ) : 0;
		c.held_at = ( p->pressed == i && p->held_in ) ? p->held_at : 0;
		c.caret = pn_lit;
		if ( kind == WM_PT_CHOICE && pt->count == 0 ) {
			c.first = group_first(p, i);
			group_rect(p, pt->group, &c.group);
		}
		if ( kind == WM_PT_POPUP ) {
			c.held = ( ls_pid > 0 && ls_owner == pid && ls_part == i ) ? 1 : 0;
		}
		if ( pn_conv != NULL && pn_conv_pid == pid && pn_conv_i == i ) {
			c.conv = pn_conv;
		}
		pd_part(pt, &r, &c);
		if ( kind == MS_PARTS && pt->text[0] != 0 ) {
			/* a button with a picture: in the middle, inside its edge */
			T_DPRECT	in = r;

			in.left += 3;  in.top += 3;
			in.right -= 3; in.bottom -= 3;
			icon_draw(gid, &in, pt->text);
		}
		return;
	}
	}
	/* a part that cannot be worked is veiled in a half-tone of white */
	if ( PART_GREY(pt) ) {
		T_DPPAT	v;

		veil_pat(&v);
		dp_fill_round(gid, &r, rad, rad, &v);
	}
	(void)face;
	(void)margin;
}

LOCAL ER panel_draw( INT pid );

#if USE_UITEST
/* The panels drawn whole, and one being drawn now: read by the UI tests' pnstat (dtuitest.c) */
EXPORT volatile UW	knl_pn_draws = 0;
EXPORT volatile UW	knl_pn_drawing = 0;
#endif

EXPORT ER wm_panel_draw( INT pid )
{
	ER	er;

	pn_lock();
#if USE_UITEST
	knl_pn_draws++;
	knl_pn_drawing = 1;
#endif
	er = panel_draw(pid);
#if USE_UITEST
	knl_pn_drawing = 0;
#endif
	pn_unlock();
	return er;
}

LOCAL ER panel_draw( INT pid )
{
	PANEL		*p = pn_of(pid);
	INT		gid, i, rad, margin;

	if ( p == NULL ) {
		return E_ID;
	}
	gid = wm_gid(p->wid);
	if ( gid < 0 ) {
		return (ER)gid;
	}
	if ( p->is_menu ) {
		menu_paint(p);
		return E_OK;
	}
	rad = (INT)wm_look(WM_LOOK_PART_RAD);
	if ( rad <= 0 ) {
		rad = 12;
	}
	margin = part_margin();
	if ( p->scr >= 0 ) {
		pn_settle(p);			/* another sheet shown starts from its top */
	}

	panel_body(p, gid);

	p->vvalid = (BOOL)( p->scr >= 0 && pn_view(p, &p->vcache, NULL) );
	for ( i = 0; i < p->def.npart; i++ ) {
		draw_part_at(p, pid, gid, i, rad, margin);
	}
	p->vvalid = FALSE;
	wm_damage(p->wid, &p->def.r);
	if ( p->scr >= 0 ) {
		pn_bar(p);
	}
	if ( pn_conv != NULL && pn_conv_pid == pid && pn_conv_i >= 0 && pn_conv->list ) {
		T_DPRECT	r;
		T_WMWIN		w;
		INT		h = 4096;

		if ( wm_ref(p->wid, &w) >= E_OK ) {
			h = w.work.bottom - w.work.top;
		}
		part_rect(p, &p->def.part[pn_conv_i], &r);
		pd_cands(gid, &r, pn_conv, h);
		wm_damage(p->wid, &pn_conv->box);
	}

	return E_OK;
}

/*
 * One part drawn again, and only it put on the screen. While a knob is
 * carried the part changes with every move of the pointer; drawing the
 * whole panel each time paints its ground over every part first, and
 * whoever builds the screen meanwhile shows the panel half drawn.
 */
LOCAL void pn_draw_part( PANEL *p, INT pid, INT i )
{
	T_DPRECT	r;
	INT		gid, rad, margin;

	if ( i < 0 || i >= p->def.npart || p->is_menu ) {
		return;
	}
	gid = wm_gid(p->wid);
	if ( gid < 0 ) {
		return;
	}
	rad = (INT)wm_look(WM_LOOK_PART_RAD);
	if ( rad <= 0 ) {
		rad = 12;
	}
	margin = part_margin();
	pn_lock();
	draw_part_at(p, pid, gid, i, rad, margin);
	part_rect(p, &p->def.part[i], &r);
	pn_unlock();
	r.left -= margin + 2;   r.top -= margin + 2;
	r.right += margin + 2;  r.bottom += margin + 2;
	wm_damage(p->wid, &r);
}

/* A volume and the number box that follows it, drawn again */
LOCAL void vl_draw( PANEL *p, INT pid, INT i )
{
	T_WMPART	*pt = &p->def.part[i], *nb;

	pn_draw_part(p, pid, i);
	if ( pt->sub != 0 && ( nb = part_of(p, pt->sub) ) != NULL ) {
		pn_draw_part(p, pid, (INT)( nb - p->def.part ));
	}
}

/* ---------------------------------------------------------------- events */

/* A key as a letter, in the latin layout in use (ts/keymap.h) */
EXPORT UB wm_key_char( UINT code, UINT mods )
{
	return km_char(code, mods);
}


/* A letter typed into a box, or the caret moved. TRUE when something
 * changed and the panel wants drawing again. */
/* A digit typed into a number, or the number stepped */
LOCAL BOOL num_key( T_WMPART *pt, UINT code, UINT mods )
{
	UB	c = wm_key_char(code, mods);
	INT	v = pt->value;

	switch ( code ) {
	case 0x52:				/* up */
		pt->value = v + 1;
		return TRUE;
	case 0x51:				/* down */
		pt->value = v - 1;
		return TRUE;
	case 0x2A:				/* what was typed last */
		pt->value = v / 10;
		return TRUE;
	default:
		break;
	}
	if ( c >= '0' && c <= '9' ) {
		pt->value = v * 10 + ( c - '0' );
		return TRUE;
	}
	if ( c == '-' ) {
		pt->value = -v;
		return TRUE;
	}

	return FALSE;
}

/*
 * Where the letter before or after a place in UTF-8 text starts. A byte
 * of the form 10xxxxxx is the middle of a letter, never its start.
 */
LOCAL INT utf8_back( CONST UB *t, INT at )
{
	if ( at > 0 ) {
		at--;
	}
	while ( at > 0 && ( t[at] & 0xC0 ) == 0x80 ) {
		at--;
	}

	return at;
}

LOCAL INT utf8_next( CONST UB *t, INT at, INT len )
{
	if ( at < len ) {
		at++;
	}
	while ( at < len && ( t[at] & 0xC0 ) == 0x80 ) {
		at++;
	}

	return at;
}

LOCAL BOOL box_key( T_WMPART *pt, UINT code, UINT mods )
{
	INT	len = 0, i;
	UB	c;

	if ( PART_GREY(pt) ) {
		return FALSE;
	}
	if ( WM_PT_KIND(pt->type) == WM_PT_NUMBOX ) {
		return num_key(pt, code, mods);
	}
	if ( WM_PT_KIND(pt->type) == SB_PARTS ) {
		/* the field the caret is in takes the digits */
		INT	f = pt->sb_at;

		if ( f < 0 || f >= WM_SB_MAX ) {
			f = 0;
		}
		if ( code == 0x4F && f + 1 < pt->sb_n ) {
			pt->sb_at = f + 1;
			return TRUE;
		}
		if ( code == 0x50 && f > 0 ) {
			pt->sb_at = f - 1;
			return TRUE;
		}
		{
			UB	d = wm_key_char(code, mods);

			if ( d >= '0' && d <= '9' ) {
				INT	v = pt->sbv[f] * 10 + ( d - '0' );

				if ( pt->sbhi[f] != pt->sblo[f] && v > pt->sbhi[f] ) {
					v = d - '0';
				}
				pt->sbv[f] = v;
				return TRUE;
			}
			if ( code == 0x2A ) {
				pt->sbv[f] /= 10;
				return TRUE;
			}
		}
		return FALSE;
	}
	if ( WM_PT_KIND(pt->type) != WM_PT_BOX ) {
		return FALSE;
	}
	while ( len < WM_LABEL_MAX - 1 && pt->text[len] != 0 ) {
		len++;
	}
	if ( pt->caret > len ) {
		pt->caret = len;
	}
	/*
	 * The text is UTF-8, and the caret moves and deletes by letter, not
	 * by byte: a letter of a Japanese name is three bytes, and taking
	 * one of them away leaves bytes that are no letter at all.
	 */
	switch ( code ) {
	case 0x50:				/* left */
		if ( pt->caret > 0 ) {
			pt->caret = utf8_back(pt->text, pt->caret);
			return TRUE;
		}
		return FALSE;
	case 0x4F:				/* right */
		if ( pt->caret < len ) {
			pt->caret = utf8_next(pt->text, pt->caret, len);
			return TRUE;
		}
		return FALSE;
	case 0x4A:				/* home */
		pt->caret = 0;
		return TRUE;
	case 0x4D:				/* end */
		pt->caret = len;
		return TRUE;
	case 0x2A:				/* the letter before the caret */
		if ( pt->caret <= 0 ) {
			return FALSE;
		}
		{
			INT	from = utf8_back(pt->text, pt->caret);
			INT	gone = pt->caret - from;

			for ( i = from; i + gone <= len; i++ ) {
				pt->text[i] = pt->text[i + gone];
			}
			pt->caret = from;
		}
		return TRUE;
	case 0x4C:				/* and the one after it */
		if ( pt->caret >= len ) {
			return FALSE;
		}
		{
			INT	gone = utf8_next(pt->text, pt->caret, len)
				     - pt->caret;

			for ( i = pt->caret; i + gone <= len; i++ ) {
				pt->text[i] = pt->text[i + gone];
			}
		}
		return TRUE;
	default:
		break;
	}
	c = wm_key_char(code, mods);
	if ( c < 0x20 || c == 0x7F || len >= WM_LABEL_MAX - 1 ) {
		return FALSE;
	}
	for ( i = len; i > pt->caret; i-- ) {
		pt->text[i] = pt->text[i - 1];
	}
	pt->text[pt->caret] = c;
	pt->text[len + 1] = 0;
	pt->caret++;

	return TRUE;
}

/*
 * A press in a list: on a row it chooses that name, on the bar it winds
 * the list to where the knob was taken. Nothing here scrolls by itself;
 * a list shows what it was told to show.
 */
LOCAL void list_press( PANEL *p, T_WMPART *pt, INT x, INT y )
{
	T_DPRECT	r;
	INT		rowh = list_rowh(), rows, k, len, off, klen, at;
	T_WMBAR		bar;

	part_rect(p, pt, &r);
	rows = list_rows(&r);
	pt->rows = rows;
	if ( x >= r.right - LIST_BAR_W ) {
		/* the bar: the knob's middle goes where the press was */
		len = (r.bottom - r.top) - 2;
		bar.lo = 0;
		bar.hi = ( pt->count > 0 ) ? pt->count : 1;
		bar.clo = ( pt->top > 0 ) ? pt->top - 1 : 0;
		bar.chi = bar.clo + rows;
		if ( bar.chi > bar.hi ) {
			bar.chi = bar.hi;
		}
		if ( bar.hi <= rows ) {
			return;			/* all of it is shown */
		}
		wm_bar_knob(&bar, len, &off, &klen);
		at = y - (r.top + 1) - klen / 2;
		if ( at < 0 ) {
			at = 0;
		}
		if ( at > len - klen ) {
			at = len - klen;
		}
		pt->top = 1 + ( ( len - klen > 0 )
				? (INT)(((D)at * (bar.hi - rows)) / (len - klen))
				: 0 );
		if ( pt->top < 1 ) {
			pt->top = 1;
		}
		if ( pt->top > pt->count - rows + 1 ) {
			pt->top = pt->count - rows + 1;
		}
		return;
	}
	k = ( y - (r.top + 2) ) / rowh;
	if ( k >= 0 && k < rows ) {
		INT	idx = pt->top + k;

		if ( idx >= 1 && idx <= pt->count ) {
			pt->value = idx;
		}
	}
}


/* Which part is at a place in the work area, or -1 */
LOCAL INT part_at( PANEL *p, INT x, INT y )
{
	T_DPRECT	r, v;
	INT		i;
	BOOL		in_view;

	in_view = (BOOL)( pn_view(p, &v, NULL) && rect_has(&v, x, y) );
	for ( i = p->def.npart - 1; i >= 0; i-- ) {
		T_WMPART *pt = &p->def.part[i];

		if ( WM_PT_KIND(pt->type) == WM_PT_LABEL || WM_PT_KIND(pt->type) == WM_PT_LINE || WM_PT_KIND(pt->type) == WM_PT_ICON
		  || PART_GREY(pt) || !pn_shown(p, pt) ) {
			continue;		/* it takes nothing */
		}
		if ( part_scrolls(p, pt) && !in_view ) {
			continue;		/* scrolled out of what shows of the sheet */
		}
		part_rect(p, pt, &r);
		if ( rect_has(&r, x, y) ) {
			return i;
		}
	}

	return -1;
}

/*
 * Pressing a part. A choice turns its whole group off and itself on, so
 * that at most one of a group is ever on and the check for it can be an
 * invariant rather than a hope.
 */
LOCAL void take_press( PANEL *p, INT i )
{
	T_WMPART	*pt = &p->def.part[i];
	INT		k;

	switch ( WM_PT_KIND(pt->type) ) {
	case WM_PT_CHOICE:
		for ( k = 0; k < p->def.npart; k++ ) {
			if ( WM_PT_KIND(p->def.part[k].type) == WM_PT_CHOICE
			  && p->def.part[k].group == pt->group ) {
				p->def.part[k].value = 0;
			}
		}
		pt->value = 1;
		break;
	case WM_PT_CHECK:
		pt->value = ( pt->value != 0 ) ? 0 : 1;
		break;
	case WM_PT_SWATCH:
		for ( k = 0; k < p->def.npart; k++ ) {
			if ( WM_PT_KIND(p->def.part[k].type) == WM_PT_SWATCH
			  && p->def.part[k].group == pt->group ) {
				p->def.part[k].value = 0;
			}
		}
		pt->value = 1;
		break;
	default:
		break;
	}
}

LOCAL ER menu_event( INT pid, CONST T_WMEV *ev, UINT *p_answer )
{
	PANEL	*p = pn_of(pid);
	INT	i;

	if ( p == NULL ) {
		return E_ID;
	}
	if ( ev == NULL ) {
		return E_PAR;
	}
	if ( p_answer != NULL ) {
		*p_answer = WM_ANS_NONE;
	}
	/*
	 * A panel in a window of its own takes the pointer there, and the
	 * keys through the window it was opened for, which is the one
	 * that has the input.
	 */
	if ( ev->wid != p->wid
	  && !( p->popup > 0 && !p->is_menu && ev->wid == p->owner_wid
	     && ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) ) ) {
		return E_OK;			/* another window's business */
	}
	switch ( ev->type ) {
	case HID_EV_MOVE:
		/* a menu follows the pointer; a panel does not */
		if ( p->is_menu ) {
			/*
			 * While the pointer is in a menu this one opened, the
			 * row it came from stays lit and stays open: the
			 * pointer has not left the row, it has gone into what
			 * the row holds.
			 */
			if ( menu_in_child(p, ev->wid) ) {
				break;
			}
			/*
			 * A list opened by a click on its row is held open: the
			 * pointer passing over the other rows on the way to it,
			 * or anywhere else, does not put it away. Only a click
			 * on another row does (HID_EV_BTN_UP below).
			 */
			if ( p->kept && p->child > 0 ) {
				break;
			}
			p->kept = FALSE;
			i = part_at(p, ev->x, ev->y);
			if ( i != p->hot ) {
				p->hot = i;
				if ( p->child > 0 ) {
					INT	c = p->child;

					p->child = 0;
					wm_panel_close(c);
				}
				wm_panel_draw(pid);
				/*
				 * The list a row holds opens once the pointer has
				 * stayed on it as long as ユーザ環境設定 says (メニュー
				 * 反応時間); passing over rows opens nothing.
				 */
				p->pend = -1;
				if ( i >= 0 ) {
					if ( wm_num(LK_MENU_DLY, 100) <= 0 ) {
						menu_sub_open(pid, i);
					} else {
						p->pend = i;
						p->pend_at = ev->when;
					}
				}
			}
		}
		break;
	case HID_EV_BTN_DOWN:
		i = part_at(p, ev->x, ev->y);
		p->pressed = i;
		if ( i >= 0 ) {
			T_WMPART	*pt = &p->def.part[i];
			INT		was = pt->value;

			if ( ( WM_PT_KIND(pt->type) == WM_PT_BOX
			    || WM_PT_KIND(pt->type) == WM_PT_NUMBOX
			    || WM_PT_KIND(pt->type) == SB_PARTS )
			  && !PART_GREY(pt) ) {
				p->focus = i;		/* letters go here now */
			} else if ( WM_PT_KIND(pt->type) == WM_PT_LIST && !PART_GREY(pt) ) {
				list_press(p, pt, ev->x, ev->y);
			}
			take_press(p, i);
			wm_panel_draw(pid);
			knl_wmobj_part(pid, i, OB_E_PRESS, ev);
			if ( pt->value != was ) {
				knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
			}
		}
		break;
	case HID_EV_BTN_UP:
		i = p->pressed;
		p->pressed = -1;
		/*
		 * A menu is chosen in by letting go over a row, wherever
		 * the button went down: the press that opened it was not
		 * in it, and carrying the pointer to a row and letting go
		 * there is one of the two ways a menu is used.
		 */
		if ( i < 0 && p->is_menu ) {
			i = part_at(p, ev->x, ev->y);
		}
		if ( i >= 0 && i < p->def.npart ) {
			T_WMPART *pt = &p->def.part[i];

			/* the answer is given on the release, not the press:
			   pressing and moving away is how a person changes
			   their mind */
			if ( part_at(p, ev->x, ev->y) == i ) {
				if ( p->is_menu && pt->sub != 0 ) {
					/* a row with a list under it is not chosen: its list is held open */
					menu_sub_keep(pid, i);
				} else if ( p->is_menu && pt->num != 0 ) {
					p->cmd = pt->num;
					p->answer = WM_ANS_OK;
				} else if ( pt->answer != WM_ANS_NONE ) {
					p->answer = pt->answer;
				}
			}
			wm_panel_draw(pid);
			knl_wmobj_part(pid, i, OB_E_RELEASE, ev);
		}
		if ( p_answer != NULL ) {
			*p_answer = p->answer;
		}
		break;
	case HID_EV_KEY_DOWN:
		if ( ev->code == 0x29 ) {	/* escape abandons it */
			p->answer = WM_ANS_CANCEL;
			if ( p_answer != NULL ) {
				*p_answer = p->answer;
			}
		} else if ( p->focus >= 0 && p->focus < p->def.npart ) {
			if ( box_key(&p->def.part[p->focus], ev->code, ev->mods) ) {
				wm_panel_draw(pid);
				knl_wmobj_part(pid, p->focus, OB_E_CHANGE, ev);
			}
		}
		break;
	default:
		break;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- a panel's parts worked */

#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_DEL		0x4C
#define K_HOME		0x4A
#define K_END		0x4D
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define K_KPENTER	0x58
#define K_HENKAN	0x8A
#define K_MUHENKAN	0x8B
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )

LOCAL INT tlen( CONST UB *t )
{
	INT	n = 0;

	while ( n < WM_LABEL_MAX - 1 && t[n] != 0 ) n++;
	return n;
}

LOCAL INT tletters( CONST UB *t, INT n )
{
	INT	i, k = 0;

	for ( i = 0; i < n; i++ ) {
		if ( ( t[i] & 0xC0 ) != 0x80 ) k++;
	}
	return k;
}

/* Letters put into a box at its caret, whole letters and as many as it may hold */
LOCAL BOOL box_put( T_WMPART *pt, CONST UB *s, INT n )
{
	INT	len = tlen(pt->text), have = tletters(pt->text, len), i, k = 0;

	if ( pt->caret < 0 || pt->caret > len ) pt->caret = len;
	while ( k < n ) {
		INT	m = 1;

		while ( k + m < n && ( s[k + m] & 0xC0 ) == 0x80 ) m++;
		if ( ( pt->max > 0 && have >= pt->max ) || len + m >= WM_LABEL_MAX - 1 ) {
			break;
		}
		for ( i = len; i >= pt->caret; i-- ) pt->text[i + m] = pt->text[i];
		for ( i = 0; i < m; i++ ) pt->text[pt->caret + i] = s[k + i];
		pt->caret += m;
		len += m;
		have++;
		k += m;
	}
	return (BOOL)( k > 0 );
}

/* The letters chosen in a box taken out */
LOCAL BOOL box_cut( T_WMPART *pt )
{
	INT	len = tlen(pt->text), a = pt->sb_at, b = pt->caret, i;

	if ( a < 0 || a == b ) {
		pt->sb_at = -1;
		return FALSE;
	}
	if ( a > b ) { INT t = a; a = b; b = t; }
	if ( b > len ) b = len;
	for ( i = a; i + ( b - a ) <= len; i++ ) pt->text[i] = pt->text[i + ( b - a )];
	pt->caret = a;
	pt->sb_at = -1;
	return TRUE;
}

/* The highest a field may hold: its range, or as many nines as it is wide */
LOCAL INT sb_hi( CONST T_WMPART *pt, INT f )
{
	INT	m = 1, k;

	if ( pt->sbhi[f] != pt->sblo[f] ) return pt->sbhi[f];
	for ( k = 0; k < pt->sbw[f] && k < 9; k++ ) m *= 10;
	return m - 1;
}

LOCAL void sb_keep( T_WMPART *pt, INT f )
{
	if ( f < 0 || f >= pt->sb_n || ( pt->sbf[f] & SBF_NAMES ) ) return;
	if ( pt->sbv[f] < pt->sblo[f] ) pt->sbv[f] = pt->sblo[f];
	if ( pt->sbv[f] > sb_hi(pt, f) ) pt->sbv[f] = sb_hi(pt, f);
}

/* A field stepped up or down, round from one end to the other */
LOCAL BOOL sb_step( T_WMPART *pt, INT f, INT d )
{
	INT	lo = ( pt->sbf[f] & SBF_NAMES ) ? 0 : pt->sblo[f], hi = sb_hi(pt, f);

	if ( f < 0 || f >= pt->sb_n ) return FALSE;
	if ( pt->type & P_BLANK ) {
		INT	k;

		for ( k = 0; k < pt->sb_n; k++ ) pt->sbv[k] = ( pt->sbf[k] & SBF_NAMES ) ? 0 : pt->sblo[k];
		pt->type &= ~P_BLANK;
	}
	pt->sbv[f] += d;
	if ( pt->sbv[f] > hi ) pt->sbv[f] = lo;
	if ( pt->sbv[f] < lo ) pt->sbv[f] = hi;
	return TRUE;
}

/* A key into a box of some kind that has the keys: TRUE when it changed */
LOCAL BOOL pn_box_key( T_WMPART *pt, UINT code, UINT mods )
{
	UINT	kind = WM_PT_KIND(pt->type);
	UB	c = wm_key_char(code, mods);
	INT	len = tlen(pt->text), f;

	if ( PART_GREY(pt) ) return FALSE;
	if ( kind == NB_PARTS ) return num_key(pt, code, mods);
	if ( kind == SB_PARTS ) {
		f = ( pt->sb_at >= 0 && pt->sb_at < pt->sb_n ) ? pt->sb_at : 0;
		if ( c >= '0' && c <= '9' && !( pt->sbf[f] & SBF_NAMES ) ) {
			INT	v, hi = sb_hi(pt, f);

			if ( pt->type & P_BLANK ) {
				INT	k;

				for ( k = 0; k < pt->sb_n; k++ ) pt->sbv[k] = ( pt->sbf[k] & SBF_NAMES ) ? 0 : pt->sblo[k];
				pt->type &= ~P_BLANK;
				pt->sbv[f] = 0;
			}
			v = pt->sbv[f] * 10 + ( c - '0' );
			if ( v > hi ) v = c - '0';
			pt->sbv[f] = v;
			/* the field full: on to the next, as the words read */
			if ( v * 10 > hi && f + 1 < pt->sb_n ) {
				sb_keep(pt, f);
				pt->sb_at = f + 1;
			}
			return TRUE;
		}
		switch ( code ) {
		case K_BS:
			if ( pt->sbf[f] & SBF_NAMES ) return FALSE;
			pt->sbv[f] /= 10;
			return TRUE;
		case K_RIGHT:
			sb_keep(pt, f);
			if ( f + 1 < pt->sb_n ) pt->sb_at = f + 1;
			return FALSE;
		case K_LEFT:
			sb_keep(pt, f);
			if ( f > 0 ) pt->sb_at = f - 1;
			return FALSE;
		case K_UP:
		case K_HENKAN:
			return sb_step(pt, f, 1);
		case K_DOWN:
		case K_MUHENKAN:
			return sb_step(pt, f, -1);
		default:
			if ( c == '.' || c == ':' || c == '/' || c == '-' ) {
				sb_keep(pt, f);
				if ( f + 1 < pt->sb_n ) pt->sb_at = f + 1;
			}
			return FALSE;
		}
	}
	if ( kind == XB_PARTS ) {
		/*
		 * marks standing in for letters kept elsewhere (XB_STANDIN, a
		 * password saved): the first key takes them all away, and what is
		 * typed then is the new letters
		 */
		if ( len > 0 && pt->text[0] == XB_STANDIN && ( code == K_BS || ( c >= 0x20 && c < 0x7F ) ) ) {
			pt->text[0] = 0;
			len = 0;
			pt->caret = 0;
			if ( code == K_BS ) return TRUE;
		}
		/* letters go on the end and come off it, one at a time */
		pt->caret = len;
		if ( code == K_BS ) {
			INT	i = len;

			if ( len == 0 ) return FALSE;
			while ( i > 0 && ( pt->text[--i] & 0xC0 ) == 0x80 ) ;
			pt->text[i] = 0;
			pt->caret = i;
			return TRUE;
		}
		if ( c < 0x20 || c >= 0x7F ) return FALSE;
		return box_put(pt, &c, 1);
	}
	if ( kind != TB_PARTS ) return FALSE;
	switch ( code ) {
	case K_LEFT:
		pt->sb_at = -1;
		if ( pt->caret > 0 ) pt->caret = utf8_back(pt->text, pt->caret);
		return FALSE;
	case K_RIGHT:
		pt->sb_at = -1;
		if ( pt->caret < len ) pt->caret = utf8_next(pt->text, pt->caret, len);
		return FALSE;
	case K_HOME:
		pt->sb_at = -1;
		pt->caret = 0;
		return FALSE;
	case K_END:
		pt->sb_at = -1;
		pt->caret = len;
		return FALSE;
	case K_BS:
	case K_DEL:
		if ( box_cut(pt) ) return TRUE;
		if ( code == K_BS ) {
			INT	from, i;

			if ( pt->caret <= 0 ) return FALSE;
			from = utf8_back(pt->text, pt->caret);
			for ( i = from; i + ( pt->caret - from ) <= len; i++ ) pt->text[i] = pt->text[i + pt->caret - from];
			pt->caret = from;
			return TRUE;
		} else {
			INT	gone, i;

			if ( pt->caret >= len ) return FALSE;
			gone = utf8_next(pt->text, pt->caret, len) - pt->caret;
			for ( i = pt->caret; i + gone <= len; i++ ) pt->text[i] = pt->text[i + gone];
			return TRUE;
		}
	default:
		break;
	}
	if ( c < 0x20 || c >= 0x7F ) return FALSE;
	if ( ( pt->type & P_DIGITS ) && ( c < '0' || c > '9' ) ) return FALSE;
	(void)box_cut(pt);
	return box_put(pt, &c, 1);
}

/* ---------------------------------------------------------------- かな漢字変換 in a box */

LOCAL BOOL pn_busy( INT pid, INT i )
{
	return (BOOL)( pn_conv != NULL && pn_conv_pid == pid && pn_conv_i == i && pn_conv->ncl > 0 );
}

LOCAL BOOL pn_ready( BOOL roman )
{
	if ( pn_kout == NULL ) {
		pn_kout = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
		pn_conv = (T_PDCONV *)Kmalloc(sizeof(T_PDCONV));
		if ( pn_kout == NULL || pn_conv == NULL ) return FALSE;
		knl_memset(pn_kout, 0, sizeof(*pn_kout));
		knl_memset(pn_conv, 0, sizeof(*pn_conv));
	}
	if ( pn_kid <= 0 ) {
		pn_kid = kc_open();
		pn_roman = -1;
	}
	if ( pn_kid > 0 && pn_roman != (INT)roman ) {
		(void)kc_input(pn_kid, roman ? TSMOZC_M_ROMAN : 0);
		pn_roman = roman;
	}
	return (BOOL)( pn_kid > 0 );
}

/*
 * What the converter gave, taken: the committed clauses into the box at
 * its caret, the rest kept as what the box shows being converted, and
 * the candidates fetched when there is a list to show.
 */
LOCAL void pn_take( PANEL *p, INT pid, INT i, INT flags )
{
	T_KCOUT		*o = pn_kout;
	T_PDCONV	*cv = pn_conv;
	T_WMPART	*pt = &p->def.part[i];
	INT		base = 0, k, n;

	if ( o->n_out > 0 && o->cl[o->n_out] > 0 ) {
		base = o->cl[o->n_out];
		(void)box_cut(pt);
		(void)box_put(pt, (CONST UB *)o->text, base);
	}
	n = o->cl[o->n_cl] - base;
	cv->ncl = 0;
	cv->list = FALSE;
	if ( n > 0 && n < PD_CONV_TEXT ) {
		knl_memcpy(cv->text, o->text + base, n);
		cv->text[n] = 0;
		for ( k = o->n_out; k <= o->n_cl && k - o->n_out <= PD_CONV_CL; k++ ) {
			cv->cl[k - o->n_out] = o->cl[k] - base;
		}
		cv->ncl = o->n_cl - o->n_out;
		cv->clause = o->yomi ? -1 : o->clause - o->n_out;
		if ( ( flags & TSMOZC_LIST ) != 0 ) {
			INT	total = 0, sel = kc_list(pn_kid, cv->cand, PD_CAND_MAX, &cv->ncand, &pn_first, &total);

			if ( sel > 0 && cv->ncand > 0 ) {
				cv->list = TRUE;
				cv->chosen = sel - pn_first;
			}
		}
	}
	pn_conv_pid = ( cv->ncl > 0 ) ? pid : 0;
	pn_conv_i = ( cv->ncl > 0 ) ? i : -1;
}

LOCAL BOOL pn_send( PANEL *p, INT pid, INT i, UINT code, UINT stat )
{
	INT	r = kc_key(pn_kid, code, stat, pn_kout);

	if ( r == KC_NOTMINE || r < 0 ) return FALSE;
	pn_take(p, pid, i, r);
	return TRUE;
}

/* Whatever is being converted, committed into the box it was begun in */
LOCAL void pn_commit( void )
{
	PANEL	*p = pn_of(pn_conv_pid);

	if ( p != NULL && pn_conv_i >= 0 && pn_busy(pn_conv_pid, pn_conv_i) ) {
		(void)pn_send(p, pn_conv_pid, pn_conv_i, TSMOZC_K_CR, 0);
	}
	if ( pn_conv != NULL ) {
		pn_conv->ncl = 0;
		pn_conv->list = FALSE;
	}
	pn_conv_pid = 0;
	pn_conv_i = -1;
}

/*
 * A key into a box of Japanese (P_JP) in 日本語: a letter to the
 * converter, romaji or the kana on the key; a key that is not a letter
 * to the converter while it is working. TRUE when the key was taken.
 */
LOCAL BOOL pn_conv_key( PANEL *p, INT pid, INT i, UINT key, UINT mods )
{
	UINT	m = wm_msg_get_mode(), code, tc;
	BOOL	shift = (BOOL)( ( mods & M_SHIFT ) != 0 );
	UINT	stat = shift ? TSMOZC_S_SHIFT : 0;
	UB	ch = wm_key_char(key, mods);
	BOOL	busy = pn_busy(pid, i);

	if ( ( m & WM_MODE_ALPH ) != 0 || !pn_ready(( m & WM_MODE_ROMAN ) != 0) ) {
		return FALSE;
	}
	if ( m & WM_MODE_KANA ) stat |= TSMOZC_S_KANA;
	if ( busy && pn_conv->list && ch >= '1' && ch <= '9' && ch - '1' < pn_conv->ncand ) {
		INT	r = kc_choose(pn_kid, pn_first + ( ch - '1' ), pn_kout);

		if ( r >= 0 && r != KC_NOTMINE ) pn_take(p, pid, i, r);
		return TRUE;
	}
	code = kc_code_key(key, mods);
	if ( code != 0 ) {
		if ( !busy ) return FALSE;
		if ( pn_conv->list && code == TSMOZC_K_PG_U ) code = TSMOZC_K_LIST_PREV;
		if ( pn_conv->list && code == TSMOZC_K_PG_D ) code = TSMOZC_K_LIST_NEXT;
		(void)pn_send(p, pid, i, code, stat);
		return TRUE;
	}
	tc = ( m & WM_MODE_ROMAN ) ? ( ( ch > 0x20 && ch < 0x7F ) ? ch : 0 ) : kc_kana_key_m(key, mods);
	if ( tc != 0 ) {
		(void)box_cut(&p->def.part[i]);
		return (BOOL)( pn_send(p, pid, i, tc, stat) || busy );
	}
	return busy;
}

LOCAL BOOL wm_panel_type_u( INT wid, CONST UB *s, INT n );

EXPORT BOOL wm_panel_type( INT wid, CONST UB *s, INT n )
{
	BOOL	res;

	pn_lock();
	res = wm_panel_type_u(wid, s, n);
	pn_unlock();
	return res;
}

LOCAL BOOL wm_panel_type_u( INT wid, CONST UB *s, INT n )
{
	INT	i, k;

	if ( s == NULL || n <= 0 ) return FALSE;
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL		*p = &pn_tab[i];
		T_WMPART	*pt;

		if ( !p->used || p->is_menu || p->wid != wid || p->focus < 0 || p->focus >= p->def.npart ) {
			continue;
		}
		pt = &p->def.part[p->focus];
		if ( WM_PT_KIND(pt->type) != TB_PARTS || PART_GREY(pt) || !pn_shown(p, pt) ) {
			continue;
		}
		for ( k = 0; k < n; k++ ) {
			if ( ( pt->type & ( P_ASCII | P_DIGITS ) ) != 0 && ( s[k] < 0x20 || s[k] >= 0x7F ) ) return FALSE;
			if ( ( pt->type & P_DIGITS ) != 0 && ( s[k] < '0' || s[k] > '9' ) ) return FALSE;
		}
		if ( pn_conv_pid == i + 1 && pn_conv_i == p->focus ) {
			pn_commit();
		}
		(void)box_cut(pt);
		if ( !box_put(pt, s, n) ) {
			return FALSE;
		}
		(void)wm_panel_draw(i + 1);
		knl_wmobj_part(i + 1, p->focus, OB_E_CHANGE, NULL);
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- presses and keys */

/* The keys given to a part: a box has all its letters chosen as it takes them */
LOCAL void pn_focus( PANEL *p, INT i, BOOL choose )
{
	if ( p->focus != i ) {
		pn_commit();
		if ( p->focus >= 0 && p->focus < p->def.npart ) {
			sb_keep(&p->def.part[p->focus], p->def.part[p->focus].sb_at);
		}
	}
	p->focus = i;
	if ( i >= 0 && choose ) {
		T_WMPART	*pt = &p->def.part[i];
		UINT		k = WM_PT_KIND(pt->type);

		if ( k == TB_PARTS ) {
			pt->caret = tlen(pt->text);
			pt->sb_at = 0;
		} else if ( k == XB_PARTS ) {
			pt->caret = tlen(pt->text);
			pt->sb_at = -1;
		} else if ( k == SB_PARTS ) {
			pt->sb_at = 0;
		}
	}
	pn_lit = TRUE;
	(void)ts_get_mono(&pn_lit_at);
}

/* The next box that takes the keys, after (d 1) or before (-1) the one that has them */
LOCAL void pn_focus_next( PANEL *p, INT d )
{
	INT	n = p->def.npart, at = p->focus, k;

	for ( k = 0; k < n; k++ ) {
		at = ( at + d + n ) % n;
		if ( pn_takes_keys(p, &p->def.part[at]) ) {
			pn_focus(p, at, TRUE);
			pn_reveal(p, at);
			return;
		}
	}
}

LOCAL void pn_focus_first( PANEL *p )
{
	INT	k;

	for ( k = 0; k < p->def.npart; k++ ) {
		if ( pn_takes_keys(p, &p->def.part[k]) ) {
			pn_focus(p, k, TRUE);
			return;
		}
	}
	pn_focus(p, -1, FALSE);
}

LOCAL BOOL name_off( CONST T_WMPART *pt, INT idx )
{
	return (BOOL)( idx >= 1 && idx <= 32 && ( pt->inact & ( 1U << ( idx - 1 ) ) ) != 0 );
}

/* A volume's number box, told what the volume is set to */
LOCAL void vl_follow( PANEL *p, CONST T_WMPART *pt )
{
	T_WMPART	*nb;

	if ( pt->sub != 0 && ( nb = part_of(p, pt->sub) ) != NULL && WM_PT_KIND(nb->type) == NB_PARTS ) {
		nb->value = pt->value;
	}
}

LOCAL void pn_press( PANEL *p, INT pid, CONST T_WMEV *ev )
{
	T_WMPART	*pt;
	T_DPRECT	r;
	INT		i, k, half = 0, was;
	BOOL		changed = FALSE;

	if ( pn_conv != NULL && pn_conv_pid == pid && pn_conv->list
	  && ( k = pd_cand_at(pn_conv, ev->x, ev->y) ) >= 0 ) {
		PANEL	*q = pn_of(pn_conv_pid);
		INT	r2 = kc_choose(pn_kid, pn_first + k, pn_kout);

		if ( q != NULL && r2 >= 0 && r2 != KC_NOTMINE ) pn_take(q, pn_conv_pid, pn_conv_i, r2);
		wm_panel_draw(pid);
		return;
	}
	i = part_at(p, ev->x, ev->y);
	if ( pn_conv_pid == pid && pn_conv_i >= 0 && i != pn_conv_i ) {
		pn_commit();
	}
	p->pressed = i;
	p->held_at = 0;
	p->held_half = 0;
	p->held_in = TRUE;
	p->held_knob = FALSE;
	if ( i < 0 ) {
		if ( p->beside && p->focus >= 0 ) {
			p->focus = -1;		/* pressed on the content: the keys are its again */
		}
		wm_panel_draw(pid);
		return;
	}
	pt = &p->def.part[i];
	part_rect(p, pt, &r);
	was = pt->value;
	switch ( WM_PT_KIND(pt->type) ) {
	case TB_PARTS:
		pn_focus(p, i, FALSE);
		pt->caret = pd_tb_at(pt, &r, ev->x);
		pt->sb_at = -1;
		break;
	case XB_PARTS:
	case NB_PARTS:
		pn_focus(p, i, FALSE);
		pt->caret = tlen(pt->text);
		break;
	case SB_PARTS:
		if ( p->focus != i ) {
			pn_focus(p, i, FALSE);
		}
		k = pd_sb_at(pt, &r, ev->x, ev->y, &half);
		if ( half != 0 ) {
			p->held_half = half;
			changed = sb_step(pt, pt->sb_at, ( half == -1 ) ? 1 : -1);
		} else if ( k >= 0 ) {
			sb_keep(pt, pt->sb_at);
			pt->sb_at = k;
		}
		break;
	case WS_PARTS:
		if ( pt->count > 0 ) {
			k = pd_ws_at(pt, &r, ev->x, ev->y);
			if ( k >= 0 && !name_off(pt, k + 1) ) {
				p->held_at = k + 1;
			} else {
				p->pressed = -1;
			}
		}
		break;
	case SS_PARTS:
		k = pd_ss_at(pt, &r, ev->x, ev->y);
		if ( k > 0 && !name_off(pt, k) ) {
			pt->value = k;
			p->held_at = k;
		} else if ( k == -1 ) {
			pt->top -= ( pt->rows > 0 ) ? pt->rows : 1;
		} else if ( k == -3 ) {
			pt->top += ( pt->rows > 0 ) ? pt->rows : 1;
		} else if ( k == -2 ) {
			p->held_knob = TRUE;
		}
		break;
	case VL_PARTS:
		pt->value = pd_vl_at(pt, &r, ev->x, ev->y);
		vl_follow(p, pt);
		break;
	case TG_PARTS:
		k = pd_tag_at(pt, p->def.pool, ev->x, ev->y, NULL);
		if ( k > 0 ) {
			p->held_at = k;
		} else {
			p->pressed = -1;
		}
		break;
	case WM_PT_POPUP:
		/*
		 * The list opens and takes every input from here on, the
		 * release of this press as well: the part itself holds
		 * nothing.
		 */
		p->pressed = -1;
		wm_panel_draw(pid);
		knl_wmobj_part(pid, i, OB_E_PRESS, ev);
		pop_open(pid, i, ev);
		return;
	case WM_PT_SWATCH:
		if ( pt->answer == WM_ANS_NONE ) {
			take_press(p, i);
		}
		break;
	case WM_PT_SVMAP:
	case WM_PT_HUEBAR:
		chooser_at(pt, &r, ev->x, ev->y);
		break;
	default:
		break;
	}
	wm_panel_draw(pid);
	knl_wmobj_part(pid, i, OB_E_PRESS, ev);
	if ( pt->value != was || changed ) {
		knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
	}
}

LOCAL void pn_move( PANEL *p, INT pid, CONST T_WMEV *ev )
{
	INT		i = p->pressed, k;
	T_WMPART	*pt;
	T_DPRECT	r;
	BOOL		in, draw = FALSE;

	if ( i < 0 || i >= p->def.npart ) return;
	pt = &p->def.part[i];
	part_rect(p, pt, &r);
	in = rect_has(&r, ev->x, ev->y);
	switch ( WM_PT_KIND(pt->type) ) {
	case WM_PT_SVMAP:
	case WM_PT_HUEBAR:
		k = pt->value;
		chooser_at(pt, &r, ev->x, ev->y);
		if ( k != pt->value ) {
			wm_panel_draw(pid);	/* a ring may stand over its rectangle's edge */
			knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
		}
		return;
	case VL_PARTS:
		k = pd_vl_at(pt, &r, ev->x, ev->y);
		if ( k != pt->value ) {
			pt->value = k;
			vl_follow(p, pt);
			vl_draw(p, pid, i);
			knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
		}
		return;
	case SS_PARTS:
		if ( p->held_knob ) {
			k = pd_ss_top_at(pt, &r, ev->y);
			if ( k != pt->top ) {
				pt->top = k;
				pn_draw_part(p, pid, i);
			}
			return;
		} else if ( p->held_at > 0 ) {
			k = pd_ss_at(pt, &r, ev->x, ev->y);
			if ( k > 0 && k != pt->value && !name_off(pt, k) ) {
				pt->value = p->held_at = k;
				wm_panel_draw(pid);
				knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
				return;
			}
		}
		break;
	case WS_PARTS:
		if ( pt->count > 0 ) {
			in = (BOOL)( pd_ws_at(pt, &r, ev->x, ev->y) + 1 == p->held_at );
		}
		draw = ( in != p->held_in );
		p->held_in = in;
		break;
	case TG_PARTS:
		in = (BOOL)( pd_tag_at(pt, p->def.pool, ev->x, ev->y, NULL) == p->held_at );
		draw = ( in != p->held_in );
		p->held_in = in;
		break;
	case AS_PARTS:
	case MS_PARTS:
		draw = ( in != p->held_in );
		p->held_in = in;
		break;
	default:
		break;
	}
	if ( draw ) wm_panel_draw(pid);
}

LOCAL void pn_release( PANEL *p, INT pid, CONST T_WMEV *ev )
{
	INT		i = p->pressed, was, k;
	T_WMPART	*pt;
	T_DPRECT	r;
	BOOL		in, pushed = FALSE;

	p->pressed = -1;
	if ( i < 0 || i >= p->def.npart ) return;
	pt = &p->def.part[i];
	part_rect(p, pt, &r);
	was = pt->value;
	in = (BOOL)( p->held_in && rect_has(&r, ev->x, ev->y) );
	switch ( WM_PT_KIND(pt->type) ) {
	case AS_PARTS:
		if ( in ) pt->value = ( pt->value != 0 ) ? 0 : 1;
		break;
	case WM_PT_DROPPER:
	case WM_PT_SWATCH:
		if ( in && pt->answer != WM_ANS_NONE ) {
			pushed = TRUE;
			p->answer = pt->answer;
		}
		break;
	case MS_PARTS:
		if ( in ) {
			pushed = TRUE;
			if ( pt->answer != WM_ANS_NONE ) p->answer = pt->answer;
		}
		break;
	case WS_PARTS:
		if ( pt->count > 0 ) {
			k = pd_ws_at(pt, &r, ev->x, ev->y) + 1;
			if ( p->held_at > 0 && k == p->held_at ) {
				pt->value = ( pt->value == k && ( pt->type & P_NOSEL ) ) ? 0 : k;
			}
		} else if ( in ) {
			if ( pt->value != 0 && ( pt->type & P_NOSEL ) ) {
				pt->value = 0;
			} else {
				take_press(p, i);
			}
		}
		break;
	case TG_PARTS:
		if ( p->held_at > 0 && pd_tag_at(pt, p->def.pool, ev->x, ev->y, NULL) == p->held_at
		  && pd_tag_show(pt, p->def.pool, p->held_at) > 0 ) {
			pn_focus_first(p);
		}
		break;
	default:
		break;
	}
	p->held_at = 0;
	p->held_half = 0;
	p->held_knob = FALSE;
	wm_panel_draw(pid);
	knl_wmobj_part(pid, i, OB_E_RELEASE, ev);
	if ( pt->value != was || pushed ) {
		knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
	}
}

LOCAL void pn_key( PANEL *p, INT pid, CONST T_WMEV *ev )
{
	INT		i = p->focus, k;
	T_WMPART	*pt = ( i >= 0 && i < p->def.npart ) ? &p->def.part[i] : NULL;

	pn_lit = TRUE;
	(void)ts_get_mono(&pn_lit_at);
	if ( ( ev->mods & M_CTRL ) != 0 && !pn_busy(pid, i) ) {
		return;				/* Ctrl with a key: the program's command, not a letter */
	}
	if ( pt != NULL && WM_PT_KIND(pt->type) == TB_PARTS && ( pt->type & P_JP )
	  && ( pn_busy(pid, i) || ( ev->code != K_TAB && ev->code != K_ENTER && ev->code != K_KPENTER
				  && ev->code != K_ESC ) )
	  && pn_conv_key(p, pid, i, ev->code, ev->mods) ) {
		wm_panel_draw(pid);
		knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
		return;
	}
	switch ( ev->code ) {
	case K_ESC:
		p->answer = WM_ANS_CANCEL;
		return;
	case K_TAB:
		pn_focus_next(p, ( ev->mods & M_SHIFT ) ? -1 : 1);
		wm_panel_draw(pid);
		return;
	case K_ENTER:
	case K_KPENTER:
		/* the button that answers is pressed */
		for ( k = 0; k < p->def.npart; k++ ) {
			T_WMPART	*b = &p->def.part[k];

			if ( WM_PT_KIND(b->type) == MS_PARTS && ( b->type & P_EMPHAS )
			  && !PART_GREY(b) && pn_shown(p, b) ) {
				if ( b->answer != WM_ANS_NONE ) p->answer = b->answer;
				knl_wmobj_part(pid, k, OB_E_PRESS, ev);
				knl_wmobj_part(pid, k, OB_E_RELEASE, ev);
				knl_wmobj_part(pid, k, OB_E_CHANGE, ev);
				return;
			}
		}
		break;
	default:
		break;
	}
	if ( pt != NULL && pn_box_key(pt, ev->code, ev->mods) ) {
		wm_panel_draw(pid);
		knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
	} else if ( pt != NULL ) {
		wm_panel_draw(pid);
	}
}

/*
 * The wheel over a list rolls it, as its knob would, a few rows a notch
 * (turned away from the person, up the list); the list's drawing keeps
 * the first row within it. Lists and the sheet go only up and down, so
 * the wheel across is let go, not taken for the upright one.
 */
#define WHEEL_ROWS	3

LOCAL void pn_wheel( PANEL *p, INT pid, CONST T_WMEV *ev )
{
	T_WMPART	*pt;
	T_DPRECT	r, v;
	INT		i, was, mx = 0;
	BOOL		in_view;

	if ( ev->dz == 0 || ev->code != HID_WHEEL_V || wm_num(LK_PD_WHEEL, 1) == 0 ) {
		return;
	}
	in_view = (BOOL)( pn_view(p, &v, &mx) && rect_has(&v, ev->x, ev->y) );
	for ( i = 0; i < p->def.npart; i++ ) {
		pt = &p->def.part[i];
		if ( WM_PT_KIND(pt->type) != SS_PARTS || !pn_shown(p, pt) ) {
			continue;
		}
		if ( part_scrolls(p, pt) && !in_view ) {
			continue;
		}
		part_rect(p, pt, &r);
		if ( !rect_has(&r, ev->x, ev->y) ) {
			continue;
		}
		was = pt->top;
		pt->top -= ev->dz * WHEEL_ROWS;
		if ( pt->rows > 0 && pt->top > pt->count - pt->rows + 1 ) {
			pt->top = pt->count - pt->rows + 1;	/* the last row at the foot */
		}
		if ( pt->top < 1 ) {
			pt->top = 1;
		}
		if ( pt->top != was ) {
			pn_draw_part(p, pid, i);
		}
		return;
	}
	/* anywhere else in a scrolling panel: its sheet, as many rows of a list a notch */
	if ( p->scr >= 0 && mx > 0 ) {
		pn_lock();
		was = p->sy;
		p->sy -= ev->dz * WHEEL_ROWS * list_rowh();
		pn_settle(p);
		pn_unlock();
		if ( p->sy != was ) {
			wm_panel_draw(pid);
		}
	}
}

LOCAL ER wm_panel_event_u( INT pid, CONST T_WMEV *ev, UINT *p_answer );

EXPORT ER wm_panel_event( INT pid, CONST T_WMEV *ev, UINT *p_answer )
{
	ER	res;

	pn_lock();
	res = wm_panel_event_u(pid, ev, p_answer);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_event_u( INT pid, CONST T_WMEV *ev, UINT *p_answer )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
	}
	if ( ev == NULL ) {
		return E_PAR;
	}
	if ( p->is_menu ) {
		return menu_event(pid, ev, p_answer);
	}
	if ( p_answer != NULL ) {
		*p_answer = WM_ANS_NONE;
	}
	if ( ev->wid != p->wid
	  && !( p->popup > 0 && ev->wid == p->owner_wid
	     && ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) ) ) {
		return E_OK;			/* another window's business */
	}
	switch ( ev->type ) {
	case HID_EV_BTN_DOWN:
		pn_press(p, pid, ev);
		break;
	case HID_EV_MOVE:
		pn_move(p, pid, ev);
		break;
	case HID_EV_BTN_UP:
		pn_release(p, pid, ev);
		break;
	case HID_EV_KEY_DOWN:
		pn_key(p, pid, ev);
		break;
	case HID_EV_WHEEL:
		pn_wheel(p, pid, ev);
		break;
	default:
		break;
	}
	if ( p_answer != NULL ) {
		*p_answer = p->answer;
	}
	if ( p->answer >= WM_ANS_ACT ) {
		p->answer = WM_ANS_NONE;	/* told once; the panel goes on */
	}
	return E_OK;
}

/* ---------------------------------------------------------------- the caret's blink */

EXPORT INT knl_pn_wait( void )
{
	INT	blink = wm_num(LK_BLINK, 0), i;
	UD	now = 0, due;

	if ( blink <= 0 ) return -1;
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( p->used && !p->is_menu && p->focus >= 0 && p->wid == wm_focused() ) {
			(void)ts_get_mono(&now);
			due = pn_lit_at + (UD)blink * 1000000ULL;
			return ( due <= now ) ? 0 : (INT)( ( due - now ) / 1000000ULL ) + 1;
		}
	}
	return -1;
}

EXPORT void knl_pn_tick( void )
{
	INT	blink = wm_num(LK_BLINK, 0), i;
	UD	now = 0;
	BOOL	drew = FALSE;

	if ( blink <= 0 ) {
		pn_lit = TRUE;
		return;
	}
	(void)ts_get_mono(&now);
	if ( now - pn_lit_at < (UD)blink * 1000000ULL ) {
		return;
	}
	pn_lit_at = now;
	pn_lit = (BOOL)!pn_lit;
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL		*p = &pn_tab[i];
		INT		gid;
		UINT		k;
		T_DPRECT	r;

		if ( !p->used || p->is_menu || p->focus < 0 || p->focus >= p->def.npart
		  || p->wid != wm_focused() ) {
			continue;
		}
		k = WM_PT_KIND(p->def.part[p->focus].type);
		if ( k != TB_PARTS && k != XB_PARTS && k != NB_PARTS ) {
			continue;
		}
		gid = wm_gid(p->wid);
		if ( gid < 0 ) continue;
		pn_lock();
		draw_part_at(p, i + 1, gid, p->focus, 12, 0);
		part_rect(p, &p->def.part[p->focus], &r);
		pn_unlock();
		wm_damage(p->wid, &r);
		drew = TRUE;
	}
	if ( drew ) wm_update();
}

/* ---------------------------------------------------------------- what a part holds */

/*
 * A part's state as its record carries it (design 18.13): a box's
 * letters, the words of a label or a push button, the numbers of fields
 * along a line (an INT each), a selector's name chosen and the name in
 * use now (two INTs), anything else its value (an INT).
 */
LOCAL INT knl_pn_state_u( INT pid, INT i, UB *buf, INT max );

EXPORT INT knl_pn_state( INT pid, INT i, UB *buf, INT max )
{
	INT	res;

	pn_lock();
	res = knl_pn_state_u(pid, i, buf, max);
	pn_unlock();
	return res;
}

LOCAL INT knl_pn_state_u( INT pid, INT i, UB *buf, INT max )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	INT		n = 0, v[2];
	UINT		k;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	k = WM_PT_KIND(pt->type);
	if ( k == TB_PARTS || k == XB_PARTS ) {
		n = tlen(pt->text);
		if ( n > max ) n = max;
		knl_memcpy(buf, pt->text, n);
	} else if ( k == WM_PT_LABEL || k == MS_PARTS ) {
		n = tlen(pt->label);
		if ( n > max ) n = max;
		knl_memcpy(buf, pt->label, n);
	} else if ( k == SB_PARTS ) {
		n = pt->sb_n * (INT)sizeof(INT);
		if ( n > max ) n = max;
		knl_memcpy(buf, pt->sbv, n);
	} else if ( k == WS_PARTS || k == SS_PARTS ) {
		v[0] = pt->value;
		v[1] = ( pt->count == 0 ) ? ( ( pt->type & P_NOW ) ? 1 : 0 ) : pt->now;
		n = ( max < (INT)sizeof(v) ) ? max : (INT)sizeof(v);
		knl_memcpy(buf, v, n);
	} else {
		n = ( max < (INT)sizeof(INT) ) ? max : (INT)sizeof(INT);
		knl_memcpy(buf, &pt->value, n);
	}
	return n;
}

LOCAL ER knl_pn_set_state_u( INT pid, INT i, CONST UB *buf, INT len );

/*
 * What a part was before it is written, to tell whether the write changed
 * anything drawn: the part whole, which part has the keys, and whether a
 * conversion is under way in it (its letters are committed by the write).
 * knl_pn_set_state and knl_pn_set_flags return 1 when the part changed
 * and E_OK when it is as it was, so that a program that writes again
 * what a part already holds -- a page read again every second, a window
 * filled as it opens -- does not have the panel drawn again and built.
 */
typedef struct {
	T_WMPART	part;
	INT		focus;
	BOOL		conv;
	BOOL		ok;
} PNWAS;

LOCAL void pn_was_take( INT pid, INT i, PNWAS *w )
{
	PANEL	*p = pn_of(pid);

	w->ok = (BOOL)( p != NULL && i >= 0 && i < p->def.npart );
	if ( !w->ok ) {
		return;
	}
	knl_memcpy(&w->part, &p->def.part[i], sizeof(w->part));
	w->focus = p->focus;
	w->conv = (BOOL)( pn_conv_pid == pid && pn_conv_i == i );
}

LOCAL ER pn_was_res( INT pid, INT i, CONST PNWAS *w, ER res )
{
	PANEL		*p = pn_of(pid);
	CONST UB	*a, *b;
	SZ		k;

	if ( res < E_OK ) {
		return res;
	}
	if ( !w->ok || p == NULL || w->conv || p->focus != w->focus ) {
		return 1;
	}
	a = (CONST UB *)&w->part;
	b = (CONST UB *)&p->def.part[i];
	for ( k = 0; k < (SZ)sizeof(w->part); k++ ) {
		if ( a[k] != b[k] ) {
			return 1;
		}
	}
	return E_OK;
}

EXPORT ER knl_pn_set_state( INT pid, INT i, CONST UB *buf, INT len )
{
	PNWAS	was;
	ER	res;

	pn_lock();
	pn_was_take(pid, i, &was);
	res = knl_pn_set_state_u(pid, i, buf, len);
	res = pn_was_res(pid, i, &was, res);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_set_state_u( INT pid, INT i, CONST UB *buf, INT len )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	UINT		k;
	INT		n, v[3];

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	k = WM_PT_KIND(pt->type);
	if ( k == WM_PT_SWATCH && len >= (INT)sizeof(INT) ) {
		/* a swatch: the colour it shows, and with a second INT whether it is marked as chosen */
		knl_memcpy(v, buf, ( len >= 2 * (INT)sizeof(INT) ) ? 2 * (INT)sizeof(INT) : (INT)sizeof(INT));
		pt->colour = (UW)v[0] & 0x00FFFFFFU;
		if ( len >= 2 * (INT)sizeof(INT) ) {
			pt->value = ( v[1] != 0 ) ? 1 : 0;
		}
		return E_OK;
	}
	if ( k == VL_PARTS && len >= (INT)sizeof(v) ) {
		/* a volume: its value and the two ends it runs between */
		knl_memcpy(v, buf, sizeof(v));
		pt->lo = v[1];
		pt->hi = v[2];
		pt->value = v[0];
		vl_follow(p, pt);
		return E_OK;
	}
	if ( k == TB_PARTS || k == XB_PARTS || k == WM_PT_LABEL || k == MS_PARTS || k == WM_PT_ICON ) {
		UB	*t = ( k == TB_PARTS || k == XB_PARTS ) ? pt->text : pt->label;
		/*
		 * a box that has the keys with all of its letters chosen, as one
		 * has when the panel comes up, keeps the letters it is given chosen
		 */
		BOOL	whole = (BOOL)( k == TB_PARTS && p->focus == i && pt->sb_at == 0
				     && pt->caret == tlen(pt->text) );

		n = ( len < WM_LABEL_MAX - 1 ) ? len : WM_LABEL_MAX - 1;
		knl_memcpy(t, buf, n);
		t[n] = 0;
		if ( k == WM_PT_ICON ) {
			icon_load(t);		/* read now, in the caller's task: drawing only shows it */
			return E_OK;
		}
		if ( t == pt->text ) {
			pt->caret = n;
			pt->sb_at = whole ? 0 : -1;
			if ( pn_conv_pid == pid && pn_conv_i == i ) pn_commit();
		}
		return E_OK;
	}
	if ( k == SB_PARTS ) {
		if ( len <= 0 ) {
			pt->type |= P_BLANK;
			return E_OK;
		}
		n = len / (INT)sizeof(INT);
		if ( n > WM_SB_MAX ) n = WM_SB_MAX;
		knl_memcpy(pt->sbv, buf, n * (INT)sizeof(INT));
		pt->type &= ~P_BLANK;
		return E_OK;
	}
	if ( len < (INT)sizeof(INT) ) return E_PAR;
	knl_memcpy(v, buf, ( len >= 2 * (INT)sizeof(INT) ) ? 2 * (INT)sizeof(INT) : (INT)sizeof(INT));
	if ( k == TG_PARTS ) {
		/* a sheet no tag names -- a page opened from another -- is shown as well */
		if ( pd_tag_show(pt, p->def.pool, v[0]) < 0 ) pt->value = v[0];
		pn_focus_first(p);
		return E_OK;
	}
	pt->value = v[0];
	if ( ( k == WS_PARTS || k == SS_PARTS ) && len >= 2 * (INT)sizeof(INT) ) {
		if ( pt->count == 0 ) {
			pt->type = ( v[1] != 0 ) ? ( pt->type | P_NOW ) : ( pt->type & ~P_NOW );
		} else {
			pt->now = v[1];
		}
	}
	if ( k == WS_PARTS && pt->count == 0 && v[0] != 0 ) {
		take_press(p, i);
	}
	if ( k == SS_PARTS && pt->value > 0 ) {
		if ( pt->value < pt->top ) pt->top = pt->value;
		if ( pt->rows > 0 && pt->value >= pt->top + pt->rows ) pt->top = pt->value - pt->rows + 1;
	}
	if ( k == VL_PARTS ) vl_follow(p, pt);
	return E_OK;
}

/* Whether a part can be worked and is shown, and has the keys: OB_WP_SHOWN, OB_WP_OFF, OB_WP_FOCUS */
EXPORT UINT knl_pn_flags( INT pid, INT i )
{
	PANEL	*p = pn_of(pid);
	UINT	f = 0;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return 0;
	if ( pn_shown(p, &p->def.part[i]) ) f |= OB_WP_SHOWN;
	if ( p->def.part[i].type & P_DISABLE ) f |= OB_WP_OFF;
	if ( p->focus == i ) f |= OB_WP_FOCUS;
	return f;
}

LOCAL ER knl_pn_set_flags_u( INT pid, INT i, UINT f );

EXPORT ER knl_pn_set_flags( INT pid, INT i, UINT f )
{
	PNWAS	was;
	ER	res;

	pn_lock();
	pn_was_take(pid, i, &was);
	res = knl_pn_set_flags_u(pid, i, f);
	res = pn_was_res(pid, i, &was, res);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_set_flags_u( INT pid, INT i, UINT f )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	/*
	 * Shown or not changes only when asked to be other than it reads:
	 * a part of a sheet not shown now reads as not shown, and writing
	 * that back with only OB_WP_OFF changed is not to hide it for good.
	 */
	if ( ( f ^ knl_pn_flags(pid, i) ) & OB_WP_SHOWN ) {
		pt->type = ( f & OB_WP_SHOWN ) ? ( pt->type & ~P_GONE ) : ( pt->type | P_GONE );
	}
	pt->type = ( f & OB_WP_OFF ) ? ( pt->type | P_DISABLE ) : ( pt->type & ~P_DISABLE );
	if ( p->focus == i && !pn_takes_keys(p, pt) ) pn_focus_first(p);
	/*
	 * The program gives the box the keys, as a press on it would: what
	 * is typed next goes on after what it holds. Written without the
	 * flag, the keys stay where they are (the flags are read and written
	 * back to change the others).
	 */
	if ( ( f & OB_WP_FOCUS ) != 0 && p->focus != i && pn_takes_keys(p, pt) ) {
		pn_focus(p, i, FALSE);
		if ( WM_PT_KIND(pt->type) == TB_PARTS || WM_PT_KIND(pt->type) == XB_PARTS ) {
			pt->caret = tlen(pt->text);
			pt->sb_at = -1;
		}
	}
	if ( ls_owner == pid && ls_part == i && ( PART_GREY(pt) || !pn_shown(p, pt) ) ) {
		pop_close(TRUE);
	}
	return E_OK;
}

/* The names a selector shows, each ending in a nought: how many bytes */
LOCAL INT knl_pn_names_u( INT pid, INT i, UB *buf, INT max );

EXPORT INT knl_pn_names( INT pid, INT i, UB *buf, INT max )
{
	INT	res;

	pn_lock();
	res = knl_pn_names_u(pid, i, buf, max);
	pn_unlock();
	return res;
}

LOCAL INT knl_pn_names_u( INT pid, INT i, UB *buf, INT max )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	CONST UB	*e;
	INT		n;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	if ( pt->count <= 0 || pt->pool < 0 || pt->pool >= WM_POOL_BYTES ) return 0;
	e = pd_pool_name(p->def.pool, pt->pool, pt->count);
	n = ( e != NULL ) ? (INT)( e - &p->def.pool[pt->pool] ) : WM_POOL_BYTES - pt->pool;
	if ( n > max ) n = max;
	knl_memcpy(buf, &p->def.pool[pt->pool], n);
	return n;
}

/*
 * The names a selector shows, or a list's, set anew: the panel's pool
 * made again with every part's names where they were and this part's
 * new ones in theirs. names holds count names, each ending in a nought.
 */
LOCAL ER knl_pn_set_names_u( INT pid, INT i, CONST UB *names, INT len );

EXPORT ER knl_pn_set_names( INT pid, INT i, CONST UB *names, INT len )
{
	ER	res;

	pn_lock();
	res = knl_pn_set_names_u(pid, i, names, len);
	pn_unlock();
	return res;
}

LOCAL ER knl_pn_set_names_u( INT pid, INT i, CONST UB *names, INT len )
{
	PANEL	*p = pn_of(pid);
	UB	*np;
	INT	used = 0, k, f, count = 0, at;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	for ( k = 0; k < len; k++ ) {
		if ( names[k] == 0 ) count++;
	}
	{
		/*
		 * the names it has already, written again (a program that reads
		 * what it shows every second and sets it each time): nothing
		 * changes, and a list opened from the part stays open
		 */
		T_WMPART	*pt = &p->def.part[i];
		CONST UB	*e;
		INT		n = 0;

		if ( pt->count == count && pt->count > 0 && pt->pool >= 0 && pt->pool < WM_POOL_BYTES ) {
			e = pd_pool_name(p->def.pool, pt->pool, pt->count);
			n = ( e != NULL ) ? (INT)( e - &p->def.pool[pt->pool] ) : WM_POOL_BYTES - pt->pool;
		}
		if ( n == len && n > 0 ) {
			for ( k = 0; k < len && p->def.pool[pt->pool + k] == names[k]; k++ ) ;
			if ( k == len ) return E_OK;
		}
	}
	if ( ls_owner == pid && ls_part == i ) {
		pop_close(TRUE);		/* the list shows names no longer the part's */
	}
	np = (UB *)Kmalloc(WM_POOL_BYTES);
	if ( np == NULL ) return E_NOMEM;
	for ( k = 0; k < p->def.npart; k++ ) {
		T_WMPART	*pt = &p->def.part[k];
		UINT		kind = WM_PT_KIND(pt->type);
		INT		n;

		if ( k == i ) {
			if ( used + len > WM_POOL_BYTES ) goto full;
			knl_memcpy(np + used, names, len);
			pt->pool = used;
			pt->count = count;
			used += len;
			continue;
		}
		if ( ( ( kind == WS_PARTS || kind == SS_PARTS || kind == WM_PT_POPUP || kind == TG_PARTS )
		       && pt->count > 0 ) ) {
			CONST UB *e = pd_pool_name(p->def.pool, pt->pool, pt->count);

			at = pt->pool;
			n = ( e != NULL ) ? (INT)( e - &p->def.pool[at] ) : WM_POOL_BYTES - at;
			if ( used + n > WM_POOL_BYTES ) goto full;
			knl_memcpy(np + used, &p->def.pool[at], n);
			pt->pool = used;
			used += n;
		}
		if ( kind == SB_PARTS ) {
			for ( f = 0; f < pt->sb_n && f < WM_SB_MAX; f++ ) {
				CONST UB *e;

				if ( !( pt->sbf[f] & SBF_NAMES ) ) continue;
				at = pt->sbpool[f];
				e = pd_pool_name(p->def.pool, at, pt->sbhi[f] + 1);
				n = ( e != NULL ) ? (INT)( e - &p->def.pool[at] ) : WM_POOL_BYTES - at;
				if ( used + n > WM_POOL_BYTES ) goto full;
				knl_memcpy(np + used, &p->def.pool[at], n);
				pt->sbpool[f] = used;
				used += n;
			}
		}
	}
	knl_memcpy(p->def.pool, np, used);
	p->def.used = used;
	Kfree(np);
	return E_OK;
full:
	Kfree(np);
	return E_LIMIT;
}

LOCAL ER wm_panel_get_u( INT pid, INT num, INT *p_value );

EXPORT ER wm_panel_get( INT pid, INT num, INT *p_value )
{
	ER	res;

	pn_lock();
	res = wm_panel_get_u(pid, num, p_value);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_get_u( INT pid, INT num, INT *p_value )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL ) {
		return E_NOEXS;
	}
	if ( p_value != NULL ) {
		*p_value = pt->value;
	}

	return E_OK;
}

LOCAL ER wm_panel_set_u( INT pid, INT num, INT value );

EXPORT ER wm_panel_set( INT pid, INT num, INT value )
{
	ER	res;

	pn_lock();
	res = wm_panel_set_u(pid, num, value);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_set_u( INT pid, INT num, INT value )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL ) {
		return E_NOEXS;
	}
	pt->value = value;

	return E_OK;
}

EXPORT ER wm_panel_show( INT pid, INT num, BOOL shown )
{
	PANEL		*p;
	T_WMPART	*pt;
	ER		er = E_NOEXS;

	pn_lock();
	p = pn_of(pid);
	if ( p == NULL ) {
		er = E_ID;
	} else if ( ( pt = part_of(p, num) ) != NULL ) {
		pt->type = shown ? ( pt->type & ~P_GONE ) : ( pt->type | P_GONE );
		if ( p->focus < 0 || ( p->def.part[p->focus].type & P_GONE ) != 0 ) {
			pn_focus_first(p);
		}
		er = E_OK;
	}
	pn_unlock();

	return er;
}

EXPORT ER wm_panel_part_screen( INT pid, INT num, T_DPRECT *out )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	T_WMWIN		w;
	T_DPRECT	r;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL || wm_ref(p->wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	part_rect(p, pt, &r);
	out->left = w.work.left + r.left;
	out->top = w.work.top + r.top;
	out->right = w.work.left + r.right;
	out->bottom = w.work.top + r.bottom;

	return E_OK;
}

EXPORT ER wm_panel_set_colour( INT pid, INT num, UW colour )
{
	PANEL		*p;
	T_WMPART	*pt;
	ER		er = E_NOEXS;

	pn_lock();
	p = pn_of(pid);
	if ( p == NULL ) {
		er = E_ID;
	} else if ( ( pt = part_of(p, num) ) != NULL ) {
		pt->colour = colour & 0x00FFFFFFU;
		er = E_OK;
	}
	pn_unlock();

	return er;
}

EXPORT ER wm_panel_colour( INT pid, INT num, UW *p_colour )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL ) {
		return E_NOEXS;
	}
	if ( p_colour != NULL ) {
		*p_colour = pt->colour;
	}

	return E_OK;
}

/* ---------------------------------------------------------------- menus */

/* ---------------------------------------------------------------- menus */

/*
 * The measures of a menu, from the size of its letters in the look
 * table: a row is the size and a quarter more; the name's base line is
 * an eighth of the size up from the row's foot; a mark is three
 * quarters of the size wide and a third of it high; the indent that
 * makes room for a mark is the size and an eighth; a list's body has a
 * quarter of the size and two more of padding at either side, none
 * above and below; and a list is framed in one line and throws a shadow
 * four wide to the right and below.
 */
#define MN_FRAME	1
#define MN_SHADOW	4

LOCAL void mn_metrics( MNMET *m )
{
	INT	size = (INT)wm_num(WM_LOOK_MENU_H, 16);

	if ( size < 8 ) {
		size = 16;
	}
	m->cw = size;
	m->ch = size;
	m->row = size + size / 4;
	m->base = m->row - size / 8;
	m->ind_h = size / 3;
	m->ind_w = size * 3 / 4;
	m->indent = size + size / 8;
	m->vpad = 0;
	m->hpad = size / 4 + 2;
}

/* The face the lists are written in, at the menus' size */
LOCAL ID mn_font( void )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, (INT)wm_num(WM_LOOK_MENU_H, 16));
	}

	return fid;
}

LOCAL BOOL mn_is_rule( CONST T_WMITEM *it )
{
	return (BOOL)( it->cmd == 0 && it->sub == 0 && it->label[0] == 0 );
}

/*
 * How wide a list's body is: its widest name, and room for the key
 * letters a name carries, and for a mark when any name in the list is
 * set.
 */
LOCAL INT mn_body_width( CONST T_WMMENU *def, CONST MNMET *m, BOOL *p_marks )
{
	ID	fid = mn_font();
	INT	i, wide = 0;
	BOOL	marks = FALSE;

	for ( i = 0; i < def->nitem; i++ ) {
		CONST T_WMITEM	*it = &def->item[i];
		INT		w;

		if ( mn_is_rule(it) ) {
			continue;
		}
		w = ( fid > 0 ) ? fn_width(fid, it->label) : 8 * m->cw;
		if ( it->key[0] != 0 ) {
			w += m->cw + ( ( fid > 0 ) ? fn_width(fid, it->key) : 2 * m->cw );
		}
		if ( it->tick ) {
			marks = TRUE;
		}
		if ( w > wide ) {
			wide = w;
		}
	}
	if ( marks ) {
		wide += m->indent;
	}
	*p_marks = marks;

	return wide;
}

/* The screen's size, which a list is kept inside, shadow and all */
LOCAL void mn_screen( INT *p_w, INT *p_h )
{
	T_DISPSPEC	spec;

	*p_w = 1280;
	*p_h = 800;
	if ( ts_disp_ref(&spec) >= E_OK ) {
		/* the view the windows are laid in */
		*p_w = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width ) / ts_disp_screens();
		*p_h = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	}
}

/* A list's body moved back onto the screen, frame, padding and shadow */
LOCAL void mn_keep_on( T_DPRECT *b, CONST MNMET *m )
{
	INT		d;
	INT		gx = MN_FRAME + m->hpad, gy = MN_FRAME + m->vpad;
	T_DPRECT	on;

	/* on the screen where it opens, not across two */
	wm_screen_at(( b->left + b->right ) / 2, ( b->top + b->bottom ) / 2, &on);
	d = on.right - ( b->right + gx + MN_SHADOW );
	if ( d < 0 ) { b->left += d;  b->right += d; }
	d = on.bottom - ( b->bottom + gy + MN_SHADOW );
	if ( d < 0 ) { b->top += d;  b->bottom += d; }
	d = on.left - ( b->left - gx );
	if ( d > 0 ) { b->left += d;  b->right += d; }
	d = 0 - ( b->top - gy );
	if ( d > 0 ) { b->top += d;  b->bottom += d; }
}

/*
 * How a list of n rows is laid: as many rows to a column as the screen
 * has room for, and as many columns side by side as that takes.
 */
LOCAL void mn_columns( INT n, CONST MNMET *m, INT *p_rows, INT *p_cols )
{
	INT	sw, sh, fit;

	mn_screen(&sw, &sh);
	fit = ( sh - 2 * ( MN_FRAME + m->vpad ) - MN_SHADOW ) / m->row;
	if ( fit < 1 ) fit = 1;
	if ( n < 1 ) n = 1;
	*p_rows = ( n < fit ) ? n : fit;
	*p_cols = ( n + *p_rows - 1 ) / *p_rows;
}

/*
 * A list opened, its body at a place on the screen: a popup window of
 * its own as large as its frame and its shadow, so that it stands over
 * every window and reaches past the one it was opened from. Its rows
 * are the parts of a panel in that window, one to a row across the
 * body and the padding either side, which is the part a press lands on
 * and the part that is inverted. A list taller than the screen is in
 * columns, as many rows to each as the body is high (mn_columns).
 */
LOCAL INT mn_open_body( INT owner, CONST T_WMMENU *def, CONST T_DPRECT *body,
			CONST MNMET *m, BOOL marks )
{
	T_WMPANEL	*pn;
	PANEL		*p;
	T_DPRECT	o;
	INT		i, n = 0, pid, pop, gx, gy, rows, cols, colw;

	gx = MN_FRAME + m->hpad;
	gy = MN_FRAME + m->vpad;
	o.left   = body->left - gx;
	o.top    = body->top - gy;
	o.right  = body->right + gx + MN_SHADOW;
	o.bottom = body->bottom + gy + MN_SHADOW;
	pop = wm_open(&o, WM_ATTR_POPUP, NULL);
	if ( pop < 0 ) {
		return pop;
	}
	pn = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( pn == NULL ) {
		wm_close(pop);
		return E_NOMEM;
	}
	knl_memset(pn, 0, sizeof(*pn));
	pn->num   = def->num;
	pn->owner = def->owner;
	pn->kind  = WM_PNL_PLAIN;
	pn->r.left   = 0;
	pn->r.top    = 0;
	pn->r.right  = ( o.right - o.left ) - MN_SHADOW;
	pn->r.bottom = ( o.bottom - o.top ) - MN_SHADOW;
	rows = ( body->bottom - body->top ) / m->row;
	if ( rows < 1 ) rows = 1;
	cols = ( def->nitem + rows - 1 ) / rows;
	if ( cols < 1 ) cols = 1;
	colw = ( body->right - body->left ) / cols;

	for ( i = 0; i < def->nitem && n < WM_PART_MAX; i++ ) {
		T_WMPART	*pt = &pn->part[n];
		CONST T_WMITEM	*it = &def->item[i];
		INT		k, c = i / rows;

		pt->type   = mn_is_rule(it) ? WM_PT_LABEL : WM_PT_BUTTON;
		if ( it->grey ) {
			pt->type |= P_DISABLE;
		}
		pt->num    = it->cmd;
		pt->sub    = it->sub;
		pt->answer = WM_ANS_OK;
		pt->value  = it->tick ? 1 : 0;
		pt->group  = marks ? 1 : 0;	/* the list keeps room for marks */
		pt->r.left   = ( c == 0 ) ? MN_FRAME : gx + c * colw;
		pt->r.top    = gy + ( i % rows ) * m->row;
		pt->r.right  = ( c == cols - 1 ) ? pn->r.right - MN_FRAME : gx + ( c + 1 ) * colw;
		pt->r.bottom = pt->r.top + m->row;
		for ( k = 0; k < WM_LABEL_MAX; k++ ) {
			pt->label[k] = it->label[k];
		}
		for ( k = 0; k < WM_KEY_MAX; k++ ) {
			pt->key[k] = it->key[k];
		}
		n++;
	}
	pn->npart = n;
	pid = wm_panel_open(pop, pn);
	Kfree(pn);
	if ( pid < 0 ) {
		wm_close(pop);
		return pid;
	}
	p = pn_of(pid);
	if ( p != NULL ) {
		p->is_menu = TRUE;
		p->popup = pop;
		p->owner_wid = owner;
	}

	return pid;
}

/*
 * The main list: the names one under another, the one in hand -- the
 * one chosen last time -- half a letter to the right of the press and
 * with the press half a row into it, so that choosing the same thing
 * again is no movement at all.
 */
LOCAL INT wm_menu_open_u( INT wid, CONST T_WMMENU *def, INT x, INT y );

EXPORT INT wm_menu_open( INT wid, CONST T_WMMENU *def, INT x, INT y )
{
	INT	res;

	pn_lock();
	res = wm_menu_open_u(wid, def, x, y);
	pn_unlock();
	return res;
}

LOCAL INT wm_menu_open_u( INT wid, CONST T_WMMENU *def, INT x, INT y )
{
	MNMET		m;
	T_WMWIN		w;
	T_DPRECT	body;
	BOOL		marks;
	INT		wide, cur, sx = x, sy = y, rows, cols;

	if ( def == NULL || def->nitem <= 0 ) {
		return E_PAR;
	}
	if ( wm_ref(wid, &w) >= E_OK ) {
		sx = w.work.left + x;
		sy = w.work.top + y;
	}
	mn_metrics(&m);
	wide = mn_body_width(def, &m, &marks) + 2 * m.hpad;
	mn_columns(def->nitem, &m, &rows, &cols);
	cur  = ( def->cur > 0 && def->cur < def->nitem ) ? def->cur : 0;
	body.left   = sx - m.cw / 2 - ( cur / rows ) * wide;
	body.top    = sy - ( cur % rows ) * m.row - m.row / 2;
	body.right  = body.left + wide * cols - 2 * m.hpad;
	body.bottom = body.top + rows * m.row;
	mn_keep_on(&body, &m);

	return mn_open_body(wid, def, &body, &m, marks);
}

/*
 * The list under a row: beside the list it hangs from and level with
 * the row, to the right -- or to the left, ending half a letter into
 * the list it hangs from, when there is no room on the right.
 *
 * Nothing is opened for a row with no list under it, for a row that
 * cannot be worked, or once the chain is as deep as it may go: a list
 * that names itself would otherwise open for ever.
 */
LOCAL void menu_sub_open( INT pid, INT row )
{
	PANEL		*p = pn_of(pid);
	T_WMMENU	*def;
	T_WMWIN		pw;
	T_DPRECT	body, parent;
	MNMET		m;
	BOOL		marks;
	INT		child, wide, top, size, rows, cols, rowr;

	if ( p == NULL || row < 0 || row >= p->def.npart ) {
		return;
	}
	if ( p->def.part[row].sub == 0 || PART_GREY(&p->def.part[row]) ) {
		return;
	}
	if ( wm_ref(p->wid, &pw) < E_OK ) {
		return;
	}
	/* the list as large as the box keeps it */
	size = db_size(DB_MENU, p->def.part[row].sub, p->def.owner);
	if ( size < (INT)sizeof(T_WMMENU) ) {
		return;
	}
	def = (T_WMMENU *)Kmalloc((SZ)size);
	if ( def == NULL ) {
		return;
	}
	if ( db_get(DB_MENU, p->def.part[row].sub, p->def.owner, def, (SZ)size) < 0
	  || (SZ)size < (SZ)WM_MENU_SIZE(def->nitem) || def->nitem <= 0 ) {
		Kfree(def);
		return;
	}
	def->owner = p->def.owner;
	mn_metrics(&m);
	wide = mn_body_width(def, &m, &marks) + 2 * m.hpad;
	mn_columns(def->nitem, &m, &rows, &cols);
	wide = wide * cols - 2 * m.hpad;
	/* the parent's frame, on the screen */
	parent.left   = pw.work.left + p->def.r.left;
	parent.top    = pw.work.top + p->def.r.top;
	parent.right  = pw.work.left + p->def.r.right;
	parent.bottom = pw.work.top + p->def.r.bottom;
	top = pw.work.top + p->def.part[row].r.top;
	rowr = pw.work.left + p->def.part[row].r.right;	/* the right of the row's column */

	body.left   = rowr;
	body.top    = top;
	body.right  = body.left + wide;
	body.bottom = top + rows * m.row;
	mn_keep_on(&body, &m);
	if ( rowr - m.cw > body.left ) {
		/* pushed back over its parent: it goes on the left instead */
		body.right  = parent.left + m.cw / 2;
		body.left   = body.right - wide;
		body.top    = top;
		body.bottom = top + rows * m.row;
		mn_keep_on(&body, &m);
	}
	child = mn_open_body(p->owner_wid, def, &body, &m, marks);
	Kfree(def);
	if ( child > 0 ) {
		PANEL	*c = pn_of(child);

		p->child = child;
		if ( c != NULL ) {
			c->parent = pid;
			c->at_row = row;
		}
		wm_panel_draw(child);
	}
}

/*
 * A click (a release over the row it was pressed on, or over a row of
 * the menu after the press that opened it) on a row with a list under
 * it. The row is not an item that can be chosen: its list opens at
 * once, without waiting for the menu's delay, and is held open, as is
 * every list of the chain it hangs from, until an item is chosen in it
 * or another row is clicked. A list open under another row goes first.
 * Clicked again, the row keeps its list as it is.
 */
LOCAL void menu_sub_keep( INT pid, INT row )
{
	PANEL	*p = pn_of(pid), *a;
	INT	depth = 0;

	if ( p == NULL || row < 0 || row >= p->def.npart ) {
		return;
	}
	if ( p->child > 0 ) {
		PANEL	*c = pn_of(p->child);

		if ( c == NULL || c->at_row != row ) {
			INT	k = p->child;

			p->child = 0;
			p->kept = FALSE;
			wm_panel_close(k);
		}
	}
	p->pend = -1;
	if ( p->hot != row ) {
		p->hot = row;
		wm_panel_draw(pid);
	}
	if ( p->child <= 0 ) {
		menu_sub_open(pid, row);
	}
	if ( p->child <= 0 ) {
		return;				/* nothing to open: the row stays lit */
	}
	/* the lists it hangs from stay as they are, too */
	for ( a = p; a != NULL && depth < WM_PANEL_MAX; depth++ ) {
		a->kept = TRUE;
		a = ( a->parent > 0 ) ? pn_of(a->parent) : NULL;
	}
}

/*
 * A list drawn: filled in its ground and framed in one line, the shadow
 * laid a half-tone of black over what is below and to the right of it,
 * then each row -- a rule as a dotted line through its middle, a name
 * on the base line in the list's colour or the barred one, a mark in
 * the indent when the list has room for marks, the key letters two in
 * from the right. The row under the pointer is inverted.
 */
LOCAL void menu_paint_body( PANEL *p );

LOCAL void menu_paint( PANEL *p )
{
	pn_lock();
	menu_paint_body(p);
	pn_unlock();
}

LOCAL void menu_paint_body( PANEL *p )
{
	INT		gid = wm_gid(p->wid), i;
	MNMET		m;
	T_DPPAT		ground, frame, shade;
	T_DPRECT	f = p->def.r, s;
	BOOL		sub = (BOOL)( p->parent > 0 );
	ID		fid;
	T_FNMET		met;

	if ( gid < 0 ) {
		return;
	}
	mn_metrics(&m);
	wm_look_pat(sub ? LK_SUBMGROUND : LK_MENUGROUND, &ground);
	wm_look_pat(sub ? LK_SUBMFRAME : LK_MENUFRAME, &frame);
	dp_fill_rect_pat(gid, &f, &ground);
	dp_frame_rect_pat(gid, &f, &frame, MN_FRAME);

	dp_pat_tone(&shade, 4, 0x00000000U, WM_CLEAR);
	s.left = f.left + MN_SHADOW;  s.right = f.right + MN_SHADOW;
	s.top = f.bottom;             s.bottom = f.bottom + MN_SHADOW;
	dp_fill_rect_pat(gid, &s, &shade);
	s.left = f.right;             s.right = f.right + MN_SHADOW;
	s.top = f.top + MN_SHADOW;    s.bottom = f.bottom;
	dp_fill_rect_pat(gid, &s, &shade);

	fid = mn_font();
	if ( fid <= 0 || fn_metrics(fid, &met) < E_OK ) {
		fid = 0;
	}
	for ( i = 0; i < p->def.npart; i++ ) {
		T_WMPART	*pt = &p->def.part[i];
		T_DPRECT	r = pt->r;
		INT		l = r.left + m.hpad, rr = r.right - m.hpad;
		BOOL		grey = PART_GREY(pt);
		UW		col;

		if ( WM_PT_KIND(pt->type) == WM_PT_LABEL && pt->label[0] == 0 ) {
			/* a rule: dotted, two on and two off */
			INT	y = ( r.top + r.bottom ) / 2, x;

			for ( x = l; x < rr; x++ ) {
				if ( ( ( x - l ) & 3 ) < 2 ) {
					dp_put_pixel(gid, x, y, wm_look(LK_MENUFRAME));
				}
			}
			continue;
		}
		if ( pt->group != 0 ) {
			/* the list keeps room for a mark at the head of each row */
			if ( pt->value != 0 ) {
				T_DPRECT	b;
				T_DPPAT		in, lt;

				b.left = l + ( m.indent - m.ind_w ) / 2;
				b.top = r.top + ( m.row - m.ind_h ) / 2;
				b.right = b.left + m.ind_w;
				b.bottom = b.top + m.ind_h;
				wm_look_pat(LK_MARKON_IN, &in);
				wm_look_pat(LK_MARKON_LT, &lt);
				dp_fill_rect_pat(gid, &b, &in);
				dp_frame_rect_pat(gid, &b, &lt, 2);
			}
			l += m.indent;
		}
		if ( fid <= 0 ) {
			continue;
		}
		col = wm_look(grey ? LK_MENUINACT : LK_MENUCHCOL);
		fn_draw(gid, fid, l, r.top + m.base - 1, pt->label, col);
		if ( pt->key[0] != 0 ) {
			INT	kw = fn_width(fid, pt->key);

			fn_draw(gid, fid, rr - kw - 2, r.top + m.base - 1, pt->key,
				wm_look(grey ? LK_MENUKEYINACT : LK_MENUKEYCOL));
		}
	}

	/* the row in hand, inverted */
	if ( p->hot >= 0 && p->hot < p->def.npart ) {
		T_WMPART	*pt = &p->def.part[p->hot];

		if ( !PART_GREY(pt) && ( pt->num != 0 || pt->sub != 0 ) ) {
			dp_set_mode(gid, DP_MODE_XOR);
			dp_fill_rect(gid, &pt->r, 0x00FFFFFFU);
			dp_set_mode(gid, DP_MODE_COPY);
		}
	}
	(void)met;
	wm_damage(p->wid, &f);
	{
		T_DPRECT	all = f;

		all.right += MN_SHADOW;
		all.bottom += MN_SHADOW;
		wm_damage(p->wid, &all);
	}
}

/* The one furthest down the chain: what the pointer is working in */
LOCAL INT menu_deepest( INT pid )
{
	PANEL	*p = pn_of(pid);
	INT	depth = 0;

	/* the count is what stops a ring of panels, not a limit on menus:
	   a chain cannot be longer than there are panels to make it of */
	while ( p != NULL && p->child > 0 && depth < WM_PANEL_MAX ) {
		pid = p->child;
		p = pn_of(pid);
		depth++;
	}

	return pid;
}

/* Whether a window is one of this menu's lists, or one opened from it */
LOCAL BOOL menu_in_child( CONST PANEL *p, INT wid )
{
	INT	depth = 0;

	while ( p != NULL && p->child > 0 && depth < WM_PANEL_MAX ) {
		p = pn_of(p->child);
		if ( p != NULL && p->wid == wid ) {
			return TRUE;
		}
		depth++;
	}

	return FALSE;
}

/* Whether a row is waiting for its list to open: the time left (ms), -1 none */
EXPORT INT knl_menu_wait( void )
{
	UD	now = 0, due;
	INT	i, left = -1;

	(void)ts_get_mono(&now);
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( !p->used || !p->is_menu || p->pend < 0 || p->pend != p->hot || p->child > 0 ) {
			continue;
		}
		due = p->pend_at + (UD)wm_num(LK_MENU_DLY, 100) * 1000000ULL;
		left = ( due <= now ) ? 0 : (INT)( ( due - now ) / 1000000ULL ) + 1;
		break;
	}
	return left;
}

/* Rows the pointer has stayed on long enough: their lists opened (from wm_read_event) */
EXPORT void knl_menu_tick( void )
{
	UD	now = 0;
	INT	i;

	(void)ts_get_mono(&now);
	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];

		if ( !p->used || !p->is_menu || p->pend < 0 || p->pend != p->hot || p->child > 0 ) {
			continue;
		}
		if ( now - p->pend_at >= (UD)wm_num(LK_MENU_DLY, 100) * 1000000ULL ) {
			INT	row = p->pend;


			p->pend = -1;
			menu_sub_open(i + 1, row);
			wm_update();
		}
	}
}

EXPORT BOOL wm_menu_has( INT pid, INT wid )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return FALSE;
	}

	return (BOOL)( p->wid == wid || menu_in_child(p, wid) );
}

LOCAL ER wm_menu_event_u( INT pid, CONST T_WMEV *ev, INT *p_cmd );

EXPORT ER wm_menu_event( INT pid, CONST T_WMEV *ev, INT *p_cmd )
{
	ER	res;

	pn_lock();
	res = wm_menu_event_u(pid, ev, p_cmd);
	pn_unlock();
	return res;
}

LOCAL ER wm_menu_event_u( INT pid, CONST T_WMEV *ev, INT *p_cmd )
{
	PANEL	*p = pn_of(pid);
	UINT	answer = WM_ANS_NONE;
	ER	er;
	INT	deep;

	if ( p == NULL ) {
		return E_ID;
	}
	if ( p_cmd != NULL ) {
		*p_cmd = 0;
	}
	/*
	 * An item is chosen with the main button only. The other button
	 * opens the menu; pressed or let go over it again, it chooses
	 * nothing, and the menu stays as it is.
	 */
	if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return E_OK;
	}
	/*
	 * A chain of menus answers as one. The event goes to every menu in
	 * it from the far end inwards: the far end is where the pointer is
	 * when a child is open, and the one it was opened from still has
	 * to see the pointer leave. Whichever of them is chosen in, the
	 * command comes back here, because here is where the whole chain
	 * was opened from.
	 */
	deep = menu_deepest(pid);
	while ( deep > 0 && deep != pid ) {
		PANEL	*d = pn_of(deep);
		UINT	sub_answer = WM_ANS_NONE;
		INT	up;

		if ( d == NULL ) {
			break;
		}
		up = d->parent;
		if ( wm_panel_event(deep, ev, &sub_answer) >= E_OK
		  && sub_answer == WM_ANS_OK ) {
			if ( p_cmd != NULL ) {
				*p_cmd = d->cmd;
			}
			p->answer = WM_ANS_OK;
			p->cmd = d->cmd;
			if ( p->child > 0 ) {
				INT	c = p->child;

				p->child = 0;
				wm_panel_close(c);
			}
			return E_OK;
		}
		deep = up;
	}
	er = wm_panel_event(pid, ev, &answer);
	if ( er < E_OK ) {
		return er;
	}
	if ( answer == WM_ANS_OK && p_cmd != NULL ) {
		*p_cmd = p->cmd;
	}
	if ( answer == WM_ANS_CANCEL && p_cmd != NULL ) {
		*p_cmd = 0;			/* abandoned: no command */
	}

	return E_OK;
}

/* ---------------------------------------------------------------- a pop-up's list */

/*
 * A name of a pop-up as a row of its list: the words before a tab as
 * the row's name, and those after it as the row's keys, which a list
 * sets against its right end as a pop-up's body sets them against its
 * mark.
 */
LOCAL void pop_row_words( CONST UB *s, UB *label, UB *key )
{
	INT	i = 0, k = 0;

	while ( s[i] != 0 && s[i] != 0x09 && i < WM_LABEL_MAX - 1 ) {
		label[i] = s[i];
		i++;
	}
	label[i] = 0;
	if ( s[i] == 0x09 ) {
		i++;
		while ( s[i] != 0 && k < WM_KEY_MAX - 1 ) {
			key[k++] = s[i++];
		}
	}
	key[k] = 0;
}

/*
 * The list of pop-up i of panel pid opened. It is a menu's list -- a
 * popup window of its own in front of every window, framed, with its
 * shadow, the row under the pointer turned over -- whose rows are the
 * part's names, the one chosen marked. It stands so that the row of
 * the name chosen lies over the part, its names starting where the
 * part's name starts, and it is at least as wide as the part; then it
 * is moved onto the screen as a menu is. A list longer than the screen
 * shows the rows round the name chosen.
 */
LOCAL void pop_open( INT pid, INT i, CONST T_WMEV *ev )
{
	PANEL		*p = pn_of(pid), *l;
	T_WMPART	*pt;
	T_WMPANEL	*pn;
	T_WMWIN		w;
	T_DPRECT	ps, body, o;
	MNMET		m;
	ID		fid;
	INT		n, fit, first, cur, k, wide = 0, sw, sh, gx, gy, pop, lp;

	if ( p == NULL || ls_pid > 0 || i < 0 || i >= p->def.npart ) {
		return;
	}
	pt = &p->def.part[i];
	n = pt->count;
	if ( n <= 0 || PART_GREY(pt) || wm_ref(p->wid, &w) < E_OK ) {
		return;
	}
	pn = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( pn == NULL ) {
		return;
	}
	knl_memset(pn, 0, sizeof(*pn));
	mn_metrics(&m);
	mn_screen(&sw, &sh);
	gx = MN_FRAME + m.hpad;
	gy = MN_FRAME + m.vpad;
	fit = ( sh - 2 * gy - MN_SHADOW ) / m.row;
	if ( fit > WM_PART_MAX ) fit = WM_PART_MAX;
	if ( fit < 1 ) fit = 1;
	cur = ( pt->value >= 1 && pt->value <= n ) ? pt->value - 1 : 0;
	first = 0;
	if ( n > fit ) {
		first = cur - fit / 2;
		if ( first > n - fit ) first = n - fit;
		if ( first < 0 ) first = 0;
		n = fit;
	}

	/* the rows, and how wide the widest of them is */
	fid = mn_font();
	for ( k = 0; k < n; k++ ) {
		T_WMPART	*r = &pn->part[k];
		CONST UB	*nm = pd_pool_name(p->def.pool, pt->pool, first + k);
		INT		ww;

		if ( nm == NULL ) {
			break;
		}
		pop_row_words(nm, r->label, r->key);
		r->type = WM_PT_BUTTON;
		if ( first + k < 32 && ( pt->inact & ( 1U << ( first + k ) ) ) != 0 ) {
			r->type |= P_DISABLE;
		}
		r->num = first + k + 1;
		r->answer = WM_ANS_OK;
		r->group = 1;			/* the list keeps room for the mark */
		r->value = ( pt->value == first + k + 1 ) ? 1 : 0;
		ww = ( fid > 0 ) ? fn_width(fid, r->label) : 8 * m.cw;
		if ( r->key[0] != 0 ) {
			ww += m.cw + ( ( fid > 0 ) ? fn_width(fid, r->key) : 2 * m.cw );
		}
		if ( ww > wide ) wide = ww;
	}
	n = k;
	if ( n <= 0 ) {
		Kfree(pn);
		return;
	}
	wide += m.indent;

	/* the part on the screen, and the list's body over it */
	part_rect(p, pt, &ps);
	ps.left += w.work.left;   ps.right += w.work.left;
	ps.top += w.work.top;     ps.bottom += w.work.top;
	body.left = ps.left + PD_FH - m.indent;
	body.right = body.left + wide;
	if ( body.right < ps.right - gx ) {
		body.right = ps.right - gx;
	}
	body.top = ( ps.top + ps.bottom ) / 2 - m.row / 2 - ( cur - first ) * m.row;
	body.bottom = body.top + n * m.row;
	mn_keep_on(&body, &m);

	o.left = body.left - gx;
	o.top = body.top - gy;
	o.right = body.right + gx + MN_SHADOW;
	o.bottom = body.bottom + gy + MN_SHADOW;
	pop = wm_open(&o, WM_ATTR_POPUP, NULL);
	if ( pop < 0 ) {
		Kfree(pn);
		return;
	}
	pn->num = p->def.num;
	pn->owner = p->def.owner;
	pn->kind = WM_PNL_PLAIN;
	pn->r.right = ( o.right - o.left ) - MN_SHADOW;
	pn->r.bottom = ( o.bottom - o.top ) - MN_SHADOW;
	for ( k = 0; k < n; k++ ) {
		T_WMPART	*r = &pn->part[k];

		r->r.left = MN_FRAME;
		r->r.top = gy + k * m.row;
		r->r.right = pn->r.right - MN_FRAME;
		r->r.bottom = r->r.top + m.row;
	}
	pn->npart = n;
	lp = wm_panel_open(pop, pn);
	Kfree(pn);
	if ( lp < 0 ) {
		wm_close(pop);
		return;
	}
	l = pn_of(lp);
	l->is_menu = TRUE;
	l->popup = pop;
	l->owner_wid = p->wid;
	l->hot = ( cur - first >= 0 && cur - first < n && !PART_GREY(&l->def.part[cur - first]) )
		 ? cur - first : -1;
	ls_pid = lp;
	ls_owner = pid;
	ls_part = i;
	ls_first = first;
	/* the row the pointer is on as the list opens over the part, -1 none */
	{
		T_WMWIN	lw;

		ls_hot0 = -1;
		if ( wm_ref(l->wid, &lw) >= E_OK ) {
			ls_hot0 = part_at(l, w.work.left + ev->x - lw.work.left, w.work.top + ev->y - lw.work.top);
		}
	}
	wm_click_begin(&ls_click, ev->when, ev->code);
	wm_panel_draw(lp);
	pn_draw_part(p, pid, i);
	wm_update();
}

/* The list put away, and the part it was opened from drawn as it is now */
LOCAL void pop_close( BOOL draw )
{
	INT	l = ls_pid, o = ls_owner, i = ls_part;
	PANEL	*p;

	if ( l <= 0 ) {
		return;
	}
	ls_pid = 0;
	ls_owner = 0;
	ls_part = -1;
	(void)wm_panel_close(l);
	p = pn_of(o);
	if ( draw && p != NULL && i >= 0 && i < p->def.npart ) {
		pn_draw_part(p, o, i);
	}
	if ( draw ) {
		wm_update();
	}
}

/* Row k of the list chosen: its name becomes the part's, and the list goes */
LOCAL void pop_choose( INT k, CONST T_WMEV *ev )
{
	PANEL		*l = pn_of(ls_pid), *p = pn_of(ls_owner);
	INT		pid = ls_owner, i = ls_part, was;
	T_WMPART	*pt;

	if ( l == NULL || p == NULL || k < 0 || k >= l->def.npart || i < 0 || i >= p->def.npart
	  || PART_GREY(&l->def.part[k]) ) {
		pop_close(TRUE);
		return;
	}
	pt = &p->def.part[i];
	was = pt->value;
	pt->value = l->def.part[k].num;
	pop_close(TRUE);
	knl_wmobj_part(pid, i, OB_E_RELEASE, ev);
	if ( pt->value != was ) {
		knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
	}
}

/* The row lit moved by d over the rows that can be chosen; it stays at either end */
LOCAL void pop_step( PANEL *l, INT d )
{
	INT	k = l->hot;

	if ( k < 0 ) {
		k = ( d > 0 ) ? -1 : l->def.npart;
	}
	for ( k += d; k >= 0 && k < l->def.npart; k += d ) {
		if ( !PART_GREY(&l->def.part[k]) ) {
			l->hot = k;
			menu_paint(l);
			wm_update();
			return;
		}
	}
}

EXPORT INT knl_pn_list( INT *p_wid )
{
	PANEL	*l = pn_of(ls_pid);

	if ( l == NULL ) {
		return 0;
	}
	if ( p_wid != NULL ) {
		*p_wid = l->wid;
	}
	return ls_pid;
}

/*
 * An input while a list is open. The pointer over the list lights the
 * row under it; a release on a row chooses it, and a release anywhere
 * else puts the list away unchanged -- except the release that ends the
 * opening press while it is still a click (within the double click's
 * time, or with the pointer not moved, wm_click_take), after which the
 * list stands until a row is pressed and let go on, or a press is made
 * off it. The second button does nothing, nor does any other button
 * while the opening one is held. Up and Down move the lit row, Enter
 * chooses it, Escape puts the list away.
 */
LOCAL BOOL knl_pn_list_event_u( CONST T_WMEV *ev );

EXPORT BOOL knl_pn_list_event( CONST T_WMEV *ev )
{
	BOOL	res;

	pn_lock();
	res = knl_pn_list_event_u(ev);
	pn_unlock();
	return res;
}

LOCAL BOOL knl_pn_list_event_u( CONST T_WMEV *ev )
{
	PANEL	*l, *p;
	INT	row = -1;

	if ( ls_pid <= 0 || ev == NULL ) {
		return FALSE;
	}
	l = pn_of(ls_pid);
	p = pn_of(ls_owner);
	if ( l == NULL || p == NULL || ls_part < 0 || ls_part >= p->def.npart ) {
		pop_close(FALSE);
		return FALSE;
	}
	if ( ( ev->type == HID_EV_MOVE || ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP )
	  && ev->wid == l->wid ) {
		row = part_at(l, ev->x, ev->y);
	}
	/* carried off the row it opened on: no longer a click, whenever it is let go */
	if ( ls_click.live && ( ev->type == HID_EV_MOVE || ev->type == HID_EV_BTN_UP ) && row != ls_hot0 ) {
		ls_click.moved = TRUE;
	}
	if ( wm_click_take(&ls_click, ev) != WM_CK_PASS ) {
		return TRUE;		/* a click: the list stays; another button while it is held */
	}
	switch ( ev->type ) {
	case HID_EV_MOVE:
		if ( row != l->hot ) {
			l->hot = row;
			menu_paint(l);
			wm_update();
		}
		break;
	case HID_EV_BTN_DOWN:
		if ( ev->code != 0 ) {
			break;
		}
		if ( ev->wid != l->wid ) {
			pop_close(TRUE);		/* pressed off the list */
			break;
		}
		if ( row != l->hot ) {
			l->hot = row;
			menu_paint(l);
			wm_update();
		}
		break;
	case HID_EV_BTN_UP:
		if ( ev->code != 0 ) {
			break;
		}
		if ( row >= 0 ) {
			pop_choose(row, ev);
		} else {
			pop_close(TRUE);
		}
		break;
	case HID_EV_KEY_DOWN:
		switch ( ev->code ) {
		case K_ESC:
			pop_close(TRUE);
			break;
		case K_UP:
			pop_step(l, -1);
			break;
		case K_DOWN:
			pop_step(l, 1);
			break;
		case K_ENTER:
		case K_KPENTER:
			if ( l->hot >= 0 ) {
				pop_choose(l->hot, ev);
			} else {
				pop_close(TRUE);
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return TRUE;
}

/*
 * A panel in a window of its own, in the middle of the screen: the way
 * a panel that asks something stands in front of everything until it
 * is answered. Only the size of def->r counts; the parts are placed in
 * the panel as they are for any panel. The keys reach it through
 * 'owner', the window it asks for.
 */
EXPORT INT wm_panel_open_centre( INT owner, CONST T_WMPANEL *def )
{
	INT	sw = 0, sh = 0;

	if ( def == NULL ) {
		return E_PAR;
	}
	mn_screen(&sw, &sh);

	return wm_panel_open_at(owner, def, ( sw - ( def->r.right - def->r.left ) ) / 2,
				( sh - ( def->r.bottom - def->r.top ) ) / 2);
}

EXPORT INT wm_panel_open_at( INT owner, CONST T_WMPANEL *def, INT x, INT y )
{
	T_WMPANEL	*pn;
	PANEL		*p;
	T_DPRECT	o;
	INT		w, h, sw = 0, sh = 0, pop, pid;

	if ( def == NULL ) {
		return E_PAR;
	}
	w = def->r.right - def->r.left;
	h = def->r.bottom - def->r.top;
	if ( w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	mn_screen(&sw, &sh);
	o.left = ( x + w > sw ) ? sw - w : x;
	o.top = ( y + h > sh ) ? sh - h : y;
	if ( o.left < 0 ) o.left = 0;
	if ( o.top < 0 )  o.top = 0;
	o.right = o.left + w;
	o.bottom = o.top + h;
	pop = wm_open(&o, WM_ATTR_POPUP, NULL);
	if ( pop < 0 ) {
		return pop;
	}
	pn = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
	if ( pn == NULL ) {
		wm_close(pop);
		return E_NOMEM;
	}
	*pn = *def;
	pn->r.left = 0;
	pn->r.top = 0;
	pn->r.right = w;
	pn->r.bottom = h;
	pid = wm_panel_open(pop, pn);
	Kfree(pn);
	if ( pid < 0 ) {
		wm_close(pop);
		return pid;
	}
	p = pn_of(pid);
	if ( p != NULL ) {
		p->popup = pop;
		p->owner_wid = owner;
	}
	/* a window that never has the keys shows as having them while it is up (E_OBJ for one that can) */
	if ( owner > 0 ) {
		(void)wm_lit(owner, TRUE);
	}

	return pid;
}

/* ---------------------------------------------------------------- checking */

EXPORT INT wm_panel_name( T_WMPANEL *def, CONST UB *name )
{
	INT	at, n = 0;

	if ( def == NULL || name == NULL ) {
		return -1;
	}
	if ( def->used < 0 || def->used >= WM_POOL_BYTES ) {
		def->used = 0;
	}
	at = def->used;
	while ( name[n] != 0 && at + n + 1 < WM_POOL_BYTES ) {
		def->pool[at + n] = name[n];
		n++;
	}
	if ( at + n + 1 >= WM_POOL_BYTES ) {
		return -1;			/* the pool is full */
	}
	def->pool[at + n] = 0;
	def->used = at + n + 1;

	return at;
}

EXPORT ER wm_panel_field( INT pid, INT num, INT field, INT *p_value )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL || p_value == NULL
	  || field < 0 || field >= WM_SB_MAX ) {
		return E_PAR;
	}
	*p_value = pt->sbv[field];

	return E_OK;
}

LOCAL ER wm_panel_set_field_u( INT pid, INT num, INT field, INT value );

EXPORT ER wm_panel_set_field( INT pid, INT num, INT field, INT value )
{
	ER	res;

	pn_lock();
	res = wm_panel_set_field_u(pid, num, field, value);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_set_field_u( INT pid, INT num, INT field, INT value )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL || field < 0 || field >= WM_SB_MAX ) {
		return E_PAR;
	}
	if ( pt->sblo[field] != pt->sbhi[field] ) {
		if ( value < pt->sblo[field] ) value = pt->sblo[field];
		if ( value > pt->sbhi[field] ) value = pt->sbhi[field];
	}
	pt->sbv[field] = value;

	return E_OK;
}

LOCAL ER wm_panel_text_u( INT pid, INT num, UB *buf, INT max );

EXPORT ER wm_panel_text( INT pid, INT num, UB *buf, INT max )
{
	ER	res;

	pn_lock();
	res = wm_panel_text_u(pid, num, buf, max);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_text_u( INT pid, INT num, UB *buf, INT max )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	INT		i;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL || buf == NULL || max <= 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < max - 1 && pt->text[i] != 0; i++ ) {
		buf[i] = pt->text[i];
	}
	buf[i] = 0;

	return E_OK;
}

LOCAL ER wm_panel_set_text_u( INT pid, INT num, CONST UB *str );

EXPORT ER wm_panel_set_text( INT pid, INT num, CONST UB *str )
{
	ER	res;

	pn_lock();
	res = wm_panel_set_text_u(pid, num, str);
	pn_unlock();
	return res;
}

LOCAL ER wm_panel_set_text_u( INT pid, INT num, CONST UB *str )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	INT		i = 0;

	if ( p == NULL ) {
		return E_ID;
	}
	pt = part_of(p, num);
	if ( pt == NULL || str == NULL ) {
		return E_PAR;
	}
	if ( WM_PT_KIND(pt->type) == WM_PT_LABEL ) {
		/* words alone: what they say */
		while ( i < WM_LABEL_MAX - 1 && str[i] != 0 ) {
			pt->label[i] = str[i];
			i++;
		}
		pt->label[i] = 0;
		return E_OK;
	}
	while ( i < WM_LABEL_MAX - 1 && str[i] != 0 ) {
		pt->text[i] = str[i];
		i++;
	}
	pt->text[i] = 0;
	pt->caret = i;

	return E_OK;
}

EXPORT INT wm_panel_self_check( void )
{
	INT	faults = 0;
	INT	i, j, k;

	for ( i = 0; i < WM_PANEL_MAX; i++ ) {
		PANEL	*p = &pn_tab[i];
		INT	w, h;

		if ( !p->used ) {
			continue;
		}
		w = p->def.r.right - p->def.r.left;
		h = p->def.r.bottom - p->def.r.top;
		if ( p->scr >= 0 ) {
			h = p->full_h;		/* a scrolling sheet's parts are inside the panel as defined */
		}
		for ( j = 0; j < p->def.npart; j++ ) {
			T_WMPART *a = &p->def.part[j];

			/* every part is inside the panel it belongs to */
			if ( a->r.left < 0 || a->r.top < 0
			  || a->r.right > w || a->r.bottom > h ) {
				faults++;
			}
			if ( a->r.right <= a->r.left || a->r.bottom <= a->r.top ) {
				faults++;
			}
			/* no two parts of one panel answer to the same number */
			for ( k = j + 1; k < p->def.npart; k++ ) {
				if ( a->num != 0 && p->def.part[k].num == a->num ) {
					faults++;
				}
			}
			/* at most one choice of a group is on */
			if ( WM_PT_KIND(a->type) == WM_PT_CHOICE && a->value != 0 ) {
				for ( k = 0; k < p->def.npart; k++ ) {
					if ( k != j
					  && WM_PT_KIND(p->def.part[k].type) == WM_PT_CHOICE
					  && p->def.part[k].group == a->group
					  && p->def.part[k].value != 0 ) {
						faults++;
					}
				}
			}
		}
	}

	return faults;
}
