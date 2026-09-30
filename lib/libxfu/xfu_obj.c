/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_obj.c
 *	The objects lib/libxfu reads and writes, through ob_ alone
 *
 *	A link is a reference that is counted. The native store counts the
 *	links of a record itself when the record is written (OB_F_AUTOREF),
 *	but only links to objects that are there already; the store on FAT
 *	keeps the count in the metadata and is told of each link with
 *	ob_lnk_obj. So everything a record links to is made first, the
 *	record written after, and on a store that does not count, each link
 *	written is counted by hand.
 */

#include "xfu_in.h"
#include <ts/sysdef.h>

/* ---------------------------------------------------------------- whole parts */

EXPORT UB *xfu_rec_all( ID key, INT recno, SZ *p_size )
{
	T_OBREC	rec[16];
	UB	*buf;
	SZ	size = 0, got = 0, n;
	INT	cnt = 0, i;

	if ( ob_lst_rec(key, rec, 16, &cnt) < E_OK ) {
		return NULL;
	}
	for ( i = 0; i < cnt && i < 16 && rec[i].recno != recno; i++ ) ;
	if ( i >= cnt || i >= 16 ) {
		return NULL;
	}
	size = (SZ)rec[i].size;
	buf = (UB *)xfu_sys_alloc(size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	while ( got < size ) {
		n = 0;
		if ( ob_rea_rec(key, recno, (D)got, buf + got, size - got, &n) < E_OK || n <= 0 ) {
			break;
		}
		got += n;
	}
	buf[got] = 0;
	if ( p_size != NULL ) *p_size = got;
	return buf;
}

EXPORT UB *xfu_meta_all( ID key, SZ *p_size )
{
	UB	*buf;
	SZ	n = 0;

	buf = (UB *)xfu_sys_alloc(OB_ATR_MAX + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( ob_get_atr(key, buf, OB_ATR_MAX, &n) < E_OK || n <= 0 || n > OB_ATR_MAX ) {
		xfu_sys_free(buf);
		return NULL;
	}
	buf[n] = 0;
	if ( p_size != NULL ) *p_size = n;
	return buf;
}

/*
 * "tessronos.file" replaced: only that member, so what the store put in
 * when the object was made (its dates) stays as it is.
 */
EXPORT ER xfu_meta_file( ID key, CONST UB *file, SZ n )
{
	T_JSON	root, tf, f;
	UB	*old;
	SZ	olen = 0;
	XBUF	w;
	INT	at;
	ER	er;

	old = xfu_meta_all(key, &olen);
	if ( old == NULL ) {
		return E_NOMEM;
	}
	if ( js_parse(old, (INT)olen, &root) < E_OK || js_get(&root, "tessronos", &tf) < E_OK
	  || js_get(&tf, "file", &f) < E_OK ) {
		xfu_sys_free(old);
		return E_OBJ;
	}
	xb_init(&w);
	at = (INT)( f.s - old );
	xb_bytes(&w, old, at);
	xb_bytes(&w, file, n);
	xb_bytes(&w, old + at + f.len, olen - at - f.len);
	er = w.bad ? E_NOMEM : ( w.at > OB_ATR_MAX ) ? E_LIMIT : ob_set_atr(key, w.b, w.at);
	xb_free(&w);
	xfu_sys_free(old);
	return er;
}

EXPORT BOOL xfu_counts( CONST TS_UUID *u )
{
	T_OBREF	r;

	return (BOOL)( ob_ref_obj(u, &r) >= E_OK && ( r.flags & OB_F_AUTOREF ) != 0 );
}

/* ---------------------------------------------------------------- links */

LOCAL BOOL blank( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/* The value of attribute name in the tag s[a..b), or -1 */
LOCAL INT attr_at( CONST UB *s, INT a, INT b, CONST char *name )
{
	INT	m = xfu_slen((CONST UB *)name), i, k;

	for ( i = a; i + m + 3 < b; i++ ) {
		if ( !blank(s[i]) ) continue;
		for ( k = 0; k < m && s[i + 1 + k] == (UB)name[k]; k++ ) ;
		if ( k == m && s[i + 1 + m] == '=' && s[i + 2 + m] == '"' ) {
			return i + 3 + m;
		}
	}
	return -1;
}

/* "<link" and a blank, its target the UUID the id attribute starts with */
EXPORT INT xfu_links( CONST UB *s, SZ n, XFU_LINKCB cb, void *arg )
{
	INT	i = 0, j, v, cnt = 0;
	TS_UUID	u;

	while ( (SZ)i + 5 < n ) {
		i = xfu_find(s, n, "<link", i);
		if ( i < 0 || (SZ)i + 5 >= n ) break;
		if ( !blank(s[i + 5]) ) {
			i += 5;
			continue;
		}
		j = xfu_find(s, n, ">", i + 5);
		if ( j < 0 ) break;
		v = attr_at(s, i + 5, j, "id");
		if ( v >= 0 && v + 36 <= j && xfu_str_uuid(s + v, &u) ) {
			if ( cb != NULL ) cb(arg, &u);
			cnt++;
		}
		i = j + 1;
	}
	return cnt;
}

EXPORT void xfu_link_xml( XBUF *w, CONST TS_UUID *target, INT l, INT t, INT r, INT b, INT z )
{
	TS_UUID	vid;
	char	us[40];
	INT	i;

	if ( xfu_sys_uuid(&vid) < E_OK ) {
		/* an identity of its own all the same: the target's, told apart by place */
		vid = *target;
		vid.b[6] = (UB)( 0x40 | ( vid.b[6] & 0x0F ) );
		for ( i = 10; i < 16; i++ ) vid.b[i] ^= (UB)( ( l * 31 + t * 17 + z ) >> ( ( i & 3 ) * 4 ) );
	}
	xb_put(w, "<link id=\"");
	xfu_uuid_str(target, us);
	xb_put(w, us);
	xb_put(w, "_0.xtad\" vobjid=\"");
	xfu_uuid_str(&vid, us);
	xb_put(w, us);
	xb_put(w, "\" vobjleft=\"");
	xb_num(w, l);
	xb_put(w, "\" vobjtop=\"");
	xb_num(w, t);
	xb_put(w, "\" vobjright=\"");
	xb_num(w, r);
	xb_put(w, "\" vobjbottom=\"");
	xb_num(w, b);
	xb_put(w, "\" height=\"");
	xb_num(w, b - t);
	xb_put(w, "\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\""
		  " bgcol=\"#ffffff\" dlen=\"0\" pictdisp=\"true\" namedisp=\"true\""
		  " roledisp=\"false\" typedisp=\"false\" updatedisp=\"false\""
		  " framedisp=\"true\" autoopen=\"false\" zIndex=\"");
	xb_num(w, z);
	xb_put(w, "\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>\n");
}

/* The number of an attribute of a tag, or dflt */
LOCAL INT attr_num( CONST UB *s, INT a, INT b, CONST char *name, INT dflt )
{
	INT	v = attr_at(s, a, b, name), n = 0;
	BOOL	neg = FALSE;

	if ( v < 0 ) {
		return dflt;
	}
	if ( s[v] == '-' ) {
		neg = TRUE;
		v++;
	}
	while ( v < b && s[v] >= '0' && s[v] <= '9' ) n = n * 10 + ( s[v++] - '0' );
	return neg ? -n : n;
}

/*
 * A link put at the foot of a box's figure, below the lowest link
 * there, and counted when the box's store does not count; a box with
 * a link to the object already is left as it is.
 */
EXPORT ER xfu_link_add( CONST TS_UUID *box, CONST TS_UUID *u )
{
	UB	*rec;
	SZ	size = 0, asz = 0;
	XBUF	w;
	INT	i, j, v, low = 0, z = 0, end;
	TS_UUID	t;
	ID	key;
	ER	er;

	key = ob_opn_obj(box, OB_OP_READ | OB_OP_ATRRD | OB_OP_WRITE);
	if ( key < E_OK ) {
		return (ER)key;
	}
	rec = xfu_rec_all(key, 0, &size);
	if ( rec == NULL ) {
		ob_cls_obj(key);
		return E_NOEXS;
	}
	for ( i = 0; ( i = xfu_find(rec, size, "<link", i) ) >= 0; i = j + 1 ) {
		j = xfu_find(rec, size, ">", i);
		if ( j < 0 ) break;
		v = attr_at(rec, i + 5, j, "id");
		if ( v >= 0 && xfu_str_uuid(rec + v, &t) && xfu_uuid_eq(&t, u) ) {
			xfu_sys_free(rec);
			ob_cls_obj(key);
			return E_OK;
		}
		v = attr_num(rec, i + 5, j, "vobjbottom", 0);
		if ( v > low ) low = v;
		v = attr_num(rec, i + 5, j, "zIndex", 0);
		if ( v > z ) z = v;
	}
	/* before the figure's end, or the document's */
	end = -1;
	for ( i = 0; ( i = xfu_find(rec, size, "</figure>", i) ) >= 0; i++ ) end = i;
	if ( end < 0 ) {
		for ( i = 0; ( i = xfu_find(rec, size, "</document>", i) ) >= 0; i++ ) end = i;
	}
	if ( end < 0 ) {
		xfu_sys_free(rec);
		ob_cls_obj(key);
		return E_OBJ;
	}
	xb_init(&w);
	xb_bytes(&w, rec, end);
	xfu_link_xml(&w, u, 8, low + 8, 248, low + 33, z + 1);
	xb_bytes(&w, rec + end, size - end);
	xfu_sys_free(rec);
	er = w.bad ? E_NOMEM : ob_wri_rec(key, 0, 0, w.b, w.at, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(key, 0, (UD)w.at);
	xb_free(&w);
	ob_cls_obj(key);
	if ( er >= E_OK && !xfu_counts(box) ) {
		er = ob_lnk_obj(u);
	}
	return er;
}

/* ---------------------------------------------------------------- a call */

EXPORT ER xfu_begin( XFU *x, CONST T_XFUTREE *t, CONST T_XFUOPT *opt )
{
	xfu_mset(x, 0, sizeof(*x));
	x->t = t;
	x->opt = opt;
	x->depth = ( opt != NULL && opt->depth > 0 ) ? opt->depth : XFU_DEPTH;
	x->chunk = (UB *)xfu_sys_alloc(XFU_CHUNK);
	return ( x->chunk != NULL ) ? E_OK : E_NOMEM;
}

EXPORT void xfu_finish( XFU *x )
{
	INT	i;

	for ( i = 0; i < XFU_NAPP; i++ ) {
		if ( x->app[i].icon != NULL ) xfu_sys_free(x->app[i].icon);
	}
	if ( x->chunk != NULL ) xfu_sys_free(x->chunk);
	if ( x->seen != NULL ) xfu_sys_free(x->seen);
	if ( x->stack != NULL ) xfu_sys_free(x->stack);
	if ( x->made != NULL ) xfu_sys_free(x->made);
	if ( x->opt != NULL && x->opt->stat != NULL ) {
		*x->opt->stat = x->st;
	}
}

EXPORT void xfu_note( XFU *x, CONST UB *name, ER er )
{
	if ( er < E_OK && er != E_ABORT ) {
		x->st.skipped++;
		x->st.last = er;
	}
	if ( x->opt != NULL && x->opt->note != NULL && x->opt->note(x->opt->arg, name, er) < E_OK ) {
		x->stop = TRUE;
	}
}

EXPORT void xfu_tick( XFU *x, D bytes )
{
	if ( x->opt != NULL && x->opt->tick != NULL && x->opt->tick(x->opt->arg, bytes) < E_OK ) {
		x->stop = TRUE;
	}
}

EXPORT void xfu_made( XFU *x, CONST TS_UUID *u )
{
	if ( x->made == NULL ) {
		x->made = (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * XFU_OBJ_MAX);
	}
	if ( x->made != NULL && x->nmade < XFU_OBJ_MAX ) {
		x->made[x->nmade++] = *u;
	}
}

EXPORT void xfu_unmade( XFU *x, CONST TS_UUID *u )
{
	INT	i;

	for ( i = x->nmade - 1; i >= 0; i-- ) {
		if ( xfu_uuid_eq(&x->made[i], u) ) {
			for ( ; i < x->nmade - 1; i++ ) x->made[i] = x->made[i + 1];
			x->nmade--;
			return;
		}
	}
}

/* The objects made that are still there, tried once each, the last made first; how many went */
LOCAL INT undo_pass( XFU *x )
{
	INT	i, k, n = 0;

	for ( i = x->nmade - 1; i >= 0; i-- ) {
		if ( ob_del_obj(&x->made[i]) >= E_OK ) {
			for ( k = i; k < x->nmade - 1; k++ ) x->made[k] = x->made[k + 1];
			x->nmade--;
			n++;
		}
	}
	return n;
}

/*
 * What the call made, thrown away. A store refuses an object something
 * still links to, so the objects are tried again as long as some go: a
 * figure goes before what it links to, and the native store then takes
 * down the count of those itself. The store on FAT does not; for what
 * it still refuses, the count is taken down by hand -- only for objects
 * this call made, which nothing else knows -- and the rest tried again.
 */
EXPORT void xfu_undo( XFU *x )
{
	INT	i, k;

	while ( x->nmade > 0 && undo_pass(x) > 0 ) ;
	for ( i = 0; i < x->nmade; i++ ) {
		if ( xfu_counts(&x->made[i]) ) continue;
		for ( k = 0; k < 64 && ob_unl_obj(&x->made[i]) >= E_OK; k++ ) ;
	}
	while ( x->nmade > 0 && undo_pass(x) > 0 ) ;
	x->nmade = 0;
}

/* ---------------------------------------------------------------- programs */

LOCAL CONST char * CONST app_id[XFU_NAPP] = {
	"basic-text-editor", "basic-figure-editor", "virtual-object-list", "unpack-file"
};

EXPORT CONST char *xfu_app_id( INT app )
{
	return ( app >= 0 && app < XFU_NAPP ) ? app_id[app] : "";
}

/* The icon of an object, whole; NULL when it has none */
LOCAL UB *icon_of( CONST TS_UUID *u, SZ *p_size )
{
	UB	*buf = NULL;
	SZ	len = 0;
	ID	key;

	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return NULL;
	}
	if ( ob_get_ico(key, NULL, 0, &len) >= E_OK && len > 0 && len <= OB_ICO_MAX ) {
		buf = (UB *)xfu_sys_alloc(len);
		if ( buf != NULL && ob_get_ico(key, buf, len, &len) < E_OK ) {
			xfu_sys_free(buf);
			buf = NULL;
		}
	}
	ob_cls_obj(key);
	*p_size = len;
	return buf;
}

/* A program of the program box: its name and its first template's icon */
LOCAL void read_prog( XFU *x, CONST TS_UUID *u )
{
	T_JSON	root, tf, g, base, it;
	UB	*meta, id[64], us[40];
	SZ	size = 0;
	INT	a;
	ID	key;

	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return;
	}
	meta = xfu_meta_all(key, &size);
	ob_cls_obj(key);
	if ( meta == NULL ) {
		return;
	}
	if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "program", &g) >= E_OK && js_get_str(&g, "id", id, sizeof(id)) > 0 ) {
		for ( a = 0; a < XFU_NAPP; a++ ) {
			XFUAPP	*p = &x->app[a];
			TS_UUID	t;

			if ( !xfu_same(id, app_id[a]) || p->have ) continue;
			p->have = TRUE;
			if ( js_get_str(&g, "name", p->name, sizeof(p->name)) <= 0 ) {
				(void)js_get_str(&root, "name", p->name, sizeof(p->name));
			}
			if ( js_get(&g, "base", &base) >= E_OK ) {
				it.s = NULL;
				if ( js_next(&base, &it) && js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
				  && xfu_str_uuid(us, &t) ) {
					p->icon = icon_of(&t, &p->iconsz);
				}
			}
		}
	}
	xfu_sys_free(meta);
}

