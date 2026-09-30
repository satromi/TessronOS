/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_tad.c
 *	A binary TAD record as xmlTAD (design 17.16)
 *
 *	A binary TAD is a row of 16-bit words: a word above FF00 starts a
 *	segment (its id, then its length in bytes, FFFF and 32 bits when it
 *	is long, then its data), any other word is a TRON character, and 0
 *	ends it. The segments are turned into the elements the xmlTAD reader
 *	knows, one for one where there is one: a text start is <document>
 *	and its paragraphs <p>, a figure start <figure>, a figure element
 *	<rect>, <line> and the rest with the same numbers, a virtual object
 *	<link> to the object made for its link record's target, a picture
 *	<image> of a PNG kept beside the record.
 *
 *	Character decorations (下線, 太字 ...) begin and end where the TAD
 *	says, which need not nest; they are kept as a stack of open elements
 *	and closed and opened again round a paragraph's end, a virtual
 *	object and a decoration ending out of order, so that the XML always
 *	nests. A text inside a figure inside a text has decorations of its
 *	own.
 *
 *	The page's settings (用紙, マージン, オーバーレイ, 禁則) that stand
 *	before the first letter are written outside the first paragraph, at
 *	the head of the document, where an editor looks for them. An
 *	overlay's own text is converted into its <paper-overlay-define>, and
 *	a figure's overlay's own shapes into its <figoverlay>. A
 *	variable is a <page-number> when it is the page's number, else a
 *	<variable>. A character with no Unicode is 〓 in a <tchar> that keeps
 *	its plane and code. A picture compressed as a facsimile is decoded
 *	first.
 *
 *	What has no element of its own is kept, not dropped: the 付箋 of an
 *	application as <docappl> or <figappl>, anything else as <tadseg>,
 *	each with the segment's bytes in hexadecimal, and those kept as
 *	<tadseg> are counted.
 */

#include <ts/bpk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVEL_MAX	16
#define SPAN_MAX	24
#define ATTR_MAX	192
#define TCHAR_MAX	64		/* codes of one <tchar> */
#define KEEP_PART	2000		/* bytes of a kept segment in one element */
#define KEEP_MAX	( 16 * 1024 )	/* and in all */
#define APPL_CALC	0x0009		/* 基本表計算's application ID, the middle word */

/* An element open in the text: a decoration or a span with attributes */
typedef struct {
	char	name[16];
	char	attrs[ATTR_MAX];
} SPAN;

/* A text or a figure being written */
typedef struct {
	char	kind;			/* 'T' text, 'F' figure */
	BOOL	para;			/* a <p> is open */
	INT	plane;			/* the TRON plane outside it */
	INT	nspan;
	SPAN	span[SPAN_MAX];
	INT	head;			/* where its first <p> was written, -1 when that is past */
	INT	hunit, vunit;		/* its coordinates' units (UNITS) */
} LEVEL;

typedef struct {
	BPKCONV		*cv;
	BPKBUF		*o;
	const char	*self;		/* the UUID of the object the record goes to */
	const UB	*d;		/* the segment's data */
	UINT		len;		/* its bytes */
	UINT		id;
	INT		sp;
	LEVEL		lv[LEVEL_MAX];
	BOOL		started;	/* a text or figure start was seen */
	BOOL		figtad;		/* the record began with a figure */
	INT		z;		/* the zIndex of a figure's next element */
	INT		fontsize;	/* thousandths of a point */
	INT		sub_target, sub_base;
	BOOL		ruby;
	BOOL		arrow_s, arrow_e;
	INT		pg_num, pg_step;	/* the pages' numbering, as ページ番号指定 last said */
	INT		ncalc;			/* decorations a table's cell opened */
	char		calc[4][16];
	INT		tplane;			/* the characters with no Unicode waiting: their plane */
	INT		ntcode;
	UH		tcode[TCHAR_MAX];
} TX;

LOCAL void walk( TX *t, const UB *rec, UINT n, BOOL chars, void (*fn)( TX * ) );
LOCAL void segment( TX *t );
LOCAL BOOL in_text( TX *t );
LOCAL void calc_close( TX *t );
LOCAL void fig_end( TX *t );

/* ---------------------------------------------------------------- words */

LOCAL UINT rd16( const UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL UINT rd32( const UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 ) | ( (UINT)p[2] << 16 ) | ( (UINT)p[3] << 24 );
}

/* Word i of the segment's data, 0 past its end */
LOCAL UINT W16( TX *t, INT i )
{
	return ( (UINT)( 2 * i + 1 ) < t->len ) ? rd16(t->d + 2 * i) : 0;
}

LOCAL INT S16( TX *t, INT i )
{
	return (INT)(H)W16(t, i);
}

/* A COLOR in words i and i + 1, the low word first */
LOCAL UINT C32( TX *t, INT i )
{
	return W16(t, i) | ( W16(t, i + 1) << 16 );
}

LOCAL INT nwords( TX *t )
{
	return (INT)( t->len / 2 );
}

LOCAL void colour( TX *t, UINT raw, char out[8] )
{
	bpk_colour(raw, t->cv->cmap, t->cv->ncmap, out);
}

/*
 * The segment kept as it stands, its bytes in hexadecimal in 'data', in
 * as many elements (part 1, 2 ...) as an attribute's room takes; FALSE
 * when it is too big to keep.
 */
LOCAL BOOL keep( TX *t, const char *name, const char *attrs )
{
	UINT	at = 0, n, i, part = 0;

	if ( t->len > KEEP_MAX ) return FALSE;
	do {
		n = ( t->len - at > KEEP_PART ) ? KEEP_PART : t->len - at;
		bpk_buf_printf(t->o, "<%s%s", name, attrs);
		if ( part > 0 ) bpk_buf_printf(t->o, " part=\"%u\"", part);
		bpk_buf_puts(t->o, " data=\"");
		for ( i = 0; i < n; i++ ) bpk_buf_printf(t->o, "%02x", t->d[at + i]);
		bpk_buf_puts(t->o, "\"/>");
		if ( !in_text(t) ) bpk_buf_putc(t->o, '\n');
		at += n;
		part++;
	} while ( at < t->len );
	return TRUE;
}

/* A segment with no element of its own: kept as <tadseg>, and counted */
LOCAL void skip( TX *t, UINT sub )
{
	BPKCONV	*cv = t->cv;
	UINT	key = ( t->id << 8 ) | ( sub & 0xFF );
	INT	i;
	char	a[24];

	snprintf(a, sizeof(a), " id=\"%04x\"", t->id);
	(void)keep(t, "tadseg", a);
	cv->nskipped++;
	for ( i = 0; i < cv->nskip; i++ ) {
		if ( cv->skip[i].id == key ) {
			cv->skip[i].count++;
			return;
		}
	}
	if ( cv->nskip < BPK_SKIP_KINDS ) {
		cv->skip[cv->nskip].id = key;
		cv->skip[cv->nskip].count = 1;
		cv->nskip++;
	}
}

/* A TRON string from word i to the segment's end, escaped for an attribute */
LOCAL void tron_attr( TX *t, INT from )
{
	INT	i, plane = 1;

	for ( i = from; i < nwords(t); i++ ) {
		UINT	cp = bpk_tron_ucs(W16(t, i), &plane);
		char	s[5];

		if ( cp < 0x20 ) continue;
		s[bpk_utf8(cp, s)] = 0;
		bpk_buf_xml(t->o, s);
	}
}

/* A SCALE: a ratio a/b (the high bit clear) or so many units ("abs:n") */
LOCAL void scale_attr( TX *t, UINT v, BOOL slash )
{
	UINT	a = ( v >> 8 ) & 0x7F, b = v & 0xFF;

	if ( v & 0x8000 ) {
		bpk_buf_printf(t->o, "abs:%u", v & 0x7FFF);
	} else if ( slash ) {
		if ( b == 0 ) bpk_buf_puts(t->o, "1/1");
		else bpk_buf_printf(t->o, "%u/%u", a, b);
	} else if ( b == 0 ) {
		bpk_buf_printf(t->o, "%u", a);
	} else {
		bpk_buf_num(t->o, (INT)( ( a * 1000 + b / 2 ) / b ));
	}
}

/*
 * So many units (UNITS: below nought so many to the inch, above so
 * many to the centimetre, nought the screen's dots of 1/120 inch) in
 * thousandths of a point.
 */
LOCAL INT units_milli( INT unit, INT v )
{
	if ( unit < 0 ) return (INT)( (D)v * 72000 / -unit );
	if ( unit > 0 ) return (INT)( (D)v * 7200000 / ( 254 * (D)unit ) );
	return v * 600;
}

/* ---------------------------------------------------------------- levels and spans */

LOCAL LEVEL *top( TX *t )
{
	return ( t->sp > 0 ) ? &t->lv[t->sp - 1] : NULL;
}

LOCAL BOOL in_text( TX *t )
{
	LEVEL	*l = top(t);

	return (BOOL)( l != NULL && l->kind == 'T' );
}

/* Lengths across and down in the text or figure now written, in thousandths of a point */
LOCAL INT h_milli( TX *t, INT v )
{
	return units_milli(( top(t) != NULL ) ? top(t)->hunit : -120, v);
}

LOCAL INT v_milli( TX *t, INT v )
{
	return units_milli(( top(t) != NULL ) ? top(t)->vunit : -120, v);
}

/*
 * A CHSIZE in thousandths of a point: the top two bits say in what --
 * 10 in 1/20 point, 01 in 1/20 Q, 11 in dots of the screen, 00 in the
 * units of the text or figure it is in.
 */
LOCAL INT chsize_milli( TX *t, UINT v )
{
	UINT	n = v & 0x3FFF;

	switch ( v & 0xC000 ) {
	case 0x8000:	return (INT)( n * 50 );
	case 0x4000:	return (INT)( ( (UD)n * 10000000 + 35280 ) / 70560 );
	case 0xC000:	return units_milli(0, (INT)n);
	default:	return v_milli(t, (INT)n);
	}
}

EXPORT INT bpk_chsize_pt( UINT v )
{
	UINT	n = v & 0x3FFF;
	INT	m;

	switch ( v & 0xC000 ) {
	case 0x8000:	m = (INT)( n * 50 );					break;
	case 0x4000:	m = (INT)( ( (UD)n * 10000000 + 35280 ) / 70560 );	break;
	case 0xC000:	m = units_milli(0, (INT)n);				break;
	default:	m = units_milli(-120, (INT)n);				break;
	}
	return ( m + 500 ) / 1000;
}

