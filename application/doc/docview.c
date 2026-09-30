/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docview.c
 *	Setting a document out on the screen (design 17.5.2, phase 14)
 *
 *	This is the part of the text editor that everything else needs: the
 *	cabinet shows documents, the figure editor holds pieces of text,
 *	and a person reading is doing nothing but this. Editing sits on top
 *	of it later; laying out and drawing come first, because a document
 *	that cannot be shown cannot be edited either.
 *
 *	The line is broken where it stops fitting, pulled back for the
 *	characters that may not begin or end one. Japanese allows a break
 *	between almost any two characters, which is why the rule that
 *	matters is not "where may it break" but "where may it not": a full
 *	stop may not begin a line, and an opening bracket may not end one.
 *
 *	Nothing here measures by counting characters. What fits is asked of
 *	the font layer, which walks the letters it will actually draw; a
 *	count of characters times a width is wrong the moment two letters
 *	differ, and every document has letters that differ.
 *
 *	A line is laid in two passes: what goes on it is found first, then
 *	how tall it is -- the tallest letters, the ruby over them, a picture
 *	standing in it -- and only then is anything put down, on a base line
 *	common to the whole line. Its pitch is the letters' size and the gap
 *	the paragraph's tab format asks for, half a size when it asks none.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/tadview.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/wm.h>
#include <ts/om.h>
#include <ts/dt.h>

#define LINK_W		120		/* a virtual object standing in the text */
#define LINK_H		22

/* ---------------------------------------------------------------- letters */

LOCAL UW utf8_at( CONST UB *s, INT *p_n )
{
	UW	c = s[0];
	INT	n = 1;

	if ( (c & 0x80U) == 0 ) {
		n = 1;
	} else if ( (c & 0xE0U) == 0xC0U ) {
		c = ((c & 0x1FU) << 6) | (s[1] & 0x3FU);
		n = 2;
	} else if ( (c & 0xF0U) == 0xE0U ) {
		c = ((c & 0x0FU) << 12) | ((s[1] & 0x3FU) << 6) | (s[2] & 0x3FU);
		n = 3;
	} else {
		c = ((c & 0x07U) << 18) | ((s[1] & 0x3FU) << 12)
		    | ((s[2] & 0x3FU) << 6) | (s[3] & 0x3FU);
		n = 4;
	}
	if ( p_n != NULL ) {
		*p_n = n;
	}

	return c;
}

