/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fg_edit.c
 *	基本図形編集 as a process: the figure drawn, what is picked, carried
 *	and sized, taken back, and given to the tray and taken from it
 *	(design 11.3)
 *
 *	  a press on a shape or a link	takes it, and only it
 *	  a press with Shift or Ctrl	adds it to what is taken, or leaves it
 *	  a press on the page		lets everything go and pulls out a band;
 *					what the band touches is taken, or with
 *					Shift or Ctrl changes whether it is
 *	  carrying the middle		moves everything that is taken; the
 *					second button pressed on the way leaves
 *					a copy behind (複写ドラッグ)
 *	  carrying an edge or the handle sizes the one pressed: the right edge
 *					its width, the foot its height, the
 *					corner and the handle both
 *	  two presses on a link		opens it, as the desktop opens it
 *	  two presses on a link, carried	a copy of its object, named in a
 *					panel, linked where the hand lets go
 *					(実身複製ドラッグ)
 *	  carried out of the window	the links taken go on as the desktop's
 *					outline, to be put where the hand lets
 *					go: in a window that takes links, they
 *					leave the figure unless the second
 *					button went down on the way
 *
 *	While the hand carries something, its outline is drawn over the
 *	figure in inverted lines, drawn again in the same place to take it
 *	off: the figure is not drawn again for every step of the hand. What
 *	the hand did goes into the record once, when it lets go, so that it
 *	is taken back as one change.
 *
 *	Before each change the record is written out as text and kept;
 *	taking a change back is reading the newest of those back.
 */

#include "fg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/look.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/tvsrc.h>
#include <ts/figtool.h>
#include <ts/time.h>

#define ED_EDGE		5		/* how near an edge counts as on it */
#define ED_MIN		8		/* the least a box may be sized to */
#define ED_STEP		16		/* how far each paste is set off */
#define ED_PICK_COL	0x00FF8F00U	/* the outline of what is taken */
#define ED_HANDLE	8		/* the handle a picked shape is sized by */
#define OUT_COL		0x00FFFFFFU	/* the outline carried, drawn inverted */
#define OUT_MAX		64		/* outlines drawn while carrying */
#define UNDO_BYTES	( 64 * 1024 * 1024 )	/* what the kept records may take together */

#define HOLD_NONE	0
#define HOLD_MOVE	1
#define HOLD_W		2
#define HOLD_H		3
#define HOLD_WH		4
#define HOLD_BAND	5
#define HOLD_TWICE	6		/* a link pressed twice: opened when let go */
#define HOLD_DUP	7		/* a link pressed twice and carried: a copy of its object */

/* ---------------------------------------------------------------- what the hand holds */

static INT	hold_how = HOLD_NONE;
static INT	hold_which = -1;
static T_DPRECT	hold_box;		/* the shape pressed, as it is sized */
static T_DPRECT	hold_was;		/* its box as the handle was taken */
static INT	hold_cx, hold_cy;	/* where the handle is carried, in the window */
static UINT	hold_flip;		/* turned over by being sized past an edge (FE_SIZE_FLIP_*) */
static INT	hold_dx, hold_dy;	/* how far what is carried has come */
static INT	hold_x0, hold_y0, hold_px, hold_py;
static BOOL	hold_moved;
static BOOL	hold_add;		/* Shift or Ctrl: the band changes whether */
static BOOL	hold_copy;		/* the second button went down: a copy stays */
static T_VOBJ	hold_v;

/* The outlines drawn inverted now, to be drawn again in the same place to take them off */
static T_DPRECT	out_r[OUT_MAX];
static INT	out_n;

static INT	click_x = -1000, click_y = -1000;
static UD	click_at;

static INT	pasted;			/* pastes since the tray was last filled */
static BOOL	blink_off;		/* 選択枠のちらつき: the turn the frames are hidden */
static UINT	pick_gen;		/* counts the changes of what is picked */

/* What is to be drawn again, short of the whole window: empty when nothing */
static T_DPRECT	damage;

/* ---------------------------------------------------------------- the model */

static CONST T_FGHOST	fe_host;
static BOOL has_joins( void );
static BOOL shows_itself( void );

void fg_sync( void )
{
	g_ed.rec = g_rec;
	g_ed.fig = g_fig;
	g_ed.pick = g_pick;
	g_ed.npick = g_npick;
	g_ed.usable = (BOOL)( g_fig != NULL && !fg_cabinet );
	g_ed.host = &fe_host;
	g_ed.arg = NULL;
}

/* The model made again from the record; picks past its end let go */
void fg_model( void )
{
	INT	i;

	if ( g_fig != NULL ) tv_fig_free(g_fig);
	g_fig = NULL;
	if ( g_rec != NULL && tv_fig(g_rec, &g_fig) < E_OK ) g_fig = NULL;
	if ( g_fig != NULL ) {
		g_fig->zoom = g_zoom;
		g_fig->show_hidden = g_show_hidden;
		if ( g_fig->nsh > g_maxpick ) {
			INT	m = g_fig->nsh + g_fig->nsh / 2 + 16;
			UINT	*p = realloc(g_pick, sizeof(UINT) * (size_t)m);

			if ( p == NULL ) {
				tv_fig_free(g_fig);
				g_fig = NULL;
				tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
			} else {
				memset(p + g_maxpick, 0, sizeof(UINT) * (size_t)( m - g_maxpick ));
				g_pick = p;
				g_maxpick = m;
			}
		}
	}
	g_npick = 0;
	for ( i = 0; i < g_maxpick; i++ ) {
		if ( g_pick[i] == 0 ) continue;
		if ( g_fig == NULL || i >= g_fig->nsh ) g_pick[i] = 0;
		else g_npick++;
	}
	fg_sync();
}

void fg_after( void )
{
	fg_model();
	g_changed = TRUE;
	g_dirty = TRUE;
}

/*
 * Changed only where what is picked was and is now: the model made
 * again, and those places drawn again -- the whole window when a line
 * joined to shapes may have moved with them, or when a link in it to the
 * figure itself shows the figure as it is being edited (tv_src_live)
 */
static void after_part( CONST T_DPRECT *was )
{
	fg_model();
	g_changed = TRUE;
	if ( has_joins() || shows_itself() ) {
		g_dirty = TRUE;
		return;
	}
	fg_damage(was);
	fg_damage_picks();
}

/* ---------------------------------------------------------------- taking back */

static UB	**u_stk;
static SZ	*u_len;
static INT	u_n, u_max;

static SZ undo_held( void )
{
	SZ	t = 0;
	INT	i;

	for ( i = 0; i < u_n; i++ ) t += u_len[i];
	return t;
}

static void undo_drop_oldest( void )
{
	if ( u_n <= 0 ) return;
	free(u_stk[0]);
	memmove(u_stk, u_stk + 1, sizeof(UB *) * (size_t)( u_n - 1 ));
	memmove(u_len, u_len + 1, sizeof(SZ) * (size_t)( u_n - 1 ));
	u_n--;
}

/* The record as it is now, as text; NULL when there is no memory for it */
static UB *rec_text( SZ *p_size )
{
	UB	*buf;
	SZ	size = 0;

	(void)tad_write_mem(g_rec, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return NULL;
	if ( tad_write_mem(g_rec, buf, size + 1, &size) < E_OK ) {
		free(buf);
		return NULL;
	}
	*p_size = size;
	return buf;
}

/*
 * About to change the record: kept as it is. The oldest go while the
 * kept ones would take more than their room; one that cannot be kept
 * at all goes, and the change is made all the same.
 */
void fg_before( void )
{
	SZ	size = 0;
	UB	*xml = rec_text(&size);

	if ( xml == NULL ) return;
	while ( u_n > 0 && undo_held() + size > UNDO_BYTES ) undo_drop_oldest();
	if ( u_n >= u_max ) {
		INT	m = ( u_max > 0 ) ? u_max * 2 : 16;
		UB	**ns = realloc(u_stk, sizeof(UB *) * (size_t)m);
		SZ	*nl;

		if ( ns != NULL ) u_stk = ns;
		nl = ( ns != NULL ) ? realloc(u_len, sizeof(SZ) * (size_t)m) : NULL;
		if ( nl == NULL ) {
			free(xml);
			return;
		}
		u_len = nl;
		u_max = m;
	}
	u_stk[u_n] = xml;
	u_len[u_n] = size;
	u_n++;
}

BOOL fg_undo( void )
{
	T_TAD	*rec = NULL;

	if ( u_n <= 0 ) return FALSE;
	if ( tad_parse(u_stk[u_n - 1], u_len[u_n - 1], NULL, &rec) < E_OK || rec == NULL ) {
		tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
		return FALSE;
	}
	u_n--;
	free(u_stk[u_n]);
	u_stk[u_n] = NULL;
	tad_free(g_rec);
	g_rec = rec;
	fg_pick_none();
	fg_after();
	return TRUE;
}

INT fg_undos( void )
{
	return u_n;
}

void fg_undo_clear( void )
{
	while ( u_n > 0 ) free(u_stk[--u_n]);
}

/* ---------------------------------------------------------------- picking */

BOOL fg_is_link( INT i )
{
	return (BOOL)( g_fig != NULL && i >= 0 && i < g_fig->nsh
		    && g_fig->sh[i].kind == TV_SH_LINK && g_fig->sh[i].link >= 0 );
}

/* What can be taken: a link, and any shape the editor works on */
BOOL fg_takeable( INT i )
{
	fg_sync();
	return (BOOL)( fg_is_link(i) || fe_takeable(&g_ed, i) );
}

/* 固定化 or 背景化: taken, but not carried, sized, cut or thrown away */
static BOOL is_protected( INT i )
{
	T_VOBJ	v;

	return (BOOL)( fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK
		    && ( v.fixed || v.background ) );
}

static void pick_add( INT i )
{
	if ( g_pick[i] != 0 ) return;
	pick_gen++;
	if ( g_pick_seq == 0xFFFFFFFFU ) g_pick_seq = 0xFFFFFFFEU;	/* past counting: they tie */
	g_pick[i] = ++g_pick_seq;
	g_npick++;
}

void fg_pick_none( void )
{
	INT	i;

	if ( g_npick > 0 ) pick_gen++;
	for ( i = 0; i < g_maxpick; i++ ) g_pick[i] = 0;
	g_npick = 0;
	g_pick_seq = 0;
	fg_sync();
}

static void whole_groups( void )
{
	fg_sync();
	fe_whole_groups(&g_ed);
	g_npick = g_ed.npick;
}

/* What is protected is not taken with everything else */
void fg_pick_all( void )
{
	INT	i;

	fg_pick_none();
	/* what is protected is taken too, as the desktop takes it; what is done to what is taken leaves it */
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( fg_takeable(i) ) pick_add(i);
	}
	fg_sync();
}

