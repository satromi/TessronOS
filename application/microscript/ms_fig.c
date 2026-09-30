/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_fig.c
 *	The stage read from a figure: its segments, what stands behind
 *	them, and its links
 *
 *	A group that holds a text beginning with "@" (or "＠") is a segment;
 *	the rest of that text, up to its first line end, is its name, the
 *	first 12 characters of it. The label is taken out of what is drawn.
 *	A group holding no label directly is still a segment when exactly
 *	one of the groups right inside it holds one, or when exactly one
 *	label is anywhere inside it; otherwise the groups inside it are
 *	looked at in turn.
 *
 *	Each segment is drawn from a small figure of its own: the text of its
 *	element as the record has it with the label cut out, behind the
 *	pattern and mask definitions of the whole figure. The shapes that
 *	stand directly in the figure and are neither groups nor links are
 *	the ground, drawn behind everything.
 */

#include "ms.h"
#include <string.h>
#include <stdlib.h>

#define FIG_HEAD	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
#define FIG_TAIL	"</figure></tad>"

static BOOL is_shape( const MSX *e )
{
	static const char *const tags[] = {
		"rect", "rectangle", "ellipse", "circle", "line", "polygon", "polyline",
		"text", "image", "pixelmap", "link", "group", "document", "arc", "curve",
		NULL
	};
	INT	i;

	for ( i = 0; tags[i] != NULL; i++ ) {
		if ( msx_is(e, tags[i]) ) return TRUE;
	}
	return FALSE;
}

/* A text shape: a text element, a document, or a rectangle that carries text */
static BOOL is_text( const MSX *e )
{
	if ( msx_is(e, "text") || msx_is(e, "document") ) return TRUE;
	if ( msx_is(e, "rect") || msx_is(e, "rectangle") ) {
		return (BOOL)( msx_attr(e, "fontSize") != NULL || msx_attr(e, "textColor") != NULL
			    || msx_find((MSX *)e, "document") != NULL );
	}
	return FALSE;
}

/* The paragraphs of a document, each a line, without its view and style elements */
static void doc_text( const MSX *d, MSBUF *b )
{
	const MSX	*c;
	BOOL		first = TRUE, any = FALSE;

	for ( c = d->first; c != NULL; c = c->next ) {
		if ( msx_is(c, "p") ) {
			const MSX	*k;

			any = TRUE;
			if ( !first ) mb_putc(b, '\n');
			first = FALSE;
			for ( k = c->first; k != NULL; k = k->next ) {
				if ( k->tag == NULL ) { if ( k->text ) mb_puts(b, k->text); continue; }
				if ( msx_is(k, "docView") || msx_is(k, "docDraw") || msx_is(k, "docScale")
				  || msx_is(k, "text") || msx_is(k, "font") ) continue;
				msx_text(k, b, TRUE);
			}
		}
	}
	if ( !any ) {
		/* text straight in the document, as an older writer left it */
		for ( c = d->first; c != NULL; c = c->next ) {
			if ( c->tag == NULL ) { if ( c->text ) mb_puts(b, c->text); continue; }
			if ( msx_is(c, "docView") || msx_is(c, "docDraw") || msx_is(c, "docScale")
			  || msx_is(c, "text") || msx_is(c, "font") ) continue;
			msx_text(c, b, TRUE);
		}
	}
}

/* The text a text shape shows, trimmed; NUL ended in b */
static void shape_text( const MSX *e, MSBUF *b )
{
	MSBUF	t;
	INT	s, n;

	mb_init(&t);
	mb_putn(&t, "", 0);
	if ( msx_is(e, "document") ) doc_text(e, &t);
	else msx_text(e, &t, TRUE);
	for ( s = 0; s < t.n && ( t.s[s] == ' ' || t.s[s] == '\t' || t.s[s] == '\r' || t.s[s] == '\n' ); s++ ) ;
	for ( n = t.n; n > s && ( t.s[n - 1] == ' ' || t.s[n - 1] == '\t' || t.s[n - 1] == '\r' || t.s[n - 1] == '\n' ); n-- ) ;
	mb_putn(b, t.s + s, n - s);
	mb_free(&t);
}

/*
 * The segment name a label gives: normalised as the script's names are,
 * trimmed, 12 characters at most. FALSE when e is not a label.
 */