LOCAL void span_emit( TX *t, const SPAN *s )
{
	bpk_buf_printf(t->o, "<%s%s>", s->name, s->attrs);
}

/* Every span of the text closed, innermost first */
LOCAL void spans_close( TX *t )
{
	LEVEL	*l = top(t);
	INT	i;

	if ( l == NULL ) return;
	for ( i = l->nspan - 1; i >= 0; i-- ) bpk_buf_printf(t->o, "</%s>", l->span[i].name);
}

LOCAL void spans_open( TX *t )
{
	LEVEL	*l = top(t);
	INT	i;

	if ( l == NULL ) return;
	for ( i = 0; i < l->nspan; i++ ) span_emit(t, &l->span[i]);
}

LOCAL INT span_find( LEVEL *l, const char *name )
{
	INT	i;

	for ( i = l->nspan - 1; i >= 0; i-- ) {
		if ( strcmp(l->span[i].name, name) == 0 ) return i;
	}
	return -1;
}

/*
 * The lowest of the open spans that draw a line or a ground round the
 * letters -- a virtual object is kept out of those -- or the count when
 * none is open.
 */
LOCAL INT span_lined( TX *t )
{
	static const char *const lined[] = {
		"underline", "overline", "strikethrough", "box", "invert", "mesh", "background", "noprint", NULL
	};
	LEVEL	*l = top(t);
	INT	i, k;

	if ( l == NULL ) return 0;
	for ( i = 0; i < l->nspan; i++ ) {
		for ( k = 0; lined[k] != NULL; k++ ) {
			if ( strcmp(l->span[i].name, lined[k]) == 0 ) return i;
		}
	}
	return l->nspan;
}

/* An element opened: in a text, and not open already unless it may repeat */
LOCAL void span_on( TX *t, const char *name, const char *attrs, BOOL again )
{
	LEVEL	*l = top(t);
	SPAN	*s;

	if ( l == NULL || l->kind != 'T' ) return;
	if ( !again && span_find(l, name) >= 0 ) return;
	if ( l->nspan >= SPAN_MAX ) return;
	s = &l->span[l->nspan++];
	strncpy(s->name, name, sizeof(s->name) - 1);
	s->name[sizeof(s->name) - 1] = 0;
	strncpy(s->attrs, ( attrs != NULL ) ? attrs : "", sizeof(s->attrs) - 1);
	s->attrs[sizeof(s->attrs) - 1] = 0;
	span_emit(t, s);
}

/* The innermost element of that name closed; those inside it opened again */
LOCAL void span_off( TX *t, const char *name )
{
	LEVEL	*l = top(t);
	INT	k, i;

	if ( l == NULL || ( k = span_find(l, name) ) < 0 ) return;
	for ( i = l->nspan - 1; i >= k; i-- ) bpk_buf_printf(t->o, "</%s>", l->span[i].name);
	memmove(&l->span[k], &l->span[k + 1], sizeof(SPAN) * (size_t)( l->nspan - k - 1 ));
	l->nspan--;
	for ( i = k; i < l->nspan; i++ ) span_emit(t, &l->span[i]);
}

LOCAL void level_push( TX *t, char kind )
{
	LEVEL	*l;

	if ( t->sp >= LEVEL_MAX ) return;
	l = &t->lv[t->sp++];
	l->kind = kind;
	l->para = FALSE;
	l->plane = t->cv->plane;
	l->nspan = 0;
	l->head = -1;
	/* the units of what it is in, until its own start says */
	l->hunit = ( t->sp > 1 ) ? t->lv[t->sp - 2].hunit : -120;
	l->vunit = ( t->sp > 1 ) ? t->lv[t->sp - 2].vunit : -120;
}

LOCAL void level_pop( TX *t )
{
	if ( t->sp <= 0 ) return;
	t->cv->plane = t->lv[t->sp - 1].plane;
	t->sp--;
}

/*
 * The characters with no Unicode waiting, written: 〓 for each, in a
 * <tchar> that keeps their plane and their codes.
 */
LOCAL void tchar_flush( TX *t )
{
	INT	i;

	if ( t->ntcode == 0 ) return;
	bpk_buf_printf(t->o, "<tchar plane=\"%d\" code=\"", t->tplane);
	for ( i = 0; i < t->ntcode; i++ ) bpk_buf_printf(t->o, "%s%04x", i ? " " : "", t->tcode[i]);
	bpk_buf_puts(t->o, "\">");
	for ( i = 0; i < t->ntcode; i++ ) bpk_buf_cp(t->o, UC_GETA);
	bpk_buf_puts(t->o, "</tchar>");
	t->ntcode = 0;
}

/*
 * A setting of the page, before the text's first letter: the text's
 * first <p> is taken back so that the setting stands outside it, and
 * page_end writes the <p> again after it. FALSE when anything has been
 * written into the paragraph: the setting then stands where it is.
 */
LOCAL BOOL page_begin( TX *t )
{
	LEVEL	*l = top(t);

	if ( l == NULL || l->kind != 'T' || l->head < 0 || t->o->s == NULL || t->o->n != l->head + 3 ) return FALSE;
	t->o->n = l->head;
	t->o->s[t->o->n] = 0;
	return TRUE;
}

LOCAL void page_end( TX *t, BOOL moved )
{
	LEVEL	*l = top(t);

	if ( !moved || l == NULL ) return;
	l->head = t->o->n;
	bpk_buf_puts(t->o, "<p>");
}

/* ---------------------------------------------------------------- text */

LOCAL void text_start( TX *t )
{
	BOOL	infig = (BOOL)( top(t) != NULL && top(t)->kind == 'F' );
	INT	hunit = S16(t, 8), vunit = S16(t, 9);

	level_push(t, 'T');
	if ( top(t) != NULL ) {
		top(t)->hunit = hunit;
		top(t)->vunit = vunit;
	}
	t->cv->plane = 1;
	t->started = TRUE;
	bpk_buf_puts(t->o, "<document>\n");
	if ( infig ) {
		bpk_buf_printf(t->o, "<docView viewleft=\"%d\" viewtop=\"%d\" viewright=\"%d\" viewbottom=\"%d\"/>\n",
			  S16(t, 0), S16(t, 1), S16(t, 2), S16(t, 3));
		bpk_buf_printf(t->o, "<docDraw drawleft=\"%d\" drawtop=\"%d\" drawright=\"%d\" drawbottom=\"%d\"/>\n",
			  S16(t, 4), S16(t, 5), S16(t, 6), S16(t, 7));
	}
	bpk_buf_printf(t->o, "<docScale hunit=\"%d\" vunit=\"%d\"/>\n", hunit, vunit);
	bpk_buf_printf(t->o, "<text lang=\"%u\" bpat=\"%u\"", W16(t, 10), W16(t, 11));
	if ( infig ) bpk_buf_printf(t->o, " zIndex=\"%d\"", ++t->z);
	bpk_buf_puts(t->o, "/>\n");
	top(t)->head = t->o->n;
	bpk_buf_puts(t->o, "<p>");
	top(t)->para = TRUE;
}

LOCAL void text_end( TX *t )
{
	LEVEL	*l = top(t);

	if ( l == NULL || l->kind != 'T' ) return;
	tchar_flush(t);
	calc_close(t);
	spans_close(t);
	l->nspan = 0;
	if ( l->para ) bpk_buf_puts(t->o, "</p>\n");
	bpk_buf_puts(t->o, "</document>\n");
	t->ruby = FALSE;
	level_pop(t);
}

/* A paragraph ends: the open decorations close and open again in the next */
LOCAL void text_newline( TX *t )
{
	LEVEL	*l = top(t);

	calc_close(t);
	spans_close(t);
	bpk_buf_puts(t->o, l->para ? "</p>\n<p>" : "<p>");
	l->para = TRUE;
	spans_open(t);
}

LOCAL void text_char( TX *t, UINT ch )
{
	UINT	u[2], cp;
	INT	n, i;

	n = bpk_tron_chars(ch, &t->cv->plane, u);
	if ( n == BPK_TRON_NOUCS ) {
		/* kept with its plane and code, as many in a row as come */
		if ( t->ntcode > 0 && ( t->tplane != t->cv->plane || t->ntcode >= TCHAR_MAX ) ) tchar_flush(t);
		t->tplane = t->cv->plane;
		t->tcode[t->ntcode++] = (UH)ch;
		return;
	}
	if ( n == 0 ) return;
	tchar_flush(t);
	if ( ch == TC_NL || ch == TC_CR ) {
		if ( in_text(t) ) text_newline(t);
		return;
	}
	if ( ch == TC_FF ) {
		if ( in_text(t) ) bpk_buf_puts(t->o, "<pagebreak/>");
		return;
	}
	for ( i = 0; i < n; i++ ) {
		cp = u[i];
		if ( cp == TC_TAB ) {
			bpk_buf_putc(t->o, '\t');
			continue;
		}
		if ( cp < 0x20 ) continue;
		switch ( cp ) {
		case '&':	bpk_buf_puts(t->o, "&amp;");		break;
		case '<':	bpk_buf_puts(t->o, "&lt;");		break;
		case '>':	bpk_buf_puts(t->o, "&gt;");		break;
		case '"':	bpk_buf_puts(t->o, "&quot;");	break;
		default:	bpk_buf_cp(t->o, cp);		break;
		}
	}
}

/*
 * 用紙オーバーレイ定義: its text, a TAD of its own with no text start,
 * written as the paragraphs of the <paper-overlay-define> it is in.
 */
LOCAL void overlay_text( TX *t, const UB *d, UINT len )
{
	const UB	*sd = t->d;
	UINT		slen = t->len, sid = t->id;
	INT		sp = t->sp;

	level_push(t, 'T');
	if ( t->sp == sp ) return;
	t->cv->plane = 1;
	bpk_buf_puts(t->o, "<p>");
	top(t)->para = TRUE;
	walk(t, d, len, TRUE, segment);
	while ( t->sp > sp + 1 ) {
		if ( top(t)->kind == 'T' ) text_end(t);
		else fig_end(t);
	}
	tchar_flush(t);
	calc_close(t);
	spans_close(t);
	if ( top(t)->para ) bpk_buf_puts(t->o, "</p>");
	t->ruby = FALSE;
	level_pop(t);
	t->d = sd;
	t->len = slen;
	t->id = sid;
}

