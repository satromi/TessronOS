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
 *	the paragraph's tab format asks for, half a size when it asks none;
 *	a virtual object, a picture or a figure stands on the base line and
 *	adds to that only what it stands taller than the letters.
 */

#include <ts/tvport.h>
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
	INT	lasc, ldesc;		/* the same for the letters where it stands */
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
	/*
	 * The line that holds the place asked for (LM_CARET_BOX): its first
	 * and last place, and the top of the piece that held it. And a
	 * place under a point looked for in one stretch of one paragraph
	 * only (LM_CARET_AT, 'only_para' >= 0).
	 */
	INT		line_a, line_e, hit_top, hits;
	INT		only_para, only_a, only_e;
	INT		best_dy, best_dx;
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

	/*
	 * 縦書き. The walk is the same either way and works in places of
	 * its own: x along a line, y across the lines, as a text written
	 * across has them. Written downwards, a place (x, y) of the walk is
	 * the point (vr - 1 - y, x) of the box -- x runs down the box, y runs
	 * to the left from its right edge 'vr' -- and every rectangle the
	 * walk puts down or answers is turned so on its way out. Only the
	 * letters themselves are drawn another way: down the line, in their
	 * vertical forms.
	 */
	BOOL		vert;
	INT		vr;

	/*
	 * How the document is shown, from the document (T_TVDOC): what it
	 * is magnified by, zoom / zden, every length of it alike; how many
	 * opened objects deep it is, its links drawn that deep; and how
	 * deep opened objects inside it are drawn with what is inside.
	 */
	INT		zoom, zden;
	INT		depth;
	INT		open_max;
	BOOL		bare;

	/*
	 * Drawn: the whole box across the lines, as top and bottom are
	 * before they are narrowed to the part being drawn again, and where
	 * the paragraphs that show in it are put down (tv_doc_band); NULL
	 * not kept.
	 */
	INT		vtop, vbottom;
	T_TVSHOWN	*seen;

	/*
	 * How far past what is being drawn a line of letters is drawn too,
	 * its letters reaching past its pitch: the whole box, INK_SLACK; a
	 * part of it drawn again, only what a shadow or a bag reaches by
	 * (BAND_EDGE), the part having been made that much larger.
	 */
	INT		slack;
} LAY;

#define INK_SLACK	64
#define BAND_EDGE	4	/* past a line's own pitch: a shadow's 2, a bag's 1 */

/* A rectangle of the walk's places, as it stands in the box */
LOCAL void ph_rect( CONST LAY *l, T_DPRECT *rc )
{
	T_DPRECT	o = *rc;

	if ( !l->vert ) {
		return;
	}
	rc->left = l->vr - o.bottom;
	rc->right = l->vr - o.top;
	rc->top = o.left;
	rc->bottom = o.right;
}

/* A point of the box, as a place of the walk */
LOCAL void lg_point( CONST LAY *l, INT px, INT py, INT *p_x, INT *p_y )
{
	if ( !l->vert ) {
		*p_x = px;
		*p_y = py;
		return;
	}
	*p_x = py;
	*p_y = l->vr - 1 - px;
}

/* A line between two places of the walk */
LOCAL void lg_line( CONST LAY *l, INT x0, INT y0, INT x1, INT y1, UW col )
{
	if ( l->vert ) {
		dp_line(l->gid, l->vr - 1 - y0, x0, l->vr - 1 - y1, x1, col);
	} else {
		dp_line(l->gid, x0, y0, x1, y1, col);
	}
}

/* A rectangle of the walk's places filled */
LOCAL void lg_fill( CONST LAY *l, CONST T_DPRECT *rc, UW col )
{
	T_DPRECT	o = *rc;

	ph_rect(l, &o);
	dp_fill_rect(l->gid, &o, col);
}

LOCAL void lg_pixel( CONST LAY *l, INT x, INT y, UW col )
{
	if ( l->vert ) {
		dp_put_pixel(l->gid, l->vr - 1 - y, x, col);
	} else {
		dp_put_pixel(l->gid, x, y, col);
	}
}

#define LM_PLAIN	0		/* drawing, measuring, or finding a link */
#define LM_CARET_AT	1		/* the place under a point */
#define LM_CARET_BOX	2		/* where a place is */
#define LM_SELECT	3		/* a stretch turned over */

/*
 * A length of the document as it is drawn: magnified as the document is
 * (LAY.zoom / LAY.zden), by the walk 'l' in hand
 */
#define DZ(v)	( ( l->zoom == l->zden ) ? (v) : (v) * l->zoom / l->zden )

LOCAL INT num_text( INT n, UB *out );
LOCAL INT page_no( CONST T_TVRUN *r, INT pg );
LOCAL INT var_text( CONST LAY *l, CONST T_TVRUN *r, UB *out );
LOCAL void overlays( LAY *l, CONST T_TVDOC *d, INT h );
LOCAL void sheets( LAY *l, CONST T_TVDOC *d, INT h );

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
LOCAL INT run_px( CONST LAY *l, CONST T_TVRUN *r )
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

/*
 * The face of a run, and which of the looks it asks for (FN_FA_LOOK) the
 * face has of itself: a face registered as the bold, the italic, the
 * outlined or the shadowed letters of its family is taken for those, and
 * the look is not added to it again.
 */
LOCAL ID run_face( CONST T_TVRUN *r, UINT *p_has )
{
	UINT	want = 0;

	*p_has = 0;
	if ( r->face == NULL ) {
		return fn_system();
	}
	if ( ( r->style & TV_ST_BOLD ) != 0 )   want |= FN_FA_BOLD;
	if ( ( r->style & TV_ST_ITALIC ) != 0 ) want |= FN_FA_ITALIC;
	if ( ( r->style & TV_ST_BAG ) != 0 )    want |= FN_FA_BAG;
	if ( ( r->style & TV_ST_SHADOW ) != 0 ) want |= FN_FA_SHADOW;
	return fn_find_style(r->face, want, p_has);
}

