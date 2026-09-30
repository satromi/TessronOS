/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad_view.c
 *	What a parsed record means: paragraphs and shapes (design 16.3, 17.5)
 *
 *	The parser gives a tree of elements with their attributes; this
 *	walks that tree and builds the thing it means. Nothing here copies
 *	text: a run points into the bytes the parser holds, so a document
 *	of any length costs a few words per run.
 *
 *	The vocabulary is what the records actually contain. A <document>
 *	is paragraphs of runs; the attributes that decide how a run looks
 *	come from the <font> elements it sits inside, which nest, so the
 *	walk carries a stack of them. A <figure> is shapes in the order
 *	their zIndex says, and a shape may be a piece of text, which is a
 *	document of its own.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/tad.h>
#include <ts/tadview.h>

#define DOC_MAX_FIG	16		/* figures standing in one text */
#define DOC_MAX_PARA	512
#define DOC_MAX_RUN	2048
#define FONT_DEPTH	32

/* ---------------------------------------------------------------- what a link points at */

EXPORT CONST T_TVSRC	*knl_tv_src = NULL;
EXPORT void		*knl_tv_src_arg = NULL;

EXPORT void tv_source( CONST T_TVSRC *src, void *arg )
{
	knl_tv_src = src;
	knl_tv_src_arg = arg;
}

/* ---------------------------------------------------------------- numbers */

LOCAL INT to_int( CONST UB *s, INT dflt )
{
	INT	v = 0, sign = 1;

	if ( s == NULL || *s == 0 ) {
		return dflt;
	}
	if ( *s == '-' ) {
		sign = -1;
		s++;
	} else if ( *s == '+' ) {
		s++;
	}
	if ( *s < '0' || *s > '9' ) {
		return dflt;
	}
	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + ( *s - '0' );
		s++;
	}

	return v * sign;
}

LOCAL INT hex_digit( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;

	return -1;
}

EXPORT UW tv_colour( CONST UB *s, UW dflt )
{
	UW	v = 0;
	INT	i, d;

	if ( s == NULL || s[0] != '#' ) {
		return dflt;
	}
	for ( i = 1; i <= 6; i++ ) {
		d = hex_digit(s[i]);
		if ( d < 0 ) {
			return dflt;
		}
		v = (v << 4) | (UW)d;
	}

	return v & 0x00FFFFFFU;
}

LOCAL BOOL name_is( CONST T_TADNODE *nd, CONST char *name )
{
	INT	i;

	if ( nd == NULL || nd->kind != TAD_ND_ELEM || nd->name == NULL ) {
		return FALSE;
	}
	for ( i = 0; name[i] != 0; i++ ) {
		if ( nd->name[i] != (UB)name[i] ) {
			return FALSE;
		}
	}

	return ( nd->name[i] == 0 );
}

/* The rectangle an element carries, under whichever names it uses */
LOCAL void rect_attr( CONST T_TADNODE *nd, CONST char *l, CONST char *t,
		      CONST char *r, CONST char *b, T_DPRECT *out )
{
	out->left   = to_int(tad_attr(nd, l), out->left);
	out->top    = to_int(tad_attr(nd, t), out->top);
	out->right  = to_int(tad_attr(nd, r), out->right);
	out->bottom = to_int(tad_attr(nd, b), out->bottom);
}

/* ---------------------------------------------------------------- documents */

EXPORT UINT tv_kind( CONST T_TAD *doc )
{
	T_TADNODE	*body = tad_body(doc);

	if ( name_is(body, "document") ) {
		return TV_KIND_DOC;
	}
	if ( name_is(body, "figure") ) {
		return TV_KIND_FIG;
	}

	return TV_KIND_NONE;
}

/*
 * The look of letters at a point of the document: what the elements it
 * sits inside have made it. <font> with nothing inside changes it from
 * there on; everything else -- <font> round letters, <bold>, <ruby> and
 * the rest -- changes it for what it holds.
 */
typedef struct {
	INT		size;
	UW		colour;
	UINT		style;
	CONST UB	*face;
	INT		space;
	INT		wr_n, wr_d, hr_n, hr_d;
	UW		back;
	UINT		bouten;
	CONST UB	*ruby;
	INT		ruby_len;
	BOOL		ruby_below;
	INT		ruby_group;
	INT		turn;		/* as T_TVRUN keeps them */
	INT		lift;
	BOOL		lift_abs;
} FONTST;

/*
 * What holds from a tab format, or a <text> setting, until the next one
 * changes it: every paragraph after takes it on.
 */
typedef struct {
	INT	gap, pargap;
	BOOL	gap_abs, pargap_abs;
	BOOL	gap_set, pargap_set;
	INT	left, right, indent;
	INT	ntabs;
	INT	tabs[TV_MAX_TABS];
	UINT	align;
	CONST UB *khead, *ktail;	/* 禁則, as T_TVPARA keeps it */
	INT	khead_len, ktail_len;
	INT	khead_kind, ktail_kind;
} PARAST;

typedef struct {
	T_TVDOC	*d;
	INT	maxpara, maxrun;
	FONTST	st[FONT_DEPTH];
	INT	depth;
	INT	link_no;		/* links seen so far, to number them */
	PARAST	ps;
	INT	ruby_next;		/* the next ruby group's number */
	BOOL	page_next;		/* a page break waits for the next paragraph */
	CONST T_TADNODE	*fmt_next;	/* a tab format waits for the next paragraph */
} DOCBUILD;

LOCAL void run_add( DOCBUILD *b, UINT kind, CONST UB *text, INT len,
		    INT link, CONST UB *href, CONST T_TADNODE *nd, INT noff )
{
	T_TVRUN		*r;
	CONST FONTST	*f = &b->st[b->depth];

	if ( b->d->nrun >= b->maxrun ) {
		return;
	}
	r = &b->d->run[b->d->nrun];
	r->kind   = kind;
	r->text   = text;
	r->len    = len;
	r->size   = f->size;
	r->colour = f->colour;
	r->style  = f->style;
	r->link   = link;
	r->href   = href;
	r->face   = f->face;
	r->space  = f->space;
	r->wr_n   = f->wr_n;
	r->wr_d   = f->wr_d;
	r->hr_n   = f->hr_n;
	r->hr_d   = f->hr_d;
	r->back   = f->back;
	r->bouten = f->bouten;
	r->ruby   = f->ruby;
	r->ruby_len = f->ruby_len;
	r->ruby_below = f->ruby_below;
	r->ruby_group = f->ruby_group;
	r->width  = 0;
	r->turn   = f->turn;
	r->lift   = f->lift;
	r->lift_abs = f->lift_abs;
	r->node   = (T_TADNODE *)nd;
	r->noff   = noff;
	b->d->nrun++;
	if ( b->d->npara > 0 ) {
		b->d->para[b->d->npara - 1].n++;
	}
}

/* The paragraph now being built takes on what holds */
LOCAL void para_take( DOCBUILD *b, T_TVPARA *p )
{
	INT	i;

	p->align      = b->ps.align;
	p->gap        = b->ps.gap_set ? b->ps.gap : 500;
	p->gap_abs    = b->ps.gap_abs;
	p->pargap     = b->ps.pargap_set ? b->ps.pargap : 1000;
	p->pargap_abs = b->ps.pargap_abs;
	p->left       = b->ps.left;
	p->right      = b->ps.right;
	p->indent     = b->ps.indent;
	p->ntabs      = b->ps.ntabs;
	for ( i = 0; i < b->ps.ntabs && i < TV_MAX_TABS; i++ ) {
		p->tabs[i] = b->ps.tabs[i];
	}
	p->khead      = b->ps.khead;
	p->khead_len  = b->ps.khead_len;
	p->khead_kind = b->ps.khead_kind;
	p->ktail      = b->ps.ktail;
	p->ktail_len  = b->ps.ktail_len;
	p->ktail_kind = b->ps.ktail_kind;
}

LOCAL void para_add( DOCBUILD *b, CONST T_TADNODE *nd )
{
	T_TVPARA	*p;

	if ( b->d->npara >= b->maxpara ) {
		return;
	}
	p = &b->d->para[b->d->npara];
	p->first  = b->d->nrun;
	p->n      = 0;
	p->node   = (T_TADNODE *)nd;
	para_take(b, p);
	p->page_before = b->page_next;
	b->page_next = FALSE;
	p->fmt = (T_TADNODE *)b->fmt_next;
	b->fmt_next = NULL;
	p->align_at = NULL;
	if ( nd != NULL ) {
		CONST UB	*a = tad_attr(nd, "align");

		if ( a != NULL ) {
			p->align = ( a[0] == 'c' ) ? TV_ALIGN_CENTRE
				 : ( a[0] == 'r' ) ? TV_ALIGN_RIGHT
				 : ( a[0] == 'j' ) ? TV_ALIGN_JUSTIFY : TV_ALIGN_LEFT;
		}
		if ( tad_attr(nd, "indent") != NULL ) {
			p->indent = to_int(tad_attr(nd, "indent"), 0);
		}
	}
	b->d->npara++;
}

/* The paragraph being built, when a setting inside it changes it */
LOCAL T_TVPARA *para_now( DOCBUILD *b )
{
	return ( b->d->npara > 0 ) ? &b->d->para[b->d->npara - 1] : NULL;
}

/* How many bytes of text a node holds, not counting what only spaces */
LOCAL INT text_len( CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}

	return n;
}

/* The line breaks a record is written with, which are not text */
LOCAL BOOL is_break( UB c )
{
	return ( c == 0x0A || c == 0x0D || c == 0x09 );
}

LOCAL void doc_walk( DOCBUILD *b, CONST T_TADNODE *nd );

/* Whether text holds anything but spaces and line breaks */
LOCAL BOOL has_ink( CONST UB *t )
{
	for ( ; *t != 0; t++ ) {
		if ( *t != ' ' && *t != 0x0A && *t != 0x0D && *t != 0x09 ) {
			return TRUE;
		}
	}

	return FALSE;
}

LOCAL void doc_children( DOCBUILD *b, CONST T_TADNODE *nd )
{
	CONST T_TADNODE	*c;

	for ( c = nd->first; c != NULL; c = c->next ) {
		doc_walk(b, c);
	}
}

/*
 * A SCALE: "0.75" is a fraction of the letters' size, kept here in
 * thousandths; "abs:150" is so many units.
 */
LOCAL INT scale_of( CONST UB *v, BOOL *p_abs )
{
	INT	whole = 0, frac = 0, digits = 0;

	*p_abs = FALSE;
	if ( v == NULL ) {
		return 0;
	}
	if ( v[0] == 'a' && v[1] == 'b' && v[2] == 's' && v[3] == ':' ) {
		*p_abs = TRUE;
		return to_int(v + 4, 0);
	}
	while ( *v >= '0' && *v <= '9' ) {
		whole = whole * 10 + ( *v++ - '0' );
	}
	if ( *v == '.' ) {
		v++;
		while ( *v >= '0' && *v <= '9' && digits < 3 ) {
			frac = frac * 10 + ( *v++ - '0' );
			digits++;
		}
		while ( digits < 3 ) {
			frac *= 10;
			digits++;
		}
	}

	return whole * 1000 + frac;
}