/* 用紙オーバーレイ指定: the overlays put on, by number, the top bit overlay 0 */
LOCAL void overlay_list( TX *t, UINT bits )
{
	INT	i, k = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( bits & ( 0x8000U >> i ) ) bpk_buf_printf(t->o, "%s%d", k++ ? ", " : "", i);
	}
}

/* 用紙指定, マージン指定 and the rest of the page's 付箋 */
LOCAL void text_page( TX *t, UINT sub )
{
	UINT	attr = W16(t, 0) & 0xFF;
	BOOL	moved;

	switch ( sub ) {
	case 0:
		if ( t->figtad || t->len < 14 ) break;
		moved = page_begin(t);
		bpk_buf_printf(t->o, "<paper type=\"doc\" length=\"%u\" width=\"%u\" binding=\"%u\" imposition=\"%u\""
			  " top=\"%u\" bottom=\"%d\" left=\"%d\" right=\"%d\" />\n",
			  W16(t, 1), W16(t, 2), ( attr & 2 ) ? 1U : 0U, attr & 1, W16(t, 3), S16(t, 4), S16(t, 5), S16(t, 6));
		page_end(t, moved);
		return;
	case 1:
		if ( t->figtad || t->len < 10 ) break;
		moved = page_begin(t);
		bpk_buf_printf(t->o, "<docmargin top=\"%u\" bottom=\"%u\" left=\"%u\" right=\"%u\" />\n",
			  W16(t, 1), W16(t, 2), W16(t, 3), W16(t, 4));
		page_end(t, moved);
		return;
	case 3:
		/* xxPPNNNN: P 0 every page, 1 the odd, 2 the even; N its number */
		if ( t->figtad || t->len < 2 ) break;
		moved = page_begin(t);
		bpk_buf_printf(t->o, "<paper-overlay-define N=\"%u\" P=\"%u\">", attr & 0x0F, ( attr >> 4 ) & 3);
		overlay_text(t, t->d + 2, t->len - 2);
		bpk_buf_puts(t->o, "</paper-overlay-define>\n");
		page_end(t, moved);
		return;
	case 4:
		if ( t->figtad || t->len < 4 ) break;
		moved = page_begin(t);
		bpk_buf_puts(t->o, "<docoverlay active=\"");
		overlay_list(t, W16(t, 1));
		bpk_buf_puts(t->o, "\"/>\n");
		page_end(t, moved);
		return;
	case 2:
		if ( t->figtad || t->len < 4 ) break;
		bpk_buf_printf(t->o, "<column column=\"%u\" balance=\"%u\" colsp=\"%u\"", attr & 0x0F, ( attr >> 7 ) & 1, W16(t, 1));
		if ( t->len > 4 ) {
			UINT	hi = W16(t, 2) >> 8, lo = W16(t, 2) & 0xFF, ld = ( lo >> 4 ) & 0x0F, hd = ( hi >> 4 ) & 0x0F;

			bpk_buf_printf(t->o, " colline=\"%u\" linenum=\"%u\" lineDensity=\"%u\" lineWidth=\"%u\" lineType=\"%u\""
				  " colline2=\"%u\" linenum2=\"%u\" lineDensity2=\"%u\" lineWidth2=\"%u\" lineType2=\"%u\"",
				  ld, ( ld >> 2 ) & 1, ( ld >> 3 ) & 1, ld & 3, lo & 0x0F,
				  hd, ( hd >> 2 ) & 1, ( hd >> 3 ) & 1, hd & 3, hi & 0x0F);
		}
		bpk_buf_puts(t->o, " />\n");
		return;
	case 5:
		if ( t->figtad || t->len < 10 ) break;
		bpk_buf_printf(t->o, "<frame-open abs=\"%u\" halign=\"%u\" valign=\"%u\" page=\"%u\" wrap=\"%u\""
			  " top=\"%d\" left=\"%d\" bottom=\"%d\" right=\"%d\" />\n",
			  ( attr >> 7 ) & 1, ( attr >> 2 ) & 3, attr & 3, ( attr >> 6 ) & 1, ( attr >> 5 ) & 1,
			  S16(t, 1), S16(t, 2), S16(t, 3) - 1, S16(t, 4) - 1);
		return;
	case 6:
		if ( t->figtad || t->len < 4 ) break;
		t->pg_step = (INT)(B)attr;
		t->pg_num = (INT)W16(t, 1);
		bpk_buf_printf(t->o, "<page-number step=\"%d\" num=\"%u\" />\n", (INT)(B)attr, W16(t, 1));
		return;
	case 7:
		if ( t->figtad || t->len < 4 ) break;
		bpk_buf_printf(t->o, "<pagebreak cond=\"%u\" remain=\"%u\" />\n", attr, W16(t, 1));
		return;
	case 8:
		if ( t->figtad ) break;
		bpk_buf_puts(t->o, "<fill-line />\n");
		return;
	default:
		break;
	}
	skip(t, sub);		/* what a figure record does not use */
}

/* 行書式指定付箋 */
LOCAL void text_ruler( TX *t, UINT sub )
{
	UINT	attr = W16(t, 0) & 0xFF, v = W16(t, 1);
	INT	i, n;
	static const char *const align[] = { "left", "center", "right", "justify", "justify-all" };

	switch ( sub ) {
	case 0: {
		UINT	a = ( v >> 8 ) & 0x7F, b = v & 0xFF;

		if ( b == 0 ) a = b = 1;
		bpk_buf_puts(t->o, "<text line-height=\"");
		if ( v & 0x8000 ) bpk_buf_printf(t->o, "%u", ( v & 0x7FFF ) + 1);
		else bpk_buf_num(t->o, (INT)( 1000 + ( a * 1000 + b / 2 ) / b ));
		bpk_buf_puts(t->o, "\"/>");
		return;
	}
	case 1:
		bpk_buf_printf(t->o, "<text align=\"%s\"/>", ( attr <= 4 ) ? align[attr] : "left");
		return;
	case 2:
		n = S16(t, 6);
		bpk_buf_printf(t->o, "<tab-format R=\"%u\" P=\"%u\" height=\"", ( attr >> 7 ) & 1, attr & 3);
		scale_attr(t, W16(t, 1), FALSE);
		bpk_buf_puts(t->o, "\" pargap=\"");
		scale_attr(t, W16(t, 2), FALSE);
		bpk_buf_printf(t->o, "\" left=\"%d\" right=\"%d\" indent=\"%d\" ntabs=\"%d\"", S16(t, 3), S16(t, 4), S16(t, 5), n);
		if ( n > 0 ) {
			bpk_buf_puts(t->o, " tabs=\"");
			for ( i = 0; i < n && 7 + i < nwords(t); i++ ) bpk_buf_printf(t->o, "%s%d", i ? "," : "", S16(t, 7 + i));
			bpk_buf_puts(t->o, "\"");
		}
		bpk_buf_puts(t->o, "/>");
		return;
	case 3:
		if ( t->len < 10 ) break;
		n = S16(t, 4);
		bpk_buf_printf(t->o, "<field-format R=\"%u\" P=\"%u\" height=\"", ( attr >> 7 ) & 1, attr & 3);
		scale_attr(t, W16(t, 1), FALSE);
		bpk_buf_puts(t->o, "\" pargap=\"");
		scale_attr(t, W16(t, 2), FALSE);
		bpk_buf_printf(t->o, "\" line=\"%u\" nfld=\"%d\">", W16(t, 3), n);
		for ( i = 0; i < n && 5 + i * 5 + 4 < nwords(t); i++ ) {
			INT	b = 5 + i * 5;

			bpk_buf_printf(t->o, "<field fld=\"%d\" left=\"%d\" right=\"%d\" margin=\"%d\" f_attr=\"%u\" />",
				  S16(t, b), S16(t, b + 1), S16(t, b + 2), S16(t, b + 3), W16(t, b + 4));
		}
		bpk_buf_puts(t->o, "</field-format>");
		return;
	case 4:
		bpk_buf_printf(t->o, "<text direction=\"%u\"/>", attr);
		return;
	case 5:
		calc_close(t);
		bpk_buf_puts(t->o, "<tab/>");
		return;
	default:
		break;
	}
	skip(t, sub);
}

/* フォント属性: the decorations it turns on and off, then the font's look */
LOCAL void font_type( TX *t )
{
	UINT		a = W16(t, 1);
	UINT		line = ( a >> 9 ) & 7, ital = ( a >> 6 ) & 7, wt = ( a >> 3 ) & 7, wd = a & 7;
	const char	*style = "normal", *stretch = "normal";
	INT		weight = 400, scale = 1000;
	INT		bold = -1;		/* unchanged */

	if ( line >= 1 && line <= 3 ) span_on(t, "bagchar", NULL, FALSE);
	else span_off(t, "bagchar");

	switch ( ital ) {
	case 1: case 2: case 3:	style = "italic";		break;
	case 5:			style = "oblique 10deg";	break;
	case 6:			style = "oblique 15deg";	break;
	case 7:			style = "oblique 20deg";	break;
	default:		break;
	}
	if ( strcmp(style, "normal") != 0 ) span_on(t, "i", NULL, FALSE);
	else span_off(t, "i");

	switch ( wt ) {
	case 0:	weight = 400; bold = 0;	break;
	case 1:	weight = 100; bold = 0;	break;
	case 2:	weight = 300; bold = 0;	break;
	case 4:	weight = 500; bold = 1;	break;
	case 5:	weight = 700; bold = 1;	break;
	case 6:	weight = 800; bold = 1;	break;
	case 7:	weight = 900; bold = 1;	break;
	default: break;
	}
	if ( bold == 1 ) span_on(t, "strong", NULL, FALSE);
	else if ( bold == 0 ) span_off(t, "strong");

	switch ( wd ) {
	case 1:	stretch = "condensed";		scale = 800;	break;
	case 2:	stretch = "extra-condensed";	scale = 600;	break;
	case 3:	stretch = "ultra-condensed";	scale = 500;	break;
	case 5:	stretch = "extra-expanded";	scale = 1500;	break;
	case 6:	stretch = "ultra-expanded";	scale = 2000;	break;
	default: break;
	}
	bpk_buf_printf(t->o, "<font style=\"%s\" weight=\"%d\" stretch=\"%s\" stretchscale=\"", style, weight, stretch);
	bpk_buf_num(t->o, scale);
	bpk_buf_puts(t->o, "\"/>");
}