void fg_pick_add( INT i )
{
	if ( g_fig != NULL && i >= 0 && i < g_fig->nsh && fg_takeable(i) ) pick_add(i);
	fg_sync();
}

static void pick_only( INT i )
{
	fg_pick_none();
	if ( fg_takeable(i) ) pick_add(i);
	whole_groups();
}

static void pick_toggle( INT i )
{
	if ( !fg_takeable(i) ) return;
	if ( g_pick[i] != 0 ) {
		g_pick[i] = 0;
		g_npick--;
		pick_gen++;
	} else {
		pick_add(i);
		whole_groups();
	}
	fg_sync();
}

/* The link added from number 'from' on taken, and nothing else */
static void pick_from( INT from )
{
	INT	i;

	fg_pick_none();
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( fg_is_link(i) && g_fig->sh[i].link >= from ) pick_add(i);
	}
	fg_sync();
}

INT fg_first_pick( void )
{
	INT	i, best = -1;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && ( best < 0 || g_pick[i] < g_pick[best] ) ) best = i;
	}
	return best;
}

/* 選択枠のちらつき is the figure editor's: a cabinet's frames stay, as the desktop's cabinet shows them */
BOOL fg_blinks( void )
{
	return (BOOL)( g_fig != NULL && g_npick > 0 && !fg_holding() && !fg_cabinet );
}

/* A key or a press: the frames shown at once, and for a whole turn */
void fg_blink_show( void )
{
	blink_off = FALSE;
	if ( g_npick > 0 ) fg_damage_picks();
}

void fg_blink_turn( void )
{
	blink_off = fg_blinks() ? (BOOL)!blink_off : FALSE;
	fg_damage_picks();
}

/* ---------------------------------------------------------------- the figure editor's code */

static void h_before( T_FGED *e )
{
	g_npick = e->npick;
	fg_before();
	fg_sync();
}

static void h_changed( T_FGED *e )
{
	g_npick = e->npick;
	fg_after();
}

static void h_model( T_FGED *e )
{
	g_npick = e->npick;
	fg_model();
}

static void h_pick_none( T_FGED *e )
{
	(void)e;
	fg_pick_none();
}

static void h_pick_add( T_FGED *e, INT i )
{
	g_npick = e->npick;
	fg_pick_add(i);
}

static INT h_first_pick( T_FGED *e )
{
	g_npick = e->npick;
	return fg_first_pick();
}

static void h_no_memory( T_FGED *e )
{
	(void)e;
	tell("メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
}

/* 位置あわせ: across なし 左 中央 右, down なし 上 中央 下 */
static BOOL h_align_form( T_FGED *e, INT *p_h, INT *p_v )
{
	UI	q;
	T_OBNTM	m;
	INT	num, ev, done = 0;

	(void)e;
	if ( ui_dlg_open(&g_ui, PN_ALIGN, &q) < E_OK ) return FALSE;
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		if ( ev == UI_EV_BUTTON ) done = ( num == P_OK ) ? 1 : -1;
		else if ( ev == UI_EV_CLOSE ) done = -1;
		else if ( ev == UI_EV_NONE && m.event == OB_E_KEY && ( m.code == 0x28 || m.code == 0x58 ) ) done = 1;
	}
	if ( done > 0 ) {
		INT	i;

		/* the switch of each group that is on */
		*p_h = *p_v = 0;
		for ( i = 0; i < 4; i++ ) {
			if ( ui_value(&q, P_ALIGN_H + i) != 0 ) *p_h = i;
			if ( ui_value(&q, P_ALIGN_V + i) != 0 ) *p_v = i;
		}
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
	if ( done > 0 ) {
		char	t[32];

		snprintf(t, sizeof(t), "align %d,%d", *p_h, *p_v);
		note(t);
	}
	return (BOOL)( done > 0 );
}

static CONST T_FGHOST	fe_host = {
	h_before, h_changed, h_model, h_pick_none, h_pick_add, h_first_pick,
	h_no_memory, h_align_form
};

/* ---------------------------------------------------------------- where things are */

static INT fz( INT v )
{
	return v * g_zoom / 8;
}

static INT unz( INT v )
{
	return v * 8 / g_zoom;
}

static void page_rect( T_DPRECT *r )
{
	r->left = r->top = 0;
	r->right = g_w;
	r->bottom = g_h;
}

/* Where shape i is drawn, in the window's work area */
static BOOL shape_box( INT i, T_DPRECT *b )
{
	T_DPRECT	r;

	if ( g_fig == NULL || i < 0 || i >= g_fig->nsh ) return FALSE;
	r = g_fig->sh[i].r;
	if ( g_fig->sh[i].kind == TV_SH_LINE && ( g_fig->sh[i].c0_shape >= 0 || g_fig->sh[i].c1_shape >= 0 ) ) {
		/* a line joined to shapes is where it is drawn: its ends at those shapes */
		INT		max = TV_OUT_PTS(g_fig->sh[i].npt);
		T_DPPOINT	*pts = malloc(sizeof(T_DPPOINT) * (size_t)max);
		INT		n = ( pts != NULL ) ? tv_line_path(g_fig, &g_fig->sh[i], pts, max) : 0, k;

		for ( k = 0; k < n; k++ ) {
			if ( k == 0 || pts[k].x < r.left ) r.left = pts[k].x;
			if ( k == 0 || pts[k].y < r.top ) r.top = pts[k].y;
			if ( k == 0 || pts[k].x + 1 > r.right ) r.right = pts[k].x + 1;
			if ( k == 0 || pts[k].y + 1 > r.bottom ) r.bottom = pts[k].y + 1;
		}
		free(pts);
	}
	b->left = fz(r.left) - g_sx;
	b->top = fz(r.top) - g_sy;
	b->right = fz(r.right) - g_sx;
	b->bottom = fz(r.bottom) - g_sy;
	return TRUE;
}

/* A link is never lower than it is closed */
static INT least_h( INT i )
{
	T_VOBJ	v;
	INT	h;

	if ( !fg_is_link(i) || tad_lnk_get(g_rec, g_fig->sh[i].link, &v) < E_OK ) return ED_MIN;
	h = fz(tad_lnk_closed_h(&v));
	return ( h > ED_MIN ) ? h : ED_MIN;
}

/* ---------------------------------------------------------------- what is drawn again */

static void grow( T_DPRECT *u, CONST T_DPRECT *r )
{
	if ( r->right <= r->left || r->bottom <= r->top ) return;
	if ( u->right <= u->left ) {
		*u = *r;
		return;
	}
	if ( r->left < u->left ) u->left = r->left;
	if ( r->top < u->top ) u->top = r->top;
	if ( r->right > u->right ) u->right = r->right;
	if ( r->bottom > u->bottom ) u->bottom = r->bottom;
}

static INT frame_w( void );

/* Where the picked shapes are drawn with their frames and handles: empty when none */
static void picks_bound( T_DPRECT *u )
{
	T_DPRECT	b;
	INT		i, m = frame_w() + ED_HANDLE;

	memset(u, 0, sizeof(*u));
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !shape_box(i, &b) ) continue;
		b.left -= m;  b.top -= m;
		b.right += m;  b.bottom += m;
		grow(u, &b);
	}
}

