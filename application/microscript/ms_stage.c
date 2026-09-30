/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_stage.c
 *	The stage: the segments, what they look like, and what is under the
 *	pointer
 *
 *	The window shows the figure from a point of it, the view: at first
 *	its top left corner, after a SCENE the top left corner of the scene.
 *	A segment keeps its place in the figure's coordinates; what the
 *	script sees (.X, .Y, the places of events) is the window's. The
 *	ground is drawn first, then the segments that are shown, the one
 *	brought forward last on top. A segment is drawn from the piece of
 *	figure it was made from, moved by how far it is from where the figure
 *	has it. A segment whose text the script wrote is drawn without its
 *	own texts and with the text written over it. The copies MOVE ... DUP
 *	leaves behind are shown and hidden with their segment.
 *
 *	Several segments may have one name (a figure may hold four of a
 *	card): the first has the name as its key, the others the name and
 *	their number.
 */

#include "ms.h"
#include "ms_run.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SEG_BUCKETS	1024

static MSSEG	*first, *last;
static MSSEG	*bucket[SEG_BUCKETS];
static INT	zseq;
static UW	paper;
static double	vx, vy;			/* the point of the figure at the window's top left */

void st_view( double x, double y )
{
	vx = x;
	vy = y;
}

double st_vx( void ) { return vx; }
double st_vy( void ) { return vy; }

/* Whether a segment is shown: a copy with the one it was copied from */
BOOL st_shown( const MSSEG *s )
{
	return ( s->dupof != NULL ) ? s->dupof->visible : s->visible;
}

typedef struct byname {
	MSSTR		*name;
	MSSEG		**list;
	INT		n, cap;
	struct byname	*next;
} BYNAME;

static BYNAME	*names;

static BYNAME *byname_of( MSSTR *name, BOOL make )
{
	BYNAME	*b;

	for ( b = names; b != NULL; b = b->next ) if ( b->name == name ) return b;
	if ( !make ) return NULL;
	b = ms_alloc(sizeof(BYNAME));
	b->name = name;
	b->next = names;
	names = b;
	return b;
}

void st_init( MSFIG *f, UW ground )
{
	(void)f;
	paper = ground;
}

MSSEG *st_first( void )
{
	return first;
}

MSSEG *st_seg( MSSTR *key )
{
	MSSEG	*s;

	if ( key == NULL ) return NULL;
	for ( s = bucket[(UW)( (UBINT)key >> 4 ) % SEG_BUCKETS]; s != NULL; s = s->hnext ) {
		if ( s->key == key ) return s;
	}
	return NULL;
}

INT st_seg_count( MSSTR *name )
{
	BYNAME	*b = byname_of(name, FALSE);

	return ( b != NULL ) ? b->n : 0;
}

MSSEG *st_seg_occ( MSSTR *name, INT occ )
{
	BYNAME	*b = byname_of(name, FALSE);

	if ( b == NULL || b->n == 0 ) return st_seg(name);
	return ( occ >= 0 && occ < b->n ) ? b->list[occ] : b->list[0];
}

void st_text_set( MSSEG *s, const UW *c, INT n )
{
	ms_free(s->tx);
	s->tx = ms_alloc(sizeof(UW) * (size_t)( n + 1 ));
	if ( n > 0 && c != NULL ) memcpy(s->tx, c, sizeof(UW) * (size_t)n);
	s->tx[n] = 0;
	s->ntx = n + 1;
}

static void text_from_utf8( MSSEG *s, const char *u )
{
	INT	n = ( u != NULL ) ? (INT)strlen(u) : 0, k = 0, i;
	UW	*c = ms_alloc(sizeof(UW) * (size_t)( n + 1 ));

	for ( i = 0; i < n; ) {
		UW	cp;
		INT	d = ms_utf8_dec((const UB *)u + i, n - i, &cp);

		if ( d <= 0 ) break;
		c[k++] = cp;
		i += d;
	}
	st_text_set(s, c, k);
	ms_free(c);
}

