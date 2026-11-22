/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtprog.c
 *	The programs of the system as real objects: the program box, the
 *	原紙箱 and the 小物箱 (design 18.12)
 *
 *	A program is a real object whose metadata says what it is:
 *	  "tessronos": { "program": { "id": "basic-text-editor", "kind": "app",
 *		"name": "基本文章編集", "base": [ UUID ], "builtin": false } }
 *	The id is the one an applist names, so what a TADjs Desktop wrote
 *	opens with the program it names. "aliases" lists ids the program
 *	had before (書庫管理 took in 書庫解凍, unpack-file): an applist that
 *	still names one of them opens with it. "kind" is app (it has templates,
 *	原紙, and opens objects), accessory (小物: no data of its own),
 *	utility (a tool of the system) or command (no window of its own: run
 *	in a コンソール, which shows what it prints). A program the desktop carries in
 *	itself (the list 'carried') is "builtin" and has no executable; one
 *	that is a process has its executable in a record of type 9
 *	(tessronos.exec).
 *
 *	The program box is the list of programs: an object of the store
 *	whose record links to each program object. The system's programs
 *	come as objects in the store (etc/def), the clock with its
 *	executable as record 1; nothing here makes them. What the 実行 menu
 *	names is looked up in the box, the 原紙箱 shows the templates of its
 *	applications, and the 小物箱 its accessories.
 *
 *	The parts box (部品箱) is made the same way: definitions of the
 *	standard parts -- a button, a switch, a box of letters, a panel, a
 *	menu -- each an object whose metadata says what it is,
 *	  "tessronos": { "part": { "sub": "part", "kind": 6, "rect": [0,0,80,24] } }
 *	and a program has a part of its own by copying one (ob_cpy_obj)
 *	and placing the copy (ob_map_rec, design 18.13).
 *
 *	A TADjs plugin -- a directory with its plugin.json and the files of
 *	its template -- is installed as a program object and a template
 *	object (dt_plugin_install, design 18.12) from the object ファイル変換
 *	made of the directory, as a package is: the person drops it on the
 *	〈バージョン〉 sheet of システム環境設定. Its code is HTML for the
 *	browser and does not run here, so the program has no executable:
 *	what it brings is its name, its id for the applist, and its template.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/json.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include <ts/conf.h>
#include <ts/fquit.h>
#include "desktop.h"

/*
 * The programs of the program box, as many as it links to. Each entry
 * is allocated once and kept: what dt_prog_find and the others hand out
 * points at it, and stays good when the box is read again (an entry is
 * then filled anew, never moved or freed). The table of pointers grows
 * by a copy; the one before is kept as well.
 */
LOCAL DTPROG	**prog = NULL;
LOCAL INT	nprog = 0, prog_cap = 0;

/* Room for one more entry at nprog: FALSE for no memory */
LOCAL BOOL prog_room( void )
{
	DTPROG	**t;
	INT	cap, i;

	if ( nprog < prog_cap ) {
		return TRUE;
	}
	cap = ( prog_cap > 0 ) ? prog_cap * 2 : 64;
	t = (DTPROG **)Kcalloc((SZ)cap, sizeof(DTPROG *));
	if ( t == NULL ) {
		return FALSE;
	}
	for ( i = 0; i < prog_cap; i++ ) t[i] = prog[i];
	for ( ; i < cap; i++ ) {
		t[i] = (DTPROG *)Kcalloc(1, sizeof(DTPROG));
		if ( t[i] == NULL ) break;
	}
	if ( i == prog_cap ) {
		Kfree(t);
		return FALSE;
	}
	prog = t;
	prog_cap = i;
	return TRUE;
}

LOCAL BOOL has_type( CONST UB *types, CONST UB *t );

/* ---------------------------------------------------------------- text */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != 0 && at + 1 < max ) {
		b[at++] = (UB)*s++;
	}
	b[at] = 0;
	return at;
}

/* Text inside a JSON string */
LOCAL INT put_json( UB *b, INT at, INT max, CONST UB *s )
{
	for ( ; *s != 0 && at + 2 < max; s++ ) {
		if ( *s == '"' || *s == '\\' ) b[at++] = '\\';
		if ( *s >= 0x20 ) b[at++] = *s;
	}
	b[at] = 0;
	return at;
}

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* A member's string, whole and unescaped, in memory of its own (Kfree it); NULL: none, or no memory */
LOCAL UB *js_val_dup( CONST T_JSON *v, CONST char *key )
{
	UB	*s;

	if ( js_type(v) != JS_STRING ) {
		return NULL;
	}
	s = (UB *)Kmalloc((size_t)v->len + 1);	/* unescaped it is never longer than written */
	if ( s == NULL ) {
		tm_printf((UB *)"dtprog: no memory for \"%s\" of %d bytes\n", key, (INT)v->len);
		return NULL;
	}
	if ( js_str(v, s, (INT)v->len + 1) < 0 ) {
		Kfree(s);
		return NULL;
	}
	return s;
}

LOCAL UB *js_dup( CONST T_JSON *obj, CONST char *key )
{
	T_JSON	v;

	if ( js_get(obj, key, &v) < E_OK ) {
		return NULL;
	}
	return js_val_dup(&v, key);
}

/*
 * A string member that may be absent, as js_dup reads it: *p_s is NULL
 * when the member is not there or is not a string, and E_NOMEM answers
 * when it is there but could not be read for lack of memory.
 */
LOCAL ER js_dup_opt( CONST T_JSON *obj, CONST char *key, UB **p_s )
{
	T_JSON	v;

	*p_s = NULL;
	if ( js_get(obj, key, &v) < E_OK || js_type(&v) != JS_STRING ) {
		return E_OK;
	}
	*p_s = js_dup(obj, key);
	return ( *p_s != NULL ) ? E_OK : E_NOMEM;
}

/* ---------------------------------------------------------------- what a program says */

/*
 * The programs the desktop carries in itself, by their ids: only these
 * are "builtin". A program object of any other id that says it is is
 * started as a process all the same: the desktop has no code of its own
 * for it (基本図形編集, 基本文章編集, 仮身一覧, the 原紙箱, the 小物箱, the
 * トレー and the system's tools (屑実身操作, 実身/仮身検索, 仮身ネットワーク,
 * ウインドウ一覧) run only as processes).
 */
LOCAL CONST char	*CONST carried[] = {
	"force-quit",
};

LOCAL BOOL is_carried( CONST UB *id )
{
	INT	i;

	for ( i = 0; i < (INT)( sizeof(carried) / sizeof(carried[0]) ); i++ ) {
		if ( same(id, carried[i]) ) return TRUE;
	}
	return FALSE;
}

/* A word of a file type in lower case, added to a comma separated list that grows as it needs (Kfree it) */
LOCAL ER types_add( UB **p_t, INT *p_cap, UB *w )
{
	INT	q, len = (INT)knl_strlen((CONST char *)w), k;
	UB	*t;

	for ( q = 0; q < len; q++ ) if ( w[q] >= 'A' && w[q] <= 'Z' ) w[q] = (UB)( w[q] - 'A' + 'a' );
	if ( len <= 0 || ( *p_t != NULL && has_type(*p_t, w) ) ) {
		return E_OK;
	}
	k = ( *p_t != NULL ) ? (INT)knl_strlen((CONST char *)*p_t) : 0;
	if ( *p_t == NULL || k + len + 2 > *p_cap ) {
		INT	cap = ( *p_cap > 0 ) ? *p_cap : 32;

		while ( cap < k + len + 2 ) cap *= 2;
		t = (UB *)Krealloc(*p_t, (size_t)cap);
		if ( t == NULL ) {
			tm_printf((UB *)"dtprog: no memory for the file types of %d bytes\n", k + len + 2);
			return E_NOMEM;
		}
		if ( *p_t == NULL ) t[0] = 0;
		*p_t = t;
		*p_cap = cap;
	}
	t = *p_t;
	if ( k > 0 ) t[k++] = ',';
	for ( q = 0; q < len; q++ ) t[k++] = w[q];
	t[k] = 0;
	return E_OK;
}

/* A string the entry holds, replaced by a new one (taken over); the one held stays when they read alike */
LOCAL void swap_str( UB **p_held, UB *nw )
{
	if ( *p_held != NULL && knl_strcmp((CONST char *)*p_held, (CONST char *)nw) == 0 ) {
		Kfree(nw);
		return;
	}
	if ( *p_held != NULL ) Kfree(*p_held);
	*p_held = nw;
}

