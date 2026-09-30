/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad_edit.c
 *	Changing the text of a document (design 17.12)
 *
 *	What a text editor does to a record, done to its nodes: letters
 *	put into a text node and taken out of one, a paragraph cut in two
 *	and two put back together, an element taken out, one wrapped round
 *	a stretch of text. The record stays the one copy of the document;
 *	the editor makes its model again from the record after each change.
 *
 *	A node that is changed is written from what it now holds, not from
 *	the text it was read from. Memory for what is changed comes from
 *	the document's arena and goes back when the document is freed.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "tad_local.h"

/* A node taken out of the list of its parent's children */
LOCAL void ed_unhook( T_TADNODE *nd )
{
	T_TADNODE	*parent = nd->parent, *p, *last = NULL;

	if ( parent == NULL ) {
		return;
	}
	if ( parent->first == nd ) {
		parent->first = nd->next;
	} else {
		for ( p = parent->first; p != NULL; p = p->next ) {
			if ( p->next == nd ) {
				p->next = nd->next;
				break;
			}
		}
	}
	for ( p = parent->first; p != NULL; p = p->next ) {
		last = p;
	}
	parent->last = last;
	nd->parent = NULL;
	nd->next = NULL;
}

/* A node put into a parent before one of its children, or last */
LOCAL void ed_hook( T_TADNODE *parent, T_TADNODE *nd, T_TADNODE *before )
{
	T_TADNODE	*p;

	if ( before == NULL || before->parent != parent ) {
		tad_add_child(parent, nd);
		return;
	}
	nd->parent = parent;
	nd->next = before;
	if ( parent->first == before ) {
		parent->first = nd;
		return;
	}
	for ( p = parent->first; p != NULL; p = p->next ) {
		if ( p->next == before ) {
			p->next = nd;
			return;
		}
	}
}

/* A node put in right after another, under the same parent */
LOCAL void ed_hook_after( T_TADNODE *nd, T_TADNODE *after )
{
	T_TADNODE	*parent = after->parent;

	nd->parent = parent;
	nd->next = after->next;
	after->next = nd;
	if ( parent != NULL && parent->last == after ) {
		parent->last = nd;
	}
}

EXPORT ER tad_text_splice( T_TAD *doc, T_TADNODE *nd, INT at, INT cut,
			   CONST UB *ins, INT n )
{
	INT	len = 0, i, k = 0;
	UB	*nt;

	if ( doc == NULL || nd == NULL || nd->kind != TAD_ND_TEXT
	  || at < 0 || cut < 0 || n < 0 ) {
		return E_PAR;
	}
	while ( nd->text != NULL && nd->text[len] != 0 ) {
		len++;
	}
	if ( at > len ) {
		at = len;
	}
	if ( at + cut > len ) {
		cut = len - at;
	}
	nt = (UB *)tad_alloc(doc, (SZ)( len - cut + n + 1 ));
	if ( nt == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < at; i++ ) {
		nt[k++] = nd->text[i];
	}
	for ( i = 0; i < n; i++ ) {
		nt[k++] = ins[i];
	}
	for ( i = at + cut; i < len; i++ ) {
		nt[k++] = nd->text[i];
	}
	nt[k] = 0;
	nd->text = nt;
	nd->raw = NULL;
	nd->rawlen = 0;

	return E_OK;
}

EXPORT T_TADNODE *tad_text_new( T_TAD *doc, T_TADNODE *parent,
				T_TADNODE *before, CONST UB *s, INT n )
{
	T_TADNODE	*nd;

	if ( doc == NULL || parent == NULL ) {
		return NULL;
	}
	nd = tad_new_node(doc, TAD_ND_TEXT);
	if ( nd == NULL ) {
		return NULL;
	}
	nd->text = tad_dup(doc, ( s != NULL ) ? s : (CONST UB *)"",
			   ( s != NULL ) ? (SZ)n : 0);
	if ( nd->text == NULL ) {
		return NULL;
	}
	ed_hook(parent, nd, before);

	return nd;
}

EXPORT T_TADNODE *tad_elem_new( T_TAD *doc, CONST char *name,
				T_TADNODE *parent, T_TADNODE *before,
				BOOL empty )
{
	T_TADNODE	*nd;

	if ( doc == NULL || name == NULL || parent == NULL ) {
		return NULL;
	}
	nd = tad_new_node(doc, TAD_ND_ELEM);
	if ( nd == NULL ) {
		return NULL;
	}
	nd->name = tad_dup(doc, (CONST UB *)name, tad_slen(name));
	if ( nd->name == NULL ) {
		return NULL;
	}
	nd->empty = empty;
	ed_hook(parent, nd, before);

	return nd;
}

/*
 * A node copied, with all it holds, into a place of any document: the
 * same document or another. Answers the copy, or NULL.
 */
