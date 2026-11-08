/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtpkg.c
 *	Applications installed from a package, updated and deleted
 *	(design 18.12)
 *
 *	A package (.tpk, made by tools/mktpk.py) holds the program object
 *	and the templates it brings, each as the files of the TADjs form
 *	({uuid}.json, {uuid}_N.xtad, {uuid}_N.bin, {uuid}.ico), and
 *	"package.json", which names the program, its id and its version.
 *	Taken in from outside (ファイル変換), a package is an object whose
 *	record 1 is the file; that object is what is installed.
 *
 *	Installing puts each object of the package on the system under its
 *	own UUID -- made when the system does not have it, and its metadata
 *	and records written anew when it does, the records past the last
 *	the package brings deleted -- and links the program from the program
 *	box. The program is reached through that link and what its record 0
 *	links to through it; the templates are named only in its metadata,
 *	so each object no link reaches is held once, which keeps it out of
 *	the 屑実身 and from being collected. A program the system already
 *	has by the same id is updated only by a newer version, unless the
 *	caller says to go ahead all the same (the person was asked); one that
 *	came under another UUID is taken out of the box and deleted. What the
 *	version it replaces brought and the new one does not (its templates
 *	and what its record 0 linked to) is deleted as well. A version is
 *	written R1.000: R, a number of any length, a point and three digits,
 *	and compares as the number times a thousand and the digits.
 *
 *	Deleting takes a program out of the box and gives up the holds on what
 *	it brought, so that what nothing refers to any more is a 屑実身:
 *	屑実身操作 deletes it for good or brings it back, and a link to the
 *	program brought back and dropped on the list registers it again. The
 *	programs the desktop carries in itself (builtin) are neither replaced
 *	nor deleted; every other one, the system's programs that run as
 *	processes among them, a package replaces and the person deletes.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/json.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define PKG_HEAD	16
#define PKG_ENTRY	64
#define PKG_NAME	48
#define PKG_FORMAT	1

typedef struct {
	CONST UB	*name;		/* NUL filled to PKG_NAME */
	CONST UB	*data;
	UW		size;
	UW		rt;
} PKGENT;

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UH le16( CONST UB *p )
{
	return (UH)( p[0] | ( p[1] << 8 ) );
}

EXPORT D dt_ver_num( CONST UB *v )
{
	D	n = 0;
	INT	i = 1, k;

	if ( v == NULL || v[0] != 'R' || v[1] < '0' || v[1] > '9' ) {
		return -1;
	}
	for ( ; v[i] >= '0' && v[i] <= '9'; i++ ) {
		if ( n > 1000000000000LL ) return -1;
		n = n * 10 + ( v[i] - '0' );
	}
	if ( v[i] != '.' ) {
		return -1;
	}
	n *= 1000;
	for ( k = 0; k < 3; k++ ) {
		UB	c = v[i + 1 + k];

		if ( c < '0' || c > '9' ) return -1;
		n += ( c - '0' ) * ( k == 0 ? 100 : k == 1 ? 10 : 1 );
	}
	return ( v[i + 4] == 0 ) ? n : -1;
}

/* ---------------------------------------------------------------- reading a package */

/* The entries of a package, checked to lie within it: how many, or an error */
LOCAL INT pkg_entries( CONST UB *raw, SZ size, PKGENT *e, INT max )
{
	INT	n, i;

	if ( size < PKG_HEAD || raw[0] != 'T' || raw[1] != 'S' || raw[2] != 'P' || raw[3] != 'K'
	  || le16(raw + 4) != PKG_FORMAT ) {
		return E_OBJ;
	}
	n = le16(raw + 6);
	if ( n <= 0 || n > max || (SZ)PKG_HEAD + (SZ)n * PKG_ENTRY > size ) {
		return E_OBJ;
	}
	for ( i = 0; i < n; i++ ) {
		CONST UB	*d = raw + PKG_HEAD + i * PKG_ENTRY;
		UW		off = le32(d + PKG_NAME), sz = le32(d + PKG_NAME + 4);

		if ( d[PKG_NAME - 1] != 0 || (SZ)off > size || (SZ)sz > size - (SZ)off ) {
			return E_OBJ;
		}
		e[i].name = d;
		e[i].data = raw + off;
		e[i].size = sz;
		e[i].rt = le32(d + PKG_NAME + 8);
	}
	return n;
}