void fg_damage( CONST T_DPRECT *r )
{
	grow(&damage, r);
}

void fg_damage_picks( void )
{
	T_DPRECT	u;

	picks_bound(&u);
	grow(&damage, &u);
}

/* A line joined to shapes runs to them: what moves one moves the line too, out of their boxes */
static BOOL has_joins( void )
{
	INT	i;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_fig->sh[i].c0_shape >= 0 || g_fig->sh[i].c1_shape >= 0 ) return TRUE;
	}
	return FALSE;
}

/* A link in the figure to the figure itself: what it shows changes with every change */
static BOOL shows_itself( void )
{
	T_VOBJ	v;
	INT	i;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK
		  && memcmp(&v.target, &g_obj, sizeof(TS_UUID)) == 0 ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- drawing */

static INT frame_w( void )
{
	return (INT)wm_look(LK_SEL_W) + 1;	/* 細 中 太, as ユーザ環境設定 says */
}

static void dash_frame( CONST T_DPRECT *b, INT sw )
{
	T_DPPAT	pat;

	dp_pat_colour(&pat, ED_PICK_COL);
	dp_line_wide(g_gid, b->left, b->top, b->right, b->top, sw, DP_LINE_DASH, &pat);
	dp_line_wide(g_gid, b->left, b->bottom, b->right, b->bottom, sw, DP_LINE_DASH, &pat);
	dp_line_wide(g_gid, b->left, b->top, b->left, b->bottom, sw, DP_LINE_DASH, &pat);
	dp_line_wide(g_gid, b->right, b->top, b->right, b->bottom, sw, DP_LINE_DASH, &pat);
}

/*
 * What is taken: a broken frame just outside each, and at its foot on
 * the right the handle it is sized by -- except a link, sized by its
 * edges, and a line, changed by its ends. The frames come and go by
 * turns, except while what is taken is carried or sized.
 */
static void draw_picks( void )
{
	T_DPRECT	b, h;
	INT		i, sw = frame_w();

	if ( g_fig == NULL || g_npick == 0 || ( blink_off && !fg_holding() ) ) return;
	for ( i = 0; i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !shape_box(i, &b) ) continue;
		if ( b.right + ED_HANDLE < 0 || b.left - ED_HANDLE > g_w
		  || b.bottom + ED_HANDLE < 0 || b.top - ED_HANDLE > g_h ) continue;
		b.left -= sw;  b.top -= sw;
		b.right += sw;  b.bottom += sw;
		if ( fg_cabinet ) {
			/* a cabinet's frame is whole, as the desktop's cabinet draws it */
			dp_frame_rect(g_gid, &b, ED_PICK_COL, sw);
			continue;
		}
		dash_frame(&b, sw);
		if ( !fg_is_link(i) && g_fig->sh[i].kind != TV_SH_LINE ) {
			h.left = b.right - ED_HANDLE / 2;
			h.top = b.bottom - ED_HANDLE / 2;
			h.right = h.left + ED_HANDLE;
			h.bottom = h.top + ED_HANDLE;
			dp_fill_rect(g_gid, &h, 0x00FFFFFFU);
			dp_frame_rect(g_gid, &h, ED_PICK_COL, 2);
		}
	}
}

/* The outlines of what is carried, as they are now */
static INT outlines( T_DPRECT *r )
{
	T_DPRECT	b;
	INT		i, n = 0;

	switch ( hold_how ) {
	case HOLD_DUP:
		b = hold_box;
		b.left += hold_dx;  b.right += hold_dx;
		b.top += hold_dy;   b.bottom += hold_dy;
		r[n++] = b;
		break;
	case HOLD_MOVE:
		for ( i = 0; g_fig != NULL && i < g_fig->nsh && n < OUT_MAX; i++ ) {
			if ( g_pick[i] == 0 || is_protected(i) || !shape_box(i, &b) ) continue;
			b.left += hold_dx;  b.right += hold_dx;
			b.top += hold_dy;   b.bottom += hold_dy;
			r[n++] = b;
		}
		break;
	case HOLD_W:
	case HOLD_H:
	case HOLD_WH:
		r[n++] = hold_box;
		break;
	case HOLD_BAND:
		b.left = ( hold_x0 < hold_px ) ? hold_x0 : hold_px;
		b.right = ( hold_x0 < hold_px ) ? hold_px : hold_x0;
		b.top = ( hold_y0 < hold_py ) ? hold_y0 : hold_py;
		b.bottom = ( hold_y0 < hold_py ) ? hold_py : hold_y0;
		if ( b.right > b.left && b.bottom > b.top ) r[n++] = b;
		break;
	default:
		break;
	}
	return n;
}

/* Outlines drawn inverted: drawn twice, what was there is back */
static void xor_frames( CONST T_DPRECT *r, INT n )
{
	INT	i;

	if ( n <= 0 ) return;
	(void)dp_set_mode(g_gid, DP_MODE_XOR);
	for ( i = 0; i < n; i++ ) (void)dp_frame_rect(g_gid, &r[i], OUT_COL, 1);
	(void)dp_set_mode(g_gid, DP_MODE_COPY);
}

static void bound( CONST T_DPRECT *r, INT n, T_DPRECT *u )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( u->right <= u->left ) {
			*u = r[i];
			continue;
		}
		if ( r[i].left < u->left ) u->left = r[i].left;
		if ( r[i].top < u->top ) u->top = r[i].top;
		if ( r[i].right > u->right ) u->right = r[i].right;
		if ( r[i].bottom > u->bottom ) u->bottom = r[i].bottom;
	}
}

/* The outline taken off where it was and drawn where it is now, and only that shown */
static void outline_show( void )
{
	T_DPRECT	now[OUT_MAX], u;
	INT		n = outlines(now);

	memset(&u, 0, sizeof(u));
	bound(out_r, out_n, &u);
	bound(now, n, &u);
	xor_frames(out_r, out_n);
	xor_frames(now, n);
	memcpy(out_r, now, sizeof(T_DPRECT) * (size_t)n);
	out_n = n;
	if ( u.right > u.left ) {
		u.left -= 1;  u.top -= 1;
		u.right += 2;  u.bottom += 2;
		if ( u.left < 0 ) u.left = 0;
		if ( u.top < 0 ) u.top = 0;
		if ( u.right > g_w ) u.right = g_w;
		if ( u.bottom > g_h ) u.bottom = g_h;
		if ( u.right > u.left && u.bottom > u.top ) {
			(void)wm_obj_flush(g_kw, &u);
			g_shown_px += (UD)( u.right - u.left ) * (UD)( u.bottom - u.top );
		}
	}
}

static void bars( void )
{
	T_OBWBARS	b;
	SZ		asz = 0;
	INT		hi;

	memset(&b, 0, sizeof(b));
	b.bar[OB_BAR_R].hi = ( g_fh > 0 ) ? g_fh : 1;
	b.bar[OB_BAR_R].clo = g_sy;
	b.bar[OB_BAR_R].chi = g_sy + g_h;
	if ( b.bar[OB_BAR_R].chi > b.bar[OB_BAR_R].hi ) b.bar[OB_BAR_R].chi = b.bar[OB_BAR_R].hi;
	/* across: a figure wider than the page, or one scrolled over */
	hi = ( g_fw > 0 ) ? g_fw : g_w;
	if ( hi < g_sx + g_w ) hi = g_sx + g_w;
	if ( hi <= 0 ) hi = 1;
	b.bar[OB_BAR_B].hi = hi;
	b.bar[OB_BAR_B].clo = g_sx;
	b.bar[OB_BAR_B].chi = g_sx + g_w;
	if ( b.bar[OB_BAR_B].chi > hi ) b.bar[OB_BAR_B].chi = hi;
	(void)ob_wri_rec(g_kw, OB_WR_BARS, 0, &b, (SZ)( sizeof(T_OBWBAR) * 2 ), &asz);
}

static UD now_ns( void )
{
	UD	t = 0;

	(void)ts_get_mono(&t);
	return t;
}

/*
 * The window drawn: the page in its colour to the frame, the figure
 * over it, what is taken over that, and the outline being carried
 * last. 'quiet': a turn of the frames, not said on the console.
 */
static BOOL	paint_held = FALSE;	/* drawn, not yet put on the screen */

/* The window drawn whole, not yet put on the screen: something is drawn over it first */
void fg_paint_held( void )
{
	paint_held = TRUE;
	fg_paint(TRUE);
	paint_held = FALSE;
}