MSSEG *st_add( MSSTR *name, MSFSEG *fs )
{
	MSSEG	*s = ms_alloc(sizeof(MSSEG));
	BYNAME	*b = byname_of(name, TRUE);
	UW	h;

	s->name = name;
	if ( b->n == 0 ) {
		s->key = name;
	} else {
		char	k[160];

		snprintf(k, sizeof(k), "%s%d", name->s, b->n);
		s->key = ms_intern_z(k);
	}
	if ( b->n >= b->cap ) {
		b->cap = ( b->cap == 0 ) ? 4 : b->cap * 2;
		b->list = ms_realloc(b->list, sizeof(MSSEG *) * (size_t)b->cap);
	}
	b->list[b->n++] = s;
	s->fs = fs;
	if ( fs != NULL ) {
		s->x = s->x0 = s->hx = fs->box.l;
		s->y = s->y0 = s->hy = fs->box.t;
		s->w = ( fs->box.r - fs->box.l > 1 ) ? fs->box.r - fs->box.l : 1;
		s->h = ( fs->box.b - fs->box.t > 1 ) ? fs->box.b - fs->box.t : 1;
		s->lox = fs->gl - fs->box.l;
		s->loy = fs->gt - fs->box.t;
		text_from_utf8(s, fs->text);
	} else {
		text_from_utf8(s, "");
	}
	s->pid = -1;
	s->tfcol = -1;
	s->tbcol = 0xFFFFFF;
	s->tsize = ( fs != NULL && fs->textsize > 0 ) ? fs->textsize : 14;
	s->z = ++zseq;
	if ( last != NULL ) last->next = s; else first = s;
	last = s;
	h = (UW)( (UBINT)s->key >> 4 ) % SEG_BUCKETS;
	s->hnext = bucket[h];
	bucket[h] = s;
	return s;
}

void st_remove( MSSEG *s )
{
	MSSEG	**pp;
	BYNAME	*b;
	UW	h = (UW)( (UBINT)s->key >> 4 ) % SEG_BUCKETS;
	INT	i;
	MSSEG	*q, *nx;

	/* the copies it left go with it */
	for ( q = first; q != NULL; q = nx ) {
		nx = q->next;
		if ( q->dupof == s ) { st_remove(q); nx = first; }
	}
	if ( st_input_seg() == s ) st_input_end();
	for ( pp = &bucket[h]; *pp != NULL; pp = &(*pp)->hnext ) {
		if ( *pp == s ) { *pp = s->hnext; break; }
	}
	for ( pp = &first; *pp != NULL; pp = &(*pp)->next ) {
		if ( *pp == s ) {
			*pp = s->next;
			break;
		}
	}
	last = NULL;
	for ( pp = &first; *pp != NULL; pp = &(*pp)->next ) last = *pp;
	b = byname_of(s->name, FALSE);
	if ( b != NULL ) {
		for ( i = 0; i < b->n; i++ ) {
			if ( b->list[i] == s ) {
				memmove(&b->list[i], &b->list[i + 1], sizeof(MSSEG *) * (size_t)( b->n - i - 1 ));
				b->n--;
				break;
			}
		}
	}
	ms_free(s->tx);
	ms_free(s);
}

void st_front( MSSEG *s )
{
	s->z = ++zseq;
}

/*
 * Several segments of one name in the order they stand, left to right
 * and then top to bottom, rather than the order the figure has them:
 * the script numbers them so.
 */
void st_sort_by_place( void )
{
	BYNAME	*b;
	INT	i, j;

	for ( b = names; b != NULL; b = b->next ) {
		if ( b->n < 2 ) continue;
		for ( i = 1; i < b->n; i++ ) {
			MSSEG	*x = b->list[i];

			for ( j = i; j > 0 && ( b->list[j - 1]->x > x->x
					     || ( b->list[j - 1]->x == x->x && b->list[j - 1]->y > x->y ) ); j-- ) {
				b->list[j] = b->list[j - 1];
			}
			b->list[j] = x;
		}
	}
}

