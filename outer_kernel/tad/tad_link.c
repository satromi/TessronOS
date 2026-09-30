/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad_link.c
 *	The virtual objects of a document: the <link> elements (design
 *	16.3.2, xmlTAD specification 5)
 *
 *	A <link> refers to another real object by UUID and carries how the
 *	frame around it is to be drawn. Reading one gives a T_VOBJ; adding
 *	or removing one changes the tree and notes what the reference count
 *	of the object it points at now owes, which lib/libom settles when
 *	the document is written (design 16.3.4).
 *
 *	The id attribute is either the UUID of the object or the name of
 *	one of its records, "{uuid}_N.xtad" (design 11.3). Both are read;
 *	the record number is kept so that writing gives back the same form.
 */

#include <tk/tkernel.h>
#include "tad_local.h"

#define LNK_MAX_ATTR	40		/* attributes a link built here has */

LOCAL CONST TS_UUID tad_uuid_zero = { { 0 } };

/* ---------------------------------------------------------------- text */

LOCAL void cpy( UB *dst, CONST UB *src, SZ n )
{
	while ( n-- > 0 ) *dst++ = *src++;
}

LOCAL BOOL uuid_zero( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( u->b[i] != 0 ) return FALSE;
	}

	return TRUE;
}

/* The value of an attribute into a field of the caller, whole or not at all */
LOCAL ER cpy_attr( CONST T_TADNODE *nd, CONST char *name, UB *out, SZ max )
{
	CONST UB	*v = tad_attr(nd, name);
	SZ		len;

	out[0] = '\0';
	if ( v == NULL ) {
		return E_OK;
	}
	len = tad_slen((CONST char *)v);
	if ( len >= max ) {
		return E_LIMIT;
	}
	cpy(out, v, len);
	out[len] = '\0';

	return E_OK;
}

LOCAL INT to_num( CONST UB *s, INT dflt )
{
	INT	v = 0, neg = 0, digits = 0;

	if ( s == NULL ) {
		return dflt;
	}
	if ( *s == '-' ) { neg = 1; s++; }
	while ( *s >= '0' && *s <= '9' ) {
		v = v * 10 + (*s - '0');
		s++;
		digits++;
	}
	if ( digits == 0 ) {
		return dflt;
	}

	return ( neg ) ? -v : v;
}

/* "true" and "false"; anything else is the default */
LOCAL BOOL to_bool( CONST UB *s, BOOL dflt )
{
	if ( s == NULL ) {
		return dflt;
	}
	if ( tad_same(s, "true") ) return TRUE;
	if ( tad_same(s, "false") ) return FALSE;

	return dflt;
}

/* "#rrggbb" to 0x00rrggbb; anything else is TAD_COL_NONE */
LOCAL UW to_col( CONST UB *s )
{
	UW	v = 0;
	INT	i;

	if ( s == NULL || s[0] != '#' ) {
		return TAD_COL_NONE;
	}
	for ( i = 1; i <= 6; i++ ) {
		UB	c = s[i];
		UW	d;

		if ( c >= '0' && c <= '9' ) d = (UW)(c - '0');
		else if ( c >= 'a' && c <= 'f' ) d = (UW)(c - 'a' + 10);
		else if ( c >= 'A' && c <= 'F' ) d = (UW)(c - 'A' + 10);
		else return TAD_COL_NONE;
		v = (v << 4) | d;
	}
	if ( s[7] != '\0' ) {
		return TAD_COL_NONE;
	}

	return v;
}

/* The magnification as per cent: "1" is 100, "1.5" is 150 */
LOCAL INT to_zoom( CONST UB *s )
{
	INT	v, frac = 0, scale = 10;

	if ( s == NULL ) {
		return 100;
	}
	v = to_num(s, 1) * 100;
	while ( *s == '-' || (*s >= '0' && *s <= '9') ) s++;
	if ( *s == '.' ) {
		s++;
		while ( *s >= '0' && *s <= '9' && scale > 0 ) {
			frac += (*s - '0') * scale;
			scale /= 10;
			s++;
		}
	}

	return ( v < 0 ) ? v - frac : v + frac;
}

LOCAL SZ put_num( UB *out, INT v )
{
	UB	tmp[12];
	SZ	n = 0;
	INT	i = 0;

	if ( v < 0 ) {
		out[n++] = '-';
		v = -v;
	}
	do {
		tmp[i++] = (UB)('0' + (v % 10));
		v /= 10;
	} while ( v > 0 );
	while ( i > 0 ) out[n++] = tmp[--i];
	out[n] = '\0';

	return n;
}