LOCAL BOOL name_is( CONST UB *name, CONST char *s )
{
	INT	i;

	for ( i = 0; s[i] != 0; i++ ) {
		if ( name[i] != (UB)s[i] ) return FALSE;
	}
	return (BOOL)( name[i] == 0 );
}

/*
 * What an entry's name says: the object (its UUID) and which of its
 * files it is -- the metadata (-1), the icon (-2), or record N.
 */
#define F_META	(-1)
#define F_ICON	(-2)
LOCAL BOOL entry_of( CONST UB *name, TS_UUID *u, INT *p_what )
{
	char	us[TS_UUID_STRLEN + 1];
	CONST UB *t = name + TS_UUID_STRLEN;
	INT	i, n = 0;

	for ( i = 0; i < TS_UUID_STRLEN; i++ ) {
		if ( name[i] == 0 ) return FALSE;
		us[i] = (char)name[i];
	}
	us[i] = 0;
	if ( ts_str_to_uuid(us, u) < E_OK ) {
		return FALSE;
	}
	if ( name_is(t, ".json") ) {
		*p_what = F_META;
		return TRUE;
	}
	if ( name_is(t, ".ico") ) {
		*p_what = F_ICON;
		return TRUE;
	}
	if ( t[0] != '_' || t[1] < '0' || t[1] > '9' ) {
		return FALSE;
	}
	for ( t++; *t >= '0' && *t <= '9'; t++ ) {
		n = n * 10 + ( *t - '0' );
		if ( n > 255 ) return FALSE;
	}
	if ( !name_is(t, ".xtad") && !name_is(t, ".bin") ) {
		return FALSE;
	}
	*p_what = n;
	return TRUE;
}

/* ---------------------------------------------------------------- one object put on */

/*
 * The records of an object the package wrote over that the package does
 * not bring: those past the last it brings, deleted from the last down
 * so that the numbers below stay as they are.
 */
LOCAL void drop_records( CONST TS_UUID *u, CONST PKGENT *e, INT n )
{
	T_OBREC	*rec;
	TS_UUID	fu;
	INT	nr, i, what, last = -1;
	ID	key;

	for ( i = 0; i < n; i++ ) {
		if ( entry_of(e[i].name, &fu, &what) && what > last && ts_uuid_cmp(&fu, u) == 0 ) {
			last = what;
		}
	}
	/* every record the object has, however many */
	nr = om_obj_records_all(u, &rec);
	if ( nr < 0 ) {
		return;
	}
	key = 0;
	for ( i = nr - 1; i >= 0; i-- ) {
		if ( rec[i].recno <= last ) {
			continue;
		}
		if ( key <= 0 ) {
			key = ob_opn_obj(u, OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);	/* records taken out together */
			if ( key <= 0 ) {
				break;
			}
		}
		if ( ob_del_rec(key, rec[i].recno) >= E_OK ) {
			tm_printf((UB *)"TessronOS desktop: record %d the new version does not have, deleted\n",
				  rec[i].recno);
		}
	}
	if ( key > 0 ) {
		ob_cls_obj(key);
	}
	Kfree(rec);
}

LOCAL ER put_object( CONST TS_UUID *u, CONST PKGENT *e, INT n, CONST T_OBPRT *prt )
{
	CONST PKGENT	*meta = NULL, *icon = NULL;
	TS_UUID		fu, got;
	T_OBREF		r;
	T_OBCRE		c;
	INT		i, what;
	ER		er = E_OK;

	for ( i = 0; i < n; i++ ) {
		if ( entry_of(e[i].name, &fu, &what) && ts_uuid_cmp(&fu, u) == 0 ) {
			if ( what == F_META ) meta = &e[i];
			if ( what == F_ICON ) icon = &e[i];
		}
	}
	if ( meta == NULL ) {
		return E_OBJ;
	}
	if ( ob_ref_obj(u, &r) >= E_OK ) {
		/* one the system has: its metadata and records made the package's */
		er = om_obj_meta_put(u, meta->data, (SZ)meta->size);
		if ( er >= E_OK && icon != NULL ) {
			er = om_obj_icon_put(u, icon->data, (SZ)icon->size);
		}
	} else {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = meta->data;
		c.jsonsz = (SZ)meta->size;
		c.prt = prt;
		c.uuid = *u;
		c.icon = ( icon != NULL ) ? icon->data : NULL;
		c.iconsz = ( icon != NULL ) ? (SZ)icon->size : 0;
		er = ob_cre_obj(&c, &got);
	}
	for ( i = 0; er >= E_OK && i < n; i++ ) {
		if ( entry_of(e[i].name, &fu, &what) && what >= 0 && ts_uuid_cmp(&fu, u) == 0 ) {
			er = om_obj_record_put(u, what, e[i].rt, 0, e[i].data, (SZ)e[i].size);
		}
	}
	if ( er >= E_OK ) {
		drop_records(u, e, n);
	}
	return er;
}