/* A program object's metadata read into an entry; FALSE when it is not a program */
LOCAL BOOL read_prog( CONST TS_UUID *uuid, DTPROG *p )
{
	T_JSON	root, tf, g, base, it;
	UB	*meta, *nm, *ver, *types = NULL, s[32];
	SZ	size = 0;
	INT	tcap = 0;
	BOOL	ok;

	meta = om_obj_meta(uuid, &size);
	if ( meta == NULL ) {
		return FALSE;
	}
	ok = (BOOL)( js_parse(meta, (INT)size, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "program", &g) >= E_OK
		  && js_get_str(&g, "id", p->id, DT_PROG_ID) > 0 );
	if ( ok ) {
		p->uuid = *uuid;
		/* the program's name, whole; else the object's */
		nm = js_dup(&g, "name");
		if ( nm == NULL || nm[0] == 0 ) {
			if ( nm != NULL ) Kfree(nm);
			nm = js_dup(&root, "name");
		}
		if ( nm == NULL ) {
			nm = dt_cat("", NULL);
		}
		if ( nm == NULL ) {
			Kfree(meta);
			return FALSE;		/* said on the console */
		}
		if ( p->name != NULL && knl_strcmp((CONST char *)p->name, (CONST char *)nm) == 0 ) {
			Kfree(nm);		/* read again unchanged: the one held stays */
		} else {
			if ( p->name != NULL ) Kfree(p->name);
			p->name = nm;
		}
		p->kind = DT_PK_APP;
		if ( js_get_str(&g, "kind", s, sizeof(s)) > 0 ) {
			p->kind = same(s, "accessory") ? DT_PK_ACCESSORY
				: same(s, "utility") ? DT_PK_UTILITY
				: same(s, "command") ? DT_PK_COMMAND : DT_PK_APP;
		}
		/* the version as long as it is written; empty when there is none */
		ver = js_dup(&g, "version");
		if ( ver == NULL ) ver = dt_cat("", NULL);
		if ( ver == NULL ) {
			Kfree(meta);
			return FALSE;		/* said on the console */
		}
		swap_str(&p->version, ver);
		p->builtin = (BOOL)( js_get_bool(&g, "builtin", FALSE) && is_carried(p->id) );
		p->menu = js_get_bool(&g, "menu", FALSE);
		p->open_link = (BOOL)( js_get_str(&g, "openArg", s, sizeof(s)) > 0 && same(s, "link") );
		p->once = js_get_bool(&g, "once", FALSE);

		/* the kinds of object it opens from 実行: "fileTypes": ["xtad", "tad"], every one of them */
		if ( js_get(&g, "fileTypes", &base) >= E_OK ) {
			for ( it.s = NULL; js_next(&base, &it); ) {
				UB	*w;
				ER	er;

				if ( js_type(&it) != JS_STRING ) continue;
				w = js_val_dup(&it, "fileTypes");
				if ( w == NULL ) {
					if ( types != NULL ) Kfree(types);
					Kfree(meta);
					return FALSE;	/* said on the console */
				}
				er = types_add(&types, &tcap, w);
				Kfree(w);
				if ( er < E_OK ) {
					if ( types != NULL ) Kfree(types);
					Kfree(meta);
					return FALSE;
				}
			}
		}
		if ( types == NULL ) types = dt_cat("", NULL);
		if ( types == NULL ) {
			Kfree(meta);
			return FALSE;
		}
		swap_str(&p->types, types);

		/* the ids it had before */
		p->nalias = 0;
		if ( js_get(&g, "aliases", &base) >= E_OK ) {
			for ( it.s = NULL; p->nalias < DT_PROG_ALIAS && js_next(&base, &it); ) {
				if ( js_str(&it, p->alias[p->nalias], DT_PROG_ID) > 0 ) p->nalias++;
			}
		}

		/* its templates */
		p->nbase = 0;
		if ( js_get(&g, "base", &base) >= E_OK ) {
			for ( it.s = NULL; p->nbase < DT_PROG_BASE && js_next(&base, &it); ) {
				UB	us[TS_UUID_STRLEN + 1];

				if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
				  && ts_str_to_uuid((CONST char *)us, &p->base[p->nbase]) >= E_OK ) {
					p->nbase++;
				}
			}
		}
	}
	Kfree(meta);
	return ok;
}

/* ---------------------------------------------------------------- the list */

/* The programs the box links to, read into the list */
LOCAL void read_box( void )
{
	TS_UUID	u, *lk;
	INT	n = 0, i;

	nprog = 0;
	if ( !knl_sys_find(SYSROLE_PROGS, SYSDEF_PROG_BOX, &u) ) {
		return;
	}
	/* every link of the box: asked how many, then taken */
	if ( ob_lst_lnk(&u, NULL, NULL, 0, &n) < E_OK || n <= 0 ) {
		return;
	}
	lk = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( lk != NULL ) {
		INT	all = 0;

		if ( ob_lst_lnk(&u, NULL, lk, n, &all) < E_OK ) all = 0;
		if ( all < n ) n = all;
		for ( i = 0; i < n; i++ ) {
			if ( !prog_room() ) {
				tm_printf((UB *)"desktop: no memory for the programs past %d\n", nprog);
				break;
			}
			if ( read_prog(&lk[i], prog[nprog]) ) {
				DTPROG	*p = prog[nprog++];
				INT	k;

				/*
				 * A template nothing links to any more -- its last
				 * link taken out of a 原紙箱 -- held, so that it is
				 * not a 屑実身: 原紙集め still shows it.
				 */
				for ( k = 0; k < p->nbase; k++ ) {
					if ( om_store_refs(&p->base[k]) == 0 ) {
						(void)ob_lnk_obj(&p->base[k]);
					}
				}
			}
		}
		Kfree(lk);
	}
}

/* ---------------------------------------------------------------- TADjs plugins */

/*
 * A TADjs plugin is installed from the object ファイル変換 made of its
 * directory: a figure of links to what the directory held, plugin.json
 * among them as an object whose record 1 is the file, and the files of
 * its template already taken in as that TADjs object under its UUID
 * (design 18.20). Nothing is taken in by itself: the person installs
 * the plugin as a package is installed, by dropping the object on the
 * list of the 〈バージョン〉 sheet of システム環境設定 (dt_pkg_install,
 * design 18.12).
 */
/*
 * The program's metadata: what plugin.json says, as a program, and in
 * "tessronos.file" what it was installed from.
 */
LOCAL INT plugin_meta( UB *m, INT max, CONST UB *from, CONST UB *id, CONST UB *name,
		       CONST char *kind, CONST UB *ver, CONST TS_UUID *base, CONST UB *types )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = 0;

	n = put(m, n, max, "{\"name\":\"");
	n = put_json(m, n, max, name);
	n = put(m, n, max, "\",\"relationship\":[],\"linktype\":false,"
			   "\"refCount\":1,\"recordCount\":0,\"editable\":false,"
			   "\"deletable\":false,\"readable\":true,\"maker\":\"TADjs plugin\","
			   "\"applist\":{},\"tessronos\":{\"program\":{\"id\":\"");
	n = put_json(m, n, max, id);
	n = put(m, n, max, "\",\"kind\":\"");
	n = put(m, n, max, kind);
	n = put(m, n, max, "\",\"name\":\"");
	n = put_json(m, n, max, name);
	n = put(m, n, max, "\",\"builtin\":false,\"base\":[");
	if ( base != NULL && ts_uuid_to_str(base, us, sizeof(us)) >= E_OK ) {
		n = put(m, n, max, "\"");
		n = put(m, n, max, us);
		n = put(m, n, max, "\"");
	}
	n = put(m, n, max, "]");
	if ( types != NULL && types[0] != 0 ) {
		/* what its right-button menu was offered for (contextMenu fileTypes), as 実行 offers it */
		INT	k;

		n = put(m, n, max, ",\"fileTypes\":[\"");
		for ( k = 0; types[k] != 0 && n < max - 4; k++ ) {
			if ( types[k] == ',' ) n = put(m, n, max, "\",\"");
			else m[n++] = types[k];
		}
		m[n] = 0;
		n = put(m, n, max, "\"]");
	}
	n = put(m, n, max, "},\"plugin\":{\"version\":\"");
	n = put_json(m, n, max, ver);
	n = put(m, n, max, "\"},\"file\":{\"name\":\"");
	n = put_json(m, n, max, from);
	n = put(m, n, max, "/plugin.json\",\"mediatype\":\"application/json\"}}}");
	return ( n < max - 1 ) ? n : -1;
}