LOCAL void put_col( UB *out, UW v )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	out[0] = '#';
	for ( i = 0; i < 6; i++ ) {
		out[1 + i] = (UB)hex[(v >> (20 - 4 * i)) & 0xF];
	}
	out[7] = '\0';
}

/* The per cent back as it is written: 100 is "1", 150 is "1.5" */
LOCAL void put_zoom( UB *out, INT v )
{
	SZ	n;

	if ( v < 0 ) {
		v = 100;
	}
	n = put_num(out, v / 100);
	if ( v % 100 != 0 ) {
		out[n++] = '.';
		out[n++] = (UB)('0' + ((v / 10) % 10));
		if ( v % 10 != 0 ) {
			out[n++] = (UB)('0' + (v % 10));
		}
		out[n] = '\0';
	}
}

/* ---------------------------------------------------------------- reading */

LOCAL BOOL is_link( CONST T_TADNODE *nd )
{
	return ( nd->kind == TAD_ND_ELEM && tad_same(nd->name, "link") );
}

LOCAL T_TADNODE *nth_link( CONST T_TAD *doc, INT i )
{
	T_TADNODE	*nd;
	INT		n = 0;

	for ( nd = tad_walk(doc, NULL); nd != NULL; nd = tad_walk(doc, nd) ) {
		if ( !is_link(nd) ) continue;
		if ( n == i ) return nd;
		n++;
	}

	return NULL;
}

/*
 * The id attribute: the UUID of the object, and the record of it the
 * name points at, which is 0 when the id is the bare UUID.
 */
LOCAL ER id_of( CONST T_TADNODE *nd, TS_UUID *p_uuid, INT *p_recno )
{
	CONST UB	*id = tad_attr(nd, "id");
	SZ		len;
	ER		er;

	if ( id == NULL ) {
		return E_PAR;
	}
	len = tad_slen((CONST char *)id);
	if ( len < TS_UUID_STRLEN ) {
		return E_PAR;
	}
	er = ts_str_to_uuid((CONST char *)id, p_uuid);
	if ( er < E_OK ) {
		return E_PAR;
	}
	*p_recno = 0;
	if ( len > TS_UUID_STRLEN ) {
		if ( id[TS_UUID_STRLEN] != '_' ) {
			return E_PAR;		/* neither a UUID nor a record name */
		}
		*p_recno = to_num(id + TS_UUID_STRLEN + 1, 0);
	}

	return E_OK;
}

