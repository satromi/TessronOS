/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_list.c
 *	マイクロカード: the list window (design 11.18)
 *
 *	The cards worked on, a row each: the mark at the left, then the
 *	fields whose width in the list is not 0, each as many letters wide;
 *	a number to the right of its column, "###" when it does not fit. The
 *	row of the card the card window shows is turned over, and follows it.
 *
 *	A press on a row shows its card in the card window, pressed twice
 *	brings that window to the front, on the mark turns the mark over. ↑
 *	and ↓ go to the row above and below, Alt with them to the first and
 *	the last; Delete deletes the card, asked first. A row carried with
 *	the second button pressed on the way and let go over another row is
 *	copied after it (複写); carried out of the window it goes to where it
 *	is let go as a text of the values separated by tabs. Links dropped on
 *	the list are read in as cards after the row they are let go over.
 */

#include "mc.h"
#include <ts/obdrop.h>

#define MARK_W		24

static INT	g_lx, g_ly;		/* how far the list is scrolled */
static BOOL	hold;
static INT	hold_pos, hold_x, hold_y;
static BOOL	hold_moved, hold_copy;
static UD	press_at;
static INT	press_pos = -1;

BOOL list_holding( void )	{ return hold; }

static INT head_h( void )	{ return list_px() + 10; }
static INT rh( void )		{ return list_px() + 8; }

static INT col_w( INT k )
{
	CONST MCFIELD	*f = &g_db.f[k];
	INT		px = list_px();

	if ( f->width <= 0 ) return 0;
	if ( f->type == MC_T_NUM ) return f->width * px / 2 + 12;
	return f->width * px + 8;
}

static INT total_w( void )
{
	INT	k, w = MARK_W;

	for ( k = 0; k < g_db.nf; k++ ) w += col_w(k);
	return w;
}

static void fill( INT l, INT t, INT r, INT b, UW c )
{
	T_DPRECT	rc;

	rc.left = l;
	rc.top = t;
	rc.right = r;
	rc.bottom = b;
	if ( r > l && b > t ) (void)dp_fill_rect(g_lw.gid, &rc, c);
}

/* Words cut at a letter's edge to fit width */
static INT cut_to( CONST UB *s, INT px, INT width, UB *out, INT max )
{
	INT	n;

	snprintf((char *)out, (size_t)max, "%s", (CONST char *)s);
	n = (INT)strlen((char *)out);
	while ( n > 0 && dp_text_width(out, px) > width ) {
		do {
			n--;
		} while ( n > 0 && ( out[n] & 0xC0 ) == 0x80 );
		out[n] = 0;
	}
	return n;
}

static void cell( INT x, INT y, INT w, CONST UB *v, BOOL right, UW ink )
{
	UB	s[MC_VALUE_MAX + 8];
	INT	px = list_px(), tw;

	if ( right ) {
		if ( dp_text_width(v, px) > w - 8 ) {
			snprintf((char *)s, sizeof(s), "###");
		} else {
			snprintf((char *)s, sizeof(s), "%s", (CONST char *)v);
		}
		tw = dp_text_width(s, px);
		(void)dp_text(g_lw.gid, x + w - 6 - tw, y, s, ink, px);
		return;
	}
	(void)cut_to(v, px, w - 8, s, sizeof(s));
	(void)dp_text(g_lw.gid, x + 4, y, s, ink, px);
}

static void bars( void )
{
	T_OBWBARS	b;
	SZ		asz = 0;
	INT		n = mc_count(&g_db);

	memset(&b, 0, sizeof(b));
	b.bar[OB_BAR_R].lo = 0;
	b.bar[OB_BAR_R].hi = n * rh();
	b.bar[OB_BAR_R].clo = g_ly;
	b.bar[OB_BAR_R].chi = g_ly + ( g_lw.h - head_h() );
	b.bar[OB_BAR_B].lo = 0;
	b.bar[OB_BAR_B].hi = total_w();
	b.bar[OB_BAR_B].clo = g_lx;
	b.bar[OB_BAR_B].chi = g_lx + g_lw.w;
	if ( b.bar[OB_BAR_R].chi > b.bar[OB_BAR_R].hi ) b.bar[OB_BAR_R].hi = b.bar[OB_BAR_R].chi;
	if ( b.bar[OB_BAR_B].chi > b.bar[OB_BAR_B].hi ) b.bar[OB_BAR_B].hi = b.bar[OB_BAR_B].chi;
	(void)ob_wri_rec(g_lw.kw, OB_WR_BARS, 0, &b, 2 * sizeof(T_OBWBAR), &asz);
}

