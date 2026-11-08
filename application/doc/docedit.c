/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	docedit.c
 *	Editing the text of a document (design 17.12)
 *
 *	The same code edits a document and a piece of text in a figure, in
 *	a window of the desktop and in a process alike (include/ts/docedit.h).
 *	It works on a T_DDED; what belongs to whoever shows the text is asked
 *	through the table T_DDHOST.
 *
 *	A place in the text is a paragraph and a count into it: the bytes
 *	of its text, and one for each virtual object, picture or break
 *	standing in it (docview.c). The caret is one such place and the
 *	selection runs from another, the anchor, to it. Both are kept as
 *	numbers, not as pointers into the model, because the model is made
 *	again from the record after every change; a count into a paragraph
 *	means the same thing before and after.
 *
 *	Every change is made to the record's nodes (tad_edit.c), through
 *	the node each piece of the model was made from, and the model is
 *	made again afterwards -- the record is the one copy of the text.
 *
 *	Keys go to the converter of かな漢字 first (the host's ime_key);
 *	what it does not take is typed here.
 */

#include <ts/tvport.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/rx.h>
#include <ts/docmenu.h>
#include <ts/part.h>
#include <ts/look.h>
#include <ts/uuid.h>
#include <ts/tray.h>
#include <ts/figedit.h>
#include <ts/docedit.h>

#define KEY_A		0x04
#define KEY_B		0x05
#define KEY_C		0x06
#define KEY_F		0x09
#define KEY_I		0x0C
#define KEY_V		0x19
#define KEY_X		0x1B
#define KEY_Z		0x1D
#define KEY_9		0x26
#define KEY_ENTER	0x28
#define KEY_BS		0x2A
#define KEY_TAB		0x2B
#define KEY_U		0x18
#define KEY_HOME	0x4A
#define KEY_DEL		0x4C
#define KEY_END		0x4D
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52

#define MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define MOD_ALT		( HID_MOD_LALT | HID_MOD_RALT )


LOCAL T_DDED	*de_drag = NULL;	/* the window a selection is being drawn in */
LOCAL T_DDED	*sc_win = NULL;		/* the window whose selection is pressed, to be carried */

/* かな input, and the letters typed that do not yet spell a kana */

/* ---------------------------------------------------------------- places */

/* The record the text is edited in: in 原稿, the record's own text */
LOCAL T_TAD *rec_of( T_DDED *d )
{
	return ( (*d->xml_view) && (*d->xrec) != NULL ) ? (*d->xrec) : (T_TAD *)(*d->rec);
}

LOCAL INT run_units( CONST T_TVRUN *r )
{
	return ( r->kind == TV_RUN_TEXT ) ? r->len : 1;
}

/* How long a paragraph is, in places */
LOCAL INT para_len( CONST T_DDED *d, INT pi )
{
	CONST T_TVPARA	*p;
	INT		i, n = 0;

	if ( (*d->doc) == NULL || pi < 0 || pi >= (*d->doc)->npara ) {
		return 0;
	}
	p = &(*d->doc)->para[pi];
	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		n += run_units(&(*d->doc)->run[i]);
	}

	return n;
}

/* Whether place a is before place b */
LOCAL BOOL before_pl( INT pa, INT a, INT pb, INT b )
{
	return (BOOL)( pa < pb || ( pa == pb && a < b ) );
}

LOCAL BOOL has_sel( CONST T_DDED *d )
{
	return (BOOL)( d->cpara != d->apara || d->cpos != d->apos );
}

/* The selection in order: from (pa, a) to (pb, b) */
LOCAL void sel_order( CONST T_DDED *d, INT *pa, INT *a, INT *pb, INT *b )
{
	if ( before_pl(d->apara, d->apos, d->cpara, d->cpos) ) {
		*pa = d->apara;  *a = d->apos;
		*pb = d->cpara;  *b = d->cpos;
	} else {
		*pa = d->cpara;  *a = d->cpos;
		*pb = d->apara;  *b = d->apos;
	}
}

/* The caret kept inside the text there is */
LOCAL void clamp( T_DDED *d )
{
	INT	np = ( (*d->doc) != NULL ) ? (*d->doc)->npara : 0;

	if ( np == 0 ) {
		d->cpara = d->cpos = d->apara = d->apos = 0;
		return;
	}
	if ( d->cpara >= np ) d->cpara = np - 1;
	if ( d->cpara < 0 )   d->cpara = 0;
	if ( d->apara >= np ) d->apara = np - 1;
	if ( d->apara < 0 )   d->apara = 0;
	if ( d->cpos > para_len(d, d->cpara) ) d->cpos = para_len(d, d->cpara);
	if ( d->apos > para_len(d, d->apara) ) d->apos = para_len(d, d->apara);
	if ( d->cpos < 0 ) d->cpos = 0;
	if ( d->apos < 0 ) d->apos = 0;
}

/*
 * The run a place falls in, and how far into it. At the edge between
 * two runs, text is preferred to what is not, and the run before to
 * the run after: what is typed there takes on the look of the letters
 * before it. -1 when there is no text at the place.
 */
LOCAL INT text_at( CONST T_DDED *d, INT pi, INT pos, INT *p_off )
{
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i, acc = 0;

	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];
		INT		u = run_units(r);

		if ( r->kind == TV_RUN_TEXT && pos >= acc && pos <= acc + u ) {
			*p_off = pos - acc;
			return i;
		}
		acc += u;
	}

	return -1;
}

/* The run that begins at a place, whatever it is; -1 at the end */
LOCAL INT run_from( CONST T_DDED *d, INT pi, INT pos )
{
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i, acc = 0;

	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		if ( acc == pos ) {
			return i;
		}
		acc += run_units(&(*d->doc)->run[i]);
		if ( acc > pos ) {
			return -1;
		}
	}

	return -1;
}

/* The place one letter or thing before, or after, within the paragraph */
LOCAL INT step_back( CONST T_DDED *d, INT pi, INT pos )
{
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i, acc = 0;

	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];
		INT		u = run_units(r);

		if ( pos > acc && pos <= acc + u ) {
			if ( r->kind != TV_RUN_TEXT ) {
				return acc;
			}
			{
				INT	k = pos - acc - 1;

				while ( k > 0 && ( r->text[k] & 0xC0U ) == 0x80U ) {
					k--;
				}
				return acc + k;
			}
		}
		acc += u;
	}

	return ( pos > 0 ) ? pos - 1 : 0;
}

LOCAL INT step_fwd( CONST T_DDED *d, INT pi, INT pos )
{
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i, acc = 0;

	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];
		INT		u = run_units(r);

		if ( pos >= acc && pos < acc + u ) {
			if ( r->kind != TV_RUN_TEXT ) {
				return acc + 1;
			}
			{
				INT	k = pos - acc + 1;

				while ( k < u && ( r->text[k] & 0xC0U ) == 0x80U ) {
					k++;
				}
				return acc + k;
			}
		}
		acc += u;
	}

	return pos;
}

/* ---------------------------------------------------------------- changes */

/*
 * What was drawn in the window laid on the screen: all of its work area
 * is marked as changed, and only that is built again.
 */
LOCAL void shown( T_DDED *d )
{
	d->host->shown(d);
}

LOCAL INT caret_to( T_DDED *d );
LOCAL BOOL ink_of( T_DDED *d, CONST T_DPRECT *page, T_DPRECT *out );

/* The smallest rectangle holding both */
LOCAL void rect_join( T_DPRECT *a, CONST T_DPRECT *b )
{
	if ( b->right <= b->left || b->bottom <= b->top ) {
		return;
	}
	if ( a->right <= a->left || a->bottom <= a->top ) {
		*a = *b;
		return;
	}
	if ( b->left < a->left )     a->left = b->left;
	if ( b->top < a->top )       a->top = b->top;
	if ( b->right > a->right )   a->right = b->right;
	if ( b->bottom > a->bottom ) a->bottom = b->bottom;
}

/*
 * After a change, the part of the work area that has to be drawn again,
 * when it is not all of it: what the text differs by from what the page
 * showed (tv_doc_band), and the lines the caret and the selection were
 * on and are on now. FALSE: drawn whole -- the host cannot draw a part,
 * a piece of text in a figure, or the page is to be wound to the caret.
 */
LOCAL BOOL band_of( T_DDED *d, T_DPRECT *band )
{
	T_DPRECT	page, now;

	if ( d->host->draw_band == NULL || d->framed || (*d->doc) == NULL || !d->inked
	  || caret_to(d) != (*d->scroll_y) ) {
		return FALSE;
	}
	d->host->work_rect(d, &page);
	if ( !tv_doc_band((*d->doc), &page, (*d->scroll_y), (*d->rec), band)
	  || !ink_of(d, &page, &now) ) {
		return FALSE;
	}
	rect_join(band, &d->ink);
	rect_join(band, &now);

	return TRUE;
}

/*
 * The model made again, the window marked changed and drawn: only what
 * the change made differ, when that is known, else the whole page.
 */
LOCAL void changed( T_DDED *d )
{
	T_DPRECT	band;

	d->host->model(d);
	(*d->dirty) = TRUE;
	/* a piece of text in a figure: the figure shows it as it now is */
	d->host->changed(d);
	if ( (*d->xml_view) ) {
		d->xml_edited = TRUE;
	}
	clamp(d);
	if ( band_of(d, &band) ) {
		if ( band.right > band.left && band.bottom > band.top ) {
			d->host->draw_band(d, &band);
		}
		return;
	}
	d->host->draw(d);
	de_show_caret(d);
	shown(d);
}

/* One step to take back, unless this is more of the same typing */
LOCAL void undo_point( T_DDED *d, BOOL typing )
{
	/* 原稿 is read back into the record as one change, on leaving it */
	if ( !( typing && d->typing ) && !(*d->xml_view) ) {
		/* a piece of text in a figure is a change to the figure */
		d->host->before(d);
	}
	d->typing = typing;
}

/*
 * Words put in at a place, with no line breaks in them. Where there is
 * text at the place they go into it; where there is none -- an empty
 * paragraph, or between two things that are not text -- a text node is
 * made for them.
 */
LOCAL BOOL same_word( CONST UB *a, CONST char *b );

LOCAL BOOL put_words( T_DDED *d, INT pi, INT pos, CONST UB *s, INT n )
{
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		ri, off = 0;

	if ( n <= 0 ) {
		return TRUE;
	}
	ri = text_at(d, pi, pos, &off);
	if ( ri >= 0 ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[ri];

		if ( ( r->style & TV_ST_COMB ) != 0 && ( off == 0 || off == r->len ) ) {
			/*
			 * 縦中横 (<combchar>) is not typed into: letters at
			 * either end of it go beside it, outside it.
			 */
			T_TADNODE	*e = r->node;

			while ( e != NULL && !( e->kind == TAD_ND_ELEM
					      && same_word(e->name, "combchar") ) ) {
				e = e->parent;
			}
			if ( e != NULL && e->parent != NULL ) {
				return (BOOL)( tad_text_new(rec, e->parent,
							    ( off == 0 ) ? e : e->next, s, n) != NULL );
			}
		}
		return (BOOL)( tad_text_splice(rec, r->node, r->noff + off, 0,
					       s, n) >= E_OK );
	}
	ri = run_from(d, pi, pos);
	if ( ri >= 0 ) {
		T_TADNODE	*at = (*d->doc)->run[ri].node;

		return (BOOL)( tad_text_new(rec, at->parent, at, s, n) != NULL );
	}
	if ( p->n > 0 ) {
		/* at the end, after something that is not text */
		T_TADNODE	*last = (*d->doc)->run[p->first + p->n - 1].node;

		return (BOOL)( tad_text_new(rec, last->parent, last->next, s, n)
			       != NULL );
	}
	if ( p->node == NULL ) {
		return FALSE;
	}

	return (BOOL)( tad_text_new(rec, p->node, NULL, s, n) != NULL );
}

/* A run that is a link marked 固定化 or 背景化: it is not thrown away, carried or sized */
LOCAL BOOL run_protected( T_DDED *d, CONST T_TVRUN *r )
{
	T_VOBJ	*v;
	BOOL	yes = FALSE;

	if ( r->kind != TV_RUN_LINK || r->link < 0 ) {
		return FALSE;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v != NULL ) {
		yes = (BOOL)( tad_lnk_get((*d->rec), r->link, v) >= E_OK && ( v->fixed || v->background ) );
		Kfree(v);
	}
	return yes;
}

/* Whether a paragraph holds such a link */
LOCAL BOOL para_protected( T_DDED *d, INT pi )
{
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i;

	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		if ( run_protected(d, &(*d->doc)->run[i]) ) {
			return TRUE;
		}
	}
	return FALSE;
}

/* The stretch [a, b) of one paragraph taken out, protected links left in */
LOCAL void cut_within( T_DDED *d, INT pi, INT a, INT b )
{
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		i, acc, k;

	if ( b <= a ) {
		return;
	}
	/* where the runs end, then the runs from the last back */
	acc = 0;
	for ( k = 0; k < p->n && p->first + k < (*d->doc)->nrun; k++ ) {
		acc += run_units(&(*d->doc)->run[p->first + k]);
	}
	for ( i = k - 1; i >= 0; i-- ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[p->first + i];
		INT		hi = acc, lo = acc - run_units(r);
		INT		ca = ( a > lo ) ? a : lo;
		INT		cb = ( b < hi ) ? b : hi;

		acc = lo;
		if ( cb <= ca ) {
			continue;
		}
		if ( r->kind == TV_RUN_TEXT ) {
			(void)tad_text_splice(rec, r->node, r->noff + ( ca - lo ),
					      cb - ca, NULL, 0);
		} else if ( !run_protected(d, r) ) {
			tad_node_remove(r->node);
		}
	}
}

/*
 * Paragraph 'pi + 1' put onto the end of paragraph 'pi'. A paragraph
 * the record does not write as a <p> cannot be joined.
 */
LOCAL BOOL join_after( T_DDED *d, INT pi )
{
	T_TADNODE	*a, *b;

	if ( pi < 0 || pi + 1 >= (*d->doc)->npara ) {
		return FALSE;
	}
	a = (*d->doc)->para[pi].node;
	b = (*d->doc)->para[pi + 1].node;
	if ( a == NULL || b == NULL ) {
		return FALSE;
	}
	tad_join(a, b);

	return TRUE;
}

/* The selection taken out; the caret where it began */
LOCAL void cut_sel( T_DDED *d )
{
	INT	pa, a, pb, b, i;

	sel_order(d, &pa, &a, &pb, &b);
	if ( pa == pb ) {
		cut_within(d, pa, a, b);
	} else {
		cut_within(d, pb, 0, b);
		for ( i = pb - 1; i > pa; i-- ) {
			if ( (*d->doc)->para[i].node == NULL ) {
				continue;
			}
			if ( para_protected(d, i) ) {
				/* its words go; the protected links stay, in it */
				cut_within(d, i, 0, para_len(d, i));
				continue;
			}
			tad_node_remove((*d->doc)->para[i].node);
		}
		cut_within(d, pa, a, para_len(d, pa));
		if ( (*d->doc)->para[pa].node != NULL
		  && (*d->doc)->para[pb].node != NULL ) {
			tad_join((*d->doc)->para[pa].node, (*d->doc)->para[pb].node);
		}
	}
	d->cpara = d->apara = pa;
	d->cpos = d->apos = a;
	d->host->model(d);
}

/*
 * The paragraph cut in two at a place: everything after it, in copies
 * of the elements it sits in, goes into a new paragraph after this one.
 */
LOCAL BOOL split_at( T_DDED *d, INT pi, INT pos )
{
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	T_TADNODE	*from = NULL;
	INT		ri, off = 0;

	if ( p->node == NULL ) {
		return FALSE;
	}
	ri = text_at(d, pi, pos, &off);
	if ( ri >= 0 ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[ri];

		from = tad_text_split(rec, r->node, r->noff + off);
	} else if ( ( ri = run_from(d, pi, pos) ) >= 0 ) {
		from = (*d->doc)->run[ri].node;
	} else if ( p->n > 0 ) {
		T_TADNODE	*last = (*d->doc)->run[p->first + p->n - 1].node;

		from = tad_text_new(rec, last->parent, last->next, NULL, 0);
	} else {
		from = tad_text_new(rec, p->node, NULL, NULL, 0);
	}
	if ( from == NULL ) {
		return FALSE;
	}

	return (BOOL)( tad_split_before(rec, from, p->node) != NULL );
}