/* Characters that may not begin a line */
LOCAL BOOL no_start( UW c )
{
	LOCAL CONST UW	tab[] = {
		0x3001, 0x3002,			/* 、 。 */
		0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B,	/* ， ． ： ； */
		0xFF1F, 0xFF01,			/* ？ ！ */
		0x30FB, 0x30FC,			/* ・ ー */
		0xFF09, 0x300D, 0x300F, 0x3011,	/* ） 」 』 】 */
		0x3015, 0xFF5D, 0x3009, 0x300B,	/* 〕 ｝ 〉 》 */
		0x3041, 0x3043, 0x3045, 0x3047, 0x3049,	/* ぁぃぅぇぉ */
		0x3063, 0x3083, 0x3085, 0x3087, 0x308E,	/* っゃゅょゎ */
		0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,	/* ァィゥェォ */
		0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE,	/* ッャュョヮ */
		',', '.', ':', ';', '?', '!', ')', ']', '}',
	};
	INT	i;

	for ( i = 0; i < (INT)(sizeof(tab) / sizeof(tab[0])); i++ ) {
		if ( tab[i] == c ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* Characters that may not end one */
LOCAL BOOL no_end( UW c )
{
	LOCAL CONST UW	tab[] = {
		0xFF08, 0x300C, 0x300E, 0x3010,	/* （ 「 『 【 */
		0x3014, 0xFF5B, 0x3008, 0x300A,	/* 〔 ｛ 〈 《 */
		'(', '[', '{',
	};
	INT	i;

	for ( i = 0; i < (INT)(sizeof(tab) / sizeof(tab[0])); i++ ) {
		if ( tab[i] == c ) {
			return TRUE;
		}
	}

	return FALSE;
}


/* Whether a letter is among those of a 禁則 setting (UTF-8, n bytes) */
LOCAL BOOL in_letters( CONST UB *s, INT n, UW c )
{
	INT	i = 0, m;

	while ( s != NULL && i < n ) {
		if ( utf8_at(s + i, &m) == c ) {
			return TRUE;
		}
		i += m;
	}

	return FALSE;
}

/*
 * 禁則 as the paragraph has it: the document's own letters when it gives
 * them (a kind whose K is 0 is none at all), else the viewer's sets.
 */
LOCAL BOOL head_no( CONST T_TVPARA *p, UW c )
{
	if ( p == NULL || p->khead_kind < 0 ) {
		return no_start(c);
	}

	return (BOOL)( ( p->khead_kind >> 4 ) != 0 && in_letters(p->khead, p->khead_len, c) );
}

LOCAL BOOL tail_no( CONST T_TVPARA *p, UW c )
{
	if ( p == NULL || p->ktail_kind < 0 ) {
		return no_end(c);
	}

	return (BOOL)( ( p->ktail_kind >> 4 ) != 0 && in_letters(p->ktail, p->ktail_len, c) );
}

/* ---------------------------------------------------------------- the walk */

/*
 * One thing laid on a line: a letter, a word of Latin letters kept
 * whole, a run of 結合文字 kept whole, a space, or something that is
 * not text. Each covers a stretch of the paragraph's places.
 */
typedef struct {
	INT	ri;			/* its run */
	INT	off, len;		/* for text, its bytes in the run */
	INT	fpos, units;		/* the places it covers */
	INT	w;			/* how wide */
	INT	x;			/* where it is on its line, from the left */
	INT	asc, desc;		/* above and below the base line */
	INT	shift;			/* raised (below nought) or lowered */
	UINT	flags;
} ITEM;

#define IT_BREAK_OK	0x01		/* a line may end after it */
#define IT_FORCE	0x02		/* a line does end after it */
#define IT_TAB		0x04		/* as wide as it takes to the next stop */
#define IT_FILL		0x08		/* as wide as what is left of the line */
#define IT_INDENT	0x10		/* later lines start where it is */
#define IT_SPACE	0x20		/* a space: stretched when lines are justified */

typedef struct {
	INT		gid;		/* below nought: measuring only */
	CONST T_TAD	*src;
	INT		x0, x1;		/* where the lines run between */
	INT		y;		/* the top of the text */
	INT		top, bottom;	/* what is worth drawing */
	INT		scroll;
	ID		fid;		/* the face now set */

	/*
	 * Asking rather than drawing: a place, and what was found there.
	 * It is the same walk either way, and it has to be -- a second
	 * piece of code that worked out where the links are would answer
	 * for a page that is not the one on the screen.
	 */
	BOOL		asking;
	INT		ask_x, ask_y;
	INT		found;		/* which link, or -1 */
	T_DPRECT	found_box;

	/*
	 * The caret. A place in the text is a paragraph and a count into
	 * it: the bytes of its text, and one for each thing standing in
	 * it that is not text -- a virtual object, a picture, a break.
	 * Every piece the walk puts down says which stretch of that count
	 * it covers, so that the same walk can answer where a place is
	 * and what place is under a point, and turn over a selection.
	 */
	UINT		mode;		/* LM_* */
	INT		best, best_para, best_pos;
	INT		want_para, want_pos;
	BOOL		got;
	T_DPRECT	box;
	INT		sa_para, sa_pos, sb_para, sb_pos;

	/* the face as it was last set, so as not to set it again */
	ID		set_fid;
	INT		set_px, set_xpx;
	UINT		set_style;

	/* what the document is to be shown with (T_TVDOC) */
	BOOL		detail;
	BOOL		show_hidden;
	INT		page_h;
	INT		mtop, mbottom;

	/*
	 * 段組: on pages, the columns of a page. The lines are laid down
	 * one column after another as though each were a page of its own;
	 * where one of those stands on the paper is worked out when it is
	 * put down.
	 */
	INT		cols, colstep;

	/*
	 * A thing standing in the text -- a link or a picture -- asked
	 * about: the one under a place, or where a given one is.
	 */
	CONST T_TVDOC	*doc;			/* what is being laid out */
	BOOL		thing_asking;
	INT		thing_x, thing_y;
	INT		thing_want;		/* its run, or -1 */
	INT		thing_run;		/* what was found, or -1 */
	T_DPRECT	thing_box;

	/* a page's number, for a header or footer drawn on it; else -1 */
	INT		pageno;
	INT		npages;			/* and how many there are; 0 not known */
	INT		full_x1;		/* the right margin, over all the columns */
	INT		last_pitch;		/* the height of the last line laid */

	/* 詳細: the mark of a setting under a place, asked about */
	BOOL		mark_asking;
	INT		mark_x, mark_y;
	T_TVMARK	mark;
} LAY;

#define LM_PLAIN	0		/* drawing, measuring, or finding a link */
#define LM_CARET_AT	1		/* the place under a point */
#define LM_CARET_BOX	2		/* where a place is */
#define LM_SELECT	3		/* a stretch turned over */

/* What is not normally seen shown: the marks of breaks, tabs, pages, notes */
LOCAL BOOL	dv_detail = FALSE;

EXPORT void tv_doc_detail( BOOL on )
{
	dv_detail = on;
}

/*
 * A document drawn magnified, in eighths: as a piece of text in a
 * figure is when the figure is. Every length of it grows alike.
 */
LOCAL INT	dv_zoom = 8;

EXPORT void tv_doc_zoom( INT z )
{
	dv_zoom = ( z > 0 ) ? z : 8;
}

#define DZ(v)	( ( dv_zoom == 8 ) ? (v) : (v) * dv_zoom / 8 )

LOCAL INT num_text( INT n, UB *out );
LOCAL INT page_no( CONST T_TVRUN *r, INT pg );
LOCAL INT var_text( CONST LAY *l, CONST T_TVRUN *r, UB *out );
LOCAL void overlays( LAY *l, CONST T_TVDOC *d, INT h );

/* 詳細: the flags and rulers settings are shown by */
#define FLAG_W		20		/* a flag in a line: its box and its point */
#define FLAG_H		14
#define RULER_H		24		/* the ruler above a paragraph with a format */
#define RULER_ROOM	32		/* the room it takes, with a space above and below */
#define RULER_UNIT	14		/* one letter of the standard size */

#define FLAG_GROUND	0x00FFFFD0U
#define FLAG_EDGE	0x00C0A040U
#define FLAG_INK	0x00806020U

/* ---------------------------------------------------------------- the letters' face */

/* How tall a run's letters are drawn */
LOCAL INT run_px( CONST T_TVRUN *r )
{
	INT	px = DZ(( r->size > 0 ) ? r->size : 14);

	if ( ( r->style & ( TV_ST_SUP | TV_ST_SUB ) ) != 0 ) {
		px = px * 6 / 10;
	}
	if ( r->hr_d > 0 && r->hr_n > 0 ) {
		px = px * r->hr_n / r->hr_d;
	}

	return ( px > 0 ) ? px : 1;
}

/* The face, size and look of a run set, when they are not set already */
LOCAL void run_font( LAY *l, CONST T_TVRUN *r )
{
	ID	fid = ( r->face != NULL ) ? fn_find(r->face) : fn_system();
	INT	px = run_px(r);
	INT	xpx = 0;
	UINT	st = 0;

	if ( r->wr_d > 0 && r->wr_n > 0 && r->wr_n != r->wr_d ) {
		xpx = px * r->wr_n / r->wr_d;
	}
	if ( ( r->style & TV_ST_BOLD ) != 0 )   st |= FN_ST_BOLD;
	if ( ( r->style & TV_ST_ITALIC ) != 0 ) st |= FN_ST_ITALIC;
	l->fid = fid;
	if ( fid <= 0 ) {
		return;
	}
	if ( fid == l->set_fid && px == l->set_px && xpx == l->set_xpx
	  && st == l->set_style ) {
		return;
	}
	fn_set_size(fid, px);
	if ( st != 0 || xpx != 0 ) {
		fn_set_style(fid, st, xpx);
	}
	l->set_fid = fid;
	l->set_px = px;
	l->set_xpx = xpx;
	l->set_style = st;
}

/* The extra room after each letter of a run, in pixels */
LOCAL INT run_space( CONST T_TVRUN *r )
{
	return ( r->space > 0 ) ? run_px(r) * r->space / 1000 : 0;
}

/*
 * How far a run's baseline is moved up (文字基準位置移動), in pixels:
 * below nought, down.
 */
LOCAL INT run_lift( CONST T_TVRUN *r )
{
	if ( r->lift_abs ) {
		return DZ(r->lift);
	}

	return DZ(( r->size > 0 ) ? r->size : 14) * r->lift / 1000;
}

/*
 * A run's letters turned (文字回転) while they are measured or drawn,
 * and upright again after: the face is shared, and whoever uses it next
 * sets only what it needs.
 */
LOCAL void turn_on( LAY *l, CONST T_TVRUN *r )
{
	if ( r->turn != 0 && l->fid > 0 ) {
		(void)fn_set_angle(l->fid, r->turn);
	}
}

LOCAL void turn_off( LAY *l, CONST T_TVRUN *r )
{
	if ( r->turn != 0 && l->fid > 0 ) {
		(void)fn_set_angle(l->fid, 0);
	}
}

/* How far n bytes of a turned run reach above and below the baseline */
LOCAL void turned_extent( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n,
			  INT *p_asc, INT *p_desc )
{
	UB	buf[16];
	INT	i;

	for ( i = 0; i < n && i < (INT)sizeof(buf) - 1; i++ ) {
		buf[i] = s[i];
	}
	buf[i] = 0;
	turn_on(l, r);
	if ( l->fid <= 0 || fn_extent(l->fid, buf, p_asc, p_desc) < E_OK ) {
		*p_asc = run_px(r);
		*p_desc = run_px(r) / 4;
	}
	turn_off(l, r);
}

/* The width of n bytes of text in the face now set, spacing and all */
LOCAL INT text_w( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n )
{
	UB	buf[64], *b = buf;
	INT	i, w, chars = 0;

	if ( n <= 0 || l->fid <= 0 ) {
		return 0;
	}
	if ( n >= (INT)sizeof(buf) ) {
		b = (UB *)Kmalloc((SZ)n + 1);
		if ( b == NULL ) {
			return 0;
		}
	}
	for ( i = 0; i < n; i++ ) {
		b[i] = s[i];
		if ( ( s[i] & 0xC0U ) != 0x80U ) {
			chars++;
		}
	}
	b[n] = 0;
	turn_on(l, r);
	w = fn_width(l->fid, b);
	turn_off(l, r);
	if ( b != buf ) {
		Kfree(b);
	}
	if ( w < 0 ) {
		w = 0;
	}

	return w + chars * run_space(r);
}

/* Text put down in the face now set: each letter, and the spacing after it */
LOCAL void text_put( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n, INT x,
		     INT base, UW col )
{
	UB	buf[64], *b = buf;
	INT	i, sp = run_space(r);

	if ( n <= 0 || l->fid <= 0 || l->gid < 0 ) {
		return;
	}
	turn_on(l, r);
	if ( sp == 0 ) {
		if ( n >= (INT)sizeof(buf) ) {
			b = (UB *)Kmalloc((SZ)n + 1);
			if ( b == NULL ) {
				turn_off(l, r);
				return;
			}
		}
		for ( i = 0; i < n; i++ ) {
			b[i] = s[i];
		}
		b[n] = 0;
		(void)fn_draw(l->gid, l->fid, x, base, b, col);
		if ( b != buf ) {
			Kfree(b);
		}
		turn_off(l, r);
		return;
	}
	/* letter by letter, the spacing after each */
	for ( i = 0; i < n; ) {
		INT	k = 1;

		while ( i + k < n && ( s[i + k] & 0xC0U ) == 0x80U ) {
			k++;
		}
		knl_memcpy(buf, s + i, (SZ)k);
		buf[k] = 0;
		x += fn_draw(l->gid, l->fid, x, base, buf, col) + sp;
		i += k;
	}
	turn_off(l, r);
}

/* ---------------------------------------------------------------- places */

/* The width of the first n bytes of a piece, spacing and all */
LOCAL INT prefix_w( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n )
{
	return text_w(l, r, s, n);
}

/*
 * One piece put down: 'units' of paragraph 'pi' from 'pos', at x and
 * 'w' wide, on the line whose top is 'top' and which is 'h' tall.
 * For text, 's' is its bytes and a unit is a byte.
 */
LOCAL void piece( LAY *l, CONST T_TVRUN *r, INT pi, INT pos, INT units,
		  CONST UB *s, INT x, INT w, INT top, INT h )
{
	switch ( l->mode ) {
	case LM_CARET_AT: {
		INT	dy = 0, dx = 0, at = pos, score;

		if ( l->ask_y < top ) {
			dy = top - l->ask_y;
		} else if ( l->ask_y >= top + h ) {
			dy = l->ask_y - ( top + h ) + 1;
		}
		if ( l->ask_x <= x ) {
			dx = x - l->ask_x;
		} else if ( l->ask_x >= x + w ) {
			dx = l->ask_x - ( x + w );
			at = pos + units;
		} else if ( s == NULL ) {
			at = ( l->ask_x < x + w / 2 ) ? pos : pos + units;
		} else {
			/* the boundary between two letters nearest the point */
			INT	k = 0, last_w = 0;

			while ( k < units ) {
				INT	nk = k + 1, cw;

				while ( nk < units && ( s[nk] & 0xC0U ) == 0x80U ) {
					nk++;
				}
				cw = prefix_w(l, r, s, nk);
				if ( x + cw > l->ask_x ) {
					at = ( l->ask_x - ( x + last_w )
					       < ( x + cw ) - l->ask_x )
					     ? pos + k : pos + nk;
					break;
				}
				last_w = cw;
				k = nk;
				at = pos + k;
			}
		}
		score = dy * 4096 + dx;
		if ( l->best < 0 || score < l->best ) {
			l->best = score;
			l->best_para = pi;
			l->best_pos = at;
		}
		break;
	}
	case LM_CARET_BOX:
		/*
		 * The last piece that holds the place: where a line is
		 * broken, the place between is the start of the next line.
		 */
		if ( pi == l->want_para && l->want_pos >= pos
		  && l->want_pos <= pos + units ) {
			INT	cx = x;

			if ( l->want_pos > pos ) {
				cx = ( s != NULL )
				     ? x + prefix_w(l, r, s, l->want_pos - pos)
				     : x + w;
			}
			l->box.left = cx;
			l->box.top = top;
			l->box.right = cx + 2;
			l->box.bottom = top + h;
			l->got = TRUE;
		}
		break;
	case LM_SELECT: {
		INT	a, b;

		if ( pi < l->sa_para || pi > l->sb_para || l->gid < 0 ) {
			break;
		}
		a = ( pi == l->sa_para ) ? l->sa_pos : 0;
		b = ( pi == l->sb_para ) ? l->sb_pos : 0x7FFFFFFF;
		if ( a < pos ) a = pos;
		if ( b > pos + units ) b = pos + units;
		if ( b > a && top + h >= l->top && top <= l->bottom ) {
			T_DPRECT	rc;

			rc.left = ( s != NULL ) ? x + prefix_w(l, r, s, a - pos) : x;
			rc.right = ( s != NULL ) ? x + prefix_w(l, r, s, b - pos)
						 : x + w;
			rc.top = top;
			rc.bottom = top + h;
			dp_set_mode(l->gid, DP_MODE_XOR);
			dp_fill_rect(l->gid, &rc, 0x00FFFFFFU);
			dp_set_mode(l->gid, DP_MODE_COPY);
		}
		break;
	}
	default:
		break;
	}
}

/* ---------------------------------------------------------------- things in the text */

/* A whole number an attribute gives, or nought */
LOCAL INT to_num( CONST UB *s )
{
	INT	v = 0, sign = 1;

	if ( s == NULL ) {
		return 0;
	}
	if ( *s == '-' ) {
		sign = -1;
		s++;
	}
	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + ( *s++ - '0' );
	}

	return v * sign;
}

/* A length in a document's units (-dpi, as <docScale> gives them) in pixels */
LOCAL INT units_px( INT unit, INT v )
{
	INT	dpi = ( unit < 0 ) ? -unit : 72;

	return ( dpi == 72 ) ? v : v * 72 / dpi;
}

/* How large a virtual object or a picture in the text is */
LOCAL void thing_size( LAY *l, CONST T_TVRUN *r, INT *p_w, INT *p_h )
{
	T_VOBJ	v;

	*p_w = LINK_W;
	*p_h = LINK_H;
	if ( r->kind == TV_RUN_FIGURE ) {
		/* a figure: the room its view takes, else all its shapes */
		CONST T_TVFIG	*f = ( l->doc != NULL && r->link >= 0 && r->link < l->doc->nfig )
				     ? l->doc->figs[r->link] : NULL;
		INT		w = 64, h = 64;

		if ( f != NULL ) {
			w = f->view.right - f->view.left;
			h = f->view.bottom - f->view.top;
			if ( w <= 0 || h <= 0 ) {
				(void)tv_fig_size(f, &w, &h);
			}
		}
		*p_w = DZ(( w > 0 ) ? w : 64);
		*p_h = DZ(( h > 0 ) ? h : 64);
		return;
	}
	if ( r->kind == TV_RUN_IMAGE ) {
		/* the size the record gives it, else a square of 64 */
		INT	w = 0, h = 0;

		if ( r->node != NULL ) {
			w = to_num(tad_attr(r->node, "right")) - to_num(tad_attr(r->node, "left"));
			h = to_num(tad_attr(r->node, "bottom")) - to_num(tad_attr(r->node, "top"));
		}
		*p_w = DZ(( w > 0 ) ? w : 64);
		*p_h = DZ(( h > 0 ) ? h : 64);
		return;
	}
	if ( l->src != NULL && tad_lnk_get(l->src, r->link, &v) >= E_OK ) {
		BOOL	sized = FALSE;

		if ( v.hidden && !l->show_hidden ) {
			*p_w = 0;		/* a hidden link takes no room */
			*p_h = 0;
			return;
		}
		/*
		 * The width and height a text gives it; or, from a record that
		 * gives only the rectangle a binary TAD had, that rectangle
		 * in the document's units.
		 */
		if ( v.width > 0 ) {
			*p_w = v.width;
			sized = TRUE;
		} else if ( v.right > v.left ) {
			*p_w = units_px(( l->doc != NULL ) ? l->doc->hunit : -72, v.right - v.left);
			sized = TRUE;
		}
		if ( v.heightpx > 0 ) {
			*p_h = v.heightpx;
			sized = TRUE;
		} else if ( v.bottom > v.top ) {
			*p_h = units_px(( l->doc != NULL ) ? l->doc->vunit : -72, v.bottom - v.top);
			sized = TRUE;
		}
		/* never shorter than the band its name is written in */
		if ( sized && *p_h < om_band_h(&v) + 2 * OM_FRAME_W ) {
			*p_h = om_band_h(&v) + 2 * OM_FRAME_W;
		}
	}
	*p_w = DZ(*p_w);
	*p_h = DZ(*p_h);
}

/* A virtual object standing in the text, drawn as a box with its name */
LOCAL void put_link( LAY *l, INT idx, INT x, INT y, INT w, INT h )
{
	T_VOBJ		v;
	T_DPRECT	box;
	BOOL		known = FALSE;

	if ( l->src != NULL && tad_lnk_get(l->src, idx, &v) >= E_OK ) {
		known = TRUE;
	}
	if ( w <= 0 || h <= 0 ) {
		return;			/* hidden */
	}
	box.left = x;  box.top = y;
	box.right = x + w;  box.bottom = y + h;

	if ( l->asking ) {
		if ( l->found < 0
		  && l->ask_x >= box.left && l->ask_x < box.right
		  && l->ask_y >= box.top && l->ask_y < box.bottom ) {
			l->found = idx;
			l->found_box = box;
		}
		return;
	}
	if ( l->gid < 0 || l->mode != LM_PLAIN ) {
		return;
	}
	if ( known ) {
		if ( v.chsz > 0 ) {
			v.chsz = DZ(v.chsz);	/* magnified with the text */
		}
		tv_draw_link(l->gid, &box, &v, tv_open_depth);
		l->set_fid = 0;		/* its name set the face to its own size */
	} else {
		dp_frame_rect(l->gid, &box, wm_look(WM_LOOK_FRAME), 1);
	}
}

/* ---------------------------------------------------------------- pages */

/*
 * On pages, where a line that tall may start at or after y: inside the
 * margins of the page y is on, or at the head of the next when it does
 * not fit what is left of this one. A line taller than a whole page
 * starts at the head of one and runs past its foot.
 */
LOCAL INT page_fit( CONST LAY *l, INT y, INT h )
{
	INT	top;

	if ( l->page_h <= 0 ) {
		return y;
	}
	top = ( y / l->page_h ) * l->page_h;
	if ( y < top + l->mtop ) {
		y = top + l->mtop;
	}
	if ( y + h > top + l->page_h - l->mbottom && y > top + l->mtop ) {
		y = top + l->page_h + l->mtop;
	}

	return y;
}

/*
 * On pages, the head of the next page, unless y is at the head of one.
 * With columns, a page is all of its columns.
 */
LOCAL INT page_next( CONST LAY *l, INT y )
{
	INT	top, whole;

	if ( l->page_h <= 0 ) {
		return y;
	}
	whole = l->page_h * ( ( l->cols > 1 ) ? l->cols : 1 );
	top = ( y / whole ) * whole;
	if ( y <= top + l->mtop ) {
		return top + l->mtop;
	}

	return top + whole + l->mtop;
}

/*
 * Where a place laid in columns is on the paper: how far down, and how
 * far across from the first column.
 */
LOCAL INT page_map( CONST LAY *l, INT y, INT *p_dx )
{
	INT	v;

	*p_dx = 0;
	if ( l->page_h <= 0 || l->cols <= 1 || y < 0 ) {
		return y;
	}
	v = y / l->page_h;
	*p_dx = ( v % l->cols ) * l->colstep;

	return ( v / l->cols ) * l->page_h + y % l->page_h;
}

/* ---------------------------------------------------------------- gathering */

/* Whether a byte is part of a word of Latin letters, kept on one line */
LOCAL BOOL wordish( UB c )
{
	return (BOOL)( c > 0x20 && c < 0x7F );
}

/*
 * The things of one paragraph, each measured: letters one by one,
 * a word of Latin letters as one, a run of 結合文字 as one, and each
 * thing that is not text as one. What may end a line is marked by the
 * rules of 禁則.
 */
LOCAL INT gather( LAY *l, CONST T_TVDOC *d, CONST T_TVPARA *p, ITEM *it, INT max )
{
	INT	ri, n = 0, fpos = 0, k;

	for ( ri = p->first; ri < p->first + p->n && ri < d->nrun && n < max; ri++ ) {
		CONST T_TVRUN	*r = &d->run[ri];
		T_FNMET		met;
		INT		asc, desc, px = run_px(r), met_asc, met_desc;

		run_font(l, r);
		asc = px;
		desc = px / 4;
		if ( l->fid > 0 && fn_metrics(l->fid, &met) >= E_OK ) {
			asc = met.ascent;
			desc = met.descent;
		}
		met_asc = asc;
		met_desc = desc;
		/* room over or under for ruby and 傍点 */
		if ( r->ruby != NULL ) {
			if ( r->ruby_below ) {
				desc += ( px / 2 > 6 ? px / 2 : 6 ) + 1;
			} else {
				asc += ( px / 2 > 6 ? px / 2 : 6 ) + 1;
			}
		}
		if ( r->bouten != 0 ) {
			if ( ( r->bouten & 3 ) == 2 ) desc += px / 3;
			else asc += px / 3;
		}
		switch ( r->kind ) {
		case TV_RUN_TEXT: {
			INT	i = 0, shift = 0;
			INT	up_extra = asc - met_asc, down_extra = desc - met_desc;

			if ( ( r->style & TV_ST_SUP ) != 0 ) {
				shift = -( DZ(r->size) * 4 / 10 );
			} else if ( ( r->style & TV_ST_SUB ) != 0 ) {
				shift = DZ(r->size) * 2 / 10;
			}
			shift -= run_lift(r);
			while ( i < r->len && n < max ) {
				ITEM	*t = &it[n];
				INT	j = i + 1;

				if ( ( r->style & TV_ST_COMB ) != 0 ) {
					j = r->len;
				} else if ( r->turn != 0 ) {
					/* turned letters stand each in its own box */
					while ( j < r->len && ( r->text[j] & 0xC0U ) == 0x80U ) {
						j++;
					}
				} else if ( wordish(r->text[i]) ) {
					while ( j < r->len && wordish(r->text[j]) ) {
						j++;
					}
				} else {
					while ( j < r->len && ( r->text[j] & 0xC0U ) == 0x80U ) {
						j++;
					}
				}
				t->ri = ri;
				t->off = i;
				t->len = j - i;
				t->fpos = fpos + i;
				t->units = j - i;
				t->w = text_w(l, r, r->text + i, j - i);
				t->asc = asc;
				t->desc = desc;
				if ( r->turn != 0 ) {
					/* the letter's box as it is turned, and what
					   stands over and under it as before */
					turned_extent(l, r, r->text + i, j - i, &t->asc, &t->desc);
					t->asc += up_extra;
					t->desc += down_extra;
				}
				t->asc -= ( shift < 0 ) ? shift : 0;
				t->desc += ( shift > 0 ) ? shift : 0;
				t->shift = shift;
				t->flags = IT_BREAK_OK;
				if ( j - i == 1 && ( r->text[i] == ' ' ) ) {
					t->flags |= IT_SPACE;
				}
				n++;
				i = j;
			}
			fpos += r->len;
			break;
		}
		case TV_RUN_LINK:
		case TV_RUN_IMAGE:
		case TV_RUN_FIGURE: {
			INT	w, h;

			thing_size(l, r, &w, &h);
			it[n].ri = ri;  it[n].off = 0;  it[n].len = 0;
			it[n].fpos = fpos;  it[n].units = 1;
			it[n].w = w + 2;
			it[n].asc = h;  it[n].desc = 0;  it[n].shift = 0;
			it[n].flags = IT_BREAK_OK;
			n++;
			fpos++;
			break;
		}
		default:
			it[n].ri = ri;  it[n].off = 0;  it[n].len = 0;
			it[n].fpos = fpos;  it[n].units = 1;
			it[n].w = 0;
			it[n].asc = asc;  it[n].desc = desc;  it[n].shift = 0;
			it[n].flags = IT_BREAK_OK;
			switch ( r->kind ) {
			case TV_RUN_BREAK:
			case TV_RUN_PAGE:	it[n].flags |= IT_FORCE;	break;
			case TV_RUN_TAB:	it[n].flags |= IT_TAB;		break;
			case TV_RUN_INDENT:	it[n].flags = IT_INDENT;	break;
			case TV_RUN_FILL:	it[n].flags |= IT_FILL;		break;
			case TV_RUN_FIXSP:
				it[n].w = r->width;
				if ( l->detail && it[n].w < FLAG_W ) {
					it[n].w = FLAG_W;	/* its flag */
				}
				break;
			case TV_RUN_VAR: {
				UB	buf[64];

				it[n].w = text_w(l, r, buf, var_text(l, r, buf));
				if ( l->detail ) {
					it[n].w = FLAG_W;
				}
				break;
			}
			case TV_RUN_PAGENO:
			case TV_RUN_MEMO:
				it[n].w = text_w(l, r, r->text, r->len);
				if ( l->detail ) {
					it[n].w = FLAG_W;	/* its flag, not its words */
				} else if ( r->kind == TV_RUN_MEMO ) {
					it[n].w = 0;	/* a note is seen only in detail */
				}
				break;
			default:
				break;
			}
			n++;
			fpos++;
			break;
		}
	}
	/* 禁則: no line ends before a letter that may not begin one, or after
	   a letter that may not end one */
	for ( k = 0; k + 1 < n; k++ ) {
		CONST T_TVRUN	*ra = &d->run[it[k].ri], *rb = &d->run[it[k + 1].ri];

		if ( rb->kind == TV_RUN_TEXT && it[k + 1].len > 0
		  && head_no(p, utf8_at(rb->text + it[k + 1].off, NULL)) ) {
			it[k].flags &= ~IT_BREAK_OK;
		}
		if ( ra->kind == TV_RUN_TEXT && it[k].len > 0 ) {
			INT	e = it[k].off + it[k].len - 1;

			while ( e > it[k].off && ( ra->text[e] & 0xC0U ) == 0x80U ) {
				e--;
			}
			if ( tail_no(p, utf8_at(ra->text + e, NULL)) ) {
				it[k].flags &= ~IT_BREAK_OK;
			}
		}
	}

	return n;
}

/* ---------------------------------------------------------------- one paragraph */

/* The next tab stop after x, the stops counted from the line's left edge */
LOCAL INT tab_to( CONST T_TVPARA *p, INT base, INT x, INT size )
{
	INT	i, step = DZ( size > 0 ? size : 14 ) * 4;

	for ( i = 0; i < p->ntabs; i++ ) {
		if ( base + DZ(p->tabs[i]) > x ) {
			return base + DZ(p->tabs[i]);
		}
	}

	return base + ( ( x - base ) / step + 1 ) * step;
}

/* Draw one text item, with every decoration its run carries */
LOCAL void draw_text_item( LAY *l, CONST T_TVRUN *r, CONST ITEM *t, INT x,
			   INT base, INT top, INT h )
{
	CONST UB	*s = r->text + t->off;
	UW		col = r->colour;
	INT		px = run_px(r), b = base + t->shift;
	T_DPRECT	rc;

	rc.left = x;
	rc.right = x + t->w;
	rc.top = top;
	rc.bottom = top + h;
	if ( r->back != TAD_COL_NONE ) {
		dp_fill_rect(l->gid, &rc, r->back);
	}
	if ( ( r->style & TV_ST_MESH ) != 0 ) {
		/* 網掛: every other pixel of the ground in grey */
		INT	yy, xx;

		for ( yy = rc.top; yy < rc.bottom; yy++ ) {
			for ( xx = rc.left + ( yy & 1 ); xx < rc.right; xx += 2 ) {
				dp_put_pixel(l->gid, xx, yy, 0x00A0A0A0U);
			}
		}
	}
	if ( ( r->style & TV_ST_INVERT ) != 0 ) {
		dp_fill_rect(l->gid, &rc, col);
		col = ( col == 0x00FFFFFFU ) ? 0x00000000U : 0x00FFFFFFU;
	}
	if ( ( r->style & TV_ST_SHADOW ) != 0 ) {
		text_put(l, r, s, t->len, x + 2, b + 2, 0x00909090U);
	}
	if ( ( r->style & TV_ST_BAG ) != 0 ) {
		/* 袋文字: the letters' outline, and their inside in white */
		INT	dx, dy;

		for ( dy = -1; dy <= 1; dy++ ) {
			for ( dx = -1; dx <= 1; dx++ ) {
				if ( dx != 0 || dy != 0 ) {
					text_put(l, r, s, t->len, x + dx, b + dy, col);
				}
			}
		}
		text_put(l, r, s, t->len, x, b, 0x00FFFFFFU);
	} else {
		text_put(l, r, s, t->len, x, b, col);
	}
	if ( ( r->style & TV_ST_UNDER ) != 0 ) {
		dp_line(l->gid, x, b + 2, x + t->w - 1, b + 2, col);
	}
	if ( ( r->style & TV_ST_OVER ) != 0 ) {
		dp_line(l->gid, x, b - px, x + t->w - 1, b - px, col);
	}
	if ( ( r->style & TV_ST_STRIKE ) != 0 ) {
		dp_line(l->gid, x, b - px / 3, x + t->w - 1, b - px / 3, col);
	}
	if ( ( r->style & TV_ST_BOX ) != 0 ) {
		dp_line(l->gid, x, b - px - 1, x + t->w, b - px - 1, col);
		dp_line(l->gid, x, b + px / 4 + 1, x + t->w, b + px / 4 + 1, col);
	}
	if ( r->bouten != 0 ) {
		/* 傍点: a dot, or a sesame, over or under each letter */
		INT	i = 0, cx = x;

		while ( i < t->len ) {
			INT	k = 1, cw, dy;

			while ( i + k < t->len && ( s[i + k] & 0xC0U ) == 0x80U ) k++;
			cw = text_w(l, r, s + i, k);
			dy = ( ( r->bouten & 3 ) == 2 ) ? b + px / 4 + 3 : b - px - 3;
			if ( ( r->bouten >> 4 ) == 1 ) {
				dp_line(l->gid, cx + cw / 2 - 1, dy - 1, cx + cw / 2 + 1,
					dy + 1, col);
			} else {
				T_DPRECT	d;

				d.left = cx + cw / 2 - 1;  d.right = d.left + 3;
				d.top = dy - 1;  d.bottom = dy + 2;
				dp_fill_rect(l->gid, &d, col);
			}
			cx += cw;
			i += k;
		}
	}
}

/* The left and right ends of a run's box, where its frame closes */
LOCAL void box_ends( LAY *l, CONST T_TVDOC *d, CONST ITEM *it, INT a, INT e,
		     INT shift_x, INT base )
{
	INT	k;

	for ( k = a; k < e; k++ ) {
		CONST T_TVRUN	*r = &d->run[it[k].ri];
		INT		px = run_px(r);

		if ( ( r->style & TV_ST_BOX ) == 0 ) {
			continue;
		}
		if ( k == a || it[k - 1].ri != it[k].ri ) {
			dp_line(l->gid, shift_x + it[k].x, base - px - 1,
				shift_x + it[k].x, base + px / 4 + 1, r->colour);
		}
		if ( k + 1 == e || it[k + 1].ri != it[k].ri ) {
			INT	xr = shift_x + it[k].x + it[k].w;

			dp_line(l->gid, xr, base - px - 1, xr, base + px / 4 + 1,
				r->colour);
		}
	}
}

/* The ruby of each group on a line, over (or under) what it belongs to */
LOCAL void draw_ruby( LAY *l, CONST T_TVDOC *d, CONST ITEM *it, INT a, INT e,
		      INT shift_x, INT base )
{
	INT	k = a;

	while ( k < e ) {
		CONST T_TVRUN	*r = &d->run[it[k].ri];
		INT		g = r->ruby_group, j = k, x0, x1, px, rw, save_px;
		ID		fid;
		UB		buf[128];
		INT		m;
		T_FNMET		met;

		if ( r->ruby == NULL || g < 0 ) {
			k++;
			continue;
		}
		while ( j < e && d->run[it[j].ri].ruby_group == g ) {
			j++;
		}
		x0 = shift_x + it[k].x;
		x1 = shift_x + it[j - 1].x + it[j - 1].w;
		px = run_px(r) / 2;
		if ( px < 6 ) px = 6;
		fid = l->fid;
		save_px = l->set_px;
		if ( fid > 0 ) {
			fn_set_size(fid, px);
			l->set_px = -1;			/* the face is not what it was */
			for ( m = 0; m < r->ruby_len && m < (INT)sizeof(buf) - 1; m++ ) {
				buf[m] = r->ruby[m];
			}
			buf[m] = 0;
			rw = fn_width(fid, buf);
			(void)fn_metrics(fid, &met);
			(void)fn_draw(l->gid, fid, x0 + ( ( x1 - x0 ) - rw ) / 2,
				      r->ruby_below
				      ? base + run_px(r) / 4 + met.ascent + 1
				      : base - run_px(r) - met.descent - 1,
				      buf, r->colour);
		}
		(void)save_px;
		k = j;
	}
}

/*
 * One paragraph laid from 'y' down: the things gathered, broken into
 * lines, each line measured and put where its alignment says, and then
 * drawn or asked about. What comes back is where the paragraph ends.
 */
/* ---------------------------------------------------------------- 詳細: the marks of settings */

/* A mark noted, when it is under the place asked about */
LOCAL void mark_found( LAY *l, INT kind, INT pi, INT run, INT index,
		       T_TADNODE *node, CONST T_DPRECT *b )
{
	if ( !l->mark_asking || l->mark.kind != TV_MARK_NONE
	  || l->mark_x < b->left || l->mark_x >= b->right
	  || l->mark_y < b->top || l->mark_y >= b->bottom ) {
		return;
	}
	l->mark.kind = kind;
	l->mark.para = pi;
	l->mark.run = run;
	l->mark.index = index;
	l->mark.node = node;
	l->mark.box = *b;
}

/* Small letters of a mark, in the face of the system */
LOCAL INT mark_text( LAY *l, INT x, INT base, CONST char *s, INT px, UW col,
		     BOOL centred )
{
	ID	fid = fn_system();
	INT	w;

	if ( fid <= 0 ) {
		return 0;
	}
	(void)fn_set_size(fid, px);
	(void)fn_set_style(fid, FN_ST_BOLD, 0);
	w = fn_width(fid, (CONST UB *)s);
	if ( l->gid >= 0 ) {
		(void)fn_draw(l->gid, fid, centred ? x - w / 2 : x, base,
			      (CONST UB *)s, col);
	}
	l->set_fid = 0;				/* the letters set theirs again */

	return w;
}

/* A small filled triangle, row by row: its point at x, y */
LOCAL void point_fill( INT gid, INT x, INT y, INT dir, INT size, UW col )
{
	INT	i;

	for ( i = 0; i <= size; i++ ) {
		switch ( dir ) {
		case 0:		/* pointing right */
			dp_line(gid, x - i, y - ( size - i ), x - i, y + ( size - i ), col);
			break;
		default:	/* pointing down */
			dp_line(gid, x - ( size - i ), y - size + i, x + ( size - i ),
				y - size + i, col);
			break;
		}
	}
}

/* A flag in a line: a box with its letter and a point to the right */
LOCAL void flag_draw( LAY *l, INT x, INT w, INT cy, CONST char *sym )
{
	T_DPRECT	b;

	b.left = x + 1;
	b.right = x + w - 6;
	b.top = cy - FLAG_H / 2;
	b.bottom = b.top + FLAG_H;
	if ( l->gid < 0 || l->mode != LM_PLAIN
	  || b.bottom < l->top || b.top > l->bottom ) {
		return;
	}
	dp_fill_rect(l->gid, &b, FLAG_GROUND);
	dp_frame_rect(l->gid, &b, FLAG_EDGE, 1);
	point_fill(l->gid, b.right + 4, cy, 0, 4, FLAG_EDGE);
	(void)mark_text(l, ( b.left + b.right ) / 2, b.bottom - 3, sym, 10,
			FLAG_INK, TRUE);
}

/* The letter an alignment is flagged with */
LOCAL CONST char *align_sym( UINT align )
{
	return ( align == TV_ALIGN_CENTRE ) ? "C"
	     : ( align == TV_ALIGN_RIGHT ) ? "R"
	     : ( align == TV_ALIGN_JUSTIFY ) ? "J" : "L";
}

/* One marker of the ruler: a coloured tab with its sign drawn in white */
LOCAL void marker_draw( INT gid, INT kind, INT x, INT top )
{
	T_DPRECT	b;
	UW		ground = ( kind == TV_MARK_FHEAD ) ? 0x00406090U
			       : ( kind == TV_MARK_LHEAD ) ? 0x00408060U
			       : ( kind == TV_MARK_LEND ) ? 0x00804060U : 0x00804020U;
	UW		ink = 0x00FFFFFFU;

	b.left = x - 8;
	b.right = x + 8;
	b.top = top + 1;
	b.bottom = top + 21;
	dp_fill_rect(gid, &b, ground);
	dp_frame_rect(gid, &b, 0x00602010U, 1);
	switch ( kind ) {
	case TV_MARK_FHEAD:	/* a pole, a bar over it and a point to the right */
		dp_line(gid, x - 4, b.top + 3, x - 4, b.bottom - 3, ink);
		dp_line(gid, x - 4, b.top + 3, x + 4, b.top + 3, ink);
		point_fill(gid, x + 4, b.top + 10, 0, 4, ink);
		break;
	case TV_MARK_LHEAD:	/* a pole with its flag to the right */
		dp_line(gid, x - 4, b.top + 3, x - 4, b.bottom - 3, ink);
		{
			INT	i;

			for ( i = 0; i < 7; i++ ) {
				dp_line(gid, x - 4, b.top + 3 + i, x + 4 - i, b.top + 3 + i, ink);
			}
		}
		break;
	case TV_MARK_LEND:	/* a pole with its flag to the left */
		dp_line(gid, x + 4, b.top + 3, x + 4, b.bottom - 3, ink);
		{
			INT	i;

			for ( i = 0; i < 7; i++ ) {
				dp_line(gid, x - 4 + i, b.top + 3 + i, x + 4, b.top + 3 + i, ink);
			}
		}
		break;
	default:		/* a tab stop: pointing down at its place */
		point_fill(gid, x, b.bottom - 4, 1, 5, ink);
		break;
	}
}

/*
 * The ruler of a tab format given at a paragraph: the paper's width
 * across, a letter to each step of its scale counted from the left
 * margin, and on it the left margin, the first line, the right margin
 * and the tab stops. Drawn, or asked which of them is under a place.
 */
LOCAL void ruler( LAY *l, CONST T_TVDOC *d, INT pi, INT dx, INT top )
{
	CONST T_TVPARA	*p = &d->para[pi];
	INT		x0 = l->x0 + dx, x1 = l->x1 + dx, u = DZ(RULER_UNIT);
	INT		lx = x0 + DZ(p->left), fx = lx + DZ(p->indent);
	INT		jx = x1 - DZ(p->right), i, x;
	INT		jm = ( jx > x1 - 9 ) ? x1 - 9 : jx;	/* its marker, kept on the ruler */
	CONST UB	*rv = tad_attr(p->fmt, "R");
	BOOL		rel = (BOOL)( rv != NULL && rv[0] == '1' );
	T_DPRECT	bar, b;

	bar.left = x0;
	bar.top = top;
	bar.right = x1;
	bar.bottom = top + RULER_H;
	if ( l->mark_asking ) {
		if ( l->mark.kind != TV_MARK_NONE ) {
			return;			/* found already, above */
		}
		/* the origin, the heads over the tab stops, and then the bar */
		b.left = x0 + 2;  b.top = top + 4;  b.right = b.left + 12;  b.bottom = b.top + 12;
		mark_found(l, TV_MARK_ORIGIN, pi, -1, 0, p->fmt, &b);
		b.top = top + 1;  b.bottom = top + 21;
		b.left = fx - 8;  b.right = fx + 8;
		mark_found(l, TV_MARK_FHEAD, pi, -1, 0, p->fmt, &b);
		b.left = lx - 8;  b.right = lx + 8;
		mark_found(l, TV_MARK_LHEAD, pi, -1, 0, p->fmt, &b);
		b.left = jm - 8;  b.right = jm + 8;
		mark_found(l, TV_MARK_LEND, pi, -1, 0, p->fmt, &b);
		for ( i = 0; i < p->ntabs; i++ ) {
			x = lx + DZ(p->tabs[i]);
			b.left = x - 8;  b.right = x + 8;
			mark_found(l, TV_MARK_TAB, pi, -1, i, p->fmt, &b);
		}
		mark_found(l, TV_MARK_BAR, pi, -1, 0, p->fmt, &bar);
		if ( l->mark.kind != TV_MARK_NONE ) {
			l->mark.zero = x0;
			l->mark.head = lx;
			l->mark.end = x1;
			l->mark.unit = u;
			l->mark.at = ( l->mark.kind == TV_MARK_FHEAD ) ? fx
				   : ( l->mark.kind == TV_MARK_LHEAD ) ? lx
				   : ( l->mark.kind == TV_MARK_LEND ) ? jx
				   : ( l->mark.kind == TV_MARK_TAB )
				     ? lx + DZ(p->tabs[l->mark.index]) : 0;
		}
		return;
	}
	if ( l->gid < 0 || l->mode != LM_PLAIN
	  || bar.bottom < l->top || bar.top > l->bottom ) {
		return;
	}
	dp_fill_rect(l->gid, &bar, 0x00FFE8B8U);
	dp_frame_rect(l->gid, &bar, 0x00A07020U, 1);
	for ( i = 1, x = lx + u; u > 0 && x < x1; i++, x += u ) {
		BOOL	five = (BOOL)( i % 5 == 0 );

		dp_line(l->gid, x, bar.bottom - 1 - ( five ? 10 : 6 ), x, bar.bottom - 2,
			five ? 0x00A07040U : 0x00C09060U);
		if ( five ) {
			UB	num[8];
			INT	v = i, k = 0, j;
			UB	t;

			do { num[k++] = (UB)( '0' + v % 10 ); v /= 10; } while ( v > 0 && k < 6 );
			num[k] = 0;
			for ( j = 0; j < k / 2; j++ ) {
				t = num[j];  num[j] = num[k - 1 - j];  num[k - 1 - j] = t;
			}
			(void)mark_text(l, x, top + 9, (CONST char *)num, 8,
					0x00604020U, TRUE);
		}
	}
	for ( i = 0; i < p->ntabs; i++ ) {
		marker_draw(l->gid, TV_MARK_TAB, lx + DZ(p->tabs[i]), top);
	}
	marker_draw(l->gid, TV_MARK_LEND, jm, top);
	marker_draw(l->gid, TV_MARK_LHEAD, lx, top);
	marker_draw(l->gid, TV_MARK_FHEAD, fx, top);
	/* where the tab stops count from: the paper, or the line before */
	b.left = x0 + 2;  b.top = top + 4;  b.right = b.left + 12;  b.bottom = b.top + 12;
	dp_fill_rect(l->gid, &b, 0x00FFE8B8U);
	dp_frame_rect(l->gid, &b, 0x00806020U, 1);
	(void)mark_text(l, b.left + 6, b.bottom - 2, rel ? ">" : "|", 10,
			0x00604010U, TRUE);
}

LOCAL INT lay_para( LAY *l, CONST T_TVDOC *d, INT pi, INT y )
{
	CONST T_TVPARA	*p = &d->para[pi];
	ITEM		*it;
	INT		max, n, a, first_line = 1;
	INT		left = l->x0 + DZ(p->left), limit = l->x1 - DZ(p->right);
	INT		indent_x = -1, para_size = 14;

	max = 2;
	{
		INT	ri;

		for ( ri = p->first; ri < p->first + p->n && ri < d->nrun; ri++ ) {
			max += ( d->run[ri].kind == TV_RUN_TEXT ) ? d->run[ri].len + 1 : 1;
			if ( ri == p->first ) {
				para_size = DZ(d->run[ri].size);
			}
		}
	}
	it = (ITEM *)Kmalloc(sizeof(ITEM) * (SZ)max);
	if ( it == NULL ) {
		return y;
	}
	n = gather(l, d, p, it, max);
	if ( p->page_before ) {
		y = page_next(l, y);
	}
	if ( p->page_before && l->detail && l->gid >= 0 && l->mode == LM_PLAIN ) {
		INT	yy = l->y + y - l->scroll, xx;

		for ( xx = l->x0; xx < l->x1; xx += 6 ) {
			dp_line(l->gid, xx, yy, xx + 2, yy, 0x00808080U);
		}
	}
	/* 詳細: a tab format given here is shown as a ruler above it */
	if ( l->detail && p->fmt != NULL ) {
		INT	rtop, rdx;

		y = page_fit(l, y, RULER_ROOM);
		rtop = l->y + page_map(l, y, &rdx) - l->scroll;
		ruler(l, d, pi, rdx, rtop + ( RULER_ROOM - RULER_H ) / 2);
		y += RULER_ROOM;
	}

	/* a paragraph with nothing in it is still a line */
	if ( n == 0 ) {
		INT	h = para_size + para_size / 2, top;

		y = page_fit(l, y, h);
		{
			INT	dx;

			top = l->y + page_map(l, y, &dx) - l->scroll;
			left += dx;
		}

		piece(l, NULL, pi, 0, 0, NULL, left + DZ(p->indent), 0, top, h);
		Kfree(it);
		y += h;
		y += p->pargap_abs ? p->pargap : para_size * p->pargap / 1000;
		return y;
	}

	a = 0;
	while ( a < n ) {
		INT	start = ( first_line ) ? left + DZ(p->indent)
				: ( indent_x >= 0 ) ? indent_x : left;
		INT	x = start, e = a, k, asc = 0, desc = 0, big = 0, pitch;
		INT	top, base, width, extra, shift_x = 0, spaces = 0, col_dx = 0;
		BOOL	forced = FALSE;

		/* how many things go on this line */
		for ( k = a; k < n; k++ ) {
			CONST T_TVRUN	*r = &d->run[it[k].ri];
			INT		w = it[k].w;

			if ( ( it[k].flags & IT_TAB ) != 0 ) {
				w = tab_to(p, left, x, r->size) - x;
			}
			if ( k > a && x + w > limit && w > 0 ) {
				INT	j = k - 1;

				while ( j >= a && ( it[j].flags & IT_BREAK_OK ) == 0 ) {
					j--;
				}
				e = ( j >= a ) ? j + 1 : k;
				break;
			}
			it[k].x = x;
			it[k].w = w;
			if ( ( it[k].flags & IT_INDENT ) != 0 ) {
				indent_x = x;
			}
			x += w;
			e = k + 1;
			if ( ( it[k].flags & IT_FORCE ) != 0 ) {
				forced = TRUE;
				break;
			}
		}
		if ( e <= a ) {
			e = a + 1;
		}
		/* the items of the line again, now that where it ends is known */
		x = start;
		for ( k = a; k < e; k++ ) {
			CONST T_TVRUN	*r = &d->run[it[k].ri];

			if ( ( it[k].flags & IT_TAB ) != 0 ) {
				it[k].w = tab_to(p, left, x, r->size) - x;
			}
			it[k].x = x;
			x += it[k].w;
			if ( it[k].asc > asc )   asc = it[k].asc;
			if ( it[k].desc > desc ) desc = it[k].desc;
			if ( r->kind == TV_RUN_TEXT || r->kind == TV_RUN_TAB ) {
				INT	sz = DZ(r->size);

				if ( r->hr_d > 0 && r->hr_n > 0 ) sz = sz * r->hr_n / r->hr_d;
				if ( sz > big ) big = sz;
			} else if ( r->kind == TV_RUN_LINK || r->kind == TV_RUN_IMAGE
				 || r->kind == TV_RUN_FIGURE ) {
				if ( it[k].asc > big ) big = it[k].asc;
			}
			if ( ( it[k].flags & IT_SPACE ) != 0 ) {
				spaces++;
			}
		}
		/* a fill takes what is left of the line */
		for ( k = a; k < e; k++ ) {
			if ( ( it[k].flags & IT_FILL ) != 0 && x < limit ) {
				INT	j;

				it[k].w = limit - x;
				for ( j = k + 1; j < e; j++ ) {
					it[j].x += it[k].w;
				}
				x = limit;
				break;
			}
		}
		width = x - start;
		if ( big <= 0 ) big = para_size;
		pitch = p->gap_abs ? p->gap : big * ( 1000 + p->gap ) / 1000;
		if ( pitch < asc + desc ) {
			pitch = asc + desc;
		}
		y = page_fit(l, y, pitch);
		{
			INT	dx;

			top = l->y + page_map(l, y, &dx) - l->scroll;
			col_dx = dx;
		}
		base = top + ( pitch - ( asc + desc ) ) / 2 + asc;
		l->last_pitch = pitch;

		/* alignment */
		extra = limit - start - width;
		if ( extra > 0 ) {
			if ( p->align == TV_ALIGN_CENTRE ) {
				shift_x = extra / 2;
			} else if ( p->align == TV_ALIGN_RIGHT ) {
				shift_x = extra;
			} else if ( p->align == TV_ALIGN_JUSTIFY && !forced && e < n
				 && e - a > 1 ) {
				/* the room shared out between the things of the line */
				INT	gaps = ( spaces > 0 ) ? spaces : e - a - 1, g = 0;

				for ( k = a; k < e; k++ ) {
					it[k].x += extra * g / gaps;
					if ( spaces == 0 || ( it[k].flags & IT_SPACE ) != 0 ) {
						g++;
					}
				}
			}
		}

		shift_x += col_dx;

		/* 詳細: an alignment given here is flagged at the right */
		if ( l->detail && first_line && p->align_at != NULL ) {
			T_DPRECT	fb;

			fb.left = limit + col_dx - 4 - FLAG_W;
			fb.right = fb.left + FLAG_W;
			fb.top = top + 2;
			fb.bottom = fb.top + FLAG_H;
			mark_found(l, TV_MARK_ALIGN, pi, -1, 0, p->align_at, &fb);
			flag_draw(l, fb.left, FLAG_W, fb.top + FLAG_H / 2,
				  align_sym(p->align));
		}

		/* drawn, or asked about */
		for ( k = a; k < e; k++ ) {
			CONST T_TVRUN	*r = &d->run[it[k].ri];
			INT		ix = shift_x + it[k].x;
			CONST UB	*s = ( r->kind == TV_RUN_TEXT )
					     ? r->text + it[k].off : NULL;

			run_font(l, r);
			switch ( r->kind ) {
			case TV_RUN_LINK:
			case TV_RUN_IMAGE:
			case TV_RUN_FIGURE:
				if ( l->thing_asking ) {
					T_DPRECT	tb;

					tb.left = ix;
					tb.top = base - it[k].asc;
					tb.right = ix + it[k].w - 2;
					tb.bottom = base;
					if ( ( l->thing_want < 0
					       && l->thing_x >= tb.left && l->thing_x < tb.right
					       && l->thing_y >= tb.top && l->thing_y < tb.bottom )
					  || l->thing_want == it[k].ri ) {
						l->thing_run = it[k].ri;
						l->thing_box = tb;
					}
					break;
				}
				if ( r->kind == TV_RUN_FIGURE ) {
					if ( l->gid >= 0 && l->mode == LM_PLAIN
					  && top + pitch >= l->top && top <= l->bottom
					  && r->link >= 0 && r->link < d->nfig ) {
						T_TVFIG		*f = d->figs[r->link];
						T_DPRECT	box;
						INT		keep = f->zoom;

						box.left = ix;
						box.top = base - it[k].asc;
						box.right = ix + it[k].w - 2;
						box.bottom = base;
						/* its view, magnified as the text is */
						f->zoom = dv_zoom;
						(void)tv_fig_draw_at(l->gid, f, &box,
								     DZ(f->view.left), DZ(f->view.top),
								     l->src, TAD_COL_NONE, 1);
						f->zoom = keep;
						/* its letters set the faces afresh */
						l->set_fid = 0;
					}
				} else if ( r->kind == TV_RUN_LINK ) {
					put_link(l, r->link, ix, base - it[k].asc,
						 it[k].w - 2, it[k].asc);
				} else if ( l->gid >= 0 && l->mode == LM_PLAIN
					 && top + pitch >= l->top && top <= l->bottom ) {
					T_DPRECT	box;

					box.left = ix;
					box.top = base - it[k].asc;
					box.right = ix + it[k].w - 2;
					box.bottom = base;
					tv_draw_picture(l->gid, &box, r->href);
				}
				break;
			case TV_RUN_TEXT:
				if ( l->gid >= 0 && l->mode == LM_PLAIN
				  && top + pitch >= l->top - 64 && top <= l->bottom + 64 ) {
					draw_text_item(l, r, &it[k], ix, base, top, pitch);
					if ( l->detail && r->space > 0 ) {
						/* 詳細: the ends of a stretch spaced apart */
						INT	ri = it[k].ri, hh = DZ(r->size);

						if ( it[k].off == 0 && ( ri == p->first
							|| d->run[ri - 1].space != r->space ) ) {
							dp_line(l->gid, ix, base - hh, ix, base + 2,
								FLAG_EDGE);
						}
						if ( it[k].off + it[k].len == r->len
						  && ( ri + 1 >= p->first + p->n
						       || d->run[ri + 1].space != r->space ) ) {
							dp_line(l->gid, ix + it[k].w - 1, base - hh,
								ix + it[k].w - 1, base + 2, FLAG_EDGE);
						}
					}
				}
				break;
			case TV_RUN_FILL:
				if ( l->gid >= 0 && l->mode == LM_PLAIN && r->len > 0 ) {
					INT	cw = text_w(l, r, r->text, r->len), xx;

					for ( xx = ix; cw > 0 && xx + cw <= ix + it[k].w; xx += cw ) {
						text_put(l, r, r->text, r->len, xx, base,
							 r->colour);
					}
				}
				break;
			case TV_RUN_VAR:
				if ( l->detail ) {
					T_DPRECT	fb;

					fb.left = ix;
					fb.right = ix + it[k].w;
					fb.top = base - 5 - FLAG_H / 2;
					fb.bottom = fb.top + FLAG_H;
					mark_found(l, TV_MARK_RUN, pi, it[k].ri, 0, r->node, &fb);
					flag_draw(l, ix, it[k].w, base - 5, "V");
				} else if ( l->gid >= 0 && l->mode == LM_PLAIN ) {
					UB	buf[64];

					text_put(l, r, buf, var_text(l, r, buf), ix, base, r->colour);
				}
				break;
			case TV_RUN_PAGENO:
			case TV_RUN_MEMO:
				if ( l->detail ) {
					/* 詳細: a flag, M for a note and # for a page number */
					T_DPRECT	fb;

					fb.left = ix;
					fb.right = ix + it[k].w;
					fb.top = base - 5 - FLAG_H / 2;
					fb.bottom = fb.top + FLAG_H;
					mark_found(l, TV_MARK_RUN, pi, it[k].ri, 0, r->node, &fb);
					flag_draw(l, ix, it[k].w, base - 5,
						  ( r->kind == TV_RUN_MEMO ) ? "M" : "#");
				} else if ( r->kind == TV_RUN_PAGENO
					 && l->gid >= 0 && l->mode == LM_PLAIN ) {
					if ( l->pageno >= 0 ) {
						/* on a page: its number, counted on from 'num' */
						UB	buf[16];
						INT	n = page_no(r, l->pageno), m;

						m = num_text(n, buf);
						text_put(l, r, buf, m, ix, base, r->colour);
					} else {
						text_put(l, r, r->text, r->len, ix, base, r->colour);
					}
				}
				break;
			case TV_RUN_FIXSP:
				if ( l->detail ) {
					T_DPRECT	fb;

					fb.left = ix;
					fb.right = ix + it[k].w;
					fb.top = base - 5 - FLAG_H / 2;
					fb.bottom = fb.top + FLAG_H;
					mark_found(l, TV_MARK_RUN, pi, it[k].ri, 0, r->node, &fb);
					flag_draw(l, ix, it[k].w, base - 5, "_");
				}
				break;
			default:
				/* the marks of breaks and tabs, seen in detail */
				if ( l->detail && l->gid >= 0 && l->mode == LM_PLAIN ) {
					INT	mh = r->size / 2;

					if ( r->kind == TV_RUN_BREAK ) {
						dp_line(l->gid, ix + 2, base - mh, ix + 2,
							base, 0x008080FFU);
						dp_line(l->gid, ix + 2, base, ix + 6, base,
							0x008080FFU);
					} else if ( r->kind == TV_RUN_TAB ) {
						dp_line(l->gid, ix + 1, base - mh / 2,
							ix + it[k].w - 2, base - mh / 2,
							0x008080FFU);
					} else if ( r->kind == TV_RUN_INDENT ) {
						dp_line(l->gid, ix, base - mh, ix, base,
							0x0080C080U);
					}
				}
				break;
			}
			piece(l, r, pi, it[k].fpos, it[k].units, s, ix, it[k].w,
			      top, pitch);
			/* after a break, the place at the head of the next line */
			if ( ( r->kind == TV_RUN_BREAK || r->kind == TV_RUN_PAGE )
			  && k + 1 == n ) {
				piece(l, r, pi, it[k].fpos + 1, 0, NULL,
				      ( indent_x >= 0 ) ? indent_x : left, 0,
				      top + pitch, pitch);
			}
		}
		if ( l->gid >= 0 && l->mode == LM_PLAIN
		  && top + pitch >= l->top && top <= l->bottom ) {
			box_ends(l, d, it, a, e, shift_x, base);
			draw_ruby(l, d, it, a, e, shift_x, base);
			if ( l->detail && e == n && !forced ) {
				/* the end of the paragraph */
				INT	ex = shift_x + it[e - 1].x + it[e - 1].w + 2;

				dp_line(l->gid, ex, base - 6, ex, base, 0x0080C080U);
				dp_line(l->gid, ex - 3, base - 3, ex, base, 0x0080C080U);
			}
		}
		y += pitch;
		/* a page break inside the paragraph goes on to the next page */
		if ( forced && d->run[it[e - 1].ri].kind == TV_RUN_PAGE ) {
			y = page_next(l, y);
		}
		a = e;
		first_line = 0;
	}
	Kfree(it);
	y += p->pargap_abs ? p->pargap : para_size * p->pargap / 1000;

	return y;
}

/*
 * The whole of the work: walk the paragraphs, break the lines, and
 * either draw or only measure. One walk does all of it so that what is
 * measured and what is drawn cannot come apart.
 */
LOCAL INT lay_out( LAY *l, CONST T_TVDOC *d )
{
	INT	pi, y = 0;

	for ( pi = 0; pi < d->npara; pi++ ) {
		y = lay_para(l, d, pi, y);
	}
	/* in columns, as far down as the last page goes */
	if ( l->page_h > 0 && l->cols > 1 && y > 0 ) {
		y = ( ( y - 1 ) / l->page_h / l->cols + 1 ) * l->page_h;
	}

	return y;
}

/* A walk set up over a box, with nothing asked */
LOCAL void lay_init( LAY *l, INT gid, CONST T_TVDOC *d, CONST T_DPRECT *r,
		     INT scroll_y, CONST T_TAD *src )
{
	knl_memset(l, 0, sizeof(*l));
	l->gid = gid;
	l->doc = d;
	l->src = src;
	/*
	 * The margins are the paper's: a document laid on pages keeps
	 * inside them on every page, and one shown in a single length
	 * uses the whole of the box.
	 */
	l->x0 = r->left;
	l->x1 = r->right;
	l->y = r->top;
	l->detail = (BOOL)( dv_detail || d->detail );
	l->show_hidden = d->show_hidden;
	if ( d->page_h > 0
	  && d->page_h - d->margin[1] - d->margin[3] >= 32
	  && r->right - r->left - d->margin[0] - d->margin[2] >= 32 ) {
		l->page_h = d->page_h;
		l->mtop = d->margin[1];
		l->mbottom = d->margin[3];
		l->x0 += d->margin[0];
		l->x1 -= d->margin[2];
		l->full_x1 = l->x1;
		if ( d->columns > 1 && d->columns <= 8 ) {
			INT	sp = ( d->colsp > 0 ) ? d->colsp : 14;
			INT	w = ( l->x1 - l->x0 - ( d->columns - 1 ) * sp )
				  / d->columns;

			if ( w >= 32 ) {
				l->cols = d->columns;
				l->colstep = w + sp;
				l->x1 = l->x0 + w;
			}
		}
	}
	l->top = r->top;
	l->bottom = r->bottom;
	l->scroll = scroll_y;
	l->fid = fn_system();
	l->found = -1;
	l->best = -1;
	l->mode = LM_PLAIN;
	l->set_fid = 0;
	l->set_px = -1;
	l->pageno = -1;
	l->npages = 0;
	if ( l->full_x1 == 0 ) {
		l->full_x1 = l->x1;
	}
}

EXPORT ER tv_doc_caret_at( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			   CONST T_TAD *src, INT x, INT y, INT *p_para,
			   INT *p_pos )
{
	LAY	l;

	if ( d == NULL || r == NULL || p_para == NULL || p_pos == NULL ) {
		return E_PAR;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	l.mode = LM_CARET_AT;
	l.ask_x = x;
	l.ask_y = y;
	(void)lay_out(&l, d);
	if ( l.best < 0 ) {
		return E_NOEXS;
	}
	*p_para = l.best_para;
	*p_pos = l.best_pos;

	return E_OK;
}

EXPORT ER tv_doc_caret_box( CONST T_TVDOC *d, CONST T_DPRECT *r,
			    INT scroll_y, CONST T_TAD *src, INT para, INT pos,
			    T_DPRECT *box )
{
	LAY	l;

	if ( d == NULL || r == NULL || box == NULL ) {
		return E_PAR;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	l.mode = LM_CARET_BOX;
	l.want_para = para;
	l.want_pos = pos;
	(void)lay_out(&l, d);
	if ( !l.got ) {
		return E_NOEXS;
	}
	*box = l.box;

	return E_OK;
}

EXPORT void tv_doc_select( INT gid, CONST T_TVDOC *d, CONST T_DPRECT *r,
			   INT scroll_y, CONST T_TAD *src, INT a_para,
			   INT a_pos, INT b_para, INT b_pos )
{
	LAY	l;

	if ( d == NULL || r == NULL || gid < 0 ) {
		return;
	}
	lay_init(&l, gid, d, r, scroll_y, src);
	l.mode = LM_SELECT;
	l.sa_para = a_para;
	l.sa_pos = a_pos;
	l.sb_para = b_para;
	l.sb_pos = b_pos;
	(void)lay_out(&l, d);
}

EXPORT INT tv_doc_draw( INT gid, CONST T_TVDOC *d, CONST T_DPRECT *r,
			INT scroll_y, CONST T_TAD *src, UW paper )
{
	LAY		l;
	T_DPENV		env;
	BOOL		narrowed = FALSE;
	INT		h;

	if ( d == NULL || r == NULL ) {
		return 0;
	}
	/*
	 * Nothing goes outside the box. Where a line breaks is worked out
	 * from the widths of its letters, and a document that is shown
	 * with the wrong widths -- a face that could not be opened, a
	 * letter the face does not have -- would otherwise put ink on the
	 * frame around it. The box is the box.
	 */
	if ( gid >= 0 && dp_ref(gid, &env) >= E_OK ) {
		T_DPRECT	cut = env.visible;

		if ( cut.left < r->left )     cut.left = r->left;
		if ( cut.top < r->top )       cut.top = r->top;
		if ( cut.right > r->right )   cut.right = r->right;
		if ( cut.bottom > r->bottom ) cut.bottom = r->bottom;
		if ( cut.right > cut.left && cut.bottom > cut.top ) {
			dp_set_visible(gid, &cut);
			narrowed = TRUE;
		}
	}
	/*
	 * The paper first. What was on the screen before is not part of
	 * this document, and a viewer that draws only its own letters
	 * leaves the last document's showing between its lines.
	 *
	 * The ground comes from the numbered table and not from a colour
	 * written here, so that a document laid on a dark scheme is laid
	 * on that scheme's paper.
	 */
	if ( gid >= 0 ) {
		dp_fill_rect(gid, r, ( paper == TV_PAPER_LOOK )
				     ? wm_look(WM_LOOK_WORK) : paper);
	}
	lay_init(&l, gid, d, r, scroll_y, src);
	h = lay_out(&l, d);
	if ( gid >= 0 && l.page_h > 0 && d->over_on != 0 ) {
		overlays(&l, d, h);
	}
	if ( narrowed ) {
		dp_set_visible(gid, &env.visible);
	}

	return h;
}

/*
 * How tall the document comes out in a box that wide.
 *
 * It is the same reckoning as drawing it, with nothing drawn: the same
 * room inside the same margins, and the same record behind it, because
 * a link is as wide as the record says it is. Measuring it any other
 * way gives a height that the drawing then disagrees with, and every
 * disagreement of that kind shows up on the screen -- as a scroll bar
 * that runs out early, or as a line of text laid past the edge of the
 * box it was measured for.
 */
EXPORT INT tv_doc_height( CONST T_TVDOC *d, INT width, CONST T_TAD *src )
{
	LAY		l;
	T_DPRECT	r;

	if ( d == NULL ) {
		return 0;
	}
	r.left = 0;
	r.top = 0;
	r.right = width;
	r.bottom = 0;
	lay_init(&l, -1, d, &r, 0, src);
	l.y = 0;

	return lay_out(&l, d);
}

/*
 * Which virtual object is at a place, if any. The place is in the same
 * coordinates the document was drawn in.
 *
 * It is the drawing walk with nothing drawn and a question asked at
 * each link, so what it answers is what is on the screen. Working out
 * where the links are a second way would answer for a page that is not
 * the one the person is looking at, which is the kind of fault that
 * only shows up as "it opened the wrong thing".
 */
/*
 * A link or a picture standing in the text: the one under a place, or,
 * with 'want' a run's number, where that one is. Answers the run.
 */
EXPORT INT tv_doc_thing( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			 CONST T_TAD *src, INT x, INT y, INT want,
			 T_DPRECT *p_box )
{
	LAY	l;

	if ( d == NULL || r == NULL ) {
		return -1;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	l.thing_asking = TRUE;
	l.thing_x = x;
	l.thing_y = y;
	l.thing_want = want;
	l.thing_run = -1;
	(void)lay_out(&l, d);
	if ( l.thing_run >= 0 && p_box != NULL ) {
		*p_box = l.thing_box;
	}

	return l.thing_run;
}

/* A number as its digits; answers how many bytes */
/* A number as Roman numerals, small letters when 'small' */
LOCAL INT roman_text( INT n, BOOL small, UB *out )
{
	LOCAL CONST INT		val[13] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
	LOCAL CONST char	*CONST sym[13] = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
	INT			i, k, m = 0;

	for ( i = 0; i < 13 && n > 0; i++ ) {
		while ( n >= val[i] && m < 30 ) {
			for ( k = 0; sym[i][k] != 0; k++ ) {
				out[m++] = (UB)( small ? sym[i][k] + ( 'a' - 'A' ) : sym[i][k] );
			}
			n -= val[i];
		}
	}
	out[m] = 0;

	return m;
}

/*
 * 変数参照: what a variable stands for as it is drawn, into out (64
 * bytes): 0 the record's name; 100 the year's last two figures, 101
 * the era, 110/111 the month (two figures), 112/113 its name in small
 * and capital letters, 120/121 the day; 200/201/202 the page's number
 * in figures, small and capital Roman numerals; 250/251/252 the number
 * of pages the same ways. A page's number off the pages is 1; any
 * other variable, and one given by name, is its name.
 */
LOCAL INT var_text( CONST LAY *l, CONST T_TVRUN *r, UB *out )
{
	LOCAL CONST char	months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	TS_TIME			t;
	TS_TM			tm;
	INT			id = r->link, m = 0, pg, i;
	BOOL			have = FALSE;

	out[0] = 0;
	if ( id >= 100 && id < 200 ) {
		have = (BOOL)( dt_gettime(&t) >= E_OK && dt_localtime(&t, &tm) >= E_OK );
		if ( !have ) {
			return 0;
		}
	}
	pg = ( l->pageno >= 0 ) ? l->pageno + 1 : 1;
	switch ( id ) {
	case 0: {
		CONST UB	*nm = ( l->src != NULL ) ? tad_attr(tad_root(l->src), "filename") : NULL;

		for ( i = 0; nm != NULL && nm[i] != 0 && m < 63; i++ ) {
			out[m++] = nm[i];
		}
		out[m] = 0;
		return m;
	}
	case 100:
		m = num_text(tm.tm_year % 100, out);
		return m;
	case 101: {
		INT		y = tm.tm_year + 1900;
		CONST char	*e = ( y >= 2019 ) ? "令和" : ( y >= 1989 ) ? "平成" : ( y >= 1926 ) ? "昭和" : "大正";

		for ( i = 0; e[i] != 0; i++ ) {
			out[m++] = (UB)e[i];
		}
		out[m] = 0;
		return m;
	}
	case 110:
	case 120:
		return num_text(( id == 110 ) ? tm.tm_mon + 1 : tm.tm_mday, out);
	case 111:
	case 121: {
		INT	v = ( id == 111 ) ? tm.tm_mon + 1 : tm.tm_mday;

		out[0] = (UB)( '0' + v / 10 % 10 );
		out[1] = (UB)( '0' + v % 10 );
		out[2] = 0;
		return 2;
	}
	case 112:
	case 113:
		for ( i = 0; i < 3; i++ ) {
			UB	c = (UB)months[tm.tm_mon % 12 * 3 + i];

			out[i] = ( id == 113 ) ? (UB)( c - ( 'a' - 'A' ) ) : c;
		}
		out[3] = 0;
		return 3;
	case 200:
		return num_text(pg, out);
	case 201:
	case 202:
		return roman_text(pg, (BOOL)( id == 201 ), out);
	case 250:
		return num_text(( l->npages > 0 ) ? l->npages : 1, out);
	case 251:
	case 252:
		return roman_text(( l->npages > 0 ) ? l->npages : 1, (BOOL)( id == 251 ), out);
	default:
		break;
	}
	for ( i = 0; r->text != NULL && i < r->len && m < 63; i++ ) {
		out[m++] = r->text[i];
	}
	out[m] = 0;

	return m;
}

LOCAL INT num_text( INT n, UB *out )
{
	UB	t[12];
	INT	k = 0, m = 0;

	if ( n < 0 ) {
		out[m++] = '-';
		n = -n;
	}
	do {
		t[k++] = (UB)( '0' + n % 10 );
		n /= 10;
	} while ( n > 0 && k < 11 );
	while ( k > 0 ) {
		out[m++] = t[--k];
	}
	out[m] = 0;

	return m;
}

/* The number a page-number shows on the page 'pg' after the first */
LOCAL INT page_no( CONST T_TVRUN *r, INT pg )
{
	INT		num = 0, i, step = 1;
	CONST UB	*s;

	for ( i = 0; r->text != NULL && i < r->len; i++ ) {
		if ( r->text[i] >= '0' && r->text[i] <= '9' ) {
			num = num * 10 + ( r->text[i] - '0' );
		}
	}
	if ( r->node != NULL && ( s = tad_attr(r->node, "step") ) != NULL ) {
		step = 0;
		for ( i = 0; s[i] >= '0' && s[i] <= '9'; i++ ) {
			step = step * 10 + ( s[i] - '0' );
		}
	}

	return num + step * pg;
}

/*
 * One overlay laid in a band of a page: each paragraph on the band's one
 * line, placed by its own alignment, so that what stands at the left, in
 * the middle and at the right of a header share the line. A line as tall
 * as the band, as a tab format's height makes it, starts at its top;
 * a lower one is put in the middle of it.
 */
LOCAL void band_draw( LAY *l, CONST T_TVDOC *o, INT top, INT h, BOOL at_foot,
		      INT pg )
{
	LAY		m;
	T_DPRECT	r;
	INT		pi, lh = 0;

	r.left = l->x0;
	r.right = l->full_x1;
	r.top = top;
	r.bottom = top + h;
	for ( pi = 0; pi < o->npara; pi++ ) {
		lay_init(&m, -1, o, &r, 0, l->src);
		m.y = 0;
		(void)lay_para(&m, o, pi, 0);
		if ( m.last_pitch > lh ) {
			lh = m.last_pitch;
		}
	}
	if ( ( o->npara > 0 && o->para[0].gap_abs && lh > 0 ) || h < lh ) {
		/* the band as tall as it says, or as the line when the margin is less */
		h = lh;
		if ( at_foot ) {
			top = r.bottom - h;
		}
	}
	for ( pi = 0; pi < o->npara; pi++ ) {
		lay_init(&m, l->gid, o, &r, 0, l->src);
		m.top = l->top;
		m.bottom = l->bottom;
		m.y = top + ( ( h > lh ) ? ( h - lh ) / 2 : 0 );
		m.pageno = pg;
		m.npages = l->npages;
		(void)lay_para(&m, o, pi, 0);
	}
}

/*
 * 用紙オーバーレイ: each overlay the document puts on, on the pages it is
 * for, in the top margin of every page on the screen -- the footer (1),
 * and one whose text begins with a fill line, in the bottom margin.
 */
LOCAL void overlays( LAY *l, CONST T_TVDOC *d, INT h )
{
	INT	per = l->page_h * ( ( l->cols > 1 ) ? l->cols : 1 );
	INT	pages = ( per > 0 ) ? ( h + per - 1 ) / per : 0, pg, k;

	l->npages = pages;
	for ( pg = 0; pg < pages; pg++ ) {
		INT	top = l->y + pg * l->page_h - l->scroll;

		if ( top > l->bottom || top + l->page_h < l->top ) {
			continue;
		}
		for ( k = 0; k < TV_MAX_OVER; k++ ) {
			CONST T_TVDOC	*o = d->over[k];
			UINT		p = d->over_pages[k];

			if ( o == NULL || ( d->over_on & ( 1U << k ) ) == 0
			  || ( p == 1 && ( pg + 1 ) % 2 == 0 )
			  || ( p == 2 && ( pg + 1 ) % 2 != 0 ) ) {
				continue;
			}
			if ( k != 1 && !o->foot ) {
				band_draw(l, o, top, l->mtop, FALSE, pg);
			} else {
				band_draw(l, o, top + l->page_h - l->mbottom, l->mbottom,
					  TRUE, pg);
			}
		}
	}
}

/*
 * 詳細: the mark of a setting under a place -- a flag, or the ruler of
 * a tab format and what on it -- as the document is drawn in detail.
 */
EXPORT INT tv_doc_mark( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			CONST T_TAD *src, INT x, INT y, T_TVMARK *out )
{
	LAY	l;

	if ( out != NULL ) {
		knl_memset(out, 0, sizeof(*out));
	}
	if ( d == NULL || r == NULL ) {
		return TV_MARK_NONE;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	if ( !l.detail ) {
		return TV_MARK_NONE;
	}
	l.mark_asking = TRUE;
	l.mark_x = x;
	l.mark_y = y;
	(void)lay_out(&l, d);
	if ( out != NULL ) {
		*out = l.mark;
	}

	return l.mark.kind;
}

EXPORT ER tv_doc_at( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
		     CONST T_TAD *src, INT x, INT y, T_VOBJ *out,
		     T_DPRECT *p_box, INT *p_which )
{
	LAY	l;

	if ( d == NULL || r == NULL || src == NULL ) {
		return E_PAR;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	l.asking = TRUE;
	l.ask_x = x;
	l.ask_y = y;
	(void)lay_out(&l, d);
	if ( l.found < 0 ) {
		return E_NOEXS;
	}
	if ( p_box != NULL ) {
		*p_box = l.found_box;
	}
	if ( p_which != NULL ) {
		*p_which = l.found;
	}
	if ( out != NULL ) {
		return tad_lnk_get(src, l.found, out);
	}

	return E_OK;
}

EXPORT ER tv_paint( INT gid, CONST T_DPRECT *r, CONST UB *xml, SZ len )
{
	T_TAD	*doc;
	UW	paper = wm_look(WM_LOOK_WORK);
	ER	er;

	if ( r == NULL ) {
		return E_PAR;
	}
	dp_fill_rect(gid, r, paper);
	if ( xml == NULL || len <= 0 ) {
		return E_OK;
	}
	er = tad_parse(xml, len, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	switch ( tv_kind(doc) ) {
	case TV_KIND_DOC: {
		T_TVDOC	*d;

		er = tv_doc(doc, &d);
		if ( er >= E_OK ) {
			(void)tv_doc_draw(gid, d, r, 0, doc, paper);
			tv_doc_free(d);
		}
		break;
	}
	case TV_KIND_FIG: {
		T_TVFIG	*f;

		er = tv_fig(doc, &f);
		if ( er >= E_OK ) {
			er = tv_fig_draw(gid, f, r, 0, 0, doc, paper);
			tv_fig_free(f);
		}
		break;
	}
	default:
		break;
	}
	tad_free(doc);

	return er;
}

/* The colour nothing a figure draws comes out as: what is left of it is outside */
#define TV_SHAPE_KEY	0xFF01FE03U

/*
 * The rows of a mask put into a region. A region is made from rows of
 * one span each, so the k-th span of each row is gathered while the
 * rows run on with one, and each such run is added in one piece.
 */
LOCAL ER mask_region( CONST UW *px, INT w, INT h, T_DPRGN **p_rgn )
{
	INT	*x0, *x1;
	T_DPRGN	*all = NULL, *part, *both;
	INT	k, y, run, found;
	ER	er = E_OK;

	x0 = (INT *)Kmalloc(sizeof(INT) * (SZ)h);
	x1 = (INT *)Kmalloc(sizeof(INT) * (SZ)h);
	if ( x0 == NULL || x1 == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	for ( k = 0, found = 1; found && er >= E_OK; k++ ) {
		found = 0;
		run = 0;
		for ( y = 0; y <= h && er >= E_OK; y++ ) {
			INT	a = -1, b = -1, n = 0, x;

			/* the k-th span of this row, if it has one */
			for ( x = 0; y < h && x < w; ) {
				if ( px[(SZ)y * w + x] == TV_SHAPE_KEY ) { x++; continue; }
				a = x;
				while ( x < w && px[(SZ)y * w + x] != TV_SHAPE_KEY ) x++;
				b = x;
				if ( n++ == k ) break;
				a = -1;
			}
			if ( a >= 0 ) {
				x0[run] = a;
				x1[run] = b;
				run++;
				found = 1;
				continue;
			}
			if ( run > 0 ) {
				er = dp_rgn_from_rows(y - run, run, x0, x1, &part);
				if ( er >= E_OK ) {
					if ( all == NULL ) {
						all = part;
					} else {
						er = dp_rgn_or(all, part, &both);
						dp_rgn_free(part);
						if ( er >= E_OK ) {
							dp_rgn_free(all);
							all = both;
						}
					}
				}
				run = 0;
			}
		}
	}
	if ( er >= E_OK && all == NULL ) {
		T_DPRECT	none = { 0, 0, 0, 0 };

		er = dp_rgn_rect(&none, &all);
	}
    out:
	if ( x0 != NULL ) Kfree(x0);
	if ( x1 != NULL ) Kfree(x1);
	if ( er < E_OK ) {
		dp_rgn_free(all);
		all = NULL;
	}
	*p_rgn = all;
	return er;
}

EXPORT ER tv_shape( CONST UB *xml, SZ len, INT w, INT h, T_DPRGN **p_rgn )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;
	T_DPRECT	r;
	UW		*px;
	INT		gid;
	SZ		i;
	ER		er;

	if ( xml == NULL || len <= 0 || w <= 0 || h <= 0 || p_rgn == NULL ) {
		return E_PAR;
	}
	er = tad_parse(xml, len, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	if ( tv_kind(doc) != TV_KIND_FIG || tv_fig(doc, &f) < E_OK ) {
		tad_free(doc);
		return E_PAR;			/* an outline is a figure */
	}
	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	gid = ( px != NULL ) ? dp_open() : E_NOMEM;
	if ( gid < 0 ) {
		er = (ER)gid;
		goto out;
	}
	for ( i = 0; i < (SZ)w * h; i++ ) {
		px[i] = TV_SHAPE_KEY;
	}
	r.left = 0;
	r.top = 0;
	r.right = w;
	r.bottom = h;
	dp_set_target(gid, px, (UINT)( w * sizeof(UW) ), 0, 0);
	dp_set_origin(gid, 0, 0);
	dp_set_frame(gid, &r);
	dp_set_visible(gid, &r);
	er = tv_fig_draw(gid, f, &r, 0, 0, doc, TV_SHAPE_KEY);
	dp_close(gid);
	if ( er >= E_OK ) {
		er = mask_region(px, w, h, p_rgn);
	}
    out:
	if ( px != NULL ) Kfree(px);
	tv_fig_free(f);
	tad_free(doc);
	return er;
}
