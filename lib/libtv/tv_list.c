/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_list.c
 *	Lists of links made into a figure, and what they are made from: an
 *	object's metadata and name, the objects of the store (design 11.2)
 *
 *	A list is a record like any figure's, so that it is drawn, pressed,
 *	opened from and carried from by the same code as an object's
 *	record. Each link is closed, as wide as its name and the frame and
 *	the pictogram round it (tad_closed_w), with the system's colours.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/ui.h>
#include <ts/ob.h>
#include <ts/obatr.h>
#include <ts/tvlist.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

CONST T_TLLAY	tl_rows = { .how = TL_ROWS, .x = 10, .y = 14, .gap = 4, .chsz = 14 };

INT tl_put_x( char *x, INT at, INT max, CONST UB *s )
{
	for ( ; *s != 0 && at + 7 < max; s++ ) {
		CONST char	*e = ( *s == '&' ) ? "&amp;" : ( *s == '<' ) ? "&lt;" : ( *s == '>' ) ? "&gt;"
				   : ( *s == '"' ) ? "&quot;" : NULL;

		if ( e != NULL ) at += snprintf(x + at, (size_t)( max - at ), "%s", e);
		else x[at++] = (char)*s;
	}
	x[at] = 0;
	return at;
}

/* ---------------------------------------------------------------- the store */

INT tl_objects( TS_UUID **p_ids )
{
	INT	n = ui_lst_obj(OB_T_STORAGE, OB_S_FILE, p_ids);

	if ( n < 0 ) {
		*p_ids = NULL;
		return 0;
	}
	return n;
}

BOOL tl_meta( CONST TS_UUID *u, UB **p_buf, T_JSON *root )
{
	UB	*m = NULL;
	INT	n;
	ID	k;

	*p_buf = NULL;
	/* one the person may not read is passed by, asked first so that no refusal is noted */
	if ( ob_acc_obj(u, OB_OP_ATRRD) < E_OK ) return FALSE;
	k = ob_opn_obj(u, OB_OP_ATRRD);
	if ( k <= 0 ) return FALSE;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n <= 0 || js_parse(m, n, root) < E_OK ) {
		free(m);
		return FALSE;
	}
	*p_buf = m;
	return TRUE;
}

BOOL tl_name( CONST TS_UUID *u, UB *nm, INT max )
{
	T_JSON	root;
	UB	*m;
	BOOL	ok;

	nm[0] = 0;
	if ( !tl_meta(u, &m, &root) ) return FALSE;
	ok = (BOOL)( js_get_str(&root, "name", nm, max) >= 0 );
	free(m);
	return ok;
}

/* ---------------------------------------------------------------- the list */

T_TAD *tl_record( CONST char *title, CONST TS_UUID *ids, CONST UB *CONST *names,
		  CONST BOOL *dim, INT n, CONST T_TLLAY *lay, INT *p_rows )
{
	INT	max = 512 + ( ( n > 0 ) ? n : 0 ) * ( 480 + TAD_NAME_MAX * 5 ), at, i, w;
	INT	x = lay->x, y = lay->y, band = tad_closed_h(lay->chsz, TAD_D_DEFAULT), rows = 0;
	char	*xml = malloc((size_t)max), us[TS_UUID_STRLEN + 1];
	T_TAD	*rec = NULL;

	if ( p_rows != NULL ) *p_rows = 0;
	if ( xml == NULL ) return NULL;
	at = snprintf(xml, (size_t)max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	at = tl_put_x(xml, at, max, (CONST UB *)title);
	at += snprintf(xml + at, (size_t)( max - at ), "\"><figure>");
	for ( i = 0; i < n; i++ ) {
		UB	nm[TAD_NAME_MAX];
		BOOL	grey = (BOOL)( dim != NULL && dim[i] );

		if ( ob_acc_obj(&ids[i], 0) == E_NOEXS ) continue;	/* not in the store */
		if ( names != NULL && names[i] != NULL ) snprintf((char *)nm, sizeof(nm), "%s", (CONST char *)names[i]);
		else (void)tl_name(&ids[i], nm, TAD_NAME_MAX);
		w = dp_text_width(nm, lay->chsz);
		if ( w < 0 ) w = lay->chsz * 6;
		w = tad_closed_w(lay->chsz, TAD_D_DEFAULT, w);	/* the pictogram and the edges */
		if ( lay->how == TL_ROWS ) {
			if ( rows > 0 ) y += band + lay->gap;
			rows++;
		} else if ( rows == 0 ) {
			rows = 1;
		} else if ( x > lay->x && x + w > lay->width - lay->x ) {
			x = lay->x;
			y += band + lay->gap;
			rows++;
		}
		ts_uuid_text(&ids[i], us);
		at += snprintf(xml + at, (size_t)( max - at ), "<link id=\"%s_0.xtad\" name=\"", us);
		at = tl_put_x(xml, at, max, nm);
		at += snprintf(xml + at, (size_t)( max - at ),
			       "\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\" vobjbottom=\"%d\" height=\"%d\""
			       " chsz=\"%d\" frcol=\"%s\" chcol=\"%s\" tbcol=\"#ffffff\" bgcol=\"#ffffff\""
			       " pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\" autoopen=\"false\"/>",
			       x, y, x + w, y + band, band, lay->chsz,
			       grey ? "#a0a0a0" : "#000000", grey ? "#808080" : "#000000");
		if ( lay->how == TL_FLOW ) x += w + lay->gap;
	}
	at += snprintf(xml + at, (size_t)( max - at ), "</figure></tad>");
	if ( tad_parse((CONST UB *)xml, (SZ)at, NULL, &rec) < E_OK ) rec = NULL;
	free(xml);
	if ( p_rows != NULL ) *p_rows = rows;
	return rec;
}