/* An object deleted, what kept it from deletion (削除不可) taken off first */
LOCAL ER del_object( CONST TS_UUID *u )
{
	T_OBPRT	*prt = knl_ob_prt_get(u);

	if ( prt != NULL && ( prt->attr & ( OB_A_PERM | OB_A_RONLY ) ) ) {
		prt->attr &= ~( OB_A_PERM | OB_A_RONLY );
		(void)ob_set_prt(u, prt);
	}
	if ( prt != NULL ) Kfree(prt);
	return ob_del_obj(u);
}

/* ---------------------------------------------------------------- installing */

/*
 * What a program brought beside itself, every one: its templates (the
 * metadata's base) and what its record 0 links to, in a table of their
 * own (*p_out, Kfree it). Answers how many.
 */
LOCAL INT brought( CONST TS_UUID *prog, CONST DTPROG *p, TS_UUID **p_out )
{
	UB	*rec;
	SZ	size = 0;
	INT	n = 0, i, nb = ( p != NULL ) ? p->nbase : 0, nl;
	TS_UUID	*out;

	rec = om_obj_record(prog, 0, &size);
	nl = ( rec != NULL ) ? om_store_links(rec, size, NULL, 0) : 0;
	out = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( nb + nl + 1 ));
	*p_out = out;
	if ( out == NULL ) {
		if ( rec != NULL ) Kfree(rec);
		return 0;
	}
	for ( i = 0; i < nb; i++ ) {
		out[n++] = p->base[i];
	}
	if ( rec != NULL ) {
		n += om_store_links(rec, size, out + n, nl);
		Kfree(rec);
	}
	return n;
}

/* The files a package holds, as its head says, or an error */
LOCAL INT pkg_count( CONST UB *raw, SZ size )
{
	if ( size < PKG_HEAD || raw[0] != 'T' || raw[1] != 'S' || raw[2] != 'P' || raw[3] != 'K'
	  || le16(raw + 4) != PKG_FORMAT ) {
		return E_OBJ;
	}
	return le16(raw + 6);
}