EXPORT T_TADNODE *tad_node_copy( T_TAD *doc, T_TADNODE *parent, T_TADNODE *before,
				 CONST T_TADNODE *from )
{
	T_TADNODE	*e;
	CONST T_TADNODE	*c;
	INT		i;

	if ( doc == NULL || parent == NULL || from == NULL ) {
		return NULL;
	}
	if ( from->kind == TAD_ND_TEXT ) {
		return tad_text_new(doc, parent, before, from->text,
				    ( from->text != NULL ) ? (INT)tad_slen((CONST char *)from->text) : 0);
	}
	if ( from->kind != TAD_ND_ELEM || from->name == NULL ) {
		return NULL;
	}
	e = tad_elem_new(doc, (CONST char *)from->name, parent, before,
			 (BOOL)( from->first == NULL && from->empty ));
	for ( i = 0; e != NULL && i < from->nattr; i++ ) {
		(void)tad_set_attr(doc, e, (CONST char *)from->attr[i].name, from->attr[i].value);
	}
	/* what it holds, in order; a node being copied into itself stops at its own copy */
	for ( c = from->first; e != NULL && c != NULL && c != e; c = c->next ) {
		(void)tad_node_copy(doc, e, NULL, c);
	}
	return e;
}

EXPORT void tad_node_move( T_TADNODE *nd, T_TADNODE *parent,
			   T_TADNODE *before )
{
	if ( nd == NULL || parent == NULL || nd == parent ) {
		return;
	}
	ed_unhook(nd);
	ed_hook(parent, nd, before);
}

EXPORT void tad_node_remove( T_TADNODE *nd )
{
	if ( nd != NULL ) {
		ed_unhook(nd);
	}
}

/* An element made again, with the same name and attributes and no children */
LOCAL T_TADNODE *ed_clone( T_TAD *doc, CONST T_TADNODE *src )
{
	T_TADNODE	*nd = tad_new_node(doc, TAD_ND_ELEM);

	if ( nd == NULL ) {
		return NULL;
	}
	nd->name  = src->name;
	nd->empty = FALSE;
	/*
	 * An array of its own: an attribute is set by writing into the
	 * array, and a copy that shared it would change with the original.
	 * The names and values themselves are only ever replaced, so those
	 * can be shared.
	 */
	if ( src->nattr > 0 ) {
		INT	i;

		nd->attr = (T_TADATTR *)tad_alloc(doc,
					sizeof(T_TADATTR) * (SZ)src->nattr);
		if ( nd->attr == NULL ) {
			return NULL;
		}
		for ( i = 0; i < src->nattr; i++ ) {
			nd->attr[i] = src->attr[i];
		}
		nd->nattr = src->nattr;
	}

	return nd;
}

EXPORT T_TADNODE *tad_text_split( T_TAD *doc, T_TADNODE *nd, INT at )
{
	T_TADNODE	*two;
	INT		len = 0;

	if ( doc == NULL || nd == NULL || nd->kind != TAD_ND_TEXT
	  || nd->parent == NULL ) {
		return NULL;
	}
	while ( nd->text != NULL && nd->text[len] != 0 ) {
		len++;
	}
	if ( at < 0 ) at = 0;
	if ( at > len ) at = len;
	two = tad_new_node(doc, TAD_ND_TEXT);
	if ( two == NULL ) {
		return NULL;
	}
	two->text = tad_dup(doc, nd->text + at, (SZ)( len - at ));
	if ( two->text == NULL
	  || tad_text_splice(doc, nd, at, len - at, NULL, 0) < E_OK ) {
		return NULL;
	}
	ed_hook_after(two, nd);

	return two;
}

/*
 * Everything from 'c' to the end of 'upto', moved into a copy of each
 * element on the way up, the copy of 'upto' put right after it. The
 * elements a stretch of text sits in -- the size and the colour of its
 * letters -- go on round the part that was moved.
 */
EXPORT T_TADNODE *tad_split_before( T_TAD *doc, T_TADNODE *c,
				    T_TADNODE *upto )
{
	T_TADNODE	*cur = c, *par, *copy, *n, *next;

	if ( doc == NULL || c == NULL || upto == NULL ) {
		return NULL;
	}
	for ( par = c->parent; par != NULL; par = par->parent ) {
		copy = ed_clone(doc, par);
		if ( copy == NULL ) {
			return NULL;
		}
		/* cur and everything after it under par go to the copy */
		for ( n = cur; n != NULL; n = next ) {
			next = n->next;
			ed_unhook(n);
			tad_add_child(copy, n);
		}
		if ( par->parent != NULL ) {
			ed_hook_after(copy, par);
		}
		if ( par == upto ) {
			return copy;
		}
		cur = copy;
	}

	return NULL;				/* 'upto' was not above 'c' */
}

EXPORT void tad_join( T_TADNODE *a, T_TADNODE *b )
{
	T_TADNODE	*n, *next;

	if ( a == NULL || b == NULL || a == b ) {
		return;
	}
	for ( n = b->first; n != NULL; n = next ) {
		next = n->next;
		ed_unhook(n);
		tad_add_child(a, n);
	}
	a->empty = FALSE;
	ed_unhook(b);
}

/*
 * A run of siblings, from 'first' to 'last', put inside a new element
 * that takes their place.
 */
EXPORT T_TADNODE *tad_wrap( T_TAD *doc, T_TADNODE *first, T_TADNODE *last,
			    CONST char *name )
{
	T_TADNODE	*parent, *w, *n, *next;

	if ( doc == NULL || first == NULL || last == NULL
	  || first->parent == NULL || last->parent != first->parent ) {
		return NULL;
	}
	parent = first->parent;
	w = tad_elem_new(doc, name, parent, first, FALSE);
	if ( w == NULL ) {
		return NULL;
	}
	for ( n = first; n != NULL; n = next ) {
		next = n->next;
		ed_unhook(n);
		tad_add_child(w, n);
		if ( n == last ) {
			break;
		}
	}

	return w;
}