/*
 * The plugin's template made the system's: a copy of the TADjs object
 * the directory brought, beside the program box and protected as the
 * system's objects are, so that what the person does with the object
 * they installed from does not touch it
 */
LOCAL ER plugin_base( CONST TS_UUID *brought, CONST T_OBPRT *prt, TS_UUID *p_base )
{
	T_OBCRE	c;
	TS_UUID	box;

	knl_memset(&c, 0, sizeof(c));
	c.prt = prt;
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) {
		c.near = box;
	}
	return ob_cpy_obj(brought, &c, p_base);
}

LOCAL ER plugin_install( CONST TS_UUID *dir, BOOL force, ID pid, CONST TS_UUID *win,
			 TS_UUID *p_prog, UINT *p_how, UB **p_name );

/* The plugin's name, read whole, let go whatever way the install went */
EXPORT ER dt_plugin_install( CONST TS_UUID *dir, BOOL force, ID pid, CONST TS_UUID *win,
			     TS_UUID *p_prog, UINT *p_how )
{
	UB	*name = NULL;
	ER	er = plugin_install(dir, force, pid, win, p_prog, p_how, &name);

	if ( name != NULL ) Kfree(name);
	return er;
}

LOCAL ER plugin_install( CONST TS_UUID *dir, BOOL force, ID pid, CONST TS_UUID *win,
			 TS_UUID *p_prog, UINT *p_how, UB **p_name )
{
	T_JSON		root, bf, cm, it, ft, t1;
	UB		id[DT_PROG_ID], *name, *type = NULL, *ver = NULL, nul[1] = { 0 }, jf[128];
	UB		*types = NULL;
	INT		tcap = 0;
	UB		*j, *m, *from;
	SZ		size = 0, fl = 0;
	INT		n, mmax;
	TS_UUID		base, brought, prog, box, pj, old;
	T_OBREF		r, dr;
	CONST DTPROG	*have;
	CONST char	*kind;
	BOOL		has_base = FALSE, had = FALSE;
	T_OBCRE		c;
	T_OBPRT		prt;
	ER		er = E_OK;

	if ( dir == NULL ) {
		return E_PAR;
	}
	/* plugin.json: a file the directory held */
	if ( ob_ref_obj(dir, &dr) < E_OK || ob_fnd_lnk(dir, (CONST UB *)"plugin.json", &pj) < E_OK ) {
		return E_OBJ;
	}
	j = om_obj_record(&pj, 1, &size);
	if ( j == NULL || size <= 0 ) {
		if ( j != NULL ) Kfree(j);
		return E_OBJ;
	}
	if ( js_parse(j, (INT)size, &root) < E_OK
	  || js_get_str(&root, "id", id, sizeof(id)) <= 0
	  || ( *p_name = name = js_dup(&root, "name") ) == NULL || name[0] == 0 ) {
		Kfree(j);
		return E_OBJ;
	}
	/* type and version as long as they are written, let go at the end */
	er = js_dup_opt(&root, "type", &type);
	if ( er >= E_OK ) er = js_dup_opt(&root, "version", &ver);
	if ( er < E_OK ) {
		Kfree(j);
		goto done;
	}
	/* the kinds of file its menu entries are for, lower case, each once */
	if ( js_get(&root, "contextMenu", &cm) >= E_OK ) {
		for ( it.s = NULL; js_next(&cm, &it); ) {
			if ( js_get(&it, "fileTypes", &ft) < E_OK ) continue;
			for ( t1.s = NULL; js_next(&ft, &t1); ) {
				UB	*w;

				if ( js_type(&t1) != JS_STRING ) continue;
				w = js_val_dup(&t1, "fileTypes");
				if ( w == NULL ) {
					er = E_NOMEM;
					break;
				}
				er = types_add(&types, &tcap, w);
				Kfree(w);
			}
			if ( er < E_OK ) break;
		}
	}
	if ( er < E_OK ) {
		Kfree(j);
		goto done;
	}
	if ( type == NULL ) type = nul;		/* absent: empty; nul is not let go */
	if ( ver == NULL ) ver = nul;
	kind = same(type, "base") ? "app" : same(type, "accessory") ? "accessory" : "utility";

	/* its template: the TADjs object of the directory, named by its UUID */
	if ( js_get(&root, "basefile", &bf) >= E_OK && js_type(&bf) == JS_OBJECT ) {
		char	us[TS_UUID_STRLEN + 1];
		INT	k;

		er = ( js_get_str(&bf, "json", jf, sizeof(jf)) >= TS_UUID_STRLEN ) ? E_OK : E_OBJ;
		for ( k = 0; er >= E_OK && k < TS_UUID_STRLEN; k++ ) us[k] = (char)jf[k];
		us[TS_UUID_STRLEN] = 0;
		if ( er >= E_OK && ( ts_str_to_uuid(us, &brought) < E_OK || ob_ref_obj(&brought, &r) < E_OK ) ) {
			er = E_OBJ;		/* the directory did not bring it */
		}
		has_base = (BOOL)( er >= E_OK );
	}
	Kfree(j);
	if ( er < E_OK ) {
		goto done;
	}

	/* a program the system has by that id: replaced only when asked to all the same */
	have = dt_prog_find(id);
	if ( have != NULL ) {
		if ( have->builtin ) {
			er = E_PAR;		/* the desktop's own is not replaced */
			goto done;
		}
		if ( !force ) {
			if ( p_prog != NULL ) *p_prog = have->uuid;
			er = E_LIMIT;
			goto done;
		}
		had = TRUE;
		old = have->uuid;
	}
	er = dt_pkg_admin(pid, win);
	if ( er < E_OK ) {
		goto done;
	}

	dt_sys_prt(&prt);
	if ( has_base ) {
		er = plugin_base(&brought, &prt, &base);
		if ( er < E_OK ) {
			goto done;
		}
	}
	/* the directory's whole name, and room for every text of the metadata escaped (6 bytes a byte at most) */
	from = knl_ob_name(dir, &fl, &er);
	m = NULL;
	mmax = 0;
	if ( from != NULL ) {
		mmax = 1024 + 6 * (INT)( fl + 2 * knl_strlen((CONST char *)name) + knl_strlen((CONST char *)id)
					 + knl_strlen((CONST char *)ver)
					 + ( types != NULL ? knl_strlen((CONST char *)types) : 0 ) ) + 16;
		m = (UB *)Kmalloc((size_t)mmax);
	}
	if ( m == NULL ) {
		if ( from != NULL || er >= E_OK ) er = E_NOMEM;
	} else {
		n = plugin_meta(m, mmax, from, id, name, kind, ver,
				has_base ? &base : NULL, types);
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = m;
		c.jsonsz = ( n > 0 ) ? n : 0;
		c.prt = &prt;
		if ( has_base ) {
			c.near = base;			/* beside its template */
		}
		er = ( n > 0 ) ? ob_cre_obj(&c, &prog) : E_LIMIT;
		Kfree(m);
	}
	if ( from != NULL ) {
		Kfree(from);
	}
	if ( er >= E_OK ) {
		er = ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_add(&box, &prog)
										     : E_SYS;
	}
	if ( er < E_OK ) {
		if ( has_base ) (void)ob_del_obj(&base);
		goto done;
	}
	if ( had ) {
		(void)dt_prog_drop(&old);		/* the version it replaces, and its template */
	}
	dt_prog_reread();				/* the registry sees it now */
	dt_base_box_put(&prog, TRUE);
	if ( p_prog != NULL ) *p_prog = prog;
	if ( p_how != NULL ) *p_how = had ? DT_HOW_UPDATED : DT_HOW_NEW;
	tm_printf((UB *)"dtprog: the plugin %s installed\n", id);
	er = E_OK;
done:
	if ( type != NULL && type != nul ) Kfree(type);
	if ( ver != NULL && ver != nul ) Kfree(ver);
	if ( types != NULL ) Kfree(types);
	return er;
}