void list_paint( void )
{
	INT	n, pos, i, k, x, y, px = list_px(), top;
	UB	v[MC_VALUE_MAX + 8];

	if ( !g_lw.open || g_lw.gid < 0 ) return;
	n = mc_count(&g_db);
	fill(0, 0, g_lw.w, g_lw.h, C_PAPER);
	top = head_h();
	pos = g_ly / rh();
	i = mc_index(&g_db, pos);
	for ( y = top + pos * rh() - g_ly; pos < n && y < g_lw.h && i >= 0; pos++, y += rh() ) {
		MCCARD		*c = g_db.c[i];
		BOOL		in_hand = (BOOL)( pos == g_cur );
		UW		ink = in_hand ? C_PAPER : C_TEXT;
		T_DPRECT	m;

		if ( in_hand ) fill(0, y, g_lw.w, y + rh(), C_PICK);
		/* the mark */
		m.left = 5 - g_lx;
		m.top = y + ( rh() - 14 ) / 2;
		m.right = m.left + 14;
		m.bottom = m.top + 14;
		if ( m.right > 0 ) {
			(void)dp_fill_rect(g_lw.gid, &m, C_PAPER);
			(void)dp_frame_rect(g_lw.gid, &m, C_TEXT, 1);
			if ( c->mark ) fill(m.left + 3, m.top + 3, m.right - 3, m.bottom - 3, C_PICK);
		}
		x = MARK_W - g_lx;
		for ( k = 0; k < g_db.nf; k++ ) {
			INT	w = col_w(k);

			if ( w == 0 ) continue;
			if ( x + w > 0 && x < g_lw.w ) {
				(void)mc_shown(&g_db, c, k, v, sizeof(v));
				cell(x, y + ( rh() + px ) / 2 - px / 8, w, v, (BOOL)( g_db.f[k].type == MC_T_NUM ), ink);
				fill(x + w - 1, y, x + w, y + rh(), in_hand ? C_PICK : 0x00E0E0E0U);
			}
			x += w;
		}
		fill(0, y + rh() - 1, g_lw.w, y + rh(), in_hand ? C_PICK : 0x00E8E8E8U);
		/* the next card worked on */
		for ( i++; i < g_db.n && !mc_in(&g_db, i); i++ ) ;
		if ( i >= g_db.n ) i = -1;
	}
	/* the names along the top */
	fill(0, 0, g_lw.w, top, C_BAR);
	fill(0, top - 1, g_lw.w, top, C_LINE);
	x = MARK_W - g_lx;
	for ( k = 0; k < g_db.nf; k++ ) {
		INT	w = col_w(k);

		if ( w == 0 ) continue;
		if ( x + w > 0 && x < g_lw.w ) {
			cell(x, ( top + px ) / 2 - px / 8 + 1, w, g_db.f[k].name, FALSE, C_TEXT);
			fill(x + w - 1, 0, x + w, top, C_LINE);
		}
		x += w;
	}
	(void)wm_obj_flush(g_lw.kw, NULL);
	bars();
	g_lw.dirty = FALSE;
}

/* The row of the card shown brought into sight */
void list_show_cur( void )
{
	INT	t = g_cur * rh(), b = t + rh(), vh = g_lw.h - head_h();

	if ( t < g_ly ) g_ly = t;
	else if ( b > g_ly + vh && vh > 0 ) g_ly = b - vh;
	if ( g_ly < 0 ) g_ly = 0;
	g_lw.dirty = TRUE;
}

static INT pos_at( INT y )
{
	INT	p;

	if ( y < head_h() ) return -1;
	p = ( y - head_h() + g_ly ) / rh();
	return ( p >= 0 && p < mc_count(&g_db) ) ? p : -1;
}

static void press( CONST T_OBNTM *m )
{
	INT	p = pos_at(m->y);
	BOOL	twice;

	if ( m->x < 0 || m->y < 0 || m->x >= g_lw.w || m->y >= g_lw.h || p < 0 ) return;
	twice = (BOOL)( p == press_pos && m->when - press_at < DBL_NS );
	press_at = m->when;
	press_pos = p;
	if ( m->x + g_lx < MARK_W ) {
		INT	i = mc_index(&g_db, p);

		if ( i >= 0 ) {
			char	s[48];

			g_db.c[i]->mark = (BOOL)!g_db.c[i]->mark;
			snprintf(s, sizeof(s), "mark %d %s", p + 1, g_db.c[i]->mark ? "on" : "off");
			note(s);
			redraw_all();
		}
		return;
	}
	show_card(p);
	if ( twice ) {
		win_front(&g_cw);
		note("card window front");
		return;
	}
	hold = TRUE;
	hold_pos = p;
	hold_x = m->x;
	hold_y = m->y;
	hold_moved = FALSE;
	hold_copy = FALSE;
}

static void moved( CONST T_OBNTM *m )
{
	if ( !hold ) return;
	if ( m->x - hold_x >= MOVE_MIN || hold_x - m->x >= MOVE_MIN || m->y - hold_y >= MOVE_MIN || hold_y - m->y >= MOVE_MIN ) {
		hold_moved = TRUE;
	}
	if ( m->x < 0 || m->y < 0 || m->x >= g_lw.w || m->y >= g_lw.h ) {
		hold = FALSE;
		carry_rows_out(hold_pos, hold_pos);
	}
}