/* The face, size and look of a run set, when they are not set already */
LOCAL void run_font( LAY *l, CONST T_TVRUN *r )
{
	UINT	has;
	ID	fid = run_face(r, &has);
	INT	px = run_px(l, r);
	INT	xpx = 0;
	UINT	st = 0;

	if ( r->wr_d > 0 && r->wr_n > 0 && r->wr_n != r->wr_d ) {
		xpx = px * r->wr_n / r->wr_d;
	}
	if ( ( r->style & TV_ST_BOLD ) != 0 && ( has & FN_FA_BOLD ) == 0 )     st |= FN_ST_BOLD;
	if ( ( r->style & TV_ST_ITALIC ) != 0 && ( has & FN_FA_ITALIC ) == 0 ) st |= FN_ST_ITALIC;
	/*
	 * Down the line the letters take their vertical forms, but not
	 * those turned (回転), which are the letters across turned, nor
	 * those set across in one box (縦中横).
	 */
	if ( l->vert && r->turn == 0 && ( r->style & TV_ST_COMB ) == 0 ) {
		st |= FN_ST_VERT;
	}
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
LOCAL INT run_space( CONST LAY *l, CONST T_TVRUN *r )
{
	return ( r->space > 0 ) ? run_px(l, r) * r->space / 1000 : 0;
}

/*
 * How far a run's baseline is moved up (文字基準位置移動), in pixels:
 * below nought, down.
 */
LOCAL INT run_lift( CONST LAY *l, CONST T_TVRUN *r )
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
		*p_asc = run_px(l, r);
		*p_desc = run_px(l, r) / 4;
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
	/* 縦中横: whatever it holds, one box down the line */
	if ( l->vert && ( r->style & TV_ST_COMB ) != 0 ) {
		return run_px(l, r) + run_space(l, r);
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
	if ( l->vert ) {
		w = fn_vwidth(l->fid, b);
	} else if ( run_space(l, r) != 0 ) {
		/*
		 * Spaced letters are put down one at a time (text_put), each
		 * at a whole pixel: measured the same way, letter by letter,
		 * or the fractions of a pixel a whole string keeps would make
		 * it measure other than it draws.
		 */
		INT	k, wk;

		w = 0;
		for ( i = 0; i < n; i += k ) {
			UB	c[8];

			for ( k = 1; i + k < n && k < (INT)sizeof(c) - 1 && ( b[i + k] & 0xC0U ) == 0x80U; k++ ) {
				;
			}
			knl_memcpy(c, b + i, (SZ)k);
			c[k] = 0;
			wk = fn_width(l->fid, c);
			w += ( wk > 0 ) ? wk : 0;
		}
	} else {
		w = fn_width(l->fid, b);
	}
	turn_off(l, r);
	if ( b != buf ) {
		Kfree(b);
	}
	if ( w < 0 ) {
		w = 0;
	}

	return w + chars * run_space(l, r);
}

/*
 * 縦中横: the letters of a run set across in one box down the line, the
 * box as tall as the letters' size and its middle on the line's middle.
 * Letters wider together than the box are drawn narrower to fit it.
 */
LOCAL void comb_put( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n, INT x,
		     INT base, UW col )
{
	UB	mem[64], *buf = mem;
	INT	i, em = run_px(l, r), tw;
	UINT	st = 0;
	T_FNMET	met;

	if ( n >= (INT)sizeof(mem) ) {
		buf = (UB *)Kmalloc((SZ)n + 1);
		if ( buf == NULL ) {
			return;
		}
	}
	for ( i = 0; i < n; i++ ) {
		buf[i] = s[i];
	}
	buf[i] = 0;
	if ( ( r->style & TV_ST_BOLD ) != 0 )   st |= FN_ST_BOLD;
	if ( ( r->style & TV_ST_ITALIC ) != 0 ) st |= FN_ST_ITALIC;
	tw = fn_width(l->fid, buf);
	if ( tw > em && tw > 0 ) {
		INT	xpx = em * em / tw;

		(void)fn_set_style(l->fid, st, ( xpx > 0 ) ? xpx : 1);
		l->set_fid = 0;			/* the face is not as it was set */
		tw = fn_width(l->fid, buf);
	}
	if ( fn_metrics(l->fid, &met) < E_OK ) {
		met.ascent = em;
		met.descent = em / 4;
	}
	(void)fn_draw(l->gid, l->fid, l->vr - 1 - base - tw / 2,
		      x + em / 2 + ( met.ascent - met.descent ) / 2, buf, col);
	if ( buf != mem ) {
		Kfree(buf);
	}
}

/* Text put down in the face now set: each letter, and the spacing after it */
LOCAL void text_put( LAY *l, CONST T_TVRUN *r, CONST UB *s, INT n, INT x,
		     INT base, UW col )
{
	UB	buf[64], *b = buf;
	INT	i, sp = run_space(l, r);

	if ( n <= 0 || l->fid <= 0 || l->gid < 0 ) {
		return;
	}
	if ( l->vert && ( r->style & TV_ST_COMB ) != 0 ) {
		comb_put(l, r, s, n, x, base, col);
		return;
	}
	if ( l->vert ) {
		/* down the line, letter by letter, the spacing after each */
		turn_on(l, r);
		for ( i = 0; i < n; ) {
			INT	k = 1;

			while ( i + k < n && k < (INT)sizeof(buf) - 1
			     && ( s[i + k] & 0xC0U ) == 0x80U ) {
				k++;
			}
			knl_memcpy(buf, s + i, (SZ)k);
			buf[k] = 0;
			x += fn_vdraw(l->gid, l->fid, l->vr - 1 - base, x, buf, col) + sp;
			i += k;
		}
		turn_off(l, r);
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
		INT	dy = 0, dx = 0, at = pos;

		if ( l->only_para >= 0
		  && ( pi != l->only_para || pos < l->only_a
		    || pos + units > l->only_e ) ) {
			break;
		}

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
		/* the nearest line first, then the nearest place along it */
		if ( l->best < 0 || dy < l->best_dy
		  || ( dy == l->best_dy && dx < l->best_dx ) ) {
			l->best = 0;
			l->best_dy = dy;
			l->best_dx = dx;
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
			l->line_a = l->line_e = l->want_pos;
			l->hit_top = top;
			l->hits++;
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
			ph_rect(l, &rc);
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
		} else if ( v.right > v.left ) {
			*p_w = units_px(( l->doc != NULL ) ? l->doc->hunit : -72, v.right - v.left);
		}
		if ( v.heightpx > 0 ) {
			*p_h = v.heightpx;
		} else if ( v.bottom > v.top ) {
			*p_h = units_px(( l->doc != NULL ) ? l->doc->vunit : -72, v.bottom - v.top);
		}
		/*
		 * The height a figure gives it (tad_lnk_fit_h): never shorter
		 * than the object closed, and closed when it was sized only a
		 * little past its band. The width is the record's, never less
		 * than the least a box of it may be (tad_lnk_least_w): the
		 * name is cut to it, not the box sized to the name.
		 */
		*p_h = tad_lnk_fit_h(&v, *p_h);
		if ( *p_w < tad_lnk_least_w(&v) ) {
			*p_w = tad_lnk_least_w(&v);
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
		T_TVHOW	how;

		if ( v.chsz > 0 ) {
			v.chsz = DZ(v.chsz);	/* magnified with the text */
		}
		how.depth = l->depth;
		how.open_max = l->open_max;
		how.bare = l->bare;
		tv_draw_link_how(l->gid, &box, &v, &how);
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
		INT		asc, desc, px = run_px(l, r), met_asc, met_desc;

		run_font(l, r);
		asc = px;
		desc = px / 4;
		if ( l->vert ) {
			/* down the line a letter's box is centred on the line */
			asc = px - px / 2;
			desc = px / 2;
		} else if ( l->fid > 0 && fn_metrics(l->fid, &met) >= E_OK ) {
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
			shift -= run_lift(l, r);
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
				if ( r->turn != 0 && !l->vert ) {
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
			/* across, it stands on the base line: all of it above */
			it[n].asc = h;  it[n].desc = 0;  it[n].shift = 0;
			it[n].lasc = asc;  it[n].ldesc = desc;
			if ( l->vert ) {
				/*
				 * Down the line it stays upright, as it is
				 * across, and stands centred on the line: its
				 * height goes down the line, its width across.
				 */
				it[n].w = h + 2;
				it[n].asc = w - w / 2;
				it[n].desc = w / 2;
			}
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
LOCAL INT tab_to( CONST LAY *l, CONST T_TVPARA *p, INT base, INT x, INT size )
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
	UINT		has = 0;
	INT		px = run_px(l, r), b = base + t->shift;
	INT		up = l->vert ? px - px / 2 : px;	/* the letters' box over */
	INT		dn = l->vert ? px / 2 : px / 4;		/* and under the base line */
	T_DPRECT	rc;

	rc.left = x;
	rc.right = x + t->w;
	rc.top = top;
	rc.bottom = top + h;
	if ( r->back != TAD_COL_NONE ) {
		lg_fill(l, &rc, r->back);
	}
	if ( ( r->style & TV_ST_MESH ) != 0 ) {
		/* 網掛: every other pixel of the ground in grey */
		INT	yy, xx;

		for ( yy = rc.top; yy < rc.bottom; yy++ ) {
			for ( xx = rc.left + ( yy & 1 ); xx < rc.right; xx += 2 ) {
				lg_pixel(l, xx, yy, 0x00A0A0A0U);
			}
		}
	}
	if ( ( r->style & TV_ST_INVERT ) != 0 ) {
		lg_fill(l, &rc, col);
		col = ( col == 0x00FFFFFFU ) ? 0x00000000U : 0x00FFFFFFU;
	}
	if ( ( r->style & ( TV_ST_SHADOW | TV_ST_BAG ) ) != 0 ) {
		(void)run_face(r, &has);	/* a face drawn so already is not drawn so again */
	}
	if ( ( r->style & TV_ST_SHADOW ) != 0 && ( has & FN_FA_SHADOW ) == 0 ) {
		text_put(l, r, s, t->len, x + 2, b + 2, 0x00909090U);
	}
	if ( ( r->style & TV_ST_BAG ) != 0 && ( has & FN_FA_BAG ) == 0 ) {
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
	/*
	 * The lines: across a line under and over the letters; down a line
	 * the side under is the left, the side over the right.
	 */
	if ( ( r->style & TV_ST_UNDER ) != 0 ) {
		INT	u = l->vert ? b + dn + 1 : b + 2;

		lg_line(l, x, u, x + t->w - 1, u, col);
	}
	if ( ( r->style & TV_ST_OVER ) != 0 ) {
		lg_line(l, x, b - up, x + t->w - 1, b - up, col);
	}
	if ( ( r->style & TV_ST_STRIKE ) != 0 ) {
		INT	m = l->vert ? b : b - px / 3;

		lg_line(l, x, m, x + t->w - 1, m, col);
	}
	if ( ( r->style & TV_ST_BOX ) != 0 ) {
		lg_line(l, x, b - up - 1, x + t->w, b - up - 1, col);
		lg_line(l, x, b + dn + 1, x + t->w, b + dn + 1, col);
	}
	if ( r->bouten != 0 ) {
		/* 傍点: a dot, or a sesame, over or under each letter */
		INT	i = 0, cx = x;

		while ( i < t->len ) {
			INT	k = 1, cw, dy;

			while ( i + k < t->len && ( s[i + k] & 0xC0U ) == 0x80U ) k++;
			cw = text_w(l, r, s + i, k);
			dy = ( ( r->bouten & 3 ) == 2 ) ? b + dn + 3 : b - up - 3;
			if ( ( r->bouten >> 4 ) == 1 ) {
				lg_line(l, cx + cw / 2 - 1, dy - 1, cx + cw / 2 + 1,
					dy + 1, col);
			} else {
				T_DPRECT	d;

				d.left = cx + cw / 2 - 1;  d.right = d.left + 3;
				d.top = dy - 1;  d.bottom = dy + 2;
				lg_fill(l, &d, col);
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
		INT		px = run_px(l, r);
		INT		up = l->vert ? px - px / 2 : px;
		INT		dn = l->vert ? px / 2 : px / 4;

		if ( ( r->style & TV_ST_BOX ) == 0 ) {
			continue;
		}
		if ( k == a || it[k - 1].ri != it[k].ri ) {
			lg_line(l, shift_x + it[k].x, base - up - 1,
				shift_x + it[k].x, base + dn + 1, r->colour);
		}
		if ( k + 1 == e || it[k + 1].ri != it[k].ri ) {
			INT	xr = shift_x + it[k].x + it[k].w;

			lg_line(l, xr, base - up - 1, xr, base + dn + 1,
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
		UB		mem[128], *buf = mem;
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
		px = run_px(l, r) / 2;
		if ( px < 6 ) px = 6;
		fid = l->fid;
		save_px = l->set_px;
		if ( r->ruby_len >= (INT)sizeof(mem) ) {
			/* a long ruby: as long as it is */
			buf = (UB *)Kmalloc((SZ)r->ruby_len + 1);
			if ( buf == NULL ) {
				k = j;
				continue;
			}
		}
		if ( fid > 0 ) {
			fn_set_size(fid, px);
			l->set_px = -1;			/* the face is not what it was */
			for ( m = 0; m < r->ruby_len; m++ ) {
				buf[m] = r->ruby[m];
			}
			buf[m] = 0;
			if ( l->vert ) {
				/*
				 * Down the line: the ruby down its right side
				 * (over) or its left (under), in the vertical
				 * forms, centred along what it belongs to.
				 */
				INT	half = run_px(l, r) / 2, axis;

				(void)fn_set_style(fid, FN_ST_VERT, 0);
				rw = fn_vwidth(fid, buf);
				axis = r->ruby_below ? base + half + px / 2 + 1
						     : base - ( run_px(l, r) - half ) - ( px - px / 2 ) - 1;
				(void)fn_vdraw(l->gid, fid, l->vr - 1 - axis,
					       x0 + ( ( x1 - x0 ) - rw ) / 2, buf, r->colour);
				if ( buf != mem ) Kfree(buf);
				k = j;
				continue;
			}
			rw = fn_width(fid, buf);
			(void)fn_metrics(fid, &met);
			(void)fn_draw(l->gid, fid, x0 + ( ( x1 - x0 ) - rw ) / 2,
				      r->ruby_below
				      ? base + run_px(l, r) / 4 + met.ascent + 1
				      : base - run_px(l, r) - met.descent - 1,
				      buf, r->colour);
		}
		if ( buf != mem ) Kfree(buf);
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
	if ( !l->mark_asking || l->mark.kind != TV_MARK_NONE || l->vert
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
	/* down the line a flag keeps its room and is not drawn */
	if ( l->gid < 0 || l->mode != LM_PLAIN || l->vert
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

/*
 * Whether a thing may hang past the end of a line (ぶら下げ): a letter
 * the paragraph's 禁則 forbids at the head of a line, when its kind K is
 * 3, ぶら下がり -- the comma and the full stop of 原稿用紙, written in
 * the last square with the letter before them rather than at the top of
 * the next line.
 */
LOCAL BOOL hangs( CONST T_TVDOC *d, CONST T_TVPARA *p, CONST ITEM *t )
{
	CONST T_TVRUN	*r = &d->run[t->ri];

	if ( p->khead_kind < 0 || ( p->khead_kind >> 4 ) != 3
	  || r->kind != TV_RUN_TEXT || t->len <= 0 ) {
		return FALSE;
	}

	return in_letters(p->khead, p->khead_len, utf8_at(r->text + t->off, NULL));
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
			lg_line(l, xx, yy, xx + 2, yy, 0x00808080U);
		}
	}
	/* 詳細: a tab format given here is shown as a ruler above it */
	if ( l->detail && p->fmt != NULL && !l->vert ) {
		INT	rtop, rdx;

		y = page_fit(l, y, RULER_ROOM);
		rtop = l->y + page_map(l, y, &rdx) - l->scroll;
		ruler(l, d, pi, rdx, rtop + ( RULER_ROOM - RULER_H ) / 2);
		y += RULER_ROOM;
	}

	/* a paragraph with nothing in it is still a line */
	if ( n == 0 ) {
		/* as tall as its lines are when their pitch is given in pixels */
		INT	h = ( p->gap_abs && p->gap > 0 ) ? p->gap
			  : para_size + para_size / 2, top;

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
		INT	oasc = 0, odesc = 0, lasc = 0, ldesc = 0, base_off, up, down;
		INT	top, base, width, extra, shift_x = 0, spaces = 0, col_dx = 0;
		INT	hits0;
		BOOL	forced = FALSE;

		/* how many things go on this line */
		for ( k = a; k < n; k++ ) {
			CONST T_TVRUN	*r = &d->run[it[k].ri];
			INT		w = it[k].w;

			if ( ( it[k].flags & IT_TAB ) != 0 ) {
				w = tab_to(l, p, left, x, r->size) - x;
			}
			if ( k > a && x + w > limit && w > 0 && x <= limit
			  && hangs(d, p, &it[k]) ) {
				/*
				 * ぶら下げ: a letter that may not begin a line
				 * stands past the end of this one, and the line
				 * ends after it.
				 */
				it[k].x = x;
				it[k].w = w;
				x += w;
				e = k + 1;
				break;
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
				it[k].w = tab_to(l, p, left, x, r->size) - x;
			}
			it[k].x = x;
			x += it[k].w;
			/*
			 * A link, a picture or a figure is not a letter: the
			 * gap between lines is a share of the letters' size (or
			 * so many pixels) and is not given to a box. What it
			 * stands above and below the letters is added to the
			 * line below (oasc, odesc).
			 */
			if ( r->kind == TV_RUN_LINK || r->kind == TV_RUN_IMAGE
			  || r->kind == TV_RUN_FIGURE ) {
				if ( it[k].asc > oasc )    oasc = it[k].asc;
				if ( it[k].desc > odesc )  odesc = it[k].desc;
				if ( it[k].lasc > lasc )   lasc = it[k].lasc;
				if ( it[k].ldesc > ldesc ) ldesc = it[k].ldesc;
			} else {
				if ( it[k].asc > asc )   asc = it[k].asc;
				if ( it[k].desc > desc ) desc = it[k].desc;
			}
			if ( r->kind == TV_RUN_TEXT || r->kind == TV_RUN_TAB ) {
				INT	sz = DZ(r->size);

				if ( r->hr_d > 0 && r->hr_n > 0 ) sz = sz * r->hr_n / r->hr_d;
				if ( sz > big ) big = sz;
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
		if ( asc + desc <= 0 ) {
			/* only boxes on the line: the letters they stand among */
			asc = lasc;
			desc = ldesc;
		}
		/*
		 * The letters' line first: the pitch their size and the gap
		 * give (a share of the size, or with abs the whole pitch in
		 * pixels, as xmlTAD is written), the letters centred in it.
		 */
		pitch = p->gap_abs ? p->gap : big * ( 1000 + p->gap ) / 1000;
		if ( pitch < asc + desc ) {
			pitch = asc + desc;
		}
		base_off = ( pitch - ( asc + desc ) ) / 2 + asc;
		if ( l->vert && p->gap_abs ) {
			/*
			 * Down the line with a pitch given in pixels -- the
			 * squares of 原稿用紙 -- the letters stand on the
			 * line's middle whatever stands beside them, so that
			 * a ruby goes into the room between the lines rather
			 * than moving its letters out of their squares.
			 */
			base_off = pitch / 2;
		}
		/*
		 * A box taller than the letters makes the line taller by what
		 * it stands past them, and by nothing more: across, on the
		 * base line, it reaches up past the letters; down the line,
		 * centred on it, either side. The gap above the letters stays
		 * above the box, so lines of boxes alone are as far apart as
		 * the gap, and no box reaches into the line above.
		 */
		up = ( oasc > asc ) ? oasc - asc : 0;
		down = ( odesc > desc ) ? odesc - desc : 0;
		pitch += up + down;
		y = page_fit(l, y, pitch);
		{
			INT	dx;

			top = l->y + page_map(l, y, &dx) - l->scroll;
			col_dx = dx;
		}
		base = top + base_off + up;
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
		hits0 = l->hits;
		for ( k = a; k < e; k++ ) {
			CONST T_TVRUN	*r = &d->run[it[k].ri];
			INT		ix = shift_x + it[k].x;
			CONST UB	*s = ( r->kind == TV_RUN_TEXT )
					     ? r->text + it[k].off : NULL;
			T_DPRECT	tb;

			run_font(l, r);
			switch ( r->kind ) {
			case TV_RUN_LINK:
			case TV_RUN_IMAGE:
			case TV_RUN_FIGURE:
				{
					/*
					 * Its box: on the base line across; down the
					 * line, centred on the line, upright.
					 */
					tb.left = ix;
					tb.top = base - it[k].asc;
					tb.right = ix + it[k].w - 2;
					tb.bottom = base + it[k].desc;
					ph_rect(l, &tb);
				}
				if ( l->thing_asking ) {
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
						INT		keep = f->zoom, keep_den = f->zoom_den;

						/* its view, magnified as the text is */
						INT		keep_max = f->open_max;
						BOOL		keep_bare = f->bare;

						f->zoom = l->zoom;
						f->zoom_den = l->zden;
						f->open_max = l->open_max;
						f->bare = l->bare;
						(void)tv_fig_draw_at(l->gid, f, &tb,
								     DZ(f->view.left), DZ(f->view.top),
								     l->src, TAD_COL_NONE, 1);
						f->zoom = keep;
						f->zoom_den = keep_den;
						f->open_max = keep_max;
						f->bare = keep_bare;
						/* its letters set the faces afresh */
						l->set_fid = 0;
					}
				} else if ( r->kind == TV_RUN_LINK ) {
					put_link(l, r->link, tb.left, tb.top,
						 tb.right - tb.left, tb.bottom - tb.top);
				} else if ( l->gid >= 0 && l->mode == LM_PLAIN
					 && top + pitch >= l->top && top <= l->bottom ) {
					tv_draw_picture(l->gid, &tb, r->href);
				}
				break;
			case TV_RUN_TEXT:
				if ( l->gid >= 0 && l->mode == LM_PLAIN
				  && top + pitch >= l->top - l->slack && top <= l->bottom + l->slack ) {
					draw_text_item(l, r, &it[k], ix, base, top, pitch);
					if ( l->detail && r->space > 0 ) {
						/* 詳細: the ends of a stretch spaced apart */
						INT	ri = it[k].ri, hh = DZ(r->size);

						if ( it[k].off == 0 && ( ri == p->first
							|| d->run[ri - 1].space != r->space ) ) {
							lg_line(l, ix, base - hh, ix, base + 2,
								FLAG_EDGE);
						}
						if ( it[k].off + it[k].len == r->len
						  && ( ri + 1 >= p->first + p->n
						       || d->run[ri + 1].space != r->space ) ) {
							lg_line(l, ix + it[k].w - 1, base - hh,
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
						lg_line(l, ix + 2, base - mh, ix + 2,
							base, 0x008080FFU);
						lg_line(l, ix + 2, base, ix + 6, base,
							0x008080FFU);
					} else if ( r->kind == TV_RUN_TAB ) {
						lg_line(l, ix + 1, base - mh / 2,
							ix + it[k].w - 2, base - mh / 2,
							0x008080FFU);
					} else if ( r->kind == TV_RUN_INDENT ) {
						lg_line(l, ix, base - mh, ix, base,
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
		/* the line that holds the place asked for: all of it */
		if ( l->mode == LM_CARET_BOX && l->hits != hits0
		  && l->hit_top == top ) {
			l->line_a = it[a].fpos;
			l->line_e = it[e - 1].fpos + it[e - 1].units;
		}
		if ( l->gid >= 0 && l->mode == LM_PLAIN
		  && top + pitch >= l->top && top <= l->bottom ) {
			box_ends(l, d, it, a, e, shift_x, base);
			draw_ruby(l, d, it, a, e, shift_x, base);
			if ( l->detail && e == n && !forced ) {
				/* the end of the paragraph */
				INT	ex = shift_x + it[e - 1].x + it[e - 1].w + 2;

				lg_line(l, ex, base - 6, ex, base, 0x0080C080U);
				lg_line(l, ex - 3, base - 3, ex, base, 0x0080C080U);
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

/* ---------------------------------------------------------------- the paragraphs kept */

/*
 * The paragraphs kept (tv_doc_cache) are counted in blocks of LB_N: a
 * block's paragraphs' heights added up, and how many of them are a
 * guess. A walk that wants nothing of a block steps over it whole, so
 * that what it costs goes with what it wants rather than with how
 * long the document is.
 */
#define LB_SHIFT	8
#define LB_N		( 1 << LB_SHIFT )

/*
 * The settings a paragraph's lines depend on, as one number: how wide
 * the lines are, the pages and columns, the magnification, what is
 * shown in detail, which way the text runs. Laid at other settings, a
 * paragraph's height is only a guess.
 */
LOCAL UW lay_key( CONST LAY *l )
{
	UW	h = 2166136261U;
	INT	v[14], i, k;

	v[0] = l->x1 - l->x0;
	v[1] = l->full_x1 - l->x0;
	v[2] = l->page_h;
	v[3] = l->mtop;
	v[4] = l->mbottom;
	v[5] = l->cols;
	v[6] = l->colstep;
	v[7] = l->zoom;
	v[8] = l->detail ? 1 : 0;
	v[9] = l->show_hidden ? 1 : 0;
	v[10] = l->vert ? 1 : 0;
	v[11] = l->x0;
	v[12] = l->zden;
	v[13] = 0x5A17;
	for ( i = 0; i < 14; i++ ) {
		for ( k = 0; k < 4; k++ ) {
			h ^= ( (UW)v[i] >> ( k * 8 ) ) & 0xFFU;
			h *= 16777619U;
		}
	}

	return h;
}

/* A paragraph's height and what it is, its block's sums kept with it */
LOCAL void laid_set( T_TVLAYC *c, INT pi, INT h, UINT state )
{
	T_TVLAID	*e = &c->e[pi];
	INT		b = pi >> LB_SHIFT;

	c->bh[b] += h - e->h;
	if ( e->state == TV_LAID_EXACT && state != TV_LAID_EXACT ) {
		c->bx[b]++;
	} else if ( e->state != TV_LAID_EXACT && state == TV_LAID_EXACT ) {
		c->bx[b]--;
	}
	if ( e->state == TV_LAID_NONE && state != TV_LAID_NONE ) {
		c->nnone--;
	}
	e->h = h;
	e->state = state;
}

/* How tall a paragraph never laid is taken to be, from how much text it holds */
LOCAL INT laid_guess( CONST LAY *l, CONST T_TVDOC *d, INT pi )
{
	CONST T_TVPARA	*p = &d->para[pi];
	INT		size = 14, bytes = 0, breaks = 0, ri, w, lines, pitch;

	for ( ri = p->first; ri < p->first + p->n && ri < d->nrun; ri++ ) {
		CONST T_TVRUN	*r = &d->run[ri];

		if ( ri == p->first && r->size > 0 ) {
			size = r->size;
		}
		if ( r->kind == TV_RUN_TEXT ) {
			bytes += r->len;
		} else if ( r->kind == TV_RUN_BREAK ) {
			breaks++;
		}
	}
	size = DZ(size);
	if ( size < 1 ) {
		size = 1;
	}
	/* letters of three bytes, each about as wide as it is tall */
	w = ( l->x1 - l->x0 ) - DZ(p->left) - DZ(p->right);
	if ( w < size ) {
		w = size;
	}
	lines = ( bytes / 3 * size + w - 1 ) / w + breaks;
	if ( lines < 1 ) {
		lines = 1;
	}
	pitch = p->gap_abs ? ( ( p->gap > 0 ) ? p->gap : size + size / 2 )
			   : size + size * p->gap / 1000;

	return lines * pitch + ( p->pargap_abs ? p->pargap : size * p->pargap / 1000 );
}

/*
 * Before a walk: at other settings than last, what was laid is a guess
 * now; a paragraph never laid is given a guess.
 */
LOCAL void laid_prepare( CONST LAY *l, CONST T_TVDOC *d )
{
	T_TVLAYC	*c = d->layc;
	UW		key = lay_key(l);
	INT		pi;

	if ( c->key != key ) {
		for ( pi = 0; pi < c->n; pi++ ) {
			if ( c->e[pi].state == TV_LAID_EXACT ) {
				laid_set(c, pi, c->e[pi].h, TV_LAID_GUESS);
			}
		}
		c->key = key;
	}
	for ( pi = 0; c->nnone > 0 && pi < c->n; pi++ ) {
		if ( c->e[pi].state == TV_LAID_NONE ) {
			laid_set(c, pi, laid_guess(l, d, pi), TV_LAID_GUESS);
		}
	}
}

/* Whether what was kept of paragraph pi is its lines now, laid at y */
LOCAL BOOL laid_fresh( CONST LAY *l, CONST T_TVDOC *d, INT pi, INT y )
{
	CONST T_TVLAID	*e = &d->layc->e[pi];

	return (BOOL)( e->state == TV_LAID_EXACT && ( l->page_h <= 0 || e->y_in == y ) );
}

/* Paragraph pi as just laid, from y to y1 */
LOCAL void laid_keep( LAY *l, CONST T_TVDOC *d, INT pi, INT y, INT y1 )
{
	T_TVLAYC	*c = d->layc;
	T_TVLAID	*e = &c->e[pi];
	CONST T_TVPARA	*p = &d->para[pi];

	e->node = p->node;
	e->gen = ( p->node != NULL ) ? p->node->gen : 0;
	e->sig = p->sig;
	e->y_in = y;
	e->pitch = l->last_pitch;
	laid_set(c, pi, y1 - y, p->nocache ? TV_LAID_GUESS : TV_LAID_EXACT);
}

/*
 * The point a walk asks about, as a place across the lines (what its
 * pieces' tops are measured in), when it asks about one
 */
LOCAL BOOL lay_point( CONST LAY *l, INT *p_y )
{
	INT	px;

	if ( l->mode == LM_CARET_AT && l->only_para < 0 ) {
		*p_y = l->ask_y;
		return TRUE;
	}
	if ( l->mode != LM_PLAIN || ( l->thing_asking && l->thing_want >= 0 ) ) {
		return FALSE;
	}
	if ( l->asking ) {
		lg_point(l, l->ask_x, l->ask_y, &px, p_y);
	} else if ( l->thing_asking ) {
		lg_point(l, l->thing_x, l->thing_y, &px, p_y);
	} else if ( l->mark_asking ) {
		lg_point(l, l->mark_x, l->mark_y, &px, p_y);
	} else {
		return FALSE;
	}

	return TRUE;
}

/*
 * Whether paragraphs a up to e (not e), from y and h tall together, are
 * wanted for what the walk is doing: drawn where the box shows them,
 * holding the place asked about, near the point asked about (the
 * paragraph it is in and the ones either side, since a point between
 * two is nearest to a line of one of them), or between the ends of a
 * selection that shows. 'hp' and 'hn' are how tall the paragraphs
 * just before and just after are.
 */
LOCAL BOOL lay_wanted( CONST LAY *l, CONST T_TVDOC *d, INT a, INT e, INT y, INT h,
		       INT hp, INT hn )
{
	INT	top = l->y + y - l->scroll, bot = top + h, py;
	BOOL	seen = (BOOL)( bot > l->top && top < l->bottom );

	if ( lay_point(l, &py) ) {
		if ( a == 0 && py < top ) {
			return TRUE;
		}
		if ( e >= d->npara && py >= bot ) {
			return TRUE;
		}
		return (BOOL)( py >= top - hp && py < bot + hn );
	}
	switch ( l->mode ) {
	case LM_CARET_BOX:
		return (BOOL)( l->want_para >= a && l->want_para < e );
	case LM_CARET_AT:
		return (BOOL)( l->only_para >= a && l->only_para < e );
	case LM_SELECT:
		return (BOOL)( l->sb_para >= a && l->sa_para < e && seen );
	default:
		if ( l->thing_asking && l->thing_want >= 0 ) {
			return (BOOL)( l->thing_want >= d->para[a].first
				    && l->thing_want < d->para[e - 1].first + d->para[e - 1].n );
		}
		return (BOOL)( l->gid >= 0 && seen );
	}
}

/*
 * The whole of the work: walk the paragraphs, break the lines, and
 * either draw or only measure. One walk does all of it so that what is
 * measured and what is drawn cannot come apart.
 *
 * With the paragraphs kept (tv_doc_cache), one that is not wanted for
 * what the walk is doing (lay_wanted) is not laid out again: it counts
 * as tall as it was laid, or as the guess. Not laid on pages, a block
 * of paragraphs none of which is wanted is stepped over whole. In
 * columns, where a paragraph's place on the paper does not follow from
 * how tall the ones before it are, every one is laid.
 */
/* Whether what is from y and h tall shows in the whole box, across the lines */
LOCAL BOOL in_box( CONST LAY *l, INT y, INT h )
{
	INT	top = l->y + y - l->scroll;

	return (BOOL)( top + h > l->vtop && top < l->vbottom );
}

/* Paragraph pi, from y and h tall, put down as one that showed, when it did */
LOCAL void seen_put( LAY *l, CONST T_TVDOC *d, INT pi, INT y, INT h )
{
	CONST T_TADNODE	*nd = d->para[pi].node;
	T_TVSEEN	*e;

	if ( l->seen == NULL || !in_box(l, y, h) ) {
		return;
	}
	if ( l->seen->n >= TV_SEEN_MAX ) {
		l->seen = NULL;			/* too many to keep: the next change draws it whole */
		return;
	}
	e = &l->seen->e[l->seen->n++];
	e->node = nd;
	e->gen = ( nd != NULL ) ? nd->gen : 0;
	e->sig = d->para[pi].sig;
	e->y = y;
	e->h = h;
}

LOCAL INT lay_out( LAY *l, CONST T_TVDOC *d )
{
	T_TVLAYC	*c = d->layc;
	INT		pi = 0, y = 0, n = d->npara;
	BOOL		keep = (BOOL)( c != NULL && c->n == n
				    && !( l->page_h > 0 && l->cols > 1 ) );
	BOOL		blocks = (BOOL)( keep && l->page_h <= 0 );

	if ( keep ) {
		laid_prepare(l, d);
	}
	while ( pi < n ) {
		INT	h, hp, hn, y1;

		if ( !keep ) {
			y = lay_para(l, d, pi, y);
			pi++;
			continue;
		}
		hp = ( pi > 0 ) ? c->e[pi - 1].h : 0;
		/* a block that shows is gone through one by one when what shows is put down */
		if ( blocks && ( pi & ( LB_N - 1 ) ) == 0 ) {
			INT	b = pi >> LB_SHIFT, e = ( pi + LB_N < n ) ? pi + LB_N : n;

			hn = ( e < n ) ? c->e[e].h : 0;
			if ( !lay_wanted(l, d, pi, e, y, c->bh[b], hp, hn)
			  && !( l->seen != NULL && in_box(l, y, c->bh[b]) ) ) {
				y += c->bh[b];
				pi = e;
				continue;
			}
		}
		h = c->e[pi].h;
		hn = ( pi + 1 < n ) ? c->e[pi + 1].h : 0;
		if ( laid_fresh(l, d, pi, y) && !lay_wanted(l, d, pi, pi + 1, y, h, hp, hn) ) {
			l->last_pitch = c->e[pi].pitch;
			seen_put(l, d, pi, y, h);
			y += h;
			pi++;
			continue;
		}
		if ( !laid_fresh(l, d, pi, y) && !lay_wanted(l, d, pi, pi + 1, y, h, hp, hn) ) {
			seen_put(l, d, pi, y, h);
			y += h;			/* the guess */
			pi++;
			continue;
		}
		y1 = lay_para(l, d, pi, y);
		laid_keep(l, d, pi, y, y1);
		seen_put(l, d, pi, y, y1 - y);
		y = y1;
		pi++;
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
	l->detail = d->detail;
	if ( d->zoom > 0 && d->zoom_den > 0 ) {
		l->zoom = d->zoom;
		l->zden = d->zoom_den;
	} else {
		l->zoom = l->zden = 8;
	}
	l->depth = d->depth;
	l->open_max = ( d->open_max > 0 ) ? d->open_max : TV_MAX_DEPTH;
	l->bare = d->bare;
	l->show_hidden = d->show_hidden;
	if ( d->vert ) {
		/*
		 * 縦書き: the lines run down the box and follow one another
		 * from its right edge; on pages, a page is as wide as the
		 * paper, its first line inside the right margin, and each
		 * line runs between the top and the bottom margins.
		 */
		l->vert = TRUE;
		l->vr = r->right;
		l->x0 = r->top;
		l->x1 = r->bottom;
		l->y = 0;
		if ( d->page_h > 0
		  && d->page_h - d->margin[0] - d->margin[2] >= 32
		  && r->bottom - r->top - d->margin[1] - d->margin[3] >= 32 ) {
			l->page_h = d->page_h;
			l->mtop = d->margin[2];
			l->mbottom = d->margin[0];
			l->x0 += d->margin[1];
			l->x1 -= d->margin[3];
			l->full_x1 = l->x1;
		}
	} else if ( d->page_h > 0
	  && d->page_h - d->margin[1] - d->margin[3] >= 32
	  && r->right - r->left - d->margin[0] - d->margin[2] >= 32 ) {
		l->page_h = d->page_h;
		l->mtop = d->margin[1];
		l->mbottom = d->margin[3];
		l->x0 += d->margin[0];
		l->x1 -= d->margin[2];
		l->full_x1 = l->x1;
	}
	if ( l->page_h > 0 ) {
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
	if ( l->vert ) {
		/* what is worth drawing, across the lines: the box's width */
		l->top = 0;
		l->bottom = r->right - r->left;
	}
	l->vtop = l->top;
	l->vbottom = l->bottom;
	l->slack = INK_SLACK;
	l->scroll = scroll_y;
	l->fid = fn_system();
	l->found = -1;
	l->best = -1;
	l->only_para = -1;
	l->mode = LM_PLAIN;
	l->set_fid = 0;
	l->set_px = -1;
	l->pageno = -1;
	l->npages = 0;
	if ( l->full_x1 == 0 ) {
		l->full_x1 = l->x1;
	}
}

/* An overlay of a page shown as the page is: magnified as it is, and as deep */
LOCAL void lay_view( LAY *m, CONST LAY *l )
{
	m->zoom = l->zoom;
	m->zden = l->zden;
	m->depth = l->depth;
	m->open_max = l->open_max;
	m->bare = l->bare;
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
	lg_point(&l, x, y, &l.ask_x, &l.ask_y);
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
	ph_rect(&l, &l.box);
	*box = l.box;

	return E_OK;
}

/*
 * The line a place is on: where the place is drawn, and the first and
 * the last place of the line. A place where a line is broken belongs
 * to the line after, as tv_doc_caret_box draws it.
 */
EXPORT ER tv_doc_caret_line( CONST T_TVDOC *d, CONST T_DPRECT *r,
			     INT scroll_y, CONST T_TAD *src, INT para, INT pos,
			     T_DPRECT *box, INT *p_head, INT *p_end )
{
	LAY	l;

	if ( d == NULL || r == NULL || box == NULL || p_head == NULL
	  || p_end == NULL ) {
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
	ph_rect(&l, &l.box);
	*box = l.box;
	*p_head = l.line_a;
	*p_end = l.line_e;

	return E_OK;
}

/*
 * The place under a point, looked for only among the places from
 * 'head' to 'end' of paragraph 'para': one line, as tv_doc_caret_line
 * gives it.
 */
EXPORT ER tv_doc_caret_in( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			   CONST T_TAD *src, INT para, INT head, INT end,
			   INT x, INT y, INT *p_pos )
{
	LAY	l;

	if ( d == NULL || r == NULL || p_pos == NULL || para < 0 ) {
		return E_PAR;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	l.mode = LM_CARET_AT;
	l.only_para = para;
	l.only_a = head;
	l.only_e = end;
	lg_point(&l, x, y, &l.ask_x, &l.ask_y);
	(void)lay_out(&l, d);
	if ( l.best < 0 ) {
		return E_NOEXS;
	}
	*p_pos = l.best_pos;

	return E_OK;
}

/*
 * The paragraphs laid out kept with d, those of 'from' still the same
 * taken over: the two are matched by their <p>s, in order, so that a
 * paragraph put in or taken out does not make those after it count as
 * changed. One whose <p> has changed since, or whose settings or the
 * look of letters at its head have, keeps its height only as a guess.
 */
EXPORT ER tv_doc_cache( T_TVDOC *d, CONST T_TVDOC *from )
{
	T_TVLAYC	*c;
	INT		n, nb, i = 0, j = 0, on;
	SZ		sz;

	if ( d == NULL ) {
		return E_PAR;
	}
	n = d->npara;
	nb = ( n + LB_N - 1 ) >> LB_SHIFT;
	sz = sizeof(T_TVLAYC) + sizeof(T_TVLAID) * (SZ)( ( n > 0 ) ? n : 1 )
	   + sizeof(INT) * 2 * (SZ)( nb + 1 );
	c = (T_TVLAYC *)Kmalloc(sz);
	if ( c == NULL ) {
		return E_NOMEM;			/* every paragraph laid every time, as before */
	}
	knl_memset(c, 0, sz);
	c->n = n;
	c->nb = nb;
	c->bh = (INT *)&c->e[( n > 0 ) ? n : 1];
	c->bx = c->bh + nb + 1;
	if ( from != NULL && from->layc != NULL && from->layc->n == from->npara
	  && from->serial != 0 && from->serial == d->serial ) {
		CONST T_TVLAYC	*o = from->layc;

		on = from->npara;
		c->key = o->key;
		/* what the last drawing showed is the same box's still: the next drawing differs from it */
		c->shown = from->layc->shown;
		from->layc->shown = NULL;
		while ( i < n && j < on ) {
			CONST T_TADNODE	*a = d->para[i].node, *b = from->para[j].node;
			INT		k;

			if ( a != NULL && a == b ) {
				c->e[i++] = o->e[j++];
				continue;
			}
			/* paragraphs taken out: the old ones skipped to where this one is */
			for ( k = 1; k <= 32 && j + k < on && from->para[j + k].node != a; k++ ) {
			}
			if ( a != NULL && j + k < on && k <= 32 ) {
				j += k;
				continue;
			}
			/* paragraphs put in: the new ones left to be guessed */
			for ( k = 1; k <= 32 && i + k < n && d->para[i + k].node != b; k++ ) {
			}
			if ( b != NULL && i + k < n && k <= 32 ) {
				i += k;
				continue;
			}
			/* one in place of another: its height kept as the guess */
			c->e[i] = o->e[j];
			if ( c->e[i].state == TV_LAID_EXACT ) {
				c->e[i].state = TV_LAID_GUESS;
			}
			i++;
			j++;
		}
	}
	for ( i = 0; i < n; i++ ) {
		T_TVLAID	*e = &c->e[i];
		CONST T_TVPARA	*p = &d->para[i];

		if ( e->state == TV_LAID_EXACT
		  && ( p->nocache || p->node == NULL || e->node != p->node
		    || e->gen != p->node->gen || e->sig != p->sig ) ) {
			e->state = TV_LAID_GUESS;	/* changed: its height a guess */
		}
		if ( e->state == TV_LAID_NONE ) {
			e->h = 0;
			c->nnone++;
		}
		c->bh[i >> LB_SHIFT] += e->h;
		if ( e->state != TV_LAID_EXACT ) {
			c->bx[i >> LB_SHIFT]++;
		}
	}
	if ( d->layc != NULL ) {
		if ( d->layc->shown != NULL && c->shown == NULL ) {
			c->shown = d->layc->shown;	/* made again in place: the same box's */
		} else if ( d->layc->shown != NULL ) {
			Kfree(d->layc->shown);
		}
		Kfree(d->layc);
	}
	d->layc = c;

	return E_OK;
}

EXPORT INT tv_doc_settle( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			  CONST T_TAD *src, INT budget, INT *p_shift, INT *p_laid )
{
	T_TVLAYC	*c;
	LAY		l;
	INT		pi = 0, y = 0, y_old = 0, left = 0, shift = 0, laid = 0, n;
	BOOL		out = FALSE;
	SYSTIM		t0, t;

	if ( p_shift != NULL ) {
		*p_shift = 0;
	}
	if ( p_laid != NULL ) {
		*p_laid = 0;
	}
	if ( d == NULL || r == NULL || ( c = d->layc ) == NULL || c->n != d->npara ) {
		return 0;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	if ( l.page_h > 0 && l.cols > 1 ) {
		return 0;			/* in columns every paragraph is laid anyway */
	}
	laid_prepare(&l, d);
	n = d->npara;
	(void)tk_get_otm(&t0);
	while ( pi < n ) {
		T_TVLAID	*e = &c->e[pi];
		INT		h_old;

		if ( ( pi & ( LB_N - 1 ) ) == 0 ) {
			INT	b = pi >> LB_SHIFT;

			if ( c->bx[b] == 0 || out ) {
				/* nothing in it to lay, or no time left: as tall as it is */
				left += c->bx[b];
				y += c->bh[b];
				y_old += c->bh[b];
				pi = ( pi + LB_N < n ) ? pi + LB_N : n;
				continue;
			}
		}
		h_old = e->h;
		if ( e->state != TV_LAID_EXACT && !d->para[pi].nocache ) {
			(void)tk_get_otm(&t);
			if ( (INT)( t.lo - t0.lo ) < budget ) {
				INT	y1 = lay_para(&l, d, pi, y);

				laid_keep(&l, d, pi, y, y1);
				if ( y_old + h_old <= scroll_y ) {
					shift += ( y1 - y ) - h_old;	/* above what is shown */
				}
				y = y1;
				y_old += h_old;
				laid++;
				pi++;
				continue;
			}
			out = TRUE;
			left++;
		}
		y += h_old;
		y_old += h_old;
		pi++;
	}
	if ( p_shift != NULL ) {
		*p_shift = shift;
	}
	if ( p_laid != NULL ) {
		*p_laid = laid;
	}

	return left;
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
	T_DPRECT	cut;
	BOOL		narrowed = FALSE;
	T_TVSHOWN	*sh = NULL;
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
		cut = env.visible;
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
	if ( gid >= 0 && !d->bare ) {
		dp_fill_rect(gid, r, ( paper == TV_PAPER_LOOK )
				     ? wm_look(WM_LOOK_WORK) : paper);
	}
	lay_init(&l, gid, d, r, scroll_y, src);
	/*
	 * Only part of the box drawn again: only the lines that reach into
	 * it are laid and drawn, across the lines (down the box written
	 * downwards, where a place of the walk is that far left of the
	 * box's right edge). Those outside it keep what is on the screen.
	 */
	if ( narrowed ) {
		INT	a = l.vert ? l.vr - cut.right : cut.top;
		INT	e = l.vert ? l.vr - cut.left : cut.bottom;

		if ( a > l.top )    l.top = a;
		if ( e < l.bottom ) l.bottom = e;
		if ( l.top != l.vtop || l.bottom != l.vbottom ) {
			l.slack = BAND_EDGE;
		}
	}
	/* what shows in the box put down, for what a change makes differ (tv_doc_band) */
	if ( gid >= 0 && d->layc != NULL && d->layc->n == d->npara
	  && !( l.page_h > 0 && l.cols > 1 ) ) {
		if ( d->layc->shown == NULL ) {
			d->layc->shown = (T_TVSHOWN *)Kmalloc(sizeof(T_TVSHOWN));
		}
		sh = d->layc->shown;
		if ( sh != NULL ) {
			sh->valid = FALSE;
			sh->n = 0;
			l.seen = sh;
		}
	}
	if ( gid >= 0 && l.page_h > 0 && tv_doc_has_sheet(d) ) {
		/*
		 * What is laid over the whole sheet goes under the text, on
		 * every page there is: how many there are is known from
		 * laying the text out once without drawing it.
		 */
		LAY	m;

		lay_init(&m, -1, d, r, scroll_y, src);
		sheets(&l, d, lay_out(&m, d));
	}
	h = lay_out(&l, d);
	if ( sh != NULL && l.seen != NULL ) {
		sh->valid = TRUE;
		sh->box = *r;
		sh->scroll = scroll_y;
		sh->key = lay_key(&l);
		sh->height = h;
	}
	if ( gid >= 0 && l.page_h > 0 && d->over_on != 0 ) {
		overlays(&l, d, h);
	}
	if ( narrowed ) {
		dp_set_visible(gid, &env.visible);
	}

	return h;
}

/*
 * Whether a paragraph shows an object opened in it -- a link taller than
 * its band, or a figure with links in it: what is inside may be what
 * changed, so the box is drawn whole.
 */
LOCAL BOOL para_opens( LAY *l, CONST T_TVDOC *d, INT pi )
{
	CONST T_TVPARA	*p = &d->para[pi];
	INT		ri, w, h, k;
	T_VOBJ		v;

	for ( ri = p->first; ri < p->first + p->n && ri < d->nrun; ri++ ) {
		CONST T_TVRUN	*r = &d->run[ri];

		if ( r->kind == TV_RUN_FIGURE ) {
			CONST T_TVFIG	*f = ( r->link >= 0 && r->link < d->nfig ) ? d->figs[r->link] : NULL;

			for ( k = 0; f != NULL && k < f->nsh; k++ ) {
				if ( f->sh[k].kind == TV_SH_LINK ) {
					return TRUE;
				}
			}
			continue;
		}
		if ( r->kind != TV_RUN_LINK || l->src == NULL
		  || tad_lnk_get(l->src, r->link, &v) < E_OK ) {
			continue;
		}
		thing_size(l, r, &w, &h);
		if ( h <= 0 ) {
			continue;		/* hidden */
		}
		if ( v.chsz > 0 ) {
			v.chsz = DZ(v.chsz);
		}
		if ( h > om_band_h(&v) + OM_OPEN_SLACK ) {
			return TRUE;
		}
	}

	return FALSE;
}

EXPORT BOOL tv_doc_band( CONST T_TVDOC *d, CONST T_DPRECT *r, INT scroll_y,
			 CONST T_TAD *src, T_DPRECT *band )
{
	T_TVLAYC	*c;
	CONST T_TVSHOWN	*s;
	LAY		l;
	INT		pi = 0, y = 0, n, j = 0, top, a = 0x7FFFFFFF, e = -0x7FFFFFFF;
	BOOL		to_end = FALSE;

	if ( d == NULL || r == NULL || band == NULL || ( c = d->layc ) == NULL
	  || c->n != d->npara || ( s = c->shown ) == NULL || !s->valid
	  || s->scroll != scroll_y || s->box.left != r->left || s->box.top != r->top
	  || s->box.right != r->right || s->box.bottom != r->bottom ) {
		return FALSE;
	}
	lay_init(&l, -1, d, r, scroll_y, src);
	if ( ( l.page_h > 0 && ( l.cols > 1 || tv_doc_has_sheet(d) || d->over_on != 0 ) )
	  || lay_key(&l) != s->key ) {
		return FALSE;
	}
	laid_prepare(&l, d);
	n = d->npara;
	while ( pi < n ) {
		CONST T_TADNODE	*nd;
		CONST T_TVSEEN	*o;
		INT		h;

		top = l.y + y - l.scroll;
		if ( top >= l.vbottom ) {
			break;
		}
		if ( ( pi & ( LB_N - 1 ) ) == 0 && l.page_h <= 0
		  && top + c->bh[pi >> LB_SHIFT] <= l.vtop ) {
			y += c->bh[pi >> LB_SHIFT];		/* a block above the box */
			pi = ( pi + LB_N < n ) ? pi + LB_N : n;
			continue;
		}
		h = c->e[pi].h;
		if ( top + h <= l.vtop ) {
			y += h;
			pi++;
			continue;
		}
		/* it shows: laid now if it changed, as the drawing will lay it */
		if ( !laid_fresh(&l, d, pi, y) ) {
			INT	y1 = lay_para(&l, d, pi, y);

			laid_keep(&l, d, pi, y, y1);
			h = y1 - y;
		}
		if ( para_opens(&l, d, pi) ) {
			return FALSE;
		}
		nd = d->para[pi].node;
		o = ( j < s->n ) ? &s->e[j] : NULL;
		if ( o != NULL && o->node == nd && o->y == y && o->h == h ) {
			if ( o->gen != ( ( nd != NULL ) ? nd->gen : 0 ) || o->sig != d->para[pi].sig ) {
				if ( top < a )     a = top;	/* changed in its place */
				if ( top + h > e ) e = top + h;
			}
			j++;
		} else {
			/* moved, or another in its place: from where either was, to the end */
			INT	ot = ( o != NULL ) ? l.y + o->y - l.scroll : top;

			if ( top < a ) a = top;
			if ( ot < a )  a = ot;
			to_end = TRUE;
			break;
		}
		y += h;
		pi++;
	}
	if ( !to_end && j < s->n ) {
		/* more showed before, below what shows now */
		top = l.y + s->e[j].y - l.scroll;
		if ( top < a ) a = top;
		to_end = TRUE;
	}
	if ( to_end ) {
		e = l.vbottom;
	}
	if ( a > e ) {
		band->left = band->top = band->right = band->bottom = 0;	/* nothing changed that shows */
		return TRUE;
	}
	a -= BAND_EDGE;
	e += BAND_EDGE;
	if ( a < l.vtop )    a = l.vtop;
	if ( e > l.vbottom ) e = l.vbottom;
	/* across the whole box, and what is beside it: whoever draws it cuts it to its window */
	if ( l.vert ) {
		band->left = l.vr - e;
		band->right = l.vr - a;
		band->top = -0x4000;
		band->bottom = 0x4000;
	} else {
		band->top = a;
		band->bottom = e;
		band->left = -0x4000;
		band->right = 0x4000;
	}

	return TRUE;
}

/* The paper a document names, or A4 upright when it names none */
LOCAL void paper_of( CONST T_TVDOC *d, INT *p_w, INT *p_h )
{
	*p_w = ( d->paper_w > 0 ) ? d->paper_w : 595;
	*p_h = ( d->paper_h > 0 ) ? d->paper_h : 842;
}

/* The document as laid on its paper: a copy of the model with its pages set */
LOCAL void on_paper( CONST T_TVDOC *d, T_TVDOC *c, T_DPRECT *r )
{
	INT	w, h;

	paper_of(d, &w, &h);
	*c = *d;
	c->layc = NULL;			/* laid another way: none of it kept */
	c->page_h = d->vert ? w : h;
	r->left = 0;
	r->top = 0;
	r->right = w;
	r->bottom = h;
}

EXPORT INT tv_doc_pages( CONST T_TVDOC *d, CONST T_TAD *src )
{
	T_TVDOC		c;
	T_DPRECT	r;
	LAY		l;
	INT		h, per;

	if ( d == NULL ) {
		return 0;
	}
	on_paper(d, &c, &r);
	lay_init(&l, -1, &c, &r, 0, src);
	h = lay_out(&l, &c);
	per = l.page_h;
	if ( per <= 0 ) {
		return 1;
	}

	return ( h > 0 ) ? ( h + per - 1 ) / per : 1;
}

EXPORT ER tv_doc_draw_page( INT gid, CONST T_TVDOC *d, INT pg,
			    CONST T_DPRECT *sheet, CONST T_TAD *src, UW paper )
{
	T_TVDOC		c;
	T_DPRECT	r;

	if ( d == NULL || sheet == NULL || pg < 0 ) {
		return E_PAR;
	}
	on_paper(d, &c, &r);
	r.left = sheet->left;
	r.top = sheet->top;
	r.right = sheet->left + ( r.right - r.left );
	r.bottom = sheet->top + ( r.bottom - r.top );
	(void)tv_doc_draw(gid, &c, &r, pg * c.page_h, src, paper);

	return E_OK;
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
	if ( d->vert ) {
		/* written downwards: its lines that long, and how wide it comes to */
		r.right = 0;
		r.bottom = width;
	}
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
 * the era, 102 the whole year, 110/111 the month (two figures), 112/113 its name in small
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
	case 102:
		/* the whole year, as ヘッダ/フッタ's %y asks for it */
		return num_text(tm.tm_year + 1900, out);
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
 * Where page 'pg' of the paper is in the box: below the one before, or
 * written downwards, to the left of it.
 */
LOCAL void page_rect( CONST LAY *l, CONST T_TVDOC *d, INT pg, T_DPRECT *pr )
{
	INT	at = pg * l->page_h - l->scroll;

	if ( l->vert ) {
		pr->right = l->vr - at;
		pr->left = pr->right - l->page_h;
		pr->top = l->x0 - d->margin[1];
		pr->bottom = l->full_x1 + d->margin[3];
		return;
	}
	pr->left = l->x0 - d->margin[0];
	pr->right = l->full_x1 + d->margin[2];
	pr->top = l->y + at;
	pr->bottom = pr->top + l->page_h;
}

/* Whether page 'pg' is where anything is drawn */
LOCAL BOOL page_seen( CONST LAY *l, INT pg )
{
	INT	at = l->y + pg * l->page_h - l->scroll;

	return (BOOL)( at <= l->bottom && at + l->page_h >= l->top );
}

/*
 * One overlay laid in a band of a page: each paragraph on the band's one
 * line, placed by its own alignment, so that what stands at the left, in
 * the middle and at the right of a header share the line. A line as tall
 * as the band, as a tab format's height makes it, starts at its top;
 * a lower one is put in the middle of it. A header is written across
 * whichever way the text runs.
 */
LOCAL void band_draw( LAY *l, CONST T_TVDOC *o, CONST T_DPRECT *band,
		      BOOL at_foot, INT pg )
{
	LAY		m;
	T_DPRECT	r = *band;
	INT		pi, lh = 0, top = band->top, h = band->bottom - band->top;

	for ( pi = 0; pi < o->npara; pi++ ) {
		lay_init(&m, -1, o, &r, 0, l->src);
		lay_view(&m, l);
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
		lay_view(&m, l);
		m.top = l->vert ? r.top - 4096 : l->top;
		m.bottom = l->vert ? r.bottom + 4096 : l->bottom;
		m.y = top + ( ( h > lh ) ? ( h - lh ) / 2 : 0 );
		m.pageno = pg;
		m.npages = l->npages;
		(void)lay_para(&m, o, pi, 0);
	}
}

/*
 * Whether an overlay is laid over the whole sheet rather than in a band:
 * one that holds figures and no letters -- the squares of 原稿用紙, a
 * frame round the page. Its figures are in the paper's own places, from
 * the corner of the sheet.
 */
LOCAL BOOL sheet_like( CONST T_TVDOC *o )
{
	INT	i;

	if ( o == NULL || o->nfig <= 0 ) {
		return FALSE;
	}
	for ( i = 0; i < o->nrun; i++ ) {
		if ( o->run[i].kind == TV_RUN_TEXT || o->run[i].kind == TV_RUN_LINK
		  || o->run[i].kind == TV_RUN_PAGENO || o->run[i].kind == TV_RUN_VAR ) {
			return FALSE;
		}
	}

	return TRUE;
}

/* Whether overlay k is put on page pg */
LOCAL BOOL over_on_page( CONST T_TVDOC *d, INT k, INT pg )
{
	UINT	p = d->over_pages[k];

	return (BOOL)( d->over[k] != NULL && ( d->over_on & ( 1U << k ) ) != 0
		    && !( p == 1 && ( pg + 1 ) % 2 == 0 )
		    && !( p == 2 && ( pg + 1 ) % 2 != 0 ) );
}

EXPORT BOOL tv_doc_has_sheet( CONST T_TVDOC *d )
{
	INT	k;

	for ( k = 0; d != NULL && k < TV_MAX_OVER; k++ ) {
		if ( d->over[k] != NULL && ( d->over_on & ( 1U << k ) ) != 0
		  && sheet_like(d->over[k]) ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* How many pages a text that came to h takes */
LOCAL INT page_count( CONST LAY *l, INT h )
{
	return ( l->page_h > 0 && h > 0 ) ? ( h + l->page_h - 1 ) / l->page_h : 1;
}

/*
 * The overlays laid over the whole sheet, on every page on the screen,
 * before the text: each figure from the corner of the paper, magnified
 * as the text is.
 */
LOCAL void sheets( LAY *l, CONST T_TVDOC *d, INT h )
{
	INT	pages = page_count(l, h), pg, k, i;

	for ( pg = 0; pg < pages; pg++ ) {
		T_DPRECT	pr;

		if ( !page_seen(l, pg) ) {
			continue;
		}
		page_rect(l, d, pg, &pr);
		for ( k = 0; k < TV_MAX_OVER; k++ ) {
			CONST T_TVDOC	*o = d->over[k];

			if ( !over_on_page(d, k, pg) || !sheet_like(o) ) {
				continue;
			}
			for ( i = 0; i < o->nfig; i++ ) {
				T_TVFIG	*f = o->figs[i];
				INT	keep = f->zoom, keep_den = f->zoom_den;

				INT	keep_max = f->open_max;
				BOOL	keep_bare = f->bare;

				f->zoom = l->zoom;
				f->zoom_den = l->zden;
				f->open_max = l->open_max;
				f->bare = l->bare;
				(void)tv_fig_draw_at(l->gid, f, &pr, 0, 0, l->src,
						     TAD_COL_NONE, 1);
				f->zoom = keep;
				f->zoom_den = keep_den;
				f->open_max = keep_max;
				f->bare = keep_bare;
			}
		}
	}
	l->set_fid = 0;
}

/*
 * 用紙オーバーレイ: each overlay the document puts on, on the pages it is
 * for, in the top margin of every page on the screen -- the footer (1),
 * and one whose text begins with a fill line, in the bottom margin.
 * Those laid over the whole sheet were drawn before the text.
 */
LOCAL void overlays( LAY *l, CONST T_TVDOC *d, INT h )
{
	INT	pages = page_count(l, h), pg, k;

	l->npages = pages;
	for ( pg = 0; pg < pages; pg++ ) {
		T_DPRECT	pr, band;

		if ( !page_seen(l, pg) ) {
			continue;
		}
		page_rect(l, d, pg, &pr);
		for ( k = 0; k < TV_MAX_OVER; k++ ) {
			CONST T_TVDOC	*o = d->over[k];
			BOOL		foot;

			if ( !over_on_page(d, k, pg) || sheet_like(o) ) {
				continue;
			}
			foot = (BOOL)( k == 1 || o->foot );
			/* the band: the top or the bottom margin, inside the side ones */
			if ( l->vert ) {
				band.left = pr.left + d->margin[0];
				band.right = pr.right - d->margin[2];
			} else {
				band.left = l->x0;
				band.right = l->full_x1;
			}
			if ( foot ) {
				band.bottom = pr.bottom;
				band.top = pr.bottom - d->margin[3];
			} else {
				band.top = pr.top;
				band.bottom = pr.top + d->margin[1];
			}
			band_draw(l, o, &band, foot, pg);
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
	if ( !l.detail || l.vert ) {
		return TV_MARK_NONE;		/* down the line no marks are shown */
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

/* tv_paint and tv_shape, which make a window's picture and its shape, are in docpaint.c */
