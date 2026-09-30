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
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/fn.h>
#include <ts/dbox.h>
#include <ts/disp.h>
#include <ts/ob.h>
#include <ts/look.h>
#include <ts/kconv.h>
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
} PANEL;

LOCAL PANEL	pn_tab[WM_PANEL_MAX];

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

LOCAL void part_label_col( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r,
			   INT indent, UW colour );
LOCAL void part_label( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r, INT indent );
LOCAL INT  list_rowh( void );
LOCAL INT  list_rows( CONST T_DPRECT *r );
LOCAL void list_press( PANEL *p, T_WMPART *pt, INT x, INT y );
LOCAL BOOL box_key( T_WMPART *pt, UINT code, UINT mods );
LOCAL void menu_sub_open( INT pid, INT row );
LOCAL INT  menu_deepest( INT pid );
LOCAL BOOL menu_in_child( CONST PANEL *p, INT wid );
LOCAL void menu_paint( PANEL *p );
typedef struct {
	INT	cw, ch, row, base, ind_w, ind_h, indent, hpad, vpad;
} MNMET;
LOCAL void mn_metrics( MNMET *m );

/*
 * A definition is kept in the data box whole, so it has to fit in one
 * entry of it. This stops the build if the sizes of a part or a label
 * are ever raised past that.
 */
typedef char pn_fits_box[( sizeof(T_WMPANEL) <= DB_MAX_SIZE
			   && sizeof(T_WMMENU) <= DB_MAX_SIZE ) ? 1 : -1];

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

/* A part's rectangle in the work area, from its place inside the panel */
LOCAL void part_rect( CONST PANEL *p, CONST T_WMPART *pt, T_DPRECT *out )
{
	out->left   = p->def.r.left + pt->r.left;
	out->top    = p->def.r.top + pt->r.top;
	out->right  = p->def.r.left + pt->r.right;
	out->bottom = p->def.r.top + pt->r.bottom;
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
EXPORT INT knl_pn_add( INT pid, CONST T_WMPART *pt )
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

	return i;
}

/* A part taken off a panel that is open; the ones after it move up one */
EXPORT ER knl_pn_del( INT pid, INT i )
{
	PANEL	*p = pn_of(pid);
	INT	k;

	if ( p == NULL || p->is_menu ) {
		return E_ID;
	}
	if ( i < 0 || i >= p->def.npart ) {
		return E_PAR;
	}
	for ( k = i; k + 1 < p->def.npart; k++ ) {
		p->def.part[k] = p->def.part[k + 1];
	}
	p->def.npart--;
	if ( p->pressed == i ) p->pressed = -1; else if ( p->pressed > i ) p->pressed--;
	if ( p->focus == i ) p->focus = -1; else if ( p->focus > i ) p->focus--;
	if ( p->hot == i ) p->hot = -1; else if ( p->hot > i ) p->hot--;
	wm_damage(p->wid, &p->def.r);

	return E_OK;
}

/* A part moved within its panel: the rectangle is the panel's own coordinates */
EXPORT ER knl_pn_set_rect( INT pid, INT i, CONST T_DPRECT *r )
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
EXPORT ER knl_pn_part( INT pid, INT i, T_WMPART *out )
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