BOOL ms_label_name( const MSX *e, char *out, INT max )
{
	MSBUF	b, nm;
	UW	cp;
	INT	i, k, chars = 0, s, n;

	if ( !is_text(e) ) return FALSE;
	mb_init(&b);
	shape_text(e, &b);
	if ( b.n == 0 ) { mb_free(&b); return FALSE; }
	k = ms_utf8_dec((const UB *)b.s, b.n, &cp);
	if ( cp != '@' && cp != 0xFF20 ) { mb_free(&b); return FALSE; }
	mb_init(&nm);
	mb_putn(&nm, "", 0);
	for ( i = k; i < b.n; ) {
		INT	d = ms_utf8_dec((const UB *)b.s + i, b.n - i, &cp);

		if ( cp == '\r' || cp == '\n' ) break;
		mb_putcp(&nm, ms_norm_cp(cp));
		i += d;
	}
	mb_free(&b);
	for ( s = 0; s < nm.n && ( nm.s[s] == ' ' || nm.s[s] == '\t' ); s++ ) ;
	for ( n = nm.n; n > s && ( nm.s[n - 1] == ' ' || nm.s[n - 1] == '\t' ); n-- ) ;
	/* the first 12 characters */
	for ( i = s; i < n && chars < 12; chars++ ) {
		i += ms_utf8_dec((const UB *)nm.s + i, n - i, &cp);
	}
	n = i;
	if ( n - s >= max ) n = s + max - 1;
	memcpy(out, nm.s + s, (size_t)( n - s ));
	out[n - s] = 0;
	mb_free(&nm);
	return TRUE;
}

/* ---------------------------------------------------------------- boxes */

static void box_add( MSBOX *b, double l, double t, double r, double bt )
{
	if ( l > r ) { double x = l; l = r; r = x; }
	if ( t > bt ) { double x = t; t = bt; bt = x; }
	if ( !b->ok ) {
		b->l = l; b->t = t; b->r = r; b->b = bt;
		b->ok = TRUE;
		return;
	}
	if ( l < b->l ) b->l = l;
	if ( t < b->t ) b->t = t;
	if ( r > b->r ) b->r = r;
	if ( bt > b->b ) b->b = bt;
}

static void box_points( MSBOX *b, const char *pts )
{
	const char	*p = pts;
	char		*end;
	double		x, y;

	while ( p != NULL && *p != 0 ) {
		while ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) p++;
		if ( *p == 0 ) break;
		x = strtod(p, &end);
		if ( end == p ) break;
		p = end;
		if ( *p == ',' ) p++;
		y = strtod(p, &end);
		if ( end == p ) break;
		p = end;
		box_add(b, x, y, x, y);
	}
}

static MSBOX shape_box( const MSX *e );

static BOOL has4( const MSX *e, const char *l, const char *t, const char *r, const char *b )
{
	return (BOOL)( msx_attr(e, l) != NULL && msx_attr(e, t) != NULL
		    && msx_attr(e, r) != NULL && msx_attr(e, b) != NULL );
}

static MSBOX shape_box( const MSX *e )
{
	MSBOX	b;
	MSX	*c;

	memset(&b, 0, sizeof(b));
	if ( msx_is(e, "group") ) {
		if ( has4(e, "left", "top", "right", "bottom") ) {
			box_add(&b, msx_num(e, "left", 0), msx_num(e, "top", 0),
				msx_num(e, "right", 0), msx_num(e, "bottom", 0));
			return b;
		}
		for ( c = e->first; c != NULL; c = c->next ) {
			MSBOX	k;

			if ( !is_shape(c) ) continue;
			k = shape_box(c);
			if ( k.ok ) box_add(&b, k.l, k.t, k.r, k.b);
		}
		return b;
	}
	if ( msx_is(e, "document") ) {
		MSX	*v = msx_find((MSX *)e, "docView");

		if ( v != NULL ) {
			box_add(&b, msx_num(v, "viewleft", msx_num(v, "left", 0)),
				msx_num(v, "viewtop", msx_num(v, "top", 0)),
				msx_num(v, "viewright", msx_num(v, "right", 0)),
				msx_num(v, "viewbottom", msx_num(v, "bottom", 0)));
		}
		return b;
	}
	if ( msx_is(e, "link") ) {
		box_add(&b, msx_num(e, "vobjleft", msx_num(e, "left", 0)),
			msx_num(e, "vobjtop", msx_num(e, "top", 0)),
			msx_num(e, "vobjright", msx_num(e, "right", 0)),
			msx_num(e, "vobjbottom", msx_num(e, "bottom", 0)));
		return b;
	}
	if ( msx_is(e, "ellipse") || msx_is(e, "circle") ) {
		box_add(&b, msx_num(e, "frameLeft", msx_num(e, "left", 0)),
			msx_num(e, "frameTop", msx_num(e, "top", 0)),
			msx_num(e, "frameRight", msx_num(e, "right", 0)),
			msx_num(e, "frameBottom", msx_num(e, "bottom", 0)));
		return b;
	}
	if ( msx_is(e, "line") || msx_is(e, "polygon") || msx_is(e, "polyline")
	  || msx_is(e, "curve") ) {
		const char	*pts = msx_attr(e, "points");

		if ( pts != NULL && *pts != 0 ) {
			box_points(&b, pts);
		} else {
			box_add(&b, msx_num(e, "x1", msx_num(e, "left", 0)),
				msx_num(e, "y1", msx_num(e, "top", 0)),
				msx_num(e, "x2", msx_num(e, "right", 0)),
				msx_num(e, "y2", msx_num(e, "bottom", 0)));
		}
		return b;
	}
	if ( msx_attr(e, "left") != NULL || msx_attr(e, "top") != NULL ) {
		box_add(&b, msx_num(e, "left", 0), msx_num(e, "top", 0),
			msx_num(e, "right", 0), msx_num(e, "bottom", 0));
	}
	return b;
}