/* "A/B" as two numbers; B of nought, or anything else, is one to one */
LOCAL void ratio_of( CONST UB *v, INT *p_n, INT *p_d )
{
	INT	n = 0, d = 0;

	*p_n = 1;
	*p_d = 1;
	if ( v == NULL ) {
		return;
	}
	while ( *v >= '0' && *v <= '9' ) n = n * 10 + ( *v++ - '0' );
	if ( *v++ != '/' ) {
		return;
	}
	while ( *v >= '0' && *v <= '9' ) d = d * 10 + ( *v++ - '0' );
	if ( n > 0 && d > 0 ) {
		*p_n = n;
		*p_d = d;
	}
}

LOCAL INT vpx( CONST DOCBUILD *b, INT v );

/* The settings of a <font>, into the look they change */
LOCAL void font_attrs( CONST DOCBUILD *b, FONTST *f, CONST T_TADNODE *nd )
{
	CONST UB	*v;
	BOOL		abs;

	/*
	 * 文字回転: degrees, clockwise. rotabs says whether they count from
	 * the paper or from the line, which on a line written across are
	 * the same.
	 */
	if ( ( v = tad_attr(nd, "rotation") ) != NULL ) {
		f->turn = ( to_int(v, 0) % 360 + 360 ) % 360;
	}
	/*
	 * 文字基準位置移動: a SCALE of the letters' size, or so many units;
	 * baseattr 0 moves the letters towards the line before (up), 1
	 * towards the line after (down).
	 */
	if ( ( v = tad_attr(nd, "baseshift") ) != NULL ) {
		INT	s = scale_of(v, &abs);

		f->lift_abs = abs;
		f->lift = abs ? vpx(b, s) : s;
		if ( ( to_int(tad_attr(nd, "baseattr"), 0) & 1 ) != 0 ) {
			f->lift = -f->lift;
		}
	}

	if ( ( v = tad_attr(nd, "size") ) != NULL ) {
		f->size = to_int(v, f->size);
	}
	if ( ( v = tad_attr(nd, "color") ) != NULL ) {
		f->colour = tv_colour(v, f->colour);
	}
	if ( ( v = tad_attr(nd, "face") ) != NULL ) {
		f->face = ( v[0] != 0 ) ? v : NULL;
	}
	if ( ( v = tad_attr(nd, "space") ) != NULL ) {
		f->space = scale_of(v, &abs);
	}
	if ( ( v = tad_attr(nd, "wRatio") ) != NULL
	  || ( v = tad_attr(nd, "wratio") ) != NULL ) {
		ratio_of(v, &f->wr_n, &f->wr_d);
	}
	if ( ( v = tad_attr(nd, "hRatio") ) != NULL
	  || ( v = tad_attr(nd, "hratio") ) != NULL ) {
		ratio_of(v, &f->hr_n, &f->hr_d);
	}
	/* weight: 5 and heavier is bold; 700 in the other way of saying it */
	if ( ( v = tad_attr(nd, "weight") ) != NULL ) {
		INT	w = to_int(v, 0);

		if ( w >= 5 && w <= 7 ) f->style |= TV_ST_BOLD;
		else if ( w >= 600 ) f->style |= TV_ST_BOLD;
		else f->style &= ~TV_ST_BOLD;
	}
	if ( ( v = tad_attr(nd, "style") ) != NULL ) {
		INT	st = to_int(v, 0);

		if ( ( st >= 1 && st <= 7 ) || v[0] == 'i' || v[0] == 'o' ) {
			f->style |= TV_ST_ITALIC;
		} else {
			f->style &= ~TV_ST_ITALIC;
		}
	}
}

LOCAL INT hpx( CONST DOCBUILD *b, INT v );

/* The paragraph settings of a <tab-format>, from here on */
LOCAL void tab_format( DOCBUILD *b, CONST T_TADNODE *nd )
{
	PARAST		*ps = &b->ps;
	CONST UB	*v;
	T_TVPARA	*p;

	if ( ( v = tad_attr(nd, "height") ) != NULL ) {
		ps->gap = scale_of(v, &ps->gap_abs);
		ps->gap_set = (BOOL)( ps->gap > 0 || ps->gap_abs );
	}
	if ( ( v = tad_attr(nd, "pargap") ) != NULL ) {
		ps->pargap = scale_of(v, &ps->pargap_abs);
		ps->pargap_set = TRUE;
	}
	if ( tad_attr(nd, "left") != NULL ) {
		ps->left = hpx(b, to_int(tad_attr(nd, "left"), 0));
	}
	if ( tad_attr(nd, "right") != NULL ) {
		ps->right = hpx(b, to_int(tad_attr(nd, "right"), 0));
	}
	if ( tad_attr(nd, "indent") != NULL ) {
		ps->indent = hpx(b, to_int(tad_attr(nd, "indent"), 0));
	}
	if ( to_int(tad_attr(nd, "ntabs"), 0) >= 0
	  && ( v = tad_attr(nd, "tabs") ) != NULL ) {
		ps->ntabs = 0;
		while ( *v != 0 && ps->ntabs < TV_MAX_TABS ) {
			INT	x = 0, sign = 1;

			while ( *v == ',' || *v == ' ' ) v++;
			if ( *v == 0 ) break;
			if ( *v == '-' ) { sign = -1; v++; }
			while ( *v >= '0' && *v <= '9' ) x = x * 10 + ( *v++ - '0' );
			/* a stop given below nought lines up decimal points
			   at the place its size gives: a stop there */
			(void)sign;
			ps->tabs[ps->ntabs++] = hpx(b, x);
			while ( *v != 0 && *v != ',' ) v++;
		}
	} else if ( to_int(tad_attr(nd, "ntabs"), 0) == 0 ) {
		ps->ntabs = 0;
	}
	p = para_now(b);
	if ( p != NULL && p->n == 0 ) {
		para_take(b, p);		/* at the head of a paragraph: it too */
		p->fmt = (T_TADNODE *)nd;
	} else {
		b->fmt_next = nd;
	}
}

/* Lengths across in the record's units, as pixels */
LOCAL INT hpx( CONST DOCBUILD *b, INT v )
{
	INT	dpi = ( b->d->hunit < 0 ) ? -b->d->hunit : 72;

	return ( dpi == 72 ) ? v : v * 72 / dpi;
}

/* Lengths down, the same */
LOCAL INT vpx( CONST DOCBUILD *b, INT v )
{
	INT	dpi = ( b->d->vunit < 0 ) ? -b->d->vunit : 72;

	return ( dpi == 72 ) ? v : v * 72 / dpi;
}

/* A new look for what an element holds; the old one back after */
LOCAL BOOL push( DOCBUILD *b )
{
	if ( b->depth + 1 >= FONT_DEPTH ) {
		return FALSE;
	}
	b->depth++;
	b->st[b->depth] = b->st[b->depth - 1];

	return TRUE;
}

LOCAL void pop( DOCBUILD *b, BOOL pushed )
{
	if ( pushed && b->depth > 0 ) {
		b->depth--;
	}
}

/* The overlays an 'active' list puts on, "0, 3": a bit for each number */
LOCAL UINT over_list( CONST UB *v )
{
	UINT	on = 0;

	while ( *v != 0 ) {
		INT	n = 0;
		BOOL	any = FALSE;

		while ( *v == ',' || *v == ' ' ) v++;
		while ( *v >= '0' && *v <= '9' ) {
			n = n * 10 + ( *v++ - '0' );
			any = TRUE;
		}
		if ( any && n < TV_MAX_OVER ) {
			on |= 1U << n;
		}
		while ( *v != 0 && *v != ',' ) v++;
	}

	return on;
}

/* The first paragraph, for what stands before any paragraph at all */
LOCAL void para_need( DOCBUILD *b )
{
	if ( b->d->npara == 0 ) {
		para_add(b, NULL);
	}
}

/* The decorations each element stands for */
typedef struct {
	CONST char	*name;
	UINT		style;
} DECO;

LOCAL CONST DECO deco_tab[] = {
	{ "bold", TV_ST_BOLD },		{ "strong", TV_ST_BOLD },
	{ "italic", TV_ST_ITALIC },	{ "i", TV_ST_ITALIC },
	{ "underline", TV_ST_UNDER },	{ "overline", TV_ST_OVER },
	{ "strikethrough", TV_ST_STRIKE }, { "strike", TV_ST_STRIKE },
	{ "box", TV_ST_BOX },		{ "invert", TV_ST_INVERT },
	{ "mesh", TV_ST_MESH },		{ "bagchar", TV_ST_BAG },
	{ "shadow", TV_ST_SHADOW },	{ "noprint", TV_ST_NOPRINT },
	{ "combchar", TV_ST_COMB },
};

