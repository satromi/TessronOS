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
 *
 *	A fragment may stand for a picture the set carries beside it: an
 *	element whose href is "tray:N" names the PNG in record N of the set
 *	(TR_PNG), so that a picture can be in the tray without an object to
 *	hold it. Whatever pastes such a fragment into a figure writes each
 *	of those pictures beside its own record first (dt_tray_pictures).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/tad.h>
#include <ts/tray.h>
#include <ts/om.h>
#include <ts/uuid.h>
#include "desktop.h"


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
		if ( inf.kind == TR_TAD && inf.size > 0 ) {
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

/* The number after the last '_' of an href of a picture beside a record ("<object>_0_<n>.png") */
LOCAL INT pic_number( CONST UB *h )
{
	INT	j, v = 0, under = 0;

	for ( j = 0; h != NULL && h[j] != 0 && h[j] != '.'; j++ ) {
		if ( h[j] == '_' ) {
			under++;
			v = 0;
		} else if ( under == 2 && h[j] >= '0' && h[j] <= '9' ) {
			v = v * 10 + ( h[j] - '0' );
		}
	}
	return ( under == 2 ) ? v : 0;
}

/* The highest number the pictures beside a record have, in the record and the fragment */
LOCAL INT pic_top( CONST T_TAD *rec, CONST T_TAD *frag )
{
	CONST T_TAD	*docs[2];
	T_TADNODE	*n;
	INT		top = 0, k, v;

	docs[0] = rec;
	docs[1] = frag;
	for ( k = 0; k < 2; k++ ) {
		if ( docs[k] == NULL ) continue;
		for ( n = tad_walk(docs[k], NULL); n != NULL; n = tad_walk(docs[k], n) ) {
			if ( n->kind != TAD_ND_ELEM ) continue;
			v = pic_number(tad_attr(n, "href"));
			if ( v > top ) top = v;
		}
	}
	return top;
}

/*
 * The pictures a fragment from the tray names by its set ("tray:N"),
 * written beside the record of 'id' (whose text is 'rec') under names of
 * their own, "<id>_0_<n>.png" past every number the record and the
 * fragment use, and the fragment's href made that name. Answers how many.
 */
EXPORT INT dt_tray_pictures( CONST TS_UUID *id, CONST T_TAD *rec, T_TAD *frag )
{
	T_TADNODE	*n;
	T_TRINF		inf;
	CONST UB	*h;
	char		us[TS_UUID_STRLEN + 1];
	UB		name[TS_UUID_STRLEN + 24], *png;
	SZ		asz = 0;
	INT		done = 0, rn, i, k, v;

	if ( id == NULL || frag == NULL || ts_uuid_to_str(id, us, sizeof(us)) < E_OK ) {
		return 0;
	}
	for ( n = tad_walk(frag, NULL); n != NULL; n = tad_walk(frag, n) ) {
		h = ( n->kind == TAD_ND_ELEM ) ? tad_attr(n, "href") : NULL;
		if ( h == NULL || h[0] != 't' || h[1] != 'r' || h[2] != 'a' || h[3] != 'y' || h[4] != ':' ) {
			continue;
		}
		for ( rn = 0, i = 5; h[i] >= '0' && h[i] <= '9'; i++ ) rn = rn * 10 + ( h[i] - '0' );
		if ( tr_ref_rec(TR_HAND, rn, &inf) < E_OK || inf.kind != TR_PNG || inf.size <= 0 ) {
			continue;
		}
		png = (UB *)Kmalloc(inf.size);
		if ( png == NULL ) {
			continue;
		}
		if ( tr_rea_rec(TR_HAND, rn, png, inf.size, &asz) >= E_OK ) {
			char	t[12];

			for ( k = 0; us[k] != 0; k++ ) name[k] = (UB)us[k];
			name[k++] = '_';
			name[k++] = '0';
			name[k++] = '_';
			v = pic_top(rec, frag) + 1;
			for ( i = 0; v > 0 || i == 0; v /= 10 ) t[i++] = (char)( '0' + v % 10 );
			while ( i > 0 ) name[k++] = (UB)t[--i];
			name[k++] = '.';  name[k++] = 'p';  name[k++] = 'n';  name[k++] = 'g';
			name[k] = 0;
			if ( om_obj_res_put(id, name + TS_UUID_STRLEN, png, asz) >= E_OK
			  && tad_set_attr(frag, n, "href", name) >= E_OK ) {
				tm_printf((UB *)"tray: picture %s\n", name);
				done++;
			}
		}
		Kfree(png);
	}
	return done;
}

EXPORT void dt_tray_taken( void )
{
	(void)tr_del_dat(TR_HAND);
}