void fg_paint( BOOL quiet )
{
	T_DPRECT	all;
	INT		w = 0, h = 0;
	UD		t0 = now_ns(), p0 = 0, f0 = 0, f1 = 0, s0 = 0, s1 = 0;
	char		s[96];

	page_rect(&all);
	dp_fill_rect(g_gid, &all, ( g_paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : g_paper);
	if ( g_fig != NULL ) {
		tv_src_self(&g_obj);
		p0 = now_ns();
		tv_src_prefetch(g_rec);
		f0 = now_ns();
		(void)tv_fig_draw(g_gid, g_fig, &all, g_sx, g_sy, g_rec, g_paper);
		fgt_overlay();
		f1 = now_ns();
		tv_src_self(NULL);
		(void)tv_fig_size(g_fig, &w, &h);
		g_fw = fz(w);
		g_fh = fz(h);
	}
	draw_picks();
	fg_text_draw(g_gid);
	out_n = 0;
	if ( fg_holding() && hold_moved ) {
		out_n = outlines(out_r);
		xor_frames(out_r, out_n);
	}
	fgt_drawing_over();		/* the shape being drawn, inverted over all of it */
	if ( !paint_held ) {
		s0 = now_ns();
		(void)wm_obj_flush(g_kw, NULL);
		s1 = now_ns();
		g_shown_px += (UD)g_w * (UD)g_h;
	}
	bars();
	g_dirty = FALSE;
	memset(&damage, 0, sizeof(damage));
	if ( !quiet ) {
		/*
		 * the whole, and of it the figure alone, as the desktop's figure
		 * editor says it; and what the objects of the links were asked
		 * beforehand, and putting it on the screen
		 */
		snprintf(s, sizeof(s), "drew in %d us figure %d us objects %d us shown %d us",
			 (INT)( ( now_ns() - t0 ) / 1000 ), (INT)( ( f1 - f0 ) / 1000 ),
			 (INT)( ( f0 - p0 ) / 1000 ), (INT)( ( s1 - s0 ) / 1000 ));
		note(s);
		if ( g_input_at != 0 ) {
			/* from the key or the press, as the keyboard or the pointer told it, to the window shown */
			snprintf(s, sizeof(s), "input to screen %d us", (INT)( ( now_ns() - g_input_at ) / 1000 ));
			note(s);
		}
	}
	g_input_at = 0;
}

/*
 * What was marked to be drawn again, alone: the page, the figure and the
 * frames drawn inside it only, and only it shown. Drawing the figure
 * there passes over the shapes nowhere near it (figview.c), so a change
 * of what is picked costs the shapes round it, not the whole figure.
 * An outline being carried, and the shape being drawn, are taken off
 * first and inverted again over it after, so that the window needs no
 * more than the part drawn either.
 */
BOOL fg_paint_part( BOOL quiet )
{
	T_DPENV		env;
	T_DPRECT	u = damage;
	UD		t0 = now_ns(), f0 = 0, f1 = 0;
	char		s[80];

	if ( fg_text_on() ) {
		/* a piece of text being edited is drawn over the whole figure */
		fg_paint(quiet);
		return TRUE;
	}
	memset(&damage, 0, sizeof(damage));
	if ( u.left < 0 ) u.left = 0;
	if ( u.top < 0 ) u.top = 0;
	if ( u.right > g_w ) u.right = g_w;
	if ( u.bottom > g_h ) u.bottom = g_h;
	if ( u.right <= u.left || u.bottom <= u.top ) {
		g_input_at = 0;
		return FALSE;
	}
	if ( g_fig == NULL ) {
		fg_paint(quiet);
		return TRUE;
	}
	/* what was visible before is put back after: a visible rectangle is never "none" (dp_set_visible) */
	if ( dp_ref(g_gid, &env) < E_OK ) {
		fg_paint(quiet);
		return TRUE;
	}
	xor_frames(out_r, out_n);
	fgt_drawing_over();		/* the shape being drawn taken off, to be put back after */
	(void)dp_set_visible(g_gid, &u);
	dp_fill_rect(g_gid, &u, ( g_paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : g_paper);
	{
		T_DPRECT	all;

		page_rect(&all);
		tv_src_self(&g_obj);
		f0 = now_ns();
		(void)tv_fig_draw(g_gid, g_fig, &all, g_sx, g_sy, g_rec, g_paper);
		fgt_overlay();
		f1 = now_ns();
		tv_src_self(NULL);
	}
	draw_picks();
	(void)dp_set_visible(g_gid, &env.visible);
	xor_frames(out_r, out_n);		/* the outline put back, over what was drawn */
	fgt_drawing_over();
	if ( !paint_held ) {
		(void)wm_obj_flush(g_kw, &u);
		g_shown_px += (UD)( u.right - u.left ) * (UD)( u.bottom - u.top );
	}
	if ( !quiet ) {
		snprintf(s, sizeof(s), "drew part %dx%d in %d us figure %d us", u.right - u.left, u.bottom - u.top,
			 (INT)( ( now_ns() - t0 ) / 1000 ), (INT)( ( f1 - f0 ) / 1000 ));
		note(s);
		if ( g_input_at != 0 ) {
			snprintf(s, sizeof(s), "input to screen %d us", (INT)( ( now_ns() - g_input_at ) / 1000 ));
			note(s);
		}
	}
	g_input_at = 0;
	return TRUE;
}

BOOL fg_damaged( void )
{
	return (BOOL)( damage.right > damage.left && damage.bottom > damage.top );
}

/* ---------------------------------------------------------------- scrolling */

void fg_scroll_to( INT x, INT y )
{
	INT	rx = g_fw - g_w, ry = g_fh - g_h;

	if ( rx < 0 ) rx = 0;
	if ( ry < 0 ) ry = 0;
	if ( x > rx ) x = rx;
	if ( y > ry ) y = ry;
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	if ( x == g_sx && y == g_sy ) return;
	g_sx = x;
	g_sy = y;
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- pressing */

/* The handle of a picked shape under a place: the shape it sizes, or -1 */
static INT handle_at( INT x, INT y, INT slop )
{
	T_DPRECT	b;
	INT		i, ix, iy, out = ED_HANDLE / 2 + 1;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || fg_is_link(i) || g_fig->sh[i].kind == TV_SH_LINE || !shape_box(i, &b) ) continue;
		ix = wm_hit_in(out, slop, b.right - b.left);
		iy = wm_hit_in(out, slop, b.bottom - b.top);
		b.right += 2;
		b.bottom += 2;
		if ( x >= b.right - ix && x <= b.right + out + slop && y >= b.bottom - iy && y <= b.bottom + out + slop ) {
			return i;
		}
	}
	return -1;
}

static INT hold_kind( CONST T_DPRECT *box, INT x, INT y, INT slop )
{
	BOOL	right = (BOOL)( x >= box->right - wm_hit_in(ED_EDGE, slop, box->right - box->left) );
	BOOL	foot = (BOOL)( y >= box->bottom - wm_hit_in(ED_EDGE, slop, box->bottom - box->top) );

	if ( right && foot ) return HOLD_WH;
	if ( right ) return HOLD_W;
	if ( foot ) return HOLD_H;
	return HOLD_MOVE;
}

/* A finger just out of the one thing taken, by its right edge or foot: that thing, or -1 */
static INT edge_near( INT x, INT y, INT slop, T_DPRECT *p_box )
{
	T_DPRECT	b;
	INT		i;

	if ( slop <= 0 || g_fig == NULL || g_npick != 1 ) return -1;
	for ( i = 0; i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !fg_takeable(i) || g_fig->sh[i].kind == TV_SH_LINE
		  || is_protected(i) || !shape_box(i, &b) ) continue;
		if ( x >= b.left && x < b.right + slop && y >= b.top && y < b.bottom + slop
		  && ( x >= b.right || y >= b.bottom ) ) {
			*p_box = b;
			return i;
		}
	}
	return -1;
}

static void hold( INT how, INT which, CONST T_DPRECT *box, INT x, INT y )
{
	hold_how = how;
	hold_which = which;
	if ( box != NULL ) hold_box = *box;
	hold_dx = hold_dy = 0;
	hold_x0 = hold_px = x;
	hold_y0 = hold_py = y;
	hold_moved = FALSE;
	hold_copy = FALSE;
	hold_flip = 0;
	out_n = 0;
}

BOOL fg_holding( void )
{
	return (BOOL)( hold_how != HOLD_NONE );
}

void fg_hold_cancel( void )
{
	xor_frames(out_r, out_n);
	out_n = 0;
	hold_how = HOLD_NONE;
	g_dirty = TRUE;
}