LOCAL ER vobj_of( CONST T_TADNODE *nd, T_VOBJ *pk )
{
	CONST UB	*v;
	ER		er;
	INT		i;

	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) {
		((UB *)pk)[i] = 0;
	}
	er = id_of(nd, &pk->target, &pk->recno);
	if ( er < E_OK ) {
		return er;
	}
	v = tad_attr(nd, "vobjid");
	if ( v != NULL && ts_str_to_uuid((CONST char *)v, &pk->vobjid) < E_OK ) {
		return E_PAR;
	}

	er = cpy_attr(nd, "name", pk->name, TAD_NAME_MAX);
	if ( er >= E_OK ) er = cpy_attr(nd, "relationship", pk->relationship,
					TAD_REL_MAX);
	if ( er >= E_OK ) er = cpy_attr(nd, "applist", pk->applist, TAD_APPL_MAX);
	if ( er < E_OK ) {
		return er;
	}
	if ( pk->name[0] == '\0' && nd->first != NULL
	  && nd->first->kind == TAD_ND_TEXT && nd->first->text != NULL ) {
		/* with no name attribute the text of the element is the name */
		SZ len = tad_slen((CONST char *)nd->first->text);

		if ( len >= TAD_NAME_MAX ) {
			return E_LIMIT;
		}
		cpy(pk->name, nd->first->text, len);
		pk->name[len] = '\0';
	}

	pk->left     = to_num(tad_attr(nd, "vobjleft"), 0);
	pk->top      = to_num(tad_attr(nd, "vobjtop"), 0);
	pk->right    = to_num(tad_attr(nd, "vobjright"), 0);
	pk->bottom   = to_num(tad_attr(nd, "vobjbottom"), 0);
	pk->height   = to_num(tad_attr(nd, "height"), 0);
	pk->width    = to_num(tad_attr(nd, "width"), 0);
	pk->heightpx = to_num(tad_attr(nd, "heightpx"), 0);
	if ( pk->heightpx == 0 ) {
		pk->heightpx = to_num(tad_attr(nd, "heightPx"), 0);	/* the same, as some records spell it */
	}

	pk->frcol = to_col(tad_attr(nd, "frcol"));
	pk->chcol = to_col(tad_attr(nd, "chcol"));
	pk->tbcol = to_col(tad_attr(nd, "tbcol"));
	pk->bgcol = to_col(tad_attr(nd, "bgcol"));

	pk->chsz = to_num(tad_attr(nd, "chsz"), 0);
	pk->dlen = to_num(tad_attr(nd, "dlen"), 0);

	pk->disp = 0;
	if ( to_bool(tad_attr(nd, "namedisp"), TRUE) )    pk->disp |= TAD_D_NAME;
	if ( to_bool(tad_attr(nd, "roledisp"), FALSE) )   pk->disp |= TAD_D_ROLE;
	if ( to_bool(tad_attr(nd, "typedisp"), FALSE) )   pk->disp |= TAD_D_TYPE;
	if ( to_bool(tad_attr(nd, "updatedisp"), FALSE) ) pk->disp |= TAD_D_UPDATE;
	if ( to_bool(tad_attr(nd, "framedisp"), TRUE) )   pk->disp |= TAD_D_FRAME;
	if ( to_bool(tad_attr(nd, "pictdisp"), TRUE) )    pk->disp |= TAD_D_PICT;

	pk->autoopen = to_bool(tad_attr(nd, "autoopen"), FALSE);
	pk->fixed    = to_bool(tad_attr(nd, "fixed"), FALSE);
	pk->background = to_bool(tad_attr(nd, "background"), FALSE);
	pk->hidden   = to_bool(tad_attr(nd, "hidden"), FALSE);

	pk->scrollx = to_num(tad_attr(nd, "scrollx"), 0);
	pk->scrolly = to_num(tad_attr(nd, "scrolly"), 0);
	pk->zoom    = to_zoom(tad_attr(nd, "zoomratio"));

	/* both spellings are in the records */
	v = tad_attr(nd, "viewMode");
	if ( v == NULL ) v = tad_attr(nd, "viewmode");
	if ( v != NULL ) {
		SZ len = tad_slen((CONST char *)v);

		if ( len < TAD_VIEW_MAX ) {
			cpy(pk->viewmode, v, len);
			pk->viewmode[len] = '\0';
		}
	}
	v = tad_attr(nd, "wordWrap");
	if ( v == NULL ) v = tad_attr(nd, "wordwrap");
	pk->wordwrap = ( v == NULL ) ? TAD_WRAP_NONE : to_bool(v, TRUE) ? TAD_WRAP_ON : TAD_WRAP_OFF;
	pk->vobjheight = to_num(tad_attr(nd, "vobjheight"), 0);
	v = tad_attr(nd, "zIndex");
	if ( v == NULL ) v = tad_attr(nd, "zindex");
	if ( v != NULL ) {
		pk->has_zindex = TRUE;
		pk->zindex = to_num(v, 0);
	}

	return E_OK;
}

EXPORT INT tad_lnk_count( CONST T_TAD *doc )
{
	T_TADNODE	*nd;
	INT		n = 0;

	if ( doc == NULL ) {
		return 0;
	}
	for ( nd = tad_walk(doc, NULL); nd != NULL; nd = tad_walk(doc, nd) ) {
		if ( is_link(nd) ) n++;
	}

	return n;
}

EXPORT ER tad_lnk_get( CONST T_TAD *doc, INT i, T_VOBJ *pk_vobj )
{
	T_TADNODE	*nd;

	if ( doc == NULL || pk_vobj == NULL || i < 0 ) {
		return E_PAR;
	}
	nd = nth_link(doc, i);
	if ( nd == NULL ) {
		return E_NOEXS;
	}

	return vobj_of(nd, pk_vobj);
}

