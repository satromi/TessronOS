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
 *		"name": "基本文章編集", "base": [ UUID ], "builtin": true } }
 *	The id is the one an applist names, so what a TADjs Desktop wrote
 *	opens with the program it names. "aliases" lists ids the program
 *	had before (書庫管理 took in 書庫解凍, unpack-file): an applist that
 *	still names one of them opens with it. "kind" is app (it has templates,
 *	原紙, and opens objects), accessory (小物: no data of its own),
 *	utility (a tool of the system) or command (no window of its own: run
 *	in a コンソール, which shows what it prints). A program the desktop carries in
 *	itself is "builtin" and has no executable; one that is a process has
 *	its executable in a record of type 9 (tessronos.exec).
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

/* ---------------------------------------------------------------- what a program says */

/* A program object's metadata read into an entry; FALSE when it is not a program */
LOCAL BOOL read_prog( CONST TS_UUID *uuid, DTPROG *p )
{
	T_JSON	root, tf, g, base, it;
	UB	*meta, s[TAD_NAME_MAX];
	SZ	size = 0;
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
		if ( js_get_str(&g, "name", p->name, TAD_NAME_MAX) <= 0 ) {
			(void)js_get_str(&root, "name", p->name, TAD_NAME_MAX);
		}
		p->kind = DT_PK_APP;
		if ( js_get_str(&g, "kind", s, sizeof(s)) > 0 ) {
			p->kind = same(s, "accessory") ? DT_PK_ACCESSORY
				: same(s, "utility") ? DT_PK_UTILITY
				: same(s, "command") ? DT_PK_COMMAND : DT_PK_APP;
		}
		if ( js_get_str(&g, "version", p->version, DT_VER_MAX) <= 0 ) {
			p->version[0] = 0;
		}
		p->builtin = js_get_bool(&g, "builtin", FALSE);
		p->menu = js_get_bool(&g, "menu", FALSE);

		/* the kinds of object it opens from 実行: "fileTypes": ["xtad", "tad"] */
		p->types[0] = 0;
		if ( js_get(&g, "fileTypes", &base) >= E_OK ) {
			INT	k = 0;

			for ( it.s = NULL; js_next(&base, &it); ) {
				UB	t[16];
				INT	q, n = js_str(&it, t, sizeof(t));

				if ( n <= 0 || k + n + 2 >= DT_TYPES_MAX ) continue;
				if ( k > 0 ) p->types[k++] = ',';
				for ( q = 0; q < n; q++ ) {
					p->types[k++] = ( t[q] >= 'A' && t[q] <= 'Z' ) ? (UB)( t[q] - 'A' + 'a' ) : t[q];
				}
				p->types[k] = 0;
			}
		}

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
				nprog++;
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
#define PLUGIN_JSON_MAX	( 64 * 1024 )

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

EXPORT ER dt_plugin_install( CONST TS_UUID *dir, BOOL force, TS_UUID *p_prog, UINT *p_how )
{
	T_JSON		root, bf, cm, it, ft, t1;
	UB		id[DT_PROG_ID], name[TAD_NAME_MAX], type[32], ver[32], jf[128];
	UB		types[DT_TYPES_MAX];
	UB		*j, *m;
	SZ		size = 0;
	INT		n;
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
	if ( j == NULL || size <= 0 || size > PLUGIN_JSON_MAX ) {
		if ( j != NULL ) Kfree(j);
		return E_OBJ;
	}
	if ( js_parse(j, (INT)size, &root) < E_OK
	  || js_get_str(&root, "id", id, sizeof(id)) <= 0
	  || js_get_str(&root, "name", name, sizeof(name)) <= 0 ) {
		Kfree(j);
		return E_OBJ;
	}
	if ( js_get_str(&root, "type", type, sizeof(type)) <= 0 ) type[0] = 0;
	/* the kinds of file its menu entries are for, lower case, each once */
	types[0] = 0;
	if ( js_get(&root, "contextMenu", &cm) >= E_OK ) {
		INT	k = 0;

		for ( it.s = NULL; js_next(&cm, &it); ) {
			if ( js_get(&it, "fileTypes", &ft) < E_OK ) continue;
			for ( t1.s = NULL; js_next(&ft, &t1); ) {
				UB	w[16];
				INT	q, len = js_str(&t1, w, sizeof(w));

				for ( q = 0; q < len; q++ ) if ( w[q] >= 'A' && w[q] <= 'Z' ) w[q] = (UB)( w[q] - 'A' + 'a' );
				if ( len <= 0 || has_type(types, w) || k + len + 2 >= DT_TYPES_MAX ) continue;
				if ( k > 0 ) types[k++] = ',';
				for ( q = 0; q < len; q++ ) types[k++] = w[q];
				types[k] = 0;
			}
		}
	}
	if ( js_get_str(&root, "version", ver, sizeof(ver)) <= 0 ) ver[0] = 0;
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
		return er;
	}

	/* a program the system has by that id: replaced only when asked to all the same */
	have = dt_prog_find(id);
	if ( have != NULL ) {
		if ( have->builtin ) {
			return E_PAR;		/* the desktop's own is not replaced */
		}
		if ( !force ) {
			if ( p_prog != NULL ) *p_prog = have->uuid;
			return E_LIMIT;
		}
		had = TRUE;
		old = have->uuid;
	}

	dt_sys_prt(&prt);
	if ( has_base ) {
		er = plugin_base(&brought, &prt, &base);
		if ( er < E_OK ) {
			return er;
		}
	}
	m = (UB *)Kmalloc(1024 + OB_NAME_MAX * 2);
	if ( m == NULL ) {
		er = E_NOMEM;
	} else {
		n = plugin_meta(m, 1024 + OB_NAME_MAX * 2, dr.name, id, name, kind, ver,
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
	if ( er >= E_OK ) {
		er = ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_add(&box, &prog)
										     : E_SYS;
	}
	if ( er < E_OK ) {
		if ( has_base ) (void)ob_del_obj(&base);
		return er;
	}
	if ( had ) {
		(void)dt_prog_remove(&old);		/* the version it replaces, and its template */
	}
	read_box();					/* the registry sees it now */
	if ( p_prog != NULL ) *p_prog = prog;
	if ( p_how != NULL ) *p_how = had ? DT_HOW_UPDATED : DT_HOW_NEW;
	tm_printf((UB *)"dtprog: the plugin %s installed\n", id);
	return E_OK;
}

EXPORT void dt_prog_reread( void )
{
	read_box();
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
	UB	*meta, name[TAD_NAME_MAX];
	SZ	size = 0;
	INT	n, i, k = 0;
	T_OBREC	r[2];

	t[0] = 0;
	meta = om_obj_meta(&v->target, &size);
	if ( meta != NULL ) {
		if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
		  && js_get(&tf, "file", &f) >= E_OK && ( n = js_get_str(&f, "name", name, sizeof(name)) ) > 0 ) {
			for ( i = n - 1; i >= 0 && name[i] != '.'; i-- ) ;
			for ( i++; i > 0 && i < n && k < max - 1; i++ ) {
				t[k++] = ( name[i] >= 'A' && name[i] <= 'Z' ) ? (UB)( name[i] - 'A' + 'a' ) : name[i];
			}
			t[k] = 0;
		}
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
	(void)copy_s(out[i].id, DT_PROG_ID, id);
	(void)copy_s(out[i].raw, DT_PROG_ID, raw);
	(void)copy_s(out[i].name, TAD_NAME_MAX, name);
	out[i].listed = listed;
	(*n)++;
	return i;
}

EXPORT INT dt_app_list( CONST T_VOBJ *v, DTAPP *out, INT max, INT *p_dflt )
{
	UB		(*names)[OM_APP_NAME] = NULL, id[DT_PROG_ID], dflt[DT_PROG_ID], t[16];
	CONST DTPROG	*p;
	INT		na, i, n = 0;

	if ( p_dflt != NULL ) *p_dflt = -1;
	if ( v == NULL ) return 0;
	na = om_link_napps(v);
	if ( na < 0 ) na = 0;
	if ( out == NULL ) return na + nprog;

	/* the applist, each name as the system has the program */
	if ( na > 0 ) names = (UB (*)[OM_APP_NAME])Kmalloc(sizeof(UB) * OM_APP_NAME * (SZ)na);
	na = ( names != NULL ) ? om_link_apps(v, names, na) : 0;
	for ( i = 0; i < na; i++ ) {
		if ( om_link_app_id(v, i, id, DT_PROG_ID) <= 0 ) continue;
		p = dt_prog_find(id);
		(void)app_put(out, max, &n, ( p != NULL ) ? p->id : id, id, ( p != NULL ) ? p->name : names[i], TRUE);
	}
	if ( names != NULL ) Kfree(names);

	/* then what opens an object of its kind */
	if ( obj_type(v, t, sizeof(t)) ) {
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
 * A box's page: the 原紙箱 shows the templates of the applications,
 * each under its program's name; the 小物箱 the accessories themselves.
 */
EXPORT T_TAD *dt_prog_page( UINT kind )
{
	TS_UUID		*ids;
	CONST char	**names;
	T_TAD		*t;
	INT		i, k, n = 0;

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( nprog * DT_PROG_BASE + 1 ));
	names = (CONST char **)Kmalloc(sizeof(char *) * (SZ)( nprog * DT_PROG_BASE + 1 ));
	if ( ids == NULL || names == NULL ) {
		if ( ids != NULL ) Kfree(ids);
		if ( names != NULL ) Kfree((void *)names);
		return NULL;
	}
	for ( i = 0; i < nprog; i++ ) {
		if ( prog[i]->kind != kind ) continue;
		if ( kind == DT_PK_APP ) {
			for ( k = 0; k < prog[i]->nbase; k++ ) {
				ids[n] = prog[i]->base[k];
				names[n++] = (CONST char *)prog[i]->name;
			}
		} else {
			ids[n] = prog[i]->uuid;
			names[n++] = (CONST char *)prog[i]->name;
		}
	}
	t = dt_list_record(( kind == DT_PK_APP ) ? "原紙箱" : "小物箱", ids, names, n);
	Kfree(ids);
	Kfree((void *)names);
	return t;
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
	TS_UUID	arg;			/* the object to open, when has_arg */
	BOOL	has_arg;
	TS_UUID	reply;			/* where to answer; 0 for nobody */
	UINT	seq;
	SZ	anssz;			/* how much of a T_DTANS the asker reads */
	BOOL	with_er;		/* the answer says whether the start failed */
	UB	name[TAD_NAME_MAX];	/* the program, for the message line */
} DTSTART;

LOCAL ID	start_mbf = 0;
LOCAL ID	start_tsk = 0;

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

/* The process object made from the program, given the object to open (its UUID as the start-up argument) */
LOCAL ER start_now( CONST DTSTART *s, TS_UUID *pu )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = s->prog;
	if ( s->has_arg ) {
		c.arg = &s->arg;
		c.argsz = sizeof(s->arg);
	}
	return ob_cre_obj(&c, pu);
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
		UB	msg[TAD_NAME_MAX + 48];
		INT	n = put(msg, 0, sizeof(msg), (CONST char *)s->name);

		(void)put(msg, n, sizeof(msg), "を起動できませんでした");
		(void)wm_msg_put((CONST char *)msg);
		tm_printf((UB *)"TessronOS desktop: %s not started (%d)\n", s->name, (INT)er);
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

/* A process started from the program: handed to the starter, or started here when there is none */
LOCAL ER start_process( CONST DTPROG *p, CONST TS_UUID *arg )
{
	DTSTART	s;
	TS_UUID	pu;
	INT	i;
	ER	er;

	knl_memset(&s, 0, sizeof(s));
	s.prog = p->uuid;
	if ( arg != NULL ) {
		s.arg = *arg;
		s.has_arg = TRUE;
	}
	for ( i = 0; i < TAD_NAME_MAX - 1 && p->name[i] != 0; i++ ) {
		s.name[i] = p->name[i];
	}
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
			return E_OK;
		}
		if ( er != E_TMOUT ) {
			start_done(&s, er, NULL);
			return er;
		}
	}
	/* no starter, or it has more than it can hold: started here */
	knl_memset(&pu, 0, sizeof(pu));
	er = start_now(&s, &pu);
	start_done(&s, er, &pu);
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
	DTPROG		*p = (DTPROG *)Kmalloc(sizeof(DTPROG));
	CONST DTPROG	*con;
	TS_UUID		cu;
	BOOL		is;

	if ( p == NULL ) {
		return E_NOMEM;
	}
	is = (BOOL)( read_prog(target, p) && p->kind == DT_PK_COMMAND && !p->builtin );
	Kfree(p);
	if ( !is || ts_str_to_uuid(SYSDEF_PROG_CONSOLE, &cu) < E_OK || ( con = dt_prog_of(&cu) ) == NULL ) {
		return E_NOEXS;
	}
	return start_process(con, target);
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
	if ( !p->builtin ) {
		return start_process(p, ( v != NULL ) ? &v->target : NULL);
	}
	if ( same(p->id, "base-file-manager") || same(p->id, "accessory-box") ) {
		T_DPRECT	o;
		UINT		kind = same(p->id, "base-file-manager") ? DT_PK_APP : DT_PK_ACCESSORY;

		o.left = 120;  o.top = 90;  o.right = 520;  o.bottom = 420;
		return ( dt_open_made(dt_prog_page(kind), (CONST char *)p->name,
				      DT_TOOL_NONE, &o) != NULL ) ? E_OK : E_NOMEM;
	}
	if ( same(p->id, "trash-real-objects") ) {
		dt_trash_open();
		return E_OK;
	}
	if ( same(p->id, "window-list-viewer") ) {
		dt_winlist_open();
		return E_OK;
	}
	if ( same(p->id, "force-quit") ) {
		knl_fq_open();			/* 強制終了's own window (outer_kernel/wm/fquit.c) */
		return E_OK;
	}
	if ( same(p->id, "screen-print") ) {
		dt_scr_open();
		return E_OK;
	}
	if ( same(p->id, "tray-viewer") ) {
		dt_trayv_open();
		return E_OK;
	}
	if ( same(p->id, "disk-repair") ) {
		dt_fix_open();
		return E_OK;
	}
	if ( same(p->id, "system-install") ) {
		dt_sysinst_open();
		return E_OK;
	}
	if ( same(p->id, "real-object-search") ) {
		dt_search_open(from);
		return E_OK;
	}
	if ( same(p->id, "virtual-object-network") ) {
		if ( v == NULL ) return E_PAR;
		dt_network_open(from, v);
		return E_OK;
	}
	/* an editor: the object opened in a window of that kind */
	if ( v == NULL ) {
		return E_PAR;
	}
	return dt_open_as(from, v, p->id);
}