LOCAL void doc_walk( DOCBUILD *b, CONST T_TADNODE *nd )
{
	UINT	i;

	if ( nd->kind == TAD_ND_TEXT ) {
		if ( nd->text != NULL && b->d->npara == 0 && has_ink(nd->text) ) {
			/*
			 * Words before any paragraph or break -- a piece of
			 * text in a figure is written that way -- are the
			 * first paragraph, not nothing.
			 */
			para_add(b, NULL);
		}
		if ( nd->text != NULL && b->d->npara > 0 ) {
			CONST UB	*t = nd->text;
			INT		n = text_len(t);

			/*
			 * The breaks a record is written with are not text.
			 * A paragraph put on its own line has a newline after
			 * the tag that opens it and one before the tag that
			 * closes it, and neither is anything the writer typed;
			 * what is between two tags on the same line is.
			 */
			while ( n > 0 && is_break(*t) ) {
				t++;
				n--;
			}
			while ( n > 0 && is_break(t[n - 1]) ) {
				n--;
			}
			if ( n > 0 ) {
				run_add(b, TV_RUN_TEXT, t, n, -1, NULL, nd,
					(INT)( t - nd->text ));
			}
		}
		return;
	}
	if ( nd->kind != TAD_ND_ELEM ) {
		return;
	}
	if ( name_is(nd, "p") ) {
		/* what a <font/> changes goes on into the next paragraph */
		para_add(b, nd);
		doc_children(b, nd);
		return;
	}
	if ( name_is(nd, "br") ) {
		para_need(b);
		run_add(b, TV_RUN_BREAK, NULL, 0, -1, NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "tab") ) {
		para_need(b);
		run_add(b, TV_RUN_TAB, NULL, 0, -1, NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "indent") ) {
		para_need(b);
		run_add(b, TV_RUN_INDENT, NULL, 0, -1, NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "pagebreak") ) {
		/* a new page: before the next paragraph, or here in this one */
		if ( b->d->npara > 0 && para_now(b)->n > 0 ) {
			run_add(b, TV_RUN_PAGE, NULL, 0, -1, NULL, nd, 0);
		} else {
			b->page_next = TRUE;
		}
		return;
	}
	if ( name_is(nd, "fixed-space") || name_is(nd, "fixedSpace")
	  || name_is(nd, "widthspace") ) {
		BOOL		abs;
		CONST UB	*v = tad_attr(nd, "width");
		INT		w;

		para_need(b);
		run_add(b, TV_RUN_FIXSP, NULL, 0, -1, NULL, nd, 0);
		w = scale_of(v, &abs);
		b->d->run[b->d->nrun - 1].width = abs ? w
			: ( v != NULL && v[0] != 0 ) ? b->st[b->depth].size * w / 1000
			: b->st[b->depth].size;
		return;
	}
	if ( name_is(nd, "fill-char") ) {
		CONST UB	*v = tad_attr(nd, "str");

		para_need(b);
		run_add(b, TV_RUN_FILL, v, ( v != NULL ) ? text_len(v) : 0, -1,
			NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "page-number") ) {
		CONST UB	*v = tad_attr(nd, "num");

		para_need(b);
		run_add(b, TV_RUN_PAGENO, v, ( v != NULL ) ? text_len(v) : 0, -1,
			NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "variable") ) {
		/* a variable, filled in as it is drawn: its number, or its name */
		CONST UB	*v = tad_attr(nd, "name");

		para_need(b);
		run_add(b, TV_RUN_VAR, v, ( v != NULL ) ? text_len(v) : 0,
			to_int(tad_attr(nd, "id"), -1), NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "line-head-kinsoku") || name_is(nd, "line-tail-kinsoku") ) {
		/* 禁則 from here on: its kind 0xKL and its letters */
		BOOL		head = name_is(nd, "line-head-kinsoku");
		CONST UB	*k = tad_attr(nd, "kind"), *ch = tad_attr(nd, "ch");
		INT		kind = 0, i, d;
		T_TVPARA	*p = para_now(b);

		for ( i = ( k != NULL && k[0] == '0' && ( k[1] == 'x' || k[1] == 'X' ) ) ? 2 : 0;
		      k != NULL && ( d = hex_digit(k[i]) ) >= 0; i++ ) {
			kind = kind * 16 + d;
		}
		if ( k == NULL ) {
			kind = 0x10;
		}
		if ( head ) {
			b->ps.khead = ch;
			b->ps.khead_len = ( ch != NULL ) ? text_len(ch) : 0;
			b->ps.khead_kind = kind;
		} else {
			b->ps.ktail = ch;
			b->ps.ktail_len = ( ch != NULL ) ? text_len(ch) : 0;
			b->ps.ktail_kind = kind;
		}
		if ( p != NULL && p->n == 0 ) {
			p->khead = b->ps.khead;
			p->khead_len = b->ps.khead_len;
			p->khead_kind = b->ps.khead_kind;
			p->ktail = b->ps.ktail;
			p->ktail_len = b->ps.ktail_len;
			p->ktail_kind = b->ps.ktail_kind;
		}
		return;
	}
	if ( name_is(nd, "fill-line") ) {
		/* before any text: what follows goes to the foot of the page */
		if ( b->d->nrun == 0 ) {
			b->d->foot = TRUE;
		}
		return;
	}
	if ( name_is(nd, "docmemo") ) {
		CONST UB	*v = tad_attr(nd, "text");

		para_need(b);
		run_add(b, TV_RUN_MEMO, v, ( v != NULL ) ? text_len(v) : 0, -1,
			NULL, nd, 0);
		return;
	}
	if ( name_is(nd, "tab-format") ) {
		tab_format(b, nd);
		return;
	}
	if ( name_is(nd, "font") && nd->first == NULL ) {
		/*
		 * A <font .../> with nothing in it changes the letters from
		 * here on, until another changes them again.
		 */
		font_attrs(b, &b->st[b->depth], nd);
		return;
	}
	if ( name_is(nd, "font") ) {
		BOOL	pushed = push(b);

		font_attrs(b, &b->st[b->depth], nd);
		doc_children(b, nd);
		pop(b, pushed);
		return;
	}
	for ( i = 0; i < sizeof(deco_tab) / sizeof(deco_tab[0]); i++ ) {
		if ( name_is(nd, deco_tab[i].name) ) {
			BOOL	pushed = push(b);

			b->st[b->depth].style |= deco_tab[i].style;
			doc_children(b, nd);
			pop(b, pushed);
			return;
		}
	}
	if ( name_is(nd, "background") ) {
		BOOL	pushed = push(b);

		b->st[b->depth].back = tv_colour(tad_attr(nd, "color"), 0x00FFFF00U);
		doc_children(b, nd);
		pop(b, pushed);
		return;
	}
	if ( name_is(nd, "attend") ) {
		/* 添え字: type 1 above the line, 0 below it */
		BOOL	pushed = push(b);

		b->st[b->depth].style |= ( to_int(tad_attr(nd, "type"), 0) == 1 )
					 ? TV_ST_SUP : TV_ST_SUB;
		doc_children(b, nd);
		pop(b, pushed);
		return;
	}
	if ( name_is(nd, "bouten") ) {
		BOOL		pushed = push(b);
		CONST UB	*side = tad_attr(nd, "side");

		b->st[b->depth].bouten =
			( ( side != NULL && side[0] == 'l' ) ? 2U : 1U )
			| ( (UINT)to_int(tad_attr(nd, "kind"), 0) << 4 );
		doc_children(b, nd);
		pop(b, pushed);
		return;
	}
	if ( name_is(nd, "ruby") ) {
		BOOL		pushed = push(b);
		CONST UB	*t = tad_attr(nd, "text");
		CONST UB	*pos = tad_attr(nd, "position");

		b->st[b->depth].ruby = t;
		b->st[b->depth].ruby_len = ( t != NULL ) ? text_len(t) : 0;
		/* "bottom", or 1 as the menu writes it: under the letters */
		b->st[b->depth].ruby_below = (BOOL)( pos != NULL
						    && ( pos[0] == 'b' || pos[0] == '1' ) );
		b->st[b->depth].ruby_group = b->ruby_next++;
		doc_children(b, nd);
		pop(b, pushed);
		return;
	}
	if ( name_is(nd, "link") ) {
		para_need(b);
		run_add(b, TV_RUN_LINK, NULL, 0, b->link_no, NULL, nd, 0);
		b->link_no++;
		return;
	}
	if ( name_is(nd, "image") ) {
		para_need(b);
		run_add(b, TV_RUN_IMAGE, NULL, 0, -1, tad_attr(nd, "href"), nd, 0);
		return;
	}
	if ( name_is(nd, "figure") ) {
		/* a figure standing in the text, drawn in the room its view takes */
		T_TVDOC		*d = b->d;
		T_TVFIG		*f = NULL;

		if ( d->figs == NULL ) {
			d->figs = (T_TVFIG **)Kmalloc(sizeof(T_TVFIG *) * DOC_MAX_FIG);
			d->nfig = 0;
		}
		if ( d->figs == NULL || d->nfig >= DOC_MAX_FIG
		  || tv_fig_of(nd, &f) < E_OK || f == NULL ) {
			return;
		}
		d->figs[d->nfig] = f;
		para_need(b);
		run_add(b, TV_RUN_FIGURE, NULL, 0, d->nfig, NULL, nd, 0);
		d->nfig++;
		return;
	}
	if ( name_is(nd, "docView") ) {
		rect_attr(nd, "viewleft", "viewtop", "viewright", "viewbottom",
			  &b->d->view);
		return;
	}
	if ( name_is(nd, "docDraw") ) {
		rect_attr(nd, "drawleft", "drawtop", "drawright", "drawbottom",
			  &b->d->draw);
		return;
	}
	if ( name_is(nd, "docScale") ) {
		b->d->hunit = to_int(tad_attr(nd, "hunit"), b->d->hunit);
		b->d->vunit = to_int(tad_attr(nd, "vunit"), b->d->vunit);
		return;
	}
	if ( name_is(nd, "docmargin") ) {
		INT	v;

		/* 65535 keeps what the last one said */
		v = to_int(tad_attr(nd, "left"), to_int(tad_attr(nd, "marginleft"), b->d->margin[0]));
		if ( v != 65535 ) b->d->margin[0] = v;
		v = to_int(tad_attr(nd, "top"), to_int(tad_attr(nd, "margintop"), b->d->margin[1]));
		if ( v != 65535 ) b->d->margin[1] = v;
		v = to_int(tad_attr(nd, "right"), to_int(tad_attr(nd, "marginright"), b->d->margin[2]));
		if ( v != 65535 ) b->d->margin[2] = v;
		v = to_int(tad_attr(nd, "bottom"), to_int(tad_attr(nd, "marginbottom"), b->d->margin[3]));
		if ( v != 65535 ) b->d->margin[3] = v;
		return;
	}
	if ( name_is(nd, "paper") ) {
		b->d->paper_w = to_int(tad_attr(nd, "width"), b->d->paper_w);
		b->d->paper_h = to_int(tad_attr(nd, "length"), b->d->paper_h);
		return;
	}
	if ( name_is(nd, "column") ) {
		b->d->columns = to_int(tad_attr(nd, "column"), 1);
		b->d->colsp = to_int(tad_attr(nd, "colsp"), 0);
		return;
	}
	if ( name_is(nd, "text") ) {
		CONST UB	*v;
		T_TVPARA	*p = para_now(b);

		b->d->lang = to_int(tad_attr(nd, "lang"), b->d->lang);
		b->d->bpat = to_int(tad_attr(nd, "bpat"), b->d->bpat);
		/* the alignment and the line pitch, from here on */
		if ( ( v = tad_attr(nd, "align") ) != NULL ) {
			b->ps.align = ( v[0] == 'c' ) ? TV_ALIGN_CENTRE
				    : ( v[0] == 'r' ) ? TV_ALIGN_RIGHT
				    : ( v[0] == 'j' ) ? TV_ALIGN_JUSTIFY : TV_ALIGN_LEFT;
			if ( p != NULL ) {
				p->align = b->ps.align;
				p->align_at = (T_TADNODE *)nd;
			}
		}
		if ( ( v = tad_attr(nd, "line-height") ) != NULL ) {
			BOOL	abs;
			INT	lh = scale_of(v, &abs);

			if ( lh > 10000 ) {
				lh = 1500;	/* not a ratio at all */
			}
			if ( lh > 1000 ) {
				b->ps.gap = lh - 1000;
				b->ps.gap_abs = FALSE;
				b->ps.gap_set = TRUE;
				if ( p != NULL ) {
					p->gap = b->ps.gap;
					p->gap_abs = FALSE;
				}
			}
		}
		return;
	}
	/* nothing to show of these, and nothing inside them to show */
	if ( name_is(nd, "paper-overlay-define") ) {
		/* an overlay defined: a text of its own, until defined again */
		INT		n = to_int(tad_attr(nd, "N"), -1);
		T_TVDOC		*o = NULL;

		if ( n >= 0 && n < TV_MAX_OVER && tv_doc_of(nd, &o) >= E_OK ) {
			tv_doc_free(b->d->over[n]);
			b->d->over[n] = o;
			b->d->over_pages[n] = (UINT)to_int(tad_attr(nd, "P"), 0);
		}
		return;
	}
	if ( name_is(nd, "docoverlay") ) {
		/* the overlays put on from here: their numbers, listed */
		CONST UB	*v = tad_attr(nd, "active");

		if ( v != NULL ) {
			b->d->over_on = over_list(v);
		}
		return;
	}
	if ( name_is(nd, "tcode") || name_is(nd, "page-number-define")
	  || name_is(nd, "frame-open") || name_is(nd, "field-format")
	  || name_is(nd, "docappl") || name_is(nd, "tadseg") ) {
		return;
	}
	/* anything else: whatever is inside it still counts */
	doc_children(b, nd);
}