void fg_press( CONST T_OBNTM *m )
{
	T_DPRECT	page, box;
	T_VOBJ		v;
	INT		x = m->x, y = m->y, which, slop = wm_hit_slop(m->when);
	INT		near = (INT)wm_look(LK_DBL_W) / 2, gap_ms = (INT)wm_look(WM_LOOK_DBLTIME);
	BOOL		add = (BOOL)( ( m->mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT | HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 );
	BOOL		twice, shape = FALSE;
	ER		er = E_NOEXS;
	T_DPRECT	was;
	UINT		gen0;

	if ( g_fig == NULL ) return;
	if ( near <= 0 ) near = 5;
	if ( gap_ms <= 0 ) gap_ms = 400;
	page_rect(&page);
	/* the frames as they were: drawn again where they were, when what is picked changes */
	picks_bound(&was);
	gen0 = pick_gen;
	if ( blink_off ) {
		blink_off = FALSE;
		fg_damage(&was);
	}
	/* the handle of a picked shape sizes it, from its foot on the right */
	if ( !add && ( which = handle_at(x, y, slop) ) >= 0 && shape_box(which, &box) && !is_protected(which) ) {
		hold(HOLD_WH, which, &box, x, y);
		return;
	}
	which = -1;
	er = tv_fig_at(g_fig, &page, g_sx, g_sy, g_rec, x, y, &v, &box, &which);
	if ( er < E_OK ) {
		fg_sync();
		if ( tv_fig_shape_at(g_fig, &page, g_sx, g_sy, x, y, &which) >= E_OK
		  && fe_takeable(&g_ed, which) && shape_box(which, &box) ) {
			shape = TRUE;
			er = E_OK;
		}
	}
	/* a finger by the edge of what is taken, on nothing: that edge, to size it */
	if ( er < E_OK && !add && ( which = edge_near(x, y, slop, &box) ) >= 0 ) {
		hold(hold_kind(&box, x, y, slop), which, &box, x, y);
		return;
	}
	twice = (BOOL)( x > click_x - near && x < click_x + near && y > click_y - near && y < click_y + near
		     && m->when - click_at < (UD)gap_ms * 1000000ULL );
	click_x = x;
	click_y = y;
	click_at = m->when;

	if ( er >= E_OK ) {
		if ( twice && !shape ) {
			hold(HOLD_TWICE, which, &box, x, y);
			hold_v = v;
			return;
		}
		/* a finger reaches the edges farther in only of one taken before the press */
		if ( g_pick[which] == 0 ) slop = 0;
		if ( add ) pick_toggle(which);
		else if ( g_pick[which] == 0 ) pick_only(which);
		/* sizing is for one; with several taken, a press on an edge carries them all */
		hold(( g_npick == 1 ) ? hold_kind(&box, x, y, slop) : HOLD_MOVE, which, &box, x, y);
		if ( g_pick[which] == 0 || is_protected(which) ) hold_how = HOLD_NONE;
		if ( pick_gen != gen0 ) {
			fg_damage(&was);
			fg_damage_picks();
		}
		return;
	}
	/* the page itself: everything let go, and a band */
	if ( !add ) fg_pick_none();
	hold(HOLD_BAND, -1, NULL, x, y);
	hold_add = add;
	if ( pick_gen != gen0 ) fg_damage(&was);
}

/* 複写ドラッグ: the second button pressed while what is taken is carried */
void fg_second_down( void )
{
	if ( hold_how == HOLD_MOVE ) hold_copy = TRUE;
}

/* A place of the window on the figure's grid, when the grid holds (格子点拘束); a cabinet has none */
static void grid_snap( INT *x, INT *y )
{
	if ( fg_cabinet ) return;
	ft_snap_win(-g_sx, -g_sy, g_zoom, x, y);
}

/* What is carried brought to the pointer at (x, y), the corner of the shape pressed on the grid */
static void carry_to( INT x, INT y )
{
	INT	cx = hold_box.left + x - hold_x0, cy = hold_box.top + y - hold_y0;

	grid_snap(&cx, &cy);
	hold_dx = cx - hold_box.left;
	hold_dy = cy - hold_box.top;
}

/* What the hand holds followed to (x, y): FALSE when it did not move it */
static BOOL follow( INT x, INT y )
{
	INT	dx = x - hold_px, dy = y - hold_py, near = (INT)wm_look(LK_DBL_W) / 2, least, cx, cy;

	if ( !fg_holding() || ( dx == 0 && dy == 0 ) ) return FALSE;
	if ( near <= 0 ) near = 5;
	switch ( hold_how ) {
	case HOLD_TWICE:
		/* a shaky hand, not a carry: what was pressed twice is still opened */
		if ( x - hold_x0 < near && hold_x0 - x < near && y - hold_y0 < near && hold_y0 - y < near ) {
			hold_px = x;
			hold_py = y;
			return FALSE;
		}
		hold_how = HOLD_DUP;
		carry_to(x, y);
		break;
	case HOLD_MOVE:
	case HOLD_DUP:
		carry_to(x, y);
		break;
	case HOLD_W:
	case HOLD_H:
	case HOLD_WH:
		/* past the edge that stays the shape goes on to the other side, turned over (fe_size_box) */
		if ( !hold_moved ) {
			hold_was = hold_box;
			hold_cx = hold_box.right;
			hold_cy = hold_box.bottom;
		}
		hold_cx += dx;
		hold_cy += dy;
		cx = hold_cx;
		cy = hold_cy;
		grid_snap(&cx, &cy);
		least = ( hold_how == HOLD_H || hold_how == HOLD_WH ) ? least_h(hold_which) : ED_MIN;
		fe_size_box(&hold_was, cx, cy, (BOOL)( hold_how != HOLD_H ), (BOOL)( hold_how != HOLD_W ),
			    ED_MIN, least, &hold_box, &hold_flip);
		break;
	default:
		break;
	}
	hold_px = x;
	hold_py = y;
	hold_moved = TRUE;
	return TRUE;
}

void fg_moved( INT x, INT y )
{
	if ( !follow(x, y) ) return;
	/* out of the window, the desktop carries the links on */
	if ( ( hold_how == HOLD_MOVE || hold_how == HOLD_DUP ) && ( x < 0 || y < 0 || x >= g_w || y >= g_h ) ) {
		fg_left();
		return;
	}
	outline_show();
}

/* ---------------------------------------------------------------- carried out of the window */

/* The outline taken off, the hand let go of, without anything done */
static void let_go( void )
{
	T_DPRECT	u;

	memset(&u, 0, sizeof(u));
	bound(out_r, out_n, &u);
	if ( u.right > u.left ) {
		u.left -= 1;  u.top -= 1;
		u.right += 2;  u.bottom += 2;
		fg_damage(&u);
	}
	out_n = 0;
	hold_how = HOLD_NONE;
	hold_moved = FALSE;
}

/*
 * The links taken, carried out of the window as the desktop's outline
 * (DT_RQ_CARRY), each put where it was let go (DT_RQ_PLACE). Put in a
 * window that takes links, they are taken out of the figure (a move),
 * unless the second button went down on the way (a copy) or they went
 * to another program's window, which may yet refuse them. Pressed twice
 * before it was carried, the one link put is to a copy of its object and
 * nothing here changes. Shapes that are not links stay in the figure.
 */
static void carry_out( BOOL twice )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	o, b;
	T_VOBJ		v;
	INT		i, k, n = 0, last = -1, done = 0, px = hold_x0, py = hold_y0;
	BOOL		copy, given;
	UINT		seq;
	ER		er;
	char		s[TAD_NAME_MAX + 64];

	memset(&o, 0, sizeof(o));
	if ( twice ) {
		/* the one pressed, alone */
		fg_pick_none();
		if ( hold_which >= 0 ) pick_add(hold_which);
	}
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !fg_is_link(i) || is_protected(i) || !shape_box(i, &b) ) continue;
		if ( n == 0 ) o = b;
		grow(&o, &b);
		last = i;
		n++;
	}
	if ( n == 0 ) return;
	if ( n == 1 && tad_lnk_get(g_rec, g_fig->sh[last].link, &v) >= E_OK ) {
		fg_ask_of(&v, DT_RQ_CARRY, &rq);
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_CARRY;
		rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	}
	rq.flags |= DT_F_TO_PROG;
	rq.look.w = o.right - o.left;
	rq.look.h = o.bottom - o.top;
	rq.grab_x = px - o.left;
	rq.grab_y = py - o.top;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "carried nowhere (%d)", (INT)er);
		note(s);
		g_dirty = TRUE;
		return;
	}
	seq = rq.seq;
	copy = (BOOL)( an.how == DT_HOW_COPY || hold_copy );
	given = (BOOL)( ( an.flags & DT_F_TO_PROG ) != 0 );
	for ( i = 0, k = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !fg_is_link(i) || is_protected(i) || !shape_box(i, &b)
		  || tad_lnk_get(g_rec, g_fig->sh[i].link, &v) < E_OK ) continue;
		fg_ask_of(&v, ( ++k < n ) ? DT_RQ_PLACE_MORE : DT_RQ_PLACE, &rq);
		rq.grab_x = b.left - o.left;
		rq.grab_y = b.top - o.top;
		rq.carry = seq;
		if ( twice ) rq.flags |= DT_F_DUPLICATE;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 10 * 60 * 1000);
		snprintf(s, sizeof(s), "carried %s (%d)%s", fg_link_name(&v), (INT)er,
			 twice ? " as a new object" : copy ? " as a copy" : "");
		note(s);
		if ( er >= E_OK ) done++;
		else g_pick[i] = 0;		/* not put there: it stays here, and is not taken away */
	}
	if ( done > 0 && !twice && !copy ) {
		/* what was put there leaves the figure; the shapes that are not links stay */
		for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
			if ( g_pick[i] != 0 && !fg_is_link(i) ) g_pick[i] = 0;
		}
		g_npick = 0;
		for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) if ( g_pick[i] != 0 ) g_npick++;
		fg_sync();
		fg_delete();
		snprintf(s, sizeof(s), "%d個の仮身を移動しました", done);
		say(s);
	} else if ( done > 0 && !given ) {
		snprintf(s, sizeof(s), "%d個の仮身を%s", done, twice ? "実身複製しました" : "複写しました");
		say(s);
	}
	g_dirty = TRUE;
}

