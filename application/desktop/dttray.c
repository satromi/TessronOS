/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttray.c
 *	The desktop's windows and the tray (design 18.16)
 *
 *	What a window copies goes to the tray as a set of one xmlTAD
 *	fragment: a <figure> of shapes and <link>s from a figure or a
 *	cabinet, a <document> of paragraphs from a text. Whatever window
 *	takes from the tray reads the fragment of the set in hand back and
 *	takes what it can use of it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/tad.h>
#include <ts/tray.h>
#include "desktop.h"

#define FRAG_MAX	( 4 * 1024 * 1024 )	/* bytes of one fragment */

EXPORT T_TAD *dt_frag_new( CONST char *body )
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

EXPORT ER dt_tray_put( CONST T_TAD *frag, CONST char *name )
{
	T_TRREC	r;
	UB	*buf;
	SZ	size = 0;
	ER	er;

	if ( frag == NULL ) {
		return E_PAR;
	}
	(void)tad_write_mem(frag, NULL, 0, &size);
	buf = (UB *)Kmalloc(size + 1);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = tad_write_mem(frag, buf, size + 1, &size);
	if ( er >= E_OK ) {
		r.kind = TR_TAD;
		r.rt = 0;
		r.sub = 0;
		r.data = buf;
		r.size = size;
		er = tr_psh_dat(&r, 1, (CONST UB *)name);
	}
	Kfree(buf);
	return er;
}

/* The first xmlTAD fragment of the set in hand, as it is written */
LOCAL UB *hand_bytes( SZ *p_size )
{
	T_TRINF	inf;
	UB	*buf;
	SZ	asz = 0;
	INT	i;

	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) {
			return NULL;
		}
		if ( inf.kind == TR_TAD && inf.size > 0 && inf.size < FRAG_MAX ) {
			break;
		}
	}
	if ( i == TR_REC_MAX ) {
		return NULL;
	}
	buf = (UB *)Kmalloc(inf.size + 1);
	if ( buf != NULL && tr_rea_rec(TR_HAND, i, buf, inf.size, &asz) < E_OK ) {
		Kfree(buf);
		return NULL;
	}
	*p_size = inf.size;
	return buf;
}

EXPORT T_TAD *dt_tray_frag( void )
{
	T_TAD	*doc = NULL;
	UB	*buf;
	SZ	size = 0;

	buf = hand_bytes(&size);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tad_parse(buf, size, NULL, &doc) < E_OK ) {
		doc = NULL;
	}
	Kfree(buf);
	return doc;
}

EXPORT UINT dt_tray_kind( INT *p_links )
{
	T_TAD		*doc = dt_tray_frag();
	T_TADNODE	*body;
	UINT		kind = DT_TRAY_NONE;

	if ( p_links != NULL ) {
		*p_links = 0;
	}
	if ( doc == NULL ) {
		return kind;
	}
	body = tad_body(doc);
	if ( body != NULL && body->name != NULL ) {
		kind = ( body->name[0] == 'f' ) ? DT_TRAY_FIG
		     : ( body->name[0] == 'd' ) ? DT_TRAY_DOC : DT_TRAY_NONE;
	}
	if ( p_links != NULL ) {
		*p_links = tad_lnk_count(doc);
	}
	tad_free(doc);
	return kind;
}

/* The shapes of a figure fragment: every element of its body but the links */
EXPORT INT dt_frag_shapes( CONST T_TAD *frag )
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

EXPORT void dt_tray_taken( void )
{
	(void)tr_del_dat(TR_HAND);
}