/* How many paragraphs and nodes there are under a node */
LOCAL void count_nodes( CONST T_TADNODE *nd, INT *np, INT *nr )
{
	CONST T_TADNODE	*c;

	for ( c = nd->first; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && name_is(c, "p") ) {
			(*np)++;
		}
		(*nr)++;
		count_nodes(c, np, nr);
	}
}

LOCAL T_TVDOC *doc_build( CONST T_TADNODE *body )
{
	DOCBUILD	b;
	T_TVDOC		*d;
	INT		np = 0, nr = 0;

	d = (T_TVDOC *)Kmalloc(sizeof(T_TVDOC));
	if ( d == NULL ) {
		return NULL;
	}
	knl_memset(d, 0, sizeof(*d));
	d->hunit = d->vunit = -72;
	d->columns = 1;
	/*
	 * Room for as many paragraphs and runs as the record could make:
	 * one paragraph per <p>, and at most one run per node.
	 */
	count_nodes(body, &np, &nr);
	np = np + 2;
	nr = nr + 2;
	if ( np < DOC_MAX_PARA ) np = DOC_MAX_PARA;
	if ( nr < DOC_MAX_RUN ) nr = DOC_MAX_RUN;
	d->para = (T_TVPARA *)Kmalloc(sizeof(T_TVPARA) * (SZ)np);
	d->run  = (T_TVRUN *)Kmalloc(sizeof(T_TVRUN) * (SZ)nr);
	if ( d->para == NULL || d->run == NULL ) {
		tv_doc_free(d);
		return NULL;
	}
	knl_memset(&b, 0, sizeof(b));
	b.d = d;
	b.maxpara = np;
	b.maxrun = nr;
	b.depth = 0;
	b.link_no = 0;
	b.st[0].size = 14;
	b.st[0].colour = 0x00000000U;
	b.st[0].style = 0;
	b.st[0].face = NULL;
	b.st[0].wr_n = b.st[0].wr_d = 1;
	b.st[0].hr_n = b.st[0].hr_d = 1;
	b.st[0].back = TAD_COL_NONE;
	b.st[0].ruby_group = -1;
	b.ps.align = TV_ALIGN_LEFT;
	b.ps.khead_kind = b.ps.ktail_kind = -1;

	doc_children(&b, body);

	/* the paper and the margins, from the record's units into pixels */
	if ( d->hunit < 0 && d->hunit != -72 ) {
		d->paper_w = d->paper_w * 72 / -d->hunit;
		d->margin[0] = d->margin[0] * 72 / -d->hunit;
		d->margin[2] = d->margin[2] * 72 / -d->hunit;
		d->colsp = d->colsp * 72 / -d->hunit;
	}
	if ( d->vunit < 0 && d->vunit != -72 ) {
		d->paper_h = d->paper_h * 72 / -d->vunit;
		d->margin[1] = d->margin[1] * 72 / -d->vunit;
		d->margin[3] = d->margin[3] * 72 / -d->vunit;
	}

	return d;
}

EXPORT ER tv_doc( CONST T_TAD *doc, T_TVDOC **p_out )
{
	T_TADNODE	*body = tad_body(doc);

	if ( p_out == NULL ) {
		return E_PAR;
	}
	if ( !name_is(body, "document") ) {
		return E_NOEXS;
	}
	*p_out = doc_build(body);

	return ( *p_out != NULL ) ? E_OK : E_NOMEM;
}

/* A document made from an element of its own: a piece of text in a figure */
EXPORT ER tv_doc_of( CONST T_TADNODE *nd, T_TVDOC **p_out )
{
	if ( nd == NULL || p_out == NULL ) {
		return E_PAR;
	}
	*p_out = doc_build(nd);

	return ( *p_out != NULL ) ? E_OK : E_NOMEM;
}

EXPORT void tv_doc_free( T_TVDOC *d )
{
	if ( d == NULL ) {
		return;
	}
	if ( d->figs != NULL ) {
		INT	i;

		for ( i = 0; i < d->nfig; i++ ) {
			tv_fig_free(d->figs[i]);
		}
		Kfree(d->figs);
	}
	{
		INT	i;

		for ( i = 0; i < TV_MAX_OVER; i++ ) {
			tv_doc_free(d->over[i]);
		}
	}
	if ( d->para != NULL ) {
		Kfree(d->para);
	}
	if ( d->run != NULL ) {
		Kfree(d->run);
	}
	Kfree(d);
}

/* ---------------------------------------------------------------- figures */

#define CP_MAX		TV_MAX_FPAT	/* a figure's own patterns that are kept */
#define CP_NCOL		8		/* colours of one of them */
#define MK_MAX		128		/* masks */

/*
 * One of the figure's own patterns as it is written: its colours, each
 * with the mask saying where it shows, and the ground under them. The
 * masks it names may be given after it, so it is made into pixels only
 * when a shape first uses it.
 */
typedef struct {
	INT	id;
	INT	ncol;
	UW	col[CP_NCOL];
	INT	mask[CP_NCOL];		/* -1: all of it */
	UW	bg;			/* TAD_COL_NONE: none */
	INT	made;			/* its place in the figure's pats, or -1 */
} RAWPAT;

typedef struct {
	T_TVFIG	*f;
	INT	link_no;
	INT	z_next;

	/* the place of the element of the figure being read */
	INT	place;

	/*
	 * The figure's own patterns, and the masks they are drawn
	 * through: in memory of their own, being too large to keep on
	 * the stack of whoever reads a figure.
	 */
	INT	ncp;
	RAWPAT	*cp;
	INT	nmk;
	INT	*mk_id;
	UH	(*mk_rows)[16];

	/*
	 * What the element before said of the next one: a <transform> to
	 * move, turn and lean it by, and a <figmodifier> to give it arrows.
	 * They are worked into its coordinates as it is read.
	 */
	BOOL	ts_on;
	INT	ts_dh, ts_dv;
	INT	ts_turn, ts_lean;	/* 4096ths of a turn */
	INT	fm_arrow;		/* the arrows, as a line's are kept; -1 none */

	/* the kinds of mark the figure defines: <markerDefine> */
	INT	nmd;
	INT	md_id[16], md_kind[16], md_size[16];
	UW	md_col[16];
} FIGBUILD;

/* A mask of the figure's own, its sixteen rows, or NULL */
LOCAL CONST UH *mask_of( CONST FIGBUILD *b, INT id )
{
	INT	i;

	for ( i = 0; i < b->nmk; i++ ) {
		if ( b->mk_id[i] == id ) {
			return b->mk_rows[i];
		}
	}

	return NULL;
}

/* A pattern of the figure's own made into pixels, when first used */
LOCAL CONST T_TVPAT *pat_make( FIGBUILD *b, RAWPAT *rp )
{
	T_TVFIG		*f = b->f;
	T_TVPAT		*p;
	INT		c, x, y;

	if ( rp->made >= 0 ) {
		return &f->pats[rp->made];
	}
	if ( f->pats == NULL ) {
		f->pats = (T_TVPAT *)Kmalloc(sizeof(T_TVPAT) * TV_MAX_FPAT);
		f->npats = 0;
	}
	if ( f->pats == NULL || f->npats >= TV_MAX_FPAT ) {
		return NULL;
	}
	p = &f->pats[f->npats];
	knl_memset(p, 0, sizeof(*p));
	p->id = rp->id;
	p->colour = ( rp->ncol > 0 ) ? rp->col[0] : rp->bg;
	if ( rp->bg != TAD_COL_NONE ) {
		for ( y = 0; y < 16; y++ ) {
			for ( x = 0; x < 16; x++ ) {
				p->tile[y * 16 + x] = rp->bg;
			}
			p->mask[y] = 0xFFFF0000U;
		}
	}
	for ( c = 0; c < rp->ncol; c++ ) {
		CONST UH	*m = ( rp->mask[c] >= 0 ) ? mask_of(b, rp->mask[c]) : NULL;

		for ( y = 0; y < 16; y++ ) {
			UH	row = ( m != NULL ) ? m[y]
				    : ( rp->mask[c] < 0 ) ? 0xFFFFU : 0;

			for ( x = 0; x < 16; x++ ) {
				if ( ( row & ( 0x8000U >> x ) ) != 0 ) {
					p->tile[y * 16 + x] = rp->col[c];
					p->mask[y] |= 0x80000000U >> x;
				}
			}
		}
	}
	rp->made = f->npats++;

	return p;
}

/* "#rrggbb,#rrggbb" or "1,2": the next item of a list, or NULL */
LOCAL CONST UB *list_next( CONST UB *s, CONST UB **p_item, INT *p_len )
{
	CONST UB	*a;

	if ( s == NULL ) {
		return NULL;
	}
	while ( *s == ',' || *s == ' ' ) {
		s++;
	}
	if ( *s == 0 ) {
		return NULL;
	}
	a = s;
	while ( *s != 0 && *s != ',' && *s != ' ' ) {
		s++;
	}
	*p_item = a;
	*p_len = (INT)( s - a );

	return s;
}

/* A mask of the figure's own: its rows, sixteen pixels across, repeated */
LOCAL void mask_read( FIGBUILD *b, CONST T_TADNODE *nd )
{
	CONST UB	*s = tad_attr(nd, "data"), *it;
	INT		w = to_int(tad_attr(nd, "width"), 16);
	INT		h = to_int(tad_attr(nd, "height"), 16);
	INT		n = 0, len, k, y;
	UW		rows[16];

	if ( b->nmk >= MK_MAX || s == NULL || w <= 0 || h <= 0 ) {
		return;
	}
	while ( n < 16 && ( s = list_next(s, &it, &len) ) != NULL ) {
		UW	v = 0;

		for ( k = 0; k < len; k++ ) {
			UB	c = it[k];

			v <<= 4;
			if ( c >= '0' && c <= '9' )      v |= (UW)( c - '0' );
			else if ( c >= 'a' && c <= 'f' ) v |= (UW)( c - 'a' + 10 );
			else if ( c >= 'A' && c <= 'F' ) v |= (UW)( c - 'A' + 10 );
		}
		rows[n++] = v & 0xFFFFU;
	}
	if ( n == 0 ) {
		return;
	}
	if ( w > 16 ) w = 16;
	if ( h > n ) h = n;
	for ( y = 0; y < 16; y++ ) {
		UW	src = rows[y % h], row = 0;
		INT	x;

		/* a narrower mask is laid again beside itself */
		for ( x = 0; x < 16; x++ ) {
			if ( ( src & ( 0x8000U >> ( x % w ) ) ) != 0 ) {
				row |= 0x8000U >> x;
			}
		}
		b->mk_rows[b->nmk][y] = (UH)row;
	}
	b->mk_id[b->nmk] = to_int(tad_attr(nd, "id"), -1);
	b->nmk++;
}