/* The pointer went out of the window with the hand down: links carried on by the desktop */
void fg_left( void )
{
	INT	how = hold_how;
	BOOL	any = FALSE;
	INT	i;

	if ( how != HOLD_MOVE && how != HOLD_DUP ) return;
	for ( i = 0; g_fig != NULL && i < g_fig->nsh && !any; i++ ) {
		any = (BOOL)( g_pick[i] != 0 && fg_is_link(i) && !is_protected(i) );
	}
	note(any ? "left the window with links" : "left the window");
	if ( how == HOLD_MOVE && !any ) return;	/* shapes alone: carried on within the window */
	let_go();
	carry_out((BOOL)( how == HOLD_DUP ));
}

/*
 * 実身複製ドラッグ let go in the window itself: the object copied with all
 * it holds under a name asked for, a link to the copy put where the
 * outline was let go, looking as the pressed one does
 */
static void dup_here( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[TAD_NAME_MAX + 48];

	fg_ask_of(&hold_v, DT_RQ_DUPLICATE, &rq);
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er >= E_OK ) fg_link_copy_put(&hold_v, &an.into, unz(hold_dx), unz(hold_dy));
	snprintf(s, sizeof(s), "duplicated %s here (%d)", fg_link_name(&hold_v), (INT)er);
	note(s);
	g_dirty = TRUE;
}

/* A link of the record changed; one that cannot be is said, and nothing shown as if it were */
static void link_put( CONST T_VOBJ *v )
{
	ER	er = tad_lnk_set(g_rec, v);
	char	s[TAD_NAME_MAX + 48];

	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "the link \"%s\" was not changed (%d)", (char *)v->name, (INT)er);
		note(s);
	}
}

static void moved_picked( void )
{
	T_TADNODE	**keep = NULL;
	T_VOBJ		v;
	INT		i, k, nkeep = 0, n = 0, dx = unz(hold_dx), dy = unz(hold_dy);
	char		s[64];
	T_DPRECT	was;

	picks_bound(&was);
	fg_before();
	fg_sync();
	if ( hold_copy ) {
		/* the copies go in under what they copy: what was taken is known by its element after */
		keep = malloc(sizeof(T_TADNODE *) * (size_t)( g_fig->nsh + 1 ));
		for ( i = 0; keep != NULL && i < g_fig->nsh; i++ ) {
			if ( g_pick[i] != 0 && g_fig->sh[i].node != NULL ) keep[nkeep++] = g_fig->sh[i].node;
		}
		fe_dup_picked(&g_ed);
	}
	for ( i = 0; i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fe_takeable(&g_ed, i) ) {
			fe_shift(&g_ed, i, dx, dy);
			n++;
			continue;
		}
		if ( g_pick[i] == 0 || !fg_is_link(i) || is_protected(i)
		  || tad_lnk_get(g_rec, g_fig->sh[i].link, &v) < E_OK ) continue;
		v.left += dx;  v.right += dx;
		v.top += dy;   v.bottom += dy;
		link_put(&v);
		n++;
	}
	after_part(&was);
	if ( keep != NULL ) {
		fg_pick_none();
		for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
			for ( k = 0; k < nkeep; k++ ) {
				if ( g_fig->sh[i].node == keep[k] && g_pick[i] == 0 ) pick_add(i);
			}
		}
		free(keep);
		fg_sync();
		fg_damage_picks();
	}
	snprintf(s, sizeof(s), "%s %d by %d,%d", hold_copy ? "copied" : "moved", n, dx, dy);
	note(s);
}

static void sized_picked( void )
{
	T_DPRECT	nb, was;
	T_VOBJ		v;
	char		s[80];

	picks_bound(&was);
	nb.left = unz(hold_box.left + g_sx);
	nb.top = unz(hold_box.top + g_sy);
	nb.right = unz(hold_box.right + g_sx);
	nb.bottom = unz(hold_box.bottom + g_sy);
	fg_sync();
	if ( fe_takeable(&g_ed, hold_which) ) {
		fg_before();
		fg_sync();
		fe_resize_flip(&g_ed, hold_which, &nb, hold_flip);
		after_part(&was);
	} else if ( fg_is_link(hold_which) && tad_lnk_get(g_rec, g_fig->sh[hold_which].link, &v) >= E_OK ) {
		fg_before();
		v.left = nb.left;
		v.top = nb.top;
		v.right = nb.right;
		/* closed at the closed height; opened only when well past it */
		v.bottom = v.top + tad_lnk_fit_h(&v, nb.bottom - nb.top);
		v.height = v.bottom - v.top;
		link_put(&v);
		after_part(&was);
	} else {
		return;
	}
	snprintf(s, sizeof(s), "sized %d to %dx%d", hold_which, nb.right - nb.left, nb.bottom - nb.top);
	note(s);
}

static void banded( void )
{
	T_DPRECT	band, b, was;
	INT		i;
	char		s[40];
	UINT		gen0 = pick_gen;

	band.left = ( hold_x0 < hold_px ) ? hold_x0 : hold_px;
	band.right = ( hold_x0 < hold_px ) ? hold_px : hold_x0;
	band.top = ( hold_y0 < hold_py ) ? hold_y0 : hold_py;
	band.bottom = ( hold_y0 < hold_py ) ? hold_py : hold_y0;
	picks_bound(&was);
	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( !fg_takeable(i) || !shape_box(i, &b) ) continue;
		/* touched by the band at all counts */
		if ( b.right < band.left || b.left > band.right || b.bottom < band.top || b.top > band.bottom ) continue;
		if ( hold_add ) pick_toggle(i);
		else pick_add(i);
	}
	whole_groups();
	fg_sync();
	if ( pick_gen != gen0 ) {
		fg_damage(&was);
		fg_damage_picks();
	}
	snprintf(s, sizeof(s), "picked %d", g_npick);
	note(s);
}

/*
 * Shapes carried by themselves and let go outside the window, over a
 * window of the desktop's that links would be carried into: only links
 * leave a figure's window, so nothing goes there and nothing moves, and
 * that window takes the keys (DT_RQ_LET_GO)
 */
static BOOL let_go_over_other( INT x, INT y )
{
	T_OBWPOS	wp;
	T_DTREQ		rq;
	T_DTANS		an;
	SZ		asz = 0;

	if ( x >= 0 && y >= 0 && x < g_w && y < g_h ) return FALSE;
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return FALSE;
	if ( wp.wleft + x >= wp.left && wp.wleft + x < wp.right && wp.wtop + y >= wp.top && wp.wtop + y < wp.bottom ) {
		return FALSE;		/* over its own frame */
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_LET_GO;
	rq.grab_x = wp.wleft + x;
	rq.grab_y = wp.wtop + y;
	memset(&an, 0, sizeof(an));
	return (BOOL)( ask_desktop(&rq, &an, 5000) >= E_OK );
}

void fg_released( INT x, INT y )
{
	INT	how = hold_how;
	char	s[40];

	if ( how == HOLD_NONE ) return;
	/* let go elsewhere than it was last followed to (outside the window the moves are not told) */
	if ( how != HOLD_BAND ) {
		(void)follow(x, y);
		how = hold_how;
	}
	/*
	 * Pressed twice and let go outside: what is carried out of the
	 * window went with the desktop as it left (fg_left); nothing is
	 * opened or copied here
	 */
	if ( ( how == HOLD_TWICE || how == HOLD_DUP ) && ( x < 0 || y < 0 || x >= g_w || y >= g_h ) ) {
		let_go();
		return;
	}
	hold_px = x;
	hold_py = y;
	/* the outline is taken off by drawing again where it was, with whatever the release changed */
	if ( out_n > 0 ) {
		T_DPRECT	u;
		INT		i;

		memset(&u, 0, sizeof(u));
		for ( i = 0; i < out_n; i++ ) {
			T_DPRECT	r = out_r[i];

			r.left -= 1;  r.top -= 1;
			r.right += 2;  r.bottom += 2;
			grow(&u, &r);
		}
		fg_damage(&u);
	}
	out_n = 0;
	hold_how = HOLD_NONE;
	if ( how == HOLD_TWICE ) {
		link_open(&hold_v);
	} else if ( how == HOLD_DUP ) {
		dup_here();
	} else if ( hold_moved && how == HOLD_MOVE && let_go_over_other(x, y) ) {
		T_DPRECT	was;

		picks_bound(&was);
		fg_pick_none();
		fg_damage(&was);
		note("let go over another window: nothing moved");
	} else if ( hold_moved && how == HOLD_MOVE ) {
		moved_picked();
	} else if ( hold_moved && ( how == HOLD_W || how == HOLD_H || how == HOLD_WH ) ) {
		sized_picked();
	} else if ( how == HOLD_BAND ) {
		if ( hold_moved ) banded();
	} else {
		/* let go where it was pressed: nothing more to draw */
		snprintf(s, sizeof(s), "picked %d", g_npick);
		note(s);
	}
	hold_moved = FALSE;
}

/* ---------------------------------------------------------------- changes */

/* The links and shapes taken, deleted; what is protected stays */
void fg_delete( void )
{
	TS_UUID	*gone;
	T_VOBJ	v;
	INT	i, n = 0;
	char	s[40];

	if ( g_fig == NULL || g_npick == 0 ) return;
	gone = malloc(sizeof(TS_UUID) * (size_t)g_npick);
	if ( gone == NULL ) return;
	/* the identities first: deleting one moves the others' numbers */
	for ( i = 0; i < g_fig->nsh && n < g_npick; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && !is_protected(i)
		  && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK ) gone[n++] = v.vobjid;
	}
	fg_sync();
	if ( n > 0 || fe_picked(&g_ed) > 0 ) {
		fg_before();
		fg_sync();
		fe_delete_picked(&g_ed);
		for ( i = 0; i < n; i++ ) tad_lnk_del(g_rec, &gone[i]);
		fg_pick_none();
		fg_after();
		snprintf(s, sizeof(s), "deleted, %d shapes", ( g_fig != NULL ) ? g_fig->nsh : 0);
		note(s);
	}
	free(gone);
}