/* 文字指定付箋 */
LOCAL void text_font( TX *t, UINT sub )
{
	UINT	v = W16(t, 1);
	char	c[8];

	switch ( sub ) {
	case 0:
		if ( t->len < 4 ) break;
		bpk_buf_puts(t->o, "<font face=\"");
		tron_attr(t, 2);
		bpk_buf_puts(t->o, "\"/>");
		return;
	case 1:
		if ( t->len < 4 ) break;
		font_type(t);
		return;
	case 2:
		t->fontsize = chsize_milli(t, v);
		bpk_buf_puts(t->o, "<font size=\"");
		bpk_buf_num(t->o, t->fontsize);
		bpk_buf_puts(t->o, "\"/>");
		return;
	case 3: {
		UINT	ha = ( v >> 8 ) & 0xFF, hb = v & 0xFF, w = W16(t, 2), wa = ( w >> 8 ) & 0xFF, wb = w & 0xFF;

		if ( t->len < 6 ) break;
		if ( hb == 0 ) ha = hb = 1;
		if ( wb == 0 ) wa = wb = 1;
		bpk_buf_printf(t->o, "<font hRatio=\"%u/%u\" wRatio=\"%u/%u\"/>", ha, hb, wa, wb);
		return;
	}
	case 4: {
		UINT	attr = W16(t, 0) & 0xFF;

		bpk_buf_printf(t->o, "<font direction=\"%u\" kerning=\"%u\" pattern=\"%u\" space=\"",
			  ( attr >> 7 ) & 1, ( attr >> 6 ) & 1, attr & 1);
		if ( v & 0x8000 ) {
			bpk_buf_printf(t->o, "%u", v & 0x7FFF);
		} else {
			UINT	a = ( v >> 8 ) & 0x7F, b = v & 0xFF;

			if ( b == 0 ) a = b = 1;
			bpk_buf_num(t->o, (INT)( ( a * 1000 + b / 2 ) / b ));
		}
		bpk_buf_puts(t->o, "\"/>");
		return;
	}
	case 5:
		/* 文字回転: degrees, and whether against the paper rather than the line */
		if ( t->len < 4 ) break;
		bpk_buf_printf(t->o, "<font rotation=\"%u\" rotabs=\"%u\"/>", v, W16(t, 0) & 0xFF);
		return;
	case 6:
		if ( t->len < 4 ) break;
		colour(t, C32(t, 1), c);
		bpk_buf_printf(t->o, "<font color=\"%s\"/>", c);
		return;
	case 7:
		/* 文字基準位置移動: how far the letters' base moves, as a SCALE */
		if ( t->len < 4 ) break;
		bpk_buf_puts(t->o, "<font baseshift=\"");
		scale_attr(t, v, FALSE);
		bpk_buf_printf(t->o, "\" baseattr=\"%u\"/>", W16(t, 0) & 0xFF);
		return;
	default:
		break;
	}
	skip(t, sub);
}

/* 特殊文字指定付箋 */
LOCAL void text_char_seg( TX *t, UINT sub )
{
	switch ( sub ) {
	case 0:
		bpk_buf_puts(t->o, "<fixed-space width=\"");
		scale_attr(t, W16(t, 1), TRUE);
		bpk_buf_puts(t->o, "\" />");
		return;
	case 1:
		if ( t->figtad ) break;
		bpk_buf_puts(t->o, "<fill-char str=\"");
		tron_attr(t, 1);
		bpk_buf_puts(t->o, "\" />\n");
		return;
	default:
		break;
	}
	skip(t, sub);		/* 文字罫線 */
}

/* 文字割付け指定付箋: 結合, 割付け, 添え字, ルビ, 禁則 */
LOCAL void text_attr( TX *t, UINT sub )
{
	UINT	low = W16(t, 0) & 0xFF;
	char	a[ATTR_MAX];

	switch ( sub ) {
	case 0:
		span_on(t, "combchar", NULL, TRUE);
		return;
	case 1:
		span_off(t, "combchar");
		return;
	case 2: {
		UINT	w = W16(t, 1);

		if ( t->figtad || t->len < 4 ) break;
		if ( w & 0x8000 ) snprintf(a, sizeof(a), " kind=\"%u\" width=\"abs:%u\"", low, w & 0x7FFF);
		else if ( ( w & 0xFF ) == 0 ) snprintf(a, sizeof(a), " kind=\"%u\" width=\"1/1\"", low);
		else snprintf(a, sizeof(a), " kind=\"%u\" width=\"%u/%u\"", low, ( w >> 8 ) & 0x7F, w & 0xFF);
		span_on(t, "char-layout", a, TRUE);
		return;
	}
	case 3:
		if ( t->figtad ) break;
		span_off(t, "char-layout");
		return;
	case 4: {
		UINT	f = ( low >> 3 ) & 1, unit = ( low >> 2 ) & 1, type = low & 3;

		if ( t->len > 2 ) {
			if ( unit == 1 ) t->sub_target = (INT)( W16(t, 1) & 1 );
			else t->sub_base = (INT)( W16(t, 1) & 7 );
		}
		snprintf(a, sizeof(a), " type=\"%u\" position=\"%u\" unit=\"%u\" targetPosition=\"%d\" baseline=\"%d\"",
			 type, f, unit, t->sub_target, t->sub_base);
		span_on(t, "attend", a, TRUE);
		return;
	}
	case 5:
		span_off(t, "attend");
		return;
	case 6: {
		BPKBUF	*keep = t->o, rt;

		bpk_buf_init(&rt);
		t->o = &rt;
		tron_attr(t, 1);
		t->o = keep;
		snprintf(a, sizeof(a), " position=\"%u\" text=\"%s\"", low & 1, ( rt.s != NULL ) ? rt.s : "");
		bpk_buf_free(&rt);
		span_on(t, "ruby", a, TRUE);
		t->ruby = TRUE;
		return;
	}
	case 7:
		if ( t->ruby ) span_off(t, "ruby");
		t->ruby = FALSE;
		return;
	case 8:
	case 9: {
		/* 行頭禁則, 行末禁則: the kind (禁則方式 << 4 | 禁則レベル) and the letters */
		BOOL	moved = page_begin(t);

		bpk_buf_printf(t->o, "<%s kind=\"0x%02x\" ch=\"", ( sub == 8 ) ? "line-head-kinsoku" : "line-tail-kinsoku", low);
		tron_attr(t, 1);
		bpk_buf_puts(t->o, moved ? "\"/>\n" : "\"/>");
		page_end(t, moved);
		return;
	}
	case 0x0A:
		bpk_buf_puts(t->o, "<fixed-space width=\"");
		scale_attr(t, W16(t, 1), TRUE);
		bpk_buf_puts(t->o, "\" />");
		return;
	default:
		break;
	}
	skip(t, sub);
}

/*
 * 変数参照: the page's number (variable 200) a <page-number> counted as
 * ページ番号指定 last said, any other a <variable> by its number or its
 * name, for whoever shows the text to fill in.
 */
LOCAL void text_var( TX *t, UINT sub )
{
	if ( sub == 0 && t->len >= 4 ) {
		INT	id = S16(t, 1);

		if ( id == 200 ) bpk_buf_printf(t->o, "<page-number num=\"%d\" step=\"%d\"/>", t->pg_num, t->pg_step);
		else bpk_buf_printf(t->o, "<variable id=\"%d\"/>", id);
		return;
	}
	if ( sub == 1 ) {
		bpk_buf_puts(t->o, "<variable name=\"");
		tron_attr(t, 1);
		bpk_buf_puts(t->o, "\"/>");
		return;
	}
	skip(t, sub);
}

/* ---------------------------------------------------------------- 基本表計算's cells */

/* The decorations a cell opened, closed at its end: a tab or a new line */
LOCAL void calc_close( TX *t )
{
	while ( t->ncalc > 0 ) span_off(t, t->calc[--t->ncalc]);
}

/*
 * A cell of 基本表計算, the 指定付箋 it keeps in the text before each:
 * its place as <calcPos cell="B3"/>, its size, decorations, rules and
 * colour. The data holds the row, the column, then a word of size (low
 * byte) and decorations, and a word of rules and colour.
 */
LOCAL void calc_cell( TX *t )
{
	static const UINT	size_of[7] = { 0, 6, 9, 0, 24, 36, 48 };
	static const char *const deco_name[5] = { "bold", "italic", "underline", "mesh", "invert" };
	static const UINT	deco_bit[5] = { 0x01, 0x02, 0x04, 0x20, 0x40 };
	static const char *const col_of[8] = {
		"#ffffff", NULL, "#ff0000", "#00ff00", "#0000ff", "#ffff00", "#ff00ff", "#00ffff"
	};
	static const char *const rule_of[4] = { "", "line", "double", "dot" };
	const INT	at = DF_DATA / 2;
	UINT		row = W16(t, at + 1), col = W16(t, at + 2);
	UINT		fs = W16(t, at + 4) & 0xFF, deco = W16(t, at + 4) >> 8;
	UINT		rule = W16(t, at + 5) & 0xFF, colour = W16(t, at + 5) >> 8;
	char		letters[8];
	INT		n = 0, i, c = (INT)col - 1;

	calc_close(t);
	do {
		letters[n++] = (char)( 'A' + c % 26 );
		c = c / 26 - 1;
	} while ( c >= 0 && n < 6 );
	bpk_buf_puts(t->o, "<calcPos cell=\"");
	while ( n > 0 ) bpk_buf_putc(t->o, letters[--n]);
	bpk_buf_printf(t->o, "%u\"/>", row);
	if ( fs < 7 && size_of[fs] != 0 ) bpk_buf_printf(t->o, "<font size=\"%u\"/>", size_of[fs]);
	for ( i = 0; i < 5 && t->ncalc < 4; i++ ) {
		if ( ( deco & deco_bit[i] ) == 0 ) continue;
		span_on(t, deco_name[i], NULL, TRUE);
		strcpy(t->calc[t->ncalc++], deco_name[i]);
	}
	if ( rule != 0 ) {
		bpk_buf_puts(t->o, "<calcCell");
		if ( ( rule & 0x0F ) != 0 ) bpk_buf_printf(t->o, " borderLeft=\"1\" borderLeftType=\"%s\"", rule_of[( rule & 0x0F ) < 4 ? rule & 0x0F : 1]);
		if ( ( rule >> 4 ) != 0 ) bpk_buf_printf(t->o, " borderTop=\"1\" borderTopType=\"%s\"", rule_of[( rule >> 4 ) < 4 ? rule >> 4 : 1]);
		bpk_buf_puts(t->o, "/>");
	}
	if ( colour < 8 && col_of[colour] != NULL ) bpk_buf_printf(t->o, "<font color=\"%s\"/>", col_of[colour]);
}