/* The box read again: what shows the accessories shows them as they are now */
EXPORT void dt_prog_reread( void )
{
	read_box();
	dt_acc_boxes_tell();
}

EXPORT BOOL dt_prog_read( CONST TS_UUID *uuid, DTPROG *p )
{
	return read_prog(uuid, p);
}

EXPORT ER dt_prog_start( void )
{
	read_box();
	tm_printf((UB *)"TessronOS desktop: %d programs\n", nprog);

	return ( nprog > 0 ) ? E_OK : E_NOEXS;
}

EXPORT CONST DTPROG *dt_prog_find( CONST UB *id )
{
	INT	i, k;

	for ( i = 0; id != NULL && i < nprog; i++ ) {
		if ( same(prog[i]->id, (CONST char *)id) ) {
			return prog[i];
		}
	}
	/* no program has it now: the one that had it before */
	for ( i = 0; id != NULL && i < nprog; i++ ) {
		for ( k = 0; k < prog[i]->nalias; k++ ) {
			if ( same(prog[i]->alias[k], (CONST char *)id) ) {
				return prog[i];
			}
		}
	}
	return NULL;
}

/* The i-th program the registry knows, NULL past the last */
EXPORT CONST DTPROG *dt_prog_at( INT i )
{
	return ( i >= 0 && i < nprog ) ? prog[i] : NULL;
}

EXPORT CONST DTPROG *dt_prog_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; uuid != NULL && i < nprog; i++ ) {
		if ( ts_uuid_cmp(&prog[i]->uuid, uuid) == 0 ) {
			return prog[i];
		}
	}
	return NULL;
}

/* How many programs of a kind there are (0: of every kind) */
EXPORT INT dt_prog_count( UINT kind )
{
	INT	i, n = 0;

	for ( i = 0; i < nprog; i++ ) {
		if ( kind == 0 || prog[i]->kind == kind ) n++;
	}
	return n;
}

EXPORT INT dt_prog_list( UINT kind, CONST DTPROG **out, INT max )
{
	INT	i, n = 0;

	for ( i = 0; i < nprog && n < max; i++ ) {
		if ( kind == 0 || prog[i]->kind == kind ) {
			out[n++] = prog[i];
		}
	}
	return n;
}

/* The kind of object a link points at, as fileTypes names it: "xtad", or the extension of its file */
LOCAL BOOL obj_type( CONST T_VOBJ *v, UB *t, INT max )
{
	T_JSON	root, tf, f;
	UB	*meta, *name = NULL;
	SZ	size = 0;
	INT	n, i, k = 0;
	T_OBREC	r[2];

	t[0] = 0;
	meta = om_obj_meta(&v->target, &size);
	if ( meta != NULL ) {
		/* the file's name whole: its extension is what follows the last dot */
		if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
		  && js_get(&tf, "file", &f) >= E_OK && ( name = js_dup(&f, "name") ) != NULL
		  && ( n = (INT)knl_strlen((CONST char *)name) ) > 0 ) {
			for ( i = n - 1; i >= 0 && name[i] != '.'; i-- ) ;
			for ( i++; i > 0 && i < n && k < max - 1; i++ ) {
				t[k++] = ( name[i] >= 'A' && name[i] <= 'Z' ) ? (UB)( name[i] - 'A' + 'a' ) : name[i];
			}
			t[k] = 0;
		}
		if ( name != NULL ) Kfree(name);
		Kfree(meta);
		if ( k > 0 ) return TRUE;
	}
	/* an object whose record 0 is xmlTAD */
	n = om_obj_records(&v->target, r, 2);
	if ( n > 0 && r[0].recno == 0 && r[0].rt == OB_RT_TAD ) {
		t[0] = 'x';
		t[1] = 't';
		t[2] = 'a';
		t[3] = 'd';
		t[4] = 0;
		return TRUE;
	}
	return FALSE;
}

LOCAL BOOL has_type( CONST UB *types, CONST UB *t )
{
	INT	i = 0, k;

	while ( types[i] != 0 ) {
		for ( k = 0; t[k] != 0 && types[i + k] == t[k]; k++ ) ;
		if ( t[k] == 0 && ( types[i + k] == ',' || types[i + k] == 0 ) ) return TRUE;
		while ( types[i] != 0 && types[i] != ',' ) i++;
		if ( types[i] == ',' ) i++;
	}
	return FALSE;
}

LOCAL INT copy_s( UB *d, INT max, CONST UB *s )
{
	INT	i;

	for ( i = 0; s != NULL && s[i] != 0 && i < max - 1; i++ ) d[i] = s[i];
	d[i] = 0;
	return i;
}

/* One entry more, unless the program is in the list already: its place, or -1 */
LOCAL INT app_put( DTAPP *out, INT max, INT *n, CONST UB *id, CONST UB *raw, CONST UB *name, BOOL listed )
{
	INT	i;

	for ( i = 0; i < *n; i++ ) {
		if ( same(out[i].id, (CONST char *)id) ) return i;
	}
	if ( *n >= max ) return -1;
	out[i].name = dt_cat(( name != NULL ) ? (CONST char *)name : "", NULL);	/* whole */
	if ( out[i].name == NULL ) return -1;		/* said on the console */
	(void)copy_s(out[i].id, DT_PROG_ID, id);
	(void)copy_s(out[i].raw, DT_PROG_ID, raw);
	out[i].listed = listed;
	(*n)++;
	return i;
}

EXPORT void dt_app_free( DTAPP *a, INT n )
{
	INT	i;

	if ( a == NULL ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		if ( a[i].name != NULL ) Kfree(a[i].name);
	}
	Kfree(a);
}

EXPORT void dt_prog_clear( DTPROG *p )
{
	if ( p != NULL && p->name != NULL ) {
		Kfree(p->name);
		p->name = NULL;
	}
	if ( p != NULL && p->version != NULL ) {
		Kfree(p->version);
		p->version = NULL;
	}
	if ( p != NULL && p->types != NULL ) {
		Kfree(p->types);
		p->types = NULL;
	}
}

EXPORT INT dt_app_list( CONST T_VOBJ *v, DTAPP *out, INT max, INT *p_dflt )
{
	UB		**names = NULL, id[DT_PROG_ID], dflt[DT_PROG_ID], t[16];
	CONST DTPROG	*p;
	INT		na, i, n = 0;

	if ( p_dflt != NULL ) *p_dflt = -1;
	if ( v == NULL ) return 0;
	na = om_link_napps(v);
	if ( na < 0 ) na = 0;
	if ( out == NULL ) return na + nprog;

	/* the applist, each name as the system has the program */
	names = ( na > 0 ) ? om_link_apps(v, &na) : NULL;
	if ( names == NULL ) na = 0;
	for ( i = 0; i < na; i++ ) {
		if ( om_link_app_id(v, i, id, DT_PROG_ID) <= 0 ) continue;
		p = dt_prog_find(id);
		(void)app_put(out, max, &n, ( p != NULL ) ? p->id : id, id, ( p != NULL ) ? p->name : names[i], TRUE);
	}
	om_apps_free(names, na);

	/*
	 * An object that names no program (a file taken in): what opens an
	 * object of its kind. One that names any keeps to what it names, so
	 * that 機能付箋 are what its template or the person chose.
	 */
	if ( n == 0 && obj_type(v, t, sizeof(t)) ) {
		for ( i = 0; i < nprog; i++ ) {
			if ( prog[i]->types[0] == 0 || !has_type(prog[i]->types, t) ) continue;
			(void)app_put(out, max, &n, prog[i]->id, (CONST UB *)"", prog[i]->name, FALSE);
		}
	}

	if ( p_dflt != NULL && om_link_default_app(v, dflt, DT_PROG_ID) > 0 ) {
		p = dt_prog_find(dflt);
		if ( p != NULL ) (void)copy_s(dflt, DT_PROG_ID, p->id);
		for ( i = 0; i < n && *p_dflt < 0; i++ ) {
			if ( same(out[i].id, (CONST char *)dflt) ) *p_dflt = i;
		}
	}
	return n;
}

/*
 * What the system holds that no record links to: the システム箱, which
 * links to every box of the system, and the templates of the programs
 */