/* ---------------------------------------------------------------- segments */

typedef struct {
	BOOL	ok;
	char	name[64];
	MSX	*label;
} DET;

static void labels_deep( MSX *g, INT *p_count, DET *first )
{
	MSX	*c;

	for ( c = g->first; c != NULL; c = c->next ) {
		char	nm[64];

		if ( c->tag == NULL ) continue;
		if ( ms_label_name(c, nm, sizeof(nm)) ) {
			(*p_count)++;
			if ( !first->ok ) {
				first->ok = TRUE;
				strcpy(first->name, nm);
				first->label = c;
			}
		}
		if ( msx_is(c, "group") ) labels_deep(c, p_count, first);
	}
}

static DET detect( MSX *g )
{
	DET	d, found;
	MSX	*c, *k;
	INT	cnt = 0;

	memset(&d, 0, sizeof(d));
	for ( c = g->first; c != NULL; c = c->next ) {
		if ( c->tag != NULL && ms_label_name(c, d.name, sizeof(d.name)) ) {
			d.ok = TRUE;
			d.label = c;
			return d;
		}
	}
	memset(&found, 0, sizeof(found));
	for ( c = g->first; c != NULL; c = c->next ) {
		if ( !msx_is(c, "group") ) continue;
		for ( k = c->first; k != NULL; k = k->next ) {
			char	nm[64];

			if ( k->tag != NULL && ms_label_name(k, nm, sizeof(nm)) ) {
				cnt++;
				if ( !found.ok ) {
					found.ok = TRUE;
					strcpy(found.name, nm);
					found.label = k;
				}
				break;
			}
		}
	}
	if ( cnt == 1 ) return found;
	cnt = 0;
	memset(&found, 0, sizeof(found));
	labels_deep(g, &cnt, &found);
	if ( cnt == 1 ) return found;
	memset(&d, 0, sizeof(d));
	return d;
}

/*
 * A figure of its own holding src[e->start, e->end) without the spans of
 * cut[]: the pieces of the element in their order, behind the head.
 */
static char *piece( MSFIG *f, const MSX *e, MSX **cut, INT ncut, INT *p_len )
{
	MSBUF	b;
	INT	at = e->start, i, j;

	/* the cuts in the order they stand */
	for ( i = 1; i < ncut; i++ ) {
		for ( j = i; j > 0 && cut[j - 1]->start > cut[j]->start; j-- ) {
			MSX	*x = cut[j]; cut[j] = cut[j - 1]; cut[j - 1] = x;
		}
	}
	mb_init(&b);
	mb_puts(&b, FIG_HEAD);
	mb_putn(&b, f->head, f->headlen);
	for ( i = 0; i < ncut; i++ ) {
		if ( cut[i]->start < at || cut[i]->end > e->end ) continue;
		mb_putn(&b, f->doc.src + at, cut[i]->start - at);
		at = cut[i]->end;
	}
	mb_putn(&b, f->doc.src + at, e->end - at);
	mb_puts(&b, FIG_TAIL);
	*p_len = b.n;
	return b.s;
}