/* 指定付箋 in a record: a cell of a table in a text, else kept */
LOCAL void dfusen( TX *t )
{
	if ( in_text(t) && t->len >= DF_DATA + 12 && W16(t, DF_APPL / 2) == BPK_APPL_HI
	  && W16(t, DF_APPL / 2 + 1) == APPL_CALC && W16(t, DF_APPL / 2 + 2) == BPK_APPL_HI ) {
		calc_cell(t);
		return;
	}
	skip(t, 0);
}

/* 文章・図形アプリケーション指定付箋: kept for the application, its ID said */
LOCAL void appl_fusen( TX *t, const char *name )
{
	char	a[48];

	snprintf(a, sizeof(a), " appl=\"%04x-%04x-%04x\"", W16(t, 1), W16(t, 2), W16(t, 3));
	if ( !keep(t, name, a) ) skip(t, W16(t, 0) >> 8);
}

/* 文字修飾指定付箋: even sub ids begin, odd ones end */
LOCAL void text_style( TX *t, UINT sub )
{
	static const char *const names[10] = {
		"underline", "overline", "strikethrough", "box", NULL, NULL, "invert", "mesh", "background", "noprint"
	};
	UINT		k = sub >> 1;

	if ( k == 4 || k == 5 ) {
		/* 傍点, above (right) or below (left) */
		char	a[48];

		if ( t->figtad ) {
			skip(t, sub);
			return;
		}
		if ( ( sub & 1 ) == 0 ) {
			snprintf(a, sizeof(a), " side=\"%s\" kind=\"%u\"", ( k == 4 ) ? "upper" : "lower", W16(t, 0) & 0x0F);
			span_on(t, "bouten", a, TRUE);
		} else {
			span_off(t, "bouten");
		}
		return;
	}
	if ( k >= 10 || names[k] == NULL ) {
		skip(t, sub);
		return;
	}
	if ( ( sub & 1 ) == 0 ) span_on(t, names[k], NULL, FALSE);
	else span_off(t, names[k]);
}

/* ---------------------------------------------------------------- figures */

LOCAL void fig_start( TX *t )
{
	if ( !t->started ) {
		t->started = TRUE;
		t->figtad = TRUE;
	}
	level_push(t, 'F');
	if ( top(t) != NULL ) {
		top(t)->hunit = S16(t, 8);
		top(t)->vunit = S16(t, 9);
	}
	t->z = 0;
	bpk_buf_puts(t->o, "<figure>\n");
	bpk_buf_printf(t->o, "<figView left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\"/>\n", S16(t, 0), S16(t, 1), S16(t, 2), S16(t, 3));
	bpk_buf_printf(t->o, "<figDraw left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\"/>\n", S16(t, 4), S16(t, 5), S16(t, 6), S16(t, 7));
	bpk_buf_printf(t->o, "<figScale hunit=\"%d\" vunit=\"%d\"/>\n", S16(t, 8), S16(t, 9));
}

LOCAL void fig_end( TX *t )
{
	LEVEL	*l = top(t);

	if ( l == NULL || l->kind != 'F' ) return;
	bpk_buf_puts(t->o, "</figure>\n");
	level_pop(t);
}

/* The line attribute (type and width) and line pattern every element begins with */
LOCAL void line_attrs( TX *t, INT at )
{
	UINT	la = W16(t, at);

	bpk_buf_printf(t->o, " lineType=\"%u\" lineWidth=\"%u\" l_pat=\"%u\"", ( la >> 8 ) & 0xFF, la & 0xFF, W16(t, at + 1));
}

LOCAL void arrows( TX *t )
{
	bpk_buf_printf(t->o, " start_arrow=\"%d\" end_arrow=\"%d\" arrow_type=\"simple\"", t->arrow_s ? 1 : 0, t->arrow_e ? 1 : 0);
}

LOCAL void points( TX *t, INT from, INT np )
{
	INT	i;

	bpk_buf_puts(t->o, " points=\"");
	for ( i = 0; i < np && from + 2 * i + 1 < nwords(t); i++ ) {
		bpk_buf_printf(t->o, "%s%d,%d", i ? " " : "", S16(t, from + 2 * i), S16(t, from + 2 * i + 1));
	}
	bpk_buf_puts(t->o, "\"");
}

LOCAL void frame4( TX *t, INT at )
{
	bpk_buf_printf(t->o, " frameLeft=\"%d\" frameTop=\"%d\" frameRight=\"%d\" frameBottom=\"%d\"",
		  S16(t, at), S16(t, at + 1), S16(t, at + 2), S16(t, at + 3));
}

LOCAL void zindex_end( TX *t )
{
	bpk_buf_printf(t->o, " zIndex=\"%d\" />\n", ++t->z);
}

/* 図形要素セグメント */
LOCAL void fig_prim( TX *t, UINT sub )
{
	INT	np;

	switch ( sub ) {
	case 0x00:	/* 長方形 */
		if ( t->len < 0x12 ) break;
		bpk_buf_puts(t->o, "<rect round=\"0\"");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" angle=\"%d\" left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\"",
			  W16(t, 3), S16(t, 4), S16(t, 5), S16(t, 6), S16(t, 7), S16(t, 8));
		zindex_end(t);
		return;
	case 0x01:	/* 角丸長方形 */
		if ( t->len < 0x16 ) break;
		bpk_buf_puts(t->o, "<rect round=\"1\"");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" angle=\"%d\" figRH=\"%d\" figRV=\"%d\" left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\"",
			  W16(t, 3), S16(t, 4), S16(t, 5), S16(t, 6), S16(t, 7), S16(t, 8), S16(t, 9), S16(t, 10));
		zindex_end(t);
		return;
	case 0x02:	/* 楕円 */
		if ( t->len < 0x12 ) break;
		bpk_buf_puts(t->o, "<ellipse");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" angle=\"%d\"", W16(t, 3), S16(t, 4));
		frame4(t, 5);
		zindex_end(t);
		return;
	case 0x03:	/* 扇形 */
	case 0x04:	/* 弓形 */
		if ( t->len < 0x18 ) break;
		bpk_buf_puts(t->o, ( sub == 3 ) ? "<arc" : "<chord");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" angle=\"%d\"", W16(t, 3), S16(t, 4));
		frame4(t, 5);
		bpk_buf_printf(t->o, " startX=\"%d\" startY=\"%d\" endX=\"%d\" endY=\"%d\"", S16(t, 9), S16(t, 10), S16(t, 11), S16(t, 12));
		arrows(t);
		zindex_end(t);
		return;
	case 0x05:	/* 多角形 */
		if ( t->len < 0x16 ) break;
		np = S16(t, 5);
		bpk_buf_puts(t->o, "<polygon");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" round=\"%d\" np=\"%d\"", W16(t, 3), S16(t, 4), np);
		points(t, 6, np);
		zindex_end(t);
		return;
	case 0x06:	/* 直線 */
		if ( t->len < 0x0E ) break;
		bpk_buf_puts(t->o, "<line");
		line_attrs(t, 1);
		bpk_buf_puts(t->o, " f_pat=\"0\"");
		arrows(t);
		points(t, 3, ( nwords(t) - 3 ) / 2);
		zindex_end(t);
		return;
	case 0x07:	/* 楕円弧 */
		if ( t->len < 0x18 ) break;
		bpk_buf_puts(t->o, "<elliptical_arc");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " angle=\"%d\"", S16(t, 3));
		frame4(t, 4);
		bpk_buf_printf(t->o, " startX=\"%d\" startY=\"%d\" endX=\"%d\" endY=\"%d\"", S16(t, 8), S16(t, 9), S16(t, 10), S16(t, 11));
		arrows(t);
		zindex_end(t);
		return;
	case 0x08:	/* 折れ線 */
		if ( t->len < 0x0A ) break;
		np = S16(t, 4);
		bpk_buf_puts(t->o, "<polyline");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " round=\"%d\"", S16(t, 3));
		arrows(t);
		points(t, 5, np);
		zindex_end(t);
		return;
	case 0x09: {	/* 曲線 */
		INT	x0, y0, xn, yn;

		np = (INT)W16(t, 5);
		if ( t->len < 0x0C || ( W16(t, 0) & 0xFF ) != 0 || np < 2 || (INT)t->len < 12 + np * 4 ) break;
		x0 = S16(t, 6);
		y0 = S16(t, 7);
		xn = S16(t, 6 + 2 * ( np - 1 ));
		yn = S16(t, 7 + 2 * ( np - 1 ));
		bpk_buf_puts(t->o, "<curve");
		line_attrs(t, 1);
		bpk_buf_printf(t->o, " f_pat=\"%u\" type=\"%u\" closed=\"%d\"", W16(t, 3), W16(t, 4), ( x0 == xn && y0 == yn ) ? 1 : 0);
		arrows(t);
		points(t, 6, np);
		zindex_end(t);
		return;
	}
	case 0x0A:	/* マーカー列 */
		np = S16(t, 2);
		if ( np <= 0 || (INT)t->len < 3 + np * 2 ) break;
		bpk_buf_printf(t->o, "<marker mode=\"%u\" markerId=\"%d\"", W16(t, 0) & 0xFF, S16(t, 1));
		points(t, 3, np);
		bpk_buf_printf(t->o, " zIndex=\"%d\" />\n", t->z++);
		return;
	case 0x0B: {	/* 任意図形 */
		INT	i, nh = (INT)W16(t, 5);

		if ( t->len < 0x0E ) break;
		bpk_buf_printf(t->o, "<freefig mode=\"%u\" f_pat=\"%u\" sy=\"%u\" nr=\"%u\" bx=\"%d\" nh=\"%d\" h=\"",
			  W16(t, 0) & 0xFF, W16(t, 1), W16(t, 2), W16(t, 3), S16(t, 4), nh);
		for ( i = 0; i < nh && 6 + i < nwords(t); i++ ) bpk_buf_printf(t->o, "%s%u", i ? "," : "", W16(t, 6 + i));
		bpk_buf_printf(t->o, "\" zIndex=\"%d\" />\n", ++t->z);
		return;
	}
	default:
		break;
	}
	skip(t, sub);
}