MSSEG *st_hit( INT x, INT y, BOOL (*has)( MSSEG * ) )
{
	MSSEG	*s, *best = NULL;
	INT	bz = -1;

	for ( s = first; s != NULL; s = s->next ) {
		double	wx = s->x - vx, wy = s->y - vy;

		if ( s->dupof != NULL || !s->visible || s->z < bz ) continue;
		if ( x < wx || x >= wx + s->w || y < wy || y >= wy + s->h ) continue;
		if ( has != NULL && !has(s) ) continue;
		bz = s->z;
		best = s;
	}
	return best;
}

/* ---------------------------------------------------------------- drawing */

/* The one whose look a segment has: SETSEG makes one look like another */
static MSSEG *look_of( MSSEG *s )
{
	INT	guard = 0;

	while ( s->shared != NULL && guard++ < 64 ) s = s->shared;
	return s;
}

static UW colour_of( double c, UW dflt )
{
	if ( !isfinite(c) || c < 0 ) return dflt;
	return (UW)ms_int32(c) & 0x00FFFFFF;
}

/* The width of characters in letters of px */
static INT chars_width( const UW *c, INT n, INT px, INT gap )
{
	UB	buf[1024];
	INT	k = 0, i;

	if ( n <= 0 ) return 0;
	for ( i = 0; i < n && k < (INT)sizeof(buf) - 5; i++ ) k += ms_utf8_enc(c[i], buf + k);
	buf[k] = 0;
	return dp_text_width(buf, px) + gap * n;
}

/* One line of the text drawn at (x, y top), as much as fits in w; answers the characters drawn */
static INT text_line( INT gid, const UW *c, INT n, INT x, INT y, INT w, INT px, UW col, INT gap )
{
	UB	buf[1024];
	INT	k = 0, i, taken = 0;

	for ( i = 0; i < n && k < (INT)sizeof(buf) - 5; i++ ) {
		INT	m = ms_utf8_enc(c[i], buf + k);

		buf[k + m] = 0;
		if ( i > 0 && dp_text_width(buf, px) + gap * i > w ) break;
		k += m;
		taken = i + 1;
	}
	buf[k] = 0;
	if ( gap > 0 ) {
		/* letter by letter, with the gap between */
		INT	cx = x;

		for ( i = 0; i < taken; i++ ) {
			UB	one[5];
			INT	m = ms_utf8_enc(c[i], one);

			one[m] = 0;
			cx += dp_text(gid, cx, y + px, one, col, px) + gap;
		}
	} else if ( k > 0 ) {
		(void)dp_text(gid, x, y + px, buf, col, px);
	}
	return taken;
}

/* Where a segment's text starts in the window, and its letters' size */
static void text_origin( MSSEG *s, MSSEG *look, INT *p_x, INT *p_y, INT *p_px )
{
	INT	px = (INT)s->tsize;

	*p_x = (INT)( s->x - vx ) + 4;
	*p_y = (INT)( s->y - vy ) + 4;
	if ( look->fs != NULL && look->fs->hastextbox ) {
		*p_x = (INT)( s->x - vx + ( look->fs->tbox.l - look->fs->box.l ) ) + 2;
		*p_y = (INT)( s->y - vy + ( look->fs->tbox.t - look->fs->box.t ) ) + 2;
	}
	*p_px = ( px > 0 ) ? px : 14;
}

static INT line_height( MSSEG *s, INT px )
{
	return px + ( ( s->tlgap > 0 ) ? (INT)s->tlgap : (INT)floor(px * 0.5 + 0.5) );
}

