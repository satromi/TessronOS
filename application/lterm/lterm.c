/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lterm.c
 *	Lines of text in a window of a program's own (design 16.5.19)
 */

#include "lterm.h"

#define INK		0x00000000U
#define GROUND		0x00FFFFFFU
#define CARET		0x00303030U

LOCAL UB *cur_line( LTERM *t )
{
	return t->line[( t->first + t->n - 1 ) % LT_LINES];
}

LOCAL INT *cur_len( LTERM *t )
{
	/* whoever asks for it may change it: its rows are counted again */
	t->rows[( t->first + t->n - 1 ) % LT_LINES] = 0;
	return &t->len[( t->first + t->n - 1 ) % LT_LINES];
}

LOCAL void newline( LTERM *t )
{
	if ( t->n < LT_LINES ) {
		t->n++;
	} else {
		t->first = ( t->first + 1 ) % LT_LINES;	/* the oldest goes */
	}
	*cur_len(t) = 0;
	if ( t->back > 0 ) {
		t->back++;			/* a wound back view stays where it was */
	}
}

EXPORT void lt_clear( LTERM *t )
{
	t->first = 0;
	t->n = 1;
	t->len[0] = 0;
	t->cr = FALSE;
	t->esc = 0;
	t->back = 0;
	t->rows[0] = 0;
	t->wrap_w = 0;
	t->total = t->shown = 0;
	t->grab = -1;
}

/*
 * Bytes as a terminal shows them: a line feed starts a new line, a
 * carriage return the same one again, a backspace takes the last letter
 * off, a tab is spaces; escape sequences and the other controls are
 * passed over.
 */
EXPORT void lt_put( LTERM *t, CONST UB *s, INT n )
{
	INT	i, *len;
	UB	c;

	for ( i = 0; i < n; i++ ) {
		c = s[i];
		if ( t->esc == 1 ) {
			t->esc = ( c == '[' ) ? 2 : 0;
			continue;
		}
		if ( t->esc == 2 ) {
			if ( c >= 0x40 && c <= 0x7E ) t->esc = 0;
			continue;
		}
		if ( c == '\n' ) {
			t->cr = FALSE;
			newline(t);
			continue;
		}
		if ( c == '\r' ) {
			t->cr = TRUE;
			continue;
		}
		if ( c == 0x1B ) {
			t->esc = 1;
			continue;
		}
		len = cur_len(t);
		if ( t->cr ) {
			*len = 0;		/* written over from its start */
			t->cr = FALSE;
		}
		if ( c == 0x08 || c == 0x7F ) {
			while ( *len > 0 && ( cur_line(t)[--*len] & 0xC0 ) == 0x80 ) ;
			continue;
		}
		if ( c == '\t' ) {
			do {
				if ( *len < LT_COLS - 1 ) cur_line(t)[(*len)++] = ' ';
			} while ( ( *len % 8 ) != 0 && *len < LT_COLS - 1 );
			continue;
		}
		if ( c < 0x20 ) {
			continue;
		}
		if ( *len < LT_COLS - 1 ) {
			cur_line(t)[(*len)++] = c;
		}
	}
}

EXPORT void lt_puts( LTERM *t, CONST char *s )
{
	lt_put(t, (CONST UB *)s, (INT)strlen(s));
}

EXPORT INT lt_num( UB *out, D v )
{
	UB	d[24];
	UD	u = ( v < 0 ) ? (UD)-v : (UD)v;
	INT	n = 0, i = 0;

	if ( v < 0 ) out[n++] = '-';
	do {
		d[i++] = (UB)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 );
	while ( i > 0 ) out[n++] = d[--i];
	out[n] = 0;
	return n;
}

EXPORT void lt_putn( LTERM *t, D v )
{
	UB	b[24];

	lt_put(t, b, lt_num(b, v));
}

EXPORT BOOL lt_same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 || a[i] == ' ' );
}