/* データ定義セグメント: the colour map, masks, patterns, markers */
LOCAL void fig_def( TX *t, UINT sub )
{
	BPKCONV	*cv = t->cv;
	char	c[8];
	INT	i, n;

	switch ( sub ) {
	case 0:		/* カラーマップ */
		if ( t->len < 8 ) break;
		n = S16(t, 1);
		for ( i = 0; i < n && i < BPK_CMAP_MAX; i++ ) cv->cmap[i] = C32(t, 2 + 2 * i);
		if ( n > cv->ncmap ) cv->ncmap = ( n < BPK_CMAP_MAX ) ? n : BPK_CMAP_MAX;
		return;
	case 1: {	/* マスク */
		INT	hs = (INT)W16(t, 2), vs = (INT)W16(t, 3), wpr = ( hs + 15 ) / 16, y, k;

		if ( t->len < 8 || ( W16(t, 0) & 0xFF ) != 0 || hs <= 0 || vs <= 0 || 4 + wpr * vs > nwords(t) ) break;
		bpk_buf_printf(t->o, "<mask id=\"%u\" type=\"0\" width=\"%d\" height=\"%d\" data=\"", W16(t, 1), hs, vs);
		for ( y = 0; y < vs; y++ ) {
			for ( k = 0; k < wpr; k++ ) {
				UINT	w = W16(t, 4 + y * wpr + k);
				INT	bits = hs - k * 16;

				if ( bits < 16 ) w &= ~( ( 1U << ( 16 - bits ) ) - 1 ) & 0xFFFF;
				bpk_buf_printf(t->o, "%s%04x", ( y || k ) ? "," : "", w);
			}
		}
		bpk_buf_puts(t->o, "\" />\n");
		return;
	}
	case 2: {	/* パターン */
		INT	ncol = S16(t, 4), at;

		if ( t->len < 0x0E || ncol < 0 || 5 + ncol * 3 + 2 > nwords(t) ) break;
		bpk_buf_printf(t->o, "<pattern id=\"%d\" type=\"0\" width=\"%d\" height=\"%d\" ncol=\"%d\" fgcolors=\"",
			  S16(t, 1), S16(t, 2), S16(t, 3), ncol);
		for ( i = 0, at = 5; i < ncol; i++, at += 2 ) {
			UINT	raw = C32(t, at);

			colour(t, raw, c);
			bpk_buf_printf(t->o, "%s%s", i ? "," : "", ( raw & 0x80000000U ) ? "transparent" : c);
		}
		if ( C32(t, at) & 0x80000000U ) {
			bpk_buf_puts(t->o, "\" bgcolor=\"transparent");
		} else {
			colour(t, C32(t, at), c);
			bpk_buf_printf(t->o, "\" bgcolor=\"%s", c);
		}
		at += 2;
		bpk_buf_puts(t->o, "\" masks=\"");
		for ( i = 0; i < ncol; i++ ) bpk_buf_printf(t->o, "%s%d", i ? "," : "", S16(t, at + i));
		bpk_buf_puts(t->o, "\" />\n");
		return;
	}
	case 3: {	/* 線種定義: the line's pattern, a bit a pixel, the first the top bit */
		INT	nb = (INT)W16(t, 2);

		if ( t->len < 6 || nb <= 0 || 6 + (UINT)nb > t->len ) break;
		bpk_buf_printf(t->o, "<lineTypeDefine id=\"%u\" nb=\"%d\" mask=\"", W16(t, 1), nb);
		for ( i = 0; i < nb; i++ ) bpk_buf_printf(t->o, "%02x", t->d[6 + i]);
		bpk_buf_puts(t->o, "\" />\n");
		return;
	}
	case 4: {	/* マーカー */
		if ( t->len < 10 || ( W16(t, 0) & 0xFF ) != 0 ) break;
		colour(t, C32(t, 3), c);
		bpk_buf_printf(t->o, "<markerDefine type=\"0\" id=\"%u\" size=\"%u\" fgCol=\"%s\"", W16(t, 1), W16(t, 2), c);
		if ( W16(t, 1) > 4 && t->len >= 12 ) bpk_buf_printf(t->o, " mask=\"%u\"", W16(t, 5));
		bpk_buf_puts(t->o, " />\n");
		return;
	}
	default:
		break;
	}
	skip(t, sub);
}

/* 図形修飾セグメント: arrows for the next element, or a move of it */
LOCAL void fig_attr( TX *t, UINT sub )
{
	if ( sub == 0 ) {
		UINT	a = W16(t, 1);

		t->arrow_s = (BOOL)( ( a & 1 ) != 0 );
		t->arrow_e = (BOOL)( ( a & 2 ) != 0 );
		bpk_buf_printf(t->o, "<figmodifier arrow=\"%s%s\" />\n", t->arrow_s ? "S" : "", t->arrow_e ? "E" : "");
		return;
	}
	if ( sub == 1 && ( t->len == 6 || t->len == 8 || t->len == 10 ) ) {
		INT	va = ( t->len >= 10 ) ? S16(t, 4) : 0;

		if ( va <= -90 ) va = -89;
		if ( va >= 90 ) va = 89;
		bpk_buf_printf(t->o, "<transform dh=\"%d\" dv=\"%d\" hangle=\"%d\" vangle=\"%d\" />\n",
			  S16(t, 1), S16(t, 2), ( t->len >= 8 ) ? S16(t, 3) : 0, va);
		return;
	}
	skip(t, sub);
}

/*
 * 用紙オーバーレイ定義 of a figure: its shapes, a TAD of their own with
 * no figure start, written inside the <figoverlay> as the elements of a
 * figure. The words stay in overlayData as well, for whoever reads only
 * those.
 */
LOCAL void overlay_fig( TX *t, const UB *d, UINT len )
{
	const UB	*sd = t->d;
	UINT		slen = t->len, sid = t->id;
	INT		sp = t->sp, sz = t->z;

	level_push(t, 'F');
	if ( t->sp == sp ) return;
	t->z = 0;
	walk(t, d, len, TRUE, segment);
	while ( t->sp > sp + 1 ) {
		if ( top(t)->kind == 'T' ) text_end(t);
		else fig_end(t);
	}
	tchar_flush(t);
	level_pop(t);
	t->d = sd;
	t->len = slen;
	t->id = sid;
	t->z = sz;
}

/* 図形ページ割付け指定付箋 */
LOCAL void fig_page( TX *t, UINT sub )
{
	UINT	attr = W16(t, 0) & 0xFF;

	if ( !t->figtad ) {
		skip(t, sub);
		return;
	}
	switch ( sub ) {
	case 0:
		if ( t->len < 7 ) break;
		bpk_buf_printf(t->o, "<paper imposition=\"%u\" binding=\"%u\" length=\"%u\" width=\"%u\" top=\"%u\""
			  " bottom=\"%d\" left=\"%d\" right=\"%d\" />\n",
			  attr & 1, ( attr & 2 ) ? 1U : 0U, W16(t, 1), W16(t, 2), W16(t, 3), S16(t, 4), S16(t, 5), S16(t, 6));
		return;
	case 1:
		if ( t->len < 5 ) break;
		bpk_buf_printf(t->o, "<paper margintop=\"%u\" marginbottom=\"%u\" marginleft=\"%u\" marginright=\"%u\" />\n",
			  W16(t, 1), W16(t, 2), W16(t, 3), W16(t, 4));
		return;
	case 3: {
		/* 用紙オーバーレイ定義 of a figure: kept, its words as they are */
		UINT	pp = ( attr >> 4 ) & 3;
		INT	i;

		if ( t->len < 2 ) break;
		bpk_buf_printf(t->o, "<figoverlay number=\"%u\" even=\"%s\" odd=\"%s\" overlayData=\"",
			  attr & 0x0F, ( pp == 1 ) ? "false" : "true", ( pp == 2 ) ? "false" : "true");
		for ( i = 0; i < nwords(t); i++ ) bpk_buf_printf(t->o, "%s%u", i ? "," : "", W16(t, i));
		bpk_buf_puts(t->o, "\">\n");
		overlay_fig(t, t->d + 2, t->len - 2);
		bpk_buf_puts(t->o, "</figoverlay>\n");
		return;
	}
	case 4:
		if ( t->len < 4 ) break;
		bpk_buf_puts(t->o, "<figoverlay active=\"");
		overlay_list(t, W16(t, 1));
		bpk_buf_puts(t->o, "\" />\n");
		return;
	case 6:
		if ( t->len < 4 ) break;
		bpk_buf_printf(t->o, "<figpagenumber step=\"%d\" num=\"%u\" />\n", (INT)(B)attr, W16(t, 1));
		return;
	default:
		break;
	}
	skip(t, sub);
}

LOCAL void group( TX *t, UINT sub )
{
	if ( sub == 0 ) {
		if ( S16(t, 1) == 0 ) bpk_buf_puts(t->o, "<group>\n");
		else bpk_buf_printf(t->o, "<group id=\"%d\">\n", S16(t, 1));
		return;
	}
	if ( sub == 1 ) {
		bpk_buf_puts(t->o, "</group>\n");
		return;
	}
	skip(t, sub);
}

LOCAL void memo( TX *t, const char *name )
{
	bpk_buf_printf(t->o, "<%s text=\"", name);
	tron_attr(t, 1);
	bpk_buf_puts(t->o, "\" />\n");
}

/* ---------------------------------------------------------------- virtual objects */