LOCAL BOOL among( CONST TS_UUID *u, CONST TS_UUID *set, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( ts_uuid_cmp(u, &set[i]) == 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Only an administrator adds a program. The password is asked for once the
 * version is known to go ahead, so that an install refused as not newer and
 * asked again with force asks only once.
 */
EXPORT ER dt_pkg_admin( ID pid, CONST TS_UUID *win )
{
	if ( knl_ob_pid_admin(pid) ) {
		return E_OK;
	}
	return dt_admin_ask(pid, (CONST UB *)"プログラムの追加", win);
}

LOCAL ER prog_adopt( CONST TS_UUID *u, CONST DTPROG *q, BOOL force, ID pid, CONST TS_UUID *win,
		     TS_UUID *p_prog, UINT *p_how );

EXPORT ER dt_pkg_install( CONST TS_UUID *pkg, BOOL force, ID pid, CONST TS_UUID *win,
			  TS_UUID *p_prog, UINT *p_how )
{
	PKGENT		*e = NULL;
	UB		*raw, id[DT_PROG_ID], ver[DT_VER_MAX], us[TS_UUID_STRLEN + 1];
	SZ		size = 0;
	INT		n, i, k, nobj = 0, room;
	TS_UUID		prog, *obj = NULL, box;
	T_JSON		root, list, it;
	T_OBPRT		prt;
	CONST DTPROG	*have;
	TS_UUID		old, *was = NULL;
	T_OBREF		wref;
	INT		nwas = 0, nwbase = 0;
	BOOL		had = FALSE, moved = FALSE;
	ER		er;

	if ( pkg == NULL ) {
		return E_PAR;
	}
	{
		/* a program object itself, as 屑実身操作 brought it back */
		DTPROG	*q = (DTPROG *)Kmalloc(sizeof(DTPROG));

		if ( q == NULL ) {
			return E_NOMEM;
		}
		if ( dt_prog_read(pkg, q) ) {
			er = prog_adopt(pkg, q, force, pid, win, p_prog, p_how);
			Kfree(q);
			return er;
		}
		Kfree(q);
	}
	raw = om_obj_record(pkg, 1, &size);
	if ( raw == NULL ) {
		/* no file of its own: the directory of a TADjs plugin, as ファイル変換 made it */
		return dt_plugin_install(pkg, force, pid, win, p_prog, p_how);
	}
	/* every file the package holds, as many as its head says (16 bits of them) */
	n = pkg_count(raw, size);
	e = ( n > 0 ) ? (PKGENT *)Kmalloc(sizeof(PKGENT) * (SZ)n) : NULL;
	n = ( e != NULL ) ? pkg_entries(raw, size, e, n) : ( n > 0 ) ? E_NOMEM : E_OBJ;
	er = ( n > 0 ) ? E_OBJ : (ER)n;

	/* package.json: the program, its id and version, and the objects */
	for ( i = 0; n > 0 && i < n; i++ ) {
		if ( !name_is(e[i].name, "package.json") ) continue;
		if ( js_parse(e[i].data, (INT)e[i].size, &root) < E_OK
		  || js_get_str(&root, "id", id, sizeof(id)) <= 0
		  || js_get_str(&root, "version", ver, sizeof(ver)) <= 0 || dt_ver_num(ver) < 0
		  || js_get_str(&root, "program", us, sizeof(us)) != TS_UUID_STRLEN
		  || ts_str_to_uuid((CONST char *)us, &prog) < E_OK ) {
			break;
		}
		/* the program and every object it brings */
		room = 1;
		if ( js_get(&root, "objects", &list) >= E_OK ) {
			for ( it.s = NULL; js_next(&list, &it); ) room++;
		}
		obj = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)room);
		if ( obj == NULL ) {
			er = E_NOMEM;
			break;
		}
		obj[nobj++] = prog;
		if ( js_get(&root, "objects", &list) >= E_OK ) {
			for ( it.s = NULL; nobj < room && js_next(&list, &it); ) {
				TS_UUID	u;

				if ( js_str(&it, us, sizeof(us)) != TS_UUID_STRLEN
				  || ts_str_to_uuid((CONST char *)us, &u) < E_OK ) continue;
				for ( k = 0; k < nobj && ts_uuid_cmp(&obj[k], &u) != 0; k++ ) ;
				if ( k == nobj ) obj[nobj++] = u;
			}
		}
		er = E_OK;
		break;
	}
	if ( er < E_OK ) {
		goto done;
	}

	/* the same program: only a newer version, unless asked to all the same */
	have = dt_prog_find(id);
	if ( have != NULL ) {
		had = TRUE;
		old = have->uuid;
		if ( have->builtin ) {
			er = E_PAR;		/* the desktop's own is not replaced */
			goto done;
		}
		if ( !force && dt_ver_num(ver) <= dt_ver_num(have->version) ) {
			er = E_LIMIT;
			goto done;
		}
		moved = (BOOL)( ts_uuid_cmp(&old, &prog) != 0 );
		nwas = brought(&old, have, &was);
		nwbase = have->nbase;
	}
	er = dt_pkg_admin(pid, win);
	if ( er < E_OK ) {
		goto done;
	}

	dt_sys_prt(&prt);
	prt.attr = 0;				/* what is installed can be deleted */
	for ( k = 0; er >= E_OK && k < nobj; k++ ) {
		er = put_object(&obj[k], e, n, &prt);
	}
	if ( er >= E_OK && ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) {
		if ( moved ) {
			(void)om_obj_link_del(&box, &old);
		}
		if ( !had || moved ) {
			er = om_obj_link_add(&box, &prog);
		}
	}
	/* what nothing links to, held, so that it is not taken for a 屑実身 */
	for ( k = 1; er >= E_OK && k < nobj; k++ ) {
		if ( om_store_refs(&obj[k]) == 0 ) {
			(void)ob_lnk_obj(&obj[k]);
		}
	}
	/*
	 * What the version replaced brought and this one does not: a template
	 * of its own that only the hold it was given keeps, or something its
	 * record 0 linked to that nothing links to any more. Something shared
	 * (a menu other programs link to too) stays.
	 */
	if ( er >= E_OK && had ) {
		for ( k = 0; k < nwas; k++ ) {
			if ( among(&was[k], obj, nobj) || ob_ref_obj(&was[k], &wref) < E_OK ) {
				continue;
			}
			if ( om_store_refs(&was[k]) <= ( ( k < nwbase ) ? 1 : 0 ) ) {
				(void)del_object(&was[k]);
			}
		}
		if ( moved ) {
			(void)del_object(&old);
		}
	}
	if ( er >= E_OK && had && dt_base_box(&box) ) {
		/* the templates of the version replaced that this one does not bring */
		for ( k = 0; k < nwbase && k < nwas; k++ ) {
			if ( !among(&was[k], obj, nobj) ) {
				(void)om_obj_link_del(&box, &was[k]);
			}
		}
	}
	dt_prog_reread();
	if ( er >= E_OK ) {
		dt_base_box_put(&prog, TRUE);	/* after the holds: the templates are not kept by these links */
		if ( p_prog != NULL ) *p_prog = prog;
		if ( p_how != NULL ) *p_how = had ? DT_PKG_UPDATED : DT_PKG_NEW;
		tm_printf((UB *)"TessronOS desktop: %s %s %s\n", id, ver, had ? "updated" : "installed");
	}
done:
	if ( e != NULL ) Kfree(e);
	if ( obj != NULL ) Kfree(obj);
	if ( was != NULL ) Kfree(was);
	Kfree(raw);
	return er;
}