/* Words put in at the caret, a new paragraph for each line break */
LOCAL void insert( T_DDED *d, CONST UB *s, INT n, BOOL typing )
{
	INT	i, from = 0;

	if ( (*d->doc) == NULL || (*d->doc)->npara == 0 ) {
		return;
	}
	undo_point(d, typing);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	for ( i = 0; i <= n; i++ ) {
		if ( i < n && s[i] != '\n' ) {
			continue;
		}
		if ( i > from ) {
			INT	k = i - from;

			/* a line ending with a carriage return loses it */
			if ( s[i - 1] == '\r' ) {
				k--;
			}
			if ( put_words(d, d->cpara, d->cpos, s + from, k) ) {
				d->cpos += k;
			}
			/* the model made again only for what follows; changed() makes it at the end */
			if ( i < n ) {
				d->host->model(d);
			}
		}
		if ( i < n ) {
			if ( split_at(d, d->cpara, d->cpos) ) {
				d->cpara++;
				d->cpos = 0;
			}
			if ( i + 1 < n ) {
				d->host->model(d);
			}
		}
		from = i + 1;
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* Text typed into a window, as the keys would put it (the TIP's commits) */
EXPORT void de_type_text( T_DDED *d, CONST UB *s, INT n )
{
	if ( de_editable(d) && n > 0 ) {
		insert(d, s, n, TRUE);
	}
}

/* Backspace: the selection, or what is before the caret */
LOCAL void erase_back( T_DDED *d )
{
	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	} else if ( d->cpos == 0 ) {
		INT	was;

		if ( d->cpara == 0 ) {
			return;
		}
		was = para_len(d, d->cpara - 1);
		if ( join_after(d, d->cpara - 1) ) {
			d->cpara--;
			d->cpos = was;
		}
	} else {
		INT	b = step_back(d, d->cpara, d->cpos);

		cut_within(d, d->cpara, b, d->cpos);
		d->cpos = b;
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* Delete: the selection, or what is after the caret */
LOCAL void erase_fwd( T_DDED *d )
{
	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	} else if ( d->cpos >= para_len(d, d->cpara) ) {
		(void)join_after(d, d->cpara);
	} else {
		cut_within(d, d->cpara, d->cpos,
			   step_fwd(d, d->cpara, d->cpos));
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* ---------------------------------------------------------------- the selection as words */

/*
 * The selection as words: a line break between paragraphs and for a
 * break. out NULL: only how many bytes there are, every one of them.
 */
LOCAL INT sel_text( T_DDED *d, UB *out, INT max )
{
	INT	pa, a, pb, b, pi, n = 0;

	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[pi];
		INT		lo = ( pi == pa ) ? a : 0;
		INT		hi = ( pi == pb ) ? b : 0x7FFFFFFF;
		INT		i, acc = 0;

		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[i];
			INT		u = run_units(r), k;

			for ( k = 0; k < u; k++ ) {
				if ( acc + k < lo || acc + k >= hi || ( out != NULL && n >= max - 1 ) ) {
					continue;
				}
				if ( r->kind == TV_RUN_TEXT ) {
					if ( out != NULL ) out[n] = r->text[k];
					n++;
				} else if ( r->kind == TV_RUN_BREAK ) {
					if ( out != NULL ) out[n] = '\n';
					n++;
				}
			}
			acc += u;
		}
		if ( pi < pb && ( out == NULL || n < max - 1 ) ) {
			if ( out != NULL ) out[n] = '\n';
			n++;
		}
	}
	if ( out != NULL ) out[n] = 0;

	return n;
}

/* ---------------------------------------------------------------- 検索/置換 */

LOCAL UB	de_what[WM_LABEL_MAX];
LOCAL UB	de_with[WM_LABEL_MAX];
LOCAL BOOL	de_regex = FALSE;	/* 正規表現: what is looked for is a pattern */
LOCAL T_RX	*de_rx = NULL;		/* that pattern, read */

/*
 * The last match found: where it is, and, for a pattern, the text of
 * its paragraph and where its groups are, for the words that replace it.
 */
LOCAL INT	de_m_para = -1, de_m_pos, de_m_len;
LOCAL UB	*de_m_text = NULL;
LOCAL INT	de_m_groups[RX_GROUPS * 2];

#define WITH_MAX	( WM_LABEL_MAX * 4 )

LOCAL UB fold( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c + 'a' - 'A' ) : c;
}

/*
 * A paragraph's text as one string for looking through: a thing that is
 * not text stands as a new line, which nothing looked for can match.
 */
LOCAL UB *para_words( T_DDED *d, INT p, INT *p_n )
{
	CONST T_TVPARA	*para = &(*d->doc)->para[p];
	INT		n = para_len(d, p), k = 0, i;
	UB		*buf = (UB *)Kmalloc((SZ)n + 1);

	if ( buf == NULL ) {
		return NULL;
	}
	for ( i = para->first; i < para->first + para->n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];
		INT		j;

		if ( r->kind == TV_RUN_TEXT ) {
			for ( j = 0; j < r->len; j++ ) {
				buf[k++] = r->text[j];
			}
		} else {
			buf[k++] = '\n';
		}
	}
	buf[k] = 0;
	*p_n = k;

	return buf;
}

/* In a paragraph's words from 'from': where the words or the pattern are */
LOCAL BOOL find_in( CONST UB *buf, INT n, INT from, CONST UB *what, INT wl,
		    INT *p_at, INT *p_len, INT *groups )
{
	INT	i;

	if ( de_regex ) {
		INT	a, b;

		/* a match of nothing is passed over: it could not be selected */
		for ( i = from; i <= n; i = a + 1 ) {
			if ( !rx_find(de_rx, buf, n, i, &a, &b, groups) ) {
				return FALSE;
			}
			if ( b > a ) {
				*p_at = a;
				*p_len = b - a;
				return TRUE;
			}
		}
		return FALSE;
	}
	for ( i = from; i + wl <= n; i++ ) {
		INT	j;

		for ( j = 0; j < wl && fold(buf[i + j]) == fold(what[j]); j++ ) {
			;
		}
		if ( j == wl ) {
			*p_at = i;
			*p_len = wl;
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * The first place at or after (pi, pos) where the words are, in order
 * through the paragraphs and round to the start again. A thing that is
 * not text stands in the way of a match. Letters match either way up;
 * a pattern matches them as it writes them. The match is kept, with
 * its paragraph's words, for what replaces it.
 */
LOCAL BOOL find_next( T_DDED *d, CONST UB *what, INT pi, INT pos,
		      INT *p_pi, INT *p_pos, INT *p_len )
{
	UB	*buf;
	INT	n = 0, wl = 0, round, np = (*d->doc)->npara;

	while ( what[wl] != 0 ) {
		wl++;
	}
	if ( wl == 0 || np == 0 || ( de_regex && de_rx == NULL ) ) {
		return FALSE;
	}
	for ( round = 0; round <= np; round++ ) {
		INT	p = ( pi + round ) % np, at, len;

		buf = para_words(d, p, &n);
		if ( buf == NULL ) {
			return FALSE;
		}
		if ( find_in(buf, n, ( round == 0 ) ? pos : 0, what, wl, &at, &len,
			     de_m_groups) ) {
			if ( de_m_text != NULL ) {
				Kfree(de_m_text);
			}
			de_m_text = buf;
			de_m_para = *p_pi = p;
			de_m_pos = *p_pos = at;
			de_m_len = *p_len = len;
			return TRUE;
		}
		Kfree(buf);
	}

	return FALSE;
}

/* The words to put in place of the last match */
LOCAL INT with_words( UB *out, INT max )
{
	INT	i;

	if ( de_regex ) {
		return rx_expand(de_with, de_m_text, de_m_groups, out, max);
	}
	for ( i = 0; de_with[i] != 0 && i < max - 1; i++ ) {
		out[i] = de_with[i];
	}
	out[i] = 0;

	return i;
}

/* The words, found and selected after the caret; FALSE when there are none */
LOCAL BOOL find_select( T_DDED *d, BOOL from_sel_end )
{
	INT	pa, a, pb, b, fp, fpos, len;

	sel_order(d, &pa, &a, &pb, &b);
	if ( !from_sel_end ) {
		pb = pa;
		b = a;
		if ( has_sel(d) ) {
			b++;			/* not the one already chosen */
		}
	}
	if ( !find_next(d, de_what, pb, b, &fp, &fpos, &len) ) {
		return FALSE;
	}
	d->apara = d->cpara = fp;
	d->apos = fpos;
	d->cpos = fpos + len;
	d->typing = FALSE;
	clamp(d);
	d->host->draw(d);
	de_show_caret(d);
	shown(d);

	return TRUE;
}

/* Whether what is selected is the words looked for, or the last match */
LOCAL BOOL sel_is_what( T_DDED *d )
{
	UB	got[WM_LABEL_MAX];
	INT	n, i, pa, a, pb, b;

	if ( !has_sel(d) ) {
		return FALSE;
	}
	if ( de_regex ) {
		sel_order(d, &pa, &a, &pb, &b);
		return (BOOL)( de_m_text != NULL && pa == pb && pa == de_m_para
			    && a == de_m_pos && b == de_m_pos + de_m_len );
	}
	n = sel_text(d, got, (INT)sizeof(got));
	for ( i = 0; i < n && de_what[i] != 0 && fold(got[i]) == fold(de_what[i]); i++ ) {
		;
	}

	return (BOOL)( i == n && de_what[i] == 0 );
}

/*
 * 検索/置換: the panel asked, and what was pressed done. 置換 puts the
 * words in place of the selection when that is what was looked for,
 * then looks for the next; 全置換 does that from the start to the end,
 * as one change that is taken back as one. With 正規表現 what is looked
 * for is a pattern, and $& $1 .. $9 in the words put in its place are
 * what it matched.
 */
LOCAL void find_replace( T_DDED *d )
{
	INT	how = d->host->find_form(d, de_what, de_with, WM_LABEL_MAX, &de_regex);
	INT	count = 0, guard;
	UB	with[WITH_MAX];

	if ( how == 0 || de_what[0] == 0 ) {
		return;
	}
	rx_free(de_rx);
	de_rx = NULL;
	de_m_para = -1;
	if ( de_regex && rx_compile(de_what, &de_rx) < E_OK ) {
		d->host->tell(d, "正規表現が正しくありません。", (CONST char *)de_what);
		return;
	}
	switch ( how ) {
	case DE_FIND_NEXT:
		if ( !find_select(d, FALSE) ) {
			d->host->tell(d, "見つかりません。", NULL);
		}
		break;
	case DE_FIND_REPLACE:
		if ( sel_is_what(d) ) {
			insert(d, with, with_words(with, WITH_MAX), FALSE);
		}
		if ( !find_select(d, TRUE) ) {
			d->host->tell(d, "見つかりません。", NULL);
		}
		break;
	default:
		undo_point(d, FALSE);
		d->cpara = d->apara = 0;
		d->cpos = d->apos = 0;
		for ( guard = 0; guard < 10000; guard++ ) {
			INT	fp, fpos, len;

			if ( !find_next(d, de_what, d->cpara, d->cpos, &fp, &fpos, &len)
			  || before_pl(fp, fpos, d->cpara, d->cpos) ) {
				break;		/* round the end: all done */
			}
			d->apara = d->cpara = fp;
			d->apos = fpos;
			d->cpos = fpos + len;
			d->typing = TRUE;	/* the one step taken above */
			insert(d, with, with_words(with, WITH_MAX), TRUE);
			count++;
		}
		d->typing = FALSE;
		{
			UB	msg[64];
			INT	k = 0, v = count, j;
			char	num[12];

			do {
				num[k++] = (char)( '0' + v % 10 );
				v /= 10;
			} while ( v > 0 && k < 11 );
			for ( j = 0; j < k; j++ ) {
				msg[j] = (UB)num[k - 1 - j];
			}
			msg[k] = 0;
			{
				CONST char	*t = "件置換しました。";
				INT		m = 0;

				while ( t[m] != 0 && k < 63 ) {
					msg[k++] = (UB)t[m++];
				}
				msg[k] = 0;
			}
			d->host->tell(d, (CONST char *)msg, NULL);
		}
		break;
	}
}

/* ---------------------------------------------------------------- the look of letters */

/*
 * The names the elements that change the look of letters go by. The
 * first of each list is the one written; the others are read, and are
 * taken away with it.
 */
LOCAL CONST char *CONST nm_bold[]    = { "bold", "strong", "b", NULL };
LOCAL CONST char *CONST nm_italic[]  = { "italic", "i", "em", NULL };
LOCAL CONST char *CONST nm_bag[]     = { "bagchar", NULL };
LOCAL CONST char *CONST nm_box[]     = { "box", NULL };
LOCAL CONST char *CONST nm_shadow[]  = { "shadow", NULL };
LOCAL CONST char *CONST nm_under[]   = { "underline", "u", NULL };
LOCAL CONST char *CONST nm_over[]    = { "overline", NULL };
LOCAL CONST char *CONST nm_strike[]  = { "strikethrough", "strike", "s", NULL };
LOCAL CONST char *CONST nm_mesh[]    = { "mesh", NULL };
LOCAL CONST char *CONST nm_invert[]  = { "invert", NULL };
LOCAL CONST char *CONST nm_noprint[] = { "noprint", NULL };
LOCAL CONST char *CONST nm_attend[]  = { "attend", "sup", "sub", NULL };
LOCAL CONST char *CONST nm_ruby[]    = { "ruby", NULL };

/* 解除: every decoration; 標準: those and the face, size and colour too */
LOCAL CONST char *CONST nm_deco[] = {
	"bold", "strong", "b", "italic", "i", "em", "bagchar", "box",
	"shadow", "underline", "u", "overline", "strikethrough", "strike",
	"s", "mesh", "invert", "noprint", "attend", "sup", "sub", NULL
};
LOCAL CONST char *CONST nm_plain[] = {
	"bold", "strong", "b", "italic", "i", "em", "bagchar", "box",
	"shadow", "underline", "u", "overline", "strikethrough", "strike",
	"s", "mesh", "invert", "noprint", "attend", "sup", "sub", "font", NULL
};

LOCAL BOOL same_word( CONST UB *a, CONST char *b )
{
	INT	i;

	if ( a == NULL ) {
		return FALSE;
	}
	for ( i = 0; b[i] != 0 && a[i] == (UB)b[i]; i++ ) {
		;
	}

	return (BOOL)( b[i] == 0 && a[i] == 0 );
}

/*
 * An attribute the elements looked for must also have, while it is set:
 * <font rotation> is 回転, and taking 回転 off takes away only those
 * <font> elements that turn the letters.
 */
LOCAL CONST char	*de_need_attr = NULL;

/* Whether a node is an element with one of the names */
LOCAL BOOL named( CONST T_TADNODE *nd, CONST char *CONST *names )
{
	INT	i;

	if ( nd == NULL || nd->kind != TAD_ND_ELEM ) {
		return FALSE;
	}
	if ( de_need_attr != NULL && tad_attr(nd, de_need_attr) == NULL ) {
		return FALSE;
	}
	for ( i = 0; names[i] != NULL; i++ ) {
		if ( same_word(nd->name, names[i]) ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* Whether element e holds node n, at any depth */
LOCAL BOOL holds( CONST T_TADNODE *e, CONST T_TADNODE *n )
{
	for ( ; n != NULL; n = n->parent ) {
		if ( n == e ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* The nearest element over a node with one of the names, below 'stop' */
LOCAL T_TADNODE *named_above( T_TADNODE *n, CONST T_TADNODE *stop,
			      CONST char *CONST *names )
{
	for ( n = ( n != NULL ) ? n->parent : NULL; n != NULL && n != stop;
	      n = n->parent ) {
		if ( named(n, names) ) {
			return n;
		}
	}

	return NULL;
}

/* What a paragraph's things sit in: its <p>, or the body for one implied */
LOCAL T_TADNODE *para_home( T_DDED *d, INT pi )
{
	T_TADNODE	*pn = (*d->doc)->para[pi].node;

	return ( pn != NULL ) ? pn : tad_body(rec_of(d));
}

/* The text cut where the stretch [a, b) of a paragraph begins and ends */
LOCAL void cut_ends( T_DDED *d, INT pi, INT a, INT b )
{
	T_TAD	*rec = rec_of(d);
	INT	off = 0, ri;

	ri = text_at(d, pi, b, &off);
	if ( ri >= 0 && off > 0 && off < (*d->doc)->run[ri].len ) {
		(void)tad_text_split(rec, (*d->doc)->run[ri].node,
				     (*d->doc)->run[ri].noff + off);
		d->host->model(d);
	}
	ri = text_at(d, pi, a, &off);
	if ( ri >= 0 && off > 0 && off < (*d->doc)->run[ri].len ) {
		(void)tad_text_split(rec, (*d->doc)->run[ri].node,
				     (*d->doc)->run[ri].noff + off);
		d->host->model(d);
	}
}

/*
 * The stretch [a, b) of one paragraph put inside an element: the text
 * at each end cut where the stretch ends, and the runs between wrapped
 * -- one wrapping for each stretch of them that has the same parent,
 * since an element can only hold siblings. 'attrs' is the element's
 * attributes, a name and a value each, ending with NULL.
 */
LOCAL void wrap_para( T_DDED *d, INT pi, INT a, INT b, CONST char *name,
		      CONST char *CONST *attrs )
{
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p;
	T_TADNODE	**nodes, *w;
	INT		i, acc, k = 0, j, t;

	if ( b <= a ) {
		return;
	}
	cut_ends(d, pi, a, b);
	/* the runs wholly inside [a, b): no more nodes than the paragraph has runs */
	p = &(*d->doc)->para[pi];
	nodes = (T_TADNODE **)Kmalloc(sizeof(T_TADNODE *) * (SZ)( p->n + 1 ));
	if ( nodes == NULL ) {
		d->host->no_memory(d);
		return;
	}
	acc = 0;
	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];
		INT		u = run_units(r);

		if ( acc >= a && acc + u <= b && u > 0
		  && ( k == 0 || nodes[k - 1] != r->node ) ) {
			nodes[k++] = r->node;
		}
		acc += u;
	}
	/* each stretch of siblings, wrapped */
	i = 0;
	while ( i < k ) {
		j = i;
		while ( j + 1 < k && nodes[j + 1]->parent == nodes[i]->parent
		     && nodes[j]->next == nodes[j + 1] ) {
			j++;
		}
		w = tad_wrap(rec, nodes[i], nodes[j], name);
		for ( t = 0; w != NULL && attrs != NULL && attrs[t] != NULL;
		      t += 2 ) {
			(void)tad_set_attr(rec, w, attrs[t],
					   (CONST UB *)attrs[t + 1]);
		}
		i = j + 1;
	}
	Kfree(nodes);
	d->host->model(d);
}

/* The selection put inside an element, every paragraph of it */
LOCAL void wrap_range( T_DDED *d, CONST char *name, CONST char *CONST *attrs )
{
	INT	pa, a, pb, b, pi;

	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		wrap_para(d, pi, ( pi == pa ) ? a : 0,
			  ( pi == pb ) ? b : para_len(d, pi), name, attrs);
	}
}

LOCAL void wrap_sel( T_DDED *d, CONST char *name, CONST char *CONST *attrs )
{
	if ( !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	wrap_range(d, name, attrs);
	changed(d);
}

/*
 * The elements of those names taken off the stretch [a, b) of one
 * paragraph, and off nothing else. An element that holds more than
 * the stretch is first cut in two where the stretch begins or ends --
 * the part outside keeps it -- until the one over the stretch holds
 * the stretch alone; then it is taken away and what it held stays
 * where it was.
 */
LOCAL void unwrap_para( T_DDED *d, INT pi, INT a, INT b,
			CONST char *CONST *names )
{
	T_TAD	*rec = rec_of(d);
	INT	guard;

	if ( b <= a ) {
		return;
	}
	cut_ends(d, pi, a, b);
	for ( guard = 0; guard < 256; guard++ ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[pi];
		T_TADNODE	*stop = para_home(d, pi);
		T_TADNODE	*e = NULL, *first = NULL, *after = NULL;
		BOOL		before = FALSE;
		INT		i, acc = 0;

		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[i];
			INT		u = run_units(r);

			if ( acc >= a && acc + u <= b ) {
				e = named_above(r->node, stop, names);
				if ( e != NULL ) {
					break;
				}
			}
			acc += u;
		}
		if ( e == NULL ) {
			break;
		}
		acc = 0;
		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[i];
			INT		u = run_units(r);

			if ( holds(e, r->node) ) {
				if ( acc < a ) {
					before = TRUE;
				} else if ( acc >= b ) {
					if ( after == NULL ) {
						after = r->node;
					}
				} else if ( first == NULL ) {
					first = r->node;
				}
			}
			acc += u;
		}
		if ( before && first != NULL && first != e ) {
			(void)tad_split_before(rec, first, e);
		} else if ( after != NULL && after != e ) {
			(void)tad_split_before(rec, after, e);
		} else {
			while ( e->first != NULL ) {
				tad_node_move(e->first, e->parent, e);
			}
			tad_node_remove(e);
		}
		d->host->model(d);
	}
}

LOCAL void unwrap_range( T_DDED *d, CONST char *CONST *names )
{
	INT	pa, a, pb, b, pi;

	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		unwrap_para(d, pi, ( pi == pa ) ? a : 0,
			    ( pi == pb ) ? b : para_len(d, pi), names);
	}
}

LOCAL void unwrap_sel( T_DDED *d, CONST char *CONST *names )
{
	if ( !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	unwrap_range(d, names);
	changed(d);
}

/* Whether every letter picked already has a look */
LOCAL BOOL sel_all( T_DDED *d, UINT bit )
{
	INT	pa, a, pb, b, pi, i, acc;
	BOOL	any = FALSE;

	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[pi];
		INT		lo = ( pi == pa ) ? a : 0;
		INT		hi = ( pi == pb ) ? b : para_len(d, pi);

		acc = 0;
		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[i];
			INT		u = run_units(r);

			if ( r->kind == TV_RUN_TEXT && u > 0 && acc < hi
			  && acc + u > lo ) {
				any = TRUE;
				if ( ( r->style & bit ) == 0 ) {
					return FALSE;
				}
			}
			acc += u;
		}
	}

	return any;
}

/*
 * A decoration put on the letters picked, or, when every one of them
 * has it already, taken off: the menu row works both ways.
 */
LOCAL void style_sel( T_DDED *d, CONST char *CONST *names, UINT bit )
{
	if ( !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	if ( sel_all(d, bit) ) {
		unwrap_range(d, names);
	} else {
		wrap_range(d, names[0], NULL);
	}
	changed(d);
}

/* 上付き, 下付き: one or the other, and again to take it off */
LOCAL void attend_sel( T_DDED *d, BOOL above )
{
	LOCAL CONST char *CONST up[] = {
		"type", "1", "position", "0", "unit", "0",
		"targetPosition", "0", "baseline", "0", NULL
	};
	LOCAL CONST char *CONST down[] = {
		"type", "0", "position", "0", "unit", "0",
		"targetPosition", "0", "baseline", "0", NULL
	};
	BOOL	had;

	if ( !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	had = sel_all(d, above ? TV_ST_SUP : TV_ST_SUB);
	unwrap_range(d, nm_attend);
	if ( !had ) {
		wrap_range(d, "attend", above ? up : down);
	}
	changed(d);
}

/*
 * The faces used last, newest first: the 書体 menu offers them again.
 * A face used again moves to the front.
 */
#define RECENT_MAX	10

LOCAL UB	de_recent[RECENT_MAX][WM_LABEL_MAX];
LOCAL INT	de_nrecent = 0;

LOCAL void face_used( CONST UB *face )
{
	INT	i, k, at = de_nrecent;

	for ( i = 0; i < de_nrecent; i++ ) {
		if ( same_word(de_recent[i], (CONST char *)face) ) {
			at = i;
			break;
		}
	}
	if ( at == de_nrecent ) {
		if ( de_nrecent < RECENT_MAX ) {
			de_nrecent++;
		}
		at = de_nrecent - 1;
	}
	for ( i = at; i > 0; i-- ) {
		for ( k = 0; k < WM_LABEL_MAX; k++ ) {
			de_recent[i][k] = de_recent[i - 1][k];
		}
	}
	for ( k = 0; face[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) {
		de_recent[0][k] = face[k];
	}
	de_recent[0][k] = 0;
}

EXPORT INT de_recent_faces( UB (*out)[WM_LABEL_MAX], INT max )
{
	INT	i, k;

	for ( i = 0; i < de_nrecent && i < max; i++ ) {
		for ( k = 0; k < WM_LABEL_MAX; k++ ) {
			out[i][k] = de_recent[i][k];
		}
	}

	return i;
}

/* One attribute of <font> for the letters picked */
LOCAL void font_sel( T_DDED *d, CONST char *attr, CONST UB *value )
{
	CONST char	*attrs[3];

	if ( same_word((CONST UB *)attr, "face") && has_sel(d) ) {
		face_used(value);
	}
	attrs[0] = attr;
	attrs[1] = (CONST char *)value;
	attrs[2] = NULL;
	wrap_sel(d, "font", attrs);
}

/*
 * A look of the letters picked set on or off, whatever they had: what
 * a check box asks for, where a menu row turns it over.
 */
EXPORT void de_look( T_DDED *d, INT cmd, BOOL on )
{
	CONST char *CONST	*names;
	UINT			bit;

	switch ( cmd ) {
	case DM_STYLE_BOLD:	 names = nm_bold;   bit = TV_ST_BOLD;	break;
	case DM_STYLE_ITALIC:	 names = nm_italic; bit = TV_ST_ITALIC;	break;
	case DM_STYLE_UNDERLINE: names = nm_under;  bit = TV_ST_UNDER;	break;
	case DM_STYLE_STRIKE:	 names = nm_strike; bit = TV_ST_STRIKE;	break;
	default:		 return;
	}
	if ( !de_editable(d) || !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	if ( on && !sel_all(d, bit) ) {
		wrap_range(d, names[0], NULL);
	} else if ( !on ) {
		unwrap_range(d, names);
	}
	changed(d);
}

/* One attribute of <font> for the letters picked, from outside */
EXPORT void de_font( T_DDED *d, CONST char *attr, CONST UB *value )
{
	if ( de_editable(d) ) {
		font_sel(d, attr, value);
	}
}

/* Whether anything is picked */
EXPORT BOOL de_has_sel( T_DDED *d )
{
	return (BOOL)( de_editable(d) && has_sel(d) );
}

/* Everything picked */
EXPORT void de_pick_all( T_DDED *d )
{
	if ( !de_editable(d) ) {
		return;
	}
	d->apara = 0;
	d->apos = 0;
	d->cpara = (*d->doc)->npara - 1;
	d->cpos = para_len(d, d->cpara);
}

/* ---------------------------------------------------------------- things put in */

/* The length of a text node's text */
LOCAL INT node_len( CONST T_TADNODE *nd )
{
	INT	n = 0;

	while ( nd->text != NULL && nd->text[n] != 0 ) {
		n++;
	}

	return n;
}

/* An empty element put in at a place, the text there cut in two for it */
LOCAL T_TADNODE *put_elem( T_DDED *d, INT pi, INT pos, CONST char *name )
{
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p = &(*d->doc)->para[pi];
	INT		ri, off = 0;

	ri = text_at(d, pi, pos, &off);
	if ( ri >= 0 ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[ri];
		INT		at = r->noff + off;
		T_TADNODE	*second;

		if ( at <= 0 ) {
			return tad_elem_new(rec, name, r->node->parent, r->node,
					    TRUE);
		}
		if ( at >= node_len(r->node) ) {
			return tad_elem_new(rec, name, r->node->parent,
					    r->node->next, TRUE);
		}
		second = tad_text_split(rec, r->node, at);

		return ( second != NULL )
		       ? tad_elem_new(rec, name, second->parent, second, TRUE)
		       : NULL;
	}
	ri = run_from(d, pi, pos);
	if ( ri >= 0 ) {
		T_TADNODE	*at = (*d->doc)->run[ri].node;

		return tad_elem_new(rec, name, at->parent, at, TRUE);
	}
	if ( p->n > 0 ) {
		T_TADNODE	*last = (*d->doc)->run[p->first + p->n - 1].node;

		return tad_elem_new(rec, name, last->parent, last->next, TRUE);
	}
	if ( p->node == NULL ) {
		return NULL;
	}

	return tad_elem_new(rec, name, p->node, NULL, TRUE);
}

/* A thing that is not text put in at the caret, in place of the selection */
LOCAL void put_thing( T_DDED *d, CONST char *name )
{
	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	if ( put_elem(d, d->cpara, d->cpos, name) != NULL ) {
		d->cpos++;
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/*
 * 字下げ: where the lines after the first of the paragraph start. A
 * paragraph has one; putting it somewhere else moves it.
 */
LOCAL void indent_here( T_DDED *d )
{
	CONST T_TVPARA	*p;
	INT		i, acc = 0;

	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	p = &(*d->doc)->para[d->cpara];
	for ( i = p->first + p->n - 1; i >= p->first; i-- ) {
		if ( (*d->doc)->run[i].kind == TV_RUN_INDENT ) {
			INT	k, at = 0;

			for ( k = p->first; k < i; k++ ) {
				at += run_units(&(*d->doc)->run[k]);
			}
			tad_node_remove((*d->doc)->run[i].node);
			if ( at < d->cpos ) {
				acc++;
			}
		}
	}
	d->cpos -= acc;
	d->host->model(d);
	if ( put_elem(d, d->cpara, d->cpos, "indent") != NULL ) {
		d->cpos++;
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/*
 * 改ページ: the paragraph cut in two at the caret, and a page break
 * put between the halves, so that what follows starts a new page.
 */
LOCAL void page_break( T_DDED *d )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*pn;

	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	if ( (*d->doc)->para[d->cpara].node == NULL ) {
		(void)put_elem(d, d->cpara, d->cpos, "pagebreak");
		changed(d);
		return;
	}
	if ( d->cpos > 0 && d->cpos < para_len(d, d->cpara) ) {
		if ( split_at(d, d->cpara, d->cpos) ) {
			d->host->model(d);
			d->cpara++;
			d->cpos = 0;
		}
	}
	pn = (*d->doc)->para[d->cpara].node;
	if ( d->cpos == 0 ) {
		(void)tad_elem_new(rec, "pagebreak", pn->parent, pn, TRUE);
	} else {
		(void)tad_elem_new(rec, "pagebreak", pn->parent, pn->next, TRUE);
		if ( d->cpara + 1 < (*d->doc)->npara ) {
			d->cpara++;
			d->cpos = 0;
		}
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* ---------------------------------------------------------------- paragraphs */

/*
 * A setting of that name -- one with that attribute, when one is
 * named -- among the things a paragraph begins with, before any text
 * or anything shown.
 */
LOCAL T_TADNODE *head_setting( T_TADNODE *pn, CONST char *name,
			       CONST char *attr )
{
	T_TADNODE	*c;

	for ( c = ( pn != NULL ) ? pn->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT ) {
			INT	k;

			for ( k = 0; c->text != NULL && c->text[k] != 0; k++ ) {
				if ( c->text[k] > ' ' ) {
					return NULL;
				}
			}
			continue;
		}
		if ( c->kind != TAD_ND_ELEM ) {
			continue;
		}
		if ( same_word(c->name, name)
		  && ( attr == NULL || tad_attr(c, attr) != NULL ) ) {
			return c;
		}
		if ( c->first != NULL || same_word(c->name, "br")
		  || same_word(c->name, "link") || same_word(c->name, "image")
		  || same_word(c->name, "tab") ) {
			return NULL;
		}
	}

	return NULL;
}

LOCAL CONST char *align_word( UINT a )
{
	return ( a == TV_ALIGN_CENTRE ) ? "center"
	     : ( a == TV_ALIGN_RIGHT ) ? "right"
	     : ( a == TV_ALIGN_JUSTIFY ) ? "justify" : "left";
}

/* A paragraph's alignment, written at its head */
LOCAL void set_align( T_DDED *d, INT pi, CONST char *how )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*pn = (*d->doc)->para[pi].node, *c;

	if ( pn == NULL ) {
		return;
	}
	c = head_setting(pn, "text", "align");
	if ( c == NULL ) {
		c = tad_elem_new(rec, "text", pn, pn->first, TRUE);
	}
	if ( c != NULL ) {
		(void)tad_set_attr(rec, c, "align", (CONST UB *)how);
	}
	if ( tad_attr(pn, "align") != NULL ) {
		(void)tad_set_attr(rec, pn, "align", (CONST UB *)how);
	}
}

/*
 * 行揃え, for every paragraph the selection touches. An alignment holds
 * from where it is written until another is, so the paragraph after
 * the last one is given back the alignment it had, unless it says its
 * own.
 */
LOCAL void align_sel( T_DDED *d, CONST char *how )
{
	CONST char	*keep = NULL;
	INT		pa, a, pb, b, pi;

	undo_point(d, FALSE);
	sel_order(d, &pa, &a, &pb, &b);
	if ( pb + 1 < (*d->doc)->npara && (*d->doc)->para[pb + 1].node != NULL
	  && head_setting((*d->doc)->para[pb + 1].node, "text", "align") == NULL
	  && tad_attr((*d->doc)->para[pb + 1].node, "align") == NULL ) {
		keep = align_word((*d->doc)->para[pb + 1].align);
	}
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		set_align(d, pi, how);
	}
	if ( keep != NULL ) {
		set_align(d, pb + 1, keep);
	}
	changed(d);
}

/* A number as words, whole or with three places: "12", "0.75" */
LOCAL void num_word( INT v, INT thousandths, UB *out )
{
	char	t[16];
	INT	n = 0, k = 0, whole, frac;

	if ( v < 0 ) {
		out[k++] = '-';
		v = -v;
	}
	whole = thousandths ? v / 1000 : v;
	frac = thousandths ? v % 1000 : 0;
	do {
		t[n++] = (char)( '0' + whole % 10 );
		whole /= 10;
	} while ( whole > 0 && n < 15 );
	while ( n > 0 ) {
		out[k++] = (UB)t[--n];
	}
	if ( frac > 0 ) {
		out[k++] = '.';
		out[k++] = (UB)( '0' + frac / 100 );
		frac %= 100;
		if ( frac > 0 ) {
			out[k++] = (UB)( '0' + frac / 10 );
			if ( frac % 10 > 0 ) {
				out[k++] = (UB)( '0' + frac % 10 );
			}
		}
	}
	out[k] = 0;
}

/* Words as a number in thousandths; -1 when they are not one */
LOCAL INT word_num( CONST UB *s )
{
	INT	whole = 0, frac = 0, digits = 0;
	BOOL	any = FALSE;

	while ( *s == ' ' ) {
		s++;
	}
	while ( *s >= '0' && *s <= '9' ) {
		whole = whole * 10 + ( *s++ - '0' );
		any = TRUE;
	}
	if ( *s == '.' ) {
		for ( s++; *s >= '0' && *s <= '9'; s++ ) {
			if ( digits < 3 ) {
				frac = frac * 10 + ( *s - '0' );
				digits++;
			}
			any = TRUE;
		}
	}
	while ( *s == ' ' ) {
		s++;
	}
	if ( !any || *s != 0 ) {
		return -1;
	}
	while ( digits < 3 ) {
		frac *= 10;
		digits++;
	}

	return whole * 1000 + frac;
}

/* Pixels in the record's units, and back: the units are its docScale */
LOCAL INT to_units( CONST T_DDED *d, INT px )
{
	INT	dpi = ( (*d->doc)->hunit < 0 ) ? -(*d->doc)->hunit : 72;

	return ( px * dpi + 36 ) / 72;
}

#define CHAR_PX		14		/* one letter of the standard size */

/*
 * 新規タブ書式: the margins, the first line's indent, the line and
 * paragraph spacing and the tab stops, written as a <tab-format> at the
 * head of the paragraph. It holds from there on until another says
 * otherwise, as the paragraphs after it had the one before. With the
 * caret inside a paragraph, the paragraph is cut there first and the
 * new format starts the second half.
 */
LOCAL void tab_format( T_DDED *d )
{
	LOCAL CONST char *CONST labels[] = {
		"左マージン:", "右マージン:", "字下げ:", "行間:", "段落間隔:",
		"タブ間隔:"
	};
	LOCAL CONST char *CONST hints[] = {
		"字 (2行目以降)", "字", "字 (1行目)", "0=標準(1.5倍)", "倍率",
		"字 (0=指定なし)"
	};
	UB		vals[6][DE_FIELD_MAX], num[16];
	T_TAD		*rec = rec_of(d);
	CONST T_TVPARA	*p;
	T_TADNODE	*pn, *c;
	INT		pa, a, pb, b, v[6], i, pi;

	sel_order(d, &pa, &a, &pb, &b);
	p = &(*d->doc)->para[pa];
	num_word(p->left / CHAR_PX, 0, vals[0]);
	num_word(p->right / CHAR_PX, 0, vals[1]);
	num_word(p->indent / CHAR_PX, 0, vals[2]);
	num_word(( p->gap_abs || p->gap == 500 ) ? 0 : p->gap, 1, vals[3]);
	num_word(p->pargap_abs ? 0 : p->pargap, 1, vals[4]);
	num_word(( p->ntabs > 0 ) ? p->tabs[0] / CHAR_PX : 4, 0, vals[5]);
	if ( !d->host->fields_form(d, "タブ書式設定", 6, labels, hints, vals) ) {
		return;
	}
	for ( i = 0; i < 6; i++ ) {
		v[i] = word_num(vals[i]);
		if ( v[i] < 0 ) {
			d->host->tell(d, "タブ書式を設定できません", "数値を入力してください");
			return;
		}
	}

	undo_point(d, FALSE);
	pi = pa;
	if ( !has_sel(d) && d->cpos > 0 && split_at(d, d->cpara, d->cpos) ) {
		d->host->model(d);
		pi = ++d->cpara;
		d->cpos = 0;
		d->apara = d->cpara;
		d->apos = 0;
	}
	pn = (*d->doc)->para[pi].node;
	if ( pn == NULL ) {
		changed(d);
		return;
	}
	c = head_setting(pn, "tab-format", NULL);
	if ( c == NULL ) {
		c = tad_elem_new(rec, "tab-format", pn, pn->first, TRUE);
	}
	if ( c == NULL ) {
		changed(d);
		return;
	}
	num_word(0, 0, num);
	(void)tad_set_attr(rec, c, "R", num);
	(void)tad_set_attr(rec, c, "P", num);
	num_word(v[3], 1, num);
	(void)tad_set_attr(rec, c, "height", num);
	num_word(v[4], 1, num);
	(void)tad_set_attr(rec, c, "pargap", num);
	num_word(to_units(d, v[0] / 1000 * CHAR_PX), 0, num);
	(void)tad_set_attr(rec, c, "left", num);
	num_word(to_units(d, v[1] / 1000 * CHAR_PX), 0, num);
	(void)tad_set_attr(rec, c, "right", num);
	num_word(to_units(d, v[2] / 1000 * CHAR_PX), 0, num);
	(void)tad_set_attr(rec, c, "indent", num);
	num_word(( v[5] >= 1000 ) ? 1 : 0, 0, num);
	(void)tad_set_attr(rec, c, "ntabs", num);
	if ( v[5] >= 1000 ) {
		num_word(to_units(d, v[5] / 1000 * CHAR_PX), 0, num);
	} else {
		num[0] = 0;
	}
	(void)tad_set_attr(rec, c, "tabs", num);
	changed(d);
}

/*
 * ルビ: the words picked given a reading, over them or under them. On
 * letters that have one already it is changed, or, given nothing,
 * taken off.
 */
LOCAL void ruby_sel( T_DDED *d, BOOL below )
{
	UB		start[WM_LABEL_MAX], text[WM_LABEL_MAX];
	CONST char	*attrs[5];
	T_TADNODE	*e = NULL;
	INT		pa, a, pb, b, i, acc = 0, k;

	if ( !has_sel(d) ) {
		return;
	}
	sel_order(d, &pa, &a, &pb, &b);
	{
		CONST T_TVPARA	*p = &(*d->doc)->para[pa];

		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			INT	u = run_units(&(*d->doc)->run[i]);

			if ( acc + u > a ) {
				e = named_above((*d->doc)->run[i].node,
						para_home(d, pa), nm_ruby);
				break;
			}
			acc += u;
		}
	}
	start[0] = 0;
	if ( e != NULL && tad_attr(e, "text") != NULL ) {
		CONST UB	*t = tad_attr(e, "text");

		for ( k = 0; t[k] != 0 && k < WM_LABEL_MAX - 1; k++ ) {
			start[k] = t[k];
		}
		start[k] = 0;
	}
	if ( !d->host->ask_text(d, "ルビを入力してください（空欄で削除）", NULL, start,
			  text, WM_LABEL_MAX, "設定") ) {
		return;
	}
	undo_point(d, FALSE);
	if ( e != NULL ) {
		if ( text[0] == 0 ) {
			unwrap_range(d, nm_ruby);
		} else {
			(void)tad_set_attr(rec_of(d), e, "text", text);
			(void)tad_set_attr(rec_of(d), e, "position",
					   (CONST UB *)( below ? "1" : "0" ));
		}
	} else if ( text[0] != 0 ) {
		attrs[0] = "position";
		attrs[1] = below ? "1" : "0";
		attrs[2] = "text";
		attrs[3] = (CONST char *)text;
		attrs[4] = NULL;
		wrap_range(d, "ruby", attrs);
	}
	changed(d);
}

/* ---------------------------------------------------------------- 全角、半角 */

/*
 * The letters, figures and signs of ASCII as their full-width forms
 * (U+FF01 to U+FF5E), or those back; everything else as it is.
 */
LOCAL INT width_conv( CONST UB *s, INT n, UB *out, BOOL full )
{
	INT	i = 0, k = 0;

	while ( i < n ) {
		UB	c = s[i];

		if ( full && c >= 0x21 && c <= 0x7E ) {
			UW	u = 0xFF01U + (UW)( c - 0x21 );

			out[k++] = (UB)( 0xE0 | ( u >> 12 ) );
			out[k++] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			out[k++] = (UB)( 0x80 | ( u & 0x3F ) );
			i++;
			continue;
		}
		if ( !full && c == 0xEF && i + 2 < n
		  && ( s[i + 1] & 0xC0 ) == 0x80 && ( s[i + 2] & 0xC0 ) == 0x80 ) {
			UW	u = ( (UW)( c & 0x0F ) << 12 )
				  | ( (UW)( s[i + 1] & 0x3F ) << 6 )
				  | (UW)( s[i + 2] & 0x3F );

			if ( u >= 0xFF01U && u <= 0xFF5EU ) {
				out[k++] = (UB)( u - 0xFF01U + 0x21 );
				i += 3;
				continue;
			}
		}
		out[k++] = s[i++];
	}

	return k;
}

LOCAL void width_sel( T_DDED *d, BOOL full )
{
	T_TAD	*rec = rec_of(d);
	INT	pa, a, pb, b, pi, delta = 0;

	if ( !has_sel(d) ) {
		return;
	}
	undo_point(d, FALSE);
	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pb; pi >= pa; pi-- ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[pi];
		INT		lo = ( pi == pa ) ? a : 0;
		INT		hi = ( pi == pb ) ? b : para_len(d, pi);
		INT		i, k, acc = 0, at;

		for ( k = 0; k < p->n && p->first + k < (*d->doc)->nrun; k++ ) {
			acc += run_units(&(*d->doc)->run[p->first + k]);
		}
		for ( i = k - 1; i >= 0; i-- ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[p->first + i];
			INT		ca, cb;
			UB		*buf;
			INT		m;

			at = acc - run_units(r);	/* where this run begins */
			acc = at;
			ca = ( lo > at ) ? lo : at;
			cb = at + run_units(r);

			if ( cb > hi ) {
				cb = hi;
			}
			if ( r->kind != TV_RUN_TEXT || cb <= ca ) {
				continue;
			}
			buf = (UB *)Kmalloc((SZ)( ( cb - ca ) * 3 + 1 ));
			if ( buf == NULL ) {
				continue;
			}
			m = width_conv(r->text + ( ca - at ), cb - ca, buf, full);
			(void)tad_text_splice(rec, r->node, r->noff + ( ca - at ),
					      cb - ca, buf, m);
			Kfree(buf);
			if ( pi == pb ) {
				delta += m - ( cb - ca );
			}
		}
	}
	d->apara = pa;
	d->apos = a;
	d->cpara = pb;
	d->cpos = b + delta;
	changed(d);
}

/* ---------------------------------------------------------------- the paper */

LOCAL T_TADNODE *first_named( T_TAD *rec, CONST char *name )
{
	T_TADNODE	*n;

	for ( n = tad_walk(rec, NULL); n != NULL; n = tad_walk(rec, n) ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, name) ) {
			return n;
		}
	}

	return NULL;
}

/* Pixels as tenths of a millimetre, and back */
LOCAL INT px_mm10( INT px )
{
	/* to the whole millimetre: a pixel is a third of one */
	return ( ( px * 254 / 72 ) + 5 ) / 10 * 10;
}

LOCAL INT mm10_px( INT mm10 )
{
	return ( mm10 * 72 + 127 ) / 254;
}


/*
 * ヘッダ/フッタ: what stands at the left, in the middle and at the right
 * of the top and the bottom of every page, how tall each band is, and
 * the number of the first page. They are 用紙オーバーレイ 0 and 1, put
 * on by a <docoverlay>: each place a paragraph aligned to it, a height
 * given as the line's own. The letters that stand for something else
 * as it is printed are written as what fills them in: %P (and '#') a
 * <page-number> that counts the pages on from the first; %T the number
 * of pages, %N the object's name, %Y %y %M %m %D %d the date (the
 * year's last two figures and the whole year, the month and the day
 * without and with a nought in front) as <variable>s; %% a '%'.
 */

/* The letter after '%' and the <variable> that fills it in */
typedef struct {
	UB	letter;
	INT	id;
} HFVAR;

LOCAL CONST HFVAR hf_vars[] = {
	{ 'T', 250 }, { 'N', 0 }, { 'Y', 100 }, { 'y', 102 },
	{ 'M', 110 }, { 'm', 111 }, { 'D', 120 }, { 'd', 121 }
};
#define N_HFVAR	( (INT)( sizeof(hf_vars) / sizeof(hf_vars[0]) ) )

/* The overlay defined with a number, at the head of the document */
LOCAL T_TADNODE *overlay_def( T_TAD *rec, INT n )
{
	T_TADNODE	*body = tad_body(rec), *c;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		CONST UB	*nv;

		if ( c->kind == TAD_ND_ELEM && same_word(c->name, "paper-overlay-define")
		  && ( nv = tad_attr(c, "N") ) != NULL && word_num(nv) == n * 1000 ) {
			return c;
		}
	}

	return NULL;
}

/* What an overlay holds, read back: the words of each place, the height */
LOCAL void overlay_read( T_TADNODE *def, UB (*place)[DE_FIELD_MAX],
			 INT *p_mm, INT *p_start )
{
	T_TADNODE	*p, *c;

	*p_mm = 0;
	for ( p = ( def != NULL ) ? def->first : NULL; p != NULL; p = p->next ) {
		INT	which = 0, k = 0;
		UB	buf[DE_FIELD_MAX];

		if ( p->kind != TAD_ND_ELEM ) {
			continue;
		}
		if ( same_word(p->name, "tab-format") ) {
			CONST UB	*h = tad_attr(p, "height");

			if ( h != NULL && h[0] == 'a' && h[1] == 'b' && h[2] == 's' ) {
				*p_mm = px_mm10(word_num(h + 4) / 1000) / 10;
			}
			continue;
		}
		if ( !same_word(p->name, "p") ) {
			continue;
		}
		for ( c = p->first; c != NULL; c = c->next ) {
			if ( c->kind == TAD_ND_TEXT ) {
				INT	i;

				for ( i = 0; c->text != NULL && c->text[i] != 0
					     && k < DE_FIELD_MAX - 2; i++ ) {
					if ( c->text[i] == '%' ) {
						buf[k++] = '%';		/* a '%' of its own */
					}
					buf[k++] = c->text[i];
				}
			} else if ( same_word(c->name, "page-number") ) {
				if ( k < DE_FIELD_MAX - 2 ) {
					buf[k++] = '%';
					buf[k++] = 'P';
				}
				if ( tad_attr(c, "num") != NULL ) {
					*p_start = word_num(tad_attr(c, "num")) / 1000;
				}
			} else if ( same_word(c->name, "variable") && tad_attr(c, "id") != NULL ) {
				INT	id = word_num(tad_attr(c, "id")) / 1000, v;

				for ( v = 0; v < N_HFVAR && hf_vars[v].id != id; v++ ) ;
				if ( v < N_HFVAR && k < DE_FIELD_MAX - 2 ) {
					buf[k++] = '%';
					buf[k++] = hf_vars[v].letter;
				}
			} else if ( same_word(c->name, "text")
				 && tad_attr(c, "align") != NULL ) {
				CONST UB	*a = tad_attr(c, "align");

				which = ( a[0] == 'c' ) ? 1 : ( a[0] == 'r' ) ? 2 : 0;
			}
		}
		buf[k] = 0;
		for ( k = 0; buf[k] != 0; k++ ) {
			place[which][k] = buf[k];
		}
		place[which][k] = 0;
	}
}

/* An overlay written: a paragraph for each place that has words */
LOCAL BOOL overlay_write( T_TAD *rec, T_TADNODE *before, INT n,
			  UB (*place)[DE_FIELD_MAX], INT mm, INT start )
{
	LOCAL CONST char *CONST aligns[3] = { "left", "center", "right" };
	T_TADNODE	*body = tad_body(rec), *def, *p, *e;
	UB		num[16];
	INT		i, k;

	if ( place[0][0] == 0 && place[1][0] == 0 && place[2][0] == 0 ) {
		return FALSE;
	}
	def = tad_elem_new(rec, "paper-overlay-define", body, before, FALSE);
	if ( def == NULL ) {
		return FALSE;
	}
	num_word(n, 0, num);
	(void)tad_set_attr(rec, def, "N", num);
	(void)tad_set_attr(rec, def, "P", (CONST UB *)"0");
	if ( mm > 0 && ( e = tad_elem_new(rec, "tab-format", def, NULL, TRUE) ) != NULL ) {
		UB	h[20] = "abs:";

		num_word(mm10_px(mm * 10), 0, h + 4);
		(void)tad_set_attr(rec, e, "height", h);
	}
	for ( i = 0; i < 3; i++ ) {
		INT	from = 0;

		if ( place[i][0] == 0 || ( p = tad_elem_new(rec, "p", def, NULL, FALSE) ) == NULL ) {
			continue;
		}
		if ( ( e = tad_elem_new(rec, "text", p, NULL, TRUE) ) != NULL ) {
			(void)tad_set_attr(rec, e, "align", (CONST UB *)aligns[i]);
		}
		for ( k = 0; ; k++ ) {
			UB	c = place[i][k], nx = ( c == '%' ) ? place[i][k + 1] : 0;
			INT	v;

			if ( c != '#' && c != '%' && c != 0 ) {
				continue;
			}
			if ( c == '%' && nx != 'P' && nx != '%' ) {
				for ( v = 0; v < N_HFVAR && hf_vars[v].letter != nx; v++ ) ;
				if ( v == N_HFVAR ) {
					continue;	/* a '%' that stands for nothing: kept as it is */
				}
			}
			if ( k > from ) {
				(void)tad_text_new(rec, p, NULL, place[i] + from, k - from);
			}
			if ( c == 0 ) {
				break;
			}
			from = k + 1;
			if ( c == '%' && nx == '%' ) {
				(void)tad_text_new(rec, p, NULL, (CONST UB *)"%", 1);
				from = ++k + 1;
				continue;
			}
			if ( c == '#' || nx == 'P' ) {
				if ( ( e = tad_elem_new(rec, "page-number", p, NULL, TRUE) ) != NULL ) {
					(void)tad_set_attr(rec, e, "step", (CONST UB *)"1");
					num_word(start, 0, num);
					(void)tad_set_attr(rec, e, "num", num);
				}
			} else {
				for ( v = 0; v < N_HFVAR && hf_vars[v].letter != nx; v++ ) ;
				if ( ( e = tad_elem_new(rec, "variable", p, NULL, TRUE) ) != NULL ) {
					num_word(hf_vars[v].id, 0, num);
					(void)tad_set_attr(rec, e, "id", num);
				}
			}
			if ( c == '%' ) {
				from = ++k + 1;
			}
		}
	}

	return TRUE;
}

LOCAL void header_footer( T_DDED *d )
{
	LOCAL CONST char *CONST labels[] = {
		"ヘッダ 左端:", "ヘッダ 中央:", "ヘッダ 右端:", "ヘッダ 高さ:",
		"フッタ 左端:", "フッタ 中央:", "フッタ 右端:", "フッタ 高さ:",
		"開始ページ番号:"
	};
	LOCAL CONST char *CONST hints[] = {
		"%P ページ %T 全ページ %N 実身名", "%y/%m/%d 日付", NULL, "mm (0=上余白)",
		NULL, NULL, NULL, "mm (0=下余白)", NULL
	};
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *c, *next, *at, *h0, *h1;
	UB		vals[9][DE_FIELD_MAX];
	INT		hmm = 0, fmm = 0, start = 1, i;
	UINT		on = 0;

	if ( body == NULL ) {
		return;
	}
	for ( i = 0; i < 9; i++ ) {
		vals[i][0] = 0;
	}
	h0 = overlay_def(rec, 0);
	h1 = overlay_def(rec, 1);
	overlay_read(h0, vals, &hmm, &start);
	overlay_read(h1, vals + 4, &fmm, &start);
	num_word(hmm, 0, vals[3]);
	num_word(fmm, 0, vals[7]);
	num_word(start, 0, vals[8]);
	if ( !d->host->fields_form(d, "ヘッダ/フッタ", 9, labels, hints, vals) ) {
		return;
	}
	hmm = word_num(vals[3]);
	fmm = word_num(vals[7]);
	start = word_num(vals[8]);
	if ( hmm < 0 || fmm < 0 || start < 0 ) {
		d->host->tell(d, "ヘッダ/フッタを設定できません", "数値を入力してください");
		return;
	}
	undo_point(d, FALSE);
	/* what was there before goes, all of it */
	for ( c = body->first; c != NULL; c = next ) {
		next = c->next;
		if ( c->kind == TAD_ND_ELEM
		  && ( ( same_word(c->name, "paper-overlay-define")
			 && ( c == h0 || c == h1 ) )
		       || ( same_word(c->name, "docoverlay")
			    && tad_attr(c, "active") != NULL ) ) ) {
			tad_node_remove(c);
		}
	}
	/* after the paper and its margins, before the text */
	for ( at = body->first; at != NULL; at = at->next ) {
		if ( at->kind == TAD_ND_ELEM && !same_word(at->name, "paper")
		  && !same_word(at->name, "docmargin") && !same_word(at->name, "docScale")
		  && !same_word(at->name, "docscale") ) {
			break;
		}
	}
	if ( overlay_write(rec, at, 0, vals, hmm / 1000, start / 1000) ) {
		on |= 1U;
	}
	if ( overlay_write(rec, at, 1, vals + 4, fmm / 1000, start / 1000) ) {
		on |= 2U;
	}
	if ( on != 0 && ( c = tad_elem_new(rec, "docoverlay", body, at, TRUE) ) != NULL ) {
		(void)tad_set_attr(rec, c, "active",
				   (CONST UB *)( ( on == 3U ) ? "0, 1" : ( on == 1U ) ? "0" : "1" ));
	}
	changed(d);
}

/*
 * 用紙設定: the size of the paper and its margins, written into the
 * <paper> and <docmargin> at the head of the document, or into new
 * ones put there.
 */
LOCAL void page_setup( T_DDED *d )
{
	T_TAD		*rec = rec_of(d);
	T_TVDOC		*doc = (*d->doc);
	T_TADNODE	*body = tad_body(rec), *pp, *dm;
	T_DEPAPER	ps;
	UB		num[16];
	INT		r;

	ps.w = px_mm10(( doc->paper_w > 0 ) ? doc->paper_w : DE_PAPER_W);
	ps.h = px_mm10(( doc->paper_h > 0 ) ? doc->paper_h : DE_PAPER_H);
	ps.left = px_mm10(doc->margin[0]);
	ps.top = px_mm10(doc->margin[1]);
	ps.right = px_mm10(doc->margin[2]);
	ps.bottom = px_mm10(doc->margin[3]);
	pp = first_named(rec, "paper");
	ps.imposition = ( pp != NULL && tad_attr(pp, "imposition") != NULL
			  && tad_attr(pp, "imposition")[0] == '1' ) ? 1 : 0;
	r = d->host->paper_form(d, &ps);
	while ( r == DE_PAPER_HF ) {
		/* the header and footer, and then the paper again */
		header_footer(d);
		r = d->host->paper_form(d, &ps);
	}
	if ( r == DE_PAPER_CANCEL || body == NULL ) {
		return;
	}
	if ( r == DE_PAPER_STD ) {
		/* A4, 20 mm all round but 10 mm at the fore-edge */
		ps.w = 2100;  ps.h = 2970;
		ps.top = 200;  ps.bottom = 200;
		ps.left = 200;  ps.right = 100;
		ps.imposition = 0;
	}
	undo_point(d, FALSE);
	if ( pp == NULL ) {
		pp = tad_elem_new(rec, "paper", body, body->first, TRUE);
		if ( pp == NULL ) {
			return;
		}
		(void)tad_set_attr(rec, pp, "type", (CONST UB *)"doc");
		(void)tad_set_attr(rec, pp, "binding", (CONST UB *)"0");
	}
	num_word(to_units(d, mm10_px(ps.h)), 0, num);
	(void)tad_set_attr(rec, pp, "length", num);
	num_word(to_units(d, mm10_px(ps.w)), 0, num);
	(void)tad_set_attr(rec, pp, "width", num);
	num_word(ps.imposition, 0, num);
	(void)tad_set_attr(rec, pp, "imposition", num);
	dm = first_named(rec, "docmargin");
	if ( dm == NULL ) {
		dm = tad_elem_new(rec, "docmargin", pp->parent, pp->next, TRUE);
	}
	num_word(to_units(d, mm10_px(ps.top)), 0, num);
	(void)tad_set_attr(rec, pp, "top", num);
	if ( dm != NULL ) (void)tad_set_attr(rec, dm, "top", num);
	num_word(to_units(d, mm10_px(ps.bottom)), 0, num);
	(void)tad_set_attr(rec, pp, "bottom", num);
	if ( dm != NULL ) (void)tad_set_attr(rec, dm, "bottom", num);
	num_word(to_units(d, mm10_px(ps.left)), 0, num);
	(void)tad_set_attr(rec, pp, "left", num);
	if ( dm != NULL ) (void)tad_set_attr(rec, dm, "left", num);
	num_word(to_units(d, mm10_px(ps.right)), 0, num);
	(void)tad_set_attr(rec, pp, "right", num);
	if ( dm != NULL ) (void)tad_set_attr(rec, dm, "right", num);
	changed(d);
}

/* ---------------------------------------------------------------- 縦書き */

/*
 * The 文字方向 of the text: a <text direction> among the elements that
 * stand at the head of the document, outside any paragraph, where the
 * settings of the whole text are. NULL when there is none.
 */
LOCAL T_TADNODE *dir_node( T_TAD *rec )
{
	T_TADNODE	*body = tad_body(rec), *n;

	for ( n = ( body != NULL ) ? body->first : NULL; n != NULL; n = n->next ) {
		if ( n->kind == TAD_ND_ELEM && same_word(n->name, "text")
		  && tad_attr(n, "direction") != NULL ) {
			return n;
		}
	}

	return NULL;
}

/* Whether a node only spaces the elements of a record apart */
LOCAL BOOL blank_node( CONST T_TADNODE *n )
{
	CONST UB	*t;

	if ( n->kind != TAD_ND_TEXT ) {
		return FALSE;
	}
	for ( t = n->text; t != NULL && *t != 0; t++ ) {
		if ( *t != ' ' && *t != '\t' && *t != '\r' && *t != '\n' ) {
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * 縦書き and 横書き: the whole text written downwards, or across again.
 * Down is <text direction="2"/> put after what heads the document --
 * its paper, its margins, its scale, its <text lang> -- before the first
 * paragraph; across takes it away, or, when it says more than the
 * direction, sets the direction to 0.
 */
LOCAL void set_direction( T_DDED *d, BOOL vert )
{
	T_TAD		*rec = rec_of(d);
	T_TADNODE	*body = tad_body(rec), *t, *at;

	if ( (*d->xml_view) || body == NULL || (*d->doc) == NULL || (*d->doc)->vert == vert ) {
		return;
	}
	undo_point(d, FALSE);
	t = dir_node(rec);
	if ( vert ) {
		if ( t == NULL ) {
			for ( at = body->first; at != NULL; at = at->next ) {
				if ( blank_node(at) ) {
					continue;
				}
				if ( at->kind == TAD_ND_ELEM
				  && ( same_word(at->name, "paper") || same_word(at->name, "docmargin")
				    || same_word(at->name, "docScale") || same_word(at->name, "docView")
				    || same_word(at->name, "docDraw") || same_word(at->name, "text") ) ) {
					continue;
				}
				break;
			}
			t = tad_elem_new(rec, "text", body, at, TRUE);
			if ( t != NULL ) {
				(void)tad_text_new(rec, body, at, (CONST UB *)"\n", 1);
			}
		}
		if ( t != NULL ) {
			(void)tad_set_attr(rec, t, "direction", (CONST UB *)"2");
		}
	} else if ( t != NULL ) {
		if ( t->nattr <= 1 ) {
			if ( t->next != NULL && blank_node(t->next) ) {
				tad_node_remove(t->next);
			}
			tad_node_remove(t);
		} else {
			(void)tad_set_attr(rec, t, "direction", (CONST UB *)"0");
		}
	}
	/* the head of the text, where either way of writing it begins */
	(*d->scroll_x) = (*d->scroll_y) = 0;
	changed(d);
}

/* Whether every letter picked is turned (回転) */
LOCAL BOOL sel_turned( T_DDED *d )
{
	INT	pa, a, pb, b, pi, i, acc;
	BOOL	any = FALSE;

	sel_order(d, &pa, &a, &pb, &b);
	for ( pi = pa; pi <= pb && pi < (*d->doc)->npara; pi++ ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[pi];
		INT		lo = ( pi == pa ) ? a : 0;
		INT		hi = ( pi == pb ) ? b : para_len(d, pi);

		acc = 0;
		for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
			CONST T_TVRUN	*r = &(*d->doc)->run[i];
			INT		u = run_units(r);

			if ( r->kind == TV_RUN_TEXT && u > 0 && acc < hi
			  && acc + u > lo ) {
				any = TRUE;
				if ( r->turn == 0 ) {
					return FALSE;
				}
			}
			acc += u;
		}
	}

	return any;
}

/*
 * 縦中横: the letters picked set across in one box of the line, as
 * <combchar> (結合文字: letters taken together as one). The letters
 * picked must stand in one line of one paragraph; picked again, they
 * are taken out of it.
 */
LOCAL void comb_sel( T_DDED *d )
{
	LOCAL CONST char *CONST nm_comb[] = { "combchar", NULL };
	INT	pa, a, pb, b, i, acc = 0;

	if ( !has_sel(d) ) {
		return;
	}
	sel_order(d, &pa, &a, &pb, &b);
	if ( pa != pb ) {
		d->host->tell(d, "縦中横にする範囲に改段落を含めることはできません", NULL);
		return;
	}
	for ( i = (*d->doc)->para[pa].first;
	      i < (*d->doc)->para[pa].first + (*d->doc)->para[pa].n && i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];

		if ( acc >= a && acc < b
		  && ( r->kind == TV_RUN_BREAK || r->kind == TV_RUN_PAGE ) ) {
			d->host->tell(d, "縦中横にする範囲に改行を含めることはできません", NULL);
			return;
		}
		acc += run_units(r);
	}
	style_sel(d, nm_comb, TV_ST_COMB);
}

/*
 * 回転: the letters picked -- or, with nothing picked, from the caret to
 * the end of the text -- turned a quarter clockwise, so that a word of
 * Latin letters reads down the line, as <font rotation="90">. Picked
 * again, they are upright again: the <font> elements that turn them are
 * taken away and nothing else.
 */
LOCAL void turn_sel( T_DDED *d )
{
	LOCAL CONST char *CONST attrs[] = { "rotation", "90", "rotabs", "0", NULL };
	LOCAL CONST char *CONST nm_font[] = { "font", NULL };

	if ( !has_sel(d) ) {
		INT	np = (*d->doc)->npara;

		if ( np <= 0 ) {
			return;
		}
		d->apara = np - 1;
		d->apos = para_len(d, np - 1);
		if ( !has_sel(d) ) {
			return;
		}
	}
	undo_point(d, FALSE);
	if ( sel_turned(d) ) {
		de_need_attr = "rotation";
		unwrap_range(d, nm_font);
		de_need_attr = NULL;
	} else {
		wrap_range(d, "font", attrs);
	}
	changed(d);
}

/* How tall the letters at the caret are drawn: those before it, else those after */
EXPORT INT de_caret_px( T_DDED *d )
{
	INT	off = 0, ri;

	if ( !de_editable(d) ) {
		return 14;
	}
	clamp(d);
	ri = text_at(d, d->cpara, d->cpos, &off);
	if ( ri < 0 ) {
		CONST T_TVPARA	*p = &(*d->doc)->para[d->cpara];

		ri = ( p->n > 0 ) ? p->first : -1;
	}

	return ( ri >= 0 && (*d->doc)->run[ri].size > 0 ) ? (*d->doc)->run[ri].size : 14;
}

/* What the menu ticks for the letters picked: 縦中横 and 回転 */
EXPORT void de_menu_state( T_DDED *d, BOOL *p_comb, BOOL *p_turned )
{
	*p_comb = FALSE;
	*p_turned = FALSE;
	if ( !de_editable(d) || (*d->xml_view) || !has_sel(d) ) {
		return;
	}
	clamp(d);
	*p_comb = sel_all(d, TV_ST_COMB);
	*p_turned = sel_turned(d);
}

/* ---------------------------------------------------------------- laying while idle */

/*
 * A document's paragraphs not yet laid at the window's present width
 * count as a guess (docview.c); they are laid a few at a time while the
 * desktop has nothing else to do, so that the scroll bar comes to say
 * how long the text is. What the window shows was laid when it was
 * drawn; when the paragraphs above it come out taller or shorter than
 * the guess, the window scrolls by as much, and what it shows stays.
 */

/*
 * More of the paragraphs laid only as a guess laid, for at most ms: how
 * many are left. What was laid above the place shown moved it; the page
 * is wound by as much (*p_shift), to show the same place. *p_laid says
 * how many were laid this time: the window is drawn again when the last
 * of them is laid, even when that was the first time round, so that the
 * scroll bars come to say how tall the text is.
 */
EXPORT INT de_settle( T_DDED *d, INT ms, INT *p_shift, INT *p_laid )
{
	T_DPRECT	page;
	INT		left, shift = 0;

	*p_shift = 0;
	*p_laid = 0;
	if ( !(*d->settling) || (*d->doc) == NULL || (*d->doc)->layc == NULL ) {
		return 0;
	}
	d->host->work_rect(d, &page);
	left = tv_doc_settle((*d->doc), &page, (*d->scroll_y), (*d->rec), ms, &shift, p_laid);
	if ( shift != 0 ) {
		(*d->scroll_y) += shift;
		if ( (*d->scroll_y) < 0 ) {
			(*d->scroll_y) = 0;
		}
	}
	if ( left == 0 ) {
		(*d->settling) = FALSE;
	}
	*p_shift = shift;
	return left;
}

/* ---------------------------------------------------------------- keys */

/* The caret moved; with Shift the selection follows it, without, it goes */
LOCAL void move_to( T_DDED *d, INT pi, INT pos, BOOL extend )
{
	d->cpara = pi;
	d->cpos = pos;
	if ( !extend ) {
		d->apara = pi;
		d->apos = pos;
	}
	d->typing = FALSE;
	clamp(d);
	d->host->draw(d);
	de_show_caret(d);
	shown(d);
}

/* The place at the head (dx_mode < 0) or at the end (> 0) of the caret's line */
LOCAL BOOL line_place( T_DDED *d, INT dx_mode, INT *p_para, INT *p_pos )
{
	T_DPRECT	page, box;
	INT		x, y;

	d->host->work_rect(d, &page);
	if ( tv_doc_caret_box((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara,
			      d->cpos, &box) < E_OK ) {
		return FALSE;
	}
	if ( (*d->doc)->vert ) {
		/* 縦書き: a line's head is at the top of the page */
		x = ( box.left + box.right ) / 2;
		y = ( dx_mode < 0 ) ? page.top : page.bottom;
	} else {
		x = ( dx_mode < 0 ) ? page.left : page.right;
		y = ( box.top + box.bottom ) / 2;
	}

	return (BOOL)( tv_doc_caret_at((*d->doc), &page, (*d->scroll_y), (*d->rec), x, y,
				       p_para, p_pos) >= E_OK );
}

/*
 * Where the caret was across its line when ↑ or ↓ began to move it (縦書き:
 * down the line), from the page's edge, and the window and the place
 * the last ↑ or ↓ left it at. While the keys follow one another the
 * caret keeps going back to that first place across, even through a
 * line too short to reach it; any other move forgets it.
 */
LOCAL T_DDED	*ud_win;
LOCAL INT	ud_para, ud_pos, ud_goal;

/*
 * The place on the line before (dir < 0) or after (dir > 0) the caret's,
 * in the same paragraph or the one next to it, nearest the place across
 * the caret started from. FALSE on the first or the last line.
 */
LOCAL BOOL line_step( T_DDED *d, INT dir, INT *p_para, INT *p_pos )
{
	T_DPRECT	page, box, tb;
	BOOL		v = (*d->doc)->vert;
	INT		goal, head, end, qp, qpos, th, te, x, y, np = (*d->doc)->npara;

	d->host->work_rect(d, &page);
	if ( tv_doc_caret_line((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara,
			       d->cpos, &box, &head, &end) < E_OK ) {
		return FALSE;
	}
	if ( ud_win == d && ud_para == d->cpara && ud_pos == d->cpos ) {
		goal = ud_goal;
	} else {
		goal = v ? box.top - page.top : box.left - page.left;
	}

	/* a place on the line wanted, and that line */
	if ( dir < 0 ) {
		if ( head > 0 ) {
			qp = d->cpara;
			qpos = step_back(d, qp, head);
		} else if ( d->cpara > 0 ) {
			qp = d->cpara - 1;
			qpos = para_len(d, qp);
		} else {
			return FALSE;
		}
	} else {
		/* where a line is broken, the place is the next line's */
		qp = d->cpara;
		qpos = end;
	}
	if ( tv_doc_caret_line((*d->doc), &page, (*d->scroll_y), (*d->rec), qp, qpos,
			       &tb, &th, &te) < E_OK ) {
		return FALSE;
	}
	if ( dir > 0 && qp == d->cpara && th == head ) {
		/* the last line of the paragraph: the next paragraph's first */
		if ( d->cpara + 1 >= np ) {
			return FALSE;
		}
		qp = d->cpara + 1;
		if ( tv_doc_caret_line((*d->doc), &page, (*d->scroll_y), (*d->rec), qp, 0,
				       &tb, &th, &te) < E_OK ) {
			return FALSE;
		}
	}

	/* the place on it nearest the place across */
	if ( v ) {
		x = ( tb.left + tb.right ) / 2;
		y = page.top + goal;
	} else {
		x = page.left + goal;
		y = ( tb.top + tb.bottom ) / 2;
	}
	if ( tv_doc_caret_in((*d->doc), &page, (*d->scroll_y), (*d->rec), qp, th, te,
			     x, y, p_pos) < E_OK ) {
		return FALSE;
	}
	/*
	 * The end of a line that is broken is drawn at the head of the next
	 * one: the caret stops before the line's last letter instead.
	 */
	if ( *p_pos == te && te > th && te < para_len(d, qp) ) {
		*p_pos = step_back(d, qp, te);
	}
	*p_para = qp;
	ud_goal = goal;

	return TRUE;
}

/*
 * A page up or down: the text moved by the height of the window less a
 * line, and the caret to the place that is where it was on the screen.
 */
LOCAL void page_move( T_DDED *d, INT dir, BOOL shift )
{
	T_DPRECT	page, box;
	INT		room, step, to, pi, pos;

	d->host->work_rect(d, &page);
	if ( tv_doc_caret_box((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara,
			      d->cpos, &box) < E_OK ) {
		return;
	}
	step = ( page.bottom - page.top ) - ( box.bottom - box.top );
	room = (*d->height) - ( page.bottom - page.top );
	if ( (*d->doc)->vert ) {
		/* 縦書き: a page is the width of the window less a line */
		step = ( page.right - page.left ) - ( box.right - box.left );
		room = (*d->height) - ( page.right - page.left );
	}
	if ( step < 16 ) {
		step = 16;
	}
	to = (*d->scroll_y) + dir * step;
	if ( to > room ) to = room;
	if ( to < 0 )    to = 0;
	if ( to == (*d->scroll_y) ) {
		/* no further to go: the first or the last line */
		if ( dir < 0 ) {
			move_to(d, 0, 0, shift);
		} else if ( (*d->doc)->npara > 0 ) {
			move_to(d, (*d->doc)->npara - 1, para_len(d, (*d->doc)->npara - 1), shift);
		}
		return;
	}
	(*d->scroll_y) = to;
	if ( tv_doc_caret_at((*d->doc), &page, (*d->scroll_y), (*d->rec),
			     (*d->doc)->vert ? ( box.left + box.right ) / 2 : box.left,
			     (*d->doc)->vert ? box.top : ( box.top + box.bottom ) / 2,
			     &pi, &pos) >= E_OK ) {
		move_to(d, pi, pos, shift);
	}
	d->host->draw(d);
	shown(d);
}

/* Whether a key is one of the four arrows */
LOCAL BOOL arrow( UINT code )
{
	return (BOOL)( code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT
		    || code == KEY_RIGHT );
}

/*
 * An arrow with Alt or with Ctrl held. Across, Alt with ← and → goes to
 * the head and the end of the line, with ↑ and ↓ to the head and the end
 * of the text, and Ctrl with ↑ and ↓ goes a page back and on. Down the
 * line (縦書き) the same is turned: Alt with ↑ and ↓ to the head and the
 * end of the line, with → and ← to the head and the end of the text, and
 * Ctrl with → and ← a page back and on.
 */
LOCAL void arrow_far( T_DDED *d, UINT code, BOOL alt, BOOL shift )
{
	BOOL	v = (*d->doc)->vert;
	INT	pi, pos, np = (*d->doc)->npara;
	UINT	line_head = v ? KEY_UP : KEY_LEFT, line_end = v ? KEY_DOWN : KEY_RIGHT;
	UINT	text_head = v ? KEY_RIGHT : KEY_UP, text_end = v ? KEY_LEFT : KEY_DOWN;

	if ( alt ) {
		if ( code == line_head || code == line_end ) {
			if ( line_place(d, ( code == line_head ) ? -1 : 1, &pi, &pos) ) {
				move_to(d, pi, pos, shift);
			}
		} else if ( code == text_head ) {
			move_to(d, 0, 0, shift);
		} else if ( np > 0 ) {
			move_to(d, np - 1, para_len(d, np - 1), shift);
		}
		return;
	}
	if ( code == text_head ) {
		page_move(d, -1, shift);
	} else if ( code == text_end ) {
		page_move(d, 1, shift);
	}
}

EXPORT BOOL de_key( T_DDED *d, UINT code, UINT mods )
{
	BOOL	ctrl = (BOOL)( ( mods & MOD_CTRL ) != 0 );
	BOOL	shift = (BOOL)( ( mods & MOD_SHIFT ) != 0 );
	BOOL	alt = (BOOL)( ( mods & MOD_ALT ) != 0 );
	INT	pi, pos;
	UB	c;

	if ( !de_editable(d) ) {
		return FALSE;
	}
	clamp(d);
	if ( arrow(code) && ( alt || ctrl ) ) {
		arrow_far(d, code, alt, shift);
		return TRUE;
	}
	if ( ctrl ) {
		switch ( wm_key_cmd(code) ) {		/* the letter the key types: the command's */
		case KEY_A:	de_command(d, DM_SELECT_ALL);	return TRUE;
		case KEY_F:	de_command(d, DM_FIND);		return TRUE;
		case KEY_C:	de_command(d, DM_COPY);		return TRUE;
		case KEY_X:	de_command(d, DM_CUT);		return TRUE;
		case KEY_V:	de_command(d, DM_PASTE);	return TRUE;
		case KEY_Z:	de_command(d, DM_MOVE_BACK);	return TRUE;
		case KEY_U:	de_command(d, DM_STYLE_UNDERLINE); return TRUE;
		case KEY_ENTER:
			/* a new line in the paragraph, not a new paragraph */
			if ( !(*d->xml_view) ) {
				put_thing(d, "br");
			}
			return TRUE;
		case KEY_B:	de_command(d, DM_STYLE_BOLD);	return TRUE;
		case KEY_9:	de_command(d, DM_VIRTUALIZE);	return TRUE;
		case KEY_I:
			de_command(d, shift ? DM_STYLE_ITALIC : DM_FMT_INDENT);
			return TRUE;
		case KEY_HOME:
		case KEY_END:
			break;			/* the head or foot of the text, below */
		default:	return FALSE;
		}
	}
	if ( !( (*d->doc)->vert ? ( code == KEY_LEFT || code == KEY_RIGHT )
			     : ( code == KEY_UP || code == KEY_DOWN ) ) ) {
		/* only ↑ and ↓ (縦書き: ← and →) keep the place across */
		ud_win = NULL;
	}
	/* the converter of かな漢字 sees the keys first (the host's ime_key) */
	if ( !(*d->xml_view) && d->host->ime_key(d, code, mods) ) {
		return TRUE;
	}
	if ( (*d->doc)->vert && arrow(code) ) {
		/*
		 * 縦書き: ↑ and ↓ go to the letter before and after, ← to the
		 * line after (to the left), → to the line before.
		 */
		code = ( code == KEY_UP ) ? KEY_LEFT : ( code == KEY_DOWN ) ? KEY_RIGHT
		     : ( code == KEY_LEFT ) ? KEY_DOWN : KEY_UP;
	}
	switch ( code ) {
	case KEY_ENTER:
		if ( shift && !(*d->xml_view) ) {
			put_thing(d, "br");	/* a new line in the paragraph */
			return TRUE;
		}
		insert(d, (CONST UB *)"\n", 1, FALSE);
		return TRUE;
	case KEY_BS:
		erase_back(d);
		return TRUE;
	case KEY_DEL:
		erase_fwd(d);
		return TRUE;
	case KEY_TAB:
		/* a tab stop in the text; in 原稿, a tab in the words */
		if ( (*d->xml_view) ) {
			UB	t = 0x09;

			insert(d, &t, 1, FALSE);
		} else {
			put_thing(d, "tab");
		}
		return TRUE;
	case KEY_LEFT:
		if ( !shift && has_sel(d) ) {
			INT	pb, b;

			sel_order(d, &pi, &pos, &pb, &b);
			move_to(d, pi, pos, FALSE);
		} else if ( d->cpos > 0 ) {
			move_to(d, d->cpara, step_back(d, d->cpara, d->cpos), shift);
		} else if ( d->cpara > 0 ) {
			move_to(d, d->cpara - 1, para_len(d, d->cpara - 1), shift);
		}
		return TRUE;
	case KEY_RIGHT:
		if ( d->cpos < para_len(d, d->cpara) ) {
			move_to(d, d->cpara, step_fwd(d, d->cpara, d->cpos), shift);
		} else if ( d->cpara + 1 < (*d->doc)->npara ) {
			move_to(d, d->cpara + 1, 0, shift);
		}
		return TRUE;
	case KEY_PGUP:
	case KEY_PGDN:
		page_move(d, ( code == KEY_PGDN ) ? 1 : -1, shift);
		return TRUE;
	case KEY_HOME:
	case KEY_END:
		if ( ctrl ) {
			/* the head or the foot of the whole text */
			if ( code == KEY_HOME ) {
				move_to(d, 0, 0, shift);
			} else if ( (*d->doc)->npara > 0 ) {
				move_to(d, (*d->doc)->npara - 1,
					para_len(d, (*d->doc)->npara - 1), shift);
			}
			return TRUE;
		}
		if ( line_place(d, ( code == KEY_HOME ) ? -1 : 1, &pi, &pos) ) {
			move_to(d, pi, pos, shift);
		}
		return TRUE;
	case KEY_UP:
	case KEY_DOWN:
		/* to the line before or after, into the next paragraph as well */
		if ( line_step(d, ( code == KEY_UP ) ? -1 : 1, &pi, &pos) ) {
			move_to(d, pi, pos, shift);
			ud_win = d;
			ud_para = d->cpara;
			ud_pos = d->cpos;
		}
		return TRUE;
	default:
		break;
	}
	c = wm_key_char(code, mods);
	if ( c < 0x20 || c >= 0x7F ) {
		return FALSE;
	}
	insert(d, &c, 1, TRUE);

	return TRUE;
}

LOCAL BOOL thing_take( T_DDED *d, INT x, INT y, INT slop );
LOCAL BOOL thing_picked( T_DDED *d, INT *p_run, T_DPRECT *p_box );
LOCAL BOOL thing_marks( T_DDED *d, INT gid );
LOCAL BOOL sel_carry_take( T_DDED *d, INT x, INT y, INT pi, INT pos );
LOCAL void sel_carry_follow( INT sx, INT sy );
LOCAL void sel_carry_click( void );

/* ---------------------------------------------------------------- the pointer */

EXPORT BOOL de_editable( CONST T_DDED *d )
{
	return (BOOL)( d != NULL && (*d->doc) != NULL && !(*d->sealed)
		    && (*d->doc)->npara > 0 );
}

/*
 * A press on the page: the caret put there, and a selection begun. A
 * finger's press (when, wm_hit_slop) just out of the right edge or the
 * foot of the one thing picked sizes it, as its handle drawn there.
 */
EXPORT void de_press( T_DDED *d, INT x, INT y, BOOL extend, UD when )
{
	T_DPRECT	page;
	INT		pi, pos, slop;

	if ( !de_editable(d) ) {
		return;
	}
	ud_win = NULL;
	slop = wm_hit_slop(when);
	/* a picture standing in the text is taken, as a link is */
	if ( !extend && thing_take(d, x, y, slop) ) {
		return;
	}
	d->host->work_rect(d, &page);
	if ( tv_doc_caret_at((*d->doc), &page, (*d->scroll_y), (*d->rec), x, y,
			     &pi, &pos) < E_OK ) {
		return;
	}
	/* on what is selected: the letters are carried, if the hand moves */
	if ( !extend && sel_carry_take(d, x, y, pi, pos) ) {
		return;
	}
	move_to(d, pi, pos, extend);
	de_drag = d;
}

/* The hand moving with the button down: the selection follows it */
EXPORT void de_follow( INT sx, INT sy )
{
	T_DDED		*d = de_drag;
	INT		ox, oy;
	T_DPRECT	page;
	INT		pi, pos;

	if ( sc_win != NULL ) {
		sel_carry_follow(sx, sy);
		return;
	}
	if ( d == NULL || !(*d->used) || !d->host->origin(d, &ox, &oy) ) {
		return;
	}
	d->host->work_rect(d, &page);
	if ( tv_doc_caret_at((*d->doc), &page, (*d->scroll_y), (*d->rec),
			     sx - ox, sy - oy,
			     &pi, &pos) >= E_OK
	  && ( pi != d->cpara || pos != d->cpos ) ) {
		move_to(d, pi, pos, TRUE);
	}
}

EXPORT BOOL de_dragging( void )
{
	return (BOOL)( de_drag != NULL || sc_win != NULL );
}

EXPORT void de_release( void )
{
	de_drag = NULL;
	if ( sc_win != NULL ) {
		sel_carry_click();	/* let go where it was pressed: the caret there */
	}
}

/* ---------------------------------------------------------------- 詳細: the marks of settings */

LOCAL T_DDED	*mk_win = NULL;		/* the window a ruler's marker is carried in */
LOCAL T_TVMARK	mk;			/* that marker, as it was taken */
LOCAL INT	mk_x0;			/* where the hand took it */
LOCAL BOOL	mk_moved;
LOCAL INT	mk_click_x, mk_click_y, mk_click_kind = TV_MARK_NONE;
LOCAL UD	mk_click_at;

/* One attribute of an element, as words after its name, for telling */
LOCAL INT say_attr( UB *out, INT at, INT max, CONST T_TADNODE *nd,
		    CONST char *name )
{
	CONST UB	*v = tad_attr(nd, name);
	INT		i;

	if ( v == NULL ) {
		return at;
	}
	for ( i = 0; name[i] != 0 && at < max - 1; i++ ) {
		out[at++] = (UB)name[i];
	}
	if ( at < max - 1 ) out[at++] = '=';
	for ( i = 0; v[i] != 0 && at < max - 1; i++ ) {
		out[at++] = v[i];
	}
	if ( at < max - 1 ) out[at++] = ' ';
	out[at] = 0;

	return at;
}

/* Words after a heading, for telling: "幅: 12" */
LOCAL void say_after( UB *out, INT max, CONST char *head, CONST UB *v,
		      CONST char *none )
{
	INT	at = 0, i;

	for ( i = 0; head[i] != 0 && at < max - 1; i++ ) {
		out[at++] = (UB)head[i];
	}
	if ( v == NULL || v[0] == 0 ) {
		v = (CONST UB *)none;
	}
	for ( i = 0; v[i] != 0 && at < max - 1; i++ ) {
		out[at++] = v[i];
	}
	out[at] = 0;
}

/* What a mark stands for, told in a panel */
LOCAL void mark_tell( T_DDED *d, CONST T_TVMARK *m )
{
	UB		line[160];
	CONST T_TADNODE	*nd = m->node;

	line[0] = 0;
	if ( nd == NULL ) {
		return;
	}
	if ( m->kind == TV_MARK_ALIGN ) {
		CONST UB	*a = tad_attr(nd, "align");
		CONST char	*s = ( a == NULL ) ? "(未指定)"
				   : ( a[0] == 'c' ) ? "中央そろえ"
				   : ( a[0] == 'r' ) ? "行末そろえ (右そろえ)"
				   : ( a[0] == 'j' ) ? "両端そろえ" : "行頭そろえ (左そろえ)";

		d->host->tell(d, "そろえ指定付箋", s);
		return;
	}
	if ( m->kind == TV_MARK_RUN ) {
		if ( same_word(nd->name, "docmemo") ) {
			say_after(line, sizeof(line), "内容: ", tad_attr(nd, "text"), "(なし)");
			d->host->tell(d, "文章メモ", (CONST char *)line);
		} else if ( same_word(nd->name, "page-number") ) {
			INT	at = 0;

			at = say_attr(line, at, sizeof(line), nd, "step");
			at = say_attr(line, at, sizeof(line), nd, "num");
			d->host->tell(d, "ページ番号", ( at > 0 ) ? (CONST char *)line : "(属性なし)");
		} else {
			say_after(line, sizeof(line), "幅: ", tad_attr(nd, "width"),
				  "(指定なし)");
			d->host->tell(d, "固定幅空白", (CONST char *)line);
		}
		return;
	}
	{
		LOCAL CONST char *CONST names[] = {
			"R", "P", "height", "pargap", "left", "right", "indent",
			"ntabs", "tabs"
		};
		INT	at = 0, i;

		for ( i = 0; i < (INT)( sizeof(names) / sizeof(names[0]) ); i++ ) {
			at = say_attr(line, at, sizeof(line), nd, names[i]);
		}
		d->host->tell(d, "タブ書式指定付箋", ( at > 0 ) ? (CONST char *)line : "(属性なし)");
	}
}

/* 段落間隔: chosen from a list and written into the tab format */
LOCAL void pargap_form( T_DDED *d, T_TADNODE *nd )
{
	LOCAL CONST char *CONST names[] = {
		"1/8 行あき", "1/4 行あき", "3/8 行あき", "1/2 行あき",
		"5/8 行あき", "3/4 行あき (標準)", "7/8 行あき", "1 行あき",
		"1.25 行あき", "1.5 行あき", "1.75 行あき", "2 行あき"
	};
	LOCAL CONST INT values[] = {
		125, 250, 375, 500, 625, 750, 875, 1000, 1250, 1500, 1750, 2000
	};
	CONST UB	*v = tad_attr(nd, "pargap");
	INT		now = ( v != NULL ) ? word_num(v) : 750, i, at = 5;
	UB		num[16];

	for ( i = 0; i < 12; i++ ) {
		if ( values[i] == now ) {
			at = i;
		}
	}
	i = d->host->list_form(d, "段落間隔", names, 12, at);
	if ( i < 0 ) {
		return;
	}
	undo_point(d, FALSE);
	num_word(values[i], 1, num);
	(void)tad_set_attr(rec_of(d), nd, "pargap", num);
	changed(d);
}

/*
 * A press in a text shown in detail, on the mark of a setting: a flag
 * tells what it sets; on a ruler, a marker is taken to be carried, the
 * origin turns between the paper and the line before, and the rest of
 * it chooses the space after the paragraph. A marker pressed twice
 * tells the whole tab format.
 */
EXPORT BOOL de_mark_press( T_DDED *d, INT x, INT y, UD when )
{
	T_DPRECT	page;
	T_TVMARK	m;
	BOOL		twice;
	UD		gap = (UD)d->host->look(d, WM_LOOK_DBLTIME, 400) * 1000000U;

	if ( d == NULL || (*d->doc) == NULL || !(*d->detail_view) || (*d->xml_view)
	  || !de_editable(d) ) {
		return FALSE;
	}
	d->host->work_rect(d, &page);
	if ( tv_doc_mark((*d->doc), &page, (*d->scroll_y), (*d->rec), x, y, &m)
	     == TV_MARK_NONE || m.node == NULL ) {
		return FALSE;
	}
	twice = (BOOL)( m.kind == mk_click_kind
		     && x > mk_click_x - ( d->host->look(d, LK_DBL_W, 10) / 2 ) && x < mk_click_x + ( d->host->look(d, LK_DBL_W, 10) / 2 )
		     && y > mk_click_y - ( d->host->look(d, LK_DBL_W, 10) / 2 ) && y < mk_click_y + ( d->host->look(d, LK_DBL_W, 10) / 2 )
		     && when - mk_click_at < gap );
	mk_click_x = x;
	mk_click_y = y;
	mk_click_at = when;
	mk_click_kind = m.kind;
	switch ( m.kind ) {
	case TV_MARK_ALIGN:
	case TV_MARK_RUN:
		mark_tell(d, &m);
		break;
	case TV_MARK_ORIGIN: {
		CONST UB	*r = tad_attr(m.node, "R");

		undo_point(d, FALSE);
		(void)tad_set_attr(rec_of(d), m.node, "R",
				   (CONST UB *)( ( r != NULL && r[0] == '1' ) ? "0" : "1" ));
		changed(d);
		break;
	}
	case TV_MARK_BAR:
		pargap_form(d, m.node);
		break;
	default:
		if ( twice ) {
			mark_tell(d, &m);
			mk_click_kind = TV_MARK_NONE;
			break;
		}
		mk_win = d;
		mk = m;
		mk_x0 = x;
		mk_moved = FALSE;
		break;
	}

	return TRUE;
}

EXPORT BOOL de_mark_carrying( void )
{
	return (BOOL)( mk_win != NULL );
}

/* The tab stops of a format, with one of them put somewhere else */
LOCAL void tabs_word( CONST UB *old, INT which, INT units, UB *out, INT max,
		      INT *p_n )
{
	INT	n = 0, at = 0;

	while ( old != NULL && *old != 0 ) {
		INT	v = 0, sign = 1;
		UB	num[16];
		INT	i;

		while ( *old == ',' || *old == ' ' ) old++;
		if ( *old == 0 ) break;
		if ( *old == '-' ) { sign = -1; old++; }
		while ( *old >= '0' && *old <= '9' ) v = v * 10 + ( *old++ - '0' );
		while ( *old != 0 && *old != ',' ) old++;
		if ( n == which ) {
			v = units;			/* a decimal stop stays one */
		}
		num_word(sign * v, 0, num);
		if ( n > 0 && at < max - 1 ) out[at++] = ',';
		for ( i = 0; num[i] != 0 && at < max - 1; i++ ) {
			out[at++] = num[i];
		}
		n++;
	}
	out[at] = 0;
	*p_n = n;
}

/* The marker follows the hand, a letter at a time, and the text with it */
EXPORT void de_mark_follow( INT sx, INT sy )
{
	T_DDED		*d = mk_win;
	INT		ox, oy;
	INT		pos, letters, u;
	UB		num[16];
	CONST char	*name;
	T_TAD		*rec;

	(void)sy;
	if ( d == NULL || !(*d->used) || (*d->doc) == NULL || mk.node == NULL
	  || !d->host->origin(d, &ox, &oy) ) {
		mk_win = NULL;
		return;
	}
	u = ( mk.unit > 0 ) ? mk.unit : CHAR_PX;
	pos = mk.at + ( sx - ox ) - mk_x0;
	switch ( mk.kind ) {
	case TV_MARK_LHEAD:	letters = pos - mk.zero;	name = "left";	 break;
	case TV_MARK_LEND:	letters = mk.end - pos;		name = "right";	 break;
	case TV_MARK_FHEAD:	letters = pos - mk.head;	name = "indent"; break;
	default:		letters = pos - mk.head;	name = "tabs";	 break;
	}
	letters = ( letters + u / 2 ) / u;
	if ( letters < 0 ) {
		letters = 0;
	}
	rec = rec_of(d);
	if ( mk.kind == TV_MARK_TAB ) {
		UB	tabs[128], was[128];
		INT	n;
		CONST UB *old = tad_attr(mk.node, "tabs");

		tabs_word(old, mk.index, to_units(d, letters * CHAR_PX), tabs,
			  sizeof(tabs), &n);
		was[0] = 0;
		if ( old != NULL ) {
			INT	i;

			for ( i = 0; old[i] != 0 && i < (INT)sizeof(was) - 1; i++ ) {
				was[i] = old[i];
			}
			was[i] = 0;
		}
		if ( same_word(was, (CONST char *)tabs) ) {
			return;
		}
		if ( !mk_moved ) {
			undo_point(d, FALSE);
			mk_moved = TRUE;
		}
		(void)tad_set_attr(rec, mk.node, "tabs", tabs);
		num_word(n, 0, num);
		(void)tad_set_attr(rec, mk.node, "ntabs", num);
	} else {
		CONST UB	*old = tad_attr(mk.node, name);

		num_word(to_units(d, letters * CHAR_PX), 0, num);
		if ( old != NULL && same_word(old, (CONST char *)num) ) {
			return;
		}
		if ( old == NULL && letters == 0 ) {
			return;
		}
		if ( !mk_moved ) {
			undo_point(d, FALSE);
			mk_moved = TRUE;
		}
		(void)tad_set_attr(rec, mk.node, name, num);
	}
	changed(d);
}

EXPORT void de_mark_release( void )
{
	mk_win = NULL;
}

/* ---------------------------------------------------------------- showing */

/*
 * The caret's box as thick as ユーザ環境設定 says (細 中 太): a bar down
 * the line across, across the line down (縦書き).
 */
LOCAL void caret_thick( CONST T_DDED *d, T_DPRECT *r )
{
	INT	w = d->host->look((T_DDED *)d, LK_CARET_W, 1) + 1;

	if ( (*d->doc) != NULL && (*d->doc)->vert ) {
		r->bottom = r->top + w;
	} else {
		r->right = r->left + w;
	}
}

/*
 * Where the caret is drawn in the window's work area, whether it shows
 * or not: FALSE when there is none to blink (nothing typed into, or a
 * selection shown instead). A blink draws the page again inside this
 * alone, and only this goes to the screen.
 */
EXPORT BOOL de_caret_rect( T_DDED *d, T_DPRECT *r )
{
	T_DPRECT	page;

	if ( d == NULL || d->framed || !de_editable(d) || has_sel(d) ) {
		return FALSE;
	}
	clamp(d);
	d->host->work_rect(d, &page);
	if ( tv_doc_caret_box((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara, d->cpos, r) < E_OK ) {
		return FALSE;
	}
	caret_thick(d, r);
	return (BOOL)( r->right > r->left && r->bottom > r->top );
}

/*
 * The lines the caret and the selection are on, across the page (down
 * it, written downwards), with a little room either way for the frame
 * of a thing picked: what de_draw draws over the page is inside it, and
 * what the converter is working on is on the caret's line.
 */
LOCAL BOOL ink_of( T_DDED *d, CONST T_DPRECT *page, T_DPRECT *out )
{
	T_DPRECT	ba, bb;
	INT		pa, a, pb, b;

	sel_order(d, &pa, &a, &pb, &b);
	if ( tv_doc_caret_box((*d->doc), page, (*d->scroll_y), (*d->rec), pa, a, &ba) < E_OK ) {
		return FALSE;
	}
	if ( !has_sel(d) ) {
		bb = ba;
	} else if ( tv_doc_caret_box((*d->doc), page, (*d->scroll_y), (*d->rec), pb, b, &bb) < E_OK ) {
		return FALSE;
	}
	if ( (*d->doc)->vert ) {
		out->left = ( ( ba.left < bb.left ) ? ba.left : bb.left ) - 4;
		out->right = ( ( ba.right > bb.right ) ? ba.right : bb.right ) + 4;
		out->top = -0x4000;
		out->bottom = 0x4000;
	} else {
		out->top = ( ( ba.top < bb.top ) ? ba.top : bb.top ) - 4;
		out->bottom = ( ( ba.bottom > bb.bottom ) ? ba.bottom : bb.bottom ) + 4;
		out->left = -0x4000;
		out->right = 0x4000;
	}

	return TRUE;
}

/* The selection turned over and the caret drawn, over the page as drawn */
EXPORT void de_draw( T_DDED *d, INT gid )
{
	T_DPRECT	page, box;

	if ( !de_editable(d) || gid < 0 ) {
		return;
	}
	clamp(d);
	d->host->work_rect(d, &page);
	/* where it is drawn, for a change drawn again in part to draw it again there */
	d->inked = ink_of(d, &page, &d->ink);
	if ( has_sel(d) ) {
		INT	pa, a, pb, b;

		/* one thing picked shows by its frame, words by turning over */
		if ( !thing_marks(d, gid) ) {
			sel_order(d, &pa, &a, &pb, &b);
			tv_doc_select(gid, (*d->doc), &page, (*d->scroll_y), (*d->rec),
				      pa, a, pb, b);
		}
		d->host->ime_draw(d, gid);
		return;
	}
	/* only the window typed into shows the caret */
	if ( d->host->caret_on(d)
	  && tv_doc_caret_box((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara,
			      d->cpos, &box) >= E_OK ) {
		/* as thick as ユーザ環境設定 says: 細 中 太 */
		caret_thick(d, &box);
		dp_fill_rect(gid, &box, d->host->ime_on(d) ? 0x00D00000U : 0x00000000U);
	}
	/* what the converter is working on, over the text at the caret */
	d->host->ime_draw(d, gid);
}

/*
 * The page wound so that the caret can be seen: a caret above the page
 * brings its line to the top, one below brings it to the bottom.
 */
EXPORT void de_show_caret( T_DDED *d )
{
	INT	to = caret_to(d);

	if ( to != (*d->scroll_y) ) {
		(*d->scroll_y) = to;
		d->host->draw(d);
	}
}

/* Where the page is to be wound to for the caret to be seen; where it is when it is seen */
LOCAL INT caret_to( T_DDED *d )
{
	T_DPRECT	page, box;
	INT		to = (*d->scroll_y);

	if ( !de_editable(d) || d->framed ) {
		return to;		/* a piece of text in a figure does not wind */
	}
	d->host->work_rect(d, &page);
	if ( tv_doc_caret_box((*d->doc), &page, (*d->scroll_y), (*d->rec), d->cpara,
			      d->cpos, &box) < E_OK ) {
		return to;
	}
	if ( (*d->doc)->vert ) {
		/*
		 * 縦書き: the text is wound to the left as the scroll grows,
		 * a caret past the left edge winds it on, one past the right
		 * edge winds it back.
		 */
		if ( box.left < page.left ) {
			to += page.left - box.left;
		} else if ( box.right > page.right ) {
			to -= box.right - page.right;
		}
	} else if ( box.top < page.top ) {
		to -= page.top - box.top;
	} else if ( box.bottom > page.bottom ) {
		to += box.bottom - page.bottom;
	}
	if ( to < 0 ) {
		to = 0;
	}

	return to;
}

/* ---------------------------------------------------------------- links in the text */

/*
 * The link just added -- a link is added at the end of the record --
 * moved into the text right after the link 'orig'.
 */
EXPORT void de_link_after( T_DDED *d, CONST TS_UUID *orig )
{
	T_TAD		*rec = (*d->rec);
	T_TADNODE	*was = NULL, *now = NULL;
	T_VOBJ		v;
	INT		i, n;

	d->host->model(d);
	n = tad_lnk_count(rec);
	if ( (*d->doc) == NULL || n < 1 ) {
		return;
	}
	for ( i = 0; i < (*d->doc)->nrun; i++ ) {
		CONST T_TVRUN	*r = &(*d->doc)->run[i];

		if ( r->kind != TV_RUN_LINK ) {
			continue;
		}
		if ( r->link == n - 1 ) {
			now = r->node;
		} else if ( tad_lnk_get(rec, r->link, &v) >= E_OK
			 && ts_uuid_cmp(&v.vobjid, orig) == 0 ) {
			was = r->node;
		}
	}
	if ( was != NULL && now != NULL && was != now ) {
		tad_node_move(now, was->parent, was->next);
	}
}

/* The link just added moved to a place in the text */
LOCAL void link_to( T_DDED *d, INT pi, INT pos )
{
	T_TADNODE	*now = NULL, *mark;
	INT		i, n = tad_lnk_count((*d->rec));

	d->host->model(d);
	for ( i = 0; (*d->doc) != NULL && i < (*d->doc)->nrun; i++ ) {
		if ( (*d->doc)->run[i].kind == TV_RUN_LINK
		  && (*d->doc)->run[i].link == n - 1 ) {
			now = (*d->doc)->run[i].node;
		}
	}
	if ( now == NULL ) {
		return;
	}
	/* an empty element marks the place, and the link takes its place */
	mark = put_elem(d, pi, pos, "tab");
	if ( mark != NULL && mark != now ) {
		tad_node_move(now, mark->parent, mark);
		tad_node_remove(mark);
	}
}

/*
 * A thing standing in the text -- a link or a picture -- worked by the
 * hand. Pressed and let go, it is picked, as one place of the text.
 * Carried, its outline follows the pointer over every window, and where
 * it is let go it goes: to that place in this text, to a place in
 * another text, or into a figure or a cabinet. Its right edge or its
 * foot carried sizes it: a link taller than its name opens to show what
 * it points at, and a picture keeps its proportions.
 */
#define CARRY_START	4		/* how far the hand goes before it carries */
#define THING_EDGE	5		/* how near an edge counts as on it */
#define LINK_MIN_W	50

LOCAL T_DDED	*lc_win = NULL;
LOCAL INT	lc_run;			/* which run of the text */
LOCAL UINT	lc_kind;		/* TV_RUN_LINK or TV_RUN_IMAGE */
LOCAL INT	lc_link;		/* a link's number in the record */
LOCAL T_DPRECT	lc_box;			/* where it was drawn, in its window */
LOCAL T_DPRECT	lc_now;			/* where it is being sized to */
LOCAL INT	lc_x0, lc_y0;		/* where the hand went down, likewise */
LOCAL BOOL	lc_moved;
LOCAL BOOL	lc_copy;		/* 複写ドラッグ: carried as a copy */
LOCAL BOOL	lc_fixed;		/* a link that is 固定化 or 背景化: only copied */
LOCAL UINT	lc_size;		/* sizing: 1 the right edge, 2 the foot */

/*
 * A thing of the text under a place taken up by the hand; FALSE if
 * none. A finger (slop, wm_hit_slop) reaches its edges farther in, and
 * the one thing picked by its edges from slop out of it.
 */
LOCAL BOOL thing_take( T_DDED *d, INT x, INT y, INT slop )
{
	T_DPRECT	page, box;
	INT		run;

	if ( d == NULL || (*d->doc) == NULL || (*d->sealed) || (*d->xml_view) ) {
		return FALSE;
	}
	d->host->work_rect(d, &page);
	run = tv_doc_thing((*d->doc), &page, (*d->scroll_y), (*d->rec), x, y, -1, &box);
	if ( ( run < 0 || run >= (*d->doc)->nrun ) && slop > 0 && thing_picked(d, &run, &box)
	  && (*d->doc)->run[run].kind != TV_RUN_FIGURE
	  && x >= box.left && x < box.right + slop && y >= box.top && y < box.bottom + slop
	  && ( x >= box.right || y >= box.bottom ) ) {
		/* by the picked one's handle: it is sized, however far out */
		lc_win = d;
		lc_run = run;
		lc_kind = (*d->doc)->run[run].kind;
		lc_link = (*d->doc)->run[run].link;
		lc_box = box;
		lc_now = box;
		lc_x0 = x;
		lc_y0 = y;
		lc_moved = FALSE;
		lc_copy = FALSE;
		lc_fixed = run_protected(d, &(*d->doc)->run[run]);
		lc_size = 0;
		if ( !lc_fixed ) {
			if ( x >= box.right - wm_hit_in(THING_EDGE, slop, box.right - box.left) ) lc_size |= 1;
			if ( y >= box.bottom - wm_hit_in(THING_EDGE, slop, box.bottom - box.top) ) lc_size |= 2;
		}
		return TRUE;
	}
	if ( run < 0 || run >= (*d->doc)->nrun ) {
		return FALSE;
	}
	{
		INT		pr;
		T_DPRECT	pb;

		/* farther in only on the one picked before, whose handle is shown */
		if ( slop > 0 && !( thing_picked(d, &pr, &pb) && pr == run ) ) slop = 0;
	}
	lc_win = d;
	lc_run = run;
	lc_kind = (*d->doc)->run[run].kind;
	lc_link = (*d->doc)->run[run].link;
	lc_box = box;
	lc_now = box;
	lc_x0 = x;
	lc_y0 = y;
	lc_moved = FALSE;
	lc_copy = FALSE;
	lc_size = 0;
	lc_fixed = run_protected(d, &(*d->doc)->run[run]);
	if ( lc_kind != TV_RUN_FIGURE && !lc_fixed ) {
		if ( x >= box.right - wm_hit_in(THING_EDGE, slop, box.right - box.left) ) lc_size |= 1;
		if ( y >= box.bottom - wm_hit_in(THING_EDGE, slop, box.bottom - box.top) ) lc_size |= 2;
	}

	return TRUE;
}

EXPORT void de_link_take( T_DDED *d, INT which, CONST T_DPRECT *box, INT x, INT y, UD when )
{
	(void)which;
	(void)box;
	(void)thing_take(d, x, y, wm_hit_slop(when));
}

EXPORT BOOL de_link_carrying( void )
{
	return (BOOL)( lc_win != NULL );
}

EXPORT void de_link_follow( INT sx, INT sy )
{
	INT		ox, oy;
	T_DPRECT	r;
	INT		x, y;
	BOOL		first;

	if ( lc_win == NULL || !lc_win->host->origin(lc_win, &ox, &oy) ) {
		return;
	}
	x = sx - ox;
	y = sy - oy;
	if ( !lc_moved && x - lc_x0 < CARRY_START && lc_x0 - x < CARRY_START
	  && y - lc_y0 < CARRY_START && lc_y0 - y < CARRY_START ) {
		return;			/* a shaky hand, not a carry */
	}
	if ( lc_fixed && !lc_copy ) {
		return;			/* protected: it stays where it is */
	}
	first = (BOOL)!lc_moved;
	lc_moved = TRUE;
	if ( lc_size != 0 ) {
		INT	bw = lc_box.right - lc_box.left, bh = lc_box.bottom - lc_box.top;

		lc_now = lc_box;
		if ( lc_size & 1 ) {
			lc_now.right = lc_box.right + ( x - lc_x0 );
		}
		if ( lc_size & 2 ) {
			lc_now.bottom = lc_box.bottom + ( y - lc_y0 );
		}
		if ( lc_kind == TV_RUN_IMAGE && bw > 0 && bh > 0 ) {
			/* a picture keeps its proportions */
			if ( lc_size & 1 ) {
				lc_now.bottom = lc_now.top + ( lc_now.right - lc_now.left ) * bh / bw;
			} else {
				lc_now.right = lc_now.left + ( lc_now.bottom - lc_now.top ) * bw / bh;
			}
		}
		if ( lc_now.right - lc_now.left < ( lc_kind == TV_RUN_LINK ? LINK_MIN_W : 8 ) ) {
			lc_now.right = lc_now.left + ( lc_kind == TV_RUN_LINK ? LINK_MIN_W : 8 );
		}
		{
			/* a link is never lower than it is closed */
			T_VOBJ	v;
			INT	least = 8;

			if ( lc_kind == TV_RUN_LINK
			  && tad_lnk_get((*lc_win->rec), lc_link, &v) >= E_OK
			  && tad_lnk_closed_h(&v) > least ) {
				least = tad_lnk_closed_h(&v);
			}
			if ( lc_now.bottom - lc_now.top < least ) {
				lc_now.bottom = lc_now.top + least;
			}
		}
		r = lc_now;
	} else {
		r.left = lc_box.left + ( x - lc_x0 );
		r.top = lc_box.top + ( y - lc_y0 );
		r.right = r.left + ( lc_box.right - lc_box.left );
		r.bottom = r.top + ( lc_box.bottom - lc_box.top );
	}
	r.left += ox;  r.right += ox;
	r.top += oy;    r.bottom += oy;
	lc_win->host->drag(lc_win, &r, (BOOL)( first && lc_size == 0 ), x - lc_x0, y - lc_y0);
	lc_win->host->update(lc_win);
}

/* The place in a text under a place on the screen */
LOCAL BOOL text_place( T_DDED *t, INT sx, INT sy, INT *pi, INT *pos )
{
	INT		ox, oy;
	T_DPRECT	page;

	if ( (*t->doc) == NULL || !t->host->origin(t, &ox, &oy) ) {
		return FALSE;
	}
	t->host->work_rect(t, &page);

	return (BOOL)( tv_doc_caret_at((*t->doc), &page, (*t->scroll_y), (*t->rec),
				       sx - ox, sy - oy, pi, pos) >= E_OK );
}

/* Where a run is: its paragraph and its place in it */
LOCAL BOOL run_place( T_DDED *d, INT run, INT *p_pi, INT *p_pos )
{
	INT	p, i, acc;

	for ( p = 0; (*d->doc) != NULL && p < (*d->doc)->npara; p++ ) {
		CONST T_TVPARA	*pa = &(*d->doc)->para[p];

		if ( run < pa->first || run >= pa->first + pa->n ) {
			continue;
		}
		acc = 0;
		for ( i = pa->first; i < run; i++ ) {
			acc += run_units(&(*d->doc)->run[i]);
		}
		*p_pi = p;
		*p_pos = acc;
		return TRUE;
	}

	return FALSE;
}

/*
 * A link put into a text at the place under a place on the screen: what
 * a template let go over a text makes. Answers below E_OK when there is
 * no place there.
 */
EXPORT ER de_link_drop( T_DDED *t, INT sx, INT sy, CONST T_VOBJ *v )
{
	TS_UUID	id;
	INT	pi = 0, pos = 0;
	ER	er;

	if ( t == NULL || (*t->doc) == NULL || (*t->sealed) || (*t->xml_view) ) {
		return E_PAR;
	}
	if ( !text_place(t, sx, sy, &pi, &pos) ) {
		pi = ( (*t->doc)->npara > 0 ) ? (*t->doc)->npara - 1 : 0;
		pos = ( (*t->doc)->npara > 0 ) ? para_len(t, pi) : 0;
	}
	t->host->before(t);
	er = tad_lnk_add((T_TAD *)(*t->rec), v, &id);
	if ( er >= E_OK ) {
		link_to(t, pi, pos);
		changed(t);
	}
	return er;
}

EXPORT void de_link_copy( void )
{
	if ( lc_win != NULL && lc_size == 0 ) {
		lc_copy = TRUE;
	}
}

/*
 * A thing of the text (node, a TV_RUN_* kind; v the link's when it is
 * one) carried to another place in the same text, where the hand is at
 * sx, sy: moved there, or a copy put there and the thing left where it
 * was. It stays picked at the place it went to.
 */
LOCAL void thing_here( T_DDED *d, INT sx, INT sy, T_TADNODE *node, UINT kind, T_VOBJ *v, BOOL copy )
{
	T_TADNODE	*mark;
	INT		pi, pos;

	if ( !text_place(d, sx, sy, &pi, &pos) ) {
		return;
	}
	undo_point(d, FALSE);
	mark = put_elem(d, pi, pos, "tab");
	if ( mark != NULL && mark != node && copy ) {
		/* a copy put there; the thing stays where it was */
		T_TADNODE	*c = NULL;

		if ( kind == TV_RUN_LINK ) {
			TS_UUID	id;

			knl_memset(&v->vobjid, 0, sizeof(v->vobjid));
			if ( tad_lnk_add((T_TAD *)(*d->rec), v, &id) >= E_OK ) {
				link_to(d, pi, pos);
			}
			tad_node_remove(mark);
			mark = NULL;
		} else {
			c = tad_node_copy(rec_of(d), mark->parent, mark, node);
		}
		if ( mark != NULL ) {
			tad_node_remove(mark);
		}
		node = c;
	} else if ( mark != NULL && mark != node ) {
		tad_node_move(node, mark->parent, mark);
		tad_node_remove(mark);
	}
	changed(d);
	/* the thing stays picked, at the place it went to */
	d->apara = d->cpara = pi;
	d->apos = d->cpos = pos;
	if ( (*d->doc) != NULL ) {
		INT	i;

		for ( i = 0; i < (*d->doc)->nrun; i++ ) {
			if ( (*d->doc)->run[i].node == node
			  && run_place(d, i, &pi, &pos) ) {
				d->apara = d->cpara = pi;
				d->apos = pos;
				d->cpos = pos + 1;
				break;
			}
		}
	}
	d->host->draw(d);
	shown(d);
}

/*
 * A thing carried out of this text's window and let go back over it,
 * the hand at sx, sy: moved, or copied, within it as when it had not
 * gone out (a program's window, which the desktop carried it on over)
 */
EXPORT void de_thing_back( T_DDED *d, INT sx, INT sy, CONST T_DDTHING *th )
{
	T_VOBJ	v = th->v;

	if ( !(*d->used) || (*d->doc) == NULL || th->node == NULL || (*d->sealed) ) {
		d->host->update(d);
		return;
	}
	thing_here(d, sx, sy, th->node, th->kind, &v, th->copy);
}

EXPORT void de_link_release( INT sx, INT sy )
{
	T_DDED		*d = lc_win, *t = NULL;
	T_TADNODE	*node;
	T_VOBJ		v;
	T_DDTHING	th;
	INT		pi, pos;
	UINT		at;

	lc_win = NULL;
	if ( d == NULL ) {
		return;
	}
	d->host->drag(d, NULL, FALSE, 0, 0);
	if ( !(*d->used) || (*d->doc) == NULL || lc_run >= (*d->doc)->nrun ) {
		d->host->update(d);
		return;
	}
	node = (*d->doc)->run[lc_run].node;
	if ( lc_fixed && !lc_copy ) {
		lc_moved = FALSE;	/* protected: a press on it only picks it */
	}
	if ( !lc_moved ) {
		/* pressed and let go: picked, as one thing of the text */
		if ( run_place(d, lc_run, &pi, &pos) ) {
			d->apara = d->cpara = pi;
			d->apos = pos;
			d->cpos = pos + 1;
			d->host->draw(d);
			shown(d);
		}
		return;
	}
	if ( lc_size != 0 ) {
		/* sized: a link to the size of its box, a picture likewise */
		INT	w = lc_now.right - lc_now.left, h = lc_now.bottom - lc_now.top;

		undo_point(d, FALSE);
		if ( lc_kind == TV_RUN_LINK && tad_lnk_get((*d->rec), lc_link, &v) >= E_OK ) {
			v.width = w;
			/* closed at the closed height; opened only when well past it */
			v.heightpx = tad_lnk_fit_h(&v, h);
			if ( tad_lnk_set((T_TAD *)(*d->rec), &v) < E_OK ) {
				CONST char	*head = "the link \"", *tail = "\" was not sized";
				char		say[TAD_NAME_MAX + 32];
				INT		k = 0, i;

				for ( i = 0; head[i] != 0; i++ ) say[k++] = head[i];
				for ( i = 0; v.name[i] != 0 && i < TAD_NAME_MAX; i++ ) say[k++] = (char)v.name[i];
				for ( i = 0; tail[i] != 0; i++ ) say[k++] = tail[i];
				say[k] = 0;
				d->host->note(d, say);
			}
		} else if ( lc_kind == TV_RUN_IMAGE && node != NULL ) {
			T_TAD	*rec = rec_of(d);
			UB	num[16];

			num_word(0, 0, num);
			(void)tad_set_attr(rec, node, "left", num);
			(void)tad_set_attr(rec, node, "top", num);
			num_word(w, 0, num);
			(void)tad_set_attr(rec, node, "right", num);
			num_word(h, 0, num);
			(void)tad_set_attr(rec, node, "bottom", num);
		}
		changed(d);
		return;
	}
	if ( lc_kind == TV_RUN_LINK && tad_lnk_get((*d->rec), lc_link, &v) < E_OK ) {
		d->host->update(d);
		return;
	}
	at = d->host->at(d, sx, sy, &t);
	if ( at != DE_AT_TEXT ) {
		/* to whatever else is there, which takes it as it does */
		knl_memset(&th, 0, sizeof(th));
		th.kind = lc_kind;
		if ( lc_kind == TV_RUN_LINK ) {
			th.v = v;
		}
		th.node = node;
		th.box = lc_box;
		th.x0 = lc_x0;
		th.y0 = lc_y0;
		th.copy = lc_copy;
		if ( node != NULL && d->host->thing_away(d, sx, sy, &th) ) {
			changed(d);
			d->host->draw_all(d);
		}
		d->host->update(d);
		return;
	}
	if ( node == NULL || (*t->sealed) ) {
		d->host->update(d);
		return;
	}
	if ( t == d ) {
		thing_here(d, sx, sy, node, lc_kind, &v, lc_copy);
		return;
	}
	/* an object is not put inside itself */
	if ( lc_kind == TV_RUN_LINK && ts_uuid_cmp(&v.target, t->id) == 0 ) {
		d->host->update(d);
		return;
	}
	d->host->focus(d, t);
	if ( (*t->kind) != TV_KIND_DOC || (*t->xml_view) ) {
		d->host->update(d);
		return;
	}
	/* into another text, where it was let go */
	if ( !text_place(t, sx, sy, &pi, &pos) ) {
		return;
	}
	t->host->before(t);
	d->host->before(d);
	if ( lc_kind == TV_RUN_LINK ) {
		TS_UUID	id, was = v.vobjid;

		knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
		if ( tad_lnk_add((T_TAD *)(*t->rec), &v, &id) >= E_OK ) {
			link_to(t, pi, pos);
			if ( !lc_copy ) {
				(void)tad_lnk_del((T_TAD *)(*d->rec), &was);
			}
		}
	} else {
		T_TADNODE	*mark = put_elem(t, pi, pos, "tab");

		if ( mark != NULL
		  && tad_node_copy((T_TAD *)(*t->rec), mark->parent, mark, node) != NULL ) {
			tad_node_remove(mark);
			if ( !lc_copy ) {
				tad_node_remove(node);
			}
		}
	}
	changed(t);
	changed(d);
}

/* A thing picked alone: its frame, and the handle it is sized by */
/* The one thing picked (the selection is that run alone): which run, and its box; FALSE if none */
LOCAL BOOL thing_picked( T_DDED *d, INT *p_run, T_DPRECT *p_box )
{
	INT		pa, a, pb, b, i, acc = 0, run = -1;
	T_DPRECT	page;
	CONST T_TVPARA	*p;

	sel_order(d, &pa, &a, &pb, &b);
	if ( pa != pb || b != a + 1 ) {
		return FALSE;
	}
	p = &(*d->doc)->para[pa];
	for ( i = p->first; i < p->first + p->n && i < (*d->doc)->nrun; i++ ) {
		if ( acc == a ) {
			run = i;
			break;
		}
		acc += run_units(&(*d->doc)->run[i]);
	}
	if ( run < 0 || ( (*d->doc)->run[run].kind != TV_RUN_LINK
			  && (*d->doc)->run[run].kind != TV_RUN_IMAGE
			  && (*d->doc)->run[run].kind != TV_RUN_FIGURE ) ) {
		return FALSE;
	}
	d->host->work_rect(d, &page);
	if ( tv_doc_thing((*d->doc), &page, (*d->scroll_y), (*d->rec), 0, 0, run, p_box) < 0 ) {
		return FALSE;
	}
	*p_run = run;
	return TRUE;
}

/*
 * The link that is picked alone (the selection is that link and nothing
 * else), in *v: what the system's items of the menu are about wherever
 * the menu is opened. FALSE when no link is picked so.
 */
EXPORT BOOL de_link_picked( T_DDED *d, T_VOBJ *v )
{
	INT		run;
	T_DPRECT	box;

	if ( d == NULL || v == NULL || (*d->doc) == NULL || (*d->rec) == NULL || (*d->xml_view)
	  || !thing_picked(d, &run, &box) ) {
		return FALSE;
	}
	return (BOOL)( (*d->doc)->run[run].kind == TV_RUN_LINK
		    && tad_lnk_get((*d->rec), (*d->doc)->run[run].link, v) >= E_OK );
}

LOCAL BOOL thing_marks( T_DDED *d, INT gid )
{
	INT		run;
	T_DPRECT	box, h;

	if ( !thing_picked(d, &run, &box) ) {
		return FALSE;
	}
	box.left -= 2;  box.top -= 2;  box.right += 2;  box.bottom += 2;
	dp_frame_rect(gid, &box, 0x00FF8F00U, 2);
	if ( (*d->doc)->run[run].kind == TV_RUN_FIGURE ) {
		return TRUE;			/* a figure keeps its size */
	}
	h.left = box.right - 5;  h.top = box.bottom - 5;
	h.right = h.left + 10;  h.bottom = h.top + 10;
	dp_fill_rect(gid, &h, 0x00FF8F00U);
	dp_frame_rect(gid, &h, 0x00FFFFFFU, 1);
	return TRUE;
}


/*
 * 仮身化: what is picked made into a real object of its own, and a link
 * to it put where it was. The new object is the whole document with
 * everything outside the selection cut away, so that it keeps the
 * paper, the format and the look the words had.
 */
/*
 * A text edited where nobody sees it: a record of its own and the model
 * made from it, for cutting a copy down to what is wanted
 */
typedef struct {
	T_DDED		e;
	T_TAD		*rec;
	T_TVDOC		*doc;
	T_TAD		*xrec;
	INT		scroll_x, scroll_y, height;
	UINT		kind;
	BOOL		xml_view, detail_view, dirty, sealed, used, settling;
	TS_UUID		id;
	INT		recno;
} T_SCRATCH;

LOCAL void sc_model( T_DDED *e )
{
	T_SCRATCH	*x = (T_SCRATCH *)e->arg;
	T_TVDOC		*was = x->doc;

	x->doc = NULL;
	(void)tv_doc_again(x->rec, was, &x->doc);
	if ( was != NULL ) {
		tv_doc_free(was);
	}
}

LOCAL void sc_none( T_DDED *e )
{
	(void)e;
}

LOCAL CONST T_DDHOST	scratch_host = {
	.model = sc_model, .changed = sc_none, .draw = sc_none, .before = sc_none,
	.forget = sc_none,
};

LOCAL T_DDED *scratch_new( T_TAD *rec )
{
	T_SCRATCH	*x = (T_SCRATCH *)Kmalloc(sizeof(T_SCRATCH));

	if ( x == NULL ) {
		return NULL;
	}
	knl_memset(x, 0, sizeof(*x));
	x->rec = rec;
	x->kind = TV_KIND_DOC;
	x->used = TRUE;
	x->e.rec = &x->rec;		x->e.doc = &x->doc;
	x->e.xrec = &x->xrec;		x->e.scroll_x = &x->scroll_x;
	x->e.scroll_y = &x->scroll_y;	x->e.height = &x->height;
	x->e.kind = &x->kind;		x->e.xml_view = &x->xml_view;
	x->e.detail_view = &x->detail_view; x->e.dirty = &x->dirty;
	x->e.sealed = &x->sealed;	x->e.used = &x->used;
	x->e.settling = &x->settling;	x->e.id = &x->id;
	x->e.recno = &x->recno;
	x->e.host = &scratch_host;
	x->e.arg = x;
	return &x->e;
}

LOCAL void scratch_free( T_DDED *e )
{
	T_SCRATCH	*x = (T_SCRATCH *)e->arg;

	if ( x->doc != NULL ) {
		tv_doc_free(x->doc);
	}
	Kfree(x);
}

LOCAL void virtualize( T_DDED *d )
{
	UB		start[WM_LABEL_MAX], name[WM_LABEL_MAX];
	TS_UUID		nid, vid;
	T_TAD		*nrec;
	T_DDED		*f;
	T_VOBJ		c;
	INT		pa, a, pb, b, i, k, n, chars = 0;

	if ( !has_sel(d) || (*d->xml_view) ) {
		return;
	}
	/* the name offered: the first ten letters picked */
	n = sel_text(d, start, WM_LABEL_MAX - 1);
	for ( i = 0, k = 0; i < n && chars < 10; ) {
		INT	w = ( start[i] < 0x80 ) ? 1 : ( start[i] < 0xE0 ) ? 2
			  : ( start[i] < 0xF0 ) ? 3 : 4;

		if ( start[i] == 0x0A || start[i] == 0x0D ) {
			i++;
			continue;
		}
		while ( w-- > 0 && i < n ) {
			name[k++] = start[i++];
		}
		chars++;
	}
	name[k] = 0;
	for ( i = 0; i <= k; i++ ) {
		start[i] = name[i];
	}
	if ( !d->host->ask_name(d, "新しい実身の名称を入力してください", start, name,
			  WM_LABEL_MAX) || name[0] == 0 ) {
		return;
	}
	/* the whole of this document into it */
	if ( d->host->new_object(d, name, &nid, &nrec) < E_OK || nrec == NULL ) {
		return;
	}

	/* and everything but the selection cut out of it */
	sel_order(d, &pa, &a, &pb, &b);
	f = scratch_new(nrec);
	if ( f == NULL ) {
		d->host->object_done(d, &nid, name);
		return;
	}
	f->host->model(f);
	if ( (*f->doc) != NULL && pb < (*f->doc)->npara ) {
		cut_within(f, pb, b, para_len(f, pb));
		for ( i = (*f->doc)->npara - 1; i > pb; i-- ) {
			if ( (*f->doc)->para[i].node != NULL ) {
				tad_node_remove((*f->doc)->para[i].node);
			}
		}
		f->host->model(f);
		cut_within(f, pa, 0, a);
		for ( i = pa - 1; i >= 0; i-- ) {
			if ( (*f->doc)->para[i].node != NULL ) {
				tad_node_remove((*f->doc)->para[i].node);
			}
		}
	}
	scratch_free(f);
	/* kept, and its record calls itself by the new name, not the text it came out of */
	d->host->object_done(d, &nid, name);

	/* in this one, the selection given way to a link to it */
	undo_point(d, FALSE);
	cut_sel(d);
	knl_memset(&c, 0, sizeof(c));
	c.target = nid;
	c.chsz = 14;
	c.frcol = 0x000000;
	c.chcol = 0x000000;
	c.tbcol = 0xFFFFFF;
	c.bgcol = 0xFFFFFF;
	c.disp = TAD_D_DEFAULT;
	c.zoom = 100;
	if ( tad_lnk_add((*d->rec), &c, &vid) >= E_OK ) {
		link_to(d, d->cpara, d->cpos);
		d->cpos++;
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* ---------------------------------------------------------------- the views */

/* The document made again and drawn, after the way it is shown changed */
EXPORT void de_view( T_DDED *d )
{
	d->host->model(d);
	clamp(d);
	d->host->draw(d);
	shown(d);
}

/*
 * 原稿: the record as it is written, as text, one paragraph for each
 * line of it. The text is a record of its own, edited with everything
 * text is edited with, and read back into the object on leaving.
 */
EXPORT void de_xml_enter( T_DDED *d )
{
	UB		*src, *out;
	SZ		len = 0, cap, k = 0, i;
	T_TADLIM	lim;
	T_TAD		*x = NULL;
	INT		lines = 1;
	CONST char	*head = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><tab-format height=\"0.2\" pargap=\"0\"/><p>", *tail = "</p></document></tad>";

	if ( (*d->xml_view) || (*d->rec) == NULL || (*d->kind) != TV_KIND_DOC ) {
		return;
	}
	(void)tad_write_mem((*d->rec), NULL, 0, &len);
	if ( len == 0 ) {
		return;
	}
	src = (UB *)Kmalloc(len + 1);
	if ( src == NULL ) {
		return;
	}
	if ( tad_write_mem((*d->rec), src, len + 1, &len) < E_OK ) {
		Kfree(src);
		return;
	}
	cap = len * 6 + 64;
	out = (UB *)Kmalloc(cap);
	if ( out == NULL ) {
		Kfree(src);
		return;
	}
	for ( i = 0; head[i] != 0; i++ ) {
		out[k++] = (UB)head[i];
	}
	for ( i = 0; i < len; i++ ) {
		CONST char	*e = NULL;
		UB		c = src[i];

		if ( c == 0x0D || c == 0 ) {
			continue;
		}
		if ( c == 0x0A ) {
			e = "</p><p>";
			lines++;
		} else if ( c == '&' ) {
			e = "&amp;";
		} else if ( c == '<' ) {
			e = "&lt;";
		} else if ( c == '>' ) {
			e = "&gt;";
		}
		if ( e == NULL ) {
			out[k++] = c;
		} else {
			while ( *e != 0 ) {
				out[k++] = (UB)*e++;
			}
		}
	}
	for ( i = 0; tail[i] != 0; i++ ) {
		out[k++] = (UB)tail[i];
	}
	Kfree(src);
	knl_memset(&lim, 0, sizeof(lim));
	lim.nodes = lines * 3 + 64;
	lim.docsize = k + 16;
	if ( tad_parse(out, k, &lim, &x) < E_OK ) {
		x = NULL;
	}
	Kfree(out);
	if ( x == NULL ) {
		return;
	}
	(*d->xrec) = x;
	(*d->xml_view) = TRUE;
	d->xml_edited = FALSE;
	d->cpara = d->cpos = d->apara = d->apos = 0;
	(*d->scroll_y) = 0;
	de_view(d);
}

/*
 * What 原稿 holds, read back into the object: every line's text as it
 * is in the record, the lines joined again. Text that does not read as
 * a document is not put back, and the person is told.
 */
EXPORT BOOL de_xml_commit( T_DDED *d )
{
	T_TADNODE	*body, *p, *t;
	UB		*buf;
	SZ		n = 0, cap = 1;
	T_TAD		*chk = NULL, *rec;
	INT		lines = 0;

	if ( !(*d->xml_view) || (*d->xrec) == NULL || !d->xml_edited ) {
		return TRUE;
	}
	body = tad_body((*d->xrec));
	for ( p = ( body != NULL ) ? body->first : NULL; p != NULL; p = p->next ) {
		for ( t = p->first; t != NULL; t = t->next ) {
			cap += (SZ)node_len(t);
		}
		cap++;
	}
	buf = (UB *)Kmalloc(cap + 1);
	if ( buf == NULL ) {
		return FALSE;
	}
	for ( p = ( body != NULL ) ? body->first : NULL; p != NULL; p = p->next ) {
		if ( !same_word(p->name, "p") ) {
			continue;		/* the lines are the paragraphs */
		}
		if ( lines++ > 0 && n < cap ) {
			buf[n++] = 0x0A;
		}
		for ( t = p->first; t != NULL; t = t->next ) {
			INT	k, m = node_len(t);

			for ( k = 0; k < m && n < cap; k++ ) {
				buf[n++] = t->text[k];
			}
		}
	}
	buf[n] = 0;
	if ( n == 0 || tad_parse(buf, n, NULL, &chk) < E_OK
	  || tv_kind(chk) != TV_KIND_DOC ) {
		if ( chk != NULL ) {
			tad_free(chk);
		}
		Kfree(buf);
		d->host->tell(d, "原稿を実身に戻せません", "XML の形式が正しくありません");
		return FALSE;
	}
	tad_free(chk);
	d->host->before(d);
	rec = d->host->restore(d, buf, n);
	Kfree(buf);
	if ( rec == NULL ) {
		return FALSE;
	}
	(*d->rec) = rec;
	(*d->dirty) = TRUE;
	d->xml_edited = FALSE;

	return TRUE;
}

EXPORT BOOL de_xml_leave( T_DDED *d )
{
	if ( !(*d->xml_view) ) {
		return TRUE;
	}
	if ( !de_xml_commit(d) ) {
		return FALSE;
	}
	d->host->forget(d);
	if ( (*d->xrec) != NULL ) {
		tad_free((*d->xrec));
		(*d->xrec) = NULL;
	}
	(*d->xml_view) = FALSE;
	d->cpara = d->cpos = d->apara = d->apos = 0;
	(*d->scroll_y) = 0;

	return TRUE;
}

/* ---------------------------------------------------------------- the menu */

/* "#rrggbb" for a colour */
LOCAL void colour_word( UW col, UB *out )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	out[0] = '#';
	for ( i = 0; i < 6; i++ ) {
		out[1 + i] = (UB)hex[( col >> ( 20 - i * 4 ) ) & 0xF];
	}
	out[7] = 0;
}

/* The faces there are, by their family names */
#define FACE_MAX	8

LOCAL void face_list( T_DDED *d )
{
	UB		names[FACE_MAX][64];
	CONST char	*list[FACE_MAX];
	INT		i, n, pick;

	n = d->host->faces(d, names, FACE_MAX);
	for ( i = 0; i < n; i++ ) {
		list[i] = (CONST char *)names[i];
	}
	if ( n == 0 ) {
		d->host->tell(d, "書体がありません", NULL);
		return;
	}
	pick = d->host->list_form(d, "書体を選択してください", list, n, 0);
	if ( pick >= 0 ) {
		font_sel(d, "face", names[pick]);
	}
}

/*
 * 単語登録 started with the selection's first line as the word: it is
 * put in the tray's place of its own (UD_TRAY_SUB), where the accessory
 * takes it from as it starts.
 */
LOCAL void word_register( T_DDED *d )
{
	UB	w[WM_LABEL_MAX];
	INT	n = 0, i;

	if ( has_sel(d) ) {
		n = sel_text(d, w, sizeof(w));
		for ( i = 0; i < n && w[i] != '\n'; i++ ) ;
		n = i;
		if ( n > 0 ) {
			/* a letter cut short at the end goes */
			INT	k = n - 1, len;

			while ( k > 0 && ( w[k] & 0xC0 ) == 0x80 ) k--;
			len = ( w[k] >= 0xF0 ) ? 4 : ( w[k] >= 0xE0 ) ? 3 : ( w[k] >= 0xC0 ) ? 2 : 1;
			if ( k + len > n ) n = k;
		}
	}
	d->host->word_register(d, w, n);
}

/* The commands that apply in 原稿 too: the rest change the look */
LOCAL BOOL plain_text_cmd( INT cmd )
{
	return (BOOL)( cmd == DM_UNDO || cmd == DM_COPY || cmd == DM_PASTE
		    || cmd == DM_CUT || cmd == DM_MOVE_BACK
		    || cmd == DM_SELECT_ALL || cmd == DM_FIND || cmd == DM_WORDDIC );
}

/* ---------------------------------------------------------------- the tray */

/*
 * What is copied keeps what it looks like: each piece of it, text or
 * not, with the size, colour, face, spacing and decorations it was
 * drawn in, and the links and pictures in it. It goes to the tray as a
 * <document> fragment, the look written round each piece of text as the
 * editor writes it into its own record. What is taken from the tray is
 * read back through the view, piece by piece with its look, and put in
 * at the caret the same way. Paragraphs end where they ended.
 */
#define CR_PARA		100		/* a paragraph ends here */

typedef struct {
	UINT	kind;			/* TV_RUN_*, or CR_PARA */
	UB	*text;			/* the letters */
	INT	len;
	INT	size;
	UW	colour;
	UINT	style;
	UB	*face;
	INT	space;
	UW	back;
	T_VOBJ	*v;			/* a link */
	CONST T_TADNODE *node;		/* a picture or a figure, in the record read from */
} CRUN;

LOCAL CRUN	*cr = NULL;
LOCAL INT	ncr = 0, maxcr = 0;
LOCAL BOOL	cr_short = FALSE;	/* a piece could not be kept: what is kept is not all */

/* The decorations, and the element each is written as */
LOCAL CONST struct { UINT bit; CONST char *name; } cr_decos[] = {
	{ TV_ST_BOLD, "bold" },		{ TV_ST_ITALIC, "italic" },
	{ TV_ST_UNDER, "underline" },	{ TV_ST_OVER, "overline" },
	{ TV_ST_STRIKE, "strikethrough" }, { TV_ST_BOX, "box" },
	{ TV_ST_INVERT, "invert" },	{ TV_ST_MESH, "mesh" },
	{ TV_ST_BAG, "bagchar" },	{ TV_ST_SHADOW, "shadow" },
	{ TV_ST_NOPRINT, "noprint" },
};

#define NDECO	( (INT)( sizeof(cr_decos) / sizeof(cr_decos[0]) ) )

LOCAL void cr_clear( void )
{
	INT	i;

	for ( i = 0; cr != NULL && i < ncr; i++ ) {
		if ( cr[i].text != NULL ) Kfree(cr[i].text);
		if ( cr[i].face != NULL ) Kfree(cr[i].face);
		if ( cr[i].v != NULL ) Kfree(cr[i].v);
	}
	ncr = 0;
	cr_short = FALSE;
}

LOCAL UB *cr_dup( CONST UB *s, INT n )
{
	UB	*p;
	INT	i;

	if ( s == NULL ) {
		return NULL;
	}
	if ( n < 0 ) {
		for ( n = 0; s[n] != 0; n++ ) {
			;
		}
	}
	p = (UB *)Kmalloc((SZ)n + 1);
	if ( p != NULL ) {
		for ( i = 0; i < n; i++ ) {
			p[i] = s[i];
		}
		p[n] = 0;
	}

	return p;
}

/* One piece added, taking the look of the run it came from */
LOCAL CRUN *cr_add( UINT kind, CONST T_TVRUN *r )
{
	CRUN	*c;

	if ( ncr >= maxcr ) {
		/* as many pieces as there are */
		INT	m = ( maxcr > 0 ) ? maxcr * 2 : 256;
		CRUN	*n = (CRUN *)Krealloc(cr, sizeof(CRUN) * (size_t)m);

		if ( n == NULL ) {
			cr_short = TRUE;
			return NULL;
		}
		cr = n;
		maxcr = m;
	}
	c = &cr[ncr++];
	knl_memset(c, 0, sizeof(*c));
	c->kind = kind;
	c->size = 14;
	c->colour = 0x00000000U;
	c->back = TAD_COL_NONE;
	if ( r != NULL ) {
		c->size = r->size;
		c->colour = r->colour;
		c->style = r->style;
		c->face = cr_dup(r->face, -1);
		c->space = r->space;
		c->back = r->back;
	}

	return c;
}

/* The pieces of a stretch of a view, from [pa, a) to [pb, b), with their look */
LOCAL void cr_from( CONST T_TVDOC *doc, CONST T_TAD *rec, INT pa, INT a, INT pb, INT b )
{
	INT	pi;

	cr_clear();
	for ( pi = pa; pi <= pb && pi < doc->npara; pi++ ) {
		CONST T_TVPARA	*p = &doc->para[pi];
		INT		lo = ( pi == pa ) ? a : 0;
		INT		hi = ( pi == pb ) ? b : 0x7FFFFFFF;
		INT		i, acc = 0;

		for ( i = p->first; i < p->first + p->n && i < doc->nrun; i++ ) {
			CONST T_TVRUN	*r = &doc->run[i];
			INT		u = run_units(r);
			INT		ca = ( lo > acc ) ? lo : acc;
			INT		cb = ( hi < acc + u ) ? hi : acc + u;
			CRUN		*c;

			if ( cb <= ca ) {
				acc += u;
				continue;
			}
			switch ( r->kind ) {
			case TV_RUN_TEXT:
				c = cr_add(TV_RUN_TEXT, r);
				if ( c != NULL ) {
					c->text = cr_dup(r->text + ( ca - acc ), cb - ca);
					c->len = cb - ca;
					if ( c->text == NULL ) {
						cr_short = TRUE;
					}
				}
				break;
			case TV_RUN_LINK:
				c = cr_add(TV_RUN_LINK, r);
				if ( c != NULL ) {
					c->v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
					if ( c->v == NULL || tad_lnk_get(rec, r->link, c->v) < E_OK ) {
						ncr--;
					}
				}
				break;
			case TV_RUN_IMAGE:
			case TV_RUN_FIGURE:
				c = cr_add(r->kind, r);
				if ( c != NULL ) {
					c->node = r->node;
				}
				break;
			case TV_RUN_BREAK:
			case TV_RUN_TAB:
			case TV_RUN_INDENT:
			case TV_RUN_PAGE:
				(void)cr_add(r->kind, r);
				break;
			default:
				break;
			}
			acc += u;
		}
		if ( pi < pb ) {
			(void)cr_add(CR_PARA, NULL);
		}
	}
}

LOCAL CONST char *cr_elem_name( UINT kind )
{
	return ( kind == TV_RUN_BREAK ) ? "br" : ( kind == TV_RUN_TAB ) ? "tab"
	     : ( kind == TV_RUN_INDENT ) ? "indent" : "pagebreak";
}

/* A piece of text, its look round it, into a paragraph of a fragment */
LOCAL void cr_text_into( T_TAD *frag, T_TADNODE *p, CONST CRUN *c )
{
	T_TADNODE	*at = p, *e;
	UB		num[16];
	INT		i;

	if ( ( c->size > 0 && c->size != 14 ) || c->colour != 0x00000000U
	  || ( c->face != NULL && c->face[0] != 0 ) || c->space != 0 ) {
		e = tad_elem_new(frag, "font", at, NULL, FALSE);
		if ( e != NULL ) {
			if ( c->size > 0 && c->size != 14 ) {
				num_word(c->size, 0, num);
				(void)tad_set_attr(frag, e, "size", num);
			}
			if ( c->colour != 0x00000000U ) {
				colour_word(c->colour, num);
				(void)tad_set_attr(frag, e, "color", num);
			}
			if ( c->face != NULL && c->face[0] != 0 ) {
				(void)tad_set_attr(frag, e, "face", c->face);
			}
			if ( c->space != 0 ) {
				num_word(c->space, 1, num);
				(void)tad_set_attr(frag, e, "space", num);
			}
			at = e;
		}
	}
	for ( i = 0; i < NDECO; i++ ) {
		if ( ( c->style & cr_decos[i].bit ) != 0
		  && ( e = tad_elem_new(frag, cr_decos[i].name, at, NULL, FALSE) ) != NULL ) {
			at = e;
		}
	}
	if ( ( c->style & ( TV_ST_SUP | TV_ST_SUB ) ) != 0
	  && ( e = tad_elem_new(frag, "attend", at, NULL, FALSE) ) != NULL ) {
		(void)tad_set_attr(frag, e, "type",
				   (CONST UB *)( ( ( c->style & TV_ST_SUP ) != 0 ) ? "1" : "0" ));
		at = e;
	}
	(void)tad_text_new(frag, at, NULL, c->text, c->len);
}

/* The pieces as a <document> fragment, with their look; NULL: no memory */
LOCAL T_TAD *cr_frag( void )
{
	T_TAD		*frag = de_frag_new("document");
	T_TADNODE	*body, *p;
	TS_UUID		id;
	INT		i;

	if ( frag == NULL ) {
		return NULL;
	}
	body = tad_body(frag);
	p = ( body != NULL ) ? tad_elem_new(frag, "p", body, NULL, FALSE) : NULL;
	for ( i = 0; p != NULL && i < ncr; i++ ) {
		CONST CRUN	*r = &cr[i];

		switch ( r->kind ) {
		case CR_PARA:
			p = tad_elem_new(frag, "p", body, NULL, FALSE);
			break;
		case TV_RUN_TEXT:
			cr_text_into(frag, p, r);
			break;
		case TV_RUN_LINK:
			/*
			 * The link goes at the end of the body, a line break
			 * after it; it is moved into the paragraph. What is
			 * after the body's last node before it is the link.
			 */
			{
				T_TADNODE	*was = body->last;

				if ( r->v != NULL && tad_lnk_add(frag, r->v, &id) >= E_OK ) {
					T_TADNODE	*lk = ( was != NULL ) ? was->next : body->first;
					T_TADNODE	*nl = body->last;

					if ( nl != NULL && nl != lk && nl->kind == TAD_ND_TEXT ) {
						tad_node_remove(nl);
					}
					if ( lk != NULL && lk->kind == TAD_ND_ELEM ) {
						tad_node_move(lk, p, NULL);
					}
				}
			}
			break;
		case TV_RUN_IMAGE:
		case TV_RUN_FIGURE:
			if ( r->node != NULL ) {
				(void)tad_node_copy(frag, p, NULL, r->node);
			}
			break;
		default:
			(void)tad_elem_new(frag, cr_elem_name(r->kind), p, NULL, TRUE);
			break;
		}
	}
	return frag;
}

/* The pieces as a <document> fragment, to the tray as one set */
LOCAL ER cr_to_tray( T_DDED *d )
{
	T_TAD	*frag = cr_frag();
	UB	name[TR_NAME_MAX];
	INT	i;
	ER	er;

	if ( frag == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; "文字列"[i] != 0; i++ ) name[i] = (UB)"文字列"[i];
	name[i] = 0;
	er = d->host->tray_put(d, frag, (CONST char *)name);
	tad_free(frag);

	return er;
}

/*
 * The selection, with its look, as a <document> fragment (Kfree it with
 * tad_free): what a text carried out of its window hands on, to be put
 * in as it looked where it is let go. NULL: nothing selected, or no
 * memory for all of it.
 */
EXPORT T_TAD *de_sel_frag( T_DDED *d )
{
	INT	pa, a, pb, b;
	T_TAD	*frag = NULL;

	if ( !(*d->used) || (*d->doc) == NULL || !has_sel(d) ) {
		return NULL;
	}
	sel_order(d, &pa, &a, &pb, &b);
	cr_from((*d->doc), (*d->rec), pa, a, pb, b);
	if ( !cr_short ) {
		frag = cr_frag();
	}
	cr_clear();
	return frag;
}

/*
 * The selection, with its look, to the tray: TRUE when it is all there.
 * When it is not -- no memory for all of it -- the person is told, and
 * nothing is to be taken away from the text.
 */
LOCAL BOOL cr_copy( T_DDED *d )
{
	INT	pa, a, pb, b;
	ER	er;

	sel_order(d, &pa, &a, &pb, &b);
	cr_from((*d->doc), (*d->rec), pa, a, pb, b);
	er = cr_short ? E_NOMEM : cr_to_tray(d);
	cr_clear();
	if ( er < E_OK ) {
		d->host->tell(d, "トレーに入れられませんでした", ( er == E_NOMEM )
			? "メモリが足りません。文章は元のままです" : "文章は元のままです");
		return FALSE;
	}

	return TRUE;
}

/* The look of a piece written round the stretch [a, b) of a paragraph */
LOCAL void cr_look( T_DDED *d, INT pi, INT a, INT b, CONST CRUN *c )
{
	CONST char	*attrs[9];
	UB		sz[16], col[8], sp[16];
	INT		n = 0, i;

	if ( b <= a ) {
		return;
	}
	if ( c->size > 0 && c->size != 14 ) {
		num_word(c->size, 0, sz);
		attrs[n++] = "size";
		attrs[n++] = (CONST char *)sz;
	}
	if ( c->colour != 0x00000000U ) {
		colour_word(c->colour, col);
		attrs[n++] = "color";
		attrs[n++] = (CONST char *)col;
	}
	if ( c->face != NULL && c->face[0] != 0 ) {
		attrs[n++] = "face";
		attrs[n++] = (CONST char *)c->face;
	}
	if ( c->space != 0 ) {
		num_word(c->space, 1, sp);
		attrs[n++] = "space";
		attrs[n++] = (CONST char *)sp;
	}
	attrs[n] = NULL;
	if ( n > 0 ) {
		wrap_para(d, pi, a, b, "font", attrs);
	}
	for ( i = 0; i < NDECO; i++ ) {
		if ( ( c->style & cr_decos[i].bit ) != 0 ) {
			wrap_para(d, pi, a, b, cr_decos[i].name, NULL);
		}
	}
	if ( ( c->style & ( TV_ST_SUP | TV_ST_SUB ) ) != 0 ) {
		CONST char	*at[3];

		at[0] = "type";
		at[1] = ( ( c->style & TV_ST_SUP ) != 0 ) ? "1" : "0";
		at[2] = NULL;
		wrap_para(d, pi, a, b, "attend", at);
	}
}

/* What was copied put in at the caret, as it looked */
LOCAL void cr_paste( T_DDED *d )
{
	T_TAD	*rec = rec_of(d);
	INT	i;

	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	for ( i = 0; i < ncr; i++ ) {
		CONST CRUN	*c = &cr[i];

		switch ( c->kind ) {
		case CR_PARA:
			if ( split_at(d, d->cpara, d->cpos) ) {
				d->host->model(d);
				d->cpara++;
				d->cpos = 0;
			}
			break;
		case TV_RUN_TEXT: {
			INT	start = d->cpos;

			if ( c->len > 0 && put_words(d, d->cpara, d->cpos, c->text, c->len) ) {
				d->host->model(d);
				d->cpos += c->len;
				if ( !(*d->xml_view) ) {
					cr_look(d, d->cpara, start, d->cpos, c);
				}
			}
			break;
		}
		case TV_RUN_LINK: {
			T_VOBJ	v;
			TS_UUID	id;

			if ( c->v == NULL || (*d->xml_view)
			  || ts_uuid_cmp(&c->v->target, &(*d->id)) == 0 ) {
				break;		/* not inside itself */
			}
			v = *c->v;
			knl_memset(&v.vobjid, 0, sizeof(v.vobjid));
			if ( tad_lnk_add((T_TAD *)(*d->rec), &v, &id) >= E_OK ) {
				link_to(d, d->cpara, d->cpos);
				d->host->model(d);
				d->cpos++;
			}
			break;
		}
		case TV_RUN_IMAGE:
		case TV_RUN_FIGURE: {
			T_TADNODE	*mark;

			if ( (*d->xml_view) || c->node == NULL ) {
				break;
			}
			/* the element as it was, where a mark was put */
			mark = put_elem(d, d->cpara, d->cpos, "tab");
			if ( mark != NULL ) {
				(void)tad_node_copy(rec, mark->parent, mark, c->node);
				tad_node_remove(mark);
			}
			d->host->model(d);
			d->cpos++;
			break;
		}
		default: {
			CONST char	*name = cr_elem_name(c->kind);

			if ( (*d->xml_view) ) {
				break;
			}
			if ( put_elem(d, d->cpara, d->cpos, name) != NULL ) {
				d->host->model(d);
				d->cpos++;
			}
			break;
		}
		}
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/*
 * A figure fragment from the tray, put in the text at the caret: the
 * shapes as a figure standing in the text, their own view the box round
 * them, and the links each as a link in the text.
 */
LOCAL void links_paste( T_DDED *d, CONST T_TAD *frag )
{
	INT	n = tad_lnk_count(frag), i;

	if ( ( n <= 0 && de_frag_shapes(frag) == 0 ) || (*d->xml_view) ) {
		return;
	}
	undo_point(d, FALSE);
	if ( has_sel(d) ) {
		cut_sel(d);
	}
	if ( de_frag_shapes(frag) > 0 ) {
		T_TAD		*rec = rec_of(d);
		T_TADNODE	*mark = put_elem(d, d->cpara, d->cpos, "tab"), *fg;
		T_TVFIG		*f = NULL;

		fg = ( mark != NULL ) ? tad_elem_new(rec, "figure", mark->parent, mark, FALSE)
				      : NULL;
		if ( mark != NULL ) {
			tad_node_remove(mark);
		}
		if ( fg != NULL && fe_frag_into(rec, fg, frag) > 0 && tv_fig_of(fg, &f) >= E_OK
		  && f != NULL ) {
			T_DPRECT	b;
			T_TADNODE	*vw, *dw;
			UB		num[16];
			INT		k, j;

			b = f->sh[0].r;
			for ( k = 1; k < f->nsh; k++ ) {
				if ( f->sh[k].r.left < b.left ) b.left = f->sh[k].r.left;
				if ( f->sh[k].r.top < b.top ) b.top = f->sh[k].r.top;
				if ( f->sh[k].r.right > b.right ) b.right = f->sh[k].r.right;
				if ( f->sh[k].r.bottom > b.bottom ) b.bottom = f->sh[k].r.bottom;
			}
			tv_fig_free(f);
			vw = tad_elem_new(rec, "figView", fg, fg->first, TRUE);
			dw = tad_elem_new(rec, "figDraw", fg, vw != NULL ? vw->next : NULL, TRUE);
			for ( j = 0; j < 2; j++ ) {
				T_TADNODE	*e = ( j == 0 ) ? vw : dw;

				if ( e == NULL ) {
					continue;
				}
				num_word(b.left, 0, num);
				(void)tad_set_attr(rec, e, "left", num);
				num_word(b.top, 0, num);
				(void)tad_set_attr(rec, e, "top", num);
				num_word(b.right, 0, num);
				(void)tad_set_attr(rec, e, "right", num);
				num_word(b.bottom, 0, num);
				(void)tad_set_attr(rec, e, "bottom", num);
			}
			d->host->model(d);
			d->cpos++;
		} else {
			if ( f != NULL ) {
				tv_fig_free(f);
			}
			if ( fg != NULL ) {
				tad_node_remove(fg);
			}
			d->host->model(d);
		}
	}
	for ( i = 0; i < n; i++ ) {
		T_VOBJ	c;
		TS_UUID	id;

		if ( tad_lnk_get(frag, i, &c) < E_OK || ts_uuid_cmp(&c.target, &(*d->id)) == 0 ) {
			continue;
		}
		knl_memset(&c.vobjid, 0, sizeof(c.vobjid));
		/* a figure's box as the size a text gives it */
		c.width = c.right - c.left;
		c.heightpx = c.bottom - c.top;
		if ( tad_lnk_add((T_TAD *)(*d->rec), &c, &id) >= E_OK ) {
			link_to(d, d->cpara, d->cpos);
			d->host->model(d);
			d->cpos++;
		}
	}
	d->apara = d->cpara;
	d->apos = d->cpos;
	changed(d);
}

/* A record with nothing in its body but an element of that name */
EXPORT T_TAD *de_frag_new( CONST char *body )
{
	UB	text[96];
	T_TAD	*doc = NULL;
	INT	n = 0, i;
	CONST char *head = "<tad version=\"1.0\" encoding=\"UTF-8\"><";

	for ( i = 0; head[i] != 0; i++ ) text[n++] = (UB)head[i];
	for ( i = 0; body[i] != 0 && n < 80; i++ ) text[n++] = (UB)body[i];
	text[n++] = '>';
	text[n++] = '<';
	text[n++] = '/';
	for ( i = 0; body[i] != 0 && n < 90; i++ ) text[n++] = (UB)body[i];
	text[n++] = '>';
	for ( i = 0; "</tad>"[i] != 0; i++ ) text[n++] = (UB)"</tad>"[i];
	if ( tad_parse(text, (SZ)n, NULL, &doc) < E_OK ) {
		return NULL;
	}
	return doc;
}

/* The shapes of a figure fragment: every element of its body but the links */
EXPORT INT de_frag_shapes( CONST T_TAD *frag )
{
	CONST T_TADNODE	*body = tad_body(frag), *c;
	INT		n = 0;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && c->name != NULL
		  && !( c->name[0] == 'l' && c->name[1] == 'i' && c->name[2] == 'n'
			&& c->name[3] == 'k' && c->name[4] == 0 ) ) {
			n++;
		}
	}
	return n;
}

/*
 * The set in hand in the tray put in at the caret: a figure's shapes and
 * links as a figure and links in the text, a text's pieces as they
 * looked. Answers whether anything was there to put in.
 */
LOCAL BOOL from_tray( T_DDED *d )
{
	T_TAD		*frag = d->host->tray_frag(d);
	T_TADNODE	*body;
	T_TVDOC		*m = NULL;
	BOOL		any = FALSE;

	if ( frag == NULL ) {
		return FALSE;
	}
	body = tad_body(frag);
	if ( body != NULL && body->name != NULL && body->name[0] == 'f' ) {
		/* the pictures the set carries (画面印刷), beside this text's record */
		if ( !(*d->xml_view) ) d->host->tray_pictures(d, frag);
		links_paste(d, frag);
		any = TRUE;
	} else if ( body != NULL && tv_doc(frag, &m) >= E_OK && m != NULL ) {
		cr_from(m, frag, 0, 0, m->npara - 1, 0x7FFFFFFF);
		if ( ncr > 0 ) {
			cr_paste(d);
			any = TRUE;
		}
		cr_clear();
		tv_doc_free(m);
	}
	tad_free(frag);
	return any;
}

/* ---------------------------------------------------------------- letters carried */

/*
 * A press on what is selected and the hand moved: the letters are
 * carried, an outline following the hand held at its left. Let go over
 * a text of the desktop's they go in where the outline's corner stands,
 * as they looked, and leave where they were (a move) unless the second
 * button went down on the way (a copy). Let go over a program's window
 * that takes letters (OB_E_TEXT) they are dropped on it as one line,
 * and stay here as well: the program may still refuse them. Let go
 * where they were pressed, it was a click: the caret goes there.
 */
#define SC_GRAB		4		/* the hand holds the outline this far in from its left */
#define SC_H		20

LOCAL INT	sc_x0, sc_y0;		/* where it was pressed, on the screen */
LOCAL INT	sc_pi, sc_pos;		/* and in the text */
LOCAL INT	sc_w;			/* the outline's width */
LOCAL BOOL	sc_moved, sc_copy;

LOCAL BOOL sel_carry_take( T_DDED *d, INT x, INT y, INT pi, INT pos )
{
	INT	ox, oy;
	INT	pa, a, pb, b, n;

	if ( !has_sel(d) || !d->host->origin(d, &ox, &oy) ) {
		return FALSE;
	}
	sel_order(d, &pa, &a, &pb, &b);
	if ( pi < pa || ( pi == pa && pos < a ) || pi > pb || ( pi == pb && pos > b ) ) {
		return FALSE;
	}
	sc_win = d;
	sc_x0 = ox + x;
	sc_y0 = oy + y;
	sc_pi = pi;
	sc_pos = pos;
	sc_moved = FALSE;
	sc_copy = FALSE;
	n = ( pa == pb ) ? b - a : 30;
	sc_w = n * 4 + 16;		/* about a letter's width for every three bytes */
	if ( sc_w > 320 ) {
		sc_w = 320;
	}
	return TRUE;
}

LOCAL void sel_carry_follow( INT sx, INT sy )
{
	T_DPRECT	r;

	if ( !sc_moved && sx - sc_x0 < CARRY_START && sc_x0 - sx < CARRY_START
	  && sy - sc_y0 < CARRY_START && sc_y0 - sy < CARRY_START ) {
		return;			/* a shaky hand, not a carry */
	}
	sc_moved = TRUE;
	r.left = sx - SC_GRAB;
	r.top = sy - SC_H / 2;
	r.right = r.left + sc_w;
	r.bottom = r.top + SC_H;
	sc_win->host->drag(sc_win, &r, FALSE, 0, 0);
	sc_win->host->update(sc_win);
}

LOCAL void sel_carry_click( void )
{
	T_DDED	*d = sc_win;

	sc_win = NULL;
	if ( d != NULL && (*d->used) && (*d->doc) != NULL && sc_pi < (*d->doc)->npara ) {
		move_to(d, sc_pi, sc_pos, FALSE);
	}
}

EXPORT BOOL de_sel_carrying( void )
{
	return (BOOL)( sc_win != NULL && sc_moved );
}

EXPORT void de_sel_copy( void )
{
	if ( sc_win != NULL ) {
		sc_copy = TRUE;
	}
}

/* Letters put in a text where a place on the screen is: what a program carries out, or this text's own */
EXPORT ER de_text_drop( T_DDED *t, INT sx, INT sy, CONST UB *s, INT n )
{
	INT	pi, pos;

	if ( !de_editable(t) || (*t->xml_view) ) {
		return E_PAR;
	}
	if ( !text_place(t, sx, sy, &pi, &pos) ) {
		pi = (*t->doc)->npara - 1;
		pos = para_len(t, pi);
	}
	t->apara = t->cpara = pi;
	t->apos = t->cpos = pos;
	insert(t, s, n, FALSE);
	return E_OK;
}

LOCAL void sel_put( T_DDED *d, T_DDED *t, INT cx, INT cy, BOOL copy );

EXPORT void de_sel_release( INT sx, INT sy )
{
	T_DDED	*d = sc_win, *t = NULL;
	INT	cx = sx - SC_GRAB, cy = sy - SC_H / 2;
	INT	pa, a, pb, b;
	UINT	at;

	if ( d == NULL || !sc_moved ) {
		return;			/* a click: de_release puts the caret */
	}
	sc_win = NULL;
	d->host->drag(d, NULL, FALSE, 0, 0);
	if ( !(*d->used) || (*d->doc) == NULL || !has_sel(d) ) {
		d->host->update(d);
		return;
	}
	sel_order(d, &pa, &a, &pb, &b);
	at = d->host->at(d, sx, sy, &t);
	if ( at == DE_AT_OTHER ) {
		/*
		 * Something else, a program's window or a figure: the letters
		 * handed to it, leaving here only when it says they moved
		 */
		INT	n = sel_text(d, NULL, 0);
		UB	*s = (UB *)Kmalloc((SZ)n + 1);
		ER	er = E_NOMEM;

		if ( s != NULL ) {
			n = sel_text(d, s, n + 1);
			er = ( n > 0 ) ? d->host->text_away(d, sx, sy, cx, cy, s, n) : E_NOEXS;
			Kfree(s);
		}
		if ( er > 0 && !sc_copy && (*d->used) && (*d->doc) != NULL ) {
			d->apara = pa;
			d->apos = a;
			d->cpara = pb;
			d->cpos = b;
			erase_back(d);
		}
		d->host->update(d);
		return;
	}
	sel_put(d, ( at == DE_AT_TEXT ) ? t : NULL, cx, cy, sc_copy);
}

/*
 * Letters carried out of this text's window and let go back over it,
 * with the hand at sx, sy: moved, or copied, within it as when they had
 * not gone out (a program's window, which the desktop carried them on
 * over)
 */
EXPORT void de_sel_back( T_DDED *d, INT sx, INT sy, BOOL copy )
{
	if ( !(*d->used) || (*d->doc) == NULL || !has_sel(d) ) {
		d->host->update(d);
		return;
	}
	sel_put(d, d, sx - SC_GRAB, sy - SC_H / 2, copy);
}

/*
 * The pieces of a <document> fragment put in a text where a place on the
 * screen is, as they looked: what a program carries out of its text
 */
EXPORT ER de_frag_drop( T_DDED *t, INT sx, INT sy, CONST T_TAD *frag )
{
	T_TVDOC	*m = NULL;
	INT	pi, pos;
	ER	er;

	if ( !de_editable(t) || (*t->xml_view) || frag == NULL ) {
		return E_PAR;
	}
	if ( tv_doc(frag, &m) < E_OK || m == NULL ) {
		return E_OBJ;
	}
	if ( !text_place(t, sx, sy, &pi, &pos) ) {
		pi = (*t->doc)->npara - 1;
		pos = para_len(t, pi);
	}
	cr_from(m, frag, 0, 0, m->npara - 1, 0x7FFFFFFF);
	er = cr_short ? E_NOMEM : ( ncr > 0 ) ? E_OK : E_NOEXS;
	if ( er >= E_OK ) {
		t->apara = t->cpara = pi;
		t->apos = t->cpos = pos;
		cr_paste(t);
	}
	cr_clear();
	tv_doc_free(m);
	return er;
}

/*
 * The selection of d put in t (NULL: nothing that takes letters) with the
 * outline's corner at cx, cy, as it looked; it leaves d unless copy
 */
LOCAL void sel_put( T_DDED *d, T_DDED *t, INT cx, INT cy, BOOL copy )
{
	INT	pa, a, pb, b, pi, pos;

	if ( t == NULL || !de_editable(t) || (*t->xml_view) ) {
		(void)wm_msg_put("ここには置けません");
		d->host->update(d);
		return;
	}
	sel_order(d, &pa, &a, &pb, &b);
	if ( !text_place(t, cx, cy, &pi, &pos) ) {
		pi = (*t->doc)->npara - 1;
		pos = para_len(t, pi);
	}
	if ( t == d && ( pi > pa || ( pi == pa && pos >= a ) ) && ( pi < pb || ( pi == pb && pos <= b ) ) ) {
		d->host->update(d);
		return;			/* onto itself: nothing moves */
	}
	cr_from((*d->doc), (*d->rec), pa, a, pb, b);
	if ( cr_short ) {
		/* not all of it could be kept: nothing moved, nothing lost */
		cr_clear();
		d->host->no_memory(d);
		return;
	}
	if ( t != d ) {
		d->host->focus(d, t);
	}
	if ( t == d && !copy && ( pi < pa || ( pi == pa && pos < a ) ) ) {
		/* before where they were, in the same text: taken out first, the place stays */
		erase_back(d);
		d->apara = d->cpara = pi;
		d->apos = d->cpos = pos;
		cr_paste(d);
	} else {
		t->apara = t->cpara = pi;
		t->apos = t->cpos = pos;
		cr_paste(t);
		if ( !copy && (*d->used) && (*d->doc) != NULL ) {
			/* after them, or in another text: they leave where they were */
			d->apara = pa;
			d->apos = a;
			d->cpara = pb;
			d->cpos = b;
			erase_back(d);
		}
	}
	cr_clear();
}

EXPORT BOOL de_command( T_DDED *d, INT cmd )
{
	LOCAL CONST UW	colours[] = {
		0x000000, 0x0000ff, 0xee0000, 0xff69b4, 0xff8c00,
		0x008000, 0x7fff00, 0x00ffff, 0xffff00, 0xffffff
	};
	LOCAL CONST char *CONST spaces[] = {
		"0", "0.0625", "0.125", "0.25", "0.375", "0.5", "0.75"
	};
	UB	word[WM_LABEL_MAX];
	UW	col;
	INT	v;

	if ( !de_editable(d) ) {
		return FALSE;
	}
	clamp(d);
	if ( (*d->xml_view) && !plain_text_cmd(cmd) ) {
		return TRUE;
	}
	switch ( cmd ) {
	case DM_UNDO:
		if ( (*d->xml_view) ) {
			return TRUE;
		}
		d->typing = FALSE;
		d->host->undo(d);
		clamp(d);
		d->host->draw(d);
		shown(d);
		return TRUE;
	case DM_SELECT_ALL:
		d->apara = 0;
		d->apos = 0;
		d->cpara = (*d->doc)->npara - 1;
		d->cpos = para_len(d, d->cpara);
		d->host->draw(d);
		shown(d);
		return TRUE;
	case DM_COPY:
		if ( has_sel(d) ) {
			cr_copy(d);
		}
		return TRUE;
	case DM_CUT:
		/* taken away only once it is safe in the tray */
		if ( has_sel(d) && cr_copy(d) ) {
			erase_back(d);
		}
		return TRUE;
	case DM_PASTE:
	case DM_MOVE_BACK:
		/* the set in hand, whatever window put it in the tray */
		if ( from_tray(d) && cmd == DM_MOVE_BACK ) {
			d->host->tray_taken(d);
		}
		return TRUE;
	case DM_FIND:
		find_replace(d);
		return TRUE;
	case DM_VIRTUALIZE:
		virtualize(d);
		return TRUE;
	case DM_WORDDIC:
		word_register(d);
		return TRUE;

	/* 書体 */
	case DM_FONT_LIST:
		face_list(d);
		return TRUE;
	case DM_FONT_GOTHIC:
		font_sel(d, "face", (CONST UB *)"\"Noto Sans JP\", sans-serif");
		return TRUE;
	case DM_FONT_MINCHO:
		font_sel(d, "face", (CONST UB *)"\"Noto Serif JP\", serif");
		return TRUE;
	case DM_FONT_MEIRYO:
		font_sel(d, "face", (CONST UB *)"\"Meiryo\", \"メイリオ\"");
		return TRUE;

	/* 文字修飾 */
	case DM_STYLE_NORMAL:	unwrap_sel(d, nm_plain);		return TRUE;
	case DM_STYLE_CLEAR:	unwrap_sel(d, nm_deco);			return TRUE;
	case DM_STYLE_BOLD:	style_sel(d, nm_bold, TV_ST_BOLD);	return TRUE;
	case DM_STYLE_ITALIC:	style_sel(d, nm_italic, TV_ST_ITALIC);	return TRUE;
	case DM_STYLE_BAGCHAR:	style_sel(d, nm_bag, TV_ST_BAG);	return TRUE;
	case DM_STYLE_BOX:	style_sel(d, nm_box, TV_ST_BOX);	return TRUE;
	case DM_STYLE_SHADOW:	style_sel(d, nm_shadow, TV_ST_SHADOW);	return TRUE;
	case DM_STYLE_UNDERLINE: style_sel(d, nm_under, TV_ST_UNDER);	return TRUE;
	case DM_STYLE_OVERLINE:	style_sel(d, nm_over, TV_ST_OVER);	return TRUE;
	case DM_STYLE_STRIKE:	style_sel(d, nm_strike, TV_ST_STRIKE);	return TRUE;
	case DM_STYLE_HATCH:	style_sel(d, nm_mesh, TV_ST_MESH);	return TRUE;
	case DM_STYLE_INVERSE:	style_sel(d, nm_invert, TV_ST_INVERT);	return TRUE;
	case DM_STYLE_NOPRINT:	style_sel(d, nm_noprint, TV_ST_NOPRINT); return TRUE;
	case DM_STYLE_SUPER:	attend_sel(d, TRUE);			return TRUE;
	case DM_STYLE_SUB:	attend_sel(d, FALSE);			return TRUE;
	case DM_STYLE_INDENT:
	case DM_FMT_INDENT:
		indent_here(d);
		return TRUE;

	/* 文字サイズ */
	case DM_SIZE_CUSTOM:
		if ( !d->host->ask_text(d, "文字サイズを入力してください（pt）", NULL,
				  (CONST UB *)"14", word, 16, "設定") ) {
			return TRUE;
		}
		v = word_num(word);
		if ( v < 1000 || v > 999000 ) {
			d->host->tell(d, "文字サイズを設定できません", "数値を入力してください");
			return TRUE;
		}
		num_word(v, 1, word);
		font_sel(d, "size", word);
		return TRUE;
	case DM_SIZE_FULLWIDTH:
		width_sel(d, TRUE);
		return TRUE;
	case DM_SIZE_HALFWIDTH:
		width_sel(d, FALSE);
		return TRUE;

	/* 文字色 */
	case DM_COLOUR_CUSTOM:
		col = 0x00000000U;
		if ( !d->host->pick_colour(d, &col) ) {
			d->host->draw(d);
			return TRUE;
		}
		colour_word(col, word);
		font_sel(d, "color", word);
		return TRUE;

	/* 書式 */
	case DM_FMT_NEW_TAB:
		tab_format(d);
		return TRUE;
	case DM_ALIGN_LEFT:	align_sel(d, "left");			return TRUE;
	case DM_ALIGN_CENTRE:	align_sel(d, "center");			return TRUE;
	case DM_ALIGN_RIGHT:	align_sel(d, "right");			return TRUE;
	case DM_RUBY_TOP:	ruby_sel(d, FALSE);			return TRUE;
	case DM_RUBY_BOTTOM:	ruby_sel(d, TRUE);			return TRUE;
	case DM_DIR_VERT:	set_direction(d, TRUE);			return TRUE;
	case DM_DIR_HORIZ:	set_direction(d, FALSE);		return TRUE;
	case DM_COMB:		comb_sel(d);				return TRUE;
	case DM_TURN:		turn_sel(d);				return TRUE;
	case DM_PAGEBREAK:
		page_break(d);
		return TRUE;
	case DM_PAGE_SETUP:
		page_setup(d);
		return TRUE;
	default:
		break;
	}
	if ( cmd >= DM_FONT_RECENT && cmd < DM_FONT_RECENT + de_nrecent ) {
		UB	face[WM_LABEL_MAX];
		INT	k;

		for ( k = 0; k < WM_LABEL_MAX; k++ ) {
			face[k] = de_recent[cmd - DM_FONT_RECENT][k];
		}
		font_sel(d, "face", face);
		return TRUE;
	}
	if ( cmd >= DM_SPACE_0 && cmd <= DM_SPACE_3_4 ) {
		font_sel(d, "space", (CONST UB *)spaces[cmd - DM_SPACE_0]);
		return TRUE;
	}
	if ( cmd >= DM_COLOUR_BLACK && cmd <= DM_COLOUR_WHITE ) {
		colour_word(colours[cmd - DM_COLOUR_BLACK], word);
		font_sel(d, "color", word);
		return TRUE;
	}
	if ( cmd >= DM_SIZE_BASE && cmd < DM_SIZE_BASE + 10000 ) {
		/* the size in tenths, as a number of points */
		num_word(DM_SIZE_TENTHS(cmd) * 100, 1, word);
		font_sel(d, "size", word);
		return TRUE;
	}

	return FALSE;
}