LOCAL void vobj( TX *t )
{
	BPKCONV		*cv = t->cv;
	const char	*id = cv->hook->link_target(cv->hook->ctx, cv->linkno++);
	char		fr[8], ch[8], tb[8], bg[8], vid[40];
	LEVEL		*l = top(t);
	INT		lined, i;

	if ( t->len < 0x1E || id == NULL ) {
		skip(t, 0xFF);		/* a virtual object whose link record is missing */
		return;
	}
	colour(t, C32(t, 6), fr);
	colour(t, C32(t, 8), ch);
	colour(t, C32(t, 10), tb);
	colour(t, C32(t, 12), bg);
	cv->hook->new_vobjid(cv->hook->ctx, vid);
	lined = span_lined(t);
	for ( i = ( l != NULL ) ? l->nspan - 1 : -1; i >= lined; i-- ) bpk_buf_printf(t->o, "</%s>", l->span[i].name);
	bpk_buf_printf(t->o, "<link id=\"%s_0.xtad\" vobjid=\"%s\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\" vobjbottom=\"%d\""
		  " height=\"%d\" vobjheight=\"%d\"",
		  id, vid, S16(t, 0), S16(t, 1), S16(t, 2), S16(t, 3), S16(t, 3) - S16(t, 1), S16(t, 4));
	/*
	 * In a text a link takes a width and a height, in points as its
	 * letters are; the rectangle is kept beside them. The name's size
	 * is in points too, whatever the CHSIZE was written in.
	 */
	if ( in_text(t) ) {
		bpk_buf_printf(t->o, " width=\"%d\" heightpx=\"%d\"",
			  ( h_milli(t, S16(t, 2) - S16(t, 0)) + 500 ) / 1000,
			  ( v_milli(t, S16(t, 3) - S16(t, 1)) + 500 ) / 1000);
	}
	bpk_buf_printf(t->o, " chsz=\"%d\" frcol=\"%s\" chcol=\"%s\" tbcol=\"%s\" bgcol=\"%s\" dlen=\"%u\""
		  " pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\" zIndex=\"%d\"/>",
		  ( chsize_milli(t, W16(t, 5)) + 500 ) / 1000, fr, ch, tb, bg, W16(t, 14), ++t->z);
	if ( !in_text(t) ) bpk_buf_putc(t->o, '\n');
	for ( i = lined; l != NULL && i < l->nspan; i++ ) span_emit(t, &l->span[i]);
}

/* ---------------------------------------------------------------- pictures */

typedef struct {
	const UB	*s;		/* the segment from its id on */
	UINT		n;
	UINT		pixbits, rowbytes, planes, mask;
	UINT		base[8];
	UINT		cinfo[4];
	UINT		invert, palette, mode;
	UINT		*cmap;
	INT		ncmap;
	const UB	*pix;		/* the planes decoded, base[] into them; NULL: in the segment */
	UINT		npix;
} PIC;

/* A byte at an offset from the segment's start, 0 outside it */
LOCAL UINT pb( const PIC *p, UINT off )
{
	return ( off >= 4 && off < p->n ) ? p->s[off - 4] : 0;
}

/* A byte of the pixels: of the planes decoded, else of the segment */
LOCAL UINT pxb( const PIC *p, UINT off )
{
	if ( p->pix != NULL ) return ( off < p->npix ) ? p->pix[off] : 0;
	return pb(p, off);
}

LOCAL UINT plane_px( const PIC *p, UINT off, INT x, INT y )
{
	UINT	cnt = p->pixbits & 0xFF, wid = ( p->pixbits >> 8 ) & 0xFF;
	UINT	stride = ( wid > 0 ) ? wid : cnt, at;

	if ( off == 0 && p->pix == NULL ) return 0;
	if ( stride <= 8 && stride > 0 ) {
		UINT	bit = (UINT)x * stride;

		at = off + (UINT)y * p->rowbytes + bit / 8;
		return ( pxb(p, at) >> ( 8 - stride - bit % 8 ) ) & ( ( 1U << stride ) - 1 );
	}
	at = off + (UINT)y * p->rowbytes + (UINT)x * ( stride / 8 );
	switch ( stride ) {
	case 16: return pxb(p, at) | ( pxb(p, at + 1) << 8 );
	case 24: return pxb(p, at) | ( pxb(p, at + 1) << 8 ) | ( pxb(p, at + 2) << 16 );
	case 32: return pxb(p, at) | ( pxb(p, at + 1) << 8 ) | ( pxb(p, at + 2) << 16 ) | ( pxb(p, at + 3) << 24 );
	default: return 0;
	}
}

/* A channel of bits (its place in the high byte, its width in the low) to 8 bits */
LOCAL UINT channel( UINT v, UINT ci )
{
	UINT	pos = ( ci >> 8 ) & 0xFF, w = ci & 0xFF, c, r = 0;
	INT	rem = 8;

	if ( w == 0 ) return 0;
	c = ( pos < 32 ) ? ( v >> pos ) & ( ( w >= 32 ) ? 0xFFFFFFFFU : ( 1U << w ) - 1 ) : 0;
	if ( w == 8 ) return c & 0xFF;
	if ( w > 8 ) return ( c >> ( w - 8 ) ) & 0xFF;
	while ( rem >= (INT)w ) {
		rem -= (INT)w;
		r |= c << rem;
	}
	if ( rem > 0 ) r |= c >> ( w - (UINT)rem );
	return r & 0xFF;
}

LOCAL UINT pixel( const PIC *p, INT x, INT y )
{
	UINT	v = 0, k, r, g, b;

	if ( p->planes <= 1 ) {
		v = plane_px(p, p->base[0], x, y);
	} else {
		for ( k = 0; k < p->planes && k < 8; k++ ) v |= plane_px(p, p->base[k], x, y) << ( k * ( p->pixbits & 0xFF ) );
	}
	if ( p->palette ) {
		if ( p->cmap != NULL && v < (UINT)p->ncmap ) return p->cmap[v] & 0xFFFFFF;
		v &= 0xFF;
		return ( v << 16 ) | ( v << 8 ) | v;
	}
	switch ( p->mode ) {
	case 0: {
		/* the grey's bits: cinfo[0], else the whole pixel; 0 is white unless inverted */
		UINT	ci = p->cinfo[0] ? p->cinfo[0] : ( p->pixbits & 0xFF ), bw = ci & 0xFF, l;

		if ( bw == 0 ) bw = ( p->pixbits & 0xFF ) ? ( p->pixbits & 0xFF ) : 8;
		l = channel(v, ( ci & 0xFF00 ) | bw);
		if ( p->invert == 0 ) l = 255 - l;
		return ( l << 16 ) | ( l << 8 ) | l;
	}
	case 1:
		r = channel(v, p->cinfo[0]);
		g = channel(v, p->cinfo[1]);
		b = channel(v, p->cinfo[2]);
		return ( r << 16 ) | ( g << 8 ) | b;
	case 2: {
		UINT	c = channel(v, p->cinfo[0]), m = channel(v, p->cinfo[1]), yy = channel(v, p->cinfo[2]);
		UINT	kk = channel(v, p->cinfo[3]);

		if ( p->invert ) {
			c = 255 - c; m = 255 - m; yy = 255 - yy; kk = 255 - kk;
		}
		r = ( ( 255 - c ) * ( 255 - kk ) + 127 ) / 255;
		g = ( ( 255 - m ) * ( 255 - kk ) + 127 ) / 255;
		b = ( ( 255 - yy ) * ( 255 - kk ) + 127 ) / 255;
		return ( r << 16 ) | ( g << 8 ) | b;
	}
	default:
		return 0;
	}
}

/* The bytes of a row of the mask: two for up to 16 pixels, four to 32, six to 48 */
LOCAL UINT mask_rowbytes( INT w )
{
	UINT	rb;

	if ( w <= 16 ) return 2;
	if ( w <= 32 ) return 4;
	if ( w <= 48 ) return 6;
	rb = (UINT)( w + 7 ) / 8;
	return ( rb & 1 ) ? rb + 1 : rb;
}

/*
 * 画像セグメント: a pixel map made a PNG kept beside the record, as the
 * resource "_<record>_<n>.png", and an <image> that shows it.
 */