/* One of the figure's own patterns: its colours, masks and ground */
LOCAL void pattern_read( FIGBUILD *b, CONST T_TADNODE *nd )
{
	CONST UB	*s, *it;
	RAWPAT		*rp;
	INT		len, i;

	if ( b->ncp >= CP_MAX ) {
		return;
	}
	rp = &b->cp[b->ncp];
	knl_memset(rp, 0, sizeof(*rp));
	rp->id = to_int(tad_attr(nd, "id"), 0);
	rp->made = -1;
	rp->bg = tv_colour(tad_attr(nd, "bgcolor"), TAD_COL_NONE);
	s = tad_attr(nd, "fgcolors");
	while ( rp->ncol < CP_NCOL && ( s = list_next(s, &it, &len) ) != NULL ) {
		UB	one[16];

		for ( i = 0; i < len && i < 15; i++ ) {
			one[i] = it[i];
		}
		one[i] = 0;
		rp->col[rp->ncol] = tv_colour(one, 0x00000000U);
		rp->mask[rp->ncol] = -1;
		rp->ncol++;
	}
	s = tad_attr(nd, "masks");
	for ( i = 0; i < rp->ncol && ( s = list_next(s, &it, &len) ) != NULL; i++ ) {
		UB	one[12];
		INT	k;

		for ( k = 0; k < len && k < 11; k++ ) {
			one[k] = it[k];
		}
		one[k] = 0;
		rp->mask[i] = to_int(one, -1);
	}
	if ( rp->ncol == 0 && rp->bg == TAD_COL_NONE ) {
		return;
	}
	b->ncp++;
}

EXPORT CONST T_TVPAT *tv_fig_pattern( CONST T_TVFIG *f, INT id )
{
	INT	i;

	for ( i = 0; f != NULL && f->pats != NULL && i < f->npats; i++ ) {
		if ( f->pats[i].id == id ) {
			return &f->pats[i];
		}
	}

	return NULL;
}

/*
 * What a pattern number comes to: a colour, or nothing for 0, and a
 * mask for a fixed pattern that is one.
 */
LOCAL BOOL pat_of( FIGBUILD *b, INT id, UW *p_col, CONST UH **p_tex,
		   CONST T_TVPAT **p_pat )
{
	INT	i;

	*p_tex = NULL;
	*p_pat = NULL;
	if ( id == 0 ) {
		*p_col = TAD_COL_NONE;
		return TRUE;
	}
	/* the figure's own first: it may draw a fixed number its own way */
	for ( i = 0; i < b->ncp; i++ ) {
		if ( b->cp[i].id == id ) {
			RAWPAT	*rp = &b->cp[i];

			*p_col = ( rp->ncol > 0 ) ? rp->col[0] : rp->bg;
			/* one colour over all of it is a colour, not a pattern */
			if ( rp->ncol == 1 && rp->mask[0] < 0 ) {
				return TRUE;
			}
			*p_pat = pat_make(b, rp);
			return TRUE;
		}
	}
	if ( id < TV_PAT_FIXED ) {
		return tv_pattern(id, p_col, p_tex);
	}

	return FALSE;
}

LOCAL T_TVSHAPE *shape_add( FIGBUILD *b, UINT kind, CONST T_TADNODE *nd )
{
	T_TVSHAPE	*s;

	if ( b->f->nsh >= TV_MAX_SHAPE ) {
		return NULL;
	}
	s = &b->f->sh[b->f->nsh];
	s->kind = kind;
	s->r.left = s->r.top = s->r.right = s->r.bottom = 0;
	s->npt = 0;
	s->line_col = 0x00000000U;
	s->fill_col = TAD_COL_NONE;
	s->line_w = 1;
	s->line_type = 0;
	s->rad_h = s->rad_v = 0;
	s->doc = -1;
	s->link = -1;
	s->href = NULL;
	s->text = NULL;
	s->text_px = 0;
	s->text_col = 0;
	s->arrow = 0;
	s->arrow_type = 0;
	s->node = (T_TADNODE *)nd;
	s->fill_tex = NULL;
	s->fill_pat = NULL;
	s->pt = NULL;
	s->rot = 0;
	s->angle = 0;
	s->a0 = 0;
	s->a1 = 4096;
	s->closed = FALSE;
	s->flip_h = s->flip_v = FALSE;
	s->back = TAD_COL_NONE;
	s->place = b->place;
	s->conn = TV_CONN_STRAIGHT;
	s->c0_shape = s->c1_shape = -1;
	s->c0_point = s->c1_point = 0;
	s->z = b->z_next++;
	if ( nd != NULL ) {
		CONST UB	*v = tad_attr(nd, "zIndex");

		if ( v == NULL ) {
			v = tad_attr(nd, "zindex");
		}
		if ( v != NULL ) {
			s->z = to_int(v, s->z);
		}
		s->line_w = to_int(tad_attr(nd, "lineWidth"), 1);
		s->line_type = (UINT)to_int(tad_attr(nd, "lineType"), 0);
		s->line_col = tv_colour(tad_attr(nd, "lineColor"),
					tv_colour(tad_attr(nd, "strokeColor"),
						  0x00000000U));
		s->fill_col = tv_colour(tad_attr(nd, "fillColor"), TAD_COL_NONE);
		{
			/* a line that is written as not there is not drawn */
			CONST UB	*lc = tad_attr(nd, "lineColor");

			if ( lc == NULL ) lc = tad_attr(nd, "strokeColor");
			if ( lc != NULL && ( lc[0] == 't' || lc[0] == 'n' ) ) s->line_w = 0;
		}
		s->rot = to_int(tad_attr(nd, "rotation"), 0);
		{
			CONST UB	*at = tad_attr(nd, "arrow_type");

			s->arrow_type = ( at != NULL && at[0] == 'f' ) ? 1 : 0;
		}
		/*
		 * The patterns by number, where no colour is written out:
		 * a line of pattern 0 is not drawn, an inside of pattern 0
		 * is not filled.
		 */
		v = tad_attr(nd, "l_pat");
		if ( v != NULL && tad_attr(nd, "lineColor") == NULL
		  && tad_attr(nd, "strokeColor") == NULL ) {
			UW		col;
			CONST UH	*tex;
			CONST T_TVPAT	*pat;

			if ( pat_of(b, to_int(v, 1), &col, &tex, &pat) ) {
				if ( col == TAD_COL_NONE ) {
					s->line_w = 0;
				} else {
					s->line_col = col;
				}
			}
		}
		v = tad_attr(nd, "f_pat");
		if ( v != NULL && tad_attr(nd, "fillColor") == NULL ) {
			UW		col;
			CONST UH	*tex;
			CONST T_TVPAT	*pat;

			if ( pat_of(b, to_int(v, 0), &col, &tex, &pat) ) {
				s->fill_col = col;
				s->fill_tex = tex;
				s->fill_pat = pat;
			}
		}
	}
	b->f->nsh++;

	return s;
}

/* "x,y x,y ..." into points */
LOCAL INT points_of( CONST UB *s, T_DPPOINT *pt, INT max )
{
	INT	n = 0;

	if ( s == NULL ) {
		return 0;
	}
	while ( *s != 0 && n < max ) {
		INT	x, y, sign = 1;

		while ( *s == ' ' || *s == ',' ) {
			s++;
		}
		if ( *s == 0 ) {
			break;
		}
		if ( *s == '-' ) { sign = -1; s++; }
		x = 0;
		while ( *s >= '0' && *s <= '9' ) { x = x * 10 + (*s - '0'); s++; }
		if ( *s == '.' ) {
			s++;
			if ( *s >= '5' && *s <= '9' ) x++;	/* to the nearest */
			while ( *s >= '0' && *s <= '9' ) s++;
		}
		x *= sign;
		while ( *s == ' ' || *s == ',' ) {
			s++;
		}
		sign = 1;
		if ( *s == '-' ) { sign = -1; s++; }
		y = 0;
		while ( *s >= '0' && *s <= '9' ) { y = y * 10 + (*s - '0'); s++; }
		if ( *s == '.' ) {
			s++;
			if ( *s >= '5' && *s <= '9' ) y++;
			while ( *s >= '0' && *s <= '9' ) s++;
		}
		y *= sign;
		pt[n].x = x;
		pt[n].y = y;
		n++;
	}

	return n;
}

LOCAL void fig_walk( FIGBUILD *b, CONST T_TADNODE *nd );

/*
 * 線種定義: a kind of line of the figure's own, its pattern a bit a
 * pixel from the top bit of 'mask' (hexadecimal, nb bytes) and drawn
 * over and over. It is kept as the lengths of its runs, the first one
 * drawn; one of all bits set is a solid line.
 */
LOCAL void lkind_read( T_TVFIG *f, CONST T_TADNODE *nd )
{
	CONST UB	*m = tad_attr(nd, "mask");
	INT		id = to_int(tad_attr(nd, "id"), -1), nbit = 0, i, k, n = 0, start = -1;
	UB		bits[64];
	UB		*dash;

	if ( m == NULL || id < 0 || f->nlt >= TV_MAX_LTYPE ) {
		return;
	}
	for ( i = 0; m[i] != 0 && m[i + 1] != 0 && nbit + 8 <= 64 * 8; i += 2 ) {
		INT	hi = hex_digit(m[i]), lo = hex_digit(m[i + 1]);

		if ( hi < 0 || lo < 0 ) {
			return;
		}
		bits[nbit / 8] = (UB)( hi * 16 + lo );
		nbit += 8;
	}
	/* the first bit drawn after one not drawn: the dashes start there */
	for ( i = 0; i < nbit && start < 0; i++ ) {
		INT	a = ( bits[i / 8] >> ( 7 - i % 8 ) ) & 1;
		INT	p = ( bits[( i + nbit - 1 ) % nbit / 8] >> ( 7 - ( i + nbit - 1 ) % nbit % 8 ) ) & 1;

		if ( a == 1 && p == 0 ) {
			start = i;
		}
	}
	dash = f->lt_dash[f->nlt];
	if ( start >= 0 ) {
		for ( k = 0; k < nbit && n < TV_DASH_MAX - 1; ) {
			INT	i0 = ( start + k ) % nbit;
			INT	v = ( bits[i0 / 8] >> ( 7 - i0 % 8 ) ) & 1, len = 0;

			while ( k < nbit ) {
				INT	j = ( start + k ) % nbit;

				if ( ( ( bits[j / 8] >> ( 7 - j % 8 ) ) & 1 ) != v ) {
					break;
				}
				len++;
				k++;
			}
			dash[n++] = (UB)( ( len < 255 ) ? len : 255 );
		}
		if ( n & 1 ) {
			n--;		/* runs on and off in pairs */
		}
	}
	dash[n] = 0;
	f->lt_id[f->nlt++] = id;
}