EXPORT INT dt_prog_roots( TS_UUID *ids, INT max )
{
	INT	i, k, n = 0;

	if ( n < max && ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &ids[n]) >= E_OK ) n++;
	for ( i = 0; i < nprog; i++ ) {
		for ( k = 0; k < prog[i]->nbase && n < max; k++ ) {
			ids[n++] = prog[i]->base[k];
		}
	}
	return n;
}

/* The name a template is shown by: its program's; NULL for what is not one */
EXPORT CONST char *dt_prog_base_name( CONST TS_UUID *target )
{
	INT	i, k;

	for ( i = 0; i < nprog; i++ ) {
		for ( k = 0; k < prog[i]->nbase; k++ ) {
			if ( ts_uuid_cmp(&prog[i]->base[k], target) == 0 ) {
				return (CONST char *)prog[i]->name;
			}
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- the system's 原紙箱 */

/* The 原紙箱 システム links to as the system's (SYSROLE_BASES): FALSE when there is none */
EXPORT BOOL dt_base_box( TS_UUID *p_box )
{
	return knl_sys_find(SYSROLE_BASES, NULL, p_box);
}

/*
 * A program's templates linked from the system's 原紙箱 (add), as
 * installing it puts them there, or the links to them taken out of it
 * (as it is deleted). Links to them in other boxes are the person's and
 * stay. The templates are held by the hold installing gave them, not by
 * these links: taking a link out of a 原紙箱 leaves the template, for
 * 原紙集め to show.
 */
EXPORT void dt_base_box_put( CONST TS_UUID *prog, BOOL add )
{
	DTPROG	*p;
	TS_UUID	box;
	INT	k;
	ER	er;

	if ( prog == NULL || !dt_base_box(&box) ) {
		return;
	}
	p = (DTPROG *)Kcalloc(1, sizeof(DTPROG));
	if ( p == NULL ) {
		return;
	}
	if ( read_prog(prog, p) && p->kind == DT_PK_APP ) {
		for ( k = 0; k < p->nbase; k++ ) {
			er = add ? om_obj_link_add(&box, &p->base[k]) : om_obj_link_del(&box, &p->base[k]);
			tm_printf((UB *)"dtprog: template %d of %s %s the 原紙箱 (%d)\n",
				  k, p->id, add ? "linked from" : "taken out of", (INT)er);
		}
	}
	dt_prog_clear(p);
	Kfree(p);
}

/*
 * An object made by the person from a template (tmpl): made the
 * person's, and, when the template is the 原紙箱 program's own, the new
 * 原紙箱 given links to the templates the system's 原紙箱 links to now,
 * in its order and laid out as om_obj_links_set lays a box (closed,
 * as wide as the name, LINK_GAP below the one before). A box of the
 * person's starts with what the system has, an application installed
 * later included; a ユーザ原紙 that is a 原紙箱 is copied as it is.
 */
/*
 * The place an object made from a template keeps is the template's, as
 * every other thing of its metadata is. Where a window stands at that
 * very corner now -- the system's 原紙箱, beside which a 原紙箱 made from
 * its template is opened, keeps the template's place -- the new object
 * keeps a place a step down and across, so that its window does not
 * hide the other when it is opened.
 */
#define MADE_STEP	28
#define MADE_STEPS	8

LOCAL void made_place( CONST TS_UUID *made )
{
	T_DPRECT	r;
	T_WMWIN		w;
	INT		*wids, n, i, k;
	BOOL		over = TRUE, moved = FALSE;

	if ( om_store_window(made, &r) < E_OK ) {
		return;				/* no place of its own: it opens where a new window opens */
	}
	n = dt_wins(&wids);			/* every window, however many are open */
	for ( k = 0; k < MADE_STEPS && over; k++ ) {
		over = FALSE;
		for ( i = 0; i < n && !over; i++ ) {
			over = (BOOL)( wm_ref(wids[i], &w) >= E_OK && ( w.attr & WM_ATTR_POPUP ) == 0
				       && w.outer.left == r.left && w.outer.top == r.top );
		}
		if ( over ) {
			r.left += MADE_STEP;
			r.right += MADE_STEP;
			r.top += MADE_STEP;
			r.bottom += MADE_STEP;
			moved = TRUE;
		}
	}
	if ( wids != NULL ) Kfree(wids);
	if ( moved ) {
		(void)om_store_set_window(made, &r);
		tm_printf((UB *)"dtprog: made from a template, kept at %d,%d beside the window there\n",
			  (INT)r.left, (INT)r.top);
	}
}

EXPORT void dt_template_made( CONST TS_UUID *tmpl, CONST TS_UUID *made )
{
	CONST DTPROG	*bb;
	TS_UUID		box, *ids = NULL;
	T_TAD		*doc = NULL;
	T_VOBJ		v = { .recno = 0 };
	UB		*rec;
	SZ		size = 0;
	INT		i, k, n = 0, all;
	ER		er;

	(void)om_store_set_maker(made);
	made_place(made);
	if ( ts_str_to_uuid(SYSDEF_PROG_BASEBOX, &box) < E_OK
	  || ( bb = dt_prog_of(&box) ) == NULL ) {
		return;
	}
	for ( k = 0; k < bb->nbase && ts_uuid_cmp(&bb->base[k], tmpl) != 0; k++ ) ;
	if ( k == bb->nbase || !dt_base_box(&box) || ts_uuid_cmp(&box, made) == 0 ) {
		return;
	}
	rec = om_obj_record(&box, 0, &size);
	if ( rec == NULL ) {
		return;
	}
	er = tad_parse(rec, size, NULL, &doc);
	Kfree(rec);
	if ( er < E_OK || doc == NULL ) {
		return;
	}
	tad_vobj_init(&v);
	all = tad_lnk_count(doc);
	ids = ( all > 0 ) ? (TS_UUID *)Kcalloc((size_t)all, sizeof(TS_UUID)) : NULL;
	for ( i = 0; ids != NULL && i < all; i++ ) {
		if ( tad_lnk_get(doc, i, &v) >= E_OK ) {
			ids[n++] = v.target;
		}
	}
	tad_vobj_clear(&v);
	tad_free(doc);
	er = ( n > 0 ) ? om_obj_links_set(made, ids, n) : E_OK;
	if ( ids != NULL ) Kfree(ids);
	tm_printf((UB *)"dtprog: new 原紙箱 given the system's %d templates (%d)\n", n, (INT)er);
}

/* ---------------------------------------------------------------- running one */

/*
 * A process is started by a task of its own, not by the desktop's.
 * Making it reads the whole program into the new space, and a large
 * program (the browser is over 100MB) takes seconds to read from an SD
 * card; the desktop's task is the one that follows the pointer and
 * moves the windows, and would stand still for all that time. The
 * desktop hands the start over and goes on; the starter makes the
 * process object, answers the process that asked for it when one did,
 * and says on the message line when the program could not be started.
 * One starter takes the starts in the order they were asked for.
 */
#define START_QLEN	8		/* starts waiting for the starter */
#define START_PRI	21		/* just below the desktop, which has input to follow */
#define START_STKSZ	(32 * 1024)

typedef struct {
	TS_UUID	prog;			/* the program object */
	T_PRCOPEN arg;			/* the object to open, when has_arg, and the link it was opened from */
	BOOL	has_arg;
	BOOL	link_arg;		/* the program is given all of arg (openArg "link"), not the object alone */
	TS_UUID	reply;			/* where to answer; 0 for nobody */
	UINT	seq;
	SZ	anssz;			/* how much of a T_DTANS the asker reads */
	BOOL	with_er;		/* the answer says whether the start failed */
	UINT	ticket;			/* of the person's act that asked for it, 0 none */
	UINT	rgroup, rank;		/* of a group opened together (dt_prog_rank), 0 none */
	struct watchent	*watch;		/* its place in the watched starts; NULL not watched */
	struct openent	*opening;	/* its place in the objects being opened; NULL none */
	INT	info;			/* a 管理情報 started: its place in dtinfo.c's table, from 1; 0 none */
	UB	*name;			/* the program, for the message line, whole (Kmalloc); NULL none */
	SZ	rawsz;			/* a start-up argument of the program's own (dt_prog_run_arg), 0 none */
	UB	*raw;			/* (Kmalloc) */
	UB	*data;			/* the 固有データ given after arg (Kmalloc); NULL none */
	SZ	datalen;
} DTSTART;

/* What a start holds let go, once it is over: whoever has the DTSTART then */
LOCAL void start_free( DTSTART *s )
{
	if ( s->raw != NULL ) Kfree(s->raw);
	if ( s->data != NULL ) Kfree(s->data);
	if ( s->name != NULL ) Kfree(s->name);
	s->raw = s->data = s->name = NULL;
	s->rawsz = s->datalen = 0;
}

LOCAL ID	start_mbf = 0;
LOCAL ID	start_tsk = 0;

/*
 * What the desktop opened by itself as it started (auto_open), watched
 * until each process has shown its first picture or has ended: the
 * start is over then (dt_prog_settled). Each start has its own place,
 * given as it is asked; the task that starts it fills it in.
 */
typedef struct watchent {
	struct watchent	*next;
	TS_UUID		proc;		/* the process; 0 for a start that failed */
	UINT		done;		/* proc is filled in */
} WATCHENT;

LOCAL BOOL	watch_on = FALSE;		/* the starts asked now are watched */
LOCAL INT	watch_asked = 0;		/* of the desktop's task */
LOCAL WATCHENT	*watch_head, *watch_tail;	/* in the order asked: only the desktop's task adds, a start's place is filled by the starter */

EXPORT void dt_prog_watch( BOOL on )
{
	watch_on = on;
}

/*
 * The starts asked now are of a group of windows opened together, at
 * this place in it (auto_open): the window each process puts up first
 * takes that place among the group's (wm_set_rank). 0 ends it.
 */
LOCAL UINT	rank_group = 0, rank_place = 0;

EXPORT void dt_prog_rank( UINT group, UINT rank )
{
	rank_group = group;
	rank_place = rank;
}

/*
 * Objects being opened in a process's window. A program started on an
 * object takes a while to put its window up (the start is the
 * starter's, and a large program is read in for seconds), and until it
 * has, no window says it shows the object (wm_obj_showing). An open
 * asked for in that time -- a double click done twice quickly, 実行
 * chosen while the first is still starting -- finds the object here and
 * starts nothing: one object is open in one window. An entry is
 * forgotten when the start failed, when the process has ended, and once
 * the process has shown a window, which says what it shows from then on.
 *
 * The desktop's task takes a free place and asks the start; the starter
 * says how the start went (OP_STARTED with the process, or OP_FAILED);
 * the desktop's task alone frees the place again. The state is read and
 * written with __atomic, the process written before OP_STARTED is.
 */
#define OP_FREE		0
#define OP_STARTING	1		/* handed to the starter */
#define OP_STARTED	2		/* the process is there */
#define OP_FAILED	3

typedef struct openent {
	struct openent	*next;
	UINT		state;
	TS_UUID		obj;
	TS_UUID		proc;
	UINT		keys;		/* asked again: the ticket of that act (wm_act_ticket), 0 none */
} OPENENT;

/* A place is kept once made and used again when it is free, so the table is as long as the most opened at once */
LOCAL OPENENT	*opening_head;

/* A place taken for an object about to be opened: NULL for no memory */
LOCAL OPENENT *opening_take( CONST TS_UUID *obj )
{
	OPENENT	*e;

	for ( e = opening_head; e != NULL; e = e->next ) {
		if ( __atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == OP_FREE ) {
			break;
		}
	}
	if ( e == NULL ) {
		e = (OPENENT *)Kcalloc(1, sizeof(OPENENT));
		if ( e == NULL ) {
			tm_printf((UB *)"TessronOS desktop: no memory to watch an object being opened\n");
			return NULL;
		}
		e->next = opening_head;
		opening_head = e;
	}
	e->obj = *obj;
	e->keys = 0;
	__atomic_store_n(&e->state, OP_STARTING, __ATOMIC_RELEASE);
	return e;
}

/* Whether a place still stands for an object being opened; freed when not */
LOCAL BOOL opening_stands( OPENENT *e )
{
	UINT	st = __atomic_load_n(&e->state, __ATOMIC_ACQUIRE);
	ID	pid;

	if ( st == OP_FREE || st == OP_STARTING ) {
		return (BOOL)( st == OP_STARTING );
	}
	if ( st == OP_STARTED ) {
		pid = knl_prc_of_uuid(&e->proc);
		if ( pid > 0 && !wm_prc_drawn(pid) ) {
			return TRUE;
		}
		/*
		 * Asked to be opened again while it was being started: the
		 * press of that ask came after the one the start carried, so
		 * the window is given the keys here, unless the person has
		 * gone on to something else since
		 */
		if ( pid > 0 && e->keys != 0 && wm_act_good(e->keys) ) {
			INT	wid = wm_obj_showing(&e->obj);

			if ( wid > 0 ) {
				wm_raise(wid);
				(void)wm_focus(wid);
				wm_update();
			}
		}
	}
	__atomic_store_n(&e->state, OP_FREE, __ATOMIC_RELEASE);
	return FALSE;
}

EXPORT BOOL dt_prog_opening( CONST TS_UUID *obj )
{
	OPENENT	*e;
	BOOL	is = FALSE;

	for ( e = opening_head; e != NULL; e = e->next ) {
		if ( opening_stands(e) && ts_uuid_cmp(&e->obj, obj) == 0 ) {
			is = TRUE;
		}
	}
	return is;
}

EXPORT void dt_prog_opening_keys( CONST TS_UUID *obj )
{
	OPENENT	*e;

	for ( e = opening_head; e != NULL; e = e->next ) {
		if ( opening_stands(e) && ts_uuid_cmp(&e->obj, obj) == 0 ) {
			e->keys = ( dt_opens_quiet > 0 ) ? 0 : wm_act_ticket();
		}
	}
}

EXPORT void dt_prog_opening_poll( void )
{
	OPENENT	*e;

	for ( e = opening_head; e != NULL; e = e->next ) {
		(void)opening_stands(e);
	}
}

/*
 * TRUE once every start watched is done and its process has shown what
 * it drew in a window of its own (wm_prc_drawn) or is gone; *p_drawn
 * and *p_gone say how many of each, *p_all how many were watched
 */
EXPORT BOOL dt_prog_settled( INT *p_drawn, INT *p_gone, INT *p_all )
{
	TS_UUID		none;
	INT		n = 0, drawn = 0, gone = 0;
	ID		pid;
	WATCHENT	*e;

	knl_memset(&none, 0, sizeof(none));
	for ( e = watch_head; e != NULL; e = e->next ) {
		if ( __atomic_load_n(&e->done, __ATOMIC_ACQUIRE) == 0 ) {
			break;
		}
		pid = ( ts_uuid_cmp(&e->proc, &none) != 0 ) ? knl_prc_of_uuid(&e->proc) : 0;
		if ( pid <= 0 ) {
			gone++;
		} else if ( wm_prc_drawn(pid) ) {
			drawn++;
		} else {
			break;
		}
		n++;
	}
	*p_drawn = drawn;
	*p_gone = gone;
	*p_all = watch_asked;
	/* over once every start watched is up, and none is to be asked any more */
	return (BOOL)( n == watch_asked && !watch_on );
}

/* The answer the next start takes over (dt_prog_reply) */
LOCAL struct {
	BOOL	on;
	BOOL	taken;
	TS_UUID	reply;
	UINT	seq;
	SZ	size;
	BOOL	with_er;
} start_reply;

EXPORT void dt_prog_reply( CONST TS_UUID *reply, UINT seq, SZ size, BOOL with_er )
{
	knl_memset(&start_reply, 0, sizeof(start_reply));
	if ( reply != NULL ) {
		start_reply.on = TRUE;
		start_reply.reply = *reply;
		start_reply.seq = seq;
		start_reply.size = size;
		start_reply.with_er = with_er;
	}
}

EXPORT BOOL dt_prog_reply_taken( void )
{
	return start_reply.taken;
}

/*
 * The process object made from the program, given the object to open:
 * its UUID as the start-up argument, or, for a program that asks for it,
 * the link it was opened from after it (T_PRCOPEN)
 */
LOCAL ER start_now( CONST DTSTART *s, TS_UUID *pu )
{
	T_OBCRE	c;
	UB	*arg = NULL;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = s->prog;
	if ( s->rawsz > 0 ) {
		c.arg = s->raw;
		c.argsz = s->rawsz;
	} else if ( s->has_arg && s->link_arg && s->data != NULL && s->datalen > 0 ) {
		/* the link, and after it the 固有データ as long as it is, ending in 0 (dt_prcopen_data) */
		SZ	size = (SZ)sizeof(T_PRCOPEN) + s->datalen + 1;

		arg = (UB *)Kmalloc((size_t)size);
		if ( arg == NULL ) {
			return E_NOMEM;
		}
		knl_memcpy(arg, &s->arg, sizeof(T_PRCOPEN));
		knl_memcpy(arg + sizeof(T_PRCOPEN), s->data, s->datalen);
		arg[size - 1] = 0;
		c.arg = arg;
		c.argsz = size;
	} else if ( s->has_arg ) {
		c.arg = &s->arg;
		c.argsz = s->link_arg ? sizeof(s->arg) : sizeof(s->arg.target);
	}
	knl_ob_act_arm(s->ticket);
	knl_ob_rank_arm(s->rgroup, s->rank);
	er = ob_cre_obj(&c, pu);
	knl_ob_rank_arm(0, 0);
	knl_ob_act_arm(0);
	if ( arg != NULL ) Kfree(arg);
	return er;
}

/*
 * A start done: the asker answered with the process, and a start that
 * failed told on the message line and the console, unless the answer
 * already says so to the one who asked.
 */
LOCAL void start_done( CONST DTSTART *s, ER er, CONST TS_UUID *pu )
{
	T_DTANS	an;
	TS_UUID	none;
	BOOL	asked;

	knl_memset(&none, 0, sizeof(none));
	asked = (BOOL)( ts_uuid_cmp(&s->reply, &none) != 0 );
	if ( asked ) {
		knl_memset(&an, 0, sizeof(an));
		an.seq = s->seq;
		an.er = s->with_er ? er : E_OK;
		if ( er >= E_OK && pu != NULL ) {
			an.proc = *pu;
		}
		dt_answer(&s->reply, &an, s->anssz);
	}
	if ( er < E_OK && !( asked && s->with_er ) ) {
		CONST char	*pn = ( s->name != NULL ) ? (CONST char *)s->name : "プログラム";
		UB		*msg = dt_cat(pn, "を起動できませんでした", NULL);	/* the line shows what fits */

		(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg : "プログラムを起動できませんでした");
		if ( msg != NULL ) Kfree(msg);
		tm_printf((UB *)"TessronOS desktop: %s not started (%d)\n", pn, (INT)er);
	}
	if ( s->watch != NULL ) {
		if ( er >= E_OK && pu != NULL ) {
			s->watch->proc = *pu;
		}
		__atomic_store_n(&s->watch->done, 1, __ATOMIC_RELEASE);
	}
	if ( s->info > 0 ) {
		dt_info_started(s->info, er, pu);
	}
	if ( s->opening != NULL ) {
		if ( er >= E_OK && pu != NULL ) {
			s->opening->proc = *pu;
			__atomic_store_n(&s->opening->state, OP_STARTED, __ATOMIC_RELEASE);
		} else {
			__atomic_store_n(&s->opening->state, OP_FAILED, __ATOMIC_RELEASE);
		}
	}
}

LOCAL void start_task( INT stacd, void *exinf )
{
	DTSTART	s;
	TS_UUID	pu;
	ER	er;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		if ( tk_rcv_mbf(start_mbf, &s, TMO_FEVR) != (INT)sizeof(s) ) {
			continue;
		}
		knl_memset(&pu, 0, sizeof(pu));
		er = start_now(&s, &pu);
		start_done(&s, er, &pu);
		start_free(&s);
	}
}

/* The starter, made the first time it is wanted; FALSE when it cannot be */
LOCAL BOOL starter( void )
{
	T_CMBF	cmbf;
	T_CTSK	ctsk;
	ID	id;

	if ( start_tsk > 0 ) {
		return TRUE;
	}
	if ( start_mbf <= 0 ) {
		knl_memset(&cmbf, 0, sizeof(cmbf));
		cmbf.mbfatr = TA_TFIFO;
		cmbf.bufsz = (SZ)( START_QLEN * ( sizeof(DTSTART) + sizeof(W) ) );
		cmbf.maxmsz = (INT)sizeof(DTSTART);
		id = tk_cre_mbf(&cmbf);
		if ( id <= 0 ) {
			return FALSE;
		}
		start_mbf = id;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)start_task;
	ctsk.itskpri = START_PRI;
	ctsk.stksz = START_STKSZ;
	id = tk_cre_tsk(&ctsk);
	if ( id <= 0 ) {
		return FALSE;
	}
	if ( tk_sta_tsk(id, 0) < E_OK ) {
		tk_del_tsk(id);
		return FALSE;
	}
	start_tsk = id;
	return TRUE;
}

/*
 * A process started from the program: handed to the starter, or started
 * here when there is none. from and v, when given, are the window of the
 * desktop's and the link in it the object was opened from. opens: the
 * process is to show the object arg in its window, which is kept in
 * opening until it does.
 */
LOCAL UB	*raw_next = NULL;		/* the argument dt_prog_run_arg gives the next start (Kmalloc) */
LOCAL SZ	raw_next_sz = 0;

LOCAL INT	start_info = 0;		/* the place of the 管理情報 dt_prog_start_info starts */

/*
 * 固有データ (design 11.3). The system does not read it: it is the
 * string "data" of the applist entry of the program started, given to
 * the program as it is, as long as it is (after T_PRCOPEN). A link's own
 * entry comes before the object's, as a link fixed to its program does;
 * whether it is fixed does not matter.
 */
LOCAL UB	*data_next = NULL;		/* dt_prog_data_next's, for the next start (Kmalloc) */
LOCAL SZ	data_next_len = 0;
LOCAL BOOL	data_next_on = FALSE;

EXPORT UB *dt_prog_data( CONST DTPROG *p, CONST TS_UUID *target, CONST UB *link_applist, SZ *p_len )
{
	UB	*d;
	INT	i;

	*p_len = 0;
	if ( p == NULL ) {
		return NULL;
	}
	d = om_app_data(target, link_applist, p->id, p_len);
	for ( i = 0; d == NULL && i < p->nalias; i++ ) {
		d = om_app_data(target, link_applist, p->alias[i], p_len);
	}
	if ( d == NULL ) {
		*p_len = 0;
	}
	return d;
}

EXPORT ER dt_prog_data_next( BOOL on, CONST UB *data, SZ len )
{
	if ( data_next != NULL ) {
		Kfree(data_next);
		data_next = NULL;
	}
	data_next_len = 0;
	data_next_on = on;
	if ( on && data != NULL ) {
		data_next = (UB *)Kmalloc((size_t)( len + 1 ));
		if ( data_next == NULL ) {
			data_next_on = FALSE;
			return E_NOMEM;
		}
		knl_memcpy(data_next, data, len);
		data_next[len] = 0;
		data_next_len = len;
	}
	return E_OK;
}

/* The data dt_prog_data_next gave, taken: FALSE when none was given */
EXPORT BOOL dt_prog_data_take( UB **p_data, SZ *p_len )
{
	if ( !data_next_on ) {
		*p_data = NULL;
		*p_len = 0;
		return FALSE;
	}
	*p_data = data_next;
	*p_len = data_next_len;
	data_next = NULL;
	data_next_len = 0;
	data_next_on = FALSE;
	return TRUE;
}

LOCAL ER start_process( CONST DTPROG *p, CONST TS_UUID *arg, CONST DTWIN *from, CONST T_VOBJ *v, BOOL opens )
{
	DTSTART	s;
	TS_UUID	pu;
	ER	er;

	knl_memset(&s, 0, sizeof(s));
	s.info = start_info;
	if ( raw_next_sz > 0 ) {
		s.raw = raw_next;		/* the start's now */
		s.rawsz = raw_next_sz;
		raw_next = NULL;
		raw_next_sz = 0;
	}
	s.prog = p->uuid;
	s.ticket = ( dt_opens_quiet > 0 ) ? 0 : wm_act_ticket();
	s.rgroup = rank_group;
	s.rank = rank_place;
	if ( watch_on ) {
		WATCHENT	*we = (WATCHENT *)Kcalloc(1, sizeof(WATCHENT));

		if ( we != NULL ) {
			if ( watch_tail != NULL ) watch_tail->next = we;
			else watch_head = we;
			watch_tail = we;
			watch_asked++;
			s.watch = we;
		} else {
			tm_printf((UB *)"TessronOS desktop: no memory to watch a start\n");
		}
	}
	s.arg.recno = -1;
	if ( arg != NULL ) {
		s.arg.target = *arg;
		s.has_arg = TRUE;
		s.link_arg = p->open_link;
		if ( opens ) {
			s.opening = opening_take(arg);
		}
	}
	if ( v != NULL ) {
		s.arg.recno = v->recno;
		s.arg.fromlink = TRUE;
		s.arg.bgcol = (UINT)v->bgcol;
		s.arg.wordwrap = v->wordwrap;
		/* a viewMode longer than the field is no keyword a window knows: given as one (dtreq.h) */
		if ( v->vview_len > 0 && v->vview_len < DT_VIEW_MAX ) {
			knl_memcpy(s.arg.viewmode, v->vview, v->vview_len);
			s.arg.viewmode[v->vview_len] = 0;
		} else if ( v->vview_len >= DT_VIEW_MAX ) {
			knl_strcpy((char *)s.arg.viewmode, DT_VIEW_UNKNOWN);
		}
		if ( from != NULL && from->used ) {
			s.arg.parent = from->id;
			s.arg.vobjid = v->vobjid;
		} else if ( dt_plink_opening(v, &s.arg.parent) ) {
			s.arg.vobjid = v->vobjid;	/* a link of a program's window, written through it */
		}
	}
	/* the applist's data for the program: given with the link (openArg "link") */
	if ( !dt_prog_data_take(&s.data, &s.datalen) ) {
		s.data = dt_prog_data(p, ( v != NULL ) ? &v->target : arg, ( v != NULL ) ? v->vapplist : NULL,
				      &s.datalen);
	}
	if ( s.data != NULL && s.datalen > 0 && p->open_link && !s.has_arg && s.rawsz == 0 ) {
		/* no object: target 0, the data alone */
		s.has_arg = TRUE;
		s.link_arg = TRUE;
	}
	s.name = dt_cat(( p->name != NULL ) ? (CONST char *)p->name : "", NULL);	/* NULL: said as プログラム */
	if ( start_reply.on && !start_reply.taken ) {
		s.reply = start_reply.reply;
		s.seq = start_reply.seq;
		s.anssz = start_reply.size;
		s.with_er = start_reply.with_er;
		start_reply.taken = TRUE;
	}
	if ( starter() ) {
		er = tk_snd_mbf(start_mbf, &s, (INT)sizeof(s), TMO_POL);
		if ( er >= E_OK ) {
			return E_OK;		/* what it holds is the starter's to let go */
		}
		if ( er != E_TMOUT ) {
			start_done(&s, er, NULL);
			start_free(&s);
			return er;
		}
	}
	/* no starter, or it has more than it can hold: started here */
	knl_memset(&pu, 0, sizeof(pu));
	er = start_now(&s, &pu);
	start_done(&s, er, &pu);
	start_free(&s);
	return er;
}

/*
 * A command opened, wherever it is linked from: it has no window of its
 * own and what it prints is the console's, so a コンソール is started
 * for it, given the command's object, and runs it. E_NOEXS when the
 * object is no command.
 */
EXPORT ER dt_command_run( CONST TS_UUID *target )
{
	DTPROG		*p = (DTPROG *)Kcalloc(1, sizeof(DTPROG));
	CONST DTPROG	*con;
	TS_UUID		cu;
	BOOL		is;

	if ( p == NULL ) {
		return E_NOMEM;
	}
	is = (BOOL)( read_prog(target, p) && p->kind == DT_PK_COMMAND && !p->builtin );
	dt_prog_clear(p);
	Kfree(p);
	if ( !is || ts_str_to_uuid(SYSDEF_PROG_CONSOLE, &cu) < E_OK || ( con = dt_prog_of(&cu) ) == NULL ) {
		return E_NOEXS;
	}
	return start_process(con, target, NULL, NULL, FALSE);
}

/*
 * 管理情報 started on the link v of the window from (the link its
 * start-up argument): what process it is, or that it failed, is told
 * to dtinfo.c's place slot once the starter has made it (dt_info_started)
 */
EXPORT ER dt_prog_start_info( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v, INT slot )
{
	ER	er;

	start_info = slot;
	er = start_process(p, &v->target, from, v, FALSE);
	start_info = 0;
	return er;
}

/*
 * The object v points at opened in the window of a program that is a
 * process. Until the window is up the object counts as open
 * (dt_prog_opening). One of the desktop's own, or a command, is run as
 * dt_prog_run runs it.
 */
EXPORT ER dt_prog_open( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v )
{
	if ( p == NULL || v == NULL ) {
		return E_PAR;
	}
	if ( p->builtin || p->kind == DT_PK_COMMAND ) {
		return dt_prog_run(from, p, v);
	}
	/* a program of one window, up already: that window told, with the object and its data */
	if ( p->once ) {
		ER	er;

		if ( dt_prog_again(p, v, &er) ) {
			return er;
		}
	}
	return start_process(p, &v->target, from, v, TRUE);
}

/*
 * A program started with a start-up argument of its own, as a process,
 * unless a window of it is up already
 */
EXPORT ER dt_prog_run_arg( DTWIN *from, CONST DTPROG *p, CONST void *arg, SZ size )
{
	INT	wid;

	if ( p == NULL || p->builtin || size <= 0 ) {
		return E_PAR;
	}
	/* a window of it standing for its program object ("shows"): brought to the front, as dt_prog_run does */
	if ( ( wid = wm_obj_showing(&p->uuid) ) > 0 ) {
		wm_raise(wid);
		(void)wm_focus(wid);
		wm_update();
		return E_OK;
	}
	if ( raw_next != NULL ) Kfree(raw_next);
	raw_next = (UB *)Kmalloc((size_t)size);
	if ( raw_next == NULL ) {
		raw_next_sz = 0;
		return E_NOMEM;
	}
	knl_memcpy(raw_next, arg, size);
	raw_next_sz = size;
	return start_process(p, NULL, from, NULL, FALSE);
}

/*
 * A program run: one of the desktop's own by what it is, any other as a
 * process object made from it, given the object to open (its UUID as
 * the start-up argument) when there is one.
 */
EXPORT ER dt_prog_run( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v )
{
	if ( p == NULL ) {
		return E_PAR;
	}
	if ( p->kind == DT_PK_COMMAND && !p->builtin ) {
		return dt_command_run(&p->uuid);
	}
	if ( same(p->id, "base-file-manager") || same(p->id, "accessory-box") ) {
		/*
		 * The 原紙箱's or the 小物箱's program run by itself, or on its
		 * own program object: the system's 原紙箱 or 小物箱 opened with
		 * it, in one window, as a double click on its link opens it
		 */
		T_VOBJ	bv = { .recno = 0 };
		BOOL	bb = same(p->id, "base-file-manager");
		ER	er;

		tad_vobj_init(&bv);
		if ( v != NULL && ts_uuid_cmp(&v->target, &p->uuid) != 0 ) {
			if ( ( er = tad_vobj_copy(&bv, v) ) < E_OK ) {
				return er;
			}
		} else if ( bb ? !dt_base_box(&bv.target) : !knl_sys_find(SYSROLE_ACCS, SYSDEF_ACC_BOX, &bv.target) ) {
			(void)wm_msg_put(bb ? "システムの原紙箱がありません" : "システムの小物箱がありません");
			return E_NOEXS;
		}
		er = dt_open_as(from, &bv, p->id);
		tad_vobj_clear(&bv);
		return er;
	}
	if ( !p->builtin ) {
		/*
		 * Run by itself while a window of it stands for its program
		 * object (an accessory's, a tool's, "shows"): that window
		 * brought to the front and the program told it was run again,
		 * one window for the program as for any object
		 */
		ER	er;

		if ( dt_prog_again(p, v, &er) ) {
			return er;
		}
		return start_process(p, ( v != NULL ) ? &v->target : NULL, from, v, FALSE);
	}
	if ( same(p->id, "force-quit") ) {
		knl_fq_open();			/* 強制終了's own window (outer_kernel/wm/fquit.c) */
		return E_OK;
	}
	/* an editor: the object opened in a window of that kind */
	if ( v == NULL ) {
		return E_PAR;
	}
	return dt_open_as(from, v, p->id);
}