static void draw_text_over( INT gid, MSSEG *s, MSSEG *look )
{
	T_DPRECT	r;
	INT		px, lh, x, y, w, i, st, n, sx = (INT)( s->x - vx ), sy = (INT)( s->y - vy );
	UW		col;

	text_origin(s, look, &x, &y, &px);
	lh = line_height(s, px);
	if ( s->tbset ) {
		r.left = sx;
		r.top = sy;
		r.right = sx + (INT)s->w;
		r.bottom = sy + (INT)s->h;
		(void)dp_fill_rect(gid, &r, colour_of(s->tbcol, 0xFFFFFF));
	}
	col = colour_of(s->tfcol, ( look->fs != NULL && look->fs->textcol != 0xFFFFFFFFU ) ? look->fs->textcol : 0);
	w = (INT)s->w - ( x - sx ) - 4;
	if ( w < px ) w = px;
	n = ( s->ntx > 0 ) ? s->ntx - 1 : 0;
	for ( i = 0; i < n && s->tx[i] != 0; i++ ) ;
	n = i;
	st = 0;
	while ( st < n && y < sy + s->h + lh ) {
		INT	e = st, took;

		while ( e < n && s->tx[e] != '\n' ) e++;
		if ( e == st ) {
			y += lh;
			st = e + 1;
			continue;
		}
		took = text_line(gid, s->tx + st, e - st, x, y, w, px, col, (INT)s->tcgap);
		if ( s->tstyl != 0 && ( ms_int32(s->tstyl) & 1 ) ) {
			T_DPRECT	u;

			u.left = x;
			u.right = x + w;
			u.top = y + px + 1;
			u.bottom = y + px + 2;
			(void)dp_fill_rect(gid, &u, col);
		}
		y += lh;
		st += ( took > 0 ) ? took : 1;
		if ( st < n && s->tx[st] == '\n' ) st++;
	}
}

static MSSEG	*input_seg;
static BOOL	input_kanji;
static INT	caret;			/* where typing goes: a character's place */
static INT	caret_back;		/* INPUT: the caret kept this far from the end */

/* The characters of a segment's text, without its 0 */
static INT text_len( const MSSEG *s )
{
	INT	n = ( s->ntx > 0 ) ? s->ntx - 1 : 0, i;

	for ( i = 0; i < n && s->tx[i] != 0; i++ ) ;
	return i;
}

/* The order segments are drawn in: by how far forward each is; a copy just behind its own */
static INT draw_key( const MSSEG *s )
{
	return ( s->dupof != NULL ) ? s->dupof->z * 2 : s->z * 2 + 1;
}