EXPORT CONST UB *tv_fig_dash( CONST T_TVFIG *f, UINT type )
{
	INT	i;

	for ( i = ( f != NULL ) ? f->nlt - 1 : -1; i >= 0; i-- ) {
		if ( f->lt_id[i] == (INT)type ) {
			return f->lt_dash[i];
		}
	}

	return NULL;
}

/* A shape's points, out of the figure's pool: as many as the attribute has */
LOCAL INT points_take( FIGBUILD *b, T_TVSHAPE *s, CONST UB *attr )
{
	INT	room = TV_PT_POOL - b->f->npool;

	if ( room > TV_MAX_PT ) {
		room = TV_MAX_PT;
	}
	s->pt = b->f->pool + b->f->npool;
	s->npt = ( room > 0 ) ? points_of(attr, s->pt, room) : 0;
	b->f->npool += s->npt;

	return s->npt;
}

/* Room for n points of a shape, when they are made rather than read */
LOCAL BOOL points_room( FIGBUILD *b, T_TVSHAPE *s, INT n )
{
	if ( b->f->npool + n > TV_PT_POOL ) {
		return FALSE;
	}
	s->pt = b->f->pool + b->f->npool;
	s->npt = n;
	b->f->npool += n;

	return TRUE;
}

/* The box round a shape's points */
LOCAL void points_box( T_TVSHAPE *s )
{
	INT	k;

	if ( s->npt == 0 ) {
		return;
	}
	s->r.left = s->r.right = s->pt[0].x;
	s->r.top = s->r.bottom = s->pt[0].y;
	for ( k = 1; k < s->npt; k++ ) {
		if ( s->pt[k].x < s->r.left )   s->r.left = s->pt[k].x;
		if ( s->pt[k].x > s->r.right )  s->r.right = s->pt[k].x;
		if ( s->pt[k].y < s->r.top )    s->r.top = s->pt[k].y;
		if ( s->pt[k].y > s->r.bottom ) s->r.bottom = s->pt[k].y;
	}
	s->r.right++;
	s->r.bottom++;
}

/* "a,b" as two numbers, for a line's joined ends */
LOCAL BOOL pair_of( CONST UB *v, INT *a, INT *b )
{
	INT	x = 0, y = 0;

	if ( v == NULL || *v < '0' || *v > '9' ) {
		return FALSE;
	}
	while ( *v >= '0' && *v <= '9' ) x = x * 10 + ( *v++ - '0' );
	if ( *v++ != ',' ) {
		return FALSE;
	}
	while ( *v >= '0' && *v <= '9' ) y = y * 10 + ( *v++ - '0' );
	*a = x;
	*b = y;

	return TRUE;
}

/*
 * Where an arc of an ellipse starts or ends: the point given is only a
 * direction from the middle, and the arc starts where that direction
 * meets the ellipse -- at this parameter.
 */
LOCAL INT arc_param( CONST T_DPRECT *r, INT px, INT py )
{
	D	cx2 = (D)r->left + r->right - 2, cy2 = (D)r->top + r->bottom - 2;
	D	dx2 = (D)( px - 1 ) * 2 - cx2, dy2 = (D)( py - 1 ) * 2 - cy2;
	D	rx = r->right - r->left, ry = r->bottom - r->top;

	if ( rx <= 0 || ry <= 0 ) {
		return 0;
	}

	return tv_atan2(dy2 * rx, dx2 * ry);
}

/* ---------------------------------------------------------------- worked into coordinates */

/* A place as a <transform> takes it: leaned, turned, then moved */
LOCAL void ts_point( CONST FIGBUILD *b, INT *px, INT *py )
{
	D	x = *px, y = *py;

	if ( b->ts_lean != 0 ) {
		D	c = tv_cos(b->ts_lean);

		if ( c != 0 ) {
			x = x + y * tv_sin(b->ts_lean) / c;
		}
	}
	if ( b->ts_turn != 0 ) {
		D	c = tv_cos(b->ts_turn), s = tv_sin(b->ts_turn);
		D	nx = ( x * c - y * s ) / 16384, ny = ( x * s + y * c ) / 16384;

		x = nx;
		y = ny;
	}
	*px = (INT)( x + b->ts_dh );
	*py = (INT)( y + b->ts_dv );
}

/* A place of a figure inside the figure: from its drawing area to its view */
typedef struct {
	T_DPRECT	view, draw;
} INNER;

LOCAL void inner_point( CONST INNER *in, INT *px, INT *py )
{
	INT	dw = in->draw.right - in->draw.left, dh = in->draw.bottom - in->draw.top;
	INT	vw = in->view.right - in->view.left, vh = in->view.bottom - in->view.top;

	*px = in->view.left + (INT)( (D)( *px - in->draw.left ) * vw / dw );
	*py = in->view.top + (INT)( (D)( *py - in->draw.top ) * vh / dh );
}

/*
 * The shapes from n0 on, their boxes and their points moved as a place
 * is by the function given: a <transform>, or a figure's own view.
 */
LOCAL void shapes_map( FIGBUILD *b, INT n0, CONST FIGBUILD *tf, CONST INNER *in )
{
	INT	i, k;

	for ( i = n0; i < b->f->nsh; i++ ) {
		T_TVSHAPE	*s = &b->f->sh[i];
		INT		x0 = s->r.left, y0 = s->r.top;
		INT		x1 = s->r.right, y1 = s->r.bottom;

		if ( tf != NULL ) {
			ts_point(tf, &x0, &y0);
			ts_point(tf, &x1, &y1);
		} else {
			inner_point(in, &x0, &y0);
			inner_point(in, &x1, &y1);
		}
		s->r.left = ( x0 < x1 ) ? x0 : x1;
		s->r.right = ( x0 < x1 ) ? x1 : x0;
		s->r.top = ( y0 < y1 ) ? y0 : y1;
		s->r.bottom = ( y0 < y1 ) ? y1 : y0;
		for ( k = 0; k < s->npt; k++ ) {
			if ( tf != NULL ) {
				ts_point(tf, &s->pt[k].x, &s->pt[k].y);
			} else {
				inner_point(in, &s->pt[k].x, &s->pt[k].y);
			}
		}
	}
}

/* The shapes of an element: each child that is one */
LOCAL void fig_children( FIGBUILD *b, CONST T_TADNODE *nd )
{
	CONST T_TADNODE	*c;

	for ( c = nd->first; c != NULL; c = c->next ) {
		fig_walk(b, c);
	}
}

LOCAL void fig_walk1( FIGBUILD *b, CONST T_TADNODE *nd );

/*
 * One element of the figure. A <transform> or a <figmodifier> is kept
 * for the element after it, and worked into every shape that one makes.
 */
LOCAL void fig_walk( FIGBUILD *b, CONST T_TADNODE *nd )
{
	FIGBUILD	tf;
	BOOL		had;
	INT		arrow, n0 = b->f->nsh;

	if ( nd->kind == TAD_ND_ELEM && name_is(nd, "transform") ) {
		b->ts_on = TRUE;
		b->ts_dh = to_int(tad_attr(nd, "dh"), 0);
		b->ts_dv = to_int(tad_attr(nd, "dv"), 0);
		b->ts_turn = to_int(tad_attr(nd, "hangle"), 0) * 4096 / 360;
		b->ts_lean = to_int(tad_attr(nd, "vangle"), 0) * 4096 / 360;
		return;
	}
	if ( nd->kind == TAD_ND_ELEM && name_is(nd, "figmodifier") ) {
		CONST UB	*a = tad_attr(nd, "arrow");

		/* start, end or both; or the number: 1 the end, 2 the start */
		b->fm_arrow = ( a == NULL ) ? -1
			    : ( a[0] == 'b' || a[0] == '3' ) ? 3
			    : ( a[0] == 's' || a[0] == '2' ) ? 2
			    : ( a[0] == 'e' || a[0] == '1' ) ? 1 : -1;
		return;
	}
	if ( nd->kind != TAD_ND_ELEM ) {
		return;
	}
	/* what was kept is this element's, and not its children's alone */
	had = b->ts_on;
	tf = *b;
	arrow = b->fm_arrow;
	b->ts_on = FALSE;
	b->fm_arrow = -1;
	fig_walk1(b, nd);
	if ( had && b->f->nsh > n0 ) {
		shapes_map(b, n0, &tf, NULL);
	} else if ( had ) {
		b->ts_on = TRUE;	/* nothing drawn yet: for the next one */
	}
	if ( arrow >= 0 && b->f->nsh > n0 ) {
		b->f->sh[n0].arrow = arrow;
	} else if ( arrow >= 0 ) {
		b->fm_arrow = arrow;
	}
}

LOCAL ER fig_make( CONST T_TAD *doc, CONST T_TADNODE *body, INT *p_link,
		   T_TVFIG **p_out );

/*
 * 用紙オーバーレイ of a figure. One with a number defines it: the
 * shapes it holds are a figure of their own, until it is defined again;
 * 'even' and 'odd' say which pages have it. One with 'active' lists the
 * overlays put on. The links in a definition are numbered on from the
 * figure's, as the record counts them.
 */
LOCAL void fig_overlay( FIGBUILD *b, CONST T_TADNODE *nd )
{
	CONST UB	*v = tad_attr(nd, "active");
	INT		n = to_int(tad_attr(nd, "number"), -1);
	T_TVFIG		*o = NULL;
	INT		link = b->link_no;

	if ( v != NULL && tad_attr(nd, "number") == NULL ) {
		b->f->over_on = over_list(v);
		return;
	}
	if ( fig_make(NULL, nd, &link, &o) < E_OK ) {
		return;
	}
	b->link_no = link;
	if ( n < 0 || n >= TV_MAX_OVER ) {
		tv_fig_free(o);
		return;
	}
	{
		CONST UB	*ev = tad_attr(nd, "even"), *od = tad_attr(nd, "odd");
		BOOL		even = (BOOL)( ev == NULL || ev[0] == 't' || ev[0] == '1' );
		BOOL		odd = (BOOL)( od == NULL || od[0] == 't' || od[0] == '1' );

		b->f->over_pages[n] = ( even && odd ) ? 0 : odd ? 1 : even ? 2 : 3;
	}
	tv_fig_free(b->f->over[n]);
	b->f->over[n] = o;
}