EXPORT XFUAPP *xfu_app( XFU *x, INT app )
{
	TS_UUID	box, *ids;
	INT	i, n = 0, max = 64;

	if ( app < 0 || app >= XFU_NAPP ) {
		return NULL;
	}
	if ( !x->apps ) {
		x->apps = TRUE;
		ids = xfu_str_uuid((CONST UB *)SYSDEF_PROG_BOX, &box)
		    ? (TS_UUID *)xfu_sys_alloc(sizeof(TS_UUID) * max) : NULL;
		if ( ids != NULL ) {
			if ( ob_lst_lnk(&box, NULL, ids, max, &n) < E_OK ) n = 0;
			for ( i = 0; i < n && i < max; i++ ) read_prog(x, &ids[i]);
			xfu_sys_free(ids);
		}
	}
	return x->app[app].have ? &x->app[app] : NULL;
}

/* ---------------------------------------------------------------- boxes */

EXPORT ER xfu_box_link( CONST TS_UUID *box, CONST TS_UUID *uuid )
{
	if ( box == NULL || uuid == NULL ) {
		return E_PAR;
	}
	return xfu_link_add(box, uuid);
}

EXPORT ER xfu_make_box( CONST UB *name, CONST TS_UUID *near, TS_UUID *p_uuid )
{
	XFU	*x;
	XFUAPP	*a;
	XBUF	w, r;
	T_OBCRE	c;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( name == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	x = (XFU *)xfu_sys_alloc(sizeof(XFU));
	if ( x == NULL ) {
		return E_NOMEM;
	}
	xfu_mset(x, 0, sizeof(*x));
	xb_init(&w);
	xb_init(&r);
	xb_put(&w, "{\"name\":\"");
	xb_json(&w, name, xfu_slen(name));
	xb_put(&w, "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,\"recordCount\":1,"
		   "\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{");
	a = xfu_app(x, XFU_APP_LIST);
	xb_put(&w, "\"virtual-object-list\":{");
	if ( a != NULL && a->name[0] != 0 ) {
		xb_put(&w, "\"name\":\"");
		xb_json(&w, a->name, xfu_slen(a->name));
		xb_put(&w, "\",");
	}
	xb_put(&w, "\"defaultOpen\":true},\"basic-figure-editor\":{\"defaultOpen\":false}},"
		   "\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1}]}}");
	xb_put(&r, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	xb_xml(&r, name, xfu_slen(name));
	xb_put(&r, "\">\n<figure>\n</figure>\n</tad>\n");
	er = ( w.bad || r.bad ) ? E_NOMEM : E_OK;
	if ( er >= E_OK ) {
		xfu_mset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = w.b;
		c.jsonsz = w.at;
		if ( near != NULL ) c.near = *near;
		if ( a != NULL && a->icon != NULL ) {
			c.icon = a->icon;
			c.iconsz = a->iconsz;
		}
		er = ob_cre_obj(&c, p_uuid);
	}
	if ( er >= E_OK ) {
		key = ob_opn_obj(p_uuid, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
		er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_TAD, 0, &recno) : (ER)key;
		if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, r.b, r.at, &asz);
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < E_OK ) (void)ob_del_obj(p_uuid);
	}
	xb_free(&w);
	xb_free(&r);
	xfu_finish(x);
	xfu_sys_free(x);
	return er;
}