void st_render( INT gid, INT w, INT h, const T_DPRECT *clip )
{
	T_DPRECT	all, cut;
	MSSEG		*s, **vis;
	INT		n = 0, i, j;

	all.left = 0;
	all.top = 0;
	all.right = w;
	all.bottom = h;
	cut = ( clip != NULL ) ? *clip : all;
	if ( cut.left < 0 ) cut.left = 0;
	if ( cut.top < 0 ) cut.top = 0;
	if ( cut.right > w ) cut.right = w;
	if ( cut.bottom > h ) cut.bottom = h;
	if ( cut.right <= cut.left || cut.bottom <= cut.top ) return;
	(void)dp_fill_rect(gid, &cut, paper);
	if ( ms_fig.view.ok ) {
		/* what the figure's view leaves out stays empty */
		INT	l = (INT)( ms_fig.view.l - vx ), t = (INT)( ms_fig.view.t - vy );
		INT	r = (INT)( ms_fig.view.r - vx ), b = (INT)( ms_fig.view.b - vy );

		if ( cut.left < l ) cut.left = l;
		if ( cut.top < t ) cut.top = t;
		if ( ms_fig.view.r > ms_fig.view.l && cut.right > r ) cut.right = r;
		if ( ms_fig.view.b > ms_fig.view.t && cut.bottom > b ) cut.bottom = b;
		if ( cut.right <= cut.left || cut.bottom <= cut.top ) return;
	}
	if ( ms_fig.backlen > 0 ) {
		(void)dp_draw_tad(gid, &cut, (INT)-vx, (INT)-vy, (const UB *)ms_fig.back, ms_fig.backlen);
	}
	for ( s = first; s != NULL; s = s->next ) if ( st_shown(s) ) n++;
	vis = ms_alloc(sizeof(MSSEG *) * (size_t)( n + 1 ));
	n = 0;
	for ( s = first; s != NULL; s = s->next ) if ( st_shown(s) ) vis[n++] = s;
	for ( i = 1; i < n; i++ ) {
		MSSEG	*x = vis[i];

		for ( j = i; j > 0 && draw_key(vis[j - 1]) > draw_key(x); j-- ) vis[j] = vis[j - 1];
		vis[j] = x;
	}
	for ( i = 0; i < n; i++ ) {
		MSSEG	*look = look_of(vis[i]);
		MSFSEG	*fs = look->fs;
		BOOL	over = (BOOL)( vis[i]->textover || vis[i] == input_seg );
		INT	ox, oy;

		if ( fs != NULL && fs->img != NULL ) {
			/* a picture: the part of it inside what is drawn */
			INT	l = (INT)( vis[i]->x - vx ), t = (INT)( vis[i]->y - vy );
			INT	x0 = ( cut.left > l ) ? cut.left : l, y0 = ( cut.top > t ) ? cut.top : t;
			INT	x1 = ( cut.right < l + fs->imgw ) ? cut.right : l + fs->imgw;
			INT	y1 = ( cut.bottom < t + fs->imgh ) ? cut.bottom : t + fs->imgh;

			if ( x1 > x0 && y1 > y0 ) {
				(void)dp_put_argb(gid, x0, y0, fs->img + (SZ)( y0 - t ) * fs->imgpitch + ( x0 - l ),
						  fs->imgpitch, x1 - x0, y1 - y0, 0xFFFFFFFFU);
			}
		} else if ( fs != NULL ) {
			const char	*frag = over ? fs->frag_notext : fs->frag;
			INT		len = over ? fs->fraglen_notext : fs->fraglen;

			ox = (INT)( vis[i]->x - vx - fs->box.l );
			oy = (INT)( vis[i]->y - vy - fs->box.t );
			if ( vis[i]->tfcol >= 0 && !over && fs->text != NULL && fs->text[0] != 0 ) {
				/* the figure's own text in the colour the script gave */
				(void)dp_draw_tad(gid, &cut, ox, oy, (const UB *)fs->frag_notext, fs->fraglen_notext);
				over = TRUE;
			} else {
				(void)dp_draw_tad(gid, &cut, ox, oy, (const UB *)frag, len);
			}
		}
		if ( over ) draw_text_over(gid, vis[i], look);
	}
	ms_free(vis);
	if ( input_seg != NULL && st_shown(input_seg) ) {
		/* the caret before the character typing goes to */
		T_DPRECT	c;
		INT		x, y, px, k, ls = 0, line = 0;

		text_origin(input_seg, look_of(input_seg), &x, &y, &px);
		for ( k = 0; k < caret; k++ ) {
			if ( input_seg->tx[k] == '\n' ) {
				line++;
				ls = k + 1;
			}
		}
		c.left = x + chars_width(input_seg->tx + ls, caret - ls, px, (INT)input_seg->tcgap);
		c.right = c.left + 2;
		c.top = y + line * line_height(input_seg, px);
		c.bottom = c.top + px + 2;
		(void)dp_fill_rect(gid, &c, 0x00000000);
		if ( input_kanji ) ms_ime_draw(gid, c.left, c.top, px, line_height(input_seg, px));
	}
}

/*
 * What an effect has not yet brought in at `ratio` of its way (0 none of
 * it, 1 all), as rectangles inside `box`. STRP divides the box into
 * `steps` stripes; MOSAIC into cells shown in a fixed shuffled order.
 */
#define MOSAIC_COLS	8
#define MOSAIC_ROWS	6

static UB	mosaic_order[MOSAIC_COLS * MOSAIC_ROWS];
static BOOL	mosaic_made;