EXPORT INT wm_panel_open( INT wid, CONST T_WMPANEL *def )
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
	/*
	 * The first box that can be typed in has the letters from the
	 * start. A panel that asks for a name and then wants a press on
	 * the box before it will take one is a panel that loses the first
	 * thing typed into it.
	 */
	{
		INT	k;

		for ( k = 0; k < p->def.npart; k++ ) {
			if ( pn_takes_keys(p, &p->def.part[k]) ) {
				p->focus = k;
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
	p->held_at = 0;
	p->held_half = 0;
	p->held_in = FALSE;
	p->held_knob = FALSE;
	p->used = TRUE;

	/* what it is about to cover */
	{
		INT	w = p->def.r.right - p->def.r.left;
		INT	h = p->def.r.bottom - p->def.r.top;
		INT	gid = wm_gid(wid);

		if ( gid >= 0 && w > 0 && h > 0
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

EXPORT ER wm_panel_close( INT pid )
{
	PANEL	*p = pn_of(pid);

	if ( p == NULL ) {
		return E_ID;
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

		if ( gid >= 0 ) {
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

/* ---------------------------------------------------------------- drawing */

/*
 * Everything in a panel is one of two shapes, and both are the same
 * three strokes.
 *
 * A raised thing is filled, outlined once in the shaded colour, and
 * outlined again in the lit colour with the far corner kept out of the
 * way -- a third of the corner short of the right and the foot -- so
 * that the light falls on the near side only. A sunken thing is the
 * same with the two colours the other way about, which is why pressing
 * a button needs no second drawing of anything.
 *
 * The corners are round. A part's corner comes from the look table, so
 * squaring off every part in the system is one number.
 */
LOCAL void raised( INT gid, CONST T_DPRECT *r, INT rad, UW face, BOOL sunken )
{
	T_DPPAT		pat;
	T_DPENV		env;
	T_DPRECT	cut;
	UW		light = wm_look(sunken ? WM_LOOK_SHADOW : WM_LOOK_LIGHT);
	UW		dark  = wm_look(sunken ? WM_LOOK_LIGHT : WM_LOOK_SHADOW);

	if ( r->right - r->left < 2 || r->bottom - r->top < 2 ) {
		return;
	}
	dp_pat_colour(&pat, face);
	dp_fill_round(gid, r, rad, rad, &pat);
	dp_pat_colour(&pat, dark);
	dp_frame_round(gid, r, rad, rad, 1, &pat);

	if ( dp_ref(gid, &env) < E_OK ) {
		return;
	}
	cut = env.visible;
	if ( cut.right > r->right - rad / 3 )   cut.right = r->right - rad / 3;
	if ( cut.bottom > r->bottom - rad / 3 ) cut.bottom = r->bottom - rad / 3;
	if ( cut.right > cut.left && cut.bottom > cut.top ) {
		dp_set_visible(gid, &cut);
		dp_pat_colour(&pat, light);
		dp_frame_round(gid, r, rad, rad, 1, &pat);
	}
	dp_set_visible(gid, &env.visible);
}

/* The room a list keeps on its right for its own bar */
#define LIST_BAR_W	20

/* The name at an index (1 up) in a panel's pool, or NULL */
LOCAL CONST UB *pool_name( CONST PANEL *p, CONST T_WMPART *pt, INT idx )
{
	INT	at = pt->pool, n = 1;

	if ( idx < 1 || idx > pt->count || at < 0 || at >= WM_POOL_BYTES ) {
		return NULL;
	}
	while ( n < idx && at < WM_POOL_BYTES ) {
		while ( at < WM_POOL_BYTES && p->def.pool[at] != 0 ) {
			at++;
		}
		at++;				/* past the nought */
		n++;
	}

	return ( at < WM_POOL_BYTES ) ? &p->def.pool[at] : NULL;
}

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

/* A part's words, cut where they stop fitting, on the middle line, in
 * the colour given */
LOCAL void part_label_col( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r,
			   INT indent, UW colour )
{
	ID	fid = pn_font();
	T_FNMET	met;
	INT	room, n, k;
	UB	cut[WM_LABEL_MAX];

	if ( pt->label[0] == 0 || fid <= 0 || fn_metrics(fid, &met) < E_OK ) {
		return;
	}
	room = r->right - r->left - indent - PN_GAP;
	n = ( room > 0 ) ? fn_fit(fid, pt->label, room) : 0;
	if ( n <= 0 ) {
		return;
	}
	for ( k = 0; k < n && k < WM_LABEL_MAX - 1; k++ ) {
		cut[k] = pt->label[k];
	}
	cut[( n < WM_LABEL_MAX - 1 ) ? n : WM_LABEL_MAX - 1] = 0;
	fn_draw(gid, fid, r->left + indent,
		r->top + (r->bottom - r->top + met.ascent - met.descent) / 2,
		cut, colour);
}

LOCAL void part_label( INT gid, CONST T_WMPART *pt, CONST T_DPRECT *r, INT indent )
{
	part_label_col(gid, pt, r, indent,
		       wm_look(PART_GREY(pt) ? WM_LOOK_PART_TEXT_OFF
					: WM_LOOK_PART_TEXT));
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
LOCAL void draw_part_at( PANEL *p, INT pid, INT gid, INT i, INT rad, INT margin )
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

	case PM_PARTS: {
		/*
		 * A pop-up: a raised box showing the name chosen, with
		 * a mark at its right saying that pressing it brings
		 * the rest. What the mark is drawn with is the same
		 * triangle a volume's knob carries.
		 */
		CONST UB	*nm = pool_name(p, pt, pt->value);
		T_WMPART	tmp = *pt;
		INT		j;
		T_DPPOINT	tri[3];
		T_DPPAT		tpat;

		raised(gid, &r, rad, face, FALSE);
		for ( j = 0; j < WM_LABEL_MAX - 1 && nm != NULL
			  && nm[j] != 0; j++ ) {
			tmp.label[j] = nm[j];
		}
		tmp.label[( nm != NULL ) ? j : 0] = 0;
		part_label(gid, &tmp, &r, margin);

		tri[0].x = r.right - margin - 8;
		tri[0].y = (r.top + r.bottom) / 2 - 3;
		tri[1].x = r.right - margin;
		tri[1].y = tri[0].y;
		tri[2].x = (tri[0].x + tri[1].x) / 2;
		tri[2].y = tri[0].y + 6;
		dp_pat_colour(&tpat, wm_look(PART_GREY(pt)
					     ? WM_LOOK_PART_TEXT_OFF
					     : WM_LOOK_PART_TEXT));
		dp_fill_poly(gid, tri, 3, DP_POLY_ODD, &tpat);
		break;
	}

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
		if ( pn_conv != NULL && pn_conv_pid == pid && pn_conv_i == i ) {
			c.conv = pn_conv;
		}
		pd_part(pt, &r, &c);
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

EXPORT ER wm_panel_draw( INT pid )
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

	panel_body(p, gid);

	for ( i = 0; i < p->def.npart; i++ ) {
		draw_part_at(p, pid, gid, i, rad, margin);
	}
	wm_damage(p->wid, &p->def.r);
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

/* ---------------------------------------------------------------- events */

/*
 * A key as a letter. The codes are the ones the keyboard sends, which
 * are not characters: 0x04 is 'a' whatever is written on the key, and
 * what a shift does to each is a table, not arithmetic. The table is
 * the Japanese keyboard's: 0x35 is 半角/全角 and makes no letter, 0x31
 * and 0x32 are both the ] key, 0x87 is ろ (\ _) and 0x89 is ¥ (\ |).
 */
EXPORT UB wm_key_char( UINT code, UINT mods )
{
	LOCAL CONST char	plain[] =
		"abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\033\b\t "
		"-^@[]];:" "\0" ",./";
	LOCAL CONST char	shift[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!\"#$%&'()\0" "\n\033\b\t "
		"=~`{}}+*" "\0" "<>?";
	/* an English keyboard (システム環境設定): its signs are elsewhere */
	LOCAL CONST char	us_plain[] =
		"abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\033\b\t "
		"-=[]\\\\;'" "`" ",./";
	LOCAL CONST char	us_shift[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()" "\n\033\b\t "
		"_+{}||:\"" "~" "<>?";
	BOOL	up = (BOOL)( (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0 );

	if ( wm_num(LK_KBD_US, 0) != 0 ) {
		if ( code < 0x04 || code > 0x38 ) {
			return 0;
		}
		return (UB)( up ? us_shift[code - 0x04] : us_plain[code - 0x04] );
	}
	if ( code == 0x87 ) {
		return (UB)( up ? '_' : '\\' );
	}
	if ( code == 0x89 ) {
		return (UB)( up ? '|' : '\\' );
	}
	if ( code < 0x04 || code > 0x38 ) {
		return 0;
	}

	return (UB)( up ? shift[code - 0x04] : plain[code - 0x04] );
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
	T_DPRECT	r;
	INT		i;

	for ( i = p->def.npart - 1; i >= 0; i-- ) {
		T_WMPART *pt = &p->def.part[i];

		if ( WM_PT_KIND(pt->type) == WM_PT_LABEL || WM_PT_KIND(pt->type) == WM_PT_LINE
		  || PART_GREY(pt) || !pn_shown(p, pt) ) {
			continue;		/* it takes nothing */
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
			} else if ( WM_PT_KIND(pt->type) == PM_PARTS
				 && !PART_GREY(pt) ) {
				/* a pop-up steps to the next of its names */
				if ( pt->count > 0 ) {
					pt->value = ( pt->value % pt->count ) + 1;
				}
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
				if ( p->is_menu && pt->num != 0 ) {
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
	tc = ( m & WM_MODE_ROMAN ) ? ( ( ch > 0x20 && ch < 0x7F ) ? ch : 0 ) : kc_kana_key(key, shift);
	if ( tc != 0 ) {
		(void)box_cut(&p->def.part[i]);
		return (BOOL)( pn_send(p, pid, i, tc, stat) || busy );
	}
	return busy;
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
	case PM_PARTS:
		if ( pt->count > 0 ) pt->value = ( pt->value % pt->count ) + 1;
		break;
	case WM_PT_SWATCH:
		take_press(p, i);
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
	case VL_PARTS:
		k = pd_vl_at(pt, &r, ev->x, ev->y);
		if ( k != pt->value ) {
			pt->value = k;
			vl_follow(p, pt);
			wm_panel_draw(pid);
			knl_wmobj_part(pid, i, OB_E_CHANGE, ev);
		}
		return;
	case SS_PARTS:
		if ( p->held_knob ) {
			k = pd_ss_top_at(pt, &r, ev->y);
			draw = ( k != pt->top );
			pt->top = k;
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

EXPORT ER wm_panel_event( INT pid, CONST T_WMEV *ev, UINT *p_answer )
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
		draw_part_at(p, i + 1, gid, p->focus, 12, 0);
		part_rect(p, &p->def.part[p->focus], &r);
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
EXPORT INT knl_pn_state( INT pid, INT i, UB *buf, INT max )
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

EXPORT ER knl_pn_set_state( INT pid, INT i, CONST UB *buf, INT len )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;
	UINT		k;
	INT		n, v[3];

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	k = WM_PT_KIND(pt->type);
	if ( k == VL_PARTS && len >= (INT)sizeof(v) ) {
		/* a volume: its value and the two ends it runs between */
		knl_memcpy(v, buf, sizeof(v));
		pt->lo = v[1];
		pt->hi = v[2];
		pt->value = v[0];
		vl_follow(p, pt);
		return E_OK;
	}
	if ( k == TB_PARTS || k == XB_PARTS || k == WM_PT_LABEL || k == MS_PARTS ) {
		UB	*t = ( k == TB_PARTS || k == XB_PARTS ) ? pt->text : pt->label;

		n = ( len < WM_LABEL_MAX - 1 ) ? len : WM_LABEL_MAX - 1;
		knl_memcpy(t, buf, n);
		t[n] = 0;
		if ( t == pt->text ) {
			pt->caret = n;
			pt->sb_at = -1;
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

/* Whether a part can be worked and is shown: OB_WP_SHOWN, OB_WP_OFF */
EXPORT UINT knl_pn_flags( INT pid, INT i )
{
	PANEL	*p = pn_of(pid);
	UINT	f = 0;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return 0;
	if ( pn_shown(p, &p->def.part[i]) ) f |= OB_WP_SHOWN;
	if ( p->def.part[i].type & P_DISABLE ) f |= OB_WP_OFF;
	return f;
}

EXPORT ER knl_pn_set_flags( INT pid, INT i, UINT f )
{
	PANEL		*p = pn_of(pid);
	T_WMPART	*pt;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	pt = &p->def.part[i];
	pt->type = ( f & OB_WP_OFF ) ? ( pt->type | P_DISABLE ) : ( pt->type & ~P_DISABLE );
	pt->type = ( f & OB_WP_SHOWN ) ? ( pt->type & ~P_GONE ) : ( pt->type | P_GONE );
	if ( p->focus == i && !pn_takes_keys(p, pt) ) pn_focus_first(p);
	return E_OK;
}

/* The names a selector shows, each ending in a nought: how many bytes */
EXPORT INT knl_pn_names( INT pid, INT i, UB *buf, INT max )
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
EXPORT ER knl_pn_set_names( INT pid, INT i, CONST UB *names, INT len )
{
	PANEL	*p = pn_of(pid);
	UB	*np;
	INT	used = 0, k, f, count = 0, at;

	if ( p == NULL || i < 0 || i >= p->def.npart ) return E_ID;
	for ( k = 0; k < len; k++ ) {
		if ( names[k] == 0 ) count++;
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
		if ( ( ( kind == WS_PARTS || kind == SS_PARTS || kind == PM_PARTS || kind == TG_PARTS )
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

EXPORT ER wm_panel_get( INT pid, INT num, INT *p_value )
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

EXPORT ER wm_panel_set( INT pid, INT num, INT value )
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
		*p_w = (INT)spec.width;
		*p_h = (INT)spec.height;
	}
}

/* A list's body moved back onto the screen, frame, padding and shadow */
LOCAL void mn_keep_on( T_DPRECT *b, CONST MNMET *m )
{
	INT	sw, sh, d;
	INT	gx = MN_FRAME + m->hpad, gy = MN_FRAME + m->vpad;

	mn_screen(&sw, &sh);
	d = sw - ( b->right + gx + MN_SHADOW );
	if ( d < 0 ) { b->left += d;  b->right += d; }
	d = sh - ( b->bottom + gy + MN_SHADOW );
	if ( d < 0 ) { b->top += d;  b->bottom += d; }
	d = 0 - ( b->left - gx );
	if ( d > 0 ) { b->left += d;  b->right += d; }
	d = 0 - ( b->top - gy );
	if ( d > 0 ) { b->top += d;  b->bottom += d; }
}

/*
 * A list opened, its body at a place on the screen: a popup window of
 * its own as large as its frame and its shadow, so that it stands over
 * every window and reaches past the one it was opened from. Its rows
 * are the parts of a panel in that window, one to a row across the
 * body and the padding either side, which is the part a press lands on
 * and the part that is inverted.
 */
LOCAL INT mn_open_body( INT owner, CONST T_WMMENU *def, CONST T_DPRECT *body,
			CONST MNMET *m, BOOL marks )
{
	T_WMPANEL	*pn;
	PANEL		*p;
	T_DPRECT	o;
	INT		i, n = 0, pid, pop, gx, gy;

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

	for ( i = 0; i < def->nitem && n < WM_PART_MAX; i++ ) {
		T_WMPART	*pt = &pn->part[n];
		CONST T_WMITEM	*it = &def->item[i];
		INT		k;

		pt->type   = mn_is_rule(it) ? WM_PT_LABEL : WM_PT_BUTTON;
		if ( it->grey ) {
			pt->type |= P_DISABLE;
		}
		pt->num    = it->cmd;
		pt->sub    = it->sub;
		pt->answer = WM_ANS_OK;
		pt->value  = it->tick ? 1 : 0;
		pt->group  = marks ? 1 : 0;	/* the list keeps room for marks */
		pt->r.left   = MN_FRAME;
		pt->r.top    = gy + i * m->row;
		pt->r.right  = pn->r.right - MN_FRAME;
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
EXPORT INT wm_menu_open( INT wid, CONST T_WMMENU *def, INT x, INT y )
{
	MNMET		m;
	T_WMWIN		w;
	T_DPRECT	body;
	BOOL		marks;
	INT		wide, cur, sx = x, sy = y;

	if ( def == NULL || def->nitem <= 0 || def->nitem > WM_ITEM_MAX ) {
		return E_PAR;
	}
	if ( wm_ref(wid, &w) >= E_OK ) {
		sx = w.work.left + x;
		sy = w.work.top + y;
	}
	mn_metrics(&m);
	wide = mn_body_width(def, &m, &marks);
	cur  = ( def->cur > 0 && def->cur < def->nitem ) ? def->cur : 0;
	body.left   = sx - m.cw / 2;
	body.top    = sy - cur * m.row - m.row / 2;
	body.right  = body.left + wide;
	body.bottom = body.top + def->nitem * m.row;
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
	INT		child, wide, top;

	if ( p == NULL || row < 0 || row >= p->def.npart ) {
		return;
	}
	if ( p->def.part[row].sub == 0 || PART_GREY(&p->def.part[row]) ) {
		return;
	}
	if ( wm_ref(p->wid, &pw) < E_OK ) {
		return;
	}
	def = (T_WMMENU *)Kmalloc(sizeof(T_WMMENU));
	if ( def == NULL ) {
		return;
	}
	if ( db_get(DB_MENU, p->def.part[row].sub, p->def.owner,
		    def, sizeof(T_WMMENU)) < 0 ) {
		Kfree(def);
		return;
	}
	def->owner = p->def.owner;
	mn_metrics(&m);
	wide = mn_body_width(def, &m, &marks);
	/* the parent's frame, on the screen */
	parent.left   = pw.work.left + p->def.r.left;
	parent.top    = pw.work.top + p->def.r.top;
	parent.right  = pw.work.left + p->def.r.right;
	parent.bottom = pw.work.top + p->def.r.bottom;
	top = pw.work.top + p->def.part[row].r.top;

	body.left   = parent.right;
	body.top    = top;
	body.right  = body.left + wide;
	body.bottom = top + def->nitem * m.row;
	mn_keep_on(&body, &m);
	if ( parent.right - m.cw > body.left ) {
		/* pushed back over its parent: it goes on the left instead */
		body.right  = parent.left + m.cw / 2;
		body.left   = body.right - wide;
		body.top    = top;
		body.bottom = top + def->nitem * m.row;
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
 * A list drawn: filled in its ground and framed in one line, the shadow
 * laid a half-tone of black over what is below and to the right of it,
 * then each row -- a rule as a dotted line through its middle, a name
 * on the base line in the list's colour or the barred one, a mark in
 * the indent when the list has room for marks, the key letters two in
 * from the right. The row under the pointer is inverted.
 */
LOCAL void menu_paint( PANEL *p )
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

EXPORT ER wm_menu_event( INT pid, CONST T_WMEV *ev, INT *p_cmd )
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

/*
 * A panel in a window of its own, in the middle of the screen: the way
 * a panel that asks something stands in front of everything until it
 * is answered. Only the size of def->r counts; the parts are placed in
 * the panel as they are for any panel. The keys reach it through
 * 'owner', the window it asks for.
 */
EXPORT INT wm_panel_open_centre( INT owner, CONST T_WMPANEL *def )
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
	o.left = ( sw - w ) / 2;
	o.top = ( sh - h ) / 2;
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

EXPORT ER wm_panel_set_field( INT pid, INT num, INT field, INT value )
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

EXPORT ER wm_panel_text( INT pid, INT num, UB *buf, INT max )
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

EXPORT ER wm_panel_set_text( INT pid, INT num, CONST UB *str )
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