static void released( CONST T_OBNTM *m )
{
	INT	p;

	if ( !hold ) return;
	hold = FALSE;
	if ( !hold_moved || !hold_copy ) return;
	p = pos_at(m->y);
	if ( p < 0 && m->y >= head_h() ) p = mc_count(&g_db) - 1;
	if ( p < 0 ) return;
	{
		INT	from = mc_index(&g_db, hold_pos), after = mc_index(&g_db, p);
		char	s[64];

		if ( from < 0 || after < 0 ) return;
		if ( mc_card_dup(&g_db, from, after + 1) == NULL ) {
			warn("カードを複製できません。", NULL);
			return;
		}
		g_changed = TRUE;
		show_card(mc_pos(&g_db, after + 1));
		snprintf(s, sizeof(s), "row copied %d after %d", hold_pos + 1, p + 1);
		note(s);
		redraw_all();
	}
}

static void key( CONST T_OBNTM *m )
{
	BOOL	alt = (BOOL)( ( m->mods & M_ALT ) != 0 );

	if ( ( m->mods & M_CTRL ) != 0 ) {
		UB	c = wm_key_char(m->code, 0);

		switch ( c ) {
		case 'e': case 'E':	command("close", 0);	break;
		case 's': case 'S':	command("save", 0);	break;
		case 'c': case 'C':	command("copy", 0);	break;
		case 'v': case 'V':	command("paste", 0);	break;
		case 'n': case 'N':	command("card.new", 0);	break;
		case 'd': case 'D':	command("card.dup", 0);	break;
		case 'l': case 'L':	command("list", 0);	break;
		default:		break;
		}
		return;
	}
	switch ( m->code ) {
	case K_UP:
		show_card(alt ? 0 : g_cur - 1);
		break;
	case K_DOWN:
		show_card(alt ? mc_count(&g_db) - 1 : g_cur + 1);
		break;
	case K_DEL:
		command("card.del", 0);
		break;
	case K_ENTER:
	case K_KPENTER:
		win_front(&g_cw);
		break;
	default:
		break;
	}
}

void list_dropped( void )
{
	T_OBDROP	*d;
	INT		i, p, total = 0;
	char		s[64];

	/* the list window's own drop record, every link of it */
	d = ob_drop_read(g_lw.kw);
	if ( d == NULL ) return;
	if ( d->n <= 0 ) {
		free(d);
		return;
	}
	p = pos_at(d->y);
	if ( p < 0 ) p = ( d->y < head_h() ) ? -1 : mc_count(&g_db) - 1;
	{
		T_OBDRANS	a;
		SZ		asz = 0;

		memset(&a, 0, sizeof(a));
		a.seq = d->seq;
		a.answer = OB_DR_ACCEPT;
		(void)ob_wri_rec(g_lw.kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	}
	for ( i = 0; i < d->n; i++ ) {
		INT	at = ( p >= 0 ) ? mc_index(&g_db, p) + 1 : 0, r;

		if ( p >= 0 && at <= 0 ) at = g_db.n;
		r = import_obj(&d->v[i].target, at);
		if ( r > 0 ) {
			total += r;
			p = ( p >= 0 ? p : -1 ) + r;
		}
	}
	snprintf(s, sizeof(s), "read in %d", total);
	note(s);
	if ( total > 0 ) {
		g_changed = TRUE;
		snprintf(s, sizeof(s), "%d 枚のカードを読み込みました", total);
		say(s);
	} else {
		say("カードとして読み込めるデータがありません");
	}
	redraw_all();
	free(d);
}

void list_event( CONST T_OBNTM *m )
{
	switch ( m->event ) {
	case OB_E_PRESS:
		if ( m->code == 0 ) {
			press(m);
		} else if ( m->code == 1 && hold ) {
			hold_copy = TRUE;		/* the second button on the way: a copy */
		}
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m);
		break;
	case OB_E_MOVE:
		moved(m);
		break;
	case OB_E_LEAVE:
		if ( hold && hold_moved ) {
			hold = FALSE;
			carry_rows_out(hold_pos, hold_pos);
		}
		break;
	case OB_E_KEY:
		key(m);
		break;
	case OB_E_WHEEL:
		if ( m->code == HID_WHEEL_H ) g_lx += m->dz * 60;	/* across: the way it turns */
		else if ( ( m->mods & M_SHIFT ) != 0 ) g_lx -= m->dz * 60;
		else g_ly -= m->dz * 3 * rh();
		if ( g_lx < 0 ) g_lx = 0;
		if ( g_ly < 0 ) g_ly = 0;
		g_lw.dirty = TRUE;
		break;
	case OB_E_SCROLL:
		if ( m->code == OB_BAR_R ) g_ly = m->x;
		else if ( m->code == OB_BAR_B ) g_lx = m->x;
		if ( g_lx < 0 ) g_lx = 0;
		if ( g_ly < 0 ) g_ly = 0;
		g_lw.dirty = TRUE;
		break;
	case OB_E_DROP:
		list_dropped();
		break;
	default:
		break;
	}
}