/* ---------------------------------------------------------------- deleting */

/* What the program's metadata calls its own resources ("res"), in a table of their own: how many */
LOCAL INT prog_res( CONST TS_UUID *prog, TS_UUID **p_out )
{
	T_JSON	root, tf, g, list, it;
	UB	*meta, us[TS_UUID_STRLEN + 1];
	SZ	size = 0;
	INT	n = 0, room = 0;
	TS_UUID	*out = NULL;

	*p_out = NULL;
	meta = om_obj_meta(prog, &size);
	if ( meta == NULL ) {
		return 0;
	}
	if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "program", &g) >= E_OK && js_get(&g, "res", &list) >= E_OK ) {
		for ( it.s = NULL; js_next(&list, &it); ) room++;
		out = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( room + 1 ));
		for ( it.s = NULL; out != NULL && n < room && js_next(&list, &it); ) {
			if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
			  && ts_str_to_uuid((CONST char *)us, &out[n]) >= E_OK ) {
				n++;
			}
		}
	}
	Kfree(meta);
	*p_out = out;
	return n;
}

/* Whether a program of the box other than this one names the object as its template or resource */
LOCAL BOOL used_by_other( CONST TS_UUID *u, CONST TS_UUID *self )
{
	CONST DTPROG	*o;
	TS_UUID		*res;
	INT		i, n;
	BOOL		used = FALSE;

	for ( i = 0; !used && ( o = dt_prog_at(i) ) != NULL; i++ ) {
		if ( o->builtin || ts_uuid_cmp(&o->uuid, self) == 0 ) continue;
		used = among(u, o->base, o->nbase);
		n = prog_res(&o->uuid, &res);
		used = (BOOL)( used || among(u, res, n) );
		if ( res != NULL ) Kfree(res);
	}
	return used;
}

/* What keeps an object from being deleted taken off, so that 屑実身操作 can */
LOCAL void perm_off( CONST TS_UUID *u )
{
	T_OBPRT	*prt = knl_ob_prt_get(u);

	if ( prt != NULL && ( prt->attr & OB_A_PERM ) != 0 ) {
		prt->attr &= ~OB_A_PERM;
		(void)ob_set_prt(u, prt);
	}
	if ( prt != NULL ) Kfree(prt);
}

/*
 * The holds on what the program brought, given up or taken again. Its
 * templates and resources are held because nothing links to them; what
 * its record 0 links to is kept by that link, and goes when the program
 * is deleted for good.
 */