LOCAL T_TADNODE *find_link( CONST T_TAD *doc, CONST TS_UUID *vobjid )
{
	T_TADNODE	*nd;
	TS_UUID		id;

	for ( nd = tad_walk(doc, NULL); nd != NULL; nd = tad_walk(doc, nd) ) {
		CONST UB *v;

		if ( !is_link(nd) ) continue;
		v = tad_attr(nd, "vobjid");
		if ( v == NULL ) continue;
		if ( ts_str_to_uuid((CONST char *)v, &id) < E_OK ) continue;
		if ( ts_uuid_cmp(&id, vobjid) == 0 ) return nd;
	}

	return NULL;
}

EXPORT ER tad_lnk_find( CONST T_TAD *doc, CONST TS_UUID *vobjid, T_VOBJ *pk_vobj )
{
	T_TADNODE	*nd;

	if ( doc == NULL || vobjid == NULL || pk_vobj == NULL ) {
		return E_PAR;
	}
	nd = find_link(doc, vobjid);
	if ( nd == NULL ) {
		return E_NOEXS;
	}

	return vobj_of(nd, pk_vobj);
}

/* ---------------------------------------------------------------- editing */

typedef struct {
	T_TAD		*doc;
	T_TADNODE	*nd;
	INT		n;		/* attributes put in so far */
	ER		er;
} LNKBLD;

LOCAL void put_attr( LNKBLD *b, CONST char *name, CONST UB *value )
{
	UB	*nm, *vl;

	if ( b->er < E_OK || b->n >= LNK_MAX_ATTR ) {
		if ( b->er >= E_OK ) b->er = E_LIMIT;
		return;
	}
	nm = tad_dup(b->doc, (CONST UB *)name, tad_slen(name));
	vl = tad_dup(b->doc, value, tad_slen((CONST char *)value));
	if ( nm == NULL || vl == NULL ) {
		b->er = E_NOMEM;
		return;
	}
	b->nd->attr[b->n].name  = nm;
	b->nd->attr[b->n].value = vl;
	b->n++;
}

LOCAL void put_attr_num( LNKBLD *b, CONST char *name, INT v )
{
	UB	num[16];

	put_num(num, v);
	put_attr(b, name, num);
}

LOCAL void put_attr_bool( LNKBLD *b, CONST char *name, BOOL v )
{
	put_attr(b, name, (CONST UB *)( ( v ) ? "true" : "false" ));
}

LOCAL void put_attr_col( LNKBLD *b, CONST char *name, UW v )
{
	UB	col[10];

	if ( v == TAD_COL_NONE ) {
		return;			/* a colour that was never given */
	}
	put_col(col, v);
	put_attr(b, name, col);
}

/*
 * Build the element of a virtual object. The attributes go in the order
 * the specification sets them out, and only those that say something are
 * written: a colour that was not given and an empty name, relationship
 * or applist leave no attribute behind.
 */