LOCAL void fig_walk1( FIGBUILD *b, CONST T_TADNODE *nd )
{
	T_TVSHAPE	*s;

	if ( nd->kind != TAD_ND_ELEM ) {
		return;
	}
	if ( name_is(nd, "figoverlay") ) {
		fig_overlay(b, nd);
		return;
	}
	if ( name_is(nd, "figure") ) {
		/*
		 * A figure inside the figure: its shapes are drawn from its
		 * own drawing area into its view, as a picture is fitted to
		 * the box it is given.
		 */
		INNER		in;
		BOOL		hv = FALSE, hd = FALSE;
		CONST T_TADNODE	*c;
		INT		n0 = b->f->nsh;

		knl_memset(&in, 0, sizeof(in));
		for ( c = nd->first; c != NULL; c = c->next ) {
			if ( c->kind != TAD_ND_ELEM ) {
				continue;
			}
			if ( name_is(c, "figView") ) {
				rect_attr(c, "left", "top", "right", "bottom", &in.view);
				hv = TRUE;
			} else if ( name_is(c, "figDraw") ) {
				rect_attr(c, "left", "top", "right", "bottom", &in.draw);
				hd = TRUE;
			}
		}
		for ( c = nd->first; c != NULL; c = c->next ) {
			if ( c->kind == TAD_ND_ELEM && ( name_is(c, "figView")
			  || name_is(c, "figDraw") || name_is(c, "figScale") ) ) {
				continue;
			}
			fig_walk(b, c);
		}
		if ( hv && hd && in.draw.right > in.draw.left
		  && in.draw.bottom > in.draw.top ) {
			shapes_map(b, n0, NULL, &in);
		}
		return;
	}
	if ( name_is(nd, "markerDefine") || name_is(nd, "markerdefine") ) {
		/* a kind of mark: 0 to 4 the drawn ones, else one of a mask */
		if ( b->nmd < 16 ) {
			INT	id = to_int(tad_attr(nd, "id"), 0);

			b->md_id[b->nmd] = id;
			b->md_kind[b->nmd] = ( id >= 0 && id <= 4 ) ? id : 5;
			b->md_size[b->nmd] = to_int(tad_attr(nd, "size"), 16);
			b->md_col[b->nmd] = tv_colour(tad_attr(nd, "fgCol") != NULL
						      ? tad_attr(nd, "fgCol")
						      : tad_attr(nd, "fgcol"), 0x00000000U);
			b->nmd++;
		}
		return;
	}
	if ( name_is(nd, "marker") ) {
		s = shape_add(b, TV_SH_MARKER, nd);
		if ( s != NULL ) {
			CONST UB	*m = tad_attr(nd, "markerId");
			INT		id = to_int(( m != NULL ) ? m : tad_attr(nd, "markerid"), 0);
			INT		i, half;

			s->rad_h = 16;
			s->rad_v = ( id >= 0 && id <= 4 ) ? id : 5;
			s->line_col = 0x00000000U;
			for ( i = 0; i < b->nmd; i++ ) {
				if ( b->md_id[i] == id ) {
					s->rad_h = b->md_size[i];
					s->rad_v = b->md_kind[i];
					s->line_col = b->md_col[i];
				}
			}
			s->fill_col = s->line_col;
			s->fill_tex = NULL;
			s->fill_pat = NULL;
			(void)points_take(b, s, tad_attr(nd, "points"));
			points_box(s);
			half = s->rad_h / 2 + 1;
			s->r.left -= half;  s->r.top -= half;
			s->r.right += half;  s->r.bottom += half;
		}
		return;
	}
	if ( name_is(nd, "freefig") ) {
		/*
		 * 任意図形: bands of the rows from sy, nr of them, each from
		 * bx plus one number of 'h' to bx plus the next.
		 */
		CONST UB	*h = tad_attr(nd, "h"), *it;
		INT		sy = to_int(tad_attr(nd, "sy"), 0);
		INT		nr = to_int(tad_attr(nd, "nr"), 0);
		INT		bx = to_int(tad_attr(nd, "bx"), 0);
		INT		v[2], k = 0, len;

		while ( nr > 0 && ( h = list_next(h, &it, &len) ) != NULL ) {
			UB	one[12];
			INT	j;

			for ( j = 0; j < len && j < 11; j++ ) {
				one[j] = it[j];
			}
			one[j] = 0;
			v[k++] = to_int(one, 0);
			if ( k == 2 ) {
				s = shape_add(b, TV_SH_RECT, nd);
				if ( s != NULL ) {
					s->r.left = bx + v[0];
					s->r.right = bx + v[1];
					s->r.top = sy;
					s->r.bottom = sy + nr;
					s->line_w = 0;
				}
				k = 0;
			}
		}
		return;
	}
	if ( name_is(nd, "figView") ) {
		rect_attr(nd, "left", "top", "right", "bottom", &b->f->view);
		return;
	}
	if ( name_is(nd, "figDraw") ) {
		rect_attr(nd, "left", "top", "right", "bottom", &b->f->draw);
		return;
	}
	if ( name_is(nd, "figScale") ) {
		b->f->hunit = to_int(tad_attr(nd, "hunit"), b->f->hunit);
		b->f->vunit = to_int(tad_attr(nd, "vunit"), b->f->vunit);
		return;
	}
	if ( name_is(nd, "rect") ) {
		s = shape_add(b, TV_SH_RECT, nd);
		if ( s != NULL ) {
			INT	round = to_int(tad_attr(nd, "round"), 0);

			rect_attr(nd, "left", "top", "right", "bottom", &s->r);
			/*
			 * The corners: figRH and figRV are the diameters across
			 * and down; cornerRadius the radius when they are the
			 * same; an old record gives only round, which is then
			 * the radius itself when it is more than a flag.
			 */
			if ( tad_attr(nd, "figRH") != NULL ) {
				s->rad_h = to_int(tad_attr(nd, "figRH"), 0) / 2;
				s->rad_v = to_int(tad_attr(nd, "figRV"), 0) / 2;
			} else if ( tad_attr(nd, "cornerRadius") != NULL ) {
				s->rad_h = s->rad_v =
					to_int(tad_attr(nd, "cornerRadius"), 0);
			} else if ( round > 1 ) {
				s->rad_h = s->rad_v = round;
			}
			if ( round == 0 && tad_attr(nd, "figRH") == NULL
			  && tad_attr(nd, "cornerRadius") == NULL ) {
				s->rad_h = s->rad_v = 0;
			}
			/*
			 * A rectangle that says how its letters look is a box
			 * of text: the text inside it is drawn, and it has no
			 * frame of its own.
			 */
			if ( tad_attr(nd, "fontSize") != NULL || tad_attr(nd, "textColor") != NULL ) {
				CONST T_TADNODE	*c;

				s->line_w = 0;
				s->text_px = to_int(tad_attr(nd, "fontSize"), 14);
				s->text_col = tv_colour(tad_attr(nd, "textColor"), 0x00000000U);
				for ( c = nd->first; c != NULL; c = c->next ) {
					if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
						s->text = c->text;
						break;
					}
				}
			}
		}
		return;
	}
	if ( name_is(nd, "arc") || name_is(nd, "chord")
	  || name_is(nd, "elliptical_arc") ) {
		s = shape_add(b, name_is(nd, "arc") ? TV_SH_ARC
			      : name_is(nd, "chord") ? TV_SH_CHORD : TV_SH_EARC, nd);
		if ( s != NULL ) {
			rect_attr(nd, "frameLeft", "frameTop", "frameRight",
				  "frameBottom", &s->r);
			s->angle = to_int(tad_attr(nd, "angle"), 0) * 4096 / 360;
			s->a0 = arc_param(&s->r, to_int(tad_attr(nd, "startX"), s->r.right),
					  to_int(tad_attr(nd, "startY"), s->r.top));
			s->a1 = arc_param(&s->r, to_int(tad_attr(nd, "endX"), s->r.left),
					  to_int(tad_attr(nd, "endY"), s->r.bottom));
			s->arrow = ( to_int(tad_attr(nd, "end_arrow"), 0) != 0 ) ? 1 : 0;
			if ( to_int(tad_attr(nd, "start_arrow"), 0) != 0 ) {
				s->arrow += 2;
			}
			if ( s->kind == TV_SH_EARC ) {
				s->fill_col = TAD_COL_NONE;
				s->fill_tex = NULL;
			}
		}
		return;
	}
	if ( name_is(nd, "curve") ) {
		s = shape_add(b, TV_SH_CURVE, nd);
		if ( s != NULL ) {
			(void)points_take(b, s, tad_attr(nd, "points"));
			points_box(s);
			s->closed = (BOOL)( to_int(tad_attr(nd, "closed"), 0) != 0 );
			if ( !s->closed ) {
				s->fill_col = TAD_COL_NONE;
				s->fill_tex = NULL;
			}
		}
		return;
	}
	if ( name_is(nd, "pixelmap") ) {
		s = shape_add(b, TV_SH_PIXMAP, nd);
		if ( s != NULL ) {
			CONST UB	*fl;

			rect_attr(nd, "left", "top", "right", "bottom", &s->r);
			s->href = tad_attr(nd, "href");
			s->back = tv_colour(tad_attr(nd, "bgcolor"), TAD_COL_NONE);
			fl = tad_attr(nd, "flipH");
			s->flip_h = (BOOL)( fl != NULL && fl[0] == 't' );
			fl = tad_attr(nd, "flipV");
			s->flip_v = (BOOL)( fl != NULL && fl[0] == 't' );
		}
		return;
	}
	if ( name_is(nd, "pattern") ) {
		pattern_read(b, nd);
		return;
	}
	if ( name_is(nd, "lineTypeDefine") ) {
		lkind_read(b->f, nd);
		return;
	}
	if ( name_is(nd, "mask") ) {
		mask_read(b, nd);
		return;
	}
	if ( name_is(nd, "ellipse") || name_is(nd, "circle") ) {
		s = shape_add(b, TV_SH_ELLIPSE, nd);
		if ( s != NULL ) {
			s->angle = to_int(tad_attr(nd, "angle"), 0) * 4096 / 360;
		}
		if ( s != NULL && tad_attr(nd, "frameLeft") != NULL ) {
			rect_attr(nd, "frameLeft", "frameTop", "frameRight",
				  "frameBottom", &s->r);
		} else if ( s != NULL ) {
			INT	cx = to_int(tad_attr(nd, "cx"), 0);
			INT	cy = to_int(tad_attr(nd, "cy"), 0);
			INT	rx = to_int(tad_attr(nd, "rx"), 0);
			INT	ry = to_int(tad_attr(nd, "ry"), 0);

			if ( rx == 0 && ry == 0 ) {
				INT	rr = to_int(tad_attr(nd, "r"), 0);

				rx = ry = rr;
			}
			if ( rx > 0 || ry > 0 ) {
				s->r.left = cx - rx;  s->r.right = cx + rx;
				s->r.top = cy - ry;   s->r.bottom = cy + ry;
			} else {
				rect_attr(nd, "left", "top", "right", "bottom", &s->r);
			}
		}
		return;
	}
	if ( name_is(nd, "line") || name_is(nd, "polyline")
	  || name_is(nd, "polygon") ) {
		s = shape_add(b, name_is(nd, "polygon") ? TV_SH_POLY : TV_SH_LINE, nd);
		if ( s != NULL ) {
			if ( points_take(b, s, tad_attr(nd, "points")) == 0
			  && points_room(b, s, 2) ) {
				s->pt[0].x = to_int(tad_attr(nd, "x1"), 0);
				s->pt[0].y = to_int(tad_attr(nd, "y1"), 0);
				s->pt[1].x = to_int(tad_attr(nd, "x2"), 0);
				s->pt[1].y = to_int(tad_attr(nd, "y2"), 0);
			}
			s->arrow = ( to_int(tad_attr(nd, "end_arrow"), 0) != 0 ) ? 1 : 0;
			if ( to_int(tad_attr(nd, "start_arrow"), 0) != 0 ) {
				s->arrow += 2;
			}
			if ( s->kind == TV_SH_LINE ) {
				s->fill_col = TAD_COL_NONE;
				s->fill_tex = NULL;
			}
			/* a line joined to shapes: its ends follow them */
			if ( name_is(nd, "line") ) {
				CONST UB	*t = tad_attr(nd, "lineConnectionType");

				(void)pair_of(tad_attr(nd, "start_conn"), &s->c0_shape,
					      &s->c0_point);
				(void)pair_of(tad_attr(nd, "end_conn"), &s->c1_shape,
					      &s->c1_point);
				if ( t != NULL && t[0] == 'e' ) {
					s->conn = TV_CONN_ELBOW;
				} else if ( t != NULL && t[0] == 'c' ) {
					s->conn = TV_CONN_CURVE;
				}
			}
			points_box(s);
		}
		return;
	}
	if ( name_is(nd, "image") ) {
		s = shape_add(b, TV_SH_IMAGE, nd);
		if ( s != NULL ) {
			CONST UB	*fl;

			rect_attr(nd, "left", "top", "right", "bottom", &s->r);
			s->href = tad_attr(nd, "href");
			fl = tad_attr(nd, "flipH");
			s->flip_h = (BOOL)( fl != NULL && fl[0] == 't' );
			fl = tad_attr(nd, "flipV");
			s->flip_v = (BOOL)( fl != NULL && fl[0] == 't' );
		}
		return;
	}
	if ( name_is(nd, "link") ) {
		s = shape_add(b, TV_SH_LINK, nd);
		if ( s != NULL ) {
			s->r.left   = to_int(tad_attr(nd, "vobjleft"), 0);
			s->r.top    = to_int(tad_attr(nd, "vobjtop"), 0);
			s->r.right  = to_int(tad_attr(nd, "vobjright"), 0);
			s->r.bottom = to_int(tad_attr(nd, "vobjbottom"), 0);
			s->link = b->link_no;
			{
				CONST UB	*h = tad_attr(nd, "hidden");

				s->hidden = (BOOL)( h != NULL && h[0] == 't' );
			}
		}
		b->link_no++;
		return;
	}
	if ( name_is(nd, "document") ) {
		/* a piece of text standing in the figure: a document of its own */
		if ( b->f->ndoc < TV_MAX_DOC ) {
			T_TVDOC	*d = doc_build(nd);

			if ( d != NULL ) {
				s = shape_add(b, TV_SH_DOC, nd);
				if ( s != NULL ) {
					CONST T_TADNODE	*c;

					s->doc = b->f->ndoc;
					s->r = d->view;
					/* its place in the order is on its <text> */
					for ( c = nd->first; c != NULL; c = c->next ) {
						if ( c->kind == TAD_ND_ELEM
						  && name_is(c, "text")
						  && tad_attr(c, "zIndex") != NULL ) {
							s->z = to_int(tad_attr(c, "zIndex"),
								      s->z);
						}
					}
				}
				b->f->doc[b->f->ndoc] = d;
				b->f->ndoc++;
			}
		}
		return;
	}
	if ( name_is(nd, "group") || name_is(nd, "realGroup") ) {
		fig_children(b, nd);
		return;
	}
	/* anything else: what is inside it may still be a shape */
	fig_children(b, nd);
}