/* An empty fragment of the kind given, to fill */
static T_TAD *frag_new( CONST char *body )
{
	char	text[96];
	T_TAD	*doc = NULL;

	snprintf(text, sizeof(text), "<tad version=\"1.0\" encoding=\"UTF-8\"><%s></%s></tad>", body, body);
	if ( tad_parse((CONST UB *)text, (SZ)strlen(text), NULL, &doc) < E_OK ) return NULL;
	return doc;
}

/* A set's name: what it holds and how many */
static void set_name( UB *out, CONST char *what, INT n )
{
	snprintf((char *)out, TR_NAME_MAX, "%s%d個", what, n);
}

/*
 * The links and shapes taken, to the tray as one set: a <figure>
 * holding a copy of each shape and a link to each object. FALSE when
 * they could not be put there, and the person is told.
 */
BOOL fg_copy( void )
{
	T_TAD	*frag;
	T_VOBJ	v;
	TS_UUID	id;
	T_TRREC	r;
	UB	name[TR_NAME_MAX], *buf;
	SZ	size = 0;
	INT	i, nl = 0, ns;
	ER	er = E_OK;
	char	s[48];

	if ( g_fig == NULL || g_npick == 0 ) return TRUE;
	frag = frag_new("figure");
	if ( frag == NULL ) {
		h_no_memory(&g_ed);
		return FALSE;
	}
	pasted = 0;
	fg_sync();
	ns = fe_copy_into(&g_ed, frag);
	if ( ns < 0 ) {
		tad_free(frag);
		h_no_memory(&g_ed);
		return FALSE;
	}
	for ( i = 0; i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && tad_lnk_get(g_rec, g_fig->sh[i].link, &v) >= E_OK ) {
			memset(&v.vobjid, 0, sizeof(v.vobjid));
			if ( tad_lnk_add(frag, &v, &id) >= E_OK ) nl++;
		}
	}
	if ( nl + ns > 0 ) {
		set_name(name, ( ns == 0 ) ? "仮身" : ( nl == 0 ) ? "図形" : "図形と仮身", nl + ns);
		(void)tad_write_mem(frag, NULL, 0, &size);
		buf = malloc((size_t)size + 1);
		er = ( buf != NULL ) ? tad_write_mem(frag, buf, size + 1, &size) : E_NOMEM;
		if ( er >= E_OK ) {
			memset(&r, 0, sizeof(r));
			r.kind = TR_TAD;
			r.data = buf;
			r.size = size;
			er = tr_psh_dat(&r, 1, name);
		}
		free(buf);
	}
	tad_free(frag);
	if ( er < E_OK ) {
		tell("トレーに入れられませんでした", ( er == E_NOMEM ) ? "メモリが足りません。図形は元のままです" : "図形は元のままです");
		return FALSE;
	}
	snprintf(s, sizeof(s), "copied to the tray %d", nl + ns);
	note(s);
	return TRUE;
}

/* What is protected stays, and is not taken to the tray; the rest goes once it is safe there */
void fg_cut( void )
{
	INT	i;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && is_protected(i) ) {
			g_pick[i] = 0;
			g_npick--;
		}
	}
	fg_sync();
	if ( fg_copy() ) fg_delete();
}

/* The first xmlTAD fragment of the set in hand, parsed; NULL none */
static T_TAD *tray_frag( INT *p_rec )
{
	T_TRINF	inf;
	T_TAD	*doc = NULL;
	UB	*buf;
	SZ	asz = 0;
	INT	i;

	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) return NULL;
		if ( inf.kind == TR_TAD && inf.size > 0 ) break;
	}
	if ( i == TR_REC_MAX ) return NULL;
	buf = malloc((size_t)inf.size + 1);
	if ( buf == NULL ) return NULL;
	if ( tr_rea_rec(TR_HAND, i, buf, inf.size, &asz) >= E_OK ) {
		if ( tad_parse(buf, inf.size, NULL, &doc) < E_OK ) doc = NULL;
	}
	free(buf);
	if ( p_rec != NULL ) *p_rec = i;
	return doc;
}

T_TAD *fg_tray_frag( void )
{
	return tray_frag(NULL);
}

/* Whether the set in hand is a figure this can take: its shapes, its links */
BOOL fg_tray_has( void )
{
	T_TAD		*doc = tray_frag(NULL);
	T_TADNODE	*body;
	BOOL		fig;

	if ( doc == NULL ) return FALSE;
	body = tad_body(doc);
	fig = (BOOL)( body != NULL && body->name != NULL && body->name[0] == 'f' );
	tad_free(doc);
	return fig;
}

/* How many links the set in hand has when it is a figure, what a cabinet takes of it; 0 otherwise */
INT fg_tray_links( void )
{
	T_TAD		*doc = tray_frag(NULL);
	T_TADNODE	*body;
	INT		n = 0;

	if ( doc == NULL ) return 0;
	body = tad_body(doc);
	if ( body != NULL && body->name != NULL && body->name[0] == 'f' ) n = tad_lnk_count(doc);
	tad_free(doc);
	return n;
}

/* The number after the last '_' of a picture beside a record ("<object>_0_<n>.png") */
static INT pic_number( CONST UB *h )
{
	INT	j, v = 0, under = 0;

	for ( j = 0; h != NULL && h[j] != 0 && h[j] != '.'; j++ ) {
		if ( h[j] == '_' ) {
			under++;
			v = 0;
		} else if ( under == 2 && h[j] >= '0' && h[j] <= '9' ) {
			v = v * 10 + ( h[j] - '0' );
		}
	}
	return ( under == 2 ) ? v : 0;
}

static INT pic_top( CONST T_TAD *frag )
{
	CONST T_TAD	*docs[2];
	T_TADNODE	*n;
	INT		top = 0, k, v;

	docs[0] = g_rec;
	docs[1] = frag;
	for ( k = 0; k < 2; k++ ) {
		for ( n = tad_walk(docs[k], NULL); n != NULL; n = tad_walk(docs[k], n) ) {
			if ( n->kind != TAD_ND_ELEM ) continue;
			v = pic_number(tad_attr(n, "href"));
			if ( v > top ) top = v;
		}
	}
	return top;
}

/*
 * The pictures a fragment from the tray names by its set ("tray:N"),
 * written beside this object's record under names of their own past
 * every number used, and the fragment made to name them so
 */
static void tray_pictures( T_TAD *frag )
{
	T_TADNODE	*n;
	T_TRINF		inf;
	CONST UB	*h;
	char		us[TS_UUID_STRLEN + 1];
	UB		name[TS_UUID_STRLEN + 24], *png;
	SZ		asz = 0;
	INT		rn, i;
	ID		k;

	ts_uuid_text(&g_obj, us);
	for ( n = tad_walk(frag, NULL); n != NULL; n = tad_walk(frag, n) ) {
		h = ( n->kind == TAD_ND_ELEM ) ? tad_attr(n, "href") : NULL;
		if ( h == NULL || strncmp((CONST char *)h, "tray:", 5) != 0 ) continue;
		for ( rn = 0, i = 5; h[i] >= '0' && h[i] <= '9'; i++ ) rn = rn * 10 + ( h[i] - '0' );
		if ( tr_ref_rec(TR_HAND, rn, &inf) < E_OK || inf.kind != TR_PNG || inf.size <= 0 ) continue;
		png = malloc((size_t)inf.size);
		if ( png == NULL ) continue;
		k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_W);
		if ( k > 0 && tr_rea_rec(TR_HAND, rn, png, inf.size, &asz) >= E_OK ) {
			snprintf((char *)name, sizeof(name), "%s_0_%d.png", us, pic_top(frag) + 1);
			if ( ob_wri_res(k, name + TS_UUID_STRLEN, png, asz) >= E_OK
			  && tad_set_attr(frag, n, "href", name) >= E_OK ) {
				char	s[TS_UUID_STRLEN + 40];

				snprintf(s, sizeof(s), "tray picture %s", (char *)name);
				note(s);
				tv_src_forget(&g_obj);
			}
		}
		if ( k > 0 ) ob_cls_obj(k);
		free(png);
	}
}