LOCAL ER build_link( T_TAD *doc, CONST T_VOBJ *pk, T_TADNODE **p_nd )
{
	LNKBLD	b;
	UB	id[TS_UUID_STRLEN + 16];
	UB	num[16];
	SZ	n;
	ER	er;

	b.doc = doc;
	b.n   = 0;
	b.er  = E_OK;
	b.nd  = tad_new_node(doc, TAD_ND_ELEM);
	if ( b.nd == NULL ) {
		return E_LIMIT;
	}
	b.nd->name = tad_dup(doc, (CONST UB *)"link", 4);
	b.nd->attr = (T_TADATTR *)tad_alloc(doc, sizeof(T_TADATTR) * LNK_MAX_ATTR);
	b.nd->empty = TRUE;
	if ( b.nd->name == NULL || b.nd->attr == NULL ) {
		return E_NOMEM;
	}

	er = ts_uuid_to_str(&pk->vobjid, (char *)id, sizeof(id));
	if ( er < E_OK ) {
		return er;
	}
	put_attr(&b, "vobjid", id);

	er = ts_uuid_to_str(&pk->target, (char *)id, sizeof(id));
	if ( er < E_OK ) {
		return er;
	}
	n = TS_UUID_STRLEN;
	id[n++] = '_';
	n += put_num(id + n, pk->recno);
	id[n++] = '.';
	id[n++] = 'x';
	id[n++] = 't';
	id[n++] = 'a';
	id[n++] = 'd';
	id[n] = '\0';
	put_attr(&b, "id", id);

	if ( pk->name[0] != '\0' ) {
		put_attr(&b, "name", pk->name);
	}
	put_attr_col(&b, "tbcol", pk->tbcol);
	put_attr_col(&b, "frcol", pk->frcol);
	put_attr_col(&b, "chcol", pk->chcol);
	put_attr_col(&b, "bgcol", pk->bgcol);

	if ( pk->width != 0 || pk->heightpx != 0 ) {
		/* in a document a link takes a width and a height */
		put_attr_num(&b, "width", pk->width);
		put_attr_num(&b, "heightpx", pk->heightpx);
	} else {
		/* in a figure it takes the rectangle it sits in */
		put_attr_num(&b, "vobjleft", pk->left);
		put_attr_num(&b, "vobjtop", pk->top);
		put_attr_num(&b, "vobjright", pk->right);
		put_attr_num(&b, "vobjbottom", pk->bottom);
		put_attr_num(&b, "height", pk->height);
	}
	put_attr_num(&b, "dlen", pk->dlen);
	put_attr_num(&b, "chsz", pk->chsz);

	put_attr_bool(&b, "framedisp", (pk->disp & TAD_D_FRAME) != 0);
	put_attr_bool(&b, "namedisp", (pk->disp & TAD_D_NAME) != 0);
	put_attr_bool(&b, "pictdisp", (pk->disp & TAD_D_PICT) != 0);
	put_attr_bool(&b, "roledisp", (pk->disp & TAD_D_ROLE) != 0);
	put_attr_bool(&b, "typedisp", (pk->disp & TAD_D_TYPE) != 0);
	put_attr_bool(&b, "updatedisp", (pk->disp & TAD_D_UPDATE) != 0);
	put_attr_bool(&b, "autoopen", pk->autoopen);
	if ( pk->fixed ) {
		put_attr_bool(&b, "fixed", TRUE);
	}
	if ( pk->background ) {
		put_attr_bool(&b, "background", TRUE);
	}
	if ( pk->hidden ) {
		put_attr_bool(&b, "hidden", TRUE);
	}
	put_attr_num(&b, "scrollx", pk->scrollx);
	put_attr_num(&b, "scrolly", pk->scrolly);
	put_zoom(num, ( pk->zoom > 0 ) ? pk->zoom : 100);
	put_attr(&b, "zoomratio", num);
	if ( pk->applist[0] != '\0' ) {
		put_attr(&b, "applist", pk->applist);
	}
	if ( pk->relationship[0] != '\0' ) {
		put_attr(&b, "relationship", pk->relationship);
	}
	if ( pk->viewmode[0] != '\0' ) {
		put_attr(&b, "viewMode", pk->viewmode);
	}
	if ( pk->wordwrap != TAD_WRAP_NONE ) {
		put_attr_bool(&b, "wordWrap", pk->wordwrap == TAD_WRAP_ON);
	}
	if ( pk->vobjheight != 0 ) {
		put_attr_num(&b, "vobjheight", pk->vobjheight);
	}
	if ( pk->has_zindex ) {
		put_attr_num(&b, "zIndex", pk->zindex);
	}
	if ( b.er < E_OK ) {
		return b.er;
	}
	b.nd->nattr = b.n;
	*p_nd = b.nd;

	return E_OK;
}

EXPORT ER tad_lnk_add( T_TAD *doc, CONST T_VOBJ *pk_vobj, TS_UUID *p_vobjid )
{
	T_TADNODE	*body, *nd, *nl;
	T_VOBJ		*pk;
	ER		er;

	if ( doc == NULL || pk_vobj == NULL ) {
		return E_PAR;
	}
	if ( uuid_zero(&pk_vobj->target) ) {
		return E_PAR;			/* a link has to point somewhere */
	}
	body = tad_body(doc);
	if ( body == NULL || body->empty ) {
		return E_OBJ;			/* no <document> or <figure> to put it in */
	}

	/*
	 * The caller may leave the identity of the link itself unset, in
	 * which case it gets one of its own. The copy is made because that
	 * is the only field written back.
	 */
	pk = (T_VOBJ *)tad_alloc(doc, sizeof(T_VOBJ));
	if ( pk == NULL ) {
		return E_NOMEM;
	}
	*pk = *pk_vobj;
	if ( uuid_zero(&pk->vobjid) ) {
		er = ts_gen_uuid(&pk->vobjid);
		if ( er < E_OK ) {
			return er;
		}
	} else if ( find_link(doc, &pk->vobjid) != NULL ) {
		return E_OBJ;			/* that identity is already in here */
	}

	er = build_link(doc, pk, &nd);
	if ( er < E_OK ) {
		return er;
	}

	/* a newline after it, so that the text keeps one element to a line */
	nl = tad_new_node(doc, TAD_ND_TEXT);
	if ( nl == NULL ) {
		return E_LIMIT;
	}
	nl->text = tad_dup(doc, (CONST UB *)"\n", 1);
	if ( nl->text == NULL ) {
		return E_NOMEM;
	}
	tad_add_child(body, nd);
	tad_add_child(body, nl);

	er = tad_put_delta(doc, &pk->target, 1);
	if ( er < E_OK ) {
		return er;
	}
	if ( p_vobjid != NULL ) {
		*p_vobjid = pk->vobjid;
	}

	return E_OK;
}