LOCAL INT hexv( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT ER lt_uuid( CONST char *s, TS_UUID *u )
{
	INT	i = 0, k = 0, h, l;

	while ( k < 16 ) {
		if ( s[i] == '-' ) {
			i++;
			continue;
		}
		h = hexv(s[i]);
		l = ( h >= 0 ) ? hexv(s[i + 1]) : -1;
		if ( l < 0 ) {
			return E_PAR;
		}
		u->b[k++] = (UB)( h * 16 + l );
		i += 2;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- drawing */

/* The work area's size, from the window's outer edge */
LOCAL void work_size( LTWIN *w, INT *p_w, INT *p_h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	*p_w = 600;
	*p_h = 360;
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK
	  && asz >= (SZ)sizeof(wp) ) {
		*p_w = wp.wright - wp.wleft;
		*p_h = wp.wbottom - wp.wtop;
	}
	if ( *p_w < 40 ) *p_w = 40;
	if ( *p_h < LT_LH ) *p_h = LT_LH;
}

EXPORT void lt_wind( LTERM *t, INT rows )
{
	INT	most = t->total - t->shown;

	t->back += rows;
	if ( most < 0 ) most = 0;
	if ( t->back > most ) t->back = most;
	if ( t->back < 0 ) t->back = 0;
}

/* How wide some bytes are, drawn in the letters' size */
LOCAL INT width_of( CONST UB *s, INT n )
{
	static UB buf[LT_COLS + 1];
	INT	i;

	for ( i = 0; i < n && i < LT_COLS; i++ ) buf[i] = s[i];
	buf[i] = 0;
	return ( i > 0 ) ? dp_text_width(buf, LT_PX) : 0;
}

/*
 * Where the row that starts at `from` ends: as many whole letters as fit
 * in the width, and at least one, so that a letter wider than the window
 * still goes somewhere.
 */
LOCAL INT row_end( CONST UB *s, INT from, INT len, INT width )
{
	static INT cut[LT_COLS + 1];	/* kept off the stack: a program's is small */
	INT	n = 0, i, lo, hi, mid;

	if ( width_of(s + from, len - from) <= width ) {
		return len;
	}
	/* where each letter after the first ends: the places a row may end */
	for ( i = from + 1; i <= len; i++ ) {
		if ( i == len || ( s[i] & 0xC0 ) != 0x80 ) cut[n++] = i;
	}
	/* the first letter is taken whatever its width; then the most that fit */
	lo = 0;
	hi = n - 1;
	while ( lo < hi ) {
		mid = ( lo + hi + 1 ) / 2;
		if ( width_of(s + from, cut[mid] - from) <= width ) lo = mid;
		else hi = mid - 1;
	}
	return cut[lo];
}

/* The rows some text takes in the width */
LOCAL INT rows_of( CONST UB *s, INT len, INT width )
{
	INT	at = 0, n = 0;

	if ( len <= 0 ) return 1;
	while ( at < len ) {
		at = row_end(s, at, len, width);
		n++;
	}
	return n;
}

/*
 * Draw the rows of some text that fall between the rows first..last of
 * the view, the text's own first row being number `row`. Answers where
 * its last letter ended, for the caret, and how many rows it took.
 */
LOCAL INT draw_rows( LTWIN *w, CONST UB *s, INT len, INT width, INT row, INT first, INT last,
		     INT *p_x, INT *p_y )
{
	static UB buf[LT_COLS + 1];
	INT	at = 0, e, n = 0, i, y;

	do {
		e = ( len > 0 ) ? row_end(s, at, len, width) : 0;
		y = LT_PAD + ( row + n - first ) * LT_LH;
		*p_x = LT_PAD;
		*p_y = y;
		if ( row + n >= first && row + n <= last && e > at ) {
			for ( i = 0; i < e - at; i++ ) buf[i] = s[at + i];
			buf[i] = 0;
			*p_x += dp_text(w->gid, LT_PAD, y + LT_PX, buf, INK, LT_PX);
		}
		at = e;
		n++;
	} while ( at < len );
	return n;
}

EXPORT void lt_draw( LTERM *t, LTWIN *w, CONST UB *tail, BOOL caret )
{
	T_DPRECT	all, bar;
	T_WMBAR		b;
	INT		ww, wh, rows, last, first, row, k, i, nl, width, bw, cx = LT_PAD, cy = LT_PAD;
	INT		tlen = ( tail != NULL ) ? (INT)strlen((CONST char *)tail) : 0;

	work_size(w, &ww, &wh);
	all.left = 0;  all.top = 0;
	all.right = ww;  all.bottom = wh;
	(void)dp_fill_rect(w->gid, &all, GROUND);

	/* the scroll bar down the right side, as wide as the windows' own */
	bw = (INT)wm_look(LK_BAR_W);
	if ( bw < 6 || bw > 64 ) bw = 16;
	width = ww - bw - 2 * LT_PAD;
	if ( width < 16 ) width = 16;
	if ( width != t->wrap_w ) {
		for ( i = 0; i < LT_LINES; i++ ) t->rows[i] = 0;
		t->wrap_w = width;
	}

	rows = ( wh - 2 * LT_PAD ) / LT_LH;
	if ( rows < 1 ) rows = 1;
	/* the line being written, when it is still empty, is where the tail goes */
	nl = t->n;
	if ( tail != NULL && nl > 0 && t->len[( t->first + nl - 1 ) % LT_LINES] == 0 ) nl--;

	/* the rows there are: each line's, counted once for this width */
	t->total = 0;
	for ( row = 0; row < nl; row++ ) {
		k = ( t->first + row ) % LT_LINES;
		if ( t->rows[k] == 0 ) t->rows[k] = rows_of(t->line[k], t->len[k], width);
		t->total += t->rows[k];
	}
	if ( tail != NULL ) t->total += rows_of(tail, tlen, width);
	t->shown = rows;
	lt_wind(t, 0);				/* kept within what there is */

	last = t->total - 1 - t->back;		/* the row at the foot of the view */
	first = last - rows + 1;
	if ( first < 0 ) {
		first = 0;
		last = rows - 1;
	}
	for ( row = 0, i = 0; i < nl && row <= last; i++ ) {
		k = ( t->first + i ) % LT_LINES;
		if ( row + t->rows[k] - 1 < first ) {
			row += t->rows[k];		/* above the view: nothing to draw */
			continue;
		}
		row += draw_rows(w, t->line[k], t->len[k], width, row, first, last, &cx, &cy);
	}
	if ( tail != NULL && row <= last ) {
		(void)draw_rows(w, tail, tlen, width, row, first, last, &cx, &cy);
		if ( caret ) {
			bar.left = cx + 1;  bar.right = cx + 3;
			bar.top = cy + 1;   bar.bottom = cy + LT_LH - 1;
			(void)dp_fill_rect(w->gid, &bar, CARET);
		}
	}

	t->bar.left = ww - bw;
	t->bar.top = 0;
	t->bar.right = ww;
	t->bar.bottom = wh;
	b.lo = 0;
	b.hi = ( t->total > 0 ) ? t->total : 1;
	b.clo = ( t->total > rows ) ? first : 0;
	b.chi = ( t->total > rows ) ? first + rows : b.hi;
	(void)wm_draw_bar(w->gid, &t->bar, &b, FALSE, TRUE);
	(void)wm_obj_flush(w->kw, NULL);
}

/* The knob's place along the bar, in the same reckoning the bar is drawn with */
LOCAL void knob_of( CONST LTERM *t, INT *p_k0, INT *p_k1 )
{
	INT	len = t->bar.bottom - t->bar.top - 2;
	INT	total = ( t->total > 0 ) ? t->total : 1;
	INT	first = total - t->shown - t->back;

	if ( first < 0 ) first = 0;
	*p_k0 = t->bar.top + 1 + first * len / total;
	*p_k1 = t->bar.top + 1 + ( first + t->shown ) * len / total;
	if ( *p_k1 > t->bar.bottom - 1 ) *p_k1 = t->bar.bottom - 1;
	if ( *p_k1 - *p_k0 < 8 ) *p_k1 = *p_k0 + 8;
}

EXPORT BOOL lt_pointer( LTERM *t, CONST T_OBNTM *m )
{
	INT	k0, k1, len, first;

	if ( m->event == OB_E_WHEEL ) {
		if ( m->code != 0 || m->dz == 0 || wm_look(LK_PD_WHEEL) == 0 ) return FALSE;
		lt_wind(t, m->dz * LT_WHEEL);
		return TRUE;
	}
	if ( m->event == OB_E_RELEASE ) {
		if ( t->grab < 0 ) return FALSE;
		t->grab = -1;
		return TRUE;
	}
	if ( m->event == OB_E_MOVE ) {
		if ( t->grab < 0 ) return FALSE;
		len = t->bar.bottom - t->bar.top - 2;
		if ( len <= 0 ) return FALSE;
		first = ( m->y - t->grab - t->bar.top - 1 ) * t->total / len;
		t->back = t->total - t->shown - first;
		lt_wind(t, 0);
		return TRUE;
	}
	if ( m->event != OB_E_PRESS || m->code != 0 ) return FALSE;
	if ( m->x < t->bar.left || m->x >= t->bar.right || m->y < t->bar.top || m->y >= t->bar.bottom ) {
		return FALSE;
	}
	knob_of(t, &k0, &k1);
	if ( m->y < k0 ) {
		lt_wind(t, t->shown - 1);		/* a page back */
	} else if ( m->y >= k1 ) {
		lt_wind(t, -( t->shown - 1 ));		/* a page on */
	} else {
		t->grab = m->y - k0;			/* the knob, held until let go */
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the window */

EXPORT ER lt_open( LTWIN *w, CONST char *name, CONST char *json )
{
	T_OBCRE	c;
	T_OBNTF	req;
	ER	er;

	memset(w, 0, sizeof(*w));
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)name;
	c.json = (CONST UB *)json;
	c.jsonsz = (INT)strlen(json);
	er = ob_cre_obj(&c, &w->win);
	if ( er < E_OK ) {
		return er;
	}
	w->kw = ob_opn_obj(&w->win, OB_OP_ALL);
	w->gid = ( w->kw > 0 ) ? wm_obj_gid(w->kw) : E_ID;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	w->port = ( ob_cre_obj(&c, &w->ch) >= E_OK )
		? ob_opn_obj(&w->ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY
		   | OB_E_REDRAW | OB_E_DELETE | OB_E_WHEEL;
	if ( w->kw <= 0 || w->gid < 0 || w->port <= 0
	  || ob_ntf_evt(w->kw, OB_REC_ANY, &req, w->port) <= 0 ) {
		lt_close(w);
		return E_OBJ;
	}
	return E_OK;
}

EXPORT void lt_close( LTWIN *w )
{
	if ( w->port > 0 ) {
		ob_cls_obj(w->port);
		(void)ob_del_obj(&w->ch);
	}
	if ( w->kw > 0 ) {
		ob_cls_obj(w->kw);
	}
	(void)ob_del_obj(&w->win);
	w->port = w->kw = 0;
}