/* Shapes in the order they are drawn: what their zIndex says */
LOCAL void fig_sort( T_TVFIG *f )
{
	INT	i, j;

	for ( i = 1; i < f->nsh; i++ ) {
		T_TVSHAPE	t = f->sh[i];

		j = i - 1;
		while ( j >= 0 && f->sh[j].z > t.z ) {
			f->sh[j + 1] = f->sh[j];
			j--;
		}
		f->sh[j + 1] = t;
	}
}

EXPORT ER tv_fig( CONST T_TAD *doc, T_TVFIG **p_out )
{
	T_TADNODE	*body = tad_body(doc);
	INT		link = 0;

	if ( p_out == NULL ) {
		return E_PAR;
	}
	if ( !name_is(body, "figure") ) {
		return E_NOEXS;
	}

	return fig_make(doc, body, &link, p_out);
}

EXPORT ER tv_fig_of( CONST T_TADNODE *nd, T_TVFIG **p_out )
{
	INT	link = 0;

	if ( nd == NULL || p_out == NULL || !name_is(nd, "figure") ) {
		return E_PAR;
	}

	return fig_make(NULL, nd, &link, p_out);
}

/*
 * A figure's model from its <figure> element; with the record, the
 * paper given ahead of it as well. Its links are numbered from *p_link,
 * which is left at the number after its last.
 */
LOCAL ER fig_make( CONST T_TAD *doc, CONST T_TADNODE *body, INT *p_link,
		   T_TVFIG **p_out )
{
	FIGBUILD	b;
	T_TVFIG		*f;
	INT		i;

	f = (T_TVFIG *)Kmalloc(sizeof(T_TVFIG));
	if ( f == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < TV_MAX_OVER; i++ ) {
		f->over[i] = NULL;
		f->over_pages[i] = 0;
	}
	f->over_on = 0;
	f->view.left = f->view.top = 0;
	f->view.right = f->view.bottom = 0;
	f->draw = f->view;
	f->hunit = f->vunit = -72;
	f->nsh = 0;
	f->ndoc = 0;
	f->pool = NULL;
	f->npool = 0;
	f->zoom = 0;			/* life size, until whoever shows it says */
	f->pats = NULL;
	f->npats = 0;
	f->nlt = 0;
	f->paper_w = f->paper_h = 0;
	f->pmargin[0] = f->pmargin[1] = f->pmargin[2] = f->pmargin[3] = 0;
	/* the paper, and its margins, given ahead of the figure */
	{
		CONST T_TADNODE	*c;

		for ( c = ( doc != NULL ) ? tad_root(doc)->first : NULL;
		      c != NULL && c != body; c = c->next ) {
			if ( c->kind != TAD_ND_ELEM ) {
				continue;
			}
			if ( name_is(c, "paper") ) {
				f->paper_w = to_int(tad_attr(c, "width"), 0);
				f->paper_h = to_int(tad_attr(c, "length"), 0);
			} else if ( name_is(c, "docmargin") ) {
				f->pmargin[0] = to_int(tad_attr(c, "left"), 0);
				f->pmargin[1] = to_int(tad_attr(c, "top"), 0);
				f->pmargin[2] = to_int(tad_attr(c, "right"), 0);
				f->pmargin[3] = to_int(tad_attr(c, "bottom"), 0);
			}
		}
	}
	f->sh = (T_TVSHAPE *)Kmalloc(sizeof(T_TVSHAPE) * TV_MAX_SHAPE);
	f->doc = (T_TVDOC **)Kmalloc(sizeof(T_TVDOC *) * TV_MAX_DOC);
	if ( f->sh == NULL || f->doc == NULL ) {
		tv_fig_free(f);
		return E_NOMEM;
	}
	f->pool = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * TV_PT_POOL);
	f->npool = 0;
	if ( f->pool == NULL ) {
		tv_fig_free(f);
		return E_NOMEM;
	}
	b.f = f;
	b.link_no = *p_link;
	b.z_next = 1;
	b.ncp = 0;
	b.nmk = 0;
	b.place = -1;
	b.ts_on = FALSE;
	b.fm_arrow = -1;
	b.nmd = 0;
	b.cp = (RAWPAT *)Kmalloc(sizeof(RAWPAT) * CP_MAX);
	b.mk_id = (INT *)Kmalloc(sizeof(INT) * MK_MAX);
	b.mk_rows = (UH (*)[16])Kmalloc(sizeof(UH) * 16 * MK_MAX);
	if ( b.cp == NULL || b.mk_id == NULL || b.mk_rows == NULL ) {
		if ( b.cp != NULL ) Kfree(b.cp);
		if ( b.mk_id != NULL ) Kfree(b.mk_id);
		if ( b.mk_rows != NULL ) Kfree(b.mk_rows);
		tv_fig_free(f);
		return E_NOMEM;
	}
	/*
	 * Each element of the figure has a place, counting from nought,
	 * which is how a line joined to shapes names them. What only sets
	 * up the figure -- its view, its scale, its patterns -- has none.
	 */
	{
		CONST T_TADNODE	*c;
		INT		k = 0;

		for ( c = body->first; c != NULL; c = c->next ) {
			if ( c->kind != TAD_ND_ELEM ) {
				continue;
			}
			if ( name_is(c, "figView") || name_is(c, "figDraw")
			  || name_is(c, "figScale") || name_is(c, "transform")
			  || name_is(c, "figmodifier") || name_is(c, "patterns")
			  || name_is(c, "markerDefine") || name_is(c, "markerdefine")
			  || name_is(c, "lineTypeDefine") || name_is(c, "figoverlay") ) {
				b.place = -1;
				fig_walk(&b, c);
				continue;
			}
			b.place = k++;
			fig_walk(&b, c);
		}
	}
	fig_sort(f);
	/* every pattern of its own made, used or not, for whoever lists them */
	{
		INT	i;

		for ( i = 0; i < b.ncp; i++ ) {
			if ( !( b.cp[i].ncol == 1 && b.cp[i].mask[0] < 0 ) ) {
				(void)pat_make(&b, &b.cp[i]);
			}
		}
	}
	Kfree(b.cp);
	Kfree(b.mk_id);
	Kfree(b.mk_rows);
	*p_link = b.link_no;
	*p_out = f;

	return E_OK;
}

EXPORT void tv_fig_free( T_TVFIG *f )
{
	INT	i;

	if ( f == NULL ) {
		return;
	}
	if ( f->doc != NULL ) {
		for ( i = 0; i < f->ndoc; i++ ) {
			tv_doc_free(f->doc[i]);
		}
		Kfree(f->doc);
	}
	if ( f->sh != NULL ) {
		Kfree(f->sh);
	}
	if ( f->pool != NULL ) {
		Kfree(f->pool);
	}
	if ( f->pats != NULL ) {
		Kfree(f->pats);
	}
	for ( i = 0; i < TV_MAX_OVER; i++ ) {
		tv_fig_free(f->over[i]);
	}
	Kfree(f);
}