/*
 * Change what an existing <link> says. The element keeps its place among
 * its siblings and its identity; what changes is its attributes, and the
 * source text it was parsed from is dropped so that the writer puts out
 * the new ones. Every other node still carries its own bytes, so the
 * rest of the document comes back exactly as it went in.
 *
 * What it points at cannot be changed here. A link to another real
 * object is a different reference, and the counts of the two objects
 * would both have to move; that is an add and a delete, and saying so
 * keeps the counting in one place.
 */
/* Two attribute names that say the same thing: the records spell some two ways */
LOCAL BOOL same_attr( CONST UB *a, CONST UB *b )
{
	LOCAL CONST char * CONST twins[][2] = {
		{ "heightpx", "heightPx" }, { "viewMode", "viewmode" },
		{ "wordWrap", "wordwrap" }, { "zIndex", "zindex" },
	};
	UINT	i;

	if ( tad_same(a, (CONST char *)b) ) {
		return TRUE;
	}
	for ( i = 0; i < sizeof(twins) / sizeof(twins[0]); i++ ) {
		if ( ( tad_same(a, twins[i][0]) && tad_same(b, twins[i][1]) )
		  || ( tad_same(a, twins[i][1]) && tad_same(b, twins[i][0]) ) ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT ER tad_lnk_set( T_TAD *doc, CONST T_VOBJ *pk_vobj )
{
	T_TADNODE	*nd, *fresh;
	TS_UUID		target = tad_uuid_zero;
	INT		recno = 0;
	ER		er;

	if ( doc == NULL || pk_vobj == NULL ) {
		return E_PAR;
	}
	nd = find_link(doc, &pk_vobj->vobjid);
	if ( nd == NULL ) {
		return E_NOEXS;
	}
	er = id_of(nd, &target, &recno);
	if ( er < E_OK ) {
		return er;
	}
	if ( ts_uuid_cmp(&target, &pk_vobj->target) != 0 ) {
		return E_PAR;			/* that would be a different reference */
	}
	er = build_link(doc, pk_vobj, &fresh);
	if ( er < E_OK ) {
		return er;
	}

	/*
	 * Keep what this library does not model. The format carries more
	 * about a link than a T_VOBJ holds, and an attribute nobody here
	 * understands still means something to whoever wrote it. So the
	 * attributes that were there stay, in their order, with their
	 * values replaced by the new ones where the two name the same
	 * thing; what only the new one has goes on the end.
	 *
	 * Without this, moving a virtual object would quietly drop every
	 * attribute this version does not know about, which is the one
	 * thing a program that shares a format with another must not do.
	 */
	{
		T_TADATTR	*merged;
		INT		total = nd->nattr + fresh->nattr;
		INT		i, k, n = 0;

		merged = (T_TADATTR *)tad_alloc(doc, sizeof(T_TADATTR) * total);
		if ( merged == NULL ) {
			return E_NOMEM;
		}
		for ( i = 0; i < nd->nattr; i++ ) {
			merged[n] = nd->attr[i];
			for ( k = 0; k < fresh->nattr; k++ ) {
				if ( same_attr(nd->attr[i].name, fresh->attr[k].name) ) {
					merged[n].value = fresh->attr[k].value;
					break;
				}
			}
			/*
			 * A protection that was there and is now taken off is
			 * written as off. The new element leaves it out, which
			 * would otherwise leave the old "true" standing.
			 */
			if ( k == fresh->nattr
			  && ( tad_same(nd->attr[i].name, "fixed")
			    || tad_same(nd->attr[i].name, "background") ) ) {
				merged[n].value = tad_dup(doc, (CONST UB *)"false", 5);
			}
			n++;
		}
		for ( k = 0; k < fresh->nattr; k++ ) {
			BOOL had = FALSE;

			for ( i = 0; i < nd->nattr; i++ ) {
				if ( same_attr(nd->attr[i].name, fresh->attr[k].name) ) {
					had = TRUE;
					break;
				}
			}
			if ( !had ) {
				merged[n++] = fresh->attr[k];
			}
		}
		nd->attr  = merged;
		nd->nattr = n;
	}
	nd->name   = fresh->name;
	nd->raw    = NULL;			/* write it from the attributes now */
	nd->rawlen = 0;

	return E_OK;
}

/* Take a node out of the list of children of its parent */
LOCAL void unhook( T_TADNODE *nd )
{
	T_TADNODE	*parent = nd->parent, *prev;

	if ( parent == NULL ) {
		return;
	}
	if ( parent->first == nd ) {
		parent->first = nd->next;
	} else {
		for ( prev = parent->first; prev != NULL; prev = prev->next ) {
			if ( prev->next == nd ) {
				prev->next = nd->next;
				break;
			}
		}
	}
	if ( parent->last == nd ) {
		T_TADNODE *last = NULL, *p;

		for ( p = parent->first; p != NULL; p = p->next ) last = p;
		parent->last = last;
	}
	nd->parent = NULL;
	nd->next   = NULL;
}

LOCAL BOOL is_blank_text( CONST T_TADNODE *nd )
{
	SZ	i;

	if ( nd == NULL || nd->kind != TAD_ND_TEXT || nd->text == NULL ) {
		return FALSE;
	}
	for ( i = 0; nd->text[i] != '\0'; i++ ) {
		UB c = nd->text[i];

		if ( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) return FALSE;
	}

	return TRUE;
}

EXPORT ER tad_set_attr( T_TAD *doc, T_TADNODE *nd, CONST char *name,
			CONST UB *value )
{
	T_TADATTR	*na;
	UB		*v;
	SZ		len = 0;
	INT		i;

	if ( doc == NULL || nd == NULL || name == NULL || value == NULL
	  || nd->kind != TAD_ND_ELEM ) {
		return E_PAR;
	}
	while ( value[len] != 0 ) {
		len++;
	}
	v = tad_dup(doc, value, len);
	if ( v == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < nd->nattr; i++ ) {
		if ( tad_same(nd->attr[i].name, name) ) {
			nd->attr[i].value = v;
			nd->raw = NULL;
			nd->rawlen = 0;
			return E_OK;
		}
	}
	na = (T_TADATTR *)tad_alloc(doc, sizeof(T_TADATTR) * (SZ)( nd->nattr + 1 ));
	if ( na == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < nd->nattr; i++ ) {
		na[i] = nd->attr[i];
	}
	len = 0;
	while ( name[len] != 0 ) {
		len++;
	}
	na[i].name = tad_dup(doc, (CONST UB *)name, len);
	na[i].value = v;
	if ( na[i].name == NULL ) {
		return E_NOMEM;
	}
	nd->attr = na;
	nd->nattr++;
	nd->raw = NULL;
	nd->rawlen = 0;

	return E_OK;
}

/*
 * Links that say their place in the drawing (zIndex) keep the order they
 * were just put in: the numbers they had between them are given out
 * again, smallest first, in the new order. Without this the numbers
 * would travel with the links and sort them straight back.
 */
LOCAL ER renumber_z( T_TAD *doc, T_TADNODE **slot, INT n )
{
	INT	*z, nz = 0, i, a, k, j;

	z = (INT *)tad_alloc(doc, sizeof(INT) * (SZ)n);
	if ( z == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		CONST UB *v = tad_attr(slot[i], "zIndex");

		if ( v == NULL ) v = tad_attr(slot[i], "zindex");
		if ( v != NULL ) {
			z[nz++] = to_num(v, 0);
		}
	}
	for ( i = 1; i < nz; i++ ) {
		INT	x = z[i];

		for ( j = i; j > 0 && z[j - 1] > x; j-- ) z[j] = z[j - 1];
		z[j] = x;
	}
	for ( i = 0, k = 0; i < n && k < nz; i++ ) {
		T_TADNODE	*nd = slot[i];

		for ( a = 0; a < nd->nattr; a++ ) {
			if ( tad_same(nd->attr[a].name, "zIndex") || tad_same(nd->attr[a].name, "zindex") ) {
				UB	num[16];
				SZ	len = put_num(num, z[k++]);

				nd->attr[a].value = tad_dup(doc, num, len);
				if ( nd->attr[a].value == NULL ) {
					return E_NOMEM;
				}
				nd->raw = NULL;		/* written from the attributes now */
				nd->rawlen = 0;
				break;
			}
		}
	}
	return E_OK;
}

EXPORT ER tad_lnk_reorder( T_TAD *doc, CONST TS_UUID *order, INT n )
{
	T_TADNODE	**slot, *moved;
	INT		count, i, k;

	if ( doc == NULL || order == NULL || n < 0 ) {
		return E_PAR;
	}
	count = tad_lnk_count(doc);
	if ( n != count ) {
		return E_PAR;			/* every link, once */
	}
	if ( n == 0 ) {
		return E_OK;
	}
	/*
	 * The places are the elements where they stand; what moves is
	 * what each element holds. The places' own links to their
	 * neighbours and parent stay, so nothing between the links moves.
	 */
	slot = (T_TADNODE **)tad_alloc(doc, sizeof(T_TADNODE *) * (SZ)n);
	moved = (T_TADNODE *)tad_alloc(doc, sizeof(T_TADNODE) * (SZ)n);
	if ( slot == NULL || moved == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < n; i++ ) {
		slot[i] = nth_link(doc, i);
		if ( slot[i] == NULL ) {
			return E_OBJ;
		}
	}
	for ( i = 0; i < n; i++ ) {
		T_TADNODE	*nd = find_link(doc, &order[i]);

		if ( nd == NULL ) {
			return E_NOEXS;
		}
		for ( k = 0; k < i; k++ ) {
			if ( ts_uuid_cmp(&order[k], &order[i]) == 0 ) {
				return E_PAR;	/* named twice */
			}
		}
		moved[i] = *nd;
	}
	for ( i = 0; i < n; i++ ) {
		T_TADNODE	*to = slot[i], *c;

		to->kind   = moved[i].kind;
		to->name   = moved[i].name;
		to->text   = moved[i].text;
		to->attr   = moved[i].attr;
		to->nattr  = moved[i].nattr;
		to->empty  = moved[i].empty;
		to->raw    = moved[i].raw;
		to->rawlen = moved[i].rawlen;
		to->first  = moved[i].first;
		to->last   = moved[i].last;
		for ( c = to->first; c != NULL; c = c->next ) {
			c->parent = to;
		}
	}
	return renumber_z(doc, slot, n);
}

EXPORT ER tad_lnk_del( T_TAD *doc, CONST TS_UUID *vobjid )
{
	T_TADNODE	*nd, *after;
	TS_UUID		target = tad_uuid_zero;
	INT		recno = 0;
	ER		er;

	if ( doc == NULL || vobjid == NULL ) {
		return E_PAR;
	}
	nd = find_link(doc, vobjid);
	if ( nd == NULL ) {
		return E_NOEXS;
	}
	er = id_of(nd, &target, &recno);
	if ( er < E_OK ) {
		return er;			/* it says nothing about what it points at */
	}
	/*
	 * The whitespace that followed the element goes with it, so that
	 * taking out a link that was put in leaves the text as it was.
	 */
	after = nd->next;
	unhook(nd);
	if ( is_blank_text(after) ) {
		unhook(after);
	}

	return tad_put_delta(doc, &target, -1);
}

/* ---------------------------------------------------------------- deltas */

EXPORT INT tad_lnk_ndelta( CONST T_TAD *doc )
{
	return ( doc != NULL ) ? doc->ndelta : 0;
}

EXPORT ER tad_lnk_delta( CONST T_TAD *doc, INT i, TS_UUID *p_target, INT *p_delta )
{
	T_TADDELTA	*d;

	if ( doc == NULL || i < 0 || p_target == NULL || p_delta == NULL ) {
		return E_PAR;
	}
	for ( d = doc->delta; d != NULL; d = d->next ) {
		if ( i-- == 0 ) {
			*p_target = d->target;
			*p_delta  = d->delta;
			return E_OK;
		}
	}

	return E_NOEXS;
}

EXPORT void tad_lnk_clr_delta( T_TAD *doc )
{
	if ( doc == NULL ) {
		return;
	}
	doc->delta      = NULL;
	doc->delta_last = NULL;
	doc->ndelta     = 0;
}