/*
 * The set in hand put into the figure, each time a step further down
 * and across than the last. A pasted link is a new link to the same
 * object. 'take' takes the set out of the tray after (トレーから移動).
 */
void fg_paste( BOOL take )
{
	T_TAD	*frag = tray_frag(NULL);
	T_VOBJ	v;
	TS_UUID	id;
	INT	i, off, first, nlinks;
	char	s[48];

	if ( frag == NULL ) return;
	if ( tad_body(frag) == NULL || tad_body(frag)->name == NULL || tad_body(frag)->name[0] != 'f' || g_fig == NULL ) {
		tad_free(frag);
		return;
	}
	fg_before();
	first = tad_lnk_count(g_rec);
	nlinks = tad_lnk_count(frag);
	pasted++;
	off = ED_STEP * pasted;
	tray_pictures(frag);
	fg_sync();
	(void)fe_paste_frag(&g_ed, frag, off);
	for ( i = 0; i < nlinks; i++ ) {
		if ( tad_lnk_get(frag, i, &v) < E_OK ) continue;
		memset(&v.vobjid, 0, sizeof(v.vobjid));
		v.left += off;  v.right += off;
		v.top += off;   v.bottom += off;
		(void)tad_lnk_add(g_rec, &v, &id);
	}
	tad_free(frag);
	if ( take ) (void)tr_del_dat(TR_HAND);
	fg_model();
	pick_from(first);
	fg_sync();
	fe_pick_pasted(&g_ed);
	g_npick = g_ed.npick;
	fg_after();
	snprintf(s, sizeof(s), "pasted, %d shapes %d picked", ( g_fig != NULL ) ? g_fig->nsh : 0, g_npick);
	note(s);
}

/* ---------------------------------------------------------------- the order of the links */

/* Whether link number k of the record is one of those taken */
static BOOL link_picked( INT k )
{
	INT	i;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && g_fig->sh[i].link == k ) return TRUE;
	}
	return FALSE;
}

static BOOL link_background( INT k )
{
	T_VOBJ	v;

	return (BOOL)( tad_lnk_get(g_rec, k, &v) >= E_OK && v.background );
}

static BOOL moving_front( INT k )
{
	return (BOOL)( !link_picked(k) || link_background(k) );
}

static BOOL moving_back( INT k )
{
	return (BOOL)( link_picked(k) && !link_background(k) );
}

/*
 * The links of the record put in the order of three groups, each keeping
 * the order it had: first those 'first' says, then those 'mid' says, then
 * the rest. Later in the record is further forward.
 */
static ER regroup( BOOL (*first)( INT ), BOOL (*mid)( INT ) )
{
	TS_UUID	*ids, *order;
	BOOL	*placed;
	UINT	*taken;
	T_VOBJ	v;
	INT	n = tad_lnk_count(g_rec), i, k = 0, pass;
	ER	er;

	if ( n <= 0 ) return E_NOEXS;
	ids = malloc(sizeof(TS_UUID) * (size_t)n);
	order = malloc(sizeof(TS_UUID) * (size_t)n);
	placed = malloc(sizeof(BOOL) * (size_t)n);
	if ( ids == NULL || order == NULL || placed == NULL ) {
		free(ids);
		free(order);
		free(placed);
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(g_rec, i, &v) < E_OK ) {
			free(ids);
			free(order);
			free(placed);
			return E_NOEXS;
		}
		ids[i] = v.vobjid;
		placed[i] = FALSE;
	}
	for ( pass = 0; pass < 3; pass++ ) {
		for ( i = 0; i < n; i++ ) {
			BOOL	in;

			if ( placed[i] ) continue;
			in = ( pass == 0 ) ? ( first != NULL && first(i) )
			   : ( pass == 1 ) ? ( mid != NULL && mid(i) ) : TRUE;
			if ( in ) {
				order[k++] = ids[i];
				placed[i] = TRUE;
			}
		}
	}
	/*
	 * A pick belongs to a shape by its number, and the numbers of the
	 * links' shapes change with their order: what is taken is kept by
	 * the identities of its links, and taken again by them once the
	 * model has been made again.
	 */
	taken = calloc((size_t)n, sizeof(UINT));
	if ( taken == NULL ) {
		free(ids);
		free(order);
		free(placed);
		return E_NOMEM;
	}
	for ( i = 0; g_fig != NULL && i < g_fig->nsh && i < g_maxpick; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) && g_fig->sh[i].link < n ) taken[g_fig->sh[i].link] = g_pick[i];
	}
	er = tad_lnk_reorder(g_rec, order, n);
	if ( er >= E_OK ) {
		fg_model();
		pick_gen++;
		g_npick = 0;
		for ( i = 0; i < g_maxpick; i++ ) g_pick[i] = 0;
		for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
			INT	j;

			if ( !fg_is_link(i) || tad_lnk_get(g_rec, g_fig->sh[i].link, &v) < E_OK ) continue;
			for ( j = 0; j < n; j++ ) {
				if ( taken[j] != 0 && memcmp(&ids[j], &v.vobjid, sizeof(TS_UUID)) == 0 ) {
					g_pick[i] = taken[j];
					g_npick++;
					break;
				}
			}
		}
		fg_sync();
	}
	free(taken);
	free(placed);
	free(order);
	free(ids);
	return er;
}

/*
 * いちばん前へ and いちばん後ろへ of a cabinet, by the order of the links in
 * the record: what is taken goes after the rest, or behind all but what
 * is in the background. What is in the background stays behind.
 */
void fg_link_order( BOOL front )
{
	if ( g_fig == NULL || g_npick == 0 ) return;
	fg_before();
	if ( front ) (void)regroup(moving_front, NULL);
	else (void)regroup(link_background, moving_back);
	fg_after();
	note(front ? "links to the front" : "links to the back");
}

/* 固定化, 背景化 and taking them off: what the links taken are. What goes into the background goes behind the rest too */
void fg_protect( BOOL background, BOOL on )
{
	T_VOBJ	v;
	INT	i, n = 0;

	for ( i = 0; g_fig != NULL && i < g_fig->nsh; i++ ) {
		if ( g_pick[i] != 0 && fg_is_link(i) ) n++;
	}
	if ( n == 0 ) return;
	fg_before();
	for ( i = 0; i < g_fig->nsh; i++ ) {
		if ( g_pick[i] == 0 || !fg_is_link(i) || tad_lnk_get(g_rec, g_fig->sh[i].link, &v) < E_OK ) continue;
		if ( background ) v.background = on;
		else v.fixed = on;
		link_put(&v);
	}
	if ( background && on ) (void)regroup(link_background, NULL);
	fg_after();
}

/* Links dropped on the window: put where they were let go, taken */
void fg_dropped( CONST T_OBDROP *dr )
{
	T_VOBJ	v;
	TS_UUID	id;
	INT	i, first, x, y, n = 0;
	char	s[48];

	if ( g_fig == NULL || dr->n <= 0 ) return;
	fg_before();
	first = tad_lnk_count(g_rec);
	x = unz(dr->x + g_sx);
	y = unz(dr->y + g_sy);
	for ( i = 0; i < dr->n; i++ ) {
		CONST T_OBDROPV	*d = &dr->v[i];
		INT		w = d->right - d->left, h = d->bottom - d->top;

		memset(&v, 0, sizeof(v));
		v.target = d->target;
		v.recno = 0;
		v.frcol = d->frcol;
		v.chcol = d->chcol;
		v.tbcol = d->tbcol;
		v.bgcol = d->bgcol;
		v.chsz = ( d->chsz > 0 ) ? d->chsz : 14;
		v.disp = ( d->disp != 0 ) ? d->disp : TAD_D_DEFAULT;
		v.zoom = 100;
		/* the box it had where it was, no smaller than a link with its letters is; closed when it was */
		if ( w < tad_lnk_least_w(&v) ) w = tad_lnk_least_w(&v);
		h = ( h > 0 ) ? tad_lnk_fit_h(&v, h) : tad_lnk_closed_h(&v);
		/* each where it stood from the first, as they were carried */
		v.left = x + ( d->left - dr->v[0].left );
		v.top = y + ( d->top - dr->v[0].top );
		v.right = v.left + w;
		v.bottom = v.top + h;
		v.height = h;
		if ( tad_lnk_add(g_rec, &v, &id) >= E_OK ) n++;
	}
	fg_model();
	pick_from(first);
	fg_after();
	snprintf(s, sizeof(s), "dropped %d", n);
	note(s);
}