INT st_effect_rects( const char *effect, double ratio, const T_DPRECT *box, INT steps,
		     T_DPRECT *out, INT max )
{
	INT	n = 0, i;
	double	l = box->left, t = box->top, w = box->right - box->left, h = box->bottom - box->top;

#define RECT(x0, y0, x1, y1)	do { if ( n < max && (INT)(x1) > (INT)(x0) && (INT)(y1) > (INT)(y0) ) { \
				     out[n].left = (INT)(x0); out[n].top = (INT)(y0); \
				     out[n].right = (INT)(x1); out[n].bottom = (INT)(y1); n++; } } while ( 0 )
	if ( strcmp(effect, "WIPE_D") == 0 || strcmp(effect, "SCRL_D") == 0 ) RECT(l, t + h * ratio, l + w, t + h);
	else if ( strcmp(effect, "WIPE_U") == 0 || strcmp(effect, "SCRL_U") == 0 ) RECT(l, t, l + w, t + h - h * ratio);
	else if ( strcmp(effect, "WIPE_R") == 0 || strcmp(effect, "SCRL_R") == 0 ) RECT(l + w * ratio, t, l + w, t + h);
	else if ( strcmp(effect, "WIPE_L") == 0 || strcmp(effect, "SCRL_L") == 0 ) RECT(l, t, l + w - w * ratio, t + h);
	else if ( strcmp(effect, "WIPE_V") == 0 ) {
		double	hh = h * ( 1 - ratio ) / 2;

		RECT(l, t, l + w, t + hh);
		RECT(l, t + h - hh, l + w, t + h);
	} else if ( strcmp(effect, "WIPE_H") == 0 ) {
		double	ww = w * ( 1 - ratio ) / 2;

		RECT(l, t, l + ww, t + h);
		RECT(l + w - ww, t, l + w, t + h);
	} else if ( strcmp(effect, "WIPE_C") == 0 ) {
		double	ww = w * ( 1 - ratio ) / 2, hh = h * ( 1 - ratio ) / 2;

		RECT(l, t, l + w, t + hh);
		RECT(l, t + h - hh, l + w, t + h);
		RECT(l, t + hh, l + ww, t + h - hh);
		RECT(l + w - ww, t + hh, l + w, t + h - hh);
	} else if ( strcmp(effect, "STRP_H") == 0 || strcmp(effect, "STRP_V") == 0 ) {
		INT	k = ( steps < 1 ) ? 1 : steps;

		for ( i = 0; i < k; i++ ) {
			if ( effect[5] == 'H' ) {
				double	sh = h / k, sy = t + sh * i;

				RECT(l, sy + sh * ratio, l + w, sy + sh);
			} else {
				double	sw = w / k, sx = l + sw * i;

				RECT(sx + sw * ratio, t, sx + sw, t + h);
			}
		}
	} else if ( strcmp(effect, "MOSAIC") == 0 ) {
		INT	shown;
		double	cw = w / MOSAIC_COLS, ch = h / MOSAIC_ROWS;

		if ( !mosaic_made ) {
			UW	x = 2463534242U;

			for ( i = 0; i < MOSAIC_COLS * MOSAIC_ROWS; i++ ) mosaic_order[i] = (UB)i;
			for ( i = MOSAIC_COLS * MOSAIC_ROWS - 1; i > 0; i-- ) {
				INT	j;
				UB	tmp;

				x ^= x << 13; x ^= x >> 17; x ^= x << 5;
				j = (INT)( x % (UW)( i + 1 ) );
				tmp = mosaic_order[i]; mosaic_order[i] = mosaic_order[j]; mosaic_order[j] = tmp;
			}
			mosaic_made = TRUE;
		}
		shown = (INT)floor(MOSAIC_COLS * MOSAIC_ROWS * ratio);
		for ( i = shown; i < MOSAIC_COLS * MOSAIC_ROWS; i++ ) {
			INT	idx = mosaic_order[i];
			double	cx = l + ( idx % MOSAIC_COLS ) * cw, cy = t + ( idx / MOSAIC_COLS ) * ch;

			RECT(cx, cy, cx + cw + 1, cy + ch + 1);
		}
	}
#undef RECT
	return n;
}

/* ---------------------------------------------------------------- input */

/*
 * INPUT and KINPUT: the characters typed go into the segment's text at
 * the caret. INPUT's caret is placed at the end unless a place in the
 * segment is given, and stays as far from the end as it was when a
 * character goes in or out; KINPUT's is placed at the start and moves
 * with what goes in. The arrow keys move it.
 */