static UW parse_colour( const char *s, UW dflt )
{
	UW	v = 0;
	INT	i;

	if ( s == NULL || s[0] != '#' ) return dflt;
	for ( i = 1; i <= 6; i++ ) {
		char	c = s[i];

		if ( c >= '0' && c <= '9' ) v = v * 16 + (UW)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) v = v * 16 + (UW)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) v = v * 16 + (UW)( c - 'A' + 10 );
		else return dflt;
	}
	return v;
}

/* The first colour a <font> inside e says, in the order they stand */
static BOOL font_colour( const MSX *e, UW *p_col )
{
	const MSX	*c;

	for ( c = e->first; c != NULL; c = c->next ) {
		if ( msx_is(c, "font") && msx_attr(c, "color") != NULL ) {
			*p_col = parse_colour(msx_attr(c, "color"), 0);
			return TRUE;
		}
		if ( c->tag != NULL && font_colour(c, p_col) ) return TRUE;
	}
	return FALSE;
}

/* The colour a text shape's letters have */
static UW text_colour( const MSX *e )
{
	UW	col = 0;

	if ( msx_attr(e, "textColor") != NULL ) return parse_colour(msx_attr(e, "textColor"), 0);
	(void)font_colour(e, &col);
	return col;
}

/* The size of a text shape's letters in pixels; 0 when it does not say */
static double text_size( const MSX *e )
{
	MSX	*fs;

	if ( msx_attr(e, "fontSize") != NULL ) return msx_num(e, "fontSize", 0);
	fs = msx_find((MSX *)e, "font");
	while ( fs != NULL && msx_attr(fs, "size") == NULL ) fs = NULL;
	if ( fs != NULL ) return msx_num(fs, "size", 0) * 1.4;
	return 0;
}

static void add_seg( MSFIG *f, MSX *g, DET *d )
{
	MSFSEG	*s;
	MSX	*c, *cut[64];
	INT	ncut = 0, nchild = 0, ntext = 0;
	MSBUF	tb;

	f->seg = ms_realloc(f->seg, sizeof(MSFSEG) * (size_t)( f->nseg + 1 ));
	s = &f->seg[f->nseg++];
	memset(s, 0, sizeof(*s));
	s->name = ms_intern_z(d->name);
	s->group = g;
	s->label = d->label;
	s->textcol = 0xFFFFFFFFU;
	for ( c = g->first; c != NULL; c = c->next ) {
		MSBOX	k;

		if ( c->tag == NULL || !is_shape(c) || c == d->label ) continue;
		nchild++;
		k = shape_box(c);
		if ( k.ok ) box_add(&s->box, k.l, k.t, k.r, k.b);
		if ( msx_is(c, "link") ) s->haslink = TRUE;
		if ( is_text(c) ) {
			ntext++;
			if ( s->text == NULL ) {
				mb_init(&tb);
				shape_text(c, &tb);
				s->text = ( tb.s != NULL ) ? tb.s : ms_strdup("");
				s->textcol = text_colour(c);
				s->textsize = text_size(c);
				s->tbox = shape_box(c);
				s->hastextbox = s->tbox.ok;
			}
			if ( ncut < 63 ) cut[ncut++] = c;
		}
	}
	s->alltext = (BOOL)( nchild > 0 && ntext == nchild );
	{
		MSBOX	gb = shape_box(g);

		s->gl = gb.ok ? gb.l : s->box.l;
		s->gt = gb.ok ? gb.t : s->box.t;
	}
	if ( !s->box.ok ) {
		/* nothing to draw: a place of its own all the same */
		MSBOX	k = shape_box(g);

		s->box = k;
		if ( !s->box.ok ) { s->box.ok = TRUE; }
	}
	{
		MSX	*one[1];

		one[0] = d->label;
		s->frag = piece(f, g, one, 1, &s->fraglen);
	}
	cut[ncut++] = d->label;
	s->frag_notext = piece(f, g, cut, ncut, &s->fraglen_notext);
}

static void collect( MSFIG *f, MSX *g )
{
	DET	d = detect(g);
	MSX	*c;

	if ( d.ok ) {
		add_seg(f, g, &d);
		return;
	}
	for ( c = g->first; c != NULL; c = c->next ) {
		if ( msx_is(c, "group") ) collect(f, c);
	}
}

/*
 * A virtual object of the figure made a segment named by its object's
 * name: what is drawn is the virtual object itself.
 */