LOCAL void release_brought( CONST TS_UUID *prog, CONST DTPROG *p, BOOL release )
{
	TS_UUID	*was, *res;
	INT	nwas = brought(prog, p, &was), nres = prog_res(prog, &res), i;
	INT	nb = p->nbase;

	for ( i = 0; i < nwas + nres; i++ ) {
		CONST TS_UUID	*u = ( i < nwas ) ? &was[i] : &res[i - nwas];
		INT		refs;
		BOOL		linked = (BOOL)( i >= nb && i < nwas );

		if ( !linked && i >= nwas && among(u, was, nwas) ) {
			continue;		/* a resource already seen as a template or a link */
		}
		refs = om_store_refs(u);
		if ( refs < 0 ) {
			continue;
		}
		if ( !release ) {
			if ( refs == 0 && !linked ) (void)ob_lnk_obj(u);
			continue;
		}
		if ( used_by_other(u, prog) ) {
			continue;
		}
		if ( !linked && refs == 1 ) {
			(void)ob_unl_obj(u);
			refs = 0;
		}
		if ( refs <= 1 ) {
			perm_off(u);
		}
	}
	if ( was != NULL ) Kfree(was);
	if ( res != NULL ) Kfree(res);
}

/*
 * The person's 削除: out of the box, and what it comes to is a 屑実身.
 * Nothing is deleted here. An accessory's line leaves 小物の設定.
 */
EXPORT ER dt_prog_remove( CONST TS_UUID *prog )
{
	CONST DTPROG	*p = dt_prog_of(prog);
	TS_UUID		box;
	ER		er;

	if ( p == NULL ) {
		return E_NOEXS;
	}
	if ( p->builtin ) {
		return E_PAR;
	}
	perm_off(prog);
	er = ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_del(&box, prog) : E_SYS;
	if ( er >= E_OK ) {
		dt_base_box_put(prog, FALSE);	/* before the holds are counted */
		release_brought(prog, p, TRUE);
	}
	dt_prog_reread();
	dt_acc_sync();
	dt_tool_refresh(DT_TOOL_TRASH);
	return er;
}

/*
 * A version replaced by another under a new UUID: it is not the person's
 * to bring back, so it goes for good with its templates and what only
 * its record 0 linked to.
 */
EXPORT ER dt_prog_drop( CONST TS_UUID *prog )
{
	CONST DTPROG	*p = dt_prog_of(prog);
	TS_UUID		box, *was;
	INT		nbase, nwas, i;
	ER		er;

	if ( p == NULL ) {
		return E_NOEXS;
	}
	if ( p->builtin ) {
		return E_PAR;
	}
	nbase = p->nbase;
	nwas = brought(prog, p, &was);
	dt_base_box_put(prog, FALSE);
	er = ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_del(&box, prog) : E_SYS;
	if ( er >= E_OK ) {
		er = del_object(prog);
		/* its templates; what its record 0 linked to once nothing else does */
		for ( i = 0; i < nwas; i++ ) {
			if ( i < nbase || om_store_refs(&was[i]) <= 0 ) {
				(void)del_object(&was[i]);
			}
		}
	}
	if ( was != NULL ) Kfree(was);
	dt_prog_reread();
	return er;
}

/*
 * A program object dropped on the list, as 屑実身操作 brought it back:
 * registered again as it is. By the same object, it is there already
 * (E_LIMIT, nothing to do when asked to go ahead); a different program
 * has its id: refused, as it holds a version of its own.
 */
LOCAL ER prog_adopt( CONST TS_UUID *u, CONST DTPROG *q, BOOL force, ID pid, CONST TS_UUID *win,
		     TS_UUID *p_prog, UINT *p_how )
{
	CONST DTPROG	*have = dt_prog_find(q->id);
	TS_UUID		box;
	ER		er;

	if ( q->builtin ) {
		return E_PAR;
	}
	if ( have != NULL ) {
		if ( p_prog != NULL ) *p_prog = have->uuid;
		if ( have->builtin ) return E_PAR;
		if ( !force ) return E_LIMIT;
		if ( ts_uuid_cmp(&have->uuid, u) != 0 ) return E_OBJ;
		if ( p_how != NULL ) *p_how = DT_PKG_UPDATED;
		return E_OK;
	}
	er = dt_pkg_admin(pid, win);
	if ( er < E_OK ) {
		return er;
	}
	er = ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_add(&box, u) : E_SYS;
	if ( er < E_OK ) {
		return er;
	}
	release_brought(u, q, FALSE);
	dt_base_box_put(u, TRUE);
	dt_prog_reread();
	dt_tool_refresh(DT_TOOL_TRASH);
	if ( p_prog != NULL ) *p_prog = *u;
	if ( p_how != NULL ) *p_how = DT_PKG_NEW;
	tm_printf((UB *)"TessronOS desktop: %s registered again\n", q->id);
	return E_OK;
}