void st_input_begin( MSSEG *s, BOOL kanji, INT x, INT y )
{
	INT	n = text_len(s), px, tx, ty, lh, line = 0, k, ls = 0;

	input_seg = s;
	input_kanji = kanji;
	caret = kanji ? 0 : n;
	if ( x > 0 && y > 0 && x < s->w && y < s->h ) {
		/* the character nearest to that place */
		text_origin(s, look_of(s), &tx, &ty, &px);
		lh = line_height(s, px);
		line = ( y + (INT)( s->y - vy ) - ty ) / lh;
		for ( k = 0; k < n && line > 0; k++ ) {
			if ( s->tx[k] == '\n' ) { line--; ls = k + 1; }
		}
		for ( k = ls; k < n && s->tx[k] != '\n'; k++ ) {
			if ( tx + chars_width(s->tx + ls, k - ls + 1, px, (INT)s->tcgap) > x + (INT)( s->x - vx ) ) break;
		}
		caret = k;
	}
	caret_back = n - caret;
	ms_dirty();
}

void st_input_end( void )
{
	if ( input_seg == NULL ) return;
	if ( input_kanji ) ms_ime_commit();	/* what is being composed goes in first */
	ms_ime_end();
	input_seg->textover = TRUE;
	input_seg = NULL;
	ms_dirty();
}

MSSEG *st_input_seg( void )
{
	return input_seg;
}

BOOL st_input_kanji( void )
{
	return (BOOL)( input_seg != NULL && input_kanji );
}

static void text_splice( MSSEG *s, INT at, INT cut, const UW *ins, INT nins )
{
	INT	n = text_len(s);
	UW	*c = ms_alloc(sizeof(UW) * (size_t)( n + nins + 1 ));

	if ( at < 0 ) at = 0;
	if ( at > n ) at = n;
	if ( at + cut > n ) cut = n - at;
	memcpy(c, s->tx, sizeof(UW) * (size_t)at);
	if ( nins > 0 ) memcpy(c + at, ins, sizeof(UW) * (size_t)nins);
	memcpy(c + at + nins, s->tx + at + cut, sizeof(UW) * (size_t)( n - at - cut ));
	st_text_set(s, c, n - cut + nins);
	ms_free(c);
}

/* A key while a segment takes input; FALSE when it did not take it */
BOOL st_input_key( UW ch )
{
	MSSEG	*s = input_seg;
	INT	n;

	if ( s == NULL ) return FALSE;
	n = text_len(s);
	if ( !input_kanji ) caret = n - caret_back;
	if ( caret < 0 ) caret = 0;
	if ( caret > n ) caret = n;
	switch ( ch ) {
	case 0x102:				/* right */
		if ( caret < n ) caret++;
		caret_back = n - caret;
		break;
	case 0x103:				/* left */
		if ( caret > 0 ) caret--;
		caret_back = n - caret;
		break;
	case 0x08:				/* the character before */
		if ( caret == 0 ) return TRUE;
		text_splice(s, caret - 1, 1, NULL, 0);
		if ( input_kanji ) caret--;
		break;
	case 0x7F:				/* the character at the caret */
		if ( caret >= n ) return TRUE;
		text_splice(s, caret, 1, NULL, 0);
		if ( !input_kanji ) caret_back--;
		break;
	case 0x0A:
		if ( !input_kanji ) return FALSE;	/* INPUT: a key for the script */
		text_splice(s, caret, 0, &ch, 1);
		caret++;
		break;
	default:
		if ( ch < 0x20 || ch == 0x100 || ch == 0x101 ) return FALSE;
		text_splice(s, caret, 0, &ch, 1);
		if ( input_kanji ) caret++;
		break;
	}
	n = text_len(s);
	if ( input_kanji ) caret_back = n - caret;
	else caret = n - caret_back;
	ms_dirty();
	return TRUE;
}

/* A string typed at once (what the converter gives), each character as a key */
void st_input_text( const UW *c, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) (void)st_input_key(c[i]);
}