void ms_fig_add_vobj( MSFIG *f, INT link, MSSTR *name )
{
	MSFSEG	*s;
	MSX	*e = f->link[link].node;

	f->seg = ms_realloc(f->seg, sizeof(MSFSEG) * (size_t)( f->nseg + 1 ));
	s = &f->seg[f->nseg++];
	memset(s, 0, sizeof(*s));
	s->name = name;
	s->group = e;
	s->textcol = 0xFFFFFFFFU;
	s->box = shape_box(e);
	if ( !s->box.ok ) s->box.ok = TRUE;
	s->gl = s->box.l;
	s->gt = s->box.t;
	s->haslink = TRUE;
	s->isvobj = TRUE;
	s->target = f->link[link].target;
	s->frag = piece(f, e, NULL, 0, &s->fraglen);
	s->frag_notext = piece(f, e, NULL, 0, &s->fraglen_notext);
}

/* The segments in the order the figure has them, back to front */
void ms_fig_order( MSFIG *f )
{
	INT	i, j;

	for ( i = 1; i < f->nseg; i++ ) {
		MSFSEG	x = f->seg[i];

		for ( j = i; j > 0 && f->seg[j - 1].group->start > x.group->start; j-- ) f->seg[j] = f->seg[j - 1];
		f->seg[j] = x;
	}
}

/* A UUID as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"; out holds 37 bytes */
void ms_uuid_str( const TS_UUID *u, char *out )
{
	static const char	hex[] = "0123456789abcdef";
	const UB		*b = (const UB *)u;
	INT			i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[b[i] >> 4];
		out[n++] = hex[b[i] & 0x0F];
	}
	out[n] = 0;
}

/* The UUID at the head of "xxxxxxxx-xxxx-...-xxxxxxxxxxxx_0.xtad" */
BOOL ms_uuid( const char *s, TS_UUID *u )
{
	UB	*o = (UB *)u;
	INT	i, k = 0;

	if ( s == NULL ) return FALSE;
	for ( i = 0; s[i] != 0 && k < 32; i++ ) {
		char	c = s[i];
		INT	v;

		if ( c == '-' ) continue;
		if ( c >= '0' && c <= '9' ) v = c - '0';
		else if ( c >= 'a' && c <= 'f' ) v = c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) v = c - 'A' + 10;
		else return FALSE;
		if ( k % 2 == 0 ) o[k / 2] = (UB)( v << 4 );
		else o[k / 2] |= (UB)v;
		k++;
	}
	return (BOOL)( k == 32 );
}

ER ms_fig_read( MSFIG *f, const char *xml, INT len )
{
	MSX	*fig, *c;
	MSBUF	head, back;

	memset(f, 0, sizeof(*f));
	f->paper = 0x00FFFFFF;
	msx_parse(&f->doc, xml, len);
	fig = msx_find(f->doc.root, "figure");
	if ( fig == NULL ) {
		return E_OBJ;
	}

	/* what every piece carries: the patterns and masks */
	mb_init(&head);
	mb_putn(&head, "", 0);
	for ( c = fig->first; c != NULL; c = c->next ) {
		if ( msx_is(c, "pattern") || msx_is(c, "mask") ) {
			mb_putn(&head, xml + c->start, c->end - c->start);
		}
		if ( msx_is(c, "figView") ) {
			box_add(&f->view, msx_num(c, "left", 0), msx_num(c, "top", 0),
				msx_num(c, "right", 0), msx_num(c, "bottom", 0));
		}
	}
	f->head = head.s;
	f->headlen = head.n;

	mb_init(&back);
	mb_puts(&back, FIG_HEAD);
	mb_putn(&back, f->head, f->headlen);
	for ( c = fig->first; c != NULL; c = c->next ) {
		if ( c->tag == NULL || !is_shape(c) ) continue;
		if ( msx_is(c, "group") ) {
			collect(f, c);
			continue;
		}
		if ( msx_is(c, "link") ) {
			f->link = ms_realloc(f->link, sizeof(MSFLINK) * (size_t)( f->nlink + 1 ));
			memset(&f->link[f->nlink], 0, sizeof(MSFLINK));
			f->link[f->nlink].node = c;
			if ( ms_uuid(msx_attr(c, "id"), &f->link[f->nlink].target)
			  || ms_uuid(msx_attr(c, "link_id"), &f->link[f->nlink].target) ) {
				f->nlink++;
			}
			continue;
		}
		mb_putn(&back, xml + c->start, c->end - c->start);
	}
	mb_puts(&back, FIG_TAIL);
	f->back = back.s;
	f->backlen = back.n;
	return E_OK;
}