LOCAL void image( TX *t )
{
	BPKCONV	*cv = t->cv;
	PIC	p;
	INT	w, h, x, y, len = 0;
	UINT	cbyte, *px, k, tend, *cmap = NULL, compac;
	UB	*png, *dec = NULL;
	ER	er;

	if ( t->len < 0x3C ) {
		skip(t, 0);
		return;
	}
	memset(&p, 0, sizeof(p));
	p.s = t->d;
	p.n = t->len + 4;
	cbyte = W16(t, 11) & 0xFF;
	p.invert = ( cbyte >> 7 ) & 1;
	p.palette = ( cbyte >> 3 ) & 1;
	p.mode = cbyte & 7;
	for ( k = 0; k < 4; k++ ) p.cinfo[k] = W16(t, 12 + (INT)k);
	p.mask = rd32(t->d + 40);
	p.planes = W16(t, 23);
	p.pixbits = W16(t, 24);
	p.rowbytes = W16(t, 25);
	w = S16(t, 28) - S16(t, 26);
	h = S16(t, 29) - S16(t, 27);
	compac = W16(t, 22);
	if ( compac > BPK_MR4COMPAC || w <= 0 || h <= 0 || w > 8192 || h > 8192 || p.planes == 0 || p.planes > 8
	  || ( compac != BPK_NOCOMPAC && ( ( p.pixbits & 0xFF ) != 1 || ( ( p.pixbits >> 8 ) & 0xFF ) > 1 ) ) ) {
		skip(t, 1);		/* not a picture this knows */
		return;
	}
	/* where each plane is; an offset into the table itself means just after it */
	tend = 0x40 + p.planes * 4;
	for ( k = 0; k < p.planes; k++ ) {
		UINT	off = pb(&p, 0x40 + 4 * k) | ( pb(&p, 0x41 + 4 * k) << 8 ) | ( pb(&p, 0x42 + 4 * k) << 16 ) | ( pb(&p, 0x43 + 4 * k) << 24 );

		if ( p.planes == 1 ? off < tend : off < tend - 4 ) off = tend;
		p.base[k] = off;
	}
	if ( compac != BPK_NOCOMPAC ) {
		/* each one-bit plane a facsimile: decoded, a plane after another */
		UINT	rb = (UINT)( ( w + 15 ) / 16 ) * 2, sz = rb * (UINT)h;

		dec = malloc((size_t)sz * p.planes);
		if ( dec == NULL ) {
			if ( cv->er >= E_OK ) cv->er = E_NOMEM;
			return;
		}
		for ( k = 0; k < p.planes; k++ ) {
			if ( p.base[k] < 4 || p.base[k] >= p.n
			  || bpk_fax_decode(t->d + p.base[k] - 4, p.n - p.base[k], (BOOL)( compac != BPK_MHCOMPAC ),
					    w, h, dec + sz * k, rb) < 0 ) {
				free(dec);
				skip(t, 2);	/* its codes go wrong */
				return;
			}
			p.base[k] = sz * k;
		}
		p.pix = dec;
		p.npix = sz * p.planes;
		p.rowbytes = rb;
	}
	/* the colour map of a picture that has one: at its end when it says nearly so */
	if ( p.palette ) {
		UINT	nb = p.cinfo[0], off = ( p.cinfo[2] << 16 ) | p.cinfo[3], endoff = ( nb < p.n ) ? p.n - nb : 0;

		if ( off > 0 && endoff > off && endoff - off <= 8 ) off = endoff;
		if ( nb >= 4 && off > 0 && off + nb <= p.n ) {
			cmap = malloc(sizeof(UINT) * ( nb / 4 ));
			if ( cmap != NULL ) {
				for ( k = 0; k < nb / 4; k++ ) {
					UINT	raw = pb(&p, off + 4 * k) | ( pb(&p, off + 4 * k + 1) << 8 )
						    | ( pb(&p, off + 4 * k + 2) << 16 ) | ( pb(&p, off + 4 * k + 3) << 24 );
					char	c[8];

					bpk_colour(raw, cv->cmap, cv->ncmap, c);
					cmap[k] = (UINT)strtoul(c + 1, NULL, 16);
				}
				p.cmap = cmap;
				p.ncmap = (INT)( nb / 4 );
			}
		}
	}
	px = malloc(sizeof(UINT) * (size_t)w * (size_t)h);
	if ( px == NULL ) {
		free(dec);
		free(cmap);
		if ( cv->er >= E_OK ) cv->er = E_NOMEM;
		return;
	}
	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < w; x++ ) {
			UINT	a = 0xFF;

			if ( p.mask != 0 ) {
				UINT	at = p.mask + 4 + (UINT)y * mask_rowbytes(w) + (UINT)x / 8;

				if ( at < p.n && ( ( pb(&p, at) >> ( 7 - x % 8 ) ) & 1 ) == 0 ) a = 0;
			}
			px[y * w + x] = ( a << 24 ) | pixel(&p, x, y);
		}
	}
	free(cmap);
	free(dec);
	png = bpk_png_encode(px, w, h, (BOOL)( p.mask != 0 ), &len);
	free(px);
	if ( png == NULL ) {
		if ( cv->er >= E_OK ) cv->er = E_NOMEM;
		return;
	}
	er = cv->hook->picture(cv->hook->ctx, cv->recno, cv->nimage, png, len);
	free(png);
	if ( er < E_OK ) {
		if ( cv->er >= E_OK ) cv->er = er;
		return;
	}
	bpk_buf_printf(t->o, "<image lineType=\"0\" lineWidth=\"1\" l_pat=\"0\" f_pat=\"0\" angle=\"0\" rotation=\"0\""
		  " flipH=\"false\" flipV=\"false\" left=\"%d\" top=\"%d\" right=\"%d\" bottom=\"%d\""
		  " href=\"%s_%d_%d.png\" zIndex=\"%d\"/>\n",
		  S16(t, 0), S16(t, 1), S16(t, 2), S16(t, 3), t->self, cv->recno, cv->nimage, ++t->z);
	cv->nimage++;
}

/* ---------------------------------------------------------------- the record */

LOCAL void segment( TX *t )
{
	UINT	sub = ( W16(t, 0) >> 8 ) & 0xFF;

	tchar_flush(t);
	switch ( t->id ) {
	case TS_INFO:					break;
	case TS_TEXT:	text_start(t);			break;
	case TS_TEXTEND: text_end(t);			break;
	case TS_FIG:	fig_start(t);			break;
	case TS_FIGEND:	fig_end(t);			break;
	case TS_IMAGE:	image(t);			break;
	case TS_VOBJ:	vobj(t);			break;
	case TS_TPAGE:	text_page(t, sub);		break;
	case TS_TRULER:	text_ruler(t, sub);		break;
	case TS_TFONT:	text_font(t, sub);		break;
	case TS_TCHAR:	text_char_seg(t, sub);		break;
	case TS_TATTR:	text_attr(t, sub);		break;
	case TS_TSTYLE:	text_style(t, sub);		break;
	case TS_TMEMO:	memo(t, "docmemo");		break;
	case TS_TVAR:	text_var(t, sub);		break;
	case TS_TAPPL:	appl_fusen(t, "docappl");	break;
	case TS_FAPPL:	appl_fusen(t, "figappl");	break;
	case TS_DFUSEN:	dfusen(t);			break;
	case TS_FPRIM:	fig_prim(t, sub);		break;
	case TS_FDEF:	fig_def(t, sub);		break;
	case TS_FGRP:	group(t, sub);			break;
	case TS_FATTR:	fig_attr(t, sub);		break;
	case TS_FPAGE:	fig_page(t, sub);		break;
	case TS_FMEMO:	memo(t, "figmemo");		break;
	default:
		/* 機能付箋, 設定付箋, macros, and ids unknown */
		skip(t, sub);
		break;
	}
	/* arrows and moves are for the one element after them */
	if ( t->id != TS_FATTR ) t->arrow_s = t->arrow_e = FALSE;
}

/*
 * The segments of a record in turn, each given to fn with the segment's
 * id, data and length in t; the characters between them to text_char
 * when chars is set.
 */
LOCAL void walk( TX *t, const UB *rec, UINT n, BOOL chars, void (*fn)( TX * ) )
{
	UINT	pos = 0;

	while ( pos + 2 <= n ) {
		UINT	w = rd16(rec + pos), id, len;

		if ( w == 0 ) break;
		if ( w <= TC_SPEC ) {
			if ( chars ) text_char(t, w);
			pos += 2;
			continue;
		}
		pos += 2;
		id = w;
		if ( w == TS_EXT ) {
			/* an id past FFFF: whatever it is, it is not one known here */
			UINT	w2 = ( pos + 2 <= n ) ? rd16(rec + pos) : 0;

			pos += 2;
			if ( w2 >= 0xFF00 ) {
				UINT	w3 = ( pos + 2 <= n ) ? rd16(rec + pos) : 0;

				pos += ( w3 == 0xFEFE ) ? 4 : 2;
			}
			id = 0x10000;
		}
		if ( pos + 2 > n ) break;
		len = rd16(rec + pos);
		pos += 2;
		if ( len == TS_LONG ) {
			if ( pos + 4 > n ) break;
			len = rd32(rec + pos);
			pos += 4;
		}
		if ( len > n - pos ) break;		/* cut short: the rest is lost */
		t->id = id;
		t->d = rec + pos;
		t->len = len;
		fn(t);
		pos += len;
	}
}

EXPORT void bpk_conv_init( BPKCONV *cv, const BPKHOOK *hook )
{
	memset(cv, 0, sizeof(*cv));
	cv->hook = hook;
	cv->er = E_OK;
}

EXPORT void bpk_conv_object( BPKCONV *cv )
{
	cv->linkno = 0;
}

EXPORT ER bpk_tad_to_xml( BPKCONV *cv, const char *name, const char *self, INT recno,
			  const UB *rec, UINT len, BPKBUF *out )
{
	TX	*t = calloc(1, sizeof(TX));

	if ( t == NULL ) return E_NOMEM;
	cv->recno = recno;
	cv->nimage = 0;
	cv->plane = 1;
	cv->ncmap = 0;
	t->cv = cv;
	t->o = out;
	t->self = ( self != NULL ) ? self : "";
	t->fontsize = 14000;
	t->pg_num = 1;
	t->pg_step = 1;
	bpk_buf_puts(out, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	bpk_buf_xml(out, ( name != NULL ) ? name : "");
	bpk_buf_puts(out, "\">\n");
	walk(t, rec, len, TRUE, segment);
	tchar_flush(t);
	while ( t->sp > 0 ) {
		if ( top(t)->kind == 'T' ) text_end(t);
		else fig_end(t);
	}
	bpk_buf_puts(out, "</tad>\n");
	free(t);
	if ( out->fail ) return E_NOMEM;
	return cv->er;
}

/* ---------------------------------------------------------------- looks before the work */

typedef struct {
	UINT	*cmap;
	INT	max, n;
	BOOL	first, fig, doc;
} SCAN;

LOCAL SCAN	*scan_now;

LOCAL void scan_seg( TX *t )
{
	SCAN	*s = scan_now;
	INT	i, n;

	if ( s->first ) {
		s->first = FALSE;
		s->fig = (BOOL)( t->id == TS_FIG );
		s->doc = (BOOL)( t->id == TS_FIG || t->id == TS_TEXT );
		if ( t->id == TS_INFO ) s->first = TRUE;
	}
	if ( t->id == TS_FDEF && ( ( W16(t, 0) >> 8 ) & 0xFF ) == 0 && t->len >= 8 ) {
		n = S16(t, 1);
		for ( i = 0; i < n && i < s->max; i++ ) s->cmap[i] = C32(t, 2 + 2 * i);
		if ( n > s->n ) s->n = ( n < s->max ) ? n : s->max;
	}
}

LOCAL void scan( const UB *rec, UINT len, SCAN *s )
{
	TX	*t = calloc(1, sizeof(TX));

	if ( t == NULL ) return;
	s->first = TRUE;
	scan_now = s;
	walk(t, rec, len, FALSE, scan_seg);
	free(t);
}

EXPORT INT bpk_tad_cmap( const UB *rec, UINT len, UINT *cmap, INT max )
{
	SCAN	s;

	memset(&s, 0, sizeof(s));
	s.cmap = cmap;
	s.max = max;
	scan(rec, len, &s);
	return s.n;
}

EXPORT BOOL bpk_tad_is_fig( const UB *rec, UINT len )
{
	SCAN	s;
	UINT	none[1];

	memset(&s, 0, sizeof(s));
	s.cmap = none;
	s.max = 0;
	scan(rec, len, &s);
	return s.fig;
}

EXPORT BOOL bpk_tad_is_doc( const UB *rec, UINT len )
{
	SCAN	s;
	UINT	none[1];

	memset(&s, 0, sizeof(s));
	s.cmap = none;
	s.max = 0;
	scan(rec, len, &s);
	return s.doc;
}
